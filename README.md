# TinyOS — Ultra-Lightweight RTOS for IoT

An ultra-lightweight real-time operating system for resource-constrained IoT and embedded devices.  
Kernel footprint under 10 KB, 2 KB minimum RAM, preemptive priority-based scheduling.

---

## Features

| Category | Details |
|---|---|
| **Kernel** | Preemptive priority-based scheduling (256 levels), round-robin within same priority, O(1) priority lookup via bitmap, priority inheritance |
| **Synchronization** | Mutex (with priority inheritance), semaphore, condition variable, event groups, message queues |
| **Software Timers** | One-shot and auto-reload, millisecond precision, period change at runtime |
| **Memory** | Fixed-block pool allocator, stack overflow detection, per-task high-water mark |
| **Shell** | VT100 interactive shell — 23 built-in commands, command history (↑↓), tab completion, full line editor |
| **POSIX Compatibility** | pthreads (create/join/detach/exit, mutex, cond var) · BSD socket API (socket/bind/listen/accept/connect/send/recv, inet_pton/ntop, htons/htonl) |
| **File System** | Lightweight block-device FS, POSIX-like API, wear levelling, power-fail safe |
| **Network** | Ethernet, IPv4, ICMP, UDP, TCP, HTTP client/server, DNS |
| **TLS / DTLS** | TLS 1.2/1.3 over TCP, DTLS 1.2 over UDP (mbedTLS backend) |
| **MQTT** | Full MQTT 3.1.1 — QoS 0/1/2 with in-flight retry table, offline queue, auto-reconnect with exponential back-off |
| **CoAP** | RFC 7252 compliant client/server, observe pattern |
| **OTA** | A/B partition firmware updates, CRC32 verification, rollback |
| **Watchdog** | Hardware and software watchdog, per-task timeout monitoring |
| **Power** | Idle / Sleep / Deep-sleep modes, tickless idle, CPU frequency scaling |
| **Security** | MPU-based memory protection, secure boot support |

---

## Supported Hardware

| Architecture | Examples |
|---|---|
| ARM Cortex-M (M0/M0+/M3/M4/M7) | STM32, nRF52, Raspberry Pi Pico |
| RISC-V (RV32I) | ESP32-C3 |
| AVR (experimental) | ATmega |

---

## Quick Start

**Prerequisites:** `gcc-arm-none-eabi`

```bash
make example-blink    # LED blink
make example-shell    # Interactive shell over UART
make example-mqtt     # MQTT publish/subscribe
make example-iot      # Multi-sensor IoT node
make size             # Binary size report
```

**Minimal task example:**

```c
#include "tinyos.h"

void my_task(void *param) {
    while (1) {
        /* work */
        os_task_delay_ms(100);
    }
}

int main(void) {
    tcb_t task;
    os_init();
    os_task_create(&task, "my_task", my_task, NULL, PRIORITY_NORMAL);
    os_start();
}
```

---

## API Overview

### Task Management

```c
os_task_create(tcb, name, entry, param, priority)
os_task_delete(task)
os_task_suspend(task) / os_task_resume(task)
os_task_delay(ticks) / os_task_delay_ms(ms)
os_task_set_priority(task, priority)
os_task_get_stats(task, stats)
os_task_get_stats_by_index(index, stats)   /* iterate all tasks by index */
os_task_find_by_name(name)                 /* returns tcb_t*, NULL if not found */
os_get_system_stats(stats)
os_get_memory_stats(&free, &used, &allocs, &frees)
```

### Synchronization

```c
os_mutex_init(mutex) / os_mutex_lock(mutex, timeout) / os_mutex_unlock(mutex)
os_semaphore_init(sem, count) / os_semaphore_wait(sem, timeout) / os_semaphore_post(sem)
os_cond_init(cond) / os_cond_wait(cond, mutex, timeout)
os_cond_signal(cond) / os_cond_broadcast(cond)
os_event_group_set_bits(eg, bits) / os_event_group_wait_bits(eg, bits, opts, out, timeout)
os_queue_init(q, buf, item_size, max) / os_queue_send(q, item, timeout)
os_queue_receive(q, item, timeout) / os_queue_peek(q, item, timeout)
```

### Timers

```c
os_timer_create(timer, name, type, period_ms, callback, param)
os_timer_start(timer) / os_timer_stop(timer) / os_timer_reset(timer)
os_timer_change_period(timer, ms) / os_timer_get_remaining_ms(timer)
```

### Shell

```c
/* Register custom commands before calling shell_start() */
shell_register_cmd(name, handler_fn, help_text)

/* Provide UART I/O callbacks and start the shell task */
shell_io_t io = { .getc = uart_getc, .puts = uart_puts };
shell_start(&io)

/* Change the prompt at any time */
shell_set_prompt("mydevice> ")

/* Execute a single line programmatically */
shell_exec(line)
```

**Custom command example:**

```c
static int cmd_led(int argc, char *argv[]) {
    if (argc < 2) return 1;  /* non-zero → prints usage */
    bool on = (strcmp(argv[1], "on") == 0);
    gpio_write(LED_PIN, on);
    return 0;
}

/* In main(), before shell_start(): */
shell_register_cmd("led", cmd_led, "led <on|off>  Toggle LED");
```

#### Built-in Shell Commands

| Command | Description |
|---|---|
| `help [cmd]` | List all commands, or show detailed help for `cmd` |
| `clear` | Clear the terminal screen (VT100) |
| `echo <text>` | Print text to the terminal |
| `history` | Show command history |
| `ps` | List all tasks (state, priority, CPU%, stack usage) |
| `top` | Task list sorted by CPU usage, descending |
| `kill <name> [suspend\|resume\|delete]` | Control a task by name |
| `mem` | Heap statistics (total / used / free, alloc/free counts) |
| `ver` | TinyOS version and formatted uptime |
| `net` | Network statistics (Ethernet, IP, UDP, TCP counters) |
| `ping <ip> [count]` | Send ICMP echo requests |
| `ifconfig [ip\|netmask\|gw\|dns <addr>]` | Show or change network configuration |
| `power [active\|idle\|sleep\|deepsleep]` | Power stats or mode change |
| `ls [path]` | List directory (default: `/`) |
| `cat <file>` | Display file contents |
| `mkdir <path>` | Create directory |
| `rm <path>` | Remove file or empty directory |
| `df` | Filesystem usage statistics |
| `touch <file>` | Create empty file |
| `cp <src> <dst>` | Copy file |
| `uptime` | Show system uptime (`HH:MM:SS` or `N day(s), HH:MM:SS`) |
| `sleep <ms>` | Delay the shell task for N milliseconds |
| `reboot` | Reboot the system |

**Line editor key bindings:**

| Key | Action |
|---|---|
| `←` / `Ctrl-B` | Move cursor left |
| `→` / `Ctrl-F` | Move cursor right |
| `Home` / `Ctrl-A` | Jump to start of line |
| `End` / `Ctrl-E` | Jump to end of line |
| `↑` / `↓` | Navigate command history |
| `Tab` | Complete command name |
| `Ctrl-K` | Kill to end of line |
| `Ctrl-U` | Kill to start of line |
| `Ctrl-W` | Kill previous word |
| `Ctrl-L` | Clear screen and redraw |
| `Ctrl-C` | Cancel current line |

**Shell configuration** (`include/tinyos/shell.h`):

```c
#define SHELL_MAX_COMMANDS   32    /* max registered commands       */
#define SHELL_LINE_MAX       128   /* max input line length (bytes) */
#define SHELL_ARGV_MAX       16    /* max arguments per command     */
#define SHELL_HISTORY_DEPTH  8     /* command history entries       */
```

### Network

```c
net_init(driver, config) / net_start()
net_socket(type) / net_bind(sock, addr) / net_connect(sock, addr, timeout_ms)
net_send(sock, data, len, timeout_ms) / net_recv(sock, buf, len, timeout_ms)
net_sendto(sock, data, len, addr) / net_recvfrom(sock, buf, len, addr)
net_close(sock)
net_ping(dest_ip, timeout_ms, rtt)
net_dns_resolve(hostname, ip, timeout_ms)
net_http_get(url, response, timeout_ms)
net_http_post(url, content_type, body, len, response, timeout_ms)
```

### POSIX Compatibility

TinyOS provides two thin compatibility layers that allow standard portable code to
be compiled and run on TinyOS with minimal changes.

#### pthreads (`include/tinyos/posix_threads.h`)

Add `src/posix/posix_threads.c` to your build.

```c
#include "tinyos/posix_threads.h"

/* ── Thread ── */
pthread_t tid;
pthread_attr_t attr;
pthread_attr_init(&attr);
tinyos_pthread_attr_setpriority(&attr, PRIORITY_NORMAL); /* TinyOS extension */
pthread_create(&tid, &attr, my_fn, arg);
pthread_join(tid, &retval);
pthread_detach(tid);    /* free resources automatically on exit */
pthread_exit(retval);   /* terminate calling thread */
pthread_self();         /* handle of calling thread */

/* ── Mutex ── */
pthread_mutex_t mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_lock(&mtx);
pthread_mutex_trylock(&mtx);   /* returns EBUSY if already locked */
pthread_mutex_unlock(&mtx);

/* ── Condition variable ── */
pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_cond_wait(&cond, &mtx);
pthread_cond_timedwait(&cond, &mtx, &abstime); /* abstime relative to boot */
pthread_cond_signal(&cond);
pthread_cond_broadcast(&cond);
```

| Concept | Mapped to |
|---|---|
| `pthread_t` | Index into an internal pool of TinyOS `tcb_t` slots |
| `pthread_mutex_t` | Embeds `mutex_t` directly (zero-init / `PTHREAD_MUTEX_INITIALIZER` valid) |
| `pthread_cond_t` | Embeds `cond_var_t` directly (zero-init / `PTHREAD_COND_INITIALIZER` valid) |
| `pthread_join` | Waits on a per-thread `semaphore_t` posted by `pthread_exit` |

**Not supported:** `pthread_cancel`, thread-local storage (`pthread_key_*`),
recursive mutexes (returns `ENOTSUP`).

**Configuration** (`include/tinyos/posix_threads.h`):

```c
#define PTHREAD_MAX_THREADS  MAX_TASKS  /* max concurrent pthreads */
```

#### BSD Sockets (`include/tinyos/posix_socket.h`)

Add `src/posix/posix_socket.c` to your build.

```c
#include "tinyos/posix_socket.h"

/* ── TCP server ── */
int srv = socket(AF_INET, SOCK_STREAM, 0);

int reuse = 1;
setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

struct sockaddr_in addr = {
    .sin_family = AF_INET,
    .sin_port   = htons(8080),
    .sin_addr   = { htonl(INADDR_ANY) },
};
bind(srv, (struct sockaddr *)&addr, sizeof(addr));
listen(srv, 4);

struct sockaddr_in peer;
socklen_t plen = sizeof(peer);
int client = accept(srv, (struct sockaddr *)&peer, &plen);
recv(client, buf, sizeof(buf), 0);
send(client, response, response_len, 0);
posix_sock_close(client);   /* or define TINYOS_POSIX_WRAP_CLOSE to use close() */
posix_sock_close(srv);

/* ── TCP client ── */
int fd = socket(AF_INET, SOCK_STREAM, 0);
struct sockaddr_in dest = {
    .sin_family = AF_INET,
    .sin_port   = htons(80),
    .sin_addr   = { inet_addr("192.168.1.1") },
};
connect(fd, (struct sockaddr *)&dest, sizeof(dest));
send(fd, request, request_len, 0);
recv(fd, buf, sizeof(buf), 0);
posix_sock_close(fd);

/* ── UDP ── */
int udp = socket(AF_INET, SOCK_DGRAM, 0);
sendto(udp, data, len, 0, (struct sockaddr *)&dest, sizeof(dest));
recvfrom(udp, buf, sizeof(buf), 0, (struct sockaddr *)&src, &srclen);
posix_sock_close(udp);

/* ── Address utilities ── */
htons(port) / htonl(addr) / ntohs(n) / ntohl(n)
inet_addr("192.168.1.1")              /* → in_addr_t, network byte order */
inet_ntoa(in)                          /* → "192.168.1.1" (static buffer) */
inet_pton(AF_INET, "192.168.1.1", &in_addr)
inet_ntop(AF_INET, &in_addr, buf, sizeof(buf))
```

| BSD call | Mapped to |
|---|---|
| `socket()` | `net_socket()` |
| `bind()` | `net_bind()` |
| `listen()` | `net_listen()` |
| `accept()` | `net_accept()` |
| `connect()` | `net_connect()` |
| `send()` / `recv()` | `net_send()` / `net_recv()` |
| `sendto()` / `recvfrom()` | `net_sendto()` / `net_recvfrom()` |
| `posix_sock_close()` | `net_close()` |

**Supported `setsockopt` options:**

| Option | Effect |
|---|---|
| `SO_REUSEADDR` | Accepted; no-op (always reusable in TinyOS) |
| `SO_RCVTIMEO` | Sets receive timeout per socket (struct timeval → ms) |
| `SO_SNDTIMEO` | Sets send/connect timeout per socket |

**`close()` redirection:** define `TINYOS_POSIX_WRAP_CLOSE` before including the
header to map `close(fd)` → `posix_sock_close(fd)`.

**Not supported:** `select` / `poll` / `epoll`, non-blocking mode (`O_NONBLOCK`),
IPv6 (`AF_INET6` returns `EAFNOSUPPORT`).

---

### TLS / DTLS

TLS 1.2/1.3 over TCP and DTLS 1.2 over UDP backed by **mbedTLS**.  
Enable at build time with `-DTINYOS_TLS_ENABLE` and link against mbedTLS.

```c
/* Client (TLS over TCP) */
tls_context_t tls;
tls_config_t  cfg = TLS_CONFIG_DEFAULT_CLIENT;
cfg.ca_cert     = ca_cert_pem;
cfg.ca_cert_len = sizeof(ca_cert_pem);
tls_init(&tls, &cfg);

net_socket_t sock = net_socket(SOCK_STREAM);
net_connect(sock, &broker_addr, 5000);
tls_connect(&tls, sock, "example.com", 5000);

tls_send(&tls, data, len);
tls_recv(&tls, buf, sizeof(buf), 5000);
tls_close(&tls);

/* Server (TLS accept) */
tls_config_t srv_cfg = TLS_CONFIG_DEFAULT_SERVER;
srv_cfg.cert     = server_cert_pem;
srv_cfg.cert_len = sizeof(server_cert_pem);
srv_cfg.key      = server_key_pem;
srv_cfg.key_len  = sizeof(server_key_pem);
tls_init(&tls, &srv_cfg);
tls_accept(&tls, client_sock, 5000);

/* DTLS over UDP */
tls_config_t dtls_cfg = TLS_CONFIG_DEFAULT_DTLS_CLIENT;
net_socket_t usock = net_socket(SOCK_DGRAM);
tls_connect_dtls(&tls, usock, "example.com", 5000);
```

### MQTT

Full MQTT 3.1.1 with per-message QoS delivery guarantees.

```c
mqtt_config_t cfg = {
    .broker_host          = "mqtt.example.com",
    .client_id            = "tinyos-01",
    .keepalive_sec        = 60,
    .clean_session        = true,
    .auto_reconnect       = true,
    .reconnect_interval_ms = 3000,   /* base; doubles each attempt (max 60 s) */
};
mqtt_client_t client;
mqtt_client_init(&client, &cfg);
mqtt_set_connection_callback(&client, on_connect, NULL);
mqtt_set_message_callback(&client, on_message, NULL);
mqtt_connect(&client);

/* Publish — QoS1/2 are buffered offline if disconnected */
mqtt_publish(&client, "sensors/temp", "23.5", 4, MQTT_QOS_1, false);

/* Inspect reliability queues */
uint8_t in_flight = mqtt_get_inflight_count(&client);  /* sent, awaiting ACK  */
uint8_t pending   = mqtt_get_pending_count(&client);   /* queued while offline */

mqtt_subscribe(&client, "cmd/#", MQTT_QOS_1);
mqtt_flush_pending(&client);   /* discard offline queue */
mqtt_disconnect(&client);
```

#### MQTT Reliability Model

```
QoS 0  ─── fire-and-forget; dropped if disconnected
QoS 1  ─── in-flight table tracks each PUBLISH until PUBACK
              ↳ retransmits with DUP=1 every 5 s, up to 5 times
              ↳ if offline → offline queue (up to 8 messages)
QoS 2  ─── full PUBLISH → PUBREC → PUBREL → PUBCOMP handshake
              ↳ each step is retried independently on timeout

Auto-reconnect back-off: 3 s → 6 s → 12 s → … → 60 s (cap)
On reconnect: re-subscribes all topics, flushes offline queue
```

**MQTT reliability configuration** (`include/tinyos/mqtt.h`):

```c
#define MQTT_MAX_INFLIGHT          8      /* in-flight slots       */
#define MQTT_MAX_PENDING           8      /* offline queue slots   */
#define MQTT_MAX_PAYLOAD_SIZE      512    /* bytes per queued msg  */
#define MQTT_RETRY_INTERVAL_MS     5000   /* retry after (ms)      */
#define MQTT_MAX_RETRY_COUNT       5      /* retries before drop   */
#define MQTT_RECONNECT_BASE_MS     3000   /* first reconnect delay */
#define MQTT_RECONNECT_MAX_MS      60000  /* backoff ceiling       */
```

### CoAP

```c
coap_init(ctx, config, is_server) / coap_start(ctx) / coap_stop(ctx)
coap_get(ctx, ip, port, path, response, timeout_ms)
coap_post(ctx, ip, port, path, format, payload, len, response, timeout_ms)
coap_resource_create(ctx, path, handler, user_data)
coap_process(ctx, timeout_ms)
```

### OTA

```c
ota_init(config)
ota_start_update(url, callback, user_data)
ota_write_chunk(data, size, offset) / ota_finalize_update()
ota_confirm_boot() / ota_rollback()
ota_verify_partition(type)
```

### File System

```c
fs_format(device) / fs_mount(device) / fs_unmount()
fs_open(path, flags) / fs_close(fd)
fs_read(fd, buf, size) / fs_write(fd, buf, size)
fs_seek(fd, offset, whence) / fs_tell(fd)
fs_mkdir(path) / fs_remove(path) / fs_rmdir(path)
fs_stat(path, stat)
fs_opendir(path) / fs_readdir(dir, entry) / fs_closedir(dir)
fs_get_stats(stats) / fs_get_free_space() / fs_is_mounted()
```

### Power Management

```c
os_power_init()
os_power_set_mode(mode)          /* ACTIVE / IDLE / SLEEP / DEEP_SLEEP */
os_power_get_mode()
os_power_enter_sleep(duration_ms)
os_power_enter_deep_sleep(duration_ms)
os_power_enable_tickless_idle(enable)
os_power_set_cpu_frequency(freq_hz)
os_power_configure_wakeup(source, enable)
os_power_get_stats(stats)
os_power_get_consumption_mw()
os_power_estimate_battery_life_hours()
```

### Watchdog

```c
wdt_init(config) / wdt_start() / wdt_stop()
wdt_feed() / wdt_set_timeout(ms)
wdt_register_task(task, timeout_ms) / wdt_feed_task(task)
```

---

## Configuration

**`include/tinyos.h`** — kernel and OS:

```c
#define MAX_TASKS        8      /* max concurrent tasks           */
#define STACK_SIZE       256    /* stack size per task (words)    */
#define TICK_RATE_HZ     1000   /* scheduler tick frequency (Hz)  */
#define TIME_SLICE_MS    10     /* round-robin time slice (ms)    */
```

**`include/tinyos/shell.h`** — interactive shell:

```c
#define SHELL_MAX_COMMANDS   32   /* max registered commands         */
#define SHELL_LINE_MAX       128  /* max input line length (bytes)   */
#define SHELL_ARGV_MAX       16   /* max arguments per command       */
#define SHELL_HISTORY_DEPTH  8    /* command history ring buffer     */
```

**`include/tinyos/mqtt.h`** — MQTT reliability:

```c
#define MQTT_MAX_INFLIGHT        8      /* in-flight QoS1/2 slots    */
#define MQTT_MAX_PENDING         8      /* offline queue slots       */
#define MQTT_MAX_PAYLOAD_SIZE    512    /* max queued payload bytes  */
#define MQTT_RETRY_INTERVAL_MS   5000   /* unACKed retry interval    */
#define MQTT_MAX_RETRY_COUNT     5      /* retries before discard    */
#define MQTT_RECONNECT_BASE_MS   3000   /* initial reconnect delay   */
#define MQTT_RECONNECT_MAX_MS    60000  /* back-off ceiling          */
```

**TLS** — requires mbedTLS; enable with:

```makefile
CFLAGS += -DTINYOS_TLS_ENABLE
LDFLAGS += -lmbedtls -lmbedcrypto -lmbedx509
```

---

## Project Structure

```
tinyos-rtos/
├── include/
│   ├── tinyos.h              # Core API (tasks, sync, timers, memory, FS, power)
│   └── tinyos/
│       ├── shell.h           # Interactive shell API & configuration
│       ├── net.h             # Network stack
│       ├── tls.h             # TLS 1.2/1.3 + DTLS 1.2 (mbedTLS)
│       ├── mqtt.h            # MQTT 3.1.1 client
│       ├── coap.h            # CoAP RFC 7252
│       ├── ota.h             # OTA firmware updates
│       ├── watchdog.h        # Watchdog timer
│       ├── posix_threads.h   # POSIX pthreads compatibility layer
│       └── posix_socket.h    # BSD socket compatibility layer
├── src/
│   ├── kernel.c              # Preemptive scheduler & task management
│   ├── sync.c                # Mutex, semaphore, queue, condition var, event groups
│   ├── timer.c               # Software timers
│   ├── memory.c              # Heap allocator
│   ├── shell.c               # Interactive shell (VT100, history, tab completion)
│   ├── filesystem.c          # Block-device file system
│   ├── security.c            # MPU memory protection
│   ├── power.c               # Power management & CPU frequency scaling
│   ├── watchdog.c            # Watchdog (HW + SW, per-task monitoring)
│   ├── bootloader.c          # Secure bootloader
│   ├── ota.c                 # OTA A/B partition updates
│   ├── mqtt.c                # MQTT client (in-flight table, offline queue)
│   ├── coap.c                # CoAP client/server
│   ├── net/
│   │   ├── network.c         # Core & buffer management
│   │   ├── ethernet.c        # Ethernet / ARP
│   │   ├── ip.c              # IPv4 / ICMP
│   │   ├── socket.c          # UDP / TCP socket API
│   │   ├── http_dns.c        # HTTP client & DNS resolver
│   │   └── tls.c             # TLS/DTLS (mbedTLS wrapper)
│   └── posix/
│       ├── posix_threads.c   # pthreads → TinyOS task/sync wrapper
│       └── posix_socket.c    # BSD socket → net_* wrapper
├── drivers/
│   ├── flash.c / flash.h     # Flash memory driver
│   ├── ramdisk.c / ramdisk.h # RAM disk (testing)
│   └── loopback_net.c        # Loopback network driver (testing)
└── examples/
    ├── blink_led.c           # GPIO blink
    ├── iot_sensor.c          # Multi-task sensor node
    ├── shell_demo.c          # Custom shell commands over UART
    ├── network_demo.c        # TCP/UDP/HTTP/ping
    ├── tls_demo.c            # TLS client/server
    ├── mqtt_demo.c           # MQTT publish/subscribe (QoS 1/2)
    ├── coap_demo.c           # CoAP client/server
    ├── ota_demo.c            # Firmware update flow
    ├── filesystem_demo.c     # File I/O
    ├── watchdog_demo.c       # Watchdog configuration
    ├── low_power.c           # Power mode transitions
    ├── software_timers.c     # Timer creation and callbacks
    ├── event_groups.c        # Event synchronisation
    ├── condition_variable.c  # Producer/consumer
    ├── priority_adjustment.c # Dynamic priority
    ├── task_statistics.c     # CPU and stack monitoring
    └── posix_compat_demo.c   # POSIX pthreads + socket usage examples
```

---

## Performance

| Component | ROM | RAM |
|---|---|---|
| Kernel | 6 KB | 512 B |
| Per task | — | ~1 KB |
| Mutex | — | 12 B |
| Semaphore | — | 8 B |
| Message queue (10 items) | — | 40 B + data |
| Shell (with 23 built-ins) | ~4 KB | ~2.5 KB |
| MQTT client (with queues) | ~8 KB | ~10 KB |
| POSIX threads layer | ~2 KB | ~`PTHREAD_MAX_THREADS` × (tcb_t + 32 B) |
| POSIX socket layer | ~1 KB | ~`NET_MAX_SOCKETS` × 12 B |

| Architecture | Context switch |
|---|---|
| Cortex-M0 | ~2 μs |
| Cortex-M4 | ~1 μs |
| RISC-V | ~1.5 μs |

**System requirements:** 2 KB RAM minimum · < 10 KB ROM for kernel alone

---

## License

MIT License — see `LICENSE` for details.
