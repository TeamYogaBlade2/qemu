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
#include "hw/gpio/mt6589_gpio1_regs.h"

/*
 * The second pin window (0x1020_c000) carries only the registers the extraction lists for it - 25 of
 * them, nine of which also appear in the first window's page at the same offset. Treating it as a full
 * copy of the first is what broke the preloader: measured, the kernel log fell from 3348 lines to 813
 * and the preloader stopped mid-line after "init eMMC success", because storage semantics were being
 * given to addresses that hold something else.
 *
 * This table is that list. Anything outside it reads as zero and ignores writes, which is exactly what
 * the peripheral catch-all did before, so the window behaves as documented and nothing more.
 */
static const uint16_t mt6589_gpio1_offsets[] = {
#define G1(name) MT6589_GPIO1_WIN_##name,
    G1(GPIO_IES_CON7) G1(GPIO_IES_CON8) G1(GPIO_IES_CON9) G1(GPIO_IES_CON10)
    G1(GPIO_PULLEN7) G1(GPIO_PULLEN8) G1(GPIO_PULLEN9) G1(GPIO_PULLEN10)
    G1(GPIO_PULLSEL7) G1(GPIO_PULLSEL8) G1(GPIO_PULLSEL9) G1(GPIO_PULLSEL10)
    G1(GPIO_SMT_CON7) G1(GPIO_SMT_CON8) G1(GPIO_SMT_CON9) G1(GPIO_SMT_CON10)
    G1(GPIO_DRV_CON6) G1(GPIO_DRV_CON7) G1(GPIO_DRV_CON8) G1(GPIO_DRV_CON9)
    G1(GPIO_DSEL_CON6) G1(GPIO_DSEL_CON7)
    G1(GPIO_BIAS_CON2) G1(GPIO_BIAS_CON3)
#undef G1
};

static bool mt6589_gpio_reg_exists(const Mt6589GpioState *s, hwaddr offset)
{
    size_t i;

    if (!s->strict) {
        return true;
    }
    /* Bisecting which documented register changes the preloader's behaviour: MT6589_GPIO1_EMPTY=1
     * makes the window behave exactly as the catch-all did (every read zero, every write dropped). */
    if (getenv("MT6589_GPIO1_EMPTY")) {
        return false;
    }
    for (i = 0; i < ARRAY_SIZE(mt6589_gpio1_offsets); i++) {
        if (mt6589_gpio1_offsets[i] == offset) {
            return true;
        }
    }
    return false;
}
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"
#include "target/arm/cpu.h"
#include "hw/arm/mt6589.h"

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

/*
 * Widths, from the datasheet's register table: the pin banks (DIR, DOUT, DIN, MODE, PULLEN, PULLSEL,
 * SMT_CON, IES_CON, DINV) are 16-bit - sixteen pins to a register and one bit each - while DRV_CON,
 * DSEL_CON, MSDC_R and BIAS_CON0 are 32-bit. The model keeps 32 bits per slot and honours byte and
 * halfword lanes, so a 16-bit register simply has an upper half that stays zero.
 *
 * These registers are not all 32 bits wide: the datasheet gives the pin banks as 16-bit registers and
 * BIAS_CON2 as an 8-bit one, and drivers do byte and halfword accesses to them. A model that accepts
 * only 4-byte accesses turns a byte write at 0x921 into a 4-byte write at 0x920 and corrupts the
 * neighbouring bits, so the lane has to be honoured.
 */
static uint64_t mt6589_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589GpioState *s = MT6589_GPIO(opaque);
    uint32_t v;

    if ((offset >> 2) >= ARRAY_SIZE(s->regs) || !mt6589_gpio_reg_exists(s, offset & ~3u)) {
        return 0;
    }
    if (offset >= GPIO(GPIO_DIN0) && offset < GPIO(GPIO_DIN0) + 15 * 0x10) {
        /*
         * Pin inputs. Nothing is attached to the pads, so the only self-consistent answer is the
         * output latch - and that is also what real hardware reports for a pin the SoC drives.
         * GPIO_DINVn (0x600 + bank) inverts the value the pin reports, one bit per pin, so it is
         * applied here rather than stored in the data registers: a driver that sets DINV and reads
         * DIN back expects the inverted value.
         */
        hwaddr bank = offset - GPIO(GPIO_DIN0);
        uint32_t latched = s->regs[(GPIO(GPIO_DOUT0) + bank) >> 2];
        uint32_t inv = s->regs[(GPIO(GPIO_DINV0) + bank) >> 2];
        uint32_t dir = s->regs[(GPIO(GPIO_DIR0) + bank) >> 2];
        uint32_t pullen = s->regs[(GPIO(GPIO_PULLEN0) + bank) >> 2];
        uint32_t pullsel = s->regs[(GPIO(GPIO_PULLSEL0) + bank) >> 2];

        /*
         * A pin configured as an input reads what the pad sees. Nothing is attached to these pads,
         * so the pull decides: pull-up reads 1, pull-down reads 0, and a floating pin reads the
         * output latch (the only defined value left). This matters concretely - the panel driver
         * identifies the display by reading a strapping pin, and a floating read of 0 makes it
         * decide the panel does not match.
         */
        uint32_t pads = (dir & ~(pullen & pullsel)) |          /* inputs with pull-up -> 1 */
                        (latched & (dir | (pullen & ~pullsel))); /* outputs, and pull-downs -> 0 */

        v = pads ^ inv;
    } else {
        v = s->regs[offset >> 2];
    }
    /* Shift the lane down for sub-word reads. */
    if (size < 4) {
        v >>= 8 * (offset & 3);
        v &= (size == 1) ? 0xffu : 0xffffu;
    }
    if (mt6589_gpio_log_enabled()) {
        /*
         * The guest PC matters more than the value: a driver that polls a pin shows up as the same
         * two registers over and over, and the PC is what turns that into a function name.
         */
        fprintf(stderr, "GPIO%s R %03" HWADDR_PRIx " = %08x pc=%08x lr=%08x task=%s\n", s->win, offset, v,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[15] : 0,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[14] : 0,
                mt6589_guest_task());
    }
    return v;
}

static void mt6589_gpio_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Mt6589GpioState *s = MT6589_GPIO(opaque);

    if ((offset >> 2) >= ARRAY_SIZE(s->regs) || !mt6589_gpio_reg_exists(s, offset & ~3u)) {
        return;
    }
    if (mt6589_gpio_log_enabled()) {
        fprintf(stderr, "GPIO%s W %03" HWADDR_PRIx " = %08x pc=%08x lr=%08x task=%s\n", s->win, offset, (uint32_t)value,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[15] : 0,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[14] : 0,
                mt6589_guest_task());
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
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void mt6589_gpio_realize(DeviceState *dev, Error **errp)
{
    Mt6589GpioState *s = MT6589_GPIO(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_gpio_ops, s,
                          "mt6589-gpio", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static const Property mt6589_gpio_props[] = {
    /* Second pin window: only the registers its datasheet page lists exist there. */
    DEFINE_PROP_BOOL("strict", Mt6589GpioState, strict, false),
    /* Label for the trace, so the two windows can be told apart in a log. */
    DEFINE_PROP_STRING("win", Mt6589GpioState, win),
};

static void mt6589_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mt6589_gpio_realize;
    device_class_set_props(dc, mt6589_gpio_props);
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
