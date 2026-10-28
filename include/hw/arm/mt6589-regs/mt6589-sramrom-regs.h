/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * sramrom - generated from the MT6589 datasheet by
 * mt6589-emulator/tools/gen_regs_header.py.  DO NOT EDIT.
 *
 * page 0x10002000, 3 registers.
 *
 * Extract this page with tools/extract_all_regs.py, which is the
 * only pipeline that sees every register: it ignores the
 * datasheet's unreliable module headings and groups by address.
 */
#ifndef HW_ARM_SRAMROM_REGS_H
#define HW_ARM_SRAMROM_REGS_H

#define MT6589_SRAMROM_BASE 0x10002000u

/* 0x0000  SEC_DOMAIN - Security setting register  [reset 0x00000000u] */
#define MT6589_SRAMROM_SEC_DOMAIN 0x10002000u
#define MT6589_SRAMROM_SEC_DOMAIN_RESET 0x00000000u
#define MT6589_SRAMROM_SEC_DOMAIN_SEC_SET2_SHIFT 16
#define MT6589_SRAMROM_SEC_DOMAIN_SEC_SET2_MASK  0x00070000u
#define MT6589_SRAMROM_SEC_DOMAIN_SEC_SET1_SHIFT 8
#define MT6589_SRAMROM_SEC_DOMAIN_SEC_SET1_MASK  0x00000700u
#define MT6589_SRAMROM_SEC_DOMAIN_SEC_SET0_SHIFT 0
#define MT6589_SRAMROM_SEC_DOMAIN_SEC_SET0_MASK  0x00000007u

/* 0x0010  SEC_VIO_STAT - Security violation status register  [reset 0x00000000u] */
#define MT6589_SRAMROM_SEC_VIO_STAT 0x10002010u
#define MT6589_SRAMROM_SEC_VIO_STAT_RESET 0x00000000u

/* 0x0014  SEC_VIO_ADDR - Security violation address register  [reset 0x00000000u] */
#define MT6589_SRAMROM_SEC_VIO_ADDR 0x10002014u
#define MT6589_SRAMROM_SEC_VIO_ADDR_RESET 0x00000000u
#define MT6589_SRAMROM_SEC_VIO_ADDR_SEC_VIO_ADDR_31_16__SHIFT 16
#define MT6589_SRAMROM_SEC_VIO_ADDR_SEC_VIO_ADDR_31_16__MASK  0xffff0000u
#define MT6589_SRAMROM_SEC_VIO_ADDR_SEC_VIO_ADDR_15_0__SHIFT 0
#define MT6589_SRAMROM_SEC_VIO_ADDR_SEC_VIO_ADDR_15_0__MASK  0x0000ffffu

#define MT6589_SRAMROM_NUM_REGS 3

#endif /* HW_ARM_SRAMROM_REGS_H */
