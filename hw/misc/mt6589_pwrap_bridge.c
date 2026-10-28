/*
 * MediaTek MT6589 PWRAP bridge (the second wrapper window at 0x1101_7000).
 *
 * Why this exists: the wrapper has two windows, and the boot touches both. mt6589.dtsi describes it
 * plainly - pwrap@1000f000 with two reg ranges, "pwrap" and "pwrap-bridge" - and mainline's
 * mtk-pmic-wrap.c initialises the second one: IORD_ARB_EN, WACS3_EN, WACS4_EN, WDT_UNIT, WDT_SRC_EN,
 * TIMER_EN, INT_EN, then INIT_DONE3/4. The downstream does the same through mt_pmic_wrap_init.h's
 * PERI_PWRAP_BRIDGE_* registers. With no model there, those writes landed in the peripheral catch-all:
 * they were dropped and read back zero, which is what an absent block looks like rather than a broken
 * one.
 *
 * What is modelled: both register chains (WACS3 and WACS4) run PMIC transactions against the same PMIC
 * register file as the main window - the modem side and the AP see one PMIC, which is the point of the
 * bridge - INIT_DONE3/4 read back what was written (the guest sets them and expects them to stay set),
 * and INT_EN/INT_FLG/INT_CLR behave as enable/flag/clear storage.
 *
 * Not modelled: arbitration between the two chains (IARB is storage), the watchdog timer, and the
 * event routing to the modem - nothing in the boot path uses them yet, and inventing behaviour there
 * would be worse than leaving them as latches.
 *
 * Register offsets are expressions over mt6589-pwrap_bridge-regs.h, which the datasheet extraction
 * produced for this page.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/mt6589_pwrap_bridge.h"
#include "hw/arm/mt6589-regs/mt6589-pwrap_bridge-regs.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/memory.h"

#define BRIDGE(name) (MT6589_PWRAP_BRIDGE_##name - MT6589_PWRAP_BRIDGE_BASE)

/* The chain parks here when a read result has not been collected yet. */
#define BRIDGE_FSM_IDLE      0
#define BRIDGE_FSM_WFVLDCLR  6

static bool mt6589_pwrap_bridge_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("MT6589_PWRAP_LOG");

        enabled = e && *e && strcmp(e, "0") != 0;
    }
    return enabled;
}

static void mt6589_pwrap_bridge_chain(Mt6589PwrapBridgeState *s, hwaddr cmd_off, uint32_t cmd)
{
    uint16_t wacs = (cmd >> 16) & 0x7fff;
    bool is_write = (cmd & 0x80000000u) != 0;

    if (is_write) {
        mt6589_pwrap_pmic_write(s->pwrap, wacs, cmd & 0xffff);
        s->fsm[cmd_off == BRIDGE(WACS3_CMD) ? 0 : 1] = BRIDGE_FSM_IDLE;
    } else {
        s->rdata[cmd_off == BRIDGE(WACS3_CMD) ? 0 : 1] = mt6589_pwrap_pmic_read(s->pwrap, wacs);
        s->fsm[cmd_off == BRIDGE(WACS3_CMD) ? 0 : 1] = BRIDGE_FSM_WFVLDCLR;
    }
    if (mt6589_pwrap_bridge_log_enabled()) {
        fprintf(stderr, "PWRAP-BRIDGE %s wacs=%04x val=%08x\n",
                is_write ? "write" : "read", wacs, cmd);
    }
}

static uint64_t mt6589_pwrap_bridge_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589PwrapBridgeState *s = MT6589_PWRAP_BRIDGE(opaque);
    uint32_t v;

    if (offset == BRIDGE(WACS3_RDATA)) {
        v = s->rdata[0] | ((s->fsm[0] & 0x7) << 16);
    } else if (offset == BRIDGE(WACS4_RDATA)) {
        v = s->rdata[1] | ((s->fsm[1] & 0x7) << 16);
    } else if (offset == BRIDGE(INT_FLG) || offset == BRIDGE(INT_FLG_RAW)) {
        v = s->int_flg & s->regs[BRIDGE(INT_EN) >> 2];
    } else if (offset == BRIDGE(INIT_DONE3) || offset == BRIDGE(INIT_DONE4)) {
        v = s->regs[offset >> 2];
    } else if (offset >= sizeof(s->regs)) {
        return 0;
    } else {
        v = s->regs[offset >> 2];
    }
    if (mt6589_pwrap_bridge_log_enabled()) {
        fprintf(stderr, "PWRAP-BRIDGE R %03" HWADDR_PRIx " = %08x\n", offset, v);
    }
    return v;
}

static void mt6589_pwrap_bridge_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Mt6589PwrapBridgeState *s = MT6589_PWRAP_BRIDGE(opaque);
    uint32_t v = value;

    if (mt6589_pwrap_bridge_log_enabled()) {
        fprintf(stderr, "PWRAP-BRIDGE W %03" HWADDR_PRIx " = %08x\n", offset, v);
    }
    if (offset == BRIDGE(WACS3_CMD) || offset == BRIDGE(WACS4_CMD)) {
        mt6589_pwrap_bridge_chain(s, offset, v);
        return;
    }
    if (offset == BRIDGE(WACS3_VLDCLR) || offset == BRIDGE(WACS4_VLDCLR)) {
        s->fsm[offset == BRIDGE(WACS3_VLDCLR) ? 0 : 1] = BRIDGE_FSM_IDLE;
        return;
    }
    if (offset == BRIDGE(INT_CLR)) {
        s->int_flg &= ~v;
        return;
    }
    if (offset >= sizeof(s->regs)) {
        return;
    }
    s->regs[offset >> 2] = v;
}

static const MemoryRegionOps mt6589_pwrap_bridge_ops = {
    .read = mt6589_pwrap_bridge_read,
    .write = mt6589_pwrap_bridge_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

void mt6589_pwrap_bridge_connect(Mt6589PwrapBridgeState *s, Mt6589PwrapState *pwrap)
{
    s->pwrap = pwrap;
}

static void mt6589_pwrap_bridge_realize(DeviceState *dev, Error **errp)
{
    Mt6589PwrapBridgeState *s = MT6589_PWRAP_BRIDGE(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->int_flg = 0;
    s->fsm[0] = s->fsm[1] = BRIDGE_FSM_IDLE;
    s->rdata[0] = s->rdata[1] = 0;
    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_pwrap_bridge_ops, s,
                          "mt6589-pwrap-bridge", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void mt6589_pwrap_bridge_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mt6589_pwrap_bridge_realize;
    dc->desc = "MediaTek MT6589 PWRAP bridge";
}

static const TypeInfo mt6589_pwrap_bridge_types[] = {
    {
        .name = TYPE_MT6589_PWRAP_BRIDGE,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589PwrapBridgeState),
        .class_init = mt6589_pwrap_bridge_class_init,
    },
};

DEFINE_TYPES(mt6589_pwrap_bridge_types)
