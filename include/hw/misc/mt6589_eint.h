/*
 * MediaTek MT6589 external interrupt controller (EINT).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MT6589_EINT_H
#define HW_MISC_MT6589_EINT_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_EINT "mt6589-eint"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589EintState, MT6589_EINT)

struct Mt6589EintState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[0x1000 / 4];
    uint32_t line_asserted[6];   /* logical state per bank of 32 lines */
    uint32_t pmic_sta, pmic_mask;
};

/* Drive one external line, e.g. from a PMIC model asserting its interrupt output. */
void mt6589_eint_set_line(Mt6589EintState *s, unsigned line, bool asserted);
/* Raise one of the PMIC's interrupt bits through the passthrough group. */
void mt6589_eint_raise_pmic(Mt6589EintState *s, unsigned bit);

#endif /* HW_MISC_MT6589_EINT_H */
