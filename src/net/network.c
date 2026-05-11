/**
 * @file network.c
 * @brief Network Stack Core - Buffer Management and Main Loop
 */

#include "tinyos/net.h"
#include "tinyos/stdio_uart.h"
#include <string.h>

#define TAG "net"

/*===========================================================================
 * Internal Data Structures
 *===========================================================================*/

/* Network driver */
static net_driver_t *current_driver = NULL;

/* Network configuration */
static net_config_t current_config;

/* Network statistics */
static net_stats_t net_statistics;

/* Network task */
static tcb_t network_task;

/* External functions from other network modules */
extern void net_ethernet_init(void);
extern void net_ethernet_input(const uint8_t *data, uint16_t length);

extern void net_ip_init(void);
extern void net_ip_input(const uint8_t *data, uint16_t length, const mac_addr_t *src_mac);

extern void net_icmp_init(void);
extern void net_udp_init(void);
extern void net_tcp_init(void);

/*===========================================================================
 * Dynamic Buffer Pool — three size classes
 *
 * Each class has:
 *   - A contiguous storage array (BSS)
 *   - A descriptor array (one net_buffer_t per slot, data ptr into storage)
 *   - A free-list head protected by a critical section
 *   - A counting semaphore (count = available buffers)
 *     → semaphore_wait blocks the caller when the class is exhausted
 *     → semaphore_post wakes blocked allocators on free
 *
 * ref_count lifecycle:
 *   alloc → ref_count = 1
 *   ref   → ref_count++       (zero-copy share)
 *   free  → ref_count--;  if ref_count == 0: return to free list + sem_post
 *===========================================================================*/

typedef enum {
    CLS_SMALL  = 0,
    CLS_MEDIUM = 1,
    CLS_LARGE  = 2,
    CLS_COUNT  = 3
} buf_class_t;

/* Per-class runtime state */
typedef struct {
    net_buffer_t  *free_head;       /* free-list head (critical-section protected) */
    semaphore_t    sem;             /* counts available buffers; blocks on alloc    */
    uint16_t       buf_size;        /* payload capacity of each buffer in this class*/
    uint8_t        total;           /* total buffers in this class                  */
    uint8_t        in_use;          /* currently allocated                          */
    uint8_t        peak;            /* high-water mark                              */
    uint32_t       alloc_count;     /* cumulative successful allocs                 */
    uint32_t       free_count;      /* cumulative frees (ref_count → 0)             */
    uint32_t       wait_count;      /* allocs that had to wait on semaphore         */
} buf_pool_t;

static buf_pool_t pool[CLS_COUNT];

/* Backing storage — 8-byte aligned, in BSS */
static uint8_t small_storage [NET_BUF_SMALL_COUNT ][NET_BUF_SMALL ] __attribute__((aligned(8)));
static uint8_t medium_storage[NET_BUF_MEDIUM_COUNT][NET_BUF_MEDIUM] __attribute__((aligned(8)));
static uint8_t large_storage [NET_BUF_LARGE_COUNT ][NET_BUF_LARGE ] __attribute__((aligned(8)));

/* Buffer descriptors — separate from storage so next/ref_count don't eat payload */
static net_buffer_t small_descs [NET_BUF_SMALL_COUNT ];
static net_buffer_t medium_descs[NET_BUF_MEDIUM_COUNT];
static net_buffer_t large_descs [NET_BUF_LARGE_COUNT ];

/* Combined alloc-failure counter (timeout or size too large for any class) */
static uint32_t buf_alloc_failures;

/*---------------------------------------------------------------------------
 * Helpers
 *---------------------------------------------------------------------------*/

/* Return which class owns buf based on its capacity field. */
static buf_class_t buf_class_of(const net_buffer_t *buf) {
    if (buf->capacity <= NET_BUF_SMALL)  return CLS_SMALL;
    if (buf->capacity <= NET_BUF_MEDIUM) return CLS_MEDIUM;
    return CLS_LARGE;
}

/* Wire descriptors to storage and build the free list for one class. */
static void pool_class_init(buf_class_t cls,
                            net_buffer_t *descs, uint8_t *storage_base,
                            uint8_t count, uint16_t buf_size) {
    pool[cls].buf_size   = buf_size;
    pool[cls].total      = count;
    pool[cls].in_use     = 0;
    pool[cls].peak       = 0;
    pool[cls].alloc_count = 0;
    pool[cls].free_count  = 0;
    pool[cls].wait_count  = 0;
    pool[cls].free_head  = NULL;
    os_semaphore_init(&pool[cls].sem, (int32_t)count);

    for (uint8_t i = 0; i < count; i++) {
        descs[i].data      = storage_base + (size_t)i * buf_size;
        descs[i].capacity  = buf_size;
        descs[i].length    = 0;
        descs[i].offset    = 0;
        descs[i].ref_count = 0;
        descs[i].next      = pool[cls].free_head;
        pool[cls].free_head = &descs[i];
    }
}

/*===========================================================================
 * Network Buffer Management — public API
 *===========================================================================*/

/**
 * @brief Allocate a buffer of at least min_size bytes.
 *
 * Picks the tightest-fitting class and blocks for up to timeout_ms ticks
 * if the class is currently exhausted.
 */
net_buffer_t *net_buffer_alloc_size(uint16_t min_size, uint32_t timeout_ms) {
    /* Select tightest-fitting class */
    buf_class_t cls;
    if      (min_size <= NET_BUF_SMALL)  cls = CLS_SMALL;
    else if (min_size <= NET_BUF_MEDIUM) cls = CLS_MEDIUM;
    else if (min_size <= NET_BUF_LARGE)  cls = CLS_LARGE;
    else {
        uint32_t cs = os_enter_critical();
        buf_alloc_failures++;
        os_exit_critical(cs);
        LOG_W(TAG, "buf alloc: size %u exceeds max (%u)", min_size, NET_BUF_LARGE);
        return NULL;
    }

    /* Block until a buffer of this class is available (or timeout). */
    bool immediate = (pool[cls].in_use < pool[cls].total);
    if (os_semaphore_wait(&pool[cls].sem, timeout_ms) != OS_OK) {
        uint32_t cs = os_enter_critical();
        buf_alloc_failures++;
        os_exit_critical(cs);
        LOG_W(TAG, "buf alloc timeout: class=%d in_use=%u/%u",
              cls, pool[cls].in_use, pool[cls].total);
        return NULL;
    }

    /* Grab from free list (guaranteed non-empty after semaphore acquired). */
    uint32_t cs = os_enter_critical();

    if (!immediate) pool[cls].wait_count++;

    net_buffer_t *buf   = pool[cls].free_head;
    pool[cls].free_head = buf->next;
    buf->next           = NULL;
    buf->length         = 0;
    buf->offset         = 0;
    buf->ref_count      = 1;

    pool[cls].in_use++;
    if (pool[cls].in_use > pool[cls].peak) pool[cls].peak = pool[cls].in_use;
    pool[cls].alloc_count++;

    os_exit_critical(cs);
    return buf;
}

/** Backwards-compatible: allocate a large (MTU) buffer, block indefinitely. */
net_buffer_t *net_buffer_alloc(void) {
    return net_buffer_alloc_size(NET_BUF_LARGE, OS_WAIT_FOREVER);
}

/** Add a reference (zero-copy sharing).  Returns buf for convenience. */
net_buffer_t *net_buffer_ref(net_buffer_t *buf) {
    if (buf == NULL) return NULL;
    uint32_t cs = os_enter_critical();
    buf->ref_count++;
    os_exit_critical(cs);
    return buf;
}

/** Release a reference; returns buffer to pool when ref_count hits zero. */
void net_buffer_free(net_buffer_t *buf) {
    if (buf == NULL) return;

    uint32_t cs = os_enter_critical();

    if (buf->ref_count == 0) {          /* guard against double-free */
        os_exit_critical(cs);
        return;
    }
    buf->ref_count--;
    if (buf->ref_count > 0) {           /* still referenced by another holder */
        os_exit_critical(cs);
        return;
    }

    /* ref_count reached 0 — return to free list */
    buf_class_t cls     = buf_class_of(buf);
    buf->next           = pool[cls].free_head;
    pool[cls].free_head = buf;
    pool[cls].in_use--;
    pool[cls].free_count++;

    os_exit_critical(cs);

    /* Wake any task blocked in net_buffer_alloc_size() for this class. */
    os_semaphore_post(&pool[cls].sem);
}

/** Copy a statistics snapshot. */
void net_buffer_get_stats(net_buf_stats_t *stats) {
    if (stats == NULL) return;
    uint32_t cs = os_enter_critical();
    for (int c = 0; c < CLS_COUNT; c++) {
        stats->alloc_count[c] = pool[c].alloc_count;
        stats->free_count[c]  = pool[c].free_count;
        stats->wait_count[c]  = pool[c].wait_count;
        stats->in_use[c]      = pool[c].in_use;
        stats->peak[c]        = pool[c].peak;
    }
    stats->timeout_count = buf_alloc_failures;
    os_exit_critical(cs);
}

/*---------------------------------------------------------------------------
 * Pool initialisation (called from net_init)
 *---------------------------------------------------------------------------*/

static void net_buffer_pool_init(void) {
    buf_alloc_failures = 0;
    pool_class_init(CLS_SMALL,  small_descs,  &small_storage [0][0],
                    NET_BUF_SMALL_COUNT,  NET_BUF_SMALL);
    pool_class_init(CLS_MEDIUM, medium_descs, &medium_storage[0][0],
                    NET_BUF_MEDIUM_COUNT, NET_BUF_MEDIUM);
    pool_class_init(CLS_LARGE,  large_descs,  &large_storage [0][0],
                    NET_BUF_LARGE_COUNT,  NET_BUF_LARGE);
}

/*===========================================================================
 * Network Stack Initialization
 *===========================================================================*/

os_error_t net_init(net_driver_t *driver, const net_config_t *config) {
    if (driver == NULL || config == NULL) {
        return OS_ERR_INVALID_PARAM;
    }

    /* Initialize buffer pool */
    net_buffer_pool_init();

    /* Store driver and configuration */
    current_driver = driver;
    memcpy(&current_config, config, sizeof(net_config_t));

    /* Clear statistics */
    memset(&net_statistics, 0, sizeof(net_stats_t));

    /* Initialize network driver */
    if (driver->init) {
        os_error_t err = driver->init();
        if (err != OS_OK) {
            LOG_E(TAG, "driver init failed: err=%d", err);
            return err;
        }
    }

    /* Initialize protocol layers */
    net_ethernet_init();
    net_ip_init();
    net_icmp_init();
    net_udp_init();
    net_tcp_init();

    LOG_I(TAG, "initialized: ip=%u.%u.%u.%u mask=%u.%u.%u.%u gw=%u.%u.%u.%u",
          IPV4_ADDR(config->ip), IPV4_ADDR(config->netmask), IPV4_ADDR(config->gateway));

    return OS_OK;
}

/*===========================================================================
 * Network Main Loop Task
 *===========================================================================*/

static void network_task_func(void *param) {
    (void)param;

    uint8_t rx_buffer[NET_BUFFER_SIZE];

    while (1) {
        /* Check for incoming packets */
        if (current_driver && current_driver->receive) {
            int32_t length = current_driver->receive(rx_buffer, NET_BUFFER_SIZE);

            if (length > 0) {
                /* Pass to Ethernet layer */
                net_ethernet_input(rx_buffer, (uint16_t)length);
                net_statistics.eth_rx_packets++;
            }
        }

        /* Small delay to prevent busy-waiting */
        os_task_delay(1);  /* 1ms polling interval */
    }
}

os_error_t net_start(void) {
    os_error_t err = os_task_create(
        &network_task,
        "net_task",
        network_task_func,
        NULL,
        PRIORITY_HIGH
    );
    if (err != OS_OK) {
        LOG_E(TAG, "failed to create network task: err=%d", err);
    } else {
        LOG_I(TAG, "network task started");
    }
    return err;
}

/*===========================================================================
 * Network Statistics
 *===========================================================================*/

void net_get_stats(net_stats_t *stats) {
    if (stats) {
        memcpy(stats, &net_statistics, sizeof(net_stats_t));
    }
}

void net_get_config(net_config_t *config) {
    if (config) {
        memcpy(config, &current_config, sizeof(net_config_t));
    }
}

os_error_t net_set_config(const net_config_t *config) {
    if (config == NULL) {
        return OS_ERR_INVALID_PARAM;
    }

    memcpy(&current_config, config, sizeof(net_config_t));
    return OS_OK;
}

/*===========================================================================
 * Utility Functions
 *===========================================================================*/

bool net_parse_ipv4(const char *str, ipv4_addr_t *ip) {
    if (str == NULL || ip == NULL) {
        return false;
    }

    int count = 0;
    const char *p = str;

    /* Simple parser for IPv4 address */
    for (int i = 0; i < 4; i++) {
        int value = 0;
        int digits = 0;

        while (*p >= '0' && *p <= '9' && digits < 3) {
            value = value * 10 + (*p - '0');
            p++;
            digits++;
        }

        if (value > 255 || digits == 0) {
            return false;
        }

        ip->addr[i] = (uint8_t)value;
        count++;

        if (i < 3) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }

    return (count == 4) && (*p == '\0');
}

void net_format_ipv4(ipv4_addr_t ip, char *str) {
    if (str == NULL) {
        return;
    }

    /* Simple sprintf alternative */
    int pos = 0;
    for (int i = 0; i < 4; i++) {
        int value = ip.addr[i];
        int temp_pos = pos;

        /* Convert digit to string */
        if (value >= 100) {
            str[pos++] = '0' + (value / 100);
            value %= 100;
        }
        if (value >= 10 || pos > temp_pos) {
            str[pos++] = '0' + (value / 10);
            value %= 10;
        }
        str[pos++] = '0' + value;

        if (i < 3) {
            str[pos++] = '.';
        }
    }
    str[pos] = '\0';
}

bool net_ipv4_equal(ipv4_addr_t a, ipv4_addr_t b) {
    return memcmp(a.addr, b.addr, 4) == 0;
}

uint16_t net_checksum(const void *data, uint16_t length) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    /* Sum all 16-bit words */
    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }

    /* Add remaining byte if odd length */
    if (length > 0) {
        sum += *(const uint8_t *)ptr;
    }

    /* Fold 32-bit sum to 16 bits */
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return ~sum;
}

/*===========================================================================
 * Internal Functions for Other Modules
 *===========================================================================*/

/**
 * @brief Send raw ethernet frame
 * @param data Frame data
 * @param length Frame length
 * @return OS_OK on success
 */
os_error_t net_driver_send(const uint8_t *data, uint16_t length) {
    if (current_driver && current_driver->send) {
        os_error_t err = current_driver->send(data, length);
        if (err == OS_OK) {
            net_statistics.eth_tx_packets++;
        } else {
            net_statistics.eth_tx_errors++;
            LOG_E(TAG, "driver send failed: len=%u err=%d", length, err);
        }
        return err;
    }
    LOG_E(TAG, "driver send: stack not initialized");
    return OS_ERR_NOT_INITIALIZED;
}

/**
 * @brief Get current MAC address
 * @param mac Output MAC address
 */
void net_get_mac_addr(mac_addr_t *mac) {
    if (mac && current_driver && current_driver->get_mac) {
        current_driver->get_mac(mac);
    }
}

/**
 * @brief Get current IP address
 * @param ip Output IP address
 */
void net_get_ip_addr(ipv4_addr_t *ip) {
    if (ip) {
        *ip = current_config.ip;
    }
}

/**
 * @brief Get gateway IP address
 * @param gateway Output gateway IP
 */
void net_get_gateway(ipv4_addr_t *gateway) {
    if (gateway) {
        *gateway = current_config.gateway;
    }
}

/**
 * @brief Get DNS server IP address
 * @param dns Output DNS IP
 */
void net_get_dns(ipv4_addr_t *dns) {
    if (dns) {
        *dns = current_config.dns;
    }
}

/**
 * @brief Update network statistics (internal)
 */
void net_stats_update(const char *counter, int delta) {
    /* This is a simplified implementation */
    /* In a real implementation, you'd have proper counters */
    (void)counter;
    (void)delta;
}
