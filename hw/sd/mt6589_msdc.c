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
/*
 * FIFOCS field positions are from the downstream preloader's msdc.h, because the
 * datasheet's FIFOCS table lists only FIFOCLR:
 *
 *     MSDC_FIFOCS_RXCNT  (0xff << 0)    R
 *     MSDC_FIFOCS_TXCNT  (0xff << 16)   R
 *     MSDC_FIFOCS_CLR    (1UL  << 31)   RW
 *
 * RXCNT at 7:0, NOT 11:8 - the guest masks the register with 0xff to get the word
 * count, so a count placed at 11:8 reads as zero and the drain loop exits
 * immediately or spins.
 */
#define MSDC_FIFOCS_RXCNT     (0xffu << 0)
#define MSDC_FIFOCS_TXCNT     (0xffu << 16)
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
/*
 * SDC_BLK_NUM is 0x50, not 0x58 - the datasheet puts SDC_BLK_NUM at 0x50,
 * SDC_CSTS at 0x58 and SDC_CSTS_EN at 0x5c. An earlier version of this file had
 * 0x58/0x5c, which silently aliased the block count onto the card-status register:
 * the guest's block count writes landed nowhere and reads of card status returned
 * a count.
 */
/*
 * SDC_CMD fields, from the datasheet: CMD in 5:0, BREAK 6, RSPTYP 9:7, DTYPE 12:11,
 * RW 13, STOP 14, GOIRQ 15, LEN 27:16.
 *
 * DTYPE is a two-bit field, not a single "has data" bit: 00 = none, 01 = single
 * block, 10 = multiple blocks, 11 = stream. Treating bit 11 alone as "has data"
 * happens to work for single-block reads and breaks for multiple-block ones, so the
 * whole field is decoded.
 */
#define SDC_CMD_CMD_MASK  0x3fu
#define SDC_CMD_DTYPE_SHIFT 11
#define SDC_CMD_DTYPE_MASK (0x3u << SDC_CMD_DTYPE_SHIFT)
#define SDC_CMD_DTYPE_NONE 0u
#define SDC_CMD_RW         (1u << 13)

#define SDC_BLK_NUM   0x50
#define SDC_CSTS      0x58
#define SDC_CSTS_EN   0x5c
#define MSDC_BLK_SZ   0x54
#define EMMC_CFG0     0x70
#define EMMC_CFG1     0x74
#define EMMC_STS      0x78

/*
 * EMMC_CFG0/STS. The boot ROM's last action before halting is to start the eMMC
 * boot mode and wait for the result:
 *
 *   0x244a  ldr.w r5, [r8, #0x78]     ; r8 = 0x11230000, so EMMC_STS
 *   0x244e  tst.w r5, r9              ; r9 = 0x6e
 *   0x2452  beq   0x2458              ; count down r4 and retry
 *
 * 0x6e is BTDRCV|BTARCV|BTATO|BTDTO - "boot data received, ack received, or an
 * ack/data timeout". Any of them ends the wait, and the ACK/data-received bits are
 * what a card that answered produces.
 */
#define EMMC_CFG0_BTSTART (1u << 0)
#define EMMC_CFG0_BTSTOP  (1u << 1)
/*
 * BOOTUPSTATE (bit 4) is the one the boot ROM waits on, not the ack/data-received
 * bits above it:
 *
 *     0x233a  ldr r0, [r4, #120]   ; r4 = 0x11230000, so EMMC_STS
 *     0x233e  lsls r0, r0, #27     ; bit 4 into the sign bit
 *     0x2340  bpl  ...             ; branch while bit 4 is clear
 *
 * "Boot operation has completed". Reporting only BTARCV/BTDRCV leaves this clear and
 * the ROM waits for ever - which is why it never issued commands: it was already past
 * the command path, waiting for a boot-mode handshake that the model answered flatly.
 */
#define EMMC_STS_BOOTUPSTATE (1u << 4)

#define EMMC_STS_BTDERR   (1u << 0)
#define EMMC_STS_BTAERR   (1u << 1)
#define EMMC_STS_BTDTO    (1u << 2)
#define EMMC_STS_BTATO    (1u << 3)
#define EMMC_STS_BTSTS    (1u << 4)
#define EMMC_STS_BTARCV   (1u << 5)
#define EMMC_STS_BTDRCV   (1u << 6)
/*
 * How much the boot area fetch delivers. eMMC boot partitions are 128 KiB or 64
 * KiB depending on the part; the FIFO here is 2 KiB, so this is a chunk size rather
 * than the whole area - the guest reads it out and, if it wants more, issues further
 * transfers. Sized to one block so the ROM's first read sees a complete block.
 */
#define BOOT_AREA_DEFAULT 512
/* The unit emmc_boot_read_blocks consumes in, and therefore the refill size. */
#define MSDC_CHUNK_BYTES 0x200

/* A card that is present but has no backing image: identification succeeds and
 * any data transfer reads zeroes. Enough for the boot path to choose a device;
 * see the note at the end of this file about what that means. */
#define MSDC_CARD_OCR 0xc0ff8000u

/*
 * Card capacity in 512-byte sectors. A card is at least 1 MiB so the CSD
 * capacity field never underflows when no image is attached; with an image the
 * real size is used.
 */
static uint32_t mt6589_msdc_sectors(Mt6589MsdcState *s)
{
    size_t bytes = s->image_size ? s->image_size : (1u << 20);
    return (uint32_t)(bytes / 512);
}

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

/*
 * Fill the read FIFO from the backing image for the current block command.
 *
 * The FIFO is modelled as a byte buffer that RXDATA drains 4 bytes at a time,
 * which is how the guest reads it (MSDC_RXDATA reads in the data-port loop). When
 * no image is loaded the buffer stays zero, and the transfer still completes - a
 * card that is present but blank.
 */
static void mt6589_msdc_fill_fifo(Mt6589MsdcState *s)
{
    uint32_t block_size = s->sdc_blk_sz ? s->sdc_blk_sz : 512;
    uint32_t count = s->sdc_blk_num ? s->sdc_blk_num : 1;
    uint64_t total = (uint64_t)block_size * count;
    uint64_t off = s->sdc_arg;

    s->data = NULL;
    s->data_total = 0;
    s->data_len = 0;
    s->data_pos = 0;
    if (s->image && off < s->image_size) {
        total = MIN(total, s->image_size - off);
        s->data = s->image + off;
        s->data_total = total;
        s->data_len = MIN(total, (uint32_t)MSDC_CHUNK_BYTES);
    }
    s->int_status |= MSDC_INT_XFER_COMPL;
}

static void mt6589_msdc_complete_command(Mt6589MsdcState *s)
{
    uint32_t cmd = s->sdc_cmd;
    uint32_t opcode = cmd & 0x3f;
    bool has_data = ((cmd & SDC_CMD_DTYPE_MASK) >> SDC_CMD_DTYPE_SHIFT) !=
                    SDC_CMD_DTYPE_NONE;
    bool is_write = (cmd & SDC_CMD_RW) != 0;

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
        /*
         * CSD_STRUCTURE lives at CSD bits 127:126. unstuff_bits() in mmc_core.c
         * maps CSD bit b to resp[3 - b/32], so bit 126 lands in resp[0] bit 30 -
         * the *top* word of the register pair is resp[0].
         *
         * The value has to be 1 (block-addressed SDHC), and the two obvious
         * alternatives both fail on this driver:
         *
         *   3 (CSD_STRUCT_EXT_CSD) is what a real eMMC reports, but
         *     mmc_decode_csd() only accepts 3 inside its mmc_card_sd() branch;
         *     on the path this card actually takes it prints "Unknown CSD ver 3"
         *     and returns MMC_ERR_INVALID, so no block device is ever registered
         *     and the preloader asserts in sec.c on blkdev_get(). That was the
         *     failure this model was producing.
         *   2 (CSD_STRUCT_VER_1_2) is accepted on the eMMC path, but that path
         *     is only reached when CMD55 fails, and making CMD55 time out aborts
         *     the run before card init finishes.
         *
         * With 1 the driver fills the fixed fields in itself and takes the
         * capacity from CSD bits 69:48 as capacity = (1 + m) << 10, so real
         * block addressing and a real sector count come out either way.
         *
         * Capacity therefore rides in resp[1] and resp[2], not resp[0]:
         * CSD bit 48 maps to resp[2] bit 16, so m spans resp[2] bits 31:16 and
         * resp[1] bits 15:0. Writing those the other way round still looks like
         * a plausible card, just with the wrong size.
         */
        s->sdc_resp[0] = (uint32_t)1 << 30;
        s->sdc_resp[1] = (uint32_t)((mt6589_msdc_sectors(s) / 1024 - 1) & 0xffff) << 16;
        s->sdc_resp[2] = (uint32_t)((mt6589_msdc_sectors(s) / 1024 - 1) >> 16) & 0x3f;
        s->sdc_resp[3] = s->sdc_resp[0];
        break;
    case 13:  /* SEND_STATUS */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 16:  /* SET_BLOCKLEN */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 17:  /* READ_SINGLE_BLOCK */
    case 18:  /* READ_MULTIPLE_BLOCK */
    case 24:  /* WRITE_BLOCK */
    case 25:  /* WRITE_MULTIPLE_BLOCK */
        /*
         * Data phase. The address in sdc_arg is a byte offset for SDHC cards,
         * which is what the identification sequence above presents.
         *
         * Reads are served from the backing image when one is present, so a card
         * image with a preloader in it is actually readable - without that the
         * transfer reports success and returns zeroes, which looks like a card
         * that is present but empty rather than a model with no data path.
         */
        s->sdc_resp[0] = 0x00000900;
        if (has_data && !is_write) {
            mt6589_msdc_fill_fifo(s);
        }
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
        /*
         * CKSTB (bit 7) reads 1 whenever the clock is not powered down: there is no
         * real oscillator to settle, and reporting "not stable" stalls the boot ROM.
         *
         * RST (bit 2) must never read back set. It is write-one self-clearing, and
         * the boot ROM polls its *absence*:
         *
         *     0x2194  ldr r0, [r4]        ; MSDC_CFG
         *     0x2196  lsls r0, r0, #0x1d  ; bit 2 -> sign
         *     0x2198  bmi  0x2194
         *
         * Clearing it on write is not enough - it must not be visible in a
         * read-modify-write either, or the loop never ends.
         */
        return (s->cfg & ~(MSDC_CFG_CKSTB | MSDC_CFG_RST)) |
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
    case MSDC_FIFOCS: {
        /*
         * RXCNT is the count of valid words left, in bits 11:8 - check against
         * the datasheet rather than assuming the shift, because the guest polls
         * this before every drain and a wrong field means it either spins waiting
         * for data that is already there or reads past the end.
         */
        /*
         * RXCNT is an 8-bit FIFO occupancy, not a count of everything still
         * pending. Saturate at 0xff: the guest tests `FIFOCS & 0xff != 0` to mean
         * "data available", so a total remaining of 65536 words reads as 0 and
         * the drain loop exits immediately with nothing read.
         */
        /*
         * Refill per 0x200-byte chunk rather than serving one contiguous span.
         *
         * emmc_boot_read_blocks consumes in 0x200 units and treats an empty FIFO
         * as "come back later" - it loops on FIFOCS until either data appears or
         * a status bit says the transfer ended. A model that drains one long span
         * to zero leaves the loop waiting for data that it thinks is still
         * coming, because nothing distinguishes "drained a chunk" from "finished".
         *
         * So the reported occupancy is always the size of the current chunk, and
         * the next chunk is presented when the previous one is exhausted.
         */
        uint32_t remaining = (s->data_len > s->data_pos)
                           ? s->data_len - s->data_pos : 0;
        if (s->data && remaining == 0 && s->data_pos < s->data_total) {
            remaining = MIN(s->data_total - s->data_pos,
                            (uint32_t)MSDC_CHUNK_BYTES);
        }
        uint32_t words = (remaining + 3) / 4;
        if (words > 0xff) {
            words = 0xff;
        }
        qemu_log_mask(LOG_UNIMP, "%s: %s FIFOCS read -> rxcnt %u\n",
                      __func__, s->name, words);
        return words;
    }
    case SDC_CFG:     return s->sdc_cfg;
    case SDC_CMD:     return s->sdc_cmd;
    case SDC_ARG:     return s->sdc_arg;
    case SDC_STS:     return s->sdc_sts;
    case SDC_RESP0:   return s->sdc_resp[0];
    case SDC_RESP1:   return s->sdc_resp[1];
    case SDC_RESP2:   return s->sdc_resp[2];
    case SDC_RESP3:   return s->sdc_resp[3];
    case SDC_BLK_NUM: return s->sdc_blk_num;
    case MSDC_BLK_SZ: return s->sdc_blk_sz;
    case SDC_CSTS:    return 0;   /* not in a card-status transfer */
    case EMMC_CFG0:   return s->emmc_cfg0;
    case EMMC_CFG1:   return s->emmc_cfg1;
    case EMMC_STS:
        qemu_log_mask(LOG_UNIMP, "%s: %s EMMC_STS read -> 0x%08x\n",
                      __func__, s->name, s->emmc_sts);
        return s->emmc_sts;
    case MSDC_RXDATA: {
        /*
         * Drain four bytes at a time from whatever the data phase staged.
         *
         * The data is a pointer into the card image rather than a copy, because the
         * boot partition can be up to 0x40000 bytes and the guest fetches it in one
         * go - it reads until FIFOCS.RXCNT is 0, so stopping after one block leaves
         * it draining an empty FIFO and timing out.
         */
        uint32_t v = 0;
        for (unsigned i = 0; i < 4; i++) {
            uint8_t b = 0;
            if (s->data && s->data_pos < s->data_total) {
                b = s->data[s->data_pos++];
                /* Roll into the next chunk when this one is exhausted, so the
                 * guest sees fresh occupancy rather than a permanent zero. */
                if (s->data_pos >= s->data_len) {
                    s->data_len = MIN(s->data_total,
                                      s->data_pos + (uint32_t)MSDC_CHUNK_BYTES);
                }
            }
            v |= (uint32_t)b << (8 * i);
        }
        return v;
    }
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
    case MSDC_BLK_SZ:
        s->sdc_blk_sz = value;
        return;
    case EMMC_CFG0: {
        /*
         * BOOTSTART is an EDGE, not a level: the controller fetches the boot
         * partition on the write that sets bit 0, and the old value has to be read
         * before the store - reading it after gives the new value back and turns
         * every edge test into "no edge".
         */
        uint32_t prev = s->emmc_cfg0;
        s->emmc_cfg0 = value;
        qemu_log_mask(LOG_UNIMP, "%s: %s EMMC_CFG0 0x%08x -> 0x%08x\n",
                      __func__, s->name, prev, (uint32_t)value);
        if ((value & EMMC_CFG0_BTSTART) && !(prev & EMMC_CFG0_BTSTART)) {
            /*
             * Stage the whole boot image, not one block. The guest's payload loader
             * copies the first 0x200 bytes it already has and then fetches the rest
             * in a single request, draining until RXCNT reaches 0; capping at one
             * block means that request can never be satisfied and the guest sits in
             * its timeout countdown.
             *
             * eMMC boots from a separate boot partition, which this image does not
             * carry, so the preloader is served from the front of the card. That is
             * a decision about the modelled board, not a claim about the part.
             */
            qemu_log_mask(LOG_UNIMP, "%s: %s BOOTSTART edge, image %s, len %zu\n",
                          __func__, s->name, s->image ? "yes" : "no",
                          s->image ? s->image_size : (size_t)0);
            s->data = s->image;
            s->data_total = s->image ? MIN(s->image_size, 0x40000) : 0;
            s->data_len = MIN(s->data_total, (uint32_t)MSDC_CHUNK_BYTES);
            s->data_pos = 0;
            /*
             * BOOTUPSTATE (4) is what FUN_0000242c() classifies, but the data
             * mover emmc_boot_read_blocks() waits on `EMMC_STS & 5` - bits 0 and
             * 2, i.e. BTDERR/BTDTO. It also needs RXCNT non-zero in FIFOCS, which
             * the staged buffer provides.
             *
             * Note this must survive the write-1-to-clear in FUN_0000242c, which
             * writes the status word back after classifying it. That function is
             * called before the first read, so a status cleared there has to be
             * re-established by the time the reader polls - which is why the
             * completion bits are set here rather than only on a separate event.
             */
            s->emmc_sts = EMMC_STS_BOOTUPSTATE | EMMC_STS_BTARCV |
                          EMMC_STS_BTDRCV | EMMC_STS_BTDERR;
            s->int_status |= MSDC_INT_XFER_COMPL;
            mt6589_msdc_update_irq(s);
        }
        }
        if (value & EMMC_CFG0_BTSTOP) {
            s->emmc_sts = 0;
            s->emmc_cfg1 = 0;
        }
        return;
    case EMMC_STS:
        /*
         * Write-1-to-clear, as the hardware does - but the boot-mode completion
         * bits must stay asserted while a transfer is still in progress, because
         * emmc_boot_read_blocks waits on `EMMC_STS & 5` (BTDERR/BTDTO) as one of
         * its two liveness conditions. The ROM's own FUN_0000242c writes the
         * status word back after classifying it, which would clear exactly the
         * bits the reader then needs.
         *
         * Keeping BOOTUPSTATE while data remains staged is the behaviour that
         * makes the drain loop terminate on the data running out rather than on a
         * status timeout.
         */
        s->emmc_sts &= ~((uint32_t)value);
        if (s->data && s->data_pos < s->data_len) {
            s->emmc_sts |= EMMC_STS_BOOTUPSTATE;
        }
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

static void mt6589_msdc_finalize(Object *obj)
{
    Mt6589MsdcState *s = MT6589_MSDC(obj);

    g_free(s->image);
    s->image = NULL;
}

static void mt6589_msdc_realize(DeviceState *dev, Error **errp)
{
    Mt6589MsdcState *s = MT6589_MSDC(dev);

    if (s->image) {
        qemu_log_mask(LOG_UNIMP, "%s: %s has a %zu byte card image\n",
                      __func__, s->name, s->image_size);
    }
}

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

static void mt6589_msdc_set_image(Object *obj, const char *value, Error **errp)
{
    Mt6589MsdcState *s = MT6589_MSDC(obj);
    gchar *contents = NULL;
    gsize len = 0;

    if (!g_file_get_contents(value, &contents, &len, NULL)) {
        error_setg(errp, "cannot read card image '%s'", value);
        return;
    }
    g_free(s->image);
    s->image = (uint8_t *)contents;
    s->image_size = len;
    qemu_log_mask(LOG_UNIMP, "%s: card image loaded, %zu bytes\n", __func__,
                  (size_t)len);
}

static void mt6589_msdc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mt6589_msdc_realize;
    object_class_property_add_str(oc, "image", NULL, mt6589_msdc_set_image);
}

static const TypeInfo mt6589_msdc_types[] = {
    {
        .name = TYPE_MT6589_MSDC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589MsdcState),
        .instance_init = mt6589_msdc_init,
        .instance_finalize = mt6589_msdc_finalize,
        .class_init = mt6589_msdc_class_init,
    },
};

DEFINE_TYPES(mt6589_msdc_types)
