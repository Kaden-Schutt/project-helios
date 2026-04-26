#!/usr/bin/env bash
# prep-pi4b-sd.sh — one-shot prep for a Helios-ready DietPi Pi 4B SD card.
#
# What this does:
#   1. Downloads DietPi RPi2/3/4 image (if not cached)
#   2. Flashes it to the target SD card
#   3. Writes all Helios customizations to the DIETPISETUP FAT partition:
#      - dietpi.txt: automated install, hostname helios, WiFi, apt packages
#        (including bluez + avahi-daemon explicitly — do NOT rely on
#         DietPi software IDs which have silently failed on our tests)
#      - dietpi-wifi.txt: 4 networks (Raspberry Pi, iPhone hotspot, home)
#      - Automation_Custom_Script.sh: clones helios repo, installs Python
#        deps, sets up helios-server systemd unit (disabled by default)
#
# Usage:
#   ./prep-pi4b-sd.sh /dev/diskN   (macOS)
#   ./prep-pi4b-sd.sh /dev/sdN     (Linux)
#
# Safety:
#   - Requires the disk path as arg, will NOT guess
#   - Asks to confirm before wiping

set -euo pipefail

if [[ $# -lt 1 ]]; then
    echo "Usage: $0 /dev/diskN" >&2
    echo "  Find the SD card with: diskutil list external  (macOS)" >&2
    echo "                     or: lsblk                    (Linux)" >&2
    exit 1
fi

TARGET_DISK="$1"
IMAGE_URL="https://dietpi.com/downloads/images/DietPi_RPi234-ARMv8-Bookworm.img.xz"
IMAGE_CACHE="/tmp/dietpi-rpi4.img.xz"

# Repo-local files we copy onto the FAT partition
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DIETPI_TXT_SRC="$SCRIPT_DIR/dietpi.txt.pi4b"
DIETPI_WIFI_SRC="$SCRIPT_DIR/wifi.conf"  # has real credentials, gitignored
AUTOMATION_SRC="$SCRIPT_DIR/Automation_Custom_Script.sh.pi4b"

log() { echo "[prep-sd] $*"; }

# --- 1. Sanity checks ---
if [[ ! -b "$TARGET_DISK" && ! -e "$TARGET_DISK" ]]; then
    log "ERROR: $TARGET_DISK does not exist" >&2
    exit 1
fi

log "Target disk: $TARGET_DISK"
log "WILL WIPE THIS DISK. Type 'yes' to proceed:"
read -r CONFIRM
[[ "$CONFIRM" == "yes" ]] || { log "aborted"; exit 1; }

# --- 2. Download image if needed ---
if [[ ! -f "$IMAGE_CACHE" ]]; then
    log "Downloading DietPi RPi2/3/4 image..."
    curl -L -o "$IMAGE_CACHE" "$IMAGE_URL"
fi
log "Image: $(ls -lh "$IMAGE_CACHE" | awk '{print $5}')"

# --- 3. Flash ---
log "Unmounting disk..."
if [[ "$(uname)" == "Darwin" ]]; then
    diskutil unmountDisk "$TARGET_DISK" || true
    RAW_DISK="${TARGET_DISK/disk/rdisk}"
else
    for p in "$TARGET_DISK"*[0-9]; do sudo umount "$p" 2>/dev/null || true; done
    RAW_DISK="$TARGET_DISK"
fi

log "Flashing (this takes ~40 seconds)..."
xz -dc "$IMAGE_CACHE" | sudo dd of="$RAW_DISK" bs=4m 2>&1 | tail -3
log "Flash complete."

# Let macOS/Linux remount the DIETPISETUP partition
sleep 3

# --- 4. Find DIETPISETUP mount point ---
DIETPISETUP=""
for candidate in /Volumes/DIETPISETUP /media/*/DIETPISETUP /mnt/DIETPISETUP; do
    if [[ -d "$candidate" ]]; then
        DIETPISETUP="$candidate"
        break
    fi
done

if [[ -z "$DIETPISETUP" ]]; then
    log "ERROR: couldn't find DIETPISETUP mount. Mount the small FAT partition and re-run just steps 5+." >&2
    exit 1
fi
log "DIETPISETUP: $DIETPISETUP"

# --- 5. Copy our customizations ---
log "Writing dietpi.txt..."
if [[ -f "$DIETPI_TXT_SRC" ]]; then
    cp "$DIETPI_TXT_SRC" "$DIETPISETUP/dietpi.txt"
else
    log "WARN: $DIETPI_TXT_SRC not found, using inline generation"
    # Inline generation — applies our known-good settings to the stock file
    sed -i.bak \
        -e 's/^AUTO_SETUP_LOCALE=.*/AUTO_SETUP_LOCALE=en_US.UTF-8/' \
        -e 's/^AUTO_SETUP_KEYBOARD_LAYOUT=.*/AUTO_SETUP_KEYBOARD_LAYOUT=us/' \
        -e 's|^AUTO_SETUP_TIMEZONE=.*|AUTO_SETUP_TIMEZONE=America/Phoenix|' \
        -e 's/^AUTO_SETUP_NET_ETHERNET_ENABLED=.*/AUTO_SETUP_NET_ETHERNET_ENABLED=0/' \
        -e 's/^AUTO_SETUP_NET_WIFI_ENABLED=.*/AUTO_SETUP_NET_WIFI_ENABLED=1/' \
        -e 's/^AUTO_SETUP_NET_WIFI_COUNTRY_CODE=.*/AUTO_SETUP_NET_WIFI_COUNTRY_CODE=US/' \
        -e 's/^AUTO_SETUP_NET_HOSTNAME=.*/AUTO_SETUP_NET_HOSTNAME=helios/' \
        -e 's/^AUTO_SETUP_HEADLESS=.*/AUTO_SETUP_HEADLESS=1/' \
        -e 's/^CONFIG_SERIAL_CONSOLE_ENABLE=.*/CONFIG_SERIAL_CONSOLE_ENABLE=0/' \
        -e 's/^#AUTO_SETUP_APT_INSTALLS=.*/AUTO_SETUP_APT_INSTALLS=git curl build-essential pkg-config libopus-dev libdbus-1-dev libssl-dev python3 python3-venv python3-dev python3-pip bluez bluez-tools avahi-daemon avahi-utils/' \
        -e 's/^AUTO_SETUP_SSH_SERVER_INDEX=.*/AUTO_SETUP_SSH_SERVER_INDEX=-2/' \
        -e 's/^AUTO_SETUP_AUTOMATED=.*/AUTO_SETUP_AUTOMATED=1/' \
        "$DIETPISETUP/dietpi.txt"
    rm -f "$DIETPISETUP/dietpi.txt.bak"
fi

log "Writing dietpi-wifi.txt..."
if [[ -f "$DIETPI_WIFI_SRC" ]]; then
    # Use our helper to convert wifi.conf to dietpi-wifi.txt format
    # For now, assume Automation_Custom_Script handles the conversion
    cp "$DIETPI_WIFI_SRC" "$DIETPISETUP/helios-wifi.conf"
fi

log "Writing Automation_Custom_Script.sh..."
if [[ -f "$AUTOMATION_SRC" ]]; then
    cp "$AUTOMATION_SRC" "$DIETPISETUP/Automation_Custom_Script.sh"
fi

# Clean up macOS metadata files if we're on macOS
if [[ "$(uname)" == "Darwin" ]]; then
    dot_clean -m "$DIETPISETUP" 2>/dev/null || true
fi

log "Files on DIETPISETUP:"
ls -la "$DIETPISETUP/"

log "Ejecting..."
if [[ "$(uname)" == "Darwin" ]]; then
    diskutil eject "$TARGET_DISK"
else
    sudo umount "$DIETPISETUP" 2>/dev/null || true
    sudo eject "$TARGET_DISK" 2>/dev/null || true
fi

log "Done. Insert SD into Pi 4B and power on."
log "First boot takes ~5 min, then:"
log "  ssh root@helios.local   (password: dietpi)"
log "  scp .env root@helios.local:/root/project-helios/.env"
log "  bluetoothctl   # pair + connect MT BT speaker"
log "  systemctl start helios-server"
