/**
 * @file socket.c
 * @brief Socket API - UDP and simplified TCP implementation
 */

#include "tinyos/net.h"
#include <string.h>

/* UDP Header */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dest_port;
    uint16_t length;
    uint16_t checksum;
} udp_header_t;

/* TCP Header (simplified) */
typedef struct __attribute__((packed)) {
    uint16_t src_port;
    uint16_t dest_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t data_offset_flags;
    uint8_t flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} tcp_header_t;

/* TCP Flags */
#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

/* Socket structure */
typedef struct {
    socket_type_t type;
    bool in_use;
    sockaddr_in_t local_addr;
    sockaddr_in_t remote_addr;
    tcp_state_t state;

    /* RX buffer */
    uint8_t rx_buffer[1024];
    uint16_t rx_length;
    semaphore_t rx_sem;

    /* TCP specific */
    uint32_t seq_num;
    uint32_t ack_num;
} socket_t;

static socket_t sockets[NET_MAX_SOCKETS];
static mutex_t socket_mutex;
static uint16_t next_ephemeral_port = 49152;

extern os_error_t net_ip_send(ipv4_addr_t dest_ip, uint8_t protocol, const uint8_t *data, uint16_t length);

static uint16_t htons(uint16_t h) { return ((h & 0xFF) << 8) | ((h & 0xFF00) >> 8); }
static uint16_t ntohs(uint16_t n) { return htons(n); }
static uint32_t htonl(uint32_t h) {
    return ((h & 0xFF) << 24) | ((h & 0xFF00) << 8) | ((h & 0xFF0000) >> 8) | ((h & 0xFF000000) >> 24);
}
static uint32_t ntohl(uint32_t n) { return htonl(n); }

/*===========================================================================
 * Initialization
 *===========================================================================*/

void net_udp_init(void) {
    os_mutex_init(&socket_mutex);
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        sockets[i].in_use = false;
        sockets[i].rx_length = 0;
        os_semaphore_init(&sockets[i].rx_sem, 0);
    }
}

void net_tcp_init(void) {
    /* Shared with UDP */
}

/*===========================================================================
 * Socket API Implementation
 *===========================================================================*/

net_socket_t net_socket(socket_type_t type) {
    os_mutex_lock(&socket_mutex, OS_WAIT_FOREVER);

    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (!sockets[i].in_use) {
            sockets[i].in_use = true;
            sockets[i].type = type;
            sockets[i].state = TCP_CLOSED;
            sockets[i].rx_length = 0;
            sockets[i].local_addr.port = 0;
            sockets[i].remote_addr.port = 0;
            sockets[i].seq_num = os_get_tick_count();
            sockets[i].ack_num = 0;
            os_mutex_unlock(&socket_mutex);
            return i;
        }
    }

    os_mutex_unlock(&socket_mutex);
    return INVALID_SOCKET;
}

os_error_t net_bind(net_socket_t sock, const sockaddr_in_t *addr) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].in_use) {
        return OS_ERR_INVALID_PARAM;
    }

    sockets[sock].local_addr = *addr;
    return OS_OK;
}

os_error_t net_close(net_socket_t sock) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].in_use) {
        return OS_ERR_INVALID_PARAM;
    }

    /* For TCP: send FIN and wait for close (simplified) */
    if (sockets[sock].type == SOCK_STREAM && sockets[sock].state == TCP_ESTABLISHED) {
        /* Send FIN packet - simplified, not fully implemented */
        sockets[sock].state = TCP_CLOSED;
    }

    sockets[sock].in_use = false;
    sockets[sock].rx_length = 0;
    return OS_OK;
}

/*===========================================================================
 * UDP Implementation
 *===========================================================================*/

void net_udp_input(const uint8_t *data, uint16_t length, ipv4_addr_t src_ip, ipv4_addr_t dest_ip) {
    (void)dest_ip;

    if (length < sizeof(udp_header_t)) {
        return;
    }

    const udp_header_t *udp = (const udp_header_t *)data;
    uint16_t dest_port = ntohs(udp->dest_port);
    uint16_t src_port = ntohs(udp->src_port);

    /* Find matching socket */
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].type == SOCK_DGRAM &&
            sockets[i].local_addr.port == dest_port) {

            /* Store data in receive buffer */
            uint16_t payload_len = ntohs(udp->length) - sizeof(udp_header_t);
            if (payload_len > sizeof(sockets[i].rx_buffer)) {
                payload_len = sizeof(sockets[i].rx_buffer);
            }

            memcpy(sockets[i].rx_buffer, data + sizeof(udp_header_t), payload_len);
            sockets[i].rx_length = payload_len;
            sockets[i].remote_addr.addr = src_ip;
            sockets[i].remote_addr.port = src_port;

            /* Signal data available */
            os_semaphore_post(&sockets[i].rx_sem);
            break;
        }
    }
}

int32_t net_sendto(net_socket_t sock, const void *data, uint16_t length, const sockaddr_in_t *addr) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].in_use) {
        return -1;
    }

    if (sockets[sock].type != SOCK_DGRAM) {
        return -1;
    }

    uint8_t packet[sizeof(udp_header_t) + length];
    udp_header_t *udp = (udp_header_t *)packet;

    /* Build UDP header */
    udp->src_port = htons(sockets[sock].local_addr.port);
    udp->dest_port = htons(addr->port);
    udp->length = htons(sizeof(udp_header_t) + length);
    udp->checksum = 0;  /* Optional for IPv4 */

    /* Copy payload */
    memcpy(packet + sizeof(udp_header_t), data, length);

    /* Send via IP */
    if (net_ip_send(addr->addr, 17, packet, sizeof(packet)) == OS_OK) {
        return length;
    }

    return -1;
}

int32_t net_recvfrom(net_socket_t sock, void *buffer, uint16_t max_length, sockaddr_in_t *addr) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].in_use) {
        return -1;
    }

    if (sockets[sock].type != SOCK_DGRAM) {
        return -1;
    }

    /* Wait for data */
    if (os_semaphore_wait(&sockets[sock].rx_sem, 5000) != OS_OK) {
        return 0;  /* Timeout */
    }

    /* Copy data */
    uint16_t copy_len = sockets[sock].rx_length;
    if (copy_len > max_length) {
        copy_len = max_length;
    }

    memcpy(buffer, sockets[sock].rx_buffer, copy_len);

    if (addr) {
        *addr = sockets[sock].remote_addr;
    }

    sockets[sock].rx_length = 0;

    return copy_len;
}

/*===========================================================================
 * TCP Implementation (Simplified)
 *===========================================================================*/

void net_tcp_input(const uint8_t *data, uint16_t length, ipv4_addr_t src_ip, ipv4_addr_t dest_ip) {
    (void)dest_ip;

    if (length < sizeof(tcp_header_t)) {
        return;
    }

    const tcp_header_t *tcp = (const tcp_header_t *)data;
    uint16_t dest_port = ntohs(tcp->dest_port);
    uint16_t src_port = ntohs(tcp->src_port);

    /* Find matching socket */
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (sockets[i].in_use && sockets[i].type == SOCK_STREAM &&
            sockets[i].local_addr.port == dest_port &&
            net_ipv4_equal(sockets[i].remote_addr.addr, src_ip) &&
            sockets[i].remote_addr.port == src_port) {

            /* Process based on flags and state */
            uint8_t flags = tcp->flags;

            if (flags & TCP_FLAG_ACK && sockets[i].state == TCP_SYN_SENT) {
                /* Connection established */
                sockets[i].state = TCP_ESTABLISHED;
                sockets[i].ack_num = ntohl(tcp->seq_num) + 1;
                os_semaphore_post(&sockets[i].rx_sem);
            }
            else if (flags & TCP_FLAG_PSH && sockets[i].state == TCP_ESTABLISHED) {
                /* Data received */
                uint8_t data_offset = (tcp->data_offset_flags >> 4) * 4;
                uint16_t payload_len = length - data_offset;

                if (payload_len > 0 && payload_len <= sizeof(sockets[i].rx_buffer)) {
                    memcpy(sockets[i].rx_buffer, data + data_offset, payload_len);
                    sockets[i].rx_length = payload_len;
                    os_semaphore_post(&sockets[i].rx_sem);
                }
            }

            break;
        }
    }
}

os_error_t net_connect(net_socket_t sock, const sockaddr_in_t *addr, uint32_t timeout_ms) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].in_use) {
        return OS_ERR_INVALID_PARAM;
    }

    if (sockets[sock].type != SOCK_STREAM) {
        return OS_ERR_INVALID_PARAM;
    }

    /* Assign ephemeral port if not bound */
    if (sockets[sock].local_addr.port == 0) {
        sockets[sock].local_addr.port = next_ephemeral_port++;
    }

    sockets[sock].remote_addr = *addr;

    /* Build SYN packet */
    uint8_t packet[sizeof(tcp_header_t)];
    tcp_header_t *tcp = (tcp_header_t *)packet;

    tcp->src_port = htons(sockets[sock].local_addr.port);
    tcp->dest_port = htons(addr->port);
    tcp->seq_num = htonl(sockets[sock].seq_num);
    tcp->ack_num = 0;
    tcp->data_offset_flags = 0x50;  /* 5 * 4 = 20 bytes header */
    tcp->flags = TCP_FLAG_SYN;
    tcp->window = htons(1024);
    tcp->checksum = 0;
    tcp->urgent_ptr = 0;

    sockets[sock].state = TCP_SYN_SENT;

    /* Send SYN */
    if (net_ip_send(addr->addr, 6, packet, sizeof(packet)) != OS_OK) {
        return OS_ERR_GENERIC;
    }

    /* Wait for SYN-ACK */
    if (os_semaphore_wait(&sockets[sock].rx_sem, timeout_ms) != OS_OK) {
        sockets[sock].state = TCP_CLOSED;
        return OS_ERR_TIMEOUT;
    }

    return (sockets[sock].state == TCP_ESTABLISHED) ? OS_OK : OS_ERR_GENERIC;
}

int32_t net_send(net_socket_t sock, const void *data, uint16_t length, uint32_t timeout_ms) {
    (void)timeout_ms;

    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].in_use) {
        return -1;
    }

    if (sockets[sock].type == SOCK_STREAM) {
        if (sockets[sock].state != TCP_ESTABLISHED) {
            return -1;
        }

        uint8_t packet[sizeof(tcp_header_t) + length];
        tcp_header_t *tcp = (tcp_header_t *)packet;

        tcp->src_port = htons(sockets[sock].local_addr.port);
        tcp->dest_port = htons(sockets[sock].remote_addr.port);
        tcp->seq_num = htonl(sockets[sock].seq_num);
        tcp->ack_num = htonl(sockets[sock].ack_num);
        tcp->data_offset_flags = 0x50;
        tcp->flags = TCP_FLAG_PSH | TCP_FLAG_ACK;
        tcp->window = htons(1024);
        tcp->checksum = 0;
        tcp->urgent_ptr = 0;

        memcpy(packet + sizeof(tcp_header_t), data, length);

        if (net_ip_send(sockets[sock].remote_addr.addr, 6, packet, sizeof(packet)) == OS_OK) {
            sockets[sock].seq_num += length;
            return length;
        }
    }

    return -1;
}

int32_t net_recv(net_socket_t sock, void *buffer, uint16_t max_length, uint32_t timeout_ms) {
    if (sock < 0 || sock >= NET_MAX_SOCKETS || !sockets[sock].in_use) {
        return -1;
    }

    if (sockets[sock].type == SOCK_STREAM) {
        if (sockets[sock].state != TCP_ESTABLISHED) {
            return -1;
        }

        /* Wait for data */
        if (os_semaphore_wait(&sockets[sock].rx_sem, timeout_ms) != OS_OK) {
            return 0;
        }

        uint16_t copy_len = sockets[sock].rx_length;
        if (copy_len > max_length) {
            copy_len = max_length;
        }

        memcpy(buffer, sockets[sock].rx_buffer, copy_len);
        sockets[sock].rx_length = 0;

        return copy_len;
    }

    return -1;
}

/* Stub implementations */
os_error_t net_listen(net_socket_t sock, int backlog) {
    (void)sock; (void)backlog;
    return OS_ERR_NOT_IMPLEMENTED;
}

net_socket_t net_accept(net_socket_t sock, sockaddr_in_t *addr) {
    (void)sock; (void)addr;
    return INVALID_SOCKET;
}
