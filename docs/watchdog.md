# Watchdog Timer Guide

## Quick Start

Here's a minimal example to get started with the watchdog timer:

```c
#include "tinyos.h"
#include "tinyos/watchdog.h"

/* Watchdog callback (optional) */
void my_wdt_callback(wdt_reset_reason_t reason, tcb_t *task) {
    printf("Watchdog timeout! Reason: %s\n", wdt_reset_reason_to_string(reason));
}

/* Task that feeds the watchdog */
void feeder_task(void *param) {
    while (1) {
        os_task_delay(500);  /* Wait 500ms */
        wdt_feed();          /* Feed watchdog */
    }
}

int main(void) {
    /* Initialize OS */
    os_init();

    /* Quick watchdog initialization (2 second timeout) */
    WDT_QUICK_INIT(WDT_TIMEOUT_2S, my_wdt_callback);

    /* Create feeder task */
    static tcb_t feeder;
    os_task_create(&feeder, "feeder", feeder_task, NULL, PRIORITY_HIGH);

    /* Start OS */
    os_start();
    return 0;
}
```

That's it! The watchdog will reset your system if not fed within 2 seconds.

### Common Usage Patterns

**1. Simple Watchdog with Convenience Macros:**

```c
/* Initialize with default settings */
WDT_QUICK_INIT(WDT_TIMEOUT_2S, NULL);

/* Safe feed (checks if initialized) */
WDT_SAFE_FEED();

/* Calculate recommended feed interval */
uint32_t feed_interval = WDT_FEED_INTERVAL(2000);  /* Returns 1000ms */
```

**2. Monitor a Specific Task:**

```c
static tcb_t my_task;

/* Create and register task */
os_task_create(&my_task, "worker", worker_func, NULL, PRIORITY_NORMAL);
WDT_REGISTER_TASK(my_task, 1000);  /* 1 second timeout */

/* In task function */
void worker_func(void *param) {
    while (1) {
        do_work();
        WDT_FEED_TASK(my_task);  /* Feed task watchdog */
        os_task_delay(500);
    }
}
```

**3. Debug Mode (No Reset):**

```c
/* Use debug config - logs timeout but doesn't reset */
wdt_config_t config = WDT_DEBUG_CONFIG(WDT_TIMEOUT_5S, my_callback);
wdt_init(&config);

/* Enable debug tracing (compile with -DWDT_DEBUG) */
WDT_DEBUG_FEED();         /* Traces file:line when feeding */
WDT_DEBUG_STATUS();       /* Prints current status */
```

**4. Production Mode (Aggressive):**

```c
/* Production config - automatic reset on any timeout */
wdt_config_t config = WDT_PRODUCTION_CONFIG(WDT_TIMEOUT_2S, save_and_reset);
wdt_init(&config);
```

## Overview

The Watchdog Timer (WDT) module provides system and task monitoring capabilities to detect and recover from software hangs or crashes. TinyOS supports both hardware and software watchdog timers.

## Features

- **Hardware Watchdog**: Platform-specific hardware timer that resets the system if not fed periodically
- **Software Watchdog**: OS-level monitoring for individual tasks
- **Task Monitoring**: Per-task watchdog with individual timeout settings
- **Automatic Recovery**: Configurable automatic system reset on timeout
- **Pre-Reset Callback**: Execute cleanup code before reset
- **Statistics**: Track feeds, timeouts, and reset causes

## Architecture

```
┌─────────────────────────────────────┐
│         Application Tasks           │
│  (Call wdt_feed_task periodically)  │
└──────────────┬──────────────────────┘
               │
┌──────────────▼──────────────────────┐
│      Watchdog Timer Module          │
│  - Task Registry                    │
│  - Timeout Tracking                 │
│  - Statistics                       │
└──────────────┬──────────────────────┘
               │
       ┌───────┴───────┐
       │               │
┌──────▼──────┐ ┌─────▼─────┐
│  Software   │ │ Hardware  │
│  Watchdog   │ │ Watchdog  │
└─────────────┘ └───────────┘
```

## Basic Usage

### 1. Initialization

```c
#include "tinyos/watchdog.h"

/* Configure watchdog */
wdt_config_t config = {
    .type = WDT_TYPE_BOTH,          /* Use both HW and SW watchdog */
    .timeout_ms = 2000,             /* 2 second timeout */
    .auto_start = true,             /* Start automatically */
    .enable_reset = true,           /* Enable automatic reset */
    .callback = my_watchdog_callback /* Pre-reset callback */
};

/* Initialize */
wdt_error_t err = wdt_init(&config);
if (err != WDT_OK) {
    printf("Watchdog init failed: %s\n", wdt_error_to_string(err));
}
```

### 2. System Watchdog

The system watchdog must be fed periodically by any task:

```c
void watchdog_feeder_task(void *param) {
    while (1) {
        /* Feed watchdog every 500ms */
        os_task_delay(500);
        wdt_feed();
    }
}
```

### 3. Task Watchdog

Monitor individual tasks to detect hangs:

```c
/* Register task for monitoring */
wdt_register_task(&my_task, 1000);  /* 1 second timeout */

/* In task function */
void my_task_func(void *param) {
    while (1) {
        /* Do work */
        process_data();

        /* Feed task watchdog */
        wdt_feed_task(&my_task);

        os_task_delay(100);
    }
}
```

## Configuration Options

### Watchdog Types

```c
typedef enum {
    WDT_TYPE_HARDWARE,   /* Hardware watchdog only */
    WDT_TYPE_SOFTWARE,   /* Software watchdog only */
    WDT_TYPE_BOTH        /* Both (recommended) */
} wdt_type_t;
```

### Timeout Limits

- **Minimum**: 100ms (`WDT_MIN_TIMEOUT_MS`)
- **Maximum**: 60000ms / 60s (`WDT_MAX_TIMEOUT_MS`)

### Pre-Reset Callback

```c
void my_watchdog_callback(wdt_reset_reason_t reason, tcb_t *task) {
    /* Save critical data */
    save_system_state();

    /* Log reset reason */
    log_error("Watchdog timeout: %s", wdt_reset_reason_to_string(reason));

    if (task) {
        log_error("Faulty task: %p", (void*)task);
    }

    /* Cleanup */
    close_files();
}
```

## Advanced Features

### Dynamic Timeout Adjustment

```c
/* Change global timeout */
wdt_set_timeout(5000);  /* 5 seconds */

/* Get current timeout */
uint32_t timeout = wdt_get_timeout();
```

### Task Control

```c
/* Enable/disable task watchdog */
wdt_enable_task(&my_task);
wdt_disable_task(&my_task);

/* Unregister task */
wdt_unregister_task(&my_task);

/* Check registration */
if (wdt_is_task_registered(&my_task)) {
    printf("Task is monitored\n");
}
```

### Status and Statistics

```c
/* Get status */
wdt_status_t status;
wdt_get_status(&status);
printf("Time remaining: %lu ms\n", status.time_remaining_ms);

/* Get statistics */
wdt_stats_t stats;
wdt_get_stats(&stats);
printf("Total feeds: %lu\n", stats.total_feeds);
printf("Total timeouts: %lu\n", stats.total_timeouts);

/* Print info */
wdt_print_status();
wdt_print_stats();
wdt_print_registered_tasks();
```

### Reset Detection

```c
int main(void) {
    /* Check if last reset was caused by watchdog */
    if (wdt_was_reset_by_watchdog()) {
        printf("System recovered from watchdog reset\n");
        wdt_clear_reset_flag();

        /* Take corrective action */
        enter_safe_mode();
    }

    /* Continue initialization */
    os_init();
    wdt_init(&config);
    // ...
}
```

## Best Practices

### 1. Feed Frequency

Feed the watchdog at **50-75%** of the timeout interval:

```c
/* For 2000ms timeout */
void feeder_task(void *param) {
    while (1) {
        os_task_delay(1000);  /* Feed at 50% interval */
        wdt_feed();
    }
}
```

### 2. High Priority Feeder

Give the watchdog feeder task high priority:

```c
os_task_create(
    &feeder_task,
    "wdt_feeder",
    feeder_func,
    NULL,
    PRIORITY_HIGH  /* Ensure it runs */
);
```

### 3. Monitor Critical Tasks

Only monitor time-critical tasks:

```c
/* Monitor sensor reading task */
wdt_register_task(&sensor_task, 500);

/* Don't monitor UI tasks (they may be blocked by user input) */
```

### 4. Graceful Degradation

Use callback for cleanup before reset:

```c
void watchdog_callback(wdt_reset_reason_t reason, tcb_t *task) {
    /* Save sensor data */
    save_to_flash();

    /* Log event */
    log_watchdog_reset(reason, task);

    /* Send alert */
    send_alert_message();
}
```

### 5. Debug vs Production

```c
#ifdef DEBUG
    /* Disable reset in debug mode */
    config.enable_reset = false;
#else
    /* Enable reset in production */
    config.enable_reset = true;
#endif
```

## Platform Integration

### Hardware Watchdog HAL

Implement these functions for your platform:

```c
/* Initialize hardware watchdog */
void hal_wdt_init(uint32_t timeout_ms) {
    /* Configure hardware WDT */
}

/* Feed hardware watchdog */
void hal_wdt_feed(void) {
    /* Refresh hardware WDT */
}

/* Enable hardware watchdog */
void hal_wdt_enable(void) {
    /* Start hardware WDT */
}

/* Disable hardware watchdog (if supported) */
void hal_wdt_disable(void) {
    /* Stop hardware WDT */
}

/* Trigger immediate reset */
void hal_wdt_trigger_reset(void) {
    /* Force system reset */
    while(1);
}

/* Check if last reset was watchdog reset */
bool hal_wdt_was_reset_by_watchdog(void) {
    /* Check reset status register */
    return false;
}

/* Clear watchdog reset flag */
void hal_wdt_clear_reset_flag(void) {
    /* Clear status register */
}
```

### Example: STM32 Implementation

```c
#include "stm32f4xx_hal.h"

static IWDG_HandleTypeDef hiwdg;

void hal_wdt_init(uint32_t timeout_ms) {
    hiwdg.Instance = IWDG;
    hiwdg.Init.Prescaler = IWDG_PRESCALER_32;
    hiwdg.Init.Reload = (timeout_ms * 32000) / 32000;
    HAL_IWDG_Init(&hiwdg);
}

void hal_wdt_feed(void) {
    HAL_IWDG_Refresh(&hiwdg);
}

void hal_wdt_enable(void) {
    /* STM32 IWDG starts on init */
}

void hal_wdt_disable(void) {
    /* STM32 IWDG cannot be disabled */
}

bool hal_wdt_was_reset_by_watchdog(void) {
    return __HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != RESET;
}

void hal_wdt_clear_reset_flag(void) {
    __HAL_RCC_CLEAR_RESET_FLAGS();
}
```

## Troubleshooting

### Problem: System keeps resetting

**Cause**: Watchdog not being fed frequently enough

**Solution**:
- Increase timeout: `wdt_set_timeout(5000)`
- Increase feed frequency
- Check if feeder task is blocked

### Problem: Task timeout not detected

**Cause**: Task watchdog disabled or not registered

**Solution**:
```c
/* Verify registration */
if (!wdt_is_task_registered(&task)) {
    wdt_register_task(&task, 1000);
}

/* Verify enabled */
wdt_enable_task(&task);
```

### Problem: Callback not called

**Cause**: Hardware reset happens before callback completes

**Solution**:
- Use software watchdog only for debugging
- Keep callback short and fast
- Log to non-volatile storage

## Performance Impact

- **Memory**: ~200 bytes for watchdog state
- **CPU**: Negligible (<0.1% for 8 monitored tasks)
- **Latency**: Sub-millisecond for feed operations

## Example Applications

### 1. Sensor Node

```c
/* Monitor sensor reading every 500ms */
wdt_register_task(&sensor_task, 500);

/* Monitor data transmission every 5s */
wdt_register_task(&transmit_task, 5000);
```

### 2. Motor Controller

```c
/* Critical: Monitor control loop */
wdt_register_task(&control_task, 100);

/* Less critical: Monitor UI updates */
wdt_register_task(&ui_task, 1000);
```

### 3. Network Gateway

```c
/* Monitor network stack */
wdt_register_task(&network_task, 2000);

/* Monitor message processing */
wdt_register_task(&message_task, 1000);
```

## See Also

- [TinyOS API Reference](../README.md#api-reference)
- [Task Management](../README.md#task-management)
- [System Recovery](./recovery.md)
