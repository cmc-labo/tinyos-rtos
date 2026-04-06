/**
 * @file posix_threads.c
 * @brief POSIX pthreads compatibility layer — implementation
 *
 * Wraps TinyOS task/sync primitives in the POSIX pthread API.
 *
 * Thread lifecycle
 * ----------------
 *   pthread_create  →  allocates a slot in pthread_pool[], initialises a
 *                      done_sem (count=0), creates a TinyOS task running
 *                      pthread_wrapper().
 *   pthread_wrapper →  calls the user fn, then calls pthread_exit() with
 *                      the return value.
 *   pthread_exit    →  stores retval, signals done_sem, then either marks
 *                      the slot free (detached) or suspends (joinable, waits
 *                      for the joiner to call os_task_delete).
 *   pthread_join    →  waits on done_sem, copies retval, deletes the task,
 *                      marks the slot free.
 */

#include "tinyos/posix_threads.h"
#include <string.h>
#include <errno.h>
#include <time.h>   /* struct timespec */

/*===========================================================================
 * Internal thread slot
 *===========================================================================*/

typedef struct {
    tcb_t        tcb;        /* TinyOS task control block                   */
    semaphore_t  done_sem;   /* signalled when the thread exits              */
    void        *retval;     /* value passed to pthread_exit()               */
    bool         detached;   /* true after pthread_detach()                  */
    bool         used;       /* slot is allocated                            */
    void        *(*fn)(void *); /* user thread entry point                  */
    void        *arg;        /* argument for fn                              */
} pthread_slot_t;

static pthread_slot_t pthread_pool[PTHREAD_MAX_THREADS];
static mutex_t        pool_mutex;
static bool           pool_inited = false;

/*===========================================================================
 * Pool helpers
 *===========================================================================*/

static void pool_init(void) {
    if (pool_inited) return;
    os_mutex_init(&pool_mutex);
    memset(pthread_pool, 0, sizeof(pthread_pool));
    pool_inited = true;
}

static int pool_alloc(void) {
    for (int i = 0; i < PTHREAD_MAX_THREADS; i++) {
        if (!pthread_pool[i].used) {
            pthread_pool[i].used = true;
            return i;
        }
    }
    return -1;
}

static bool pool_valid(pthread_t t) {
    return t < PTHREAD_MAX_THREADS && pthread_pool[t].used;
}

/*===========================================================================
 * Thread wrapper
 *===========================================================================*/

static void pthread_wrapper(void *arg) {
    uint8_t idx = (uint8_t)(uintptr_t)arg;
    pthread_slot_t *slot = &pthread_pool[idx];
    void *retval = slot->fn(slot->arg);
    pthread_exit(retval);   /* noreturn */
}

/*===========================================================================
 * pthread_attr_t
 *===========================================================================*/

int pthread_attr_init(pthread_attr_t *attr) {
    if (!attr) return EINVAL;
    attr->stacksize   = 0;                  /* 0 = use OS default */
    attr->detachstate = PTHREAD_CREATE_JOINABLE;
    attr->priority    = PRIORITY_NORMAL;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize) {
    if (!attr) return EINVAL;
    attr->stacksize = stacksize;
    return 0;
}

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize) {
    if (!attr || !stacksize) return EINVAL;
    *stacksize = attr->stacksize;
    return 0;
}

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate) {
    if (!attr) return EINVAL;
    if (detachstate != PTHREAD_CREATE_JOINABLE &&
        detachstate != PTHREAD_CREATE_DETACHED) return EINVAL;
    attr->detachstate = detachstate;
    return 0;
}

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate) {
    if (!attr || !detachstate) return EINVAL;
    *detachstate = attr->detachstate;
    return 0;
}

int tinyos_pthread_attr_setpriority(pthread_attr_t *attr,
                                     task_priority_t priority) {
    if (!attr) return EINVAL;
    attr->priority = priority;
    return 0;
}

/*===========================================================================
 * Mutex
 *===========================================================================*/

int pthread_mutexattr_init(pthread_mutexattr_t *attr) {
    if (!attr) return EINVAL;
    attr->type = PTHREAD_MUTEX_DEFAULT;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type) {
    if (!attr) return EINVAL;
    if (type == PTHREAD_MUTEX_RECURSIVE) return ENOTSUP;
    if (type != PTHREAD_MUTEX_DEFAULT)   return EINVAL;
    attr->type = type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type) {
    if (!attr || !type) return EINVAL;
    *type = attr->type;
    return 0;
}

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attr) {
    if (!mutex) return EINVAL;
    if (attr && attr->type == PTHREAD_MUTEX_RECURSIVE) return ENOTSUP;
    os_mutex_init(mutex);
    return 0;
}

int pthread_mutex_destroy(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    /* Destroying a locked mutex is undefined behaviour in POSIX;
     * here we just zero it out. */
    memset(mutex, 0, sizeof(*mutex));
    return 0;
}

int pthread_mutex_lock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    os_error_t r = os_mutex_lock(mutex, OS_WAIT_FOREVER);
    if (r == OS_OK) return 0;
    return (r == OS_ERROR_TIMEOUT) ? ETIMEDOUT : EIO;
}

int pthread_mutex_trylock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    /* Try with a minimal (1-tick) timeout to approximate non-blocking. */
    os_error_t r = os_mutex_lock(mutex, 1);
    if (r == OS_OK)             return 0;
    if (r == OS_ERROR_TIMEOUT)  return EBUSY;
    return EIO;
}

int pthread_mutex_unlock(pthread_mutex_t *mutex) {
    if (!mutex) return EINVAL;
    os_error_t r = os_mutex_unlock(mutex);
    return (r == OS_OK) ? 0 : EPERM;
}

/*===========================================================================
 * Condition variable
 *===========================================================================*/

int pthread_condattr_init(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_condattr_destroy(pthread_condattr_t *attr) {
    (void)attr;
    return 0;
}

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr) {
    if (!cond) return EINVAL;
    (void)attr;
    os_cond_init(cond);
    return 0;
}

int pthread_cond_destroy(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    memset(cond, 0, sizeof(*cond));
    return 0;
}

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) {
    if (!cond || !mutex) return EINVAL;
    os_error_t r = os_cond_wait(cond, mutex, OS_WAIT_FOREVER);
    return (r == OS_OK) ? 0 : EIO;
}

int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime) {
    if (!cond || !mutex || !abstime) return EINVAL;

    /* Convert absolute boot-time deadline to a relative timeout in ms.
     * os_get_uptime_ms() is our "clock" since TinyOS has no wall clock. */
    uint32_t now_ms  = os_get_uptime_ms();
    uint32_t dead_ms = (uint32_t)((uint64_t)abstime->tv_sec * 1000ULL +
                                  (uint64_t)abstime->tv_nsec / 1000000ULL);
    uint32_t timeout_ms = (dead_ms > now_ms) ? (dead_ms - now_ms) : 1;

    os_error_t r = os_cond_wait(cond, mutex, timeout_ms);
    if (r == OS_ERROR_TIMEOUT) { errno = ETIMEDOUT; return ETIMEDOUT; }
    return (r == OS_OK) ? 0 : EIO;
}

int pthread_cond_signal(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    os_error_t r = os_cond_signal(cond);
    return (r == OS_OK) ? 0 : EIO;
}

int pthread_cond_broadcast(pthread_cond_t *cond) {
    if (!cond) return EINVAL;
    os_error_t r = os_cond_broadcast(cond);
    return (r == OS_OK) ? 0 : EIO;
}

/*===========================================================================
 * Thread lifecycle
 *===========================================================================*/

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg) {
    if (!thread || !fn) return EINVAL;

    pool_init();

    /* Use defaults if no attribute block supplied. */
    pthread_attr_t defaults;
    if (!attr) {
        pthread_attr_init(&defaults);
        attr = &defaults;
    }

    /* Allocate a pool slot (critical section). */
    uint32_t cs = os_enter_critical();
    int idx = pool_alloc();
    os_exit_critical(cs);

    if (idx < 0) return EAGAIN;   /* EAGAIN: no threads available right now */

    pthread_slot_t *slot = &pthread_pool[idx];
    slot->fn       = fn;
    slot->arg      = arg;
    slot->retval   = NULL;
    slot->detached = (attr->detachstate == PTHREAD_CREATE_DETACHED);

    os_semaphore_init(&slot->done_sem, 0);

    task_priority_t prio = attr->priority ? attr->priority : PRIORITY_NORMAL;
    os_error_t r = os_task_create(&slot->tcb, "pthread",
                                   pthread_wrapper, (void *)(uintptr_t)(uint8_t)idx,
                                   prio);
    if (r != OS_OK) {
        slot->used = false;
        return (r == OS_ERROR_NO_MEMORY) ? EAGAIN : EIO;
    }

    *thread = (pthread_t)idx;
    return 0;
}

int pthread_join(pthread_t thread, void **retval) {
    pool_init();
    if (!pool_valid(thread)) return ESRCH;

    pthread_slot_t *slot = &pthread_pool[thread];
    if (slot->detached) return EINVAL;

    /* Block until the thread signals done_sem on exit. */
    os_semaphore_wait(&slot->done_sem, OS_WAIT_FOREVER);

    if (retval) *retval = slot->retval;

    /* Clean up: delete the (now suspended) task and free the slot. */
    os_task_delete(&slot->tcb);
    slot->used = false;
    return 0;
}

int pthread_detach(pthread_t thread) {
    pool_init();
    if (!pool_valid(thread)) return ESRCH;

    pthread_slot_t *slot = &pthread_pool[thread];
    if (slot->detached) return EINVAL;

    uint32_t cs = os_enter_critical();
    slot->detached = true;
    os_exit_critical(cs);
    return 0;
}

__attribute__((noreturn))
void pthread_exit(void *retval) {
    pool_init();

    /* Find the slot belonging to the calling task. */
    tcb_t *self = os_task_get_current();
    for (int i = 0; i < PTHREAD_MAX_THREADS; i++) {
        pthread_slot_t *slot = &pthread_pool[i];
        if (!slot->used) continue;
        if (&slot->tcb != self) continue;

        slot->retval = retval;
        os_semaphore_post(&slot->done_sem);

        if (slot->detached) {
            /* Joiner will never come; free the slot now. */
            slot->used = false;
            os_task_delete(NULL);   /* delete self */
        } else {
            /* Suspend and wait for pthread_join to call os_task_delete. */
            os_task_suspend(NULL);
        }
        break;
    }

    /* Fallback: should not reach here — suspend if slot not found. */
    os_task_suspend(NULL);
    for (;;) { os_task_yield(); }   /* silence noreturn warning */
}

pthread_t pthread_self(void) {
    pool_init();
    tcb_t *self = os_task_get_current();
    for (int i = 0; i < PTHREAD_MAX_THREADS; i++) {
        if (pthread_pool[i].used && &pthread_pool[i].tcb == self)
            return (pthread_t)i;
    }
    return PTHREAD_T_INVALID;
}

int pthread_equal(pthread_t t1, pthread_t t2) {
    return (t1 == t2) ? 1 : 0;
}

int pthread_yield(void) {
    os_task_yield();
    return 0;
}
