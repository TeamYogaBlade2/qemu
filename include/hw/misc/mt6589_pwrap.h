/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 PMIC_WRAP (pwrap)
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef HW_MISC_MT6589_PWRAP_H
#define HW_MISC_MT6589_PWRAP_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_PWRAP "mt6589-pwrap"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589PwrapState, MT6589_PWRAP)

struct Mt6589PwrapState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /* Wrapper-side FSM state, in the encoding the guest polls (0 idle, 6 valid). */
    uint32_t fsm;
    uint32_t rdata;
    uint32_t cipher_rdy;
    uint32_t aux_dio_en, aux_arb_sidly, aux_hiprio_en;
    uint32_t aux_int_en;
    uint32_t aux_event_in_en, aux_event_dst_en, aux_event_sta;
    uint32_t vldclr_polls;
    uint32_t fsm_owner;      /* task that parked the result */
    uint32_t aux_harb_sta0, aux_harb_sta1;


    /* The device on the other side; when set, accesses go through it. */
    struct Mt6320PmicState *pmic_dev;
};

/* PMIC register access by WACS number, shared with the pwrap bridge. */
uint16_t mt6589_pwrap_pmic_read(Mt6589PwrapState *s, uint16_t wacs);
void mt6589_pwrap_pmic_write(Mt6589PwrapState *s, uint16_t wacs, uint16_t value);

void mt6589_pwrap_connect_pmic(Mt6589PwrapState *s, struct Mt6320PmicState *pmic);

#endif /* HW_MISC_MT6589_PWRAP_H */
