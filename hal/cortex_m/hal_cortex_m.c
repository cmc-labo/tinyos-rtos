/**
 * HAL — ARM Cortex-M implementation
 *
 * Implements the non-inline HAL functions declared in hal/hal.h:
 *   - SysTick tick source (init, suppress, unsuppress)
 *   - DWT cycle counter
 *   - MPU region management
 *   - Exception priority control
 *   - System reset
 *   - Fault info capture
 *   - Platform registry
 */

#include "../hal.h"
#include <stddef.h>

/* Cached tick configuration (used by hal_tick_suppress / unsuppress) */
static uint32_t g_core_clock_hz = 168000000UL;
static uint32_t g_tick_rate_hz  = 1000UL;

static const hal_platform_t *g_platform = NULL;

/* ====================================================================
 * HAL initialisation
 * ==================================================================== */

void hal_init(void) {
    hal_cycle_counter_init();
}

/* ====================================================================
 * Tick source — SysTick
 * ==================================================================== */

void hal_tick_init(uint32_t core_clock_hz, uint32_t tick_rate_hz) {
    g_core_clock_hz = core_clock_hz;
    g_tick_rate_hz  = tick_rate_hz;

    /* Set exception priorities:
     *   PendSV  = 255 (lowest)  — runs after all other ISRs return
     *   SysTick = 192           — higher than PendSV so tick counting
     *                              is not delayed by a context switch  */
    HAL_CM_SHPR3 = (0xFFUL << HAL_CM_PENDSV_PRIO_SHIFT)
                 | (0xC0UL << HAL_CM_SYSTICK_PRIO_SHIFT);

    /* Program SysTick:
     *   reload = (core_clock / tick_rate) - 1
     *   e.g.  168 000 000 / 1000 - 1 = 167 999 at 168 MHz / 1 kHz   */
    HAL_CM_SYST_RVR = (core_clock_hz / tick_rate_hz) - 1UL;
    HAL_CM_SYST_CVR = 0UL;
    HAL_CM_SYST_CSR = HAL_CM_SYST_CLKSRC   /* processor clock source  */
                    | HAL_CM_SYST_TICKINT   /* enable SysTick IRQ      */
                    | HAL_CM_SYST_ENABLE;   /* start counter           */
}

void hal_tick_suppress(uint32_t max_ticks) {
    (void)max_ticks;
    /* Stop SysTick — no more tick interrupts until hal_tick_unsuppress().
     * The caller uses the DWT cycle counter to measure elapsed time.   */
    HAL_CM_SYST_CSR &= ~HAL_CM_SYST_ENABLE;
}

uint32_t hal_tick_unsuppress(void) {
    /* Restart SysTick from a clean period boundary. */
    HAL_CM_SYST_CVR  = 0UL;
    HAL_CM_SYST_CSR |= HAL_CM_SYST_ENABLE;
    /* Elapsed tick count is computed by the caller from the cycle
     * counter (hal_cycle_counter_read / hal_core_clock_hz).           */
    return 0;
}

uint32_t hal_core_clock_hz(void) {
    return g_core_clock_hz;
}

/* ====================================================================
 * DWT cycle counter  (M3/M4/M7; reads as 0 on M0/M0+)
 * ==================================================================== */

bool hal_cycle_counter_init(void) {
    HAL_CM_DWT_CTRL |= HAL_CM_DWT_CYCCNTENA;
    /* Return true only if the enable bit actually stuck */
    return (HAL_CM_DWT_CTRL & HAL_CM_DWT_CYCCNTENA) != 0U;
}

void hal_cycle_counter_reset(void) {
    HAL_CM_DWT_CYCCNT = 0UL;
}

uint32_t hal_cycle_counter_read(void) {
    return HAL_CM_DWT_CYCCNT;
}

/* ====================================================================
 * Memory Protection Unit
 * ==================================================================== */

bool hal_mpu_init(uint8_t *regions) {
    uint32_t type  = HAL_CM_MPU_TYPE;
    uint8_t  count = (uint8_t)((type >> 8) & 0xFFU);
    if (regions != NULL) {
        *regions = count;
    }
    return count > 0U;
}

int hal_mpu_configure_region(uint8_t region, uint32_t base,
                              uint32_t size, uint32_t attrs) {
    (void)size;   /* SIZE field is encoded inside attrs (RASR) by the caller */
    HAL_CM_MPU_RNR  = region;
    HAL_CM_MPU_RBAR = base;
    HAL_CM_MPU_RASR = attrs;
    return 0;
}

void hal_mpu_enable(bool allow_privileged_default) {
    uint32_t ctrl = HAL_CM_MPU_CTRL_ENABLE | HAL_CM_MPU_CTRL_HFNMIENA;
    if (allow_privileged_default) {
        ctrl |= HAL_CM_MPU_CTRL_PRIVDEFENA;
    }
    hal_cpu_dsb();
    HAL_CM_MPU_CTRL = ctrl;
    hal_cpu_isb();
}

void hal_mpu_disable(void) {
    HAL_CM_MPU_CTRL &= ~HAL_CM_MPU_CTRL_ENABLE;
    hal_cpu_dsb();
}

/* ====================================================================
 * Interrupt / exception priority
 * ==================================================================== */

/**
 * Exception numbers (negative = core, matching CMSIS convention):
 *   -2  PendSV
 *   -1  SysTick
 *    0+ peripheral IRQ
 */
void hal_irq_set_priority(int irq_num, uint8_t priority) {
    if (irq_num == -2) {
        /* PendSV — SHPR3 bits [23:16] */
        HAL_CM_SHPR3 = (HAL_CM_SHPR3 & ~(0xFFUL << HAL_CM_PENDSV_PRIO_SHIFT))
                     | ((uint32_t)priority << HAL_CM_PENDSV_PRIO_SHIFT);
    } else if (irq_num == -1) {
        /* SysTick — SHPR3 bits [31:24] */
        HAL_CM_SHPR3 = (HAL_CM_SHPR3 & ~(0xFFUL << HAL_CM_SYSTICK_PRIO_SHIFT))
                     | ((uint32_t)priority << HAL_CM_SYSTICK_PRIO_SHIFT);
    }
    /* Peripheral IRQ priorities (NVIC->IPR[n]) intentionally not
     * included here — add platform-specific code if needed.           */
}

/* ====================================================================
 * System reset
 * ==================================================================== */

void hal_system_reset(void) {
    hal_cpu_dsb();
    HAL_CM_AIRCR = HAL_CM_AIRCR_VECTKEY
                 | (HAL_CM_AIRCR & 0x0700U)   /* preserve PRIGROUP    */
                 | HAL_CM_AIRCR_SYSRESET;
    hal_cpu_isb();
    while (1) {}   /* Unreachable — silences compiler warnings */
}

/* ====================================================================
 * Fault information capture
 * ==================================================================== */

/**
 * ARM Cortex-M exception frame (pushed onto the stack by hardware):
 *   Offset  Register
 *      0    R0
 *      4    R1
 *      8    R2
 *     12    R3
 *     16    R12
 *     20    LR
 *     24    PC  ← faulting address
 *     28    xPSR
 */
void hal_fault_capture(const uint32_t *frame, hal_fault_info_t *info) {
    if (info == NULL) {
        return;
    }
    info->pc         = frame[6];                           /* PC  */
    info->lr         = frame[5];                           /* LR  */
    info->sp         = (uint32_t)(uintptr_t)(frame + 8);  /* SP after frame */
    info->status     = HAL_CM_CFSR;
    info->extra[0]   = HAL_CM_HFSR;
    info->extra[1]   = HAL_CM_MMFAR;
    info->extra[2]   = HAL_CM_BFAR;
    /* Populate fault_addr from whichever address register is valid */
    if (HAL_CM_CFSR & HAL_CM_CFSR_MMARVALID) {
        info->fault_addr = HAL_CM_MMFAR;
    } else if (HAL_CM_CFSR & HAL_CM_CFSR_BFARVALID) {
        info->fault_addr = HAL_CM_BFAR;
    } else {
        info->fault_addr = 0UL;
    }
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
