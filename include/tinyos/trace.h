/**
 * @file trace.h
 * @brief TinyOS Trace Log — task-switch and syscall event recorder
 *
 * Records events into a fixed-size circular ring buffer.  The buffer
 * can be dumped at any time via the shell "trace" command or by calling
 * trace_get_event() directly.
 *
 * Configuration (override via compiler flags):
 *   TRACE_BUFFER_SIZE  — number of events kept in the ring buffer (default 64)
 *
 * Recorded automatically:
 *   TRACE_TASK_SWITCH  — every context switch (kernel.c)
 *   TRACE_SYSCALL      — mutex lock/unlock, semaphore wait/post (sync.c)
 *
 * Usage from application code:
 *   trace_record_syscall(task_name, "my_call");  // add custom events
 */

#ifndef TINYOS_TRACE_H
#define TINYOS_TRACE_H

#include "../tinyos.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Configuration
 *===========================================================================*/

#ifndef TRACE_BUFFER_SIZE
#define TRACE_BUFFER_SIZE  64   /**< Ring buffer capacity (events) */
#endif

/*===========================================================================
 * Event type
 *===========================================================================*/

typedef enum {
    TRACE_TASK_SWITCH = 0, /**< Context switch: task → task      */
    TRACE_SYSCALL,         /**< System call issued by a task     */
} trace_event_type_t;

/*===========================================================================
 * Event record (20 bytes per event)
 *===========================================================================*/

typedef struct {
    trace_event_type_t type;    /**< Event kind                              */
    uint32_t           tick;    /**< os_get_tick_count() at time of event    */
    char               task[9]; /**< Outgoing / issuing task name (max 8 ch) */
    char               detail[9]; /**< SWITCH: next task; SYSCALL: call name */
} trace_event_t;

/*===========================================================================
 * Recording API  (safe to call from any context, including ISR)
 *===========================================================================*/

/**
 * @brief Record a context switch event.
 * @param from  Name of the task being switched out.
 * @param to    Name of the task being switched in.
 */
void trace_record_switch(const char *from, const char *to);

/**
 * @brief Record a system-call event.
 * @param task  Name of the issuing task.
 * @param call  Short name of the system call (e.g. "mutex_lock").
 */
void trace_record_syscall(const char *task, const char *call);

/*===========================================================================
 * Control API
 *===========================================================================*/

/** Clear all recorded events and reset counters. */
void trace_clear(void);

/** Enable or disable event recording (default: enabled). */
void trace_enable(bool on);

/** @return true if recording is currently enabled. */
bool trace_is_enabled(void);

/*===========================================================================
 * Retrieval API
 *===========================================================================*/

/** @return Number of events currently stored in the ring buffer. */
uint32_t trace_get_stored(void);

/** @return Total events ever recorded (may exceed TRACE_BUFFER_SIZE). */
uint32_t trace_get_total(void);

/**
 * @brief Copy one event out of the buffer.
 * @param index  0 = oldest event in buffer, trace_get_stored()-1 = newest.
 * @param out    Destination struct (must not be NULL).
 * @return true on success, false if index is out of range.
 */
bool trace_get_event(uint32_t index, trace_event_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_TRACE_H */
