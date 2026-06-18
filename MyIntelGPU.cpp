/*===========================================================================
 *  MyIntelGPU.cpp
 *  Hackintosh Kext — FakeID Alder Lake → Coffee Lake
 *
 *  Implementation ของคลาส MyIntelGPU
 *
 *  แกนหลัก:
 *    - PCI Initialization (i915_pci_probe counterpart)
 *    - MMIO BAR mapping (i915_driver_mmio_probe counterpart)
 *    - Dynamic Register Translation — แก้ทาง macOS
 *      ให้ส่ง register I/O ไปตำแหน่ง real hardware
 *    - GGTT TLB flush + CPU cache coherency
 *
 *  Reference:
 *    Linux i915:
 *      i915_driver.c       → i915_driver_mmio_probe, i915_driver_hw_probe
 *      i915_pci.c          → i915_pci_probe, pciidlist
 *      intel_uncore.c      → intel_uncore_read, intel_uncore_write
 *      i915_gem.c          → GGTT, aperture, GEM object lifecycle
 *      intel_engine_cs.c   → engine MMIO base definition
 *
 *    macOS IOKit:
 *      IODeviceMemory, IOMemoryMap
 *      IOPCIDevice API
 *      OSSynchronizeIO / OSMemoryBarrier / IODelayWriteCombined
 *///=========================================================================

#include "MyIntelGPU.hpp"
#include <libkern/libkern.h>
#include <libkern/OSAtomic.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <stdint.h>

#ifndef kIOPCIMemorySpace64Bit
#define kIOPCIMemorySpace64Bit 0x1000000000000000ULL
#endif

/*
 * Macro สำหรับลงทะเบียนคลาสกับ IOKit runtime
 * IOService ทุกตัวต้องมี OSDefineMetaClassAndStructors หนึ่งครั้ง
 */
#define super IOService
OSDefineMetaClassAndStructors(MyIntelGPU, IOService)

/*
 * IODebug — ปริ้น debug output ทาง kernel log
 *
 * ใช้ IOLog จริง; ถ้าอยากเงียบก็ comment บรรทัดนี้ออก
 * หรือใช้ _EXTRA_DEBUG เพื่อให้ verbose ขึ้นเฉพาะตอน dev
 */
#define IODebug(fmt, ...) \
    IOLog("MyIntelGPU: [%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

// #define EXTRA_DEBUG  /* เปิดถ้าต้องการ log การ translate address รายครั้ง */

/* จำนวนครั้งที่ retry ถ้ากาก MMIO */
#define BAR_RETRY_COUNT     3

#pragma mark -
#pragma mark - init / free

/*
 * ─────────────────────────────────────────────────────────────────
 *  bool MyIntelGPU::init(OSDictionary *dict)
 *
 *  เรียกเมื่อ IOService เริ่มสร้าง instance:
 *    - เรียก super::init() ก่อน
 *    - กำหนดค่าเริ่มต้นของ member variables
 *    - จองทรัพยากรพื้นฐาน
 *
 *  เทียบ Linux: i915_driver_create() → devm_drm_dev_alloc()
 *               (แต่ Linux จอง struct + drm_device ผ่าน allocator)
 *               macOS ใช้ C++ constructor pattern แทน
 * ─────────────────────────────────────────────────────────────────
 */
bool MyIntelGPU::init(OSDictionary *dict)
{
    /*
     * ขั้นแรก: เรียก init ของ IOService ก่อนเสมอ
     * ถ้า super ไม่ผ่าน = ไม่ต้องทำอะไรต่อ
     */
    if (!super::init(dict)) {
        return false;
    }

    /*
     * ตั้งค่าเริ่มต้นของตัวแปรทั้งหมดให้เป็น 0/NULL
     * เพื่อป้องกันการใช้งาน pointer ที่ยังไม่ถูก map
     */
    fPCIDevice    = NULL;
    fMMIOMap      = NULL;
    fRegs         = NULL;
    fApertureMap  = NULL;
    fApertureVA   = NULL;
    fApertureSize = 0;
    fDeviceID     = 0;
    fRevision     = 0;
    fGraphicsVer  = 0;
    fFakeGen      = 0;
    fUseGmdId     = false;
    fGttTotal     = 0;
    fMappableEnd  = 0;
    fGsm          = NULL;
    fTransCount   = 0;
    fMMIODesc     = NULL;
    fApertureDesc = NULL;

    /*
     * clear translation table ทั้งหมด
     * แต่ละ entry มี 4 fields + name pointer = ปลอดภัยตอน init
     */
    memset(fTransTable, 0, sizeof(fTransTable));

    IODebug("init() — OK");
    return true;
}

/*
 * ─────────────────────────────────────────────────────────────────
 *  void MyIntelGPU::free()
 *
 *  เรียกก่อน object จะถูกทำลาย:
 *    - ปล่อย resources ที่ยังค้าง
 *    - IOKit จะเรียก stop() ก่อน free() เสมอ
 *    - แต่ถ้า init() แล้ว start() ไม่ผ่าน ก็ต้องแน่ใจว่า
 *      resources ถูกปล่อยแล้ว (defensive coding)
 * ─────────────────────────────────────────────────────────────────
 */
void MyIntelGPU::free()
{
    IODebug("free()");

    /*
     * ถ้า MMIO mapping ยังไม่ถูกปล่อย → ปล่อยเดี๋ยวนี้
     * (ควรถูกปล่อยใน stop() แล้ว แต่กันลืม)
     */
    if (fMMIOMap) {
        fMMIOMap->release();
        fMMIOMap = NULL;
    }
    if (fMMIODesc) {
        fMMIODesc->release();
        fMMIODesc = NULL;
    }
    if (fApertureMap) {
        fApertureMap->release();
        fApertureMap = NULL;
    }
    if (fApertureDesc) {
        fApertureDesc->release();
        fApertureDesc = NULL;
    }

    super::free();
}

#pragma mark -
#pragma mark - detectHardwareGeneration

/*
 * ─────────────────────────────────────────────────────────────────
 *  uint32_t MyIntelGPU::detectHardwareGeneration()
 *
 *  ตรวจจับเจนของฮาร์ดแวร์จริง + ตั้งค่า fFakeGen
 *
 *  อ่าน PCI Device ID → map เป็นคู่เทียบ:
 *    ADL-S (0x468x)  → Gen12 (real), FakeGen=9 (Coffee Lake)
 *    ADL-P (0x469x)  → Gen12 (real), FakeGen=9
 *    RPL-P (0xA7Ax)  → Gen12 (real), FakeGen=9 (same IP as ADL)
 *    CFL  (0x3E9x)   → Gen9  (real), FakeGen=9 (native, no translation)
 *    TGL  (0x9A60)   → Gen12 (real), FakeGen=9
 *    Unknown          → FakeGen=0 (pass-through)
 *
 *  Reference:
 *    include/drm/intel/pciids.h — INTEL_ADLS_IDS, INTEL_ADLP_IDS
 *    i915_pci.c pciidlist[]
 *
 *  เพิ่มการอ่าน GMD_ID register (0xD8C) ถ้ามี:
 *    - Meteor Lake+ มี has_gmd_id=1
 *    - register บอก IP version จริง
 *    - ถ้า fGraphicsVer >= 12.70 → MTL, ใช้ path พิเศษ
 * ─────────────────────────────────────────────────────────────────
 */
uint32_t MyIntelGPU::detectHardwareGeneration(void)
{
    uint32_t gen = 0;

    /*
     * ตรวจสอบว่า fPCIDevice valid
     * ถ้าไม่มี → ไม่สามารถอ่าน config space ได้ → ออก
     */
    if (!fPCIDevice) {
        IODebug("ERROR: fPCIDevice is NULL");
        return 0;
    }

    /*
     * ขั้นที่ 1: อ่าน PCI Device ID และ Revision ID
     *
     * PCI config space offset 0x00 = Vendor+Device (2+2 bytes)
     * kIOPCIConfigDeviceID = 0x02
     * kIOPCIConfigRevisionID = 0x08
     */
    fDeviceID = fPCIDevice->configRead16(kIOPCIConfigDeviceID);
    fRevision = fPCIDevice->configRead8(kIOPCIConfigRevisionID);

    IODebug("PCI DeviceID = 0x%04X, Revision = 0x%02X", fDeviceID, fRevision);

    /*
     * ขั้นที่ 2: จับคู่ Device ID เพื่อหาเจนจริง
     *
     * ใช้ top-level PCI ID ranges — รายละเอียดย่อย (GT1/GT2/GT3)
     * จะ map revision ID แทน
     */
    switch (fDeviceID >> 8) {
        case 0x46:  /* Alder Lake-S/P (0x468x-0x46Dx) */
            /*
             * Alder Lake = GEN12 (Xe_HPG architecture)
             *
             * แม้ว่าฮาร์ดแวร์จะเป็น Gen12 แต่เราจะแกล้งเป็น
             * Coffee Lake (Gen9) เพื่อให้ macOS driver เก่า
             * (AppleIntelCFLGraphics) ทำงานได้
             *
             * fFakeGen = 9  → translateAddress() จะทำงาน
             */
            gen = 12;
            fFakeGen = 9;
            IODebug("Detected Alder Lake (Gen12) → Faking Coffee Lake (Gen9)");
            break;

        case 0x3E:  /* Coffee Lake (0x3E90-0x3E9F) */
        case 0x9B:  /* Coffee Lake-S (0x9BC4, 0x9BC6) */
            /*
             * Coffee Lake — ของจริง ไม่ต้อง Fake
             * fFakeGen = 9 → translate หยุดทำงาน (fake==real)
             */
            gen = 9;
            fFakeGen = 9;
            IODebug("Detected Coffee Lake (Gen9) — native mode");
            break;

        case 0x9A:  /* Tiger Lake (0x9A60-0x9A70) */
            /*
             * Tiger Lake → สามารถ fake เป็น Coffee Lake
             * engine base ต่างกัน (VCS0=0x1c0000)
             */
            gen = 12;
            fFakeGen = 9;
            IODebug("Detected Tiger Lake (Gen12) → Faking Coffee Lake (Gen9)");
            break;

        case 0xA7:  /* Raptor Lake-P (0xA7AC, 0xA7Bx etc.) */
            /*
             * Raptor Lake = same Gen12.2 Xe_HPG IP as Alder Lake
             * engine register offsets เหมือน ADL ทั้งหมด
             * → fake เป็น Coffee Lake (Gen9) เช่นกัน
             */
            gen = 12;
            fFakeGen = 9;
            IODebug("Detected Raptor Lake (Gen12) → Faking Coffee Lake (Gen9)");
            break;

        default:
            /*
             * ไม่รู้จักเจน — ตั้งค่าเป็น pass-through
             * translateAddress() จะไม่เปลี่ยน offset
             */
            gen = 0;
            fFakeGen = 0;
            IODebug("Unknown device 0x%04X — pass-through mode", fDeviceID);
            break;
    }

    /*
     * ขั้นที่ 3: พยายามอ่าน GMD_ID register (เฉพาะ Gen12+)
     *
     * GMD_ID_GRAPHICS (0xD8C):
     *   bit [31:22] = Architecture version (0x00C = 12 for TGL/ADL)
     *   bit [21:14] = Release (0x05 = 5 for ADL)
     *   bit [5:0]   = Stepping (revision)
     *
     * ดัก exception: ถ้า register นี้ไม่มีในเจนเก่า
     * ให้อ่าน fFakeGen เป็นหลักแทน
     *
     * Reference: intel_device_info.c intel_ipver_early_init()
     */
    if (fRegs && gen >= 12) {
        uint32_t gmdId = 0;

        /*
         * อ่าน register 0xD8C โดยตรง
         * ใช้ readReg32() แทน dereference เพื่อให้ translation ทำงาน
         * (แต่ 0xD8C น่าจะอยู่นอกช่วง translation)
         */
        gmdId = readReg32(GMD_ID_GRAPHICS);

        if (gmdId != 0 && gmdId != 0xFFFFFFFF) {
            /*
             * GMD_ID มีค่า — ฮาร์ดแวร์รองรับ
             * สกัด graphics IP version
             *
             * GMD_ID_ARCH_MASK   = bits 31:22 (>> 22)
             * GMD_ID_RELEASE_MASK = bits 21:14 (>> 14) & 0xFF
             */
            uint32_t arch = (gmdId >> 22) & 0x3FF;   /* Architecture */
            uint32_t rel  = (gmdId >> 14) & 0xFF;     /* Release */

            IODebug("GMD_ID = 0x%08X (arch=%u, rel=%u)", gmdId, arch, rel);

            /*
             * ถ้า GMD_ID architecture = 0x00C (12)
             * → เก็บเลข version เต็ม
             */
            if (arch >= 12) {
                fGraphicsVer = (arch << 4) | rel;  /* เช่น 12.5 → 0xC5 */
                fUseGmdId = true;
                IODebug("GMD_ID confirmed: graphics version %u.%u", arch, rel);
            }
        } else {
            /*
             * GMD_ID all-0 หรือ all-1 — register ไม่มี
             * ใช้ device ID fallback
             */
            IODebug("GMD_ID not available — using PCI ID fallback");
        }
    }

    /*
     * ตั้งค่า fGraphicsVer ถ้ายังไม่ได้ set จาก GMD_ID
     */
    if (fGraphicsVer == 0) {
        fGraphicsVer = gen;  /* fallback */
    }

    return gen;
}

#pragma mark -
#pragma mark - buildTranslationTable

/*
 * ─────────────────────────────────────────────────────────────────
 *  void MyIntelGPU::buildTranslationTable()
 *
 *  สร้างตารางแปลพิกัด Register Offset
 *
 *  หลักการ:
 *    - macOS Intel driver (AppleIntelCFLGraphics) ถูก compile
 *      ด้วย register layout ของ Coffee Lake (Gen9)
 *    - เวลา macOS จะเขียน Ring Tail Register ของ VCS0
 *      มันจะคำนวณ address = 0x12000 + 0x80 = 0x12080
 *    - แต่ฮาร์ดแวร์ Alder Lake เก็บ VCS0 register ที่
 *      address = 0x1C0000 + 0x80 = 0x1C0080
 *    - ถ้าส่งไป 0x12080 → ผิดตำแหน่ง → GPU crash
 *
 *  วิธีแก้: translateAddress() จะรับ offset 0x12080
 *          แล้วคืน 0x1C0080
 *
 *  Engine ที่ต้องแปล:
 *    VCS0:   fake 0x12000 → real 0x1C0000  (Video Decode Gen12+)
 *    VECS0:  fake 0x1A000 → real 0x1C8000  (Video Encode Gen12+)
 *    RCS0:   fake 0x02000 → real 0x02000   (Render = same)
 *    BCS0:   fake 0x22000 → real 0x22000   (Blitter = same)
 *
 *  Cursor:
 *    Pipe B: fake 0x700C0 → real 0x71080
 *    Pipe A/C: same
 *
 *  Reference:
 *    intel_engine_cs.c engine mmio_bases[]
 *    intel_display_regs.h CURSOR_*_BASE macros
 * ─────────────────────────────────────────────────────────────────
 */
void MyIntelGPU::buildTranslationTable(void)
{
    /*
     * รีเซ็ตตารางก่อนสร้างใหม่
     */
    fTransCount = 0;
    memset(fTransTable, 0, sizeof(fTransTable));

    /*
     * ถ้า fFakeGen == 9 และ gen จริง != 9
     * → ต้องแปล register offset
     *
     * ถ้า fFakeGen == gen จริง → ไม่ต้องทำตาราง
     * (engine register เหมือนกันอยู่แล้ว)
     */
    if (fFakeGen == fGraphicsVer || fFakeGen == 0) {
        IODebug("No translation needed — native mode");
        return;
    }

    IODebug("Building translation table (FakeGen=%u, RealGen=%u)", fFakeGen, fGraphicsVer);

    /*
     * Entry 0: RCS0 (Render) — ไม่ต้องแปล
     *
     * ใส่ไว้เป็น entry ว่าง ๆ เพื่อความสะดวกตอน debug
     * เพราะ RCS0 offset เท่ากันทุกเจน (0x02000)
     */
    fTransTable[fTransCount].name       = "RCS0";
    fTransTable[fTransCount].fakeBase   = RCS0_BASE_FAKE;
    fTransTable[fTransCount].realBase   = RCS0_BASE_REAL;
    fTransTable[fTransCount].windowSize = ENGINE_WINDOW_SIZE;
    fTransTable[fTransCount].enabled    = false;  /* ไม่ต้องแปล */
    fTransCount++;

    /*
     * Entry 1: VCS0 (Video Decode) — ต้องแปล
     *
     * Coffee Lake: 0x12000
     * Alder Lake:  0x1C0000
     *
     * offset delta = 0x1C0000 - 0x12000 = 0x1AE000
     *
     * register ในช่วงนี้:
     *   0x12000 + 0x80 = ENGINE_TAIL (Ring Tail Pointer)
     *   0x12000 + 0x34 = ENGINE_HEAD (Ring Head Pointer)
     *   0x12000 + 0x3C = ENGINE_CTL  (Ring Control)
     *   0x12000 + 0x38 = ENGINE_START (Ring Start Address)
     *   0x12000 + 0x30 = HWSTAM (Hardware Status Mask)
     *
     * reference: intel_engine_regs.h — RING_TAIL, RING_HEAD
     */
    fTransTable[fTransCount].name       = "VCS0 (Video Decode)";
    fTransTable[fTransCount].fakeBase   = VCS0_BASE_FAKE;   /* 0x12000 */
    fTransTable[fTransCount].realBase   = VCS0_BASE_REAL;   /* 0x1C0000 */
    fTransTable[fTransCount].windowSize = ENGINE_WINDOW_SIZE;
    fTransTable[fTransCount].enabled    = true;
    fTransCount++;

    /*
     * Entry 2: BCS0 (Blitter) — ไม่ต้องแปล
     *
     * offset เหมือนกันทุกเจน (0x22000)
     * ใช้สำหรับ blit buffer / memory copy
     */
    fTransTable[fTransCount].name       = "BCS0 (Blitter)";
    fTransTable[fTransCount].fakeBase   = BCS0_BASE_FAKE;   /* 0x22000 */
    fTransTable[fTransCount].realBase   = BCS0_BASE_REAL;   /* 0x22000 */
    fTransTable[fTransCount].windowSize = ENGINE_WINDOW_SIZE;
    fTransTable[fTransCount].enabled    = false;  /* ไม่ต้องแปล */
    fTransCount++;

    /*
     * Entry 3: VECS0 (Video Encode) — ต้องแปล
     *
     * Coffee Lake: 0x1A000
     * Alder Lake:  0x1C8000
     *
     * offset delta = 0x1C8000 - 0x1A000 = 0x1AE000
     *
     * register: RING_TAIL, RING_HEAD, RING_CTL, RING_START, HWSTAM
     */
    fTransTable[fTransCount].name       = "VECS0 (Video Encode)";
    fTransTable[fTransCount].fakeBase   = VECS0_BASE_FAKE;   /* 0x1A000 */
    fTransTable[fTransCount].realBase   = VECS0_BASE_REAL;   /* 0x1C8000 */
    fTransTable[fTransCount].windowSize = ENGINE_WINDOW_SIZE;
    fTransTable[fTransCount].enabled    = true;
    fTransCount++;

    /*
     * Entry 4: Cursor Pipe B — ต้องแปล
     *
     * Coffee Lake: CUR_BASE Pipe B = 0x700C0
     * Alder Lake:  CUR_BASE Pipe B = 0x71080
     *
     * register ใน 0x700C0:
     *   +0x00 = CURCNTR  (cursor control)
     *   +0x04 = CURBASE  (cursor surface base address)
     *   +0x08 = CURPOS   (cursor position)
     *   +0x0C = CURBSIZE (cursor buffer size)
     *
     * Pipe B cursor offset ต่างกันเพราะ Alder Lake
     * จัด pipe register block ใหม่
     *
     * Reference: intel_display_regs.h — CURSOR_A/B/C/D_OFFSET
     *            _CURABASE = 0x70080, _CURBBASE = 0x71080 (TGL+)
     */
    fTransTable[fTransCount].name       = "Cursor Pipe B";
    fTransTable[fTransCount].fakeBase   = CURSOR_B_FAKE;    /* 0x700C0 */
    fTransTable[fTransCount].realBase   = CURSOR_B_REAL;    /* 0x71080 */
    fTransTable[fTransCount].windowSize = 0x40;             /* บล็อกละ 64 bytes */
    fTransTable[fTransCount].enabled    = true;
    fTransCount++;

    /*
     * Entry 5: Cursor Pipe D — ที่ Alder Lake มี แต่ Coffee Lake ไม่มี
     *
     * ถ้า macOS พยายามเขียน 0x73080 (pipe D cursor offset)
     * เราไม่ต้องแปล เพราะ pipe D ไม่มีใน Coffee Lake
     * แต่เราเปิด translation ไว้สำหรับ Gen12+ พื้นเมือง
     *
     * ถ้าต้องการ FakeID → ควร block pipe D ไว้
     * เพราะ Coffee Lake มีแค่ pipe A, B, C
     *
     * disabled = true + real==fake → pass-through (มีผลเฉพาะ mode native)
     * ถ้า fGraphicsVer >= 12 และ fFakeGen == 9 → ควรไม่ใช้ pipe D
     */
    fTransTable[fTransCount].name       = "Cursor Pipe D";
    fTransTable[fTransCount].fakeBase   = CURSOR_D_FAKE;    /* 0x73080 */
    fTransTable[fTransCount].realBase   = CURSOR_D_REAL;    /* 0x73080 */
    fTransTable[fTransCount].windowSize = 0x40;
    fTransTable[fTransCount].enabled    = false;  /* ปิดไว้ — pipe D ไม่มีใน CFL */
    fTransCount++;

    /*
     * Done — ปริ้นตารางทั้งหมดเพื่อ debug
     */
    IODebug("Translation table built with %d entries:", fTransCount);
    for (int i = 0; i < fTransCount; i++) {
        IODebug("  [%d] %s: 0x%04X → 0x%04X %s",
                i,
                fTransTable[i].name,
                fTransTable[i].fakeBase,
                fTransTable[i].realBase,
                fTransTable[i].enabled ? "[active]" : "[skipped]");
    }
}

#pragma mark -
#pragma mark - translateAddress (CORE FAKEID LOGIC)

/*
 * ─────────────────────────────────────────────────────────────────
 *  uint32_t MyIntelGPU::translateAddress(uint32_t fakeOffset)
 *
 *  แกนหลักของ FakeID — แปลง MMIO register offset
 *
 *  รับ offset ที่ macOS ส่งมา (Coffee Lake expected address)
 *  เปลี่ยนเป็น offset จริงของ Alder Lake
 *
 *  Algorithm:
 *    for each entry in fTransTable:
 *        if offset >= entry.fakeBase
 *        && offset <  entry.fakeBase + entry.windowSize:
 *            if !entry.enabled → return offset (ไม่ต้องแปล)
 *            realOffset = entry.realBase + (offset - entry.fakeBase)
 *            return realOffset
 *    return offset (ไม่ตรง entry ไหน → pass-through)
 *
 *  Edge Cases:
 *    - Register ที่ common ระหว่างเจน (0x60000 transcoder,
 *      0x70000 pipe, 0x44400 interrupts) → pass-through
 *    - หากพบ offset ที่เลย windowSize → return offset ("partial match"
 *      ก็ไม่ทำ เพราะอาจชี้ไปผิดตำแหน่ง)
 *    - ถ้า fFakeGen == 0 → return offset ทันที (unknown device)
 *
 *  Performance Note:
 *    readReg32/writeReg32 ถูกเรียกถี่มาก (ทุกคำสั่งของ framebuffer/
 *    acceleration). การ loop 8 entries ต่อ 1 read → ~8 cmp + branch.
 *    บน x86_64 cost ≈ 2-3 ns → ยอมรับได้
 *
 *    แต่ถ้าต้องการ optimize: จัดเรียง entry จากใช้งานบ่อยไปน้อย
 *    (cursor, VCS0, VECS0, ...) แต่ละ entry ที่เจอก่อน = return ทันที
 * ─────────────────────────────────────────────────────────────────
 */
uint32_t MyIntelGPU::translateAddress(uint32_t fakeOffset)
{
    /*
     * ถ้า FakeGen == 0 (ไม่รู้จักฮาร์ดแวร์) → ส่งตรง
     * หรือถ้าไม่ต้อง Fake → ส่งตรง
     */
    if (fFakeGen == 0 || fFakeGen == fGraphicsVer) {
        return fakeOffset;
    }

    /*
     * วนลูปตาราง translation
     *
     * ถ้า offset ของ macOS อยู่ในช่วง fakeBase ~ fakeBase+size
     * → แปลงเป็น realBase + offset ภายในช่วง
     *
     * เช่น: fakeOffset = 0x12080
     *       entry.fakeBase = 0x12000, entry.realBase = 0x1C0000
     *       offset_in_window = 0x12080 - 0x12000 = 0x80
     *       realOffset = 0x1C0000 + 0x80 = 0x1C0080
     *       return 0x1C0080 ✓
     */
    for (int i = 0; i < fTransCount; i++) {
        RegisterTranslationEntry *entry = &fTransTable[i];

        if (!entry->enabled) {
            continue;
        }

        /*
         * ตรวจสอบว่า offset อยู่ในช่วงของ entry นี้หรือไม่
         * ใช้เชิงคณิตศาสตร์แทน range check แบบ mask
         * (เพราะ window ไม่จำเป็นต้องเป็น power of two)
         */
        if (fakeOffset >= entry->fakeBase &&
            fakeOffset <  entry->fakeBase + entry->windowSize) {

            /*
             * คำนวณ offset จริง
             * offset_in_window = fakeOffset - entry->fakeBase
             * realOffset = entry->realBase + offset_in_window
             */
            uint32_t offset_in_window = fakeOffset - entry->fakeBase;
            uint32_t realOffset = entry->realBase + offset_in_window;

#ifdef EXTRA_DEBUG
            IODebug("translate: 0x%04X → 0x%04X (%s, +0x%X in window)",
                    fakeOffset, realOffset, entry->name, offset_in_window);
#endif

            return realOffset;
        }
    }

    /*
     * ไม่มี translation entry ที่ตรงกัน
     * แสดงว่า offset นี้เป็น register "ทั่วไป" (common register)
     * เช่น 0x70000 (pipe register), 0x60000 (transcoder), 0x44400 (interrupt)
     * → ส่งตรง ไม่ต้องเปลี่ยน
     */
    return fakeOffset;
}

#pragma mark -
#pragma mark - MMIO Read / Write

/*
 * ─────────────────────────────────────────────────────────────────
 *  uint32_t MyIntelGPU::readReg32(uint32_t offset)
 *
 *  อ่าน 32-bit register จาก MMIO BAR0
 *
 *  Flow:
 *    1. translateAddress(offset) → realOffset (FakeID)
 *    2. ตรวจ fRegs != NULL (mapping ถูกหรือไม่)
 *    3. OSMemoryBarrier() — compiler barrier ก่อนอ่าน
 *    4. ใช้ volatile pointer เพื่อให้ compiler ไม่ optimize
 *       (volatile ∀ registers)
 *    5. OSSynchronizeIO() — memory barrier guarantee
 *       ให้คำสั่งที่มาก่อน/หลังไม่ข้ามลำดับ
 *    6. return value
 *
 *  Memory Barrier Explanation:
 *    OSMemoryBarrier():   ป้องกัน compiler reorder (ราคาถูก)
 *    OSSynchronizeIO():   CPU memory fence (mfence/lfence)
 *                            ราคาแพง แต่กัน DMA ไม่ถูกต้อง
 *
 *  Reference: Linux intel_uncore_read -> __raw_i915_read32 + mb()
 *             Apple IOKit: OSSynchronizeIO() ≈ dmb() on ARM64
 *                                          ≈ mfence on x86_64
 * ─────────────────────────────────────────────────────────────────
 */
uint32_t MyIntelGPU::readReg32(uint32_t offset)
{
    uint32_t realOffset;
    uint32_t value;

    /*
     * แปลง offset ถ้าอยู่ในช่วงที่ต้อง Fake
     */
    realOffset = translateAddress(offset);

    /*
     * ถ้า fRegs ยังไม่ถูก map → return 0xFF (all-ones = "no device")
     */
    if (!fRegs) {
        IODebug("WARNING: readReg32(0x%04X) — fRegs is NULL", realOffset);
        return 0xFFFFFFFF;
    }

    /*
     * Compiler barrier: ห้าม compiler เปลี่ยนลำดับ
     * คำสั่งที่อ่าน/เขียน memory ก่อนหน้านี้ กับ register read นี้
     */
    OSMemoryBarrier();

    /*
     * อ่าน register ผ่าน volatile pointer:
     *   - volatile → compiler ต้องอ่านทุกครั้ง (ไม่ cache)
     *   - IOMemoryMap::getVirtualAddress() → virtual address ของ BAR0
     *   - fRegs เป็น uint8_t* เพราะฉะนั้น +
     *     realOffset เป็น byte offset
     */
    value = *((volatile uint32_t *)(fRegs + realOffset));

    /*
     * Hardware memory barrier:
     *   - กัน CPU OoO execution
     *   - กรณี register read หลัง write → ต้อง fence เพื่อ flush
     *     write buffer ก่อนอ่าน
     */
    OSSynchronizeIO();

    return value;
}

/*
 * ─────────────────────────────────────────────────────────────────
 *  void MyIntelGPU::writeReg32(uint32_t offset, uint32_t value)
 *
 *  เขียน 32-bit register ไปยัง MMIO BAR0
 *
 *  Flow:
 *    1. translateAddress(offset)
 *    2. ตรวจ fRegs
 *    3. volatile write
 *    4. OSSynchronizeIO() — จำเป็นมาก!
 *       โดยเฉพาะ write ไปยัง Engine Ring Register:
 *         - RING_TAIL → ถ้าไม่ fence → hardware เห็น value เก่า
 *         - GFX_FLSH_CNTL → TLB flush → ต้อง complete ก่อน
 *         - Interrupt masking → ต้อง immediate effect
 *    5. (optional) extra IODelayWriteCombined() ถ้าเป็น WC area
 *
 *  NB: register write ไม่จำเป็นต้องมี compiler barrier ก่อน
 *      แต่อาจต้องมีหลัง write เพื่อ flush
 *      ในทางปฏิบัติ OSSynchronizeIO() ครอบคลุมทั้งสอง
 *
 *  Reference: Linux intel_uncore_write -> __raw_i915_write32 + mb()
 * ─────────────────────────────────────────────────────────────────
 */
void MyIntelGPU::writeReg32(uint32_t offset, uint32_t value)
{
    uint32_t realOffset;

    /*
     * แปลง offset ถ้าต้อง Fake
     */
    realOffset = translateAddress(offset);

    if (!fRegs) {
        IODebug("WARNING: writeReg32(0x%04X, 0x%08X) — fRegs is NULL",
                realOffset, value);
        return;
    }

    /*
     * เขียน register
     * volatile — บังคับ CPU เขียนเลย ไม่ buffer
     */
    *((volatile uint32_t *)(fRegs + realOffset)) = value;

    /*
     * Memory Barrier — สำคัญมาก!
     *
     * หลังจาก write register แล้ว ต้องแน่ใจว่า
     * write transaction ไปถึง PCI bus ก่อน return
     *
     * ถ้าไม่ใส่: CPU อาจ merge write หลายครั้ง
     * หรือ execute out-of-order → register เขียนไม่ทันการ
     * ซึ่งโดยเฉพาะ Ring Tail Register นั้น อ่อนไหวมาก
     * (ถ้าไม่ flush → GPU ไม่เห็น tail → scheduler ล็อก)
     *
     * OSSynchronizeIO() = full memory barrier (sfence/mfence)
     */
    OSSynchronizeIO();
}

#pragma mark -
#pragma mark - Aperture (BAR2) Access

/*
 * ─────────────────────────────────────────────────────────────────
 *  uint32_t readAperture32 / writeAperture32
 *
 *  Aperture (BAR2 / GMADR) คือ CPU-mappable window ไปยัง GGTT
 *  - Coffee Lake: aperture ขนาด 256MB (สูงสุด 512MB)
 *  - Alder Lake:  aperture ขนาด 256MB-1GB (ขึ้นกับ config)
 *
 *  access ผ่าน WC (Write-Combined) mapping สำหรับประสิทธิภาพ
 *  เวลา CPU→GPU transfer
 *
 *  Linux counterpart:
 *    io_mapping_map_wc(&ggtt->iomap, offset) → ioremap_wc(bar2)
 *    ggtt->iomap.data = ioremap_wc(ggtt->gmadr.start, aperture_size)
 *
 *  macOS IOKit:
 *    mapDeviceMemoryWithIndex(2) → map BAR2
 *    kIOMapWriteCombined ถ้าต้องการ WC semantics
 *    (ถ้า default = cache-disabled (UC) ก็ safe แต่ช้า)
 * ─────────────────────────────────────────────────────────────────
 */
uint32_t MyIntelGPU::readAperture32(uint64_t apertureOffset)
{
    if (!fApertureVA) {
        IODebug("WARNING: readAperture32(0x%llX) — fApertureVA is NULL", apertureOffset);
        return 0xFFFFFFFF;
    }

    if (apertureOffset >= fApertureSize) {
        IODebug("WARNING: readAperture32(0x%llX) — out of bounds (size=0x%llX)",
                apertureOffset, fApertureSize);
        return 0xFFFFFFFF;
    }

    OSMemoryBarrier();
    uint32_t value = *((volatile uint32_t *)(fApertureVA + apertureOffset));
    OSSynchronizeIO();
    return value;
}

void MyIntelGPU::writeAperture32(uint64_t apertureOffset, uint32_t value)
{
    if (!fApertureVA) {
        IODebug("WARNING: writeAperture32(0x%llX) — fApertureVA is NULL", apertureOffset);
        return;
    }

    if (apertureOffset >= fApertureSize) {
        IODebug("WARNING: writeAperture32(0x%llX) — out of bounds", apertureOffset);
        return;
    }

    *((volatile uint32_t *)(fApertureVA + apertureOffset)) = value;

    /*
     * Write-Combined buffer flush
     *
     * ถ้า aperture ถูก map แบบ WC:
     *   CPU เลือก buffered writes แล้ว flush ทีเดียว
     *   → ต้อง IODelayWriteCombined() เพื่อ flush WC buffer
     *   → หรือ OSSynchronizeIO() ซึ่งหนักกว่า
     *
     * ถ้า map แบบ UC (uncacheable):
     *   write แต่ละครั้งไปถึง bus ทันที → ไม่ต้อง flush
     *   แต่ช้า (≈ 1/10 bandwidth ของ WC)
     *
     * ที่นี่ใช้ OSSynchronizeIO() เพราะ simple and safe
     */
    OSSynchronizeIO();

    /*
     * IODelayWriteCombined(address) — flush WC buffer
     * (สำหรับ x86_64, IODelayWriteCombined อ่านจาก WC region
     *  เพื่อ flush pending writes)
     * ใช้ *((volatile uint32_t *)(fApertureVA)) = value;
     * แต่ต้องมี address ที่ไม่ใช่ destination
     * แต่ safest = OSSynchronizeIO()
     */
}

#pragma mark -
#pragma mark - GGTT Management

/*
 * ─────────────────────────────────────────────────────────────────
 *  void MyIntelGPU::ggttInvalidate(void)
 *
 *  ส่ง TLB Invalidate ไปยัง GPU
 *
 *  GGTT มี TLBs (Translation Lookaside Buffers) ที่ cache
 *  page table entries หลังจากเขียน PTE ใหม่ลง GSM
 *  ต้อง invalidate เพื่อให้ hardware เห็น PTE ใหม่
 *
 *  Linux: ggtt->invalidate() หรือ
 *         intel_uncore_write(uncore, GFX_FLSH_CNTL_GEN6, 0)
 *
 *  Register:
 *    Gen6+: GFX_FLSH_CNTL = 0x10100
 *           write 0 → invalidate
 *    Gen8+: เขียนเพิ่ม 1 ที่ GFX_FLSH_CNTL
 *    (function เดียวกับที่ใช้ invalidate TLB)
 *
 *  หลัง invalidate ต้องรอให้ hardware เสร็จ → dummy read
 * ─────────────────────────────────────────────────────────────────
 */
void MyIntelGPU::ggttInvalidate(void)
{
    if (!fRegs) {
        return;
    }

    /*
     * เขียน 0 ไปที่ GFX_FLSH_CNTL_GEN6 = 0x10100
     * เพื่อ flush GGTT TLB
     *
     * writeReg32 จะ OSSynchronizeIO เองอัตโนมัติ
     */
    writeReg32(GFX_FLSH_CNTL_GEN6, 0);

    /*
     * Dummy read — รอให้ TLB flush สมบูรณ์
     *
     * อ่าน register เดียวกัน (ผ่านการ translate ด้วย)
     * เพื่อรอ pending write ให้สมบูรณ์
     *
     * (MMIO write ถึง hardware → hardware เริ่มทำงาน
     *  → hardware เขียน register เสร็จ → read เห็นค่า
     *  → แสดงว่า flush สมบูรณ์)
     */
    (void)readReg32(GFX_FLSH_CNTL_GEN6);
}

#pragma mark -
#pragma mark - CPU Cache Flush

/*
 * ─────────────────────────────────────────────────────────────────
 *  void MyIntelGPU::clflushRange(const void *addr, size_t size)
 *
 *  CPU Cache Line Flush
 *
 *  ใช้ตอน CPU เขียน buffer ใน WB (Write-Back) cache
 *  แล้วต้องให้ GPU อ่านค่าล่าสุด (dGPU / non-LLC platform)
 *
 *  Linux: drm_clflush_virt_range():
 *    - ใช้ inline asm: clflush [addr]
 *    - วน flush ทุก cache line (64 bytes)
 *
 *  x86_64 CLFLUSH instruction:
 *    clflush m8 — flush cache line ที่มี address นี้
 *    ต้อง address-aligned ถึง cache line
 *
 *  สำคัญ:
 *    CLFLUSH เป็น serializing instruction → หลัง flush
 *    memory อื่น safe โดยไม่ต้อง barrier เพิ่ม
 *
 *    แต่ compiler barrier (OSMemoryBarrier) ยังจำเป็น
 *    เพื่อกัน compiler reorder
 *
 *  หมายเหตุ:
 *    Gen9+ iGPU มักมี LLC (Last Level Cache) แชร์กับ CPU
 *    → coherency อัตโนมัติ → ไม่ต้อง flush
 *    แต่ถ้า FakeID แล้วตั้งค่า PAT ไม่ตรง → อาจต้อง flush
 *
 *    Alder Lake = has_llc=1 (LLC shared) → WB access coherenct
 *    แต่บาง memory types (WC, UC) → ยังต้อง flush
 *
 *  Apple Silicon (ARM64):
 *    ไม่มี CLFLUSH — ใช้ syscall สำหรับ cache maintenance
 *    (dc cvac / dc cvau)
 *    บน macOS for ARM64: ใช้ IODelayWriteCombined() หรือ
 *    memcpy + OSSynchronizeIO()
 * ─────────────────────────────────────────────────────────────────
 */
void MyIntelGPU::clflushRange(const void *addr, size_t size)
{
    /*
     * ตรวจสอบ validity
     */
    if (!addr || size == 0) {
        return;
    }

    /*
     * Align address ลงไปยัง cache line boundary (64 bytes)
     *
     * เพราะ CLFLUSH ทำงานกับ cache line ทั้งก้อน
     * ถ้าไม่ align: flush = cache line ก่อนหน้า + ยาวขึ้น
     * (แต่ยังถูกต้อง เพียงแต่อาจ flush ข้อมูลที่ไม่เกี่ยวข้อง)
     */
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)0x3F;   /* align 64 */
    uintptr_t end   = (uintptr_t)addr + size;

    /*
     * compiler barrier — ก่อน flush ต้องแน่ใจว่า
     * store ทั้งหมดถูก commit แล้ว
     */
    OSMemoryBarrier();

    /*
     * วน CLFLUSH ทุก cache line ที่ range ครอบคลุม
     *
     * การใช้ inline assembly:
     *   "clflush (%0)" :: "r"(ptr)
     *   → flush cache line ที่มี address = ptr
     *
     * clflush ใช้กับ virtual address; hardware จะหา
     * physical cache line โดยอัตโนมัติ
     */
#if defined(__x86_64__)
    for (uintptr_t ptr = start; ptr < end; ptr += 64) {
        __asm__ volatile (
            "clflush (%0)"
            :
            : "r"(ptr)
            : "memory"
        );
    }
#elif defined(__aarch64__)
    /*
     * ARM64 path (ถ้าพอร์ตไป Apple Silicon)
     *
     * ใช้ syscall cache_invalidate หรือ dc cvac
     *
     * macOS kernel: flush_dcache64(addr, size, 0)
     * แต่ Apple API อาจไม่ public
     *
     * IOMemoryDescriptor::flushProcessorCache() —
     * มีเฉพาะ macOS internal
     *
     * ที่นี่กันไว้ก่อน (skeleton)
     */
    // __builtin_arm_dc(0, addr);  /* dc cvac */
#endif

    /*
     * compiler barrier หลัง flush
     * กัน compiler เอา store ไปไว้หลัง flush instruction
     */
    OSMemoryBarrier();
}

#pragma mark -
#pragma mark - start

/*
 * ─────────────────────────────────────────────────────────────────
 *  bool MyIntelGPU::start(IOService *provider)
 *
 *  ✅ นี่คือ MAIN ENTRY POINT ของ Kext
 *
 *  Pipeline (ตรงกับ i915_driver_probe + i915_driver_hw_probe):
 *
 *  ┌─────────────────────────────────────┐
 *  │ Phase 1: PCI Enable                 │ ← pci_enable_device()
 *  │   setBusMasterEnable(true)          │
 *  │   setMemoryEnable(true)             │
 *  ├─────────────────────────────────────┤
 *  │ Phase 2: BAR0 Mapping (MMIO Regs)   │ ← i915_driver_mmio_probe()
 *  │   mapDeviceMemoryWithIndex(0)       │
 *  │   → fRegs[*]  for MMIO access       │
 *  ├─────────────────────────────────────┤
 *  │ Phase 3: BAR2 Mapping (Aperture)    │ ← i915_ggtt_probe_hw()
 *  │   mapDeviceMemoryWithIndex(2)       │
 *  │   → fApertureVA, fApertureSize      │
 *  ├─────────────────────────────────────┤
 *  │ Phase 4: Detect Generation          │ ← intel_device_info_runtime_init_early()
 *  │   → GMD_ID (ถ้ามี)                  │
 *  │   → fFakeGen = 9 (ถ้า ADL)         │
 *  ├─────────────────────────────────────┤
 *  │ Phase 5: Build Translation Table    │ ← Per-gen offset table
 *  │   → VCS0, VECS0, Cursor B           │
 *  ├─────────────────────────────────────┤
 *  │ Phase 6: GGTT Basic Init            │ ← i915_ggtt_init_hw()
 *  │   → read GTT_SIZE register          │
 *  │   → GGTT TLB invalidate             │
 *  ├─────────────────────────────────────┤
 *  │ Phase 7: Register Service           │ ← drm_dev_register()
 *  │   → registerService()              │
 *  └─────────────────────────────────────┘
 *
 *  Reference:
 *    i915_driver.c int i915_driver_probe(struct pci_dev *pdev, ...)
 *      line 841 → calls i915_driver_mmio_probe, i915_driver_hw_probe
 *      line 800 → i915_driver_create — create struct
 *
 *    i915_driver.c i915_driver_mmio_probe(struct drm_i915_private *i915)
 *      line 326 → intel_uncore_init_mmio()
 *                 intel_device_info_runtime_init()
 *
 *    i915_driver.c i915_driver_hw_probe(struct drm_i915_private *i915)
 *      line 471 → i915_ggtt_probe_hw()
 *                 i915_ggtt_init_hw()
 *                 pci_set_master()
 * ─────────────────────────────────────────────────────────────────
 */
bool MyIntelGPU::start(IOService *provider)
{
    /*
     * ============================================================
     *  Phase 0: Super Call + Validation
     * ============================================================
     *
     * เรียก super::start() ก่อน — ถ้าไม่ผ่าน แสดงว่า
     * IOKit ไม่พร้อม → หยุดทันที
     */
    if (!super::start(provider)) {
        IODebug("ERROR: super::start() failed");
        return false;
    }

    /*
     * ทุกอย่างใน start() ใช้ goto แทนการ return ทันที
     * (เหมือน Linux i915) เพื่อให้ cleanup ชัวร์
     *
     * ตัวแปรสำหรับ goto fail
     */
    bool        success  = false;
    uint32_t    barCount = 0;

    /*
     * ============================================================
     *  Phase 1: PCI Setup
     * ============================================================
     *
     * จับ IOPCIDevice จาก provider
     * (AppleIntelGraphics ควรได้รับจาก IOPCIDevice)
     *
     * Linux-equivalent:
     *   struct pci_dev *pdev = to_pci_dev(dev->dev);
     *   pci_enable_device(pdev);
     *   pci_set_master(pdev);
     */
    fPCIDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!fPCIDevice) {
        IODebug("ERROR: provider is not IOPCIDevice");
        goto fail;
    }

    /*
     * retain() — เพิ่ม refcount เก็บไว้ใช้ตลอดอายุ kext
     * เพราะ provider อาจถูก release ได้ถ้าเราไม่ retain
     */
    fPCIDevice->retain();

    /*
     * Enable PCI Memory Space + Bus Mastering
     *
     * command register ตั้งค่า:
     *   bit 1 = Memory Space Enable (MSE)
     *   bit 2 = Bus Master Enable (BME)
     *
     * Linux:
     *   pci_enable_device(pdev) → set PCI_COMMAND_MEMORY
     *   pci_set_master(pdev)    → set PCI_COMMAND_MASTER
     */
    fPCIDevice->setBusMasterEnable(true);
    fPCIDevice->setMemoryEnable(true);

    IODebug("Phase 1: PCI setup OK");

    /*
     * ============================================================
     *  Phase 2: BAR0 (MMIO) Mapping
     * ============================================================
     *
     * BAR0 = 64-bit prefetchable memory (MMIO registers)
     * ขนาด ≈ 2MB (0x200000) บน Gen9+
     *
     * Linux:
     *   intel_uncore_init_mmio():
     *     i915->uncore.regs = ioremap(pci_resource_start(pdev, 0),
     *                                 pci_resource_len(pdev, 0));
     *
     * macOS IOKit:
     *   mapDeviceMemoryWithIndex(0) → IOMemoryMap
     *   → getVirtualAddress() = ioremap equivalent
     */
    fMMIODesc = fPCIDevice->getDeviceMemoryWithRegister(
                    kIOPCIMemorySpace64Bit, 0);
    if (!fMMIODesc) {
        IODebug("ERROR: Cannot get BAR0 descriptor");
        goto fail;
    }
    fMMIODesc->retain();

    /*
     * Map BAR0 ผ่าน IOMemoryMap
     *
     * kIOMapInhibitCache = UC (uncacheable) — safe สำหรับ MMIO
     * (register I/O ต้องไม่ cache เพราะ volatile)
     */
    fMMIOMap = fMMIODesc->createMappingInTask(
                    kernel_task,
                    kIOMapAnywhere | kIOMapInhibitCache,
                    0,
                    fMMIODesc->getLength());

    if (!fMMIOMap) {
        IODebug("ERROR: Cannot map BAR0 MMIO");
        goto fail;
    }

    fRegs = reinterpret_cast<volatile uint8_t *>(fMMIOMap->getVirtualAddress());

    IODebug("Phase 2: BAR0 MMIO mapped at 0x%p (size=0x%llX)",
            fRegs, fMMIODesc->getLength());

    /*
     * ============================================================
     *  Phase 3: BAR2 (Aperture) Mapping
     * ============================================================
     *
     * BAR2 = 64-bit prefetchable memory (GMADR)
     * = Aperture → CPU-mappable window สู่ GGTT
     *
     * Linux:
     *   ggtt->gmadr = pci_resource(pdev, 2);
     *   ggtt->iomap = io_mapping_create_wc(ggtt->gmadr.start,
     *                                       ggtt->mappable_end);
     *
     * macOS:
     *   mapDeviceMemoryWithIndex(2)
     *
     * เช็คว่ามี BAR2 จริง (อาจไม่มีในบาง gen)
     * ใช้ getLength() == 0 เพื่อ check
     */
    barCount = fPCIDevice->getDeviceMemoryCount();
    IODebug("Device memory count: %u", barCount);

    /*
     * BAR2 โดยปกติคือ index ที่ 2
     * แต่สำหรับ iGPU บางรุ่น BAR0=MMIO, BAR2=GTT aperture
     * (BAR1 เป็น optional GSM)
     */
    if (barCount > 2) {
        fApertureDesc = fPCIDevice->getDeviceMemoryWithRegister(
                            kIOPCIMemorySpace64Bit, 2);
        if (fApertureDesc && fApertureDesc->getLength() > 0) {
            fApertureDesc->retain();

            /*
             * Map BAR2 — เลือก cache strategy ตามโหมด FakeID
             *
             * Write-Combined (WC):
             *   ถ้า fake → Coffee Lake: ใช้ WC เพื่อ CPU→GPU
             *   transfer ความเร็วสูง (buffer writes แล้ว flush ทีเดียว)
             *   แต่ต้อง flush WC buffer ด้วย OSSynchronizeIO()
             *
             * Uncacheable (UC/InhibitCache):
             *   safe default — write ถึง bus ทันที
             *   แต่ bandwidth ต่ำกว่า WC ประมาณ 10x
             */
            IOOptionBits apertireFlags = kIOMapAnywhere;
            if (fFakeGen == 9 && fGraphicsVer != 9) {
                apertireFlags |= kIOMapWriteCombined;
            } else {
                apertireFlags |= kIOMapInhibitCache;
            }

            fApertureMap = fApertureDesc->createMappingInTask(
                               kernel_task,
                               apertireFlags,
                               0,
                               fApertureDesc->getLength());

            if (fApertureMap) {
                fApertureVA = reinterpret_cast<volatile uint8_t *>(
                                  fApertureMap->getVirtualAddress());
                fApertureSize = fApertureDesc->getLength();

                IODebug("Phase 3: BAR2 aperture mapped at 0x%p (size=0x%llX, %s)",
                        fApertureVA, fApertureSize,
                        (apertireFlags & kIOMapWriteCombined) ? "WC" : "UC");

                OSSynchronizeIO();
            } else {
                IODebug("WARNING: Could not map BAR2 aperture — continuing");
            }
        } else {
            IODebug("BAR2 descriptor not available — no aperture?");
        }
    } else {
        IODebug("Device has only %u BARs — no aperture index 2", barCount);
    }

    /*
     * ============================================================
     *  Phase 4: Detect Generation
     * ============================================================
     *
     * ตอนนี้ fRegs พร้อม → detect device + GMD_ID
     *
     * Linux: intel_device_info_runtime_init_early()
     */
    detectHardwareGeneration();

    IODebug("Phase 4: Hardware detected — Gen=%u, FakeGen=%u%s",
            fGraphicsVer, fFakeGen,
            fUseGmdId ? " (GMD_ID)" : "");

    /*
     * ============================================================
     *  Phase 5: Build Translation Table
     * ============================================================
     *
     * สร้างตารางเพื่อ VCS0, VECS0, Cursor register
     * ใช้ใน translateAddress() ตอน readReg32 / writeReg32
     */
    buildTranslationTable();

    IODebug("Phase 5: Translation table ready (%d entries)", fTransCount);

    /*
     * ============================================================
     *  Phase 6: GGTT Basic Init
     * ============================================================
     *
     * อ่าน GTT size register และ invalidate
     *
     * Linux: i915_ggtt_init_hw() → ggtt_init_hw()
     *   - intel_uncore_write(uncore, GFX_FLSH_CNTL, 0)  → TLB flush
     *   - GGTT TLB invalidate fix (Wa_22011193314)
     *
     * แต่ถ้าไม่แน่ใจ GGTT address → ปล่อยไว้ก่อน (skeleton)
     */
    ggttInvalidate();

    IODebug("Phase 6: GGTT invalidated");

    /*
     * ============================================================
     *  Phase 7: Register Service
     * ============================================================
     *
     * registerService() → IOKit ประกาศ service นี้
     * driver ตัวอื่น (เช่น framebuffer) จะ probe มาหาเรา
     *
     * Linux: drm_dev_register() → ทำให้ device node ปรากฏ
     *
     * attachToParent(provider, fPCIDevice) — เชื่อมโยง
     * กับ provider tree เพื่อให้ IOService ส่ง event ถึงกัน
     * (interrupt, power management)
     */
    registerService();

    /*
     * สำเร็จ!
     */
    IODebug("Phase 7: Kext start completed successfully");
    success = true;
    goto done;

    /*
     * ============================================================
     *  Error Handler — "Fail" Label
     * ============================================================
     *
     * ถ้า phase ใดล้มเหลว → cleanup partial state
     * และ stop() ที่ IOKit เรียกตาม
     *
     * (คล้าย goto out_err ใน Linux i915_pci_probe)
     */
fail:
    IODebug("FAILED — cleaning up and returning false");
    success = false;

    /*
     * stop() will be called by IOService
     * เราสามารถ cleanup ได้ใน stop() หรือตรงนี้ก็ได้
     * โดยทั่วไปปล่อยให้ stop() จัดการ (defensive)
     */

done:
    return success;
}

#pragma mark -
#pragma mark - stop

/*
 * ─────────────────────────────────────────────────────────────────
 *  void MyIntelGPU::stop(IOService *provider)
 *
 *  หยุด Kext — ปล่อยทรัพยากรที่จองไว้ใน start()
 *
 *  Linux: i915_driver_remove() → cleanup ที่ mirror กับ probe
 *         intel_uncore_fini_mmio()
 *         i915_ggtt_driver_release()
 *
 *  macOS: ปล่อย IOMemoryMap, release IOPCIDevice
 *
 *  หมายเหตุ: IOKit จะเรียก stop() ก่อน free() เสมอ
 *            ยกเว้นกรณี error ตอน init → ไม่เรียก stop()
 *            ดังนั้น free() ต้อง defense ด้วย
 * ─────────────────────────────────────────────────────────────────
 */
void MyIntelGPU::stop(IOService *provider)
{
    IODebug("stop() — releasing resources");

    /*
     * ปล่อย Aperture Map
     * (IOMemoryMap::release() → unmap + release)
     */
    if (fApertureMap) {
        fApertureMap->release();
        fApertureMap = NULL;
    }
    if (fApertureDesc) {
        fApertureDesc->release();
        fApertureDesc = NULL;
    }
    fApertureVA = NULL;
    fApertureSize = 0;

    /*
     * ปล่อย MMIO Map
     */
    if (fMMIOMap) {
        fMMIOMap->release();
        fMMIOMap = NULL;
    }
    if (fMMIODesc) {
        fMMIODesc->release();
        fMMIODesc = NULL;
    }
    fRegs = NULL;

    /*
     * ปล่อย PCI Device (retain ใน start)
     */
    if (fPCIDevice) {
        fPCIDevice->release();
        fPCIDevice = NULL;
    }

    /*
     * reset translation state
     */
    fTransCount = 0;
    fFakeGen = 0;

    /*
     * เรียก super (IOService stop)
     */
    super::stop(provider);

    IODebug("stop() — done");
}

#pragma mark -
#pragma mark - Static IOKit Metadata

/*
 * ─────────────────────────────────────────────────────────────────
 *  IOKit MetaClass Registration
 *
 *  จำเป็นสำหรับ Apple's IOKit runtime เพื่อสร้าง instance
 *  ของคลาสนี้ผ่าน factory method
 *
 *  OSDefineMetaClassAndStructors อยู่บนสุดของไฟล์
 *
 *  ถ้าต้องการ IOKit Matching Dictionary:
 *    ให้ผู้ใช้ใส่ IOPCIDevice match ใน Info.plist
 *    ระบุ IONameMatch หรือ IOPCIMatch key
 *
 *  Example Info.plist snippet:
 *
 *    <key>IOPCIMatch</key>
 *    <string>0x8086 0x4691</string>    // Alder Lake-P GT2
 *
 *    หรือ match หลาย ID:
 *    <string>0x8086 0x4680 0x8086 0x468b 0x8086 0x4691 0x8086 0x4692</string>
 * ─────────────────────────────────────────────────────────────────
 */

/*
 * ฟังก์ชันตรงนี้ (extern "C") สำหรับ IOKit library probe
 * ถ้าต้องการ custom IOService matching:
 *
 *   static MyIntelGPU *ourInstance = NULL;
 *
 *   MyIntelGPU *MyIntelGPU::probe(IOService *provider, SInt32 *score) {
 *       ...
 *   }
 *
 * แต่คลาสนี้ใช้ standard IOPCIDevice match ผ่าน Info.plist
 * ไม่ต้อง override probe()
 */
