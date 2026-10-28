/*
 * Synaptics S7300 touch controller, as far as the MT6589 board's driver needs it.
 *
 * Why this exists: the s7300 driver's probe talks to the panel over I2C6 at address 0x38 and fails
 * -ENODEV when nothing answers, and the boot stops there. The panel is an RMI4 device, and RMI4's
 * register protocol is public: writing {0xff, page} selects a page, and the function descriptors
 * ("PDT") are read as 6-byte records walking down from 0xe9 until a record's function number is
 * zero. That is all this model implements, plus a page-0 register file that stores what is written.
 *
 * What it deliberately does not do: report touches. No interrupt is ever raised and the 2D data
 * registers stay zero, so the driver's input device registers and then sees nothing - which is
 * exactly what a panel with no fingers on it looks like.
 *
 * The descriptor addresses below are this model's choice; the driver only ever reads them back and
 * uses them as addresses, so any consistent set works. They are not claimed to be what a real S7300
 * returns, and nothing here is taken from a Synaptics datasheet.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/mt6589_s7300.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define S7300_PAGE_SELECT   0xff
#define S7300_PDT_START     0xe9    /* first descriptor, walking down in 6-byte steps */
#define S7300_PDT_STEP      6

static bool s7300_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("MT6589_S7300_LOG");

        enabled = e && *e && strcmp(e, "0") != 0;
    }
    return enabled;
}

/* Page 0 register file, with the PDT already in place. */
static void s7300_put_descriptor(Mt6589S7300State *s, unsigned addr, uint8_t fn,
                                 uint8_t query, uint8_t command, uint8_t control, uint8_t data)
{
    s->page0[addr + 0] = query;
    s->page0[addr + 1] = command;
    s->page0[addr + 2] = control;
    s->page0[addr + 3] = data;
    s->page0[addr + 4] = fn;
    s->page0[addr + 5] = 1;           /* one interrupt source */
}

static void s7300_reset(Mt6589S7300State *s)
{
    memset(s, 0, sizeof(*s));
    /* Function 0x01 (device control) and 0x11 (2D data), then a zero record ends the walk. */
    s7300_put_descriptor(s, S7300_PDT_START, 0x01, 0x80, 0x81, 0x82, 0x83);
    s7300_put_descriptor(s, S7300_PDT_START - S7300_PDT_STEP, 0x11, 0x84, 0x85, 0x86, 0x87);
    /* The third record is all zeroes: end of PDT. */
}

static bool s7300_transfer(void *opaque, uint8_t addr, bool read, uint8_t *buf, unsigned len)
{
    Mt6589S7300State *s = opaque;
    unsigned i;

    if (read) {
        for (i = 0; i < len; i++) {
            buf[i] = (s->page == 0) ? s->page0[s->reg + i] : 0;
        }
        if (s7300_log_enabled()) {
            fprintf(stderr, "S7300 read reg=0x%02x len=%u -> %02x\n", s->reg, len, buf[0]);
        }
        return true;
    }

    if (len >= 2 && buf[0] == S7300_PAGE_SELECT) {
        s->page = buf[1];
        if (s7300_log_enabled()) {
            fprintf(stderr, "S7300 page select -> %u\n", s->page);
        }
        return true;
    }

    if (len >= 1) {
        s->reg = buf[0];
        for (i = 1; i < len; i++) {
            if (s->page == 0) {
                s->page0[s->reg + (i - 1)] = buf[i];
            }
        }
        if (s7300_log_enabled()) {
            fprintf(stderr, "S7300 write reg=0x%02x len=%u\n", s->reg, len);
        }
    }
    return true;
}

void mt6589_s7300_attach(Mt6589I2CState *i2c, uint8_t addr7, Mt6589S7300State *s)
{
    s7300_reset(s);
    s->i2c = i2c;
    s->addr7 = addr7;
    mt6589_i2c_attach(i2c, addr7, s7300_transfer, s);
    if (s7300_log_enabled()) {
        fprintf(stderr, "S7300 attached to addr 0x%02x\n", addr7);
    }
}
