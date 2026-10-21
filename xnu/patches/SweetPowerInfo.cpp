/*
 * SWEET power information service - the IOKit side of the power and
 * thermal probe of bsd/dev/sweet_powerinfo.c.
 *
 * The service is an IOPMPowerSource, so the iOS power stack finds it the
 * same way it finds SweetBattery (patch 0062/0104): by the class
 * IOPMPowerSource.  It publishes the standard properties the iOS power UI
 * reads:
 *
 *   CurrentCapacity, MaxCapacity, Voltage, Amperage, IsCharging,
 *   FullyCharged, IsPresent, Temperature, ExternalConnected,
 *   ExternalChargeCapable, BatteryInstalled, AtWarnLevel,
 *   AtCriticalLevel, TimeRemaining, CycleCount, LegacyIOBatteryInfo
 *
 * and the AppleRaw* names that thermalmonitord looks for
 * (AppleRawCurrentCapacity, AppleRawMaxCapacity, ...).
 *
 * The class is called AppleSmartBattery (the name the iOS power stack
 * uses for the battery service) and the user client is called
 * AppleSmartBatteryUserClient.  The user client answers the
 * AppleKeyStore style selectors that the iOS power stack really calls
 * (the numbers and the call counts are taken from the live log,
 * logs/venus-live-20261021l.txt):
 *
 *   sel   calls  who calls it
 *   0     2211   powerd, SpringBoard, homed, chronod, bluetoothuserd, ...
 *   7     2287   powerd, SpringBoard, securityd, CloudKeychainProxy, ...
 *   8       94   SpringBoard (the only selector that fails today)
 *   14      62   keybagd
 *   17   12506   SpringBoard, powerd, contextstored, coreduetd, ...
 *   26     191   SpringBoard
 *   27     230   SpringBoard
 *   32      40   SpringBoard
 *   35    1467   SpringBoard, searchd, chronod, nanoregistryd, ...
 *   102     20   UserEventAgent
 *
 * The battery related ones are answered with the real numbers of the
 * probe, the rest is answered with kIOReturnUnsupported and a log line,
 * so that the caller can see what is missing instead of a silent error.
 *
 * The values are the ones of the probe, which reads the same PMIC
 * registers as SweetBattery, so both services publish the same numbers.
 *
 * Sources: logs/research/06-power-battery-charging.md,
 *          logs/research/26-thermal-plan.md
 */
#if defined(ARM64_BOARD_CONFIG_SWEET)
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <libkern/libkern.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>

/*
 * bsd/dev/sweet_powerinfo.c.  The header of that file cannot be included
 * here (the bsd headers are not exported to the iokit component), so the
 * prototypes are repeated - keep them in sync with
 * bsd/dev/sweet_powerinfo.h.
 */
extern "C" int sweet_powerinfo_battery(unsigned *mv, int *ma, unsigned *flags,
    unsigned *percent);
extern "C" int sweet_powerinfo_thermal(unsigned sid, unsigned *temp_mC,
    unsigned *code, unsigned *uv, unsigned *alarm);
extern "C" unsigned sweet_powerinfo_level(void);
extern "C" unsigned sweet_powerinfo_capacity(void);

/* The flags of the battery sample (bsd/dev/sweet_powerinfo.h). */
#define PI_F_VALID    0x0001u
#define PI_F_EXTERNAL 0x0002u
#define PI_F_CAPABLE  0x0004u
#define PI_F_CHARGING 0x0008u
#define PI_F_FULL     0x0010u
#define PI_F_VOLTAGE  0x0020u
#define PI_F_CURRENT  0x0040u

/* The thermal levels (bsd/dev/sweet_powerinfo.h). */
#define PI_LEVEL_NORMAL   0u
#define PI_LEVEL_LIGHT    1u
#define PI_LEVEL_MODERATE 2u
#define PI_LEVEL_HEAVY    3u
#define PI_LEVEL_CRITICAL 4u

/* The PMICs with a temperature alarm block: SPMI SID 1..5. */
#define PI_PMIC_SIDS 5u

/* How often the properties are refreshed (the probe runs every 5 s). */
#define PI_UPDATE_MS 5000u

/* The AppleKeyStore style selectors of the iOS power stack. */
enum {
	kPIOpen = 0,
	kPIGetLockState = 7,
	kPIGetSystem = 14,
	kPIGetDeviceState = 17,
	kPIAssertHold = 26,
	kPIAssertDrop = 27,
	kPIGetExtDeviceState = 35,
	kPINotifyStateSet = 102,
};

/*
 * The power state bitmask that selector 7 returns:
 * bit0 external power, bit1 charging, bit2 fully charged, bit3 the battery
 * is present, bit4 a thermal alarm is active.
 */
#define PI_STATE_EXTERNAL 0x1u
#define PI_STATE_CHARGING 0x2u
#define PI_STATE_FULL     0x4u
#define PI_STATE_PRESENT  0x8u
#define PI_STATE_ALARM    0x10u

class AppleSmartBattery : public IOPMPowerSource {
	OSDeclareDefaultStructors(AppleSmartBattery);
public:
	bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	void free(void) APPLE_KEXT_OVERRIDE;
	unsigned powerState(void);
private:
	IOWorkLoop *fLoop;
	IOTimerEventSource *fTimer;
	bool fTimerAdded, fRunning;
	unsigned fCapacity;
	unsigned fAssertions;
	void putNumber(const char *key, UInt32 value);
	void putBool(const char *key, bool value);
	void publish(void);
	void publishThermal(void);
	static void timerFired(OSObject *owner, IOTimerEventSource *sender);
	static IOReturn stopPolling(OSObject *owner, void *, void *, void *, void *);
};
OSDefineMetaClassAndStructors(AppleSmartBattery, IOPMPowerSource);

bool
AppleSmartBattery::init(OSDictionary *dictionary)
{
	/* This XNU's IOPMPowerSource only overrides init(void): run it
	 * explicitly to allocate the power source dictionary. */
	if (!IOPMPowerSource::init()) {
		return false;
	}
	fLoop = NULL;
	fTimer = NULL;
	fTimerAdded = fRunning = false;
	fCapacity = sweet_powerinfo_capacity();
	fAssertions = 0;
	if (dictionary) {
		OSCollectionIterator *it = OSCollectionIterator::withCollection(dictionary);
		if (!it) {
			return false;
		}
		OSObject *object;
		bool ok = true;
		while ((object = it->getNextObject())) {
			const OSSymbol *key = OSDynamicCast(OSSymbol, object);
			if (!key || !setProperty(key, dictionary->getObject(key))) {
				ok = false;
				break;
			}
		}
		it->release();
		if (!ok) {
			return false;
		}
	}
	return true;
}

void
AppleSmartBattery::putNumber(const char *key, UInt32 value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	OSNumber *number = OSNumber::withNumber(value, 32);
	if (symbol && number) {
		setPSProperty(symbol, number);
	}
	OSSafeReleaseNULL(number);
	OSSafeReleaseNULL(symbol);
}

void
AppleSmartBattery::putBool(const char *key, bool value)
{
	const OSSymbol *symbol = OSSymbol::withCString(key);
	if (symbol) {
		setPSProperty(symbol, value ? kOSBooleanTrue : kOSBooleanFalse);
		symbol->release();
	}
}

unsigned
AppleSmartBattery::powerState(void)
{
	unsigned mv = 0, flags = 0, percent = 0;
	int ma = 0;
	unsigned state = PI_STATE_PRESENT;

	(void)sweet_powerinfo_battery(&mv, &ma, &flags, &percent);
	if (flags & PI_F_EXTERNAL) {
		state |= PI_STATE_EXTERNAL;
	}
	if (flags & PI_F_CHARGING) {
		state |= PI_STATE_CHARGING;
	}
	if (flags & PI_F_FULL) {
		state |= PI_STATE_FULL;
	}
	for (unsigned sid = 1; sid <= PI_PMIC_SIDS; ++sid) {
		unsigned temp = 0, code = 0, uv = 0, alarm = 0;

		(void)sweet_powerinfo_thermal(sid, &temp, &code, &uv, &alarm);
		if (alarm != 0xffu && (alarm & 0x07u) != 0u) {
			state |= PI_STATE_ALARM;
		}
	}
	return state;
}

/*
 * The thermal part: the die temperatures of the PMICs and the thermal
 * level of the board.  The XNU thermal contract (iokit/IOKit/pwr_mgt/IOPM.h)
 * is published as well: "Power Status" with "Thermal_Level_Warning" and
 * the CPU/graphics power limits - the keys an iOS power stack (and our
 * own tooling) can read.  iOS does not read them itself (it uses the
 * libSystem thermal notification), but they are the contract the kernel
 * side of the thermal policy uses.
 */
void
AppleSmartBattery::publishThermal(void)
{
	OSDictionary *samples;
	unsigned level = sweet_powerinfo_level();
	unsigned hottest = 0;
	unsigned warning;

	samples = OSDictionary::withCapacity(PI_PMIC_SIDS);
	for (unsigned sid = 1; sid <= PI_PMIC_SIDS; ++sid) {
		unsigned temp = 0, code = 0, uv = 0, alarm = 0;
		bool valid = sweet_powerinfo_thermal(sid, &temp, &code, &uv, &alarm) == 0;
		OSDictionary *entry = OSDictionary::withCapacity(5);
		char name[8];

		if (!entry) {
			continue;
		}
		entry->setObject("Temperature", OSNumber::withNumber(temp, 32));
		entry->setObject("Code", OSNumber::withNumber(code, 32));
		entry->setObject("Microvolts", OSNumber::withNumber(uv, 32));
		entry->setObject("Alarm", OSNumber::withNumber(alarm, 32));
		entry->setObject("Valid", valid ? kOSBooleanTrue : kOSBooleanFalse);
		snprintf(name, sizeof(name), "SID%u", sid);
		if (samples) {
			samples->setObject(OSString::withCString(name), entry);
		}
		entry->release();
		if (temp > hottest) {
			hottest = temp;
		}
	}
	if (samples) {
		setProperty("SweetThermalSamples", samples);
		samples->release();
	}
	putNumber("SweetThermalLevel", level);
	putNumber("SweetThermalMaxTemp", hottest);
	putBool("SweetThermalSampleValid", hottest != 0u);

	/* The XNU thermal contract: 0 = normal, 100 = warning, 110 = trap. */
	switch (level) {
	case PI_LEVEL_NORMAL:
		warning = 0;
		break;
	case PI_LEVEL_LIGHT:
		warning = 0;
		break;
	case PI_LEVEL_MODERATE:
		warning = 100;
		break;
	case PI_LEVEL_HEAVY:
		warning = 110;
		break;
	default:
		warning = 110;
		break;
	}
	{
		OSDictionary *status = OSDictionary::withCapacity(3);
		OSDictionary *cpu = OSDictionary::withCapacity(3);
		OSDictionary *gpu = OSDictionary::withCapacity(1);

		if (status && cpu && gpu) {
			/* There is no cpufreq on this board: the scheduler and
			 * the core count are the only limits we can offer. */
			cpu->setObject("CPU_Speed_Limit", OSNumber::withNumber(100, 32));
			cpu->setObject("CPU_Available_CPUs", OSNumber::withNumber(8, 32));
			cpu->setObject("CPU_Scheduler_Limit",
			    OSNumber::withNumber(warning ? 50 : 100, 32));
			gpu->setObject("Graphics_Power_Performance",
			    OSNumber::withNumber(warning ? 50 : 100, 32));
			status->setObject("Thermal_Level_Warning",
			    OSNumber::withNumber(warning, 32));
			status->setObject("CPU_Power_Limits", cpu);
			status->setObject("Graphics_Power_Limits", gpu);
			setProperty("Power Status", status);
		}
		OSSafeReleaseNULL(status);
		OSSafeReleaseNULL(cpu);
		OSSafeReleaseNULL(gpu);
	}
}

void
AppleSmartBattery::publish(void)
{
	unsigned mv = 0, flags = 0, percent = 0;
	int ma = 0;
	bool valid = sweet_powerinfo_battery(&mv, &ma, &flags, &percent) == 0;
	bool external = (flags & PI_F_EXTERNAL) != 0;
	bool capable = (flags & PI_F_CAPABLE) != 0;
	bool charging = (flags & PI_F_CHARGING) != 0;
	bool full = (flags & PI_F_FULL) != 0;
	unsigned capacity;
	unsigned temp = 0, code = 0, uv = 0, alarm = 0xffu;

	fCapacity = sweet_powerinfo_capacity();
	capacity = (fCapacity * percent + 50u) / 100u;
	if (!valid) {
		/* The static fallback of 0062, so that the phone always shows
		 * a battery - the same values SweetBattery publishes. */
		mv = 4200u;
		ma = 0;
		percent = 100u;
		capacity = fCapacity;
		external = true;
		capable = true;
		charging = false;
		full = true;
	}
	setBatteryInstalled(true);
	setCurrentCapacity(capacity);
	setMaxCapacity(fCapacity);
	setExternalConnected(external);
	setExternalChargeCapable(capable);
	setIsCharging(charging);
	setAtWarnLevel(false);
	/* An uncalibrated voltage curve must not assert a critical shutdown. */
	setAtCriticalLevel(false);
	setVoltage(mv);
	setAmperage(ma);
	setTimeRemaining(-1);	/* Unknown: never publish zero minutes. */
	setCycleCount(0);	/* Placeholder: there is no gauge. */
	putBool("FullyCharged", full);
	putBool("IsPresent", true);
	putBool("AppleRawExternalConnected", external);
	putBool("AppleRawIsCharging", charging);
	putBool("AppleRawFullyCharged", full);
	putNumber("AppleRawCurrentCapacity", capacity);
	putNumber("AppleRawMaxCapacity", fCapacity);
	putNumber("DesignCapacity", fCapacity);
	putNumber("InstantAmperage", (UInt32)ma);

	/*
	 * The pack temperature is NOT reachable from the application
	 * processor on this board (the gauge and its thermistor live behind
	 * the ADSP charger protection domain).  What the PMIC measures is its
	 * own die temperature; it is published next to the standard property
	 * and the source is named, so that nobody mistakes it for a pack
	 * temperature.
	 */
	for (unsigned sid = 1; sid <= PI_PMIC_SIDS; ++sid) {
		unsigned t = 0, c = 0, u = 0, a = 0;

		if (sweet_powerinfo_thermal(sid, &t, &c, &u, &a) == 0 && t != 0u) {
			temp = t;
			code = c;
			uv = u;
			break;
		}
	}
	if (temp != 0u) {		/* iOS battery convention: centi degrees Celsius. */
		putNumber("Temperature", temp / 10u);
		putBool("SweetPowerInfoTemperatureEstimated", true);
		putBool("SweetPowerInfoTemperatureSource", true);
	} else {
		putNumber("Temperature", 2500);
		putBool("SweetPowerInfoTemperatureEstimated", true);
		putBool("SweetPowerInfoTemperatureSource", false);
	}
	putNumber("SweetThermalPMICDiesTemp", temp);
	putNumber("SweetThermalADCCode", code);
	putNumber("SweetThermalADCMicrovolts", uv);
	putNumber("SweetThermalAlarm", alarm);

	putBool("SweetPowerInfoSampleValid", valid);
	putBool("SweetPowerInfoVoltageValid", (flags & PI_F_VOLTAGE) != 0);
	putBool("SweetPowerInfoCurrentValid", (flags & PI_F_CURRENT) != 0);
	putBool("SweetPowerInfoSOCEstimated", true);
	putBool("SweetPowerInfoFallback", !valid);
	putNumber("SweetPowerInfoPercent", percent);
	putNumber("SweetPowerInfoVoltage", mv);
	putNumber("SweetPowerInfoAmperage", (UInt32)ma);
	putNumber("SweetPowerInfoPackCapacity", fCapacity);
	putBool("SweetPowerInfoSecondarySource", true);

	publishThermal();

	/* IOPMCopyBatteryInfo's per source compatibility dictionary. */
	{
		OSDictionary *legacy = OSDictionary::withCapacity(7);
		const char *keys[] = {kIOBatteryCurrentChargeKey, kIOBatteryCapacityKey,
		    kIOBatteryVoltageKey, kIOBatteryAmperageKey, kIOBatteryCycleCountKey,
		    kIOBatteryFlagsKey};
		UInt32 values[] = {capacity, fCapacity, mv, (UInt32)ma, 0,
		    (UInt32)(kIOBatteryInstalled | (external ? kIOBatteryChargerConnect : 0) |
		    (charging ? kIOBatteryCharge : 0))};
		if (legacy) {
			for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
				OSNumber *number = OSNumber::withNumber(values[i], 32);
				if (number) {
					legacy->setObject(keys[i], number);
					number->release();
				}
			}
			setLegacyIOBatteryInfo(legacy);
		}
		legacy->release();
	}
	updateStatus();	/* Flush the properties and notify on a change. */
}

void
AppleSmartBattery::timerFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleSmartBattery *self = OSDynamicCast(AppleSmartBattery, owner);
	if (self && self->fRunning) {
		self->publish();
		sender->setTimeoutMS(PI_UPDATE_MS);
	}
}

bool
AppleSmartBattery::start(IOService *provider)
{
	if (!IOPMPowerSource::start(provider)) {
		return false;
	}
	setName("AppleSmartBattery");
	setProperty(kIOUserClientClassKey, "AppleSmartBatteryUserClient");
	/* Publish a complete source BEFORE registerService wakes powerd. */
	publish();
	fLoop = IOWorkLoop::workLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &AppleSmartBattery::timerFired);
	if (fLoop && fTimer && fLoop->addEventSource(fTimer) == kIOReturnSuccess) {
		fTimerAdded = true;
		fRunning = true;
		fTimer->setTimeoutMS(PI_UPDATE_MS);
	} else {
		IOLog("SWEET POWERINFO: no work loop; the values stay static\n");
	}
	registerService();
	IOLog("SWEET POWERINFO: AppleSmartBattery published (pack %u mAh)\n",
	    fCapacity);
	return true;
}

void
AppleSmartBattery::stop(IOService *provider)
{
	if (fLoop && fTimerAdded) {
		(void)fLoop->runAction(&AppleSmartBattery::stopPolling, this);
		fLoop->removeEventSource(fTimer);
		fTimerAdded = false;
	}
	IOPMPowerSource::stop(provider);
}

IOReturn
AppleSmartBattery::stopPolling(OSObject *owner, void *, void *, void *, void *)
{
	AppleSmartBattery *self = OSDynamicCast(AppleSmartBattery, owner);
	if (!self) {
		return kIOReturnBadArgument;
	}
	self->fRunning = false;
	self->fTimer->cancelTimeout();
	self->fTimer->disable();
	return kIOReturnSuccess;
}

void
AppleSmartBattery::free(void)
{
	OSSafeReleaseNULL(fTimer);
	OSSafeReleaseNULL(fLoop);
	IOPMPowerSource::free();
}

/* ------------------------------------------------------------------ */
/* The user client                                                     */
/* ------------------------------------------------------------------ */

class AppleSmartBatteryUserClient : public IOUserClient {
	OSDeclareDefaultStructors(AppleSmartBatteryUserClient);
public:
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AppleSmartBatteryUserClient, IOUserClient);

IOReturn
AppleSmartBatteryUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* Copy len bytes into the caller's output structure. */
static IOReturn
sweet_pi_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		if (len > args->structureOutputDescriptor->getLength()) {
			return kIOReturnNoSpace;
		}
		args->structureOutputDescriptor->writeBytes(0, buf, len);
		args->structureOutputDescriptorSize = len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL || len > args->structureOutputSize) {
		return kIOReturnNoSpace;
	}
	memcpy(args->structureOutput, buf, len);
	args->structureOutputSize = len;
	return kIOReturnSuccess;
}

/*
 * The state of the battery and of the thermals as a property list.  A
 * property list is used because it is the only format every caller of an
 * Apple style battery service understands.
 */
static uint32_t
sweet_pi_plist(char *buf, uint32_t size, bool withThermal)
{
	unsigned mv = 0, flags = 0, percent = 0;
	int ma = 0;
	unsigned level = 0, temp = 0;
	char thermal[160];
	int n;

	(void)sweet_powerinfo_battery(&mv, &ma, &flags, &percent);
	thermal[0] = '\0';
	if (withThermal) {
		level = sweet_powerinfo_level();
		for (unsigned sid = 1; sid <= PI_PMIC_SIDS; ++sid) {
			unsigned t = 0, c = 0, u = 0, a = 0;

			if (sweet_powerinfo_thermal(sid, &t, &c, &u, &a) == 0 && t != 0u) {
				temp = t;
				break;
			}
		}
		scnprintf(thermal, sizeof(thermal),
		    "<key>ThermalLevel</key><integer>%u</integer>"
		    "<key>ThermalTemperature</key><integer>%u</integer>",
		    level, temp);
	}
	n = scnprintf(buf, size,
	    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\""
	    " \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<plist version=\"1.0\"><dict>"
	    "<key>Voltage</key><integer>%u</integer>"
	    "<key>Amperage</key><integer>%d</integer>"
	    "<key>CurrentCapacity</key><integer>%u</integer>"
	    "<key>MaxCapacity</key><integer>%u</integer>"
	    "<key>Percent</key><integer>%u</integer>"
	    "<key>IsCharging</key><%s/>"
	    "<key>FullyCharged</key><%s/>"
	    "<key>IsPresent</key><true/>"
	    "<key>ExternalConnected</key><%s/>"
	    "<key>ExternalChargeCapable</key><%s/>"
	    "<key>Temperature</key><integer>%u</integer>"
	    "%s"
	    "</dict></plist>\n",
	    mv, ma, percent, sweet_powerinfo_capacity(), percent,
	    (flags & PI_F_CHARGING) ? "true" : "false",
	    (flags & PI_F_FULL) ? "true" : "false",
	    (flags & PI_F_EXTERNAL) ? "true" : "false",
	    (flags & PI_F_CAPABLE) ? "true" : "false",
	    temp ? temp / 10u : 2500u,
	    thermal);
	if (n < 0 || (uint32_t)n >= size) {
		return 0;
	}
	return (uint32_t)n;
}

IOReturn
AppleSmartBatteryUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args, __unused IOExternalMethodDispatch *dispatch,
    __unused OSObject *target, __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint32_t outScalars = args->scalarOutputCount;
	AppleSmartBattery *battery = OSDynamicCast(AppleSmartBattery, getProvider());
	char plist[1024];

	switch (selector) {
	case kPIOpen:
	case kPIGetDeviceState:
	case kPIGetExtDeviceState:
		/* The state of the battery (and of the thermals for the
		 * extended state) as a property list. */
		args->scalarOutputCount = 0;
		ret = sweet_pi_struct_out(args, plist,
		    sweet_pi_plist(plist, sizeof(plist),
		    selector == kPIGetExtDeviceState));
		break;
	case kPIGetLockState:
		/* The power state of the phone. */
		if (outScalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = battery ? battery->powerState() : 0u;
		args->scalarOutputCount = 1;
		break;
	case kPIGetSystem:
		if (outScalars < 1) {
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = 0;	/* the system handle */
		args->scalarOutputCount = 1;
		break;
	case kPIAssertHold:
	case kPIAssertDrop:
		/* A power assertion: the phone stack holds or drops a
		 * resource.  Nothing to do for us, but the caller gets a
		 * clean answer instead of an error. */
		args->scalarOutputCount = 0;
		break;
	case kPINotifyStateSet:
		/* A state notification: accepted, nothing to do. */
		args->scalarOutputCount = 0;
		break;
	default:
		IOLog("SWEET AKS: selector %u not supported by AppleSmartBattery\n",
		    selector);
		ret = kIOReturnUnsupported;
		break;
	}
	IOLog("SWEET AKS: AppleSmartBattery sel %u -> 0x%x\n", selector,
	    (unsigned)ret);
	return ret;
}

/* ------------------------------------------------------------------ */
/* The entry point of the BSD side                                     */
/* ------------------------------------------------------------------ */

/* Called from bsd/dev/sweet_powerinfo.c: the BSD layer cannot create an
 * IOService itself. */
extern "C" void sweet_powerinfo_publish(void);

/*
 * The service is attached to IOResources and registered by hand, which
 * is what the personality table of iokit/KernelConfigTables.cpp does for
 * the other services of this kernel - the difference is only that this
 * one is created late (after the root device is known).
 */
extern "C" void
sweet_powerinfo_publish(void)
{
	static AppleSmartBattery *service = NULL;

	if (service != NULL) {
		return;
	}
	service = new AppleSmartBattery;
	if (service == NULL) {
		IOLog("SWEET POWERINFO: no memory for the service\n");
		return;
	}
	if (!service->init()) {
		IOLog("SWEET POWERINFO: init failed\n");
		service->release();
		return;
	}
	if (!service->attach(IOService::getResourceService())) {
		IOLog("SWEET POWERINFO: attach failed\n");
		service->release();
		return;
	}
	if (!service->start(IOService::getResourceService())) {
		IOLog("SWEET POWERINFO: start failed\n");
		service->detach(IOService::getResourceService());
		service->release();
		return;
	}
	IOLog("SWEET POWERINFO: AppleSmartBattery created and registered\n");
}
#endif /* ARM64_BOARD_CONFIG_SWEET */
