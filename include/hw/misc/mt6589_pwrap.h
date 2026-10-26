/*
 * MediaTek MT6589 PMIC_WRAP (pwrap)
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_MISC_MT6589_PWRAP_H
#define HW_MISC_MT6589_PWRAP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_PWRAP "mt6589-pwrap"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589PwrapState, MT6589_PWRAP)

struct Mt6589PwrapState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* Wrapper-side FSM state, in the encoding the guest polls (0 idle, 6 valid). */
    uint32_t fsm;
    uint32_t rdata;
    uint32_t cipher_rdy;

    /*
     * PMIC-side register file, indexed by WACS number (PMIC address >> 1). 32K
     * entries covers the whole MT6320 address space at 16 bits each.
     */
    uint16_t pmic[0x8000];
};

#endif /* HW_MISC_MT6589_PWRAP_H */
