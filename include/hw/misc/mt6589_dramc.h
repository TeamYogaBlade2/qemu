/*
 * MediaTek MT6589 DRAM controller.
 *
 * See hw/misc/mt6589_dramc.c for what is and is not modelled - the short version
 * is that the calibration sequence is answered so it can complete, but no real
 * PHY behaviour is simulated.
 */

#ifndef HW_MISC_MT6589_DRAMC_H
#define HW_MISC_MT6589_DRAMC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_MT6589_DRAMC "mt6589-dramc"
OBJECT_DECLARE_SIMPLE_TYPE(Mt6589DramcState, MT6589_DRAMC)

/*
 * The register window. DRAMC's highest modelled offset is 0x3fc (TESTRPT), and
 * the preloader also touches the neighbouring windows for the other channels, so
 * this covers one controller's block.
 */
#define MT6589_DRAMC_SIZE 0x1000

struct Mt6589DramcState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    MemoryRegion iomem;
    uint32_t reg[MT6589_DRAMC_SIZE / 4];
};

#endif /* HW_MISC_MT6589_DRAMC_H */
