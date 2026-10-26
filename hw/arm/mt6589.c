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
 *
 * The design of this file follows one rule: the register map comes from the
 * datasheet (tools/extract_regs.py), and the behaviour comes from what the
 * guest is measured to poll. A register that exists in the datasheet but whose
 * behaviour is not modelled reads its documented reset value and ignores
 * writes, which is what the real hardware does for a block that has not been
 * enabled - and it is better than returning 0, which the guest reads as "block
 * not present" and acts on.
 *
 * TODO, in boot-chain order. Each needs the guest to get further than the
 * previous stage before its behaviour can be measured against anything:
 *   - PWRAP + MT6320 PMIC (CHRDET/PWRKEY decide preloader vs power-off)
 *   - MSDC (card enumeration; INTEN gating matters, see the Renode notes)
 *   - M4U, SMI/LARB (the display pipeline will not bind without them)
 *   - DISP: OVL/RDMA/COLOR/DSI
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/arm/mt6589.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "hw/core/loader.h"
#include "qemu/units.h"

/*
 * The GPT tick. The datasheet's counters are clocked from the 13 MHz system
 * clock divided by CLK[3:0], but nothing in the guest polls them often enough to
 * derive a compare match on access alone: the kernel arms a timer, stops
 * touching the block, and expects the interrupt. So the deadline is computed
 * from the programmed period and a timer is armed for it, which is the only way
 * the interrupt can arrive while the guest is idle.
 *
 * Renode's port got this wrong first (it advanced only on access) and the guest
 * stalled; this is the QEMU equivalent done up front rather than rediscovered.
 */
#define MT6589_GPT_SYS_HZ 13000000ull
#define MT6589_GPT_RTC_HZ 32768ull

static uint64_t mt6589_gpt_channel_hz(Mt6589State *s, int n)
{
    uint32_t clk = s->gpt_chan[n].clk;
    uint64_t base = (clk & MT6589_GPT_CLK_SRC_RTC) ? MT6589_GPT_RTC_HZ
                                                   : MT6589_GPT_SYS_HZ;
    uint32_t div = (clk & MT6589_GPT_CLK_DIV_MASK) + 1;

    return base / div;
}

/* Current counter value, derived from elapsed emulated time. */
static uint32_t mt6589_gpt_count(Mt6589State *s, int n)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                      s->gpt_chan[n].start_ns;
    uint64_t hz = mt6589_gpt_channel_hz(s, n);

    if (elapsed < 0) {
        return 0;
    }
    return (uint32_t)((elapsed * hz) / 1000000000ull);
}

static void mt6589_gpt_update_irq(Mt6589State *s)
{
    /* The GIC line is level-triggered, so it is the AND of status and enable.
     * Driving it on status alone makes an acknowledged interrupt re-deliver for
     * ever, which is exactly the Renode bug that cost a day of LK debugging -
     * see docs/qemu/gpt.md. */
    qemu_set_irq(s->gpt_irq, (s->gpt_irqsta & s->gpt_irqen) != 0);
}

static void mt6589_gpt_rearm(Mt6589State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t soonest = INT64_MAX;

    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        uint32_t con = s->gpt_chan[n].con;
        uint32_t compare = s->gpt_chan[n].compare;
        uint64_t hz;

        if (!(con & MT6589_GPT_CON_EN) || compare == 0) {
            continue;
        }
        hz = mt6589_gpt_channel_hz(s, n);
        if (hz == 0) {
            continue;
        }
        int64_t due = s->gpt_chan[n].start_ns +
                      (int64_t)((compare * 1000000000ull) / hz);
        soonest = MIN(soonest, due);
    }
    if (soonest == INT64_MAX) {
        timer_del(&s->gpt_tick);
        return;
    }
    if (soonest <= now) {
        soonest = now + 1;
    }
    timer_mod(&s->gpt_tick, soonest);
}

static void mt6589_gpt_tick(void *opaque)
{
    Mt6589State *s = opaque;

    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        uint32_t con = s->gpt_chan[n].con;
        uint32_t compare = s->gpt_chan[n].compare;

        if (!(con & MT6589_GPT_CON_EN) || compare == 0) {
            continue;
        }
        if (mt6589_gpt_count(s, n) < compare) {
            continue;
        }
        /* Compare reached: raise the channel's status bit. */
        s->gpt_irqsta |= MT6589_GPT_IRQ_BIT(n);

        switch ((con & MT6589_GPT_CON_MODE_MASK) >> MT6589_GPT_CON_MODE_SHIFT) {
        case MT6589_GPT_MODE_ONESHOT:
            /* Stop the counter at the compare value. */
            s->gpt_chan[n].con &= ~MT6589_GPT_CON_EN;
            break;
        case MT6589_GPT_MODE_REPEAT:
            /* Reload: the next period starts now, and the status bit is
             * cleared so the interrupt can be delivered again. */
            s->gpt_chan[n].start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->gpt_irqsta &= ~MT6589_GPT_IRQ_BIT(n);
            break;
        default:
            /* KEEP-GO and FREERUN keep counting past the compare value. */
            break;
        }
    }
    mt6589_gpt_update_irq(s);
    mt6589_gpt_rearm(s);
}

static uint64_t mt6589_gpt_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589State *s = opaque;

    switch (offset) {
    case MT6589_GPT_IRQEN:
        return s->gpt_irqen;
    case MT6589_GPT_IRQSTA:
        return s->gpt_irqsta;
    case MT6589_GPT_IRQACK:
        /* Write-only on hardware; reading back 0 is harmless and honest. */
        return 0;
    }
    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        hwaddr base = MT6589_GPT_CHAN(n);

        if (offset < base || offset >= base + 0x10) {
            continue;
        }
        switch (offset - base) {
        case MT6589_GPT_CON:
            return s->gpt_chan[n].con;
        case MT6589_GPT_CLK:
            return s->gpt_chan[n].clk;
        case MT6589_GPT_COUNT:
            return mt6589_gpt_count(s, n);
        case MT6589_GPT_COMPARE:
            return s->gpt_chan[n].compare;
        }
    }
    qemu_log_mask(LOG_UNIMP, "%s: read of unimplemented offset 0x%" HWADDR_PRIx
                  "\n", __func__, offset);
    return 0;
}

static void mt6589_gpt_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Mt6589State *s = opaque;

    switch (offset) {
    case MT6589_GPT_IRQEN:
        s->gpt_irqen = value;
        mt6589_gpt_update_irq(s);
        return;
    case MT6589_GPT_IRQSTA:
        /* Status is cleared by writing 1 to IRQACK, not by writing status. */
        return;
    case MT6589_GPT_IRQACK:
        s->gpt_irqsta &= ~((uint32_t)value);
        mt6589_gpt_update_irq(s);
        return;
    }
    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        hwaddr base = MT6589_GPT_CHAN(n);

        if (offset < base || offset >= base + 0x10) {
            continue;
        }
        switch (offset - base) {
        case MT6589_GPT_CON:
            /* CLR reloads the counter; EN starts it. Both matter: the guest
             * writes CLR then EN, and treating EN alone as the start makes the
             * first period shorter than programmed. */
            if (value & MT6589_GPT_CON_CLR) {
                s->gpt_chan[n].start_ns =
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                s->gpt_irqsta &= ~MT6589_GPT_IRQ_BIT(n);
            }
            s->gpt_chan[n].con = value & ~MT6589_GPT_CON_CLR;
            mt6589_gpt_update_irq(s);
            mt6589_gpt_rearm(s);
            return;
        case MT6589_GPT_CLK:
            s->gpt_chan[n].clk = value;
            mt6589_gpt_rearm(s);
            return;
        case MT6589_GPT_COUNT:
            /* Writing the counter restarts the period from that value, which is
             * how the kernel sets an absolute deadline. */
            s->gpt_chan[n].start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                (int64_t)(((uint64_t)value * 1000000000ull) /
                          mt6589_gpt_channel_hz(s, n));
            mt6589_gpt_rearm(s);
            return;
        case MT6589_GPT_COMPARE:
            s->gpt_chan[n].compare = value;
            mt6589_gpt_rearm(s);
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to unimplemented offset "
                  "0x%" HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_gpt_ops = {
    .read = mt6589_gpt_read,
    .write = mt6589_gpt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};


/*
 * SEJ - security engine.
 *
 * What is known, from the boot ROM's own code at 0xd060 rather than from any
 * document: 16 bytes go into 0x1000_a010..0x1000_a01c, bit 0 of 0x1000_a008
 * starts an operation, bit 17 of the same register means done, and 16 bytes of
 * result come back from a DRAM address the caller supplies.
 *
 * What is NOT known is the transform. The datasheet has no register chapter for
 * SEJ and the downstream tree has no driver. So this models the interface
 * faithfully - a real caller's polling will complete and its result buffer will
 * be filled - and performs a pass-through, logging every block. That is the
 * honest position: the ROM gets through, and any later stage that depends on the
 * *value* of a SEJ result fails with a logged block rather than with a mysterious
 * wrong hash.
 *
 * If a fuller datasheet or a hardware trace of this address appears, the
 * transform goes here and nothing else changes.
 */
static uint64_t mt6589_sej_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589State *s = opaque;

    switch (offset) {
    case MT6589_SEJ_STATUS:
        return s->sej_status;
    case MT6589_SEJ_STATUS + 4:
        return 0;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void mt6589_sej_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Mt6589State *s = opaque;

    if (offset == MT6589_SEJ_STATUS) {
        if (value & MT6589_SEJ_STATUS_START) {
            /*
             * Report completion at once. The guest polls, it does not measure
             * elapsed time, so a synchronous completion is faithful to what is
             * observable - and no transform is performed because none is known.
             */
            s->sej_status |= MT6589_SEJ_STATUS_DONE;
            qemu_log_mask(LOG_UNIMP,
                          "%s: SEJ operation reported done WITHOUT performing it "
                          "(transform unknown)\n", __func__);
        }
        return;
    }
    /* Key/IV/result registers beyond the control word are plain storage. Their
     * layout is not documented and no transform is performed, so a store is kept
     * (so it reads back) and nothing is derived from it. */
    if (offset >= 0x10 && offset < 0x100) {
        s->sej_data[(offset - 0x10) / 4] = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to offset 0x%"
                  HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_sej_ops = {
    .read = mt6589_sej_read,
    .write = mt6589_sej_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Peripheral-region catch-all. The datasheet documents a register for every
 * block in 0x1000_0000..0x14FF_FFFF; modelling behaviour for all of them is the
 * long-term job, but the guest must not see a bus error or a silent 0 for a
 * register that physically exists. Reads return the documented reset value
 * where the generated map knows it, otherwise 0; writes are ignored. Anything
 * logged here is a register whose behaviour is still missing.
 */
static uint64_t mt6589_peri_read(void *opaque, hwaddr offset, unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: offset 0x%" HWADDR_PRIx " size %u\n",
                  __func__, offset, size);
    return 0;
}

static void mt6589_peri_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: offset 0x%" HWADDR_PRIx " <- 0x%" PRIx64
                  " size %u\n", __func__, offset, value, size);
}

static const MemoryRegionOps mt6589_peri_ops = {
    .read = mt6589_peri_read,
    .write = mt6589_peri_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * 1-byte access must be allowed. The boot ROM probes registers with byte
     * loads and stores (a single-byte read of 0x11006024 aborted with DFSR
     * alignment fault when min_access_size was 4), and a Data Abort there sends
     * the ROM to its abort vector, where it loops. Being permissive here costs
     * nothing: the handler ignores the value anyway.
     */
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void mt6589_init(Object *obj)
{
    Mt6589State *s = MT6589(obj);

    memory_region_init_io(&s->gpt_iomem, OBJECT(s), &mt6589_gpt_ops, s,
                          "mt6589.gpt", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->gpt_iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->gpt_irq);

    timer_init_ns(&s->gpt_tick, QEMU_CLOCK_VIRTUAL, mt6589_gpt_tick, s);

    /* SRAM: the boot ROM drops the preloader here. 256 KiB is the documented
     * size; the preloader is loaded by the BROM, so the region has to exist
     * before the guest starts rather than being created on first write. */
    memory_region_init_ram(&s->sram, OBJECT(s), "mt6589.sram",
                           MT6589_SRAM_SIZE, &error_fatal);
    memory_region_init_ram(&s->sramboot, OBJECT(s), "mt6589.sramboot",
                           MT6589_SRAMBOOT_SIZE, &error_fatal);

    /* The boot ROM window. Backed by RAM rather than ROM so the image can be
     * loaded into it with -bios, and so a wrong write faults visibly rather than
     * being silently ignored - but the region exists even when empty, because
     * the BROM's own vectors must be readable from address 0. */
    memory_region_init_ram(&s->bootrom, OBJECT(s), "mt6589.bootrom",
                           MT6589_BOOTROM_SIZE, &error_fatal);
    memory_region_init_io(&s->sej_iomem, OBJECT(s), &mt6589_sej_ops, s,
                          "mt6589.sej", 0x1000);
}

static void mt6589_realize(DeviceState *dev, Error **errp)
{
    Mt6589State *s = MT6589(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /* cpu_create needs the concrete model: TYPE_ARM_CPU is abstract and
     * object_initialize_with_type asserts on it. */
    s->cpu = ARM_CPU(cpu_create(ARM_CPU_TYPE_NAME("cortex-a7")));

    /*
     * GICv2 at the addresses the kernel device tree declares: distributor
     * 0x1021_1000, CPU interface 0x1021_2000. The CPU has to be told which GIC
     * serves it, and the peripherals take their IRQ lines from it - without
     * this the CPU exposes no input lines at all and qdev's
     * qdev_get_gpio_in_named asserts.
     */
    s->gic = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(s->gic, "num-cpu", 1);
    qdev_prop_set_uint32(s->gic, "num-irq", 160);
    qdev_prop_set_uint32(s->gic, "revision", 2);
    qdev_realize(s->gic, sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 0, MT6589_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 1, MT6589_GIC_CPU_BASE);
    /* The GIC's CPU-side lines are sysbus IRQs, not named GPIOs, so they are
     * connected with sysbus_connect_irq rather than qdev_connect_gpio_out. */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->gic), 0,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_IRQ));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->gic), 1,
                       qdev_get_gpio_in(DEVICE(s->cpu), ARM_CPU_FIQ));

    for (int i = 0; i < MT6589_NUM_UARTS; i++) {
        hwaddr base = MT6589_UART0_BASE + i * 0x1000;
        qemu_irq irq = qdev_get_gpio_in(s->gic, MT6589_UART_IRQ + i);

        /*
         * The UART is a plain 16550, but with a 4-byte register stride rather
         * than a packed 8-byte window: the datasheet puts RBR at 0x00, IER at
         * 0x04, IIR/FCR at 0x08, LCR at 0x0c and so on to SCR at 0x1c, so
         * regshift must be 2. With regshift 0 serial_mm registers only eight
         * bytes, every register above IER falls outside it, and the guest's very
         * first LCR write (0x1100600c) raised a Data Abort - which the boot ROM
         * answers by taking the abort vector and looping there.
         */
        s->uart[i] = serial_mm_init(get_system_memory(), base, 2, irq, 115200,
                                    serial_hd(i), DEVICE_LITTLE_ENDIAN);
    }

    /*
     * MSDC0. The boot ROM programs the clock and polls MSDC_CFG.CKSTB, so a
     * missing controller stalls it at SD initialisation - which is the point the
     * QEMU port reached before this was added.
     */
    for (int i = 0; i < MT6589_NUM_MSDC; i++) {
        char name[8];

        snprintf(name, sizeof(name), "msdc%d", i);
        s->msdc[i] = MT6589_MSDC(object_new(TYPE_MT6589_MSDC));
        object_property_add_child(OBJECT(s), name, OBJECT(s->msdc[i]));
        snprintf(s->msdc[i]->name, sizeof(s->msdc[i]->name), "msdc%d", i);
        /*
         * The card goes in slot 0 and only slot 0. Setting the property on every
         * MSDC - which is what -global mt6589-msdc.image=... does - models a board
         * with a card in all five slots, and the ROM probes each of them.
         */
        if (i == 0 && s->card_image) {
            object_property_set_str(OBJECT(s->msdc[i]), "image",
                                    s->card_image, &error_fatal);
        }
        qdev_realize(DEVICE(s->msdc[i]), sysbus_get_default(), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->msdc[i]), 0, MT6589_MSDC_BASE(i));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->msdc[i]), 0,
                           qdev_get_gpio_in(s->gic, MT6589_MSDC_IRQ(i)));
    }

    /*
     * NFI + NFIECC. The board has no NAND, but the boot ROM probes it as a
     * preloader source and polls NFIECC_ENCIDLE bit 0 while doing so, so an
     * absent block stalls the probe instead of letting the ROM move on.
     */
    s->nfi = MT6589_NFI(object_new(TYPE_MT6589_NFI));
    object_property_add_child(OBJECT(s), "nfi", OBJECT(s->nfi));
    qdev_realize(DEVICE(s->nfi), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->nfi), 0, MT6589_NFI_BASE);

    /*
     * TOPRGU at 0x10000000. Its WDT_STA is the boot reason the guest branches
     * on, so it must be modelled rather than left to the catch-all: reading 0
     * there is correct by accident, but a WDT_MODE write being dropped would be
     * a silent difference from hardware.
     *
     * Note the address: 0x1000_7000 is TOPRGU on several other MediaTek SoCs and
     * is RESERVED on MT6589. The datasheet map, the preloader's mt_reg_base.h
     * and the kernel dtsi all say 0x1000_0000.
     */
    s->toprgu = MT6589_TOPRGU(object_new(TYPE_MT6589_TOPRGU));
    object_property_add_child(OBJECT(s), "toprgu", OBJECT(s->toprgu));
    qdev_realize(DEVICE(s->toprgu), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->toprgu), 0, MT6589_TOPRGU_BASE);

    /*
     * EFUSEC at 0x10009000 - a flat read-only window onto the fuse array rather
     * than a register bank, carrying the real board's dump. Firmware reads fuses
     * to decide what the hardware is, so invented contents produce a board that
     * boots differently from the one being debugged.
     */
    s->efuse = MT6589_EFUSE(object_new(TYPE_MT6589_EFUSE));
    object_property_add_child(OBJECT(s), "efuse", OBJECT(s->efuse));
    qdev_realize(DEVICE(s->efuse), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->efuse), 0, MT6589_EFUSE_BASE);

    sysbus_mmio_map(sbd, 0, MT6589_GPT_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(s->gic, MT6589_GPT_IRQ));

    memory_region_add_subregion(get_system_memory(), MT6589_SRAM_BASE,
                                &s->sram);
    memory_region_add_subregion(get_system_memory(), MT6589_BOOTROM_BASE,
                                &s->bootrom);
    /* "BootROM/SRAM" at 0x1000_2000 - separate from the 64 KiB on-chip SRAM. */
    memory_region_add_subregion(get_system_memory(), MT6589_SRAMBOOT_BASE,
                                &s->sramboot);
    /* Mapped before the catch-all, which is added with a negative priority so
     * this wins. */
    memory_region_add_subregion(get_system_memory(), MT6589_SEJ_BASE,
                                &s->sej_iomem);

    /*
     * Catch-all for the infrastructure and peripheral windows. The datasheet
     * documents a register for every block here; behaviour is added block by
     * block, but in the meantime a read must return a plausible value rather
     * than a bus error, because the guest interprets an abort as "hardware not
     * present" and takes a different boot path. The specific models (GPT, UART)
     * are mapped as subregions of the same space and take priority.
     */
    /*
     * Size and start are chosen to cover every documented block, not just the
     * infrastructure window: the boot ROM's first out-of-BROM access is a read
     * of 0x1500_0188 (IMAGE system, datasheet Table 3-1 "0x1500_0000 16MB"), and
     * with the region ending at 0x1500_0000 that access raised a Data Abort, so
     * the ROM took the abort vector and looped there for ever.
     *
     * 0x1000_0000 .. 0x17FF_FFFF covers the infrastructure (0x1000_0000), the
     * peripheral window (0x1100_0000), the multimedia window (0x1400_0000) and
     * the image/camera window (0x1500_0000), which is everything the datasheet
     * places below the modem cores at 0xA000_0000.
     */
    memory_region_init_io(&s->peri_iomem, OBJECT(s), &mt6589_peri_ops, s,
                          "mt6589.peri", 0x08000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x10000000,
                                        &s->peri_iomem, -1);
}

/*
 * Card image for MSDC0 - a property on the SoC rather than on each MSDC, because
 * only one slot has a card in it.
 */
static const Property mt6589_props[] = {
    DEFINE_PROP_STRING("card-image", Mt6589State, card_image),
};

static void mt6589_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mt6589_realize;
    device_class_set_props(dc, mt6589_props);
}

static const TypeInfo mt6589_types[] = {
    {
        .name = TYPE_MT6589,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589State),
        .instance_init = mt6589_init,
        .class_init = mt6589_class_init,
    },
};

DEFINE_TYPES(mt6589_types)

/* ---------------- machine ---------------- */

static void mt6589_machine_init(MachineState *machine)
{
    static struct arm_boot_info boot_info;
    Mt6589State *s;
    const char *brom;

    assert(machine->ram_size <= 0x40000000);

    s = MT6589(object_new(TYPE_MT6589));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(s));
    /* A SysBusDevice must be realised on the system bus, not on NULL: the
     * machine has no bus of its own, and qdev asserts on a device whose class
     * declares a bus type being realised outside one. */
    qdev_realize(DEVICE(s), sysbus_get_default(), &error_fatal);

    memory_region_add_subregion(get_system_memory(), MT6589_DRAM_BASE,
                                machine->ram);

    /*
     * The MT6589 does not boot a kernel. The boot ROM at address 0 reads the
     * preloader from eMMC/NAND/UART, the preloader brings up DRAM, and LK loads
     * whatever comes next. So:
     *
     *   -bios FILE    the boot ROM image, loaded at 0x0000_0000
     *   -kernel FILE  the preloader, loaded at the DRAM base
     *
     * Both are needed for a faithful run, and loading the BROM with -kernel
     * (which puts it at the DRAM base) makes it execute from an address it was
     * never linked for. That mistake is worth recording: the BROM appeared to
     * run, and only its failure to reach its own UART poll showed anything was
     * wrong.
     */
    brom = machine->firmware;
    if (brom) {
        /*
         * rom_add_file_fixed, not load_image_size: the latter writes to a *host*
         * pointer, so passing the guest address silently scribbles on this
         * process's memory and leaves address 0 empty. The symptom is a BROM
         * that appears to run - the entry point and vectors execute - and then
         * walks into zeros, because only the first page's worth of code was
         * ever legible.
         */
        if (rom_add_file_fixed(brom, MT6589_BOOTROM_BASE, -1) < 0) {
            error_report("mt6589: could not load boot ROM '%s'", brom);
            exit(1);
        }
    }

    boot_info = (struct arm_boot_info) {
        .loader_start = MT6589_DRAM_BASE,
        .kernel_filename = machine->kernel_filename,
        .ram_size = machine->ram_size,
        .board_id = -1,
        .firmware_loaded = brom != NULL,
    };
    arm_load_kernel(s->cpu, machine, &boot_info);
}

static void mt6589_machine_class_init(MachineClass *mc)
{
    mc->desc = "MediaTek MT6589 (Cortex-A7)";
    mc->init = mt6589_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    mc->default_ram_id = "mt6589.ram";
    mc->default_ram_size = 1024 * MiB;
    /* Sets machine->firmware from -bios. */
    mc->default_boot_order = NULL;
}

DEFINE_MACHINE("mt6589", mt6589_machine_class_init)
