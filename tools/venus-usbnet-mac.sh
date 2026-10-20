#!/bin/sh
# macOS host side of the phone's USB Ethernet (kernel sweetusb=2, patches
# 0075-0077; venus=1 for SM8350). Linux twin: tools/sweet-usbnet.sh.
#   tools/venus-usbnet-mac.sh [LOGFILE]
# Waits for the phone's CDC-ECM interface (host MAC 02:00:5e:77:ee:01),
# gives it 192.168.7.1/24 (the phone is 192.168.7.2), then prints the live
# kernel console the phone streams to UDP port 5140 (and appends it to
# LOGFILE if given). Needs sudo for the address.
set -eu
mac=02:00:5e:77:ee:01
echo "waiting for the phone's USB Ethernet ($mac)... (the phone image must run with sweetusb=2)" >&2

find_ifname() {
	ifconfig -a 2>/dev/null | awk -v mac="$mac" '
		/^[a-z][a-z0-9]*:/ { iface = $1; sub(/:$/, "", iface) }
		$1 == "ether" && tolower($2) == mac { print iface; exit }
	'
}

ifname=""
while [ -z "$ifname" ]; do
	ifname=$(find_ifname)
	[ -n "$ifname" ] || sleep 1
done
sudo ifconfig "$ifname" 192.168.7.1 netmask 255.255.255.0 up
echo "$ifname up: 192.168.7.1 <-> phone 192.168.7.2; live log on udp/5140 (ping 192.168.7.2 checks the link)" >&2

# The phone reboots (fastboot -> kernel) re-enumerate the ECM interface under a
# new name and macOS drops the static address.  Keep re-applying it so the
# phone's UDP log stream is not lost after a re-enumeration.
(
	while :; do
		i=$(find_ifname)
		if [ -n "$i" ] && ! ifconfig "$i" 2>/dev/null | grep -q "inet 192.168.7.1 "; then
			sudo ifconfig "$i" 192.168.7.1 netmask 255.255.255.0 up || true
			echo "re-applied 192.168.7.1 on $i" >&2
		fi
		sleep 2
	done
) &
if [ $# -ge 1 ]; then
	nc -ukl 5140 | tee -a "$1"
else
	nc -ukl 5140
fi
