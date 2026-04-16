/**
 * TinyOS Enhanced Message Queue Demo
 *
 * Demonstrates all new message queue features:
 *   - True blocking send/receive (no spin delay)
 *   - send_to_front  — bypass FIFO for urgent messages
 *   - send_try / receive_try — non-blocking variants
 *   - flush          — discard all items
 *   - is_full / is_empty / get_capacity — predicates
 *   - get_stats / reset_stats — diagnostics
 */

#include "tinyos.h"
#include <stdio.h>
#include <string.h>

/* ---------- queue definition ------------------------------------------- */

#define QUEUE_CAPACITY  4
#define MSG_LEN         32

typedef struct {
    uint32_t id;
    char     text[MSG_LEN];
    uint8_t  priority;
} message_t;

static message_t  queue_buf[QUEUE_CAPACITY];
static msg_queue_t shared_queue;

/* ---------- task stacks / TCBs ------------------------------------------ */

static tcb_t producer_tcb;
static tcb_t consumer_tcb;
static tcb_t monitor_tcb;

/* ---------- helper ------------------------------------------------------ */

static void print_stats(const char *label) {
    msg_queue_stats_t s;
    if (os_queue_get_stats(&shared_queue, &s) != OS_OK) return;

    printf("[%s] count=%zu/%zu  peak=%zu  sent=%u  rcvd=%u  "
           "overflow=%u  send_wait=%u  recv_wait=%u\n",
           label,
           s.current_count, s.capacity, s.peak_count,
           s.total_sent, s.total_received,
           s.overflow_count,
           s.senders_waiting, s.receivers_waiting);
}

/* ---------- producer task ----------------------------------------------- */

static void producer_task(void *param) {
    (void)param;

    uint32_t seq = 0;
    message_t msg;

    /* 1. Fill the queue completely using blocking send. */
    printf("[PRODUCER] Filling queue (%u slots)...\n", QUEUE_CAPACITY);
    for (uint32_t i = 0; i < QUEUE_CAPACITY; i++) {
        msg.id       = seq++;
        msg.priority = 0;
        snprintf(msg.text, sizeof(msg.text), "msg-%u", msg.id);
        os_queue_send(&shared_queue, &msg, OS_WAIT_FOREVER);
        printf("[PRODUCER] sent  id=%u  count=%zu\n",
               msg.id, os_queue_get_count(&shared_queue));
    }
    print_stats("after fill");

    /* 2. Non-blocking send on a full queue → should fail immediately. */
    msg.id = seq++;
    snprintf(msg.text, sizeof(msg.text), "try-%u", msg.id);
    os_error_t err = os_queue_send_try(&shared_queue, &msg);
    printf("[PRODUCER] send_try on full queue: %s (expected NO_RESOURCE)\n",
           err == OS_ERROR_NO_RESOURCE ? "NO_RESOURCE" : "UNEXPECTED");

    /* 3. Send a high-priority URGENT message to the FRONT while full.
     *    With timeout so we don't block forever — consumer runs in parallel. */
    msg.id       = 999;
    msg.priority = 255;
    snprintf(msg.text, sizeof(msg.text), "URGENT");
    printf("[PRODUCER] sending URGENT to front (will block until slot frees)...\n");
    err = os_queue_send_to_front(&shared_queue, &msg, 500 /* ticks */);
    if (err == OS_OK)
        printf("[PRODUCER] URGENT enqueued at front\n");
    else
        printf("[PRODUCER] URGENT timed out (%d)\n", err);

    /* 4. Demonstrate flush. */
    os_task_delay(10);
    printf("[PRODUCER] flushing queue...\n");
    os_queue_flush(&shared_queue);
    print_stats("after flush");

    /* 5. Demonstrate reset_stats then show clean counters. */
    /* Re-send a few items so stats are interesting. */
    for (uint32_t i = 0; i < 2; i++) {
        msg.id = seq++;
        snprintf(msg.text, sizeof(msg.text), "post-flush-%u", msg.id);
        os_queue_send(&shared_queue, &msg, OS_WAIT_FOREVER);
    }
    print_stats("before reset_stats");
    os_queue_reset_stats(&shared_queue);
    print_stats("after reset_stats");

    printf("[PRODUCER] done.\n");
    os_task_suspend(os_task_get_current());
}

/* ---------- consumer task ----------------------------------------------- */

static void consumer_task(void *param) {
    (void)param;

    /* Let the producer fill the queue first. */
    os_task_delay(50);

    message_t msg;

    /* 1. Non-blocking receive. */
    os_error_t err = os_queue_receive_try(&shared_queue, &msg);
    if (err == OS_OK)
        printf("[CONSUMER] receive_try ok: id=%u text='%s'\n", msg.id, msg.text);
    else
        printf("[CONSUMER] receive_try: queue empty\n");

    /* 2. Peek without consuming. */
    err = os_queue_peek(&shared_queue, &msg, 100);
    if (err == OS_OK)
        printf("[CONSUMER] peek: id=%u text='%s'  count still=%zu\n",
               msg.id, msg.text, os_queue_get_count(&shared_queue));

    /* 3. Drain remaining items via blocking receive. */
    while (!os_queue_is_empty(&shared_queue)) {
        os_queue_receive(&shared_queue, &msg, OS_WAIT_FOREVER);
        printf("[CONSUMER] received id=%u text='%s'\n", msg.id, msg.text);
    }

    /* 4. Block waiting for new items (producer will flush and re-send). */
    printf("[CONSUMER] queue empty, waiting for next item (OS_WAIT_FOREVER)...\n");
    err = os_queue_receive(&shared_queue, &msg, OS_WAIT_FOREVER);
    if (err == OS_OK)
        printf("[CONSUMER] unblocked: id=%u text='%s'\n", msg.id, msg.text);

    printf("[CONSUMER] done.\n");
    os_task_suspend(os_task_get_current());
}

/* ---------- monitor task ------------------------------------------------ */

static void monitor_task(void *param) {
    (void)param;

    /* Periodically print predicates and capacity. */
    for (int i = 0; i < 5; i++) {
        os_task_delay(30);
        printf("[MONITOR] capacity=%zu  count=%zu  full=%d  empty=%d\n",
               os_queue_get_capacity(&shared_queue),
               os_queue_get_count(&shared_queue),
               os_queue_is_full(&shared_queue)  ? 1 : 0,
               os_queue_is_empty(&shared_queue) ? 1 : 0);
    }

    printf("[MONITOR] done.\n");
    os_task_suspend(os_task_get_current());
}

/* ---------- main entry point -------------------------------------------- */

int main(void) {
    os_init();

    /* Init queue (static storage, no heap allocation). */
    os_queue_init(&shared_queue, queue_buf,
                  sizeof(message_t), QUEUE_CAPACITY);

    /* Create tasks. */
    os_task_create(&producer_tcb, "producer", producer_task,
                   NULL, PRIORITY_NORMAL);
    os_task_create(&consumer_tcb, "consumer", consumer_task,
                   NULL, PRIORITY_HIGH);
    os_task_create(&monitor_tcb,  "monitor",  monitor_task,
                   NULL, PRIORITY_LOW);

    os_start();  /* never returns */
}
