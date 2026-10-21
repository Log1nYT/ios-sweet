#!/bin/bash
# Fresh UDP listener + temporary fastboot boot of v18-8g with sweethidtest=15.
set -e
cd "$(dirname "$0")/.."
sudo pkill -f 'nc -ukl 5140' || true
sudo pkill -f 'venus-usbnet-mac.sh' || true
sudo bash tools/venus-usbnet-mac.sh logs/venus-live-20261021g.txt &
fastboot boot shim/out/shim-boot-venus-v18i.img
