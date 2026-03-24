"""
Helios Test Bench — Pi-Side Relay Server
=========================================
Simulates the Raspberry Pi's role in the Helios pipeline:

  ESP32 payload (image + audio)
    -> Cartesia Ink-Whisper STT  (audio -> transcript)
    -> Gemini via OpenRouter     (transcript + image -> response text)
    -> Cartesia Sonic 3 TTS      (response text -> PCM audio)
  <- raw PCM audio back to ESP32

Run:  python server.py
Test: curl -X POST http://localhost:5750/health
"""

import os
import json
import base64
import asyncio
import logging
import time
from contextlib import asynccontextmanager

from fastapi import FastAPI, Request
from fastapi.responses import Response, JSONResponse
import httpx
import websockets
from dotenv import load_dotenv

load_dotenv()

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
CARTESIA_API_KEY = os.getenv("CARTESIA_API_KEY", "")
OPENROUTER_API_KEY = os.getenv("OPENROUTER_API_KEY", "")

STT_MODEL = "ink-whisper"
STT_SAMPLE_RATE = 16000
STT_ENCODING = "pcm_s16le"

TTS_MODEL = "sonic-3-2026-01-12"
TTS_VOICE_ID = os.getenv("TTS_VOICE_ID", "f786b574-daa5-4673-aa0c-cbe3e8534c02")
TTS_SAMPLE_RATE = 16000
TTS_ENCODING = "pcm_s16le"
CARTESIA_VERSION = "2026-01-12"

GEMINI_MODEL = "google/gemini-3.1-flash-lite-preview:nitro"
GEMINI_MAX_TOKENS = 300  # Keep responses short for spoken output

# Conversation history — ephemeral context window
CONVERSATION_TIMEOUT = int(os.getenv("CONVERSATION_TIMEOUT", "300"))  # 5 min default

# System prompt for Gemini — tune this for your use case
GEMINI_SYSTEM_PROMPT = """You are an assistive AI embedded in a wearable device for a vision-impaired user.
You receive an image from the user's camera and their spoken question.
Respond concisely and helpfully — your response will be spoken aloud via TTS.
Keep answers to 1-3 sentences. Be direct and descriptive about what you see."""

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("helios")

# ---------------------------------------------------------------------------
# Conversation state (ephemeral history with auto-expiry)
# ---------------------------------------------------------------------------
conversation = {
    "history": [],       # list of {"role": "user"/"assistant", "content": str}
    "last_query_time": 0.0,
}


def check_conversation_timeout():
    """Clear conversation if it has expired."""
    if conversation["history"] and time.time() - conversation["last_query_time"] > CONVERSATION_TIMEOUT:
        turn_count = len([m for m in conversation["history"] if m["role"] == "user"])
        log.info(f"[CONV] Expired ({turn_count} turns, {CONVERSATION_TIMEOUT}s timeout) — clearing")
        conversation["history"] = []


# ---------------------------------------------------------------------------
# Step 1: Cartesia Ink-Whisper STT
# ---------------------------------------------------------------------------
async def stt_transcribe(audio_pcm: bytes) -> str:
    """Send raw PCM audio to Cartesia Ink-Whisper via WebSocket, return transcript."""
    if not audio_pcm:
        return ""

    log.info(f"[STT] Sending {len(audio_pcm):,} bytes ({len(audio_pcm)/2/STT_SAMPLE_RATE:.1f}s of audio)")
    t0 = time.time()

    # WebSocket URL with config as query params, auth via header
    uri = (
        f"wss://api.cartesia.ai/stt/websocket"
        f"?model={STT_MODEL}"
        f"&encoding={STT_ENCODING}"
        f"&sample_rate={STT_SAMPLE_RATE}"
        f"&language=en"
    )
    headers = {
        "X-API-Key": CARTESIA_API_KEY,
        "Cartesia-Version": CARTESIA_VERSION,
    }

    transcript_parts = []

    try:
        async with websockets.connect(uri, additional_headers=headers) as ws:
            # Send audio in chunks (8KB ~ 0.25s of 16kHz s16le mono)
            chunk_size = 8192
            for i in range(0, len(audio_pcm), chunk_size):
                await ws.send(audio_pcm[i : i + chunk_size])

            # Signal we're done sending audio (plain text command, not JSON)
            await ws.send("done")

            # Collect transcription results
            async for msg in ws:
                if isinstance(msg, str):
                    data = json.loads(msg)
                    msg_type = data.get("type", "")

                    if msg_type == "transcript":
                        text = data.get("text", "")
                        is_final = data.get("is_final", False)
                        log.info(f"[STT] {'FINAL' if is_final else 'interim'}: \"{text}\"")
                        if is_final and text.strip():
                            transcript_parts.append(text.strip())

                    elif msg_type in ("done", "flush_done"):
                        break

                    elif msg_type == "error":
                        log.error(f"[STT] Error from Cartesia: {data}")
                        break
                else:
                    # Binary message — unexpected, log it
                    log.warning(f"[STT] Unexpected binary message: {len(msg)} bytes")

    except Exception as e:
        log.error(f"[STT] WebSocket error: {e}")
        log.error("[STT] Check that CARTESIA_API_KEY is valid and the WebSocket URL is correct")
        return ""

    transcript = " ".join(transcript_parts)
    elapsed = time.time() - t0
    log.info(f"[STT] Done in {elapsed:.2f}s — \"{transcript}\"")
    return transcript


# ---------------------------------------------------------------------------
# Step 2: Gemini Vision via OpenRouter
# ---------------------------------------------------------------------------
async def vision_query(transcript: str, image_b64: str, history: list) -> str:
    """Send transcript + base64 JPEG to Gemini with conversation history."""
    log.info(f"[LLM] Sending to {GEMINI_MODEL}: \"{transcript[:80]}\" ({len(history)} history msgs)")
    t0 = time.time()

    messages = []

    # System prompt
    if GEMINI_SYSTEM_PROMPT.strip():
        messages.append({
            "role": "system",
            "content": GEMINI_SYSTEM_PROMPT.strip(),
        })

    # Prior conversation turns (text-only — no old images)
    messages.extend(history)

    # Current user message with text + image (text first for optimal parsing)
    messages.append({
        "role": "user",
        "content": [
            {
                "type": "text",
                "text": transcript,
            },
            {
                "type": "image_url",
                "image_url": {
                    "url": f"data:image/jpeg;base64,{image_b64}",
                },
            },
        ],
    })

    async with httpx.AsyncClient(timeout=30.0) as client:
        resp = await client.post(
            "https://openrouter.ai/api/v1/chat/completions",
            headers={
                "Authorization": f"Bearer {OPENROUTER_API_KEY}",
                "Content-Type": "application/json",
            },
            json={
                "model": GEMINI_MODEL,
                "messages": messages,
                "max_tokens": GEMINI_MAX_TOKENS,
            },
        )

        if resp.status_code != 200:
            log.error(f"[LLM] OpenRouter error {resp.status_code}: {resp.text[:300]}")
            return "Sorry, I couldn't process that request."

        data = resp.json()

    result = data["choices"][0]["message"]["content"]
    elapsed = time.time() - t0
    log.info(f"[LLM] Done in {elapsed:.2f}s — \"{result[:100]}...\"")
    return result


# ---------------------------------------------------------------------------
# Step 3: Cartesia Sonic 3 TTS
# ---------------------------------------------------------------------------
async def tts_synthesize(text: str) -> bytes:
    """Send text to Cartesia Sonic 3 TTS, return raw PCM bytes."""
    log.info(f"[TTS] Synthesizing {len(text)} chars")
    t0 = time.time()

    async with httpx.AsyncClient(timeout=30.0) as client:
        resp = await client.post(
            "https://api.cartesia.ai/tts/bytes",
            headers={
                "Authorization": f"Bearer {CARTESIA_API_KEY}",
                "Cartesia-Version": CARTESIA_VERSION,
                "Content-Type": "application/json",
            },
            json={
                "model_id": TTS_MODEL,
                "transcript": text,
                "voice": {
                    "mode": "id",
                    "id": TTS_VOICE_ID,
                },
                "output_format": {
                    "container": "raw",
                    "encoding": TTS_ENCODING,
                    "sample_rate": TTS_SAMPLE_RATE,
                },
                "language": "en",
            },
        )

        if resp.status_code != 200:
            log.error(f"[TTS] Cartesia error {resp.status_code}: {resp.text[:300]}")
            return b""

    pcm_bytes = resp.content
    duration = len(pcm_bytes) / 2 / TTS_SAMPLE_RATE
    elapsed = time.time() - t0
    log.info(f"[TTS] Done in {elapsed:.2f}s — {len(pcm_bytes):,} bytes ({duration:.1f}s audio)")
    return pcm_bytes


# ---------------------------------------------------------------------------
# FastAPI App
# ---------------------------------------------------------------------------
app = FastAPI(title="Helios Relay Server")


@app.get("/health")
async def health():
    """Quick check that the server is running and keys are configured."""
    return {
        "status": "ok",
        "cartesia_key": "set" if CARTESIA_API_KEY else "MISSING",
        "openrouter_key": "set" if OPENROUTER_API_KEY else "MISSING",
        "stt_model": STT_MODEL,
        "llm_model": GEMINI_MODEL,
        "tts_model": TTS_MODEL,
        "tts_voice": TTS_VOICE_ID,
    }


@app.post("/clear")
async def clear_conversation_endpoint():
    """Manually clear conversation history (double-spacebar from client)."""
    turn_count = len([m for m in conversation["history"] if m["role"] == "user"])
    conversation["history"] = []
    conversation["last_query_time"] = 0.0
    log.info(f"[CONV] Manually cleared ({turn_count} turns)")
    return {"status": "cleared", "turns_cleared": turn_count}


@app.post("/query")
async def handle_query(request: Request):
    """
    Main endpoint — mirrors what the ESP32 will POST to the Pi.

    Request JSON:
        image:      base64-encoded JPEG
        audio:      base64-encoded PCM s16le 16kHz mono (optional if transcript given)
        transcript: text query, bypasses STT (for test harness)
        timestamp:  unix timestamp (optional)

    Response:
        Body: raw PCM s16le audio bytes
        Headers:
            X-Transcript:          what the user said (STT result)
            X-Response-Text:       what the AI said (LLM result)
            X-Sample-Rate:         PCM sample rate
            X-Encoding:            PCM encoding
            X-Total-Ms:            total processing time
            X-Conversation-Turns:  number of turns in current conversation
    """
    total_t0 = time.time()
    body = await request.json()

    image_b64 = body.get("image", "")
    audio_b64 = body.get("audio", "")

    log.info(f"{'='*60}")
    log.info(f"[QUERY] Received: image={len(image_b64)//1024}KB  audio={len(audio_b64)//1024}KB")

    # --- Check conversation timeout ---
    check_conversation_timeout()

    # --- Step 1: STT (skip if transcript provided) ---
    transcript = body.get("transcript", "")
    if transcript:
        log.info(f"[QUERY] Using provided transcript: \"{transcript}\"")
    else:
        audio_pcm = base64.b64decode(audio_b64) if audio_b64 else b""
        transcript = await stt_transcribe(audio_pcm)

    if not transcript.strip():
        transcript = "What do you see in this image?"
        log.warning(f"[QUERY] Empty transcript, using fallback: \"{transcript}\"")

    # --- Step 2: Vision LLM (with conversation history) ---
    response_text = await vision_query(transcript, image_b64, conversation["history"])

    # --- Update conversation history ---
    conversation["history"].append({"role": "user", "content": transcript})
    conversation["history"].append({"role": "assistant", "content": response_text})
    conversation["last_query_time"] = time.time()
    turn_count = len([m for m in conversation["history"] if m["role"] == "user"])
    log.info(f"[CONV] Turn {turn_count} recorded (timeout in {CONVERSATION_TIMEOUT}s)")

    # --- Step 3: TTS ---
    tts_audio = await tts_synthesize(response_text)

    total_ms = (time.time() - total_t0) * 1000
    log.info(f"[QUERY] Total pipeline: {total_ms:.0f}ms")
    log.info(f"{'='*60}")

    return Response(
        content=tts_audio,
        media_type="application/octet-stream",
        headers={
            "X-Transcript": transcript[:500],
            "X-Response-Text": response_text[:500],
            "X-Sample-Rate": str(TTS_SAMPLE_RATE),
            "X-Encoding": TTS_ENCODING,
            "X-Total-Ms": str(int(total_ms)),
            "X-Conversation-Turns": str(turn_count),
        },
    )


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn

    if not CARTESIA_API_KEY:
        log.error("CARTESIA_API_KEY not set! Copy .env.example to .env and add your key.")
    if not OPENROUTER_API_KEY:
        log.error("OPENROUTER_API_KEY not set! Copy .env.example to .env and add your key.")

    log.info(f"Starting Helios relay server on port 5750")
    log.info(f"  STT:  {STT_MODEL} @ {STT_SAMPLE_RATE}Hz {STT_ENCODING}")
    log.info(f"  LLM:  {GEMINI_MODEL}")
    log.info(f"  TTS:  {TTS_MODEL} -> {TTS_ENCODING} @ {TTS_SAMPLE_RATE}Hz")
    log.info(f"  Voice: {TTS_VOICE_ID}")

    uvicorn.run(app, host="0.0.0.0", port=5750)
