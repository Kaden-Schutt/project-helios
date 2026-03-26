# Project Helios — CLAUDE.md

## What This Is

A wearable assistive device for vision-impaired individuals. Two physical units communicate over WiFi:

1. **Pendant** (XIAO ESP32S3 Sense) — worn on chest, has camera, mic, speaker, 2x ultrasonic sensors, 2x buzzers, button
2. **Belt unit** (Raspberry Pi 4B) — fanny pack, runs cloud API pipeline, has rear ultrasonic sensor + vibration motor

## Architecture

### Three Independent Loops

1. **Forward Safety Loop** (pendant, always on, no WiFi)
   - 2x HC-SR04 ultrasonic sensors poll at 10Hz
   - Left/right buzzers fire when obstacles detected (<100cm pulse, <30cm continuous)
   - Runs on ESP32 only — zero network dependency

2. **Rear Safety Loop** (belt, always on, no WiFi)
   - 1x rear-facing HC-SR04 on Pi
   - Vibration motor intensity proportional to proximity
   - Fully independent from pendant

3. **AI Query Loop** (pendant + belt, on-demand via button)
   - Button hold → camera JPEG + mic PCM capture
   - Button release → WiFi on → POST JSON to Pi → WiFi off
   - Pi pipeline: Cartesia Ink-Whisper (STT) → Gemini 3.1 Pro via OpenRouter (vision) → Cartesia Sonic 3 (TTS)
   - TTS audio sent back to ESP32, played through speaker
   - Round-trip: ~5-6 seconds

### Pin Assignments

**Safety Loop (base board, no Sense):**
| Pin | Function |
|-----|----------|
| D0/GPIO1 | Left HC-SR04 TRIG |
| D1/GPIO2 | Left HC-SR04 ECHO |
| D2/GPIO3 | Left Buzzer (+) |
| D8/GPIO7 | Right HC-SR04 TRIG |
| D9/GPIO8 | Right HC-SR04 ECHO |
| D10/GPIO9 | Right Buzzer (+) |

**AI Query Loop (Sense board):**
| Pin | Function |
|-----|----------|
| D3/GPIO4 | Push button OUT |
| D4/GPIO5 | MAX98357A LRC |
| D5/GPIO6 | MAX98357A BCLK |
| D6/GPIO43 | MAX98357A DIN |
| Camera/Mic | Built into Sense expansion board |

## Codebase

```
firmware/              # ESP32 C code (ESP-IDF via PlatformIO)
  src/main.c           # Entry point, safety loop + query loop state machine
  src/camera.c         # OV2640 JPEG capture
  src/mic.c            # PDM microphone PCM recording
  include/helios.h     # Pin definitions, config
  platformio.ini       # Build config (XIAO ESP32S3 Sense target)

server.py              # Pi HTTP server — receives JSON, runs STT→Vision→TTS pipeline
client.py              # Test client for server.py
usb_receiver.py        # USB serial receiver for dev/testing
tests/                 # Test images and scenario scripts
diagram.json           # Wokwi circuit simulator (Safety Loop)
wokwi.toml             # Wokwi firmware pointer
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
- Claude Code (this tool) is pre-configured with Sonnet 4.6 and full permissions
- PlatformIO, clangd, Python, Wokwi simulator are pre-installed
- `.env` has API keys for Cartesia, OpenRouter, etc. — never commit it

## Key Commands

```bash
pio run                      # Build firmware
pio run --target upload      # Flash ESP32 (needs physical board)
python3 server.py            # Run Pi server locally
python3 client.py            # Test the server
```

## Design Constraints

- Raspberry Pi 4B as central hub (required by course)
- XIAO ESP32S3 Sense as pendant controller
- WiFi only active during query transfer (duty cycling for battery)
- All safety loops must work without internet
- Total additional sensor budget: $25
- Must be wearable, discreet, lightweight

## Timeline

- **Week 10 (Mar 24-26):** Core functionality — safety loops + query pipeline working
- **Week 11 (Mar 31-Apr 2):** Integration — pendant + belt communicating
- **Week 12 (Apr 7-9):** Refinement + documentation
- **Week 13 (Apr 14-16):** Final presentation
- **Demo Day: April 21**
