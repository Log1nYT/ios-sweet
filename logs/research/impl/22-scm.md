# 22 - SCM (secure world) helper: the zap-auth gate for the Adreno 660

Implementation of the **Stage B/C gate** of `logs/research/07-gpu-plan.md` §3.5 and
§6 (B2 step 6): *"upstream does this with `qcom_scm_pas_auth_and_reset(pas_id = 0x0d)`
over the SCM fastcall ... Until this works the GPU stays in secure mode and the AHB
reads may return 0."*

This document is the one that decides whether the whole GPU effort is worth **months**
or **days**.  Short version: **the gate itself is cheap (the code is in this patch, the
experiment is one boot), but clearing it does not make the iOS UI faster** - the GPU
would still be useless to iOS without a Metal/AGX translator that does not exist.  See
§10 for the decision.

* files touched: `bsd/dev/sweet_scm.c` (new), `bsd/dev/sweet_scm.h` (new),
  `xnu/patches/0124-venus-scm-helper.patch` (new),
  `xnu/patches/sweet_scm.c` + `xnu/patches/sweet_scm.h` (copies of the new sources,
  for the local compiler and for the patch generator)
* shared files touched **only inside the patch**: `bsd/conf/files` (one line, keyword at
  column 32) and `bsd/kern/bsd_init.c` (one declaration + one call) - see §7.3
* no phone access, no flashing, no GPU register is touched, no IOKit service

---

## 1. The question this answers

The GPU of the Mi 11 is an **Adreno 660** behind Qualcomm's KGSL-class bindings
(`qcom,kgsl-3d0`, chip id `0x06060001`).  On a 6xx GPU the core is not driven by the
kernel at all: it runs under the **GMU** firmware, and the GMU only runs after the
**zap shader** has been *authenticated by the secure world*.  Until that happens the
GPU is in **secure mode**: the AHB aperture may read as zeros and nothing can be
commanded.

The authentication is a single SCM (secure computing manager) call:

```
SCM service PIL (0x02), command PAS_AUTH_AND_RESET (0x05), argument 0 = PAS id 0x0d
```

We had **no SCM driver at all** in the tree (`grep -ri "smc #0" logs/bt3/xnu` finds only
the two PSCI call sites, see §2).  This patch adds one, plus a probe that answers the
only question that matters: **does the secure world of this phone answer us at all, and
does it know the GPU?**

---

## 2. How the secure world is reached on venus

* The phone boots bare metal on the SM8350.  The **TrustZone (TZ)** runs the secure
  world; the normal world (our kernel) runs at **EL1, non-secure**.
* The SMC conduit is the `smc #0` instruction.  Our tree already uses it and it is
  **proven on this phone**:
  * `iokit/Kernel/arm/SweetCPU.cpp:32` - PSCI `CPU_ON` (`0xC4000003`), the secondaries
    really start (NOTES: "CPU_ON works for all 7 secondaries");
  * `bsd/dev/sweet_reboot.c:335` - PSCI `SYSTEM_RESET` (`0xC4000009`), the warm reboot.
* So the *conduit* is not the problem.  The question is whether the **SCM service**
  (a different owner in the same SMC dispatcher) is enabled in this phone's TZ and
  whether it accepts calls from the non-secure EL1.
* The device tree says it should: venus.dts:327 has a

  ```
  qcom,scm {
          compatible = "qcom,scm";
          qcom,dload-mode = <0x02 0x13000>;
  };
  ```

  node - upstream Linux binds exactly this node to its `qcom_scm` driver
  (`drivers/firmware/qcom/qcom_scm.c`, `of_match_table` = `"qcom,scm"`).  The DT is
  therefore *asking* for an SCM client, and Android has one (CAF
  `drivers/soc/qcom/scm.c`).
* The PSCI function ids live in a different part of the id space (owner "Standard
  Service", `0xC4xxxxxx`) than the SCM ids (owner "SIP", `0x02xxxxxx`), so an SCM call
  can never be mistaken for a PSCI call and vice versa.

---

## 3. The calling convention (and where every bit comes from)

### 3.1 The layout

The function id in x0 follows the **ARM SMC calling convention** (Linux
`include/linux/arm-smccc.h`, current master):

```
x0 = [31]      type              0 = standard call, 1 = fast (atomic) call
    [30]      calling convention 0 = 32 bit arguments, 1 = 64 bit arguments
    [29:24]   owner              2 = "SIP" (the owner of the Qualcomm SCM services)
    [15:0]    function id        (service << 8) | command
```

Constants (Linux `include/linux/arm-smccc.h`):

| constant | value |
|---|---|
| `ARM_SMCCC_STD_CALL` / `ARM_SMCCC_FAST_CALL` | 0 / 1 |
| `ARM_SMCCC_TYPE_SHIFT` | 31 |
| `ARM_SMCCC_SMC_32` / `ARM_SMCCC_SMC_64` | 0 / 1 |
| `ARM_SMCCC_CALL_CONV_SHIFT` | 30 |
| `ARM_SMCCC_OWNER_MASK` / `ARM_SMCCC_OWNER_SHIFT` | 0x3F / 24 |
| `ARM_SMCCC_FUNC_MASK` | 0xFFFF |
| `ARM_SMCCC_OWNER_SIP` / `ARM_SMCCC_OWNER_TRUSTED_OS` | 2 / 50 |

The register layout of the call (Linux `drivers/firmware/qcom/qcom_scm-smc.c`,
`__scm_smc_call()`):

```
x0 = ARM_SMCCC_CALL_VAL(type, convention, owner, fnid)
x1 = desc.arginfo        (number of arguments, 2 bits of type per argument)
x2..x5 = args[0..3]      (more than four arguments need a DMA buffer)
answer: x0 = error code (0 = ok, negative = SCM error, 1 = "interrupted")
        x1..x3 = up to three results
```

### 3.2 The same thing under the CAF name - and why they are the same

CAF (Qualcomm's own kernel, `include/soc/qcom/scm.h` and
`drivers/soc/qcom/scm.c`, android-7.1.0_r0.2) spells the same thing differently:

```c
#define SCM_SIP_FNID(s, c)  (((((s) & 0xFF) << 8) | ((c) & 0xFF)) | 0x02000000)
#define SMC64_MASK          0x40000000
#define SMC_ATOMIC_SYSCALL  31        /* -> 0x80000000 */
```

Putting the two together:

| encoding | x0 | equals |
|---|---|---|
| CAF `SCM_SIP_FNID` + `SMC64_MASK` | `0x42000000 \| fnid` | SMCCC standard call, SMC64, owner SIP |
| CAF `SCM_SIP_FNID` | `0x02000000 \| fnid` | SMCCC standard call, SMC32, owner SIP |
| CAF + `SMC_ATOMIC_MASK` (+SMC64) | `0xc2000000 \| fnid` | SMCCC fast call, SMC64, owner SIP |

**The "Qualcomm vendor extension id 0x02000000" of the brief is the SMCCC owner
field** (owner 2 = "SIP", at bit 24) - not a flag of its own.  The CAF spelling and
the modern upstream spelling produce *bit identical* function ids, which is why the
same TrustZone answers both and why this helper only needs one family of encodings.
The header (`sweet_scm.h`) defines all four x0 values explicitly.

### 3.3 The argument of IS_CALL_AVAIL

The convention probe asks the secure world whether a call exists:

```
service INFO (0x06), command IS_CALL_AVAIL (0x01)
x1 = 1 (one argument)
x2 = the function id that is asked for, in the vendor spelling
     (0x02000000 | fnid)
answer: x0 = 0 (no error) and x1 = 1 (the call exists)
```

Linux upstream does the same (`qcom_scm.c`, `__get_convention()`:
`desc.args[0] = SCM_SMC_FNID(svc, cmd) | (ARM_SMCCC_OWNER_SIP << ARM_SMCCC_OWNER_SHIFT)`),
CAF does the same (`scm.c`, `is_scm_armv8()`).  Upstream tries the 64 bit form first,
then the 32 bit form, then the old buffer form; this helper tries the same order
(§7.2).

### 3.4 The "legacy" convention (not implemented here)

Pre-ARMv8 firmwares want a **DMA command buffer**: x0 = 1, x1 = &context_id, x2 =
physical address of a page aligned, cache clean buffer that holds
`{len, buf_offset, resp_hdr_offset, id = (svc << 10) | cmd, args...}` and the answer
comes back in the same buffer (Linux `drivers/firmware/qcom/qcom_scm-legacy.c`, CAF
`scm_call_common()`).  It is **not** implemented: it needs a physically contiguous,
cache-clean buffer that we would have to allocate and flush by hand.  It is also very
unlikely to be needed: a TrustZone that answers the modern form is a TrustZone that
was built after 2016, and SM8350 is from 2020.  If the probe of §8 finds nothing, this
is the first thing to add.

---

## 4. Service and command ids

### 4.1 Services (`svc`)

CAF `include/soc/qcom/scm.h` == Linux `drivers/firmware/qcom/qcom_scm.h`:

| id | service | used here |
|---|---|---|
| 0x01 | BOOT | no |
| 0x02 | **PIL** (peripheral / image loader) | **yes** (PAS) |
| 0x05 | IO (secure register read/write) | no |
| 0x06 | **INFO** | **yes** (version, secure state, call availability) |
| 0x0c | MP (memory protection) | no |

### 4.2 Commands

| id | service | command | source | read only? |
|---|---|---|---|---|
| 0x01 | INFO | IS_CALL_AVAIL | Linux `qcom_scm.h` `QCOM_SCM_INFO_IS_CALL_AVAIL`; CAF `scm.c` `IS_CALL_AVAIL_CMD` | yes |
| 0x04 | INFO | GET_SECURE_STATE | CAF `scm.c` `TZ_INFO_GET_SECURE_STATE` (used by `scm_is_secure_device()`) | yes |
| 0x01 | PIL | PAS_INIT_IMAGE | CAF `subsys-pil-tz.c` `PAS_INIT_IMAGE_CMD`; Linux `QCOM_SCM_PIL_PAS_INIT_IMAGE` | **no** (programs the PAS region) |
| 0x02 | PIL | PAS_MEM_SETUP | CAF `PAS_MEM_SETUP_CMD`; Linux `QCOM_SCM_PIL_PAS_MEM_SETUP` | **no** (changes memory protection) |
| 0x05 | PIL | **PAS_AUTH_AND_RESET** | CAF `PAS_AUTH_AND_RESET_CMD`; Linux `QCOM_SCM_PIL_PAS_AUTH_AND_RESET` | **no** (authenticates + resets) |
| 0x06 | PIL | PAS_SHUTDOWN | CAF `PAS_SHUTDOWN_CMD`; Linux `QCOM_SCM_PIL_PAS_SHUTDOWN` | **no** (kills the peripheral) |
| 0x07 | PIL | PAS_IS_SUPPORTED | Linux `QCOM_SCM_PIL_PAS_IS_SUPPORTED` | yes |

The PAS command ids are the same in CAF and in current upstream Linux - that is not a
coincidence, CAF's `enum scm_cmd` was upstreamed.

### 4.3 The PAS ids of venus (from the live device tree)

Extracted from `/Users/matviy/Documents/Mu-Silicium/Resources/DTBs/venus.dts` (the
checked-in copy of the phone's live DT), all nodes are `qcom,pil-tz-generic`:

| PAS id | node | firmware | memory-region |
|---|---|---|---|
| 0x01 | `qcom,lpass@17300000` | adsp | 0xbc |
| 0x04 | `qcom,mss@8a800000` | modem | 0xb7 |
| 0x09 | `qcom,venus@aab0000` | venus | 0xc5 |
| 0x0c | `qcom,ssc@5c00000` | slpi | 0xcb |
| **0x0d** | **`qcom,kgsl-hyp` (`pil_gpu`)** | **a660_zap** | **0x296 = `pil_gpu_region` @ 0x8e71a000, 8 KiB** |
| 0x0e | `qcom,spss@1880000` | spss | 0xce |
| 0x0f | `qcom,ipa_fws` | ipa_fws | 0xc6 |
| 0x12 | `qcom,turing@0x98900000` | cdsp | 0xbf |
| 0x1a | `qcom,evass@abb0000` | evass | 0xc9/0xca |
| 0x1c | `qcom,trustedvm@d0800000` | trustedvm / slpi | - |

(The CAF `enum pas_id` in `subsys-pil-tz.c` - MODEM 0, Q6 1, DSPS 2, TZAPPS 3,
MODEM_SW 4, MODEM_FW 5, WCNSS 6, SECAPP 7, GSS 8, VIDC 9, VPU 10, BCSS 11 - is from an
older SoC generation and does **not** match these values; the DT is authoritative.)

**0x0d is the GPU.**  That is the id upstream msm passes to
`qcom_scm_pas_auth_and_reset()`.

---

## 5. Read only vs destructive - the safety rule of this file

| call | effect | how it is reached in this patch |
|---|---|---|
| IS_CALL_AVAIL (INFO 0x01) | none | used by the convention probe |
| GET_SECURE_STATE (INFO 0x04) | none | the default probe |
| PAS_IS_SUPPORTED (PIL 0x07) | none | the PAS probe |
| PAS_AUTH_AND_RESET (PIL 0x05) | **authenticates the firmware and RESETS the peripheral** | **only** behind `sweetscmauth=1` |
| PAS_INIT_IMAGE (PIL 0x01) | programs the PAS region | not exposed |
| PAS_MEM_SETUP (PIL 0x02) | changes the memory protection of the region | not exposed |
| PAS_SHUTDOWN (PIL 0x06) | shuts the peripheral down | not exposed |
| BOOT.* (0x01) | CPU boot addresses, power down | not exposed |
| IO.READ / IO.WRITE (0x05) | secure register access | not exposed |

The probe path contains **no** destructive call, and the destructive one is not even
reachable without an explicit boot argument.  The helper also never touches a GPU
register, a clock gate or a power domain.

---

## 6. What was implemented

`bsd/dev/sweet_scm.c` (547 lines) + `bsd/dev/sweet_scm.h` (146 lines):

```c
int sweet_scm_raw(uint64_t fid, uint64_t a1..a5, struct sweet_scm_res *res);
int sweet_scm_call(uint32_t svc, uint32_t cmd, const uint64_t *args,
                   unsigned nargs, struct sweet_scm_res *res);
int sweet_scm_pas_supported(uint32_t pas_id, bool *supported);
int sweet_scm_secure_state(uint32_t *state);
int sweet_scm_pas_auth_and_reset(uint32_t pas_id, struct sweet_scm_res *res);
const char *sweet_scm_convention(void);
int sweet_scm_init(void);
```

* `sweet_scm_raw()` is the `smc #0` itself.  x0..x5 are passed as read/write operands
  (the SMC convention lets the callee clobber x0..x5), x6..x17 and the flags are
  clobbers, `memory` is a clobber.  The answer is read back from x0..x3.
* `sweet_scm_call()` uses the convention that was detected, retries up to 8 times when
  the secure world answers "interrupted" (`SCM_INTERRUPTED` = 1), and refuses more than
  four arguments (that would need a DMA buffer).
* The detection order is `smc64` -> `smc32` -> `fast64` -> `fast32`, i.e. the same order
  as Linux upstream (`ARM_64`, `ARM_32`, legacy) and CAF (`is_scm_armv8()`).

Verified locally:

* `tools/research/compile-one.sh bsd/dev/sweet_scm.c` - **clean**, no warnings (the
  arm64 object contains the `smc #0` and the x0..x5 setup; the only undefined symbols
  are `_PE_parse_boot_argn`, `_kernel_thread_start`, `_printf`, `_thread_deallocate`);
* a host test of the encoding (`clang -I xnu/patches`, header only) checks the ten
  function ids against the values derived from the sources: all OK, e.g.
  `X0_SMC64(PIL, AUTH_AND_RESET) = 0x42000205`;
* `tools/research/check-patch-order.py --no-stage`: **0124 applies** in the full
  series and the new sources are registered in a `conf/files` list.

---

## 7. How it is wired and how to switch it on

### 7.1 Boot arguments

| argument | default | meaning |
|---|---|---|
| `sweetscm=1` | off | run the probe (in a kernel thread) |
| `sweetscmfn=<hex>` | - | probe this function id instead of the default one; the value is `(svc << 8) \| cmd`, e.g. `0x0604` = INFO / GET_SECURE_STATE.  The spelling with the `0x02000000` prefix is accepted too |
| `sweetscmpas=1` | off | read only PAS probe for PAS id 0x0d |
| `sweetscmauth=1` | off | **destructive**: really call PAS_AUTH_AND_RESET for PAS 0x0d |

### 7.2 What the probe prints

```
SWEET SCM: probe thread started
SWEET SCM: probe start (read only unless sweetscmauth=1)
SWEET SCM: convention smc64, is-call-avail a0=0 a1=1
SWEET SCM: secure-state a0=0 a1=0x00000003 a2=0x0 a3=0x0
SWEET SCM: secure-state bits: boot=1 debug=1
SWEET SCM: secure-state raw=0x00000003
SWEET SCM: pas auth-and-reset available=1
SWEET SCM: pas is-supported available=1
SWEET SCM: pas id 0x0d supported=1
SWEET SCM: probe done, convention smc64
```

If nothing answers:

```
SWEET SCM: convention smc64 answered a0=-2
SWEET SCM: convention smc32 answered a0=-2
...
SWEET SCM: no convention answered - the secure world is silent
```

### 7.3 The init hook

`bsd/kern/bsd_init.c` gets one declaration and one call at the end of `bsd_init()`:

```c
#if defined(ARM64_BOARD_CONFIG_SWEET)
extern void sweet_scm_init(void);       /* SWEET: secure world (SCM) probe */
#endif
...
	sweet_scm_init();
```

`bsd_init.c` was chosen on purpose: it is an upstream file that no other agent
touches, and `bsd_init()` runs with the scheduler up, so the probe can be started as a
real kernel thread.  The SCM call itself is **never** made from `bsd_init()` - an SCM
call may block (the secure world can put the caller to sleep), so it runs in its own
thread (`kernel_thread_start()`).

`bsd/conf/files` gets one line, at the end of the `bsd/dev` block, with the keyword at
**column 32**:

```
bsd/dev/sweet_scm.c            standard
```

The line is placed after `bsd/dev/unix_startup.c` on purpose: the other GPU patches
(0112/0113/0122) all insert their lines in the middle of the `sweet_*` block, and a
hunk there would conflict with them.  This hunk has the two preceding lines as
context, so it survives those insertions.

---

## 8. The exact probe to run on the phone

```sh
# 1. build a kernel with patch 0124 (the CI applies the series automatically)
# 2. put the arguments on the command line of the temporary image
tools/mkpayload.py xnu/out-XXXX/kernel.development.sweet \
        tools/mkadt_venus.py-out.adt shim/out/payload-scm.bin \
        --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam \
                    sweetscm=1 sweetscmpas=1'

# 3. temporary boot, never flash
fastboot boot shim/out/shim-boot-XXXX.img

# 4. read the console (the phone has no serial port; the log comes over USB ECM
#    or from the persistent log area, see tools/sweet-log-pull.sh)
grep 'SWEET SCM' <the log>
```

**Do not** add `sweetscmauth=1` to that first run.  It is the only argument that can
change the machine (it resets the GPU), and there is no reason to call it before the
read only probe has answered.

### Decision tree

| probe result | meaning | what it means for the GPU effort |
|---|---|---|
| `convention smc64, a1=1` + `pas id 0x0d supported=1` | the SCM answers and knows the GPU | the zap-auth gate **can** be cleared; the zap load is ~2-3 days of work (§10.2) |
| `convention ...` but `pas id 0x0d supported=0` | the SCM answers but does not know PAS 0x0d | the GPU cannot be authenticated from the normal world: Stage B/C is dead on this device |
| `no convention answered` | the SCM does not answer the normal world | same as above, and the legacy buffer form (§3.4) would have to be tried first |
| no `SWEET SCM:` line at all | the patch is not in the kernel | the CI skipped the patch - check the build log |

---

## 9. Risks

* **The probe itself**: read only, one SMC call at a time, no loops that can spin (at
  most 8 retries on "interrupted"), no timeout needed because the SCM answers
  synchronously or not at all.
* **An unknown x0 must not hang the machine.**  The TZ's SMC dispatcher answers
  unknown function ids with an error; it does not trap.  The worst case is an error
  code in the log.
* **`sweetscmauth=1`**: this is the one dangerous switch.  PAS_AUTH_AND_RESET resets
  the peripheral it authenticates.  With an empty carve-out the secure world will
  refuse (and the GPU stays in secure mode); with a valid `a660_zap` image it will
  release the GPU.  It cannot brick the SoC, but it does reset the GPU, so it is
  never issued by the read only path.
* **The SCM call must run in a preemptible context** - that is why it runs in a
  kernel thread and not in `bsd_init()`.
* **Boot cost**: one thread, a handful of SMC calls, no timers, no memory.  Nothing
  runs at all unless `sweetscm=1` is given.

---

## 10. The conclusion: is a KGSL-class GPU driver reachable on venus?

### 10.1 The gate is real, but it is *not* the biggest problem

The zap-auth is a hard gate for *driving* the GPU: without it the GPU stays in secure
mode.  But it is **not** the gate that decides the project, because:

* Even with a perfectly working GPU, **iOS cannot use it**.  §4 of the GPU plan is
  unambiguous: the userland polls for `IOAcceleratorES`/`AGXAccelerator`, finds
  nothing, and CoreAnimation renders in software.  Metal speaks Apple's closed AGX
  command stream; there is no translator from AGX to Adreno PM4, and the AGX/Metal
  plug-in is closed.  A KGSL-class kernel driver would give us a GPU that *we* could
  talk to, through DRM and (months later) Mesa Turnip - not a GPU that iOS could
  talk to.
* Therefore the SCM work is **necessary but not sufficient**.  Clearing it converts
  "Stage B/C is impossible" into "Stage C3 (DRM + Mesa + a renderer interposer) is
  a multi-year project with no user-visible benefit for stock apps".

### 10.2 If the SCM answers (the good case)

What the zap-auth actually needs, and what it costs:

1. **The firmware**: `a660_zap.bin` from linux-firmware (`qcom/a660_zap.bin`; CAF
   uses the same container under the name `a660_zap.mdt`).  It is an MDT container
   with a header and segments - upstream msm parses it itself
   (`adreno_zap_shader_load_mdt()`).
2. **The copy**: the segments have to be written into the 8 KiB carve-out
   `pil_gpu_region` at **0x8e71a000**, as *device* memory (the Qualcomm XPUs forbid
   cacheable mappings of carve-outs), which in our tree means `ml_io_map()` and a
   non-cached copy.
3. **The call**: `PAS_AUTH_AND_RESET` with PAS id 0x0d - which this patch provides.

That is upstream's `adreno_zap_shader_load_mdt()` plus one SCM call: **two to three
days**, not months.

**But**: on Android this work is done by the CAF *secure firmware loader* during the
normal boot flow (the `qcom,pil-tz-generic` node with `qcom,firmware-name` is read by
the bootloader).  With `fastboot boot` the ABL boots our kernel directly and that
loader never runs, so the carve-out is most likely **empty** - we would have to do
steps 1-3 ourselves, exactly like upstream Linux does.  This is the one unknown that
the PAS probe cannot answer (the carve-out is not readable from the normal world);
the AUTH_AND_RESET answer will tell us.

### 10.3 If the SCM does not answer (the bad case)

Then the GPU cannot be released from secure mode outside the Android boot flow, and
Stage B/C of the GPU plan is dead on this device.  What remains is Stage A (make the
CPU renderer fast: measure the fps, shrink the canvas to 1125x2436, honour
`sweetfb_hz`) - which is what the iOS UI actually needs - plus the Stage D idea
(hardware scaling in the display pipeline).  **That is the honest end state of the GPU
effort for iOS.**

### 10.4 What is therefore worth doing

| work | cost | benefit for iOS |
|---|---|---|
| this SCM helper + one probe boot | done / 1 boot | none directly, but it *decides* the GPU project |
| zap load + auth (if the probe is positive) | 2-3 days | none for iOS (no Metal); it only makes the GPU registers readable for Stage B |
| Stage B (CX rail, clocks, chip id read) | 1-2 weeks | none for iOS |
| Stage C3 (DRM/msm port + Mesa + renderer interposer) | 6-18 months, no precedent | apps still get no Metal device; only our own renderer would benefit |

**Recommendation**: run the probe (one boot, no risk).  If it is positive, do the zap
load - it is cheap and it tells us whether the AHB aperture is readable at all, which
is the last unknown of Stage B - but do **not** start the DRM/Mesa port on the strength
of it: that project only pays off if the goal is a GPU for *our own* userspace, not
for the iOS UI.  If the probe is negative, close the GPU plan for iOS and spend the
time on Stage A (fps) and the display pipeline instead.

---

## 11. What is still missing

* the legacy (DMA buffer) convention, if the probe finds nothing (§3.4);
* an SCM call to read the **SCM version** (`INFO` service, command `SCM_INFO_VERSION`)
  - the command id was not verified in this session, so the probe uses the
  secure-boot state instead;
* the zap loader itself (MDT parse + copy into 0x8e71a000) - not part of this patch;
* a way to load `a660_zap.bin` into the image (it is not in our tree yet);
* the `qcom,scm` DT node is not in *our* ADT (`tools/mkadt_venus.py`); it does not
  matter for the probe (the ids are compiled in), but a real driver should read it
  from the DT instead of hard-coding the addresses.

---

## 12. Sources

Read in this session (2026-10-21), all fetched over the network:

| what | where |
|---|---|
| SMCCC layout and constants | `include/linux/arm-smccc.h` (Linux master) |
| SCM service/command ids, `SCM_SMC_FNID`, `SCM_LEGACY_FNID` | `drivers/firmware/qcom/qcom_scm.h` (Linux master) |
| the 64 bit call, the argument layout, the convention probe | `drivers/firmware/qcom/qcom_scm-smc.c`, `drivers/firmware/qcom/qcom_scm.c` (Linux master) |
| the legacy buffer convention | `drivers/firmware/qcom/qcom_scm-legacy.c` (Linux master) |
| CAF spelling: `SCM_SIP_FNID`, `SMC64_MASK`, `SCM_ATOMIC_SYSCALL`, `scm_call2()`, `TZ_INFO_GET_SECURE_STATE`, `is_scm_armv8()` | `include/soc/qcom/scm.h`, `drivers/soc/qcom/scm.c` (android.googlesource.com/kernel/msm, android-7.1.0_r0.2) |
| CAF PAS command ids and the PAS id enum | `drivers/soc/qcom/subsys-pil-tz.c` (same tree) |
| the phone's own facts: `qcom,scm` node, `pil_gpu` node, PAS ids, the carve-out | `venus.dts` (the live DT) |
| what upstream msm does with the zap shader | `drivers/gpu/drm/msm/adreno/adreno_gpu.c` (Linux master) |

Local reproduction:

```sh
# compile the new file with the CI flags
XNU_BT=$PWD/logs/gpu7 XNU_OBJDIR=$PWD/logs/gpu7/objs \
        tools/research/compile-one.sh -o bsd/dev/sweet_scm.c

# the patch applies in the whole series
python3 tools/research/check-patch-order.py --no-stage
```
