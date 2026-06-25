/*===========================================================================
 *  MyIntelFramebuffer.cpp
 *  Hackintosh Kext — IOFramebuffer Binding Implementation (Phase 4)
 *  macOS 15 Sequoia SDK
 *///=========================================================================

#include "MyIntelFramebuffer.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>

// Constants missing from macOS 15.2 SDK headers
#ifndef kIOFBBrightness
#define kIOFBBrightness         'brgt'
#endif
#ifndef kIOFBPowerStateOn
#define kIOFBPowerStateOn       1
#endif

#define FBLog(fmt, ...) \
    IOLog("MyIntelFB: [%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

#define super IOFramebuffer
OSDefineMetaClassAndStructors(MyIntelFramebuffer, IOFramebuffer)

#pragma mark - init / free

bool MyIntelFramebuffer::init(OSDictionary *dict)
{
    if (!super::init(dict)) return false;

    fGPU            = NULL;
    fCurrentModeID  = MYFB_DISPLAY_MODE_ID;
    fCurrentDepth   = 0;
    fDisplayOn      = true;
    fFrameCounter   = 0;
    fLastVblankTime = 0;
    fVSyncEnabled   = false;
    fBrightness     = MYFB_BRIGHTNESS_DEFAULT;
    fVRAMDescriptor = NULL;

    FBLog("init() OK - 1920x1080 @ 60Hz");
    return true;
}

void MyIntelFramebuffer::free()
{
    FBLog("free()");
    if (fVRAMDescriptor) {
        fVRAMDescriptor->release();
        fVRAMDescriptor = NULL;
    }
    fGPU = NULL;
    super::free();
}

#pragma mark - start / stop

bool MyIntelFramebuffer::start(IOService *provider)
{
    FBLog("=== start() ===");
    if (!super::start(provider)) {
        FBLog("super::start failed");
        return false;
    }

    fGPU = OSDynamicCast(MyIntelGPU, provider);
    if (!fGPU) {
        FBLog("provider is not MyIntelGPU");
        return false;
    }
    fGPU->retain();

    setupDefaultMode();
    createVRAMDescriptor();
    publishIORegistryProperties();
    registerService(kIOServiceAsynchronous);

    FBLog("start() OK");
    return true;
}

void MyIntelFramebuffer::stop(IOService *provider)
{
    FBLog("stop()");
    // vsyncEnable/VblankEvent removed from macOS 15.2 SDK IOFramebuffer
    if (fVRAMDescriptor) {
        fVRAMDescriptor->release();
        fVRAMDescriptor = NULL;
    }
    if (fGPU) {
        fGPU->release();
        fGPU = NULL;
    }
    super::stop(provider);
}

#pragma mark - Pure Virtual Methods

IODeviceMemory *MyIntelFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    (void)aperture;
    if (!fVRAMDescriptor) return NULL;
    fVRAMDescriptor->retain();
    return fVRAMDescriptor;
}

const char *MyIntelFramebuffer::getPixelFormats(void)
{
    return "O010";
}

IOReturn MyIntelFramebuffer::getInformationForDisplayMode(
        IODisplayModeID displayMode,
        IODisplayModeInformation *info)
{
    if (!info) return kIOReturnBadArgument;
    if (displayMode != MYFB_DISPLAY_MODE_ID)
        return kIOReturnUnsupportedMode;

    bzero(info, sizeof(IODisplayModeInformation));
    info->nominalWidth    = MYFB_H_ACTIVE;
    info->nominalHeight   = MYFB_V_ACTIVE;
    info->refreshRate     = MYFB_REFRESH_RATE << 16; // 16.16 fixed point
    info->maxDepthIndex   = 0;
    info->flags           = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;

    FBLog("getInformationForDisplayMode: %ux%u @ %uHz",
          (uint32_t)info->nominalWidth, (uint32_t)info->nominalHeight,
          (uint32_t)info->refreshRate);
    return kIOReturnSuccess;
}

IOReturn MyIntelFramebuffer::getPixelInformation(
        IODisplayModeID displayMode,
        IOIndex depth,
        IOPixelAperture aperture,
        IOPixelInformation *pixelInfo)
{
    (void)depth;
    (void)aperture;

    if (!pixelInfo) return kIOReturnBadArgument;
    if (displayMode != MYFB_DISPLAY_MODE_ID)
        return kIOReturnUnsupportedMode;

    bzero(pixelInfo, sizeof(IOPixelInformation));

    pixelInfo->pixelType        = kIORGBDirectPixels;
    pixelInfo->componentCount   = 3;
    pixelInfo->bitsPerPixel     = MYFB_BITS_PER_PIXEL;
    pixelInfo->bytesPerRow      = MYFB_BYTES_PER_ROW;
    pixelInfo->bytesPerPlane    = 0;
    pixelInfo->bitsPerComponent = MYFB_BITS_PER_COMP;
    pixelInfo->componentMasks[0] = 0x00FF0000;
    pixelInfo->componentMasks[1] = 0x0000FF00;
    pixelInfo->componentMasks[2] = 0x000000FF;
    pixelInfo->componentMasks[3] = 0xFF000000;
    pixelInfo->flags            = 0;
    strlcpy(pixelInfo->pixelFormat, IO32BitDirectPixels, sizeof(pixelInfo->pixelFormat));
    pixelInfo->activeWidth  = MYFB_H_ACTIVE;
    pixelInfo->activeHeight = MYFB_V_ACTIVE;

    return kIOReturnSuccess;
}

UInt64 MyIntelFramebuffer::getPixelFormatsForDisplayMode(
        IODisplayModeID displayMode,
        IOIndex depth)
{
    (void)displayMode;
    (void)depth;
    return 0; // Obsolete per IOFramebuffer.h — must return 0
}

#pragma mark - Non-Pure Virtual Overrides

UInt32 MyIntelFramebuffer::getDisplayModeCount(void)
{
    return MYFB_MODE_COUNT;
}

IOReturn MyIntelFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes) return kIOReturnBadArgument;
    allDisplayModes[0] = MYFB_DISPLAY_MODE_ID;
    return kIOReturnSuccess;
}

IOReturn MyIntelFramebuffer::getCurrentDisplayMode(
        IODisplayModeID *displayMode,
        IOIndex *depth)
{
    if (!displayMode || !depth) return kIOReturnBadArgument;
    *displayMode = fCurrentModeID;
    *depth       = fCurrentDepth;
    return kIOReturnSuccess;
}

IOReturn MyIntelFramebuffer::setDisplayMode(
        IODisplayModeID displayMode,
        IOIndex depth)
{
    FBLog("setDisplayMode: mode=%u depth=%lu", (uint32_t)displayMode, (long)depth);
    if (displayMode != MYFB_DISPLAY_MODE_ID)
        return kIOReturnUnsupportedMode;
    if (fCurrentModeID == displayMode && fCurrentDepth == depth)
        return kIOReturnSuccess;

    fCurrentModeID = displayMode;
    fCurrentDepth  = depth;

    if (fGPU) {
        fGPU->initDisplay();
    }
    return kIOReturnSuccess;
}

IODeviceMemory *MyIntelFramebuffer::getVRAMRange(void)
{
    if (!fVRAMDescriptor) return NULL;
    fVRAMDescriptor->retain();
    return fVRAMDescriptor;
}

IOReturn MyIntelFramebuffer::getAttributeForConnection(
        IOIndex connectIndex,
        IOSelect key,
        uintptr_t *value)
{
    (void)connectIndex;
    switch (key) {
        case kIOFBBrightness:
            if (value) *value = fBrightness;
            return kIOReturnSuccess;
        default:
            break;
    }
    return kIOReturnUnsupported;
}

IOReturn MyIntelFramebuffer::setAttributeForConnection(
        IOIndex connectIndex,
        IOSelect key,
        uintptr_t value)
{
    (void)value;
    switch (key) {
        case kIOFBBrightness:
            fBrightness = (value > MYFB_BRIGHTNESS_MAX) ? MYFB_BRIGHTNESS_MAX : (UInt32)value;
            FBLog("brightness = %u", (uint32_t)fBrightness);
            return kIOReturnSuccess;
        default:
            break;
    }
    return kIOReturnUnsupported;
}

#pragma mark - Public Methods

void MyIntelFramebuffer::handleVblank(void)
{
    fFrameCounter++;
    clock_get_uptime(&fLastVblankTime);
    // VblankEvent removed from macOS 15.2 SDK IOFramebuffer
}

void MyIntelFramebuffer::setBrightness(UInt32 brightness)
{
    fBrightness = (brightness > MYFB_BRIGHTNESS_MAX) ? MYFB_BRIGHTNESS_MAX : brightness;
}

IOReturn MyIntelFramebuffer::performPowerStateChange(
        IOIndex connectIndex,
        UInt32 powerState)
{
    (void)connectIndex;
    if (powerState == kIOFBPowerStateOn) {
        fDisplayOn = true;
        FBLog("power ON");
        if (fGPU) {
            fGPU->panelPowerOn();
            fGPU->initBacklight();
        }
    } else {
        fDisplayOn = false;
        FBLog("power OFF");
    }
    return kIOReturnSuccess;
}

#pragma mark - Internal Helpers

void MyIntelFramebuffer::setupDefaultMode(void)
{
    fCurrentModeID = MYFB_DISPLAY_MODE_ID;
    fCurrentDepth  = 0;
}

bool MyIntelFramebuffer::createVRAMDescriptor(void)
{
    if (!fGPU) return false;

    IOMemoryMap *apertureMap = fGPU->getApertureMap();
    uint64_t apertureSize = apertureMap ? apertureMap->getLength() : 0;

    if (apertureMap && apertureSize > 0) {
        fVRAMDescriptor = static_cast<IODeviceMemory *>(
            IOMemoryDescriptor::withAddressRange(
                apertureMap->getAddress(), apertureSize,
                kIODirectionInOut, kernel_task).detach());
    }

    if (!fVRAMDescriptor) {
        FBLog("WARNING: No BAR2 - creating 16MB stub");
        fVRAMDescriptor = static_cast<IODeviceMemory *>(
            IOMemoryDescriptor::withAddressRange(
                0, 16 * 1024 * 1024,
                kIODirectionInOut, kernel_task).detach());
    }

    if (fVRAMDescriptor) {
        fVRAMDescriptor->retain();
        FBLog("VRAM: 0x%llX bytes", fVRAMDescriptor->getLength());
        return true;
    }
    return false;
}

void MyIntelFramebuffer::publishIORegistryProperties(void)
{
    OSDictionary *dict = OSDictionary::withCapacity(8);
    if (!dict) return;

    OSString *modeStr = OSString::withCString("1920x1080 @ 60Hz");
    if (modeStr) { dict->setObject("display-mode", modeStr); modeStr->release(); }

    OSString *fmtStr = OSString::withCString("XRGB8888 (O010)");
    if (fmtStr) { dict->setObject("pixel-format", fmtStr); fmtStr->release(); }

    if (fGPU) {
        IOMemoryMap *ap = fGPU->getApertureMap();
        if (ap) {
            OSNumber *vramNum = OSNumber::withNumber(ap->getLength(), 64);
            if (vramNum) { dict->setObject("VRAM", vramNum); vramNum->release(); }
        }
    }

    dict->setObject("built-in", kOSBooleanTrue);

    OSString *dispType = OSString::withCString("LCD");
    if (dispType) { dict->setObject("display-type", dispType); dispType->release(); }

    setProperties(dict);
    dict->release();
    FBLog("IORegistry properties published");
}
