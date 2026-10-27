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

#define RDMA(name)  (MT6589_DISP_RDMA_##name - MT6589_DISP_RDMA_BASE)
#define MUTEX(name) (MT6589_DISP_MUTEX_##name - MT6589_DISP_MUTEX_BASE)


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
    { 0x0400, 0, 0 },                   /* DISP_COLOR_CFG_MAIN */
    { 0x0404, 0, 0 },                   /* DISP_COLOR_START */
    { 0x0408, 0, MREG_SELFCL },         /* DISP_COLOR_INTEN */
    { 0x040c, 0, MREG_W1C },            /* DISP_COLOR_INTSTA */
    { 0x0f64, 0, 0 },                   /* R2Y coefficient 0 */
    { 0x0f68, 0, 0 },
    { 0x0f6c, 0, 0 },
    { 0x0f70, 0, 0 },
    { 0x0f74, 0, 0 },
    { 0x0f78, 0, 0 },
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
    { MUTEX(DISP_MUTEX_INTSTA), 0, MREG_W1C },
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
    { 0x0000, 0x00000000, 0 },  /* DSI_START */
    { 0x0004, 0x00000000, 0 },  /* DSI_STATUS */
    { 0x0008, 0x00000000, 0 },  /* DSI_INTEN */
    { 0x000c, 0x00000000, 0 },  /* DSI_INTSTA */
    { 0x0010, 0x00000000, 0 },  /* DSI_COM_CON */
    { 0x0014, 0x00000000, 0 },  /* DSI_MODE_CON */
    { 0x0018, 0x00000000, 0 },  /* DSI_TXRX_CON */
    { 0x001c, 0x00000000, 0 },  /* DSI_PSCON */
    { 0x0020, 0x00000000, 0 },  /* DSI_VSA_NL */
    { 0x0024, 0x00000000, 0 },  /* DSI_VBP_NL */
    { 0x0028, 0x00000000, 0 },  /* DSI_VFP_NL */
    { 0x002c, 0x00000000, 0 },  /* DSI_VACT_NL */
    { 0x0050, 0x00000000, 0 },  /* DSI_HSA_WC */
    { 0x0054, 0x00000000, 0 },  /* DSI_HBP_WC */
    { 0x0058, 0x00000000, 0 },  /* DSI_HFP_WC */
    { 0x005c, 0x00000000, 0 },  /* DSI_BLLP_WC - the driver writes it; without the
                                 * entry the value was dropped and read back as 0 */
    { 0x0060, 0x00000000, 0 },  /* DSI_CMDQ_CON */
    { 0x0074, 0x00000000, 0 },  /* DSI_RX_DATA03 */
    { 0x0078, 0x00000000, 0 },  /* DSI_RX_DATA47 */
    { 0x007c, 0x00000000, 0 },  /* DSI_RX_DATA8B */
    { 0x0080, 0x00000000, 0 },  /* DSI_RX_DATAC */
    { 0x0084, 0x00000000, 0 },  /* DSI_RX_RACK */
    { 0x0088, 0x00000000, 0 },  /* DSI_RX_TRIG_STA */
    { 0x0090, 0x00000000, 0 },  /* DSI_MEM_CONTI */
    { 0x0094, 0x00000000, 0 },  /* DSI_FRM_BC */
    { 0x0104, 0x00000000, 0 },  /* DSI_PHY_LCCON */
    { 0x0108, 0x00000000, 0 },  /* DSI_PHY_LD0CON */
    { 0x0110, 0x00005050, 0 },  /* DSI_PHY_TIMCON0 */
    { 0x0114, 0x00004c68, 0 },  /* DSI_PHY_TIMCON1 */
    { 0x0118, 0x00000000, 0 },  /* DSI_PHY_TIMCON2 */
    { 0x011c, 0x00005070, 0 },  /* DSI_PHY_TIMCON3 */
    { 0x0130, 0x00000000, 0 },  /* DSI_VM_CMD_ */
    { 0x0144, 0x0000ffff, 0 },  /* DSI_CKSM_OUT */
    { 0x0148, 0x00001008, 0 },  /* DSI_STATE_DBG0 */
    { 0x014c, 0x00004000, 0 },  /* DSI_STATE_DBG1 */
    { 0x0150, 0x00000200, 0 },  /* DSI_STATE_DBG2 */
    { 0x0154, 0x00000210, 0 },  /* DSI_STATE_DBG3 */
    { 0x0158, 0x00000210, 0 },  /* DSI_STATE_DBG4 */
    { 0x0160, 0x00002000, 0 },  /* DSI_STATE_DBG6 */
    { 0x0164, 0x00000000, 0 },  /* DSI_STATE_DBG7 */
    { 0x0168, 0x00000000, 0 },  /* DSI_STATE_DBG8 */
    { 0x016c, 0x00000000, 0 },  /* DSI_STATE_DBG9 */
    { 0x0170, 0x00000000, 0 },  /* DSI_DEBUG_SEL */
    { 0x0174, 0x00000000, 0 },  /* DSI_RESERVED */
    { 0x0178, 0x00000000, 0 },  /* DSI_BIST_PATTERN */
    { 0x017c, 0x00000000, 0 },  /* DSI_BIST_CON */
    { 0x0180, 0x00000000, 0 },  /* DSI_CMDQ0 */
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
    { 0x0000, 0x00000000, 0 },  /* DPI_EN */
    { 0x0004, 0x00000000, 0 },  /* DPI_RST */
    { 0x0008, 0x00000000, 0 },  /* DPI_INTEN */
    { 0x000c, 0x00000000, 0 },  /* DPI_INTSTA */
    { 0x0010, 0x00000000, 0 },  /* DPI_CON */
    { 0x0014, 0x00000000, 0 },  /* DPI_CLKCON */
    { 0x0018, 0x00000000, 0 },  /* DPI_SIZE */
    { 0x0044, 0x00000000, 0 },  /* DPI_ESAV_VTIM_L */
    { 0x0048, 0x00000000, 0 },  /* DPI_ESAV_VTIM_R */
    { 0x004c, 0x00000000, 0 },  /* DPI_ESAV_FTIM */
    { 0x0050, 0x00000400, 0 },  /* DPI_BG_HCNTL */
    { 0x0054, 0x00000000, 0 },  /* DPI_BG_VCNTL */
    { 0x0058, 0x00000000, 0 },  /* DPI_BG_COLOR */
    { 0x005c, 0x00000000, 0 },  /* DPI_TGEN_POL */
    { 0x0060, 0x00000000, 0 },  /* DPI_STATUS */
    { 0x0064, 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET0 */
    { 0x0068, 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET1 */
    { 0x006c, 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET2 */
    { 0x0070, 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET3 */
    { 0x0074, 0x00000000, 0 },  /* DPI_MATRIX_COEFF_SET4 */
    { 0x0078, 0x00000000, 0 },  /* DPI_MATRIX_PREADD_SET0 */
    { 0x007c, 0x00000000, 0 },  /* DPI_MATRIX_PREADD_SET1 */
    { 0x0080, 0x00000000, 0 },  /* DPI_MATRIX_POSTADD_SET0 */
    { 0x0084, 0x00000000, 0 },  /* DPI_MATRIX_POSTADD_SET1 */
    { 0x0088, 0x00000000, 0 },  /* DPI_CLPF_SET */
    { 0x008c, 0x00000000, 0 },  /* DPI_Y_LIMIT */
    { 0x0090, 0x00000000, 0 },  /* DPI_C_LIMIT */
    { 0x0094, 0x00000000, 0 },  /* DPI_YUV422_S */
    { 0x0098, 0x00000000, 0 },  /* DPI_YCMUX_SE */
    { 0x009c, 0x00000000, 0 },  /* DPI_EMBSYNC_ */
    { 0x00a8, 0x00000000, 0 },  /* DPI_OUTPUT_S */
    { 0x00ac, 0x00000000, 0 },  /* DPI_TMODE */
    { 0x00b0, 0x00000000, 0 },  /* DPI_CHKSUM */
    { 0x00b4, 0x00000000, 0 },  /* DPI_PATTERN */
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
    { 0x0000, 0x00000000, 0 },  /* WDMA_INTEN */
    { 0x000c, 0x00000000, 0 },  /* WDMA_RST */
    { 0x0014, 0x00000000, 0 },  /* WDMA_CFG */
    { 0x0018, 0x00000000, 0 },  /* WDMA_SRC_SIZE */
    { 0x001c, 0x00000000, 0 },  /* WDMA_CLIP_SIZE */
    { 0x002c, 0x000000ff, 0 },  /* WDMA_ALPHA */
    { 0x00a4, 0x00000000, 0 },  /* WDMA_EXEC_DBG */
    { 0x00a8, 0x00000000, 0 },  /* WDMA_CLIP_DBG */
    { 0x0004, 0x00000000, 0 },  /* WDMA_INTSTA */
    { 0x0008, 0x00000000, 0 },  /* WDMA_EN */
    { 0x0010, 0x0000c000, 0 },  /* WDMA_SMI_CON */
    { 0x0020, 0x00000000, 0 },  /* WDMA_CLIP_COORD */
    { 0x0024, 0x00000000, 0 },  /* WDMA_DST_ADDR */
    { 0x0028, 0x00000000, 0 },  /* WDMA_DST_W_IN_BYTE */
    { 0x0034, 0x00008000, 0 },  /* WDMA_STA */
    { 0x0038, 0x000001fe, 0 },  /* WDMA_BUF_CON1 */
    { 0x003c, 0x00000808, 0 },  /* WDMA_BUF_CON2 */
    { 0x0040, 0x00000000, 0 },  /* WDMA_C00 */
    { 0x0044, 0x00000000, 0 },  /* WDMA_C02 */
    { 0x0048, 0x00000000, 0 },  /* WDMA_C10 */
    { 0x004c, 0x00000000, 0 },  /* WDMA_C12 */
    { 0x0050, 0x00000000, 0 },  /* WDMA_C20 */
    { 0x0054, 0x00000000, 0 },  /* WDMA_C22 */
    { 0x0058, 0x00000000, 0 },  /* WDMA_PRE_ADD0 */
    { 0x005c, 0x00000000, 0 },  /* WDMA_PRE_ADD2 */
    { 0x0060, 0x00000000, 0 },  /* WDMA_POST_ADD0 */
    { 0x0064, 0x00000000, 0 },  /* WDMA_POST_ADD2 */
    { 0x0070, 0x00000000, 0 },  /* WDMA_DST_U_ADDR */
    { 0x0074, 0x00000000, 0 },  /* WDMA_DST_V_ADDR */
    { 0x0078, 0x00000000, 0 },  /* WDMA_DST_UV_PITCH */
    { 0x00a0, 0x00008000, 0 },  /* WDMA_FLOW_CTRL_DBG */
    { 0x0100, 0x00000000, 0 },  /* WDMA_DUMMY */
    { 0x0e00, 0x00000000, 0 },  /* WDMA_DITHER_0 */
    { 0x0e14, 0x00000000, 0 },  /* WDMA_DITHER_5 */
    { 0x0e18, 0x00000000, 0 },  /* WDMA_DITHER_6 */
    { 0x0e1c, 0x00000000, 0 },  /* WDMA_DITHER_7 */
    { 0x0e20, 0x00000000, 0 },  /* WDMA_DITHER_8 */
    { 0x0e24, 0x00000000, 0 },  /* WDMA_DITHER_9 */
    { 0x0e28, 0x00000000, 0 },  /* WDMA_DITHER_10 */
    { 0x0e30, 0x00000000, 0 },  /* WDMA_DITHER_12 */
    { 0x0e34, 0x00000000, 0 },  /* WDMA_DITHER_13 */
    { 0x0e38, 0x00000000, 0 },  /* WDMA_DITHER_14 */
    { 0x0e3c, 0x00000000, 0 },  /* WDMA_DITHER_15 */
    { 0x0e40, 0x00000000, 0 },  /* WDMA_DITHER_16 */
    { 0x0e44, 0x00000000, 0 },  /* WDMA_DITHER_17 */
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
    { 0x0018, 0x00000000, 0 },  /* BLS_SRC_SIZE */
    { 0x001c, 0x00000000, 0 },  /* BLS_HIS_PROT */
    { 0x0020, 0x00000000, 0 },  /* BLS_GAIN */
    { 0x006c, 0x00000000, 0 },  /* BLS_SC_BIN_T */
    { 0x0000, 0x00000000, 0 },  /* BLS_EN */
    { 0x0004, 0x00000000, 0 },  /* BLS_RST */
    { 0x0008, 0x00000000, 0 },  /* BLS_BLS_SETTING */
    { 0x000c, 0x00000000, 0 },  /* BLS_HIS_SETTING */
    { 0x0010, 0x00000000, 0 },  /* BLS_INTEN */
    { 0x0014, 0x00000000, 0 },  /* BLS_INTSTA */
    { 0x0024, 0x00000000, 0 },  /* BLS_DISTORT_POINT */
    { 0x0028, 0x00000000, 0 },  /* BLS_GAIN_SETTING */
    { 0x0030, 0x00000000, 0 },  /* BLS_GAMMA_SETTING */
    { 0x0034, 0x00000000, 0 },  /* BLS_GAMMA_B */
    { 0x0038, 0x00000000, 0 },  /* BLS_LUT_UPDATE */
    { 0x003c, 0x00000000, 0 },  /* BLS_PWM_LUT_SEL */
    { 0x0040, 0x000000ff, 0 },  /* BLS_MAXCLR_LIMIT */
    { 0x0044, 0x00000000, 0 },  /* BLS_PRE_DIST_THD */
    { 0x0048, 0x00000000, 0 },  /* BLS_DIST_THD_NORM */
    { 0x004c, 0x00000000, 0 },  /* BLS_DIST_THD_DARK */
    { 0x0050, 0x00000000, 0 },  /* BLS_DIST_THD_BRIGHT */
    { 0x0054, 0x00000000, 0 },  /* BLS_DIST_THD_TEXT */
    { 0x0058, 0x00000000, 0 },  /* BLS_DS_SETTING */
    { 0x005c, 0x00000000, 0 },  /* BLS_BS_SETTING */
    { 0x0060, 0x00000000, 0 },  /* BLS_TS_SETTING0 */
    { 0x0064, 0x00000000, 0 },  /* BLS_TS_SETTING1 */
    { 0x0070, 0x00000000, 0 },  /* BLS_FAST_IIR_ */
    { 0x0074, 0x00000000, 0 },  /* BLS_FAST_IIR_ */
    { 0x0078, 0x00000000, 0 },  /* BLS_SLOW_IIR_XCOEFF */
    { 0x007c, 0x00000000, 0 },  /* BLS_SLOW_IIR_YCOEFF */
    { 0x0080, 0x00000000, 0 },  /* BLS_PWM_DUTY */
    { 0x0084, 0x00000001, 0 },  /* BLS_PWM_DUTY_GAIN */
    { 0x0088, 0x00000000, 0 },  /* BLS_PATTERN */
    { 0x0090, 0x00000000, 0 },  /* BLS_PWM_CON */
    { 0x0094, 0x00008000, 0 },  /* PWM_H_DURATION */
    { 0x0098, 0x00008000, 0 },  /* PWM_L_DURATION */
    { 0x009c, 0x00000000, 0 },  /* PWM_G_DURATION */
    { 0x00a0, 0x00000000, 0 },  /* PWM_SEND_DATA0 */
    { 0x00a4, 0x00000000, 0 },  /* PWM_SEND_DATA1 */
    { 0x00a8, 0x00000000, 0 },  /* PWM_WAVE_NUM */
    { 0x00ac, 0x00000000, 0 },  /* PWM_DATA_WIDTH */
    { 0x00b0, 0x00000000, 0 },  /* PWM_THRESH */
    { 0x00b4, 0x00000000, 0 },  /* PWM_SEND_W */
    { 0x0180, 0x00000000, 0 },  /* BLS_DARK_BIN */
    { 0x0184, 0x00000000, 0 },  /* BLS_BRIGHT_BIN */
    { 0x0200, 0x000001fe, 0 },  /* BLS_PWM_DUTY_RD */
    { 0x0204, 0x0000ffff, 0 },  /* BLS_MAXCLR_RD */
    { 0x0208, 0x00000080, 0 },  /* BLS_GAIN_RD */
    { 0x020c, 0x00000000, 0 },  /* BLS_SC_RD */
    { 0x0e00, 0x00000000, 0 },  /* BLS_DITHER_0 */
    { 0x0e14, 0x00000000, 0 },  /* BLS_DITHER_5 */
    { 0x0e18, 0x00000000, 0 },  /* BLS_DITHER_6 */
    { 0x0e1c, 0x00000000, 0 },  /* BLS_DITHER_7 */
    { 0x0e20, 0x00000000, 0 },  /* BLS_DITHER_8 */
    { 0x0e24, 0x00000000, 0 },  /* BLS_DITHER_9 */
    { 0x0e28, 0x00000000, 0 },  /* BLS_DITHER_10 */
    { 0x0e2c, 0x00000000, 0 },  /* BLS_DITHER_11 */
    { 0x0e30, 0x00000000, 0 },  /* BLS_DITHER_12 */
    { 0x0e34, 0x00000000, 0 },  /* BLS_DITHER_13 */
    { 0x0e38, 0x00000000, 0 },  /* BLS_DITHER_14 */
    { 0x0e3c, 0x00000000, 0 },  /* BLS_DITHER_15 */
    { 0x0e40, 0x00000000, 0 },  /* BLS_DITHER_16 */
    { 0x0e44, 0x00000000, 0 },  /* BLS_DITHER_17 */
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
    { 0x0000, 0x00000000, 0 },  /* DISP_GAMMA_EN */
    { 0x0004, 0x00000000, 0 },  /* DISP_GAMMA_ */
    { 0x0008, 0x00000000, 0 },  /* DISP_GAMMA_LUT_CLK_EN */
    { 0x000c, 0x00000000, 0 },  /* DISP_GAMMA_I */
    { 0x0010, 0x00000000, 0 },  /* DISP_GAMMA_I */
    { 0x0e00, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_0 */
    { 0x0e10, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_4 */
    { 0x0e14, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_5 */
    { 0x0e18, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_6 */
    { 0x0e1c, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_7 */
    { 0x0e20, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_8 */
    { 0x0e24, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_9 */
    { 0x0e28, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_10 */
    { 0x0e2c, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_11 */
    { 0x0e30, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_12 */
    { 0x0e34, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_13 */
    { 0x0e38, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_14 */
    { 0x0e3c, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_15 */
    { 0x0e40, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_16 */
    { 0x0e44, 0x00000000, 0 },  /* DISP_GAMMA_DITHER_17 */
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
    BLOCK("ovl",   0x14003000, mt6589_ovl_regs,   163),
    BLOCK("rdma",  0x14006000, mt6589_rdma_regs,  0),
    BLOCK("color", 0x1400a000, mt6589_color_regs, 0),
    BLOCK("mutex", 0x14011000, mt6589_mutex_regs, 192),
    BLOCK("wdma",  0x14004000, mt6589_wdma_regs,  0),
    BLOCK("bls",   0x14008000, mt6589_bls_regs,   0),
    BLOCK("gamma", 0x14009000, mt6589_gamma_regs, 0),
    BLOCK("dsi",   0x1400d000, mt6589_dsi_regs,   0),
    BLOCK("dpi",   0x1400e000, mt6589_dpi_regs,   0),
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
    if (sum == s->last_sum) {
        return;
    }
    s->last_sum = sum;
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

        if (address_space_read(&address_space_memory, addr + (hwaddr)y * pitch,
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
        hwaddr addr = s->regs[OVL_L_ADDR(n) >> 2];
        uint32_t pitch = s->regs[OVL_L_PITCH(n) >> 2];
        uint32_t size = s->regs[OVL_L_SIZE(n) >> 2];
        int lw, lh, bpp;

        if (!addr || !pitch) {
            continue;
        }
        lw = size & 0xffff;
        lh = size >> 16;
        if (!lw || !lh) {
            lw = w;
            lh = h;
        }
        bpp = pitch / lw;
        if (bpp < 1 || bpp > 4) {
            continue;
        }
        lw = MIN(lw, w);
        lh = MIN(lh, h);
        lw = MIN(lw, (int)(sizeof(line) / bpp));

        if (mt6589_disp_log_enabled() && s->frame_count <= 1) {
            fprintf(stderr, "DISP scanout layer %d: addr=%08" PRIx64
                    " pitch=%u size=%dx%d bpp=%d\n", n, (uint64_t)addr, pitch,
                    size & 0xffff, size >> 16, bpp);
        }

        for (int y = 0; y < lh; y++) {
            uint32_t *d = dst + (size_t)y * (stride / 4);

            if (address_space_read(&address_space_memory,
                                   addr + (hwaddr)y * pitch,
                                   MEMTXATTRS_UNSPECIFIED, line,
                                   (hwaddr)lw * bpp) != MEMTX_OK) {
                continue;
            }
            for (int x = 0; x < lw; x++) {
                d[x] = mt6589_disp_cvt_pixel(line + x * bpp, bpp);
            }
        }
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
        fprintf(stderr, "DISP frame %u: %dx%d first pixel %08" PRIx32
                " layer-addr %08" PRIx32 "\n", s->frame_count, w, h, dst[0],
                (uint32_t)s->regs[OVL_L_ADDR(0) >> 2]);
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
    if ((s->desc->base == 0x14006000 || s->desc->base == 0x14007000) &&
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
static void mt6589_disp_mutex_update_irq(Mt6589DispBlockState *s)
{
    /* MUTEX_INTEN at 0x0000, MUTEX_INTSTA at 0x0004. */
    uint32_t en = s->regs[MUTEX(DISP_MUTEX_INTEN) >> 2];
    uint32_t sta = s->regs[MUTEX(DISP_MUTEX_INTSTA) >> 2];

    if (s->desc->has_irq) {
        qemu_set_irq(s->irq, (en & sta) != 0);
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
    bool enabled = (s->regs[MUTEX(DISP_MUTEX0_EN) >> 2] & 1u) ||
                    (s->regs[MUTEX(DISP_MUTEX1_EN) >> 2] & 1u);

    if (!enabled) {
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
    bool enabled = (s->regs[MUTEX(DISP_MUTEX0_EN) >> 2] & 1u) ||
                    (s->regs[MUTEX(DISP_MUTEX1_EN) >> 2] & 1u);
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
    if (r->flags & MREG_W1C) {
        s->regs[offset >> 2] &= ~((uint32_t)value);
        if (s->desc->base == 0x14011000) {   /* MUTEX: INTSTA just cleared */
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
    if (s->desc->base == 0x14011000) {
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
        /* Enabling or disabling a mutex starts or stops the per-frame event. */
        if (offset == MUTEX(DISP_MUTEX0_EN) || offset == MUTEX(DISP_MUTEX1_EN)) {
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
    if (s->desc->base == 0x14003000) {
        mt6589_ovl_kick(s);
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
    if (s->desc->base == 0x14003000) {
        s->con = qemu_graphic_console_create(dev, 0, &mt6589_ovl_gfx_ops, s);
        if (s->con) {
            qemu_console_resize(s->con, 1280, 800);
        }
        timer_init_ns(&s->frame_timer, QEMU_CLOCK_VIRTUAL,
                      mt6589_ovl_frame_tick, s);
    }
    if (s->desc->base == 0x14011000) {
        timer_init_ns(&s->vsync_timer, QEMU_CLOCK_VIRTUAL,
                      mt6589_disp_mutex_vsync, s);
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
        if (b->base == 0x14003000) {
            ovl = s;
        } else if (b->base == 0x14000000) {
            cfg = s;
        } else if (b->base == 0x14006000) {
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
    }
}
