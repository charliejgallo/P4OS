/* What aos_sshc.c and the SSH crypto ask of the HAL, on the Mac: the log and
 * the clock of the downloader's tests (tests/fetch_host/fakes.c, taken
 * whole), and plain sockets for aos_hal_tcp_*. */
#include "../fetch_host/fakes.c"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>

int aos_hal_tcp_connect(const char *host, int port, int timeout_ms)
{
    (void)timeout_ms;
    char p[8];
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *res = NULL;
    snprintf(p, sizeof p, "%d", port);
    if (getaddrinfo(host, p, &hints, &res) != 0 || !res) return AOS_TCP_ERR_DNS;
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    int rc = fd >= 0 ? connect(fd, res->ai_addr, res->ai_addrlen) : -1;
    freeaddrinfo(res);
    if (rc != 0) { if (fd >= 0) close(fd); return AOS_TCP_ERR_CONNECT; }
    return fd + 1;              /* a handle is > 0 */
}

int aos_hal_tcp_send(int handle, const void *data, int len, int timeout_ms)
{
    (void)timeout_ms;
    const char *p = data;
    int left = len;
    while (left) {
        ssize_t k = send(handle - 1, p, (size_t)left, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return AOS_TCP_ERR_CLOSED;
        p += k;
        left -= (int)k;
    }
    return len;
}

int aos_hal_tcp_recv(int handle, void *buf, int max, int timeout_ms)
{
    struct pollfd pf = { handle - 1, POLLIN, 0 };
    int r = poll(&pf, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : AOS_TCP_ERR_CLOSED;
    ssize_t k = recv(handle - 1, buf, (size_t)max, 0);
    return k > 0 ? (int)k : AOS_TCP_ERR_CLOSED;
}

void aos_hal_tcp_close(int handle) { close(handle - 1); }
