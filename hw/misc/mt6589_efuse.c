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

static const struct Mt6589EfuseByte mt6589_efuse_bytes[] = {
    /* 0x0000: 0011 000f 0062 0000 */
    { 0x0000, 0x11 }, { 0x0001, 0x00 }, { 0x0002, 0x0f }, { 0x0003, 0x00 },
    { 0x0004, 0x62 },
    /* 0x0020: 0010 */
    { 0x0020, 0x10 },
    /* 0x0040: 1244 80c6 */
    { 0x0040, 0x44 }, { 0x0041, 0x12 }, { 0x0042, 0xc6 }, { 0x0043, 0x80 },
    /* 0x0100: 005d 0000 9a80 ca92 */
    { 0x0100, 0x5d },
    { 0x0104, 0x80 }, { 0x0105, 0x9a }, { 0x0106, 0x92 }, { 0x0107, 0xca },
    /* 0x0128: 0007 */
    { 0x0128, 0x07 },
    /* 0x0130: 0040 ... 0138: 0007 */
    { 0x0130, 0x40 },
    { 0x0138, 0x07 },
    /* 0x0140: c664 2a84 c70d 40c9 */
    { 0x0140, 0x64 }, { 0x0141, 0xc6 }, { 0x0142, 0x84 }, { 0x0143, 0x2a },
    { 0x0144, 0x0d }, { 0x0145, 0xc7 }, { 0x0146, 0xc9 }, { 0x0147, 0x40 },
    /* 0x0178: 2b07 1033 5555 f955 */
    { 0x0178, 0x07 }, { 0x0179, 0x2b }, { 0x017a, 0x33 }, { 0x017b, 0x10 },
    { 0x017c, 0x55 }, { 0x017d, 0x55 }, { 0x017e, 0x55 }, { 0x017f, 0xf9 },
    /* 0x0184: 0400 286d */
    { 0x0184, 0x00 }, { 0x0185, 0x04 }, { 0x0186, 0x6d }, { 0x0187, 0x28 },
};

/* Offset of the array above within the window. */
#define EFUSE_DATA_OFFSET 0x0000

static uint64_t mt6589_efuse_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589EfuseState *s = opaque;
    uint64_t v = 0;

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
    /*
     * Fuses cannot be programmed once the part has left the factory, and the
     * boot path never writes here. Accept and ignore rather than raising a bus
     * error: firmware probing the window with a read-modify-write is not doing
     * anything wrong, and an abort would be far more disruptive than a no-op.
     */
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
