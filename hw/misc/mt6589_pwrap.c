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
     * The PMIC's auxiliary ADC channels (AUXADC_ADC0..ADC9 at 0x0512..0x0524, so
     * wire addresses 0x0289..0x0292).
     *
     * Each channel register is bit 15 = ready and bits 9:0 = the 10-bit code
     * (PMIC_RG_ADC_RDY_Cn_SHIFT 15, PMIC_RG_ADC_OUT_Cn_SHIFT 0). With no default
     * they read 0, which means "conversion not ready" for ever, and the PMIC
     * thermal and battery code retries indefinitely:
     *
     *     [PWRAP] index=76..79 ... addr=51a
     *     [PWRAP] ERROR,line=357 wait_for_fsm_idle fail,return_value=9
     *     [PWRAP] ERROR,line=388 timeout:BUG_ON here
     *     [Power/PMIC] [pmic_read_interface] Reg[51a]= pmic_wrap read data fail
     *     [THERMAL/MONITOR] thermal_zone_bind_cooling_device Fail. Code(-22)
     *
     * measured. Reporting ready with a mid-scale code is the honest model of an ADC
     * with nothing connected to it: the conversion has completed and the answer is
     * unremarkable, which is what a board sitting at room temperature looks like. The
     * SoC-side AUXADC at 0x1100_1000 is modelled separately and carries the battery
     * and charger values the drivers actually act on.
     */
    { 0x0289, 0x8000 | 0x200 },     /* AUXADC_ADC0 */
    { 0x028A, 0x8000 | 0x200 },     /* AUXADC_ADC1 */
    { 0x028B, 0x8000 | 0x200 },     /* AUXADC_ADC2 */
    { 0x028C, 0x8000 | 0x200 },     /* AUXADC_ADC3 */
    { 0x028D, 0x8000 | 0x200 },     /* AUXADC_ADC4 */
    { 0x028E, 0x8000 | 0x200 },     /* AUXADC_ADC5 */
    { 0x028F, 0x8000 | 0x200 },     /* AUXADC_ADC6 */
    { 0x0290, 0x8000 | 0x200 },     /* AUXADC_ADC7 */
    { 0x0291, 0x8000 | 0x200 },     /* AUXADC_ADC8 */
    { 0x0292, 0x8000 | 0x200 },     /* AUXADC_ADC9 */
    /*
     * PMIC 0x0300 chip ID -> 0x0180. The MT6320 answers 0x2020 on this board
     * (operator's hardware, E2 silicon). Without a chip ID the kernel's
     * mt6397_probe() reads 0, rejects the part as "unsupported chip", and every
     * consumer of the PMIC defers for ever.
     */
    { 0x0180, 0x2020 },

    /*
     * The RTC's frequency meter (FQMTR), reached through PWRAP like every other
     * PMIC register: mt_rtc.c's RTC_Read() is pwrap_read(). The addresses halve
     * on the wire, so FQMTR_CON0 0x0188 -> 0x00C4 and FQMTR_CON2 0x018C ->
     * 0x00C6 (docs/ips/pwrap.md records the captured traffic).
     *
     * CON1 holds the window size (4) and CON0 the "started" bit; bit 3 of CON0
     * reads back clear so the guest sees the measurement as already complete.
     * The *results* are not a default value - see the CON2 read path.
     */
    { 0x00C4, 0x0000 },
    { 0x00C5, 0x0004 },

    /*
     * DEW (the PMIC-side cipher engine) reports ready. mt_pmic_wrap_init.c waits
     * with `do { pwrap_read_nochk(DEW_CIPHER_RDY, &rdata); } while (rdata != 0x1)`,
     * so leaving this at the reset value 0 is an endless wait that only the
     * 1-second timeout ends - which then reports "Enable Encryption fail" and
     * drops the preloader into emergency download mode.
     *
     * DEW_BASE 0xBC00 + 0x20 = 0xBC20, halved to 0x5E10 on the wire.
     */
    { 0x5E10, 0x0001 },

    /*
     * CHRSTATUS.PWRKEY_DEB (0x0144 bit 3). pmic_detect_powerkey() reads it and
     * reports the key as *released* when the bit is 1, pressed when it is 0 - so
     * a zeroed PMIC register file says "power key held", which is exactly what
     * sent the preloader to "Power key boot!" and then to emergency download
     * mode. Bit 3 set = released, which is the state of a board nobody is
     * touching. Wire address 0x0144 >> 1 = 0x00a2.
     */
    /*
     * PWRKEY_DEB stays *clear*: bit 3 = 1 means released, and the preloader's
     * platform_boot_status() checks mtk_detect_key(8) before it looks at the USB
     * cable. Reporting the key as held is what selects BR_POWER_KEY, i.e. a
     * normal boot from storage, which is the path this emulator is meant to
     * exercise. (It is read again later and reported released there, which is
     * what a real boot looks like too - the key is let go once boot starts.)
     */
    { 0x00A2, 0x0000 },

    /*
     * PMIC-side AUXADC, registers 0x0512..0x0530 (wire 0x0289.. and so on).
     *
     * This is a DIFFERENT thing from the SoC's AUXADC at 0x1100_3000, and the
     * two share the macro name AUXADC_CON0 in LK's headers - mt_pmic.h maps it to
     * PMIC 0x0540 while mtk_auxadc_hw.h maps it to the SoC block. Battery and
     * charger voltages come from THIS one, read over PWRAP:
     *
     *   mt_pmic.c: upmu_get_rg_adc_rdy_c0() -> pmic_read_interface(AUXADC_ADC0,
     *              PMIC_RG_ADC_RDY_C0_MASK=0x1, PMIC_RG_ADC_RDY_C0_SHIFT=15)
     *   then:      while (upmu_get_rg_adc_rdy_c0() != 1) { ... "Time out!" }
     *
     * So bit 15 of PMIC 0x0512 must read set or LK reports a timeout, takes the
     * sample as 0, concludes "is_low_battery, TRUE" and calls mt_power_off,
     * which spins asking for a charger a desktop host cannot provide.
     *
     * Layout, from mt_pmic.h:
     *   PMIC_RG_ADC_OUT_C0_MASK  0x3FF, shift 0   <- the 10-bit sample
     *   PMIC_RG_ADC_RDY_C0_MASK  0x1,   shift 15  <- ready
     *
     * The sample is the battery voltage through a divider. blade10's
     * cust_battery.h sets R_BAT_SENSE = 4 and mt_pmic.h has VOLTAGE_FULL_RANGE
     * 1200 over ADC_PRECISE 1024, so
     *
     *     mV = raw * 4 * 1200 / 1024
     *
     * and raw 896 reads back as 4200 mV - a charged battery. That matters
     * because LK's is_low_battery() (mt_bat_bq24196.c) compares against
     * BATTERY_LOWVOL_THRESOLD 3450 and, below it, calls mt_power_off, which then
     * spins waiting for a charger that a desktop host cannot provide.
     *
     * 0x8000 | 896 = 0x8380.
     */
    { 0x0289, 0x8380 },   /* PMIC 0x0512 AUXADC_ADC0: RDY_C0 | 896 -> 4200 mV */
    { 0x028A, 0x8380 },   /* PMIC 0x0514 AUXADC_ADC1 */
    { 0x028B, 0x8380 },   /* PMIC 0x0516 AUXADC_ADC2 */
    /*
     * The trim variant, which is what LK actually reads. Its call passes
     * trimd = 1, so PMIC_IMM_GetOneChannelValue() takes the
     * upmu_get_rg_adc_out_c0_trim() branch - AUXADC_ADC11 (0x0528, wire 0x294)
     * with PMIC_RG_ADC_OUT_C0_TRIM_MASK 0x3FF at shift 0. Setting only ADC0 left
     * ret_data reading 0, i.e. a flat battery, even though the ready bit was
     * set and the timeout was gone.
     */
    { 0x0294, 0x0380 },   /* PMIC 0x0528 AUXADC_ADC11: trim sample 896 */

    /*
     * CHR_CON0.PMIC_RGS_CHRDET (wire 0x0000, bit 5) is left *clear*: no cable is
     * connected. It has to be modelled explicitly rather than left to the
     * zeroed register file only in the sense that its value is deliberate.
     *
     * Setting it (as the Renode work did, to get past "Unknown boot!") makes
     * platform_boot_status() take the BR_USB branch *before* falling through,
     * so the preloader enters USB download mode and prints the "READY" handshake
     * instead of booting. With the power key reported as held the chain reaches
     * BR_POWER_KEY first and never consults this, which is the ordering the
     * preloader's own source has.
     */
    { 0x0000, 0x0000 },
};

/* Store the PMIC-side register file. Indexed by WACS number (address >> 1). */
#define PMIC_NUM_REGS 0x8000

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

    memset(s->pmic, 0, sizeof(s->pmic));
    for (size_t i = 0; i < ARRAY_SIZE(mt6589_pmic_defaults); i++) {
        s->pmic[mt6589_pmic_defaults[i].wacs] = mt6589_pmic_defaults[i].value;
    }
    s->fsm = FSM_IDLE;
    s->rdata = 0;
    s->cipher_rdy = 1;
    s->fqmtr_n = 0;
    s->fqmtr_started = 0;
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
#define FQMTR_CON0_WACS  0x00C4
#define FQMTR_CON1_WACS  0x00C5
#define FQMTR_CON2_WACS  0x00C6
static const uint16_t mt6589_fqmtr_results[] = { 5, 3000, 0, 0 };

static uint16_t mt6589_pwrap_pmic_read(Mt6589PwrapState *s, uint16_t wacs)
{
    if (wacs == 0x0000 || wacs == 0x0051 || wacs == 0x00a2) {
        qemu_log_mask(LOG_UNIMP, "pwrap: rd wacs=%04x -> %04x\n", wacs,
                      s->pmic[wacs]);
    }
    if (wacs == FQMTR_CON2_WACS) {
        /*
         * CON2 carries the measurement result. The guest reads it twice per
         * measurement with different expectations:
         *
         *   while (!(RTC_Read(FQMTR_CON2) == 0) && ...)   ; must be 0 to enter
         *   ret = RTC_Read(FQMTR_CON2);                   ; the result
         *
         * Both are the same register, so the value handed back is whatever the
         * last completed measurement produced - and it is 0 until the guest has
         * started one by writing CON0. That is what makes the first wait exit.
         */
        if (s->fqmtr_started == 0) {
            return 0;
        }
        uint32_t i = s->fqmtr_n ? s->fqmtr_n - 1 : 0;
        return i < ARRAY_SIZE(mt6589_fqmtr_results) ?
               mt6589_fqmtr_results[i] :
               mt6589_fqmtr_results[ARRAY_SIZE(mt6589_fqmtr_results) - 1];
    }
    if (wacs == FQMTR_CON0_WACS) {
        /* Bit 3 set means "measurement in progress"; report it finished. */
        return s->pmic[wacs] & ~0x0008;
    }
    return s->pmic[wacs];
}

static void mt6589_pwrap_pmic_write(Mt6589PwrapState *s, uint16_t wacs,
                                    uint16_t value)
{
    s->pmic[wacs] = value;

    if (wacs == FQMTR_CON1_WACS) {
        /*
         * The window-size write is what advances the result sequence. Counting
         * the CON0 start instead would be wrong: CON0 also carries the measure
         * source, and get_frequency_meter() writes it on every call including
         * the ones whose result is discarded by the wait above.
         */
        s->fqmtr_started = 1;
        if (s->fqmtr_n < ARRAY_SIZE(mt6589_fqmtr_results) + 1) {
            s->fqmtr_n++;
        }
    }
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
                " fsm=%u\n", offset, cmd, s->fsm);
    }

    switch (offset) {
    case R_CMD:
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
        }
        return;
    case R_VLDCLR:
        /* Data collected: back to idle. */
        s->fsm = FSM_IDLE;
        return;
    case R_DIO_EN:
    case R_ARB_SIDLY:
    case R_HIPRIO_EN:
    case R_HARB_STA0:
    case R_HARB_STA1:
    case R_INT_EN:
        mt6589_pwrap_aux_set(s, offset, cmd);
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
