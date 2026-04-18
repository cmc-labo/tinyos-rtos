/**
 * @file stdio_uart.c
 * @brief Standard I/O retargeting and structured logging for TinyOS.
 *
 * Two integration points:
 *
 *  1. Newlib _write() syscall override
 *     gcc-arm-none-eabi links against newlib.  When printf formats a string it
 *     ultimately calls _write(1, buf, len) to emit characters.  By providing
 *     our own _write() we redirect all stdio output to the platform UART.
 *
 *  2. os_log() structured logger
 *     Wraps vsnprintf with a timestamp + level + tag prefix, serialised by a
 *     mutex so concurrent calls from different tasks do not interleave.
 */

#include "tinyos/stdio_uart.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/*===========================================================================
 * Internal state
 *===========================================================================*/

static struct {
    tinyos_putc_fn_t putc_fn;
    tinyos_puts_fn_t puts_fn;  /* optional fast path */
    bool             ready;
    mutex_t          lock;
    log_level_t      min_level;
} stdio_state;

/* Level label strings (5 chars + NUL for alignment) */
static const char * const level_labels[] = {
    "DEBUG",
    "INFO ",
    "WARN ",
    "ERROR",
};

/*===========================================================================
 * Helpers
 *===========================================================================*/

/**
 * Write a single character via the registered callback.
 * No locking — callers are responsible for holding the mutex when needed.
 */
static inline void raw_putc(char c) {
    if (stdio_state.putc_fn) stdio_state.putc_fn(c);
}

/**
 * Write a NUL-terminated string via the registered callbacks.
 * Uses puts_fn if available, otherwise falls back to raw_putc per character.
 */
static void raw_puts(const char *s) {
    if (!s) return;
    if (stdio_state.puts_fn) {
        stdio_state.puts_fn(s);
    } else {
        while (*s) raw_putc(*s++);
    }
}

/**
 * Write exactly len bytes from buf (may contain embedded NULs).
 */
static void raw_write(const char *buf, size_t len) {
    for (size_t i = 0; i < len; i++) raw_putc(buf[i]);
}

/**
 * Acquire the stdio mutex only if the scheduler is running.
 * Before os_start() there are no concurrent tasks so locking is unnecessary
 * and would deadlock (os_mutex_lock requires a running current task).
 */
static bool stdio_lock(void) {
    if (!stdio_state.ready) return false;
    if (!os_is_running()) return false;
    os_mutex_lock(&stdio_state.lock, OS_WAIT_FOREVER);
    return true;
}

static void stdio_unlock(bool was_locked) {
    if (was_locked) os_mutex_unlock(&stdio_state.lock);
}

/*===========================================================================
 * Public initialisation
 *===========================================================================*/

void tinyos_stdio_init(tinyos_putc_fn_t putc_fn, tinyos_puts_fn_t puts_fn) {
    if (!putc_fn) return;

    stdio_state.putc_fn   = putc_fn;
    stdio_state.puts_fn   = puts_fn;
    stdio_state.min_level = LOG_INFO;

    os_mutex_init(&stdio_state.lock);
    stdio_state.ready = true;
}

bool tinyos_stdio_is_ready(void) {
    return stdio_state.ready;
}

/*===========================================================================
 * Newlib syscall: _write
 *
 * Prototype: int _write(int fd, char *buf, int len);
 *
 * Called by printf / puts / putchar / fwrite(stdout/stderr ...).
 * We route fd=1 (stdout) and fd=2 (stderr) to the UART.
 * Other file descriptors are ignored (no filesystem integration here).
 *===========================================================================*/

int _write(int fd, const char *buf, int len) {
    if (!stdio_state.ready || len <= 0) return len;
    if (fd != 1 && fd != 2) return len;   /* only stdout / stderr */

    bool locked = stdio_lock();
    raw_write(buf, (size_t)len);
    stdio_unlock(locked);

    return len;
}

/*===========================================================================
 * Low-level direct output (no mutex)
 *===========================================================================*/

void tinyos_putc_raw(char c) { raw_putc(c); }
void tinyos_puts_raw(const char *s) { raw_puts(s); }

/*===========================================================================
 * Log-level control
 *===========================================================================*/

void os_log_set_level(log_level_t level) {
    stdio_state.min_level = level;
}

log_level_t os_log_get_level(void) {
    return stdio_state.min_level;
}

/*===========================================================================
 * Structured logger
 *===========================================================================*/

/**
 * Format and emit one log line.
 *
 * Output format (fixed columns for easy grepping):
 *   [<uptime_ms right-justified to 7 digits>ms][<LEVEL>][<tag padded>] msg\r\n
 *
 * Example:
 *   [   1234ms][INFO ][net         ] link up
 *   [  56789ms][WARN ][heap        ] free < 512 bytes
 */
void os_log_v(log_level_t level, const char *tag, const char *fmt, va_list ap) {
    if (!stdio_state.ready) return;
    if (level < stdio_state.min_level) return;
    if (level > LOG_ERROR) return;

    char msg_buf[TINYOS_STDIO_BUF_SIZE];
    char prefix[64];

    /* Format the user message first */
    vsnprintf(msg_buf, sizeof(msg_buf), fmt, ap);

    /* Build the prefix: [<ms>ms][LEVEL][<tag>] */
    uint32_t uptime = os_get_uptime_ms();
    const char *label = level_labels[level];

    /* Tag: truncate or pad to TINYOS_LOG_TAG_WIDTH */
    char tag_buf[TINYOS_LOG_TAG_WIDTH + 1];
    if (tag) {
        int tag_len = (int)strlen(tag);
        if (tag_len >= TINYOS_LOG_TAG_WIDTH) {
            memcpy(tag_buf, tag, TINYOS_LOG_TAG_WIDTH);
            tag_buf[TINYOS_LOG_TAG_WIDTH] = '\0';
        } else {
            memcpy(tag_buf, tag, (size_t)tag_len);
            memset(tag_buf + tag_len, ' ', (size_t)(TINYOS_LOG_TAG_WIDTH - tag_len));
            tag_buf[TINYOS_LOG_TAG_WIDTH] = '\0';
        }
    } else {
        memset(tag_buf, ' ', TINYOS_LOG_TAG_WIDTH);
        tag_buf[TINYOS_LOG_TAG_WIDTH] = '\0';
    }

    snprintf(prefix, sizeof(prefix), "[%7lums][%s][%s] ",
             (unsigned long)uptime, label, tag_buf);

    bool locked = stdio_lock();
    raw_puts(prefix);
    raw_puts(msg_buf);
    raw_puts("\r\n");
    stdio_unlock(locked);
}

void os_log(log_level_t level, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    os_log_v(level, tag, fmt, ap);
    va_end(ap);
}
