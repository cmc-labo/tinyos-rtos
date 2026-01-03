/**
 * @file watchdog_demo.c
 * @brief Watchdog Timer Demonstration for TinyOS
 *
 * This example demonstrates the watchdog timer functionality:
 * - Hardware and software watchdog
 * - Task watchdog monitoring
 * - Automatic system reset on timeout
 * - Watchdog statistics and status
 */

#include "tinyos.h"
#include "tinyos/watchdog.h"
#include <stdio.h>

/* Task control blocks */
static tcb_t watchdog_feeder_task;
static tcb_t monitored_task1;
static tcb_t monitored_task2;
static tcb_t statistics_task;

/* Simulated hang flag */
static volatile bool simulate_hang = false;

/**
 * @brief Watchdog pre-reset callback
 *
 * Called before system reset to allow cleanup
 */
static void watchdog_callback(wdt_reset_reason_t reason, tcb_t *task) {
    printf("\n!!! WATCHDOG TIMEOUT DETECTED !!!\n");
    printf("Reset Reason: %s\n", wdt_reset_reason_to_string(reason));
    if (task) {
        printf("Faulty Task: %p\n", (void*)task);
    }
    printf("System will reset in 1 second...\n");

    /* Cleanup code here */
    // Save critical data
    // Close files
    // etc.
}

/**
 * @brief Watchdog feeder task
 *
 * This task periodically feeds the watchdog to prevent timeout
 */
static void watchdog_feeder_task_func(void *param) {
    (void)param;

    printf("[Feeder] Watchdog feeder task started\n");

    while (1) {
        /* Feed watchdog every 500ms */
        os_task_delay(500);

        /* Feed watchdog */
        wdt_error_t err = wdt_feed();
        if (err == WDT_OK) {
            printf("[Feeder] Watchdog fed, time remaining: %lu ms\n",
                   (unsigned long)wdt_get_time_remaining());
        } else {
            printf("[Feeder] Failed to feed watchdog: %s\n",
                   wdt_error_to_string(err));
        }
    }
}

/**
 * @brief Monitored task 1 (Well-behaved)
 *
 * This task regularly feeds its watchdog
 */
static void monitored_task1_func(void *param) {
    (void)param;

    printf("[Task1] Monitored task 1 started\n");

    while (1) {
        /* Simulate work */
        printf("[Task1] Doing work...\n");
        os_task_delay(300);

        /* Feed task watchdog */
        wdt_error_t err = wdt_feed_task(&monitored_task1);
        if (err == WDT_OK) {
            printf("[Task1] Task watchdog fed\n");
        }
    }
}

/**
 * @brief Monitored task 2 (Can simulate hang)
 *
 * This task can be made to hang to demonstrate watchdog timeout
 */
static void monitored_task2_func(void *param) {
    (void)param;

    printf("[Task2] Monitored task 2 started\n");

    uint32_t iteration = 0;

    while (1) {
        iteration++;

        /* Simulate work */
        printf("[Task2] Iteration %lu\n", (unsigned long)iteration);
        os_task_delay(400);

        /* Simulate hang after 10 iterations */
        if (simulate_hang && iteration >= 10) {
            printf("[Task2] !!! SIMULATING HANG - Not feeding watchdog !!!\n");
            while (1) {
                os_task_delay(1000);
                printf("[Task2] Still hung...\n");
            }
        }

        /* Feed task watchdog */
        wdt_error_t err = wdt_feed_task(&monitored_task2);
        if (err == WDT_OK) {
            printf("[Task2] Task watchdog fed\n");
        }
    }
}

/**
 * @brief Statistics task
 *
 * Periodically prints watchdog statistics
 */
static void statistics_task_func(void *param) {
    (void)param;

    printf("[Stats] Statistics task started\n");

    while (1) {
        /* Print statistics every 5 seconds */
        os_task_delay(5000);

        printf("\n========================================\n");
        wdt_print_status();
        wdt_print_stats();
        wdt_print_registered_tasks();
        printf("========================================\n\n");
    }
}

/**
 * @brief Main function
 */
int main(void) {
    printf("\n");
    printf("=====================================\n");
    printf("  TinyOS Watchdog Timer Demo\n");
    printf("=====================================\n\n");

    /* Initialize OS */
    os_init();

    /* Check if last reset was caused by watchdog */
    if (wdt_was_reset_by_watchdog()) {
        printf("!!! System was reset by watchdog !!!\n");
        wdt_clear_reset_flag();
    }

    /* Configure watchdog */
    wdt_config_t wdt_config = {
        .type = WDT_TYPE_BOTH,          /* Use both hardware and software watchdog */
        .timeout_ms = 2000,             /* 2 second timeout */
        .auto_start = true,             /* Start watchdog automatically */
        .enable_reset = true,           /* Enable automatic reset on timeout */
        .callback = watchdog_callback   /* Pre-reset callback */
    };

    /* Initialize watchdog */
    wdt_error_t err = wdt_init(&wdt_config);
    if (err != WDT_OK) {
        printf("Failed to initialize watchdog: %s\n", wdt_error_to_string(err));
        return -1;
    }

    printf("Watchdog initialized successfully\n");
    printf("Timeout: %lu ms\n", (unsigned long)wdt_get_timeout());
    printf("Type: %s\n\n", "BOTH (Hardware + Software)");

    /* Create watchdog feeder task */
    os_task_create(
        &watchdog_feeder_task,
        "wdt_feeder",
        watchdog_feeder_task_func,
        NULL,
        PRIORITY_HIGH  /* High priority to ensure it runs */
    );

    /* Create monitored tasks */
    os_task_create(
        &monitored_task1,
        "monitored1",
        monitored_task1_func,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &monitored_task2,
        "monitored2",
        monitored_task2_func,
        NULL,
        PRIORITY_NORMAL
    );

    /* Create statistics task */
    os_task_create(
        &statistics_task,
        "stats",
        statistics_task_func,
        NULL,
        PRIORITY_LOW
    );

    /* Register tasks for watchdog monitoring */
    err = wdt_register_task(&monitored_task1, 1000);  /* 1 second timeout */
    if (err != WDT_OK) {
        printf("Failed to register task1: %s\n", wdt_error_to_string(err));
    } else {
        printf("Task 1 registered for watchdog monitoring (1000ms timeout)\n");
    }

    err = wdt_register_task(&monitored_task2, 1500);  /* 1.5 second timeout */
    if (err != WDT_OK) {
        printf("Failed to register task2: %s\n", wdt_error_to_string(err));
    } else {
        printf("Task 2 registered for watchdog monitoring (1500ms timeout)\n");
    }

    printf("\nSystem initialized. Starting tasks...\n");
    printf("\nTo simulate a task hang and watchdog reset:\n");
    printf("1. Let the system run normally for a few seconds\n");
    printf("2. Set 'simulate_hang' to true in debugger\n");
    printf("3. Watch Task 2 hang and trigger watchdog reset\n\n");

    /* Uncomment the line below to automatically simulate hang after startup */
    // simulate_hang = true;

    /* Start OS scheduler */
    os_start();

    /* Should never reach here */
    return 0;
}

/* Example output:
 *
 * =====================================
 *   TinyOS Watchdog Timer Demo
 * =====================================
 *
 * Watchdog initialized successfully
 * Timeout: 2000 ms
 * Type: BOTH (Hardware + Software)
 *
 * Task 1 registered for watchdog monitoring (1000ms timeout)
 * Task 2 registered for watchdog monitoring (1500ms timeout)
 *
 * System initialized. Starting tasks...
 *
 * [Feeder] Watchdog feeder task started
 * [Task1] Monitored task 1 started
 * [Task2] Monitored task 2 started
 * [Stats] Statistics task started
 * [Task1] Doing work...
 * [Task2] Iteration 1
 * [Feeder] Watchdog fed, time remaining: 1500 ms
 * [Task1] Task watchdog fed
 * [Task2] Task watchdog fed
 * [Task1] Doing work...
 * [Task2] Iteration 2
 * [Feeder] Watchdog fed, time remaining: 1500 ms
 * [Task1] Task watchdog fed
 * ...
 *
 * === Watchdog Status ===
 * Initialized: Yes
 * Enabled: Yes
 * Type: 2
 * Timeout: 2000 ms
 * Time Remaining: 1500 ms
 * Registered Tasks: 2
 * =====================
 *
 * === Watchdog Statistics ===
 * Total Feeds: 42
 * Total Timeouts: 0
 * Hardware Resets: 0
 * Software Resets: 0
 * Task Timeouts: 0
 * Last Reset Reason: None
 * ===========================
 *
 * === Registered Tasks ===
 * Total: 2
 * Task 0: 0x20001234, Timeout: 1000 ms, Enabled: Yes, Timeouts: 0
 * Task 1: 0x20001278, Timeout: 1500 ms, Enabled: Yes, Timeouts: 0
 * ========================
 *
 * [Task2] Iteration 10
 * [Task2] !!! SIMULATING HANG - Not feeding watchdog !!!
 * [Task2] Still hung...
 *
 * !!! WATCHDOG TIMEOUT DETECTED !!!
 * Reset Reason: Task timeout
 * Faulty Task: 0x20001278
 * System will reset in 1 second...
 *
 * [System Resets]
 */
