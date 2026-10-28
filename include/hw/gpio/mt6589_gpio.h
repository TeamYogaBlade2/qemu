/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * MediaTek MT6589 GPIO controller.
 */

#ifndef HW_GPIO_MT6589_GPIO_H
#define HW_GPIO_MT6589_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_GPIO "mt6589-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589GpioState, MT6589_GPIO)

struct Mt6589GpioState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    uint32_t regs[0x1000 / 4];
};

#endif /* HW_GPIO_MT6589_GPIO_H */
