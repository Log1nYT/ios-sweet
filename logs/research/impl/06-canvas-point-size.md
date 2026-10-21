# 06 - Canvas and point size: present 1125x2436 @3x (375x812 pt) on the 1440x3200 panel

Implementation of item 14 of `logs/research/24-upstream-backlog.md` ("Presented geometry /
`display-scale` must be a real iPhone point size") and of the "two halves of the canvas
rule" in `NOTES.md` L195 / L377.

Date: 2026-10-21. Author: implementation agent (canvas + point size).

---

## 1. The truth from the code

### 1.1 What the kernel publishes

`iokit/Kernel/arm/SweetFramebuffer.cpp` (patches 0046, 0056, 0061, 0079, 0103, 0111):

| item | source | value on venus |
| --- | --- | --- |
| **panel** geometry | `PE_current_console(&video)` = `boot_args.Video`, filled by the shim from `shim/console.h` (`FB_BASE/FB_W/FB_H/FB_STRIDE`) | 0xe2a00000, 1440x3200, stride 5760 |
| **presented** size | `fLogicalW/fLogicalH` | was 1440x3200, now 1125x2436 |
| `GetDisplaySize` (user client **selector 8**) | returns `fLogicalW/fLogicalH` | 1440x3200 -> 1125x2436 |
| default surface (selector 3) | the panel memory as an IOSurface | 1440x3200 |
| swap path (selector 5, record 0x46c) | the enabled layers' surfaces are copied into the panel memory | 1:1 copy (0111 adds a scale-up) |

XNU itself knows nothing about the display size: `grep -rn "display-scale" logs/bt3/xnu`
returns **nothing**. The presented size leaves the kernel only through the IOMobileFramebuffer
user client (selector 8) and through the device tree.

### 1.2 What the userland does with it

* **libMobileGestalt** (`MGIOMFBSupport.c`, `sub_19c560a54`) reads `display-scale` (u32,
  default 0) and `display-rotation` from **IODeviceTree:/chosen**, and the **pixel size**
  from `IOMobileFramebufferGetDisplaySize` - the live log shows
  `SWEET FB: MobileGestaltHelper sel 8 in 0/0 out 2/0` and `sel 28` (dot pitch).
* **SpringBoard** turns that into an `FBSDisplayMode`. The failure string from `NOTES.md`
  L432 shows the format:

  ```
  FBSDisplayMode cannot be configured as requested :
      <FBSDisplayMode: ...; infxinf@0x (1080x2400/0) 60Hz sRGB SDR>
  ```

  i.e. `<pointW>x<pointH>@<scale>x<scale> (<pixelW>x<pixelH>/<scale>)`. With scale 0 the
  point size is `pixel/scale` = infinity -> "infxinf" -> the mode is rejected
  (checks: w/h >= 2, preferredScale > 0, scaleOverride >= 0, refreshRate != 0, valid
  gamut/HDR/rotation). **point size = pixel size / display-scale.**
* **IOMobileFramebuffer.framework** (upstream RE, upstream `NOTES.md` L113):
  `__ioMobileFramebufferAlloc` reads the CFPreferences domain
  `com.apple.iokit.IOMobileGraphicsFamily` (`canvas_width/height`,
  `override_display_width/height`, `override_panel_width/height`), and `getEDTProperty`
  reads **`canvas-width` / `canvas-height` from the service's IODeviceTree parent** -
  for us the `/arm-io/disp0` node. The canvas is what sizes the render target: with the
  750x1334 canvas, backboardd created a 750x1334 surface
  (`SWEET SURF: backboardd create id 2 750x1334 ...`).

So there are **two sizes** and they are not the same thing:

* the **canvas** (device tree) = the size the userland renders at;
* the **display size** (kernel, selector 8) = the pixel size the display mode and the
  point size are computed from.

### 1.3 The "two halves" and the rule that connects them

| config | canvas (DT) | GetDisplaySize (sel 8) | point size | result |
| --- | --- | --- | --- | --- |
| venus v0-v9 (payload-venus-v0..v9) | 750x1334 | 1440x3200 | 720x1600 | **no swaps at all** |
| venus v13/v18+ (payload-venus-v30..v40) | 1440x3200 | 1440x3200 | 720x1600 | thousands of swaps |
| upstream sweet 0061 (1080x2400 panel) | 1125x2436 | 1125x2436 | 375x812 | works, no red status bar |

* Half 1 - the **canvas must equal the size the kernel reports**. When they disagree,
  backboardd swaps *empty* frames: in `logs/venus-live-20261020c.txt` (canvas 750x1334)
  `sel 4` (SwapBegin) and `sel 5` (SwapEnd) are called, but the kernel never logs a
  layer, because the kernel only logs an **enabled** layer and the record had none
  (`SweetFramebuffer.cpp`: the `mask`/`id` test is before the log). Result: a black
  panel, no crash, no clue - exactly the "750x1334 broke the swaps" lesson.
  The framework has no reason to compare the canvas with the panel: upstream sweet
  ran canvas 1125x2436 with a 1080x2400 panel *and* a 1080x2400 default surface
  (patch 0061) and that worked, so "the canvas must be the real panel size" is
  really "the canvas must be the size the kernel reports".
* Half 2 - the **point size must be a real iPhone screen**. Upstream's 1080x2400 @3x =
  360x800 pt is no iPhone and gave the red status bar (`rdar://45025538`, UIKit "No
  status bar visual provider for screen"); upstream fixed it by presenting 1125x2436
  @3x = 375x812 pt. Our 1440x3200 @2x = 720x1600 pt is just as unreal (the iPads are
  768x1024, 810x1080, 820x1180, 834x1194, 1024x1366) - the symptom may already be
  there, we simply have not looked at a photo of the venus status bar.

### 1.4 Option (a) is impossible

1440/375 = 3.84 and 3200/812 = 3.94: no integer display scale maps 1440x3200 onto
375x812. The only integer scales give 1440x3200@1, 720x1600@2, 480x1066.67@3 (not an
integer height) and 360x800@4 (upstream's broken case). **No real iPhone point size is
reachable from a 1440x3200 canvas.** The canvas has to change.

---

## 2. The decision: option (b)

**Present the iPhone X canvas 1125x2436 at display-scale 3 (375x812 pt) and scale it up
to the 1440x3200 panel.**

Why 1125x2436 and not the iPhone 8's own 750x1334 @2x (375x667 pt, also a real iPhone
point size, and the size the venus ADT used first):

* **aspect**: 1125x2436 is 0.4618, the 1440x3200 panel is 0.45 -> a 2.6 % vertical
  stretch, invisible. 750x1334 is 0.562 -> a 25 % vertical stretch, very visible.
* **proved**: upstream sweet renders iOS 16.7.16 with exactly this geometry (iPhone X
  userland + iPhone X identity), and the red status bar went away.
* **it fits**: the canvas is smaller than the panel, so the scaled canvas covers the
  whole panel (no letterbox bars).
* **it is cheaper**: CoreAnimation's CPU renderer (there is no GPU) draws 2.74 Mpx
  instead of 4.6 Mpx per frame, -40 %.

Option (c) (keep 1440x3200 @2x, accept 720x1600 pt) is rejected: that point size belongs
to no device, and UIKit picks its status bar / layout assets per real iPhone screen.

---

## 3. The knobs (exact)

### 3.1 `tools/mkadt_venus.py` (the ADT)

| property | before | after |
| --- | --- | --- |
| `/chosen/display-scale` | 2 | **3** |
| `/arm-io/disp0 canvas-width` | 1440 | **1125** (`CANVAS_W`) |
| `/arm-io/disp0 canvas-height` | 3200 | **2436** (`CANVAS_H`) |

Nothing else in the ADT changes: `/sweet-fbcon` (width/height/stride) and the panel
constants `FB/FB_W/FB_H/FB_STRIDE` stay 1440x3200 - they describe the **panel**, not the
canvas.

### 3.2 `xnu/patches/0111-venus-canvas-scale.patch` (new kernel patch)

`iokit/Kernel/arm/SweetFramebuffer.cpp`:

1. `AppleCLCD::start()` reads `canvas-width` / `canvas-height` from the provider nub
   (the `/arm-io/disp0` node - the same two properties the framework reads) and uses
   them as the presented size, so `GetDisplaySize(8)` reports exactly the canvas.
   With no canvas in the tree the old behaviour is kept (upstream's 1125x2436 fallback).
2. If the canvas is smaller than the panel, `present()` **scales the canvas up into the
   panel memory**: nearest neighbour, *inverse* mapping (panel pixel -> the canvas pixel
   that covers it), so every panel pixel is written and no column/row is left black.
   Verified numerically: canvas 1125x2436 -> panel 1440x3200 gives scale 1.2800 /
   1.3136, destination rect 0,0 1440x3200, 0 unwritten panel pixels, all 2436 canvas
   rows and 1125 canvas columns used, corners canvas(0,0)->panel(0,0) and
   canvas(1124,2435)->panel(1439,3199).
3. Boot-arg switches: `sweetfb_letterbox=1` draws the canvas 1:1 centered (the old
   path, black bars, no scaling), `sweetfb_native=1` presents the real panel size
   (1440x3200) as before. Both make the change reversible at runtime.

The patch was compile-checked locally (`tools/research/compile-one.sh
iokit/Kernel/arm/SweetFramebuffer.cpp`, 0 errors) and applies cleanly on top of
0103 (`git apply --check` in a scratch tree).

### 3.3 `shim/loader.c` / `shim/console.h`: **no change**

`FB_BASE` (0xe2a00000), `FB_W` (1440), `FB_H` (3200), `FB_STRIDE` (5760) live in
`shim/console.h` and describe the **panel**: the ABL splash memory that the panel scans
out 1:1. `shim/loader.c` copies them into `boot_args.Video`, which is what
`PE_current_console()` hands to the kernel, so the kernel's panel surface and the
kernel's text console stay 1440x3200.

If the shim published the *canvas* (1125x2436, stride 4500) instead, the kernel would
copy the frames with the wrong stride into a 1440x3200 panel (a sheared picture, and
the boot console would be garbage as well). **The panel geometry must stay in the shim;
the canvas must come from the ADT.** If a change were ever needed here, it would be in
`shim/console.h`, not in `loader.c`.

### 3.4 The touch driver: no change

`SweetHID` publishes normalized (0..1) digitizer coordinates: `x/1440`. With the canvas
filling the whole panel, the panel fraction is the canvas fraction, so the mapping stays
correct (a touch at the panel centre stays at 0.5, i.e. the centre of the 375x812 pt
screen).

---

## 4. Risks

1. **The ADT and the kernel patch are one unit.** The new ADT with a kernel older than
   0111 = the canvas says 1125x2436 while the kernel reports 1440x3200 = exactly the
   750x1334 failure = no swaps, black panel. Do not boot `shim/out/venus-v39.adt`
   with an old kernel. The regenerated ADT was written to `shim/out/venus-v39.adt`
   only; the existing payloads v39/v40 still carry the old ADT and were left alone on
   purpose (they would be a mismatched pair).
2. **CPU cost of the scale-up.** The scaled path writes the same 4.6 Mpx per presented
   frame as the old 1:1 memcpy, but with a per-pixel gather (~8-10 instructions/pixel)
   instead of `memcpy`: ~5 ms/frame at 3 GHz. At the default `sweetfb_hz=10` that is
   ~5 % of one core, at 30 Hz ~15 %. Against that, the CPU renderer draws 40 % fewer
   pixels, so the net effect should be positive. If it turns out to hurt, use
   `sweetfb_letterbox=1` (no scaling at all) or lower `sweetfb_hz`.
3. **Nearest neighbour.** A 1.28x/1.31x upscale duplicates pixels; no interpolation
   (bilinear would be ~4x the cost). The 2.6 % aspect difference stretches the picture
   vertically by 2.6 %.
4. **MobileGestalt cache.** Its answers are cached in
   `/private/var/containers/Shared/SystemGroup/systemgroup.com.apple.mobilegestaltcache/`
   (keyed by build + platform UUID, `NOTES.md` L432). If the scale does not change
   after the switch, that cache has to go (delete the file - or use a different
   synthetic ECID in the ADT).
5. **Identity vs geometry.** The ADT identity is iPhone10,1 (iPhone 8) while the
   presented screen is now the iPhone X geometry (375x812 pt). UIKit looks the status
   bar provider up by the *screen size*, so the presentation is an iPhone X screen; the
   model still comes from MobileGestalt (iPhone 8 artwork, device class phone). The
   clean long-term fix is the iPhone X userland with the iPhone X identity (what
   upstream sweet does), but the activation work depends on the current identity - do
   not touch it now.
6. **The panel is still 1440x3200.** Only what iOS *thinks* changed; the picture on the
   glass is a 1.28x/1.31x upscale of the 1125x2436 render.

---

## 5. How to test

1. Build the kernel with 0111 (CI), then
   `python3 tools/mkpayload.py <kernel> shim/out/venus-v39.adt shim/out/payload-venus-v41.bin --cmdline '<the usual venus cmdline>'`
   and `(cd shim && ./build.sh ../shim/out/payload-venus-v41.bin)`.
2. In the log expect:
   * `SWEET FB: AppleCLCD 1440x3200 stride 5760 at 0xe2a00000, default surface N, presented as 1125x2436 (scaled to the panel)`
   * `SWEET FB: swap N layer 0 surface M 1125x2436 ...` (the surface is canvas-sized now)
   * `SWEET FB: MobileGestaltHelper sel 8 in 0/0 out 2/0` -> (1125, 2436).
3. The picture must fill the whole panel (no bars, no shear) and the status bar must
   look like a normal iPhone status bar (no red `rdar://45025538` bar).
4. If there are **no** swap lines at all: the canvas and the kernel's presented size
   disagree again - compare the ADT's `canvas-width/height` with the "presented as ..."
   line in the log.
5. Fallbacks without a rebuild: `sweetfb_letterbox=1` (1:1 centered) and
   `sweetfb_native=1` (the old 1440x3200 @2x = 720x1600 pt presentation) are boot-args
   of the new kernel.

---

## 6. Still missing

* The identity/geometry mismatch (iPhone 8 identity, iPhone X screen) - separate work.
* A better scaler (bilinear / NEON) if the CPU cost shows up in the frame rate.
* The MobileGestalt cache question (risk 4) - only visible after a boot.
* `logs/research/24-upstream-backlog.md` item 14 should be marked SOLVED (pending a
  successful boot with the red-status-bar check).
