/*
 * SWEET GICv3 distributor probe - venus (Xiaomi Mi 11, SM8350, Cortex-A78).
 *
 * WHAT THIS IS: a read only probe of the GICv3 distributor and of the
 * redistributor frames.  It maps the distributor frame (0x17a00000, 64 KiB)
 * and the redistributor region (0x17a60000, 1 MiB, eight 128 KiB PE frames)
 * - both the addresses and the stride come from the live device tree
 * (/soc/interrupt-controller@17a00000 of venus.dtb) - and it prints what the
 * GIC says about itself: the type register, the number of implemented
 * interrupts, whether the GIC has two security states, whether LPIs exist,
 * how many redistributor frames are real, and whether the distributor has
 * the Group 1 (non secure) interrupts enabled.
 *
 * WHAT THIS IS NOT: a GIC driver.  It does not enable, disable, route,
 * mask, acknowledge or EOI a single interrupt, it does not program an LPI
 * property or pending table, and it does not touch ICC_*.  The kernel keeps
 * using the Group 1 NS path that pe_fiq.c set up (GICR_IGROUPR0,
 * GICR_ISENABLER0, ICC_IGRPEN1_EL1, sleh_irq -> ICC_IAR1_EL1).
 *
 * READ ONLY BY DESIGN.  There is not a single store into any GIC register
 * in this file.  A stray write to GICD_CTLR can wedge the interrupt path
 * of the whole system (the distributor stops accepting the group enable
 * bits and the CPU never sees another interrupt again), and a write to
 * GICD_IGROUPR or GICD_IGRPMODR can silently move an interrupt into the
 * secure world, where this kernel can never acknowledge it.  Both are
 * unrecoverable without a reboot, so this probe only ever reads.
 *
 * Boot args (all optional):
 *   sweetgic=1     enable the probe (default: off)
 *   sweetgiclpi=1  also map the LPI pending table and count the pending
 *                  LPIs (default: off - it maps a RAM page that the
 *                  bootloader owns, so it waits for an explicit request)
 *
 * Evidence for every address and every register offset used here is in
 * logs/research/impl/13-gic-smmu-probe.md.
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

/*
 * Live device tree: /soc/interrupt-controller@17a00000
 *   compatible = "arm,gic-v3";
 *   reg = <0x17a00000 0x10000 0x17a60000 0x100000>;
 *   redistributor-stride = <0x00 0x20000>;
 *   #redistributor-regions = <0x01>;
 */
#define GIC_GICD_PA       0x17a00000u
#define GIC_GICD_SIZE     0x00010000u
#define GIC_GICR_PA       0x17a60000u
#define GIC_GICR_SIZE     0x00100000u
#define GIC_GICR_PE_SIZE  0x00020000u   /* redistributor-stride */
#define GIC_MAX_PES       8u            /* 1 MiB / 128 KiB */

/*
 * Distributor register offsets.  Arm IHI 0069 (GICv3), as implemented by
 * Linux (include/linux/irqchip/arm-gic-v3.h) and by QEMU
 * (hw/intc/gicv3_internal.h); the two agree on every offset used here.
 * GICD_CTLR, GICR_TYPER, GICR_WAKER and the GICR frame offsets come from
 * the board configuration (pexpert/pexpert/arm64/SWEET.h) that pe_fiq.c
 * uses, so the probe and the kernel cannot drift apart.
 */
#if !defined(GICD_CTLR) || !defined(GICR_TYPER) || !defined(GICR_WAKER)
#error "the SWEET board config must define the GICv3 registers"
#endif
#define GICD_TYPER        0x0004u
#define GICD_IIDR         0x0008u
#define GICD_TYPER2       0x000cu
#define GICD_STATUSR      0x0010u
#define GICD_IGROUPR      0x0080u
#define GICD_ISENABLER    0x0100u
#define GICD_ISPENDR      0x0200u
#define GICD_IGRMODR      0x0d00u

/* GICD_CTLR bits (non secure view, DS == 0). */
#define GICD_CTLR_ENABLE_G1    GICD_CTLR_ENABLEGRP1
#define GICD_CTLR_ENABLE_G1A   GICD_CTLR_ENABLEGRP1A
#define GICD_CTLR_DS           0x00000040u

/* GICD_TYPER fields. */
#define GICD_TYPER_NMI         0x00000200u  /* bit 9  */
#define GICD_TYPER_SECURITY    0x00000400u  /* bit 10, SecurityExtn */
#define GICD_TYPER_ESPI        0x00000100u  /* bit 8  */
#define GICD_TYPER_LPI_IDBITS(t) (((((t) >> 11) & 0x1fu) + 1u))
#define GICD_TYPER_MBIS        0x00010000u  /* bit 16 */
#define GICD_TYPER_LPIS        0x00020000u  /* bit 17 */
#define GICD_TYPER_ID_BITS(t)  (((((t) >> 19) & 0x1fu) + 1u))
#define GICD_TYPER_INTIDS(t)   (((((t) & 0x1fu) + 1u) * 32u))

/*
 * Redistributor register offsets (Arm IHI 0069, chapter 3.4).  A PE frame
 * is 128 KiB: the redistributor registers live in the first 64 KiB, the SGI
 * (and, in GICv4, the VLPI) frame in the second one.  pe_fiq.c and the
 * bare metal shim both use the same SGI frame offset.
 */
#define GICR_CTLR         0x0000u
#define GICR_IIDR         0x0004u
#define GICR_STATUSR      0x0010u
#define GICR_PROPBASER    0x0070u
#define GICR_PENDBASER    0x0078u

/* GICR_CTLR bits. */
#define GICR_CTLR_ENABLE_LPIS   0x00000001u
#define GICR_CTLR_CES           0x00000002u
#define GICR_CTLR_IR            0x00000004u
#define GICR_CTLR_RWP           0x00000008u

/* GICR_TYPER fields. */
#define GICR_TYPER_PLPIS        0x0000000000000001ull
#define GICR_TYPER_VLPIS        0x0000000000000002ull
#define GICR_TYPER_DIRECT_LPIS  0x0000000000000008ull
#define GICR_TYPER_AFFINITY     0xffffffff00000000ull

/* GICR_WAKER bits. */
#define GICR_WAKER_SLEEP        0x00000002u
#define GICR_WAKER_CHILDREN     0x00000004u

/* GICR_PROPBASER / GICR_PENDBASER fields. */
#define GICR_BASER_IDBITS       0x0000001fu
#define GICR_PROPBASER_ADDR     0x000ffffffffff000ull
#define GICR_PENDBASER_ADDR     0x000fffffffff0000ull

/* The two interrupts the GPU work needs (venus.dtb). */
#define GIC_INTID_GPU           300u   /* qcom,kgsl-3d0 interrupts */
#define GIC_INTID_SMMU_FIRST    672u   /* kgsl-smmu interrupts[0] */
#define GIC_INTID_SMMU_LAST     681u   /* kgsl-smmu interrupts[9] */

/* The virtual timer of venus (arm,armv8-timer: virtual = PPI 11). */
#define GIC_INTID_VTIMER        27u
#define GIC_INTID_SGI           1u    /* SWEET_IPI_SGI */

/* The prototype of the entry point, so the file compiles without warnings. */
void sweet_gic_init(void);

static vm_offset_t gicd_base;
static vm_offset_t gicr_base;

/* One 32 bit read of a GIC register, ordered after the previous access. */
static uint32_t
gic_rd32(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* One 64 bit read (GICR_TYPER, GICR_PROPBASER, ...). */
static uint64_t
gic_rd64(vm_offset_t base, uint32_t off)
{
	uint64_t v = *(volatile uint64_t *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* The enable and the pending bit of one interrupt ID, read only. */
static unsigned
gic_irq_state(uint32_t intid, uint32_t reg, uint32_t *value)
{
	uint32_t word, bit, v;

	if (intid < 32u) {
		/* SGIs and PPIs live in the redistributor, not here. */
		return 0;
	}
	word = (intid - 32u) / 32u;
	bit = (intid - 32u) % 32u;
	v = gic_rd32(gicd_base, reg + 4u * word);
	if (value) {
		*value = v;
	}
	return (v >> bit) & 1u;
}

/*
 * The LPI pending table lives in RAM, not in the GIC registers: GICR_PENDBASER
 * points at it.  Its first byte is the "LPI pending group" field, the pending
 * bits start at byte 8 (one bit per LPI, LPI n at bit n % 8 of byte
 * 8 + n / 8).  Reading it is optional and off by default.
 */
static void
gic_lpi_pending(uint64_t pendbaser, unsigned idbits)
{
	uint64_t addr = pendbaser & GICR_PENDBASER_ADDR;
	unsigned nlpi, i, pending = 0;
	vm_offset_t table;

	if (!addr || (addr & 0xffffu) || (addr >> 32) != 0) {
		printf("SWEET GIC: LPI pending table address 0x%llx is not usable\n",
		    (uint64_t)addr);
		return;
	}
	nlpi = 1u << idbits;
	if (nlpi > 65536u) {
		nlpi = 65536u;
	}
	table = ml_io_map((vm_offset_t)addr, 0x10000);
	if (!table) {
		printf("SWEET GIC: LPI pending table 0x%llx could not be mapped\n",
		    (uint64_t)addr);
		return;
	}
	for (i = 0; i < nlpi; ++i) {
		uint32_t byte = gic_rd32(table, 8u + (i / 8u));

		if ((byte >> (i % 8u)) & 1u) {
			++pending;
		}
	}
	printf("SWEET GIC: LPI pending table 0x%llx: %u of %u LPIs pending\n",
	    (uint64_t)addr, pending, nlpi);
}

void
sweet_gic_init(void)
{
	uint32_t enabled = 0, lpi = 0;
	uint32_t typer, typer4, typer8, iidr, ctlr, statusr, grp, grpmod;
	uint64_t typer64, propbaser = 0, pendbaser = 0;
	uint32_t my_aff;
	unsigned nr_redis = 0, secure = 0, lpi_count = 0, lpi_idbits = 0;
	unsigned sleep_pes = 0, awake_pes = 0, unimplemented = 0;
	unsigned i;

	if (!PE_parse_boot_argn("sweetgic", &enabled, sizeof(enabled)) || !enabled) {
		/* Default off: the probe must never run unless it is asked for. */
		return;
	}
	if (PE_parse_boot_argn("sweetgiclpi", &lpi, sizeof(lpi))) {
		lpi = (lpi != 0);
	} else {
		lpi = 0;
	}
	gicd_base = ml_io_map(GIC_GICD_PA, GIC_GICD_SIZE);
	gicr_base = ml_io_map(GIC_GICR_PA, GIC_GICR_SIZE);
	if (!gicd_base || !gicr_base) {
		printf("SWEET GIC: mapping failed gicd=0x%08x gicr=0x%08x\n",
		    GIC_GICD_PA, GIC_GICR_PA);
		return;
	}
	/* The affinity of the PE that runs the probe (aff0, aff1, aff2). */
	my_aff = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") & 0x00ffffffu);
	printf("SWEET GIC: probe start gicd=0x%08x gicr=0x%08x stride=0x%x "
	    "(read only, no GIC register is written)\n",
	    GIC_GICD_PA, GIC_GICR_PA, GIC_GICR_PE_SIZE);

	/*
	 * The identity window.  Linux and QEMU put GICD_TYPER at 0x004 and
	 * GICD_IIDR at 0x008; some older descriptions of the GICv3 map put
	 * GICD_TYPER at 0x008.  Both words are printed raw and the type
	 * register is taken from the offset that answers with a sane value
	 * (a non zero ITLinesNumber that fits the distributor).
	 */
	typer4 = gic_rd32(gicd_base, GICD_TYPER);
	typer8 = gic_rd32(gicd_base, GICD_IIDR);
	printf("SWEET GIC: ident ctlr=0x%08x type@0x004=0x%08x word@0x008=0x%08x "
	    "typer2=0x%08x statusr=0x%08x\n",
	    gic_rd32(gicd_base, GICD_CTLR), typer4, typer8,
	    gic_rd32(gicd_base, GICD_TYPER2), gic_rd32(gicd_base, GICD_STATUSR));
	typer = typer4;
	if (((typer4 & 0x1fu) == 0u || (typer4 & 0x1fu) > 30u) &&
	    ((typer8 & 0x1fu) != 0u && (typer8 & 0x1fu) <= 30u)) {
		printf("SWEET GIC: GICD_TYPER answers at 0x008, not at 0x004\n");
		typer = typer8;
	}
	iidr = gic_rd32(gicd_base, GICD_IIDR);
	ctlr = gic_rd32(gicd_base, GICD_CTLR);
	statusr = gic_rd32(gicd_base, GICD_STATUSR);
	secure = (typer & GICD_TYPER_SECURITY) ? 1u : 0u;
	printf("SWEET GIC: ctlr=0x%08x (ARE_NS=%u EnableGrp1=%u EnableGrp1A=%u "
	    "DS=%u RWP=%u) iidr=0x%08x statusr=0x%08x\n",
	    ctlr, (ctlr & GICD_CTLR_ARE_NS) ? 1u : 0u,
	    (ctlr & GICD_CTLR_ENABLE_G1) ? 1u : 0u,
	    (ctlr & GICD_CTLR_ENABLE_G1A) ? 1u : 0u,
	    (ctlr & GICD_CTLR_DS) ? 1u : 0u,
	    (ctlr & GICD_CTLR_RWP) ? 1u : 0u, iidr, statusr);
	printf("SWEET GIC: intids=%u idbits=%u lpi_idbits=%u espi=%u mbis=%u "
	    "nmi=%u\n",
	    GICD_TYPER_INTIDS(typer), GICD_TYPER_ID_BITS(typer),
	    (typer & GICD_TYPER_LPIS) ? GICD_TYPER_LPI_IDBITS(typer) : 0u,
	    (typer & GICD_TYPER_ESPI) ? 1u : 0u,
	    (typer & GICD_TYPER_MBIS) ? 1u : 0u,
	    (typer & GICD_TYPER_NMI) ? 1u : 0u);

	/*
	 * The group of the interrupts.  In the non secure view of a GIC with
	 * two security states GICD_IGROUPR reads as zero even when the
	 * interrupts are really Group 1, so the raw words are printed and
	 * the state is taken from the enable/pending registers instead.
	 */
	grp = gic_rd32(gicd_base, GICD_IGROUPR);
	grpmod = gic_rd32(gicd_base, GICD_IGRMODR);
	printf("SWEET GIC: igroupr[0]=0x%08x igrmodr[0]=0x%08x (NS view: RAZ is "
	    "normal, the group is not readable from here)\n", grp, grpmod);
	for (i = GIC_INTID_GPU; i <= GIC_INTID_SMMU_LAST; i++) {
		uint32_t en = 0, pend = 0;
		unsigned en_bit, pend_bit;

		if (i != GIC_INTID_GPU && i != GIC_INTID_SMMU_FIRST &&
		    i != GIC_INTID_SMMU_LAST) {
			continue;
		}
		en_bit = gic_irq_state(i, GICD_ISENABLER, &en);
		pend_bit = gic_irq_state(i, GICD_ISPENDR, &pend);
		printf("SWEET GIC: intid %u enabled=%u pending=%u\n",
		    i, en_bit, pend_bit);
	}

	/*
	 * The redistributor frames.  A frame that is not implemented reads
	 * back as all ones; the affinity value in GICR_TYPER[63:32] says
	 * which PE it belongs to.
	 */
	for (i = 0; i < GIC_MAX_PES; ++i) {
		vm_offset_t frame = gicr_base + (vm_offset_t)i * GIC_GICR_PE_SIZE;
		uint32_t waker, ctlr_r;

		typer64 = gic_rd64(frame, GICR_TYPER);
		if (typer64 == 0xffffffffffffffffull || typer64 == 0) {
			++unimplemented;
			continue;
		}
		++nr_redis;
		waker = gic_rd32(frame, GICR_WAKER);
		ctlr_r = gic_rd32(frame, GICR_CTLR);
		if (waker & (GICR_WAKER_SLEEP | GICR_WAKER_CHILDREN)) {
			++sleep_pes;
		} else {
			++awake_pes;
		}
		printf("SWEET GIC: redis %u typer=0x%016llx affinity=0x%llx "
		    "cpunum=%u last=%u plpis=%u vlpis=%u waker=0x%08x "
		    "(sleep=%u children=%u) ctlr=0x%08x (lpis=%u)\n",
		    i, (uint64_t)typer64,
		    (uint64_t)((typer64 & GICR_TYPER_AFFINITY) >> 32),
		    (unsigned)((typer64 >> 8) & 0xffffu),
		    (typer64 & GICR_TYPER_LAST) ? 1u : 0u,
		    (typer64 & GICR_TYPER_PLPIS) ? 1u : 0u,
		    (typer64 & GICR_TYPER_VLPIS) ? 1u : 0u,
		    waker, (waker & GICR_WAKER_SLEEP) ? 1u : 0u,
		    (waker & GICR_WAKER_CHILDREN) ? 1u : 0u,
		    ctlr_r, (ctlr_r & GICR_CTLR_ENABLE_LPIS) ? 1u : 0u);
		if (nr_redis == 1) {
			propbaser = gic_rd64(frame, GICR_PROPBASER);
			pendbaser = gic_rd64(frame, GICR_PENDBASER);
			lpi_idbits = (unsigned)(propbaser & GICR_BASER_IDBITS);
			if ((typer64 & GICR_TYPER_PLPIS) && lpi_idbits <= 16u) {
				lpi_count = 1u << lpi_idbits;
			}
			printf("SWEET GIC: LPI tables prop=0x%llx (idbits=%u) "
			    "pend=0x%llx (idbits=%u) lpis=%u\n",
			    (uint64_t)propbaser, lpi_idbits,
			    (uint64_t)pendbaser,
			    (unsigned)(pendbaser & GICR_BASER_IDBITS),
			    lpi_count);
			if (lpi && (typer64 & GICR_TYPER_PLPIS) && lpi_idbits) {
				gic_lpi_pending(pendbaser, lpi_idbits);
			}
		}
		/*
		 * The redistributor of the PE that runs this probe: its SGI
		 * frame holds the group and the enable state of the SGIs and
		 * the PPIs, which is what pe_fiq.c programmed (timer and SGI
		 * enabled, everything Group 1 non secure).
		 */
		if (((typer64 >> 32) & 0x00ffffffu) == my_aff) {
			uint32_t group0 = gic_rd32(frame, GICR_IGROUPR0);
			uint32_t enable0 = gic_rd32(frame, GICR_ISENABLER0);

			printf("SWEET GIC: this PE igroupr0=0x%08x isenabler0=0x%08x "
			    "(sgi1=%u vtimer%u=%u)\n",
			    group0, enable0, (enable0 >> GIC_INTID_SGI) & 1u,
			    GIC_INTID_VTIMER, (enable0 >> GIC_INTID_VTIMER) & 1u);
		}
	}
	printf("SWEET GIC: type=0x%08x lpi=%u nr_redis=%u secure=%u "
	    "(frames: %u awake, %u asleep, %u not implemented)\n",
	    typer, lpi_count, nr_redis, secure, awake_pes, sleep_pes,
	    unimplemented);
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
