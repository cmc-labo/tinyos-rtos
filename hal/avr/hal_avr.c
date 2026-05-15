/**
 * HAL — AVR (ATmega/ATtiny) implementation
 *
 * Implements the non-inline HAL functions declared in hal/hal.h:
 *   - Timer0 CTC tick source (init, suppress, unsuppress)
 *   - No hardware cycle counter (always returns 0)
 *   - No MPU (always returns false)
 *   - No configurable exception priorities
 *   - Software reset via watchdog
 *   - Fault capture (unsupported — no fault status registers on AVR)
 *   - Platform registry
 *
 * Timer0 CTC configuration:
 *   OCR0A = (core_clock / prescaler / tick_rate) - 1
 *   At 16 MHz / prescaler=64 / 1 kHz: OCR0A = (16000000/64/1000)-1 = 249
 */

#include "../hal.h"
#include <stddef.h>

/* Watchdog reset registers (ATmega-compatible) */
#define HAL_AVR_MCUSR   (*(volatile uint8_t *)0x55U)
#define HAL_AVR_WDTCSR  (*(volatile uint8_t *)0x60U)

#define HAL_AVR_WDTCSR_WDE    (1U << 3)
#define HAL_AVR_WDTCSR_WDCE   (1U << 4)
#define HAL_AVR_WDTCSR_WDP0   (1U << 0)  /* shortest timeout (~16 ms) */

/* Timer0 CTC prescaler codes for TCCR0B */
#define HAL_AVR_T0_CS_64    0x03U   /* clkIO / 64   */
#define HAL_AVR_T0_CS_256   0x04U   /* clkIO / 256  */
#define HAL_AVR_T0_CS_1024  0x05U   /* clkIO / 1024 */

/* TCCR0A WGM bits for CTC mode */
#define HAL_AVR_T0_WGM_CTC  0x02U   /* CTC (Clear Timer on Compare Match) */

static uint32_t g_core_clock_hz = 16000000UL;
static uint32_t g_tick_rate_hz  = 1000UL;

static const hal_platform_t *g_platform = NULL;

/* ====================================================================
 * HAL initialisation
 * ==================================================================== */

void hal_init(void) {
    /* AVR has no cycle counter to init. */
}

/* ====================================================================
 * Tick source — Timer0 CTC
 * ==================================================================== */

void hal_tick_init(uint32_t core_clock_hz, uint32_t tick_rate_hz) {
    g_core_clock_hz = core_clock_hz;
    g_tick_rate_hz  = tick_rate_hz;

    /* Choose a prescaler that keeps OCR0A in 0–255. */
    uint32_t prescaler;
    uint8_t  cs_bits;
    uint32_t ocr;

    /* Try prescalers in ascending order: 64, 256, 1024 */
    prescaler = 64UL;
    cs_bits   = HAL_AVR_T0_CS_64;
    ocr = (core_clock_hz / prescaler / tick_rate_hz) - 1UL;

    if (ocr > 255UL) {
        prescaler = 256UL;
        cs_bits   = HAL_AVR_T0_CS_256;
        ocr = (core_clock_hz / prescaler / tick_rate_hz) - 1UL;
    }
    if (ocr > 255UL) {
        prescaler = 1024UL;
        cs_bits   = HAL_AVR_T0_CS_1024;
        ocr = (core_clock_hz / prescaler / tick_rate_hz) - 1UL;
    }
    if (ocr > 255UL) {
        ocr = 255UL;   /* best-effort: clamp if tick_rate too slow */
    }

    HAL_AVR_TCCR0A = HAL_AVR_T0_WGM_CTC;
    HAL_AVR_OCR0A  = (uint8_t)ocr;
    HAL_AVR_TCNT0  = 0U;
    HAL_AVR_TIMSK0 |= HAL_AVR_TIMSK0_OCIE;   /* enable compare match IRQ */
    HAL_AVR_TCCR0B = cs_bits;                 /* start timer */
}

void hal_tick_suppress(uint32_t max_ticks) {
    (void)max_ticks;
    /* Stop Timer0 by clearing the clock-select bits. */
    HAL_AVR_TCCR0B = 0U;
}

uint32_t hal_tick_unsuppress(void) {
    /* Restart Timer0 with the same settings. */
    HAL_AVR_TCNT0  = 0U;
    uint32_t prescaler = 64UL;
    uint8_t  cs_bits   = HAL_AVR_T0_CS_64;
    uint32_t ocr = (g_core_clock_hz / prescaler / g_tick_rate_hz) - 1UL;
    if (ocr > 255UL) { prescaler = 256UL;  cs_bits = HAL_AVR_T0_CS_256;
                       ocr = (g_core_clock_hz / prescaler / g_tick_rate_hz) - 1UL; }
    if (ocr > 255UL) { prescaler = 1024UL; cs_bits = HAL_AVR_T0_CS_1024;
                       ocr = (g_core_clock_hz / prescaler / g_tick_rate_hz) - 1UL; }
    (void)ocr;
    HAL_AVR_TCCR0B = cs_bits;
    return 0;
}

uint32_t hal_core_clock_hz(void) {
    return g_core_clock_hz;
}

/* ====================================================================
 * Cycle counter — not available on AVR
 * ==================================================================== */

bool hal_cycle_counter_init(void) {
    return false;
}

void hal_cycle_counter_reset(void) {}

uint32_t hal_cycle_counter_read(void) {
    return 0;
}

/* ====================================================================
 * MPU — not available on AVR
 * ==================================================================== */

bool hal_mpu_init(uint8_t *regions) {
    if (regions != NULL) {
        *regions = 0U;
    }
    return false;
}

int hal_mpu_configure_region(uint8_t region, uint32_t base,
                              uint32_t size, uint32_t attrs) {
    (void)region; (void)base; (void)size; (void)attrs;
    return -1;
}

void hal_mpu_enable(bool allow_privileged_default) {
    (void)allow_privileged_default;
}

void hal_mpu_disable(void) {}

/* ====================================================================
 * Interrupt priority — AVR uses fixed hardware vectors; no NVIC.
 * ==================================================================== */

void hal_irq_set_priority(int irq_num, uint8_t priority) {
    (void)irq_num; (void)priority;
}

/* ====================================================================
 * System reset — Watchdog shortest timeout (~16 ms)
 * ==================================================================== */

void hal_system_reset(void) {
    /* Timed sequence to enable WDT: write WDCE+WDE, then configure */
    HAL_AVR_WDTCSR = HAL_AVR_WDTCSR_WDCE | HAL_AVR_WDTCSR_WDE;
    HAL_AVR_WDTCSR = HAL_AVR_WDTCSR_WDE | HAL_AVR_WDTCSR_WDP0;
    while (1) {}
}

/* ====================================================================
 * Fault information capture — AVR has no fault status hardware
 * ==================================================================== */

void hal_fault_capture(const uint32_t *frame, hal_fault_info_t *info) {
    if (info == NULL) {
        return;
    }
    /* AVR provides no hardware fault status registers. */
    info->pc         = frame ? frame[0] : 0U;
    info->lr         = 0U;
    info->sp         = 0U;
    info->status     = 0U;
    info->fault_addr = 0U;
    info->extra[0]   = 0U;
    info->extra[1]   = 0U;
    info->extra[2]   = 0U;
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
