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
    /* For logging: which slot this is, so a command trace can be attributed. */
    char name[8];
};

#endif /* HW_SD_MT6589_MSDC_H */
