/**
 * @file coap.c
 * @brief CoAP (Constrained Application Protocol) Implementation
 *
 * RFC 7252 compliant implementation for TinyOS
 */

#include "tinyos/coap.h"
#include "tinyos/net.h"
#include "tinyos.h"
#include <string.h>
#include <stdio.h>

/* Helper macros */
#define COAP_HEADER_SIZE        4
#define COAP_PAYLOAD_MARKER     0xFF

/* Get CoAP class from code */
#define COAP_CODE_CLASS(c)      ((c) >> 5)
#define COAP_CODE_DETAIL(c)     ((c) & 0x1F)
#define COAP_MAKE_CODE(class, detail) (((class) << 5) | (detail))

/* ====================
 * Static Helper Functions
 * ==================== */

static uint16_t coap_generate_message_id(coap_context_t *context) {
    return context->next_message_id++;
}

static void coap_generate_token(uint8_t *token, uint8_t length) {
    /* Simple token generation using system ticks */
    uint32_t tick = os_get_tick_count();
    for (uint8_t i = 0; i < length && i < COAP_MAX_TOKEN_LEN; i++) {
        token[i] = (tick >> (i * 8)) & 0xFF;
    }
}

static uint16_t coap_encode_option_delta_length(uint8_t *buffer, uint16_t value) {
    if (value < 13) {
        return value;
    } else if (value < 269) {
        if (buffer) {
            *buffer = value - 13;
        }
        return 13;
    } else {
        if (buffer) {
            buffer[0] = (value - 269) >> 8;
            buffer[1] = (value - 269) & 0xFF;
        }
        return 14;
    }
}

static uint16_t coap_decode_option_delta_length(const uint8_t **buffer, uint16_t encoded) {
    if (encoded < 13) {
        return encoded;
    } else if (encoded == 13) {
        uint8_t val = **buffer;
        (*buffer)++;
        return val + 13;
    } else if (encoded == 14) {
        uint16_t val = ((*buffer)[0] << 8) | (*buffer)[1];
        (*buffer) += 2;
        return val + 269;
    }
    return 0;
}

/* ====================
 * PDU Functions
 * ==================== */

coap_error_t coap_pdu_init(coap_pdu_t *pdu, coap_msg_type_t type, uint8_t code, uint16_t message_id) {
    if (!pdu) {
        return COAP_ERROR_INVALID_PARAM;
    }

    memset(pdu, 0, sizeof(coap_pdu_t));
    pdu->version = COAP_VERSION;
    pdu->type = type;
    pdu->code = code;
    pdu->message_id = message_id;
    pdu->token_length = 0;
    pdu->option_count = 0;
    pdu->payload = NULL;
    pdu->payload_length = 0;

    return COAP_OK;
}

coap_error_t coap_pdu_set_token(coap_pdu_t *pdu, const uint8_t *token, uint8_t token_length) {
    if (!pdu || token_length > COAP_MAX_TOKEN_LEN) {
        return COAP_ERROR_INVALID_PARAM;
    }

    if (token && token_length > 0) {
        memcpy(pdu->token, token, token_length);
    }
    pdu->token_length = token_length;

    return COAP_OK;
}

coap_error_t coap_pdu_add_option(coap_pdu_t *pdu, uint16_t option_num, const uint8_t *value, uint16_t length) {
    if (!pdu || pdu->option_count >= COAP_MAX_OPTION_COUNT) {
        return COAP_ERROR_INVALID_PARAM;
    }

    coap_option_t *option = &pdu->options[pdu->option_count++];
    option->number = option_num;
    option->length = length;
    option->value = value;

    return COAP_OK;
}

coap_error_t coap_pdu_set_payload(coap_pdu_t *pdu, const uint8_t *payload, uint16_t length) {
    if (!pdu || length > COAP_MAX_PAYLOAD_SIZE) {
        return COAP_ERROR_INVALID_PARAM;
    }

    pdu->payload = (uint8_t *)payload;
    pdu->payload_length = length;

    return COAP_OK;
}

const coap_option_t *coap_pdu_get_option(const coap_pdu_t *pdu, uint16_t option_num) {
    if (!pdu) {
        return NULL;
    }

    for (uint8_t i = 0; i < pdu->option_count; i++) {
        if (pdu->options[i].number == option_num) {
            return &pdu->options[i];
        }
    }

    return NULL;
}

int coap_pdu_encode(const coap_pdu_t *pdu, uint8_t *buffer, size_t buffer_size) {
    if (!pdu || !buffer || buffer_size < COAP_HEADER_SIZE) {
        return -1;
    }

    uint8_t *ptr = buffer;
    size_t remaining = buffer_size;

    /* Header */
    *ptr++ = (pdu->version << 6) | (pdu->type << 4) | pdu->token_length;
    *ptr++ = pdu->code;
    *ptr++ = pdu->message_id >> 8;
    *ptr++ = pdu->message_id & 0xFF;
    remaining -= COAP_HEADER_SIZE;

    /* Token */
    if (pdu->token_length > 0) {
        if (remaining < pdu->token_length) {
            return -1;
        }
        memcpy(ptr, pdu->token, pdu->token_length);
        ptr += pdu->token_length;
        remaining -= pdu->token_length;
    }

    /* Options (sorted by option number) */
    uint16_t prev_option_num = 0;
    for (uint8_t i = 0; i < pdu->option_count; i++) {
        const coap_option_t *opt = &pdu->options[i];
        uint16_t delta = opt->number - prev_option_num;

        uint8_t delta_ext[2];
        uint8_t length_ext[2];
        uint8_t delta_nibble = coap_encode_option_delta_length(delta_ext, delta);
        uint8_t length_nibble = coap_encode_option_delta_length(length_ext, opt->length);

        if (remaining < 1) {
            return -1;
        }
        *ptr++ = (delta_nibble << 4) | length_nibble;
        remaining--;

        /* Extended delta */
        if (delta_nibble == 13) {
            if (remaining < 1) return -1;
            *ptr++ = delta_ext[0];
            remaining--;
        } else if (delta_nibble == 14) {
            if (remaining < 2) return -1;
            *ptr++ = delta_ext[0];
            *ptr++ = delta_ext[1];
            remaining -= 2;
        }

        /* Extended length */
        if (length_nibble == 13) {
            if (remaining < 1) return -1;
            *ptr++ = length_ext[0];
            remaining--;
        } else if (length_nibble == 14) {
            if (remaining < 2) return -1;
            *ptr++ = length_ext[0];
            *ptr++ = length_ext[1];
            remaining -= 2;
        }

        /* Option value */
        if (opt->length > 0) {
            if (remaining < opt->length) {
                return -1;
            }
            memcpy(ptr, opt->value, opt->length);
            ptr += opt->length;
            remaining -= opt->length;
        }

        prev_option_num = opt->number;
    }

    /* Payload */
    if (pdu->payload_length > 0) {
        if (remaining < 1 + pdu->payload_length) {
            return -1;
        }
        *ptr++ = COAP_PAYLOAD_MARKER;
        memcpy(ptr, pdu->payload, pdu->payload_length);
        ptr += pdu->payload_length;
    }

    return ptr - buffer;
}

coap_error_t coap_pdu_decode(coap_pdu_t *pdu, const uint8_t *buffer, size_t length) {
    if (!pdu || !buffer || length < COAP_HEADER_SIZE) {
        return COAP_ERROR_INVALID_PARAM;
    }

    const uint8_t *ptr = buffer;

    /* Header */
    pdu->version = (*ptr >> 6) & 0x03;
    pdu->type = (*ptr >> 4) & 0x03;
    pdu->token_length = *ptr & 0x0F;
    ptr++;

    pdu->code = *ptr++;
    pdu->message_id = (*ptr << 8) | *(ptr + 1);
    ptr += 2;

    if (pdu->version != COAP_VERSION) {
        return COAP_ERROR_INVALID_MESSAGE;
    }

    /* Token */
    if (pdu->token_length > 0) {
        if (pdu->token_length > COAP_MAX_TOKEN_LEN ||
            (ptr + pdu->token_length) > (buffer + length)) {
            return COAP_ERROR_INVALID_MESSAGE;
        }
        memcpy(pdu->token, ptr, pdu->token_length);
        ptr += pdu->token_length;
    }

    /* Options and Payload */
    pdu->option_count = 0;
    uint16_t option_num = 0;

    while (ptr < buffer + length) {
        if (*ptr == COAP_PAYLOAD_MARKER) {
            /* Payload starts here */
            ptr++;
            pdu->payload_length = (buffer + length) - ptr;
            if (pdu->payload_length > 0) {
                memcpy(pdu->buffer, ptr, pdu->payload_length);
                pdu->payload = pdu->buffer;
            }
            break;
        }

        /* Parse option */
        if (pdu->option_count >= COAP_MAX_OPTION_COUNT) {
            return COAP_ERROR_INVALID_MESSAGE;
        }

        uint8_t delta_nibble = (*ptr >> 4) & 0x0F;
        uint8_t length_nibble = *ptr & 0x0F;
        ptr++;

        uint16_t delta = coap_decode_option_delta_length(&ptr, delta_nibble);
        uint16_t opt_length = coap_decode_option_delta_length(&ptr, length_nibble);

        option_num += delta;

        if ((ptr + opt_length) > (buffer + length)) {
            return COAP_ERROR_INVALID_MESSAGE;
        }

        coap_option_t *opt = &pdu->options[pdu->option_count++];
        opt->number = option_num;
        opt->length = opt_length;
        opt->value = ptr;
        ptr += opt_length;
    }

    return COAP_OK;
}

/* ====================
 * Context Management
 * ==================== */

coap_error_t coap_init(coap_context_t *context, const coap_config_t *config, bool is_server) {
    if (!context || !config) {
        return COAP_ERROR_INVALID_PARAM;
    }

    memset(context, 0, sizeof(coap_context_t));
    context->endpoint.ip_address = config->bind_address;
    context->endpoint.port = config->port ? config->port : COAP_DEFAULT_PORT;
    context->next_message_id = 1;
    context->is_server = is_server;
    context->socket_fd = -1;
    context->resources = NULL;

    return COAP_OK;
}

coap_error_t coap_start(coap_context_t *context) {
    if (!context) {
        return COAP_ERROR_INVALID_PARAM;
    }

    /* Create UDP socket */
    context->socket_fd = net_socket(SOCK_DGRAM);
    if (context->socket_fd < 0) {
        return COAP_ERROR_NETWORK;
    }

    /* Bind socket */
    sockaddr_in_t addr;
    addr.addr = context->endpoint.ip_address;
    addr.port = context->endpoint.port;

    if (net_bind(context->socket_fd, &addr) != OS_OK) {
        net_close(context->socket_fd);
        context->socket_fd = -1;
        return COAP_ERROR_NETWORK;
    }

    return COAP_OK;
}

void coap_stop(coap_context_t *context) {
    if (!context) {
        return;
    }

    if (context->socket_fd >= 0) {
        net_close(context->socket_fd);
        context->socket_fd = -1;
    }

    /* Free resources */
    coap_resource_t *resource = context->resources;
    while (resource) {
        coap_resource_t *next = resource->next;
        os_free(resource);
        resource = next;
    }
    context->resources = NULL;
}

/* ====================
 * Client API
 * ==================== */

static coap_error_t coap_send_request_internal(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    coap_method_t method,
    const char *uri_path,
    coap_content_format_t content_format,
    const uint8_t *payload,
    uint16_t payload_length,
    coap_response_t *response,
    uint32_t timeout_ms
) {
    if (!context || !uri_path || !response) {
        return COAP_ERROR_INVALID_PARAM;
    }

    /* Create request PDU */
    coap_pdu_t request;
    uint16_t msg_id = coap_generate_message_id(context);
    coap_pdu_init(&request, COAP_TYPE_CON, method, msg_id);

    /* Generate token */
    uint8_t token[4];
    coap_generate_token(token, 4);
    coap_pdu_set_token(&request, token, 4);

    /* Add URI-Path option */
    const char *path = uri_path;
    if (*path == '/') path++;

    char path_segment[64];
    const char *slash = strchr(path, '/');
    while (path && *path) {
        size_t len;
        if (slash) {
            len = slash - path;
            if (len >= sizeof(path_segment)) len = sizeof(path_segment) - 1;
            memcpy(path_segment, path, len);
            path_segment[len] = '\0';
            path = slash + 1;
            slash = strchr(path, '/');
        } else {
            strncpy(path_segment, path, sizeof(path_segment) - 1);
            path_segment[sizeof(path_segment) - 1] = '\0';
            path = NULL;
        }

        if (strlen(path_segment) > 0) {
            coap_pdu_add_option(&request, COAP_OPTION_URI_PATH,
                              (const uint8_t *)path_segment, strlen(path_segment));
        }
    }

    /* Add Content-Format option for POST/PUT */
    if ((method == COAP_METHOD_POST || method == COAP_METHOD_PUT) && payload_length > 0) {
        uint8_t format = (uint8_t)content_format;
        coap_pdu_add_option(&request, COAP_OPTION_CONTENT_FORMAT, &format, 1);
        coap_pdu_set_payload(&request, payload, payload_length);
    }

    /* Encode PDU */
    uint8_t buffer[COAP_MAX_PDU_SIZE];
    int len = coap_pdu_encode(&request, buffer, sizeof(buffer));
    if (len < 0) {
        return COAP_ERROR_PARSE;
    }

    /* Send request */
    sockaddr_in_t dest_addr;
    dest_addr.addr = server_ip;
    dest_addr.port = server_port;

    int32_t sent = net_sendto(context->socket_fd, buffer, len, &dest_addr);
    if (sent < 0) {
        return COAP_ERROR_NETWORK;
    }

    /* Wait for response */
    uint8_t recv_buffer[COAP_MAX_PDU_SIZE];
    sockaddr_in_t from_addr;
    int32_t recv_len = net_recvfrom(context->socket_fd, recv_buffer, sizeof(recv_buffer), &from_addr);

    if (recv_len <= 0) {
        return COAP_ERROR_TIMEOUT;
    }

    /* Decode response */
    coap_pdu_t resp_pdu;
    if (coap_pdu_decode(&resp_pdu, recv_buffer, recv_len) != COAP_OK) {
        return COAP_ERROR_PARSE;
    }

    /* Verify token matches */
    if (resp_pdu.token_length != request.token_length ||
        memcmp(resp_pdu.token, request.token, request.token_length) != 0) {
        return COAP_ERROR_INVALID_MESSAGE;
    }

    /* Fill response structure */
    response->code = resp_pdu.code;
    response->success = (COAP_CODE_CLASS(resp_pdu.code) == 2);

    /* Get content format */
    const coap_option_t *cf_opt = coap_pdu_get_option(&resp_pdu, COAP_OPTION_CONTENT_FORMAT);
    if (cf_opt && cf_opt->length > 0) {
        response->content_format = (coap_content_format_t)cf_opt->value[0];
    } else {
        response->content_format = COAP_CONTENT_FORMAT_TEXT_PLAIN;
    }

    /* Copy payload */
    if (resp_pdu.payload_length > 0) {
        response->payload = os_malloc(resp_pdu.payload_length);
        if (response->payload) {
            memcpy(response->payload, resp_pdu.payload, resp_pdu.payload_length);
            response->payload_length = resp_pdu.payload_length;
        }
    } else {
        response->payload = NULL;
        response->payload_length = 0;
    }

    return COAP_OK;
}

coap_error_t coap_get(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const char *uri_path,
    coap_response_t *response,
    uint32_t timeout_ms
) {
    return coap_send_request_internal(context, server_ip, server_port,
                                     COAP_METHOD_GET, uri_path,
                                     COAP_CONTENT_FORMAT_TEXT_PLAIN,
                                     NULL, 0, response, timeout_ms);
}

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
) {
    return coap_send_request_internal(context, server_ip, server_port,
                                     COAP_METHOD_POST, uri_path,
                                     content_format, payload, payload_length,
                                     response, timeout_ms);
}

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
) {
    return coap_send_request_internal(context, server_ip, server_port,
                                     COAP_METHOD_PUT, uri_path,
                                     content_format, payload, payload_length,
                                     response, timeout_ms);
}

coap_error_t coap_delete(
    coap_context_t *context,
    uint32_t server_ip,
    uint16_t server_port,
    const char *uri_path,
    coap_response_t *response,
    uint32_t timeout_ms
) {
    return coap_send_request_internal(context, server_ip, server_port,
                                     COAP_METHOD_DELETE, uri_path,
                                     COAP_CONTENT_FORMAT_TEXT_PLAIN,
                                     NULL, 0, response, timeout_ms);
}

/* ====================
 * Server API
 * ==================== */

coap_resource_t *coap_resource_create(
    coap_context_t *context,
    const char *uri_path,
    coap_resource_handler_t handler,
    void *user_data
) {
    if (!context || !uri_path || !handler) {
        return NULL;
    }

    coap_resource_t *resource = os_malloc(sizeof(coap_resource_t));
    if (!resource) {
        return NULL;
    }

    memset(resource, 0, sizeof(coap_resource_t));
    strncpy(resource->uri_path, uri_path, sizeof(resource->uri_path) - 1);
    resource->handler = handler;
    resource->user_data = user_data;
    resource->observable = false;
    resource->max_age = 60;
    resource->next = context->resources;
    context->resources = resource;

    return resource;
}

coap_error_t coap_resource_set_observable(coap_resource_t *resource, uint32_t max_age) {
    if (!resource) {
        return COAP_ERROR_INVALID_PARAM;
    }

    resource->observable = true;
    resource->max_age = max_age;

    return COAP_OK;
}

static coap_resource_t *coap_find_resource(coap_context_t *context, const char *uri_path) {
    coap_resource_t *resource = context->resources;
    while (resource) {
        if (strcmp(resource->uri_path, uri_path) == 0) {
            return resource;
        }
        resource = resource->next;
    }
    return NULL;
}

coap_error_t coap_process(coap_context_t *context, uint32_t timeout_ms) {
    if (!context || context->socket_fd < 0) {
        return COAP_ERROR_INVALID_PARAM;
    }

    /* Receive message */
    uint8_t buffer[COAP_MAX_PDU_SIZE];
    sockaddr_in_t from_addr;
    int32_t recv_len = net_recvfrom(context->socket_fd, buffer, sizeof(buffer), &from_addr);

    if (recv_len <= 0) {
        return COAP_ERROR_TIMEOUT;
    }

    /* Decode PDU */
    coap_pdu_t request;
    if (coap_pdu_decode(&request, buffer, recv_len) != COAP_OK) {
        return COAP_ERROR_PARSE;
    }

    /* Process as server */
    if (context->is_server && COAP_CODE_CLASS(request.code) == 0) {
        /* Extract URI path from options */
        char uri_path[128] = "/";
        size_t path_len = 1;

        for (uint8_t i = 0; i < request.option_count; i++) {
            if (request.options[i].number == COAP_OPTION_URI_PATH) {
                if (path_len + request.options[i].length + 1 < sizeof(uri_path)) {
                    memcpy(uri_path + path_len, request.options[i].value, request.options[i].length);
                    path_len += request.options[i].length;
                    uri_path[path_len++] = '/';
                }
            }
        }
        uri_path[path_len - 1] = '\0';  /* Remove trailing slash */

        /* Find resource */
        coap_resource_t *resource = coap_find_resource(context, uri_path);

        /* Create response */
        coap_pdu_t response;
        coap_pdu_init(&response, COAP_TYPE_ACK, COAP_RESPONSE_404_NOT_FOUND, request.message_id);
        coap_pdu_set_token(&response, request.token, request.token_length);

        if (resource) {
            /* Call resource handler */
            response.code = COAP_RESPONSE_205_CONTENT;
            resource->handler(context, resource, &request, &response, resource->user_data);
        }

        /* Send response */
        uint8_t resp_buffer[COAP_MAX_PDU_SIZE];
        int len = coap_pdu_encode(&response, resp_buffer, sizeof(resp_buffer));
        if (len > 0) {
            net_sendto(context->socket_fd, resp_buffer, len, &from_addr);
        }
    }

    return COAP_OK;
}

/* ====================
 * Utility Functions
 * ==================== */

const char *coap_error_to_string(coap_error_t error) {
    switch (error) {
        case COAP_OK: return "OK";
        case COAP_ERROR_INVALID_PARAM: return "Invalid parameter";
        case COAP_ERROR_NO_MEMORY: return "No memory";
        case COAP_ERROR_TIMEOUT: return "Timeout";
        case COAP_ERROR_NETWORK: return "Network error";
        case COAP_ERROR_PARSE: return "Parse error";
        case COAP_ERROR_INVALID_MESSAGE: return "Invalid message";
        case COAP_ERROR_NOT_FOUND: return "Not found";
        default: return "Unknown error";
    }
}

const char *coap_response_code_to_string(uint8_t code) {
    switch (code) {
        case COAP_RESPONSE_201_CREATED: return "2.01 Created";
        case COAP_RESPONSE_202_DELETED: return "2.02 Deleted";
        case COAP_RESPONSE_203_VALID: return "2.03 Valid";
        case COAP_RESPONSE_204_CHANGED: return "2.04 Changed";
        case COAP_RESPONSE_205_CONTENT: return "2.05 Content";
        case COAP_RESPONSE_400_BAD_REQUEST: return "4.00 Bad Request";
        case COAP_RESPONSE_404_NOT_FOUND: return "4.04 Not Found";
        case COAP_RESPONSE_405_METHOD_NOT_ALLOWED: return "4.05 Method Not Allowed";
        case COAP_RESPONSE_500_INTERNAL_ERROR: return "5.00 Internal Server Error";
        default: return "Unknown";
    }
}

void coap_response_free(coap_response_t *response) {
    if (response && response->payload) {
        os_free(response->payload);
        response->payload = NULL;
        response->payload_length = 0;
    }
}

void coap_pdu_print(const coap_pdu_t *pdu) {
    if (!pdu) return;

    printf("CoAP PDU:\n");
    printf("  Version: %d\n", pdu->version);
    printf("  Type: %d\n", pdu->type);
    printf("  Token Length: %d\n", pdu->token_length);
    printf("  Code: %d.%02d\n", COAP_CODE_CLASS(pdu->code), COAP_CODE_DETAIL(pdu->code));
    printf("  Message ID: %u\n", pdu->message_id);
    printf("  Options: %d\n", pdu->option_count);
    printf("  Payload Length: %u\n", pdu->payload_length);
}
