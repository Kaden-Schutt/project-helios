#!/usr/bin/env bash
# Helios board bootstrap — Raspberry Pi OS Lite (Bookworm/Trixie). Idempotent.
#
# What it does:
#   1. Installs system packages (libopus, bluez, python3-venv)
#   2. Clones or updates the Helios repo
#   3. Creates a Python venv and installs deps
#   4. Installs + enables helios-server (WiFi HTTP) and rear_safety systemd units
#
# The Pi owns the BT link to the MT speaker via system bluez; pendant talks to
# the Pi over WiFi HTTP. BLE is used only as an OTA rescue path to the pendant
# (scripts/ble_ota.py, which is why bleak is in the venv).
#
# Run: curl -fsSL <url>/setup.sh | bash
# or:  cd /home/pi/project-helios && bash scripts/setup.sh

set -euo pipefail

HELIOS_REPO="https://github.com/Kaden-Schutt/project-helios.git"
HELIOS_BRANCH="main"
TARGET_USER="${HELIOS_USER:-pi}"

if [[ $EUID -ne 0 ]]; then
    echo "This script needs root (sudo). Re-running with sudo..."
    exec sudo -E bash "$0" "$@"
fi

if ! id "$TARGET_USER" &>/dev/null; then
    if id pi &>/dev/null; then
        TARGET_USER=pi
    else
        echo "No pi user found. Set HELIOS_USER env var." >&2
        exit 1
    fi
fi
USER_HOME=$(getent passwd "$TARGET_USER" | cut -d: -f6)
HELIOS_DIR="$USER_HOME/project-helios"

echo "== Helios setup =="
echo "User:       $TARGET_USER"
echo "Repo dir:   $HELIOS_DIR"
echo

# --- 1. System packages ---
echo "[1/4] Installing apt packages..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y \
    git curl build-essential pkg-config \
    libopus-dev libssl-dev \
    python3 python3-venv python3-dev python3-pip \
    bluez bluetooth bluez-alsa-utils

# --- 2. Clone or update repo ---
echo "[2/4] Cloning Helios repo..."
if [[ ! -d "$HELIOS_DIR/.git" ]]; then
    sudo -u "$TARGET_USER" git clone --branch "$HELIOS_BRANCH" "$HELIOS_REPO" "$HELIOS_DIR"
else
    sudo -u "$TARGET_USER" git -C "$HELIOS_DIR" fetch origin
    sudo -u "$TARGET_USER" git -C "$HELIOS_DIR" reset --hard "origin/$HELIOS_BRANCH" || true
fi

# --- 3. Python venv + deps ---
echo "[3/4] Setting up Python venv..."
sudo -u "$TARGET_USER" bash <<EOF
set -e
cd "$HELIOS_DIR"
if [[ ! -d .venv ]]; then
    python3 -m venv .venv
fi
.venv/bin/pip install --upgrade pip wheel
.venv/bin/pip install \
    httpx websockets python-dotenv \
    anthropic numpy fastapi uvicorn \
    bleak opuslib
EOF

# --- 4. systemd services + reachability ---
echo "[4/4] Installing systemd units + reachability layer..."

# WiFi import helper — reads /boot/firmware/wifi.conf at boot so credentials
# can be swapped from any OS by editing a FAT32 file.
if [[ -f "$HELIOS_DIR/scripts/helios-wifi-import.sh" ]]; then
    install -m 755 "$HELIOS_DIR/scripts/helios-wifi-import.sh" /usr/local/sbin/
    install -m 644 "$HELIOS_DIR/scripts/helios-wifi-import.service" /etc/systemd/system/
    systemctl enable helios-wifi-import.service
fi

# avahi (mDNS), tailscale (cross-network), ssh.
if [[ -f "$HELIOS_DIR/scripts/helios-reachable.sh" ]]; then
    bash "$HELIOS_DIR/scripts/helios-reachable.sh"
fi

cat > /etc/systemd/system/helios-server.service <<EOF
[Unit]
Description=Helios Voice + Vision Server (WiFi HTTP)
After=bluetooth.target network-online.target
Wants=bluetooth.target

[Service]
Type=simple
User=$TARGET_USER
WorkingDirectory=$HELIOS_DIR
ExecStart=$HELIOS_DIR/.venv/bin/python server.py
Restart=always
RestartSec=3
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
EOF

if [[ -f "$HELIOS_DIR/rear_safety.py" ]]; then
    cat > /etc/systemd/system/rear_safety.service <<EOF
[Unit]
Description=Helios Rear Safety Loop
After=multi-user.target

[Service]
Type=simple
User=root
WorkingDirectory=$HELIOS_DIR
ExecStart=/usr/bin/python3 $HELIOS_DIR/rear_safety.py
Restart=always
RestartSec=3

[Install]
WantedBy=multi-user.target
EOF
    systemctl enable rear_safety.service
fi

systemctl daemon-reload
systemctl enable helios-server.service

echo
echo "== Setup complete =="
echo
echo "Next steps:"
echo "  1. Place .env at $HELIOS_DIR/.env (CARTESIA_API_KEY, ANTHROPIC_API_KEY)"
echo "  2. Pair MT speaker: bluetoothctl → scan, pair, trust, connect"
echo "  3. Start services:  systemctl start helios-server rear_safety"
echo "  4. Watch logs:      journalctl -u helios-server -f"
echo
