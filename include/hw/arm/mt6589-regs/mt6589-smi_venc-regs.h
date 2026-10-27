/*
 * smi_venc - generated from the MT6589 datasheet by
 * mt6589-emulator/tools/gen_regs_header.py.  DO NOT EDIT.
 *
 * page 0x17001000, 155 registers.
 *
 * Extract this page with tools/extract_all_regs.py, which is the
 * only pipeline that sees every register: it ignores the
 * datasheet's unreliable module headings and groups by address.
 */
#ifndef HW_ARM_SMI_VENC_REGS_H
#define HW_ARM_SMI_VENC_REGS_H

#define MT6589_SMI_VENC_BASE 0x17001000u

/* 0x0000  SMI_LARB_STAT - SMI Level 2 Local Arbiter Status  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_STAT 0x17001000u
#define MT6589_SMI_VENC_SMI_LARB_STAT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_STAT_IDEN_SHIFT 2
#define MT6589_SMI_VENC_SMI_LARB_STAT_IDEN_MASK  0x00000004u
#define MT6589_SMI_VENC_SMI_LARB_STAT_INT0_BUSY_SHIFT 1
#define MT6589_SMI_VENC_SMI_LARB_STAT_INT0_BUSY_MASK  0x00000002u

/* 0x0010  SMI_LARB_CON - SMI Level 2 Local Arbiter Control Register  [reset 0x00078003u] */
#define MT6589_SMI_VENC_SMI_LARB_CON 0x17001010u
#define MT6589_SMI_VENC_SMI_LARB_CON_RESET 0x00078003u
#define MT6589_SMI_VENC_SMI_LARB_CON_IDEN_SHIFT 20
#define MT6589_SMI_VENC_SMI_LARB_CON_IDEN_MASK  0x00300000u
#define MT6589_SMI_VENC_SMI_LARB_CON_DCM_ON_SHIFT 15
#define MT6589_SMI_VENC_SMI_LARB_CON_DCM_ON_MASK  0x00008000u

/* 0x0014  SMI_LARB_CON_SET - SMI level 2 local arbiter set register This register is used to set up SMI local arbiter and report the status of settings.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_CON_SET 0x17001014u
#define MT6589_SMI_VENC_SMI_LARB_CON_SET_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_CON_SET_IDEN_SHIFT 31
#define MT6589_SMI_VENC_SMI_LARB_CON_SET_IDEN_MASK  0x80000000u
#define MT6589_SMI_VENC_SMI_LARB_CON_SET_IDENIN_PREULTRA_FWD_ENIN_ULTRA_FWD_ENPOST_DCM_ON_SHIFT 16
#define MT6589_SMI_VENC_SMI_LARB_CON_SET_IDENIN_PREULTRA_FWD_ENIN_ULTRA_FWD_ENPOST_DCM_ON_MASK  0x7fff0000u

/* 0x0018  SMI_LARB_CON_CLR - SMI level 2 local arbiter clear register This register is used to reset SMI local arbiter and report the status of settings.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_CON_CLR 0x17001018u
#define MT6589_SMI_VENC_SMI_LARB_CON_CLR_RESET 0x00000000u

/* 0x0020  SMI_ARB_CON - SMI level 2 arbiter control register  [reset 0x0000c020u] */
#define MT6589_SMI_VENC_SMI_ARB_CON 0x17001020u
#define MT6589_SMI_VENC_SMI_ARB_CON_RESET 0x0000c020u
#define MT6589_SMI_VENC_SMI_ARB_CON_ULTRA_EN_SHIFT 14
#define MT6589_SMI_VENC_SMI_ARB_CON_ULTRA_EN_MASK  0x00004000u
#define MT6589_SMI_VENC_SMI_ARB_CON_STARVEN_SHIFT 7
#define MT6589_SMI_VENC_SMI_ARB_CON_STARVEN_MASK  0x00000380u
#define MT6589_SMI_VENC_SMI_ARB_CON_FRMSIZE_SHIFT 4
#define MT6589_SMI_VENC_SMI_ARB_CON_FRMSIZE_MASK  0x00000030u
#define MT6589_SMI_VENC_SMI_ARB_CON_RRARBEN_SHIFT 0
#define MT6589_SMI_VENC_SMI_ARB_CON_RRARBEN_MASK  0x0000000fu

/* 0x0024  SMI_ARB_CON_SET - SMI level 2 arbiter set register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_ARB_CON_SET 0x17001024u
#define MT6589_SMI_VENC_SMI_ARB_CON_SET_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_ARB_CON_SET_SSTARVEN_SHIFT 7
#define MT6589_SMI_VENC_SMI_ARB_CON_SET_SSTARVEN_MASK  0x00000380u
#define MT6589_SMI_VENC_SMI_ARB_CON_SET_SFRMSIZE_SHIFT 4
#define MT6589_SMI_VENC_SMI_ARB_CON_SET_SFRMSIZE_MASK  0x00000030u
#define MT6589_SMI_VENC_SMI_ARB_CON_SET_SRRARBEN_SHIFT 0
#define MT6589_SMI_VENC_SMI_ARB_CON_SET_SRRARBEN_MASK  0x0000000fu

/* 0x0028  SMI_ARB_CON_CLR - SMI level 2 arbiter clear register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR 0x17001028u
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR_CSTARVEN_SHIFT 7
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR_CSTARVEN_MASK  0x00000380u
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR_CFRMSIZE_SHIFT 4
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR_CFRMSIZE_MASK  0x00000030u
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR_CRRARBEN_SHIFT 0
#define MT6589_SMI_VENC_SMI_ARB_CON_CLR_CRRARBEN_MASK  0x0000000fu

/* 0x002c  SMI_LARB_BWFILTER_EN - SMI bandwidth filter control register This register is used to control SMI level 2 bandwidth turn.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_BWFILTER_EN 0x1700102cu
#define MT6589_SMI_VENC_SMI_LARB_BWFILTER_EN_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_BWFILTER_EN_BWFILTEREN_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_BWFILTER_EN_BWFILTEREN_15_0__MASK  0x0000ffffu

/* 0x0030  SMI_BW_EXT_CON0 - SMI Level 2 Bandwidth Control Register 0 Bit 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17  [reset 0x00000001u] */
#define MT6589_SMI_VENC_SMI_BW_EXT_CON0 0x17001030u
#define MT6589_SMI_VENC_SMI_BW_EXT_CON0_RESET 0x00000001u
#define MT6589_SMI_VENC_SMI_BW_EXT_CON0_BWARBTN_SHIFT 0
#define MT6589_SMI_VENC_SMI_BW_EXT_CON0_BWARBTN_MASK  0x0000ffffu

/* 0x0040  SMI_STARV_CON0 - SMI VCO0 Starvation Control Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_STARV_CON0 0x17001040u
#define MT6589_SMI_VENC_SMI_STARV_CON0_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_STARV_CON0_SRVVCID0_SHIFT 8
#define MT6589_SMI_VENC_SMI_STARV_CON0_SRVVCID0_MASK  0x0000ff00u
#define MT6589_SMI_VENC_SMI_STARV_CON0_SRVCNT0_SHIFT 0
#define MT6589_SMI_VENC_SMI_STARV_CON0_SRVCNT0_MASK  0x000000ffu

/* 0x0044  SMI_STARV_CON1 - SMI VCO1 Starvation Control Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_STARV_CON1 0x17001044u
#define MT6589_SMI_VENC_SMI_STARV_CON1_RESET 0x00000000u

/* 0x0048  SMI_STARV_CON2 - SMI VCO2 Starvation Control Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_STARV_CON2 0x17001048u
#define MT6589_SMI_VENC_SMI_STARV_CON2_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_STARV_CON2_SRVVCID2_SHIFT 8
#define MT6589_SMI_VENC_SMI_STARV_CON2_SRVVCID2_MASK  0x0000ff00u
#define MT6589_SMI_VENC_SMI_STARV_CON2_SRVCNT2_SHIFT 0
#define MT6589_SMI_VENC_SMI_STARV_CON2_SRVCNT2_MASK  0x000000ffu

/* 0x0054  SMI_INT_PATH_SEL - SMI Internal SRAM Path Selection  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_INT_PATH_SEL 0x17001054u
#define MT6589_SMI_VENC_SMI_INT_PATH_SEL_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_INT_PATH_SEL_INTPATHSEL0_31_16__SHIFT 16
#define MT6589_SMI_VENC_SMI_INT_PATH_SEL_INTPATHSEL0_31_16__MASK  0xffff0000u
#define MT6589_SMI_VENC_SMI_INT_PATH_SEL_INTPATHSEL0_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_INT_PATH_SEL_INTPATHSEL0_15_0__MASK  0x0000ffffu

/* 0x0080  SMI_BW_VC0 - SMI VC0 bandwidth setting register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_BW_VC0 0x17001080u
#define MT6589_SMI_VENC_SMI_BW_VC0_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_BW_VC0_VC0_BW_SHIFT 0
#define MT6589_SMI_VENC_SMI_BW_VC0_VC0_BW_MASK  0x00000fffu

/* 0x0084  SMI_BW_VC1 - SMI VC1 Bandwidth Setting Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_BW_VC1 0x17001084u
#define MT6589_SMI_VENC_SMI_BW_VC1_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_BW_VC1_VC1_BW_SHIFT 0
#define MT6589_SMI_VENC_SMI_BW_VC1_VC1_BW_MASK  0x00000fffu

/* 0x0088  SMI_BW_VC2 - SMI VC2 Bandwidth Setting Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_BW_VC2 0x17001088u
#define MT6589_SMI_VENC_SMI_BW_VC2_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_BW_VC2_VC2_BW_SHIFT 0
#define MT6589_SMI_VENC_SMI_BW_VC2_VC2_BW_MASK  0x00000fffu

/* 0x0100  SMI_EBW_PORT0 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access.  [reset 0x1700117cu] */
#define MT6589_SMI_VENC_SMI_EBW_PORT0 0x17001100u
#define MT6589_SMI_VENC_SMI_EBW_PORT0_RESET 0x1700117cu

/* 0x0104  SMI_EBW_PORT1 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT1 0x17001104u
#define MT6589_SMI_VENC_SMI_EBW_PORT1_RESET 0x00000000u

/* 0x0108  SMI_EBW_PORT2 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT2 0x17001108u
#define MT6589_SMI_VENC_SMI_EBW_PORT2_RESET 0x00000000u

/* 0x010c  SMI_EBW_PORT3 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT3 0x1700110cu
#define MT6589_SMI_VENC_SMI_EBW_PORT3_RESET 0x00000000u

/* 0x0110  SMI_EBW_PORT4 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT4 0x17001110u
#define MT6589_SMI_VENC_SMI_EBW_PORT4_RESET 0x00000000u

/* 0x0114  SMI_EBW_PORT5 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT5 0x17001114u
#define MT6589_SMI_VENC_SMI_EBW_PORT5_RESET 0x00000000u

/* 0x0118  SMI_EBW_PORT6 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT6 0x17001118u
#define MT6589_SMI_VENC_SMI_EBW_PORT6_RESET 0x00000000u

/* 0x011c  SMI_EBW_PORT7 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT7 0x1700111cu
#define MT6589_SMI_VENC_SMI_EBW_PORT7_RESET 0x00000000u

/* 0x0120  SMI_EBW_PORT8 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT8 0x17001120u
#define MT6589_SMI_VENC_SMI_EBW_PORT8_RESET 0x00000000u

/* 0x0124  SMI_EBW_PORT9 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT9 0x17001124u
#define MT6589_SMI_VENC_SMI_EBW_PORT9_RESET 0x00000000u

/* 0x0128  SMI_EBW_PORT10 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT10 0x17001128u
#define MT6589_SMI_VENC_SMI_EBW_PORT10_RESET 0x00000000u

/* 0x012c  SMI_EBW_PORT11 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT11 0x1700112cu
#define MT6589_SMI_VENC_SMI_EBW_PORT11_RESET 0x00000000u

/* 0x0130  SMI_EBW_PORT12 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT12 0x17001130u
#define MT6589_SMI_VENC_SMI_EBW_PORT12_RESET 0x00000000u

/* 0x0134  SMI_EBW_PORT13 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT13 0x17001134u
#define MT6589_SMI_VENC_SMI_EBW_PORT13_RESET 0x00000000u

/* 0x0138  SMI_EBW_PORT14 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT14 0x17001138u
#define MT6589_SMI_VENC_SMI_EBW_PORT14_RESET 0x00000000u

/* 0x013c  SMI_EBW_PORT15 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT15 0x1700113cu
#define MT6589_SMI_VENC_SMI_EBW_PORT15_RESET 0x00000000u

/* 0x0140  SMI_EBW_PORT16 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT16 0x17001140u
#define MT6589_SMI_VENC_SMI_EBW_PORT16_RESET 0x00000000u

/* 0x0144  SMI_EBW_PORT17 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT17 0x17001144u
#define MT6589_SMI_VENC_SMI_EBW_PORT17_RESET 0x00000000u

/* 0x0148  SMI_EBW_PORT18 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT18 0x17001148u
#define MT6589_SMI_VENC_SMI_EBW_PORT18_RESET 0x00000000u

/* 0x014c  SMI_EBW_PORT19 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT19 0x1700114cu
#define MT6589_SMI_VENC_SMI_EBW_PORT19_RESET 0x00000000u

/* 0x0150  SMI_EBW_PORT20 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT20 0x17001150u
#define MT6589_SMI_VENC_SMI_EBW_PORT20_RESET 0x00000000u

/* 0x0154  SMI_EBW_PORT21 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT21 0x17001154u
#define MT6589_SMI_VENC_SMI_EBW_PORT21_RESET 0x00000000u

/* 0x0158  SMI_EBW_PORT22 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT22 0x17001158u
#define MT6589_SMI_VENC_SMI_EBW_PORT22_RESET 0x00000000u

/* 0x015c  SMI_EBW_PORT23 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT23 0x1700115cu
#define MT6589_SMI_VENC_SMI_EBW_PORT23_RESET 0x00000000u

/* 0x0160  SMI_EBW_PORT24 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT24 0x17001160u
#define MT6589_SMI_VENC_SMI_EBW_PORT24_RESET 0x00000000u

/* 0x0164  SMI_EBW_PORT25 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT25 0x17001164u
#define MT6589_SMI_VENC_SMI_EBW_PORT25_RESET 0x00000000u

/* 0x0168  SMI_EBW_PORT26 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT26 0x17001168u
#define MT6589_SMI_VENC_SMI_EBW_PORT26_RESET 0x00000000u

/* 0x016c  SMI_EBW_PORT27 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT27 0x1700116cu
#define MT6589_SMI_VENC_SMI_EBW_PORT27_RESET 0x00000000u

/* 0x0170  SMI_EBW_PORT28 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT28 0x17001170u
#define MT6589_SMI_VENC_SMI_EBW_PORT28_RESET 0x00000000u

/* 0x0174  SMI_EBW_PORT29 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT29 0x17001174u
#define MT6589_SMI_VENC_SMI_EBW_PORT29_RESET 0x00000000u

/* 0x0178  SMI_EBW_PORT30 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_EBW_PORT30 0x17001178u
#define MT6589_SMI_VENC_SMI_EBW_PORT30_RESET 0x00000000u

/* 0x017c  SMI_EBW_PORT31 - SMI external port bandwidth allocation setting This register is used to control port bandwidth of external access.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_EBW_PORT31 0x1700117cu
#define MT6589_SMI_VENC_SMI_EBW_PORT31_RESET 0x00000000u

/* 0x0180  SMI_MAX_GNT0 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access.  [reset 0x170011fcu] */
#define MT6589_SMI_VENC_SMI_MAX_GNT0 0x17001180u
#define MT6589_SMI_VENC_SMI_MAX_GNT0_RESET 0x170011fcu

/* 0x0184  SMI_MAX_GNT1 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT1 0x17001184u
#define MT6589_SMI_VENC_SMI_MAX_GNT1_RESET 0x00000000u

/* 0x0188  SMI_MAX_GNT2 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT2 0x17001188u
#define MT6589_SMI_VENC_SMI_MAX_GNT2_RESET 0x00000000u

/* 0x018c  SMI_MAX_GNT3 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT3 0x1700118cu
#define MT6589_SMI_VENC_SMI_MAX_GNT3_RESET 0x00000000u

/* 0x0190  SMI_MAX_GNT4 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT4 0x17001190u
#define MT6589_SMI_VENC_SMI_MAX_GNT4_RESET 0x00000000u

/* 0x0194  SMI_MAX_GNT5 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT5 0x17001194u
#define MT6589_SMI_VENC_SMI_MAX_GNT5_RESET 0x00000000u

/* 0x0198  SMI_MAX_GNT6 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT6 0x17001198u
#define MT6589_SMI_VENC_SMI_MAX_GNT6_RESET 0x00000000u

/* 0x019c  SMI_MAX_GNT7 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT7 0x1700119cu
#define MT6589_SMI_VENC_SMI_MAX_GNT7_RESET 0x00000000u

/* 0x01a0  SMI_MAX_GNT8 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT8 0x170011a0u
#define MT6589_SMI_VENC_SMI_MAX_GNT8_RESET 0x00000000u

/* 0x01a4  SMI_MAX_GNT9 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT9 0x170011a4u
#define MT6589_SMI_VENC_SMI_MAX_GNT9_RESET 0x00000000u

/* 0x01a8  SMI_MAX_GNT10 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT10 0x170011a8u
#define MT6589_SMI_VENC_SMI_MAX_GNT10_RESET 0x00000000u

/* 0x01ac  SMI_MAX_GNT11 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT11 0x170011acu
#define MT6589_SMI_VENC_SMI_MAX_GNT11_RESET 0x00000000u

/* 0x01b0  SMI_MAX_GNT12 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT12 0x170011b0u
#define MT6589_SMI_VENC_SMI_MAX_GNT12_RESET 0x00000000u

/* 0x01b4  SMI_MAX_GNT13 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT13 0x170011b4u
#define MT6589_SMI_VENC_SMI_MAX_GNT13_RESET 0x00000000u

/* 0x01b8  SMI_MAX_GNT14 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT14 0x170011b8u
#define MT6589_SMI_VENC_SMI_MAX_GNT14_RESET 0x00000000u

/* 0x01bc  SMI_MAX_GNT15 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT15 0x170011bcu
#define MT6589_SMI_VENC_SMI_MAX_GNT15_RESET 0x00000000u

/* 0x01c0  SMI_MAX_GNT16 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT16 0x170011c0u
#define MT6589_SMI_VENC_SMI_MAX_GNT16_RESET 0x00000000u

/* 0x01c4  SMI_MAX_GNT17 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT17 0x170011c4u
#define MT6589_SMI_VENC_SMI_MAX_GNT17_RESET 0x00000000u

/* 0x01c8  SMI_MAX_GNT18 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT18 0x170011c8u
#define MT6589_SMI_VENC_SMI_MAX_GNT18_RESET 0x00000000u

/* 0x01cc  SMI_MAX_GNT19 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT19 0x170011ccu
#define MT6589_SMI_VENC_SMI_MAX_GNT19_RESET 0x00000000u

/* 0x01d0  SMI_MAX_GNT20 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT20 0x170011d0u
#define MT6589_SMI_VENC_SMI_MAX_GNT20_RESET 0x00000000u

/* 0x01d4  SMI_MAX_GNT21 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT21 0x170011d4u
#define MT6589_SMI_VENC_SMI_MAX_GNT21_RESET 0x00000000u

/* 0x01d8  SMI_MAX_GNT22 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT22 0x170011d8u
#define MT6589_SMI_VENC_SMI_MAX_GNT22_RESET 0x00000000u

/* 0x01dc  SMI_MAX_GNT23 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT23 0x170011dcu
#define MT6589_SMI_VENC_SMI_MAX_GNT23_RESET 0x00000000u

/* 0x01e0  SMI_MAX_GNT24 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT24 0x170011e0u
#define MT6589_SMI_VENC_SMI_MAX_GNT24_RESET 0x00000000u

/* 0x01e4  SMI_MAX_GNT25 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT25 0x170011e4u
#define MT6589_SMI_VENC_SMI_MAX_GNT25_RESET 0x00000000u

/* 0x01e8  SMI_MAX_GNT26 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT26 0x170011e8u
#define MT6589_SMI_VENC_SMI_MAX_GNT26_RESET 0x00000000u

/* 0x01ec  SMI_MAX_GNT27 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT27 0x170011ecu
#define MT6589_SMI_VENC_SMI_MAX_GNT27_RESET 0x00000000u

/* 0x01f0  SMI_MAX_GNT28 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT28 0x170011f0u
#define MT6589_SMI_VENC_SMI_MAX_GNT28_RESET 0x00000000u

/* 0x01f4  SMI_MAX_GNT29 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT29 0x170011f4u
#define MT6589_SMI_VENC_SMI_MAX_GNT29_RESET 0x00000000u

/* 0x01f8  SMI_MAX_GNT30 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access. */
#define MT6589_SMI_VENC_SMI_MAX_GNT30 0x170011f8u
#define MT6589_SMI_VENC_SMI_MAX_GNT30_RESET 0x00000000u

/* 0x01fc  SMI_MAX_GNT31 - SMI external port bandwidth limiter setting This register is used to control port bandwidth of external access.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAX_GNT31 0x170011fcu
#define MT6589_SMI_VENC_SMI_MAX_GNT31_RESET 0x00000000u

/* 0x0210  SMI_SHARE_EN - Sharable Enable Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_SHARE_EN 0x17001210u
#define MT6589_SMI_VENC_SMI_SHARE_EN_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_SHARE_EN_MMUEN1_31_16__SHIFT 16
#define MT6589_SMI_VENC_SMI_SHARE_EN_MMUEN1_31_16__MASK  0xffff0000u
#define MT6589_SMI_VENC_SMI_SHARE_EN_MMUEN1_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_SHARE_EN_MMUEN1_15_0__MASK  0x0000ffffu

/* 0x0220  SMI_ROUTE_SEL - Route selection register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_ROUTE_SEL 0x17001220u
#define MT6589_SMI_VENC_SMI_ROUTE_SEL_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_ROUTE_SEL_ROUTE_SEL_31_16__SHIFT 16
#define MT6589_SMI_VENC_SMI_ROUTE_SEL_ROUTE_SEL_31_16__MASK  0xffff0000u

/* 0x0230  SMI_MMULOCK_EN - MMU lock enable register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MMULOCK_EN 0x17001230u
#define MT6589_SMI_VENC_SMI_MMULOCK_EN_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_MMULOCK_EN_MMULOCK_ENABLE_31_16__SHIFT 16
#define MT6589_SMI_VENC_SMI_MMULOCK_EN_MMULOCK_ENABLE_31_16__MASK  0xffff0000u
#define MT6589_SMI_VENC_SMI_MMULOCK_EN_MMULOCK_ENABLE_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_MMULOCK_EN_MMULOCK_ENABLE_15_0__MASK  0x0000ffffu

/* 0x0280  SMI_SUB_PINFO0  [reset 0x170012fcu] */
#define MT6589_SMI_VENC_SMI_SUB_PINFO0 0x17001280u
#define MT6589_SMI_VENC_SMI_SUB_PINFO0_RESET 0x170012fcu

/* 0x0284  SMI_SUB_PINFO1 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO1 0x17001284u
#define MT6589_SMI_VENC_SMI_SUB_PINFO1_RESET 0x00000000u

/* 0x0288  SMI_SUB_PINFO2 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO2 0x17001288u
#define MT6589_SMI_VENC_SMI_SUB_PINFO2_RESET 0x00000000u

/* 0x028c  SMI_SUB_PINFO3 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO3 0x1700128cu
#define MT6589_SMI_VENC_SMI_SUB_PINFO3_RESET 0x00000000u

/* 0x0290  SMI_SUB_PINFO4 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO4 0x17001290u
#define MT6589_SMI_VENC_SMI_SUB_PINFO4_RESET 0x00000000u

/* 0x0294  SMI_SUB_PINFO5 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO5 0x17001294u
#define MT6589_SMI_VENC_SMI_SUB_PINFO5_RESET 0x00000000u

/* 0x0298  SMI_SUB_PINFO6 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO6 0x17001298u
#define MT6589_SMI_VENC_SMI_SUB_PINFO6_RESET 0x00000000u

/* 0x029c  SMI_SUB_PINFO7 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO7 0x1700129cu
#define MT6589_SMI_VENC_SMI_SUB_PINFO7_RESET 0x00000000u

/* 0x02a0  SMI_SUB_PINFO8 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO8 0x170012a0u
#define MT6589_SMI_VENC_SMI_SUB_PINFO8_RESET 0x00000000u

/* 0x02a4  SMI_SUB_PINFO9 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO9 0x170012a4u
#define MT6589_SMI_VENC_SMI_SUB_PINFO9_RESET 0x00000000u

/* 0x02a8  SMI_SUB_PINFO10 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO10 0x170012a8u
#define MT6589_SMI_VENC_SMI_SUB_PINFO10_RESET 0x00000000u

/* 0x02ac  SMI_SUB_PINFO11 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO11 0x170012acu
#define MT6589_SMI_VENC_SMI_SUB_PINFO11_RESET 0x00000000u

/* 0x02b0  SMI_SUB_PINFO12 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO12 0x170012b0u
#define MT6589_SMI_VENC_SMI_SUB_PINFO12_RESET 0x00000000u

/* 0x02b4  SMI_SUB_PINFO13 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO13 0x170012b4u
#define MT6589_SMI_VENC_SMI_SUB_PINFO13_RESET 0x00000000u

/* 0x02b8  SMI_SUB_PINFO14 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO14 0x170012b8u
#define MT6589_SMI_VENC_SMI_SUB_PINFO14_RESET 0x00000000u

/* 0x02bc  SMI_SUB_PINFO15 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO15 0x170012bcu
#define MT6589_SMI_VENC_SMI_SUB_PINFO15_RESET 0x00000000u

/* 0x02c0  SMI_SUB_PINFO16 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO16 0x170012c0u
#define MT6589_SMI_VENC_SMI_SUB_PINFO16_RESET 0x00000000u

/* 0x02c4  SMI_SUB_PINFO17 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO17 0x170012c4u
#define MT6589_SMI_VENC_SMI_SUB_PINFO17_RESET 0x00000000u

/* 0x02c8  SMI_SUB_PINFO18 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO18 0x170012c8u
#define MT6589_SMI_VENC_SMI_SUB_PINFO18_RESET 0x00000000u

/* 0x02cc  SMI_SUB_PINFO19 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO19 0x170012ccu
#define MT6589_SMI_VENC_SMI_SUB_PINFO19_RESET 0x00000000u

/* 0x02d0  SMI_SUB_PINFO20 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO20 0x170012d0u
#define MT6589_SMI_VENC_SMI_SUB_PINFO20_RESET 0x00000000u

/* 0x02d4  SMI_SUB_PINFO21 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO21 0x170012d4u
#define MT6589_SMI_VENC_SMI_SUB_PINFO21_RESET 0x00000000u

/* 0x02d8  SMI_SUB_PINFO22 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO22 0x170012d8u
#define MT6589_SMI_VENC_SMI_SUB_PINFO22_RESET 0x00000000u

/* 0x02dc  SMI_SUB_PINFO23 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO23 0x170012dcu
#define MT6589_SMI_VENC_SMI_SUB_PINFO23_RESET 0x00000000u

/* 0x02e0  SMI_SUB_PINFO24 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO24 0x170012e0u
#define MT6589_SMI_VENC_SMI_SUB_PINFO24_RESET 0x00000000u

/* 0x02e4  SMI_SUB_PINFO25 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO25 0x170012e4u
#define MT6589_SMI_VENC_SMI_SUB_PINFO25_RESET 0x00000000u

/* 0x02e8  SMI_SUB_PINFO26 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO26 0x170012e8u
#define MT6589_SMI_VENC_SMI_SUB_PINFO26_RESET 0x00000000u

/* 0x02ec  SMI_SUB_PINFO27 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO27 0x170012ecu
#define MT6589_SMI_VENC_SMI_SUB_PINFO27_RESET 0x00000000u

/* 0x02f0  SMI_SUB_PINFO28 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO28 0x170012f0u
#define MT6589_SMI_VENC_SMI_SUB_PINFO28_RESET 0x00000000u

/* 0x02f4  SMI_SUB_PINFO29 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO29 0x170012f4u
#define MT6589_SMI_VENC_SMI_SUB_PINFO29_RESET 0x00000000u

/* 0x02f8  SMI_SUB_PINFO30 */
#define MT6589_SMI_VENC_SMI_SUB_PINFO30 0x170012f8u
#define MT6589_SMI_VENC_SMI_SUB_PINFO30_RESET 0x00000000u

/* 0x02fc  SMI_SUB_PINFO31  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_SUB_PINFO31 0x170012fcu
#define MT6589_SMI_VENC_SMI_SUB_PINFO31_RESET 0x00000000u

/* 0x0300  SMI_MAU_ENTR0_START - SMI MAU range 0 start register This register is used to set up MAU range 0 start  [reset 0x00300000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_START 0x17001300u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_START_RESET 0x00300000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_START_MAUR0STR_SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_START_MAUR0STR_MASK  0x3fffffffu

/* 0x0304  SMI_MAU_ENTR0_END - SMI MAU range 1 end register This register is used to set up MAU range 0 end  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END 0x17001304u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_VPADDRCTRL_15_0_M_SHIFT 15
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_VPADDRCTRL_15_0_M_MASK  0x00008000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_E_SHIFT 14
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_E_MASK  0x00004000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_D_SHIFT 13
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_D_MASK  0x00002000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_I_SHIFT 12
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_I_MASK  0x00001000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_A_SHIFT 11
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_A_MASK  0x00000800u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_T_SHIFT 10
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_T_MASK  0x00000400u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_E_SHIFT 9
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_E_MASK  0x00000200u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_K_SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_END_K_MASK  0x000001ffu

/* 0x0308  SMI_MAU_ENTR0_GID_0 - SMI MAU range 1 invalid master register 0 This register is used to set MAU up range 0 invalid masters. (Refer to EMI# in smi_mux)  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_GID_0 0x17001308u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_GID_0_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_GID_0_R0INVLD31_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_GID_0_R0INVLD31_15_0__MASK  0x0000ffffu

/* 0x0310  SMI_MAU_ENTR1_START - SMI MAU range 1 start register This register is used to set up MAU range 1 start  [reset 0x00300000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_START 0x17001310u
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_START_RESET 0x00300000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_START_MAUR1STR_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_START_MAUR1STR_15_0__MASK  0x0000ffffu

/* 0x0314  SMI_MAU_ENTR1_END - SMI MAU range 1 end register This register is used to set up MAU range 1 end  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_END 0x17001314u
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_END_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_END_VPADDRCTRL_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_END_VPADDRCTRL_15_0__MASK  0x0000ffffu

/* 0x0318  SMI_MAU_ENTR1_GID_0 - SMI MAU range 1 invalid master register 0 This register is used to set up MAU range 1 invalid masters.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_GID_0 0x17001318u
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_GID_0_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_GID_0_R1INVLD31_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_GID_0_R1INVLD31_15_0__MASK  0x0000ffffu

/* 0x0320  SMI_MAU_ENTR2_START - SMI MAU range 2 start register This register is used to set up MAU range 2 start  [reset 0x00300000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_START 0x17001320u
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_START_RESET 0x00300000u

/* 0x0324  SMI_MAU_ENTR2_END - SMI MAU range 2 end register This register is used to set MAU range2 end  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_END 0x17001324u
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_END_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_END_VPADDRCTRL_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_END_VPADDRCTRL_15_0__MASK  0x0000ffffu

/* 0x0328  SMI_MAU_ENTR2_GID_0 - SMI MAU range 2 invalid master register 0 This register is used to set up MAU range 2 invalid masters. M E D I A T E K  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_GID_0 0x17001328u
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_GID_0_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_GID_0_R2INVLD31_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_GID_0_R2INVLD31_15_0__MASK  0x0000ffffu

/* 0x0400  SMI_LARB_MON_ENA - SMI LARB performance monitor enable This register is used to enable performance monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_ENA 0x17001400u
#define MT6589_SMI_VENC_SMI_LARB_MON_ENA_RESET 0x00000000u

/* 0x0404  SMI_LARB_MON_CLR - SMI LARB performance monitor clear This register is used to clear performance monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR 0x17001404u
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR_MONCLR_SHIFT 15
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR_MONCLR_MASK  0x00008000u
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR_TYPE_SHIFT 14
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR_TYPE_MASK  0x00004000u
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR_OTHER_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_CLR_OTHER_MASK  0x00003fffu

/* 0x0408  SMI_LARB_MON_PORT - SMI LARB monitor port selection register This register is used to select one port to monitor its performance.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_PORT 0x17001408u
#define MT6589_SMI_VENC_SMI_LARB_MON_PORT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_PORT_MONPSEL_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_PORT_MONPSEL_MASK  0x0000ffffu

/* 0x040c  SMI_LARB_MON_TYPE - SMI LARB monitor type selection register This register is used to select performance monitoring type.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_TYPE 0x1700140cu
#define MT6589_SMI_VENC_SMI_LARB_MON_TYPE_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_TYPE_MONTSEL_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_TYPE_MONTSEL_MASK  0x0000ffffu

/* 0x0410  SMI_LARB_MON_CON - SMI LARB performance monitor control register This register is used to control performance monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_CON 0x17001410u
#define MT6589_SMI_VENC_SMI_LARB_MON_CON_RESET 0x00000000u

/* 0x0420  SMI_LARB_MON_ACT_CNT - SMI LARB monitor active counter register This register is used to count active cycles for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_ACT_CNT 0x17001420u
#define MT6589_SMI_VENC_SMI_LARB_MON_ACT_CNT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_ACT_CNT_ACTCNT_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_ACT_CNT_ACTCNT_MASK  0xffffffffu

/* 0x0424  SMI_LARB_MON_REQ_CNT - SMI LARB monitor request counter register This register is used to count request cycles for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_REQ_CNT 0x17001424u
#define MT6589_SMI_VENC_SMI_LARB_MON_REQ_CNT_RESET 0x00000000u

/* 0x0424  SMI_LARB_M - SMI LARB Monitor Request Counter Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_M 0x17001424u
#define MT6589_SMI_VENC_SMI_LARB_M_RESET 0x00000000u

/* 0x0428  SMI_LARB_MON_IDL_CNT - SMI LARB monitor idle counter register This register is used to count idle cycles for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_IDL_CNT 0x17001428u
#define MT6589_SMI_VENC_SMI_LARB_MON_IDL_CNT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_IDL_CNT_IDLCNT_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_IDL_CNT_IDLCNT_MASK  0xffffffffu

/* 0x042c  SMI_LARB_MON_BEA_CNT - SMI LARB monitor beat counter register This register is used to count beats for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_BEA_CNT 0x1700142cu
#define MT6589_SMI_VENC_SMI_LARB_MON_BEA_CNT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_BEA_CNT_BEACNT_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_BEA_CNT_BEACNT_MASK  0xffffffffu

/* 0x0430  SMI_LARB_MON_BYT_CNT - SMI LARB monitor byte counter register This register is used to count bytes for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT 0x17001430u
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT_BYTCNT_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT_BYTCNT_15_0__MASK  0x0000ffffu
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT_CPCNT_31_16__SHIFT 16
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT_CPCNT_31_16__MASK  0xffff0000u
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT_CPCNT_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_BYT_CNT_CPCNT_15_0__MASK  0x0000ffffu

/* 0x0434  SMI_LARB_MON_CP_CNT - SMI LARB monitor command phase counter register This register is used to count command phase cycles for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_CP_CNT 0x17001434u
#define MT6589_SMI_VENC_SMI_LARB_MON_CP_CNT_RESET 0x00000000u

/* 0x0438  SMI_LARB_MON_DP_CNT - SMI LARB monitor data phase counter register This register is used to count data phase cycles for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_DP_CNT 0x17001438u
#define MT6589_SMI_VENC_SMI_LARB_MON_DP_CNT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_DP_CNT_DPCNT_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_DP_CNT_DPCNT_MASK  0xffffffffu

/* 0x043c  SMI_LARB_MON_CDP_MAX - SMI LARB monitor command/data phase maximum register This register is used to count command/data phase maximum cycles for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_CDP_MAX 0x1700143cu
#define MT6589_SMI_VENC_SMI_LARB_MON_CDP_MAX_RESET 0x00000000u

/* 0x0440  SMI_LARB_MON_COS_MAX - SMI LARB monitor outstanding transaction maximum register This register is used to count outstanding transaction's maximum cycles for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_COS_MAX 0x17001440u
#define MT6589_SMI_VENC_SMI_LARB_MON_COS_MAX_RESET 0x00000000u

/* 0x0450  SMI_LARB_MON_BUS_REQ0 - SMI LARB monitor bus request register 0 This register is used to monitor requests on GMC bus of master 0 ~ 31.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ0 0x17001450u
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ0_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ0_MONREQ0_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ0_MONREQ0_15_0__MASK  0x0000ffffu

/* 0x0454  SMI_LARB_MON_BUS_REQ1 - SMI LARB monitor bus request register 0 This register is used to monitor requests on GMC bus of master 32~63. M E D I A T E K  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ1 0x17001454u
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ1_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ1_MONREQ1_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_BUS_REQ1_MONREQ1_15_0__MASK  0x0000ffffu

/* 0x0460  SMI_LARB_MON_WDT_CNT - SMI LARB monitor write data counter register This register is used to count beats for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_WDT_CNT 0x17001460u
#define MT6589_SMI_VENC_SMI_LARB_MON_WDT_CNT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_WDT_CNT_BEACNT_SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_WDT_CNT_BEACNT_MASK  0xffffffffu

/* 0x0464  SMI_LARB_MON_RDT_CNT - SMI LARB monitor read data counter register This register is used to count beats for monitoring.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_RDT_CNT 0x17001464u
#define MT6589_SMI_VENC_SMI_LARB_MON_RDT_CNT_RESET 0x00000000u
#define MT6589_SMI_VENC_SMI_LARB_MON_RDT_CNT_BEACNT_15_0__SHIFT 0
#define MT6589_SMI_VENC_SMI_LARB_MON_RDT_CNT_BEACNT_15_0__MASK  0x0000ffffu

/* 0x0468  SMI_LARB_MON_OST_CNT - SMI LARB monitor outstanding transaction register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_LARB_MON_OST_CNT 0x17001468u
#define MT6589_SMI_VENC_SMI_LARB_MON_OST_CNT_RESET 0x00000000u

/* 0x0500  SMI_MAU_ENTR0_STAT - SMI MAU range 0 status register This register is used to read MAU range 0 status.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_STAT 0x17001500u
#define MT6589_SMI_VENC_SMI_MAU_ENTR0_STAT_RESET 0x00000000u

/* 0x0504  SMI_MAU_ENTR1_STAT - SMI MAU range 1 status register This register is used to read MAU range 1 status.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_STAT 0x17001504u
#define MT6589_SMI_VENC_SMI_MAU_ENTR1_STAT_RESET 0x00000000u

/* 0x0508  SMI_MAU_ENTR2_STAT - SMI MAU range 2 status register This register is used to read MAU range 2 status.  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_STAT 0x17001508u
#define MT6589_SMI_VENC_SMI_MAU_ENTR2_STAT_RESET 0x00000000u

/* 0x0520  SMI_IRQ_STATUS - SMI IRQ Status Register  [reset 0x00000000u] */
#define MT6589_SMI_VENC_SMI_IRQ_STATUS 0x17001520u
#define MT6589_SMI_VENC_SMI_IRQ_STATUS_RESET 0x00000000u

/* 0x0600  SMI_LARB_FIFO_STAT0 - SMI LARB EXTCON FIFO status  [reset 0x0000aaaau] */
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0 0x17001600u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_RESET 0x0000aaaau
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WRSP_EMP0_SHIFT 15
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WRSP_EMP0_MASK  0x00008000u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WRSP_FUL0_SHIFT 14
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WRSP_FUL0_MASK  0x00004000u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WR_EMP0_SHIFT 13
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WR_EMP0_MASK  0x00002000u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WR_FULL0_SHIFT 12
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_WR_FULL0_MASK  0x00001000u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_RRSP_EMP0_SHIFT 11
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_RRSP_EMP0_MASK  0x00000800u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_RRSP_FUL0_SHIFT 10
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_RRSP_FUL0_MASK  0x00000400u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_D_DAT_EMP0_SHIFT 9
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_D_DAT_EMP0_MASK  0x00000200u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_D_DAT_FUL0_SHIFT 8
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT0_D_DAT_FUL0_MASK  0x00000100u

/* 0x0604  SMI_LARB_FIFO_STAT1 - SMI LARB AXI OUT FIFO status  [reset 0x000002aau] */
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1 0x17001604u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_RESET 0x000002aau
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_B_EMP_SHIFT 9
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_B_EMP_MASK  0x00000200u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_B_FUL_SHIFT 8
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_B_FUL_MASK  0x00000100u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_R_EMP_SHIFT 7
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_R_EMP_MASK  0x00000080u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_R_FUL_SHIFT 6
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_R_FUL_MASK  0x00000040u
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_W_EMP_SHIFT 5
#define MT6589_SMI_VENC_SMI_LARB_FIFO_STAT1_AXI_W_EMP_MASK  0x00000020u

/* 0x0610  SMI_LARB_BUS_STAT0 - SMI LARB EXTCON bus status  [reset 0x000003e0u] */
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0 0x17001610u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_RESET 0x000003e0u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_BREADY0_SHIFT 9
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_BREADY0_MASK  0x00000200u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_RREADY0_SHIFT 8
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_RREADY0_MASK  0x00000100u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_WREADY0_SHIFT 7
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_WREADY0_MASK  0x00000080u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_AWREADY0_SHIFT 6
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_AWREADY0_MASK  0x00000040u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_ARREADY0_SHIFT 5
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_ARREADY0_MASK  0x00000020u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_BVALID0_SHIFT 4
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT0_BVALID0_MASK  0x00000010u

/* 0x0614  SMI_LARB_BUS_STAT1 - SMI LARB ASYNC bus status  [reset 0x000003e0u] */
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1 0x17001614u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_RESET 0x000003e0u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_BREADY0_SHIFT 9
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_BREADY0_MASK  0x00000200u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_RREADY0_SHIFT 8
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_RREADY0_MASK  0x00000100u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_WREADY0_SHIFT 7
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_WREADY0_MASK  0x00000080u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_AWREADY0_SHIFT 6
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_AWREADY0_MASK  0x00000040u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_ARREADY0_SHIFT 5
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_ARREADY0_MASK  0x00000020u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_BVALID0_SHIFT 4
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_BVALID0_MASK  0x00000010u
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_RVALID0_SHIFT 3
#define MT6589_SMI_VENC_SMI_LARB_BUS_STAT1_RVALID0_MASK  0x00000008u

/* 0x0700  SMI_LARB_DBG_MODE0 - SMI LARB debug mode register 0  [reset 0x17001704u] */
#define MT6589_SMI_VENC_SMI_LARB_DBG_MODE0 0x17001700u
#define MT6589_SMI_VENC_SMI_LARB_DBG_MODE0_RESET 0x17001704u

/* 0x0704  SMI_LARB_DBG_MODE1 - SMI LARB debug mode register 1  [reset 0x17001780u] */
#define MT6589_SMI_VENC_SMI_LARB_DBG_MODE1 0x17001704u
#define MT6589_SMI_VENC_SMI_LARB_DBG_MODE1_RESET 0x17001780u

/* 0x0780  SMI_LARB_TST_MODE0 - SMI LARB test mode register 0 */
#define MT6589_SMI_VENC_SMI_LARB_TST_MODE0 0x17001780u
#define MT6589_SMI_VENC_SMI_LARB_TST_MODE0_RESET 0x00000000u

#define MT6589_SMI_VENC_NUM_REGS 155

#endif /* HW_ARM_SMI_VENC_REGS_H */
