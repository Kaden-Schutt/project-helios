# Project Helios

A wearable assistive device for vision-impaired individuals. Two physical units communicate over BLE:

1. **Pendant** (XIAO ESP32S3 Sense) — worn on chest; has camera, microphone, speaker, button
2. **Belt unit** (Raspberry Pi 4B) — fanny pack; runs the AI pipeline

Press and hold the button, ask a question about your surroundings, release. The device captures what the camera sees, transcribes your speech, asks an AI that can see the image, and speaks the answer back — all in about 2 seconds.

## How It Works

```
  PENDANT (ESP32)                              BELT (Raspberry Pi)
 ┌──────────────────┐          BLE            ┌───────────────────────┐
 │                  │  ── JPEG + Opus mic ──> │                       │
 │  Button held:    │                         │  1. STT (Cartesia)    │
 │    Camera snap   │                         │  2. Vision LLM        │
 │    Mic stream    │                         │     (Claude Haiku)    │
 │                  │  <── Opus TTS stream ── │  3. TTS (Cartesia)    │
 │  Button released:│                         │     streamed back     │
 │    Speaker plays │                         │                       │
 └──────────────────┘                         └───────────────────────┘
```

**Peripheral lifecycle:** Only one group is active at a time. During recording, the speaker amp is powered off. During playback, the camera and mic are powered off. This eliminates electrical noise and allows higher speaker volume.

**Streaming TTS:** Audio plays as it generates — the Pi streams Opus frames over BLE with a 300ms buffer, so the speaker starts ~1-2 seconds after button release instead of ~5 seconds.

## Hardware

| Component | Part | Notes |
|-----------|------|-------|
| MCU | XIAO ESP32S3 Sense | Dual-core 240MHz, 8MB PSRAM, 8MB flash |
| Camera | OV2640 | On Sense board, 640x480 JPEG |
| Microphone | MSM261S4030H0 | On Sense board, PDM, 16kHz |
| Speaker amp | MAX98357A | I2S, +15dB gain (GAIN=GND) |
| Speaker | 3W 8 ohm | Driven by MAX98357A |
| Button | Momentary push | GPIO 4 |
| SD card | MicroSD | On Sense board, config storage |
| Hub | Raspberry Pi 4B | Runs Python BLE server |

## Repository Structure

```
project-helios/
├── firmware/                    # ESP32 firmware (C, ESP-IDF via PlatformIO)
│   ├── src/
│   │   ├── main.c              # State machine, peripheral lifecycle, speaker task
│   │   ├── speaker.c           # I2S output, Opus decode, DSP filters, streaming
│   │   ├── mic.c               # PDM microphone input
│   │   ├── camera.c            # OV2640 JPEG capture
│   │   ├── ble.c               # NimBLE GATT server, BLE protocol
│   │   ├── config.c            # JSON config persistence
│   │   └── sdcard.c            # SD card SPI driver
│   ├── include/
│   │   └── helios.h            # Pin defs, API declarations, config struct
│   └── platformio.ini          # Build configuration
├── server_ble.py               # Pi BLE server (production) — full pipeline
├── server.py                   # HTTP test server (development)
├── client.py                   # HTTP test client (development)
├── usb_receiver.py             # USB serial receiver (development)
├── tests/                      # BLE protocol tests, throughput tests
├── docs/                       # Firmware documentation + design specs
├── diagram.json                # Wokwi circuit simulator
└── requirements.txt            # Python dependencies
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

Run the BLE server:
```bash
python server_ble.py
```

### Firmware (ESP32)

```bash
# Build (requires PlatformIO + ESP-IDF toolchain)
cd firmware
pio run -e xiao_esp32s3

# Flash
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 \
  write-flash 0x0 .pio/build/xiao_esp32s3/bootloader.bin \
              0x8000 .pio/build/xiao_esp32s3/partitions.bin \
              0x10000 .pio/build/xiao_esp32s3/firmware.bin
```

### HTTP Test Mode (no hardware needed)

```bash
# Terminal 1: start test server
python server.py

# Terminal 2: run test client (hold spacebar to talk)
python client.py
```

## BLE Protocol

Four BLE characteristics on a single GATT service:

| Characteristic | UUID suffix | Direction | Purpose |
|---------------|-------------|-----------|---------|
| Mic TX | `...4322` | ESP → Pi | JPEG + Opus mic audio (multiplexed) |
| Speaker RX | `...4323` | Pi → ESP | Opus TTS audio stream |
| Control | `...4324` | Both | Button events, volume, status |

### Mic → Pi (multiplexed on Mic TX)

```
JPEG:   0xFFFFFFFE + uint32_le(length) + data chunks
Opus:   0xFFFFFFFF + [uint16_le(len)][frame]... + empty notification
```

### Pi → ESP (on Speaker RX)

```
Start:  0xFFFFFFFF (4 bytes)
Data:   [uint16_le(frame_len)][opus_frame]... (packed into BLE writes)
End:    empty write (0 bytes)
```

## Audio Pipeline

**Capture** (ESP32): PDM mic → 16kHz 16-bit PCM → Opus encode (24kbps VOIP, 20ms frames) → BLE

**AI** (Pi): Opus decode → Cartesia STT → Claude Haiku (with JPEG) → Cartesia TTS (WebSocket streaming)

**Playback** (ESP32): BLE → PSRAM buffer → Opus decode → 200Hz high-pass biquad → low-pass → volume scale → I2S stereo → MAX98357A → speaker

## Pin Map

| GPIO | Function | GPIO | Function |
|------|----------|------|----------|
| 4 | Button | 41 | Mic DATA |
| 5 | Speaker LRC (I2S WS) | 42 | Mic CLK |
| 6 | Speaker BCLK | 10 | Camera XCLK |
| 43 | Speaker DIN | 13 | Camera PCLK |
| 21 | SD CS | 39 | Camera SIOC |
| 7 | SD SCK | 40 | Camera SIOD |
| 8 | SD MISO | 38 | Camera VSYNC |
| 9 | SD MOSI | 47 | Camera HREF |

## Team

| Name | Role | Branch |
|------|------|--------|
| Kaden Schutt | Integration lead, firmware, API pipeline | `kaden` |
| Jeremy Branom | 3D housing, fanny-pack hardware | `jer` |
| Mohamed Tigana | Pi software, GPIO, rear safety loop | `mohamed` |
| Raghav Pahuja | Pendant hardware, WiFi/HTTP, prompt engineering | `raghav` |
| Anikesh Gupta | Button/GPIO integration research | `anikesh` |

## Documentation

- [`docs/firmware-overview.md`](docs/firmware-overview.md) — Detailed firmware architecture, memory layout, module descriptions
- [`CLAUDE.md`](CLAUDE.md) — AI assistant configuration for this project
