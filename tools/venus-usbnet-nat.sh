#!/bin/sh
# Shares this Mac's internet connection with the phone over the USB-ECM
# link (ue0 kernel default route -> 192.168.7.1, patch 0097). Run this
# ALONGSIDE venus-usbnet-mac.sh (needs the same ifname, e.g. en7) while
# the phone is booted.
#   sudo bash tools/venus-usbnet-nat.sh <mac-ifname-for-the-phone's-ECM-link> <uplink-ifname>
# Example: sudo bash tools/venus-usbnet-nat.sh en7 en0
set -eu
[ $# -ge 2 ] || { echo "usage: $0 <ecm-ifname> <uplink-ifname>  (e.g. en7 en0)" >&2; exit 1; }
ecm="$1"; up="$2"
sudo sysctl -w net.inet.ip.forwarding=1
anchor=/tmp/venus-nat.conf
cat > "$anchor" <<RULES
nat on $up from 192.168.7.0/24 to any -> ($up)
pass in on $ecm all
pass out on $up all
RULES
sudo pfctl -f "$anchor" -e 2>&1 | grep -v "^No ALTQ" || true
echo "NAT on: phone (192.168.7.2) -> $ecm -> $up -> internet. 'sudo pfctl -d' to stop." >&2
