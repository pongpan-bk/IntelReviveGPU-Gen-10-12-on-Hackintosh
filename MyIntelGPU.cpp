/*
 *  MyIntelGPU.cpp
 *  MyIntelGPU
 *
 *  IOKit kernel extension for Intel integrated GPU (Raptor Lake).
 *  PCI device probing, BAR0/BAR2 memory mapping, GGTT page table.
 *
 *  Build: x86_64, -mkernel, -std=c++11, -fno-exceptions -fno-rtti
 */

#include "MyIntelGPU.hpp"
#include <libkern/libkern.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <stdint.h>

// ── SDK compatibility shims ─────────────────
#ifndef OSMemoryBarrier
#define OSMemoryBarrier()       __sync_synchronize()
#endif
#ifndef kIOMapWriteCombined
#define kIOMapWriteCombined     kIOMapWriteCombineCache
#endif
#ifndef kIOMapInhibitCache
#define kIOMapInhibitCache      (1UL << kIOMapCacheShift)
#endif

#define super IOService
OSDefineMetaClassAndStructors(MyIntelGPU, IOService)

// ─────────────────────────────────────────────
#pragma mark - Lifecycle: init / free
// ─────────────────────────────────────────────
bool MyIntelGPU::init(OSDictionary *dict)
{
    if (!super::init(dict))
        return false;

    fPCIDevice    = nullptr;
    fMMIODesc     = nullptr;
    fMMIOMap      = nullptr;
    fRegs         = nullptr;
    fApertureDesc = nullptr;
    fApertureMap  = nullptr;
    fApertureVA   = nullptr;
    fApertureSize = 0;
    fGsm          = nullptr;
    fGttTotal     = 0;
    fMappableEnd  = 0;
    fDeviceID     = 0;
    fRevision     = 0;
    fGraphicsVer  = 0;

    fInterruptSource = nullptr;
    fRingBuffer      = nullptr;
    fRingTail        = 0;
    fRingHead        = 0;

    return true;
}

void MyIntelGPU::free()
{
    if (fMMIOMap)      { fMMIOMap->release();      fMMIOMap = nullptr; }
    if (fMMIODesc)     { fMMIODesc->release();     fMMIODesc = nullptr; }
    if (fApertureMap)  { fApertureMap->release();   fApertureMap = nullptr; }
    if (fApertureDesc) { fApertureDesc->release();  fApertureDesc = nullptr; }
    super::free();
}

// ─────────────────────────────────────────────
#pragma mark - IOKit Probe
// ─────────────────────────────────────────────
IOService * MyIntelGPU::probe(IOService *provider, SInt32 *score)
{
    // Defer to super first
    IOService *res = super::probe(provider, score);
    if (!res)
        return nullptr;

    IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) {
        IOLog("MyIntelGPU::probe: provider is not IOPCIDevice\n");
        return nullptr;
    }

    uint16_t vendorId = pci->configRead16(kIOPCIConfigVendorID);
    uint16_t deviceId = pci->configRead16(kIOPCIConfigDeviceID);

    if (vendorId != INTEL_VENDOR_ID) {
        IOLog("MyIntelGPU::probe: non-Intel vendor 0x%04x\n", vendorId);
        return nullptr;
    }

    // Raptor Lake iGPU only (S / P / U / H)
    switch (deviceId) {
        case RPL_GT1_DEVICE_ID:
        case RPL_GT2_DEVICE_ID:
        case RPL_GT2_HIGH_DEVICE_ID:
        case RPLU_GT1_DEVICE_ID:
        case RPLU_GT2_DEVICE_ID:
        case RPLU_GT2B_DEVICE_ID:
        case RPLP_GT1_DEVICE_ID:
        case RPLP_GT2_DEVICE_ID:
            break;
        default:
            IOLog("MyIntelGPU::probe: unsupported device 0x%04x\n", deviceId);
            return nullptr;
    }

    IOLog("MyIntelGPU::probe: matched RPL iGPU vendor=0x%04x device=0x%04x\n",
          vendorId, deviceId);

    // Assign high probe score so IOKit prefers this driver
    if (score)
        *score = 5000;

    return this;
}

// ─────────────────────────────────────────────
#pragma mark - IOKit Start
// ─────────────────────────────────────────────
bool MyIntelGPU::start(IOService *provider)
{
    if (!super::start(provider)) {
        IOLog("MyIntelGPU::start: super::start failed\n");
        return false;
    }

    // ── Phase 1: PCI provider ─────────────────
    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IOLog("MyIntelGPU::start: OSDynamicCast failed\n");
        goto fail_super;
    }
    fPCIDevice->retain();

    // Enable bus-master DMA + memory-space decoding
    if (fPCIDevice->setBusMasterEnable(true) != kIOReturnSuccess) {
        IOLog("MyIntelGPU::start: setBusMasterEnable failed\n");
        goto fail_pci;
    }
    if (fPCIDevice->setMemoryEnable(true) != kIOReturnSuccess) {
        IOLog("MyIntelGPU::start: setMemoryEnable failed\n");
        goto fail_pci;
    }

    // ── Phase 2: BAR0 (MMIO) mapping ──────────
    fMMIODesc = fPCIDevice->getDeviceMemoryWithIndex(0);
    if (!fMMIODesc) {
        IOLog("MyIntelGPU::start: BAR0 descriptor not found\n");
        goto fail_pci;
    }

    fMMIOMap = fMMIODesc->createMappingInTask(
        kernel_task,
        kIOMapAnywhere | kIOMapInhibitCache,
        0,
        fMMIODesc->getLength());
    if (!fMMIOMap) {
        IOLog("MyIntelGPU::start: BAR0 createMappingInTask failed\n");
        goto fail_pci;
    }

    fRegs = (volatile uint32_t *)fMMIOMap->getVirtualAddress();
    if (!fRegs) {
        IOLog("MyIntelGPU::start: BAR0 virtual address is NULL\n");
        goto fail_bar0;
    }

    IOLog("MyIntelGPU::BAR0 mapped: phys=0x%llx virt=%p size=0x%llx\n",
          fMMIOMap->getPhysicalAddress(), fRegs, fMMIOMap->getLength());

    // ── Phase 3: BAR2 (Aperture) mapping ──────
    fApertureDesc = fPCIDevice->getDeviceMemoryWithIndex(2);
    if (fApertureDesc) {
        fApertureSize = fApertureDesc->getLength();

        // Try write-combined first, fall back to uncacheable
        IOOptionBits mapOpts = kIOMapAnywhere | kIOMapWriteCombined;
        fApertureMap = fApertureDesc->createMappingInTask(
            kernel_task, mapOpts, 0, fApertureSize);
        if (!fApertureMap) {
            mapOpts = kIOMapAnywhere | kIOMapInhibitCache;
            fApertureMap = fApertureDesc->createMappingInTask(
                kernel_task, mapOpts, 0, fApertureSize);
        }

        if (fApertureMap) {
            fApertureVA = (volatile uint8_t *)fApertureMap->getVirtualAddress();
            IOLog("MyIntelGPU::BAR2 aperture: virt=%p size=0x%llx\n",
                  fApertureVA, fApertureSize);
        } else {
            IOLog("MyIntelGPU::start: BAR2 aperture mapping failed\n");
            fApertureVA   = nullptr;
            fApertureSize = 0;
        }
    } else {
        IOLog("MyIntelGPU::start: BAR2 aperture not present\n");
    }

    // ── Phase 4: Hardware handshake test ──────
    fDeviceID = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
    fRevision = fPCIDevice->configRead8(kIOPCIConfigRevisionID);

    // Read GMD_ID to confirm MMIO is live
    uint32_t gmdId = readReg32(GMD_ID_GRAPHICS);
    if (gmdId == 0xFFFFFFFF || gmdId == 0) {
        IOLog("MyIntelGPU::start: WARNING — GMD_ID=0x%08x (device may be asleep)\n", gmdId);
    } else {
        fGraphicsVer = (gmdId >> 20) & 0x1FF;
        IOLog("MyIntelGPU::start: GMD_ID=0x%08x graphicsVer=%u\n", gmdId, fGraphicsVer);
    }

    // ── Phase 5: GGTT initialisation ──────────
    if (fApertureVA) {
        if (!ggttInit())
            IOLog("MyIntelGPU::start: ggttInit failed — framebuffer will not work\n");
    } else {
        IOLog("MyIntelGPU::start: skipping GGTT init (no aperture)\n");
    }

    // ── Phase 6: Interrupt + Engine Ring (P1) ─
    if (!initInterrupts(provider)) {
        IOLog("MyIntelGPU::start: initInterrupts failed\n");
        goto fail_ring;
    }
    if (!initEngineRing()) {
        IOLog("MyIntelGPU::start: initEngineRing failed\n");
        goto fail_ring;
    }

    // ── Phase 7: Publish service ──────────────
    registerService();

    IOLog("MyIntelGPU::start: OK device=0x%04x rev=0x%02x\n",
          fDeviceID, fRevision);
    return true;

    // ── Error unwind ──────────────────────────
fail_ring:
    if (fInterruptSource) {
        fInterruptSource->disable();
        if (IOWorkLoop *wl = getWorkLoop())
            wl->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = nullptr;
    }
    if (fRingBuffer) {
        IOFree((void *)fRingBuffer, RING_SIZE_BYTES);
        fRingBuffer = nullptr;
    }
    fRingTail = 0;
    fRingHead = 0;
    /* fall through */
fail_bar0:
    if (fMMIOMap)  { fMMIOMap->release();  fMMIOMap = nullptr; }
    if (fMMIODesc) { fMMIODesc->release(); fMMIODesc = nullptr; }
    /* fall through */
fail_pci:
    if (fPCIDevice) { fPCIDevice->release(); fPCIDevice = nullptr; }
    /* fall through */
fail_super:
    fRegs = nullptr;
    super::stop(provider);
    return false;
}

// ─────────────────────────────────────────────
#pragma mark - IOKit Stop
// ─────────────────────────────────────────────
void MyIntelGPU::stop(IOService *provider)
{
    // Reverse order of start — all null-guarded against double-release

    // P1: Interrupt + ring (highest-level, release first)
    if (fInterruptSource) {
        fInterruptSource->disable();
        if (IOWorkLoop *wl = getWorkLoop())
            wl->removeEventSource(fInterruptSource);
        fInterruptSource->release();
        fInterruptSource = nullptr;
    }
    if (fRingBuffer) {
        IOFree((void *)fRingBuffer, RING_SIZE_BYTES);
        fRingBuffer = nullptr;
    }
    fRingTail = 0;
    fRingHead = 0;

    // GGTT / aperture
    if (fGsm)          { fGsm = nullptr; }
    if (fApertureMap)  { fApertureMap->release();  fApertureMap  = nullptr; }
    if (fApertureDesc) { fApertureDesc->release(); fApertureDesc = nullptr; }
    fApertureSize = 0;
    fGttTotal     = 0;
    fMappableEnd  = 0;

    // MMIO (BAR0)
    if (fMMIOMap)  { fMMIOMap->release();  fMMIOMap  = nullptr; }
    if (fMMIODesc) { fMMIODesc->release(); fMMIODesc = nullptr; }
    fRegs = nullptr;

    // PCI provider
    if (fPCIDevice) { fPCIDevice->release(); fPCIDevice = nullptr; }

    super::stop(provider);
}

// ─────────────────────────────────────────────
#pragma mark - MMIO (BAR0) Access
// ─────────────────────────────────────────────
uint32_t MyIntelGPU::readReg32(uint32_t offset)
{
    if (!fRegs) return 0xFFFFFFFF;
    OSSynchronizeIO();
    return fRegs[offset / 4];
}

void MyIntelGPU::writeReg32(uint32_t offset, uint32_t value)
{
    if (!fRegs) return;
    fRegs[offset / 4] = value;
    OSSynchronizeIO();
}

// ─────────────────────────────────────────────
#pragma mark - Aperture (BAR2) Access
// ─────────────────────────────────────────────
uint32_t MyIntelGPU::readAperture32(uint64_t apertureOffset)
{
    if (!fApertureVA || fApertureSize == 0)
        return 0xFFFFFFFF;
    if (apertureOffset >= fApertureSize)
        return 0xFFFFFFFF;

    OSMemoryBarrier();
    uint32_t val = *(volatile uint32_t *)(fApertureVA + apertureOffset);
    OSMemoryBarrier();
    return val;
}

void MyIntelGPU::writeAperture32(uint64_t apertureOffset, uint32_t value)
{
    if (!fApertureVA || fApertureSize == 0)
        return;
    if (apertureOffset >= fApertureSize)
        return;

    *(volatile uint32_t *)(fApertureVA + apertureOffset) = value;
    OSSynchronizeIO();
}

// ─────────────────────────────────────────────
#pragma mark - GGTT Page Table
// ─────────────────────────────────────────────
/*
 *  ggttProbeEntries()
 *
 *  Determine the number of GGTT entries for this platform.
 *  Strategy (in priority order):
 *    1. Read GGTT size from register if available (future)
 *    2. Fall back to platform-specific defaults
 *
 *  On Gen8+ each entry is 8 bytes (64-bit).
 *  The GGTT table occupies (entries * 8) bytes at the start of
 *  the aperture BAR. The remainder is mappable aperture.
 *
 *  Returns: total GGTT entry count, 0 if aperture is unavailable.
 */
uint32_t MyIntelGPU::ggttProbeEntries(void)
{
    if (!fApertureVA || fApertureSize == 0)
        return 0;

    uint32_t entries;

    // For now, derive from device ID
    switch (fDeviceID) {
        case RPL_GT1_DEVICE_ID:
        case RPL_GT2_DEVICE_ID:
        case RPL_GT2_HIGH_DEVICE_ID:
        case RPLU_GT1_DEVICE_ID:
        case RPLU_GT2_DEVICE_ID:
        case RPLU_GT2B_DEVICE_ID:
        case RPLP_GT1_DEVICE_ID:
        case RPLP_GT2_DEVICE_ID:
        default:
            entries = GGTT_ENTRIES_RPL;     // 512K entries = 4 MB GGTT
            break;
    }

    // Sanity: GGTT table must not exceed aperture size
    uint64_t tableSize = (uint64_t)entries * GGTT_ENTRY_SIZE;
    if (tableSize >= fApertureSize) {
        IOLog("MyIntelGPU::ggttProbe: table 0x%llx exceeds aperture 0x%llx, clamping\n",
              tableSize, fApertureSize);
        entries = (uint32_t)(fApertureSize / GGTT_ENTRY_SIZE / 2);
        if (entries < 1024) entries = 1024;
    }

    return entries;
}

/*
 *  ggttInit()
 *
 *  Initialise the GGTT (Global Graphics Translation Table):
 *    1. Probe the number of GGTT entries.
 *    2. Zero all entries to an unmapped state.
 *    3. Flush the GGTT TLB via ggttInvalidate().
 *
 *  Returns: true on success, false on failure.
 */
bool MyIntelGPU::ggttInit(void)
{
    if (!fApertureVA || fApertureSize == 0) {
        IOLog("MyIntelGPU::ggttInit: aperture not mapped\n");
        return false;
    }

    fGttTotal = ggttProbeEntries();
    if (fGttTotal == 0) {
        IOLog("MyIntelGPU::ggttInit: failed to probe entry count\n");
        return false;
    }

    // GGTT table starts at aperture base
    fGsm = (volatile uint32_t *)fApertureVA;

    // Upper bound of mappable aperture (after GGTT table)
    uint64_t tableBytes = (uint64_t)fGttTotal * GGTT_ENTRY_SIZE;
    fMappableEnd = (tableBytes < fApertureSize)
                       ? (uint32_t)(fApertureSize - tableBytes)
                       : 0;

    // Clear every entry (two 32-bit writes per 64-bit entry)
    for (uint32_t i = 0; i < fGttTotal; i++) {
        fGsm[i * 2]     = 0;     // low dword
        fGsm[i * 2 + 1] = 0;     // high dword
    }
    OSMemoryBarrier();

    // Flush GPU TLBs
    ggttInvalidate();

    IOLog("MyIntelGPU::ggttInit: %u entries, table=0x%llx, mappableEnd=%u\n",
          fGttTotal, tableBytes, fMappableEnd);
    return true;
}

/*
 *  ggttInsertEntry(pageIndex, physAddr, cacheBits)
 *
 *  Insert a 64-bit GGTT entry mapping 'pageIndex' to the physical
 *  page at 'physAddr' (4 KB granularity).
 *
 *  Gen8+ GGTT entry format (64-bit):
 *    [ 0]    = VALID (present)
 *    [ 2:1]  = cache control (00=UC, 01=WC, 10=WT, 11=WB)
 *    [10]    = snoop enable
 *    [35:12] = Physical Page Frame Number (physAddr >> 12)
 *
 *  The entry is committed as two 32-bit writes (low then high)
 *  with a memory barrier between.
 */
void MyIntelGPU::ggttInsertEntry(uint32_t pageIndex, uint64_t physAddr,
                                  uint32_t cacheBits)
{
    if (!fGsm || pageIndex >= fGttTotal)
        return;

    // PFN occupies entry bits [35:12]:
    //   Low dword [31:12] = PFN[19:0]
    //   High dword [3:0]  = PFN[23:20]
    uint32_t pfnLow  = (uint32_t)(physAddr >> 12) & 0xFFFFF;
    uint32_t pfnHigh = (uint32_t)(physAddr >> 32) & 0x0F;

    uint32_t entryLo = (pfnLow << 12) | (cacheBits & GGTT_CACHE_MASK) | GGTT_ENTRY_VALID;
    uint32_t entryHi = pfnHigh;

    fGsm[pageIndex * 2]     = entryLo;
    OSMemoryBarrier();
    fGsm[pageIndex * 2 + 1] = entryHi;
    OSMemoryBarrier();
}

/*
 *  ggttRemoveEntry(pageIndex)
 *
 *  Clear a GGTT entry (unmap the page).  Invalidates the GPU TLB
 *  immediately to prevent stale translations.
 */
void MyIntelGPU::ggttRemoveEntry(uint32_t pageIndex)
{
    if (!fGsm || pageIndex >= fGttTotal)
        return;

    fGsm[pageIndex * 2]     = 0;
    OSMemoryBarrier();
    fGsm[pageIndex * 2 + 1] = 0;
    OSMemoryBarrier();

    ggttInvalidate();
}

/*
 *  ggttBindPages(gpuAddr, physAddr, pageCount)
 *
 *  Bind a contiguous range of physical pages into the GGTT at
 *  the given GPU virtual address.  GPU address must be 4 KB aligned.
 *
 *  Skips the TLB flush per-entry; issues a single flush after all
 *  entries are written.
 */
void MyIntelGPU::ggttBindPages(uint64_t gpuAddr, uint64_t physAddr,
                                uint32_t pageCount)
{
    if (!fGsm || pageCount == 0)
        return;

    uint32_t startIndex = (uint32_t)(gpuAddr >> 12);

    for (uint32_t i = 0; i < pageCount; i++) {
        uint64_t pagePhys = physAddr + ((uint64_t)i << 12);
        uint32_t idx      = startIndex + i;

        if (idx >= fGttTotal)
            break;

        uint32_t pfnLow  = (uint32_t)(pagePhys >> 12) & 0xFFFFF;
        uint32_t pfnHigh = (uint32_t)(pagePhys >> 32) & 0x0F;

        uint32_t entryLo = (pfnLow << 12) | GGTT_ENTRY_VALID;
        uint32_t entryHi = pfnHigh;

        fGsm[idx * 2]     = entryLo;
        fGsm[idx * 2 + 1] = entryHi;
    }
    OSMemoryBarrier();

    ggttInvalidate();
}

// ─────────────────────────────────────────────
#pragma mark - TLB / Cache
// ─────────────────────────────────────────────
/*
 *  ggttInvalidate()
 *
 *  Invalidate the GGTT TLB by toggling GFX_FLSH_CNTL_GEN6
 *  (BAR0 offset 0x10100).  A dummy read serialises before
 *  returning.
 */
void MyIntelGPU::ggttInvalidate(void)
{
    if (!fRegs) return;
    writeReg32(GFX_FLSH_CNTL_GEN6, 0);
    readReg32(GFX_FLSH_CNTL_GEN6);      // posting flush
}

/*
 *  clflushRange(addr, size)
 *
 *  Flush CPU cache lines covering [addr, addr+size) via CLFLUSH.
 *  64-byte aligned start; issues CPU barrier before and after.
 */
void MyIntelGPU::clflushRange(const void *addr, size_t size)
{
    if (!addr || size == 0) return;

    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)63;
    uintptr_t end   = (uintptr_t)addr + size;

    OSMemoryBarrier();
    for (uintptr_t line = start; line < end; line += 64)
        __asm__ volatile("clflush %0" : "+m" (*(volatile char *)line));
    OSMemoryBarrier();
}

// ====================================================================
#pragma mark - P1: MSI Interrupt + Engine Ring
// ====================================================================

bool MyIntelGPU::initInterrupts(IOService *provider)
{
    if (!fPCIDevice)
        return false;

    IOLog("MyIntelGPU::[MSI] Registering interrupt handler...\n");

    fInterruptSource = IOInterruptEventSource::interruptEventSource(
        this,
        &MyIntelGPU::interruptHandler,
        provider,
        0);     // interrupt index 0 (MSI primary)

    if (!fInterruptSource) {
        IOLog("MyIntelGPU::[MSI] Failed to allocate interrupt event source\n");
        return false;
    }

    IOWorkLoop *wl = getWorkLoop();
    if (!wl || wl->addEventSource(fInterruptSource) != kIOReturnSuccess) {
        IOLog("MyIntelGPU::[MSI] Failed to add event source to work loop\n");
        fInterruptSource->release();
        fInterruptSource = nullptr;
        return false;
    }

    fInterruptSource->enable();
    IOLog("MyIntelGPU::[MSI] Interrupt handler active\n");
    return true;
}

void MyIntelGPU::interruptHandler(OSObject *owner,
                                   IOInterruptEventSource *src,
                                   int count)
{
    MyIntelGPU *self = OSDynamicCast(MyIntelGPU, owner);
    if (!self || !self->fRegs)
        return;

    uint32_t irqStatus = self->readReg32(GT_INTR_IDENTITY_REG);
    self->writeReg32(GT_INTR_IDENTITY_REG, irqStatus);  // clear

    IOLog("MyIntelGPU::[IRQ] count=%d status=0x%08x\n", count, irqStatus);
}

bool MyIntelGPU::initEngineRing(void)
{
    IOLog("MyIntelGPU::[RING] Initialising Execlist engine ring...\n");

    fRingBuffer = (volatile uint32_t *)IOMalloc(RING_SIZE_BYTES);
    if (!fRingBuffer) {
        IOLog("MyIntelGPU::[RING] IOMalloc failed\n");
        return false;
    }
    bzero((void *)fRingBuffer, RING_SIZE_BYTES);
    fRingTail = 0;
    fRingHead = 0;

    // Get physical address of ring buffer via IOMemoryDescriptor
    // (kvtophys/addr64_t are XNU internals not in MacKernelSDK)
    IOMemoryDescriptor *ringMD = IOMemoryDescriptor::withAddress(
        (void *)fRingBuffer, RING_SIZE_BYTES, kIODirectionNone);
    if (!ringMD) {
        IOLog("MyIntelGPU::[RING] withAddress failed\n");
        IOFree((void *)fRingBuffer, RING_SIZE_BYTES);
        fRingBuffer = nullptr;
        return false;
    }
    IOByteCount segLen;
    uint64_t ringPhysAddr = ringMD->getPhysicalSegment64(0, &segLen);
    ringMD->release();

    writeReg32(RING_START_REG, (uint32_t)ringPhysAddr);
    writeReg32(RING_HEAD_REG,  0);
    writeReg32(RING_TAIL_REG,  0);

    // enable ring + size encoding (bit 0 = enable)
    // On Gen8+ the ring size is encoded in bits [1:3]
    uint32_t ringCtrl = (RING_SIZE_BYTES - 4096) | 1;
    writeReg32(RING_CTRL_REG, ringCtrl);

    IOLog("MyIntelGPU::[RING] Engine ring enabled phys=0x%llx\n",
          ringPhysAddr);
    return true;
}

void MyIntelGPU::submitCommand(uint32_t command)
{
    if (!fRingBuffer || !fRegs)
        return;

    fRingBuffer[fRingTail / 4] = command;
    fRingTail = (fRingTail + 4) % RING_SIZE_BYTES;

    OSMemoryBarrier();
    writeReg32(RING_TAIL_REG, fRingTail);
}
