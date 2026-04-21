/**
 * @file mqtt.c
 * @brief MQTT 3.1.1 client implementation for TinyOS-RTOS
 *
 * Reliability model
 * -----------------
 *  In-flight table  (client->inflight[])
 *    Every QoS1/2 PUBLISH that has been transmitted is stored here until the
 *    final ACK arrives (PUBACK for QoS1; PUBCOMP for QoS2).  If MQTT_RETRY_
 *    INTERVAL_MS elapses without an ACK the packet is retransmitted with the
 *    DUP flag set, up to MQTT_MAX_RETRY_COUNT times.
 *
 *  Offline queue  (client->pending[])
 *    QoS1/2 publishes made while the client is disconnected are held here.
 *    On successful (re-)connection the queue is flushed: each entry is sent
 *    and moved to the in-flight table.
 *
 *  Auto-reconnect
 *    When a network error is detected the client transitions to DISCONNECTED
 *    and schedules a reconnect attempt using exponential back-off
 *    (MQTT_RECONNECT_BASE_MS … MQTT_RECONNECT_MAX_MS).  After reconnecting
 *    the client re-subscribes to all active subscriptions and flushes the
 *    offline queue.
 */

#include "tinyos/mqtt.h"
#include "tinyos/net.h"
#include <string.h>
#include <stdio.h>

#define SET_DEFAULT(f, v)  do { if ((f) == 0) (f) = (v); } while (0)

/* =========================================================================
 * Forward declarations
 * ========================================================================= */

static uint16_t     mqtt_encode_remaining_length(uint8_t *buf, uint32_t len);
static uint32_t     mqtt_decode_remaining_length(const uint8_t *buf, uint16_t *used);
static uint16_t     mqtt_encode_string(uint8_t *buf, const char *str);
static uint16_t     mqtt_next_message_id(mqtt_client_t *c);
static mqtt_error_t mqtt_send_packet(mqtt_client_t *c, const uint8_t *data, uint16_t len);
static mqtt_error_t mqtt_recv_packet(mqtt_client_t *c, uint8_t *type, uint32_t timeout_ms);
static mqtt_error_t mqtt_send_connect(mqtt_client_t *c);
static mqtt_error_t mqtt_send_disconnect(mqtt_client_t *c);
static mqtt_error_t mqtt_send_pingreq(mqtt_client_t *c);
static mqtt_error_t mqtt_send_publish(mqtt_client_t *c, const char *topic,
                                       const void *payload, uint16_t plen,
                                       mqtt_qos_t qos, bool retained,
                                       bool dup, uint16_t message_id);
static mqtt_error_t mqtt_send_puback(mqtt_client_t *c, uint16_t message_id);
static mqtt_error_t mqtt_send_pubrec(mqtt_client_t *c, uint16_t message_id);
static mqtt_error_t mqtt_send_pubrel(mqtt_client_t *c, uint16_t message_id);
static mqtt_error_t mqtt_send_pubcomp(mqtt_client_t *c, uint16_t message_id);
static mqtt_error_t mqtt_send_subscribe(mqtt_client_t *c, const char *topic, mqtt_qos_t qos);
static mqtt_error_t mqtt_send_unsubscribe(mqtt_client_t *c, const char *topic);

static mqtt_error_t mqtt_handle_connack(mqtt_client_t *c);
static mqtt_error_t mqtt_handle_publish(mqtt_client_t *c);
static mqtt_error_t mqtt_handle_puback(mqtt_client_t *c);
static mqtt_error_t mqtt_handle_pubrec(mqtt_client_t *c);
static mqtt_error_t mqtt_handle_pubrel(mqtt_client_t *c);
static mqtt_error_t mqtt_handle_pubcomp(mqtt_client_t *c);
static mqtt_error_t mqtt_handle_suback(mqtt_client_t *c);

static mqtt_inflight_t *inflight_find(mqtt_client_t *c, uint16_t id);
static void             inflight_add(mqtt_client_t *c, uint16_t id,
                                     const char *topic, const void *payload,
                                     uint16_t plen, mqtt_qos_t qos,
                                     bool retained, mqtt_inflight_state_t state);
static void             inflight_remove(mqtt_client_t *c, uint16_t id);
static void             inflight_check_timeouts(mqtt_client_t *c, uint32_t now);

static mqtt_error_t pending_enqueue(mqtt_client_t *c, const char *topic,
                                     const void *payload, uint16_t plen,
                                     mqtt_qos_t qos, bool retained);
static void         pending_flush(mqtt_client_t *c);

static mqtt_error_t do_connect(mqtt_client_t *c);
static void         do_disconnect(mqtt_client_t *c, bool notify);
static void         do_reconnect(mqtt_client_t *c);
static void         resubscribe_all(mqtt_client_t *c);

static bool         topic_matches(const char *sub, const char *topic);
static void         mqtt_task_fn(void *param);

static uint32_t mqtt_now_ms(void) { return os_get_uptime_ms(); }

/* =========================================================================
 * Packet codec helpers
 * ========================================================================= */

static uint16_t mqtt_encode_remaining_length(uint8_t *buf, uint32_t len) {
    uint16_t pos = 0;
    do {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;
        if (len > 0) b |= 0x80;
        buf[pos++] = b;
    } while (len > 0);
    return pos;
}

static uint32_t mqtt_decode_remaining_length(const uint8_t *buf, uint16_t *used) {
    uint32_t mult = 1, val = 0;
    uint16_t pos  = 0;
    uint8_t  b;
    do {
        b    = buf[pos++];
        val += (b & 0x7F) * mult;
        mult *= 128;
    } while ((b & 0x80) && pos < 4);
    *used = pos;
    return val;
}

static uint16_t mqtt_encode_string(uint8_t *buf, const char *str) {
    uint16_t len = str ? (uint16_t)strlen(str) : 0;
    buf[0] = (uint8_t)((len >> 8) & 0xFF);
    buf[1] = (uint8_t)(len & 0xFF);
    if (len) memcpy(&buf[2], str, len);
    return len + 2;
}

static uint16_t mqtt_next_message_id(mqtt_client_t *c) {
    if (++c->next_message_id == 0) c->next_message_id = 1;
    return c->next_message_id;
}

/* =========================================================================
 * Low-level send / receive
 * ========================================================================= */

static mqtt_error_t mqtt_send_packet(mqtt_client_t *c, const uint8_t *data, uint16_t len) {
    if (c->state != MQTT_STATE_CONNECTED && c->state != MQTT_STATE_CONNECTING)
        return MQTT_ERROR_NOT_CONNECTED;
    int32_t sent = net_send(c->socket, data, len, c->config.timeout_ms);
    if (sent != len) return MQTT_ERROR_NETWORK;
    c->last_activity_ms = mqtt_now_ms();
    return MQTT_OK;
}

static mqtt_error_t mqtt_recv_packet(mqtt_client_t *c, uint8_t *msg_type,
                                      uint32_t timeout_ms) {
    uint8_t hdr[5];
    int32_t n = net_recv(c->socket, hdr, sizeof(hdr), timeout_ms);
    if (n == 0) return MQTT_ERROR_TIMEOUT;
    if (n  < 0) return MQTT_ERROR_NETWORK;
    if (n  < 2) return MQTT_ERROR_NETWORK;

    *msg_type = (hdr[0] >> 4) & 0x0F;
    /* Store lower nibble of first byte for callers that need flags */
    c->rx_buffer[MQTT_MAX_PACKET_SIZE - 1] = hdr[0] & 0x0F;

    uint16_t  used;
    uint32_t  rlen   = mqtt_decode_remaining_length(&hdr[1], &used);
    uint16_t  hsize  = 1 + used;
    uint16_t  extra  = (uint16_t)n - hsize;

    if (extra > 0) memcpy(c->rx_buffer, &hdr[hsize], extra);

    if (rlen > 0) {
        if (rlen > MQTT_MAX_PACKET_SIZE - 1) return MQTT_ERROR_BUFFER_OVERFLOW;
        uint32_t total = extra;
        while (total < rlen) {
            n = net_recv(c->socket, &c->rx_buffer[total],
                         (uint16_t)(rlen - total), timeout_ms);
            if (n <= 0) return MQTT_ERROR_NETWORK;
            total += (uint32_t)n;
        }
        c->rx_buffer_pos = (uint16_t)rlen;
    } else {
        c->rx_buffer_pos = 0;
    }

    c->last_activity_ms = mqtt_now_ms();
    return MQTT_OK;
}

/* =========================================================================
 * Packet builders
 * ========================================================================= */

static mqtt_error_t mqtt_send_connect(mqtt_client_t *c) {
    uint16_t pos = 0;
    uint8_t *buf = c->tx_buffer;

    buf[pos++] = (uint8_t)(MQTT_MSG_TYPE_CONNECT << 4);

    uint16_t pl_start = pos + 4;
    uint16_t pl_pos   = pl_start;

    pl_pos += mqtt_encode_string(&buf[pl_pos], "MQTT");
    buf[pl_pos++] = MQTT_PROTOCOL_VERSION_3_1_1;

    uint8_t flags = 0;
    if (c->config.clean_session) flags |= 0x02;
    if (c->config.will_topic) {
        flags |= 0x04;
        flags |= (uint8_t)((c->config.will_qos & 0x03) << 3);
        if (c->config.will_retained) flags |= 0x20;
    }
    if (c->config.username) flags |= 0x80;
    if (c->config.password) flags |= 0x40;
    buf[pl_pos++] = flags;

    uint16_t ka = c->config.keepalive_sec;
    buf[pl_pos++] = (uint8_t)((ka >> 8) & 0xFF);
    buf[pl_pos++] = (uint8_t)(ka & 0xFF);

    pl_pos += mqtt_encode_string(&buf[pl_pos], c->config.client_id);
    if (c->config.will_topic) {
        pl_pos += mqtt_encode_string(&buf[pl_pos], c->config.will_topic);
        buf[pl_pos++] = (uint8_t)((c->config.will_message_len >> 8) & 0xFF);
        buf[pl_pos++] = (uint8_t)(c->config.will_message_len & 0xFF);
        if (c->config.will_message_len > 0) {
            memcpy(&buf[pl_pos], c->config.will_message, c->config.will_message_len);
            pl_pos += c->config.will_message_len;
        }
    }
    if (c->config.username) pl_pos += mqtt_encode_string(&buf[pl_pos], c->config.username);
    if (c->config.password) pl_pos += mqtt_encode_string(&buf[pl_pos], c->config.password);

    uint32_t rlen     = pl_pos - pl_start;
    uint16_t lb       = mqtt_encode_remaining_length(&buf[pos], rlen);
    if (lb != 4) {
        memmove(&buf[pos + lb], &buf[pl_start], rlen);
        pl_pos = pos + lb + (uint16_t)rlen;
    }
    return mqtt_send_packet(c, buf, pl_pos);
}

static mqtt_error_t mqtt_send_disconnect(mqtt_client_t *c) {
    uint8_t pkt[2] = { (uint8_t)(MQTT_MSG_TYPE_DISCONNECT << 4), 0 };
    return mqtt_send_packet(c, pkt, 2);
}

static mqtt_error_t mqtt_send_pingreq(mqtt_client_t *c) {
    uint8_t pkt[2] = { (uint8_t)(MQTT_MSG_TYPE_PINGREQ << 4), 0 };
    return mqtt_send_packet(c, pkt, 2);
}

static mqtt_error_t mqtt_send_publish(mqtt_client_t *c, const char *topic,
                                       const void *payload, uint16_t plen,
                                       mqtt_qos_t qos, bool retained,
                                       bool dup, uint16_t message_id) {
    uint16_t pos = 0;
    uint8_t *buf = c->tx_buffer;

    uint8_t flags = 0;
    if (retained) flags |= 0x01;
    flags |= (uint8_t)((qos & 0x03) << 1);
    if (dup)      flags |= 0x08;
    buf[pos++] = (uint8_t)((MQTT_MSG_TYPE_PUBLISH << 4) | flags);

    uint16_t  tlen = (uint16_t)strlen(topic);
    uint32_t  rlen = (uint32_t)(2 + tlen + plen);
    if (qos > MQTT_QOS_0) rlen += 2;
    pos += mqtt_encode_remaining_length(&buf[pos], rlen);

    pos += mqtt_encode_string(&buf[pos], topic);
    if (qos > MQTT_QOS_0) {
        buf[pos++] = (uint8_t)((message_id >> 8) & 0xFF);
        buf[pos++] = (uint8_t)(message_id & 0xFF);
    }
    if (plen > 0) { memcpy(&buf[pos], payload, plen); pos += plen; }

    return mqtt_send_packet(c, buf, pos);
}

/* Two-byte ACK packet (PUBACK / PUBREC / PUBREL / PUBCOMP) */
static mqtt_error_t mqtt_send_ack2(mqtt_client_t *c, uint8_t type, uint16_t id,
                                    uint8_t extra_flags) {
    uint8_t pkt[4] = {
        (uint8_t)((type << 4) | extra_flags),
        2,
        (uint8_t)((id >> 8) & 0xFF),
        (uint8_t)(id & 0xFF)
    };
    return mqtt_send_packet(c, pkt, 4);
}

static mqtt_error_t mqtt_send_puback(mqtt_client_t *c, uint16_t id) {
    return mqtt_send_ack2(c, MQTT_MSG_TYPE_PUBACK, id, 0);
}
static mqtt_error_t mqtt_send_pubrec(mqtt_client_t *c, uint16_t id) {
    return mqtt_send_ack2(c, MQTT_MSG_TYPE_PUBREC, id, 0);
}
static mqtt_error_t mqtt_send_pubrel(mqtt_client_t *c, uint16_t id) {
    /* PUBREL has fixed flags = 0x02 per MQTT spec */
    return mqtt_send_ack2(c, MQTT_MSG_TYPE_PUBREL, id, 0x02);
}
static mqtt_error_t mqtt_send_pubcomp(mqtt_client_t *c, uint16_t id) {
    return mqtt_send_ack2(c, MQTT_MSG_TYPE_PUBCOMP, id, 0);
}

static mqtt_error_t mqtt_send_subscribe(mqtt_client_t *c, const char *topic,
                                         mqtt_qos_t qos) {
    uint16_t pos = 0;
    uint8_t *buf = c->tx_buffer;

    buf[pos++] = (uint8_t)((MQTT_MSG_TYPE_SUBSCRIBE << 4) | 0x02);
    uint16_t tlen = (uint16_t)strlen(topic);
    uint32_t rlen = 2 + 2 + tlen + 1;
    pos += mqtt_encode_remaining_length(&buf[pos], rlen);

    uint16_t mid = mqtt_next_message_id(c);
    buf[pos++] = (uint8_t)((mid >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(mid & 0xFF);
    pos += mqtt_encode_string(&buf[pos], topic);
    buf[pos++] = (uint8_t)qos;

    return mqtt_send_packet(c, buf, pos);
}

static mqtt_error_t mqtt_send_unsubscribe(mqtt_client_t *c, const char *topic) {
    uint16_t pos = 0;
    uint8_t *buf = c->tx_buffer;

    buf[pos++] = (uint8_t)((MQTT_MSG_TYPE_UNSUBSCRIBE << 4) | 0x02);
    uint16_t tlen = (uint16_t)strlen(topic);
    uint32_t rlen = 2 + 2 + tlen;
    pos += mqtt_encode_remaining_length(&buf[pos], rlen);

    uint16_t mid = mqtt_next_message_id(c);
    buf[pos++] = (uint8_t)((mid >> 8) & 0xFF);
    buf[pos++] = (uint8_t)(mid & 0xFF);
    pos += mqtt_encode_string(&buf[pos], topic);

    return mqtt_send_packet(c, buf, pos);
}

/* =========================================================================
 * Incoming packet handlers
 * ========================================================================= */

static mqtt_error_t mqtt_handle_connack(mqtt_client_t *c) {
    if (c->rx_buffer_pos < 2) return MQTT_ERROR_PROTOCOL;
    if (c->rx_buffer[1] != MQTT_CONNACK_ACCEPTED) return MQTT_ERROR_BROKER_REFUSED;
    c->state = MQTT_STATE_CONNECTED;
    if (c->connection_callback)
        c->connection_callback(c, true, c->connection_callback_data);
    return MQTT_OK;
}

static mqtt_error_t mqtt_handle_publish(mqtt_client_t *c) {
    uint16_t pos = 0;

    /* Fixed-header lower nibble stored at rx_buffer[MAX-1] by recv */
    uint8_t  flags    = c->rx_buffer[MQTT_MAX_PACKET_SIZE - 1];
    mqtt_qos_t qos    = (mqtt_qos_t)((flags >> 1) & 0x03);
    bool retained     = (flags & 0x01) != 0;

    if (pos + 2 > c->rx_buffer_pos) return MQTT_ERROR_PROTOCOL;
    uint16_t tlen = (uint16_t)((c->rx_buffer[pos] << 8) | c->rx_buffer[pos + 1]);
    pos += 2;

    if (tlen >= MQTT_MAX_TOPIC_LENGTH || pos + tlen > c->rx_buffer_pos)
        return MQTT_ERROR_PROTOCOL;

    char topic[MQTT_MAX_TOPIC_LENGTH];
    memcpy(topic, &c->rx_buffer[pos], tlen);
    topic[tlen] = '\0';
    pos += tlen;

    uint16_t message_id = 0;
    if (qos > MQTT_QOS_0) {
        if (pos + 2 > c->rx_buffer_pos) return MQTT_ERROR_PROTOCOL;
        message_id = (uint16_t)((c->rx_buffer[pos] << 8) | c->rx_buffer[pos + 1]);
        pos += 2;
    }

    uint16_t       plen    = c->rx_buffer_pos - pos;
    const uint8_t *payload = &c->rx_buffer[pos];

    /* Deliver to application */
    if (c->message_callback) {
        mqtt_message_t msg = {
            .topic          = topic,
            .payload        = payload,
            .payload_length = plen,
            .qos            = qos,
            .retained       = retained,
            .message_id     = message_id
        };
        c->message_callback(c, &msg, c->message_callback_data);
    }

    /* QoS handshake for incoming messages */
    if (qos == MQTT_QOS_1) {
        mqtt_send_puback(c, message_id);
    } else if (qos == MQTT_QOS_2) {
        mqtt_send_pubrec(c, message_id);
        /* Full dedup tracking omitted; PUBCOMP sent on PUBREL receipt */
    }

    return MQTT_OK;
}

static mqtt_error_t mqtt_handle_puback(mqtt_client_t *c) {
    if (c->rx_buffer_pos < 2) return MQTT_ERROR_PROTOCOL;
    uint16_t id = (uint16_t)((c->rx_buffer[0] << 8) | c->rx_buffer[1]);
    inflight_remove(c, id);
    return MQTT_OK;
}

static mqtt_error_t mqtt_handle_pubrec(mqtt_client_t *c) {
    if (c->rx_buffer_pos < 2) return MQTT_ERROR_PROTOCOL;
    uint16_t id = (uint16_t)((c->rx_buffer[0] << 8) | c->rx_buffer[1]);

    mqtt_inflight_t *msg = inflight_find(c, id);
    if (msg && msg->state == MQTT_INFLIGHT_WAIT_PUBREC) {
        /* Transition: send PUBREL, wait for PUBCOMP */
        msg->state        = MQTT_INFLIGHT_WAIT_PUBCOMP;
        msg->sent_time_ms = mqtt_now_ms();
        msg->retry_count  = 0;
    }
    mqtt_send_pubrel(c, id);
    return MQTT_OK;
}

static mqtt_error_t mqtt_handle_pubrel(mqtt_client_t *c) {
    /* Broker releasing a QoS2 message it sent to us → reply with PUBCOMP */
    if (c->rx_buffer_pos < 2) return MQTT_ERROR_PROTOCOL;
    uint16_t id = (uint16_t)((c->rx_buffer[0] << 8) | c->rx_buffer[1]);
    mqtt_send_pubcomp(c, id);
    return MQTT_OK;
}

static mqtt_error_t mqtt_handle_pubcomp(mqtt_client_t *c) {
    if (c->rx_buffer_pos < 2) return MQTT_ERROR_PROTOCOL;
    uint16_t id = (uint16_t)((c->rx_buffer[0] << 8) | c->rx_buffer[1]);
    inflight_remove(c, id);
    return MQTT_OK;
}

static mqtt_error_t mqtt_handle_suback(mqtt_client_t *c) {
    if (c->rx_buffer_pos < 3) return MQTT_ERROR_PROTOCOL;
    return (c->rx_buffer[2] == 0x80) ? MQTT_ERROR_SUBSCRIBE_FAILED : MQTT_OK;
}

/* =========================================================================
 * In-flight table
 * ========================================================================= */

static mqtt_inflight_t *inflight_find(mqtt_client_t *c, uint16_t id) {
    for (int i = 0; i < MQTT_MAX_INFLIGHT; i++)
        if (c->inflight[i].active && c->inflight[i].message_id == id)
            return &c->inflight[i];
    return NULL;
}

static void inflight_add(mqtt_client_t *c, uint16_t id,
                          const char *topic, const void *payload, uint16_t plen,
                          mqtt_qos_t qos, bool retained,
                          mqtt_inflight_state_t state) {
    for (int i = 0; i < MQTT_MAX_INFLIGHT; i++) {
        if (!c->inflight[i].active) {
            mqtt_inflight_t *m = &c->inflight[i];
            m->active      = true;
            m->message_id  = id;
            m->qos         = qos;
            m->retained    = retained;
            m->state       = state;
            m->sent_time_ms= mqtt_now_ms();
            m->retry_count = 0;
            strncpy(m->topic, topic, MQTT_MAX_TOPIC_LENGTH - 1);
            m->topic[MQTT_MAX_TOPIC_LENGTH - 1] = '\0';
            uint16_t copy  = (plen > MQTT_MAX_PAYLOAD_SIZE)
                             ? MQTT_MAX_PAYLOAD_SIZE : plen;
            memcpy(m->payload, payload, copy);
            m->payload_len = copy;
            c->inflight_count++;
            return;
        }
    }
    /* Table full: silently drop (caller should check inflight_count first) */
}

static void inflight_remove(mqtt_client_t *c, uint16_t id) {
    for (int i = 0; i < MQTT_MAX_INFLIGHT; i++) {
        if (c->inflight[i].active && c->inflight[i].message_id == id) {
            c->inflight[i].active = false;
            if (c->inflight_count > 0) c->inflight_count--;
            return;
        }
    }
}

/**
 * Scan the in-flight table; retransmit timed-out entries (with DUP flag)
 * or discard after MQTT_MAX_RETRY_COUNT attempts.
 * Called from mqtt_loop() — client must be CONNECTED.
 */
static void inflight_check_timeouts(mqtt_client_t *c, uint32_t now) {
    for (int i = 0; i < MQTT_MAX_INFLIGHT; i++) {
        mqtt_inflight_t *m = &c->inflight[i];
        if (!m->active) continue;
        if ((now - m->sent_time_ms) < MQTT_RETRY_INTERVAL_MS) continue;

        if (m->retry_count >= MQTT_MAX_RETRY_COUNT) {
            /* Give up */
            m->active = false;
            if (c->inflight_count > 0) c->inflight_count--;
            continue;
        }

        m->retry_count++;
        m->sent_time_ms = now;

        if (m->state == MQTT_INFLIGHT_WAIT_PUBACK ||
            m->state == MQTT_INFLIGHT_WAIT_PUBREC) {
            /* Resend PUBLISH with DUP=1 */
            mqtt_send_publish(c, m->topic, m->payload, m->payload_len,
                              m->qos, m->retained, true, m->message_id);
        } else if (m->state == MQTT_INFLIGHT_WAIT_PUBCOMP) {
            /* Resend PUBREL */
            mqtt_send_pubrel(c, m->message_id);
        }
    }
}

/* =========================================================================
 * Offline pending queue
 * ========================================================================= */

static mqtt_error_t pending_enqueue(mqtt_client_t *c, const char *topic,
                                     const void *payload, uint16_t plen,
                                     mqtt_qos_t qos, bool retained) {
    if (c->pending_count >= MQTT_MAX_PENDING) return MQTT_ERROR_QUEUE_FULL;

    for (int i = 0; i < MQTT_MAX_PENDING; i++) {
        if (!c->pending[i].active) {
            mqtt_pending_t *p = &c->pending[i];
            p->active   = true;
            p->qos      = qos;
            p->retained = retained;
            strncpy(p->topic, topic, MQTT_MAX_TOPIC_LENGTH - 1);
            p->topic[MQTT_MAX_TOPIC_LENGTH - 1] = '\0';
            uint16_t copy = (plen > MQTT_MAX_PAYLOAD_SIZE)
                            ? MQTT_MAX_PAYLOAD_SIZE : plen;
            memcpy(p->payload, payload, copy);
            p->payload_len = copy;
            c->pending_count++;
            return MQTT_OK;
        }
    }
    return MQTT_ERROR_QUEUE_FULL;  /* shouldn't reach here */
}

/**
 * Send every pending message and move it to the in-flight table.
 * Must be called while connected and holding the mutex.
 */
static void pending_flush(mqtt_client_t *c) {
    for (int i = 0; i < MQTT_MAX_PENDING; i++) {
        mqtt_pending_t *p = &c->pending[i];
        if (!p->active) continue;
        if (c->state != MQTT_STATE_CONNECTED) break;

        uint16_t mid = mqtt_next_message_id(c);
        mqtt_error_t err = mqtt_send_publish(c, p->topic, p->payload, p->payload_len,
                                              p->qos, p->retained, false, mid);
        if (err == MQTT_OK) {
            mqtt_inflight_state_t st = (p->qos == MQTT_QOS_1)
                                       ? MQTT_INFLIGHT_WAIT_PUBACK
                                       : MQTT_INFLIGHT_WAIT_PUBREC;
            inflight_add(c, mid, p->topic, p->payload, p->payload_len,
                         p->qos, p->retained, st);
            p->active = false;
            if (c->pending_count > 0) c->pending_count--;
        }
        /* If send fails (unlikely right after connect), leave in queue */
    }
}

/* =========================================================================
 * Re-subscribe all active subscriptions after (re)connection
 * ========================================================================= */

static void resubscribe_all(mqtt_client_t *c) {
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (c->subscriptions[i].active)
            mqtt_send_subscribe(c, c->subscriptions[i].topic,
                                c->subscriptions[i].qos);
    }
}

/* =========================================================================
 * Connection helpers
 * ========================================================================= */

/**
 * Perform TCP connect + MQTT handshake.
 * Does NOT create the background task; does NOT lock the mutex.
 * On failure the socket is closed and state is set to DISCONNECTED.
 */
static mqtt_error_t do_connect(mqtt_client_t *c) {
    c->state  = MQTT_STATE_CONNECTING;
    c->socket = net_socket(SOCK_STREAM);
    if (c->socket < 0) goto fail_no_socket;

    ipv4_addr_t ip;
    if (net_dns_resolve(c->config.broker_host, &ip, c->config.timeout_ms) != OS_OK)
        goto fail;

    sockaddr_in_t addr = { .addr = ip, .port = c->config.broker_port };
    if (net_connect(c->socket, &addr, c->config.timeout_ms) != OS_OK)
        goto fail;

    if (mqtt_send_connect(c) != MQTT_OK) goto fail;

    uint8_t      mt;
    mqtt_error_t err = mqtt_recv_packet(c, &mt, c->config.timeout_ms);
    if (err != MQTT_OK || mt != MQTT_MSG_TYPE_CONNACK) goto fail;

    err = mqtt_handle_connack(c);
    if (err != MQTT_OK) goto fail;

    c->last_activity_ms = mqtt_now_ms();
    c->last_ping_ms     = mqtt_now_ms();
    return MQTT_OK;

fail:
    net_close(c->socket);
fail_no_socket:
    c->state = MQTT_STATE_DISCONNECTED;
    return MQTT_ERROR_NETWORK;
}

/**
 * Mark the client as disconnected and optionally fire the connection callback.
 * Must be called with the mutex held.
 */
static void do_disconnect(mqtt_client_t *c, bool notify) {
    net_close(c->socket);
    c->state = MQTT_STATE_DISCONNECTED;

    /* Schedule first reconnect attempt */
    if (c->config.auto_reconnect) {
        uint32_t base = c->config.reconnect_interval_ms;
        if (base == 0) base = MQTT_RECONNECT_BASE_MS;
        c->next_reconnect_ms  = mqtt_now_ms() + base;
        c->reconnect_attempt  = 0;
    }

    if (notify && c->connection_callback)
        c->connection_callback(c, false, c->connection_callback_data);
}

/**
 * Attempt one reconnect.  Called from mqtt_task (without mutex held).
 * Schedules the next attempt on failure using exponential back-off.
 */
static void do_reconnect(mqtt_client_t *c) {
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);

    c->state = MQTT_STATE_RECONNECTING;
    mqtt_error_t err = do_connect(c);   /* sets state = CONNECTED or DISCONNECTED */

    if (err == MQTT_OK) {
        c->reconnect_attempt = 0;
        /* Restore in-flight messages: reset timestamps so retries fire quickly */
        for (int i = 0; i < MQTT_MAX_INFLIGHT; i++) {
            if (c->inflight[i].active) {
                c->inflight[i].sent_time_ms = 0;  /* force immediate retry */
                c->inflight[i].retry_count  = 0;
                /* QoS2 that reached WAIT_PUBCOMP: restart from WAIT_PUBREC */
                if (c->inflight[i].state == MQTT_INFLIGHT_WAIT_PUBCOMP)
                    c->inflight[i].state = MQTT_INFLIGHT_WAIT_PUBREC;
            }
        }
        resubscribe_all(c);
        pending_flush(c);
    } else {
        /* Exponential back-off capped at MQTT_RECONNECT_MAX_MS */
        c->reconnect_attempt++;
        uint32_t base  = c->config.reconnect_interval_ms;
        if (base == 0)  base = MQTT_RECONNECT_BASE_MS;
        uint32_t delay = base;
        for (uint8_t i = 0; i < c->reconnect_attempt && i < 10; i++) {
            delay *= 2;
            if (delay >= MQTT_RECONNECT_MAX_MS) { delay = MQTT_RECONNECT_MAX_MS; break; }
        }
        c->next_reconnect_ms = mqtt_now_ms() + delay;
    }

    os_mutex_unlock(&c->mutex);
}

/* =========================================================================
 * Topic wildcard matching
 * ========================================================================= */

__attribute__((unused)) static bool topic_matches(const char *sub, const char *topic) {
    const char *s = sub, *t = topic;
    while (*s && *t) {
        if (*s == '#') return true;
        if (*s == '+') {
            while (*t && *t != '/') t++;
            s++;
            if (*s == '/') s++;
        } else if (*s == *t) {
            s++; t++;
        } else {
            return false;
        }
    }
    return (*s == '\0' && *t == '\0');
}
/* =========================================================================
 * Background task
 * ========================================================================= */

static void mqtt_task_fn(void *param) {
    mqtt_client_t *c = (mqtt_client_t *)param;
    while (c->task_running) {
        mqtt_loop(c);
        os_task_delay(10);
    }
}

/* =========================================================================
 * Public API
 * ========================================================================= */

mqtt_error_t mqtt_client_init(mqtt_client_t *c, const mqtt_config_t *cfg) {
    if (!c || !cfg) return MQTT_ERROR_INVALID_PARAM;
    memset(c, 0, sizeof(mqtt_client_t));
    memcpy(&c->config, cfg, sizeof(mqtt_config_t));
    SET_DEFAULT(c->config.broker_port,   MQTT_DEFAULT_PORT);
    SET_DEFAULT(c->config.keepalive_sec, MQTT_DEFAULT_KEEPALIVE);
    SET_DEFAULT(c->config.timeout_ms,    MQTT_DEFAULT_TIMEOUT_MS);
    c->state           = MQTT_STATE_DISCONNECTED;
    c->next_message_id = 1;
    os_mutex_init(&c->mutex);
    return MQTT_OK;
}

mqtt_error_t mqtt_connect(mqtt_client_t *c) {
    if (!c) return MQTT_ERROR_INVALID_PARAM;
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);

    if (c->state != MQTT_STATE_DISCONNECTED) {
        os_mutex_unlock(&c->mutex);
        return MQTT_ERROR_ALREADY_CONNECTED;
    }

    mqtt_error_t err = do_connect(c);
    if (err == MQTT_OK) {
        resubscribe_all(c);
        pending_flush(c);
        c->task_running = true;
        os_task_create(&c->task, "mqtt", mqtt_task_fn, c, PRIORITY_NORMAL);
    }

    os_mutex_unlock(&c->mutex);
    return err;
}

mqtt_error_t mqtt_disconnect(mqtt_client_t *c) {
    if (!c) return MQTT_ERROR_INVALID_PARAM;
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);

    if (c->state != MQTT_STATE_CONNECTED) {
        os_mutex_unlock(&c->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    c->state        = MQTT_STATE_DISCONNECTING;
    c->task_running = false;
    mqtt_send_disconnect(c);
    do_disconnect(c, true);

    os_mutex_unlock(&c->mutex);
    return MQTT_OK;
}

mqtt_error_t mqtt_publish(mqtt_client_t *c, const char *topic,
                           const void *payload, uint16_t payload_length,
                           mqtt_qos_t qos, bool retained) {
    if (!c || !topic) return MQTT_ERROR_INVALID_PARAM;
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);

    if (c->state == MQTT_STATE_CONNECTED) {
        uint16_t mid = 0;
        if (qos > MQTT_QOS_0) {
            if (c->inflight_count >= MQTT_MAX_INFLIGHT) {
                os_mutex_unlock(&c->mutex);
                return MQTT_ERROR_INFLIGHT_FULL;
            }
            mid = mqtt_next_message_id(c);
        }

        mqtt_error_t err = mqtt_send_publish(c, topic, payload, payload_length,
                                              qos, retained, false, mid);
        if (err == MQTT_OK && qos > MQTT_QOS_0) {
            mqtt_inflight_state_t st = (qos == MQTT_QOS_1)
                                       ? MQTT_INFLIGHT_WAIT_PUBACK
                                       : MQTT_INFLIGHT_WAIT_PUBREC;
            inflight_add(c, mid, topic, payload, payload_length, qos, retained, st);
        }
        os_mutex_unlock(&c->mutex);
        return err;
    }

    /* Disconnected path */
    if (qos == MQTT_QOS_0) {
        /* Fire-and-forget: no point queuing */
        os_mutex_unlock(&c->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    /* QoS1/2: buffer for later delivery */
    mqtt_error_t err = pending_enqueue(c, topic, payload, payload_length,
                                        qos, retained);
    os_mutex_unlock(&c->mutex);
    return (err == MQTT_OK) ? MQTT_QUEUED : err;
}

mqtt_error_t mqtt_subscribe(mqtt_client_t *c, const char *topic, mqtt_qos_t qos) {
    if (!c || !topic) return MQTT_ERROR_INVALID_PARAM;
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);

    if (c->state != MQTT_STATE_CONNECTED) {
        os_mutex_unlock(&c->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    int slot = -1;
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (!c->subscriptions[i].active) { slot = i; break; }
    }
    if (slot == -1) {
        os_mutex_unlock(&c->mutex);
        return MQTT_ERROR_NO_MEMORY;
    }

    mqtt_error_t err = mqtt_send_subscribe(c, topic, qos);
    if (err == MQTT_OK) {
        strncpy(c->subscriptions[slot].topic, topic, MQTT_MAX_TOPIC_LENGTH - 1);
        c->subscriptions[slot].topic[MQTT_MAX_TOPIC_LENGTH - 1] = '\0';
        c->subscriptions[slot].qos    = qos;
        c->subscriptions[slot].active = true;
    }

    os_mutex_unlock(&c->mutex);
    return err;
}

mqtt_error_t mqtt_unsubscribe(mqtt_client_t *c, const char *topic) {
    if (!c || !topic) return MQTT_ERROR_INVALID_PARAM;
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);

    if (c->state != MQTT_STATE_CONNECTED) {
        os_mutex_unlock(&c->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (c->subscriptions[i].active &&
            strcmp(c->subscriptions[i].topic, topic) == 0) {
            c->subscriptions[i].active = false;
            break;
        }
    }

    mqtt_error_t err = mqtt_send_unsubscribe(c, topic);
    os_mutex_unlock(&c->mutex);
    return err;
}

void mqtt_set_message_callback(mqtt_client_t *c, mqtt_message_callback_t cb,
                                void *user_data) {
    if (c) { c->message_callback = cb; c->message_callback_data = user_data; }
}

void mqtt_set_connection_callback(mqtt_client_t *c, mqtt_connection_callback_t cb,
                                   void *user_data) {
    if (c) { c->connection_callback = cb; c->connection_callback_data = user_data; }
}

bool         mqtt_is_connected(const mqtt_client_t *c) {
    return c && c->state == MQTT_STATE_CONNECTED;
}
mqtt_state_t mqtt_get_state(const mqtt_client_t *c) {
    return c ? c->state : MQTT_STATE_DISCONNECTED;
}
uint8_t mqtt_get_pending_count(const mqtt_client_t *c) {
    return c ? c->pending_count : 0;
}
uint8_t mqtt_get_inflight_count(const mqtt_client_t *c) {
    return c ? c->inflight_count : 0;
}
void mqtt_flush_pending(mqtt_client_t *c) {
    if (!c) return;
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);
    for (int i = 0; i < MQTT_MAX_PENDING; i++) c->pending[i].active = false;
    c->pending_count = 0;
    os_mutex_unlock(&c->mutex);
}

/* =========================================================================
 * Main loop  (called every 10 ms from mqtt_task_fn)
 * ========================================================================= */

mqtt_error_t mqtt_loop(mqtt_client_t *c) {
    if (!c) return MQTT_ERROR_INVALID_PARAM;

    uint32_t now = mqtt_now_ms();

    /* ── Disconnected: drive auto-reconnect ── */
    if (c->state == MQTT_STATE_DISCONNECTED) {
        if (c->config.auto_reconnect &&
            c->next_reconnect_ms != 0 &&
            now >= c->next_reconnect_ms) {
            do_reconnect(c);
        }
        return MQTT_OK;
    }

    if (c->state != MQTT_STATE_CONNECTED) return MQTT_OK;

    /* ── Keepalive ── */
    uint32_t ka_ms = (uint32_t)c->config.keepalive_sec * 1000;
    if (ka_ms > 0 && (now - c->last_ping_ms) >= ka_ms) {
        os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);
        mqtt_send_pingreq(c);
        c->last_ping_ms = now;
        os_mutex_unlock(&c->mutex);
    }

    /* ── Retry timed-out in-flight messages ── */
    os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);
    inflight_check_timeouts(c, now);
    os_mutex_unlock(&c->mutex);

    /* ── Receive incoming packet (short timeout = non-blocking feel) ── */
    uint8_t      mt;
    mqtt_error_t err = mqtt_recv_packet(c, &mt, 100);

    if (err == MQTT_ERROR_TIMEOUT) return MQTT_OK;

    if (err != MQTT_OK) {
        /* Network error → treat as disconnect */
        os_mutex_lock(&c->mutex, OS_WAIT_FOREVER);
        do_disconnect(c, true);
        os_mutex_unlock(&c->mutex);
        return MQTT_ERROR_NETWORK;
    }

    /* ── Dispatch ── */
    switch (mt) {
        case MQTT_MSG_TYPE_PUBLISH:  return mqtt_handle_publish(c);
        case MQTT_MSG_TYPE_PUBACK:   return mqtt_handle_puback(c);
        case MQTT_MSG_TYPE_PUBREC:   return mqtt_handle_pubrec(c);
        case MQTT_MSG_TYPE_PUBREL:   return mqtt_handle_pubrel(c);
        case MQTT_MSG_TYPE_PUBCOMP:  return mqtt_handle_pubcomp(c);
        case MQTT_MSG_TYPE_SUBACK:   return mqtt_handle_suback(c);
        case MQTT_MSG_TYPE_PINGRESP: return MQTT_OK;
        default:                     return MQTT_OK;
    }
}

/* =========================================================================
 * Diagnostics
 * ========================================================================= */

const char *mqtt_error_to_string(mqtt_error_t e) {
    switch (e) {
        case MQTT_OK:                    return "OK";
        case MQTT_QUEUED:                return "Queued";
        case MQTT_ERROR_INVALID_PARAM:   return "Invalid parameter";
        case MQTT_ERROR_NOT_CONNECTED:   return "Not connected";
        case MQTT_ERROR_ALREADY_CONNECTED: return "Already connected";
        case MQTT_ERROR_NETWORK:         return "Network error";
        case MQTT_ERROR_TIMEOUT:         return "Timeout";
        case MQTT_ERROR_PROTOCOL:        return "Protocol error";
        case MQTT_ERROR_BUFFER_OVERFLOW: return "Buffer overflow";
        case MQTT_ERROR_BROKER_REFUSED:  return "Broker refused";
        case MQTT_ERROR_SUBSCRIBE_FAILED:return "Subscribe failed";
        case MQTT_ERROR_PUBLISH_FAILED:  return "Publish failed";
        case MQTT_ERROR_NO_MEMORY:       return "No memory";
        case MQTT_ERROR_QUEUE_FULL:      return "Offline queue full";
        case MQTT_ERROR_INFLIGHT_FULL:   return "In-flight table full";
        default:                         return "Unknown error";
    }
}

const char *mqtt_state_to_string(mqtt_state_t s) {
    switch (s) {
        case MQTT_STATE_DISCONNECTED:  return "Disconnected";
        case MQTT_STATE_CONNECTING:    return "Connecting";
        case MQTT_STATE_CONNECTED:     return "Connected";
        case MQTT_STATE_RECONNECTING:  return "Reconnecting";
        case MQTT_STATE_DISCONNECTING: return "Disconnecting";
        default:                       return "Unknown";
    }
}
