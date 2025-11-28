/**
 * TinyOS - Ultra-lightweight Real-Time Operating System for IoT
 *
 * Features:
 * - Minimal footprint (<10KB)
 * - Real-time scheduling (preemptive priority-based)
 * - Memory protection
 * - Secure by design
 *
 * Target: ARM Cortex-M, RISC-V, AVR
 */

#ifndef TINYOS_H
#define TINYOS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Version information */
#define TINYOS_VERSION_MAJOR    1
#define TINYOS_VERSION_MINOR    0
#define TINYOS_VERSION_PATCH    0

/* Configuration */
#define MAX_TASKS               8
#define STACK_SIZE              256
#define TICK_RATE_HZ            1000
#define TIME_SLICE_MS           10

/* Task priorities (0 = highest, 255 = lowest) */
typedef uint8_t task_priority_t;
#define PRIORITY_CRITICAL       0
#define PRIORITY_HIGH           64
#define PRIORITY_NORMAL         128
#define PRIORITY_LOW            192
#define PRIORITY_IDLE           255

/* Task states */
typedef enum {
    TASK_STATE_READY = 0,
    TASK_STATE_RUNNING,
    TASK_STATE_BLOCKED,
    TASK_STATE_SUSPENDED,
    TASK_STATE_TERMINATED
} task_state_t;

/* Task control block */
typedef struct task_control_block {
    uint32_t *stack_ptr;                /* Current stack pointer */
    uint32_t stack[STACK_SIZE];         /* Task stack */
    task_state_t state;                 /* Current state */
    task_priority_t priority;           /* Task priority */
    uint32_t time_slice;                /* Remaining time slice */
    char name[16];                      /* Task name */
    void (*entry_point)(void *);        /* Entry function */
    void *param;                        /* Task parameter */
    uint32_t run_time;                  /* Total run time (ticks) */
    struct task_control_block *next;    /* Next task in queue */
} tcb_t;

/* Return codes */
typedef enum {
    OS_OK = 0,
    OS_ERROR = -1,
    OS_ERROR_NO_MEMORY = -2,
    OS_ERROR_INVALID_PARAM = -3,
    OS_ERROR_TIMEOUT = -4,
    OS_ERROR_PERMISSION_DENIED = -5
} os_error_t;

/* Mutex for synchronization */
typedef struct {
    volatile bool locked;
    tcb_t *owner;
    task_priority_t ceiling_priority;
} mutex_t;

/* Semaphore */
typedef struct {
    volatile int32_t count;
    tcb_t *wait_queue;
} semaphore_t;

/* Message queue */
typedef struct {
    void *buffer;
    size_t item_size;
    size_t max_items;
    volatile size_t head;
    volatile size_t tail;
    volatile size_t count;
    mutex_t lock;
} msg_queue_t;

/*
 * Core OS API
 */

/* Initialize the OS */
void os_init(void);

/* Start the scheduler */
void os_start(void) __attribute__((noreturn));

/* Create a new task */
os_error_t os_task_create(
    tcb_t *task,
    const char *name,
    void (*entry)(void *),
    void *param,
    task_priority_t priority
);

/* Delete a task */
os_error_t os_task_delete(tcb_t *task);

/* Suspend a task */
os_error_t os_task_suspend(tcb_t *task);

/* Resume a task */
os_error_t os_task_resume(tcb_t *task);

/* Yield CPU to other tasks */
void os_task_yield(void);

/* Delay task execution */
void os_task_delay(uint32_t ticks);

/* Get current task */
tcb_t *os_task_get_current(void);

/*
 * Synchronization API
 */

/* Initialize mutex */
void os_mutex_init(mutex_t *mutex);

/* Lock mutex */
os_error_t os_mutex_lock(mutex_t *mutex, uint32_t timeout);

/* Unlock mutex */
os_error_t os_mutex_unlock(mutex_t *mutex);

/* Initialize semaphore */
void os_semaphore_init(semaphore_t *sem, int32_t initial_count);

/* Wait on semaphore */
os_error_t os_semaphore_wait(semaphore_t *sem, uint32_t timeout);

/* Post to semaphore */
os_error_t os_semaphore_post(semaphore_t *sem);

/*
 * Message Queue API
 */

/* Initialize message queue */
os_error_t os_queue_init(
    msg_queue_t *queue,
    void *buffer,
    size_t item_size,
    size_t max_items
);

/* Send message to queue */
os_error_t os_queue_send(
    msg_queue_t *queue,
    const void *item,
    uint32_t timeout
);

/* Receive message from queue */
os_error_t os_queue_receive(
    msg_queue_t *queue,
    void *item,
    uint32_t timeout
);

/*
 * Memory Management API
 */

/* Allocate memory */
void *os_malloc(size_t size);

/* Free memory */
void os_free(void *ptr);

/* Get free memory */
size_t os_get_free_memory(void);

/*
 * Time Management API
 */

/* Get system tick count */
uint32_t os_get_tick_count(void);

/* Get uptime in milliseconds */
uint32_t os_get_uptime_ms(void);

/*
 * Security API
 */

/* Memory region protection */
typedef struct {
    void *start_addr;
    size_t size;
    uint8_t permissions;  /* Read=1, Write=2, Execute=4 */
} memory_region_t;

/* Set memory protection */
os_error_t os_mpu_set_region(
    uint8_t region_id,
    memory_region_t *region
);

/* Enable/disable memory protection unit */
void os_mpu_enable(bool enable);

/*
 * Critical Section
 */

/* Enter critical section (disable interrupts) */
uint32_t os_enter_critical(void);

/* Exit critical section (restore interrupts) */
void os_exit_critical(uint32_t state);

/*
 * Statistics and Monitoring
 */

typedef struct {
    uint32_t total_tasks;
    uint32_t running_tasks;
    uint32_t blocked_tasks;
    uint32_t context_switches;
    uint32_t uptime_ticks;
    size_t free_memory;
    size_t used_memory;
} os_stats_t;

/* Get OS statistics */
void os_get_stats(os_stats_t *stats);

/* Get task CPU usage (0-100%) */
uint8_t os_task_get_cpu_usage(tcb_t *task);

#endif /* TINYOS_H */
