/*
 * MediaTek MT6589 TOPRGU - top-level reset generator and watchdog
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * WHY THIS FILE EXISTS AT ALL
 *
 * WDT_STA at +0x0c is the boot reason, and the guest makes decisions on it.
 * LK's is_force_boot() calls mtk_wdt_boot_check(); if that says "the last boot
 * ended in a watchdog reset" the recovery path is taken instead of the normal
 * one. Getting this wrong does not look like a watchdog problem - it looks like
 * the boot going somewhere unexpected, several stages later.
 *
 * It was previously served by the catch-all MMIO region, which returns 0. Boot
 * reason reading 0 happens to be *correct* (normal boot), so it worked by
 * accident. That is the dangerous kind of correct: the moment anything writes
 * WDT_MODE to enable the watchdog, the write would also be dropped silently, and
 * a guest that waits for WDT_RESTART to take effect would hang with nothing to
 * show for it.
 *
 * ADDRESS
 *
 * 0x1000_0000. This is worth stating loudly because 0x1000_7000 is the address a
 * lot of other MediaTek SoCs use for TOPRGU, and MT6589's 0x1000_7000 is
 * reserved. Three sources agree on 0x1000_0000 here - the datasheet memory map,
 * the preloader's mt_reg_base.h (RGU_BASE = IO_PHYS + 0) and the kernel dtsi
 * (watchdog@10000000) - so do not take the address from another SoC's layout.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/arm/mt6589.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"

#define RGU_WDT_MODE       0x00
#define RGU_WDT_LENGTH     0x04
#define RGU_WDT_RESTART    0x08
#define RGU_WDT_STA        0x0c
#define RGU_WDT_INTERVAL   0x10
#define RGU_WDT_SWRST      0x14
#define RGU_WDT_SWSYSRST   0x18
#define RGU_WDT_NONRST_REG 0x20
#define RGU_WDT_REQ_MODE   0x30
#define RGU_WDT_REQ_IRQ_EN 0x34
#define RGU_WDT_DRAMC_CTL  0x40

/* WDT_MODE bits, datasheet TOPRGU field table */
#define WDT_MODE_EN          (1u << 0)
#define WDT_MODE_EXTPOL      (1u << 1)
#define WDT_MODE_EXTEN       (1u << 2)
#define WDT_MODE_WDR_IRQ     (1u << 3)
#define WDT_MODE_AUTO_RESTART (1u << 4)
#define WDT_MODE_IN_DIS      (1u << 5)
#define WDT_MODE_UNLOCK_KEY  0x22000000u
#define WDT_MODE_UNLOCK_MASK 0xff000000u

/* WDT_STA bits - the boot reason */
#define WDT_STA_SPM_RST      (1u << 0)
#define WDT_STA_DEBUG_RST    (1u << 19)
#define WDT_STA_IRQ_RST      (1u << 29)
#define WDT_STA_SW_RST       (1u << 30)
#define WDT_STA_HW_RST       (1u << 31)

static uint64_t mt6589_toprgu_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589ToprguState *s = opaque;

    switch (offset) {
    case RGU_WDT_MODE:      return s->mode;
    case RGU_WDT_LENGTH:    return s->length;
    case RGU_WDT_RESTART:   return s->restart;
    case RGU_WDT_STA:
        /*
         * All zero on a normal boot: no hw/sw/irq/debug/spm reset was recorded.
         * This is a deliberate value rather than an unmodelled 0 - it is the
         * documented meaning of the bits, and LK's mtk_wdt_boot_check() reads
         * them to decide whether to take the recovery path.
         */
        return s->sta;
    case RGU_WDT_INTERVAL:  return s->interval;
    case RGU_WDT_SWRST:     return s->swrst;
    case RGU_WDT_SWSYSRST:  return s->swsysrst;
    case RGU_WDT_NONRST_REG:  return s->nonrst;
    case RGU_WDT_REQ_MODE:  return s->req_mode;
    case RGU_WDT_REQ_IRQ_EN: return s->req_irq_en;
    case RGU_WDT_DRAMC_CTL: return s->dramc_ctl;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void mt6589_toprgu_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    Mt6589ToprguState *s = opaque;

    switch (offset) {
    case RGU_WDT_MODE:
        /*
         * Bits 31:24 are an unlock key and the datasheet says 0x22 written
         * there permits the rest. Enforcing it matters: code that forgets the
         * key must not appear to have enabled the watchdog, because then the
         * absence of a reset looks like a broken model instead of a missing
         * unlock.
         */
        if ((value & WDT_MODE_UNLOCK_MASK) != WDT_MODE_UNLOCK_KEY) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "%s: WDT_MODE write 0x%" PRIx64 " without unlock key "
                          "0x22 in bits 31:24, ignored\n", __func__, value);
            return;
        }
        s->mode = (uint32_t)value;
        return;
    case RGU_WDT_RESTART:
        /* Kick: accept silently. There is no counter to reload, and the guest
         * treats a successful write as "the watchdog is fed". */
        s->restart = value;
        return;
    case RGU_WDT_SWRST:
        /*
         * Software reset. A real SoC would restart here; the model records the
         * request and keeps running, because QEMU has no board-level reset
         * contract for this machine yet. Logged so an unexpected reset request
         * is visible rather than invisible.
         */
        qemu_log_mask(LOG_UNIMP,
                      "%s: software reset requested (0x%" PRIx64 ") - recorded, "
                      "not acted on\n", __func__, value);
        s->swrst = value;
        return;
    case RGU_WDT_LENGTH:    s->length = value; return;
    case RGU_WDT_INTERVAL:  s->interval = value; return;
    case RGU_WDT_SWSYSRST:  s->swsysrst = value; return;
    case RGU_WDT_NONRST_REG:  s->nonrst = value; return;
    case RGU_WDT_REQ_MODE:  s->req_mode = value; return;
    case RGU_WDT_REQ_IRQ_EN: s->req_irq_en = value; return;
    case RGU_WDT_DRAMC_CTL: s->dramc_ctl = value; return;
    case RGU_WDT_STA:
        /* W1C on the reset-cause bits. */
        s->sta &= ~((uint32_t)value);
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to offset 0x%"
                  HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_toprgu_ops = {
    .read = mt6589_toprgu_read,
    .write = mt6589_toprgu_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_toprgu_reset(DeviceState *dev)
{
    Mt6589ToprguState *s = MT6589_TOPRGU(dev);

    /* Datasheet reset values where given, 0 otherwise. */
    s->mode = 0;
    s->length = 0x1fff;
    s->restart = 0x1978;
    s->sta = 0;          /* normal boot */
    s->interval = 0x0fff;
    s->swrst = 0x1248;
    s->swsysrst = 0;
    s->nonrst = 0;
    s->req_mode = 0;
    s->req_irq_en = 0;
    s->dramc_ctl = 0;
}

static void mt6589_toprgu_init(Object *obj)
{
    Mt6589ToprguState *s = MT6589_TOPRGU(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_toprgu_ops, s,
                          "mt6589.toprgu", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mt6589_toprgu_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    /* device_class_set_legacy_reset, not dc->reset: DeviceClass has no 'reset'
     * member in this version, and the legacy hook is the documented way to keep
     * a two-phase device. */
    device_class_set_legacy_reset(dc, mt6589_toprgu_reset);
}

static const TypeInfo mt6589_toprgu_types[] = {
    {
        .name = TYPE_MT6589_TOPRGU,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589ToprguState),
        .instance_init = mt6589_toprgu_init,
        .class_init = mt6589_toprgu_class_init,
    },
};

DEFINE_TYPES(mt6589_toprgu_types)
