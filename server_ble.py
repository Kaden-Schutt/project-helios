"""
Helios BLE Voice + Vision Server
==================================
Runs on the Raspberry Pi 4B (belt unit). Acts as a BLE central that connects
to the ESP32S3 Sense pendant (BLE peripheral named "Helios").

Pipeline (triggered by button press on pendant):
  1. ESP32 captures JPEG from camera + Opus audio from mic
  2. ESP32 sends multiplexed JPEG + Opus stream over BLE notifications
  3. This server demultiplexes the stream (see _on_mic_notify)
  4. Opus audio → PCM → Cartesia Ink-Whisper STT → text transcript
  5. Transcript + JPEG → Anthropic Claude Haiku (vision LLM) → response text
  6. Response text → Cartesia Sonic 3 TTS → PCM → Opus encode
  7. Opus frames sent back to ESP32 over BLE → speaker playback

BLE Protocol:
  - 4 GATT characteristics under one custom service
  - MIC_TX (notify): ESP32 → Pi, carries JPEG then Opus mic data
  - SPEAKER_RX (write): Pi → ESP32, carries Opus TTS audio
  - CONTROL (notify+write): bidirectional command channel
  - Multiplexing markers: 0xFFFFFFFE = JPEG start, 0xFFFFFFFF = Opus start

Dependencies: bleak, opuslib (requires libopus), httpx, websockets, anthropic
Environment: .env file with CARTESIA_API_KEY, ANTHROPIC_API_KEY

Run:    python server_ble.py
Debug:  python server_ble.py --debug
"""

import asyncio
import struct
import time
import os
import sys
import json
import logging
import base64
import numpy as np

if os.path.exists("/opt/homebrew/lib"):
    os.environ.setdefault("DYLD_LIBRARY_PATH", "/opt/homebrew/lib")

import argparse

from bleak import BleakClient, BleakScanner
from dotenv import load_dotenv
import httpx
import websockets
import anthropic

_parser = argparse.ArgumentParser(description="Helios BLE Voice + Vision Server")
_parser.add_argument('--debug', action='store_true', help="Enable verbose logging")
_args = _parser.parse_args()

load_dotenv()

# --- Config ---
# API keys loaded from .env file (never committed to git)
CARTESIA_API_KEY = os.getenv("CARTESIA_API_KEY", "")   # For STT + TTS
ANTHROPIC_API_KEY = os.getenv("ANTHROPIC_API_KEY", "")  # For Claude Haiku LLM

STT_MODEL = "ink-whisper"
STT_SAMPLE_RATE = 16000

TTS_MODEL = "sonic-3-2026-01-12"
TTS_VOICE_ID = os.getenv("TTS_VOICE_ID", "f786b574-daa5-4673-aa0c-cbe3e8534c02")
TTS_SAMPLE_RATE = 24000
CARTESIA_VERSION = "2026-01-12"

LLM_MODEL = "claude-haiku-4-5-20251001"
LLM_MAX_TOKENS = 150

SYSTEM_PROMPT = """You are an assistive AI in a wearable for a vision-impaired user. Your response is spoken aloud via TTS.

STRICT RULES:
- Maximum 3 sentences. Never exceed 3 sentences.
- Be direct. No filler, no hedging, no "I can see that..."
- Describe what matters most to the user's safety or question.
- If asked to identify something, name it immediately."""

# BLE UUIDs — must match the ESP32 firmware (ble.c)
SERVICE_UUID    = "87654321-4321-4321-4321-cba987654321"  # Main Helios service
MIC_TX_UUID     = "87654321-4321-4321-4321-cba987654322"  # ESP→Pi: JPEG + mic audio
SPEAKER_RX_UUID = "87654321-4321-4321-4321-cba987654323"  # Pi→ESP: TTS audio
CONTROL_UUID    = "87654321-4321-4321-4321-cba987654324"  # Bidirectional commands

DEVICE_NAME = "Helios"
BLE_CHUNK_SIZE = 509  # MTU 512 - 3 bytes ATT header = 509 bytes max payload

# Control commands (Pi → ESP32)
CMD_CONNECTED       = 0x01  # Pi has connected
CMD_PROCESSING      = 0x02  # Pi is running STT/LLM/TTS pipeline
CMD_SET_VOLUME      = 0x03  # Set speaker volume (payload: uint8 0-100)
CMD_ERROR           = 0x04  # Error message (payload: ASCII string)
CMD_REQUEST_STATUS  = 0x05  # Request device status report

# Control commands (ESP32 → Pi)
CMD_BUTTON_PRESSED  = 0x10  # User pressed the query button
CMD_BUTTON_RELEASED = 0x11  # User released the query button
CMD_PLAYBACK_DONE   = 0x12  # Speaker finished playing TTS
CMD_DEVICE_STATUS   = 0x13  # Status report (vol, heap, etc.)

CONVERSATION_TIMEOUT = 300  # Seconds before conversation history auto-clears

logging.basicConfig(
    level=logging.DEBUG if _args.debug else logging.WARNING,
    format="%(asctime)s [%(name)s] %(message)s",
    datefmt="%H:%M:%S",
)
logging.getLogger("bleak").setLevel(logging.WARNING)
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("httpcore").setLevel(logging.WARNING)
log = logging.getLogger("helios")

# --- Conversation ---
conversation = {"history": [], "last_time": 0.0}


# --- Opus encode ---
def pcm_to_opus(pcm_s16: bytes, sample_rate: int = 24000) -> bytes:
    """Encode s16le PCM to length-prefixed Opus frames for ESP32 playback."""
    try:
        import opuslib
    except ImportError:
        log.error("opuslib not installed! Run: pip install opuslib")
        return b""

    enc = opuslib.Encoder(sample_rate, 1, opuslib.APPLICATION_VOIP)
    frame_samples = sample_rate // 50  # 20ms frames
    frame_bytes = frame_samples * 2

    frames = bytearray()
    for i in range(0, len(pcm_s16) - frame_bytes + 1, frame_bytes):
        pcm_frame = pcm_s16[i:i + frame_bytes]
        opus_frame = enc.encode(pcm_frame, frame_samples)
        frames.extend(struct.pack('<H', len(opus_frame)))
        frames.extend(opus_frame)

    # End marker
    frames.extend(struct.pack('<H', 0))

    ratio = len(pcm_s16) / len(frames) if frames else 0
    log.info(f"Opus encode: {len(pcm_s16):,} bytes PCM -> {len(frames):,} bytes Opus ({ratio:.1f}:1)")
    return bytes(frames)


# --- STT (Cartesia streaming WebSocket) ---
async def stt_transcribe(audio_pcm: bytes) -> str:
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
    log.info(f"[STT] {time.time()-t0:.1f}s -- \"{transcript}\"")
    return transcript


# --- LLM (Anthropic Claude Haiku) ---
async def llm_query(transcript: str, history: list, jpeg_b64: str = None) -> str:
    log.info(f"[LLM] \"{transcript[:80]}\" (image={'yes' if jpeg_b64 else 'no'})")
    t0 = time.time()

    client = anthropic.Anthropic(api_key=ANTHROPIC_API_KEY)

    # Build user message content
    if jpeg_b64:
        user_content = [
            {"type": "text", "text": transcript},
            {"type": "image", "source": {
                "type": "base64", "media_type": "image/jpeg", "data": jpeg_b64}},
        ]
    else:
        user_content = transcript

    messages = history + [{"role": "user", "content": user_content}]

    try:
        response = client.messages.create(
            model=LLM_MODEL,
            max_tokens=LLM_MAX_TOKENS,
            system=SYSTEM_PROMPT,
            messages=messages,
        )
        result = response.content[0].text
    except Exception as e:
        log.error(f"[LLM] Error: {e}")
        return "Sorry, I couldn't process that."

    log.info(f"[LLM] {time.time()-t0:.1f}s -- \"{result[:100]}\"")
    return result


# --- TTS (Cartesia batch) ---
async def tts_synthesize(text: str) -> bytes:
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
    log.info(f"[TTS] {time.time()-t0:.1f}s -- {len(resp.content):,} bytes ({duration:.1f}s)")
    return resp.content


# --- TTS Streaming (Cartesia WebSocket → Opus → BLE) ---
# Watermark: accumulate this many bytes of Opus before starting BLE transmission.
# This buffers ~300ms of audio so the ESP32 speaker doesn't starve waiting for
# the next BLE packet. Balances latency vs. smooth playback.
TTS_STREAM_WATERMARK = 900

async def tts_stream_and_send(text: str, helios: "HeliosClient"):
    """Stream TTS: Cartesia WebSocket → Opus encode → 300ms buffer → BLE stream."""
    import uuid
    try:
        import opuslib
    except ImportError:
        log.error("opuslib not installed! Falling back to batch TTS")
        tts_pcm = await tts_synthesize(text)
        if tts_pcm:
            opus_data = pcm_to_opus(tts_pcm, TTS_SAMPLE_RATE)
            await helios.send_tts(opus_data)
        return

    log.info(f"[TTS-STREAM] Synthesizing {len(text)} chars via WebSocket")
    t0 = time.time()

    uri = (f"wss://api.cartesia.ai/tts/websocket"
           f"?api_key={CARTESIA_API_KEY}"
           f"&cartesia_version={CARTESIA_VERSION}")

    context_id = str(uuid.uuid4())
    request_msg = json.dumps({
        "model_id": TTS_MODEL,
        "transcript": text,
        "voice": {"mode": "id", "id": TTS_VOICE_ID},
        "output_format": {
            "container": "raw",
            "encoding": "pcm_s16le",
            "sample_rate": TTS_SAMPLE_RATE,
        },
        "context_id": context_id,
        "language": "en",
    })

    enc = opuslib.Encoder(TTS_SAMPLE_RATE, 1, opuslib.APPLICATION_VOIP)
    frame_samples = TTS_SAMPLE_RATE // 50  # 20ms = 480 samples at 24kHz
    frame_bytes = frame_samples * 2        # 960 bytes per frame

    pcm_accum = bytearray()
    opus_buf = bytearray()
    streaming = False
    total_pcm = 0
    total_opus_sent = 0

    async with helios.ble_write_lock:
        try:
            async with websockets.connect(uri) as ws:
                await ws.send(request_msg)
                log.info(f"[TTS-STREAM] Request sent (context={context_id[:8]})")

                async for msg in ws:
                    if not isinstance(msg, str):
                        continue

                    data = json.loads(msg)
                    msg_type = data.get("type", "")

                    if msg_type == "chunk":
                        # Decode base64 PCM chunk
                        pcm_chunk = base64.b64decode(data.get("data", ""))
                        pcm_accum.extend(pcm_chunk)
                        total_pcm += len(pcm_chunk)

                        # Encode complete 20ms frames to Opus
                        while len(pcm_accum) >= frame_bytes:
                            pcm_frame = bytes(pcm_accum[:frame_bytes])
                            del pcm_accum[:frame_bytes]
                            opus_frame = enc.encode(pcm_frame, frame_samples)
                            opus_buf.extend(struct.pack('<H', len(opus_frame)))
                            opus_buf.extend(opus_frame)

                        # Watermark: start streaming over BLE
                        if not streaming and len(opus_buf) >= TTS_STREAM_WATERMARK:
                            streaming = True
                            log.info(f"[TTS-STREAM] Watermark reached ({len(opus_buf)} bytes), "
                                     f"starting BLE stream")
                            # Send start marker
                            await helios.client.write_gatt_char(
                                SPEAKER_RX_UUID, b'\xFF\xFF\xFF\xFF', response=True)
                            # Send buffered data
                            offset = 0
                            while offset < len(opus_buf):
                                end = min(offset + BLE_CHUNK_SIZE, len(opus_buf))
                                await helios.client.write_gatt_char(
                                    SPEAKER_RX_UUID, bytes(opus_buf[offset:end]), response=True)
                                offset = end
                            total_opus_sent += len(opus_buf)
                            opus_buf.clear()

                        # In streaming mode, send new opus data as it arrives
                        elif streaming and len(opus_buf) > 0:
                            offset = 0
                            while offset < len(opus_buf):
                                end = min(offset + BLE_CHUNK_SIZE, len(opus_buf))
                                await helios.client.write_gatt_char(
                                    SPEAKER_RX_UUID, bytes(opus_buf[offset:end]), response=True)
                                offset = end
                            total_opus_sent += len(opus_buf)
                            opus_buf.clear()

                    elif msg_type == "done":
                        break

                    elif msg_type == "error":
                        log.error(f"[TTS-STREAM] Cartesia error: {data}")
                        break

        except Exception as e:
            log.error(f"[TTS-STREAM] WebSocket error: {e}")
            if not streaming:
                # Fallback to batch if streaming never started
                log.info("[TTS-STREAM] Falling back to batch TTS")
                tts_pcm = await tts_synthesize(text)
                if tts_pcm:
                    opus_data = pcm_to_opus(tts_pcm, TTS_SAMPLE_RATE)
                    await helios.send_tts(opus_data)
                return

        # Encode any remaining PCM (pad to full frame)
        if len(pcm_accum) > 0:
            pcm_accum.extend(b'\x00' * (frame_bytes - len(pcm_accum)))
            opus_frame = enc.encode(bytes(pcm_accum), frame_samples)
            opus_buf.extend(struct.pack('<H', len(opus_frame)))
            opus_buf.extend(opus_frame)

        # Append Opus end marker
        opus_buf.extend(struct.pack('<H', 0))

        if not streaming:
            # Never reached watermark — send everything now
            await helios.client.write_gatt_char(
                SPEAKER_RX_UUID, b'\xFF\xFF\xFF\xFF', response=True)

        # Send remaining data
        if len(opus_buf) > 0:
            offset = 0
            while offset < len(opus_buf):
                end = min(offset + BLE_CHUNK_SIZE, len(opus_buf))
                await helios.client.write_gatt_char(
                    SPEAKER_RX_UUID, bytes(opus_buf[offset:end]), response=True)
                offset = end
            total_opus_sent += len(opus_buf)

        # Empty end marker
        await asyncio.sleep(0.02)
        await helios.client.write_gatt_char(SPEAKER_RX_UUID, b'', response=True)

    elapsed = time.time() - t0
    duration = total_pcm / 2 / TTS_SAMPLE_RATE
    log.info(f"[TTS-STREAM] Done in {elapsed:.1f}s — "
             f"{total_pcm:,} bytes PCM ({duration:.1f}s), "
             f"{total_opus_sent:,} bytes Opus sent")


# --- BLE Client ---

class HeliosClient:
    def __init__(self):
        self.client = None
        self.ble_write_lock = asyncio.Lock()
        # Mic stream state
        self.mic_buffer = bytearray()
        self.mic_receiving = False
        self.mic_opus_mode = False
        self.mic_opus_frames = 0
        self.opus_dec = None
        self.query_event = asyncio.Event()
        # JPEG demux state
        self.jpeg_buffer = None
        self.jpeg_expected = 0
        self.jpeg_receiving = False
        self.jpeg_b64 = None
        # STT streaming
        self.stt_ws = None
        self.stt_transcript_parts = []
        self.stt_transcript_ready = asyncio.Event()
        self.stt_open_done = asyncio.Event()  # set when connect attempt finishes
        self.stt_open_done.set()  # start as "done" (no pending open)
        self.stt_reader_task = None
        self.record_start_time = None

    async def _open_stt_stream(self):
        """Open STT WebSocket. Signals stt_open_done when attempt finishes."""
        uri = (f"wss://api.cartesia.ai/stt/websocket"
               f"?model={STT_MODEL}&encoding=pcm_s16le&sample_rate={STT_SAMPLE_RATE}&language=en")
        headers = {"X-API-Key": CARTESIA_API_KEY, "Cartesia-Version": CARTESIA_VERSION}
        self.stt_transcript_parts = []
        self.stt_transcript_ready.clear()
        self.stt_open_done.clear()
        try:
            self.stt_ws = await websockets.connect(uri, additional_headers=headers)
            self.stt_reader_task = asyncio.create_task(self._stt_reader())
            log.info("[STT] WebSocket opened for streaming")
        except Exception as e:
            log.error(f"[STT] WebSocket open failed: {e}")
            self.stt_ws = None
            self.stt_transcript_ready.set()  # unblock _finalize_stt
        finally:
            self.stt_open_done.set()  # unblock _finalize_stt's wait

    async def _stt_reader(self):
        """Background task: read STT transcript results."""
        try:
            async for msg in self.stt_ws:
                if isinstance(msg, str):
                    data = json.loads(msg)
                    if data.get("type") == "transcript" and data.get("is_final"):
                        text = data.get("text", "").strip()
                        if text:
                            self.stt_transcript_parts.append(text)
                    elif data.get("type") in ("done", "flush_done"):
                        break
                    elif data.get("type") == "error":
                        log.error(f"[STT] Error: {data}")
                        break
        except Exception as e:
            log.error(f"[STT] Reader error: {e}")
        finally:
            self.stt_transcript_ready.set()

    async def _finalize_stt(self) -> str:
        # Wait for _open_stt_stream to finish connecting (or failing)
        await self.stt_open_done.wait()
        if self.stt_ws:
            # Send complete mic buffer to STT, then close
            audio = bytes(self.mic_buffer)
            if audio:
                log.info(f"[STT] Sending {len(audio):,} bytes to STT")
                try:
                    for i in range(0, len(audio), 8192):
                        await self.stt_ws.send(audio[i:i + 8192])
                except Exception:
                    pass
            try:
                await self.stt_ws.send("done")
            except Exception:
                pass
        await self.stt_transcript_ready.wait()
        if self.stt_ws:
            try:
                await self.stt_ws.close()
            except Exception:
                pass
            self.stt_ws = None
        return " ".join(self.stt_transcript_parts)

    def _on_mic_notify(self, sender, data: bytearray):
        """Demux multiplexed stream: JPEG (0xFFFFFFFE) then Opus (0xFFFFFFFF)."""
        # Empty = end of stream
        if len(data) == 0:
            if self.mic_opus_mode and self.opus_dec:
                log.info(f"[MIC] Opus done: {self.mic_opus_frames} frames, "
                         f"{len(self.mic_buffer):,} bytes PCM")
            self.mic_receiving = False
            self.mic_opus_mode = False
            self.query_event.set()
            return

        # --- JPEG receiving mode ---
        if self.jpeg_receiving:
            self.jpeg_buffer.extend(data)
            if len(self.jpeg_buffer) >= self.jpeg_expected:
                # JPEG complete — auto-enter Opus mode (marker may be dropped)
                jpeg_data = bytes(self.jpeg_buffer[:self.jpeg_expected])
                self.jpeg_b64 = base64.b64encode(jpeg_data).decode()
                valid = jpeg_data[:2] == b'\xff\xd8'
                log.info(f"[JPEG] Complete: {len(jpeg_data):,} bytes (valid={valid})")
                self.jpeg_receiving = False
                self.jpeg_buffer = None
                # Pre-enter Opus mode so we don't need the marker
                self.mic_opus_mode = True
                self.mic_buffer = bytearray()
                self.mic_receiving = True
                self.mic_opus_frames = 0
                try:
                    import opuslib
                    self.opus_dec = opuslib.Decoder(STT_SAMPLE_RATE, 1)
                except Exception as e:
                    log.error(f"[MIC] Opus decoder failed: {e}")
                    self.opus_dec = None
            return

        # --- Detect markers (when not in JPEG or Opus mode) ---
        if not self.mic_receiving:
            # JPEG start: 0xFFFFFFFE + uint32_le length
            if len(data) >= 8 and data[:4] == b'\xFE\xFF\xFF\xFF':
                self.jpeg_expected = struct.unpack('<I', data[4:8])[0]
                self.jpeg_buffer = bytearray()
                self.jpeg_receiving = True
                self.jpeg_b64 = None
                log.info(f"[JPEG] Receiving {self.jpeg_expected:,} bytes...")
                return

            # Opus start: 0xFFFFFFFF
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
                    log.error(f"[MIC] Opus decoder failed: {e}")
                    self.opus_dec = None
                return

        # --- Opus data ---
        if self.mic_opus_mode and self.opus_dec:
            # Skip redundant Opus start marker if it wasn't dropped
            if len(data) == 4 and data[:4] == b'\xFF\xFF\xFF\xFF':
                return
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

    def _on_control_notify(self, sender, data: bytearray):
        if len(data) == 0:
            return
        cmd = data[0]
        if cmd == CMD_BUTTON_PRESSED:
            log.info("[ESP32] Button pressed -- recording...")
            self.record_start_time = time.time()
            self.jpeg_b64 = None
            asyncio.get_running_loop().create_task(self._open_stt_stream())
        elif cmd == CMD_BUTTON_RELEASED:
            log.info("[ESP32] Button released")
            # Fallback: if mic stream never sent end marker, trigger query now
            if not self.query_event.is_set():
                log.warning("[ESP32] No mic end marker received — triggering query from BUTTON_RELEASED")
                self.mic_receiving = False
                self.mic_opus_mode = False
                self.query_event.set()
        elif cmd == CMD_PLAYBACK_DONE:
            log.info("[ESP32] Playback finished")
        elif cmd == CMD_DEVICE_STATUS:
            if len(data) >= 4:  # cmd byte + 3 payload bytes
                vol = data[1]
                heap_kb = data[2] | (data[3] << 8)
                print(f"  [ESP32] Volume: {vol}%, Heap: {heap_kb}KB, BLE: connected")

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

        try:
            await self.client.start_notify(MIC_TX_UUID, self._on_mic_notify)
            log.info("Subscribed to Mic TX")
        except Exception as e:
            log.error(f"Mic notify subscribe failed: {e}")

        try:
            await self.client.start_notify(CONTROL_UUID, self._on_control_notify)
            log.info("Subscribed to Control")
        except Exception as e:
            log.error(f"Control notify subscribe failed: {e}")

        # On bluez, MTU is negotiated when AcquireNotify/AcquireWrite happens
        # (i.e. after start_notify). Check it now.
        mtu = self.client.mtu_size if hasattr(self.client, 'mtu_size') else 23
        log.info(f"MTU: {mtu}")
        if mtu < 200:
            log.warning(f"MTU is only {mtu} — trying explicit request")
            try:
                # Force an ATT Exchange MTU Request by writing to a characteristic
                await self.client.write_gatt_char(CONTROL_UUID, bytes([CMD_CONNECTED]), response=True)
                await asyncio.sleep(0.5)
                mtu = self.client.mtu_size if hasattr(self.client, 'mtu_size') else 23
                log.info(f"MTU after write-with-response: {mtu}")
            except Exception as e:
                log.warning(f"MTU request failed: {e}")

        log.info(f"Connected to {device.name} [{device.address}]")
        return True

    async def send_tts(self, opus_data: bytes):
        """Send Opus TTS data to ESP: start marker + data chunks + empty end."""
        async with self.ble_write_lock:
            total_len = len(opus_data)

            # Start marker (mirrors mic→Pi Opus protocol)
            header = b'\xFF\xFF\xFF\xFF'
            await self.client.write_gatt_char(SPEAKER_RX_UUID, header, response=True)

            # Data chunks — response=True guarantees delivery (no silent drops)
            offset = 0
            chunks = 0
            while offset < total_len:
                end = min(offset + BLE_CHUNK_SIZE, total_len)
                chunk = opus_data[offset:end]
                await self.client.write_gatt_char(SPEAKER_RX_UUID, chunk, response=True)
                offset = end
                chunks += 1

            # Empty end marker
            await asyncio.sleep(0.02)
            await self.client.write_gatt_char(SPEAKER_RX_UUID, b'', response=True)
            log.info(f"[TX] Sent {total_len:,} bytes Opus in {chunks} BLE chunks")

    async def cli_loop(self):
        """Read CLI commands from stdin."""
        loop = asyncio.get_running_loop()
        reader = asyncio.StreamReader()
        protocol = asyncio.StreamReaderProtocol(reader)
        await loop.connect_read_pipe(lambda: protocol, sys.stdin)

        while self.client and self.client.is_connected:
            try:
                line = await asyncio.wait_for(reader.readline(), timeout=1.0)
            except asyncio.TimeoutError:
                continue
            if not line:
                break
            cmd = line.decode().strip().lower()
            if not cmd:
                continue

            if cmd.startswith("vol "):
                try:
                    val = int(cmd.split()[1])
                    val = max(0, min(100, val))
                    async with self.ble_write_lock:
                        await self.client.write_gatt_char(
                            CONTROL_UUID, bytes([CMD_SET_VOLUME, val]), response=False)
                    print(f"  [SET] Volume -> {val}%")
                except (ValueError, IndexError):
                    print("  Usage: vol <0-100>")

            elif cmd == "status":
                async with self.ble_write_lock:
                    await self.client.write_gatt_char(
                        CONTROL_UUID, bytes([CMD_REQUEST_STATUS]), response=False)

            elif cmd == "clear":
                turns = len([m for m in conversation["history"] if m["role"] == "user"])
                conversation["history"] = []
                conversation["last_time"] = 0.0
                print(f"  [CONV] Cleared {turns} turns")

            elif cmd == "help":
                print("  Commands: vol <0-100>, status, clear, help")

            else:
                print(f"  Unknown command: {cmd}. Type 'help' for commands.")

    async def run(self):
        log.info("Waiting for button press...\n")
        cli_task = asyncio.create_task(self.cli_loop())

        try:
            while self.client.is_connected:
                self.query_event.clear()
                try:
                    await asyncio.wait_for(self.query_event.wait(), timeout=1.0)
                except asyncio.TimeoutError:
                    continue

                if len(self.mic_buffer) == 0:
                    continue

                t0 = time.time()
                dur = len(self.mic_buffer) / 2 / STT_SAMPLE_RATE
                log.info(f"Query: {len(self.mic_buffer):,} bytes PCM ({dur:.1f}s), "
                         f"JPEG={'yes' if self.jpeg_b64 else 'no'}")

                # Finalize STT
                transcript = await self._finalize_stt()
                stt_time = time.time() - t0
                if not transcript.strip():
                    transcript = "Hello"
                    log.warning(f"[STT] Empty, fallback: \"{transcript}\"")
                else:
                    log.info(f"[STT] {stt_time:.1f}s: \"{transcript}\"")

                # Signal processing
                async with self.ble_write_lock:
                    await self.client.write_gatt_char(
                        CONTROL_UUID, bytes([CMD_PROCESSING]), response=False)

                # Conversation timeout
                if conversation["history"] and time.time() - conversation["last_time"] > CONVERSATION_TIMEOUT:
                    log.info("[CONV] Expired -- clearing")
                    conversation["history"] = []

                # LLM query (with optional JPEG)
                response_text = await llm_query(
                    transcript, conversation["history"], self.jpeg_b64)

                # Update conversation
                # One-shot: no conversation history for now
                conversation["history"] = []
                conversation["last_time"] = time.time()

                # TTS → Opus → BLE (streaming)
                await tts_stream_and_send(response_text, self)

                total = time.time() - t0
                log.info(f"[TOTAL] {total:.1f}s | \"{response_text[:60]}\"")
                log.info("Waiting for next button press...\n")
        finally:
            cli_task.cancel()


async def main():
    print()
    print("  +======================================+")
    print("  |  HELIOS BLE Voice + Vision Server    |")
    print("  +======================================+")
    print()

    if not CARTESIA_API_KEY:
        log.error("CARTESIA_API_KEY not set in .env")
    if not ANTHROPIC_API_KEY:
        log.error("ANTHROPIC_API_KEY not set in .env")

    print(f"  STT:   {STT_MODEL} @ {STT_SAMPLE_RATE}Hz")
    print(f"  LLM:   {LLM_MODEL}")
    print(f"  TTS:   {TTS_MODEL} @ {TTS_SAMPLE_RATE}Hz")
    print(f"  Voice: {TTS_VOICE_ID}")
    print(f"  Type 'help' for commands.\n")

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
