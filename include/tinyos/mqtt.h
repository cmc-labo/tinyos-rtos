/**
 * @file mqtt.h
 * @brief MQTT Client for TinyOS-RTOS
 *
 * Lightweight MQTT 3.1.1 client implementation for IoT devices.
 * Supports QoS 0, 1, 2 and automatic reconnection.
 */

#ifndef TINYOS_MQTT_H
#define TINYOS_MQTT_H

#include <stdint.h>
#include <stdbool.h>
#include "tinyos.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup MQTT Client
 * @{
 */

/* MQTT Constants */
#define MQTT_MAX_PACKET_SIZE        1024
#define MQTT_MAX_TOPIC_LENGTH       128
#define MQTT_MAX_CLIENT_ID_LENGTH   23
#define MQTT_MAX_USERNAME_LENGTH    64
#define MQTT_MAX_PASSWORD_LENGTH    64
#define MQTT_MAX_SUBSCRIPTIONS      16
#define MQTT_DEFAULT_KEEPALIVE      60
#define MQTT_DEFAULT_PORT           1883
#define MQTT_DEFAULT_TIMEOUT_MS     5000

/* MQTT Protocol Version */
#define MQTT_PROTOCOL_VERSION_3_1_1 4

/* MQTT QoS Levels */
typedef enum {
    MQTT_QOS_0 = 0,  /* At most once delivery */
    MQTT_QOS_1 = 1,  /* At least once delivery */
    MQTT_QOS_2 = 2   /* Exactly once delivery */
} mqtt_qos_t;

/* MQTT Error Codes */
typedef enum {
    MQTT_OK = 0,
    MQTT_ERROR_INVALID_PARAM,
    MQTT_ERROR_NOT_CONNECTED,
    MQTT_ERROR_ALREADY_CONNECTED,
    MQTT_ERROR_NETWORK,
    MQTT_ERROR_TIMEOUT,
    MQTT_ERROR_PROTOCOL,
    MQTT_ERROR_BUFFER_OVERFLOW,
    MQTT_ERROR_BROKER_REFUSED,
    MQTT_ERROR_SUBSCRIBE_FAILED,
    MQTT_ERROR_PUBLISH_FAILED,
    MQTT_ERROR_NO_MEMORY
} mqtt_error_t;

/* MQTT Connection States */
typedef enum {
    MQTT_STATE_DISCONNECTED = 0,
    MQTT_STATE_CONNECTING,
    MQTT_STATE_CONNECTED,
    MQTT_STATE_DISCONNECTING
} mqtt_state_t;

/* MQTT Connection Return Codes (CONNACK) */
typedef enum {
    MQTT_CONNACK_ACCEPTED = 0,
    MQTT_CONNACK_REFUSED_PROTOCOL_VERSION,
    MQTT_CONNACK_REFUSED_IDENTIFIER_REJECTED,
    MQTT_CONNACK_REFUSED_SERVER_UNAVAILABLE,
    MQTT_CONNACK_REFUSED_BAD_CREDENTIALS,
    MQTT_CONNACK_REFUSED_NOT_AUTHORIZED
} mqtt_connack_t;

/* MQTT Message Types */
typedef enum {
    MQTT_MSG_TYPE_CONNECT = 1,
    MQTT_MSG_TYPE_CONNACK = 2,
    MQTT_MSG_TYPE_PUBLISH = 3,
    MQTT_MSG_TYPE_PUBACK = 4,
    MQTT_MSG_TYPE_PUBREC = 5,
    MQTT_MSG_TYPE_PUBREL = 6,
    MQTT_MSG_TYPE_PUBCOMP = 7,
    MQTT_MSG_TYPE_SUBSCRIBE = 8,
    MQTT_MSG_TYPE_SUBACK = 9,
    MQTT_MSG_TYPE_UNSUBSCRIBE = 10,
    MQTT_MSG_TYPE_UNSUBACK = 11,
    MQTT_MSG_TYPE_PINGREQ = 12,
    MQTT_MSG_TYPE_PINGRESP = 13,
    MQTT_MSG_TYPE_DISCONNECT = 14
} mqtt_msg_type_t;

/* Forward declarations */
typedef struct mqtt_client mqtt_client_t;
typedef struct mqtt_message mqtt_message_t;

/**
 * @brief MQTT message structure
 */
struct mqtt_message {
    const char *topic;           /* Topic name */
    const uint8_t *payload;      /* Message payload */
    uint16_t payload_length;     /* Payload length in bytes */
    mqtt_qos_t qos;              /* Quality of Service level */
    bool retained;               /* Retained message flag */
    uint16_t message_id;         /* Message ID (for QoS > 0) */
};

/**
 * @brief MQTT message callback function
 *
 * Called when a message is received on a subscribed topic.
 *
 * @param client MQTT client instance
 * @param message Received message
 * @param user_data User data passed to callback
 */
typedef void (*mqtt_message_callback_t)(
    mqtt_client_t *client,
    const mqtt_message_t *message,
    void *user_data
);

/**
 * @brief MQTT connection callback function
 *
 * Called when connection state changes.
 *
 * @param client MQTT client instance
 * @param connected true if connected, false if disconnected
 * @param user_data User data passed to callback
 */
typedef void (*mqtt_connection_callback_t)(
    mqtt_client_t *client,
    bool connected,
    void *user_data
);

/**
 * @brief MQTT client configuration
 */
typedef struct {
    const char *broker_host;     /* MQTT broker hostname or IP */
    uint16_t broker_port;        /* MQTT broker port (default: 1883) */
    const char *client_id;       /* Client identifier (max 23 chars) */
    const char *username;        /* Username (NULL if not used) */
    const char *password;        /* Password (NULL if not used) */
    uint16_t keepalive_sec;      /* Keep-alive interval in seconds */
    bool clean_session;          /* Clean session flag */
    const char *will_topic;      /* Last Will topic (NULL if not used) */
    const char *will_message;    /* Last Will message */
    uint16_t will_message_len;   /* Last Will message length */
    mqtt_qos_t will_qos;         /* Last Will QoS */
    bool will_retained;          /* Last Will retained flag */
    uint32_t timeout_ms;         /* Command timeout in milliseconds */
    bool auto_reconnect;         /* Enable automatic reconnection */
    uint32_t reconnect_interval_ms; /* Reconnect interval */
} mqtt_config_t;

/**
 * @brief Subscription structure (internal)
 */
typedef struct {
    char topic[MQTT_MAX_TOPIC_LENGTH];
    mqtt_qos_t qos;
    bool active;
} mqtt_subscription_t;

/**
 * @brief MQTT client structure
 */
struct mqtt_client {
    /* Configuration */
    mqtt_config_t config;

    /* State */
    mqtt_state_t state;
    net_socket_t socket;
    uint16_t next_message_id;
    uint32_t last_activity_ms;
    uint32_t last_ping_ms;

    /* Callbacks */
    mqtt_message_callback_t message_callback;
    void *message_callback_data;
    mqtt_connection_callback_t connection_callback;
    void *connection_callback_data;

    /* Subscriptions */
    mqtt_subscription_t subscriptions[MQTT_MAX_SUBSCRIPTIONS];

    /* Buffers */
    uint8_t tx_buffer[MQTT_MAX_PACKET_SIZE];
    uint8_t rx_buffer[MQTT_MAX_PACKET_SIZE];
    uint16_t rx_buffer_pos;

    /* Task handle */
    tcb_t task;
    bool task_running;

    /* Synchronization */
    mutex_t mutex;
};

/**
 * @brief Initialize MQTT client
 *
 * @param client MQTT client instance
 * @param config Client configuration
 * @return MQTT_OK on success, error code otherwise
 */
mqtt_error_t mqtt_client_init(mqtt_client_t *client, const mqtt_config_t *config);

/**
 * @brief Connect to MQTT broker
 *
 * Establishes TCP connection and performs MQTT handshake.
 *
 * @param client MQTT client instance
 * @return MQTT_OK on success, error code otherwise
 */
mqtt_error_t mqtt_connect(mqtt_client_t *client);

/**
 * @brief Disconnect from MQTT broker
 *
 * Sends DISCONNECT packet and closes connection gracefully.
 *
 * @param client MQTT client instance
 * @return MQTT_OK on success, error code otherwise
 */
mqtt_error_t mqtt_disconnect(mqtt_client_t *client);

/**
 * @brief Publish message to topic
 *
 * @param client MQTT client instance
 * @param topic Topic name
 * @param payload Message payload
 * @param payload_length Payload length in bytes
 * @param qos Quality of Service level (0, 1, or 2)
 * @param retained Retained message flag
 * @return MQTT_OK on success, error code otherwise
 */
mqtt_error_t mqtt_publish(
    mqtt_client_t *client,
    const char *topic,
    const void *payload,
    uint16_t payload_length,
    mqtt_qos_t qos,
    bool retained
);

/**
 * @brief Subscribe to topic
 *
 * @param client MQTT client instance
 * @param topic Topic name (supports wildcards: + and #)
 * @param qos Maximum QoS level for messages on this topic
 * @return MQTT_OK on success, error code otherwise
 */
mqtt_error_t mqtt_subscribe(
    mqtt_client_t *client,
    const char *topic,
    mqtt_qos_t qos
);

/**
 * @brief Unsubscribe from topic
 *
 * @param client MQTT client instance
 * @param topic Topic name
 * @return MQTT_OK on success, error code otherwise
 */
mqtt_error_t mqtt_unsubscribe(mqtt_client_t *client, const char *topic);

/**
 * @brief Set message received callback
 *
 * @param client MQTT client instance
 * @param callback Callback function
 * @param user_data User data to pass to callback
 */
void mqtt_set_message_callback(
    mqtt_client_t *client,
    mqtt_message_callback_t callback,
    void *user_data
);

/**
 * @brief Set connection state callback
 *
 * @param client MQTT client instance
 * @param callback Callback function
 * @param user_data User data to pass to callback
 */
void mqtt_set_connection_callback(
    mqtt_client_t *client,
    mqtt_connection_callback_t callback,
    void *user_data
);

/**
 * @brief Check if client is connected
 *
 * @param client MQTT client instance
 * @return true if connected, false otherwise
 */
bool mqtt_is_connected(const mqtt_client_t *client);

/**
 * @brief Get current connection state
 *
 * @param client MQTT client instance
 * @return Current state
 */
mqtt_state_t mqtt_get_state(const mqtt_client_t *client);

/**
 * @brief Process MQTT client (internal loop)
 *
 * This is called automatically by the background task.
 * Do not call manually unless using custom task management.
 *
 * @param client MQTT client instance
 * @return MQTT_OK on success, error code otherwise
 */
mqtt_error_t mqtt_loop(mqtt_client_t *client);

/**
 * @brief Convert error code to string
 *
 * @param error Error code
 * @return Error description string
 */
const char *mqtt_error_to_string(mqtt_error_t error);

/**
 * @brief Convert state to string
 *
 * @param state MQTT state
 * @return State description string
 */
const char *mqtt_state_to_string(mqtt_state_t state);

/** @} */

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_MQTT_H */
