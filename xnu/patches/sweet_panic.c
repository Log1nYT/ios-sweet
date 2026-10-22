/*
 * SWEET panic capture (venus).
 *
 * A panic on this board used to be a dead end: the panic text only reached
 * the framebuffer console, the phone never came back by itself, and after
 * the manual power cycle the reason for the panic was gone.  This file
 * makes a panic survivable and diagnosable:
 *
 *  1. The panic text is copied into the RAM console ring (bsd/dev/sweet_ufs.c)
 *     as one of the first things the panic path does, so that the USB
 *     logger (tools/venus-udplog.py) receives it BEFORE the phone resets.
 *     The panic path then waits a little (sweetpanicflush=) so that the
 *     logger thread can really transmit it: without that wait the text
 *     would still be in the ring when the SoC goes down.
 *  2. The same text is written into the reserved panic region (/pram,
 *     /chosen/embedded-panic-log-size) into a private slot at the end of
 *     the region.  The region is outside the allocator's window, so it
 *     survives the reset, and the next boot prints it (sweetpaniclast=1).
 *  3. The region is read back right after the write, so the log says
 *     whether the copy really made it into RAM.
 *  4. sweetpanic=1 panics the kernel on purpose, to test the whole chain
 *     (panic -> USB log -> reserved region -> reset -> next boot).
 *
 * Everything that runs in the panic path is panic safe: no vm_map
 * allocation, no sleeping, no lock that the panicking thread could hold.
 * The only things the panic path touches are static memory, the console
 * ring (append only, no lock) and the panic region (already mapped by
 * pexpert/arm/pe_init.c).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/backtrace.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>

#include "sweet_panic.h"

/* bsd/dev/sweet_ufs.c: the RAM console ring that the USB logger streams. */
extern void sweet_log_append(const char *c, int size);
/* osfmk/arm/locks_arm.c: the preemption counters are not exported to BSD. */
extern void _disable_preemption(void);
extern void _enable_preemption(void);
/* osfmk/kern/misc_protos.h */
extern int paniclog_append_noflush(const char *format, ...);
/* osfmk/kern/thread.h, not exported to the BSD layer */
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);

/* How long the panic path waits for the USB logger (milliseconds). */
#define SP_FLUSH_MS        400u
/* How long the phone stays on the panic screen before the fallback reset. */
#define SP_GRACE_MS        5000u
/* How long the test panic waits before it panics (seconds). */
#define SP_TEST_DELAY      5u

static struct sp_slot *sp_slot;        /* our slot at the end of the region */
static uintptr_t sp_region;             /* the panic region (gPanicBase) */
static unsigned int sp_region_size;
static char sp_prev_text[SP_TEXT_MAX];  /* our copy of the previous panic */
static unsigned int sp_prev_len;
static char sp_prev_xnu[SP_TEXT_MAX];   /* XNU's panic log of the last boot */
static unsigned int sp_prev_xnu_len;
static uint64_t sp_seq;
static int sp_captured;                 /* the first panic of this boot */
static int sp_flushed;                  /* the USB log wait already happened */
static uint32_t sp_flush_ms = SP_FLUSH_MS;
static uint32_t sp_grace_ms = SP_GRACE_MS;
static uint32_t sp_test_delay = SP_TEST_DELAY;
static int sp_test_chan;

/*
 * Append to the RAM console ring.  This is the buffer that
 * bsd/dev/sweet_usb.c streams to the Mac over USB, so it is the only way to
 * get the panic text out of a phone that is about to reset.  The ring
 * append never blocks and never allocates; the preemption is off so that
 * the text cannot be interleaved with the console output of another CPU.
 */
static void
sp_ring(const char *s)
{
	size_t len;

	if (s == NULL) {
		return;
	}
	len = strlen(s);
	if (len == 0) {
		return;
	}
	_disable_preemption();
	sweet_log_append(s, (int)len);
	_enable_preemption();
}

/*
 * Format the panic text into a buffer.  No allocation, no lock.
 */
static unsigned int
sp_format(const char *fmt, va_list *args, char *buf, unsigned int size)
{
	va_list copy;
	int n;

	if (fmt == NULL || args == NULL || size == 0) {
		return 0;
	}
	va_copy(copy, *args);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
	n = vsnprintf(buf, size, fmt, copy);
#pragma clang diagnostic pop
	va_end(copy);
	if (n < 0) {
		buf[0] = '\0';
		return 0;
	}
	if ((unsigned int)n >= size) {
		n = (int)size - 1;
	}
	buf[n] = '\0';
	return (unsigned int)n;
}

/* The panic region, as mapped by pexpert/arm/pe_init.c. */
static void
sp_region_setup(void)
{
	if (sp_region != 0) {
		return;
	}
	sp_region = (uintptr_t)gPanicBase;
	sp_region_size = gPanicSize;
	if (sp_region == 0 ||
	    sp_region_size < SP_SLOT_SIZE + sizeof(struct sp_slot)) {
		sp_region = 0;
		sp_region_size = 0;
		return;
	}
	sp_slot = (struct sp_slot *)(uintptr_t)(sp_region +
	    (sp_region_size - SP_SLOT_SIZE));
}

/*
 * Write our copy of the panic text into the reserved region.
 */
static void
sp_region_store(const char *text, unsigned int len, uint64_t caller)
{
	unsigned int max;

	if (sp_slot == NULL) {
		return;
	}
	max = SP_SLOT_SIZE - (unsigned int)sizeof(struct sp_slot);
	if (len > max) {
		len = max;
	}
	_disable_preemption();
	sp_slot->magic = SP_MAGIC;
	sp_slot->version = SP_VERSION;
	sp_slot->len = len;
	sp_slot->crc = crc32(0, text, len);
	sp_slot->seq = ++sp_seq;
	sp_slot->time = mach_absolute_time();
	sp_slot->caller = caller;
	if (len > 0) {
		memcpy(SP_SLOT_TEXT(sp_slot), text, len);
	}
	_enable_preemption();
}

/* Read our copy back out of the reserved region. */
static unsigned int
sp_region_readback(char *out, unsigned int size)
{
	unsigned int len;

	if (sp_slot == NULL || out == NULL || size == 0) {
		return 0;
	}
	_disable_preemption();
	len = sp_slot->len;
	if (len > SP_SLOT_SIZE - (unsigned int)sizeof(struct sp_slot)) {
		len = 0;
	}
	if (len > size - 1) {
		len = size - 1;
	}
	if (len > 0) {
		memcpy(out, SP_SLOT_TEXT(sp_slot), len);
	}
	out[len] = '\0';
	_enable_preemption();
	return len;
}

/* Copy text out of the panic region, printable characters only. */
static void
sp_sanitize(const char *in, unsigned int in_len, char *out,
    unsigned int out_size, unsigned int *out_len)
{
	unsigned int i, n = 0;

	for (i = 0; i < in_len && n + 1 < out_size; i++) {
		unsigned char c = (unsigned char)in[i];

		out[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	out[n] = '\0';
	*out_len = n;
}

/* mach_absolute_time() ticks for a number of milliseconds. */
static uint64_t
sp_ms_to_ticks(uint32_t ms)
{
	mach_timebase_info_data_t tb;

	clock_timebase_info((mach_timebase_info_t)&tb);
	if (tb.denom == 0) {
		return 0;
	}
	return ((uint64_t)tb.numer * (uint64_t)ms * 1000000ULL) /
	    (uint64_t)tb.denom;
}

/*
 * Wait for the USB logger.  The panic path must not sleep, so this spins.
 * The other CPUs are still running at this point (they are halted later, in
 * handle_debugger_trap()), so the logger thread can still send the text.
 */
static void
sp_flush_wait(uint32_t ms)
{
	uint64_t ticks, deadline;

	ticks = sp_ms_to_ticks(ms);
	if (ticks == 0) {
		return;
	}
	deadline = mach_absolute_time() + ticks;
	while (mach_absolute_time() < deadline) {
		/* spin */
	}
}

/*
 * The panic path.  Called from osfmk/kern/debug.c (DebuggerSaveState) with
 * the panic format string and its arguments, before the other CPUs are
 * halted and long before the phone resets.
 */
void
sweet_panic_capture(int is_panic, const char *fmt, va_list *args,
    uint64_t caller)
{
	char text[SP_TEXT_MAX];
	char msg[SP_TEXT_MAX + 96];
	unsigned int len, back;

	sp_region_setup();

	len = sp_format(fmt, args, text, sizeof(text));
	if (len == 0) {
		return;
	}

	/*
	 * 1. the USB console ring: this is what tools/venus-udplog.py sees.
	 */
	(void)snprintf(msg, sizeof(msg), "%s: %s (caller 0x%llx)\n",
	    is_panic ? "SWEET PANIC" : "SWEET DEBUGGER", text,
	    (unsigned long long)caller);
	sp_ring(msg);

	/*
	 * 1b. the call chain of the panicking thread (unslid kernel addresses,
	 * symbolize with nm -n on the kernel image): "failed with 3" panics
	 * carry no hint about the caller otherwise.
	 */
	{
		uintptr_t bt[24];
		backtrace_info_t info = BTI_NONE;
		unsigned int n = backtrace(bt, 24, NULL, &info), i;
		char bl[24 * 20 + 32];
		unsigned int off = 0;

		off += (unsigned int)snprintf(bl + off, sizeof(bl) - off,
		    "SWEET PANIC: bt");
		for (i = 0; i < n && off + 20 < sizeof(bl); ++i) {
			off += (unsigned int)snprintf(bl + off, sizeof(bl) - off,
			    " 0x%lx", (unsigned long)bt[i]);
		}
		(void)snprintf(bl + off, sizeof(bl) - off, "\n");
		sp_ring(bl);
	}

	/*
	 * 2. the console and the XNU panic log.  paniclog_append_noflush() is
	 * the panic safe writer: it does not take the console lock and it
	 * writes into the panic region as well.
	 */
	paniclog_append_noflush("SWEET PANIC: %s\n", text);

	/*
	 * 3. our own copy in the reserved region, so that the next boot can
	 *    print why the phone died.
	 */
	if (is_panic && !sp_captured) {
		sp_captured = 1;
		if (sp_slot == NULL) {
			sp_ring("SWEET PANIC: no reserved panic region "
			    "(/pram is missing)\n");
		} else {
			sp_region_store(text, len, caller);

			/*
			 * 4. read it back: the log must say whether the copy
			 *    is in RAM.
			 */
			back = sp_region_readback(msg, sizeof(msg));
			if (back == len && memcmp(msg, text, len) == 0) {
				sp_ring("SWEET PANIC: panic text saved in the "
				    "reserved panic region\n");
			} else {
				sp_ring("SWEET PANIC: the reserved panic "
				    "region copy FAILED\n");
			}
		}
	}

	/*
	 * 5. give the USB logger the time to send the text before the phone
	 *    resets.  Only once: a nested panic must not add to the delay.
	 */
	if (!sp_flushed) {
		sp_flushed = 1;
		sp_flush_wait(sp_flush_ms);
	}
}

/*
 * Called from pexpert/arm/pe_init.c just before the panic region is cleared
 * at boot: the region still holds the panic of the previous boot.
 */
void
sweet_panic_previous_boot(uintptr_t region, unsigned int size)
{
	struct sp_slot *slot;
	struct embedded_panic_header *hdr;
	unsigned int len;

	if (region == 0 || size < SP_SLOT_SIZE + sizeof(struct sp_slot)) {
		return;
	}

	/* our own copy of the previous panic */
	slot = (struct sp_slot *)(uintptr_t)(region + (size - SP_SLOT_SIZE));
	if (slot->magic == SP_MAGIC && slot->version == SP_VERSION) {
		len = slot->len;
		if (len > SP_SLOT_SIZE - (unsigned int)sizeof(struct sp_slot)) {
			len = 0;
		}
		if (len > SP_TEXT_MAX - 1) {
			len = SP_TEXT_MAX - 1;
		}
		if (len > 0) {
			memcpy(sp_prev_text, SP_SLOT_TEXT(slot), len);
			sp_prev_text[len] = '\0';
			sp_prev_len = len;
			/*
			 * The USB logger may already be running, so put the
			 * text into the ring as well: that is the copy the
			 * Mac receives.
			 */
			sp_ring("\nSWEET PANIC: (previous boot) ");
			sp_ring(sp_prev_text);
			sp_ring("\n");
		}
	}

	/* XNU's own panic log of the previous boot */
	hdr = (struct embedded_panic_header *)(uintptr_t)region;
	if (hdr->eph_magic == EMBEDDED_PANIC_MAGIC) {
		uint32_t off = hdr->eph_panic_log_offset;
		uint32_t plen = hdr->eph_panic_log_len;

		if (off != 0 && plen != 0 &&
		    (uint64_t)off + (uint64_t)plen <= (uint64_t)size) {
			sp_sanitize((const char *)(uintptr_t)(region + off),
			    plen, sp_prev_xnu, sizeof(sp_prev_xnu),
			    &sp_prev_xnu_len);
		}
	}
}

/*
 * The boot arguments.  They are read as early as possible (the console is
 * up long before the root device is), because the panic path uses the flush
 * time.
 */
static void
sp_parse_args(void)
{
	(void)PE_parse_boot_argn("sweetpanicflush", &sp_flush_ms,
	    sizeof(sp_flush_ms));
	(void)PE_parse_boot_argn("sweetpanicgrace", &sp_grace_ms,
	    sizeof(sp_grace_ms));
	(void)PE_parse_boot_argn("sweetpanicdelay", &sp_test_delay,
	    sizeof(sp_test_delay));
	if (sp_flush_ms > 10000u) {
		sp_flush_ms = SP_FLUSH_MS;
	}
	if (sp_grace_ms > 60000u) {
		sp_grace_ms = SP_GRACE_MS;
	}
	if (sp_test_delay > 600u) {
		sp_test_delay = SP_TEST_DELAY;
	}
}

/*
 * Called from pexpert/arm/pe_init.c once the console is up: print the panic
 * of the previous boot, so that a photo of the screen is enough.
 */
void
sweet_panic_console_up(void)
{
	sp_parse_args();

	if (sp_prev_len == 0 && sp_prev_xnu_len == 0) {
		return;
	}
	printf("SWEET PANIC: the previous boot panicked:\n");
	if (sp_prev_len > 0) {
		printf("SWEET PANIC: %s\n", sp_prev_text);
	}
	if (sp_prev_xnu_len > 0) {
		printf("SWEET PANIC: (XNU panic log) %s\n", sp_prev_xnu);
	}
	printf("SWEET PANIC: (end of the previous panic)\n");
}

/*
 * PSCI SYSTEM_RESET (SMCCC 0xC4000009): a warm reset of the SoC.  Same
 * call as bsd/dev/sweet_reboot.c uses.
 */
static void
sp_psci_reset(void)
{
	register uint64_t x0 __asm__("x0") = 0xC4000009ULL;
	register uint64_t x1 __asm__("x1") = 0ULL;   /* reset type: system reset */

	__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1) :
	    : "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
	    "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
}

/*
 * The fastboot cookie could not be set (see the audit of patch 0102): the
 * bootloader would boot the "boot" partition instead of fastboot.  The
 * phone must come back by itself all the same, so reset it after a grace
 * period.  A plain reset is recoverable: the phone comes up in fastboot or
 * with the bootloader, not with a dead phone on a panic screen.
 */
void
sweet_panic_fallback_reset(void)
{
	uint64_t deadline, ticks;

	sp_ring("SWEET PANIC: no fastboot cookie - plain reset\n");
	ticks = sp_ms_to_ticks(sp_grace_ms);
	deadline = mach_absolute_time() + ticks;
	while (mach_absolute_time() < deadline) {
		/* spin: the panic path must not sleep */
	}
	sp_ring("SWEET PANIC: PSCI SYSTEM_RESET (plain)\n");
	__asm__ volatile("dsb sy" ::: "memory");
	sp_psci_reset();
	/*
	 * The firmware did not take the reset: the caller keeps the phone on
	 * the panic screen.
	 */
}

/* The test panic: sweetpanic=1 panics the kernel on purpose. */
__attribute__((noreturn))
static void
sp_test_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t deadline;

	clock_interval_to_deadline(sp_test_delay, (uint32_t)NSEC_PER_SEC,
	    &deadline);
	(void)assert_wait_deadline(&sp_test_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
	printf("SWEET PANIC: test panic (sweetpanic=1)\n");
	panic("SWEET PANIC: test panic (sweetpanic=1)");
}

/*
 * Called from bsd/dev/sweet_ufs.c once the root device is up: the boot
 * arguments and the test panic.
 */
void
sweet_panic_init(void)
{
	uint32_t v;
	thread_t thread;

	sp_region_setup();
	sp_parse_args();

	if (sp_region == 0) {
		printf("SWEET PANIC: no reserved panic region (/pram) in the "
		    "device tree\n");
	}

	v = 0;
	if (PE_parse_boot_argn("sweetpaniclast", &v, sizeof(v)) && v != 0 &&
	    sp_prev_len == 0 && sp_prev_xnu_len == 0) {
		printf("SWEET PANIC: no previous panic in the reserved region\n");
	}

	v = 0;
	if (PE_parse_boot_argn("sweetpanic", &v, sizeof(v)) && v != 0) {
		printf("SWEET PANIC: test panic in %u s (sweetpanic=1)\n",
		    sp_test_delay);
		if (kernel_thread_start(sp_test_thread, NULL, &thread) ==
		    KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
}

#endif /* ARM64_BOARD_CONFIG_SWEET */
