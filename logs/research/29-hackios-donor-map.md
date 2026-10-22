# 29 - HackiOS (github.com/oosexgang) as a donor for venus

Date: 2026-10-22. Source: shallow clones in `~/Documents/hackios-ref/` (`hackios-xnu-kernel`
APSL 2.0, `hackios-xnu-userland` MIT). Read-only research: nothing from there is in this repo yet.
Licence note: their kernel changes are APSL 2.0 derivatives of XNU, so porting them into our XNU
patches is fine if their file headers stay; userland is MIT.

## 0. What they are

XNU xnu-8796.121.2 (iOS 16.5) on a POCO X5 Pro (redwood, SM7325, Adreno 642L), booted from a
Mu-Silicium UEFI + their own EFI loader that stays resident (firmware services, log, watchdog).
4 KiB pages (we use 16 KiB). Root = APFS (own kernel APFS driver) or a RAM image. iOS userland is
Apple's, but they start only a hand-picked set of launchd jobs.

## 1. Stability lessons (what they did, how it maps to our crashes)

| Lesson | Where | Our evidence | Action |
|---|---|---|---|
| Do NOT start every Apple daemon. `DENY` list in `userland/mkjobs.py`: watchdog, CommCenter, wifi, bluetooth, **locationd**, softwareupdate, **datamigrator**, backupd, thermalmonitord, keybagd, nfcd, biometrickitd, obliterat, storage_mounter, sepd, seputil, camera, **ReportCrash**, osanalytics, lockdown, audio, mobileassetd, bird, **homed, routined, duetexpertd**, dmd, syncdefaultsd, photoanalysisd, avconferenced. Jobs run on demand only (RunAtLoad/KeepAlive removed). | `mkjobs.py:196-226` | Our live log: locationd crashed 60x, gpsd 22x, homed 8x, fairplayd/bluetoothd 23x, datamigrator 6x; the `kmem_alloc(3 MiB, NOFAIL) failed with 3` panic fires right after `duetexpertd`/`routined`/`OTACrashCopier` start (3 of 3 boots). | Patch 0126 `sweetdeny=` (kernel refuses exec of named processes, nothing on disk changes). First list: routined, duetexpertd, homed, locationd, gpsd, OTACrashCopier, ReportCrash. Keep `datamigrator` / `mobileactivationd` / lockdown / purplebuddy OUT of it (activation rule). |
| A daemon can misbehave because ANOTHER one is denied (cloudd retried nsurlsessiond thousands of times). Retest the whole list after each change, with a control run. | `mkjobs.py` comments | | change one word at a time |
| The kernel runs under Qualcomm's hypervisor: an access it dislikes ends the run at once or ~15 s later, often with no message. So every new MMIO access is a staged step: `reads before writes`, `adreno=N`, `buttons_steps=N`, "say what you are about to touch before touching it". | header of `redwood_adreno.c`, `redwood_buttons.c` | silent resets (RSC typo), v68 hang | follow the same staging for every port |
| jetsam thresholds are percent of `max_mem`; with a big reserved/RAM-disk share they killed daemons too early. They use `sane_size` instead (`kern_memorystatus.c` hunk). | `redwood.patch` ~l.752 | our `dram-size` window includes non-RAM carve-outs | try the same one-liner if jetsam kills show up |
| Loader keeps the hypervisor watchdog petted every 10 s (`hv_wdt_pet`, SMC 0x86000007) only when the loader armed it. | `osfmk/arm64/loader_return.c:505` | we never arm it | none |
| `docs/checks.md`: "missing information reads as a missing problem". Inference from absence is the usual lie: a `grep` for a pattern that is never printed proves nothing. | | I did exactly this: grepped only `Kernel data abort|Unaligned` and called v67/v68 panic-free while `kmem_alloc ... failed with 3` was in the log. | every "no panic" claim must grep the generic `SWEET PANIC` |

## 2. Drivers worth porting (file, size, venus relevance)

| Donor file | Lines | What | Venus (SM8350, Adreno 660, PM8350) |
|---|---|---|---|
| `bsd/dev/redwood_adreno.c` + `iokit/Kernel/RedwoodGPU.cpp` | 4125 + 1295 | 22-step Adreno bring-up (`adreno=N`): cmd-db rails, CX GDSC + clocks with a frequency counter, GMU regs, SMMU, TZ zap-shader auth, GMU firmware + HFI, SQE microcode, first command, 3-colour picture. GMU firmware votes the GX rail itself. | **Top priority.** Same family and same cmd-db `0x80860000`. Firmware = `a660_*` from the phone's `/vendor/firmware`. Change addresses (gmu, smmu, gcc offsets) to SM8350. |
| `bsd/dev/redwood_buttons.c` | 583 | power = PMK8350 PON KPDPWR, volume down = RESIN, volume up = **PM GPIO6** (same wiring as venus), read-only, observer channels, 60 Hz, two-looks debounce, `buttons_steps=N`. | Compare with our `sweet_keys.c`; our volume-up needs `sweetkeyvol=1`. |
| `bsd/dev/redwood_battery.c`, `RedwoodBattery.cpp` | 198 + 230 | battery numbers come from the UEFI loader's charger context (PMIC GLINK/ADSP), kernel only reads a shared cache. | Needs a resident loader/UEFI; not portable to our ABL-booted shim as is. Our own PMIC SPMI BCL path (0104) stays. |
| `bsd/dev/redwood_cpufreq.c` | 597 | EPSS frequency table write per cluster. | Venus EPSS differs; later, for speed. |
| `bsd/dev/redwood_haptic.c`, `redwood_torch.c` | 1005 + 763 | AW86927 haptics, PM8350C flash LED. | Venus uses AW8697 haptics (our 0119 already). |
| `bsd/dev/redwood_sound.c`, `redwood_audio.c`, `redwood_adsp.c`, `redwood_qmi.c` | 1518 + 354 | audio DSP services, QMI/GLINK/SMP2P/QRTR. | Later. |
| `redwood_wifi.c`, `redwood_lx.c`, `redwood_wlfw.c`, `wifi/` | big | ath11k on a Linux compat layer for WCN6750. | Venus has WCN6855/QCA6390-class: different driver, much later. |
| `bsd/miscfs/apfs/*` | many | own kernel APFS (read/write) + libdeflate + libzbitmap. | A root without our HFS+ corruption class; big project, later. |
| `bsd/dev/redwood_ufs.c`, `redwood_ufshc.c` | 1619 | UFS block devices per GPT partition. | We have `sweet_ufs.c`. |

## 3. Order I propose

1. Stability first: `sweetdeny` (0126) + panic backtrace (0125) in v69; read the real caller of the 3 MiB
   `kmem_alloc` from the backtrace (`nm -n` on the kernel image).
2. GPU: pull `a660_*` from the phone's `/vendor/firmware` over TWRP (read-only), then port
   `redwood_adreno.c` step by step (`adreno=1..N`, one step per boot, reads before writes).
3. Buttons: align our `sweet_keys.c` with `redwood_buttons.c` (volume up GPIO6, volume down RESIN).
4. Battery/thermal: keep our SPMI reads, add the loader-less parts only.
