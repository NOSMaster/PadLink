#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Hands a Wi-Fi interface over from NetworkManager to padlink.
# Everything changed here is recorded in /run/padlink/net-state so that
# padlink-net-restore.sh can undo it exactly.
#
# usage: sudo scripts/padlink-net-up.sh [interface]   (default: wlan0)
set -euo pipefail

iface="${1:-wlan0}"
state_dir=/run/padlink
state="$state_dir/net-state"

die() { echo "padlink-net-up: $*" >&2; exit 1; }

[[ $EUID -eq 0 ]] || die "run as root: sudo $0 $iface"
[[ -d "/sys/class/net/$iface/wireless" || -e "/sys/class/net/$iface/phy80211" ]] \
    || die "$iface is not a Wi-Fi interface"
[[ ! -e "$state" ]] || die "already set up ($state exists). Run padlink-net-restore.sh first."

mkdir -p "$state_dir"
chmod 700 "$state_dir"

# Remember what to restore.
nm_managed=no
nm_connection=""
if command -v nmcli >/dev/null && nmcli -t -f DEVICE,STATE device 2>/dev/null | grep -q "^$iface:" ; then
    if ! nmcli -t -f DEVICE,STATE device | grep -q "^$iface:unmanaged"; then
        nm_managed=yes
        nm_connection="$(nmcli -g GENERAL.CONNECTION device show "$iface" 2>/dev/null || true)"
        [[ "$nm_connection" == "--" ]] && nm_connection=""
    fi
fi
ufw_rules=no
if command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q "Status: active"; then
    ufw_rules=yes
fi

{
    echo "IFACE=$(printf '%q' "$iface")"
    echo "NM_MANAGED=$nm_managed"
    echo "NM_CONNECTION=$(printf '%q' "$nm_connection")"
    echo "UFW_RULES=$ufw_rules"
} > "$state"
chmod 600 "$state"

echo "Releasing $iface from NetworkManager (connection: ${nm_connection:-none})."
echo "Your Wi-Fi internet connection on $iface stops until you run padlink-net-restore.sh."
if [[ $nm_managed == yes ]]; then
    nmcli device disconnect "$iface" >/dev/null 2>&1 || true
    nmcli device set "$iface" managed no
fi

rfkill unblock wifi 2>/dev/null || true
ip addr flush dev "$iface"
ip link set dev "$iface" down
# Some drivers report the radio busy for a few seconds after NetworkManager
# lets go; give it time before hostapd takes over.
sleep 3

if [[ $ufw_rules == yes ]]; then
    echo "Adding temporary firewall rules (DHCP + GamePad UDP ports) on $iface."
    ufw allow in on "$iface" to any port 67 proto udp comment padlink >/dev/null
    ufw allow in on "$iface" from 192.168.1.0/24 to any port 50010:50025 proto udp comment padlink >/dev/null
fi

country="$(iw reg get 2>/dev/null | awk '/^country/ {print $2; exit}' | tr -d ':')"
echo "Regulatory domain: ${country:-unknown}"
echo "Ready. Next: sudo build/padlinkd pair   (or: sudo build/padlinkd run)"
