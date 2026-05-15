/**
 * HAL — RISC-V RV32I/RV32IM implementation header
 *
 * Register conventions:
 *   mstatus.MIE (bit 3) — global machine interrupt enable
 *   mcause              — exception/interrupt cause
 *   mtval               — trap value (bad address or instruction)
 *   mepc                — exception program counter
 *   rdcycle             — cycle counter CSR (may read as 0 if not wired)
 *
 * Platform assumptions (standard SiFive / QEMU RISC-V layout):
 *   CLINT base: 0x02000000
 *     +0x0000  MSIP[0]     — machine software interrupt pending (hart 0)
 *     +0x4000  MTIMECMP[0] — machine timer compare (hart 0, 64-bit)
 *     +0xBFF8  MTIME       — machine timer (global, 64-bit)
 *
 * Context switch strategy:
 *   hal_context_switch_trigger() raises the machine software interrupt
 *   (MSIP bit) so the MSWI handler can perform the context switch, mirroring
 *   the role PendSV plays on Cortex-M.
 */

#ifndef HAL_RISCV_H
#define HAL_RISCV_H

#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 * CLINT — Core Local Interruptor
 * ==================================================================== */

#define HAL_RV_CLINT_BASE       0x02000000UL

#define HAL_RV_CLINT_MSIP       (*(volatile uint32_t *)(HAL_RV_CLINT_BASE + 0x0000U))
#define HAL_RV_CLINT_MTIMECMP   (*(volatile uint64_t *)(HAL_RV_CLINT_BASE + 0x4000U))
#define HAL_RV_CLINT_MTIME      (*(volatile uint64_t *)(HAL_RV_CLINT_BASE + 0xBFF8U))

/* mstatus register bits */
#define HAL_RV_MSTATUS_MIE   (1UL << 3)   /**< Machine interrupt enable */
#define HAL_RV_MSTATUS_MPIE  (1UL << 7)   /**< Previous MIE              */

/* mie / mip register bits */
#define HAL_RV_MIE_MSIE  (1UL << 3)   /**< Machine software interrupt enable */
#define HAL_RV_MIE_MTIE  (1UL << 7)   /**< Machine timer interrupt enable    */
#define HAL_RV_MIE_MEIE  (1UL << 11)  /**< Machine external interrupt enable */

/* ====================================================================
 * Inline HAL primitives
 * ==================================================================== */

/**
 * Disable all maskable interrupts and return the previous mstatus value.
 * Uses CSRRCI to atomically clear MIE and return the old mstatus.
 */
static inline uint32_t hal_irq_save(void) {
    uint32_t mstatus;
    __asm__ volatile(
        "csrrci %0, mstatus, 0x8\n"
        : "=r"(mstatus) : : "memory"
    );
    return mstatus;
}

/**
 * Restore interrupt state saved by hal_irq_save().
 */
static inline void hal_irq_restore(uint32_t mstatus) {
    __asm__ volatile(
        "csrw mstatus, %0\n"
        : : "r"(mstatus) : "memory"
    );
}

/**
 * Request a context switch by raising the machine software interrupt.
 * The MSWI handler clears MSIP and performs the actual switch.
 */
static inline void hal_context_switch_trigger(void) {
    HAL_RV_CLINT_MSIP = 1U;
    __asm__ volatile("fence" ::: "memory");
}

/**
 * Enter low-power sleep until the next unmasked interrupt fires.
 */
static inline void hal_cpu_wait_for_interrupt(void) {
    __asm__ volatile("wfi" ::: "memory");
}

/** Memory/store fence — ensures all prior stores are visible. */
static inline void hal_cpu_dsb(void) {
    __asm__ volatile("fence" ::: "memory");
}

/** Instruction fence — flushes the instruction pipeline. */
static inline void hal_cpu_isb(void) {
    __asm__ volatile("fence.i" ::: "memory");
}

#endif /* HAL_RISCV_H */
