"""
Helios USB Receiver
====================
Listens on ESP32 USB serial, captures JPEG + PCM on button press,
saves to output/ and optionally forwards to the Helios server.

Protocol: ESP32 sends base64-encoded data as text lines:
    HELIOS_JPEG:<base64>\n
    HELIOS_PCM:<base64>\n
    HELIOS_END\n

Usage:
    python usb_receiver.py                  # save only
    python usb_receiver.py --send           # save + send to server
"""

import serial
import time
import os
import sys
import glob
import base64
import argparse
import requests
import numpy as np
import sounddevice as sd

OUTPUT_DIR = "output"
SERVER_URL = "http://localhost:5750/query"


def find_esp32_port():
    matches = glob.glob("/dev/cu.usbmodem*")
    return matches[0] if matches else None


def play_pcm(pcm_bytes, sample_rate=16000):
    if not pcm_bytes:
        return
    audio = np.frombuffer(pcm_bytes, dtype=np.int16)
    duration = len(audio) / sample_rate
    print(f"  Playing TTS response ({duration:.1f}s)...")
    sd.play(audio, samplerate=sample_rate)
    sd.wait()


def send_tts_to_esp32(ser, pcm_bytes):
    """Send TTS PCM audio back to ESP32 over serial."""
    if not pcm_bytes:
        print("  (no TTS audio to send)")
        return

    b64 = base64.b64encode(pcm_bytes).decode()
    line = f"HELIOS_TTS:{b64}\n"
    duration = len(pcm_bytes) / 2 / 16000
    print(f"  Sending TTS to ESP32: {len(pcm_bytes):,} bytes ({duration:.1f}s audio, {len(b64)//1024}KB b64)")
    ser.write(line.encode())
    ser.flush()
    print(f"  TTS sent to ESP32.")


def send_to_server(jpeg_path, pcm_path, ser=None):
    with open(jpeg_path, "rb") as f:
        image_b64 = base64.b64encode(f.read()).decode()
    with open(pcm_path, "rb") as f:
        audio_b64 = base64.b64encode(f.read()).decode()

    payload = {
        "image": image_b64,
        "audio": audio_b64,
        "timestamp": int(time.time()),
    }

    print(f"\n  Sending to server: image={len(image_b64)//1024}KB audio={len(audio_b64)//1024}KB")
    t0 = time.time()

    try:
        resp = requests.post(SERVER_URL, json=payload, timeout=60)
        elapsed = time.time() - t0
        resp.raise_for_status()

        transcript = resp.headers.get("X-Transcript", "")
        response_text = resp.headers.get("X-Response-Text", "")
        total_ms = resp.headers.get("X-Total-Ms", "?")

        print(f"\n  ┌─ Server Response ({elapsed:.1f}s round-trip, {total_ms}ms server)")
        print(f"  │ You said:  {transcript}")
        print(f"  │ AI says:   {response_text[:300]}")
        print(f"  │ Audio:     {len(resp.content):,} bytes PCM")
        print(f"  └─")

        tts_path = os.path.join(OUTPUT_DIR, "last_tts_response.pcm")
        with open(tts_path, "wb") as f:
            f.write(resp.content)

        # Send TTS audio back to ESP32 if serial connection provided
        if ser:
            send_tts_to_esp32(ser, resp.content)
        else:
            play_pcm(resp.content)

    except requests.exceptions.ConnectionError:
        print("  ERROR: Server not reachable. Start it with: python server.py")
    except Exception as e:
        print(f"  ERROR: {e}")


def read_long_line(ser, timeout=30):
    """Read a potentially very long line (base64 data can be >100KB)."""
    buf = b""
    deadline = time.time() + timeout
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if b"\n" in buf:
                line, _ = buf.split(b"\n", 1)
                return line.rstrip(b"\r")
        elif buf:
            # Small pause then retry
            time.sleep(0.01)
    return buf.rstrip(b"\r\n")


def main():
    parser = argparse.ArgumentParser(description="Helios USB Receiver")
    parser.add_argument("--send", action="store_true", help="Forward captures to Helios server")
    args = parser.parse_args()

    os.makedirs(OUTPUT_DIR, exist_ok=True)

    port = find_esp32_port()
    if not port:
        print("No ESP32 found. Plug it in and try again.")
        sys.exit(1)

    print(f"  Connected: {port}")
    print(f"  Save dir:  {OUTPUT_DIR}/")
    if args.send:
        print(f"  Server:    {SERVER_URL}")
    print(f"\n  Press the button on the ESP32 to capture...\n")

    ser = serial.Serial(port, 115200, timeout=0.5, dsrdtr=False, rtscts=False)
    ser.setDTR(False)
    ser.setRTS(False)
    ser.reset_input_buffer()
    capture_num = 0

    jpeg_data = None
    pcm_data = None

    try:
        while True:
            # Read line by line
            raw = ser.readline()
            if not raw:
                continue

            line = raw.rstrip(b"\r\n")

            if line.startswith(b"HELIOS_JPEG:"):
                b64_str = line[len(b"HELIOS_JPEG:"):]
                # The base64 line might be very long — keep reading if no \n yet
                while not raw.endswith(b"\n"):
                    more = ser.read(4096)
                    if more:
                        raw += more
                        b64_str += more.rstrip(b"\r\n")
                    else:
                        break

                try:
                    jpeg_data = base64.b64decode(b64_str)
                    print(f"  Received JPEG: {len(jpeg_data)} bytes")
                except Exception as e:
                    print(f"  ERROR decoding JPEG base64: {e}")
                    jpeg_data = None

            elif line.startswith(b"HELIOS_PCM:"):
                b64_str = line[len(b"HELIOS_PCM:"):]
                while not raw.endswith(b"\n"):
                    more = ser.read(4096)
                    if more:
                        raw += more
                        b64_str += more.rstrip(b"\r\n")
                    else:
                        break

                try:
                    pcm_data = base64.b64decode(b64_str)
                    print(f"  Received PCM:  {len(pcm_data)} bytes ({len(pcm_data)/32000:.1f}s)")
                except Exception as e:
                    print(f"  ERROR decoding PCM base64: {e}")
                    pcm_data = None

            elif line == b"HELIOS_END":
                capture_num += 1
                ts = time.strftime("%Y%m%d_%H%M%S")

                jpeg_path = None
                pcm_path = None

                if jpeg_data:
                    jpeg_path = os.path.join(OUTPUT_DIR, f"capture_{ts}.jpg")
                    with open(jpeg_path, "wb") as f:
                        f.write(jpeg_data)
                    soi = jpeg_data[:2] == b"\xff\xd8"
                    eoi = jpeg_data[-2:] == b"\xff\xd9"
                    print(f"  Saved: {jpeg_path} ({len(jpeg_data)} bytes, {'valid' if soi and eoi else 'INVALID'} JPEG)")

                if pcm_data:
                    pcm_path = os.path.join(OUTPUT_DIR, f"capture_{ts}.pcm")
                    with open(pcm_path, "wb") as f:
                        f.write(pcm_data)
                    print(f"  Saved: {pcm_path} ({len(pcm_data)} bytes)")

                if args.send and jpeg_path and pcm_path:
                    send_to_server(jpeg_path, pcm_path, ser=ser)

                jpeg_data = None
                pcm_data = None
                print(f"\n  Waiting for next button press...")

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
    main()
