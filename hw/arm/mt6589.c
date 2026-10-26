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
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/arm/mt6589.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
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
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
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
    memory_region_init_ram(&s->sram, OBJECT(s), "mt6589.sram", 0x40000,
                           &error_fatal);
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

        /* The UART is a plain 16550 at 0x1000 stride, which the datasheet's
         * chapter 64 confirms; no MTK-specific register is documented. */
        s->uart[i] = serial_mm_init(get_system_memory(), base,
                                    MT6589_UART_REG_SHIFT, irq, 115200,
                                    serial_hd(i), DEVICE_LITTLE_ENDIAN);
    }

    sysbus_mmio_map(sbd, 0, MT6589_GPT_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(s->gic, MT6589_GPT_IRQ));

    memory_region_add_subregion(get_system_memory(), MT6589_SRAM_BASE,
                                &s->sram);

    /*
     * Catch-all for the infrastructure and peripheral windows. The datasheet
     * documents a register for every block here; behaviour is added block by
     * block, but in the meantime a read must return a plausible value rather
     * than a bus error, because the guest interprets an abort as "hardware not
     * present" and takes a different boot path. The specific models (GPT, UART)
     * are mapped as subregions of the same space and take priority.
     */
    memory_region_init_io(&s->peri_iomem, OBJECT(s), &mt6589_peri_ops, s,
                          "mt6589.peri", 0x05000000);
    memory_region_add_subregion_overlap(get_system_memory(), 0x10000000,
                                        &s->peri_iomem, -1);
}

static void mt6589_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mt6589_realize;
    /* no device properties yet */
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
    int i;

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
     * The MT6589 has no U-Boot or Fastboot contract; the boot ROM reads the
     * preloader from eMMC/NAND/UART and the chain continues from there. QEMU's
     * -kernel on this board therefore means "load this as the first stage at
     * the boot ROM's drop address" rather than a Linux zImage directly, and the
     * image is expected to include whatever second stage it wants.
     */
    boot_info = (struct arm_boot_info) {
        .loader_start = MT6589_DRAM_BASE,
        .kernel_filename = machine->kernel_filename,
        .ram_size = machine->ram_size,
        .board_id = -1,
    };
    arm_load_kernel(s->cpu, machine, &boot_info);

    for (i = 0; i < MT6589_NUM_UARTS; i++) {
        /* nothing: serial_mm_init already wired the chardev */
    }
}

static void mt6589_machine_class_init(MachineClass *mc)
{
    mc->desc = "MediaTek MT6589 (Cortex-A7)";
    mc->init = mt6589_machine_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    mc->default_ram_id = "mt6589.ram";
    mc->default_ram_size = 1024 * MiB;
}

DEFINE_MACHINE("mt6589", mt6589_machine_class_init)
