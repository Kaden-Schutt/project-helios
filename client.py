"""
Helios Test Bench — ESP32 Simulator (Client)
==============================================
Simulates the XIAO ESP32S3 Sense pendant on your Mac:

  1. Loads a JPEG from ./input/ folder (the "camera")
  2. Hold SPACEBAR — records from your mic
  3. Release SPACEBAR — fires payload to server
  4. Plays back the TTS response through speakers

Run:  python client.py
Quit: Ctrl+C

Note: On macOS you may need to grant Terminal/IDE microphone and
      input monitoring permissions in System Settings > Privacy.
"""

import os
import sys
import glob
import base64
import time
import threading
import numpy as np
import requests

# sounddevice for mic recording and speaker playback
import sounddevice as sd

# pynput for spacebar detection (pip install pynput)
from pynput import keyboard

from dotenv import load_dotenv

load_dotenv()

SERVER_URL = os.getenv("HELIOS_SERVER_URL", "http://localhost:5750/query")
SAMPLE_RATE = 16000  # Must match server's STT_SAMPLE_RATE
CHANNELS = 1
DOUBLE_TAP_WINDOW = 0.5  # seconds — two taps within this = clear conversation
TAP_THRESHOLD = 0.25     # seconds — press shorter than this = tap, not recording


# ---------------------------------------------------------------------------
# Image loader — picks first JPEG from ./input/
# ---------------------------------------------------------------------------
def load_image() -> str:
    """Find and load the first JPEG in ./input/ as base64."""
    for ext in ("*.jpg", "*.jpeg", "*.JPG", "*.JPEG", "*.png", "*.PNG"):
        matches = glob.glob(os.path.join("input", ext))
        if matches:
            path = sorted(matches)[0]
            with open(path, "rb") as f:
                data = f.read()
            b64 = base64.b64encode(data).decode()
            size_kb = len(data) / 1024
            print(f"  Image: {path} ({size_kb:.0f}KB)")
            return b64

    print("\n  ERROR: No image files found in ./input/ folder.")
    print("  Drop a JPEG in there and try again.\n")
    sys.exit(1)


# ---------------------------------------------------------------------------
# Mic recorder — spacebar hold-to-talk
# ---------------------------------------------------------------------------
def wait_for_spacebar():
    """
    Wait for spacebar interaction. Returns either:
      ("record", pcm_bytes)  — user held spacebar to record
      ("clear", b"")         — user double-tapped spacebar to clear conversation
    """
    state = {"recording": False, "press_time": 0.0, "last_tap_time": 0.0}
    frames = []
    result = {"action": None, "audio": b""}

    def audio_callback(indata, frame_count, time_info, status):
        if status:
            print(f"  [mic] {status}")
        if state["recording"]:
            frames.append(indata.copy())

    stream = sd.InputStream(
        samplerate=SAMPLE_RATE,
        channels=CHANNELS,
        dtype="int16",
        callback=audio_callback,
        blocksize=1024,
    )
    stream.start()

    done = threading.Event()

    def on_press(key):
        if key == keyboard.Key.space and not state["recording"]:
            state["recording"] = True
            state["press_time"] = time.time()

    def on_release(key):
        if key == keyboard.Key.space and state["recording"]:
            state["recording"] = False
            hold_duration = time.time() - state["press_time"]

            if hold_duration < TAP_THRESHOLD:
                # Short tap — check for double-tap
                now = time.time()
                if state["last_tap_time"] and (now - state["last_tap_time"]) < DOUBLE_TAP_WINDOW:
                    result["action"] = "clear"
                    done.set()
                    return False
                state["last_tap_time"] = now
                # Wait briefly for possible second tap — don't stop listener yet
            else:
                # Real recording
                result["action"] = "record"
                done.set()
                return False

    print("\n  Hold SPACEBAR to record  |  Double-tap SPACEBAR to clear conversation")

    listener = keyboard.Listener(on_press=on_press, on_release=on_release)
    listener.start()

    # Wait for either a recording or double-tap, with a timeout for single taps
    while not done.is_set():
        done.wait(timeout=0.1)
        # If we had a single tap but no second tap came, ignore it
        if (state["last_tap_time"]
                and not state["recording"]
                and time.time() - state["last_tap_time"] > DOUBLE_TAP_WINDOW):
            state["last_tap_time"] = 0.0  # Reset — it was just a single tap

    listener.stop()
    stream.stop()
    stream.close()

    if result["action"] == "clear":
        return ("clear", b"")

    if not frames:
        return ("record", b"")

    audio = np.concatenate(frames)
    duration = len(audio) / SAMPLE_RATE
    print(f"  Captured {duration:.1f}s of audio")
    return ("record", audio.tobytes())


# ---------------------------------------------------------------------------
# Audio playback
# ---------------------------------------------------------------------------
def play_response(pcm_bytes: bytes, sample_rate: int = 16000):
    """Play raw PCM s16le audio through default output device."""
    if not pcm_bytes:
        print("  (no audio to play)")
        return

    audio = np.frombuffer(pcm_bytes, dtype=np.int16)
    duration = len(audio) / sample_rate
    print(f"  Playing response ({duration:.1f}s)...")
    sd.play(audio, samplerate=sample_rate)
    sd.wait()
    print("  Done.")


# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
def send_query(image_b64: str, audio_pcm: bytes):
    """POST payload to server, play response."""
    audio_b64 = base64.b64encode(audio_pcm).decode()

    payload = {
        "image": image_b64,
        "audio": audio_b64,
        "timestamp": int(time.time()),
    }

    img_kb = len(image_b64) // 1024
    aud_kb = len(audio_b64) // 1024
    print(f"\n  Sending to server: image={img_kb}KB  audio={aud_kb}KB")

    t0 = time.time()
    resp = requests.post(SERVER_URL, json=payload, timeout=60)
    elapsed = time.time() - t0
    resp.raise_for_status()

    # Read metadata from response headers
    transcript = resp.headers.get("X-Transcript", "")
    response_text = resp.headers.get("X-Response-Text", "")
    sample_rate = int(resp.headers.get("X-Sample-Rate", "16000"))
    total_ms = resp.headers.get("X-Total-Ms", "?")

    turns = resp.headers.get("X-Conversation-Turns", "?")

    print(f"\n  ┌─ Response ({elapsed:.1f}s round-trip, {total_ms}ms server)")
    print(f"  │ You said:  {transcript}")
    print(f"  │ AI says:   {response_text[:300]}")
    print(f"  │ Audio:     {len(resp.content):,} bytes PCM @ {sample_rate}Hz")
    print(f"  │ Conv turn: {turns}")
    print(f"  └─")

    play_response(resp.content, sample_rate)


def main():
    print()
    print("  ╔══════════════════════════════════════╗")
    print("  ║     HELIOS TEST BENCH                ║")
    print("  ║     ESP32 Simulator                  ║")
    print("  ╚══════════════════════════════════════╝")
    print(f"  Server: {SERVER_URL}")
    print()

    # Check server health
    try:
        r = requests.get(SERVER_URL.replace("/query", "/health"), timeout=5)
        health = r.json()
        print(f"  Server status: {health.get('status', '?')}")
        for k in ("cartesia_key", "anthropic_key"):
            v = health.get(k, "?")
            status = "✓" if v == "set" else "✗ MISSING"
            print(f"    {k}: {status}")
        print()
    except Exception:
        print("  WARNING: Server not reachable. Start it first:\n")
        print("    python server.py\n")

    # Load image once
    image_b64 = load_image()
    print()

    # Main interaction loop
    while True:
        try:
            action, pcm = wait_for_spacebar()

            if action == "clear":
                # Double-tap — clear conversation
                try:
                    clear_url = SERVER_URL.replace("/query", "/clear")
                    r = requests.post(clear_url, timeout=5)
                    data = r.json()
                    print(f"\n  Conversation cleared ({data.get('turns_cleared', '?')} turns)")
                except Exception:
                    print("\n  Conversation cleared (server unreachable)")
                continue

            if not pcm:
                print("  No audio captured — try again")
                continue

            send_query(image_b64, pcm)

        except requests.exceptions.ConnectionError:
            print("\n  ERROR: Can't reach server. Is it running?")
        except requests.exceptions.HTTPError as e:
            print(f"\n  ERROR: Server returned {e.response.status_code}")
            print(f"  {e.response.text[:200]}")
        except Exception as e:
            print(f"\n  ERROR: {e}")

        print("\n  ─── Ready for next query (Ctrl+C to quit) ───")


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n\n  Bye!\n")
