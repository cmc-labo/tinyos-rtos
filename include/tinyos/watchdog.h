/**
 * @file watchdog.h
 * @brief Watchdog Timer Module for TinyOS
 *
 * This module provides hardware and software watchdog functionality
 * for system monitoring and recovery from hangs or crashes.
 *
 * Features:
 * - Hardware watchdog timer support
 * - Software watchdog for task monitoring
 * - Automatic system reset on timeout
 * - Per-task watchdog monitoring
 * - Watchdog statistics and status
 */

#ifndef TINYOS_WATCHDOG_H
#define TINYOS_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct tcb tcb_t;

/* ========================================================================
 * Convenience Macros
 * ======================================================================== */

/**
 * @brief Quick initialization with default settings
 *
 * Usage:
 *   WDT_QUICK_INIT(2000, my_callback);
 */
#define WDT_QUICK_INIT(timeout_ms, callback_func) do { \
    wdt_config_t _cfg = WDT_DEFAULT_CONFIG(timeout_ms, callback_func); \
    wdt_init(&_cfg); \
} while(0)

/**
 * @brief Quick task registration
 *
 * Usage:
 *   WDT_REGISTER_TASK(my_task, 1000);
 */
#define WDT_REGISTER_TASK(task, timeout) \
    wdt_register_task(&(task), (timeout))

/**
 * @brief Quick task feed
 *
 * Usage:
 *   WDT_FEED_TASK(my_task);
 */
#define WDT_FEED_TASK(task) \
    wdt_feed_task(&(task))

/**
 * @brief Safe watchdog feed (checks if initialized)
 */
#define WDT_SAFE_FEED() do { \
    if (wdt_is_initialized()) wdt_feed(); \
} while(0)

/**
 * @brief Watchdog feed with error checking
 */
#define WDT_FEED_OR_WARN() do { \
    wdt_error_t _err = wdt_feed(); \
    if (_err != WDT_OK) { \
        printf("WDT feed failed: %s\n", wdt_error_to_string(_err)); \
    } \
} while(0)

/* ========================================================================
 * Debug Helper Macros
 * ======================================================================== */

#ifdef WDT_DEBUG
/**
 * @brief Debug trace for watchdog feed
 */
#define WDT_DEBUG_FEED() do { \
    printf("[WDT] Feed at %s:%d\n", __FILE__, __LINE__); \
    wdt_feed(); \
} while(0)

/**
 * @brief Debug trace for task feed
 */
#define WDT_DEBUG_FEED_TASK(task) do { \
    printf("[WDT] Task feed at %s:%d, task=%p\n", __FILE__, __LINE__, (void*)(task)); \
    wdt_feed_task(task); \
} while(0)

/**
 * @brief Print compact status line for debugging
 */
#define WDT_DEBUG_STATUS() do { \
    printf("[WDT] Enabled=%d, Timeout=%lums, Remaining=%lums\n", \
           wdt_is_enabled(), (unsigned long)wdt_get_timeout(), \
           (unsigned long)wdt_get_time_remaining()); \
} while(0)

#else
#define WDT_DEBUG_FEED() wdt_feed()
#define WDT_DEBUG_FEED_TASK(task) wdt_feed_task(task)
#define WDT_DEBUG_STATUS() do {} while(0)
#endif /* WDT_DEBUG */

/**
 * @brief Assert-style watchdog check
 *
 * Usage in debug builds to ensure watchdog is responsive
 */
#define WDT_ASSERT_RESPONSIVE() do { \
    if (wdt_is_initialized() && wdt_is_enabled()) { \
        uint32_t remaining = wdt_get_time_remaining(); \
        if (remaining < 100) { \
            printf("WARNING: Watchdog critical! %lums remaining\n", \
                   (unsigned long)remaining); \
        } \
    } \
} while(0)

/**
 * @brief Convert milliseconds to optimal feed interval (50%)
 */
#define WDT_FEED_INTERVAL(timeout_ms) ((timeout_ms) / 2)

/**
 * @brief Convert milliseconds to conservative feed interval (33%)
 */
#define WDT_SAFE_FEED_INTERVAL(timeout_ms) ((timeout_ms) / 3)

/**
 * @brief Default configuration preset
 */
#define WDT_DEFAULT_CONFIG(timeout, cb) { \
    .type = WDT_TYPE_BOTH, \
    .timeout_ms = (timeout), \
    .auto_start = true, \
    .enable_reset = true, \
    .callback = (cb) \
}

/**
 * @brief Debug-friendly configuration (no auto-reset)
 */
#define WDT_DEBUG_CONFIG(timeout, cb) { \
    .type = WDT_TYPE_SOFTWARE, \
    .timeout_ms = (timeout), \
    .auto_start = true, \
    .enable_reset = false, \
    .callback = (cb) \
}

/**
 * @brief Production configuration (aggressive)
 */
#define WDT_PRODUCTION_CONFIG(timeout, cb) { \
    .type = WDT_TYPE_BOTH, \
    .timeout_ms = (timeout), \
    .auto_start = true, \
    .enable_reset = true, \
    .callback = (cb) \
}

/**
 * @brief Software-only configuration
 */
#define WDT_SOFTWARE_ONLY_CONFIG(timeout, cb) { \
    .type = WDT_TYPE_SOFTWARE, \
    .timeout_ms = (timeout), \
    .auto_start = true, \
    .enable_reset = true, \
    .callback = (cb) \
}

/* Common timeout values (milliseconds) */
#define WDT_TIMEOUT_100MS   100
#define WDT_TIMEOUT_500MS   500
#define WDT_TIMEOUT_1S      1000
#define WDT_TIMEOUT_2S      2000
#define WDT_TIMEOUT_5S      5000
#define WDT_TIMEOUT_10S     10000
#define WDT_TIMEOUT_30S     30000
#define WDT_TIMEOUT_60S     60000

/**
 * @brief Watchdog error codes
 */
typedef enum {
    WDT_OK = 0,
    WDT_ERROR_INVALID_PARAM,
    WDT_ERROR_NOT_INITIALIZED,
    WDT_ERROR_ALREADY_INITIALIZED,
    WDT_ERROR_TIMEOUT_TOO_SHORT,
    WDT_ERROR_TIMEOUT_TOO_LONG,
    WDT_ERROR_NOT_ENABLED,
    WDT_ERROR_TASK_NOT_REGISTERED,
    WDT_ERROR_MAX_TASKS_REACHED
} wdt_error_t;

/**
 * @brief Watchdog timer types
 */
typedef enum {
    WDT_TYPE_HARDWARE,      /**< Hardware watchdog timer */
    WDT_TYPE_SOFTWARE,      /**< Software watchdog timer */
    WDT_TYPE_BOTH          /**< Both hardware and software */
} wdt_type_t;

/**
 * @brief Watchdog reset reason
 */
typedef enum {
    WDT_RESET_NONE = 0,
    WDT_RESET_HARDWARE,     /**< Hardware watchdog timeout */
    WDT_RESET_SOFTWARE,     /**< Software watchdog timeout */
    WDT_RESET_TASK_TIMEOUT  /**< Task watchdog timeout */
} wdt_reset_reason_t;

/**
 * @brief Watchdog callback function type
 *
 * Called before system reset to allow cleanup
 *
 * @param reason Reset reason
 * @param task Task that caused timeout (NULL if not task-specific)
 */
typedef void (*wdt_callback_t)(wdt_reset_reason_t reason, tcb_t *task);

/**
 * @brief Watchdog configuration
 */
typedef struct {
    wdt_type_t type;              /**< Watchdog type */
    uint32_t timeout_ms;          /**< Watchdog timeout in milliseconds */
    bool auto_start;              /**< Start watchdog automatically */
    bool enable_reset;            /**< Enable automatic reset on timeout */
    wdt_callback_t callback;      /**< Pre-reset callback (optional) */
} wdt_config_t;

/**
 * @brief Task watchdog entry
 */
typedef struct {
    tcb_t *task;                  /**< Task pointer */
    uint32_t timeout_ms;          /**< Task-specific timeout */
    uint32_t last_feed_time;      /**< Last feed timestamp */
    bool enabled;                 /**< Task watchdog enabled */
    uint32_t timeout_count;       /**< Number of timeouts */
} wdt_task_entry_t;

/**
 * @brief Watchdog statistics
 */
typedef struct {
    uint32_t total_feeds;         /**< Total number of feeds */
    uint32_t total_timeouts;      /**< Total number of timeouts */
    uint32_t hw_resets;           /**< Hardware watchdog resets */
    uint32_t sw_resets;           /**< Software watchdog resets */
    uint32_t task_timeouts;       /**< Task watchdog timeouts */
    wdt_reset_reason_t last_reset_reason;  /**< Last reset reason */
    tcb_t *last_timeout_task;     /**< Last task that timed out */
} wdt_stats_t;

/**
 * @brief Watchdog status
 */
typedef struct {
    bool initialized;             /**< Watchdog initialized */
    bool enabled;                 /**< Watchdog enabled */
    wdt_type_t type;             /**< Watchdog type */
    uint32_t timeout_ms;          /**< Current timeout value */
    uint32_t time_remaining_ms;   /**< Time until timeout */
    uint32_t last_feed_time;      /**< Last feed timestamp */
    uint32_t registered_tasks;    /**< Number of registered tasks */
} wdt_status_t;

/* ========================================================================
 * Initialization and Configuration
 * ======================================================================== */

/**
 * @brief Initialize watchdog timer
 *
 * @param config Watchdog configuration
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_init(const wdt_config_t *config);

/**
 * @brief Deinitialize watchdog timer
 *
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_deinit(void);

/**
 * @brief Check if watchdog is initialized
 *
 * @return true if initialized, false otherwise
 */
bool wdt_is_initialized(void);

/* ========================================================================
 * Watchdog Control
 * ======================================================================== */

/**
 * @brief Start watchdog timer
 *
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_start(void);

/**
 * @brief Stop watchdog timer
 *
 * Note: Some hardware watchdogs cannot be stopped once started
 *
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_stop(void);

/**
 * @brief Feed (refresh) watchdog timer
 *
 * This must be called periodically to prevent timeout
 *
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_feed(void);

/**
 * @brief Set watchdog timeout
 *
 * @param timeout_ms Timeout in milliseconds
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_set_timeout(uint32_t timeout_ms);

/**
 * @brief Get watchdog timeout
 *
 * @return Current timeout in milliseconds
 */
uint32_t wdt_get_timeout(void);

/**
 * @brief Enable watchdog
 *
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_enable(void);

/**
 * @brief Disable watchdog
 *
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_disable(void);

/**
 * @brief Check if watchdog is enabled
 *
 * @return true if enabled, false otherwise
 */
bool wdt_is_enabled(void);

/* ========================================================================
 * Task Watchdog
 * ======================================================================== */

/**
 * @brief Register task for watchdog monitoring
 *
 * @param task Task to monitor
 * @param timeout_ms Task-specific timeout in milliseconds
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_register_task(tcb_t *task, uint32_t timeout_ms);

/**
 * @brief Unregister task from watchdog monitoring
 *
 * @param task Task to unregister
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_unregister_task(tcb_t *task);

/**
 * @brief Feed watchdog for specific task
 *
 * @param task Task feeding the watchdog
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_feed_task(tcb_t *task);

/**
 * @brief Enable task watchdog
 *
 * @param task Task to enable watchdog for
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_enable_task(tcb_t *task);

/**
 * @brief Disable task watchdog
 *
 * @param task Task to disable watchdog for
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_disable_task(tcb_t *task);

/**
 * @brief Check if task is registered
 *
 * @param task Task to check
 * @return true if registered, false otherwise
 */
bool wdt_is_task_registered(tcb_t *task);

/* ========================================================================
 * Status and Statistics
 * ======================================================================== */

/**
 * @brief Get watchdog status
 *
 * @param status Pointer to status structure
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_get_status(wdt_status_t *status);

/**
 * @brief Get watchdog statistics
 *
 * @param stats Pointer to statistics structure
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_get_stats(wdt_stats_t *stats);

/**
 * @brief Reset watchdog statistics
 *
 * @return WDT_OK on success, error code otherwise
 */
wdt_error_t wdt_reset_stats(void);

/**
 * @brief Get last reset reason
 *
 * @return Last reset reason
 */
wdt_reset_reason_t wdt_get_last_reset_reason(void);

/**
 * @brief Get time remaining until timeout
 *
 * @return Time remaining in milliseconds
 */
uint32_t wdt_get_time_remaining(void);

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

/**
 * @brief Convert error code to string
 *
 * @param error Error code
 * @return Error string
 */
const char *wdt_error_to_string(wdt_error_t error);

/**
 * @brief Convert reset reason to string
 *
 * @param reason Reset reason
 * @return Reason string
 */
const char *wdt_reset_reason_to_string(wdt_reset_reason_t reason);

/**
 * @brief Print watchdog status
 */
void wdt_print_status(void);

/**
 * @brief Print watchdog statistics
 */
void wdt_print_stats(void);

/**
 * @brief Print registered tasks
 */
void wdt_print_registered_tasks(void);

/* ========================================================================
 * Hardware-Specific Functions (Optional)
 * ======================================================================== */

/**
 * @brief Trigger watchdog reset immediately
 *
 * This will cause an immediate system reset
 */
void wdt_trigger_reset(void);

/**
 * @brief Check if last reset was caused by watchdog
 *
 * @return true if last reset was watchdog reset, false otherwise
 */
bool wdt_was_reset_by_watchdog(void);

/**
 * @brief Clear watchdog reset flag
 */
void wdt_clear_reset_flag(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_WATCHDOG_H */
