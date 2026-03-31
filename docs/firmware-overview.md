# Helios Firmware — Overview

Helios is a wearable assistive device for vision-impaired users. The pendant (XIAO ESP32S3 Sense) captures camera images and voice, sends them to a Raspberry Pi over BLE, and plays back spoken AI responses through a speaker.

## How It Works (One-Page Summary)

The firmware runs a single interaction loop triggered by a physical button:

```
        BUTTON HELD                          BUTTON RELEASED
 ┌──────────────────────┐          ┌──────────────────────────────┐
 │  Camera captures     │          │  Camera + mic powered OFF    │
 │  JPEG snapshot       │          │  Speaker powered ON          │
 │                      │    ──>   │                              │
 │  Microphone streams  │          │  Opus audio streams in from  │
 │  Opus-encoded voice  │          │  Pi and plays as it arrives  │
 │  to Pi over BLE      │          │  (~1-2s to first audio)      │
 └──────────────────────┘          └──────────────────────────────┘
                                              │
                                              v
                                   Speaker OFF, camera + mic ON
                                   Ready for next query
```

**Key design principle:** Only one peripheral group is active at a time. During recording, the speaker amplifier is powered off. During playback, the camera and microphone are powered off. This eliminates electrical noise that would otherwise cause speaker buzzing, and allows playback at higher volume (50% default vs. the 20% that was previously required).

**The AI pipeline** runs entirely on the Raspberry Pi:
1. Speech-to-text (Cartesia Ink-Whisper)
2. Vision LLM (Claude Haiku — understands the image + spoken question)
3. Text-to-speech (Cartesia Sonic 3 — streamed back as it generates)

The ESP32 handles no AI processing. It captures sensors, streams data, and plays audio.

## Hardware

| Component | Part | Connection |
|-----------|------|------------|
| Microcontroller | XIAO ESP32S3 Sense | USB-C (power + debug) |
| Camera | OV2640 (on Sense board) | DVP parallel, 640x480 JPEG |
| Microphone | MSM261S4030H0 (on Sense board) | PDM, 16kHz 16-bit mono |
| Speaker amp | MAX98357A | I2S, +15dB gain |
| Speaker | 3W 8 ohm | Driven by MAX98357A |
| Button | Momentary push | GPIO 4 |
| Storage | MicroSD (on Sense board) | SPI, config persistence |

## Firmware Modules

### `main.c` — State Machine + Peripheral Lifecycle

The main loop polls the button at 50Hz. On press:

1. **Capture phase** (button held): Takes a JPEG, streams Opus mic audio over BLE
2. **Transition** (button released): Deinits camera + mic, spawns speaker task
3. **Playback phase** (speaker task on Core 1): Waits for 300ms of audio (watermark), inits speaker, streams Opus decode to I2S
4. **Restore** (playback done): Deinits speaker, re-inits camera + mic

Three semaphores coordinate this:
- `spk_done_sem` — prevents starting a new query while playback is running
- `tts_watermark_sem` — signals speaker task that enough audio has arrived to start
- `tts_sem` — signals that the complete TTS stream has been received

### `speaker.c` — Audio Output + DSP

Drives the MAX98357A via I2S (24kHz, 16-bit stereo, Philips format).

**Audio processing chain** (per sample):
```
Opus decode → 200Hz high-pass biquad → low-pass filter → volume scale → stereo duplicate → I2S
```

- **200Hz high-pass**: 2nd-order Butterworth biquad (Q14 fixed-point). Removes DC offset and sub-bass frequencies the small speaker cannot reproduce.
- **Low-pass filter**: Single-pole IIR (alpha=0.85). Gentle high-frequency rolloff.
- **Volume scaling**: 0-100% mapped to 0-90% of full scale. The 90% ceiling prevents clipping given the amp's +15dB gain.

Two playback modes:
- `speaker_play_opus()` — batch: decodes a complete Opus buffer
- `speaker_play_opus_stream()` — streaming: reads from a shared PSRAM buffer while BLE keeps writing to it. Uses `esp_cache_msync` for cross-core coherence (Core 0 writes, Core 1 reads).

### `mic.c` — Microphone Input

PDM microphone on I2S_NUM_0 at 16kHz. Audio is read by `ble.c` which Opus-encodes and streams it.

### `camera.c` — Image Capture

OV2640 via DVP parallel interface. Captures 640x480 JPEG frames into PSRAM. Discards first 3 frames on init for auto-exposure warmup.

### `ble.c` — BLE Communication

NimBLE GATT server with three characteristics:

| Characteristic | Direction | Purpose |
|---------------|-----------|---------|
| Mic TX | ESP → Pi | JPEG image + Opus mic audio (multiplexed) |
| Speaker RX | Pi → ESP | Opus TTS audio stream |
| Control | Bidirectional | Button events, status, volume commands |

**Mic streaming protocol** (ESP → Pi):
```
JPEG:  0xFFFFFFFE + uint32 length + data chunks
Opus:  0xFFFFFFFF + [len16][frame] packets + empty notification (end)
```

**TTS streaming protocol** (Pi → ESP):
```
Start: 0xFFFFFFFF (4 bytes)
Data:  [uint16_le frame_len][opus_frame] (packed into BLE writes)
End:   empty write (0 bytes)
```

### `config.c` / `sdcard.c` — Persistent Settings

Configuration stored as JSON on SD card (`/sdcard/helios_cfg.json`). Stores volume, button idle level, BLE device name, TTS voice ID, and sample rate.

## Pin Map

```
GPIO 1  — Left ultrasonic TRIG       GPIO 7  — SD SCK
GPIO 2  — Left ultrasonic ECHO       GPIO 8  — SD MISO
GPIO 3  — Left buzzer                GPIO 9  — SD MOSI
GPIO 4  — Button                     GPIO 10 — Camera XCLK
GPIO 5  — Speaker LRC (I2S WS)      GPIO 21 — SD CS
GPIO 6  — Speaker BCLK              GPIO 39 — Camera SIOC
GPIO 7  — Right ultrasonic TRIG     GPIO 40 — Camera SIOD
GPIO 8  — Right ultrasonic ECHO     GPIO 41 — Mic DATA
GPIO 9  — Right buzzer              GPIO 42 — Mic CLK
GPIO 43 — Speaker DIN               GPIO 13 — Camera PCLK
```

## Memory Layout

| Resource | Size | Location |
|----------|------|----------|
| Firmware | 923 KB | Flash (88% of 1 MB) |
| Static RAM | 35 KB | Internal SRAM (11% of 328 KB) |
| TTS buffer | 256 KB | PSRAM (cache-line aligned) |
| Camera framebuffers | 2x ~300 KB | PSRAM |
| Opus decode buffers | ~6 KB | PSRAM |

## Build + Flash

```bash
# Build (on build server)
cd helios-build && pio run -e xiao_esp32s3

# Flash (ESP connected via USB)
esptool --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 \
  write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

## Dependencies

- **ESP-IDF** via PlatformIO (framework)
- **esp32-camera** >=2.1.0 (camera driver)
- **esp-opus** ^1.0.5 (Opus codec for mic encoding + speaker decoding)
- **NimBLE** (BLE stack, built into ESP-IDF)
