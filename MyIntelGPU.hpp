#ifndef __MyIntelGPU_HPP__
#define __MyIntelGPU_HPP__

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOInterruptEventSource.h>

// ── Intel PCI Vendor ──────────────
#define INTEL_VENDOR_ID         0x8086

// ── Raptor Lake iGPU Device IDs ───
//
// Architecture breakdown:
//   RPL-S  (Desktop)      0xA78x — GT1=0xA780, GT2=0xA78A
//   RPL-HX (Desktop-mobile) 0xA78D/E
//   RPL-P  (28W mobile)   0xA7A0-3 — GT1=0xA7A0, GT2=0xA7A1
//   RPL-U  (15W mobile)   0xA7A8-B — GT2(80EU)=0xA7A9 ← Core 5 120U
//   RPL-H  (45W mobile)   0xA7AC
//
#define RPL_GT1_DEVICE_ID       0xA780   // RPL-S desktop GT1 (UHD 770)
#define RPL_GT2_DEVICE_ID       0xA78A   // RPL-S desktop GT2
#define RPL_GT2_HIGH_DEVICE_ID  0xA7AC   // RPL-H mobile GT2
#define RPLU_GT1_DEVICE_ID      0xA7A8   // RPL-U 15W GT1
#define RPLU_GT2_DEVICE_ID      0xA7A9   // RPL-U 15W GT2 (Iris Xe 80EU)
#define RPLU_GT2B_DEVICE_ID     0xA7AA   // RPL-U 15W GT2 alt
#define RPLP_GT1_DEVICE_ID      0xA7A0   // RPL-P 28W GT1
#define RPLP_GT2_DEVICE_ID      0xA7A1   // RPL-P 28W GT2 (Iris Xe 96EU)

// ── GGTT Entry Format (Gen8+) ─────
#define GGTT_ENTRY_SIZE         8           // 64-bit entries per page-table slot
#define GGTT_ENTRY_VALID        (1 << 0)

// Cache-control : entry bits [2:1]
#define GGTT_CACHE_MASK         0x06
#define GGTT_CACHE_UC           0x00        // uncacheable
#define GGTT_CACHE_WC           0x02        // write-combining
#define GGTT_CACHE_WT           0x04
#define GGTT_CACHE_WB           0x06

#define GGTT_ENTRY_SNOOP        (1 << 10)   // snoop enable

// Default GGTT sizes by platform
#define GGTT_ENTRIES_RPL        0x80000     // 512K entries = 4 MB table
#define GGTT_ENTRIES_ADL        0x80000
#define GGTT_ENTRIES_CFL        0x20000     // 128K entries = 512 KB table

// ── MMIO Register Offsets ─────────
#define GMD_ID_GRAPHICS         0xD8C
#define GFX_FLSH_CNTL_GEN6      0x10100
#define RCS0_BASE               0x02000

// ── Ring Buffer (Execlist Legacy) ─
#define RING_SIZE_BYTES         4096    // 4 KB ring buffer
#define RING_START_REG          0x2030  // ring base address (RCS + 0x30)
#define RING_TAIL_REG           0x2034  // ring tail pointer
#define RING_HEAD_REG           0x2038  // ring head pointer
#define RING_CTRL_REG           0x203C  // ring control

#define GT_INTR_IDENTITY_REG    0x20A0  // GT interrupt identity

// ──────────────────────────────────
// MyIntelGPU — IOKit Intel iGPU driver
// ──────────────────────────────────
class MyIntelGPU : public IOService {
    OSDeclareDefaultStructors(MyIntelGPU)

public:
    // Lifecycle
    virtual bool init(OSDictionary *dict) override;
    virtual IOService * probe(IOService *provider, SInt32 *score) override;
    virtual bool start(IOService *provider) override;
    virtual void stop(IOService *provider) override;
    virtual void free() override;

    // MMIO (BAR0) register access
    virtual uint32_t readReg32(uint32_t offset);
    virtual void writeReg32(uint32_t offset, uint32_t value);

    // Aperture (BAR2) access — used by GGTT/framebuffer
    virtual uint32_t readAperture32(uint64_t apertureOffset);
    virtual void writeAperture32(uint64_t apertureOffset, uint32_t value);

    // GGTT page table management
    virtual void ggttInvalidate(void);
    virtual bool ggttInit(void);
    virtual uint32_t ggttProbeEntries(void);
    virtual void ggttInsertEntry(uint32_t pageIndex, uint64_t physAddr, uint32_t cacheBits);
    virtual void ggttRemoveEntry(uint32_t pageIndex);
    virtual void ggttBindPages(uint64_t gpuAddr, uint64_t physAddr, uint32_t pageCount);
    virtual void clflushRange(const void *addr, size_t size);

    // ── P1: Interrupt + Engine Ring ──────────
    virtual bool initInterrupts(IOService *provider);
    virtual bool initEngineRing(void);
    virtual void submitCommand(uint32_t command);

private:
    static void interruptHandler(OSObject *owner,
                                  IOInterruptEventSource *src,
                                  int count);
    // PCI / MMIO
    IOPCIDevice        *fPCIDevice;
    IOMemoryDescriptor *fMMIODesc;
    IOMemoryMap        *fMMIOMap;
    volatile uint32_t  *fRegs;           // BAR0 virtual base (32-bit MMIO view)

    // Aperture (BAR2)
    IOMemoryDescriptor *fApertureDesc;
    IOMemoryMap        *fApertureMap;
    volatile uint8_t   *fApertureVA;     // BAR2 virtual base (byte-level)
    uint64_t            fApertureSize;

    // GGTT
    volatile uint32_t  *fGsm;            // GGTT page-table base within aperture
    uint32_t            fGttTotal;       // total GGTT entries
    uint32_t            fMappableEnd;    // end offset of mappable aperture window

    // Device info
    uint32_t            fDeviceID;
    uint32_t            fRevision;
    uint32_t            fGraphicsVer;

    // P1: Interrupt + Engine Ring
    IOInterruptEventSource *fInterruptSource;
    volatile uint32_t      *fRingBuffer;
    uint32_t                fRingTail;
    uint32_t                fRingHead;
};

#endif /* __MyIntelGPU_HPP__ */
