/**
 * TinyOS Synchronization Primitives
 *
 * Implements mutexes, semaphores, condition variables, and message queues
 * with priority inheritance to prevent priority inversion
 */

#include "tinyos.h"
#include "tinyos/trace.h"
#include <string.h>

/* Convenience: get the current task name safely (may be NULL before scheduler starts). */
static const char *cur_name(void) {
    tcb_t *t = os_task_get_current();
    return (t && t->name[0]) ? t->name : "?";
}

/*===========================================================================
 * Mutex internal helpers
 *===========================================================================*/

/**
 * Insert a task into the mutex wait queue, sorted by priority.
 * Highest priority (lowest numeric value) goes first — it will be picked
 * first by os_mutex_unlock().
 * Must be called inside a critical section.
 */
static void mutex_wait_enqueue(mutex_t *mutex, tcb_t *task) {
    tcb_t **pp = &mutex->wait_queue;
    /* Walk past tasks with equal or higher priority (lower number). */
    while (*pp != NULL && (*pp)->priority <= task->priority) {
        pp = &(*pp)->next;
    }
    task->next = *pp;
    *pp = task;
}

/**
 * Remove a specific task from the mutex wait queue.
 * Must be called inside a critical section.
 */
static void mutex_wait_dequeue(mutex_t *mutex, tcb_t *task) {
    tcb_t **pp = &mutex->wait_queue;
    while (*pp != NULL) {
        if (*pp == task) {
            *pp = task->next;
            task->next = NULL;
            return;
        }
        pp = &(*pp)->next;
    }
}

/**
 * Recalculate the mutex owner's inherited priority after the wait queue
 * changes (a waiter was added, removed, or woken).
 *
 * Rule: owner inherits the priority of its highest-priority waiter.
 *       If no waiters remain, the owner reverts to its base priority.
 *
 * Must be called inside a critical section.
 */
static void mutex_pip_recalculate(mutex_t *mutex) {
    if (mutex->owner == NULL) return;

    /* Head of the sorted wait queue is the highest-priority waiter. */
    if (mutex->wait_queue != NULL) {
        task_priority_t top = mutex->wait_queue->priority;
        if (top < mutex->owner->priority) {
            /* Boost owner to match the best waiter. */
            os_task_raise_priority(mutex->owner, top);
        }
        /* If top >= owner->priority the owner is already at least as good;
         * no downgrade needed here (raise_priority only ever lifts). */
    } else {
        /* No more waiters — restore owner to its base priority. */
        os_task_reset_priority(mutex->owner);
    }
}

/*===========================================================================
 * Mutex public API
 *===========================================================================*/

/**
 * Initialize mutex
 */
void os_mutex_init(mutex_t *mutex) {
    if (mutex == NULL) return;

    mutex->locked           = false;
    mutex->owner            = NULL;
    mutex->ceiling_priority = PRIORITY_IDLE;
    mutex->wait_queue       = NULL;
}

/**
 * Lock mutex — Priority Inheritance Protocol implementation.
 *
 * Fast path (mutex free): acquire immediately, O(1).
 *
 * Slow path (mutex held):
 *   - OS_WAIT_FOREVER: the calling task truly blocks (state = BLOCKED).
 *     The owner's priority is boosted to the caller's priority to prevent
 *     priority inversion.  The task is placed on a priority-sorted wait
 *     queue and woken directly by os_mutex_unlock() when it is next in line.
 *
 *   - timeout > 0: spin-loop with priority boost.  The calling task yields
 *     each iteration so the boosted owner can make progress, and returns
 *     OS_ERROR_TIMEOUT when the deadline is reached.
 */
os_error_t os_mutex_lock(mutex_t *mutex, uint32_t timeout) {
    if (mutex == NULL) return OS_ERROR_INVALID_PARAM;

    tcb_t *current = os_task_get_current();

    /* ── Fast path ─────────────────────────────────────────── */
    uint32_t cs = os_enter_critical();
    if (!mutex->locked) {
        mutex->locked = true;
        mutex->owner  = current;
        os_exit_critical(cs);
        trace_record_syscall(cur_name(), "mutex_lock");
        return OS_OK;
    }
    os_exit_critical(cs);

    /* ── Slow path: mutex is held ───────────────────────────── */

    if (timeout == OS_WAIT_FOREVER) {
        /*
         * True blocking wait with PIP.
         *
         * 1. Inside critical section: boost owner, enqueue self, set BLOCKED.
         * 2. Release critical section, yield (PendSV fires; BLOCKED → not re-enqueued).
         * 3. Wake: os_mutex_unlock() has already made us the owner before
         *    calling os_task_wakeup(), so we return OS_OK immediately.
         */
        cs = os_enter_critical();

        /* PIP: boost the owner so it can finish faster. */
        if (current->priority < mutex->owner->priority) {
            os_task_raise_priority(mutex->owner, current->priority);
        }

        /* Register as waiter (sorted list, highest-priority first). */
        mutex_wait_enqueue(mutex, current);
        current->state = TASK_STATE_BLOCKED;

        os_exit_critical(cs);

        /* Give up CPU.  Because state == BLOCKED, os_pendsv_switch() will
         * NOT re-enqueue this task — it stays off the ready queue until
         * os_mutex_unlock() calls os_task_wakeup(). */
        os_task_yield();

        /* ── Resumed: we are now the mutex owner ── */
        trace_record_syscall(cur_name(), "mutex_lock");
        return OS_OK;

    } else {
        /*
         * Timed spin-loop with PIP.
         * Simpler implementation: keeps trying until the mutex is free or
         * the timeout expires, boosting the owner's priority each iteration.
         */
        uint32_t start = os_get_tick_count();

        while (true) {
            cs = os_enter_critical();

            if (!mutex->locked) {
                mutex->locked = true;
                mutex->owner  = current;
                os_exit_critical(cs);
                trace_record_syscall(cur_name(), "mutex_lock");
                return OS_OK;
            }

            /* PIP boost each iteration in case owner's priority changed. */
            if (current->priority < mutex->owner->priority) {
                os_task_raise_priority(mutex->owner, current->priority);
            }

            os_exit_critical(cs);

            if ((os_get_tick_count() - start) >= timeout) {
                return OS_ERROR_TIMEOUT;
            }

            os_task_yield();
        }
    }
}

/**
 * Unlock mutex — transfers ownership to the highest-priority waiter (if any).
 *
 * Steps:
 *   1. Verify caller is the owner.
 *   2. If wait queue is non-empty:
 *        a. Pop the highest-priority waiter (head of sorted list).
 *        b. Transfer mutex ownership to the waiter directly.
 *        c. Recalculate PIP for the new owner from remaining waiters.
 *        d. Wake the waiter via os_task_wakeup() (may trigger immediate
 *           preemption if waiter outranks us).
 *      Else:
 *        Release the mutex (locked = false, owner = NULL).
 *   3. Restore our own priority to base_priority.
 *   4. Yield so the woken (likely higher-priority) task can run.
 */
os_error_t os_mutex_unlock(mutex_t *mutex) {
    if (mutex == NULL) return OS_ERROR_INVALID_PARAM;

    tcb_t *current = os_task_get_current();
    uint32_t cs = os_enter_critical();

    if (mutex->owner != current) {
        os_exit_critical(cs);
        return OS_ERROR_PERMISSION_DENIED;
    }

    if (mutex->wait_queue != NULL) {
        /* Pop the highest-priority waiter (head of priority-sorted list). */
        tcb_t *next_owner   = mutex->wait_queue;
        mutex->wait_queue   = next_owner->next;
        next_owner->next    = NULL;

        /* Hand over ownership BEFORE waking the waiter so that when
         * it resumes in os_mutex_lock() it already owns the mutex. */
        mutex->owner = next_owner;
        /* mutex->locked stays true — ownership is transferred, not released. */

        /* Recalculate our own inherited priority now that we released.
         * (We are no longer the owner, so pass a temporary NULL owner to
         *  avoid touching the new owner's priority here — we'll let
         *  mutex_pip_recalculate handle the new owner's waiters.) */
        current->priority = current->base_priority;   /* restore ours inline */

        /* Recalculate PIP for the NEW owner based on remaining waiters. */
        mutex_pip_recalculate(mutex);

        /* Wake the new owner — may pend PendSV if it outranks current. */
        os_task_wakeup(next_owner);

    } else {
        /* No waiters: simply release. */
        mutex->locked = false;
        mutex->owner  = NULL;
        current->priority = current->base_priority;   /* restore inline */
    }

    os_exit_critical(cs);
    trace_record_syscall(cur_name(), "mutex_unlk");

    /* Yield so the woken high-priority task (or any other) can run. */
    os_task_yield();
    return OS_OK;
}

/**
 * Initialize semaphore
 */
void os_semaphore_init(semaphore_t *sem, int32_t initial_count) {
    if (sem == NULL) return;

    sem->count = initial_count;
    sem->wait_queue = NULL;
}

/**
 * Wait on semaphore (P operation)
 */
os_error_t os_semaphore_wait(semaphore_t *sem, uint32_t timeout) {
    if (sem == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t start_tick = os_get_tick_count();

    while (true) {
        uint32_t state = os_enter_critical();

        if (sem->count > 0) {
            sem->count--;
            os_exit_critical(state);
            trace_record_syscall(cur_name(), "sem_wait");
            return OS_OK;
        }

        os_exit_critical(state);

        /* Check timeout */
        if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
            return OS_ERROR_TIMEOUT;
        }

        /* Block and wait */
        os_task_yield();
    }
}

/**
 * Get current semaphore count (non-blocking query)
 */
int32_t os_semaphore_get_count(semaphore_t *sem) {
    if (sem == NULL) {
        return 0;
    }

    uint32_t state = os_enter_critical();
    int32_t count = sem->count;
    os_exit_critical(state);

    return count;
}

/**
 * Post to semaphore (V operation)
 */
os_error_t os_semaphore_post(semaphore_t *sem) {
    if (sem == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();
    sem->count++;
    os_exit_critical(state);
    trace_record_syscall(cur_name(), "sem_post");

    /* Wake up waiting tasks */
    os_task_yield();

    return OS_OK;
}

/**
 * Initialize message queue
 */
os_error_t os_queue_init(
    msg_queue_t *queue,
    void *buffer,
    size_t item_size,
    size_t max_items
) {
    if (queue == NULL || buffer == NULL || item_size == 0 || max_items == 0) {
        return OS_ERROR_INVALID_PARAM;
    }

    queue->buffer = buffer;
    queue->item_size = item_size;
    queue->max_items = max_items;
    queue->head = 0;
    queue->tail = 0;
    queue->count = 0;

    os_mutex_init(&queue->lock);

    return OS_OK;
}

/**
 * Send message to queue
 */
os_error_t os_queue_send(
    msg_queue_t *queue,
    const void *item,
    uint32_t timeout
) {
    if (queue == NULL || item == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t start_tick = os_get_tick_count();

    while (true) {
        os_error_t err = os_mutex_lock(&queue->lock, 10);
        if (err != OS_OK) {
            if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
                return OS_ERROR_TIMEOUT;
            }
            continue;
        }

        if (queue->count < queue->max_items) {
            /* Copy item to queue */
            uint8_t *dest = (uint8_t *)queue->buffer + (queue->tail * queue->item_size);
            memcpy(dest, item, queue->item_size);

            queue->tail = (queue->tail + 1) % queue->max_items;
            queue->count++;

            os_mutex_unlock(&queue->lock);
            return OS_OK;
        }

        os_mutex_unlock(&queue->lock);

        /* Check timeout */
        if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
            return OS_ERROR_TIMEOUT;
        }

        /* Queue full, yield and retry */
        os_task_delay(1);
    }
}

/**
 * Peek at the front of the queue without consuming the item.
 * Blocks until an item is available or the timeout expires.
 */
os_error_t os_queue_peek(msg_queue_t *queue, void *item, uint32_t timeout) {
    if (queue == NULL || item == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t start_tick = os_get_tick_count();

    while (true) {
        os_error_t err = os_mutex_lock(&queue->lock, 10);
        if (err != OS_OK) {
            if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
                return OS_ERROR_TIMEOUT;
            }
            continue;
        }

        if (queue->count > 0) {
            /* Copy the front item WITHOUT advancing head or decrementing count */
            const uint8_t *src = (const uint8_t *)queue->buffer +
                                 (queue->head * queue->item_size);
            memcpy(item, src, queue->item_size);
            os_mutex_unlock(&queue->lock);
            return OS_OK;
        }

        os_mutex_unlock(&queue->lock);

        /* Check timeout */
        if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
            return OS_ERROR_TIMEOUT;
        }

        os_task_delay(1);
    }
}

/**
 * Get current number of items in the queue (non-blocking query)
 */
size_t os_queue_get_count(msg_queue_t *queue) {
    if (queue == NULL) {
        return 0;
    }

    uint32_t state = os_enter_critical();
    size_t count = queue->count;
    os_exit_critical(state);

    return count;
}

/**
 * Receive message from queue
 */
os_error_t os_queue_receive(
    msg_queue_t *queue,
    void *item,
    uint32_t timeout
) {
    if (queue == NULL || item == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t start_tick = os_get_tick_count();

    while (true) {
        os_error_t err = os_mutex_lock(&queue->lock, 10);
        if (err != OS_OK) {
            if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
                return OS_ERROR_TIMEOUT;
            }
            continue;
        }

        if (queue->count > 0) {
            /* Copy item from queue */
            uint8_t *src = (uint8_t *)queue->buffer + (queue->head * queue->item_size);
            memcpy(item, src, queue->item_size);

            queue->head = (queue->head + 1) % queue->max_items;
            queue->count--;

            os_mutex_unlock(&queue->lock);
            return OS_OK;
        }

        os_mutex_unlock(&queue->lock);

        /* Check timeout */
        if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
            return OS_ERROR_TIMEOUT;
        }

        /* Queue empty, yield and retry */
        os_task_delay(1);
    }
}

/**
 * Event Group Implementation
 */

/**
 * Initialize event group
 */
void os_event_group_init(event_group_t *event_group) {
    if (event_group == NULL) return;

    event_group->events = 0;
    event_group->wait_queue = NULL;
}

/**
 * Set event bits
 * This function sets the specified bits and wakes up any waiting tasks
 */
os_error_t os_event_group_set_bits(event_group_t *event_group, uint32_t bits) {
    if (event_group == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Set the event bits */
    event_group->events |= bits;

    os_exit_critical(state);

    /* Wake up tasks that might be waiting for these events */
    os_task_yield();

    return OS_OK;
}

/**
 * Clear event bits
 */
os_error_t os_event_group_clear_bits(event_group_t *event_group, uint32_t bits) {
    if (event_group == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Clear the specified bits */
    event_group->events &= ~bits;

    os_exit_critical(state);

    return OS_OK;
}

/**
 * Wait for event bits
 * Supports waiting for ANY or ALL specified bits with optional auto-clear
 */
os_error_t os_event_group_wait_bits(
    event_group_t *event_group,
    uint32_t bits_to_wait_for,
    uint8_t options,
    uint32_t *bits_received,
    uint32_t timeout
) {
    if (event_group == NULL || bits_to_wait_for == 0) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t start_tick = os_get_tick_count();
    bool wait_all = (options & EVENT_WAIT_ALL) != 0;
    bool clear_on_exit = (options & EVENT_CLEAR_ON_EXIT) != 0;

    while (true) {
        uint32_t state = os_enter_critical();

        uint32_t current_events = event_group->events;
        bool condition_met = wait_all
            ? (current_events & bits_to_wait_for) == bits_to_wait_for  /* ALL bits */
            : (current_events & bits_to_wait_for) != 0;                /* ANY bit  */

        if (condition_met) {
            /* Return the bits that matched */
            if (bits_received != NULL) {
                *bits_received = current_events & bits_to_wait_for;
            }

            /* Clear bits if requested */
            if (clear_on_exit) {
                event_group->events &= ~bits_to_wait_for;
            }

            os_exit_critical(state);
            return OS_OK;
        }

        os_exit_critical(state);

        /* Check timeout */
        if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
            return OS_ERROR_TIMEOUT;
        }

        /* Yield and wait for events */
        os_task_yield();
    }
}

/**
 * Get current event bits (non-blocking)
 */
uint32_t os_event_group_get_bits(event_group_t *event_group) {
    if (event_group == NULL) {
        return 0;
    }

    uint32_t state = os_enter_critical();
    uint32_t bits = event_group->events;
    os_exit_critical(state);

    return bits;
}

/**
 * Condition Variable Implementation
 */

/**
 * Remove a task from a condition variable's wait queue (must be called within critical section)
 */
static void cond_remove_task(cond_var_t *cond, tcb_t *task) {
    if (cond->wait_queue == task) {
        cond->wait_queue = task->next;
    } else {
        tcb_t *prev = cond->wait_queue;
        while (prev != NULL && prev->next != task) {
            prev = prev->next;
        }
        if (prev != NULL) {
            prev->next = task->next;
        }
    }
    cond->waiting_count--;
    task->next = NULL;
}

/**
 * Initialize condition variable
 */
void os_cond_init(cond_var_t *cond) {
    if (cond == NULL) return;

    cond->wait_queue = NULL;
    cond->waiting_count = 0;
}

/**
 * Wait on condition variable
 *
 * This function atomically unlocks the mutex and waits on the condition.
 * When signaled or timed out, it re-acquires the mutex before returning.
 *
 * @param cond Pointer to condition variable
 * @param mutex Pointer to mutex (must be locked by caller)
 * @param timeout Timeout in milliseconds (0 = wait forever)
 * @return OS_OK on success, OS_ERROR_TIMEOUT on timeout, OS_ERROR_INVALID_PARAM on error
 */
os_error_t os_cond_wait(cond_var_t *cond, mutex_t *mutex, uint32_t timeout) {
    if (cond == NULL || mutex == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    tcb_t *current_task = os_task_get_current();
    uint32_t start_tick = os_get_tick_count();

    /* Add current task to wait queue */
    uint32_t state = os_enter_critical();

    /* Add to wait queue (simple FIFO) */
    if (cond->wait_queue == NULL) {
        cond->wait_queue = current_task;
        current_task->next = NULL;
    } else {
        /* Find end of queue */
        tcb_t *tail = cond->wait_queue;
        while (tail->next != NULL) {
            tail = tail->next;
        }
        tail->next = current_task;
        current_task->next = NULL;
    }
    cond->waiting_count++;

    os_exit_critical(state);

    /* Release the mutex */
    os_error_t unlock_result = os_mutex_unlock(mutex);
    if (unlock_result != OS_OK) {
        /* Remove from wait queue if unlock fails */
        state = os_enter_critical();

        cond_remove_task(cond, current_task);

        os_exit_critical(state);
        return unlock_result;
    }

    /* Wait to be signaled */
    bool signaled = false;
    while (!signaled) {
        state = os_enter_critical();

        /* Check if we're still in the wait queue */
        tcb_t *task = cond->wait_queue;
        bool found = false;
        while (task != NULL) {
            if (task == current_task) {
                found = true;
                break;
            }
            task = task->next;
        }

        if (!found) {
            /* We've been signaled */
            signaled = true;
        }

        os_exit_critical(state);

        if (signaled) {
            break;
        }

        /* Check timeout */
        if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
            /* Timeout - remove ourselves from wait queue */
            state = os_enter_critical();

            cond_remove_task(cond, current_task);

            os_exit_critical(state);

            /* Re-acquire mutex before returning */
            os_mutex_lock(mutex, 0);
            return OS_ERROR_TIMEOUT;
        }

        /* Yield to other tasks */
        os_task_yield();
    }

    /* Re-acquire the mutex before returning */
    os_mutex_lock(mutex, 0);

    return OS_OK;
}

/**
 * Signal one waiting task
 * Wakes up the first task in the wait queue
 */
os_error_t os_cond_signal(cond_var_t *cond) {
    if (cond == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Wake up one task if any are waiting */
    if (cond->wait_queue != NULL) {
        tcb_t *task_to_wake = cond->wait_queue;
        cond->wait_queue = task_to_wake->next;
        task_to_wake->next = NULL;
        cond->waiting_count--;
    }

    os_exit_critical(state);

    /* Allow the awakened task to run */
    os_task_yield();

    return OS_OK;
}

/**
 * Broadcast to all waiting tasks
 * Wakes up all tasks in the wait queue
 */
os_error_t os_cond_broadcast(cond_var_t *cond) {
    if (cond == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Wake up all waiting tasks */
    while (cond->wait_queue != NULL) {
        tcb_t *task_to_wake = cond->wait_queue;
        cond->wait_queue = task_to_wake->next;
        task_to_wake->next = NULL;
        cond->waiting_count--;
    }

    os_exit_critical(state);

    /* Allow awakened tasks to run */
    os_task_yield();

    return OS_OK;
}
