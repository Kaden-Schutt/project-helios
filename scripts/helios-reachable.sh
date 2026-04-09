#!/usr/bin/env bash
# Make the Pi reachable from your MacBook regardless of network conditions.
#
# Layered fallback strategy:
#   1. mDNS / Avahi      — `helios.local` works on the same LAN
#   2. Tailscale         — works across any network, NAT, captive portals,
#                          even cellular. Auth required once via tskey.
#   3. USB ethernet      — `g_ether` gadget mode over USB-C, last resort
#                          when everything else is broken
#
# Run as root from setup.sh. Idempotent.

set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run as root" >&2
    exit 1
fi

log() { echo "[helios-reachable] $*"; }

# --- 1. mDNS via Avahi ---
log "Installing avahi-daemon for helios.local discovery..."
DEBIAN_FRONTEND=noninteractive apt-get install -y avahi-daemon avahi-utils
systemctl enable --now avahi-daemon

# Set hostname so it advertises consistently
if [[ "$(hostname)" != "helios" ]]; then
    log "Setting hostname to 'helios'"
    hostnamectl set-hostname helios
    # Update /etc/hosts so sudo doesn't complain
    if ! grep -q "127.0.1.1.*helios" /etc/hosts; then
        sed -i '/^127.0.1.1/d' /etc/hosts
        echo "127.0.1.1 helios" >> /etc/hosts
    fi
fi

# --- 2. Tailscale (optional, but recommended for class) ---
# If TAILSCALE_AUTHKEY is set as an env var, install + auth headlessly.
# Otherwise install Tailscale but leave it un-authed; you can `tailscale up`
# manually later.
if ! command -v tailscale >/dev/null 2>&1; then
    log "Installing Tailscale..."
    curl -fsSL https://tailscale.com/install.sh | sh
fi
systemctl enable --now tailscaled

if [[ -n "${TAILSCALE_AUTHKEY:-}" ]]; then
    log "Authenticating Tailscale with provided authkey..."
    tailscale up \
        --authkey="$TAILSCALE_AUTHKEY" \
        --hostname=helios \
        --ssh \
        --accept-routes \
        --reset || log "tailscale up failed (re-run manually)"
else
    log "TAILSCALE_AUTHKEY not set — install only, run 'tailscale up' to auth"
fi

# --- 3. SSH always-on, password fallback ---
log "Ensuring SSH is enabled..."
systemctl enable --now ssh
# Allow password auth as a fallback (your authorized_keys should be primary)
if [[ -f /etc/ssh/sshd_config ]]; then
    sed -i 's/^#*PasswordAuthentication.*/PasswordAuthentication yes/' /etc/ssh/sshd_config
    systemctl reload ssh || true
fi

# --- 4. Print contact info ---
log "Reachability summary:"
log "  hostname:    $(hostname)"
log "  mDNS:        ssh pi@helios.local"
ip -4 addr show wlan0 2>/dev/null | awk '/inet / {print "  WiFi IP:     " $2}'
ip -4 addr show eth0 2>/dev/null | awk '/inet / {print "  Ethernet:    " $2}'
if command -v tailscale >/dev/null 2>&1; then
    ts_ip=$(tailscale ip -4 2>/dev/null | head -1 || true)
    [[ -n "$ts_ip" ]] && log "  Tailscale:   ssh pi@$ts_ip"
fi
