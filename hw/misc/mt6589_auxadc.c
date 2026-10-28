/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 AUXADC
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * Register offsets and field positions come from the generated header
 * include/hw/arm/mt6589-regs/mt6589-auxadc-regs.h, which is extracted from the
 * datasheet. Two things the datasheet does not give, and where they came from
 * instead:
 *
 *  1. **DAT0..DAT15 (0x014..0x050)** are in the datasheet, but as a two-line
 *     array row (`11003014 ~` / `11003050` / `AUXADC_DAT[n]` / `(n=0~15)`),
 *     which the extractor used to drop. The firmware reads them as
 *     `mtk_auxadc.c: AUXADC_DAT0 + channel * 0x04`. **Bit 12 of each is the
 *     data-ready flag**, which the datasheet's field tables do not say - that
 *     comes from the driver, which polls it before and after starting a
 *     conversion.
 *
 *  2. **The conversion handshake.** The datasheet's CON1 field table is flagged
 *     DATASHEET-ISSUE by the generator (bit 5 claimed by both IMM1 and IMM5, and
 *     so on), so the reliable description of the protocol is the driver:
 *
 *         mtk_auxadc.c IMMAuxADC_GetProcessedRawData():
 *           while (CON2 & 0x01)                       // wait for module idle
 *           CON1 &= ~(1 << ch)                        // drop the request
 *           while (DAT[ch] & (1 << 12))               // wait for ready to clear
 *           CON1 |=  (1 << ch)                        // start the conversion
 *           while (!(DAT[ch] & (1 << 12)))            // wait for ready to set
 *           value = DAT[ch] & 0x0fff
 *
 *     So **bit 12 of each DAT register is the data-ready flag**, not part of the
 *     value, and CON2 bit 0 is a module-idle flag that reads clear when idle.
 *
 * The model therefore has to honour the clear-then-set dance. If DAT[ch] bit 12
 * is set as soon as the channel is enabled, the driver's "wait for ready to
 * clear" loop spins instead - the opposite failure to the one it looks like it
 * should have.
 *
 * Why this file exists at all: with no AUXADC, LK's battery read times out and
 * returns 0, which the firmware reads as 0 mV, sets `is_low_battery, TRUE` and
 * answers with mt_power_off, spinning on a charger that a desktop has no way to
 * provide. An all-zero AUXADC is not neutral - it is a flat battery. The sample
 * values are a board property so the difference is explicit in the machine
 * definition rather than hidden in the model.
 */

#include "qemu/osdep.h"
#include "hw/misc/mt6589_auxadc.h"
#include "hw/arm/mt6589-regs/mt6589-auxadc-regs.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

/*
 * The datasheet does not name DAT0..DAT15, so the generator could not emit them.
 * Their offsets are pinned here from the driver's `AUXADC_DAT0 + ch * 0x04`
 * against the header's `MT6589_AUXADC_AUXADC_CON2` (0x010), and the field below
 * is the ready flag the driver polls.
 */
#define R_DAT0          (MT6589_AUXADC_AUXADC_CON2 + 0x04)
#define R_DAT(ch)       (R_DAT0 + 0x04 * (ch))
#define DAT_READY       (1u << 12)
#define DAT_VALUE_MASK  0x0fffu

/* CON2 bit 0: module busy, reads set while a conversion is outstanding. */
#define CON2_BUSY       (1u << 0)

/*
 * CON2 is the module status register. The driver waits for it to read clear
 * before starting, so the model must not leave it set after a conversion
 * finishes - clear it as the result becomes available.
 */

static void mt6589_auxadc_settle(Mt6589AuxAdcState *s)
{
    /*
     * A channel that has been requested produces a result immediately and
     * clears the busy flag. Doing this synchronously rather than after a delay
     * keeps the model deterministic: the driver's poll loops have no delay that
     * would advance a virtual clock, so a conversion that "takes time" would
     * never appear to complete.
     */
    for (int i = 0; i < MT6589_AUXADC_NUM_CHANNELS; i++) {
        if (s->con1 & (1u << i)) {
            uint32_t v = s->sample[i] & DAT_VALUE_MASK;
            s->dat[i] = DAT_READY | v;
            s->con2 &= ~CON2_BUSY;
        }
    }
}

static uint64_t mt6589_auxadc_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589AuxAdcState *s = MT6589_AUXADC(opaque);

    if (offset >= R_DAT0 &&
        offset < R_DAT0 + 4 * MT6589_AUXADC_NUM_CHANNELS) {
        return s->dat[(offset - R_DAT0) / 4];
    }

    switch (offset) {
    case MT6589_AUXADC_AUXADC_CON0:
        return s->con0;
    case MT6589_AUXADC_AUXADC_CON1:
        return s->con1;
    case MT6589_AUXADC_AUXADC_CON2:
        return s->con2;
    case MT6589_AUXADC_AUXADC_DET_VOLT:
        return s->det_volt;
    case MT6589_AUXADC_AUXADC_DET_SEL:
        return s->det_sel;
    case MT6589_AUXADC_AUXADC_DET_PERIOD:
        return s->det_period;
    case MT6589_AUXADC_AUXADC_DET_DEBT:
        return s->det_debt;
    case MT6589_AUXADC_AUXADC_MISC:
        return s->misc;
    case MT6589_AUXADC_AUXADC_ECC:
        return s->ecc;
    case MT6589_AUXADC_AUXADC_SAMPLE_LIST:
        return s->sample_list;
    case MT6589_AUXADC_AUXADC_ABIST_PERIOD:
        return s->abist_period;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read of unimplemented 0x%" HWADDR_PRIx
                      "\n", __func__, offset);
        return 0;
    }
}

static void mt6589_auxadc_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    Mt6589AuxAdcState *s = MT6589_AUXADC(opaque);

    switch (offset) {
    case MT6589_AUXADC_AUXADC_CON0:
        s->con0 = value;
        /* CON0's AUTOSET bits request channels the same way CON1 does. */
        s->con1 |= value;
        mt6589_auxadc_settle(s);
        break;
    case MT6589_AUXADC_AUXADC_CON1:
        /*
         * The driver clears the channel bit and then waits for DAT[ch] bit 12
         * to drop, so a cleared request has to clear the ready flag too. Without
         * that the "wait for ready to clear" loop never exits.
         */
        for (int i = 0; i < MT6589_AUXADC_NUM_CHANNELS; i++) {
            if ((s->con1 & (1u << i)) && !(value & (1u << i))) {
                s->dat[i] &= ~DAT_READY;
            }
        }
        s->con1 = value;
        if (value) {
            s->con2 |= CON2_BUSY;
            mt6589_auxadc_settle(s);
        }
        break;
    case MT6589_AUXADC_AUXADC_CON1_SET:
        /*
         * Write-1-to-set alias. The driver uses CON1 itself, but the header
         * documents this one and a guest using it must behave identically.
         */
        for (int i = 0; i < MT6589_AUXADC_NUM_CHANNELS; i++) {
            if (value & (1u << i)) {
                s->con1 |= (1u << i);
            }
        }
        s->con2 |= CON2_BUSY;
        mt6589_auxadc_settle(s);
        break;
    case MT6589_AUXADC_AUXADC_CON1_CLR:
        for (int i = 0; i < MT6589_AUXADC_NUM_CHANNELS; i++) {
            if (value & (1u << i)) {
                s->con1 &= ~(1u << i);
                s->dat[i] &= ~DAT_READY;
            }
        }
        break;
    case MT6589_AUXADC_AUXADC_CON2:
        /* Writable per the header; the driver only reads it. */
        s->con2 = value;
        break;
    case MT6589_AUXADC_AUXADC_DET_VOLT:
        s->det_volt = value;
        break;
    case MT6589_AUXADC_AUXADC_DET_SEL:
        s->det_sel = value;
        break;
    case MT6589_AUXADC_AUXADC_DET_PERIOD:
        s->det_period = value;
        break;
    case MT6589_AUXADC_AUXADC_DET_DEBT:
        s->det_debt = value;
        break;
    case MT6589_AUXADC_AUXADC_MISC:
        s->misc = value;
        break;
    case MT6589_AUXADC_AUXADC_ECC:
        s->ecc = value;
        break;
    case MT6589_AUXADC_AUXADC_SAMPLE_LIST:
        s->sample_list = value;
        break;
    case MT6589_AUXADC_AUXADC_ABIST_PERIOD:
        s->abist_period = value;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write of 0x%" PRIx64
                      " to unimplemented 0x%" HWADDR_PRIx "\n",
                      __func__, value, offset);
        break;
    }
}

static const MemoryRegionOps mt6589_auxadc_ops = {
    .read = mt6589_auxadc_read,
    .write = mt6589_auxadc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 2,
        .max_access_size = 4,
    },
};

static void mt6589_auxadc_reset(DeviceState *dev)
{
    Mt6589AuxAdcState *s = MT6589_AUXADC(dev);

    s->con0 = 0;
    s->con1 = 0;
    s->con2 = 0;
    s->det_volt = 0;
    s->det_sel = 0;
    s->det_period = 0;
    s->det_debt = 0;
    s->misc = 0;
    s->ecc = 0;
    s->sample_list = 0;
    s->abist_period = 0;
    memset(s->dat, 0, sizeof(s->dat));
    /* sample[] is a board property, not device state, so reset leaves it. */
}

static void mt6589_auxadc_init(Object *obj)
{
    Mt6589AuxAdcState *s = MT6589_AUXADC(obj);

    memory_region_init_io(&s->iomem, obj, &mt6589_auxadc_ops, s,
                          TYPE_MT6589_AUXADC, MT6589_AUXADC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property mt6589_auxadc_properties[] = {
    /*
     * 12-bit sample per channel, as a flat array. Channels left at zero read as
     * zero, which is what an unconnected input does - so a board that wants a
     * battery must say so here.
     *
     * Set from the machine with e.g.
     *   -global mt6589-auxadc.sample[0]=0xb00
     */
    DEFINE_PROP_ARRAY("sample", Mt6589AuxAdcState, num_samples, sample,
                      qdev_prop_uint32, uint32_t),
};

static void mt6589_auxadc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, mt6589_auxadc_reset);
    device_class_set_props(dc, mt6589_auxadc_properties);
}

static const TypeInfo mt6589_auxadc_info = {
    .name = TYPE_MT6589_AUXADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Mt6589AuxAdcState),
    .instance_init = mt6589_auxadc_init,
    .class_init = mt6589_auxadc_class_init,
};

static void mt6589_auxadc_register_types(void)
{
    type_register_static(&mt6589_auxadc_info);
}

type_init(mt6589_auxadc_register_types)
