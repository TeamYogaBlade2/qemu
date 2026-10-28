/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 eFuse (EFUSEC)
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * The datasheet has no register chapter for EFUSEC - it appears only in the
 * memory map ("0x1000_9000 4KB EFUSEC") - so the semantics here come from the
 * mainline driver (drivers/nvmem/mtk-efuse.c) and from a dump taken off the real
 * board (mt6589-emulator/docs/eFuse.md).
 *
 * It is NOT a register bank. The driver's read path is
 *
 *     readl(priv->base + align) >> shift     // mtk_reg_read(), aligned_read
 *
 * i.e. the window is a flat view of fuse words: reading `base + n` returns fuse
 * byte n. There are no control/status registers to model, which is why this file
 * is a memory region rather than a device with fields.
 *
 * The contents matter. The boot ROM reads bit 0 of the word at +0x00 and branches
 * on it (see the note below), so an all-zero fuse block is not a neutral choice -
 * it sends the ROM down a specific path.
 *
 * The data below is the operator's dump from the real YOGA Tablet 10. Keeping it
 * exact is the point: the whole reason for modelling this block is that firmware
 * reads fuses to decide what the hardware is, and invented values produce a board
 * that boots differently from the one being debugged.
 *
 *     ~ # hexdump /sys/bus/nvmem/devices/nvmem1/nvmem
 *     0000000 0011 000f 0062 0000 ...
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/arm/mt6589.h"
#include "system/memory.h"

/*
 * Fuse contents, little-endian byte order, as dumped from the real board.
 *
 * Only the first 0x1a0 bytes are non-zero in the dump; the rest of the 4 KiB
 * window reads 0, which is what unprogrammed fuses read as. Stored as bytes
 * because that is how the driver reads them (one byte per fuse, assembled from a
 * 32-bit aligned read).
 */
/*
 * Fuse contents as a sparse table, keyed by byte offset within the window.
 *
 * Positional: the dump has long zero runs, and writing them out as array elements
 * is how the first version of this file silently shifted every entry past the
 * first gap - a comment naming an offset contributes no bytes, so every element
 * after the first gap lands early. Listing only the non-zero runs makes the offset
 * part of the data rather than a comment that can drift.
 *
 * Values are the operator's hexdump of the real board, little-endian per word.
 */
struct Mt6589EfuseByte {
    uint16_t offset;
    uint8_t value;
};

/*
 * The array starts at 0x100, not 0x000. Offsets 0x00 and 0x20 are the block's
 * control and status words, and putting fuse data there is how the first version of
 * this file broke the boot ROM: the ROM polls bit 0 of 0x00 as "read complete", so
 * firing fuse data at that offset made completion depend on the first fuse word's
 * low bit - which happens to be 1 in this dump, so it looked like it worked and then
 * took a branch on nonsense.
 *
 * Offsets below are the word offsets from the dump, i.e. already in this window.
 */
static const struct Mt6589EfuseByte mt6589_efuse_bytes[] = {
    /* 0x100: 0011 000f 0062 0000 */
    { 0x0100, 0x11 }, { 0x0102, 0x0f }, { 0x0104, 0x62 },
    /* 0x120: 0010 */
    { 0x0120, 0x10 },
    /* 0x140: 1244 80c6 */
    { 0x0140, 0x44 }, { 0x0141, 0x12 }, { 0x0142, 0xc6 }, { 0x0143, 0x80 },
    /* 0x160: 005d 0000 9a80 ca92 */
    { 0x0160, 0x5d },
    { 0x0164, 0x80 }, { 0x0165, 0x9a }, { 0x0166, 0x92 }, { 0x0167, 0xca },
    /* 0x188: 0007 */
    { 0x0188, 0x07 },
    /* 0x190: 0040, 0x198: 0007 */
    { 0x0190, 0x40 }, { 0x0198, 0x07 },
    /* 0x1a0: c664 2a84   0x1a4: c70d 40c9 */
    { 0x01a0, 0x64 }, { 0x01a1, 0xc6 }, { 0x01a2, 0x84 }, { 0x01a3, 0x2a },
    { 0x01a4, 0x0d }, { 0x01a5, 0xc7 }, { 0x01a6, 0xc9 }, { 0x01a7, 0x40 },
    /* 0x1c8: 2b07 1033   0x1cc: 5555 f955 */
    { 0x01c8, 0x07 }, { 0x01c9, 0x2b }, { 0x01ca, 0x33 }, { 0x01cb, 0x10 },
    { 0x01cc, 0x55 }, { 0x01cd, 0x55 }, { 0x01ce, 0x55 }, { 0x01cf, 0xf9 },
    /* 0x1d4: 0400 286d */
    { 0x01d4, 0x00 }, { 0x01d5, 0x04 }, { 0x01d6, 0x6d }, { 0x01d7, 0x28 },
};

/*
 * Control and status, decoded from the boot ROM's own accessors (the datasheet has
 * no EFUSEC register map at all - it lists the block only in the clock tree):
 *
 *   0xc88c  ldr r1,[r0]    ; orr r1,#0x4 ; str r1,[r0]   -> bit 2 = START a read
 *   0xc898  ldr r0,[r0]    ; ubfx r0,#0,#1               -> bit 0 = read COMPLETE
 *   0xc8a2  ldr r0,[r0,#0x20] ; ubfx r0,#5,#1            -> bit 5 of 0x20 = status
 *
 * FUN_0000a6b2 spins on the bit-0 accessor until it reads 1, so this must report
 * complete rather than hand back fuse data at that offset.
 */
#define EFUSE_CTRL   0x00
#define EFUSE_STS2   0x20
#define EFUSE_CTRL_DONE  (1u << 0)
#define EFUSE_CTRL_START (1u << 2)
#define EFUSE_STS2_BIT5  (1u << 5)

/* Offset of the array above within the window. */
#define EFUSE_DATA_OFFSET 0x0000

static uint64_t mt6589_efuse_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589EfuseState *s = opaque;
    uint64_t v = 0;

    /* Control/status words come before the array. */
    if (offset == EFUSE_CTRL) {
        /* Complete always (there is no shift-out delay), and echo the start bit
         * the guest set so a read-modify-write sees what it wrote. */
        return EFUSE_CTRL_DONE | (s->ctrl & EFUSE_CTRL_START);
    }
    if (offset == EFUSE_STS2) {
        return EFUSE_STS2_BIT5;
    }

    /*
     * Little-endian assembly of `size` fuse bytes. The driver does a 32-bit
     * aligned read and shifts, so a 4-byte access is the common case, but 1- and
     * 2-byte accesses are legal on the window and are handled the same way.
     */
    for (unsigned i = 0; i < size; i++) {
        hwaddr pos = offset + i;
        uint8_t b;

        b = 0;
        for (size_t k = 0; k < ARRAY_SIZE(mt6589_efuse_bytes); k++) {
            if (mt6589_efuse_bytes[k].offset == pos) {
                b = mt6589_efuse_bytes[k].value;
                break;
            }
        }
        v |= (uint64_t)b << (8 * i);
    }
    return v;
}

static void mt6589_efuse_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Mt6589EfuseState *s = opaque;

    /*
     * Fuses cannot be programmed once the part has left the factory, and the
     * boot path never writes here. Accept and ignore rather than raising a bus
     * error: firmware probing the window with a read-modify-write is not doing
     * anything wrong, and an abort would be far more disruptive than a no-op.
     */
    if (offset == EFUSE_CTRL) {
        s->ctrl = (uint32_t)value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to 0x%" HWADDR_PRIx
                  " ignored (fuses are read-only)\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_efuse_ops = {
    .read = mt6589_efuse_read,
    .write = mt6589_efuse_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    /* The driver reads 32-bit aligned words and shifts; byte access must work
     * too because nvmem may ask for one fuse at a time. */
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void mt6589_efuse_init(Object *obj)
{
    Mt6589EfuseState *s = MT6589_EFUSE(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_efuse_ops, s,
                          "mt6589.efuse", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mt6589_efuse_class_init(ObjectClass *oc, const void *data)
{
}

static const TypeInfo mt6589_efuse_types[] = {
    {
        .name = TYPE_MT6589_EFUSE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589EfuseState),
        .instance_init = mt6589_efuse_init,
        .class_init = mt6589_efuse_class_init,
    },
};

DEFINE_TYPES(mt6589_efuse_types)
