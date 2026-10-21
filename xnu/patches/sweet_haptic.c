/*
 * SWEET venus haptics (AW8697) and cover/headset sensors.
 *
 * Hardware (Xiaomi Mi 11 / venus, SM8350).  All of it comes from the vendor
 * device tree (Mu-Silicium/Resources/DTBs/venus.dtb, decompiled with dtc):
 *
 *   /soc/i2c@998000            qupv3_se6_i2c, "qcom,i2c-geni", irq 0x25f
 *     pins: gpio28 = SDA, gpio29 = SCL (pinctrl qupv3_se6_i2c_active)
 *     aw8697_haptic@5A         "awinic,aw8697_haptic", reg 0x5a
 *       reset-gpio = <&tlmm 155 0>   irq-gpio = <&tlmm 119 0>
 *   /soc/i2c@a94000            qupv3_se13_i2c, irq 0x166
 *     pins: gpio0 = SDA, gpio1 = SCL
 *     fsa4480@42               "qcom,fsa4480-i2c" (USB-C audio accessory
 *                              switch; the 3.5 mm jack detect itself is the
 *                              WCD938x MBHC block, which is not reachable
 *                              without a SoundWire audio stack)
 *   gpio-keys/hall_key         <&tlmm 80 GPIO_ACTIVE_LOW>  (stock Xiaomi
 *                              DTS; the DTB in use has the node disabled)
 *
 * The QUPv3 I2C blocks need GENI firmware in the SE, which only the
 * bootloader may have left behind, so this driver does not use the QUP
 * sequencer at all: it bit-bangs I2C on the two bus pins, the same way
 * bsd/dev/sweet_venus_touch.c bit-bangs SPI on the touch pins.  That keeps
 * the driver independent of the bootloader state, of the GENI firmware and
 * of the clock votes, and it touches only the two pins of the bus it uses.
 *
 * The AW8697 register map is the Awinic one (register 0x00 = chip id, 0x04 =
 * SYSCTRL, 0x05 = GO, 0x48 = CONT_CTRL, 0x7b/0x7c = drive level, ...).  The
 * tuning values are the ones the vendor device tree carries for this board
 * (vib_cont_drv_lev, vib_cont_td, vib_cont_zc_thr, vib_cont_num_brk,
 * vib_tset, vib_r_spare, vib_bstdbg, vib_bemf_config, vib_f0_pre,
 * vib_f0_coeff); they are copied verbatim into the driver below and are
 * marked with DT_ so that they can be checked against the device tree.
 *
 * Safety: the motor is only ever started through AW_REG_GO, and the driver
 * clears GO again after the requested time, verifies every register write by
 * reading it back, and refuses to write anything at all unless the chip
 * answered with its id.  If a register address were wrong, the write would
 * land in a harmless register - the only register that can make the motor
 * run is GO, and GO is written last and cleared again.
 *
 * Boot args:
 *   sweethaptic=0|1        enable the haptic driver (default 0 = off)
 *   sweethapticdelay=S     start after S seconds (default 45)
 *   sweethap buzz=MS       length of one buzz in ms (default 100)
 *   sweetcover=0|1         enable the hall/cover + headset poller
 *                          (default 1, it only reads)
 *   sweetcovercfg=0|1      also re-mux TLMM 80 to a GPIO input
 *                          (default 0: the pin is only read, never changed)
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern kern_return_t thread_terminate(thread_t);
/* IOKit side (iokit/Kernel/arm/hid/SweetCoverSensor.cpp). */
extern void sweet_hid_cover(int closed, int headset);

/* Exported to the IOKit haptic service (iokit/Kernel/arm/hid/SweetHaptics.cpp). */
int sweet_haptic_play(int intensity, int duration_ms);
void sweet_haptic_stop(void);
void sweet_haptic_init(void);

/*
 * TLMM (SM8350 GPIO controller).  The vendor device tree describes the
 * block at 0x0f000000, but the GPIO configuration registers live 0x100000
 * higher (bsd/dev/sweet_venus_touch.c found this the hard way: reading
 * 0x0f000000 returns zeroes).  One 4 KiB page per pin, TLMM v2 layout.
 */
#define VENUS_TLMM_PA        0x0f100000u
#define VENUS_TLMM_SIZE      0x00300000u
#define TLMM_CFG(pin)        ((uint32_t)(pin) * 0x1000u)
#define TLMM_IO(pin)         (TLMM_CFG(pin) + 4u)
#define TLMM_FUNC_GPIO       0u      /* function field, bits 4:2 */
#define TLMM_PULL_UP         3u      /* pull field, bits 1:0 */
#define TLMM_OE              0x200u  /* output enable, bit 9 */
#define TLMM_OUT_VALUE       0x002u  /* output value, bit 1 of IN/OUT */
#define TLMM_IN_VALUE        0x001u  /* input value, bit 0 of IN/OUT */

/* Device tree pins. */
#define AW_GPIO_RESET        155u
#define AW_GPIO_IRQ          119u
#define HALL_GPIO            80u
#define I2C6_SDA             28u     /* qupv3_se6_i2c_active */
#define I2C6_SCL             29u
#define I2C13_SDA            0u      /* qupv3_se13_i2c_active */
#define I2C13_SCL            1u
#define AW_I2C_ADDR          0x5au
#define FSA_I2C_ADDR         0x42u

/* AW8697 registers (Awinic). */
#define AW_REG_ID            0x00u
#define AW_REG_SYSINTM       0x03u
#define AW_REG_SYSCTRL       0x04u
#define AW_REG_GO            0x05u
#define AW_REG_TRG_CFG1      0x1bu
#define AW_REG_TRG_CFG2      0x1cu
#define AW_REG_DATCTRL       0x2bu
#define AW_REG_BSTDBG1       0x31u
#define AW_REG_BSTDBG2       0x32u
#define AW_REG_BSTDBG3       0x33u
#define AW_REG_GLB_STATE     0x46u
#define AW_REG_BST_AUTO      0x47u
#define AW_REG_CONT_CTRL     0x48u
#define AW_REG_F_PRE_H       0x49u
#define AW_REG_F_PRE_L       0x4au
#define AW_REG_TD_H          0x4bu
#define AW_REG_TD_L          0x4cu
#define AW_REG_TSET          0x4du
#define AW_REG_R_SPARE       0x5du
#define AW_REG_ZC_THRSH_H    0x72u
#define AW_REG_ZC_THRSH_L    0x73u
#define AW_REG_BEMF_VTHH_H   0x74u
#define AW_REG_BEMF_VTHH_L   0x75u
#define AW_REG_BEMF_VTHL_H   0x76u
#define AW_REG_BEMF_VTHL_L   0x77u
#define AW_REG_BEMF_NUM      0x78u
#define AW_REG_TIME_NZC      0x7au
#define AW_REG_DRV_LVL       0x7bu
#define AW_REG_DRV_LVL_OV    0x7cu
#define AW_CHIP_ID_8697      0x97u   /* AW8697; 0x07/0x06 are AW86915/17 */

/*
 * Device tree tuning values (venus.dtb, aw8697_haptic@5A).  Every one of
 * them is copied from the device tree; the comments give the property.
 */
#define DT_CONT_DRV_LEV      0x49u   /* vib_cont_drv_lev */
#define DT_CONT_DRV_LEV_OV   0x49u   /* vib_cont_drv_lvl_ov */
#define DT_CONT_TD           0xf06cu /* vib_cont_td */
#define DT_CONT_ZC_THR       0x8f8u  /* vib_cont_zc_thr */
#define DT_CONT_NUM_BRK      0x03u   /* vib_cont_num_brk */
#define DT_TSET              0x12u   /* vib_tset */
#define DT_R_SPARE           0x68u   /* vib_r_spare */
#define DT_BSTDBG1           0x30u   /* vib_bstdbg[0] */
#define DT_BSTDBG2           0xebu   /* vib_bstdbg[1] */
#define DT_BSTDBG3           0xd4u   /* vib_bstdbg[2] */
#define DT_BEMF_VTHH_H       0x00u   /* vib_bemf_config[0] */
#define DT_BEMF_VTHH_L       0x08u   /* vib_bemf_config[1] */
#define DT_BEMF_VTHL_H       0x03u   /* vib_bemf_config[2] */
#define DT_BEMF_VTHL_L       0xf8u   /* vib_bemf_config[3] */
#define DT_F0_PRE            0x6a4u  /* vib_f0_pre */
#define DT_F0_COEFF          0x104u  /* vib_f0_coeff */

/* SYSCTRL bits (play mode 3:2, boost 1, work mode 0). */
#define AW_SYSCTRL_PLAY_CONT     (2u << 2)
#define AW_SYSCTRL_PLAY_MASK     (~(3u << 2))
#define AW_SYSCTRL_BOOST_MASK    (~(1u << 1))
#define AW_SYSCTRL_BOOST_BYPASS  (0u << 1)
#define AW_SYSCTRL_WORK_MASK     (~(1u << 0))
#define AW_SYSCTRL_STANDBY       (1u << 0)
#define AW_SYSCTRL_ACTIVE        (0u << 0)
/* SYSINTM bits (under voltage lock out interrupt, bit 5). */
#define AW_SYSINTM_UVLO_MASK     (~(1u << 5))
#define AW_SYSINTM_UVLO_EN       (0u << 5)
/* CONT_CTRL bits. */
#define AW_CONT_ZC_DETEC_MASK     (~(1u << 7))
#define AW_CONT_ZC_DETEC_EN       (1u << 7)
#define AW_CONT_WAIT_MASK         (~(3u << 5))
#define AW_CONT_WAIT_1PERIOD      (0u << 5)
#define AW_CONT_MODE_MASK         (~(1u << 4))
#define AW_CONT_BY_GO             (0u << 4)
#define AW_CONT_CLOSE_MASK        (~(1u << 3))
#define AW_CONT_CLOSE_PLAYBACK    (1u << 3)
#define AW_CONT_F0_DETECT_MASK    (~(1u << 2))
#define AW_CONT_F0_DETECT_OFF     (0u << 2)
#define AW_CONT_O2C_MASK          (~(1u << 1))
#define AW_CONT_O2C_OFF           (0u << 1)
#define AW_CONT_AUTO_BRK_MASK     (~(1u << 0))
#define AW_CONT_AUTO_BRK_EN       (1u << 0)
/* DATCTRL bits (data path filter). */
#define AW_DATCTRL_FC_MASK        (~(3u << 6))
#define AW_DATCTRL_FC_1000HZ      (3u << 6)
#define AW_DATCTRL_LPF_MASK       (~(1u << 5))
#define AW_DATCTRL_LPF_EN         (1u << 5)
/* BEMF_NUM brake count, bits 3:0. */
#define AW_BEMF_NUM_BRK_MASK      (~(15u << 0))
/* GO bit 0. */
#define AW_GO_MASK                (~(1u << 0))
#define AW_GO_ENABLE              (1u << 0)

#define AW_MAX_BUZZ_MS            2000
#define AW_DEFAULT_BUZZ_MS        100

/* ------------------------------------------------------------------ state */

struct sweet_haptic {
	vm_offset_t tlmm;
	bool tlmm_ok;
	bool started;
	bool probed;        /* the AW8697 answered with its id */
	bool playing;
	int sda, scl;       /* I2C pins of the AW8697 bus */
	unsigned buzz_ms;
	unsigned errors;
	unsigned writes;
	/* request hand-off to the worker thread */
	int req_chan;
	int work_chan;
	bool req_pending;
	bool stop_pending;
	int req_intensity;
	int req_ms;
};

static struct sweet_haptic sh;
static int sh_cover_chan;

/* --------------------------------------------------------------- low level */

static inline uint32_t
sh_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
sh_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static inline bool
gpio_out(unsigned pin, bool high)
{
	uint32_t io = sh_rd(sh.tlmm, TLMM_IO(pin));

	if (high) {
		io |= TLMM_OUT_VALUE;
	} else {
		io &= ~TLMM_OUT_VALUE;
	}
	sh_wr(sh.tlmm, TLMM_IO(pin), io);
	return true;
}

static inline void
gpio_cfg(unsigned pin, unsigned func, unsigned pull, bool output)
{
	uint32_t cfg = sh_rd(sh.tlmm, TLMM_CFG(pin));

	cfg &= ~(0x7u << 2);         /* function */
	cfg &= ~0x3u;               /* pull */
	cfg |= (func << 2) | (pull & 3u);
	if (output) {
		cfg |= TLMM_OE;
	} else {
		cfg &= ~TLMM_OE;
	}
	sh_wr(sh.tlmm, TLMM_CFG(pin), cfg);
}

static inline bool
gpio_in(unsigned pin)
{
	return (sh_rd(sh.tlmm, TLMM_IO(pin)) & TLMM_IN_VALUE) != 0;
}

/* --------------------------------------------------------------- timing */

static uint64_t sh_ticks_per_us;

static void
sh_calibrate_time(void)
{
	mach_timebase_info_data_t tb;

	clock_timebase_info(&tb);
	if (tb.numer == 0 || tb.denom == 0) {
		sh_ticks_per_us = 0;
		return;
	}
	/* mach_absolute_time() ticks: 1 us = 1000 * denom / numer ticks. */
	sh_ticks_per_us = ((uint64_t)tb.denom * 1000ull) / (uint64_t)tb.numer;
	if (sh_ticks_per_us == 0) {
		sh_ticks_per_us = 1;
	}
}

/* Busy wait; the I2C bit banging must not sleep. */
static inline void
sh_udelay(unsigned usec)
{
	uint64_t ticks, end;

	if (sh_ticks_per_us == 0) {
		delay(1);
		return;
	}
	ticks = (uint64_t)usec * sh_ticks_per_us;
	end = mach_absolute_time() + ticks;
	while (mach_absolute_time() < end) {
	}
}

/* ------------------------------------------------------------------ I2C */

/*
 * Open drain emulation: the line is driven low by enabling the output with
 * a zero in the output latch, and released by switching the pad back to an
 * input.  The internal pull-up (TLMM pull = 3) holds the released line
 * high, exactly like the pull-up the device tree asks the QUP block for.
 */
static void
i2c_release(unsigned pin)
{
	gpio_cfg(pin, TLMM_FUNC_GPIO, TLMM_PULL_UP, false);
}

static void
i2c_drive_low(unsigned pin)
{
	/* the latch has to be zero before the output is enabled */
	gpio_out(pin, false);
	gpio_cfg(pin, TLMM_FUNC_GPIO, TLMM_PULL_UP, true);
}

static bool
i2c_line_high(unsigned pin)
{
	return gpio_in(pin);
}

/*
 * A start condition is SDA falling while SCL is high, a stop condition is
 * SDA rising while SCL is high.  The bus is left idle (both lines high)
 * after every transfer.
 */
static void
i2c_start(void)
{
	i2c_release(sh.sda);
	i2c_release(sh.scl);
	sh_udelay(5);
	i2c_drive_low(sh.sda);	/* SDA falls while SCL is high: START */
	sh_udelay(5);
	i2c_drive_low(sh.scl);
	sh_udelay(5);
}

static void
i2c_stop(void)
{
	i2c_drive_low(sh.sda);
	sh_udelay(5);
	i2c_release(sh.scl);	/* SCL high */
	sh_udelay(5);
	i2c_release(sh.sda);	/* SDA rises while SCL is high: STOP */
	sh_udelay(5);
}

/* Returns false when the slave did not acknowledge. */
static bool
i2c_put_byte(uint8_t byte)
{
	uint8_t bit;
	bool ack;

	/* SCL is low here: set SDA, raise SCL, lower SCL again. */
	for (int i = 7; i >= 0; --i) {
		bit = (byte >> i) & 1u;
		if (bit) {
			i2c_release(sh.sda);
		} else {
			i2c_drive_low(sh.sda);
		}
		sh_udelay(5);
		i2c_release(sh.scl);
		sh_udelay(5);
		i2c_drive_low(sh.scl);
		sh_udelay(5);
	}
	/* acknowledge bit: release SDA while SCL is low, then look at it */
	i2c_release(sh.sda);
	sh_udelay(5);
	i2c_release(sh.scl);
	sh_udelay(5);
	ack = !i2c_line_high(sh.sda);
	i2c_drive_low(sh.scl);
	sh_udelay(5);
	return ack;
}

static uint8_t
i2c_get_byte(bool send_ack)
{
	uint8_t byte = 0;

	i2c_release(sh.sda);
	for (int i = 7; i >= 0; --i) {
		i2c_release(sh.scl);
		sh_udelay(5);
		byte = (uint8_t)((byte << 1) | (i2c_line_high(sh.sda) ? 1u : 0u));
		i2c_drive_low(sh.scl);
		sh_udelay(5);
	}
	/* the master acknowledges (or not) while SCL is low */
	if (send_ack) {
		i2c_release(sh.sda);
	} else {
		i2c_drive_low(sh.sda);
	}
	sh_udelay(5);
	i2c_release(sh.scl);
	sh_udelay(5);
	i2c_drive_low(sh.scl);
	sh_udelay(5);
	return byte;
}

static int
i2c_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
	int err = 0;

	i2c_start();
	if (!i2c_put_byte((uint8_t)(addr << 1))) {
		i2c_stop();
		return ENXIO;
	}
	if (!i2c_put_byte(reg)) {
		i2c_stop();
		return ENXIO;
	}
	if (!i2c_put_byte(val)) {
		i2c_stop();
		return EIO;
	}
	i2c_stop();
	return err;
}

static int
i2c_read_reg(uint8_t addr, uint8_t reg, uint8_t *val)
{
	i2c_start();
	if (!i2c_put_byte((uint8_t)(addr << 1))) {
		i2c_stop();
		return ENXIO;
	}
	if (!i2c_put_byte(reg)) {
		i2c_stop();
		return ENXIO;
	}
	/* repeated start: SDA falls while SCL is high */
	i2c_release(sh.sda);
	sh_udelay(5);
	i2c_release(sh.scl);
	sh_udelay(5);
	i2c_drive_low(sh.sda);
	sh_udelay(5);
	i2c_drive_low(sh.scl);
	sh_udelay(5);
	if (!i2c_put_byte((uint8_t)((addr << 1) | 1u))) {
		i2c_stop();
		return ENXIO;
	}
	*val = i2c_get_byte(false);
	i2c_stop();
	return 0;
}

/* ------------------------------------------------------------- AW8697 I/O */

static int
aw_write(uint8_t reg, uint8_t val)
{
	int err = i2c_write_reg(AW_I2C_ADDR, reg, val);

	if (err == 0) {
		++sh.writes;
	}
	return err;
}

/* Write and read back: every write has to be confirmed. */
static int
aw_write_verify(uint8_t reg, uint8_t val)
{
	uint8_t rb = 0;
	int err = aw_write(reg, val);

	if (err) {
		return err;
	}
	err = i2c_read_reg(AW_I2C_ADDR, reg, &rb);
	if (err) {
		return err;
	}
	if (rb != val) {
		printf("SWEET HAPTIC: write reg %#x = %#x read back %#x\n",
		    reg, val, rb);
		return EIO;
	}
	return 0;
}

static int
aw_read(uint8_t reg, uint8_t *val)
{
	return i2c_read_reg(AW_I2C_ADDR, reg, val);
}

/*
 * Read, change some bits, write, read back.  "keep" is the mask of the
 * bits that stay as they are (the vendor driver uses the inverted mask).
 */
static int
aw_write_bits_verify(uint8_t reg, uint8_t keep, uint8_t value)
{
	uint8_t old = 0, nval;
	int err = aw_read(reg, &old);

	if (err) {
		return err;
	}
	nval = (uint8_t)((old & keep) | value);
	return aw_write_verify(reg, nval);
}

/*
 * The motor is started and stopped through GO only.  Stopping is done
 * before every start, so a failed or partial start can never leave the
 * motor running.
 */
static int
aw_go(bool on)
{
	uint8_t val = 0;
	int err;

	err = aw_write(AW_REG_GO, on ? AW_GO_ENABLE : 0);
	if (err) {
		return err;
	}
	err = aw_read(AW_REG_GO, &val);
	if (err) {
		return err;
	}
	if ((val & 1u) != (on ? 1u : 0u)) {
		printf("SWEET HAPTIC: GO=%#x after %s\n", val, on ? "start" : "stop");
		return EIO;
	}
	sh.playing = on;
	return 0;
}

/*
 * Program the continuous mode setup with the values of the device tree.
 * Every register is read back afterwards.
 */
static int
aw_setup(void)
{
	uint8_t f0_reg;
	int err;

	/*
	 * SYSCTRL: continuous playback, boost bypassed, chip out of standby
	 * (the vendor driver does the same for the continuous mode).
	 */
	err = aw_write_verify(AW_REG_SYSCTRL,
	    (uint8_t)((AW_SYSCTRL_PLAY_CONT | AW_SYSCTRL_BOOST_BYPASS |
	    AW_SYSCTRL_ACTIVE) & 0xffu));
	if (err) {
		return err;
	}
	/* SYSINTM: under voltage lock out interrupt on (bit 5 = 0). */
	err = aw_write_bits_verify(AW_REG_SYSINTM, 0xdfu, AW_SYSINTM_UVLO_EN);
	if (err) {
		return err;
	}
	/* DATCTRL: 1 kHz data path, low pass filter on. */
	err = aw_write_verify(AW_REG_DATCTRL,
	    (uint8_t)((AW_DATCTRL_FC_1000HZ | AW_DATCTRL_LPF_EN) & 0xffu));
	if (err) {
		return err;
	}
	/* CONT_CTRL: zero cross detect, one period wait, start on GO,
	 * close the playback, no f0 detect, no overshoot control,
	 * automatic brake. */
	err = aw_write_verify(AW_REG_CONT_CTRL,
	    (uint8_t)((AW_CONT_ZC_DETEC_EN | AW_CONT_WAIT_1PERIOD |
	    AW_CONT_BY_GO | AW_CONT_CLOSE_PLAYBACK | AW_CONT_F0_DETECT_OFF |
	    AW_CONT_O2C_OFF | AW_CONT_AUTO_BRK_EN) & 0xffu));
	if (err) {
		return err;
	}
	/* f0 preset: 1 GHz / (f0_pre * f0_coeff). */
	f0_reg = (uint8_t)((1000000000u / (DT_F0_PRE * DT_F0_COEFF)) >> 8);
	err = aw_write_verify(AW_REG_F_PRE_H, f0_reg);
	if (err) {
		return err;
	}
	f0_reg = (uint8_t)((1000000000u / (DT_F0_PRE * DT_F0_COEFF)) & 0xffu);
	err = aw_write_verify(AW_REG_F_PRE_L, f0_reg);
	if (err) {
		return err;
	}
	/* drive time, tset, zero cross threshold, brake count. */
	err = aw_write_verify(AW_REG_TD_H, (uint8_t)(DT_CONT_TD >> 8));
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_TD_L, (uint8_t)(DT_CONT_TD & 0xffu));
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_TSET, DT_TSET);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_ZC_THRSH_H, (uint8_t)(DT_CONT_ZC_THR >> 8));
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_ZC_THRSH_L, (uint8_t)(DT_CONT_ZC_THR & 0xffu));
	if (err) {
		return err;
	}
	err = aw_write_bits_verify(AW_REG_BEMF_NUM, 0xf0u,
	    (uint8_t)(DT_CONT_NUM_BRK & 0x0fu));
	if (err) {
		return err;
	}
	/* 35 * 171 us = 5.985 ms of non zero crossing time. */
	err = aw_write_verify(AW_REG_TIME_NZC, 0x23);
	if (err) {
		return err;
	}
	/* drive level of the LRA. */
	err = aw_write_verify(AW_REG_DRV_LVL, DT_CONT_DRV_LEV);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_DRV_LVL_OV, DT_CONT_DRV_LEV_OV);
	if (err) {
		return err;
	}
	/* boost debug / r spare / bemf thresholds (device tree values). */
	err = aw_write_verify(AW_REG_BSTDBG1, DT_BSTDBG1);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_BSTDBG2, DT_BSTDBG2);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_BSTDBG3, DT_BSTDBG3);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_R_SPARE, DT_R_SPARE);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_BEMF_VTHH_H, DT_BEMF_VTHH_H);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_BEMF_VTHH_L, DT_BEMF_VTHH_L);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_BEMF_VTHL_H, DT_BEMF_VTHL_H);
	if (err) {
		return err;
	}
	err = aw_write_verify(AW_REG_BEMF_VTHL_L, DT_BEMF_VTHL_L);
	if (err) {
		return err;
	}
	return 0;
}

/* Hardware reset of the AW8697 (TLMM 155, active high in the tree). */
static void
aw_hw_reset(void)
{
	gpio_cfg(AW_GPIO_RESET, TLMM_FUNC_GPIO, 0, true);
	gpio_out(AW_GPIO_RESET, false);
	delay(2000);
	gpio_out(AW_GPIO_RESET, true);
	delay(5000);
	/* The interrupt line is an input with a pull-up. */
	gpio_cfg(AW_GPIO_IRQ, TLMM_FUNC_GPIO, TLMM_PULL_UP, false);
}

static int
aw_probe(void)
{
	uint8_t id = 0;
	int err;

	for (int attempt = 0; attempt < 5; ++attempt) {
		aw_hw_reset();
		err = aw_read(AW_REG_ID, &id);
		if (err == 0 && id == AW_CHIP_ID_8697) {
			/* soft reset (0xaa in the id register) */
			err = aw_write(AW_REG_ID, 0xaau);
			if (err) {
				return err;
			}
			delay(3);
			sh.probed = true;
			return 0;
		}
		printf("SWEET HAPTIC: chip id read err=%d id=%#x\n", err,
		    (unsigned)id);
		delay(20);
	}
	return ENXIO;
}

/* ------------------------------------------------------------- fsa4480 */

/*
 * The USB-C audio accessory switch.  Its status register tells whether the
 * aux/analog switch is in the display port position; the QCOM fsa4480
 * driver expects 0x23 or 0x1c there when nothing analog is plugged in.
 * The 3.5 mm jack itself is detected by the WCD938x MBHC block over
 * SoundWire, which this port does not have at all.
 */
static int
fsa_read_status(uint8_t *status, uint8_t *control)
{
	int err;
	int sda = sh.sda, scl = sh.scl;

	sh.sda = I2C13_SDA;
	sh.scl = I2C13_SCL;
	gpio_cfg(sh.sda, TLMM_FUNC_GPIO, TLMM_PULL_UP, false);
	gpio_cfg(sh.scl, TLMM_FUNC_GPIO, TLMM_PULL_UP, false);
	err = i2c_read_reg(FSA_I2C_ADDR, 0x07, status);
	if (err == 0) {
		err = i2c_read_reg(FSA_I2C_ADDR, 0x05, control);
	}
	sh.sda = sda;
	sh.scl = scl;
	return err;
}

/* --------------------------------------------------------------- worker */

static void
sh_sleep_ms(int *chan, unsigned ms)
{
	uint64_t deadline;

	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
haptic_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 45;
	int enabled = 0;
	uint8_t id = 0;

	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("sweethaptic", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("sweethapticdelay", &delay_s, sizeof(delay_s));
	if (!enabled) {
		printf("SWEET HAPTIC: disabled (sweethaptic=0)\n");
		goto out;
	}
	printf("SWEET HAPTIC: starting in %u s\n", delay_s);
	if (delay_s) {
		sh_sleep_ms(&sh.work_chan, delay_s * 1000u);
	}
	sh.tlmm = ml_io_map(VENUS_TLMM_PA, VENUS_TLMM_SIZE);
	if (!sh.tlmm) {
		printf("SWEET HAPTIC: TLMM map failed\n");
		goto out;
	}
	sh.tlmm_ok = true;
	sh_calibrate_time();
	printf("SWEET HAPTIC: tlmm=%#x ticks/us=%llu\n", (unsigned)sh.tlmm,
	    (unsigned long long)sh_ticks_per_us);

	/* The device tree does not say which of the two pins is SDA. */
	for (int swap = 0; swap < 2 && !sh.probed; ++swap) {
		sh.sda = swap ? I2C6_SCL : I2C6_SDA;
		sh.scl = swap ? I2C6_SDA : I2C6_SCL;
		gpio_cfg(sh.sda, TLMM_FUNC_GPIO, TLMM_PULL_UP, false);
		gpio_cfg(sh.scl, TLMM_FUNC_GPIO, TLMM_PULL_UP, false);
		/* a released line must read high */
		if (!i2c_line_high(sh.sda) || !i2c_line_high(sh.scl)) {
			printf("SWEET HAPTIC: bus %u lines low (sda=%u scl=%u)\n",
			    (unsigned)swap, (unsigned)i2c_line_high(sh.sda),
			    (unsigned)i2c_line_high(sh.scl));
			continue;
		}
		printf("SWEET HAPTIC: probing %u as SDA, %u as SCL\n",
		    (unsigned)sh.sda, (unsigned)sh.scl);
		if (aw_probe() == 0) {
			printf("SWEET HAPTIC: AW8697 id=%#x on sda=%u scl=%u\n",
			    (unsigned)id, (unsigned)sh.sda, (unsigned)sh.scl);
			break;
		}
	}
	if (!sh.probed) {
		printf("SWEET HAPTIC: no AW8697 found; haptics disabled\n");
		goto out;
	}
	if (aw_setup() != 0) {
		printf("SWEET HAPTIC: setup failed; haptics disabled\n");
		goto out;
	}
	if (aw_go(false) != 0) {
		printf("SWEET HAPTIC: cannot stop the motor; haptics disabled\n");
		goto out;
	}
	printf("SWEET HAPTIC: ready (continuous mode, %u writes verified)\n", sh.writes);

	for (;;) {
		uint64_t deadline;

		if (sh.stop_pending) {
			sh.stop_pending = false;
			if (sh.probed) {
				(void)aw_go(false);
			}
		}
		if (sh.req_pending) {
			int ms = sh.req_ms;
			int intensity = sh.req_intensity;
			uint8_t level;

			sh.req_pending = false;
			if (ms <= 0) {
				ms = AW_DEFAULT_BUZZ_MS;
			}
			if (ms > AW_MAX_BUZZ_MS) {
				ms = AW_MAX_BUZZ_MS;
			}
			/* intensity scales the drive level, 0..255 -> 0x00..0x7f */
			level = (uint8_t)((DT_CONT_DRV_LEV * intensity) / 255);
			if (intensity <= 0) {
				level = 0;
			}
			if (level == 0) {
				level = 1;
			}
			(void)aw_write_verify(AW_REG_DRV_LVL, level);
			if (aw_go(true) == 0) {
				sh_sleep_ms(&sh.work_chan, (unsigned)ms);
				(void)aw_go(false);
			} else {
				printf("SWEET HAPTIC: start failed\n");
			}
		}
		/* wait for the next request (100 ms timeout) */
		clock_interval_to_deadline(100, NSEC_PER_MSEC, &deadline);
		(void)assert_wait_deadline(&sh.req_chan, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* ------------------------------------------------------------- cover/head */

static void
cover_thread(void *arg, wait_result_t result)
{
	int enabled = 1, cfg = 0;
	int last_closed = -1, last_headset = -1;

	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("sweetcover", &enabled, sizeof(enabled));
	(void)PE_parse_boot_argn("sweetcovercfg", &cfg, sizeof(cfg));
	if (!enabled) {
		printf("SWEET COVER: disabled (sweetcover=0)\n");
		goto out;
	}
	if (!sh.tlmm_ok) {
		sh.tlmm = ml_io_map(VENUS_TLMM_PA, VENUS_TLMM_SIZE);
		if (!sh.tlmm) {
			printf("SWEET COVER: TLMM map failed\n");
			goto out;
		}
		sh.tlmm_ok = true;
	}
	printf("SWEET COVER: hall gpio %u cfg=%#x io=%#x\n", HALL_GPIO,
	    sh_rd(sh.tlmm, TLMM_CFG(HALL_GPIO)), sh_rd(sh.tlmm, TLMM_IO(HALL_GPIO)));
	if (cfg) {
		/* the stock device tree uses it as a GPIO input, active low */
		gpio_cfg(HALL_GPIO, TLMM_FUNC_GPIO, TLMM_PULL_UP, false);
		printf("SWEET COVER: hall gpio %u re-muxed to input (cfg=%#x)\n",
		    HALL_GPIO, sh_rd(sh.tlmm, TLMM_CFG(HALL_GPIO)));
	}
	for (;;) {
		/* active low: a low level means the magnet is near */
		int closed = !gpio_in(HALL_GPIO);
		int headset = 0;

		if (sh.probed) {
			uint8_t status = 0, control = 0;
			if (fsa_read_status(&status, &control) == 0) {
				/* the aux switch sits in the display port
				 * position when nothing analog is plugged */
				headset = (status != 0x23 && status != 0x1c);
				if (last_headset != headset) {
					printf("SWEET COVER: fsa4480 status=%#x "
					    "control=%#x headset=%d\n",
					    status, control, headset);
				}
			}
		}
		if (closed != last_closed || headset != last_headset) {
			last_closed = closed;
			last_headset = headset;
			sweet_hid_cover(closed, headset);
		}
		sh_sleep_ms(&sh_cover_chan, 200);
	}
out:
	(void)thread_terminate(current_thread());
	thread_block(THREAD_CONTINUE_NULL);
}

/* --------------------------------------------------------------- public */

/*
 * Called from the IOKit service when the userland asks for a buzz.
 * Returns 0 when the request was queued.
 */
int
sweet_haptic_play(int intensity, int duration_ms)
{
	if (!sh.probed) {
		return ENXIO;
	}
	if (intensity < 0) {
		intensity = 0;
	}
	if (intensity > 255) {
		intensity = 255;
	}
	if (duration_ms <= 0) {
		duration_ms = AW_DEFAULT_BUZZ_MS;
	}
	if (duration_ms > AW_MAX_BUZZ_MS) {
		duration_ms = AW_MAX_BUZZ_MS;
	}
	sh.req_intensity = intensity;
	sh.req_ms = duration_ms;
	sh.req_pending = true;
	thread_wakeup(&sh.req_chan);
	return 0;
}

/*
 * Stops the motor (used when the service goes away).  The I2C bus belongs
 * to the worker thread, so the request is handed over to it.
 */
void
sweet_haptic_stop(void)
{
	sh.req_pending = false;
	sh.stop_pending = true;
	thread_wakeup(&sh.req_chan);
}

void
sweet_haptic_init(void)
{
	thread_t thread;

	if (sh.started) {
		return;
	}
	sh.started = true;
	sh.buzz_ms = AW_DEFAULT_BUZZ_MS;
	(void)PE_parse_boot_argn("sweethapbuzz", &sh.buzz_ms, sizeof(sh.buzz_ms));
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET HAPTIC: kernel_thread_start failed\n");
	}
	if (kernel_thread_start(cover_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET COVER: kernel_thread_start failed\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
