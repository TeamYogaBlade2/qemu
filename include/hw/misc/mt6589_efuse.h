/*
 * MediaTek MT6589 eFuse (EFUSEC)
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_MISC_MT6589_EFUSE_H
#define HW_MISC_MT6589_EFUSE_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_EFUSE "mt6589-efuse"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589EfuseState, MT6589_EFUSE)

struct Mt6589EfuseState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* Bit 2 of the control word: a read was started. */
    uint32_t ctrl;
};

#endif /* HW_MISC_MT6589_EFUSE_H */
