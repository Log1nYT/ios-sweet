#!/bin/bash
# v88: v87 without sweetgpu=1 (its 5 s thread is the suspect for the touch thread never waking); sweetgpuprobe=1 stays; v86 + sweetgpu=1 sweetgpuprobe=1 (G1/G2 read-only probes under the armed watchdog; keepsyms dropped for room); G0b kernel, sweetwdt=2 arms the hypervisor watchdog (30 s bite, 5 s pet); G0 kernel (noinline ldr/str accessors, sweetwdt status probe) + sweetwdt=2 on the v84 recipe; FULL kernel (all patches 0112-0131, GPU args off) on the v83 recipe; kernel with 0130 (mad alias) + 0131 (pmap widen); v82b recipe; new CI kernel (panic backtrace + sweetdeny) + v58 ADT + v61 cmdline + sweetgpu=1 (read-only GPU probe)
set -euo pipefail
cd /Users/matviy/Documents/venus-sweet
REPO=Log1nYT/ios-sweet
CMDLINE='debug=0xc serial=3 cpus=8 -noprogress wdt=-1 rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 venus=1 sweetusb=2 sweetfb_noconsole=1 venusftsdelay=10 venusftspins=1 sweetlog=0 sweetscan=- sweetfb_force_on=1 sweetfilepatch=mad:170d4:00484039:20008052,mad:1b5d0:e0030034:1f2003d5,mad:41e2c:76050036:1f2003d5,mad:41e94:000545f9:000945f9,mad:46218:760c0036:1f2003d5,mad:4629c:a00c0036:1f2003d5,mad:462cc:f50c00b4:1f2003d5,/System/Library/Caches/com.apple.dyld/dyld_shared_cache_arm64.09:37ab75c:a1850194:000080d2 sweetwdt=2 sweetgpuprobe=1'
echo "cmdline bytes: ${#CMDLINE} (limit 607)"
python3 tools/mkpayload.py xnu/out-venus-v86kernel/kernel.development.sweet shim/out/venus-v80m.adt shim/out/payload-venus-v88.bin --cmdline "$CMDLINE"
(cd shim && ./build.sh ../shim/out/payload-venus-v88.bin)
cp shim/out/shim-boot-venus.img shim/out/shim-boot-venus-v88.img
shasum -a 256 xnu/out-venus-v86kernel/kernel.development.sweet shim/out/shim-boot-venus-v88.img
strings shim/out/shim-boot-venus-v88.img | grep -o 'sweetgpu=1' | head -1
echo READY
