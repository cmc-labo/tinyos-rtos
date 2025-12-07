/**
 * @file net.h
 * @brief TinyOS Network Stack - Lightweight TCP/IP Implementation
 *
 * Ultra-lightweight network stack for embedded systems
 * Features: Ethernet, IPv4, ICMP, UDP, TCP, HTTP, DNS
 */

#ifndef TINYOS_NET_H
#define TINYOS_NET_H

#include "../tinyos.h"
#include <stdint.h>
#include <stdbool.h>

/*===========================================================================
 * Network Configuration
 *===========================================================================*/

#define NET_MAX_SOCKETS         8       /* Maximum number of sockets */
#define NET_BUFFER_SIZE         1500    /* MTU size */
#define NET_MAX_BUFFERS         8       /* Number of network buffers */
#define NET_TCP_MAX_CONNECTIONS 4       /* Maximum TCP connections */
#define NET_UDP_MAX_SOCKETS     4       /* Maximum UDP sockets */

/*===========================================================================
 * MAC Address (6 bytes)
 *===========================================================================*/

typedef struct {
    uint8_t addr[6];
} mac_addr_t;

/*===========================================================================
 * IPv4 Address (4 bytes)
 *===========================================================================*/

typedef struct {
    uint8_t addr[4];
} ipv4_addr_t;

/* Helper macros for IPv4 addresses */
#define IPV4(a,b,c,d) ((ipv4_addr_t){{a,b,c,d}})
#define IPV4_ADDR(ip) ((ip).addr[0]), ((ip).addr[1]), ((ip).addr[2]), ((ip).addr[3])

/*===========================================================================
 * Network Buffer Management
 *===========================================================================*/

typedef struct net_buffer {
    uint8_t data[NET_BUFFER_SIZE];
    uint16_t length;
    uint16_t offset;
    struct net_buffer *next;
    bool in_use;
} net_buffer_t;

/*===========================================================================
 * Network Interface Configuration
 *===========================================================================*/

typedef struct {
    mac_addr_t mac;
    ipv4_addr_t ip;
    ipv4_addr_t netmask;
    ipv4_addr_t gateway;
    ipv4_addr_t dns;
} net_config_t;

/*===========================================================================
 * Network Driver Interface
 *===========================================================================*/

typedef struct net_driver {
    /* Initialize hardware */
    os_error_t (*init)(void);

    /* Send packet */
    os_error_t (*send)(const uint8_t *data, uint16_t length);

    /* Receive packet (non-blocking) */
    int32_t (*receive)(uint8_t *buffer, uint16_t max_length);

    /* Get MAC address */
    void (*get_mac)(mac_addr_t *mac);

    /* Link status */
    bool (*is_link_up)(void);
} net_driver_t;

/*===========================================================================
 * Socket Types
 *===========================================================================*/

typedef enum {
    SOCK_STREAM = 1,    /* TCP */
    SOCK_DGRAM = 2      /* UDP */
} socket_type_t;

typedef int net_socket_t;
#define INVALID_SOCKET (-1)

/*===========================================================================
 * Socket Address Structure
 *===========================================================================*/

typedef struct {
    ipv4_addr_t addr;
    uint16_t port;
} sockaddr_in_t;

/*===========================================================================
 * TCP States
 *===========================================================================*/

typedef enum {
    TCP_CLOSED = 0,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT
} tcp_state_t;

/*===========================================================================
 * Network Statistics
 *===========================================================================*/

typedef struct {
    /* Ethernet stats */
    uint32_t eth_rx_packets;
    uint32_t eth_tx_packets;
    uint32_t eth_rx_errors;
    uint32_t eth_tx_errors;

    /* IP stats */
    uint32_t ip_rx_packets;
    uint32_t ip_tx_packets;
    uint32_t ip_rx_errors;

    /* ICMP stats */
    uint32_t icmp_rx_packets;
    uint32_t icmp_tx_packets;

    /* UDP stats */
    uint32_t udp_rx_packets;
    uint32_t udp_tx_packets;

    /* TCP stats */
    uint32_t tcp_rx_packets;
    uint32_t tcp_tx_packets;
    uint32_t tcp_connections;
    uint32_t tcp_resets;
} net_stats_t;

/*===========================================================================
 * Network Stack Initialization
 *===========================================================================*/

/**
 * @brief Initialize network stack
 * @param driver Network driver interface
 * @param config Network configuration
 * @return OS_OK on success
 */
os_error_t net_init(net_driver_t *driver, const net_config_t *config);

/**
 * @brief Start network stack (creates network task)
 * @return OS_OK on success
 */
os_error_t net_start(void);

/**
 * @brief Get network statistics
 * @param stats Pointer to statistics structure
 */
void net_get_stats(net_stats_t *stats);

/**
 * @brief Get current IP configuration
 * @param config Pointer to configuration structure
 */
void net_get_config(net_config_t *config);

/**
 * @brief Set IP configuration (DHCP not implemented yet)
 * @param config New configuration
 * @return OS_OK on success
 */
os_error_t net_set_config(const net_config_t *config);

/*===========================================================================
 * Socket API (BSD-like interface)
 *===========================================================================*/

/**
 * @brief Create a socket
 * @param type Socket type (SOCK_STREAM or SOCK_DGRAM)
 * @return Socket descriptor or INVALID_SOCKET on error
 */
net_socket_t net_socket(socket_type_t type);

/**
 * @brief Bind socket to local address
 * @param sock Socket descriptor
 * @param addr Local address and port
 * @return OS_OK on success
 */
os_error_t net_bind(net_socket_t sock, const sockaddr_in_t *addr);

/**
 * @brief Listen for incoming connections (TCP only)
 * @param sock Socket descriptor
 * @param backlog Maximum pending connections
 * @return OS_OK on success
 */
os_error_t net_listen(net_socket_t sock, int backlog);

/**
 * @brief Accept incoming connection (TCP only)
 * @param sock Listening socket
 * @param addr Remote address (output)
 * @return New socket descriptor or INVALID_SOCKET on error
 */
net_socket_t net_accept(net_socket_t sock, sockaddr_in_t *addr);

/**
 * @brief Connect to remote host (TCP only)
 * @param sock Socket descriptor
 * @param addr Remote address and port
 * @param timeout_ms Timeout in milliseconds (0 = blocking)
 * @return OS_OK on success
 */
os_error_t net_connect(net_socket_t sock, const sockaddr_in_t *addr, uint32_t timeout_ms);

/**
 * @brief Send data
 * @param sock Socket descriptor
 * @param data Data buffer
 * @param length Data length
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes sent or negative on error
 */
int32_t net_send(net_socket_t sock, const void *data, uint16_t length, uint32_t timeout_ms);

/**
 * @brief Receive data
 * @param sock Socket descriptor
 * @param buffer Receive buffer
 * @param max_length Buffer size
 * @param timeout_ms Timeout in milliseconds
 * @return Number of bytes received or negative on error
 */
int32_t net_recv(net_socket_t sock, void *buffer, uint16_t max_length, uint32_t timeout_ms);

/**
 * @brief Send datagram (UDP only)
 * @param sock Socket descriptor
 * @param data Data buffer
 * @param length Data length
 * @param addr Destination address
 * @return Number of bytes sent or negative on error
 */
int32_t net_sendto(net_socket_t sock, const void *data, uint16_t length, const sockaddr_in_t *addr);

/**
 * @brief Receive datagram (UDP only)
 * @param sock Socket descriptor
 * @param buffer Receive buffer
 * @param max_length Buffer size
 * @param addr Source address (output)
 * @return Number of bytes received or negative on error
 */
int32_t net_recvfrom(net_socket_t sock, void *buffer, uint16_t max_length, sockaddr_in_t *addr);

/**
 * @brief Close socket
 * @param sock Socket descriptor
 * @return OS_OK on success
 */
os_error_t net_close(net_socket_t sock);

/*===========================================================================
 * ICMP (Ping)
 *===========================================================================*/

/**
 * @brief Send ICMP ping request
 * @param dest_ip Destination IP address
 * @param timeout_ms Timeout in milliseconds
 * @param rtt Round-trip time in milliseconds (output)
 * @return OS_OK on success
 */
os_error_t net_ping(ipv4_addr_t dest_ip, uint32_t timeout_ms, uint32_t *rtt);

/*===========================================================================
 * DNS Client
 *===========================================================================*/

/**
 * @brief Resolve hostname to IP address
 * @param hostname Domain name (e.g., "example.com")
 * @param ip Resolved IP address (output)
 * @param timeout_ms Timeout in milliseconds
 * @return OS_OK on success
 */
os_error_t net_dns_resolve(const char *hostname, ipv4_addr_t *ip, uint32_t timeout_ms);

/*===========================================================================
 * HTTP Client
 *===========================================================================*/

typedef enum {
    HTTP_GET = 0,
    HTTP_POST,
    HTTP_PUT,
    HTTP_DELETE
} http_method_t;

typedef struct {
    uint16_t status_code;           /* HTTP status code */
    char *body;                      /* Response body */
    uint32_t body_length;           /* Response body length */
    char content_type[64];          /* Content-Type header */
} http_response_t;

/**
 * @brief Send HTTP request
 * @param method HTTP method
 * @param url URL (e.g., "http://192.168.1.100/api/data")
 * @param headers Additional headers (NULL-terminated array, can be NULL)
 * @param body Request body (can be NULL)
 * @param body_length Body length
 * @param response Response structure (output)
 * @param timeout_ms Timeout in milliseconds
 * @return OS_OK on success
 */
os_error_t net_http_request(
    http_method_t method,
    const char *url,
    const char **headers,
    const void *body,
    uint32_t body_length,
    http_response_t *response,
    uint32_t timeout_ms
);

/**
 * @brief Free HTTP response resources
 * @param response Response structure
 */
void net_http_free_response(http_response_t *response);

/**
 * @brief Simplified HTTP GET request
 * @param url URL
 * @param response Response structure (output)
 * @param timeout_ms Timeout in milliseconds
 * @return OS_OK on success
 */
os_error_t net_http_get(const char *url, http_response_t *response, uint32_t timeout_ms);

/**
 * @brief Simplified HTTP POST request
 * @param url URL
 * @param content_type Content-Type (e.g., "application/json")
 * @param body Request body
 * @param body_length Body length
 * @param response Response structure (output)
 * @param timeout_ms Timeout in milliseconds
 * @return OS_OK on success
 */
os_error_t net_http_post(
    const char *url,
    const char *content_type,
    const void *body,
    uint32_t body_length,
    http_response_t *response,
    uint32_t timeout_ms
);

/*===========================================================================
 * HTTP Server (Simple)
 *===========================================================================*/

typedef struct http_request {
    http_method_t method;
    char path[128];
    char query[256];
    const char *body;
    uint32_t body_length;
    net_socket_t client_sock;
} http_request_t;

typedef os_error_t (*http_handler_t)(const http_request_t *request);

/**
 * @brief Start HTTP server
 * @param port Port number (e.g., 80)
 * @param handler Request handler callback
 * @return OS_OK on success
 */
os_error_t net_http_server_start(uint16_t port, http_handler_t handler);

/**
 * @brief Send HTTP response
 * @param request Request structure
 * @param status_code HTTP status code
 * @param content_type Content-Type header
 * @param body Response body
 * @param body_length Body length
 * @return OS_OK on success
 */
os_error_t net_http_send_response(
    const http_request_t *request,
    uint16_t status_code,
    const char *content_type,
    const void *body,
    uint32_t body_length
);

/*===========================================================================
 * Utility Functions
 *===========================================================================*/

/**
 * @brief Parse IP address from string
 * @param str IP address string (e.g., "192.168.1.100")
 * @param ip Parsed IP address (output)
 * @return true on success
 */
bool net_parse_ipv4(const char *str, ipv4_addr_t *ip);

/**
 * @brief Format IP address to string
 * @param ip IP address
 * @param str Output buffer (minimum 16 bytes)
 */
void net_format_ipv4(ipv4_addr_t ip, char *str);

/**
 * @brief Compare two IP addresses
 * @param a First IP address
 * @param b Second IP address
 * @return true if equal
 */
bool net_ipv4_equal(ipv4_addr_t a, ipv4_addr_t b);

/**
 * @brief Calculate checksum (Internet Checksum - RFC 1071)
 * @param data Data buffer
 * @param length Data length
 * @return Checksum value
 */
uint16_t net_checksum(const void *data, uint16_t length);

#endif /* TINYOS_NET_H */
