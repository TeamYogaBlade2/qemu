/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 NAND flash interface (NFI) and ECC engine (NFIECC)
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_MISC_MT6589_NFI_H
#define HW_MISC_MT6589_NFI_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_NFI "mt6589-nfi"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589NfiState, MT6589_NFI)

struct Mt6589NfiState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* NFI */
    uint32_t cnfg;
    uint32_t pagefmt;
    uint32_t con;
    uint32_t acccon;
    uint32_t intr_en;
    uint32_t intr;
    uint32_t cmd;
    uint32_t addrnob;
    uint32_t coladdr;
    uint32_t rowaddr;
    uint32_t sta;

    /* NFIECC */
    uint32_t enccon;
    uint32_t enccnfg;
    uint32_t deccon;
    uint32_t deccnfg;
    /* Generic storage for the ECC registers this model does not interpret. */
    uint32_t ecc_regs[0x80];
};

#endif /* HW_MISC_MT6589_NFI_H */
