/*
 * P4OS - Grabador: an RTSP server, so Frigate, VLC, ffmpeg or the Cámaras app
 * take the board as an IP camera: rtsp://<board>:8554/
 *
 * Small on purpose:
 *   - RTP over the RTSP connection itself (interleaved, RFC 2326 10.12),
 *     channels 0-1: no UDP ports to open, and it goes through anything. A
 *     client that asks for UDP is answered 461 and asks again for TCP
 *     (ffmpeg, Frigate and VLC do; Frigate's go2rtc too).
 *   - H.264 as RFC 6184 says: a NAL in a packet when it fits, FU-A when not;
 *     the SPS and PPS go with every key frame, and in the SDP too once one
 *     has been seen.
 *   - A viewer starts at the next key frame, and asks the encoder for one
 *     (gr_rtsp_wants_key), so it sees a picture within a frame.
 *   - Two viewers at most: the board has four TCP handles in all, and the
 *     listening one is a third.
 *
 * One thread answers the requests of both; the camera's thread sends the
 * frames (gr_rtsp_push), each viewer's sends under its own lock.
 */
#include "gr.h"

#include <stdlib.h>
#include <strings.h>

#define MAX_VIEWERS 2
#define MTU_PAYLOAD 1400

typedef struct {
    int      h;                     /* the TCP handle, 0 free */
    bool     playing, want_key, dead;
    char     in[1536];
    int      in_n;
    uint16_t seq;
    uint32_t ssrc;
    uint32_t session;
    void    *mx;
} viewer_t;

static struct {
    volatile bool stop, running;
    int      port;
    int      lh;
    viewer_t v[MAX_VIEWERS];
    void    *mx;
    uint8_t  sps[64], pps[32];
    int      sps_len, pps_len;
    int      w, h;
} R;

static uint8_t *s_pkt;              /* one RTP packet with its interleave header */

/* ---------------------------------------------------------------- base64 */

static void b64(const uint8_t *in, int n, char *out, size_t on)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (int i = 0; i < n && o + 5 < on; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = T[v >> 18 & 63];
        out[o++] = T[v >> 12 & 63];
        out[o++] = i + 1 < n ? T[v >> 6 & 63] : '=';
        out[o++] = i + 2 < n ? T[v & 63] : '=';
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------- requests */

static void reply(viewer_t *c, int code, const char *reason, int cseq, const char *extra, const char *body)
{
    char h[1024];
    int bl = body ? (int)strlen(body) : 0;
    int n = snprintf(h, sizeof h, "RTSP/1.0 %d %s\r\nCSeq: %d\r\nServer: P4OS Grabador\r\n%s", code, reason, cseq,
                     extra ? extra : "");
    if (bl) n += snprintf(h + n, sizeof h - (size_t)n, "Content-Type: application/sdp\r\nContent-Length: %d\r\n", bl);
    n += snprintf(h + n, sizeof h - (size_t)n, "\r\n");
    aos_hal_mutex_lock(c->mx);
    bool ok = aos_hal_tcp_send(c->h, h, n, 2000) == n && (!bl || aos_hal_tcp_send(c->h, body, bl, 2000) == bl);
    aos_hal_mutex_unlock(c->mx);
    if (!ok) c->dead = true;
}

static const char *header(const char *req, const char *name, char *out, size_t n)
{
    size_t ln = strlen(name);
    for (const char *p = req; *p; p++) {
        if ((p == req || p[-1] == '\n') && !strncasecmp(p, name, ln) && p[ln] == ':') {
            p += ln + 1;
            while (*p == ' ') p++;
            size_t k = 0;
            while (p[k] && p[k] != '\r' && p[k] != '\n' && k + 1 < n) { out[k] = p[k]; k++; }
            out[k] = 0;
            return out;
        }
    }
    return NULL;
}

static void handle(viewer_t *c, char *req)
{
    char method[24] = "", tmp[256];
    sscanf(req, "%23s", method);
    int cseq = header(req, "CSeq", tmp, sizeof tmp) ? atoi(tmp) : 0;
    char sess[64];
    snprintf(sess, sizeof sess, "Session: %08X;timeout=60\r\n", (unsigned)c->session);
    if (!strcmp(method, "OPTIONS")) {
        reply(c, 200, "OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER, SET_PARAMETER\r\n", NULL);
    } else if (!strcmp(method, "DESCRIBE")) {
        char sdp[768], ps[256] = "";
        aos_hal_mutex_lock(R.mx);
        if (R.sps_len && R.pps_len) {
            char a[128], b[64];
            b64(R.sps, R.sps_len, a, sizeof a);
            b64(R.pps, R.pps_len, b, sizeof b);
            snprintf(ps, sizeof ps, ";sprop-parameter-sets=%s,%s;profile-level-id=%02X%02X%02X", a, b,
                     R.sps_len > 3 ? R.sps[1] : 0x42, R.sps_len > 3 ? R.sps[2] : 0xE0, R.sps_len > 3 ? R.sps[3] : 0x1F);
        }
        int w = R.w, h = R.h;
        aos_hal_mutex_unlock(R.mx);
        snprintf(sdp, sizeof sdp,
                 "v=0\r\no=- %u 1 IN IP4 0.0.0.0\r\ns=P4OS Grabador\r\nc=IN IP4 0.0.0.0\r\nt=0 0\r\n"
                 "a=control:*\r\nm=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 packetization-mode=1%s\r\na=framesize:96 %d-%d\r\na=control:track0\r\n",
                 (unsigned)c->session, ps, w, h);
        reply(c, 200, "OK", cseq, "Content-Base: rtsp://p4os/\r\n", sdp);
    } else if (!strcmp(method, "SETUP")) {
        if (!header(req, "Transport", tmp, sizeof tmp) || !strstr(tmp, "TCP")) {
            reply(c, 461, "Unsupported Transport", cseq, NULL, NULL);
            return;
        }
        char extra[160];
        snprintf(extra, sizeof extra, "Transport: RTP/AVP/TCP;unicast;interleaved=0-1;ssrc=%08X\r\n%s", (unsigned)c->ssrc, sess);
        reply(c, 200, "OK", cseq, extra, NULL);
    } else if (!strcmp(method, "PLAY")) {
        reply(c, 200, "OK", cseq, sess, NULL);
        c->want_key = true;
        c->playing = true;
        aos_hal_log("grabador", "RTSP: a viewer is watching");
    } else if (!strcmp(method, "TEARDOWN")) {
        reply(c, 200, "OK", cseq, sess, NULL);
        c->dead = true;
    } else if (!strcmp(method, "GET_PARAMETER") || !strcmp(method, "SET_PARAMETER")) {
        reply(c, 200, "OK", cseq, sess, NULL);
    } else {
        reply(c, 405, "Method Not Allowed", cseq, NULL, NULL);
    }
}

/* what came in: whole requests (head and body) handled; RTCP from the
 * client ('$' frames) read and dropped */
static void feed(viewer_t *c)
{
    for (;;) {
        if (c->in_n > 0 && c->in[0] == '$') {
            if (c->in_n < 4) return;
            int len = ((uint8_t)c->in[2] << 8 | (uint8_t)c->in[3]) + 4;
            if (len > (int)sizeof c->in) { c->dead = true; return; }
            if (c->in_n < len) return;
            memmove(c->in, c->in + len, (size_t)(c->in_n - len));
            c->in_n -= len;
            continue;
        }
        c->in[c->in_n] = 0;
        char *end = strstr(c->in, "\r\n\r\n");
        if (!end) {
            if (c->in_n >= (int)sizeof c->in - 1) c->dead = true;
            return;
        }
        int head = (int)(end - c->in) + 4;
        char tmp[16];
        int body = header(c->in, "Content-Length", tmp, sizeof tmp) ? atoi(tmp) : 0;
        if (body < 0 || head + body >= (int)sizeof c->in) { c->dead = true; return; }
        if (c->in_n < head + body) return;
        char save = c->in[head];
        c->in[head] = 0;
        handle(c, c->in);
        c->in[head] = save;
        memmove(c->in, c->in + head + body, (size_t)(c->in_n - head - body));
        c->in_n -= head + body;
    }
}

static void drop(viewer_t *c)
{
    aos_hal_mutex_lock(c->mx);
    if (c->h > 0) aos_hal_tcp_close(c->h);
    c->h = 0;
    c->playing = c->dead = false;
    c->in_n = 0;
    aos_hal_mutex_unlock(c->mx);
}

static void server(void *arg)
{
    (void)arg;
    R.lh = aos_hal_tcp_listen(R.port);
    if (R.lh <= 0) {
        aos_hal_log("grabador", "RTSP: port %d not free (%d)", R.port, R.lh);
        R.running = false;
        return;
    }
    aos_hal_log("grabador", "RTSP on port %d", R.port);
    while (!R.stop) {
        /* a new viewer, when there is room */
        int slot = -1;
        for (int i = 0; i < MAX_VIEWERS; i++) if (!R.v[i].h) { slot = i; break; }
        if (slot >= 0) {
            int h = aos_hal_tcp_accept(R.lh, 50, NULL, 0);
            if (h > 0) {
                viewer_t *c = &R.v[slot];
                aos_hal_mutex_lock(c->mx);
                c->h = h;
                c->in_n = 0;
                c->playing = c->dead = false;
                uint32_t r;
                aos_hal_random(&r, sizeof r);
                c->ssrc = r;
                c->session = r ^ 0x5A5A1234u;
                c->seq = (uint16_t)r;
                aos_hal_mutex_unlock(c->mx);
            }
        } else {
            aos_hal_sleep_ms(50);
        }
        for (int i = 0; i < MAX_VIEWERS; i++) {
            viewer_t *c = &R.v[i];
            if (!c->h) continue;
            if (c->dead) { drop(c); continue; }
            int n = aos_hal_tcp_recv(c->h, c->in + c->in_n, (int)sizeof c->in - 1 - c->in_n, 10);
            if (n == AOS_TCP_ERR_CLOSED || (n < 0 && n != AOS_TCP_ERR_TIMEOUT)) { drop(c); continue; }
            if (n > 0) { c->in_n += n; feed(c); }
        }
    }
    for (int i = 0; i < MAX_VIEWERS; i++) if (R.v[i].h) drop(&R.v[i]);
    aos_hal_tcp_close(R.lh);
    R.lh = 0;
    R.running = false;
}

/* ---------------------------------------------------------------- frames */

static bool send_rtp(viewer_t *c, const uint8_t *payload, int n, const uint8_t *fu, int fun, bool marker, uint32_t ts)
{
    int len = 12 + fun + n;
    uint8_t *p = s_pkt;
    p[0] = '$';
    p[1] = 0;
    p[2] = (uint8_t)(len >> 8);
    p[3] = (uint8_t)len;
    uint8_t *r = p + 4;
    r[0] = 0x80;
    r[1] = (uint8_t)(96 | (marker ? 0x80 : 0));
    r[2] = (uint8_t)(c->seq >> 8);
    r[3] = (uint8_t)c->seq;
    c->seq++;
    r[4] = (uint8_t)(ts >> 24); r[5] = (uint8_t)(ts >> 16); r[6] = (uint8_t)(ts >> 8); r[7] = (uint8_t)ts;
    r[8] = (uint8_t)(c->ssrc >> 24); r[9] = (uint8_t)(c->ssrc >> 16); r[10] = (uint8_t)(c->ssrc >> 8); r[11] = (uint8_t)c->ssrc;
    if (fun) memcpy(r + 12, fu, (size_t)fun);
    memcpy(r + 12 + fun, payload, (size_t)n);
    return aos_hal_tcp_send(c->h, p, len + 4, 300) == len + 4;
}

static void send_nal(viewer_t *c, const uint8_t *nal, int n, bool last, uint32_t ts)
{
    if (n <= MTU_PAYLOAD) {
        if (!send_rtp(c, nal, n, NULL, 0, last, ts)) c->dead = true;
        return;
    }
    /* FU-A: the NAL's header split into an indicator and a header */
    uint8_t fu[2] = { (uint8_t)((nal[0] & 0xE0) | 28), (uint8_t)(nal[0] & 0x1F) };
    const uint8_t *d = nal + 1;
    int left = n - 1;
    bool first = true;
    while (left > 0 && !c->dead) {
        int k = left > MTU_PAYLOAD ? MTU_PAYLOAD : left;
        fu[1] = (uint8_t)((nal[0] & 0x1F) | (first ? 0x80 : 0) | (k == left ? 0x40 : 0));
        if (!send_rtp(c, d, k, fu, 2, last && k == left, ts)) c->dead = true;
        d += k;
        left -= k;
        first = false;
    }
}

void gr_rtsp_push(const uint8_t *p, int len, bool key, uint32_t ms, int w, int h)
{
    if (!R.running || !s_pkt) return;
    /* the NALs, and the parameter sets kept for the SDP */
    const uint8_t *nal[16];
    int nl[16], nn = 0;
    for (int i = 0; i + 3 < len && nn < 16;) {
        int s = i;
        while (s + 3 < len && !(p[s] == 0 && p[s + 1] == 0 && (p[s + 2] == 1 || (p[s + 2] == 0 && p[s + 3] == 1)))) s++;
        if (s + 3 >= len) break;
        int a = s + (p[s + 2] == 1 ? 3 : 4), e = a;
        while (e + 3 < len && !(p[e] == 0 && p[e + 1] == 0 && (p[e + 2] == 1 || (p[e + 2] == 0 && p[e + 3] == 1)))) e++;
        if (e + 3 >= len) e = len;
        nal[nn] = p + a;
        nl[nn] = e - a;
        nn++;
        i = e;
    }
    if (key) {
        aos_hal_mutex_lock(R.mx);
        for (int k = 0; k < nn; k++) {
            int t = nl[k] ? nal[k][0] & 0x1F : 0;
            if (t == 7 && nl[k] <= (int)sizeof R.sps) { memcpy(R.sps, nal[k], (size_t)nl[k]); R.sps_len = nl[k]; }
            if (t == 8 && nl[k] <= (int)sizeof R.pps) { memcpy(R.pps, nal[k], (size_t)nl[k]); R.pps_len = nl[k]; }
        }
        R.w = w;
        R.h = h;
        aos_hal_mutex_unlock(R.mx);
    }
    uint32_t ts = ms * 90;
    for (int i = 0; i < MAX_VIEWERS; i++) {
        viewer_t *c = &R.v[i];
        if (!c->h || !c->playing || c->dead) continue;
        if (c->want_key && !key) continue;
        c->want_key = false;
        aos_hal_mutex_lock(c->mx);
        for (int k = 0; k < nn && !c->dead; k++) send_nal(c, nal[k], nl[k], k == nn - 1, ts);
        aos_hal_mutex_unlock(c->mx);
    }
}

/* ---------------------------------------------------------------- the API */

bool gr_rtsp_start(int port)
{
    if (R.running) return true;
    if (!R.mx) R.mx = aos_hal_mutex_create();
    for (int i = 0; i < MAX_VIEWERS; i++) if (!R.v[i].mx) R.v[i].mx = aos_hal_mutex_create();
    if (!s_pkt) s_pkt = malloc(4 + 12 + 2 + MTU_PAYLOAD + 16);
    if (!R.mx || !s_pkt) return false;
    R.port = port;
    R.stop = false;
    R.running = true;
    if (!aos_hal_thread_start("gr_rtsp", server, NULL, 6144, 3)) { R.running = false; return false; }
    return true;
}

void gr_rtsp_stop(void)
{
    R.stop = true;
    for (int t = 0; R.running && t < 3000; t += 20) aos_hal_sleep_ms(20);
}

int gr_rtsp_viewers(void)
{
    int n = 0;
    for (int i = 0; i < MAX_VIEWERS; i++) n += R.v[i].h && R.v[i].playing && !R.v[i].dead;
    return n;
}

bool gr_rtsp_wants_key(void)
{
    for (int i = 0; i < MAX_VIEWERS; i++) if (R.v[i].h && R.v[i].playing && R.v[i].want_key) return true;
    return false;
}
