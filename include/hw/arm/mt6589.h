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
#include "hw/sd/mt6589_msdc.h"
#include "hw/misc/mt6589_dramc.h"
#include "hw/misc/mt6589_nfi.h"
#include "hw/misc/mt6589_toprgu.h"
#include "hw/misc/mt6589_efuse.h"
#include "hw/misc/mt6589_auxadc.h"
#include "hw/display/mt6589_disp.h"
#include "hw/misc/mt6589_pwrap.h"
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
/*
 * Boot ROM. Datasheet Table 3-1: 0x0000_0000..0x0000_FFFF is 64 KiB of on-chip
 * boot ROM. The BROM executes from here, not from DRAM - loading it at the DRAM
 * base makes it run from an address it was never linked for, which is why it
 * appeared to execute but never reached its UART poll.
 */
#define MT6589_BOOTROM_BASE     0x00000000
#define MT6589_BOOTROM_SIZE     0x00010000
/*
 * On-chip SRAM is 64 KiB at 0x0010_0000, not 256 KiB at 0x0010_0000 - datasheet
 * p44: "0x0010_0000 0x0010_FFFF 64KB On-chip SRAM". Getting the size wrong is
 * harmless until the boot ROM reads or writes past the end, at which point the
 * access goes somewhere else entirely and the failure appears somewhere unrelated.
 *
 * Note this is a different thing from "BootROM/SRAM" at 0x1000_2000 (datasheet
 * p45, 4 KiB), which is a separate block in the infrastructure window.
 */
#define MT6589_SRAM_BASE        0x00100000
#define MT6589_SRAM_SIZE        0x00010000
/*
 * The preloader's own SRAM. The boot ROM copies the whole preloader image -
 * header included - to 0x1200_0D00 and jumps to 0x1200_1000, so this is where a
 * stage runs from before DRAM is up. The datasheet describes 0x1200_0000 as a
 * bank of small SRAMs (DISP 32 KiB at +0, AUDIO 16 KiB at +0x8000, ISP 80 KiB at
 * +0xC000); they are presented here as one 2 MiB window, which is what the
 * preloader actually uses and is documented as such rather than as a claim about
 * the block layout.
 */
#define MT6589_PRERAM_BASE      0x12000000
#define MT6589_PRERAM_SIZE      0x00200000
#define MT6589_PRERAM_END       (MT6589_PRERAM_BASE + MT6589_PRERAM_SIZE)
#define MT6589_PRELOADER_LOAD   0x12000D00
#define MT6589_PRELOADER_ENTRY  0x12001000

#define MT6589_SRAMBOOT_BASE    0x10002000
#define MT6589_SRAMBOOT_SIZE    0x00001000
/*
 * Chip ID / HW and SW version, datasheet p44: 16 bytes at 0x0800_0000. The
 * variant and revision live here and the preloader reads them to pick device
 * configuration, so a model that does not answer produces a wrong board.
 */
#define MT6589_CHIPID_BASE      0x08000000

/*
 * Keypad controller. Only the scan-result registers matter to the boot chain:
 * the preloader reads KP_MEM1..5 to decide whether a download/recovery key
 * combination is held. KP_MEM1 is at +0x04 (mtk_key.h), one 16-bit word per
 * 16 scan codes.
 */
#define MT6589_KPD_BASE         0x10015000
#define MT6589_KPD_MEM1         0x0004
#define MT6589_DRAM_BASE        0x80000000

#define MT6589_TOPCKGEN_BASE    0x10000000
#define MT6589_TOPRGU_BASE      0x10000000
#define MT6589_SRAMROM_BASE     0x10002000
#define MT6589_PERI_BASE        0x10003000
#define MT6589_DRAMC_BASE       0x10004000
#define MT6589_DRAMC_SIZE       0x1000

/*
 * The DDR PHY. It mirrors DRAMC's register layout for the delay/tuning block, and
 * the preloader polls it directly (DDRPHY_BASE in the downstream mt6589.h is
 * IO_PHYS + 0x11000). Left unimplemented, every delay read comes back 0 and the
 * calibration's per-tap scan has no passing tap at all.
 */
#define MT6589_DDRPHY_BASE      0x10011000
#define MT6589_DDRPHY_SIZE      0x1000

/* System DMA, datasheet Table 3-1. */
#define MT6589_DMA_BASE         0x11000000
#define MT6589_GPIO_BASE        0x10005000
#define MT6589_SPM_BASE         0x10006000
#define MT6589_GPT_BASE         0x10008000
#define MT6589_EFUSE_BASE       0x10009000
#define MT6589_PWRAP_BASE       0x1000F000
/*
 * SEJ - Security Engine Junior, 0x1000_A000.
 *
 * Datasheet Table 3-1/3-2 lists the window ("4KB SEJ") and has no register
 * chapter. The interface comes from the guest's own code, and the boot ROM and
 * the preloader use the same one at +0x08:
 *
 *   ldr  r4, [r0, #8]      ; r0 = 0x1000A000
 *   orr  r4, r4, #1        ; bit 0 = start
 *   str  r4, [r0, #8]
 *   ldr  r4, [r0, #8]      ; <- poll
 *   tst  r4, #0x902        ; bit 15 = done   <- see the warning below
 *   beq  <poll>
 *
 * The `tst` operand is the trap. 0x902 is not the immediate: ARM encodes a data
 * processing immediate as an 8-bit value rotated right by twice the top four
 * bits, so imm8 = 0x02 with rotate 18 gives 0x00008000, which is bit 15. Reading
 * the raw low twelve bits gives "bit 9", which is wrong. The Renode port recorded
 * this exact mistake costing a round of wrong guesses, and I repeated it here
 * before checking the preloader's copy of the same sequence.
 *
 * So: bit 0 starts, bit 15 means done. The transform itself is NOT known or
 * modelled - see docs/qemu/sej.md - so a block is passed through and logged,
 * which lets the interface complete while making a wrong result visible.
 */
#define MT6589_SEJ_BASE         0x1000A000
#define MT6589_SEJ_STATUS       0x0008
#define MT6589_SEJ_STATUS_START (1u << 0)
#define MT6589_SEJ_STATUS_DONE  (1u << 15)
#define MT6589_MCUSYS_BASE      0x10200000
/*
 * SMI common. NOT 0x10202000: the datasheet gives this block two incompatible
 * addresses in its own memory map (0x12000000 in the top-level table and
 * 0x10202000 in the infrastructure table) and both are wrong. The downstream tree
 * says SMI1_BASE = 0xF000E000 and the kernel dtsi says smi@1000e000, which agree
 * on this. The local arbiters below ARE right in the datasheet, which is what
 * makes the SMI row easy to trust by association - see docs in the emulator tree,
 * docs/memory-map.md.
 */
#define MT6589_SMI_COMMON_BASE  0x1000E000
#define MT6589_M4U_BASE         0x10205000
#define MT6589_GIC_DIST_BASE    0x10211000
#define MT6589_GIC_CPU_BASE     0x10212000

/*
 * SMI local arbiters. These do match the datasheet and the kernel dtsi. Note LARB1
 * is 64 KiB where the others are 4 KiB.
 */
#define MT6589_LARB0_BASE       0x17001000
#define MT6589_LARB1_BASE       0x16010000
#define MT6589_LARB2_BASE       0x14010000
#define MT6589_LARB3_BASE       0x15001000
#define MT6589_LARB4_BASE       0x15002000

#define MT6589_APDMA_BASE       0x11000000
#define MT6589_NFI_BASE         0x11001000
#define MT6589_NFIECC_BASE      0x11002000
#define MT6589_AUXADC_BASE      0x11003000
#define MT6589_UART0_BASE       0x11006000
#define MT6589_UART1_BASE       0x11007000
#define MT6589_UART2_BASE       0x11008000
#define MT6589_UART3_BASE       0x11009000
#define MT6589_I2C0_BASE        0x1100D000
/*
 * MSDC0..MSDC4, 64 KiB each at 0x1123_0000 + n*0x10000 (datasheet p46). The boot
 * ROM probes all five when it looks for a preloader on a card, so only modelling
 * MSDC0 leaves it polling an absent block on the second slot - which is exactly
 * where it stalled.
 */
#define MT6589_MSDC_BASE(n)     (0x11230000 + (n) * 0x10000)
#define MT6589_NUM_MSDC         5
/* MSDC0's IRQ is 78 and they are consecutive in the GIC's SPI map. */
#define MT6589_MSDC_IRQ(n)      (78 + (n))

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
    Mt6589MsdcState *msdc[MT6589_NUM_MSDC];
    Mt6589NfiState *nfi;
    Mt6589ToprguState *toprgu;
    Mt6589EfuseState *efuse;
    Mt6589AuxAdcState *auxadc;
    Mt6589PwrapState *pwrap;
    Mt6589DramcState *dramc;
    Mt6589DramcState *ddrphy;

    /* Card image applied to MSDC0 only, set by the machine from -drive. */
    char *card_image;

    MemoryRegion sram;
    MemoryRegion sramboot;
    MemoryRegion preram;
    MemoryRegion peri_iomem_hi;
    MemoryRegion bootrom;

    /* SEJ. Interface read off the boot ROM (see the constants above); the
     * transform is not known, so a block is passed through and logged. */
    MemoryRegion sej_iomem;
    uint32_t sej_status;
    uint32_t sej_data[0x3c];
    MemoryRegion peri_iomem;
};

#endif /* HW_ARM_MT6589_H */
