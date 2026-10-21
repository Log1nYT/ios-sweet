/*
 * SWEET SCM (secure world) helper - venus (Xiaomi Mi 11, SM8350, Adreno 660).
 *
 * The secure world of a Qualcomm SoC is reached with "smc #0".  The
 * function id in x0 has the shape of the ARM SMC calling convention:
 *
 *   x0 = [31] type | [30] calling convention | [29:24] owner | [15:0] function
 *
 *   type          0 = standard (preemptible) call, 1 = fast call
 *   convention    0 = 32 bit arguments,       1 = 64 bit arguments
 *   owner         2 = "SIP", the owner of the Qualcomm SCM services
 *   function      (service << 8) | command
 *
 * The Qualcomm vendor range is therefore 0x02000000 | (service << 8) |
 * command - the "0x02000000" is the owner field, not a flag of its own.
 * With the 64 bit convention bit set the function id becomes
 * 0x42000000 | fnid, with the fast call bit 0xc2000000 | fnid.
 *
 * Sources (see logs/research/impl/22-scm.md for the full list):
 *   - Linux include/linux/arm-smccc.h          (the layout above)
 *   - Linux drivers/firmware/qcom/qcom_scm.h   (service/command ids)
 *   - Linux drivers/firmware/qcom/qcom_scm-smc.c   (the 64 bit call)
 *   - CAF include/soc/qcom/scm.h and drivers/soc/qcom/scm.c (the same thing
 *     under the name SCM_SIP_FNID / SMC64_MASK)
 */
#ifndef _SWEET_SCM_H_
#define _SWEET_SCM_H_

#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

/* ------------------------------------------------------------------ SMCCC */
/* Linux include/linux/arm-smccc.h */
#define SWEET_SMCCC_STD_CALL           0u
#define SWEET_SMCCC_FAST_CALL          1u
#define SWEET_SMCCC_TYPE_SHIFT         31u
#define SWEET_SMCCC_SMC_32             0u
#define SWEET_SMCCC_SMC_64             1u
#define SWEET_SMCCC_CALL_CONV_SHIFT    30u
#define SWEET_SMCCC_OWNER_MASK         0x3fu
#define SWEET_SMCCC_OWNER_SHIFT        24u
#define SWEET_SMCCC_FUNC_MASK          0xffffu
#define SWEET_SMCCC_OWNER_SIP          2u
#define SWEET_SMCCC_OWNER_TRUSTED_OS   50u

#define SWEET_SMCCC_CALL_VAL(type, convention, owner, func)              \
	(((uint32_t)(type) << SWEET_SMCCC_TYPE_SHIFT) |                 \
	 ((uint32_t)(convention) << SWEET_SMCCC_CALL_CONV_SHIFT) |       \
	 (((uint32_t)(owner) & SWEET_SMCCC_OWNER_MASK) <<                \
	  SWEET_SMCCC_OWNER_SHIFT) |                                    \
	 ((uint32_t)(func) & SWEET_SMCCC_FUNC_MASK))

/* Linux drivers/firmware/qcom/qcom_scm.h: SCM_SMC_FNID(svc, cmd) */
#define SWEET_SCM_FNID(svc, cmd)                                 \
	((((uint32_t)(svc) & 0xffu) << 8) | ((uint32_t)(cmd) & 0xffu))

/* ------------------------------------------------- Qualcomm vendor range */
/*
 * CAF include/soc/qcom/scm.h: SCM_SIP_FNID(s, c) = 0x02000000 | fnid.
 * The 0x02000000 is the SMCCC owner field (owner 2 = SIP) at bit 24.
 */
#define SWEET_SCM_SIP_BASE           0x02000000u
#define SWEET_SCM_SIP_FNID(svc, cmd) (SWEET_SCM_SIP_BASE | SWEET_SCM_FNID(svc, cmd))
/* CAF: SMC64_MASK (the SMCCC convention bit) and SMC_ATOMIC_SYSCALL. */
#define SWEET_SCM_SMC64_MASK         0x40000000u
#define SWEET_SCM_ATOMIC_MASK        0x80000000u

/* The complete x0 of the four calls this helper knows. */
#define SWEET_SCM_X0_SMC64(svc, cmd)                                 \
	(SWEET_SCM_SIP_FNID(svc, cmd) | SWEET_SCM_SMC64_MASK)
#define SWEET_SCM_X0_SMC32(svc, cmd)  (SWEET_SCM_SIP_FNID(svc, cmd))
#define SWEET_SCM_X0_FAST64(svc, cmd)                                \
	(SWEET_SCM_SIP_FNID(svc, cmd) | SWEET_SCM_SMC64_MASK |          \
	 SWEET_SCM_ATOMIC_MASK)
#define SWEET_SCM_X0_FAST32(svc, cmd)                                \
	(SWEET_SCM_SIP_FNID(svc, cmd) | SWEET_SCM_ATOMIC_MASK)

/* --------------------------------------------------------------- services */
/* CAF include/soc/qcom/scm.h == Linux drivers/firmware/qcom/qcom_scm.h */
#define SWEET_SCM_SVC_BOOT           0x01u
#define SWEET_SCM_SVC_PIL            0x02u   /* peripheral (image) loader */
#define SWEET_SCM_SVC_IO             0x05u
#define SWEET_SCM_SVC_INFO           0x06u
#define SWEET_SCM_SVC_MP             0x0cu

/* -------------------------------------------------------------- commands */
/* Linux drivers/firmware/qcom/qcom_scm.h (QCOM_SCM_*) */
#define SWEET_SCM_INFO_IS_CALL_AVAIL     0x01u
#define SWEET_SCM_INFO_GET_SECURE_STATE  0x04u  /* CAF: TZ_INFO_GET_SECURE_STATE */

/* Linux qcom_scm.h / CAF subsys-pil-tz.c (enum scm_cmd) */
#define SWEET_SCM_PIL_PAS_INIT_IMAGE     0x01u
#define SWEET_SCM_PIL_PAS_MEM_SETUP      0x02u
#define SWEET_SCM_PIL_PAS_AUTH_AND_RESET 0x05u
#define SWEET_SCM_PIL_PAS_SHUTDOWN       0x06u
#define SWEET_SCM_PIL_PAS_IS_SUPPORTED   0x07u

/* ------------------------------------------------------------- PAS ids */
/* The GPU (zap shader) carve out of venus.dts: pil_gpu, qcom,pas-id = 0x0d */
#define SWEET_PAS_GPU               0x0du

/* ------------------------------------------------------------------ API */

struct sweet_scm_res {
	uint64_t a0;     /* error code: 0 = success, negative = SCM error */
	uint64_t a1;     /* result 0 */
	uint64_t a2;     /* result 1 */
	uint64_t a3;     /* result 2 */
};

/*
 * One SCM call.  svc/cmd are the Qualcomm service and command ids, args
 * are the plain arguments (at most four; more would need a DMA buffer).
 * The calling convention is detected once and then reused.
 * Returns 0 if the call was made, -1 if the arguments are not usable.
 */
int sweet_scm_call(uint32_t svc, uint32_t cmd, const uint64_t *args,
    unsigned nargs, struct sweet_scm_res *res);

/* Raw call: x0 = fid, x1..x5 = arguments.  Used by the probe. */
int sweet_scm_raw(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, struct sweet_scm_res *res);

/* Read only: is the peripheral known to the secure world? */
int sweet_scm_pas_supported(uint32_t pas_id, bool *supported);

/*
 * Read only: the secure boot state of the device.  Bit 0 and bit 2 of the
 * answer mean "secure device" (CAF: scm_is_secure_device()).
 */
int sweet_scm_secure_state(uint32_t *state);

/*
 * DESTRUCTIVE: authenticates the firmware image of the peripheral and
 * resets it.  Only call this when the firmware is really in place.
 */
int sweet_scm_pas_auth_and_reset(uint32_t pas_id, struct sweet_scm_res *res);

/* The name of the detected calling convention ("smc64", "smc32", ...). */
const char *sweet_scm_convention(void);

/* Boot time probe; started from bsd_init(). */
int sweet_scm_init(void);

#endif /* _SWEET_SCM_H_ */
