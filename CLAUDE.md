# Project Helios — CLAUDE.md

## What This Is

A wearable assistive device for vision-impaired individuals. Three physical units:

1. **Pendant** (XIAO ESP32-S3 Sense) — worn on chest, has camera, PDM mic, button. No speaker on pendant.
2. **Belt unit** (Raspberry Pi 4B) — fanny pack, runs cloud API pipeline, has rear ultrasonic sensor + vibration motor, BT-paired to MT speaker.
3. **Forward-safety pods** (2x RP2040) — independent ultrasonic + buzzer modules on the front of the wearable. No comms with anything else.

## Architecture

### Three Independent Loops

1. **Forward Safety Loop** — RP2040 pods, always on, standalone.
   Two pods each run an HC-SR04 + buzzer. Zero dependency on pendant or Pi.

2. **Rear Safety Loop** — Pi, always on, no WiFi.
   `rear_safety.py`: 1x rear HC-SR04 → vibration motor PWM, intensity proportional to proximity.

3. **AI Query Loop** — pendant ↔ Pi over WiFi, on-demand via button.
   - Button hold: pendant streams PCM audio via chunked HTTP to Pi, continues capturing JPEG.
   - Button release: pendant POSTs JPEG to Pi, closes audio stream.
   - Pi pipeline: Cartesia Ink-Whisper (STT, streaming) → Claude Haiku 4.5 via Anthropic API (vision) → Cartesia Sonic 3 (TTS) → direct A2DP playback to MT BT speaker.
   - Pendant does not handle audio output. Pi owns the speaker.

### Transport

- **Pendant ↔ Pi: WiFi HTTP only.** WiFi stays up (OTA + admin server is always listening).
- **Pi → MT speaker: BT A2DP** via system bluez on Pi.
- **BLE on pendant: OTA rescue only.** If WiFi enrollment fails, pendant pivots to `ble_recovery.signed.bin` from SD and serves a NimBLE GATT rescue endpoint. No BLE transport anywhere in the product path.

### Pin Assignments

**Pendant (XIAO ESP32-S3 Sense):**
| Pin | Function |
|-----|----------|
| GPIO 44 (D7) | Push button (active-high, 3.3V supply) |
| GPIO 40/39 | Camera SCCB (SDA/SCL) |
| GPIO 10 | Camera XCLK |
| GPIO 41/42 | PDM mic DATA / CLK |
| GPIO 7/8/9/21 | microSD SPI (CS/CLK/MISO/MOSI) — onboard pullups, do not repurpose |
| Camera DVP | GPIO 10–18, 38–40, 47, 48 (Sense board internal) |

**Belt (Pi 4B):** see `rear_safety.py` for rear HC-SR04 + motor PWM pinout.

**Forward-safety pods:** each RP2040 owns its own HC-SR04 + buzzer; pinouts live with the pod firmware, not here.

## Codebase

```
firmware/
  diag/camera_ota/      # Active pendant fw: camera + mic + button + OTA + admin + SD recovery
  diag/ble_recovery/    # Standalone NimBLE rescue fw, loaded from SD when WiFi fails
  test_apps/            # wifi_throughput.c (standalone WiFi bench)
  src/                  # wifi.c — ESP-IDF WiFi abstraction used by wifi_throughput
  platformio.ini        # wifi_test env only (real fw is under firmware/diag/)

server.py               # Pi WiFi HTTP server — STT → Claude Haiku → TTS, plays via Pi BT to MT
client.py               # Test client for server.py
rear_safety.py          # Pi rear ultrasonic + vibration motor loop (systemd unit)
throughput_server.py    # HTTP throughput test server for WiFi debugging

scripts/
  ble_ota.py            # Bleak-based BLE rescue push client (OTA fallback only)
  sign_ota.py           # HMAC-SHA256 signer for OTA images
  gen_ota_key.py        # Generates OTA signing key + pubkey header
  package_ota.py        # Emits tagged <name>-<tag>-<version>.signed.bin + yaml
  setup.sh              # Pi bootstrap (system deps, venv, systemd units)
  prep-pi4b-sd.sh       # One-shot Pi 4B SD card provisioning

docs/OTA_STACK.md       # Full OTA + recovery architecture reference — source of truth
```

## Team

| Name | Role | Branch |
|------|------|--------|
| Kaden Schutt | Integration lead, firmware, API pipeline | `kaden` |
| Jeremy Branom | 3D housing, fanny-pack hardware | `jer` |
| Mohamed Tigana | Pi software, GPIO, rear safety loop | `mohamed` |
| Raghav Pahuja | Pendant hardware, WiFi/HTTP, prompt engineering | `raghav` |
| Anikesh Gupta | Button/GPIO integration research | `anikesh` |

## Dev Environment

- Each team member has a browser-based VS Code at `helios.schutt.dev`
- Each person works on their own git branch
- PlatformIO, clangd, Python are pre-installed
- `.env` has API keys for Cartesia + Anthropic — never commit it

## Key Commands

```bash
# Build + OTA-push active pendant fw
ssh kaden@k9lin.local 'bash -lc "cd ~/helios-diag-camera-ota && pio run"'
scp kaden@k9lin.local:helios-diag-camera-ota/.pio/build/xiao_esp32s3/firmware.bin /tmp/helios-bins/staged/firmware.bin
python3 scripts/sign_ota.py /tmp/helios-bins/staged/firmware.bin
curl --data-binary @/tmp/helios-bins/staged/firmware.signed.bin http://helios-cam.local/ota

# Pi-side server (dev on Mac or prod on Pi)
python3 server.py

# Test client
python3 client.py
```

## Design Constraints

- Raspberry Pi 4B as central hub (required by course)
- XIAO ESP32-S3 Sense as pendant controller
- All safety loops must work without internet (RP2040 pods + Pi rear loop satisfy this)
- Total additional sensor budget: $25
- Must be wearable, discreet, lightweight

## Timeline

- **Demo Day: April 21, 2026** (today)
