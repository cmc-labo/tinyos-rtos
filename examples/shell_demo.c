/**
 * @file shell_demo.c
 * @brief Interactive shell example for TinyOS
 *
 * Wires the shell to a UART and registers a custom "led" command.
 */

#include "tinyos.h"
#include "tinyos/shell.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Platform UART stubs — replace with real UART driver
 * ------------------------------------------------------------------------- */

static int uart_getc(uint32_t timeout_ms) {
    /* In a real system this blocks on a UART RX semaphore */
    (void)timeout_ms;
    return getchar();   /* stdin for simulation */
}

static void uart_puts(const char *s) {
    fputs(s, stdout);
    fflush(stdout);
}

/* -------------------------------------------------------------------------
 * Custom command: led <on|off>
 * ------------------------------------------------------------------------- */

static bool led_state = false;

static int cmd_led(int argc, char *argv[]) {
    if (argc != 2) return 1;   /* non-zero → print usage */

    if (strcmp(argv[1], "on") == 0) {
        led_state = true;
        /* gpio_write(LED_PIN, 1); */
        printf("LED: ON\r\n");
    } else if (strcmp(argv[1], "off") == 0) {
        led_state = false;
        /* gpio_write(LED_PIN, 0); */
        printf("LED: OFF\r\n");
    } else {
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Custom command: ping <ip>
 * ------------------------------------------------------------------------- */

static int cmd_ping(int argc, char *argv[]) {
    if (argc != 2) return 1;

    ipv4_addr_t ip;
    if (!net_parse_ipv4(argv[1], &ip)) {
        printf("Invalid IP: %s\r\n", argv[1]);
        return 0;
    }

    uint32_t rtt;
    os_error_t err = net_ping(ip, 2000, &rtt);
    if (err == OS_OK) {
        printf("Reply from %s: time=%lu ms\r\n", argv[1], (unsigned long)rtt);
    } else {
        printf("Request timeout for %s\r\n", argv[1]);
    }
    return 0;
}

/* -------------------------------------------------------------------------
 * Main
 * ------------------------------------------------------------------------- */

void shell_demo_main(void) {
    /* Register application-specific commands before starting */
    shell_register_cmd("led",  cmd_led,  "led <on|off>   Toggle LED");
    shell_register_cmd("ping", cmd_ping, "ping <ip>      ICMP ping");

    /* Start the shell (also registers built-ins: help, ps, mem, ver, net) */
    shell_io_t io = { .getc = uart_getc, .puts = uart_puts };
    shell_start(&io);
}
