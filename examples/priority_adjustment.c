/**
 * TinyOS Example: Dynamic Priority Adjustment
 *
 * This example demonstrates:
 * 1. Dynamic priority adjustment during runtime
 * 2. Priority inheritance to prevent priority inversion
 * 3. Task priority changes affecting scheduling
 *
 * Scenario:
 * - Low priority task holds a shared resource (mutex)
 * - High priority task needs the same resource
 * - Priority inheritance automatically boosts the low priority task
 * - After release, priority is restored
 */

#include "tinyos.h"
#include <stdio.h>

/* Task control blocks */
static tcb_t task_high;
static tcb_t task_medium;
static tcb_t task_low;

/* Shared resource */
static mutex_t shared_resource;
static volatile int shared_counter = 0;

/* Statistics */
static volatile uint32_t high_task_runs = 0;
static volatile uint32_t medium_task_runs = 0;
static volatile uint32_t low_task_runs = 0;

/**
 * Low priority task - simulates holding a resource
 */
void low_priority_task(void *param) {
    (void)param;

    printf("[LOW] Started with priority %d\n", os_task_get_priority(&task_low));

    while (1) {
        low_task_runs++;

        printf("[LOW] Trying to acquire mutex...\n");

        /* Acquire shared resource */
        if (os_mutex_lock(&shared_resource, 1000) == OS_OK) {
            printf("[LOW] Acquired mutex! Priority: %d\n",
                   os_task_get_priority(&task_low));

            /* Simulate long critical section */
            for (int i = 0; i < 3; i++) {
                shared_counter++;
                printf("[LOW] Working in critical section... counter=%d, priority=%d\n",
                       shared_counter, os_task_get_priority(&task_low));
                os_task_delay(100);
            }

            /* Release resource */
            os_mutex_unlock(&shared_resource);
            printf("[LOW] Released mutex! Priority: %d\n",
                   os_task_get_priority(&task_low));
        }

        os_task_delay(500);
    }
}

/**
 * Medium priority task - demonstrates priority levels
 */
void medium_priority_task(void *param) {
    (void)param;

    printf("[MEDIUM] Started with priority %d\n", os_task_get_priority(&task_medium));

    /* Wait a bit before starting */
    os_task_delay(200);

    while (1) {
        medium_task_runs++;
        printf("[MEDIUM] Running (no mutex needed)\n");
        os_task_delay(150);
    }
}

/**
 * High priority task - needs the shared resource
 */
void high_priority_task(void *param) {
    (void)param;

    printf("[HIGH] Started with priority %d\n", os_task_get_priority(&task_high));

    /* Wait for low priority task to acquire mutex */
    os_task_delay(300);

    while (1) {
        high_task_runs++;

        printf("[HIGH] Need shared resource!\n");

        /* Try to acquire mutex - this will trigger priority inheritance */
        if (os_mutex_lock(&shared_resource, 1000) == OS_OK) {
            printf("[HIGH] Got the mutex!\n");

            shared_counter += 10;
            printf("[HIGH] Updated counter to %d\n", shared_counter);

            os_mutex_unlock(&shared_resource);
            printf("[HIGH] Released mutex\n");
        }

        /* Dynamic priority adjustment demo */
        if (high_task_runs == 5) {
            printf("\n=== [HIGH] Demonstrating dynamic priority change ===\n");
            printf("[HIGH] Current priority: %d\n", os_task_get_priority(&task_high));

            /* Lower our priority temporarily */
            os_task_set_priority(&task_high, PRIORITY_NORMAL);
            printf("[HIGH] Changed to NORMAL priority: %d\n",
                   os_task_get_priority(&task_high));

            os_task_delay(500);

            /* Restore high priority */
            os_task_set_priority(&task_high, PRIORITY_HIGH);
            printf("[HIGH] Restored to HIGH priority: %d\n",
                   os_task_get_priority(&task_high));
            printf("=== Demo complete ===\n\n");
        }

        os_task_delay(400);
    }
}

/**
 * Monitor task - displays statistics
 */
void monitor_task(void *param) {
    (void)param;

    os_task_delay(1000);

    while (1) {
        printf("\n--- Task Statistics ---\n");
        printf("High priority runs:   %lu (priority: %d)\n",
               high_task_runs, os_task_get_priority(&task_high));
        printf("Medium priority runs: %lu (priority: %d)\n",
               medium_task_runs, os_task_get_priority(&task_medium));
        printf("Low priority runs:    %lu (priority: %d)\n",
               low_task_runs, os_task_get_priority(&task_low));
        printf("Shared counter:       %d\n", shared_counter);

        os_stats_t stats;
        os_get_stats(&stats);
        printf("Context switches:     %lu\n", stats.context_switches);
        printf("Uptime:               %lu ms\n", os_get_uptime_ms());
        printf("----------------------\n\n");

        os_task_delay(2000);
    }
}

int main(void) {
    printf("==============================================\n");
    printf("TinyOS - Dynamic Priority Adjustment Example\n");
    printf("==============================================\n\n");

    /* Initialize OS */
    os_init();

    /* Initialize shared resource */
    os_mutex_init(&shared_resource);

    /* Create tasks with different priorities */
    printf("Creating tasks...\n");

    os_task_create(
        &task_low,
        "low_task",
        low_priority_task,
        NULL,
        PRIORITY_LOW        // Priority: 192
    );
    printf("- Low priority task created (priority: %d)\n", PRIORITY_LOW);

    os_task_create(
        &task_medium,
        "medium_task",
        medium_priority_task,
        NULL,
        PRIORITY_NORMAL     // Priority: 128
    );
    printf("- Medium priority task created (priority: %d)\n", PRIORITY_NORMAL);

    os_task_create(
        &task_high,
        "high_task",
        high_priority_task,
        NULL,
        PRIORITY_HIGH       // Priority: 64
    );
    printf("- High priority task created (priority: %d)\n", PRIORITY_HIGH);

    /* Create monitor task */
    static tcb_t task_monitor;
    os_task_create(
        &task_monitor,
        "monitor",
        monitor_task,
        NULL,
        PRIORITY_NORMAL
    );
    printf("- Monitor task created\n\n");

    printf("Starting scheduler...\n");
    printf("Watch for priority inheritance when HIGH task needs mutex!\n\n");

    /* Start the scheduler - never returns */
    os_start();

    return 0;
}
