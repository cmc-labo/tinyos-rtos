/**
 * TinyOS Power Management
 *
 * Low-power mode support for battery-powered IoT devices
 * Features:
 * - Multiple power modes (Active, Idle, Sleep, Deep Sleep)
 * - Tickless idle mode to reduce power consumption
 * - Wakeup source configuration
 * - Power consumption monitoring and battery life estimation
 */

#include "tinyos.h"
#include <string.h>

/* Power management state */
static struct {
    power_mode_t current_mode;
    power_config_t config;
    uint32_t sleep_ticks;
    uint32_t wakeup_sources;
    bool tickless_idle_enabled;
    power_callback_t enter_callback;
    power_callback_t exit_callback;
    uint32_t total_sleep_time_ms;
} power_state = {
    .current_mode = POWER_MODE_ACTIVE,
    .tickless_idle_enabled = false,
    .wakeup_sources = 0,
    .total_sleep_time_ms = 0
};

/* Platform-specific power control functions (weak symbols for override) */
__attribute__((weak)) void platform_enter_sleep_mode(void) {
    /* Default: WFI (Wait For Interrupt) */
    __asm__ volatile("wfi");
}

__attribute__((weak)) void platform_enter_deep_sleep_mode(void) {
    /* Default: WFI - platform should override with deep sleep */
    __asm__ volatile("wfi");
}

__attribute__((weak)) void platform_set_clock_frequency(uint32_t freq_hz) {
    /* Platform-specific clock adjustment */
    (void)freq_hz;
}

__attribute__((weak)) void platform_enable_wakeup_source(wakeup_source_t source) {
    /* Platform-specific wakeup source configuration */
    (void)source;
}

__attribute__((weak)) void platform_disable_wakeup_source(wakeup_source_t source) {
    /* Platform-specific wakeup source disable */
    (void)source;
}

__attribute__((weak)) uint32_t platform_get_power_consumption_mw(void) {
    /* Return estimated values based on power mode */
    switch (power_state.current_mode) {
        case POWER_MODE_ACTIVE:     return 50;   /* 50mW */
        case POWER_MODE_IDLE:       return 10;   /* 10mW */
        case POWER_MODE_SLEEP:      return 1;    /* 1mW */
        case POWER_MODE_DEEP_SLEEP: return 0;    /* <1mW */
        default:                    return 50;
    }
}

/**
 * Initialize power management
 */
void os_power_init(void) {
    memset(&power_state, 0, sizeof(power_state));
    power_state.current_mode = POWER_MODE_ACTIVE;
    power_state.tickless_idle_enabled = false;

    /* Set default configuration */
    power_state.config.idle_mode_enabled = true;
    power_state.config.sleep_mode_enabled = true;
    power_state.config.deep_sleep_threshold_ms = 1000;  /* 1 second */
}

/**
 * Configure power management
 */
os_error_t os_power_configure(const power_config_t *config) {
    if (config == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();
    memcpy(&power_state.config, config, sizeof(power_config_t));

    /* Apply clock frequency if specified */
    if (config->cpu_freq_hz > 0) {
        platform_set_clock_frequency(config->cpu_freq_hz);
    }

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Set power mode
 */
os_error_t os_power_set_mode(power_mode_t mode) {
    if (mode >= POWER_MODE_MAX) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t critical_state = os_enter_critical();

    power_mode_t old_mode = power_state.current_mode;

    /* Call exit callback for old mode */
    if (power_state.exit_callback != NULL) {
        power_state.exit_callback(old_mode);
    }

    power_state.current_mode = mode;

    /* Call enter callback for new mode */
    if (power_state.enter_callback != NULL) {
        power_state.enter_callback(mode);
    }

    os_exit_critical(critical_state);
    return OS_OK;
}

/**
 * Get current power mode
 */
power_mode_t os_power_get_mode(void) {
    return power_state.current_mode;
}

/**
 * Enter idle mode (called automatically by idle task)
 */
void os_power_enter_idle(void) {
    if (!power_state.config.idle_mode_enabled) {
        return;
    }

    /* Enter low-power idle */
    platform_enter_sleep_mode();
}

/**
 * Enter sleep mode
 */
os_error_t os_power_enter_sleep(uint32_t duration_ms) {
    if (duration_ms == 0) {
        return OS_ERROR_INVALID_PARAM;
    }

    if (!power_state.config.sleep_mode_enabled) {
        return OS_ERROR_PERMISSION_DENIED;
    }

    uint32_t state = os_enter_critical();

    power_mode_t old_mode = power_state.current_mode;
    power_state.current_mode = POWER_MODE_SLEEP;

    /* Calculate sleep duration in ticks */
    uint32_t sleep_ticks = (duration_ms * TICK_RATE_HZ) / 1000;
    power_state.sleep_ticks = sleep_ticks;

    os_exit_critical(state);

    /* Enter platform sleep mode */
    platform_enter_sleep_mode();

    /* Woken up */
    state = os_enter_critical();
    power_state.current_mode = old_mode;
    power_state.total_sleep_time_ms += duration_ms;
    os_exit_critical(state);

    return OS_OK;
}

/**
 * Enter deep sleep mode
 */
os_error_t os_power_enter_deep_sleep(uint32_t duration_ms) {
    if (duration_ms == 0) {
        return OS_ERROR_INVALID_PARAM;
    }

    if (!power_state.config.sleep_mode_enabled) {
        return OS_ERROR_PERMISSION_DENIED;
    }

    uint32_t state = os_enter_critical();

    power_mode_t old_mode = power_state.current_mode;
    power_state.current_mode = POWER_MODE_DEEP_SLEEP;

    /* Calculate sleep duration */
    uint32_t sleep_ticks = (duration_ms * TICK_RATE_HZ) / 1000;
    power_state.sleep_ticks = sleep_ticks;

    os_exit_critical(state);

    /* Enter platform deep sleep mode */
    platform_enter_deep_sleep_mode();

    /* Woken up */
    state = os_enter_critical();
    power_state.current_mode = old_mode;
    power_state.total_sleep_time_ms += duration_ms;
    os_exit_critical(state);

    return OS_OK;
}

/**
 * Enable/disable tickless idle mode
 */
os_error_t os_power_enable_tickless_idle(bool enable) {
    uint32_t state = os_enter_critical();
    power_state.tickless_idle_enabled = enable;
    os_exit_critical(state);
    return OS_OK;
}

/**
 * Check if tickless idle is enabled
 */
bool os_power_is_tickless_idle_enabled(void) {
    return power_state.tickless_idle_enabled;
}

/**
 * Register power mode callbacks
 */
os_error_t os_power_register_callback(
    power_callback_t enter_callback,
    power_callback_t exit_callback
) {
    uint32_t state = os_enter_critical();
    power_state.enter_callback = enter_callback;
    power_state.exit_callback = exit_callback;
    os_exit_critical(state);
    return OS_OK;
}

/**
 * Configure wakeup sources
 */
os_error_t os_power_configure_wakeup(wakeup_source_t source, bool enable) {
    if (source >= WAKEUP_SOURCE_MAX) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    if (enable) {
        power_state.wakeup_sources |= (1 << source);
        platform_enable_wakeup_source(source);
    } else {
        power_state.wakeup_sources &= ~(1 << source);
        platform_disable_wakeup_source(source);
    }

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Get power statistics
 */
void os_power_get_stats(power_stats_t *stats) {
    if (stats == NULL) {
        return;
    }

    uint32_t state = os_enter_critical();

    stats->current_mode = power_state.current_mode;
    stats->total_sleep_time_ms = power_state.total_sleep_time_ms;
    stats->total_active_time_ms = os_get_uptime_ms() - power_state.total_sleep_time_ms;
    stats->wakeup_sources = power_state.wakeup_sources;
    stats->power_consumption_mw = platform_get_power_consumption_mw();

    /* Calculate battery life estimate (if configured) */
    if (power_state.config.battery_capacity_mah > 0) {
        uint32_t avg_power_mw = stats->power_consumption_mw;
        uint32_t battery_mwh = power_state.config.battery_capacity_mah *
                               power_state.config.battery_voltage_mv / 1000;

        if (avg_power_mw > 0) {
            stats->estimated_battery_life_hours = battery_mwh / avg_power_mw;
        } else {
            stats->estimated_battery_life_hours = 0xFFFFFFFF; /* Effectively infinite */
        }
    } else {
        stats->estimated_battery_life_hours = 0;
    }

    os_exit_critical(state);
}

/**
 * Dynamic CPU frequency control
 */
os_error_t os_power_set_cpu_frequency(uint32_t freq_hz) {
    if (freq_hz == 0) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();
    power_state.config.cpu_freq_hz = freq_hz;
    platform_set_clock_frequency(freq_hz);
    os_exit_critical(state);

    return OS_OK;
}

/**
 * Get current power consumption (in milliwatts)
 */
uint32_t os_power_get_consumption_mw(void) {
    return platform_get_power_consumption_mw();
}

/**
 * Estimate battery life remaining
 * Returns estimated hours remaining based on current consumption
 */
uint32_t os_power_estimate_battery_life_hours(void) {
    if (power_state.config.battery_capacity_mah == 0) {
        return 0;  /* Battery capacity not configured */
    }

    uint32_t power_mw = platform_get_power_consumption_mw();
    if (power_mw == 0) {
        return 0xFFFFFFFF;  /* Effectively infinite */
    }

    /* Convert battery capacity to mWh */
    uint32_t battery_mwh = power_state.config.battery_capacity_mah *
                          power_state.config.battery_voltage_mv / 1000;

    return battery_mwh / power_mw;
}
