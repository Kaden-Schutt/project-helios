# Project Helios

A wearable assistive device for vision-impaired individuals. Press and hold a button on a chest pendant, ask a question about what's in front of you, release. The pendant streams microphone audio and a JPEG to a belt-worn Raspberry Pi over WiFi; the Pi transcribes, asks Claude Haiku to answer about the image, and speaks the response over a paired Bluetooth speaker.

Three physical units, all independent:

1. **Pendant** (XIAO ESP32-S3 Sense) — chest-worn; camera, PDM microphone, button. No speaker.
2. **Belt unit** (Raspberry Pi 4B) — fanny pack; runs the AI pipeline and drives a paired BT speaker.
3. **Forward-safety pods** (2× RP2040) — front-mounted ultrasonic + buzzer modules. Standalone — no comms with the rest of the system.

A fourth always-on **rear-safety loop** runs on the Pi itself (`rear_safety.py`): rear-facing HC-SR04 → vibration motor PWM, intensity proportional to proximity. No WiFi or cloud dependency.

## How It Works

```
   PENDANT (ESP32-S3)                    BELT (Raspberry Pi 4B)           BT speaker
 ┌────────────────────┐    WiFi HTTP    ┌──────────────────────┐            ┌────────┐
 │  Button held:      │ ──────────────> │  1. STT (Cartesia)   │            │        │
 │    Camera JPEG     │   chunked PCM   │  2. Vision LLM       │   A2DP     │        │
 │    Mic PCM stream  │   + JPEG POST   │     (Claude Haiku)   │ ─────────> │        │
 │                    │                 │  3. TTS (Cartesia)   │   bluez    │        │
 │  (WiFi always on,  │                 │     played directly  │            │        │
 │   OTA + admin also │                 │     over BT          │            │        │
 │   served)          │                 │                      │            │        │
 └────────────────────┘                 └──────────────────────┘            └────────┘

   Front-safety pods (2× RP2040) own their own HC-SR04 + buzzer — no comms with anything.
   Rear-safety loop (Pi GPIO) runs alongside the AI pipeline, no network involvement.
```

## Networking — WiFi vs BLE

This is the part that confuses every reader. The pendant has both radios, but they have **completely different roles**:

| Radio | When it's on | What it does |
|-------|--------------|--------------|
| **WiFi** | Always (the only runtime transport) | Talks to the Pi's HTTP server, accepts signed OTA updates, serves admin endpoints. Nothing in the product path uses anything else. |
| **BLE** | **Disabled in the main firmware.** Only comes online if WiFi fails to connect. | Rescue path. Boots a separate "Helios-Recovery" firmware from SD that advertises a NimBLE GATT service so a new firmware can be pushed when WiFi is unreachable (wrong network, bad credentials, etc.). |

The pendant **never** talks to the Pi over BLE. There is no BLE in the audio, camera, or query path. If you see Bluetooth in this project, it's either the Pi → BT speaker link (A2DP, system bluez) or the rescue path described above.

## Hardware

What you need to reproduce the pendant + belt setup:

| Component | Part | Approx. cost | Notes |
|-----------|------|--------------|-------|
| Pendant MCU | **Seeed XIAO ESP32-S3 Sense** | $14 | Must be the **Sense** variant — the expansion board carries the camera, PDM mic, and microSD slot. The bare XIAO ESP32-S3 will not work without external sensors. |
| microSD | Any FAT32 card, ≥1 GB | $5 | Holds `wifi.conf`, BLE rescue image, OTA fallback. SanDisk 64 GB tested. |
| Push button | 3-pin breakout, momentary | $2 | Active-high; wired to GPIO 44 (D7). |
| Belt hub | **Raspberry Pi 4B** (2 GB+) | $35 | Course requirement. Runs `server.py` + `rear_safety.py`. |
| Speaker | Any A2DP BT speaker | $15+ | Paired once via `bluetoothctl`. We use a generic MT-branded portable. |
| LiPo cell + charger | TP4057 + JST | $5 | Single JST swap between XIAO and charger; 1S Li-ion, 1000+ mAh. |
| Front-safety pods (optional) | 2× RP2040 + HC-SR04 + buzzer | $20 | Independent of everything else; build to taste. |
| Rear-safety hardware (optional) | HC-SR04 + ERM motor | $5 | Wired to Pi GPIO; pinout in `rear_safety.py`. |

You also need an account at:
- [Cartesia](https://cartesia.ai) — STT (Ink-Whisper) + TTS (Sonic 3)
- [Anthropic](https://console.anthropic.com) — Claude Haiku 4.5 vision

## Repository Structure

```
project-helios/
├── firmware/
│   ├── diag/camera_ota/            # MAIN pendant firmware
│   │   ├── main.c                  # Boot → SD → recovery → camera → WiFi → HTTP server
│   │   ├── query_client.c          # POSTs PCM + JPEG to the Pi on button-hold
│   │   ├── ota.c / ota_verify.c    # Signed (HMAC-SHA256) WiFi OTA
│   │   ├── admin.c                 # /admin partition + SD firmware library
│   │   ├── sd_card.c               # SD mount + wifi.conf parser
│   │   ├── recovery.c              # 3-tier recovery (rollback → SD → BLE pivot)
│   │   └── platformio.ini          # board = seeed_xiao_esp32s3
│   ├── diag/ble_recovery/          # Rescue firmware (loaded from SD when WiFi fails)
│   ├── test_apps/wifi_throughput.c # Standalone WiFi bench
│   └── platformio.ini              # wifi throughput env only — main fw is under diag/
├── server.py                       # Pi: STT → Claude → TTS → BT (port 5750)
├── client.py                       # Spacebar test client (no pendant required)
├── rear_safety.py                  # Pi rear ultrasonic + vibration motor (systemd)
├── throughput_server.py            # HTTP throughput bench for WiFi debug
├── scripts/
│   ├── setup.sh                    # Pi bootstrap (apt + venv + systemd)
│   ├── prep-pi4b-sd.sh             # One-shot Pi 4B SD provisioning
│   ├── gen_ota_key.py              # One-time HMAC key generation
│   ├── sign_ota.py                 # firmware.bin → firmware.signed.bin
│   ├── package_ota.py              # Tagged SD-library bundles
│   ├── ble_ota.py                  # BLE rescue push client (rescue path only)
│   └── helios-wifi-import.{sh,service}  # Boot-time wifi.conf → NetworkManager (Pi)
├── docs/OTA_STACK.md               # Full OTA architecture + endpoint reference
└── requirements.txt
```

## Bring-up (first time, anyone with the hardware)

This is the one-USB-flash, then-OTA-forever path. After step 5 you never need to plug in USB again.

### 1. Pi server

On the Pi (or any Linux/Mac for testing):

```bash
git clone https://github.com/<you>/project-helios && cd project-helios
pip install -r requirements.txt
cp .env.example .env
# Edit .env:
#   CARTESIA_API_KEY    — Cartesia (STT + TTS)
#   ANTHROPIC_API_KEY   — Anthropic (Claude Haiku vision)
#   TTS_VOICE_ID        — (optional) Cartesia voice ID
```

Pair the BT speaker once: `bluetoothctl` → `scan on` → `pair <MAC>` → `trust <MAC>` → `connect <MAC>`.

Note the Pi's LAN IP — you'll need it for the pendant build (`ip addr show wlan0` or `hostname -I`).

Then start the server (default port 5750):

```bash
python server.py
```

### 2. OTA signing key (one-time)

```bash
python3 scripts/gen_ota_key.py
# Writes ~/.helios/ota_sign.key (32-byte secret, chmod 600)
# Writes firmware/diag/{camera_ota,ble_recovery}/ota_pubkey.h (gitignored)
```

Back this key up. Losing it means every signed `.bin` you've built becomes unusable and you have to USB-reflash to rotate.

### 3. Build both firmwares

You need two binaries: the main pendant fw and the BLE rescue fw.

```bash
# Main pendant firmware
cd firmware/diag/camera_ota
pio run --build-flag '-DHELIOS_PI_URL="http://<your-pi-ip>:5750"'
# .pio/build/xiao_esp32s3/firmware.bin

# BLE rescue firmware
cd ../ble_recovery
pio run
# .pio/build/xiao_esp32s3/firmware.bin
```

Sign both:

```bash
python3 scripts/sign_ota.py firmware/diag/camera_ota/.pio/build/xiao_esp32s3/firmware.bin
python3 scripts/sign_ota.py firmware/diag/ble_recovery/.pio/build/xiao_esp32s3/firmware.bin
```

### 4. Prep the microSD card

Format FAT32, then drop three files at the root:

```bash
# wifi.conf — multi-network priority list (first that connects wins)
cat > /Volumes/HELIOS/wifi.conf <<EOF
ssid1=Your Home Network
psk1=your-password-here
ssid2=Phone Hotspot
psk2=hotspot-password
EOF

# BLE rescue image (renamed)
cp firmware/diag/ble_recovery/.pio/build/xiao_esp32s3/firmware.signed.bin \
   /Volumes/HELIOS/ble_recovery.signed.bin

# Tier-2 OTA fallback (the same main fw, named for the recovery system)
cp firmware/diag/camera_ota/.pio/build/xiao_esp32s3/firmware.signed.bin \
   /Volumes/HELIOS/recovery.signed.bin
```

Insert the SD into the XIAO Sense expansion board.

### 5. USB flash the pendant — once

```bash
cd firmware/diag/camera_ota
pio run --target upload
```

After the device boots, it will:
1. Mount the SD card.
2. Read `/sd/wifi.conf` and join the first network it can.
3. Bring up mDNS as `helios-cam.local` and serve HTTP on port 80.
4. (If WiFi fails: load `ble_recovery.signed.bin` and reboot into BLE rescue mode.)

Verify:

```bash
curl http://helios-cam.local/info     # JSON: app name, partition, heap, uptime
```

Unplug USB. From here on, all firmware updates are over the air.

### 6. Try it

Hold the button on the pendant, say "what's in front of me?", release. You should hear a spoken answer through the BT speaker.

## Day-to-day: pushing new firmware

```bash
cd firmware/diag/camera_ota
pio run
python3 scripts/sign_ota.py .pio/build/xiao_esp32s3/firmware.bin
curl --data-binary @.pio/build/xiao_esp32s3/firmware.signed.bin \
    http://helios-cam.local/ota
# ~15 s round-trip. If the new fw boots and survives validation,
# the bootloader cancels rollback and the SD recovery slot is updated.
```

If it doesn't boot, ESP-IDF rolls back automatically after 3 failed attempts. If both OTA slots are dead, the device falls back to `recovery.signed.bin` from SD. If WiFi itself is unreachable, it pivots to BLE rescue. See [`docs/OTA_STACK.md`](docs/OTA_STACK.md).

## Updating WiFi credentials over the air

```bash
curl -X POST --data-binary @new-wifi.conf http://helios-cam.local/wifi
curl -X POST http://helios-cam.local/reboot
```

The reboot is required (the file is read once at boot) and the handler unmounts SD cleanly first so FatFS flushes.

## Test mode (no pendant required)

```bash
# Terminal 1
python server.py

# Terminal 2
python client.py    # hold spacebar to talk
```

`client.py` uses your laptop's mic + a JPEG dropped into `input/` to exercise the same `/query` endpoint the pendant hits.

## Pin Map (Pendant — XIAO ESP32-S3 Sense)

| GPIO | Function | GPIO | Function |
|------|----------|------|----------|
| 44 (D7) | Push button (active-high) | 41 | PDM mic DATA |
| 39 | Camera SIOC (SCCB) | 42 | PDM mic CLK |
| 40 | Camera SIOD (SCCB) | 10 | Camera XCLK |
| 21 | SD CS  | 7 | SD SCK |
| 8  | SD MISO | 9 | SD MOSI |

GPIO 7/8/9/21 each have an onboard pullup to 3V3 — do not repurpose them for buttons. GPIO 0/3/45/46 are strapping pins, also avoid. Camera DVP pins (10–18, 38–40, 47, 48) are wired internally on the Sense expansion.

## Documentation

- [`docs/OTA_STACK.md`](docs/OTA_STACK.md) — full OTA, recovery tiers, admin endpoints, signing model, troubleshooting
- [`CLAUDE.md`](CLAUDE.md) — AI assistant configuration for this project

## Team

| Name | Role | Branch |
|------|------|--------|
| Kaden Schutt | Integration lead, firmware, API pipeline | `kaden` |
| Jeremy Branom | 3D housing, fanny-pack hardware | `jer` |
| Mohamed Tigana | Pi software, GPIO, rear safety loop | `mohamed` |
| Raghav Pahuja | Pendant hardware, WiFi/HTTP, prompt engineering | `raghav` |
| Anikesh Gupta | Button/GPIO integration research | `anikesh` |
