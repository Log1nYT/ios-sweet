#!/bin/sh
# Host side IP only (no capture): keep 192.168.7.1/24 on the phone's USB
# Ethernet (host MAC 02:00:5e:77:ee:01) across re-enumerations, for as long
# as this runs. Capture the log with tools/venus-udplog.sh instead: `nc -ukl`
# locks onto the first sender's source port and drops every later boot.
# One sudo password at the start; sudo is kept alive in the background.
set -u
mac=02:00:5e:77:ee:01
sudo -v || exit 1
( while :; do sudo -n -v 2>/dev/null; sleep 50; done ) &
find_ifname() {
	ifconfig -a 2>/dev/null | awk -v mac="$mac" '
		/^[a-z][a-z0-9]*:/ { iface = $1; sub(/:$/, "", iface) }
		$1 == "ether" && tolower($2) == mac { print iface; exit }
	'
}
echo "keeping 192.168.7.1 on the phone's USB Ethernet ($mac)" >&2
while :; do
	i=$(find_ifname)
	if [ -n "$i" ] && ! ifconfig "$i" 2>/dev/null | grep -q "inet 192.168.7.1 "; then
		sudo -n ifconfig "$i" 192.168.7.1 netmask 255.255.255.0 up && echo "applied 192.168.7.1 on $i" >&2
	fi
	sleep 2
done
