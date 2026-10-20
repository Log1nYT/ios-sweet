/*
 * Venus (Xiaomi Mi 11, SM8350) ST FTS SPI touch bring-up.
 *
 * The touch IC hangs off QUP11 (SE11) on TLMM GPIO 48-51.  The bootloader
 * does not leave GENI SPI firmware for this SE, so this driver bit-bangs
 * mode-0 SPI on those pins as plain GPIOs.  Reset is GPIO 22, IRQ is GPIO 23
 * and the AVDD load switch is GPIO 132.  VDD (PM8350C L2, 1.8 V) is expected
 * to be on with the display rails; the load switch is driven here.
 *
 * ST FTS SPI protocol (not FocalTech FT):
 *   read HW register: 0xFB + 32-bit big-endian address, dummy byte, data
 *   write HW register: 0xFA + 32-bit big-endian address, data
 *   read event FIFO: 0x87, dummy byte, 8-byte events
 *   chip id at 0x20000000 must return 0x36 0x39
 *   events: 0x03 controller ready, 0x13 enter, 0x23 motion, 0x33 leave
 *   event[1] bits 7:4 = finger id, bits 3:0 = touch type
 *
 * The chip runs its firmware in 4-wire SPI mode.  A blank/boot-ROM part
 * starts in 3-wire SPI; the vendor driver switches it to 4-wire by writing
 * ADDR_GPIO_* and ADDR_ICR before reading the chip id.  The same sequence
 * is issued here if the first chip-id probe fails.
 *
 * Boot args:
 *   venusfts=0        disable this driver
 *   venusftspins=N    force one of the 24 pin permutations (0..23)
 *   venusftsdelay=S   start after S seconds, default 45
 *   venusftsdump=0    do not print raw events
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
extern void sweet_hid_touch(int finger, int down, int x, int y);

/* SM8350 GPIO config registers live at 0x0f100000 (the vendor DTB's
 * pinctrl@f000000 aperture starts 0x100000 earlier). Using 0x0f000000
 * reads/writes reserved space: every cfg/io read comes back zero. */
#define VENUS_TLMM_PA        0x0f100000u
#define VENUS_TLMM_SIZE      0x00300000u

#define VENUS_GPIO_AVDD      132u
#define VENUS_GPIO_RESET     22u
#define VENUS_GPIO_IRQ       23u

#define FTS_CMD_HW_REG_R     0xfbu
#define FTS_CMD_HW_REG_W     0xfau
#define FTS_CMD_FIFO         0x87u

#define FTS_ADDR_CHIP_ID     0x20000000u
#define FTS_ADDR_ICR         0x2000002du
#define FTS_ADDR_GPIO_DIR    0x20000032u
#define FTS_ADDR_GPIO_PULL   0x20000034u
#define FTS_ADDR_GPIO_CFG0   0x2000003du
#define FTS_ADDR_GPIO_CFG2   0x2000003fu

#define FTS_CHIP_ID0         0x36u
#define FTS_CHIP_ID1         0x39u

#define FTS_EVT_READY        0x03u
#define FTS_EVT_ENTER        0x13u
#define FTS_EVT_MOTION       0x23u
#define FTS_EVT_LEAVE        0x33u

#define VENUS_FINGERS        10
#define VENUS_PANEL_W        1440
#define VENUS_PANEL_H        3200

struct venus_touch_state {
	vm_offset_t tlmm;
	int miso, mosi, clk, cs;
	int map_index;
	bool started, dead, dump;
	bool fw_alive;
	bool down[VENUS_FINGERS];
	int last_x[VENUS_FINGERS];
	int last_y[VENUS_FINGERS];
	unsigned event_count;
	unsigned error_count;
};

static struct venus_touch_state vt;
static int venus_touch_chan;

static inline uint32_t
vt_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static inline void
vt_wr(vm_offset_t base, uint32_t off, uint32_t val)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = val;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
vt_wait_ms(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&venus_touch_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

static void
vt_gpio_cfg(unsigned pin, unsigned mux, unsigned pull, unsigned drive, bool out, bool high)
{
	uint32_t off = pin * 0x1000u;

	if (out) {
		uint32_t io = vt_rd(vt.tlmm, off + 4u);
		io = high ? (io | 2u) : (io & ~2u);
		vt_wr(vt.tlmm, off + 4u, io);
	}
	uint32_t cfg = vt_rd(vt.tlmm, off);
	cfg &= ~0x3ffu;
	cfg |= (mux << 2) | (pull & 3u) | ((drive & 7u) << 6) | (out ? 0x200u : 0u);
	vt_wr(vt.tlmm, off, cfg);
}

static inline void
vt_set(int pin, bool high)
{
	uint32_t off = (uint32_t)pin * 0x1000u + 4u;
	uint32_t io = vt_rd(vt.tlmm, off);
	io = high ? (io | 2u) : (io & ~2u);
	vt_wr(vt.tlmm, off, io);
}

static inline bool
vt_get(int pin)
{
	return (vt_rd(vt.tlmm, (uint32_t)pin * 0x1000u + 4u) & 1u) != 0;
}

static inline void
vt_spi_delay(void)
{
	delay(1);
}

static uint8_t
vt_spi_xfer(uint8_t out)
{
	uint8_t in = 0;

	for (int i = 7; i >= 0; --i) {
		vt_set(vt.mosi, (out >> i) & 1u);
		vt_spi_delay();
		vt_set(vt.clk, true);
		vt_spi_delay();
		in = (uint8_t)((in << 1) | (vt_get(vt.miso) ? 1u : 0u));
		vt_set(vt.clk, false);
		vt_spi_delay();
	}
	return in;
}

static void
vt_spi_send(const uint8_t *buf, unsigned len)
{
	for (unsigned i = 0; i < len; ++i) {
		(void)vt_spi_xfer(buf[i]);
	}
}

static void
vt_spi_select(bool on)
{
	vt_set(vt.cs, !on);
	vt_spi_delay();
}

static void
vt_read_hw(uint32_t addr, uint8_t *buf, unsigned len)
{
	uint8_t cmd[5] = {
		FTS_CMD_HW_REG_R,
		(uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
		(uint8_t)(addr >> 8), (uint8_t)addr
	};

	vt_spi_select(true);
	vt_spi_send(cmd, sizeof(cmd));
	(void)vt_spi_xfer(0);	/* dummy byte */
	for (unsigned i = 0; i < len; ++i) {
		buf[i] = vt_spi_xfer(0);
	}
	vt_spi_select(false);
}

static void
vt_write_hw(uint32_t addr, uint8_t val)
{
	uint8_t cmd[6] = {
		FTS_CMD_HW_REG_W,
		(uint8_t)(addr >> 24), (uint8_t)(addr >> 16),
		(uint8_t)(addr >> 8), (uint8_t)addr,
		val
	};

	vt_spi_select(true);
	vt_spi_send(cmd, sizeof(cmd));
	vt_spi_select(false);
}

static bool
vt_read_event(uint8_t *ev)
{
	vt_spi_select(true);
	(void)vt_spi_xfer(FTS_CMD_FIFO);
	(void)vt_spi_xfer(0);	/* dummy byte */
	for (unsigned i = 0; i < 8; ++i) {
		ev[i] = vt_spi_xfer(0);
	}
	vt_spi_select(false);
	return ev[0] != 0x00u && ev[0] != 0xffu;
}

static void
vt_pins_idle(void)
{
	vt_gpio_cfg(48, 0, 2, 0, false, false);
	vt_gpio_cfg(49, 0, 2, 0, false, false);
	vt_gpio_cfg(50, 0, 2, 0, false, false);
	vt_gpio_cfg(51, 0, 2, 0, false, false);
	delay(1000);
}

static void
vt_pins_setup(int miso, int mosi, int clk, int cs)
{
	vt.miso = miso;
	vt.mosi = mosi;
	vt.clk = clk;
	vt.cs = cs;
	vt_pins_idle();
	vt_gpio_cfg((unsigned)cs, 0, 3, 6, true, true);
	vt_gpio_cfg((unsigned)clk, 0, 3, 6, true, false);
	vt_gpio_cfg((unsigned)mosi, 0, 3, 6, true, false);
	vt_gpio_cfg((unsigned)miso, 0, 0, 0, false, false);
	delay(10);
}

static void
vt_dump_pins(void)
{
	uint32_t m = (uint32_t)vt.miso * 0x1000u, o = (uint32_t)vt.mosi * 0x1000u;
	uint32_t c = (uint32_t)vt.clk * 0x1000u, s = (uint32_t)vt.cs * 0x1000u;

	printf("SWEET VENUS TOUCH: cfg %#x/%#x/%#x/%#x io %#x/%#x/%#x/%#x\n",
	    vt_rd(vt.tlmm, m), vt_rd(vt.tlmm, o), vt_rd(vt.tlmm, c), vt_rd(vt.tlmm, s),
	    vt_rd(vt.tlmm, m + 4u), vt_rd(vt.tlmm, o + 4u),
	    vt_rd(vt.tlmm, c + 4u), vt_rd(vt.tlmm, s + 4u));
}

static void
vt_reset(void)
{
	vt_set(VENUS_GPIO_RESET, false);
	delay(10000);
	vt_set(VENUS_GPIO_RESET, true);
	delay(300000);
}

/* Blank parts (boot ROM) come up in 3-wire SPI.  This is the vendor
 * fts_spi4_mode_set() sequence; writes work in both 3-wire and 4-wire. */
static void
vt_spi4_mode(void)
{
	vt_write_hw(FTS_ADDR_GPIO_DIR, 0x10);
	vt_write_hw(FTS_ADDR_GPIO_PULL, 0x02);
	vt_write_hw(FTS_ADDR_GPIO_CFG2, 0x07);
	vt_write_hw(FTS_ADDR_GPIO_CFG0, 0x30);
	vt_write_hw(FTS_ADDR_ICR, 0x02);
	delay(10000);
}

static bool
vt_chip_id(void)
{
	uint8_t id[2] = {0, 0};

	for (unsigned t = 0; t < 2; ++t) {
		vt_read_hw(FTS_ADDR_CHIP_ID, id, 2);
		printf("SWEET VENUS TOUCH: chip-id %02x:%02x\n", id[0], id[1]);
		if (id[0] == FTS_CHIP_ID0 && id[1] == FTS_CHIP_ID1) {
			return true;
		}
		delay(10000);
	}
	return false;
}

/*
 * After a hardware reset the running firmware posts a controller-ready
 * event.  Accept any real touch event too, so a touch during probe is not
 * lost.  Raw non-empty events are logged to show partial communication.
 */
static bool
vt_fifo_alive(void)
{
	uint8_t ev[8];
	unsigned logged = 0;

	for (unsigned i = 0; i < 16; ++i) {
		if (vt_read_event(ev)) {
			if (logged < 2) {
				printf("SWEET VENUS TOUCH: fifo %02x %02x %02x %02x %02x %02x %02x %02x\n",
				    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
				logged++;
			}
			if (ev[0] == FTS_EVT_READY) {
				printf("SWEET VENUS TOUCH: controller ready\n");
				return true;
			}
			if (ev[0] == FTS_EVT_ENTER || ev[0] == FTS_EVT_MOTION ||
			    ev[0] == FTS_EVT_LEAVE) {
				return true;
			}
		}
		vt_wait_ms(25);
	}
	return false;
}

/* All 24 role permutations of GPIO 48-51; the two most likely first:
 * QUP SPI data0=MOSI on 48, data1=MISO on 49, clk=50, cs=51. */
static const int vt_maps[24][4] = {
	/* miso, mosi, clk, cs */
	{49, 48, 50, 51},
	{48, 49, 50, 51},
	{49, 48, 51, 50},
	{48, 49, 51, 50},
	{49, 50, 48, 51},
	{50, 49, 48, 51},
	{49, 51, 48, 50},
	{51, 49, 48, 50},
	{49, 50, 51, 48},
	{50, 49, 51, 48},
	{49, 51, 50, 48},
	{51, 49, 50, 48},
	{50, 48, 49, 51},
	{48, 50, 49, 51},
	{50, 51, 49, 48},
	{51, 50, 49, 48},
	{48, 51, 49, 50},
	{51, 48, 49, 50},
	{48, 50, 51, 49},
	{50, 48, 51, 49},
	{48, 51, 50, 49},
	{51, 48, 50, 49},
	{50, 51, 48, 49},
	{51, 50, 48, 49},
};

static bool
vt_probe(void)
{
	int forced = -1;

	(void)PE_parse_boot_argn("venusftspins", &forced, sizeof(forced));
	for (int i = 0; i < 24; ++i) {
		if (forced >= 0 && i != forced) {
			continue;
		}
		printf("SWEET VENUS TOUCH: probing map %d pins %d/%d/%d/%d\n",
		    i, vt_maps[i][0], vt_maps[i][1], vt_maps[i][2], vt_maps[i][3]);
		vt_pins_setup(vt_maps[i][0], vt_maps[i][1], vt_maps[i][2], vt_maps[i][3]);
		vt_dump_pins();
		vt_reset();

		if (vt_fifo_alive()) {
			vt.map_index = i;
			vt.fw_alive = true;
			printf("SWEET VENUS TOUCH: mapping %d selected (fw)\n", i);
			return true;
		}

		if (vt_chip_id()) {
			vt.map_index = i;
			printf("SWEET VENUS TOUCH: mapping %d selected (chip id)\n", i);
			return true;
		}

		/* Boot ROM in 3-wire mode: switch to 4-wire and retry. */
		vt_spi4_mode();
		if (vt_chip_id()) {
			vt.map_index = i;
			printf("SWEET VENUS TOUCH: mapping %d selected (spi4 + chip id)\n", i);
			return true;
		}
		if (vt_fifo_alive()) {
			vt.map_index = i;
			vt.fw_alive = true;
			printf("SWEET VENUS TOUCH: mapping %d selected (spi4 + fw)\n", i);
			return true;
		}
	}
	return false;
}

static void
vt_handle_event(const uint8_t *ev)
{
	unsigned id = ev[0];

	if (vt.dump && vt.event_count < 60) {
		printf("SWEET VENUS TOUCH: ev %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    ev[0], ev[1], ev[2], ev[3], ev[4], ev[5], ev[6], ev[7]);
		vt.event_count++;
	}
	if (id == FTS_EVT_ENTER || id == FTS_EVT_MOTION) {
		/* ST event[1]: bits 7:4 finger id, bits 3:0 touch type. */
		int tid = (ev[1] & 0xf0) >> 4;
		int x = ((ev[3] & 0x0f) << 8) | ev[2];
		int y = (ev[4] << 4) | ((ev[3] & 0xf0) >> 4);
		if (tid >= VENUS_FINGERS || x >= VENUS_PANEL_W || y >= VENUS_PANEL_H) {
			if ((vt.error_count++ % 20u) == 0) {
				printf("SWEET VENUS TOUCH: bad point tid=%d x=%d y=%d\n", tid, x, y);
			}
			return;
		}
		vt.last_x[tid] = x;
		vt.last_y[tid] = y;
		vt.down[tid] = true;
		sweet_hid_touch(tid, 1, x, y);
	} else if (id == FTS_EVT_LEAVE) {
		int tid = (ev[1] & 0xf0) >> 4;
		if (tid >= VENUS_FINGERS) {
			return;
		}
		if (vt.down[tid]) {
			vt.down[tid] = false;
			sweet_hid_touch(tid, 0, vt.last_x[tid], vt.last_y[tid]);
		}
	}
}

static void
venus_touch_thread(void *arg, wait_result_t result)
{
	unsigned delay_s = 45;
	int enabled = 1, dump = 1;
	uint8_t ev[8];

	(void)arg;
	(void)result;
	(void)PE_parse_boot_argn("venusftsdelay", &delay_s, sizeof(delay_s));
	(void)PE_parse_boot_argn("venusftsdump", &dump, sizeof(dump));
	(void)PE_parse_boot_argn("venusfts", &enabled, sizeof(enabled));
	vt.dump = dump != 0;
	if (!enabled) {
		printf("SWEET VENUS TOUCH: disabled (venusfts=0)\n");
		goto out;
	}
	printf("SWEET VENUS TOUCH: thread start (delay %u s)\n", delay_s);
	if (delay_s) {
		vt_wait_ms(delay_s * 1000u);
	}
	if (vt.dead) {
		goto out;
	}
	vt.tlmm = ml_io_map(VENUS_TLMM_PA, VENUS_TLMM_SIZE);
	if (!vt.tlmm) {
		printf("SWEET VENUS TOUCH: TLMM map failed\n");
		goto out;
	}
	printf("SWEET VENUS TOUCH: TLMM=%#x reset=%#x avdd=%#x\n",
	    (unsigned)vt.tlmm, vt_rd(vt.tlmm, VENUS_GPIO_RESET * 0x1000u + 4u),
	    vt_rd(vt.tlmm, VENUS_GPIO_AVDD * 0x1000u + 4u));

	/* avddold (L3C 3.3 V) feeds the avdd switch; vdd (L2C 1.8 V) is the
	 * I/O rail. Both are best-effort votes. */
	extern int sweet_rpmh_vreg_enable(const char *res, unsigned mv);
	int avdd_err = sweet_rpmh_vreg_enable("ldoc3", 3300);
	printf("SWEET VENUS TOUCH: AVDD L3C vote=%d\n", avdd_err);
	int vdd_err = sweet_rpmh_vreg_enable("ldoc2", 1800);
	printf("SWEET VENUS TOUCH: VDD L2C vote=%d\n", vdd_err);

	/* Power sequence from the ST driver: avddold, avdd switch, vdd,
	 * reset low, then release reset. */
	vt_gpio_cfg(VENUS_GPIO_AVDD, 0, 2, 3, true, true);
	vt_gpio_cfg(VENUS_GPIO_RESET, 0, 2, 3, true, false);
	vt_gpio_cfg(VENUS_GPIO_IRQ, 0, 3, 3, false, false);
	delay(1000);
	delay(5000);
	delay(10000);
	vt_set(VENUS_GPIO_RESET, true);
	delay(300000);
	printf("SWEET VENUS TOUCH: pwr rst=%#x/%#x irq=%#x/%#x avdd=%#x/%#x\n",
	    vt_rd(vt.tlmm, VENUS_GPIO_RESET * 0x1000u),
	    vt_rd(vt.tlmm, VENUS_GPIO_RESET * 0x1000u + 4u),
	    vt_rd(vt.tlmm, VENUS_GPIO_IRQ * 0x1000u),
	    vt_rd(vt.tlmm, VENUS_GPIO_IRQ * 0x1000u + 4u),
	    vt_rd(vt.tlmm, VENUS_GPIO_AVDD * 0x1000u),
	    vt_rd(vt.tlmm, VENUS_GPIO_AVDD * 0x1000u + 4u));

	if (!vt_probe()) {
		printf("SWEET VENUS TOUCH: no FTS chip found (irq=%#x); polling map 0\n",
		    vt_rd(vt.tlmm, VENUS_GPIO_IRQ * 0x1000u + 4u));
		vt_pins_setup(vt_maps[0][0], vt_maps[0][1], vt_maps[0][2], vt_maps[0][3]);
	}

	if (!vt.fw_alive) {
		printf("SWEET VENUS TOUCH: no fw event yet; will keep polling\n");
	}

	printf("SWEET VENUS TOUCH: polling FIFO\n");
	while (!vt.dead) {
		for (unsigned n = 0; n < 16; ++n) {
			if (!vt_read_event(ev)) {
				break;
			}
			vt_handle_event(ev);
		}
		vt_wait_ms(10);
	}
out:
	/* Do not tear down the kernel thread on a failed probe; keep it parked
	 * so a bad exit path cannot corrupt scheduler/vm state. */
	printf("SWEET VENUS TOUCH: thread parked\n");
	for (;;) {
		vt_wait_ms(60000);
	}
}

void
sweet_venus_touch_init(void)
{
	thread_t thread;

	if (vt.started) {
		return;
	}
	vt.started = true;
	printf("SWEET VENUS TOUCH: init (ST FTS SPI on TLMM 48-51)\n");
	kern_return_t kr = kernel_thread_start(venus_touch_thread, NULL, &thread);
	if (kr == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		printf("SWEET VENUS TOUCH: kernel_thread_start failed %d\n", kr);
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
