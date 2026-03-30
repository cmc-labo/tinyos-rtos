/**
 * @file tls_demo.c
 * @brief TLS / DTLS usage examples for TinyOS
 *
 * Demonstrates:
 *  1. HTTPS GET (TLS over TCP)
 *  2. MQTT over TLS
 *  3. CoAP over DTLS (with PSK)
 */

#include "tinyos.h"
#include "tinyos/net.h"
#include "tinyos/tls.h"
#include "tinyos/mqtt.h"
#include "tinyos/coap.h"
#include <string.h>
#include <stdio.h>

/* -------------------------------------------------------------------------
 * Example CA certificate (PEM).  Replace with your real CA cert.
 * Must be NULL-terminated for mbedTLS PEM parsing.
 * ------------------------------------------------------------------------- */
static const uint8_t EXAMPLE_CA_CERT[] =
    "-----BEGIN CERTIFICATE-----\r\n"
    "... (your CA certificate here) ...\r\n"
    "-----END CERTIFICATE-----\r\n";

/* -------------------------------------------------------------------------
 * Example 1: HTTPS GET
 * ------------------------------------------------------------------------- */
static void task_https_get(void *param) {
    (void)param;

    /* Resolve hostname */
    ipv4_addr_t server_ip;
    if (net_dns_resolve("example.com", &server_ip, 5000) != OS_OK) {
        printf("DNS resolution failed\n");
        return;
    }

    /* Open TCP connection */
    net_socket_t sock = net_socket(SOCK_STREAM);
    sockaddr_in_t addr = { .addr = server_ip, .port = 443 };
    if (net_connect(sock, &addr, 5000) != OS_OK) {
        printf("TCP connect failed\n");
        net_close(sock);
        return;
    }

    /* TLS handshake */
    tls_context_t tls;
    tls_config_t cfg = TLS_CONFIG_DEFAULT_CLIENT;
    cfg.ca_cert     = EXAMPLE_CA_CERT;
    cfg.ca_cert_len = sizeof(EXAMPLE_CA_CERT);

    tls_init(&tls, &cfg);

    tls_error_t err = tls_connect(&tls, sock, "example.com", 5000);
    if (err != TLS_OK) {
        printf("TLS handshake failed: %s\n", tls_error_to_string(err));
        tls_free(&tls);
        net_close(sock);
        return;
    }
    printf("TLS handshake OK  cipher: %s\n",
           mbedtls_ssl_get_ciphersuite(&tls.ssl));

    /* Send HTTP GET over TLS */
    const char req[] =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Connection: close\r\n"
        "\r\n";
    tls_send(&tls, req, (uint16_t)(sizeof(req) - 1));

    /* Receive response */
    char buf[512];
    int32_t n;
    while ((n = tls_recv(&tls, buf, sizeof(buf) - 1, 5000)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }

    tls_close(&tls);
    tls_free(&tls);
    net_close(sock);
}

/* -------------------------------------------------------------------------
 * Example 2: MQTT over TLS (port 8883)
 * ------------------------------------------------------------------------- */
static void on_mqtt_message(const char *topic, const uint8_t *payload,
                             uint32_t len, void *user_data) {
    (void)user_data;
    printf("MQTT msg [%s]: %.*s\n", topic, (int)len, payload);
}

static void task_mqtt_tls(void *param) {
    (void)param;

    /* Resolve broker */
    ipv4_addr_t broker_ip;
    if (net_dns_resolve("mqtt.example.com", &broker_ip, 5000) != OS_OK) {
        printf("Broker DNS failed\n");
        return;
    }

    /* TLS: connect to port 8883 */
    net_socket_t sock = net_socket(SOCK_STREAM);
    sockaddr_in_t addr = { .addr = broker_ip, .port = 8883 };
    net_connect(sock, &addr, 5000);

    tls_context_t tls;
    tls_config_t cfg = TLS_CONFIG_DEFAULT_CLIENT;
    cfg.ca_cert     = EXAMPLE_CA_CERT;
    cfg.ca_cert_len = sizeof(EXAMPLE_CA_CERT);
    tls_init(&tls, &cfg);
    tls_connect(&tls, sock, "mqtt.example.com", 5000);

    /*
     * Hand the TLS context to MQTT.
     * mqtt_connect_tls() (declared below as a thin wrapper) uses
     * tls_send/tls_recv instead of net_send/net_recv.
     */
    mqtt_client_t client;
    mqtt_config_t mcfg;
    memset(&mcfg, 0, sizeof(mcfg));
    strncpy(mcfg.client_id, "tinyos-device-01", sizeof(mcfg.client_id) - 1);
    strncpy(mcfg.broker_host, "mqtt.example.com", sizeof(mcfg.broker_host) - 1);
    mcfg.broker_port = 8883;

    mqtt_client_init(&client, &mcfg);
    mqtt_set_message_callback(&client, on_mqtt_message, NULL);

    /*
     * For MQTT-over-TLS the caller connects the TCP socket, performs the
     * TLS handshake, then passes the TLS-wrapped socket to the MQTT layer
     * via mqtt_connect_with_tls().  That function uses tls_send/tls_recv
     * for all I/O so the MQTT layer is fully unaware of TLS.
     */
    if (mqtt_connect_with_tls(&client, &tls) == MQTT_OK) {
        printf("MQTT over TLS connected\n");
        mqtt_subscribe(&client, "sensors/#", MQTT_QOS_1);
        mqtt_publish(&client, "sensors/temp", (const uint8_t *)"25.3", 4,
                     MQTT_QOS_1, false);

        /* Receive a few messages */
        for (int i = 0; i < 10; i++) {
            mqtt_loop(&client);
            os_task_delay(100);
        }
        mqtt_disconnect(&client);
    }

    tls_close(&tls);
    tls_free(&tls);
    net_close(sock);
}

/* -------------------------------------------------------------------------
 * Example 3: CoAP over DTLS with Pre-Shared Key
 * ------------------------------------------------------------------------- */
static const uint8_t EXAMPLE_PSK[]    = { 0x73, 0x65, 0x63, 0x72, 0x65, 0x74 }; /* "secret" */
static const char    EXAMPLE_PSK_ID[] = "tinyos-client";

static void task_coap_dtls(void *param) {
    (void)param;

    ipv4_addr_t server_ip;
    net_dns_resolve("coap.example.com", &server_ip, 5000);

    /* DTLS with PSK — no certificate needed */
    tls_context_t tls;
    tls_config_t cfg = TLS_CONFIG_DEFAULT_DTLS_CLIENT;
    cfg.verify_mode = TLS_VERIFY_NONE;   /* PSK mode: no cert */
    cfg.psk         = EXAMPLE_PSK;
    cfg.psk_len     = sizeof(EXAMPLE_PSK);
    cfg.psk_id      = EXAMPLE_PSK_ID;

    tls_init(&tls, &cfg);

    net_socket_t sock = net_socket(SOCK_DGRAM);
    sockaddr_in_t peer = { .addr = server_ip, .port = 5684 /* CoAPS */ };

    tls_error_t err = tls_connect_dtls(&tls, sock, &peer,
                                        "coap.example.com", 5000);
    if (err != TLS_OK) {
        printf("DTLS handshake failed: %s\n", tls_error_to_string(err));
        tls_free(&tls);
        net_close(sock);
        return;
    }
    printf("DTLS handshake OK\n");

    /*
     * Send a CoAP GET /sensors/temperature over DTLS.
     * Encode the CoAP PDU manually and hand the bytes to tls_send().
     */
    coap_context_t coap;
    coap_config_t coap_cfg = { .port = 5684 };
    coap_init(&coap, &coap_cfg, false);
    coap_start(&coap);

    coap_response_t response;
    /* coap_get_dtls() is a thin wrapper that passes tls_send/tls_recv */
    coap_error_t cerr = coap_get_with_tls(&coap, &tls,
                                           "/sensors/temperature",
                                           &response, 5000);
    if (cerr == COAP_OK && response.success) {
        printf("CoAP response: %.*s\n",
               (int)response.payload_length, (char *)response.payload);
        coap_response_free(&response);
    }

    coap_stop(&coap);
    tls_close(&tls);
    tls_free(&tls);
    net_close(sock);
}

/* -------------------------------------------------------------------------
 * Main entry point
 * ------------------------------------------------------------------------- */
void tls_demo_main(void) {
    static tcb_t task_https, task_mqtt, task_coap;

    os_task_create(&task_https, "https_get",  task_https_get,  NULL, PRIORITY_NORMAL);
    os_task_create(&task_mqtt,  "mqtt_tls",   task_mqtt_tls,   NULL, PRIORITY_NORMAL);
    os_task_create(&task_coap,  "coap_dtls",  task_coap_dtls,  NULL, PRIORITY_NORMAL);
}
