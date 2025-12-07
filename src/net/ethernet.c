/**
 * @file ethernet.c
 * @brief Ethernet Layer (Layer 2) - Frame handling
 */

#include "tinyos/net.h"
#include <string.h>

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

    /* Initialize ARP cache */
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        arp_cache[i].valid = false;
    }
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
 * @brief Send ARP request
 */
static os_error_t arp_send_request(ipv4_addr_t target_ip) {
    uint8_t frame[ETH_HEADER_SIZE + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + ETH_HEADER_SIZE);

    mac_addr_t my_mac;
    ipv4_addr_t my_ip;

    net_get_mac_addr(&my_mac);
    net_get_ip_addr(&my_ip);

    /* Ethernet header (broadcast) */
    memset(eth->dest.addr, 0xFF, 6);  /* Broadcast MAC */
    eth->src = my_mac;
    eth->type = htons(ETH_TYPE_ARP);

    /* ARP packet */
    arp->hardware_type = htons(ARP_HARDWARE_ETHERNET);
    arp->protocol_type = htons(ARP_PROTOCOL_IP);
    arp->hardware_size = 6;
    arp->protocol_size = 4;
    arp->opcode = htons(ARP_OP_REQUEST);
    arp->sender_mac = my_mac;
    arp->sender_ip = my_ip;
    memset(arp->target_mac.addr, 0, 6);
    arp->target_ip = target_ip;

    return net_driver_send(frame, sizeof(frame));
}

/**
 * @brief Send ARP reply
 */
static os_error_t arp_send_reply(ipv4_addr_t target_ip, mac_addr_t target_mac) {
    uint8_t frame[ETH_HEADER_SIZE + sizeof(arp_packet_t)];
    eth_header_t *eth = (eth_header_t *)frame;
    arp_packet_t *arp = (arp_packet_t *)(frame + ETH_HEADER_SIZE);

    mac_addr_t my_mac;
    ipv4_addr_t my_ip;

    net_get_mac_addr(&my_mac);
    net_get_ip_addr(&my_ip);

    /* Ethernet header */
    eth->dest = target_mac;
    eth->src = my_mac;
    eth->type = htons(ETH_TYPE_ARP);

    /* ARP packet */
    arp->hardware_type = htons(ARP_HARDWARE_ETHERNET);
    arp->protocol_type = htons(ARP_PROTOCOL_IP);
    arp->hardware_size = 6;
    arp->protocol_size = 4;
    arp->opcode = htons(ARP_OP_REPLY);
    arp->sender_mac = my_mac;
    arp->sender_ip = my_ip;
    arp->target_mac = target_mac;
    arp->target_ip = target_ip;

    return net_driver_send(frame, sizeof(frame));
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
        arp_send_reply(arp->sender_ip, arp->sender_mac);
    }
    /* Handle ARP reply */
    else if (opcode == ARP_OP_REPLY && net_ipv4_equal(arp->target_ip, my_ip)) {
        /* Already added to cache above */
    }
}

/*===========================================================================
 * Ethernet Input
 *===========================================================================*/

void net_ethernet_input(const uint8_t *data, uint16_t length) {
    if (length < ETH_HEADER_SIZE) {
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
            /* Unknown EtherType, ignore */
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
        return OS_ERR_INVALID_PARAM;
    }

    mac_addr_t dest_mac;

    /* Lookup MAC address in ARP cache */
    if (!arp_cache_lookup(dest_ip, &dest_mac)) {
        /* MAC not in cache, send ARP request */
        arp_send_request(dest_ip);
        return OS_ERR_TIMEOUT;  /* Caller should retry */
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
        os_task_delay(10);  /* 10ms polling */

        if (arp_cache_lookup(ip, mac)) {
            return OS_OK;
        }
    }

    return OS_ERR_TIMEOUT;
}
