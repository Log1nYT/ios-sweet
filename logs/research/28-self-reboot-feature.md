# 28 - The phone reboots itself into fastboot (patch 0102)

**State: patch `xnu/patches/0102-venus-self-reboot-fastboot.patch` is written and compiles
locally (bsd + osfmk, CI flags).  It is NOT built into a kernel yet and it was NOT tested on
the phone (no flashing, no rebooting the phone).**

## 1. The problem

A panic in our kernel leaves the phone in the panic screen: `PEHaltRestart()` returns without
doing anything on this board (`IOPlatformExpert::haltRestart()` only calls `PE_halt_restart`,
which is NULL for the SWEET platform), `kdp_machine_reboot_type()` then calls
`halt_all_cpus(TRUE)` -> `PEHaltRestart(kPERestartCPU)` -> nothing -> the CPU spins in
`while (1)`.  Until now only a human power-cycle (Vol- + Power) recovered it, and the
Mac-side supervisor (`tools/venus-supervisor.sh`, written by another agent) could not do
anything about it.

Goal: the kernel resets the phone into the **Qualcomm fastboot** by itself, so that the
supervisor sees `fastboot devices` and boots the newest iOS image again.

## 2. The mechanism (what the bootloader actually looks at)

### 2.1 The ABL "reboot reason" cookie

The Xiaomi/Qualcomm bootloader on this phone is the EDK2 **QcomModulePkg** (ABL).  Its
`LinuxLoader` entry point does (public ABL sources, e.g. CodeLinaro `abl/tianocore/edk2`,
`QcomModulePkg/Library/BootLib/QcomBootLib.c` + `Application/LinuxLoader/LinuxLoader.c`):

```c
  Status = GetRebootReason (&BootReason);
  switch (BootReason) {
  case FASTBOOT_MODE:   BootIntoFastboot = TRUE; break;
  case RECOVERY_MODE:   BootIntoRecovery = TRUE; break;
  case ALARM_BOOT:      BootReasonAlarm  = TRUE; break;
  case DM_VERITY_ENFORCING: ...
  default:              /* "Boot reason: 0x%x not handled, defaulting to Normal Boot" */
  }
  ...
  if (!BootIntoFastboot) { LoadImageAndAuth(...); BootLinux(...); }
fastboot:
  DEBUG((EFI_D_INFO, "Launching fastboot\n"));
  FastbootInitialize();
```

The enum is `NORMAL_MODE = 0, FASTBOOT_MODE = 1, RECOVERY_MODE = 2, ...` - i.e. **the fastboot
magic is 0x1 in the "reboot reason"** (the task's hint is right).

### 2.2 Where the reason lives on venus (SM8350 / PMK8350)

Linux writes that reason through the **PMIC**, and the venus vendor device tree (extracted
from `shim/out/kernel_blob.bin`, the DTB at offset 7261) says exactly where:

```dts
  reboot_reason {
          compatible = "qcom,reboot-reason";
          nvmem-cells = <0x584>;
          nvmem-cell-names = "restart_reason";
  };
  ...
  sdam@7100 {                       /* inside the PMK8350 node */
          compatible = "qcom,spmi-sdam";
          reg = <0x7100>;
          restart@48 {
                  reg = <0x48 0x01>;
                  bits = <0x01 0x07>;     /* the reason lives in bits [7:1] */
                  phandle = <0x584>;     /* <- the cell used above */
          };
  };
```

So on venus the "restart reason" is **one byte in the PMK8350's SDAM block: SPMI peripheral
0x71, register 0x48**, and the reason code sits in bits [7:1] of that byte.

The value for "boot into the bootloader" is **0x02** (the byte): Linux' `reboot bootloader`
writes the magic `1` shifted left by one bit into bits [7:1]:

* mainline `drivers/power/reset/qcom-pon.c`: `PON_SOFT_RB_SPARE 0x8f`, `GEN2_REASON_SHIFT 1`
  for `qcom,pmk8350-pon`, `regmap_update_bits(..., GENMASK(7, 1), magic << 1)` with
  `REBOOT_MODE_BOOTLOADER = 1` -> the byte 0x02;
* the CAF driver of this very platform (`drivers/power/reset/qcom-reboot-reason.c`, the
  `qcom,reboot-reason` node we have in the DT) writes `pon_reason = 0x02` for
  `"bootloader"` (its table: recovery 0x01, bootloader 0x02, rtc 0x03, ...).

`fastboot` on this phone is therefore reached the same way Android's `adb reboot bootloader`
reaches it.

### 2.3 The reset itself

A **warm** reset is needed, so that the PMIC (always-on) keeps the cookie.  PSCI
`SYSTEM_RESET` (SMCCC `0xC4000009`, x1 = 0) is exactly that on QCOM: the SoC restarts, the
PMIC stays powered, the bootloader comes up and reads the reason.  Linux does the same
(`qpnp_pon_system_pwr_off(PON_POWER_OFF_WARM_RESET)` after setting the reason).

### 2.4 What was rejected

| idea | why not |
|---|---|
| the "qcom,msm-imem" `restart_reason@65c` cookie (`0x77665500`, the LK era magic) | the base address `0x146bf000` is not a documented kalama region; an unmapped MMIO write is a synchronous abort (panic) |
| `scm_set_dload_mode()` / EDL | that is EDL (firehose, 9008), **not** fastboot; the supervisor looks for `fastboot devices` |
| an RPMh "reboot reason" register | there is none for this purpose; RPMh carries regulator votes (our `sweet_rpmh.c`) |
| writing the "bootloader_message" BCB in the `misc` partition | would need a raw partition writer in the kernel; far more work than the PMIC byte |

## 3. What the patch does

`bsd/dev/sweet_reboot.c` (new, ~530 lines):

* `sr_spmi_init()` - maps the SPMI arbiter/observer windows (the same ones `sweet_keys.c`
  uses: arbiter `0x0c440000`, observers `0x0e600000`, config `0x0c40a000`), finds the APID of
  peripheral 0x71 and **reads** the restart reason byte (the log line
  `SWEET REBOOT: SPMI arb=... sdam-apid=... owner=... reason=...` tells us what the last boot
  left and who owns the APID).
* `sr_spmi_write()` - writes one byte into that register and **verifies it by reading it
  back**.  A write does not go through the observer window (that one is for reads) but through
  the arbiter's *read/write channel* window (`0x0c600000`, one 64 KiB window per APID), with
  the command type in bits [29:27] (1 = read, 2 = write) and the data byte in the WDATA
  register of the window - the same encoding that the parallel battery patch 0104 uses for
  its SPMI writes.  This driver is self-contained (it does not depend on 0104), but the
  encodings agree.  Five candidate encodings are selectable with `sweetrebootcmd=0..4` in
  case the first one is rejected; a rejected command changes nothing in the PMIC (the status
  register reports the error) and the driver logs it.  A write is refused when the APID is
  not owned by the APPS (another master would race us).
* `sr_set_fastboot_cookie()` - read-modify-write of the reason byte (bit 0 belongs to the
  PMIC and is preserved): `(old & 1) | 0x02`.
* `sr_psci_reset()` - the PSCI `SYSTEM_RESET` SMC.
* `sr_reboot(request, why)` - writes the cookie (for a fastboot request), waits 300 ms so the
  USB console log can flush, then resets.  Never returns (it spins if the firmware ignores
  the reset).
* the **watchdog** thread (`sweetreboot=<seconds>`), the **magic UDP** listener (port 5141),
  the **sysctl** `kern.sweet_reboot` and the init (`sweet_reboot_init()`, called from
  `sweet_ufs_init()`).

Hooks:

* `osfmk/kern/debug.c`
  * `panic_trap_to_debugger()` copies the panic text into the USB log
    (`sweet_reboot_panic_log()`) - the panic text normally only reaches the framebuffer
    console, and the phone is about to reset;
  * `kdp_machine_reboot_type()` calls `sweet_reboot_panic()` - this is the normal end of the
    panic path (the panic log has been written at this point);
  * the `panic_spin_forever()` fallback also calls it, for the `panicDebugging` case.
* `osfmk/arm64/machine_routines.c`: `halt_all_cpus(TRUE)` (the normal reboot request,
  `host_reboot()`) calls `sweet_reboot_request()` instead of hanging in `PEHaltRestart()`.

## 4. Usage

```sh
# what the kernel prints at every boot
#   SWEET REBOOT: reason=0x2 cmd=0 panic=1 udp=1 watchdog=0s mode=fastboot
#   SWEET REBOOT: SPMI arb=0x... apids=... sdam-apid=... reason=0x.. (readable)
#   SWEET REBOOT: listening on UDP port 5141

# reboot into fastboot right now (from a shell on the phone)
sysctl -w kern.sweet_reboot=1        # 1 = fastboot, 2 = plain reboot
# or, from the Mac over the USB-ECM link (this is what the supervisor does):
printf 'reboot\n' | nc -u -w2 192.168.7.2 5141      # "reboot-plain" = no cookie
```

Boot arguments (all optional, **the watchdog is off by default**):

| argument | meaning |
|---|---|
| `sweetreboot=<seconds>` | reboot into fastboot every N seconds (watchdog).  Absent or 0 = off.  **A wrong value is a reboot loop - the phone would come up, wait, and reset again.** |
| `sweetrebootmode=plain` | the automatic resets (watchdog, normal reboot request) are plain warm resets instead of fastboot resets |
| `sweetrebootreason=<n>` | the raw byte written into the restart reason (default 2 = bootloader/fastboot, 4 = recovery) |
| `sweetrebootcmd=<0..4>` | which SPMI command encoding / window to use (see 2.2/6) |
| `sweetrebootpanic=0` | do not reset the phone on a panic (the panic screen stays) |
| `sweetrebootudp=0` | do not listen for the magic UDP packet |

The sysctl can also be *read*: it returns the last requested action (0 = nothing yet).

## 5. How to verify (without the phone)

```sh
bash tools/check-0102-self-reboot.sh      # structure + compile check, no hardware
```

The script checks the patch structure, that the patch applies to the CI build tree
(`logs/bt3/xnu`), and compiles every touched file with the flags of the CI kernel build
(`osfmk/kern/debug.c`, `osfmk/arm64/machine_routines.c`, `bsd/dev/sweet_ufs.c` from the CI
log, `bsd/dev/sweet_reboot.c` with `tools/research/compile-one.sh`).  All of them compile
cleanly (only the usual XNU `SYSCTL_PROC` cast warning, which every XNU sysctl produces).

On the phone (a later, separate step - **not done here**): boot a kernel with 0102 and look
for `SWEET REBOOT: SPMI ... reason=0x..`; then `sysctl -w kern.sweet_reboot=1` and check that
the phone shows the fastboot screen (`fastboot devices` on the Mac).

## 6. Risks and open questions

1. **The SPMI write is not proven on the phone.**  The encoding (EXT_WRITE = 2 in bits
   [29:27], the data byte in the WDATA register of the arbiter's read/write channel window)
   is the one the parallel battery patch 0104 uses, but nobody has written a PMIC register on
   this phone yet.  The driver tries one encoding (`sweetrebootcmd=`, default 0 = the channel
   window) and verifies the result by reading the byte back; a rejected command is harmless
   (the PMIC does not change).  If the log says `SPMI write form 0 rejected` or `did not
   stick`, try `sweetrebootcmd=1..4` (the variants differ in the command bits and in the
   window they are issued through).  The *worst* case is that a wrong encoding writes one
   byte to a neighbouring register of the same SDAM block (0x7149..0x714b) - the reason
   byte itself is never corrupted (read-modify-write + verification).
2. **The magic value could be 0x04 instead of 0x02** (the shift convention of the
   `bits = <0x01 0x07>` cell is not 100% confirmed).  If the phone reboots into Android
   instead of fastboot, try `sweetrebootreason=4` (and 1) - the value is a boot argument, no
   new kernel build needed.  The log prints the byte before and after the write.
3. **Reboot loop.**  `sweetreboot=<seconds>` with a small value resets the phone forever.
   That is why the watchdog is **off by default** and why the value has to be >= 10 s in
   practice.  The panic hook cannot loop: after the reset the phone is in fastboot (or in
   Android), not in our kernel, so our kernel does not run again.
4. **A plain reset boots Android.**  A warm reset on this phone leads to the ABL, which
   boots the `boot` partition (Android) unless the fastboot cookie is set.  So a plain
   reset takes the phone out of the iOS session; the supervisor boots the iOS image again
   from fastboot.
5. **The UDP listener is unauthenticated.**  Anybody who can reach the phone on the network
   (the USB-ECM link) can reset it with one datagram.  It only accepts the exact words
   `reboot` / `reboot-plain`, and it can be switched off with `sweetrebootudp=0`.
6. **The panic hook loses the panic log** in the NVRAM sense?  No: the hook is in
   `kdp_machine_reboot_type()`, i.e. *after* `SavePanicInfo()`; but the reset happens ~300 ms
   later, so a panic log that is still being written to NVRAM may be cut short.  The panic
   *text* is copied into the USB log before the reset.
7. **The reset happens on the CPU that panicked**, which is not necessarily the boot CPU.
   PSCI `SYSTEM_RESET` resets the whole SoC on QCOM (the firmware stops the other cores),
   but this has not been tested on the phone.
8. The phone must come back with the *same* kernel: after `fastboot boot` the supervisor
   boots the newest image; a kernel without 0102 simply has no such feature.

## 7. Files

| file | what |
|---|---|
| `xnu/patches/0102-venus-self-reboot-fastboot.patch` | the patch (new file + `bsd/conf/files` + `bsd/dev/sweet_ufs.c` + `osfmk/kern/debug.c` + `osfmk/arm64/machine_routines.c`) |
| `tools/check-0102-self-reboot.sh` | host side check (structure + compile) |
| `logs/bt3/xnu/bsd/dev/sweet_reboot.c` | the new driver in the local build tree (where the patch was made) |

## 8. Relation to the other patches

* `0104` (the battery agent) adds an SPMI *write* path for the PMIC.  Its encoding is the
  one this driver uses for the cookie, but this driver does not depend on 0104 (it has its
  own, self-contained SPMI code) - so the two can be merged later into one `sweet_spmi.c`
  write primitive.
* `tools/venus-supervisor.sh` (the supervisor agent) already expects this feature: it sends
  the magic packet to `192.168.7.2:5141` and mentions `kern.sweet_reboot` and this report.
