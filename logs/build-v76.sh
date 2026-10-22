#!/bin/bash
# v76: new CI kernel (panic backtrace + sweetdeny) + v58 ADT + v61 cmdline + sweetgpu=1 (read-only GPU probe)
set -euo pipefail
cd /Users/matviy/Documents/venus-sweet
REPO=Log1nYT/ios-sweet
CMDLINE='debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress wdt=-1 rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 venus=1 sweetusb=2 sweetfb_noconsole=1 venusftsdelay=10 venusftspins=1 sweetlog=0 sweetscan=- sweetfb_force_on=1 sweetfilepatch=mad:170d4:00484039:20008052,mad:1b5d0:e0030034:1f2003d5,mad:41e2c:76050036:1f2003d5,mad:41e94:000545f9:000945f9,mad:46218:760c0036:1f2003d5,mad:4629c:a00c0036:1f2003d5,mad:462cc:f50c00b4:1f2003d5 sweetdeny=routined,duetexpertd,homed,locationd,gpsd,OTACrashCopier,ReportCrash,vmd,mDNSResponder'
echo "cmdline bytes: ${#CMDLINE} (limit 607)"
python3 tools/mkpayload.py xnu/out-venus-v54kernel/kernel.development.sweet shim/out/venus-v76s2.adt shim/out/payload-venus-v76.bin --cmdline "$CMDLINE"
(cd shim && ./build.sh ../shim/out/payload-venus-v76.bin)
cp shim/out/shim-boot-venus.img shim/out/shim-boot-venus-v76.img
shasum -a 256 xnu/out-venus-v54kernel/kernel.development.sweet shim/out/shim-boot-venus-v76.img
strings shim/out/shim-boot-venus-v76.img | grep -o 'sweetgpu=1' | head -1
echo READY
