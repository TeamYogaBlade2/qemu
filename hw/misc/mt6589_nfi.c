/*
 * MediaTek MT6589 NAND flash interface (NFI) and ECC engine (NFIECC)
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * The board this models has no NAND fitted. The boot ROM still probes it as one
 * of the candidate preloader sources, so the blocks have to exist and answer
 * sensibly, or the ROM hangs in its probe rather than moving on to the source
 * that is actually present.
 *
 * Register offsets are from the datasheet chapters NFI and NFIECC (both under
 * the peripheral window, 0x1100_1000 and 0x1100_2000). The ECC chapter gives its
 * addresses through the IO alias (C1002000) - canonicalised by the extractor; see
 * docs/qemu/registers.md.
 *
 * What the ROM does, from its own code at 0xf98, and what each part needs:
 *
 *     ldr  r3, [pc, #436]     @ r3 = 0x11002000  (NFIECC)
 *     ldr  r1, [r3, #12]      @ NFIECC_ENCIDLE
 *     lsls r1, r1, #31        @ test bit 0
 *     beq  <poll>             @ wait until the encoder is idle
 *     str  r4, [r3, #4]       @ NFIECC_ENCCNFG
 *
 * So ENCIDLE bit 0 must read 1. It is a status bit ("encoder available"), and a
 * model that leaves it 0 stalls the ROM for ever at its first NAND probe.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/arm/mt6589.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"

#include "hw/arm/mt6589-regs/mt6589-nfi-regs.h"

/*
 * NFI, base 0x1100_1000.
 *
 * The datasheet puts this block at 0xC100_1000 and the generated header repeats
 * that. It is wrong: the preloader's mt6589.h has NFI_BASE = IO_PHYS +
 * 0x01001000 = 0x1100_1000, and that preloader runs on this hardware. The
 * document printed the high nibble as 0xC where 0x1 belongs (0xC1001000 &
 * 0x0FFFFFFF == 0x01001000). So the *offsets* come from the generated header and
 * the base stays here, with a build-time check that they still agree.
 */
#define NFI_OFF(x)    ((x) - 0xc1001000u)
#define NFI_CNFG      NFI_OFF(MT6589_NFI_NFI_CNFG)
#define NFI_PAGEFMT   NFI_OFF(MT6589_NFI_NFI_PAGEFMT)
#define NFI_CON       NFI_OFF(MT6589_NFI_NFI_CON)
#define NFI_ACCCON    NFI_OFF(MT6589_NFI_NFI_ACCCON)
#define NFI_INTR_EN   NFI_OFF(MT6589_NFI_NFI_INTR_EN)
#define NFI_INTR      NFI_OFF(MT6589_NFI_NFI_INTR)
#define NFI_CMD       NFI_OFF(MT6589_NFI_NFI_CMD)
#define NFI_ADDRNOB   NFI_OFF(MT6589_NFI_NFI_ADDRNOB)
#define NFI_COLADDR   NFI_OFF(MT6589_NFI_NFI_COLADDR)
#define NFI_ROWADDR   NFI_OFF(MT6589_NFI_NFI_ROWADDR)
#define NFI_STA       NFI_OFF(MT6589_NFI_NFI_STA)
#define NFI_FIFOSTA   NFI_OFF(MT6589_NFI_NFI_FIFOSTA)
/*
 * NFI_FDM0L is at 0x0A0, not 0x080. The datasheet, the preloader's
 * mtk_nand_core.h (NFI_FDM0L_REG32 = NFI_BASE + 0x00A0), LK's mtk_nand.h and
 * mainline's mtk_nand.c (NFI_FDML(x) = 0xA0 + ...) all agree on 0xA0, and this
 * file had 0x80 - so the free-data bytes a driver reads back came from the
 * wrong register.
 */
#define NFI_FDM0L     NFI_OFF(MT6589_NFI_NFI_FDM0L)

QEMU_BUILD_BUG_ON(NFI_CNFG != 0x00);
QEMU_BUILD_BUG_ON(NFI_PAGEFMT != 0x04);
QEMU_BUILD_BUG_ON(NFI_CON != 0x08);
QEMU_BUILD_BUG_ON(NFI_ACCCON != 0x0c);
QEMU_BUILD_BUG_ON(NFI_INTR_EN != 0x10);
QEMU_BUILD_BUG_ON(NFI_INTR != 0x14);
QEMU_BUILD_BUG_ON(NFI_CMD != 0x20);
QEMU_BUILD_BUG_ON(NFI_ADDRNOB != 0x30);
QEMU_BUILD_BUG_ON(NFI_COLADDR != 0x34);
QEMU_BUILD_BUG_ON(NFI_ROWADDR != 0x38);
QEMU_BUILD_BUG_ON(NFI_STA != 0x60);
QEMU_BUILD_BUG_ON(NFI_FIFOSTA != 0x64);
QEMU_BUILD_BUG_ON(NFI_FDM0L != 0xa0);

/*
 * NFIECC, base 0x1100_2000. The encoder and decoder are separate register
 * pages, 0x000 and 0x100 - not adjacent offsets. Getting this wrong is silent
 * in the worst way: a poll on DECIDLE at 0x10c reads 0 from an unmapped offset
 * and the guest waits for ever, which looks exactly like a missing device.
 */
#define NFIECC_ENCCON    0x000
#define NFIECC_ENCCNFG   0x004
#define NFIECC_ENCIDLE   0x00c
#define NFIECC_ENCSTA    0x02c
#define NFIECC_DECCON    0x100
#define NFIECC_DECCNFG   0x104
#define NFIECC_DECIDLE   0x10c
#define NFIECC_DECDONE   0x11c

/* NFIECC_ENCCON / DECCON */
#define NFIECC_CON_EN    (1u << 0)
/* NFI_STA */
#define NFI_STA_BUSY     (1u << 0)

static uint64_t mt6589_nfi_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589NfiState *s = opaque;

    /* NFIECC lives in the second 4 KiB page of this region. */
    if (offset >= 0x1000) {
        switch (offset - 0x1000) {
        case NFIECC_ENCCON:
            return s->enccon;
        case NFIECC_ENCCNFG:
            return s->enccnfg;
        case NFIECC_ENCIDLE:
            /*
             * "Encoder available". Reporting idle whenever the encoder is not
             * enabled, and also when it is (because a block encodes
             * instantaneously here), keeps both the probe and the sample-code
             * sequence in the datasheet happy. Leaving it 0 is what stalls the
             * boot ROM.
             */
            return (s->enccon & NFIECC_CON_EN) ? 1u : 1u;
        case NFIECC_DECCON:
            return s->deccon;
        case NFIECC_DECCNFG:
            return s->deccnfg;
        case NFIECC_DECIDLE:
            /* "Decoder available", same shape as ENCIDLE. */
            return 1u;
        case NFIECC_DECDONE:
            return 1u;
        }
        /* Any other documented register reads its stored value or 0. */
        if ((offset - 0x1000) < 0x200) {
            return s->ecc_regs[((offset - 0x1000) >> 2) & 0x7f];
        }
        return 0;
    }

    switch (offset) {
    case NFI_CNFG:     return s->cnfg;
    case NFI_PAGEFMT:  return s->pagefmt;
    case NFI_CON:      return s->con;
    case NFI_ACCCON:   return s->acccon;
    case NFI_INTR_EN:  return s->intr_en;
    case NFI_INTR:     return s->intr;
    case NFI_ADDRNOB:  return s->addrnob;
    case NFI_COLADDR:  return s->coladdr;
    case NFI_ROWADDR:  return s->rowaddr;
    case NFI_STA:
        /* Never busy: anything the guest starts here completes at once, so a
         * poll on BUSY clears immediately. */
        return s->sta & ~NFI_STA_BUSY;
    case NFI_FIFOSTA:
        /* FIFO always empty and ready. */
        return 0;
    }
    qemu_log_mask(LOG_UNIMP, "%s: nfi read offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void mt6589_nfi_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Mt6589NfiState *s = opaque;

    if (offset >= 0x1000) {
        switch (offset - 0x1000) {
        case NFIECC_ENCCON:  s->enccon = value; return;
        case NFIECC_ENCCNFG: s->enccnfg = value; return;
        case NFIECC_DECCON:  s->deccon = value; return;
        case NFIECC_DECCNFG: s->deccnfg = value; return;
        }
        if ((offset - 0x1000) < 0x200) {
            s->ecc_regs[((offset - 0x1000) >> 2) & 0x7f] = value;
        }
        return;
    }

    switch (offset) {
    case NFI_CNFG:    s->cnfg = value; return;
    case NFI_PAGEFMT: s->pagefmt = value; return;
    case NFI_CON:     s->con = value; return;
    case NFI_ACCCON:  s->acccon = value; return;
    case NFI_INTR_EN: s->intr_en = value; return;
    case NFI_INTR:    s->intr &= ~((uint32_t)value); return;
    case NFI_CMD:     s->cmd = value; return;
    case NFI_ADDRNOB: s->addrnob = value; return;
    case NFI_COLADDR: s->coladdr = value; return;
    case NFI_ROWADDR: s->rowaddr = value; return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: nfi write 0x%" PRIx64 " offset 0x%"
                  HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_nfi_ops = {
    .read = mt6589_nfi_read,
    .write = mt6589_nfi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_nfi_init(Object *obj)
{
    Mt6589NfiState *s = MT6589_NFI(obj);

    /* 0x1100_1000 (NFI) and 0x1100_2000 (NFIECC) are adjacent, so one 8 KiB
     * region covers both and the handlers split on the offset. */
    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_nfi_ops, s,
                          "mt6589.nfi", 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mt6589_nfi_class_init(ObjectClass *oc, const void *data)
{
}

static const TypeInfo mt6589_nfi_types[] = {
    {
        .name = TYPE_MT6589_NFI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589NfiState),
        .instance_init = mt6589_nfi_init,
        .class_init = mt6589_nfi_class_init,
    },
};

DEFINE_TYPES(mt6589_nfi_types)
