/**
 * @file tls.c
 * @brief TLS/DTLS implementation for TinyOS (mbedTLS backend)
 *
 * Bridges mbedTLS to the TinyOS socket API.
 * - TLS  : wraps net_send / net_recv  (SOCK_STREAM)
 * - DTLS : wraps net_sendto / net_recvfrom (SOCK_DGRAM)
 */

#include "tinyos/tls.h"
#include <string.h>

/*===========================================================================
 * Internal: mbedTLS BIO callbacks
 *===========================================================================*/

/**
 * mbedTLS calls this to send bytes — we forward to the TinyOS socket.
 * Must return the number of bytes written, or a negative MBEDTLS_ERR_* code.
 */
static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    tls_context_t *tls = (tls_context_t *)ctx;
    int32_t sent = net_send(tls->sock, buf, (uint16_t)len, tls->config.io_timeout_ms);
    if (sent == 0) {
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    if (sent < 0) {
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return (int)sent;
}

/**
 * mbedTLS calls this to receive bytes — we forward to the TinyOS socket.
 */
static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    tls_context_t *tls = (tls_context_t *)ctx;
    int32_t rcvd = net_recv(tls->sock, buf, (uint16_t)len, tls->config.io_timeout_ms);
    if (rcvd == 0) {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (rcvd < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)rcvd;
}

/**
 * Variant with timeout, used by mbedTLS for DTLS retransmission.
 */
static int tls_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                 uint32_t timeout_ms) {
    tls_context_t *tls = (tls_context_t *)ctx;
    int32_t rcvd = net_recv(tls->sock, buf, (uint16_t)len,
                            timeout_ms ? timeout_ms : tls->config.io_timeout_ms);
    if (rcvd == 0) {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
    if (rcvd < 0) {
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return (int)rcvd;
}

/* DTLS: BIO wrappers that use sendto/recvfrom */
static int dtls_bio_send(void *ctx, const unsigned char *buf, size_t len) {
    tls_context_t *tls = (tls_context_t *)ctx;
    int32_t sent = net_sendto(tls->sock, buf, (uint16_t)len, &tls->peer_addr);
    if (sent < 0) return MBEDTLS_ERR_NET_SEND_FAILED;
    if (sent == 0) return MBEDTLS_ERR_SSL_WANT_WRITE;
    return (int)sent;
}

static int dtls_bio_recv(void *ctx, unsigned char *buf, size_t len) {
    tls_context_t *tls = (tls_context_t *)ctx;
    sockaddr_in_t from;
    int32_t rcvd = net_recvfrom(tls->sock, buf, (uint16_t)len, &from);
    if (rcvd == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    if (rcvd < 0)  return MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)rcvd;
}

static int dtls_bio_recv_timeout(void *ctx, unsigned char *buf, size_t len,
                                  uint32_t timeout_ms) {
    tls_context_t *tls = (tls_context_t *)ctx;
    sockaddr_in_t from;
    /* Temporarily override io_timeout to honour the DTLS retransmission timer */
    uint32_t saved = tls->config.io_timeout_ms;
    tls->config.io_timeout_ms = timeout_ms ? timeout_ms : saved;
    int32_t rcvd = net_recvfrom(tls->sock, buf, (uint16_t)len, &from);
    tls->config.io_timeout_ms = saved;
    if (rcvd == 0) return MBEDTLS_ERR_SSL_TIMEOUT;
    if (rcvd < 0)  return MBEDTLS_ERR_NET_RECV_FAILED;
    return (int)rcvd;
}

/*===========================================================================
 * Internal: common setup shared by client and server
 *===========================================================================*/

static tls_error_t setup_ssl_config(tls_context_t *ctx) {
    int ret;
    int endpoint = (ctx->config.role == TLS_ROLE_CLIENT)
                   ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER;
    int transport = (ctx->config.transport == TLS_TRANSPORT_DTLS)
                    ? MBEDTLS_SSL_TRANSPORT_DATAGRAM
                    : MBEDTLS_SSL_TRANSPORT_STREAM;

    ret = mbedtls_ssl_config_defaults(&ctx->ssl_cfg, endpoint, transport,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) return TLS_ERR_CRYPTO;

    /* RNG */
    mbedtls_ssl_conf_rng(&ctx->ssl_cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    /* Certificate verification mode */
    int authmode;
    switch (ctx->config.verify_mode) {
        case TLS_VERIFY_NONE:     authmode = MBEDTLS_SSL_VERIFY_NONE;     break;
        case TLS_VERIFY_OPTIONAL: authmode = MBEDTLS_SSL_VERIFY_OPTIONAL; break;
        default:                  authmode = MBEDTLS_SSL_VERIFY_REQUIRED; break;
    }
    mbedtls_ssl_conf_authmode(&ctx->ssl_cfg, authmode);

    /* CA certificate for peer verification */
    if (ctx->config.ca_cert != NULL && ctx->config.ca_cert_len > 0) {
        ret = mbedtls_x509_crt_parse(&ctx->ca_cert,
                                     ctx->config.ca_cert,
                                     ctx->config.ca_cert_len);
        if (ret != 0) return TLS_ERR_CERT_VERIFY;
        mbedtls_ssl_conf_ca_chain(&ctx->ssl_cfg, &ctx->ca_cert, NULL);
    }

    /* Own certificate + private key (mutual TLS or server auth) */
    if (ctx->config.own_cert != NULL && ctx->config.private_key != NULL) {
        ret = mbedtls_x509_crt_parse(&ctx->own_cert,
                                     ctx->config.own_cert,
                                     ctx->config.own_cert_len);
        if (ret != 0) return TLS_ERR_CERT_VERIFY;

        ret = mbedtls_pk_parse_key(&ctx->private_key,
                                   ctx->config.private_key,
                                   ctx->config.private_key_len,
                                   NULL, 0,
                                   mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
        if (ret != 0) return TLS_ERR_CRYPTO;

        ret = mbedtls_ssl_conf_own_cert(&ctx->ssl_cfg,
                                         &ctx->own_cert, &ctx->private_key);
        if (ret != 0) return TLS_ERR_CRYPTO;
    }

    /* Pre-Shared Key (alternative to certs) */
    if (ctx->config.psk != NULL && ctx->config.psk_id != NULL) {
        ret = mbedtls_ssl_conf_psk(&ctx->ssl_cfg,
                                    ctx->config.psk, ctx->config.psk_len,
                                    (const unsigned char *)ctx->config.psk_id,
                                    strlen(ctx->config.psk_id));
        if (ret != 0) return TLS_ERR_CRYPTO;
    }

    /* DTLS: attach timing callbacks for retransmission */
    if (ctx->config.transport == TLS_TRANSPORT_DTLS) {
        mbedtls_ssl_conf_handshake_timeout(&ctx->ssl_cfg,
                                            1000,  /* min retry delay ms */
                                            ctx->config.handshake_timeout_ms);
    }

    ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->ssl_cfg);
    if (ret != 0) return TLS_ERR_CRYPTO;

    return TLS_OK;
}

static tls_error_t run_handshake(tls_context_t *ctx) {
    int ret;
    uint32_t start = os_get_tick_count();

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED)
                   ? TLS_ERR_CERT_VERIFY : TLS_ERR_HANDSHAKE;
        }
        if ((os_get_tick_count() - start) >= ctx->config.handshake_timeout_ms) {
            return TLS_ERR_TIMEOUT;
        }
        os_task_delay(1);
    }

    ctx->handshake_done = true;
    return TLS_OK;
}

/*===========================================================================
 * Public API
 *===========================================================================*/

tls_error_t tls_init(tls_context_t *ctx, const tls_config_t *cfg) {
    if (ctx == NULL || cfg == NULL) return TLS_ERR_INVALID_PARAM;

    memset(ctx, 0, sizeof(tls_context_t));
    ctx->config = *cfg;
    ctx->sock   = INVALID_SOCKET;

    /* Apply default timeouts if caller left them zero */
    if (ctx->config.handshake_timeout_ms == 0)
        ctx->config.handshake_timeout_ms = TLS_HANDSHAKE_TIMEOUT_MS;
    if (ctx->config.io_timeout_ms == 0)
        ctx->config.io_timeout_ms = TLS_DEFAULT_TIMEOUT_MS;

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->ssl_cfg);
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    mbedtls_x509_crt_init(&ctx->ca_cert);
    mbedtls_x509_crt_init(&ctx->own_cert);
    mbedtls_pk_init(&ctx->private_key);

    /* Seed the RNG */
    static const char pers[] = "tinyos_tls";
    int ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func,
                                     &ctx->entropy,
                                     (const unsigned char *)pers, sizeof(pers) - 1);
    if (ret != 0) {
        tls_free(ctx);
        return TLS_ERR_CRYPTO;
    }

    ctx->initialized = true;
    return TLS_OK;
}

tls_error_t tls_connect(tls_context_t *ctx, net_socket_t sock,
                         const char *hostname, uint32_t timeout_ms) {
    if (!ctx || !ctx->initialized || sock == INVALID_SOCKET)
        return TLS_ERR_INVALID_PARAM;

    ctx->sock = sock;
    if (timeout_ms > 0) ctx->config.handshake_timeout_ms = timeout_ms;

    tls_error_t err = setup_ssl_config(ctx);
    if (err != TLS_OK) return err;

    /* SNI: send server name in ClientHello */
    if (hostname != NULL) {
        mbedtls_ssl_set_hostname(&ctx->ssl, hostname);
    }

    /* BIO: wire up TLS read/write to net_send/net_recv */
    mbedtls_ssl_set_bio(&ctx->ssl, ctx,
                         tls_bio_send, tls_bio_recv, tls_bio_recv_timeout);

    return run_handshake(ctx);
}

tls_error_t tls_connect_dtls(tls_context_t *ctx, net_socket_t sock,
                               const sockaddr_in_t *peer, const char *hostname,
                               uint32_t timeout_ms) {
    if (!ctx || !ctx->initialized || sock == INVALID_SOCKET || peer == NULL)
        return TLS_ERR_INVALID_PARAM;

    if (ctx->config.transport != TLS_TRANSPORT_DTLS)
        return TLS_ERR_INVALID_PARAM;

    ctx->sock      = sock;
    ctx->peer_addr = *peer;
    if (timeout_ms > 0) ctx->config.handshake_timeout_ms = timeout_ms;

    tls_error_t err = setup_ssl_config(ctx);
    if (err != TLS_OK) return err;

    if (hostname != NULL) {
        mbedtls_ssl_set_hostname(&ctx->ssl, hostname);
    }

    /* DTLS timing callbacks — needed for retransmission */
    mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->timer,
                              mbedtls_timing_set_delay,
                              mbedtls_timing_get_delay);

    mbedtls_ssl_set_bio(&ctx->ssl, ctx,
                         dtls_bio_send, dtls_bio_recv, dtls_bio_recv_timeout);

    return run_handshake(ctx);
}

tls_error_t tls_accept(tls_context_t *ctx, net_socket_t sock,
                        uint32_t timeout_ms) {
    if (!ctx || !ctx->initialized || sock == INVALID_SOCKET)
        return TLS_ERR_INVALID_PARAM;

    ctx->sock = sock;
    if (timeout_ms > 0) ctx->config.handshake_timeout_ms = timeout_ms;

    tls_error_t err = setup_ssl_config(ctx);
    if (err != TLS_OK) return err;

    if (ctx->config.transport == TLS_TRANSPORT_DTLS) {
        mbedtls_ssl_set_timer_cb(&ctx->ssl, &ctx->timer,
                                  mbedtls_timing_set_delay,
                                  mbedtls_timing_get_delay);
        mbedtls_ssl_set_bio(&ctx->ssl, ctx,
                             dtls_bio_send, dtls_bio_recv, dtls_bio_recv_timeout);
    } else {
        mbedtls_ssl_set_bio(&ctx->ssl, ctx,
                             tls_bio_send, tls_bio_recv, tls_bio_recv_timeout);
    }

    return run_handshake(ctx);
}

int32_t tls_send(tls_context_t *ctx, const void *data, uint16_t length) {
    if (!ctx || !ctx->handshake_done || data == NULL || length == 0)
        return TLS_ERR_INVALID_PARAM;

    int ret;
    uint16_t sent = 0;

    while (sent < length) {
        ret = mbedtls_ssl_write(&ctx->ssl,
                                 (const unsigned char *)data + sent,
                                 length - sent);
        if (ret > 0) {
            sent += (uint16_t)ret;
        } else if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            os_task_delay(1);
        } else if (ret == 0) {
            return TLS_ERR_CLOSED;
        } else {
            return TLS_ERR_SEND;
        }
    }

    return (int32_t)sent;
}

int32_t tls_recv(tls_context_t *ctx, void *buffer, uint16_t max_length,
                  uint32_t timeout_ms) {
    if (!ctx || !ctx->handshake_done || buffer == NULL || max_length == 0)
        return TLS_ERR_INVALID_PARAM;

    uint32_t deadline = os_get_tick_count() + timeout_ms;

    for (;;) {
        int ret = mbedtls_ssl_read(&ctx->ssl,
                                    (unsigned char *)buffer, max_length);
        if (ret > 0) {
            return (int32_t)ret;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return TLS_ERR_CLOSED;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            if (timeout_ms > 0 && os_get_tick_count() >= deadline) {
                return 0;  /* Caller-visible timeout: no bytes */
            }
            os_task_delay(1);
            continue;
        }
        return TLS_ERR_RECV;
    }
}

void tls_close(tls_context_t *ctx) {
    if (!ctx || !ctx->handshake_done) return;

    /* Send close_notify; ignore errors (peer may have already closed) */
    mbedtls_ssl_close_notify(&ctx->ssl);
    ctx->handshake_done = false;
}

void tls_free(tls_context_t *ctx) {
    if (!ctx) return;

    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->ssl_cfg);
    mbedtls_entropy_free(&ctx->entropy);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_x509_crt_free(&ctx->ca_cert);
    mbedtls_x509_crt_free(&ctx->own_cert);
    mbedtls_pk_free(&ctx->private_key);

    ctx->initialized    = false;
    ctx->handshake_done = false;
    ctx->sock           = INVALID_SOCKET;
}

const char *tls_error_to_string(tls_error_t err) {
    switch (err) {
        case TLS_OK:                  return "OK";
        case TLS_ERR_INVALID_PARAM:   return "Invalid parameter";
        case TLS_ERR_NOT_INITIALIZED: return "Not initialized";
        case TLS_ERR_HANDSHAKE:       return "Handshake failed";
        case TLS_ERR_CERT_VERIFY:     return "Certificate verification failed";
        case TLS_ERR_SEND:            return "Send error";
        case TLS_ERR_RECV:            return "Receive error";
        case TLS_ERR_TIMEOUT:         return "Timeout";
        case TLS_ERR_CLOSED:          return "Connection closed";
        case TLS_ERR_NO_MEMORY:       return "Out of memory";
        case TLS_ERR_CRYPTO:          return "Crypto error";
        default:                      return "Unknown error";
    }
}

void tls_mbedtls_strerror(int mbedtls_err, char *buf, size_t buf_len) {
    mbedtls_strerror(mbedtls_err, buf, buf_len);
}
