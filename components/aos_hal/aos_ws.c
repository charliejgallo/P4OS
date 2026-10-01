/*
 * P4OS - WebSocket client (RFC 6455), shared between the board and the
 * simulator. It sits on the HAL's connection (aos_tls.c, the one aos_http.c
 * uses too), so it is the same file on both: nothing here knows which
 * platform it runs on.
 *
 * Written for Home Assistant's WebSocket API, which decides its shape:
 *   - one text message at a time, whole: HA's answer to get_states is a
 *     single message of hundreds of KB, so the receive buffer grows on
 *     demand (on the board a large malloc lands in PSRAM) and shrinks back
 *     after an oversized one;
 *   - pings from the server are answered here, the caller never sees them;
 *   - fragmented messages are joined;
 *   - ws:// and wss:// (2026-09-28). wss:// is what reaches HA from outside
 *     the house: Nabu Casa (https://xxxx.ui.nabu.casa) or a reverse proxy
 *     with a real certificate. The TLS is aos_http.c's, moved to aos_tls.c
 *     so both share it: the same trust store (the IDF bundle on the board,
 *     /etc/ssl/cert.pem plus P4_SIM_EXTRA_CA in the simulator), the same
 *     "no valid time, no verification" rule, the same session cache (a full
 *     handshake is 1.6-1.8 s on the board, a resumed one 0.6). Everything
 *     above the transport - framing, masking, pings - does not know which
 *     one it has. Why a TLS connection was refused (untrusted authority,
 *     wrong name, no time) goes to the log as a "[ws]" line, since open
 *     only answers NULL.
 *
 * The connection is not one of the apps' aos_hal_tcp_* slots any more, and it
 * is opened with TCP_NODELAY (a frame goes out as header + payload writes).
 *
 * For a service's thread: every call has a timeout, none of them may run in
 * LVGL's task. With wss:// the thread's stack must also hold a certificate
 * chain verification: ~4.5 KB at its deepest, measured for aos_http.c.
 */
#include "aos_hal.h"
#include "aos_tls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WS_KEEP_BYTES   (64 * 1024)     /* the buffer is shrunk back to this */
#define WS_MAX_MESSAGE  (8 * 1024 * 1024)

struct aos_ws {
    aos_tls_t *conn;                /* in PSRAM on the board */
    /* raw bytes from the socket not yet parsed */
    uint8_t *raw;
    size_t raw_len, raw_cap;
    /* the message being put together */
    char *msg;
    size_t msg_len, msg_cap;
    bool in_fragment;
    bool closed;
};

/* ---- helpers ---- */

static uint32_t ws_rand(void)
{
    static uint32_t s;
    if (!s) s = (uint32_t)time(NULL) ^ (uint32_t)aos_hal_uptime_ms() ^ 0x9E3779B9u;
    s ^= s << 13; s ^= s >> 17; s ^= s << 5;        /* xorshift: masking is not a secret */
    return s;
}

static void b64(const uint8_t *in, int n, char *out)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int o = 0;
    for (int i = 0; i < n; i += 3) {
        uint32_t v = in[i] << 16 | (i + 1 < n ? in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

static bool grow(void **buf, size_t *cap, size_t need)
{
    if (need <= *cap) return true;
    size_t c = *cap ? *cap : 4096;
    while (c < need) c *= 2;
    void *p = realloc(*buf, c);
    if (!p) return false;
    *buf = p;
    *cap = c;
    return true;
}

/* ws://host[:port][/path] or wss://host[:port][/path] */
static bool parse_url(const char *url, bool *tls, char *host, size_t hn, int *port, char *path, size_t pn)
{
    const char *h;
    if (!strncmp(url, "ws://", 5)) {
        *tls = false;
        h = url + 5;
    } else if (!strncmp(url, "wss://", 6)) {
        *tls = true;
        h = url + 6;
    } else {
        return false;
    }
    const char *end = h + strcspn(h, ":/");
    if (end == h || (size_t)(end - h) >= hn) return false;
    memcpy(host, h, end - h);
    host[end - h] = 0;
    *port = *tls ? 443 : 80;
    if (*end == ':') *port = atoi(end + 1);
    const char *p = strchr(end, '/');
    snprintf(path, pn, "%s", p ? p : "/");
    return *port > 0 && *port < 65536;
}

static int send_all(aos_ws_t *ws, const void *data, int len, int timeout_ms)
{
    if (!aos_tls_send(ws->conn, data, len, timeout_ms)) { ws->closed = true; return -1; }
    return 0;
}

static int send_frame(aos_ws_t *ws, int opcode, const void *data, size_t len, int timeout_ms)
{
    uint8_t h[14];
    int n = 0;
    h[n++] = 0x80 | (opcode & 0x0F);                 /* FIN: never fragmented on the way out */
    if (len < 126) {
        h[n++] = 0x80 | (uint8_t)len;
    } else if (len < 65536) {
        h[n++] = 0x80 | 126;
        h[n++] = (uint8_t)(len >> 8);
        h[n++] = (uint8_t)len;
    } else {
        h[n++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--) h[n++] = (uint8_t)((uint64_t)len >> (8 * i));
    }
    uint32_t m = ws_rand();
    uint8_t mask[4] = { (uint8_t)(m >> 24), (uint8_t)(m >> 16), (uint8_t)(m >> 8), (uint8_t)m };
    memcpy(h + n, mask, 4);
    n += 4;
    if (send_all(ws, h, n, timeout_ms)) return -1;
    /* the payload goes masked, a chunk at a time */
    uint8_t chunk[512];
    const uint8_t *p = data;
    for (size_t off = 0; off < len; ) {
        size_t k = len - off < sizeof chunk ? len - off : sizeof chunk;
        for (size_t i = 0; i < k; i++) chunk[i] = p[off + i] ^ mask[(off + i) & 3];
        if (send_all(ws, chunk, (int)k, timeout_ms)) return -1;
        off += k;
    }
    return 0;
}

/* ---- opening ---- */

/* Why the last open failed, for the user: the TLS layer's line or the
 * refused upgrade. One process-wide string: the last failure wins. */
static char s_last_error[112];

const char *aos_hal_ws_last_error(void) { return s_last_error; }

aos_ws_t *aos_hal_ws_open(const char *url, const char *extra_headers, int timeout_ms)
{
    s_last_error[0] = 0;
    char host[128], path[256];
    int port;
    bool tls;
    if (!url || !parse_url(url, &tls, host, sizeof host, &port, path, sizeof path)) return NULL;
    aos_ws_t *ws = calloc(1, sizeof *ws);
    if (!ws) return NULL;
    ws->conn = aos_tls_new();
    if (!ws->conn) { free(ws); return NULL; }
    char why[112];
    if (aos_tls_open(ws->conn, host, port, (tls ? AOS_TLS_ENCRYPT : 0) | AOS_TLS_NODELAY,
                     timeout_ms, why, sizeof why) != 0) {
        aos_hal_log("ws", "%s://%s:%d: %s", tls ? "wss" : "ws", host, port, why);
        snprintf(s_last_error, sizeof s_last_error, "%s", why);
        ws->closed = true;              /* nothing to say goodbye on */
        goto fail;
    }

    uint8_t key[16];
    for (int i = 0; i < 16; i += 4) {
        uint32_t r = ws_rand();
        memcpy(key + i, &r, 4);
    }
    char key64[32];
    b64(key, 16, key64);
    /* Host carries the port only when it is not the scheme's own: some
     * reverse proxies route on the exact Host and "example.com:443" is not
     * "example.com" to them. */
    char hostport[140];
    if (port == (tls ? 443 : 80)) snprintf(hostport, sizeof hostport, "%s", host);
    else                          snprintf(hostport, sizeof hostport, "%s:%d", host, port);
    char req[768];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n%s\r\n",
                     path, hostport, key64, extra_headers ? extra_headers : "");
    if (n >= (int)sizeof req || send_all(ws, req, n, timeout_ms)) goto fail;

    /* the answer's headers; anything after them is already the first frames */
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    for (;;) {
        if (!grow((void **)&ws->raw, &ws->raw_cap, ws->raw_len + 1024)) goto fail;
        int r = aos_tls_recv(ws->conn, ws->raw + ws->raw_len, 1024, 200);
        if (r == AOS_TLS_TIMEOUT) r = 0;
        else if (r <= 0) goto fail;
        ws->raw_len += r;
        ws->raw[ws->raw_len] = 0;       /* grow() left room */
        char *end = strstr((char *)ws->raw, "\r\n\r\n");
        if (end) {
            if (strncmp((char *)ws->raw, "HTTP/1.1 101", 12)) {
                aos_hal_log("ws", "%s refused the upgrade: %.40s", host, (char *)ws->raw);
                snprintf(s_last_error, sizeof s_last_error, "the server refused the WebSocket upgrade");
                goto fail;
            }
            size_t hl = (size_t)(end + 4 - (char *)ws->raw);
            memmove(ws->raw, ws->raw + hl, ws->raw_len - hl);
            ws->raw_len -= hl;
            return ws;
        }
        if (ws->raw_len > 8192 || (uint32_t)aos_hal_uptime_ms() - t0 > (uint32_t)timeout_ms) goto fail;
    }
fail:
    aos_hal_ws_close(ws);
    return NULL;
}

/* ---- sending ---- */

int aos_hal_ws_send_text(aos_ws_t *ws, const char *text, int len)
{
    if (!ws || ws->closed) return -1;
    if (len < 0) len = (int)strlen(text);
    return send_frame(ws, 0x1, text, (size_t)len, 5000);
}

/* ---- receiving ---- */

/* One frame out of ws->raw if it is all there: 1 = a frame was consumed,
 * 0 = need more bytes, -1 = protocol error. A finished text message sets
 * *done. */
static int parse_one(aos_ws_t *ws, bool *done)
{
    if (ws->raw_len < 2) return 0;
    const uint8_t *b = ws->raw;
    bool fin = b[0] & 0x80;
    int op = b[0] & 0x0F;
    bool masked = b[1] & 0x80;
    uint64_t len = b[1] & 0x7F;
    size_t h = 2;
    if (len == 126) {
        if (ws->raw_len < 4) return 0;
        len = (uint64_t)b[2] << 8 | b[3];
        h = 4;
    } else if (len == 127) {
        if (ws->raw_len < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; i++) len = len << 8 | b[2 + i];
        h = 10;
    }
    if (len > WS_MAX_MESSAGE) return -1;
    size_t mh = masked ? 4 : 0;
    if (ws->raw_len < h + mh + len) {
        /* make room for the whole frame so recv can land it */
        if (!grow((void **)&ws->raw, &ws->raw_cap, h + mh + len + 1)) return -1;
        return 0;
    }
    uint8_t *pl = ws->raw + h + mh;
    if (masked)
        for (uint64_t i = 0; i < len; i++) pl[i] ^= b[h + (i & 3)];

    switch (op) {
    case 0x1:   /* text */
    case 0x0:   /* continuation */
        if (op == 0x1) { ws->msg_len = 0; ws->in_fragment = !fin; }
        if (!grow((void **)&ws->msg, &ws->msg_cap, ws->msg_len + len + 1)) return -1;
        memcpy(ws->msg + ws->msg_len, pl, len);
        ws->msg_len += len;
        ws->msg[ws->msg_len] = 0;
        if (fin) { ws->in_fragment = false; *done = true; }
        break;
    case 0x2:   /* binary: HA never sends it; dropped */
        break;
    case 0x8:   /* close */
        send_frame(ws, 0x8, pl, len >= 2 ? 2 : 0, 1000);
        ws->closed = true;
        break;
    case 0x9:   /* ping */
        send_frame(ws, 0xA, pl, len, 2000);
        break;
    default:    /* pong and anything else */
        break;
    }
    size_t used = h + mh + len;
    memmove(ws->raw, ws->raw + used, ws->raw_len - used);
    ws->raw_len -= used;
    return 1;
}

int aos_hal_ws_recv_text(aos_ws_t *ws, const char **out, int timeout_ms)
{
    if (!ws || ws->closed) return -1;
    /* the previous big message is done with: give the memory back */
    if (ws->msg_cap > WS_KEEP_BYTES && !ws->in_fragment) {
        free(ws->msg);
        ws->msg = NULL;
        ws->msg_cap = ws->msg_len = 0;
    }
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    for (;;) {
        bool done = false;
        int r;
        while ((r = parse_one(ws, &done)) == 1)
            if (done) { *out = ws->msg; return (int)ws->msg_len; }
        if (r < 0) { ws->closed = true; return -1; }
        if (ws->closed) return -1;
        int left = timeout_ms - (int)((uint32_t)aos_hal_uptime_ms() - t0);
        if (left <= 0) return 0;
        if (!grow((void **)&ws->raw, &ws->raw_cap, ws->raw_len + 16384)) return -1;
        size_t room = ws->raw_cap - ws->raw_len - 1;
        int n = aos_tls_recv(ws->conn, ws->raw + ws->raw_len, room > 65536 ? 65536 : (int)room,
                             left < 100 ? left : 100);
        if (n == AOS_TLS_TIMEOUT) n = 0;
        else if (n <= 0) { ws->closed = true; return -1; }
        ws->raw_len += n;
        if (ws->raw_cap > WS_KEEP_BYTES * 4 && ws->raw_len < WS_KEEP_BYTES && !n) {
            uint8_t *p = realloc(ws->raw, WS_KEEP_BYTES);
            if (p) { ws->raw = p; ws->raw_cap = WS_KEEP_BYTES; }
        }
    }
}

bool aos_hal_ws_is_open(aos_ws_t *ws) { return ws && !ws->closed; }

void aos_hal_ws_close(aos_ws_t *ws)
{
    if (!ws) return;
    if (!ws->closed && ws->conn) {
        uint8_t code[2] = { 0x03, 0xE8 };           /* 1000: normal */
        send_frame(ws, 0x8, code, 2, 500);
    }
    aos_tls_free(ws->conn);
    free(ws->raw);
    free(ws->msg);
    free(ws);
}
