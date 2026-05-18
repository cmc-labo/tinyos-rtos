/**
 * TinyOS Kernel - Core Implementation
 *
 * Implements the real-time scheduler and task management
 */

#include "tinyos.h"
#include "tinyos/trace.h"
#include "hal/hal.h"
#include <string.h>

/* Default core clock — override via -DSYSTEM_CORE_CLOCK=<Hz> */
#ifndef SYSTEM_CORE_CLOCK
#define SYSTEM_CORE_CLOCK  168000000UL   /* 168 MHz — STM32F4 default */
#endif

/* Assembly entry points declared in context_switch.s */
extern void os_start_first_task(uint32_t *sp);
extern void os_pend_sv(void);


/* Global kernel state */
static struct {
    tcb_t *current_task;
    tcb_t *ready_queue[256];      /* Priority-based ready queue — head pointers */
    tcb_t *ready_queue_tail[256]; /* Tail pointers for O(1) enqueue             */
    uint32_t ready_bitmap[8];     /* Bitmap: bit set = that priority has ready task */
    tcb_t *delay_queue;           /* Singly-linked list sorted by wake_tick ASC  */
    tcb_t task_pool[MAX_TASKS];
    tcb_t *task_registry[MAX_TASKS]; /* Flat list of all live task pointers */
    uint8_t task_count;
    volatile uint32_t tick_count;
    volatile uint32_t context_switch_count;
    bool scheduler_running;
} kernel;

/* Forward declarations */
static void scheduler_remove_task(tcb_t *task);
static void scheduler_enqueue(tcb_t *task);
static void scheduler_add_ready_task(tcb_t *task);
static void delay_queue_insert(tcb_t *task);
static void delay_queue_remove(tcb_t *task);
static void delay_queue_tick(void);

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

    /* Memory must be initialized before any subsystem that may allocate */
    os_mem_init();

    /* Power management must be initialized before the idle task runs */
    os_power_init();

    /* Initialize software timer subsystem */
    os_timer_init();

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

    /* Stack guard check — verify all guard words */
    for (uint32_t _gi = 0; _gi < STACK_GUARD_WORDS; _gi++) {
        if (old_task->stack[_gi] != STACK_GUARD_MAGIC) {
            os_stack_overflow_hook(old_task);
            break;
        }
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

    /* Wake any tasks whose delay has expired. */
    delay_queue_tick();

    /* Fire any software timers that have expired. */
    os_timer_process();

    if (kernel.current_task != NULL) {
        kernel.current_task->run_time++;

        /* Periodic stack guard check — catch overflows as early as possible,
         * not only on context switch.  Hooks bkpt/halt on detection. */
        for (uint32_t _gi = 0; _gi < STACK_GUARD_WORDS; _gi++) {
            if (kernel.current_task->stack[_gi] != STACK_GUARD_MAGIC) {
                os_stack_overflow_hook(kernel.current_task);
                break;
            }
        }

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

    /* Initialise the HAL and start the periodic tick source. */
    hal_init();
    hal_tick_init(SYSTEM_CORE_CLOCK, TICK_RATE_HZ);

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

    /* Poison-fill the entire stack so high-water-mark scanning is accurate
     * and so uninitialized stack reads are easily spotted in a debugger. */
    for (uint32_t i = 0; i < STACK_SIZE; i++) {
        task->stack[i] = STACK_POISON;
    }

    /* Plant STACK_GUARD_WORDS guard words at the bottom (lowest addresses).
     * The stack grows downward, so these are the last words overwritten on
     * overflow — checking all of them greatly reduces false-negative risk. */
    for (uint32_t i = 0; i < STACK_GUARD_WORDS; i++) {
        task->stack[i] = STACK_GUARD_MAGIC;
    }

    /* Initialize stack (grows downward) */
    uint32_t *stack_top = &task->stack[STACK_SIZE - 1];

    /* CPU auto-save frame (popped by EXC_RETURN) — grows downward */
    *(--stack_top) = 0x01000000;      /* xPSR: Thumb bit set */
    *(--stack_top) = (uint32_t)entry; /* PC */
    *(--stack_top) = 0xFFFFFFFD;      /* LR: EXC_RETURN Thread/PSP */
    *(--stack_top) = 0;               /* R12 */
    *(--stack_top) = 0;               /* R3 */
    *(--stack_top) = 0;               /* R2 */
    *(--stack_top) = 0;               /* R1 */
    *(--stack_top) = (uint32_t)param; /* R0 */

    /* PendSV-saved registers (R4-R11), zeroed for first run */
    *(--stack_top) = 0;  /* R11 */
    *(--stack_top) = 0;  /* R10 */
    *(--stack_top) = 0;  /* R9 */
    *(--stack_top) = 0;  /* R8 */
    *(--stack_top) = 0;  /* R7 */
    *(--stack_top) = 0;  /* R6 */
    *(--stack_top) = 0;  /* R5 */
    *(--stack_top) = 0;  /* R4 */

    /* stack_ptr points at R4; PendSV's LDMIA starts here */
    task->stack_ptr = stack_top;

    /* Register in flat task registry for iteration (ps, top, kill commands). */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (kernel.task_registry[i] == NULL) {
            kernel.task_registry[i] = task;
            break;
        }
    }

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

    scheduler_remove_task(task);
    delay_queue_remove(task);

    /* If task was blocked on a mutex with a timed lock, evict it from the
     * mutex wait queue so the owner does not try to wake a dead task. */
    if (task->waiting_on != NULL) {
        mutex_t *m = task->waiting_on;
        task->waiting_on = NULL;
        tcb_t **pp = &m->wait_queue;
        while (*pp != NULL) {
            if (*pp == task) { *pp = task->next; task->next = NULL; break; }
            pp = &(*pp)->next;
        }
        if (m->owner != NULL) {
            task_priority_t p = (m->wait_queue != NULL)
                                ? m->wait_queue->priority
                                : m->owner->base_priority;
            if (p > m->owner->priority) scheduler_reprioritize(m->owner, p);
        }
    }

    task->state = TASK_STATE_TERMINATED;

    /* Remove from registry */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (kernel.task_registry[i] == task) {
            kernel.task_registry[i] = NULL;
            break;
        }
    }

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

    scheduler_remove_task(task);
    delay_queue_remove(task);

    /* Same cleanup as delete: evict from mutex wait queue if applicable. */
    if (task->waiting_on != NULL) {
        mutex_t *m = task->waiting_on;
        task->waiting_on = NULL;
        tcb_t **pp = &m->wait_queue;
        while (*pp != NULL) {
            if (*pp == task) { *pp = task->next; task->next = NULL; break; }
            pp = &(*pp)->next;
        }
        if (m->owner != NULL) {
            task_priority_t p = (m->wait_queue != NULL)
                                ? m->wait_queue->priority
                                : m->owner->base_priority;
            if (p > m->owner->priority) scheduler_reprioritize(m->owner, p);
        }
    }

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
 * Delay task execution — true blocking implementation.
 *
 * Sets the calling task's state to BLOCKED, inserts it into the delay
 * queue sorted by wake_tick, then pends PendSV.  Because the task is
 * BLOCKED, os_pendsv_switch() will NOT re-enqueue it; it stays off the
 * ready queue until delay_queue_tick() wakes it from the SysTick ISR.
 *
 * Zero-tick delay yields once (lets equal-priority peers run) and returns.
 */
void os_task_delay(uint32_t ticks) {
    if (ticks == 0) {
        os_task_yield();
        return;
    }

    uint32_t cs = os_enter_critical();

    tcb_t *task    = kernel.current_task;
    task->wake_tick = kernel.tick_count + ticks;
    task->state     = TASK_STATE_BLOCKED;
    delay_queue_insert(task);

    os_exit_critical(cs);

    /* Hand off the CPU — task will not be re-enqueued until it wakes. */
    os_pend_sv();
}

/**
 * Delay task execution in milliseconds.
 * Converts ms to ticks; rounds up to ensure at least the requested delay.
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
    return hal_irq_save();
}

void os_exit_critical(uint32_t state) {
    hal_irq_restore(state);
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
    size_t free_m, used_m;
    os_get_memory_stats(&free_m, &used_m, NULL, NULL);
    stats->free_memory = free_m;
    stats->used_memory = used_m;
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

/*===========================================================================
 * Delay queue — sorted singly-linked list (ascending wake_tick)
 *
 * A task occupies either the ready queue OR the delay queue, never both.
 * The existing tcb_t::next pointer is reused since membership is exclusive.
 *===========================================================================*/

/**
 * Insert a task into the delay queue in ascending wake_tick order.
 * Uses delay_next so that a task can simultaneously occupy a mutex/sem wait
 * queue (via next) and the delay queue (via delay_next).
 * Must be called inside a critical section.
 */
static void delay_queue_insert(tcb_t *task) {
    tcb_t **pp = &kernel.delay_queue;
    while (*pp != NULL &&
           (int32_t)((*pp)->wake_tick - task->wake_tick) <= 0) {
        pp = &(*pp)->delay_next;
    }
    task->delay_next = *pp;
    *pp = task;
}

/**
 * Remove a specific task from the delay queue (e.g. on delete/suspend/acquire).
 * No-op if the task is not currently in the delay queue.
 * Must be called inside a critical section.
 */
static void delay_queue_remove(tcb_t *task) {
    tcb_t **pp = &kernel.delay_queue;
    while (*pp != NULL) {
        if (*pp == task) {
            *pp = task->delay_next;
            task->delay_next = NULL;
            return;
        }
        pp = &(*pp)->delay_next;
    }
}

/**
 * Wake up all tasks whose wake_tick has been reached.
 * Called from os_scheduler() (SysTick ISR) — must be ISR-safe.
 *
 * If the expiring task was blocked on a mutex (waiting_on != NULL), this is a
 * timed-lock timeout: remove it from the mutex wait queue and recalculate PIP
 * for the mutex owner before making the task runnable.
 */
static void delay_queue_tick(void) {
    while (kernel.delay_queue != NULL &&
           (int32_t)(kernel.tick_count - kernel.delay_queue->wake_tick) >= 0) {
        tcb_t *task = kernel.delay_queue;
        kernel.delay_queue = task->delay_next;
        task->delay_next = NULL;

        /* Timed mutex-lock timeout: evict from mutex wait queue and fix PIP. */
        if (task->waiting_on != NULL) {
            mutex_t *m = task->waiting_on;
            task->waiting_on = NULL;

            /* Remove from the mutex's wait queue (linked via next). */
            tcb_t **pp = &m->wait_queue;
            while (*pp != NULL) {
                if (*pp == task) {
                    *pp = task->next;
                    task->next = NULL;
                    break;
                }
                pp = &(*pp)->next;
            }

            /* Recalculate PIP: the owner may have been boosted for this waiter.
             * scheduler_reprioritize is safe from ISR context — it only moves
             * the task within the ready queue and updates the priority value. */
            if (m->owner != NULL) {
                task_priority_t new_prio = (m->wait_queue != NULL)
                    ? m->wait_queue->priority   /* boost to next best waiter */
                    : m->owner->base_priority;  /* no more waiters — restore  */
                if (new_prio > m->owner->priority) {
                    /* Priority decrease is safe: scheduler_reprioritize handles
                     * READY re-enqueue; RUNNING tasks just get the new value. */
                    scheduler_reprioritize(m->owner, new_prio);
                }
            }
        }

        scheduler_add_ready_task(task);
    }
}

/**
 * Arm a timeout for a task that is about to block on a synchronisation object.
 * The caller must have already set task->wake_tick before calling.
 * Must be called inside a critical section.
 * Called from sync.c for timed mutex/semaphore blocking.
 */
void os_delay_queue_arm(tcb_t *task) {
    delay_queue_insert(task);
}

/**
 * Disarm a previously armed timeout (e.g. the resource was acquired before the
 * deadline, so we no longer need the wakeup).
 * No-op if the task is not in the delay queue.
 * Must be called inside a critical section.
 */
void os_delay_queue_disarm(tcb_t *task) {
    delay_queue_remove(task);
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
 * Unblock a BLOCKED task and place it in the ready queue.
 *
 * Called by synchronisation primitives (mutex unlock, semaphore post, etc.)
 * to wake a task that is waiting for a resource.  Uses scheduler_add_ready_task
 * so preemption is triggered automatically if the woken task outranks the
 * currently running task.
 *
 * Note: Do NOT call this for tasks in the delay queue — they have their own
 * wakeup path via delay_queue_tick().
 */
void os_task_wakeup(tcb_t *task) {
    if (task == NULL) return;
    uint32_t cs = os_enter_critical();
    if (task->state == TASK_STATE_BLOCKED) {
        scheduler_add_ready_task(task);  /* sets state=READY, enqueues, may pend PendSV */
    }
    os_exit_critical(cs);
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

    /* Scan upward from above the guard region, counting untouched poison
     * words.  The first non-poison word is the high-water mark.
     * Stack grows downward, so untouched (low) addresses still hold
     * STACK_POISON; used addresses hold real register values. */
    uint32_t *stack_bottom = &task->stack[STACK_GUARD_WORDS]; /* skip guards */
    uint32_t *stack_top    = &task->stack[STACK_SIZE - 1];
    uint32_t *current      = stack_bottom;

    while (current < stack_top && *current == STACK_POISON) {
        current++;
    }

    uint32_t used_words = (uint32_t)(stack_top - current + 1);
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

    /* Stack guard status — all guard words must be intact */
    stats->stack_guard_ok = os_task_stack_is_healthy(task);

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
 * Get task statistics by zero-based index into the live task registry.
 */
os_error_t os_task_get_stats_by_index(uint32_t index, task_stats_t *stats) {
    if (stats == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t count = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        tcb_t *task = kernel.task_registry[i];
        if (task == NULL || task->state == TASK_STATE_TERMINATED) continue;
        if (count == index) return os_task_get_stats(task, stats);
        count++;
    }
    return OS_ERROR;
}

/**
 * Find a live task by name.  Returns NULL if not found.
 */
tcb_t *os_task_find_by_name(const char *name) {
    if (name == NULL) return NULL;
    for (int i = 0; i < MAX_TASKS; i++) {
        tcb_t *task = kernel.task_registry[i];
        if (task == NULL || task->state == TASK_STATE_TERMINATED) continue;
        if (strcmp(task->name, name) == 0) return task;
    }
    return NULL;
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

    /* Print each task from the registry (covers external TCBs, skips empty slots) */
    for (int i = 0; i < MAX_TASKS; i++) {
        if (kernel.task_registry[i] != NULL) {
            os_print_task_stats(kernel.task_registry[i]);
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
    for (uint32_t i = 0; i < STACK_GUARD_WORDS; i++) {
        if (task->stack[i] != STACK_GUARD_MAGIC) return false;
    }
    return true;
}

/*===========================================================================
 * Stack overflow detection — persistent record + configurable recovery
 *===========================================================================*/

/*
 * Overflow diagnostic record in .noinit RAM.
 *
 * The .noinit section is intentionally NOT zeroed by Reset_Handler, so this
 * variable retains its value across a soft (NVIC) reset.  That allows the
 * OVERFLOW_ACTION_RESET path to write a diagnosis, trigger a reset, and have
 * the application read back what happened on the next boot via
 * os_stack_overflow_get_record().
 *
 * On a cold power-on the content is undefined; the magic word distinguishes a
 * valid record from random memory.
 */
static os_overflow_record_t overflow_record
    __attribute__((section(".noinit"), used));

/* Configured recovery action — default: halt with breakpoint */
static overflow_action_t overflow_action = OVERFLOW_ACTION_HALT;

void os_stack_overflow_set_action(overflow_action_t action) {
    overflow_action = action;
}

bool os_stack_overflow_record_valid(void) {
    return (overflow_record.magic == OS_OVERFLOW_RECORD_MAGIC);
}

bool os_stack_overflow_get_record(os_overflow_record_t *out) {
    if (overflow_record.magic != OS_OVERFLOW_RECORD_MAGIC) {
        return false;
    }
    if (out != NULL) {
        *out = overflow_record;
    }
    overflow_record.magic = 0U;  /* consume — prevent stale re-read */
    return true;
}

/**
 * Default overflow hook — fills a persistent diagnostic record, then acts on
 * the configured overflow_action.  Override (non-weak) for custom behaviour.
 *
 * Called from interrupt context (PendSV or SysTick) with interrupts masked.
 */
__attribute__((weak))
void os_stack_overflow_hook(tcb_t *task) {
    /* 1. Count how many of the STACK_GUARD_WORDS guard words were corrupted. */
    uint32_t corrupted = 0;
    if (task != NULL) {
        for (uint32_t i = 0; i < STACK_GUARD_WORDS; i++) {
            if (task->stack[i] != STACK_GUARD_MAGIC) {
                corrupted++;
            }
        }
    }

    /* 2. Write diagnostic record (valid for both RESET and post-mortem reads). */
    overflow_record.magic           = OS_OVERFLOW_RECORD_MAGIC;
    overflow_record.tick_count      = kernel.tick_count;
    overflow_record.corrupted_words = corrupted;
    overflow_record.action_taken    = overflow_action;
    if (task != NULL) {
        for (int i = 0; i < 16; i++) {
            overflow_record.task_name[i] = task->name[i];
        }
        overflow_record.stack_high_water = task->stack_high_water_mark;
    } else {
        overflow_record.task_name[0] = '?';
        overflow_record.task_name[1] = '\0';
        overflow_record.stack_high_water = 0U;
    }

    /* 3. Leave a breadcrumb in the in-RAM trace ring buffer. */
    if (task != NULL) {
        trace_record_switch(task->name, "STACK_OVERFLOW");
    }

    /* 4. Execute the configured recovery action. */
    switch (overflow_action) {

        case OVERFLOW_ACTION_RESET:
            /* Record is already written above.  Issue an NVIC system reset so
             * the record survives in .noinit and the system restarts cleanly. */
            hal_system_reset();   /* does not return */
            while (1);            /* unreachable — satisfies compiler */

        case OVERFLOW_ACTION_KILL_TASK:
            if (task != NULL) {
                /* Mark the task terminated so the scheduler will not re-enqueue
                 * it.  The check in os_pendsv_switch() (old_task->state ==
                 * TASK_STATE_RUNNING) will be false and the task is skipped. */
                task->state = TASK_STATE_TERMINATED;

                /* Replant guard words so the guard check at the next PendSV
                 * entry does not re-fire the hook for the same task. */
                for (uint32_t i = 0; i < STACK_GUARD_WORDS; i++) {
                    task->stack[i] = STACK_GUARD_MAGIC;
                }

                /* Remove from task registry so stats iteration skips it. */
                for (int i = 0; i < MAX_TASKS; i++) {
                    if (kernel.task_registry[i] == task) {
                        kernel.task_registry[i] = NULL;
                        break;
                    }
                }
                if (kernel.task_count > 0U) {
                    kernel.task_count--;
                }

                /* Pend a context switch.  The current task context (which may
                 * still be executing on the stack we just killed) will be
                 * abandoned at the next PendSV; the scheduler picks the next
                 * ready task instead. */
                os_pend_sv();
            }
            return;   /* do NOT spin — let the system continue */

        case OVERFLOW_ACTION_HALT:
        default:
            /* Trigger a debug breakpoint.  Without a debugger attached on
             * ARMv7-M this escalates to HardFault, which is intentional. */
            __asm__ volatile("bkpt #1");
            while (1);
    }
}

/*===========================================================================
 * Tickless Idle
 *
 * Called by os_power_enter_idle() when tickless idle mode is enabled.
 *
 * Normal (non-tickless) idle fires SysTick every 1 ms regardless of whether
 * any work is pending.  Each ISR entry/exit burns ~hundreds of cycles and
 * prevents the CPU's deepest sleep states.
 *
 * Tickless idle:
 *   1. Inspects the delay queue to find the earliest task wakeup.
 *   2. Stops SysTick so the CPU can sleep uninterrupted.
 *   3. Executes WFI — any enabled interrupt (timer, GPIO, UART …) wakes it.
 *   4. Measures actual elapsed time via the DWT cycle counter (Cortex-M3/4/7).
 *   5. Re-enables SysTick and advances kernel.tick_count by the measured ticks.
 *   6. Processes the delay queue and software timers for the elapsed period.
 *
 * DWT cycle counter (0xE0001004):
 *   32-bit free-running counter driven by the processor clock.  Unsigned
 *   subtraction (after - before) handles the wrap-around correctly.
 *   At 168 MHz it wraps every ~25 s; since we cap sleep at
 *   TICKLESS_MAX_SLEEP_TICKS (100 ms) this is never an issue.
 *
 * Thread safety:
 *   Must be called only from the idle task (single caller guaranteed).
 *   Internal critical sections protect shared kernel state.
 *===========================================================================*/

/* Cycles per SysTick tick — derived from the compile-time clock constant. */
#define CYCLES_PER_TICK  (SYSTEM_CORE_CLOCK / TICK_RATE_HZ)

void os_kernel_tickless_sleep(void) {
    uint32_t cs = os_enter_critical();

    /* ── 1. Determine safe sleep duration ─────────────────────────────── */
    uint32_t sleep_ticks = TICKLESS_MAX_SLEEP_TICKS;

    if (kernel.delay_queue != NULL) {
        int32_t until_next =
            (int32_t)(kernel.delay_queue->wake_tick - kernel.tick_count);
        if (until_next <= 0) {
            /* A task is already overdue — skip sleep entirely. */
            os_exit_critical(cs);
            return;
        }
        if ((uint32_t)until_next < sleep_ticks) {
            sleep_ticks = (uint32_t)until_next;
        }
    }

    /* ── 2. Stop tick source and snapshot the cycle counter ───────────── */
    hal_tick_suppress(sleep_ticks);
    uint32_t cyc_before = hal_cycle_counter_read();

    os_exit_critical(cs);

    /* ── 3. Sleep ─────────────────────────────────────────────────────── */
    hal_cpu_wait_for_interrupt();

    /* ── 4. Measure elapsed ticks via cycle counter ────────────────────── */
    cs = os_enter_critical();

    uint32_t elapsed_cycles = hal_cycle_counter_read() - cyc_before;
    uint32_t elapsed_ticks  = elapsed_cycles / CYCLES_PER_TICK;
    if (elapsed_ticks > sleep_ticks) {
        elapsed_ticks = sleep_ticks;   /* clamp: never advance past our bound */
    }

    /* ── 5. Restart tick source and advance tick counter ─────────────── */
    hal_tick_unsuppress();

    kernel.tick_count += elapsed_ticks;

    /* ── 6. Wake overdue tasks (all in one sorted-list pass) ──────────── */
    delay_queue_tick();

    os_exit_critical(cs);

    /* Process software timers outside the critical section so that timer
     * callbacks can safely call blocking OS APIs (e.g. semaphore post). */
    if (elapsed_ticks > 0) {
        os_timer_process();
    }
}
