#!/bin/sh
# Keep the UDP console listener alive (it can die; the phone then logs into the void).
LOG=${1:-logs/venus-live.log}
while :; do
	python3 "$(dirname "$0")/venus-udplog.py" "$LOG"
	sleep 1
done
