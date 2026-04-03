/**
 * @file mqtt.h
 * @brief MQTT Client for TinyOS-RTOS
 *
 * Lightweight MQTT 3.1.1 client with:
 *  - Full QoS 0 / 1 / 2 publish handshakes (PUBACK / PUBREC-PUBREL-PUBCOMP)
 *  - In-flight table  : persists unACKed QoS1/2 messages across the session;
 *                       automatically retransmits with DUP flag on timeout.
 *  - Offline queue    : QoS1/2 publishes made while disconnected are buffered
 *                       and flushed automatically on reconnection.
 *  - Auto-reconnect   : exponential back-off, re-subscribes after reconnect.
 */

#ifndef TINYOS_MQTT_H
#define TINYOS_MQTT_H

#include <stdint.h>
#include <stdbool.h>
#include "tinyos.h"
#include "tinyos/net.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Compile-time limits  (override via -D flags)
 * ========================================================================= */

#define MQTT_MAX_PACKET_SIZE        1024  /* Max raw packet size (bytes)        */
#define MQTT_MAX_TOPIC_LENGTH       128   /* Max topic string length            */
#define MQTT_MAX_CLIENT_ID_LENGTH   23
#define MQTT_MAX_USERNAME_LENGTH    64
#define MQTT_MAX_PASSWORD_LENGTH    64
#define MQTT_MAX_SUBSCRIPTIONS      16    /* Max concurrent subscriptions       */
#define MQTT_MAX_PAYLOAD_SIZE       512   /* Max payload in queued messages     */

/* Reliability */
#define MQTT_MAX_INFLIGHT           8     /* In-flight QoS1/2 slots             */
#define MQTT_MAX_PENDING            8     /* Offline-queue slots                */
#define MQTT_RETRY_INTERVAL_MS      5000  /* Retry unACKed msg after this many ms */
#define MQTT_MAX_RETRY_COUNT        5     /* Discard after this many retries    */

/* Auto-reconnect */
#define MQTT_RECONNECT_BASE_MS      3000  /* First retry delay                  */
#define MQTT_RECONNECT_MAX_MS       60000 /* Maximum retry delay (exp. backoff) */

/* Protocol defaults */
#define MQTT_DEFAULT_KEEPALIVE      60
#define MQTT_DEFAULT_PORT           1883
#define MQTT_DEFAULT_TIMEOUT_MS     5000
#define MQTT_PROTOCOL_VERSION_3_1_1 4

/* =========================================================================
 * Enumerations
 * ========================================================================= */

typedef enum {
    MQTT_QOS_0 = 0,   /* At most once  */
    MQTT_QOS_1 = 1,   /* At least once */
    MQTT_QOS_2 = 2    /* Exactly once  */
} mqtt_qos_t;

typedef enum {
    MQTT_OK                    =  0,
    MQTT_QUEUED                =  1,  /* Message buffered; not yet sent      */
    MQTT_ERROR_INVALID_PARAM   = -1,
    MQTT_ERROR_NOT_CONNECTED   = -2,
    MQTT_ERROR_ALREADY_CONNECTED = -3,
    MQTT_ERROR_NETWORK         = -4,
    MQTT_ERROR_TIMEOUT         = -5,
    MQTT_ERROR_PROTOCOL        = -6,
    MQTT_ERROR_BUFFER_OVERFLOW = -7,
    MQTT_ERROR_BROKER_REFUSED  = -8,
    MQTT_ERROR_SUBSCRIBE_FAILED= -9,
    MQTT_ERROR_PUBLISH_FAILED  = -10,
    MQTT_ERROR_NO_MEMORY       = -11,
    MQTT_ERROR_QUEUE_FULL      = -12, /* Offline queue is full               */
    MQTT_ERROR_INFLIGHT_FULL   = -13  /* In-flight table is full             */
} mqtt_error_t;

typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_RECONNECTING,
    MQTT_STATE_DISCONNECTING
} mqtt_state_t;

typedef enum {
    MQTT_CONNACK_ACCEPTED                    = 0,
    MQTT_CONNACK_REFUSED_PROTOCOL_VERSION    = 1,
    MQTT_CONNACK_REFUSED_IDENTIFIER_REJECTED = 2,
    MQTT_CONNACK_REFUSED_SERVER_UNAVAILABLE  = 3,
    MQTT_CONNACK_REFUSED_BAD_CREDENTIALS     = 4,
    MQTT_CONNACK_REFUSED_NOT_AUTHORIZED      = 5
} mqtt_connack_t;

typedef enum {
    MQTT_MSG_TYPE_CONNECT     = 1,
    MQTT_MSG_TYPE_CONNACK     = 2,
    MQTT_MSG_TYPE_PUBLISH     = 3,
    MQTT_MSG_TYPE_PUBACK      = 4,
    MQTT_MSG_TYPE_PUBREC      = 5,
    MQTT_MSG_TYPE_PUBREL      = 6,
    MQTT_MSG_TYPE_PUBCOMP     = 7,
    MQTT_MSG_TYPE_SUBSCRIBE   = 8,
    MQTT_MSG_TYPE_SUBACK      = 9,
    MQTT_MSG_TYPE_UNSUBSCRIBE = 10,
    MQTT_MSG_TYPE_UNSUBACK    = 11,
    MQTT_MSG_TYPE_PINGREQ     = 12,
    MQTT_MSG_TYPE_PINGRESP    = 13,
    MQTT_MSG_TYPE_DISCONNECT  = 14
} mqtt_msg_type_t;

/** State of a single in-flight QoS1/2 message. */
typedef enum {
    MQTT_INFLIGHT_WAIT_PUBACK  = 0,  /* QoS1 – waiting for PUBACK          */
    MQTT_INFLIGHT_WAIT_PUBREC  = 1,  /* QoS2 – waiting for PUBREC          */
    MQTT_INFLIGHT_WAIT_PUBCOMP = 2   /* QoS2 – PUBREL sent, waiting PUBCOMP*/
} mqtt_inflight_state_t;

/* =========================================================================
 * Data structures
 * ========================================================================= */

typedef struct mqtt_client   mqtt_client_t;
typedef struct mqtt_message  mqtt_message_t;

struct mqtt_message {
    const char    *topic;
    const uint8_t *payload;
    uint16_t       payload_length;
    mqtt_qos_t     qos;
    bool           retained;
    uint16_t       message_id;
};

typedef void (*mqtt_message_callback_t)(
    mqtt_client_t       *client,
    const mqtt_message_t *message,
    void                *user_data);

typedef void (*mqtt_connection_callback_t)(
    mqtt_client_t *client,
    bool           connected,
    void          *user_data);

typedef struct {
    const char *broker_host;
    uint16_t    broker_port;
    const char *client_id;
    const char *username;
    const char *password;
    uint16_t    keepalive_sec;
    bool        clean_session;
    const char *will_topic;
    const char *will_message;
    uint16_t    will_message_len;
    mqtt_qos_t  will_qos;
    bool        will_retained;
    uint32_t    timeout_ms;
    bool        auto_reconnect;
    uint32_t    reconnect_interval_ms; /* 0 → use MQTT_RECONNECT_BASE_MS    */
} mqtt_config_t;

/** One slot in the in-flight table. */
typedef struct {
    bool                  active;
    uint16_t              message_id;
    char                  topic[MQTT_MAX_TOPIC_LENGTH];
    uint8_t               payload[MQTT_MAX_PAYLOAD_SIZE];
    uint16_t              payload_len;
    mqtt_qos_t            qos;
    bool                  retained;
    uint32_t              sent_time_ms;   /* When (re)sent last          */
    uint8_t               retry_count;
    mqtt_inflight_state_t state;
} mqtt_inflight_t;

/** One slot in the offline pending queue. */
typedef struct {
    bool       active;
    char       topic[MQTT_MAX_TOPIC_LENGTH];
    uint8_t    payload[MQTT_MAX_PAYLOAD_SIZE];
    uint16_t   payload_len;
    mqtt_qos_t qos;
    bool       retained;
} mqtt_pending_t;

typedef struct {
    char       topic[MQTT_MAX_TOPIC_LENGTH];
    mqtt_qos_t qos;
    bool       active;
} mqtt_subscription_t;

struct mqtt_client {
    /* Configuration */
    mqtt_config_t config;

    /* Connection state */
    mqtt_state_t  state;
    net_socket_t  socket;
    uint16_t      next_message_id;
    uint32_t      last_activity_ms;
    uint32_t      last_ping_ms;

    /* Callbacks */
    mqtt_message_callback_t    message_callback;
    void                      *message_callback_data;
    mqtt_connection_callback_t connection_callback;
    void                      *connection_callback_data;

    /* Subscriptions */
    mqtt_subscription_t subscriptions[MQTT_MAX_SUBSCRIPTIONS];

    /* QoS1/2 in-flight table */
    mqtt_inflight_t inflight[MQTT_MAX_INFLIGHT];
    uint8_t         inflight_count;

    /* Offline publish queue (QoS1/2 messages buffered while disconnected) */
    mqtt_pending_t pending[MQTT_MAX_PENDING];
    uint8_t        pending_count;

    /* Auto-reconnect bookkeeping */
    uint32_t next_reconnect_ms;    /* Absolute time for next attempt       */
    uint8_t  reconnect_attempt;    /* Consecutive failures (for backoff)   */

    /* Packet buffers */
    uint8_t  tx_buffer[MQTT_MAX_PACKET_SIZE];
    uint8_t  rx_buffer[MQTT_MAX_PACKET_SIZE];
    uint16_t rx_buffer_pos;

    /* Background task */
    tcb_t    task;
    bool     task_running;

    /* Concurrency */
    mutex_t  mutex;
};

/* =========================================================================
 * Public API
 * ========================================================================= */

/** Initialise client; must be called before any other function. */
mqtt_error_t mqtt_client_init(mqtt_client_t *client, const mqtt_config_t *config);

/** Connect to broker (blocking; spawns background task on success). */
mqtt_error_t mqtt_connect(mqtt_client_t *client);

/** Graceful disconnect; flushes in-flight table. */
mqtt_error_t mqtt_disconnect(mqtt_client_t *client);

/**
 * Publish a message.
 *
 * - QoS 0 : fire-and-forget; returns MQTT_ERROR_NOT_CONNECTED if offline.
 * - QoS 1/2: if connected, sends immediately and adds to the in-flight table.
 *             If disconnected, enqueues to the offline buffer and returns
 *             MQTT_QUEUED.  Returns MQTT_ERROR_QUEUE_FULL when the buffer is
 *             exhausted.
 */
mqtt_error_t mqtt_publish(
    mqtt_client_t *client,
    const char    *topic,
    const void    *payload,
    uint16_t       payload_length,
    mqtt_qos_t     qos,
    bool           retained);

/** Subscribe to a topic (wildcards + and # supported). */
mqtt_error_t mqtt_subscribe(mqtt_client_t *client, const char *topic, mqtt_qos_t qos);

/** Unsubscribe from a topic. */
mqtt_error_t mqtt_unsubscribe(mqtt_client_t *client, const char *topic);

void mqtt_set_message_callback(
    mqtt_client_t            *client,
    mqtt_message_callback_t   callback,
    void                     *user_data);

void mqtt_set_connection_callback(
    mqtt_client_t              *client,
    mqtt_connection_callback_t  callback,
    void                       *user_data);

bool         mqtt_is_connected(const mqtt_client_t *client);
mqtt_state_t mqtt_get_state(const mqtt_client_t *client);

/** Number of messages currently in the offline queue. */
uint8_t mqtt_get_pending_count(const mqtt_client_t *client);

/** Number of messages in the in-flight table (sent, awaiting ACK). */
uint8_t mqtt_get_inflight_count(const mqtt_client_t *client);

/** Discard all messages in the offline queue. */
void mqtt_flush_pending(mqtt_client_t *client);

/**
 * Internal loop — called automatically by the background task.
 * Do not call directly unless you manage the task yourself.
 */
mqtt_error_t mqtt_loop(mqtt_client_t *client);

const char *mqtt_error_to_string(mqtt_error_t error);
const char *mqtt_state_to_string(mqtt_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_MQTT_H */
