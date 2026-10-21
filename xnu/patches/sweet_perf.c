/*
 * SWEET performance profiler - venus (Xiaomi Mi 11).
 *
 * WHY THIS FILE EXISTS
 * --------------------
 * The measured boot of venus is fast inside the kernel and slow outside it:
 * launchd says "hello" -> "Early boot complete" in 2.81 s, but the first iOS
 * frame appears 56.4 s later (logs/research/22-performance-budget.md, the
 * measured timeline in section 2).  The kernel prints NO timestamps, so from
 * the outside those 56 seconds cannot be attributed to anything: the report
 * had to interpolate them from the launchd wall clock and from the two
 * kernel tick counters.
 *
 * This file adds the missing kernel side measurement.  It has three parts,
 * all of them opt-in and all of them cheap:
 *
 *  1. A BOOT PHASE PROFILE.  The profiler watches the kernel console ring
 *     (the same 4 MiB ring the USB live logger reads) and timestamps the
 *     phases that matter: the root volume mount, the exec of launchd,
 *     launchd's "Early boot complete", the exec of Setup and of SpringBoard
 *     and the first frame presented.  When the boot is over it prints the
 *     table, so the 56 s can be split into "launchd was busy" and "the
 *     daemon storm" from the kernel's own point of view.
 *
 *  2. A RUNTIME SAMPLER.  Every sweetperfint seconds it prints one line
 *     with the console line rate (counted exactly in the ring, not
 *     estimated), the number of processes and of threads (the daemon storm
 *     of report 22 shows up as a rising process count) and the cost of the
 *     UFS SYNCHRONIZE CACHE.
 *
 *  3. sweet_perf_ufs_sync_hook(): the wrapper the UFS driver calls around
 *     its whole-LUN SYNCHRONIZE CACHE.  It measures how long the flush
 *     blocks the global UFS mutex (metric B11 of report 22) and can skip
 *     the flush (sweetufssync=<secs>).
 *
 * EVERYTHING IS OFF BY DEFAULT.  Without boot-arg sweetperf=1 this file
 * starts no thread, prints nothing and changes nothing.
 *
 * BOOT ARGS
 *   sweetperf=1        enable the profiler (default off)
 *   sweetperfint=<s>   sampler period in seconds (default 5, 1..3600)
 *   sweetperfboot=<s>  how long the boot profile watches for the phase
 *                       markers (default 120, 0 = watch forever)
 *   sweetufssync=<s>   minimum number of seconds between two whole-LUN
 *                       SYNCHRONIZE CACHE calls of the persistent log
 *                       flusher (0 = keep the driver's behaviour, default)
 *
 * The UFS driver calls the hook like this (one line, see
 * logs/research/impl/19-perf.md section 4):
 *
 *	-		if (!error) {
 *-			error = ufs_sync_cache();
 *-		}
 *+		if (!error) {
 *+			error = sweet_perf_ufs_sync_hook(ufs_sync_cache);
 *+		}
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>

/* bsd/dev/sweet_ufs.c: the console ring every console line goes through. */
extern uint32_t sweet_log_copy(uint64_t *pos, char *out, uint32_t max);
/* bsd/kern/proc.c */
extern task_t proc_task(proc_t);
/* osfmk/kern/processor.h: the global task and thread counters */
extern int tasks_count;
extern int threads_count;
extern int hz;

#define SWEET_PERF_LINE_MAX      256
#define SWEET_PERF_MAX_PHASES    24
#define SWEET_PERF_POLL_MS       200

/* The public entry points of this file. */
void sweet_perf_init(void);
void sweet_perf_mark(const char *name);
int sweet_perf_ufs_sync_hook(int (*do_sync)(void));

/*
 * The phases the console is watched for.  "exact" matches the whole line
 * (the exec log of patch 0042 prints "exec <name>"), "needle" matches a
 * substring of the line.
 */
struct sweet_perf_phase {
	const char *needle;
	const char *exact;
	const char *label;
	uint64_t   at;
	bool       seen;
};

static struct sweet_perf_phase sweet_perf_phases[SWEET_PERF_MAX_PHASES] = {
	{ "hfs: mounted iosroot", NULL, "root volume mounted", 0, false },
	{ NULL, "exec launchd", "launchd exec'd", 0, false },
	{ "Early boot complete", NULL, "launchd early boot complete", 0, false },
	{ NULL, "exec Setup", "Setup exec'd (activation)", 0, false },
	{ NULL, "exec SpringBoard", "SpringBoard exec'd", 0, false },
	{ "SWEET FB: display on", NULL, "first frame presented", 0, false },
};

static bool     sweet_perf_on;
static int      sweet_perf_int_secs = 5;
static int      sweet_perf_boot_secs = 120;
static int      sweet_perf_sync_secs;         /* 0 = keep the driver's behaviour */
static uint64_t sweet_perf_t0;                /* mach_absolute_time() at init */
static uint64_t sweet_perf_ticks_per_ms = 1;
static uint64_t sweet_perf_pos;               /* read position in the console ring */
static uint64_t sweet_perf_lines;
static uint64_t sweet_perf_bytes;
static int      sweet_perf_chan;
static char     sweet_perf_line[SWEET_PERF_LINE_MAX];
static int      sweet_perf_line_len;
static bool     sweet_perf_table_printed;
static int      sweet_perf_nphases = 6;

/* UFS SYNCHRONIZE CACHE statistics */
static uint64_t sweet_perf_sync_count;
static uint64_t sweet_perf_sync_skipped;
static uint64_t sweet_perf_sync_total_ns;
static uint64_t sweet_perf_sync_max_ns;
static uint64_t sweet_perf_sync_last;

/* ------------------------------------------------------------------ time */

static void
sweet_perf_timebase(void)
{
	mach_timebase_info_data_t tb;

	tb.numer = 0;
	tb.denom = 0;
	clock_timebase_info(&tb);
	if (tb.denom != 0) {
		/* ticks -> ns: ticks * numer / denom */
		sweet_perf_ticks_per_ms = ((uint64_t)tb.numer * 1000000ULL) / (uint64_t)tb.denom;
	}
	if (sweet_perf_ticks_per_ms == 0) {
		sweet_perf_ticks_per_ms = 1;
	}
}

/* Milliseconds for a delta of mach_absolute_time() ticks. */
static uint64_t
sweet_perf_ms(uint64_t ticks)
{
	return ticks / sweet_perf_ticks_per_ms;
}

static uint64_t
sweet_perf_ns(uint64_t ticks)
{
	return (ticks * 1000000ULL) / sweet_perf_ticks_per_ms;
}

/* ---------------------------------------------------------------- console */

/*
 * Look at one complete console line and note the phase markers in it.
 */
static void
sweet_perf_check_line(const char *line)
{
	for (int i = 0; i < sweet_perf_nphases; i++) {
		struct sweet_perf_phase *p = &sweet_perf_phases[i];

		if (p->seen) {
			continue;
		}
		if (p->exact != NULL) {
			if (strcmp(line, p->exact) == 0) {
				p->at = mach_absolute_time();
				p->seen = true;
			}
		} else if (p->needle != NULL &&
		    strnstr(line, p->needle, strlen(line)) != NULL) {
			p->at = mach_absolute_time();
			p->seen = true;
		}
	}
}

/*
 * Drain the console ring: count the lines and the bytes and look for the
 * phase markers.  The ring is the same one the USB live logger reads, so
 * this costs one memcpy out of the ring per poll and nothing else.
 */
static void
sweet_perf_pump(void)
{
	char buf[1024];
	uint32_t n;

	while ((n = sweet_log_copy(&sweet_perf_pos, buf, sizeof(buf))) > 0) {
		sweet_perf_bytes += n;
		for (uint32_t i = 0; i < n; i++) {
			char c = buf[i];

			if (c == '\n') {
				sweet_perf_line[sweet_perf_line_len] = 0;
				sweet_perf_lines++;
				sweet_perf_check_line(sweet_perf_line);
				sweet_perf_line_len = 0;
			} else if (sweet_perf_line_len < SWEET_PERF_LINE_MAX - 1) {
				sweet_perf_line[sweet_perf_line_len++] = c;
			}
		}
	}
}

/* ----------------------------------------------------------------- counts */

struct sweet_perf_counts {
	int procs;
	int threads;
};

static int
sweet_perf_proc_count(__unused proc_t p, void *arg)
{
	struct sweet_perf_counts *c = arg;

	c->procs++;
	return PROC_RETURNED;
}

static void
sweet_perf_get_counts(struct sweet_perf_counts *c)
{
	c->procs = 0;
	c->threads = 0;
	/*
	 * PROC_NOWAITTRANS: never block the sampler behind a process that
	 * is in transition, the count is then a lower bound.
	 */
	proc_iterate(PROC_ALLPROCLIST | PROC_NOWAITTRANS, sweet_perf_proc_count, c, NULL, NULL);
	/* the global counters of osfmk/kern/processor.h */
	c->threads = threads_count;
}

/* ------------------------------------------------------------------ table */

/*
 * Print the phase table: every phase that was seen, with the time since
 * the profiler was armed.  The phases are printed in the order they
 * happened, not in the order they are listed.
 */
static void
sweet_perf_print_table(const char *why)
{
	uint64_t at[SWEET_PERF_MAX_PHASES];
	const char *label[SWEET_PERF_MAX_PHASES];
	int n = 0;

	for (int i = 0; i < sweet_perf_nphases; i++) {
		if (!sweet_perf_phases[i].seen) {
			continue;
		}
		at[n] = sweet_perf_phases[i].at;
		label[n] = sweet_perf_phases[i].label;
		n++;
	}
	/* insertion sort, n is tiny */
	for (int i = 1; i < n; i++) {
		uint64_t a = at[i];
		const char *l = label[i];
		int j = i - 1;

		while (j >= 0 && at[j] > a) {
			at[j + 1] = at[j];
			label[j + 1] = label[j];
			j--;
		}
		at[j + 1] = a;
		label[j + 1] = l;
	}
	printf("SWEET PERF: boot phases (%s), t0 = profiler armed, root device attached:\n", why);
	for (int i = 0; i < n; i++) {
		printf("SWEET PERF:   +%6llu ms  %s\n",
		    (unsigned long long)sweet_perf_ms(at[i] - sweet_perf_t0), label[i]);
	}
	if (n == 0) {
		printf("SWEET PERF:   (no phase marker seen)\n");
	}
	sweet_perf_table_printed = true;
}

/* ----------------------------------------------------------------- thread */

static void sweet_perf_thread(__unused void *arg, __unused wait_result_t wr) __dead2;

static void
sweet_perf_thread(__unused void *arg, __unused wait_result_t wr)
{
	uint64_t last = mach_absolute_time();
	uint64_t last_lines = 0;
	uint64_t last_bytes = 0;
	uint64_t boot_end = 0;

	for (;;) {
		uint64_t now;
		uint64_t period;

		(void)tsleep(&sweet_perf_chan, PZERO, "sweetperf", hz * SWEET_PERF_POLL_MS / 1000);
		sweet_perf_pump();

		now = mach_absolute_time();
		period = (uint64_t)sweet_perf_ticks_per_ms * 1000ULL *
		    (uint64_t)sweet_perf_int_secs;

		if (now - last < period) {
			/*
			 * The boot table is printed when the boot is over: as
			 * soon as the first frame was presented (plus 3 s) or
			 * when the boot window is over.
			 */
			if (!sweet_perf_table_printed && boot_end == 0) {
				for (int i = 0; i < sweet_perf_nphases; i++) {
					if (sweet_perf_phases[i].seen &&
					    strcmp(sweet_perf_phases[i].label,
					    "first frame presented") == 0) {
						boot_end = now;
						break;
					}
				}
				if (boot_end != 0 &&
				    now - boot_end >= (uint64_t)sweet_perf_ticks_per_ms * 3000ULL) {
					sweet_perf_print_table("first frame presented");
					boot_end = 0;
				}
			}
			if (!sweet_perf_table_printed && sweet_perf_boot_secs > 0 &&
			    sweet_perf_ms(now - sweet_perf_t0) >
			    (uint64_t)sweet_perf_boot_secs * 1000ULL) {
				sweet_perf_print_table("boot window over");
			}
			continue;
		}

		struct sweet_perf_counts c;
		uint64_t dt;
		uint64_t ms;
		uint64_t lines_per_s;
		uint64_t bytes_per_s;
		uint64_t avg_us;

		sweet_perf_get_counts(&c);
		dt = now - last;
		ms = sweet_perf_ms(dt);
		lines_per_s = ms ? (sweet_perf_lines - last_lines) * 1000ULL / ms : 0;
		bytes_per_s = ms ? (sweet_perf_bytes - last_bytes) * 1000ULL / ms : 0;
		avg_us = sweet_perf_sync_count ?
		    sweet_perf_sync_total_ns / sweet_perf_sync_count / 1000 : 0;

		printf("SWEET PERF: t=%llums console=%llu lines/s %llu B/s procs=%d threads=%d "
		    "ufsync=%llu skipped=%llu max=%lluus avg=%lluus\n",
		    (unsigned long long)sweet_perf_ms(now - sweet_perf_t0),
		    (unsigned long long)lines_per_s,
		    (unsigned long long)bytes_per_s,
		    c.procs, c.threads,
		    (unsigned long long)sweet_perf_sync_count,
		    (unsigned long long)sweet_perf_sync_skipped,
		    (unsigned long long)(sweet_perf_sync_max_ns / 1000),
		    (unsigned long long)avg_us);

		last = now;
		last_lines = sweet_perf_lines;
		last_bytes = sweet_perf_bytes;
	}
}

/* ------------------------------------------------------------------- api */

/*
 * Arm the profiler.  Called once from sweet_ufs_init() (the root device
 * attach), before the root volume is mounted, so that t0 is the moment the
 * kernel had the root device.
 */
void
sweet_perf_init(void)
{
	thread_t thread;
	uint32_t on = 0;

	if (!PE_parse_boot_argn("sweetperf", &on, sizeof(on)) || !on) {
		return;
	}
	PE_parse_boot_argn("sweetperfint", &sweet_perf_int_secs, sizeof(sweet_perf_int_secs));
	PE_parse_boot_argn("sweetperfboot", &sweet_perf_boot_secs, sizeof(sweet_perf_boot_secs));
	PE_parse_boot_argn("sweetufssync", &sweet_perf_sync_secs, sizeof(sweet_perf_sync_secs));
	if (sweet_perf_int_secs < 1 || sweet_perf_int_secs > 3600) {
		sweet_perf_int_secs = 5;
	}
	if (sweet_perf_boot_secs < 0 || sweet_perf_boot_secs > 3600) {
		sweet_perf_boot_secs = 120;
	}
	if (sweet_perf_sync_secs < 0 || sweet_perf_sync_secs > 3600) {
		sweet_perf_sync_secs = 0;
	}
	sweet_perf_timebase();
	sweet_perf_t0 = mach_absolute_time();
	sweet_perf_on = true;
	printf("SWEET PERF: profiler armed (int=%ds boot=%ds ufsync=%ds)\n",
	    sweet_perf_int_secs, sweet_perf_boot_secs, sweet_perf_sync_secs);
	if (kernel_thread_start(sweet_perf_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * An extra phase mark, for the caller that has a better idea of where it
 * is in the boot than the console text does.  sweet_ufs.c may call
 * sweet_perf_mark("ufs probe done") for example.
 */
void
sweet_perf_mark(const char *name)
{
	if (!sweet_perf_on || name == NULL) {
		return;
	}
	for (int i = 0; i < sweet_perf_nphases; i++) {
		struct sweet_perf_phase *p = &sweet_perf_phases[i];

		if (p->exact != NULL && strcmp(p->exact, name) == 0) {
			p->at = mach_absolute_time();
			p->seen = true;
			return;
		}
	}
	if (sweet_perf_nphases < SWEET_PERF_MAX_PHASES) {
		struct sweet_perf_phase *p = &sweet_perf_phases[sweet_perf_nphases++];

		p->needle = NULL;
		p->exact = name;
		p->label = name;
		p->at = mach_absolute_time();
		p->seen = true;
	}
}

/*
 * The wrapper around the UFS whole-LUN SYNCHRONIZE CACHE.
 *
 * The persistent log flusher of sweet_ufs.c issues the flush while it
 * holds the global UFS mutex, so every HFS+ read and write waits for it.
 * With ufslogsecs=2 that is 480 stalls per 16 minute session (report 22,
 * section 4.3).  This hook measures the cost (metric B11) and can skip
 * the flush when the previous one is not old enough yet.
 *
 * With the profiler off it is a plain pass through: the behaviour of the
 * driver does not change at all.
 */
int
sweet_perf_ufs_sync_hook(int (*do_sync)(void))
{
	uint64_t t0;
	uint64_t dt;
	uint64_t ns;
	int error;

	if (!sweet_perf_on || do_sync == NULL) {
		return do_sync != NULL ? do_sync() : 0;
	}
	t0 = mach_absolute_time();
	if (sweet_perf_sync_secs > 0 && sweet_perf_sync_last != 0 &&
	    sweet_perf_ms(t0 - sweet_perf_sync_last) <
	    (uint64_t)sweet_perf_sync_secs * 1000ULL) {
		sweet_perf_sync_skipped++;
		return 0;
	}
	error = do_sync();
	dt = mach_absolute_time() - t0;
	ns = sweet_perf_ns(dt);
	sweet_perf_sync_last = mach_absolute_time();
	sweet_perf_sync_count++;
	sweet_perf_sync_total_ns += ns;
	if (ns > sweet_perf_sync_max_ns) {
		sweet_perf_sync_max_ns = ns;
	}
	return error;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
