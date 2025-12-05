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
#define TINYOS_VERSION_MINOR    2
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
    task_priority_t base_priority;      /* Base priority (for inheritance) */
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

/* Event group */
typedef struct {
    volatile uint32_t events;     /* Current event bits */
    tcb_t *wait_queue;            /* Tasks waiting for events */
} event_group_t;

/* Event group wait options */
#define EVENT_WAIT_ALL    0x01    /* Wait for all specified bits */
#define EVENT_WAIT_ANY    0x02    /* Wait for any specified bit */
#define EVENT_CLEAR_ON_EXIT 0x04  /* Clear bits after waking up */

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

/* Get task priority */
task_priority_t os_task_get_priority(tcb_t *task);

/* Set task priority (dynamic priority adjustment) */
os_error_t os_task_set_priority(tcb_t *task, task_priority_t new_priority);

/* Raise task priority temporarily (returns to base priority when released) */
os_error_t os_task_raise_priority(tcb_t *task, task_priority_t new_priority);

/* Reset task to base priority */
os_error_t os_task_reset_priority(tcb_t *task);

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
 * Event Group API
 */

/* Initialize event group */
void os_event_group_init(event_group_t *event_group);

/* Set event bits */
os_error_t os_event_group_set_bits(event_group_t *event_group, uint32_t bits);

/* Clear event bits */
os_error_t os_event_group_clear_bits(event_group_t *event_group, uint32_t bits);

/* Wait for event bits */
os_error_t os_event_group_wait_bits(
    event_group_t *event_group,
    uint32_t bits_to_wait_for,
    uint8_t options,
    uint32_t *bits_received,
    uint32_t timeout
);

/* Get current event bits */
uint32_t os_event_group_get_bits(event_group_t *event_group);

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
 * Software Timer API
 */

/* Timer types */
typedef enum {
    TIMER_ONE_SHOT = 0,    /* Timer fires once */
    TIMER_AUTO_RELOAD      /* Timer automatically reloads */
} timer_type_t;

/* Timer callback function */
typedef void (*timer_callback_t)(void *param);

/* Timer control block */
typedef struct software_timer {
    char name[16];                    /* Timer name */
    timer_type_t type;                /* Timer type */
    uint32_t period;                  /* Timer period in ticks */
    uint32_t expire_time;             /* When timer expires */
    bool active;                      /* Timer is active */
    timer_callback_t callback;        /* Callback function */
    void *callback_param;             /* Callback parameter */
    struct software_timer *next;      /* Next timer in list */
} timer_t;

/* Create a software timer */
os_error_t os_timer_create(
    timer_t *timer,
    const char *name,
    timer_type_t type,
    uint32_t period_ms,
    timer_callback_t callback,
    void *callback_param
);

/* Start a timer */
os_error_t os_timer_start(timer_t *timer);

/* Stop a timer */
os_error_t os_timer_stop(timer_t *timer);

/* Reset a timer */
os_error_t os_timer_reset(timer_t *timer);

/* Delete a timer */
os_error_t os_timer_delete(timer_t *timer);

/* Change timer period */
os_error_t os_timer_change_period(timer_t *timer, uint32_t new_period_ms);

/* Check if timer is active */
bool os_timer_is_active(timer_t *timer);

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

/*
 * Power Management API
 */

/* Power modes */
typedef enum {
    POWER_MODE_ACTIVE = 0,      /* Normal operation */
    POWER_MODE_IDLE,            /* CPU stopped, peripherals running */
    POWER_MODE_SLEEP,           /* CPU and most peripherals stopped */
    POWER_MODE_DEEP_SLEEP,      /* Minimal power, RTC only */
    POWER_MODE_MAX
} power_mode_t;

/* Wakeup sources */
typedef enum {
    WAKEUP_SOURCE_RTC = 0,      /* Real-time clock */
    WAKEUP_SOURCE_GPIO,         /* GPIO interrupt */
    WAKEUP_SOURCE_UART,         /* UART activity */
    WAKEUP_SOURCE_TIMER,        /* Timer interrupt */
    WAKEUP_SOURCE_I2C,          /* I2C activity */
    WAKEUP_SOURCE_SPI,          /* SPI activity */
    WAKEUP_SOURCE_ADC,          /* ADC conversion complete */
    WAKEUP_SOURCE_USB,          /* USB activity */
    WAKEUP_SOURCE_MAX
} wakeup_source_t;

/* Power configuration */
typedef struct {
    bool idle_mode_enabled;             /* Enable idle mode */
    bool sleep_mode_enabled;            /* Enable sleep modes */
    uint32_t deep_sleep_threshold_ms;   /* Min time for deep sleep */
    uint32_t cpu_freq_hz;               /* CPU frequency */
    uint32_t min_cpu_freq_hz;           /* Minimum CPU frequency */
    uint32_t max_cpu_freq_hz;           /* Maximum CPU frequency */
    uint32_t battery_capacity_mah;      /* Battery capacity */
    uint32_t battery_voltage_mv;        /* Battery voltage */
} power_config_t;

/* Power statistics */
typedef struct {
    power_mode_t current_mode;          /* Current power mode */
    uint32_t total_sleep_time_ms;       /* Total time in sleep modes */
    uint32_t total_active_time_ms;      /* Total active time */
    uint32_t wakeup_sources;            /* Enabled wakeup sources (bitmap) */
    uint32_t power_consumption_mw;      /* Current power consumption */
    uint32_t estimated_battery_life_hours; /* Estimated battery life */
} power_stats_t;

/* Power mode callback */
typedef void (*power_callback_t)(power_mode_t mode);

/* Initialize power management */
void os_power_init(void);

/* Configure power management */
os_error_t os_power_configure(const power_config_t *config);

/* Set power mode */
os_error_t os_power_set_mode(power_mode_t mode);

/* Get current power mode */
power_mode_t os_power_get_mode(void);

/* Enter idle mode (called automatically by idle task) */
void os_power_enter_idle(void);

/* Enter sleep mode for specified duration */
os_error_t os_power_enter_sleep(uint32_t duration_ms);

/* Enter deep sleep mode for specified duration */
os_error_t os_power_enter_deep_sleep(uint32_t duration_ms);

/* Enable/disable tickless idle mode */
os_error_t os_power_enable_tickless_idle(bool enable);

/* Check if tickless idle is enabled */
bool os_power_is_tickless_idle_enabled(void);

/* Register power mode callbacks */
os_error_t os_power_register_callback(
    power_callback_t enter_callback,
    power_callback_t exit_callback
);

/* Configure wakeup sources */
os_error_t os_power_configure_wakeup(wakeup_source_t source, bool enable);

/* Get power statistics */
void os_power_get_stats(power_stats_t *stats);

/* Set CPU frequency */
os_error_t os_power_set_cpu_frequency(uint32_t freq_hz);

/* Get current power consumption (milliwatts) */
uint32_t os_power_get_consumption_mw(void);

/* Estimate battery life remaining (hours) */
uint32_t os_power_estimate_battery_life_hours(void);

/*
 * File System API
 */

/* File system configuration */
#define FS_MAX_OPEN_FILES       8       /* Maximum open files */
#define FS_MAX_PATH_LENGTH      128     /* Maximum path length */
#define FS_MAX_FILENAME_LENGTH  32      /* Maximum filename length */
#define FS_BLOCK_SIZE           512     /* File system block size */
#define FS_MAX_BLOCKS           1024    /* Maximum number of blocks */

/* File open flags */
#define FS_O_RDONLY             0x01    /* Read-only */
#define FS_O_WRONLY             0x02    /* Write-only */
#define FS_O_RDWR               0x03    /* Read-write */
#define FS_O_CREAT              0x04    /* Create if not exists */
#define FS_O_TRUNC              0x08    /* Truncate to zero length */
#define FS_O_APPEND             0x10    /* Append mode */

/* Seek origins */
#define FS_SEEK_SET             0       /* Beginning of file */
#define FS_SEEK_CUR             1       /* Current position */
#define FS_SEEK_END             2       /* End of file */

/* File types */
#define FS_TYPE_REGULAR         1       /* Regular file */
#define FS_TYPE_DIRECTORY       2       /* Directory */

/* File descriptor */
typedef int32_t fs_file_t;
#define FS_INVALID_FD           (-1)

/* Directory entry */
typedef struct {
    char name[FS_MAX_FILENAME_LENGTH];
    uint8_t type;                       /* FS_TYPE_REGULAR or FS_TYPE_DIRECTORY */
    uint32_t size;                      /* File size in bytes */
    uint32_t mtime;                     /* Modification time (timestamp) */
} fs_dirent_t;

/* File statistics */
typedef struct {
    uint8_t type;                       /* File type */
    uint32_t size;                      /* File size in bytes */
    uint32_t blocks;                    /* Number of blocks used */
    uint32_t mtime;                     /* Modification time */
    uint32_t ctime;                     /* Creation time */
} fs_stat_t;

/* Directory handle */
typedef struct fs_dir_s *fs_dir_t;

/* File system statistics */
typedef struct {
    uint32_t total_blocks;              /* Total blocks */
    uint32_t used_blocks;               /* Used blocks */
    uint32_t free_blocks;               /* Free blocks */
    uint32_t total_files;               /* Total files */
    uint32_t total_dirs;                /* Total directories */
    uint32_t block_size;                /* Block size in bytes */
} fs_stats_t;

/* Block device operations (storage abstraction) */
typedef struct {
    /* Read blocks from storage */
    int (*read)(uint32_t block, void *buffer, uint32_t count);

    /* Write blocks to storage */
    int (*write)(uint32_t block, const void *buffer, uint32_t count);

    /* Erase blocks (for flash memory) */
    int (*erase)(uint32_t block, uint32_t count);

    /* Sync/flush data to storage */
    int (*sync)(void);

    /* Get total block count */
    uint32_t (*get_block_count)(void);
} fs_block_device_t;

/* Initialize file system */
os_error_t fs_init(void);

/* Format storage device */
os_error_t fs_format(fs_block_device_t *device);

/* Mount file system */
os_error_t fs_mount(fs_block_device_t *device);

/* Unmount file system */
os_error_t fs_unmount(void);

/* Check if file system is mounted */
bool fs_is_mounted(void);

/* File operations */

/* Open file */
fs_file_t fs_open(const char *path, uint32_t flags);

/* Close file */
os_error_t fs_close(fs_file_t fd);

/* Read from file */
int32_t fs_read(fs_file_t fd, void *buffer, size_t size);

/* Write to file */
int32_t fs_write(fs_file_t fd, const void *buffer, size_t size);

/* Seek to position */
int32_t fs_seek(fs_file_t fd, int32_t offset, int whence);

/* Get current position */
int32_t fs_tell(fs_file_t fd);

/* Get file size */
int32_t fs_size(fs_file_t fd);

/* Sync file data to storage */
os_error_t fs_sync(fs_file_t fd);

/* Truncate file */
os_error_t fs_truncate(fs_file_t fd, uint32_t size);

/* Remove file */
os_error_t fs_remove(const char *path);

/* Rename file */
os_error_t fs_rename(const char *old_path, const char *new_path);

/* Get file statistics */
os_error_t fs_stat(const char *path, fs_stat_t *stat);

/* Directory operations */

/* Create directory */
os_error_t fs_mkdir(const char *path);

/* Remove directory */
os_error_t fs_rmdir(const char *path);

/* Open directory */
fs_dir_t fs_opendir(const char *path);

/* Read directory entry */
os_error_t fs_readdir(fs_dir_t dir, fs_dirent_t *entry);

/* Close directory */
os_error_t fs_closedir(fs_dir_t dir);

/* File system information */

/* Get file system statistics */
os_error_t fs_get_stats(fs_stats_t *stats);

/* Get free space in bytes */
uint32_t fs_get_free_space(void);

/* Get total space in bytes */
uint32_t fs_get_total_space(void);

#endif /* TINYOS_H */
