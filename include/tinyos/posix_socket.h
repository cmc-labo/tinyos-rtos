/**
 * @file posix_socket.h
 * @brief BSD/POSIX socket compatibility layer for TinyOS
 *
 * Wraps the TinyOS net_* socket API in the standard BSD socket interface
 * so that portable network code can be compiled for TinyOS with minimal
 * changes.
 *
 * Supported:
 *   socket, bind, listen, accept, connect
 *   send, recv, sendto, recvfrom
 *   setsockopt (SO_REUSEADDR, SO_RCVTIMEO, SO_SNDTIMEO)
 *   getsockopt (SO_ERROR)
 *   inet_addr, inet_ntoa, inet_pton, inet_ntop (IPv4 only)
 *   htons, htonl, ntohs, ntohl
 *
 * Limitations:
 *   - IPv4 only (AF_INET6 returns EAFNOSUPPORT)
 *   - select/poll/epoll are not supported
 *   - Non-blocking mode (O_NONBLOCK / SOCK_NONBLOCK) is not supported
 *   - close() wrapping: define TINYOS_POSIX_WRAP_CLOSE before including
 *     this header to redirect close(fd) → posix_sock_close(fd).
 *
 * Usage:
 *   #include "tinyos/posix_socket.h"
 *
 *   int fd = socket(AF_INET, SOCK_STREAM, 0);
 *   struct sockaddr_in addr = { .sin_family = AF_INET,
 *                               .sin_port   = htons(8080),
 *                               .sin_addr   = { htonl(INADDR_ANY) } };
 *   bind(fd, (struct sockaddr *)&addr, sizeof(addr));
 */

#ifndef TINYOS_POSIX_SOCKET_H
#define TINYOS_POSIX_SOCKET_H

#include "../tinyos.h"
#include "net.h"
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================
 * Primitive types
 *===========================================================================*/

#ifndef _SOCKLEN_T_DECLARED
typedef uint32_t socklen_t;
#define _SOCKLEN_T_DECLARED
#endif

#ifndef _SSIZE_T_DECLARED
typedef int32_t ssize_t;
#define _SSIZE_T_DECLARED
#endif

typedef uint32_t in_addr_t;   /**< IPv4 address in network byte order */
typedef uint16_t in_port_t;   /**< Port number in network byte order  */

/*===========================================================================
 * Address families and socket types
 *===========================================================================*/

#define AF_UNSPEC    0
#define AF_INET      2         /**< IPv4 */

/* socket() type — values intentionally match socket_type_t in net.h */
#define SOCK_STREAM  1         /**< TCP */
#define SOCK_DGRAM   2         /**< UDP */

#define IPPROTO_IP   0
#define IPPROTO_TCP  6
#define IPPROTO_UDP  17

/*===========================================================================
 * Special address constants
 *===========================================================================*/

#define INADDR_ANY       ((in_addr_t)0x00000000UL)
#define INADDR_BROADCAST ((in_addr_t)0xFFFFFFFFUL)
#define INADDR_NONE      ((in_addr_t)0xFFFFFFFFUL)
#define INADDR_LOOPBACK  ((in_addr_t)0x7F000001UL)

/*===========================================================================
 * Socket address structures
 *===========================================================================*/

/** Generic socket address (opaque carrier). */
struct sockaddr {
    uint16_t sa_family;
    char     sa_data[14];
};

/** IPv4 internet address. */
struct in_addr {
    in_addr_t s_addr;   /**< Network byte order */
};

/** IPv4 socket address (AF_INET). */
struct sockaddr_in {
    uint16_t       sin_family;  /**< AF_INET               */
    in_port_t      sin_port;    /**< Port, network byte order */
    struct in_addr sin_addr;    /**< IP,   network byte order */
    uint8_t        sin_zero[8]; /**< Padding (must be zero)   */
};

/** IPv4 storage (compatible with sockaddr, large enough for any address). */
struct sockaddr_storage {
    uint16_t ss_family;
    uint8_t  _pad[26];
};

/*===========================================================================
 * Byte-order conversion (little-endian ARM / RISC-V)
 *===========================================================================*/

static inline uint16_t htons(uint16_t h) {
    return (uint16_t)(((h & 0x00FFU) << 8) | ((h & 0xFF00U) >> 8));
}
static inline uint16_t ntohs(uint16_t n) { return htons(n); }

static inline uint32_t htonl(uint32_t h) {
    return ((h & 0x000000FFUl) << 24) |
           ((h & 0x0000FF00Ul) <<  8) |
           ((h & 0x00FF0000Ul) >>  8) |
           ((h & 0xFF000000Ul) >> 24);
}
static inline uint32_t ntohl(uint32_t n) { return htonl(n); }

/*===========================================================================
 * Socket option levels and names
 *===========================================================================*/

#define SOL_SOCKET   1

#define SO_DEBUG      1
#define SO_REUSEADDR  2
#define SO_TYPE       3
#define SO_ERROR      4
#define SO_KEEPALIVE  9
#define SO_RCVTIMEO  20
#define SO_SNDTIMEO  21

/*===========================================================================
 * send / recv flags
 *===========================================================================*/

#define MSG_PEEK      0x02
#define MSG_WAITALL   0x08
#define MSG_DONTWAIT  0x40  /**< Treated as a hint; actual non-block unsupported */

/*===========================================================================
 * Socket API
 *===========================================================================*/

/**
 * @brief Create a socket.
 * @param domain    AF_INET only.
 * @param type      SOCK_STREAM or SOCK_DGRAM.
 * @param protocol  IPPROTO_TCP, IPPROTO_UDP, or 0 (auto).
 * @return File descriptor ≥ 0, or -1 on error (errno set).
 */
int socket(int domain, int type, int protocol);

/**
 * @brief Assign a local address to a socket.
 */
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
 * @brief Mark a socket as passive (server, TCP only).
 */
int listen(int sockfd, int backlog);

/**
 * @brief Accept an incoming connection (TCP only, blocks).
 * @param addr     Remote address is written here if non-NULL.
 * @param addrlen  In/out: size of @p addr buffer.
 * @return New connected socket fd, or -1 on error.
 */
int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);

/**
 * @brief Initiate a connection to a remote address (TCP only, blocks).
 */
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/**
 * @brief Send data on a connected socket.
 * @param flags  Only MSG_DONTWAIT is recognised (as a hint; blocking is used).
 * @return Bytes sent, or -1 on error.
 */
ssize_t send(int sockfd, const void *buf, size_t len, int flags);

/**
 * @brief Receive data from a connected socket.
 * @return Bytes received, 0 on peer close, or -1 on error.
 */
ssize_t recv(int sockfd, void *buf, size_t len, int flags);

/**
 * @brief Send a datagram to a specific address (UDP).
 */
ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);

/**
 * @brief Receive a datagram and record the sender's address (UDP).
 */
ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);

/**
 * @brief Set socket option.
 *
 * Recognised options:
 *   SOL_SOCKET / SO_REUSEADDR  — accepted, no-op (always reusable)
 *   SOL_SOCKET / SO_RCVTIMEO  — sets receive timeout (struct timeval, ms)
 *   SOL_SOCKET / SO_SNDTIMEO  — sets send timeout (struct timeval, ms)
 *
 * All other options return -1 / ENOPROTOOPT.
 */
int setsockopt(int sockfd, int level, int optname,
               const void *optval, socklen_t optlen);

/**
 * @brief Get socket option.
 *
 * Recognised options:
 *   SOL_SOCKET / SO_TYPE   — returns SOCK_STREAM or SOCK_DGRAM
 *   SOL_SOCKET / SO_ERROR  — returns and clears last error (always 0)
 */
int getsockopt(int sockfd, int level, int optname,
               void *optval, socklen_t *optlen);

/**
 * @brief Close a socket and release its resources.
 * @note  If you need to use the standard close() name, define
 *        TINYOS_POSIX_WRAP_CLOSE before including this header.
 */
int posix_sock_close(int sockfd);

#ifdef TINYOS_POSIX_WRAP_CLOSE
#define close(fd)  posix_sock_close(fd)
#endif

/*===========================================================================
 * Address conversion utilities
 *===========================================================================*/

/**
 * @brief Convert dotted-decimal string to network-byte-order IPv4 address.
 * @return Packed address in network byte order, or INADDR_NONE on error.
 */
in_addr_t inet_addr(const char *cp);

/**
 * @brief Convert network-byte-order address to dotted-decimal string.
 * @note  Uses a static buffer — not reentrant.
 */
char *inet_ntoa(struct in_addr in);

/**
 * @brief Convert presentation string to binary address (IPv4 only).
 * @param af   AF_INET only.
 * @return 1 on success, 0 if string is invalid, -1 if af not supported.
 */
int inet_pton(int af, const char *src, void *dst);

/**
 * @brief Convert binary address to presentation string (IPv4 only).
 * @param af   AF_INET only.
 * @return @p dst on success, NULL on error.
 */
const char *inet_ntop(int af, const void *src, char *dst, socklen_t size);

#ifdef __cplusplus
}
#endif

#endif /* TINYOS_POSIX_SOCKET_H */
