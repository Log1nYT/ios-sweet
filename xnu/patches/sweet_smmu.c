/*
 * SWEET SMMU capability probe - venus (Xiaomi Mi 11, SM8350, Adreno 660).
 *
 * WHAT THIS IS: a read only probe of the GPU's IOMMU, the Qualcomm
 * QSMMU v5 ("qcom,qsmmu-v500") that guards the Adreno 660 address space.
 * It maps the SMMU global register window (0x3da0000, 128 KiB), the TCU
 * ("client interface", 0x3dc2000), the two TBU debug windows
 * (0x3dc5000 and 0x3dc9000) and the two TBU status registers
 * (0x3dc2200 and 0x3dc2208) - every address comes from the live device
 * tree (/soc/kgsl-smmu@3da0000 of venus.dtb) - and it prints what the
 * hardware says about itself: the revision, the configuration registers,
 * the number of stream match registers and context banks, the translation
 * table geometry and the state of the two TBUs.
 *
 * WHAT THIS IS NOT: an SMMU driver.  It programs no page table, no
 * stream match register, no context bank and no command queue.  The
 * device tree says "qcom,skip-init": the SMMU is configured by the
 * bootloader and the secure world, and touching it here would fight
 * them.  A real GPU driver needs a client: the Adreno has to be a bus
 * master of the SMMU, and only then can a page table be built for it.
 *
 * READ ONLY BY DESIGN.  There is not a single store into any SMMU, TCU or
 * TBU register in this file.  Writing SMMU_CR0 (enable/disable), the
 * stream match registers or a context bank control register can lock the
 * GPU out of memory (or leave a half programmed translation table behind
 * that the secure world then has to clean up), and the global TLB
 * invalidation registers (TLBIVMID, TLBIALLH, sTLBGSYNC) are writes that
 * affect every client of the SMMU.
 *
 * Boot args (all optional):
 *   sweet_smmu=1     enable the probe (default: off)
 *   sweet_smmudump=1 also print the stream match and context bank
 *                    registers of the first banks (default: off, they are
 *                    a lot of numbers)
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
 * Live device tree: /soc/kgsl-smmu@3da0000
 *   compatible = "qcom,qsmmu-v500";
 *   reg = <0x3da0000 0x20000 0x3dc2000 0x20>;
 *   reg-names = "base", "tcu-base";
 *   #global-interrupts = <0x02>;
 *   interrupts = <0x00 0x2a0 0x04 ... 0x00 0x2af 0x04>;   (SPI 672..681)
 *   qcom,skip-init; qcom,use-3-lvl-tables; qcom,split-tables;
 *   gfx_0_tbu@3dc5000: reg = <0x3dc5000 0x1000 0x3dc2200 0x08>
 *   gfx_1_tbu@3dc9000: reg = <0x3dc9000 0x1000 0x3dc2208 0x08>
 */
#define SMMU_BASE_PA        0x03da0000u
#define SMMU_BASE_SIZE      0x00020000u
#define SMMU_TCU_PA         0x03dc2000u
#define SMMU_TCU_SIZE       0x00000020u
#define SMMU_TBU0_PA        0x03dc5000u
#define SMMU_TBU0_SIZE      0x00001000u
#define SMMU_TBU1_PA        0x03dc9000u
#define SMMU_TBU1_SIZE      0x00001000u
#define SMMU_TBU_STAT0_PA   0x03dc2200u
#define SMMU_TBU_STAT1_PA   0x03dc2208u
#define SMMU_TBU_STAT_SIZE  0x00000008u

/*
 * The QSMMU v5 is driven by the ARM SMMU v2 (MMU-500/600) driver: CAF's
 * arm-smmu.c carries the qsmmuv500_* code and upstream Linux drives it
 * with drivers/iommu/arm/arm-smmu/arm-smmu-qcom.c ("SMMU-500 TRM").  The
 * global register map is therefore the SMMU v2 one:
 *   0x000 CR0   0x004 CR1   0x008 CR2   0x00c CR3   0x010 ACR
 *   0x020 ID0   0x024 ID1   0x028 ID2   ...  0x03c ID7  (version)
 *   0x048 GFSR  0x050 GFSYNR0 0x054 GFSYNR1 0x058 GFSYNR2
 *   0x064 TLBIVMID 0x068 TLBIALLNSNH 0x06c TLBIALLH 0x070 sTLBGSYNC
 *   0x074 sTLBGSTATUS
 *   0x800 + 4n SMRn        0xc00 + 4n S2CRn
 * The SMMU v3 map (used by the SMMUv3 driver) has its ID registers at
 * 0x000 and its CR0 at 0x020; the probe prints both candidates and the
 * identification registers decide which one is real.
 */
#define SMMU_CR0            0x000u
#define SMMU_CR1            0x004u
#define SMMU_CR2            0x008u
#define SMMU_CR3            0x00cu
#define SMMU_ACR            0x010u
#define SMMU_ID0            0x020u
#define SMMU_ID1            0x024u
#define SMMU_ID2            0x028u
#define SMMU_ID3            0x02cu
#define SMMU_ID4            0x030u
#define SMMU_ID5            0x034u
#define SMMU_ID6            0x038u
#define SMMU_ID7            0x03cu   /* version: MAJOR[7:4], MINOR[3:0] */
#define SMMU_GFSR           0x048u
#define SMMU_GFSYNR0        0x050u
#define SMMU_GFSYNR1        0x054u
#define SMMU_GFSYNR2        0x058u
#define SMMU_TLBGSTATUS     0x074u
#define SMMU_SMR(n)         (0x800u + ((n) << 2))
#define SMMU_S2CR(n)        (0xc00u + ((n) << 2))

/* The SMMU v3 candidates, printed so the map can be confirmed on the die. */
#define SMMU_V3_IDR0        0x000u
#define SMMU_V3_IIDR        0x018u
#define SMMU_V3_AIDR        0x01cu
#define SMMU_V3_CR0         0x020u

/* SMMU v2 identification register fields (Linux arm-smmu.h). */
#define SMMU_ID0_NUMSMRG(t)   ((t) & 0xffu)
#define SMMU_ID0_NUMSIDB(t)   (((t) >> 9) & 0xfu)
#define SMMU_ID0_CTTW(t)      (((t) >> 14) & 1u)
#define SMMU_ID0_NUMIRPT(t)   (((t) >> 16) & 0xffu)
#define SMMU_ID0_ATOSNS(t)    (((t) >> 26) & 1u)
#define SMMU_ID0_SMS(t)       (((t) >> 27) & 1u)
#define SMMU_ID0_NTS(t)       (((t) >> 28) & 1u)
#define SMMU_ID0_S2TS(t)      (((t) >> 29) & 1u)
#define SMMU_ID0_S1TS(t)      (((t) >> 30) & 1u)
#define SMMU_ID1_NUMCB(t)     ((t) & 0xffu)
#define SMMU_ID1_NUMS2CB(t)   (((t) >> 16) & 0xffu)
#define SMMU_ID1_PAGESIZE(t)  (((t) >> 31) & 1u)
#define SMMU_ID2_IAS(t)       ((t) & 0xfu)
#define SMMU_ID2_OAS(t)       (((t) >> 4) & 0xfu)
#define SMMU_ID2_UBS(t)       (((t) >> 8) & 0xfu)
#define SMMU_ID2_PTFS_4K(t)   (((t) >> 12) & 1u)
#define SMMU_ID2_PTFS_16K(t)  (((t) >> 13) & 1u)
#define SMMU_ID2_PTFS_64K(t)  (((t) >> 14) & 1u)
#define SMMU_ID7_MAJOR(t)     (((t) >> 4) & 0xfu)
#define SMMU_ID7_MINOR(t)     ((t) & 0xfu)

/* SMMU v3 IDR0 fields (Linux arm-smmu-v3.h). */
#define SMMU_IDR0_S1P(t)      (((t) >> 1) & 1u)
#define SMMU_IDR0_TTF(t)      (((t) >> 2) & 3u)
#define SMMU_IDR0_ATS(t)      (((t) >> 10) & 1u)
#define SMMU_IDR0_PRI(t)      (((t) >> 16) & 1u)
#define SMMU_IDR0_ST_LVL(t)   (((t) >> 27) & 3u)

/*
 * The TBU debug registers (upstream Linux arm-smmu-qcom-debug.c).  They
 * are read only here: DEBUG_SID_HALT_REG is a halt request, writing it
 * stops the TBU.
 */
#define TBU_DEBUG_SID_HALT   0x00u
#define TBU_DEBUG_VA_ADDR     0x08u
#define TBU_DEBUG_TXN_TRIGG   0x18u
#define TBU_DEBUG_SR_HALT_ACK 0x20u
#define TBU_DEBUG_PAR         0x28u
#define TBU_DEBUG_AXUSER      0x30u

/*
 * The ACTLR settings the device tree asks for (qcom,actlr): for every
 * stream id X with X & ~mask == sid, the client gets this ACTLR value.
 * Upstream: bit 0 CMTLB, bit 1 CPRE, bit 5 GFX_ACTLR_PRR, bits [9:8] the
 * prefetch depth.
 */
#define ACTLR_CMTLB           0x00000001u
#define ACTLR_CPRE            0x00000002u
#define ACTLR_GFX_PRR         0x00000020u
#define ACTLR_PREFETCH_SHIFT  8
#define ACTLR_PREFETCH_MASK   0x3u

/* The stream id ranges of the two TBUs (qcom,stream-id-range). */
#define TBU0_SID_FIRST       0x000u
#define TBU0_SID_LAST        0x3ffu
#define TBU1_SID_FIRST       0x400u
#define TBU1_SID_LAST        0x7ffu

static vm_offset_t smmu_base;
static vm_offset_t smmu_tcu;
static vm_offset_t smmu_tbu[2];
static vm_offset_t smmu_tbu_stat[2];

/* One 32 bit read of an SMMU register, ordered after the previous access. */
static uint32_t
smmu_rd32(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

/* One 64 bit read (the TBU debug registers are 64 bit wide). */
static uint64_t
smmu_rd64(vm_offset_t base, uint32_t off)
{
	uint64_t v = *(volatile uint64_t *)(base + off);

	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static const char * const smmu_prefetch_name[4] = {
	"default", "shallow", "moderate", "deep"
};

/*
 * The ACTLR value comes from the device tree, not from a register, so the
 * bits are decoded at run time (the helper is not inlined on purpose, so
 * that the compiler does not fold the constant and call the other branch
 * of the test unreachable).
 */
static uint32_t smmu_actlr_bit(uint32_t actlr, uint32_t mask)
    __attribute__((noinline));

static uint32_t
smmu_actlr_bit(uint32_t actlr, uint32_t mask)
{
	return (actlr & mask) ? 1u : 0u;
}

static const char *
smmu_actlr_prefetch(uint32_t actlr)
{
	return smmu_prefetch_name[(actlr >> ACTLR_PREFETCH_SHIFT) &
	    ACTLR_PREFETCH_MASK];
}

/* The prototype of the entry point, so the file compiles without warnings. */
void sweet_smmu_init(void);

void
sweet_smmu_init(void)
{
	uint32_t enabled = 0, dump = 0;
	uint32_t cr0, cr1, cr2, cr3, acr, id0, id1, id2, id7;
	uint32_t v3_idr0, v3_iidr, v3_aidr;
	unsigned i, major, minor;

	if (!PE_parse_boot_argn("sweet_smmu", &enabled, sizeof(enabled)) || !enabled) {
		/* Default off: the probe must never run unless it is asked for. */
		return;
	}
	if (PE_parse_boot_argn("sweet_smmudump", &dump, sizeof(dump))) {
		dump = (dump != 0);
	} else {
		dump = 0;
	}
	smmu_base = ml_io_map(SMMU_BASE_PA, SMMU_BASE_SIZE);
	smmu_tcu = ml_io_map(SMMU_TCU_PA, SMMU_TCU_SIZE);
	smmu_tbu[0] = ml_io_map(SMMU_TBU0_PA, SMMU_TBU0_SIZE);
	smmu_tbu[1] = ml_io_map(SMMU_TBU1_PA, SMMU_TBU1_SIZE);
	smmu_tbu_stat[0] = ml_io_map(SMMU_TBU_STAT0_PA, SMMU_TBU_STAT_SIZE);
	smmu_tbu_stat[1] = ml_io_map(SMMU_TBU_STAT1_PA, SMMU_TBU_STAT_SIZE);
	if (!smmu_base || !smmu_tcu || !smmu_tbu[0] || !smmu_tbu[1] ||
	    !smmu_tbu_stat[0] || !smmu_tbu_stat[1]) {
		printf("SWEET SMMU: mapping failed base=0x%08x tcu=0x%08x\n",
		    SMMU_BASE_PA, SMMU_TCU_PA);
		return;
	}
	printf("SWEET SMMU: probe start base=0x%08x tcu=0x%08x tbu0=0x%08x "
	    "tbu1=0x%08x (read only, no SMMU register is written)\n",
	    SMMU_BASE_PA, SMMU_TCU_PA, SMMU_TBU0_PA, SMMU_TBU1_PA);

	/*
	 * The first word of the global register space is the register the
	 * QSMMU v5 documentation calls SMMU_REVISION.  In the Arm SMMU v2
	 * map (the one this part is driven with) it is CR0, in the SMMU v3
	 * map it is IDR0.  Both readings are printed; the identification
	 * registers below decide which map the silicon uses.
	 */
	cr0 = smmu_rd32(smmu_base, SMMU_CR0);
	cr1 = smmu_rd32(smmu_base, SMMU_CR1);
	cr2 = smmu_rd32(smmu_base, SMMU_CR2);
	cr3 = smmu_rd32(smmu_base, SMMU_CR3);
	acr = smmu_rd32(smmu_base, SMMU_ACR);
	v3_idr0 = smmu_rd32(smmu_base, SMMU_V3_IDR0);
	v3_iidr = smmu_rd32(smmu_base, SMMU_V3_IIDR);
	v3_aidr = smmu_rd32(smmu_base, SMMU_V3_AIDR);
	id0 = smmu_rd32(smmu_base, SMMU_ID0);
	id1 = smmu_rd32(smmu_base, SMMU_ID1);
	id2 = smmu_rd32(smmu_base, SMMU_ID2);
	id7 = smmu_rd32(smmu_base, SMMU_ID7);
	major = SMMU_ID7_MAJOR(id7);
	minor = SMMU_ID7_MINOR(id7);
	printf("SWEET SMMU: rev-word@0x000=0x%08x (v2 CR0 / v3 IDR0) "
	    "cr1=0x%08x cr2=0x%08x cr3=0x%08x acr=0x%08x\n",
	    v3_idr0, cr1, cr2, cr3, acr);
	printf("SWEET SMMU: v3 candidates idr0=0x%08x (s1p=%u ttf=%u ats=%u pri=%u "
	    "stlvl=%u) iidr=0x%08x aidr=0x%08x\n",
	    v3_idr0, SMMU_IDR0_S1P(v3_idr0), SMMU_IDR0_TTF(v3_idr0),
	    SMMU_IDR0_ATS(v3_idr0), SMMU_IDR0_PRI(v3_idr0),
	    SMMU_IDR0_ST_LVL(v3_idr0), v3_iidr, v3_aidr);
	printf("SWEET SMMU: id0=0x%08x id1=0x%08x id2=0x%08x id7=0x%08x "
	    "(version %u.%u)\n", id0, id1, id2, id7, major, minor);
	printf("SWEET SMMU: cr0=0x%08x (b0=%u b1=%u; the ARM SMMU v2 non secure "
	    "map names bit 0 SMMUEN and bit 1 ClientPD) smr=%u sidbits=%u "
	    "cttw=%u irpt=%u atosns=%u sms=%u\n",
	    cr0, cr0 & 1u, (cr0 >> 1) & 1u, SMMU_ID0_NUMSMRG(id0),
	    SMMU_ID0_NUMSIDB(id0), SMMU_ID0_CTTW(id0),
	    SMMU_ID0_NUMIRPT(id0), SMMU_ID0_ATOSNS(id0), SMMU_ID0_SMS(id0));
	printf("SWEET SMMU: cb=%u s2cb=%u pagesize64k=%u ias=%u oas=%u ubs=%u "
	    "ptfs=%s%s%s s1ts=%u s2ts=%u nts=%u\n",
	    SMMU_ID1_NUMCB(id1), SMMU_ID1_NUMS2CB(id1),
	    SMMU_ID1_PAGESIZE(id1), SMMU_ID2_IAS(id2), SMMU_ID2_OAS(id2),
	    SMMU_ID2_UBS(id2),
	    SMMU_ID2_PTFS_4K(id2) ? "4k" : "",
	    SMMU_ID2_PTFS_16K(id2) ? "16k" : "",
	    SMMU_ID2_PTFS_64K(id2) ? "64k" : "",
	    SMMU_ID0_S1TS(id0), SMMU_ID0_S2TS(id0), SMMU_ID0_NTS(id0));
	printf("SWEET SMMU: faults gfsr=0x%08x gfsynr=0x%08x/0x%08x/0x%08x "
	    "tlbgstatus=0x%08x (gsactive=%u)\n",
	    smmu_rd32(smmu_base, SMMU_GFSR),
	    smmu_rd32(smmu_base, SMMU_GFSYNR0),
	    smmu_rd32(smmu_base, SMMU_GFSYNR1),
	    smmu_rd32(smmu_base, SMMU_GFSYNR2),
	    smmu_rd32(smmu_base, SMMU_TLBGSTATUS),
	    smmu_rd32(smmu_base, SMMU_TLBGSTATUS) & 1u);

	/* The TCU (client interface) and the two TBUs. */
	printf("SWEET SMMU: tcu[0..3]=");
	for (i = 0; i < 4; ++i) {
		printf("%s0x%08x", i ? " " : "", smmu_rd32(smmu_tcu, 4u * i));
	}
	printf("\n");
	for (i = 0; i < 2; ++i) {
		uint32_t stat = smmu_rd32(smmu_tbu_stat[i], 0);
		uint32_t stat1 = smmu_rd32(smmu_tbu_stat[i], 4);

		printf("SWEET SMMU: tbu%u sid=0x%03x-0x%03x status=0x%08x/0x%08x "
		    "debug sid_halt=0x%016llx va=0x%016llx trigg=0x%016llx "
		    "halt_ack=0x%016llx par=0x%016llx axuser=0x%016llx\n",
		    i, i ? TBU1_SID_FIRST : TBU0_SID_FIRST,
		    i ? TBU1_SID_LAST : TBU0_SID_LAST, stat, stat1,
		    (uint64_t)smmu_rd64(smmu_tbu[i], TBU_DEBUG_SID_HALT),
		    (uint64_t)smmu_rd64(smmu_tbu[i], TBU_DEBUG_VA_ADDR),
		    (uint64_t)smmu_rd64(smmu_tbu[i], TBU_DEBUG_TXN_TRIGG),
		    (uint64_t)smmu_rd64(smmu_tbu[i], TBU_DEBUG_SR_HALT_ACK),
		    (uint64_t)smmu_rd64(smmu_tbu[i], TBU_DEBUG_PAR),
		    (uint64_t)smmu_rd64(smmu_tbu[i], TBU_DEBUG_AXUSER));
	}

	/*
	 * The stream map the bootloader left behind (qcom,skip-init).  The
	 * ACTLR settings of the device tree are for the stream ids 0, 1, 2,
	 * 4, 5 and 7 with the mask 0x400: CMTLB, CPRE and the GFX prefetch
	 * request register, moderate prefetch depth.
	 */
	printf("SWEET SMMU: actlr(sid 0,1,2,4,5,7 mask 0x400) = 0x%03x "
	    "(cmtlb=%u cpre=%u gfx_prr=%u prefetch=%s)\n",
	    0x32bu, smmu_actlr_bit(0x32bu, ACTLR_CMTLB),
	    smmu_actlr_bit(0x32bu, ACTLR_CPRE),
	    smmu_actlr_bit(0x32bu, ACTLR_GFX_PRR),
	    smmu_actlr_prefetch(0x32bu));
	if (dump) {
		for (i = 0; i < 4; ++i) {
			printf("SWEET SMMU: smr%u=0x%08x s2cr%u=0x%08x\n", i,
			    smmu_rd32(smmu_base, SMMU_SMR(i)), i,
			    smmu_rd32(smmu_base, SMMU_S2CR(i)));
		}
	}
	printf("SWEET SMMU: base=0x%08x rev=%u.%u cr0=0x%08x cb=%u smr=%u tbu=2 "
	    "(probe only: no page table, no stream map, no context bank)\n",
	    SMMU_BASE_PA, major, minor, cr0, SMMU_ID1_NUMCB(id1),
	    SMMU_ID0_NUMSMRG(id0));
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
