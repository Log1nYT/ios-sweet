/*
 * SWEET key state machine: debounce, long press and the Home/Menu key.
 *
 * bsd/dev/sweet_keys.c polls the PMIC every 20 ms and reports the raw
 * state of the power and volume keys.  Those transitions are noisy (the
 * PMIC key state bounces for a few ms around a real press) and they
 * carry no timing, so this file turns them into clean HID button events:
 *
 *   - debounce: a transition is only published after it has been stable
 *     for sweetkeydebounce ms (default 30).  A bounce that ends in the
 *     old state never produces an event at all.
 *   - long press: while a key stays down, a timer fires after
 *     sweetkeylong ms (default 1200, 0 disables it) and publishes one
 *     extra HID event - a second key down for the same usage pair - so
 *     that a userland which missed the first down still learns that the
 *     key is held.  The matching key up is published when the key is
 *     released, so the press duration (down -> up) stays exact and a
 *     userland that times the press itself is not disturbed.
 *   - home: index 3 (Consumer page 0x0C usage 0x40, "Menu") is handled
 *     like every other key.  Nothing feeds it: venus has no home button
 *     and the venus ADT sets home-button-type = 2, so iOS 16 treats the
 *     device as a Face ID phone without a home button and does not want
 *     one.  The home affordance on venus is the bottom edge swipe, which
 *     the touch driver already reports as an edge tip.
 *
 * The HID events themselves are still built by the IOKit layer
 * (sweet_hid_key() in iokit/Kernel/arm/hid/SweetHID.cpp); this file only
 * decides *when* they are published.  With sweetkeyevents=0 (the
 * default) every raw event is passed straight through, so the behaviour
 * is bit identical to the driver without this file.
 *
 * All timing is done by one kernel thread that waits with a timeout; the
 * driver thread only posts the raw state.  No work happens in an
 * interrupt context, and a timer only exists between a published key
 * down and the matching key up, so nothing can fire after the key was
 * released (or after the display was turned off).
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLocks.h>

#include "sweet_keyevents.h"

extern void sweet_hid_key(int key, int down);

#define KE_DEBOUNCE_DEFAULT_MS	30u
#define KE_LONG_DEFAULT_MS	1200u
#define KE_MAX_MS		60000u

/* What the state machine decided to publish for one key. */
#define KE_ACT_NONE		0
#define KE_ACT_UP		1
#define KE_ACT_DOWN		2
#define KE_ACT_LONG		3

typedef struct {
	uint8_t	raw;		/* last raw state seen from the driver */
	uint8_t	reported;	/* last state published to the HID layer */
	uint8_t	pending;	/* state waiting for the debounce window */
	bool	pending_valid;
	uint64_t pending_deadline;	/* mach_absolute_time() ticks */
	uint64_t long_deadline;		/* mach_absolute_time() ticks, 0 = off */
	bool	long_sent;
} ke_key_t;

static ke_key_t ke_keys[SWEET_KEY_COUNT];
static IOLock *ke_lock;
static int ke_chan;
static bool ke_running;
static bool ke_started;
static bool ke_display_off;
static bool ke_long_mark = true;
static uint32_t ke_debounce_ms = KE_DEBOUNCE_DEFAULT_MS;
static uint32_t ke_long_ms = KE_LONG_DEFAULT_MS;
static uint64_t ke_debounce_ticks;
static uint64_t ke_long_ticks;

/* mach_absolute_time() ticks for a number of milliseconds. */
static uint64_t
ke_ms_to_ticks(uint32_t ms)
{
	uint64_t ticks = 0;

	nanoseconds_to_absolutetime((uint64_t)ms * (uint64_t)NSEC_PER_MSEC, &ticks);
	return ticks;
}

/*
 * The state machine thread.  It sleeps until the earliest pending
 * deadline (a debounce window or a long press) and publishes what has
 * become due.  With nothing pending it waits on the channel, so an idle
 * system has no timers at all.
 *
 * The wait is armed while the lock is held: the driver thread updates
 * the state under the same lock, so a transition can never be lost
 * between the state check and the sleep.
 */
__attribute__((noreturn))
static void
ke_thread(void *arg, wait_result_t result)
{
	(void)arg;
	(void)result;

	for (;;) {
		uint64_t now = mach_absolute_time();
		uint64_t deadline = 0;
		bool timed = false;
		bool must_block;
		uint8_t action[SWEET_KEY_COUNT];
		int i;

		for (i = 0; i < SWEET_KEY_COUNT; i++) {
			action[i] = KE_ACT_NONE;
		}

		IOLockLock(ke_lock);
		for (i = 0; i < SWEET_KEY_COUNT; i++) {
			ke_key_t *s = &ke_keys[i];

			if (s->pending_valid && s->pending_deadline <= now) {
				uint8_t st = s->pending;

				s->pending_valid = false;
				if (st != s->reported) {
					s->reported = st;
					if (st) {
						s->long_sent = false;
						s->long_deadline = 0;
						if (ke_long_ms && !ke_display_off) {
							s->long_deadline = now + ke_long_ticks;
						}
						action[i] = KE_ACT_DOWN;
					} else {
						/* the key is gone: no timer may
						 * survive the release */
						s->long_deadline = 0;
						action[i] = KE_ACT_UP;
					}
				}
			}
			if (s->long_deadline != 0 && s->long_deadline <= now) {
				s->long_deadline = 0;
				if (!s->long_sent && !ke_display_off &&
				    s->reported && action[i] == KE_ACT_NONE) {
					s->long_sent = true;
					action[i] = KE_ACT_LONG;
				}
			}
			if (s->pending_valid &&
			    (!timed || s->pending_deadline < deadline)) {
				deadline = s->pending_deadline;
				timed = true;
			}
			if (s->long_deadline != 0 &&
			    (!timed || s->long_deadline < deadline)) {
				deadline = s->long_deadline;
				timed = true;
			}
		}
		if (timed) {
			must_block = assert_wait_deadline(&ke_chan, THREAD_UNINT,
			    deadline) == THREAD_WAITING;
		} else {
			must_block = assert_wait(&ke_chan, THREAD_UNINT) == THREAD_WAITING;
		}
		IOLockUnlock(ke_lock);

		for (i = 0; i < SWEET_KEY_COUNT; i++) {
			switch (action[i]) {
			case KE_ACT_UP:
				sweet_hid_key(i, 0);
				break;
			case KE_ACT_DOWN:
				sweet_hid_key(i, 1);
				break;
			case KE_ACT_LONG:
				/*
				 * The press and hold marker: a second down
				 * for the same usage pair.  The key up
				 * follows when the key is released, so
				 * the press duration is unchanged.
				 */
				if (ke_long_mark) {
					sweet_hid_key(i, 1);
				}
				printf("SWEET KEYS: long press key=%d after %u ms\n",
				    i, ke_long_ms);
				break;
			default:
				break;
			}
		}

		if (must_block) {
			thread_block(THREAD_CONTINUE_NULL);
		}
	}
}

void
sweet_keyevents_key(int key, int down)
{
	ke_key_t *s;
	uint8_t raw;
	bool changed = false;

	if (key < 0 || key >= SWEET_KEY_COUNT) {
		return;
	}
	if (!ke_running) {
		/* The state machine is off or not running: the raw event
		 * goes straight to the HID layer, exactly as before. */
		sweet_hid_key(key, down);
		return;
	}

	raw = down ? 1 : 0;
	s = &ke_keys[key];

	IOLockLock(ke_lock);
	if (raw != s->raw) {
		s->raw = raw;
		s->pending = raw;
		s->pending_valid = true;
		s->pending_deadline = mach_absolute_time() + ke_debounce_ticks;
		changed = true;
	}
	IOLockUnlock(ke_lock);

	if (changed) {
		thread_wakeup(&ke_chan);
	}
}

void
sweet_keyevents_set_display_off(int off)
{
	int i;

	if (!ke_running) {
		return;
	}
	IOLockLock(ke_lock);
	ke_display_off = off ? true : false;
	if (ke_display_off) {
		/* Nothing may fire while the display is off. */
		for (i = 0; i < SWEET_KEY_COUNT; i++) {
			ke_keys[i].long_deadline = 0;
		}
	}
	IOLockUnlock(ke_lock);
}

void
sweet_keyevents_init(void)
{
	thread_t thread;
	int enabled = 0;
	int display_off = 0;
	int long_mark = 1;
	uint32_t ms = 0;

	if (ke_started) {
		return;
	}
	ke_started = true;

	(void)PE_parse_boot_argn("sweetkeyevents", &enabled, sizeof(enabled));
	if (PE_parse_boot_argn("sweetkeydebounce", &ms, sizeof(ms)) &&
	    ms <= KE_MAX_MS) {
		ke_debounce_ms = ms;
	}
	ms = 0;
	if (PE_parse_boot_argn("sweetkeylong", &ms, sizeof(ms)) && ms <= KE_MAX_MS) {
		ke_long_ms = ms;
	}
	(void)PE_parse_boot_argn("sweetkeylongmark", &long_mark,
	    sizeof(long_mark));
	ke_long_mark = long_mark ? true : false;
	(void)PE_parse_boot_argn("sweetkeydisplayoff", &display_off,
	    sizeof(display_off));
	ke_display_off = display_off ? true : false;

	printf("SWEET KEYS: key state machine %s (debounce %u ms, long press %u ms)\n",
	    enabled ? "on" : "off", ke_debounce_ms, ke_long_ms);

	if (!enabled) {
		return;
	}
	ke_lock = IOLockAlloc();
	if (!ke_lock) {
		printf("SWEET KEYS: key state machine lock failed, raw events only\n");
		return;
	}
	ke_debounce_ticks = ke_ms_to_ticks(ke_debounce_ms);
	ke_long_ticks = ke_ms_to_ticks(ke_long_ms);
	if (kernel_thread_start(ke_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		ke_running = true;
	} else {
		printf("SWEET KEYS: key state machine thread failed, raw events only\n");
	}
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
