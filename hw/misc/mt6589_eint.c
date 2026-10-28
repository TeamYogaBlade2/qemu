/*
 * MediaTek MT6589 external interrupt controller (EINT).
 *
 * Why this exists: on this SoC most peripherals do not interrupt the GIC directly - the pin-based
 * ones (the PMIC's interrupt output, the touch panel, the headset detect, the hall sensor) land on
 * EINT lines, and the driver programs level/edge, polarity, mask and debounce per line before it
 * unmasks. Nothing else in the model can deliver those interrupts, so without this block no such
 * device can ever work, however well its own registers are modelled.
 *
 * Where the facts come from: base, GIC line and register grouping are cross-checked between the
 * datasheet extraction (mt6589-eint-regs.h) and the device tree - mt6589.dtsi gives the pinctrl
 * three windows (gpio 0x1000_5000, gpio1 0x1020_c000, eint 0x1000_b000) and
 * "interrupts = <GIC_SPI 116 IRQ_TYPE_LEVEL_HIGH>", i.e. GIC line 148. The downstream's
 * cust_eint.h agrees on the users: the PMIC is EINT 3, level sensitive, high polarity (mainline's
 * mt6320.dtsi says the same thing as <3 IRQ_TYPE_LEVEL_HIGH> on the pio).
 *
 * What is modelled: 192 lines in six 32-bit registers, mask/sense/polarity per line, ACK, software
 * interrupts, and the PMIC_EINT passthrough group. Lines are driven through
 * mt6589_eint_set_line(); nothing drives them yet, which is the next step (the PMIC's interrupt
 * output is EINT 3).
 *
 * Not modelled: debounce timing (the registers store what is written, and no delay is applied),
 * domain gating (D0EN/D1EN/D2EN are storage), and the second pin bank's routing.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/mt6589_eint.h"
#include "hw/arm/mt6589-regs/mt6589-eint-regs.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/memory.h"
#include "target/arm/cpu.h"

#define EINT(name) (MT6589_EINT_##name - MT6589_EINT_BASE)

/* Six registers of 32 lines each. */
#define EINT_REGS   6
#define EINT_LINES  (EINT_REGS * 32)

#define STA(n)   (EINT(EINT_STA0) + (n) * 4)
#define ACK(n)   (EINT(EINT_ACK0) + (n) * 4)
#define MASK(n)  (EINT(EINT_MASK0) + (n) * 4)
#define SENS(n)  (EINT(EINT_SENS0) + (n) * 4)
#define POL(n)   (EINT(EINT_POL0) + (n) * 4)
#define SOFT(n)  (EINT(EINT_SOFT0) + (n) * 4)

static bool mt6589_eint_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("MT6589_EINT_LOG");

        enabled = e && *e && strcmp(e, "0") != 0;
    }
    return enabled;
}

/* A line is pending when its status bit is set; a masked line does not reach the GIC. */
static void mt6589_eint_update_irq(Mt6589EintState *s)
{
    uint32_t pending = 0;
    int i;

    for (i = 0; i < EINT_REGS; i++) {
        pending |= s->regs[STA(i) >> 2] & ~s->regs[MASK(i) >> 2];
    }
    pending |= s->pmic_sta & ~s->pmic_mask;
    qemu_set_irq(s->irq, pending != 0);
}

/*
 * A level-sensitive line is continuously evaluated: a line that is already asserted latches as soon
 * as the driver sets its polarity and unmasks it, which is what a device that held its interrupt
 * line before the handler existed does on real hardware.
 */
static void mt6589_eint_eval_line(Mt6589EintState *s, unsigned line)
{
    unsigned reg = line / 32;
    uint32_t bit = 1u << (line % 32);
    bool asserted = (s->line_asserted[reg] & bit) != 0;
    bool sens = (s->regs[SENS(reg) >> 2] & bit) != 0;

    if (!sens && asserted) {
        s->regs[STA(reg) >> 2] |= bit;
    }
}

static void mt6589_eint_eval_all(Mt6589EintState *s)
{
    unsigned line;

    for (line = 0; line < EINT_LINES; line++) {
        mt6589_eint_eval_line(s, line);
    }
    mt6589_eint_update_irq(s);
}

void mt6589_eint_set_line(Mt6589EintState *s, unsigned line, bool asserted)
{
    unsigned reg, bit;
    bool old, sens;

    if (line >= EINT_LINES) {
        return;
    }
    reg = line / 32;
    bit = 1u << (line % 32);
    old = (s->line_asserted[reg] & bit) != 0;
    s->line_asserted[reg] = asserted ? (s->line_asserted[reg] | bit)
                                     : (s->line_asserted[reg] & ~bit);

    /*
     * The model has no electrical layer, so this API carries the logical state and polarity is
     * stored but not applied. Getting that wrong is not theoretical: treating pol=0 as
     * "active low" made every undriven line read as asserted, and the first boot with this block
     * produced a storm of SD-card insert/remove interrupts from EINT lines nothing was driving.
     *
     * Edge-sensitive lines latch on the transition, level-sensitive ones for as long as the device
     * holds the line.
     */
    sens = (s->regs[SENS(reg) >> 2] & bit) != 0;
    if (sens) {
        if (asserted && !old) {
            s->regs[STA(reg) >> 2] |= bit;
        }
    } else if (asserted) {
        s->regs[STA(reg) >> 2] |= bit;
    }

    if (mt6589_eint_log_enabled()) {
        fprintf(stderr, "EINT line %u asserted=%u (sens=%u sta=%08x mask=%08x)\n",
                line, asserted, sens, s->regs[STA(reg) >> 2], s->regs[MASK(reg) >> 2]);
    }
    mt6589_eint_update_irq(s);
}

/* Raise one of the PMIC's own interrupt bits (the passthrough group at 0xa00). */
void mt6589_eint_raise_pmic(Mt6589EintState *s, unsigned bit)
{
    s->pmic_sta |= 1u << bit;
    if (mt6589_eint_log_enabled()) {
        fprintf(stderr, "EINT pmic event %u, sta=%08x mask=%08x\n",
                bit, s->pmic_sta, s->pmic_mask);
    }
    mt6589_eint_update_irq(s);
}

static uint64_t mt6589_eint_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589EintState *s = MT6589_EINT(opaque);
    uint32_t v;

    if (offset == EINT(PMIC_EINT_STA)) {
        v = s->pmic_sta;
    } else if (offset == EINT(PMIC_EINT_MASK)) {
        v = s->pmic_mask;
    } else if (offset >= sizeof(s->regs)) {
        return 0;
    } else {
        v = s->regs[offset >> 2];
    }
    if (mt6589_eint_log_enabled()) {
        fprintf(stderr, "EINT R %03" HWADDR_PRIx " = %08x pc=%08x\n", offset, v,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[15] : 0);
    }
    return v;
}

static void mt6589_eint_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Mt6589EintState *s = MT6589_EINT(opaque);
    uint32_t v = value;
    int i;

    if (mt6589_eint_log_enabled()) {
        fprintf(stderr, "EINT W %03" HWADDR_PRIx " = %08x pc=%08x\n", offset, v,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[15] : 0);
    }

    if (offset == EINT(PMIC_EINT_ACK)) {
        s->pmic_sta &= ~v;
        mt6589_eint_update_irq(s);
        return;
    }
    if (offset == EINT(PMIC_EINT_MASK) || offset == EINT(PMIC_EINT_MASK_SET)) {
        s->pmic_mask |= v;
        mt6589_eint_update_irq(s);
        return;
    }
    if (offset == EINT(PMIC_EINT_MASK_CLR)) {
        s->pmic_mask &= ~v;
        mt6589_eint_update_irq(s);
        return;
    }

    /* Per-line registers: ACK clears, the _SET/_CLR pairs adjust one bitfield group. */
    for (i = 0; i < EINT_REGS; i++) {
        if (offset == ACK(i)) {
            s->regs[STA(i) >> 2] &= ~v;
            mt6589_eint_update_irq(s);
            return;
        }
        if (offset == MASK(i)) {
            s->regs[MASK(i) >> 2] = v;
            mt6589_eint_eval_all(s);
            return;
        }
        if (offset == EINT(EINT_MASK_SET0) + i * 4) {
            s->regs[MASK(i) >> 2] |= v;
            mt6589_eint_update_irq(s);
            return;
        }
        if (offset == EINT(EINT_MASK_CLR0) + i * 4) {
            s->regs[MASK(i) >> 2] &= ~v;
            mt6589_eint_eval_all(s);
            return;
        }
        if (offset == SOFT(i)) {
            s->regs[STA(i) >> 2] |= v;      /* software interrupt: sets the line */
            mt6589_eint_update_irq(s);
            return;
        }
        if (offset == EINT(EINT_SOFT_SET0) + i * 4) {
            s->regs[STA(i) >> 2] |= v;
            mt6589_eint_update_irq(s);
            return;
        }
        if (offset == EINT(EINT_SOFT_CLR0) + i * 4) {
            s->regs[STA(i) >> 2] &= ~v;
            mt6589_eint_update_irq(s);
            return;
        }
    }

    if (offset < sizeof(s->regs)) {
        s->regs[offset >> 2] = v;
        /* Polarity and sense are plain storage; a change re-evaluates every line. */
        if (offset >= SENS(0) && offset < POL(0) + EINT_REGS * 4) {
            mt6589_eint_eval_all(s);
        }
    }
}

static const MemoryRegionOps mt6589_eint_ops = {
    .read = mt6589_eint_read,
    .write = mt6589_eint_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_eint_realize(DeviceState *dev, Error **errp)
{
    Mt6589EintState *s = MT6589_EINT(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->line_asserted, 0, sizeof(s->line_asserted));
    s->pmic_sta = 0;
    s->pmic_mask = 0;
    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_eint_ops, s,
                          "mt6589-eint", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static void mt6589_eint_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mt6589_eint_realize;
    dc->desc = "MediaTek MT6589 external interrupt controller";
}

static const TypeInfo mt6589_eint_types[] = {
    {
        .name = TYPE_MT6589_EINT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589EintState),
        .class_init = mt6589_eint_class_init,
    },
};

DEFINE_TYPES(mt6589_eint_types)
