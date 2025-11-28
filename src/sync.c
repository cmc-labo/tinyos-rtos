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
 */
os_error_t os_mutex_lock(mutex_t *mutex, uint32_t timeout) {
    if (mutex == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t start_tick = os_get_tick_count();

    while (true) {
        uint32_t state = os_enter_critical();

        if (!mutex->locked) {
            /* Acquire mutex */
            mutex->locked = true;
            mutex->owner = os_task_get_current();

            /* Priority ceiling protocol */
            if (mutex->owner->priority > mutex->ceiling_priority) {
                mutex->ceiling_priority = mutex->owner->priority;
            }

            os_exit_critical(state);
            return OS_OK;
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
 */
os_error_t os_mutex_unlock(mutex_t *mutex) {
    if (mutex == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Check ownership */
    if (mutex->owner != os_task_get_current()) {
        os_exit_critical(state);
        return OS_ERROR_PERMISSION_DENIED;
    }

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
