#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot.

VENUS variant: Xiaomi Mi 11 (venus, Qualcomm SM8350), iPhone 8 identity
(iPhone10,1 / D21AP).  Every MMIO address comes from the venus live FDT
(venus.dtb):
  - all QCom reserved-memory carve-outs lie in 0x80000000..0xE4E00000
  - /memory = <0x80000000 +0x39700000>, <0xC0000000 +0x1C0000000> (7.9GB)
  - free window above the carve-outs: 0xE6000000 .. 0x280000000 (real DRAM)
  - splash_region (ABL framebuffer) 0xE2A00000 +0x2300000, 1440x3200
  - GICD 0x17a00000, GICR 0x17a60000 (same as sweet!), vtimer PPI 11 -> 27
The top 1 MiB of the window is carved out for XNU's embedded panic log
(/pram + /chosen/embedded-panic-log-size), so the allocator cannot use it.
Usage:  mkadt_venus.py OUT.adt [--dump] [--qemu] [--highmem]
                             [--camera] [--sep] [--haptics] [--audio] [--backlight]

The feature switches turn the REAL iPhone-8 values of the matching
device-tree nodes on; by default the nodes exist (so the iOS path lookups
resolve) but every "hardware present" flag reads 0 - venus has none of that
hardware.  See logs/research/impl/08-adt-nodes.md.
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM: single carve-out-free window (venus bank2 above the
# splash); every XPU carve-out lies BELOW 0xe4e00000.
# phys_base MUST be 32MB-aligned: the kernel's own early MMU tables block-map
# from the mach-header physical block 0xe6000000, and the payload header's
# virt_base sits 32MB below the mach header. A misaligned phys_base (as
# 0xe5000000 was) skews every VA->PA translation by the sub-block offset
# (observed +0x1000000) -> first fetch after MMU-on hits zeros -> UDEF panic.
PHYS_BASE = 0xe6000000
# Real DRAM on venus ends at 0x280000000: the board has 8 GiB and the vendor
# FDT says /memory = <0x80000000 +0x39700000>, <0xc0000000 +0x1c0000000>, so
# bank 2 (the last one) ends at 0x280000000.  Publishing more than that
# hands the allocator pages with no backing behind them (the _bzero external
# abort at NOTES.md L227: the allocator returned 0x2800c0000, just past the
# real end), so the window stops at the real end of RAM.
MEM_SIZE = 0x19a000000          # up to the end of DRAM (0x280000000)
# --highmem: same window; kept as an alias so the flag stays harmless.
HIGH_PHYS_BASE = PHYS_BASE
HIGH_MEM_SIZE = MEM_SIZE
# XNU's "embedded panic log" buffer.  pexpert/arm/pe_init.c:
# check_for_panic_log() takes the physical base from the /pram node's reg[0]
# and the size from /chosen/embedded-panic-log-size, maps the region with
# ml_io_map_wcomb() and uses it as debug_buf_base (osfmk/kern/debug.c:
# debug_log_init()).  Without both nodes gPanicBase stays 0, debug_log_init()
# bails out with "Error!! gPanicBase is still not initialized" and every
# panic spins forever in SavePanicInfo() (osfmk/arm/model_dep.c:
# "while (!panic_info || panic_info->eph_panic_log_offset == 0)").
# 1 MiB = 64 x 16 KiB pages.  The region is carved off the TOP of the DRAM
# window we publish (like upstream sweet, dipa-studio/ios-sweet PR #6
# "adt-panic-region"), so the kernel maps it but the allocator can never
# hand it out.
PANIC_LOG_SIZE = 0x100000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
FB, FB_SIZE = 0xe2a00000, 1440 * 3200 * 4
VENUS_ECID = 0x56454e5553000001  # 'VENUS' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1440, 3200, 1440 * 4
# Internal UFS host (ufshc@1d84000 "ufs_mem"); probe-verified on venus
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 27               # venus arm,armv8-timer: virtual = PPI 11
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone 8 (iPhone10,1 / D21AP) IPSW. MobileGestalt
# derives product type, device class, Home-button UI, artwork etc. from
# the root identity and /product of the real D21AP device tree. That tree is
# an Apple artifact (kept out of git); we keep the one already extracted for
# the Mu-Silicium project at an absolute path, overridable by VENUS_D21_ADT.
D21AP_ADT = os.environ.get(
    'VENUS_D21_ADT',
    '/Users/matviy/Documents/Mu-Silicium/esp-payload/dt.raw')


def d21ap_product(path=D21AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')


# ---------------------------------------------------------------------------
# Device-tree nodes the iOS userland looks up but that our ADT did not have.
#
# logs/research/18-iokit-gap-inventory.md item 8: every boot the iOS side
# did 293 device-tree PATH lookups that returned "none" (the iOS code
# treats a missing node as "no hardware"):
#
#   IODeviceTree:/options           108x  (MobileKeyBag/keybagd, mobileassetd)
#   IODeviceTree:/product/camera     44x  (MobileGestalt, camera stack)
#   IODeviceTree:/arm-io/pearl-sep   43x  (SEP stack)
#   IODeviceTree:/product/haptics    28x  (haptics engine, Setup)
#   IODeviceTree:/arm-io/sep         25x  (SEP stack)
#   IODeviceTree:/arm-io/isp          25x  (MobileGestalt, camera stack)
#   IODeviceTree:/product/audio       24x  (audio stack, VirtualAudio HAL)
#   IODeviceTree:/backlight           17x  (backboardd, CoreBrightness)
#
# The property VALUES below are the real ones from the iPhone-8 (D21AP)
# device tree (Mu-Silicium/esp-payload/dt.raw) - the tree the iOS 16.7
# userland was written against.  venus has none of the hardware behind
# these nodes, so every "hardware present" flag is gated OFF by default
# and the CLI switches (--camera/--sep/--haptics/--audio/--backlight)
# put the real values back for a controlled experiment.
#
# 'sweet-hardware-present' is OUR marker: 0 = "no sweet driver binds to
# this node yet".  A future stub (camera/SEP/audio) MUST check it before
# it touches hardware.  See logs/research/impl/08-adt-nodes.md for the
# node -> consumer -> value -> risk table.
# ---------------------------------------------------------------------------

# Properties copied verbatim from the real D21AP tree - only the ones the
# iOS userland looks up.  Everything else is dropped on purpose: 'reg',
# 'interrupts', 'interrupt-parent', 'iommu-parent', 'clock-gates' and
# 'power-gates' are the A11's MMIO/interrupt plumbing and carry PHANDLE
# values from the Apple tree (they would point at our own nodes!), and the
# Apple 'function-*' blobs are dispatch tables for Apple kernel drivers.
ISP_KEEP = ('device_type', 'compatible', 'camera-rear', 'camera-front',
            'has-sphere', 'face-detection-support', 'sensor-type')
SEP_KEEP = ('device_type', 'compatible', 'role', 'iop-version', 'sika-support')
BACKLIGHT_KEEP = ('device_type', 'compatible')
# The big iPhone-8 backlight calibration tables (milliAmps2DACTablePart1/2,
# iDAC2MilliAmpsTable, backlight-marketing-table) describe the iPhone 8's
# LCD PWM backlight; venus is an AMOLED panel, so they are never copied.
# The same goes for 'backlight-calibration', which is an iBoot placeholder
# (syscfg/BLCl) in the real tree.
BACKLIGHT_TABLES = ('milliAmps2DACTablePart1', 'milliAmps2DACTablePart2',
                    'iDAC2MilliAmpsTable', 'backlight-marketing-table',
                    'backlight-calibration', 'display-backlight-calibration')
# The flags that mean "this device HAS the hardware"; they read 0 unless the
# matching feature switch is given.
CAMERA_GATES = ('camera-rear', 'camera-front', 'has-sphere',
                'face-detection-support')
HAPTICS_GATES = ('closed-loop', 'supports-3rd-party-haptics')
AUDIO_GATES = ('wireless-splitter', 'supports-always-listening')


def d21ap_node(path, path_to_file=D21AP_ADT):
    """The node at `path` ('/product/camera') of the real D21AP tree."""
    if not os.path.exists(path_to_file):
        return None
    root, _ = adt.parse(adt.load(path_to_file))
    cur = root
    for part in [p for p in path.split('/') if p]:
        nxt = None
        for c in cur.children:
            if c.name == part:
                nxt = c
                break
        if nxt is None:
            return None
        cur = nxt
    return cur


def d21ap_props(path, keep=None, path_to_file=D21AP_ADT):
    """{name: bytes} of the real D21AP node at `path`.

    'name'/'AAPL,phandle' are dropped (we assign our own phandles), so are
    the iBoot-filled placeholders (syscfg/*), and `keep` restricts the set
    to the properties the iOS userland actually looks up."""
    n = d21ap_node(path, path_to_file)
    if n is None:
        return {}
    out = {}
    for name, flags, val in n.props:
        if name in ('name', 'AAPL,phandle') or flags:
            continue
        if keep is not None and name not in keep:
            continue
        out[name] = val
    return out


def gate_off(props, names):
    """Turn the named u32 "hardware present" flags off (0).  Properties with
    another width are dropped instead: an absent property is how iOS spells
    "not supported"."""
    out = {}
    for k, v in props.items():
        if k in names:
            if len(v) == 4:
                out[k] = u32(0)
        else:
            out[k] = v
    return out


u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def build(seed=None, qemu=False, highmem=False, features=()):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py).

    features: subset of ('camera', 'sep', 'haptics', 'audio', 'backlight').
    Each switch turns the REAL iPhone-8 values of the matching device-tree
    nodes on; without them the nodes exist (so the iOS path lookups
    resolve) but every "hardware present" flag reads 0.  See
    logs/research/impl/08-adt-nodes.md."""
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    # Carve the panic log off the top of the DRAM window we publish, so the
    # kernel's allocator can never allocate into it.  The region is real
    # DRAM (0x27ff00000..0x280000000), just above the window we hand to
    # XNU (which now ends at 0x27ff00000 instead of at the real end of RAM,
    # 0x280000000).
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        # Size (bytes) of the embedded panic log buffer.  The physical base
        # comes from the /pram node below; XNU maps the region with
        # ml_io_map_wcomb() and writes the panic log into it.  The region is
        # above the DRAM window we publish, so nothing else uses it.
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1440x3200 @2x =
        # 750x1334 points (iPhone 8 class devices).
        ('display-scale', u32(2)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', VENUS_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
        # Hactivation gate (theapplewiki.com/wiki/Hactivation).  Upstream
        # sweet only sets the four security-status properties above, but
        # mobileactivationd also looks for "allow-hactivation" and
        # "ShouldHactivate" (both strings are in the 20H392 binary) and for
        # "disable-hactivation-ma".  Put all of them in /chosen (where the
        # other security status lives) so the daemon finds them whichever
        # node it reads.
        ('allow-hactivation', u32(1)),
        ('ShouldHactivate', u32(1)),
        ('disable-hactivation-ma', u32(0)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # VENUS SMP: SM8350 DynamIQ single SLT - MPIDR Aff1 = 0..7 from the
    # venus.dtb cpu nodes (cpu0 = X1 prime, cpu1-3 = A78 gold, cpu4-7 = A55
    # silver), all enable-method psci. The kernel defaults to one and uses
    # cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            # 'qcom,kryo470' matches the kernel's SweetCPU driver IONameMatch
            # (patch 0018); kryo585 kept as the venus-specific identity.
            ('compatible', cstr('qcom,kryo585') + cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2841600000 if core == 0
                                    else 2419200000 if core < 4 else 1804800000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),  # 27 = venus PPI 11
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io_children = [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(1440)),  # 750x1334 -> no swaps on the 1440x3200 panel
            ('canvas-height', u32(3200)),
        ])]

    # /arm-io/isp - the camera ISP node (25x "path IODeviceTree:/arm-io/isp
    # -> none" per boot).  MobileGestalt probes it next to /product/camera
    # (report 08 §3.3).  The MMIO/interrupt plumbing of the real A11 node
    # is NOT copied (see ISP_KEEP): 'reg' would point at the A11 ISP at
    # 0x0f000000, which does not exist on venus, and 'interrupts' /
    # 'clock-gates' carry Apple phandles.  The camera-present flags
    # (camera-rear / camera-front) are the feature gate: they read 0
    # unless --camera is given, so no subsystem goes looking for a camera
    # that is not there.
    isp_props = d21ap_props('/arm-io/isp', ISP_KEEP)
    if isp_props:
        if 'camera' not in features:
            isp_props = gate_off(isp_props, CAMERA_GATES)
        isp_props['sweet-hardware-present'] = u32(0)
        arm_io_children.append(node('isp', [ph()] + list(isp_props.items())))

    # /arm-io/sep + /arm-io/pearl-sep - the SEP (secure enclave) coprocessor.
    # NOT in the default tree on purpose: there is no SEP on venus
    # (report 21) and a SEP node would be a false positive that can push
    # the activation / FairPlay path down its SEP branch - the activation
    # path is exactly what the current byte patches work around.  The
    # nodes exist in the tool (--sep) so the experiment can be run; the
    # real D21AP values are device_type='sep', role='SEP', iop-version=1,
    # sika-support=1 (the SafePlay/FairPlay gate).
    if 'sep' in features:
        sep_props = d21ap_props('/arm-io/sep', SEP_KEEP)
        if sep_props:
            sep_props['sweet-hardware-present'] = u32(0)
            arm_io_children.append(node('sep', [ph()] + list(sep_props.items())))
            # 'pearl-sep' is the A13+ spelling of the same coprocessor; the
            # iOS 16.7 userland probes both names (43x + 25x per boot).
            # The D21AP (iPhone 8) tree only has 'sep', so this is an alias
            # with the same properties.
            arm_io_children.append(node('pearl-sep', [ph()] + list(sep_props.items())))

    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], arm_io_children)

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (absent = 0 = classic Home-button device, iPhone 8 class).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('opposed-power-vol-buttons', u32(1)),
        # 2 = no Home button (iPhone X class gestures), as in tools/mkadt.py
        ('home-button-type', u32(2)),
    ])

    product_props = d21ap_product()
    # Hactivation gate (theapplewiki.com/wiki/Hactivation): needs allow-hactivation in /product + a development kernel
    if product_props is not None:
        product_props = [(k, v) for k, v in product_props if k != 'allow-hactivation'] + [('allow-hactivation', u32(1))]

    # /product/camera, /product/haptics, /product/audio - the child nodes
    # of /product the iOS userland looks up (44x / 28x / 24x "-> none" per
    # boot).  d21ap_product() copies only the *properties* of the real
    # /product node, so its children were missing.
    product_children = []
    # /product/camera - the camera capability node MobileGestalt reads next
    # to /arm-io/isp (report 08 §3.3).  Every flag is a "this device HAS
    # this camera feature" claim; they read 0 unless --camera is given.
    camera_props = d21ap_props('/product/camera')
    if camera_props:
        if 'camera' not in features:
            # every /product/camera property is a "this device HAS this
            # camera feature" claim (auto-focus, flash, 4K fps, ...), so all
            # of them read 0 until --camera is given.
            camera_props = gate_off(camera_props, tuple(camera_props))
        product_children.append(node('camera', [ph()] + list(camera_props.items())))
    # /product/haptics - 'closed-loop' (Taptic Engine) and
    # 'supports-3rd-party-haptics' (Core Haptics).  venus has an AW8697
    # LRA on I2C (report 09 §2.4) but no AOP haptic engine, so both read
    # 0 unless --haptics is given.
    haptics_props = d21ap_props('/product/haptics')
    if haptics_props:
        if 'haptics' not in features:
            haptics_props = gate_off(haptics_props, HAPTICS_GATES)
        product_children.append(node('haptics', [ph()] + list(haptics_props.items())))
    # /product/audio - 'wireless-splitter' (AirPlay 2) and
    # 'supports-always-listening' (Siri always-on mic).  The node itself is
    # what report 17 §A0 asks for (VirtualAudio has no device without it);
    # the flags stay 0 until --audio.
    audio_props = d21ap_props('/product/audio')
    if audio_props:
        if 'audio' not in features:
            audio_props = gate_off(audio_props, AUDIO_GATES)
        product_children.append(node('audio', [ph()] + list(audio_props.items())))

    product = [node('product', [ph()] + product_props, product_children)] if product_props else []

    # /options - the iOS "options" device-tree node (108x "-> none" per
    # boot).  MobileKeyBag prints "MKB_INIT: Could not get options entry
    # from the device tree" because of it and MADAnalyticsManager reads
    # IODeviceTree:/options for its event-submission overrides.  On the real
    # iPhone 8 the node is EMPTY (report 21 §3.2) - only the phandle - and
    # that is what is copied here: the node exists so the lookups resolve.
    #
    # NOTE: this node is in SweetPlatformExpert::excludeList(), so no nub
    # is created for it and IODTPlatformExpert::publishNVRAM() (which would
    # assert on a non-IOPlatformExpertDevice provider) is never called.
    options = node('options', [ph()])

    # /backlight - the display backlight node (17x "-> none" per boot).
    # venus is an AMOLED panel: there is NO backlight PWM on the board
    # (report 09 §2.5), so by default only the identity of the node is
    # published.  With --backlight the real iPhone-8 scalars are added
    # (the big calibration tables are never copied - they describe the
    # iPhone 8's LCD PWM backlight).  The brightness slider route is the
    # 'backlight-control' property on the AppleCLCD service (report 18
    # §5.3), not this node.
    if 'backlight' in features:
        backlight_props = d21ap_props('/backlight')
        for drop in BACKLIGHT_TABLES:
            backlight_props.pop(drop, None)
    else:
        backlight_props = d21ap_props('/backlight', BACKLIGHT_KEEP)
    if backlight_props:
        backlight_props['sweet-hardware-present'] = u32(0)
        backlight = node('backlight', [ph()] + list(backlight_props.items()))
    else:
        backlight = node('backlight', [ph(), ('sweet-hardware-present', u32(0))])

    # Panic log region.  XNU's pe_init.c check_for_panic_log() looks up this
    # node by name and takes reg[0] as the physical address of the region
    # (reg[1], iBoot's kMemoryRegion_Panic size, is ignored); the size comes
    # from /chosen/embedded-panic-log-size.  The region sits just above the
    # dram-size we expose, so XNU maps it but never allocates into it.
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Identity of the iPhone 8 whose userland runs here: compatible[0] is
    # hw.model/hw.target (D21AP), model is hw.machine/hw.product
    # (iPhone10,1). 'xiaomi,sweet' is required for the kernel's built-in
    # SweetPlatformExpert (patch 0016 IONameMatch); 'xiaomi,venus' /
    # 'qcom,sm8350' keep the board's own identity for SweetPlatformExpert
    # matching port-specific code.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D21AP') + cstr('iPhone10,1') + cstr('xiaomi,sweet') + cstr('xiaomi,venus') + cstr('qcom,sm8350') + cstr('AppleARM')),
        ('model', cstr('iPhone10,1')),
        ('target-type', cstr('D21')),
        ('target-sub-type', cstr('D21AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram, options,
         backlight] + product + ([] if qemu else [ufs]))


# The feature switches: each one turns the REAL iPhone-8 values of one
# device-tree node on.  Default = "the node exists, the hardware-present
# flags read 0" (the truth on venus).
FEATURES = ('camera', 'sep', 'haptics', 'audio', 'backlight')

if __name__ == '__main__':
    argv = sys.argv[1:]
    feats = tuple(f for f in FEATURES if '--' + f in argv)
    root = build(qemu='--qemu' in argv, highmem='--highmem' in argv, features=feats)
    blob = adt.serialize(root)
    open(argv[0], 'wb').write(blob)
    if '--dump' in argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{argv[0]}: {len(blob)} bytes', file=sys.stderr)
    print(f'features enabled: {", ".join(feats) if feats else "(none - all hardware flags off)"}',
          file=sys.stderr)
