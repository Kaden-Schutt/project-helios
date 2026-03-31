"""
Helios Voice Assistant Relay — No Camera
=========================================
USB serial bridge: ESP32 sends PCM audio, this relays through
STT → LLM (text-only) → TTS pipeline, sends TTS audio back.

Run:  python server_nocam.py
"""

import os
import sys
import glob
import base64
import time
import asyncio
import json
import serial
import numpy as np
import sounddevice as sd

import httpx
import websockets
from dotenv import load_dotenv

load_dotenv()

CARTESIA_API_KEY = os.getenv("CARTESIA_API_KEY", "")
OPENROUTER_API_KEY = os.getenv("OPENROUTER_API_KEY", "")

STT_MODEL = "ink-whisper"
STT_SAMPLE_RATE = 16000

TTS_MODEL = "sonic-3-2026-01-12"
TTS_VOICE_ID = os.getenv("TTS_VOICE_ID", "f786b574-daa5-4673-aa0c-cbe3e8534c02")
TTS_SAMPLE_RATE = 16000
CARTESIA_VERSION = "2026-01-12"

LLM_MODEL = "google/gemini-3.1-flash-lite-preview:nitro"
LLM_MAX_TOKENS = 300

SYSTEM_PROMPT = """You are a helpful AI voice assistant embedded in a wearable device.
You receive spoken questions and respond conversationally.
Keep answers to 1-3 sentences. Be direct, friendly, and concise — your response will be spoken aloud."""

# Conversation history
conversation = {"history": [], "last_time": 0.0}
CONVERSATION_TIMEOUT = 300


def find_esp32():
    matches = glob.glob("/dev/cu.usbmodem*")
    return matches[0] if matches else None


async def stt_transcribe(audio_pcm):
    if not audio_pcm:
        return ""

    duration = len(audio_pcm) / 2 / STT_SAMPLE_RATE
    print(f"  [STT] Sending {len(audio_pcm):,} bytes ({duration:.1f}s)")
    t0 = time.time()

    uri = (
        f"wss://api.cartesia.ai/stt/websocket"
        f"?model={STT_MODEL}"
        f"&encoding=pcm_s16le"
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
            for i in range(0, len(audio_pcm), 8192):
                await ws.send(audio_pcm[i:i + 8192])
            await ws.send("done")

            async for msg in ws:
                if isinstance(msg, str):
                    data = json.loads(msg)
                    if data.get("type") == "transcript" and data.get("is_final"):
                        text = data.get("text", "").strip()
                        if text:
                            transcript_parts.append(text)
                    elif data.get("type") in ("done", "flush_done"):
                        break
    except Exception as e:
        print(f"  [STT] Error: {e}")
        return ""

    transcript = " ".join(transcript_parts)
    print(f"  [STT] {time.time() - t0:.1f}s — \"{transcript}\"")
    return transcript


async def llm_query(transcript, history):
    print(f"  [LLM] \"{transcript[:80]}\"")
    t0 = time.time()

    messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    messages.extend(history)
    messages.append({"role": "user", "content": transcript})

    async with httpx.AsyncClient(timeout=30.0) as client:
        resp = await client.post(
            "https://openrouter.ai/api/v1/chat/completions",
            headers={
                "Authorization": f"Bearer {OPENROUTER_API_KEY}",
                "Content-Type": "application/json",
            },
            json={
                "model": LLM_MODEL,
                "messages": messages,
                "max_tokens": LLM_MAX_TOKENS,
            },
        )
        if resp.status_code != 200:
            print(f"  [LLM] Error {resp.status_code}: {resp.text[:200]}")
            return "Sorry, I couldn't process that."

        result = resp.json()["choices"][0]["message"]["content"]

    print(f"  [LLM] {time.time() - t0:.1f}s — \"{result[:100]}\"")
    return result


async def tts_synthesize(text):
    print(f"  [TTS] Synthesizing {len(text)} chars")
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
                "voice": {"mode": "id", "id": TTS_VOICE_ID},
                "output_format": {
                    "container": "raw",
                    "encoding": "pcm_s16le",
                    "sample_rate": TTS_SAMPLE_RATE,
                },
                "language": "en",
            },
        )
        if resp.status_code != 200:
            print(f"  [TTS] Error {resp.status_code}: {resp.text[:200]}")
            return b""

    duration = len(resp.content) / 2 / TTS_SAMPLE_RATE
    print(f"  [TTS] {time.time() - t0:.1f}s — {len(resp.content):,} bytes ({duration:.1f}s)")
    return resp.content


async def process_query(pcm_data):
    """Run the full STT → LLM → TTS pipeline."""
    total_t0 = time.time()

    # Check conversation timeout
    if conversation["history"] and time.time() - conversation["last_time"] > CONVERSATION_TIMEOUT:
        print("  [CONV] Expired — clearing")
        conversation["history"] = []

    # STT
    transcript = await stt_transcribe(pcm_data)
    if not transcript.strip():
        transcript = "Hello"
        print(f"  [STT] Empty, using fallback: \"{transcript}\"")

    # LLM
    response_text = await llm_query(transcript, conversation["history"])

    # Update history
    conversation["history"].append({"role": "user", "content": transcript})
    conversation["history"].append({"role": "assistant", "content": response_text})
    conversation["last_time"] = time.time()

    # TTS
    tts_audio = await tts_synthesize(response_text)

    total = time.time() - total_t0
    print(f"  [TOTAL] {total:.1f}s pipeline")
    return tts_audio


def send_tts_to_esp(ser, pcm_bytes):
    """ESP-pull protocol: ESP requests chunks, we respond."""
    if not pcm_bytes:
        ser.write(b"HELIOS_TTS_SIZE:0\n")
        ser.flush()
        return

    duration = len(pcm_bytes) / 2 / TTS_SAMPLE_RATE
    print(f"  [TX] TTS ready: {len(pcm_bytes):,} bytes ({duration:.1f}s), waiting for ESP to pull...")

    # Send size header
    ser.write(f"HELIOS_TTS_SIZE:{len(pcm_bytes)}\n".encode())
    ser.flush()

    # Serve pull requests until ESP says done
    chunks_served = 0
    deadline = time.time() + 60  # 60s total timeout

    while time.time() < deadline:
        r = ser.readline()
        if not r:
            continue

        line = r.rstrip(b"\r\n")

        if b"HELIOS_TTS_DONE" in line:
            break

        if line.startswith(b"HELIOS_PULL:"):
            parts = line.decode().split(":")
            if len(parts) >= 3:
                offset = int(parts[1])
                size = int(parts[2])
                chunk = pcm_bytes[offset:offset + size]
                b64 = base64.b64encode(chunk).decode()
                ser.write(f"HELIOS_DATA:{b64}\n".encode())
                ser.flush()
                chunks_served += 1
        else:
            # Log ESP debug output
            text = line.decode("utf-8", errors="replace")
            if text.strip():
                print(f"  [ESP32] {text}")

    print(f"  [TX] Served {chunks_served} chunks.")


def main():
    print()
    print("  ╔══════════════════════════════════════╗")
    print("  ║  HELIOS Voice Relay (No Camera)      ║")
    print("  ╚══════════════════════════════════════╝")
    print()

    if not CARTESIA_API_KEY:
        print("  ERROR: CARTESIA_API_KEY not set in .env")
    if not OPENROUTER_API_KEY:
        print("  ERROR: OPENROUTER_API_KEY not set in .env")

    port = find_esp32()
    if not port:
        print("  No ESP32 found. Plug it in and try again.")
        sys.exit(1)

    print(f"  Port:    {port}")
    print(f"  STT:     {STT_MODEL}")
    print(f"  LLM:     {LLM_MODEL}")
    print(f"  TTS:     {TTS_MODEL}")
    print(f"  Voice:   {TTS_VOICE_ID}")
    print()

    ser = serial.Serial(port, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
    ser.setDTR(False)
    ser.setRTS(False)
    ser.reset_input_buffer()

    pcm_data = None

    print("  Waiting for ESP32 button press...\n")

    try:
        while True:
            raw = ser.readline()
            if not raw:
                continue

            line = raw.rstrip(b"\r\n")

            if line.startswith(b"HELIOS_PCM:"):
                b64_str = line[len(b"HELIOS_PCM:"):]
                # Keep reading if line isn't complete
                while not raw.endswith(b"\n"):
                    more = ser.read(4096)
                    if more:
                        raw += more
                        b64_str += more.rstrip(b"\r\n")
                    else:
                        break
                try:
                    pcm_data = base64.b64decode(b64_str)
                    duration = len(pcm_data) / 2 / STT_SAMPLE_RATE
                    print(f"  Received PCM: {len(pcm_data):,} bytes ({duration:.1f}s)")
                except Exception as e:
                    print(f"  ERROR decoding PCM: {e}")
                    pcm_data = None

            elif line == b"HELIOS_END":
                if pcm_data:
                    print(f"\n  ┌─ Processing query...")
                    tts_audio = asyncio.run(process_query(pcm_data))
                    print(f"  └─ Done.\n")
                    pcm_data = None

                    # Wait for HELIOS_READY then send TTS binary to ESP
                    deadline = time.time() + 5
                    while time.time() < deadline:
                        r = ser.readline()
                        if r and b"HELIOS_READY" in r:
                            send_tts_to_esp(ser, tts_audio)
                            break
                    else:
                        print("  WARN: ESP32 didn't send HELIOS_READY")
                else:
                    print("  No audio captured.")

                print("  Waiting for next button press...\n")

            else:
                text = line.decode("utf-8", errors="replace")
                if text.strip():
                    print(f"  [ESP32] {text}")

    except KeyboardInterrupt:
        print("\n\n  Bye!")
    except serial.SerialException:
        print("\n  ESP32 disconnected.")
    finally:
        ser.close()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n  Bye!\n")
