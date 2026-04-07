# Project Helios — System Status & Architecture

**Last updated:** April 7, 2026
**Prepared by:** Kaden Schutt (Integration Lead)

> Note: AI assistance (Claude) was used to help structure this document.

---

## 1. System Overview

Helios is a wearable assistive device for vision-impaired individuals consisting of two physical units:

1. **Pendant** (XIAO ESP32S3 Sense) — worn on chest
2. **Belt Unit** (Raspberry Pi 4B) — worn in fanny pack
3. **Safety Modules** (RP2040 x3) — distributed ultrasonic obstacle detection

---

## 2. Architecture — Three Independent Loops

### Loop 1: Forward Safety (RP2040 boards, always on, no network)

```
 ┌─────────────────────────────────────────────────┐
 │              RP2040 Safety Module                │
 │                                                  │
 │   HC-SR04 Sensor                                 │
 │       │                                          │
 │       ▼                                          │
 │   get_distance()  ──► 10ms poll rate             │
 │       │                                          │
 │       ▼                                          │
 │   History Buffer (10 samples)                    │
 │       │                                          │
 │       ├──► avg_distance() (smoothed)             │
 │       └──► approach_velocity() (cm/s)            │
 │               │                                  │
 │               ▼                                  │
 │   ┌─────────────────────────┐                    │
 │   │     STATE MACHINE       │                    │
 │   │                         │                    │
 │   │  IDLE ──────► NOTICED   │                    │
 │   │   ▲    <100cm    │      │                    │
 │   │   │   approach   │      │                    │
 │   │   │   detected   ▼      │                    │
 │   │   │          TRACKING   │                    │
 │   │   │              │      │                    │
 │   │   │   >100cm or  │      │                    │
 │   │   └──── static   │      │                    │
 │   │         5s       │      │                    │
 │   └─────────────────────────┘                    │
 │               │                                  │
 │               ▼                                  │
 │        Active-Low Buzzer (GP8)                   │
 │                                                  │
 │   Zones:                                         │
 │     <20cm  = SOLID (danger)                      │
 │     <40cm  = 100ms beep (close)                  │
 │     <70cm  = 300ms beep (warning)                │
 │     <100cm = 600ms beep (far)                    │
 │     >100cm = silent                              │
 └─────────────────────────────────────────────────┘
```

**State Transitions:**

```
                    object <100cm
                    + approaching
                    (3 consecutive)
    ┌──────┐  ─────────────────►  ┌──────────┐
    │ IDLE │                      │ NOTICED  │
    └──────┘  ◄─────────────────  └──────────┘
                object left or         │
                stopped closing        │ object <70cm
                                       ▼
                                 ┌──────────┐
                                 │ TRACKING │
                                 └──────────┘
                                       │
                             ┌─────────┴──────────┐
                             ▼                     ▼
                       object left           still 5s+
                       (>100cm)            ┌────────────┐
                             │             │  STATIC    │
                             ▼             │  (silent)  │
                        ┌──────┐           └────────────┘
                        │ IDLE │                 │
                        └──────┘            movement
                                            resumes
                                               │
                                               ▼
                                          ┌──────────┐
                                          │ TRACKING │
                                          │ (beeping)│
                                          └──────────┘
```

### Loop 2: Rear Safety (Raspberry Pi, always on, no network)

```
 ┌──────────────────────────────────────┐
 │          Raspberry Pi GPIO           │
 │                                      │
 │   Rear HC-SR04 ──► get_distance()    │
 │                        │             │
 │                        ▼             │
 │                 Vibration Motor      │
 │                 (PWM proportional)   │
 │                                      │
 │   Status: DEGRADED (wiring issues)   │
 └──────────────────────────────────────┘
```

### Loop 3: AI Query (Pendant + Belt, on-demand via button)

```
 ┌─────────┐          BLE           ┌──────────────┐
 │  ESP32  │ ◄─────────────────────►│  Raspberry   │
 │  S3     │                        │  Pi 4B       │
 │ Sense   │                        │              │
 └────┬────┘                        └──────┬───────┘
      │                                    │
      │  1. Button press                   │
      │  2. Camera JPEG capture            │
      │  3. Mic Opus stream     ──────►    │
      │                                    │
      │                              4. Cartesia STT
      │                                 (Ink-Whisper)
      │                                    │
      │                              5. Claude Haiku
      │                                 (Vision LLM)
      │                                    │
      │                              6. Cartesia TTS
      │                                 (Sonic 3)
      │                                    │
      │  7. Opus audio playback ◄──────    │
      │  8. Speaker output                 │
      │                                    │
```

**Detailed AI Query Pipeline:**

```
 Button Hold
     │
     ▼
 ┌──────────────────┐
 │ camera_capture()  │ → JPEG (~10-12KB)
 │ ble_send_jpeg()   │ → BLE notify (509-byte chunks)
 └──────────────────┘
     │
     ▼
 ┌──────────────────┐
 │ mic Opus stream   │ → 20ms frames @ 16kHz
 │ (while held)      │ → Opus encode → BLE notify
 └──────────────────┘
     │
 Button Release
     │
     ▼
 ┌──────────────────┐    ┌─────────────────────────────┐
 │ ESP32 deinits     │    │ Pi: server_ble.py            │
 │ camera + mic      │    │                              │
 │ (power savings)   │    │  1. Decode Opus → PCM        │
 └──────────────────┘    │  2. Cartesia STT WebSocket   │
     │                    │     → transcript              │
     │                    │  3. Anthropic Claude Haiku    │
     │                    │     + JPEG → response text    │
     │                    │  4. Cartesia TTS WebSocket    │
     │                    │     → PCM → Opus encode       │
     │                    │  5. BLE write Opus chunks     │
     │                    └─────────────────────────────┘
     │                              │
     ▼                              │
 ┌──────────────────┐               │
 │ speaker_task()    │ ◄────────────┘
 │ (Core 1)          │
 │  - Opus decode    │
 │  - HP filter      │
 │  - Volume scale   │
 │  - I2S output     │
 └──────────────────┘
     │
     ▼
 ┌──────────────────┐
 │ Restore camera    │
 │ + mic             │
 │ Ready for next    │
 └──────────────────┘
```

---

## 3. Hardware Status

### ESP32S3 Sense (Pendant)

| Component | Pin(s) | Status | Verified |
|-----------|--------|--------|----------|
| Camera (OV3660) | DVP bus (10 pins) | **OK** | Apr 7 — 3 JPEG frames captured |
| PDM Mic | GPIO42 (CLK), GPIO41 (DATA) | **OK** | Apr 7 — 16kHz mono init |
| Speaker (MAX98357A) | GPIO5 (LRC), GPIO6 (BCLK), GPIO43 (DIN) | **OK** | Needs wall power on MacBook |
| Button | GPIO4 | **OK** | Internal pull-up enabled, 2-pin config |
| BLE (NimBLE) | Internal | **OK** | Advertising as "Helios" |
| PSRAM | Internal | **OK** | 8MB detected, test passed |
| Battery charging | USB-C | **OK** | Built-in LiPo charging circuit |

### RP2040 Safety Modules (x3)

| Board | Firmware | Pins | Status |
|-------|----------|------|--------|
| Board 1 | MicroPython 1.27 + safety_loop | GP0/1/8 | **OK** |
| Board 2 | MicroPython 1.27 + safety_loop | GP0/1/8 | **OK** |
| Board 3 | MicroPython 1.27 + safety_loop | GP0/1/8 | **OK** |

**Pin Map:**
- GP0 = HC-SR04 TRIG
- GP1 = HC-SR04 ECHO
- GP8 = Buzzer (active-low)
- 3V3 = Sensor VCC
- GND = Common ground

**Note:** GP4/GP5 dead on Board 1 — avoided. All boards use GP0/GP1.

### Raspberry Pi 4B (Belt Unit)

| Component | Status | Detail |
|-----------|--------|--------|
| BLE server | **OK** | systemd service, auto-starts on boot |
| Python venv | **OK** | bleak, opuslib, anthropic, httpx, etc. |
| libopus | **OK** | apt installed |
| Bluetooth | **DEGRADED** | MTU stuck at 23 (should be 512) |
| GPIO sensors | **DEGRADED** | Right sensor pins (22/23) unreliable |

---

## 4. Software Components

### Firmware (ESP32, C/ESP-IDF)

| File | Purpose | Status |
|------|---------|--------|
| `main.c` | Entry point, button handler, state machine | **OK** |
| `camera.c` | OV3660 JPEG capture | **OK** |
| `mic.c` | PDM microphone, 16kHz mono | **OK** |
| `speaker.c` | I2S output, Opus decode, HP filter, volume | **OK** |
| `ble.c` | NimBLE GATT server, JPEG/Opus multiplexing | **OK** |
| `config.c` | NVS persistent config (volume, button level) | **OK** |

### Server (Pi, Python)

| File | Purpose | Status |
|------|---------|--------|
| `server_ble.py` | BLE central, STT→LLM→TTS pipeline | **OK** (MTU issue) |
| `server.py` | HTTP/WiFi variant (legacy) | **OK** |

### Safety Loop (RP2040, MicroPython)

| File | Purpose | Status |
|------|---------|--------|
| `tests/safety_loop_rp2040.py` | Smart approach detection state machine | **OK** |
| `tests/ultrasonic_rp2040.py` | Raw multi-sensor test utility | **OK** |

### Pi GPIO Safety (Python)

| File | Purpose | Status |
|------|---------|--------|
| `lefttest.py` | Pi-side sensor + buzzer test (Mohamed) | **BUGGY** — GPIO 22/23 issues |

---

## 5. Known Issues & Bugs

### Critical

| Issue | Impact | Workaround |
|-------|--------|------------|
| BLE MTU = 23 on Pi | JPEG transfer slow/truncated, mic stream drops end marker | Run from MacBook for full functionality |
| No USB port on 3D enclosure | Cannot reflash firmware once sealed | Flash final firmware before assembly |

### Moderate

| Issue | Impact | Workaround |
|-------|--------|------------|
| Mic gain too low | Must hold device to face for STT | Software gain stage needed (not yet implemented) |
| Speaker brownout on MacBook USB | MAX98357A cuts out during playback | Plug MacBook into wall power |
| Static suppression unverified | 5s timeout may not trigger reliably | Distance-range method implemented, needs field testing |
| Conversation mode disabled | Each query is one-shot, no context | Intentional for now to avoid ESP memory issues |

### Minor / Hardware

| Issue | Impact | Workaround |
|-------|--------|------------|
| Pi GPIO 22/23 unreliable | Right sensor on Pi doesn't work | Use RP2040 boards instead |
| Board 1 GP4/GP5 dead | Two pins unusable on one RP2040 | Use GP0/GP1 on all boards |
| Rear sensor on Pi erratic | Reads ~1.5cm constantly (floating echo) | Hardware rewire needed |

---

## 6. API Pipeline

```
┌─────────────────────────────────────────────────────┐
│                   Cloud Services                     │
│                                                      │
│   ┌──────────────┐   ┌──────────────┐   ┌────────┐ │
│   │  Cartesia    │   │  Anthropic   │   │Cartesia│ │
│   │  Ink-Whisper │   │  Claude      │   │Sonic 3 │ │
│   │  (STT)       │   │  Haiku 4.5   │   │ (TTS)  │ │
│   │              │   │  (Vision)    │   │        │ │
│   │  WebSocket   │   │  REST API    │   │  REST  │ │
│   │  Streaming   │   │              │   │  or WS │ │
│   └──────┬───────┘   └──────┬───────┘   └───┬────┘ │
│          │                  │                │      │
│    PCM audio in       image + text      text in     │
│    → transcript       → response        → PCM out   │
│                                                      │
│   Round-trip: ~5-6 seconds                           │
└─────────────────────────────────────────────────────┘
```

---

## 7. Build & Deploy Commands

### ESP32 Firmware
```bash
pio run                                    # Build (production, LOG_NONE)
pio run -e xiao_esp32s3_debug              # Build (debug, LOG_INFO)
pio run --target upload --upload-port /dev/cu.usbmodem2101   # Flash
```

### RP2040 Safety Loop
```bash
mpremote connect /dev/cu.usbmodem101 cp tests/safety_loop_rp2040.py :main.py
mpremote connect /dev/cu.usbmodem101 reset
```

### Pi BLE Server
```bash
sudo systemctl start helios-ble            # Start
sudo systemctl stop helios-ble             # Stop
sudo systemctl restart helios-ble          # Restart
sudo journalctl -u helios-ble -f           # Live logs
```

### Pi Sensor Test
```bash
ssh root@192.168.68.113
cd "/home/pi/sensor test"
python3 lefttest.py
```

---

## 8. Team Responsibilities

| Member | Role | Status |
|--------|------|--------|
| Kaden Schutt | Integration lead, firmware, API pipeline, safety loop | Ahead of schedule |
| Jeremy Branom | 3D housing, fanny-pack hardware | In progress |
| Mohamed Tigana | Pi software, GPIO, rear safety loop | Needs GPIO debug |
| Raghav Pahuja | Pendant hardware, WiFi/HTTP, prompt engineering | In progress |
| Anikesh Gupta | Button/GPIO integration research | In progress |

---

## 9. Remaining Work (Pre-Demo)

1. Fix BLE MTU on Pi (bluez/bleak negotiation)
2. Add mic software gain stage (~4-8x amplification)
3. Implement speaker playback abort (button press during TTS)
4. Field test safety loop distance thresholds
5. Add USB port cutout to 3D enclosure OR finalize firmware before sealing
6. Wire rear vibration motor on belt unit
7. Integration test: all three loops running simultaneously
8. Documentation and presentation prep

---

*Demo Day: April 21, 2026*
