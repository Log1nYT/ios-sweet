# 37 - Full GPU support for venus: implementation plan (2026-10-23)

Status when written: desktop works on the full kernel (v84), the hypervisor's virtual watchdog is armed and
petted (v86), a660 firmware is pulled (hashes in NOTES.md), the touch/probe blocker is understood
(io_submap 8 MiB exhausted, patch 0132, CI run 40001649090). "OBS" = read in a file or log, "INF" = my inference.

## 1. What we now know (facts first)

### 1.1 SM8350 / Adreno 660 facts from the phone's live DTS (`~/Documents/hackios-venus/dts/venus.dts`) [OBS]

| Block | Value | DTS line |
|---|---|---|
| GPU registers | 0x3d00000, 0x40000; chipid 0x06060001; highest-bank-bit 0x10; min-access-length 0x20; ubwc-mode 4 | 22750-22770 |
| GPU rscc / cx_dbgc / isense / qdss | 0x3de0000 (0x10000) / 0x3d61000 / 0x3d8b000 / 0x6900000 | 22753 |
| GMU registers | 0x3d6a000, size 0x34000 (the node label says 3d69000, the reg says 3d6a000); IRQs 0x130 (HFI), 0x131 (GMU); mbox "aop" | 23585-23600 |
| GMU PDC config | 0xb290000, size 0x10000 (no separate seq region listed) | 23587 |
| GPU SMMU | 0x3da0000, size 0x20000, `qcom,kgsl-smmu-v2`; TBUs at 0x3dc5000 / 0x3dc9000 | 13413, 23556 |
| SMMU streams | gfx3d_user SID 0, gfx3d_lpac SID 1, gfx3d_secure SID 2, **gmu (kernel context) SID 5**, all mask 0x400 | 23563-23596 |
| GPU_CC | 0x3d90000; CX GDSC 0x3d9106c, GX GDSC 0x3d9100c, CX hw ctrl 0x3d91540 | 6819-6860 |
| cmd-db | 0x80860000 (measured in v67) | NOTES |

The same two stream facts (GMU firmware/HFI/log buffers belong to the GMU **kernel** context, SID 5, mask 0x400;
`gmu_user` SID 4 is a different address space) are what the Mi 10 fork found out the hard way (routing through
stream 4 reset the phone at CM3 reset release).

### 1.2 What the donors prove (so we do not rediscover it)

| Donor | Proves | Where |
|---|---|---|
| Mi 10 `umi` fork (A650, same GMU/HFI generation as A660) | full chain on hardware: clocks/CX, SMMU, GMU init returns 0x100, HFI version 0x20000001, GMU powers the core, TrustZone authenticates the zap shader, CP leaves secure mode, three-colour picture, then CoreAnimation through Mesa FD650 (>1000 frames) | `~/Documents/umi-ref/.../docs/umi-gpu.md` (read via API; code in `hk_adreno.c`) |
| umi pitfalls | do NOT skip SCM/PAS stages 10-12 (stalls at stage 19); GMU clock wake/sleep fields 0xf; RSCC sleep/wake + AOP-managed PDC prepared before GMU start; A650: primitive FIFO threshold, highest bank bit 16 (venus DTS also says 16); fixed 587 MHz operating point | same doc |
| Adreno 618 `ios-sweet` fork (same patch architecture as ours) | the ladder as boot-arg levels: 1 probe, 2 CX power + GMU clocks, 3 SMMU + TCM test, 4 GMU cold boot (0xBABEFACE), 5 SMMU+HFI, 6 GX via GMU OOB, 7 signed zap PAS, 8 CP buffers, 9 CP config (no commands yet there) | `~/Documents/a618-ref/patches/0078..0098` (21 patches, SHA of the files by `ls`) |
| a618 pitfall | exact SMRs: with no valid SMR the SoC resets; flush the console log before each step; ATOS disabled by the DT; 64-bit `pmap_sync_page_attributes_phys` (our 0131 is the same fix) | 0083, 0084, 0085 |
| HackiOS redwood | 22 staged steps, reads before writes, "say what you touch before you touch it" | report 31 |

### 1.3 Blockers found today and removed or understood

1. Touch thread hung forever when the GPU probe was on: `ml_io_map` windows (io_submap, 8 MiB, KMA_NOFAIL) exhausted -> patch 0132 (64 MiB). Same cause as the old `kmem_alloc(..., 3145728, 0x100021)` panic. [OBS in v90 log + code]
2. Accessors: `gp_rd/gp_wr` are now noinline single `ldr`/`str` (G0). [done]
3. Watchdog: SMC 0x86000006/5/7/8 work; `sweetwdt=2` arms (30 s bite, 5 s pet). [OBS v86]
4. The non-journaled HFS+ root can be damaged by any silent reset: repair procedure exists, activation re-verified after each write. [OBS]
5. The SCM/PAS and SMMU code of ours (0115, 0124) exists as probes only.

## 2. Phases (every phase = one or two boot-arg steps, one CI build, one phone cycle; riskiest write last)

Rule for every step: announce what is about to be touched in the log, wait for it to reach the host, do the access, read it back, log it; the step only runs if the previous step's success flag is set; the watchdog is armed first (`sweetwdt=2`); the console log is not on the critical path.

| Phase | Boot-arg | What it does | Success criterion | Failure signature -> rollback | Source to port |
|---|---|---|---|---|---|
| P0 | `sweetwdt=2 sweetgpuprobe=1` | base: touch alive, probe prints | touch works, probe lines complete | touch hang -> io window (0132) | done (0132) |
| P1 | + `sweetgpupower=1` | CX GDSC on, GMU clocks on, frequency counter check, undo list | CX hw ctrl bit, GMU AO counter ticks (about 1.92 M per 100 ms) | silent reset -> power-cycle, mark block | our 0113 (v68 hung: was it the io window? retest first), a618 0079 |
| P2 | `sweetgpusmmu=1` | GPU SMMU: context bank for the GMU kernel context (SID 5), exact SMRs (SIDs 0,1,2,5 mask 0x400), static page table for GMU buffers, ATOS not used | S2CR/SMR read back, FSR clean, no reset | reset (wrong SMR) -> stop, record | a618 0080/0082/0085, umi, our 0115 |
| P3 | `sweetgpugmu=1` | load `a660_gmu.bin` (55256 B) into ITCM/DTCM with CM3 held in reset, prepare RSCC/PDC (wake/sleep 0xf), release CM3, wait for init result, hold in reset again | GMU init result 0x100 / 0xbabeface seen | hang or reset at CM3 release (stream 4 vs 5 mistake) -> undo | a618 0081, umi |
| P4 | `sweetgpuhfi=1` | HFI queues (version 0x20000001), H2F_MSG_INIT/FW_VER, GMU back to slumber | HFI handshake | HFI error -> stop | a618 0082/0086, umi |
| P5 | `sweetgpugx=1` | GX rail by the GMU OOB (never voted by hand), lowest operating point first | GMU_SPTPRAC bits clear, core powered | GX rail event -> possible reset | a618 0087, umi |
| P6 | `sweetgpuscm=1` | SCM availability + PAS 13 supported (read-only), then signed zap AUTH_AND_RESET, SMMU stream 0 and 1 set up first (stage 10-12 NOT skipped) | TZ returns 0 | TZ error / reset -> stop, riskiest write so far | a618 0088-0092, umi, our 0124 |
| P7 | `sweetgpucp=1` | CP ring, SQE microcode (`a660_sqe.fw`), A660 3D init (bank bit 16, FIFO values to be read from the A660 register list), CP start, leave secure mode, write one word | word appears in memory | CP hang -> halt SQE, power-off order | a618 0093-0098, umi |
| P8 | `sweetgpupic=1` | GPU fills three colour bands in a spare buffer (not the panel), checksum | expected pixels | SMMU fault -> stop | umi `umi-gpu.md` |
| P9 | `sweetgpu_keep=1 sweetgpu_recover=1` | kernel service (our 0114 class rules: never publish an IOAccelerator*/AGX* class until the whole path works) + hang recovery | submission/complete cycle | recover fails -> remove arg | donor `RedwoodGPU.cpp` |
| P10 | userland | EAGL + Mesa (freedreno) behind backboardd, **CPU renderer stays the automatic fallback**; fps tools | CoreAnimation frames through the GPU, measured win | UI freeze -> remove arg | HackiOS/umi userland repos |

Parallel track S (does not need the GPU): `sweetfb_hz=30`, cpufreq (EPSS reads then writes), core placement, command-mode TE copy window (umi `hk_display.c`, panel measured about 59.93 Hz) - report 32 section 5.1.

## 3. How we work (staying fast and safe)

- Code for P1-P8 is written ahead of the phone: port the a618 patches 0080-0098 onto our tree as new patches (0140+) with the SM8350 constants of section 1.1, cross-checked against the umi fix list. They are bigger than anything ours so far (a618 0082 alone is 22 KB), so each stays opt-in by its boot-arg and compiles cleanly before pushing (`tools/research/compile-one.sh`).
- One phase per CI build and per phone cycle. A CI build takes 10-15 minutes, a phone cycle 3-5 minutes plus the user.
- Between phases: read the log, then decide. If a phase causes a reset, run the metadata `fsck` procedure before the next boot (activation re-verified after any write).
- All GPU patches keep default builds untouched (no GPU boot-arg in the default command line).

## 4. Effort (honest, INF)

| Milestone | Estimate | Notes |
|---|---|---|
| P0-P1 verified | 1 session | needs CI 40001649090, then one phone cycle each |
| P2-P4 (SMMU, GMU start, HFI) | 3-5 sessions | biggest code port; two donors agree on the order |
| P5-P8 (GX, zap, CP, picture) | 4-8 sessions | zap and CP are where the hypervisor kills silently |
| P9 kernel service + recovery | 2-3 sessions | |
| P10 userland (Mesa + EAGL) | weeks | donor builds exist (umi/HackiOS `GL=1`), but it is a separate project; stay on the CPU renderer until it is measured better |

## 5. Needs from the user

1. Phone in fastboot for each cycle (and the sudo password for the USB network keeper after a Mac restart).
2. OK for each new write class: P1 (CX power), P5 (GX), P6 (TrustZone zap), P7 (CP start).
3. A cool, uncharged phone for P5-P8 (thermal rail events).
