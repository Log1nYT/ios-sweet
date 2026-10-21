/*
 * SweetGraphicsAccelerator - an IOKit service that describes the graphics
 * hardware of venus (Adreno 660) under a deliberately NON-Apple name.
 *
 * WHY THIS SERVICE EXISTS
 * =======================
 * The iOS 16.7 userland looks for a graphics accelerator all the time:
 * backboardd does IOServiceMatching("IOAcceleratorES") 5449 times and
 * IOServiceMatching("AGXAccelerator") 34 times per log and finds nothing
 * (logs/research/18-iokit-gap-inventory.md, section 2.1).  That is not a
 * defect: with no accelerator published, CoreAnimation falls back to its
 * software rasteriser and the UI works (upstream sweet renders at 20-30 fps
 * with no GPU patch at all).
 *
 * The trap (logs/research/07-gpu-plan.md, section 4.3): if a service whose
 * CLASS NAME were "IOAcceleratorES" or "AGXAccelerator" were published,
 * backboardd would find it, CoreAnimation would pick the Metal renderer, and
 * the closed AGXMetalPlugin inside the dyld shared cache would block waiting
 * for a command queue that does not exist - the whole UI freezes.  Two
 * things therefore keep this service safe:
 *
 *   1. The class name is "SweetGraphicsAccelerator".  IOServiceMatching
 *      compares the class name of a published service with the
 *      "IOProviderClass" / "IOClass" key of the matching dictionary, so a
 *      service of this class can never answer a query for "IOAcceleratorES",
 *      "IOAcceleratorESLegacy", "AGXAccelerator" or "IOAccelerator".
 *   2. None of the properties the Metal / CoreAnimation path keys on are
 *      published.  The full list, with the reason for each omission, is in
 *      the comment above sweet_gpu_publish() below.
 *
 * WHAT THE SERVICE IS FOR
 * =======================
 * It is an *informational* service: it says what the GPU is (chip id, the
 * device tree node it comes from, the limits of the software renderer that
 * stands in for it) and it carries a user client whose "device info" call
 * answers immediately with a version string, the pixel format list and the
 * maximum texture size.  Nothing in here can block, so anything that opens
 * the user client gets an answer instead of a hang.
 *
 * It is NOT a driver.  No command stream, no ringbuffer, no GPU register
 * access: the Adreno is not powered up by this service (that is stage B of
 * report 07 and it needs the CX rail vote, the GPU_CC clocks and the zap
 * shader, none of which exist yet).
 *
 * BOOT ARG
 * ========
 * sweetgpuiokit=1 publishes the service.  Without it the class is never
 * published, so the "no accelerator at all" behaviour - the one that keeps
 * CoreAnimation on the CPU renderer - can be compared on the same kernel
 * build.  Default: OFF.
 */

#if defined(ARM64_BOARD_CONFIG_SWEET)

#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>

/* --------------------------------------------------------------------- */
/* What we know about the GPU of venus (report 07, sections 2.1 and 3.2) */
/* --------------------------------------------------------------------- */

/*
 * Adreno 660, CAF/KGSL binding "qcom,kgsl-3d0", chip id 0x06060001
 * (family 6, major 6, minor 0, patch 1 -> upstream Linux prints "u.6.6.0.1";
 * the ADRENO_6XX_GEN4 entry of the msm/adreno driver).
 */
#define SWEET_GPU_CHIPID                  0x06060001u
#define SWEET_GPU_COMPATIBLE              "qcom,kgsl-3d0"
#define SWEET_GPU_DT_PATH                 "IODeviceTree:/arm-io/gpu"

/*
 * The limits below are the limits of the SOFTWARE renderer that stands in
 * for the GPU (CoreAnimation's CPU rasteriser writing into our IOSurface
 * allocator) - not the limits of the Adreno, which are much higher.  They
 * are the numbers a userspace tool needs to size a surface the compositor
 * will actually accept.
 */
#define SWEET_GPU_MAX_TEXTURE_SIZE        8192
#define SWEET_GPU_MAX_DIMENSION           8192

#define SWEET_GPU_VERSION                 1
#define SWEET_GPU_VERSION_STRING          "sweetgpu 1.0"
#define SWEET_GPU_DEVICE_NAME             "Sweet Venus GPU (Adreno 660 / KGSL, software renderer)"

/* The pixel formats our surface allocator accepts (fourcc). */
#define SWEET_GPU_FORMAT_BGRA             0x42475241u   /* 'BGRA' */
#define SWEET_GPU_FORMAT_ARGB             0x41524742u   /* 'ARGB' */
#define SWEET_GPU_FORMAT_RGBA             0x52474241u   /* 'RGBA' */
#define SWEET_GPU_MAX_FORMATS             8

/* The user client selectors. */
enum {
	kSweetGPUGetDeviceInfo    = 0,    /* the whole struct below            */
	kSweetGPUGetVersion       = 1,    /* the version string                */
	kSweetGPUGetPixelFormats  = 2,    /* count + the fourcc list            */
	kSweetGPUGetMaxTexture    = 3,    /* the maximum texture size          */
	kSweetGPUGetCapabilities  = 4,    /* the capabilities as an XML plist  */
};

/* The answer to the "device info" call. */
typedef struct {
	uint32_t magic;                  /* 'SGP1'                              */
	uint32_t structSize;             /* sizeof(SweetGPUDeviceInfo)         */
	uint32_t version;                /* SWEET_GPU_VERSION                   */
	uint32_t maxTextureSize;         /* SWEET_GPU_MAX_TEXTURE_SIZE          */
	uint32_t maxDimension;           /* SWEET_GPU_MAX_DIMENSION             */
	uint32_t formatCount;            /* number of entries in formats[]      */
	uint32_t formats[SWEET_GPU_MAX_FORMATS];
	uint32_t chipId;                 /* 0x06060001 = Adreno 660             */
	uint32_t flags;                  /* reserved, 0                         */
	char     versionString[32];
	char     deviceName[64];
} SweetGPUDeviceInfo;

#define SWEET_GPU_INFO_MAGIC 0x31504753u     /* 'SGP1' little endian */

class SweetGraphicsAccelerator : public IOService {
	OSDeclareDefaultStructors(SweetGraphicsAccelerator);

	void publish(uint32_t chipId, bool haveChipId);
	bool readDeviceTreeChipId(uint32_t *chipId);

public:
	uint32_t fChipId;
	bool     fHaveChipId;

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetGraphicsAccelerator, IOService);

class SweetGraphicsAcceleratorUserClient : public IOUserClient {
	OSDeclareDefaultStructors(SweetGraphicsAcceleratorUserClient);

	SweetGraphicsAccelerator *fAccel;

public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(SweetGraphicsAcceleratorUserClient, IOUserClient);

/*
 * The fourcc list.  Only the formats the surface allocator really handles
 * are offered: claiming a format we cannot honour is how the display
 * scaler stub in report 18 (item 5.4) would end up producing wrong frames.
 */
static uint32_t
sweet_gpu_formats(uint32_t *out, uint32_t max)
{
	uint32_t n = 0;
	if (max > 0) {
		out[n++] = SWEET_GPU_FORMAT_BGRA;
	}
	if (max > 1) {
		out[n++] = SWEET_GPU_FORMAT_ARGB;
	}
	if (max > 2) {
		out[n++] = SWEET_GPU_FORMAT_RGBA;
	}
	return n;
}

static void
sweet_gpu_format_string(uint32_t fmt, char *out, uint32_t len)
{
	out[0] = '\0';
	if (len < 5) {
		return;
	}
	out[0] = (char)((fmt >> 24) & 0xff);
	out[1] = (char)((fmt >> 16) & 0xff);
	out[2] = (char)((fmt >> 8) & 0xff);
	out[3] = (char)(fmt & 0xff);
	out[4] = '\0';
}

/*
 * Read the chip id out of the device tree, if the GPU node is there.
 * Stage B1 of report 07 adds /arm-io/gpu with compatible "qcom,kgsl-3d0" and
 * qcom,chipid 0x06060001; until that node exists this returns false and the
 * service publishes the values from the CAF documentation instead.
 */
bool
SweetGraphicsAccelerator::readDeviceTreeChipId(uint32_t *chipId)
{
	IORegistryEntry *node = IORegistryEntry::fromPath(SWEET_GPU_DT_PATH);
	if (node == NULL) {
		return false;
	}
	bool found = false;
	static const char *keys[] = { "qcom,chipid", "chip-id", "gpu-chip-id" };
	for (unsigned int i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		OSNumber *num = OSDynamicCast(OSNumber, node->getProperty(keys[i]));
		if (num != NULL) {
			*chipId = num->unsigned32BitValue();
			found = true;
			break;
		}
	}
	node->release();
	return found;
}

/*
 * Publish the properties.
 *
 * PUBLISHED (all of them harmless - none of them is looked at by the
 * CoreAnimation renderer selection or by the Metal plug-in):
 *
 *   IOUserClientClass                  "SweetGraphicsAcceleratorUserClient"
 *   IOMaintainedDevice                 true      (power management bookkeeping
 *                                               only; the kernel never reads it)
 *   SweetGraphicsAcceleratorVersion    1
 *   SweetGraphicsAcceleratorVersionString "sweetgpu 1.0"
 *   SweetGraphicsAcceleratorDeviceName "..."
 *   SweetGraphicsAcceleratorChipID     0x06060001
 *   SweetGraphicsAcceleratorMaxTextureSize 8192
 *   SweetGraphicsAcceleratorMaxDimension  8192
 *   SweetGraphicsAcceleratorPixelFormats   [ 'BGRA', 'ARGB', 'RGBA' ]
 *   SweetGraphicsAcceleratorCapabilities   (dictionary with the same values)
 *
 * DELIBERATELY NOT PUBLISHED - each one of these is a trap:
 *
 *   IOGPUFamily, "AppleGPUFamilyN", "AppleGPUFamily"
 *       The Metal plug-in (AGXMetalPlugin / AGXMetalG<gen> in the dyld shared
 *       cache) and MTLCreateSystemDefaultDevice() look for a service that
 *       carries a GPU *family* string.  The family selects the command
 *       stream format and the plug-in image.  Publishing "AppleGPUFamily8"
 *       (the A11/A12 family) would make the userland load a plug-in that
 *       then talks a protocol no driver here implements -> the UI freezes.
 *
 *   "MetalPluginName", "AGXMetalPlugin", "AGXMetalG*", "IOGPUCommandQueue",
 *   "IOGPUCommandQueueName", "IOGPUFamilyCapabilities"
 *       The names the plug-in probes for when it looks for its kernel
 *       counterpart.
 *
 *   "IOAccelerator*", "AGX*", "IOGPU*"
 *       Anything that starts with one of these prefixes is an accelerator
 *       identifier that some framework may match on.
 *
 *   "IOPixelFormat", "IOAccelDeviceProfile", "IOAccelStatistics*"
 *       Accelerator-only properties: they are what IOSurfaceAccelerator and
 *       the GPU statistics code look for.
 *
 *   "HIDServiceSupport", "PrimaryUsage", "DeviceUsagePairs"
 *       Would make the iOS HID event system subscribe to us (it matches
 *       IOProviderClass = IOService with IOPropertyMatch{HIDServiceSupport}).
 *
 *   "backlight-control", "IOSurfaceAcceleratorCapabilitiesDict"
 *       Belong to other services (the backlight stub, the scaler stub); if
 *       we claimed them the userland would use the wrong service.
 */
void
SweetGraphicsAccelerator::publish(uint32_t chipId, bool haveChipId)
{
	setName("SweetGraphicsAccelerator");

	/* The user client: the only way in, and it always answers. */
	setProperty(kIOUserClientClassKey, "SweetGraphicsAcceleratorUserClient");

	/* The two properties every accelerator-shaped service carries. */
	setProperty("IOMaintainedDevice", true);
	setProperty("external", false);

	setProperty("SweetGraphicsAcceleratorVersion", (uint64_t)SWEET_GPU_VERSION, 32);
	setProperty("SweetGraphicsAcceleratorVersionString", SWEET_GPU_VERSION_STRING);
	setProperty("SweetGraphicsAcceleratorDeviceName", SWEET_GPU_DEVICE_NAME);
	setProperty("SweetGraphicsAcceleratorMaxTextureSize", (uint64_t)SWEET_GPU_MAX_TEXTURE_SIZE, 32);
	setProperty("SweetGraphicsAcceleratorMaxDimension", (uint64_t)SWEET_GPU_MAX_DIMENSION, 32);
	setProperty("SweetGraphicsAcceleratorRenderer", "software (CoreAnimation CPU rasteriser)");

	if (haveChipId) {
		setProperty("SweetGraphicsAcceleratorChipID", (uint64_t)chipId, 32);
		setProperty("SweetGraphicsAcceleratorChipIDHex", (uint64_t)chipId, 32);
	} else {
		setProperty("SweetGraphicsAcceleratorChipID", (uint64_t)0, 32);
	}

	uint32_t formats[SWEET_GPU_MAX_FORMATS];
	uint32_t n = sweet_gpu_formats(formats, SWEET_GPU_MAX_FORMATS);
	OSArray *array = OSArray::withCapacity(n);
	for (uint32_t i = 0; i < n; i++) {
		char name[8];
		sweet_gpu_format_string(formats[i], name, sizeof(name));
		array->setObject(OSNumber::withNumber((uint64_t)formats[i], 32));
		array->setObject(OSString::withCString(name));
	}
	setProperty("SweetGraphicsAcceleratorPixelFormats", array);
	array->release();

	/* The same numbers as a dictionary, for a tool that reads properties. */
	OSDictionary *caps = OSDictionary::withCapacity(8);
	caps->setObject("Version", OSNumber::withNumber((uint64_t)SWEET_GPU_VERSION, 32));
	caps->setObject("VersionString", OSString::withCString(SWEET_GPU_VERSION_STRING));
	caps->setObject("DeviceName", OSString::withCString(SWEET_GPU_DEVICE_NAME));
	caps->setObject("ChipID", OSNumber::withNumber((uint64_t)(haveChipId ? chipId : 0), 32));
	caps->setObject("MaxTextureSize", OSNumber::withNumber((uint64_t)SWEET_GPU_MAX_TEXTURE_SIZE, 32));
	caps->setObject("MaxDimension", OSNumber::withNumber((uint64_t)SWEET_GPU_MAX_DIMENSION, 32));
	caps->setObject("PixelFormats", array);
	setProperty("SweetGraphicsAcceleratorCapabilities", caps);
	caps->release();
}

bool
SweetGraphicsAccelerator::start(IOService *provider)
{
	int enabled = 0;
	PE_parse_boot_argn("sweetgpuiokit", &enabled, sizeof(enabled));
	if (enabled == 0) {
		/*
		 * Default OFF: the class is instantiated (the personality in
		 * KernelConfigTables.cpp attaches it to IOResources) but never
		 * published, so the registry looks exactly as it does today -
		 * no accelerator at all, CoreAnimation on the CPU renderer.
		 */
		IOLog("SWEET GPU: sweetgpuiokit is not set, the service stays unpublished\n");
		return false;
	}

	if (!IOService::start(provider)) {
		return false;
	}

	fChipId = SWEET_GPU_CHIPID;
	fHaveChipId = readDeviceTreeChipId(&fChipId);
	if (!fHaveChipId) {
		/* No /arm-io/gpu node yet: use the value from the CAF DT. */
		fChipId = SWEET_GPU_CHIPID;
	}

	publish(fChipId, true);
	registerService();

	IOLog("SWEET GPU: SweetGraphicsAccelerator published (chip 0x%x%s, "
	    "max texture %u, formats %u, renderer software; NO IOGPUFamily, "
	    "NO IOAcceleratorES/AGXAccelerator class, so CoreAnimation stays on "
	    "the CPU rasteriser)\n",
	    fChipId, fHaveChipId ? " from the device tree" : " (hardcoded)",
	    (unsigned)SWEET_GPU_MAX_TEXTURE_SIZE, (unsigned)3);
	return true;
}

/* ------------------------------------------------------------------ */
/* The user client                                                     */
/* ------------------------------------------------------------------ */

bool
SweetGraphicsAcceleratorUserClient::start(IOService *provider)
{
	fAccel = OSDynamicCast(SweetGraphicsAccelerator, provider);
	if (fAccel == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	return true;
}

IOReturn
SweetGraphicsAcceleratorUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

/* Copy len bytes into the caller's output structure. */
static IOReturn
sweet_gpu_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
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
 * The capabilities as an XML property list, so that a test tool can read
 * them with plistlib without linking against IOKit.
 */
static uint32_t
sweet_gpu_caps_plist(char *buf, uint32_t len, uint32_t chipId)
{
	uint32_t n = 0;
	uint32_t formats[SWEET_GPU_MAX_FORMATS];
	uint32_t nfmt = sweet_gpu_formats(formats, SWEET_GPU_MAX_FORMATS);
#define SWEET_GPU_ADD(...) do {                                   \
	int _r = snprintf(buf + n, len - n, __VA_ARGS__);       \
	if (_r < 0 || (uint32_t)_r >= len - n) {                  \
		return len;                                    \
	}                                                       \
	n += (uint32_t)_r;                                      \
} while (0)

	SWEET_GPU_ADD("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
	SWEET_GPU_ADD("<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n");
	SWEET_GPU_ADD("<plist version=\"1.0\"><dict>\n");
	SWEET_GPU_ADD("<key>Version</key><integer>%u</integer>\n", (unsigned)SWEET_GPU_VERSION);
	SWEET_GPU_ADD("<key>VersionString</key><string>%s</string>\n", SWEET_GPU_VERSION_STRING);
	SWEET_GPU_ADD("<key>DeviceName</key><string>%s</string>\n", SWEET_GPU_DEVICE_NAME);
	SWEET_GPU_ADD("<key>ChipID</key><integer>%u</integer>\n", chipId);
	SWEET_GPU_ADD("<key>MaxTextureSize</key><integer>%u</integer>\n", (unsigned)SWEET_GPU_MAX_TEXTURE_SIZE);
	SWEET_GPU_ADD("<key>MaxDimension</key><integer>%u</integer>\n", (unsigned)SWEET_GPU_MAX_DIMENSION);
	SWEET_GPU_ADD("<key>Renderer</key><string>software</string>\n");
	SWEET_GPU_ADD("<key>PixelFormats</key><array>");
	for (uint32_t i = 0; i < nfmt; i++) {
		char name[8];
		sweet_gpu_format_string(formats[i], name, sizeof(name));
		SWEET_GPU_ADD("<string>%s</string>", name);
	}
	SWEET_GPU_ADD("</array>\n");
	SWEET_GPU_ADD("</dict></plist>\n");
#undef SWEET_GPU_ADD
	return n;
}

IOReturn
SweetGraphicsAcceleratorUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint32_t chipId = (fAccel != NULL) ? fAccel->fChipId : SWEET_GPU_CHIPID;

	switch (selector) {
	case kSweetGPUGetDeviceInfo: {
		/*
		 * The "device info" call: one structure with everything a
		 * caller needs - the version string, the pixel format list
		 * and the maximum texture size.  It is answered from memory,
		 * so it cannot block.
		 */
		SweetGPUDeviceInfo info;
		memset(&info, 0, sizeof(info));
		info.magic = SWEET_GPU_INFO_MAGIC;
		info.structSize = (uint32_t)sizeof(info);
		info.version = SWEET_GPU_VERSION;
		info.maxTextureSize = SWEET_GPU_MAX_TEXTURE_SIZE;
		info.maxDimension = SWEET_GPU_MAX_DIMENSION;
		info.formatCount = sweet_gpu_formats(info.formats, SWEET_GPU_MAX_FORMATS);
		info.chipId = chipId;
		info.flags = 0;
		strlcpy(info.versionString, SWEET_GPU_VERSION_STRING, sizeof(info.versionString));
		strlcpy(info.deviceName, SWEET_GPU_DEVICE_NAME, sizeof(info.deviceName));
		args->scalarOutputCount = 0;
		ret = sweet_gpu_struct_out(args, &info, (uint32_t)sizeof(info));
		break;
	}
	case kSweetGPUGetVersion:
		args->scalarOutputCount = 0;
		ret = sweet_gpu_struct_out(args, SWEET_GPU_VERSION_STRING,
		    (uint32_t)(sizeof(SWEET_GPU_VERSION_STRING) - 1));
		break;
	case kSweetGPUGetPixelFormats: {
		uint32_t formats[SWEET_GPU_MAX_FORMATS];
		uint32_t n = sweet_gpu_formats(formats, SWEET_GPU_MAX_FORMATS);
		if (args->scalarOutputCount < 1) {
			args->scalarOutputCount = 0;
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = n;
		args->scalarOutputCount = 1;
		ret = sweet_gpu_struct_out(args, formats, n * (uint32_t)sizeof(uint32_t));
		break;
	}
	case kSweetGPUGetMaxTexture:
		if (args->scalarOutputCount < 1) {
			args->scalarOutputCount = 0;
			ret = kIOReturnBadArgument;
			break;
		}
		args->scalarOutput[0] = SWEET_GPU_MAX_TEXTURE_SIZE;
		args->scalarOutputCount = 1;
		break;
	case kSweetGPUGetCapabilities: {
		char buf[1024];
		uint32_t n = sweet_gpu_caps_plist(buf, sizeof(buf), chipId);
		args->scalarOutputCount = 0;
		ret = sweet_gpu_struct_out(args, buf, n);
		break;
	}
	default:
		ret = kIOReturnUnsupported;
		break;
	}

	char name[32];
	proc_selfname(name, sizeof(name));
	IOLog("SWEET GPU: %s sel %u -> 0x%x\n", name, selector, (unsigned)ret);
	return ret;
}

#endif /* ARM64_BOARD_CONFIG_SWEET */
