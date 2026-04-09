#!/usr/bin/env bash
# Helios board bootstrap — works on Raspberry Pi OS Lite (Bookworm/Trixie)
# and Armbian for OrangePi Zero 2W. Idempotent: safe to re-run.
#
# What it does:
#   1. Installs system packages (libopus, libdbus-dev, python3-venv, rust)
#   2. Configures bluez ExchangeMTU = 517 (ATT MTU negotiation enabled)
#   3. Clones or updates the Helios repo
#   4. Creates a Python venv and installs deps
#   5. Builds the helios_ble Rust extension via maturin
#   6. Installs and enables the helios-ble + rear_safety systemd services
#
# Run: curl -fsSL <url>/setup.sh | bash
# or:  cd /home/pi/project-helios && bash scripts/setup.sh

set -euo pipefail

# --- Config ---
HELIOS_REPO="https://github.com/Kaden-Schutt/project-helios.git"
HELIOS_BRANCH="main"
HELIOS_DIR="/home/pi/project-helios"
# Keep ownership + services running under the pi user even on Armbian where
# the default user is 'orangepi' — remap if needed.
TARGET_USER="${HELIOS_USER:-pi}"

# --- Pre-flight ---
if [[ $EUID -ne 0 ]]; then
    echo "This script needs root (sudo). Re-running with sudo..."
    exec sudo -E bash "$0" "$@"
fi

# Use whatever user actually exists. Armbian OrangePi images ship with
# 'orangepi' by default; Pi OS ships with 'pi'. Fall back gracefully.
if ! id "$TARGET_USER" &>/dev/null; then
    if id pi &>/dev/null; then
        TARGET_USER=pi
    elif id orangepi &>/dev/null; then
        TARGET_USER=orangepi
        HELIOS_DIR="/home/orangepi/project-helios"
    else
        echo "No pi or orangepi user found. Set HELIOS_USER env var." >&2
        exit 1
    fi
fi
USER_HOME=$(getent passwd "$TARGET_USER" | cut -d: -f6)
HELIOS_DIR="$USER_HOME/project-helios"

echo "== Helios setup =="
echo "User:       $TARGET_USER"
echo "Home:       $USER_HOME"
echo "Repo dir:   $HELIOS_DIR"
echo

# --- 1. System packages ---
echo "[1/6] Installing apt packages..."
export DEBIAN_FRONTEND=noninteractive
apt-get update -y
apt-get install -y \
    git curl build-essential pkg-config \
    libopus-dev libdbus-1-dev libssl-dev \
    python3 python3-venv python3-dev python3-pip \
    bluez bluetooth \
    rustc cargo

# --- 2. bluez MTU configuration ---
echo "[2/6] Configuring bluez ATT MTU..."
BLUEZ_CONF="/etc/bluetooth/main.conf"
if [[ -f "$BLUEZ_CONF" ]]; then
    if grep -q "^#ExchangeMTU" "$BLUEZ_CONF"; then
        sed -i 's/^#ExchangeMTU.*/ExchangeMTU = 517/' "$BLUEZ_CONF"
    elif ! grep -q "^ExchangeMTU" "$BLUEZ_CONF"; then
        # Insert under [GATT] section if present, else append
        if grep -q "^\[GATT\]" "$BLUEZ_CONF"; then
            sed -i '/^\[GATT\]/a ExchangeMTU = 517' "$BLUEZ_CONF"
        else
            printf "\n[GATT]\nExchangeMTU = 517\n" >> "$BLUEZ_CONF"
        fi
    fi
    systemctl restart bluetooth
fi

# --- 3. Clone or update repo ---
echo "[3/6] Cloning Helios repo..."
if [[ ! -d "$HELIOS_DIR/.git" ]]; then
    sudo -u "$TARGET_USER" git clone --branch "$HELIOS_BRANCH" "$HELIOS_REPO" "$HELIOS_DIR"
else
    sudo -u "$TARGET_USER" git -C "$HELIOS_DIR" fetch origin
    sudo -u "$TARGET_USER" git -C "$HELIOS_DIR" reset --hard "origin/$HELIOS_BRANCH" || true
fi

# --- 4. Python venv + deps ---
echo "[4/6] Setting up Python venv..."
sudo -u "$TARGET_USER" bash <<EOF
set -e
cd "$HELIOS_DIR"
if [[ ! -d .venv ]]; then
    python3 -m venv .venv
fi
.venv/bin/pip install --upgrade pip wheel
.venv/bin/pip install \
    bleak opuslib httpx websockets python-dotenv \
    anthropic numpy fastapi uvicorn maturin
EOF

# --- 5. Build helios_ble Rust extension ---
echo "[5/6] Building helios_ble Rust extension (this takes a few minutes)..."
sudo -u "$TARGET_USER" bash <<EOF
set -e
cd "$HELIOS_DIR/rust/helios_ble"
source "$HELIOS_DIR/.venv/bin/activate"
maturin develop --release
EOF

# --- 6. systemd services ---
echo "[6/6] Installing systemd services..."

# WiFi import helper — reads /boot/firmware/wifi.conf at boot.
# Lets you swap WiFi credentials by editing a FAT32 file from any OS.
if [[ -f "$HELIOS_DIR/scripts/helios-wifi-import.sh" ]]; then
    install -m 755 "$HELIOS_DIR/scripts/helios-wifi-import.sh" /usr/local/sbin/
    install -m 644 "$HELIOS_DIR/scripts/helios-wifi-import.service" /etc/systemd/system/
    systemctl enable helios-wifi-import.service
fi

cat > /etc/systemd/system/helios-ble.service <<EOF
[Unit]
Description=Helios BLE Voice + Vision Server
After=bluetooth.target network-online.target
Wants=bluetooth.target

[Service]
Type=simple
User=$TARGET_USER
WorkingDirectory=$HELIOS_DIR
ExecStart=$HELIOS_DIR/.venv/bin/python server_ble.py
Restart=always
RestartSec=3
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
EOF

# Rear safety only if the file exists
if [[ -f "$HELIOS_DIR/rear_safety.py" ]]; then
    cp "$HELIOS_DIR/rear_safety.service" /etc/systemd/system/ 2>/dev/null || \
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
systemctl enable helios-ble.service

echo
echo "== Setup complete =="
echo
echo "Next steps:"
echo "  1. Place your .env file at $HELIOS_DIR/.env (CARTESIA_API_KEY, ANTHROPIC_API_KEY)"
echo "  2. Start services: systemctl start helios-ble rear_safety"
echo "  3. Watch logs:     journalctl -u helios-ble -f"
echo
