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
| **Memory** | First-fit allocator with immediate coalescing (8 KB heap, 8-byte aligned), stack overflow detection, per-task high-water mark |
| **Shell** | VT100 interactive shell — 23 built-in commands, command history (↑↓), tab completion, full line editor |
| **POSIX Compatibility** | pthreads (create/join/detach/exit, mutex, cond var) · BSD socket API (socket/bind/listen/accept/connect/send/recv, inet_pton/ntop, htons/htonl) |
| **File System** | Journaling block-device FS (WAL, crash recovery), COW block sharing, atomic snapshots, POSIX-like API |
| **Network** | Ethernet, IPv4, ICMP, UDP, TCP, HTTP client/server, DNS |
| **TLS / DTLS** | TLS 1.2/1.3 over TCP, DTLS 1.2 over UDP (mbedTLS backend) |
| **MQTT** | Full MQTT 3.1.1 — QoS 0/1/2 with in-flight retry table, offline queue, auto-reconnect with exponential back-off |
| **CoAP** | RFC 7252 compliant client/server, observe pattern |
| **OTA** | A/B partition firmware updates, CRC32 verification, rollback |
| **Watchdog** | Hardware and software watchdog, per-task timeout monitoring |
| **Power** | Idle / Sleep / Deep-sleep modes, tickless idle, CPU frequency scaling |
| **Security** | MPU-based memory protection, secure boot support |
| **HAL** | Generic Hardware Abstraction Layer — ARM Cortex-M / RISC-V / AVR; compile-time arch selection, peripheral op-tables |

---

## Supported Hardware

| Architecture | Examples |
|---|---|
| ARM Cortex-M (M0/M0+/M3/M4/M7) | STM32, nRF52, Raspberry Pi Pico |
| RISC-V (RV32I) | ESP32-C3 |
| AVR (experimental) | ATmega |

---

## Quick Start

### Prerequisites

```bash
# ARM cross-compiler (required)
sudo apt-get install -y gcc-arm-none-eabi binutils-arm-none-eabi

# QEMU ARM emulator (optional — for running without hardware)
sudo apt-get install -y qemu-system
```

Verify installation:

```bash
arm-none-eabi-gcc --version   # 10.x or later
qemu-system-arm --version     # 6.x or later
```

### Build

```bash
# Default example (blink_led) — ARM Cortex-M4
make

# Target a different architecture (auto-selects toolchain and HAL)
make ARCH=cortex-m0           # Cortex-M0/M0+
make ARCH=cortex-m7           # Cortex-M7
make ARCH=riscv32             # RISC-V RV32I  (uses riscv32-unknown-elf-gcc)
make ARCH=avr5                # AVR ATmega    (uses avr-gcc)

# Specific example
make EXAMPLE=blink_led        # LED blink + task scheduler demo
make EXAMPLE=event_groups     # Event group AND/OR/NOT/SYNC demo
make EXAMPLE=iot_sensor       # Multi-sensor IoT node
make EXAMPLE=shell_demo       # Interactive UART shell
make EXAMPLE=mqtt_demo        # MQTT publish/subscribe
make EXAMPLE=condition_variable  # Producer/consumer

# Convenience aliases
make example-blink
make example-events
make example-shell
make example-mqtt
make example-iot

# Build output
make size                     # Print ROM/RAM usage
```

Build artifacts are placed in `build/`:

| File | Description |
|---|---|
| `build/tinyos.elf` | ELF image with debug symbols |
| `build/tinyos.bin` | Raw binary for flashing |
| `build/tinyos.map` | Linker map (symbol sizes) |

### Run on QEMU

TinyOS runs on the QEMU `mps2-an385` target (ARM Cortex-M3, 4 MB flash, 4 MB RAM):

```bash
# Run indefinitely (Ctrl-A X to quit)
qemu-system-arm \
    -machine mps2-an385 \
    -cpu cortex-m3 \
    -nographic \
    -kernel build/tinyos.elf

# Run for a fixed duration (e.g. 10 seconds)
timeout 10 qemu-system-arm \
    -machine mps2-an385 \
    -cpu cortex-m3 \
    -nographic \
    -kernel build/tinyos.elf

# Debug: trace interrupts
qemu-system-arm \
    -machine mps2-an385 \
    -cpu cortex-m3 \
    -nographic \
    -d int \
    -kernel build/tinyos.elf
```

Expected output from the interrupt trace: repeated `successful exception return` lines confirm the scheduler is running, SysTick is ticking, and PendSV context switches are completing cleanly.

### Flash to Hardware

```bash
# OpenOCD (STM32 example)
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
    -c "program build/tinyos.bin verify reset exit 0x08000000"

# pyOCD (generic ARM Cortex-M)
pyocd flash --target cortex_m build/tinyos.bin
```

### Build with TLS (mbedTLS)

TLS support is enabled automatically when mbedTLS is present at `~/mbedtls`.
To use a different path:

```bash
# Clone and build mbedTLS
git clone https://github.com/Mbed-TLS/mbedtls ~/mbedtls
make -C ~/mbedtls

# Build TinyOS with TLS
make MBEDTLS_DIR=~/mbedtls
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

/* Copy-on-Write snapshots */
fs_snapshot(source_path, snapshot_name)   /* atomic COW snapshot of a file */
fs_get_block_refcount(block_nr)           /* reference count of a data block */
```

#### Journaling (Write-Ahead Log)

The filesystem uses **metadata-only journaling** (ordered mode).  Before any
inode, bitmap, or directory block is modified, a journal record is written to a
dedicated 32-block WAL area at the start of the partition.  On the next mount
after a crash, `fs_mount` replays the journal and restores a consistent state.

```
Disk layout (FS_BLOCK_SIZE = 512 bytes)
 Block 0      Superblock  (version 0x00020000 — v2 with journaling)
 Block 1      Block bitmap
 Block 2      Journal header
 Blocks 3-33  Journal data  (31 slots)
 Blocks 34-41 Inode table   (8 blocks, 128 inodes)
 Block 42+    Data blocks
```

#### Copy-on-Write (COW)

Each data block carries an in-memory reference count rebuilt from the inode
table at mount time.  Writing to a block that is shared (`refcount > 1`)
allocates a private copy first — the original block's count is decremented and
all changes happen to the new block.

`fs_snapshot()` creates an atomic point-in-time snapshot by duplicating the
source inode (same block pointers, bumped refcounts) inside a single journal
transaction.  Either the full snapshot is committed or nothing changes.

```c
/* Create a snapshot of /data/config → /snapshots/config-20260101 */
fs_snapshot("/data/config", "/snapshots/config-20260101");

/* Inspect sharing */
uint8_t rc = fs_get_block_refcount(42);   /* 1 = private, >1 = shared */
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

### HAL (Hardware Abstraction Layer)

TinyOS provides a generic HAL that hides all architecture-specific register
access behind a stable C interface.  The architecture is selected at **compile
time** via the `ARCH` Makefile variable; no `#ifdef` guards appear in kernel
code.

#### Portable primitives (declared `static inline` in each arch header)

```c
uint32_t hal_irq_save(void)           /* disable IRQs, return saved state   */
void     hal_irq_restore(uint32_t s)  /* restore IRQ state                  */
void     hal_context_switch_trigger() /* pend PendSV / raise MSIP / Timer0  */
void     hal_cpu_wait_for_interrupt() /* WFI / wfi / sleep instruction      */
void     hal_cpu_dsb(void)            /* data synchronization barrier        */
void     hal_cpu_isb(void)            /* instruction synchronization barrier */
```

#### Non-inline functions (implemented per arch in `hal/<arch>/hal_<arch>.c`)

```c
void     hal_init(void)
void     hal_tick_init(uint32_t core_clock_hz, uint32_t tick_rate_hz)
void     hal_tick_suppress(uint32_t max_ticks)
uint32_t hal_tick_unsuppress(void)
uint32_t hal_core_clock_hz(void)

bool     hal_cycle_counter_init(void)
void     hal_cycle_counter_reset(void)
uint32_t hal_cycle_counter_read(void)

bool     hal_mpu_init(uint8_t *region_count)
int      hal_mpu_configure_region(uint8_t region, uint32_t base,
                                   uint32_t size, uint32_t attrs)
void     hal_mpu_enable(bool allow_privileged_default)
void     hal_mpu_disable(void)

void     hal_irq_set_priority(int irq_num, uint8_t priority)
void     hal_system_reset(void)          /* does not return */
void     hal_fault_capture(const uint32_t *frame, hal_fault_info_t *info)
```

#### Board-level platform registry

Register board peripherals once at startup; the kernel and power manager query
the table via `hal_platform_get()` instead of using weak symbols.

```c
static const hal_uart_ops_t  my_uart  = { .init = ..., .putc = ... };
static const hal_power_ops_t my_power = { .enter_sleep = ...,
                                           .set_clock_hz = ... };

static const hal_platform_t board = {
    .uart[0] = &my_uart,
    .power   = &my_power,
};

hal_platform_register(&board);   /* call before os_start() */
```

`hal_platform_t` provides slots for `uart[4]`, `flash`, `gpio`, `spi[4]`,
`i2c[4]`, and `power`.  Any `NULL` pointer means "not present on this board".

#### Architecture support matrix

| Architecture | Tick source | Cycle counter | MPU / PMP | Context switch trigger |
|---|---|---|---|---|
| Cortex-M0/M0+/M3/M4/M7 | SysTick | DWT CYCCNT (M3+) | MPU | PendSV via ICSR |
| RISC-V RV32I/IM | CLINT MTIMECMP | `rdcycle` CSR | PMP (4 regions) | MSIP software interrupt |
| AVR ATmega/ATtiny | Timer0 CTC | — (returns 0) | — | Timer0 overflow |

---

## Configuration

**`include/tinyos.h`** — kernel and OS:

```c
#define MAX_TASKS              16     /* max concurrent tasks                    */
#define STACK_SIZE             256    /* stack size per task (words)             */
#define TICK_RATE_HZ           1000   /* scheduler tick frequency (Hz)           */
#define TIME_SLICE_MS          10     /* round-robin time slice (ms)             */
#define TICKLESS_MAX_SLEEP_TICKS 100U /* tickless idle: max ticks per WFI sleep  */
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
│   ├── startup.s             # Vector table, Reset_Handler, SysTick/SVC/PendSV stubs
│   ├── context_switch.s      # Thumb-2: PendSV_Handler, SVC_Handler, os_pend_sv
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
│   │   └── tls.c             # TLS/DTLS (mbedTLS wrapper, excluded when mbedTLS absent)
│   └── posix/
│       ├── posix_threads.c   # pthreads → TinyOS task/sync wrapper
│       └── posix_socket.c    # BSD socket → net_* wrapper
├── hal/
│   ├── hal.h                 # Portable HAL interface (arch-agnostic API + peripheral op-tables)
│   ├── cortex_m/
│   │   ├── hal_cortex_m.h    # Register defines + static inline primitives (irq_save, WFI, DSB …)
│   │   └── hal_cortex_m.c    # SysTick, DWT, MPU, AIRCR reset, fault capture
│   ├── riscv/
│   │   ├── hal_riscv.h       # csrrci/csrw inline primitives, CLINT defines
│   │   └── hal_riscv.c       # CLINT tick, rdcycle counter, PMP, PLIC priority, CSR fault capture
│   └── avr/
│       ├── hal_avr.h         # SREG-based irq_save, Timer0 context-switch trigger
│       └── hal_avr.c         # Timer0 CTC tick, watchdog reset, stub MPU/cycle-counter
├── drivers/
│   ├── flash.c / flash.h     # Flash memory driver
│   ├── ramdisk.c / ramdisk.h # RAM disk (testing)
│   └── loopback_net.c        # Loopback network driver (testing)
├── linker.ld                 # Linker script (mps2-an385: Flash 0x0/4MB, RAM 0x20000000/4MB)
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
    ├── event_flags_logic.c   # AND / OR / NOT(CLEAR) / SYNC(barrier) patterns
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

## Changelog

### v2.0.0

**New features**

- **Journaling filesystem** (`src/filesystem.c`) — Write-Ahead Log (WAL) protects
  all metadata writes (inodes, bitmap, superblock, directories).  
  On-disk format bumped to `FS_VERSION 0x00020000`.  Journal occupies 32 blocks
  starting at block 2; `fs_mount` replays any committed but unapplied transaction
  before handing control to the application.  Metadata-only (ordered) journaling
  keeps write amplification low while guaranteeing a consistent filesystem after
  a power failure or reset at any point during a write.

- **Copy-on-Write block sharing** (`src/filesystem.c`) — Every data block carries
  an in-memory reference count (rebuilt from the inode table at mount time).
  Writing to a shared block silently allocates a private copy; the shared block's
  count is decremented.  `fs_snapshot(source, name)` creates an atomic
  point-in-time snapshot inside a single journal transaction — either all or
  nothing is committed.  New public API: `fs_snapshot()` and
  `fs_get_block_refcount()` (see `include/tinyos.h`).

- **Generic HAL** (`hal/`) — A two-tier Hardware Abstraction Layer decouples the
  kernel from ARM Cortex-M-specific assembly.
  - `hal/hal.h` — architecture-agnostic interface: tick, cycle counter, MPU,
    IRQ save/restore, context-switch trigger, system reset, fault capture, and
    peripheral op-tables (`hal_platform_t` with UART / flash / GPIO / SPI / I²C /
    power slots).
  - `hal/cortex_m/` — full Cortex-M0–M7 implementation (SysTick, DWT, AIRCR,
    PRIMASK, PendSV trigger).
  - `hal/riscv/` — RISC-V RV32 stub (CLINT MTIMECMP tick, `rdcycle` counter,
    PMP, PLIC priority, CSR fault capture).
  - `hal/avr/` — AVR ATmega stub (Timer0 CTC tick, SREG critical sections,
    watchdog reset).
  - Makefile auto-selects the HAL from `ARCH=` (`cortex-m*` / `riscv*` / `avr*`)
    and passes `-DHAL_ARCH_*` to the compiler.

**Breaking changes**

- `src/kernel.c` no longer contains bare `SYST_*`, `SCB_SHPR3`, `SCB_AIRCR`, or
  `DWT_*` register defines — these are now supplied by the HAL.  Code that
  directly referenced these macros must be updated to use the HAL API.
- `src/power.c` weak symbols (`platform_enter_sleep_mode`, etc.) now delegate to
  `hal_platform_t->power` when a platform is registered; boards that relied on
  the old `__asm__ volatile("wfi")` default will see identical behaviour unless
  a `hal_power_ops_t` is registered.
- Filesystem on-disk version is `0x00020000`.  Volumes formatted with v1.x must
  be reformatted (`fs_format`).

---

### v1.2.0

**Bug fixes**

- **`startup.s` vector table** — `MemManage_Handler`, `BusFault_Handler`, and `UsageFault_Handler` now point to the correct handlers in `fault.c`.  
  Previously all three entries resolved to `Default_Handler`, so MPU, bus, and usage faults were silently swallowed instead of triggering the diagnostic dump.
- **`os_cond_wait` double-decrement** — `cond_remove_task()` already decrements `waiting_count`; the three call sites that decremented it again afterward have been fixed.  
  Previously a spurious extra decrement could underflow the counter and corrupt condition variable state.
- **Mutex PIP boost not released on timeout** — the timed spin path in `os_mutex_lock` now calls `mutex_pip_recalculate()` before returning `OS_ERROR_TIMEOUT`.  
  Previously the priority boost applied to the owner was never undone when the waiting task gave up, causing permanent priority inflation.

**Improvements**

- **Memory allocator rewrite** (`src/memory.c`) — replaced the fixed 32-byte block pool with a first-fit allocator featuring:  
  - 8 KB heap (up from 4 KB), 8-byte aligned blocks  
  - Free list kept in address order; adjacent free blocks are coalesced immediately on every `os_free()` (forward and backward)  
  - Blocks are split on allocation when the remainder is large enough to be useful (`≥ BLK_HDR + ALIGN`)
- **`os_init()` initialization order** — `os_mem_init()` and `os_power_init()` are now called before `os_timer_init()`, so the heap and power subsystem are ready before any timer callbacks or task code runs.
- **Tickless idle implemented** (`src/kernel.c: os_kernel_tickless_sleep`) — when enabled via `os_power_enable_tickless_idle(true)`, the idle task suppresses SysTick before `WFI` and uses the DWT `CYCCNT` cycle counter to measure actual elapsed time.  
  After wakeup SysTick is restarted, `kernel.tick_count` is advanced by the measured ticks (capped at `TICKLESS_MAX_SLEEP_TICKS`), and `delay_queue_tick()` is called to immediately unblock any overdue tasks.  
  Previously the flag existed but the idle path always fell through to a plain `WFI` with SysTick running.
- **`MAX_TASKS` increased** — raised from 8 to 16 to support more realistic IoT workloads without user-side configuration changes.

---

### v1.1.0

**New features**

- `EVENT_WAIT_CLEAR` flag for event groups — wait until bits become **clear** (NOT condition).  
  `EVENT_WAIT_ALL | EVENT_WAIT_CLEAR` wakes when all masked bits are 0; `EVENT_WAIT_ANY | EVENT_WAIT_CLEAR` wakes when any is 0.
- `os_event_group_sync()` — rendezvous / barrier primitive.  
  Each participating task sets its own arrival bit and blocks until the full set is present; all unblock simultaneously.
- New example `examples/event_flags_logic.c` demonstrating all four modes (AND, OR, NOT, SYNC).

**Build & runtime fixes**

- Added `src/startup.s`: vector table, `Reset_Handler` (.data copy, .bss zero), `SysTick_Handler` stub, `HardFault_Handler`.
- Added `linker.ld`: memory layout for `mps2-an385` (Flash `0x00000000` / 4 MB, RAM `0x20000000` / 4 MB).
- Fixed initial task stack: R4–R11 save area is now pre-allocated so PendSV's `LDMIA {R4-R11}` works correctly on first context switch into any newly created task.
- `SVC_Handler` updated to `LDMIA {R4-R11}` before setting PSP, keeping both SVC and PendSV paths symmetric.
- `MPU_TYPE` check in `os_mpu_configure_default()` — MPU setup is skipped silently when no MPU is present (QEMU `mps2-an385`).
- Renamed `timer_t` → `os_timer_t` to avoid conflict with POSIX `<sys/types.h>`.
- Makefile: `-I.` added for `drivers/` includes; TLS sources excluded automatically when mbedTLS is absent; `-Wno-stringop-truncation` for false-positive strncpy warnings.
- Fixed sign-compare, unused-function, uninitialized-variable, and implicit-declaration warnings across `coap.c`, `filesystem.c`, `mqtt.c`, `ota.c`, `security.c`, `net/ip.c`, `net/http_dns.c`.

---

## License

MIT License — see `LICENSE` for details.
