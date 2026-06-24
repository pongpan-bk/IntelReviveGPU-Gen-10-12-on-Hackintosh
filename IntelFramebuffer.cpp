/*===========================================================================
 *  IntelFramebuffer.cpp
 *  Hackintosh Kext — Display Output & Interrupt Management Implementation
 *
 *  แกนหลัก:
 *    - Interrupt Initialization (clear → mask → enable)
 *    - Vblank Interrupt Registration ผ่าน IOInterruptEventSource
 *    - Pipe Management
 *
 *  ขั้นตอน Interrupt Lifecycle:
 *    1. initInterrupts()  — สั่ง Mask + Clear Register ก่อน
 *    2. installInterruptHandlers() — ลงทะเบียน IOInterruptEventSource
 *    3. enableVblankInterrupt() — เปิดเฉพาะ Vblank ใน IER
 *    4. handleInterrupt() — Callback เมื่อเกิด Interrupt
 *    5. disableInterrupts() — ปิด + ล้างตอน unload
 *
 *  อ้างอิง Linux i915:
 *    drivers/gpu/drm/i915/i915_irq.c — icl_irq_handler, gen8_irq_handler
 *    drivers/gpu/drm/i915/display/intel_display_power.c
 *///=========================================================================

#include "IntelFramebuffer.hpp"

/*
 * Debug Logging Macro — ใช้ IOLog เหมือน MyIntelGPU
 */
#define FBLog(fmt, ...) \
    IOLog("IntelFB: [%s:%d] " fmt "\n", __FUNCTION__, __LINE__, ##__VA_ARGS__)

// #define EXTRA_FB_DEBUG    /* เปิดเพื่อ log รายละเอียด interrupt */

/*
 * Register Macro สำหรับ IOKit Runtime
 */
#define super IOService
OSDefineMetaClassAndStructors(IntelFramebuffer, IOService)

#pragma mark -
#pragma mark - init / free

bool IntelFramebuffer::init(OSDictionary *dict)
{
    if (!super::init(dict)) {
        return false;
    }

    fParent              = NULL;
    fInterruptEventSource = NULL;
    fInterruptsReady     = false;
    fEnabledPipes        = 0;

    FBLog("init() — OK");
    return true;
}

void IntelFramebuffer::free()
{
    FBLog("free()");

    if (fInterruptEventSource) {
        fInterruptEventSource->release();
        fInterruptEventSource = NULL;
    }

    fParent = NULL;
    super::free();
}

#pragma mark -
#pragma mark - Interrupt Register Helpers

/*
 * ─────────────────────────────────────────────────────────────────
 *  uint32_t IntelFramebuffer::pipeBase(uint32_t pipe)
 *
 *  คำนวณ MMIO base address ของ Pipe ตาม Gen12+ layout:
 *    Pipe A: 0x70000
 *    Pipe B: 0x71000
 *    Pipe C: 0x72000
 *    Pipe D: 0x73000 (Gen12+ มี แต่ Coffee Lake ไม่มี)
 *
 *  ถ้า pipe >= FB_MAX_CRTC → return 0 (invalid)
 * ─────────────────────────────────────────────────────────────────
 */
uint32_t IntelFramebuffer::pipeBase(uint32_t pipe)
{
    if (pipe >= FB_MAX_CRTC) {
        return 0;
    }
    return PIPE_A_BASE + (pipe * 0x1000);
}

uint32_t IntelFramebuffer::getPipeIIR(uint32_t pipe)
{
    uint32_t base = pipeBase(pipe);
    if (base == 0) return 0xFFFFFFFF;
    return fParent->readReg32(base + PIPE_IIR_OFFSET);
}

uint32_t IntelFramebuffer::getPipeIER(uint32_t pipe)
{
    uint32_t base = pipeBase(pipe);
    if (base == 0) return 0xFFFFFFFF;
    return fParent->readReg32(base + PIPE_IER_OFFSET);
}

#pragma mark -
#pragma mark - Interrupt Clearing

/*
 * ─────────────────────────────────────────────────────────────────
 *  void IntelFramebuffer::clearAllInterruptRegisters(void)
 *
 *  ล้าง Interrupt ที่ค้างอยู่ทั้งหมด (ป้องกัน spurious interrupt
 *  หลัง enable master interrupt)
 *
 *  ขั้นตอน (อ้างอิง i915_irq.c — gen8_de_irq_postinstall):
 *    1. อ่าน + เขียน 1 ไปยัง GEN11_DE_MISC_IIR (clear misc)
 *    2. อ่าน + เขียน 1 ไปยัง PIPEA_IIR, PIPEB_IIR, PIPEC_IIR
 *       (clear pending pipe interrupts)
 *    3. อ่าน IIR ซ้ำเพื่อ confirm clear
 *    4. Mask IMR ทั้งหมดก่อน (กัน interrupt ระหว่าง setup)
 *
 *  Register Rule:
 *    IIR = write 1 to clear (W1C)
 *    IMR = bit = 1 means masked (disabled)
 *    IER = bit = 1 means enabled
 *
 *  การ clear IIR ต้อง:
 *    - อ่าน IIR ก่อน (return pending status)
 *    - เขียนกลับด้วยค่าที่อ่าน (write 1 to clear)
 *    - ทำซ้ำถ้า IIR ยังไม่เป็น 0 (pending interrupt ใหม่)
 *      (ตาม Gen HW spec — IIR clear loop)
 *
 *  Reference:
 *    i915_reg.h — GEN11_DE_MISC_IIR
 *    intel_uncore_write(uncore, GEN11_DE_MISC_IIR,
 *                       intel_uncore_read(uncore, GEN11_DE_MISC_IIR))
 *─────────────────────────────────────────────────────────────────
 */
void IntelFramebuffer::clearAllInterruptRegisters(void)
{
    FBLog("Clearing all pending display interrupts...");

    if (!fParent || !fParent->getRegs()) {
        FBLog("ERROR: parent or MMIO not ready");
        return;
    }

    /*
     * Step 1: Clear Misc Display Interrupts (GEN11_DE_MISC_IIR)
     */
    uint32_t miscIIR = fParent->readReg32(GEN11_DE_MISC_IIR);
    if (miscIIR != 0) {
        FBLog("  GEN11_DE_MISC_IIR = 0x%08X (clearing)", miscIIR);
        fParent->writeReg32(GEN11_DE_MISC_IIR, miscIIR);
        /*
         * Dummy read — flush PCI write buffer
         */
        (void)fParent->readReg32(GEN11_DE_MISC_IIR);
    }

    /*
     * Step 2: Mask All Misc Interrupts (IMR = all 1s = disabled)
     */
    fParent->writeReg32(GEN11_DE_MISC_IMR, 0xFFFFFFFF);

    /*
     * Step 3: Clear Pipe Interrupts (PIPEA/B/C_IIR)
     */
    for (uint32_t pipe = 0; pipe < FB_MAX_CRTC; pipe++) {
        uint32_t base = pipeBase(pipe);
        if (base == 0) continue;

        uint32_t iir = fParent->readReg32(base + PIPE_IIR_OFFSET);
        if (iir != 0) {
            FBLog("  Pipe %c IIR = 0x%08X (clearing)", 'A' + pipe, iir);
            fParent->writeReg32(base + PIPE_IIR_OFFSET, iir);
        }

        /*
         * Mask all interrupts on this pipe
         * IMR = 1 → disabled
         */
        fParent->writeReg32(base + PIPE_IMR_OFFSET, 0xFFFFFFFF);

        /*
         * Disable all interrupts on this pipe
         * IER = 0 → disabled
         */
        fParent->writeReg32(base + PIPE_IER_OFFSET, 0);
    }

    /*
     * Step 4: Disable Master Interrupt (defer enable until handler ready)
     */
    uint32_t masterCtl = fParent->readReg32(GEN11_DE_INTERRUPT_CONTROL);
    masterCtl &= ~GEN11_DE_MASTER_ENABLE;
    fParent->writeReg32(GEN11_DE_INTERRUPT_CONTROL, masterCtl);

    /*
     * Step 5: Disable Display Interrupt Control
     */
    fParent->writeReg32(DISP_INT_CTL, 0);

    FBLog("All display interrupts cleared and masked");
}

#pragma mark -
#pragma mark - Vblank Interrupt Enable/Disable

/*
 * ─────────────────────────────────────────────────────────────────
 *  void IntelFramebuffer::enableVblankInterrupt(uint32_t pipe)
 *
 *  เปิด Vblank Interrupt สำหรับ pipe ที่กำหนด
 *
 *  ขั้นตอน:
 *    1. ตรวจสอบ pipe index
 *    2. อ่าน IER ปัจจุบัน
 *    3. Set bit 0 (GEN11_PIPE_VBLANK)
 *    4. เขียน IER กลับ
 *    5. Unmask IMR (clear bit 0)
 *    6. ถ้ายังไม่เปิด Master Interrupt → เปิด
 *
 *  Pipe Vblank Register (per pipe):
 *    IER + 0x2A0: bit 0 = Vblank Interrupt Enable
 *    IMR + 0x2A8: bit 0 = 0 (unmasked) / 1 (masked)
 *
 *  Reference:
 *    i915_reg.h — GEN11_PIPE_VBLANK
 *    intel_display_power.c — intel_crtc_vblank_on
 *─────────────────────────────────────────────────────────────────
 */
void IntelFramebuffer::enableVblankInterrupt(uint32_t pipe)
{
    if (pipe >= FB_MAX_CRTC) {
        FBLog("ERROR: enableVblankInterrupt: invalid pipe %u", pipe);
        return;
    }

    uint32_t base = pipeBase(pipe);
    if (base == 0) return;

    FBLog("Enabling Vblank interrupt on Pipe %c", 'A' + pipe);

    /*
     * Unmask Vblank in IMR (clear bit 0)
     */
    uint32_t imr = fParent->readReg32(base + PIPE_IMR_OFFSET);
    imr &= ~GEN11_PIPE_VBLANK;
    fParent->writeReg32(base + PIPE_IMR_OFFSET, imr);

    /*
     * Enable Vblank in IER (set bit 0)
     */
    uint32_t ier = fParent->readReg32(base + PIPE_IER_OFFSET);
    ier |= GEN11_PIPE_VBLANK;
    fParent->writeReg32(base + PIPE_IER_OFFSET, ier);

    /*
     * Enable Master Display Interrupt
     */
    uint32_t masterCtl = fParent->readReg32(GEN11_DE_INTERRUPT_CONTROL);
    masterCtl |= GEN11_DE_MASTER_ENABLE;
    fParent->writeReg32(GEN11_DE_INTERRUPT_CONTROL, masterCtl);

    /*
     * Enable Display Interrupt Control
     */
    uint32_t dispCtl = fParent->readReg32(DISP_INT_CTL);
    dispCtl |= DISP_INT_ENABLE;
    fParent->writeReg32(DISP_INT_CTL, dispCtl);

    /*
     * Mark pipe as enabled
     */
    fEnabledPipes |= (1U << pipe);

    FBLog("Vblank enabled on Pipe %c (IER=0x%08X, IMR=0x%08X)",
          'A' + pipe,
          fParent->readReg32(base + PIPE_IER_OFFSET),
          fParent->readReg32(base + PIPE_IMR_OFFSET));

#ifdef EXTRA_FB_DEBUG
    FBLog("  MasterCtl=0x%08X, DispCtl=0x%08X",
          fParent->readReg32(GEN11_DE_INTERRUPT_CONTROL),
          fParent->readReg32(DISP_INT_CTL));
#endif
}

/*
 * ─────────────────────────────────────────────────────────────────
 *  void IntelFramebuffer::disableVblankInterrupt(uint32_t pipe)
 *
 *  ปิด Vblank Interrupt สำหรับ pipe
 *
 *  ขั้นตอน:
 *    1. Mask IMR bit 0
 *    2. Clear IER bit 0
 *    3. อ่าน IIR เพื่อ clear pending
 *─────────────────────────────────────────────────────────────────
 */
void IntelFramebuffer::disableVblankInterrupt(uint32_t pipe)
{
    if (pipe >= FB_MAX_CRTC) {
        return;
    }

    uint32_t base = pipeBase(pipe);
    if (base == 0) return;

    FBLog("Disabling Vblank interrupt on Pipe %c", 'A' + pipe);

    /*
     * Mask Vblank in IMR (set bit 0)
     */
    uint32_t imr = fParent->readReg32(base + PIPE_IMR_OFFSET);
    imr |= GEN11_PIPE_VBLANK;
    fParent->writeReg32(base + PIPE_IMR_OFFSET, imr);

    /*
     * Disable Vblank in IER (clear bit 0)
     */
    uint32_t ier = fParent->readReg32(base + PIPE_IER_OFFSET);
    ier &= ~GEN11_PIPE_VBLANK;
    fParent->writeReg32(base + PIPE_IER_OFFSET, ier);

    /*
     * Clear pending IIR
     */
    uint32_t iir = fParent->readReg32(base + PIPE_IIR_OFFSET);
    if (iir & GEN11_PIPE_VBLANK) {
        fParent->writeReg32(base + PIPE_IIR_OFFSET, GEN11_PIPE_VBLANK);
    }

    fEnabledPipes &= ~(1U << pipe);

    FBLog("Vblank disabled on Pipe %c", 'A' + pipe);
}

#pragma mark -
#pragma mark - initInterrupts

/*
 * ─────────────────────────────────────────────────────────────────
 *  bool IntelFramebuffer::initInterrupts(MyIntelGPU *parent)
 *
 *  จุดเริ่มต้นของระบบ Interrupt — ต้องเรียกก่อนทุกอย่าง!
 *
 *  Debug Note:
 *    ถ้าเครื่องค้าง (panic/hang) ตอน kext load:
 *      - ดู log ว่า "InitInterrupts: START" → "Clear OK" → ?
 *      - ถ้าค้างก่อน "Clear OK" → ปัญหา MMIO mapping
 *      - ถ้าค้างหลัง "Clear OK" ก่อน "Handlers registered"
 *        → ปัญหา IOInterruptEventSource::interruptEventSource()
 *      - ถ้าค้างหลัง "Handlers registered"
 *        → ปัญหา registerInterrupt() + spurious interrupt
 *          (อาจต้อง clear IIR อีกครั้งหลัง enable master)
 *
 *  Pipeline:
 *    1. ตรวจ parent และ MMIO validity
 *    2. clearAllInterruptRegisters() — ล้าง + Mask
 *    3. installInterruptHandlers() — ลงทะเบียน IOKit
 *    4. เปิด Master Interrupt พร้อมรับ
 *
 *  Reference:
 *    i915_irq.c — intel_irq_install, gen8_irq_postinstall
 *─────────────────────────────────────────────────────────────────
 */
bool IntelFramebuffer::initInterrupts(MyIntelGPU *parent)
{
    FBLog("=== InitInterrupts: START ===");

    if (!parent) {
        FBLog("ERROR: parent is NULL");
        return false;
    }

    fParent = parent;

    if (!fParent->getRegs()) {
        FBLog("ERROR: parent MMIO not mapped");
        return false;
    }

    /*
     * Step 1: Clear all pending interrupts immediately
     *         (ก่อน enable master — ป้องกัน spurious interrupt)
     */
    FBLog("  Step 1: Clearing pending interrupts...");
    clearAllInterruptRegisters();
    FBLog("  Step 1: Clear OK");

    /*
     * Step 2: Register interrupt handlers with IOKit
     */
    FBLog("  Step 2: Registering interrupt handlers...");
    if (!installInterruptHandlers()) {
        FBLog("ERROR: installInterruptHandlers failed");
        return false;
    }
    FBLog("  Step 2: Handlers registered");

    /*
     * Step 3: Confirm master interrupt ready
     *         (แต่ยังไม่เปิด per-pipe — รอให้ display mode set)
     */
    FBLog("  Step 3: Interrupt system ready (pipes masked)");

    fInterruptsReady = true;
    FBLog("=== InitInterrupts: DONE ===");
    return true;
}

#pragma mark -
#pragma mark - installInterruptHandlers

/*
 * ─────────────────────────────────────────────────────────────────
 *  bool IntelFramebuffer::installInterruptHandlers(void)
 *
 *  ลงทะเบียน Interrupt Handler กับ IOKit
 *
 *  ใช้ IOInterruptEventSource:
 *    - เป็น IOKit abstraction สำหรับ event-based interrupt
 *    - ทำงานคล้าย request_threaded_irq() ใน Linux
 *    - handler ทำงานใน workloop context (สามารถ sleep ได้)
 *
 *  ขั้นตอน:
 *    1. ตรวจ parent IOPCIDevice
 *    2. สร้าง IOInterruptEventSource::interruptEventSource()
 *    3. ลงทะเบียนกับ workloop
 *    4. เปิดรับ interrupt ด้วย enable()
 *
 *  ถ้าค้างที่ registerInterrupt() — ปัญหาที่พบบ่อย:
 *    - PCI device ไม่ config interrupt ไว้ (BIOS)
 *    - interrupt vector ขัดแย้ง (MSI/MSI-X)
 *    - IIR ค้าง → interrupt มาทันทีที่ enable → loop
 *
 *  ทางแก้:
 *    - ตรวจ clearAllInterruptRegisters() ซ้ำก่อน enable
 *    - ถ้ายังค้าง → ลอง disable MSI/X → ใช้ INTx
 *    - ดู boot-args: -irqdebug สำหรับ IOKit interrupt log
 *
 *  Reference:
 *    IOKit: IOInterruptEventSource.h
 *    i915_irq.c — intel_irq_install
 *─────────────────────────────────────────────────────────────────
 */
bool IntelFramebuffer::installInterruptHandlers(void)
{
    FBLog("installInterruptHandlers: START");

    if (!fParent) {
        FBLog("ERROR: no parent");
        return false;
    }

    IOPCIDevice *pciDev = fParent->getPCIDevice();
    if (!pciDev) {
        FBLog("ERROR: no PCI device from parent");
        return false;
    }

    /*
     * ตรวจสอบว่า PCI device supports interrupt
     */
    int intrType = 0;
    bool hasIntr = pciDev->getInterruptType(&intrType);
    FBLog("  PCI interrupt type = 0x%08X (has=%d)", intrType, (int)hasIntr);

    if (!hasIntr || intrType == 0) {
        FBLog("WARNING: No interrupt type detected — continuing anyway");
    }

    /*
     * สร้าง IOInterruptEventSource
     *
     * Parameters:
     *   owner   = this (IOService)
     *   handler = sInterruptHandler (static callback)
     *   provider = pciDev (IOPCIDevice)
     *   index   = 0 (interrupt source index, 0 = first)
     *
     * การใช้ static method + OSObject *target:
     *   — IOKit จะส่ง "this" กลับมาในฐานะ OSObject *
     *   — เรา static_cast กลับเป็น IntelFramebuffer *
     */
    fInterruptEventSource = IOInterruptEventSource::interruptEventSource(
                                this,
                                &IntelFramebuffer::sInterruptHandler,
                                pciDev,
                                0);

    if (!fInterruptEventSource) {
        FBLog("ERROR: Failed to create IOInterruptEventSource");

        /*
         * ลองด้วย interrupt index อื่น (MSI/X มักใช้ index 0
         * แต่ INTx legacy อาจเป็น index อื่น)
         */
        FBLog("  Retrying with interrupt index 1...");
        fInterruptEventSource = IOInterruptEventSource::interruptEventSource(
                                    this,
                                    &IntelFramebuffer::sInterruptHandler,
                                    pciDev,
                                    1);

        if (!fInterruptEventSource) {
            FBLog("ERROR: Still failed — no interrupt source available");
            return false;
        }
    }

    /*
     * เพิ่ม Event Source เข้า Workloop
     * (Workloop = IOKit thread ที่จัดการ event serialization)
     */
    IOWorkLoop *wl = getWorkLoop();
    if (!wl) {
        FBLog("ERROR: Cannot get workloop");
        fInterruptEventSource->release();
        fInterruptEventSource = NULL;
        return false;
    }

    if (wl->addEventSource(fInterruptEventSource) != kIOReturnSuccess) {
        FBLog("ERROR: Failed to add event source to workloop");
        fInterruptEventSource->release();
        fInterruptEventSource = NULL;
        return false;
    }

    /*
     * เปิด Interrupt Source
     * enable() → IOKit จะ unmask interrupt ที่ PCI level
     * แล้วเริ่มส่ง event มายัง handler ของเรา
     *
     * !! สำคัญ: ตรงนี้ถ้ามี IIR ค้าง → handler ถูกเรียกทันที !!
     * ดังนั้น clearAllInterruptRegisters() ต้องถูกเรียกก่อน
     */
    fInterruptEventSource->enable();

    FBLog("installInterruptHandlers: DONE — interrupts enabled");
    return true;
}

#pragma mark -
#pragma mark - disableInterrupts

/*
 * ─────────────────────────────────────────────────────────────────
 *  void IntelFramebuffer::disableInterrupts(void)
 *
 *  ปิดระบบ Interrupt ทั้งหมด — เรียกตอน kext unload / sleep
 *
 *  Pipeline:
 *    1. disable IOInterruptEventSource
 *    2. ปิด Master Interrupt Control
 *    3. ล้าง IIR ทั้งหมด
 *    4. release event source
 *─────────────────────────────────────────────────────────────────
 */
void IntelFramebuffer::disableInterrupts(void)
{
    FBLog("disableInterrupts: START");

    fInterruptsReady = false;

    /*
     * Disable IOInterruptEventSource ก่อน
     */
    if (fInterruptEventSource) {
        fInterruptEventSource->disable();

        IOWorkLoop *wl = getWorkLoop();
        if (wl) {
            wl->removeEventSource(fInterruptEventSource);
        }

        fInterruptEventSource->release();
        fInterruptEventSource = NULL;
    }

    /*
     * Clear + Mask all interrupts again (safety)
     */
    clearAllInterruptRegisters();

    fEnabledPipes = 0;
    FBLog("disableInterrupts: DONE");
}

#pragma mark -
#pragma mark - Interrupt Handler

/*
 * ─────────────────────────────────────────────────────────────────
 *  void IntelFramebuffer::sInterruptHandler(OSObject *target,
 *                                           IOInterruptEventSource *sender)
 *
 *  Static callback — IOKit เรียกเมื่อมี interrupt
 *  ส่งต่อให้ handleInterrupt() ผ่าน OSDynamicCast
 *─────────────────────────────────────────────────────────────────
 */
void IntelFramebuffer::sInterruptHandler(OSObject *target,
                                         IOInterruptEventSource *sender,
                                         int count)
{
    IntelFramebuffer *fb = OSDynamicCast(IntelFramebuffer, target);
    if (fb) {
        fb->handleInterrupt();
    }
}

/*
 * ─────────────────────────────────────────────────────────────────
 *  void IntelFramebuffer::handleInterrupt(void)
 *
 *  MAIN INTERRUPT HANDLER — ตรวจสอบว่า interrupt มาจาก Display Engine
 *  และ dispatch ไปยัง Pipe ต่างๆ
 *
 *  ขั้นตอน (อ้างอิง gen8_de_irq_handler ใน i915_irq.c):
 *    1. อ่าน GEN11_DE_MISC_IIR → ดูว่า misc interrupt อะไร
 *    2. สำหรับแต่ละ pipe ที่ enabled:
 *       a. อ่าน PIPEA_IIR
 *       b. เช็ค GEN11_PIPE_VBLANK bit
 *       c. ถ้ามี → handle vblank (store timestamp, frame counter)
 *       d. เขียน IIR เพื่อ clear (write 1 to clear)
 *    3. loop จนกว่า IIR ทุก pipe เป็น 0
 *
 *  ถ้า interrupt นี้ไม่ใช่ของ display → return
 *  (ไม่ต้อง clear — CPU จะ retry จนกว่าจะเจอ owner ที่ถูกต้อง)
 *
 *  Reference:
 *    i915_irq.c — gen8_de_irq_handler()
 *    IOLog อยู่ใน EXTRA_FB_DEBUG เพื่อไม่ให้ spam log
 *─────────────────────────────────────────────────────────────────
 */
void IntelFramebuffer::handleInterrupt(void)
{
    IOLog("MyIntelGPU: IRQ\n");

    if (!fInterruptsReady) {
        return;
    }

    /*
     * ตรวจสอบว่านี่คือ Display Engine Interrupt จริงหรือไม่
     * โดยการอ่าน GEN11_DE_MISC_IIR:
     *   ถ้า != 0 → display interrupt (we handle it)
     *   ถ้า == 0 → spurious หรือ interrupt จาก engine อื่น
     */
    uint32_t miscIIR = fParent->readReg32(GEN11_DE_MISC_IIR);

    if (miscIIR == 0) {
        /*
         * ไม่ใช่ display interrupt — อาจเป็น GT interrupt
         * หรือ spurious → return โดยไม่ clear
         * (IOKit จะ retry จนกว่าจะมีใคร clear)
         */
        return;
    }

    /*
     * Clear Misc IIR ก่อน (เพื่อให้ interrupt นี้ไม่ถูกส่งซ้ำ)
     */
    fParent->writeReg32(GEN11_DE_MISC_IIR, miscIIR);

#ifdef EXTRA_FB_DEBUG
    FBLog("handleInterrupt: DE_MISC_IIR = 0x%08X", miscIIR);
#endif

    /*
     * ตรวจสอบแต่ละ pipe ว่ามี Vblank interrupt หรือไม่
     */
    for (uint32_t pipe = 0; pipe < FB_MAX_CRTC; pipe++) {
        if (!(fEnabledPipes & (1U << pipe))) {
            continue;   /* pipe นี้ไม่ได้เปิด interrupt */
        }

        uint32_t base = pipeBase(pipe);
        if (base == 0) continue;

        uint32_t iir = fParent->readReg32(base + PIPE_IIR_OFFSET);

        if (iir == 0) {
            continue;   /* ไม่มี interrupt สำหรับ pipe นี้ */
        }

        /*
         * Clear IIR — เขียนค่าที่อ่านกลับ (W1C semantics)
         */
        fParent->writeReg32(base + PIPE_IIR_OFFSET, iir);

#ifdef EXTRA_FB_DEBUG
        if (iir & GEN11_PIPE_VBLANK) {
            FBLog("  VBLANK on Pipe %c (IIR=0x%08X)", 'A' + pipe, iir);
        }
        if (iir & GEN11_PIPE_FIFO_UNDERRUN) {
            FBLog("  FIFO UNDERRUN on Pipe %c!", 'A' + pipe);
        }
#endif

        /*
         * TODO Phase 4: IOFramebuffer Binding
         * — แจ้ง IOFramebuffer ว่ามี Vblank
         * — อัปเดต frame counter / timestamp
         * — ส่ง event ไปยัง user space (VSync)
         */
    }

    /*
     * Loop safety check — ถ้า IIR ยังไม่ clear
     * ต้องวนซ้ำ (i915 ทำ loop สูงสุด ~10 ครั้ง)
     */
    uint32_t sanity = 10;
    while (sanity--) {
        /*
         * Re-check master IIR
         */
        uint32_t checkIIR = fParent->readReg32(GEN11_DE_MISC_IIR);
        if (checkIIR == 0) break;

        fParent->writeReg32(GEN11_DE_MISC_IIR, checkIIR);

#ifdef EXTRA_FB_DEBUG
        FBLog("  Loop clear: IIR=0x%08X (remaining=%u)", checkIIR, sanity);
#endif
    }
}
