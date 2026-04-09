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
full desktop and cruft from old projects. For the demo, bring your own
pre-configured SD card and swap it in.

### How WiFi credentials work on your SD

Your preconfigured SD has a `helios-wifi-import` systemd service that
runs at boot. It reads `/boot/firmware/wifi.conf` (a plain text file on
the FAT32 boot partition, **mountable from any OS**) and imports each
listed network into NetworkManager. Format:

```ini
[SSID Name]
psk=password
priority=100

[Another Network]
psk=anotherpass
```

This means you never need to read the Linux rootfs to update WiFi —
just mount the boot partition on Mac, edit `wifi.conf`, eject, swap.

### Workflow — no GUI required

**Before class** (at home):
1. Prepare your SD with `setup.sh`. Verify Helios runs end-to-end on battery.
2. Edit `/Volumes/bootfs/wifi.conf` to include:
   - Your phone hotspot (highest priority — fallback that always works)
   - Your home WiFi
3. Test boot-to-service-running time. Should be under 60s.
4. Label your SD card clearly.

**In class — Option 1: just use your hotspot (recommended)**
1. Turn on your phone hotspot.
2. Power off the provided Pi 4B (`sudo shutdown -h now` or pull power if you must).
3. Pop out the original SD, put it somewhere safe — you must return it.
4. Insert your SD, power on. NetworkManager will pick your hotspot.
5. `ssh pi@<hotspot-assigned-ip>` (find via your phone's hotspot device list).

**In class — Option 2: inherit the class WiFi**

If you want the Pi to use whatever network the class Pi was on, you have
to read the credentials off its rootfs (which is ext4, not Mac-mountable
out of the box). Three ways:

- **Easiest: SSH into the running class Pi first.** Before powering it off:
  ```bash
  ssh pi@<class-pi-ip>
  sudo grep -E '^(ssid|psk)=' /etc/NetworkManager/system-connections/*.nmconnection
  ```
  Copy SSID + psk into your SD's `wifi.conf`, eject, swap.

- **Use your OrangePi as an ext4 reader.** Plug the class SD into a USB
  reader, plug the reader into the OrangePi:
  ```bash
  ssh orangepi@<orangepi-ip>
  lsblk                                       # find sda2 (the rootfs)
  sudo mount /dev/sda2 /mnt
  sudo bash ~/project-helios/scripts/extract-wifi.sh /mnt
  sudo umount /mnt
  ```
  `extract-wifi.sh` prints credentials in helios `wifi.conf` format —
  paste straight into your SD's boot partition.

- **Install ext4fuse on your Mac** (fragile, optional):
  ```bash
  brew install --cask macfuse
  brew install gromgit/fuse/ext4fuse-mac
  ```
  Then: `ext4fuse /dev/disk5s2 /Volumes/picross && bash extract-wifi.sh /Volumes/picross`.
  macFUSE requires kernel-extension permissions in macOS Settings.

**After class**:
1. Power off the Pi.
2. Remove your SD.
3. Reinsert the original SD.
4. Power on to verify it still boots normally.
5. Hand the Pi back as you found it.

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

## Reaching the Pi from your MacBook

`setup.sh` installs a three-layer reachability stack so you can SSH in
no matter what network the Pi ends up on:

| Layer | How it works | When it helps |
|-------|-------------|---------------|
| **mDNS / Avahi** | Pi advertises as `helios.local` on the LAN | Same WiFi network as your Mac |
| **Tailscale** | Encrypted overlay network across NAT/firewalls | Class WiFi where you can't see the Pi directly, or different networks entirely |
| **USB ethernet** | (Manual) `g_ether` gadget mode for last-resort | Pi has no working network at all |

### Setting up Tailscale (one-time)

1. Sign up at https://login.tailscale.com (free for personal)
2. Generate an auth key: https://login.tailscale.com/admin/settings/keys
   - Tag with `tag:helios`, set "Reusable" + "Ephemeral=no"
3. Pass it to setup.sh:
   ```bash
   TAILSCALE_AUTHKEY=tskey-auth-... sudo bash scripts/setup.sh
   ```
4. After boot, the Pi shows up in your Tailscale admin panel as `helios`
5. Install Tailscale on your Mac (one-time, https://tailscale.com/download/mac)
6. SSH from anywhere: `ssh pi@helios` or `ssh pi@<tailscale-ip>`

If you don't pass `TAILSCALE_AUTHKEY`, Tailscale gets installed but
not authenticated. Run `sudo tailscale up` later to finish setup.

### Quick connection tests from your Mac

```bash
# mDNS (same network)
ssh pi@helios.local

# Tailscale (any network, after setup)
ssh pi@helios
tailscale ping helios

# Find IP via your phone hotspot's connected-devices list
ssh pi@<ip-from-phone>
```

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
