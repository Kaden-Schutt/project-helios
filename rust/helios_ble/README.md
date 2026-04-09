# helios_ble

A minimal Rust library that bypasses `bleak`'s 23-byte MTU bug on BlueZ
when talking to the Helios ESP32-S3. Exposed as a Python extension via PyO3
so `server_ble.py` can use it as a drop-in transport replacement.

## Why this exists

`bleak` 3.x on BlueZ 5.66+ never negotiates ATT MTU above the default 23 bytes,
even when the peripheral (ESP32-S3) preferences 512. At 23 bytes per packet,
the JPEG + Opus mic stream can't complete, and the Helios pipeline degrades to
"push button, get 'Hello' fallback." Not useful.

This library sidesteps `bleak` entirely and uses `bluer` to call
`AcquireWrite` / `AcquireNotify` on the D-Bus GATT interface. Those methods
force BlueZ to run the ATT MTU Exchange and return a real socket whose
`mtu()` reports the negotiated value. If it comes back below 512, we fail
loudly — the caller should not proceed.

## Python API

```python
import helios_ble

ble = helios_ble.HeliosBle()

mtu = ble.connect(
    mac="90:70:69:13:01:C2",
    write_chars=[SPEAKER_RX_UUID, CONTROL_UUID],
)
# Raises ConnectionError if mtu < 512.

ble.start_notify(MIC_TX_UUID, lambda uuid, data: handle(data))
ble.write(SPEAKER_RX_UUID, opus_chunk)   # one ATT packet, ≤ mtu-3 bytes
ble.stop_notify(MIC_TX_UUID)
ble.disconnect()

ble.mtu            # negotiated MTU, read-only
ble.is_connected() # bool
```

`write()` accepts one packet per call — no fragmentation, matching
`SOCK_SEQPACKET` semantics. Chunk payloads larger than MTU on the Python
side, the same way `server_ble.py` already does with `BLE_CHUNK_SIZE`.

## Building

### Option A — Native build on the target board (simplest)

Works on Pi 4B (`aarch64`) and OrangePi Zero 2W (also `aarch64`). SSH in and:

```bash
# One-time setup
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
source "$HOME/.cargo/env"
sudo apt install -y libdbus-1-dev pkg-config python3-dev

# In your project venv
pip install maturin

# Build + install into the active venv
cd /home/pi/project-helios/rust/helios_ble
maturin develop --release
```

After `maturin develop`, `import helios_ble` works in the same venv.
Expect a 5-10 min first build (bluer pulls dbus / zbus), then rebuilds
are fast.

### Option B — Cross-compile from macOS

Painful because PyO3 + bluer need aarch64 versions of `libdbus`,
`libpython`, and a linker. The cleanest route is a Docker container
with the aarch64 toolchain preinstalled:

```bash
# Using maturin's built-in aarch64 cross image
rustup target add aarch64-unknown-linux-gnu
maturin build --release --target aarch64-unknown-linux-gnu --zig

# Result: target/wheels/helios_ble-*.whl
# Copy to the Pi and pip install
```

`--zig` uses the zig linker to avoid needing a separate aarch64 GCC.
Requires `pip install ziglang` first.

If that fails, fall back to Option A. Cross-compiling D-Bus extensions
is rarely worth the effort for a one-off project.

## Wiring into `server_ble.py`

Replace the `bleak` imports and the `HeliosClient.connect()` body with:

```python
import helios_ble

class HeliosClient:
    def __init__(self):
        self.ble = helios_ble.HeliosBle()
        # ...existing state...

    async def connect(self):
        # helios_ble is synchronous — wrap in a thread so it doesn't
        # block the asyncio loop. block_on inside tokio handles the
        # actual async work under the hood.
        loop = asyncio.get_running_loop()
        mtu = await loop.run_in_executor(
            None,
            self.ble.connect,
            HELIOS_MAC,
            [SPEAKER_RX_UUID, CONTROL_UUID],
        )
        log.info(f"MTU: {mtu}")

        # Notifications: callbacks fire on the Rust reader task.
        # The existing _on_mic_notify / _on_control_notify signatures
        # take (sender, data); adapt them to (uuid_str, data).
        self.ble.start_notify(MIC_TX_UUID, self._on_mic_notify_v2)
        self.ble.start_notify(CONTROL_UUID, self._on_control_notify_v2)
        return True

    def _on_mic_notify_v2(self, uuid, data):
        self._on_mic_notify(None, bytearray(data))

    def _on_control_notify_v2(self, uuid, data):
        self._on_control_notify(None, bytearray(data))

    async def send_tts(self, opus_data: bytes):
        # Chunk to BLE_CHUNK_SIZE and write synchronously via the Rust layer.
        loop = asyncio.get_running_loop()
        await loop.run_in_executor(None, self._send_tts_sync, opus_data)

    def _send_tts_sync(self, opus_data: bytes):
        self.ble.write(SPEAKER_RX_UUID, b"\xFF\xFF\xFF\xFF")
        for i in range(0, len(opus_data), BLE_CHUNK_SIZE):
            chunk = opus_data[i:i + BLE_CHUNK_SIZE]
            self.ble.write(SPEAKER_RX_UUID, chunk)
        self.ble.write(SPEAKER_RX_UUID, b"")
```

The rest of the STT → LLM → TTS pipeline is unchanged.

## Troubleshooting

- **`negotiated MTU 23 is below the required 512`** — BlueZ still isn't
  running the ATT Exchange. Check `/etc/bluetooth/main.conf` has
  `ExchangeMTU = 517` uncommented under `[GATT]`, then
  `systemctl restart bluetooth`.
- **`GATT services were not resolved within 10s`** — the device connected
  but BlueZ never finished GATT discovery. Usually means another central
  (your MacBook, a phone) is holding the BLE connection. Disconnect it.
- **Build fails with `dbus.h not found`** — `sudo apt install libdbus-1-dev`.
- **`journalctl -u bluetooth`** — check the BlueZ log for refused MTU
  exchanges or access-denied errors.
