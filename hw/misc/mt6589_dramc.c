/*
 * MediaTek MT6589 DRAM controller (DRAMC) and its DDR PHY.
 *
 * The preloader runs a real DDR calibration before it will boot: DQS gating
 * window scan, DLE, and RX DQ/DQS delay tuning. Each step programs a delay tap
 * and then asks the controller's built-in test agent whether the tap works, so
 * the whole sequence depends on the test agent reporting results.
 *
 * Modelling a DDR PHY faithfully is not the point here - QEMU's RAM already
 * works, and there is no analogue domain to calibrate. What this model does is
 * answer the *questions the calibration asks*, so the sequence can complete:
 *
 *   DRAMC_TESTRPT (0x3fc)  bit 10 "done", bit 14 "error", bit 18 "passed"
 *
 * Reading 0 for that register - which is what an unimplemented window gives -
 * leaves the preloader spinning on `while (!(TESTRPT & (1 << 10)))`, and when it
 * does give up the calibration has no passing tap, so first_bit_set() returns -1
 * and dramc_calib() reports failure. The preloader then leaves dram_rank_num as
 * 0xffffffff in the boot argument, and LK branches on it and hangs.
 *
 * The delay/PHY registers are latched rather than simulated: the calibration
 * writes a tap value and reads it back to confirm, so they must hold their value,
 * but the value has no effect on data here because there is no data path to
 * affect.
 *
 * NOT modelled, and deliberately: any behaviour that would make a *particular*
 * tap fail. Reporting every tap as passing makes the pass window the whole range,
 * which is not what silicon reports on a given board - it just means the
 * calibration finishes. That distinction is recorded here rather than papered
 * over, because "the calibration passed" is a weaker statement than it looks.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/arm/mt6589.h"
#include "hw/core/irq.h"
#include "system/memory.h"

/* Registers with meaning. Everything else is a plain latch. */
#define DRAMC_CONF1       0x004
#define DRAMC_SPCMD       0x1e4
#define DRAMC_R0DELDLY    0x018
#define DRAMC_PHYCTL1     0x0f0
#define DRAMC_R0DQSIEN    0x094
#define DRAMC_TESTRPT     0x3fc
#define DRAMC_SPCMDRESP   0x3b8
#define DRAMC_DQSGCTL     0x124
#define DRAMC_DQSCTL1     0x0e0
#define DRAMC_DQSGNWCNT0  0x3c0

/*
 * Test agent status. Bit 10 is "the test finished", bit 14 "it failed" and bit
 * 18 "it passed". Reporting done+passed with error clear is what a working board
 * produces.
 */
#define TESTRPT_DONE      (1u << 10)
#define TESTRPT_ERROR     (1u << 14)
#define TESTRPT_PASS      (1u << 18)

/*
 * Whether the currently programmed DQS-gating delay lands inside the sampling
 * window.
 *
 * Real silicon has a contiguous window of taps that work. Here the window is a
 * fixed band of coarse taps, chosen to sit inside dqsi_gw_dly_coarse_tbl[] in
 * dramc_dqs_gw.c (which carries entries 12..17 and a NULL after them): the
 * calibration selects the coarse value with the largest pass count using `>=`,
 * so a window that runs to the end of the range selects index 31, and reading
 * dqsi_gw_dly_coarse_tbl[31] runs past the NULL.
 *
 * The tap is encoded the way dqsi_gw_dly_coarse_factor_handler() writes it:
 * DQSCTL1[26:24] holds the top three bits and DQSGCTL[1:0] the bottom two.
 */
static bool mt6589_dramc_window_ok(Mt6589DramcState *s)
{
    uint32_t hi = (s->reg[DRAMC_DQSCTL1 / 4] >> 24) & 0x7;
    uint32_t lo = s->reg[DRAMC_DQSGCTL / 4] & 0x3;
    uint32_t coarse = (hi << 2) | lo;

    return coarse >= 13 && coarse <= 16;
}

static uint64_t mt6589_dramc_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589DramcState *s = opaque;


    switch (offset) {
    case DRAMC_TESTRPT:
        /*
         * The test agent reports whether the *currently programmed* delay tap
         * samples data correctly. The pass/fail decision is made from
         * DQSGNWCNT0 (see that case), so this register only has to agree with it:
         * done, and pass or fail to match.
         */
        return mt6589_dramc_window_ok(s) ? (TESTRPT_DONE | TESTRPT_PASS)
                                         : (TESTRPT_DONE | TESTRPT_ERROR);

    case DRAMC_DQSGNWCNT0:
        /*
         * This is the register that actually decides whether a delay tap passes.
         * Do_Read_Test_DDR2() in dramc_dqs_gw.c does, twice per tap:
         *
         *     if (DRAMC_READ_REG(DRAMC_DQSGNWCNT0) == 0x04040404) err = 0;
         *     else                                               err = -1;
         *
         * i.e. each of the four byte lanes must report a gating window of exactly
         * 4. Returning the latched 0 - which is what a plain register file does -
         * fails every tap, `dqs_gw[]` stays all zeroes, first_bit_set() returns -1
         * and the calibration reports failure. That is the chain that leaves
         * dram_rank_num at 0xffffffff and hangs LK.
         */
        return mt6589_dramc_window_ok(s) ? 0x04040404u : 0u;

    case DRAMC_SPCMDRESP:
        /*
         * Mode-register read response. DRAM_MRR() in mt_emi.c assembles a 9-bit
         * manufacturer id from bits [10:8] of this register, three times, with a
         * different DQ swap before each read:
         *
         *     MRR_value  = (resp & 0x700) >> 8
         *     MRR_value |= ((resp & 0x700) >> 8) << 3
         *     MRR_value |= ((resp & 0x300) >> 8) << 6
         *
         * The preloader then compares the result against DDR2_MODE_REG_5 for each
         * of its EMI settings; the only values that table accepts, as its own
         * "emi_settings[i].DDR2_MODE_REG_5:..." trace shows, are 1 and 6.
         *
         * Returning 0 - which is what an unimplemented register does - gives
         * manu_id 0, no table entry matches, and the caller prints "setting
         * failed 0xFFFFFFFF". Reporting 1 in bits [10:8] makes all three reads
         * agree and yields 1 | 8 | 64... no: the shifts above combine to 1 because
         * each field is bits [10:8] of the same value, so the low three bits are
         * what matter. 1 lands on a real entry.
         */
        return 1u << 8;

    default:
        break;
    }

    if (offset < sizeof(s->reg)) {
        return s->reg[offset / 4];
    }

    qemu_log_mask(LOG_UNIMP, "%s: read of unmodelled offset 0x%" HWADDR_PRIx
                  "\n", __func__, offset);
    return 0;
}

static void mt6589_dramc_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Mt6589DramcState *s = opaque;

    if (offset < sizeof(s->reg)) {
        s->reg[offset / 4] = (uint32_t)value;
        return;
    }

    qemu_log_mask(LOG_UNIMP, "%s: write 0x%" PRIx64 " to unmodelled offset "
                  "0x%" HWADDR_PRIx "\n", __func__, value, offset);
}

static const MemoryRegionOps mt6589_dramc_ops = {
    .read = mt6589_dramc_read,
    .write = mt6589_dramc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /*
     * 1-byte access has to be allowed: the boot chain probes registers with byte
     * loads, and a Data Abort there sends a stage to its abort vector where it
     * loops - the same trap that the peripheral window hit.
     */
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void mt6589_dramc_reset(DeviceState *dev)
{
    Mt6589DramcState *s = MT6589_DRAMC(dev);

    memset(s->reg, 0, sizeof(s->reg));

    /*
     * A plausible reset state for the PHY control registers: the calibration
     * writes these before reading them, so any value would do, but starting from
     * zero gives a bit-exact "nothing programmed yet" baseline.
     */
    s->reg[DRAMC_CONF1 / 4] = 0;
}

static void mt6589_dramc_realize(DeviceState *dev, Error **errp)
{
    Mt6589DramcState *s = MT6589_DRAMC(dev);

    memory_region_init_io(&s->iomem, OBJECT(dev), &mt6589_dramc_ops, s,
                          "mt6589.dramc", MT6589_DRAMC_SIZE);
}

static void mt6589_dramc_init(Object *obj)
{
    Mt6589DramcState *s = MT6589_DRAMC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    sysbus_init_mmio(sbd, &s->iomem);
}

static void mt6589_dramc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = mt6589_dramc_realize;
    device_class_set_legacy_reset(dc, mt6589_dramc_reset);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo mt6589_dramc_info = {
    .name          = TYPE_MT6589_DRAMC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Mt6589DramcState),
    .instance_init = mt6589_dramc_init,
    .class_init    = mt6589_dramc_class_init,
};

static void mt6589_dramc_register_types(void)
{
    type_register_static(&mt6589_dramc_info);
}

type_init(mt6589_dramc_register_types)
