/**
 * @file http_dns.c
 * @brief HTTP Client/Server and DNS Client (Simplified)
 */

#include "tinyos/net.h"
#include <string.h>
#include <stdlib.h>

/*===========================================================================
 * HTTP Client Implementation
 *===========================================================================*/

/**
 * @brief Parse URL into components
 */
static bool parse_url(const char *url, ipv4_addr_t *ip, uint16_t *port, char *path, size_t path_len) {
    /* Simple URL parser for http://IP:PORT/path */
    const char *p = url;

    /* Skip http:// */
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    }

    /* Extract IP address */
    char ip_str[16];
    int i = 0;
    while (*p && *p != ':' && *p != '/' && i < 15) {
        ip_str[i++] = *p++;
    }
    ip_str[i] = '\0';

    if (!net_parse_ipv4(ip_str, ip)) {
        return false;
    }

    /* Extract port (default 80) */
    *port = 80;
    if (*p == ':') {
        p++;
        *port = 0;
        while (*p >= '0' && *p <= '9') {
            *port = (*port) * 10 + (*p - '0');
            p++;
        }
    }

    /* Extract path */
    if (*p == '/') {
        strncpy(path, p, path_len - 1);
        path[path_len - 1] = '\0';
    } else {
        strcpy(path, "/");
    }

    return true;
}

os_error_t net_http_request(
    http_method_t method,
    const char *url,
    const char **headers,
    const void *body,
    uint32_t body_length,
    http_response_t *response,
    uint32_t timeout_ms
) {
    ipv4_addr_t server_ip;
    uint16_t port;
    char path[128];

    if (!parse_url(url, &server_ip, &port, path, sizeof(path))) {
        return OS_ERR_INVALID_PARAM;
    }

    /* Create TCP socket */
    net_socket_t sock = net_socket(SOCK_STREAM);
    if (sock == INVALID_SOCKET) {
        return OS_ERR_NO_RESOURCE;
    }

    /* Connect to server */
    sockaddr_in_t addr;
    addr.addr = server_ip;
    addr.port = port;

    os_error_t err = net_connect(sock, &addr, timeout_ms);
    if (err != OS_OK) {
        net_close(sock);
        return err;
    }

    /* Build HTTP request */
    char request[512];
    const char *method_str = (method == HTTP_GET) ? "GET" : "POST";

    int req_len = snprintf(request, sizeof(request),
        "%s %s HTTP/1.1\r\n"
        "Host: %d.%d.%d.%d\r\n"
        "Connection: close\r\n",
        method_str, path,
        IPV4_ADDR(server_ip)
    );

    /* Add custom headers */
    if (headers) {
        for (int i = 0; headers[i] != NULL; i++) {
            req_len += snprintf(request + req_len, sizeof(request) - req_len,
                "%s\r\n", headers[i]);
        }
    }

    /* Add body */
    if (body && body_length > 0) {
        req_len += snprintf(request + req_len, sizeof(request) - req_len,
            "Content-Length: %lu\r\n\r\n", (unsigned long)body_length);

        /* Send request header */
        net_send(sock, request, req_len, timeout_ms);
        /* Send body */
        net_send(sock, body, body_length, timeout_ms);
    } else {
        req_len += snprintf(request + req_len, sizeof(request) - req_len, "\r\n");
        net_send(sock, request, req_len, timeout_ms);
    }

    /* Receive response */
    uint8_t rx_buffer[2048];
    int32_t total_received = 0;
    int32_t bytes;

    while ((bytes = net_recv(sock, rx_buffer + total_received,
                             sizeof(rx_buffer) - total_received - 1, 2000)) > 0) {
        total_received += bytes;
        if (total_received >= (int32_t)sizeof(rx_buffer) - 1) {
            break;
        }
    }

    rx_buffer[total_received] = '\0';
    net_close(sock);

    if (total_received == 0) {
        return OS_ERR_TIMEOUT;
    }

    /* Parse response (simplified) */
    char *response_str = (char *)rx_buffer;

    /* Extract status code */
    char *status_line = strstr(response_str, "HTTP/1.");
    if (status_line) {
        sscanf(status_line, "HTTP/1.%*d %hu", &response->status_code);
    }

    /* Find body (after \r\n\r\n) */
    char *body_start = strstr(response_str, "\r\n\r\n");
    if (body_start) {
        body_start += 4;
        response->body_length = total_received - (body_start - response_str);
        response->body = os_malloc(response->body_length + 1);
        if (response->body) {
            memcpy(response->body, body_start, response->body_length);
            response->body[response->body_length] = '\0';
        }
    }

    return OS_OK;
}

void net_http_free_response(http_response_t *response) {
    if (response && response->body) {
        os_free(response->body);
        response->body = NULL;
    }
}

os_error_t net_http_get(const char *url, http_response_t *response, uint32_t timeout_ms) {
    return net_http_request(HTTP_GET, url, NULL, NULL, 0, response, timeout_ms);
}

os_error_t net_http_post(
    const char *url,
    const char *content_type,
    const void *body,
    uint32_t body_length,
    http_response_t *response,
    uint32_t timeout_ms
) {
    char ct_header[128];
    snprintf(ct_header, sizeof(ct_header), "Content-Type: %s", content_type);

    const char *headers[] = { ct_header, NULL };

    return net_http_request(HTTP_POST, url, headers, body, body_length, response, timeout_ms);
}

/*===========================================================================
 * HTTP Server (Stub)
 *===========================================================================*/

os_error_t net_http_server_start(uint16_t port, http_handler_t handler) {
    (void)port;
    (void)handler;
    /* Not implemented - would require server socket and accept() */
    return OS_ERR_NOT_IMPLEMENTED;
}

os_error_t net_http_send_response(
    const http_request_t *request,
    uint16_t status_code,
    const char *content_type,
    const void *body,
    uint32_t body_length
) {
    (void)request;
    (void)status_code;
    (void)content_type;
    (void)body;
    (void)body_length;
    return OS_ERR_NOT_IMPLEMENTED;
}

/*===========================================================================
 * DNS Client (Simplified)
 *===========================================================================*/

os_error_t net_dns_resolve(const char *hostname, ipv4_addr_t *ip, uint32_t timeout_ms) {
    (void)timeout_ms;

    /* This is a stub - real DNS would:
     * 1. Create UDP socket
     * 2. Build DNS query packet
     * 3. Send to DNS server (port 53)
     * 4. Wait for DNS response
     * 5. Parse response and extract IP
     *
     * For now, we just fail
     */

    (void)hostname;
    (void)ip;

    return OS_ERR_NOT_IMPLEMENTED;
}
