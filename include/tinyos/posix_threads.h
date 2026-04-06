/**
 * @file posix_threads.h
 * @brief POSIX pthreads compatibility layer for TinyOS
 *
 * Maps a subset of the POSIX pthread API onto TinyOS task and
 * synchronisation primitives, allowing code written against standard
 * pthreads headers to be compiled for TinyOS with minimal changes.
 *
 * Supported:
 *   Threads  : pthread_create, pthread_join, pthread_detach,
 *              pthread_exit, pthread_self, pthread_equal
 *   Attributes: pthread_attr_init/destroy/setstacksize/setdetachstate/
 *               setschedparam (priority), tinyos_pthread_attr_setpriority
 *   Mutex    : pthread_mutex_init/destroy/lock/trylock/unlock,
 *              PTHREAD_MUTEX_INITIALIZER
 *   Cond var : pthread_cond_init/destroy/wait/timedwait/signal/broadcast,
 *              PTHREAD_COND_INITIALIZER
 *
 * Not supported (returns ENOSYS):
 *   pthread_cancel, thread-local storage (pthread_key_*),
 *   realtime scheduling classes beyond priority mapping.
 *
 * Usage:
 *   #include "tinyos/posix_threads.h"
 *
 *   void *worker(void *arg) { ... return NULL; }
 *
 *   pthread_t tid;
 *   pthread_create(&tid, NULL, worker, NULL);
 *   pthread_join(tid, NULL);
 */

#ifndef TINYOS_POSIX_THREADS_H
#define TINYOS_POSIX_THREADS_H

#include "../tinyos.h"
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Configuration
 *===========================================================================*/

/** Maximum number of concurrent pthreads (must not exceed MAX_TASKS). */
#ifndef PTHREAD_MAX_THREADS
#define PTHREAD_MAX_THREADS  MAX_TASKS
#endif

/*===========================================================================
 * pthread_t  — opaque thread handle (index into internal pool)
 *===========================================================================*/

typedef uint8_t pthread_t;

#define PTHREAD_T_INVALID  ((pthread_t)0xFF)

/*===========================================================================
 * pthread_attr_t
 *===========================================================================*/

#define PTHREAD_CREATE_JOINABLE  0
#define PTHREAD_CREATE_DETACHED  1

typedef struct {
    size_t          stacksize;    /**< Stack size in bytes (0 = OS default) */
    int             detachstate;  /**< PTHREAD_CREATE_JOINABLE / _DETACHED   */
    task_priority_t priority;     /**< TinyOS priority (0=highest, 255=lowest) */
} pthread_attr_t;

int pthread_attr_init(pthread_attr_t *attr);
int pthread_attr_destroy(pthread_attr_t *attr);
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize);
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize);
int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate);
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate);

/**
 * @brief TinyOS extension: set thread priority directly.
 * @param priority  TinyOS priority value (0 = highest, 255 = lowest).
 *                  Use the PRIORITY_* macros defined in tinyos.h.
 */
int tinyos_pthread_attr_setpriority(pthread_attr_t *attr,
                                     task_priority_t priority);

/*===========================================================================
 * Mutex
 *===========================================================================*/

/** pthread_mutex_t embeds TinyOS mutex_t directly — zero-init is valid. */
typedef mutex_t pthread_mutex_t;

/** Mutex attributes (only the type field is inspected; others are ignored). */
typedef struct {
    int type;   /**< PTHREAD_MUTEX_DEFAULT (0) */
} pthread_mutexattr_t;

#define PTHREAD_MUTEX_DEFAULT    0
#define PTHREAD_MUTEX_RECURSIVE  1   /* Not supported; init returns ENOTSUP */

#define PTHREAD_MUTEX_INITIALIZER \
    { .locked = false, .owner = NULL, .ceiling_priority = PRIORITY_IDLE }

int pthread_mutex_init(pthread_mutex_t *mutex,
                       const pthread_mutexattr_t *attr);
int pthread_mutex_destroy(pthread_mutex_t *mutex);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_trylock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

int pthread_mutexattr_init(pthread_mutexattr_t *attr);
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr);
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type);
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type);

/*===========================================================================
 * Condition variable
 *===========================================================================*/

/** pthread_cond_t embeds TinyOS cond_var_t directly — zero-init is valid. */
typedef cond_var_t pthread_cond_t;

typedef struct {
    int _unused;
} pthread_condattr_t;

#define PTHREAD_COND_INITIALIZER \
    { .wait_queue = NULL, .waiting_count = 0 }

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);

/**
 * @brief Wait on a condition variable with an absolute timeout.
 *
 * @param abstime  Absolute CLOCK_REALTIME deadline.  Since TinyOS has no
 *                 wall clock, the seconds field is interpreted as seconds
 *                 since boot (i.e., os_get_uptime_ms() / 1000).
 */
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex,
                           const struct timespec *abstime);

int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_broadcast(pthread_cond_t *cond);

int pthread_condattr_init(pthread_condattr_t *attr);
int pthread_condattr_destroy(pthread_condattr_t *attr);

/*===========================================================================
 * Thread lifecycle
 *===========================================================================*/

/**
 * @brief Create a new thread.
 *
 * @param thread   Output: thread handle.
 * @param attr     Thread attributes (NULL = defaults).
 * @param fn       Thread entry function.
 * @param arg      Argument passed to @p fn.
 * @return 0 on success, or a positive errno value on failure.
 */
int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*fn)(void *), void *arg);

/**
 * @brief Wait for a thread to terminate and collect its exit value.
 *
 * @param thread   Thread to join (must not be detached).
 * @param retval   If non-NULL, receives the value passed to pthread_exit().
 * @return 0 on success, or EINVAL / ESRCH.
 */
int pthread_join(pthread_t thread, void **retval);

/**
 * @brief Detach a thread so its resources are freed automatically on exit.
 * @return 0 on success, or EINVAL / ESRCH.
 */
int pthread_detach(pthread_t thread);

/**
 * @brief Terminate the calling thread.
 * @note  This function does not return.
 */
__attribute__((noreturn))
void pthread_exit(void *retval);

/** @return Handle of the calling thread (PTHREAD_T_INVALID if not a pthread). */
pthread_t pthread_self(void);

/** @return Non-zero if @p t1 and @p t2 refer to the same thread. */
int pthread_equal(pthread_t t1, pthread_t t2);

/**
 * @brief Yield the processor to another thread of the same or higher priority.
 */
int pthread_yield(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_POSIX_THREADS_H */
