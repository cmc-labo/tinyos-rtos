/**
 * @file stdio_uart_demo.c
 * @brief Demonstration of TinyOS stdio-UART integration.
 *
 * Shows three usage patterns:
 *   1. Standard printf / puts / putchar routed through the UART via _write().
 *   2. Structured log API (os_log / LOG_I / LOG_E …) with timestamp and tags.
 *   3. Multiple tasks printing concurrently without interleaving.
 *
 * How to build:
 *   make EXAMPLE=stdio_uart_demo
 *
 * Platform requirements:
 *   Implement uart_putc() and (optionally) uart_puts() for your hardware.
 *   The stubs below compile to no-ops; replace them with real UART code.
 */

#include "tinyos.h"
#include "tinyos/stdio_uart.h"
#include <stdio.h>
#include <string.h>

/*===========================================================================
 * Platform UART stubs — replace with real hardware register writes
 *===========================================================================*/

/**
 * STM32F4 USART1 example (uncomment and adapt):
 *
 *   static void uart_putc(char c) {
 *       while (!(USART1->SR & USART_SR_TXE));
 *       USART1->DR = (uint8_t)c;
 *   }
 */
static void uart_putc(char c) {
    /* TODO: write c to UART TX register */
    (void)c;
}

/* Optional fast-path for strings: reduces per-character overhead on slow UARTs.
 * Remove and pass NULL to tinyos_stdio_init() if not needed. */
static void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

/*===========================================================================
 * Task: sensor — simulates a sensor reading task
 *===========================================================================*/

static tcb_t sensor_tcb;

static void sensor_task(void *param) {
    (void)param;
    uint32_t count = 0;

    LOG_I("sensor", "Sensor task started");

    while (1) {
        float temperature = 22.5f + (float)(count % 10) * 0.3f;
        float humidity    = 55.0f + (float)(count % 5);

        LOG_D("sensor", "tick %lu: temp=%.1f C  hum=%.0f%%",
              (unsigned long)count, temperature, humidity);

        if (temperature > 25.0f) {
            LOG_W("sensor", "temperature above threshold: %.1f C", temperature);
        }

        count++;
        os_task_delay_ms(500);
    }
}

/*===========================================================================
 * Task: net — simulates a network task using plain printf
 *===========================================================================*/

static tcb_t net_tcb;

static void net_task(void *param) {
    (void)param;
    uint32_t packets = 0;

    /* Plain printf works because _write() is redirected to the UART */
    printf("[net] network task started\r\n");

    while (1) {
        packets++;

        /* printf from a task — thread-safe via the internal mutex */
        printf("[net] sent packet #%lu  uptime=%lu ms\r\n",
               (unsigned long)packets,
               (unsigned long)os_get_uptime_ms());

        if (packets % 10 == 0) {
            LOG_I("net", "%lu packets sent", (unsigned long)packets);
        }

        os_task_delay_ms(1000);
    }
}

/*===========================================================================
 * Task: monitor — prints system stats periodically using os_log
 *===========================================================================*/

static tcb_t monitor_tcb;

static void monitor_task(void *param) {
    (void)param;

    LOG_I("monitor", "monitor task started, log level = %d",
          (int)os_log_get_level());

    while (1) {
        os_task_delay_ms(5000);

        system_stats_t sys;
        if (os_get_system_stats(&sys) == OS_OK) {
            LOG_I("monitor",
                  "uptime=%lus  tasks=%lu  ctx_sw=%lu  cpu=%.1f%%  heap=%zu B",
                  (unsigned long)sys.uptime_seconds,
                  (unsigned long)sys.total_tasks,
                  (unsigned long)sys.total_context_switches,
                  sys.cpu_usage,
                  sys.free_heap);
        }
    }
}

/*===========================================================================
 * main
 *===========================================================================*/

int main(void) {
    os_init();
    os_mem_init();

    /* ── Bind stdio to UART ──────────────────────────────────────────────
     * Must be called before any printf / LOG_* usage.
     * putc_fn is required; puts_fn is optional (pass NULL to omit).         */
    tinyos_stdio_init(uart_putc, uart_puts);

    /* Set minimum log level (all messages at or above this level are shown). */
    os_log_set_level(LOG_DEBUG);

    /* Early boot message — before scheduler start, no mutex is needed.
     * tinyos_stdio_init must already have been called.                       */
    LOG_I("main", "TinyOS stdio-UART demo starting  (v%s)",
          os_get_version_string());

    /* Create tasks */
    os_task_create(&sensor_tcb,  "sensor",  sensor_task,  NULL, PRIORITY_NORMAL);
    os_task_create(&net_tcb,     "net",     net_task,     NULL, PRIORITY_NORMAL);
    os_task_create(&monitor_tcb, "monitor", monitor_task, NULL, PRIORITY_LOW);

    LOG_I("main", "starting scheduler — %lu tasks ready",
          (unsigned long)3);

    os_start();   /* never returns */
    return 0;
}
