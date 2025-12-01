/**
 * TinyOS Software Timers Example
 *
 * Demonstrates the use of software timers for periodic and one-shot callbacks
 * Useful for IoT applications that need periodic tasks without dedicated tasks
 */

#include "tinyos.h"

/* Timer control blocks */
static timer_t led_blink_timer;
static timer_t sensor_read_timer;
static timer_t watchdog_timer;
static timer_t one_shot_timer;

/* Application state */
static struct {
    bool led_state;
    uint32_t sensor_reading_count;
    uint32_t watchdog_kicks;
    bool one_shot_executed;
} app_state = {false, 0, 0, false};

/**
 * LED blink timer callback
 * Toggles LED state every 500ms (auto-reload timer)
 */
void led_blink_callback(void *param) {
    (void)param;

    app_state.led_state = !app_state.led_state;

    /* In real hardware, toggle GPIO here */
    // gpio_set_pin(LED_PIN, app_state.led_state);
}

/**
 * Sensor reading timer callback
 * Reads sensor data every 1 second
 */
void sensor_read_callback(void *param) {
    (void)param;

    /* Simulate sensor reading */
    app_state.sensor_reading_count++;

    /* In real application, read actual sensor */
    // float temperature = read_temperature_sensor();
    // process_sensor_data(temperature);
}

/**
 * Watchdog kick timer callback
 * Kicks watchdog every 500ms to prevent system reset
 */
void watchdog_kick_callback(void *param) {
    (void)param;

    app_state.watchdog_kicks++;

    /* In real application, kick hardware watchdog */
    // kick_watchdog();
}

/**
 * One-shot timer callback
 * Executes once after 5 seconds
 */
void one_shot_callback(void *param) {
    (void)param;

    app_state.one_shot_executed = true;

    /* Perform one-time initialization */
    // initialize_network();
    // calibrate_sensors();
}

/**
 * Monitoring task
 * Displays timer statistics
 */
void monitor_task(void *param) {
    (void)param;

    while (1) {
        /* Display statistics (would output to UART in real hardware) */
        // printf("LED blinks: %d\n", app_state.sensor_reading_count * 2);
        // printf("Sensor readings: %d\n", app_state.sensor_reading_count);
        // printf("Watchdog kicks: %d\n", app_state.watchdog_kicks);
        // printf("One-shot executed: %s\n", app_state.one_shot_executed ? "Yes" : "No");
        // printf("Active timers: %d\n", os_timer_get_count());

        os_task_delay(5000);  /* Update every 5 seconds */
    }
}

/**
 * Control task
 * Demonstrates timer control operations
 */
void control_task(void *param) {
    (void)param;

    os_task_delay(10000);  /* Wait 10 seconds */

    /* Demonstrate changing timer period */
    os_timer_change_period(&led_blink_timer, 1000);  /* Change to 1 second */

    os_task_delay(10000);  /* Wait another 10 seconds */

    /* Demonstrate stopping and restarting timer */
    os_timer_stop(&sensor_read_timer);
    os_task_delay(5000);
    os_timer_start(&sensor_read_timer);

    while (1) {
        os_task_delay(1000);
    }
}

/**
 * Example: Timeout detection using one-shot timer
 */
static timer_t timeout_timer;
static bool operation_completed = false;

void timeout_callback(void *param) {
    (void)param;

    if (!operation_completed) {
        /* Operation timed out - handle error */
        // handle_timeout_error();
    }
}

void async_operation_with_timeout(void) {
    /* Reset state */
    operation_completed = false;

    /* Create and start timeout timer */
    os_timer_create(
        &timeout_timer,
        "timeout",
        TIMER_ONE_SHOT,
        3000,  /* 3 second timeout */
        timeout_callback,
        NULL
    );

    os_timer_start(&timeout_timer);

    /* Start async operation */
    // start_async_operation();

    /* When operation completes, stop timeout timer */
    // operation_completed = true;
    // os_timer_stop(&timeout_timer);
}

/**
 * Example: Periodic heartbeat for network keepalive
 */
static timer_t heartbeat_timer;

void heartbeat_callback(void *param) {
    (void)param;

    /* Send heartbeat to server */
    // send_heartbeat_packet();
}

void setup_network_heartbeat(void) {
    os_timer_create(
        &heartbeat_timer,
        "heartbeat",
        TIMER_AUTO_RELOAD,
        30000,  /* Send every 30 seconds */
        heartbeat_callback,
        NULL
    );

    os_timer_start(&heartbeat_timer);
}

/**
 * Example: Debounce timer for button input
 */
static timer_t debounce_timer;
static bool button_state_stable = false;

void debounce_callback(void *param) {
    (void)param;

    /* Button state has been stable for debounce period */
    button_state_stable = true;

    /* Process button press */
    // handle_button_press();
}

void button_interrupt_handler(void) {
    /* Reset debounce timer on each button event */
    if (os_timer_is_active(&debounce_timer)) {
        os_timer_reset(&debounce_timer);
    } else {
        button_state_stable = false;
        os_timer_start(&debounce_timer);
    }
}

/**
 * Main application
 */
int main(void) {
    /* Initialize OS */
    os_init();

    /* Create LED blink timer (auto-reload, 500ms) */
    os_timer_create(
        &led_blink_timer,
        "led_blink",
        TIMER_AUTO_RELOAD,
        500,  /* 500ms period */
        led_blink_callback,
        NULL
    );

    /* Create sensor reading timer (auto-reload, 1000ms) */
    os_timer_create(
        &sensor_read_timer,
        "sensor",
        TIMER_AUTO_RELOAD,
        1000,  /* 1 second period */
        sensor_read_callback,
        NULL
    );

    /* Create watchdog timer (auto-reload, 500ms) */
    os_timer_create(
        &watchdog_timer,
        "watchdog",
        TIMER_AUTO_RELOAD,
        500,  /* 500ms period */
        watchdog_kick_callback,
        NULL
    );

    /* Create one-shot timer (fires once after 5 seconds) */
    os_timer_create(
        &one_shot_timer,
        "one_shot",
        TIMER_ONE_SHOT,
        5000,  /* 5 seconds */
        one_shot_callback,
        NULL
    );

    /* Create debounce timer (one-shot, 50ms) */
    os_timer_create(
        &debounce_timer,
        "debounce",
        TIMER_ONE_SHOT,
        50,  /* 50ms debounce */
        debounce_callback,
        NULL
    );

    /* Start timers */
    os_timer_start(&led_blink_timer);
    os_timer_start(&sensor_read_timer);
    os_timer_start(&watchdog_timer);
    os_timer_start(&one_shot_timer);

    /* Create monitoring task */
    static tcb_t monitor;
    os_task_create(
        &monitor,
        "monitor",
        monitor_task,
        NULL,
        PRIORITY_LOW
    );

    /* Create control task */
    static tcb_t control;
    os_task_create(
        &control,
        "control",
        control_task,
        NULL,
        PRIORITY_NORMAL
    );

    /* Setup additional examples */
    setup_network_heartbeat();

    /* Start scheduler */
    os_start();

    return 0;
}

/*
 * Timer Use Cases Summary:
 *
 * 1. LED Blinking: Visual feedback without dedicated task
 * 2. Periodic Sensor Reading: Efficient data collection
 * 3. Watchdog Kicking: System health monitoring
 * 4. One-Shot Initialization: Delayed startup operations
 * 5. Timeout Detection: Async operation monitoring
 * 6. Network Heartbeat: Keepalive messages
 * 7. Button Debouncing: Input filtering
 *
 * Benefits over dedicated tasks:
 * - Lower memory usage (no task stack required)
 * - Simpler for periodic operations
 * - Callbacks execute in interrupt context (fast response)
 * - Easy to create/destroy dynamically
 *
 * Considerations:
 * - Callbacks run in interrupt context (keep them short)
 * - Cannot call blocking operations in callbacks
 * - Use tasks for complex, long-running operations
 */
