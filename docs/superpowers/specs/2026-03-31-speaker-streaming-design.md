# Speaker Driver + Streaming TTS Design

## Problem

The 3W 8ohm speaker buzzes at volumes above 20%. Root cause: the camera's 20MHz XCLK and PDM mic clock run during playback, coupling noise into shared power rails. The MAX98357A's +15dB gain amplifies that rail noise. Additionally, the current pipeline waits for all TTS audio before playback begins (~5s delay).

## Solution

1. Symmetrical peripheral lifecycle — only one group (capture or playback) active at a time
2. Simple voice-optimized filtering (200Hz high-pass biquad)
3. Streaming TTS from server to ESP32 with 300ms watermark playback

## Peripheral Lifecycle

Only one peripheral group is active at any time:

```
Startup:
  camera_init() + mic_init()          # ready for first query
  speaker NOT init'd                  # MAX98357A powered off (no BCLK)

Button pressed (capture phase):
  speaker remains deinit'd            # amp off, zero noise
  camera + mic active                 # JPEG capture + Opus mic stream

Button released:
  camera_deinit() + mic_deinit()      # free I2S RX, camera DMA/PSRAM
  speaker_init() + speaker_set_volume() # recreate I2S TX channel

Playback (streaming TTS):
  speaker active                      # decoding + playing from PSRAM buffer
  camera + mic deinit'd               # no noise sources

Playback complete:
  speaker_deinit()                    # amp powers off
  camera_init() + mic_init()          # ready for next query
```

## Speaker Improvements

### Volume

- Default: 50% (was 20%)
- `SPK_MAX_SCALE`: 0.90 (unchanged, hardware clipping limit)
- With camera+mic killed during playback, power rail noise is eliminated, allowing higher volume

### 200Hz High-Pass Biquad Filter

2nd-order IIR high-pass at 200Hz. Cuts sub-bass rumble the small speaker can't reproduce. Subsumes the existing DC-blocking filter (biquad already blocks DC).

Coefficients computed from `SPK_SAMPLE_RATE` (24kHz) at init time. Applied per-sample in `speaker_play_opus()` before volume scaling.

Keep the existing gentle low-pass filter as-is.

### New Function: `speaker_deinit()`

```c
void speaker_deinit(void);
```

- `i2s_channel_disable(s_tx_chan)`
- `i2s_del_channel(s_tx_chan)`
- `s_tx_chan = NULL`
- Safe to call when already deinit'd (no-op if NULL)

## New Function: `mic_deinit()`

```c
void mic_deinit(void);
```

- `i2s_channel_disable(s_rx_chan)`
- `i2s_del_channel(s_rx_chan)`
- `s_rx_chan = NULL`
- Safe to call when already deinit'd (no-op if NULL)

## Streaming TTS Protocol

Mirrors the existing mic-to-Pi Opus streaming protocol, reversed:

### Wire Format (Pi -> ESP32 over BLE writes)

```
Start:   0xFFFFFFFF                        (4 bytes — stream beginning)
Data:    [uint16_le frame_len][opus_frame]  (packed into BLE writes)
End:     empty write (0 bytes)             (stream complete)
```

This is the same framing as mic->Pi (`ble_stream_mic_opus`), just in the other direction.

### Server Side (server.py)

1. Replace `tts/bytes` batch API with Cartesia WebSocket streaming (`wss://api.cartesia.ai/tts/websocket`)
2. As PCM chunks arrive from Cartesia, Opus-encode them into frames
3. Buffer 300ms of Opus frames before starting to send over BLE
4. Stream remaining Opus frames as they're encoded: `[uint16_le len][data]`
5. Send `0x0000` end marker when TTS complete, then empty BLE write

### ESP32 Side

**BLE callback (`on_tts_chunk`)**:
- First write: detect `0xFFFFFFFF` start marker, reset buffer state
- Subsequent writes: append raw bytes to PSRAM buffer (no parsing, no allocations)
- Empty write: set `tts_done = true`

**Speaker task (streaming playback)**:
- Starts once `tts_received >= watermark` (300ms of Opus data, ~few KB)
- Reads Opus frames from PSRAM buffer using existing `[len16][data]` format
- Decodes, applies high-pass + low-pass + volume scaling, writes to I2S
- When read pointer reaches `tts_received` and `tts_done` is false: spin-wait for more data
- When read pointer reaches `tts_received` and `tts_done` is true: drain and exit

**Buffer**: Keep existing 256KB PSRAM buffer. No queue, no per-frame allocations.

## Files Changed

| File | Changes |
|------|---------|
| `firmware/src/speaker.c` | Add `speaker_deinit()`, 200Hz biquad filter, streaming playback (watermark-based) |
| `firmware/src/mic.c` | Add `mic_deinit()` |
| `firmware/src/main.c` | Peripheral lifecycle toggling, streaming speaker task, 50% default volume, updated `on_tts_chunk` for start marker |
| `firmware/include/helios.h` | Declare `speaker_deinit()`, `mic_deinit()`, default volume constant |
| `server.py` | WebSocket streaming TTS, Opus encoding, 300ms buffer-then-stream |
