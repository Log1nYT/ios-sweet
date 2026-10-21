/*
 * SWEET power + thermal probe - venus (Xiaomi Mi 11, SM8350).
 *
 * WHAT THIS IS: a READ ONLY probe that samples the PMICs over SPMI and
 * keeps the result in memory for the IOKit side
 * (iokit/Kernel/arm/SweetPowerInfo.cpp) and for the console.
 *
 * WHAT IT READS
 *
 *  - the battery PMIC (PM8350B = SPMI SID 3 on the venus board):
 *      0x110b POWER_PATH_STATUS  external power, valid source, suspend
 *      0x1006 BATTERY_CHARGER_STATUS_1  charge state 0..7
 *      0x1007 BATTERY_CHARGER_STATUS_2  error bits (bit1 = pack OV)
 *      0x100d BATTERY_CHARGER_STATUS_7  pack too hot / too cold
 *      0x1308 APSD_RESULT  adapter type
 *      0x1309 QC_CHANGE     negotiated USB voltage
 *      0x4776 BCL_VBAT_READ  the REAL battery voltage (49.827 mV per code)
 *      0x4786 BCL_IBAT_READ  the REAL battery current (78.127 mA per code)
 *
 *  - the PMIC temperature alarm blocks (SPMI peripheral 0x0a, SID 1..5):
 *      0xa01 STATUS   the over temperature alarm state
 *      0xa02..0xa04   the three trip thresholds (TH1/TH2/TH3)
 *
 *  - the PMK8350 VADC / ADC7 (SPMI SID 0, peripheral 0x31):
 *      0x3100 REVISION, 0x3108 STATUS1, 0x3144 CH_SEL_CTL,
 *      0x3150 DATA0, 0x3151 DATA1 - the last conversion of the die
 *      temperature channels (0x003 PMK8350, 0x103 PM8350, 0x303 PM8350B,
 *      0x403 PMR735A, 0x503 PMR735B).
 *
 * READ ONLY BY DESIGN: there is not a single store into a PMIC register
 * in this file.  The BCL v5 block is initialised (0x4746 / 0x4759) by
 * SweetBattery (patch 0104), which owns the write path; this probe only
 * reads what that block produces.
 *
 * Boot arguments (all optional, the probe is off by default):
 *   sweetthermal=1     the thermal probe (default: off)
 *   sweetbattery=1     the battery probe (default: off; SweetBattery uses
 *                       the same argument for its own polling, so both
 *                       services publish the same numbers)
 *   sweetbatsid=<0..5> the SPMI SID of the battery PMIC (default 3)
 *   sweetbattmah=<mAh> the pack size (default 4600 on venus)
 *
 * Sources: logs/research/06-power-battery-charging.md,
 *          logs/research/26-thermal-plan.md
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <IOKit/IOLocks.h>
#include "sweet_spmi.h"
#include "sweet_powerinfo.h"

extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern void thread_deallocate(thread_t);
extern int hz;

/* The IOKit service (iokit/Kernel/arm/SweetPowerInfo.cpp) is published
 * from here: the BSD layer cannot create an IOService itself. */
extern void sweet_powerinfo_publish(void);

/* ------------------------------------------------------------------ */
/* Register map                                                        */
/* ------------------------------------------------------------------ */

/* PMK8350 VADC (ADC7): SPMI SID 0, peripheral 0x31. */
#define PI_ADC7_REV      0x3100u
#define PI_ADC7_STATUS1  0x3108u
#define PI_ADC7_STATUS2  0x3109u
#define PI_ADC7_APP_SID  0x3140u
#define PI_ADC7_DIG_PARAM 0x3142u
#define PI_ADC7_FAST_AVG 0x3143u
#define PI_ADC7_CH_SEL   0x3144u
#define PI_ADC7_DELAY    0x3145u
#define PI_ADC7_EN_CTL1  0x3146u
#define PI_ADC7_CONV_REQ 0x3147u
#define PI_ADC7_DATA0    0x3150u
#define PI_ADC7_DATA1    0x3151u

/* PMIC temperature alarm: SPMI peripheral 0x0a (base 0xa00). */
#define PI_TALARM_BASE    0x0a00u
#define PI_TALARM_STATUS  0x0a01u
#define PI_TALARM_TH1     0x0a02u
#define PI_TALARM_TH2     0x0a03u
#define PI_TALARM_TH3     0x0a04u
#define PI_TALARM_LVL_SEL 0x0a05u

/* SMB5 charger and BCL v5 block of the battery PMIC. */
#define PI_CHGR_STATUS1  0x1006u
#define PI_CHGR_STATUS2  0x1007u
#define PI_CHGR_STATUS7  0x100du
#define PI_POWER_PATH    0x110bu
#define PI_APSD_RESULT   0x1308u
#define PI_QC_CHANGE     0x1309u
#define PI_BCL_VBAT_READ 0x4776u
#define PI_BCL_IBAT_READ 0x4786u

/* POWER_PATH_STATUS bits. */
#define PI_PP_USBIN   0x10u
#define PI_PP_DCIN    0x08u
#define PI_PP_VALID   0x01u
#define PI_PP_SUSPEND 0x40u

/* BATTERY_CHARGER_STATUS_1 charge states. */
#define PI_CHG_INHIBIT   0u
#define PI_CHG_TAPER     4u
#define PI_CHG_TERMINATE 5u

/* The ADC7: vref 1875 mV, full scale code 0x70e4. */
#define PI_ADC7_VREF_UV   1875000u
#define PI_ADC7_FULLSCALE 0x70e4u

/* The die temperature sensor of the PMIC: 1.500 V at 25 degC, 10 mV/degC.
 * This is an APPROXIMATION (the datasheet curve is a table); the raw code
 * and the microvolt value are published next to it so that the number can
 * be calibrated on the phone.  See report 26, question Q3/Q6. */
#define PI_DIE_TEMP_UV_25DEG 1500000
#define PI_DIE_TEMP_UV_PER_DEG 10000

/* The thermal trips (report 26, section 5.C): 45 / 55 / 65 / 75 degC. */
#define PI_TRIP_LIGHT_MC    45000
#define PI_TRIP_MODERATE_MC 55000
#define PI_TRIP_HEAVY_MC    65000
#define PI_TRIP_CRITICAL_MC 75000

/* The pack size of the Mi 11 (venus). */
#define PI_MAH_VENUS 4600u
#define PI_MAH_SWEET 5020u

/* The probe period. */
#define PI_PERIOD_SECS 5u

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef struct {
	unsigned mv;		/* battery voltage in mV                */
	int ma;			/* battery current in mA, + = charging  */
	unsigned flags;		/* SWEET_PI_F_*                        */
	unsigned percent;	/* voltage derived SOC estimate, 0..100 */
	unsigned capacity;	/* pack size in mAh                     */
} pi_battery_t;

typedef struct {
	unsigned temp_mC;	/* derived die temperature, 0 = unknown */
	unsigned code;		/* raw ADC code                        */
	unsigned uv;		/* ADC voltage in microvolts            */
	unsigned alarm;		/* temperature alarm status, 0xff = ?  */
	unsigned thr[3];	/* the three trip thresholds            */
	unsigned status;	/* the ADC7 status bytes                */
	unsigned valid;		/* a sample was read                   */
} pi_thermal_t;

static IOLock *pi_lock;
static pi_battery_t pi_battery;
static pi_thermal_t pi_thermal[SWEET_PI_PMIC_SIDS + 1];
static unsigned pi_level;
static unsigned pi_sid = 3;
static unsigned pi_capacity = PI_MAH_VENUS;
static bool pi_thermal_on;
static bool pi_battery_on;
static bool pi_alarm_warned;
static int pi_chan;

/* The die temperature channel of every PMIC that has one (venus DT). */
static const struct {
	unsigned sid;
	unsigned channel;
} pi_die_temp[SWEET_PI_PMIC_SIDS] = {
	{ 0, 0x003u },	/* PMK8350  */
	{ 1, 0x103u },	/* PM8350   */
	{ 3, 0x303u },	/* PM8350B  */
	{ 4, 0x403u },	/* PMR735A  */
	{ 5, 0x503u },	/* PMR735B  */
};

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static bool
pi_read(unsigned sid, unsigned addr, uint8_t *out)
{
	return sweet_spmi_read(sid, addr, out) == 0;
}

/*
 * The generic single cell state of charge curve.  It is the same curve as
 * the one SweetBattery uses, so that both services publish the same
 * number; it is an ESTIMATE (there is no fuel gauge on this board, the
 * real gauge lives behind the ADSP charger protection domain).
 */
static unsigned
pi_estimate_percent(unsigned mv)
{
	static const unsigned volts[] = {3400, 3500, 3600, 3700, 3800, 3900,
	    4000, 4100, 4200, 4350};
	static const unsigned soc[] = {1, 5, 10, 20, 40, 60, 75, 85, 93, 100};
	unsigned i;

	if (mv <= volts[0]) {
		return soc[0];
	}
	for (i = 1; i < sizeof(volts) / sizeof(volts[0]); ++i) {
		if (mv < volts[i]) {
			return soc[i - 1] + (mv - volts[i - 1]) * (soc[i] - soc[i - 1]) /
			       (volts[i] - volts[i - 1]);
		}
	}
	return 100;
}

/* ------------------------------------------------------------------ */
/* The battery sample                                                  */
/* ------------------------------------------------------------------ */

static void
pi_sample_battery(void)
{
	pi_battery_t sample;
	uint8_t path = 0, status1 = 0, status2 = 0, status7 = 0;
	uint8_t vbat = 0, ibat = 0;
	bool pathOk, st1Ok, st2Ok, st7Ok, vbatOk, ibatOk;
	unsigned state;
	bool external, capable, charging, full, error, tooHot;
	unsigned mv;
	int pmicMa;

	sample.mv = pi_battery.mv;
	sample.ma = pi_battery.ma;
	sample.flags = pi_battery.flags;
	sample.percent = pi_battery.percent;
	sample.capacity = pi_capacity;

	pathOk = pi_read(pi_sid, PI_POWER_PATH, &path);
	st1Ok = pi_read(pi_sid, PI_CHGR_STATUS1, &status1);
	st2Ok = pi_read(pi_sid, PI_CHGR_STATUS2, &status2);
	st7Ok = pi_read(pi_sid, PI_CHGR_STATUS7, &status7);
	if (!pathOk && !st1Ok) {
		return; /* keep the last known good values */
	}

	external = pathOk && ((path & (PI_PP_USBIN | PI_PP_DCIN)) != 0);
	capable = pathOk && (path & PI_PP_VALID) && !(path & PI_PP_SUSPEND);
	state = st1Ok ? (unsigned)(status1 & 7u) : 0u;
	error = st2Ok && (status2 & 0x02u);
	tooHot = st7Ok && (status7 & 0x0cu);
	charging = capable && state > PI_CHG_INHIBIT && state <= PI_CHG_TAPER &&
	    !error && !tooHot;
	full = capable && state == PI_CHG_TERMINATE && !error;

	/* BCL v5: the real battery voltage and current. */
	vbatOk = pi_read(pi_sid, PI_BCL_VBAT_READ, &vbat) && vbat != 0x7fu &&
	    vbat != 0u;
	mv = vbatOk ? (unsigned)vbat * 49827u / 1000u : 0u;
	vbatOk = vbatOk && mv >= 2800u && mv <= 4600u;
	ibatOk = pi_read(pi_sid, PI_BCL_IBAT_READ, &ibat);
	pmicMa = 0;
	if (ibatOk) {
		int raw = (ibat & 0x80u) ? (int)ibat - 256 : (int)ibat;

		pmicMa = (int)((int64_t)raw * 78127 / 1000);
	}
	if (vbatOk) {
		sample.mv = mv;
	} /* else: keep the last known good voltage */
	if (ibatOk) {
		sample.ma = -pmicMa;	/* the PMIC counts into the pack */
	} /* else: keep the last known good current */

	sample.flags = SWEET_PI_F_VALID;
	if (external) {
		sample.flags |= SWEET_PI_F_EXTERNAL;
	}
	if (capable) {
		sample.flags |= SWEET_PI_F_CAPABLE;
	}
	if (charging) {
		sample.flags |= SWEET_PI_F_CHARGING;
	}
	if (full) {
		sample.flags |= SWEET_PI_F_FULL;
	}
	if (vbatOk) {
		sample.flags |= SWEET_PI_F_VOLTAGE;
	}
	if (ibatOk) {
		sample.flags |= SWEET_PI_F_CURRENT;
	}
	sample.percent = full ? 100u :
	    (vbatOk ? pi_estimate_percent(sample.mv) : sample.percent);
	sample.capacity = pi_capacity;

	IOLockLock(pi_lock);
	if (sample.mv != pi_battery.mv || sample.ma != pi_battery.ma ||
	    sample.flags != pi_battery.flags || sample.percent != pi_battery.percent) {
		printf("SWEET POWER: sid=%u %u%% %umV %dmA external=%d capable=%d "
		    "charging=%d full=%d state=%u path=%#x status1=%#x status2=%#x "
		    "status7=%#x\n",
		    pi_sid, sample.percent, sample.mv, sample.ma, external, capable,
		    charging, full, state, path, status1, status2, status7);
	}
	pi_battery = sample;
	IOLockUnlock(pi_lock);
}

/* ------------------------------------------------------------------ */
/* The thermal sample                                                  */
/* ------------------------------------------------------------------ */

/*
 * The temperature alarm blocks of the PMICs.  The block does NOT hold a
 * temperature, it holds the trip thresholds and the alarm state; the
 * temperature itself comes from the ADC7 (below).  The peripheral 0x0a is
 * not in the allow list of sweet_spmi.c yet, so these reads fail until
 * that one line is added - see logs/research/impl/21-power-thermal.md.
 */
static void
pi_sample_alarms(void)
{
	unsigned sid;

	for (sid = 1; sid <= SWEET_PI_PMIC_SIDS; ++sid) {
		pi_thermal_t sample = pi_thermal[sid];
		uint8_t status = 0, th1 = 0, th2 = 0, th3 = 0;
		bool ok;

		ok = pi_read(sid, PI_TALARM_STATUS, &status) &&
		    pi_read(sid, PI_TALARM_TH1, &th1) &&
		    pi_read(sid, PI_TALARM_TH2, &th2) &&
		    pi_read(sid, PI_TALARM_TH3, &th3);
		if (!ok) {
			if (!pi_alarm_warned) {
				pi_alarm_warned = true;
				printf("SWEET THERMAL: the temperature alarm blocks "
				    "(SPMI peripheral 0x0a) do not answer; add 0x0a to "
				    "sp_peripheral_ok() in bsd/dev/sweet_spmi.c\n");
			}
			sample.alarm = 0xffu;
			sample.thr[0] = sample.thr[1] = sample.thr[2] = 0u;
		} else {
			sample.alarm = status;
			sample.thr[0] = th1;
			sample.thr[1] = th2;
			sample.thr[2] = th3;
		}
		IOLockLock(pi_lock);
		pi_thermal[sid] = sample;
		IOLockUnlock(pi_lock);
	}
}

/*
 * The ADC7 of the PMK8350: the last conversion result and the channel it
 * was made for.  The conversion is started by the PMIC itself (this probe
 * never writes), so the value is only a temperature when the channel
 * belongs to one of the die temperature channels.
 */
static void
pi_sample_adc7(void)
{
	uint8_t rev = 0, status1 = 0, status2 = 0, ch = 0, d0 = 0, d1 = 0;
	unsigned code, uv, channel;
	unsigned i;

	if (!pi_read(0, PI_ADC7_REV, &rev) ||
	    !pi_read(0, PI_ADC7_STATUS1, &status1) ||
	    !pi_read(0, PI_ADC7_STATUS2, &status2) ||
	    !pi_read(0, PI_ADC7_CH_SEL, &ch) ||
	    !pi_read(0, PI_ADC7_DATA0, &d0) ||
	    !pi_read(0, PI_ADC7_DATA1, &d1)) {
		return; /* the ADC7 does not answer */
	}
	code = (unsigned)d0 | ((unsigned)d1 << 8);
	uv = code * PI_ADC7_VREF_UV / PI_ADC7_FULLSCALE;
	channel = ch;

	for (i = 0; i < sizeof(pi_die_temp) / sizeof(pi_die_temp[0]); ++i) {
		unsigned sid = pi_die_temp[i].sid;
		pi_thermal_t sample;

		if (pi_die_temp[i].channel != channel) {
			continue;
		}
		IOLockLock(pi_lock);
		sample = pi_thermal[sid];
		IOLockUnlock(pi_lock);
		sample.code = code;
		sample.uv = uv;
		sample.status = ((unsigned)status1 << 8) | status2;
		sample.valid = 1u;
		if (status1 & 0x01u) {	/* EOC: a conversion has finished */
			int temp = PI_DIE_TEMP_UV_25DEG +
			    ((int)uv - PI_DIE_TEMP_UV_25DEG) * 1000 /
			    PI_DIE_TEMP_UV_PER_DEG;

			if (temp < -40000) {
				temp = -40000;
			}
			if (temp > 150000) {
				temp = 150000;
			}
			sample.temp_mC = (unsigned)temp;
		}
		IOLockLock(pi_lock);
		pi_thermal[sid] = sample;
		IOLockUnlock(pi_lock);
	}
}

/* The thermal level of the board: the hottest PMIC and the alarm state. */
static void
pi_update_level(void)
{
	unsigned level = SWEET_PI_LEVEL_NORMAL;
	unsigned hottest = 0;
	unsigned sid;

	for (sid = 1; sid <= SWEET_PI_PMIC_SIDS; ++sid) {
		pi_thermal_t sample;

		IOLockLock(pi_lock);
		sample = pi_thermal[sid];
		IOLockUnlock(pi_lock);
		if (sample.temp_mC > hottest) {
			hottest = sample.temp_mC;
		}
		/* An active temperature alarm is a hardware over temperature
		 * event: it is always critical, whatever the ADC says. */
		if (sample.alarm != 0xffu && (sample.alarm & 0x07u) != 0u) {
			level = SWEET_PI_LEVEL_CRITICAL;
		}
	}
	if (level != SWEET_PI_LEVEL_CRITICAL) {
		if (hottest >= PI_TRIP_CRITICAL_MC) {
			level = SWEET_PI_LEVEL_CRITICAL;
		} else if (hottest >= PI_TRIP_HEAVY_MC) {
			level = SWEET_PI_LEVEL_HEAVY;
		} else if (hottest >= PI_TRIP_MODERATE_MC) {
			level = SWEET_PI_LEVEL_MODERATE;
		} else if (hottest >= PI_TRIP_LIGHT_MC) {
			level = SWEET_PI_LEVEL_LIGHT;
		}
	}
	IOLockLock(pi_lock);
	pi_level = level;
	IOLockUnlock(pi_lock);
}

/* The console line of one PMIC. */
static void
pi_print_thermal(unsigned sid)
{
	pi_thermal_t sample;

	IOLockLock(pi_lock);
	sample = pi_thermal[sid];
	IOLockUnlock(pi_lock);
	printf("SWEET THERMAL: sid=%u temp=%d code=0x%04x uv=%u alarm=0x%02x "
	    "thr=%u,%u,%u adc=0x%04x valid=%u\n",
	    sid, (int)sample.temp_mC, sample.code, sample.uv, sample.alarm,
	    sample.thr[0], sample.thr[1], sample.thr[2], sample.status,
	    sample.valid);
}

/* ------------------------------------------------------------------ */
/* The probe thread                                                    */
/* ------------------------------------------------------------------ */

static void pi_probe_thread(__unused void *arg,
    __unused wait_result_t wr) __attribute__((noreturn));

static void
pi_probe_thread(__unused void *arg, __unused wait_result_t wr)
{
	for (;;) {
		(void)tsleep(&pi_chan, PZERO, "sweetpowerinfo",
		    hz * (int)PI_PERIOD_SECS);
		if (pi_thermal_on) {
			pi_sample_alarms();
			pi_sample_adc7();
			pi_update_level();
			for (unsigned sid = 1; sid <= SWEET_PI_PMIC_SIDS; ++sid) {
				pi_print_thermal(sid);
			}
		}
		if (pi_battery_on) {
			pi_sample_battery();
		}
	}
}

/* ------------------------------------------------------------------ */
/* The accessors (used by the IOKit side)                              */
/* ------------------------------------------------------------------ */

int
sweet_powerinfo_battery(unsigned *mv, int *ma, unsigned *flags, unsigned *percent)
{
	pi_battery_t sample;
	bool valid;

	IOLockLock(pi_lock);
	sample = pi_battery;
	IOLockUnlock(pi_lock);
	valid = (sample.flags & SWEET_PI_F_VALID) != 0u;
	if (mv) {
		*mv = sample.mv;
	}
	if (ma) {
		*ma = sample.ma;
	}
	if (flags) {
		*flags = sample.flags;
	}
	if (percent) {
		*percent = sample.percent;
	}
	return valid ? 0 : -1;
}

int
sweet_powerinfo_thermal(unsigned sid, unsigned *temp_mC, unsigned *code,
    unsigned *uv, unsigned *alarm)
{
	pi_thermal_t sample;
	bool valid;

	if (sid < 1u || sid > SWEET_PI_PMIC_SIDS) {
		return -1;
	}
	IOLockLock(pi_lock);
	sample = pi_thermal[sid];
	IOLockUnlock(pi_lock);
	valid = sample.valid != 0u;
	if (temp_mC) {
		*temp_mC = sample.temp_mC;
	}
	if (code) {
		*code = sample.code;
	}
	if (uv) {
		*uv = sample.uv;
	}
	if (alarm) {
		*alarm = sample.alarm;
	}
	return valid ? 0 : -1;
}

unsigned
sweet_powerinfo_level(void)
{
	unsigned level;

	IOLockLock(pi_lock);
	level = pi_level;
	IOLockUnlock(pi_lock);
	return level;
}

unsigned
sweet_powerinfo_capacity(void)
{
	return pi_capacity;
}

/* ------------------------------------------------------------------ */
/* Start                                                               */
/* ------------------------------------------------------------------ */

int
sweet_powerinfo_init(void)
{
	uint32_t thermal = 0, battery = 0, mah = 0, venus = 0, publish = 1;
	thread_t thread;

	if (pi_lock == NULL) {
		pi_lock = IOLockAlloc();
		if (pi_lock == NULL) {
			printf("SWEET POWERINFO: no lock\n");
			return -1;
		}
	}
	(void)PE_parse_boot_argn(SWEET_PI_ARG_THERMAL, &thermal, sizeof(thermal));
	(void)PE_parse_boot_argn(SWEET_PI_ARG_BATTERY, &battery, sizeof(battery));
	(void)PE_parse_boot_argn(SWEET_PI_ARG_MAH, &mah, sizeof(mah));
	(void)PE_parse_boot_argn("venus", &venus, sizeof(venus));
	/* The service is a second power source; sweetpowerinfo=0 removes it
	 * again without touching anything else. */
	(void)PE_parse_boot_argn("sweetpowerinfo", &publish, sizeof(publish));
	pi_thermal_on = thermal == 1u;
	pi_battery_on = battery == 1u;

	/* The pack size: 4600 mAh for the Mi 11 (venus), 5020 mAh for the
	 * sweet board; sweetbattmah= overrides both. */
	pi_capacity = venus ? PI_MAH_VENUS : PI_MAH_SWEET;
	if (mah > 0u && mah <= 20000u) {
		pi_capacity = (unsigned)mah;
	}
	/* The static values of 0062/0104, so that the service never shows
	 * an empty battery before the first sample. */
	pi_battery.mv = 4200u;
	pi_battery.ma = 0;
	pi_battery.flags = 0u;
	pi_battery.percent = 100u;
	pi_battery.capacity = pi_capacity;
	for (unsigned sid = 1; sid <= SWEET_PI_PMIC_SIDS; ++sid) {
		pi_thermal[sid].alarm = 0xffu;
	}

	if (pi_thermal_on || pi_battery_on) {
		if (sweet_spmi_init() != 0) {
			printf("SWEET POWERINFO: the SPMI reader is not available\n");
		} else {
			pi_sid = sweet_spmi_sid();
		}
	}
	printf("SWEET POWERINFO: thermal=%d battery=%d sid=%u pack=%u mAh "
	    "(read only)\n", pi_thermal_on, pi_battery_on, pi_sid, pi_capacity);

	/* The IOKit service: it publishes the same numbers as SweetBattery
	 * and the thermal data of this probe. */
	if (publish) {
		sweet_powerinfo_publish();
	} else {
		printf("SWEET POWERINFO: the service is switched off "
		    "(sweetpowerinfo=0)\n");
	}

	if ((pi_thermal_on || pi_battery_on) &&
	    kernel_thread_start(pi_probe_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		printf("SWEET POWERINFO: probe every %u s\n", PI_PERIOD_SECS);
	}
	return 0;
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
