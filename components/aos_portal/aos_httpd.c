/*
 * P4OS - the small HTTP server. The contract is in aos_httpd.h.
 *
 * Why not esp_http_server: the simulator does not have it, and the portal is
 * designed and tested there first. The part HTTP asks of a portal like this
 * is small - a request line, some headers, a body with Content-Length - and
 * having one implementation means the Mac finds the bugs the board would.
 */
#include "aos_httpd.h"
#include "aos_hal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>

#define HEAD_MAX   4096
#define IO_TIMEOUT 10000

struct aos_httpd_req {
    int fd;
    char head[HEAD_MAX + 1];
    size_t head_len;
    char *method, *target, *path, *query;
    char *hdr_name[32], *hdr_val[32];
    int nhdr;
    /* body bytes already read along with the headers */
    char *pre;
    size_t pre_len;
    long body_len, body_read;
    bool began;
};

static aos_httpd_handler_t s_handler;
static void *s_mx;
static int s_active;

/* ---- socket helpers ---- */

static bool wait_fd(int fd, bool wr, int ms)
{
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    return select(fd + 1, wr ? NULL : &set, wr ? &set : NULL, NULL, &tv) > 0;
}

static bool send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len) {
        if (!wait_fd(fd, true, IO_TIMEOUT)) return false;
        ssize_t n = send(fd, p, len, 0);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static int recv_some(int fd, void *buf, size_t len)
{
    if (!wait_fd(fd, false, IO_TIMEOUT)) return -1;
    ssize_t n = recv(fd, buf, len, 0);
    return n < 0 ? -1 : (int)n;
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s == '+' ? ' ' : *s;
        }
    }
    *o = 0;
}

/* ---- parsing ---- */

static bool read_head(aos_httpd_req_t *r)
{
    for (;;) {
        if (r->head_len >= HEAD_MAX) return false;
        int n = recv_some(r->fd, r->head + r->head_len, HEAD_MAX - r->head_len);
        if (n <= 0) return false;
        r->head_len += (size_t)n;
        r->head[r->head_len] = 0;
        char *end = strstr(r->head, "\r\n\r\n");
        if (!end) continue;
        *end = 0;
        r->pre = end + 4;
        r->pre_len = r->head_len - (size_t)(r->pre - r->head);
        break;
    }
    /* request line */
    char *line = r->head, *nl = strstr(line, "\r\n");
    if (nl) *nl = 0;
    r->method = line;
    char *sp = strchr(line, ' ');
    if (!sp) return false;
    *sp = 0;
    r->target = sp + 1;
    sp = strchr(r->target, ' ');
    if (sp) *sp = 0;
    /* headers */
    for (char *h = nl ? nl + 2 : NULL; h && *h && r->nhdr < 32; ) {
        char *e = strstr(h, "\r\n");
        if (e) *e = 0;
        char *c = strchr(h, ':');
        if (c) {
            *c = 0;
            char *v = c + 1;
            while (*v == ' ') v++;
            r->hdr_name[r->nhdr] = h;
            r->hdr_val[r->nhdr] = v;
            r->nhdr++;
        }
        h = e ? e + 2 : NULL;
    }
    /* path and query: the path is decoded here, the query per parameter */
    r->path = r->target;
    r->query = strchr(r->target, '?');
    if (r->query) *r->query++ = 0;
    url_decode(r->path);
    const char *cl = aos_httpd_header(r, "Content-Length");
    r->body_len = cl ? atol(cl) : -1;
    return true;
}

const char *aos_httpd_method(aos_httpd_req_t *r) { return r->method; }
const char *aos_httpd_path(aos_httpd_req_t *r) { return r->path; }
long aos_httpd_body_len(aos_httpd_req_t *r) { return r->body_len; }

bool aos_httpd_local_ip(aos_httpd_req_t *r, char *out, size_t out_len)
{
    struct sockaddr_in a;
    socklen_t l = sizeof a;
    if (getsockname(r->fd, (struct sockaddr *)&a, &l) || a.sin_family != AF_INET) return false;
    uint32_t ip = ntohl(a.sin_addr.s_addr);
    snprintf(out, out_len, "%u.%u.%u.%u", (unsigned)(ip >> 24), (unsigned)(ip >> 16 & 255),
             (unsigned)(ip >> 8 & 255), (unsigned)(ip & 255));
    return true;
}

const char *aos_httpd_header(aos_httpd_req_t *r, const char *name)
{
    for (int i = 0; i < r->nhdr; i++)
        if (!strcasecmp(r->hdr_name[i], name)) return r->hdr_val[i];
    return NULL;
}

bool aos_httpd_query(aos_httpd_req_t *r, const char *key, char *out, size_t out_len)
{
    if (!r->query) return false;
    size_t kl = strlen(key);
    for (const char *p = r->query; *p; ) {
        const char *amp = strchr(p, '&');
        size_t l = amp ? (size_t)(amp - p) : strlen(p);
        if (l >= kl && !strncmp(p, key, kl) && (p[kl] == '=' || l == kl)) {
            const char *v = p[kl] == '=' ? p + kl + 1 : p + kl;
            size_t vl = l - (size_t)(v - p);
            if (vl >= out_len) vl = out_len - 1;
            memcpy(out, v, vl);
            out[vl] = 0;
            url_decode(out);
            return true;
        }
        p += l + (amp ? 1 : 0);
        if (!amp) break;
    }
    return false;
}

long aos_httpd_query_int(aos_httpd_req_t *r, const char *key, long def)
{
    char v[24];
    return aos_httpd_query(r, key, v, sizeof v) ? strtol(v, NULL, 10) : def;
}

int aos_httpd_body_read(aos_httpd_req_t *r, void *buf, int len)
{
    long left = r->body_len - r->body_read;
    if (r->body_len < 0 || left <= 0) return 0;
    if (len > left) len = (int)left;
    int n;
    if (r->pre_len) {
        n = len < (int)r->pre_len ? len : (int)r->pre_len;
        memcpy(buf, r->pre, (size_t)n);
        r->pre += n;
        r->pre_len -= (size_t)n;
    } else {
        n = recv_some(r->fd, buf, (size_t)len);
        if (n <= 0) return -1;
    }
    r->body_read += n;
    return n;
}

char *aos_httpd_body_all(aos_httpd_req_t *r, size_t max)
{
    if (r->body_len < 0 || (size_t)r->body_len > max) return NULL;
    char *b = malloc((size_t)r->body_len + 1);
    if (!b) return NULL;
    long got = 0;
    while (got < r->body_len) {
        int n = aos_httpd_body_read(r, b + got, (int)(r->body_len - got));
        if (n <= 0) { free(b); return NULL; }
        got += n;
    }
    b[got] = 0;
    return b;
}

/* ---- answers ---- */

static const char *reason(int s)
{
    switch (s) {
    case 200: return "OK";
    case 204: return "No Content";
    case 206: return "Partial Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 403: return "Forbidden";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 416: return "Range Not Satisfiable";
    case 421: return "Misdirected Request";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "";
    }
}

bool aos_httpd_begin(aos_httpd_req_t *r, int status, const char *ctype, long len, const char *extra)
{
    if (r->began) return false;
    r->began = true;
    char h[512];
    /* nosniff: a file from the card is what its type says; DENY: no other
     * site can frame the portal and make the user click on it */
    int n = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nConnection: close\r\n"
                                  "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n"
                                  "X-Frame-Options: DENY\r\n",
                     status, reason(status), ctype ? ctype : "application/octet-stream");
    if (len >= 0) n += snprintf(h + n, sizeof h - n, "Content-Length: %ld\r\n", len);
    n += snprintf(h + n, sizeof h - n, "%s\r\n", extra ? extra : "");
    return send_all(r->fd, h, (size_t)n);
}

bool aos_httpd_write(aos_httpd_req_t *r, const void *data, size_t len)
{
    return len == 0 || send_all(r->fd, data, len);
}

void aos_httpd_send(aos_httpd_req_t *r, int status, const char *ctype, const void *data, size_t len)
{
    if (aos_httpd_begin(r, status, ctype, (long)len, NULL)) aos_httpd_write(r, data, len);
}

void aos_httpd_send_text(aos_httpd_req_t *r, int status, const char *text)
{
    aos_httpd_send(r, status, "text/plain; charset=utf-8", text, strlen(text));
}

void aos_httpd_send_json(aos_httpd_req_t *r, int status, const char *json)
{
    aos_httpd_send(r, status, "application/json", json, strlen(json));
}

/* ---- the server ---- */

static void conn_thread(void *arg)
{
    aos_httpd_req_t *r = arg;
    if (read_head(r)) {
        s_handler(r);
        if (!r->began) aos_httpd_send_text(r, 500, "no answer");
        /* drain what the client still sends so it sees our answer, not a reset */
        char junk[512];
        while (r->body_len > r->body_read && aos_httpd_body_read(r, junk, sizeof junk) > 0) {}
    }
    shutdown(r->fd, SHUT_WR);
    close(r->fd);
    free(r);
    aos_hal_mutex_lock(s_mx);
    s_active--;
    aos_hal_mutex_unlock(s_mx);
}

static void listen_thread(void *arg)
{
    int port = (int)(intptr_t)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (ls < 0 || bind(ls, (struct sockaddr *)&a, sizeof a) < 0 || listen(ls, 4) < 0) {
        aos_hal_log("httpd", "cannot listen on port %d (errno %d)", port, errno);
        if (ls >= 0) close(ls);
        return;
    }
    aos_hal_log("httpd", "portal on port %d", port);
    for (;;) {
        int fd = accept(ls, NULL, NULL);
        if (fd < 0) { aos_hal_sleep_ms(100); continue; }
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        aos_hal_mutex_lock(s_mx);
        bool room = s_active < AOS_HTTPD_CONNS;
        if (room) s_active++;
        aos_hal_mutex_unlock(s_mx);
        aos_httpd_req_t *r = room ? calloc(1, sizeof *r) : NULL;
        if (!r) {
            static const char busy[] = "HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
            send_all(fd, busy, sizeof busy - 1);
            close(fd);
            if (room) { aos_hal_mutex_lock(s_mx); s_active--; aos_hal_mutex_unlock(s_mx); }
            continue;
        }
        r->fd = fd;
        if (!aos_hal_thread_start("http", conn_thread, r, 12288, 3)) {
            close(fd);
            free(r);
            aos_hal_mutex_lock(s_mx);
            s_active--;
            aos_hal_mutex_unlock(s_mx);
        }
    }
}

bool aos_httpd_start(int port, aos_httpd_handler_t handler)
{
    if (s_handler) return true;
    s_handler = handler;
    s_mx = aos_hal_mutex_create();
    return aos_hal_thread_start("httpd", listen_thread, (void *)(intptr_t)port, 4096, 3);
}
