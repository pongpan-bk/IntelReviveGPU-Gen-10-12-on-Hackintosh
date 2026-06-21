/*===========================================================================
 *  MyIntelGPU.hpp
 *  Hackintosh Kext — FakeID Alder Lake → Coffee Lake
 *
 *  โครงสร้างคลาสหลักของ Intel Graphics Driver ฝั่ง macOS (IOKit C++)
 *  รับหน้าที่:
 *    - เปิด PCI BAR0 (MMIO Registers) + BAR2 (Aperture/GMADR)
 *    - Dynamic Register Offset Translation: สลับ MMIO base ของ engine
 *      และ cursor register ให้ macOS (ที่คิดว่าเป็น Coffee Lake)
 *      ส่งคำสั่งไปยังตำแหน่งจริงของ Alder Lake
 *    - จัดการ CPU memory barrier + cache flush สำหรับ GPU coherency
 *
 *  อ้างอิงโครงสร้างจาก Linux i915:
 *    drivers/gpu/drm/i915/i915_pci.c        — PCI ID table + per-gen info
 *    drivers/gpu/drm/i915/intel_device_info.c — GMD_ID runtime detection
 *    drivers/gpu/drm/i915/gt/intel_engine_cs.c — engine MMIO base per gen
 *    drivers/gpu/drm/i915/i915_drv.c        — i915_driver_mmio_probe / hw_probe
 *///=========================================================================

#ifndef __MY_INTEL_GPU_HPP__
#define __MY_INTEL_GPU_HPP__

#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLib.h>

#pragma mark - Constants & Register Offsets

/*
 * ─────────────────────────────────────────────
 *  PCI Device IDs — ใช้แยกเจนของฮาร์ดแวร์จริง
 * ─────────────────────────────────────────────
 *  Coffee Lake (เป้าหมายปลอมที่ macOS จะคิดว่าเป็น)
 *    Device IDs: 0x3E91 (U14 GT2), 0x3E92 (U15 GT2),
 *                0x3E9B (U23 GT2), 0x9BC4 (S GT3)
 *
 *  Alder Lake (ฮาร์ดแวร์จริงที่เราต้องส่งคำสั่งไป)
 *    Device IDs: 0x4680 (ADL-S GT1), 0x468B (ADL-P GT1),
 *                0x4691 (ADL-P GT2), 0x4692 (ADL-S GT2)
 * ─────────────────────────────────────────────
 */

#define ADL_P_GT2_DEVICE_ID    0x4691  /* Alder Lake-P GT2 (จริง) */
#define CFL_GT2_DEVICE_ID      0x3E92  /* Coffee Lake-U15 GT2 (ปลอม) */

/*
 * ช่วง MMIO Register Address สำหรับ Engine Base
 * ──────────────────────────────────────────
 *  ฝั่ง Coffee Lake (GEN9)           ฝั่ง Alder Lake (GEN12+)
 *    RCS0 = 0x02000                    RCS0 = 0x02000 (ตรงกัน)
 *    VCS0 = 0x12000                    VCS0 = 0x1c0000 ← ต่าง!
 *    BCS0 = 0x22000                    BCS0 = 0x22000 (ตรงกัน)
 *    VECS0 = 0x1a000                   VECS0 = 0x1c8000 ← ต่าง!
 *    VCS1 = N/A (ไม่มี)                VCS1 = 0x1c4000
 *    VCS2 = N/A                       VCS2 = 0x1d0000
 *    CCS0 = N/A                       CCS0 = 0x1a000
 *
 *  อ้างอิง: intel_engine_cs.c __engine_mmio_base()
 *           i915_pci.c  ENGINE_*_MMIO_BASE macros
 * ──────────────────────────────────────────
 */
#define RCS0_BASE_REAL      0x02000    /* Render — เหมือนกันทุกเจน */
#define RCS0_BASE_FAKE      0x02000

#define VCS0_BASE_REAL      0x1C0000   /* Video Decode Gen12+ (จริง) */
#define VCS0_BASE_FAKE      0x12000    /* Coffee Lake (ปลอม) */

#define BCS0_BASE_REAL      0x22000    /* Blitter — เหมือนกัน */
#define BCS0_BASE_FAKE      0x22000

#define VECS0_BASE_REAL     0x1C8000   /* Video Encode Gen12+ (จริง) */
#define VECS0_BASE_FAKE     0x1A000    /* Coffee Lake (ปลอม) */

/*
 * Cursor Register Base — ต่างกันระหว่างเจน
 * ──────────────────────────────────────────
 *  Coffee Lake:                       Alder Lake:
 *    Pipe A CURBASE = 0x70080           Pipe A CURBASE = 0x70080 (ตรงกัน)
 *    Pipe B CURBASE = 0x700c0           Pipe B CURBASE = 0x71080 ← ต่าง!
 *    Pipe C CURBASE = 0x72080           Pipe C CURBASE = 0x72080 (ตรงกัน)
 *                                        Pipe D CURBASE = 0x73080 (มีเพิ่ม)
 * ──────────────────────────────────────────
 */
#define CURSOR_A_FAKE       0x70080
#define CURSOR_A_REAL       0x70080

#define CURSOR_B_FAKE       0x700C0
#define CURSOR_B_REAL       0x71080    /* ต่าง! */

#define CURSOR_C_FAKE       0x72080
#define CURSOR_C_REAL       0x72080

#define CURSOR_D_FAKE       0x73080    /* ไม่มีใน Coffee Lake */
#define CURSOR_D_REAL       0x73080

/*
 * Register Offsets อื่น ๆ ที่สำคัญ
 */
#define GMD_ID_GRAPHICS     0xD8C      /* Graphics Media Device ID register
                                          (มีเฉพาะ Gen12+ / Meteor Lake)
                                          read32(0xD8C) → ver/release */
#define GMD_ID_DISPLAY      0x510A0    /* Display IP version (MTL+)
                                          อ่านเพื่อ verify display gen */

#define GEN11_GT_INTR_DW0   0x44074    /* GT Interrupt DW0 (shared) */
#define ENGINE_TAIL_REG      0x80      /* Ring Tail Register offset
                                          (สัมพัทธ์จาก engine base) */
#define ENGINE_HEAD_REG      0x34      /* Ring Head Register offset */
#define ENGINE_CTL_REG       0x3C      /* Ring Control Register offset */
#define ENGINE_START_REG     0x38      /* Ring Start (Base Address) */

#define GFX_FLSH_CNTL_GEN6  0x10100   /* GGTT TLB Invalidate Register
                                          = write 1 → flush */

/*
 * ขนาดของ Translation Window สำหรับแต่ละ Engine
 *  (ใช้ mask สำหรับตรวจสอบว่าอยู่ในช่วงที่ต้องแปลหรือไม่)
 */
#define ENGINE_WINDOW_SIZE  0x1000     /* Engine register block = 4KB */

#pragma mark - Translation Entry Structure

/*
 * RegisterTranslationEntry
 *
 * โครงสร้างสำหรับ Dynamic Offset Translation:
 *   macOS อ่าน/เขียน register address A (ในมุมมอง Coffee Lake)
 *   → translateTable[] จะแปลงเป็น offset จริงของ Alder Lake
 *
 * วิธีใช้:
 *   1. วนลูปเทียบ (address == fakeBase ถึง fakeBase + size)
 *   2. ถ้าตรง → return addr - fakeBase + realBase
 *   3. ไม่ตรง → return addr เดิม (pass-through)
 */
typedef struct {
    const char *name;       /* ชื่อ engine/component (debug) */
    uint32_t    fakeBase;   /* MMIO base ที่ macOS ควรเห็น (Coffee Lake) */
    uint32_t    realBase;   /* MMIO base จริงของฮาร์ดแวร์ (Alder Lake) */
    uint32_t    windowSize; /* ขนาดช่วง register (ปกติ 0x1000) */
    bool        enabled;    /* เปิด/ปิด การแปลพิกัด */
} RegisterTranslationEntry;

#pragma mark - MyIntelGPU Class

/*
 * MyIntelGPU : IOService
 *
 * คลาสหลักของ Kext — ทำหน้าที่:
 *   1. Probe/Start → จับ配对 IOPCIDevice
 *   2. PCI Config → setBusMaster, map BAR0 + BAR2
 *   3. MMIO Register Access → อ่าน/เขียนด้วยไส้ Dynamic Translation
 *   4. GGTT Management → aperture + page table
 *   5. Memory Barrier → OSSynchronizeIO + CLFLUSH
 */
class MyIntelGPU : public IOService {
    OSDeclareDefaultStructors(MyIntelGPU)

public:

    /*
     * ─────────────────────────────────────
     *  IOKit Lifecycle Methods
     * ─────────────────────────────────────
     */

    /*!
     * @brief  ตรวจสอบว่า provider (IOPCIDevice) ตรงกับฮาร์ดแวร์ที่รองรับ
     *
     *  ทำงานคล้าย i915_pci_probe():
     *    - เปิด PCI device, enable memory space
     *    - อ่าน device ID เพื่อจำแนกเจน
     *    - สร้าง translation table เตรียมไว้
     *
     * @param provider  IOPCIDevice ที่ IOService จับคู่ให้
     * @return true ถ้ารับ device นี้, false ปฏิเสธ
     */
    virtual bool init(OSDictionary *dict) override;

    /*!
     * @brief  เริ่มต้นการทำงานหลักของ Kext
     *
     *  Pipeline (เทียบ i915_driver_mmio_probe → hw_probe):
     *    1. super::start()
     *    2. จับ IOPCIDevice pointer
     *    3. PCI enable + bus master
     *    4. mapDeviceMemoryWithIndex(0) → BAR0 (MMIO registers)
     *    5. mapDeviceMemoryWithIndex(2) → BAR2 (Aperture/GMADR)
     *    6. Detect generation → ตั้ง translation table
     *    7. GGTT init พื้นฐาน
     *    8. registerService()
     *
     * @param provider  IOPCIDevice ที่ IOService ส่งให้
     * @return true = success, false = fail
     */
    virtual bool start(IOService *provider) override;

    /*!
     * @brief  หยุด Kext (unload / sleep)
     *
     *  Cleanup MMIO mapping, free memory
     */
    virtual void stop(IOService *provider) override;

    /*!
     * @brief  Free ทรัพยากรทั้งหมด
     */
    virtual void free() override;

    /*
     * ─────────────────────────────────────
     *  MMIO Register Access
     * ─────────────────────────────────────
     */

    /*!
     * @brief  อ่าน 32-bit register จาก MMIO BAR0
     *
     *  ขั้นตอน:
     *    1. translateAddress(offset) → แปลง fake offset เป็น real
     *    2. *reinterpret_cast<volatile uint32_t*>(fRegs + realOffset)
     *    3. OSSynchronizeIO() → memory barrier
     *
     * @param offset  MMIO register offset (ในมุมมอง Coffee Lake)
     * @return ค่า register 32-bit
     */
    virtual uint32_t readReg32(uint32_t offset);

    /*!
     * @brief  เขียน 32-bit register ไปยัง MMIO BAR0
     *
     *  ขั้นตอน:
     *    1. translateAddress(offset) → real offset
     *    2. volatile store
     *    3. OSSynchronizeIO() → write buffer drain
     *
     * @param offset  MMIO register offset (ในมุมมอง Coffee Lake)
     * @param value   ค่าที่จะเขียน
     */
    virtual void writeReg32(uint32_t offset, uint32_t value);

    /*
     * ─────────────────────────────────────
     *  Aperture Access (BAR2 / GMADR)
     * ─────────────────────────────────────
     */

    /*!
     * @brief  อ่าน 32-bit จาก Aperture (BAR2) ที่ตำแหน่งสัมพัทธ์
     *
     *  Aperture = CPU-mappable window to GGTT.
     *  ใช้สำหรับ CPU access buffer objects ที่ผูกกับ GGTT.
     *
     * @param apertureOffset  offset ใน aperture window
     * @return 32-bit value
     */
    virtual uint32_t readAperture32(uint64_t apertureOffset);

    /*!
     * @brief  เขียน 32-bit ไปยัง Aperture (BAR2) แบบ Write-Combined
     *
     *  สำหรับ CPU → GPU data transfer (staging buffer, cursors, etc.)
     *  ต้อง flush WC buffer หลังเขียนเสมอ:
     *    - IODelayWriteCombined(uint32_t) หรือ
     *    - OSSynchronizeIO() (heavy-weight)
     *
     * @param apertureOffset  offset ใน aperture window
     * @param value           ค่าที่เขียน
     */
    virtual void writeAperture32(uint64_t apertureOffset, uint32_t value);

    /*
     * ─────────────────────────────────────
     *  GGTT / Cache Management
     * ─────────────────────────────────────
     */

    /*!
     * @brief  GGTT TLB Invalidate
     *
     *  เขียน 1 ไปที่ GFX_FLSH_CNTL_GEN6 (0x10100)
     *  หรือใช้ register ตามเจน:
     *    - Gen8+: write 1 to GFX_FLSH_CNTL_GEN8 = 0x10100
     *    - Gen6-7: PGTBL_ER 0x2024
     *
     *  เทียบ Linux: ggtt->invalidate()
     *               intel_uncore_write(uncore, GFX_FLSH_CNTL_GEN6, 0)
     */
    virtual void ggttInvalidate(void);

    /*!
     * @brief  CPU Cache Line Flush — สำหรับ non-coherent platforms
     *
     *  เทียบ Linux drm_clflush_virt_range():
     *    - ใช้ CLFLUSH instruction บน x86_64
     *    - วน flush ทุก cache line (aligned)
     *    - ต้องใช้ compiler barrier (OSMemoryBarrier) ก่อน/หลัง
     *
     *  ใช้เมื่อเปิด cache (WB) แล้วต้องการให้ GPU เห็นค่าล่าสุด
     *  (แต่บน iGPU Gen9+ ที่ has_llc==1 ไม่จำเป็นมากนัก;
     *   จำเป็นสำหรับ discrete หรือ Gen12+ DG2/MTL ที่ has_llc==0)
     *
     * @param addr  virtual address ที่ต้องการ flush
     * @param size  ขนาดข้อมูล (bytes)
     */
    virtual void clflushRange(const void *addr, size_t size);

    /*
     * ─────────────────────────────────────
     *  FakeID Helpers
     * ─────────────────────────────────────
     */

    /*!
     * @brief  ตรวจจับเจนของฮาร์ดแวร์จริงจาก PCI Device ID
     *
     *  Flow (เทียบ intel_device_info_runtime_init_early()):
     *    1. อ่าน PCI config space → device ID
     *    2. จับคู่กับตารางรู้จัก (ADL-S/P, CFL, TGL, ICL, SKL)
     *    3. ถ้าเป็น ADL → fFakeGen = 9 (Coffee Lake)
     *    4. ถ้าเป็น CFL → fFakeGen = 9 (Native, ไม่ต้องเปลี่ยน)
     *    5. ถ้าไม่รู้จัก → fFakeGen = 0 (English mode)
     *
     *  นอกจากนี้ อ่าน GMD_ID register (0xD8C) ถ้าฮาร์ดแวร์รองรับ
     *  ใช้สำหรับยืนยัน IP version
     *
     * @return จริง (device ID + revision) หรือ 0
     */
    virtual uint32_t detectHardwareGeneration(void);

    /*!
     * @brief  สร้างตาราง Translation Entry สำหรับ Dynamic Offset
     *
     *  ตอนสร้าง:
     *    - ใส่ entry สำหรับแต่ละ engine ที่ offset ต่างกัน
     *    - ใส่ entry สำหรับ cursor register
     *    - เรียง fakeBase จากน้อยไปมาก
     *
     *  ถูกเรียกจาก start() หลังจาก detectHardwareGeneration()
     */
    virtual void buildTranslationTable(void);

    /*!
     * @brief  แปลง MMIO offset ตามตาราง Translation
     *
     *  รับ offset ที่ macOS ส่งมา (Coffee Lake expected)
     *  แล้วเปลี่ยนเป็น offset จริงของ Alder Lake
     *
     *  3 กรณี:
     *    1. เทียบ offset กับ fakeBase ของแต่ละ entry
     *    2. ถ้าตรง → return realBase + (offset - fakeBase)
     *    3. ไม่ตรง → return offset (pass-through)
     *
     * @param fakeOffset  offset ในมุมมอง Coffee Lake
     * @return offset จริงที่ฮาร์ดแวร์คาดหวัง
     */
    virtual uint32_t translateAddress(uint32_t fakeOffset);

private:

    /*
     * ─────────────────────────────────────
     *  Member Variables
     * ─────────────────────────────────────
     */

    /* PCI Device */
    IOPCIDevice             *fPCIDevice;       /*!< IOPCIDevice ที่จับคู่ (provider) */

    /* MMIO (BAR0) */
    IOMemoryMap             *fMMIOMap;         /*!< IOMemoryMap ของ BAR0 (64-bit prefetchable) */
    volatile uint8_t        *fRegs;            /*!< Virtual address ของ MMIO registers */

    /* Aperture (BAR2 / GMADR) */
    IOMemoryMap             *fApertureMap;     /*!< IOMemoryMap ของ BAR2 (aperture/GMADR) */
    volatile uint8_t        *fApertureVA;      /*!< Virtual address ของ aperture window */
    uint64_t                 fApertureSize;    /*!< ขนาด aperture (ปกติ 256MB) */

    /* Hardware Info */
    uint32_t                 fDeviceID;        /*!< PCI Device ID จริง (จากฮาร์ดแวร์) */
    uint32_t                 fRevision;        /*!< PCI Revision ID */
    uint32_t                 fGraphicsVer;     /*!< Graphics IP version (GRAPHICS_VER)
                                                    = 12 สำหรับ ADL, 9 สำหรับ CFL */
    uint8_t                  fFakeGen;         /*!< Fake Generation ID:
                                                    9  = แกล้งเป็น Coffee Lake
                                                    12 = ส่งตรง Alder Lake
                                                    0  = ไม่รู้จัก, pass-through */
    bool                     fUseGmdId;        /*!< true ถ้าฮาร์ดแวร์รองรับ GMD_ID register
                                                    (Meteor Lake+) */

    /* GGTT Info */
    uint32_t                 fGttTotal;        /*!< GGTT total entries (pages) */
    uint32_t                 fMappableEnd;     /*!< จุดสิ้นสุดของ mappable aperture */
    volatile uint32_t       *fGsm;             /*!< GTT Stolen Memory (page table array) */

    /* Translation Table */
    RegisterTranslationEntry fTransTable[8];   /*!< ตารางแปลพิกัด register offset */
    int                      fTransCount;      /*!< จำนวน entry ในตาราง */

    /* Memory Map References — เก็บไว้เพื่อ release ใน stop() */
    IOMemoryDescriptor      *fMMIODesc;        /*!< BAR0 IOMemoryDescriptor */
    IOMemoryDescriptor      *fApertureDesc;    /*!< BAR2 IOMemoryDescriptor */
};

#endif /* __MY_INTEL_GPU_HPP__ */
