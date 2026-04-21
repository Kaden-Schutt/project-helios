# Helios OTA + Recovery Stack

_A signed, WiFi-first, SD-backed, BLE-rescue-armed firmware pipeline for the Helios pendant._

## Why this exists

The pendant (XIAO ESP32-S3 Sense) is embedded deep in the wearable assembly. The USB port is a mounting hazard — we tore our housing once pulling on it. This stack ensures that after a single initial USB flash, **all further firmware iteration happens over-the-air**: WiFi first, Bluetooth as a fallback, SD card as a safety net. Bad firmware can't permanently brick the device.

## Three tiers of recovery

```
+----------------------------------------------------------+
| Push signed fw via /ota  ──→  validates HMAC             |
|                               writes to inactive slot    |
|                               reboots into it            |
|                                                          |
| ┌─ if app crashes before mark_valid ─────────┐           |
| │  Tier 1: ESP-IDF bootloader rollback       │           |
| │  reverts to previous OTA slot after N boots│           |
| └────────────────────────────────────────────┘           |
|                                                          |
| ┌─ if both OTA slots unusable ───────────────┐           |
| │  Tier 2: SD-backed recovery                │           |
| │  /sd/recovery.signed.bin → inactive slot   │           |
| │  (auto-maintained on every successful OTA) │           |
| └────────────────────────────────────────────┘           |
|                                                          |
| ┌─ if WiFi unreachable (wrong network, etc) ─┐           |
| │  Tier 3: BLE rescue                        │           |
| │  /sd/ble_recovery.signed.bin loaded        │           |
| │  advertises "Helios-Recovery"              │           |
| │  accepts signed fw over BLE GATT           │           |
| └────────────────────────────────────────────┘           |
+----------------------------------------------------------+
```

## Components

### Firmware

| Directory | Purpose |
|---|---|
| `firmware/diag/camera_ota/` | **Main diag/OTA firmware.** Camera + mic + button probes, HTTP server with signed `/ota`, signed SD recovery, WiFi multi-network, BLE fallback pivot. |
| `firmware/diag/ble_recovery/` | **Standalone BLE rescue firmware.** NimBLE GATT server accepting signed firmware uploads, writes to inactive OTA slot + reboots. No WiFi, no peripherals. |
| `firmware/diag/camera/` | Earlier standalone XIAO camera diagnostic (kept for reference — USB-only, no WiFi). |
| `firmware/diag/camera_s3cam/` | Variant of the camera diag for the Goouuu ESP32-S3-CAM pinout. |
| `firmware/diag/speaker/` | Earlier MAX98357A + TTS speaker diagnostic (superseded after the MAX approach was dropped). |

### Host-side scripts

| Script | Purpose |
|---|---|
| `scripts/gen_ota_key.py` | Generate the one-time HMAC-SHA256 signing secret. Writes `~/.helios/ota_sign.key` (private, `chmod 600`) and `firmware/diag/camera_ota/ota_pubkey.h` (embedded). |
| `scripts/sign_ota.py` | Sign any `firmware.bin` → `firmware.signed.bin` (appends 32-byte HMAC tag). |
| `scripts/ble_ota.py` | Push a signed firmware over BLE to a device advertising `Helios-Recovery`. Requires `pip install bleak`. |

### SD card layout

The main firmware auto-mounts microSD at `/sd` on boot. Files the firmware knows about:

| Path | Writer | Purpose |
|---|---|---|
| `/sd/wifi.conf` | user (via `POST /wifi`) | Network priority list. See format below. |
| `/sd/recovery.signed.bin` | firmware (on `mark_valid`) | Last-known-good signed main fw. Loaded into inactive slot if boot counter exceeds threshold. |
| `/sd/recovery.staging.bin` | firmware (during `POST /ota`) | Staging area; promoted atomically on `mark_valid`. |
| `/sd/ble_recovery.signed.bin` | user (initial setup) | Standalone BLE rescue fw. Loaded into inactive slot when WiFi connect fails. |

Card is ordinary FAT32, formatted with `diskutil eraseDisk FAT32 HELIOS MBRFormat /dev/diskN`.

## HTTP endpoints

All served on port 80 by the running main firmware.

| Method + path | Purpose |
|---|---|
| `GET /info` | JSON: app name, compile time, running/boot partition, heap (internal + PSRAM), uptime. |
| `GET /logs` | Ring-buffered `[CAMDIAG]` log (16 KB, chunked, PSRAM-staged). |
| `GET /frame` | Latest captured JPEG (image/jpeg). Headers: `X-Frame-Id`, `X-Frame-Age-Ms`. |
| `GET /button` | JSON: `{pressed, presses_total, last_press_ms_ago}`. |
| `GET /mic` | JSON: `{rms, peak, frames_read}` — rolling ~1 s window. |
| `GET /pins` | Candidate GPIO sweep — 4 s scan with both pulldown and pullup, min/max per pin. Useful for identifying button wiring problems. |
| `GET /wifi` | Current `/sd/wifi.conf` contents. |
| `POST /wifi` | Replace `/sd/wifi.conf`. Body is new file contents. Reboot required to apply. |
| `POST /reboot` | Unmount SD (for clean writes) + reboot. |
| `POST /ota` | Flash a signed firmware image. Body is `firmware.signed.bin` (firmware + 32-byte HMAC tag). Rejects with 401 on bad signature. Reboots into new slot on success. |

## Signing

Symmetric HMAC-SHA256 with a 32-byte key. Chosen over asymmetric (Ed25519/RSA) because ESP-IDF 5.5's bundled mbedTLS doesn't expose Ed25519 and our threat model doesn't require public-key crypto — the goal is "prevent accidental mismatched firmware," not "defend against a determined attacker with flash access."

### Key management

- Private (HMAC secret): `~/.helios/ota_sign.key` — hex string, chmod 600. **Never committed.** Back it up somewhere safe.
- Public (same secret, embedded): `firmware/diag/camera_ota/ota_pubkey.h` and `firmware/diag/ble_recovery/ota_pubkey.h`. **Gitignored.** Regenerated by `scripts/gen_ota_key.py`.

If the secret is lost or rotated, every previously-built signed `.bin` becomes invalid. Only a USB reflash can rotate the embedded secret.

### Signing a firmware

```bash
python3 scripts/sign_ota.py firmware.bin
# writes firmware.signed.bin (firmware + 32-byte HMAC tag)
```

### Verification path (on-device)

- `firmware/diag/camera_ota/ota_verify.c` — incremental HMAC-SHA256 via mbedTLS, constant-time tag compare.
- Invoked by `POST /ota` (streaming over HTTP) and by `recovery_pivot_to_ble()` / `recovery_boot_check()` (whole-file over SD).

## First-time setup (one USB flash)

```bash
# 1. Generate signing key
python3 scripts/gen_ota_key.py

# 2. Build (on remote build host, per project convention)
ssh kaden@k9lin.local 'cd ~/helios-diag-camera-ota && pio run'
scp kaden@k9lin.local:helios-diag-camera-ota/.pio/build/xiao_esp32s3/{bootloader.bin,partitions.bin,firmware.bin} /tmp/helios-bins/staged/

# 3. Build BLE rescue (same pattern), sign, stage for SD
ssh kaden@k9lin.local 'cd ~/helios-ble-recovery && pio run'
scp kaden@k9lin.local:helios-ble-recovery/.pio/build/xiao_esp32s3/firmware.bin /tmp/helios-bins/staged/ble_recovery.bin
python3 scripts/sign_ota.py /tmp/helios-bins/staged/ble_recovery.bin

# 4. Flash the XIAO over USB
/tmp/helios-bins/staged/FLASH_WHEN_PLUGGED.sh

# 5. Prep SD card
diskutil eraseDisk FAT32 HELIOS MBRFormat /dev/diskN  # whichever is your SD
cp /tmp/helios-bins/staged/ble_recovery.signed.bin /Volumes/HELIOS/
# Also sign firmware.bin and copy as recovery.signed.bin so Tier 2 has
# a fallback from the start:
python3 scripts/sign_ota.py /tmp/helios-bins/staged/firmware.bin --out /Volumes/HELIOS/recovery.signed.bin
diskutil eject /Volumes/HELIOS

# 6. Insert SD into the XIAO, unplug USB, run on battery
```

From here the device is on WiFi + BLE-rescue-armed. USB is no longer needed.

## Day-to-day: pushing new firmware

```bash
# Edit and rebuild on k9lin, pull the bin back
ssh kaden@k9lin.local 'cd ~/helios-diag-camera-ota && pio run'
scp kaden@k9lin.local:helios-diag-camera-ota/.pio/build/xiao_esp32s3/firmware.bin /tmp/helios-bins/staged/

# Sign + push
python3 scripts/sign_ota.py /tmp/helios-bins/staged/firmware.bin
curl --data-binary @/tmp/helios-bins/staged/firmware.signed.bin http://helios-cam.local/ota
```

The device:
1. Validates HMAC over the body (reject → 401).
2. Writes firmware into the inactive OTA slot.
3. Stages a copy to `/sd/recovery.staging.bin`.
4. On `esp_ota_set_boot_partition` success, reboots.
5. On fresh boot, WiFi + HTTP come up, `mark_app_valid` fires, rollback is cancelled, staging is promoted to `recovery.signed.bin`.

Whole cycle: ~15 s from `curl` return to new fw serving `/info`.

## WiFi configuration

### Multi-network `wifi.conf` format

```
# comments allowed
ssid1=Schutt Home
psk1=kadi0215
ssid2=Raspberry Pi
psk2=
ssid3=Kaden's iPhone
psk3=kadi0215
```

- Each `ssidN`/`pskN` pair is one network; up to 16.
- Empty `pskN=` means open network.
- Unnumbered `ssid=`/`psk=` is a legacy single-network form, still supported.
- Priority = file order. First that connects wins (12 s timeout each).
- Comments start with `#`.

### Pushing a new `wifi.conf` over the air

```bash
curl -X POST --data-binary @wifi.conf http://helios-cam.local/wifi
curl -X POST http://helios-cam.local/reboot   # apply
```

Reboot is required (the file is read once at boot). The reboot handler unmounts SD cleanly before `esp_restart` so FatFS flushes.

### Fallback order

1. `/sd/wifi.conf` (if file present and parses at least one entry)
2. Compiled-in `WIFI_SSID`/`WIFI_PASSWORD` from `firmware/include/wifi_credentials.h`
3. If all entries fail → BLE rescue pivot

## BLE rescue

Triggered when WiFi connect fails and `/sd/ble_recovery.signed.bin` is present + verified. The main fw:
1. Reads `ble_recovery.signed.bin` from SD.
2. Verifies HMAC against the embedded key.
3. Writes into the inactive OTA slot.
4. `esp_ota_set_boot_partition` + `esp_restart`.

Device boots into `Helios-Recovery` BLE app. Advertises with service UUID `0xFFE0`.

To push a firmware over BLE from the Mac:

```bash
pip install bleak
python3 scripts/sign_ota.py firmware.bin
python3 scripts/ble_ota.py firmware.signed.bin
```

The client scans for `Helios-Recovery`, writes `fw_size` characteristic with the body length, then streams chunks via `fw_chunk`. When device-side byte count equals `fw_size`, it verifies HMAC and reboots into the new slot.

After a successful BLE rescue push, the new main fw boots, reconnects to WiFi normally, and resumes OTA service. The BLE rescue image stays available on SD for future fallbacks.

## Troubleshooting

### Device not reachable on WiFi

```bash
# See if the router knows about it:
arp -a | grep 1c:db:d4   # last 6 chars of the MAC

# Force a macOS mDNS refresh:
dscacheutil -flushcache
```

If it's on the LAN (ARP entry present) but HTTP times out, the app may have crashed before `mark_valid`. Wait a few minutes — bootloader rollback will trigger after 3 failed boots.

### Bad OTA pushed

1. **Before `mark_valid`** (app crashed) → Tier 1 auto-reverts after 3 boots.
2. **After `mark_valid`** (app runs but has a bug that prevents new OTAs) → force Tier 2/3:
   - Move device to an unknown WiFi network → BLE rescue engages.
   - Or power-cycle with SD inserted and remove `/sd/recovery.signed.bin` temporarily — actually this makes it worse, don't do that.
   - Last resort: USB reflash.

### `/wifi` POST returns 405 Method Not Allowed

`cfg.max_uri_handlers` is saturated. We set it to 16 — if you've added handlers in a fork, bump this.

### `wifi.conf` not persisting across reboot

Call `POST /reboot` (which unmounts SD before restarting), not a manual power-cycle, within the first session after the POST. FatFS buffers directory writes until unmount or timeout.

### Signing fails

`~/.helios/ota_sign.key` must be 64 hex chars (32 bytes). Regenerate with `scripts/gen_ota_key.py --force` if corrupted — but this invalidates every previously-built signed bin. You'll need to USB-reflash to rotate the embedded public key.

## Threat model

The HMAC-SHA256 signature prevents:
- Accidentally pushing the wrong firmware
- Corrupted uploads (transfer errors, truncation)
- Someone on the LAN with a firmware they built against a different key

It does **not** prevent:
- An attacker who can dump the ESP's flash (they'll extract the HMAC key and sign their own firmware)
- An attacker with physical USB access (who can reflash directly)

For the Helios project (wearable assistive device, demo context), this is the right tradeoff. If the device goes to production with stronger requirements, swap HMAC for Ed25519 (requires bundling a reference implementation like TweetNaCl — ~1000 lines of portable C) or enable ESP-IDF Secure Boot V2 (RSA-3072, burns eFuses, one-way).

## Pin map (XIAO ESP32-S3 Sense)

Current allocation in `firmware/diag/camera_ota/`:

| Pin | Function | Notes |
|---|---|---|
| GPIO 10, 39, 40, 38, 47, 13, 15, 17, 18, 14, 12, 11, 48 | OV3660 DVP + SCCB | Built into the Sense expansion |
| GPIO 41, 42 | PDM microphone | MSM261S4030H0, 16 kHz |
| GPIO 7, 8, 9, 21 | microSD SPI | onboard pullups to VCC on all four |
| **GPIO 44 (D7)** | Button OUT | Active-high 3-pin breakout. Moved here from D3/D9 because D9=GPIO8 collides with the SD pullup. |

## File map of this project's OTA contributions

```
firmware/diag/
├── camera_ota/              # Main diag/OTA fw
│   ├── main.c               # boot → SD → recovery check → camera → WiFi → OTA server
│   ├── ota.c                # HTTP handlers (all endpoints)
│   ├── ota_verify.c/.h      # HMAC-SHA256 verification
│   ├── ota_pubkey.h         # (gitignored) HMAC key
│   ├── sd_card.c/.h         # SDSPI mount + wifi.conf parser
│   ├── recovery.c/.h        # Tier 1+2+3 logic
│   ├── button.c/.h          # GPIO44 debounced poll
│   ├── mic_probe.c/.h       # PDM rolling RMS/peak
│   ├── diag_log.c/.h        # DLOG → stdout + ring buffer for /logs
│   ├── camera.c, wifi.c, mic.c  # drivers
│   ├── partitions.csv       # dual-OTA layout (no factory)
│   ├── sdkconfig.defaults   # enables rollback, FATFS, mdns, camera, BLE off
│   └── CMakeLists.txt, idf_component.yml, platformio.ini
│
├── ble_recovery/            # Standalone BLE rescue fw
│   ├── main.c               # NimBLE init + GATT service + HMAC verify + OTA write
│   ├── ota_verify.c/.h      # (same as camera_ota/)
│   ├── ota_pubkey.h         # (gitignored) same HMAC key
│   ├── partitions.csv       # (same as camera_ota/)
│   ├── sdkconfig.defaults   # NimBLE on, WiFi off, everything else stripped
│   └── CMakeLists.txt, idf_component.yml, platformio.ini
│
└── (camera/, camera_s3cam/, speaker/ — earlier diags, kept for reference)

scripts/
├── gen_ota_key.py           # One-time keygen
├── sign_ota.py              # firmware.bin → firmware.signed.bin
└── ble_ota.py               # BLE push client

docs/
└── OTA_STACK.md             # (this file)
```
