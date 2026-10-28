/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 TOPRGU - top-level reset generator and watchdog
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_MISC_MT6589_TOPRGU_H
#define HW_MISC_MT6589_TOPRGU_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_TOPRGU "mt6589-toprgu"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589ToprguState, MT6589_TOPRGU)

struct Mt6589ToprguState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t mode;
    uint32_t length;
    uint32_t restart;
    uint32_t sta;        /* boot reason - all zero means a normal boot */
    uint32_t interval;
    uint32_t swrst;
    uint32_t swsysrst;
    uint32_t nonrst;
    uint32_t req_mode;
    uint32_t req_irq_en;
    uint32_t dramc_ctl;
};

#endif /* HW_MISC_MT6589_TOPRGU_H */
