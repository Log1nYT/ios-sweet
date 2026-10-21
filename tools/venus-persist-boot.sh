#!/bin/bash
# Make the phone boot our iOS by itself (no Mac needed).
#
# The ABL runs the SAME BootLinux() for "fastboot boot <file>" and for a file
# flashed into the boot partition (research report 25), and our shim image is a
# valid Android boot image (ANDROID! header v1), so:
#     fastboot flash boot_b <image>   &&   fastboot set_active b
# makes the phone boot iOS on every power-up.  Rollback is one command:
#     fastboot set_active a
#
# DESTRUCTIVE: it overwrites the stock boot image of slot b.  Slot a is never
# touched, so Android stays recoverable.  Requires the user's OK.
set -euo pipefail
cd "$(dirname "$0")/.."
IMG=${IMG:-shim/out/shim-boot-venus-v43.img}
echo "image: $IMG"
[ -f "$IMG" ] || { echo "no such image"; exit 1; }
fastboot devices || true
read -r -p "Type FLASH to overwrite boot_b with $IMG: " ans
[ "$ans" = "FLASH" ] || { echo "aborted"; exit 1; }
fastboot flash boot_b "$IMG"
fastboot set_active b
echo "done: the phone now boots iOS on its own.  rollback: fastboot set_active a"
