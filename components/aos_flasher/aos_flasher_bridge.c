/*
 * P4OS - Programmer: the bridge for the computer. A job that holds a serial
 * port (the header's or a USB one) and lends it over TCP, so the tools on
 * the Mac work through the board as if the target were plugged into it:
 *
 *   4000  RFC 2217 (telnet's COM-PORT-OPTION): esptool and anything on
 *         pyserial, "--port rfc2217://p4os.local:4000". Speed, format and
 *         the DTR/RTS lines come from the client; on the header RTS is EN
 *         and DTR is BOOT (asserted = low), esptool's own reset; on a USB
 *         port they are its RTS and DTR.
 *   4001  the bytes alone, for avrdude ("-c arduino -P net:p4os.local:4001")
 *         and whatever speaks a plain socket. avrdude cannot reset a board
 *         over a socket, so each connection starts with an Arduino's reset
 *         (DTR on USB, EN on the header): the bootloader is listening when
 *         the first byte arrives. The speed is the job's (opts.baud).
 *
 * One client at a time; another is turned away while one is connected. A
 * connection is taken only from where the portal itself would open without
 * a password - the USB cable, the board's AP, a network marked as home
 * (aos_access) - since a socket has no way to log in: on any other network
 * the bridge says no. It runs until cancelled, like the RTT console.
 *
 * BSD sockets, which lwIP and the Mac both have: the simulator runs it as
 * is (P4_SIM_BRIDGE_PORT moves its two ports, so sims side by side do not
 * fight over them).
 */
#include "aos_flasher_priv.h"
#include "aos_access.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>

#define PORT_RFC2217    4000
#define PORT_RAW        4001

/* telnet */
#define IAC     255
#define DONT    254
#define DO      253
#define WONT    252
#define WILL    251
#define SB      250
#define SE      240
#define T_BINARY 0
#define T_ECHO   1
#define T_SGA    3
#define T_COMPORT 44

typedef struct {
    aos_io_uart_t *u;
    bool usb, lines;
    int lsock[2];               /* listening: RFC 2217, raw */
    int csock;                  /* the client, -1 none */
    bool rfc;                   /* the client is on the RFC 2217 port */
    uint32_t baud;
    bool dtr, rts;
    /* the telnet parser */
    int tstate;                 /* 0 data, 1 IAC, 2 option verb, 3 SB, 4 SB IAC */
    uint8_t verb;
    uint8_t sb[16];
    int sbn;
    uint8_t opt_sent[256];      /* what we said about an option, to answer once */
    uint32_t up, down;          /* bytes to the target, from it */
} br_t;

/* ---- sockets ---- */

static int listen_on(int port)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) < 0 || listen(s, 1) < 0) {
        close(s);
        return -1;
    }
    return s;
}

static bool send_all(int s, const void *buf, int n)
{
    const uint8_t *b = buf;
    while (n > 0) {
        ssize_t w = send(s, b, (size_t)n, 0);
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) { aos_hal_sleep_ms(1); continue; }
        if (w <= 0) return false;
        b += w;
        n -= (int)w;
    }
    return true;
}

/* Whether a connection that came in on this socket may stay: the address
 * it reached the board at decides, as for the portal (aos_portal_access.c) */
static bool zone_open(int s, char *why, size_t n)
{
    struct sockaddr_in me;
    socklen_t l = sizeof me;
    char ip[16] = "";
    if (getsockname(s, (struct sockaddr *)&me, &l) == 0) inet_ntop(AF_INET, &me.sin_addr, ip, sizeof ip);
    aos_zone_t z;
#ifdef AOS_SIM
    const char *e = getenv("P4_SIM_ZONE");
    z = e && !strcmp(e, "away") ? AOS_ZONE_AWAY : e && !strcmp(e, "home") ? AOS_ZONE_HOME : AOS_ZONE_CABLE;
#else
    if (!strcmp(ip, "192.168.7.1")) z = AOS_ZONE_CABLE;
    else if (!strcmp(ip, aos_hal_net_ap_ip())) z = AOS_ZONE_AP;
    else z = aos_access_here_trusted() ? AOS_ZONE_HOME : AOS_ZONE_AWAY;
#endif
    if (aos_access_need(z) == AOS_ACCESS_OPEN) return true;
    snprintf(why, n, "%s", z == AOS_ZONE_AWAY ? _("una red que no es de casa") : _("la red pide contraseña y un socket no la puede dar"));
    return false;
}

/* ---- the lines ---- */

/* RFC 2217's DTR/RTS: on the header, esptool's meaning (RTS asserted = EN
 * low, DTR asserted = BOOT low); on USB, the adapter's own lines */
static void set_lines(br_t *b)
{
    if (b->usb) aos_io_uart_dtr_rts(b->u, b->dtr, b->rts);
    else if (b->lines) aos_io_uart_lines(b->u, b->rts ? 0 : 1, b->dtr ? 0 : 1);
}

/* An Arduino's reset, for the raw port: DTR off and on (USB), EN low and
 * released (the header) */
static void arduino_reset(br_t *b)
{
    if (b->usb) {
        aos_io_uart_dtr_rts(b->u, false, false);
        aos_hal_sleep_ms(250);
        aos_io_uart_dtr_rts(b->u, true, true);
        aos_hal_sleep_ms(50);
    } else if (b->lines) {
        aos_io_uart_lines(b->u, 0, -1);
        aos_hal_sleep_ms(50);
        aos_io_uart_lines(b->u, 1, -1);
    }
}

/* ---- RFC 2217 ---- */

static void tn_send(br_t *b, uint8_t verb, uint8_t opt)
{
    uint8_t m[3] = { IAC, verb, opt };
    send_all(b->csock, m, 3);
}

/* What we take: binary, suppress-go-ahead, the COM port option */
static bool tn_ok(uint8_t opt) { return opt == T_BINARY || opt == T_SGA || opt == T_COMPORT; }

static void tn_option(br_t *b, uint8_t verb, uint8_t opt)
{
    /* answer each request once, or two peers echo each other forever */
    uint8_t reply = verb == WILL ? (tn_ok(opt) ? DO : DONT) : verb == DO ? (tn_ok(opt) ? WILL : WONT)
                  : verb == WONT ? DONT : WONT;
    uint8_t key = (uint8_t)(verb == WILL || verb == WONT ? 1 : 2);
    if ((b->opt_sent[opt] & key) && (verb == WILL || verb == DO)) return;
    b->opt_sent[opt] |= key;
    tn_send(b, reply, opt);
}

/* A COM-PORT-OPTION subnegotiation: done, and answered with its code +100 */
static void comport(br_t *b)
{
    if (b->sbn < 2 || b->sb[0] != T_COMPORT) return;
    uint8_t c = b->sb[1], *v = b->sb + 2;
    int vn = b->sbn - 2;
    uint8_t ans[8];
    int an = 0;
    switch (c) {
    case 1: {   /* SET-BAUDRATE, 0 asks for the current one */
        uint32_t baud = vn >= 4 ? (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3] : 0;
        if (baud && baud != b->baud && aos_io_uart_set_baud(b->u, baud)) {
            b->baud = baud;
            fl_logf(L_INFO, _("Puente: %u baudios"), (unsigned)baud);
            fl_lock();
            fl_st.baud = baud;
            fl_unlock();
        }
        ans[an++] = (uint8_t)(b->baud >> 24); ans[an++] = (uint8_t)(b->baud >> 16);
        ans[an++] = (uint8_t)(b->baud >> 8); ans[an++] = (uint8_t)b->baud;
        break;
    }
    case 2: ans[an++] = 8; break;                           /* SET-DATASIZE: eight */
    case 3: {   /* SET-PARITY: 1 none, 2 odd, 3 even */
        uint8_t p = vn ? v[0] : 0;
        if (p >= 1 && p <= 3) aos_io_uart_set_format(b->u, p == 2 ? 'O' : p == 3 ? 'E' : 'N', 1);
        ans[an++] = p ? p : 1;
        break;
    }
    case 4: ans[an++] = vn && v[0] ? v[0] : 1; break;       /* SET-STOPSIZE */
    case 5: {   /* SET-CONTROL */
        uint8_t x = vn ? v[0] : 0;
        if (x == 8 || x == 9) { b->dtr = x == 8; set_lines(b); }
        else if (x == 11 || x == 12) { b->rts = x == 11; set_lines(b); }
        ans[an++] = x == 0 ? 1 : x == 7 ? (b->dtr ? 8 : 9) : x == 13 ? (b->rts ? 11 : 12) : x;
        break;
    }
    case 10: case 11: case 12: ans[an++] = vn ? v[0] : 0; break;   /* the masks, PURGE-DATA */
    default: return;                                        /* SIGNATURE, notifies: nothing to say */
    }
    uint8_t m[20] = { IAC, SB, T_COMPORT, (uint8_t)(c + 100) };
    int k = 4;
    for (int i = 0; i < an; i++) {
        m[k++] = ans[i];
        if (ans[i] == IAC) m[k++] = IAC;
    }
    m[k++] = IAC;
    m[k++] = SE;
    send_all(b->csock, m, k);
}

/* What the client sent: its data to the port, its telnet to us */
static void from_client(br_t *b, const uint8_t *in, int n)
{
    uint8_t out[512];
    int k = 0;
    for (int i = 0; i < n; i++) {
        uint8_t c = in[i];
        if (!b->rfc) { out[k++] = c; }
        else switch (b->tstate) {
        case 0: if (c == IAC) b->tstate = 1; else out[k++] = c; break;
        case 1:
            if (c == IAC) { out[k++] = IAC; b->tstate = 0; }
            else if (c >= WILL) { b->verb = c; b->tstate = 2; }
            else if (c == SB) { b->sbn = 0; b->tstate = 3; }
            else b->tstate = 0;
            break;
        case 2: tn_option(b, b->verb, c); b->tstate = 0; break;
        case 3:
            if (c == IAC) b->tstate = 4;
            else if (b->sbn < (int)sizeof b->sb) b->sb[b->sbn++] = c;
            break;
        case 4:
            if (c == SE) { comport(b); b->tstate = 0; }
            else { if (c == IAC && b->sbn < (int)sizeof b->sb) b->sb[b->sbn++] = IAC; b->tstate = 3; }
            break;
        }
        if (k == (int)sizeof out) { aos_io_uart_write(b->u, out, k); b->up += (uint32_t)k; k = 0; }
    }
    if (k) {
        for (int done = 0; done < k;) {
            int w = aos_io_uart_write(b->u, out + done, k - done);
            if (w < 0) break;
            done += w;
            if (done < k) aos_hal_sleep_ms(1);
        }
        b->up += (uint32_t)k;
    }
}

/* What the port said, to the client: IAC doubled on the telnet port */
static bool to_client(br_t *b, const uint8_t *in, int n)
{
    if (!b->rfc) return send_all(b->csock, in, n);
    uint8_t out[1100];
    int k = 0;
    for (int i = 0; i < n; i++) {
        out[k++] = in[i];
        if (in[i] == IAC) out[k++] = IAC;
    }
    return send_all(b->csock, out, k);
}

/* ---- the job ---- */

static void drop_client(br_t *b, const char *why)
{
    if (b->csock < 0) return;
    close(b->csock);
    b->csock = -1;
    fl_logf(L_INFO, _("Puente: se fue el cliente (%s); %u bytes al objetivo, %u de vuelta"), why, (unsigned)b->up, (unsigned)b->down);
}

static void take_client(br_t *b, int which)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    int s = accept(b->lsock[which], (struct sockaddr *)&a, &l);
    if (s < 0) return;
    char ip[16] = "", why[96] = "";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    if (b->csock >= 0) {
        fl_logf(L_WARN, _("Puente: %s quiso entrar y ya hay alguien"), ip);
        close(s);
        return;
    }
    if (!zone_open(s, why, sizeof why)) {
        fl_logf(L_WARN, _("Puente: %s rechazado: %s"), ip, why);
        close(s);
        return;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    b->csock = s;
    b->rfc = which == 0;
    b->tstate = 0;
    b->up = b->down = 0;
    memset(b->opt_sent, 0, sizeof b->opt_sent);
    fl_logf(L_OK, _("Puente: %s conectado (%s)"), ip, b->rfc ? "RFC 2217" : _("crudo"));
    if (b->rfc) {
        /* what we are ready for, without waiting to be asked */
        tn_send(b, WILL, T_COMPORT);
        tn_send(b, WILL, T_BINARY);
        tn_send(b, DO, T_BINARY);
        b->opt_sent[T_COMPORT] |= 2;
        b->opt_sent[T_BINARY] |= 3;
    } else {
        arduino_reset(b);
    }
}

static int base_port(void)
{
#ifdef AOS_SIM
    const char *e = getenv("P4_SIM_BRIDGE_PORT");
    if (e && atoi(e) > 0) return atoi(e);
#endif
    return PORT_RFC2217;
}

static bool run(br_t *b)
{
    fl_phase(AOS_FLASHER_OPENING);
    const aos_io_port_t *pt = aos_io_port_find(J.port);
    b->usb = !strncmp(J.port, "usb", 3);
    if (!b->usb && (!pt || pt->kind != AOS_PORT_UART)) return fl_fail(_("No hay un puerto serie %s"), J.port);
    b->baud = J.opts.baud ? J.opts.baud : 115200;
    b->u = aos_io_uart_open(J.port, b->baud, false, AOS_FLASHER_OWNER);
    if (!b->u) {
        if (b->usb) return fl_fail(_("No se pudo abrir %s: ¿está enchufado, o abierto en la Terminal?"), J.port);
        const char *o = aos_io_owner(pt->pins[0]);
        if (!o) o = aos_io_owner(pt->pins[1]);
        return o ? fl_fail(_("%s está ocupado: lo tiene %s"), J.port, o) : fl_fail(_("No se pudo abrir %s"), J.port);
    }
    b->lines = b->usb || aos_io_uart_lines(b->u, -1, -1);
    int p0 = base_port();
    b->lsock[0] = listen_on(p0);
    b->lsock[1] = listen_on(p0 + 1);
    if (b->lsock[0] < 0 || b->lsock[1] < 0) return fl_fail(_("No se pudieron abrir los puertos %d y %d"), p0, p0 + 1);
    const char *name = aos_hal_device_name();
    fl_logf(L_OK, _("Puente abierto en %s a %u baudios"), aos_io_uart_desc(b->u), (unsigned)b->baud);
    fl_logf(L_INFO, "esptool --port rfc2217://%s.local:%d", name && *name ? name : "p4os", p0);
    fl_logf(L_INFO, "avrdude -c arduino -P net:%s.local:%d", name && *name ? name : "p4os", p0 + 1);
    fl_lock();
    fl_st.baud = b->baud;
    fl_st.lines = b->lines;
    fl_unlock();
    fl_phase(AOS_FLASHER_BRIDGE);
    uint8_t buf[1024];
    while (!fl_cancel) {
        fd_set rd;
        FD_ZERO(&rd);
        int top = -1;
        for (int i = 0; i < 2; i++) { FD_SET(b->lsock[i], &rd); if (b->lsock[i] > top) top = b->lsock[i]; }
        if (b->csock >= 0) { FD_SET(b->csock, &rd); if (b->csock > top) top = b->csock; }
        struct timeval tv = { 0, 5000 };
        int r = select(top + 1, &rd, NULL, NULL, &tv);
        if (r > 0) {
            for (int i = 0; i < 2; i++) if (FD_ISSET(b->lsock[i], &rd)) take_client(b, i);
            if (b->csock >= 0 && FD_ISSET(b->csock, &rd)) {
                ssize_t n = recv(b->csock, buf, sizeof buf, 0);
                if (n <= 0) drop_client(b, n == 0 ? _("cerró") : _("error"));
                else from_client(b, buf, (int)n);
            }
        }
        /* the port, whether or not anybody listens: nothing piles up */
        int n = aos_io_uart_read(b->u, buf, sizeof buf, 2);
        if (n > 0) {
            if (b->csock >= 0) {
                if (!to_client(b, buf, n)) drop_client(b, _("error"));
                else b->down += (uint32_t)n;
            }
            J.io_bytes += (uint32_t)n;
        } else if (n < 0 && b->usb) {
            fl_fail(_("%s se desconectó"), J.port);
            return false;
        }
        fl_lock();
        fl_st.done = b->up + b->down;
        fl_unlock();
    }
    drop_client(b, _("se cerró el puente"));
    return false;           /* a cancel: the core counts it as the end it is */
}

bool fl_bridge_run(void)
{
    br_t *b = calloc(1, sizeof *b);
    if (!b) return fl_fail("%s", _("Sin memoria"));
    b->lsock[0] = b->lsock[1] = b->csock = -1;
    bool ok = run(b);
    for (int i = 0; i < 2; i++) if (b->lsock[i] >= 0) close(b->lsock[i]);
    if (b->csock >= 0) close(b->csock);
    if (b->u) {
        if (b->lines && !b->usb) aos_io_uart_lines(b->u, 1, 1);     /* EN and BOOT let go */
        aos_io_uart_close(b->u);
    }
    free(b);
    return ok;
}
