/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * kpd - generated from the MT6589 datasheet by
 * mt6589-emulator/tools/gen_regs_header.py.  DO NOT EDIT.
 *
 * page 0x10015000, 5 registers.
 *
 * Extract this page with tools/extract_all_regs.py, which is the
 * only pipeline that sees every register: it ignores the
 * datasheet's unreliable module headings and groups by address.
 */
#ifndef HW_ARM_KPD_REGS_H
#define HW_ARM_KPD_REGS_H

#define MT6589_KPD_BASE 0x10015000u

/* 0x001c  SEL_PMIC - KP's row COL pad is from PMIC  [reset 0x00000000u] */
#define MT6589_KPD_SEL_PMIC 0x1001501cu
#define MT6589_KPD_SEL_PMIC_RESET 0x00000000u
#define MT6589_KPD_SEL_PMIC_SEL_PMIC_SHIFT 0
#define MT6589_KPD_SEL_PMIC_SEL_PMIC_MASK  0x00000001u

/* 0x0020  PMIC_REG_ADR - PMIC's register read address  [reset 0x00000000u] */
#define MT6589_KPD_PMIC_REG_ADR 0x10015020u
#define MT6589_KPD_PMIC_REG_ADR_RESET 0x00000000u
#define MT6589_KPD_PMIC_REG_ADR_PMIC_REG_ADR_SHIFT 0
#define MT6589_KPD_PMIC_REG_ADR_PMIC_REG_ADR_MASK  0x0000ffffu

/* 0x0024  PMIC_REG_RATA - PMIC's register read data  [reset 0x00000000u] */
#define MT6589_KPD_PMIC_REG_RATA 0x10015024u
#define MT6589_KPD_PMIC_REG_RATA_RESET 0x00000000u
#define MT6589_KPD_PMIC_REG_RATA_PMIC_REG_RATA_SHIFT 0
#define MT6589_KPD_PMIC_REG_RATA_PMIC_REG_RATA_MASK  0x0000ffffu

/* 0x0028  PMIC_REG_RDY - PMIC's register read ready  [reset 0x00000000u] */
#define MT6589_KPD_PMIC_REG_RDY 0x10015028u
#define MT6589_KPD_PMIC_REG_RDY_RESET 0x00000000u

/* 0x002c  BYPASS_IDLE - Bypass CG in PMIC I2C's interface  [reset 0x00000000u] */
#define MT6589_KPD_BYPASS_IDLE 0x1001502cu
#define MT6589_KPD_BYPASS_IDLE_RESET 0x00000000u

#define MT6589_KPD_NUM_REGS 5

#endif /* HW_ARM_KPD_REGS_H */
