/*===========================================================================
 *  IntelFramebuffer.hpp
 *  Hackintosh Kext — Display Output & Interrupt Management
 *
 *  คลาสสำหรับจัดการ Display Pipeline, Vblank Interrupt, และ
 *  Framebuffer Output โดยเฉพาะ แยกออกจาก MyIntelGPU หลัก
 *  เพื่อลดความซับซ้อนและแยก responsibility
 *
 *  รับ MyIntelGPU* เป็น parent เพื่อเข้าถึง MMIO (ผ่าน getRegs(),
 *  getMMIOMap(), readReg32/writeReg32) และ IOPCIDevice
 *  สำหรับลงทะเบียน Interrupt
 *
 *  อ้างอิงโครงสร้างจาก Linux i915:
 *    drivers/gpu/drm/i915/display/intel_display.c
 *    drivers/gpu/drm/i915/i915_irq.c
 *///=========================================================================

#ifndef __INTEL_FRAMEBUFFER_HPP__
#define __INTEL_FRAMEBUFFER_HPP__

#include <IOKit/IOService.h>
#include <IOKit/IOInterruptEventSource.h>
#include "MyIntelGPU.hpp"

#ifndef FB_MAX_CRTC
#define FB_MAX_CRTC 3
#endif

/*
 * ─────────────────────────────────────────────
 *  Gen12+ Display Interrupt Register Offsets
 * ─────────────────────────────────────────────
 *  อ้างอิง: i915_reg.h — GEN11_DE_INTERRUPT_*, PIPEA_*
 *
 *  Master Control:
 *    GEN11_DE_INTERRUPT_CONTROL (0x44300):
 *      bit 31: Display Engine Interrupt Master Enable
 *
 *  Display Interrupt Control:
 *    DISP_INT_CTL (0x44200):
 *      bit 0: Enable Display Interrupts
 *
 *  Misc Display Interrupts:
 *    GEN11_DE_MISC_IER (0x44468): Interrupt Enable
 *    GEN11_DE_MISC_IIR (0x4446C): Interrupt Identity (write 1 to clear)
 *    GEN11_DE_MISC_IMR (0x44470): Interrupt Mask
 *
 *  Pipe Interrupts (per pipe):
 *    PIPEA_IER = pipe_a_base + 0x2A0
 *    PIPEA_IIR = pipe_a_base + 0x2A4 (write 1 to clear)
 *    PIPEA_IMR = pipe_a_base + 0x2A8
 *
 *    Vblank interrupt bit: bit 0 (GEN11_PIPE_VBLANK)
 *─────────────────────────────────────────────
 */

#define GEN11_DE_INTERRUPT_CONTROL      0x44300
#define GEN11_DE_MASTER_ENABLE          (1U << 31)

#define DISP_INT_CTL                    0x44200
#define DISP_INT_ENABLE                 (1U << 0)

#define GEN11_DE_MISC_IER               0x44468
#define GEN11_DE_MISC_IIR               0x4446C
#define GEN11_DE_MISC_IMR               0x44470

#define PIPE_IIR_OFFSET                 0x2A4
#define PIPE_IER_OFFSET                 0x2A0
#define PIPE_IMR_OFFSET                 0x2A8

#define GEN11_PIPE_VBLANK               (1U << 0)
#define GEN11_PIPE_EOF                  (1U << 1)
#define GEN11_PIPE_PSR_STATUS           (1U << 2)
#define GEN11_PIPE_FIFO_UNDERRUN        (1U << 8)

/*
 * ─────────────────────────────────────────────
 *  IntelFramebuffer Class
 * ─────────────────────────────────────────────
 *
 *  รับผิดชอบ:
 *    1. initInterrupts() — ล้าง Interrupt ค้าง + Mask
 *    2. installInterruptHandlers() — ลงทะเบียน IOInterruptEventSource
 *    3. Vblank Interrupt Handling
 *    4. Display Pipe Enable/Disable
 *
 *  ไม่ใช่ IOService จริง — เป็น helper class ที่รับ MyIntelGPU*
 *  เพื่อ access MMIO + PCI device ผูกกับ event source ของ parent
 *─────────────────────────────────────────────
 */

class IntelFramebuffer : public IOService {
    OSDeclareDefaultStructors(IntelFramebuffer)

public:

    virtual bool init(OSDictionary *dict) override;
    virtual void free() override;

    /*
     * ─────────────────────────────────────
     *  Setup Methods (เรียกจาก parent start())
     * ─────────────────────────────────────
     */

    /*!
     * @brief  กำหนด Parent + เริ่มต้น Interrupt State
     *
     *  ต้องเรียกก่อน installInterruptHandlers()
     *  ทำงาน: ล้าง Interrupt Register, Mask ทั้งหมด
     *
     * @param parent  MyIntelGPU instance ที่เป็นเจ้าของเรา
     * @return true = success
     */
    bool initInterrupts(MyIntelGPU *parent);

    /*!
     * @brief  ลงทะเบียน Interrupt Handler กับ IOKit
     *
     *  สร้าง IOInterruptEventSource ผูกกับ fPCIDevice
     *  เพื่อรับ Vblank และ Display Interrupt
     *
     * @return true = success
     */
    bool installInterruptHandlers(void);

    /*!
     * @brief  ปิดการทำงาน Interrupt ทั้งหมด + ล้างค้าง
     *
     *  เรียกตอน stop() หรือ sleep
     */
    void disableInterrupts(void);

    /*!
     * @brief  เปิด Vblank Interrupt สำหรับ Pipe ที่กำหนด
     *
     * @param pipe  Pipe index (0=A, 1=B, 2=C)
     */
    void enableVblankInterrupt(uint32_t pipe);

    /*!
     * @brief  ปิด Vblank Interrupt สำหรับ Pipe ที่กำหนด
     *
     * @param pipe  Pipe index
     */
    void disableVblankInterrupt(uint32_t pipe);

    /*
     * ─────────────────────────────────────
     *  Accessors
     * ─────────────────────────────────────
     */
    MyIntelGPU *getParent(void) const { return fParent; }
    bool isInterruptsReady(void) const { return fInterruptsReady; }

private:

    /* Parent */
    MyIntelGPU             *fParent;

    /* Interrupt State */
    IOInterruptEventSource *fInterruptEventSource;
    bool                    fInterruptsReady;
    uint32_t                fEnabledPipes;

    /*
     * ─────────────────────────────────────
     *  Internal Helpers
     * ─────────────────────────────────────
     */

    /* ล้าง Interrupt Status Register ทั้งหมด (IIR) */
    void clearAllInterruptRegisters(void);

    /* อ่านค่า Interrupt Status Register ของ Pipe */
    uint32_t getPipeIIR(uint32_t pipe);
    uint32_t getPipeIER(uint32_t pipe);

    /* คำนวณ MMIO Base ของ Pipe */
    static uint32_t pipeBase(uint32_t pipe);

    /*
     * ─────────────────────────────────────
     *  Interrupt Handler (static callback)
     * ─────────────────────────────────────
     */
    static void sInterruptHandler(OSObject *target, IOInterruptEventSource *sender, int count);
    void handleInterrupt(void);
};

#endif /* __INTEL_FRAMEBUFFER_HPP__ */
