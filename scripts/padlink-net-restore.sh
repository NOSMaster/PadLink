#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Undoes padlink-net-up.sh: stops padlink, removes its firewall rules and
# hands the interface back to NetworkManager, reconnecting your normal Wi-Fi.
# Safe to run more than once, and also works if the state file is missing.
#
# usage: sudo scripts/padlink-net-restore.sh [interface]
set -uo pipefail

state=/run/padlink/net-state
IFACE="${1:-wlan0}"
NM_MANAGED=yes
NM_CONNECTION=""
UFW_RULES=auto

[[ $EUID -eq 0 ]] || { echo "run as root: sudo $0" >&2; exit 1; }
if [[ -f "$state" ]]; then
    # shellcheck disable=SC1090  # file written by padlink-net-up.sh
    source "$state"
else
    echo "No $state found; restoring $IFACE with defaults."
fi

echo "Stopping padlink..."
pkill -INT -x padlinkd 2>/dev/null && sleep 1
pkill -x padlink-hostapd 2>/dev/null && sleep 1
pkill -9 -x padlink-hostapd 2>/dev/null

ip addr flush dev "$IFACE" 2>/dev/null
ip link set dev "$IFACE" down 2>/dev/null
ip link set dev "$IFACE" mtu 1500 2>/dev/null
iw dev "$IFACE" set type managed 2>/dev/null

if [[ $UFW_RULES != no ]] && command -v ufw >/dev/null; then
    ufw delete allow in on "$IFACE" to any port 67 proto udp >/dev/null 2>&1
    ufw delete allow in on "$IFACE" from 192.168.1.0/24 to any port 50010:50025 proto udp >/dev/null 2>&1
fi

if [[ $NM_MANAGED == yes ]] && command -v nmcli >/dev/null; then
    echo "Giving $IFACE back to NetworkManager..."
    nmcli device set "$IFACE" managed yes
    sleep 3
    if [[ -n "$NM_CONNECTION" ]]; then
        nmcli connection up "$NM_CONNECTION" >/dev/null 2>&1 \
            || nmcli device connect "$IFACE" >/dev/null 2>&1 \
            || echo "Could not reconnect automatically. Try: nmcli connection up \"$NM_CONNECTION\""
    else
        nmcli device connect "$IFACE" >/dev/null 2>&1 || true
    fi
fi

rm -f "$state" /run/padlink/hostapd-*.conf /run/padlink/wps-credential.bin
nmcli -t -f DEVICE,STATE,CONNECTION device 2>/dev/null | grep "^$IFACE:" || true
echo "Restored."
