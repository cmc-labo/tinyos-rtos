/**
 * @file shell.c
 * @brief Interactive shell implementation for TinyOS
 *
 * Built-in commands: help, ps, mem, ver, net
 */

#include "tinyos/shell.h"
#include "tinyos/net.h"
#include <string.h>
#include <stdio.h>

/*===========================================================================
 * State
 *===========================================================================*/

static shell_cmd_t  cmd_table[SHELL_MAX_COMMANDS];
static int          cmd_count = 0;
static shell_io_t   io;
static tcb_t        shell_task;

/*===========================================================================
 * Output helpers
 *===========================================================================*/

static void shell_print(const char *s)      { io.puts(s); }
static void shell_println(const char *s)    { io.puts(s); io.puts("\r\n"); }

static void shell_printf(const char *fmt, ...) {
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io.puts(buf);
}

/*===========================================================================
 * Built-in: help
 *===========================================================================*/

static int cmd_help(int argc, char *argv[]) {
    (void)argc; (void)argv;
    shell_println("Available commands:");
    for (int i = 0; i < cmd_count; i++) {
        shell_printf("  %-14s %s\r\n", cmd_table[i].name, cmd_table[i].help);
    }
    return 0;
}

/*===========================================================================
 * Built-in: ps  (task list)
 *===========================================================================*/

static const char *task_state_str(task_state_t s) {
    switch (s) {
        case TASK_STATE_READY:      return "READY";
        case TASK_STATE_RUNNING:    return "RUN";
        case TASK_STATE_BLOCKED:    return "BLOCK";
        case TASK_STATE_SUSPENDED:  return "SUSP";
        case TASK_STATE_TERMINATED: return "TERM";
        default:                    return "?";
    }
}

static int cmd_ps(int argc, char *argv[]) {
    (void)argc; (void)argv;
    shell_println("NAME             STATE   PRIO  CPU%   STACK");
    shell_println("---------------- ------- ----- ------ --------");

    os_system_stats_t sys;
    os_get_system_stats(&sys);

    for (uint32_t i = 0; i < sys.task_count; i++) {
        task_stats_t ts;
        /* os_task_get_stats_by_index is a convenience shim over the TCB list */
        if (os_task_get_stats_by_index(i, &ts) != OS_OK) continue;
        shell_printf("%-16s %-5s   %3u  %5.1f%%  %4lu/%4u\r\n",
                     ts.name,
                     task_state_str(ts.state),
                     ts.priority,
                     ts.cpu_usage,
                     (unsigned long)ts.stack_high_water,
                     STACK_SIZE);
    }
    return 0;
}

/*===========================================================================
 * Built-in: mem  (memory stats)
 *===========================================================================*/

static int cmd_mem(int argc, char *argv[]) {
    (void)argc; (void)argv;
    size_t   free_bytes, used_bytes;
    uint32_t allocs, frees;
    os_get_memory_stats(&free_bytes, &used_bytes, &allocs, &frees);
    uint32_t total = (uint32_t)(free_bytes + used_bytes);
    shell_printf("Total : %5lu bytes\r\n", (unsigned long)total);
    shell_printf("Used  : %5lu bytes  (%lu%%)\r\n",
                 (unsigned long)used_bytes,
                 total ? (unsigned long)(used_bytes * 100 / total) : 0);
    shell_printf("Free  : %5lu bytes\r\n", (unsigned long)free_bytes);
    shell_printf("Allocs: %lu  Frees: %lu\r\n",
                 (unsigned long)allocs, (unsigned long)frees);
    return 0;
}

/*===========================================================================
 * Built-in: ver
 *===========================================================================*/

static int cmd_ver(int argc, char *argv[]) {
    (void)argc; (void)argv;
    shell_println("TinyOS " TINYOS_VERSION);
    shell_printf("Uptime: %lu ms\r\n", (unsigned long)os_get_uptime_ms());
    return 0;
}

/*===========================================================================
 * Built-in: net
 *===========================================================================*/

static void shell_print_ip(const char *label, ipv4_addr_t ip) {
    char buf[16];
    net_format_ipv4(ip, buf);
    shell_printf("%-9s: %s\r\n", label, buf);
}

static int cmd_net(int argc, char *argv[]) {
    (void)argc; (void)argv;
    net_stats_t s;
    net_get_stats(&s);
    net_config_t cfg;
    net_get_config(&cfg);

    shell_print_ip("IP",      cfg.ip);
    shell_print_ip("Gateway", cfg.gateway);

    shell_println("--- Ethernet ---");
    shell_printf("  RX: %lu pkts  TX: %lu pkts  Err: %lu\r\n",
                 (unsigned long)s.eth_rx_packets,
                 (unsigned long)s.eth_tx_packets,
                 (unsigned long)(s.eth_rx_errors + s.eth_tx_errors));
    shell_println("--- IP ---");
    shell_printf("  RX: %lu  TX: %lu  Err: %lu\r\n",
                 (unsigned long)s.ip_rx_packets,
                 (unsigned long)s.ip_tx_packets,
                 (unsigned long)s.ip_rx_errors);
    shell_println("--- UDP / TCP ---");
    shell_printf("  UDP RX: %lu  TX: %lu\r\n",
                 (unsigned long)s.udp_rx_packets,
                 (unsigned long)s.udp_tx_packets);
    shell_printf("  TCP RX: %lu  TX: %lu  Conns: %lu  RST: %lu\r\n",
                 (unsigned long)s.tcp_rx_packets,
                 (unsigned long)s.tcp_tx_packets,
                 (unsigned long)s.tcp_connections,
                 (unsigned long)s.tcp_resets);
    return 0;
}

/*===========================================================================
 * Built-in: reboot
 *===========================================================================*/

static int cmd_reboot(int argc, char *argv[]) {
    (void)argc; (void)argv;
    shell_println("Rebooting...");
    os_task_delay(100);
    /* Platform-specific: trigger watchdog or NVIC reset */
    /* NVIC_SystemReset() on ARM Cortex-M */
    __asm__ volatile("bkpt");   /* Halt in simulator; replace with reset call */
    return 0;
}

/*===========================================================================
 * Command parsing
 *===========================================================================*/

void shell_exec(char *line) {
    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;   /* blank or comment */

    /* Tokenize */
    char *argv[SHELL_ARGV_MAX];
    int   argc = 0;
    char *p    = line;

    while (*p && argc < SHELL_ARGV_MAX) {
        /* Skip whitespace */
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        argv[argc++] = p;

        /* Advance to end of token */
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = '\0';
    }

    if (argc == 0) return;

    /* Find and call command */
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, argv[0]) == 0) {
            int rc = cmd_table[i].fn(argc, argv);
            if (rc != 0) {
                shell_printf("usage: %s\r\n", cmd_table[i].help);
            }
            return;
        }
    }

    shell_printf("Unknown command: '%s'  (type 'help')\r\n", argv[0]);
}

/*===========================================================================
 * Line editor (blocking read with backspace support)
 *===========================================================================*/

static void read_line(char *buf, size_t max) {
    size_t pos = 0;

    for (;;) {
        int c = io.getc(0 /* block forever */);
        if (c < 0) continue;

        if (c == '\r' || c == '\n') {
            shell_print("\r\n");
            buf[pos] = '\0';
            return;
        }

        if ((c == '\b' || c == 0x7F) && pos > 0) {
            /* Backspace: erase last character */
            pos--;
            shell_print("\b \b");
            continue;
        }

        if (c >= 0x20 && c < 0x7F && pos < max - 1) {
            buf[pos++] = (char)c;
            char echo[2] = { (char)c, '\0' };
            shell_print(echo);
        }
    }
}

/*===========================================================================
 * Shell task
 *===========================================================================*/

static void shell_task_fn(void *param) {
    (void)param;
    char line[SHELL_LINE_MAX];

    shell_println("\r\nTinyOS shell — type 'help' for commands");

    for (;;) {
        shell_print(SHELL_PROMPT);
        read_line(line, sizeof(line));
        shell_exec(line);
    }
}

/*===========================================================================
 * Public API
 *===========================================================================*/

shell_error_t shell_register_cmd(const char *name, shell_cmd_fn_t fn,
                                  const char *help) {
    if (!name || !fn) return SHELL_ERR_INVALID_PARAM;
    if (cmd_count >= SHELL_MAX_COMMANDS) return SHELL_ERR_TABLE_FULL;
    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, name) == 0) return SHELL_ERR_DUPLICATE;
    }
    cmd_table[cmd_count++] = (shell_cmd_t){ name, fn, help ? help : "" };
    return SHELL_OK;
}

shell_error_t shell_start(const shell_io_t *cfg) {
    if (!cfg || !cfg->getc || !cfg->puts) return SHELL_ERR_INVALID_PARAM;
    io = *cfg;

    /* Register built-ins */
    static const shell_cmd_t builtins[] = {
        { "help",   cmd_help,   "List all commands"      },
        { "ps",     cmd_ps,     "Show task list"          },
        { "mem",    cmd_mem,    "Show memory usage"       },
        { "ver",    cmd_ver,    "Show version and uptime" },
        { "net",    cmd_net,    "Show network statistics" },
        { "reboot", cmd_reboot, "Reboot the system"       },
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        shell_register_cmd(builtins[i].name, builtins[i].fn, builtins[i].help);
    }

    return os_task_create(&shell_task, "shell", shell_task_fn, NULL,
                          PRIORITY_LOW);
}
