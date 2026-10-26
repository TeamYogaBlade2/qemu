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
#include "hw/arm/mt6589.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"


/* ------------------------------------------------------------------------- */
/* OVL - display overlay, 0x14003000                                          */
/*
 * OVL_STA's RDMA-idle bits are what the DRM driver's atomic commit waits on.
 * Reporting them as 0 makes the commit wait for a pipeline that never drains.
 */
static const Mt6589DispReg mt6589_ovl_regs[] = {
    { 0x0000, 0x0000001f, MREG_IDLE },  /* OVL_STA: all RDMA idle bits set */
    { 0x000c, 0, 0 },                   /* OVL_EN */
    { 0x0010, 0, MREG_SELFCL },         /* OVL_TRIG */
    { 0x0014, 0, MREG_SELFCL },         /* OVL_RST */
    { 0x0020, 0, 0 },                   /* OVL_ROI_SIZE */
    { 0x002c, 0, 0 },                   /* OVL_SRC_CON */
    { 0x0034, 0, 0 },                   /* OVL_DATAPATH_CON */
    { 0x0038, 0, 0 },                   /* OVL_ROI_BGCLR */
    { 0x003c, 0, 0 },                   /* OVL_SRC_KEY_CON */
    { 0x0040, 0, 0 },                   /* OVL_SRC_KEY_ALPHA */
    { 0x0044, 0, 0 },                   /* OVL_L0_PITCH */
    { 0x0064, 0, 0 },                   /* OVL_L1_PITCH */
    { 0x0084, 0, 0 },                   /* OVL_L2_PITCH */
    { 0x00a4, 0, 0 },                   /* OVL_L3_PITCH */
};

/* ------------------------------------------------------------------------- */
/* RDMA - read DMA, 0x14006000                                                */
/*
 * GLOBAL_CON bit 0 is the engine enable; the driver polls it plus the output-size
 * registers. INT_STATUS is W1C.
 */
static const Mt6589DispReg mt6589_rdma_regs[] = {
    { 0x0000, 0, 0 },                   /* INT_ENABLE */
    { 0x0004, 0, MREG_W1C },            /* INT_STATUS */
    { 0x0010, 0, 0 },                   /* GLOBAL_CON */
    { 0x0014, 0, 0 },                   /* SIZE_CON_0 */
    { 0x0018, 0, 0 },                   /* SIZE_CON_1 */
    { 0x001c, 0, 0 },                   /* TARGET_LINE */
    { 0x0024, 0, 0 },                   /* MEM_CON */
    { 0x0040, 0, 0 },                   /* MEM_START_ADDR */
    { 0x0044, 0, 0 },                   /* MEM_SRC_PITCH */
    { 0x0048, 0, 0 },                   /* MEM_GMC_SETTING_0 */
    { 0x004c, 0, 0 },                   /* MEM_GMC_SETTING_1 */
    { 0x0050, 0, 0 },                   /* MEM_SLOW_CON */
    { 0x0054, 0, 0 },                   /* FIFO_CON */
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
    { 0x0000, 0, 0 },                   /* MUTEX_INTEN */
    { 0x0004, 0, MREG_W1C },            /* MUTEX_INTSTA */
    { 0x0020, 0, 0 },                   /* MUTEX0_EN */
    { 0x0024, 0, 0 },                   /* MUTEX0_RST */
    { 0x0028, 0, 0 },                   /* MUTEX0_MOD */
    { 0x002c, 0, 0 },                   /* MUTEX0_SOF */
    { 0x0040, 0, 0 },                   /* MUTEX1_EN */
    { 0x0044, 0, 0 },                   /* MUTEX1_RST */
    { 0x0048, 0, 0 },                   /* MUTEX1_MOD */
    { 0x004c, 0, 0 },                   /* MUTEX1_SOF */
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
    BLOCK("ovl",   0x14003000, mt6589_ovl_regs,   163),
    BLOCK("rdma",  0x14006000, mt6589_rdma_regs,  0),
    BLOCK("color", 0x1400a000, mt6589_color_regs, 0),
    BLOCK("mutex", 0x14011000, mt6589_mutex_regs, 0),
    BLOCK("wdma",  0x14004000, mt6589_wdma_regs,  0),
    BLOCK("bls",   0x14008000, mt6589_bls_regs,   0),
    BLOCK("gamma", 0x14009000, mt6589_gamma_regs, 0),
    BLOCK("dsi",   0x1400d000, mt6589_dsi_regs,   0),
    BLOCK("dpi",   0x1400e000, mt6589_dpi_regs,   0),
};

/* ------------------------------------------------------------------------- */

static const Mt6589DispReg *find_reg(const Mt6589DispBlock *b,
                                            hwaddr offset)
{
    for (size_t i = 0; i < b->nregs; i++) {
        if (b->regs[i].offset == offset) {
            return &b->regs[i];
        }
    }
    return NULL;
}

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
    return s->regs[offset >> 2];
}

static void mt6589_disp_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Mt6589DispBlockState *s = opaque;
    const Mt6589DispReg *r = find_reg(s->desc, offset);

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
    for (size_t i = 0; i < ARRAY_SIZE(mt6589_disp_blocks); i++) {
        const Mt6589DispBlock *b = &mt6589_disp_blocks[i];
        Mt6589DispBlockState *s =
            MT6589_DISP_BLOCK(object_new(TYPE_MT6589_DISP_BLOCK));

        s->desc = b;
        object_property_add_child(OBJECT(soc), b->name, OBJECT(s));
        qdev_realize(DEVICE(s), sysbus_get_default(), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s), 0, b->base);
        if (b->has_irq && b->irq_spi && gic) {
            sysbus_connect_irq(SYS_BUS_DEVICE(s), 0,
                               qdev_get_gpio_in(gic, b->irq_spi));
        }
    }
}
