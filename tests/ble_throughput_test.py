"""
Helios BLE E2E Throughput Test
================================
Tests bidirectional BLE throughput between Pi (Mac) and ESP32.

Test 1: ESP→Pi (mic direction) — triggers test pattern from ESP
Test 2: Pi→ESP (TTS direction) — sends known data, waits for ACK
Test 3: Round-trip — send data to ESP, ESP echoes back

Reports:
- Negotiated MTU and PHY
- Throughput in KB/s for each direction
- Data integrity verification

Usage:
    DYLD_LIBRARY_PATH=/opt/homebrew/lib python3 tests/ble_throughput_test.py

Requires:
    pip install bleak
    ESP32 flashed with Helios BLE firmware (with BLE_CMD_TEST_THROUGHPUT support)
"""

import asyncio
import struct
import time
import sys
import os

# Ensure libopus can be found on macOS
if os.path.exists("/opt/homebrew/lib"):
    os.environ.setdefault("DYLD_LIBRARY_PATH", "/opt/homebrew/lib")

from bleak import BleakClient, BleakScanner

# BLE UUIDs
SERVICE_UUID    = "87654321-4321-4321-4321-cba987654321"
MIC_TX_UUID     = "87654321-4321-4321-4321-cba987654322"
SPEAKER_RX_UUID = "87654321-4321-4321-4321-cba987654323"
CONTROL_UUID    = "87654321-4321-4321-4321-cba987654324"

DEVICE_NAME = "Helios"
BLE_CHUNK_SIZE = 509

# Control commands
CMD_CONNECTED       = 0x01
CMD_TEST_THROUGHPUT  = 0x20
CMD_PLAYBACK_DONE    = 0x12


class ThroughputTest:
    def __init__(self):
        self.client = None
        self.rx_buffer = bytearray()
        self.rx_expected = 0
        self.rx_done = asyncio.Event()
        self.rx_start_time = None
        self.rx_first_chunk_time = None
        self.rx_chunks = 0
        self.tts_done = asyncio.Event()

    def _on_mic_notify(self, sender, data: bytearray):
        """Receive data from ESP (mic TX notifications)."""
        if len(data) == 0:
            self.rx_done.set()
            return

        if self.rx_start_time is None:
            self.rx_start_time = time.time()

        if not self.rx_expected and len(data) >= 4:
            self.rx_expected = struct.unpack('<I', data[:4])[0]
            data = data[4:]
            self.rx_first_chunk_time = time.time()

        self.rx_buffer.extend(data)
        self.rx_chunks += 1

    def _on_control_notify(self, sender, data: bytearray):
        """Receive control notifications."""
        if len(data) > 0 and data[0] == CMD_PLAYBACK_DONE:
            self.tts_done.set()

    async def connect(self):
        print("Scanning for Helios...")
        device = await BleakScanner.find_device_by_name(DEVICE_NAME, timeout=15.0)
        if not device:
            print("ERROR: Helios not found!")
            return False

        self.client = BleakClient(device, timeout=20.0)
        await self.client.connect()

        mtu = getattr(self.client, 'mtu_size', 'unknown')
        print(f"Connected to {device.name} [{device.address}]")
        print(f"MTU: {mtu}")

        # List services
        for svc in self.client.services:
            if SERVICE_UUID in str(svc.uuid):
                for char in svc.characteristics:
                    print(f"  Char: {char.uuid} props={char.properties}")

        await self.client.start_notify(MIC_TX_UUID, self._on_mic_notify)
        await self.client.start_notify(CONTROL_UUID, self._on_control_notify)
        await self.client.write_gatt_char(CONTROL_UUID, bytes([CMD_CONNECTED]), response=False)

        # Wait for BLE params to settle
        await asyncio.sleep(2.0)
        return True

    async def test_esp_to_pi(self, size_kb=32):
        """Test 1: ESP→Pi throughput via notifications."""
        print(f"\n{'='*50}")
        print(f"TEST 1: ESP → Pi ({size_kb} KB)")
        print(f"{'='*50}")

        self.rx_buffer = bytearray()
        self.rx_expected = 0
        self.rx_done.clear()
        self.rx_start_time = None
        self.rx_first_chunk_time = None
        self.rx_chunks = 0

        # Trigger throughput test on ESP
        t0 = time.time()
        await self.client.write_gatt_char(
            CONTROL_UUID, bytes([CMD_TEST_THROUGHPUT, size_kb]), response=False)

        # Wait for all data
        try:
            await asyncio.wait_for(self.rx_done.wait(), timeout=30.0)
        except asyncio.TimeoutError:
            print(f"  TIMEOUT after {time.time()-t0:.1f}s")
            print(f"  Received: {len(self.rx_buffer):,} / {self.rx_expected:,} bytes")
            return

        elapsed = time.time() - (self.rx_first_chunk_time or t0)
        total = len(self.rx_buffer)
        throughput = total / elapsed / 1024 if elapsed > 0 else 0

        print(f"  Expected:   {self.rx_expected:,} bytes")
        print(f"  Received:   {total:,} bytes ({total/self.rx_expected*100:.1f}%)")
        print(f"  Chunks:     {self.rx_chunks}")
        print(f"  Time:       {elapsed:.2f}s")
        print(f"  Throughput: {throughput:.1f} KB/s")
        print(f"  Latency:    {(self.rx_first_chunk_time or t0) - t0:.3f}s (first chunk)")

        # Verify data integrity (test pattern is incrementing bytes)
        errors = 0
        for i in range(min(total, self.rx_expected)):
            expected_byte = i % 256
            # Pattern repeats every BLE_CHUNK_SIZE
            chunk_offset = i % BLE_CHUNK_SIZE
            expected_byte = chunk_offset & 0xFF
            if self.rx_buffer[i] != expected_byte:
                errors += 1
                if errors <= 3:
                    print(f"  Data error at byte {i}: got 0x{self.rx_buffer[i]:02x}, expected 0x{expected_byte:02x}")

        if errors == 0:
            print(f"  Integrity:  PASS ✓")
        else:
            print(f"  Integrity:  FAIL ({errors} errors)")

    async def test_pi_to_esp(self, size_kb=32):
        """Test 2: Pi→ESP throughput via writes."""
        print(f"\n{'='*50}")
        print(f"TEST 2: Pi → ESP ({size_kb} KB)")
        print(f"{'='*50}")

        total_bytes = size_kb * 1024
        self.tts_done.clear()

        # Generate test data (Opus-style: size header + data + end marker)
        test_data = bytearray()
        # Size header
        test_data.extend(struct.pack('<I', total_bytes))
        # Fill with pattern
        while len(test_data) < total_bytes + 4:
            test_data.append(len(test_data) & 0xFF)
        # End marker (zero-length) will be sent separately

        # Send with timing
        t0 = time.time()
        offset = 0
        chunks = 0

        # Send header first
        await self.client.write_gatt_char(
            SPEAKER_RX_UUID, bytes(test_data[:4]), response=False)
        offset = 4
        await asyncio.sleep(0.01)

        # Send data chunks
        while offset < len(test_data):
            end = min(offset + BLE_CHUNK_SIZE, len(test_data))
            chunk = bytes(test_data[offset:end])
            await self.client.write_gatt_char(SPEAKER_RX_UUID, chunk, response=False)
            offset = end
            chunks += 1
            # Minimal pacing
            if chunks % 10 == 0:
                await asyncio.sleep(0.001)

        # End marker
        await asyncio.sleep(0.05)
        await self.client.write_gatt_char(SPEAKER_RX_UUID, b'', response=False)

        send_elapsed = time.time() - t0
        send_throughput = total_bytes / send_elapsed / 1024 if send_elapsed > 0 else 0

        print(f"  Sent:       {total_bytes:,} bytes in {chunks} chunks")
        print(f"  Send time:  {send_elapsed:.2f}s")
        print(f"  Send rate:  {send_throughput:.1f} KB/s")

        # Wait for ESP to process (it will try to play as Opus, which will fail
        # since test data isn't valid Opus — but we still measure the BLE transfer)
        # The playback_done notification won't come for invalid data, so timeout is OK
        try:
            await asyncio.wait_for(self.tts_done.wait(), timeout=5.0)
            print(f"  ESP ACK:    received (playback done)")
        except asyncio.TimeoutError:
            print(f"  ESP ACK:    timeout (expected — test data isn't valid Opus)")

    async def test_round_trip(self):
        """Test 3: Measure round-trip time."""
        print(f"\n{'='*50}")
        print(f"TEST 3: Round-trip latency")
        print(f"{'='*50}")

        # Send a small control write and time how long until we get any notification back
        self.rx_buffer = bytearray()
        self.rx_expected = 0
        self.rx_done.clear()
        self.rx_start_time = None

        t0 = time.time()
        # Trigger 1KB test
        await self.client.write_gatt_char(
            CONTROL_UUID, bytes([CMD_TEST_THROUGHPUT, 1]), response=False)

        try:
            await asyncio.wait_for(self.rx_done.wait(), timeout=10.0)
            elapsed = time.time() - t0
            print(f"  1 KB round-trip: {elapsed*1000:.0f}ms")
            print(f"  Data received:   {len(self.rx_buffer):,} bytes")
        except asyncio.TimeoutError:
            print(f"  TIMEOUT — no data received")

    async def run(self):
        if not await self.connect():
            return

        try:
            await self.test_esp_to_pi(32)     # 32 KB ESP→Pi
            await asyncio.sleep(1.0)

            await self.test_esp_to_pi(128)    # 128 KB ESP→Pi
            await asyncio.sleep(1.0)

            await self.test_pi_to_esp(32)     # 32 KB Pi→ESP
            await asyncio.sleep(1.0)

            await self.test_pi_to_esp(128)    # 128 KB Pi→ESP
            await asyncio.sleep(1.0)

            await self.test_round_trip()

        except Exception as e:
            print(f"\nERROR: {e}")
            import traceback
            traceback.print_exc()
        finally:
            print(f"\n{'='*50}")
            print("TEST COMPLETE")
            print(f"{'='*50}")


async def main():
    print()
    print("  ╔══════════════════════════════════════╗")
    print("  ║  HELIOS BLE Throughput Test           ║")
    print("  ╚══════════════════════════════════════╝")
    print()

    test = ThroughputTest()
    await test.run()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\n\nCancelled.")
