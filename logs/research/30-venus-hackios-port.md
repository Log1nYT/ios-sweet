# 30 - Porting the HackiOS stack to venus (Mi 11) - state and plan

Date: 2026-10-22. User decision: take the donor's stack (UEFI + loader + kernel + drivers) and adapt
it to venus, and import/adapt all drivers. Work copy: `~/Documents/hackios-venus/` (copy of the
donor repos, NOT our git; nothing of it is published). The current shim path stays usable until the
new path boots. Activation rule: their system runs from RAM by default and never writes the phone's
storage, so the activation-patched iOS volume on userdata stays untouched.

## Hardware match (device trees: donor `uefi/files/Resources/DTBs/redwood.dtb` vs Mu-Silicium `venus.dtb`)

Same addresses on both SoCs (SM7325 redwood / SM8350 venus):

| Block | redwood | venus |
|---|---|---|
| GPU kgsl-3d0 | 0x3d00000 | 0x3d00000 |
| GMU | 0x3d69000 | 0x3d69000 |
| GPU_CC | 0x3d90000 | 0x3d90000 |
| GX / CX GDSC | 0x3d9100c / 0x3d9106c | same |
| kgsl-smmu | 0x3da0000 | 0x3da0000 |
| apps RSC | 0x18200000 | 0x18200000 |
| apps SMMU | 0x15000000 | 0x15000000 |
| SPMI arbiter | 0xc440000 | 0xc440000 |
| GCC | 0x100000 | 0x100000 |
| EPSS L3 / cpufreq debug | 0x18590000 / 0x18591000 | same |
| GIC dist / redist | 0x17a00000 / 0x17a60000 | same |
| cmd-db | 0x80860000 | 0x80860000 (measured, v67) |

Different: qfprom (fuses) 0x780000 -> 0x784000; GPU a643 (chip 0x06030500) -> a660 (0x06060001)
(same a660_* firmware family); GCC branch offsets and the GPU_CC PLL tables must be checked one by
one (SC7280 vs SM8350 clock drivers); PMIC: redwood PM7325 GPIO6 (vol up) vs venus PM8350 GPIO6.
CPU: venus 8 cores, MPIDR Aff1 0..7 (core_shift 8), cores 0-3 A55, 4-6 A78, 7 X1.

## Done

- `common/devices/venus.conf` + `platform_venus` in `xnuloader/loader.c` (GIC, cores, small-core
  mask 0x0f, fb_base 0xE2A00000 = the UEFI map's "Display Reserved", TO VERIFY at the first run:
  the loader prints "display W x H at ...", and picks its platform by that address).
- `xnuloader/build.sh` builds `loader.efi` (68 KB) on this Mac with Homebrew clang 23 + lld-link.
- `new-device.sh` needed BSD sed (`sed -i ""`) on macOS; its MPIDR parser does not read
  two-cell `reg = <0x00 0x100>` (it said shift 0); fixed by hand.

## Open

1. UEFI: the local `~/Documents/Mu-Silicium` (not a git tree, has the earlier IosLoader work) takes
   `BootShim.S` and `BootManagerLib.inf` from `uefi/mu-silicium.patch`, `BootManager.c` conflicts
   (1 hunk). Either adapt by hand or clone Mu-Silicium at the donor's BASE e51cd877 (~3 GB).
2. Disk: 2.9 GiB free. The repair copy `~/Documents/ipsw-extract/repair/userdata-used.img` holds
   26 GiB on disk and is no longer needed (the repaired blocks are on the phone).
3. Kernel: their `prebuilt/mach.development.bcm2837` boots without a build; their source build needs
   Linux + theos (a Linux CI job would do).
4. System image: their `userland/build.sh` (+ `ios165/fetch.sh`, 7.4 GB iOS 16.5 IPSW from Apple's
   CDN) expects Linux tools (7-Zip >= 23, lz4, gcc). Alternative: our 16.7.16 dmgs.
5. Drivers, in this order, each gated by its boot-arg and staged (`*_steps=N`): framebuffer only
   -> buttons (`redwood_buttons.c`, same SPMI/PON/GPIO6 layout) -> touch (our ST FTS driver from
   `sweet_venus_touch.c` behind their `/dev/redwood-touch`) -> GPU (`redwood_adreno.c`: qfprom,
   chip id, GCC/GPU_CC offsets) -> cpufreq (EPSS, same base) -> haptics (venus AW8697 vs AW86927)
   -> torch (PM8350C) -> sound/camera/Wi-Fi (different parts, later).
