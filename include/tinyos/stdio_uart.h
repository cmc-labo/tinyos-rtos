/**
 * @file stdio_uart.h
 * @brief Standard I/O retargeting and structured logging over UART for TinyOS.
 *
 * Integrates the C standard library (newlib) printf family with the RTOS by
 * overriding the low-level _write() syscall.  All output is serialised through
 * a mutex so concurrent printf calls from multiple tasks are safe.
 *
 * Usage (minimal):
 * @code
 *   // In platform BSP, provide a character output function:
 *   static void my_uart_putc(char c) { USART1->DR = c; }
 *
 *   // Before os_start():
 *   tinyos_stdio_init(my_uart_putc, NULL);
 *
 *   // Now printf / puts / putchar work from any task:
 *   printf("Hello from task %s\n", pcTaskGetName(NULL));
 * @endcode
 *
 * Usage (structured logging):
 * @code
 *   os_log_set_level(LOG_DEBUG);
 *   LOG_I("sensor", "temperature = %.1f C", temp);
 *   // → [  1234ms][INFO ][sensor      ] temperature = 23.5 C
 * @endcode
 *
 * Configuration macros (set before including this header or via -D):
 *   TINYOS_STDIO_BUF_SIZE   Internal format buffer per call  (default 256)
 *   TINYOS_LOG_TAG_WIDTH    Column width of the tag field    (default 12)
 */

#ifndef TINYOS_STDIO_UART_H
#define TINYOS_STDIO_UART_H

#include "../tinyos.h"
#include <stdarg.h>

/*===========================================================================
 * Configuration
 *===========================================================================*/

#ifndef TINYOS_STDIO_BUF_SIZE
#define TINYOS_STDIO_BUF_SIZE   256   /**< Per-call format buffer (bytes)  */
#endif

#ifndef TINYOS_LOG_TAG_WIDTH
#define TINYOS_LOG_TAG_WIDTH    12    /**< Tag column width in log output  */
#endif

/*===========================================================================
 * I/O callbacks
 *===========================================================================*/

/** Output one character to the UART (called from _write / putchar). */
typedef void (*tinyos_putc_fn_t)(char c);

/** Output a NUL-terminated string to the UART (optional fast path). */
typedef void (*tinyos_puts_fn_t)(const char *s);

/*===========================================================================
 * Initialisation
 *===========================================================================*/

/**
 * @brief Bind stdio to the platform UART and enable thread-safe printf.
 *
 * Must be called once before any printf/puts/putchar usage.
 * It is safe to call before os_start() — output before scheduler start
 * bypasses the mutex and writes directly via the callbacks.
 *
 * @param putc_fn  Character output callback (required, must not be NULL).
 * @param puts_fn  String output callback (optional; pass NULL to derive from
 *                 putc_fn).  When provided it is used as a fast path for
 *                 puts() and for the final flush in printf.
 */
void tinyos_stdio_init(tinyos_putc_fn_t putc_fn, tinyos_puts_fn_t puts_fn);

/**
 * @brief Check whether stdio has been initialised.
 * @return true if tinyos_stdio_init() has been called.
 */
bool tinyos_stdio_is_ready(void);

/*===========================================================================
 * Log levels
 *===========================================================================*/

typedef enum {
    LOG_DEBUG = 0,  /**< Verbose diagnostic output   */
    LOG_INFO  = 1,  /**< Normal operational messages  */
    LOG_WARN  = 2,  /**< Non-fatal anomalies          */
    LOG_ERROR = 3,  /**< Errors requiring attention   */
    LOG_NONE  = 4,  /**< Suppress all log output      */
} log_level_t;

/*===========================================================================
 * Logging API
 *===========================================================================*/

/**
 * @brief Set the minimum log level.  Messages below this level are discarded.
 *        Default is LOG_INFO.
 */
void os_log_set_level(log_level_t level);

/** @brief Return the current minimum log level. */
log_level_t os_log_get_level(void);

/**
 * @brief Emit a structured log message.
 *
 * Output format:
 *   [<uptime_ms>ms][<LEVEL>][<tag>] <message>\r\n
 *
 * Example:
 *   [   1234ms][INFO ][net         ] link up, ip=192.168.1.10
 *
 * Thread-safe: can be called concurrently from multiple tasks.
 * ISR-safe: when called from interrupt context the mutex is skipped and
 *           output is written directly (no blocking).
 *
 * @param level  Severity level.
 * @param tag    Short identifier for the subsystem (e.g. "net", "sensor").
 *               Truncated/padded to TINYOS_LOG_TAG_WIDTH columns.
 * @param fmt    printf-style format string.
 */
void os_log(log_level_t level, const char *tag, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/** @brief va_list variant of os_log (for wrapper functions). */
void os_log_v(log_level_t level, const char *tag, const char *fmt, va_list ap);

/*===========================================================================
 * Convenience macros
 *===========================================================================*/

/** @cond INTERNAL */
#define _OS_LOG(lvl, tag, fmt, ...) os_log((lvl), (tag), (fmt), ##__VA_ARGS__)
/** @endcond */

#define LOG_D(tag, fmt, ...)  _OS_LOG(LOG_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...)  _OS_LOG(LOG_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...)  _OS_LOG(LOG_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...)  _OS_LOG(LOG_ERROR, tag, fmt, ##__VA_ARGS__)

/*===========================================================================
 * Low-level direct output (bypasses mutex — use only before os_start or in
 * early boot where blocking is not possible)
 *===========================================================================*/

/** Write a single character directly to the UART (no mutex). */
void tinyos_putc_raw(char c);

/** Write a NUL-terminated string directly to the UART (no mutex). */
void tinyos_puts_raw(const char *s);

#endif /* TINYOS_STDIO_UART_H */
