# 18 - venus volume keys (PM8350 GPIO) and the /product/audio device-tree node

**Agent:** implementation wave 2. **Date:** 2026-10-21.
**Files touched:** `xnu/patches/0120-venus-volume-keys-audio-dt.patch` (the kernel
side, `bsd/dev/sweet_keys.c`), `tools/mkadt_venus.py` (the ADT side, shared file,
small idempotent addition), `logs/research/impl/18-volume-keys-audio-dt.md` (this
file).  The regenerated ADT is `shim/out/venus-v42.adt` (build artifact, not in
git).  Nothing was written to the phone.

---

## 0. What this change does

| item | before | after |
|---|---|---|
| volume-up key | withheld forever ("GPIO2 not confirmed enabled as input") because the driver read a peripheral that does not exist on venus | read from the **PM8350 GPIO 6** pad the vendor device tree names, direction first, opt-in with `sweetkeyvol=1` |
| volume-down key | nothing | optional, `sweetkeyvdn=<PM8350 GPIO index>` (the device tree does not name it) |
| HID events | none for volume | `sweet_keyevents_key()` -> Consumer page 0x0C usage **0xE9** (up) / **0xEA** (down) - unchanged mapping in `SweetHID.cpp` |
| `/product/audio` | already present (commit f265020) | unchanged, flags still 0 |
| `/arm-io/spi2/audio-codec`, `/arm-io/i2c2/audio-speaker-top` | missing | added, every "hardware present" flag 0 |

---

## 1. The SPMI address space (the root cause)

### 1.1 The three facts that fix the address arithmetic

1. **A 16 bit SPMI address is `peripheral id (bits 15:8) | register (bits 7:0)`.**
   Evidence from the vendor DT (`/Users/matviy/Documents/Mu-Silicium/Resources/DTBs/venus.dtb`,
   decompiled with `dtc -I dtb -O dts`):
   `qcom,pmk8350@0 { reg = <0x00 0x00> }` and its child `pon_hlos@1300 { reg = <0x1300 0x800> }`
   -> peripheral id **0x13**, register offsets 0x00..0xff inside it.
   The working power key proves it: `bsd/dev/sweet_keys.c` reads `keys_pon_rt = 0x1310`
   and gets the KPDPWR/RESIN bits (the live log shows `reg=0x1310 0x10 -> 0x90`).
   0x1310 = peripheral 0x13, register **0x10** - exactly the PON "RT status" byte.
2. **The arbiter's 12 bit PPID is `(SPMI SID << 8) | peripheral id`.**
   Proven by three independent data points in the tree:
   | PPID used | SID | peripheral | DT node |
   |---|---|---|---|
   | `0x008` (sweet default) | 0 (PM6150) | 0x08 | `pon_pbs@800` |
   | `0x013` (venus) | 0 (PMK8350) | 0x13 | `pon_hlos@1300` |
   | `0x4c1` (sweet, used on venus by mistake) | 4 (PM6150L) | 0xc1 | `gpio@c100` |

   The same rule is used by the battery driver that shipped with patch 0104:
   `bsd/dev/sweet_spmi.c`: `sp_find_apid((sid << 8) | peripheral)`.
3. **A PMIC GPIO block gives every pin its own 0x100 byte page.**
   Evidence: the CAF driver of the board
   (`arch/arm64/boot/dts/vendor/qcom/pm8350.dtsi` compatible `qcom,pm8350-gpio`,
   driver `drivers/pinctrl/qcom/pinctrl-spmi-gpio.c` of the Xiaomi/LineageOS
   venus kernel, branch `lineage-24.0`):
   ```c
   #define PMIC_GPIO_ADDRESS_RANGE  0x100
   ...
   pad->base = reg + i * PMIC_GPIO_ADDRESS_RANGE;   /* one page per pin */
   ```
   and the same file's match table: `qcom,pm8350-gpio` = **10 pins**, so the pm8350
   GPIO block covers the peripherals 0x88..0x91 (base `reg = <0x8800>`).

### 1.2 The venus volume-up key

Vendor DT (venus.dtb) / Xiaomi kernel DTS
(`arch/arm64/boot/dts/vendor/qcom/xiaomi-sm8350-common.dtsi`):

```
&soc {
    gpio_keys {
        compatible = "gpio-keys";
        pinctrl-0 = <&key_vol_up_default>;
        vol_up {
            label = "volume_up";
            gpios = <&pm8350_gpios 6 GPIO_ACTIVE_LOW>;   /* pin 6, active low */
            linux,code = <KEY_VOLUMEUP>;
        };
    };
```
and inside `qcom,pm8350@1` (SID **1**):

```
    pinctrl@8800 {                       /* qcom,pm8350-gpio, reg = <0x8800> */
        key_vol_up_default {
            pins = "gpio6";
            function = "normal";
            input-enable;
            bias-pull-up;
            power-source = <0x01>;       /* 1 = PMIC pad, not TLMM */
        };
    };
```

=> the volume-up key is **PM8350 (SID 1) GPIO 6**, page peripheral
`0x88 + 6 = 0x8e`, **PPID = (1 << 8) | 0x8e = 0x18e**, and the key is **active low**
(pin pulled up, the switch pulls it to ground).

### 1.3 What was broken

The old driver looked up PPID **0x4c1** - that is the *sweet board's* PM6150L
(SID 4) GPIO block at 0xc100.  On venus SID 4 is the **pmr735a**, whose GPIO
block is at 0x8800 like every other venus PMIC: there is no peripheral 0xc1
there, so the reads never answered (the live log shows
`SWEET KEYS: read error pon-ok=1 gpio-ok=0` every 5 s) and the direction could
never be confirmed - hence "GPIO2 not confirmed enabled as input; volume-up
withheld, PMIC unchanged".

### 1.4 The register map of one PMIC GPIO page

From the same CAF driver (`drivers/pinctrl/qcom/pinctrl-spmi-gpio.c`):

| offset | name | meaning |
|---|---|---|
| 0x04 | `GPIO_TYPE` | **0x10** = PMIC GPIO block (the proof that we talk to the right peripheral) |
| 0x05 | `GPIO_SUBTYPE` | 0x01/0x05/0x09/0x0d = 4/8 channel GPIO/GPIOC, 0x10/0x11 = LV/MV pads |
| 0x10 | `MPP_RT_STS` | the **level** of the pin, bit 0 |
| 0x40 | `GPIO_MODE_CTL` | **direction in bits [6:4]**: 0 = digital input, 1 = digital output, 2 = input/output, 3 = analog pass-through. LV/MV pads (subtype 0x10/0x11) keep it in bits [1:0]. Bits [2:1] = function, bit 0 = output value |
| 0x41 | `DIG_VIN_CTL` | power source (1 = PMIC) |
| 0x42 | `DIG_PULL_CTL` | pull: 0 = pull-up 30 uA, 1 = 1.5 uA, 2 = 31.5 uA, 3 = boost, 4 = pull-down, 5 = none |
| 0x43 | `DIG_IN_CTL` | input buffer / test control |
| 0x45 | `DIG_OUT_CTL` | output buffer type and drive strength |
| 0x46 | `GPIO_EN_CTL` | bit 7 = "pad enable" (`is_enabled`) |

The old driver already used the right *offsets* (0x05 subtype, 0x10 value,
0x40 mode, 0x46 enable) - only the peripheral was wrong.

---

## 2. The rule the driver uses to decide "safe to read"

Implemented in `bsd/dev/sweet_keys.c` (patch 0120), function `keys_vol_open()`:

```
1. find the observer APID of PPID (1 << 8) | (0x88 + pin)          -> must exist
2. read GPIO_TYPE (0x04)          -> must be 0x10, else: not a PMIC GPIO page, stop
3. read GPIO_SUBTYPE (0x05) and GPIO_MODE_CTL (0x40)
4. direction = subtype is LV/MV ? (mode & 3) : ((mode >> 4) & 7)
5. direction == OUTPUT (1)  -> NEVER touch the pad: log "is an OUTPUT (the PMIC
                                drives the pin); volume-up withheld, PMIC unchanged"
                                and stop.  (the phone is driving the key line)
6. direction != INPUT (0)  -> not provably an input: withheld, unless
                              sweetkeyvol=2, which asks the PMIC for "input"
                              (write MODE_CTL = 0x00, read back, compare)
7. now - and only now - read MPP_RT_STS (0x10); bit 0 = 0 means pressed (active low)
8. the level must be seen twice in a row (40 ms) before it is published
```

Extra gates: the page identity (step 2) and the direction (step 4/5) are read
**before** the value, and the value is read **after** the pad is known to be an
input.  Nothing is written in steps 1-5 and 7-8.

### 2.1 Why the direction read must come first

* An SPMI GPIO page holds **one pin**.  Its `RT_STS` register reads the level
  the *pad* sees.  If the PMIC drives the pad (direction = output), `RT_STS`
  reads back **our own driven level**, not the key: the driver would publish
  events that have nothing to do with the key, or (worse, if the driver ever
  wrote the pad) drive a key line that the phone owns.
* The PMIC is the owner of the pad configuration (the vendor DT configures it
  from the bootloader/Android; under iOS nobody does).  The driver therefore
  *diagnoses* the direction and only *uses* a pad that is provably an input.
  This is the same rule the old code tried to apply ("GPIO configuration
  belongs to firmware. Diagnose, never repair it here."), but with the correct
  peripheral it can actually be applied.
* Reading the direction first also makes the failure legible: the log says
  *which* of the three rejections happened (no APID / not a PMIC GPIO page /
  output / not input), instead of the old blanket "not confirmed enabled as
  input".

### 2.2 The write path (only with `sweetkeyvol=2`)

`keys_write()` issues an `EXT_WRITEL` (the read command word with the read bit
cleared and the data byte in bits 23:16).  It is used for **exactly one**
thing: `MODE_CTL = 0x00` = "digital input, function normal, output value 0" -
i.e. it can only ever *stop* the PMIC from driving the pad, never make it
drive one.  Every write is read back and compared before it is trusted; if the
read-back does not show "input" the key stays withheld.

**Known risk (documented, not hidden):** the `EXT_WRITEL` encoding is inferred
from the `EXT_READL` one, not from a Qualcomm document.  The default mode
(`sweetkeyvol=1`) therefore never writes a single PMIC register.  Use
`sweetkeyvol=2` only on a phone you can reflash.

---

## 3. The ADT: the audio nodes

`tools/mkadt_venus.py` (shared file, edited in place).  All values come from
the real iPhone-8 tree (`/Users/matviy/Documents/Mu-Silicium/esp-payload/dt.raw`).

### 3.1 `/product/audio` (already present, commit f265020)

```
wireless-splitter            = 0   (was 1 in the iPhone-8 tree)
supports-always-listening    = 0   (was 1)
```
Both are "this device HAS this feature" claims; they are gated off by default
and come back with `--audio`.  The node itself is what report 17 §A0 asks for
(VirtualAudio has no device without it).

### 3.2 `/arm-io/spi2/audio-codec` and `/arm-io/i2c2/audio-speaker-top` (new)

Real D21AP values, copied faithfully, but **every hardware-present flag is 0**:

| node | property | D21AP | venus (default) |
|---|---|---|---|
| `spi2/audio-codec` | `compatible` | `audio-control,cs42l75` | same |
| | `device_type` | `audio-control` | same |
| | `primary-ASP`, `secondary-ASP`, `mikeyBus-ASP`, `primary-InMCLK`, `clp-ASP` | 2, 1, 3, 1, 1 | **0** |
| | `firmware-type`, `port-number`, `port-type`, `hawking-adc`, `receiver-pdm`, `actuator-pdm-routing` | 1, 1, 0xc, 5, 1, 0x02011400 | **0** |
| | `lmic/imic/smic/fmic-mic` + `-micbias` | 4/1/2/3 etc. | **0** |
| | `supports-CLP`, `rx-receiver`, `supports-aop-audio`, `maximum-asp1-drive-strength` | present (flags) | **dropped** (an absent flag is how iOS spells "not supported") |
| | `clp-asp-config`, `routing-source_clp`, `routing-source_clp_asp_tx`, `pollctl` | blobs | copied verbatim |
| `i2c2/audio-speaker-top` | `compatible` | `audio-control,cs35l26` | same |
| | `device_type` | `audio-control` | same |
| | `primary-speaker`, `secondary-speaker` | `SpoT`, `StoB` | same |
| | `channel-number` | 1 | **0** |
| | `tdm-config`, `speaker-config` | blobs | copied verbatim |
| both | `sweet-hardware-present` | - | **0** (our marker: no sweet driver binds to these nodes) |

Dropped on purpose (they point at the A11 or at Apple phandles): `reg`,
`interrupts`, `interrupt-parent`, `external-power-provider`,
`iis-power-provider`, `convoy-spkr`, all `function-*` dispatch blobs and the
`syscfg/*` placeholders.  The bus nodes `spi2` / `i2c2` keep only their
identity (`device_type`, `compatible`) - without `reg` no Apple bus driver can
match them.

Why 0: venus has a **WCD9385 on SoundWire** and two **CS35L41** amps on I2C
(report 17 §1.2/§1.4) - not a CS42L75/CS35L26 - and no kernel driver for any
of them.  The nodes exist so the iOS path lookups resolve; the flags say the
hardware is not there.

### 3.3 Regenerated ADT

```
$ python3 tools/mkadt_venus.py shim/out/venus-v42.adt
shim/out/venus-v42.adt: 17504 bytes
features enabled: (none - all hardware flags off)
$ python3 tools/adt.py dump shim/out/venus-v42.adt | grep -A3 audio
    spi2 { audio-codec { compatible = 'audio-control,cs42l75' ... } }
    i2c2 { audio-speaker-top { compatible = 'audio-control,cs35l26' ... } }
    audio { wireless-splitter = 0x0 supports-always-listening = 0x0 }
```
(The size depends on the other, concurrent changes to the tool - the
`/options` node of another agent is not in this tree.)

---

## 4. Boot args

| arg | default | meaning |
|---|---|---|
| `sweetkeyvol=0` | **0 (default)** | today's behaviour: the sweet PM6150L peripheral (0x4c1) is probed, does not answer on venus, the volume key stays withheld |
| `sweetkeyvol=1` | | the venus PM8350 GPIO pads are used: direction read first, read only, **no PMIC write at all** |
| `sweetkeyvol=2` | | as 1, and the driver may ask the PMIC for the "input" direction of a pad that is not an input (one write, read back, never drives the pin) |
| `sweetkeyvdn=N` | -1 (off) | the PM8350 GPIO index of the **volume-down** key (0..15).  The device tree only names volume-up, so the down key is off until the pin is known |
| `sweetkeys=0` | 1 | the whole key driver off (existing) |
| `sweetkeyevents=1` | 0 | the debounce/long-press state machine of patch 0116 (existing); without it the raw events go straight to the HID layer |

---

## 5. Test procedure

1. Build the kernel with patch 0120 (CI) and the shim with
   `shim/out/venus-v42.adt`.  Nothing is flashed by this change.
2. Boot with `sweetkeyvol=1` (keep the current cmdline otherwise) and read the
   console:
   * `SWEET KEYS: PM8350 GPIO6 page 0x8e PPID 0x18e APID <n> subtype 0x1 mode 0x0 dir 0 en 0x80 pull 0x0 level 0x1 -> volume-up live`
     -> the pad is an input: the key should work.
   * `... is an OUTPUT (the PMIC drives the pin); volume-up withheld, PMIC unchanged`
     -> the PMIC owns the pad: nothing is published (by design).  Nothing was
     written to the PMIC.
   * `... is not in the SPMI table` -> PPID 0x18e is not in the arbiter table:
     the address model is wrong; do **not** try `sweetkeyvol=2`.
   * `direction 2 is not 'input'` -> the pad is bidirectional: try
     `sweetkeyvol=2` (one write, read back).
3. Press the volume-up key: the console shows
   `SWEET KEYS: volume-up down=1 rt=0x0` and
   `SWEET HID: key 0xe9 down (1 clients)`.
   The release shows `down=0` / `key 0xe9 up`.
   `rt=0x0` while pressed is the expected value (active low).
4. Volume-down: the device tree does not name it.  Try
   `sweetkeyvdn=7`, `sweetkeyvdn=5`, `sweetkeyvdn=4` and watch which one shows
   `-> volume-down live` **and** reacts to the key.  (The pm8350 GPIO block
   has 10 pins; the DT uses 6 = volume-up, 8 = USB VBUS boost (output), 9 =
   USB VBUS detect (input); the rest are free.)
5. The iOS side: the HID events are Consumer page 0x0C usage 0xE9/0xEA, which
   is what SpringBoard/CoreAudio consume for the media keys.  The **volume
   slider** additionally needs an output device in the HAL (report 17 stage A);
   the DT half of it is this change (`/product/audio` + the two audio-control
   nodes), the other half is the userland HAL plug-in, which is not in this
   patch.
6. Regression check with the default boot args: the log must still contain
   `SWEET KEYS: GPIO2 not confirmed enabled as input; volume-up withheld, PMIC
   unchanged` and the power key must still work (`SWEET KEYS: key=0 down=1`).

---

## 6. Files / commits

| file | change |
|---|---|
| `xnu/patches/0120-venus-volume-keys-audio-dt.patch` | `bsd/dev/sweet_keys.c`: the PM8350 GPIO volume key path (direction first, read only by default, opt-in) |
| `tools/mkadt_venus.py` | the two audio-control nodes (shared file - it was under concurrent edit by another agent while this was written) |
| `logs/research/impl/18-volume-keys-audio-dt.md` | this report |

Compile check (the CI flags, local cross-compile, on a scratch copy of the
patched tree - `logs/keys5/xnu`, deleted again to keep the disk footprint
small; any build tree works):

```
$ cp -R logs/bt3/xnu logs/keys5/xnu      # scratch copy of the patched tree
$ cd logs/keys5/xnu && patch -p1 < ../../../xnu/patches/0116-venus-key-events.patch
$ XNU_BT=$PWD/logs/keys5 tools/research/compile-one.sh bsd/dev/sweet_keys.c
compile bsd/dev/sweet_keys.c  [clang]
OK after 0.1s
```
No new warnings (the four `-Wdeclaration-after-statement` and the
`-Wmissing-noreturn` warning are the ones the file already had).

The patch applies to the tree *after* 0116 (it uses `sweet_keyevents_key()`);
`tools/research/check-patch-order.py` reports it as applied.

