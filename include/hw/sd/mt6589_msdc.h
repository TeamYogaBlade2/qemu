/*
 * MediaTek MT6589 MSDC - SD/MMC controller
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_SD_MT6589_MSDC_H
#define HW_SD_MT6589_MSDC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_MSDC "mt6589-msdc"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589MsdcState, MT6589_MSDC)

struct Mt6589MsdcState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t cfg;
    uint32_t iocon;
    uint32_t ps;
    uint32_t int_status;
    uint32_t int_enable;

    uint32_t sdc_cfg;
    uint32_t sdc_cmd;
    uint32_t sdc_arg;
    uint32_t sdc_sts;
    uint32_t sdc_resp[4];
    uint32_t sdc_blk_num;
    uint32_t sdc_blk_sz;

    bool card_idle;
    uint32_t emmc_cfg0;
    uint32_t emmc_cfg1;
    uint32_t emmc_sts;
    /*
     * Register file for the pad/delay tuning registers. LK writes these and then
     * reads them back in a settle loop, so they have to hold their value - a
     * constant 0 makes the loop run for ever.
     */
    uint32_t pad_ctl0, pad_ctl1, pad_ctl2, pad_tune;
    uint32_t dma_sa;      /* GPD descriptor address */
    uint32_t dma_ctrl;
    uint32_t dma_cfg;
    uint32_t dat_rddly0, dat_rddly1;

    /* Bytes the boot-area fetch delivers; 0 means BOOT_AREA_DEFAULT. */
    uint32_t boot_area_len;

    /*
     * Current data phase: a pointer into the card image plus a position, rather than
     * a copy. The boot partition can be 0x40000 bytes and is fetched in one go.
     */
    const uint8_t *data;
    uint32_t data_len;    /* end of the current chunk */
    uint32_t data_total;  /* end of the whole staged span */
    uint32_t data_pos;

    /* Backing card image, if one was supplied. */
    uint8_t *image;
    size_t image_size;
    /* For logging: which slot this is, so a command trace can be attributed. */
    char name[8];
};

#endif /* HW_SD_MT6589_MSDC_H */
