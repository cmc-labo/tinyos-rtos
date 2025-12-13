/**
 * @file mqtt.c
 * @brief MQTT Client Implementation for TinyOS-RTOS
 */

#include "tinyos/mqtt.h"
#include "tinyos/net.h"
#include <string.h>
#include <stdio.h>

/* Internal helper functions */
static uint16_t mqtt_encode_remaining_length(uint8_t *buffer, uint32_t length);
static uint32_t mqtt_decode_remaining_length(const uint8_t *buffer, uint16_t *bytes_used);
static uint16_t mqtt_encode_string(uint8_t *buffer, const char *str);
static uint16_t mqtt_next_message_id(mqtt_client_t *client);
static mqtt_error_t mqtt_send_packet(mqtt_client_t *client, const uint8_t *data, uint16_t length);
static mqtt_error_t mqtt_receive_packet(mqtt_client_t *client, uint8_t *msg_type, uint32_t timeout_ms);
static mqtt_error_t mqtt_send_connect(mqtt_client_t *client);
static mqtt_error_t mqtt_send_disconnect(mqtt_client_t *client);
static mqtt_error_t mqtt_send_pingreq(mqtt_client_t *client);
static mqtt_error_t mqtt_send_subscribe(mqtt_client_t *client, const char *topic, mqtt_qos_t qos);
static mqtt_error_t mqtt_send_unsubscribe(mqtt_client_t *client, const char *topic);
static mqtt_error_t mqtt_send_publish(mqtt_client_t *client, const char *topic,
                                       const void *payload, uint16_t payload_len,
                                       mqtt_qos_t qos, bool retained, uint16_t message_id);
static mqtt_error_t mqtt_handle_connack(mqtt_client_t *client);
static mqtt_error_t mqtt_handle_publish(mqtt_client_t *client);
static mqtt_error_t mqtt_handle_puback(mqtt_client_t *client);
static mqtt_error_t mqtt_handle_suback(mqtt_client_t *client);
static void mqtt_task(void *param);
static bool mqtt_topic_matches(const char *subscription, const char *topic);

/**
 * @brief Get system time in milliseconds
 */
static uint32_t mqtt_get_time_ms(void) {
    extern uint32_t system_tick;
    return system_tick;
}

/**
 * @brief Encode remaining length field (variable length encoding)
 */
static uint16_t mqtt_encode_remaining_length(uint8_t *buffer, uint32_t length) {
    uint16_t pos = 0;
    do {
        uint8_t byte = length % 128;
        length /= 128;
        if (length > 0) {
            byte |= 0x80;
        }
        buffer[pos++] = byte;
    } while (length > 0);
    return pos;
}

/**
 * @brief Decode remaining length field
 */
static uint32_t mqtt_decode_remaining_length(const uint8_t *buffer, uint16_t *bytes_used) {
    uint32_t multiplier = 1;
    uint32_t value = 0;
    uint16_t pos = 0;
    uint8_t byte;

    do {
        byte = buffer[pos++];
        value += (byte & 0x7F) * multiplier;
        multiplier *= 128;
    } while ((byte & 0x80) != 0 && pos < 4);

    *bytes_used = pos;
    return value;
}

/**
 * @brief Encode UTF-8 string with length prefix
 */
static uint16_t mqtt_encode_string(uint8_t *buffer, const char *str) {
    uint16_t len = str ? strlen(str) : 0;
    buffer[0] = (len >> 8) & 0xFF;
    buffer[1] = len & 0xFF;
    if (len > 0) {
        memcpy(&buffer[2], str, len);
    }
    return len + 2;
}

/**
 * @brief Get next message ID
 */
static uint16_t mqtt_next_message_id(mqtt_client_t *client) {
    client->next_message_id++;
    if (client->next_message_id == 0) {
        client->next_message_id = 1;
    }
    return client->next_message_id;
}

/**
 * @brief Send packet to broker
 */
static mqtt_error_t mqtt_send_packet(mqtt_client_t *client, const uint8_t *data, uint16_t length) {
    if (client->state != MQTT_STATE_CONNECTED &&
        client->state != MQTT_STATE_CONNECTING) {
        return MQTT_ERROR_NOT_CONNECTED;
    }

    int32_t sent = net_send(client->socket, data, length, client->config.timeout_ms);
    if (sent != length) {
        return MQTT_ERROR_NETWORK;
    }

    client->last_activity_ms = mqtt_get_time_ms();
    return MQTT_OK;
}

/**
 * @brief Receive packet from broker
 */
static mqtt_error_t mqtt_receive_packet(mqtt_client_t *client, uint8_t *msg_type, uint32_t timeout_ms) {
    uint8_t header[5];
    int32_t received;

    /* Read fixed header (minimum 2 bytes) */
    received = net_recv(client->socket, header, 1, timeout_ms);
    if (received <= 0) {
        return (received == 0) ? MQTT_ERROR_TIMEOUT : MQTT_ERROR_NETWORK;
    }

    *msg_type = (header[0] >> 4) & 0x0F;

    /* Read remaining length */
    uint16_t len_bytes = 0;
    do {
        received = net_recv(client->socket, &header[1 + len_bytes], 1, timeout_ms);
        if (received <= 0) {
            return MQTT_ERROR_NETWORK;
        }
        len_bytes++;
    } while ((header[len_bytes] & 0x80) != 0 && len_bytes < 4);

    uint16_t bytes_used;
    uint32_t remaining_length = mqtt_decode_remaining_length(&header[1], &bytes_used);

    /* Read remaining packet */
    if (remaining_length > 0) {
        if (remaining_length > MQTT_MAX_PACKET_SIZE) {
            return MQTT_ERROR_BUFFER_OVERFLOW;
        }

        uint32_t total_received = 0;
        while (total_received < remaining_length) {
            received = net_recv(client->socket,
                               &client->rx_buffer[total_received],
                               remaining_length - total_received,
                               timeout_ms);
            if (received <= 0) {
                return MQTT_ERROR_NETWORK;
            }
            total_received += received;
        }
        client->rx_buffer_pos = remaining_length;
    }

    client->last_activity_ms = mqtt_get_time_ms();
    return MQTT_OK;
}

/**
 * @brief Send CONNECT packet
 */
static mqtt_error_t mqtt_send_connect(mqtt_client_t *client) {
    uint16_t pos = 0;
    uint8_t *buf = client->tx_buffer;

    /* Fixed header */
    buf[pos++] = (MQTT_MSG_TYPE_CONNECT << 4);

    /* Calculate remaining length */
    uint16_t payload_start = pos + 4;  /* Reserve space for remaining length */
    uint16_t payload_pos = payload_start;

    /* Variable header - Protocol Name */
    payload_pos += mqtt_encode_string(&buf[payload_pos], "MQTT");

    /* Protocol Level */
    buf[payload_pos++] = MQTT_PROTOCOL_VERSION_3_1_1;

    /* Connect Flags */
    uint8_t flags = 0;
    if (client->config.clean_session) flags |= 0x02;
    if (client->config.will_topic) {
        flags |= 0x04;  /* Will flag */
        flags |= (client->config.will_qos & 0x03) << 3;
        if (client->config.will_retained) flags |= 0x20;
    }
    if (client->config.username) flags |= 0x80;
    if (client->config.password) flags |= 0x40;
    buf[payload_pos++] = flags;

    /* Keep Alive */
    uint16_t keepalive = client->config.keepalive_sec;
    buf[payload_pos++] = (keepalive >> 8) & 0xFF;
    buf[payload_pos++] = keepalive & 0xFF;

    /* Payload - Client ID */
    payload_pos += mqtt_encode_string(&buf[payload_pos], client->config.client_id);

    /* Will Topic and Message */
    if (client->config.will_topic) {
        payload_pos += mqtt_encode_string(&buf[payload_pos], client->config.will_topic);
        buf[payload_pos++] = (client->config.will_message_len >> 8) & 0xFF;
        buf[payload_pos++] = client->config.will_message_len & 0xFF;
        if (client->config.will_message_len > 0) {
            memcpy(&buf[payload_pos], client->config.will_message, client->config.will_message_len);
            payload_pos += client->config.will_message_len;
        }
    }

    /* Username and Password */
    if (client->config.username) {
        payload_pos += mqtt_encode_string(&buf[payload_pos], client->config.username);
    }
    if (client->config.password) {
        payload_pos += mqtt_encode_string(&buf[payload_pos], client->config.password);
    }

    /* Encode remaining length */
    uint32_t remaining_length = payload_pos - payload_start;
    uint16_t len_bytes = mqtt_encode_remaining_length(&buf[pos], remaining_length);

    /* Shift payload if needed */
    if (len_bytes != 4) {
        memmove(&buf[pos + len_bytes], &buf[payload_start], remaining_length);
        payload_pos = pos + len_bytes + remaining_length;
    }

    return mqtt_send_packet(client, buf, payload_pos);
}

/**
 * @brief Send DISCONNECT packet
 */
static mqtt_error_t mqtt_send_disconnect(mqtt_client_t *client) {
    uint8_t packet[2] = {
        (MQTT_MSG_TYPE_DISCONNECT << 4),
        0  /* Remaining length = 0 */
    };
    return mqtt_send_packet(client, packet, sizeof(packet));
}

/**
 * @brief Send PINGREQ packet
 */
static mqtt_error_t mqtt_send_pingreq(mqtt_client_t *client) {
    uint8_t packet[2] = {
        (MQTT_MSG_TYPE_PINGREQ << 4),
        0  /* Remaining length = 0 */
    };
    return mqtt_send_packet(client, packet, sizeof(packet));
}

/**
 * @brief Send PUBLISH packet
 */
static mqtt_error_t mqtt_send_publish(mqtt_client_t *client, const char *topic,
                                       const void *payload, uint16_t payload_len,
                                       mqtt_qos_t qos, bool retained, uint16_t message_id) {
    uint16_t pos = 0;
    uint8_t *buf = client->tx_buffer;

    /* Fixed header */
    uint8_t flags = 0;
    if (retained) flags |= 0x01;
    flags |= (qos & 0x03) << 1;
    buf[pos++] = (MQTT_MSG_TYPE_PUBLISH << 4) | flags;

    /* Calculate remaining length */
    uint16_t topic_len = strlen(topic);
    uint32_t remaining_length = 2 + topic_len + payload_len;
    if (qos > MQTT_QOS_0) {
        remaining_length += 2;  /* Message ID */
    }

    pos += mqtt_encode_remaining_length(&buf[pos], remaining_length);

    /* Variable header - Topic */
    pos += mqtt_encode_string(&buf[pos], topic);

    /* Message ID (for QoS > 0) */
    if (qos > MQTT_QOS_0) {
        buf[pos++] = (message_id >> 8) & 0xFF;
        buf[pos++] = message_id & 0xFF;
    }

    /* Payload */
    if (payload_len > 0) {
        memcpy(&buf[pos], payload, payload_len);
        pos += payload_len;
    }

    return mqtt_send_packet(client, buf, pos);
}

/**
 * @brief Send SUBSCRIBE packet
 */
static mqtt_error_t mqtt_send_subscribe(mqtt_client_t *client, const char *topic, mqtt_qos_t qos) {
    uint16_t pos = 0;
    uint8_t *buf = client->tx_buffer;

    /* Fixed header */
    buf[pos++] = (MQTT_MSG_TYPE_SUBSCRIBE << 4) | 0x02;

    /* Calculate remaining length */
    uint16_t topic_len = strlen(topic);
    uint32_t remaining_length = 2 + 2 + topic_len + 1;  /* Message ID + Topic + QoS */

    pos += mqtt_encode_remaining_length(&buf[pos], remaining_length);

    /* Variable header - Message ID */
    uint16_t message_id = mqtt_next_message_id(client);
    buf[pos++] = (message_id >> 8) & 0xFF;
    buf[pos++] = message_id & 0xFF;

    /* Payload - Topic filter */
    pos += mqtt_encode_string(&buf[pos], topic);
    buf[pos++] = qos;

    return mqtt_send_packet(client, buf, pos);
}

/**
 * @brief Send UNSUBSCRIBE packet
 */
static mqtt_error_t mqtt_send_unsubscribe(mqtt_client_t *client, const char *topic) {
    uint16_t pos = 0;
    uint8_t *buf = client->tx_buffer;

    /* Fixed header */
    buf[pos++] = (MQTT_MSG_TYPE_UNSUBSCRIBE << 4) | 0x02;

    /* Calculate remaining length */
    uint16_t topic_len = strlen(topic);
    uint32_t remaining_length = 2 + 2 + topic_len;  /* Message ID + Topic */

    pos += mqtt_encode_remaining_length(&buf[pos], remaining_length);

    /* Variable header - Message ID */
    uint16_t message_id = mqtt_next_message_id(client);
    buf[pos++] = (message_id >> 8) & 0xFF;
    buf[pos++] = message_id & 0xFF;

    /* Payload - Topic filter */
    pos += mqtt_encode_string(&buf[pos], topic);

    return mqtt_send_packet(client, buf, pos);
}

/**
 * @brief Handle CONNACK packet
 */
static mqtt_error_t mqtt_handle_connack(mqtt_client_t *client) {
    if (client->rx_buffer_pos < 2) {
        return MQTT_ERROR_PROTOCOL;
    }

    uint8_t return_code = client->rx_buffer[1];

    if (return_code == MQTT_CONNACK_ACCEPTED) {
        client->state = MQTT_STATE_CONNECTED;
        if (client->connection_callback) {
            client->connection_callback(client, true, client->connection_callback_data);
        }
        return MQTT_OK;
    }

    return MQTT_ERROR_BROKER_REFUSED;
}

/**
 * @brief Handle PUBLISH packet
 */
static mqtt_error_t mqtt_handle_publish(mqtt_client_t *client) {
    uint16_t pos = 0;

    /* Topic name */
    uint16_t topic_len = (client->rx_buffer[pos] << 8) | client->rx_buffer[pos + 1];
    pos += 2;

    if (topic_len >= MQTT_MAX_TOPIC_LENGTH || pos + topic_len > client->rx_buffer_pos) {
        return MQTT_ERROR_PROTOCOL;
    }

    char topic[MQTT_MAX_TOPIC_LENGTH];
    memcpy(topic, &client->rx_buffer[pos], topic_len);
    topic[topic_len] = '\0';
    pos += topic_len;

    /* Message ID (for QoS > 0) - we'll skip for now */
    /* QoS would be in the fixed header flags */

    /* Payload */
    uint16_t payload_len = client->rx_buffer_pos - pos;
    const uint8_t *payload = &client->rx_buffer[pos];

    /* Call user callback */
    if (client->message_callback) {
        mqtt_message_t msg = {
            .topic = topic,
            .payload = payload,
            .payload_length = payload_len,
            .qos = MQTT_QOS_0,
            .retained = false,
            .message_id = 0
        };
        client->message_callback(client, &msg, client->message_callback_data);
    }

    return MQTT_OK;
}

/**
 * @brief Handle PUBACK packet
 */
static mqtt_error_t mqtt_handle_puback(mqtt_client_t *client) {
    /* For now, just acknowledge receipt */
    return MQTT_OK;
}

/**
 * @brief Handle SUBACK packet
 */
static mqtt_error_t mqtt_handle_suback(mqtt_client_t *client) {
    /* Check return codes */
    if (client->rx_buffer_pos < 3) {
        return MQTT_ERROR_PROTOCOL;
    }

    uint8_t return_code = client->rx_buffer[2];
    if (return_code == 0x80) {
        return MQTT_ERROR_SUBSCRIBE_FAILED;
    }

    return MQTT_OK;
}

/**
 * @brief Check if topic matches subscription pattern
 */
static bool mqtt_topic_matches(const char *subscription, const char *topic) {
    /* Simple wildcard matching */
    /* + matches single level, # matches multiple levels */

    const char *s = subscription;
    const char *t = topic;

    while (*s && *t) {
        if (*s == '#') {
            return true;  /* # matches everything remaining */
        } else if (*s == '+') {
            /* + matches until next / or end */
            while (*t && *t != '/') t++;
            s++;
            if (*s == '/') s++;
        } else if (*s == *t) {
            s++;
            t++;
        } else {
            return false;
        }
    }

    return (*s == '\0' && *t == '\0');
}

/**
 * @brief MQTT background task
 */
static void mqtt_task(void *param) {
    mqtt_client_t *client = (mqtt_client_t *)param;

    while (client->task_running) {
        mqtt_loop(client);
        os_task_delay(10);  /* 10ms delay */
    }
}

/* ========== Public API ========== */

mqtt_error_t mqtt_client_init(mqtt_client_t *client, const mqtt_config_t *config) {
    if (!client || !config) {
        return MQTT_ERROR_INVALID_PARAM;
    }

    memset(client, 0, sizeof(mqtt_client_t));
    memcpy(&client->config, config, sizeof(mqtt_config_t));

    /* Set defaults */
    if (client->config.broker_port == 0) {
        client->config.broker_port = MQTT_DEFAULT_PORT;
    }
    if (client->config.keepalive_sec == 0) {
        client->config.keepalive_sec = MQTT_DEFAULT_KEEPALIVE;
    }
    if (client->config.timeout_ms == 0) {
        client->config.timeout_ms = MQTT_DEFAULT_TIMEOUT_MS;
    }

    client->state = MQTT_STATE_DISCONNECTED;
    client->next_message_id = 1;

    os_mutex_init(&client->mutex);

    return MQTT_OK;
}

mqtt_error_t mqtt_connect(mqtt_client_t *client) {
    if (!client) {
        return MQTT_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&client->mutex, OS_WAIT_FOREVER);

    if (client->state != MQTT_STATE_DISCONNECTED) {
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_ALREADY_CONNECTED;
    }

    client->state = MQTT_STATE_CONNECTING;

    /* Create TCP connection */
    client->socket = net_socket(SOCK_STREAM);
    if (client->socket < 0) {
        client->state = MQTT_STATE_DISCONNECTED;
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NETWORK;
    }

    /* Resolve hostname and connect */
    ipv4_addr_t broker_ip;
    os_error_t err = net_dns_resolve(client->config.broker_host, &broker_ip, client->config.timeout_ms);
    if (err != OS_OK) {
        net_close(client->socket);
        client->state = MQTT_STATE_DISCONNECTED;
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NETWORK;
    }

    sockaddr_in_t broker_addr = {
        .addr = broker_ip,
        .port = client->config.broker_port
    };

    err = net_connect(client->socket, &broker_addr, client->config.timeout_ms);
    if (err != OS_OK) {
        net_close(client->socket);
        client->state = MQTT_STATE_DISCONNECTED;
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NETWORK;
    }

    /* Send CONNECT packet */
    mqtt_error_t mqtt_err = mqtt_send_connect(client);
    if (mqtt_err != MQTT_OK) {
        net_close(client->socket);
        client->state = MQTT_STATE_DISCONNECTED;
        os_mutex_unlock(&client->mutex);
        return mqtt_err;
    }

    /* Wait for CONNACK */
    uint8_t msg_type;
    mqtt_err = mqtt_receive_packet(client, &msg_type, client->config.timeout_ms);
    if (mqtt_err != MQTT_OK || msg_type != MQTT_MSG_TYPE_CONNACK) {
        net_close(client->socket);
        client->state = MQTT_STATE_DISCONNECTED;
        os_mutex_unlock(&client->mutex);
        return (mqtt_err != MQTT_OK) ? mqtt_err : MQTT_ERROR_PROTOCOL;
    }

    mqtt_err = mqtt_handle_connack(client);
    if (mqtt_err != MQTT_OK) {
        net_close(client->socket);
        client->state = MQTT_STATE_DISCONNECTED;
        os_mutex_unlock(&client->mutex);
        return mqtt_err;
    }

    /* Start background task */
    client->task_running = true;
    os_task_create(&client->task, "mqtt", mqtt_task, client, PRIORITY_NORMAL);

    client->last_activity_ms = mqtt_get_time_ms();
    client->last_ping_ms = mqtt_get_time_ms();

    os_mutex_unlock(&client->mutex);
    return MQTT_OK;
}

mqtt_error_t mqtt_disconnect(mqtt_client_t *client) {
    if (!client) {
        return MQTT_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&client->mutex, OS_WAIT_FOREVER);

    if (client->state != MQTT_STATE_CONNECTED) {
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    client->state = MQTT_STATE_DISCONNECTING;

    /* Stop background task */
    client->task_running = false;

    /* Send DISCONNECT */
    mqtt_send_disconnect(client);

    /* Close socket */
    net_close(client->socket);

    client->state = MQTT_STATE_DISCONNECTED;

    if (client->connection_callback) {
        client->connection_callback(client, false, client->connection_callback_data);
    }

    os_mutex_unlock(&client->mutex);
    return MQTT_OK;
}

mqtt_error_t mqtt_publish(mqtt_client_t *client, const char *topic,
                          const void *payload, uint16_t payload_length,
                          mqtt_qos_t qos, bool retained) {
    if (!client || !topic) {
        return MQTT_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&client->mutex, OS_WAIT_FOREVER);

    if (client->state != MQTT_STATE_CONNECTED) {
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    uint16_t message_id = 0;
    if (qos > MQTT_QOS_0) {
        message_id = mqtt_next_message_id(client);
    }

    mqtt_error_t err = mqtt_send_publish(client, topic, payload, payload_length,
                                         qos, retained, message_id);

    os_mutex_unlock(&client->mutex);
    return err;
}

mqtt_error_t mqtt_subscribe(mqtt_client_t *client, const char *topic, mqtt_qos_t qos) {
    if (!client || !topic) {
        return MQTT_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&client->mutex, OS_WAIT_FOREVER);

    if (client->state != MQTT_STATE_CONNECTED) {
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    /* Find free subscription slot */
    int slot = -1;
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (!client->subscriptions[i].active) {
            slot = i;
            break;
        }
    }

    if (slot == -1) {
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NO_MEMORY;
    }

    mqtt_error_t err = mqtt_send_subscribe(client, topic, qos);
    if (err == MQTT_OK) {
        /* Store subscription */
        strncpy(client->subscriptions[slot].topic, topic, MQTT_MAX_TOPIC_LENGTH - 1);
        client->subscriptions[slot].topic[MQTT_MAX_TOPIC_LENGTH - 1] = '\0';
        client->subscriptions[slot].qos = qos;
        client->subscriptions[slot].active = true;
    }

    os_mutex_unlock(&client->mutex);
    return err;
}

mqtt_error_t mqtt_unsubscribe(mqtt_client_t *client, const char *topic) {
    if (!client || !topic) {
        return MQTT_ERROR_INVALID_PARAM;
    }

    os_mutex_lock(&client->mutex, OS_WAIT_FOREVER);

    if (client->state != MQTT_STATE_CONNECTED) {
        os_mutex_unlock(&client->mutex);
        return MQTT_ERROR_NOT_CONNECTED;
    }

    /* Find and remove subscription */
    for (int i = 0; i < MQTT_MAX_SUBSCRIPTIONS; i++) {
        if (client->subscriptions[i].active &&
            strcmp(client->subscriptions[i].topic, topic) == 0) {
            client->subscriptions[i].active = false;
            break;
        }
    }

    mqtt_error_t err = mqtt_send_unsubscribe(client, topic);

    os_mutex_unlock(&client->mutex);
    return err;
}

void mqtt_set_message_callback(mqtt_client_t *client,
                               mqtt_message_callback_t callback,
                               void *user_data) {
    if (client) {
        client->message_callback = callback;
        client->message_callback_data = user_data;
    }
}

void mqtt_set_connection_callback(mqtt_client_t *client,
                                  mqtt_connection_callback_t callback,
                                  void *user_data) {
    if (client) {
        client->connection_callback = callback;
        client->connection_callback_data = user_data;
    }
}

bool mqtt_is_connected(const mqtt_client_t *client) {
    return client && client->state == MQTT_STATE_CONNECTED;
}

mqtt_state_t mqtt_get_state(const mqtt_client_t *client) {
    return client ? client->state : MQTT_STATE_DISCONNECTED;
}

mqtt_error_t mqtt_loop(mqtt_client_t *client) {
    if (!client || client->state != MQTT_STATE_CONNECTED) {
        return MQTT_ERROR_NOT_CONNECTED;
    }

    uint32_t now = mqtt_get_time_ms();

    /* Check for keepalive timeout */
    uint32_t keepalive_ms = client->config.keepalive_sec * 1000;
    if ((now - client->last_ping_ms) >= keepalive_ms) {
        mqtt_send_pingreq(client);
        client->last_ping_ms = now;
    }

    /* Try to receive packet (non-blocking) */
    uint8_t msg_type;
    mqtt_error_t err = mqtt_receive_packet(client, &msg_type, 100);  /* 100ms timeout */

    if (err == MQTT_ERROR_TIMEOUT) {
        return MQTT_OK;  /* No message available, that's OK */
    } else if (err != MQTT_OK) {
        return err;
    }

    /* Handle received packet */
    switch (msg_type) {
        case MQTT_MSG_TYPE_PUBLISH:
            return mqtt_handle_publish(client);
        case MQTT_MSG_TYPE_PUBACK:
            return mqtt_handle_puback(client);
        case MQTT_MSG_TYPE_SUBACK:
            return mqtt_handle_suback(client);
        case MQTT_MSG_TYPE_PINGRESP:
            /* Keepalive response received */
            return MQTT_OK;
        default:
            /* Unknown or unhandled message type */
            return MQTT_OK;
    }
}

const char *mqtt_error_to_string(mqtt_error_t error) {
    switch (error) {
        case MQTT_OK: return "OK";
        case MQTT_ERROR_INVALID_PARAM: return "Invalid parameter";
        case MQTT_ERROR_NOT_CONNECTED: return "Not connected";
        case MQTT_ERROR_ALREADY_CONNECTED: return "Already connected";
        case MQTT_ERROR_NETWORK: return "Network error";
        case MQTT_ERROR_TIMEOUT: return "Timeout";
        case MQTT_ERROR_PROTOCOL: return "Protocol error";
        case MQTT_ERROR_BUFFER_OVERFLOW: return "Buffer overflow";
        case MQTT_ERROR_BROKER_REFUSED: return "Broker refused connection";
        case MQTT_ERROR_SUBSCRIBE_FAILED: return "Subscribe failed";
        case MQTT_ERROR_PUBLISH_FAILED: return "Publish failed";
        case MQTT_ERROR_NO_MEMORY: return "No memory";
        default: return "Unknown error";
    }
}

const char *mqtt_state_to_string(mqtt_state_t state) {
    switch (state) {
        case MQTT_STATE_DISCONNECTED: return "Disconnected";
        case MQTT_STATE_CONNECTING: return "Connecting";
        case MQTT_STATE_CONNECTED: return "Connected";
        case MQTT_STATE_DISCONNECTING: return "Disconnecting";
        default: return "Unknown";
    }
}
