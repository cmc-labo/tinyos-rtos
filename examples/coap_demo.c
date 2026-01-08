/**
 * @file coap_demo.c
 * @brief CoAP Client and Server Demo for TinyOS
 *
 * Demonstrates:
 * - CoAP server with multiple resources
 * - CoAP client making GET/POST/PUT/DELETE requests
 * - RESTful sensor data access
 * - CoAP observe pattern (optional)
 */

#include "tinyos.h"
#include "tinyos/net.h"
#include "tinyos/coap.h"
#include <stdio.h>
#include <string.h>

/* Network configuration */
#define SERVER_IP       IPV4(192, 168, 1, 100)
#define CLIENT_IP       IPV4(192, 168, 1, 101)
#define SERVER_PORT     COAP_DEFAULT_PORT

/* Simulated sensor data */
static float temperature = 25.5f;
static float humidity = 60.0f;
static bool led_state = false;

/* ====================
 * CoAP Server Resources
 * ==================== */

/* GET /sensor/temperature */
void temperature_handler(
    coap_context_t *context,
    coap_resource_t *resource,
    const coap_pdu_t *request,
    coap_pdu_t *response,
    void *user_data
) {
    printf("[Server] GET /sensor/temperature\n");

    /* Prepare response payload */
    char payload[32];
    snprintf(payload, sizeof(payload), "{\"temp\":%.1f}", temperature);

    /* Set response */
    response->code = COAP_RESPONSE_205_CONTENT;

    /* Add Content-Format option (JSON) */
    uint8_t format = COAP_CONTENT_FORMAT_JSON;
    coap_pdu_add_option(response, COAP_OPTION_CONTENT_FORMAT, &format, 1);

    coap_pdu_set_payload(response, (const uint8_t *)payload, strlen(payload));

    printf("[Server] Response: %s\n", payload);
}

/* GET /sensor/humidity */
void humidity_handler(
    coap_context_t *context,
    coap_resource_t *resource,
    const coap_pdu_t *request,
    coap_pdu_t *response,
    void *user_data
) {
    printf("[Server] GET /sensor/humidity\n");

    char payload[32];
    snprintf(payload, sizeof(payload), "{\"humidity\":%.1f}", humidity);

    response->code = COAP_RESPONSE_205_CONTENT;
    uint8_t format = COAP_CONTENT_FORMAT_JSON;
    coap_pdu_add_option(response, COAP_OPTION_CONTENT_FORMAT, &format, 1);
    coap_pdu_set_payload(response, (const uint8_t *)payload, strlen(payload));

    printf("[Server] Response: %s\n", payload);
}

/* GET /actuator/led - Get LED state */
/* PUT /actuator/led - Set LED state */
void led_handler(
    coap_context_t *context,
    coap_resource_t *resource,
    const coap_pdu_t *request,
    coap_pdu_t *response,
    void *user_data
) {
    uint8_t method = request->code;

    if (method == COAP_METHOD_GET) {
        printf("[Server] GET /actuator/led\n");

        char payload[32];
        snprintf(payload, sizeof(payload), "{\"led\":\"%s\"}", led_state ? "on" : "off");

        response->code = COAP_RESPONSE_205_CONTENT;
        uint8_t format = COAP_CONTENT_FORMAT_JSON;
        coap_pdu_add_option(response, COAP_OPTION_CONTENT_FORMAT, &format, 1);
        coap_pdu_set_payload(response, (const uint8_t *)payload, strlen(payload));

        printf("[Server] Response: %s\n", payload);
    }
    else if (method == COAP_METHOD_PUT) {
        printf("[Server] PUT /actuator/led\n");

        /* Parse payload */
        if (request->payload_length > 0) {
            char payload_str[64];
            size_t len = request->payload_length < sizeof(payload_str) - 1 ?
                        request->payload_length : sizeof(payload_str) - 1;
            memcpy(payload_str, request->payload, len);
            payload_str[len] = '\0';

            printf("[Server] Payload: %s\n", payload_str);

            /* Check for "on" or "off" */
            if (strstr(payload_str, "\"on\"") || strstr(payload_str, ":1")) {
                led_state = true;
                printf("[Server] LED turned ON\n");
            } else if (strstr(payload_str, "\"off\"") || strstr(payload_str, ":0")) {
                led_state = false;
                printf("[Server] LED turned OFF\n");
            }
        }

        response->code = COAP_RESPONSE_204_CHANGED;
    }
    else {
        response->code = COAP_RESPONSE_405_METHOD_NOT_ALLOWED;
    }
}

/* POST /data - Post sensor data */
void data_handler(
    coap_context_t *context,
    coap_resource_t *resource,
    const coap_pdu_t *request,
    coap_pdu_t *response,
    void *user_data
) {
    printf("[Server] POST /data\n");

    if (request->payload_length > 0) {
        char payload_str[128];
        size_t len = request->payload_length < sizeof(payload_str) - 1 ?
                    request->payload_length : sizeof(payload_str) - 1;
        memcpy(payload_str, request->payload, len);
        payload_str[len] = '\0';

        printf("[Server] Received data: %s\n", payload_str);

        const char *resp = "{\"status\":\"ok\"}";
        response->code = COAP_RESPONSE_201_CREATED;
        uint8_t format = COAP_CONTENT_FORMAT_JSON;
        coap_pdu_add_option(response, COAP_OPTION_CONTENT_FORMAT, &format, 1);
        coap_pdu_set_payload(response, (const uint8_t *)resp, strlen(resp));
    } else {
        response->code = COAP_RESPONSE_400_BAD_REQUEST;
    }
}

/* ====================
 * CoAP Server Task
 * ==================== */

void coap_server_task(void *param) {
    printf("\n=== CoAP Server Task Started ===\n");

    /* Initialize CoAP server */
    coap_context_t server;
    coap_config_t config = {
        .bind_address = 0,  /* 0.0.0.0 - bind to all interfaces */
        .port = SERVER_PORT,
        .enable_observe = false,
        .ack_timeout_ms = COAP_ACK_TIMEOUT_MS,
        .max_retransmit = COAP_MAX_RETRANSMIT
    };

    if (coap_init(&server, &config, true) != COAP_OK) {
        printf("[Server] Failed to initialize CoAP\n");
        return;
    }

    if (coap_start(&server) != COAP_OK) {
        printf("[Server] Failed to start CoAP server\n");
        return;
    }

    printf("[Server] CoAP server listening on port %d\n", SERVER_PORT);

    /* Register resources */
    coap_resource_create(&server, "/sensor/temperature", temperature_handler, NULL);
    coap_resource_create(&server, "/sensor/humidity", humidity_handler, NULL);
    coap_resource_create(&server, "/actuator/led", led_handler, NULL);
    coap_resource_create(&server, "/data", data_handler, NULL);

    printf("[Server] Registered resources:\n");
    printf("  - GET  /sensor/temperature\n");
    printf("  - GET  /sensor/humidity\n");
    printf("  - GET  /actuator/led\n");
    printf("  - PUT  /actuator/led\n");
    printf("  - POST /data\n");
    printf("\n");

    /* Process incoming requests */
    while (1) {
        coap_error_t err = coap_process(&server, 1000);
        if (err == COAP_ERROR_TIMEOUT) {
            /* No message received, continue */
        } else if (err != COAP_OK) {
            printf("[Server] Error processing: %s\n", coap_error_to_string(err));
        }

        /* Simulate sensor updates */
        temperature += (rand() % 20 - 10) / 10.0f;
        if (temperature < 20.0f) temperature = 20.0f;
        if (temperature > 30.0f) temperature = 30.0f;

        humidity += (rand() % 10 - 5) / 10.0f;
        if (humidity < 40.0f) humidity = 40.0f;
        if (humidity > 80.0f) humidity = 80.0f;

        os_task_delay(100);
    }

    coap_stop(&server);
}

/* ====================
 * CoAP Client Task
 * ==================== */

void coap_client_task(void *param) {
    printf("\n=== CoAP Client Task Started ===\n");

    /* Wait for server to start */
    os_task_delay(2000);

    /* Initialize CoAP client */
    coap_context_t client;
    coap_config_t config = {
        .bind_address = CLIENT_IP,
        .port = 0,  /* Random port */
        .enable_observe = false,
        .ack_timeout_ms = COAP_ACK_TIMEOUT_MS,
        .max_retransmit = COAP_MAX_RETRANSMIT
    };

    if (coap_init(&client, &config, false) != COAP_OK) {
        printf("[Client] Failed to initialize CoAP\n");
        return;
    }

    if (coap_start(&client) != COAP_OK) {
        printf("[Client] Failed to start CoAP client\n");
        return;
    }

    printf("[Client] CoAP client started\n\n");

    while (1) {
        coap_response_t response;
        coap_error_t err;

        /* Test 1: GET temperature */
        printf("\n[Client] --- Test 1: GET /sensor/temperature ---\n");
        memset(&response, 0, sizeof(response));
        err = coap_get(&client, SERVER_IP, SERVER_PORT, "/sensor/temperature", &response, 5000);

        if (err == COAP_OK) {
            printf("[Client] Response code: %s\n", coap_response_code_to_string(response.code));
            if (response.payload && response.payload_length > 0) {
                printf("[Client] Payload: %.*s\n", response.payload_length, response.payload);
                coap_response_free(&response);
            }
        } else {
            printf("[Client] Error: %s\n", coap_error_to_string(err));
        }

        os_task_delay(2000);

        /* Test 2: GET humidity */
        printf("\n[Client] --- Test 2: GET /sensor/humidity ---\n");
        memset(&response, 0, sizeof(response));
        err = coap_get(&client, SERVER_IP, SERVER_PORT, "/sensor/humidity", &response, 5000);

        if (err == COAP_OK) {
            printf("[Client] Response code: %s\n", coap_response_code_to_string(response.code));
            if (response.payload && response.payload_length > 0) {
                printf("[Client] Payload: %.*s\n", response.payload_length, response.payload);
                coap_response_free(&response);
            }
        } else {
            printf("[Client] Error: %s\n", coap_error_to_string(err));
        }

        os_task_delay(2000);

        /* Test 3: PUT LED on */
        printf("\n[Client] --- Test 3: PUT /actuator/led (turn on) ---\n");
        const char *led_on = "{\"state\":\"on\"}";
        memset(&response, 0, sizeof(response));
        err = coap_put(&client, SERVER_IP, SERVER_PORT, "/actuator/led",
                      COAP_CONTENT_FORMAT_JSON,
                      (const uint8_t *)led_on, strlen(led_on),
                      &response, 5000);

        if (err == COAP_OK) {
            printf("[Client] Response code: %s\n", coap_response_code_to_string(response.code));
            coap_response_free(&response);
        } else {
            printf("[Client] Error: %s\n", coap_error_to_string(err));
        }

        os_task_delay(2000);

        /* Test 4: GET LED state */
        printf("\n[Client] --- Test 4: GET /actuator/led ---\n");
        memset(&response, 0, sizeof(response));
        err = coap_get(&client, SERVER_IP, SERVER_PORT, "/actuator/led", &response, 5000);

        if (err == COAP_OK) {
            printf("[Client] Response code: %s\n", coap_response_code_to_string(response.code));
            if (response.payload && response.payload_length > 0) {
                printf("[Client] Payload: %.*s\n", response.payload_length, response.payload);
                coap_response_free(&response);
            }
        } else {
            printf("[Client] Error: %s\n", coap_error_to_string(err));
        }

        os_task_delay(2000);

        /* Test 5: POST data */
        printf("\n[Client] --- Test 5: POST /data ---\n");
        const char *post_data = "{\"sensor\":\"test\",\"value\":42}";
        memset(&response, 0, sizeof(response));
        err = coap_post(&client, SERVER_IP, SERVER_PORT, "/data",
                       COAP_CONTENT_FORMAT_JSON,
                       (const uint8_t *)post_data, strlen(post_data),
                       &response, 5000);

        if (err == COAP_OK) {
            printf("[Client] Response code: %s\n", coap_response_code_to_string(response.code));
            if (response.payload && response.payload_length > 0) {
                printf("[Client] Payload: %.*s\n", response.payload_length, response.payload);
                coap_response_free(&response);
            }
        } else {
            printf("[Client] Error: %s\n", coap_error_to_string(err));
        }

        os_task_delay(5000);
    }

    coap_stop(&client);
}

/* ====================
 * Main
 * ==================== */

int main(void) {
    printf("\n");
    printf("============================================\n");
    printf("  TinyOS CoAP Client/Server Demo\n");
    printf("============================================\n");
    printf("\n");

    /* Initialize OS */
    os_init();
    os_mem_init();

    /* Initialize network */
    net_driver_t *driver = get_loopback_driver();
    net_config_t net_config = {
        .mac = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}},
        .ip = SERVER_IP,
        .netmask = {{255, 255, 255, 0}},
        .gateway = {{192, 168, 1, 1}},
        .dns = {{8, 8, 8, 8}}
    };

    printf("Initializing network...\n");
    if (net_init(driver, &net_config) != OS_OK) {
        printf("Failed to initialize network\n");
        return -1;
    }

    if (net_start() != OS_OK) {
        printf("Failed to start network\n");
        return -1;
    }

    printf("Network started successfully\n");

    /* Create tasks */
    static tcb_t server_task, client_task;

    os_task_create(&server_task, "coap_server", coap_server_task, NULL, PRIORITY_NORMAL);
    os_task_create(&client_task, "coap_client", coap_client_task, NULL, PRIORITY_NORMAL);

    printf("Tasks created\n");
    printf("\n");

    /* Start scheduler */
    os_start();

    return 0;
}
