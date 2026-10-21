/*
 * SWEET power + thermal probe - the C interface shared with the IOKit side.
 *
 * The probe lives in bsd/dev/sweet_powerinfo.c and reads the PMIC over
 * SPMI with the already proven read path of bsd/dev/sweet_spmi.c (the
 * observer window, one byte per command).  It never writes a register.
 *
 * Two things are sampled:
 *
 *  1. the battery: the SMB5 charger registers and the BCL v5 block of
 *     PM8350B (SPMI SID 3 on the venus board) give the REAL battery
 *     voltage (0x4776) and current (0x4786), the charge state (0x1006),
 *     the error bits (0x1007), the pack temperature status (0x100d) and
 *     the power path (0x110b);
 *
 *  2. the temperature: the PMIC temperature alarm blocks (SPMI
 *     peripheral 0x0a of SID 1..5) and the PMK8350 VADC / ADC7 (SPMI SID
 *     0, peripheral 0x31) give the over temperature alarm state and the
 *     last conversion of the die temperature channels.
 *
 * The IOKit side (iokit/Kernel/arm/SweetPowerInfo.cpp) reads the values
 * through the accessors below; the header is NOT included there (the bsd
 * headers are not exported to the iokit component), so the prototypes are
 * repeated there - keep the two lists in sync.
 *
 * Sources: logs/research/06-power-battery-charging.md,
 *           logs/research/26-thermal-plan.md
 */
#ifndef SWEET_POWERINFO_H
#define SWEET_POWERINFO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Boot arguments (all optional, the probe is off by default). */
#define SWEET_PI_ARG_THERMAL "sweetthermal"  /* 1 = thermal probe         */
#define SWEET_PI_ARG_BATTERY "sweetbattery"  /* 1 = battery probe          */
#define SWEET_PI_ARG_SID     "sweetbatsid"   /* battery PMIC SPMI SID      */
#define SWEET_PI_ARG_MAH     "sweetbattmah"  /* pack size in mAh           */

/* Flags of the battery sample. */
#define SWEET_PI_F_VALID    0x0001u /* the charger registers answered    */
#define SWEET_PI_F_EXTERNAL 0x0002u /* a power source is present         */
#define SWEET_PI_F_CAPABLE  0x0004u /* the port can charge the pack      */
#define SWEET_PI_F_CHARGING 0x0008u /* the PMIC is charging              */
#define SWEET_PI_F_FULL     0x0010u /* the PMIC says the pack is full    */
#define SWEET_PI_F_VOLTAGE  0x0020u /* mv is a measurement, not a guess  */
#define SWEET_PI_F_CURRENT  0x0040u /* ma is a measurement, not a guess  */

/* Thermal levels (our own scale, report 26 section 5.C). */
#define SWEET_PI_LEVEL_NORMAL   0u
#define SWEET_PI_LEVEL_LIGHT    1u
#define SWEET_PI_LEVEL_MODERATE 2u
#define SWEET_PI_LEVEL_HEAVY    3u
#define SWEET_PI_LEVEL_CRITICAL 4u

/* The PMICs with a temperature alarm block: SPMI SID 1..5. */
#define SWEET_PI_PMIC_SIDS 5u

/*
 * Start the probe.  Returns 0 when the probe runs (or when it is
 * switched off by the boot arguments) and -1 when it could not start.
 */
int sweet_powerinfo_init(void);

/*
 * The last battery sample.  Returns 0 when a valid sample is available.
 * mv = battery voltage in mV, ma = battery current in mA (positive means
 * charging), flags = SWEET_PI_F_*, percent = the voltage derived state of
 * charge estimate (0..100).
 */
int sweet_powerinfo_battery(unsigned *mv, int *ma, unsigned *flags,
    unsigned *percent);

/*
 * The last thermal sample of one PMIC (sid 1..5).  Returns 0 when a
 * sample is available.  temp_mC = the derived die temperature in milli
 * degrees Celsius (0 = not available), code = the raw ADC code, uv = the
 * ADC voltage in microvolts, alarm = the temperature alarm status byte
 * (0xff = the alarm block did not answer).
 */
int sweet_powerinfo_thermal(unsigned sid, unsigned *temp_mC, unsigned *code,
    unsigned *uv, unsigned *alarm);

/* The thermal level of the board (SWEET_PI_LEVEL_*). */
unsigned sweet_powerinfo_level(void);

/* The pack size in mAh that the probe uses (sweetbattmah, 4600 on venus). */
unsigned sweet_powerinfo_capacity(void);

#ifdef __cplusplus
}
#endif
#endif /* SWEET_POWERINFO_H */
