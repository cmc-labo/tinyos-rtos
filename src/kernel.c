/**
 * TinyOS Kernel - Core Implementation
 *
 * Implements the real-time scheduler and task management
 */

#include "tinyos.h"
#include <string.h>

/* Global kernel state */
static struct {
    tcb_t *current_task;
    tcb_t *ready_queue[256];  /* Priority-based ready queue */
    tcb_t task_pool[MAX_TASKS];
    uint8_t task_count;
    volatile uint32_t tick_count;
    volatile uint32_t context_switch_count;
    bool scheduler_running;
} kernel;

/* Idle task */
static void idle_task(void *param) {
    (void)param;
    while (1) {
        /* Enter low-power mode */
        __asm__ volatile("wfi");  /* Wait for interrupt */
    }
}

/**
 * Initialize the operating system
 */
void os_init(void) {
    memset(&kernel, 0, sizeof(kernel));

    /* Create idle task */
    os_task_create(
        &kernel.task_pool[0],
        "idle",
        idle_task,
        NULL,
        PRIORITY_IDLE
    );

    kernel.task_count = 1;
}

/**
 * Find highest priority ready task
 */
static tcb_t *scheduler_get_next_task(void) {
    for (int i = 0; i < 256; i++) {
        if (kernel.ready_queue[i] != NULL) {
            tcb_t *task = kernel.ready_queue[i];
            /* Remove from ready queue */
            kernel.ready_queue[i] = task->next;
            task->next = NULL;
            return task;
        }
    }
    return &kernel.task_pool[0];  /* Return idle task */
}

/**
 * Add task to ready queue
 */
static void scheduler_add_ready_task(tcb_t *task) {
    if (task->state != TASK_STATE_READY) {
        task->state = TASK_STATE_READY;
    }

    /* Insert at end of priority queue */
    if (kernel.ready_queue[task->priority] == NULL) {
        kernel.ready_queue[task->priority] = task;
    } else {
        tcb_t *current = kernel.ready_queue[task->priority];
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = task;
    }
    task->next = NULL;
}

/**
 * Context switch (to be implemented in assembly for target architecture)
 */
extern void context_switch(uint32_t **old_sp, uint32_t **new_sp);

/**
 * Scheduler - called by timer interrupt
 */
void os_scheduler(void) {
    if (!kernel.scheduler_running) {
        return;
    }

    kernel.tick_count++;

    /* Update current task time slice */
    if (kernel.current_task != NULL) {
        kernel.current_task->run_time++;
        kernel.current_task->time_slice--;

        /* Preempt if time slice expired */
        if (kernel.current_task->time_slice == 0) {
            if (kernel.current_task->state == TASK_STATE_RUNNING) {
                scheduler_add_ready_task(kernel.current_task);
            }

            /* Get next task */
            tcb_t *next_task = scheduler_get_next_task();

            if (next_task != kernel.current_task) {
                kernel.context_switch_count++;
                tcb_t *old_task = kernel.current_task;
                kernel.current_task = next_task;
                next_task->state = TASK_STATE_RUNNING;
                next_task->time_slice = TIME_SLICE_MS;

                /* Perform context switch */
                context_switch(&old_task->stack_ptr, &next_task->stack_ptr);
            }
        }
    }
}

/**
 * Start the OS scheduler
 */
void os_start(void) {
    kernel.scheduler_running = true;

    /* Get first task */
    kernel.current_task = scheduler_get_next_task();
    kernel.current_task->state = TASK_STATE_RUNNING;

    /* Start timer for tick */
    /* TODO: Platform-specific timer initialization */

    /* Jump to first task */
    __asm__ volatile(
        "mov sp, %0\n"
        "bx lr\n"
        : : "r"(kernel.current_task->stack_ptr)
    );

    /* Should never reach here */
    while (1);
}

/**
 * Create a new task
 */
os_error_t os_task_create(
    tcb_t *task,
    const char *name,
    void (*entry)(void *),
    void *param,
    task_priority_t priority
) {
    if (task == NULL || entry == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    if (kernel.task_count >= MAX_TASKS) {
        return OS_ERROR_NO_MEMORY;
    }

    /* Initialize task control block */
    memset(task, 0, sizeof(tcb_t));
    strncpy(task->name, name, sizeof(task->name) - 1);
    task->entry_point = entry;
    task->param = param;
    task->priority = priority;
    task->base_priority = priority;  /* Store base priority */
    task->state = TASK_STATE_READY;
    task->time_slice = TIME_SLICE_MS;

    /* Initialize stack (grows downward) */
    uint32_t *stack_top = &task->stack[STACK_SIZE - 1];

    /* Simulate initial stack frame for context switch */
    *(--stack_top) = 0x01000000;  /* xPSR - Thumb mode */
    *(--stack_top) = (uint32_t)entry;  /* PC */
    *(--stack_top) = 0;  /* LR */
    *(--stack_top) = 0;  /* R12 */
    *(--stack_top) = 0;  /* R3 */
    *(--stack_top) = 0;  /* R2 */
    *(--stack_top) = 0;  /* R1 */
    *(--stack_top) = (uint32_t)param;  /* R0 - parameter */

    task->stack_ptr = stack_top;

    /* Add to ready queue */
    scheduler_add_ready_task(task);
    kernel.task_count++;

    return OS_OK;
}

/**
 * Delete a task
 */
os_error_t os_task_delete(tcb_t *task) {
    if (task == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    task->state = TASK_STATE_TERMINATED;
    kernel.task_count--;

    /* If deleting current task, trigger scheduler */
    if (task == kernel.current_task) {
        os_task_yield();
    }

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Suspend a task
 */
os_error_t os_task_suspend(tcb_t *task) {
    if (task == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();
    task->state = TASK_STATE_SUSPENDED;
    os_exit_critical(state);

    return OS_OK;
}

/**
 * Resume a task
 */
os_error_t os_task_resume(tcb_t *task) {
    if (task == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();
    if (task->state == TASK_STATE_SUSPENDED) {
        scheduler_add_ready_task(task);
    }
    os_exit_critical(state);

    return OS_OK;
}

/**
 * Yield CPU to other tasks
 */
void os_task_yield(void) {
    /* Trigger scheduler */
    kernel.current_task->time_slice = 0;
    os_scheduler();
}

/**
 * Delay task execution
 */
void os_task_delay(uint32_t ticks) {
    uint32_t target = kernel.tick_count + ticks;
    while (kernel.tick_count < target) {
        os_task_yield();
    }
}

/**
 * Get current task
 */
tcb_t *os_task_get_current(void) {
    return kernel.current_task;
}

/**
 * Get system tick count
 */
uint32_t os_get_tick_count(void) {
    return kernel.tick_count;
}

/**
 * Get uptime in milliseconds
 */
uint32_t os_get_uptime_ms(void) {
    return (kernel.tick_count * 1000) / TICK_RATE_HZ;
}

/**
 * Critical section management
 */
uint32_t os_enter_critical(void) {
    uint32_t primask;
    __asm__ volatile(
        "mrs %0, primask\n"
        "cpsid i\n"
        : "=r"(primask)
    );
    return primask;
}

void os_exit_critical(uint32_t state) {
    __asm__ volatile(
        "msr primask, %0\n"
        : : "r"(state)
    );
}

/**
 * Get OS statistics
 */
void os_get_stats(os_stats_t *stats) {
    if (stats == NULL) return;

    uint32_t running = 0, blocked = 0;
    for (int i = 0; i < kernel.task_count; i++) {
        if (kernel.task_pool[i].state == TASK_STATE_RUNNING) running++;
        if (kernel.task_pool[i].state == TASK_STATE_BLOCKED) blocked++;
    }

    stats->total_tasks = kernel.task_count;
    stats->running_tasks = running;
    stats->blocked_tasks = blocked;
    stats->context_switches = kernel.context_switch_count;
    stats->uptime_ticks = kernel.tick_count;
    stats->free_memory = os_get_free_memory();
    stats->used_memory = 0;  /* To be implemented */
}

/**
 * Get task CPU usage
 */
uint8_t os_task_get_cpu_usage(tcb_t *task) {
    if (task == NULL || kernel.tick_count == 0) {
        return 0;
    }

    return (uint8_t)((task->run_time * 100) / kernel.tick_count);
}

/**
 * Remove task from ready queue
 */
static void scheduler_remove_task(tcb_t *task) {
    /* Search through all priority queues */
    for (int i = 0; i < 256; i++) {
        tcb_t *prev = NULL;
        tcb_t *current = kernel.ready_queue[i];

        while (current != NULL) {
            if (current == task) {
                /* Found the task, remove it */
                if (prev == NULL) {
                    kernel.ready_queue[i] = current->next;
                } else {
                    prev->next = current->next;
                }
                current->next = NULL;
                return;
            }
            prev = current;
            current = current->next;
        }
    }
}

/**
 * Get task priority
 */
task_priority_t os_task_get_priority(tcb_t *task) {
    if (task == NULL) {
        return PRIORITY_IDLE;
    }
    return task->priority;
}

/**
 * Set task priority (dynamic priority adjustment)
 * This changes both current and base priority
 */
os_error_t os_task_set_priority(tcb_t *task, task_priority_t new_priority) {
    if (task == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    task_priority_t old_priority = task->priority;
    task->priority = new_priority;
    task->base_priority = new_priority;  /* Update base priority too */

    /* If task is in ready queue, re-insert at new priority */
    if (task->state == TASK_STATE_READY) {
        scheduler_remove_task(task);
        scheduler_add_ready_task(task);
    }

    /* If this is the current task and priority decreased, yield */
    if (task == kernel.current_task && new_priority > old_priority) {
        os_exit_critical(state);
        os_task_yield();
        return OS_OK;
    }

    /* If a higher priority task is now ready, trigger scheduler */
    if (new_priority < kernel.current_task->priority) {
        os_exit_critical(state);
        os_task_yield();
        return OS_OK;
    }

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Raise task priority temporarily
 * Used for priority inheritance - priority will return to base when reset
 */
os_error_t os_task_raise_priority(tcb_t *task, task_priority_t new_priority) {
    if (task == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    /* Only raise priority, don't lower it */
    if (new_priority >= task->priority) {
        return OS_OK;  /* No change needed */
    }

    uint32_t state = os_enter_critical();

    task_priority_t old_priority = task->priority;
    task->priority = new_priority;
    /* Note: base_priority remains unchanged */

    /* If task is in ready queue, re-insert at new priority */
    if (task->state == TASK_STATE_READY) {
        scheduler_remove_task(task);
        scheduler_add_ready_task(task);
    }

    /* If a higher priority task is now ready, trigger scheduler */
    if (new_priority < kernel.current_task->priority) {
        os_exit_critical(state);
        os_task_yield();
        return OS_OK;
    }

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Reset task to base priority
 * Used to release priority inheritance
 */
os_error_t os_task_reset_priority(tcb_t *task) {
    if (task == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Reset to base priority */
    task_priority_t old_priority = task->priority;
    task->priority = task->base_priority;

    /* If task is in ready queue, re-insert at base priority */
    if (task->state == TASK_STATE_READY) {
        scheduler_remove_task(task);
        scheduler_add_ready_task(task);
    }

    /* If this is current task and priority decreased, yield */
    if (task == kernel.current_task && task->priority > old_priority) {
        os_exit_critical(state);
        os_task_yield();
        return OS_OK;
    }

    os_exit_critical(state);
    return OS_OK;
}
