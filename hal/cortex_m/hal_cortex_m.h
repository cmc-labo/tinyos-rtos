/**
 * HAL — ARM Cortex-M implementation header
 *
 * Provides:
 *   - Memory-mapped register definitions (all 0xE000xxxx addresses in
 *     one place — never scattered across kernel source files again).
 *   - static inline implementations of the performance-critical HAL
 *     primitives so the compiler can inline them at every call site.
 *
 * Supported variants:
 *   Cortex-M0 / M0+  — no DWT cycle counter, no fault-status registers
 *   Cortex-M3 / M4   — DWT cycle counter, CFSR/HFSR/MMFAR/BFAR present
 *   Cortex-M7         — same as M3/M4, plus instruction/data caches
 *
 * Conditional compilation:
 *   Define CORTEX_M0 to suppress access to M3/M4/M7-only registers.
 *   Without the flag the code still works on M0/M0+ — DWT reads return
 *   0 and fault-status registers read as 0 (architecturally guaranteed).
 */

#ifndef HAL_CORTEX_M_H
#define HAL_CORTEX_M_H

#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 * SysTick  (all Cortex-M variants, ARMv6-M and ARMv7-M)
 * ==================================================================== */

#define HAL_CM_SYST_CSR    (*(volatile uint32_t *)0xE000E010U)
#define HAL_CM_SYST_RVR    (*(volatile uint32_t *)0xE000E014U)
#define HAL_CM_SYST_CVR    (*(volatile uint32_t *)0xE000E018U)

#define HAL_CM_SYST_ENABLE   (1UL << 0)  /**< Counter enable            */
#define HAL_CM_SYST_TICKINT  (1UL << 1)  /**< SysTick exception enable  */
#define HAL_CM_SYST_CLKSRC   (1UL << 2)  /**< 1 = processor clock       */

/* ====================================================================
 * System Control Block (SCB)
 * ==================================================================== */

/** Interrupt Control and State Register — used to pend PendSV */
#define HAL_CM_ICSR        (*(volatile uint32_t *)0xE000ED04U)
#define HAL_CM_ICSR_PENDSVSET   (1UL << 28)

/** System Handler Priority registers */
#define HAL_CM_SHPR2       (*(volatile uint32_t *)0xE000ED1CU)  /* SVCall  */
#define HAL_CM_SHPR3       (*(volatile uint32_t *)0xE000ED20U)  /* PendSV, SysTick */

/** SHPR3 field masks */
#define HAL_CM_PENDSV_PRIO_SHIFT   16U   /**< PendSV  priority field start */
#define HAL_CM_SYSTICK_PRIO_SHIFT  24U   /**< SysTick priority field start */

/** Application Interrupt and Reset Control Register */
#define HAL_CM_AIRCR       (*(volatile uint32_t *)0xE000ED0CU)
#define HAL_CM_AIRCR_VECTKEY    (0x05FAUL << 16)  /**< Write key            */
#define HAL_CM_AIRCR_SYSRESET   (1UL << 2)        /**< Request system reset */

/* ====================================================================
 * Fault Status Registers  (Cortex-M3/M4/M7 only; read as 0 on M0/M0+)
 * ==================================================================== */

/** Configurable Fault Status Register (UFSR | BFSR | MMFSR) */
#define HAL_CM_CFSR        (*(volatile uint32_t *)0xE000ED28U)
/** HardFault Status Register */
#define HAL_CM_HFSR        (*(volatile uint32_t *)0xE000ED2CU)
/** MemManage Fault Address Register */
#define HAL_CM_MMFAR       (*(volatile uint32_t *)0xE000ED34U)
/** BusFault Address Register */
#define HAL_CM_BFAR        (*(volatile uint32_t *)0xE000ED38U)

/** CFSR: MMFAR valid bit */
#define HAL_CM_CFSR_MMARVALID  (1UL << 7)
/** CFSR: BFAR valid bit */
#define HAL_CM_CFSR_BFARVALID  (1UL << 15)

/* ====================================================================
 * Memory Protection Unit (MPU)  (optional, M0+ / M3 / M4 / M7)
 * ==================================================================== */

#define HAL_CM_MPU_TYPE    (*(volatile uint32_t *)0xE000ED90U)
#define HAL_CM_MPU_CTRL    (*(volatile uint32_t *)0xE000ED94U)
#define HAL_CM_MPU_RNR     (*(volatile uint32_t *)0xE000ED98U)
#define HAL_CM_MPU_RBAR    (*(volatile uint32_t *)0xE000ED9CU)
#define HAL_CM_MPU_RASR    (*(volatile uint32_t *)0xE000EDA0U)

#define HAL_CM_MPU_CTRL_ENABLE     (1UL << 0)
#define HAL_CM_MPU_CTRL_HFNMIENA   (1UL << 1)
#define HAL_CM_MPU_CTRL_PRIVDEFENA (1UL << 2)

/* ====================================================================
 * DWT Data Watchpoint and Trace  (Cortex-M3/M4/M7; absent on M0/M0+)
 * ==================================================================== */

#define HAL_CM_DWT_CTRL    (*(volatile uint32_t *)0xE0001000U)
#define HAL_CM_DWT_CYCCNT  (*(volatile uint32_t *)0xE0001004U)
#define HAL_CM_DWT_CYCCNTENA  (1UL << 0)

/* ====================================================================
 * Inline HAL primitives
 *
 * Declared static inline so the compiler can inline them at every call
 * site without any function-call overhead — critical in interrupt
 * handlers and the scheduler hot path.
 * ==================================================================== */

/**
 * Disable all maskable interrupts and return the previous PRIMASK value.
 * Pair with hal_irq_restore() to implement a re-entrant critical section.
 */
static inline uint32_t hal_irq_save(void) {
    uint32_t primask;
    __asm__ volatile(
        "mrs %0, primask\n\t"
        "cpsid i\n"
        : "=r"(primask) : : "memory"
    );
    return primask;
}

/**
 * Restore the interrupt state saved by hal_irq_save().
 */
static inline void hal_irq_restore(uint32_t primask) {
    __asm__ volatile("msr primask, %0" : : "r"(primask) : "memory");
}

/**
 * Request a context switch by pending PendSV.
 * PendSV runs after all higher-priority ISRs have returned, making
 * context switches safe to trigger from any exception level.
 */
static inline void hal_context_switch_trigger(void) {
    HAL_CM_ICSR = HAL_CM_ICSR_PENDSVSET;
    __asm__ volatile("dsb\n\tisb" ::: "memory");
}

/**
 * Enter low-power sleep until the next unmasked interrupt fires (WFI).
 * If an interrupt is already pending, WFI acts as a NOP.
 */
static inline void hal_cpu_wait_for_interrupt(void) {
    __asm__ volatile("wfi" ::: "memory");
}

/** Data Synchronisation Barrier — ensure all memory accesses complete. */
static inline void hal_cpu_dsb(void) {
    __asm__ volatile("dsb" ::: "memory");
}

/** Instruction Synchronisation Barrier — flush the pipeline. */
static inline void hal_cpu_isb(void) {
    __asm__ volatile("isb" ::: "memory");
}

#endif /* HAL_CORTEX_M_H */
