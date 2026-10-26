/*
 * UART - generated from the MT6589 datasheet by
 * mt6589-emulator/tools/gen_regs_header.py.  DO NOT EDIT.
 *
 * base 0x11006000, 7 registers.
 */
#ifndef HW_ARM_UART_REGS_H
#define HW_ARM_UART_REGS_H

#define MT6589_UART_BASE 0x11006000u

/* 0x0004  UARTn_IER */
#define MT6589_UART_UARTN_IER 0x11006004u
#define MT6589_UART_UARTN_IER_RESET 0x00000000u

/* 0x0008  UARTn_FCR */
#define MT6589_UART_UARTN_FCR 0x11006008u
#define MT6589_UART_UARTN_FCR_RESET 0x00000000u

/* 0x000c  UARTn_LCR */
#define MT6589_UART_UARTN_LCR 0x1100600cu
#define MT6589_UART_UARTN_LCR_RESET 0x00000000u

/* 0x0010  UARTn_MCR */
#define MT6589_UART_UARTN_MCR 0x11006010u
#define MT6589_UART_UARTN_MCR_RESET 0x00000000u

/* 0x0014  UARTn_LSR */
#define MT6589_UART_UARTN_LSR 0x11006014u
#define MT6589_UART_UARTN_LSR_RESET 0x00000000u

/* 0x0018  UARTn_MSR */
#define MT6589_UART_UARTN_MSR 0x11006018u
#define MT6589_UART_UARTN_MSR_RESET 0x00000000u

/* 0x001c  UARTn_SCR */
#define MT6589_UART_UARTN_SCR 0x1100601cu
#define MT6589_UART_UARTN_SCR_RESET 0x00000000u

/*
 * One entry per register, for a generic read/write stub:
 *
 *   static const Mt6589RegInfo uart_regs[] = {
 *       MT6589_REGINFO(MT6589_UART, GPT_IRQEN),
 *       ...
 *   };
 */
#define MT6589_UART_NUM_REGS 7

#endif /* HW_ARM_UART_REGS_H */
