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
 *  PCI Device ID Ranges — ใช้แยกเจนของฮาร์ดแวร์จริง
 * ─────────────────────────────────────────────
 *  Coffee Lake (เป้าหมายปลอมที่ macOS จะคิดว่าเป็น)
 *    0x3E9x (U14/U15 GT2), 0x9BCx (S GT3)
 *
 *  Alder Lake (Gen12 Xe_HPG)
 *    0x468x-0x469x = ADL-S (desktop)
 *    0x46Ax-0x46Bx = ADL-P (mobile)
 *    0x46Cx-0x46Dx = ADL-N (N-series, no PCH)
 *
 *  Raptor Lake (Gen12.2 Xe_HPG — same IP as ADL)
 *    0xA7Ax = RPL-P (mobile)
 *    0xA7Bx = RPL-HX (55W)
 *    0xA7Cx = RPL-U (15W)
 *
 *  Rocket Lake (Gen12 Xe_HPG)
 *    0x4C8x = RKL-S
 *
 *  Meteor Lake (Gen12.70 Xe LPG) — ⚠️ ยังไม่รองรับ Pass-through
 *    0x7D0x-0x7D6x = MTL-P/M/H
 *
 *  Arrow Lake (Gen13 Xe2) — ⚠️ ยังไม่รองรับ Pass-through
 *    0xB0xx = ARL
 * ─────────────────────────────────────────────
 */

/* Alder Lake examples */
#define ADL_P_GT2_DEVICE_ID    0x4691  /* Alder Lake-P GT2 (จริง) */
#define ADL_S_GT2_DEVICE_ID    0x4692  /* Alder Lake-S GT2 (จริง) */
#define ADL_N_GT1_DEVICE_ID    0x46D0  /* Alder Lake-N GT1 (จริง) */

/* Raptor Lake examples */
#define RPL_P_DEVICE_ID        0xA7A0  /* Raptor Lake-P (จริง) */
#define RPL_HX_DEVICE_ID       0xA7B0  /* Raptor Lake-HX (จริง) */

/* Coffee Lake (เป้าหมายปลอม) */
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
 *  NOTE: VCS1/VCS2/CCS0 มีเฉพาะ Gen12+
 *        macOS CFL driver ไม่รู้จัก engines เหล่านี้
 *        translation entries ถูกปิดไว้ (disabled=true)
 *        เพื่อให้ driver ในอนาคต (AppleIntelTGLGraphics+) ใช้งานได้
 *
 *  อ้างอิง: intel_engine_cs.c __engine_mmio_base()
 *           i915_pci.c  ENGINE_*_MMIO_BASE macros
 * ──────────────────────────────────────────
 */
#define RCS0_BASE_REAL      0x02000    /* Render — เหมือนกันทุกเจน */
#define RCS0_BASE_FAKE      0x02000

#define VCS0_BASE_REAL      0x1C0000   /* Video Decode Gen12+ (จริง) */
#define VCS0_BASE_FAKE      0x12000    /* Coffee Lake (ปลอม) */

#define VCS1_BASE_REAL      0x1C4000   /* Video Decode 2 Gen12+ (จริง) */
#define VCS1_BASE_FAKE      0x12000    /* ไม่มีใน CFL — fallback ส่งตรง */

#define VCS2_BASE_REAL      0x1D0000   /* Video Decode 3 Gen12+ (จริง) */
#define VCS2_BASE_FAKE      0x12000    /* ไม่มีใน CFL — fallback ส่งตรง */

#define BCS0_BASE_REAL      0x22000    /* Blitter — เหมือนกัน */
#define BCS0_BASE_FAKE      0x22000

#define VECS0_BASE_REAL     0x1C8000   /* Video Encode Gen12+ (จริง) */
#define VECS0_BASE_FAKE     0x1A000    /* Coffee Lake (ปลอม) */

#define CCS0_BASE_REAL      0x1A000    /* Compute Engine Gen12+ (จริง) */
#define CCS0_BASE_FAKE      0x02000    /* ไม่มีใน CFL — fallback ส่งตรง */

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
 * ─────────────────────────────────────────────
 *  Display Register Bases — CFL (Gen9) vs RPL (Gen12.2)
 * ─────────────────────────────────────────────
 *  Pipe/Plane registers:
 *    Pipe A: 0x70000 (same on both)
 *    Pipe B: 0x71000 (same)
 *    Pipe C: 0x72000 (same)
 *
 *  Transcoder registers:
 *    TRANS_A: 0x60000 (same)
 *    TRANS_B: 0x61000 (same)
 *    TRANS_C: 0x62000 (same)
 *
 *  DDI Buffer registers:
 *    DDI A: 0x64000 (same)
 *    DDI B: 0x64100 (same)
 *
 *  PCH (South Display) — ต่างกัน!
 *    CFL:  PCH registers at 0xCxxx  (I/O space)
 *    RPL:  PCH registers at 0xCxxxx (MMIO space, different base)
 *
 *  Reference: intel_display_regs.h, i915_reg.h
 * ─────────────────────────────────────────────
 */
#define TRANSCODER_A_BASE    0x60000
#define TRANSCODER_B_BASE    0x61000
#define TRANSCODER_C_BASE    0x62000

#define PIPE_A_BASE          0x70000
#define PIPE_B_BASE          0x71000
#define PIPE_C_BASE          0x72000

/* Plane registers (per pipe) */
#define PLANE_A_BASE         0x70100   /* Pipe A primary plane */
#define PLANE_B_BASE         0x71100   /* Pipe B primary plane */
#define PLANE_C_BASE         0x72100   /* Pipe C primary plane */

/* DDI (Digital Display Interface) */
#define DDI_A_BASE           0x64000
#define DDI_B_BASE           0x64100
#define DDI_C_BASE           0x64200

/*
 * ─────────────────────────────────────────────
 *  Pipe/Plane/Transcoder Register Sub-Offsets
 * ─────────────────────────────────────────────
 *  Pipe block (0x70000 + 0x1000 * pipe):
 *    +0x1C = PIPE_SRCSZ (source size, Gen12+)
 *    +0x24 = PIPE_STATUS
 *    +0x244 = PIPECONF (pipe config)
 *
 *  Primary Plane block (pipe_base + 0x100):
 *    +0x00 = PLANE_CTL
 *    +0x04 = PLANE_SIZE  [(height-1)<<16 | (width-1)]
 *    +0x08 = PLANE_STRIDE (bytes per line)
 *    +0x0C = PLANE_SURF  (GGTT page index)
 *
 *  Transcoder block (0x60000 + 0x1000 * trans):
 *    +0x1C = TRANS_CONF
 *    +0x100 = TRANS_DDI_FUNC_CTL
 *
 *  DDI block (0x64000 + 0x100 * port):
 *    +0x00 = DDI_BUF_CTL
 *
 *  Reference: i915_reg.h, intel_display.c
 * ─────────────────────────────────────────────
 */
#define PIPECONF_REG            0x244
#define PIPE_SRCSZ_REG          0x1C

#define PLANE_CTL_REG           0x00
#define PLANE_SIZE_REG          0x04
#define PLANE_STRIDE_REG        0x08
#define PLANE_SURF_REG          0x0C

#define TRANS_CONF_REG          0x1C
#define TRANS_DDI_FUNC_CTL_REG  0x100

/* Transcoder Timing Register Offsets (Gen12+) */
#define TRANS_HTOTAL_REG        0x00
#define TRANS_HSYNC_REG         0x04
#define TRANS_VTOTAL_REG        0x08
#define TRANS_VSYNC_REG         0x0C
#define TRANS_VBLANK_REG        0x10
#define TRANS_HBLANK_REG        0x14

#define DDI_BUF_CTL_REG         0x00
#define DDI_AUX_CTL_REG         0x0C
#define DDI_AUX_DATA_BASE       0x10   /* 4 x 32-bit data registers */
#define DP_TP_CTL_REG           0x40
#define DP_TP_STATUS_REG        0x44

/* DDI AUX Control bits */
#define DDI_AUX_CTL_SEND        (1U << 31)
#define DDI_AUX_CTL_DONE        (1U << 30)
#define DDI_AUX_CTL_TIMEOUT     (1U << 28)
#define DDI_AUX_CTL_ADDR_SHIFT  24
#define DDI_AUX_CTL_ADDR(x)     (((x) & 0x7) << DDI_AUX_CTL_ADDR_SHIFT)
#define DDI_AUX_CTL_TIMER_SHIFT 8
#define DDI_AUX_CTL_TIMER_400US (0x3 << DDI_AUX_CTL_TIMER_SHIFT)
#define DDI_AUX_CTL_MSG_SIZE(x) ((x) & 0xF)

/* DP Transport Control bits */
#define DP_TP_CTL_ENABLE        (1U << 31)
#define DP_TP_CTL_MODE_SST      (0x0 << 27)
#define DP_TP_CTL_MODE_MST      (0x1 << 27)
#define DP_TP_CTL_ENHANCED_FRAME (1 << 18)
#define DP_TP_CTL_FEC_ENABLE    (1U << 30)
#define DP_TP_CTL_LINK_TRAIN_PAT1  (0x0 << 8)
#define DP_TP_CTL_LINK_TRAIN_PAT2  (0x1 << 8)
#define DP_TP_CTL_LINK_TRAIN_IDLE  (0x2 << 8)
#define DP_TP_CTL_LINK_TRAIN_NORMAL (0x3 << 8)

/* EDID / DPCD constants */
#define EDID_BLOCK_SIZE         128
#define EDID_I2C_ADDR           0x50
#define DDC_SEGMENT_ADDR        0x30

/* Pipe config bits */
#define PIPECONF_ENABLE         (1U << 31)
#define PIPECONF_BPC_8          (0x0 << 5)
#define PIPECONF_BPC_10         (0x1 << 5)
#define PIPECONF_DITHER_EN      (1 << 4)

/* Primary plane control bits (Gen12+ TGL) */
#define PLANE_CTL_ENABLE        (1U << 31)
#define PLANE_CTL_FORMAT_XRGB8888  (0x4 << 24)
#define PLANE_CTL_FORMAT_XBGR8888  (0x5 << 24)
#define PLANE_CTL_FORMAT_ARGB8888  (0x6 << 24)
#define PLANE_CTL_FORMAT_ABGR8888  (0x7 << 24)
#define PLANE_CTL_TILING_LINEAR (0x0 << 21)
#define PLANE_CTL_TILING_X      (0x1 << 21)
#define PLANE_CTL_TILING_Y      (0x3 << 21)
#define PLANE_CTL_BPC_8         (0x0 << 8)
#define PLANE_CTL_BPC_10        (0x1 << 8)
#define PLANE_CTL_ALPHA_DISABLE (0x0 << 4)

/* Transcoder config bits */
#define TRANS_CONF_ENABLE       (1U << 31)
#define TRANS_CONF_PROGRESSIVE  0x0

/* Transcoder DDI Function Control bits */
#define TRANS_DDI_FUNC_ENABLE   (1U << 31)
#define TRANS_DDI_MODE_DP_SST   (0x2 << 24)
#define TRANS_DDI_MODE_HDMI     (0x0 << 24)
#define TRANS_DDI_BPC_8         (0x0 << 20)
#define TRANS_DDI_BPC_10        (0x1 << 20)
#define TRANS_DDI_PORT_WIDTH_X1 (0x0 << 17)
#define TRANS_DDI_PORT_WIDTH_X2 (0x1 << 17)
#define TRANS_DDI_PORT_WIDTH_X4 (0x3 << 17)
#define TRANS_DDI_PORT_A        (0x0 << 8)
#define TRANS_DDI_PORT_B        (0x1 << 8)
#define TRANS_DDI_SYNC_HSYNC    (1 << 4)   /* !invert => active low */
#define TRANS_DDI_SYNC_VSYNC    (1 << 3)

/* DDI Buffer Control bits */
#define DDI_BUF_CTL_ENABLE      (1U << 31)
#define DDI_BUF_CTL_ENABLE_VAL  (1U << 30)
#define DDI_BUF_CTL_PORT_WIDTH_X1 (0x0 << 8)
#define DDI_BUF_CTL_PORT_WIDTH_X2 (0x1 << 8)
#define DDI_BUF_CTL_PORT_WIDTH_X4 (0x3 << 8)

/* eDP Panel Power Sequencing — same on both */
#define PP_CONTROL           0xC50
#define PP_ON_DELAYS         0xC54
#define PP_OFF_DELAYS        0xC58
#define PP_DIVISOR           0xC60
#define PP_STATUS            0xC64

/* Backlight — ต่างกัน!
 *  CFL:  0x48250 (PCH backlight)
 *  RPL:  0xC8250 (different offset in display MMIO)
 */
#define CFL_BLC_PWM_CTL      0x48250   /* Coffee Lake backlight */
#define RPL_BLC_PWM_CTL      0xC8250   /* Raptor Lake backlight */
#define BLC_PWM_DUTY_OFF      0x4      /* Duty cycle relative to CTL */
#define BLC_PWM_PERIOD_OFF    0x8      /* Period relative to CTL */
#define BLC_PWM_CTL_ENABLE    (1U << 31)
#define BLC_PWM_CTL_POLARITY  (1U << 29)

/*
 *  CDCLK Control — ต่างกัน
 *  CFL:  CDCLK_CTL = 0x130000
 *  RPL:  CDCLK_CTL = 0x130000 (same base, but bit layout differs)
 */
#define CDCLK_CTL            0x130000
#define CDCLK_FREQ_SEL_MASK  0x00000180

/* DPLL — completely different architecture */
/* CFL: DPLL_CRTL1, LCPLL1_CTL */
/* RPL: uses TGL+ DPLL registers */
#define DPLL0_CFGCR0         0x164000   /* RPL/ADL DPLL0 config */
#define DPLL0_CFGCR1         0x164004
#define DPLL0_ENABLE         0x164100

/*
 * PCH (South Display) translation
 *  CFL PCH display registersอยู่ที่ 0x48000-0x48FFF
 *  RPL PCH display registersอยู่ที่ 0xC8000-0xC8FFF
 */
#define PCH_DISPLAY_BASE_FAKE  0x48000
#define PCH_DISPLAY_BASE_REAL  0xC8000
#define PCH_DISPLAY_WINDOW     0x1000

/*
 * Register Offsets อื่น ๆ ที่สำคัญ
 */
#define GMD_ID_GRAPHICS     0xD8C      /* Graphics Media Device ID register
                                          (มีเฉพาะ Gen12+ / Meteor Lake)
                                          read32(0xD8C) → ver/release */
#define GMD_ID_DISPLAY      0x510A0    /* Display IP version (MTL+)
                                          อ่านเพื่อ verify display gen */

#define ENGINE_TAIL_REG      0x80      /* Ring Tail Register offset
                                          (สัมพัทธ์จาก engine base) */
#define ENGINE_HEAD_REG      0x34      /* Ring Head Register offset */
#define ENGINE_CTL_REG       0x3C      /* Ring Control Register offset */
#define ENGINE_START_REG     0x38      /* Ring Start (Base Address) */

#define GFX_FLSH_CNTL_GEN6  0x10100   /* GGTT TLB Invalidate Register
                                            = write 1 → flush */

/*
 * ─────────────────────────────────────────────
 *  Phase 3 — GT Interrupts, Vblank, Hotplug
 * ─────────────────────────────────────────────
 */

/* GT Engine Interrupt Registers (Gen11+) */
#define GEN11_GT_INTR_DW0      0x44074   /* GT Interrupt DW0 (shared) */
#define GEN11_GT_INTR_DW1      0x44078   /* GT Interrupt DW1 */
#define GEN11_GT_INTR_DW2      0x4407C   /* GT Interrupt DW2 */
#define GEN11_GT_INTR_MASK0    0x440A4   /* GT Interrupt Mask DW0 */
#define GEN11_GT_INTR_MASK1    0x440A8   /* GT Interrupt Mask DW1 */
#define GEN11_GT_INTR_MASK2    0x440AC   /* GT Interrupt Mask DW2 */
#define GEN11_RENDER_INTR_ID   0x44008   /* Render CS interrupt ID */
#define GEN11_VCS0_INTR_ID     0x44010   /* VCS0 interrupt ID */
#define GEN11_VECS0_INTR_ID    0x44018   /* VECS0 interrupt ID */
#define GEN11_BCS_INTR_ID      0x44020   /* BCS interrupt ID */
#define GEN11_VCS1_INTR_ID     0x44028   /* VCS1 (Gen12+) */
#define GEN11_VCS2_INTR_ID     0x44030   /* VCS2 (Gen12+) */

/* Display Interrupt Registers (Gen12+) */
#define GEN12_DISPLAY_INT_CTL  0x44200   /* Display interrupt control */
#define GEN12_DISPLAY_INT_MASK 0x44204   /* Display interrupt mask */
#define GEN12_DISPLAY_INT_ID   0x44208   /* Display interrupt identity */

#define DISPLAY_INT_CTL_ENABLE (1U << 31)
#define DISPLAY_INT_VBLANK_A   (1 << 0)
#define DISPLAY_INT_VBLANK_B   (1 << 1)
#define DISPLAY_INT_VBLANK_C   (1 << 2)
#define DISPLAY_INT_HOTPLUG_DDI_A (1 << 16)
#define DISPLAY_INT_HOTPLUG_DDI_B (1 << 17)
#define DISPLAY_INT_HOTPLUG_DDI_C (1 << 18)
#define DISPLAY_INT_HOTPLUG_MASK  0x00070000

/* Pipe Scanline / Frame Count Registers (Gen12+) */
#define PIPE_DSL_REG             0x00   /* Display Scan Line */
#define PIPE_FRMCOUNT_REG        0x04   /* Frame Counter */
#define PIPE_FLIPCOUNT_MASK      0x3F000000

/* Hotplug Detect / Status */
#define SHOTPLUG_CTL             0x44410  /* Gen12+ shotplug control */
#define SHOTPLUG_DDI_A_DET      (1 << 0)
#define SHOTPLUG_DDI_B_DET      (1 << 1)
#define SHOTPLUG_DDI_C_DET      (1 << 2)
#define SHOTPLUG_DDI_A_EN       (1 << 8)
#define SHOTPLUG_DDI_B_EN       (1 << 9)
#define SHOTPLUG_DDI_C_EN       (1 << 10)
#define SHOTPLUG_STATUS          0x44414  /* HPD status */
#define SHOTPLUG_INT_EN          0x44418  /* HPD interrupt enable */

/* IIR / IER (Interrupt Identity/Enable Register) on display engine */
#define GEN12_PIPEA_IIR          0x70010  /* Pipe A IIR */
#define GEN12_PIPEB_IIR          0x71010  /* Pipe B IIR */
#define GEN12_PIPEC_IIR          0x72010  /* Pipe C IIR */
#define PIPE_IIR_VBLANK          (1 << 0)
#define PIPE_IIR_HOTPLUG         (1 << 16)
#define PIPE_IIR_FIFO_UNDERRUN   (1 << 19)
#define PIPE_IIR_ASYNC_FLIP      (1 << 21)
#define PIPE_IIR_FLIP_DONE       (1 << 22)
#define PIPE_IIR_MASK            0x00FF00FF

/* Framebuffer Interface Constants */
#define FB_MAX_CRTC              3
#define FB_INDEX_PRIMARY         0
#define FB_INDEX_EXTERNAL1       1
#define FB_INDEX_EXTERNAL2       2
                                          
/*
 * ─────────────────────────────────────────────
 *  GGTT (Graphics Translation Table) Registers
 * ─────────────────────────────────────────────
 *  GTT_SIZE (0x100C): ระบุจำนวน GTT entries
 *    Gen6/7: num_entries = (reg & 0xFFFF) + 1
 *    Gen8+:  num_entries = 2 * (reg & 0xFFFF)
 *
 *  GTT PTE Array (ภายใน BAR0 / GTTMMADR):
 *    Gen8+: GTT base = 0x800000
 *    แต่ละ PTE = 8 bytes (64-bit, Gen8+)
 *    รองรับสูงสุด 512 จีบี aperture
 *
 *  Reference: i915_gem_gtt.c, intel_ggtt.c
 * ─────────────────────────────────────────────
 */
#define GEN6_GTT_SIZE_REG   0x100C      /* GTT size register */
#define GEN8_GTT_BASE       0x800000    /* GTT PTE array offset in BAR0 (Gen8+) */
#define GTT_PAGE_SIZE       0x1000      /* 4 KB per GTT page */
#define GTT_PTE_SIZE        8           /* 8 bytes per PTE (64-bit, Gen8+) */
#define GTT_MAX_ENTRIES     131072      /* 512 MB / 4 KB (safety cap) */

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
     *  GGTT Page Table Management
     * ─────────────────────────────────────
     */

    /*!
     * @brief  Initialize GGTT — อ่าน GTT_SIZE, setup PTE array pointer
     *
     *  Flow:
     *    1. อ่าน GEN6_GTT_SIZE_REG (0x100C) → จำนวน entries
     *    2. ตั้งค่า pointer ไปยัง PTE array ที่ GEN8_GTT_BASE (0x800000)
     *    3. clear GGTT table (zero PTEs)
     *    4. TLB invalidate
     *
     *  ถูกเรียกจาก start() Phase 6 แทน ggttInvalidate() เดิม
     */
    virtual void initGGTT(void);

    /*!
     * @brief  เขียน 64-bit PTE ไปยัง GGTT
     *
     *  @param index     GGTT entry index (0 = first 4KB page)
     *  @param pte       64-bit PTE value (Gen8+ format)
     */
    virtual void writeGGTTPTE(uint32_t index, uint64_t pte);

    /*!
     * @brief  อ่าน 64-bit PTE จาก GGTT
     *
     *  @param index     GGTT entry index
     *  @return          64-bit PTE value
     */
    virtual uint64_t readGGTTPTE(uint32_t index);

    /*!
     * @brief  Clear GGTT ทั้งหมด (zero every PTE)
     */
    virtual void clearGGTT(void);

    /*!
     * @brief  สร้าง 64-bit PTE จาก physical address
     *
     *  Gen8+ PTE format:
     *    bit 0:   Present (valid)
     *    bit 3:   PAT index (0=WB/LLC, 1=UC, 2=WC)
     *    bits 12-63: Physical Address >> 12 (PFN)
     *
     *  @param physAddr   Physical address (ต้อง 4KB aligned)
     *  @param caching    0=WB (coherent), 1=UC, 2=WC
     *  @return           64-bit PTE
     */
    virtual uint64_t makePTE(uint64_t physAddr, uint8_t caching);

    /*
     * ─────────────────────────────────────
     *  Display / Panel Initialization
     * ─────────────────────────────────────
     */

    virtual bool initDisplay(void);
    virtual bool panelPowerOn(void);
    virtual bool initCDCLK(void);
    virtual bool initDPLL(void);
    virtual bool initBacklight(void);
    virtual void dumpDisplayStatus(void);

    /*
     * ─────────────────────────────────────
     *  Framebuffer Pipeline (Phase 2.2)
     * ─────────────────────────────────────
     */

    /*!
     * @brief  Full framebuffer pipeline setup:
     *          1. allocScanoutBuffer  → reserve aperture + GGTT PTEs
     *          2. initPipe            → configure pipe A
     *          3. initPrimaryPlane    → configure primary plane
     *          4. initTranscoder      → configure transcoder A
     *          5. initDDI             → enable DDI A for eDP/DP
     */
    virtual bool setupFramebuffer(void);

    /*!
     * @brief  Allocate scanout buffer in aperture + write GGTT PTEs
     *
     *  Allocates physical pages via IOBufferMemoryDescriptor,
     *  writes PTEs into GGTT, returns GGTT page offset.
     *
     *  @param width   image width in pixels
     *  @param height  image height in pixels
     *  @param bpp     bytes per pixel (32 = XRGB8888)
     *  @return true on success
     */
    virtual bool allocScanoutBuffer(uint32_t width, uint32_t height, uint32_t bpp);

    /*!
     * @brief  Configure Pipe (source size, pipe config, enable)
     *
     *  @param pipe    pipe index (0=PIPE_A, 1=PIPE_B)
     *  @param width   horizontal resolution
     *  @param height  vertical resolution
     *  @param bpc     bits per color component (8 or 10)
     */
    virtual void initPipe(uint32_t pipe, uint32_t width, uint32_t height, uint32_t bpc);

    /*!
     * @brief  Configure Primary Plane (format, size, stride, surface)
     *
     *  @param pipe        pipe index
     *  @param width       visible width
     *  @param height      visible height
     *  @param stride      bytes per line (pitch)
     *  @param ggttOffset  GGTT page index for framebuffer start
     *  @param format      PLANE_CTL_FORMAT_* constant
     */
    virtual void initPrimaryPlane(uint32_t pipe, uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t ggttOffset, uint32_t format);

    /*!
     * @brief  Configure Transcoder (mode, port, bpc, lane count)
     *
     *  @param trans   transcoder index (0=TRANS_A)
     *  @param port    DDI port (0=DDI_A)
     *  @param bpc     bits per color
     *  @param lanes   lane count (1/2/4)
     *  @param mode    TRANS_DDI_MODE_* constant
     */
    virtual void initTranscoder(uint32_t trans, uint32_t port, uint32_t bpc,
                                uint32_t lanes, uint32_t mode);

    /*!
     * @brief  Enable DDI Buffer
     *
     *  @param port   DDI port index (0=DDI_A)
     *  @param lanes  lane count (1/2/4)
     */
    virtual void initDDI(uint32_t port, uint32_t lanes);

    /*
     * ─────────────────────────────────────
     *  Phase 2.3 — EDID / DDI Config / Display Node
     * ─────────────────────────────────────
     */

    /*!
     * @brief  AUX channel transaction (I2C-over-AUX)
     *
     *  Sends and receives data over the DDI AUX channel.
     *  Used for EDID read and DPCD access.
     *
     *  @param port     DDI port index (0=DDI_A, 1=DDI_B)
     *  @param ddcAddr  7-bit I2C address (0x50 for EDID)
     *  @param sendBuf  data to send (NULL for read-only)
     *  @param sendLen  bytes to send (max 16)
     *  @param recvBuf  buffer for received data
     *  @param recvLen  bytes to receive (max 16)
     *  @return true on ACK
     */
    virtual bool ddiAuxXfer(uint32_t port, uint32_t ddcAddr,
                            const uint8_t *sendBuf, uint32_t sendLen,
                            uint8_t *recvBuf, uint32_t recvLen);

    /*!
     * @brief  Read EDID via DDI AUX channel
     *
     *  Performs I2C-over-AUX transaction sequence:
     *    1. Write EDID offset byte to I2C addr 0x50
     *    2. Read 128 (or 256) bytes in 16-byte chunks
     *
     *  @param port  DDI port index
     *  @return true if valid EDID read
     */
    virtual bool readEdidFromAux(uint32_t port);

    /*!
     * @brief  Attempt EDID from IORegistry (bootloader-injected)
     *
     *  Checks for an "EDID" data property on our service in IORegistry.
     *  Bootloaders like OpenCore can pre-inject this.
     *
     *  @return true if EDID found in registry
     */
    virtual bool readEdidFromRegistry(void);

    /*!
     * @brief  Parse raw EDID bytes to extract display parameters
     *
     *  Fills fDisplayWidth, fDisplayHeight, fDisplayRefresh,
     *  fDisplayName from the Preferred Timing descriptor in EDID.
     *
     *  @return true if at least one valid timing found
     */
    virtual bool parseEdid(void);

    /*!
     * @brief  Set transcoder display timings from raw params
     *
     *  Writes HTOTAL, HSYNC, VTOTAL, VSYNC, HBLANK, VBLANK
     *  registers for the specified transcoder.
     *
     *  @param trans      transcoder index (0=TRANS_A)
     *  @param hdisplay   active horizontal pixels
     *  @param hsync_start  hdisplay + hfront_porch
     *  @param hsync_end    hsync_start + hsync_width
     *  @param htotal      hsync_end + hback_porch
     *  @param vdisplay   active vertical lines
     *  @param vsync_start vdisplay + vfront_porch
     *  @param vsync_end   vsync_start + vsync_width
     *  @param vtotal      vsync_end + vback_porch
     */
    virtual void setDisplayTimings(uint32_t trans,
                                   uint32_t hdisplay, uint32_t hsync_start,
                                   uint32_t hsync_end, uint32_t htotal,
                                   uint32_t vdisplay, uint32_t vsync_start,
                                   uint32_t vsync_end, uint32_t vtotal);

    /*!
     * @brief  Configure DDI for DP/eDP output with link training
     *
     *  Sets link rate, lane count, performs link training
     *  sequence: pattern 1 → pattern 2 → normal.
     *
     *  @param port     DDI port index
     *  @param lanes    lane count (1, 2, or 4)
     *  @param linkRate 0=1.62G, 1=2.7G, 2=5.4G, 3=8.1G
     *  @return true on success
     */
    virtual bool configureDDIForDP(uint32_t port, uint32_t lanes, uint32_t linkRate);

    /*!
     * @brief  Configure DDI for HDMI output
     *
     *  Sets DDI_BUF_CTL for HDMI mode, configures DDI_BUF_TRANS
     *  for proper vswing/pre-emphasis for HDMI signaling.
     *
     *  @param port  DDI port index
     *  @return true on success
     */
    virtual bool configureDDIForHDMI(uint32_t port);

    /*!
     * @brief  Inject display node into IORegistry
     *
     *  Creates or updates IORegistry properties advertising
     *  EDID data, connection type, and display parameters
     *  so macOS recognises the display.
     *
     *  @param isInternal  true for eDP/LVDS, false for external
     *  @param edid        raw EDID bytes
     *  @param edidLen     EDID length in bytes
     *  @return true on success
     */
    virtual bool injectDisplayNode(bool isInternal, const uint8_t *edid, uint32_t edidLen);

    /*!
     * @brief  Set backlight brightness
     *  @param percent  0-100 brightness level
     */
    virtual void setBacklightBrightness(uint32_t percent);

    /*!
     * @brief  Enable or disable backlight PWM
     *  @param on  true=enable, false=disable
     */
    virtual void enableBacklight(bool on);

    /*!
     * @brief  Probe DDI port for display presence
     *  @param port  DDI port index (0=A, 1=B, etc.)
     *  @return true if display detected
     */
    virtual bool probeDDI(uint32_t port);

    /*!
     * @brief  Setup DDI B for external monitor
     */
    virtual bool setupDDIB(void);

    /*!
     * @brief  Setup DDI C for external monitor
     */
    virtual bool setupDDIC(void);

    /*!
     * @brief  Probe and enable multi-monitor configuration
     *  @return true if at least one external display configured
     */
    virtual bool setupMultiMonitor(void);

    /*
     * ─────────────────────────────────────
     *  Phase 3 — Interrupts / Vblank / Hotplug
     * ─────────────────────────────────────
     */

    /*!
     * @brief  Initialise GT engine + display interrupt system
     *
     *  1. Map PCI interrupt line
     *  2. Install interrupt handler via IOService
     *  3. Mask all GT engine interrupts initially
     *  4. Enable display interrupt control
     */
    virtual bool initInterrupts(void);

    /*!
     * @brief  Install interrupt handler on IOPCIDevice
     *
     *  Uses IOFilterInterruptEventSource or
     *  fPCIDevice->registerInterrupt().
     *
     *  @return true on success
     */
    virtual bool installInterruptHandlers(void);

    /*!
     * @brief  Top-level interrupt handler (called from primary ISR)
     *  @param src  interrupt source
     *  @param count  number of pending interrupts
     */
    virtual void handleInterrupt(void *src, int count);

    /*!
     * @brief  Handle GT engine-specific interrupt
     *  @param intrID  interrupt identity register value
     */
    virtual void handleGTInterrupt(uint32_t intrDW0);

    /*!
     * @brief  Handle display interrupt (vblank, hotplug)
     */
    virtual void handleDisplayInterrupt(void);

    /*!
     * @brief  Enable or disable VBLANK interrupt for a pipe
     *  @param pipe   pipe index (0=PIPE_A)
     *  @param enable true=enable, false=disable
     *  @return true on success
     */
    virtual bool enableVblankInterrupt(uint32_t pipe, bool enable);

    /*!
     * @brief  Read current frame count from hardware
     *  @param pipe  pipe index
     *  @return frame count value
     */
    virtual uint32_t getFrameCount(uint32_t pipe);

    /*!
     * @brief  Read current scan line from hardware
     *  @param pipe  pipe index
     *  @return scan line number (0-vertical_total)
     */
    virtual uint32_t getScanLine(uint32_t pipe);

    /*!
     * @brief  Initialise hotplug detection
     *
     *  Enables HPD interrupts and creates a work loop
     *  for polling DDI connection status.
     *
     *  @return true on success
     */
    virtual bool initHotplugDetect(void);

    /*!
     * @brief  Poll all DDI ports for connection changes
     *
     *  Reads SHOTPLUG_STATUS, compares with fHotplugDDIMask,
     *  and logs connection events.
     *
     *  @return bitmask of ports with new connection status
     */
    virtual uint32_t pollHotplugStatus(void);

    /*
     * ─────────────────────────────────────
     *  Phase 4 — IOFramebuffer Binding
     * ─────────────────────────────────────
     */

    /*!
     * @brief  Create framebuffer IORegistry interface
     *
     *  Creates a child nubs in IORegistry that match
     *  IOFramebuffer for macOS graphics driver.
     *  Sets up display mode settings per CRTC.
     *
     *  @return true on success
     */
    virtual bool createFramebufferInterface(void);

    /*!
     * @brief  Register our service as a framebuffer provider
     *
     *  Publishes IORegistry properties so AppleIntelCFLGraphics
     *  can bind to our display outputs (similar to
     *  IOFramebuffer::registerService).
     */
    virtual void registerFramebuffer(void);

    /*!
     * @brief  Initialise GTT memory for framebuffer interface
     *
     *  Pre-allocates GGTT space for up to FB_MAX_CRTC
     *  scanout buffers.
     *
     *  @return true on success
     */
    virtual bool framebufferInitGMMemory(void);

    /*!
     * @brief  Set display mode for a given CRTC index
     *
     *  @param index   CRTC index (0=primary, 1=external1, 2=external2)
     *  @param width   active pixels
     *  @param height  active lines
     *  @param bpp     bits per pixel (32)
     *  @param refresh refresh rate in mHz
     *  @return true on success
     */
    virtual bool framebufferSetDisplayMode(uint32_t index, uint32_t width,
                                           uint32_t height, uint32_t bpp,
                                           uint32_t refresh);

    /*!
     * @brief  Get CPU-accessible base address of framebuffer
     *  @param index  CRTC index
     *  @return virtual address, or NULL
     */
    virtual void *getFramebufferBase(uint32_t index);

    /*!
     * @brief  Get size (bytes) of framebuffer for a CRTC
     *  @param index  CRTC index
     *  @return size in bytes
     */
    virtual uint32_t getFramebufferSize(uint32_t index);

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
    volatile uint32_t       *fGsm;             /*!< GTT Stolen Memory (legacy — uint32 view) */
    volatile uint64_t       *fGttPteArray;     /*!< GGTT PTE array (64-bit entries, Gen8+) */

    /* Translation Table */
    RegisterTranslationEntry fTransTable[16];   /*!< ตารางแปลพิกัด register offset */
    int                      fTransCount;      /*!< จำนวน entry ในตาราง */

    /* Memory Map References — เก็บไว้เพื่อ release ใน stop() */
    IOMemoryDescriptor      *fMMIODesc;        /*!< BAR0 IOMemoryDescriptor */
    IOMemoryDescriptor      *fApertureDesc;    /*!< BAR2 IOMemoryDescriptor */

    /* Framebuffer State */
    IOMemoryDescriptor      *fScanoutDesc;      /*!< IOMemoryDescriptor backing scanout */
    void                    *fScanoutBufferVA;   /*!< Kernel virtual address of scanout buffer */
    IOPhysicalAddress        fScanoutBufferPA;   /*!< Physical address of scanout buffer */
    uint32_t                 fScanoutBufferSize; /*!< Allocated size in bytes */
    uint32_t                 fScanoutGGTTOffset; /*!< GGTT page index of scanout buffer */
    uint32_t                 fScanoutNumPages;   /*!< Number of 4KB pages in scanout buffer */
    uint32_t                 fScanoutWidth;      /*!< Active scanout width (pixels) */
    uint32_t                 fScanoutHeight;     /*!< Active scanout height (pixels) */
    uint32_t                 fScanoutBpp;        /*!< Bytes per pixel */
    uint32_t                 fScanoutStride;     /*!< Bytes per line */
    uint32_t                 fScanoutFormat;     /*!< PLANE_CTL_FORMAT_* encoding */

    /* EDID / Display Info */
    uint8_t                  fEdidData[EDID_BLOCK_SIZE * 2]; /*!< Raw EDID (up to 2 blocks) */
    uint32_t                 fEdidLength;         /*!< Valid EDID bytes */
    uint32_t                 fDisplayWidth;       /*!< Native width from EDID */
    uint32_t                 fDisplayHeight;      /*!< Native height from EDID */
    uint32_t                 fDisplayRefresh;     /*!< Preferred refresh rate (mHz) */
    uint32_t                 fDisplayBpp;         /*!< Colour depth (bits) */
    char                     fDisplayName[32];    /*!< Monitor name from EDID */
    bool                     fEdidValid;          /*!< true after successful EDID parse */

    /* Phase 3 — Interrupt / Vblank / Hotplug State */
    IOInterruptEventSource  *fInterruptSource;    /*!< Work-loop interrupt source */
    IOWorkLoop              *fWorkLoop;           /*!< IOWorkLoop for deferral */
    uint32_t                 fGtIntrMaskDW0;      /*!< Saved GT interrupt mask DW0 */
    uint32_t                 fDisplayIntrMask;    /*!< Display interrupt mask */
    bool                     fInterruptsReady;    /*!< true after interrupt install */
    uint32_t                 fHotplugDDIMask;     /*!< Bitmask of DDI ports connected */
    uint32_t                 fHotplugLastStatus;  /*!< Previous SHOTPLUG_STATUS value */

    /* Phase 4 — Framebuffer Interface (per-CRTC) */
    uint32_t                 fFramebufferCount;   /*!< Number of active CRTCs */
    uint32_t                 fFbWidth[FB_MAX_CRTC];
    uint32_t                 fFbHeight[FB_MAX_CRTC];
    uint32_t                 fFbBpp[FB_MAX_CRTC];
    uint32_t                 fFbRefresh[FB_MAX_CRTC];   /*!< in mHz */
    void                    *fFbBase[FB_MAX_CRTC];      /*!< CPU VA of scanout buffer */
    uint32_t                 fFbGGTTOffset[FB_MAX_CRTC]; /*!< GGTT page index per CRTC */
    uint32_t                 fFbNumPages[FB_MAX_CRTC];   /*!< Pages per CRTC */
};

#endif /* __MY_INTEL_GPU_HPP__ */
