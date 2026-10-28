/* SPDX-License-Identifier: GPL-2.0-or-later */
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
#include "hw/misc/mt6320_pmic.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"
#include "target/arm/cpu.h"
#include "hw/arm/mt6589.h"
#include "hw/misc/mt6320_pmic.h"
#include "hw/arm/mt6589-regs/mt6589-pwrap-regs.h"

/* Offset in the wrapper's own window, named as the datasheet names it. */
#define PWRAP(name) (MT6589_PWRAP_##name - MT6589_PWRAP_BASE)

/* Register offsets in the wrapper's own window. */
#define R_WRAP_EN PWRAP(WRAP_EN)
#define R_WRAP_STA PWRAP(WRAP_STA)
#define R_MAN_EN PWRAP(MAN_EN)
#define R_MAN_CMD PWRAP(MAN_CMD)
#define R_MAN_RDATA PWRAP(MAN_RDATA)
#define R_CMD PWRAP(WACS2_CMD)
#define R_STA PWRAP(WACS2_RDATA)
#define R_VLDCLR PWRAP(WACS2_VLDCLR)
#define R_CIPHER_RDY   0x144
/*
 * 0xb0 is PWRAP_INT_EN, not SIDLY: the datasheet extract, the preloader's
 * mt_pmic_wrap_init.h and LK's copy all put SIDLY at 0x00c and INT_EN at 0x0b0. This
 * model used to call 0xb0 "SIDLY" and answer every read of it with 2, which meant a
 * guest reading the interrupt-enable register saw a constant instead of what it wrote.
 */
#define R_INT_EN PWRAP(INT_EN)

/*
 * Auxiliary registers the preloader's own PWRAP init walks; the boot ROM never
 * touches them. Offsets are PMIC_WRAP_BASE + n from the preloader's
 * mt_pmic_wrap_init.h. They are plain read/write latches with no protocol
 * behind them, so they are stored and read back rather than modelled.
 */
#define R_DIO_EN PWRAP(DIO_EN)
#define R_ARB_SIDLY PWRAP(SIDLY)
/*
 * Event routing, which both the downstream (pwrap_hal.c) and mainline (mtk-pmic-wrap.c) program as
 * part of the wrapper init: EVENT_IN_EN 0x34, EVENT_DST_EN 0x38, and the status pair at 0xD4/0xD8.
 * Without them those writes were dropped and read back zero.
 */
#define R_EVENT_IN_EN  PWRAP(EVENT_IN_EN)
#define R_EVENT_DST_EN PWRAP(EVENT_DST_EN)
#define R_EVENT_STA    PWRAP(EVENT_STA)
#define R_EVENT_STACLR PWRAP(EVENT_STACLR)
#define R_HIPRIO_EN PWRAP(HIPRIO_ARB_EN)
#define R_HARB_STA0 PWRAP(HARB_STA0)
#define R_HARB_STA1 PWRAP(HARB_STA1)

/* Status bits, from GET_INIT_DONE0 / GET_SYNC_IDLE0 / GET_WACS0_FSM. */
#define STA_INIT_DONE  (1u << 21)
#define STA_SYNC_IDLE  (1u << 20)
#define STA_FSM_SHIFT  16
#define STA_FSM_MASK   (0x7u << STA_FSM_SHIFT)
#define STA_RDATA_MASK 0xffffu

#define FSM_IDLE       0
#define FSM_WFVLDCLR   6

/*
 * Command word. Bit 31 is the WRITE flag, not the read flag: the preloader
 * builds the command as `wacs_write = write << 31` (mt_pmic_wrap_init.c:338)
 * and calls it with write=1 to store a PMIC register, write=0 to fetch one.
 * Mainline agrees - pwrap_write32() sends `BIT(31) | ((adr >> 1) << 16) | wdata`
 * (mtk-pmic-wrap.c:1686). Having this inverted made every read look like a
 * write, so the FSM never parked in WFVLDCLR and the guest's wait_for_fsm_vldclr
 * spun until it timed out with E_PWR_WAIT_IDLE_TIMEOUT.
 */
#define CMD_ISWRITE    (1u << 31)
#define CMD_ADDR_SHIFT 16
#define CMD_DATA_MASK  0xffffu

static bool mt6589_pwrap_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = getenv("MT6589_PWRAP_LOG") != NULL;
    }
    return enabled;
}

static void mt6589_pwrap_reset(DeviceState *dev)
{
    Mt6589PwrapState *s = MT6589_PWRAP(dev);

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
    /*
     * A wrapper parked in WFVLDCLR because nobody collected the result must not stay there: the
     * guest's next access polls for an idle wrapper *before* issuing a command, so one abandoned
     * read wedges every later access. Measured consequence: the battery thread reported
     * "wait_for_fsm_idle fail" at 7.3 s and its transaction never completed; the wrapper has a
     * timeout of its own on real hardware, and this is the model's.
     */
    if (s->fsm != FSM_IDLE && mt6589_pwrap_log_enabled()) {
        /*
         * What the driver sees when it is waiting. Only non-idle states are logged: in a stuck boot
         * the status register is polled continuously, and the interesting part is which state it is
         * parked in, not the traffic around it.
         */
        fprintf(stderr, "PWRAP: status read with fsm=%u (polls=%u)\n", s->fsm, s->vldclr_polls);
    }
    if (s->fsm != FSM_IDLE && ++s->vldclr_polls > 8) {
        if (mt6589_pwrap_log_enabled()) {
            fprintf(stderr, "PWRAP: state %u abandoned, timing out to idle\n", s->fsm);
        }
        s->fsm = FSM_IDLE;
        s->vldclr_polls = 0;
    }
    return STA_INIT_DONE | STA_SYNC_IDLE |
           ((s->fsm & 0x7u) << STA_FSM_SHIFT) |
           (s->rdata & STA_RDATA_MASK);
}

/*
 * Read/write latches for the auxiliary registers above. The offsets are sparse,
 * so they go through an explicit mapping rather than an index derived from the
 * offset - 0x058 is not 0x00c + n.
 */
static uint32_t mt6589_pwrap_aux_get(Mt6589PwrapState *s, hwaddr offset)
{
    switch (offset) {
    case R_INT_EN:    return s->aux_int_en;
    case R_DIO_EN:    return s->aux_dio_en;
    case R_ARB_SIDLY: return s->aux_arb_sidly;
    case R_HIPRIO_EN: return s->aux_hiprio_en;
    case R_HARB_STA0: return s->aux_harb_sta0;
    case R_HARB_STA1: return s->aux_harb_sta1;
    default:          return 0;
    }
}

static void mt6589_pwrap_aux_set(Mt6589PwrapState *s, hwaddr offset,
                                 uint32_t value)
{
    switch (offset) {
    case R_DIO_EN:    s->aux_dio_en = value;    break;
    case R_INT_EN:    s->aux_int_en = value;    break;
    case R_ARB_SIDLY: s->aux_arb_sidly = value; break;
    case R_HIPRIO_EN: s->aux_hiprio_en = value; break;
    case R_HARB_STA0: s->aux_harb_sta0 = value; break;
    case R_HARB_STA1: s->aux_harb_sta1 = value; break;
    default:          break;
    }
}

/*
 * FQMTR results, in the order the guest asks for them. rtc_measure_four_clock()
 * measures four clocks and rtc_xosc_check_clock() accepts only
 * result[0] in 3..7, result[1] in 1500..6000, result[2] == 0, result[3] == 0.
 * A board with a working 32 kHz crystal produces figures like these; the model
 * returns them directly rather than simulating the measurement, so this is a
 * shaped response and documented as such, not a measurement.
 */
/*
 * PMIC register access. The registers belong to the PMIC device (hw/misc/mt6320_pmic.c), which owns
 * the imported map from the downstream's upmu_hw.h, the frequency meter's result sequence and the
 * chip id; the wrapper only carries the transaction. The WACS field holds the register address halved,
 * which is why these helpers shift.
 */
uint16_t mt6589_pwrap_pmic_read(Mt6589PwrapState *s, uint16_t wacs)
{
    if (!s->pmic_dev) {
        return 0;               /* no PMIC on the bus: the read has nothing to return */
    }
    return mt6320_pmic_read_wacs(s->pmic_dev, wacs);
}

void mt6589_pwrap_pmic_write(Mt6589PwrapState *s, uint16_t wacs, uint16_t value)
{
    if (!s->pmic_dev) {
        return;
    }
    mt6320_pmic_write_wacs(s->pmic_dev, wacs, value);
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
    case R_DIO_EN:
    case R_ARB_SIDLY:
    case R_HIPRIO_EN:
    case R_HARB_STA0:
    case R_HARB_STA1:
    case R_INT_EN:
        return mt6589_pwrap_aux_get(s, offset);
    case R_EVENT_IN_EN:
        return s->aux_event_in_en;
    case R_EVENT_DST_EN:
        return s->aux_event_dst_en;
    case R_EVENT_STA:
        return s->aux_event_sta;
    case R_EVENT_STACLR:
        return 0;
    case R_CIPHER_RDY:
        /*
         * PWRAP's own cipher-ready bit, not the PMIC-side DEW one. The kernel's
         * wait_for_cipher_ready() spins on `readl(...) & 1`, so 0 here stalls
         * pwrap_drv_init for ever with no other access to show for it.
         */
        return s->cipher_rdy & 1;
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

    /*
     * MT6589_PWRAP_LOG=1 traces the transaction register pair. A stuck wrapper is
     * diagnosed by what the guest wrote and in what order, not by guessing at the
     * state machine: the guest's protocol is fixed, so an access that never completes
     * is either a command the model answered wrongly or a clear the model did not see.
     */
    if (mt6589_pwrap_log_enabled()) {
        fprintf(stderr, "PWRAP W off=%02" HWADDR_PRIx " val=%08" PRIx32
                " fsm=%u pc=%08x lr=%08x task=%s\n", offset, cmd, s->fsm,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[15] : 0,
                current_cpu ? (uint32_t)cpu_env(current_cpu)->regs[14] : 0,
                mt6589_guest_task());
    }

    switch (offset) {
    case R_CMD:
        /*
         * A new command supersedes an uncollected result. The guest's protocol is: write the
         * command, wait for WFVLDCLR, read the data, write VLDCLR. When a read's result is never
         * collected - a driver that timed out gives up - the wrapper stays parked in WFVLDCLR and
         * every later wait_for_fsm_idle fails, which is the pair of errors the battery thread
         * printed ("wait_for_fsm_idle fail", then "pwrap_wacs2 fail") just before the boot went
         * quiet. Accepting the next command instead of staying parked is what keeps one abandoned
         * access from wedging the wrapper for every other thread.
         */
        if (s->fsm == FSM_WFVLDCLR) {
            if (mt6589_pwrap_log_enabled()) {
                fprintf(stderr, "PWRAP: new command while WFVLDCLR, "
                        "dropping the uncollected result\n");
            }
            s->fsm = FSM_IDLE;
        }
        if (cmd & CMD_ISWRITE) {
            mt6589_pwrap_pmic_write(s, wacs, cmd & CMD_DATA_MASK);
            /*
             * Write: store, and return straight to idle. The guest's write path
             * does not poll for the result, and parking here would make its next
             * wait_for_fsm_idle see 6 and spin.
             */
            s->fsm = FSM_IDLE;
        } else {
            /*
             * Read: put the value in the status word and park the FSM in
             * WFVLDCLR until the guest collects it by writing the valid-clear
             * register. That is what wait_for_fsm_vldclr waits for.
             */
            s->rdata = mt6589_pwrap_pmic_read(s, wacs);
            s->fsm = FSM_WFVLDCLR;
            s->vldclr_polls = 0;
            s->fsm_owner = mt6589_guest_task_ptr();
        }
        return;
    case R_VLDCLR:
        /* Data collected: back to idle. */
        s->fsm = FSM_IDLE;
        s->fsm_owner = 0;
        s->vldclr_polls = 0;
        return;
    case R_DIO_EN:
    case R_ARB_SIDLY:
    case R_HIPRIO_EN:
    case R_HARB_STA0:
    case R_HARB_STA1:
    case R_INT_EN:
        mt6589_pwrap_aux_set(s, offset, cmd);
        return;
    case R_EVENT_IN_EN:
        s->aux_event_in_en = cmd;
        return;
    case R_EVENT_DST_EN:
        s->aux_event_dst_en = cmd;
        return;
    case R_EVENT_STACLR:
        s->aux_event_sta &= ~cmd;
        return;
    case R_MAN_CMD:
        /* Manual mode: same transaction, different register pair. */
        if (cmd & CMD_ISWRITE) {
            mt6589_pwrap_pmic_write(s, wacs, cmd & CMD_DATA_MASK);
        } else {
            s->rdata = mt6589_pwrap_pmic_read(s, wacs);
        }
        return;
    case R_WRAP_EN:
    case R_MAN_EN:
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

void mt6589_pwrap_connect_pmic(Mt6589PwrapState *s, Mt6320PmicState *pmic)
{
    s->pmic_dev = pmic;
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
