"""
Minimal usage example for helios_ble
=====================================
Shows how to connect to the Helios ESP32-S3, verify MTU=512, and wire up
notification callbacks + writes. This is a drop-in replacement for the
bleak transport layer in server_ble.py.

Run on the Pi (or OrangePi):
  source .venv/bin/activate
  python usage.py
"""

import time
import helios_ble

HELIOS_MAC = "90:70:69:13:01:C2"

# GATT UUIDs — must match the ESP32 firmware (ble.c)
SERVICE_UUID    = "87654321-4321-4321-4321-cba987654321"
MIC_TX_UUID     = "87654321-4321-4321-4321-cba987654322"  # ESP→Pi notify
SPEAKER_RX_UUID = "87654321-4321-4321-4321-cba987654323"  # Pi→ESP write
CONTROL_UUID    = "87654321-4321-4321-4321-cba987654324"  # bidirectional


def on_mic(uuid: str, data: bytes):
    """Called from the Rust reader task whenever a JPEG/Opus chunk arrives."""
    print(f"[MIC] {len(data)} bytes from {uuid[-4:]}")


def on_control(uuid: str, data: bytes):
    """Called on control-channel notifications (button press, playback done)."""
    if not data:
        return
    cmd = data[0]
    print(f"[CTRL] cmd=0x{cmd:02x} payload={data[1:]!r}")


def main():
    ble = helios_ble.HeliosBle()

    print(f"helios_ble min MTU: {helios_ble.MIN_MTU}")
    print(f"Connecting to {HELIOS_MAC}...")

    # Acquire write sockets for both chars we write to. This is what
    # triggers the MTU exchange — if the result is < 512, connect() raises.
    mtu = ble.connect(
        mac=HELIOS_MAC,
        write_chars=[SPEAKER_RX_UUID, CONTROL_UUID],
    )
    print(f"Connected. Negotiated MTU = {mtu}")

    # Subscribe to notifications. Callbacks run on the Rust reader task
    # but acquire the GIL to call into Python, so they're safe.
    ble.start_notify(MIC_TX_UUID, on_mic)
    ble.start_notify(CONTROL_UUID, on_control)

    # Send a CONNECTED control command (cmd 0x01, no payload).
    ble.write(CONTROL_UUID, bytes([0x01]))

    # Keep the process alive so the reader tasks can deliver notifications.
    try:
        while ble.is_connected():
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        print("Disconnecting...")
        ble.disconnect()


if __name__ == "__main__":
    main()
