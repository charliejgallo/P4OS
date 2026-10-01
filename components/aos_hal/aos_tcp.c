/*
 * AmoledOS - A TCP stream for apps, and MD5. Shared between the board and the
 * simulator, like aos_http.c: lwIP gives the board the same POSIX sockets
 * macOS gives the simulator.
 *
 * Branch rtsp. Until the camera viewer, an app reached the network only
 * through aos_hal_http_*: one request, one body, the HAL's own task. A live
 * camera is the opposite: one connection that stays open for as long as the
 * app does and delivers a few hundred kilobits a second, forever. RTSP also
 * talks back on the same socket (keep-alives), so it is not a download
 * either. What it needs is a plain socket, and this is one, with the three
 * choices an app should not have to make made here:
 *
 *   - Every call has a timeout. An app calls these from its worker, and a
 *     worker must notice aos_hal_worker_should_stop() within a second or so,
 *     so nothing in here can block forever: connect, send and receive all
 *     give up on their own.
 *   - Handles, not file descriptors. The table is small (AOS_TCP_SLOTS) and
 *     lwIP's sockets are a firmware-wide resource (CONFIG_LWIP_MAX_SOCKETS is
 *     10, shared with the portal and aos_http.c), so an app that leaks one
 *     leaks it into a table it can see, not into lwIP.
 *   - No TLS. RTSP cameras and go2rtc speak plain TCP on the LAN, and
 *     mbedTLS would take internal RAM that the H.264 decoder needs.
 *
 * MD5 lives here too because the only user is the same one: RTSP's Digest
 * authentication (RFC 2617), which every Hikvision camera asks for. mbedTLS
 * has it on both platforms.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include "mbedtls/md5.h"

/* P4OS: 10, not the watch's 4. The camera mosaic keeps one connection per
 * tile open while MQTT, Modbus TCP or the scope hold theirs; lwIP's own
 * limit (CONFIG_LWIP_MAX_SOCKETS, 32, shared with the portal's 8) still
 * applies, and a socket() that fails comes back as AOS_TCP_ERR_SLOTS too. */
#define AOS_TCP_SLOTS 10
#define SLOT_FREE     (-1)
#define SLOT_TAKEN    (-2)      /* reserved by a connect in progress */

static int s_fd[AOS_TCP_SLOTS] = { -1, -1, -1, -1, -1, -1 };

static int slot_fd(int handle)
{
    if (handle < 1 || handle > AOS_TCP_SLOTS) {
        return -1;
    }
    return s_fd[handle - 1];
}

/* Waits until the socket is readable (want_write false) or writable, for at
 * most timeout_ms. 1 = ready, 0 = timed out, -1 = error. */
static int wait_fd(int fd, bool want_write, int timeout_ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = {
        .tv_sec  = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    int r = select(fd + 1, want_write ? NULL : &set, want_write ? &set : NULL, NULL, &tv);
    if (r < 0) {
        return errno == EINTR ? 0 : -1;
    }
    return r > 0 ? 1 : 0;
}

static int connect_fd(const char *host, int port, int timeout_ms);

int aos_hal_tcp_connect(const char *host, int port, int timeout_ms)
{
    if (!host || !host[0] || port <= 0 || port > 65535) {
        return AOS_TCP_ERR_ARG;
    }
    /* The slot is reserved before the connect, atomically: callers are
     * threads (every camera of the mosaic has one, and MQTT and Modbus have
     * theirs), and two connects that picked the same free slot used to end
     * with one handle for two sockets. */
    int slot = -1;
    for (int i = 0; i < AOS_TCP_SLOTS; i++) {
        if (__sync_bool_compare_and_swap(&s_fd[i], SLOT_FREE, SLOT_TAKEN)) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return AOS_TCP_ERR_SLOTS;
    }
    int r = connect_fd(host, port, timeout_ms);
    if (r < 0) {
        s_fd[slot] = SLOT_FREE;
        return r;
    }
    s_fd[slot] = r;
    return slot + 1;
}

/* The socket, connected, or AOS_TCP_ERR_*. */
static int connect_fd(const char *host, int port, int timeout_ms)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        return AOS_TCP_ERR_DNS;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return AOS_TCP_ERR_SLOTS;
    }
    /* Non-blocking only for the connect, so it can time out. */
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int r = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (r < 0 && errno != EINPROGRESS) {
        close(fd);
        return AOS_TCP_ERR_CONNECT;
    }
    if (r < 0) {
        if (wait_fd(fd, true, timeout_ms) != 1) {
            close(fd);
            return AOS_TCP_ERR_CONNECT;
        }
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) {
            close(fd);
            return AOS_TCP_ERR_CONNECT;
        }
    }
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);

    /* Requests are small and each one waits for its answer: Nagle would
     * hold them back for nothing. */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef AOS_SIM
    /* A peer that closes while we write must be an error, not a SIGPIPE
     * that takes the whole simulator down. */
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

    return fd;
}

int aos_hal_tcp_send(int handle, const void *data, int len, int timeout_ms)
{
    int fd = slot_fd(handle);
    if (fd < 0 || !data || len < 0) {
        return AOS_TCP_ERR_ARG;
    }
    const uint8_t *p = data;
    int sent = 0;
    while (sent < len) {
        int w = wait_fd(fd, true, timeout_ms);
        if (w <= 0) {
            return w == 0 ? AOS_TCP_ERR_TIMEOUT : AOS_TCP_ERR_CLOSED;
        }
        int n = (int)send(fd, p + sent, (size_t)(len - sent), 0);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
                continue;
            }
            return AOS_TCP_ERR_CLOSED;
        }
        sent += n;
    }
    return sent;
}

int aos_hal_tcp_recv(int handle, void *buf, int max, int timeout_ms)
{
    int fd = slot_fd(handle);
    if (fd < 0 || !buf || max <= 0) {
        return AOS_TCP_ERR_ARG;
    }
    int w = wait_fd(fd, false, timeout_ms);
    if (w == 0) {
        return 0;
    }
    if (w < 0) {
        return AOS_TCP_ERR_CLOSED;
    }
    int n = (int)recv(fd, buf, (size_t)max, 0);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) {
        return 0;
    }
    return n > 0 ? n : AOS_TCP_ERR_CLOSED;
}

void aos_hal_tcp_close(int handle)
{
    int fd = slot_fd(handle);
    if (fd >= 0) {
        close(fd);
        s_fd[handle - 1] = SLOT_FREE;
    }
}

void aos_hal_md5_hex(const void *data, size_t len, char out[33])
{
    unsigned char sum[16];
    mbedtls_md5(data, len, sum);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[2 * i]     = hex[sum[i] >> 4];
        out[2 * i + 1] = hex[sum[i] & 15];
    }
    out[32] = '\0';
}
