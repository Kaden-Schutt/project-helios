"""
Helios Test Bench — Pi-Side Relay Server
=========================================
Simulates the Raspberry Pi's role in the Helios pipeline:

  ESP32 payload (image + audio)
    -> Cartesia Ink-Whisper STT  (audio -> transcript)
    -> Claude Haiku (Anthropic)  (transcript + image -> response text)
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
import re
import time
from contextlib import asynccontextmanager

import io
import uuid
import wave

import numpy as np

from fastapi import FastAPI, Request, WebSocket
from fastapi.responses import Response, JSONResponse, StreamingResponse
from starlette.websockets import WebSocketDisconnect
import httpx
import websockets
import anthropic
from PIL import Image
from dotenv import load_dotenv

load_dotenv()

import settings       # local module — see settings.py

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
CARTESIA_API_KEY = os.getenv("CARTESIA_API_KEY", "")
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY", "")

STT_MODEL = "ink-whisper"
STT_SAMPLE_RATE = 16000
STT_ENCODING = "pcm_s16le"

TTS_MODEL = "sonic-3-2026-01-12"
# Voice defaults come from settings.json; env var still wins if set (legacy override).
def _tts_voice_id() -> str:
    return os.getenv("TTS_VOICE_ID") or settings.get("voice_id")
TTS_SAMPLE_RATE = 24000
TTS_ENCODING = "pcm_s16le"
CARTESIA_VERSION = "2026-01-12"

LLM_MODEL = "claude-haiku-4-5-20251001"
LLM_MAX_TOKENS = 512  # Short spoken replies only

SETTINGS_MODEL = "claude-opus-4-7"
SETTINGS_MAX_TOKENS = 1024

_VERBOSITY_RULE = {
    "brief":    "Respond in 1-2 sentences MAX. Never more.",
    "normal":   "Respond in 2-3 sentences MAX. Never more.",
    "detailed": "Respond in up to 5 sentences. Stay TTS-friendly and concrete.",
}

def _llm_system_prompt() -> str:
    rule = _VERBOSITY_RULE.get(settings.get("verbosity"), _VERBOSITY_RULE["brief"])
    return f"""You are an assistive AI in a wearable for a vision-impaired user. Your response is spoken aloud via TTS, so it MUST be short and immediately useful.

STRICT RULES:
- {rule}
- Use the image to answer concretely with locations/directions. The user cannot see.
- Never start with "I can see..." or "It looks like..." — just answer.
- If the user asks where something is, name where in the image it appears (e.g. "on the coffee table, left of the remote").

Examples:
User: "Where are my keys?"
You:  "Your keys are on the wooden desk, next to the black laptop."

User: "What's in front of me?"
You:  "A kitchen counter with a coffee mug and a loaf of bread. The sink is to your right."

User: "Can I cross?"
You:  "Yes. The pedestrian signal is white and the road is clear."

If the image doesn't show what the user asked about, say so in one sentence."""


SETTINGS_SYSTEM_PROMPT = """You are in SETTINGS MODE for Helios, a wearable assistive device. The user is vision-impaired and interacts by voice.

RULES:
1. Every user request MUST result in a tool call. Do not describe what you'd do — actually call update_setting, get_setting, or list_settings.
2. If unsure which setting the user means, call list_settings first.
3. Confirm changes tersely: "Volume set to 50." — not "I have successfully updated the volume setting to 50 percent." Your confirmation is spoken aloud.
4. If the user asks a non-settings question (e.g. "what's in front of me"), call exit_settings, then reply "okay" and stop. They will re-ask.
5. If the user says "done", "exit", "never mind", "that's all", "stop", or "quit" — call exit_settings and say "okay".
6. Never invent setting names. Use list_settings when in doubt.

Saved scope persists across reboots. Session scope reverts on restart. Default to saved unless the user explicitly says "just for now" / "temporarily"."""


SETTINGS_TOOLS = [
    {
        "name": "update_setting",
        "description": "Change a Helios setting. `value` should match the setting's type (bool, int, string). Use scope='session' for temporary changes, 'saved' for persistent (default).",
        "input_schema": {
            "type": "object",
            "properties": {
                "key":   {"type": "string", "description": "Setting key. Call list_settings to see valid keys."},
                "value": {"description": "New value. true/false for booleans, numeric for ints, string for enums."},
                "scope": {"type": "string", "enum": ["session", "saved"], "default": "saved"},
            },
            "required": ["key", "value"],
        },
    },
    {
        "name": "get_setting",
        "description": "Read the current value of a single setting.",
        "input_schema": {
            "type": "object",
            "properties": {"key": {"type": "string"}},
            "required": ["key"],
        },
    },
    {
        "name": "list_settings",
        "description": "List all available settings with their current values, types, and allowed ranges/enums.",
        "input_schema": {"type": "object", "properties": {}},
    },
    {
        "name": "exit_settings",
        "description": "Leave settings mode and return to normal query mode. Call this when the user is finished configuring or wants to ask a non-settings question.",
        "input_schema": {"type": "object", "properties": {}},
    },
]


# Matches "settings" / "settings mode" optionally preceded by a "hey helios"-like
# wake phrase. Matches on the full transcript (stripped), so "settings for the
# volume are too low" is NOT a match — only bare entry phrases.
# Wake-phrase prefix: h + vowel/y + anything (covers helios, hailey's, hilo's,
# halios, hylos, hylius) or "eli..." (elias, elios). Optional possessive.
_WAKE_NAME_FRAG = r"(?:h[aeiouy][a-z]*|eli[a-z]*)['s]?"
_SETTINGS_ENTRY_RE = re.compile(
    r"^\s*"
    rf"(?:(?:hey|ok|okay)\s+{_WAKE_NAME_FRAG}\s+)?"
    r"(?:open\s+|enter\s+|go\s+to\s+)?"
    r"(?:sett?ing|sitt?ing|setin)s?"
    r"(?:\s+mode)?"
    r"\s*[.!?]?\s*$",
    re.IGNORECASE,
)


mode_state: dict = {
    "mode": "query",              # "query" | "settings"
    "settings_history": [],       # scoped to current settings session; cleared on entry + exit
    "entered_at": 0.0,
}


def _is_settings_entry(transcript: str) -> bool:
    return bool(_SETTINGS_ENTRY_RE.match(transcript or ""))

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
    """Clear conversation if it has expired per settings.conversation_timeout_s."""
    timeout = settings.get("conversation_timeout_s")
    if conversation["history"] and time.time() - conversation["last_query_time"] > timeout:
        turn_count = len([m for m in conversation["history"] if m["role"] == "user"])
        log.info(f"[CONV] Expired ({turn_count} turns, {timeout}s timeout) — clearing")
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
        async with websockets.connect(uri, extra_headers=headers) as ws:
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
# Step 2: Claude Haiku Vision via Anthropic SDK
# ---------------------------------------------------------------------------
async def vision_query(transcript: str, image_b64: str, history: list) -> str:
    """Send transcript + base64 JPEG to Claude Haiku with conversation history."""
    log.info(f"[LLM] Sending to {LLM_MODEL}: \"{transcript[:80]}\" ({len(history)} history msgs)")
    t0 = time.time()

    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

    # Current user message with text + optional image (Anthropic format)
    if image_b64:
        user_content = [
            {"type": "text", "text": transcript},
            {"type": "image", "source": {
                "type": "base64", "media_type": "image/jpeg", "data": image_b64}},
        ]
    else:
        user_content = transcript

    messages = history + [{"role": "user", "content": user_content}]

    try:
        # Anthropic SDK is sync; run in a thread to avoid blocking the event loop
        response = await asyncio.to_thread(
            client.messages.create,
            model=LLM_MODEL,
            max_tokens=LLM_MAX_TOKENS,
            system=_llm_system_prompt(),
            messages=messages,
        )
        result = response.content[0].text
    except Exception as e:
        log.error(f"[LLM] Anthropic error: {e}")
        return "Sorry, I couldn't process that request."

    elapsed = time.time() - t0
    log.info(f"[LLM] Done in {elapsed:.2f}s — \"{result[:100]}...\"")
    return result


# ---------------------------------------------------------------------------
# Settings mode — Opus 4.7 with tool use
# ---------------------------------------------------------------------------
def _enter_settings_mode() -> None:
    mode_state["mode"] = "settings"
    mode_state["settings_history"] = []
    mode_state["entered_at"] = time.time()
    log.info("[MODE] -> settings")


def _exit_settings_mode() -> None:
    mode_state["mode"] = "query"
    mode_state["settings_history"] = []
    log.info("[MODE] -> query")


def _execute_settings_tool(name: str, tool_input: dict) -> dict:
    """Dispatch tool calls from Opus to the settings store. Returns a JSON-safe result."""
    try:
        if name == "update_setting":
            key = tool_input["key"]
            value = tool_input["value"]
            scope = tool_input.get("scope", "saved")
            persist = (scope == "saved")
            new_val = settings.set(key, value, persist=persist)
            return {"status": "ok", "key": key, "value": new_val, "scope": scope}
        if name == "get_setting":
            key = tool_input["key"]
            return {"key": key, "value": settings.get(key)}
        if name == "list_settings":
            return {"schema": settings.schema(), "values": settings.all_settings()}
        if name == "exit_settings":
            _exit_settings_mode()
            return {"status": "exited"}
    except settings.SettingsError as e:
        return {"status": "error", "message": str(e)}
    except Exception as e:
        log.error(f"[SETTINGS-TOOL] {name} raised: {e}")
        return {"status": "error", "message": str(e)}
    return {"status": "error", "message": f"unknown tool: {name}"}


async def settings_query(transcript: str) -> str:
    """Run Opus 4.7 with settings tools until it produces a final text response.

    Mutates mode_state["settings_history"] so multi-turn settings work. If the
    model calls exit_settings, _exit_settings_mode() runs as a side effect.
    """
    log.info(f"[SETTINGS] Opus turn: \"{transcript[:80]}\"")
    t0 = time.time()
    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

    history = mode_state["settings_history"]
    history.append({"role": "user", "content": transcript})

    final_text = ""
    max_iters = 6  # safety cap on tool loops
    for _ in range(max_iters):
        try:
            response = await asyncio.to_thread(
                client.messages.create,
                model=SETTINGS_MODEL,
                max_tokens=SETTINGS_MAX_TOKENS,
                system=SETTINGS_SYSTEM_PROMPT,
                tools=SETTINGS_TOOLS,
                messages=history,
            )
        except Exception as e:
            log.error(f"[SETTINGS] Anthropic error: {e}")
            return "Sorry, settings mode failed."

        # Persist assistant turn (content is a list of blocks; Anthropic expects it verbatim on replay)
        history.append({"role": "assistant", "content": response.content})

        if response.stop_reason == "tool_use":
            tool_results = []
            for block in response.content:
                if block.type == "tool_use":
                    result = _execute_settings_tool(block.name, block.input)
                    log.info(f"[SETTINGS-TOOL] {block.name}({block.input}) -> {result}")
                    tool_results.append({
                        "type": "tool_result",
                        "tool_use_id": block.id,
                        "content": json.dumps(result),
                    })
            history.append({"role": "user", "content": tool_results})
            continue

        # End of turn — collect any text blocks
        for block in response.content:
            if block.type == "text":
                final_text += block.text
        break
    else:
        log.warning("[SETTINGS] hit tool-use iteration cap")
        final_text = final_text or "Settings updated."

    elapsed = time.time() - t0
    log.info(f"[SETTINGS] Done in {elapsed:.2f}s — \"{final_text[:100]}\"")
    return final_text or "Okay."


# ---------------------------------------------------------------------------
# Step 3a: Cartesia Sonic 3 TTS — streaming generator
# Yields raw PCM chunks as they arrive from Cartesia. Used by StreamingResponse
# so the ESP's HTTP client gets bytes as soon as they're generated, cutting
# server-side batch-flush overhead. Accumulator lets us also publish to the
# Mac-bypass mirror after the stream completes.
# ---------------------------------------------------------------------------
async def tts_stream(text: str, pcm_sink: bytearray | None = None):
    log.info(f"[TTS-STREAM] Synthesizing {len(text)} chars")
    t0 = time.time()
    uri = (f"wss://api.cartesia.ai/tts/websocket"
           f"?api_key={CARTESIA_API_KEY}"
           f"&cartesia_version={CARTESIA_VERSION}")
    request_msg = json.dumps({
        "model_id": TTS_MODEL,
        "transcript": text,
        "voice": {"mode": "id", "id": _tts_voice_id()},
        "output_format": {
            "container": "raw",
            "encoding": TTS_ENCODING,
            "sample_rate": TTS_SAMPLE_RATE,
        },
        "context_id": str(uuid.uuid4()),
        "language": "en",
    })
    total_bytes = 0
    try:
        async with websockets.connect(uri) as ws:
            await ws.send(request_msg)
            async for msg in ws:
                if not isinstance(msg, str):
                    continue
                d = json.loads(msg)
                t = d.get("type", "")
                if t == "chunk":
                    pcm = base64.b64decode(d.get("data", ""))
                    if pcm:
                        total_bytes += len(pcm)
                        if pcm_sink is not None:
                            pcm_sink.extend(pcm)
                        yield pcm
                elif t == "done":
                    break
                elif t == "error":
                    log.error(f"[TTS-STREAM] Cartesia error: {d}")
                    break
    except Exception as e:
        log.error(f"[TTS-STREAM] Error: {e}")
    dur = total_bytes / 2 / TTS_SAMPLE_RATE
    log.info(f"[TTS-STREAM] {time.time()-t0:.2f}s, {total_bytes:,} bytes ({dur:.1f}s audio)")


# ---------------------------------------------------------------------------
# Step 3b: Cartesia Sonic 3 TTS — batch (kept for clients that want buffered bytes)
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
                    "id": _tts_voice_id(),
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
# Device state — last uploaded JPEG (consumed by next /query). Volume moved
# to settings.volume; there's only one source of truth now.
# ---------------------------------------------------------------------------
device_state = {
    "jpeg_b64": None,
}


# ---------------------------------------------------------------------------
# TTS mirror for Mac playback bypass (Mac long-polls /audio/tts/wait
# and pipes the WAV into afplay → whatever sink is active, e.g. a
# BT headset paired on the Mac).
# ---------------------------------------------------------------------------
tts_mirror = {
    "id": 0,        # monotonically increasing; 0 means nothing yet
    "wav": b"",
    "text": "",
    "ts": 0.0,
}


def _pcm_to_wav(pcm_s16le: bytes, sample_rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # 16-bit
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_s16le)
    return buf.getvalue()


def resize_jpeg_for_llm(jpeg_bytes: bytes, max_side: int = 512, quality: int = 75) -> bytes:
    """Resize JPEG so longest side is max_side. Fewer Claude image tokens + less bandwidth."""
    try:
        img = Image.open(io.BytesIO(jpeg_bytes))
        img.load()
    except Exception:
        return jpeg_bytes
    w, h = img.size
    if max(w, h) <= max_side:
        return jpeg_bytes
    scale = max_side / max(w, h)
    img = img.resize((int(w * scale), int(h * scale)), Image.LANCZOS)
    out = io.BytesIO()
    img.convert("RGB").save(out, format="JPEG", quality=quality, optimize=True)
    return out.getvalue()


def _publish_tts(pcm_s16le: bytes, text: str):
    if not pcm_s16le:
        return
    tts_mirror["id"] += 1
    tts_mirror["wav"] = _pcm_to_wav(pcm_s16le, TTS_SAMPLE_RATE)
    tts_mirror["text"] = text
    tts_mirror["ts"] = time.time()
    log.info(f"[TTS-MIRROR] Published id={tts_mirror['id']} "
             f"({len(tts_mirror['wav']):,}B WAV)")
    # Fire-and-forget playback to the Pi's default PipeWire sink
    # (typically the connected BT headset).
    asyncio.create_task(_play_on_bt(pcm_s16le))


# Default PipeWire sink to play into. Can be changed at runtime via
# POST /audio/config {"sink": "bluez_output.41_42_40_3A_47_17.1"}
# Default "" means pactl default sink.
bt_sink_name = os.getenv("HELIOS_BT_SINK", "bluez_output.41_42_40_3A_47_17.1")


def _scale_pcm_volume(pcm_s16le: bytes, pct: int) -> bytes:
    """Attenuate s16le PCM by pct (0..100) before playback. Done in software so
    we're sink-agnostic (works on macOS dev without pactl, on Pi without PulseAudio
    config). pct >= 100 passes through unchanged; pct <= 0 yields silence."""
    if pct >= 100:
        return pcm_s16le
    if pct <= 0:
        return b"\x00" * len(pcm_s16le)
    if not pcm_s16le:
        return pcm_s16le
    arr = np.frombuffer(pcm_s16le, dtype=np.int16).astype(np.int32)
    arr = (arr * int(pct)) // 100
    return arr.astype(np.int16).tobytes()


import sys as _sys

async def _play_on_bt(pcm_bytes: bytes):
    """Play raw PCM through the system's default BT sink.

    Linux (Pi prod): pipes into `paplay` with explicit s16le/24k mono.
    macOS (dev):     wraps PCM in WAV and calls `afplay`; afplay routes to
                     whatever the user has selected as default output —
                     typically the MT BT speaker after pairing it.

    Volume is applied in software from settings.volume before playback."""
    if not pcm_bytes:
        return
    pcm_bytes = _scale_pcm_volume(pcm_bytes, settings.get("volume"))

    if _sys.platform == "darwin":
        # macOS path — afplay needs a WAV on disk.
        wav = _pcm_to_wav(pcm_bytes, TTS_SAMPLE_RATE)
        path = f"/tmp/helios-tts-{int(time.time()*1000)}.wav"
        try:
            with open(path, "wb") as f:
                f.write(wav)
            proc = await asyncio.create_subprocess_exec(
                "afplay", path,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE,
            )
            _, err = await proc.communicate()
            if proc.returncode != 0:
                log.warning(f"[BT-PLAY] afplay rc={proc.returncode}: "
                            f"{err.decode('utf-8','replace')[:200]}")
            else:
                log.info(f"[BT-PLAY] afplay played {len(pcm_bytes):,}B "
                         f"(vol={settings.get('volume')}%)")
        except FileNotFoundError:
            log.error("[BT-PLAY] afplay not found — macOS path broken?")
        except Exception as e:
            log.error(f"[BT-PLAY] {e}")
        finally:
            try:
                os.unlink(path)
            except Exception:
                pass
        return

    # Linux path — paplay with raw PCM
    args = ["paplay", "--raw", "--format=s16le",
            f"--rate={TTS_SAMPLE_RATE}", "--channels=1"]
    if bt_sink_name:
        args += [f"--device={bt_sink_name}"]
    try:
        proc = await asyncio.create_subprocess_exec(
            *args,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        _, err = await proc.communicate(input=pcm_bytes)
        if proc.returncode != 0:
            log.warning(f"[BT-PLAY] paplay rc={proc.returncode}: "
                        f"{err.decode('utf-8','replace')[:200]}")
        else:
            log.info(f"[BT-PLAY] played {len(pcm_bytes):,}B to {bt_sink_name or 'default sink'} "
                     f"(vol={settings.get('volume')}%)")
    except Exception as e:
        log.error(f"[BT-PLAY] {e}")


# ---------------------------------------------------------------------------
# Pendant gesture dispatch (Pi polls pendant /button, maps gestures to mode)
# ---------------------------------------------------------------------------
PENDANT_BASE_URL = os.getenv("HELIOS_PENDANT_URL", "http://helios-cam.local")
PENDANT_POLL_S   = 0.2


async def _pendant_on_gesture(g: str):
    """Route one pendant gesture to its side-effect. No-op for gestures whose
    action isn't wired yet — those get logged so we can see them in the field."""
    log.info(f"[PENDANT] gesture: {g}  (mode={mode_state['mode']})")
    if g == "triple_tap":
        if mode_state["mode"] == "settings":
            _exit_settings_mode()
        else:
            _enter_settings_mode()
    elif g == "double_tap":
        # Repeat last response — just replay the TTS mirror.
        if tts_mirror["wav"]:
            log.info("[PENDANT] replaying last TTS")
            # Extract PCM from WAV (strip 44-byte RIFF header for raw).
            raw_pcm = tts_mirror["wav"][44:]
            asyncio.create_task(_play_on_bt(raw_pcm))
    elif g == "triple_tap_hold":
        log.info("[PENDANT] triple_tap_hold received — agent mode deferred")
    elif g == "quint_tap":
        log.info("[PENDANT] quint_tap received — sleep toggle deferred")
    elif g == "long_hold":
        log.info("[PENDANT] long_hold received — privacy pause deferred")
    # hold_start / hold_end are handled by the /ws query flow, not here.


async def _pendant_gesture_poller():
    """Polls pendant /button on a timer, drains gestures, dispatches.
    Tolerant of the pendant being offline (backoff + retry). Cancelled on shutdown."""
    backoff = PENDANT_POLL_S
    async with httpx.AsyncClient(timeout=2.0) as client:
        while True:
            try:
                r = await client.get(f"{PENDANT_BASE_URL}/button")
                if r.status_code == 200:
                    data = r.json()
                    for g in data.get("gestures", []):
                        await _pendant_on_gesture(g)
                    backoff = PENDANT_POLL_S
                else:
                    backoff = min(backoff * 2, 5.0)
            except Exception:
                backoff = min(max(backoff * 2, 1.0), 5.0)
            await asyncio.sleep(backoff)


# ---------------------------------------------------------------------------
# FastAPI App
# ---------------------------------------------------------------------------
@asynccontextmanager
async def _lifespan(app: FastAPI):
    # Pendant gesture poller — lets triple-tap etc. flip modes without voice
    poller_task = None
    if PENDANT_BASE_URL:
        log.info(f"[PENDANT] poller → {PENDANT_BASE_URL} every {PENDANT_POLL_S*1000:.0f}ms")
        poller_task = asyncio.create_task(_pendant_gesture_poller())
    try:
        yield
    finally:
        if poller_task:
            poller_task.cancel()
            try:
                await poller_task
            except (asyncio.CancelledError, Exception):
                pass


app = FastAPI(title="Helios Relay Server", lifespan=_lifespan)


@app.get("/health")
async def health():
    """Quick check that the server is running and keys are configured."""
    return {
        "status": "ok",
        "cartesia_key": "set" if CARTESIA_API_KEY else "MISSING",
        "anthropic_key": "set" if ANTHROPIC_API_KEY else "MISSING",
        "stt_model": STT_MODEL,
        "llm_model": LLM_MODEL,
        "tts_model": TTS_MODEL,
        "tts_voice": _tts_voice_id(),
        "mode": mode_state["mode"],
    }


@app.post("/clear")
async def clear_conversation_endpoint():
    """Manually clear conversation history (double-spacebar from client)."""
    turn_count = len([m for m in conversation["history"] if m["role"] == "user"])
    conversation["history"] = []
    conversation["last_query_time"] = 0.0
    log.info(f"[CONV] Manually cleared ({turn_count} turns)")
    return {"status": "cleared", "turns_cleared": turn_count}


@app.get("/audio/tts/wait")
async def tts_wait(last_id: int = 0, wait: float = 30.0):
    """Long-poll for the next TTS. Returns WAV bytes when tts_mirror id > last_id,
    or 204 if no new TTS arrived within `wait` seconds. Mac-side playback loop
    uses this to bypass the ESP speaker."""
    deadline = time.time() + max(0.0, min(wait, 60.0))
    while tts_mirror["id"] <= last_id and time.time() < deadline:
        await asyncio.sleep(0.1)
    if tts_mirror["id"] <= last_id:
        return Response(status_code=204)
    return Response(
        content=tts_mirror["wav"],
        media_type="audio/wav",
        headers={
            "X-TTS-ID": str(tts_mirror["id"]),
            "X-TTS-Text": tts_mirror["text"].encode("latin-1", "replace").decode("latin-1")[:500],
        },
    )


@app.get("/audio/tts/status")
async def tts_status():
    return {
        "id": tts_mirror["id"],
        "bytes": len(tts_mirror["wav"]),
        "text": tts_mirror["text"][:200],
        "age_s": round(time.time() - tts_mirror["ts"], 1) if tts_mirror["ts"] else None,
    }


PHOTO_DIR = "/tmp/helios-photos"
os.makedirs(PHOTO_DIR, exist_ok=True)


@app.post("/photo/upload")
async def photo_upload(request: Request):
    """ESP32 uploads a JPEG before /query. Saved to disk + resized for LLM."""
    jpeg_bytes = await request.body()
    valid = len(jpeg_bytes) >= 2 and jpeg_bytes[:2] == b'\xff\xd8'
    resized = resize_jpeg_for_llm(jpeg_bytes, max_side=512) if valid else jpeg_bytes
    device_state["jpeg_b64"] = base64.b64encode(resized).decode()

    # Save both raw and resized to disk for debugging
    ts = int(time.time())
    try:
        with open(f"{PHOTO_DIR}/{ts}.jpg", "wb") as f:
            f.write(jpeg_bytes)
        with open(f"{PHOTO_DIR}/latest.jpg", "wb") as f:
            f.write(jpeg_bytes)
        with open(f"{PHOTO_DIR}/latest_resized.jpg", "wb") as f:
            f.write(resized)
    except Exception as e:
        log.warning(f"[PHOTO] disk save failed: {e}")

    log.info(f"[PHOTO] {len(jpeg_bytes):,}B -> {len(resized):,}B resized "
             f"(valid JPEG={valid}) saved at {PHOTO_DIR}/{ts}.jpg")
    return {"status": "ok", "size": len(jpeg_bytes), "resized": len(resized), "path": f"{PHOTO_DIR}/{ts}.jpg"}


@app.get("/photo/latest")
async def photo_latest(resized: bool = False):
    """Return the most recent uploaded JPEG (or the resized version)."""
    path = f"{PHOTO_DIR}/latest_resized.jpg" if resized else f"{PHOTO_DIR}/latest.jpg"
    if not os.path.exists(path):
        return Response(status_code=404)
    with open(path, "rb") as f:
        return Response(content=f.read(), media_type="image/jpeg")


@app.get("/audio/config")
async def get_audio_config():
    """Live volume polling. Backed by settings.volume (single source of truth)."""
    return {"volume": settings.get("volume")}


@app.post("/audio/config")
async def set_audio_config(request: Request):
    """curl -X POST -d '{"volume":50}' for live tuning. Persists to settings.json."""
    body = await request.json()
    if "volume" in body:
        try:
            settings.set("volume", int(body["volume"]))
        except (settings.SettingsError, ValueError) as e:
            return JSONResponse({"error": str(e)}, status_code=400)
    return {"volume": settings.get("volume")}


# ---------------------------------------------------------------------------
# Settings HTTP surface
# ---------------------------------------------------------------------------
@app.get("/settings")
async def get_settings():
    return {
        "values": settings.all_settings(),
        "schema": settings.schema(),
        "mode": mode_state["mode"],
    }


@app.post("/settings")
async def post_settings(request: Request):
    """Batch update. Body: {"key": value, ...} or {"updates": {"key": value, ...}, "scope": "saved"|"session"}."""
    body = await request.json()
    updates = body.get("updates", {k: v for k, v in body.items() if k != "scope"})
    scope = body.get("scope", "saved")
    persist = (scope == "saved")

    results: dict[str, dict] = {}
    for key, value in updates.items():
        try:
            new_val = settings.set(key, value, persist=persist)
            results[key] = {"status": "ok", "value": new_val}
        except settings.SettingsError as e:
            results[key] = {"status": "error", "message": str(e)}
    return {"results": results, "scope": scope, "values": settings.all_settings()}


@app.post("/settings/reset")
async def post_settings_reset():
    settings.reset()
    return {"status": "ok", "values": settings.all_settings()}


@app.post("/mode/exit-settings")
async def post_exit_settings():
    """Force-exit settings mode (sighted helper / admin override)."""
    _exit_settings_mode()
    return {"mode": mode_state["mode"]}


@app.post("/pendant-event")
async def pendant_event(request: Request):
    """Push-based gesture dispatch — pendant can POST {"gesture": "triple_tap"}
    instead of waiting to be polled. Useful for lower-latency path once we add
    esp_http_client on the pendant. Currently the /button poller is the live
    path; this endpoint is an alternative."""
    try:
        body = await request.json()
    except Exception:
        return JSONResponse({"error": "invalid json"}, status_code=400)
    g = (body.get("gesture") or "").strip()
    if not g:
        return JSONResponse({"error": "missing 'gesture'"}, status_code=400)
    await _pendant_on_gesture(g)
    return {"ok": True, "mode": mode_state["mode"]}


@app.post("/query")
async def handle_query(request: Request):
    """
    Main endpoint — accepts two formats:

    1. ESP32 firmware format:
       - Content-Type: audio/L16 (or any non-JSON)
       - Body: raw PCM s16le 16kHz mono
       - JPEG comes from prior /photo/upload
       - Response: raw PCM s16le audio bytes

    2. JSON test harness format:
       - Content-Type: application/json
       - Body: {"image": base64, "audio": base64, "transcript": str}
       - Response: raw PCM s16le audio bytes + metadata headers
    """
    total_t0 = time.time()

    content_type = request.headers.get("content-type", "")

    if "json" in content_type:
        # JSON format (test harness / client.py)
        body = await request.json()
        image_b64 = body.get("image", "")
        audio_pcm = base64.b64decode(body["audio"]) if body.get("audio") else b""
        transcript = body.get("transcript", "")
    else:
        # Raw PCM format (ESP32 firmware)
        audio_pcm = await request.body()
        image_b64 = device_state.get("jpeg_b64") or ""
        device_state["jpeg_b64"] = None  # consume it
        transcript = ""

    log.info(f"{'='*60}")
    log.info(f"[QUERY] Received: image={'yes' if image_b64 else 'no'}  "
             f"audio={len(audio_pcm):,}B  transcript={'yes' if transcript else 'no'}")

    # --- Step 1: STT (skip if transcript provided) ---
    if transcript:
        log.info(f"[QUERY] Using provided transcript: \"{transcript}\"")
    else:
        transcript = await stt_transcribe(audio_pcm)

    if not transcript.strip():
        if mode_state["mode"] == "settings":
            transcript = "done"  # empty transcript in settings mode = exit intent
        else:
            transcript = "What's in front of me?"
        log.warning(f"[QUERY] Empty transcript, using fallback: \"{transcript}\"")

    # --- Step 1.5: Mode detection ---
    # A bare "settings"/"hey helios settings" transcript from query mode enters settings.
    # Once in settings mode, all transcripts route through Opus + tools; the model
    # calls exit_settings() to leave.
    if mode_state["mode"] == "query" and _is_settings_entry(transcript):
        _enter_settings_mode()
        response_text = "Settings mode."
    elif mode_state["mode"] == "settings":
        response_text = await settings_query(transcript)
    else:
        # --- Step 2: Vision LLM (conversation gated by settings.conversation_memory) ---
        keep_history = settings.get("conversation_memory")
        check_conversation_timeout()
        history = conversation["history"] if keep_history else []
        response_text = await vision_query(transcript, image_b64, history)
        if keep_history:
            conversation["history"].append({"role": "user", "content": transcript})
            conversation["history"].append({"role": "assistant", "content": response_text})
            conversation["last_query_time"] = time.time()

    # --- Step 3: Stream TTS back (also mirror to Mac bypass) ---
    def _latin1_safe(s: str) -> str:
        return s.encode("latin-1", "replace").decode("latin-1")

    pcm_accum = bytearray()

    async def _body():
        async for chunk in tts_stream(response_text, pcm_sink=pcm_accum):
            yield chunk
        # After stream completes, publish to Mac bypass mirror
        _publish_tts(bytes(pcm_accum), response_text)
        total_ms = (time.time() - total_t0) * 1000
        log.info(f"[QUERY] Total pipeline: {total_ms:.0f}ms")
        log.info(f"{'='*60}")

    return StreamingResponse(
        _body(),
        media_type="application/octet-stream",
        headers={
            "X-Transcript": _latin1_safe(transcript[:500]),
            "X-Response-Text": _latin1_safe(response_text[:500]),
            "X-Sample-Rate": str(TTS_SAMPLE_RATE),
            "X-Encoding": TTS_ENCODING,
        },
    )


# ---------------------------------------------------------------------------
# WebSocket streaming endpoint (ESP32 firmware uses this)
# ---------------------------------------------------------------------------
@app.websocket("/ws")
async def ws_query(ws: WebSocket):
    """
    Streaming query over WebSocket:
      ESP → Pi: first binary msg = JPEG (detected by SOI marker),
                subsequent binary msgs = PCM s16le 16kHz mic chunks,
                text "done" = end of mic recording.
      Pi → ESP: binary msgs = PCM s16le 24kHz TTS chunks,
                text "done" = end of TTS.
    """
    await ws.accept()
    total_t0 = time.time()
    jpeg_b64 = ""
    stt_ws = None
    first_binary = True

    try:
        # Open Cartesia STT WebSocket immediately for lowest latency
        stt_uri = (f"wss://api.cartesia.ai/stt/websocket"
                   f"?model={STT_MODEL}&encoding={STT_ENCODING}"
                   f"&sample_rate={STT_SAMPLE_RATE}&language=en")
        stt_headers = {"X-API-Key": CARTESIA_API_KEY, "Cartesia-Version": CARTESIA_VERSION}
        stt_ws = await websockets.connect(stt_uri, extra_headers=stt_headers)
        log.info("[WS] STT stream opened, waiting for ESP data...")

        # --- Receive JPEG + mic PCM from ESP ---
        mic_bytes = 0
        while True:
            msg = await ws.receive()
            if "text" in msg:
                if msg["text"] == "done":
                    break
            elif "bytes" in msg:
                data = msg["bytes"]
                if first_binary:
                    first_binary = False
                    if len(data) >= 2 and data[0] == 0xFF and data[1] == 0xD8:
                        jpeg_b64 = base64.b64encode(data).decode()
                        log.info(f"[WS] JPEG: {len(data):,} bytes")
                        continue
                # Forward PCM to Cartesia STT
                await stt_ws.send(data)
                mic_bytes += len(data)

        mic_dur = mic_bytes / 2 / STT_SAMPLE_RATE
        log.info(f"[WS] Mic done: {mic_bytes:,} bytes ({mic_dur:.1f}s)")

        # --- Finalize STT ---
        await stt_ws.send("done")
        stt_parts = []
        async for stt_msg in stt_ws:
            if isinstance(stt_msg, str):
                d = json.loads(stt_msg)
                if d.get("type") == "transcript" and d.get("is_final"):
                    t = d.get("text", "").strip()
                    if t:
                        stt_parts.append(t)
                elif d.get("type") in ("done", "flush_done"):
                    break
                elif d.get("type") == "error":
                    log.error(f"[WS-STT] Error: {d}")
                    break
        await stt_ws.close()
        stt_ws = None

        transcript = " ".join(stt_parts)
        stt_time = time.time() - total_t0
        if not transcript:
            if mode_state["mode"] == "settings":
                transcript = "done"
            else:
                transcript = "What do you see?" if jpeg_b64 else "Hello"
            log.warning(f"[WS] Empty STT, fallback: \"{transcript}\"")
        else:
            log.info(f"[WS] STT ({stt_time:.1f}s): \"{transcript}\"")

        # --- Mode detection + LLM dispatch ---
        if mode_state["mode"] == "query" and _is_settings_entry(transcript):
            _enter_settings_mode()
            response_text = "Settings mode."
        elif mode_state["mode"] == "settings":
            response_text = await settings_query(transcript)
        else:
            keep_history = settings.get("conversation_memory")
            check_conversation_timeout()
            history = conversation["history"] if keep_history else []
            response_text = await vision_query(transcript, jpeg_b64, history)
            if keep_history:
                conversation["history"].append({"role": "user", "content": transcript})
                conversation["history"].append({"role": "assistant", "content": response_text})
                conversation["last_query_time"] = time.time()
        log.info(f"[WS] LLM: \"{response_text[:80]}\"")

        # --- Stream TTS back to ESP ---
        tts_uri = (f"wss://api.cartesia.ai/tts/websocket"
                   f"?api_key={CARTESIA_API_KEY}"
                   f"&cartesia_version={CARTESIA_VERSION}")
        tts_bytes_sent = 0

        async with websockets.connect(tts_uri) as tts_ws:
            request_msg = json.dumps({
                "model_id": TTS_MODEL,
                "transcript": response_text,
                "voice": {"mode": "id", "id": _tts_voice_id()},
                "output_format": {
                    "container": "raw",
                    "encoding": TTS_ENCODING,
                    "sample_rate": TTS_SAMPLE_RATE,
                },
                "context_id": str(uuid.uuid4()),
                "language": "en",
            })
            await tts_ws.send(request_msg)

            async for tts_msg in tts_ws:
                if isinstance(tts_msg, str):
                    d = json.loads(tts_msg)
                    if d.get("type") == "chunk":
                        pcm = base64.b64decode(d.get("data", ""))
                        if pcm:
                            await ws.send_bytes(pcm)
                            tts_bytes_sent += len(pcm)
                    elif d.get("type") == "done":
                        break
                    elif d.get("type") == "error":
                        log.error(f"[WS-TTS] Error: {d}")
                        break

        await ws.send_text("done")
        total_ms = (time.time() - total_t0) * 1000
        tts_dur = tts_bytes_sent / 2 / TTS_SAMPLE_RATE
        log.info(f"[WS] Complete: {total_ms:.0f}ms total, "
                 f"TTS={tts_bytes_sent:,}B ({tts_dur:.1f}s)")

    except WebSocketDisconnect:
        log.info("[WS] Client disconnected")
    except Exception as e:
        log.error(f"[WS] Error: {e}")
        import traceback
        traceback.print_exc()
    finally:
        if stt_ws:
            try:
                await stt_ws.close()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn

    if not CARTESIA_API_KEY:
        log.error("CARTESIA_API_KEY not set! Copy .env.example to .env and add your key.")
    if not ANTHROPIC_API_KEY:
        log.error("ANTHROPIC_API_KEY not set! Copy .env.example to .env and add your key.")

    log.info(f"Starting Helios relay server on port 5750")
    log.info(f"  STT:  {STT_MODEL} @ {STT_SAMPLE_RATE}Hz {STT_ENCODING}")
    log.info(f"  LLM:  {LLM_MODEL}")
    log.info(f"  TTS:  {TTS_MODEL} -> {TTS_ENCODING} @ {TTS_SAMPLE_RATE}Hz")
    log.info(f"  Voice: {_tts_voice_id()}")

    uvicorn.run(app, host="0.0.0.0", port=5750)
