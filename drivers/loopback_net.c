/**
 * @file loopback_net.c
 * @brief Loopback Network Driver (for testing)
 *
 * This is a simple loopback driver that echoes packets back.
 * Used for testing the network stack without real hardware.
 */

#include "tinyos/net.h"
#include <string.h>

/* Simulated packet queue */
#define QUEUE_SIZE 4

static struct {
    uint8_t data[NET_BUFFER_SIZE];
    uint16_t length;
    bool valid;
} packet_queue[QUEUE_SIZE];

static int queue_head = 0;
static int queue_tail = 0;

static mac_addr_t loopback_mac = {{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};

/*===========================================================================
 * Driver Functions
 *===========================================================================*/

static os_error_t loopback_init(void) {
    /* Initialize packet queue */
    for (int i = 0; i < QUEUE_SIZE; i++) {
        packet_queue[i].valid = false;
    }

    queue_head = 0;
    queue_tail = 0;

    return OS_OK;
}

static os_error_t loopback_send(const uint8_t *data, uint16_t length) {
    if (length > NET_BUFFER_SIZE) {
        return OS_ERR_INVALID_PARAM;
    }

    /* Add to queue */
    int next_tail = (queue_tail + 1) % QUEUE_SIZE;
    if (next_tail == queue_head) {
        return OS_ERR_NO_RESOURCE;  /* Queue full */
    }

    memcpy(packet_queue[queue_tail].data, data, length);
    packet_queue[queue_tail].length = length;
    packet_queue[queue_tail].valid = true;

    queue_tail = next_tail;

    return OS_OK;
}

static int32_t loopback_receive(uint8_t *buffer, uint16_t max_length) {
    /* Check if packet available */
    if (queue_head == queue_tail || !packet_queue[queue_head].valid) {
        return 0;  /* No packet */
    }

    /* Get packet */
    uint16_t length = packet_queue[queue_head].length;
    if (length > max_length) {
        length = max_length;
    }

    memcpy(buffer, packet_queue[queue_head].data, length);
    packet_queue[queue_head].valid = false;

    queue_head = (queue_head + 1) % QUEUE_SIZE;

    return length;
}

static void loopback_get_mac(mac_addr_t *mac) {
    *mac = loopback_mac;
}

static bool loopback_is_link_up(void) {
    return true;  /* Always up */
}

/*===========================================================================
 * Driver Interface
 *===========================================================================*/

net_driver_t loopback_driver = {
    .init = loopback_init,
    .send = loopback_send,
    .receive = loopback_receive,
    .get_mac = loopback_get_mac,
    .is_link_up = loopback_is_link_up
};

/**
 * @brief Get loopback network driver
 * @return Pointer to driver interface
 */
net_driver_t *loopback_get_driver(void) {
    return &loopback_driver;
}
