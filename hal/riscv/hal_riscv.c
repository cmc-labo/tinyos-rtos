/**
 * HAL — RISC-V RV32I/RV32IM implementation
 *
 * Implements the non-inline HAL functions declared in hal/hal.h:
 *   - CLINT timer tick source (init, suppress, unsuppress)
 *   - rdcycle cycle counter
 *   - PMP region management
 *   - Machine-mode interrupt priority (CLINT/PLIC)
 *   - System reset via WFI spin (no standard RISC-V reset register)
 *   - Fault info capture from trap frame
 *   - Platform registry
 */

#include "../hal.h"
#include <stddef.h>

static uint32_t g_core_clock_hz = 32000000UL;
static uint32_t g_tick_rate_hz  = 1000UL;

static const hal_platform_t *g_platform = NULL;

/* ====================================================================
 * HAL initialisation
 * ==================================================================== */

void hal_init(void) {
    hal_cycle_counter_init();
}

/* ====================================================================
 * Tick source — CLINT MTIMECMP
 * ==================================================================== */

void hal_tick_init(uint32_t core_clock_hz, uint32_t tick_rate_hz) {
    g_core_clock_hz = core_clock_hz;
    g_tick_rate_hz  = tick_rate_hz;

    /* Enable machine software interrupt (for context switch via MSIP) and
     * machine timer interrupt (for tick).                                 */
    uint32_t mie;
    __asm__ volatile("csrr %0, mie" : "=r"(mie));
    mie |= HAL_RV_MIE_MSIE | HAL_RV_MIE_MTIE;
    __asm__ volatile("csrw mie, %0" : : "r"(mie));

    /* Program MTIMECMP for the first tick period.
     *   period_cycles = core_clock_hz / tick_rate_hz                     */
    uint64_t period = (uint64_t)core_clock_hz / tick_rate_hz;
    HAL_RV_CLINT_MTIMECMP = HAL_RV_CLINT_MTIME + period;
}

void hal_tick_suppress(uint32_t max_ticks) {
    /* Advance MTIMECMP far into the future (up to max_ticks periods). */
    uint64_t period = (uint64_t)g_core_clock_hz / g_tick_rate_hz;
    HAL_RV_CLINT_MTIMECMP = HAL_RV_CLINT_MTIME + period * (uint64_t)max_ticks;
}

uint32_t hal_tick_unsuppress(void) {
    /* Restore normal single-period MTIMECMP. */
    uint64_t period = (uint64_t)g_core_clock_hz / g_tick_rate_hz;
    HAL_RV_CLINT_MTIMECMP = HAL_RV_CLINT_MTIME + period;
    return 0;
}

uint32_t hal_core_clock_hz(void) {
    return g_core_clock_hz;
}

/* ====================================================================
 * Cycle counter — rdcycle CSR
 * Reads as 0 on implementations that do not wire mcycle to hardware.
 * ==================================================================== */

bool hal_cycle_counter_init(void) {
    uint32_t val;
    __asm__ volatile("rdcycle %0" : "=r"(val));
    /* If rdcycle returns a constant 0, the counter is not available. */
    return val != 0U;
}

void hal_cycle_counter_reset(void) {
    /* mcycle is not writable from M-mode on most implementations.
     * Record a snapshot instead; callers subtract the base value.       */
}

uint32_t hal_cycle_counter_read(void) {
    uint32_t val;
    __asm__ volatile("rdcycle %0" : "=r"(val));
    return val;
}

/* ====================================================================
 * Physical Memory Protection (PMP)
 * ==================================================================== */

bool hal_mpu_init(uint8_t *regions) {
    /* Standard RISC-V supports up to 16 PMP regions.
     * Probe by attempting to write pmpaddr0; if it sticks, PMP exists.  */
    uint32_t old_pmpaddr;
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(old_pmpaddr));
    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(0xFFFFFFFFUL));
    uint32_t test;
    __asm__ volatile("csrr %0, pmpaddr0" : "=r"(test));
    __asm__ volatile("csrw pmpaddr0, %0" : : "r"(old_pmpaddr));

    if (regions != NULL) {
        *regions = (test != 0U) ? 16U : 0U;
    }
    return test != 0U;
}

int hal_mpu_configure_region(uint8_t region, uint32_t base,
                              uint32_t size, uint32_t attrs) {
    (void)size;   /* Size is encoded in attrs (pmpcfgN) by the caller */

    /* pmpaddr holds the address shifted right by 2 (word-aligned).
     * The caller encodes the TOR/NA4/NAPOT form in attrs.              */
    uint32_t pmpaddr = base >> 2;

    /* Only regions 0–15 are valid; use a switch or runtime CSR write.
     * GCC/Clang require a compile-time CSR number so we use inline asm
     * with a jump table approach — simplified here to regions 0–3.     */
    switch (region & 0x3U) {
        case 0: __asm__ volatile("csrw pmpaddr0, %0" : : "r"(pmpaddr)); break;
        case 1: __asm__ volatile("csrw pmpaddr1, %0" : : "r"(pmpaddr)); break;
        case 2: __asm__ volatile("csrw pmpaddr2, %0" : : "r"(pmpaddr)); break;
        case 3: __asm__ volatile("csrw pmpaddr3, %0" : : "r"(pmpaddr)); break;
        default: return -1;
    }

    /* Write pmpcfg0 (covers regions 0–3, one byte each).               */
    uint32_t pmpcfg;
    __asm__ volatile("csrr %0, pmpcfg0" : "=r"(pmpcfg));
    uint8_t shift = (region & 0x3U) * 8U;
    pmpcfg = (pmpcfg & ~(0xFFUL << shift)) | ((attrs & 0xFFUL) << shift);
    __asm__ volatile("csrw pmpcfg0, %0" : : "r"(pmpcfg));

    return 0;
}

void hal_mpu_enable(bool allow_privileged_default) {
    (void)allow_privileged_default;
    /* PMP is always active in M-mode when entries have the L (lock) bit.
     * No global enable bit; individual regions are activated by pmpcfg.  */
}

void hal_mpu_disable(void) {
    /* Clear all pmpcfg0 entries to disable regions 0–3. */
    __asm__ volatile("csrw pmpcfg0, zero");
}

/* ====================================================================
 * Interrupt priority — PLIC (Platform-Level Interrupt Controller)
 * Standard PLIC base: 0x0C000000
 * Priority registers: base + 4 * irq_num
 * ==================================================================== */

#define HAL_RV_PLIC_BASE  0x0C000000UL

void hal_irq_set_priority(int irq_num, uint8_t priority) {
    if (irq_num < 0) {
        /* Machine exceptions have no PLIC-configurable priority.
         * Software (MSIP) and timer (MTIP) are always enabled via mie.  */
        return;
    }
    volatile uint32_t *prio =
        (volatile uint32_t *)(HAL_RV_PLIC_BASE + 4UL * (uint32_t)irq_num);
    *prio = priority;
}

/* ====================================================================
 * System reset — no standard register; disable interrupts and spin.
 * Real implementations override this with SoC-specific WDT trigger.
 * ==================================================================== */

void hal_system_reset(void) {
    __asm__ volatile("csrci mstatus, 0x8" ::: "memory");
    while (1) {
        __asm__ volatile("wfi");
    }
}

/* ====================================================================
 * Fault information capture
 *
 * RISC-V trap frame layout (as pushed by the trap entry stub):
 *   Offset  Register
 *      0    ra  (x1)
 *      4    sp  (x2)   ← stack pointer at fault
 *      8    gp  (x3)
 *     ...
 *     120   t6  (x31)
 *
 * mepc / mcause / mtval are read directly from CSRs, not the frame.
 * ==================================================================== */

void hal_fault_capture(const uint32_t *frame, hal_fault_info_t *info) {
    if (info == NULL) {
        return;
    }

    uint32_t mepc, mcause, mtval;
    __asm__ volatile("csrr %0, mepc"   : "=r"(mepc));
    __asm__ volatile("csrr %0, mcause" : "=r"(mcause));
    __asm__ volatile("csrr %0, mtval"  : "=r"(mtval));

    info->pc         = mepc;
    info->lr         = frame ? frame[0] : 0U;   /* ra = frame[0] */
    info->sp         = frame ? frame[1] : 0U;   /* sp = frame[1] */
    info->status     = mcause;
    info->fault_addr = mtval;
    info->extra[0]   = mcause;
    info->extra[1]   = mtval;
    info->extra[2]   = mepc;
}

/* ====================================================================
 * Platform registry
 * ==================================================================== */

void hal_platform_register(const hal_platform_t *platform) {
    g_platform = platform;
}

const hal_platform_t *hal_platform_get(void) {
    return g_platform;
}
