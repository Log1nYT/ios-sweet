#!/bin/bash
# v62: new CI kernel (0114 now applies) + v58 ADT + v61 cmdline + sweetgpu=1 (read-only GPU probe)
set -euo pipefail
cd /Users/matviy/Documents/venus-sweet
RUN=${RUN:-39663397711}
REPO=Log1nYT/ios-sweet
while :; do
  s=$(gh run view "$RUN" --repo "$REPO" --json status,conclusion --jq '.status+" "+.conclusion' 2>/dev/null || echo "? ?")
  case "$s" in "completed success") break ;; "completed "*) echo "BUILD FAILED $s"; exit 1 ;; esac
  sleep 30
done
rm -rf xnu/out-venus-v62kernel
gh run download "$RUN" --repo "$REPO" -n xnu-kernel -D xnu/out-venus-v62kernel
CMDLINE='debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress wdt=-1 rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 venus=1 sweetusb=2 sweetfb_noconsole=1 venusftsdelay=10 venusftspins=1 sweetlog=0 sweetscan=- sweetfb_force_on=1 sweetrebootudp=1 sweetfb_hz=15 ufslogsecs=30 sweetgpu=1 sweetgpuprobe=1'
echo "cmdline bytes: ${#CMDLINE} (limit 607)"
python3 tools/mkpayload.py xnu/out-venus-v62kernel/kernel.development.sweet shim/out/venus-v58.adt shim/out/payload-venus-v62.bin --cmdline "$CMDLINE"
(cd shim && ./build.sh ../shim/out/payload-venus-v62.bin)
cp shim/out/shim-boot-venus.img shim/out/shim-boot-venus-v62.img
shasum -a 256 xnu/out-venus-v62kernel/kernel.development.sweet shim/out/shim-boot-venus-v62.img
strings shim/out/shim-boot-venus-v62.img | grep -o 'sweetgpu=1' | head -1
echo READY
