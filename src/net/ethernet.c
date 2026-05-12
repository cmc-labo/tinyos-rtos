/**
 * @file ethernet.c
 * @brief Ethernet Layer (Layer 2) - Frame handling
 */

#include "tinyos/net.h"
#include "tinyos/stdio_uart.h"
#include <string.h>

#define TAG_ETH "net/eth"
#define TAG_ARP "net/arp"

/*===========================================================================
 * Ethernet Frame Structure
 *===========================================================================*/

#define ETH_HEADER_SIZE 14
#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

typedef struct __attribute__((packed)) {
    mac_addr_t dest;
    mac_addr_t src;
    uint16_t type;
} eth_header_t;

/*===========================================================================
 * ARP (Address Resolution Protocol)
 *===========================================================================*/

#define ARP_HARDWARE_ETHERNET 1
#define ARP_PROTOCOL_IP       0x0800
#define ARP_OP_REQUEST        1
#define ARP_OP_REPLY          2

typedef struct __attribute__((packed)) {
    uint16_t hardware_type;
    uint16_t protocol_type;
    uint8_t hardware_size;
    uint8_t protocol_size;
    uint16_t opcode;
    mac_addr_t sender_mac;
    ipv4_addr_t sender_ip;
    mac_addr_t target_mac;
    ipv4_addr_t target_ip;
} arp_packet_t;

/* ARP cache */
#define ARP_CACHE_SIZE 8

typedef struct {
    ipv4_addr_t ip;
    mac_addr_t mac;
    uint32_t timestamp;
    bool valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];
static mutex_t arp_mutex;

/* External functions */
extern os_error_t net_driver_send(const uint8_t *data, uint16_t length);
extern void net_get_mac_addr(mac_addr_t *mac);
extern void net_get_ip_addr(ipv4_addr_t *ip);
extern void net_ip_input(const uint8_t *data, uint16_t length, const mac_addr_t *src_mac);

/* Convert between host and network byte order */
static uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort & 0xFF00) >> 8);
}

static uint16_t ntohs(uint16_t netshort) {
    return htons(netshort);  /* Same operation */
}

/*===========================================================================
 * Ethernet Layer Initialization
 *===========================================================================*/

void net_ethernet_init(void) {
    os_mutex_init(&arp_mutex);

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = false;
    }
}

/** Invalidate all ARP cache entries — called when the link goes down. */
void net_arp_cache_flush(void) {
    os_mutex_lock(&arp_mutex, OS_WAIT_FOREVER);
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = false;
    }
    os_mutex_unlock(&arp_mutex);
    LOG_I(TAG_ARP, "cache flushed");
}

/*===========================================================================
 * ARP Functions
 *===========================================================================*/

/**
 * @brief Add entry to ARP cache
 */
static void arp_cache_add(ipv4_addr_t ip, mac_addr_t mac) {
    os_mutex_lock(&arp_mutex, OS_WAIT_FOREVER);

    /* Find empty slot or oldest entry */
    int oldest_idx = 0;
    uint32_t oldest_time = arp_cache[0].timestamp;

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid) {
            oldest_idx = i;
            break;
        }

        if (arp_cache[i].timestamp < oldest_time) {
            oldest_time = arp_cache[i].timestamp;
            oldest_idx = i;
        }
    }

    /* Add entry */
    arp_cache[oldest_idx].ip = ip;
    arp_cache[oldest_idx].mac = mac;
    arp_cache[oldest_idx].timestamp = os_get_tick_count();
    arp_cache[oldest_idx].valid = true;

    os_mutex_unlock(&arp_mutex);

    LOG_D(TAG_ARP, "cache add: %u.%u.%u.%u -> %02x:%02x:%02x:%02x:%02x:%02x",
          IPV4_ADDR(ip),
          mac.addr[0], mac.addr[1], mac.addr[2],
          mac.addr[3], mac.addr[4], mac.addr[5]);
}

/**
 * @brief Lookup MAC address in ARP cache
 */
static bool arp_cache_lookup(ipv4_addr_t ip, mac_addr_t *mac) {
    bool found = false;

    os_mutex_lock(&arp_mutex, OS_WAIT_FOREVER);

    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && net_ipv4_equal(arp_cache[i].ip, ip)) {
            *mac = arp_cache[i].mac;
            found = true;
            break;
        }
    }

    os_mutex_unlock(&arp_mutex);

    return found;
}

/**
 * @brief Build and send an ARP frame (common path for request and reply)
 */
static os_error_t arp_send(mac_addr_t dest_mac, uint16_t opcode,
                            mac_addr_t target_mac, ipv4_addr_t target_ip) {
    uint8_t frame[ETH_HEADER_SIZE + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + ETH_HEADER_SIZE);

    mac_addr_t my_mac;
    ipv4_addr_t my_ip;
    net_get_mac_addr(&my_mac);
    net_get_ip_addr(&my_ip);

    eth->dest = dest_mac;
    eth->src  = my_mac;
    eth->type = htons(ETH_TYPE_ARP);

    arp->hardware_type = htons(ARP_HARDWARE_ETHERNET);
    arp->protocol_type = htons(ARP_PROTOCOL_IP);
    arp->hardware_size = 6;
    arp->protocol_size = 4;
    arp->opcode        = htons(opcode);
    arp->sender_mac    = my_mac;
    arp->sender_ip     = my_ip;
    arp->target_mac    = target_mac;
    arp->target_ip     = target_ip;

    return net_driver_send(frame, sizeof(frame));
}

/**
 * @brief Send ARP request
 */
static os_error_t arp_send_request(ipv4_addr_t target_ip) {
    mac_addr_t broadcast, zero_mac;
    memset(broadcast.addr, 0xFF, 6);
    memset(zero_mac.addr,  0x00, 6);
    return arp_send(broadcast, ARP_OP_REQUEST, zero_mac, target_ip);
}

/**
 * @brief Send ARP reply
 */
static os_error_t arp_send_reply(ipv4_addr_t target_ip, mac_addr_t target_mac) {
    return arp_send(target_mac, ARP_OP_REPLY, target_mac, target_ip);
}

/**
 * @brief Handle incoming ARP packet
 */
static void arp_input(const uint8_t *data, uint16_t length) {
    if (length < sizeof(arp_packet_t)) {
        return;
    }

    const arp_packet_t *arp = (const arp_packet_t *)data;
    ipv4_addr_t my_ip;
    net_get_ip_addr(&my_ip);

    uint16_t opcode = ntohs(arp->opcode);

    /* Add sender to ARP cache */
    arp_cache_add(arp->sender_ip, arp->sender_mac);

    /* Handle ARP request */
    if (opcode == ARP_OP_REQUEST && net_ipv4_equal(arp->target_ip, my_ip)) {
        LOG_D(TAG_ARP, "request: who has %u.%u.%u.%u (from %u.%u.%u.%u)",
              IPV4_ADDR(arp->target_ip), IPV4_ADDR(arp->sender_ip));
        arp_send_reply(arp->sender_ip, arp->sender_mac);
    }
    /* Handle ARP reply */
    else if (opcode == ARP_OP_REPLY && net_ipv4_equal(arp->target_ip, my_ip)) {
        LOG_D(TAG_ARP, "reply: %u.%u.%u.%u is at %02x:%02x:%02x:%02x:%02x:%02x",
              IPV4_ADDR(arp->sender_ip),
              arp->sender_mac.addr[0], arp->sender_mac.addr[1], arp->sender_mac.addr[2],
              arp->sender_mac.addr[3], arp->sender_mac.addr[4], arp->sender_mac.addr[5]);
    }
}

/*===========================================================================
 * Ethernet Input
 *===========================================================================*/

void net_ethernet_input(const uint8_t *data, uint16_t length) {
    if (length < ETH_HEADER_SIZE) {
        LOG_W(TAG_ETH, "frame too short: %u bytes (min %u)", length, ETH_HEADER_SIZE);
        return;
    }

    const eth_header_t *eth = (const eth_header_t *)data;
    uint16_t type = ntohs(eth->type);
    const uint8_t *payload = data + ETH_HEADER_SIZE;
    uint16_t payload_length = length - ETH_HEADER_SIZE;

    /* Check if frame is for us (unicast or broadcast) */
    mac_addr_t my_mac;
    net_get_mac_addr(&my_mac);

    bool is_for_us = (memcmp(eth->dest.addr, my_mac.addr, 6) == 0);
    bool is_broadcast = (eth->dest.addr[0] == 0xFF && eth->dest.addr[1] == 0xFF &&
                         eth->dest.addr[2] == 0xFF && eth->dest.addr[3] == 0xFF &&
                         eth->dest.addr[4] == 0xFF && eth->dest.addr[5] == 0xFF);

    if (!is_for_us && !is_broadcast) {
        return;  /* Not for us */
    }

    /* Process based on EtherType */
    switch (type) {
        case ETH_TYPE_IP:
            net_ip_input(payload, payload_length, &eth->src);
            break;

        case ETH_TYPE_ARP:
            arp_input(payload, payload_length);
            break;

        default:
            LOG_D(TAG_ETH, "unhandled EtherType 0x%04x", type);
            break;
    }
}

/*===========================================================================
 * Ethernet Output
 *===========================================================================*/

/**
 * @brief Send IP packet via Ethernet
 * @param dest_ip Destination IP address
 * @param data IP packet data
 * @param length Packet length
 * @return OS_OK on success
 */
os_error_t net_ethernet_send_ip(ipv4_addr_t dest_ip, const uint8_t *data, uint16_t length) {
    if (length > NET_BUFFER_SIZE - ETH_HEADER_SIZE) {
        LOG_E(TAG_ETH, "send: frame too large (%u > %u)", length,
              (unsigned)(NET_BUFFER_SIZE - ETH_HEADER_SIZE));
        return OS_ERR_INVALID_PARAM;
    }

    mac_addr_t dest_mac;

    /* Lookup MAC address in ARP cache */
    if (!arp_cache_lookup(dest_ip, &dest_mac)) {
        LOG_D(TAG_ARP, "no cache entry for %u.%u.%u.%u, sending request",
              IPV4_ADDR(dest_ip));
        arp_send_request(dest_ip);
        return OS_ERR_TIMEOUT;
    }

    /* Build ethernet frame */
    uint8_t frame[NET_BUFFER_SIZE];
    eth_header_t *eth = (eth_header_t *)frame;

    net_get_mac_addr(&eth->src);
    eth->dest = dest_mac;
    eth->type = htons(ETH_TYPE_IP);

    /* Copy IP packet */
    memcpy(frame + ETH_HEADER_SIZE, data, length);

    /* Send frame */
    return net_driver_send(frame, ETH_HEADER_SIZE + length);
}

/**
 * @brief Resolve IP to MAC address (with retry)
 * @param ip IP address
 * @param mac Output MAC address
 * @param timeout_ms Timeout in milliseconds
 * @return OS_OK on success
 */
os_error_t net_arp_resolve(ipv4_addr_t ip, mac_addr_t *mac, uint32_t timeout_ms) {
    if (mac == NULL) {
        return OS_ERR_INVALID_PARAM;
    }

    uint32_t start_time = os_get_tick_count();

    /* Try to find in cache */
    if (arp_cache_lookup(ip, mac)) {
        return OS_OK;
    }

    /* Send ARP request */
    arp_send_request(ip);

    /* Wait for ARP reply (with timeout) */
    while ((os_get_tick_count() - start_time) < timeout_ms) {
        os_task_delay(10);

        if (arp_cache_lookup(ip, mac)) {
            LOG_D(TAG_ARP, "resolved %u.%u.%u.%u in %lums",
                  IPV4_ADDR(ip),
                  (unsigned long)(os_get_tick_count() - start_time));
            return OS_OK;
        }
    }

    LOG_W(TAG_ARP, "resolve timeout for %u.%u.%u.%u", IPV4_ADDR(ip));
    return OS_ERR_TIMEOUT;
}
