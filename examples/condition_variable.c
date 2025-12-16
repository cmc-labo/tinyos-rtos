/**
 * TinyOS Condition Variable Example
 *
 * Demonstrates the use of condition variables for producer-consumer pattern.
 * This example shows:
 * - How to use condition variables with mutexes
 * - Producer-consumer synchronization
 * - Proper use of os_cond_wait(), os_cond_signal(), and os_cond_broadcast()
 */

#include "tinyos.h"
#include <stdio.h>

/* Shared buffer for producer-consumer */
#define BUFFER_SIZE 5

typedef struct {
    int buffer[BUFFER_SIZE];
    int count;              /* Number of items in buffer */
    int in;                 /* Index for producer to write */
    int out;                /* Index for consumer to read */
    mutex_t mutex;          /* Protects the buffer */
    cond_var_t not_empty;   /* Signaled when buffer has items */
    cond_var_t not_full;    /* Signaled when buffer has space */
} shared_buffer_t;

static shared_buffer_t shared_data;

/* Statistics */
static volatile uint32_t items_produced = 0;
static volatile uint32_t items_consumed = 0;

/**
 * Producer task
 * Produces items and adds them to the buffer
 */
void producer_task(void *param) {
    int producer_id = (int)(intptr_t)param;
    int item = 0;

    printf("[Producer %d] Started\n", producer_id);

    while (1) {
        /* Simulate production time */
        os_task_delay(100 + (producer_id * 50));

        /* Produce an item */
        item = (producer_id * 1000) + (item + 1) % 1000;

        /* Add to buffer */
        os_mutex_lock(&shared_data.mutex, 0);

        /* Wait if buffer is full */
        while (shared_data.count == BUFFER_SIZE) {
            printf("[Producer %d] Buffer full, waiting...\n", producer_id);
            os_cond_wait(&shared_data.not_full, &shared_data.mutex, 0);
        }

        /* Add item to buffer */
        shared_data.buffer[shared_data.in] = item;
        shared_data.in = (shared_data.in + 1) % BUFFER_SIZE;
        shared_data.count++;
        items_produced++;

        printf("[Producer %d] Produced: %d (buffer: %d/%d)\n",
               producer_id, item, shared_data.count, BUFFER_SIZE);

        /* Signal that buffer is not empty */
        os_cond_signal(&shared_data.not_empty);

        os_mutex_unlock(&shared_data.mutex);
    }
}

/**
 * Consumer task
 * Consumes items from the buffer
 */
void consumer_task(void *param) {
    int consumer_id = (int)(intptr_t)param;
    int item;

    printf("[Consumer %d] Started\n", consumer_id);

    while (1) {
        /* Remove from buffer */
        os_mutex_lock(&shared_data.mutex, 0);

        /* Wait if buffer is empty */
        while (shared_data.count == 0) {
            printf("[Consumer %d] Buffer empty, waiting...\n", consumer_id);
            os_cond_wait(&shared_data.not_empty, &shared_data.mutex, 0);
        }

        /* Remove item from buffer */
        item = shared_data.buffer[shared_data.out];
        shared_data.out = (shared_data.out + 1) % BUFFER_SIZE;
        shared_data.count--;
        items_consumed++;

        printf("[Consumer %d] Consumed: %d (buffer: %d/%d)\n",
               consumer_id, item, shared_data.count, BUFFER_SIZE);

        /* Signal that buffer is not full */
        os_cond_signal(&shared_data.not_full);

        os_mutex_unlock(&shared_data.mutex);

        /* Simulate consumption time */
        os_task_delay(200 + (consumer_id * 30));
    }
}

/**
 * Monitor task
 * Displays statistics periodically
 */
void monitor_task(void *param) {
    (void)param;

    printf("[Monitor] Started\n");

    while (1) {
        os_task_delay(2000);

        os_mutex_lock(&shared_data.mutex, 0);

        printf("\n=== Statistics ===\n");
        printf("Produced: %lu items\n", items_produced);
        printf("Consumed: %lu items\n", items_consumed);
        printf("Buffer:   %d/%d items\n", shared_data.count, BUFFER_SIZE);
        printf("==================\n\n");

        os_mutex_unlock(&shared_data.mutex);
    }
}

/**
 * Broadcast demo task
 * Demonstrates broadcast functionality
 */
void broadcast_demo_task(void *param) {
    (void)param;

    printf("[Broadcast] Started\n");

    while (1) {
        os_task_delay(5000);

        printf("[Broadcast] Broadcasting to all waiting tasks...\n");

        /* Broadcast to wake up all waiting consumers and producers */
        os_cond_broadcast(&shared_data.not_empty);
        os_cond_broadcast(&shared_data.not_full);
    }
}

/**
 * Main function
 */
int main(void) {
    printf("\n=== TinyOS Condition Variable Example ===\n");
    printf("Demonstrating Producer-Consumer Pattern\n\n");

    /* Initialize OS */
    os_init();

    /* Initialize shared buffer */
    shared_data.count = 0;
    shared_data.in = 0;
    shared_data.out = 0;
    os_mutex_init(&shared_data.mutex);
    os_cond_init(&shared_data.not_empty);
    os_cond_init(&shared_data.not_full);

    /* Create tasks */
    static tcb_t producer1, producer2, consumer1, consumer2, monitor, broadcast;

    /* Create 2 producer tasks */
    os_task_create(
        &producer1,
        "Producer-1",
        producer_task,
        (void *)1,
        PRIORITY_NORMAL
    );

    os_task_create(
        &producer2,
        "Producer-2",
        producer_task,
        (void *)2,
        PRIORITY_NORMAL
    );

    /* Create 2 consumer tasks */
    os_task_create(
        &consumer1,
        "Consumer-1",
        consumer_task,
        (void *)1,
        PRIORITY_NORMAL
    );

    os_task_create(
        &consumer2,
        "Consumer-2",
        consumer_task,
        (void *)2,
        PRIORITY_NORMAL
    );

    /* Create monitor task */
    os_task_create(
        &monitor,
        "Monitor",
        monitor_task,
        NULL,
        PRIORITY_LOW
    );

    /* Create broadcast demo task */
    os_task_create(
        &broadcast,
        "Broadcast",
        broadcast_demo_task,
        NULL,
        PRIORITY_LOW
    );

    printf("Starting scheduler...\n\n");

    /* Start the scheduler */
    os_start();

    return 0;
}
