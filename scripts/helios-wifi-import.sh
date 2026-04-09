#!/usr/bin/env bash
# Runs on the Pi at boot. Reads /boot/firmware/wifi.conf (FAT32, editable
# from any OS) and imports each listed WiFi network into NetworkManager.
# Idempotent — safe to run every boot. Skips networks already configured.
#
# wifi.conf format (INI-style):
#
#   [MyHomeNetwork]
#   psk=mypassword
#
#   [iPhone]
#   psk=anotherpassword
#   priority=100      # optional, higher = preferred
#
#   [OpenNetwork]
#   psk=              # empty psk = open network
#
# Lines starting with # are comments. Blank lines are fine.

set -euo pipefail

CONF_FILE="/boot/firmware/wifi.conf"
# Fallback for older Pi OS where /boot isn't split from firmware
[[ ! -f "$CONF_FILE" ]] && CONF_FILE="/boot/wifi.conf"

if [[ ! -f "$CONF_FILE" ]]; then
    exit 0  # Nothing to do
fi

log() { logger -t helios-wifi-import -- "$*"; echo "[wifi-import] $*"; }

if ! command -v nmcli >/dev/null 2>&1; then
    log "nmcli not found — NetworkManager required. Skipping."
    exit 0
fi

current_ssid=""
current_psk=""
current_priority=""

import_network() {
    local ssid="$1" psk="$2" priority="$3"
    [[ -z "$ssid" ]] && return

    # Check if already configured — skip re-import to avoid churning
    if nmcli -t -f NAME connection show | grep -Fxq "$ssid"; then
        log "already configured: $ssid"
        return
    fi

    if [[ -z "$psk" ]]; then
        log "importing open network: $ssid"
        nmcli connection add type wifi \
            con-name "$ssid" ifname wlan0 ssid "$ssid" \
            wifi-sec.key-mgmt none 2>&1 | logger -t helios-wifi-import || true
    else
        log "importing secured network: $ssid"
        nmcli connection add type wifi \
            con-name "$ssid" ifname wlan0 ssid "$ssid" \
            wifi-sec.key-mgmt wpa-psk wifi-sec.psk "$psk" 2>&1 | logger -t helios-wifi-import || true
    fi

    if [[ -n "$priority" ]]; then
        nmcli connection modify "$ssid" connection.autoconnect-priority "$priority" || true
    fi
    nmcli connection modify "$ssid" connection.autoconnect yes || true
}

while IFS= read -r line || [[ -n "$line" ]]; do
    # Strip Windows line endings, comments, leading/trailing whitespace
    line="${line%$'\r'}"
    line="${line#"${line%%[![:space:]]*}"}"
    line="${line%"${line##*[![:space:]]}"}"
    [[ -z "$line" || "$line" =~ ^# ]] && continue

    if [[ "$line" =~ ^\[(.+)\]$ ]]; then
        # New section — flush previous
        if [[ -n "$current_ssid" ]]; then
            import_network "$current_ssid" "$current_psk" "$current_priority"
        fi
        current_ssid="${BASH_REMATCH[1]}"
        current_psk=""
        current_priority=""
    elif [[ "$line" =~ ^psk[[:space:]]*=[[:space:]]*(.*)$ ]]; then
        current_psk="${BASH_REMATCH[1]}"
    elif [[ "$line" =~ ^priority[[:space:]]*=[[:space:]]*(.*)$ ]]; then
        current_priority="${BASH_REMATCH[1]}"
    fi
done < "$CONF_FILE"

# Flush last section
if [[ -n "$current_ssid" ]]; then
    import_network "$current_ssid" "$current_psk" "$current_priority"
fi

log "done"
