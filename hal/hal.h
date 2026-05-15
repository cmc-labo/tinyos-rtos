/**
 * TinyOS Hardware Abstraction Layer (HAL)
 *
 * This header is the single, architecture-agnostic interface between the
 * portable TinyOS kernel and the hardware it runs on.  All
 * platform-specific details (register addresses, instruction encodings,
 * timer hardware) are hidden behind this API.
 *
 * Supported platforms (select one via -D flag or Makefile):
 *   HAL_ARCH_CORTEX_M   — ARM Cortex-M0/M0+/M3/M4/M7
 *   HAL_ARCH_RISCV      — RISC-V RV32I / RV32IM
 *   HAL_ARCH_AVR        — Atmel AVR (ATmega / ATtiny)
 *
 * Usage
 * -----
 * 1. Add -DHAL_ARCH_CORTEX_M (or _RISCV, _AVR) to your CFLAGS.
 * 2. Include this header once at the top of each kernel source file.
 * 3. Call hal_init() before os_start().
 * 4. Optionally fill a hal_platform_t and call hal_platform_register()
 *    to provide board-level UART, flash, GPIO, and power callbacks.
 */

#ifndef TINYOS_HAL_H
#define TINYOS_HAL_H

#include <stdint.h>
#include <stdbool.h>

/* ====================================================================
 * Architecture selection
 * ==================================================================== */

#if defined(HAL_ARCH_CORTEX_M)
#  include "cortex_m/hal_cortex_m.h"
#elif defined(HAL_ARCH_RISCV)
#  include "riscv/hal_riscv.h"
#elif defined(HAL_ARCH_AVR)
#  include "avr/hal_avr.h"
#else
#  error "No HAL architecture selected. " \
         "Add -DHAL_ARCH_CORTEX_M, -DHAL_ARCH_RISCV, or -DHAL_ARCH_AVR to CFLAGS."
#endif

/* ====================================================================
 * Peripheral operation tables
 *
 * Board-support code fills a hal_platform_t and registers it with
 * hal_platform_register().  Kernel subsystems query available
 * peripherals via hal_platform_get(); a NULL function pointer means
 * "not provided on this board".
 * ==================================================================== */

/** UART / serial port operations */
typedef struct {
    int  (*init)(uint32_t baud_rate);
    void (*putc)(char c);
    int  (*getc)(void);        /**< -1 when no data available   */
    bool (*rx_ready)(void);
    void (*deinit)(void);
} hal_uart_ops_t;

/** Non-volatile storage operations */
typedef struct {
    int      (*init)(void);
    int      (*read)(uint32_t addr, void *buf, uint32_t len);
    int      (*write)(uint32_t addr, const void *buf, uint32_t len);
    int      (*erase)(uint32_t sector);
    uint32_t (*sector_size)(void);
    uint32_t (*total_size)(void);
    void     (*deinit)(void);
} hal_flash_ops_t;

/** GPIO operations */
typedef struct {
    int  (*init)(void);
    int  (*config)(uint32_t pin, uint32_t mode, uint32_t pull);
    void (*write)(uint32_t pin, bool value);
    bool (*read)(uint32_t pin);
    int  (*irq_enable)(uint32_t pin, uint32_t trigger, void (*cb)(void));
    void (*irq_disable)(uint32_t pin);
} hal_gpio_ops_t;

/** SPI bus operations */
typedef struct {
    int  (*init)(uint32_t freq_hz, uint8_t mode);
    int  (*transfer)(const void *tx, void *rx, uint32_t len, uint32_t cs_pin);
    void (*deinit)(void);
} hal_spi_ops_t;

/** I²C bus operations */
typedef struct {
    int  (*init)(uint32_t freq_hz);
    int  (*write)(uint8_t addr, const void *buf, uint32_t len, bool stop);
    int  (*read)(uint8_t addr, void *buf, uint32_t len);
    void (*deinit)(void);
} hal_i2c_ops_t;

/**
 * Platform-specific power hooks.
 *
 * These replace (and subsume) the legacy platform_enter_sleep_mode()
 * family of weak symbols.  Provide a hal_power_ops_t in hal_platform_t
 * for chip-specific sleep modes, clock gating, and wakeup sources.
 * NULL pointers fall back to the HAL's built-in WFI-based defaults.
 */
typedef struct {
    void     (*enter_sleep)(void);          /**< light sleep (e.g. WFI)  */
    void     (*enter_deep_sleep)(void);     /**< deep sleep / stop mode  */
    void     (*set_clock_hz)(uint32_t hz);  /**< change core clock freq  */
    void     (*enable_wakeup)(uint32_t src);
    void     (*disable_wakeup)(uint32_t src);
    uint32_t (*consumption_mw)(void);       /**< current draw estimate   */
} hal_power_ops_t;

/** Complete board/platform descriptor */
typedef struct {
    const hal_uart_ops_t  *uart[4];   /**< UART 0-3  (NULL = absent) */
    const hal_flash_ops_t *flash;     /**< primary NVM               */
    const hal_gpio_ops_t  *gpio;      /**< GPIO bank                 */
    const hal_spi_ops_t   *spi[4];    /**< SPI 0-3   (NULL = absent) */
    const hal_i2c_ops_t   *i2c[4];    /**< I²C 0-3   (NULL = absent) */
    const hal_power_ops_t *power;     /**< power management hooks    */
} hal_platform_t;

/* ====================================================================
 * Non-inline HAL function declarations
 *
 * Performance-critical primitives (hal_irq_save / hal_irq_restore,
 * hal_cpu_wait_for_interrupt, hal_cpu_dsb, hal_cpu_isb, and
 * hal_context_switch_trigger) are declared as static inline in each
 * arch-specific header above so the compiler can avoid call overhead.
 *
 * The functions below are non-trivial and are implemented in
 * hal/<arch>/hal_<arch>.c.
 * ==================================================================== */

/** One-time HAL initialisation — call before os_start(). */
void hal_init(void);

/**
 * Configure the periodic tick source.
 * @param core_clock_hz  Processor clock frequency in Hz.
 * @param tick_rate_hz   Desired tick interrupts per second (e.g. 1000).
 */
void hal_tick_init(uint32_t core_clock_hz, uint32_t tick_rate_hz);

/**
 * Pause the tick source for tickless idle (suppress up to @p max_ticks
 * interrupts).  Call hal_tick_unsuppress() to resume.
 */
void hal_tick_suppress(uint32_t max_ticks);

/**
 * Resume the tick source after hal_tick_suppress().
 * @return  Number of ticks that elapsed while suppressed (0 if the
 *          platform relies solely on the cycle counter — see
 *          hal_cycle_counter_read()).
 */
uint32_t hal_tick_unsuppress(void);

/** @return Core clock frequency last set by hal_tick_init(). */
uint32_t hal_core_clock_hz(void);

/* ── Cycle counter ──────────────────────────────────────────────────── */

/**
 * Initialise the hardware cycle counter.
 * @return true  if a cycle counter is available on this platform.
 */
bool hal_cycle_counter_init(void);

/** Reset the cycle counter to zero (or record a snapshot for wrapping). */
void hal_cycle_counter_reset(void);

/** Read the current raw cycle counter value. */
uint32_t hal_cycle_counter_read(void);

/* ── Memory Protection Unit ─────────────────────────────────────────── */

/**
 * Initialise the MPU / PMP.
 * @param[out] regions  Filled with the number of available regions.
 * @return true  if an MPU is present on this device.
 */
bool hal_mpu_init(uint8_t *regions);

/**
 * Configure one MPU region.
 * @param region  Zero-based region index.
 * @param base    Base address (must meet alignment requirement).
 * @param size    Region size in bytes (power of two, arch-dependent encoding).
 * @param attrs   Architecture-specific attribute word (RASR on Cortex-M,
 *                pmpcfg on RISC-V).
 * @return 0 on success, negative on error.
 */
int hal_mpu_configure_region(uint8_t region, uint32_t base,
                              uint32_t size, uint32_t attrs);

/** Enable the MPU.
 *  @param allow_privileged_default  If true, privileged accesses may fall
 *         through to the default memory map (PRIVDEFENA on Cortex-M). */
void hal_mpu_enable(bool allow_privileged_default);

/** Disable the MPU. */
void hal_mpu_disable(void);

/* ── Interrupt priority ─────────────────────────────────────────────── */

/**
 * Set hardware priority for an exception or peripheral interrupt.
 * @param irq_num   Negative = CPU exception (arch-defined IDs),
 *                  >= 0  = peripheral interrupt number.
 * @param priority  0 = highest, 255 = lowest priority.
 */
void hal_irq_set_priority(int irq_num, uint8_t priority);

/* ── System reset ───────────────────────────────────────────────────── */

/** Perform a hardware system reset (does not return). */
void hal_system_reset(void);

/* ── Fault information ──────────────────────────────────────────────── */

/** Architecture-independent fault record. */
typedef struct {
    uint32_t status;      /**< Fault type flags (arch-defined bit field)  */
    uint32_t pc;          /**< Faulting program counter                   */
    uint32_t lr;          /**< Link register at time of fault             */
    uint32_t sp;          /**< Stack pointer at time of fault             */
    uint32_t fault_addr;  /**< Data address that triggered the fault      */
    uint32_t extra[3];    /**< Arch extras: CFSR/HFSR/BFAR on Cortex-M,
                               mcause/mtval/mepc on RISC-V               */
} hal_fault_info_t;

/**
 * Populate @p info from the CPU exception frame.
 * The frame pointer must point to the register save area created by the
 * processor on exception entry (layout is arch-specific).
 */
void hal_fault_capture(const uint32_t *exception_frame,
                       hal_fault_info_t *info);

/* ── Platform registry ──────────────────────────────────────────────── */

/** Register the board-level platform descriptor (call once at startup). */
void hal_platform_register(const hal_platform_t *platform);

/** Return the registered platform descriptor, or NULL if none. */
const hal_platform_t *hal_platform_get(void);

#endif /* TINYOS_HAL_H */
