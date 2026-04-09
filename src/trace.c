/**
 * @file trace.c
 * @brief TinyOS Trace Log — implementation
 *
 * Ring buffer layout
 * ------------------
 * write_idx always points to the NEXT slot to write.
 *
 * While stored < TRACE_BUFFER_SIZE (not yet wrapped):
 *   oldest event = buf[0],  event[i] = buf[i]
 *
 * Once stored == TRACE_BUFFER_SIZE (wrapped):
 *   oldest event = buf[write_idx],  event[i] = buf[(write_idx + i) % SIZE]
 *
 * All recording functions use a critical section so they are safe from both
 * task and interrupt (SysTick) context.
 */

#include "tinyos/trace.h"
#include <string.h>

/*===========================================================================
 * State
 *===========================================================================*/

static trace_event_t g_buf[TRACE_BUFFER_SIZE];
static uint32_t g_write_idx = 0;   /* next slot to write            */
static uint32_t g_stored    = 0;   /* events currently in buffer    */
static uint32_t g_total     = 0;   /* events ever recorded          */
static bool     g_enabled   = true;

/*===========================================================================
 * Internal helpers
 *===========================================================================*/

/* Copy at most (n-1) characters from src, always NUL-terminate. */
static void name_copy(char *dst, const char *src, size_t n) {
    size_t i = 0;
    if (src) {
        while (i < n - 1 && src[i] != '\0') {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* Write one event into the ring buffer (must be called inside critical section). */
static void push_event(trace_event_type_t type,
                        const char *task, const char *detail) {
    trace_event_t *e = &g_buf[g_write_idx];
    e->type = type;
    e->tick = os_get_tick_count();
    name_copy(e->task,   task   ? task   : "?", sizeof(e->task));
    name_copy(e->detail, detail ? detail : "?", sizeof(e->detail));

    g_write_idx = (g_write_idx + 1) % TRACE_BUFFER_SIZE;
    if (g_stored < TRACE_BUFFER_SIZE) g_stored++;
    g_total++;
}

/*===========================================================================
 * Recording
 *===========================================================================*/

void trace_record_switch(const char *from, const char *to) {
    if (!g_enabled) return;
    uint32_t cs = os_enter_critical();
    push_event(TRACE_TASK_SWITCH, from, to);
    os_exit_critical(cs);
}

void trace_record_syscall(const char *task, const char *call) {
    if (!g_enabled) return;
    uint32_t cs = os_enter_critical();
    push_event(TRACE_SYSCALL, task, call);
    os_exit_critical(cs);
}

/*===========================================================================
 * Control
 *===========================================================================*/

void trace_clear(void) {
    uint32_t cs = os_enter_critical();
    g_write_idx = 0;
    g_stored    = 0;
    g_total     = 0;
    os_exit_critical(cs);
}

void trace_enable(bool on) { g_enabled = on; }
bool trace_is_enabled(void) { return g_enabled; }

/*===========================================================================
 * Retrieval
 *===========================================================================*/

uint32_t trace_get_stored(void) { return g_stored; }
uint32_t trace_get_total(void)  { return g_total;  }

bool trace_get_event(uint32_t index, trace_event_t *out) {
    if (!out) return false;

    uint32_t cs = os_enter_critical();

    if (index >= g_stored) {
        os_exit_critical(cs);
        return false;
    }

    /* When not yet wrapped, events are contiguous from buf[0].
     * When wrapped, the oldest entry sits at buf[g_write_idx]. */
    uint32_t slot = (g_stored < TRACE_BUFFER_SIZE)
                  ? index
                  : (g_write_idx + index) % TRACE_BUFFER_SIZE;

    *out = g_buf[slot];
    os_exit_critical(cs);
    return true;
}
