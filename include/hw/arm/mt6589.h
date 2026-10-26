/*
 * MediaTek MT6589 SoC
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#ifndef HW_ARM_MT6589_H
#define HW_ARM_MT6589_H

#include "hw/core/sysbus.h"
#include "hw/arm/boot.h"
#include "hw/char/serial-mm.h"
#include "hw/intc/arm_gic.h"
#include "hw/arm/mt6589-regs/mt6589-apxgpt-regs.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_MT6589 "mt6589"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589State, MT6589)

/*
 * Physical map. Addresses are from datasheet Table 3-1/3-2/3-3 (pages 44-49),
 * checked against the kernel's device tree. Where the two disagree the tree
 * wins, because the tree is what the guest actually programs - the SMI/LARB
 * blocks in particular are incomplete in the datasheet summary. The reasoning
 * per block is in docs/qemu/addresses.md.
 */
#define MT6589_SRAM_BASE        0x00100000
#define MT6589_DRAM_BASE        0x80000000

#define MT6589_TOPCKGEN_BASE    0x10000000
#define MT6589_TOPRGU_BASE      0x10000000
#define MT6589_SRAMROM_BASE     0x10002000
#define MT6589_PERI_BASE        0x10003000
#define MT6589_DRAMC_BASE       0x10004000
#define MT6589_GPIO_BASE        0x10005000
#define MT6589_SPM_BASE         0x10006000
#define MT6589_GPT_BASE         0x10008000
#define MT6589_PWRAP_BASE       0x1000F000
#define MT6589_MCUSYS_BASE      0x10200000
#define MT6589_SMI_COMMON_BASE  0x10202000
#define MT6589_M4U_BASE         0x10205000
#define MT6589_GIC_DIST_BASE    0x10211000
#define MT6589_GIC_CPU_BASE     0x10212000

#define MT6589_APDMA_BASE       0x11000000
#define MT6589_AUXADC_BASE      0x11003000
#define MT6589_UART0_BASE       0x11006000
#define MT6589_UART1_BASE       0x11007000
#define MT6589_UART2_BASE       0x11008000
#define MT6589_UART3_BASE       0x11009000
#define MT6589_I2C0_BASE        0x1100D000
#define MT6589_MSDC0_BASE       0x11230000

#define MT6589_DISP_BASE        0x14000000
#define MT6589_OVL_BASE         0x14003000
#define MT6589_MM_BASE          0x14010000
#define MT6589_G2D_BASE         0x14013000

/* UART: standard 16550 offsets, so hw/char/serial.c is used directly. */
#define MT6589_UART_REG_SHIFT   0
#define MT6589_UART_IRQ         90

/* GPT */
#define MT6589_GPT_IRQEN        0x00
#define MT6589_GPT_IRQSTA       0x04
#define MT6589_GPT_IRQACK       0x08
#define MT6589_GPT_CHAN(n)      (0x10 + (n) * 0x10)
#define MT6589_GPT_CON          0x00
#define MT6589_GPT_CLK          0x04
#define MT6589_GPT_COUNT        0x08
#define MT6589_GPT_COMPARE      0x0c
#define MT6589_GPT_NUM_CHANNELS 6
#define MT6589_GPT_IRQ          113

/* CON field bits, from the datasheet's GPTn_CON field table. */
#define MT6589_GPT_CON_EN       (1u << 0)
#define MT6589_GPT_CON_CLR      (1u << 1)
#define MT6589_GPT_CON_MODE_SHIFT 4
#define MT6589_GPT_CON_MODE_MASK  (3u << 4)
#define MT6589_GPT_MODE_ONESHOT 0
#define MT6589_GPT_MODE_REPEAT  1
#define MT6589_GPT_MODE_KEEPGONG 2
#define MT6589_GPT_MODE_FREERUN 3

/* CLK field: 3:0 divider, bit 4 source (0 = 13 MHz system, 1 = 32 kHz RTC). */
#define MT6589_GPT_CLK_DIV_MASK 0x0fu
#define MT6589_GPT_CLK_SRC_RTC  (1u << 4)
/* Compare-match interrupt bit in IRQEN/IRQSTA: one bit per channel. */
#define MT6589_GPT_IRQ_BIT(n)   (1u << (n))

#define MT6589_NUM_UARTS        4
#define MT6589_NUM_I2C          4

struct Mt6589State {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    ARMCPU *cpu;
    DeviceState *gic;

    /* GPT: one block, six channels, one level IRQ into the GIC. */
    MemoryRegion gpt_iomem;
    qemu_irq gpt_irq;
    QEMUTimer gpt_tick;
    uint32_t gpt_irqen;
    uint32_t gpt_irqsta;
    struct {
        uint32_t con;
        uint32_t clk;
        uint32_t compare;
        /* Emulated nanoseconds at which this channel's counter was last reset,
         * so the count is derived rather than accumulated - that is what makes
         * it survive a guest that never reads the register. */
        int64_t start_ns;
    } gpt_chan[MT6589_GPT_NUM_CHANNELS];

    SerialMM *uart[MT6589_NUM_UARTS];

    MemoryRegion sram;
    MemoryRegion peri_iomem;
};

#endif /* HW_ARM_MT6589_H */
