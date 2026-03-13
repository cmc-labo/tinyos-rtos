/**
 * TinyOS Software Timer Implementation
 *
 * Provides software timers with one-shot and auto-reload modes
 * Timers execute callbacks in interrupt context
 */

#include "tinyos.h"
#include <string.h>

/* Global timer list */
static struct {
    timer_t *active_timers;  /* Linked list of active timers */
    uint32_t timer_count;
} timer_manager;

/* External functions from kernel */
extern uint32_t os_get_tick_count(void);
extern uint32_t os_enter_critical(void);
extern void os_exit_critical(uint32_t state);

/**
 * Initialize timer manager (called by os_init)
 */
void os_timer_init(void) {
    timer_manager.active_timers = NULL;
    timer_manager.timer_count = 0;
}

/**
 * Create a software timer
 */
os_error_t os_timer_create(
    timer_t *timer,
    const char *name,
    timer_type_t type,
    uint32_t period_ms,
    timer_callback_t callback,
    void *callback_param
) {
    if (timer == NULL || callback == NULL || period_ms == 0) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* Initialize timer */
    memset(timer, 0, sizeof(timer_t));

    if (name != NULL) {
        strncpy(timer->name, name, sizeof(timer->name) - 1);
        timer->name[sizeof(timer->name) - 1] = '\0';
    }

    timer->type = type;
    timer->period = period_ms;  /* Store in milliseconds for now */
    timer->callback = callback;
    timer->callback_param = callback_param;
    timer->active = false;
    timer->next = NULL;

    os_exit_critical(state);

    return OS_OK;
}

/**
 * Insert timer into sorted active list (must be called within critical section)
 */
static void timer_insert_sorted(timer_t *timer) {
    if (timer_manager.active_timers == NULL ||
        timer->expire_time < timer_manager.active_timers->expire_time) {
        timer->next = timer_manager.active_timers;
        timer_manager.active_timers = timer;
    } else {
        timer_t *current = timer_manager.active_timers;
        while (current->next != NULL &&
               current->next->expire_time <= timer->expire_time) {
            current = current->next;
        }
        timer->next = current->next;
        current->next = timer;
    }
    timer_manager.timer_count++;
}

/**
 * Start a timer
 */
os_error_t os_timer_start(timer_t *timer) {
    if (timer == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    /* If timer is already active, remove it first */
    if (timer->active) {
        os_timer_stop(timer);
    }

    /* Calculate expiration time */
    timer->expire_time = os_get_tick_count() + timer->period;
    timer->active = true;

    timer_insert_sorted(timer);

    os_exit_critical(state);

    return OS_OK;
}

/**
 * Stop a timer
 */
os_error_t os_timer_stop(timer_t *timer) {
    if (timer == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    uint32_t state = os_enter_critical();

    if (!timer->active) {
        os_exit_critical(state);
        return OS_OK;  /* Already stopped */
    }

    /* Remove timer from active list */
    if (timer_manager.active_timers == timer) {
        /* Timer is at head */
        timer_manager.active_timers = timer->next;
    } else {
        /* Find and remove timer */
        timer_t *current = timer_manager.active_timers;
        while (current != NULL && current->next != timer) {
            current = current->next;
        }

        if (current != NULL) {
            current->next = timer->next;
        }
    }

    timer->active = false;
    timer->next = NULL;
    timer_manager.timer_count--;

    os_exit_critical(state);

    return OS_OK;
}

/**
 * Reset a timer (restart with same period)
 */
os_error_t os_timer_reset(timer_t *timer) {
    if (timer == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    /* Stop and restart timer */
    os_timer_stop(timer);
    return os_timer_start(timer);
}

/**
 * Delete a timer
 */
os_error_t os_timer_delete(timer_t *timer) {
    if (timer == NULL) {
        return OS_ERROR_INVALID_PARAM;
    }

    /* Stop timer if active */
    if (timer->active) {
        os_timer_stop(timer);
    }

    /* Clear timer structure */
    memset(timer, 0, sizeof(timer_t));

    return OS_OK;
}

/**
 * Change timer period
 */
os_error_t os_timer_change_period(timer_t *timer, uint32_t new_period_ms) {
    if (timer == NULL || new_period_ms == 0) {
        return OS_ERROR_INVALID_PARAM;
    }

    bool was_active = timer->active;

    /* Stop timer if active (os_timer_stop handles its own critical section) */
    if (was_active) {
        os_timer_stop(timer);
    }

    /* Update period */
    timer->period = new_period_ms;

    /* Restart if it was active (os_timer_start handles its own critical section) */
    if (was_active) {
        os_timer_start(timer);
    }

    return OS_OK;
}

/**
 * Check if timer is active
 */
bool os_timer_is_active(timer_t *timer) {
    if (timer == NULL) {
        return false;
    }

    return timer->active;
}

/**
 * Process timers (called from system tick interrupt)
 * This should be called from os_scheduler or timer interrupt
 */
void os_timer_process(void) {
    uint32_t current_time = os_get_tick_count();

    uint32_t state = os_enter_critical();

    timer_t *timer = timer_manager.active_timers;
    timer_t *prev = NULL;

    while (timer != NULL) {
        /* Check if timer has expired (signed subtraction handles tick wraparound) */
        if ((int32_t)(current_time - timer->expire_time) >= 0) {
            timer_t *expired_timer = timer;
            timer = timer->next;

            /* Remove from list */
            if (prev == NULL) {
                timer_manager.active_timers = timer;
            } else {
                prev->next = timer;
            }

            expired_timer->active = false;
            timer_manager.timer_count--;

            /* Exit critical section before callback */
            os_exit_critical(state);

            /* Execute callback */
            if (expired_timer->callback != NULL) {
                expired_timer->callback(expired_timer->callback_param);
            }

            /* Re-enter critical section */
            state = os_enter_critical();

            /* If auto-reload, restart timer */
            if (expired_timer->type == TIMER_AUTO_RELOAD) {
                expired_timer->expire_time = current_time + expired_timer->period;
                expired_timer->active = true;
                timer_insert_sorted(expired_timer);
                prev = NULL;
                timer = timer_manager.active_timers;
            }
        } else {
            /* Timers are sorted, so we can break early */
            break;
        }
    }

    os_exit_critical(state);
}

/**
 * Get remaining time until a timer fires (in milliseconds).
 * Returns 0 if the timer is not active or has already expired.
 */
uint32_t os_timer_get_remaining_ms(timer_t *timer) {
    if (timer == NULL || !timer->active) {
        return 0;
    }

    uint32_t state = os_enter_critical();
    uint32_t current_time = os_get_tick_count();
    uint32_t expire_time = timer->expire_time;
    os_exit_critical(state);

    /* Signed subtraction handles tick counter wraparound */
    int32_t remaining = (int32_t)(expire_time - current_time);
    return (remaining > 0) ? (uint32_t)remaining : 0;
}

/**
 * Get active timer count (for debugging)
 */
uint32_t os_timer_get_count(void) {
    return timer_manager.timer_count;
}
