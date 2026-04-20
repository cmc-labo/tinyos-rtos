/**
 * TinyOS Event Flag Logic Example
 *
 * Demonstrates the four logical-operation modes for event groups:
 *
 *   1. AND  (EVENT_WAIT_ALL)             -- wake when ALL bits are SET
 *   2. OR   (EVENT_WAIT_ANY)             -- wake when ANY bit  is SET
 *   3. NOT  (EVENT_WAIT_CLEAR)           -- wake when bits become CLEAR
 *   4. SYNC (os_event_group_sync)        -- rendezvous: every participant
 *                                           sets its bit and waits for the
 *                                           rest before continuing
 *
 * Scenario: a simple pipeline with sensor, network, and processing tasks.
 */

#include "tinyos.h"

/* ── Event bit definitions ─────────────────────────────────────────────── */

/* Condition bits (set = resource is BUSY / active) */
#define BIT_SENSOR_READY    (1u << 0)   /* sensor has fresh data          */
#define BIT_NETWORK_UP      (1u << 1)   /* network link is established    */
#define BIT_PROCESSING      (1u << 2)   /* CPU-intensive work in progress */

/* Barrier bits for the rendezvous demo (one per participating task) */
#define BIT_SYNC_PRODUCER   (1u << 8)
#define BIT_SYNC_CONSUMER   (1u << 9)
#define BIT_SYNC_LOGGER     (1u << 10)
#define BIT_SYNC_ALL        (BIT_SYNC_PRODUCER | BIT_SYNC_CONSUMER | BIT_SYNC_LOGGER)

/* ── Shared state ──────────────────────────────────────────────────────── */

static event_group_t g_events;

/* TCBs */
static tcb_t tcb_sensor;
static tcb_t tcb_upload;
static tcb_t tcb_idle_monitor;
static tcb_t tcb_producer;
static tcb_t tcb_consumer;
static tcb_t tcb_logger;

/* ── Demo 1 & 2: AND / OR ──────────────────────────────────────────────── */

/**
 * sensor_task — sets BIT_SENSOR_READY periodically, simulates data arrival.
 */
static void sensor_task(void *param) {
    (void)param;

    for (;;) {
        os_task_delay(300);

        /* Mark sensor data as available. */
        os_event_group_set_bits(&g_events, BIT_SENSOR_READY);

        /* Simulate the sensor being busy for a short window. */
        os_task_delay(50);

        /* Clear after consumption window (upload_task uses CLEAR_ON_EXIT,
         * but we clear explicitly here too to keep the demo tidy). */
        os_event_group_clear_bits(&g_events, BIT_SENSOR_READY);
    }
}

/**
 * upload_task — AND example.
 *
 * Wakes only when BOTH BIT_SENSOR_READY AND BIT_NETWORK_UP are set.
 * Both bits are cleared atomically on exit (EVENT_CLEAR_ON_EXIT).
 */
static void upload_task(void *param) {
    (void)param;
    uint32_t bits;

    /* Pretend the network comes up after a short delay. */
    os_task_delay(150);
    os_event_group_set_bits(&g_events, BIT_NETWORK_UP);

    for (;;) {
        os_error_t rc = os_event_group_wait_bits(
            &g_events,
            BIT_SENSOR_READY | BIT_NETWORK_UP,   /* mask               */
            EVENT_WAIT_ALL | EVENT_CLEAR_ON_EXIT, /* AND, auto-clear    */
            &bits,
            5000                                  /* 5 s timeout        */
        );

        if (rc == OS_OK) {
            /* Both conditions were met — upload the data. */
            (void)bits;  /* would inspect bits in real code */
            os_task_delay(20);  /* simulate upload */
            /* Restore the network bit so subsequent iterations work. */
            os_event_group_set_bits(&g_events, BIT_NETWORK_UP);
        }
        /* On timeout: just loop and wait again. */
    }
}

/* ── Demo 3: NOT (EVENT_WAIT_CLEAR) ───────────────────────────────────── */

/**
 * idle_monitor_task — CLEAR example.
 *
 * Waits until BIT_PROCESSING is CLEAR, i.e. the CPU-intensive work has
 * finished.  Uses EVENT_WAIT_ALL | EVENT_WAIT_CLEAR so the condition is
 * "BIT_PROCESSING == 0".
 */
static void idle_monitor_task(void *param) {
    (void)param;

    /* Simulate a heavy computation that starts 200 ms in. */
    os_task_delay(200);
    os_event_group_set_bits(&g_events, BIT_PROCESSING);

    /* Simulate work lasting 400 ms. */
    os_task_delay(400);
    os_event_group_clear_bits(&g_events, BIT_PROCESSING);

    /* The line below will unblock as soon as clear_bits() runs above
     * (the call itself wakes any WAIT_CLEAR waiter immediately). */
    uint32_t bits;
    os_error_t rc = os_event_group_wait_bits(
        &g_events,
        BIT_PROCESSING,
        EVENT_WAIT_ALL | EVENT_WAIT_CLEAR,   /* wake when BIT_PROCESSING == 0 */
        &bits,
        OS_WAIT_FOREVER
    );
    (void)rc;  /* OS_OK: processing is no longer active */

    /* Low-power work or housekeeping can begin here. */
    for (;;) {
        os_task_delay(1000);
    }
}

/* ── Demo 4: SYNC (rendezvous / barrier) ──────────────────────────────── */

/**
 * Three tasks (producer, consumer, logger) must reach a checkpoint together
 * before any of them proceeds.  os_event_group_sync() provides the barrier.
 *
 * Each task sets its own arrival bit and waits for the union of all three.
 */

static void producer_task(void *param) {
    (void)param;
    os_task_delay(100);   /* arrives first */

    os_event_group_sync(&g_events,
                        BIT_SYNC_PRODUCER,  /* I'm here */
                        BIT_SYNC_ALL,       /* wait for everyone */
                        OS_WAIT_FOREVER);

    /* All three tasks have now passed the barrier simultaneously. */
    /* Clear barrier bits so the group can be reused. */
    os_event_group_clear_bits(&g_events, BIT_SYNC_ALL);

    for (;;) { os_task_delay(1000); }
}

static void consumer_task(void *param) {
    (void)param;
    os_task_delay(250);   /* arrives second */

    os_event_group_sync(&g_events,
                        BIT_SYNC_CONSUMER,
                        BIT_SYNC_ALL,
                        OS_WAIT_FOREVER);

    for (;;) { os_task_delay(1000); }
}

static void logger_task(void *param) {
    (void)param;
    os_task_delay(400);   /* arrives last — releases everyone */

    os_event_group_sync(&g_events,
                        BIT_SYNC_LOGGER,
                        BIT_SYNC_ALL,
                        OS_WAIT_FOREVER);

    for (;;) { os_task_delay(1000); }
}

/* ── Main ──────────────────────────────────────────────────────────────── */

int main(void) {
    os_init();
    os_event_group_init(&g_events);

    /* Demo 1 & 2 tasks */
    os_task_create(&tcb_sensor,       "sensor",    sensor_task,       NULL, PRIORITY_HIGH);
    os_task_create(&tcb_upload,       "upload",    upload_task,       NULL, PRIORITY_NORMAL);

    /* Demo 3 task */
    os_task_create(&tcb_idle_monitor, "idle_mon",  idle_monitor_task, NULL, PRIORITY_LOW);

    /* Demo 4 tasks */
    os_task_create(&tcb_producer,     "producer",  producer_task,     NULL, PRIORITY_NORMAL);
    os_task_create(&tcb_consumer,     "consumer",  consumer_task,     NULL, PRIORITY_NORMAL);
    os_task_create(&tcb_logger,       "logger",    logger_task,       NULL, PRIORITY_NORMAL);

    os_start();
    return 0;
}
