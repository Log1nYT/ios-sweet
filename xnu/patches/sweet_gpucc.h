/*
 * SWEET GPUCC: addresses, register offsets and bit layouts of the GPU clock
 * controller (GPU_CC) of the venus GPU (Xiaomi Mi 11, SM8350, Adreno 660).
 *
 * Every constant below carries the source it was taken from; the full
 * evidence list (device tree, vendor kernel, upstream Linux) is in
 * logs/research/impl/20-gpucc.md.
 *
 * READ ONLY: nothing in this header describes a write path.  The branch
 * enables (CBCR), the two RCG2 root clocks, both GDSC control registers
 * and the GX soft reset must never be written from this kernel: a wrong
 * write to the branch control or to the reset can hang the GPU, the
 * display or the whole SoC.
 */
#ifndef SWEET_GPUCC_H
#define SWEET_GPUCC_H

#include <stdbool.h>
#include <stdint.h>

/*
 * GPU clock controller window.  Source: the live device tree of this phone
 * (dtc -I dtb -O dts of venus.dtb), node /soc/qcom,gpucc@3d90000:
 *
 *   clock_gpucc: qcom,gpucc@3d90000 {
 *           compatible = "qcom,lahaina-gpucc", "syscon";
 *           reg = <0x3d90000 0x9000>;         <- 36 KiB of GPU_CC registers
 *           reg-names = "cc_base";
 *           vdd_mx-supply = <0x19>;  vdd_cx-supply = <0x1f>;
 *           #clock-cells = <0x01>;  #reset-cells = <0x01>;
 *           phandle = <0x32>;
 *   };
 *
 * The same node is the clock provider of the four GPU clocks the GPU node
 * asks for (gpu_cc_ahb, gpu_cc_cx_gmu, gpu_cc_hub_aon, gpu_cc_hub_cx_int)
 * and of the HLOS vote bit gpu_cc_hlos1_vote_gpu_smmu.
 */
#define SWEET_GPUCC_PA                 0x03d90000u
#define SWEET_GPUCC_SIZE               0x00009000u

/*
 * Register offsets inside the GPU_CC window (byte offsets).
 *
 * Source: drivers/clk/qcom/gpucc-lahaina.c of the venus kernel tree
 * (MiCode/Xiaomi_Kernel_OpenSource, branch venus-r-oss) and, with the
 * same numbers, drivers/clk/qcom/gpucc-sm8350.c of upstream Linux.  Both
 * describe the SM8350 "lahaina" GPU_CC.
 */

/* --- the two alpha PLLs of the GPU_CC (lucid 5LPE) --- */
#define SWEET_GPUCC_PLL0              0x0000u   /* alpha PLL 0 base          */
#define SWEET_GPUCC_PLL1              0x0100u   /* alpha PLL 1 base          */
#define SWEET_GPUCC_PLL0_STATUS       0x0030u   /* PLL0 + 0x30 (STATUS)      */
#define SWEET_GPUCC_PLL1_STATUS       0x0130u   /* PLL1 + 0x30 (STATUS)      */
#define SWEET_GPUCC_PLL0_OPMODE       0x0038u   /* PLL0 + 0x38 (OPMODE)      */
#define SWEET_GPUCC_PLL1_OPMODE       0x0138u   /* PLL1 + 0x38 (OPMODE)      */
#define SWEET_GPUCC_PLL0_ALPHA        0x0040u   /* PLL0 + 0x40 (ALPHA_VAL)   */
#define SWEET_GPUCC_PLL1_ALPHA        0x0140u   /* PLL1 + 0x40 (ALPHA_VAL)   */

/* --- the branch clocks (CBCR: bit 0 = clock enable, bit 31 = clock off) --- */
#define SWEET_GPUCC_CXO_AON           0x1004u   /* gpu_cc_cxo_aon_clk        */
#define SWEET_GPUCC_GX_VSENSE         0x1058u   /* gpu_cc_gx_vsense_clk      */
#define SWEET_GPUCC_GX_QDSS_TSCTR     0x105cu   /* gpu_cc_gx_qdss_tsctr_clk  */
#define SWEET_GPUCC_GX_GMU            0x1064u   /* gpu_cc_gx_gmu_clk         */
#define SWEET_GPUCC_AHB               0x1078u   /* gpu_cc_ahb_clk            */
#define SWEET_GPUCC_CRC_AHB           0x107cu   /* gpu_cc_crc_ahb_clk        */
#define SWEET_GPUCC_CX_QDSS_AT        0x1080u   /* gpu_cc_cx_qdss_at_clk     */
#define SWEET_GPUCC_CX_QDSS_TSCTR     0x1084u   /* gpu_cc_cx_qdss_tsctr_clk  */
#define SWEET_GPUCC_CX_APB            0x1088u   /* gpu_cc_cx_apb_clk         */
#define SWEET_GPUCC_CX_SNOC_DVM       0x108cu   /* gpu_cc_cx_snoc_dvm_clk    */
#define SWEET_GPUCC_SLEEP             0x1090u   /* gpu_cc_sleep_clk          */
#define SWEET_GPUCC_CX_QDSS_TRIG      0x1094u   /* gpu_cc_cx_qdss_trig_clk   */
#define SWEET_GPUCC_CX_GMU            0x1098u   /* gpu_cc_cx_gmu_clk         */
#define SWEET_GPUCC_CXO               0x109cu   /* gpu_cc_cxo_clk            */
#define SWEET_GPUCC_CB                0x1170u   /* gpu_cc_cb_clk             */
#define SWEET_GPUCC_HUB_AON           0x1178u   /* gpu_cc_hub_aon_clk        */
#define SWEET_GPUCC_HUB_CX_INT        0x1204u   /* gpu_cc_hub_cx_int_clk     */
#define SWEET_GPUCC_FREQ_MEASURE      0x120cu   /* gpu_cc_freq_measure_clk   */
#define SWEET_GPUCC_HLOS1_VOTE_SMMU   0x5000u   /* gpu_cc_hlos1_vote_gpu_smmu_clk */
#define SWEET_GPUCC_MND1X_0_GFX3D     0x802cu   /* gpu_cc_mnd1x_0_gfx3d_clk  */
#define SWEET_GPUCC_MND1X_1_GFX3D     0x8030u   /* gpu_cc_mnd1x_1_gfx3d_clk  */

/* --- the two root clocks (RCG2: cmd, cfg, m, n, d) --- */
#define SWEET_GPUCC_GMU_RCG2          0x1120u   /* gpu_cc_gmu_clk_src        */
#define SWEET_GPUCC_HUB_RCG2          0x117cu   /* gpu_cc_hub_clk_src        */
#define SWEET_GPUCC_RCG2_CFG          0x0004u   /* cfg  = cmd + 4            */
#define SWEET_GPUCC_RCG2_M            0x0008u   /* m    = cmd + 8            */
#define SWEET_GPUCC_RCG2_N            0x000cu   /* n    = cmd + 12           */
#define SWEET_GPUCC_RCG2_D            0x0010u   /* d    = cmd + 16           */

/* --- the two plain dividers of the hub clock --- */
#define SWEET_GPUCC_HUB_CX_INT_DIV    0x11bcu   /* 4 bit divider (hub cx int) */
#define SWEET_GPUCC_HUB_AHB_DIV       0x11c0u   /* 4 bit divider (hub ahb)    */

/* --- the power domains and the reset (all inside the 36 KiB window) --- */
#define SWEET_GPUCC_GX_GDSC           0x100cu   /* 0x03d9100c GX GDSCR       */
#define SWEET_GPUCC_CX_GDSC           0x106cu   /* 0x03d9106c CX GDSCR       */
#define SWEET_GPUCC_GX_SW_RESET       0x1008u   /* 0x03d91008 GX soft reset  */
#define SWEET_GPUCC_CX_HW_CTRL        0x1540u   /* 0x03d91540 CX hw control  */
#define SWEET_GPUCC_GX_DOMAIN         0x158cu   /* 0x03d9158c GX domain ctl  */
#define SWEET_GPUCC_GX_CLAMP_IO       0x1508u   /* GX clamp io control       */

/* The status register of a GDSC is GDSCR + 4. */
#define SWEET_GPUCC_GDSC_STATUS       0x0004u

/*
 * Branch (CBCR) bits.  Source: drivers/clk/qcom/clk-branch.h of the venus
 * kernel tree and of upstream Linux: the branch enable is bit 0, and the
 * "clock off" state is bit 31 of the same register.
 */
#define SWEET_GPUCC_CBCR_CLK_ENABLE   0x00000001u
#define SWEET_GPUCC_CBCR_CLK_OFF      0x80000000u

/*
 * RCG2 bits.  Source: drivers/clk/qcom/clk-rcg2.c of the venus kernel tree
 * and of upstream Linux:
 *   cmd bit  1     = root enable
 *   cmd bit 31     = root off (the root clock is switched off)
 *   cfg bits 4..0  = source divider minus one (hid_width = 5 here)
 *   cfg bits 10..8 = source select (which clock the branch runs from)
 *   cfg bit 20     = hardware clock control (the GMU drives this clock)
 */
#define SWEET_GPUCC_RCG_ROOT_ENABLE   0x00000002u
#define SWEET_GPUCC_RCG_ROOT_OFF      0x80000000u
#define SWEET_GPUCC_RCG_DIV_MASK      0x0000001fu
#define SWEET_GPUCC_RCG_SRC_SHIFT     8u
#define SWEET_GPUCC_RCG_SRC_MASK      0x00000007u
#define SWEET_GPUCC_RCG_HW_CTRL       0x00100000u

/*
 * The source select of the GMU root clock.  Source: the parent map of
 * gpu_cc_gmu_clk_src in drivers/clk/qcom/gpucc-lahaina.c of the venus
 * kernel tree.
 */
#define SWEET_GPUCC_SRC_BI_TCXO       0u        /* 19.2 MHz                 */
#define SWEET_GPUCC_SRC_GPU_CC_PLL0   1u        /* alpha PLL 0 of the GPU_CC */
#define SWEET_GPUCC_SRC_GPU_CC_PLL1   3u        /* alpha PLL 1 of the GPU_CC */
#define SWEET_GPUCC_SRC_GCC_GPLL0     5u        /* global GCC GPLL0 main    */
#define SWEET_GPUCC_SRC_GCC_GPLL0_DIV 6u        /* global GCC GPLL0 div     */
#define SWEET_GPUCC_SRC_PLL_TEST_SE   7u        /* core_bi_pll_test_se      */

/*
 * GDSC bits.  Source: drivers/clk/qcom/gdsc.c of the venus kernel tree and
 * of upstream Linux:
 *   GDSCR bit 31        = power request (PWR_ON)
 *   GDSCR bit  1        = hardware control
 *   GDSCR bit  0        = software collapse
 *   GDSCR + 4 bit 16    = power up complete
 *   GDSCR + 4 bit 15    = power down complete
 */
#define SWEET_GPUCC_GDSCR_PWR_ON      0x80000000u
#define SWEET_GPUCC_GDSCR_HW_CTRL     0x00000002u
#define SWEET_GPUCC_GDSCR_SW_COLLAPSE 0x00000001u
#define SWEET_GPUCC_GDSC_PWR_UP       0x00010000u
#define SWEET_GPUCC_GDSC_PWR_DOWN     0x00008000u

/*
 * The state of the two power domains, read out of the GPU_CC.
 * cx_on / gx_on are the "the domain is really powered" verdicts:
 *   CX: the hardware controller bit of the CX hw control register
 *       (gds_hw_ctrl = 0x1540) or the power-up-complete bit of GDSCR + 4;
 *   GX: the power-up-complete bit of GDSCR + 4 (the GX domain is polled,
 *       it has no hardware control register).
 */
typedef struct {
	uint32_t cx_gdscr;       /* GDSCR of the CX domain  (0x106c)          */
	uint32_t cx_status;      /* GDSCR + 4 of the CX domain                 */
	uint32_t cx_hw_ctrl;     /* CX hardware control      (0x1540)          */
	uint32_t gx_gdscr;       /* GDSCR of the GX domain  (0x100c)          */
	uint32_t gx_status;      /* GDSCR + 4 of the GX domain                 */
	uint32_t gx_domain;      /* GX domain register      (0x158c)          */
	uint32_t gx_clamp_io;    /* GX clamp io control     (0x1508)          */
	uint32_t gx_sw_reset;    /* GX soft reset           (0x1008)          */
	bool cx_on;              /* the CX (control) domain is powered        */
	bool gx_on;              /* the GX (graphics) domain is powered        */
} sweet_gpucc_gdsc_t;

/*
 * One clock branch of the GPU_CC.
 */
typedef struct {
	const char *name;        /* the name the vendor driver uses            */
	uint32_t off;            /* offset of the CBCR in the GPU_CC window    */
	uint32_t value;          /* the raw register                           */
	bool enabled;            /* bit 0 set: the branch is clocked           */
	bool halted;             /* bit 31 set: the branch is switched off     */
} sweet_gpucc_branch_t;

/*
 * The state of the GMU root clock (the clock the GMU firmware runs at).
 * The GMU is the microcontroller that owns the GPU *core* clock on this
 * generation: the core frequency is programmed through the GMU DCVS
 * interface and is not readable from any register of the GPU_CC.
 */
typedef struct {
	uint32_t src;            /* CFG source select                          */
	uint32_t div;            /* CFG divider (the register holds div - 1)   */
	uint32_t parent_khz;     /* nominal frequency of the selected source  */
	uint32_t khz;            /* the resulting clock of the GMU root        */
	bool root_on;            /* the root clock is not switched off         */
	bool hw_ctrl;            /* CFG bit 20: the GMU drives this clock      */
} sweet_gpucc_gmu_t;

/*
 * The API.  All of it is read only and safe to call from any context.
 * The functions map the GPU_CC window on first use (and never print), so
 * they work whether or not the boot arg sweetgpucc=1 was given.
 */
extern int sweet_gpucc_init(void);
extern bool sweet_gpucc_mapped(void);
extern bool sweet_gpucc_read_clock(uint32_t *khz);
extern bool sweet_gpucc_is_on(void);
extern bool sweet_gpucc_gdsc_state(sweet_gpucc_gdsc_t *st);
extern bool sweet_gpucc_gmu_info(sweet_gpucc_gmu_t *info);
extern uint32_t sweet_gpucc_branches(sweet_gpucc_branch_t *out, uint32_t max);

#endif /* SWEET_GPUCC_H */
