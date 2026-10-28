/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * accdet - generated from the MT6589 datasheet by
 * mt6589-emulator/tools/gen_regs_header.py.  DO NOT EDIT.
 *
 * page 0xc1016000, 3 registers.
 *
 * Extract this page with tools/extract_all_regs.py, which is the
 * only pipeline that sees every register: it ignores the
 * datasheet's unreliable module headings and groups by address.
 */
#ifndef HW_ARM_ACCDET_REGS_H
#define HW_ARM_ACCDET_REGS_H

#define MT6589_ACCDET_BASE 0xc1016000u

/* 0x0000  ACCDET_RSTB - ACCDET Software Reset Register  [reset 0x00000001u] */
#define MT6589_ACCDET_ACCDET_RSTB 0xc1016000u
#define MT6589_ACCDET_ACCDET_RSTB_RESET 0x00000001u

/* 0x0004  ACCDET_CTRL - ACCDET Control Register  [reset 0x00000000u] */
#define MT6589_ACCDET_ACCDET_CTRL 0xc1016004u
#define MT6589_ACCDET_ACCDET_CTRL_RESET 0x00000000u

/* 0x0008  ACCDET_STATE_SWCTRL - ACCDET State Switch Control Register  [reset 0x00000001u] */
#define MT6589_ACCDET_ACCDET_STATE_SWCTRL 0xc1016008u
#define MT6589_ACCDET_ACCDET_STATE_SWCTRL_RESET 0x00000001u
#define MT6589_ACCDET_ACCDET_STATE_SWCTRL_VTH_PWM_EN_SHIFT 3
#define MT6589_ACCDET_ACCDET_STATE_SWCTRL_VTH_PWM_EN_MASK  0x00000008u
#define MT6589_ACCDET_ACCDET_STATE_SWCTRL_CMP_PWM_EN_SHIFT 2
#define MT6589_ACCDET_ACCDET_STATE_SWCTRL_CMP_PWM_EN_MASK  0x00000004u

#define MT6589_ACCDET_NUM_REGS 3

#endif /* HW_ARM_ACCDET_REGS_H */
