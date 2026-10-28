/*
 * MediaTek MT6589 I2C controller model.
 *
 * Why this exists: the controller is what makes an I2C transfer *finish*. The downstream driver
 * programs the address and length, writes START, and then sleeps in mt_deal_result() until
 * I2C_TRANSAC_COMP appears in I2C_INTR_STAT. With nothing behind the registers the transfer never
 * completed, the driver dumped the controller registers, reset it and reset the bus - measured on
 * bus 3 (0x11010000), and that is where the boot stopped.
 *
 * What is modelled: the register file (reset values from the datasheet extraction in
 * mt6589-i2c-regs.h), the soft reset, the fifo clear, the data port, and a transfer engine that
 * reports completion or NACK. Devices on the bus are attached by address; an address with no
 * handler is NACKed, which is what real hardware does with nothing on the bus, rather than
 * returning invented data.
 *
 * Not modelled: the APDMA path (the driver only uses it when dma_en is set, and this boot writes
 * the data port directly), high-speed mode, the debug registers beyond plain storage.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/mt6589_i2c.h"
#include "hw/arm/mt6589-regs/mt6589-i2c-regs.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "system/memory.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Defined in hw/arm/mt6589.c; declared here to avoid pulling in the whole SoC header. */
const char *mt6589_guest_task(void);

/* Offsets relative to the block base, named after the generated header. */
#define R(name)      (MT6589_I2C_##name - MT6589_I2C_BASE)
#define RST(name)    (MT6589_I2C_##name##_RESET)

#define INTR_STAT_TRANSAC_COMP  (1u << 0)
#define INTR_STAT_ACKERR        (1u << 1)
#define INTR_STAT_HS_NACKERR    (1u << 2)
#define INTR_STAT_ARB_LOST      (1u << 3)
#define INTR_STAT_ALL           (INTR_STAT_TRANSAC_COMP | INTR_STAT_ACKERR | \
                                 INTR_STAT_HS_NACKERR | INTR_STAT_ARB_LOST)

#define CONTROL_RS               (1u << 1)   /* repeated start */
#define CONTROL_DMA_EN           (1u << 2)

#define START_BIT                (1u << 0)

static bool mt6589_i2c_log_enabled(void)
{
    static int enabled = -1;

    if (enabled < 0) {
        const char *e = getenv("MT6589_I2C_LOG");

        enabled = e && *e && strcmp(e, "0") != 0;
    }
    return enabled;
}

void mt6589_i2c_attach(Mt6589I2CState *s, uint8_t addr7, Mt6589I2CTransfer fn, void *opaque)
{
    assert(addr7 < 128);
    s->handler[addr7 & 0x7f] = fn;
    s->handler_opaque[addr7 & 0x7f] = fn ? opaque : NULL;
}

static void mt6589_i2c_update_irq(Mt6589I2CState *s)
{
    uint32_t mask = s->regs[R(INTR_MASK) >> 2];
    uint32_t stat = s->regs[R(INTR_STAT) >> 2];

    qemu_set_irq(s->irq, (mask & stat) != 0);
}

static void mt6589_i2c_soft_reset(Mt6589I2CState *s)
{
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[R(DATA_PORT) >> 2]      = RST(DATA_PORT);
    s->regs[R(SLAVE_ADDR) >> 2]     = RST(SLAVE_ADDR);
    s->regs[R(INTR_MASK) >> 2]      = RST(INTR_MASK);
    s->regs[R(INTR_STAT) >> 2]      = RST(INTR_STAT);
    s->regs[R(CONTROL) >> 2]        = RST(CONTROL);
    s->regs[R(TRANSFER_LEN) >> 2]   = RST(TRANSFER_LEN);
    s->regs[R(TRANSAC_LEN) >> 2]    = RST(TRANSAC_LEN);
    s->regs[R(DELAY_LEN) >> 2]      = RST(DELAY_LEN);
    s->regs[R(TIMING) >> 2]         = RST(TIMING);
    s->regs[R(START) >> 2]          = RST(START);
    s->regs[R(EXT_CONF) >> 2]       = RST(EXT_CONF);
    s->regs[R(FIFO_THRESH) >> 2]    = RST(FIFO_THRESH);
    s->regs[R(FIFO_ADDR_CLR) >> 2]  = RST(FIFO_ADDR_CLR);
    s->regs[R(IO_CONFIG) >> 2]      = RST(IO_CONFIG);
    s->regs[R(HS) >> 2]             = RST(HS);
    s->regs[R(SOFTRESET) >> 2]      = RST(SOFTRESET);
    s->regs[R(PATH_DIR) >> 2]       = RST(PATH_DIR);
    s->txlen = 0;
    s->rxlen = 0;
    s->rxpos = 0;
}

/*
 * Run the transfer the driver has set up. The controller on this SoC sends one message per start
 * for the paths the downstream driver uses, with the direction in bit 0 of the slave address
 * (the driver computes addr = (msg->addr << 1) | (read ? 1 : 0)).
 */
static void mt6589_i2c_run_transfer(Mt6589I2CState *s)
{
    uint32_t addr_reg = s->regs[R(SLAVE_ADDR) >> 2];
    uint8_t addr7 = (addr_reg >> 1) & 0x7f;
    bool read = addr_reg & 1;
    unsigned len = s->regs[R(TRANSFER_LEN) >> 2] & 0xff;
    bool acked = true;

    if (len == 0) {
        len = 1;
    }
    if (len > MT6589_I2C_FIFO_SIZE) {
        len = MT6589_I2C_FIFO_SIZE;
    }

    s->rxlen = 0;
    s->rxpos = 0;

    if (s->handler[addr7]) {
        uint8_t buf[MT6589_I2C_FIFO_SIZE] = { 0 };

        if (read) {
            acked = s->handler[addr7](s->handler_opaque[addr7], addr7, true, buf, len);
            if (acked) {
                memcpy(s->rxfifo, buf, len);
                s->rxlen = len;
            }
        } else {
            memcpy(buf, s->txfifo, len);
            acked = s->handler[addr7](s->handler_opaque[addr7], addr7, false, buf, len);
        }
    } else {
        /* Nothing at this address: the bus NACKs, exactly as the hardware would. */
        acked = false;
    }

    s->txlen = 0;

    s->regs[R(INTR_STAT) >> 2] |= INTR_STAT_TRANSAC_COMP;
    if (!acked) {
        s->regs[R(INTR_STAT) >> 2] |= INTR_STAT_ACKERR;
    }
    s->regs[R(START) >> 2] &= ~START_BIT;

    if (mt6589_i2c_log_enabled()) {
        fprintf(stderr, "I2C%u %s addr=0x%02x len=%u %s task=%s\n", s->bus,
                read ? "read " : "write", addr7, len, acked ? "ack" : "NACK",
                mt6589_guest_task());
    }
    mt6589_i2c_update_irq(s);
}

static uint64_t mt6589_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    Mt6589I2CState *s = MT6589_I2C(opaque);

    if (offset == R(DATA_PORT)) {
        uint32_t v = s->rxpos < s->rxlen ? s->rxfifo[s->rxpos] : 0;

        if (s->rxpos < s->rxlen) {
            s->rxpos++;
        }
        return v;
    }
    if (offset == R(FIFO_STAT)) {
        /* bit 0 reads as 1 when the write fifo is empty, which is the reset value. */
        return (s->txlen == 0) ? (RST(FIFO_STAT) | 1u) : 0;
    }
    if (offset >= sizeof(s->regs)) {
        return 0;
    }
    if (mt6589_i2c_log_enabled()) {
        fprintf(stderr, "I2C%u R %03" HWADDR_PRIx " = %08x\n", s->bus, offset,
                s->regs[offset >> 2]);
    }
    return s->regs[offset >> 2];
}

static void mt6589_i2c_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Mt6589I2CState *s = MT6589_I2C(opaque);
    uint32_t v = value;

    if (mt6589_i2c_log_enabled()) {
        fprintf(stderr, "I2C%u W %03" HWADDR_PRIx " = %08x\n", s->bus, offset, v);
    }

    if (offset == R(SOFTRESET)) {
        if (v & 1u) {
            mt6589_i2c_soft_reset(s);
        }
        return;
    }
    if (offset == R(FIFO_ADDR_CLR)) {
        if (v & 1u) {
            s->txlen = 0;
            s->rxlen = 0;
            s->rxpos = 0;
        }
        return;
    }
    if (offset == R(DATA_PORT)) {
        if (s->txlen < MT6589_I2C_FIFO_SIZE) {
            s->txfifo[s->txlen++] = v & 0xff;
        }
        return;
    }
    if (offset == R(FIFO_STAT)) {
        return;                 /* read-only */
    }
    if (offset == R(INTR_STAT)) {
        /* Written with the bits to clear. */
        s->regs[R(INTR_STAT) >> 2] &= ~(v & INTR_STAT_ALL);
        mt6589_i2c_update_irq(s);
        return;
    }
    if (offset == R(START)) {
        s->regs[R(START) >> 2] = v;
        if (v & START_BIT) {
            mt6589_i2c_run_transfer(s);
        }
        return;
    }
    if (offset >= sizeof(s->regs)) {
        return;
    }

    s->regs[offset >> 2] = v;

    if (offset == R(INTR_MASK)) {
        mt6589_i2c_update_irq(s);
    }
}

static const MemoryRegionOps mt6589_i2c_ops = {
    .read = mt6589_i2c_read,
    .write = mt6589_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void mt6589_i2c_reset(DeviceState *dev)
{
    mt6589_i2c_soft_reset(MT6589_I2C(dev));
}

static void mt6589_i2c_realize(DeviceState *dev, Error **errp)
{
    Mt6589I2CState *s = MT6589_I2C(dev);

    mt6589_i2c_soft_reset(s);
    memory_region_init_io(&s->iomem, OBJECT(s), &mt6589_i2c_ops, s,
                          "mt6589-i2c", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->irq);
}

static const Property mt6589_i2c_props[] = {
    DEFINE_PROP_UINT32("bus", Mt6589I2CState, bus, 0),
};

static void mt6589_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mt6589_i2c_realize;
    device_class_set_legacy_reset(dc, mt6589_i2c_reset);
    device_class_set_props(dc, mt6589_i2c_props);
    dc->desc = "MediaTek MT6589 I2C controller";
}

static const TypeInfo mt6589_i2c_types[] = {
    {
        .name = TYPE_MT6589_I2C,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Mt6589I2CState),
        .class_init = mt6589_i2c_class_init,
    },
};

DEFINE_TYPES(mt6589_i2c_types)
