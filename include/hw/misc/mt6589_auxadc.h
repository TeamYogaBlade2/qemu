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
 */

#ifndef HW_MISC_MT6589_AUXADC_H
#define HW_MISC_MT6589_AUXADC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_AUXADC "mt6589-auxadc"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589AuxAdcState, MT6589_AUXADC)

/*
 * DAT0..DAT15 sit at 0x014..0x050, which the datasheet omits; the last register
 * it does document in this block is TS_RAW_CON/TS_AUTO_TIME_INTVL at 0x100/0x104,
 * and the generated header's highest offset is 0x104. 0x1000 covers the whole
 * window with room to spare and matches the 4 KiB stride of the peripherals
 * around it.
 */
#define MT6589_AUXADC_SIZE 0x1000

/* DAT0..DAT15 - the results the driver indexes by channel. */
#define MT6589_AUXADC_NUM_CHANNELS 16

struct Mt6589AuxAdcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;

    uint32_t con0;
    uint32_t con1;
    uint32_t con2;
    uint32_t det_volt;
    uint32_t det_sel;
    uint32_t det_period;
    uint32_t det_debt;
    uint32_t misc;
    uint32_t ecc;
    uint32_t sample_list;
    uint32_t abist_period;

    uint32_t dat[MT6589_AUXADC_NUM_CHANNELS];

    /* Board property: what each input reads, in 12-bit counts. */
    uint32_t num_samples;
    uint32_t *sample;
};

#endif
