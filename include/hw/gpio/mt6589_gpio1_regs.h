/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Registers that the datasheet extraction places in the second pin window (0x1020_c000), generated
 * from mt6589-gpio1-regs.h.  DO NOT EDIT.
 *
 * Only these exist there. Nine of them - IES_CON7, IES_CON10, PULLEN7, PULLEN10, PULLSEL7,
 * PULLSEL10, SMT_CON7, SMT_CON10 - are also listed in the first window's page at the *same* offset,
 * which is the trap: including both generated headers in one file silently makes the later #define
 * win, and any model that treats the second window as a full copy of the first will serve registers
 * the hardware does not have there.
 */
#ifndef HW_GPIO_MT6589_GPIO1_REGS_H
#define HW_GPIO_MT6589_GPIO1_REGS_H

#define MT6589_GPIO1_WIN_BASE             0x000
#define MT6589_GPIO1_WIN_GPIO_IES_CON7    0x170
#define MT6589_GPIO1_WIN_GPIO_IES_CON8    0x180
#define MT6589_GPIO1_WIN_GPIO_IES_CON9    0x190
#define MT6589_GPIO1_WIN_GPIO_IES_CON10   0x1a0
#define MT6589_GPIO1_WIN_GPIO_PULLEN7     0x270
#define MT6589_GPIO1_WIN_GPIO_PULLEN8     0x280
#define MT6589_GPIO1_WIN_GPIO_PULLEN9     0x290
#define MT6589_GPIO1_WIN_GPIO_PULLEN10    0x2a0
#define MT6589_GPIO1_WIN_GPIO_SMT_CON7    0x370
#define MT6589_GPIO1_WIN_GPIO_SMT_CON8    0x380
#define MT6589_GPIO1_WIN_GPIO_SMT_CON9    0x390
#define MT6589_GPIO1_WIN_GPIO_SMT_CON10   0x3a0
#define MT6589_GPIO1_WIN_GPIO_PULLSEL7    0x470
#define MT6589_GPIO1_WIN_GPIO_PULLSEL8    0x480
#define MT6589_GPIO1_WIN_GPIO_PULLSEL9    0x490
#define MT6589_GPIO1_WIN_GPIO_PULLSEL10   0x4a0
#define MT6589_GPIO1_WIN_GPIO_DRV_CON6    0x560
#define MT6589_GPIO1_WIN_GPIO_DRV_CON7    0x570
#define MT6589_GPIO1_WIN_GPIO_DRV_CON8    0x580
#define MT6589_GPIO1_WIN_GPIO_DRV_CON9    0x590
#define MT6589_GPIO1_WIN_GPIO_DSEL_CON6   0x760
#define MT6589_GPIO1_WIN_GPIO_DSEL_CON7   0x770
#define MT6589_GPIO1_WIN_GPIO_BIAS_CON2   0x910
#define MT6589_GPIO1_WIN_GPIO_BIAS_CON3   0x920

#endif /* HW_GPIO_MT6589_GPIO1_REGS_H */
