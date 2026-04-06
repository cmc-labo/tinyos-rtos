/**
 * @file posix_socket.c
 * @brief BSD/POSIX socket compatibility layer — implementation
 *
 * Wraps the TinyOS net_* socket API in the standard BSD socket interface.
 *
 * Address conversion
 * ------------------
 * POSIX struct sockaddr_in stores the IP in network byte order (big-endian)
 * inside sin_addr.s_addr, and the port in network byte order in sin_port.
 *
 * TinyOS sockaddr_in_t stores the IP as four bytes in natural dotted-decimal
 * order (addr[0] = most-significant) and the port as a host-order uint16_t.
 *
 * The helpers from_posix_addr() / to_posix_addr() translate between the two.
 *
 * Per-socket timeout
 * ------------------
 * SO_RCVTIMEO / SO_SNDTIMEO are stored in a shadow table indexed by the
 * TinyOS socket descriptor.  0 means "block forever" (OS_WAIT_FOREVER).
 */

#include "tinyos/posix_socket.h"
#include <string.h>
#include <stdio.h>
#include <errno.h>

/*===========================================================================
 * Per-socket state (shadow table)
 *===========================================================================*/

typedef struct {
    bool     in_use;
    uint32_t rcv_timeout_ms;   /* 0 = block forever */
    uint32_t snd_timeout_ms;   /* 0 = block forever */
    int      so_type;          /* SOCK_STREAM or SOCK_DGRAM */
} sock_state_t;

static sock_state_t sock_state[NET_MAX_SOCKETS];
static mutex_t      state_mutex;
static bool         state_inited = false;

static void state_init(void) {
    if (state_inited) return;
    os_mutex_init(&state_mutex);
    memset(sock_state, 0, sizeof(sock_state));
    state_inited = true;
}

static bool fd_valid(int fd) {
    return fd >= 0 && fd < NET_MAX_SOCKETS && sock_state[fd].in_use;
}

/*===========================================================================
 * OS error → errno mapping
 *===========================================================================*/

static int map_error(os_error_t r) {
    switch (r) {
        case OS_OK:                     return 0;
        case OS_ERROR_NO_MEMORY:        return ENOMEM;
        case OS_ERROR_INVALID_PARAM:    return EINVAL;
        case OS_ERROR_TIMEOUT:          return ETIMEDOUT;
        case OS_ERROR_PERMISSION_DENIED:return EACCES;
        case OS_ERROR_NOT_IMPLEMENTED:  return ENOSYS;
        case OS_ERROR_NO_RESOURCE:      return EMFILE;
        default:                        return EIO;
    }
}

/* Set errno from an OS error and return -1. */
static int set_err(os_error_t r) {
    errno = map_error(r);
    return -1;
}

/*===========================================================================
 * Address conversion helpers
 *===========================================================================*/

/**
 * Convert a POSIX sockaddr_in to a TinyOS sockaddr_in_t.
 * sin_addr.s_addr is in network byte order; TinyOS addr[] is big-endian bytes.
 * sin_port is in network byte order; TinyOS port is host order.
 */
static void from_posix_addr(const struct sockaddr_in *posix,
                             sockaddr_in_t *tiny) {
    /* s_addr in network byte order: bytes in memory are [a, b, c, d] on any
     * endianness. Access byte-by-byte to avoid endian ambiguity. */
    const uint8_t *b = (const uint8_t *)&posix->sin_addr.s_addr;
    tiny->addr.addr[0] = b[0];
    tiny->addr.addr[1] = b[1];
    tiny->addr.addr[2] = b[2];
    tiny->addr.addr[3] = b[3];

    /* sin_port is network byte order (big-endian); convert to host order. */
    tiny->port = ntohs(posix->sin_port);
}

/**
 * Convert a TinyOS sockaddr_in_t to a POSIX sockaddr_in.
 */
static void to_posix_addr(const sockaddr_in_t *tiny,
                           struct sockaddr_in *posix) {
    memset(posix, 0, sizeof(*posix));
    posix->sin_family = AF_INET;

    uint8_t *b = (uint8_t *)&posix->sin_addr.s_addr;
    b[0] = tiny->addr.addr[0];
    b[1] = tiny->addr.addr[1];
    b[2] = tiny->addr.addr[2];
    b[3] = tiny->addr.addr[3];

    posix->sin_port = htons(tiny->port);
}

/*===========================================================================
 * socket()
 *===========================================================================*/

int socket(int domain, int type, int protocol) {
    state_init();

    if (domain != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    if (type != SOCK_STREAM && type != SOCK_DGRAM) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    (void)protocol;   /* auto-detect from type */

    net_socket_t fd = net_socket((socket_type_t)type);
    if (fd == INVALID_SOCKET) { errno = EMFILE; return -1; }

    os_mutex_lock(&state_mutex, OS_WAIT_FOREVER);
    sock_state[fd].in_use          = true;
    sock_state[fd].rcv_timeout_ms  = 0;
    sock_state[fd].snd_timeout_ms  = 0;
    sock_state[fd].so_type         = type;
    os_mutex_unlock(&state_mutex);

    return (int)fd;
}

/*===========================================================================
 * bind()
 *===========================================================================*/

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (!addr || addrlen < (socklen_t)sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    if (sin->sin_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }

    sockaddr_in_t tiny;
    from_posix_addr(sin, &tiny);
    os_error_t r = net_bind((net_socket_t)sockfd, &tiny);
    return (r == OS_OK) ? 0 : set_err(r);
}

/*===========================================================================
 * listen()
 *===========================================================================*/

int listen(int sockfd, int backlog) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    os_error_t r = net_listen((net_socket_t)sockfd, backlog);
    return (r == OS_OK) ? 0 : set_err(r);
}

/*===========================================================================
 * accept()
 *===========================================================================*/

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }

    sockaddr_in_t remote;
    net_socket_t newfd = net_accept((net_socket_t)sockfd, &remote);
    if (newfd == INVALID_SOCKET) { errno = EIO; return -1; }

    /* Inherit timeouts from listening socket. */
    os_mutex_lock(&state_mutex, OS_WAIT_FOREVER);
    if (newfd < NET_MAX_SOCKETS) {
        sock_state[newfd].in_use         = true;
        sock_state[newfd].rcv_timeout_ms = sock_state[sockfd].rcv_timeout_ms;
        sock_state[newfd].snd_timeout_ms = sock_state[sockfd].snd_timeout_ms;
        sock_state[newfd].so_type        = SOCK_STREAM;
    }
    os_mutex_unlock(&state_mutex);

    if (addr && addrlen && *addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
        to_posix_addr(&remote, (struct sockaddr_in *)addr);
        *addrlen = sizeof(struct sockaddr_in);
    }

    return (int)newfd;
}

/*===========================================================================
 * connect()
 *===========================================================================*/

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (!addr || addrlen < (socklen_t)sizeof(struct sockaddr_in)) {
        errno = EINVAL;
        return -1;
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
    if (sin->sin_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }

    sockaddr_in_t tiny;
    from_posix_addr(sin, &tiny);

    uint32_t timeout = sock_state[sockfd].snd_timeout_ms;
    os_error_t r = net_connect((net_socket_t)sockfd, &tiny, timeout);
    return (r == OS_OK) ? 0 : set_err(r);
}

/*===========================================================================
 * send() / recv()
 *===========================================================================*/

ssize_t send(int sockfd, const void *buf, size_t len, int flags) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (!buf)              { errno = EFAULT; return -1; }
    (void)flags;

    uint32_t timeout = sock_state[sockfd].snd_timeout_ms;
    int32_t n = net_send((net_socket_t)sockfd, buf, (uint16_t)len, timeout);
    if (n < 0) { errno = EIO; return -1; }
    return (ssize_t)n;
}

ssize_t recv(int sockfd, void *buf, size_t len, int flags) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (!buf)              { errno = EFAULT; return -1; }
    (void)flags;

    uint32_t timeout = sock_state[sockfd].rcv_timeout_ms;
    int32_t n = net_recv((net_socket_t)sockfd, buf, (uint16_t)len, timeout);
    if (n < 0) {
        errno = (n == (int32_t)OS_ERROR_TIMEOUT) ? ETIMEDOUT : EIO;
        return -1;
    }
    return (ssize_t)n;
}

/*===========================================================================
 * sendto() / recvfrom()
 *===========================================================================*/

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (!buf)              { errno = EFAULT; return -1; }
    (void)flags;

    if (!dest_addr || addrlen < (socklen_t)sizeof(struct sockaddr_in)) {
        /* Fall back to send() for connected sockets. */
        return send(sockfd, buf, len, flags);
    }

    const struct sockaddr_in *sin = (const struct sockaddr_in *)dest_addr;
    if (sin->sin_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }

    sockaddr_in_t tiny;
    from_posix_addr(sin, &tiny);

    int32_t n = net_sendto((net_socket_t)sockfd, buf, (uint16_t)len, &tiny);
    if (n < 0) { errno = EIO; return -1; }
    return (ssize_t)n;
}

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (!buf)              { errno = EFAULT; return -1; }
    (void)flags;

    sockaddr_in_t remote;
    int32_t n = net_recvfrom((net_socket_t)sockfd, buf, (uint16_t)len, &remote);
    if (n < 0) { errno = EIO; return -1; }

    if (src_addr && addrlen &&
        *addrlen >= (socklen_t)sizeof(struct sockaddr_in)) {
        to_posix_addr(&remote, (struct sockaddr_in *)src_addr);
        *addrlen = sizeof(struct sockaddr_in);
    }

    return (ssize_t)n;
}

/*===========================================================================
 * setsockopt() / getsockopt()
 *===========================================================================*/

/**
 * Extract a millisecond timeout from a POSIX struct timeval.
 * timeval = { tv_sec, tv_usec }; for simplicity we treat any 8-byte value
 * as two 32-bit fields even without including <sys/time.h>.
 */
static uint32_t timeval_to_ms(const void *optval, socklen_t optlen) {
    if (!optval || optlen < 8) return 0;
    uint32_t sec  = ((const uint32_t *)optval)[0];
    uint32_t usec = ((const uint32_t *)optval)[1];
    return sec * 1000U + usec / 1000U;
}

int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (level != SOL_SOCKET) { errno = ENOPROTOOPT; return -1; }

    switch (optname) {
        case SO_REUSEADDR:
            /* Always allowed in TinyOS; accept the call as a no-op. */
            return 0;

        case SO_RCVTIMEO:
            sock_state[sockfd].rcv_timeout_ms = timeval_to_ms(optval, optlen);
            return 0;

        case SO_SNDTIMEO:
            sock_state[sockfd].snd_timeout_ms = timeval_to_ms(optval, optlen);
            return 0;

        default:
            errno = ENOPROTOOPT;
            return -1;
    }
}

int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }
    if (level != SOL_SOCKET) { errno = ENOPROTOOPT; return -1; }
    if (!optval || !optlen)  { errno = EINVAL; return -1; }

    switch (optname) {
        case SO_TYPE:
            if (*optlen >= sizeof(int)) {
                *(int *)optval = sock_state[sockfd].so_type;
                *optlen = sizeof(int);
                return 0;
            }
            errno = EINVAL;
            return -1;

        case SO_ERROR:
            /* TinyOS does not expose a per-socket error flag; always 0. */
            if (*optlen >= sizeof(int)) {
                *(int *)optval = 0;
                *optlen = sizeof(int);
                return 0;
            }
            errno = EINVAL;
            return -1;

        default:
            errno = ENOPROTOOPT;
            return -1;
    }
}

/*===========================================================================
 * posix_sock_close()
 *===========================================================================*/

int posix_sock_close(int sockfd) {
    if (!fd_valid(sockfd)) { errno = EBADF; return -1; }

    os_mutex_lock(&state_mutex, OS_WAIT_FOREVER);
    sock_state[sockfd].in_use = false;
    os_mutex_unlock(&state_mutex);

    os_error_t r = net_close((net_socket_t)sockfd);
    return (r == OS_OK) ? 0 : set_err(r);
}

/*===========================================================================
 * Address conversion utilities
 *===========================================================================*/

in_addr_t inet_addr(const char *cp) {
    if (!cp) return INADDR_NONE;

    ipv4_addr_t ip;
    if (!net_parse_ipv4(cp, &ip)) return INADDR_NONE;

    /* Pack into network byte order: addr[0] is MSB. */
    uint8_t *b = (uint8_t *)&((struct { in_addr_t v; }){ .v = 0 }).v;
    in_addr_t result;
    uint8_t *r = (uint8_t *)&result;
    r[0] = ip.addr[0];
    r[1] = ip.addr[1];
    r[2] = ip.addr[2];
    r[3] = ip.addr[3];
    return result;
}

char *inet_ntoa(struct in_addr in) {
    static char buf[16];
    const uint8_t *b = (const uint8_t *)&in.s_addr;
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
             (unsigned)b[0], (unsigned)b[1],
             (unsigned)b[2], (unsigned)b[3]);
    return buf;
}

int inet_pton(int af, const char *src, void *dst) {
    if (!src || !dst) { errno = EFAULT; return -1; }
    if (af != AF_INET) { errno = EAFNOSUPPORT; return -1; }

    ipv4_addr_t ip;
    if (!net_parse_ipv4(src, &ip)) return 0;  /* invalid string */

    uint8_t *b = (uint8_t *)dst;
    b[0] = ip.addr[0];
    b[1] = ip.addr[1];
    b[2] = ip.addr[2];
    b[3] = ip.addr[3];
    return 1;
}

const char *inet_ntop(int af, const void *src, char *dst, socklen_t size) {
    if (!src || !dst) { errno = EFAULT; return NULL; }
    if (af != AF_INET) { errno = EAFNOSUPPORT; return NULL; }
    if (size < 16)     { errno = ENOSPC; return NULL; }

    const uint8_t *b = (const uint8_t *)src;
    snprintf(dst, size, "%u.%u.%u.%u",
             (unsigned)b[0], (unsigned)b[1],
             (unsigned)b[2], (unsigned)b[3]);
    return dst;
}
