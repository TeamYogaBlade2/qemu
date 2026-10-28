/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * topaxi - generated from the MT6589 datasheet by
 * mt6589-emulator/tools/gen_regs_header.py.  DO NOT EDIT.
 *
 * page 0x10201000, 3 registers.
 *
 * Extract this page with tools/extract_all_regs.py, which is the
 * only pipeline that sees every register: it ignores the
 * datasheet's unreliable module headings and groups by address.
 */
#ifndef HW_ARM_TOPAXI_REGS_H
#define HW_ARM_TOPAXI_REGS_H

#define MT6589_TOPAXI_BASE 0x10201000u

/* 0x0000  TOPAXI_SI0_STA - Top AXI fabric SI0 control register  [reset 0x00000000u] */
#define MT6589_TOPAXI_TOPAXI_SI0_STA 0x10201000u
#define MT6589_TOPAXI_TOPAXI_SI0_STA_RESET 0x00000000u

/* 0x0004  TOPAXI_SI1_CTL - Top AXI fabric SI1 control register  [reset 0x00000000u] */
#define MT6589_TOPAXI_TOPAXI_SI1_CTL 0x10201004u
#define MT6589_TOPAXI_TOPAXI_SI1_CTL_RESET 0x00000000u

/* 0x001c  TOPAXI_MI_STA - TOPAXI fabric MI status  [reset 0x00000000u] */
#define MT6589_TOPAXI_TOPAXI_MI_STA 0x1020101cu
#define MT6589_TOPAXI_TOPAXI_MI_STA_RESET 0x00000000u

#define MT6589_TOPAXI_NUM_REGS 3

#endif /* HW_ARM_TOPAXI_REGS_H */
