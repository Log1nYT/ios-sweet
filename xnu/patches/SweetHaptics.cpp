/*
 * SWEET: the haptic engine service.
 *
 * iOS drives the Taptic Engine through the HID event system: the userland
 * sends an output report (an element of the haptic report) to the haptic
 * service, the kernel service hands it to the driver.  Our IOHIDEventService
 * only had an input path until now, so the output report path is
 * implemented here:
 *
 *   userland  IOHIDServiceClient -> 'esuc' user client -> SetElementValue
 *   kernel    IOHIDEventService::setElementValue()
 *               -> handleOutputElement()   (this file)
 *                  -> sweet_haptic_play()   (bsd/dev/sweet_haptic.c)
 *                     -> the AW8697 on I2C 0x5a
 *
 * The service publishes the sensor page (0x20) usage "Haptic Vibrator"
 * (0x68) so that the userland can find it by usage pair, the same way
 * CoreMotion finds the accelerometer.
 *
 * The value of the output element carries the intensity in the low byte
 * and the length of the buzz in 10 ms units in the second byte; both are a
 * GUESS, the userland side of this path is not known (see
 * logs/research/impl/17-haptics-cover.md).
 */
#include <device/device_types.h> /* io_buf_ptr_t for private OSKext headers */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include <pexpert/pexpert.h>

#include "IOHIDEventService.h"
#include "IOHIDUsageTables.h"

extern "C" int sweet_haptic_play(int intensity, int duration_ms);
extern "C" void sweet_haptic_stop(void);

extern "C" uint64_t mach_absolute_time(void);

#define super IOHIDEventService

class SweetHIDHaptics : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDHaptics);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE
	{
		return kHIDPage_Sensor;
	}
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE
	{
		return kHIDUsage_Snsr_Mechanical_HapticVibrator;
	}
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 3; }
	virtual IOReturn handleOutputElement(UInt32 usagePage, UInt32 usage,
	    UInt32 value) APPLE_KEXT_OVERRIDE;
private:
	static void testFired(OSObject *owner, IOTimerEventSource *sender);
	IOTimerEventSource *fTest;
	UInt32 fTestSecs;
};

OSDefineMetaClassAndStructors(SweetHIDHaptics, IOHIDEventService);

static SweetHIDHaptics *gSweetHaptics;

bool
SweetHIDHaptics::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	/*
	 * What the userland can learn about this service.  The names are
	 * a GUESS: there is no Apple "haptics" HID service on a phone
	 * without a Taptic Engine, so no reference exists.
	 */
	setProperty("HapticSupported", kOSBooleanTrue);
	setProperty("HapticEngine", "AW8697");
	setProperty("HapticBus", "I2C 0x998000 addr 0x5a");
	setProperty("HapticElementUsagePage", (unsigned long long)kHIDPage_Sensor, 32);
	setProperty("HapticElementUsage",
	    (unsigned long long)kHIDUsage_Snsr_Mechanical_HapticVibrator, 32);
	gSweetHaptics = this;
	registerService();
	IOLog("SWEET HID: haptics registered (AW8697)\n");
	if (PE_parse_boot_argn("sweethaptictest", &fTestSecs, sizeof(fTestSecs)) &&
	    fTestSecs) {
		fTest = IOTimerEventSource::timerEventSource(this,
		    &SweetHIDHaptics::testFired);
		if (fTest && _hidWorkLoop->addEventSource(fTest) == kIOReturnSuccess) {
			fTest->setTimeoutMS(fTestSecs * 1000);
			IOLog("SWEET HID: haptic self test every %u s\n", fTestSecs);
		}
	}
	return true;
}

void
SweetHIDHaptics::free(void)
{
	/* The motor must not run on when the service is gone. */
	sweet_haptic_stop();
	super::free();
}

IOReturn
SweetHIDHaptics::handleOutputElement(UInt32 usagePage, UInt32 usage, UInt32 value)
{
	int intensity, duration_ms, err;

	if (usagePage != kHIDPage_Sensor ||
	    usage != kHIDUsage_Snsr_Mechanical_HapticVibrator) {
		/* Not ours: let the base class log it. */
		return IOHIDEventService::handleOutputElement(usagePage, usage, value);
	}
	intensity = (int)(value & 0xffu);
	duration_ms = (int)((value >> 8) & 0xffu) * 10;
	if (duration_ms == 0) {
		duration_ms = 100;
	}
	err = sweet_haptic_play(intensity, duration_ms);
	IOLog("SWEET HID: haptic output element intensity=%d duration=%d ms err=%d\n",
	    intensity, duration_ms, err);
	return err ? kIOReturnUnsupported : kIOReturnSuccess;
}

/*
 * A test path that does not need the userland: the boot arg
 * sweethaptictest=N plays one buzz every N seconds (N = 0 disables it).
 */
void
SweetHIDHaptics::testFired(OSObject *owner, IOTimerEventSource *sender)
{
	SweetHIDHaptics *self = (SweetHIDHaptics *)owner;

	(void)sweet_haptic_play(255, 100);
	sender->setTimeoutMS(self->fTestSecs * 1000);
}
