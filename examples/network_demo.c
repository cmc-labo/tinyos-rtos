/**
 * @file network_demo.c
 * @brief Network Stack Demo - TCP/IP, UDP, HTTP, Ping
 *
 * Demonstrates:
 * - Network initialization
 * - Ping (ICMP)
 * - UDP socket communication
 * - TCP socket communication
 * - HTTP GET request
 */

#include "tinyos.h"
#include "tinyos/net.h"
#include <stdio.h>

/* External loopback driver */
extern net_driver_t *loopback_get_driver(void);

/*===========================================================================
 * Network Configuration
 *===========================================================================*/

static net_config_t network_config = {
    .mac = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}},
    .ip = {{192, 168, 1, 100}},
    .netmask = {{255, 255, 255, 0}},
    .gateway = {{192, 168, 1, 1}},
    .dns = {{8, 8, 8, 8}}
};

/*===========================================================================
 * Demo Tasks
 *===========================================================================*/

/**
 * @brief Ping demo task
 */
void ping_demo_task(void *param) {
    (void)param;

    ipv4_addr_t target = IPV4(192, 168, 1, 1);
    uint32_t rtt;

    printf("[Ping] Starting ping demo...\n");

    while (1) {
        os_error_t err = net_ping(target, 2000, &rtt);

        if (err == OS_OK) {
            printf("[Ping] Reply from %d.%d.%d.%d: time=%lu ms\n",
                   IPV4_ADDR(target), rtt);
        } else {
            printf("[Ping] Request timeout\n");
        }

        os_task_delay(3000);  /* Ping every 3 seconds */
    }
}

/**
 * @brief UDP demo task
 */
void udp_demo_task(void *param) {
    (void)param;

    printf("[UDP] Starting UDP demo...\n");

    /* Create UDP socket */
    net_socket_t sock = net_socket(SOCK_DGRAM);
    if (sock == INVALID_SOCKET) {
        printf("[UDP] Failed to create socket\n");
        return;
    }

    /* Bind to local port */
    sockaddr_in_t local_addr;
    local_addr.addr = network_config.ip;
    local_addr.port = 5000;

    if (net_bind(sock, &local_addr) != OS_OK) {
        printf("[UDP] Failed to bind socket\n");
        net_close(sock);
        return;
    }

    printf("[UDP] Listening on port 5000\n");

    while (1) {
        /* Send UDP packet */
        const char *msg = "Hello from TinyOS!";
        sockaddr_in_t dest_addr;
        dest_addr.addr = IPV4(192, 168, 1, 200);
        dest_addr.port = 6000;

        int32_t sent = net_sendto(sock, msg, strlen(msg), &dest_addr);
        if (sent > 0) {
            printf("[UDP] Sent %ld bytes to %d.%d.%d.%d:%d\n",
                   sent, IPV4_ADDR(dest_addr.addr), dest_addr.port);
        }

        /* Try to receive */
        char buffer[128];
        sockaddr_in_t from_addr;
        int32_t received = net_recvfrom(sock, buffer, sizeof(buffer) - 1, &from_addr);

        if (received > 0) {
            buffer[received] = '\0';
            printf("[UDP] Received %ld bytes from %d.%d.%d.%d:%d: %s\n",
                   received, IPV4_ADDR(from_addr.addr), from_addr.port, buffer);
        }

        os_task_delay(5000);  /* Send every 5 seconds */
    }
}

/**
 * @brief TCP demo task
 */
void tcp_demo_task(void *param) {
    (void)param;

    printf("[TCP] Starting TCP demo...\n");

    while (1) {
        /* Create TCP socket */
        net_socket_t sock = net_socket(SOCK_STREAM);
        if (sock == INVALID_SOCKET) {
            printf("[TCP] Failed to create socket\n");
            os_task_delay(5000);
            continue;
        }

        /* Connect to server */
        sockaddr_in_t server_addr;
        server_addr.addr = IPV4(192, 168, 1, 200);
        server_addr.port = 8080;

        printf("[TCP] Connecting to %d.%d.%d.%d:%d...\n",
               IPV4_ADDR(server_addr.addr), server_addr.port);

        os_error_t err = net_connect(sock, &server_addr, 5000);
        if (err != OS_OK) {
            printf("[TCP] Connection failed\n");
            net_close(sock);
            os_task_delay(5000);
            continue;
        }

        printf("[TCP] Connected!\n");

        /* Send data */
        const char *msg = "GET / HTTP/1.1\r\nHost: server\r\n\r\n";
        int32_t sent = net_send(sock, msg, strlen(msg), 2000);
        printf("[TCP] Sent %ld bytes\n", sent);

        /* Receive response */
        char buffer[256];
        int32_t received = net_recv(sock, buffer, sizeof(buffer) - 1, 5000);
        if (received > 0) {
            buffer[received] = '\0';
            printf("[TCP] Received %ld bytes:\n%s\n", received, buffer);
        }

        net_close(sock);

        os_task_delay(10000);  /* Connect every 10 seconds */
    }
}

/**
 * @brief HTTP demo task
 */
void http_demo_task(void *param) {
    (void)param;

    printf("[HTTP] Starting HTTP demo...\n");

    while (1) {
        /* HTTP GET request */
        http_response_t response;
        memset(&response, 0, sizeof(response));

        printf("[HTTP] Sending GET request...\n");

        os_error_t err = net_http_get("http://192.168.1.200:80/api/status",
                                       &response, 10000);

        if (err == OS_OK) {
            printf("[HTTP] Response status: %d\n", response.status_code);
            if (response.body) {
                printf("[HTTP] Body (%lu bytes):\n%s\n",
                       response.body_length, response.body);
                net_http_free_response(&response);
            }
        } else {
            printf("[HTTP] Request failed\n");
        }

        os_task_delay(15000);  /* Request every 15 seconds */
    }
}

/**
 * @brief Network statistics task
 */
void net_stats_task(void *param) {
    (void)param;

    while (1) {
        net_stats_t stats;
        net_get_stats(&stats);

        printf("\n===== Network Statistics =====\n");
        printf("Ethernet RX: %lu packets, TX: %lu packets\n",
               stats.eth_rx_packets, stats.eth_tx_packets);
        printf("IP RX: %lu packets, TX: %lu packets\n",
               stats.ip_rx_packets, stats.ip_tx_packets);
        printf("ICMP RX: %lu packets, TX: %lu packets\n",
               stats.icmp_rx_packets, stats.icmp_tx_packets);
        printf("UDP RX: %lu packets, TX: %lu packets\n",
               stats.udp_rx_packets, stats.udp_tx_packets);
        printf("TCP RX: %lu packets, TX: %lu packets\n",
               stats.tcp_rx_packets, stats.tcp_tx_packets);
        printf("TCP Connections: %lu, Resets: %lu\n",
               stats.tcp_connections, stats.tcp_resets);
        printf("==============================\n\n");

        os_task_delay(20000);  /* Print every 20 seconds */
    }
}

/*===========================================================================
 * Main Function
 *===========================================================================*/

int main(void) {
    tcb_t ping_task;
    tcb_t udp_task;
    tcb_t tcp_task;
    tcb_t http_task;
    tcb_t stats_task;

    printf("\n");
    printf("====================================\n");
    printf("  TinyOS Network Stack Demo\n");
    printf("====================================\n\n");

    /* Initialize TinyOS */
    os_init();
    os_mem_init();

    /* Initialize network stack */
    net_driver_t *driver = loopback_get_driver();

    printf("Initializing network stack...\n");
    printf("IP Address: %d.%d.%d.%d\n", IPV4_ADDR(network_config.ip));
    printf("Netmask:    %d.%d.%d.%d\n", IPV4_ADDR(network_config.netmask));
    printf("Gateway:    %d.%d.%d.%d\n", IPV4_ADDR(network_config.gateway));
    printf("DNS:        %d.%d.%d.%d\n\n", IPV4_ADDR(network_config.dns));

    os_error_t err = net_init(driver, &network_config);
    if (err != OS_OK) {
        printf("Network initialization failed!\n");
        return -1;
    }

    /* Start network stack */
    err = net_start();
    if (err != OS_OK) {
        printf("Failed to start network stack!\n");
        return -1;
    }

    printf("Network stack started!\n\n");

    /* Create demo tasks */
    os_task_create(&ping_task, "ping", ping_demo_task, NULL, PRIORITY_NORMAL);
    os_task_create(&udp_task, "udp", udp_demo_task, NULL, PRIORITY_NORMAL);
    os_task_create(&tcp_task, "tcp", tcp_demo_task, NULL, PRIORITY_NORMAL);
    os_task_create(&http_task, "http", http_demo_task, NULL, PRIORITY_NORMAL);
    os_task_create(&stats_task, "stats", net_stats_task, NULL, PRIORITY_LOW);

    printf("Demo tasks created. Starting scheduler...\n\n");

    /* Start scheduler */
    os_start();

    return 0;
}
