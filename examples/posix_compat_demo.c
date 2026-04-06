/**
 * @file posix_compat_demo.c
 * @brief Demonstrates the POSIX pthreads and socket compatibility layers
 *
 * Shows how portable POSIX code can run on TinyOS with minimal changes.
 *
 * Build (example Makefile fragment):
 *   SRCS += src/posix/posix_threads.c src/posix/posix_socket.c
 *   SRCS += examples/posix_compat_demo.c
 */

#include "tinyos/posix_threads.h"
#include "tinyos/posix_socket.h"
#include <string.h>
#include <stdio.h>

/* =========================================================================
 * Part 1: pthreads — producer/consumer with mutex + cond var
 * ========================================================================= */

#define QUEUE_SIZE 4

static int       queue[QUEUE_SIZE];
static int       q_head = 0, q_tail = 0, q_count = 0;
static pthread_mutex_t q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  q_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  q_not_full  = PTHREAD_COND_INITIALIZER;

static void queue_push(int v) {
    pthread_mutex_lock(&q_mutex);
    while (q_count == QUEUE_SIZE)
        pthread_cond_wait(&q_not_full, &q_mutex);
    queue[q_tail] = v;
    q_tail = (q_tail + 1) % QUEUE_SIZE;
    q_count++;
    pthread_cond_signal(&q_not_empty);
    pthread_mutex_unlock(&q_mutex);
}

static int queue_pop(void) {
    pthread_mutex_lock(&q_mutex);
    while (q_count == 0)
        pthread_cond_wait(&q_not_empty, &q_mutex);
    int v = queue[q_head];
    q_head = (q_head + 1) % QUEUE_SIZE;
    q_count--;
    pthread_cond_signal(&q_not_full);
    pthread_mutex_unlock(&q_mutex);
    return v;
}

static void *producer(void *arg) {
    int items = (int)(uintptr_t)arg;
    for (int i = 0; i < items; i++) {
        queue_push(i);
        os_task_delay_ms(10);
    }
    queue_push(-1);   /* sentinel: tell consumer to stop */
    return NULL;
}

static void *consumer(void *arg) {
    (void)arg;
    int sum = 0;
    for (;;) {
        int v = queue_pop();
        if (v < 0) break;
        sum += v;
    }
    return (void *)(uintptr_t)(unsigned)sum;
}

static void demo_pthreads(void) {
    pthread_t prod_tid, cons_tid;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    tinyos_pthread_attr_setpriority(&attr, PRIORITY_NORMAL);

    pthread_create(&prod_tid, &attr, producer, (void *)(uintptr_t)10);
    pthread_create(&cons_tid, &attr, consumer, NULL);

    pthread_attr_destroy(&attr);

    pthread_join(prod_tid, NULL);

    void *cons_ret = NULL;
    pthread_join(cons_tid, &cons_ret);

    /* Expected sum: 0+1+...+9 = 45 */
    (void)cons_ret;   /* use cons_ret as needed */
}

/* =========================================================================
 * Part 2: sockets — simple echo server + client
 * ========================================================================= */

#define ECHO_PORT  7

static void *echo_server_thread(void *arg) {
    (void)arg;

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;

    int reuse = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(ECHO_PORT),
        .sin_addr   = { htonl(INADDR_ANY) }
    };
    bind(srv, (struct sockaddr *)&saddr, sizeof(saddr));
    listen(srv, 2);

    struct sockaddr_in caddr;
    socklen_t clen = sizeof(caddr);
    int client = accept(srv, (struct sockaddr *)&caddr, &clen);
    if (client >= 0) {
        char buf[64];
        ssize_t n = recv(client, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            send(client, buf, (size_t)n, 0);   /* echo back */
        }
        posix_sock_close(client);
    }
    posix_sock_close(srv);
    return NULL;
}

static void *echo_client_thread(void *arg) {
    (void)arg;

    os_task_delay_ms(50);   /* let server start */

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(ECHO_PORT),
        .sin_addr   = { htonl(INADDR_LOOPBACK) }
    };
    if (connect(fd, (struct sockaddr *)&saddr, sizeof(saddr)) == 0) {
        const char *msg = "Hello, TinyOS!";
        send(fd, msg, strlen(msg), 0);

        char reply[64];
        ssize_t n = recv(fd, reply, sizeof(reply) - 1, 0);
        if (n > 0) {
            reply[n] = '\0';
            /* reply should equal msg */
        }
    }
    posix_sock_close(fd);
    return NULL;
}

static void demo_sockets(void) {
    pthread_t srv_tid, cli_tid;

    pthread_create(&srv_tid, NULL, echo_server_thread, NULL);
    pthread_create(&cli_tid, NULL, echo_client_thread, NULL);

    pthread_join(cli_tid, NULL);
    pthread_join(srv_tid, NULL);
}

/* =========================================================================
 * Part 3: UDP sendto / recvfrom
 * ========================================================================= */

static void *udp_receiver(void *arg) {
    (void)arg;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in laddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(5000),
        .sin_addr   = { htonl(INADDR_ANY) }
    };
    bind(fd, (struct sockaddr *)&laddr, sizeof(laddr));

    char buf[128];
    struct sockaddr_in sender;
    socklen_t slen = sizeof(sender);
    ssize_t n = recvfrom(fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&sender, &slen);
    if (n > 0) {
        buf[n] = '\0';
        /* echo back to sender */
        sendto(fd, buf, (size_t)n, 0,
               (struct sockaddr *)&sender, slen);
    }
    posix_sock_close(fd);
    return NULL;
}

static void *udp_sender(void *arg) {
    (void)arg;
    os_task_delay_ms(30);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;

    struct sockaddr_in dest = {
        .sin_family = AF_INET,
        .sin_port   = htons(5000),
        .sin_addr   = { htonl(INADDR_LOOPBACK) }
    };
    const char *msg = "ping";
    sendto(fd, msg, strlen(msg), 0, (struct sockaddr *)&dest, sizeof(dest));

    char reply[64];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    recvfrom(fd, reply, sizeof(reply) - 1, 0,
             (struct sockaddr *)&from, &flen);

    posix_sock_close(fd);
    return NULL;
}

static void demo_udp(void) {
    pthread_t recv_tid, send_tid;
    pthread_create(&recv_tid, NULL, udp_receiver, NULL);
    pthread_create(&send_tid, NULL, udp_sender,   NULL);
    pthread_join(send_tid, NULL);
    pthread_join(recv_tid, NULL);
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

void posix_compat_demo(void) {
    demo_pthreads();
    demo_sockets();
    demo_udp();
}
