/*
 * SWEET GPU probe - venus (Xiaomi Mi 11, SM8350, Adreno 660, KGSL class).
 *
 * WHAT THIS IS: a read only probe that proves the GPU silicon is there and
 * says what it is.  It maps the KGSL 3D AHB aperture and the GPU_CC clock
 * controller, reads the GPU identity bytes, the clock gates and the power
 * domain status, and prints one compact line every 5 s.
 *
 * WHAT THIS IS NOT: a driver.  It does not program a command stream, it
 * does not enable a clock, it does not vote a rail, it does not publish an
 * IOKit service and it does not touch the iOS userland.  The iOS UI keeps
 * running on the CPU rasteriser; nothing here can change that (see
 * logs/research/07-gpu-plan.md, section 4.3: publishing an accelerator
 * service would move CoreAnimation onto the Metal path and freeze the UI).
 *
 * READ ONLY BY DESIGN.  There is not a single store into any GPU register
 * in this file.  In particular the GX soft reset (0x03d91008), the GDSC
 * control registers (0x03d9100c, 0x03d9106c), the CX hardware control
 * (0x03d91540) and the GPU_CC clock gates are only ever read, and the
 * GX soft reset is not read either: a stray write to any of them can hang
 * the whole SoC.
 *
 * Boot args (all optional):
 *   sweetgpu=1          enable the probe (default: off)
 *   sweetgpuaddr=<hex>  override the AHB window base (default 0x03d00000)
 *   sweetgpuprobe=1     print the summary every 5 s (default: on when
 *                       sweetgpu=1; 0 turns the periodic line off and keeps
 *                       the one shot dump at init)
 *
 * The probe never spins: it sleeps in tsleep() and never waits for an
 * interrupt.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include "sweet_gpu.h"

/* osfmk/arm/machine_routines.h is not exported to the BSD layer. */
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;

static vm_offset_t gpu_ahb_base;   /* kgsl_3d0_reg_memory, 256 KiB */
static vm_offset_t gpu_cc_base;    /* qcom,lahaina-gpucc, 36 KiB   */
static uint32_t gpu_ahb_pa = SWEET_GPU_AHB_PA;
static uint32_t gpu_cc_pa = SWEET_GPU_CC_PA;
static uint32_t gpu_chipid = SWEET_GPU_CHIPID;
static unsigned gpu_probe_secs = SWEET_GPU_PROBE_SECS;
static int gpu_probe_chan;

/*
 * One 32 bit read of a device register, ordered after the previous
 * access.  Read only: no store ever reaches this address space.
 */
static uint32_t
gpu_rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* "0x3d00000", "3d00000" and "0X3D00000" are all accepted. */
static bool
gpu_parse_hex(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	unsigned digits = 0;

	if (!s) {
		return false;
	}
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
	}
	for (; *s != '\0'; ++s) {
		uint32_t d;

		if (*s >= '0' && *s <= '9') {
			d = (uint32_t)(*s - '0');
		} else if (*s >= 'a' && *s <= 'f') {
			d = (uint32_t)(*s - 'a') + 10u;
		} else if (*s >= 'A' && *s <= 'F') {
			d = (uint32_t)(*s - 'A') + 10u;
		} else {
			return false;
		}
		if (++digits > 8) {
			return false;
		}
		v = (v << 4) | d;
	}
	if (digits == 0) {
		return false;
	}
	*out = v;
	return true;
}

/*
 * The compact summary line.  Everything in it is read fresh, so a change
 * of the clock or power state shows up in the next line.
 */
static void
sweet_gpu_summary(void)
{
	uint32_t id0, id1, ahb_clk, gmu_clk, rcg_cmd, rcg_cfg;
	uint32_t gx_st, cx_hw;
	unsigned div, src, rcg_on;

	id0 = gpu_rd(gpu_ahb_base, 0);
	id1 = gpu_rd(gpu_ahb_base, 4);
	ahb_clk = gpu_rd(gpu_cc_base, SWEET_GPU_CC_AHB_CLK);
	gmu_clk = gpu_rd(gpu_cc_base, SWEET_GPU_CC_GMU_CLK);
	rcg_cmd = gpu_rd(gpu_cc_base, SWEET_GPU_CC_GMU_RCG2);
	rcg_cfg = gpu_rd(gpu_cc_base, SWEET_GPU_CC_GMU_RCG2 + 4);
	div = (unsigned)(rcg_cfg & SWEET_GPU_RCG_DIV_MASK) + 1u;
	src = (unsigned)((rcg_cfg >> SWEET_GPU_RCG_SRC_SHIFT) & SWEET_GPU_RCG_SRC_MASK);
	rcg_on = (rcg_cmd & SWEET_GPU_RCG_ROOT_EN) ? 1u : 0u;
	gx_st = gpu_rd(gpu_cc_base, SWEET_GPU_CC_GX_GDSC + 4);
	cx_hw = gpu_rd(gpu_cc_base, SWEET_GPU_CC_CX_HW_CTRL);

	printf("SWEET GPU: chip-id=0x%08x(%u.%u.%u.%u) ahb=0x%08x id0=0x%08x "
	    "id1=0x%08x gpucc=0x%08x clk_ahb=%u clk_gmu=%u gmu_rcg=%u gmu_div=%u "
	    "gmu_src=%u gx=%s cx=%s freq=GMU\n",
	    gpu_chipid,
	    (unsigned)SWEET_GPU_CHIPID_CORE(gpu_chipid),
	    (unsigned)SWEET_GPU_CHIPID_MAJOR(gpu_chipid),
	    (unsigned)SWEET_GPU_CHIPID_MINOR(gpu_chipid),
	    (unsigned)SWEET_GPU_CHIPID_PATCH(gpu_chipid),
	    gpu_ahb_pa, id0, id1, gpu_cc_pa,
	    ahb_clk & 1u, gmu_clk & 1u, rcg_on, div, src,
	    (gx_st & SWEET_GDSCR_CFG_PWR_UP) ? "on" : "off",
	    (cx_hw & SWEET_GDSCR_PWR_ON) ? "on" : "off");
}

/*
 * The periodic probe.  It only sleeps and reads; it never spins and it
 * never waits for an interrupt.
 */
static void sweet_gpu_probe_thread(__unused void *arg,
    __unused wait_result_t wr) __attribute__((noreturn));

static void
sweet_gpu_probe_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		(void)tsleep(&gpu_probe_chan, PZERO, "sweetgpu",
		    hz * (int)gpu_probe_secs);
		sweet_gpu_summary();
	}
}

/*
 * One shot dump at init: the identity bytes of the GPU and the state of
 * the clock controller and of the two power domains.
 */
static void
sweet_gpu_dump(void)
{
	unsigned i;

	/*
	 * The clock controller and the power domains come first: they are
	 * safe to read, and they tell us whether the GPU is powered at all
	 * before the first read of the GPU aperture itself.
	 */
	printf("SWEET GPU: gpucc ahb_clk=0x%08x gmu_clk=0x%08x gmu_rcg=0x%08x/0x%08x "
	    "hub_rcg=0x%08x/0x%08x gx_gdsc=0x%08x/0x%08x cx_gdsc=0x%08x cx_hw=0x%08x "
	    "gx_domain=0x%08x\n",
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_AHB_CLK),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_GMU_CLK),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_GMU_RCG2),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_GMU_RCG2 + 4),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_HUB_RCG2),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_HUB_RCG2 + 4),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_GX_GDSC),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_GX_GDSC + 4),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_CX_GDSC),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_CX_HW_CTRL),
	    gpu_rd(gpu_cc_base, SWEET_GPU_CC_GX_DOMAIN));
	printf("SWEET GPU: ahb[0..15]=");
	for (i = 0; i < 16; ++i) {
		printf("%08x%s", gpu_rd(gpu_ahb_base, i * 4),
		    (i == 15) ? "" : " ");
	}
	printf("\n");
	printf("SWEET GPU: id bytes[0..7]=");
	for (i = 0; i < 8; ++i) {
		printf("%02x%s", gpu_rd(gpu_ahb_base, i) & 0xffu,
		    (i == 7) ? "" : " ");
	}
	printf("\n");
	/*
	 * The chip id of an a6xx is not a plain register: CAF KGSL and
	 * upstream msm both take it from the device tree
	 * ("qcom,chipid = 0x06060001").  The dump above is the raw
	 * hardware truth, so look for the value in it anyway: if the
	 * window answers, the value has to be somewhere in it.
	 */
	for (i = 0; i < 64; ++i) {
		uint32_t v = gpu_rd(gpu_ahb_base, i * 4);

		if (v == SWEET_GPU_CHIPID) {
			printf("SWEET GPU: chip-id 0x%08x found at ahb+0x%x\n",
			    v, i * 4);
			break;
		}
	}
}

int
sweet_gpu_init(void)
{
	uint32_t enabled = 0;
	uint32_t probe = 0;
	char addr[32];
	thread_t thread;

	if (!PE_parse_boot_argn("sweetgpu", &enabled, sizeof(enabled)) || !enabled) {
		/* Default off: the probe must never run unless it is asked for. */
		return 0;
	}
	if (PE_parse_boot_argn("sweetgpuaddr", addr, sizeof(addr))) {
		uint32_t pa;

		if (gpu_parse_hex(addr, &pa) && pa != 0) {
			gpu_ahb_pa = pa;
		} else {
			printf("SWEET GPU: bad sweetgpuaddr=\"%s\", keeping 0x%08x\n",
			    addr, gpu_ahb_pa);
		}
	}
	if (PE_parse_boot_argn("sweetgpuprobe", &probe, sizeof(probe))) {
		probe = (probe != 0);
	} else {
		probe = 1;
	}
	gpu_ahb_base = ml_io_map(gpu_ahb_pa, SWEET_GPU_AHB_SIZE);
	gpu_cc_base = ml_io_map(gpu_cc_pa, SWEET_GPU_CC_SIZE);
	if (!gpu_ahb_base || !gpu_cc_base) {
		printf("SWEET GPU: mapping failed ahb=0x%08x gpucc=0x%08x\n",
		    gpu_ahb_pa, gpu_cc_pa);
		return -1;
	}
	printf("SWEET GPU: probe start ahb=0x%08x gpucc=0x%08x chip-id=0x%08x "
	    "(read only, no command stream, no clock change)\n",
	    gpu_ahb_pa, gpu_cc_pa, gpu_chipid);
	sweet_gpu_dump();
	sweet_gpu_summary();
	if (probe) {
		if (kernel_thread_start(sweet_gpu_probe_thread, NULL, &thread) ==
		    KERN_SUCCESS) {
			thread_deallocate(thread);
			printf("SWEET GPU: summary every %u s\n", gpu_probe_secs);
		}
	}
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
