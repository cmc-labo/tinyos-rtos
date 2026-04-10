/**
 * @file fault.h
 * @brief TinyOS Exception/Fault Handler
 *
 * Catches ARM Cortex-M fault exceptions and records which task was running,
 * where the fault occurred, and what caused it.
 *
 * Handled exceptions:
 *   HardFault   — all Cortex-M targets (escalated or direct)
 *   MemManage   — MPU / memory-protection violation (M3/M4/M7)
 *   BusFault    — invalid memory access, bus error (M3/M4/M7)
 *   UsageFault  — divide by zero, undefined instruction, etc. (M3/M4/M7)
 *
 * Usage:
 *   // Optional: override the hook for custom recovery (reset, log, etc.)
 *   void os_fault_hook(const fault_info_t *info) {
 *       my_log_crash(info->task_name, info->pc);
 *       NVIC_SystemReset();
 *   }
 *
 *   // Query the last fault from the shell or application code
 *   const fault_info_t *fi = os_fault_get_info();
 *   if (fi->occurred) { ... }
 */

#ifndef TINYOS_FAULT_H
#define TINYOS_FAULT_H

#include "../tinyos.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Fault type constants
 * (values 1-4 are also hardcoded in the assembly trampolines in fault.c)
 *===========================================================================*/

#define FAULT_NONE   0  /**< No fault recorded yet                          */
#define FAULT_HARD   1  /**< HardFault                                      */
#define FAULT_MEM    2  /**< MemManage — MPU / memory-protection violation  */
#define FAULT_BUS    3  /**< BusFault  — bus / memory access error          */
#define FAULT_USAGE  4  /**< UsageFault — div/0, undefined instr., etc.     */

/*===========================================================================
 * Fault cause detail flags (bitmask — multiple may be set simultaneously)
 *===========================================================================*/

#define FAULT_CAUSE_NONE       0u
#define FAULT_CAUSE_DIVZERO   (1u << 0)  /**< Divide by zero                */
#define FAULT_CAUSE_UNDEF_INS (1u << 1)  /**< Undefined instruction         */
#define FAULT_CAUSE_INV_STATE (1u << 2)  /**< Invalid CPU state (bad EPSR)  */
#define FAULT_CAUSE_UNALIGNED (1u << 3)  /**< Unaligned memory access       */
#define FAULT_CAUSE_DACC_VIOL (1u << 4)  /**< Data access violation (MPU)   */
#define FAULT_CAUSE_IACC_VIOL (1u << 5)  /**< Instruction access violation  */
#define FAULT_CAUSE_BUS_PREC  (1u << 6)  /**< Precise bus fault (BFAR valid)*/
#define FAULT_CAUSE_BUS_IMPREC (1u << 7) /**< Imprecise bus fault           */

/*===========================================================================
 * Fault information record
 *===========================================================================*/

typedef struct {
    bool     occurred;       /**< true once a fault has been recorded        */
    uint8_t  type;           /**< FAULT_* constant above                     */
    uint32_t cause;          /**< FAULT_CAUSE_* bitmask                      */

    /* Stacked register values (captured automatically by CPU) */
    uint32_t pc;             /**< Program Counter — faulting instruction     */
    uint32_t lr;             /**< Link Register at the time of the fault     */
    uint32_t psr;            /**< xPSR at the time of the fault              */

    /* Fault status registers (Cortex-M3/M4/M7 only; 0 on M0/M0+) */
    uint32_t cfsr;           /**< Configurable Fault Status Register         */
    uint32_t hfsr;           /**< HardFault Status Register                  */
    uint32_t fault_addr;     /**< Faulting address (MMFAR or BFAR if valid)  */

    /* Context */
    char     task_name[16];  /**< Name of the task that was running          */
    uint32_t tick;           /**< os_get_tick_count() when fault occurred    */
} fault_info_t;

/*===========================================================================
 * Public API
 *===========================================================================*/

/**
 * @brief Return a pointer to the last recorded fault.
 *        The record is valid until the next call to os_fault_clear().
 *        Check fault_info_t::occurred before using.
 */
const fault_info_t *os_fault_get_info(void);

/**
 * @brief Clear the fault record (set occurred = false).
 */
void os_fault_clear(void);

/**
 * @brief Fault hook — called once from os_fault_dispatch() after the fault
 *        record has been filled in.
 *
 *        The default weak implementation halts with a debug breakpoint
 *        (bkpt #2).  Override this function to perform custom recovery
 *        such as logging and system reset.
 *
 * @param info  Pointer to the fault record (always non-NULL).
 */
void os_fault_hook(const fault_info_t *info);

/*===========================================================================
 * Internal: exception entry points (placed in vector table by linker)
 * Do not call these directly.
 *===========================================================================*/

void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);

/**
 * @brief C-level dispatcher — called by the assembly trampolines.
 *        Exposed here so it has external linkage; not intended for direct use.
 */
void os_fault_dispatch(uint32_t *frame, uint32_t type);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_FAULT_H */
