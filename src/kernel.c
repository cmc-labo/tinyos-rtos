/**
 * TinyOS Kernel - Core Implementation
 *
 * Implements the real-time scheduler and task management
 */

#include "tinyos.h"
#include "tinyos/trace.h"
#include <string.h>

/*===========================================================================
 * SysTick registers (ARM Cortex-M, all variants)
 *===========================================================================*/
#define SYST_CSR  (*(volatile uint32_t *)0xE000E010U) /* Control & Status  */
#define SYST_RVR  (*(volatile uint32_t *)0xE000E014U) /* Reload Value      */
#define SYST_CVR  (*(volatile uint32_t *)0xE000E018U) /* Current Value     */

#define SYST_CSR_ENABLE    (1UL << 0)  /* Counter enable                   */
#define SYST_CSR_TICKINT   (1UL << 1)  /* SysTick exception request enable */
#define SYST_CSR_CLKSOURCE (1UL << 2)  /* 1 = processor clock              */

/* Default core clock used for SysTick period calculation.
 * Override by defining SYSTEM_CORE_CLOCK before including tinyos.h,
 * or pass -DSYSTEM_CORE_CLOCK=<Hz> on the compiler command line.    */
#ifndef SYSTEM_CORE_CLOCK
#define SYSTEM_CORE_CLOCK  168000000UL   /* 168 MHz — STM32F4 default */
#endif

/* Assembly entry points declared in context_switch.s */
extern void os_start_first_task(uint32_t *sp);
extern void os_pend_sv(void);

/*===========================================================================
 * SCB system handler priority registers (for PendSV / SysTick)
 *===========================================================================*/
#define SCB_SHPR3  (*(volatile uint32_t *)0xE000ED20U)
/* SHPR3 bits [23:16] = PendSV priority, bits [31:24] = SysTick priority */
#define PENDSV_PRIO_LOWEST  (0xFFUL << 16)  /* PendSV  = 255 (lowest)    */
#define SYSTICK_PRIO_HIGH   (0xC0UL << 24)  /* SysTick = 192             */

/* Global kernel state */
static struct {
    tcb_t *current_task;
    tcb_t *ready_queue[256];      /* Priority-based ready queue — head pointers */
    tcb_t *ready_queue_tail[256]; /* Tail pointers for O(1) enqueue             */
    uint32_t ready_bitmap[8];     /* Bitmap: bit set = that priority has ready task */
    tcb_t task_pool[MAX_TASKS];
    uint8_t task_count;
    volatile uint32_t tick_count;
    volatile uint32_t context_switch_count;
    bool scheduler_running;
} kernel;

/* Forward declarations */
static void scheduler_remove_task(tcb_t *task);
static void scheduler_enqueue(tcb_t *task);
static void scheduler_add_ready_task(tcb_t *task);

/* Idle task */
static void idle_task(void *param) {
    (void)param;
    while (1) {
        /* Enter low-power mode using power management */
        os_power_enter_idle();
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
}

/**
 * Find highest priority ready task
 */
static tcb_t *scheduler_get_next_task(void) {
    /* Use bitmap to find highest priority (lowest index) in O(1) */
    for (int w = 0; w < 8; w++) {
        if (kernel.ready_bitmap[w] != 0) {
            int bit = __builtin_ctz(kernel.ready_bitmap[w]);
            int i = w * 32 + bit;
            tcb_t *task = kernel.ready_queue[i];
            kernel.ready_queue[i] = task->next;
            task->next = NULL;
            if (kernel.ready_queue[i] == NULL) {
                /* Queue for this priority is now empty — clear both pointers. */
                kernel.ready_bitmap[w]    &= ~(1u << bit);
                kernel.ready_queue_tail[i] = NULL;
            }
            return task;
        }
    }
    return &kernel.task_pool[0];  /* Return idle task */
}

/**
 * scheduler_enqueue — internal: add task to ready queue without preemption check.
 * Called from both scheduler_add_ready_task() and os_pendsv_switch() (the latter
 * must not trigger another PendSV while already inside PendSV_Handler).
 */
static void scheduler_enqueue(tcb_t *task) {
    task->state = TASK_STATE_READY;
    task->next  = NULL;

    uint8_t prio = task->priority;

    /* O(1) tail insertion */
    if (kernel.ready_queue[prio] == NULL) {
        kernel.ready_queue[prio] = task;
    } else {
        kernel.ready_queue_tail[prio]->next = task;
    }
    kernel.ready_queue_tail[prio] = task;

    /* Mark bitmap */
    kernel.ready_bitmap[prio / 32] |= (1u << (prio % 32));
}

/**
 * scheduler_add_ready_task — public-internal: enqueue + immediate preemption check.
 *
 * If the newly-ready task has strictly higher priority (lower numeric value)
 * than the currently running task, PendSV is pended so the switch happens
 * as soon as the current exception (if any) exits.
 */
static void scheduler_add_ready_task(tcb_t *task) {
    scheduler_enqueue(task);

    /* Preemption check: trigger PendSV if the new task outranks the current one. */
    if (kernel.scheduler_running &&
        kernel.current_task != NULL &&
        task->priority < kernel.current_task->priority) {
        os_pend_sv();
    }
}

/**
 * Move a task to a new priority level within the ready queue.
 * Must be called from within a critical section.
 * Uses scheduler_enqueue to avoid double preemption trigger.
 */
static void scheduler_reprioritize(tcb_t *task, task_priority_t new_priority) {
    if (task->state == TASK_STATE_READY) scheduler_remove_task(task);
    task->priority = new_priority;
    if (task->state == TASK_STATE_READY) scheduler_enqueue(task);
}

/**
 * os_pendsv_switch — C-level context switch body, called from PendSV_Handler.
 *
 * @param old_psp  PSP of the outgoing task AFTER R4-R11 have been pushed
 *                 (PendSV_Handler already saved them before calling here).
 * @return         stack_ptr of the incoming task, pointing at its saved R4-R11
 *                 (PendSV_Handler will restore R4-R11 and set PSP from this).
 *
 * Interrupts are disabled by PendSV_Handler (CPSID I) for the duration.
 */
uint32_t *os_pendsv_switch(uint32_t *old_psp) {
    tcb_t *old_task = kernel.current_task;

    /* Commit the outgoing task's PSP (R4-R11 already pushed by PendSV_Handler) */
    old_task->stack_ptr = old_psp;

    /* Stack guard check */
    if (old_task->stack[0] != STACK_GUARD_MAGIC) {
        os_stack_overflow_hook(old_task);
    }

    /* If the outgoing task is still runnable (preempted, not blocked/terminated),
     * put it back at the tail of its priority queue for round-robin within level. */
    if (old_task->state == TASK_STATE_RUNNING) {
        scheduler_enqueue(old_task);    /* enqueue without re-triggering PendSV */
    }

    /* Select the next task (highest priority, FIFO within priority) */
    tcb_t *next_task = scheduler_get_next_task();

    /* Record the switch in the trace log */
    trace_record_switch(old_task->name, next_task->name);

    /* Update counters and state */
    kernel.context_switch_count++;
    next_task->context_switches++;
    kernel.current_task = next_task;
    next_task->state    = TASK_STATE_RUNNING;
    next_task->time_slice = TIME_SLICE_MS;

    /* Return new task's stack_ptr; PendSV_Handler restores R4-R11 from there */
    return next_task->stack_ptr;
}

/**
 * os_scheduler — SysTick ISR body.
 *
 * Responsibilities (time-keeping only; actual switch is done by PendSV):
 *   1. Increment tick counter.
 *   2. Charge one tick to the current task's run time.
 *   3. Decrement time slice; pend PendSV when it reaches zero.
 *
 * Preemption triggered by newly-ready tasks (e.g. from semaphore_post) is
 * handled separately inside scheduler_add_ready_task() → os_pend_sv().
 */
void os_scheduler(void) {
    if (!kernel.scheduler_running) {
        return;
    }

    kernel.tick_count++;

    if (kernel.current_task != NULL) {
        kernel.current_task->run_time++;

        if (kernel.current_task->time_slice > 0) {
            kernel.current_task->time_slice--;
        }

        /* Time slice exhausted — request a context switch via PendSV.
         * PendSV fires after SysTick returns (it has lower priority). */
        if (kernel.current_task->time_slice == 0) {
            os_pend_sv();
        }
    }
}

/**
 * SysTick exception handler — called every 1 ms (TICK_RATE_HZ = 1000).
 * Declared here so the linker places it in the vector table without
 * requiring a separate startup file entry.
 */
void SysTick_Handler(void) {
    os_scheduler();
}

/**
 * Start the OS scheduler.
 *
 * 1. Initialises SysTick to fire at TICK_RATE_HZ using SYSTEM_CORE_CLOCK.
 * 2. Selects the highest-priority ready task.
 * 3. Calls os_start_first_task() (assembly) which:
 *      a. Triggers SVC #0 → SVC_Handler
 *      b. SVC_Handler loads PSP, switches Thread mode to PSP,
 *         and does EXC_RETURN to launch the task.
 *
 * Never returns.
 */
void os_start(void) {
    kernel.scheduler_running = true;

    /* Select the first task to run */
    kernel.current_task = scheduler_get_next_task();
    kernel.current_task->state = TASK_STATE_RUNNING;

    /* Set exception priorities (must be done before enabling SysTick).
     *   PendSV  = 0xFF (255, lowest possible) — runs after all other ISRs.
     *   SysTick = 0xC0 (192) — higher than PendSV so tick counting is
     *             not delayed by a context switch in progress.
     * Lower numeric value = higher priority on ARM Cortex-M.             */
    SCB_SHPR3 = PENDSV_PRIO_LOWEST | SYSTICK_PRIO_HIGH;

    /* Configure SysTick:
     *   reload = (core_clock / tick_rate) - 1
     *   e.g.  168 000 000 / 1000 - 1 = 167 999                          */
    SYST_RVR = (SYSTEM_CORE_CLOCK / TICK_RATE_HZ) - 1UL;
    SYST_CVR = 0UL;                              /* clear current value   */
    SYST_CSR = SYST_CSR_CLKSOURCE               /* processor clock        */
             | SYST_CSR_TICKINT                  /* enable SysTick IRQ     */
             | SYST_CSR_ENABLE;                  /* start counter          */

    /* Hand off to the first task via SVC (assembly trampoline).
     * This call never returns — EXC_RETURN in SVC_Handler launches
     * the task directly in Thread/PSP mode.                              */
    os_start_first_task(kernel.current_task->stack_ptr);

    /* Unreachable */
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
    strncpy(task->name, name ? name : "", sizeof(task->name) - 1);
    task->entry_point = entry;
    task->param = param;
    task->priority = priority;
    task->base_priority = priority;  /* Store base priority */
    task->state = TASK_STATE_READY;
    task->time_slice = TIME_SLICE_MS;

    /* Plant stack guard at the bottom (lowest address) of the stack.
     * The stack grows downward, so stack[0] is the last word to be
     * overwritten when the stack overflows. */
    task->stack[0] = STACK_GUARD_MAGIC;

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

    /* Idle task must never be deleted */
    if (task == &kernel.task_pool[0]) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Remove from ready queue before changing state */
    scheduler_remove_task(task);
    task->state = TASK_STATE_TERMINATED;
    kernel.task_count--;

    /* If deleting current task, trigger scheduler */
    if (task == kernel.current_task) {
        os_exit_critical(state);
        os_task_yield();
        return OS_OK;
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

    /* Remove from ready queue before suspending */
    scheduler_remove_task(task);
    task->state = TASK_STATE_SUSPENDED;
    bool is_current = (task == kernel.current_task);

    os_exit_critical(state);

    /* If suspending current task, yield to next task */
    if (is_current) {
        os_task_yield();
    }

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
 * Yield CPU to other tasks.
 *
 * Pends PendSV so the context switch happens cleanly after any active ISR
 * completes.  The current task remains RUNNING; os_pendsv_switch() will
 * re-enqueue it at the tail of its priority level (fair round-robin).
 */
void os_task_yield(void) {
    if (kernel.current_task == NULL) {
        return;
    }
    os_pend_sv();
}

/**
 * Delay task execution
 */
void os_task_delay(uint32_t ticks) {
    uint32_t target = kernel.tick_count + ticks;
    /* Use subtraction to handle tick_count wraparound correctly */
    while ((int32_t)(target - kernel.tick_count) > 0) {
        os_task_yield();
    }
}

/**
 * Delay task execution in milliseconds
 * Converts ms to ticks using TICK_RATE_HZ to stay portable
 * regardless of the configured tick rate.
 */
void os_task_delay_ms(uint32_t ms) {
    os_task_delay((ms * TICK_RATE_HZ + 999) / 1000);
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
    return (uint32_t)(((uint64_t)kernel.tick_count * 1000) / TICK_RATE_HZ);
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
    for (int i = 0; i < MAX_TASKS; i++) {
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

    uint32_t usage = (uint32_t)(((uint64_t)task->run_time * 100) / kernel.tick_count);
    return (uint8_t)(usage > 100u ? 100u : usage);
}

/**
 * Remove task from ready queue
 */
static void scheduler_remove_task(tcb_t *task) {
    /* A task can only reside in the queue for its own priority. */
    uint8_t prio  = task->priority;
    tcb_t *prev    = NULL;
    tcb_t *current = kernel.ready_queue[prio];

    while (current != NULL) {
        if (current == task) {
            /* Unlink from the list. */
            if (prev == NULL) {
                kernel.ready_queue[prio] = current->next;
            } else {
                prev->next = current->next;
            }

            /* Maintain tail pointer when the removed node was the tail. */
            if (kernel.ready_queue_tail[prio] == task) {
                kernel.ready_queue_tail[prio] = prev;   /* NULL when queue is now empty */
            }

            current->next = NULL;

            if (kernel.ready_queue[prio] == NULL) {
                kernel.ready_bitmap[prio / 32] &= ~(1u << (prio % 32));
            }
            return;
        }
        prev    = current;
        current = current->next;
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

    /* No-op if priority is unchanged */
    if (new_priority == old_priority) {
        os_exit_critical(state);
        return OS_OK;
    }

    /* Remove from ready queue before changing priority to ensure correct queue is searched */
    scheduler_reprioritize(task, new_priority);
    task->base_priority = new_priority;  /* Update base priority too */

    /* If this is the current task and priority decreased, yield */
    if (task == kernel.current_task && new_priority > old_priority) {
        os_exit_critical(state);
        os_task_yield();
        return OS_OK;
    }

    /* If a higher priority task is now ready, trigger scheduler */
    if (kernel.current_task != NULL && new_priority < kernel.current_task->priority) {
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

    /* Remove from ready queue before changing priority to ensure correct queue is searched */
    scheduler_reprioritize(task, new_priority);
    /* Note: base_priority remains unchanged */

    /* If a higher priority task is now ready, trigger scheduler */
    if (kernel.current_task != NULL && new_priority < kernel.current_task->priority) {
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

    scheduler_reprioritize(task, task->base_priority);

    /* If this is current task and priority decreased, yield */
    if (task == kernel.current_task && task->priority > old_priority) {
        os_exit_critical(state);
        os_task_yield();
        return OS_OK;
    }

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Statistics API Implementation
 */

/**
 * Convert a tick count to a percentage of total system ticks (CPU usage)
 */
static inline float ticks_to_percent(uint32_t ticks) {
    return kernel.tick_count > 0
           ? ((float)ticks / (float)kernel.tick_count) * 100.0f
           : 0.0f;
}

/**
 * Calculate stack usage for a task
 */
static uint32_t calculate_stack_usage(tcb_t *task) {
    if (task == NULL) {
        return 0;
    }

    /* Find lowest used stack address by looking for non-zero values.
     * Stack grows downward, so we search upward from stack[1].
     * stack[0] holds the guard magic and must be excluded from usage
     * accounting so it is never misidentified as "used" stack space. */
    uint32_t *stack_bottom = &task->stack[1];   /* skip guard word at [0] */
    uint32_t *stack_top = &task->stack[STACK_SIZE - 1];
    uint32_t *current = stack_bottom;

    /* Skip zeros at bottom (unused stack) */
    while (current < stack_top && *current == 0) {
        current++;
    }

    /* Calculate used bytes */
    uint32_t used_words = stack_top - current + 1;
    return used_words * sizeof(uint32_t);
}

/**
 * Get task statistics
 */
os_error_t os_task_get_stats(tcb_t *task, task_stats_t *stats) {
    if (task == NULL || stats == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Copy basic info */
    strncpy(stats->name, task->name, sizeof(stats->name) - 1);
    stats->name[sizeof(stats->name) - 1] = '\0';
    stats->state = task->state;
    stats->priority = task->priority;
    stats->run_time = task->run_time;
    stats->context_switches = task->context_switches;

    /* Calculate stack usage */
    stats->stack_size = STACK_SIZE * sizeof(uint32_t);
    stats->stack_used = calculate_stack_usage(task);
    stats->stack_free = (stats->stack_used <= stats->stack_size)
                      ? (stats->stack_size - stats->stack_used)
                      : 0;

    /* Update high water mark */
    if (stats->stack_used > task->stack_high_water_mark) {
        task->stack_high_water_mark = stats->stack_used;
    }

    /* Calculate CPU usage percentage */
    stats->cpu_usage = ticks_to_percent(task->run_time);

    /* Stack guard status */
    stats->stack_guard_ok = (task->stack[0] == STACK_GUARD_MAGIC);

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Get system statistics
 */
os_error_t os_get_system_stats(system_stats_t *stats) {
    if (stats == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Count running tasks */
    uint32_t running = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if (kernel.task_pool[i].state == TASK_STATE_READY ||
            kernel.task_pool[i].state == TASK_STATE_RUNNING) {
            running++;
        }
    }

    stats->total_tasks = kernel.task_count;
    stats->running_tasks = running;
    stats->total_context_switches = kernel.context_switch_count;
    stats->uptime_ticks = kernel.tick_count;
    stats->uptime_seconds = kernel.tick_count / TICK_RATE_HZ;

    /* Calculate idle time (from idle task) */
    if (kernel.task_count > 0) {
        stats->idle_time = kernel.task_pool[0].run_time;  /* Idle task is always first */
    } else {
        stats->idle_time = 0;
    }

    /* Calculate overall CPU usage */
    stats->cpu_usage = 100.0f - ticks_to_percent(stats->idle_time);

    stats->free_heap = os_get_free_memory();

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Reset task statistics
 */
os_error_t os_task_reset_stats(tcb_t *task) {
    if (task == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    task->run_time = 0;
    task->context_switches = 0;
    task->stack_high_water_mark = 0;

    os_exit_critical(state);
    return OS_OK;
}

/**
 * Print task statistics (for debugging)
 */
void os_print_task_stats(tcb_t *task) {
    if (task == NULL) {
        return;
    }

    task_stats_t stats;
    if (os_task_get_stats(task, &stats) != OS_OK) {
        return;
    }

    const char *state_str[] = {"READY", "RUNNING", "BLOCKED", "SUSPENDED", "TERMINATED"};

    /* Print task info */
    /* Note: In embedded systems without printf, this would use UART output */
    /* For now, we'll leave the implementation as a stub */
    (void)state_str;  /* Suppress unused warning */
}

/**
 * Print all tasks statistics
 */
void os_print_all_stats(void) {
    system_stats_t sys_stats;

    if (os_get_system_stats(&sys_stats) != OS_OK) {
        return;
    }

    /* Print system stats */
    /* Note: printf implementation would go here */

    /* Print each task */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (kernel.task_pool[i].state != TASK_STATE_TERMINATED) {
            os_print_task_stats(&kernel.task_pool[i]);
        }
    }
}

/**
 * Debug Helper Functions
 */

/**
 * Convert error code to human-readable string
 */
const char *os_error_to_string(os_error_t error) {
    switch (error) {
        case OS_OK:
            return "Success";
        case OS_ERROR:
            return "Error";
        case OS_ERROR_NO_MEMORY:
            return "Out of memory";
        case OS_ERROR_INVALID_PARAM:
            return "Invalid parameter";
        case OS_ERROR_TIMEOUT:
            return "Timeout";
        case OS_ERROR_PERMISSION_DENIED:
            return "Permission denied";
        case OS_ERROR_NOT_IMPLEMENTED:
            return "Not implemented";
        case OS_ERROR_NO_RESOURCE:
            return "No resource";
        case OS_ERROR_NOT_INITIALIZED:
            return "Not initialized";
        default:
            return "Unknown error";
    }
}

/**
 * Convert task state to human-readable string
 */
const char *os_task_state_to_string(task_state_t state) {
    switch (state) {
        case TASK_STATE_READY:
            return "Ready";
        case TASK_STATE_RUNNING:
            return "Running";
        case TASK_STATE_BLOCKED:
            return "Blocked";
        case TASK_STATE_SUSPENDED:
            return "Suspended";
        case TASK_STATE_TERMINATED:
            return "Terminated";
        default:
            return "Unknown";
    }
}

/**
 * Get priority level name
 */
const char *os_priority_name(task_priority_t priority) {
    if (priority == PRIORITY_CRITICAL) {
        return "Critical";
    } else if (priority <= PRIORITY_HIGH) {
        return "High";
    } else if (priority <= PRIORITY_NORMAL) {
        return "Normal";
    } else if (priority <= PRIORITY_LOW) {
        return "Low";
    } else if (priority == PRIORITY_IDLE) {
        return "Idle";
    } else {
        return "Custom";
    }
}

/* Helper macros for compile-time version string generation */
#define TINYOS_STRINGIFY(x)  #x
#define TINYOS_TOSTRING(x)   TINYOS_STRINGIFY(x)
#define TINYOS_VERSION_STRING \
    TINYOS_TOSTRING(TINYOS_VERSION_MAJOR) "." \
    TINYOS_TOSTRING(TINYOS_VERSION_MINOR) "." \
    TINYOS_TOSTRING(TINYOS_VERSION_PATCH)

/**
 * Get version string
 */
const char *os_get_version_string(void) {
    return TINYOS_VERSION_STRING;
}

/**
 * Check if scheduler is running
 */
bool os_is_running(void) {
    return kernel.scheduler_running;
}

/*===========================================================================
 * Stack Guard
 *===========================================================================*/

/**
 * Check whether a task's stack guard word is intact.
 */
bool os_task_stack_is_healthy(const tcb_t *task) {
    if (task == NULL) return false;
    return task->stack[0] == STACK_GUARD_MAGIC;
}

/**
 * Default overflow hook — halts with a breakpoint.
 * Declare your own (non-weak) os_stack_overflow_hook() to override.
 */
__attribute__((weak))
void os_stack_overflow_hook(tcb_t *task) {
    (void)task;
    /* On Cortex-M: trigger a debug breakpoint so a debugger can inspect
     * which task overflowed.  On hardware without a debugger attached this
     * escalates to a HardFault, which is intentional — a stack overflow is
     * a fatal error.  Replace this with a system reset or logging call as
     * needed for your application. */
    __asm__ volatile("bkpt #1");
    while (1);   /* should not be reached */
}
