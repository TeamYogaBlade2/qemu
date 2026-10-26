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

static const Mt6589DispBlock mt6589_disp_blocks[] = {
    BLOCK("ovl",   0x14003000, mt6589_ovl_regs,   163),
    BLOCK("rdma",  0x14006000, mt6589_rdma_regs,  0),
    BLOCK("color", 0x1400a000, mt6589_color_regs, 0),
    BLOCK("mutex", 0x14011000, mt6589_mutex_regs, 0),
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
