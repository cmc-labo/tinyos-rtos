# TinyOS — Ultra-Lightweight RTOS for IoT

An ultra-lightweight real-time operating system for IoT and embedded devices.
Kernel footprint under 10KB, 2KB minimum RAM, preemptive priority-based scheduling.

## Features

- **Scheduler** — Preemptive, priority-based (256 levels), round-robin within same priority, priority inheritance
- **Synchronization** — Mutex, semaphore, condition variable, event groups, message queues
- **Software Timers** — One-shot and auto-reload, millisecond precision
- **Memory** — Fixed-block pool allocator, stack overflow detection
- **File System** — Lightweight block-device FS with POSIX-like API
- **Network** — Ethernet, IPv4, ICMP, UDP, TCP, HTTP client/server, DNS
- **MQTT** — Full MQTT 3.1.1 client with QoS 0/1/2 and auto-reconnect
- **CoAP** — RFC 7252 compliant client/server with observe pattern
- **OTA** — A/B partition firmware updates with CRC32 and rollback
- **Watchdog** — Hardware and software watchdog with per-task monitoring
- **Power** — Idle, sleep, and deep-sleep modes with tickless idle
- **Security** — MPU-based memory protection, secure boot support

## Supported Hardware

| Architecture | Examples |
|---|---|
| ARM Cortex-M (M0/M0+/M3/M4/M7) | STM32, nRF52, Raspberry Pi Pico |
| RISC-V (RV32I) | ESP32-C3 |
| AVR (experimental) | ATmega |

## Quick Start

**Prerequisites**: `gcc-arm-none-eabi`

```bash
make example-blink    # Build LED blink example
make example-iot      # Build IoT sensor example
make size             # Show binary size
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

## API Overview

### Task Management
```c
os_task_create(tcb, name, entry, param, priority)
os_task_delete(task)
os_task_suspend(task) / os_task_resume(task)
os_task_delay(ticks) / os_task_delay_ms(ms)
os_task_set_priority(task, priority)
os_task_get_stats(task, stats)
```

### Synchronization
```c
os_mutex_init(mutex) / os_mutex_lock(mutex, timeout) / os_mutex_unlock(mutex)
os_semaphore_init(sem, count) / os_semaphore_wait(sem, timeout) / os_semaphore_post(sem)
os_semaphore_get_count(sem)
os_cond_init(cond) / os_cond_wait(cond, mutex, timeout)
os_cond_signal(cond) / os_cond_broadcast(cond)
os_event_group_set_bits(eg, bits) / os_event_group_wait_bits(eg, bits, opts, out, timeout)
os_queue_init(q, buf, item_size, max) / os_queue_send(q, item, timeout)
os_queue_receive(q, item, timeout) / os_queue_peek(q, item, timeout)
os_queue_get_count(q)
```

### Timers
```c
os_timer_create(timer, name, type, period_ms, callback, param)
os_timer_start(timer) / os_timer_stop(timer) / os_timer_reset(timer)
os_timer_change_period(timer, ms) / os_timer_get_remaining_ms(timer)
```

### Network
```c
net_init(driver, config) / net_start()
net_socket(type) / net_bind(sock, addr) / net_connect(sock, addr, timeout_ms)
net_send(sock, data, len, timeout_ms) / net_recv(sock, buf, len, timeout_ms)
net_sendto(sock, data, len, addr)    / net_recvfrom(sock, buf, len, addr)
net_close(sock)
net_ping(dest_ip, timeout_ms, rtt)
net_http_get(url, response, timeout_ms)
net_http_post(url, content_type, body, len, response, timeout_ms)
net_dns_resolve(hostname, ip, timeout_ms)
```

### MQTT
```c
mqtt_client_init(client, config)
mqtt_connect(client) / mqtt_disconnect(client)
mqtt_publish(client, topic, payload, len, qos, retained)
mqtt_subscribe(client, topic, qos) / mqtt_unsubscribe(client, topic)
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
fs_mkdir(path) / fs_remove(path) / fs_stat(path, stat)
```

### Watchdog
```c
wdt_init(config) / wdt_start() / wdt_stop()
wdt_feed() / wdt_set_timeout(ms)
wdt_register_task(task, timeout_ms) / wdt_feed_task(task)
```

## Configuration

Edit `include/tinyos.h`:

```c
#define MAX_TASKS        8      /* max concurrent tasks */
#define STACK_SIZE       256    /* stack size per task (words) */
#define TICK_RATE_HZ     1000   /* scheduler tick frequency */
#define TIME_SLICE_MS    10     /* round-robin time slice */
#define MEMORY_POOL_SIZE 4096   /* heap size (bytes) */
```

## Project Structure

```
tinyos-rtos/
├── include/
│   ├── tinyos.h          # Core API (tasks, sync, timers, memory, FS, power)
│   └── tinyos/
│       ├── net.h         # Network stack
│       ├── mqtt.h        # MQTT client
│       ├── coap.h        # CoAP client/server
│       ├── ota.h         # OTA updates
│       └── watchdog.h    # Watchdog timer
├── src/
│   ├── kernel.c          # Scheduler & task management
│   ├── sync.c            # Mutex, semaphore, queue, event groups
│   ├── timer.c           # Software timers
│   ├── memory.c          # Memory allocator
│   ├── filesystem.c      # File system
│   ├── security.c        # MPU & security
│   ├── power.c           # Power management
│   ├── watchdog.c        # Watchdog
│   ├── bootloader.c      # Bootloader
│   ├── ota.c             # OTA firmware updates
│   ├── mqtt.c            # MQTT client
│   ├── coap.c            # CoAP client/server
│   └── net/
│       ├── network.c     # Core & buffer management
│       ├── ethernet.c    # Ethernet / ARP
│       ├── ip.c          # IPv4 / ICMP
│       ├── socket.c      # UDP / TCP socket API
│       └── http_dns.c    # HTTP client & DNS
├── drivers/
│   ├── flash.c/h         # Flash memory driver
│   ├── ramdisk.c/h       # RAM disk (testing)
│   └── loopback_net.c    # Loopback network driver (testing)
└── examples/
    ├── blink_led.c
    ├── iot_sensor.c
    ├── network_demo.c
    ├── mqtt_demo.c
    ├── coap_demo.c
    ├── ota_demo.c
    ├── filesystem_demo.c
    ├── watchdog_demo.c
    ├── low_power.c
    ├── software_timers.c
    ├── event_groups.c
    ├── condition_variable.c
    ├── priority_adjustment.c
    └── task_statistics.c
```

## Performance

| Component | ROM | RAM |
|---|---|---|
| Kernel | 6 KB | 512 B |
| Per task | — | ~1 KB |
| Mutex | — | 12 B |
| Semaphore | — | 8 B |
| Message queue (10 items) | — | 40 B + data |

| Architecture | Context switch |
|---|---|
| Cortex-M0 | ~2 μs |
| Cortex-M4 | ~1 μs |
| RISC-V | ~1.5 μs |

## License

MIT License — see `LICENSE` for details.
