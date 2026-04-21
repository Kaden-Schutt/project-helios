"""
Always-on streaming — wake detection + query endpointing.

Design:
- Pendant holds a persistent WebSocket to Pi and streams PCM forever.
- Pi feeds that PCM into a persistent Cartesia Ink-Whisper STT WebSocket.
- Pi watches the rolling transcript for a fuzzy "hey helios" match.
- On wake: play earcon, switch to CAPTURING — accumulate words until a
  silence gap >= VAD_GAP_MS. The query text is everything after the wake
  phrase in the captured window.
- Call the caller-supplied `on_query(query_text)` coroutine; expect it to
  handle vision/settings pipeline + TTS side effects.

Cost / reliability:
- Streaming STT is billed per audio second. VAD gating on the pendant (only
  send audio when mic RMS above threshold) is the main cost mitigation.
- Server-side: any pendant-sent PCM stream is forwarded verbatim to Cartesia.

State machine:
  IDLE ────wake──► CAPTURING ────silence──► DRAIN (call on_query, back to IDLE)
             ▲
             └── cooldown (after TTS playback + tail) re-arms

Tune constants at top of file; they're public for operator override via
settings hooks later.
"""

from __future__ import annotations

import asyncio
import json
import logging
import re
import time
from typing import Awaitable, Callable, Optional

import websockets

log = logging.getLogger("helios.wake")

# --- Tunables ---
STT_MODEL         = "ink-whisper"
STT_SAMPLE_RATE   = 16000
STT_ENCODING      = "pcm_s16le"
CARTESIA_VERSION  = "2026-01-12"

VAD_GAP_MS        = 700        # silence gap that ends a capture
WAKE_COOLDOWN_S   = 1.5        # no second wake within this window
TTS_TAIL_S        = 0.5        # suppress wake for this long after TTS ends
CAPTURE_MAX_S     = 15.0       # hard cap on capture duration

# Fuzzy wake regex — tune on real transcripts. Matches "hey helios",
# "hey hilos", "hey hailey's", "hey elias", "ok helios", "ok, helios, ...", etc.
# Captures the text AFTER the phrase so we can use it as the query body.
_WAKE_RE = re.compile(
    r"\b(?:hey|ok|okay)\s*[,.]?\s+(?:h[aeiouy][a-z]*|eli[a-z]*)['s]?\s*[,.]?\s*(?P<body>.*)",
    re.IGNORECASE,
)

# Cleanup applied to captured body: drop a leading standalone "s" (mishear
# residue when Cartesia tokenizes "helios" as "hailey" + "s") and any leading
# punctuation.
_BODY_LEADING_S = re.compile(r"^s\b[\s,.;:]*", re.IGNORECASE)


def find_wake(transcript: str) -> Optional[str]:
    """If `transcript` contains the wake phrase, return the text that follows
    it (may be empty string if the wake was spoken alone). Returns None if no
    wake match."""
    m = _WAKE_RE.search(transcript or "")
    if not m:
        return None
    body = (m.group("body") or "").strip()
    body = _BODY_LEADING_S.sub("", body, count=1)
    body = body.lstrip(" ,.;:")
    return body


class WakeStream:
    """One persistent STT session driving one wake detector.

    Caller pushes mic PCM via `feed(pcm_bytes)` and is notified of completed
    queries via the `on_query` callback supplied to __init__.
    """

    def __init__(
        self,
        api_key: str,
        on_query: Callable[[str], Awaitable[None]],
        *,
        on_wake: Optional[Callable[[], Awaitable[None]]] = None,
        is_enabled: Callable[[], bool] = (lambda: True),
    ):
        self.api_key = api_key
        self.on_query = on_query
        self.on_wake = on_wake
        self.is_enabled = is_enabled

        self._stt_ws: Optional[websockets.WebSocketClientProtocol] = None
        self._running = False
        self._reader_task: Optional[asyncio.Task] = None

        # State
        self._state = "IDLE"              # IDLE | CAPTURING
        self._last_wake_ts = 0.0
        self._suppress_until = 0.0        # no wake before this monotonic ts
        self._capture_buf: list[str] = []
        self._capture_started_at = 0.0
        self._last_word_at = 0.0

    # --- lifecycle ---
    async def start(self):
        if self._running:
            return
        self._running = True
        await self._open_stt()
        self._reader_task = asyncio.create_task(self._read_loop())
        log.info("[WAKE] started")

    async def stop(self):
        self._running = False
        if self._reader_task:
            self._reader_task.cancel()
            try:
                await self._reader_task
            except (asyncio.CancelledError, Exception):
                pass
        if self._stt_ws:
            try:
                await self._stt_ws.close()
            except Exception:
                pass
        self._stt_ws = None
        log.info("[WAKE] stopped")

    def suppress_for(self, seconds: float):
        """Called by TTS layer while the Pi is talking so we don't self-wake
        from our own output bleeding into the mic."""
        self._suppress_until = max(self._suppress_until, time.monotonic() + seconds)

    # --- STT WS management ---
    async def _open_stt(self):
        uri = (
            f"wss://api.cartesia.ai/stt/websocket"
            f"?model={STT_MODEL}"
            f"&encoding={STT_ENCODING}"
            f"&sample_rate={STT_SAMPLE_RATE}"
            f"&language=en"
        )
        headers = {"X-API-Key": self.api_key, "Cartesia-Version": CARTESIA_VERSION}
        self._stt_ws = await websockets.connect(uri, extra_headers=headers)
        log.info("[WAKE] STT stream open")

    async def _reconnect_with_backoff(self):
        delay = 1.0
        while self._running:
            try:
                await self._open_stt()
                return
            except Exception as e:
                log.warning(f"[WAKE] STT reconnect failed: {e}; backing off {delay:.1f}s")
                await asyncio.sleep(delay)
                delay = min(delay * 2, 30.0)

    # --- audio in ---
    async def feed(self, pcm_bytes: bytes):
        if not self._stt_ws or not pcm_bytes:
            return
        if not self.is_enabled():
            return
        try:
            await self._stt_ws.send(pcm_bytes)
        except Exception as e:
            log.warning(f"[WAKE] STT send failed: {e}; reconnecting")
            await self._reconnect_with_backoff()

    # --- transcript in ---
    async def _read_loop(self):
        """Consume transcripts from Cartesia and drive the state machine."""
        while self._running:
            if not self._stt_ws:
                await asyncio.sleep(0.1)
                continue
            try:
                async for raw in self._stt_ws:
                    if not isinstance(raw, str):
                        continue
                    await self._on_transcript(json.loads(raw))
            except Exception as e:
                log.warning(f"[WAKE] STT read loop: {e}")
                await self._reconnect_with_backoff()

    async def _on_transcript(self, evt: dict):
        t = evt.get("type", "")
        if t == "error":
            log.error(f"[WAKE] STT error: {evt}")
            return
        if t != "transcript":
            return

        text = (evt.get("text") or "").strip()
        is_final = evt.get("is_final", False)
        if not text:
            return

        now = time.monotonic()

        # Suppress wake during TTS self-bleed window.
        if self._state == "IDLE" and now < self._suppress_until:
            return

        if self._state == "IDLE":
            body = find_wake(text)
            if body is None:
                return
            if now - self._last_wake_ts < WAKE_COOLDOWN_S:
                return
            self._last_wake_ts = now
            log.info(f"[WAKE] triggered — remaining body: {body!r}")
            self._state = "CAPTURING"
            self._capture_buf = [body] if body else []
            self._capture_started_at = now
            self._last_word_at = now
            if self.on_wake:
                asyncio.create_task(self.on_wake())
            return

        # CAPTURING — accumulate
        self._capture_buf.append(text)
        self._last_word_at = now

        # Natural endpoint if STT marks final + appreciable silence gap passes
        # (handled in watchdog below). Also, a hard cap prevents runaway.
        if is_final and self._should_end_capture(now):
            await self._finalize_capture()

    def _should_end_capture(self, now: float) -> bool:
        if self._state != "CAPTURING":
            return False
        if now - self._capture_started_at > CAPTURE_MAX_S:
            return True
        return (now - self._last_word_at) > (VAD_GAP_MS / 1000.0)

    async def tick(self):
        """Call every ~100ms from whoever owns the event loop to check for
        VAD silence timeouts even when Cartesia isn't sending us new events."""
        if self._state == "CAPTURING":
            now = time.monotonic()
            if self._should_end_capture(now):
                await self._finalize_capture()

    async def _finalize_capture(self):
        text = " ".join(s for s in self._capture_buf if s).strip()
        self._capture_buf = []
        self._state = "IDLE"
        log.info(f"[WAKE] capture → {text!r}")
        if text:
            try:
                await self.on_query(text)
            except Exception as e:
                log.error(f"[WAKE] on_query raised: {e}")
        else:
            log.info("[WAKE] capture empty — dropping")
