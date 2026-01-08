/**
 * @file coap.h
 * @brief CoAP (Constrained Application Protocol) Client/Server for TinyOS
 *
 * RFC 7252 compliant implementation
 * Lightweight protocol for IoT and constrained devices
 */

#ifndef TINYOS_COAP_H
#define TINYOS_COAP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CoAP version */
#define COAP_VERSION                1

/* CoAP default port */
#define COAP_DEFAULT_PORT           5683
#define COAPS_DEFAULT_PORT          5684

/* CoAP message size limits */
#define COAP_MAX_PDU_SIZE           1152
#define COAP_MAX_TOKEN_LEN          8
#define COAP_MAX_OPTION_COUNT       16
#define COAP_MAX_PAYLOAD_SIZE       1024

/* CoAP timeout and retry */
#define COAP_ACK_TIMEOUT_MS         2000
#define COAP_MAX_RETRANSMIT         4
#define COAP_ACK_RANDOM_FACTOR      1.5

/* CoAP Message Types */
typedef enum {
    COAP_TYPE_CON = 0,     /* Confirmable */
    COAP_TYPE_NON = 1,     /* Non-confirmable */
    COAP_TYPE_ACK = 2,     /* Acknowledgement */
    COAP_TYPE_RST = 3      /* Reset */
} coap_msg_type_t;

/* CoAP Method Codes */
typedef enum {
    COAP_METHOD_GET    = 1,
    COAP_METHOD_POST   = 2,
    COAP_METHOD_PUT    = 3,
    COAP_METHOD_DELETE = 4
} coap_method_t;

/* CoAP Response Codes */
typedef enum {
    /* Success 2.xx */
    COAP_RESPONSE_201_CREATED              = 65,   /* 2.01 */
    COAP_RESPONSE_202_DELETED              = 66,   /* 2.02 */
    COAP_RESPONSE_203_VALID                = 67,   /* 2.03 */
    COAP_RESPONSE_204_CHANGED              = 68,   /* 2.04 */
    COAP_RESPONSE_205_CONTENT              = 69,   /* 2.05 */

    /* Client Error 4.xx */
    COAP_RESPONSE_400_BAD_REQUEST          = 128,  /* 4.00 */
    COAP_RESPONSE_401_UNAUTHORIZED         = 129,  /* 4.01 */
    COAP_RESPONSE_402_BAD_OPTION           = 130,  /* 4.02 */
    COAP_RESPONSE_403_FORBIDDEN            = 131,  /* 4.03 */
    COAP_RESPONSE_404_NOT_FOUND            = 132,  /* 4.04 */
    COAP_RESPONSE_405_METHOD_NOT_ALLOWED   = 133,  /* 4.05 */
    COAP_RESPONSE_406_NOT_ACCEPTABLE       = 134,  /* 4.06 */
    COAP_RESPONSE_412_PRECONDITION_FAILED  = 140,  /* 4.12 */
    COAP_RESPONSE_413_REQUEST_TOO_LARGE    = 141,  /* 4.13 */
    COAP_RESPONSE_415_UNSUPPORTED_FORMAT   = 143,  /* 4.15 */

    /* Server Error 5.xx */
    COAP_RESPONSE_500_INTERNAL_ERROR       = 160,  /* 5.00 */
    COAP_RESPONSE_501_NOT_IMPLEMENTED      = 161,  /* 5.01 */
    COAP_RESPONSE_502_BAD_GATEWAY          = 162,  /* 5.02 */
    COAP_RESPONSE_503_SERVICE_UNAVAILABLE  = 163,  /* 5.03 */
    COAP_RESPONSE_504_GATEWAY_TIMEOUT      = 164,  /* 5.04 */
    COAP_RESPONSE_505_PROXYING_NOT_SUPPORTED = 165 /* 5.05 */
} coap_response_code_t;

/* CoAP Option Numbers */
typedef enum {
    COAP_OPTION_IF_MATCH       = 1,
    COAP_OPTION_URI_HOST       = 3,
    COAP_OPTION_ETAG           = 4,
    COAP_OPTION_IF_NONE_MATCH  = 5,
    COAP_OPTION_OBSERVE        = 6,
    COAP_OPTION_URI_PORT       = 7,
    COAP_OPTION_LOCATION_PATH  = 8,
    COAP_OPTION_URI_PATH       = 11,
    COAP_OPTION_CONTENT_FORMAT = 12,
    COAP_OPTION_MAX_AGE        = 14,
    COAP_OPTION_URI_QUERY      = 15,
    COAP_OPTION_ACCEPT         = 17,
    COAP_OPTION_LOCATION_QUERY = 20,
    COAP_OPTION_BLOCK2         = 23,
    COAP_OPTION_BLOCK1         = 27,
    COAP_OPTION_SIZE2          = 28,
    COAP_OPTION_PROXY_URI      = 35,
    COAP_OPTION_PROXY_SCHEME   = 39,
    COAP_OPTION_SIZE1          = 60
} coap_option_num_t;

/* CoAP Content Formats */
typedef enum {
    COAP_CONTENT_FORMAT_TEXT_PLAIN          = 0,
    COAP_CONTENT_FORMAT_LINK_FORMAT         = 40,
    COAP_CONTENT_FORMAT_XML                 = 41,
    COAP_CONTENT_FORMAT_OCTET_STREAM        = 42,
    COAP_CONTENT_FORMAT_EXI                 = 47,
    COAP_CONTENT_FORMAT_JSON                = 50,
    COAP_CONTENT_FORMAT_CBOR                = 60
} coap_content_format_t;

/* CoAP Error Codes */
typedef enum {
    COAP_OK = 0,
    COAP_ERROR_INVALID_PARAM,
    COAP_ERROR_NO_MEMORY,
    COAP_ERROR_TIMEOUT,
    COAP_ERROR_NETWORK,
    COAP_ERROR_PARSE,
    COAP_ERROR_INVALID_MESSAGE,
    COAP_ERROR_NOT_FOUND,
    COAP_ERROR_OBSERVE_FAILED,
    COAP_ERROR_MAX_RETRANSMIT
} coap_error_t;

/* Forward declarations */
typedef struct coap_context coap_context_t;
typedef struct coap_endpoint coap_endpoint_t;
typedef struct coap_pdu coap_pdu_t;
typedef struct coap_option coap_option_t;
typedef struct coap_resource coap_resource_t;

/* CoAP Option structure */
struct coap_option {
    uint16_t number;
    uint16_t length;
    const uint8_t *value;
};

/* CoAP PDU (Protocol Data Unit) */
struct coap_pdu {
    uint8_t version;                        /* CoAP version (always 1) */
    coap_msg_type_t type;                   /* Message type */
    uint8_t token_length;                   /* Token length (0-8) */
    uint8_t code;                           /* Method or response code */
    uint16_t message_id;                    /* Message ID for deduplication */
    uint8_t token[COAP_MAX_TOKEN_LEN];      /* Token */
    coap_option_t options[COAP_MAX_OPTION_COUNT];
    uint8_t option_count;
    uint8_t *payload;                       /* Payload data */
    uint16_t payload_length;                /* Payload length */
    uint8_t buffer[COAP_MAX_PDU_SIZE];      /* Internal buffer */
    uint16_t buffer_length;                 /* Used buffer length */
};

/* CoAP Request/Response structure */
typedef struct {
    coap_method_t method;
    const char *uri_path;
    const char *uri_query;
    coap_content_format_t content_format;
    const uint8_t *payload;
    uint16_t payload_length;
    uint32_t timeout_ms;
} coap_request_t;

typedef struct {
    uint8_t code;                           /* Response code */
    coap_content_format_t content_format;
    uint8_t *payload;
    uint16_t payload_length;
    bool success;
} coap_response_t;

/* CoAP resource handler callback */
typedef void (*coap_resource_handler_t)(
    coap_context_t *context,
    coap_resource_t *resource,
    const coap_pdu_t *request,
    coap_pdu_t *response,
    void *user_data
);

/* CoAP response callback */
typedef void (*coap_response_handler_t)(
    coap_context_t *context,
    const coap_response_t *response,
    void *user_data
);

/* CoAP observe notification callback */
typedef void (*coap_observe_handler_t)(
    coap_context_t *context,
    const char *uri,
    const coap_response_t *notification,
    void *user_data
);

/* CoAP Resource */
struct coap_resource {
    char uri_path[64];
    coap_resource_handler_t handler;
    void *user_data;
    bool observable;
    uint32_t max_age;
    struct coap_resource *next;
};

/* CoAP Endpoint (Client or Server) */
struct coap_endpoint {
    uint32_t ip_address;                    /* IPv4 address */
    uint16_t port;                          /* UDP port */
};

/* CoAP Context (Client/Server state) */
struct coap_context {
    coap_endpoint_t endpoint;
    int socket_fd;                          /* UDP socket */
    uint16_t next_message_id;
    coap_resource_t *resources;             /* Linked list of resources */
    coap_response_handler_t response_handler;
    coap_observe_handler_t observe_handler;
    void *user_data;
    bool is_server;
};

/* CoAP Configuration */
typedef struct {
    uint32_t bind_address;                  /* 0.0.0.0 for server */
    uint16_t port;                          /* Default: COAP_DEFAULT_PORT */
    bool enable_observe;                    /* Enable observe pattern */
    uint32_t ack_timeout_ms;                /* ACK timeout */
    uint8_t max_retransmit;                 /* Max retransmissions */
} coap_config_t;

/* ====================
 * CoAP Context Management
 * ==================== */

/**
 * @brief Initialize CoAP context
 * @param context CoAP context to initialize
 * @param config Configuration parameters
 * @param is_server True for server, false for client
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_init(coap_context_t *context, const coap_config_t *config, bool is_server);

/**
 * @brief Start CoAP context (bind socket)
 * @param context CoAP context
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_start(coap_context_t *context);

/**
 * @brief Stop CoAP context
 * @param context CoAP context
 */
void coap_stop(coap_context_t *context);

/**
 * @brief Process incoming CoAP messages (call periodically in task)
 * @param context CoAP context
 * @param timeout_ms Timeout for receiving messages
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_process(coap_context_t *context, uint32_t timeout_ms);

/* ====================
 * CoAP Client API
 * ==================== */

/**
 * @brief Send CoAP GET request
 * @param context CoAP context
 * @param server_ip Server IP address
 * @param server_port Server port
 * @param uri_path URI path (e.g., "/sensor/temperature")
 * @param response Response structure to fill
 * @param timeout_ms Timeout in milliseconds
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_get(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const char *uri_path,
    coap_response_t *response,
    uint32_t timeout_ms
);

/**
 * @brief Send CoAP POST request
 * @param context CoAP context
 * @param server_ip Server IP address
 * @param server_port Server port
 * @param uri_path URI path
 * @param content_format Content format
 * @param payload Payload data
 * @param payload_length Payload length
 * @param response Response structure to fill
 * @param timeout_ms Timeout in milliseconds
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_post(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const char *uri_path,
    coap_content_format_t content_format,
    const uint8_t *payload,
    uint16_t payload_length,
    coap_response_t *response,
    uint32_t timeout_ms
);

/**
 * @brief Send CoAP PUT request
 * @param context CoAP context
 * @param server_ip Server IP address
 * @param server_port Server port
 * @param uri_path URI path
 * @param content_format Content format
 * @param payload Payload data
 * @param payload_length Payload length
 * @param response Response structure to fill
 * @param timeout_ms Timeout in milliseconds
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_put(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const char *uri_path,
    coap_content_format_t content_format,
    const uint8_t *payload,
    uint16_t payload_length,
    coap_response_t *response,
    uint32_t timeout_ms
);

/**
 * @brief Send CoAP DELETE request
 * @param context CoAP context
 * @param server_ip Server IP address
 * @param server_port Server port
 * @param uri_path URI path
 * @param response Response structure to fill
 * @param timeout_ms Timeout in milliseconds
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_delete(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const char *uri_path,
    coap_response_t *response,
    uint32_t timeout_ms
);

/**
 * @brief Generic CoAP request
 * @param context CoAP context
 * @param server_ip Server IP address
 * @param server_port Server port
 * @param request Request parameters
 * @param response Response structure to fill
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_request(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const coap_request_t *request,
    coap_response_t *response
);

/* ====================
 * CoAP Server API
 * ==================== */

/**
 * @brief Register a CoAP resource handler
 * @param context CoAP context (server)
 * @param uri_path URI path (e.g., "/sensor/temperature")
 * @param handler Resource handler callback
 * @param user_data User data passed to handler
 * @return Resource pointer on success, NULL on error
 */
coap_resource_t *coap_resource_create(
    coap_context_t *context,
    const char *uri_path,
    coap_resource_handler_t handler,
    void *user_data
);

/**
 * @brief Make a resource observable
 * @param resource Resource to make observable
 * @param max_age Maximum age for notifications (seconds)
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_resource_set_observable(coap_resource_t *resource, uint32_t max_age);

/**
 * @brief Notify observers of a resource
 * @param context CoAP context
 * @param resource Resource to notify
 * @param payload Notification payload
 * @param payload_length Payload length
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_notify_observers(
    coap_context_t *context,
    coap_resource_t *resource,
    const uint8_t *payload,
    uint16_t payload_length
);

/* ====================
 * CoAP Observe Pattern (Client)
 * ==================== */

/**
 * @brief Start observing a resource
 * @param context CoAP context
 * @param server_ip Server IP address
 * @param server_port Server port
 * @param uri_path URI path to observe
 * @param handler Notification callback
 * @param user_data User data passed to callback
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_observe_start(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const char *uri_path,
    coap_observe_handler_t handler,
    void *user_data
);

/**
 * @brief Stop observing a resource
 * @param context CoAP context
 * @param uri_path URI path to stop observing
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_observe_stop(coap_context_t *context, const char *uri_path);

/* ====================
 * CoAP PDU Manipulation
 * ==================== */

/**
 * @brief Create a new CoAP PDU
 * @param pdu PDU structure to initialize
 * @param type Message type
 * @param code Method or response code
 * @param message_id Message ID
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_pdu_init(coap_pdu_t *pdu, coap_msg_type_t type, uint8_t code, uint16_t message_id);

/**
 * @brief Set PDU token
 * @param pdu PDU structure
 * @param token Token data
 * @param token_length Token length (0-8)
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_pdu_set_token(coap_pdu_t *pdu, const uint8_t *token, uint8_t token_length);

/**
 * @brief Add option to PDU
 * @param pdu PDU structure
 * @param option_num Option number
 * @param value Option value
 * @param length Option value length
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_pdu_add_option(coap_pdu_t *pdu, uint16_t option_num, const uint8_t *value, uint16_t length);

/**
 * @brief Set PDU payload
 * @param pdu PDU structure
 * @param payload Payload data
 * @param length Payload length
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_pdu_set_payload(coap_pdu_t *pdu, const uint8_t *payload, uint16_t length);

/**
 * @brief Get option from PDU
 * @param pdu PDU structure
 * @param option_num Option number to find
 * @return Option pointer or NULL if not found
 */
const coap_option_t *coap_pdu_get_option(const coap_pdu_t *pdu, uint16_t option_num);

/**
 * @brief Encode PDU to wire format
 * @param pdu PDU structure
 * @param buffer Output buffer
 * @param buffer_size Buffer size
 * @return Number of bytes written, or -1 on error
 */
int coap_pdu_encode(const coap_pdu_t *pdu, uint8_t *buffer, size_t buffer_size);

/**
 * @brief Decode PDU from wire format
 * @param pdu PDU structure to fill
 * @param buffer Input buffer
 * @param length Buffer length
 * @return COAP_OK on success, error code otherwise
 */
coap_error_t coap_pdu_decode(coap_pdu_t *pdu, const uint8_t *buffer, size_t length);

/* ====================
 * Utility Functions
 * ==================== */

/**
 * @brief Convert error code to string
 * @param error Error code
 * @return Error string
 */
const char *coap_error_to_string(coap_error_t error);

/**
 * @brief Convert response code to string
 * @param code Response code
 * @return Response code string (e.g., "2.05 Content")
 */
const char *coap_response_code_to_string(uint8_t code);

/**
 * @brief Free response payload (if allocated)
 * @param response Response structure
 */
void coap_response_free(coap_response_t *response);

/**
 * @brief Print CoAP PDU for debugging
 * @param pdu PDU to print
 */
void coap_pdu_print(const coap_pdu_t *pdu);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_COAP_H */
