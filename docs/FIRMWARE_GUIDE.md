# ESP32S3 Firmware — Code Walkthrough

**Target:** XIAO ESP32S3 Sense (pendant unit)
**Framework:** ESP-IDF 5.5 via PlatformIO
**Language:** C

> This document explains the firmware without modifying the source code.
> Use this as a reference when presenting or explaining the system.

---

## File Map

```
firmware/src/
├── main.c      — Entry point, button handler, query state machine
├── ble.c       — BLE GATT server, JPEG/Opus multiplexing protocol
├── camera.c    — OV3660 camera init and JPEG capture
├── mic.c       — PDM microphone init (16kHz mono)
├── speaker.c   — I2S output, Opus decode, audio filters, volume control
└── config.c    — NVS flash persistence (volume, button calibration)

firmware/include/
└── helios.h    — Pin definitions, API declarations, BLE protocol constants
```

---

## main.c — The Brain

### Boot Sequence (app_main)
1. Initialize mic (PDM, I2S_NUM_0)
2. Initialize camera (OV3660, DVP interface)
3. Load config from NVS flash (volume, button idle level)
4. Initialize button GPIO (GPIO4, internal pull-up)
5. Allocate TTS buffer (256KB in PSRAM)
6. Initialize BLE (NimBLE stack, GATT services)
7. Wait for BLE connection from Pi
8. Enter main voice loop

### Main Voice Loop
```
while (1):
    if button_pressed:
        1. Wait for previous speaker playback to finish
        2. Reset TTS buffer state
        3. Notify Pi: BUTTON_PRESSED
        4. Capture JPEG → send over BLE (ble_send_jpeg)
        5. Stream mic Opus → send over BLE (ble_stream_mic_opus)
        6. Notify Pi: BUTTON_RELEASED
        7. Deinit camera + mic (free power for speaker)
        8. Spawn speaker_task on Core 1
        9. Wait for button release
    sleep 20ms
```

### Speaker Task (runs on Core 1)
- Waits for TTS watermark (enough Opus data buffered)
- Initializes speaker (I2S_NUM_1)
- Streams Opus decode → filter → I2S output
- When done: deinits speaker, re-inits camera + mic
- Signals completion via semaphore

### Why Camera + Mic Are Killed During Playback
The MAX98357A speaker amp and the camera share the same power rail.
Running both causes audible noise in the speaker output. By deiniting
camera + mic before playback and re-initing after, we get clean audio.
This is a hardware constraint, not a software limitation.

---

## ble.c — BLE Communication

### GATT Service Structure
```
Service: 87654321-4321-4321-4321-cba987654321
├── Mic TX    (...4322) — NOTIFY     — ESP→Pi data
├── Speaker RX (...4323) — WRITE     — Pi→ESP data
└── Control   (...4324) — R/W/NOTIFY — Commands both ways
```

### Multiplexed Data Protocol (Mic TX characteristic)
The ESP32 sends JPEG and Opus audio over the same BLE characteristic
using marker bytes to distinguish them:

```
JPEG transfer:
  [0xFE 0xFF 0xFF 0xFF] [uint32_le length]   ← 8-byte header
  [JPEG data in 509-byte chunks]              ← paced with 10ms delays

Opus audio transfer:
  [0xFF 0xFF 0xFF 0xFF]                       ← 4-byte start marker
  [uint16_le frame_len] [opus_frame_data]     ← repeated per 20ms frame
  [empty notification]                        ← end marker
```

### Flow Control
- JPEG chunks: retry up to 20 times on BLE_HS_ENOMEM/EBUSY
- Opus frames: batched into 509-byte BLE packets for efficiency
- Minimum 500ms recording enforced (25 Opus frames)
- BLE write lock prevents concurrent writes from main loop + speaker

### Control Commands
Commands are single-byte IDs with optional payload:
- 0x01 CONNECTED: Pi tells ESP32 it's connected
- 0x02 PROCESSING: Pi is running the AI pipeline
- 0x03 SET_VOLUME: Set speaker volume (1 byte: 0-100)
- 0x05 REQUEST_STATUS: ESP32 replies with heap/volume info
- 0x10 BUTTON_PRESSED: Recording started
- 0x11 BUTTON_RELEASED: Recording stopped
- 0x12 PLAYBACK_DONE: Speaker finished playing

---

## camera.c — Image Capture

- Sensor: OV3660 (detected automatically via SCCB/I2C)
- Output: VGA (640x480) JPEG, quality level 6
- Interface: DVP (Digital Video Port) with PSRAM DMA
- 3 warmup frames discarded on init (auto-exposure settle)
- Single-shot capture: `camera_capture_jpeg()` returns pointer + length
- Frame buffer returned via `camera_return_fb()` after BLE send

---

## mic.c — Audio Capture

- Sensor: MSM261S4030H0 PDM MEMS microphone (built into Sense board)
- Sample rate: 16kHz, 16-bit mono
- Interface: I2S_NUM_0 in PDM RX mode
- DMA: 8 descriptors x 1024 frames
- The raw I2S channel handle (`s_rx_chan`) is accessed directly by ble.c
  for real-time Opus encoding during streaming

---

## speaker.c — Audio Output

- DAC: MAX98357A I2S amplifier
- Interface: I2S_NUM_1 in standard (Philips) mode
- Sample rate: 24kHz, 16-bit stereo (mono duplicated to both channels)
- Volume: software scaling (0-100%), capped at 0.90 to prevent amp clipping

### Audio Processing Chain
```
Opus frame (from BLE)
  → opus_decode() — Opus library, 20ms frames
  → 200Hz high-pass biquad filter — removes DC offset + sub-bass rumble
  → Low-pass smoothing filter — reduces high-frequency harshness
  → Volume scaling — integer math, clamped to int16 range
  → Mono-to-stereo duplication — MAX98357A expects stereo I2S
  → i2s_channel_write() — DMA to speaker
```

### Streaming Playback (speaker_play_opus_stream)
The speaker doesn't wait for all TTS data to arrive. It reads from a
shared PSRAM buffer while BLE is still writing to it:
- `tts_buf`: 256KB ring buffer in PSRAM
- `tts_received`: volatile write pointer (BLE callback increments)
- `tts_done`: volatile flag (set when BLE receives end marker)
- Cache coherence: `esp_cache_msync()` after every write/before every read
- Watermark: playback starts after ~900 bytes buffered (~300ms)

---

## config.c — Persistent Settings

Uses ESP-IDF NVS (Non-Volatile Storage) flash partition:
- `speaker_volume`: 0-100, default 50
- `button_idle_level`: 0 or 1, or -1 for auto-detect

Config survives power cycles. Erasing NVS (`pio run --target erase`)
forces re-detection of button idle level.

---

## helios.h — Pin Map

### Camera (DVP bus)
```
XCLK=10, SIOD=40, SIOC=39, VSYNC=38, HREF=47, PCLK=13
D0-D7: 15, 17, 18, 16, 14, 12, 11, 48
```

### Microphone (PDM)
```
CLK=42, DATA=41
```

### Speaker (I2S to MAX98357A)
```
LRC/WS=5, BCLK=6, DIN=43
```

### Button
```
GPIO4, internal pull-up, active-low (press = LOW)
```

---

## Build Environments

| Environment | LOG_LEVEL | Use |
|-------------|-----------|-----|
| `xiao_esp32s3` | NONE | Production — silent, minimal overhead |
| `xiao_esp32s3_debug` | INFO | Development — full serial logging |

```bash
pio run -e xiao_esp32s3            # Build production
pio run -e xiao_esp32s3_debug      # Build debug
pio run --target upload             # Flash to board
pio run --target erase              # Erase NVS (reset config)
```
