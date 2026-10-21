/*
 * SWEET SCM (secure world) call helper - venus (Xiaomi Mi 11, SM8350).
 *
 * WHAT THIS IS: the smallest possible client for the Qualcomm secure world.
 * It issues "smc #0" with the function id in x0 and the arguments in
 * x1..x5, and it knows the four encodings that exist (standard or fast
 * call, 32 or 64 bit arguments).  It exists because the GPU of this phone
 * (Adreno 660) cannot be commanded at all until the secure world has
 * authenticated the zap shader, and that authentication is an SCM call
 * (PAS_AUTH_AND_RESET, service PIL 0x02, command 0x05, PAS id 0x0d).
 *
 * WHAT THIS IS NOT: a GPU driver.  It does not touch the GPU, it does not
 * load the zap shader, it does not publish an IOKit service.  See
 * logs/research/impl/22-scm.md for the whole story.
 *
 * THE CALLING CONVENTION (Linux include/linux/arm-smccc.h):
 *
 *   x0 = [31] type | [30] calling convention | [29:24] owner | [15:0] fn
 *       type         0 = standard call, 1 = fast (atomic) call
 *       convention   0 = 32 bit arguments,  1 = 64 bit arguments
 *       owner        2 = SIP, the owner of the Qualcomm SCM services
 *       fn           (service << 8) | command
 *   x1 = number of arguments (CAF: SCM_ARGS(n))
 *   x2..x5 = the arguments
 *   answer: x0 = error code (0 = success), x1..x3 = results
 *
 *   A Qualcomm SCM call is therefore x0 = 0x02000000 | fnid (32 bit) or
 *   0x42000000 | fnid (64 bit); with the fast call bit 0xc2000000 | fnid.
 *
 * SAFETY.  The default build of this file does nothing at all: the probe
 * is off unless "sweetscm=1" is on the command line.  The probe issues
 * only read only calls (IS_CALL_AVAIL, GET_SECURE_STATE, PAS_IS_SUPPORTED).
 * The one call that can change the machine (PAS_AUTH_AND_RESET resets the
 * peripheral it authenticates) is behind its own boot argument
 * ("sweetscmauth=1") and is never issued by the read only probe.
 *
 * Boot arguments:
 *   sweetscm=1         run the probe (default: off)
 *   sweetscmfn=<hex>   probe this function id instead of the default one.
 *                       The value is the SCM service/command pair
 *                       (svc << 8 | cmd), e.g. 0x0604 = service INFO
 *                       (0x06), command GET_SECURE_STATE (0x04).  The
 *                       spelling with the 0x02000000 vendor prefix is
 *                       accepted as well.
 *   sweetscmpas=1      read only PAS probe for the GPU (PAS id 0x0d)
 *   sweetscmauth=1     DESTRUCTIVE: really authenticate and reset the GPU
 *                       (PAS_AUTH_AND_RESET, PAS id 0x0d)
 */

#if defined(ARM64_BOARD_CONFIG_SWEET)

#include <sys/param.h>
#include <sys/systm.h>
#include <stdbool.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

#include "sweet_scm.h"

extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);

/* CAF drivers/soc/qcom/scm.c: SCM_INTERRUPTED. */
#define SCM_INTERRUPTED     1
#define SCM_MAX_RETRY       8

/* The calling conventions this helper knows. */
#define CONV_UNKNOWN        0
#define CONV_SMC64          1       /* x0 = 0x42000000 | fnid */
#define CONV_SMC32          2       /* x0 = 0x02000000 | fnid */
#define CONV_FAST64         3       /* x0 = 0xc2000000 | fnid */
#define CONV_FAST32         4       /* x0 = 0x82000000 | fnid */

static int scm_conv = CONV_UNKNOWN;

/*
 * The raw SMC.
 *
 * x0 carries the function id, x1..x5 the arguments.  The secure world
 * answers with the error code in x0 and up to three results in x1..x3.
 * The SMC calling convention allows the callee to clobber x0..x5, so all
 * six are listed as read/write operands.
 */
int
sweet_scm_raw(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3,
    uint64_t a4, uint64_t a5, struct sweet_scm_res *res)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	register uint64_t x4 __asm__("x4") = a4;
	register uint64_t x5 __asm__("x5") = a5;

	__asm__ volatile("smc #0"
	    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5)
	    :
	    : "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
	      "x15", "x16", "x17", "cc", "memory");

	if (res != NULL) {
		res->a0 = x0;
		res->a1 = x1;
		res->a2 = x2;
		res->a3 = x3;
	}
	return 0;
}

static const char *
scm_conv_name(int conv)
{
	switch (conv) {
	case CONV_SMC64:
		return "smc64";
	case CONV_SMC32:
		return "smc32";
	case CONV_FAST64:
		return "fast64";
	case CONV_FAST32:
		return "fast32";
	default:
		return "unknown";
	}
}

const char *
sweet_scm_convention(void)
{
	return scm_conv_name(scm_conv);
}

/* The part of x0 that does not depend on the service/command pair. */
static uint32_t
scm_fid_base(int conv)
{
	switch (conv) {
	case CONV_SMC64:
		return SWEET_SCM_SIP_BASE | SWEET_SCM_SMC64_MASK;
	case CONV_SMC32:
		return SWEET_SCM_SIP_BASE;
	case CONV_FAST64:
		return SWEET_SCM_SIP_BASE | SWEET_SCM_SMC64_MASK |
		    SWEET_SCM_ATOMIC_MASK;
	case CONV_FAST32:
		return SWEET_SCM_SIP_BASE | SWEET_SCM_ATOMIC_MASK;
	default:
		return 0;
	}
}

/* One call in one specific convention. */
static int
scm_call_conv(int conv, uint32_t svc, uint32_t cmd, const uint64_t *args,
    unsigned nargs, struct sweet_scm_res *res)
{
	uint64_t a[4];
	uint64_t fid;
	uint64_t arginfo;
	unsigned i;
	unsigned retry;

	if (conv == CONV_UNKNOWN || nargs > 4) {
		return -1;
	}
	for (i = 0; i < 4; i++) {
		a[i] = (i < nargs && args != NULL) ? args[i] : 0;
	}
	/*
	 * x1 is the argument count (CAF: SCM_ARGS(n) with plain value
	 * arguments; Linux upstream: desc.arginfo).
	 */
	arginfo = (uint64_t)nargs;
	fid = (uint64_t)scm_fid_base(conv) | (uint64_t)SWEET_SCM_FNID(svc, cmd);

	for (retry = 0; retry < SCM_MAX_RETRY; retry++) {
		sweet_scm_raw(fid, arginfo, a[0], a[1], a[2], a[3], res);
		if (res == NULL || res->a0 != (uint64_t)SCM_INTERRUPTED) {
			break;
		}
	}
	return 0;
}

/*
 * Find out how the secure world wants to be talked to.
 *
 * The probe call is IS_CALL_AVAIL (service INFO 0x06, command 0x01) with
 * the function id of itself as the only argument: it is read only and it
 * answers 1 when the call exists.  Linux upstream probes the same way
 * (drivers/firmware/qcom/qcom_scm.c, __get_convention()), CAF does it in
 * drivers/soc/qcom/scm.c (is_scm_armv8()).
 */
static int
scm_detect(void)
{
	static const int order[4] = {
		CONV_SMC64, CONV_SMC32, CONV_FAST64, CONV_FAST32
	};
	uint64_t arg;
	struct sweet_scm_res res;
	int i;

	if (scm_conv != CONV_UNKNOWN) {
		return scm_conv;
	}

	/*
	 * The argument of IS_CALL_AVAIL is the function id that is asked
	 * for.  Both Linux upstream and CAF put the vendor function id
	 * (owner SIP included) into it.
	 */
	arg = (uint64_t)SWEET_SCM_SIP_FNID(SWEET_SCM_SVC_INFO,
	    SWEET_SCM_INFO_IS_CALL_AVAIL);

	for (i = 0; i < 4; i++) {
		bzero(&res, sizeof(res));
		if (scm_call_conv(order[i], SWEET_SCM_SVC_INFO,
		    SWEET_SCM_INFO_IS_CALL_AVAIL, &arg, 1, &res) != 0) {
			continue;
		}
		if (res.a0 == 0) {
			scm_conv = order[i];
			printf("SWEET SCM: convention %s, is-call-avail "
			    "a0=0 a1=%llu\n", scm_conv_name(scm_conv),
			    (unsigned long long)res.a1);
			return scm_conv;
		}
		printf("SWEET SCM: convention %s answered a0=%lld\n",
		    scm_conv_name(order[i]), (long long)(int64_t)res.a0);
	}

	printf("SWEET SCM: no convention answered - the secure world is "
	    "silent\n");
	return CONV_UNKNOWN;
}

int
sweet_scm_call(uint32_t svc, uint32_t cmd, const uint64_t *args,
    unsigned nargs, struct sweet_scm_res *res)
{
	int conv;

	conv = scm_detect();
	if (conv == CONV_UNKNOWN) {
		return -1;
	}
	return scm_call_conv(conv, svc, cmd, args, nargs, res);
}

/*
 * Is the peripheral known to the secure world?
 *
 * PAS_IS_SUPPORTED (service PIL 0x02, command 0x07) answers 1 for a
 * peripheral the secure world can authenticate.  Read only.
 */
int
sweet_scm_pas_supported(uint32_t pas_id, bool *supported)
{
	uint64_t arg;
	struct sweet_scm_res res;
	int ret;

	if (supported != NULL) {
		*supported = false;
	}
	arg = (uint64_t)pas_id;
	bzero(&res, sizeof(res));
	ret = sweet_scm_call(SWEET_SCM_SVC_PIL,
	    SWEET_SCM_PIL_PAS_IS_SUPPORTED, &arg, 1, &res);
	if (ret != 0) {
		return ret;
	}
	if (res.a0 != 0) {
		return -1;
	}
	if (supported != NULL) {
		*supported = (res.a1 != 0);
	}
	return 0;
}

/*
 * The secure boot state of the device.  Read only.
 *
 * CAF drivers/soc/qcom/scm.c (scm_is_secure_device): the answer carries
 * bit 0 (secure boot) and bit 2 (debug enabled).
 */
int
sweet_scm_secure_state(uint32_t *state)
{
	struct sweet_scm_res res;
	int ret;

	bzero(&res, sizeof(res));
	ret = sweet_scm_call(SWEET_SCM_SVC_INFO, SWEET_SCM_INFO_GET_SECURE_STATE,
	    NULL, 0, &res);
	if (ret != 0) {
		return ret;
	}
	if (res.a0 != 0) {
		return -1;
	}
	if (state != NULL) {
		*state = (uint32_t)res.a1;
	}
	return 0;
}

/*
 * Authenticate the firmware image of the peripheral and reset it.
 *
 * DESTRUCTIVE: the peripheral is reset by this call, and the secure world
 * expects the firmware image to be in the peripheral carve out already.
 * Never call this from a probe.
 */
int
sweet_scm_pas_auth_and_reset(uint32_t pas_id, struct sweet_scm_res *res)
{
	uint64_t arg;
	struct sweet_scm_res local;
	int ret;

	arg = (uint64_t)pas_id;
	bzero(&local, sizeof(local));
	ret = sweet_scm_call(SWEET_SCM_SVC_PIL,
	    SWEET_SCM_PIL_PAS_AUTH_AND_RESET, &arg, 1, &local);
	if (res != NULL) {
		*res = local;
	}
	return ret;
}

/* "0x3d00000", "3d00000" and "0X3D00000" are all accepted. */
static bool
scm_parse_hex(const char *s, uint32_t *out)
{
	uint32_t v = 0;
	unsigned digits = 0;

	if (s == NULL) {
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

static void
scm_print_res(const char *what, const struct sweet_scm_res *res)
{
	printf("SWEET SCM: %s a0=%lld a1=0x%llx a2=0x%llx a3=0x%llx\n", what,
	    (long long)(int64_t)res->a0, (unsigned long long)res->a1,
	    (unsigned long long)res->a2, (unsigned long long)res->a3);
}

/* The default probe: read the secure boot state.  Read only. */
static void
scm_probe_secure_state(void)
{
	uint32_t state = 0;
	struct sweet_scm_res res;

	bzero(&res, sizeof(res));
	if (sweet_scm_call(SWEET_SCM_SVC_INFO, SWEET_SCM_INFO_GET_SECURE_STATE,
	    NULL, 0, &res) != 0) {
		printf("SWEET SCM: secure state call failed\n");
		return;
	}
	scm_print_res("secure-state", &res);
	/*
	 * CAF drivers/soc/qcom/scm.c: bit 0 and bit 2 of the answer mean
	 * "secure device" (secure boot / debug enabled).
	 */
	if (res.a0 == 0) {
		printf("SWEET SCM: secure-state bits: boot=%u debug=%u\n",
		    (unsigned)((res.a1 & 1u) ? 1u : 0u),
		    (unsigned)((res.a1 & 4u) ? 1u : 0u));
	}
	if (sweet_scm_secure_state(&state) == 0) {
		printf("SWEET SCM: secure-state raw=0x%08x\n", state);
	}
}

/* An arbitrary function id, with no arguments.  Read only by definition. */
static void
scm_probe_fn(uint32_t fn)
{
	uint32_t svc;
	uint32_t cmd;
	struct sweet_scm_res res;

	svc = (fn >> 8) & 0xffu;
	cmd = fn & 0xffu;
	printf("SWEET SCM: probing svc=0x%02x cmd=0x%02x "
	    "(x0=0x%08x 64 bit / 0x%08x 32 bit)\n", svc, cmd,
	    SWEET_SCM_X0_SMC64(svc, cmd), SWEET_SCM_X0_SMC32(svc, cmd));

	bzero(&res, sizeof(res));
	if (sweet_scm_call(svc, cmd, NULL, 0, &res) != 0) {
		printf("SWEET SCM: no convention, nothing probed\n");
		return;
	}
	scm_print_res("probe", &res);
}

/*
 * Read only PAS probe: is the secure world able to authenticate the GPU
 * at all, and does it know the PAS id of the zap shader?
 */
static void
scm_probe_pas(uint32_t pas_id)
{
	uint64_t arg;
	struct sweet_scm_res res;
	bool supported = false;

	/* 1: is the AUTH_AND_RESET call itself available? */
	arg = (uint64_t)SWEET_SCM_SIP_FNID(SWEET_SCM_SVC_PIL,
	    SWEET_SCM_PIL_PAS_AUTH_AND_RESET);
	bzero(&res, sizeof(res));
	if (sweet_scm_call(SWEET_SCM_SVC_INFO, SWEET_SCM_INFO_IS_CALL_AVAIL,
	    &arg, 1, &res) == 0 && res.a0 == 0) {
		printf("SWEET SCM: pas auth-and-reset available=%llu\n",
		    (unsigned long long)res.a1);
	} else {
		printf("SWEET SCM: pas auth-and-reset not available "
		    "(a0=%lld)\n", (long long)(int64_t)res.a0);
	}

	/* 2: is the PAS_IS_SUPPORTED call available? */
	arg = (uint64_t)SWEET_SCM_SIP_FNID(SWEET_SCM_SVC_PIL,
	    SWEET_SCM_PIL_PAS_IS_SUPPORTED);
	bzero(&res, sizeof(res));
	if (sweet_scm_call(SWEET_SCM_SVC_INFO, SWEET_SCM_INFO_IS_CALL_AVAIL,
	    &arg, 1, &res) == 0 && res.a0 == 0) {
		printf("SWEET SCM: pas is-supported available=%llu\n",
		    (unsigned long long)res.a1);
	} else {
		printf("SWEET SCM: pas is-supported not available "
		    "(a0=%lld)\n", (long long)(int64_t)res.a0);
	}

	/* 3: does the secure world know this PAS id? */
	if (sweet_scm_pas_supported(pas_id, &supported) == 0) {
		printf("SWEET SCM: pas id 0x%02x supported=%u\n", pas_id,
		    supported ? 1u : 0u);
	} else {
		printf("SWEET SCM: pas id 0x%02x query failed\n", pas_id);
	}
}

/*
 * The probe thread.  An SCM call may block (the secure world can put the
 * caller to sleep), so the call is made from a thread, never from bsd_init.
 */
static void
scm_probe_thread(void *arg, wait_result_t result)
{
	uint32_t pas = 0;
	uint32_t auth = 0;
	uint32_t fn = 0;
	char fnbuf[32];
	struct sweet_scm_res res;

	(void)arg;
	(void)result;

	printf("SWEET SCM: probe start (read only unless sweetscmauth=1)\n");
	scm_detect();

	if (PE_parse_boot_argn("sweetscmfn", fnbuf, sizeof(fnbuf))) {
		if (scm_parse_hex(fnbuf, &fn) && fn != 0) {
			scm_probe_fn(fn);
		} else {
			printf("SWEET SCM: bad sweetscmfn=\"%s\"\n", fnbuf);
		}
	} else {
		scm_probe_secure_state();
	}

	if (PE_parse_boot_argn("sweetscmpas", &pas, sizeof(pas)) && pas != 0) {
		scm_probe_pas(SWEET_PAS_GPU);
	}

	if (PE_parse_boot_argn("sweetscmauth", &auth, sizeof(auth)) &&
	    auth != 0) {
		printf("SWEET SCM: AUTH AND RESET of PAS 0x%02x - this "
		    "RESETS the GPU\n", SWEET_PAS_GPU);
		bzero(&res, sizeof(res));
		if (sweet_scm_pas_auth_and_reset(SWEET_PAS_GPU, &res) != 0) {
			printf("SWEET SCM: auth and reset could not be "
			    "issued\n");
		} else {
			scm_print_res("auth-and-reset", &res);
		}
	}

	printf("SWEET SCM: probe done, convention %s\n",
	    sweet_scm_convention());
}

int
sweet_scm_init(void)
{
	uint32_t enabled = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("sweetscm", &enabled, sizeof(enabled)) ||
	    enabled == 0) {
		/* Default off: the probe must never run unless it is asked for. */
		return 0;
	}
	if (kernel_thread_start(scm_probe_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
		printf("SWEET SCM: probe thread started\n");
	}
	return 0;
}

#endif /* ARM64_BOARD_CONFIG_SWEET */
