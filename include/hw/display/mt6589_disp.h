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
#define MREG_W0C    (1u << 3)   /* writing 0 clears the bit (MTK mutex ack) */

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
    /*
     * A block whose every register is plain storage - no self-clearing triggers, no
     * status-only reads. The DDP routing block is one: 48 registers that the driver
     * read-modify-writes, and a table that lists them would be a copy of the header for
     * no benefit. Setting this makes any offset in the 4 KiB window a 32-bit store.
     */
    bool generic_regs;
} Mt6589DispBlock;

struct Mt6589DispBlockState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    /* Which block this instance is. Set before realize. */
    const Mt6589DispBlock *desc;

    /* 4 KiB of registers, which covers every documented block layout. */
    uint32_t regs[0x400];

    /*
     * Scanout. Only the OVL instance - it is the composer, and its output is
     * what the panel shows - gets a console.
     */
    QemuConsole *con;
    QEMUTimer frame_timer;
    uint32_t frame_count;
    bool in_gfx_update;
    uint32_t last_sum;      /* checksum of the last frame written to disk */
    bool dumped_content;    /* the file on disk has a picture, not a blank */
    /*
     * The DISP mutex raises an event at every frame start, which is what the panel
     * pipe and its clients synchronise on. Modelling it as a periodic line is the
     * honest simplification: there is no panel to drive, so the frame period is
     * whatever the mode asks for, but the interrupt has to happen or every wait for
     * a first frame blocks for ever.
     */
    QEMUTimer vsync_timer;
    uint32_t vsync_count;
    /*
     * DSI and DPI raise their own events once per frame, separately from the mutex. The bit
     * assignments of their INTSTA are not documented here, so the model raises whatever the
     * guest enabled - see mt6589_disp_panel_tick().
     */
    QEMUTimer panel_timer;
    uint32_t panel_count;
    /*
     * The OVL is the composer, but the read DMA is a separate block instance and is what
     * the kernel actually drives. The composer needs to see it, so create() links the two.
     */
    struct Mt6589DispBlockState *rdma;
    /* The DDP routing block (0x1400_0000): decides which source reaches the panel. */
    struct Mt6589DispBlockState *config;
    /*
     * The SoC, for the M4U page-table base: the kernel hands the display M4U virtual
     * addresses, not physical ones, so the composer has to translate before it can read.
     */
    struct Mt6589State *soc;
};

/* Create and map OVL, RDMA, COLOR and MUTEX. */
void mt6589_disp_create(struct Mt6589State *soc, DeviceState *gic);

#endif /* HW_DISPLAY_MT6589_DISP_H */
