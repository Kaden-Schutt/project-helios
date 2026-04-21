# Project Helios

A wearable assistive device for vision-impaired individuals. Three units:

1. **Pendant** (XIAO ESP32-S3 Sense) — chest-worn; has camera, PDM microphone, button. No speaker.
2. **Belt unit** (Raspberry Pi 4B) — fanny pack; runs the AI pipeline and drives a paired BT speaker.
3. **Forward-safety pods** (2× RP2040) — independent ultrasonic + buzzer modules on the front of the wearable.

Press and hold the button, ask a question about your surroundings, release. The pendant streams mic audio and a JPEG to the Pi over WiFi; the Pi transcribes, asks Claude Haiku to answer about the image, and speaks the answer back over its paired BT speaker.

## How It Works

```
   PENDANT (ESP32-S3)                    BELT (Raspberry Pi 4B)           MT BT speaker
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
```

## Hardware

| Component | Part | Notes |
|-----------|------|-------|
| Pendant MCU | XIAO ESP32-S3 Sense | Dual-core 240 MHz, 8 MB PSRAM, 8 MB flash |
| Camera | OV3660/OV2640 | On Sense expansion board, JPEG |
| Microphone | PDM | On Sense board, 16 kHz |
| Button | Momentary push | GPIO 44 (D7) |
| microSD | SanDisk 64 GB FAT32 | On Sense expansion board; holds OTA rescue + wifi.conf |
| Belt hub | Raspberry Pi 4B | Runs `server.py`, paired to MT BT speaker |
| Speaker | MT generic BT speaker | A2DP sink, paired to Pi |
| Charger | TP4057 with JST | Li-ion cell, single JST swap between XIAO and charger |
| Front-safety pods | 2× RP2040 + HC-SR04 + buzzer | Independent, no pendant/Pi comms |

## Repository Structure

```
project-helios/
├── firmware/
│   ├── diag/camera_ota/            # Active pendant firmware
│   │   ├── main.c                  # Entry, recovery check, WiFi, HTTP server
│   │   ├── ota.c / ota_verify.c    # Streaming HMAC-SHA256 signed OTA
│   │   ├── admin.c                 # /admin partition + SD library endpoints
│   │   ├── sd_card.c               # SD mount + wifi.conf parser
│   │   ├── recovery.c              # 3-tier recovery (rollback → SD → BLE pivot)
│   │   └── platformio.ini          # -DFW_TAG="debug"
│   ├── diag/ble_recovery/          # Standalone NimBLE rescue firmware (SD-loaded)
│   ├── test_apps/wifi_throughput.c # Standalone WiFi bench
│   └── src/wifi.c                  # WiFi helper used by throughput test
├── server.py                       # Pi WiFi HTTP server — STT → Claude → TTS → BT
├── client.py                       # Test client for server.py
├── rear_safety.py                  # Pi rear ultrasonic + vibration motor (systemd)
├── throughput_server.py            # HTTP throughput bench for WiFi debug
├── scripts/
│   ├── setup.sh                    # Pi bootstrap (apt + venv + systemd)
│   ├── prep-pi4b-sd.sh             # One-shot Pi 4B SD provisioning
│   ├── ble_ota.py                  # BLE rescue push client (OTA fallback only)
│   ├── sign_ota.py / gen_ota_key.py / package_ota.py  # OTA tooling
│   └── helios-wifi-import.{sh,service}  # Boot-time wifi.conf → NetworkManager
├── docs/OTA_STACK.md               # OTA architecture + endpoint reference
└── requirements.txt
```

## Setup

### Pi Server (Raspberry Pi or dev machine)

```bash
pip install -r requirements.txt
cp .env.example .env
# Add API keys to .env:
#   CARTESIA_API_KEY    — Cartesia (STT + TTS)
#   ANTHROPIC_API_KEY   — Anthropic (Claude Haiku vision LLM)
#   TTS_VOICE_ID        — (optional) Cartesia voice ID
```

Pair the MT BT speaker to the Pi once (`bluetoothctl` → scan, pair, trust, connect). Then:

```bash
python server.py
```

### Pendant Firmware (OTA-first)

Build on the k9lin build host and push signed OTA — no USB needed after the initial flash:

```bash
ssh kaden@k9lin.local 'bash -lc "cd ~/helios-diag-camera-ota && pio run"'
scp kaden@k9lin.local:helios-diag-camera-ota/.pio/build/xiao_esp32s3/firmware.bin /tmp/helios-bins/staged/firmware.bin
python3 scripts/sign_ota.py /tmp/helios-bins/staged/firmware.bin
curl --data-binary @/tmp/helios-bins/staged/firmware.signed.bin http://helios-cam.local/ota
```

See [`docs/OTA_STACK.md`](docs/OTA_STACK.md) for the full architecture, recovery tiers, admin endpoints, and signing model.

### HTTP Test Mode (no pendant needed)

```bash
# Terminal 1
python server.py

# Terminal 2
python client.py    # hold spacebar to talk
```

## Pin Map (Pendant — XIAO ESP32-S3 Sense)

| GPIO | Function | GPIO | Function |
|------|----------|------|----------|
| 44 (D7) | Push button (active-high) | 41 | PDM mic DATA |
| 39 | Camera SIOC (SCCB) | 42 | PDM mic CLK |
| 40 | Camera SIOD (SCCB) | 10 | Camera XCLK |
| 21 | SD CS  | 7 | SD SCK |
| 8  | SD MISO | 9 | SD MOSI |

GPIO 7/8/9/21 each have an onboard pullup to 3V3 — do not repurpose them for buttons. GPIO 0/3/45/46 are strapping pins, also avoid.

## Team

| Name | Role | Branch |
|------|------|--------|
| Kaden Schutt | Integration lead, firmware, API pipeline | `kaden` |
| Jeremy Branom | 3D housing, fanny-pack hardware | `jer` |
| Mohamed Tigana | Pi software, GPIO, rear safety loop | `mohamed` |
| Raghav Pahuja | Pendant hardware, WiFi/HTTP, prompt engineering | `raghav` |
| Anikesh Gupta | Button/GPIO integration research | `anikesh` |

## Documentation

- [`docs/OTA_STACK.md`](docs/OTA_STACK.md) — OTA, recovery tiers, admin endpoints, signing
- [`CLAUDE.md`](CLAUDE.md) — AI assistant configuration for this project
