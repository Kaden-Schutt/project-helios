"""
Helios BLE Protocol Tests
===========================
Validates ESP32 BLE GATT protocol after flashing.
Requires: ESP32 powered on, running Helios firmware, BLE advertising.

Run:  python tests/test_ble_protocol.py
"""

import asyncio, struct, time, sys, os

if os.path.exists("/opt/homebrew/lib"):
    os.environ.setdefault("DYLD_LIBRARY_PATH", "/opt/homebrew/lib")

from bleak import BleakClient, BleakScanner

# UUIDs must match firmware
SERVICE_UUID    = "87654321-4321-4321-4321-cba987654321"
MIC_TX_UUID     = "87654321-4321-4321-4321-cba987654322"
SPEAKER_RX_UUID = "87654321-4321-4321-4321-cba987654323"
CONTROL_UUID    = "87654321-4321-4321-4321-cba987654324"

DEVICE_NAME = "Helios"
CMD_CONNECTED = 0x01
CMD_PLAYBACK_DONE = 0x12

passed = 0
failed = 0


def report(name, ok, detail=""):
    global passed, failed
    if ok:
        print(f"  PASS: {name} {detail}")
        passed += 1
    else:
        print(f"  FAIL: {name} {detail}")
        failed += 1


class ProtocolTester:
    def __init__(self, client):
        self.client = client
        self.mic_notifications = []
        self.ctrl_notifications = []
        self.mic_event = asyncio.Event()
        self.ctrl_event = asyncio.Event()

    def _on_mic(self, sender, data: bytearray):
        self.mic_notifications.append(bytes(data))
        self.mic_event.set()

    def _on_ctrl(self, sender, data: bytearray):
        self.ctrl_notifications.append(bytes(data))
        self.ctrl_event.set()


async def run_tests():
    # --- Test 1: Discovery ---
    print("\n  TEST 1: BLE Discovery")
    device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15)
    report("Find Helios", device is not None,
           f"({device.address})" if device else "")
    if not device:
        print("  Cannot continue without device. Is ESP32 powered on?")
        return

    # Connect
    client = BleakClient(device, timeout=20)
    await client.connect()
    report("Connect", client.is_connected, f"MTU={client.mtu_size}")

    # Verify service
    svc = None
    for s in client.services:
        if s.uuid == SERVICE_UUID:
            svc = s
            break
    report("Service UUID", svc is not None)

    # Verify characteristics
    char_uuids = {c.uuid for s in client.services for c in s.characteristics}
    report("Mic TX characteristic", MIC_TX_UUID in char_uuids)
    report("Speaker RX characteristic", SPEAKER_RX_UUID in char_uuids)
    report("Control characteristic", CONTROL_UUID in char_uuids)

    tester = ProtocolTester(client)

    # --- Test 2: Subscribe + Control ---
    print("\n  TEST 2: Subscriptions + Control")
    await client.start_notify(MIC_TX_UUID, tester._on_mic)
    report("Subscribe Mic TX", True)
    await client.start_notify(CONTROL_UUID, tester._on_ctrl)
    report("Subscribe Control", True)

    await client.write_gatt_char(CONTROL_UUID, bytes([CMD_CONNECTED]), response=False)
    report("Write CONNECTED", True)

    # --- Test 3: JPEG + Opus receive (requires button press) ---
    print("\n  TEST 3: JPEG + Opus Receive")
    print("  >>> PRESS AND HOLD the button on the ESP32 for 2-3 seconds <<<")

    # Wait for mic notifications (JPEG marker should come first)
    jpeg_data = bytearray()
    jpeg_expected = 0
    opus_frames = 0
    opus_pcm = bytearray()
    phase = "waiting"  # waiting -> jpeg -> opus -> done

    timeout = 30  # seconds to wait for button press
    t0 = time.time()

    try:
        import opuslib
        opus_dec = opuslib.Decoder(16000, 1)
    except:
        opus_dec = None
        print("  (opuslib not available -- will skip Opus decode)")

    while time.time() - t0 < timeout:
        tester.mic_event.clear()
        try:
            await asyncio.wait_for(tester.mic_event.wait(), timeout=1.0)
        except asyncio.TimeoutError:
            continue

        while tester.mic_notifications:
            data = tester.mic_notifications.pop(0)

            if len(data) == 0:
                phase = "done"
                break

            if phase == "waiting":
                if len(data) >= 8 and data[:4] == b'\xFE\xFF\xFF\xFF':
                    jpeg_expected = struct.unpack('<I', data[4:8])[0]
                    phase = "jpeg"
                    print(f"  JPEG header: expecting {jpeg_expected:,} bytes")
                elif len(data) >= 4 and data[:4] == b'\xFF\xFF\xFF\xFF':
                    phase = "opus"
                    print("  Opus marker (no JPEG)")
            elif phase == "jpeg":
                if len(data) >= 4 and data[:4] == b'\xFF\xFF\xFF\xFF':
                    phase = "opus"
                    print(f"  JPEG complete: {len(jpeg_data):,}/{jpeg_expected:,} bytes")
                else:
                    jpeg_data.extend(data)
            elif phase == "opus":
                offset = 0
                while offset + 2 <= len(data):
                    fl = data[offset] | (data[offset+1] << 8)
                    offset += 2
                    if fl == 0 or offset + fl > len(data):
                        break
                    if opus_dec:
                        try:
                            pcm = opus_dec.decode(data[offset:offset+fl], 320)
                            opus_pcm.extend(pcm)
                        except:
                            pass
                    offset += fl
                    opus_frames += 1

        if phase == "done":
            break

    report("Received JPEG", len(jpeg_data) > 0 or phase in ("opus", "done"),
           f"{len(jpeg_data):,} bytes" if jpeg_data else "skipped")
    if jpeg_data:
        report("JPEG valid (SOI)", jpeg_data[:2] == b'\xff\xd8')
    report("Received Opus frames", opus_frames > 0, f"{opus_frames} frames")
    if opus_pcm:
        duration = len(opus_pcm) / 2 / 16000
        report("Opus decoded to PCM", len(opus_pcm) > 0, f"{duration:.1f}s")

    # --- Test 4: TTS Playback ---
    print("\n  TEST 4: TTS Playback")
    try:
        import opuslib
        import numpy as np

        # Generate 0.5s test tone and Opus encode
        sr = 24000
        t_arr = np.arange(sr // 2) / sr
        tone = (np.sin(2 * np.pi * 440 * t_arr) * 8000).astype(np.int16)
        enc = opuslib.Encoder(sr, 1, opuslib.APPLICATION_VOIP)
        frame_samples = sr // 50
        frame_bytes = frame_samples * 2

        opus_buf = bytearray()
        pcm_bytes = tone.tobytes()
        for i in range(0, len(pcm_bytes) - frame_bytes + 1, frame_bytes):
            frame = enc.encode(pcm_bytes[i:i+frame_bytes], frame_samples)
            opus_buf.extend(struct.pack('<H', len(frame)))
            opus_buf.extend(frame)
        opus_buf.extend(struct.pack('<H', 0))  # end marker

        # Send: size header + data + empty end
        total_len = len(opus_buf)
        header = struct.pack('<I', total_len)
        await client.write_gatt_char(SPEAKER_RX_UUID, header, response=False)
        await asyncio.sleep(0.01)

        offset = 0
        while offset < total_len:
            end = min(offset + 509, total_len)
            await client.write_gatt_char(SPEAKER_RX_UUID, opus_buf[offset:end], response=False)
            offset = end
            await asyncio.sleep(0.005)

        await asyncio.sleep(0.05)
        await client.write_gatt_char(SPEAKER_RX_UUID, b'', response=False)
        report("TTS data sent", True, f"{total_len} bytes Opus")

        # Wait for PLAYBACK_DONE
        tester.ctrl_notifications.clear()
        tester.ctrl_event.clear()
        playback_done = False
        for _ in range(20):  # 20s timeout
            try:
                await asyncio.wait_for(tester.ctrl_event.wait(), timeout=1.0)
                tester.ctrl_event.clear()
                for n in tester.ctrl_notifications:
                    if len(n) > 0 and n[0] == CMD_PLAYBACK_DONE:
                        playback_done = True
                if playback_done:
                    break
            except asyncio.TimeoutError:
                continue

        report("PLAYBACK_DONE received", playback_done)

    except ImportError:
        print("  SKIP: opuslib not available for TTS test")

    # --- Test 5: Repeat (state corruption check) ---
    print("\n  TEST 5: Repeat Query")
    print("  >>> PRESS AND HOLD the button again for 2-3 seconds <<<")

    tester.mic_notifications.clear()
    phase2 = "waiting"
    opus_frames_2 = 0
    t0 = time.time()

    while time.time() - t0 < timeout:
        tester.mic_event.clear()
        try:
            await asyncio.wait_for(tester.mic_event.wait(), timeout=1.0)
        except asyncio.TimeoutError:
            continue

        while tester.mic_notifications:
            data = tester.mic_notifications.pop(0)
            if len(data) == 0:
                phase2 = "done"
                break
            if phase2 == "waiting":
                if data[:4] in (b'\xFE\xFF\xFF\xFF', b'\xFF\xFF\xFF\xFF'):
                    phase2 = "streaming"
            elif phase2 == "streaming":
                if data[:4] == b'\xFF\xFF\xFF\xFF':
                    pass  # Opus marker within stream
                else:
                    opus_frames_2 += 1
        if phase2 == "done":
            break

    report("Second query received data", opus_frames_2 > 0 or phase2 == "done",
           f"{opus_frames_2} chunks")
    report("No state corruption", phase2 == "done")

    await client.disconnect()
    report("Clean disconnect", True)


async def main():
    print("\n  =======================================")
    print("  Helios BLE Protocol Tests")
    print("  =======================================")

    await run_tests()

    print(f"\n  =======================================")
    print(f"  Results: {passed} passed, {failed} failed")
    print(f"  =======================================\n")


if __name__ == "__main__":
    asyncio.run(main())
    sys.exit(1 if failed > 0 else 0)
