/*
 * SWEET: the cover (hall switch) and headset sensor service.
 *
 * Two pieces of hardware are published here:
 *
 *   - the hall switch on TLMM 80 (the stock Xiaomi device tree has it as
 *     gpio-keys/hall_key, <&tlmm 80 GPIO_ACTIVE_LOW>, a switch).  On iPad
 *     the same sensor is the "smart cover" and the system reads it through
 *     SpringBoardServices' SBSSmartCoverService, which is fed by the HID
 *     event system: the service below publishes the sensor page (0x20)
 *     usage "Hall Effect Switch" (0x69) and sends an event when the state
 *     changes.
 *
 *   - the headphone/USB-C audio accessory state.  The 3.5 mm jack of the
 *     Mi 11 lives in a USB-C dongle; the dongle's switch is the fsa4480
 *     (I2C 0x42 on i2c@a94000) and the analog switch position is what can
 *     be read from it.  The jack itself is detected by the WCD938x MBHC
 *     block over SoundWire, which this port does not have, so the value
 *     published here is the best that the hardware gives us.
 *
 * The property names are a GUESS: no Apple phone has a hall switch, so
 * there is no reference implementation to copy them from.  They follow the
 * naming of the HID sensor keys and of the other SWEET services; see
 * logs/research/impl/17-haptics-cover.md.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSArray.h>
#include <pexpert/pexpert.h>

#include "IOHIDEventService.h"
#include "IOHIDEvent.h"
#include "IOHIDUsageTables.h"

extern "C" uint64_t mach_absolute_time(void);

#define super IOHIDEventService

class SweetHIDCover : public IOHIDEventService {
	OSDeclareDefaultStructors(SweetHIDCover);
public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void update(int closed, int headset);
protected:
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE
	{
		return kHIDPage_Sensor;
	}
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE
	{
		return kHIDUsage_Snsr_Mechanical_HallEffectSwitch;
	}
	virtual UInt32 getProductID() APPLE_KEXT_OVERRIDE { return 4; }
};

OSDefineMetaClassAndStructors(SweetHIDCover, IOHIDEventService);

static SweetHIDCover *gSweetCover;

bool
SweetHIDCover::start(IOService *provider)
{
	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	/* Initial state: unknown. */
	setProperty("CoverClosed", kOSBooleanFalse);
	setProperty("LidClosed", kOSBooleanFalse);
	setProperty("HallSwitchState", 0, 32);
	setProperty("CoverState", 0, 32);		/* 0 = open, 1 = closed */
	setProperty("SmartCoverAttached", kOSBooleanFalse);
	setProperty("HeadphonesConnected", kOSBooleanFalse);
	setProperty("SensorPropertySupported", 0x3ull, 32);
	gSweetCover = this;
	registerService();
	IOLog("SWEET HID: cover/headset sensor registered (hall switch)\n");
	return true;
}

void
SweetHIDCover::update(int closed, int headset)
{
	IOHIDEvent *event;

	if (!gSweetCover) {
		return;
	}
	setProperty("CoverClosed", closed ? kOSBooleanTrue : kOSBooleanFalse);
	setProperty("LidClosed", closed ? kOSBooleanTrue : kOSBooleanFalse);
	setProperty("SmartCoverAttached", closed ? kOSBooleanTrue : kOSBooleanFalse);
	setProperty("HallSwitchState", closed ? 1 : 0, 32);
	setProperty("CoverState", closed ? 1 : 0, 32);
	setProperty("HeadphonesConnected", headset ? kOSBooleanTrue : kOSBooleanFalse);
	IOLog("SWEET HID: cover %s, headphones %s\n", closed ? "closed" : "open",
	    headset ? "connected" : "not connected");

	/*
	 * The event: a button style event with the state of the switch.
	 * The exact event the userland expects for a hall switch is a
	 * GUESS; a button event with the usage of the service is the most
	 * generic thing the HID event system can carry.
	 */
	event = IOHIDEvent::buttonEvent(mach_absolute_time(), 1, 0, closed != 0);
	if (event) {
		dispatchEvent(event);
		event->release();
	}
}

/* Called from bsd/dev/sweet_haptic.c when the hall switch changes. */
extern "C" void
sweet_hid_cover(int closed, int headset)
{
	if (gSweetCover) {
		gSweetCover->update(closed, headset);
	}
}
