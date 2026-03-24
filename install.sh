#!/usr/bin/env bash
set -euo pipefail
 
SERVICE="gpu-metrics-fix"
PREFIX="/usr/local"
 
if [[ $EUID -ne 0 ]]; then
    echo "Run as root."
    exit 1
fi
 
# Uninstall
if [[ "${1:-}" == "--uninstall" ]]; then
    systemctl stop "$SERVICE" 2>/dev/null || true
    systemctl disable "$SERVICE" 2>/dev/null || true
    rm -f "$PREFIX/bin/gpu-metrics-fix"
    rm -f "/etc/systemd/system/$SERVICE.service"
    rm -rf /var/lib/gpu-metrics-fix
    systemctl daemon-reload
    # clean up bind mount if still there
    umount /sys/class/drm/card*/device/gpu_metrics 2>/dev/null || true
    echo "Uninstalled."
    exit 0
fi
 
# Check deps
for cmd in gcc make; do
    if ! command -v $cmd &>/dev/null; then
        echo "$cmd not found, trying to install..."
        if command -v dnf &>/dev/null; then
            dnf install -y gcc make
        elif command -v rpm-ostree &>/dev/null; then
            rpm-ostree install gcc make --idempotent --allow-inactive
            echo "You might need to reboot for rpm-ostree changes, then re-run this."
        elif command -v apt-get &>/dev/null; then
            apt-get update -qq && apt-get install -y build-essential
        elif command -v pacman &>/dev/null; then
            pacman -Sy --noconfirm --needed base-devel
        elif command -v zypper &>/dev/null; then
            zypper install -y gcc make
        else
            echo "Can't auto-install $cmd. Install gcc and make manually."
            exit 1
        fi
        break
    fi
done
 
# Check that gpu_metrics actually exists
METRICS=""
for f in /sys/class/drm/card*/device/gpu_metrics; do
    [ -f "$f" ] && METRICS="$f" && break
done
if [[ -z "$METRICS" ]]; then
    echo "No gpu_metrics found. Is amdgpu loaded?"
    exit 1
fi
 
CARD=$(echo "$METRICS" | grep -oP 'card\d+')
echo "Found gpu_metrics at $METRICS"
 
# Build
make
echo "Built gpu-metrics-fix"
 
# Install
make install
 
# Adjust card if not card1
if [[ "$CARD" != "card1" ]]; then
    sed -i "s|ExecStart=.*|ExecStart=$PREFIX/bin/gpu-metrics-fix --card $CARD|" \
        "/etc/systemd/system/$SERVICE.service"
    sed -i "s|card1|$CARD|g" "/etc/systemd/system/$SERVICE.service"
fi
 
# Start
systemctl daemon-reload
systemctl enable --now "$SERVICE.service"
 
echo "Running. Check with: systemctl status $SERVICE"