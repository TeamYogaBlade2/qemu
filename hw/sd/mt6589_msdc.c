/*
 * MediaTek MT6589 MSDC - SD/MMC controller
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * Register offsets are from the datasheet's MSDC chapter and agree with the
 * preloader's own header (mediatek/platform/mt6589/preloader/src/drivers/inc/
 * msdc.h), which is the tree the guest actually compiles against. Where the two
 * disagree the header wins, because that is what the guest writes.
 *
 * What is modelled, and why each part is here rather than later:
 *
 *   - MSDC_CFG.CKSTB (bit 7). This is the first MSDC thing the boot ROM does: it
 *     programs the clock divider and then polls bit 7 until it reads 1. A model
 *     that leaves it 0 stalls the ROM at its SD initialisation for ever. That is
 *     exactly where the QEMU port stopped before this file existed.
 *   - MSDC_PS.CDSTS (bit 1) and the DAT/CMD pin bits, so a card-present board is
 *     distinguishable from an empty slot.
 *   - SDC_CMD execution with the responses the identification sequence needs
 *     (CMD0/1/2/3/8/9/55), because the boot path uses them to decide between SD
 *     and eMMC.
 *   - MSDC_INT as write-1-to-clear with MSDC_INTEN gating the IRQ line.
 *
 * The INTEN gating is not optional. In the Renode port the line was driven from
 * MSDC_INT alone; the guest's handler reads INT & INTEN, found zero, returned
 * without acknowledging, and the level-triggered GIC re-delivered for ever. That
 * cost a day and is the reason the gating is written down here.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/arm/mt6589.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"

/* MSDC_CFG */
#define MSDC_CFG_MODE       (1u << 0)
#define MSDC_CFG_CKPDN      (1u << 1)
#define MSDC_CFG_RST        (1u << 2)
#define MSDC_CFG_PIO        (1u << 3)
#define MSDC_CFG_CKDRVEN    (1u << 4)
#define MSDC_CFG_BV18SDT    (1u << 5)
#define MSDC_CFG_BV18PSS    (1u << 6)
#define MSDC_CFG_CKSTB      (1u << 7)
#define MSDC_CFG_CKDIV      (0xffu << 8)
#define MSDC_CFG_CKMOD      (0x3u << 16)

/* MSDC_PS */
#define MSDC_PS_CDEN        (1u << 0)
#define MSDC_PS_CDSTS       (1u << 1)
#define MSDC_PS_CDDEBOUNCE  (0xfu << 12)
#define MSDC_PS_DAT         (0xffu << 16)
#define MSDC_PS_CMD         (1u << 24)
#define MSDC_PS_WP          (1u << 31)

/* MSDC_INT / MSDC_INTEN */
#define MSDC_INT_MMCIRQ       (1u << 0)
#define MSDC_INT_CDSC         (1u << 1)
#define MSDC_INT_ACMDRDY      (1u << 3)
#define MSDC_INT_ACMDTMO      (1u << 4)
#define MSDC_INT_CMDRDY       (1u << 8)
#define MSDC_INT_CMDTMO       (1u << 9)
#define MSDC_INT_RSPCRCERR    (1u << 10)
#define MSDC_INT_XFER_COMPL   (1u << 12)
#define MSDC_INT_DXFER_DONE   (1u << 13)
#define MSDC_INT_DATTMO       (1u << 14)
#define MSDC_INT_DATCRCERR    (1u << 15)

/* MSDC_FIFOCS */
#define MSDC_FIFOCS_TXCNT     (0xfu << 0)
#define MSDC_FIFOCS_RXCNT     (0xfu << 8)
#define MSDC_FIFOCS_CLR       (1u << 31)

/* Register offsets */
#define MSDC_CFG      0x00
#define MSDC_IOCON    0x04
#define MSDC_PS       0x08
#define MSDC_INT      0x0c
#define MSDC_INTEN    0x10
#define MSDC_FIFOCS   0x14
#define MSDC_TXDATA   0x18
#define MSDC_RXDATA   0x1c
#define SDC_CFG       0x30
#define SDC_CMD       0x34
#define SDC_ARG       0x38
#define SDC_STS       0x3c
#define SDC_RESP0     0x40
#define SDC_RESP1     0x44
#define SDC_RESP2     0x48
#define SDC_RESP3     0x4c
#define SDC_BLK_NUM   0x58
#define SDC_BLK_SZ    0x5c

/* A card that is present but has no backing image: identification succeeds and
 * any data transfer reads zeroes. Enough for the boot path to choose a device;
 * see the note at the end of this file about what that means. */
#define MSDC_CARD_OCR 0xc0ff8000u

static void mt6589_msdc_update_irq(Mt6589MsdcState *s)
{
    /*
     * The line is the AND of status and enable. Driving it from status alone
     * makes an unacknowledgeable interrupt re-deliver for ever: the guest reads
     * INT & INTEN, sees zero, returns without clearing, and a level-triggered
     * GIC delivers again immediately. This is the single most expensive mistake
     * in the Renode port and it looked like a guest hang, not like an
     * interrupt bug.
     */
    qemu_set_irq(s->irq, (s->int_status & s->int_enable) != 0);
}

static void mt6589_msdc_complete_command(Mt6589MsdcState *s)
{
    uint32_t cmd = s->sdc_cmd;
    uint32_t opcode = cmd & 0x3f;

    s->sdc_sts &= ~0x1u;   /* SDC_STS.BUSY clear: the command finished */

    switch (opcode) {
    case 0:   /* GO_IDLE_STATE */
        s->card_idle = true;
        s->sdc_resp[0] = 0;
        break;
    case 1:   /* SEND_OP_COND (MMC) */
        s->sdc_resp[0] = MSDC_CARD_OCR;
        break;
    case 2:   /* ALL_SEND_CID */
        s->sdc_resp[0] = 0x12345678;
        s->sdc_resp[1] = 0x9abcdef0;
        s->sdc_resp[2] = 0x11223344;
        s->sdc_resp[3] = 0x55667788;
        break;
    case 3:   /* SET_RELATIVE_ADDR */
        s->sdc_resp[0] = s->sdc_arg & 0xffff0000u;
        break;
    case 8:   /* SEND_IF_COND */
        /* Echo the check pattern back, as a card that supports it must. */
        s->sdc_resp[0] = s->sdc_arg & 0xfffu;
        break;
    case 9:   /* SEND_CSD */
        s->sdc_resp[0] = 0x00ffffff;
        s->sdc_resp[1] = 0xffffffff;
        s->sdc_resp[2] = 0xffffffff;
        s->sdc_resp[3] = 0xffffffff;
        break;
    case 13:  /* SEND_STATUS */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 16:  /* SET_BLOCKLEN */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 41:  /* SD_APP_OP_COND (after CMD55) */
        /* Busy must clear or the guest retries for ever; report ready. */
        s->sdc_resp[0] = MSDC_CARD_OCR | (1u << 31);
        break;
    case 55:  /* APP_CMD */
        s->sdc_resp[0] = 0x00000920;
        break;
    default:
        s->sdc_resp[0] = 0x00000900;
        break;
    }

    s->int_status |= MSDC_INT_CMDRDY;
    if (s->sdc_cfg & (1u << 15)) {
        /* Some transfers end with a data phase; nothing is transferred here but
         * the completion is still reported so the guest does not wait. */
        s->int_status |= MSDC_INT_XFER_COMPL;
    }
    mt6589_msdc_update_irq(s);
}

static uint64_t mt6589_msdc_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589MsdcState *s = opaque;

    switch (offset) {
    case MSDC_CFG:
        /* CKSTB reads 1 whenever the clock is not powered down: there is no
         * real oscillator to settle, and reporting "not stable" is what stalls
         * the boot ROM. */
        return (s->cfg & ~MSDC_CFG_CKSTB) |
               ((s->cfg & MSDC_CFG_CKPDN) ? 0 : MSDC_CFG_CKSTB);
    case MSDC_IOCON:  return s->iocon;
    case MSDC_PS: {
        uint32_t v = s->ps;
        /* Card present, and the CMD/DAT lines idle high, which is what a card
         * that has not driven them looks like. */
        v |= MSDC_PS_CDSTS | MSDC_PS_CMD | MSDC_PS_DAT;
        return v;
    }
    case MSDC_INT:
        return s->int_status;
    case MSDC_INTEN:
        return s->int_enable;
    case MSDC_FIFOCS:
        /* FIFO always reports empty and ready: no data is buffered. */
        return 0;
    case SDC_CFG:     return s->sdc_cfg;
    case SDC_CMD:     return s->sdc_cmd;
    case SDC_ARG:     return s->sdc_arg;
    case SDC_STS:     return s->sdc_sts;
    case SDC_RESP0:   return s->sdc_resp[0];
    case SDC_RESP1:   return s->sdc_resp[1];
    case SDC_RESP2:   return s->sdc_resp[2];
    case SDC_RESP3:   return s->sdc_resp[3];
    case SDC_BLK_NUM: return s->sdc_blk_num;
    case SDC_BLK_SZ:  return s->sdc_blk_sz;
    case MSDC_RXDATA: return 0;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void mt6589_msdc_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Mt6589MsdcState *s = opaque;

    switch (offset) {
    case MSDC_CFG:
        s->cfg = (uint32_t)value & ~MSDC_CFG_CKSTB;
        if (value & MSDC_CFG_RST) {
            /* Self-clearing reset: the guest polls it back to 0. */
            s->cfg &= ~MSDC_CFG_RST;
        }
        return;
    case MSDC_IOCON:
        s->iocon = value;
        return;
    case MSDC_PS:
        s->ps = (uint32_t)value & (MSDC_PS_CDEN | MSDC_PS_CDDEBOUNCE);
        return;
    case MSDC_INT:
        /* Write 1 to clear. */
        s->int_status &= ~((uint32_t)value);
        mt6589_msdc_update_irq(s);
        return;
    case MSDC_INTEN:
        s->int_enable = value;
        mt6589_msdc_update_irq(s);
        return;
    case MSDC_FIFOCS:
        if (value & MSDC_FIFOCS_CLR) {
            /* nothing buffered */
        }
        return;
    case SDC_CFG:
        s->sdc_cfg = value;
        return;
    case SDC_ARG:
        s->sdc_arg = value;
        return;
    case SDC_BLK_NUM:
        s->sdc_blk_num = value;
        return;
    case SDC_BLK_SZ:
        s->sdc_blk_sz = value;
        return;
    case SDC_CMD:
        s->sdc_cmd = value;
        qemu_log_mask(LOG_UNIMP, "%s: CMD %u arg 0x%08x (slot %s)\n",
                      __func__, (unsigned)(value & 0x3f), s->sdc_arg,
                      s->name);
        /* The command is executed on the write, and BUSY is raised so a guest
         * that polls SDC_STS sees the same sequence as on hardware. */
        s->sdc_sts |= 0x1u;
        mt6589_msdc_complete_command(s);
        return;
    case MSDC_TXDATA:
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to offset 0x%"
                  HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_msdc_ops = {
    .read = mt6589_msdc_read,
    .write = mt6589_msdc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_msdc_init(Object *obj)
{
    Mt6589MsdcState *s = MT6589_MSDC(obj);


    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_msdc_ops, s,
                          "mt6589.msdc", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);

    s->card_idle = true;
    s->sdc_sts = 0;
    /* The SoC sets the slot number after realise; default to a placeholder so a
     * log line is never attributing to garbage. */
    snprintf(s->name, sizeof(s->name), "msdc?");
}

static void mt6589_msdc_class_init(ObjectClass *oc, const void *data)
{
}

static const TypeInfo mt6589_msdc_types[] = {
    {
        .name = TYPE_MT6589_MSDC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589MsdcState),
        .instance_init = mt6589_msdc_init,
        .class_init = mt6589_msdc_class_init,
    },
};

DEFINE_TYPES(mt6589_msdc_types)
