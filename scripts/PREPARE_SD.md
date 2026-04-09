# Preparing a Helios SD Card

This guide covers two boards and two workflows:

| Board | OS | Use case |
|-------|-----|----------|
| Raspberry Pi 4B | **Raspberry Pi OS Lite 64-bit** (Bookworm) | Class demo |
| OrangePi Zero 2W | **Armbian Bookworm** | Home dev / testing |

**One SD card = one board.** The Pi 4B and OrangePi use different bootloaders
and kernels, so you cannot boot the same card on both. Prepare separate cards
if you want to run Helios on both.

---

## Option A — Raspberry Pi 4B (for class demo)

### 1. Flash the base image

Download **Raspberry Pi Imager** (https://www.raspberrypi.com/software/),
then:

1. Open Imager → **Choose Device**: Raspberry Pi 4
2. **Choose OS**: Raspberry Pi OS (other) → **Raspberry Pi OS Lite (64-bit)**
3. **Choose Storage**: your SD card
4. Click the gear icon (⚙) and set:
   - Hostname: `helios`
   - Enable SSH (use password auth or public key)
   - Username: `pi`
   - Password: (your choice, remember it)
   - WiFi SSID / password: **pre-configure your home network** and/or your phone hotspot
   - Locale: US, your timezone
5. **Write**

First boot takes ~1 min. After boot, the Pi will be on your network at
`helios.local` or find its IP via `arp -a`.

### 2. Run the Helios setup script

```bash
ssh pi@helios.local
curl -fsSL https://raw.githubusercontent.com/Kaden-Schutt/project-helios/main/scripts/setup.sh | sudo bash
```

This takes 10–15 minutes (most of it is compiling Rust/bluer). The script
is idempotent — if it fails partway, re-run it.

### 3. Drop in your `.env`

```bash
scp .env pi@helios.local:/home/pi/project-helios/.env
```

### 4. Start services

```bash
ssh pi@helios.local
sudo systemctl start helios-ble rear_safety
sudo systemctl status helios-ble
```

Done. On boot, the services come up automatically.

---

## Option B — OrangePi Zero 2W (for home dev)

### 1. Flash Armbian

Download Armbian for OrangePi Zero 2W from https://www.armbian.com/orangepi-zero-2w/

Choose the **"Bookworm CLI"** image (no desktop, much smaller).

Flash with Raspberry Pi Imager ("Use custom" option) or `balenaEtcher`.

### 2. First boot

Plug in the SD, connect ethernet (or wire a USB keyboard + HDMI once for
initial WiFi setup). Default login on first boot is `root` / `1234` and it
will force you to create a user.

Set the user to `orangepi` or `pi` — setup.sh handles both.

### 3. Run setup.sh

Same command as Pi 4B:

```bash
ssh orangepi@<ip>
curl -fsSL https://raw.githubusercontent.com/Kaden-Schutt/project-helios/main/scripts/setup.sh | sudo bash
```

---

## The "Swap SD in Class" Strategy

The provided Pi 4B in class has a bloated Raspberry Pi OS install with a
full desktop and cruft from old projects. For the demo, you can bring your
own pre-configured SD card and swap it in.

### Workflow

**Before class** (at home):
1. Prepare your SD card as above, verify Helios runs end-to-end on battery
2. Make sure all three SSIDs are pre-configured in `wpa_supplicant.conf`:
   - Your home WiFi (for final testing)
   - Your phone hotspot (backup + class)
   - `asu guest` is a **captive portal** — won't work headless, skip it
3. Test boot-to-service-running time — should be under 60s
4. Label your SD card clearly

**In class**:
1. Power off the provided Pi 4B cleanly (`sudo shutdown -h now` if accessible)
2. Unplug power
3. Remove the original SD, place it in a labeled container (you MUST return it)
4. Insert your SD
5. Power on — expect 30-45s boot time
6. Tether the Pi to your phone hotspot if needed (the `helios-ble` service
   doesn't need internet to talk to the ESP32, but the Cartesia/Anthropic
   APIs do)
7. `ssh pi@helios.local` or whatever IP your hotspot assigned

**After class**:
1. Power off the Pi
2. Remove your SD
3. Reinsert the original SD
4. Power on to verify it still boots
5. Hand the Pi back as you found it

### Why this is better than using the provided setup

- **Predictable**: you know exactly what's on the card
- **Fast boot**: Pi OS Lite with no desktop = ~30s to services running
- **Clean**: no leftover cron jobs, background services, or artifacts
- **Reproducible**: if something breaks, reflash the card
- **Preservable**: the original SD is untouched, easy to return

### Risks

- **Don't lose or damage the original SD** — it's the course's hardware
- **Don't assume the Pi 4B's bootloader will accept any image** — some
  Pi 4Bs have specific EEPROM versions that require a minimum firmware.
  Test YOUR sd card on a Pi 4B you have access to before relying on it in class
- **Captive portals (ASU guest)** won't auto-login — rely on phone hotspot
- **ESD safety** — ground yourself before touching the Pi or SD

---

## Troubleshooting

**`setup.sh` fails at Rust/cargo stage**: out of RAM. The OrangePi Zero 2W
has 1-4GB depending on variant; make sure swap is enabled or use the 4GB variant.

**`helios-ble` fails to start with "Helios not found"**: ESP32 isn't
advertising. Check it's powered and not bonded to another central (your Mac).

**`MTU 23` errors**: the `helios_ble` Rust extension didn't build — check
`maturin develop --release` completed. See `rust/helios_ble/README.md`.

**No WiFi on OrangePi Zero 2W**: Armbian sometimes needs
`nmtui` on first boot to scan + connect. SSH via ethernet, configure
WiFi with `nmtui`, reboot.
