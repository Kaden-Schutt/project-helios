"""
Helios BLE Voice Server
========================
BLE central (client) that connects to ESP32 Helios peripheral.
Receives mic PCM via notifications, runs STT→LLM→TTS pipeline,
sends TTS PCM back via BLE writes.

Run:  python server_ble.py
"""

import asyncio
import struct
import time
import os
import json
import logging
import base64
import numpy as np

# Ensure libopus can be found on macOS (Homebrew)
if os.path.exists("/opt/homebrew/lib"):
    os.environ.setdefault("DYLD_LIBRARY_PATH", "/opt/homebrew/lib")

from bleak import BleakClient, BleakScanner
from dotenv import load_dotenv
import httpx
import websockets

load_dotenv()

# --- Config ---
CARTESIA_API_KEY = os.getenv("CARTESIA_API_KEY", "")
OPENROUTER_API_KEY = os.getenv("OPENROUTER_API_KEY", "")

STT_MODEL = "ink-whisper"
STT_SAMPLE_RATE = 16000  # Mic arrives as µ-law 8-bit @ 16kHz, decoded to s16le for STT

TTS_MODEL = "sonic-3-2026-01-12"
TTS_VOICE_ID = os.getenv("TTS_VOICE_ID", "f786b574-daa5-4673-aa0c-cbe3e8534c02")
TTS_SAMPLE_RATE = 24000  # Matches ESP speaker; Opus compresses regardless of rate
CARTESIA_VERSION = "2026-01-12"

LLM_MODEL = "google/gemini-3.1-flash-lite-preview:nitro"
LLM_VISION_MODEL = "google/gemini-3.1-pro-preview:nitro"  # vision-capable model
LLM_MAX_TOKENS = 300

SYSTEM_PROMPT = """You are an assistive AI embedded in a wearable device for a vision-impaired user.
You receive the user's spoken question and optionally an image from their camera.
Respond concisely and helpfully — your response will be spoken aloud via TTS.
Keep answers to 1-3 sentences. Be direct and descriptive about what you see."""

# BLE UUIDs (must match ESP32 GATT service)
SERVICE_UUID    = "87654321-4321-4321-4321-cba987654321"
MIC_TX_UUID     = "87654321-4321-4321-4321-cba987654322"
SPEAKER_RX_UUID = "87654321-4321-4321-4321-cba987654323"
CONTROL_UUID    = "87654321-4321-4321-4321-cba987654324"

DEVICE_NAME = "Helios"
BLE_CHUNK_SIZE = 509

# Control commands
CMD_CONNECTED       = 0x01
CMD_PROCESSING      = 0x02
CMD_SET_VOLUME      = 0x03
CMD_ERROR           = 0x04
CMD_BUTTON_PRESSED  = 0x10
CMD_BUTTON_RELEASED = 0x11
CMD_PLAYBACK_DONE   = 0x12

CONVERSATION_TIMEOUT = 300

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(message)s", datefmt="%H:%M:%S")
log = logging.getLogger("helios")

# --- Pipeline (reused from server_nocam.py) ---

conversation = {"history": [], "last_time": 0.0}


def ulaw_to_pcm(ulaw_data: bytes) -> bytes:
    """Decode µ-law 8-bit to signed 16-bit PCM. Builds a 256-entry lookup table."""
    # Build decode table once
    table = np.zeros(256, dtype=np.int16)
    for i in range(256):
        s = ~i & 0xFF
        sign = s & 0x80
        exponent = (s >> 4) & 0x07
        mantissa = s & 0x0F
        val = ((mantissa << 4) | 0x84) << exponent
        val -= 0x84
        table[i] = -val if sign else val

    indices = np.frombuffer(ulaw_data, dtype=np.uint8)
    return table[indices].tobytes()


def pcm_to_opus(pcm_s16: bytes, sample_rate: int = 16000) -> bytes:
    """Encode s16le PCM to length-prefixed Opus frames for ESP32 playback."""
    try:
        import opuslib
    except ImportError:
        log.error("opuslib not installed! Run: pip install opuslib")
        log.error("Also need: brew install opus (macOS) or apt install libopus0 (Linux)")
        return b""

    enc = opuslib.Encoder(sample_rate, 1, opuslib.APPLICATION_VOIP)
    frame_samples = sample_rate // 50  # 20ms frames
    frame_bytes = frame_samples * 2     # s16le

    frames = bytearray()
    for i in range(0, len(pcm_s16) - frame_bytes + 1, frame_bytes):
        pcm_frame = pcm_s16[i:i + frame_bytes]
        opus_frame = enc.encode(pcm_frame, frame_samples)
        frames.extend(struct.pack('<H', len(opus_frame)))
        frames.extend(opus_frame)

    # End marker
    frames.extend(struct.pack('<H', 0))

    ratio = len(pcm_s16) / len(frames) if frames else 0
    log.info(f"Opus encode: {len(pcm_s16):,} bytes PCM → {len(frames):,} bytes Opus ({ratio:.1f}:1)")
    return bytes(frames)


def pcm_to_ulaw(pcm_data: bytes) -> bytes:
    """Encode signed 16-bit PCM to µ-law 8-bit."""
    samples = np.frombuffer(pcm_data, dtype=np.int16).astype(np.int32)
    BIAS = 0x84
    MAX = 0x7FFF

    sign = (samples >> 8) & 0x80
    abs_samples = np.abs(samples)
    abs_samples = np.clip(abs_samples, 0, MAX)
    abs_samples += BIAS

    # Find exponent (position of highest set bit)
    exponent = np.zeros(len(samples), dtype=np.int32)
    for exp in range(7, 0, -1):
        mask = abs_samples & (0x4000 >> (7 - exp))
        exponent = np.where((exponent == 0) & (mask != 0), exp, exponent)

    mantissa = (abs_samples >> (exponent + 3)) & 0x0F
    ulaw = ~(sign | (exponent << 4) | mantissa) & 0xFF
    return ulaw.astype(np.uint8).tobytes()


def upsample_8k_to_16k(pcm_8k: bytes) -> bytes:
    """Upsample 8kHz s16le PCM to 16kHz with linear interpolation."""
    # Align to sample boundary
    pcm_8k = pcm_8k[:len(pcm_8k) & ~1]
    if len(pcm_8k) == 0:
        return b""
    samples = np.frombuffer(pcm_8k, dtype=np.int16)
    upsampled = np.zeros(len(samples) * 2, dtype=np.int16)
    upsampled[0::2] = samples
    upsampled[1::2][:-1] = ((samples[:-1].astype(np.int32) + samples[1:].astype(np.int32)) // 2).astype(np.int16)
    upsampled[-1] = samples[-1]
    return upsampled.tobytes()


async def stt_transcribe(audio_pcm):
    if not audio_pcm:
        return ""
    duration = len(audio_pcm) / 2 / STT_SAMPLE_RATE
    log.info(f"[STT] Sending {len(audio_pcm):,} bytes ({duration:.1f}s)")
    t0 = time.time()

    uri = (f"wss://api.cartesia.ai/stt/websocket"
           f"?model={STT_MODEL}&encoding=pcm_s16le&sample_rate={STT_SAMPLE_RATE}&language=en")
    headers = {"X-API-Key": CARTESIA_API_KEY, "Cartesia-Version": CARTESIA_VERSION}

    parts = []
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
                            parts.append(text)
                    elif data.get("type") in ("done", "flush_done"):
                        break
    except Exception as e:
        log.error(f"[STT] Error: {e}")
        return ""

    transcript = " ".join(parts)
    log.info(f"[STT] {time.time()-t0:.1f}s — \"{transcript}\"")
    return transcript


# --- Image injection for camera-less mode ---
_image_index = 0

def get_next_image_b64():
    """Get the next test image from input/ directory as base64. Returns None if no images."""
    global _image_index
    import glob as g
    images = sorted(g.glob("input/*.jpg") + g.glob("input/*.jpeg") + g.glob("input/*.png"))
    if not images:
        return None
    path = images[_image_index % len(images)]
    _image_index += 1
    with open(path, "rb") as f:
        data = f.read()
    b64 = base64.b64encode(data).decode()
    log.info(f"[IMG] Injecting {path} ({len(data)//1024}KB) [{_image_index}/{len(images)}]")
    return b64


async def llm_query(transcript, history, image_b64=None):
    log.info(f"[LLM] \"{transcript[:80]}\"")
    t0 = time.time()

    messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    messages.extend(history)

    # Build user message — with or without image
    if image_b64:
        messages.append({
            "role": "user",
            "content": [
                {"type": "text", "text": transcript},
                {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"}},
            ],
        })
    else:
        messages.append({"role": "user", "content": transcript})

    async with httpx.AsyncClient(timeout=30.0) as client:
        resp = await client.post(
            "https://openrouter.ai/api/v1/chat/completions",
            headers={"Authorization": f"Bearer {OPENROUTER_API_KEY}", "Content-Type": "application/json"},
            json={"model": LLM_MODEL, "messages": messages, "max_tokens": LLM_MAX_TOKENS},
        )
        if resp.status_code != 200:
            log.error(f"[LLM] Error {resp.status_code}: {resp.text[:200]}")
            return "Sorry, I couldn't process that."
        result = resp.json()["choices"][0]["message"]["content"]

    log.info(f"[LLM] {time.time()-t0:.1f}s — \"{result[:100]}\"")
    return result


async def tts_synthesize(text):
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
            log.error(f"[TTS] Error {resp.status_code}: {resp.text[:200]}")
            return b""

    duration = len(resp.content) / 2 / TTS_SAMPLE_RATE
    log.info(f"[TTS] {time.time()-t0:.1f}s — {len(resp.content):,} bytes ({duration:.1f}s)")
    return resp.content


async def process_query_streaming(pcm_data, is_pcm, send_tts_cb):
    """Fully streaming pipeline: STT → LLM streaming → TTS streaming → Opus → BLE.
    send_tts_cb(opus_bytes) is called for each Opus-encoded TTS chunk."""
    total_t0 = time.time()

    # Conversation timeout
    if conversation["history"] and time.time() - conversation["last_time"] > CONVERSATION_TIMEOUT:
        log.info("[CONV] Expired — clearing")
        conversation["history"] = []

    # STT (batch for now — already fast since audio arrived via Opus stream)
    if is_pcm:
        transcript = await stt_transcribe(pcm_data)
    else:
        pcm_16 = ulaw_to_pcm(pcm_data)
        transcript = await stt_transcribe(pcm_16)
    if not transcript.strip():
        transcript = "Hello"
        log.warning(f"[STT] Empty, fallback: \"{transcript}\"")

    stt_time = time.time() - total_t0
    log.info(f"[STT] Done in {stt_time:.1f}s: \"{transcript}\"")

    # Inject image if available in input/ directory
    image_b64 = get_next_image_b64()
    model = LLM_VISION_MODEL if image_b64 else LLM_MODEL

    # LLM streaming — get tokens as they arrive
    log.info(f"[LLM] Streaming: \"{transcript[:80]}\" (model={model.split('/')[-1]}, image={'yes' if image_b64 else 'no'})")
    llm_t0 = time.time()

    messages = [{"role": "system", "content": SYSTEM_PROMPT}]
    messages.extend(conversation["history"])
    if image_b64:
        messages.append({
            "role": "user",
            "content": [
                {"type": "text", "text": transcript},
                {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{image_b64}"}},
            ],
        })
    else:
        messages.append({"role": "user", "content": transcript})

    full_response = ""
    sentence_buf = ""
    tts_ws = None
    opus_enc = None
    first_audio_sent = False
    total_tts_bytes = 0
    total_opus_bytes = 0

    try:
        import opuslib
        opus_enc = opuslib.Encoder(TTS_SAMPLE_RATE, 1, opuslib.APPLICATION_VOIP)
    except Exception as e:
        log.error(f"Opus encoder failed: {e}")

    # Open TTS WebSocket
    tts_uri = f"wss://api.cartesia.ai/tts/websocket?api_key={CARTESIA_API_KEY}&cartesia_version={CARTESIA_VERSION}"
    try:
        tts_ws = await websockets.connect(tts_uri)
    except Exception as e:
        log.error(f"[TTS] WebSocket open failed: {e}")
        tts_ws = None

    # Helper: send text to TTS WebSocket, receive audio chunks, Opus encode, send to ESP
    async def flush_tts(text, is_last=False):
        nonlocal first_audio_sent, total_tts_bytes, total_opus_bytes
        if not tts_ws or not text.strip():
            return

        msg = {
            "model_id": TTS_MODEL,
            "transcript": text,
            "voice": {"mode": "id", "id": TTS_VOICE_ID},
            "output_format": {
                "container": "raw",
                "encoding": "pcm_s16le",
                "sample_rate": TTS_SAMPLE_RATE,
            },
            "language": "en",
            "context_id": "helios-stream",
            "continue": not is_last,
        }
        await tts_ws.send(json.dumps(msg))

        # Read audio chunks from TTS WebSocket
        import base64
        frame_samples = TTS_SAMPLE_RATE // 50  # 20ms
        frame_bytes = frame_samples * 2
        pcm_accum = bytearray()

        async for response in tts_ws:
            if isinstance(response, str):
                data = json.loads(response)
                if "data" in data and data["data"]:
                    audio_chunk = base64.b64decode(data["data"])
                    total_tts_bytes += len(audio_chunk)
                    pcm_accum.extend(audio_chunk)

                    # Opus encode complete 20ms frames and send
                    while len(pcm_accum) >= frame_bytes and opus_enc:
                        pcm_frame = bytes(pcm_accum[:frame_bytes])
                        pcm_accum = pcm_accum[frame_bytes:]
                        opus_frame = opus_enc.encode(pcm_frame, frame_samples)
                        # Length-prefix for ESP
                        opus_packet = struct.pack('<H', len(opus_frame)) + opus_frame
                        total_opus_bytes += len(opus_frame)

                        if not first_audio_sent:
                            first_audio_sent = True
                            latency = time.time() - total_t0
                            log.info(f"[STREAM] First audio at {latency:.1f}s after query start")

                        await send_tts_cb(opus_packet)

                if data.get("done", False):
                    break

        # Flush remaining PCM
        if len(pcm_accum) >= 2 and opus_enc:
            # Pad to frame size
            pad_len = frame_bytes - len(pcm_accum)
            if pad_len > 0:
                pcm_accum.extend(b'\x00' * pad_len)
            opus_frame = opus_enc.encode(bytes(pcm_accum[:frame_bytes]), frame_samples)
            opus_packet = struct.pack('<H', len(opus_frame)) + opus_frame
            total_opus_bytes += len(opus_frame)
            await send_tts_cb(opus_packet)

    # Stream LLM tokens
    async with httpx.AsyncClient(timeout=60.0) as client:
        async with client.stream(
            "POST",
            "https://openrouter.ai/api/v1/chat/completions",
            headers={"Authorization": f"Bearer {OPENROUTER_API_KEY}", "Content-Type": "application/json"},
            json={"model": model, "messages": messages, "max_tokens": LLM_MAX_TOKENS, "stream": True},
        ) as resp:
            first_token_time = None
            async for line in resp.aiter_lines():
                if not line.startswith("data: "):
                    continue
                payload = line[6:]
                if payload == "[DONE]":
                    break
                try:
                    chunk = json.loads(payload)
                    delta = chunk["choices"][0].get("delta", {})
                    content = delta.get("content", "")
                    if content:
                        if first_token_time is None:
                            first_token_time = time.time()
                            log.info(f"[LLM] First token at {first_token_time - llm_t0:.1f}s")
                        full_response += content
                        sentence_buf += content

                        # Flush on sentence boundaries
                        for delim in [". ", "! ", "? ", ".\n", "!\n", "?\n"]:
                            if delim in sentence_buf:
                                parts = sentence_buf.split(delim, 1)
                                sentence_to_speak = parts[0] + delim.strip()
                                sentence_buf = parts[1] if len(parts) > 1 else ""
                                log.info(f"[TTS] Flushing: \"{sentence_to_speak}\"")
                                await flush_tts(sentence_to_speak, is_last=False)
                                break
                except (json.JSONDecodeError, KeyError, IndexError):
                    continue

    # Flush remaining text
    if sentence_buf.strip():
        log.info(f"[TTS] Flushing final: \"{sentence_buf.strip()}\"")
        await flush_tts(sentence_buf.strip(), is_last=True)

    # Send Opus end marker
    await send_tts_cb(struct.pack('<H', 0))

    # Close TTS WebSocket
    if tts_ws:
        try:
            await tts_ws.close()
        except:
            pass

    # Update conversation
    conversation["history"].append({"role": "user", "content": transcript})
    conversation["history"].append({"role": "assistant", "content": full_response})
    conversation["last_time"] = time.time()

    total_time = time.time() - total_t0
    log.info(f"[LLM] Response: \"{full_response[:100]}\"")
    log.info(f"[TOTAL] {total_time:.1f}s | TTS: {total_tts_bytes:,} PCM → {total_opus_bytes:,} Opus ({total_tts_bytes/max(total_opus_bytes,1):.1f}:1)")


# Keep old batch function as fallback
async def process_query(pcm_data, is_pcm=True):
    total_t0 = time.time()
    if conversation["history"] and time.time() - conversation["last_time"] > CONVERSATION_TIMEOUT:
        conversation["history"] = []
    if is_pcm:
        transcript = await stt_transcribe(pcm_data)
    else:
        transcript = await stt_transcribe(ulaw_to_pcm(pcm_data))
    if not transcript.strip():
        transcript = "Hello"
    response_text = await llm_query(transcript, conversation["history"])
    conversation["history"].append({"role": "user", "content": transcript})
    conversation["history"].append({"role": "assistant", "content": response_text})
    conversation["last_time"] = time.time()
    tts_audio = await tts_synthesize(response_text)
    log.info(f"[TOTAL] {time.time()-total_t0:.1f}s pipeline")
    return tts_audio


# --- BLE Client ---

class HeliosClient:
    def __init__(self):
        self.client = None
        self.mic_buffer = bytearray()
        self.mic_expected = 0
        self.mic_receiving = False
        self.mic_opus_mode = False
        self.mic_opus_frames = 0
        self.opus_dec = None
        self.query_event = asyncio.Event()

    def _on_mic_notify(self, sender, data: bytearray):
        if len(data) == 0:
            # End of stream
            if self.mic_opus_mode and self.opus_dec:
                log.info(f"[MIC] Opus stream done: {self.mic_opus_frames} frames, "
                         f"{len(self.mic_buffer):,} bytes PCM ({len(self.mic_buffer)/2/STT_SAMPLE_RATE:.1f}s)")
            self.mic_receiving = False
            self.query_event.set()
            return

        if not self.mic_receiving:
            # Check for Opus stream marker (0xFFFFFFFF)
            if len(data) >= 4 and data[:4] == b'\xFF\xFF\xFF\xFF':
                self.mic_opus_mode = True
                self.mic_buffer = bytearray()
                self.mic_receiving = True
                self.mic_opus_frames = 0
                try:
                    import opuslib
                    self.opus_dec = opuslib.Decoder(STT_SAMPLE_RATE, 1)
                    log.info("[MIC] Opus stream started")
                except Exception as e:
                    log.error(f"[MIC] Opus decoder create failed: {e}")
                    self.opus_dec = None
                return
            # Legacy µ-law mode (4-byte size header)
            elif len(data) >= 4:
                self.mic_opus_mode = False
                self.opus_dec = None
                self.mic_expected = struct.unpack('<I', data[:4])[0]
                self.mic_buffer = bytearray()
                self.mic_receiving = True
                self.mic_buffer.extend(data[4:])
                log.info(f"[MIC] µ-law stream started: {self.mic_expected:,} bytes expected")
        else:
            if self.mic_opus_mode and self.opus_dec:
                # Decode Opus frames: [uint16_le len][opus data]
                offset = 0
                while offset + 2 <= len(data):
                    frame_len = data[offset] | (data[offset + 1] << 8)
                    offset += 2
                    if frame_len == 0 or offset + frame_len > len(data):
                        break
                    opus_frame = bytes(data[offset:offset + frame_len])
                    offset += frame_len
                    try:
                        pcm = self.opus_dec.decode(opus_frame, STT_SAMPLE_RATE // 50)
                        self.mic_buffer.extend(pcm)
                        self.mic_opus_frames += 1
                    except Exception as e:
                        log.warning(f"[MIC] Opus decode error: {e}")
            else:
                # Legacy µ-law: just accumulate raw bytes
                self.mic_buffer.extend(data)
                if len(self.mic_buffer) % 10240 < BLE_CHUNK_SIZE:
                    pct = len(self.mic_buffer) / self.mic_expected * 100 if self.mic_expected else 0
                    log.info(f"[MIC] {len(self.mic_buffer):,}/{self.mic_expected:,} bytes ({pct:.0f}%)")

    def _on_control_notify(self, sender, data: bytearray):
        if len(data) == 0:
            return
        cmd = data[0]
        if cmd == CMD_BUTTON_PRESSED:
            log.info("[ESP32] Button pressed — recording...")
        elif cmd == CMD_BUTTON_RELEASED:
            log.info("[ESP32] Button released — sending mic data...")
        elif cmd == CMD_PLAYBACK_DONE:
            log.info("[ESP32] Playback finished")

    async def connect(self):
        log.info("Scanning for Helios...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=30.0)
        if not device:
            log.error("Helios not found!")
            return False

        log.info(f"Found Helios at {device.address}, connecting...")
        self.client = BleakClient(device, timeout=30.0)
        try:
            await self.client.connect()
        except Exception as e:
            log.error(f"Connection failed: {e}")
            return False

        # Request high MTU
        if hasattr(self.client, 'mtu_size'):
            log.info(f"MTU: {self.client.mtu_size}")

        # List discovered services for debugging
        for svc in self.client.services:
            log.info(f"  Service: {svc.uuid}")
            for char in svc.characteristics:
                log.info(f"    Char: {char.uuid} props={char.properties}")

        try:
            await self.client.start_notify(MIC_TX_UUID, self._on_mic_notify)
            log.info("Subscribed to Mic TX notifications")
        except Exception as e:
            log.error(f"Mic notify subscribe failed: {e}")

        try:
            await self.client.start_notify(CONTROL_UUID, self._on_control_notify)
            log.info("Subscribed to Control notifications")
        except Exception as e:
            log.error(f"Control notify subscribe failed: {e}")

        try:
            await self.client.write_gatt_char(CONTROL_UUID, bytes([CMD_CONNECTED]), response=False)
        except Exception as e:
            log.error(f"Control write failed: {e}")

        log.info(f"Connected to {device.name} [{device.address}]")
        return True

    async def send_tts_start(self):
        """Send TTS stream start marker (unknown size)."""
        header = struct.pack('<I', 0xFFFFFFFF)  # streaming mode
        await self.client.write_gatt_char(SPEAKER_RX_UUID, header, response=False)
        await asyncio.sleep(0.01)
        self._tts_chunks_sent = 0
        self._tts_bytes_sent = 0

    async def send_tts_chunk(self, opus_data):
        """Send a chunk of Opus data (already length-prefixed frames)."""
        if not opus_data:
            return
        # Split into BLE-sized chunks if needed
        offset = 0
        while offset < len(opus_data):
            end = min(offset + BLE_CHUNK_SIZE, len(opus_data))
            chunk = opus_data[offset:end]
            await self.client.write_gatt_char(SPEAKER_RX_UUID, chunk, response=False)
            offset = end
            self._tts_chunks_sent += 1
            await asyncio.sleep(0.005)
        self._tts_bytes_sent += len(opus_data)

    async def send_tts_end(self):
        """Send TTS stream end marker."""
        await asyncio.sleep(0.05)
        await self.client.write_gatt_char(SPEAKER_RX_UUID, b'', response=False)
        log.info(f"[TX] Streamed {self._tts_bytes_sent:,} bytes Opus in {self._tts_chunks_sent} BLE chunks")

    async def send_tts(self, data):
        """Send pre-encoded TTS data (batch mode fallback)."""
        await self.send_tts_start()
        await self.send_tts_chunk(data)
        await self.send_tts_end()

    async def run(self):
        log.info("Waiting for button press...\n")

        while self.client.is_connected:
            self.query_event.clear()
            try:
                await asyncio.wait_for(self.query_event.wait(), timeout=1.0)
            except asyncio.TimeoutError:
                continue

            if len(self.mic_buffer) == 0:
                continue

            pcm_data = bytes(self.mic_buffer)
            dur = len(pcm_data) / 2 / STT_SAMPLE_RATE
            log.info(f"Received {len(pcm_data):,} bytes PCM ({dur:.1f}s)")

            # Signal processing
            await self.client.write_gatt_char(
                CONTROL_UUID, bytes([CMD_PROCESSING]), response=False)

            # Start TTS stream to ESP
            await self.send_tts_start()

            # Streaming callback: sends Opus chunks to ESP over BLE as they're generated
            async def on_tts_opus_chunk(opus_data):
                await self.send_tts_chunk(opus_data)

            # Run fully streaming pipeline: STT → LLM stream → TTS stream → BLE
            await process_query_streaming(
                pcm_data, is_pcm=self.mic_opus_mode, send_tts_cb=on_tts_opus_chunk)

            # End TTS stream
            await self.send_tts_end()

            log.info("Waiting for next button press...\n")


async def main():
    print()
    print("  ╔══════════════════════════════════════╗")
    print("  ║  HELIOS BLE Voice Server             ║")
    print("  ╚══════════════════════════════════════╝")
    print()

    if not CARTESIA_API_KEY:
        log.error("CARTESIA_API_KEY not set in .env")
    if not OPENROUTER_API_KEY:
        log.error("OPENROUTER_API_KEY not set in .env")

    print(f"  STT:   {STT_MODEL} @ {STT_SAMPLE_RATE}Hz")
    print(f"  LLM:   {LLM_MODEL}")
    print(f"  TTS:   {TTS_MODEL} @ {TTS_SAMPLE_RATE}Hz")
    print(f"  Voice: {TTS_VOICE_ID}")
    print()

    helios = HeliosClient()

    while True:
        try:
            if await helios.connect():
                await helios.run()
        except Exception as e:
            import traceback
            log.error(f"Error: {e}")
            traceback.print_exc()

        log.info("Reconnecting in 3 seconds...")
        await asyncio.sleep(3)
        helios = HeliosClient()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n\n  Bye!\n")
