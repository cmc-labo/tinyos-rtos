/**
 * @file ip.c
 * @brief IPv4 Layer (Layer 3) and ICMP
 */

#include "tinyos/net.h"
#include <string.h>

/*===========================================================================
 * IPv4 Header Structure
 *===========================================================================*/

#define IP_PROTOCOL_ICMP 1
#define IP_PROTOCOL_TCP  6
#define IP_PROTOCOL_UDP  17

typedef struct __attribute__((packed)) {
    uint8_t version_ihl;      /* Version (4) and Internet Header Length */
    uint8_t tos;              /* Type of Service */
    uint16_t total_length;    /* Total Length */
    uint16_t identification;  /* Identification */
    uint16_t flags_fragment;  /* Flags and Fragment Offset */
    uint8_t ttl;              /* Time to Live */
    uint8_t protocol;         /* Protocol */
    uint16_t checksum;        /* Header Checksum */
    ipv4_addr_t src;          /* Source Address */
    ipv4_addr_t dest;         /* Destination Address */
} ip_header_t;

/*===========================================================================
 * ICMP Header Structure
 *===========================================================================*/

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} icmp_header_t;

/* Ping tracking */
static uint16_t ping_id = 0;
static uint16_t ping_sequence = 0;
static bool ping_reply_received = false;
static uint32_t ping_reply_time = 0;
static mutex_t ping_mutex;
static semaphore_t ping_sem;

/* External functions */
extern os_error_t net_ethernet_send_ip(ipv4_addr_t dest_ip, const uint8_t *data, uint16_t length);
extern void net_get_ip_addr(ipv4_addr_t *ip);
extern void net_udp_input(const uint8_t *data, uint16_t length, ipv4_addr_t src_ip, ipv4_addr_t dest_ip);
extern void net_tcp_input(const uint8_t *data, uint16_t length, ipv4_addr_t src_ip, ipv4_addr_t dest_ip);

/* Byte order conversion */
static uint16_t htons(uint16_t hostshort) {
    return ((hostshort & 0xFF) << 8) | ((hostshort & 0xFF00) >> 8);
}

static uint16_t ntohs(uint16_t netshort) {
    return htons(netshort);
}

static uint32_t htonl(uint32_t hostlong) {
    return ((hostlong & 0x000000FF) << 24) |
           ((hostlong & 0x0000FF00) << 8)  |
           ((hostlong & 0x00FF0000) >> 8)  |
           ((hostlong & 0xFF000000) >> 24);
}

static uint32_t ntohl(uint32_t netlong) {
    return htonl(netlong);
}

/*===========================================================================
 * IP Layer Initialization
 *===========================================================================*/

void net_ip_init(void) {
    ping_id = 0;
    ping_sequence = 0;
    ping_reply_received = false;
}

/*===========================================================================
 * ICMP Functions
 *===========================================================================*/

void net_icmp_init(void) {
    os_mutex_init(&ping_mutex);
    os_semaphore_init(&ping_sem, 0);
}

/**
 * @brief Handle incoming ICMP packet
 */
static void icmp_input(const uint8_t *data, uint16_t length, ipv4_addr_t src_ip) {
    if (length < sizeof(icmp_header_t)) {
        return;
    }

    const icmp_header_t *icmp = (const icmp_header_t *)data;

    switch (icmp->type) {
        case ICMP_TYPE_ECHO_REQUEST: {
            /* Send echo reply */
            uint8_t reply[sizeof(ip_header_t) + length];
            ip_header_t *ip_hdr = (ip_header_t *)reply;
            icmp_header_t *icmp_reply = (icmp_header_t *)(reply + sizeof(ip_header_t));

            ipv4_addr_t my_ip;
            net_get_ip_addr(&my_ip);

            /* Build IP header */
            ip_hdr->version_ihl = 0x45;  /* Version 4, IHL 5 (20 bytes) */
            ip_hdr->tos = 0;
            ip_hdr->total_length = htons(sizeof(ip_header_t) + length);
            ip_hdr->identification = 0;
            ip_hdr->flags_fragment = 0;
            ip_hdr->ttl = 64;
            ip_hdr->protocol = IP_PROTOCOL_ICMP;
            ip_hdr->src = my_ip;
            ip_hdr->dest = src_ip;
            ip_hdr->checksum = 0;
            ip_hdr->checksum = net_checksum(ip_hdr, sizeof(ip_header_t));

            /* Build ICMP reply */
            memcpy(icmp_reply, icmp, length);
            icmp_reply->type = ICMP_TYPE_ECHO_REPLY;
            icmp_reply->checksum = 0;
            icmp_reply->checksum = net_checksum(icmp_reply, length);

            /* Send reply */
            net_ethernet_send_ip(src_ip, reply, sizeof(reply));
            break;
        }

        case ICMP_TYPE_ECHO_REPLY: {
            /* Check if this is our ping reply */
            if (ntohs(icmp->id) == ping_id && ntohs(icmp->sequence) == ping_sequence) {
                ping_reply_received = true;
                ping_reply_time = os_get_tick_count();
                os_semaphore_post(&ping_sem);
            }
            break;
        }

        default:
            /* Ignore other ICMP types */
            break;
    }
}

/**
 * @brief Send ping (ICMP Echo Request)
 */
os_error_t net_ping(ipv4_addr_t dest_ip, uint32_t timeout_ms, uint32_t *rtt) {
    uint8_t packet[sizeof(ip_header_t) + sizeof(icmp_header_t) + 32];  /* 32 bytes payload */
    ip_header_t *ip_hdr = (ip_header_t *)packet;
    icmp_header_t *icmp = (icmp_header_t *)(packet + sizeof(ip_header_t));
    uint8_t *payload = packet + sizeof(ip_header_t) + sizeof(icmp_header_t);

    ipv4_addr_t my_ip;
    net_get_ip_addr(&my_ip);

    os_mutex_lock(&ping_mutex, OS_WAIT_FOREVER);

    /* Prepare ping tracking */
    ping_id++;
    ping_sequence++;
    ping_reply_received = false;

    uint16_t current_id = ping_id;
    uint16_t current_seq = ping_sequence;

    /* Build IP header */
    ip_hdr->version_ihl = 0x45;
    ip_hdr->tos = 0;
    ip_hdr->total_length = htons(sizeof(packet));
    ip_hdr->identification = htons(current_id);
    ip_hdr->flags_fragment = 0;
    ip_hdr->ttl = 64;
    ip_hdr->protocol = IP_PROTOCOL_ICMP;
    ip_hdr->src = my_ip;
    ip_hdr->dest = dest_ip;
    ip_hdr->checksum = 0;
    ip_hdr->checksum = net_checksum(ip_hdr, sizeof(ip_header_t));

    /* Build ICMP header */
    icmp->type = ICMP_TYPE_ECHO_REQUEST;
    icmp->code = 0;
    icmp->id = htons(current_id);
    icmp->sequence = htons(current_seq);

    /* Fill payload with pattern */
    for (int i = 0; i < 32; i++) {
        payload[i] = i;
    }

    /* Calculate ICMP checksum (including payload) */
    icmp->checksum = 0;
    icmp->checksum = net_checksum(icmp, sizeof(icmp_header_t) + 32);

    /* Record start time */
    uint32_t start_time = os_get_tick_count();

    /* Send ping */
    os_error_t err = net_ethernet_send_ip(dest_ip, packet, sizeof(packet));
    if (err != OS_OK) {
        os_mutex_unlock(&ping_mutex);
        return err;
    }

    /* Wait for reply */
    err = os_semaphore_wait(&ping_sem, timeout_ms);

    if (err == OS_OK && ping_reply_received) {
        if (rtt) {
            *rtt = ping_reply_time - start_time;
        }
        os_mutex_unlock(&ping_mutex);
        return OS_OK;
    }

    os_mutex_unlock(&ping_mutex);
    return OS_ERR_TIMEOUT;
}

/*===========================================================================
 * IP Input
 *===========================================================================*/

void net_ip_input(const uint8_t *data, uint16_t length, const mac_addr_t *src_mac) {
    (void)src_mac;  /* Unused in this simple implementation */

    if (length < sizeof(ip_header_t)) {
        return;
    }

    const ip_header_t *ip_hdr = (const ip_header_t *)data;

    /* Verify IP version */
    if ((ip_hdr->version_ihl >> 4) != 4) {
        return;  /* Not IPv4 */
    }

    /* Calculate header length */
    uint8_t ihl = (ip_hdr->version_ihl & 0x0F) * 4;
    if (ihl < 20 || ihl > length) {
        return;  /* Invalid header length */
    }

    /* Verify checksum */
    uint16_t received_checksum = ip_hdr->checksum;
    ip_header_t *mutable_hdr = (ip_header_t *)data;
    mutable_hdr->checksum = 0;
    uint16_t calculated_checksum = net_checksum(data, ihl);
    mutable_hdr->checksum = received_checksum;

    if (received_checksum != calculated_checksum) {
        return;  /* Checksum mismatch */
    }

    /* Check if packet is for us */
    ipv4_addr_t my_ip;
    net_get_ip_addr(&my_ip);

    if (!net_ipv4_equal(ip_hdr->dest, my_ip)) {
        return;  /* Not for us */
    }

    /* Get payload */
    uint16_t total_length = ntohs(ip_hdr->total_length);
    if (total_length > length) {
        return;  /* Invalid length */
    }

    const uint8_t *payload = data + ihl;
    uint16_t payload_length = total_length - ihl;

    /* Process based on protocol */
    switch (ip_hdr->protocol) {
        case IP_PROTOCOL_ICMP:
            icmp_input(payload, payload_length, ip_hdr->src);
            break;

        case IP_PROTOCOL_UDP:
            net_udp_input(payload, payload_length, ip_hdr->src, ip_hdr->dest);
            break;

        case IP_PROTOCOL_TCP:
            net_tcp_input(payload, payload_length, ip_hdr->src, ip_hdr->dest);
            break;

        default:
            /* Unknown protocol */
            break;
    }
}

/*===========================================================================
 * IP Output
 *===========================================================================*/

/**
 * @brief Send IP packet
 * @param dest_ip Destination IP
 * @param protocol IP protocol number
 * @param data Payload data
 * @param length Payload length
 * @return OS_OK on success
 */
os_error_t net_ip_send(ipv4_addr_t dest_ip, uint8_t protocol, const uint8_t *data, uint16_t length) {
    if (length > NET_BUFFER_SIZE - sizeof(ip_header_t)) {
        return OS_ERR_INVALID_PARAM;
    }

    uint8_t packet[NET_BUFFER_SIZE];
    ip_header_t *ip_hdr = (ip_header_t *)packet;
    ipv4_addr_t my_ip;

    net_get_ip_addr(&my_ip);

    /* Build IP header */
    ip_hdr->version_ihl = 0x45;  /* Version 4, IHL 5 */
    ip_hdr->tos = 0;
    ip_hdr->total_length = htons(sizeof(ip_header_t) + length);
    ip_hdr->identification = 0;
    ip_hdr->flags_fragment = 0;
    ip_hdr->ttl = 64;
    ip_hdr->protocol = protocol;
    ip_hdr->src = my_ip;
    ip_hdr->dest = dest_ip;
    ip_hdr->checksum = 0;
    ip_hdr->checksum = net_checksum(ip_hdr, sizeof(ip_header_t));

    /* Copy payload */
    memcpy(packet + sizeof(ip_header_t), data, length);

    /* Send via ethernet */
    return net_ethernet_send_ip(dest_ip, packet, sizeof(ip_header_t) + length);
}
