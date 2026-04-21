#!/usr/bin/env bash
# Build a pre-populated DietPi Pi4B image with Helios preinstalled.
# Runs on the Linux build server (needs sudo).

set -euo pipefail

SRC_IMG_XZ=/tmp/dietpi-rpi4.img.xz
WORK=/tmp/helios-build
OUT_IMG=$WORK/helios-pi4b.img

mkdir -p $WORK
cd $WORK

log() { echo "[build] $*"; }

log 'Extracting base image...'
xz -dc $SRC_IMG_XZ > $OUT_IMG

log 'Growing image to 4GB so apt installs fit...'
truncate -s 4G $OUT_IMG

log 'Fixing partition table for new size...'
echo -e 'd\n2\nn\np\n2\n\n\nN\nw' | sudo fdisk $OUT_IMG >/dev/null 2>&1 || true

log 'Setting up loopback...'
LOOP=$(sudo losetup -f --show -P $OUT_IMG)
log "loop: $LOOP"
ls -la ${LOOP}*

log 'Resizing ext4 rootfs...'
sudo e2fsck -fy ${LOOP}p2 2>&1 | tail -3
sudo resize2fs ${LOOP}p2 2>&1 | tail -3

log 'Mounting rootfs...'
mkdir -p $WORK/root
sudo mount ${LOOP}p2 $WORK/root
sudo mount ${LOOP}p1 $WORK/root/boot 2>/dev/null || true

log 'Copying qemu-aarch64-static into chroot...'
sudo cp /usr/bin/qemu-aarch64-static $WORK/root/usr/bin/

log 'Binding /proc /sys /dev...'
sudo mount --bind /proc $WORK/root/proc
sudo mount --bind /sys $WORK/root/sys
sudo mount --bind /dev $WORK/root/dev
sudo mount --bind /dev/pts $WORK/root/dev/pts
sudo cp /etc/resolv.conf $WORK/root/etc/resolv.conf

log 'Entering chroot...'
sudo chroot $WORK/root /bin/bash -c '
set -e
echo "--- chroot running ---"
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    git curl ca-certificates pkg-config build-essential \
    libopus-dev libssl-dev \
    python3 python3-venv python3-dev python3-pip \
    bluez bluez-tools bluez-alsa-utils avahi-daemon avahi-utils

echo "--- cloning helios repo ---"
git clone --depth 1 --branch main https://github.com/Kaden-Schutt/project-helios.git /root/project-helios

echo "--- creating venv ---"
python3 -m venv /root/project-helios/.venv
/root/project-helios/.venv/bin/pip install --upgrade pip wheel
/root/project-helios/.venv/bin/pip install \
    httpx websockets python-dotenv \
    anthropic numpy fastapi uvicorn \
    bleak opuslib

echo "--- systemd helios-server.service ---"
cat > /etc/systemd/system/helios-server.service <<EOF
[Unit]
Description=Helios Voice + Vision Server (WiFi HTTP)
After=bluetooth.service network-online.target
Wants=bluetooth.service

[Service]
Type=simple
User=root
WorkingDirectory=/root/project-helios
ExecStart=/root/project-helios/.venv/bin/python server.py
Restart=on-failure
RestartSec=5
Environment=PYTHONUNBUFFERED=1

[Install]
WantedBy=multi-user.target
EOF

echo "--- cleanup apt cache ---"
apt-get clean
rm -rf /var/lib/apt/lists/*

echo "--- chroot done ---"
'

log 'Unmounting...'
sudo umount $WORK/root/dev/pts || true
sudo umount $WORK/root/dev || true
sudo umount $WORK/root/sys || true
sudo umount $WORK/root/proc || true
sudo umount $WORK/root/boot || true
sudo umount $WORK/root
sudo rm -f $WORK/root/usr/bin/qemu-aarch64-static 2>/dev/null || true
sudo losetup -d $LOOP

log 'Compressing output image...'
xz -T 0 -6 $OUT_IMG
ls -lh $OUT_IMG.xz

log 'Done. Image ready at $OUT_IMG.xz'
