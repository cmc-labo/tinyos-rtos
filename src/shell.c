/**
 * @file shell.c
 * @brief Interactive shell for TinyOS — Enhanced Edition
 *
 * Built-in commands (19 total):
 *   help, clear, echo, history,
 *   ps, top, kill,
 *   mem, ver,
 *   net, ping, ifconfig,
 *   power,
 *   ls, cat, mkdir, rm, df,
 *   reboot
 *
 * Line editor features:
 *   ← → arrows    : cursor movement
 *   Home / Ctrl-A  : move to start
 *   End  / Ctrl-E  : move to end
 *   Ctrl-K         : kill to end of line
 *   Ctrl-U         : kill to start of line
 *   Ctrl-W         : kill previous word
 *   Ctrl-L         : clear screen and redraw
 *   Ctrl-C         : cancel current line
 *   Del / Backspace: delete character
 *   ↑ ↓ arrows     : history navigation
 *   Tab            : command-name completion
 */

#include "tinyos/shell.h"
#include "tinyos/net.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/*===========================================================================
 * Internal state
 *===========================================================================*/

static shell_cmd_t  cmd_table[SHELL_MAX_COMMANDS];
static int          cmd_count = 0;
static shell_io_t   io;
static tcb_t        shell_tcb;
static char         shell_prompt[32];

/* Command history — circular ring buffer */
static char history[SHELL_HISTORY_DEPTH][SHELL_LINE_MAX];
static int  history_head  = 0;   /* next write slot */
static int  history_count = 0;   /* entries stored  */

/*===========================================================================
 * Output helpers
 *===========================================================================*/

static void shell_print(const char *s)   { io.puts(s); }
static void shell_println(const char *s) { io.puts(s); io.puts("\r\n"); }

static void shell_printf(const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io.puts(buf);
}

/*===========================================================================
 * Command history
 *===========================================================================*/

static void history_push(const char *line) {
    if (!line || line[0] == '\0') return;
    /* Skip duplicate of last entry */
    if (history_count > 0) {
        int last = (history_head - 1 + SHELL_HISTORY_DEPTH) % SHELL_HISTORY_DEPTH;
        if (strcmp(history[last], line) == 0) return;
    }
    strncpy(history[history_head], line, SHELL_LINE_MAX - 1);
    history[history_head][SHELL_LINE_MAX - 1] = '\0';
    history_head = (history_head + 1) % SHELL_HISTORY_DEPTH;
    if (history_count < SHELL_HISTORY_DEPTH) history_count++;
}

/* offset=0 → most recent; offset=1 → second most recent; etc. */
static const char *history_get(int offset) {
    if (offset < 0 || offset >= history_count) return NULL;
    int idx = (history_head - 1 - offset + SHELL_HISTORY_DEPTH * 2)
              % SHELL_HISTORY_DEPTH;
    return history[idx];
}

/*===========================================================================
 * VT100 line editor
 *===========================================================================*/

typedef struct {
    char buf[SHELL_LINE_MAX];
    int  len;   /* current length (NUL not counted) */
    int  pos;   /* cursor position: 0 = start, len = end */
} lb_t;

/* Internal key codes for special keys */
#define KEY_UP     0x101
#define KEY_DOWN   0x102
#define KEY_RIGHT  0x103
#define KEY_LEFT   0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_DEL    0x107

/* Ctrl-key codes (raw ASCII) */
#define CTRL_A  0x01
#define CTRL_B  0x02
#define CTRL_C  0x03
#define CTRL_E  0x05
#define CTRL_F  0x06
#define CTRL_K  0x0B
#define CTRL_L  0x0C
#define CTRL_U  0x15
#define CTRL_W  0x17

/* Redraw the prompt + buffer and reposition the cursor. */
static void lb_refresh(const lb_t *lb) {
    /* \r goes to column 0; \033[K clears to end of line. */
    shell_printf("\r%s%.*s\033[K", shell_prompt, lb->len, lb->buf);
    /* Move cursor back from end to actual position. */
    int back = lb->len - lb->pos;
    if (back > 0) shell_printf("\033[%dD", back);
}

static void lb_insert(lb_t *lb, char c) {
    if (lb->len >= SHELL_LINE_MAX - 1) return;
    memmove(&lb->buf[lb->pos + 1], &lb->buf[lb->pos], lb->len - lb->pos);
    lb->buf[lb->pos++] = c;
    lb->buf[++lb->len] = '\0';
}

static void lb_backspace(lb_t *lb) {
    if (lb->pos == 0) return;
    memmove(&lb->buf[lb->pos - 1], &lb->buf[lb->pos], lb->len - lb->pos);
    lb->buf[--lb->len] = '\0';
    lb->pos--;
}

static void lb_delete(lb_t *lb) {
    if (lb->pos == lb->len) return;
    memmove(&lb->buf[lb->pos], &lb->buf[lb->pos + 1], lb->len - lb->pos - 1);
    lb->buf[--lb->len] = '\0';
}

static void lb_kill_to_end(lb_t *lb) {
    lb->len = lb->pos;
    lb->buf[lb->len] = '\0';
}

static void lb_kill_to_start(lb_t *lb) {
    memmove(lb->buf, lb->buf + lb->pos, lb->len - lb->pos);
    lb->len -= lb->pos;
    lb->pos  = 0;
    lb->buf[lb->len] = '\0';
}

static void lb_kill_word(lb_t *lb) {
    int end = lb->pos;
    while (lb->pos > 0 && lb->buf[lb->pos - 1] == ' ') lb->pos--;
    while (lb->pos > 0 && lb->buf[lb->pos - 1] != ' ') lb->pos--;
    int removed = end - lb->pos;
    memmove(&lb->buf[lb->pos], &lb->buf[end], lb->len - end);
    lb->len -= removed;
    lb->buf[lb->len] = '\0';
}

static void lb_set(lb_t *lb, const char *s) {
    strncpy(lb->buf, s, SHELL_LINE_MAX - 1);
    lb->buf[SHELL_LINE_MAX - 1] = '\0';
    lb->len = lb->pos = (int)strlen(lb->buf);
}

/* Tab completion: match command names, fill longest common prefix. */
static void lb_tab_complete(lb_t *lb) {
    /* Only complete the first token (before any space). */
    for (int i = 0; i < lb->pos; i++) {
        if (lb->buf[i] == ' ') return;
    }

    const char *matches[SHELL_MAX_COMMANDS];
    int mc = 0;
    for (int i = 0; i < cmd_count; i++) {
        if (strncmp(cmd_table[i].name, lb->buf, (size_t)lb->pos) == 0)
            matches[mc++] = cmd_table[i].name;
    }

    if (mc == 0) {
        shell_print("\007");   /* bell */
        return;
    }

    if (mc == 1) {
        /* Unique match — complete and append a space. */
        lb_set(lb, matches[0]);
        lb_insert(lb, ' ');
        lb_refresh(lb);
        return;
    }

    /* Multiple matches — list them then fill longest common prefix. */
    shell_print("\r\n");
    for (int i = 0; i < mc; i++) {
        shell_printf("  %-14s", matches[i]);
        if ((i + 1) % 4 == 0) shell_print("\r\n");
    }
    if (mc % 4 != 0) shell_print("\r\n");

    int common = (int)strlen(matches[0]);
    for (int i = 1; i < mc; i++) {
        int j = 0;
        while (j < common && matches[0][j] == matches[i][j]) j++;
        common = j;
    }
    if (common > lb->pos) {
        /* Fill the shared prefix but don't append a space yet. */
        char prefix[SHELL_LINE_MAX];
        strncpy(prefix, matches[0], (size_t)common);
        prefix[common] = '\0';
        lb_set(lb, prefix);
        /* Restore cursor to end of prefix. */
        lb->pos = lb->len;
    }
    lb_refresh(lb);
}

/* Read one raw key; decode VT100 escape sequences into KEY_* codes. */
static int read_key(void) {
    int c = io.getc(0);   /* block until a character arrives */
    if (c < 0 || c != 0x1B) return c;

    /* ESC: try to read the rest of the sequence with a short timeout. */
    int c2 = io.getc(50);
    if (c2 < 0) return 0x1B;               /* bare ESC */
    if (c2 != '[' && c2 != 'O') return 0x1B;

    int c3 = io.getc(50);
    if (c3 < 0) return 0x1B;

    switch (c3) {
        case 'A': return KEY_UP;
        case 'B': return KEY_DOWN;
        case 'C': return KEY_RIGHT;
        case 'D': return KEY_LEFT;
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case '3': io.getc(50); return KEY_DEL;   /* ESC [ 3 ~ */
        case '1': io.getc(50); return KEY_HOME;  /* ESC [ 1 ~ */
        case '4': io.getc(50); return KEY_END;   /* ESC [ 4 ~ */
        default:  return 0x1B;
    }
}

/* Full-featured line editor.  Writes result (NUL-terminated) to *out. */
static void read_line(char *out, size_t max) {
    lb_t lb;
    memset(&lb, 0, sizeof(lb));
    int hist_idx = -1;   /* -1 = not browsing history */

    for (;;) {
        int k = read_key();

        switch (k) {
        /* ── Submit ── */
        case '\r':
        case '\n':
            shell_print("\r\n");
            strncpy(out, lb.buf, max - 1);
            out[max - 1] = '\0';
            return;

        /* ── Cancel line (Ctrl-C) ── */
        case CTRL_C:
            shell_print("^C\r\n");
            lb.len = lb.pos = 0;
            lb.buf[0] = '\0';
            shell_print(shell_prompt);
            hist_idx = -1;
            break;

        /* ── Delete / Backspace ── */
        case '\b': case 0x7F:
            lb_backspace(&lb);
            lb_refresh(&lb);
            break;
        case KEY_DEL:
            lb_delete(&lb);
            lb_refresh(&lb);
            break;

        /* ── Cursor movement ── */
        case KEY_LEFT:  case CTRL_B:
            if (lb.pos > 0) { lb.pos--; shell_print("\033[D"); }
            break;
        case KEY_RIGHT: case CTRL_F:
            if (lb.pos < lb.len) { lb.pos++; shell_print("\033[C"); }
            break;
        case KEY_HOME: case CTRL_A:
            if (lb.pos > 0) { shell_printf("\033[%dD", lb.pos); lb.pos = 0; }
            break;
        case KEY_END: case CTRL_E:
            if (lb.pos < lb.len) {
                shell_printf("\033[%dC", lb.len - lb.pos);
                lb.pos = lb.len;
            }
            break;

        /* ── Kill operations ── */
        case CTRL_K: lb_kill_to_end(&lb);   lb_refresh(&lb); break;
        case CTRL_U: lb_kill_to_start(&lb); lb_refresh(&lb); break;
        case CTRL_W: lb_kill_word(&lb);     lb_refresh(&lb); break;

        /* ── Clear screen & redraw ── */
        case CTRL_L:
            shell_print("\033[2J\033[H");
            lb_refresh(&lb);
            break;

        /* ── History navigation ── */
        case KEY_UP:
            if (hist_idx + 1 < history_count) {
                hist_idx++;
                const char *h = history_get(hist_idx);
                if (h) { lb_set(&lb, h); lb_refresh(&lb); }
            }
            break;
        case KEY_DOWN:
            if (hist_idx > 0) {
                hist_idx--;
                const char *h = history_get(hist_idx);
                if (h) { lb_set(&lb, h); lb_refresh(&lb); }
            } else {
                hist_idx = -1;
                lb.len = lb.pos = 0;
                lb.buf[0] = '\0';
                lb_refresh(&lb);
            }
            break;

        /* ── Tab completion ── */
        case '\t':
            lb_tab_complete(&lb);
            break;

        /* ── Printable characters ── */
        default:
            if (k >= 0x20 && k < 0x7F) {
                lb_insert(&lb, (char)k);
                lb_refresh(&lb);
            }
            break;
        }
    }
}

/*===========================================================================
 * Built-in: help
 *===========================================================================*/

static int cmd_help(int argc, char *argv[]) {
    if (argc >= 2) {
        for (int i = 0; i < cmd_count; i++) {
            if (strcmp(cmd_table[i].name, argv[1]) == 0) {
                shell_printf("  %s\r\n", cmd_table[i].help);
                return 0;
            }
        }
        shell_printf("  Unknown command: '%s'\r\n", argv[1]);
        return 0;
    }
    shell_println("Available commands:");
    for (int i = 0; i < cmd_count; i++) {
        shell_printf("  %-14s %s\r\n", cmd_table[i].name, cmd_table[i].help);
    }
    return 0;
}

/*===========================================================================
 * Built-in: clear
 *===========================================================================*/

static int cmd_clear(int argc, char *argv[]) {
    (void)argc; (void)argv;
    shell_print("\033[2J\033[H");
    return 0;
}

/*===========================================================================
 * Built-in: echo
 *===========================================================================*/

static int cmd_echo(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        shell_print(argv[i]);
        if (i < argc - 1) shell_print(" ");
    }
    shell_print("\r\n");
    return 0;
}

/*===========================================================================
 * Built-in: history
 *===========================================================================*/

static int cmd_history(int argc, char *argv[]) {
    (void)argc; (void)argv;
    if (history_count == 0) {
        shell_println("  (no history)");
        return 0;
    }
    for (int i = history_count - 1; i >= 0; i--) {
        const char *h = history_get(i);
        if (h) shell_printf("  %2d  %s\r\n", history_count - i, h);
    }
    return 0;
}

/*===========================================================================
 * Built-in: ps  (task list)
 *===========================================================================*/

static const char *task_state_str(task_state_t s) {
    switch (s) {
        case TASK_STATE_READY:      return "READY";
        case TASK_STATE_RUNNING:    return "RUN  ";
        case TASK_STATE_BLOCKED:    return "BLOCK";
        case TASK_STATE_SUSPENDED:  return "SUSP ";
        case TASK_STATE_TERMINATED: return "TERM ";
        default:                    return "?    ";
    }
}

static int cmd_ps(int argc, char *argv[]) {
    (void)argc; (void)argv;
    system_stats_t sys;
    os_get_system_stats(&sys);

    shell_println("  #   NAME             STATE   PRIO   CPU%   STACK used/total");
    shell_println("  --- ---------------- ------- -----  ------ ----------------");

    for (uint32_t i = 0; i < sys.total_tasks; i++) {
        task_stats_t ts;
        if (os_task_get_stats_by_index(i, &ts) != OS_OK) continue;
        shell_printf("  %2lu  %-16s %s  %3u   %5.1f%%  %4lu / %4lu\r\n",
                     (unsigned long)i,
                     ts.name,
                     task_state_str(ts.state),
                     ts.priority,
                     ts.cpu_usage,
                     (unsigned long)ts.stack_used,
                     (unsigned long)ts.stack_size);
    }
    shell_printf("\r\n  Tasks: %lu  Running: %lu\r\n",
                 (unsigned long)sys.total_tasks,
                 (unsigned long)sys.running_tasks);
    return 0;
}

/*===========================================================================
 * Built-in: top  (task list sorted by CPU usage, descending)
 *===========================================================================*/

static int cmd_top(int argc, char *argv[]) {
    (void)argc; (void)argv;
    system_stats_t sys;
    os_get_system_stats(&sys);

    task_stats_t tasks[MAX_TASKS];
    uint32_t count = 0;
    for (uint32_t i = 0; i < sys.total_tasks && count < MAX_TASKS; i++) {
        if (os_task_get_stats_by_index(i, &tasks[count]) == OS_OK) count++;
    }

    /* Bubble sort descending by cpu_usage */
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t j = 0; j + 1 < count - i; j++) {
            if (tasks[j].cpu_usage < tasks[j + 1].cpu_usage) {
                task_stats_t tmp = tasks[j];
                tasks[j] = tasks[j + 1];
                tasks[j + 1] = tmp;
            }
        }
    }

    uint32_t up_ms = os_get_uptime_ms();
    shell_printf("  Uptime: %lu ms   Tasks: %lu\r\n",
                 (unsigned long)up_ms, (unsigned long)count);
    shell_println("  NAME             STATE   PRIO   CPU%   STACK used/total");
    shell_println("  ---------------- ------- -----  ------ ----------------");

    float total_cpu = 0.0f;
    for (uint32_t i = 0; i < count; i++) {
        shell_printf("  %-16s %s  %3u   %5.1f%%  %4lu / %4lu\r\n",
                     tasks[i].name,
                     task_state_str(tasks[i].state),
                     tasks[i].priority,
                     tasks[i].cpu_usage,
                     (unsigned long)tasks[i].stack_used,
                     (unsigned long)tasks[i].stack_size);
        total_cpu += tasks[i].cpu_usage;
    }
    shell_printf("\r\n  Total CPU: %.1f%%\r\n", total_cpu);
    return 0;
}

/*===========================================================================
 * Built-in: kill  (task control by name)
 *===========================================================================*/

static int cmd_kill(int argc, char *argv[]) {
    if (argc < 2) return 1;

    const char *name   = argv[1];
    const char *action = (argc >= 3) ? argv[2] : "suspend";

    tcb_t *tcb = os_task_find_by_name(name);
    if (!tcb) {
        shell_printf("  Task '%s' not found.\r\n", name);
        return 0;
    }

    if (strcmp(action, "suspend") == 0) {
        os_error_t r = os_task_suspend(tcb);
        shell_printf("  '%s' %s.\r\n", name,
                     r == OS_OK ? "suspended" : "suspend failed");
    } else if (strcmp(action, "resume") == 0) {
        os_error_t r = os_task_resume(tcb);
        shell_printf("  '%s' %s.\r\n", name,
                     r == OS_OK ? "resumed" : "resume failed");
    } else if (strcmp(action, "delete") == 0) {
        shell_printf("  Deleting task '%s'...\r\n", name);
        os_task_delete(tcb);
    } else {
        shell_printf("  Unknown action '%s'. Use: suspend | resume | delete\r\n",
                     action);
    }
    return 0;
}

/*===========================================================================
 * Built-in: mem  (heap statistics)
 *===========================================================================*/

static int cmd_mem(int argc, char *argv[]) {
    (void)argc; (void)argv;
    size_t   free_bytes, used_bytes;
    uint32_t allocs, frees;
    os_get_memory_stats(&free_bytes, &used_bytes, &allocs, &frees);
    uint32_t total = (uint32_t)(free_bytes + used_bytes);

    shell_printf("  Total  : %6lu bytes\r\n", (unsigned long)total);
    shell_printf("  Used   : %6lu bytes  (%lu%%)\r\n",
                 (unsigned long)used_bytes,
                 total ? (unsigned long)(used_bytes * 100 / total) : 0);
    shell_printf("  Free   : %6lu bytes\r\n", (unsigned long)free_bytes);
    shell_printf("  Allocs : %lu   Frees: %lu\r\n",
                 (unsigned long)allocs, (unsigned long)frees);
    return 0;
}

/*===========================================================================
 * Built-in: ver  (version and uptime)
 *===========================================================================*/

static int cmd_ver(int argc, char *argv[]) {
    (void)argc; (void)argv;
    uint32_t up_ms  = os_get_uptime_ms();
    uint32_t up_sec = up_ms  / 1000;
    uint32_t up_min = up_sec / 60;
    uint32_t up_hr  = up_min / 60;

    shell_printf("  TinyOS %s\r\n", os_get_version_string());
    shell_printf("  Uptime : %02lu:%02lu:%02lu.%03lu\r\n",
                 (unsigned long)up_hr,
                 (unsigned long)(up_min % 60),
                 (unsigned long)(up_sec % 60),
                 (unsigned long)(up_ms  % 1000));
    shell_printf("  Ticks  : %lu\r\n", (unsigned long)os_get_tick_count());
    return 0;
}

/*===========================================================================
 * Built-in: net  (network statistics)
 *===========================================================================*/

static void print_ip(const char *label, ipv4_addr_t ip) {
    char s[16];
    net_format_ipv4(ip, s);
    shell_printf("  %-10s: %s\r\n", label, s);
}

static int cmd_net(int argc, char *argv[]) {
    (void)argc; (void)argv;
    net_stats_t  s;
    net_config_t cfg;
    net_get_stats(&s);
    net_get_config(&cfg);

    shell_printf("  MAC     : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                 cfg.mac.addr[0], cfg.mac.addr[1], cfg.mac.addr[2],
                 cfg.mac.addr[3], cfg.mac.addr[4], cfg.mac.addr[5]);
    print_ip("IP",      cfg.ip);
    print_ip("Netmask", cfg.netmask);
    print_ip("Gateway", cfg.gateway);
    print_ip("DNS",     cfg.dns);

    shell_println("  --- Ethernet ---");
    shell_printf("    RX: %lu pkts  TX: %lu pkts  Err: %lu\r\n",
                 (unsigned long)s.eth_rx_packets,
                 (unsigned long)s.eth_tx_packets,
                 (unsigned long)(s.eth_rx_errors + s.eth_tx_errors));
    shell_println("  --- IP ---");
    shell_printf("    RX: %lu  TX: %lu  Err: %lu\r\n",
                 (unsigned long)s.ip_rx_packets,
                 (unsigned long)s.ip_tx_packets,
                 (unsigned long)s.ip_rx_errors);
    shell_println("  --- UDP / TCP ---");
    shell_printf("    UDP RX: %lu  TX: %lu\r\n",
                 (unsigned long)s.udp_rx_packets,
                 (unsigned long)s.udp_tx_packets);
    shell_printf("    TCP RX: %lu  TX: %lu  Conns: %lu  RST: %lu\r\n",
                 (unsigned long)s.tcp_rx_packets,
                 (unsigned long)s.tcp_tx_packets,
                 (unsigned long)s.tcp_connections,
                 (unsigned long)s.tcp_resets);
    return 0;
}

/*===========================================================================
 * Built-in: ping
 *===========================================================================*/

static int cmd_ping(int argc, char *argv[]) {
    if (argc < 2) return 1;

    ipv4_addr_t dest;
    if (net_parse_ipv4(argv[1], &dest) != OS_OK) {
        shell_printf("  Invalid address: %s\r\n", argv[1]);
        return 0;
    }

    int count = (argc >= 3) ? (int)strtol(argv[2], NULL, 10) : 4;
    if (count <= 0 || count > 100) count = 4;

    char ip_str[16];
    net_format_ipv4(dest, ip_str);
    shell_printf("  PING %s — %d packets\r\n", ip_str, count);

    int sent = 0, received = 0;
    uint32_t total_rtt = 0;

    for (int i = 0; i < count; i++) {
        uint32_t rtt = 0;
        os_error_t err = net_ping(dest, 1000, &rtt);
        sent++;
        if (err == OS_OK) {
            received++;
            total_rtt += rtt;
            shell_printf("  Reply from %s: time=%lu ms\r\n",
                         ip_str, (unsigned long)rtt);
        } else {
            shell_println("  Request timed out.");
        }
        if (i < count - 1) os_task_delay_ms(1000);
    }

    int loss = (sent > 0) ? ((sent - received) * 100 / sent) : 100;
    shell_printf("  %d sent, %d received, %d%% loss", sent, received, loss);
    if (received > 0)
        shell_printf(", avg rtt = %lu ms", (unsigned long)(total_rtt / (uint32_t)received));
    shell_print("\r\n");
    return 0;
}

/*===========================================================================
 * Built-in: ifconfig  (show / change network configuration)
 *===========================================================================*/

static int cmd_ifconfig(int argc, char *argv[]) {
    net_config_t cfg;
    net_get_config(&cfg);

    if (argc == 1) {
        /* Show current configuration */
        char s[16];
        shell_printf("  MAC     : %02X:%02X:%02X:%02X:%02X:%02X\r\n",
                     cfg.mac.addr[0], cfg.mac.addr[1], cfg.mac.addr[2],
                     cfg.mac.addr[3], cfg.mac.addr[4], cfg.mac.addr[5]);
        net_format_ipv4(cfg.ip,      s); shell_printf("  IP      : %s\r\n", s);
        net_format_ipv4(cfg.netmask, s); shell_printf("  Netmask : %s\r\n", s);
        net_format_ipv4(cfg.gateway, s); shell_printf("  Gateway : %s\r\n", s);
        net_format_ipv4(cfg.dns,     s); shell_printf("  DNS     : %s\r\n", s);
        return 0;
    }

    /* ifconfig <field> <value> */
    if (argc < 3) return 1;

    ipv4_addr_t addr;
    if (net_parse_ipv4(argv[2], &addr) != OS_OK) {
        shell_printf("  Invalid address: %s\r\n", argv[2]);
        return 0;
    }

    if      (strcmp(argv[1], "ip")      == 0) cfg.ip      = addr;
    else if (strcmp(argv[1], "netmask") == 0) cfg.netmask = addr;
    else if (strcmp(argv[1], "gw")      == 0) cfg.gateway = addr;
    else if (strcmp(argv[1], "gateway") == 0) cfg.gateway = addr;
    else if (strcmp(argv[1], "dns")     == 0) cfg.dns     = addr;
    else {
        shell_printf("  Unknown field: %s\r\n", argv[1]);
        return 1;
    }

    net_set_config(&cfg);
    shell_println("  Configuration updated.");
    return 0;
}

/*===========================================================================
 * Built-in: power  (power management status / mode selection)
 *===========================================================================*/

static const char *power_mode_name(power_mode_t m) {
    switch (m) {
        case POWER_MODE_ACTIVE:     return "ACTIVE";
        case POWER_MODE_IDLE:       return "IDLE";
        case POWER_MODE_SLEEP:      return "SLEEP";
        case POWER_MODE_DEEP_SLEEP: return "DEEP_SLEEP";
        default:                    return "UNKNOWN";
    }
}

static int cmd_power(int argc, char *argv[]) {
    if (argc >= 2) {
        power_mode_t mode;
        if      (strcmp(argv[1], "active")    == 0) mode = POWER_MODE_ACTIVE;
        else if (strcmp(argv[1], "idle")      == 0) mode = POWER_MODE_IDLE;
        else if (strcmp(argv[1], "sleep")     == 0) mode = POWER_MODE_SLEEP;
        else if (strcmp(argv[1], "deepsleep") == 0) mode = POWER_MODE_DEEP_SLEEP;
        else { shell_printf("  Unknown mode: %s\r\n", argv[1]); return 1; }

        os_power_set_mode(mode);
        shell_printf("  Power mode set to %s.\r\n", power_mode_name(mode));
        return 0;
    }

    power_stats_t ps;
    os_power_get_stats(&ps);
    shell_printf("  Mode       : %s\r\n", power_mode_name(ps.current_mode));
    shell_printf("  Active     : %lu ms\r\n",  (unsigned long)ps.total_active_time_ms);
    shell_printf("  Sleep      : %lu ms\r\n",  (unsigned long)ps.total_sleep_time_ms);
    shell_printf("  Consumption: %lu mW\r\n",  (unsigned long)ps.power_consumption_mw);
    shell_printf("  Batt life  : %lu h\r\n",   (unsigned long)ps.estimated_battery_life_hours);
    return 0;
}

/*===========================================================================
 * Built-in: ls  (directory listing)
 *===========================================================================*/

static int cmd_ls(int argc, char *argv[]) {
    const char *path = (argc >= 2) ? argv[1] : "/";

    fs_dir_t dir = fs_opendir(path);
    if (!dir) {
        shell_printf("  Cannot open: %s\r\n", path);
        return 0;
    }

    shell_printf("  %s\r\n", path);
    shell_println("  TYPE  SIZE      NAME");
    shell_println("  ----  --------  --------");

    fs_dirent_t entry;
    int      file_count = 0, dir_count = 0;
    uint32_t total_size = 0;

    while (fs_readdir(dir, &entry) == OS_OK) {
        if (entry.type == FS_TYPE_DIRECTORY) {
            shell_printf("  DIR            %s/\r\n", entry.name);
            dir_count++;
        } else {
            shell_printf("  FILE  %8lu  %s\r\n",
                         (unsigned long)entry.size, entry.name);
            total_size += entry.size;
            file_count++;
        }
    }
    fs_closedir(dir);

    shell_printf("\r\n  %d file(s), %d dir(s), %lu bytes\r\n",
                 file_count, dir_count, (unsigned long)total_size);
    return 0;
}

/*===========================================================================
 * Built-in: cat  (display file contents)
 *===========================================================================*/

static int cmd_cat(int argc, char *argv[]) {
    if (argc < 2) return 1;

    fs_file_t fd = fs_open(argv[1], FS_O_RDONLY);
    if (fd == FS_INVALID_FD) {
        shell_printf("  Cannot open: %s\r\n", argv[1]);
        return 0;
    }

    char chunk[65];
    int32_t n;
    while ((n = fs_read(fd, chunk, sizeof(chunk) - 1)) > 0) {
        /* Expand bare \n to \r\n for terminal compatibility */
        for (int32_t i = 0; i < n; i++) {
            if (chunk[i] == '\n') {
                shell_print("\r\n");
            } else {
                char s[2] = { chunk[i], '\0' };
                shell_print(s);
            }
        }
    }
    shell_print("\r\n");
    fs_close(fd);
    return 0;
}

/*===========================================================================
 * Built-in: mkdir
 *===========================================================================*/

static int cmd_mkdir(int argc, char *argv[]) {
    if (argc < 2) return 1;
    os_error_t r = fs_mkdir(argv[1]);
    shell_printf("  mkdir '%s': %s\r\n", argv[1],
                 r == OS_OK ? "OK" : os_error_to_string(r));
    return 0;
}

/*===========================================================================
 * Built-in: rm
 *===========================================================================*/

static int cmd_rm(int argc, char *argv[]) {
    if (argc < 2) return 1;
    os_error_t r = fs_remove(argv[1]);
    if (r != OS_OK) r = fs_rmdir(argv[1]);
    shell_printf("  rm '%s': %s\r\n", argv[1],
                 r == OS_OK ? "OK" : os_error_to_string(r));
    return 0;
}

/*===========================================================================
 * Built-in: df  (filesystem statistics)
 *===========================================================================*/

static int cmd_df(int argc, char *argv[]) {
    (void)argc; (void)argv;

    if (!fs_is_mounted()) {
        shell_println("  No filesystem mounted.");
        return 0;
    }

    fs_stats_t fss;
    if (fs_get_stats(&fss) != OS_OK) {
        shell_println("  Failed to get filesystem stats.");
        return 0;
    }

    uint32_t bs        = fss.block_size;
    uint32_t total_kb  = (fss.total_blocks * bs) / 1024;
    uint32_t used_kb   = (fss.used_blocks  * bs) / 1024;
    uint32_t free_kb   = (fss.free_blocks  * bs) / 1024;
    uint32_t pct       = total_kb ? (used_kb * 100 / total_kb) : 0;

    shell_println("  Filesystem statistics:");
    shell_printf("    Total  : %6lu KB  (%lu blocks x %lu B)\r\n",
                 (unsigned long)total_kb,
                 (unsigned long)fss.total_blocks,
                 (unsigned long)bs);
    shell_printf("    Used   : %6lu KB  (%lu%%)\r\n",
                 (unsigned long)used_kb, (unsigned long)pct);
    shell_printf("    Free   : %6lu KB\r\n", (unsigned long)free_kb);
    shell_printf("    Files  : %lu   Dirs: %lu\r\n",
                 (unsigned long)fss.total_files,
                 (unsigned long)fss.total_dirs);
    return 0;
}

/*===========================================================================
 * Built-in: reboot
 *===========================================================================*/

static int cmd_reboot(int argc, char *argv[]) {
    (void)argc; (void)argv;
    shell_println("  Rebooting...");
    os_task_delay_ms(100);
    /* ARM Cortex-M: replace with NVIC_SystemReset() on real hardware */
    __asm__ volatile("bkpt");
    return 0;
}

/*===========================================================================
 * Command parsing  (supports "quoted strings" as single tokens)
 *===========================================================================*/

void shell_exec(char *line) {
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0' || *line == '#') return;

    char *argv[SHELL_ARGV_MAX];
    int   argc = 0;
    char *p    = line;

    while (*p && argc < SHELL_ARGV_MAX) {
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0') break;

        if (*p == '"') {
            /* Quoted token: include spaces, stop at closing '"' */
            p++;
            argv[argc++] = p;
            while (*p && *p != '"') p++;
            if (*p == '"') *p++ = '\0';
        } else {
            argv[argc++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) *p++ = '\0';
        }
    }

    if (argc == 0) return;

    for (int i = 0; i < cmd_count; i++) {
        if (strcmp(cmd_table[i].name, argv[0]) == 0) {
            int rc = cmd_table[i].fn(argc, argv);
            if (rc != 0)
                shell_printf("  usage: %s\r\n", cmd_table[i].help);
            return;
        }
    }
    shell_printf("  Unknown command: '%s'  (type 'help')\r\n", argv[0]);
}

/*===========================================================================
 * Shell task entry
 *===========================================================================*/

static void shell_task_fn(void *param) {
    (void)param;
    char line[SHELL_LINE_MAX];

    /* Welcome banner */
    shell_print("\033[2J\033[H");
    shell_println("  _____ _             ___  ____");
    shell_println(" |_   _(_)_ __  _   / _ \\/ ___|");
    shell_println("   | | | | '_ \\| | | | | \\___ \\");
    shell_println("   | | | | | | | |_| |_| |___) |");
    shell_println("   |_| |_|_| |_|\\__, |\\___/|____/");
    shell_println("                |___/");
    shell_printf("\r\n  TinyOS %s  —  Interactive Shell\r\n",
                 os_get_version_string());
    shell_println("  Tab: complete   ↑↓: history   Ctrl-C: cancel\r\n");

    for (;;) {
        shell_print(shell_prompt);
        read_line(line, sizeof(line));
        if (line[0] != '\0') {
            history_push(line);
            shell_exec(line);
        }
    }
}

/*===========================================================================
 * Public API
 *===========================================================================*/

void shell_set_prompt(const char *prompt) {
    if (prompt) {
        strncpy(shell_prompt, prompt, sizeof(shell_prompt) - 1);
        shell_prompt[sizeof(shell_prompt) - 1] = '\0';
    }
}

shell_error_t shell_register_cmd(const char *name, shell_cmd_fn_t fn,
                                  const char *help) {
    if (!name || !fn)                        return SHELL_ERR_INVALID_PARAM;
    if (cmd_count >= SHELL_MAX_COMMANDS)     return SHELL_ERR_TABLE_FULL;
    for (int i = 0; i < cmd_count; i++)
        if (strcmp(cmd_table[i].name, name) == 0) return SHELL_ERR_DUPLICATE;
    cmd_table[cmd_count++] = (shell_cmd_t){ name, fn, help ? help : "" };
    return SHELL_OK;
}

shell_error_t shell_start(const shell_io_t *cfg) {
    if (!cfg || !cfg->getc || !cfg->puts) return SHELL_ERR_INVALID_PARAM;
    io = *cfg;

    /* Initialise prompt */
    strncpy(shell_prompt, SHELL_PROMPT, sizeof(shell_prompt) - 1);
    shell_prompt[sizeof(shell_prompt) - 1] = '\0';

    /* Register built-in commands */
    static const shell_cmd_t builtins[] = {
        { "help",     cmd_help,
          "help [cmd]  — list commands or show detailed help" },
        { "clear",    cmd_clear,
          "clear  — clear the terminal screen" },
        { "echo",     cmd_echo,
          "echo <text ...>  — print text to terminal" },
        { "history",  cmd_history,
          "history  — show command history" },
        { "ps",       cmd_ps,
          "ps  — show task list" },
        { "top",      cmd_top,
          "top  — show tasks sorted by CPU usage" },
        { "kill",     cmd_kill,
          "kill <name> [suspend|resume|delete]  — control a task" },
        { "mem",      cmd_mem,
          "mem  — show heap memory statistics" },
        { "ver",      cmd_ver,
          "ver  — show TinyOS version and uptime" },
        { "net",      cmd_net,
          "net  — show network statistics" },
        { "ping",     cmd_ping,
          "ping <ip> [count]  — send ICMP echo requests" },
        { "ifconfig", cmd_ifconfig,
          "ifconfig [ip|netmask|gw|dns <addr>]  — network configuration" },
        { "power",    cmd_power,
          "power [active|idle|sleep|deepsleep]  — power management" },
        { "ls",       cmd_ls,
          "ls [path]  — list directory (default: /)" },
        { "cat",      cmd_cat,
          "cat <file>  — display file contents" },
        { "mkdir",    cmd_mkdir,
          "mkdir <path>  — create directory" },
        { "rm",       cmd_rm,
          "rm <path>  — remove file or empty directory" },
        { "df",       cmd_df,
          "df  — show filesystem statistics" },
        { "reboot",   cmd_reboot,
          "reboot  — reboot the system" },
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        shell_register_cmd(builtins[i].name, builtins[i].fn, builtins[i].help);

    return os_task_create(&shell_tcb, "shell", shell_task_fn, NULL,
                          PRIORITY_LOW);
}
