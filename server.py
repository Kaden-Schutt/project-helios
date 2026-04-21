"""
Helios Relay Server — core loop only.

Flow per query:
    ESP32 pendant (button hold)
      -> POST /photo/upload  (latest JPEG)
      -> POST /query         (raw PCM s16le 16kHz mono)
         - Cartesia Ink-Whisper STT
         - Claude Haiku vision (transcript + image)
         - Cartesia Sonic 3 TTS
         - paplay / afplay to the Pi's BT sink (MT speaker)
      <- response headers carry transcript + reply text

Config is in-file constants. No settings JSON, no voice-mode, no
gesture dispatch, no wake-word. Keep it dumb until the core runs.

Run:  python server.py
"""

import asyncio
import base64
import io
import json
import logging
import os
import sys
import time
import uuid
import wave

import anthropic
import httpx
import numpy as np
import websockets
from dotenv import load_dotenv
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, Response, StreamingResponse
from PIL import Image

load_dotenv()

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
CARTESIA_API_KEY  = os.getenv("CARTESIA_API_KEY", "")
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY", "")

STT_MODEL         = "ink-whisper"
STT_SAMPLE_RATE   = 16000
STT_ENCODING      = "pcm_s16le"

TTS_MODEL         = "sonic-3-2026-01-12"
TTS_VOICE_ID      = os.getenv("TTS_VOICE_ID", "f786b574-daa5-4673-aa0c-cbe3e8534c02")
TTS_SAMPLE_RATE   = 24000
TTS_ENCODING      = "pcm_s16le"
CARTESIA_VERSION  = "2026-01-12"

LLM_MODEL         = "claude-haiku-4-5-20251001"
LLM_MAX_TOKENS    = 512

# Server-controlled settings (just constants — the voice-config loop is out).
VOLUME_PCT        = int(os.getenv("HELIOS_VOLUME", "80"))   # 0..100, applied in software
BT_SINK           = os.getenv("HELIOS_BT_SINK", "bluez_output.41_42_40_3A_47_17.1")

LLM_SYSTEM_PROMPT = """You are an assistive AI in a wearable for a vision-impaired user. Your response is spoken aloud via TTS, so it MUST be short and immediately useful.

STRICT RULES:
- Respond in 1-2 sentences MAX. Never more.
- Use the image to answer concretely with locations/directions. The user cannot see.
- Never start with "I can see..." or "It looks like..." — just answer.
- If the user asks where something is, name where in the image it appears (e.g. "on the coffee table, left of the remote").

If the image doesn't show what the user asked about, say so in one sentence."""

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    datefmt="%H:%M:%S",
)
log = logging.getLogger("helios")


# ---------------------------------------------------------------------------
# Audio helpers
# ---------------------------------------------------------------------------
def _scale_pcm_volume(pcm_s16le: bytes, pct: int) -> bytes:
    if pct >= 100 or not pcm_s16le:
        return pcm_s16le
    if pct <= 0:
        return b"\x00" * len(pcm_s16le)
    arr = np.frombuffer(pcm_s16le, dtype=np.int16).astype(np.int32)
    return ((arr * int(pct)) // 100).astype(np.int16).tobytes()


def _pcm_to_wav(pcm_s16le: bytes, sample_rate: int) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_s16le)
    return buf.getvalue()


async def _play_tts(pcm_bytes: bytes):
    """Play raw 24 kHz s16le PCM to the default/Configured BT sink.

    Linux (Pi): paplay to BT_SINK.
    macOS (dev): wrap in WAV and afplay (routes to default output — MT when paired)."""
    if not pcm_bytes:
        return
    pcm = _scale_pcm_volume(pcm_bytes, VOLUME_PCT)

    if sys.platform == "darwin":
        wav = _pcm_to_wav(pcm, TTS_SAMPLE_RATE)
        path = f"/tmp/helios-tts-{int(time.time() * 1000)}.wav"
        try:
            with open(path, "wb") as f:
                f.write(wav)
            proc = await asyncio.create_subprocess_exec(
                "afplay", path,
                stdout=asyncio.subprocess.DEVNULL,
                stderr=asyncio.subprocess.PIPE,
            )
            _, err = await proc.communicate()
            if proc.returncode == 0:
                log.info(f"[PLAY] afplay {len(pcm):,}B (vol={VOLUME_PCT}%)")
            else:
                log.warning(f"[PLAY] afplay rc={proc.returncode} {err.decode('utf-8', 'replace')[:200]}")
        except Exception as e:
            log.error(f"[PLAY] {e}")
        finally:
            try:
                os.unlink(path)
            except Exception:
                pass
        return

    args = ["paplay", "--raw", "--format=s16le",
            f"--rate={TTS_SAMPLE_RATE}", "--channels=1"]
    if BT_SINK:
        args += [f"--device={BT_SINK}"]
    try:
        proc = await asyncio.create_subprocess_exec(
            *args,
            stdin=asyncio.subprocess.PIPE,
            stdout=asyncio.subprocess.DEVNULL,
            stderr=asyncio.subprocess.PIPE,
        )
        _, err = await proc.communicate(input=pcm)
        if proc.returncode == 0:
            log.info(f"[PLAY] paplay {len(pcm):,}B -> {BT_SINK or 'default'} (vol={VOLUME_PCT}%)")
        else:
            log.warning(f"[PLAY] paplay rc={proc.returncode} {err.decode('utf-8', 'replace')[:200]}")
    except Exception as e:
        log.error(f"[PLAY] {e}")


def resize_jpeg_for_llm(jpeg_bytes: bytes, max_side: int = 512, quality: int = 75) -> bytes:
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


# ---------------------------------------------------------------------------
# Pipeline stages
# ---------------------------------------------------------------------------
async def stt_transcribe(audio_pcm: bytes) -> str:
    if not audio_pcm:
        return ""
    log.info(f"[STT] {len(audio_pcm):,}B ({len(audio_pcm) / 2 / STT_SAMPLE_RATE:.1f}s)")
    t0 = time.time()
    uri = (
        f"wss://api.cartesia.ai/stt/websocket"
        f"?model={STT_MODEL}&encoding={STT_ENCODING}"
        f"&sample_rate={STT_SAMPLE_RATE}&language=en"
    )
    headers = {"X-API-Key": CARTESIA_API_KEY, "Cartesia-Version": CARTESIA_VERSION}
    parts: list[str] = []
    try:
        async with websockets.connect(uri, extra_headers=headers) as ws:
            chunk = 8192
            for i in range(0, len(audio_pcm), chunk):
                await ws.send(audio_pcm[i:i + chunk])
            await ws.send("done")
            async for raw in ws:
                if not isinstance(raw, str):
                    continue
                d = json.loads(raw)
                if d.get("type") == "transcript" and d.get("is_final"):
                    t = (d.get("text") or "").strip()
                    if t:
                        parts.append(t)
                elif d.get("type") in ("done", "flush_done"):
                    break
                elif d.get("type") == "error":
                    log.error(f"[STT] cartesia error: {d}")
                    break
    except Exception as e:
        log.error(f"[STT] {e}")
        return ""
    text = " ".join(parts)
    log.info(f"[STT] {time.time() - t0:.2f}s → {text!r}")
    return text


async def vision_query(transcript: str, image_b64: str) -> str:
    log.info(f"[LLM] {LLM_MODEL} {transcript[:80]!r} image={'yes' if image_b64 else 'no'}")
    t0 = time.time()
    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

    if image_b64:
        content = [
            {"type": "text", "text": transcript},
            {"type": "image", "source": {
                "type": "base64", "media_type": "image/jpeg", "data": image_b64}},
        ]
    else:
        content = transcript

    try:
        resp = await asyncio.to_thread(
            client.messages.create,
            model=LLM_MODEL,
            max_tokens=LLM_MAX_TOKENS,
            system=LLM_SYSTEM_PROMPT,
            messages=[{"role": "user", "content": content}],
        )
        out = resp.content[0].text
    except Exception as e:
        log.error(f"[LLM] {e}")
        return "Sorry, I couldn't process that."
    log.info(f"[LLM] {time.time() - t0:.2f}s → {out[:100]!r}")
    return out


async def tts_synthesize(text: str) -> bytes:
    log.info(f"[TTS] {len(text)} chars")
    t0 = time.time()
    async with httpx.AsyncClient(timeout=30.0) as client:
        r = await client.post(
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
                    "encoding": TTS_ENCODING,
                    "sample_rate": TTS_SAMPLE_RATE,
                },
                "language": "en",
            },
        )
        if r.status_code != 200:
            log.error(f"[TTS] http {r.status_code} {r.text[:300]}")
            return b""
    pcm = r.content
    log.info(f"[TTS] {time.time() - t0:.2f}s → {len(pcm):,}B ({len(pcm) / 2 / TTS_SAMPLE_RATE:.1f}s)")
    return pcm


# ---------------------------------------------------------------------------
# App + endpoints
# ---------------------------------------------------------------------------
app = FastAPI(title="Helios Relay Server")

_last_jpeg_b64: str | None = None

PHOTO_DIR = "/tmp/helios-photos"
os.makedirs(PHOTO_DIR, exist_ok=True)


@app.get("/health")
async def health():
    return {
        "status": "ok",
        "cartesia_key":  "set" if CARTESIA_API_KEY  else "MISSING",
        "anthropic_key": "set" if ANTHROPIC_API_KEY else "MISSING",
        "stt_model": STT_MODEL,
        "llm_model": LLM_MODEL,
        "tts_model": TTS_MODEL,
        "tts_voice": TTS_VOICE_ID,
        "platform":  sys.platform,
        "volume":    VOLUME_PCT,
    }


@app.post("/photo/upload")
async def photo_upload(request: Request):
    global _last_jpeg_b64
    raw = await request.body()
    valid = len(raw) >= 2 and raw[:2] == b"\xff\xd8"
    resized = resize_jpeg_for_llm(raw, max_side=512) if valid else raw
    _last_jpeg_b64 = base64.b64encode(resized).decode()
    ts = int(time.time())
    try:
        with open(f"{PHOTO_DIR}/{ts}.jpg", "wb") as f: f.write(raw)
        with open(f"{PHOTO_DIR}/latest.jpg", "wb") as f: f.write(raw)
    except Exception as e:
        log.warning(f"[PHOTO] save: {e}")
    log.info(f"[PHOTO] {len(raw):,}B raw → {len(resized):,}B resized valid={valid}")
    return {"status": "ok", "size": len(raw), "resized": len(resized)}


@app.get("/photo/latest")
async def photo_latest():
    path = f"{PHOTO_DIR}/latest.jpg"
    if not os.path.exists(path):
        return Response(status_code=404)
    with open(path, "rb") as f:
        return Response(content=f.read(), media_type="image/jpeg")


@app.post("/query")
async def handle_query(request: Request):
    """Accepts either:
    - Content-Type: application/json  → {"image": b64, "audio": b64, "transcript": str}
    - anything else (audio/L16 etc.)  → raw PCM s16le 16 kHz body; JPEG from last /photo/upload
    """
    global _last_jpeg_b64
    t0 = time.time()
    ctype = request.headers.get("content-type", "")
    if "json" in ctype:
        body = await request.json()
        image_b64 = body.get("image", "")
        audio = base64.b64decode(body["audio"]) if body.get("audio") else b""
        transcript = body.get("transcript", "")
    else:
        audio = await request.body()
        image_b64 = _last_jpeg_b64 or ""
        _last_jpeg_b64 = None
        transcript = ""

    log.info("=" * 60)
    log.info(f"[QUERY] audio={len(audio):,}B image={'yes' if image_b64 else 'no'} "
             f"transcript={'given' if transcript else 'stt'}")

    if not transcript:
        transcript = await stt_transcribe(audio)
    if not transcript.strip():
        transcript = "What's in front of me?"
        log.warning(f"[QUERY] empty transcript, fallback {transcript!r}")

    reply = await vision_query(transcript, image_b64)
    pcm = await tts_synthesize(reply)
    await _play_tts(pcm)

    total_ms = (time.time() - t0) * 1000
    log.info(f"[QUERY] done in {total_ms:.0f}ms")

    def _latin1(s: str) -> str:
        return s.encode("latin-1", "replace").decode("latin-1")

    return Response(
        content=pcm,
        media_type="application/octet-stream",
        headers={
            "X-Transcript":    _latin1(transcript[:500]),
            "X-Response-Text": _latin1(reply[:500]),
            "X-Sample-Rate":   str(TTS_SAMPLE_RATE),
            "X-Encoding":      TTS_ENCODING,
        },
    )


# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    import uvicorn
    if not CARTESIA_API_KEY:
        log.error("CARTESIA_API_KEY not set")
    if not ANTHROPIC_API_KEY:
        log.error("ANTHROPIC_API_KEY not set")

    log.info(f"Helios relay server on :5750  platform={sys.platform}  sink={BT_SINK}")
    log.info(f"  STT={STT_MODEL}@{STT_SAMPLE_RATE}Hz  LLM={LLM_MODEL}  TTS={TTS_MODEL}@{TTS_SAMPLE_RATE}Hz")
    log.info(f"  voice={TTS_VOICE_ID}  volume={VOLUME_PCT}%")
    uvicorn.run(app, host="0.0.0.0", port=5750)
