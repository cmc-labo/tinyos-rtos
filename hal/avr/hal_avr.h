/**
 * HAL — AVR (ATmega/ATtiny) implementation header
 *
 * Architecture notes:
 *   - 8-bit architecture; HAL uses uint32_t padded to match the API contract.
 *   - Interrupt control: SREG bit 7 (I flag) — CLI/SEI instructions.
 *   - No hardware equivalent of PendSV; context switch is triggered by
 *     forcing Timer0 to overflow on the next cycle (TCNT0 = 0xFF).
 *   - No hardware cycle counter; hal_cycle_counter_read() returns 0.
 *   - No MPU; hal_mpu_init() always returns false.
 *   - Fault capture is unsupported; hal_fault_capture() is a no-op.
 *
 * Register addresses (ATmega328P / ATmega2560 compatible):
 *   0x5F  SREG    — status register
 *   0x53  SMCR    — sleep mode control
 *   0x46  TCNT0   — Timer0 counter
 *   0x44  TCCR0A  — Timer0 control A
 *   0x45  TCCR0B  — Timer0 control B
 *   0x6E  TIMSK0  — Timer0 interrupt mask
 */

#ifndef HAL_AVR_H
#define HAL_AVR_H

#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 * Memory-mapped I/O registers (ATmega-compatible)
 * ==================================================================== */

#define HAL_AVR_SREG    (*(volatile uint8_t *)0x5FU)
#define HAL_AVR_SMCR    (*(volatile uint8_t *)0x53U)
#define HAL_AVR_TCNT0   (*(volatile uint8_t *)0x46U)
#define HAL_AVR_TCCR0A  (*(volatile uint8_t *)0x44U)
#define HAL_AVR_TCCR0B  (*(volatile uint8_t *)0x45U)
#define HAL_AVR_TIMSK0  (*(volatile uint8_t *)0x6EU)
#define HAL_AVR_OCR0A   (*(volatile uint8_t *)0x47U)

#define HAL_AVR_SREG_I      (1U << 7)   /**< Global interrupt enable flag */
#define HAL_AVR_SMCR_SE     (1U << 0)   /**< Sleep enable bit             */
#define HAL_AVR_TIMSK0_TOIE (1U << 0)   /**< Timer0 overflow interrupt    */
#define HAL_AVR_TIMSK0_OCIE (1U << 1)   /**< Timer0 output compare match  */

/* ====================================================================
 * Inline HAL primitives
 * ==================================================================== */

/**
 * Disable global interrupts, return SREG so I-flag can be restored.
 * Padded to uint32_t to match the portable API signature.
 */
static inline uint32_t hal_irq_save(void) {
    uint8_t sreg = HAL_AVR_SREG;
    __asm__ volatile("cli" ::: "memory");
    return (uint32_t)sreg;
}

/**
 * Restore the interrupt state saved by hal_irq_save().
 */
static inline void hal_irq_restore(uint32_t sreg) {
    HAL_AVR_SREG = (uint8_t)sreg;
    __asm__ volatile("" ::: "memory");
}

/**
 * Request a context switch by forcing Timer0 to overflow on the next
 * clock cycle.  The Timer0 overflow ISR performs the actual switch.
 */
static inline void hal_context_switch_trigger(void) {
    HAL_AVR_TCNT0 = 0xFFU;
    __asm__ volatile("" ::: "memory");
}

/**
 * Enter idle sleep until the next interrupt.
 * Caller must ensure SMCR.SM selects idle mode before calling.
 */
static inline void hal_cpu_wait_for_interrupt(void) {
    HAL_AVR_SMCR |= HAL_AVR_SMCR_SE;
    __asm__ volatile("sleep" ::: "memory");
    HAL_AVR_SMCR &= (uint8_t)~HAL_AVR_SMCR_SE;
}

/** No memory barrier needed on AVR; NOP satisfies the call site. */
static inline void hal_cpu_dsb(void) {
    __asm__ volatile("nop" ::: "memory");
}

/** No instruction cache on AVR. */
static inline void hal_cpu_isb(void) {
    __asm__ volatile("nop" ::: "memory");
}

#endif /* HAL_AVR_H */
