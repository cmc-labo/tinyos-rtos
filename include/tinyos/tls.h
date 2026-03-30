/**
 * @file tls.h
 * @brief TLS/DTLS support for TinyOS
 *
 * TLS 1.2/1.3 over TCP and DTLS 1.2 over UDP.
 * Uses mbedTLS as the underlying crypto engine.
 *
 * Usage (TLS client):
 *   tls_context_t tls;
 *   tls_config_t cfg = TLS_CONFIG_DEFAULT_CLIENT;
 *   cfg.ca_cert     = ca_cert_pem;
 *   cfg.ca_cert_len = sizeof(ca_cert_pem);
 *   tls_init(&tls, &cfg);
 *
 *   net_socket_t sock = net_socket(SOCK_STREAM);
 *   net_connect(sock, &addr, 5000);
 *   tls_connect(&tls, sock, "example.com", 5000);
 *
 *   tls_send(&tls, data, len);
 *   tls_recv(&tls, buf, sizeof(buf), 5000);
 *   tls_close(&tls);
 *
 * Usage (DTLS client):
 *   tls_config_t cfg = TLS_CONFIG_DEFAULT_CLIENT;
 *   cfg.transport   = TLS_TRANSPORT_DTLS;
 *   ...
 *   net_socket_t sock = net_socket(SOCK_DGRAM);
 *   tls_connect(&tls, sock, "example.com", 5000);
 */

#ifndef TINYOS_TLS_H
#define TINYOS_TLS_H

#include "../tinyos.h"
#include "net.h"
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* mbedTLS includes (must be present in build) */
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "mbedtls/timing.h"

/*===========================================================================
 * Configuration
 *===========================================================================*/

#define TLS_MAX_HOSTNAME_LEN    64
#define TLS_MAX_PSK_LEN         32
#define TLS_MAX_PSK_ID_LEN      32
#define TLS_DEFAULT_TIMEOUT_MS  10000
#define TLS_HANDSHAKE_TIMEOUT_MS 5000

/*===========================================================================
 * Error Codes
 *===========================================================================*/

typedef enum {
    TLS_OK                  =  0,
    TLS_ERR_INVALID_PARAM   = -1,
    TLS_ERR_NOT_INITIALIZED = -2,
    TLS_ERR_HANDSHAKE       = -3,
    TLS_ERR_CERT_VERIFY     = -4,
    TLS_ERR_SEND            = -5,
    TLS_ERR_RECV            = -6,
    TLS_ERR_TIMEOUT         = -7,
    TLS_ERR_CLOSED          = -8,
    TLS_ERR_NO_MEMORY       = -9,
    TLS_ERR_CRYPTO          = -10,
} tls_error_t;

/*===========================================================================
 * Transport / Role / Auth Mode
 *===========================================================================*/

typedef enum {
    TLS_TRANSPORT_TLS  = 0,   /* TLS over TCP */
    TLS_TRANSPORT_DTLS = 1,   /* DTLS over UDP */
} tls_transport_t;

typedef enum {
    TLS_ROLE_CLIENT = 0,
    TLS_ROLE_SERVER = 1,
} tls_role_t;

typedef enum {
    TLS_VERIFY_NONE     = 0,  /* Skip certificate verification (insecure) */
    TLS_VERIFY_OPTIONAL = 1,  /* Verify if cert provided, continue on failure */
    TLS_VERIFY_REQUIRED = 2,  /* Require valid certificate (default) */
} tls_verify_mode_t;

/*===========================================================================
 * TLS Configuration
 *===========================================================================*/

typedef struct {
    tls_transport_t  transport;     /* TLS or DTLS */
    tls_role_t       role;          /* Client or server */
    tls_verify_mode_t verify_mode;  /* Certificate verification */

    /* CA certificate (PEM or DER, NULL-terminated PEM or raw DER) */
    const uint8_t   *ca_cert;
    size_t           ca_cert_len;

    /* Client/server certificate and private key (for mutual TLS) */
    const uint8_t   *own_cert;
    size_t           own_cert_len;
    const uint8_t   *private_key;
    size_t           private_key_len;

    /* Pre-Shared Key (alternative to certificates) */
    const uint8_t   *psk;
    size_t           psk_len;
    const char      *psk_id;        /* PSK identity string */

    /* Timeouts */
    uint32_t         handshake_timeout_ms;
    uint32_t         io_timeout_ms;
} tls_config_t;

/* Convenience initializers */
#define TLS_CONFIG_DEFAULT_CLIENT {             \
    .transport           = TLS_TRANSPORT_TLS,   \
    .role                = TLS_ROLE_CLIENT,     \
    .verify_mode         = TLS_VERIFY_REQUIRED, \
    .ca_cert             = NULL,                \
    .ca_cert_len         = 0,                   \
    .own_cert            = NULL,                \
    .own_cert_len        = 0,                   \
    .private_key         = NULL,                \
    .private_key_len     = 0,                   \
    .psk                 = NULL,                \
    .psk_len             = 0,                   \
    .psk_id              = NULL,                \
    .handshake_timeout_ms = TLS_HANDSHAKE_TIMEOUT_MS, \
    .io_timeout_ms       = TLS_DEFAULT_TIMEOUT_MS,    \
}

#define TLS_CONFIG_DEFAULT_SERVER {             \
    .transport           = TLS_TRANSPORT_TLS,   \
    .role                = TLS_ROLE_SERVER,     \
    .verify_mode         = TLS_VERIFY_NONE,     \
    .ca_cert             = NULL,                \
    .ca_cert_len         = 0,                   \
    .own_cert            = NULL,                \
    .own_cert_len        = 0,                   \
    .private_key         = NULL,                \
    .private_key_len     = 0,                   \
    .psk                 = NULL,                \
    .psk_len             = 0,                   \
    .psk_id              = NULL,                \
    .handshake_timeout_ms = TLS_HANDSHAKE_TIMEOUT_MS, \
    .io_timeout_ms       = TLS_DEFAULT_TIMEOUT_MS,    \
}

#define TLS_CONFIG_DEFAULT_DTLS_CLIENT {        \
    .transport           = TLS_TRANSPORT_DTLS,  \
    .role                = TLS_ROLE_CLIENT,     \
    .verify_mode         = TLS_VERIFY_REQUIRED, \
    .ca_cert             = NULL,                \
    .ca_cert_len         = 0,                   \
    .own_cert            = NULL,                \
    .own_cert_len        = 0,                   \
    .private_key         = NULL,                \
    .private_key_len     = 0,                   \
    .psk                 = NULL,                \
    .psk_len             = 0,                   \
    .psk_id              = NULL,                \
    .handshake_timeout_ms = TLS_HANDSHAKE_TIMEOUT_MS, \
    .io_timeout_ms       = TLS_DEFAULT_TIMEOUT_MS,    \
}

/*===========================================================================
 * TLS Context (per-connection state)
 *===========================================================================*/

typedef struct {
    tls_config_t        config;
    net_socket_t        sock;
    bool                initialized;
    bool                handshake_done;

    /* mbedTLS state */
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       ssl_cfg;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt         ca_cert;
    mbedtls_x509_crt         own_cert;
    mbedtls_pk_context       private_key;

    /* DTLS timing (required by mbedTLS for retransmission) */
    mbedtls_timing_delay_context timer;

    /* Peer address (DTLS only) */
    sockaddr_in_t        peer_addr;
} tls_context_t;

/*===========================================================================
 * API
 *===========================================================================*/

/**
 * @brief Initialize a TLS context with the given configuration
 * @param ctx  TLS context to initialize
 * @param cfg  Configuration (certificates, PSK, verify mode, etc.)
 * @return TLS_OK on success
 */
tls_error_t tls_init(tls_context_t *ctx, const tls_config_t *cfg);

/**
 * @brief Perform TLS handshake on an already-connected TCP socket (client)
 * @param ctx      Initialized TLS context
 * @param sock     Connected TCP socket (net_connect already called)
 * @param hostname Server hostname for SNI / certificate verification
 * @param timeout_ms Handshake timeout in milliseconds
 * @return TLS_OK on success
 */
tls_error_t tls_connect(tls_context_t *ctx, net_socket_t sock,
                         const char *hostname, uint32_t timeout_ms);

/**
 * @brief Perform DTLS handshake on a bound UDP socket (client)
 * @param ctx      Initialized DTLS context (transport must be TLS_TRANSPORT_DTLS)
 * @param sock     Bound UDP socket
 * @param peer     Remote peer address
 * @param hostname Remote hostname for certificate verification
 * @param timeout_ms Handshake timeout in milliseconds
 * @return TLS_OK on success
 */
tls_error_t tls_connect_dtls(tls_context_t *ctx, net_socket_t sock,
                               const sockaddr_in_t *peer, const char *hostname,
                               uint32_t timeout_ms);

/**
 * @brief Perform TLS handshake as server (accept incoming TLS connection)
 * @param ctx      Initialized TLS context (role must be TLS_ROLE_SERVER)
 * @param sock     Accepted TCP socket (net_accept already called)
 * @param timeout_ms Handshake timeout in milliseconds
 * @return TLS_OK on success
 */
tls_error_t tls_accept(tls_context_t *ctx, net_socket_t sock,
                        uint32_t timeout_ms);

/**
 * @brief Send data over a TLS/DTLS connection
 * @param ctx    TLS context (handshake must be complete)
 * @param data   Data to send
 * @param length Length in bytes
 * @return Number of bytes sent, or negative tls_error_t on error
 */
int32_t tls_send(tls_context_t *ctx, const void *data, uint16_t length);

/**
 * @brief Receive data over a TLS/DTLS connection
 * @param ctx        TLS context
 * @param buffer     Receive buffer
 * @param max_length Buffer size
 * @param timeout_ms Receive timeout in milliseconds
 * @return Number of bytes received, 0 on timeout, or negative tls_error_t on error
 */
int32_t tls_recv(tls_context_t *ctx, void *buffer, uint16_t max_length,
                  uint32_t timeout_ms);

/**
 * @brief Send TLS close_notify and close the connection
 * @param ctx TLS context
 */
void tls_close(tls_context_t *ctx);

/**
 * @brief Free all resources held by a TLS context
 *        (call after tls_close, or if tls_init succeeded but tls_connect failed)
 * @param ctx TLS context
 */
void tls_free(tls_context_t *ctx);

/**
 * @brief Get human-readable description of a TLS error
 * @param err tls_error_t value
 * @return Static string description
 */
const char *tls_error_to_string(tls_error_t err);

/**
 * @brief Get mbedTLS error string for a raw mbedTLS error code
 * @param mbedtls_err Raw mbedTLS error (negative)
 * @param buf         Output buffer
 * @param buf_len     Buffer length
 */
void tls_mbedtls_strerror(int mbedtls_err, char *buf, size_t buf_len);

#endif /* TINYOS_TLS_H */
