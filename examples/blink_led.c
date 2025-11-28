/**
 * TinyOS Example: LED Blink
 *
 * Demonstrates basic task creation and scheduling
 */

#include "tinyos.h"

/* GPIO definitions (example for ARM Cortex-M) */
#define LED_PIN         13
#define GPIO_BASE       0x40020000

/* Task control blocks */
static tcb_t blink_task;
static tcb_t monitor_task;

/**
 * LED blink task
 */
void led_blink(void *param) {
    (void)param;

    while (1) {
        /* Toggle LED */
        /* TODO: Actual GPIO manipulation */

        /* Delay 500ms */
        os_task_delay(500);
    }
}

/**
 * System monitor task
 */
void system_monitor(void *param) {
    (void)param;
    os_stats_t stats;

    while (1) {
        /* Get OS statistics */
        os_get_stats(&stats);

        /* Print stats (requires UART driver) */
        /* printf("Tasks: %d, Switches: %d\n",
                stats.total_tasks,
                stats.context_switches); */

        /* Delay 1 second */
        os_task_delay(1000);
    }
}

/**
 * Main entry point
 */
int main(void) {
    /* Initialize OS */
    os_init();

    /* Initialize security */
    os_security_init();

    /* Initialize memory */
    os_mem_init();

    /* Create LED blink task */
    os_task_create(
        &blink_task,
        "blink",
        led_blink,
        NULL,
        PRIORITY_NORMAL
    );

    /* Create monitor task */
    os_task_create(
        &monitor_task,
        "monitor",
        system_monitor,
        NULL,
        PRIORITY_LOW
    );

    /* Start OS scheduler */
    os_start();

    /* Should never reach here */
    return 0;
}
