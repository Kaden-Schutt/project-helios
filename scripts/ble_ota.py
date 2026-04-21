#!/usr/bin/env python3
"""Push a signed firmware image to Helios-Recovery over BLE.

Prerequisites:
  pip install bleak
  An already-signed firmware:
    python3 scripts/sign_ota.py firmware.bin

Usage:
  python3 scripts/ble_ota.py path/to/firmware.signed.bin
  python3 scripts/ble_ota.py path/to/firmware.signed.bin --name Helios-Recovery

The device auto-reboots on successful signature match. Expect the BLE
connection to terminate abruptly at the end — that's the reboot.
"""

import argparse
import asyncio
import pathlib
import struct
import sys
import time

try:
    from bleak import BleakScanner, BleakClient
except ImportError:
    print("pip install bleak", file=sys.stderr)
    sys.exit(2)

SVC_UUID   = "0000ffe0-0000-1000-8000-00805f9b34fb"
CHR_SIZE   = "0000ffe1-0000-1000-8000-00805f9b34fb"
CHR_CHUNK  = "0000ffe2-0000-1000-8000-00805f9b34fb"
DEFAULT_NAME = "Helios-Recovery"


async def find_device(name: str, timeout_s: float = 15.0):
    print(f"scanning for {name!r} (up to {timeout_s:.0f}s)")
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        devs = await BleakScanner.discover(timeout=3.0)
        for d in devs:
            if d.name and name in d.name:
                print(f"  found {d.name}  {d.address}  rssi={d.rssi}")
                return d
    return None


async def push(fw_path: pathlib.Path, name: str):
    data = fw_path.read_bytes()
    total = len(data)
    print(f"firmware: {fw_path}  ({total} bytes incl. 32-byte sig)")

    dev = await find_device(name)
    if not dev:
        print(f"no {name!r} device found")
        return 1

    async with BleakClient(dev) as client:
        print(f"connected  mtu={client.mtu_size}")
        chunk_size = max(20, client.mtu_size - 3)
        print(f"chunk size = {chunk_size}")

        await client.write_gatt_char(CHR_SIZE, struct.pack("<I", total), response=True)
        print(f"  size set = {total}")

        sent = 0
        t0 = time.time()
        while sent < total:
            n = min(chunk_size, total - sent)
            try:
                await client.write_gatt_char(CHR_CHUNK, data[sent:sent + n], response=False)
            except Exception as e:
                # Device reboot mid-final-chunk looks like a disconnect; ignore near end
                if sent + n >= total - chunk_size:
                    print(f"  (final write disconnected — expected on success)")
                    break
                raise
            sent += n
            if sent % (chunk_size * 40) == 0 or sent == total:
                pct = 100.0 * sent / total
                rate = sent / (time.time() - t0 + 1e-9) / 1024
                print(f"  sent {sent}/{total}  {pct:5.1f}%  {rate:.1f} KB/s")
        dt = time.time() - t0
        rate = total / dt / 1024
        print(f"delivered in {dt:.1f}s ({rate:.1f} KB/s)")
    print("device should reboot shortly")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("firmware", type=pathlib.Path)
    ap.add_argument("--name", default=DEFAULT_NAME)
    args = ap.parse_args()
    if not args.firmware.exists():
        print(f"no such file: {args.firmware}")
        return 2
    return asyncio.run(push(args.firmware, args.name))


if __name__ == "__main__":
    sys.exit(main())
