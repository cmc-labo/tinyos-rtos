/**
 * TinyOS Example: IoT Sensor Node
 *
 * Demonstrates real-time sensor reading with message queues
 */

#include "tinyos.h"

/* Sensor data structure */
typedef struct {
    uint32_t timestamp;
    float temperature;
    float humidity;
    uint16_t battery_voltage;
} sensor_data_t;

/* Message queue for sensor data */
static msg_queue_t sensor_queue;
static sensor_data_t queue_buffer[10];

/* Mutex for I2C bus access */
static mutex_t i2c_mutex;

/* Task control blocks */
static tcb_t sensor_read_task;
static tcb_t data_process_task;
static tcb_t network_send_task;

/**
 * Sensor reading task (high priority for real-time)
 */
void sensor_reader(void *param) {
    (void)param;
    sensor_data_t data;

    while (1) {
        /* Lock I2C bus */
        if (os_mutex_lock(&i2c_mutex, 100) == OS_OK) {
            /* Read temperature sensor */
            data.timestamp = os_get_uptime_ms();
            data.temperature = 25.0f;  /* TODO: Real sensor reading */
            data.humidity = 60.0f;     /* TODO: Real sensor reading */
            data.battery_voltage = 3300;  /* mV */

            /* Unlock I2C bus */
            os_mutex_unlock(&i2c_mutex);

            /* Send to processing queue */
            os_queue_send(&sensor_queue, &data, 50);
        }

        /* Sample every 1 second */
        os_task_delay(1000);
    }
}

/**
 * Data processing task
 */
void data_processor(void *param) {
    (void)param;
    sensor_data_t data;

    while (1) {
        /* Receive sensor data */
        if (os_queue_receive(&sensor_queue, &data, 1000) == OS_OK) {
            /* Process data (e.g., filtering, averaging) */
            if (data.temperature > 30.0f) {
                /* Trigger alert */
            }

            /* Additional processing */
        }

        /* Yield to other tasks */
        os_task_yield();
    }
}

/**
 * Network transmission task (low priority)
 */
void network_sender(void *param) {
    (void)param;

    while (1) {
        /* Send data over network (WiFi, LoRa, etc.) */
        /* TODO: Actual network transmission */

        /* Delay 10 seconds between transmissions */
        os_task_delay(10000);
    }
}

/**
 * Main entry point
 */
int main(void) {
    /* Initialize OS */
    os_init();
    os_security_init();
    os_mem_init();

    /* Initialize I2C mutex */
    os_mutex_init(&i2c_mutex);

    /* Initialize sensor data queue */
    os_queue_init(
        &sensor_queue,
        queue_buffer,
        sizeof(sensor_data_t),
        10
    );

    /* Create sensor reading task (high priority for real-time) */
    os_task_create(
        &sensor_read_task,
        "sensor",
        sensor_reader,
        NULL,
        PRIORITY_HIGH
    );

    /* Create data processing task */
    os_task_create(
        &data_process_task,
        "process",
        data_processor,
        NULL,
        PRIORITY_NORMAL
    );

    /* Create network transmission task (low priority) */
    os_task_create(
        &network_send_task,
        "network",
        network_sender,
        NULL,
        PRIORITY_LOW
    );

    /* Start OS scheduler */
    os_start();

    return 0;
}
