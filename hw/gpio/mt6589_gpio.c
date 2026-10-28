/*
 * MediaTek MT6589 GPIO controller.
 *
 * Why this exists: the touch panel's reset sequence is a set of pin operations - EINT pin to EINT
 * mode, reset pin to output low, delay, output high - and the driver reads the pin state back
 * between them. With no GPIO model those accesses landed in the peripheral catch-all, which drops
 * writes and reads zeroes, so a driver that checks what it just wrote sees something else.
 *
 * What is modelled: every register group in the datasheet extraction is plain read/write storage,
 * which is what a pin configuration register is. GPIO_DIN returns the pins' input state, and with
 * nothing connected to the pads the only self-consistent answer is the output latch: a pin the SoC
 * drives reads back as what it drives, which is also what the hardware does for an output pin.
 *
 * Not modelled: pad electrical behaviour, external devices pulling a pin, EINT (that is a separate
 * block at 0x1101_7000), and the SIM/BIAS special registers beyond storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/gpio/mt6589_gpio.h"
#include "hw/arm/mt6589-regs/mt6589-gpio-regs.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "target/arm/cpu.h"

#define GPIO(name) (MT6589_GPIO_##name - MT6589_GPIO_BASE)

static bool mt6589_gpio_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("MT6589_GPIO_LOG");

        enabled = e && *e && strcmp(e, "0") != 0;
    }
    return enabled;
}

static uint64_t mt6589_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589GpioState *s = MT6589_GPIO(opaque);
    uint32_t v;

    if (offset >= sizeof(s->regs)) {
        return 0;
    }
    if (offset >= GPIO(GPIO_DIN0) && offset < GPIO(GPIO_DIN0) + 15 * 0x10) {
        /*
         * Pin inputs. Nothing is attached to the pads, so the only self-consistent answer is the
         * output latch - and that is also what real hardware reports for a pin the SoC drives.
         */
        v = s->regs[GPIO(GPIO_DOUT0) + (offset - GPIO(GPIO_DIN0)) >> 2];
    } else {
        v = s->regs[offset >> 2];
    }
    if (mt6589_gpio_log_enabled()) {
        /*
         * The guest PC matters more than the value: a driver that polls a pin shows up as the same
         * two registers over and over, and the PC is what turns that into a function name.
         */
        fprintf(stderr, "GPIO R %03" HWADDR_PRIx " = %08x pc=%08x\n", offset, v,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[15] : 0);
    }
    return v;
}

static void mt6589_gpio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Mt6589GpioState *s = MT6589_GPIO(opaque);

    if (offset >= sizeof(s->regs)) {
        return;
    }
    if (mt6589_gpio_log_enabled()) {
        fprintf(stderr, "GPIO W %03" HWADDR_PRIx " = %08x pc=%08x\n", offset, (uint32_t)value,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[15] : 0);
    }
    if (offset >= GPIO(GPIO_DIN0) && offset < GPIO(GPIO_DIN0) + 15 * 0x10) {
        return;                 /* read-only */
    }
    s->regs[offset >> 2] = value;
}

static const MemoryRegionOps mt6589_gpio_ops = {
    .read = mt6589_gpio_read,
    .write = mt6589_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_gpio_realize(DeviceState *dev, Error **errp)
{
    Mt6589GpioState *s = MT6589_GPIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_gpio_ops, s,
                          "mt6589-gpio", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void mt6589_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mt6589_gpio_realize;
    dc->desc = "MediaTek MT6589 GPIO controller";
}

static const TypeInfo mt6589_gpio_types[] = {
    {
        .name = TYPE_MT6589_GPIO,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589GpioState),
        .class_init = mt6589_gpio_class_init,
    },
};

DEFINE_TYPES(mt6589_gpio_types)
