/**
 * TinyOS Synchronization Primitives
 *
 * Implements mutexes, semaphores, and message queues
 * with priority inheritance to prevent priority inversion
 */

#include "tinyos.h"
#include <string.h>

/**
 * Initialize mutex
 */
void os_mutex_init(mutex_t *mutex) {
    if (mutex == NULL) return;

    mutex->locked = false;
    mutex->owner = NULL;
    mutex->ceiling_priority = PRIORITY_IDLE;
}

/**
 * Lock mutex with timeout
 * Implements Priority Inheritance Protocol to prevent priority inversion
 */
os_error_t os_mutex_lock(mutex_t *mutex, uint32_t timeout) {
    if (mutex == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    tcb_t *current_task = os_task_get_current();
    uint32_t start_tick = os_get_tick_count();

    while (true) {
        uint32_t state = os_enter_critical();

        if (!mutex->locked) {
            /* Acquire mutex */
            mutex->locked = true;
            mutex->owner = current_task;

            /* Priority ceiling protocol */
            if (mutex->owner->priority > mutex->ceiling_priority) {
                mutex->ceiling_priority = mutex->owner->priority;
            }

            os_exit_critical(state);
            return OS_OK;
        }

        /* Priority Inheritance: If owner has lower priority, boost it */
        if (mutex->owner != NULL && current_task->priority < mutex->owner->priority) {
            /* Temporarily raise the owner's priority to avoid priority inversion */
            os_task_raise_priority(mutex->owner, current_task->priority);
        }

        os_exit_critical(state);

        /* Check timeout */
        if (timeout != 0 && (os_get_tick_count() - start_tick) >= timeout) {
            return OS_ERROR_TIMEOUT;
        }

        /* Yield and try again */
        os_task_yield();
    }
}

/**
 * Unlock mutex
 * Resets priority inheritance if it was applied
 */
os_error_t os_mutex_unlock(mutex_t *mutex) {
    if (mutex == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    tcb_t *current_task = os_task_get_current();

    uint32_t state = os_enter_critical();

    /* Check ownership */
    if (mutex->owner != current_task) {
        os_exit_critical(state);
        return OS_ERROR_PERMISSION_DENIED;
    }

    /* Reset priority inheritance - restore base priority */
    os_task_reset_priority(current_task);

    /* Release mutex */
    mutex->locked = false;
    mutex->owner = NULL;

    os_exit_critical(state);

    /* Allow other tasks to run */
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
 * Post to semaphore (V operation)
 */
os_error_t os_semaphore_post(semaphore_t *sem) {
    if (sem == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();
    sem->count++;
    os_exit_critical(state);

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
        bool condition_met = false;

        if (wait_all) {
            /* Wait for ALL specified bits */
            condition_met = (current_events & bits_to_wait_for) == bits_to_wait_for;
        } else {
            /* Wait for ANY specified bit */
            condition_met = (current_events & bits_to_wait_for) != 0;
        }

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
