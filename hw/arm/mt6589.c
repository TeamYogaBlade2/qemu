/*
 * MediaTek MT6589 SoC
 *
 * Copyright (c) 2026 TeamYogaBlade2
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * The design of this file follows one rule: the register map comes from the
 * datasheet (tools/extract_regs.py), and the behaviour comes from what the
 * guest is measured to poll. A register that exists in the datasheet but whose
 * behaviour is not modelled reads its documented reset value and ignores
 * writes, which is what the real hardware does for a block that has not been
 * enabled - and it is better than returning 0, which the guest reads as "block
 * not present" and acts on.
 *
 * TODO, in boot-chain order. Each needs the guest to get further than the
 * previous stage before its behaviour can be measured against anything:
 *   - PWRAP + MT6320 PMIC (CHRDET/PWRKEY decide preloader vs power-off)
 *   - MSDC (card enumeration; INTEN gating matters, see the Renode notes)
 *   - M4U, SMI/LARB (the display pipeline will not bind without them)
 *   - DISP: OVL/RDMA/COLOR/DSI
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/arm/mt6589.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/arm/bsa.h"
#include "target/arm/gtimer.h"
#include "target/arm/arm-powerctl.h"
#include "hw/core/qdev-properties.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "system/system.h"
#include "hw/core/loader.h"
#include "qemu/units.h"
#include "target/arm/cpu.h"

/*
 * The GPT tick. The datasheet's counters are clocked from the 13 MHz system
 * clock divided by CLK[3:0], but nothing in the guest polls them often enough to
 * derive a compare match on access alone: the kernel arms a timer, stops
 * touching the block, and expects the interrupt. So the deadline is computed
 * from the programmed period and a timer is armed for it, which is the only way
 * the interrupt can arrive while the guest is idle.
 *
 * Renode's port got this wrong first (it advanced only on access) and the guest
 * stalled; this is the QEMU equivalent done up front rather than rediscovered.
 */
/*
 * SYS is 13 MHz, RTC is 32768 Hz.
 *
 * There is a known disagreement between these rates and what LK assumes for its
 * own tick, and it is left visible rather than papered over:
 *
 *   - LK selects the SYS source (mt_gpt.h: GPT_CLK_SYS == 0, and
 *     platform_set_periodic_timer() writes CON without the RTC bit) but computes
 *     the compare value as `TIMER_TICK_RATE(32768) * interval / 1000`, i.e. it
 *     expects the counter to advance at 32768 Hz. At 13 MHz a 10 ms tick expires
 *     in 25 us, which measures as ~21,000 interrupts/s (426,816 exceptions in a
 *     20 s run).
 *   - Setting SYS to 32768 to match LK stops that storm but makes the
 *     preloader, which uses the same GPT with the same source for its delays,
 *     about 400x too slow: boot stalls at rtc_first_boot_init after 1.4 KB of
 *     output instead of reaching LK.
 *
 * So neither value is right for both users, which means the model's clock model
 * is too simple rather than that one number is wrong: the GPT SYS source is
 * presumably a real 13 MHz clock and LK's 32768 is an error in its own tick
 * arithmetic (or it expects a divider this model does not apply). Resolving it
 * needs the real GPT clock tree, which is not established yet.
 *
 * 13 MHz is kept because it is the only value under which the whole chain runs;
 * the storm it causes is real and documented in docs/qemu/lk-irq-and-dma.md.
 */
#define MT6589_GPT_SYS_HZ 13000000ull
#define MT6589_GPT_RTC_HZ 32768ull

static uint64_t mt6589_gpt_channel_hz(Mt6589State *s, int n)
{
    uint32_t clk = s->gpt_chan[n].clk;
    uint64_t base = (clk & MT6589_GPT_CLK_SRC_RTC) ? MT6589_GPT_RTC_HZ
                                                   : MT6589_GPT_SYS_HZ;
    uint32_t div = (clk & MT6589_GPT_CLK_DIV_MASK) + 1;

    return base / div;
}

/* Current counter value, derived from elapsed emulated time. */
static uint32_t mt6589_gpt_count_raw(Mt6589State *s, int n)
{
    int64_t elapsed = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                      s->gpt_chan[n].start_ns;
    uint64_t hz = mt6589_gpt_channel_hz(s, n);

    if (elapsed < 0) {
        return 0;
    }
    return (uint32_t)((elapsed * hz) / 1000000000ull);
}

static uint32_t mt6589_gpt_count(Mt6589State *s, int n)
{
    /*
     * A real GPT advances only while CON.EN is set; with the channel disabled the
     * counter holds whatever it had reached, and CON.CLR drives it back to 0.
     *
     * This matters because the kernel starts a channel with a CLR/EN handshake:
     *
     *     writel(GPT_CON_CLR, GPT_CON);          @ bit 1
     *     while (readl(GPT_COUNT))               @ wait for the counter to be 0
     *         ;
     *     writel(GPT_CON_EN, GPT_CON);           @ bit 0
     *
     * (0xc001b4d8 in the kernel image: `ldr r3,[r2]` / `cmp r3,#0` / `bne`).
     * Deriving the count from the clock regardless of EN never reads 0 there -
     * measured counting 0x04d8e20e, 0x06c8d6f2, 0x08b8c7bb, 0x0aa8bca6 on
     * successive samples - so the boot spun for ever at 0xc001b4dc.
     */
    if (!(s->gpt_chan[n].con & MT6589_GPT_CON_EN)) {
        return s->gpt_chan[n].frozen_count;
    }
    return mt6589_gpt_count_raw(s, n);
}

static void mt6589_gpt_update_irq(Mt6589State *s)
{
    /* The GIC line is level-triggered, so it is the AND of status and enable.
     * Driving it on status alone makes an acknowledged interrupt re-deliver for
     * ever, which is exactly the Renode bug that cost a day of LK debugging -
     * see docs/qemu/gpt.md. */
    qemu_set_irq(s->gpt_irq, (s->gpt_irqsta & s->gpt_irqen) != 0);
}

static void mt6589_gpt_rearm(Mt6589State *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t soonest = INT64_MAX;

    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        uint32_t con = s->gpt_chan[n].con;
        uint32_t compare = s->gpt_chan[n].compare;
        uint64_t hz;

        if (!(con & MT6589_GPT_CON_EN) || compare == 0) {
            continue;
        }
        hz = mt6589_gpt_channel_hz(s, n);
        if (hz == 0) {
            continue;
        }
        int64_t due = s->gpt_chan[n].start_ns +
                      (int64_t)((compare * 1000000000ull) / hz);
        soonest = MIN(soonest, due);
    }
    if (soonest == INT64_MAX) {
        timer_del(&s->gpt_tick);
        return;
    }
    if (soonest <= now) {
        soonest = now + 1;
    }
    timer_mod(&s->gpt_tick, soonest);
}

static void mt6589_gpt_tick(void *opaque)
{
    Mt6589State *s = opaque;

    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        uint32_t con = s->gpt_chan[n].con;
        uint32_t compare = s->gpt_chan[n].compare;

        if (!(con & MT6589_GPT_CON_EN) || compare == 0) {
            continue;
        }
        if (mt6589_gpt_count(s, n) < compare) {
            continue;
        }
        /* Compare reached: raise the channel's status bit. */
        s->gpt_irqsta |= MT6589_GPT_IRQ_BIT(n);

        switch ((con & MT6589_GPT_CON_MODE_MASK) >> MT6589_GPT_CON_MODE_SHIFT) {
        case MT6589_GPT_MODE_ONESHOT:
            /* Stop the counter at the compare value. */
            s->gpt_chan[n].con &= ~MT6589_GPT_CON_EN;
            break;
        case MT6589_GPT_MODE_REPEAT:
            /*
             * Reload the counter for the next period, but leave the status bit
             * set: the guest acknowledges it by writing GPT_IRQACK, and clearing
             * it here means the interrupt can be missed entirely.
             *
             * This is the clock event's mode - mt_gpt.c setup_clkevt() calls
             * setup_gpt_dev_locked(dev, GPT_REPEAT, ...) - so clearing it made
             * every tick vanish and left the kernel stuck in calibrate_delay()
             * waiting for jiffies that could never advance.
             */
            s->gpt_chan[n].start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            break;
        default:
            /* KEEP-GO and FREERUN keep counting past the compare value. */
            break;
        }
    }
    mt6589_gpt_update_irq(s);
    mt6589_gpt_rearm(s);
}

static uint64_t mt6589_gpt_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589State *s = opaque;

    switch (offset) {
    case MT6589_GPT_IRQEN:
        return s->gpt_irqen;
    case MT6589_GPT_IRQSTA:
        return s->gpt_irqsta;
    case MT6589_GPT_IRQACK:
        /* Write-only on hardware; reading back 0 is harmless and honest. */
        return 0;
    }
    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        hwaddr base = MT6589_GPT_CHAN(n);

        if (offset < base || offset >= base + 0x10) {
            continue;
        }
        switch (offset - base) {
        case MT6589_GPT_CON:
            return s->gpt_chan[n].con;
        case MT6589_GPT_CLK:
            return s->gpt_chan[n].clk;
        case MT6589_GPT_COUNT:
            return mt6589_gpt_count(s, n);
        case MT6589_GPT_COMPARE:
            return s->gpt_chan[n].compare;
        }
    }
    qemu_log_mask(LOG_UNIMP, "%s: read of unimplemented offset 0x%" HWADDR_PRIx
                  "\n", __func__, offset);
    return 0;
}

static void mt6589_gpt_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Mt6589State *s = opaque;

    switch (offset) {
    case MT6589_GPT_IRQEN:
        s->gpt_irqen = value;
        mt6589_gpt_update_irq(s);
        return;
    case MT6589_GPT_IRQSTA:
        /* Status is cleared by writing 1 to IRQACK, not by writing status. */
        return;
    case MT6589_GPT_IRQACK:
        s->gpt_irqsta &= ~((uint32_t)value);
        mt6589_gpt_update_irq(s);
        return;
    }
    for (int n = 0; n < MT6589_GPT_NUM_CHANNELS; n++) {
        hwaddr base = MT6589_GPT_CHAN(n);

        if (offset < base || offset >= base + 0x10) {
            continue;
        }
        switch (offset - base) {
        case MT6589_GPT_CON:
            /* CLR reloads the counter; EN starts it. Both matter: the guest
             * writes CLR then EN, and treating EN alone as the start makes the
             * first period shorter than programmed. */
            if (value & MT6589_GPT_CON_CLR) {
                s->gpt_chan[n].start_ns =
                    qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                s->gpt_chan[n].frozen_count = 0;
                s->gpt_irqsta &= ~MT6589_GPT_IRQ_BIT(n);
            } else if ((value & MT6589_GPT_CON_EN) &&
                       !(s->gpt_chan[n].con & MT6589_GPT_CON_EN)) {
                /*
                 * Enabling restarts the counter from the value it froze at, not
                 * from wherever start_ns happens to point. Without this, a channel
                 * that was disabled long ago resumes with a count far past its
                 * compare and matches immediately: measured, GPT1 produced its
                 * status bit the instant the kernel armed it, before the driver
                 * could be ready, and then sat unacknowledged for ever.
                 */
                uint64_t hz = mt6589_gpt_channel_hz(s, n);

                if (hz) {
                    s->gpt_chan[n].start_ns =
                        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                        (int64_t)(((uint64_t)s->gpt_chan[n].frozen_count *
                                   1000000000ull) / hz);
                }
            } else if (!(value & MT6589_GPT_CON_EN)) {
                /*
                 * Stopping a channel also drops its request. GPT_IRQSTA is sticky
                 * and normally only GPT_IRQACK clears it, but leaving a status bit
                 * set on a channel the guest has just disabled is what livelocked
                 * this boot: measured, the kernel stopped GPT1 (CON=0) while
                 * IRQSTA bit 0 stayed set and IRQEN bit 0 was still enabled, so the
                 * level-triggered line stayed asserted, the CPU sat in the IRQ
                 * vector at 0xffff0018, and no printk ever appeared again because
                 * nothing in the handler acknowledged a timer it had just stopped.
                 */
                s->gpt_irqsta &= ~MT6589_GPT_IRQ_BIT(n);
            } else if ((s->gpt_chan[n].con & MT6589_GPT_CON_EN) &&
                       !(value & MT6589_GPT_CON_EN)) {
                /* Disabling freezes the counter where it stands. */
                s->gpt_chan[n].frozen_count = mt6589_gpt_count_raw(s, n);
            }
            s->gpt_chan[n].con = value & ~MT6589_GPT_CON_CLR;
            mt6589_gpt_update_irq(s);
            mt6589_gpt_rearm(s);
            return;
        case MT6589_GPT_CLK:
            s->gpt_chan[n].clk = value;
            mt6589_gpt_rearm(s);
            return;
        case MT6589_GPT_COUNT:
            /* Writing the counter restarts the period from that value, which is
             * how the kernel sets an absolute deadline. */
            s->gpt_chan[n].start_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) -
                (int64_t)(((uint64_t)value * 1000000000ull) /
                          mt6589_gpt_channel_hz(s, n));
            mt6589_gpt_rearm(s);
            return;
        case MT6589_GPT_COMPARE:
            s->gpt_chan[n].compare = value;
            mt6589_gpt_rearm(s);
            return;
        }
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to unimplemented offset "
                  "0x%" HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_gpt_ops = {
    .read = mt6589_gpt_read,
    .write = mt6589_gpt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};


/*
 * SEJ - security engine.
 *
 * What is known, from the boot ROM's own code at 0xd060 rather than from any
 * document: 16 bytes go into 0x1000_a010..0x1000_a01c, bit 0 of 0x1000_a008
 * starts an operation, bit 17 of the same register means done, and 16 bytes of
 * result come back from a DRAM address the caller supplies.
 *
 * What is NOT known is the transform. The datasheet has no register chapter for
 * SEJ and the downstream tree has no driver. So this models the interface
 * faithfully - a real caller's polling will complete and its result buffer will
 * be filled - and performs a pass-through, logging every block. That is the
 * honest position: the ROM gets through, and any later stage that depends on the
 * *value* of a SEJ result fails with a logged block rather than with a mysterious
 * wrong hash.
 *
 * If a fuller datasheet or a hardware trace of this address appears, the
 * transform goes here and nothing else changes.
 */
static uint64_t mt6589_sej_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589State *s = opaque;

    switch (offset) {
    case MT6589_SEJ_STATUS:
        return s->sej_status;
    case MT6589_SEJ_STATUS + 4:
        return 0;
    }
    qemu_log_mask(LOG_UNIMP, "%s: read offset 0x%" HWADDR_PRIx "\n",
                  __func__, offset);
    return 0;
}

static void mt6589_sej_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    Mt6589State *s = opaque;

    if (offset == MT6589_SEJ_STATUS) {
        if (value & MT6589_SEJ_STATUS_START) {
            /*
             * Report completion at once. The guest polls, it does not measure
             * elapsed time, so a synchronous completion is faithful to what is
             * observable - and no transform is performed because none is known.
             */
            s->sej_status |= MT6589_SEJ_STATUS_DONE;
            qemu_log_mask(LOG_UNIMP,
                          "%s: SEJ operation reported done WITHOUT performing it "
                          "(transform unknown)\n", __func__);
        }
        return;
    }
    /* Key/IV/result registers beyond the control word are plain storage. Their
     * layout is not documented and no transform is performed, so a store is kept
     * (so it reads back) and nothing is derived from it. */
    if (offset >= 0x10 && offset < 0x100) {
        s->sej_data[(offset - 0x10) / 4] = value;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to offset 0x%"
                  HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_sej_ops = {
    .read = mt6589_sej_read,
    .write = mt6589_sej_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Chip ID block, 0x0800_0000.
 *
 * The datasheet's memory map lists a four-word device there (0x0800_0000 -
 * 0x0800_000C) and table 14-2 gives its contents:
 *
 *     0x0800_0000  CHIP_ID   0x8a00_6583  main chip id 0x6583, sub 0x8a00
 *     0x0800_0004  CHIP_VER  0x0000_ca00  HW ID 0xca00, SW ID 0x0000
 *
 * LK reaches it through get_devinfo_with_index(20) - its devinfo address table
 * holds the twenty EFUSE words at 0x1000_9000.. and then this block - and ATAG
 * construction reads all twenty-one entries. Without the mapping LK takes a data
 * abort at 0x81e2e340 (`ldr r0,[r3]` with r3 = 0x08000000) while building
 * tag_devinfo_data, i.e. after it has already accepted the kernel.
 */
static uint64_t mt6589_chipid_read(void *opaque, hwaddr offset, unsigned size)
{
    switch (offset) {
    case 0x00:
        return 0x8a006583u;
    case 0x04:
        return 0x0000ca00u;
    default:
        /* 0x0800_0008 and 0x0800_000C are not documented. */
        return 0;
    }
}

static void mt6589_chipid_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to offset 0x%"
                  HWADDR_PRIx "\n", __func__, value, offset);
}

/*
 * MCUSYS slave-CPU release, 0x1000_2030..0x1000_204F.
 *
 * These addresses are inside the window the datasheet calls sramrom_reg
 * (0x1000_2000, which documents only SEC_DOMAIN/SEC_VIO_STAT/SEC_VIO_ADDR at
 * +0x00/+0x10/+0x14) and are not documented there, yet the ported kernel uses
 * them to start its secondaries - mt-smp.c:
 *
 *     #define SLAVE_JUMP_REG   0xF0002034   (IO_VIRT_TO_PHYS -> 0x10002034)
 *     #define SLAVE1_MAGIC_REG 0xF0002038
 *     #define SLAVE1_MAGIC_NUM 0x534C4131
 *
 * platform_smp_prepare_cpus() puts the secondary entry point in SLAVE_JUMP_REG,
 * then boot_secondary() writes a magic per CPU. They cannot live in the peri
 * catch-all because this machine also maps SRAMBOOT RAM over 0x1000_2000, which
 * swallowed them: instrumenting the catch-all showed the writes never arriving at
 * all. A small overlay with a positive priority fixes that without disturbing the
 * rest of SRAMBOOT.
 */
#define MT6589_MCUSYS_BASE   0x10002030
#define MT6589_MCUSYS_SIZE   0x20

static uint64_t mt6589_mcusys_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589State *s = opaque;

    if (offset == 0x04) {
        return s->mcusys_jump;
    }
    return 0;
}

static void mt6589_mcusys_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size)
{
    Mt6589State *s = opaque;

    if (offset == 0x04) {
        s->mcusys_jump = value;
        return;
    }
    if (offset == 0x08 || offset == 0x0c || offset == 0x10) {
        static const uint32_t magic[3] = { 0x534C4131u, 0x4C415332u, 0x41534C33u };
        int cpu = 1 + (offset - 0x08) / 4;

        if ((uint32_t)value == magic[cpu - 1]) {
            arm_set_cpu_on(cpu, s->mcusys_jump, 0, 1, false);
        }
        return;
    }
}

static const MemoryRegionOps mt6589_mcusys_ops = {
    .read = mt6589_mcusys_read,
    .write = mt6589_mcusys_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static const MemoryRegionOps mt6589_chipid_ops = {
    .read = mt6589_chipid_read,
    .write = mt6589_chipid_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/*
 * Peripheral-region catch-all. The datasheet documents a register for every
 * block in 0x1000_0000..0x14FF_FFFF; modelling behaviour for all of them is the
 * long-term job, but the guest must not see a bus error or a silent 0 for a
 * register that physically exists. Reads return the documented reset value
 * where the generated map knows it, otherwise 0; writes are ignored. Anything
 * logged here is a register whose behaviour is still missing.
 */
static bool mt6589_gpu_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        enabled = getenv("MT6589_GPU_LOG") != NULL;
    }
    return enabled;
}

/*
 * MT6589_LATE_LOG=<seconds> traces peripheral accesses after that point of guest time,
 * once per distinct (offset, pc) pair.
 *
 * This exists because "the boot stops here" does not say where the kernel is waiting,
 * and a task asleep in an interruptible wait is invisible to the hung-task detector: it
 * never prints, and its stack cannot be reached without knowing the task_struct layout.
 * What such a loop does do is wake on a timer and touch the same register every time, so
 * tracing only the accesses after the boot goes quiet shows the wait directly - offset,
 * and the guest PC, which maps to the function.
 */
static bool mt6589_late_log(hwaddr phys, uint32_t *pc_out, uint32_t *lr_out)
{
    static int64_t threshold_ns = -2;
    /*
     * Sized generously: with 512 entries this filled up during a busy boot and then
     * logged every access again, which looks like "the guest is very active" when it
     * only means the dedup table overflowed.
     */
    static uint32_t seen_off[8192];
    static uint32_t seen_pc[8192];
    static unsigned nseen;
    const char *env = getenv("MT6589_LATE_LOG");
    uint32_t pc;

    if (!env) {
        return false;
    }
    if (threshold_ns == -2) {
        threshold_ns = (int64_t)(g_ascii_strtod(env, NULL) * 1e9);
    }
    if (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) < threshold_ns) {
        return false;
    }
    pc = current_cpu ? cpu_env(current_cpu)->regs[15] : 0;
    *pc_out = pc;
    /*
     * The return address is what identifies the CALLER, and the caller is the thing
     * worth knowing: the PC says which register-access helper ran, the LR says which
     * driver function is driving the sequence and therefore where the wait lives.
     */
    *lr_out = current_cpu ? cpu_env(current_cpu)->regs[14] : 0;
    for (unsigned i = 0; i < nseen; i++) {
        if (seen_off[i] == (uint32_t)phys && seen_pc[i] == pc) {
            return false;
        }
    }
    if (nseen < ARRAY_SIZE(seen_off)) {
        seen_off[nseen] = (uint32_t)phys;
        seen_pc[nseen] = pc;
        nseen++;
    }
    return true;
}

/*
 * The peripheral region is split in two so the preloader's SRAM at 0x1200_0000 is not
 * covered, and the high half therefore starts at 0x1220_0000 rather than 0x1200_0000.
 * Both halves shared one set of handlers, which made every offset ambiguous: an access
 * to 0x1100_506C (FHCTL) and one to 0x1300_506C (the GPU) both arrived as 0x0100_506C.
 * That ambiguity produced two wrong readings - a GPU trace that could never match, and
 * a "GPU register" that was really FHCTL.
 *
 * The high half now normalises its offsets to be relative to 0x1000_0000 like the low
 * half, so an offset in these handlers always means the same physical address.
 */
#define MT6589_PERI_HI_SHIFT  (MT6589_PRERAM_END - 0x10000000)

/* The GPU/SGX register window, 0x1300_0000 for 64 KiB. */
#define MT6589_GPU_WINDOW     0x03000000
#define MT6589_GPU_SIZE       0x00010000

static uint64_t mt6589_peri_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589State *s = opaque;

    /*
     * MT6589_MFG_LOG=1 traces the GPU's register window (0x1300_0000, i.e. this
     * region's offset 0x0300_0000). The GPU is out of scope here, but what the
     * driver does to it is not: the boot currently stops with everything asleep, and
     * the only way to know whether that is a poll this model answers wrongly or a
     * wait for an interrupt nobody raises is to see the traffic.
     */
    if (mt6589_gpu_log_enabled() &&
        offset >= MT6589_GPU_WINDOW && offset < MT6589_GPU_WINDOW + MT6589_GPU_SIZE) {
        /*
         * The guest PC is included because a bare offset does not say WHERE the access
         * came from, and a register the driver polls in a loop is exactly the case where
         * the caller has to be found: searching the kernel image for the window's base
         * address finds nothing, because ARM builds it from a movw/movt pair rather than
         * a literal.
         */
        uint32_t pc = current_cpu ? cpu_env(current_cpu)->regs[15] : 0;

        fprintf(stderr, "GPU R %05" HWADDR_PRIx " pc=%08" PRIx32 "\n",
                offset - MT6589_GPU_WINDOW, pc);
    }

    {
        uint32_t pc = 0, lr = 0;

        if (mt6589_late_log(0x10000000 + offset, &pc, &lr)) {
            fprintf(stderr, "LATE R phys=%08" HWADDR_PRIx " pc=%08" PRIx32
                    " lr=%08" PRIx32 "\n", 0x10000000 + offset, pc, lr);
        }
    }

    /*
     * Poll detector. A driver that waits for a status bit this model never sets
     * spins on one address for ever, and the guest simply goes idle with no
     * output - which is exactly how the boot stopped after kthreadd's fork. Track
     * consecutive reads of the same offset and say so once it looks like a poll,
     * so the missing register names itself instead of having to be bisected.
     */
    {
        static hwaddr last;
        static unsigned long long streak;
        static int reported;

        if (offset == last) {
            if (++streak == 200000 && !reported) {
                fprintf(stderr, "PERI poll: offset %05" HWADDR_PRIx
                        " read %llu times in a row (catch-all returns 0)\n",
                        offset, streak);
                reported++;
            }
        } else {
            last = offset;
            streak = 1;
        }
    }
    /*
     * SPM (0x1000_6000): MTCMOS power domains.
     *
     * The kernel's SPM driver reads these before anything else, and a wrong
     * answer is fatal rather than cosmetic:
     *
     *   test_spm_gpu_power_on()   mt_spm_mtcmos.c:947
     *       val = spm_read(SPM_MFG_PWR_CON);
     *       BUG_ON(!(val & PWR_ISO));        <- panics if the bit is clear
     *
     * Measured, that is what happened: "Kernel BUG at c0031908", "PC is at
     * test_spm_gpu_power_on+0x15c", then "Kernel panic - not syncing: Fatal
     * exception" and a watchdog-reset loop. With the whole peripheral window
     * returning 0 the isolation bit was clear, so the first power-domain self
     * test took the BUG path.
     *
     * So the domains read as up: PWR_STATUS and PWR_STATUS_S report every domain
     * powered - what the boot chain leaves behind, and what makes that self test
     * take its "already on, return 1" branch - and each domain's PWR_CON shows
     * isolation plus both power-on bits. The GPU is out of scope; the power
     * controller is not, because the kernel probes it.
     */
    if (offset >= 0x6200 && offset < 0x62a0) {
        /*
         * PWR_ISO (bit 1) | PWR_ON (bit 2) | PWR_ON_S (bit 3), plus the SRAM
         * acknowledge bits (12:15) which hardware raises once the SRAM power-down
         * requested in SRAM_PDN (bits 8:11) has completed.
         *
         * The ack has to track SRAM_PDN rather than being always-on: the
         * power-down path waits for it to become set
         *
         *     spm_write(PWR_CON, spm_read(PWR_CON) | SRAM_PDN);
         *     while ((spm_read(PWR_CON) & ISP_SRAM_ACK) != ISP_SRAM_ACK) { }
         *
         * while the power-up path waits for it to clear, so a stuck ack would
         * hang the opposite direction. Without any ack at all, measured, PID 1
         * spun for ever in that loop at "[mt_subsys_init]SYS_ISP, change state".
         */
        uint32_t stored = s->spm_pwr_con[(offset - 0x6200) / 4];

        /*
         * Bits 12:15 are the SRAM acknowledge, driven by hardware from SRAM_PDN -
         * they are not storage, and returning whatever the guest last wrote there
         * keeps them set for ever. That mattered because the power-on path reads the
         * register, clears only SRAM_PDN and writes the result back, so the ack bits
         * it had just read came back with it:
         *
         *     c0030c3c  ldr r2, [r3, #0x230]     ; read, ack set
         *     c0030c40  bic r2, r2, #0xf00       ; clear SRAM_PDN only
         *     c0030c44  str r2, [r3, #0x230]     ; ack bits written back
         *     c0030c4c  ldr r2, [r3, #0x230]
         *     c0030c50  tst r2, #0xf000
         *     c0030c54  bne 0xc0030c4c           ; measured spinning here for ever
         *
         * (VENC's power-on, inside spm_mtcmos_ctrl_venc at 0xc0030ad8.)
         */
        return (stored & ~0xf000u) | 0xeu | ((stored & 0xf00u) ? 0xf000u : 0);
    }

    /*
     * INFRACFG AXI protection (INFRACFG_BASE = 0xF000_1000 -> 0x1000_1000, so these
     * are offsets 0x1200/0x1220/0x1228 here).
     *
     * PROT_STA1 reports which masters have actually gone idle behind the protection
     * PROT_EN asked for. Applying it immediately is the whole point of the model: the
     * MTCMOS power sequences poll the status to know when the bus is quiescent, and
     * with the status stuck at zero spm_mtcmos_ctrl_mfg() never returns - measured as
     * CPU0 spinning at 0xc0030f94 inside that function while the other three CPUs sat
     * in go_to_rgidle's wfi, which is what made the boot look hung rather than stalled.
     */
    switch (offset) {
    case 0x1200:
        return s->topaxi_si0_ctl;
    case 0x1220:
        return s->topaxi_prot_en;
    case 0x1228:
        return s->topaxi_prot_en;
    }

    /*
     * M4U at 0x1020_5000: global registers at +0x000, the L2 at +0x100, and one
     * register file per M4U instance at +0x200 and +0x800.
     *
     * The completion bits are what matter. Every TLB invalidation ends with a poll:
     *
     *     COM_WriteReg32(REG_MMUg_INVLD, F_MMUg_INV_ALL);
     *     while (!(m4uHw_get_field_by_mask(0, REG_L2_GDC_STATE,
     *                                      F_L2_GDC_ST_EVENT_MSK))) { }
     *
     * (m4u.c, m4u_invalid_tlb) - and with the whole block unmodelled that loop never
     * ends: measured, the guest clock froze at 2.473s with CPU2 spinning at 0xc04c9568
     * inside m4u_invalid_tlb while the other three CPUs sat in wfi.
     */
    if (offset >= 0x205000 && offset < 0x206000) {
        return s->m4u_regs[(offset - 0x205000) / 4];
    }
    if (offset == 0x660c || offset == 0x6610) {   /* SPM_PWR_STATUS(_S) */
        /*
         * The modem is reported off and everything else on. That is the state the
         * boot chain leaves behind, with one deliberate difference: MD1 has no
         * hardware behind it here, and reporting it as on makes
         * mt_subsys_init() try to power it *down*, which waits for an
         * acknowledge the absent modem can never give - measured, PID 1 went idle
         * after "[mt_subsys_init]SYS_MD1, change state: (1->0)" and jiffies
         * advanced to 33 s with no further output.
         */
        return s->spm_status;
    }
    /*
     * Keypad scan results. KP_MEM1..5 sit inside this window because the region
     * is the whole peripheral range and KPD has no model of its own; the read is
     * intercepted here rather than left to the catch-all so the value is right.
     *
     * No key is held on a headless emulated board, and the scan results are
     * active low - mtk_key.c's mtk_detect_key() treats a *clear* bit as pressed
     * (`din = DRV_Reg16(KP_MEM1 + (idx << 2)) & (1U << bit); if (!din) pressed`).
     * Returning 0 therefore reports all 72 scan codes as held, which is how the
     * preloader concluded "download keys are pressed" and dropped into emergency
     * download mode. All ones means nothing is pressed.
     */
    /*
     * offset is relative to this region, which is mapped at 0x1000_0000 - so the
     * KPD window is (MT6589_KPD_BASE - 0x10000000) + MT6589_KPD_MEM1, not the
     * absolute address. Comparing against the absolute address here silently
     * never matched, which is why the key registers appeared not to be read at
     * all while the guest was in fact reading them.
     */
    if (offset >= (MT6589_KPD_BASE - 0x10000000u) + MT6589_KPD_MEM1 &&
        offset < (MT6589_KPD_BASE - 0x10000000u) + MT6589_KPD_MEM1 + 5 * 4) {
        /*
         * One key is held: the power key (scan code 8, from the blade10
         * cust_kpd.h), because that is how this chain is meant to start. The
         * boot reason is decided in platform_boot_status() by trying
         * mtk_detect_key(8) *before* usb_accessory_in(), so reporting the power
         * key selects BR_POWER_KEY and the preloader boots from storage instead
         * of entering USB download.
         *
         * The download-key combination deliberately stays clear. It is an AND of
         * scan codes 8, 10 and 11, so holding 8 alone is not enough to trigger
         * emergency download - holding all three was, and that is what an
         * all-zeroes KP_MEM reports.
         */
        static const uint16_t mem[5] = {
            /* bit 8 clear = power key pressed; everything else released. */
            0xfeff, 0xffff, 0xffff, 0xffff, 0xff00,
        };
        unsigned idx = (offset - ((MT6589_KPD_BASE - 0x10000000u) +
                                 MT6589_KPD_MEM1)) / 4;
        uint32_t v = mem[idx];
        return (size == 1) ? (v & 0xff) : (v & 0xffff);
    }
    qemu_log_mask(LOG_UNIMP, "%s: offset 0x%" HWADDR_PRIx " size %u\n",
                  __func__, offset, size);
    return 0;
}

static void mt6589_peri_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    Mt6589State *s = opaque;

    if (mt6589_gpu_log_enabled() &&
        offset >= MT6589_GPU_WINDOW && offset < MT6589_GPU_WINDOW + MT6589_GPU_SIZE) {
        fprintf(stderr, "GPU W %05" HWADDR_PRIx " = %08" PRIx32 "\n",
                offset - MT6589_GPU_WINDOW, (uint32_t)value);
    }

    {
        uint32_t pc = 0, lr = 0;

        if (mt6589_late_log(0x10000000 + offset, &pc, &lr)) {
            fprintf(stderr, "LATE W phys=%08" HWADDR_PRIx " = %08" PRIx32
                    " pc=%08" PRIx32 " lr=%08" PRIx32 "\n",
                    0x10000000 + offset, (uint32_t)value, pc, lr);
        }
    }

    /* SPM MTCMOS power-control registers - see mt6589_peri_read(). */
    if (offset >= 0x6200 && offset < 0x62a0) {
        /*
         * Each domain's PWR_CON drives its bit in SPM_PWR_STATUS. The pairs come
         * from mt_clkmgr.c's syss[] table (sta_mask) and mt_spm.h (ctl_addr),
         * with this window's base at 0x1000_6000:
         *
         *   MD1 0x284 bit0   MD2 0x280 bit1   DPY 0x240 bit2   DIS 0x23c bit3
         *   MFG 0x214 bit4   ISP 0x238 bit5   IFR 0x234 bit6   VEN 0x230 bit7
         *   VDE 0x210 bit8
         *
         * Tracking all of them is what lets mt_subsys_init() power a domain down
         * and see the status clear. Handling only the GPU and the display got the
         * boot one domain further each time - measured, it went MD1 -> MD2 -> MFG
         * -> ISP as each was fixed, because each power-down waits for its own
         * status bit.
         */
        static const struct {
            uint16_t off;
            uint8_t bit;
        } spm_domains[] = {
            { 0x6284, 0 }, { 0x6280, 1 }, { 0x6240, 2 }, { 0x623c, 3 },
            { 0x6214, 4 }, { 0x6238, 5 }, { 0x6234, 6 }, { 0x6230, 7 },
            { 0x6210, 8 },
        };
        unsigned idx = (offset - 0x6200) / 4;
        uint32_t on = (uint32_t)value & 0xcu;   /* PWR_ON | PWR_ON_S */

        s->spm_pwr_con[idx] = (uint32_t)value;
        for (size_t i = 0; i < ARRAY_SIZE(spm_domains); i++) {
            if (spm_domains[i].off == offset) {
                uint32_t mask = 1u << spm_domains[i].bit;

                s->spm_status = on ? (s->spm_status | mask)
                                   : (s->spm_status & ~mask);
                break;
            }
        }
        return;
    }

    /* INFRACFG AXI protection - see mt6589_peri_read(). */
    switch (offset) {
    case 0x1200:
        s->topaxi_si0_ctl = (uint32_t)value;
        return;
    case 0x1220:
        s->topaxi_prot_en = (uint32_t)value;
        return;
    case 0x1228:
        /* Status is not writable; writes are ignored, as on hardware. */
        return;
    }

    /*
     * M4U - see mt6589_peri_read(). An invalidation completes immediately here: this
     * model has no TLB to search, so the honest completion time is zero, and the
     * status bits the driver polls are raised (and, because the driver clears them by
     * writing 0, cleared) the same way hardware would.
     *
     *   REG_MMUg_INVLD       global + 0x04   bit 0 = invalidate range, bit 1 = all
     *   REG_L2_GDC_STATE     L2     + 0x00   event field in bits 7:6
     *   REG_L2_GPE_STATUS    L2     + 0x18   bit 0 = prefetch done, bit 1 = range done
     */
    if (offset >= 0x205000 && offset < 0x206000) {
        hwaddr idx = (offset - 0x205000) / 4;
        bool invalidate = ((offset & 0xff) == 0x04) && ((uint32_t)value & 0x3u);

        s->m4u_regs[idx] = (uint32_t)value;
        if (invalidate) {
            s->m4u_regs[0x100 / 4] |= 0x3u << 6;       /* GDC_STATE event */
            s->m4u_regs[(0x100 + 0x18) / 4] |= 0x3u;   /* GPE_STATUS done bits */
        }
        return;
    }

    qemu_log_mask(LOG_UNIMP, "%s: offset 0x%" HWADDR_PRIx " <- 0x%" PRIx64
                  " size %u\n", __func__, offset, value, size);
}

static const MemoryRegionOps mt6589_peri_ops = {
    .read = mt6589_peri_read,
    .write = mt6589_peri_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * 1-byte access must be allowed. The boot ROM probes registers with byte
     * loads and stores (a single-byte read of 0x11006024 aborted with DFSR
     * alignment fault when min_access_size was 4), and a Data Abort there sends
     * the ROM to its abort vector, where it loops. Being permissive here costs
     * nothing: the handler ignores the value anyway.
     */
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

/*
 * The high half of the peripheral region reuses the same implementation with the
 * offset normalised, so a handler offset always denotes the same physical address no
 * matter which region the access came through. See MT6589_PERI_HI_SHIFT.
 */
static uint64_t mt6589_peri_hi_read(void *opaque, hwaddr offset, unsigned size)
{
    return mt6589_peri_read(opaque, offset + MT6589_PERI_HI_SHIFT, size);
}

static void mt6589_peri_hi_write(void *opaque, hwaddr offset, uint64_t value,
                                 unsigned size)
{
    mt6589_peri_write(opaque, offset + MT6589_PERI_HI_SHIFT, value, size);
}

static const MemoryRegionOps mt6589_peri_ops_hi = {
    .read = mt6589_peri_hi_read,
    .write = mt6589_peri_hi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 8,
    },
};

static void mt6589_init(Object *obj)
{
    Mt6589State *s = MT6589(obj);

    /*
     * SPM power-domain status starts at zero: the kernel's clock framework counts
     * what *it* enabled, so a domain that reads as up while its mux usecount is
     * zero sends mt_subsys_init() into sys_disable_locked() - measured as
     * "Kernel BUG at c003fed4", mux_disable_locked+0x70, BUG_ON(!mux->cnt), then
     * a panic. Reporting everything off makes the init a no-op instead, and each
     * driver powers on what it needs.
     *
     * Status bits from mt_spm_mtcmos.c: MD1 = 1 << 0, MD2 = 1 << 1, DPY = 1 << 2,
     * DIS = 1 << 3, MFG = 1 << 4, ISP = 1 << 5, IFR = 1 << 6, VEN = 1 << 7,
     * VDE = 1 << 8.
     */
    s->spm_status = 0;

    memory_region_init_io(&s->gpt_iomem, OBJECT(s), &mt6589_gpt_ops, s,
                          "mt6589.gpt", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->gpt_iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->gpt_irq);

    timer_init_ns(&s->gpt_tick, QEMU_CLOCK_VIRTUAL, mt6589_gpt_tick, s);

    /* SRAM: the boot ROM drops the preloader here. 256 KiB is the documented
     * size; the preloader is loaded by the BROM, so the region has to exist
     * before the guest starts rather than being created on first write. */
    memory_region_init_ram(&s->sram, OBJECT(s), "mt6589.sram",
                           MT6589_SRAM_SIZE, &error_fatal);
    memory_region_init_ram(&s->sramboot, OBJECT(s), "mt6589.sramboot",
                           MT6589_SRAMBOOT_SIZE, &error_fatal);
    /* The preloader's SRAM - see the note on MT6589_PRERAM_BASE. */
    memory_region_init_ram(&s->preram, OBJECT(s), "mt6589.preram",
                           MT6589_PRERAM_SIZE, &error_fatal);

    /* The boot ROM window. Backed by RAM rather than ROM so the image can be
     * loaded into it with -bios, and so a wrong write faults visibly rather than
     * being silently ignored - but the region exists even when empty, because
     * the BROM's own vectors must be readable from address 0. */
    memory_region_init_ram(&s->bootrom, OBJECT(s), "mt6589.bootrom",
                           MT6589_BOOTROM_SIZE, &error_fatal);
    memory_region_init_io(&s->sej_iomem, OBJECT(s), &mt6589_sej_ops, s,
                          "mt6589.sej", 0x1000);
    memory_region_init_io(&s->chipid_iomem, OBJECT(s), &mt6589_chipid_ops, s,
                          "mt6589.chipid", 0x10);
    memory_region_init_io(&s->mcusys_iomem, OBJECT(s), &mt6589_mcusys_ops, s,
                          "mt6589.mcusys", MT6589_MCUSYS_SIZE);
}

static void mt6589_realize(DeviceState *dev, Error **errp)
{
    Mt6589State *s = MT6589(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    /* cpu_create needs the concrete model: TYPE_ARM_CPU is abstract and
     * object_initialize_with_type asserts on it. */
    unsigned ncpu = MIN((unsigned)current_machine->smp.cpus, MT6589_MAX_CPUS);

    for (int i = 0; i < ncpu; i++) {
        s->cpu[i] = ARM_CPU(cpu_create(ARM_CPU_TYPE_NAME("cortex-a7")));
    }
    /* CBIRQ: record whether any IRQ ever reaches the CPU */
    { static int n; (void)n; }

    /*
     * GICv2 at the addresses the kernel device tree declares: distributor
     * 0x1021_1000, CPU interface 0x1021_2000. The CPU has to be told which GIC
     * serves it, and the peripherals take their IRQ lines from it - without
     * this the CPU exposes no input lines at all and qdev's
     * qdev_get_gpio_in_named asserts.
     */
    s->gic = qdev_new(TYPE_ARM_GIC);
    qdev_prop_set_uint32(s->gic, "num-cpu", ncpu);
        /*
     * The GIC needs at least 204 SPIs: the display blocks sit above 160
     * (mt6589.dtsi puts OVL at GIC_SPI 163), and qdev asserts rather than
     * truncating when a peripheral asks for a line past the end.
     */
    /*
     * 256, not 224. QEMU's arm_gic reserves the last GIC_INTERNAL (32) lines for
     * the virtual-interrupt extension, so with num-irq 224 the usable GIC lines
     * stop at 191 and any SPI above 159 lands in the reserved block: driving one
     * hits `assert(irq >= GIC_NR_SGIS)` after the line is folded back into that
     * range. DISP MUTEX is SPI 160 (line 192), so it needs num-irq >= 224 + 32.
     *
     * The old 224 was fine only because nothing above SPI 159 was wired.
     */
    qdev_prop_set_uint32(s->gic, "num-irq", MT6589_GIC_NUM_IRQ);
    qdev_prop_set_uint32(s->gic, "revision", 2);
    /*
     * The real GIC has the security extensions and the kernel programs its
     * interrupts into group 1 - GICD_IGROUPR for IRQ 145 reads 0xff - because it
     * expects to run non-secure after TrustZone boots it. This machine boots
     * everything in secure state, so QEMU classifies every access as secure and
     * gic_get_current_pending_irq() then answers 1022 for a group-1 interrupt with
     * AckCtl clear (GICC_CTLR = 0x1b), i.e. the interrupt is never dispatched.
     *
     * Turning the extensions on here does not fix that on its own (measured:
     * HPPIR still 0x3fe) - what is missing is the CPU's non-secure handover, the
     * SCR_EL3.NS switch that LK performs before entering Linux. See
     * docs/qemu/lk-irq-and-dma.md.
     */
    qdev_prop_set_bit(s->gic, "has-security-extensions", false);
    qdev_realize(s->gic, sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 0, MT6589_GIC_DIST_BASE);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->gic), 1, MT6589_GIC_CPU_BASE);
    /* The GIC's CPU-side lines are sysbus IRQs, not named GPIOs, so they are
     * connected with sysbus_connect_irq rather than qdev_connect_gpio_out. */
    /*
     * arm_gic exposes num_cpu IRQ lines followed by num_cpu FIQ lines, so CPU i
     * takes line i for its IRQ and line ncpu + i for its FIQ.
     */
    for (int i = 0; i < ncpu; i++) {
        /*
         * The CPU's architected-timer outputs have to be wired to the GIC's PPI
         * lines; qdev_init_gpio_out() only creates the CPU-side lines, and an
         * unconnected line means the generic timer never raises an interrupt.
         *
         * That is not hypothetical: once interrupts started working the kernel
         * moved its tick off the GPT and onto this timer - measured,
         * GICD_ISENABLER0 gained PPI 29 while GPT_IRQEN bit 0 was cleared - and
         * with nothing connected, jiffies froze at 0x36 and the boot stopped
         * idle in cpu_v7_do_idle with the initcalls half done.
         *
         * The line numbering matters. arm_gic's gic_set_irq() takes
         * [0 .. N-1] as external interrupts and then N + cpu*32 + irq as the PPIs
         * of each CPU, with N = num_irq - GIC_INTERNAL. Passing the bare PPI
         * number therefore wires nothing useful - it lands on an SPI - which is
         * why simply connecting GTIMER_SEC to line 29 changed nothing at all.
         */
        static const int gt_ppi[] = {
            [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,   /* 30 */
            [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,     /* 27 */
            [GTIMER_HYP]  = ARCH_TIMER_NS_EL2_IRQ,   /* 26 */
            [GTIMER_SEC]  = ARCH_TIMER_S_EL1_IRQ,    /* 29 */
        };
        int ppibase = (MT6589_GIC_NUM_IRQ - 32) + i * 32;

        for (int j = 0; j < ARRAY_SIZE(gt_ppi); j++) {
            qdev_connect_gpio_out(DEVICE(s->cpu[i]), j,
                                  qdev_get_gpio_in(s->gic, ppibase + gt_ppi[j]));
        }

        sysbus_connect_irq(SYS_BUS_DEVICE(s->gic), i,
                           qdev_get_gpio_in(DEVICE(s->cpu[i]), ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->gic), ncpu + i,
                           qdev_get_gpio_in(DEVICE(s->cpu[i]), ARM_CPU_FIQ));
    }

    /*
     * MT6589 is a quad-core part and the kernel is built for four ([CORE] num:4),
     * so the secondaries start powered down and are released by the MCUSYS slave
     * registers above. arm_set_cpu_on() refuses a CPU that is already on.
     */
    for (int i = 1; i < ncpu; i++) {
        arm_set_cpu_off(i);
    }

    /*
     * The GPT's interrupt line. It used to be declared and driven by
     * mt6589_gpt_update_irq() but never assigned from the GIC, so
     * qemu_set_irq(NULL, ...) was a no-op and the tick never reached the CPU:
     * LK's scheduler never ran from the timer, and event_wait() could not be
     * preempted. MT6589_GPT_IRQ_ID is GIC_PRIVATE_SIGNALS(32) + 113 = 145, per
     * the kernel's mt_irq.h and mainline's timer@10008000 (GIC_SPI 113).
     */
    s->gpt_irq = qdev_get_gpio_in(s->gic, MT6589_GIC_GPIO(MT6589_GPT_IRQ));

    for (int i = 0; i < MT6589_NUM_UARTS; i++) {
        hwaddr base = MT6589_UART0_BASE + i * 0x1000;
        qemu_irq irq = qdev_get_gpio_in(s->gic, MT6589_GIC_GPIO(MT6589_UART_IRQ + i));


        /*
         * The UART is a plain 16550, but with a 4-byte register stride rather
         * than a packed 8-byte window: the datasheet puts RBR at 0x00, IER at
         * 0x04, IIR/FCR at 0x08, LCR at 0x0c and so on to SCR at 0x1c, so
         * regshift must be 2. With regshift 0 serial_mm registers only eight
         * bytes, every register above IER falls outside it, and the guest's very
         * first LCR write (0x1100600c) raised a Data Abort - which the boot ROM
         * answers by taking the abort vector and looping there.
         */
        s->uart[i] = serial_mm_init(get_system_memory(), base, 2, irq, 115200,
                                    serial_hd(i), DEVICE_LITTLE_ENDIAN);
    }

    /*
     * MSDC0. The boot ROM programs the clock and polls MSDC_CFG.CKSTB, so a
     * missing controller stalls it at SD initialisation - which is the point the
     * QEMU port reached before this was added.
     */
    for (int i = 0; i < MT6589_NUM_MSDC; i++) {
        char name[8];

        snprintf(name, sizeof(name), "msdc%d", i);
        s->msdc[i] = MT6589_MSDC(object_new(TYPE_MT6589_MSDC));
        object_property_add_child(OBJECT(s), name, OBJECT(s->msdc[i]));
        snprintf(s->msdc[i]->name, sizeof(s->msdc[i]->name), "msdc%d", i);
        /*
         * The card goes in slot 0 and only slot 0. Setting the property on every
         * MSDC - which is what -global mt6589-msdc.image=... does - models a board
         * with a card in all five slots, and the ROM probes each of them.
         */
        if (i == 0 && s->card_image) {
            object_property_set_str(OBJECT(s->msdc[i]), "image",
                                    s->card_image, &error_fatal);
        }
        qdev_realize(DEVICE(s->msdc[i]), sysbus_get_default(), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->msdc[i]), 0, MT6589_MSDC_BASE(i));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->msdc[i]), 0,
                           qdev_get_gpio_in(s->gic,
                                              MT6589_GIC_GPIO(MT6589_MSDC_IRQ(i))));
    }

    /*
     * NFI + NFIECC. The board has no NAND, but the boot ROM probes it as a
     * preloader source and polls NFIECC_ENCIDLE bit 0 while doing so, so an
     * absent block stalls the probe instead of letting the ROM move on.
     */
    s->nfi = MT6589_NFI(object_new(TYPE_MT6589_NFI));
    object_property_add_child(OBJECT(s), "nfi", OBJECT(s->nfi));
    qdev_realize(DEVICE(s->nfi), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->nfi), 0, MT6589_NFI_BASE);

    /*
     * TOPRGU at 0x10000000. Its WDT_STA is the boot reason the guest branches
     * on, so it must be modelled rather than left to the catch-all: reading 0
     * there is correct by accident, but a WDT_MODE write being dropped would be
     * a silent difference from hardware.
     *
     * Note the address: 0x1000_7000 is TOPRGU on several other MediaTek SoCs and
     * is RESERVED on MT6589. The datasheet map, the preloader's mt_reg_base.h
     * and the kernel dtsi all say 0x1000_0000.
     */
    s->toprgu = MT6589_TOPRGU(object_new(TYPE_MT6589_TOPRGU));
    object_property_add_child(OBJECT(s), "toprgu", OBJECT(s->toprgu));
    qdev_realize(DEVICE(s->toprgu), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->toprgu), 0, MT6589_TOPRGU_BASE);

    /*
     * EFUSEC at 0x10009000 - a flat read-only window onto the fuse array rather
     * than a register bank, carrying the real board's dump. Firmware reads fuses
     * to decide what the hardware is, so invented contents produce a board that
     * boots differently from the one being debugged.
     */
    s->efuse = MT6589_EFUSE(object_new(TYPE_MT6589_EFUSE));
    object_property_add_child(OBJECT(s), "efuse", OBJECT(s->efuse));
    qdev_realize(DEVICE(s->efuse), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->efuse), 0, MT6589_EFUSE_BASE);

    /*
     * AUXADC. LK reads the battery channel before it will continue: with no
     * conversion it times out, takes the zero as 0 mV, decides
     * "is_low_battery, TRUE" and answers with mt_power_off, which then spins
     * asking for a charger. See hw/misc/mt6589_auxadc.c.
     */
    s->auxadc = MT6589_AUXADC(object_new(TYPE_MT6589_AUXADC));
    object_property_add_child(OBJECT(s), "auxadc", OBJECT(s->auxadc));
    qdev_realize(DEVICE(s->auxadc), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->auxadc), 0, MT6589_AUXADC_BASE);

    /* Display subsystem: OVL, RDMA, COLOR, MUTEX. */
    mt6589_disp_create(s, s->gic);

    /*
     * PWRAP at 0x1000F000. The wrapper's FSM has to be modelled rather than left
     * to the catch-all: the guest's transaction routine polls the FSM state and
     * waits for ever at the wrong one, and every PMIC access in the boot chain
     * goes through here.
     */
    s->pwrap = MT6589_PWRAP(object_new(TYPE_MT6589_PWRAP));
    object_property_add_child(OBJECT(s), "pwrap", OBJECT(s->pwrap));
    qdev_realize(DEVICE(s->pwrap), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->pwrap), 0, MT6589_PWRAP_BASE);

    /*
     * DRAMC at 0x1000_4000. The preloader runs a full DDR calibration here and
     * asks the controller's test agent whether each delay tap works; with the
     * window unimplemented every answer is 0, so the calibration reports failure
     * and the DRAM rank count is left unset. See hw/misc/mt6589_dramc.c.
     */
    s->dramc = MT6589_DRAMC(object_new(TYPE_MT6589_DRAMC));
    object_property_add_child(OBJECT(s), "dramc", OBJECT(s->dramc));
    qdev_realize(DEVICE(s->dramc), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dramc), 0, MT6589_DRAMC_BASE);

    /*
     * The DDR PHY mirrors the tuning registers. It gets its own instance rather
     * than aliasing DRAMC because the guest programs them independently - DRAMC's
     * copy and the PHY's are written separately during calibration.
     */
    s->ddrphy = MT6589_DRAMC(object_new(TYPE_MT6589_DRAMC));
    object_property_add_child(OBJECT(s), "ddrphy", OBJECT(s->ddrphy));
    qdev_realize(DEVICE(s->ddrphy), sysbus_get_default(), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->ddrphy), 0, MT6589_DDRPHY_BASE);

    /*
     * The system DMA. The DLE calibration's memory-to-memory test drives it; the
     * datasheet lists it as "0x1100_0000 4KB DMA".
     */
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dramc), 1, MT6589_DMA_BASE);

    sysbus_mmio_map(sbd, 0, MT6589_GPT_BASE);
    sysbus_connect_irq(sbd, 0,
                       qdev_get_gpio_in(s->gic,
                                          MT6589_GIC_GPIO(MT6589_GPT_IRQ)));

    memory_region_add_subregion(get_system_memory(), MT6589_SRAM_BASE,
                                &s->sram);
    memory_region_add_subregion(get_system_memory(), MT6589_BOOTROM_BASE,
                                &s->bootrom);
    /* "BootROM/SRAM" at 0x1000_2000 - separate from the 64 KiB on-chip SRAM. */
    memory_region_add_subregion(get_system_memory(), MT6589_SRAMBOOT_BASE,
                                &s->sramboot);
    /*
     * Added as an overlap with a *positive* priority: the peripheral catch-all
     * below spans 0x1000_0000..0x17FF_FFFF, which covers this window, so a plain
     * add_subregion here would be shadowed by it. That is worth spelling out
     * because it does not fail loudly - the reads simply land in the catch-all and
     * return 0, so the preloader's own data area reads as zeroes.
     */
    memory_region_add_subregion(get_system_memory(), MT6589_PRERAM_BASE,
                                &s->preram);
    /* Mapped before the catch-all, which is added with a negative priority so
     * this wins. */
    memory_region_add_subregion(get_system_memory(), MT6589_SEJ_BASE,
                                &s->sej_iomem);
    /* Chip ID at 0x0800_0000 - see mt6589_chipid_read(). */
    memory_region_add_subregion(get_system_memory(), MT6589_CHIPID_BASE,
                                &s->chipid_iomem);
    /*
     * Positive priority: this overlay sits on top of the SRAMBOOT RAM at
     * 0x1000_2000 so the kernel's slave-release writes reach it.
     */
    memory_region_add_subregion_overlap(get_system_memory(), MT6589_MCUSYS_BASE,
                                        &s->mcusys_iomem, 1);

    /*
     * Catch-all for the infrastructure and peripheral windows. The datasheet
     * documents a register for every block here; behaviour is added block by
     * block, but in the meantime a read must return a plausible value rather
     * than a bus error, because the guest interprets an abort as "hardware not
     * present" and takes a different boot path. The specific models (GPT, UART)
     * are mapped as subregions of the same space and take priority.
     */
    /*
     * Size and start are chosen to cover every documented block, not just the
     * infrastructure window: the boot ROM's first out-of-BROM access is a read
     * of 0x1500_0188 (IMAGE system, datasheet Table 3-1 "0x1500_0000 16MB"), and
     * with the region ending at 0x1500_0000 that access raised a Data Abort, so
     * the ROM took the abort vector and looped there for ever.
     *
     * 0x1000_0000 .. 0x17FF_FFFF covers the infrastructure (0x1000_0000), the
     * peripheral window (0x1100_0000), the multimedia window (0x1400_0000) and
     * the image/camera window (0x1500_0000), which is everything the datasheet
     * places below the modem cores at 0xA000_0000.
     */
    memory_region_init_io(&s->peri_iomem, OBJECT(s), &mt6589_peri_ops, s,
                          "mt6589.peri", MT6589_PRERAM_BASE - 0x10000000);
    /*
     * Two subregions rather than one, so the preloader's SRAM at 0x1200_0000 is
     * not covered. With a single 0x1000_0000..0x17FF_FFFF region the SRAM was
     * shadowed even at overlap priority 1 - the reads landed in the catch-all and
     * returned 0, with a log line to say so, so LK's own data area read as zeroes
     * while it looked like ordinary unimplemented-register noise.
     */
    memory_region_add_subregion_overlap(get_system_memory(), 0x10000000,
                                        &s->peri_iomem, -1);
    memory_region_init_io(&s->peri_iomem_hi, OBJECT(s), &mt6589_peri_ops_hi, s,
                          "mt6589.peri.hi", 0x18000000 - MT6589_PRERAM_END);
    memory_region_add_subregion_overlap(
        get_system_memory(), MT6589_PRERAM_END, &s->peri_iomem_hi, -1);
}

/*
 * Card image for MSDC0 - a property on the SoC rather than on each MSDC, because
 * only one slot has a card in it.
 */
static const Property mt6589_props[] = {
    DEFINE_PROP_STRING("card-image", Mt6589State, card_image),
};

static void mt6589_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mt6589_realize;
    device_class_set_props(dc, mt6589_props);
}

static const TypeInfo mt6589_types[] = {
    {
        .name = TYPE_MT6589,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589State),
        .instance_init = mt6589_init,
        .class_init = mt6589_class_init,
    },
};

DEFINE_TYPES(mt6589_types)

/* ---------------- machine ---------------- */

static void mt6589_machine_init(MachineState *machine)
{
    static struct arm_boot_info boot_info;
    Mt6589State *s;
    const char *brom;

    assert(machine->ram_size <= 0x40000000);

    s = MT6589(object_new(TYPE_MT6589));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(s));
    /*
     * Card image, from MT6589_CARD_IMAGE. The property lives on the SoC and the
     * machine creates that SoC itself, so there is no -machine option to reach
     * it with; an environment variable keeps the runner script in charge of it
     * rather than adding a second way to configure the same thing.
     */
    {
        const char *card = getenv("MT6589_CARD_IMAGE");
        if (card && *card) {
            object_property_set_str(OBJECT(s), "card-image", card, &error_fatal);
        }
    }
    /* A SysBusDevice must be realised on the system bus, not on NULL: the
     * machine has no bus of its own, and qdev asserts on a device whose class
     * declares a bus type being realised outside one. */
    qdev_realize(DEVICE(s), sysbus_get_default(), &error_fatal);

    memory_region_add_subregion(get_system_memory(), MT6589_DRAM_BASE,
                                machine->ram);

    /*
     * The MT6589 does not boot a kernel. The boot ROM at address 0 reads the
     * preloader from eMMC/NAND/UART, the preloader brings up DRAM, and LK loads
     * whatever comes next. So:
     *
     *   -bios FILE    the boot ROM image, loaded at 0x0000_0000
     *   -kernel FILE  the preloader, loaded at the DRAM base
     *
     * Both are needed for a faithful run, and loading the BROM with -kernel
     * (which puts it at the DRAM base) makes it execute from an address it was
     * never linked for. That mistake is worth recording: the BROM appeared to
     * run, and only its failure to reach its own UART poll showed anything was
     * wrong.
     */
    brom = machine->firmware;
    if (brom) {
        /*
         * rom_add_file_fixed, not load_image_size: the latter writes to a *host*
         * pointer, so passing the guest address silently scribbles on this
         * process's memory and leaves address 0 empty. The symptom is a BROM
         * that appears to run - the entry point and vectors execute - and then
         * walks into zeros, because only the first page's worth of code was
         * ever legible.
         */
        if (rom_add_file_fixed(brom, MT6589_BOOTROM_BASE, -1) < 0) {
            error_report("mt6589: could not load boot ROM '%s'", brom);
            exit(1);
        }
    }

    boot_info = (struct arm_boot_info) {
        .loader_start = MT6589_DRAM_BASE,
        .kernel_filename = machine->kernel_filename,
        .ram_size = machine->ram_size,
        .board_id = -1,
        .firmware_loaded = brom != NULL,
    };
    arm_load_kernel(s->cpu[0], machine, &boot_info);
}

static void mt6589_machine_class_init(MachineClass *mc)
{
    mc->desc = "MediaTek MT6589 (Cortex-A7)";
    mc->init = mt6589_machine_init;
    mc->max_cpus = MT6589_MAX_CPUS;
    mc->default_cpus = MT6589_MAX_CPUS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a7");
    mc->default_ram_id = "mt6589.ram";
    mc->default_ram_size = 1024 * MiB;
    /* Sets machine->firmware from -bios. */
    mc->default_boot_order = NULL;
}

DEFINE_MACHINE("mt6589", mt6589_machine_class_init)
