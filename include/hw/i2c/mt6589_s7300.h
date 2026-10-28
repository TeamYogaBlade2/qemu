/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Synaptics S7300 touch controller stub, attached to an MT6589 I2C bus.
 *
 * The panel is an RMI4 device: page select through register 0xff, function descriptors read as
 * 6-byte records walking down from 0xe9. See hw/i2c/mt6589_s7300.c for what is and is not modelled.
 */

#ifndef HW_I2C_MT6589_S7300_H
#define HW_I2C_MT6589_S7300_H

#include "hw/i2c/mt6589_i2c.h"

typedef struct Mt6589S7300State {
    Mt6589I2CState *i2c;
    uint8_t addr7;
    uint8_t page;
    uint8_t reg;
    uint8_t page0[256];
} Mt6589S7300State;

void mt6589_s7300_attach(Mt6589I2CState *i2c, uint8_t addr7, Mt6589S7300State *s);

#endif /* HW_I2C_MT6589_S7300_H */
