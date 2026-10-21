# 12 - SweetGraphicsAccelerator: a non-Apple-named IOKit accelerator service + the fps counter

Implementation of wave-1 items **7 / Stage A1** and **B3** of
`logs/research/07-gpu-plan.md` ("publish a non-Apple accelerator service",
"measure the frame rate") and of the hard rule in
`logs/research/18-iokit-gap-inventory.md` item 5.1:

> *"KEEP THE GAP. ... A service with these names moves CoreAnimation from the
> (working) CPU renderer onto the Metal path, where the closed `AGXMetalPlugin`
> will hang. ... If userland ever needs to *know* about the GPU, publish a
> service under a **different** class name (e.g. `SweetGPUInfo`) carrying the
> informational properties; never `IOAcceleratorES`/`AGXAccelerator`."*

* files: `xnu/patches/0114-venus-gpu-iokit-service.patch` (new, 778 lines),
  `xnu/patches/SweetGPU.cpp` (the standalone copy of the new source, same
  convention as `xnu/patches/sweet_file_patch.c` of patch 0100)
* kernel patch: **0114** (4 files: 1 new, 3 modified)
* boot args: **`sweetgpuiokit=1`** (new service, default OFF),
  **`sweetfbfps=1`** (fps counter, default OFF)
* no phone contact, no flash, no CI run spent

---

## 1. What the patch contains

| file | change |
| --- | --- |
| `iokit/Kernel/arm/SweetGPU.cpp` | **new**, 516 lines: the `SweetGraphicsAccelerator` service + its user client |
| `iokit/Kernel/arm/SweetFramebuffer.cpp` | the fps counter (7 hunks) |
| `iokit/conf/files.arm64` | `iokit/Kernel/arm/SweetGPU.cpp   optional iokitcpp` |
| `iokit/KernelConfigTables.cpp` | the personality: `IOClass = SweetGraphicsAccelerator`, `IOProviderClass = IOResources`, `IOMatchCategory = SweetGraphicsAccelerator` |

The personality is the same shape as the existing `AppleKeyStore`,
`SweetBattery` and `SweetHIDTouch` entries: the kernel attaches the class to
`IOResources` and starts it.  `IOServiceMatching("SweetGraphicsAccelerator")`
therefore resolves, and `IOServiceMatching("IOAcceleratorES")` still finds
nothing.

---

## 2. The class and the properties

### 2.1 The class

```
SweetGraphicsAccelerator            : IOService
SweetGraphicsAcceleratorUserClient  : IOUserClient
```

**Neither name contains an Apple accelerator name.**  This is the single most
important property of the whole patch:

* `IOServiceMatching("IOAcceleratorES")` builds the matching dictionary
  `{ "IOProviderClass" = "IOAcceleratorES" }` (that is verbatim what the live
  log shows, 5449 times per boot).  A service matches it only if its **class
  name** is `IOAcceleratorES` (the comparison is `metaCast()` on the class
  name, exact, no wildcards).  Our class is `SweetGraphicsAccelerator`, so
  the 5449 lookups keep finding nothing - exactly as today.
* the same holds for `IOAcceleratorESLegacy` (309x), `AGXAccelerator` (34x)
  and `IOAccelerator`.
* `IOServiceMatching` does **not** do a substring search, so the word
  "Accelerator" inside our name cannot be matched by an
  `IOProviderClass = "…Accelerator"` query either.

### 2.2 The properties we publish

| property | value | why it is safe |
| --- | --- | --- |
| `IOUserClientClass` | `SweetGraphicsAcceleratorUserClient` | the only way in; a user client of our own class, opened with type 0.  Nothing in the Metal path opens it, because nothing matches the accelerator class names. |
| `IOMaintainedDevice` | `true` | pure power-management bookkeeping ("this device is kept alive by the kernel").  `grep -rn IOMaintainedDevice iokit/` finds **no** reader in the kernel, and no Apple framework looks for it on an arbitrary service. |
| `external` | `false` | matches what `AppleCLCD` publishes; says "this is not an external/PCI device". |
| `SweetGraphicsAcceleratorVersion` | `1` | ours. |
| `SweetGraphicsAcceleratorVersionString` | `sweetgpu 1.0` | ours. |
| `SweetGraphicsAcceleratorDeviceName` | `Sweet Venus GPU (Adreno 660 / KGSL, software renderer)` | ours.  It says *software renderer* in the name on purpose: nobody should read this and think the Adreno is being used. |
| `SweetGraphicsAcceleratorChipID` | `0x06060001` | the Adreno 660 chip id (report 07 §2.1: `qcom,chipid = 0x06060001` = family 6.6.0.1 = upstream `ADRENO_6XX_GEN4`).  Read from `/arm-io/gpu` (`qcom,chipid`) when that device-tree node exists, otherwise the constant from the CAF documentation. |
| `SweetGraphicsAcceleratorMaxTextureSize` | `8192` | the limit of the **software** renderer, not of the Adreno.  It is the number a tool needs to size a surface the compositor accepts. |
| `SweetGraphicsAcceleratorMaxDimension` | `8192` | idem. |
| `SweetGraphicsAcceleratorPixelFormats` | `[ 'BGRA', 'ARGB', 'RGBA' ]` (numbers *and* strings) | the formats our IOSurface allocator really handles.  Claiming a format we cannot honour is exactly how the display-scaler stub of report 18 item 5.4 would produce wrong frames. |
| `SweetGraphicsAcceleratorCapabilities` | dictionary with the same values | convenience for a tool that reads properties instead of opening the client. |
| `SweetGraphicsAcceleratorRenderer` | `software (CoreAnimation CPU rasteriser)` | says out loud that this is not a GPU. |

### 2.3 The properties we deliberately do NOT publish, and why

This is the core of the patch; the full list is repeated as a comment above
`sweet_gpu_publish()` in the source.

| omitted property | why omitting it is the safe choice |
| --- | --- |
| **`IOGPUFamily`**, `AppleGPUFamily`, `AppleGPUFamilyN` | **This is the property that makes CoreAnimation/Metal select the GPU path.**  The Metal plug-in (`AGXMetalPlugin` / `AGXMetalG<gen>` inside the dyld shared cache) and `MTLCreateSystemDefaultDevice()` look for a service that carries a GPU *family* string; the family selects the command-stream format and the plug-in image.  Publishing `AppleGPUFamily8` (the A11/A12 family) would make the userland load a plug-in that then talks AGX to a driver that does not exist -> the plug-in blocks inside the window server -> **the whole UI freezes** (report 07 §4.3).  We publish no family at all, so the plug-in has nothing to bind to. |
| `MetalPluginName`, `AGXMetalPlugin`, `AGXMetalG*` | the names the plug-in probes for when it looks for its kernel counterpart. |
| `IOGPUCommandQueue`, `IOGPUCommandQueueName`, `IOGPUFamilyCapabilities` | the command queue is what the Metal renderer would submit to; a name without an object behind it is worse than nothing. |
| any key starting with `IOAccelerator`, `AGX` or `IOGPU` | accelerator identifiers that some framework may match on by property.  We use the `SweetGraphicsAccelerator…` prefix instead, which no Apple framework knows. |
| `IOPixelFormat`, `IOAccelDeviceProfile`, `IOAccelStatistics*` | accelerator-only properties; `IOSurfaceAccelerator` and the GPU statistics code look for them. |
| `HIDServiceSupport`, `PrimaryUsage`, `DeviceUsagePairs` | the iOS HID event system subscribes to `IOProviderClass = IOService` with `IOPropertyMatch{HIDServiceSupport=true}` (51 subscriptions per boot in the log).  Publishing it would make the HID stack subscribe to a graphics service. |
| `backlight-control` | that is the *match key* of the backlight service (report 18 item 5.3); if we claimed it, CoreBrightness / CoreMotion / powerlog would drive *our* service as if it were the backlight. |
| `IOSurfaceAcceleratorCapabilitiesDict` | belongs to the scaler service (`AppleM2ScalerCSCDriver`, report 18 item 5.4). |
| the **class name** `IOAcceleratorES` / `AGXAccelerator` | see §2.1: this one alone would freeze the UI.  It is the reason the class is called `SweetGraphicsAccelerator`. |

### 2.4 The user client

```
selector 0  kSweetGPUGetDeviceInfo    -> SweetGPUDeviceInfo (one structure)
selector 1  kSweetGPUGetVersion       -> the version string
selector 2  kSweetGPUGetPixelFormats  -> scalarOutput[0] = count, structureOutput = the fourcc list
selector 3  kSweetGPUGetMaxTexture    -> scalarOutput[0] = the maximum texture size
selector 4  kSweetGPUGetCapabilities  -> the capabilities as an XML property list
anything else                         -> kIOReturnUnsupported
```

```c
typedef struct {
	uint32_t magic;                  /* 'SGP1' */
	uint32_t structSize;
	uint32_t version;
	uint32_t maxTextureSize;         /* 8192  */
	uint32_t maxDimension;           /* 8192  */
	uint32_t formatCount;            /* 3     */
	uint32_t formats[8];             /* 'BGRA', 'ARGB', 'RGBA' */
	uint32_t chipId;                 /* 0x06060001 */
	uint32_t flags;
	char     versionString[32];      /* "sweetgpu 1.0" */
	char     deviceName[64];
} SweetGPUDeviceInfo;
```

Everything is answered **from memory**: no sleep, no I/O, no lock that could
be held by another thread.  That is the point of the "device info" call -
"the framework can query it without hanging".  Every call is logged
(`SWEET GPU: <proc> sel <n> -> 0x…`), like `SweetKeyStore` does, so the
trace shows who asked.

The output is written through `structureOutputDescriptor` when the caller
passed one, otherwise through `structureOutput` (same helper shape as
`AppleKeyStoreUserClient`).

### 2.5 What this service is **not**

It is **not** a GPU driver: no command stream, no ringbuffer, no register
access, the Adreno is not powered up.  The CX rail vote, the GPU_CC clocks
and the zap shader are Stage B of report 07 and do not exist yet (patch 0112
adds a *read-only* probe of the same hardware; it is independent of this
patch and touches no file of this patch).

---

## 3. The fps counter

### 3.1 What it prints

```
SWEET FB: fps=27 (swaps 138, presents 50, vsync 300, 5.020 s)
```

one line every **5 s**, only when the boot arg `sweetfbfps=1` is present
(default OFF, so the log stays quiet).

### 3.2 Why three numbers

| number | what it counts | what it means |
| --- | --- | --- |
| `fps=` | the `SwapEnd` calls from backboardd per second | **the frame rate of the UI** - i.e. how fast the CoreAnimation *software* rasteriser produces frames.  This is the number stage A1 of report 07 asks for. |
| `presents` | the frames actually copied into the panel per second | how fast the **screen** changes.  It is capped by `sweetfb_hz` (10 Hz by default in our tree, 30 Hz upstream), so it is *not* the renderer speed. |
| `vsync` | the vsync timer ticks per second | a sanity check on the timer itself (60 Hz nominal). |

The three are different numbers and reporting only one of them would be
misleading: with `sweetfb_hz=10` the panel shows 10 fps while the renderer
may well produce 30.

### 3.3 How it is measured

* the counters live in `AppleCLCD` (`fFpsSwaps`, `fFpsPresents`,
  `fFpsVsyncs`), the swap counter is incremented **under `fLock`** (the swap
  comes from the user-client thread, the rest from the vsync thread);
* the report is made from the vsync handler, in both exit paths (normal and
  "display off"), so the counter keeps working when the panel is off;
* the time base is `clock_get_uptime()` + `absolutetime_to_nanoseconds()`
  (kernel side, **no floating point**); the rate is integer arithmetic:
  `fps = swaps * 1000 / elapsed_ms`;
* the elapsed time is measured, not assumed from the tick count, so a
  delayed vsync timer shows up in the number instead of being hidden.

### 3.4 Interaction with the other framebuffer switches

| boot arg | effect on the numbers |
| --- | --- |
| `sweetfbfps=1` | the counter is on (this patch) |
| `sweetfb_hz=N` | caps `presents` to N Hz (default 10) |
| `sweetfb_force_on=1` | keeps the panel on; without it the "display off" path drops every swap and `presents` goes to 0 while `swaps` keeps counting |
| `sweetfb_noconsole=1` | the kernel console stops drawing over the GUI (no effect on the numbers) |

---

## 4. How to test

### 4.1 Build

```sh
# the patch is in the series, so the CI picks it up automatically:
#   .github/workflows/xnu.yml copies xnu/patches/*.patch into dxb/patches/
# local single-file check (no CI run needed):
cd <repo>
XNU_BT=$PWD/logs/bt3 tools/research/compile-one.sh \
        iokit/Kernel/arm/SweetGPU.cpp \
        iokit/Kernel/arm/SweetFramebuffer.cpp \
        iokit/KernelConfigTables.cpp
# -> "OK" for all three, no new warnings
```

The patch was verified with the CI-equivalent replay:

```sh
python3 tools/research/check-patch-order.py --no-stage
# -> "OK   0114-venus-gpu-iokit-service.patch"
#    "new source files: all registered in a conf/files list"
```

> Note: the patch must be generated against the tree **after** the other
> patches (the CI applies the series in file-name order and *skips* a patch
> that does not apply, with a warning - a silent skip means a silently
> missing feature).  `check-patch-order.py` is the tool that catches that.
> The failures the tool reports next to mine (`0012`, `0031`, `0041`,
> `0110`, `0119`, `0121`) are **not** mine - they are pre-existing or belong
> to other agents (`0119` and `0121` both add lines to `bsd/conf/files`).

### 4.2 Boot A - the service OFF (default, the comparison build)

```
sweetfbfps=1
```

expect in the log:

```
SWEET FB: sweetfbfps=1, the frame rate is reported every 5 s
SWEET GPU: sweetgpuiokit is not set, the service stays unpublished
SWEET FB: fps=NN (swaps ..., presents ..., vsync ..., 5.0xx s)      every 5 s
```

and **nothing else**: no `IOServiceMatching` for
`SweetGraphicsAccelerator` in the IOKit trace (patch 0049), no service in
`ioreg`.  The UI must behave exactly as before.

### 4.3 Boot B - the service ON

```
sweetgpuiokit=1 sweetfbfps=1
```

expect:

```
SWEET GPU: SweetGraphicsAccelerator published (chip 0x6060001 (hardcoded),
  max texture 8192, formats 3, renderer software; NO IOGPUFamily,
  NO IOAcceleratorES/AGXAccelerator class, so CoreAnimation stays on the
  CPU rasteriser)
```

and on the phone:

```sh
ioreg -c SweetGraphicsAccelerator -l          # the properties above
ioreg -c IOAcceleratorES -l                   # MUST stay empty
ioreg -c AGXAccelerator -l                    # MUST stay empty
```

The two `ioreg` lines are the safety test: **if either of them finds
something, the patch is broken** (it would mean we published a class the
Metal path can bind to).

### 4.4 The negative test that matters

With `sweetgpuiokit=1` on, the UI must still be on the **software**
renderer:

* the home screen must appear and animate,
* the `sweetgpuiokit=1` build must show the **same** `fps=` numbers as the
  build without it (the service does not touch the renderer),
* the log must still contain **no** `Metal`, `MTL`, `AGXMetal` or
  `IOGPUFamily` string, and no `IOServiceOpen` of an accelerator user
  client.

If the UI freezes with `sweetgpuiokit=1`, the service is NOT safe: check
the published property list against §2.3 of this report.

### 4.5 The user client

No test tool ships with this patch (the userland cannot be changed from the
kernel yet), but the client can be exercised from any process that can open
IOKit services - e.g. with a 20-line `IOServiceMatching` +
`IOServiceOpen(type 0)` + `IOConnectCallMethod(selector 0)` program, or
with the `ioreg`-style property read of §4.3, which needs no client at all.
The console log shows every call:

```
SWEET GPU: mytool sel 0 -> 0x0
```

### 4.6 Measuring the CPU renderer (the actual goal)

With `sweetfbfps=1` on a build that reaches the home screen:

| scenario | what to read |
| --- | --- |
| home screen, idle | `fps=` = the rate the compositor produces (upstream sweet: 20-30 fps) |
| home screen, animation | `fps=` should stay roughly the same |
| Settings list scrolling | `fps=` may drop; `presents` stays at `sweetfb_hz` |
| display off (`sweetfb_force_on=0`) | `swaps` keeps counting, `presents` goes to 0 |

The next step of report 07 is A2 (shrink the canvas to 1125x2436, already
done by patch 0111) and A3 (honour `sweetfb_hz=30`): the numbers above are
the baseline both are compared against.

---

## 5. Risk analysis

| risk | how it is excluded |
| --- | --- |
| CoreAnimation switches to Metal and the UI freezes | the class name is not an Apple accelerator name and no `IOGPUFamily` is published (§2.1, §2.3).  Test: `ioreg -c IOAcceleratorES` stays empty (§4.3). |
| the userland finds the service and hangs on it | the user client answers every selector from memory; unknown selectors return `kIOReturnUnsupported` immediately. |
| the service changes the power state of the phone | `IOMaintainedDevice` is not read by the kernel; the service attaches to `IOResources` and claims no power resources. |
| boot-time cost | one class instantiation and one `IOLog` line at boot when the boot arg is **off**; when it is on, one `IORegistryEntry::fromPath()` lookup for `/arm-io/gpu`. |
| it breaks the existing accelerator lookups | the 5449 + 309 + 34 lookups per boot keep returning "none", exactly as before (they match on the class name). |
| the patch does not apply in the CI | verified with `check-patch-order.py` (§4.1); the CI *skips* patches that do not apply, so this check is mandatory. |

---

## 6. Files

| path | role |
| --- | --- |
| `xnu/patches/0114-venus-gpu-iokit-service.patch` | the kernel patch (new source + fps counter + registration) |
| `xnu/patches/SweetGPU.cpp` | standalone copy of the new source, for the local single-file compile loop (same convention as `xnu/patches/sweet_file_patch.c`) |
| `logs/research/impl/12-gpu-iokit.md` | this report |

The compile checks were done in a scratch copy of the patched tree,
`logs/gpu3/xnu` (a copy of `logs/bt3/xnu` + the patch applied); it was
removed again afterwards, so the only lasting artefacts are the three files
above.  `logs/bt3/xnu` was **not** modified.

`xnu/patches/SweetGPU.cpp` is also the file `tools/research/compile-one.sh`
installs into a build tree when the source is new (`install_source()` looks
for `xnu/patches/<basename>` first) - note that it *writes* into the build
tree it is pointed at, so use a scratch copy (or `logs/gpu3`) and not
`logs/bt3/xnu`.
