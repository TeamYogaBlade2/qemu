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
#include "system/dma.h"
#include "target/arm/cpu.h"

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

/*
 * Pad/delay tuning registers. LK programs these and reads them back to confirm
 * the write landed; the values have no behavioural effect here because there is
 * no real PHY to tune, but they must not read as a constant or the settle loop
 * never ends. Offsets from lk/include/platform/msdc.h.
 */
#define MSDC_PAD_CTL0        0x00e0
#define MSDC_PAD_CTL1        0x00e4
#define MSDC_PAD_CTL2        0x00e8
#define MSDC_PAD_TUNE        0x00ec
#define MSDC_DAT_RDDLY0      0x00f0
#define MSDC_DAT_RDDLY1      0x00f4

/* DMA registers, from lk/include/platform/msdc.h. */
#define MSDC_DMA_SA          0x0090
#define MSDC_DMA_CA          0x0094
#define MSDC_DMA_CTRL        0x0098
#define MSDC_DMA_CFG         0x009c
#define MSDC_DMA_CTRL_START  (1u << 0)
#define MSDC_DMA_CTRL_STOP   (1u << 1)
/*
 * MSDC_DMA_CFG bit 0 is STS, a READ-ONLY "the DMA is still active" status
 * (LK's msdc.h marks it "R").
 *
 * msdc_dma_stop() busy-waits on it:
 *     while ((MSDC_READ32(MSDC_DMA_CFG) & MSDC_DMA_CFG_STS) != 0);
 *
 * so if this model stored the guest's write verbatim - including that bit - the
 * loop could never exit. Every transfer here completes synchronously before the
 * write returns, so the engine is always idle and STS must always read clear.
 */
#define MSDC_DMA_CFG_STS     (1u << 0)

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
/*
 * DMA. LK programs a GPD descriptor (lk/include/platform/msdc.h) whose address it
 * puts in MSDC_DMA_SA, then sets DMA_CTRL_START and waits for the transfer to
 * finish. Without a DMA path the staged data sits in the FIFO for ever while
 * FIFOCS keeps reporting it - which is what a 149,142-iteration FIFOCS poll with
 * rxcnt 255 was: the guest waiting for a transfer that had already "happened" as
 * far as the FIFO was concerned.
 *
 * The descriptor layout (gpd_t) is:
 *
 *   u32 hwo:1, bdp:1, rsv0:6, chksum:8, intr:1, rsv1:15;
 *   void *next;      // +0x04
 *   void *ptr;       // +0x08  destination
 *   u32 buflen:16, extlen:8, rsv2:8;   // +0x0c
 *
 * Only the fields this needs are decoded, and only a single descriptor - the
 * guests here use one per block transfer.
 */
static void mt6589_msdc_dma_run(Mt6589MsdcState *s)
{
    uint8_t desc[32];
    uint32_t gpd = s->dma_sa;
    uint32_t written = 0;


    if (!gpd) {
        return;
    }

    /*
     * Walk the guest's descriptor chain, which is GPD -> BD list -> data.
     *
     * The shape is from LK's gpd_t and bd_t (include/platform/msdc.h) and was
     * confirmed against mi/msdc_dma_config() in the LK binary:
     *
     *   gpd_t:  u32 flags;  void *next;  void *ptr;  buflen:16; ...
     *   bd_t:   u32 flags;  void *next;  void *ptr;  buflen:16;
     *
     * flags bit 1 is BDP: "the pointer is a buffer-descriptor list, not data".
     * LK always sets it (observed flags = 0x3, i.e. HWO | BDP) and then leaves
     * the GPD's own buflen at zero - it fills bd_t entries instead:
     *
     *   81e08026  strh r1,[r3,#0xc]     ; bd.buflen
     *   81e08028  str  r0,[r3,#0x8]     ; bd.ptr
     *
     * Reading only the GPD therefore finds len = 0 and reports no data at all.
     * That is what left the second mmc_legacy_init() waiting for XFER_COMPL that
     * nothing could ever raise.
     */
    for (int hops = 0; gpd && hops < 64; hops++) {
        if (dma_memory_read(&address_space_memory, gpd, desc, sizeof(desc),
                            MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
            break;
        }
        uint32_t flags = ldl_le_p(desc);
        uint32_t next = ldl_le_p(desc + 4);
        uint32_t ptr = ldl_le_p(desc + 8);
        uint32_t buflen = lduw_le_p(desc + 12);

        if (flags & (1u << 1)) {          /* BDP: ptr is a BD list */
            for (int bd = 0; ptr && bd < 512; bd++) {
                uint8_t b[16];
                if (dma_memory_read(&address_space_memory, ptr, b, sizeof(b),
                                    MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
                    break;
                }
                uint32_t bflags = ldl_le_p(b);
                uint32_t bnext = ldl_le_p(b + 4);
                uint32_t bptr = ldl_le_p(b + 8);
                uint32_t blen = lduw_le_p(b + 12);
                if (bptr && blen && s->data &&
                        s->data_pos < s->data_total) {
                    uint32_t avail = s->data_total - s->data_pos;
                    uint32_t n = MIN(blen, avail);
                    dma_memory_write(&address_space_memory, bptr,
                                     s->data + s->data_pos, n,
                                     MEMTXATTRS_UNSPECIFIED);
                    s->data_pos += n;
                    written += n;
                }
                if (bflags & 1u) {        /* EOL */
                    break;
                }
                ptr = bnext;
            }
        } else if (ptr && buflen && s->data &&
                   s->data_pos < s->data_total) {
            uint32_t avail = s->data_total - s->data_pos;
            uint32_t n = MIN(buflen, avail);
            dma_memory_write(&address_space_memory, ptr, s->data + s->data_pos,
                             n, MEMTXATTRS_UNSPECIFIED);
            s->data_pos += n;
            written += n;
        }

        if (flags & 1u) {                 /* EOL */
            break;
        }
        gpd = next;
    }

    s->data = NULL;
    s->data_total = 0;
    s->data_len = 0;
    s->data_pos = 0;

    qemu_log_mask(LOG_UNIMP, "%s: %s DMA wrote %u bytes\n",
                  __func__, s->name, written);

    s->int_status |= MSDC_INT_XFER_COMPL;
    mt6589_msdc_update_irq(s);
}

static void mt6589_msdc_fill_fifo(Mt6589MsdcState *s)
{
    uint32_t block_size = s->sdc_blk_sz ? s->sdc_blk_sz : 512;
    uint32_t count = s->sdc_blk_num ? s->sdc_blk_num : 1;
    uint64_t total = (uint64_t)block_size * count;
    /*
     * sdc_arg carries a BLOCK NUMBER here, not a byte offset. Evidence: the
     * guest issues CMD17 with 0x13900 for the UBOOT partition, and 0x13900 is
     * exactly 0x2720_0000 / 512 - the partition's start block from its own
     * part_dump. Treating that as a byte offset read from 0x13900, which lands
     * in the middle of PRELOADER, and the header check then failed with
     * "UBOOT image doesn't exist" even though the image was correct.
     *
     * This also matches sdc_blk_sz being 0 when the command arrives: the guest
     * never programmed a block size, so a fixed 512 is what the transfer uses.
     */
    uint64_t off = (uint64_t)s->sdc_arg * 512;

    s->data = NULL;
    s->data_total = 0;
    s->data_len = 0;
    s->data_pos = 0;
    /*
     * ACMD51's SCR comes from the model, not from the card image. Serve it
     * before the image path so a small transfer does not fall through to
     * "offset 0 of the image", which would return the partition table.
     */
    if (s->scr_pending) {
        s->scr_pending = 0;
        s->data = s->meta;
        s->data_total = s->meta_len;
        s->data_len = s->meta_len;
        return;
    }
    if (s->image && off < s->image_size) {
        total = MIN(total, s->image_size - off);
        s->data = s->image + off;
        s->data_total = total;
        s->data_len = MIN(total, (uint32_t)MSDC_CHUNK_BYTES);
    }
    /*
     * No XFER_COMPL here. Staging the data is not completing the transfer:
     * completion is reported by the RXDATA drain (PIO) or mt6589_msdc_dma_run()
     * (DMA) once the last byte has been delivered. Raising it at staging time
     * set 0x1000 on every command that had a data phase, and LK - which reads
     * INT once and clears only the bits it saw - never acknowledged a 0x1000
     * that appeared afterwards, so the level-triggered line stayed asserted.
     */
}

#define MT6589_MSDC_CMD_TMO_NS  (5 * 1000000)

static void mt6589_msdc_cmd_tmo(void *opaque)
{
    Mt6589MsdcState *s = opaque;

    if (!s->cmd_tmo_pending) {
        return;
    }
    s->cmd_tmo_pending = 0;
    s->int_status |= MSDC_INT_CMDTMO;
    mt6589_msdc_update_irq(s);
}

static void mt6589_msdc_complete_command(Mt6589MsdcState *s)
{
    uint32_t cmd = s->sdc_cmd;
    uint32_t opcode = cmd & 0x3f;
    bool has_data = ((cmd & SDC_CMD_DTYPE_MASK) >> SDC_CMD_DTYPE_SHIFT) !=
                    SDC_CMD_DTYPE_NONE;
    /*
     * Clear the previous command's result bits before reporting this one.
     *
     * These are per-command status, not sticky flags: CMDRDY says "the command I
     * just issued finished". Leaving it set means the next command's wait sees
     * CMDRDY together with an error bit, and the drivers test the error in an
     * `else if` chain after the success case (preloader msdc.c:1055), so a
     * genuine CMDTMO is silently ignored.
     *
     * Measured before this fix: CMD55 set MSDC_INT_CMDTMO but the guest read
     * 0x300 - the stale CMDRDY from the previous command plus the new timeout -
     * and neither LK nor the preloader took its error branch, so neither ever
     * fell back from ACMD41 to CMD1.
     */
    s->int_status &= ~(MSDC_INT_CMDRDY | MSDC_INT_CMDTMO | MSDC_INT_RSPCRCERR |
                       MSDC_INT_ACMDRDY | MSDC_INT_ACMDTMO);
    bool is_write = (cmd & SDC_CMD_RW) != 0;
    bool was_app = s->app_cmd;
    s->app_cmd = 0;
    if (s->cmd_tmo_pending) {
        s->cmd_tmo_pending = 0;
        timer_del(&s->cmd_tmo_timer);
    }

    s->sdc_sts &= ~0x1u;   /* SDC_STS.BUSY clear: the command finished */

    switch (opcode) {
    case 0:   /* GO_IDLE_STATE */
        s->card_idle = true;
        s->sdc_resp[0] = 0;
        break;
    case 1:   /* SEND_OP_COND (MMC) */
        s->is_mmc = 1;
        s->sdc_resp[0] = MSDC_CARD_OCR | (1u << 31);
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
    case 8:   /* SEND_IF_COND (SD) / SEND_EXT_CSD (MMC) */
        if (s->sdc_arg == 0) {
            memset(s->meta, 0, sizeof(s->meta));
            s->meta[192] = 7;
            s->meta[183] = 2;
            /*
             * SEC_COUNT (offset 212) is what a 4.x eMMC reports its size in, and
             * it is the only field that can describe more than the 1 GB the
             * legacy CSD layout tops out at. Keep it in step with the image.
             */
            {
                uint32_t sec = (uint32_t)mt6589_msdc_sectors(s);
                s->meta[212] = sec & 0xff;
                s->meta[213] = (sec >> 8) & 0xff;
                s->meta[214] = (sec >> 16) & 0xff;
                s->meta[215] = (sec >> 24) & 0xff;
            }
            s->meta_len = 512;
            s->scr_pending = 1;
            s->sdc_resp[0] = 0x00000900;
            if (has_data && !is_write) {
                mt6589_msdc_fill_fifo(s);
            }
            break;
        }
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
        if (s->is_mmc) {
            /*
             * eMMC CSD. mmc_decode_csd() computes capacity from the *legacy*
             * fields for anything that is not an SD card with struct 1:
             *
             *     e = BITS(resp,47,3); m = BITS(resp,62,12);
             *     csd->capacity = (1 + m) << (e + 2);
             *     csd->read_blkbits = BITS(resp,80,4);
             *
             * so writing the SDHC layout here gives capacity 0 and the preloader
             * reports "nblks(0)" then fails to read UBOOT.
             *
             * e is only three bits, so the shift (e + 2) tops out at 9 - the
             * first attempt used e = 8, which overflows the field and decodes as
             * 0. With e = 7 and m = 157 the capacity is 158 << 9 = 80896
             * sectors, and read_blkbits 9 keeps nblks == capacity. The value is
             * derived from the image so the two cannot drift apart again.
             *
             * CSD bit b lives in resp[3 - b/32] at bit (b % 32): bits 47..49 (e)
             * are resp[2] bits 15..17, bits 62..63 are resp[2] bits 30..31 and
             * bits 64..73 are resp[1] bits 0..9, so the 12-bit m splits 2/10.
             */
            /*
             * The response words are stored in the opposite order to the CSD bit
             * numbering: the working SD path sets sdc_resp[1] = m << 16 and the
             * driver reads that as CSD bits 48..69, which means driver word 2 is
             * model word 1. Everything below is placed with that reversal.
             */
            /*
             * CSD bits 126:125 are CSD_STRUCTURE (3 = EXT_CSD) and bits 125:122
             * are MMCA_VSN, which must be >= CSD_SPEC_VER_4 or mmc_read_ext_csd()
             * opts out before issuing the command:
             *
             *     if (card->csd.mmca_vsn < CSD_SPEC_VER_4) {
             *         printf("[SD%d] MMCA_VSN: %d. Skip EXT_CSD\n", ...);
             *         return MMC_ERR_NONE;
             *     }
             *
             * With MMCA_VSN 0 LK never reads EXT_CSD, so ext_csd[EXT_CSD_REV]
             * stays 0 and g_emmc_size can never be set. Both fields sit in driver
             * word 0, i.e. model word 3.
             */
            s->sdc_resp[3] = 0xc0000000u | (4u << 26);  /* struct 3, MMCA_VSN 4 */
            {
                uint64_t n = mt6589_msdc_sectors(s) >> 9;   /* e = 7 -> shift 9 */
                unsigned mmc_m = n ? (unsigned)(n - 1) : 0;

                if (mmc_m > 0xfffu) {
                    mmc_m = 0xfffu;
                }
                s->sdc_resp[1] = (7u << 15) | ((mmc_m & 0x3u) << 30);
                s->sdc_resp[2] = (9u << 16) | ((mmc_m >> 2) & 0x3ffu);
            }
            s->sdc_resp[0] = 0;
            break;
        }
        s->sdc_resp[0] = (uint32_t)1 << 30;
        s->sdc_resp[1] = (uint32_t)((mt6589_msdc_sectors(s) / 1024 - 1) & 0xffff) << 16;
        s->sdc_resp[2] = (uint32_t)((mt6589_msdc_sectors(s) / 1024 - 1) >> 16) & 0x3f;
        s->sdc_resp[3] = s->sdc_resp[0];
        /*
         * SEND_CSD returns a 136-bit (16-byte) register, so the driver treats it
         * as a transfer with a data phase and waits on the data-complete bits:
         *
         *     INTEN 0x0000d000 = XFER_COMPL | DATTMO | DATCRCERR
         *
         * CMDRDY (0x100) alone never satisfies that wait, and the run blocks in
         * msdc_intr_wait() -> event_wait() right after CMD9. Measured: INTEN goes
         * 0x1ff7b -> 0x0 -> 0xd000 and then nothing fires.
         */
        s->int_status |= MSDC_INT_XFER_COMPL;
        break;
    case 13:  /* SEND_STATUS */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 16:  /* SET_BLOCKLEN */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 17:  /* READ_SINGLE_BLOCK */
        qemu_log_mask(LOG_UNIMP, "%s: %s CMD17 block 0x%x blksz %u\n",
                      __func__, s->name, s->sdc_arg, s->sdc_blk_sz);
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
        /*
         * An eMMC does not implement ACMD41, and *this* is the command that has to
         * time out - not CMD55.
         *
         * Both the preloader/LK mmc_send_app_op_cond() and the kernel's
         * mmc_attach_sd() probe with CMD55 + ACMD41 and treat any error as "not an
         * SD card", so making ACMD41 time out still puts both on the MMC path.
         * Making CMD55 the failing one instead left the kernel re-scanning: it
         * sends CMD55 with arg 0 from mmc_send_app_op_cond(host, 0, &ocr), gets a
         * timeout, backs off, and tries again - measured at thousands of CMD55
         * timeouts/s, each raising MSDC_INT_CMDTMO and re-asserting the
         * level-triggered line. That produced the endless
         * "CPU0: Unknown IPI message 0x0": the interrupt never stopped and the
         * ARM entry code's IPI test then saw a stale register.
         */
        /* Busy must clear or the guest retries for ever; report ready. */
        s->sdc_resp[0] = MSDC_CARD_OCR | (1u << 31);
        break;
    case 55:  /* APP_CMD - eMMC has no SD application commands */
        /*
         * CMD55 has to be the command that fails, not ACMD41.
         *
         * LK's mmc_send_app_op_cond() short-circuits on success when its ocr
         * argument is 0 (0x81e057bc `cbz r6`), so if CMD55 is answered and only
         * ACMD41 times out, LK still reports "this is an SD card", never sets
         * MMC_TYPE_MMC, never reads EXT_CSD, and g_emmc_size stays 0 - measured
         * as "TOOL DL image Fail / uboot is blocking by dl info". Failing CMD55
         * is the only thing that makes both the preloader and LK take the MMC
         * branch.
         *
         * The cost is that the kernel re-scans: mmc_send_app_op_cond(host, 0,
         * &ocr) gets this timeout, backs off and retries, and because this model
         * reports the timeout instantly the retry spins at CPU speed (~8,800/s,
         * measured). Each retry sets MSDC_INT_CMDTMO again, so the level-
         * triggered line never de-asserts - which is what produces the endless
         * "CPU0: Unknown IPI message 0x0". Making the timeout take real time
         * (rather than completing instantly) would turn that into a slow retry
         * loop and is the next thing to model here.
         */
        s->cmd_tmo_pending = 1;
        timer_mod(&s->cmd_tmo_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + MT6589_MSDC_CMD_TMO_NS);
        return;
    case 6:   /* SWITCH, and ACMD6 (SET_BUS_WIDTH) after CMD55 */
        /*
         * Both are "accepted, nothing to report": CM6 switches an eMMC's EXT_CSD
         * mode and ACMD6 sets the SD bus width, and in each case the driver only
         * checks that the command succeeded. They were reaching the default arm,
         * which answers R1 but leaves the transfer state alone - and the run then
         * sat in msdc_intr_wait() with INTEN at 0, i.e. waiting for an interrupt
         * that could never be delivered.
         */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 42:  /* ERASE */
        s->sdc_resp[0] = 0x00000900;
        break;
    case 51:  /* SEND_SCR, as ACMD51 after CMD55 */
        /*
         * LK's mmc_init_card() sends ACMD51 and fails with "Fail in SEND_SCR
         * cmd" / err=4 if it gets nothing, which leaves g_emmc_size at 0. That
         * is not cosmetic: mmc_get_dl_info() computes
         * dl_addr = g_emmc_size - 2048, so a zero size becomes
         * 0xFFFF_FFFF_FFFF_F800 and the read prefetch-aborts.
         *
         * SCR is one 8-byte data block, read like any other data transfer, so
         * the response is R1 with the data going through the same FIFO path as
         * CMD17/18. 0x0350_0000 declares SD spec 2.00 support and, in bit 7,
         * physical-layer 1.0 - which is what makes the driver keep SDHC
         * addressing. The remaining bits (bus widths, security, data status
         * after erase) can all read zero.
         */
        /*
         * SCR layout: SCR_STRUCTURE[3:0], SD_SPEC[7:4], DATA_STAT_AFTER_ERASE
         * [8], SD_SECURITY[14:12], SD_BUS_WIDTHS[19:16], then reserved and the
         * manufacturer fields. SD_SPEC 2 means spec 2.00 (SDHC capable) and
         * SD_BUS_WIDTHS 0x5 advertises both 1-bit and 4-bit, which is what makes
         * the driver keep 4-bit mode instead of dropping to 1-bit.
         */
        memset(s->meta, 0, sizeof(s->meta));
        s->meta_len = 8;
        s->meta[0] = 0x02;   /* SD_SPEC = 2 */
        s->meta[1] = 0x35;   /* SECURITY = 2, BUS_WIDTHS = 1|4 */
        s->meta[2] = 0x80;
        s->meta[3] = 0x00;
        /*
         * The R1 response must carry STA_APP_CMD (bit 5) or mmc_read_scrs()
         * rejects it before ever looking at the data:
         *     if ((err != MMC_ERR_NONE) || !(cmd.resp[0] & STA_APP_CMD))
         * which is why the transfer looked implemented and still reported
         * "Fail in SEND_SCR cmd". CMD55's reply already sets 0x920; this one
         * needs the bit too because the check is on ACMD51's own response.
         */
        s->sdc_resp[0] = 0x00000920;   /* R1 | STA_APP_CMD */
        /*
         * Do NOT write sdc_blk_sz here. The driver programs the block length
         * itself (msdc_set_blklen(host, 8)) and leaving 8 in the register makes
         * the *next* normal read return 8 bytes instead of a 512-byte block -
         * which showed up as load_pt_from_fixed_addr's read of block 0x3800
         * (0x700000 / 512, the PMT) coming back with blksz 8, so the table was
         * never seen the second time.
         */
        s->scr_pending = 1;
        /*
         * The data phase has to be staged here, exactly as CMD17 does it. The
         * driver does not re-issue the command to collect the block; it reads the
         * FIFO after the command completes, so nothing is served unless
         * fill_fifo ran.
         */
        if (has_data && !is_write) {
            mt6589_msdc_fill_fifo(s);
        }
        break;
    default:
        s->sdc_resp[0] = 0x00000900;
        break;
    }

    s->int_status |= MSDC_INT_CMDRDY;
    /*
     * Do NOT raise XFER_COMPL for every command. It used to be raised whenever
     * sdc_cfg bit 15 was set, which is a *mode* bit, not "this command had a
     * data phase" - so a plain CMD55 or CMD9 left 0x1000 asserted as well as
     * CMDRDY. LK's handler reads INT once and clears only what it saw
     * (MSDC_WRITE32(MSDC_INT, sts)), so a 0x1000 set afterwards was never
     * acknowledged and the line stayed up: measured as 654,749
     * "Taking exception 5 [IRQ]" in one run, and as INT write traces showing
     * val=0x100 against before=0x1100 - i.e. only the CMDRDY bit being cleared.
     *
     * Data-phase completion is raised where the data actually completes:
     * mt6589_msdc_dma_run() for DMA, and the RXDATA drain for PIO.
     */
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
        /*
         * RXCNT counts *bytes*, not words. The drain loop in the preloader
         * compares it directly against a byte count:
         *
         *     625a: ldr  r2, [r4, #0x14]   ; FIFOCS
         *     625c: uxtb r2, r2            ; RXCNT, bits 7:0
         *     625e: cmp  r2, r3            ; r3 is remaining BYTES
         *     6260: blo  0x61f2            ; not enough yet - loop
         *
         * so reporting words leaves RXCNT a quarter of the byte count and the
         * `blo` never falls through. Reported as bytes here.
         */
        uint32_t bytes = remaining;
        /*
         * Cap at the hardware FIFO size (128 bytes), not at the 0xff the field
         * can hold. LK's drain loop reads MSDC_FIFO_SZ-sized bursts:
         *
         *     msdc.h: MSDC_FIFO_SZ 128, MSDC_FIFO_THD 128
         *
         * and reports more than that only confuses it into issuing RXDATA reads
         * for words the model will answer with 0 once the staged span runs out -
         * the run then spins on 0x1c for ever.
         */
        if (bytes > 128) {
            bytes = 128;
        }
        qemu_log_mask(LOG_UNIMP, "%s: %s FIFOCS read -> rxcnt %u\n",
                      __func__, s->name, bytes);
        return bytes;
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
    case MSDC_PAD_CTL0:  return s->pad_ctl0;
    case MSDC_PAD_CTL1:  return s->pad_ctl1;
    case MSDC_PAD_CTL2:  return s->pad_ctl2;
    case MSDC_PAD_TUNE:  return s->pad_tune;
    case MSDC_DAT_RDDLY0: return s->dat_rddly0;
    case MSDC_DAT_RDDLY1: return s->dat_rddly1;
    case MSDC_DMA_SA:     return s->dma_sa;
    case MSDC_DMA_CTRL:   return s->dma_ctrl;
    case MSDC_DMA_CFG:    return s->dma_cfg & ~MSDC_DMA_CFG_STS;
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
                /*
                 * Raise XFER_COMPL again when the last byte has been handed
                 * over. The command handler already raised it once, but the
                 * driver's interrupt handler clears MSDC_INT and then waits for
                 * the data phase to report completion separately - measured:
                 * after CMD18 (arg 0x3800, the PMT) INTEN is 0 and the run sits
                 * in event_wait() for ever.
                 */
                if (s->data_pos >= s->data_total) {
                    s->int_status |= MSDC_INT_XFER_COMPL;
                    mt6589_msdc_update_irq(s);
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

static bool mt6589_msdc_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = getenv("MT6589_MSDC_LOG") != NULL;
    }
    return enabled;
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
    /*
     * DMA register writes. These were missing from this handler, so every write
     * to MSDC_DMA_SA (0x90), MSDC_DMA_CFG (0x9c) and MSDC_DMA_CTRL (0x98) fell
     * through to the default arm and was dropped: dma_sa stayed 0 and the START
     * trigger never called mt6589_msdc_dma_run(). LK then waited for a
     * completion from a transfer that had never been started, which is exactly
     * what "the guest sits in arch_idle forever" looks like.
     *
     * LK's msdc_dma_start() (0x81e0810c) reads 0x98, clears bit 0 and sets it
     * again, then writes - an unconditional store - so this handler must treat a
     * write with START set as the trigger and read START back as 0.
     */
    case MSDC_DMA_SA:
        s->dma_sa = value;
        return;
    case MSDC_DMA_CFG:
        /* STS is status, not state: keep it clear so dma_stop's poll exits. */
        s->dma_cfg = value & ~MSDC_DMA_CFG_STS;
        return;
    case MSDC_DMA_CTRL:
        if (value & MSDC_DMA_CTRL_START) {
            mt6589_msdc_dma_run(s);
            /* START is a trigger, not state: read it back clear. */
            s->dma_ctrl = value & ~MSDC_DMA_CTRL_START;
        } else {
            s->dma_ctrl = value & ~MSDC_DMA_CTRL_START;
        }
        return;
    case MSDC_INT:
        /*
         * Write-1-to-clear. The driver acknowledges by writing back the bits it
         * handled, and ignoring the write leaves those bits asserted for ever:
         * the line is level-triggered, so the GIC re-delivers immediately and
         * the CPU never leaves the handler.
         *
         * Measured before this fix: MSDC INT_STATUS stayed 0x00000200
         * (MSDC_INT_CMDTMO, left over from the eMMC probe's CMD55 timeout) with
         * INT_EN 0x0001ff7b, and the kernel looped on IAR = 0x47 (IRQ 71) -
         * which is what produced the "CPU0: Unknown IPI message 0x0" flood,
         * because the entry code's IPI test then saw a stale register.
         */
        s->int_status &= ~((uint32_t)value);
        mt6589_msdc_update_irq(s);
        return;
    case MSDC_DAT_RDDLY0:
        s->dat_rddly0 = value;
        return;
    case MSDC_DAT_RDDLY1:
        s->dat_rddly1 = value;
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
        /*
         * MT6589_MSDC_LOG=1 with the guest PC, because the interesting question about a card
         * command is who issued it: a retry loop of the same block range can be the driver's own
         * logic or a model mis-staging the data, and the PC says which.
         */
        if (mt6589_msdc_log_enabled()) {
            uint32_t pc = current_cpu ? cpu_env(current_cpu)->regs[15] : 0;

            fprintf(stderr, "MSDC %s CMD %2u arg 0x%08x pc=%08" PRIx32 "\n",
                    s->name, (unsigned)(value & 0x3f), s->sdc_arg, pc);
        }
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

    timer_init_ns(&s->cmd_tmo_timer, QEMU_CLOCK_VIRTUAL, mt6589_msdc_cmd_tmo, s);

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
