# 31 - HackiOS donor repos: deep analysis for venus (Mi 11, SM8350)

Date: 2026-10-22. Read-only study of `~/Documents/hackios-ref/{hackios-xnu-kernel,hackios-xnu-userland}`
(shallow clones; kernel repo APSL 2.0, userland MIT). Builds on `29-hackios-donor-map.md` and
`30-venus-hackios-port.md`. "Observation" = read in their source or seen in our logs; "Inference" = my reading.

## 0. Verdict

1. The donor is NOT a drop-in for venus. Their stack = Mu-Silicium UEFI + resident EFI loader + 4 KiB-page
   XNU (BCM2837 config) + RAM-resident APFS image + injected userland libraries. Ours = ABL `fastboot boot`
   + shim + 16 KiB XNU (SWEET config) + HFS+ root on userdata. Two different boot worlds; only the
   *ideas* and the *driver code* port, not the glue.
2. On venus the donor path stopped at "kernel hangs without a word" (the loader's `logdump` write did not
   persist: partition all zeros, read via TWRP 2026-10-22). Our path has a booting userland. Decision
   (user, 2026-10-22): go back to our path and pull the donor's *knowledge* into it.
3. What is worth taking, in this order: (a) bring-up diagnostics (systrace/regtrace/applog/user crash
   trace), (b) the injected-library fixes for window-server stalls, (c) GPU bring-up as a staged
   sequence, (d) cpufreq, (e) battery/haptics/torch details. APFS, Wi-Fi, audio, camera are separate
   projects.

## 1. Architecture side by side

| | HackiOS (redwood, SM7325) | ours (venus, SM8350) |
|---|---|---|
| Boot | Mu-Silicium UEFI via `fastboot boot`, `xnuloader` stays resident (USB fastboot link, log, watchdog) | ABL `fastboot boot` of a shim image, no resident firmware after jump |
| Kernel | xnu-8796.121.2 (iOS 16.5), config BCM2837, **4 KiB pages** | xnu-8796.141.3 (macOS 13.5 source), config SWEET, **16 KiB pages** |
| Root | APFS (own kernel driver) or RAM image; `/private/var` ramfs | HFS+ on `userdata` (sda34), `rd=ufsroot0` |
| Closed Apple drivers | stand-ins: MiniIOSurface, MiniIOMobileFramebuffer, MiniAppleKeyStore, minimac (AMFI+Sandbox policies) | our own: SweetSurface (IOSurface+IOMFB), AKS stub, Sandbox stub |
| Input | `/dev/redwood-touch` + library injected in backboardd that makes a virtual HID service | in-kernel `SweetHID*` services (touch, buttons, cover, haptics) |
| Userland | iOS 16.5, curated launchd jobs (DENY list), libs injected via `DYLD_INSERT_LIBRARIES` (`kern.exec_inject`) | iOS 16.7 (20H392) mostly as is, kernel-side `sweetdeny` |
| GPU | Adreno via Mesa/freedreno + `RedwoodGPU` (msm DRM ioctls), `gpu=1` | software CoreAnimation renderer; GPU only probed |

Observation: both kernels share XNU 8796, so their iOS-16.5-era findings apply to our 16.7 userland.

## 2. Donor kernel changes (xnu/redwood.patch = 3235 lines, ~70 core files) vs ours

Core-file changes worth knowing (what each solves) and whether we have it (grep of our patches, 2026-10-22):

| Donor change | Why they made it | Ours |
|---|---|---|
| `systrace=1/2`, `systrace_name=` (systemcalls.c): log failing syscalls incl. `__mac_syscall` policy+request, sysctl MIB | no crash reporter; find what a daemon is refused | **missing** |
| `user_exception_trace.c`: every fatal user exception: registers, shared-cache slide, text at registers, FP-walk backtrace | crash of SpringBoard etc. is otherwise invisible | **missing** (we only have `CRASH name pc lr bt` from the sampler) |
| `user_abort_trace` in kern_sig.c | where an `abort()` was called | missing |
| `regtrace=1/2` (IOUserClient.cpp): registry properties user space asks for and does not find | shows what a daemon expects from a driver | missing |
| `applog=1/2`, `applog_name=` (kern_exec.c): app stdout/stderr -> console; os_log to stderr per name | read NSLog / "Terminating app" messages | missing |
| IOPMrootDomain `gSleepDisabledFlag=1`, `syssleep=1` to undo | powerd sleep request ended in IOCPUSleepKernel hang/loop | missing (check: does our log show `powerd` sleep?) |
| kern_memorystatus.c: thresholds from `sane_size`, not `max_mem` | RAM-disk share made jetsam kill daemons early | missing; our window includes carve-outs, try it if jetsam kills appear |
| kern_priv.c: allow `PRIV_NET_PRIVILEGED_SOCKET_DELEGATE` | trustd/nsurlsessiond downloads stalled | missing (only matters with networking) |
| kern_exec.c persona checks relaxed | package managers spawn in persona 99 | not needed now |
| `IOKitBSDInit`: no 30 s wait for FS kext on `md` root; second ramdisk `RAMDisk1` | boot time | n/a (we boot `ufsroot0`) |
| `kern_minimac.c` / `miniamfi`: AMFI answers 0x5a,0x60 only, every ad-hoc binary is "platform"; Sandbox "allow all" | launchd panics if a daemon is not platform | ours exists (`SWEET Sandbox:` lines); compare its `__mac_syscall` answers |
| `loader_return` hooks in debug.c (panic -> return to loader) | their reboot path | ours: `sweet_panic` + self-reboot to fastboot cookie |

Priority-1 port (small, pure kernel, no hardware): systrace + user_exception_trace + regtrace + applog.
They are what turns "SpringBoard died, pc 0" into a named cause, and each is a few hundred lines.

## 3. Donor drivers: venus mapping (all `bsd/dev/redwood_*.c`, boot-arg gated and staged)

| Driver (lines) | Does | venus differences | Our state | Effort / risk |
|---|---|---|---|---|
| `redwood_buttons.c` (583) | SPMI arbiter reads only: power = PMK8350 PON KPDPWR, vol-down = RESIN, vol-up = PM GPIO6 | PMIC chip for GPIO6 is PM8350 on venus (redwood PM7325); same arbiter v5 layout, same `0xc440000` | `sweet_keys.c` + 0087 PON_HLOS @0x1300 + `sweetkeyvol=1`: power key confirmed; vol result unseen | low |
| `redwood_cpufreq.c` (597) | writes EPSS per-cluster frequency | venus: 3 clusters (A55/A78/X1), EPSS bases `0x18590000/0x18591000` same | none | medium: writes -> staged `cpufreq=N`; gain = speed of the CPU renderer |
| `redwood_adreno.c` (4125) + `RedwoodGPU.cpp` (1295) | 22-step Adreno bring-up + msm-DRM-ioctl IOKit service | a660 vs a643: same family, same `a660_*` firmware names, chip id differs; GCC/GPU_CC offsets SC7280 vs SM8350 | we did steps 1-6 as reads (v62-v67): chip-id `0x06060001`, ahb `0x3d00000`; v68 writes (power) result unseen | high; see §5 |
| `redwood_goodix.c` (1515) | touch via bit-banged SPI | venus panel is ST/FocalTech FTS (SPI SE11, pins 48-51) | **ours works** (`SWEET VENUS TOUCH`) | done |
| `redwood_haptic.c` (1005) | AW86927 | venus AW8697 | `0119` exists | low |
| `redwood_torch.c` (763) | PM flash LED | PM8350C | none | low-medium |
| `redwood_battery.c`/`RedwoodBattery.cpp` | numbers come from the **loader's** charger context (PMIC GLINK/ADSP) | needs resident firmware | ours: SPMI BCL path `0123`/AppleSmartBattery | not portable as is |
| `redwood_ufs.c`+`ufshc` (2950) | UFS block devices per GPT partition | same UFSHC family | ours `sweet_ufs.c` works (root runs) | none |
| audio/adsp/qmi/wifi/camera | ADSP/SMP2P/GLINK/QRTR/QMI + ath11k | different parts | none | separate projects |

## 4. Donor userland findings that may explain our stalls

Observations from `inject.c`, `inject_sb.c`, `init.c`, `mkjobs.py`:

- backboardd is patched in memory (virtual-table replacement) so that it **does not wait five seconds for an
  ambient-light sensor**; containermanagerd is told personas are not supported so it does not ask 20 times
  before giving up ("everything that needs a container waits with them", the window server among them).
  Inference: any kernel change that slows or removes these answers lengthens the window-server start. Our
  log of v72 shows backboardd looping on `IODeviceTree:/product` and accelerator matching after
  `SWEET SURF: backboardd sel 13`, never reaching the `create id 2 1440x3200` that v54 reaches.
- SpringBoard needs a start delay (`sb_delay`) because apps register one after another; its first-boot
  "hello" appears if mobileactivationd cannot answer (they fake activation in memory; ours is on disk).
- The DENY list in `mkjobs.py` is a tuned set; they warn that **a daemon may misbehave because another is
  denied** (cloudd spun on a missing nsurlsessiond). Our v70/v71 evidence agrees: denying `vmd` and
  `mDNSResponder` removed a launchd lookup storm (commcenter, wifi.manager) at no visible cost.
- launchd panics when a system job runs a non-Apple (non-platform) binary: any wrapper must be platform-signed
  or injected, not exec'd. (We found the same class of panic: `kern_exec.c:7625` from `sweetdeny`, fixed.)
- All their GL stack (`eagl.c`, 4205 lines + Mesa 25.0.7 patched) exists only because CoreAnimation prefers
  GL over its software renderer once an accelerator is present. Without `gpu=1` they use the software path
  like we do. Our rule (0114): never publish an `IOAccelerator*`/`AGX*` class unless the whole path works.

## 5. GPU: what the donor path actually needs, and where we are

Needed (all observations from `redwood_adreno.c` header): CX GDSC + clocks with frequency-counter check;
GMU firmware (`a660_gmu.bin`) loaded and started, HFI queues; GMU votes the GX rail itself; SMMU contexts for
GMU and 3D core; **TrustZone zap-shader authentication via SCM** (`a660_zap.*`; without it the core stays in
secure mode); SQE microcode (`a660_sqe.fw`); first command; then a kernel driver speaking Linux msm-DRM
ioctls, Mesa freedreno in userland, an EAGL bridge, IOSurface sharing. Hypervisor rule: reads before writes,
one staged step per boot, say what you touch before touching it.

Ours: steps 1-3 reads done with correct values; v68 power writes unseen. Missing: firmware pull from the
phone (`/vendor/firmware/a660_*`, TWRP read-only), SCM helper (0124 exists as probe), GMU/HFI, SMMU,
everything above the kernel. Estimate (inference): the kernel half is days of staged phone runs; the
userland half (Mesa+EAGL) is a project of its own. **The GPU is a speed feature, not a prerequisite for the
desktop** (v54 reached the home screen with the CPU renderer).

## 6. Our current blocker, in this light (observations, 2026-10-22)

- Repaired volume + v54 (kernel CI 39631098808) => desktop.
- v72 = v70 kernel (all wave-2 patches 0112-0126) + v54 cmdline => text console only, SpringBoard crash-loops
  (`sig 11 pc 0`). v73 = same without 0114/0116/0119/0120 => same. Bisect build without 0112-0124 is queued
  (run 39808199345). Which of the remaining patches (0100/0102 changed since v54, 0112/0113/0115/0121-0124,
  0125) breaks the window server is unknown.
- Candidate mechanisms from the donor's experience (guesses to test, not facts): added latency or changed
  answers on the paths backboardd waits on; a registry/ADT node that changes IOKit matching; 0100 file-patcher
  behaviour changed ("apply before setupfix loop", capped at 3 attempts).

## 7. Plan

1. Finish the bisect (run 39808199345 -> v74): desktop yes/no tells whether 0112-0124 or something else.
   Then halve the 0112-0124 set (4 builds worst case, ~8 min each plus a phone cycle).
2. In parallel (no phone needed): port `systrace`/`user_exception_trace`/`regtrace`/`applog` into a new
   patch (0127) so every later failure names itself. Acceptance: SpringBoard's crash prints symbolisable
   frames + slide.
3. Re-add wave-2 drivers one patch at a time on the bisected-good kernel (CI build + one phone boot each).
4. Then, in order: buttons parity (sweetkeyvol result), cpufreq (staged), battery, GPU firmware pull via
   TWRP and GPU steps 4-10 one boot per step.
5. Keep activation untouched (rule): never change `sweetfilepatch`, data_ark, prefs, ADT security flags.
