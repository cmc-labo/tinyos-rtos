/**
 * @file mqtt_demo.c
 * @brief MQTT Client Demo for TinyOS-RTOS
 *
 * This example demonstrates:
 * - Connecting to an MQTT broker
 * - Publishing sensor data periodically
 * - Subscribing to control topics
 * - Handling incoming messages
 * - Automatic reconnection
 */

#include "tinyos.h"
#include "tinyos/net.h"
#include "tinyos/mqtt.h"
#include <stdio.h>
#include <string.h>

/* MQTT Configuration */
#define MQTT_BROKER_HOST    "192.168.1.100"  /* Change to your MQTT broker */
#define MQTT_BROKER_PORT    1883
#define MQTT_CLIENT_ID      "tinyos_device_001"
#define MQTT_USERNAME       NULL  /* Set if broker requires auth */
#define MQTT_PASSWORD       NULL

/* Topics */
#define TOPIC_TEMPERATURE   "sensor/temperature"
#define TOPIC_HUMIDITY      "sensor/humidity"
#define TOPIC_CONTROL       "device/control"
#define TOPIC_STATUS        "device/status"

/* Global variables */
static mqtt_client_t mqtt_client;
static bool system_running = true;
static float temperature = 25.0f;
static float humidity = 60.0f;

/**
 * @brief Simulated temperature sensor reading
 */
static float read_temperature(void) {
    /* Simulate temperature variation */
    temperature += ((float)(rand() % 20) - 10) / 10.0f;
    if (temperature < 15.0f) temperature = 15.0f;
    if (temperature > 35.0f) temperature = 35.0f;
    return temperature;
}

/**
 * @brief Simulated humidity sensor reading
 */
static float read_humidity(void) {
    /* Simulate humidity variation */
    humidity += ((float)(rand() % 20) - 10) / 10.0f;
    if (humidity < 40.0f) humidity = 40.0f;
    if (humidity > 80.0f) humidity = 80.0f;
    return humidity;
}

/**
 * @brief MQTT message received callback
 */
static void mqtt_message_received(mqtt_client_t *client,
                                  const mqtt_message_t *message,
                                  void *user_data) {
    printf("[MQTT] Message received on topic: %s\n", message->topic);
    printf("[MQTT] Payload (%u bytes): ", message->payload_length);

    /* Print payload */
    for (uint16_t i = 0; i < message->payload_length; i++) {
        printf("%c", message->payload[i]);
    }
    printf("\n");

    /* Handle control commands */
    if (strcmp(message->topic, TOPIC_CONTROL) == 0) {
        char cmd[64];
        uint16_t len = message->payload_length;
        if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;

        memcpy(cmd, message->payload, len);
        cmd[len] = '\0';

        printf("[MQTT] Processing command: %s\n", cmd);

        if (strcmp(cmd, "status") == 0) {
            /* Publish status */
            const char *status = "Device online and operational";
            mqtt_publish(client, TOPIC_STATUS, status, strlen(status),
                        MQTT_QOS_1, false);
            printf("[MQTT] Published status\n");
        } else if (strcmp(cmd, "shutdown") == 0) {
            printf("[MQTT] Shutdown command received\n");
            system_running = false;
        } else if (strncmp(cmd, "interval=", 9) == 0) {
            int interval = atoi(cmd + 9);
            printf("[MQTT] Set reporting interval to %d seconds\n", interval);
        }
    }
}

/**
 * @brief MQTT connection state callback
 */
static void mqtt_connection_changed(mqtt_client_t *client,
                                   bool connected,
                                   void *user_data) {
    if (connected) {
        printf("[MQTT] Connected to broker!\n");

        /* Subscribe to control topic */
        mqtt_error_t err = mqtt_subscribe(client, TOPIC_CONTROL, MQTT_QOS_1);
        if (err == MQTT_OK) {
            printf("[MQTT] Subscribed to %s\n", TOPIC_CONTROL);
        } else {
            printf("[MQTT] Subscribe failed: %s\n", mqtt_error_to_string(err));
        }

        /* Publish online status */
        const char *online_msg = "online";
        mqtt_publish(client, TOPIC_STATUS, online_msg, strlen(online_msg),
                    MQTT_QOS_1, true);
    } else {
        printf("[MQTT] Disconnected from broker\n");
    }
}

/**
 * @brief Sensor publishing task
 */
static void sensor_task(void *param) {
    printf("[Sensor] Task started\n");

    while (system_running) {
        if (mqtt_is_connected(&mqtt_client)) {
            /* Read sensors */
            float temp = read_temperature();
            float hum = read_humidity();

            /* Publish temperature */
            char payload[32];
            snprintf(payload, sizeof(payload), "%.1f", temp);
            mqtt_error_t err = mqtt_publish(&mqtt_client, TOPIC_TEMPERATURE,
                                           payload, strlen(payload),
                                           MQTT_QOS_0, false);
            if (err == MQTT_OK) {
                printf("[Sensor] Published temperature: %s\n", payload);
            }

            /* Publish humidity */
            snprintf(payload, sizeof(payload), "%.1f", hum);
            err = mqtt_publish(&mqtt_client, TOPIC_HUMIDITY,
                              payload, strlen(payload),
                              MQTT_QOS_0, false);
            if (err == MQTT_OK) {
                printf("[Sensor] Published humidity: %s\n", payload);
            }
        } else {
            printf("[Sensor] Waiting for MQTT connection...\n");
        }

        /* Publish every 5 seconds */
        os_task_delay(5000);
    }

    printf("[Sensor] Task stopped\n");
}

/**
 * @brief Network initialization
 */
static bool init_network(void) {
    printf("[Network] Initializing...\n");

    /* Network configuration */
    net_config_t config = {
        .mac = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}},
        .ip = {{192, 168, 1, 150}},
        .netmask = {{255, 255, 255, 0}},
        .gateway = {{192, 168, 1, 1}},
        .dns = {{8, 8, 8, 8}}
    };

    /* Get network driver (platform specific) */
    extern net_driver_t *get_loopback_driver(void);
    net_driver_t *driver = get_loopback_driver();

    /* Initialize and start network */
    os_error_t err = net_init(driver, &config);
    if (err != OS_OK) {
        printf("[Network] Initialization failed\n");
        return false;
    }

    err = net_start();
    if (err != OS_OK) {
        printf("[Network] Start failed\n");
        return false;
    }

    printf("[Network] Initialized successfully\n");
    printf("[Network] IP: 192.168.1.150\n");
    return true;
}

/**
 * @brief MQTT initialization
 */
static bool init_mqtt(void) {
    printf("[MQTT] Initializing...\n");

    /* MQTT configuration */
    mqtt_config_t config = {
        .broker_host = MQTT_BROKER_HOST,
        .broker_port = MQTT_BROKER_PORT,
        .client_id = MQTT_CLIENT_ID,
        .username = MQTT_USERNAME,
        .password = MQTT_PASSWORD,
        .keepalive_sec = 60,
        .clean_session = true,
        .will_topic = TOPIC_STATUS,
        .will_message = "offline",
        .will_message_len = 7,
        .will_qos = MQTT_QOS_1,
        .will_retained = true,
        .timeout_ms = 5000,
        .auto_reconnect = true,
        .reconnect_interval_ms = 5000
    };

    /* Initialize client */
    mqtt_error_t err = mqtt_client_init(&mqtt_client, &config);
    if (err != MQTT_OK) {
        printf("[MQTT] Initialization failed: %s\n", mqtt_error_to_string(err));
        return false;
    }

    /* Set callbacks */
    mqtt_set_message_callback(&mqtt_client, mqtt_message_received, NULL);
    mqtt_set_connection_callback(&mqtt_client, mqtt_connection_changed, NULL);

    printf("[MQTT] Initialized successfully\n");
    return true;
}

/**
 * @brief Main function
 */
int main(void) {
    printf("\n");
    printf("========================================\n");
    printf("  TinyOS-RTOS MQTT Client Demo\n");
    printf("========================================\n\n");

    /* Initialize OS */
    os_init();
    os_mem_init();

    /* Initialize network stack */
    if (!init_network()) {
        printf("ERROR: Network initialization failed\n");
        return 1;
    }

    /* Initialize MQTT client */
    if (!init_mqtt()) {
        printf("ERROR: MQTT initialization failed\n");
        return 1;
    }

    /* Wait for network to be ready */
    printf("[System] Waiting for network...\n");
    os_task_delay(1000);

    /* Connect to MQTT broker */
    printf("[MQTT] Connecting to broker at %s:%u...\n",
           MQTT_BROKER_HOST, MQTT_BROKER_PORT);

    mqtt_error_t err = mqtt_connect(&mqtt_client);
    if (err != MQTT_OK) {
        printf("[MQTT] Connection failed: %s\n", mqtt_error_to_string(err));
        printf("[MQTT] Please check:\n");
        printf("  1. Broker is running at %s:%u\n", MQTT_BROKER_HOST, MQTT_BROKER_PORT);
        printf("  2. Network connectivity\n");
        printf("  3. Firewall settings\n");
        /* Continue anyway - auto-reconnect will retry */
    }

    /* Create sensor task */
    tcb_t sensor_task_tcb;
    os_task_create(&sensor_task_tcb, "sensor", sensor_task, NULL, PRIORITY_NORMAL);

    printf("[System] All tasks created\n");
    printf("[System] System running. Press Ctrl+C to stop.\n\n");

    /* Start OS scheduler */
    os_start();

    /* Cleanup (never reached in embedded systems) */
    mqtt_disconnect(&mqtt_client);

    return 0;
}

/**
 * @brief Example output:
 *
 * ========================================
 *   TinyOS-RTOS MQTT Client Demo
 * ========================================
 *
 * [Network] Initializing...
 * [Network] Initialized successfully
 * [Network] IP: 192.168.1.150
 * [MQTT] Initializing...
 * [MQTT] Initialized successfully
 * [System] Waiting for network...
 * [MQTT] Connecting to broker at 192.168.1.100:1883...
 * [MQTT] Connected to broker!
 * [MQTT] Subscribed to device/control
 * [Sensor] Task started
 * [Sensor] Published temperature: 25.3
 * [Sensor] Published humidity: 61.2
 * [Sensor] Published temperature: 26.1
 * [Sensor] Published humidity: 59.8
 * [MQTT] Message received on topic: device/control
 * [MQTT] Payload (6 bytes): status
 * [MQTT] Processing command: status
 * [MQTT] Published status
 * [Sensor] Published temperature: 25.7
 * [Sensor] Published humidity: 60.5
 */
