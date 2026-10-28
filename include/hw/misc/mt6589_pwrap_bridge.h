/*
 * MediaTek MT6589 PWRAP bridge (second wrapper window, 0x1101_7000).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_MT6589_PWRAP_BRIDGE_H
#define HW_MISC_MT6589_PWRAP_BRIDGE_H

#include "hw/core/sysbus.h"
#include "hw/misc/mt6589_pwrap.h"
#include "qom/object.h"

#define TYPE_MT6589_PWRAP_BRIDGE "mt6589-pwrap-bridge"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589PwrapBridgeState, MT6589_PWRAP_BRIDGE)

struct Mt6589PwrapBridgeState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    /* The AP and the modem chains see one PMIC, so the register file is shared. */
    Mt6589PwrapState *pwrap;

    uint32_t regs[0x1000 / 4];
    uint32_t int_flg;
    uint32_t fsm[2];        /* WACS3, WACS4 */
    uint32_t rdata[2];
};

/* Point the bridge at the wrapper whose PMIC register file it shares. */
void mt6589_pwrap_bridge_connect(Mt6589PwrapBridgeState *s, Mt6589PwrapState *pwrap);

#endif /* HW_MISC_MT6589_PWRAP_BRIDGE_H */
