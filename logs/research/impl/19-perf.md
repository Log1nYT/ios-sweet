# 19 - Kernel side performance profiler (boot phases, console rate, UFS flush cost)

Implementation of the instrumentation part of wave-1 report 22
(`logs/research/22-performance-budget.md`, section 11 "The instrumentation to
add") and of the storage part of section 8 (W5, B11).

**Everything here is opt-in.**  Without `sweetperf=1` the new file starts no
thread, prints nothing and changes no behaviour.

## Files

| file | what |
|---|---|
| `xnu/patches/0121-venus-perf-profile.patch` | the patch: adds `bsd/dev/sweet_perf.c` + one line in `bsd/conf/files` |
| `xnu/patches/sweet_perf.c` | reference copy of the new source (same convention as `sweet_file_patch.c`, `sweet_gpu.c`) |
| `logs/opt1/xnu/bsd/dev/sweet_perf.c` | the working copy in my own tree (the patched XNU tree is `logs/bt3/xnu`, which I did **not** touch) |

`bsd/dev/sweet_ufs.c` is **not** modified by the patch - the two lines that
are needed there are written out in section 5 below so that whoever owns
that file can apply them without a conflict.

**Why the `bsd/conf/files` line is at the end of the `bsd/dev` group.**
`bsd/conf/files` is being patched by ~30 agents at the same time and the CI
*skips* a patch whose hunk does not apply (`git apply --check` -> warning,
the feature is then silently missing).  Every `sweet_*.c` line in the middle
of the `bsd/dev` group is the anchor of somebody else's hunk (0112, 0113,
0115, 0116, 0119, 0122, 0124), so an insertion there breaks their context.
The line is therefore added after `bsd/dev/dev_perfmon.c`, at the end of the
group, where no hunk of any other patch has its context.  The order of the
files in the list does not matter for the build.

## 1. The measured baseline (report 22, all numbers measured on venus)

| fact | number | source |
|---|---|---|
| launchd `hello` -> `Early boot complete` | 2.81 s | report 22 §2.1 |
| `Early boot complete` -> first iOS frame | **56.4 s** | report 22 §2 |
| what happens in those 56 s | 1 453 log lines, **431 process launches** (210 of them `xpcproxy`), 726 AKS calls, 5 `homed` crashes | report 22 §3 |
| console traffic in a normal session | 4 lines/s, 0.23 KiB/s - **not** the bottleneck | report 22 §4.2 |
| worst session | 126 lines/s, 9.8 k lines in 78 s | report 22 §4.1 |
| crash-loop heap scan | up to **96 MiB** of user memory copied per `backboardd`/`SpringBoard` crash (1 536 `copyin`s, ~30 ms) | report 22 §6.2 |
| UFS log flush | a whole-LUN `SYNCHRONIZE CACHE` **every 2 s** under the global UFS mutex = 480 stalls per 16 min session | report 22 §4.3 |
| frame rate | 33.3 fps, 4.6 Mpixel/frame on the CPU rasteriser (153 Mpixel/s), 18.4 MB copied per present | report 22 §5 |
| volume | marked inconsistent 176x/boot -> orphan-cleanup catalog scan on **every** mount | report 22 §7.3 |
| kernel phase (rootdev attach -> `exec launchd`) | **not measurable at all** - the kernel prints no timestamps | report 22 §2.1, §7.1 |

The gap that report 22 could not close is the last one: without a kernel
timestamp there is no way to say whether the 56 s are launchd, the daemon
storm or the storage stack.  That is what this patch measures.

## 2. What the new file does

### 2.1 The boot phase profile

The profiler watches the **kernel console ring** - the same 4 MiB ring that
`sweet_log_append()` fills in `osfmk/console/serial_console.c:_cnputs()` and
that the USB live logger reads (`bsd/dev/sweet_usb.c:906`) - and timestamps
the phase markers it finds there:

| marker | source |
|---|---|
| `hfs: mounted iosroot` | the first mount |
| `exec launchd` | the exec log of patch 0042 (`bsd/kern/kern_exec.c:7416`) |
| `Early boot complete` | the launchd line (userland, through the console) |
| `exec Setup` | the activation/onboarding app |
| `exec SpringBoard` | the first iOS app |
| `SWEET FB: display on` | the first frame presented |

When the boot is over (the first frame was presented + 3 s, or the boot
window is over) the table is printed:

```
SWEET PERF: boot phases (first frame presented), t0 = profiler armed, root device attached:
SWEET PERF:   +     0 ms  root volume mounted
SWEET PERF:   +    41 ms  launchd exec'd
SWEET PERF:   +  2851 ms  launchd early boot complete
SWEET PERF:   + 59310 ms  Setup exec'd (activation)
SWEET PERF:   + 59480 ms  SpringBoard exec'd
SWEET PERF:   + 60120 ms  first frame presented
```

The numbers above are placeholders; the point is that the *gaps* between the
lines are the boot budget.  `t0` is the moment the profiler was armed, i.e.
the moment the kernel had the root device (before the root volume was
mounted), so the first line of the table is the kernel's own storage cost.

`mach_absolute_time()` is the clock (the ARM arch counter, i.e. boot
relative); the ticks are converted with `clock_timebase_info()`, the same
conversion XNU itself uses.

### 2.2 The runtime sampler

Every `sweetperfint` seconds (default 5) one line:

```
SWEET PERF: t=12345ms console=41 lines/s 5120 B/s procs=63 threads=214 ufsync=3 skipped=0 max=12400us avg=8100us
```

* `console=.. lines/s` - **exact**, counted in the ring (every `\n`), not
  estimated.  This makes budget line **B10** self-measuring.
* `procs=` - the number of processes in `allproc` (`proc_iterate()`).  The
  daemon storm of report 22 §3 shows up here as a rising number.
* `threads=` - the global thread counter of `osfmk/kern/processor.h`.
* `ufsync=.. skipped=.. max=.. avg=..` - the UFS `SYNCHRONIZE CACHE`
  statistics of the hook (section 4).

Cost: one `memcpy` out of the ring every 200 ms, one `proc_iterate()` every
5 s and one `printf` every 5 s.  The profiler's own line is counted in the
console rate (1 line / 5 s), which is 0.2 lines/s - 5 % of the 4 lines/s of
a normal session.  Turn the profiler off for a clean measurement.

### 2.3 The extra mark API

`sweet_perf_mark("name")` adds a phase with an arbitrary name.  It exists so
that `sweet_ufs.c` can mark the two moments it alone knows (the start and
the end of the UFS probe) without the profiler having to guess them from the
console text.  Optional - the profiler works without it.

## 3. The boot args (exact spelling)

| arg | default | meaning |
|---|---|---|
| `sweetperf=1` | **off** | enables the profiler (whole feature) |
| `sweetperfint=5` | 5 s | sampler period, clamped to 1..3600 |
| `sweetperfboot=120` | 120 s | how long the boot profile watches for the markers; 0 = watch forever |
| `sweetufssync=30` | 0 (= the driver's own behaviour) | minimum number of seconds between two whole-LUN `SYNCHRONIZE CACHE` calls of the persistent-log flusher |

`sweetufssync` only has an effect if the UFS driver calls the hook
(section 5, change B).  Without change B the arg is parsed and ignored.

## 4. The UFS flush: what already exists and what the hook adds

**The knob already exists.**  `bsd/dev/sweet_ufs.c` parses it today:

```c
	PE_parse_boot_argn("ufslogsecs", &sweet_log_secs, sizeof(sweet_log_secs));
	if (sweet_log_secs == 0 || sweet_log_secs > 60) {
		sweet_log_secs = SWEET_LOG_SECS;         /* 10 */
	}
```

(`bsd/dev/sweet_ufs.c:868-871`; the variable is `sweet_log_secs`, the thread
is `sweet_log_thread()`, `bsd/dev/sweet_ufs.c:809`.)

⇒ **No kernel change is needed for W5.**  `ufslogsecs=30` is enough: it turns
the 480 whole-LUN `SYNCHRONIZE CACHE` operations of a 16 minute session into
32, i.e. **−448 device-wide flushes**, each of which blocks *every* HFS+
read and write for its duration (10-100 ms on real UFS).  Cost: 13 bytes of
cmdline, length-neutral against `ufslogsecs=2` (which is in the audited
diagnostic cmdline but **not** in the current one - `sweetlog=0` already
disables the flusher, so the current image has no flush at all).

`sweetlog=0` (already in the current cmdline) disables the flusher entirely,
so on the current image there is nothing to throttle.  The hook is for the
case where the persistent log is wanted *and* the flush is too expensive.

**What the hook adds** (`sweet_perf_ufs_sync_hook()` in the new file):

1. the **measured** duration of the flush (budget line **B11**: "the
   duration of each flush (write + SYNCHRONIZE CACHE)" - report 22 §11
   wanted exactly this print), and
2. an optional **skip**: with `sweetufssync=<secs>` the flush is only issued
   when the previous one is at least `<secs>` old.  The skipped count is
   printed, so the effect is visible.

With `sweetperf` off the hook is a plain pass-through: it calls the original
function and returns its result, so the behaviour of the driver does not
change.

## 5. The exact changes needed in `bsd/dev/sweet_ufs.c` (NOT in my patch)

The patch deliberately does not touch that file (another agent may own it).
Three lines are needed; all three are one-liners:

**Change A - arm the profiler** (needed, otherwise the file is inert):

```c
/* bsd/dev/sweet_ufs.c, after the line "extern void sweet_reboot_init(void);" */
extern void sweet_perf_init(void);
```
```c
/* bsd/dev/sweet_ufs.c, in sweet_ufs_init(), next to sweet_sample_start(); */
	sweet_perf_init();
```

**Change B - call the flush hook** (optional, only for the measurement and
the skip):

```c
/* bsd/dev/sweet_ufs.c, line 847, in sweet_log_thread(): */
 		if (!error) {
-			error = ufs_sync_cache();
+			error = sweet_perf_ufs_sync_hook(ufs_sync_cache);
 		}
```
(`ufs_sync_cache()` is `static` in that file, but taking its address inside
the same file and handing it to the hook is legal C.  The hook is a plain
function in the same kernel image, so no weak symbol or link order trick is
needed - `bsd/conf/files` guarantees that it is compiled in.)

**Change C - two extra boot marks** (optional):

```c
/* bsd/dev/sweet_ufs.c, at the top of sweet_ufs_init() and after the probe */
	sweet_perf_mark("ufs probe start");
	...
	sweet_perf_mark("ufs probe done");
```

## 6. The `sweetscan` gate: it already exists, verified

The task asked to make the 96 MiB heap scan of patches 0051/0053 switchable
at runtime.  **It already is**, so I changed nothing.  The gate is in
`osfmk/kern/exception.c` (patch 0051 added it, patch 0053 extended it):

```c
	if (!PE_parse_boot_argn("sweetscan", names, sizeof(names))) {
		strlcpy(names, "backboardd,SpringBoard", sizeof(names));
	}
	size_t n = strlen(name), found = 0;
	for (const char *t = names; *t;) {
		const char *end = strchr(t, ',');
		size_t tl = end ? (size_t)(end - t) : strlen(t);
		if (tl == n && n > 0 && strncmp(t, name, n) == 0) {
			found = 1;
			break;
		}
		...
	}
	if (!found) {
		return;                       /* no scan */
	}
	...
	while (lines < 700 && scanned < (96ull << 20)) {   /* 96 MiB */
```

(`osfmk/kern/exception.c:908-992`, called from `sweet_crash_strings()` for
`sig == 6 || sig == 5`, line 895.)

* **arg absent** -> the list is `"backboardd,SpringBoard"` -> the scan runs
  on every SIGABRT/SIGTRAP of those two processes (96 MiB of `copyin`).
* **`sweetscan=-`** -> the value is the one character `-`, which matches no
  process name -> `found` stays 0 -> **the scan never runs**.  Verified by
  reading the code; the same convention is used by `sweet_sample.c` and
  `sweet_keys.c` for their `-` = off values.
* **`sweetscan=0`** -> the value is the one character `0`, which likewise
  matches no process name -> the scan never runs.  (Same code path; `0` was
  not documented, it works.)

**The current cmdline already contains `sweetscan=-`** (see section 7), so
the W1 win of report 22 (**-96 MiB of user memory copies per
backboardd/SpringBoard crash**) is already in effect on the current image.
Nothing to do, nothing changed.

## 7. The cmdline budget (the limit is 607 bytes)

`shim/loader.c:15` defines `BOOT_LINE_LENGTH 608` -> the buffer holds 607
characters plus the NUL, so **607 bytes is the hard limit**.

The cmdline of the current image (from the `boot-args =` line of
`logs/venus-live-20261021l.txt`) is **598 bytes**:

```
debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress wdt=-1 rd=ufsroot0 ufsrw=1 rootrw=1 launchd_unsecure_cache=1 unentitled_ios_sim_launch=1 ignition_level=1 venus=1 sweetusb=2 sweetfb_noconsole=1 venusftsdelay=10 venusftspins=1 sweetlog=0 sweetscan=- sweetsetupfix=1 sweetfb_force_on=1 sweetfilepatch=mad:170d4:00484039:20008052,mad:1b5d0:e0030034:1f2003d5,mad:41e2c:76050036:1f2003d5,mad:41e94:000545f9:000845f9,mad:46218:760c0036:1f2003d5,mad:4629c:a00c0036:1f2003d5,mad:462cc:f50c00b4:1f2003d5 sweetfiledump=mad:170d4:8,mad:46218:8,/var/mobile/Library/Preferences/com.apple.purplebuddy.plist:0:192
```

⇒ **9 bytes free.**  The two big blocks are the `sweetfilepatch=` list
(210 B) and the `sweetfiledump=` list (103 B).

### 7.1 What each option costs

| option | text | **with the separating space** | needed? |
|---|---:|---:|---|
| `sweetperf=1` | 11 | **12** | yes - it is the switch |
| `sweetperfint=5` | 14 | 15 | optional (5 is the default) |
| `sweetperfboot=120` | 17 | 18 | optional (120 is the default) |
| `sweetufssync=30` | 15 | 16 | optional (needs change B) |
| **all four** | 57 | **61** | |

### 7.2 The cmdline-only wins of report 22 §8 (no kernel change at all)

| option | cost | win |
|---|---:|---|
| `ufslogsecs=30` | 14 B | **-448 whole-LUN `SYNCHRONIZE CACHE` per 16 min session** (480 -> 32) |
| `sweetiolog=-` | 13 B | -150 console lines/boot, -5 ms of `OSSerialize` |
| `sweetscan=-` | 12 B | **-96 MiB of user memory copies per backboardd/SpringBoard crash** (already in the current cmdline) |
| `sweetfb_hz=15` | 14 B | -529 MiB/s of memory traffic, -2.7 % of a core (costs 67 ms of latency) |

### 7.3 How to fit it

`598 + 61 = 659` > 607, so **52 bytes have to be freed** for the full set.
Options, in the order I would take them:

| what to drop | frees | comment |
|---|---:|---|
| `sweetfiledump=mad:170d4:8,mad:46218:8,...purplebuddy.plist:0:192` | **104 B** | a one-off diagnostic of a boot that is over; the purplebuddy dump is already on the phone |
| `sweetfilepatch=mad:...` (the whole list) | **211 B** | **only** if the daemon binary on the phone is already patched - NOTES.md says it is ("the daemon patches are already in the file, so they must NOT be listed again").  Keep it if the image is ever re-flashed. |
| `sweetfb_noconsole=1` | 20 B | only after the GUI is stable (it stops the console from drawing over the UI) |
| `sweetsample=...` | 88 B | not in the current cmdline; patch 0054's sampler, 252 lines per session |

**The minimum that fits today** (only `sweetperf=1`): 598 + 12 = **610**
> 607, so **3 bytes** still have to be freed - dropping the `sweetfiledump=`
block (104 B) is more than enough, and then there is room for
`sweetufssync=30` as well:

```
598 - 104 + 12 + 16 = 522 of 607   (85 bytes free)
```

With the defaults of `sweetperfint` and `sweetperfboot` there is nothing
else to add.

## 8. How to test it

1. build the kernel with patch 0121 and apply changes A and B of section 5
   to `bsd/dev/sweet_ufs.c`;
2. boot with `sweetperf=1` (plus `sweetufssync=30` if change B is in);
3. in the console log you should see:
   * `SWEET PERF: profiler armed (int=5s boot=120s ufsync=30s)` right after
     the UFS probe,
   * the boot phase table,
   * one `SWEET PERF: t=... console=... lines/s ...` line every 5 s;
4. check the numbers against report 22: the console rate must be in the
   4 lines/s range for a quiet session and up to 126 lines/s in a storm; the
   gap between `launchd early boot complete` and `first frame presented` is
   the 56 s that has to be explained;
5. `grep -c '^SWEET PERF' <log>` to see how much the profiler itself adds.

## 9. Limits and risks

* The profiler is **off by default**; with `sweetperf=1` it adds 1 console
  line per 5 s and one ring read per 200 ms.  It never writes to the disk
  and never touches the filesystem.
* The boot phase table is derived from the **console text**.  If the console
  is muted (`sweetfb_noconsole=1` only mutes the *drawing*, the ring still
  gets every byte - `osfmk/console/serial_console.c:_cnputs()` calls
  `sweet_log_append()` before the mute check) the markers are still seen.
  But if a *patch* that prints the marker is not in the kernel, the phase
  stays empty and the table says so.
* `mach_absolute_time()` on this board is the ARM arch counter.  Report 22
  §1.4 measured that the kernel's tick->time conversion is ~1.1-1.2x fast;
  the profiler therefore prints **relative** times, which are exact, and
  does not try to correct the absolute scale.
* The thread count is the global counter of `osfmk/kern/processor.h`
  (`threads_count`); it includes the kernel threads, the process count does
  not include `kernel_task`.
* `sweet_perf_ufs_sync_hook()` skips the flush only when `sweetufssync` is
  set and only for the **persistent log** flush - it never touches the HFS+
  metadata flush, which is a different code path (`hfs_sync()`).
