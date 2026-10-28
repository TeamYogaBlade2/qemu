/*
 * MediaTek MT6320/MT6323 PMIC, the device behind the MT6589's PWRAP.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MT6320_PMIC_H
#define HW_MISC_MT6320_PMIC_H

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_MT6320_PMIC "mt6320-pmic"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6320PmicState, MT6320_PMIC)

struct Mt6320PmicState {
    DeviceState parent_obj;

    /*
     * Register file indexed by address >> 1, which is the value the wrapper puts in its WACS field.
     * The map in mt6320_pmic_regs.h stops at 0x746, but the space is larger than the named registers:
     * the wrapper's link read test uses 0xBC0A, and the preloader fails into download mode if it reads
     * zero there - measured. 32K entries covers the whole 16-bit space the wrapper addresses.
     */
    uint16_t regs[0x8000];
    uint32_t cid;
    /* Frequency meter: how many measurements have been started. */
    uint32_t fqmtr_started, fqmtr_n;
};

/* Access by PMIC register address (2-byte aligned) or by the wrapper's WACS value (address >> 1). */
uint16_t mt6320_pmic_read(Mt6320PmicState *s, uint16_t reg);
void mt6320_pmic_write(Mt6320PmicState *s, uint16_t reg, uint16_t value);
uint16_t mt6320_pmic_read_wacs(Mt6320PmicState *s, uint16_t wacs);
void mt6320_pmic_write_wacs(Mt6320PmicState *s, uint16_t wacs, uint16_t value);
/* Raise interrupt source bit n of INT_STATUS0 (n < 16) or INT_STATUS1. */
void mt6320_pmic_raise_irq(Mt6320PmicState *s, unsigned bit);

#endif /* HW_MISC_MT6320_PMIC_H */
