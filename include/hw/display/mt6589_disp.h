/*
 * MediaTek MT6589 display subsystem blocks
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_DISPLAY_MT6589_DISP_H
#define HW_DISPLAY_MT6589_DISP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_DISP_BLOCK "mt6589-disp-block"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589DispBlockState, MT6589_DISP_BLOCK)

struct Mt6589State;

/* Register behaviours that are not plain read/write storage. */
#define MREG_W1C    (1u << 0)   /* writing 1 clears the bit */
#define MREG_SELFCL (1u << 1)   /* writing 1 is a trigger; reads back 0 */
#define MREG_IDLE   (1u << 2)   /* reads as a constant status value */

typedef struct Mt6589DispReg {
    uint16_t offset;
    uint32_t reset;
    uint32_t flags;
} Mt6589DispReg;

typedef struct Mt6589DispBlock {
    const char *name;
    hwaddr base;
    const struct Mt6589DispReg *regs;
    size_t nregs;
    bool has_irq;
    int irq_spi;
} Mt6589DispBlock;

struct Mt6589DispBlockState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* Which block this instance is. Set before realize. */
    const Mt6589DispBlock *desc;

    /* 4 KiB of registers, which covers every documented block layout. */
    uint32_t regs[0x400];
};

/* Create and map OVL, RDMA, COLOR and MUTEX. */
void mt6589_disp_create(struct Mt6589State *soc, DeviceState *gic);

#endif /* HW_DISPLAY_MT6589_DISP_H */
