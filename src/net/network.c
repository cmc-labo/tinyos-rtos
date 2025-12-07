/**
 * @file network.c
 * @brief Network Stack Core - Buffer Management and Main Loop
 */

#include "tinyos/net.h"
#include <string.h>

/*===========================================================================
 * Internal Data Structures
 *===========================================================================*/

/* Network buffer pool */
static net_buffer_t buffer_pool[NET_MAX_BUFFERS];
static mutex_t buffer_mutex;

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
 * Network Buffer Management
 *===========================================================================*/

/**
 * @brief Initialize network buffer pool
 */
static void net_buffer_pool_init(void) {
    os_mutex_init(&buffer_mutex);

    for (int i = 0; i < NET_MAX_BUFFERS; i++) {
        buffer_pool[i].in_use = false;
        buffer_pool[i].length = 0;
        buffer_pool[i].offset = 0;
        buffer_pool[i].next = NULL;
    }
}

/**
 * @brief Allocate network buffer
 * @return Pointer to buffer or NULL if none available
 */
net_buffer_t *net_buffer_alloc(void) {
    net_buffer_t *buf = NULL;

    os_mutex_lock(&buffer_mutex, OS_WAIT_FOREVER);

    for (int i = 0; i < NET_MAX_BUFFERS; i++) {
        if (!buffer_pool[i].in_use) {
            buf = &buffer_pool[i];
            buf->in_use = true;
            buf->length = 0;
            buf->offset = 0;
            buf->next = NULL;
            break;
        }
    }

    os_mutex_unlock(&buffer_mutex);

    return buf;
}

/**
 * @brief Free network buffer
 * @param buf Buffer to free
 */
void net_buffer_free(net_buffer_t *buf) {
    if (buf == NULL) {
        return;
    }

    os_mutex_lock(&buffer_mutex, OS_WAIT_FOREVER);

    buf->in_use = false;
    buf->length = 0;
    buf->offset = 0;
    buf->next = NULL;

    os_mutex_unlock(&buffer_mutex);
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
            return err;
        }
    }

    /* Initialize protocol layers */
    net_ethernet_init();
    net_ip_init();
    net_icmp_init();
    net_udp_init();
    net_tcp_init();

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
    /* Create network receive task */
    return os_task_create(
        &network_task,
        "net_task",
        network_task_func,
        NULL,
        PRIORITY_HIGH  /* High priority for network processing */
    );
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

    int a, b, c, d;
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
    return (a.addr[0] == b.addr[0] &&
            a.addr[1] == b.addr[1] &&
            a.addr[2] == b.addr[2] &&
            a.addr[3] == b.addr[3]);
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
        }
        return err;
    }
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
