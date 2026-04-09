#!/usr/bin/env bash
# Extract WiFi credentials from a mounted Raspberry Pi SD rootfs.
# Run this against the rootfs mountpoint of the SD you want to read.
#
# Usage:
#   extract-wifi.sh /mnt/rootfs
#
# Output: prints SSID + PSK pairs in helios wifi.conf format, ready
# to paste into your preconfigured SD's /boot/firmware/wifi.conf.
#
# Where to run this from:
#   Linux box (any) with the SD plugged in via USB reader. macOS does
#   NOT mount ext4 by default — use your OrangePi if you don't have
#   ext4fuse installed.
#
# Quick OrangePi workflow:
#   1. Plug class Pi SD into a USB-A SD reader on the OrangePi
#   2. ssh orangepi@<ip>
#   3. lsblk        # find the SD device, e.g. sda2
#   4. sudo mkdir -p /mnt/picross && sudo mount /dev/sda2 /mnt/picross
#   5. sudo bash extract-wifi.sh /mnt/picross
#   6. sudo umount /mnt/picross

set -euo pipefail

ROOTFS="${1:-}"
if [[ -z "$ROOTFS" || ! -d "$ROOTFS" ]]; then
    echo "usage: $0 <path-to-mounted-rootfs>" >&2
    echo "  e.g.: sudo $0 /mnt/picross" >&2
    exit 1
fi

found=0

# --- Modern Pi OS (Bookworm+): NetworkManager ---
NM_DIR="$ROOTFS/etc/NetworkManager/system-connections"
if [[ -d "$NM_DIR" ]]; then
    for conn in "$NM_DIR"/*.nmconnection; do
        [[ -e "$conn" ]] || continue
        ssid=$(awk -F= '/^ssid=/ {print $2; exit}' "$conn" | tr -d '\r')
        psk=$(awk -F= '/^psk=/ {print $2; exit}' "$conn" | tr -d '\r')
        key_mgmt=$(awk -F= '/^key-mgmt=/ {print $2; exit}' "$conn" | tr -d '\r')
        [[ -z "$ssid" ]] && continue
        echo "[$ssid]"
        if [[ "$key_mgmt" == "none" || -z "$psk" ]]; then
            echo "psk="
        else
            echo "psk=$psk"
        fi
        echo
        found=1
    done
fi

# --- Legacy: wpa_supplicant.conf ---
WPA_FILE="$ROOTFS/etc/wpa_supplicant/wpa_supplicant.conf"
if [[ -f "$WPA_FILE" ]]; then
    awk '
        /network=\{/ { in_net=1; ssid=""; psk=""; next }
        in_net && /ssid=/ { sub(/^[[:space:]]*ssid=/,""); gsub(/"/,""); ssid=$0 }
        in_net && /psk=/ { sub(/^[[:space:]]*psk=/,""); gsub(/"/,""); psk=$0 }
        in_net && /\}/ {
            if (ssid != "") {
                printf "[%s]\npsk=%s\n\n", ssid, psk
            }
            in_net=0
        }
    ' "$WPA_FILE"
    found=1
fi

# --- Last resort: scan for any nmconnection-like file with psk ---
if [[ "$found" -eq 0 ]]; then
    grep -rl "^psk=" "$ROOTFS/etc/" 2>/dev/null | while read -r f; do
        echo "# from $f" >&2
        ssid=$(awk -F= '/^ssid=/ {print $2; exit}' "$f" 2>/dev/null | tr -d '\r')
        psk=$(awk -F= '/^psk=/ {print $2; exit}' "$f" 2>/dev/null | tr -d '\r')
        [[ -n "$ssid" && -n "$psk" ]] && printf "[%s]\npsk=%s\n\n" "$ssid" "$psk"
    done
fi

if [[ "$found" -eq 0 ]]; then
    echo "no wifi credentials found in $ROOTFS" >&2
    echo "checked: $NM_DIR, $WPA_FILE" >&2
    exit 2
fi
