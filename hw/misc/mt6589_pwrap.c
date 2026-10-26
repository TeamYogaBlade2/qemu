/*
 * MediaTek MT6589 PMIC_WRAP (pwrap) at 0x1000_F000
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * The wrapper is how the application processor reaches the MT6320 PMIC: a small
 * state machine that serialises a register access onto a two-wire bus. It has to
 * be modelled as a state machine rather than a plain register file, because the
 * guest's transaction routine polls the FSM state and will wait for ever at the
 * wrong state.
 *
 * PROTOCOL SOURCE
 *
 * Not the datasheet (its WACS offsets disagree with what the guest uses) and not
 * guesswork: the preloader's own transaction routine, FUN_00008c9c, recovered from
 * the binary. It holds these pointers, whose values are the 0xF000_0000-aliased
 * addresses:
 *
 *   DAT_00008e10 -> 0x1000f0a8   status
 *   DAT_00008e14 -> 0x1000f0ac   valid-clear
 *   DAT_00008e18 -> 0x1000f0a4   command / write data
 *
 * and drives:
 *
 *   if ((status << 10) >= 0)  -> "initialization isn't finished"
 *       the sign test means bit 21 = INIT_DONE
 *   state = (status & 0x7ffff) >> 16         FSM state in bits 18:16
 *   loop until state == 0                    0 = idle
 *   cmd = cd | (isread << 31) | ((addr >> 1) << 16)
 *   loop until state == 6                    6 = WFVLDCLR, data valid
 *   data = status & 0xffff
 *
 * The read and write paths differ, and that difference matters:
 *
 *   0x8de0  str cmd,[0x0a4]          issue the transaction
 *   0x8df2  cmp r5,#0 / beq 0x8d4c   r5 = isr (bit 31 of cmd)
 *   write   falls through to 0x8dbc, done - NO polling
 *   read    loops until state == 6, takes the low half, then writes 1 to 0x0ac
 *
 * So a WRITE must return the FSM to idle by itself; only a READ parks in WFVLDCLR
 * waiting to be collected. Parking on writes as well makes the guest's next
 * idle-wait see state 6 and spin.
 *
 * SYNC_IDLE (bit 20) matters as much as INIT_DONE: several waits use
 * wait_for_sync and spin until bit 20 reads 1. Leaving it 0 times them out even
 * though the FSM is idle.
 *
 * WHAT IS NOT MODELLED
 *
 * The MT6320 itself. Registers store what is written and read it back, which is
 * what a register does, and a small set of power-on defaults is provided from the
 * board's PMIC header. Anything that depends on real PMIC behaviour - charger
 * detection, ADC readings, regulator voltage actually changing - is not modelled
 * and is documented per register in mt6589-emulator/docs/ips/pwrap.md.
 *
 * Fabricating plausible constants instead of storing what was written is what
 * broke the RTC in the Renode port: the preloader wrote 0x0281 and read it back
 * expecting the same value.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/arm/mt6589.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"

/* Register offsets in the wrapper's own window. */
#define R_WRAP_EN      0x004
#define R_WRAP_STA     0x03C
#define R_MAN_EN       0x064
#define R_MAN_CMD      0x068
#define R_MAN_RDATA    0x06C
#define R_CMD          0x0A4
#define R_STA          0x0A8
#define R_VLDCLR       0x0AC
#define R_CIPHER_RDY   0x144
#define R_SIDLY        0x0B0

/* Status bits, from GET_INIT_DONE0 / GET_SYNC_IDLE0 / GET_WACS0_FSM. */
#define STA_INIT_DONE  (1u << 21)
#define STA_SYNC_IDLE  (1u << 20)
#define STA_FSM_SHIFT  16
#define STA_FSM_MASK   (0x7u << STA_FSM_SHIFT)
#define STA_RDATA_MASK 0xffffu

#define FSM_IDLE       0
#define FSM_WFVLDCLR   6

/* Command word. */
#define CMD_ISREAD     (1u << 31)
#define CMD_ADDR_SHIFT 16
#define CMD_DATA_MASK  0xffffu

/*
 * PMIC power-on values that the guest reads before writing anything.
 *
 * The wire address is the PMIC register number halved, because the wrapper
 * decodes `(cmd >> 16) & 0x7fff` as the WACS register - so PMIC 0x0100 arrives as
 * 0x0080 and, for example, PMIC 0xBC0A (DEW_READ_TEST) as 0x5E05.
 *
 * DEW_READ_TEST is the wrapper's link self-test: _pwrap_init_sidly() reads it at
 * each candidate delay and keeps the delays that return 0x5AA5. Returning
 * anything else makes every delay fail and the initialisation abort.
 */
struct Mt6589PwrapDefault {
    uint16_t wacs;
    uint16_t value;
};

static const struct Mt6589PwrapDefault mt6589_pmic_defaults[] = {
    /* PMIC 0xBC0A -> 0x5E05: the link read test, fixed value. */
    { 0x5E05, 0x5AA5 },
    /*
     * PMIC 0x0300 chip ID -> 0x0180. The MT6320 answers 0x2020 on this board
     * (operator's hardware, E2 silicon). Without a chip ID the kernel's
     * mt6397_probe() reads 0, rejects the part as "unsupported chip", and every
     * consumer of the PMIC defers for ever.
     */
    { 0x0180, 0x2020 },
};

/* Store the PMIC-side register file. Indexed by WACS number (address >> 1). */
#define PMIC_NUM_REGS 0x8000

static void mt6589_pwrap_reset(DeviceState *dev)
{
    Mt6589PwrapState *s = MT6589_PWRAP(dev);

    memset(s->pmic, 0, sizeof(s->pmic));
    for (size_t i = 0; i < ARRAY_SIZE(mt6589_pmic_defaults); i++) {
        s->pmic[mt6589_pmic_defaults[i].wacs] = mt6589_pmic_defaults[i].value;
    }
    s->fsm = FSM_IDLE;
    s->rdata = 0;
    s->cipher_rdy = 1;
}

/* The status word as the guest's transaction routine expects to read it. */
static uint32_t mt6589_pwrap_status(Mt6589PwrapState *s)
{
    /*
     * Always report initialised and synchronised: the model has no serial link to
     * train, and the guest's first act is to wait for both bits. Leaving either 0
     * produces a wait that never ends, not an error message.
     */
    return STA_INIT_DONE | STA_SYNC_IDLE |
           ((s->fsm & 0x7u) << STA_FSM_SHIFT) |
           (s->rdata & STA_RDATA_MASK);
}

static uint64_t mt6589_pwrap_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589PwrapState *s = opaque;

    switch (offset) {
    case R_WRAP_EN:
        return 1;
    case R_WRAP_STA:
    case R_STA:
        return mt6589_pwrap_status(s);
    case R_CMD:
        /*
         * Reading the command register gives the same status view: the guest
         * only reads 0xa4 in the manual-mode path, which the boot chain does not
         * use, and returning the status is harmless there.
         */
        return mt6589_pwrap_status(s);
    case R_VLDCLR:
        return 0;
    case R_CIPHER_RDY:
        /*
         * PWRAP's own cipher-ready bit, not the PMIC-side DEW one. The kernel's
         * wait_for_cipher_ready() spins on `readl(...) & 1`, so 0 here stalls
         * pwrap_drv_init for ever with no other access to show for it.
         */
        return s->cipher_rdy & 1;
    case R_SIDLY:
        return 2;
    case R_MAN_EN:
        return 0;
    case R_MAN_RDATA:
        return s->rdata;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read of unmodelled offset 0x%" HWADDR_PRIx
                  "\n", __func__, offset);
    return 0;
}

static void mt6589_pwrap_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Mt6589PwrapState *s = opaque;
    uint32_t cmd = (uint32_t)value;
    uint16_t wacs = (cmd >> CMD_ADDR_SHIFT) & 0x7fff;

    switch (offset) {
    case R_CMD:
        if (cmd & CMD_ISREAD) {
            /*
             * Read: put the value in the status word and park the FSM in
             * WFVLDCLR until the guest collects it by writing the valid-clear
             * register. That is what the read path at 0x8d70 waits for.
             */
            s->rdata = s->pmic[wacs];
            s->fsm = FSM_WFVLDCLR;
        } else {
            /*
             * Write: store, and return straight to idle. The write path at
             * 0x8dbc does not poll, and parking here would make the guest's next
             * wait_for_fsm_idle see 6 and spin.
             */
            s->pmic[wacs] = cmd & CMD_DATA_MASK;
            s->fsm = FSM_IDLE;
        }
        return;
    case R_VLDCLR:
        /* Data collected: back to idle. */
        s->fsm = FSM_IDLE;
        return;
    case R_MAN_CMD:
        /* Manual mode: same transaction, different register pair. */
        if (cmd & CMD_ISREAD) {
            s->rdata = s->pmic[wacs];
        } else {
            s->pmic[wacs] = cmd & CMD_DATA_MASK;
        }
        return;
    case R_WRAP_EN:
    case R_MAN_EN:
    case R_SIDLY:
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to unmodelled offset "
                  "0x%" HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_pwrap_ops = {
    .read = mt6589_pwrap_read,
    .write = mt6589_pwrap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_pwrap_init(Object *obj)
{
    Mt6589PwrapState *s = MT6589_PWRAP(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_pwrap_ops, s,
                          "mt6589.pwrap", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void mt6589_pwrap_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    device_class_set_legacy_reset(dc, mt6589_pwrap_reset);
}

static const TypeInfo mt6589_pwrap_types[] = {
    {
        .name = TYPE_MT6589_PWRAP,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589PwrapState),
        .instance_init = mt6589_pwrap_init,
        .class_init = mt6589_pwrap_class_init,
    },
};

DEFINE_TYPES(mt6589_pwrap_types)
