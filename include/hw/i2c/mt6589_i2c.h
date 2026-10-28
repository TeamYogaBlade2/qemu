/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 I2C controller.
 *
 * The controller is a transfer engine: the driver programs slave address, length and the bytes
 * to send, writes START, and then waits for I2C_TRANSAC_COMP in I2C_INTR_STAT (or polls it when
 * it asked for polled mode). Without it the driver's wait never completes, which is what stopped
 * the boot: an I2C client probe on bus 3 timed out, dumped the controller registers and reset it.
 *
 * Register layout comes from include/hw/arm/mt6589-regs/mt6589-i2c-regs.h (datasheet extraction)
 * and agrees with the downstream driver's own offsets in
 * mediatek/platform/mt6589/kernel/drivers/i2c/i2c.c.
 *
 * Devices on the bus are modelled by attaching a handler to a 7-bit address. An address with no
 * handler is NACKed, which is what the hardware does when nothing is on the bus - the model does
 * not invent data for parts that have not been modelled.
 */

#ifndef HW_I2C_MT6589_I2C_H
#define HW_I2C_MT6589_I2C_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_I2C "mt6589-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589I2CState, MT6589_I2C)

#define MT6589_I2C_FIFO_SIZE 8
#define MT6589_I2C_NUM       7      /* I2C0..I2C6, 0x1100d000 .. 0x11013000 */

/**
 * Mt6589I2CTransfer:
 * @addr: 7-bit slave address
 * @read: true for a master read, false for a write
 * @buf:  data; filled in by the handler for a read, the bytes sent for a write
 * @len:  number of bytes
 *
 * Returns true when the device acknowledged the transfer, false to NACK it.
 */
typedef bool (*Mt6589I2CTransfer)(void *opaque, uint8_t addr, bool read,
                                  uint8_t *buf, unsigned len);

struct Mt6589I2CState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t regs[0x1000 / 4];

    /* Bytes the driver has pushed for the next write, and bytes waiting to be read back. */
    uint8_t txfifo[MT6589_I2C_FIFO_SIZE];
    unsigned txlen;
    uint8_t rxfifo[MT6589_I2C_FIFO_SIZE];
    unsigned rxlen, rxpos;

    /* Attached bus devices, indexed by 7-bit address. */
    Mt6589I2CTransfer handler[128];
    void *handler_opaque[128];

    unsigned bus;
};

/* Attach a device at a 7-bit address. Passing a NULL handler detaches. */
void mt6589_i2c_attach(Mt6589I2CState *s, uint8_t addr7, Mt6589I2CTransfer fn, void *opaque);

#endif /* HW_I2C_MT6589_I2C_H */
