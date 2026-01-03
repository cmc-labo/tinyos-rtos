/**
 * @file watchdog.c
 * @brief Watchdog Timer Implementation for TinyOS
 */

#include "tinyos/watchdog.h"
#include "tinyos.h"
#include <string.h>
#include <stdio.h>

/* Maximum number of tasks that can be monitored */
#define WDT_MAX_MONITORED_TASKS 8

/* Minimum and maximum timeout values (milliseconds) */
#define WDT_MIN_TIMEOUT_MS 100
#define WDT_MAX_TIMEOUT_MS 60000

/* Watchdog state */
typedef struct {
    bool initialized;
    bool enabled;
    wdt_config_t config;
    uint32_t last_feed_time;
    wdt_stats_t stats;
    wdt_task_entry_t task_entries[WDT_MAX_MONITORED_TASKS];
    uint32_t num_registered_tasks;
    wdt_reset_reason_t reset_reason;
} wdt_state_t;

/* Global watchdog state */
static wdt_state_t g_wdt_state = {0};

/* External time function (should be provided by OS) */
extern uint32_t os_get_tick_count(void);

/* Hardware-specific functions (weak implementations) */
__attribute__((weak)) void hal_wdt_init(uint32_t timeout_ms) {
    (void)timeout_ms;
    /* Platform-specific implementation */
}

__attribute__((weak)) void hal_wdt_feed(void) {
    /* Platform-specific implementation */
}

__attribute__((weak)) void hal_wdt_enable(void) {
    /* Platform-specific implementation */
}

__attribute__((weak)) void hal_wdt_disable(void) {
    /* Platform-specific implementation */
}

__attribute__((weak)) void hal_wdt_trigger_reset(void) {
    /* Platform-specific implementation */
    while(1); /* Infinite loop to trigger watchdog */
}

__attribute__((weak)) bool hal_wdt_was_reset_by_watchdog(void) {
    /* Platform-specific implementation */
    return false;
}

__attribute__((weak)) void hal_wdt_clear_reset_flag(void) {
    /* Platform-specific implementation */
}

/* ========================================================================
 * Internal Helper Functions
 * ======================================================================== */

static wdt_task_entry_t *find_task_entry(tcb_t *task) {
    for (uint32_t i = 0; i < g_wdt_state.num_registered_tasks; i++) {
        if (g_wdt_state.task_entries[i].task == task) {
            return &g_wdt_state.task_entries[i];
        }
    }
    return NULL;
}

static void wdt_check_tasks(void) {
    if (!g_wdt_state.initialized || !g_wdt_state.enabled) {
        return;
    }

    uint32_t current_time = os_get_tick_count();

    for (uint32_t i = 0; i < g_wdt_state.num_registered_tasks; i++) {
        wdt_task_entry_t *entry = &g_wdt_state.task_entries[i];

        if (!entry->enabled) {
            continue;
        }

        uint32_t elapsed = current_time - entry->last_feed_time;

        if (elapsed >= entry->timeout_ms) {
            /* Task timeout detected */
            entry->timeout_count++;
            g_wdt_state.stats.task_timeouts++;
            g_wdt_state.stats.last_timeout_task = entry->task;

            /* Call callback if configured */
            if (g_wdt_state.config.callback) {
                g_wdt_state.config.callback(WDT_RESET_TASK_TIMEOUT, entry->task);
            }

            /* Reset system if enabled */
            if (g_wdt_state.config.enable_reset) {
                g_wdt_state.reset_reason = WDT_RESET_TASK_TIMEOUT;
                g_wdt_state.stats.sw_resets++;
                hal_wdt_trigger_reset();
            }
        }
    }
}

/* ========================================================================
 * Initialization and Configuration
 * ======================================================================== */

wdt_error_t wdt_init(const wdt_config_t *config) {
    if (g_wdt_state.initialized) {
        return WDT_ERROR_ALREADY_INITIALIZED;
    }

    if (!config) {
        return WDT_ERROR_INVALID_PARAM;
    }

    if (config->timeout_ms < WDT_MIN_TIMEOUT_MS ||
        config->timeout_ms > WDT_MAX_TIMEOUT_MS) {
        return WDT_ERROR_INVALID_PARAM;
    }

    /* Initialize state */
    memset(&g_wdt_state, 0, sizeof(g_wdt_state));
    memcpy(&g_wdt_state.config, config, sizeof(wdt_config_t));
    g_wdt_state.initialized = true;
    g_wdt_state.last_feed_time = os_get_tick_count();

    /* Check if last reset was caused by watchdog */
    if (hal_wdt_was_reset_by_watchdog()) {
        g_wdt_state.reset_reason = WDT_RESET_HARDWARE;
        g_wdt_state.stats.hw_resets++;
        hal_wdt_clear_reset_flag();
    }

    /* Initialize hardware watchdog if configured */
    if (config->type == WDT_TYPE_HARDWARE || config->type == WDT_TYPE_BOTH) {
        hal_wdt_init(config->timeout_ms);
    }

    /* Auto-start if configured */
    if (config->auto_start) {
        wdt_start();
    }

    return WDT_OK;
}

wdt_error_t wdt_deinit(void) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    wdt_stop();
    memset(&g_wdt_state, 0, sizeof(g_wdt_state));

    return WDT_OK;
}

bool wdt_is_initialized(void) {
    return g_wdt_state.initialized;
}

/* ========================================================================
 * Watchdog Control
 * ======================================================================== */

wdt_error_t wdt_start(void) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    g_wdt_state.enabled = true;
    g_wdt_state.last_feed_time = os_get_tick_count();

    /* Enable hardware watchdog */
    if (g_wdt_state.config.type == WDT_TYPE_HARDWARE ||
        g_wdt_state.config.type == WDT_TYPE_BOTH) {
        hal_wdt_enable();
    }

    return WDT_OK;
}

wdt_error_t wdt_stop(void) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    g_wdt_state.enabled = false;

    /* Disable hardware watchdog (if possible) */
    if (g_wdt_state.config.type == WDT_TYPE_HARDWARE ||
        g_wdt_state.config.type == WDT_TYPE_BOTH) {
        hal_wdt_disable();
    }

    return WDT_OK;
}

wdt_error_t wdt_feed(void) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    if (!g_wdt_state.enabled) {
        return WDT_ERROR_NOT_ENABLED;
    }

    g_wdt_state.last_feed_time = os_get_tick_count();
    g_wdt_state.stats.total_feeds++;

    /* Feed hardware watchdog */
    if (g_wdt_state.config.type == WDT_TYPE_HARDWARE ||
        g_wdt_state.config.type == WDT_TYPE_BOTH) {
        hal_wdt_feed();
    }

    /* Check software watchdog for tasks */
    if (g_wdt_state.config.type == WDT_TYPE_SOFTWARE ||
        g_wdt_state.config.type == WDT_TYPE_BOTH) {
        wdt_check_tasks();
    }

    return WDT_OK;
}

wdt_error_t wdt_set_timeout(uint32_t timeout_ms) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    if (timeout_ms < WDT_MIN_TIMEOUT_MS) {
        return WDT_ERROR_TIMEOUT_TOO_SHORT;
    }

    if (timeout_ms > WDT_MAX_TIMEOUT_MS) {
        return WDT_ERROR_TIMEOUT_TOO_LONG;
    }

    g_wdt_state.config.timeout_ms = timeout_ms;

    /* Reinitialize hardware watchdog with new timeout */
    if (g_wdt_state.config.type == WDT_TYPE_HARDWARE ||
        g_wdt_state.config.type == WDT_TYPE_BOTH) {
        hal_wdt_init(timeout_ms);
    }

    return WDT_OK;
}

uint32_t wdt_get_timeout(void) {
    return g_wdt_state.config.timeout_ms;
}

wdt_error_t wdt_enable(void) {
    return wdt_start();
}

wdt_error_t wdt_disable(void) {
    return wdt_stop();
}

bool wdt_is_enabled(void) {
    return g_wdt_state.enabled;
}

/* ========================================================================
 * Task Watchdog
 * ======================================================================== */

wdt_error_t wdt_register_task(tcb_t *task, uint32_t timeout_ms) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    if (!task) {
        return WDT_ERROR_INVALID_PARAM;
    }

    if (timeout_ms < WDT_MIN_TIMEOUT_MS) {
        return WDT_ERROR_TIMEOUT_TOO_SHORT;
    }

    /* Check if task already registered */
    if (find_task_entry(task)) {
        return WDT_ERROR_INVALID_PARAM;
    }

    /* Check if we have space */
    if (g_wdt_state.num_registered_tasks >= WDT_MAX_MONITORED_TASKS) {
        return WDT_ERROR_MAX_TASKS_REACHED;
    }

    /* Add task entry */
    wdt_task_entry_t *entry = &g_wdt_state.task_entries[g_wdt_state.num_registered_tasks];
    entry->task = task;
    entry->timeout_ms = timeout_ms;
    entry->last_feed_time = os_get_tick_count();
    entry->enabled = true;
    entry->timeout_count = 0;

    g_wdt_state.num_registered_tasks++;

    return WDT_OK;
}

wdt_error_t wdt_unregister_task(tcb_t *task) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    if (!task) {
        return WDT_ERROR_INVALID_PARAM;
    }

    /* Find and remove task entry */
    for (uint32_t i = 0; i < g_wdt_state.num_registered_tasks; i++) {
        if (g_wdt_state.task_entries[i].task == task) {
            /* Shift remaining entries */
            for (uint32_t j = i; j < g_wdt_state.num_registered_tasks - 1; j++) {
                g_wdt_state.task_entries[j] = g_wdt_state.task_entries[j + 1];
            }
            g_wdt_state.num_registered_tasks--;
            return WDT_OK;
        }
    }

    return WDT_ERROR_TASK_NOT_REGISTERED;
}

wdt_error_t wdt_feed_task(tcb_t *task) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    if (!task) {
        return WDT_ERROR_INVALID_PARAM;
    }

    wdt_task_entry_t *entry = find_task_entry(task);
    if (!entry) {
        return WDT_ERROR_TASK_NOT_REGISTERED;
    }

    entry->last_feed_time = os_get_tick_count();
    return WDT_OK;
}

wdt_error_t wdt_enable_task(tcb_t *task) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    if (!task) {
        return WDT_ERROR_INVALID_PARAM;
    }

    wdt_task_entry_t *entry = find_task_entry(task);
    if (!entry) {
        return WDT_ERROR_TASK_NOT_REGISTERED;
    }

    entry->enabled = true;
    entry->last_feed_time = os_get_tick_count();
    return WDT_OK;
}

wdt_error_t wdt_disable_task(tcb_t *task) {
    if (!g_wdt_state.initialized) {
        return WDT_ERROR_NOT_INITIALIZED;
    }

    if (!task) {
        return WDT_ERROR_INVALID_PARAM;
    }

    wdt_task_entry_t *entry = find_task_entry(task);
    if (!entry) {
        return WDT_ERROR_TASK_NOT_REGISTERED;
    }

    entry->enabled = false;
    return WDT_OK;
}

bool wdt_is_task_registered(tcb_t *task) {
    return find_task_entry(task) != NULL;
}

/* ========================================================================
 * Status and Statistics
 * ======================================================================== */

wdt_error_t wdt_get_status(wdt_status_t *status) {
    if (!status) {
        return WDT_ERROR_INVALID_PARAM;
    }

    status->initialized = g_wdt_state.initialized;
    status->enabled = g_wdt_state.enabled;
    status->type = g_wdt_state.config.type;
    status->timeout_ms = g_wdt_state.config.timeout_ms;
    status->last_feed_time = g_wdt_state.last_feed_time;
    status->registered_tasks = g_wdt_state.num_registered_tasks;

    uint32_t current_time = os_get_tick_count();
    uint32_t elapsed = current_time - g_wdt_state.last_feed_time;
    status->time_remaining_ms = (elapsed < g_wdt_state.config.timeout_ms) ?
                                (g_wdt_state.config.timeout_ms - elapsed) : 0;

    return WDT_OK;
}

wdt_error_t wdt_get_stats(wdt_stats_t *stats) {
    if (!stats) {
        return WDT_ERROR_INVALID_PARAM;
    }

    memcpy(stats, &g_wdt_state.stats, sizeof(wdt_stats_t));
    return WDT_OK;
}

wdt_error_t wdt_reset_stats(void) {
    memset(&g_wdt_state.stats, 0, sizeof(wdt_stats_t));

    /* Reset task timeout counts */
    for (uint32_t i = 0; i < g_wdt_state.num_registered_tasks; i++) {
        g_wdt_state.task_entries[i].timeout_count = 0;
    }

    return WDT_OK;
}

wdt_reset_reason_t wdt_get_last_reset_reason(void) {
    return g_wdt_state.reset_reason;
}

uint32_t wdt_get_time_remaining(void) {
    if (!g_wdt_state.initialized) {
        return 0;
    }

    uint32_t current_time = os_get_tick_count();
    uint32_t elapsed = current_time - g_wdt_state.last_feed_time;

    return (elapsed < g_wdt_state.config.timeout_ms) ?
           (g_wdt_state.config.timeout_ms - elapsed) : 0;
}

/* ========================================================================
 * Utility Functions
 * ======================================================================== */

const char *wdt_error_to_string(wdt_error_t error) {
    switch (error) {
        case WDT_OK: return "OK";
        case WDT_ERROR_INVALID_PARAM: return "Invalid parameter";
        case WDT_ERROR_NOT_INITIALIZED: return "Not initialized";
        case WDT_ERROR_ALREADY_INITIALIZED: return "Already initialized";
        case WDT_ERROR_TIMEOUT_TOO_SHORT: return "Timeout too short";
        case WDT_ERROR_TIMEOUT_TOO_LONG: return "Timeout too long";
        case WDT_ERROR_NOT_ENABLED: return "Not enabled";
        case WDT_ERROR_TASK_NOT_REGISTERED: return "Task not registered";
        case WDT_ERROR_MAX_TASKS_REACHED: return "Maximum tasks reached";
        default: return "Unknown error";
    }
}

const char *wdt_reset_reason_to_string(wdt_reset_reason_t reason) {
    switch (reason) {
        case WDT_RESET_NONE: return "None";
        case WDT_RESET_HARDWARE: return "Hardware watchdog";
        case WDT_RESET_SOFTWARE: return "Software watchdog";
        case WDT_RESET_TASK_TIMEOUT: return "Task timeout";
        default: return "Unknown";
    }
}

void wdt_print_status(void) {
    wdt_status_t status;
    if (wdt_get_status(&status) != WDT_OK) {
        printf("Failed to get watchdog status\n");
        return;
    }

    printf("\n=== Watchdog Status ===\n");
    printf("Initialized: %s\n", status.initialized ? "Yes" : "No");
    printf("Enabled: %s\n", status.enabled ? "Yes" : "No");
    printf("Type: %d\n", status.type);
    printf("Timeout: %lu ms\n", (unsigned long)status.timeout_ms);
    printf("Time Remaining: %lu ms\n", (unsigned long)status.time_remaining_ms);
    printf("Registered Tasks: %lu\n", (unsigned long)status.registered_tasks);
    printf("=====================\n\n");
}

void wdt_print_stats(void) {
    printf("\n=== Watchdog Statistics ===\n");
    printf("Total Feeds: %lu\n", (unsigned long)g_wdt_state.stats.total_feeds);
    printf("Total Timeouts: %lu\n", (unsigned long)g_wdt_state.stats.total_timeouts);
    printf("Hardware Resets: %lu\n", (unsigned long)g_wdt_state.stats.hw_resets);
    printf("Software Resets: %lu\n", (unsigned long)g_wdt_state.stats.sw_resets);
    printf("Task Timeouts: %lu\n", (unsigned long)g_wdt_state.stats.task_timeouts);
    printf("Last Reset Reason: %s\n",
           wdt_reset_reason_to_string(g_wdt_state.stats.last_reset_reason));
    printf("===========================\n\n");
}

void wdt_print_registered_tasks(void) {
    printf("\n=== Registered Tasks ===\n");
    printf("Total: %lu\n", (unsigned long)g_wdt_state.num_registered_tasks);

    for (uint32_t i = 0; i < g_wdt_state.num_registered_tasks; i++) {
        wdt_task_entry_t *entry = &g_wdt_state.task_entries[i];
        printf("Task %lu: %p, Timeout: %lu ms, Enabled: %s, Timeouts: %lu\n",
               (unsigned long)i,
               (void*)entry->task,
               (unsigned long)entry->timeout_ms,
               entry->enabled ? "Yes" : "No",
               (unsigned long)entry->timeout_count);
    }

    printf("========================\n\n");
}

/* ========================================================================
 * Hardware-Specific Functions
 * ======================================================================== */

void wdt_trigger_reset(void) {
    /* Call callback if configured */
    if (g_wdt_state.config.callback) {
        g_wdt_state.config.callback(WDT_RESET_SOFTWARE, NULL);
    }

    g_wdt_state.reset_reason = WDT_RESET_SOFTWARE;
    g_wdt_state.stats.sw_resets++;

    hal_wdt_trigger_reset();
}

bool wdt_was_reset_by_watchdog(void) {
    return hal_wdt_was_reset_by_watchdog();
}

void wdt_clear_reset_flag(void) {
    hal_wdt_clear_reset_flag();
}
