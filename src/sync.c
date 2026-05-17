/**
 * TinyOS Synchronization Primitives
 *
 * Implements mutexes, semaphores, condition variables, and message queues
 * with priority inheritance to prevent priority inversion
 */

#include "tinyos.h"
#include "tinyos/trace.h"
#include <string.h>

/* Kernel-internal functions for arming/disarming the delay queue on behalf of
 * a task that is blocking on a synchronisation object with a finite timeout.
 * Declared here to avoid adding them to the public tinyos.h API. */
extern void os_delay_queue_arm(tcb_t *task);
extern void os_delay_queue_disarm(tcb_t *task);

/* Convenience: get the current task name safely (may be NULL before scheduler starts). */
static const char *cur_name(void) {
    tcb_t *t = os_task_get_current();
    return (t && t->name[0]) ? t->name : "?";
}

/* Forward declarations for helpers used before their definitions */
static void queue_wait_enqueue(tcb_t **head, tcb_t *task);
static void cond_remove_task(cond_var_t *cond, tcb_t *task);

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
__attribute__((unused)) static void mutex_wait_dequeue(mutex_t *mutex, tcb_t *task) {
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
 * Deadlock detection (Wait-For Graph cycle detection)
 *===========================================================================*/

/**
 * Follow the wait-for chain starting from mutex->owner.
 * Returns true if the chain loops back to waiter, indicating a deadlock cycle.
 *
 * Chain: waiter → mutex → mutex->owner → owner->waiting_on → that mutex's
 *        owner → ... → waiter (cycle).
 *
 * Depth is bounded by MAX_TASKS so this is O(N) and cannot loop forever.
 * Must be called inside a critical section (reads waiting_on / owner atomically).
 */
static bool deadlock_check(const tcb_t *waiter, const mutex_t *mutex) {
    const tcb_t *node = mutex->owner;
    for (uint32_t i = 0; i < MAX_TASKS; i++) {
        if (node == NULL)             return false;  /* chain ends cleanly    */
        if (node == waiter)           return true;   /* cycle detected        */
        if (node->waiting_on == NULL) return false;  /* node is not blocked   */
        node = node->waiting_on->owner;
    }
    return false;  /* depth limit — treat as no deadlock */
}

/* Default deadlock hook — halt with breakpoint so a debugger can inspect. */
__attribute__((weak)) void os_deadlock_hook(tcb_t *waiter, mutex_t *blocked_on) {
    (void)waiter;
    (void)blocked_on;
    while (1) { __asm__ volatile("bkpt #3"); }
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
         * 1. Inside critical section: detect deadlock, boost owner, enqueue
         *    self, set BLOCKED.
         * 2. Release critical section, yield (PendSV fires; BLOCKED → not re-enqueued).
         * 3. Wake: os_mutex_unlock() has already made us the owner before
         *    calling os_task_wakeup(), so we return OS_OK immediately.
         */
        cs = os_enter_critical();

        /* Deadlock detection: set waiting_on before checking so that
         * concurrent checks by other tasks can traverse the chain. */
        current->waiting_on = mutex;
        if (deadlock_check(current, mutex)) {
            current->waiting_on = NULL;
            os_exit_critical(cs);
            os_deadlock_hook(current, mutex);
            return OS_ERROR_DEADLOCK;
        }

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
        current->waiting_on = NULL;
        trace_record_syscall(cur_name(), "mutex_lock");
        return OS_OK;

    } else {
        /*
         * Timed blocking wait with PIP — true sleep, zero spin.
         *
         * The task is inserted into BOTH the mutex wait queue (via next) AND
         * the delay queue (via delay_next).  Whichever fires first wins:
         *
         *   Mutex unlocked first:
         *     os_mutex_unlock() pops us from the wait queue, disarms the
         *     delay queue entry, transfers ownership, and wakes us.
         *     On resume: mutex->owner == current → return OS_OK.
         *
         *   Deadline reached first (SysTick → delay_queue_tick):
         *     delay_queue_tick() removes us from the mutex wait queue,
         *     recalculates PIP for the owner, and schedules us as READY.
         *     On resume: mutex->owner != current → return OS_ERROR_TIMEOUT.
         */
        cs = os_enter_critical();

        /* Deadlock detection before committing to block. */
        current->waiting_on = mutex;
        if (deadlock_check(current, mutex)) {
            current->waiting_on = NULL;
            os_exit_critical(cs);
            os_deadlock_hook(current, mutex);
            return OS_ERROR_DEADLOCK;
        }

        /* PIP boost so the owner can release sooner. */
        if (current->priority < mutex->owner->priority) {
            os_task_raise_priority(mutex->owner, current->priority);
        }

        /* Enqueue in mutex wait queue (priority-sorted, via next). */
        mutex_wait_enqueue(mutex, current);

        /* Arm the timeout in the delay queue (via delay_next). */
        current->wake_tick = os_get_tick_count() + timeout;
        os_delay_queue_arm(current);

        current->state = TASK_STATE_BLOCKED;
        os_exit_critical(cs);

        /* Park until either the mutex is transferred or the deadline fires. */
        os_task_yield();

        /* ── Resumed ── determine outcome ─────────────────────────────── */
        cs = os_enter_critical();
        bool acquired = (mutex->owner == current);
        if (acquired) {
            /* Woken by os_mutex_unlock — disarm in case it wasn't removed yet.
             * (os_mutex_unlock already called disarm, so this is a safe no-op.) */
            current->waiting_on = NULL;
        }
        /* If !acquired: delay_queue_tick already cleared waiting_on and
         * removed us from the mutex wait queue. */
        os_exit_critical(cs);

        if (acquired) {
            trace_record_syscall(cur_name(), "mutex_lock");
            return OS_OK;
        }
        return OS_ERROR_TIMEOUT;
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

        /* Cancel any pending timeout for this waiter — it won the mutex so
         * the delay queue entry is no longer needed. Safe no-op if the waiter
         * had no timeout (WAIT_FOREVER path never armed the delay queue). */
        os_delay_queue_disarm(next_owner);
        next_owner->waiting_on = NULL;

        /* Hand over ownership BEFORE waking the waiter so that when
         * it resumes in os_mutex_lock() it already owns the mutex. */
        mutex->owner = next_owner;
        /* mutex->locked stays true — ownership is transferred, not released. */

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

    tcb_t *current = os_task_get_current();

    /* ── Fast path ──────────────────────────────────────────────── */
    uint32_t cs = os_enter_critical();
    if (sem->count > 0) {
        sem->count--;
        os_exit_critical(cs);
        trace_record_syscall(cur_name(), "sem_wait");
        return OS_OK;
    }
    os_exit_critical(cs);

    /* ── Slow path ──────────────────────────────────────────────── */
    if (timeout == OS_WAIT_FOREVER) {
        /*
         * True blocking wait (no spin).
         * 1. Enqueue self in priority-sorted wait list.
         * 2. Set state BLOCKED — PendSV will not re-enqueue the task.
         * 3. Yield; resumes when os_semaphore_post() calls os_task_wakeup().
         */
        cs = os_enter_critical();
        queue_wait_enqueue(&sem->wait_queue, current);
        current->state = TASK_STATE_BLOCKED;
        os_exit_critical(cs);

        os_task_yield();   /* parks until woken; resumes here after post() */

        trace_record_syscall(cur_name(), "sem_wait");
        return OS_OK;
    }

    /* ── Timed spin-loop ────────────────────────────────────────── */
    uint32_t start_tick = os_get_tick_count();
    while (true) {
        cs = os_enter_critical();
        if (sem->count > 0) {
            sem->count--;
            os_exit_critical(cs);
            trace_record_syscall(cur_name(), "sem_wait");
            return OS_OK;
        }
        os_exit_critical(cs);

        if ((os_get_tick_count() - start_tick) >= timeout) {
            return OS_ERROR_TIMEOUT;
        }
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

    uint32_t cs = os_enter_critical();

    if (sem->wait_queue != NULL) {
        /* Direct handoff: transfer to the highest-priority waiter without
         * incrementing count, mirroring mutex ownership transfer. */
        tcb_t *waiter  = sem->wait_queue;
        sem->wait_queue = waiter->next;
        waiter->next   = NULL;
        os_task_wakeup(waiter);   /* BLOCKED → READY; may pend PendSV */
    } else {
        sem->count++;
    }

    os_exit_critical(cs);
    trace_record_syscall(cur_name(), "sem_post");

    return OS_OK;
}

/*===========================================================================
 * Message queue internal helpers
 *===========================================================================*/

/**
 * Insert a task into a priority-sorted wait queue.
 * Highest priority (lowest numeric value) is placed first.
 * Must be called inside a critical section.
 */
static void queue_wait_enqueue(tcb_t **head, tcb_t *task) {
    tcb_t **pp = head;
    while (*pp != NULL && (*pp)->priority <= task->priority)
        pp = &(*pp)->next;
    task->next = *pp;
    *pp = task;
}

/**
 * Count tasks in a wait queue.
 * Must be called inside a critical section.
 */
static uint32_t queue_wait_count(const tcb_t *head) {
    uint32_t n = 0;
    while (head) { n++; head = head->next; }
    return n;
}

/**
 * Enqueue an item at the TAIL of the circular buffer.
 * Caller must hold a critical section and verify count < max_items.
 */
static void queue_buf_push_back(msg_queue_t *q, const void *item) {
    uint8_t *dest = (uint8_t *)q->buffer + q->tail * q->item_size;
    memcpy(dest, item, q->item_size);
    q->tail = (q->tail + 1) % q->max_items;
    q->count++;
    q->total_sent++;
    if (q->count > q->peak_count) q->peak_count = q->count;
}

/**
 * Enqueue an item at the FRONT (head) of the circular buffer.
 * Caller must hold a critical section and verify count < max_items.
 */
static void queue_buf_push_front(msg_queue_t *q, const void *item) {
    q->head = (q->head + q->max_items - 1) % q->max_items;
    uint8_t *dest = (uint8_t *)q->buffer + q->head * q->item_size;
    memcpy(dest, item, q->item_size);
    q->count++;
    q->total_sent++;
    if (q->count > q->peak_count) q->peak_count = q->count;
}

/**
 * Dequeue an item from the HEAD of the circular buffer.
 * Caller must hold a critical section and verify count > 0.
 */
static void queue_buf_pop(msg_queue_t *q, void *item) {
    const uint8_t *src = (const uint8_t *)q->buffer + q->head * q->item_size;
    memcpy(item, src, q->item_size);
    q->head = (q->head + 1) % q->max_items;
    q->count--;
    q->total_received++;
}

/**
 * After inserting an item, wake the highest-priority blocked receiver (if any)
 * via direct delivery: copy item from queue head to receiver's recv_buf.
 *
 * Must be called inside a critical section, immediately after inserting an item.
 */
static void queue_wake_receiver(msg_queue_t *q) {
    if (q->receiver_wait == NULL) return;

    tcb_t *receiver    = q->receiver_wait;
    q->receiver_wait   = receiver->next;
    receiver->next     = NULL;

    /* Direct delivery: copy the item that was just inserted (now at tail-1)
     * from the buffer to the receiver's pre-registered buffer, then remove
     * it from the circular buffer so the receiver doesn't double-count. */
    q->tail = (q->tail + q->max_items - 1) % q->max_items;
    q->count--;
    /* Note: total_sent was already incremented by queue_buf_push_*; undo the
     * total_received that queue_buf_pop would add — we account here manually. */
    const uint8_t *src = (const uint8_t *)q->buffer + q->tail * q->item_size;
    memcpy(receiver->recv_buf, src, q->item_size);
    receiver->recv_buf = NULL;
    q->total_received++;

    os_task_wakeup(receiver);
}

/**
 * After removing an item, wake the highest-priority blocked sender (if any)
 * by copying its pending_msg into the queue.
 *
 * Must be called inside a critical section, immediately after removing an item.
 */
static void queue_wake_sender(msg_queue_t *q) {
    if (q->sender_wait == NULL) return;
    if (q->count >= q->max_items) return;  /* still no room (shouldn't happen) */

    tcb_t *sender    = q->sender_wait;
    q->sender_wait   = sender->next;
    sender->next     = NULL;

    queue_buf_push_back(q, sender->pending_msg);
    sender->pending_msg = NULL;

    os_task_wakeup(sender);
}

/*===========================================================================
 * Core blocking helper shared by send and send_to_front
 *===========================================================================*/

typedef enum { QUEUE_SEND_BACK, QUEUE_SEND_FRONT } queue_send_pos_t;

/**
 * Internal implementation for os_queue_send / os_queue_send_to_front.
 *
 * Fast path (space available):
 *   - Enqueue at back or front.
 *   - If a receiver is waiting, deliver directly and wake it.
 *   - Return OS_OK.
 *
 * Slow path — OS_WAIT_FOREVER:
 *   - Store item pointer in TCB (pending_msg).
 *   - Enqueue self in priority-sorted sender_wait.
 *   - Set state = BLOCKED, yield.
 *   - The receiver that wakes us has already copied our message and woken us.
 *   - Return OS_OK.
 *
 * Slow path — finite timeout:
 *   - Spin-yield with timeout check (no true blocking to keep code simple).
 *
 * Non-blocking (try): caller passes timeout = OS_WAIT_FOREVER but sets a
 * separate flag — handled by the public wrappers, not this function.
 */
static os_error_t queue_send_impl(
    msg_queue_t      *queue,
    const void       *item,
    uint32_t          timeout,
    queue_send_pos_t  pos
) {
    if (queue == NULL || item == NULL) return OS_ERROR_INVALID_PARAM;

    tcb_t *current = os_task_get_current();

    uint32_t cs = os_enter_critical();

    /* ── Fast path: space available ─────────────────────────── */
    if (queue->count < queue->max_items) {
        if (pos == QUEUE_SEND_FRONT)
            queue_buf_push_front(queue, item);
        else
            queue_buf_push_back(queue, item);

        /* Wake a blocked receiver via direct delivery if one is waiting. */
        queue_wake_receiver(queue);

        os_exit_critical(cs);
        trace_record_syscall(cur_name(), "q_send");
        return OS_OK;
    }

    /* ── Slow path: queue full ───────────────────────────────── */
    queue->overflow_count++;

    if (timeout == OS_WAIT_FOREVER && current != NULL) {
        /* True blocking wait.
         * 1. Record the item pointer so the receiver can copy it.
         * 2. Enqueue self in priority-sorted sender_wait.
         * 3. Set BLOCKED and yield — kernel parks this task.
         * 4. When receiver dequeues, it calls queue_wake_sender() which
         *    copies pending_msg, wakes us.  We return OS_OK on resume. */
        current->pending_msg = item;
        queue_wait_enqueue(&queue->sender_wait, current);
        current->state = TASK_STATE_BLOCKED;

        os_exit_critical(cs);
        os_task_yield();  /* won't return until receiver wakes us */

        /* overflow_count was pre-incremented above; undo it since the
         * send ultimately succeeded. */
        cs = os_enter_critical();
        if (queue->overflow_count > 0) queue->overflow_count--;
        os_exit_critical(cs);

        trace_record_syscall(cur_name(), "q_send");
        return OS_OK;

    } else if (timeout > 0) {
        /* Timed spin-yield. */
        uint32_t start = os_get_tick_count();
        os_exit_critical(cs);

        while (true) {
            cs = os_enter_critical();
            if (queue->count < queue->max_items) {
                if (pos == QUEUE_SEND_FRONT)
                    queue_buf_push_front(queue, item);
                else
                    queue_buf_push_back(queue, item);
                queue_wake_receiver(queue);
                /* Succeeded: undo pre-incremented overflow counter. */
                if (queue->overflow_count > 0) queue->overflow_count--;
                os_exit_critical(cs);
                trace_record_syscall(cur_name(), "q_send");
                return OS_OK;
            }
            os_exit_critical(cs);

            if ((os_get_tick_count() - start) >= timeout)
                return OS_ERROR_TIMEOUT;

            os_task_delay(1);
        }
    } else {
        /* timeout == 0 and not WAIT_FOREVER: non-blocking, fail immediately. */
        os_exit_critical(cs);
        return OS_ERROR_NO_RESOURCE;
    }
}

/*===========================================================================
 * Message queue public API
 *===========================================================================*/

/**
 * Initialize message queue.
 * The caller supplies the storage buffer; the queue itself is zero-allocation.
 */
os_error_t os_queue_init(
    msg_queue_t *queue,
    void *buffer,
    size_t item_size,
    size_t max_items
) {
    if (queue == NULL || buffer == NULL || item_size == 0 || max_items == 0)
        return OS_ERROR_INVALID_PARAM;

    queue->buffer         = buffer;
    queue->item_size      = item_size;
    queue->max_items      = max_items;
    queue->head           = 0;
    queue->tail           = 0;
    queue->count          = 0;
    queue->sender_wait    = NULL;
    queue->receiver_wait  = NULL;
    queue->peak_count     = 0;
    queue->total_sent     = 0;
    queue->total_received = 0;
    queue->overflow_count = 0;

    return OS_OK;
}

/**
 * Send message to the back of the queue (normal FIFO order).
 * Blocks if full.  OS_WAIT_FOREVER (0) blocks indefinitely.
 */
os_error_t os_queue_send(
    msg_queue_t *queue,
    const void *item,
    uint32_t timeout
) {
    return queue_send_impl(queue, item, timeout, QUEUE_SEND_BACK);
}

/**
 * Send message to the FRONT of the queue, bypassing FIFO order.
 * Useful for high-priority / control messages.
 * Blocking behaviour is identical to os_queue_send().
 */
os_error_t os_queue_send_to_front(
    msg_queue_t *queue,
    const void *item,
    uint32_t timeout
) {
    return queue_send_impl(queue, item, timeout, QUEUE_SEND_FRONT);
}

/**
 * Non-blocking send.
 * Returns OS_ERROR_NO_RESOURCE immediately if the queue is full.
 */
os_error_t os_queue_send_try(msg_queue_t *queue, const void *item) {
    if (queue == NULL || item == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t cs = os_enter_critical();

    if (queue->count < queue->max_items) {
        queue_buf_push_back(queue, item);
        queue_wake_receiver(queue);
        os_exit_critical(cs);
        trace_record_syscall(cur_name(), "q_send_try");
        return OS_OK;
    }

    queue->overflow_count++;
    os_exit_critical(cs);
    return OS_ERROR_NO_RESOURCE;
}

/**
 * Receive message from the queue.
 * Blocks if empty.  OS_WAIT_FOREVER (0) blocks indefinitely.
 *
 * True blocking path (OS_WAIT_FOREVER):
 *   - Store recv_buf in TCB.
 *   - Enqueue self in priority-sorted receiver_wait.
 *   - Set BLOCKED, yield.
 *   - On wake, the sender has already written directly into recv_buf.
 *   - Return OS_OK.
 */
os_error_t os_queue_receive(
    msg_queue_t *queue,
    void *item,
    uint32_t timeout
) {
    if (queue == NULL || item == NULL) return OS_ERROR_INVALID_PARAM;

    tcb_t *current = os_task_get_current();

    uint32_t cs = os_enter_critical();

    /* ── Fast path: item available ───────────────────────────── */
    if (queue->count > 0) {
        queue_buf_pop(queue, item);

        /* Let the highest-priority blocked sender fill the freed slot. */
        queue_wake_sender(queue);

        os_exit_critical(cs);
        trace_record_syscall(cur_name(), "q_recv");
        return OS_OK;
    }

    /* ── Slow path: queue empty ──────────────────────────────── */

    if (timeout == OS_WAIT_FOREVER && current != NULL) {
        /* True blocking wait.
         * Sender will deliver directly into recv_buf before calling wakeup. */
        current->recv_buf = item;
        queue_wait_enqueue(&queue->receiver_wait, current);
        current->state = TASK_STATE_BLOCKED;

        os_exit_critical(cs);
        os_task_yield();  /* won't return until sender wakes us */

        trace_record_syscall(cur_name(), "q_recv");
        return OS_OK;

    } else if (timeout > 0) {
        /* Timed spin-yield. */
        uint32_t start = os_get_tick_count();
        os_exit_critical(cs);

        while (true) {
            cs = os_enter_critical();
            if (queue->count > 0) {
                queue_buf_pop(queue, item);
                queue_wake_sender(queue);
                os_exit_critical(cs);
                trace_record_syscall(cur_name(), "q_recv");
                return OS_OK;
            }
            os_exit_critical(cs);

            if ((os_get_tick_count() - start) >= timeout)
                return OS_ERROR_TIMEOUT;

            os_task_delay(1);
        }
    } else {
        /* Non-blocking path (timeout == 0, treated as try). */
        os_exit_critical(cs);
        return OS_ERROR_NO_RESOURCE;
    }
}

/**
 * Non-blocking receive.
 * Returns OS_ERROR_NO_RESOURCE immediately if the queue is empty.
 */
os_error_t os_queue_receive_try(msg_queue_t *queue, void *item) {
    if (queue == NULL || item == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t cs = os_enter_critical();

    if (queue->count > 0) {
        queue_buf_pop(queue, item);
        queue_wake_sender(queue);
        os_exit_critical(cs);
        trace_record_syscall(cur_name(), "q_recv_try");
        return OS_OK;
    }

    os_exit_critical(cs);
    return OS_ERROR_NO_RESOURCE;
}

/**
 * Peek at the front item without consuming it.
 * Uses a spin-yield loop; does not truly block the task.
 */
os_error_t os_queue_peek(msg_queue_t *queue, void *item, uint32_t timeout) {
    if (queue == NULL || item == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t start_tick = os_get_tick_count();

    while (true) {
        uint32_t cs = os_enter_critical();

        if (queue->count > 0) {
            const uint8_t *src = (const uint8_t *)queue->buffer +
                                 queue->head * queue->item_size;
            memcpy(item, src, queue->item_size);
            os_exit_critical(cs);
            return OS_OK;
        }

        os_exit_critical(cs);

        if (timeout == 0) return OS_ERROR_NO_RESOURCE;
        if (timeout != OS_WAIT_FOREVER &&
            (os_get_tick_count() - start_tick) >= timeout)
            return OS_ERROR_TIMEOUT;

        os_task_delay(1);
    }
}

/**
 * Flush all items from the queue.
 * Blocked senders' messages are enqueued (up to capacity) and those tasks
 * are woken.  Remaining blocked receivers stay blocked.
 */
os_error_t os_queue_flush(msg_queue_t *queue) {
    if (queue == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t cs = os_enter_critical();

    /* Discard all buffered items. */
    queue->head  = 0;
    queue->tail  = 0;
    queue->count = 0;

    /* Accept pending sender messages into the now-empty queue. */
    while (queue->sender_wait != NULL && queue->count < queue->max_items) {
        tcb_t *sender    = queue->sender_wait;
        queue->sender_wait = sender->next;
        sender->next     = NULL;

        queue_buf_push_back(queue, sender->pending_msg);
        sender->pending_msg = NULL;

        os_task_wakeup(sender);
    }

    os_exit_critical(cs);
    return OS_OK;
}

/**
 * Get the number of items currently in the queue (non-blocking).
 */
size_t os_queue_get_count(msg_queue_t *queue) {
    if (queue == NULL) return 0;

    uint32_t cs = os_enter_critical();
    size_t count = queue->count;
    os_exit_critical(cs);

    return count;
}

/**
 * Get the maximum item capacity of the queue (non-blocking).
 */
size_t os_queue_get_capacity(msg_queue_t *queue) {
    if (queue == NULL) return 0;
    return queue->max_items;  /* immutable after init */
}

/**
 * Return true if the queue currently holds no items.
 */
bool os_queue_is_empty(msg_queue_t *queue) {
    if (queue == NULL) return true;

    uint32_t cs = os_enter_critical();
    bool empty = (queue->count == 0);
    os_exit_critical(cs);

    return empty;
}

/**
 * Return true if the queue is at full capacity.
 */
bool os_queue_is_full(msg_queue_t *queue) {
    if (queue == NULL) return false;

    uint32_t cs = os_enter_critical();
    bool full = (queue->count >= queue->max_items);
    os_exit_critical(cs);

    return full;
}

/**
 * Fill a stats snapshot atomically.
 */
os_error_t os_queue_get_stats(msg_queue_t *queue, msg_queue_stats_t *stats) {
    if (queue == NULL || stats == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t cs = os_enter_critical();

    stats->current_count     = queue->count;
    stats->capacity          = queue->max_items;
    stats->peak_count        = queue->peak_count;
    stats->total_sent        = queue->total_sent;
    stats->total_received    = queue->total_received;
    stats->overflow_count    = queue->overflow_count;
    stats->senders_waiting   = queue_wait_count(queue->sender_wait);
    stats->receivers_waiting = queue_wait_count(queue->receiver_wait);

    os_exit_critical(cs);
    return OS_OK;
}

/**
 * Reset cumulative statistics counters.
 * Does not affect the current item count or waiting tasks.
 */
os_error_t os_queue_reset_stats(msg_queue_t *queue) {
    if (queue == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t cs = os_enter_critical();

    queue->peak_count     = queue->count;  /* reset HWM to current level */
    queue->total_sent     = 0;
    queue->total_received = 0;
    queue->overflow_count = 0;

    os_exit_critical(cs);
    return OS_OK;
}

/**
 * Event Group Implementation
 */

/**
 * Initialize event group
 */
/*===========================================================================
 * Event group internal helpers
 *===========================================================================*/

/**
 * Returns true when the current event state satisfies a waiter's condition.
 *
 *   EVENT_WAIT_ALL              — every bit in mask must be SET   (AND)
 *   EVENT_WAIT_ANY              — at least one bit in mask is SET (OR)
 *   EVENT_WAIT_CLEAR | ALL      — every bit in mask must be CLEAR (AND-NOT)
 *   EVENT_WAIT_CLEAR | ANY      — at least one bit in mask is CLEAR (OR-NOT)
 */
static bool event_condition_met(uint32_t events, uint32_t mask, uint8_t opts) {
    if (opts & EVENT_WAIT_CLEAR) {
        /* Inverted sense: check for bits being 0. */
        if (opts & EVENT_WAIT_ALL)
            return (events & mask) == 0;        /* AND-NOT: all bits clear */
        else
            return (events & mask) != mask;     /* OR-NOT:  any bit  clear */
    }
    if (opts & EVENT_WAIT_ALL)
        return (events & mask) == mask;         /* AND: all bits set */
    else
        return (events & mask) != 0;            /* OR:  any bit  set */
}

/**
 * Walk the wait queue and wake every task whose condition is now satisfied.
 *
 * Called from os_event_group_set_bits() inside a critical section.
 * For each satisfied waiter:
 *   1. Capture the matched bits into task->event_result (before any clearing).
 *   2. If EVENT_CLEAR_ON_EXIT: clear the waiter's full mask from the group.
 *   3. Remove the task from the wait queue.
 *   4. Call os_task_wakeup() — adds task to ready queue, may pend PendSV.
 *
 * Multiple waiters may be woken in one pass (e.g. two tasks both waiting
 * for "any of bit-0", or tasks waiting for independent bit subsets).
 * The loop re-reads event_group->events each iteration because a previous
 * CLEAR_ON_EXIT wakeup may have cleared bits that affect later entries.
 */
static void event_group_wake_waiters(event_group_t *eg) {
    tcb_t **pp = &eg->wait_queue;
    while (*pp != NULL) {
        tcb_t *t = *pp;
        if (event_condition_met(eg->events, t->event_bits, t->event_opts)) {
            /* Snapshot the matched bits for the waking task to read. */
            t->event_result = eg->events & t->event_bits;

            /* Auto-clear: only meaningful when waiting for bits to be SET.
             * Clearing bits that are already 0 (WAIT_CLEAR case) is a no-op
             * and would mislead readers, so we skip it. */
            if ((t->event_opts & EVENT_CLEAR_ON_EXIT) &&
                !(t->event_opts & EVENT_WAIT_CLEAR)) {
                eg->events &= ~t->event_bits;
            }

            /* Unlink from wait queue before waking (task->next will be
             * reused by the ready queue once os_task_wakeup enqueues it). */
            *pp = t->next;
            t->next = NULL;

            os_task_wakeup(t);   /* BLOCKED → READY; may trigger PendSV */
        } else {
            pp = &t->next;
        }
    }
}

/*===========================================================================
 * Event group public API
 *===========================================================================*/

void os_event_group_init(event_group_t *event_group) {
    if (event_group == NULL) return;
    event_group->events     = 0;
    event_group->wait_queue = NULL;
}

/**
 * Set one or more event bits and wake any tasks whose conditions are now met.
 *
 * Safe to call from both task and ISR context.
 * After setting bits, event_group_wake_waiters() is called inside the critical
 * section so all satisfied waiters are identified atomically.  PendSV is pended
 * inside os_task_wakeup() and fires after the critical section exits.
 */
os_error_t os_event_group_set_bits(event_group_t *event_group, uint32_t bits) {
    if (event_group == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t cs = os_enter_critical();

    event_group->events |= bits;

    /* Wake every waiter whose AND/OR condition is now satisfied. */
    event_group_wake_waiters(event_group);

    os_exit_critical(cs);
    return OS_OK;
}

/**
 * Clear one or more event bits.
 *
 * After clearing, event_group_wake_waiters() is called so that any task
 * blocking with EVENT_WAIT_CLEAR whose condition is now satisfied is woken.
 */
os_error_t os_event_group_clear_bits(event_group_t *event_group, uint32_t bits) {
    if (event_group == NULL) return OS_ERROR_INVALID_PARAM;

    uint32_t cs = os_enter_critical();
    event_group->events &= ~bits;
    /* Clearing bits may satisfy tasks waiting for bits to become CLEAR. */
    event_group_wake_waiters(event_group);
    os_exit_critical(cs);
    return OS_OK;
}

/**
 * Wait for event bits — AND/OR with true blocking.
 *
 * @param event_group      Event group to wait on.
 * @param bits_to_wait_for Bitmask of bits of interest.
 * @param options          Combination of:
 *                           EVENT_WAIT_ALL      — all bits must be set (AND)
 *                           EVENT_WAIT_ANY      — any bit suffices   (OR)
 *                           EVENT_CLEAR_ON_EXIT — clear the mask on wakeup
 * @param bits_received    Out: the event bits that triggered the wakeup
 *                         (subset of bits_to_wait_for, before any clearing).
 *                         May be NULL.
 * @param timeout          OS_WAIT_FOREVER (0): block indefinitely.
 *                         N > 0: spin-loop up to N ticks before returning
 *                         OS_ERROR_TIMEOUT.
 *
 * Fast path (condition already satisfied):  returns immediately, O(1).
 *
 * Slow path — OS_WAIT_FOREVER:
 *   Stores the wait condition in the calling task's TCB, sets state to
 *   BLOCKED, appends to the event group's wait_queue, then pends PendSV.
 *   The task is woken by os_event_group_set_bits() exactly when the
 *   condition becomes true — no polling, zero wasted CPU cycles.
 *
 * Slow path — finite timeout:
 *   Spin-loop with os_task_yield() until condition is met or timeout expires.
 */
os_error_t os_event_group_wait_bits(
    event_group_t *event_group,
    uint32_t       bits_to_wait_for,
    uint8_t        options,
    uint32_t      *bits_received,
    uint32_t       timeout
) {
    if (event_group == NULL || bits_to_wait_for == 0)
        return OS_ERROR_INVALID_PARAM;

    tcb_t *current = os_task_get_current();

    /* ── Fast path: check without blocking ───────────────────── */
    uint32_t cs = os_enter_critical();
    if (event_condition_met(event_group->events, bits_to_wait_for, options)) {
        uint32_t matched = event_group->events & bits_to_wait_for;
        if (options & EVENT_CLEAR_ON_EXIT)
            event_group->events &= ~bits_to_wait_for;
        os_exit_critical(cs);
        if (bits_received) *bits_received = matched;
        return OS_OK;
    }
    os_exit_critical(cs);

    /* ── Slow path ────────────────────────────────────────────── */

    if (timeout == OS_WAIT_FOREVER) {
        /*
         * True blocking wait.
         *
         * 1. Register this task's condition in its TCB.
         * 2. Append to the event group's wait queue.
         * 3. Set state to BLOCKED and yield — PendSV parks the task.
         * 4. os_event_group_set_bits() wakes us when condition is met,
         *    having already written event_result and done any clearing.
         * 5. After resuming, read event_result and return OS_OK.
         */
        cs = os_enter_critical();

        current->event_bits   = bits_to_wait_for;
        current->event_opts   = options;
        current->event_result = 0;

        /* Append to wait queue (simple FIFO; priority-sorted wakeup is
         * handled by the ready queue after os_task_wakeup). */
        tcb_t **pp = &event_group->wait_queue;
        while (*pp != NULL) pp = &(*pp)->next;
        *pp = current;
        current->next = NULL;

        current->state = TASK_STATE_BLOCKED;

        os_exit_critical(cs);

        /* Park this task; resumed by event_group_wake_waiters(). */
        os_task_yield();

        /* ── Resumed: condition was met ── */
        if (bits_received) *bits_received = current->event_result;
        return OS_OK;

    } else {
        /*
         * Timed spin-loop.
         * Yields each iteration so other tasks (including ones that will
         * set the bits) can run.
         */
        uint32_t start = os_get_tick_count();

        while (true) {
            cs = os_enter_critical();

            if (event_condition_met(event_group->events,
                                    bits_to_wait_for, options)) {
                uint32_t matched = event_group->events & bits_to_wait_for;
                if (options & EVENT_CLEAR_ON_EXIT)
                    event_group->events &= ~bits_to_wait_for;
                os_exit_critical(cs);
                if (bits_received) *bits_received = matched;
                return OS_OK;
            }

            os_exit_critical(cs);

            if ((os_get_tick_count() - start) >= timeout)
                return OS_ERROR_TIMEOUT;

            os_task_yield();
        }
    }
}

/**
 * Rendezvous (barrier) synchronization.
 *
 * Atomically sets @bits_to_set and waits (AND condition) until every bit in
 * @bits_to_wait_for is set.  Once the last participant arrives all blocked
 * tasks are released simultaneously.
 *
 * Bits are NOT cleared on exit — use os_event_group_clear_bits() afterwards
 * if the barrier needs to be reused.
 */
os_error_t os_event_group_sync(
    event_group_t *event_group,
    uint32_t bits_to_set,
    uint32_t bits_to_wait_for,
    uint32_t timeout
) {
    if (event_group == NULL || bits_to_wait_for == 0)
        return OS_ERROR_INVALID_PARAM;

    /* Set this task's arrival bit and wake any waiters that are now satisfied. */
    uint32_t cs = os_enter_critical();
    event_group->events |= bits_to_set;
    event_group_wake_waiters(event_group);
    os_exit_critical(cs);

    /* Wait for the full participant set (no auto-clear — every task must see it). */
    uint32_t dummy;
    return os_event_group_wait_bits(
        event_group,
        bits_to_wait_for,
        EVENT_WAIT_ALL,   /* AND: all participant bits must be set */
        &dummy,
        timeout
    );
}

/**
 * Read the current event bits without blocking or modifying them.
 */
uint32_t os_event_group_get_bits(event_group_t *event_group) {
    if (event_group == NULL) return 0;
    uint32_t cs = os_enter_critical();
    uint32_t bits = event_group->events;
    os_exit_critical(cs);
    return bits;
}

/**
 * Condition Variable Implementation
 */

/**
 * Remove a task from a condition variable's wait queue (must be called within critical section)
 */
static void cond_remove_task(cond_var_t *cond, tcb_t *task) {
    bool removed = false;
    if (cond->wait_queue == task) {
        cond->wait_queue = task->next;
        removed = true;
    } else {
        tcb_t *prev = cond->wait_queue;
        while (prev != NULL && prev->next != task) {
            prev = prev->next;
        }
        if (prev != NULL) {
            prev->next = task->next;
            removed = true;
        }
    }
    if (removed) {
        cond->waiting_count--;
    }
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

    /* ── OS_WAIT_FOREVER: true blocking ──────────────────────────────── */
    if (timeout == OS_WAIT_FOREVER) {
        /*
         * Atomically enqueue + set BLOCKED before releasing the mutex so
         * that a concurrent signal cannot be lost between unlock and sleep.
         *
         * Sequence:
         *   1. Critical section: add to wait_queue, set BLOCKED.
         *   2. Release critical section.
         *   3. Unlock mutex — os_mutex_unlock() yields internally; because
         *      state == BLOCKED, PendSV parks us until os_cond_signal/broadcast
         *      calls os_task_wakeup().
         *   4. On resume: re-acquire mutex and return.
         */
        uint32_t cs = os_enter_critical();

        /* FIFO append */
        tcb_t **pp = &cond->wait_queue;
        while (*pp != NULL) pp = &(*pp)->next;
        *pp = current_task;
        current_task->next = NULL;
        cond->waiting_count++;

        current_task->state = TASK_STATE_BLOCKED;

        os_exit_critical(cs);

        os_error_t unlock_result = os_mutex_unlock(mutex);
        if (unlock_result != OS_OK) {
            /* Unlock failed (caller doesn't own the mutex): undo enqueue.
             * cond_remove_task() already decrements waiting_count — do NOT
             * decrement again here. */
            cs = os_enter_critical();
            cond_remove_task(cond, current_task);
            current_task->state = TASK_STATE_RUNNING;
            os_exit_critical(cs);
            return unlock_result;
        }

        /* Parked here until os_cond_signal/broadcast calls os_task_wakeup(). */
        /* (os_mutex_unlock already yielded; we resume here after being woken.) */

        /* Re-acquire mutex before returning to caller */
        os_mutex_lock(mutex, OS_WAIT_FOREVER);
        return OS_OK;
    }

    /* ── Finite timeout: timed spin-poll ─────────────────────────────── */
    uint32_t state = os_enter_critical();

    /* Append to FIFO wait queue */
    tcb_t **pp = &cond->wait_queue;
    while (*pp != NULL) pp = &(*pp)->next;
    *pp = current_task;
    current_task->next = NULL;
    cond->waiting_count++;

    os_exit_critical(state);

    os_error_t unlock_result = os_mutex_unlock(mutex);
    if (unlock_result != OS_OK) {
        /* cond_remove_task() already decrements waiting_count. */
        state = os_enter_critical();
        cond_remove_task(cond, current_task);
        os_exit_critical(state);
        return unlock_result;
    }

    uint32_t start_tick = os_get_tick_count();

    while (true) {
        /* Check if signaled (removed from queue) */
        state = os_enter_critical();
        bool in_queue = false;
        for (tcb_t *t = cond->wait_queue; t != NULL; t = t->next) {
            if (t == current_task) { in_queue = true; break; }
        }
        os_exit_critical(state);

        if (!in_queue) {
            /* Signaled */
            os_mutex_lock(mutex, OS_WAIT_FOREVER);
            return OS_OK;
        }

        if ((os_get_tick_count() - start_tick) >= timeout) {
            /* cond_remove_task() already decrements waiting_count. */
            state = os_enter_critical();
            cond_remove_task(cond, current_task);
            os_exit_critical(state);
            os_mutex_lock(mutex, OS_WAIT_FOREVER);
            return OS_ERROR_TIMEOUT;
        }

        os_task_yield();
    }
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

    if (cond->wait_queue != NULL) {
        tcb_t *task_to_wake = cond->wait_queue;
        cond->wait_queue = task_to_wake->next;
        task_to_wake->next = NULL;
        cond->waiting_count--;
        os_task_wakeup(task_to_wake);   /* BLOCKED → READY; may pend PendSV */
    }

    os_exit_critical(state);

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
        os_task_wakeup(task_to_wake);   /* BLOCKED → READY; may pend PendSV */
    }

    os_exit_critical(state);

    return OS_OK;
}
