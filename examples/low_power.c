/**
 * TinyOS Low-Power Mode Example
 *
 * Demonstrates:
 * - Power mode configuration
 * - Sleep and deep sleep modes
 * - Wakeup sources
 * - Battery life estimation
 * - Power statistics
 *
 * Hardware: ARM Cortex-M with low-power modes
 */

#include "tinyos.h"
#include <stdio.h>

/* Simulate sensor reading */
typedef struct {
    float temperature;
    uint16_t battery_voltage_mv;
} sensor_data_t;

/* Task control blocks */
static tcb_t sensor_task;
static tcb_t monitor_task;
static tcb_t network_task;

/* Event group for coordination */
static event_group_t system_events;
#define EVENT_SENSOR_READY    (1 << 0)
#define EVENT_NETWORK_READY   (1 << 1)

/**
 * Simulate sensor reading
 */
static sensor_data_t read_sensor(void) {
    sensor_data_t data;
    data.temperature = 25.5f;  /* Simulated */
    data.battery_voltage_mv = 3300;  /* 3.3V */
    return data;
}

/**
 * Power mode callback - called when entering/exiting power modes
 */
static void power_mode_callback(power_mode_t mode) {
    const char *mode_str[] = {
        "ACTIVE",
        "IDLE",
        "SLEEP",
        "DEEP_SLEEP"
    };

    if (mode < POWER_MODE_MAX) {
        /* In real implementation, log or handle mode change */
        (void)mode_str[mode];
    }
}

/**
 * Sensor task - reads sensor and goes to sleep between readings
 * Demonstrates periodic wakeup pattern for low power
 */
static void sensor_task_func(void *param) {
    (void)param;

    printf("[Sensor] Task started\n");

    while (1) {
        /* Wake up and read sensor */
        sensor_data_t data = read_sensor();

        printf("[Sensor] Temperature: %.1f°C, Battery: %dmV\n",
               data.temperature, data.battery_voltage_mv);

        /* Signal data is ready */
        os_event_group_set_bits(&system_events, EVENT_SENSOR_READY);

        /* Enter sleep mode for 5 seconds to save power */
        printf("[Sensor] Entering sleep mode for 5 seconds...\n");
        os_power_enter_sleep(5000);

        printf("[Sensor] Woke up from sleep\n");
    }
}

/**
 * Network task - simulates network activity with power saving
 * Goes into deep sleep when network is idle
 */
static void network_task_func(void *param) {
    (void)param;
    uint32_t packet_count = 0;

    printf("[Network] Task started\n");

    while (1) {
        /* Wait for sensor data */
        uint32_t bits;
        os_error_t result = os_event_group_wait_bits(
            &system_events,
            EVENT_SENSOR_READY,
            EVENT_WAIT_ALL | EVENT_CLEAR_ON_EXIT,
            &bits,
            10000  /* 10 second timeout */
        );

        if (result == OS_OK) {
            /* Simulate sending data over network */
            printf("[Network] Transmitting packet #%lu...\n", packet_count++);
            os_task_delay(100);  /* Simulate TX time */

            printf("[Network] Transmission complete\n");

            /* Enter deep sleep after transmission */
            printf("[Network] Entering deep sleep for 3 seconds...\n");
            os_power_enter_deep_sleep(3000);

            printf("[Network] Woke up from deep sleep\n");
        } else {
            /* Timeout - no data to send */
            printf("[Network] No data, staying in deep sleep\n");
            os_power_enter_deep_sleep(5000);
        }
    }
}

/**
 * Monitor task - displays power statistics
 */
static void monitor_task_func(void *param) {
    (void)param;

    printf("[Monitor] Task started\n");

    while (1) {
        /* Wait 10 seconds between reports */
        os_task_delay(10000);

        /* Get power statistics */
        power_stats_t stats;
        os_power_get_stats(&stats);

        printf("\n=== Power Statistics ===\n");
        printf("Current Mode: %d\n", stats.current_mode);
        printf("Active Time: %lu ms\n", stats.total_active_time_ms);
        printf("Sleep Time: %lu ms\n", stats.total_sleep_time_ms);
        printf("Power Consumption: %lu mW\n", stats.power_consumption_mw);

        if (stats.estimated_battery_life_hours > 0 &&
            stats.estimated_battery_life_hours < 0xFFFFFFFF) {
            printf("Est. Battery Life: %lu hours\n", stats.estimated_battery_life_hours);
        }

        /* Calculate sleep percentage */
        uint32_t total_time = stats.total_active_time_ms + stats.total_sleep_time_ms;
        if (total_time > 0) {
            uint32_t sleep_pct = (stats.total_sleep_time_ms * 100) / total_time;
            printf("Sleep Percentage: %lu%%\n", sleep_pct);
        }

        printf("========================\n\n");
    }
}

/**
 * Main function
 */
int main(void) {
    printf("TinyOS Low-Power Mode Example\n");
    printf("==============================\n\n");

    /* Initialize OS */
    os_init();
    os_power_init();

    /* Configure power management */
    power_config_t power_config = {
        .idle_mode_enabled = true,
        .sleep_mode_enabled = true,
        .deep_sleep_threshold_ms = 1000,  /* Use deep sleep for >1s delays */
        .cpu_freq_hz = 48000000,          /* 48 MHz */
        .min_cpu_freq_hz = 8000000,       /* 8 MHz minimum */
        .max_cpu_freq_hz = 48000000,      /* 48 MHz maximum */
        .battery_capacity_mah = 2000,     /* 2000 mAh battery */
        .battery_voltage_mv = 3300        /* 3.3V nominal */
    };

    os_power_configure(&power_config);

    /* Register power mode callbacks */
    os_power_register_callback(power_mode_callback, power_mode_callback);

    /* Configure wakeup sources */
    os_power_configure_wakeup(WAKEUP_SOURCE_RTC, true);
    os_power_configure_wakeup(WAKEUP_SOURCE_GPIO, true);

    /* Enable tickless idle for maximum power savings */
    os_power_enable_tickless_idle(true);

    /* Initialize event group */
    os_event_group_init(&system_events);

    /* Create tasks */
    printf("Creating tasks...\n");

    os_task_create(
        &sensor_task,
        "sensor",
        sensor_task_func,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &network_task,
        "network",
        network_task_func,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &monitor_task,
        "monitor",
        monitor_task_func,
        NULL,
        PRIORITY_LOW
    );

    printf("Starting scheduler...\n\n");

    /* Start scheduler */
    os_start();

    /* Should never reach here */
    return 0;
}

/**
 * Example output:
 *
 * TinyOS Low-Power Mode Example
 * ==============================
 *
 * Creating tasks...
 * Starting scheduler...
 *
 * [Sensor] Task started
 * [Network] Task started
 * [Monitor] Task started
 * [Sensor] Temperature: 25.5°C, Battery: 3300mV
 * [Sensor] Entering sleep mode for 5 seconds...
 * [Network] Transmitting packet #0...
 * [Network] Transmission complete
 * [Network] Entering deep sleep for 3 seconds...
 * [Sensor] Woke up from sleep
 * [Sensor] Temperature: 25.5°C, Battery: 3300mV
 * [Network] Woke up from deep sleep
 *
 * === Power Statistics ===
 * Current Mode: 0
 * Active Time: 2500 ms
 * Sleep Time: 7500 ms
 * Power Consumption: 10 mW
 * Est. Battery Life: 660 hours
 * Sleep Percentage: 75%
 * ========================
 */
