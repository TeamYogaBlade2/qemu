/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * c1000000 - generated from the MT6589 datasheet by
 * mt6589-emulator/tools/gen_regs_header.py.  DO NOT EDIT.
 *
 * page 0xc1000000, 1 registers.
 *
 * Extract this page with tools/extract_all_regs.py, which is the
 * only pipeline that sees every register: it ignores the
 * datasheet's unreliable module headings and groups by address.
 */
#ifndef HW_ARM_C1000000_REGS_H
#define HW_ARM_C1000000_REGS_H

#define MT6589_C1000000_BASE 0xc1000000u

/* 0x0050  PERI_GLOBALCON_DCMCTL - Peripheral DCM Control Register  [reset 0x000000f2u] */
#define MT6589_C1000000_PERI_GLOBALCON_DCMCTL 0xc1000050u
#define MT6589_C1000000_PERI_GLOBALCON_DCMCTL_RESET 0x000000f2u
#define MT6589_C1000000_PERI_GLOBALCON_DCMCTL_DCM_IDLE_BYPASS_EN_SHIFT 15
#define MT6589_C1000000_PERI_GLOBALCON_DCMCTL_DCM_IDLE_BYPASS_EN_MASK  0x00008000u
#define MT6589_C1000000_PERI_GLOBALCON_DCMCTL_AHB_BUS_SLP_REQAXI_CLOCK_GATED_ENDCM_ENABLE_SHIFT 0
#define MT6589_C1000000_PERI_GLOBALCON_DCMCTL_AHB_BUS_SLP_REQAXI_CLOCK_GATED_ENDCM_ENABLE_MASK  0x00007fffu

#define MT6589_C1000000_NUM_REGS 1

#endif /* HW_ARM_C1000000_REGS_H */
