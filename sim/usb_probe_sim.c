/*
 * P4OS simulator - PROBE mode's packets (aos_hal.h, aos_hal_usb_probe_read).
 *
 * The board's probe is a CMSIS-DAP v2 vendor interface on the OTG port;
 * here the same packets go over TCP, each with its length first (two bytes,
 * little endian), so a test on the Mac (tools/swd_test/dap_tcp.py) drives
 * the Programador's probe against the emulated targets of sim/swd_sim.c.
 * The port is P4_SIM_PROBE_PORT, 4002 when not set; it listens while the
 * mode is PROBE, one client at a time.
 */
#include "aos_hal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

static int s_listen = -1, s_client = -1;

bool aos_hal_sim_probe_client(void) { return s_client >= 0; }

void aos_hal_sim_probe_close(void)
{
    if (s_client >= 0) close(s_client);
    if (s_listen >= 0) close(s_listen);
    s_client = s_listen = -1;
}

static bool probe_listen(void)
{
    if (s_listen >= 0) return true;
    const char *e = getenv("P4_SIM_PROBE_PORT");
    int port = e && atoi(e) > 0 ? atoi(e) : 4002;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 1) < 0) {
        printf("[probe] port %d: %s\n", port, strerror(errno));
        close(s);
        return false;
    }
    printf("[probe] CMSIS-DAP over TCP on 127.0.0.1:%d\n", port);
    s_listen = s;
    return true;
}

static bool read_all(int s, uint8_t *buf, int n)
{
    while (n > 0) {
        ssize_t r = recv(s, buf, (size_t)n, 0);
        if (r <= 0) return false;
        buf += r;
        n -= (int)r;
    }
    return true;
}

static void drop(void)
{
    close(s_client);
    s_client = -1;
    printf("[probe] the client left\n");
}

int aos_hal_usb_probe_read(uint8_t *buf, int cap, uint32_t timeout_ms)
{
    if (aos_hal_usb_mode() != AOS_HAL_USB_PROBE || !probe_listen()) {
        usleep(50000);
        return -1;
    }
    fd_set rs;
    FD_ZERO(&rs);
    int s = s_client >= 0 ? s_client : s_listen;
    FD_SET(s, &rs);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    if (select(s + 1, &rs, NULL, NULL, &tv) <= 0) return 0;
    if (s_client < 0) {
        s_client = accept(s_listen, NULL, NULL);
        if (s_client >= 0) {
            int one = 1;
            setsockopt(s_client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
            printf("[probe] a client\n");
        }
        return 0;
    }
    uint8_t h[2];
    if (!read_all(s_client, h, 2)) { drop(); return 0; }
    int n = h[0] | h[1] << 8;
    if (n > 512 || n > cap || !read_all(s_client, buf, n)) { drop(); return 0; }
    return n;
}

bool aos_hal_usb_probe_write(const uint8_t *buf, int len)
{
    if (s_client < 0 || len < 0 || len > 512) return false;
    uint8_t out[514] = { (uint8_t)len, (uint8_t)(len >> 8) };
    memcpy(out + 2, buf, (size_t)len);
    if (send(s_client, out, (size_t)len + 2, 0) != len + 2) { drop(); return false; }
    return true;
}

int aos_hal_usb_probe_packet(void) { return 512; }
