/**
 * TinyOS Task Statistics Example
 *
 * Demonstrates how to use the task statistics API to monitor:
 * - CPU usage per task and overall
 * - Task runtime and context switches
 * - Stack usage and high water marks
 * - System uptime and task states
 */

#include "tinyos.h"
#include <stdio.h>

/* Task control blocks */
static tcb_t worker_task1;
static tcb_t worker_task2;
static tcb_t heavy_task;
static tcb_t monitor_task_tcb;

/* Statistics buffer */
static task_stats_t task_stats;
static system_stats_t sys_stats;

/**
 * Worker task 1 - Light workload
 */
void worker1(void *param) {
    (void)param;

    printf("[Worker1] Started\n");

    while (1) {
        /* Simulate light work */
        for (volatile int i = 0; i < 1000; i++);

        /* Delay to allow other tasks to run */
        os_task_delay(100);  /* 100ms */
    }
}

/**
 * Worker task 2 - Medium workload
 */
void worker2(void *param) {
    (void)param;

    printf("[Worker2] Started\n");

    while (1) {
        /* Simulate medium work */
        for (volatile int i = 0; i < 5000; i++);

        /* Delay */
        os_task_delay(150);  /* 150ms */
    }
}

/**
 * Heavy task - High CPU usage
 */
void heavy_worker(void *param) {
    (void)param;

    printf("[HeavyWorker] Started\n");

    while (1) {
        /* Simulate heavy computation */
        for (volatile int i = 0; i < 10000; i++);

        /* Short delay */
        os_task_delay(50);  /* 50ms */
    }
}

/**
 * Monitor task - Displays statistics periodically
 */
void monitor_task(void *param) {
    (void)param;

    printf("[Monitor] Started - Will display statistics every 2 seconds\n");
    printf("\n");

    while (1) {
        /* Wait 2 seconds */
        os_task_delay(2000);

        printf("\n");
        printf("═══════════════════════════════════════════════════════════════\n");
        printf("                    SYSTEM STATISTICS\n");
        printf("═══════════════════════════════════════════════════════════════\n");

        /* Get and display system statistics */
        if (os_get_system_stats(&sys_stats) == OS_OK) {
            printf("System Uptime:        %lu seconds (%lu ticks)\n",
                   sys_stats.uptime_seconds, sys_stats.uptime_ticks);
            printf("Total Tasks:          %lu\n", sys_stats.total_tasks);
            printf("Running Tasks:        %lu\n", sys_stats.running_tasks);
            printf("Context Switches:     %lu\n", sys_stats.total_context_switches);
            printf("Overall CPU Usage:    %.2f%%\n", sys_stats.cpu_usage);
            printf("Idle Time:            %lu ticks (%.2f%%)\n",
                   sys_stats.idle_time,
                   100.0f - sys_stats.cpu_usage);
            printf("Free Heap:            %zu bytes\n", sys_stats.free_heap);
            printf("\n");
        }

        printf("───────────────────────────────────────────────────────────────\n");
        printf("                    TASK STATISTICS\n");
        printf("───────────────────────────────────────────────────────────────\n");
        printf("%-12s %8s %8s %10s %8s %10s %9s\n",
               "Task", "Priority", "State", "CPU%%", "Runtime", "Switches", "Stack");
        printf("───────────────────────────────────────────────────────────────\n");

        /* Display statistics for each task */
        const char *state_names[] = {"READY", "RUNNING", "BLOCKED", "SUSPEND", "TERM"};

        /* Worker1 stats */
        if (os_task_get_stats(&worker_task1, &task_stats) == OS_OK) {
            printf("%-12s %8d %8s %9.2f%% %8lu %10lu %5lu/%lu\n",
                   task_stats.name,
                   task_stats.priority,
                   state_names[task_stats.state],
                   task_stats.cpu_usage,
                   task_stats.run_time,
                   task_stats.context_switches,
                   task_stats.stack_used,
                   task_stats.stack_size);
        }

        /* Worker2 stats */
        if (os_task_get_stats(&worker_task2, &task_stats) == OS_OK) {
            printf("%-12s %8d %8s %9.2f%% %8lu %10lu %5lu/%lu\n",
                   task_stats.name,
                   task_stats.priority,
                   state_names[task_stats.state],
                   task_stats.cpu_usage,
                   task_stats.run_time,
                   task_stats.context_switches,
                   task_stats.stack_used,
                   task_stats.stack_size);
        }

        /* Heavy worker stats */
        if (os_task_get_stats(&heavy_task, &task_stats) == OS_OK) {
            printf("%-12s %8d %8s %9.2f%% %8lu %10lu %5lu/%lu\n",
                   task_stats.name,
                   task_stats.priority,
                   state_names[task_stats.state],
                   task_stats.cpu_usage,
                   task_stats.run_time,
                   task_stats.context_switches,
                   task_stats.stack_used,
                   task_stats.stack_size);
        }

        /* Monitor task stats */
        if (os_task_get_stats(&monitor_task_tcb, &task_stats) == OS_OK) {
            printf("%-12s %8d %8s %9.2f%% %8lu %10lu %5lu/%lu\n",
                   task_stats.name,
                   task_stats.priority,
                   state_names[task_stats.state],
                   task_stats.cpu_usage,
                   task_stats.run_time,
                   task_stats.context_switches,
                   task_stats.stack_used,
                   task_stats.stack_size);
        }

        printf("═══════════════════════════════════════════════════════════════\n");
        printf("\n");
    }
}

/**
 * Main function
 */
int main(void) {
    printf("\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("         TinyOS Task Statistics Demonstration\n");
    printf("═══════════════════════════════════════════════════════════════\n");
    printf("\n");
    printf("This demo shows real-time task statistics including:\n");
    printf("  • CPU usage per task and system-wide\n");
    printf("  • Task runtime and context switch counts\n");
    printf("  • Stack usage monitoring\n");
    printf("  • System uptime and task states\n");
    printf("\n");
    printf("Starting system...\n\n");

    /* Initialize OS */
    os_init();

    /* Initialize memory management */
    os_mem_init();

    /* Create worker tasks with different priorities */
    os_task_create(
        &worker_task1,
        "Worker-1",
        worker1,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &worker_task2,
        "Worker-2",
        worker2,
        NULL,
        PRIORITY_NORMAL
    );

    os_task_create(
        &heavy_task,
        "HeavyWork",
        heavy_worker,
        NULL,
        PRIORITY_HIGH  /* Higher priority = more CPU time */
    );

    /* Create monitor task with low priority */
    os_task_create(
        &monitor_task_tcb,
        "Monitor",
        monitor_task,
        NULL,
        PRIORITY_LOW
    );

    printf("Tasks created:\n");
    printf("  • Worker-1   (Priority: NORMAL, Light workload)\n");
    printf("  • Worker-2   (Priority: NORMAL, Medium workload)\n");
    printf("  • HeavyWork  (Priority: HIGH, Heavy workload)\n");
    printf("  • Monitor    (Priority: LOW, Stats display)\n");
    printf("\n");
    printf("Starting scheduler...\n");
    printf("\n");

    /* Start the scheduler */
    os_start();

    /* Should never reach here */
    return 0;
}
