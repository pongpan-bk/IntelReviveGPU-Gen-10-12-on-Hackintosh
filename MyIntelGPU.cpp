/*=====================================================================================
 * MyIntelGPU.cpp
 * Hackintosh Kext — FakeID Alder Lake → Coffee Lake
 *
 * Implementation of the MyIntelGPU Class
 *===================================================================================*/

#include "MyIntelGPU.hpp"
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>

#define FB_MAX_CRTC 4

OSDefineMetaClassAndStructors(MyIntelGPU, IOService)

bool MyIntelGPU::init(OSDictionary *dictionary) {
    if (!IOService::init(dictionary)) {
        return false;
    }
    fPCIDevice = nullptr;
    fMmioMap = nullptr;
    fMmioBase = nullptr;
    fInterruptSource = nullptr;
    fWorkLoop = nullptr;
    fGtIntrMaskDW0 = 0;
    fDisplayIntrMask = 0;
    fInterruptsReady = false;
    fHotplugDDIMask = 0;
    fHotplugLastStatus = 0;
    fFramebufferCount = 0;
    fScanoutBuffer = nullptr;
    
    for (uint32_t i = 0; i < FB_MAX_CRTC; i++) {
        fFbWidth[i] = 0;
        fFbHeight[i] = 0;
        fFbBpp[i] = 0;
        fFbRefresh[i] = 0;
        fFbBase[i] = nullptr;
    }
    return true;
}

void MyIntelGPU::free() {
    IOService::free();
}

IOService* MyIntelGPU::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) return nullptr;

    uint16_t vendorId = pciDevice->configRead16(kIOPCIConfigVendorID);
    uint16_t deviceId = pciDevice->configRead16(kIOPCIConfigDeviceID);

    if (vendorId != 0x8086) return nullptr;

    // Verify Alder Lake / Raptor Lake IDs (0x46A0, 0x46A1, 0x46A6, 0x46B0, 0x46B1, etc.)
    if ((deviceId & 0xFFF0) == 0x46A0 || (deviceId & 0xFFF0) == 0x46B0) {
        *score += 2000;
        return this;
    }
    return nullptr;
}

bool MyIntelGPU::start(IOService *provider) {
    if (!IOService::start(provider)) return false;

    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) return false;

    fPCIDevice->setMemoryEnable(true);

    // Map BAR0 for MMIO Registers
    fMmioMap = fPCIDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!fMmioMap) {
        IOLog("MyIntelGPU::start - Failed to map BAR0 MMIO memory\n");
        return false;
    }
    fMmioBase = reinterpret_cast<volatile uint8_t *>(fMmioMap->getVirtualAddress());

    // Spoof Coffee Lake Device ID (e.g., 0x3E9B8086) into IORegistry for AppleIntelCFLGraphics
    fPCIDevice->setProperty("device-id", reinterpret_cast<const void*>(0x3E9B), 4);
    fPCIDevice->setProperty("vendor-id", reinterpret_cast<const void*>(0x8086), 4);

    if (!initInterrupts()) {
        IOLog("MyIntelGPU::start - Failed to initialize event loop interrupts\n");
    }

    installInterruptHandlers();
    initBacklight();
    registerFramebuffer();

    IOLog("MyIntelGPU::start - Successfully spoofed Alder Lake/Raptor Lake GPU as Coffee Lake\n");
    return true;
}

void MyIntelGPU::stop(IOService *provider) {
    if (fWorkLoop && fInterruptSource) {
        fInterruptSource->disable();
        fWorkLoop->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = nullptr;
    }
    if (fMmioMap) {
        fMmioMap->release();
        fMmioMap = nullptr;
    }
    IOService::stop(provider);
}

uint32_t MyIntelGPU::readRegister32(uint32_t offset) {
    if (!fMmioBase) return 0;
    OSSynchronizeIO();
    return *reinterpret_cast<volatile uint32_t *>(fMmioBase + offset);
}

void MyIntelGPU::writeRegister32(uint32_t offset, uint32_t value) {
    if (!fMmioBase) return;
    *reinterpret_cast<volatile uint32_t *>(fMmioBase + offset) = value;
    OSSynchronizeIO();
}

bool MyIntelGPU::initInterrupts() {
    if (!fPCIDevice) return false;

    fWorkLoop = getWorkLoop();
    if (!fWorkLoop) {
        fWorkLoop = IOWorkLoop::workLoop();
        if (!fWorkLoop) return false;
    }

    fInterruptSource = IOInterruptEventSource::interruptEventSource(
                           this,
                           OSMemberFunctionCast(IOInterruptAction, this, &MyIntelGPU::handleInterrupt),
                           fPCIDevice,
                           0);

    if (!fInterruptSource) return false;

    if (fWorkLoop->addEventSource(fInterruptSource) != kIOReturnSuccess) {
        fInterruptSource->release();
        fInterruptSource = nullptr;
        return false;
    }

    fInterruptSource->enable();
    return true;
}

void MyIntelGPU::handleInterrupt(OSObject *owner, IOInterruptEventSource *src, int count) {
    MyIntelGPU *instance = OSDynamicCast(MyIntelGPU, owner);
    if (!instance) return;

    // Read Master Interrupt Control register to acknowledge the signaling state
    uint32_t de_iir = instance->readRegister32(0x44208); // DE_IIR (Display Engine Interrupt Identity)
    uint32_t gt_iir = instance->readRegister32(0x44018); // GT_IIR (Graphics Technology Interrupt Identity)

    if (de_iir & (1U << 0)) {
        // Handle Pipe A Vblank Event
        instance->writeRegister32(0x44208, (1U << 0));
    }
    if (de_iir & (1U << 11)) {
        // Handle South Display Engine Hotplug Event
        uint32_t shotplug = instance->readRegister32(0xC4030); // SHOTPLUG_STATUS
        instance->fHotplugLastStatus = shotplug;
        instance->writeRegister32(0xC4030, shotplug); // Clear Hotplug status bits
        instance->writeRegister32(0x44208, (1U << 11));
    }
    if (gt_iir) {
        instance->writeRegister32(0x44018, gt_iir); // Acknowledge all active GT interrupts
    }
}

void MyIntelGPU::initBacklight() {
    // Read the current state of Intel Gen12/Raptor Lake Backlight PWM Control Register
    uint32_t rpl_blc_pwm_ctl = readRegister32(0xC8250);
    
    // Extract base target hardware frequency limits/period sequence
    uint32_t target_period = rpl_blc_pwm_ctl & 0xFFFF;
    
    // Enforce valid safe architectural clock boundaries if unprogrammed by the firmware
    if (target_period < 0x0100 || target_period > 0xFFFF) {
        target_period = 0x1388; // Establish baseline safe 5000-unit period boundaries
    }
    
    // Reconstruct control payload: preserve configuration, strip old period
    rpl_blc_pwm_ctl &= ~0xFFFF;
    rpl_blc_pwm_ctl |= target_period;
    
    // Resolve hardware polarity transformations: handle clear bit shifts
    // Bit 29: 0 = Native Active High Intensity, 1 = Active Low Shift
    rpl_blc_pwm_ctl &= ~(1U << 29); 
    
    // Assert structural enable flag inside Bit 31 to turn on the PWM generation engine
    rpl_blc_pwm_ctl |= (1U << 31);
    
    writeRegister32(0xC8250, rpl_blc_pwm_ctl);
    
    // Synchronize secondary state array with maximum active operational baseline
    writeRegister32(0xC8254, target_period); 
}

void MyIntelGPU::setBacklightBrightness(uint32_t percent) {
    if (percent > 100) {
        percent = 100;
    }
    
    // Retrieve reference period mapping boundaries from Master Control
    uint32_t rpl_blc_pwm_ctl = readRegister32(0xC8250);
    uint32_t target_period = rpl_blc_pwm_ctl & 0xFFFF;
    
    if (target_period == 0) {
        target_period = 0x1388; // Safe hardware fallback parameter bounds
    }
    
    // Linearly project requested intensity scalar onto raw hardware resolution limits
    uint32_t calculated_duty_cycle = (percent * target_period) / 100;
    
    // Write duty cycle updates safely into secondary controller window (0xC8254)
    writeRegister32(0xC8254, calculated_duty_cycle);
    
    // Ensure the main PWM controller block stays strictly operational with bit 31 set
    if (!(rpl_blc_pwm_ctl & (1U << 31))) {
        writeRegister32(0xC8250, rpl_blc_pwm_ctl | (1U << 31));
    }
}

void MyIntelGPU::installInterruptHandlers() {
    fInterruptsReady = false;

    // De-assert and mask hardware control channels before committing active values
    writeRegister32(0x4400C, 0xFFFFFFFF); // GTIMR (GT Interrupt Mask Register)
    writeRegister32(0x4420C, 0xFFFFFFFF); // DEIMR (Display Engine Interrupt Mask Register)

    // Calculate baseline safe bit shifts (0 = Enabled, 1 = Masked in Intel Hardware Architecture)
    // For Gen12: Bit 0 = Pipe A Vblank, Bit 11 = South Display Engine Hotplug
    uint32_t target_de_mask = 0xFFFFFFFF;
    target_de_mask &= ~(1U << 0);  // Unmask Vblank Pipe A
    target_de_mask &= ~(1U << 11); // Unmask Hotplug

    writeRegister32(0x4420C, target_de_mask);
    writeRegister32(0x4400C, 0x00000000); // Unmask GT master line for command pipeline

    // Master enable interrupt flag inside Master Control
    writeRegister32(0x44004, (1U << 31)); 

    fInterruptsReady = true;
}

bool MyIntelGPU::registerFramebuffer() {
    if (!fPCIDevice) return false;

    // Check if we have allocated framebuffers to publish
    fFramebufferCount = 1; // Default to single primary screen pipeline
    fFbWidth[0] = 1920;
    fFbHeight[0] = 1080;
    fFbBpp[0] = 32;
    fFbRefresh[0] = 60;

    for (uint32_t i = 0; i < fFramebufferCount; i++) {
        char key[64];
        
        snprintf(key, sizeof(key), "IOFramebuffer%u-width", i);
        fPCIDevice->setProperty(key, reinterpret_cast<const void*>(fFbWidth[i]), 4);
        
        snprintf(key, sizeof(key), "IOFramebuffer%u-height", i);
        fPCIDevice->setProperty(key, reinterpret_cast<const void*>(fFbHeight[i]), 4);
        
        snprintf(key, sizeof(key), "IOFramebuffer%u-bpp", i);
        fPCIDevice->setProperty(key, reinterpret_cast<const void*>(fFbBpp[i]), 4);
        
        snprintf(key, sizeof(key), "IOFramebuffer%u-refresh", i);
        fPCIDevice->setProperty(key, reinterpret_cast<const void*>(fFbRefresh[i]), 4);
    }

    // Call IOKit registerService to announce our graphics framework to the OS hierarchy
    registerService();
    return true;
}
