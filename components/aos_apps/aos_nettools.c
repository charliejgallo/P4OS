/*
 * P4OS - the Red app's service: the part that talks to the network.
 *
 * The contract is in aos_nettools.h. Everything here is plain sockets, the
 * same file on both platforms: lwIP gives the board the POSIX calls macOS
 * gives the simulator. The differences are three, and all of them small:
 *
 *   - ICMP. lwIP opens SOCK_RAW without asking anybody; macOS lets an
 *     ordinary user open SOCK_DGRAM + IPPROTO_ICMP (the kernel still hands
 *     over the IP header, so both are parsed the same). If neither opens,
 *     ping falls back to timing a TCP connect to port 80 and says so.
 *   - Reverse DNS. lwIP has none: on the board a host's name comes from mDNS
 *     (aos_hal_mdns_browse, the component espressif__mdns); the simulator
 *     asks getnameinfo() too.
 *     (P4_SIM_NO_ICMP=1 makes the simulator behave as if neither opened.)
 *   - What gets swept. The board sweeps the /24 of its own address and
 *     nothing else. The simulator sweeps 127.0.0.1 unless P4_SIM_NET_SCAN
 *     names another range (a.b.c.d/p, p from 24 to 32): testing from a Mac
 *     must not go knocking on the real LAN.
 *
 * The threads never call aos_tr(): the messages they leave are the Spanish
 * originals (N_) and the screen translates them when it shows them.
 *
 * Sockets are a firmware-wide resource on the board (CONFIG_LWIP_MAX_SOCKETS
 * is 10, shared with the portal, Home Assistant, MQTT...), so the sweeps
 * keep only a handful of connects in flight there; the simulator can afford
 * more. Every connect has a timeout, every wait is sliced so a stop is
 * noticed within a tenth of a second.
 */
#include "aos_nettools.h"
#include "aos_i18n.h"

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
#include <arpa/inet.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#include "esp_heap_caps.h"
static int64_t now_us(void) { return esp_timer_get_time(); }
#define PROBE_CONC        5         /* of the 10 lwIP sockets */
#define PROBE_TIMEOUT_MS  600       /* WiFi in modem sleep answers in 100-300 ms */
#define SWEEP_WAIT_MS     1000      /* after the last batch */
#define SWEEP_BATCH       8         /* lwIP's ARP table holds 10 */
#define SWEEP_BATCH_MS    200
#define SWEEP_ROUNDS      2         /* a second chance for a host in power save */
#else
#include <time.h>
static int64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
#define PROBE_CONC        32
#define PROBE_TIMEOUT_MS  300
#define SWEEP_WAIT_MS     800
#define SWEEP_BATCH       64
#define SWEEP_BATCH_MS    20
#define SWEEP_ROUNDS      1
#endif

#define STACK   6144
#define PRIO    3

/* -------------------------------------------------------------------------- */
/* State                                                                       */
/* -------------------------------------------------------------------------- */

static void *s_mx;
static volatile uint32_t s_alive_ms;

/* ~31 KB of results, on the heap and in PSRAM: P4OS keeps .bss inside
 * (CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is off), and internal RAM is
 * the one that runs short. */
typedef struct {
    nt_ping_t  ping;
    nt_scan_t  scan;
    nt_ports_t ports;
    nt_wifi_t  wifi;
    nt_mdns_t  mdns;
} nt_state_t;

static nt_state_t *T;
#define P (T->ping)
#define S (T->scan)
#define R (T->ports)
#define W (T->wifi)
#define M (T->mdns)

void *nt_big_calloc(size_t n, size_t size)
{
#ifdef ESP_PLATFORM
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
#endif
    return calloc(n, size);
}

static uint32_t s_ping_gen, s_scan_gen, s_ports_gen;
static bool s_scan_run, s_ports_run, s_wifi_run, s_wifi_want, s_mdns_run;

void nt_lock(void) { aos_hal_mutex_lock(s_mx); }
void nt_unlock(void) { aos_hal_mutex_unlock(s_mx); }

bool nt_init(void)
{
    if (T) return true;
    T = nt_big_calloc(1, sizeof *T);
    if (!T) return false;
    s_mx = aos_hal_mutex_create();
    srand((unsigned)now_us());
    return true;
}

void nt_keepalive(void) { s_alive_ms = (uint32_t)aos_hal_uptime_ms(); }

static bool app_alive(void) { return (uint32_t)aos_hal_uptime_ms() - s_alive_ms < 3000; }

const nt_ping_t  *nt_ping(void)  { return &P; }
const nt_scan_t  *nt_scan(void)  { return &S; }
const nt_ports_t *nt_ports(void) { return &R; }
const nt_wifi_t  *nt_wifi(void)  { return &W; }
const nt_mdns_t  *nt_mdns(void)  { return &M; }

void nt_ip_str(uint32_t ip, char *out, size_t n)
{
    snprintf(out, n, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255),
             (unsigned)(ip >> 8 & 255), (unsigned)(ip & 255));
}

static bool ip_parse(const char *s, uint32_t *ip)
{
    unsigned a, b, c, d;
    char tail;
    if (!s || sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;
    *ip = a << 24 | b << 16 | c << 8 | d;
    return true;
}

static void set_nonblock(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* -------------------------------------------------------------------------- */
/* ICMP echo                                                                   */
/* -------------------------------------------------------------------------- */

static uint16_t csum(const void *data, size_t n)
{
    const uint8_t *p = data;
    uint32_t s = 0;
    for (; n > 1; n -= 2, p += 2) s += (uint32_t)(p[0] << 8 | p[1]);
    if (n) s += (uint32_t)(p[0] << 8);
    while (s >> 16) s = (s & 0xFFFF) + (s >> 16);
    return (uint16_t)~s;
}

static int icmp_open(void)
{
#ifdef ESP_PLATFORM
    int fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
#else
    if (getenv("P4_SIM_NO_ICMP")) return -1;               /* to try the TCP fallback */
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);    /* macOS, no root needed */
    if (fd < 0) fd = socket(AF_INET, SOCK_RAW, IPPROTO_ICMP);
#endif
    if (fd >= 0) set_nonblock(fd);
    return fd;
}

static bool icmp_send(int fd, uint32_t ip, uint16_t id, uint16_t seq)
{
    uint8_t pk[40] = { 8, 0, 0, 0, (uint8_t)(id >> 8), (uint8_t)id, (uint8_t)(seq >> 8), (uint8_t)seq };
    for (size_t i = 8; i < sizeof pk; i++) pk[i] = (uint8_t)('a' + i % 26);
    uint16_t c = csum(pk, sizeof pk);
    pk[2] = (uint8_t)(c >> 8);
    pk[3] = (uint8_t)c;
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(ip);
    return sendto(fd, pk, sizeof pk, 0, (struct sockaddr *)&a, sizeof a) == (ssize_t)sizeof pk;
}

/* 1: an echo reply (its source, id, seq and TTL out), 0: nothing to read or
 * something else, -1: the socket failed. */
static int icmp_recv(int fd, uint32_t *src, uint16_t *id, uint16_t *seq, int *ttl)
{
    uint8_t b[256];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(fd, b, sizeof b, 0, (struct sockaddr *)&from, &fl);
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? 0 : -1;
    size_t off = 0;
    *ttl = 0;
    if (n >= 20 && (b[0] >> 4) == 4) {          /* the IP header came along */
        off = (size_t)(b[0] & 15) * 4;
        *ttl = b[8];
    }
    if ((size_t)n < off + 8 || b[off] != 0) return 0;   /* not an echo reply */
    *src = ntohl(from.sin_addr.s_addr);
    *id = (uint16_t)(b[off + 4] << 8 | b[off + 5]);
    *seq = (uint16_t)(b[off + 6] << 8 | b[off + 7]);
    return 1;
}

/* Waits until fd is readable, at most ms. */
static bool wait_read(int fd, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    return select(fd + 1, &set, NULL, NULL, &tv) > 0;
}

/* -------------------------------------------------------------------------- */
/* TCP connect probes, several in flight                                       */
/* -------------------------------------------------------------------------- */

enum { PR_SILENT = -1, PR_CLOSED = 0, PR_OPEN = 1 };

typedef struct {
    uint32_t ip;
    uint16_t port;
    uint16_t tag;               /* the caller's: an index */
} probe_t;

typedef struct {
    bool (*next)(void *ud, probe_t *out);
    void (*done)(void *ud, const probe_t *p, int res, uint32_t us);
    bool (*cancel)(void *ud);
    void *ud;
    int conc, timeout_ms;
} probe_job_t;

#define PROBE_SLOTS 32

static void probe_close(int fd)
{
    struct linger lg = { 1, 0 };    /* RST, no TIME_WAIT: this was only a knock */
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &lg, sizeof lg);
    close(fd);
}

/* Opens a non-blocking connect. PR_* if it finished at once, 2 if it is in
 * flight (*fd set), -2 if there was no socket to be had. */
static int probe_open(const probe_t *p, int *fd)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -2;
    set_nonblock(s);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(p->port);
    a.sin_addr.s_addr = htonl(p->ip);
    int r = connect(s, (struct sockaddr *)&a, sizeof a);
    if (r == 0) { probe_close(s); return PR_OPEN; }
    if (errno == EINPROGRESS || errno == EALREADY) { *fd = s; return 2; }
    int e = errno;
    probe_close(s);
    return e == ECONNREFUSED || e == ECONNRESET ? PR_CLOSED : PR_SILENT;
}

/* Runs until next() has nothing and nothing is in flight. false if cancelled
 * or out of sockets for too long. */
static bool probe_run(probe_job_t *j)
{
    struct { int fd; probe_t p; int64_t t0; } sl[PROBE_SLOTS];
    int active = 0, starved = 0;
    probe_t pend;
    bool have_pend = false;
    int conc = j->conc > PROBE_SLOTS ? PROBE_SLOTS : j->conc;
    for (;;) {
        if (j->cancel(j->ud)) {
            for (int i = 0; i < active; i++) probe_close(sl[i].fd);
            return false;
        }
        while (active < conc) {
            if (!have_pend && !j->next(j->ud, &pend)) break;
            have_pend = true;
            int64_t t0 = now_us();
            int fd = -1;
            int r = probe_open(&pend, &fd);
            if (r == -2) break;                     /* no socket: try again later */
            have_pend = false;
            starved = 0;
            if (r == 2) {
                sl[active].fd = fd;
                sl[active].p = pend;
                sl[active].t0 = t0;
                active++;
            } else {
                j->done(j->ud, &pend, r, (uint32_t)(now_us() - t0));
            }
        }
        if (!active) {
            if (!have_pend) return true;            /* all done */
            if (++starved > 25) return false;       /* 5 s without a socket */
            aos_hal_sleep_ms(200);
            continue;
        }
        fd_set wr;
        FD_ZERO(&wr);
        int maxfd = -1;
        int64_t now = now_us(), first = INT64_MAX;
        for (int i = 0; i < active; i++) {
            FD_SET(sl[i].fd, &wr);
            if (sl[i].fd > maxfd) maxfd = sl[i].fd;
            int64_t dl = sl[i].t0 + (int64_t)j->timeout_ms * 1000;
            if (dl < first) first = dl;
        }
        int64_t wait = first - now;
        if (wait > 50000) wait = 50000;
        if (wait < 1000) wait = 1000;
        struct timeval tv = { .tv_sec = 0, .tv_usec = (int)wait };
        int n = select(maxfd + 1, NULL, &wr, NULL, &tv);
        now = now_us();
        for (int i = 0; i < active;) {
            int res = 2;
            if (n > 0 && FD_ISSET(sl[i].fd, &wr)) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(sl[i].fd, SOL_SOCKET, SO_ERROR, &err, &el);
                res = !err ? PR_OPEN : (err == ECONNREFUSED || err == ECONNRESET) ? PR_CLOSED : PR_SILENT;
            } else if (now - sl[i].t0 >= (int64_t)j->timeout_ms * 1000) {
                res = PR_SILENT;
            }
            if (res == 2) { i++; continue; }
            probe_close(sl[i].fd);
            probe_t p = sl[i].p;
            uint32_t us = (uint32_t)(now - sl[i].t0);
            sl[i] = sl[--active];
            j->done(j->ud, &p, res, us);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Ping                                                                        */
/* -------------------------------------------------------------------------- */

static bool resolve(const char *host, uint32_t *ip)
{
    if (ip_parse(host, ip)) return true;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0 || !res) return false;
    *ip = ntohl(((struct sockaddr_in *)res->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(res);
    return true;
}

/* Records one answer (ms >= 0) or a loss (ms < 0), if this is still the ping
 * the screen asked for. */
static bool ping_note(uint32_t gen, float ms, int ttl)
{
    nt_lock();
    bool mine = gen == s_ping_gen;
    if (mine) {
        P.sent++;
        if (ms >= 0) {
            float prev = P.recv ? P.last_ms : ms;
            P.recv++;
            P.last_ms = ms;
            if (P.recv == 1 || ms < P.min_ms) P.min_ms = ms;
            if (P.recv == 1 || ms > P.max_ms) P.max_ms = ms;
            P.avg_ms += (ms - P.avg_ms) / (float)P.recv;
            if (P.recv > 1) {
                float d = ms > prev ? ms - prev : prev - ms;
                P.jitter_ms += (d - P.jitter_ms) / (float)(P.recv - 1);
            }
            if (ttl) P.ttl = ttl;
        }
        if (P.hist_n == NT_PING_HIST) {
            memmove(P.hist, P.hist + 1, sizeof P.hist - sizeof P.hist[0]);
            P.hist_n--;
        }
        P.hist[P.hist_n++] = ms;
        P.seq++;
    }
    nt_unlock();
    return mine;
}

static bool ping_current(uint32_t gen)
{
    nt_lock();
    bool mine = gen == s_ping_gen;
    nt_unlock();
    return mine && app_alive();
}

static void ping_thread(void *arg)
{
    uint32_t gen = (uint32_t)(uintptr_t)arg;
    char host[64];
    nt_lock();
    snprintf(host, sizeof host, "%s", P.host);
    nt_unlock();

    int64_t t0 = now_us();
    uint32_t ip = 0;
    bool ok = resolve(host, &ip);
    uint32_t dns_ms = (uint32_t)((now_us() - t0) / 1000);
    int fd = ok ? icmp_open() : -1;
    nt_lock();
    if (gen == s_ping_gen) {
        P.resolved = ok;
        P.dns_ms = dns_ms;
        if (ok) nt_ip_str(ip, P.ip, sizeof P.ip);
        else {
            snprintf(P.err, sizeof P.err, "%s", N_("No se encontró ese nombre"));
            P.state = NT_FAILED;
        }
        P.icmp = fd >= 0;
        P.tcp_port = 80;
        P.seq++;
    }
    nt_unlock();
    if (!ok) return;

    uint16_t id = (uint16_t)(rand() ^ gen), seq = 0;
    while (ping_current(gen)) {
        int64_t start = now_us();
        float ms = -1;
        int ttl = 0;
        if (fd >= 0) {
            seq++;
            if (icmp_send(fd, ip, id, seq)) {
                while (now_us() - start < 1000000 && ping_current(gen)) {
                    if (!wait_read(fd, 100)) continue;
                    uint32_t src;
                    uint16_t rid, rseq;
                    int r;
                    while ((r = icmp_recv(fd, &src, &rid, &rseq, &ttl)) == 1) {
                        if (src == ip && rid == id && rseq == seq) { ms = (float)(now_us() - start) / 1000.0f; break; }
                    }
                    if (ms >= 0 || r < 0) break;
                }
            }
        } else {
            /* No ICMP here: the time a TCP connect takes to be answered,
             * accepted or refused, is a round trip too. */
            int s = -1;
            probe_t p = { .ip = ip, .port = 80 };
            int r = probe_open(&p, &s);
            if (r == PR_OPEN || r == PR_CLOSED) ms = (float)(now_us() - start) / 1000.0f;
            else if (r == 2) {
                while (now_us() - start < 1000000 && ping_current(gen)) {
                    fd_set wr;
                    FD_ZERO(&wr);
                    FD_SET(s, &wr);
                    struct timeval tv = { 0, 100000 };
                    if (select(s + 1, NULL, &wr, NULL, &tv) > 0) {
                        int err = 0;
                        socklen_t el = sizeof err;
                        getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
                        if (!err || err == ECONNREFUSED || err == ECONNRESET) ms = (float)(now_us() - start) / 1000.0f;
                        break;
                    }
                }
                probe_close(s);
            }
        }
        if (!ping_current(gen)) break;
        ping_note(gen, ms, ttl);
        /* one a second */
        while (now_us() - start < 1000000 && ping_current(gen)) aos_hal_sleep_ms(50);
    }
    if (fd >= 0) close(fd);
    nt_lock();
    if (gen == s_ping_gen && P.state == NT_BUSY) { P.state = NT_IDLE; P.seq++; }
    nt_unlock();
}

bool nt_ping_start(const char *host)
{
    if (!host || !host[0]) return false;
    nt_keepalive();
    nt_lock();
    uint32_t seq = P.seq + 1;
    memset(&P, 0, sizeof P);
    P.seq = seq;
    snprintf(P.host, sizeof P.host, "%s", host);
    P.state = NT_BUSY;
    uint32_t gen = ++s_ping_gen;
    nt_unlock();
    if (!aos_hal_thread_start("nt_ping", ping_thread, (void *)(uintptr_t)gen, STACK, PRIO)) {
        nt_lock();
        P.state = NT_FAILED;
        snprintf(P.err, sizeof P.err, "%s", N_("No hay memoria para empezar"));
        P.seq++;
        nt_unlock();
        return false;
    }
    return true;
}

void nt_ping_stop(void)
{
    nt_lock();
    s_ping_gen++;
    if (P.state == NT_BUSY) P.state = NT_IDLE;
    P.seq++;
    nt_unlock();
}

/* -------------------------------------------------------------------------- */
/* What a host is                                                              */
/* -------------------------------------------------------------------------- */

const uint16_t NT_KNOWN_PORTS[] = { 80, 443, 22, 8123, 1883, 502, 5555, 554, 6053, 8080 };
const int NT_KNOWN_N = (int)(sizeof NT_KNOWN_PORTS / sizeof NT_KNOWN_PORTS[0]);

const nt_mdns_type_t NT_MDNS_TYPES[] = {
    { "_home-assistant._tcp", "Home Assistant",        NT_MD_HA },
    { "_esphomelib._tcp",     "ESPHome",               NT_MD_ESPHOME },
    { "_mqtt._tcp",           "MQTT",                  NT_MD_MQTT },
    { "_http._tcp",           N_("Páginas web"),       NT_MD_HTTP },
    { "_hap._tcp",            "HomeKit",               NT_MD_HAP },
    { "_googlecast._tcp",     "Chromecast",            NT_MD_CAST },
    { "_airplay._tcp",        "AirPlay",               NT_MD_AIRPLAY },
    { "_spotify-connect._tcp","Spotify Connect",       NT_MD_SPOTIFY },
    { "_ipp._tcp",            N_("Impresoras"),        NT_MD_PRINTER },
    { "_printer._tcp",        N_("Impresoras (LPD)"),  NT_MD_PRINTER },
    { "_rtsp._tcp",           N_("Cámaras RTSP"),      NT_MD_RTSP },
    { "_smb._tcp",            N_("Carpetas compartidas"), NT_MD_SMB },
    { "_ssh._tcp",            "SSH",                   NT_MD_SSH },
    { "_arduino._tcp",        N_("Arduino OTA"),       NT_MD_ARDUINO },
    { "_workstation._tcp",    N_("Computadoras"),      NT_MD_WORKSTATION },
};
const int NT_MDNS_NTYPES = (int)(sizeof NT_MDNS_TYPES / sizeof NT_MDNS_TYPES[0]);

int nt_mdns_type_index(const char *type)
{
    for (int i = 0; i < NT_MDNS_NTYPES; i++) if (!strcmp(NT_MDNS_TYPES[i].type, type)) return i;
    return -1;
}

static bool has_port(const nt_host_t *h, uint16_t p)
{
    for (int i = 0; i < h->nports; i++) if (h->ports[i] == p) return true;
    return false;
}

const char *nt_guess(const nt_host_t *h)
{
    if (h->self) return N_("Este P4OS");
    if ((h->mdns & NT_MD_HA) || has_port(h, 8123)) return "Home Assistant";
    if ((h->mdns & NT_MD_ESPHOME) || has_port(h, 6053)) return "ESPHome";
    if (has_port(h, 5555)) return N_("Rigol (SCPI)");
    if ((h->mdns & NT_MD_RTSP) || has_port(h, 554)) return N_("Cámara RTSP");
    if (has_port(h, 502)) return "Modbus TCP";
    if ((h->mdns & NT_MD_MQTT) || has_port(h, 1883)) return N_("Broker MQTT");
    if (h->mdns & NT_MD_CAST) return "Chromecast";
    if (h->mdns & NT_MD_PRINTER) return N_("Impresora");
    if (h->mdns & NT_MD_HAP) return "HomeKit";
    if (h->mdns & NT_MD_AIRPLAY) return "AirPlay";
    if (h->mdns & NT_MD_SMB) return N_("Carpetas compartidas");
    if (has_port(h, 22)) return has_port(h, 80) || has_port(h, 443) ? N_("Servidor Linux") : N_("Equipo con SSH");
    if (has_port(h, 80) || has_port(h, 443) || has_port(h, 8080)) return N_("Equipo con página web");
    if (!h->nports) return h->icmp ? N_("Sólo responde al ping") : N_("Sin puertos conocidos");
    return N_("Equipo de red");
}

const char *nt_port_name(uint16_t port)
{
    static const struct { uint16_t p; const char *n; } T[] = {
        { 20, "FTP" }, { 21, "FTP" }, { 22, "SSH" }, { 23, "Telnet" }, { 25, "SMTP" }, { 53, "DNS" },
        { 80, "HTTP" }, { 88, "Kerberos" }, { 110, "POP3" }, { 111, "RPC" }, { 135, "RPC" }, { 139, "NetBIOS" },
        { 143, "IMAP" }, { 389, "LDAP" }, { 443, "HTTPS" }, { 445, "SMB" }, { 502, "Modbus" },
        { 515, "LPD" }, { 548, "AFP" }, { 554, "RTSP" }, { 587, "SMTP" }, { 631, "IPP" },
        { 853, "DNS/TLS" }, { 993, "IMAPS" }, { 995, "POP3S" }, { 1400, "Sonos" },
        { 1880, "Node-RED" }, { 1883, "MQTT" }, { 1884, "MQTT" }, { 3000, "Grafana" },
        { 3306, "MySQL" }, { 3389, "RDP" }, { 5000, "UPnP" }, { 5001, "HTTPS" },
        { 5432, "PostgreSQL" }, { 5555, "SCPI" }, { 5900, "VNC" }, { 6053, "ESPHome" },
        { 6668, "Tuya" }, { 7000, "AirPlay" }, { 8000, "HTTP" }, { 8008, "HTTP" }, { 8009, "Cast" },
        { 8080, "HTTP" }, { 8081, "HTTP" }, { 8123, "Home Assistant" }, { 8443, "HTTPS" },
        { 8554, "RTSP" }, { 8883, "MQTT/TLS" }, { 8888, "HTTP" }, { 9000, "HTTP" },
        { 9090, "HTTP" }, { 9100, N_("Impresora") }, { 32400, "Plex" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].p == port) return T[i].n;
    return "";
}

static void host_add_port(nt_host_t *h, uint16_t p)
{
    if (has_port(h, p) || h->nports >= NT_HOST_PORTS) return;
    int i = h->nports++;
    while (i > 0 && h->ports[i - 1] > p) { h->ports[i] = h->ports[i - 1]; i--; }
    h->ports[i] = p;
}

/* -------------------------------------------------------------------------- */
/* mDNS, used by its tab and by the sweep for names                            */
/* -------------------------------------------------------------------------- */

static int svc_cmp(const void *a, const void *b)
{
    const aos_mdns_svc_t *x = a, *y = b;
    int tx = nt_mdns_type_index(x->type), ty = nt_mdns_type_index(y->type);
    if (tx != ty) return tx - ty;
    return strcmp(x->instance, y->instance);
}

/* Browses every type into a fresh buffer (malloc'd, the caller frees);
 * returns the count or -1. */
static int mdns_browse_all(aos_mdns_svc_t **out, uint32_t timeout_ms)
{
    const char *types[32];
    for (int i = 0; i < NT_MDNS_NTYPES; i++) types[i] = NT_MDNS_TYPES[i].type;
    *out = nt_big_calloc(NT_MAX_SVC, sizeof **out);
    if (!*out) return -1;
    int n = aos_hal_mdns_browse(types, NT_MDNS_NTYPES, timeout_ms, *out, NT_MAX_SVC);
    if (n > 0) qsort(*out, (size_t)n, sizeof **out, svc_cmp);
    return n;
}

static void mdns_publish(const aos_mdns_svc_t *svc, int n, uint32_t ms)
{
    nt_lock();
    if (n < 0) {
        M.state = NT_FAILED;
        snprintf(M.err, sizeof M.err, "%s", N_("mDNS no está andando: ¿hay red?"));
        M.n = 0;
    } else {
        M.state = NT_DONE;
        M.err[0] = 0;
        M.n = n > NT_MAX_SVC ? NT_MAX_SVC : n;
        memcpy(M.svc, svc, (size_t)M.n * sizeof *svc);
    }
    M.elapsed_ms = ms;
    M.seq++;
    nt_unlock();
}

static void mdns_thread(void *arg)
{
    (void)arg;
    int64_t t0 = now_us();
    aos_mdns_svc_t *svc = NULL;
    int n = mdns_browse_all(&svc, 2500);
    mdns_publish(svc, n, (uint32_t)((now_us() - t0) / 1000));
    free(svc);
    nt_lock();
    s_mdns_run = false;
    nt_unlock();
}

bool nt_mdns_start(void)
{
    nt_keepalive();
    nt_lock();
    bool busy = s_mdns_run;
    if (!busy) {
        s_mdns_run = true;
        M.state = NT_BUSY;
        M.err[0] = 0;
        M.seq++;
    }
    nt_unlock();
    if (busy) return false;
    if (!aos_hal_thread_start("nt_mdns", mdns_thread, NULL, STACK, PRIO)) {
        nt_lock();
        s_mdns_run = false;
        M.state = NT_FAILED;
        snprintf(M.err, sizeof M.err, "%s", N_("No hay memoria para empezar"));
        M.seq++;
        nt_unlock();
        return false;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/* The LAN sweep                                                               */
/* -------------------------------------------------------------------------- */

#define MAX_TARGETS 254

typedef struct {
    uint32_t gen;
    bool full;
    int n;                      /* targets */
    nt_host_t *h;               /* one per target */
    bool *alive, *expanded;
    probe_t *q;                 /* the port queue, grows as hosts turn up */
    int qn, qcap, qi, qdone;
    uint32_t self;
    int64_t t0, last_pub, last_arp;
} sweep_t;

static bool scan_cancel(void *ud)
{
    sweep_t *w = ud;
    nt_lock();
    bool stop = w->gen != s_scan_gen;
    nt_unlock();
    return stop || !app_alive();
}

/* Copies what is known into S: the live ones, in address order. */
static void scan_publish(sweep_t *w, int phase, int done, int total)
{
    nt_lock();
    if (w->gen == s_scan_gen) {
        int k = 0;
        for (int i = 0; i < w->n && k < NT_MAX_HOSTS; i++) if (w->alive[i]) S.hosts[k++] = w->h[i];
        S.n = k;
        S.phase = phase;
        S.done = done;
        S.total = total;
        S.elapsed_ms = (uint32_t)((now_us() - w->t0) / 1000);
        S.seq++;
    }
    nt_unlock();
    w->last_pub = now_us();
}

static bool sweep_next(void *ud, probe_t *out)
{
    sweep_t *w = ud;
    if (w->qi >= w->qn) return false;
    *out = w->q[w->qi++];
    return true;
}

static void sweep_queue(sweep_t *w, int host, int from, int to)
{
    for (int k = from; k < to && w->qn < w->qcap; k++)
        w->q[w->qn++] = (probe_t){ .ip = w->h[host].ip, .port = NT_KNOWN_PORTS[k], .tag = (uint16_t)host };
}

/* The MACs. lwIP keeps 10 ARP entries and recycles them as the sweep asks
 * for new addresses, so the table is read after every batch of pings and
 * every 200 ms of the port probes, before the next ones push them out. An
 * address that answered ARP is there even if it ignores the ping (phones,
 * Windows with its firewall): it counts as alive, and gets its ports probed.
 * 'probing': the port phase is on, and a newly found one needs its queue. */
static void arp_harvest(sweep_t *w, bool probing)
{
    aos_arp_entry_t e[16];
    int n = aos_hal_net_arp_table(e, 16);
    for (int k = 0; k < n; k++) {
        uint32_t i = e[k].ip - w->h[0].ip;
        if (i >= (uint32_t)w->n || w->h[i].ip != e[k].ip) continue;
        nt_host_t *x = &w->h[i];
        memcpy(x->mac, e[k].mac, 6);
        x->has_mac = true;
        if (w->alive[i]) continue;
        w->alive[i] = true;
        x->arp_only = true;
        if (probing && !w->expanded[i]) {
            w->expanded[i] = true;
            sweep_queue(w, (int)i, w->full ? 2 : 0, NT_KNOWN_N);    /* the full sweep queued 80 and 443 */
        }
    }
    w->last_arp = now_us();
}

static void sweep_done(void *ud, const probe_t *p, int res, uint32_t us)
{
    sweep_t *w = ud;
    int i = p->tag;
    w->qdone++;
    if (res == PR_OPEN) host_add_port(&w->h[i], p->port);
    if (res != PR_SILENT && (!w->alive[i] || w->h[i].arp_only)) {
        /* It refused or accepted: it is there, even if it ignores the ping.
         * Now it deserves the rest of the list. */
        w->alive[i] = true;
        w->h[i].arp_only = false;
        if (!w->h[i].icmp && w->h[i].rtt_ms <= 0) w->h[i].rtt_ms = (float)us / 1000.0f;
    }
    if (w->alive[i] && !w->expanded[i]) {
        w->expanded[i] = true;
        sweep_queue(w, i, 2, NT_KNOWN_N);
    }
    if (now_us() - w->last_arp > 200000) arp_harvest(w, true);
    if (now_us() - w->last_pub > 250000) scan_publish(w, 1, w->qdone, w->qn);
}

static bool scan_targets(uint32_t *base, int *count, uint32_t *self, char *range, size_t rn, char *err, size_t en)
{
    *self = 0;
    ip_parse(aos_hal_net_ip(), self);
#ifdef AOS_SIM
    const char *env = getenv("P4_SIM_NET_SCAN");
    char spec[40];
    snprintf(spec, sizeof spec, "%s", env && env[0] ? env : "127.0.0.1/32");
    char *slash = strchr(spec, '/');
    int prefix = 32;
    if (slash) { *slash = 0; prefix = atoi(slash + 1); }
    uint32_t ip;
    if (!ip_parse(spec, &ip) || prefix < 24 || prefix > 32) {
        snprintf(err, en, "%s", N_("P4_SIM_NET_SCAN no es un rango válido (a.b.c.d/24 a /32)"));
        return false;
    }
    uint32_t mask = prefix == 32 ? 0xFFFFFFFFu : ~((1u << (32 - prefix)) - 1);
    uint32_t net = ip & mask;
    uint32_t size = 1u << (32 - prefix);
    char a[16];
    nt_ip_str(net, a, sizeof a);
    snprintf(range, rn, "%s/%d", a, prefix);
    if (size <= 2) { *base = net; *count = (int)size; }
    else { *base = net + 1; *count = (int)size - 2; }     /* not the network nor the broadcast */
    return true;
#else
    if (aos_hal_net_state() != AOS_NET_CONNECTED || !*self) {
        snprintf(err, en, "%s", N_("Sin conexión a la red WiFi"));
        return false;
    }
    /* The /24 of our own address and never more: this is a scanner of the
     * house, not of the internet. */
    uint32_t net = *self & 0xFFFFFF00u;
    char a[16];
    nt_ip_str(net, a, sizeof a);
    snprintf(range, rn, "%s/24", a);
    *base = net + 1;
    *count = 254;
    return true;
#endif
}

/* -------------------------------------------------------------------------- */
/* The saved survey                                                            */
/* -------------------------------------------------------------------------- */

/* A finished sweep goes to the card as one NDJSON file, the format AmoledOS's
 * aos_scan.c writes ("inicio", "wifi", "host", "puertos", "nombre", "fin"
 * lines), so the portal's Red page reads both. The MACs come from lwIP's
 * ARP table, read as the sweep goes (arp_harvest); the rest is what this
 * sweep knows and that one did not: the ping time, the guess of what each one
 * is and which one is the board. */

static void jstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; s && *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') fprintf(f, "\\%c", c);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static const char *auth_txt(uint8_t a)
{
    switch (a) {
    case AOS_WIFI_AUTH_OPEN:       return "abierta";
    case AOS_WIFI_AUTH_WEP:        return "WEP";
    case AOS_WIFI_AUTH_WPA:        return "WPA";
    case AOS_WIFI_AUTH_WPA2:       return "WPA2";
    case AOS_WIFI_AUTH_WPA_WPA2:   return "WPA/WPA2";
    case AOS_WIFI_AUTH_WPA3:       return "WPA3";
    case AOS_WIFI_AUTH_WPA2_WPA3:  return "WPA2/WPA3";
    case AOS_WIFI_AUTH_ENTERPRISE: return "enterprise";
    default:                       return "otra";
    }
}

typedef struct { char name[40]; time_t mtime; } saved_t;

static int saved_cmp(const void *a, const void *b)
{
    const saved_t *x = a, *y = b;
    if (x->mtime != y->mtime) return x->mtime < y->mtime ? -1 : 1;
    return strcmp(x->name, y->name);
}

/* The newest NT_SAVED_MAX stay; by date, so one saved before the clock was
 * set (dated 1980 by FAT) is the first to go. */
static void saved_prune(const char *dir)
{
    enum { CAP = 160 };
    saved_t *v = nt_big_calloc(CAP, sizeof *v);
    DIR *d = v ? opendir(dir) : NULL;
    if (!d) { free(v); return; }
    int n = 0;
    struct dirent *e;
    char path[96];
    while ((e = readdir(d)) && n < CAP) {
        size_t l = strlen(e->d_name);
        if (l < 8 || l >= sizeof v[0].name || strcmp(e->d_name + l - 7, ".ndjson")) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(path, &st)) continue;
        memcpy(v[n].name, e->d_name, l + 1);
        v[n++].mtime = st.st_mtime;
    }
    closedir(d);
    if (n > NT_SAVED_MAX) {
        qsort(v, (size_t)n, sizeof *v, saved_cmp);
        for (int i = 0; i < n - NT_SAVED_MAX; i++) {
            snprintf(path, sizeof path, "%s/%s", dir, v[i].name);
            unlink(path);
        }
    }
    free(v);
}

static void survey_save(const nt_host_t *h, const bool *alive, int count, bool full,
                        const char *range, const aos_wifi_ap_ex_t *aps, int naps,
                        uint32_t ms, char *file, size_t fn)
{
    const char *dir = aos_hal_path_scans();
    char path[96], when[24];
    struct tm t;
    aos_hal_time_now(&t);
    mkdir(dir, 0777);
    if (aos_hal_time_is_valid()) {
        strftime(file, fn, "%Y%m%d-%H%M%S.ndjson", &t);
        strftime(when, sizeof when, "%Y-%m-%d %H:%M", &t);
    } else {
        /* with no clock, a name that cannot collide is better than a date */
        snprintf(file, fn, "barrido-%u.ndjson", (unsigned)(aos_hal_uptime_ms() / 1000));
        when[0] = 0;
    }
    snprintf(path, sizeof path, "%s/%s", dir, file);
    FILE *f = fopen(path, "w");
    if (!f) { file[0] = 0; return; }

    const char *slash = strchr(range, '/');
    int prefix = slash ? atoi(slash + 1) : 24;
    uint32_t mask = prefix >= 32 ? 0xFFFFFFFFu : prefix <= 0 ? 0 : ~((1u << (32 - prefix)) - 1);
    char mk[16];
    nt_ip_str(mask, mk, sizeof mk);
    fputs("{\"t\":\"inicio\",\"fecha\":", f);
    jstr(f, when);
    fputs(",\"ssid\":", f);
    jstr(f, aos_hal_net_ssid());
    fputs(",\"ip\":", f);
    jstr(f, aos_hal_net_ip());
    fprintf(f, ",\"mascara\":\"%s\",\"rssi\":%d,\"rango\":", mk, aos_hal_net_rssi());
    jstr(f, range);
    fprintf(f, ",\"completo\":%s,\"equipo\":", full ? "true" : "false");
    jstr(f, aos_hal_device_name());
    fputs("}\n", f);

    for (int i = 0; i < naps; i++) {
        const aos_wifi_ap_ex_t *a = &aps[i];
        fputs("{\"t\":\"wifi\",\"ssid\":", f);
        jstr(f, a->ssid);
        fprintf(f, ",\"bssid\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"rssi\":%d,\"canal\":%d,"
                   "\"ancho\":%d,\"segundo\":%d,\"cifrado\":\"%s\",\"oculta\":%s}\n",
                a->bssid[0], a->bssid[1], a->bssid[2], a->bssid[3], a->bssid[4], a->bssid[5],
                a->rssi, a->channel, a->width ? a->width : 20, a->second, auth_txt(a->auth),
                a->ssid[0] ? "false" : "true");
    }

    int hosts = 0, ports = 0;
    char ip[16];
    for (int i = 0; i < count; i++) {
        if (!alive[i]) continue;
        const nt_host_t *x = &h[i];
        hosts++;
        ports += x->nports;
        nt_ip_str(x->ip, ip, sizeof ip);
        fprintf(f, "{\"t\":\"host\",\"ip\":\"%s\",\"ping\":%s", ip, x->icmp ? "true" : "false");
        if (x->has_mac)
            fprintf(f, ",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\"",
                    x->mac[0], x->mac[1], x->mac[2], x->mac[3], x->mac[4], x->mac[5]);
        if (x->arp_only) fputs(",\"arp\":true", f);
        if (x->icmp) fprintf(f, ",\"rtt\":%d", (int)(x->rtt_ms + 0.5f));      /* ms: no float printf */
        if (x->self) fputs(",\"yo\":true", f);
        fputs(",\"que\":", f);
        jstr(f, nt_guess(x));
        fputs("}\n", f);
        if (x->nports) {
            fprintf(f, "{\"t\":\"puertos\",\"ip\":\"%s\",\"abiertos\":[", ip);
            for (int k = 0; k < x->nports; k++) fprintf(f, "%s%u", k ? "," : "", x->ports[k]);
            fputs("]}\n", f);
        }
        if (x->name[0]) {
            /* one line per service, the way aos_scan.c writes them; a name
             * with no service announced (the board's own) still gets one */
            bool any = false;
            for (int k = 0; k <= NT_MDNS_NTYPES; k++) {
                if (k < NT_MDNS_NTYPES ? !(x->mdns & NT_MDNS_TYPES[k].bit) : any) continue;
                fprintf(f, "{\"t\":\"nombre\",\"ip\":\"%s\",\"nombre\":", ip);
                jstr(f, x->name);
                fputs(",\"host\":", f);
                jstr(f, x->name);
                fputs(",\"servicio\":", f);
                jstr(f, k < NT_MDNS_NTYPES ? NT_MDNS_TYPES[k].label : "");
                fputs("}\n", f);
                any = true;
            }
        }
    }
    fprintf(f, "{\"t\":\"fin\",\"redes\":%d,\"equipos\":%d,\"puertos\":%d,\"ms\":%u,\"cortado\":false}\n",
            naps, hosts, ports, (unsigned)ms);
    fclose(f);
    saved_prune(dir);
}

static void scan_thread(void *arg)
{
    sweep_t w;
    memset(&w, 0, sizeof w);
    w.gen = (uint32_t)(uintptr_t)arg;
    w.t0 = now_us();
    uint32_t base = 0;
    int count = 0;
    char range[40] = "", err[96] = "";
    bool icmp_ok = false;
    nt_lock();
    w.full = S.full;
    nt_unlock();
    if (!scan_targets(&base, &count, &w.self, range, sizeof range, err, sizeof err)) goto fail;
    if (count > MAX_TARGETS) count = MAX_TARGETS;
    w.n = count;
    w.h = nt_big_calloc((size_t)count, sizeof *w.h);
    w.alive = calloc((size_t)count, sizeof *w.alive);
    w.expanded = calloc((size_t)count, sizeof *w.expanded);
    w.qcap = count * NT_KNOWN_N;
    w.q = nt_big_calloc((size_t)w.qcap, sizeof *w.q);
    if (!w.h || !w.alive || !w.expanded || !w.q) { snprintf(err, sizeof err, "%s", N_("No hay memoria para el barrido")); goto fail; }
    for (int i = 0; i < count; i++) {
        w.h[i].ip = base + (uint32_t)i;
        w.h[i].self = w.h[i].ip == w.self;
        if (w.h[i].self) w.h[i].has_mac = aos_hal_net_mac(w.h[i].mac);
    }
    nt_lock();
    if (w.gen == s_scan_gen) { snprintf(S.range, sizeof S.range, "%s", range); S.total = count; S.seq++; }
    nt_unlock();

    /* 1. Everybody gets a ping, in small batches with a pause to read the
     *    answers, and on the board a second round for the silent ones.
     *    The batches are for lwIP's ARP table: 10 entries, and an address
     *    being resolved takes one until its answer comes. 254 pings in a row
     *    would recycle each entry long before its host could reply. */
    int fd = icmp_open();
    icmp_ok = fd >= 0;
    if (icmp_ok) {
        uint16_t id = (uint16_t)(rand() ^ 0x5A5A);
        int64_t sent_at[MAX_TARGETS];
        int sent = 0, total = count * SWEEP_ROUNDS;
        for (int round = 0; round < SWEEP_ROUNDS && !scan_cancel(&w); round++) {
            int i = 0;
            while (i < count && !scan_cancel(&w)) {
                /* a batch of the ones still silent */
                for (int b = 0; b < SWEEP_BATCH && i < count; i++) {
                    if (w.alive[i]) continue;
                    sent_at[i] = now_us();
                    icmp_send(fd, w.h[i].ip, id, (uint16_t)i);
                    b++;
                    sent++;
                }
                int64_t until = now_us() + (i < count ? SWEEP_BATCH_MS : SWEEP_WAIT_MS) * 1000;
                while (now_us() < until && !scan_cancel(&w)) {
                    if (!wait_read(fd, 20)) continue;
                    uint32_t src;
                    uint16_t rid, rseq;
                    int ttl;
                    while (icmp_recv(fd, &src, &rid, &rseq, &ttl) == 1) {
                        if (rid != id || rseq >= count || w.h[rseq].ip != src || w.alive[rseq]) continue;
                        w.alive[rseq] = true;
                        w.h[rseq].icmp = true;
                        w.h[rseq].rtt_ms = (float)(now_us() - sent_at[rseq]) / 1000.0f;
                    }
                }
                arp_harvest(&w, false);         /* before the next batch pushes these out */
                if (now_us() - w.last_pub > 250000) scan_publish(&w, 0, round * count + i, total);
            }
        }
        (void)sent;
        close(fd);
    }
    nt_lock();
    if (w.gen == s_scan_gen) S.icmp = icmp_ok;
    nt_unlock();
    if (scan_cancel(&w)) goto stopped;
    if (w.self) {                       /* this device is always there */
        for (int i = 0; i < count; i++) if (w.h[i].self) w.alive[i] = true;
    }
    scan_publish(&w, 1, 0, 0);

    /* 2. The known ports of the live ones; the silent ones only in the full
     *    sweep (or when there was no ping), and at first only 80 and 443. */
    for (int i = 0; i < count; i++) {
        if (w.alive[i]) { w.expanded[i] = true; sweep_queue(&w, i, 0, NT_KNOWN_N); }
    }
    for (int i = 0; i < count; i++) {
        if (!w.alive[i] && (w.full || !icmp_ok)) sweep_queue(&w, i, 0, 2);
    }
    probe_job_t job = { sweep_next, sweep_done, scan_cancel, &w, PROBE_CONC, PROBE_TIMEOUT_MS };
    if (!probe_run(&job)) {
        if (scan_cancel(&w)) goto stopped;
        snprintf(err, sizeof err, "%s", N_("Sin sockets libres: probá de nuevo en un rato"));
        goto fail;
    }
    arp_harvest(&w, false);             /* the last probes' answers */
    scan_publish(&w, 2, 0, 0);

    /* 3. Names: mDNS (the board has no reverse DNS), and on the desktop
     *    getnameinfo() as well. */
    {
        aos_mdns_svc_t *svc = NULL;
        int64_t m0 = now_us();
        int n = mdns_browse_all(&svc, 1500);
        if (n > 0) {
            for (int s = 0; s < n; s++) {
                int ti = nt_mdns_type_index(svc[s].type);
                for (int i = 0; i < count; i++) {
                    if (!w.alive[i] || w.h[i].ip != svc[s].ip) continue;
                    if (!w.h[i].name[0] && svc[s].host[0]) snprintf(w.h[i].name, sizeof w.h[i].name, "%s.local", svc[s].host);
                    if (ti >= 0) w.h[i].mdns |= NT_MDNS_TYPES[ti].bit;
                }
            }
            nt_lock();
            bool idle = !s_mdns_run;
            nt_unlock();
            if (idle) mdns_publish(svc, n, (uint32_t)((now_us() - m0) / 1000));   /* the mDNS tab gets it free */
        }
        free(svc);
    }
    for (int i = 0; i < count && !scan_cancel(&w); i++) {
        if (!w.alive[i]) continue;
#ifndef ESP_PLATFORM
        if (!w.h[i].name[0]) {
            struct sockaddr_in a;
            memset(&a, 0, sizeof a);
            a.sin_family = AF_INET;
            a.sin_addr.s_addr = htonl(w.h[i].ip);
            char nm[NI_MAXHOST];
            if (!getnameinfo((struct sockaddr *)&a, sizeof a, nm, sizeof nm, NULL, 0, NI_NAMEREQD))
                snprintf(w.h[i].name, sizeof w.h[i].name, "%s", nm);
        }
#endif
        if (w.h[i].self && !w.h[i].name[0]) snprintf(w.h[i].name, sizeof w.h[i].name, "%s.local", aos_hal_device_name());
    }
    if (scan_cancel(&w)) goto stopped;

    /* 4. The networks around, for the saved survey: the WiFi tab's scan if it
     *    is fresh, else one now (two seconds of the radio off the channel,
     *    which is why it waits until the sweep is over). */
    {
        aos_wifi_ap_ex_t *aps = nt_big_calloc(NT_MAX_APS, sizeof *aps);
        int naps = 0;
        if (aps) {
            nt_lock();
            bool fresh = W.n > 0 && W.state == NT_DONE && (uint32_t)aos_hal_uptime_ms() - W.scanned_ms < 120000;
            if (fresh) { naps = W.n; memcpy(aps, W.aps, (size_t)naps * sizeof *aps); }
            nt_unlock();
            if (!fresh) {
                naps = aos_hal_net_scan_ex(aps, NT_MAX_APS);
                if (naps < 0) naps = 0;
                nt_lock();
                if (naps > 0 && W.state != NT_BUSY) {       /* the WiFi tab gets it free */
                    memcpy(W.aps, aps, (size_t)naps * sizeof *aps);
                    W.n = naps;
                    W.state = NT_DONE;
                    W.scanned_ms = (uint32_t)aos_hal_uptime_ms();
                    W.seq++;
                }
                nt_unlock();
            }
        }
        if (scan_cancel(&w)) { free(aps); goto stopped; }
        char file[48];
        survey_save(w.h, w.alive, count, w.full, range, aps, naps,
                    (uint32_t)((now_us() - w.t0) / 1000), file, sizeof file);
        free(aps);
        nt_lock();
        if (w.gen == s_scan_gen) snprintf(S.file, sizeof S.file, "%s", file);
        nt_unlock();
    }
    scan_publish(&w, 2, 1, 1);
    nt_lock();
    if (w.gen == s_scan_gen) { S.state = NT_DONE; S.seq++; }
    s_scan_run = false;
    nt_unlock();
    goto out;

fail:
    nt_lock();
    if (w.gen == s_scan_gen) {
        S.state = NT_FAILED;
        snprintf(S.err, sizeof S.err, "%s", err);
        S.seq++;
    }
    s_scan_run = false;
    nt_unlock();
    goto out;
stopped:
    nt_lock();
    if (S.state == NT_BUSY) S.state = NT_IDLE;
    S.seq++;
    s_scan_run = false;
    nt_unlock();
out:
    free(w.h);
    free(w.alive);
    free(w.expanded);
    free(w.q);
}

bool nt_scan_start(bool full)
{
    nt_keepalive();
    nt_lock();
    bool busy = s_scan_run;
    uint32_t gen = 0;
    if (!busy) {
        s_scan_run = true;
        uint32_t seq = S.seq + 1;
        memset(&S, 0, sizeof S);
        S.seq = seq;
        S.state = NT_BUSY;
        S.full = full;
        gen = ++s_scan_gen;
    }
    nt_unlock();
    if (busy) return false;
    if (!aos_hal_thread_start("nt_scan", scan_thread, (void *)(uintptr_t)gen, STACK + 2048, PRIO)) {
        nt_lock();
        s_scan_run = false;
        S.state = NT_FAILED;
        snprintf(S.err, sizeof S.err, "%s", N_("No hay memoria para empezar"));
        S.seq++;
        nt_unlock();
        return false;
    }
    return true;
}

void nt_scan_stop(void)
{
    nt_lock();
    s_scan_gen++;
    nt_unlock();
}

/* -------------------------------------------------------------------------- */
/* One host's ports                                                            */
/* -------------------------------------------------------------------------- */

static const uint16_t EXTRA_PORTS[] = {
    1400, 1880, 1883, 1884, 3000, 3306, 3389, 5000, 5001, 5432, 5555, 5900, 6053, 6668, 7000,
    8000, 8008, 8009, 8080, 8081, 8123, 8443, 8554, 8883, 8888, 9000, 9090, 9100, 32400,
};
#define N_EXTRA (int)(sizeof EXTRA_PORTS / sizeof EXTRA_PORTS[0])

typedef struct {
    uint32_t gen, ip;
    int next, total;
    int64_t t0, last_pub;
    uint16_t open[NT_MAX_OPEN];
    int nopen, closed, done;
} portscan_t;

static bool ports_cancel(void *ud)
{
    portscan_t *w = ud;
    nt_lock();
    bool stop = w->gen != s_ports_gen;
    nt_unlock();
    return stop || !app_alive();
}

static void ports_publish(portscan_t *w)
{
    nt_lock();
    if (w->gen == s_ports_gen) {
        memcpy(R.open, w->open, sizeof R.open);
        R.nopen = w->nopen;
        R.closed = w->closed;
        R.done = w->done;
        R.total = w->total;
        R.elapsed_ms = (uint32_t)((now_us() - w->t0) / 1000);
        R.seq++;
    }
    nt_unlock();
    w->last_pub = now_us();
}

static bool ports_next(void *ud, probe_t *out)
{
    portscan_t *w = ud;
    if (w->next >= w->total) return false;
    int i = w->next++;
    out->ip = w->ip;
    out->port = i < 1024 ? (uint16_t)(i + 1) : EXTRA_PORTS[i - 1024];
    out->tag = 0;
    return true;
}

static void ports_done(void *ud, const probe_t *p, int res, uint32_t us)
{
    portscan_t *w = ud;
    (void)us;
    w->done++;
    if (res == PR_CLOSED) w->closed++;
    if (res == PR_OPEN && w->nopen < NT_MAX_OPEN) {
        int i = w->nopen++;
        while (i > 0 && w->open[i - 1] > p->port) { w->open[i] = w->open[i - 1]; i--; }
        w->open[i] = p->port;
    }
    if (now_us() - w->last_pub > 200000) ports_publish(w);
}

static void ports_thread(void *arg)
{
    portscan_t *w = calloc(1, sizeof *w);
    if (!w) return;
    nt_lock();
    w->gen = (uint32_t)(uintptr_t)arg;
    w->ip = R.ip;
    nt_unlock();
    w->total = 1024 + N_EXTRA;
    w->t0 = now_us();
    probe_job_t job = { ports_next, ports_done, ports_cancel, w, PROBE_CONC, PROBE_TIMEOUT_MS };
    bool ok = probe_run(&job);
    ports_publish(w);
    nt_lock();
    if (w->gen == s_ports_gen) {
        R.state = ok ? NT_DONE : ports_cancel(w) ? NT_IDLE : NT_FAILED;
        if (!ok && R.state == NT_FAILED) snprintf(R.err, sizeof R.err, "%s", N_("Sin sockets libres: probá de nuevo en un rato"));
        R.seq++;
        /* what it found goes into the sweep's list too */
        for (int i = 0; i < S.n; i++) {
            if (S.hosts[i].ip != w->ip) continue;
            for (int k = 0; k < w->nopen; k++) host_add_port(&S.hosts[i], w->open[k]);
            S.seq++;
        }
    }
    s_ports_run = false;
    nt_unlock();
    free(w);
}

bool nt_ports_start(uint32_t ip)
{
    nt_keepalive();
    nt_lock();
    bool busy = s_ports_run;
    uint32_t gen = 0;
    if (!busy) {
        s_ports_run = true;
        uint32_t seq = R.seq + 1;
        memset(&R, 0, sizeof R);
        R.seq = seq;
        R.ip = ip;
        R.state = NT_BUSY;
        R.total = 1024 + N_EXTRA;
        gen = ++s_ports_gen;
    }
    nt_unlock();
    if (busy) return false;
    if (!aos_hal_thread_start("nt_ports", ports_thread, (void *)(uintptr_t)gen, STACK, PRIO)) {
        nt_lock();
        s_ports_run = false;
        R.state = NT_FAILED;
        snprintf(R.err, sizeof R.err, "%s", N_("No hay memoria para empezar"));
        R.seq++;
        nt_unlock();
        return false;
    }
    return true;
}

void nt_ports_stop(void)
{
    nt_lock();
    s_ports_gen++;
    if (R.state == NT_BUSY) R.state = NT_IDLE;
    R.seq++;
    nt_unlock();
}

/* -------------------------------------------------------------------------- */
/* WiFi                                                                        */
/* -------------------------------------------------------------------------- */

static void wifi_thread(void *arg)
{
    (void)arg;
    aos_wifi_ap_ex_t *buf = calloc(NT_MAX_APS, sizeof *buf);
    int64_t last = 0;
    while (app_alive() && buf) {
        nt_lock();
        bool want = s_wifi_want;
        nt_unlock();
        if (want) {
            int n = aos_hal_net_scan_ex(buf, NT_MAX_APS);
            nt_lock();
            s_wifi_want = false;
            W.state = n < 0 ? NT_FAILED : NT_DONE;
            W.n = n < 0 ? 0 : n;
            if (n > 0) memcpy(W.aps, buf, (size_t)n * sizeof *buf);
            W.scanned_ms = (uint32_t)aos_hal_uptime_ms();
            W.seq++;
            nt_unlock();
        }
        if (now_us() - last >= 500000) {
            last = now_us();
            aos_wifi_ap_ex_t cur;
            bool ok = aos_hal_net_ap_info(&cur);
            nt_lock();
            W.cur_ok = ok;
            if (ok) W.cur = cur;
            if (W.rssi_n == NT_RSSI_HIST) {
                memmove(W.rssi, W.rssi + 1, sizeof W.rssi - sizeof W.rssi[0]);
                W.rssi_n--;
            }
            W.rssi[W.rssi_n++] = ok ? (cur.rssi < 0 ? cur.rssi : -1) : 0;
            W.seq++;
            nt_unlock();
        }
        aos_hal_sleep_ms(50);
    }
    free(buf);
    nt_lock();
    s_wifi_run = false;
    if (W.state == NT_BUSY) W.state = NT_IDLE;
    nt_unlock();
}

void nt_wifi_run(void)
{
    nt_keepalive();
    nt_lock();
    bool start = !s_wifi_run;
    if (start) s_wifi_run = true;
    nt_unlock();
    if (start && !aos_hal_thread_start("nt_wifi", wifi_thread, NULL, STACK, PRIO)) {
        nt_lock();
        s_wifi_run = false;
        nt_unlock();
    }
}

bool nt_wifi_scan(void)
{
    nt_lock();
    bool busy = s_wifi_want;
    if (!busy) {
        s_wifi_want = true;
        W.state = NT_BUSY;
        W.seq++;
    }
    nt_unlock();
    nt_wifi_run();
    return !busy;
}
