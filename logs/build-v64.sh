#!/bin/bash
# v64: new CI kernel (0112 unaligned-read fix) + v58 ADT + v61 cmdline + sweetgpu=1 (read-only GPU probe)
set -euo pipefail
cd /Users/matviy/Documents/venus-sweet
RUN=${RUN:-39665100074}
REPO=Log1nYT/ios-sweet
CMDLINE='debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress wdt=-1 rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 venus=1 sweetusb=2 sweetfb_noconsole=1 venusftsdelay=10 venusftspins=1 sweetlog=0 sweetscan=- sweetfb_force_on=1 sweetrebootudp=1 sweetfb_hz=15 ufslogsecs=30 sweetgpu=1 sweetgpuprobe=1 sweetpaniclast=1 sweetpanicgrace=300000'
echo "cmdline bytes: ${#CMDLINE} (limit 607)"
python3 tools/mkpayload.py xnu/out-venus-v63kernel/kernel.development.sweet shim/out/venus-v58.adt shim/out/payload-venus-v64.bin --cmdline "$CMDLINE"
(cd shim && ./build.sh ../shim/out/payload-venus-v64.bin)
cp shim/out/shim-boot-venus.img shim/out/shim-boot-venus-v64.img
shasum -a 256 xnu/out-venus-v63kernel/kernel.development.sweet shim/out/shim-boot-venus-v64.img
strings shim/out/shim-boot-venus-v64.img | grep -o 'sweetgpu=1' | head -1
echo READY

