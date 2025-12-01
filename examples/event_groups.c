/**
 * TinyOS Event Groups Example
 *
 * Demonstrates event group usage for multi-task synchronization
 * in an IoT sensor application scenario.
 *
 * This example shows:
 * - Multiple tasks waiting for different event combinations
 * - Event bit setting from different tasks
 * - Wait for ANY vs. ALL event bits
 * - Auto-clear on exit functionality
 */

#include "tinyos.h"

/* Define event bits */
#define EVENT_SENSOR_READY      (1 << 0)  /* Bit 0: Sensor initialized */
#define EVENT_DATA_AVAILABLE    (1 << 1)  /* Bit 1: Data ready */
#define EVENT_NETWORK_READY     (1 << 2)  /* Bit 2: Network connected */
#define EVENT_UPLOAD_COMPLETE   (1 << 3)  /* Bit 3: Upload finished */
#define EVENT_ERROR_DETECTED    (1 << 4)  /* Bit 4: Error occurred */

/* Global event group */
static event_group_t system_events;

/* Simulated sensor data */
static float temperature = 25.0f;
static float humidity = 60.0f;

/**
 * Sensor initialization task
 * Sets the SENSOR_READY event when initialization completes
 */
void sensor_init_task(void *param) {
    (void)param;

    /* Simulate sensor initialization delay */
    os_task_delay(100);

    /* Signal that sensor is ready */
    os_event_group_set_bits(&system_events, EVENT_SENSOR_READY);

    /* Task complete */
    while (1) {
        os_task_delay(1000);
    }
}

/**
 * Sensor reading task
 * Waits for sensor ready, then reads data periodically
 */
void sensor_read_task(void *param) {
    (void)param;
    uint32_t bits_received;

    while (1) {
        /* Wait for sensor to be ready */
        os_error_t err = os_event_group_wait_bits(
            &system_events,
            EVENT_SENSOR_READY,
            EVENT_WAIT_ANY,
            &bits_received,
            5000  /* 5 second timeout */
        );

        if (err != OS_OK) {
            /* Timeout - sensor not ready */
            os_event_group_set_bits(&system_events, EVENT_ERROR_DETECTED);
            os_task_delay(1000);
            continue;
        }

        /* Read sensor (simulated) */
        temperature = 20.0f + (os_get_tick_count() % 100) / 10.0f;
        humidity = 50.0f + (os_get_tick_count() % 200) / 10.0f;

        /* Signal data is available */
        os_event_group_set_bits(&system_events, EVENT_DATA_AVAILABLE);

        /* Wait 1 second before next reading */
        os_task_delay(1000);
    }
}

/**
 * Network connection task
 * Establishes network connection and sets NETWORK_READY event
 */
void network_task(void *param) {
    (void)param;

    /* Simulate network connection delay */
    os_task_delay(200);

    /* Signal network is ready */
    os_event_group_set_bits(&system_events, EVENT_NETWORK_READY);

    /* Maintain connection */
    while (1) {
        os_task_delay(5000);
    }
}

/**
 * Data upload task
 * Waits for BOTH data available AND network ready before uploading
 */
void upload_task(void *param) {
    (void)param;
    uint32_t bits_received;

    while (1) {
        /* Wait for BOTH data available AND network ready */
        os_error_t err = os_event_group_wait_bits(
            &system_events,
            EVENT_DATA_AVAILABLE | EVENT_NETWORK_READY,
            EVENT_WAIT_ALL | EVENT_CLEAR_ON_EXIT,  /* Wait for ALL, clear on exit */
            &bits_received,
            10000  /* 10 second timeout */
        );

        if (err == OS_ERROR_TIMEOUT) {
            /* Timeout waiting for conditions */
            continue;
        }

        /* Upload data (simulated) */
        os_task_delay(50);  /* Simulate upload time */

        /* Signal upload complete */
        os_event_group_set_bits(&system_events, EVENT_UPLOAD_COMPLETE);
    }
}

/**
 * Error monitoring task
 * Waits for error events and handles them
 */
void error_monitor_task(void *param) {
    (void)param;
    uint32_t bits_received;

    while (1) {
        /* Wait for any error event */
        os_error_t err = os_event_group_wait_bits(
            &system_events,
            EVENT_ERROR_DETECTED,
            EVENT_WAIT_ANY | EVENT_CLEAR_ON_EXIT,
            &bits_received,
            0  /* Wait forever */
        );

        if (err == OS_OK) {
            /* Handle error - reset system state */
            os_event_group_clear_bits(&system_events,
                EVENT_SENSOR_READY | EVENT_NETWORK_READY);

            /* Attempt recovery */
            os_task_delay(500);
        }
    }
}

/**
 * Status display task
 * Periodically displays system status based on event bits
 */
void status_display_task(void *param) {
    (void)param;

    while (1) {
        uint32_t current_events = os_event_group_get_bits(&system_events);

        /* Check individual bits */
        bool sensor_ready = (current_events & EVENT_SENSOR_READY) != 0;
        bool data_available = (current_events & EVENT_DATA_AVAILABLE) != 0;
        bool network_ready = (current_events & EVENT_NETWORK_READY) != 0;

        /* Display status (would output to UART in real hardware) */
        if (sensor_ready && network_ready && data_available) {
            /* System fully operational */
        }

        os_task_delay(2000);
    }
}

/**
 * Main application
 */
int main(void) {
    /* Task control blocks */
    static tcb_t sensor_init;
    static tcb_t sensor_read;
    static tcb_t network;
    static tcb_t upload;
    static tcb_t error_monitor;
    static tcb_t status_display;

    /* Initialize OS */
    os_init();

    /* Initialize event group */
    os_event_group_init(&system_events);

    /* Create tasks */
    os_task_create(
        &sensor_init,
        "sensor_init",
        sensor_init_task,
        NULL,
        PRIORITY_HIGH
    );

    os_task_create(
        &sensor_read,
        "sensor_read",
        sensor_read_task,
        NULL,
        PRIORITY_HIGH
    );

    os_task_create(
        &network,
        "network",
        network_task,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &upload,
        "upload",
        upload_task,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &error_monitor,
        "error_mon",
        error_monitor_task,
        NULL,
        PRIORITY_CRITICAL
    );

    os_task_create(
        &status_display,
        "status",
        status_display_task,
        NULL,
        PRIORITY_LOW
    );

    /* Start scheduler */
    os_start();

    return 0;
}
