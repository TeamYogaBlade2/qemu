/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 display subsystem blocks
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * WHY ONE GENERIC DEVICE
 *
 * OVL, RDMA, WDMA, COLOR, TDSHP, MUTEX, DSI, DPI, BLS and GAMMA are all the same
 * kind of thing: a bank of 32-bit registers where the guest writes configuration
 * and polls status. Writing ten near-identical C files would repeat the same read,
 * write and reset stubs ten times and make it easy for one of them to differ in a
 * way nobody notices.
 *
 * So the register set is DATA, generated from the datasheet by
 * mt6589-emulator/tools/gen_regs_header.py, and this one file implements the
 * behaviour. Adding a block is a table entry, not a new driver.
 *
 * WHAT IS MODELLED, AND WHAT IS NOT
 *
 * Modelled:
 *   - every documented register exists, reads its documented reset value, and
 *     holds whatever the guest writes (so a read-back returns what was written,
 *     which is what configuration code checks)
 *   - write-1-to-clear registers, where the datasheet marks the field W1C
 *   - self-clearing trigger bits (a write of 1 clears the bit), so a guest that
 *     starts an engine and polls the bit back to 0 does not hang
 *   - the "idle"/"status" bits that tile and frame completion are reported from,
 *     because a display pipeline that never reports idle stalls the whole probe
 *
 * NOT modelled: actual pixel output. No framebuffer is scanned out and no DSI
 * packets are generated. A block reports idle immediately after a trigger rather
 * than after a real frame, which is enough for the driver's probe and modeset path
 * but means anything that measures timing or reads back composited pixels will not
 * see what it expects. That is the honest boundary: the register interface is
 * faithful, the pixels are absent.
 *
 * The real thing this cannot do yet is verify a modeset produces the right
 * picture. It can verify the driver programs the hardware in the right ORDER with
 * the right values, which is where the porting bugs are.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "target/arm/cpu.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/arm/mt6589.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "ui/console.h"

/*
 * Register definitions come from the generated datasheet headers
 * (mt6589-emulator/tools/gen_regs_header.py), so an offset in this file is an
 * expression with the datasheet's own name rather than a number copied by hand.
 * The headers carry absolute addresses, hence the _OFF() helpers below.
 */
#include "hw/arm/mt6589-regs/mt6589-disp_ovl-regs.h"
#include "hw/arm/mt6589-regs/mt6589-disp_rdma-regs.h"
#include "hw/arm/mt6589-regs/mt6589-disp_mutex-regs.h"
#include "hw/arm/mt6589-regs/mt6589-disp_wdma-regs.h"
#include "hw/arm/mt6589-regs/mt6589-disp_bls-regs.h"
#include "hw/arm/mt6589-regs/mt6589-disp_gamma-regs.h"
#include "hw/arm/mt6589-regs/mt6589-disp_color-regs.h"
#include "hw/arm/mt6589-regs/mt6589-dsi-regs.h"
#include "hw/arm/mt6589-regs/mt6589-dpi-regs.h"
#include "hw/arm/mt6589-regs/mt6589-dispsys_config-regs.h"

#define RDMA(name)  (MT6589_DISP_RDMA_##name - MT6589_DISP_RDMA_BASE)
#define MUTEX(name) (MT6589_DISP_MUTEX_##name - MT6589_DISP_MUTEX_BASE)
#define WDMA(name)  (MT6589_DISP_WDMA_##name - MT6589_DISP_WDMA_BASE)
#define BLS(name)   (MT6589_DISP_BLS_##name - MT6589_DISP_BLS_BASE)
#define GAMMA(name) (MT6589_DISP_GAMMA_##name - MT6589_DISP_GAMMA_BASE)
#define COLOR(name) (MT6589_DISP_COLOR_##name - MT6589_DISP_COLOR_BASE)
#define DSI(name)   (MT6589_DSI_##name - MT6589_DSI_BASE)
#define DPI(name)   (MT6589_DPI_##name - MT6589_DPI_BASE)


/* ------------------------------------------------------------------------- */
/* OVL - display overlay, 0x14003000                                          */
/*
 * OVL_STA's RDMA-idle bits are what the DRM driver's atomic commit waits on.
 * Reporting them as 0 makes the commit wait for a pipeline that never drains.
 */
/*
 * The layer layout below is MEASURED, not guessed. Setting MT6589_DISP_LOG=1 and
 * booting LK gives the exact sequence its display driver programs, and it does not
 * match the offsets that were assumed from the block diagram:
 *
 *     DISP ovl W 0020 = 03200500     ROI size, h<<16 | w = 0x320 x 0x500 = 800x1280
 *     DISP ovl W 0070 = 00001000     layer 1 CON
 *     DISP ovl W 0078 = 03200500     layer 1 size
 *     DISP ovl W 0080 = bf600000     layer 1 address
 *     DISP ovl W 0084 = 00000a00     layer 1 pitch  (0xa00 / 0x500 = 2 bytes/pixel)
 *     DISP ovl W 0090 = 40000000     layer 2 CON
 *     DISP ovl W 0094 = ff000000     layer 2 key/alpha
 *     DISP ovl W 00a0 = bf7f4000     layer 2 address (exactly one 1280x800x2 frame on)
 *     DISP ovl W 00a4 = 00000a00     layer 2 pitch
 *     DISP ovl W 000c = 00000001     OVL_EN
 *
 * So layers live at 0x50 + n*0x20 with CON, ALPHA/KEY, SIZE, OFFSET, ADDR, PITCH at
 * +0x00, +0x04, +0x08, +0x0c, +0x10, +0x14. Every one of those offsets has to be
 * in this table, because the write path stores only registers it knows and drops
 * the rest - which is exactly how the earlier table lost the layer addresses and
 * left this block unable to render anything.
 */
/*
 * The layer bases come from the generated datasheet header, not from counting: the
 * layers start at 0x30 and step by 0x20, so L0 is 0x30..0x44, L1 0x50..0x64,
 * L2 0x70..0x84 and L3 0x90..0xa4. An earlier version of this table started at 0x50,
 * which quietly shifted every layer by one: the register it read as "L1" was really L2,
 * the buffer it drew was right, and **L0 - the plane a driver is most likely to use -
 * was never rendered at all**. That is exactly the class of mistake a generated header
 * prevents, and it is why the numbers below are expressions rather than literals.
 */
#define OVL(name)   (MT6589_DISP_OVL_##name - MT6589_DISP_OVL_BASE)

/* Every layer has the same six registers, 0x20 apart: CON, SRCKEY, SRC_SIZE, OFFSET,
 * ADDR, PITCH. */
#define OVL_LAYER_REGS(n)                                                  \
    { OVL(OVL_L##n##_CON), 0, 0 },                                         \
    { OVL(OVL_L##n##_SRCKEY), 0, 0 },                                      \
    { OVL(OVL_L##n##_SRC_SIZE), 0, 0 },                                    \
    { OVL(OVL_L##n##_OFFSET), 0, 0 },                                      \
    { OVL(OVL_L##n##_ADDR), 0, 0 },                                        \
    { OVL(OVL_L##n##_PITCH), 0, 0 }

static const Mt6589DispReg mt6589_ovl_regs[] = {
    { OVL(OVL_STA), 0x0000001f, MREG_IDLE },  /* RDMA idle bits */
    { OVL(OVL_INTEN), 0, 0 },
    { OVL(OVL_INTSTA), 0, MREG_W1C },
    { OVL(OVL_EN), 0, 0 },
    { OVL(OVL_TRIG), 0, MREG_SELFCL },
    { OVL(OVL_RST), 0, MREG_SELFCL },
    { OVL(OVL_ROI_SIZE), 0, 0 },
    { OVL(OVL_ROI_BGCLR), 0, 0 },
    { OVL(OVL_SRC_CON), 0, 0 },
    OVL_LAYER_REGS(0),
    OVL_LAYER_REGS(1),
    OVL_LAYER_REGS(2),
    OVL_LAYER_REGS(3),
    /* The per-layer extension blocks the driver also writes. */
    { 0x0100, 0, 0 },
    { 0x0120, 0, 0 },
    { 0x0140, 0, 0 },
    { 0x0160, 0, 0 },
};

/* ------------------------------------------------------------------------- */
/* RDMA - read DMA, 0x14006000                                                */
/*
 * GLOBAL_CON bit 0 is the engine enable; the driver polls it plus the output-size
 * registers. INT_STATUS is W1C.
 */
static const Mt6589DispReg mt6589_rdma_regs[] = {
    { RDMA(DISP_RDMA_INT_ENABLE), 0, 0 },
    { RDMA(DISP_RDMA_INT_STATUS), 0, MREG_W1C },
    { RDMA(DISP_RDMA_GLOBAL_CON), 0, 0 },
    { RDMA(DISP_RDMA_SIZE_CON_0), 0, 0 },
    { RDMA(DISP_RDMA_SIZE_CON_1), 0, 0 },
    { RDMA(DISP_RDMA_TARGET_LINE), 0, 0 },
    { RDMA(DISP_RDMA_MEM_CON), 0, 0 },
    /*
     * The source address and pitch are at 0x0028/0x002c. The previous table had them
     * at 0x0040/0x0044, which the generated header shows are FIFO_CON and FIFO_LOG -
     * so the address was being dropped, and this block could not have rendered anything
     * even if it had had a framebuffer to read. LK's own sequence writes 0x0028/0x002c,
     * which is how the discrepancy was found.
     */
    { RDMA(DISP_RDMA_MEM_START_ADDR), 0, 0 },
    { RDMA(DISP_RDMA_MEM_SRC_PITCH), 0, 0 },
    { RDMA(DISP_RDMA_MEM_GMC_SETTING_0), 0, 0 },
    { RDMA(DISP_RDMA_MEM_SLOW_CON), 0, 0 },
    { RDMA(DISP_RDMA_MEM_GMC_SETTING_1), 0, 0 },
    { RDMA(DISP_RDMA_FIFO_CON), 0, 0 },
    { 0x0154, 0, 0 },                   /* OUTPUT_SIZE */
};

/* ------------------------------------------------------------------------- */
/* COLOR - colour matrix, 0x1400a000                                          */
/*
 * The coefficient table at 0xf64..0xfdc is written by mt6589_color_init() in one
 * contiguous run, so those offsets must all hold writes.
 */
static const Mt6589DispReg mt6589_color_regs[] = {
    /*
     * The counters follow CFG_MAIN, and the block's own START / INTEN / INTSTA are
     * much further down at 0xf00. The hand-written table had them at 0x404 / 0x408 /
     * 0x40c - on top of the counters - so the driver's interrupt enable and status
     * went nowhere, and a counter carried the write-one-to-clear flag. Comparing the
     * offsets against mt6589-disp_color-regs.h is what exposed it.
     */
    { COLOR(CFG_MAIN), 0, 0 },
    { COLOR(PXL_CNT_MAIN), 0, 0 },
    { COLOR(LINE_CNT_MAIN), 0, 0 },
    { COLOR(WIN_X_MAIN), 0, 0 },
    { COLOR(DISP_COLOR_START), 0, MREG_SELFCL },
    { COLOR(DISP_COLOR_INTEN), 0, 0 },
    /* Write-one-to-clear is unverified for this block; nothing wires its line yet. */
    { COLOR(DISP_COLOR_INTSTA), 0, MREG_W1C },
    { COLOR(DISP_COLOR_CM1_C00), 0, 0 },                   /* R2Y coefficient 0 */
    { COLOR(DISP_COLOR_CM1_C01), 0, 0 },
    { COLOR(DISP_COLOR_CM1_C02), 0, 0 },
    { COLOR(DISP_COLOR_CM1_C10), 0, 0 },
    { COLOR(DISP_COLOR_CM1_C11), 0, 0 },
    { COLOR(DISP_COLOR_CM1_C12), 0, 0 },
};

/* ------------------------------------------------------------------------- */
/* MUTEX - display mutex, 0x14011000                                          */
/*
 * The mutex is how every other display block is sequenced. MOD is written with the
 * owning module id, then the block is triggered. A model that ignores MOD entirely
 * makes the driver's mutex acquisition appear to succeed for any caller, which
 * hides ordering bugs rather than exposing them.
 */
static const Mt6589DispReg mt6589_mutex_regs[] = {
    { MUTEX(DISP_MUTEX_INTEN), 0, 0 },
    { MUTEX(DISP_MUTEX_INTSTA), 0, MREG_W0C },
    /*
     * Per mutex: EN, MUTEX, RST, MOD, SOF at 0x20/0x24/0x28/0x2c/0x30 and
     * 0x40/0x44/0x48/0x4c/0x50. The previous table put RST at 0x24 and SOF at 0x2c -
     * one register early from RST onwards - so SOF writes went to MOD and never raised
     * the event, while the grant the driver actually polls (bit 1 of MUTEX at 0x24)
     * lived in what this model called RST.
     */
    { MUTEX(DISP_MUTEX0_EN), 0, 0 },
    { MUTEX(DISP_MUTEX0), 0, 0 },
    { MUTEX(DISP_MUTEX0_RST), 0, 0 },
    { MUTEX(DISP_MUTEX0_MOD), 0, 0 },
    { MUTEX(DISP_MUTEX0_SOF), 0, 0 },
    { MUTEX(DISP_MUTEX1_EN), 0, 0 },
    { MUTEX(DISP_MUTEX1), 0, 0 },
    { MUTEX(DISP_MUTEX1_RST), 0, 0 },
    { MUTEX(DISP_MUTEX1_MOD), 0, 0 },
    { MUTEX(DISP_MUTEX1_SOF), 0, 0 },
};

#define BLOCK(name_, base_, regs_, irq)                       \
    { name_, base_, regs_, ARRAY_SIZE(regs_), true, irq }

/*
 * DSI - display serial interface, 0x1400d000
 *
 * Every register the datasheet documents, generated from the extracted
 * header, carrying its documented reset value. That distinction is the point:
 * a register reading 0 because nothing models it looks exactly like one that
 * is genuinely 0, whereas one reading its documented reset is evidence the
 * block is there at all.
 */

static const Mt6589DispReg mt6589_dsi_regs[] = {
    { DSI(DSI_START), 0x00000000, 0 },  /* DSI_START */
    { DSI(DSI_STATUS), 0x00000000, 0 },  /* DSI_STATUS */
    { DSI(DSI_INTEN), 0x00000000, 0 },  /* DSI_INTEN */
    { DSI(DSI_INTSTA), 0x00000000, 0 },  /* DSI_INTSTA */
    { DSI(DSI_COM_CON), 0x00000000, 0 },  /* DSI_COM_CON */
    { DSI(DSI_MODE_CON), 0x00000000, 0 },  /* DSI_MODE_CON */
    { DSI(DSI_TXRX_CON), 0x00000000, 0 },  /* DSI_TXRX_CON */
    { DSI(DSI_PSCON), 0x00000000, 0 },  /* DSI_PSCON */
    { DSI(DSI_VSA_NL), 0x00000000, 0 },  /* DSI_VSA_NL */
    { DSI(DSI_VBP_NL), 0x00000000, 0 },  /* DSI_VBP_NL */
    { DSI(DSI_VFP_NL), 0x00000000, 0 },  /* DSI_VFP_NL */
    { DSI(DSI_VACT_NL), 0x00000000, 0 },  /* DSI_VACT_NL */
    { DSI(DSI_HSA_WC), 0x00000000, 0 },  /* DSI_HSA_WC */
    { DSI(DSI_HBP_WC), 0x00000000, 0 },  /* DSI_HBP_WC */
    { DSI(DSI_HFP_WC), 0x00000000, 0 },  /* DSI_HFP_WC */
    { DSI(DSI_BLLP_WC), 0x00000000, 0 },  /* DSI_BLLP_WC - the driver writes it; without the
                                 * entry the value was dropped and read back as 0 */
    { DSI(DSI_CMDQ_CON), 0x00000000, 0 },  /* DSI_CMDQ_CON */
    { DSI(DSI_RX_DATA03), 0x00000000, 0 },  /* DSI_RX_DATA03 */
    { DSI(DSI_RX_DATA47), 0x00000000, 0 },  /* DSI_RX_DATA47 */
    { DSI(DSI_RX_DATA8B), 0x00000000, 0 },  /* DSI_RX_DATA8B */
    { DSI(DSI_RX_DATAC), 0x00000000, 0 },  /* DSI_RX_DATAC */
    { DSI(DSI_RX_RACK), 0x00000000, 0 },  /* DSI_RX_RACK */
    { DSI(DSI_RX_TRIG_STA), 0x00000000, 0 },  /* DSI_RX_TRIG_STA */
    { DSI(DSI_MEM_CONTI), 0x00000000, 0 },  /* DSI_MEM_CONTI */
    { DSI(DSI_FRM_BC), 0x00000000, 0 },  /* DSI_FRM_BC */
    { DSI(DSI_PHY_LCCON), 0x00000000, 0 },  /* DSI_PHY_LCCON */
    { DSI(DSI_PHY_LD0CON), 0x00000000, 0 },  /* DSI_PHY_LD0CON */
    { DSI(DSI_PHY_TIMCON0), 0x00005050, 0 },  /* DSI_PHY_TIMCON0 */
    { DSI(DSI_PHY_TIMCON1), 0x00004c68, 0 },  /* DSI_PHY_TIMCON1 */
    { DSI(DSI_PHY_TIMCON2), 0x00000000, 0 },  /* DSI_PHY_TIMCON2 */
    { DSI(DSI_PHY_TIMCON3), 0x00005070, 0 },  /* DSI_PHY_TIMCON3 */
    { DSI(DSI_VM_CMD_CON), 0x00000000, 0 },  /* DSI_VM_CMD_ */
    { DSI(DSI_CKSM_OUT), 0x0000ffff, 0 },  /* DSI_CKSM_OUT */
    { DSI(DSI_STATE_DBG0), 0x00001008, 0 },  /* DSI_STATE_DBG0 */
    { DSI(DSI_STATE_DBG1), 0x00004000, 0 },  /* DSI_STATE_DBG1 */
    { DSI(DSI_STATE_DBG2), 0x00000200, 0 },  /* DSI_STATE_DBG2 */
    { DSI(DSI_STATE_DBG3), 0x00000210, 0 },  /* DSI_STATE_DBG3 */
    { DSI(DSI_STATE_DBG4), 0x00000210, 0 },  /* DSI_STATE_DBG4 */
    { DSI(DSI_STATE_DBG6), 0x00002000, 0 },  /* DSI_STATE_DBG6 */
    { DSI(DSI_STATE_DBG7), 0x00000000, 0 },  /* DSI_STATE_DBG7 */
    { DSI(DSI_STATE_DBG8), 0x00000000, 0 },  /* DSI_STATE_DBG8 */
    { DSI(DSI_STATE_DBG9), 0x00000000, 0 },  /* DSI_STATE_DBG9 */
    { DSI(DSI_DEBUG_SEL), 0x00000000, 0 },  /* DSI_DEBUG_SEL */
    { DSI(DSI_RESERVED), 0x00000000, 0 },  /* DSI_RESERVED */
    { DSI(DSI_BIST_PATTERN), 0x00000000, 0 },  /* DSI_BIST_PATTERN */
    { DSI(DSI_BIST_CON), 0x00000000, 0 },  /* DSI_BIST_CON */
    { DSI(DSI_CMDQ0), 0x00000000, 0 },  /* DSI_CMDQ0 */
};

/*
 * DPI - display parallel interface, 0x1400e000
 *
 * Every register the datasheet documents, generated from the extracted
 * header, carrying its documented reset value. That distinction is the point:
 * a register reading 0 because nothing models it looks exactly like one that
 * is genuinely 0, whereas one reading its documented reset is evidence the
 * block is there at all.
 */

static const Mt6589DispReg mt6589_dpi_regs[] = {
    { DPI(DPI_EN), 0x00000000, 0 },  /* DPI_EN */
    { DPI(DPI_RST), 0x00000000, 0 },  /* DPI_RST */
    { DPI(DPI_INTEN), 0x00000000, 0 },  /* DPI_INTEN */
    { DPI(DPI_INTSTA), 0x00000000, 0 },  /* DPI_INTSTA */
    { DPI(DPI_CON), 0x00000000, 0 },  /* DPI_CON */
    { DPI(DPI_CLKCON), 0x00000000, 0 },  /* DPI_CLKCON */
    { DPI(DPI_SIZE), 0x00000000, 0 },  /* DPI_SIZE */
    { DPI(DPI_ESAV_VTIM_L), 0x00000000, 0 },  /* DPI_ESAV_VTIM_L */
    { DPI(DPI_ESAV_VTIM_R), 0x00000000, 0 },  /* DPI_ESAV_VTIM_R */
    { DPI(DPI_ESAV_FTIM), 0x00000000, 0 },  /* DPI_ESAV_FTIM */
    { DPI(DPI_BG_HCNTL), 0x00000400, 0 },  /* DPI_BG_HCNTL */
    { DPI(DPI_BG_VCNTL), 0x00000000, 0 },  /* DPI_BG_VCNTL */
    { DPI(DPI_BG_COLOR), 0x00000000, 0 },  /* DPI_BG_COLOR */
    { DPI(DPI_TGEN_POL), 0x00000000, 0 },  /* DPI_TGEN_POL */
    { DPI(DPI_STATUS), 0x00000000, 0 },  /* DPI_STATUS */
    { DPI(DPI_MATRIX_COEFF_SET0M), 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET0 */
    { DPI(DPI_MATRIX_COEFF_SET1), 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET1 */
    { DPI(DPI_MATRIX_COEFF_SET2), 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET2 */
    { DPI(DPI_MATRIX_COEFF_SET3), 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET3 */
    { DPI(DPI_MATRIX_COEFF_SET4), 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET4 */
    { DPI(DPI_MATRIX_PREADD_SET0), 0x00000000, 0 },  /* DPI_MATRIX_PREADD_SET0 */
    { DPI(DPI_MATRIX_PREADD_SET1), 0x00000000, 0 },  /* DPI_MATRIX_PREADD_SET1 */
    { DPI(DPI_MATRIX_POSTADD_SET0), 0x00000000, 0 },  /* DPI_MATRIX_POSTADD_SET0 */
    { DPI(DPI_MATRIX_POSTADD_SET1), 0x00000000, 0 },  /* DPI_MATRIX_POSTADD_SET1 */
    { DPI(DPI_CLPF_SETTING), 0x00000000, 0 },  /* DPI_CLPF_SET */
    { DPI(DPI_Y_LIMIT), 0x00000000, 0 },  /* DPI_Y_LIMIT */
    { DPI(DPI_C_LIMIT), 0x00000000, 0 },  /* DPI_C_LIMIT */
    { DPI(DPI_YUV422_SETTING), 0x00000000, 0 },  /* DPI_YUV422_S */
    { DPI(DPI_YCMUX_SETTING), 0x00000000, 0 },  /* DPI_YCMUX_SE */
    { DPI(DPI_EMBSYNC_SETTING), 0x00000000, 0 },  /* DPI_EMBSYNC_ */
    { DPI(DPI_OUTPUT_SETTING), 0x00000000, 0 },  /* DPI_OUTPUT_S */
    { DPI(DPI_TMODE), 0x00000000, 0 },  /* DPI_TMODE */
    { DPI(DPI_CHKSUM), 0x00000000, 0 },  /* DPI_CHKSUM */
    { DPI(DPI_PATTERN), 0x00000000, 0 },  /* DPI_PATTERN */
};

/*
 * WDMA - write DMA (capture path), 0x14004000
 *
 * Every register the datasheet documents, generated from the extracted
 * header, carrying its documented reset value. That distinction is the point:
 * a register reading 0 because nothing models it looks exactly like one that
 * is genuinely 0, whereas one reading its documented reset is evidence the
 * block is there at all.
 */

static const Mt6589DispReg mt6589_wdma_regs[] = {
    { WDMA(WDMA_INTEN), 0x00000000, 0 },  /* WDMA_INTEN */
    { WDMA(WDMA_RST), 0x00000000, 0 },  /* WDMA_RST */
    { WDMA(WDMA_CFG), 0x00000000, 0 },  /* WDMA_CFG */
    { WDMA(WDMA_SRC_SIZE), 0x00000000, 0 },  /* WDMA_SRC_SIZE */
    { WDMA(WDMA_CLIP_SIZE), 0x00000000, 0 },  /* WDMA_CLIP_SIZE */
    { WDMA(WDMA_ALPHA), 0x000000ff, 0 },  /* WDMA_ALPHA */
    { WDMA(WDMA_EXEC_DBG), 0x00000000, 0 },  /* WDMA_EXEC_DBG */
    { WDMA(WDMA_CLIP_DBG), 0x00000000, 0 },  /* WDMA_CLIP_DBG */
    { WDMA(WDMA_INTSTA), 0x00000000, 0 },  /* WDMA_INTSTA */
    { WDMA(WDMA_EN), 0x00000000, 0 },  /* WDMA_EN */
    { WDMA(WDMA_SMI_CON), 0x0000c000, 0 },  /* WDMA_SMI_CON */
    { WDMA(WDMA_CLIP_COORD), 0x00000000, 0 },  /* WDMA_CLIP_COORD */
    { WDMA(WDMA_DST_ADDR), 0x00000000, 0 },  /* WDMA_DST_ADDR */
    { WDMA(WDMA_DST_W_IN_BYTE), 0x00000000, 0 },  /* WDMA_DST_W_IN_BYTE */
    { WDMA(WDMA_STA), 0x00008000, 0 },  /* WDMA_STA */
    { WDMA(WDMA_BUF_CON1), 0x000001fe, 0 },  /* WDMA_BUF_CON1 */
    { WDMA(WDMA_BUF_CON2), 0x00000808, 0 },  /* WDMA_BUF_CON2 */
    { WDMA(WDMA_C00), 0x00000000, 0 },  /* WDMA_C00 */
    { WDMA(WDMA_C02), 0x00000000, 0 },  /* WDMA_C02 */
    { WDMA(WDMA_C10), 0x00000000, 0 },  /* WDMA_C10 */
    { WDMA(WDMA_C12), 0x00000000, 0 },  /* WDMA_C12 */
    { WDMA(WDMA_C20), 0x00000000, 0 },  /* WDMA_C20 */
    { WDMA(WDMA_C22), 0x00000000, 0 },  /* WDMA_C22 */
    { WDMA(WDMA_PRE_ADD0), 0x00000000, 0 },  /* WDMA_PRE_ADD0 */
    { WDMA(WDMA_PRE_ADD2), 0x00000000, 0 },  /* WDMA_PRE_ADD2 */
    { WDMA(WDMA_POST_ADD0), 0x00000000, 0 },  /* WDMA_POST_ADD0 */
    { WDMA(WDMA_POST_ADD2), 0x00000000, 0 },  /* WDMA_POST_ADD2 */
    { WDMA(WDMA_DST_U_ADDR), 0x00000000, 0 },  /* WDMA_DST_U_ADDR */
    { WDMA(WDMA_DST_V_ADDR), 0x00000000, 0 },  /* WDMA_DST_V_ADDR */
    { WDMA(WDMA_DST_UV_PITCH), 0x00000000, 0 },  /* WDMA_DST_UV_PITCH */
    { WDMA(WDMA_FLOW_CTRL_DBG), 0x00008000, 0 },  /* WDMA_FLOW_CTRL_DBG */
    { WDMA(WDMA_DUMMY), 0x00000000, 0 },  /* WDMA_DUMMY */
    { WDMA(WDMA_DITHER_0), 0x00000000, 0 },  /* WDMA_DITHER_0 */
    { WDMA(WDMA_DITHER_5), 0x00000000, 0 },  /* WDMA_DITHER_5 */
    { WDMA(WDMA_DITHER_6), 0x00000000, 0 },  /* WDMA_DITHER_6 */
    { WDMA(WDMA_DITHER_7), 0x00000000, 0 },  /* WDMA_DITHER_7 */
    { WDMA(WDMA_DITHER_8), 0x00000000, 0 },  /* WDMA_DITHER_8 */
    { WDMA(WDMA_DITHER_9), 0x00000000, 0 },  /* WDMA_DITHER_9 */
    { WDMA(WDMA_DITHER_10), 0x00000000, 0 },  /* WDMA_DITHER_10 */
    { WDMA(WDMA_DITHER_12), 0x00000000, 0 },  /* WDMA_DITHER_12 */
    { WDMA(WDMA_DITHER_13), 0x00000000, 0 },  /* WDMA_DITHER_13 */
    { WDMA(WDMA_DITHER_14), 0x00000000, 0 },  /* WDMA_DITHER_14 */
    { WDMA(WDMA_DITHER_15), 0x00000000, 0 },  /* WDMA_DITHER_15 */
    { WDMA(WDMA_DITHER_16), 0x00000000, 0 },  /* WDMA_DITHER_16 */
    { WDMA(WDMA_DITHER_17), 0x00000000, 0 },  /* WDMA_DITHER_17 */
};

/*
 * BLS - backlight scaling, 0x14008000
 *
 * Every register the datasheet documents, generated from the extracted
 * header, carrying its documented reset value. That distinction is the point:
 * a register reading 0 because nothing models it looks exactly like one that
 * is genuinely 0, whereas one reading its documented reset is evidence the
 * block is there at all.
 */

static const Mt6589DispReg mt6589_bls_regs[] = {
    { BLS(BLS_SRC_SIZE), 0x00000000, 0 },  /* BLS_SRC_SIZE */
    { BLS(BLS_HIS_PROT), 0x00000000, 0 },  /* BLS_HIS_PROT */
    { BLS(BLS_GAIN), 0x00000000, 0 },  /* BLS_GAIN */
    { BLS(BLS_SC_BIN_THD), 0x00000000, 0 },  /* BLS_SC_BIN_T */
    { BLS(BLS_EN), 0x00000000, 0 },  /* BLS_EN */
    { BLS(BLS_RST), 0x00000000, 0 },  /* BLS_RST */
    { BLS(BLS_BLS_SETTING), 0x00000000, 0 },  /* BLS_BLS_SETTING */
    { BLS(BLS_HIS_SETTING), 0x00000000, 0 },  /* BLS_HIS_SETTING */
    { BLS(BLS_INTEN), 0x00000000, 0 },  /* BLS_INTEN */
    { BLS(BLS_INTSTA), 0x00000000, 0 },  /* BLS_INTSTA */
    { BLS(BLS_DISTORT_POINT), 0x00000000, 0 },  /* BLS_DISTORT_POINT */
    { BLS(BLS_GAIN_SETTING), 0x00000000, 0 },  /* BLS_GAIN_SETTING */
    { BLS(BLS_GAMMA_SETTING), 0x00000000, 0 },  /* BLS_GAMMA_SETTING */
    { BLS(BLS_GAMMA_BOUNDARY), 0x00000000, 0 },  /* BLS_GAMMA_B */
    { BLS(BLS_LUT_UPDATE), 0x00000000, 0 },  /* BLS_LUT_UPDATE */
    { BLS(BLS_PWM_LUT_SEL), 0x00000000, 0 },  /* BLS_PWM_LUT_SEL */
    { BLS(BLS_MAXCLR_LIMIT), 0x000000ff, 0 },  /* BLS_MAXCLR_LIMIT */
    { BLS(BLS_PRE_DIST_THD), 0x00000000, 0 },  /* BLS_PRE_DIST_THD */
    { BLS(BLS_DIST_THD_NORM), 0x00000000, 0 },  /* BLS_DIST_THD_NORM */
    { BLS(BLS_DIST_THD_DARK), 0x00000000, 0 },  /* BLS_DIST_THD_DARK */
    { BLS(BLS_DIST_THD_BRIGHT), 0x00000000, 0 },  /* BLS_DIST_THD_BRIGHT */
    { BLS(BLS_DIST_THD_TEXT), 0x00000000, 0 },  /* BLS_DIST_THD_TEXT */
    { BLS(BLS_DS_SETTING), 0x00000000, 0 },  /* BLS_DS_SETTING */
    { BLS(BLS_BS_SETTING), 0x00000000, 0 },  /* BLS_BS_SETTING */
    { BLS(BLS_TS_SETTING0), 0x00000000, 0 },  /* BLS_TS_SETTING0 */
    { BLS(BLS_TS_SETTING1), 0x00000000, 0 },  /* BLS_TS_SETTING1 */
    { BLS(BLS_FAST_IIR_XCOEFF), 0x00000000, 0 },  /* BLS_FAST_IIR_ */
    { BLS(BLS_FAST_IIR_YCOEFF), 0x00000000, 0 },  /* BLS_FAST_IIR_ */
    { BLS(BLS_SLOW_IIR_XCOEFF), 0x00000000, 0 },  /* BLS_SLOW_IIR_XCOEFF */
    { BLS(BLS_SLOW_IIR_YCOEFF), 0x00000000, 0 },  /* BLS_SLOW_IIR_YCOEFF */
    { BLS(BLS_PWM_DUTY), 0x00000000, 0 },  /* BLS_PWM_DUTY */
    { BLS(BLS_PWM_DUTY_GAIN), 0x00000001, 0 },  /* BLS_PWM_DUTY_GAIN */
    { BLS(BLS_PATTERN), 0x00000000, 0 },  /* BLS_PATTERN */
    { BLS(BLS_PWM_CON), 0x00000000, 0 },  /* BLS_PWM_CON */
    { BLS(PWM_H_DURATION), 0x00008000, 0 },  /* PWM_H_DURATION */
    { BLS(PWM_L_DURATION), 0x00008000, 0 },  /* PWM_L_DURATION */
    { BLS(PWM_G_DURATION), 0x00000000, 0 },  /* PWM_G_DURATION */
    { BLS(PWM_SEND_DATA0), 0x00000000, 0 },  /* PWM_SEND_DATA0 */
    { BLS(PWM_SEND_DATA1), 0x00000000, 0 },  /* PWM_SEND_DATA1 */
    { BLS(PWM_WAVE_NUM), 0x00000000, 0 },  /* PWM_WAVE_NUM */
    { BLS(PWM_DATA_WIDTH), 0x00000000, 0 },  /* PWM_DATA_WIDTH */
    { BLS(PWM_THRESH), 0x00000000, 0 },  /* PWM_THRESH */
    { BLS(PWM_SEND_WAVENUM), 0x00000000, 0 },  /* PWM_SEND_W */
    { BLS(BLS_DARK_BIN), 0x00000000, 0 },  /* BLS_DARK_BIN */
    { BLS(BLS_BRIGHT_BIN), 0x00000000, 0 },  /* BLS_BRIGHT_BIN */
    { BLS(BLS_PWM_DUTY_RD), 0x000001fe, 0 },  /* BLS_PWM_DUTY_RD */
    { BLS(BLS_MAXCLR_RD), 0x0000ffff, 0 },  /* BLS_MAXCLR_RD */
    { BLS(BLS_GAIN_RD), 0x00000080, 0 },  /* BLS_GAIN_RD */
    { BLS(BLS_SC_RD), 0x00000000, 0 },  /* BLS_SC_RD */
    { BLS(BLS_DITHER_0), 0x00000000, 0 },  /* BLS_DITHER_0 */
    { BLS(BLS_DITHER_5), 0x00000000, 0 },  /* BLS_DITHER_5 */
    { BLS(BLS_DITHER_6), 0x00000000, 0 },  /* BLS_DITHER_6 */
    { BLS(BLS_DITHER_7), 0x00000000, 0 },  /* BLS_DITHER_7 */
    { BLS(BLS_DITHER_8), 0x00000000, 0 },  /* BLS_DITHER_8 */
    { BLS(BLS_DITHER_9), 0x00000000, 0 },  /* BLS_DITHER_9 */
    { BLS(BLS_DITHER_10), 0x00000000, 0 },  /* BLS_DITHER_10 */
    { BLS(BLS_DITHER_11), 0x00000000, 0 },  /* BLS_DITHER_11 */
    { BLS(BLS_DITHER_12), 0x00000000, 0 },  /* BLS_DITHER_12 */
    { BLS(BLS_DITHER_13), 0x00000000, 0 },  /* BLS_DITHER_13 */
    { BLS(BLS_DITHER_14), 0x00000000, 0 },  /* BLS_DITHER_14 */
    { BLS(BLS_DITHER_15), 0x00000000, 0 },  /* BLS_DITHER_15 */
    { BLS(BLS_DITHER_16), 0x00000000, 0 },  /* BLS_DITHER_16 */
    { BLS(BLS_DITHER_17), 0x00000000, 0 },  /* BLS_DITHER_17 */
};

/*
 * GAMMA - gamma correction, 0x14009000
 *
 * Every register the datasheet documents, generated from the extracted
 * header, carrying its documented reset value. That distinction is the point:
 * a register reading 0 because nothing models it looks exactly like one that
 * is genuinely 0, whereas one reading its documented reset is evidence the
 * block is there at all.
 */

static const Mt6589DispReg mt6589_gamma_regs[] = {
    { GAMMA(DISP_GAMMA_EN), 0x00000000, 0 },  /* DISP_GAMMA_EN */
    { GAMMA(DISP_GAMMA_RST), 0x00000000, 0 },  /* DISP_GAMMA_ */
    { GAMMA(DISP_GAMMA_LUT_CLK_EN), 0x00000000, 0 },  /* DISP_GAMMA_LUT_CLK_EN */
    { GAMMA(DISP_GAMMA_INTEN), 0x00000000, 0 },  /* DISP_GAMMA_I */
    { GAMMA(DISP_GAMMA_INTSTA), 0x00000000, 0 },  /* DISP_GAMMA_I */
    { GAMMA(DISP_GAMMA_DITHER_0), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_0 */
    { GAMMA(DISP_GAMMA_DITHER_4), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_4 */
    { GAMMA(DISP_GAMMA_DITHER_5), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_5 */
    { GAMMA(DISP_GAMMA_DITHER_6), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_6 */
    { GAMMA(DISP_GAMMA_DITHER_7), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_7 */
    { GAMMA(DISP_GAMMA_DITHER_8), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_8 */
    { GAMMA(DISP_GAMMA_DITHER_9), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_9 */
    { GAMMA(DISP_GAMMA_DITHER_10), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_10 */
    { GAMMA(DISP_GAMMA_DITHER_11), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_11 */
    { GAMMA(DISP_GAMMA_DITHER_12), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_12 */
    { GAMMA(DISP_GAMMA_DITHER_13), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_13 */
    { GAMMA(DISP_GAMMA_DITHER_14), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_14 */
    { GAMMA(DISP_GAMMA_DITHER_15M), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_15 */
    { GAMMA(DISP_GAMMA_DITHER_16), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_16 */
    { GAMMA(DISP_GAMMA_DITHER_17), 0x00000000, 0 },  /* DISP_GAMMA_DITHER_17 */
};

static const Mt6589DispBlock mt6589_disp_blocks[] = {
    /*
     * DISPSYS_CONFIG at 0x1400_0000 - the DDP routing table. This is where the path is
     * actually wired: OVL_MOUT_EN decides which consumer the overlay feeds, RDMA0_OUT_SEL
     * and COLOR_SEL/TDSHP_SEL/BLS_SEL/DPI0_SEL chain the following stages, and OVL_SEL
     * chooses the overlay's own source. Everything here is read-modify-write storage
     * (the driver sets and clears bits), so leaving the block unmapped meant every read
     * came back 0 and the driver's own view of the route drifted from what it wrote.
     * The generated header mt6589-dispsys_config-regs.h lists the 48 registers.
     */
    { "config", 0x14000000, NULL, 0, false, 0, true },
    BLOCK("ovl",   MT6589_DISP_OVL_BASE,    mt6589_ovl_regs,   163),
    BLOCK("rdma",  MT6589_DISP_RDMA_BASE,   mt6589_rdma_regs,  0),
    BLOCK("color", MT6589_DISP_COLOR_BASE,  mt6589_color_regs, 0),
    BLOCK("mutex", MT6589_DISP_MUTEX_BASE,  mt6589_mutex_regs, 192),
    BLOCK("wdma",  MT6589_DISP_WDMA_BASE,   mt6589_wdma_regs,  0),
    BLOCK("bls",   MT6589_DISP_BLS_BASE,    mt6589_bls_regs,   0),
    BLOCK("gamma", MT6589_DISP_GAMMA_BASE,  mt6589_gamma_regs, 0),
    BLOCK("dsi",   MT6589_DSI_BASE,         mt6589_dsi_regs,   204),
    BLOCK("dpi",   MT6589_DPI_BASE,         mt6589_dpi_regs,   205),
};

/* ------------------------------------------------------------------------- */

static bool mt6589_disp_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = getenv("MT6589_DISP_LOG") != NULL;
    }
    return enabled;
}

static const Mt6589DispReg *find_reg(const Mt6589DispBlock *b,
                                            hwaddr offset)
{
    static const Mt6589DispReg plain = { 0, 0, 0 };

    if (b->generic_regs) {
        /* Storage only: see the comment on the block's description. */
        return (offset < 0x1000) ? &plain : NULL;
    }
    for (size_t i = 0; i < b->nregs; i++) {
        if (b->regs[i].offset == offset) {
            return &b->regs[i];
        }
    }
    return NULL;
}

/* ========================================================================== */
/* OVL scanout: real pixels into a QEMU console                               */
/*
 * The composer is the one part of this subsystem whose RESULT can be checked, so it
 * is the one part that is modelled rather than stored: on every frame the enabled
 * layers are read out of guest memory and blitted into a display surface, which is
 * what a panel would be showing. Without it the model can only prove the guest
 * programmed the right registers, not that anything comes out - and "the backlight
 * lights but the screen is black" is exactly the class of bug that needs the second
 * kind of proof.
 *
 * Format is derived from the geometry the guest wrote rather than from a format
 * field, because that is unambiguous: LK's layer has pitch 0xa00 for a width of
 * 0x500, so 2 bytes per pixel, i.e. RGB565. Three and four byte pixels (RGB888 and
 * ARGB8888) come out of the same arithmetic.
 *
 * Frame rate: the surface is refreshed at 20 Hz. The guest's own vsync cadence is
 * already satisfied by the RDMA status path, so this only feeds the human looking at
 * the window, and re-reading 2 MB of guest memory 60 times a second to show a static
 * splash would be a waste of the host CPU.
 */
#define OVL_ROI_SIZE_REG  OVL(OVL_ROI_SIZE)
#define OVL_BGCLR_REG     OVL(OVL_ROI_BGCLR)
#define OVL_EN_REG        OVL(OVL_EN)
#define OVL_L_CON(n)      (OVL(OVL_L0_CON) + (n) * 0x20)
#define OVL_L_SIZE(n)     (OVL(OVL_L0_SRC_SIZE) + (n) * 0x20)
#define OVL_L_ADDR(n)     (OVL(OVL_L0_ADDR) + (n) * 0x20)
#define OVL_L_PITCH(n)    (OVL(OVL_L0_PITCH) + (n) * 0x20)
#define OVL_NUM_LAYERS    4
#define OVL_FRAME_NS      (1000000000ull / 20)

/* One guest pixel, converted to the surface's 0xXXRRGGBB layout. */
static uint32_t mt6589_disp_cvt_pixel(const uint8_t *p, int bpp)
{
    switch (bpp) {
    case 1:
        return ((uint32_t)p[0] << 16) | ((uint32_t)p[0] << 8) | p[0];
    case 2: {
        uint32_t v = p[0] | ((uint32_t)p[1] << 8);

        return (((v >> 11) & 0x1f) << 19) | (((v >> 5) & 0x3f) << 10) |
               ((v & 0x1f) << 3);
    }
    case 3:
    case 4:
    default:
        /* Guest memory is little-endian B, G, R [, A]. */
        return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
    }
}

/*
 * MT6589_DISP_DUMP=<path> writes the composed frame out as a PPM.
 *
 * This is how the scanout is verified at all: the emulator is built
 * --without-default-features, which leaves it with neither an HMP monitor nor QMP
 * screendump (measured: "HMP monitor is not available, use '-qmp'", and then
 * "The command screendump has not been found"), so the model has to be able to show
 * what it produced. Written on every 20th frame, so a run leaves one recent picture
 * instead of gigabytes of them.
 */
static void mt6589_disp_dump(Mt6589DispBlockState *s, DisplaySurface *surface,
                             int w, int h)
{
    const char *path = getenv("MT6589_DISP_DUMP");
    g_autofree char *tmp = NULL;
    int stride;
    uint8_t *data;
    uint32_t sum = 0;
    FILE *f;

    if (!path || !s->frame_count) {
        return;
    }
    stride = surface_stride(surface);
    data = surface_data(surface);
    /*
     * Only write the file when the picture actually changed. Sampling every 64th
     * pixel is enough to notice a change and makes the check cheap enough to run
     * every frame, which is what makes the dump meaningful: it ends up holding the
     * LAST frame that had content in it, instead of whichever frame happened to
     * land on a modulo boundary before the guest drew anything.
     */
    for (int y = 0; y < h; y += 8) {
        const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);

        for (int x = 0; x < w; x += 8) {
            sum = sum * 31u + row[x];
        }
    }
    /*
     * Change-triggered only. A periodic write as well seemed safer - a recent file is always
     * there - but it is actively worse: the guest keeps switching framebuffers, so the
     * periodic write replaces a frame that had the picture with a later one that does not, and
     * an unattended check then compares against a blank. Measured: a run that had rendered the
     * boot logo left an all-black file.
     */
    /*
     * Write when the picture changes, and also every hundred frames while the frame has
     * content. Content-only periodic writes are the part that matters: the guest switches
     * framebuffers and the kernel later clears the path, so a periodic write of a blank frame
     * would replace the picture with nothing - but waiting for a change alone means the file
     * exists only if the frame timer happened to sample inside the window where the guest's
     * drawing was on screen, and that window is short.
     */
    bool periodic_content = sum != 0 && (s->frame_count % 100) == 0;

    if (sum == s->last_sum && !periodic_content) {
        return;
    }
    /*
     * Never replace a picture with a blank. The guest switches framebuffers - LK double-buffers
     * its logo and the kernel then clears the path - so "last frame written" and "last frame
     * with something in it" are different things, and only the second is worth keeping for a
     * human looking at the file or a check comparing against it.
     */
    if (!sum && s->dumped_content) {
        return;
    }
    s->last_sum = sum;
    s->dumped_content = (sum != 0);
    tmp = g_strdup_printf("%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        const uint32_t *row = (const uint32_t *)(data + (size_t)y * stride);

        for (int x = 0; x < w; x++) {
            uint32_t p = row[x];
            uint8_t rgb[3] = { (p >> 16) & 0xff, (p >> 8) & 0xff, p & 0xff };

            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
    rename(tmp, path);
}

/*
 * The kernel's path is not the same as LK's. LK drives the OVL layers and leaves the RDMA
 * alone; the kernel configures the read DMA instead -
 *
 *     RDMA: w=1280, h=800, addr=0, pitch=3840, mode=0
 *
 * (ddp_rdma.c) - so a composer that only understands OVL layers shows nothing once the
 * kernel takes over. Both are rendered here: whichever has an address and a pitch is
 * what the panel would be showing.
 */
/*
 * Translate a display address through the M4U.
 *
 * The kernel does not hand the display physical addresses: mtkfb maps its framebuffer and
 * prints "[MTKFB] FB MVA is 0x00040000 PA is 0xBF600000", and the OVL then gets the MVA.
 * A composer that reads the MVA as if it were physical reads whatever happens to live at
 * 0x40000 - which is kernel text - and shows noise or nothing.
 *
 * The table format is measured, not assumed. The M4U's global block holds the page-table
 * base at +0x10 (REG_MMUg_PT_BASE), and dumping it after a boot shows a flat table of 4 KiB
 * pages: entry index = MVA >> 12, and
 *
 *     L1[0x040] = 0xbf60000a      MVA 0x40000 -> PA 0xbf600000
 *
 * which is exactly the mapping the driver reported. So the low bits are flags, the address
 * is entry & ~0xfff, and 960 consecutive entries covered the mapped framebuffer.
 */
static hwaddr mt6589_disp_m4u(Mt6589DispBlockState *s, hwaddr addr)
{
    uint32_t table, entry = 0;

    if (!s->soc || !addr || addr >= 0x80000000) {
        return addr;            /* already physical, or nothing to translate */
    }
    table = s->soc->m4u_regs[0x0010 >> 2];
    if (!table) {
        return addr;
    }
    if (address_space_read(&address_space_memory, table + ((addr >> 12) << 2),
                           MEMTXATTRS_UNSPECIFIED, &entry, 4) != MEMTX_OK) {
        return addr;
    }
    if (!entry) {
        return addr;            /* unmapped: leave it, the read will fail honestly */
    }
    return (entry & ~0xfffu) | (addr & 0xfffu);
}

static bool mt6589_disp_render_fb(Mt6589DispBlockState *s, hwaddr addr,
                                  uint32_t pitch, uint32_t size,
                                  uint32_t *dst, int stride_bytes, int dw, int dh)
{
    uint8_t line[4096 * 4];
    int lw = size & 0xffff;
    int lh = size >> 16;
    int bpp;

    if (!addr || !pitch) {
        return false;
    }
    if (!lw || !lh) {
        lw = dw;
        lh = dh;
    }
    bpp = pitch / lw;
    if (bpp < 1 || bpp > 4) {
        return false;
    }
    lw = MIN(lw, dw);
    lh = MIN(lh, dh);
    lw = MIN(lw, (int)(sizeof(line) / bpp));
    for (int y = 0; y < lh; y++) {
        uint32_t *d = (uint32_t *)((uint8_t *)dst + (size_t)y * stride_bytes);

        if (address_space_read(&address_space_memory,
                               mt6589_disp_m4u(s, addr + (hwaddr)y * pitch),
                               MEMTXATTRS_UNSPECIFIED, line,
                               (hwaddr)lw * bpp) != MEMTX_OK) {
            return false;
        }
        for (int x = 0; x < lw; x++) {
            d[x] = mt6589_disp_cvt_pixel(line + x * bpp, bpp);
        }
    }
    return true;
}

/*
 * Which source reaches the panel.
 *
 * DDP is a mux, not a fixed pipeline: DISP_OVL_MOUT_EN says where the overlay's output
 * goes, COLOR_MOUT_EN where the colour engine's goes, and so on, with BLS_SEL and DPI0_SEL
 * picking the last stage. The values are documented where the driver writes them
 * (ddp_path.c):
 *
 *     OVL_MOUT_EN |= 0x1     "ovl_mout output to bls"
 *     OVL_MOUT_EN  = 0x4     "ovl_mout output to COLOR"
 *     COLOR_MOUT_EN = 0x8    "color_mout output to BLS"
 *     TDSHP_MOUT_EN = 0x10   "tdshp_mout output to OVL directlink"
 *
 * and the measurement of what this kernel actually programs agrees with the second and
 * third of those (OVL_MOUT_EN = 0x4, COLOR_MOUT_EN = 0x8), i.e. the visible path here is
 * OVL -> COLOR -> BLS. So the overlay is what the panel shows, and the read DMA is only
 * the source when the route says so.
 */
static bool mt6589_disp_route_is_ovl(Mt6589DispBlockState *ovl)
{
    Mt6589DispBlockState *cfg = ovl->config;
    uint32_t ovl_mout, color_mout;

    if (!cfg) {
        return true;            /* no routing modelled: the overlay is the default */
    }
    ovl_mout = cfg->regs[0x0024 >> 2];
    color_mout = cfg->regs[0x0028 >> 2];

    if (!ovl_mout && !color_mout) {
        return true;            /* nothing programmed yet: the overlay is the default */
    }
    /* OVL straight to BLS, or OVL -> COLOR -> BLS. */
    return (ovl_mout & 0x1u) || ((ovl_mout & 0x4u) && (color_mout & 0x8u));
}

static void mt6589_ovl_compose(Mt6589DispBlockState *s)
{
    DisplaySurface *surface;
    uint32_t roi;
    int w, h, stride;
    uint32_t *dst;
    uint8_t line[4096 * 4];

    if (!s->con) {
        return;
    }
    surface = qemu_console_surface(s->con);
    if (!surface) {
        return;
    }
    roi = s->regs[OVL_ROI_SIZE_REG >> 2];
    w = roi & 0xffff;
    h = roi >> 16;
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        return;
    }
    if (surface_width(surface) != w || surface_height(surface) != h) {
        /*
         * Not while the UI is inside gfx_update: resizing there re-enters the
         * graphic layer from its own callback.
         */
        if (s->in_gfx_update) {
            return;
        }
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
        if (!surface || surface_width(surface) != w || surface_height(surface) != h) {
            return;
        }
    }
    if (surface_format(surface) != PIXMAN_x8r8g8b8) {
        static bool warned;

        if (!warned) {
            warned = true;
            qemu_log_mask(LOG_UNIMP, "%s: console is not x8r8g8b8, scanout "
                          "disabled\n", __func__);
        }
        return;
    }

    stride = surface_stride(surface);
    dst = (uint32_t *)surface_data(surface);
    /* Background: black unless the guest programmed a colour at 0x38. */
    {
        uint32_t bg = s->regs[OVL_BGCLR_REG >> 2];

        bg = ((bg & 0xff) << 16) | (bg & 0xff00) | ((bg >> 16) & 0xff);
        memset(dst, 0, (size_t)stride * h);
        if (bg) {
            for (int y = 0; y < h; y++) {
                uint32_t *d = dst + (size_t)y * (stride / 4);

                for (int x = 0; x < w; x++) {
                    d[x] = bg;
                }
            }
        }
    }

    bool from_ovl = mt6589_disp_route_is_ovl(s);

    /*
     * A route that selects a source with nothing behind it falls back to the other one:
     * the point of this model is to show what the panel would show, and a driver that has
     * not finished programming the route should not produce a blank screen.
     */
    if (!from_ovl) {
        bool rdma_ready = s->rdma &&
            (s->rdma->regs[RDMA(DISP_RDMA_GLOBAL_CON) >> 2] & 1u) &&
            s->rdma->regs[RDMA(DISP_RDMA_MEM_START_ADDR) >> 2];

        if (!rdma_ready) {
            from_ovl = true;
        }
    }

    for (int n = 0; from_ovl && n < OVL_NUM_LAYERS; n++) {
        mt6589_disp_render_fb(s, s->regs[OVL_L_ADDR(n) >> 2],
                              s->regs[OVL_L_PITCH(n) >> 2],
                              s->regs[OVL_L_SIZE(n) >> 2],
                              dst, stride, w, h);
    }

    /*
     * The read DMA, if the guest is driving that path instead of the overlay. This is
     * what the kernel configures, and its output is the whole frame, so it is drawn last.
     */
    if (!from_ovl && s->rdma && (s->rdma->regs[RDMA(DISP_RDMA_GLOBAL_CON) >> 2] & 1u)) {
        hwaddr addr = s->rdma->regs[RDMA(DISP_RDMA_MEM_START_ADDR) >> 2];
        uint32_t pitch = s->rdma->regs[RDMA(DISP_RDMA_MEM_SRC_PITCH) >> 2];
        uint32_t size = (s->rdma->regs[RDMA(DISP_RDMA_SIZE_CON_0) >> 2] & 0xffff) |
                        ((s->rdma->regs[RDMA(DISP_RDMA_SIZE_CON_1) >> 2] & 0xffff) << 16);

        mt6589_disp_render_fb(s->rdma, addr, pitch, size, dst, stride, w, h);
    }

    if (mt6589_disp_log_enabled() && (s->frame_count % 200) == 0) {
        /*
         * Report the layer that actually carries the picture - the last one with an address -
         * rather than layer 0, which is usually unused and made an earlier trace read as
         * "the composer sees nothing" when it was seeing plenty. A hash of the composed
         * pixels says whether there is a picture at all, which the top-left pixel does not
         * (a centred logo leaves it black).
         */
        uint32_t last_addr = 0, last_pitch = 0;
        uint32_t hash = 0;

        for (int n = 0; n < OVL_NUM_LAYERS; n++) {
            uint32_t a = s->regs[OVL_L_ADDR(n) >> 2];

            if (a) {
                last_addr = a;
                last_pitch = s->regs[OVL_L_PITCH(n) >> 2];
            }
        }
        for (int y = 0; y < h; y += 16) {
            for (int x = 0; x < w; x += 16) {
                hash = hash * 31u + dst[(size_t)y * (stride / 4) + x];
            }
        }
        fprintf(stderr, "DISP frame %u: %dx%d src=%08" PRIx32 " pitch=%u "
                "route=%s hash=%08" PRIx32 "\n", s->frame_count, w, h, last_addr,
                last_pitch, from_ovl ? "ovl" : "rdma", hash);
    }
    mt6589_disp_dump(s, surface, w, h);
    qemu_console_update(s->con, 0, 0, w, h);
}

static void mt6589_ovl_frame_tick(void *opaque)
{
    Mt6589DispBlockState *s = opaque;

    if (!(s->regs[OVL_ROI_SIZE_REG >> 2])) {
        return;                             /* nothing programmed: stop scanning */
    }
    s->frame_count++;
    mt6589_ovl_compose(s);
    timer_mod(&s->frame_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OVL_FRAME_NS);
}

/*
 * Called after any OVL register write: as soon as the guest has programmed a region
 * of interest, start producing frames. Tying this to OVL_EN alone would miss LK,
 * which programs the layers and the ROI before it sets the enable bit.
 */
static void mt6589_ovl_kick(Mt6589DispBlockState *s)
{
    if (!s->con) {
        return;
    }
    if (!(s->regs[OVL_ROI_SIZE_REG >> 2])) {
        timer_del(&s->frame_timer);
        return;
    }
    if (!timer_pending(&s->frame_timer)) {
        timer_mod(&s->frame_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OVL_FRAME_NS);
    }
}

static bool mt6589_ovl_gfx_update(void *opaque)
{
    Mt6589DispBlockState *s = opaque;

    s->in_gfx_update = true;
    mt6589_ovl_compose(s);
    s->in_gfx_update = false;
    return true;
}

static const GraphicHwOps mt6589_ovl_gfx_ops = {
    .gfx_update = mt6589_ovl_gfx_update,
};

static uint64_t mt6589_disp_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589DispBlockState *s = opaque;
    static int read_log = -1;

    /*
     * MT6589_DISP_READ_LOG=1 traces reads as well as writes. A guest that is waiting is usually
     * polling a status register, and the write trace shows what it configured but not what it is
     * waiting on - that is in the reads.
     */
    if (read_log < 0) {
        read_log = getenv("MT6589_DISP_READ_LOG") != NULL;
    }
    if (read_log) {
        uint32_t pc = current_cpu ? cpu_env(current_cpu)->regs[15] : 0;

        fprintf(stderr, "DISP %-5s R %04" HWADDR_PRIx " pc=%08" PRIx32 "\n",
                s->desc->name, offset, pc);
    }
    const Mt6589DispReg *r = find_reg(s->desc, offset);

    if (!r) {
        qemu_log_mask(LOG_UNIMP, "%s(%s): read of unmodelled offset 0x%"
                      HWADDR_PRIx "\n", __func__, s->desc->name, offset);
        return 0;
    }
    if (r->flags & MREG_IDLE) {
        return r->reset;
    }
    /*
     * RDMA frame start.
     *
     * LK's DSI_WaitVsync() is a poll, not an interrupt wait:
     *
     *     MASKREG32(0x14006004, 0x2, 0x0);
     *     while ((INREG32(0x14006004) & 0x2) != 0x2) { ...timeout... }
     *
     * i.e. it clears bit 1 of RDMA INT_STATUS and waits for the hardware to set
     * it again. A write-1-to-clear latch can never satisfy that, because the
     * driver clears by writing zero. Report the bit as set while the engine is
     * programmed - a running read DMA raises frame-start every frame, which is
     * the behaviour the wait is modelling - and let the write path still latch
     * it for drivers that do use the interrupt.
     */
    if ((s->desc->base == MT6589_DISP_RDMA_BASE || s->desc->base == MT6589_DISP_RDMA_BASE + 0x1000) &&
        offset == 0x0004 &&
        (s->regs[0x0010 >> 2] & 1u || s->regs[0x0040 >> 2])) {
        /*
         * Bit 1 is frame start and bit 0 is frame done. RDMAWait() polls bit 0 with no
         * timeout at all:
         *
         *     while ((DISP_REG_GET(idx * DISP_INDEX_OFFSET +
         *                          DISP_REG_RDMA_INT_STATUS) & 0x1) != 0x1) ;
         *
         * (ddp_rdma.c), so leaving it clear hangs the display path with no way out.
         * Reporting both while the engine is programmed is the honest simplification
         * for a model with no memory bandwidth: a transfer takes no time, and the
         * driver's clear is a write of zero, which the write-1-to-clear rule ignores -
         * the same thing hardware does when every frame finishes as fast as it starts.
         */
        return s->regs[offset >> 2] | 0x3u;
    }
    return s->regs[offset >> 2];
}

/*
 * Display mutex interrupt.
 *
 * LK's DISP_Init waits for the mutex to report that a module has started a
 * frame:
 *
 *     DISP/Wait for DISP MUTEX0 IRQ timeout!!!
 *
 * and with the timer interrupt misrouted that wait used to expire and LK carried
 * on anyway. Once the tick was correct the wait became real, so the mutex has to
 * actually raise its interrupt.
 *
 * MT6589's DISP MUTEX is at GIC_SPI 160, i.e. GIC line 32 + 160 = 192
 * (mainline: mutex@14011000, interrupts = <GIC_SPI 160 ...>).
 *
 * Behaviour: MUTEX0_EN enables the block, MUTEX0_MOD names the module that owns
 * it, and a write to MUTEX0_SOF asks for a synchronisation event - which is when
 * the interrupt is raised. MUTEX_INTSTA is write-1-to-clear (MREG_W1C), and
 * MUTEX_INTEN gates it. Raising the status regardless of INTEN and letting the
 * level be status & enable is the same rule used by the MSDC model and by the
 * generic path below.
 */

/*
 * DSI and DPI events.
 *
 * Both interfaces raise interrupts of their own, and a framebuffer driver that waits for the
 * first vsync waits on one of them rather than on the mutex. The datasheet's bit assignments for
 * these INTSTA registers are not established, so rather than pick a bit and call it vsync, the
 * model raises exactly the events the guest has enabled: status = enable, once per frame, and the
 * line is (status & enable) as everywhere else. A driver that asked for an event gets one; nothing
 * is invented about which bit means what.
 */
/* OVL keeps its interrupt pair two registers earlier than the panel blocks. */
static uint32_t mt6589_disp_int_en_reg(const Mt6589DispBlockState *s)
{
    return (s->desc->base == MT6589_DISP_OVL_BASE) ? OVL(OVL_INTEN) : DSI(DSI_INTEN);
}

static uint32_t mt6589_disp_int_sta_reg(const Mt6589DispBlockState *s)
{
    return (s->desc->base == MT6589_DISP_OVL_BASE) ? OVL(OVL_INTSTA) : DSI(DSI_INTSTA);
}

static void mt6589_disp_panel_irq(Mt6589DispBlockState *s)
{
    uint32_t en = s->regs[mt6589_disp_int_en_reg(s) >> 2];
    uint32_t sta = s->regs[mt6589_disp_int_sta_reg(s) >> 2];

    if (s->desc->has_irq) {
        qemu_set_irq(s->irq, (en & sta) != 0);
    }
}

static void mt6589_disp_panel_tick(void *opaque)
{
    Mt6589DispBlockState *s = opaque;
    uint32_t en = s->regs[mt6589_disp_int_en_reg(s) >> 2];

    if (!en) {
        return;                 /* nothing enabled: stop until the guest asks again */
    }
    s->panel_count++;
    s->regs[mt6589_disp_int_sta_reg(s) >> 2] |= en;
    mt6589_disp_panel_irq(s);
    timer_mod(&s->panel_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OVL_FRAME_NS);
}

static void mt6589_disp_panel_kick(Mt6589DispBlockState *s)
{
    if (!s->desc->has_irq || !s->regs[mt6589_disp_int_en_reg(s) >> 2]) {
        return;
    }
    if (!timer_pending(&s->panel_timer)) {
        timer_mod(&s->panel_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + OVL_FRAME_NS);
    }
}

static void mt6589_disp_mutex_update_irq(Mt6589DispBlockState *s)
{
    /* MUTEX_INTEN at 0x0000, MUTEX_INTSTA at 0x0004. */
    uint32_t en = s->regs[MUTEX(DISP_MUTEX_INTEN) >> 2];
    uint32_t sta = s->regs[MUTEX(DISP_MUTEX_INTSTA) >> 2];
    bool level = (en & sta) != 0;

    if (s->desc->has_irq) {
        /*
         * Log the rising edges when tracing: "the driver waits for an interrupt" and "the
         * interrupt is raised" are different claims, and the trace is what tells them apart.
         */
        static bool last_level;
        static unsigned rises;

        if (mt6589_disp_log_enabled() && level && !last_level && (rises++ < 8)) {
            fprintf(stderr, "DISP mutex line -> 1 (rise #%u, inten=%08" PRIx32
                    " intsta=%08" PRIx32 ")\n", rises, en, sta);
        }
        last_level = level;
        qemu_set_irq(s->irq, level);
    }
}

/*
 * Frame start.
 *
 * The mutex does not only report when a module writes MUTEXn_SOF - it raises its event
 * every time a frame begins, and everything downstream synchronises on that: the DSI
 * and DPI paths wait for the first frame, and the kernel's display thread sleeps until
 * an interrupt tells it a frame has gone out. With only the write-triggered event
 * modelled, the kernel's own atomic-commit path waits for a first frame that never
 * arrives.
 *
 * So while any mutex is enabled the event is raised every frame period (60 Hz, which
 * is what the panel mode asks for). The interrupt is level-triggered on
 * INTSTA & INTEN, as before, so acknowledging it with MUTEX_INTSTA keeps working.
 */
#define MUTEX_VSYNC_NS  (1000000000ull / 60)

static void mt6589_disp_mutex_vsync(void *opaque)
{
    Mt6589DispBlockState *s = opaque;
    /*
     * A mutex is active when it has been enabled *or* acquired. This kernel never writes
     * MUTEXn_EN - measured across a whole boot: CFG_MUTEX(0) is written 1 then 0, and 0x20 is not
     * written at all - while it does take the mutex and then waits for the per-frame event that
     * the mutex raises. Gating the events on EN alone meant they were never raised, and
     * mtkfb_probe's wait_event (prepare_to_wait/schedule_timeout in the binary) could not complete.
     */
    /*
     * Once the mutex unit has been programmed it raises its event every frame, whatever the
     * request state is at that instant. Gating on "currently held" fails for the same reason
     * gating on EN did: the driver releases the mutex and *then* waits for the event, so a tick
     * that arrived between the two found nothing to report - measured as zero rising edges on the
     * interrupt line across a whole boot.
     */
    if (!s->panel_count && !(s->regs[MUTEX(DISP_MUTEX0_MOD) >> 2] ||
                             s->regs[MUTEX(DISP_MUTEX0) >> 2] ||
                             s->regs[MUTEX(DISP_MUTEX0_EN) >> 2])) {
        return;
    }
    s->vsync_count++;
    s->regs[MUTEX(DISP_MUTEX_INTSTA) >> 2] |= 1u;   /* event pending */
    mt6589_disp_mutex_update_irq(s);
    timer_mod(&s->vsync_timer,
              qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MUTEX_VSYNC_NS);
}

static void mt6589_disp_mutex_kick(Mt6589DispBlockState *s)
{
    /*
     * A mutex starts raising events once it has been programmed. This kernel never writes
     * MUTEXn_EN (0x20) at all - measured - and it releases the mutex before waiting for the event,
     * so neither "enabled" nor "currently held" is the right condition; "has been used" is.
     */
    bool enabled = (s->regs[MUTEX(DISP_MUTEX0_MOD) >> 2] ||
                    s->regs[MUTEX(DISP_MUTEX0) >> 2] ||
                    s->regs[MUTEX(DISP_MUTEX0_EN) >> 2]) != 0;
    static int no_vsync = -1;

    /*
     * MT6589_DISP_NOVSYNC=1 switches the per-frame event off, purely so its effect can
     * be measured: a periodic interrupt that the guest handles by re-running its display
     * configuration would look like a live guest making no progress, and the only way to
     * tell that from a guest waiting for a frame is to run both ways.
     */
    if (no_vsync < 0) {
        no_vsync = getenv("MT6589_DISP_NOVSYNC") != NULL;
    }
    if (no_vsync) {
        return;
    }

    if (!s->desc->has_irq) {
        return;
    }
    if (enabled && !timer_pending(&s->vsync_timer)) {
        timer_mod(&s->vsync_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MUTEX_VSYNC_NS);
    } else if (!enabled) {
        timer_del(&s->vsync_timer);
    }
}

static void mt6589_disp_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size);

static void mt6589_disp_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Mt6589DispBlockState *s = opaque;
    const Mt6589DispReg *r = find_reg(s->desc, offset);

    /*
     * Set MT6589_DISP_LOG=1 to see every display register write. This is how the
     * scanout model below was derived: the guest's own programming sequence is
     * the specification, and it is measured rather than guessed.
     */
    if (mt6589_disp_log_enabled()) {
        fprintf(stderr, "DISP %-5s W %04" HWADDR_PRIx " = %08" PRIx32 "\n",
                s->desc->name, offset, (uint32_t)value);
    }

    if (!r) {
        qemu_log_mask(LOG_UNIMP, "%s(%s): write 0x%" PRIx64 " to unmodelled "
                      "offset 0x%" HWADDR_PRIx "\n", __func__,
                      s->desc->name, value, offset);
        return;
    }
    if (r->flags & MREG_IDLE) {
        /* Idle status: not writable. */
        return;
    }
    if (r->flags & MREG_W0C) {
        /*
         * MTK's display interrupt handler acknowledges with ~status, i.e. it writes zero to the
         * bits it saw set. Treating this register as write-1-to-clear left bit 0 set for ever, and
         * the level-triggered line then delivered 1.8 million interrupts in 70 seconds - measured.
         */
        s->regs[offset >> 2] &= (uint32_t)value;
        if (s->desc->base == MT6589_DISP_MUTEX_BASE) {
            mt6589_disp_mutex_update_irq(s);
        }
        return;
    }
    if (r->flags & MREG_W1C) {
        s->regs[offset >> 2] &= ~((uint32_t)value);
        if (s->desc->base == MT6589_DISP_MUTEX_BASE) {   /* MUTEX: INTSTA just cleared */
            mt6589_disp_mutex_update_irq(s);
        }
        return;
    }
    if (r->flags & MREG_SELFCL) {
        /*
         * A trigger: the write takes effect and the bit reads back 0, which is
         * what "started" looks like. Reading back the written value makes a guest
         * that polls for it to clear wait for ever.
         */
        return;
    }
    s->regs[offset >> 2] = value;

    /*
     * A write to a MUTEXn_SOF register requests a sync event; that is the point
     * at which the mutex reports to the interrupt controller. The SOF registers
     * are at 0x002c and 0x004c with MUTEX0_EN at 0x0020 / MUTEX1_EN at 0x0040.
     */
    if (s->desc->base == MT6589_DISP_MUTEX_BASE) {
        /*
         * CFG_MUTEX(n) at 0x24 + n*0x20: bit 0 is the request the driver writes and
         * bit 1 is the grant, which is what the driver actually waits for:
         *
         *     DISP_REG_SET(DISP_REG_CONFIG_MUTEX(mutexID), 1);
         *     while ((DISP_REG_GET(DISP_REG_CONFIG_MUTEX(mutexID))
         *             & DISP_INT_MUTEX_BIT_MASK) != DISP_INT_MUTEX_BIT_MASK)
         *
         * and DISP_INT_MUTEX_BIT_MASK is 0x2, not 0x1. Storing the write verbatim
         * leaves bit 1 clear, so the poll gives up after 10000 tries and the driver
         * prints "error:disp_path_get_mutex() timeout! mutexID=0" and stops the whole
         * display pipeline - measured. Nothing else competes for the mutex here, so
         * the grant is immediate and mirrors the request.
         */
        if (offset == MUTEX(DISP_MUTEX0) || offset == MUTEX(DISP_MUTEX1)) {
            uint32_t v = (uint32_t)value;

            s->regs[offset >> 2] = (v & ~2u) | ((v & 1u) ? 2u : 0);
        }
        if (offset == MUTEX(DISP_MUTEX0_SOF) || offset == MUTEX(DISP_MUTEX1_SOF)) {
            uint32_t en_reg = (offset == MUTEX(DISP_MUTEX0_SOF))
                              ? MUTEX(DISP_MUTEX0_EN) : MUTEX(DISP_MUTEX1_EN);
            if (s->regs[en_reg >> 2] & 1u) {
                s->regs[0x0004 >> 2] |= 1u;   /* MUTEX_INTSTA */
                mt6589_disp_mutex_update_irq(s);
            }
        } else if (offset == 0x0000) {
            /* INTEN changed: the level follows status & enable. */
            mt6589_disp_mutex_update_irq(s);
        }
        /* Enabling, disabling or acquiring a mutex starts or stops the per-frame event. */
        if (offset == MUTEX(DISP_MUTEX0_EN) || offset == MUTEX(DISP_MUTEX1_EN) ||
            offset == MUTEX(DISP_MUTEX0) || offset == MUTEX(DISP_MUTEX1)) {
            mt6589_disp_mutex_kick(s);
        }

        /*
         * Register commit. The driver stages configuration in the display blocks and
         * then writes CFG_REG_COMMIT, whose bits name the modules to commit (bit 0 ROT,
         * 1 SCL, 2 OVL, 3 COLOR, 4 TDSHP, 5 WDMA0, 6 WDMA1, 7 RDMA0, 8 RDMA1, 9 BLS,
         * 10 GAMMA - see the timeout dump in ddp_drv.c). When those modules are updated
         * the mutex raises "update done" for the mutex that owns them, which is INTSTA
         * bit n, and that is what the driver's interrupt handler turns into
         * g_disp_mutex_reg_update.
         *
         * Without it nothing ever signals the commit, so disp_path_wait_reg_update()
         * runs its full 100 ms timeout on every pass and the boot never gets past the
         * display: measured as PID 1 asleep in that wait with the pipeline being
         * reconfigured about once a second.
         */
        if (offset == MUTEX(DISP_REG_COMMIT) && (uint32_t)value) {
            for (int n = 0; n < 2; n++) {
                uint32_t en_reg = (n == 0) ? MUTEX(DISP_MUTEX0_EN)
                                           : MUTEX(DISP_MUTEX1_EN);

                if (s->regs[en_reg >> 2] & 1u) {
                    s->regs[MUTEX(DISP_MUTEX_INTSTA) >> 2] |= 1u << n;
                }
            }
            /* The commit is consumed as the modules are updated. */
            s->regs[offset >> 2] = 0;
            mt6589_disp_mutex_update_irq(s);
        }
    }

    /* The OVL is the composer: any write may be the one that completes a modeset. */
    if (s->desc->base == MT6589_DISP_OVL_BASE) {
        mt6589_ovl_kick(s);
    }
    /*
     * OVL: the kernel's mtkfb_probe waits (prepare_to_wait/schedule_timeout, no timeout visible)
     * for the register-update event that the display interrupt handler signals, and the handler for
     * the overlay is the one that runs it - measured in the binary's callee list. So the overlay's
     * events are raised once per frame, the same way as the panel's.
     */
    if (s->desc->base == MT6589_DISP_OVL_BASE && offset == OVL(OVL_INTEN)) {
        mt6589_disp_panel_kick(s);
    }
    /* DSI/DPI: an enable written here starts their per-frame events. */
    if (s->desc->base == MT6589_DSI_BASE || s->desc->base == MT6589_DPI_BASE) {
        if (offset == 0x0008) {
            mt6589_disp_panel_kick(s);
        } else if (offset == 0x000c) {
            mt6589_disp_panel_irq(s);
        }
    }
}

static const MemoryRegionOps mt6589_disp_ops = {
    .read = mt6589_disp_read,
    .write = mt6589_disp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_disp_reset(DeviceState *dev)
{
    Mt6589DispBlockState *s = MT6589_DISP_BLOCK(dev);

    memset(s->regs, 0, sizeof(s->regs));
    for (size_t i = 0; i < s->desc->nregs; i++) {
        s->regs[s->desc->regs[i].offset >> 2] = s->desc->regs[i].reset;
    }
}

static void mt6589_disp_realize(DeviceState *dev, Error **errp)
{
    Mt6589DispBlockState *s = MT6589_DISP_BLOCK(dev);

    mt6589_disp_reset(dev);
    if (s->desc->has_irq) {
        sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
    }
    /*
     * Only the OVL gets a console, because it is the block whose output the panel
     * shows. The other instances are configuration registers with nothing to draw.
     */
    if (s->desc->base == MT6589_DISP_OVL_BASE) {
        s->con = qemu_graphic_console_create(dev, 0, &mt6589_ovl_gfx_ops, s);
        if (s->con) {
            qemu_console_resize(s->con, 1280, 800);
        }
        timer_init_ns(&s->frame_timer, QEMU_CLOCK_VIRTUAL,
                      mt6589_ovl_frame_tick, s);
    }
    if (s->desc->base == MT6589_DISP_MUTEX_BASE) {
        timer_init_ns(&s->vsync_timer, QEMU_CLOCK_VIRTUAL,
                      mt6589_disp_mutex_vsync, s);
    }
    if (s->desc->base == MT6589_DSI_BASE || s->desc->base == MT6589_DPI_BASE ||
        s->desc->base == MT6589_DISP_OVL_BASE) {
        timer_init_ns(&s->panel_timer, QEMU_CLOCK_VIRTUAL,
                      mt6589_disp_panel_tick, s);
    }
}

static void mt6589_disp_init(Object *obj)
{
    Mt6589DispBlockState *s = MT6589_DISP_BLOCK(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_disp_ops, s,
                          "mt6589.disp", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property mt6589_disp_props[] = {
};

static void mt6589_disp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mt6589_disp_realize;
    device_class_set_legacy_reset(dc, mt6589_disp_reset);
}

static const TypeInfo mt6589_disp_types[] = {
    {
        .name = TYPE_MT6589_DISP_BLOCK,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589DispBlockState),
        .instance_init = mt6589_disp_init,
        .class_init = mt6589_disp_class_init,
    },
};

DEFINE_TYPES(mt6589_disp_types)

/* Create and map every display block. Called from the SoC. */
void mt6589_disp_create(Mt6589State *soc, DeviceState *gic)
{
    Mt6589DispBlockState *ovl = NULL;
    Mt6589DispBlockState *rdma = NULL;
    Mt6589DispBlockState *cfg = NULL;

    for (size_t i = 0; i < ARRAY_SIZE(mt6589_disp_blocks); i++) {
        const Mt6589DispBlock *b = &mt6589_disp_blocks[i];
        Mt6589DispBlockState *s =
            MT6589_DISP_BLOCK(object_new(TYPE_MT6589_DISP_BLOCK));

        s->desc = b;
        object_property_add_child(OBJECT(soc), b->name, OBJECT(s));
        qdev_realize(DEVICE(s), sysbus_get_default(), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, b->base);
        if (b->has_irq && b->irq_spi && gic) {
            /*
             * MT6589_GIC_GPIO, because qdev_get_gpio_in() numbers the GIC's external
             * inputs by SPI (0-based) while irq_spi is the GIC line the device tree
             * names, i.e. SPI + 32. Connecting the line number directly put the display
             * mutex on SPI 192 (line 224) instead of SPI 160 (line 192), so the kernel's
             * registered handler never ran:
             *
             *   ISENABLER6 = 0x000391ff   bit 0 set -> line 192 enabled
             *   ISPENDR6   = 0x00000000   never pending
             *
             * and the guest sat in disp_path_wait_reg_update() waiting for a
             * register-update event that its own interrupt handler was supposed to
             * signal - measured as PID 1 asleep there with the display reconfiguring
             * itself about once a second, each pass spending the full 100 ms timeout.
             */
            sysbus_connect_irq(SYS_BUS_DEVICE(s), 0,
                               qdev_get_gpio_in(gic, MT6589_GIC_GPIO(b->irq_spi)));
        }
        if (b->base == MT6589_DISP_OVL_BASE) {
            ovl = s;
        } else if (b->base == MT6589_DISPSYS_CONFIG_BASE) {
            cfg = s;
        } else if (b->base == MT6589_DISP_RDMA_BASE) {
            rdma = s;
        }
    }

    /*
     * The composer needs the read DMA's registers and the routing table; see
     * mt6589_ovl_compose() and mt6589_disp_route_is_ovl().
     */
    if (ovl) {
        ovl->rdma = rdma;
        ovl->config = cfg;
        ovl->soc = soc;
    }
}
