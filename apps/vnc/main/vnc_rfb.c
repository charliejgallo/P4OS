/*
 * P4OS - VNC viewer: the session (RFB, RFC 6143, and the Tight extensions).
 *
 * One session at a time, in the app's worker. It owns the socket, the
 * remote screen and the two view buffers; the UI talks to it through a
 * handful of fields under one mutex:
 *
 *   input   the UI queues pointer and key events; the worker sends them
 *           between messages and while it waits for the rest of a big one
 *   view    the UI says how it wants to look at the screen (vnc_view_t);
 *           the worker renders it
 *   frames  two view buffers. A new view (zoom, pan, the screen turned) is
 *           rendered whole into the one the canvas is NOT showing and
 *           handed over; an update of the remote screen is rendered, only
 *           where it changed, straight into the one it is showing (a frame
 *           may show half an update, as every VNC viewer does). A whole
 *           render waits until the UI has taken the previous one, so the
 *           buffer under the canvas is never the one being rewritten.
 *
 * The protocol, as far as the board needs it:
 *
 *   versions   3.3, 3.7 and 3.8 (and Apple's 3.889, which is 3.8 here)
 *   security   None (1) and VNC (2: DES of a challenge, vnc_des.c). Apple's
 *              own types are not spoken: on a Mac, Screen Sharing offers VNC
 *              when "VNC viewers may control screen with password" is on
 *   format     RGB565 little-endian (LVGL's own: Raw lands as it comes), or
 *              32-bit for exact colours (the "24 bits" setting)
 *   encodings  Raw, CopyRect, Hextile, ZRLE, Tight (zlib streams, palette,
 *              gradient and JPEG, decoded by the P4's engine), and the
 *              pseudo-encodings DesktopSize, ExtendedDesktopSize (only
 *              read: the monitors it lists) and LastRect
 *   zones      one monitor of several (a Mac with two sends them as one
 *              screen): the kept screen, the update requests and the
 *              pointer all move to that rectangle, so the rest costs
 *              neither memory nor network
 *
 * Errors inside a message (a closed socket, bad data, the app leaving)
 * longjmp back to the session's loop, so the decoders read like the
 * specification instead of checking every byte.
 */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC optimize("O2")
#endif
#include "vnc.h"
#include "vnc_des.h"
#include "tinfl.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RD_CAP          (64 * 1024)
#define STAGE_CAP       (64 * 1024)
#define EVQ             128
#define STACK_BYTES     12288
#define CONNECT_MS      6000
#define STALL_MS        30000       /* no byte for this long inside a message */
#define WAIT_SLICE_MS   15
#define PRESENT_MS      60          /* render during a long update this often */
#define STATS_MS        2000
#define LOG_EVERY       5           /* stats periods per log line */
#define JPEG_CAP        (4 * 1024 * 1024)

/* encodings */
#define ENC_RAW         0
#define ENC_COPYRECT    1
#define ENC_HEXTILE     5
#define ENC_TIGHT       7
#define ENC_ZRLE        16
#define ENC_DESKTOPSIZE (-223)
#define ENC_LASTRECT    (-224)
#define ENC_EXTDESKTOP  (-308)
#define ENC_QUALITY0    (-32)
#define ENC_COMPRESS0   (-256)

enum { EV_POINTER = 1, EV_KEY };

typedef struct {
    uint8_t  type;
    uint8_t  buttons;
    bool     down;
    uint16_t x, y;
    uint32_t key;
} ev_t;

/* one zlib stream of the connection, fed from the socket as it decodes */
typedef struct {
    tinfl_decompressor *inf;
    uint8_t  *dict;             /* tinfl's 32 KB window: its output */
    size_t    dict_ofs;         /* where it writes next */
    size_t    out_ofs, out_len; /* written and not yet staged */
    uint32_t  in_left;          /* compressed bytes of this rectangle still to feed */
} zs_t;

struct vnc_sess {
    vnc_server_t srv;
    void        *mx;
    volatile bool stop;
    volatile int  owner;        /* 0 both, 1 worker gone, 2 UI gone */
    jmp_buf      jb;

    /* for the UI, under mx */
    volatile int state;
    char         detail[160];
    bool         have_desktop;
    int          rw, rh, shift;     /* the zone shown */
    int          full_w, full_h;    /* the server's whole screen */
    vnc_rect_t   cur;               /* the zone, in the server's pixels */
    vnc_rect_t   scr[VNC_MAX_SCREENS];
    int          nscr;
    bool         want_zone;
    vnc_rect_t   zone;              /* asked for; w 0: all of it */
    uint32_t     zone_gen;
    char         name[96];
    char         stats[96];
    ev_t         q[EVQ];
    int          qn;
    bool         want_refresh;
    vnc_view_t   want;
    int          front, ui_front;
    bool         new_front;
    int          bw[2], bh[2];
    bool         has_dirty;
    lv_area_t    dirty;

    /* the worker's own */
    int          sock;
    uint8_t     *rd;
    int          rd_pos, rd_len;
    uint64_t     last_in;
    vnc_fb_t     fb;
    vnc_render_t *ren;
    vnc_view_t   have;          /* the view the front buffer holds */
    bool         have_valid;
    uint16_t    *vbuf[2];
    size_t       vcap[2];       /* pixels */
    int          bpp;           /* 2 or 4 bytes a pixel */
    uint8_t     *stage;
    int          st_pos, st_len;
    zs_t         zrle, tz[4];
    uint16_t    *tile;          /* 64 x 64, and a row of up to 8192 */
    uint8_t     *grad;          /* Tight's gradient: two rows of components */
    aos_jpeg_t  *jpeg;
    uint8_t     *jbuf;
    size_t       jcap;
    uint64_t     last_present;
    bool         update_open;

    /* counters */
    uint32_t     n_updates, n_bytes, n_render_ms, n_renders, n_dec_ms;
    /* for the log line, between two of them: input sent and held back
     * (look only), and updates that came with nothing in them */
    uint32_t     n_ptr, n_key, n_held, n_empty;
    uint64_t     stats_t0;
    int          stats_n;
    int          last_enc;
    volatile uint32_t bells;
    volatile bool paused;       /* the app is in the background */
    bool         req_owed;      /* an update request held back while paused */
};

/* ---- small things ----------------------------------------------------------- */

static bool stopping(vnc_sess_t *s)
{
    return s->stop || aos_hal_worker_should_stop();
}

static void set_state(vnc_sess_t *s, vnc_state_t st, const char *detail)
{
    aos_hal_mutex_lock(s->mx);
    if (detail) snprintf(s->detail, sizeof s->detail, "%s", detail);
    s->state = st;
    aos_hal_mutex_unlock(s->mx);
}

/* Ends the session from anywhere inside it. */
static void fail(vnc_sess_t *s, const char *why)
{
    if (why && s->state != VNC_ST_ERROR) {
        aos_hal_log(VNC_TAG, "%s: %s", s->srv.name, why);
        set_state(s, VNC_ST_ERROR, why);
    }
    longjmp(s->jb, 1);
}

static const char *enc_label(int enc)
{
    switch (enc) {
    case ENC_RAW: return "Raw";
    case ENC_COPYRECT: return "CopyRect";
    case ENC_HEXTILE: return "Hextile";
    case ENC_TIGHT: return "Tight";
    case ENC_TIGHT + 1000: return "Tight JPEG";
    case ENC_ZRLE: return "ZRLE";
    default: return "?";
    }
}

/* ---- sending ---------------------------------------------------------------- */

static void send_all(vnc_sess_t *s, const void *data, int len)
{
    if (aos_hal_tcp_send(s->sock, data, len, 5000) != len) {
        fail(s, _("Se cortó la conexión"));
    }
}

static void put16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void request_area(vnc_sess_t *s, bool incremental, int x, int y, int w, int h)
{
    uint8_t m[10] = { 3, incremental ? 1 : 0 };
    put16(m + 2, (uint32_t)x);
    put16(m + 4, (uint32_t)y);
    put16(m + 6, (uint32_t)w);
    put16(m + 8, (uint32_t)h);
    send_all(s, m, sizeof m);
}

/* only the zone: the server sends nothing of the rest */
static void send_update_request(vnc_sess_t *s, bool incremental)
{
    request_area(s, incremental, s->cur.x, s->cur.y, s->cur.w, s->cur.h);
}

static void apply_zone(vnc_sess_t *s, bool force);

/* What the UI queued: sent in one go. Called between messages and while
 * the reader waits, so a click goes out even in the middle of a big update. */
static void pump(vnc_sess_t *s)
{
    ev_t q[EVQ];
    aos_hal_mutex_lock(s->mx);
    int n = s->qn;
    memcpy(q, s->q, (size_t)n * sizeof(ev_t));
    s->qn = 0;
    bool refresh = s->want_refresh;
    s->want_refresh = false;
    bool zone = s->want_zone;
    s->want_zone = false;
    aos_hal_mutex_unlock(s->mx);
    /* safe in the middle of an update too: the decoders reach the kept
     * screen only through vnc_fb_*, which cut to whatever zone is there */
    if (zone && s->have_desktop) {
        apply_zone(s, false);
        refresh = true;
    }
    if (s->req_owed && !s->paused) {
        s->req_owed = false;
        refresh = true;             /* back from the background: all of it */
    }
    if (refresh && s->have_desktop) {
        send_update_request(s, false);
    }
    if (s->srv.view_only) s->n_held += (uint32_t)n;
    if (!n || s->srv.view_only) {
        return;
    }
    uint8_t out[EVQ * 8];
    int len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t *m = out + len;
        if (q[i].type == EV_POINTER) {
            m[0] = 5;
            m[1] = q[i].buttons;
            put16(m + 2, (uint32_t)(q[i].x + s->cur.x));
            put16(m + 4, (uint32_t)(q[i].y + s->cur.y));
            len += 6;
            s->n_ptr++;
        } else {
            m[0] = 4;
            m[1] = q[i].down ? 1 : 0;
            m[2] = m[3] = 0;
            put32(m + 4, q[i].key);
            len += 8;
            s->n_key++;
        }
    }
    send_all(s, out, len);
}

/* ---- reading ---------------------------------------------------------------- */

/* At least 'need' bytes buffered (need <= RD_CAP). */
static void rd_fill(vnc_sess_t *s, int need)
{
    if (s->rd_len - s->rd_pos >= need) {
        return;
    }
    if (s->rd_pos) {
        memmove(s->rd, s->rd + s->rd_pos, (size_t)(s->rd_len - s->rd_pos));
        s->rd_len -= s->rd_pos;
        s->rd_pos = 0;
    }
    while (s->rd_len < need) {
        if (stopping(s)) fail(s, NULL);
        pump(s);
        int n = aos_hal_tcp_recv(s->sock, s->rd + s->rd_len, RD_CAP - s->rd_len, 50);
        uint64_t now = aos_hal_uptime_ms();
        if (n > 0) {
            s->rd_len += n;
            s->n_bytes += (uint32_t)n;
            s->last_in = now;
        } else if (n == 0) {
            if (now - s->last_in > STALL_MS) fail(s, _("El servidor dejó de mandar datos"));
        } else {
            fail(s, _("El servidor cerró la conexión"));
        }
    }
}

static const uint8_t *rd_ptr(vnc_sess_t *s, int n)
{
    rd_fill(s, n);
    const uint8_t *p = s->rd + s->rd_pos;
    s->rd_pos += n;
    return p;
}

static uint8_t rd_u8(vnc_sess_t *s)
{
    return *rd_ptr(s, 1);
}

static uint16_t rd_u16(vnc_sess_t *s)
{
    const uint8_t *p = rd_ptr(s, 2);
    return (uint16_t)(p[0] << 8 | p[1]);
}

static uint32_t rd_u32(vnc_sess_t *s)
{
    const uint8_t *p = rd_ptr(s, 4);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void rd_bytes(vnc_sess_t *s, void *dst, size_t n)
{
    uint8_t *d = dst;
    while (n) {
        int chunk = n > RD_CAP / 2 ? RD_CAP / 2 : (int)n;
        memcpy(d, rd_ptr(s, chunk), (size_t)chunk);
        d += chunk;
        n -= (size_t)chunk;
    }
}

static void rd_skip(vnc_sess_t *s, size_t n)
{
    while (n) {
        int chunk = n > RD_CAP / 2 ? RD_CAP / 2 : (int)n;
        rd_ptr(s, chunk);
        n -= (size_t)chunk;
    }
}

/* A reason string (a failed handshake), kept short. */
static void rd_reason(vnc_sess_t *s, char *out, size_t n)
{
    uint32_t len = rd_u32(s);
    size_t keep = len < n - 1 ? len : n - 1;
    rd_bytes(s, out, keep);
    out[keep] = '\0';
    rd_skip(s, len - keep);
}

/* ---- pixels ----------------------------------------------------------------- */

static inline uint16_t rgb565(uint32_t r, uint32_t g, uint32_t b)
{
    return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3);
}

/* a pixel in our format: 16 bits as LVGL keeps them, or 32 with red at 16 */
static inline uint16_t pix(const vnc_sess_t *s, const uint8_t *p)
{
    if (s->bpp == 2) return (uint16_t)(p[0] | p[1] << 8);
    return rgb565(p[2], p[1], p[0]);
}

/* ZRLE's CPIXEL: 32-bit pixels travel as their three low bytes */
static inline int cpx_size(const vnc_sess_t *s)
{
    return s->bpp == 2 ? 2 : 3;
}

/* Tight's TPIXEL: 24-bit colour travels as R, G, B */
static inline int tpx_size(const vnc_sess_t *s)
{
    return s->bpp == 2 ? 2 : 3;
}

static inline uint16_t tpix(const vnc_sess_t *s, const uint8_t *p)
{
    if (s->bpp == 2) return (uint16_t)(p[0] | p[1] << 8);
    return rgb565(p[0], p[1], p[2]);
}

/* ---- zlib streams ------------------------------------------------------------- */

static bool zs_init(zs_t *z)
{
    if (!z->inf) {
        z->inf = vnc_psram(sizeof(tinfl_decompressor));
        z->dict = vnc_psram(TINFL_LZ_DICT_SIZE);
        if (!z->inf || !z->dict) return false;
    }
    tinfl_init(z->inf);
    z->dict_ofs = 0;
    z->out_ofs = 0;
    z->out_len = 0;
    z->in_left = 0;
    return true;
}

static void zs_free(zs_t *z)
{
    free(z->inf);
    free(z->dict);
    memset(z, 0, sizeof *z);
}

/* Feeds the stream from the socket until it has produced something; with
 * drain, until this rectangle's bytes are all in (output or not). */
static void zs_inflate(vnc_sess_t *s, zs_t *z, bool drain)
{
    for (;;) {
        if (!z->in_left) {
            if (drain) return;
            fail(s, _("Datos comprimidos incompletos"));
        }
        if (s->rd_len == s->rd_pos) rd_fill(s, 1);
        size_t avail = (size_t)(s->rd_len - s->rd_pos);
        if (avail > z->in_left) avail = z->in_left;
        size_t in_sz = avail, out_sz = TINFL_LZ_DICT_SIZE - z->dict_ofs;
        tinfl_status st = tinfl_decompress(z->inf, s->rd + s->rd_pos, &in_sz, z->dict, z->dict + z->dict_ofs,
                                           &out_sz, TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT);
        s->rd_pos += (int)in_sz;
        z->in_left -= (uint32_t)in_sz;
        if (st < TINFL_STATUS_DONE) fail(s, _("Datos comprimidos dañados"));
        if (out_sz) {
            z->out_ofs = z->dict_ofs;
            z->out_len = out_sz;
            z->dict_ofs = (z->dict_ofs + out_sz) & (TINFL_LZ_DICT_SIZE - 1);
            return;
        }
        if (!in_sz) {
            /* tinfl took nothing: it needs bytes that have not come yet */
            if (avail >= z->in_left || s->rd_len - s->rd_pos >= RD_CAP) fail(s, _("Datos comprimidos dañados"));
            rd_fill(s, s->rd_len - s->rd_pos + 1);
        }
    }
}

/* n contiguous inflated bytes (n <= STAGE_CAP). */
static const uint8_t *zs_ptr(vnc_sess_t *s, zs_t *z, int n)
{
    if (s->st_len - s->st_pos < n) {
        if (s->st_pos) {
            memmove(s->stage, s->stage + s->st_pos, (size_t)(s->st_len - s->st_pos));
            s->st_len -= s->st_pos;
            s->st_pos = 0;
        }
        while (s->st_len < n) {
            if (!z->out_len) zs_inflate(s, z, false);
            size_t take = z->out_len;
            if (take > (size_t)(STAGE_CAP - s->st_len)) take = (size_t)(STAGE_CAP - s->st_len);
            memcpy(s->stage + s->st_len, z->dict + z->out_ofs, take);
            s->st_len += (int)take;
            z->out_ofs += take;
            z->out_len -= take;
        }
    }
    const uint8_t *p = s->stage + s->st_pos;
    s->st_pos += n;
    return p;
}

static uint8_t zs_u8(vnc_sess_t *s, zs_t *z)
{
    return *zs_ptr(s, z, 1);
}

static void zs_begin(vnc_sess_t *s, zs_t *z, uint32_t len)
{
    z->in_left = len;
    s->st_pos = s->st_len = 0;
}

/* The rectangle is decoded: what is left of its bytes still goes through
 * the stream (the end of a sync flush), or the next rectangle's window
 * would be off. Inflated bytes nobody asked for mean the two sides
 * disagree: say so in the log, once a rectangle. */
static void zs_end(vnc_sess_t *s, zs_t *z)
{
    size_t extra = z->out_len + (size_t)(s->st_len - s->st_pos);
    z->out_len = 0;
    while (z->in_left) {
        zs_inflate(s, z, true);
        extra += z->out_len;
        z->out_len = 0;
    }
    s->st_pos = s->st_len = 0;
    if (extra) aos_hal_log(VNC_TAG, "zlib: %u bytes left over in a rectangle", (unsigned)extra);
}

/* ---- the view ------------------------------------------------------------------ */

typedef struct {
    vnc_sess_t *s;
    uint16_t   *dst;
    const vnc_view_t *v;
    int         y0, y1;
} split_job_t;

static void render_half(void *arg, int part)
{
    split_job_t *j = arg;
    int mid = (j->y0 + j->y1) / 2;
    if (part == 0) vnc_render(j->s->ren, &j->s->fb, j->v, j->dst, 0, j->y0, j->v->vw, mid);
    else vnc_render(j->s->ren, &j->s->fb, j->v, j->dst, 0, mid, j->v->vw, j->y1);
}

static bool view_buf(vnc_sess_t *s, int b, size_t px)
{
    if (s->vcap[b] >= px) return true;
    free(s->vbuf[b]);
    s->vbuf[b] = vnc_psram(px * 2);
    s->vcap[b] = s->vbuf[b] ? px : 0;
    return s->vbuf[b] != NULL;
}

/* The picture for the UI: a whole new one when the view changed (and the UI
 * took the last), else what changed of the remote screen. */
static void present(vnc_sess_t *s)
{
    if (!s->fb.px) return;
    aos_hal_mutex_lock(s->mx);
    vnc_view_t want = s->want;
    bool taken = s->ui_front == s->front;
    int front = s->front;
    aos_hal_mutex_unlock(s->mx);
    if (want.vw <= 0 || want.vh <= 0 || want.scale <= 0.0f) return;

    uint64_t t0 = aos_hal_uptime_ms();
    if (!s->have_valid || want.gen != s->have.gen) {
        if (!taken) return;
        int b = front < 0 ? 0 : 1 - front;
        if (!view_buf(s, b, (size_t)want.vw * want.vh)) return;
        /* the first row on this core prepares the tables; the rest in halves */
        vnc_render(s->ren, &s->fb, &want, s->vbuf[b], 0, 0, want.vw, 1);
        split_job_t j = { s, s->vbuf[b], &want, 1, want.vh };
        if (!aos_hal_worker_split(render_half, &j)) {
            render_half(&j, 0);
            render_half(&j, 1);
        }
        s->have = want;
        s->have_valid = true;
        s->fb.dx0 = s->fb.dx1 = 0;
        aos_hal_mutex_lock(s->mx);
        s->front = b;
        s->bw[b] = want.vw;
        s->bh[b] = want.vh;
        s->new_front = true;
        s->has_dirty = false;
        aos_hal_mutex_unlock(s->mx);
    } else if (s->fb.dx0 < s->fb.dx1 && front >= 0) {
        int x0, y0, x1, y1;
        bool in = vnc_view_rect(&s->fb, &s->have, s->fb.dx0, s->fb.dy0, s->fb.dx1, s->fb.dy1, &x0, &y0, &x1, &y1);
        s->fb.dx0 = s->fb.dx1 = 0;
        if (!in) return;
        vnc_render(s->ren, &s->fb, &s->have, s->vbuf[front], x0, y0, x1, y1);
        aos_hal_mutex_lock(s->mx);
        if (!s->has_dirty) {
            s->dirty.x1 = x0;
            s->dirty.y1 = y0;
            s->dirty.x2 = x1 - 1;
            s->dirty.y2 = y1 - 1;
            s->has_dirty = true;
        } else {
            if (x0 < s->dirty.x1) s->dirty.x1 = x0;
            if (y0 < s->dirty.y1) s->dirty.y1 = y0;
            if (x1 - 1 > s->dirty.x2) s->dirty.x2 = x1 - 1;
            if (y1 - 1 > s->dirty.y2) s->dirty.y2 = y1 - 1;
        }
        aos_hal_mutex_unlock(s->mx);
    } else {
        return;
    }
    s->n_render_ms += (uint32_t)(aos_hal_uptime_ms() - t0);
    s->n_renders++;
    s->last_present = aos_hal_uptime_ms();
}

static void stats(vnc_sess_t *s)
{
    uint64_t now = aos_hal_uptime_ms();
    uint64_t dt = now - s->stats_t0;
    if (dt < STATS_MS) return;
    uint32_t fps10 = (uint32_t)((uint64_t)s->n_updates * 10000 / dt);
    uint32_t kbps = (uint32_t)((uint64_t)s->n_bytes * 8 / dt);
    uint32_t rms = s->n_renders ? s->n_render_ms / s->n_renders : 0;
    char line[96];
    snprintf(line, sizeof line, "%dx%d%s · %s · %u,%u fps · %u kbit/s", s->rw, s->rh,
             s->fb.shift ? (s->fb.shift == 1 ? " (1/2)" : " (1/4)") : "", enc_label(s->last_enc),
             (unsigned)(fps10 / 10), (unsigned)(fps10 % 10), (unsigned)kbps);
    aos_hal_mutex_lock(s->mx);
    snprintf(s->stats, sizeof s->stats, "%s", line);
    aos_hal_mutex_unlock(s->mx);
    if (++s->stats_n % LOG_EVERY == 0 && s->n_updates) {
        aos_hal_log(VNC_TAG, "%s; render %u ms, decode %u ms an update; sent %u pointer, %u key (%u held back); "
                    "%u empty updates", line, (unsigned)rms, (unsigned)(s->n_dec_ms / s->n_updates),
                    (unsigned)s->n_ptr, (unsigned)s->n_key, (unsigned)s->n_held, (unsigned)s->n_empty);
        s->n_ptr = s->n_key = s->n_held = s->n_empty = 0;
    }
    s->n_updates = s->n_bytes = s->n_render_ms = s->n_renders = s->n_dec_ms = 0;
    s->stats_t0 = now;
}

/* ---- the encodings --------------------------------------------------------------- */

static void dec_raw(vnc_sess_t *s, int x, int y, int w, int h)
{
    int rowb = w * s->bpp;
    if (rowb > RD_CAP) fail(s, _("Rectángulo demasiado ancho"));
    for (int r = 0; r < h; r++) {
        const uint8_t *p = rd_ptr(s, rowb);
        if (vnc_fb_skip_row(&s->fb, y + r)) continue;
        if (s->bpp == 2) {
            memcpy(s->tile, p, (size_t)rowb);
        } else {
            for (int i = 0; i < w; i++) s->tile[i] = pix(s, p + i * 4);
        }
        vnc_fb_row(&s->fb, x, y + r, w, s->tile);
    }
}

static void dec_hextile(vnc_sess_t *s, int x, int y, int w, int h)
{
    uint16_t bg = 0, fg = 0;
    uint16_t *t = s->tile;
    for (int ty = y; ty < y + h; ty += 16) {
        int th = y + h - ty < 16 ? y + h - ty : 16;
        for (int tx = x; tx < x + w; tx += 16) {
            int tw = x + w - tx < 16 ? x + w - tx : 16;
            uint8_t se = rd_u8(s);
            if (se & 1) {
                const uint8_t *p = rd_ptr(s, tw * th * s->bpp);
                for (int i = 0; i < tw * th; i++) t[i] = pix(s, p + i * s->bpp);
                vnc_fb_rect(&s->fb, tx, ty, tw, th, t, tw);
                continue;
            }
            if (se & 2) bg = pix(s, rd_ptr(s, s->bpp));
            if (se & 4) fg = pix(s, rd_ptr(s, s->bpp));
            if (!(se & 8)) {
                vnc_fb_fill(&s->fb, tx, ty, tw, th, bg);
                continue;
            }
            for (int i = 0; i < tw * th; i++) t[i] = bg;
            int n = rd_u8(s);
            int each = (se & 16) ? s->bpp + 2 : 2;
            const uint8_t *p = rd_ptr(s, n * each);
            for (int k = 0; k < n; k++) {
                uint16_t c = fg;
                if (se & 16) {
                    c = pix(s, p);
                    p += s->bpp;
                }
                int sx = p[0] >> 4, sy = p[0] & 15, sw = (p[1] >> 4) + 1, sh = (p[1] & 15) + 1;
                p += 2;
                for (int yy = sy; yy < sy + sh && yy < th; yy++) {
                    for (int xx = sx; xx < sx + sw && xx < tw; xx++) t[yy * tw + xx] = c;
                }
            }
            vnc_fb_rect(&s->fb, tx, ty, tw, th, t, tw);
        }
    }
}

/* ZRLE's run length: 1 + the bytes, while they are 255 */
static int zrle_run(vnc_sess_t *s, zs_t *z)
{
    int run = 1;
    uint8_t b;
    do {
        b = zs_u8(s, z);
        run += b;
    } while (b == 255);
    return run;
}

static void dec_zrle(vnc_sess_t *s, int x, int y, int w, int h)
{
    zs_t *z = &s->zrle;
    if (!z->inf && !zs_init(z)) fail(s, _("Sin memoria para ZRLE"));
    zs_begin(s, z, rd_u32(s));
    const int cs = cpx_size(s);
    uint16_t *t = s->tile;
    uint16_t pal[128];
    for (int ty = y; ty < y + h; ty += 64) {
        int th = y + h - ty < 64 ? y + h - ty : 64;
        for (int tx = x; tx < x + w; tx += 64) {
            int tw = x + w - tx < 64 ? x + w - tx : 64;
            int n = tw * th;
            uint8_t sub = zs_u8(s, z);
            if (sub == 0) {
                const uint8_t *p = zs_ptr(s, z, n * cs);
                for (int i = 0; i < n; i++) t[i] = pix(s, p + i * cs);     /* 3 bytes: the low ones */
            } else if (sub == 1) {
                uint16_t c = pix(s, zs_ptr(s, z, cs));
                vnc_fb_fill(&s->fb, tx, ty, tw, th, c);
                continue;
            } else if (sub <= 16) {
                const uint8_t *p = zs_ptr(s, z, sub * cs);
                for (int i = 0; i < sub; i++) pal[i] = pix(s, p + i * cs);
                int bits = sub == 2 ? 1 : sub <= 4 ? 2 : 4;
                int rowb = (tw * bits + 7) / 8;
                const uint8_t *q = zs_ptr(s, z, rowb * th);
                uint8_t mask = (uint8_t)((1 << bits) - 1);
                for (int r = 0; r < th; r++) {
                    const uint8_t *row = q + r * rowb;
                    for (int c = 0; c < tw; c++) {
                        int bit = c * bits;
                        int idx = (row[bit >> 3] >> (8 - bits - (bit & 7))) & mask;
                        t[r * tw + c] = pal[idx];
                    }
                }
            } else if (sub == 128) {
                for (int i = 0; i < n;) {
                    uint16_t c = pix(s, zs_ptr(s, z, cs));
                    int run = zrle_run(s, z);
                    if (run > n - i) fail(s, _("ZRLE: una tira se pasa del bloque"));
                    while (run--) t[i++] = c;
                }
            } else if (sub >= 130) {
                int np = sub - 128;
                const uint8_t *p = zs_ptr(s, z, np * cs);
                for (int i = 0; i < np; i++) pal[i] = pix(s, p + i * cs);
                for (int i = 0; i < n;) {
                    uint8_t idx = zs_u8(s, z);
                    int run = 1;
                    if (idx & 128) {
                        idx &= 127;
                        run = zrle_run(s, z);
                    }
                    if (idx >= np || run > n - i) fail(s, _("ZRLE: datos inválidos"));
                    while (run--) t[i++] = pal[idx];
                }
            } else {
                fail(s, _("ZRLE: datos inválidos"));
            }
            vnc_fb_rect(&s->fb, tx, ty, tw, th, t, tw);
        }
    }
    zs_end(s, z);
}

/* Tight's compact length: 7 bits a byte, up to three */
static uint32_t tight_len(vnc_sess_t *s)
{
    uint8_t b = rd_u8(s);
    uint32_t len = b & 0x7F;
    if (b & 0x80) {
        b = rd_u8(s);
        len |= (uint32_t)(b & 0x7F) << 7;
        if (b & 0x80) len |= (uint32_t)rd_u8(s) << 14;
    }
    return len;
}

static void dec_tight_jpeg(vnc_sess_t *s, int x, int y, int w, int h)
{
    uint32_t len = tight_len(s);
    if (len > JPEG_CAP) fail(s, _("JPEG demasiado grande"));
    if (len > s->jcap) {
        free(s->jbuf);
        s->jcap = len < 256 * 1024 ? 256 * 1024 : len;
        s->jbuf = vnc_psram(s->jcap);
        if (!s->jbuf) {
            s->jcap = 0;
            fail(s, _("Sin memoria para el JPEG"));
        }
    }
    rd_bytes(s, s->jbuf, len);
    if (!s->jpeg && !(s->jpeg = aos_hal_jpeg_open())) fail(s, _("El decodificador JPEG no abrió"));
    int jw = 0, jh = 0, stride = 0;
    const uint16_t *px = aos_hal_jpeg_decode(s->jpeg, s->jbuf, len, &jw, &jh, &stride);
    if (!px) {
        aos_hal_log(VNC_TAG, "a %dx%d JPEG did not decode", w, h);
        return;                     /* the next update paints it again */
    }
    vnc_fb_rect(&s->fb, x, y, jw < w ? jw : w, jh < h ? jh : h, px, stride);
}

static void dec_tight(vnc_sess_t *s, int x, int y, int w, int h)
{
    uint8_t cc = rd_u8(s);
    for (int i = 0; i < 4; i++) {
        if ((cc & (1 << i)) && s->tz[i].inf) tinfl_init(s->tz[i].inf);
    }
    int type = cc >> 4;
    const int ts = tpx_size(s);
    if (type == 8) {                                        /* fill */
        vnc_fb_fill(&s->fb, x, y, w, h, tpix(s, rd_ptr(s, ts)));
        return;
    }
    if (type == 9) {
        s->last_enc = ENC_TIGHT + 1000;
        dec_tight_jpeg(s, x, y, w, h);
        return;
    }
    if (type > 9) fail(s, _("Tight: compresión desconocida"));
    if (w > 2048) fail(s, _("Tight: rectángulo demasiado ancho"));

    int filter = (type & 4) ? rd_u8(s) : 0;
    uint16_t pal[256];
    int npal = 0, rowb;
    if (filter == 1) {
        npal = rd_u8(s) + 1;
        const uint8_t *p = rd_ptr(s, npal * ts);
        for (int i = 0; i < npal; i++) pal[i] = tpix(s, p + i * ts);
        rowb = npal == 2 ? (w + 7) / 8 : w;
    } else if (filter == 0 || filter == 2) {
        rowb = w * ts;
    } else {
        fail(s, _("Tight: filtro desconocido"));
        return;
    }

    zs_t *z = NULL;
    uint8_t small[16];
    int small_pos = 0;
    if (rowb * h < 12) {
        rd_bytes(s, small, (size_t)(rowb * h));            /* too short to compress */
    } else {
        z = &s->tz[type & 3];
        if (!z->inf && !zs_init(z)) fail(s, _("Sin memoria para Tight"));
        zs_begin(s, z, tight_len(s));
    }

    /* gradient: the previous row's components, three a pixel */
    int16_t *prev = (int16_t *)s->grad, *cur = prev + 3 * 2048;
    if (filter == 2) memset(prev, 0, 3 * 2048 * sizeof(int16_t));
    const int max0 = s->bpp == 2 ? 31 : 255, max1 = s->bpp == 2 ? 63 : 255;
    uint16_t *row = s->tile;
    for (int r = 0; r < h; r++) {
        const uint8_t *p;
        if (z) {
            p = zs_ptr(s, z, rowb);
        } else {
            p = small + small_pos;
            small_pos += rowb;
        }
        if (filter == 1) {
            if (npal == 2) {
                for (int c = 0; c < w; c++) row[c] = pal[(p[c >> 3] >> (7 - (c & 7))) & 1];
            } else {
                for (int c = 0; c < w; c++) row[c] = pal[p[c] < npal ? p[c] : 0];
            }
        } else if (filter == 0) {
            for (int c = 0; c < w; c++) row[c] = tpix(s, p + c * ts);
        } else {
            /* each component predicted from left + up - up-left, clamped */
            for (int c = 0; c < w; c++) {
                int comp[3];
                if (s->bpp == 2) {
                    uint16_t v = (uint16_t)(p[c * 2] | p[c * 2 + 1] << 8);
                    comp[0] = v >> 11;
                    comp[1] = (v >> 5) & 63;
                    comp[2] = v & 31;
                } else {
                    comp[0] = p[c * 3];
                    comp[1] = p[c * 3 + 1];
                    comp[2] = p[c * 3 + 2];
                }
                for (int k = 0; k < 3; k++) {
                    int max = k == 1 ? max1 : max0;
                    int left = c ? cur[(c - 1) * 3 + k] : 0;
                    int up = prev[c * 3 + k];
                    int ul = c ? prev[(c - 1) * 3 + k] : 0;
                    int pred = left + up - ul;
                    pred = pred < 0 ? 0 : pred > max ? max : pred;
                    cur[c * 3 + k] = (int16_t)((pred + comp[k]) & max);
                }
                if (s->bpp == 2) {
                    row[c] = (uint16_t)(cur[c * 3] << 11 | cur[c * 3 + 1] << 5 | cur[c * 3 + 2]);
                } else {
                    row[c] = rgb565((uint32_t)cur[c * 3], (uint32_t)cur[c * 3 + 1], (uint32_t)cur[c * 3 + 2]);
                }
            }
            int16_t *t = prev;
            prev = cur;
            cur = t;
        }
        vnc_fb_row(&s->fb, x, y + r, w, row);
    }
    if (z) zs_end(s, z);
}

/* ---- messages ---------------------------------------------------------------------- */

/* The zone asked for, cut to the screen; all of it when it does not fit
 * (a monitor unplugged since it was saved). */
static vnc_rect_t zone_for(const vnc_sess_t *s, vnc_rect_t z)
{
    vnc_rect_t all = { 0, 0, s->full_w, s->full_h };
    if (z.w <= 0 || z.h <= 0 || z.x < 0 || z.y < 0 || z.x >= s->full_w || z.y >= s->full_h) return all;
    if (z.x + z.w > s->full_w) z.w = s->full_w - z.x;
    if (z.y + z.h > s->full_h) z.h = s->full_h - z.y;
    if (z.w < 64 || z.h < 64) return all;
    return z;
}

/* The kept screen for the zone asked for. force: even if it is the same
 * (the server's screen changed under it). */
static void apply_zone(vnc_sess_t *s, bool force)
{
    aos_hal_mutex_lock(s->mx);
    vnc_rect_t z = zone_for(s, s->zone);
    aos_hal_mutex_unlock(s->mx);
    if (!force && s->fb.px && !memcmp(&z, &s->cur, sizeof z)) return;
    if (!vnc_fb_alloc(&s->fb, z.w, z.h)) fail(s, _("La pantalla remota no entra en la memoria"));
    s->fb.zx = z.x;
    s->fb.zy = z.y;
    s->cur = z;
    s->have_valid = false;
    aos_hal_mutex_lock(s->mx);
    s->rw = z.w;
    s->rh = z.h;
    s->shift = s->fb.shift;
    s->have_desktop = true;
    s->zone_gen++;
    aos_hal_mutex_unlock(s->mx);
    aos_hal_log(VNC_TAG, "showing %dx%d at %d,%d of %dx%d, kept at 1/%d: %dx%d", z.w, z.h, z.x, z.y, s->full_w,
                s->full_h, 1 << s->fb.shift, s->fb.w, s->fb.h);
}

static void desktop_size(vnc_sess_t *s, int w, int h)
{
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) fail(s, _("Tamaño de pantalla inválido"));
    aos_hal_mutex_lock(s->mx);
    s->full_w = w;
    s->full_h = h;
    aos_hal_mutex_unlock(s->mx);
    apply_zone(s, true);
}

/* ExtendedDesktopSize: the screen's size and the monitors in it. Read
 * whole before the lock: reading may wait, and the wait pumps. */
static bool ext_desktop(vnc_sess_t *s, int w, int h)
{
    int n = rd_u8(s);
    rd_ptr(s, 3);
    vnc_rect_t scr[VNC_MAX_SCREENS];
    int k = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = rd_ptr(s, 16);
        vnc_rect_t r = { p[4] << 8 | p[5], p[6] << 8 | p[7], p[8] << 8 | p[9], p[10] << 8 | p[11] };
        if (k < VNC_MAX_SCREENS && r.w > 0 && r.h > 0) scr[k++] = r;
    }
    aos_hal_mutex_lock(s->mx);
    memcpy(s->scr, scr, sizeof scr);
    s->nscr = k;
    aos_hal_mutex_unlock(s->mx);
    aos_hal_log(VNC_TAG, "the server lists %d monitor(s) in %dx%d", k, w, h);
    if (w == s->full_w && h == s->full_h && s->fb.px) return false;
    desktop_size(s, w, h);
    return true;
}

static void fb_update(vnc_sess_t *s)
{
    uint64_t t0 = aos_hal_uptime_ms();
    rd_u8(s);
    int n = rd_u16(s);
    bool resized = false;
    if (!n) s->n_empty++;
    for (int i = 0; n == 0xFFFF || i < n; i++) {
        int x = rd_u16(s), y = rd_u16(s), w = rd_u16(s), h = rd_u16(s);
        int32_t enc = (int32_t)rd_u32(s);
        if (enc == ENC_LASTRECT) break;
        if (enc == ENC_DESKTOPSIZE) {
            desktop_size(s, w, h);
            resized = true;
            continue;
        }
        if (enc == ENC_EXTDESKTOP) {
            if (ext_desktop(s, w, h)) resized = true;
            continue;
        }
        if (enc >= 0 && (x + w > s->full_w || y + h > s->full_h)) {
            fail(s, _("Un rectángulo cae fuera de la pantalla"));
        }
        switch (enc) {
        case ENC_RAW:      dec_raw(s, x, y, w, h); break;
        case ENC_COPYRECT: {
            int sx = rd_u16(s), sy = rd_u16(s);
            /* from outside the zone: that part is asked for again */
            if (!vnc_fb_copy(&s->fb, sx, sy, x, y, w, h)) request_area(s, false, x, y, w, h);
            break;
        }
        case ENC_HEXTILE:  dec_hextile(s, x, y, w, h); break;
        case ENC_ZRLE:     dec_zrle(s, x, y, w, h); break;
        case ENC_TIGHT:    dec_tight(s, x, y, w, h); break;
        default: {
            char m[80];
            snprintf(m, sizeof m, _("Codificación %d no soportada"), (int)enc);
            fail(s, m);
        }
        }
        if (enc != ENC_COPYRECT && !(enc == ENC_TIGHT && s->last_enc == ENC_TIGHT + 1000)) s->last_enc = enc;
        if (aos_hal_uptime_ms() - s->last_present > PRESENT_MS) {
            present(s);
        }
    }
    s->n_dec_ms += (uint32_t)(aos_hal_uptime_ms() - t0);
    s->n_updates++;
    /* the next one is asked for before this one is drawn: it travels
     * meanwhile. In the background nothing is asked: the server goes quiet
     * and the connection stays. */
    if (s->paused) s->req_owed = true;
    else send_update_request(s, !resized);
    present(s);
}

static void handle_message(vnc_sess_t *s)
{
    uint8_t type = rd_u8(s);
    switch (type) {
    case 0:
        fb_update(s);
        break;
    case 1: {                       /* colour map: we asked for true colour */
        rd_u8(s);
        rd_u16(s);
        int n = rd_u16(s);
        rd_skip(s, (size_t)n * 6);
        break;
    }
    case 2:                         /* bell: the UI rings it */
        s->bells++;
        break;
    case 3: {                       /* the server's clipboard: not used yet */
        rd_skip(s, 3);
        rd_skip(s, rd_u32(s));
        break;
    }
    default: {
        char m[80];
        snprintf(m, sizeof m, _("Mensaje desconocido del servidor (%d)"), type);
        fail(s, m);
    }
    }
}

/* ---- the handshake ---------------------------------------------------------------- */

static void security_failed(vnc_sess_t *s, bool with_reason)
{
    char why[120] = "";
    if (with_reason) rd_reason(s, why, sizeof why);
    char m[160];
    if (why[0]) snprintf(m, sizeof m, _("El servidor rechazó la conexión: %s"), why);
    else snprintf(m, sizeof m, "%s", _("El servidor rechazó la conexión"));
    fail(s, m);
}

static void vnc_auth(vnc_sess_t *s)
{
    if (!s->srv.pass[0]) fail(s, _("El servidor pide contraseña: cargala en la computadora"));
    uint8_t ch[16], resp[16];
    rd_bytes(s, ch, 16);
    vnc_des_response(s->srv.pass, ch, resp);
    send_all(s, resp, 16);
}

static void handshake(vnc_sess_t *s)
{
    const uint8_t *v = rd_ptr(s, 12);
    if (memcmp(v, "RFB ", 4) != 0) fail(s, _("No es un servidor VNC"));
    int major = atoi((const char *)v + 4), minor = atoi((const char *)v + 8);
    /* 3.889 is Apple's 3.8; anything newer speaks 3.8 too */
    int ver = major > 3 || minor >= 8 ? 8 : minor >= 7 ? 7 : 3;
    char mine[13];
    snprintf(mine, sizeof mine, "RFB 003.00%d\n", ver);
    send_all(s, mine, 12);
    aos_hal_log(VNC_TAG, "%s: server RFB %d.%d, speaking 3.%d", s->srv.name, major, minor, ver);

    set_state(s, VNC_ST_AUTH, NULL);
    int type = 0;
    if (ver == 3) {
        type = (int)rd_u32(s);
        if (type == 0) security_failed(s, true);
    } else {
        int n = rd_u8(s);
        if (n == 0) security_failed(s, true);
        const uint8_t *t = rd_ptr(s, n);
        bool none = false, vnc = false;
        char list[48] = "";
        for (int i = 0; i < n; i++) {
            none |= t[i] == 1;
            vnc |= t[i] == 2;
            size_t l = strlen(list);
            snprintf(list + l, sizeof list - l, "%s%d", i ? " " : "", t[i]);
        }
        aos_hal_log(VNC_TAG, "%s: security types %s", s->srv.name, list);
        if (vnc && (s->srv.pass[0] || !none)) type = 2;
        else if (none) type = 1;
        else {
            char m[160];
            /* Apple offers 30 and 35 and, with the option on, 2 */
            if (memchr(t, 30, (size_t)n) || memchr(t, 35, (size_t)n)) {
                snprintf(m, sizeof m, "%s", _("La Mac no ofrece contraseña VNC: activá «Los visores VNC pueden controlar la pantalla con contraseña» en Compartir pantalla"));
            } else {
                snprintf(m, sizeof m, _("Ningún tipo de seguridad que la placa conozca (%s)"), list);
            }
            fail(s, m);
        }
        uint8_t c = (uint8_t)type;
        send_all(s, &c, 1);
    }
    if (type == 2) {
        vnc_auth(s);
    } else if (type != 1) {
        char m[80];
        snprintf(m, sizeof m, _("Tipo de seguridad %d no soportado"), type);
        fail(s, m);
    }
    if (type == 2 || ver == 8) {
        uint32_t r = rd_u32(s);
        if (r != 0) {
            if (ver == 8) {
                char why[120];
                rd_reason(s, why, sizeof why);
                aos_hal_log(VNC_TAG, "%s: auth failed: %s", s->srv.name, why);
            }
            fail(s, type == 2 ? _("Contraseña incorrecta") : _("El servidor rechazó la conexión"));
        }
    }

    uint8_t shared = 1;
    send_all(s, &shared, 1);
    int w = rd_u16(s), h = rd_u16(s);
    rd_skip(s, 16);                 /* the server's pixel format: ours replaces it */
    char name[96];
    rd_reason(s, name, sizeof name);
    aos_hal_mutex_lock(s->mx);
    snprintf(s->name, sizeof s->name, "%s", name);
    aos_hal_mutex_unlock(s->mx);
    aos_hal_log(VNC_TAG, "%s: \"%s\" %dx%d", s->srv.name, name, w, h);
    desktop_size(s, w, h);

    /* SetPixelFormat */
    uint8_t pf[20] = { 0 };
    pf[0] = 0;
    if (s->srv.depth == 24) {
        s->bpp = 4;
        pf[4] = 32; pf[5] = 24; pf[6] = 0; pf[7] = 1;
        put16(pf + 8, 255); put16(pf + 10, 255); put16(pf + 12, 255);
        pf[14] = 16; pf[15] = 8; pf[16] = 0;
    } else {
        s->bpp = 2;
        pf[4] = 16; pf[5] = 16; pf[6] = 0; pf[7] = 1;
        put16(pf + 8, 31); put16(pf + 10, 63); put16(pf + 12, 31);
        pf[14] = 11; pf[15] = 5; pf[16] = 0;
    }
    send_all(s, pf, sizeof pf);

    /* SetEncodings: the chosen one first, then what every server has */
    int32_t encs[12];
    int n = 0;
    switch (s->srv.enc) {
    case VNC_ENC_TIGHT:   encs[n++] = ENC_TIGHT; break;
    case VNC_ENC_ZRLE:    encs[n++] = ENC_ZRLE; break;
    case VNC_ENC_HEXTILE: encs[n++] = ENC_HEXTILE; break;
    case VNC_ENC_RAW:     break;
    default:
        encs[n++] = ENC_TIGHT;
        encs[n++] = ENC_ZRLE;
        encs[n++] = ENC_HEXTILE;
        break;
    }
    encs[n++] = ENC_COPYRECT;
    encs[n++] = ENC_RAW;
    encs[n++] = ENC_DESKTOPSIZE;
    encs[n++] = ENC_EXTDESKTOP;
    encs[n++] = ENC_LASTRECT;
    encs[n++] = ENC_QUALITY0 + s->srv.quality;
    encs[n++] = ENC_COMPRESS0 + 6;
    uint8_t m[4 + 12 * 4] = { 2, 0 };
    put16(m + 2, (uint32_t)n);
    for (int i = 0; i < n; i++) put32(m + 4 + i * 4, (uint32_t)encs[i]);
    send_all(s, m, 4 + n * 4);
    send_update_request(s, false);
    set_state(s, VNC_ST_LIVE, NULL);
}

/* ---- the worker ------------------------------------------------------------------- */

static int connect_to(vnc_sess_t *s)
{
    uint64_t t0 = aos_hal_uptime_ms();
    for (;;) {
        int r = aos_hal_tcp_connect(s->srv.host, s->srv.port, 1000);
        if (r > 0 || r == AOS_TCP_ERR_DNS || r == AOS_TCP_ERR_ARG || r == AOS_TCP_ERR_SLOTS) return r;
        /* refused comes back at once: nothing listening, no point retrying */
        if (aos_hal_uptime_ms() - t0 < 900 || stopping(s) || aos_hal_uptime_ms() - t0 >= CONNECT_MS) return r;
    }
}

static void sess_free(vnc_sess_t *s)
{
    vnc_fb_free(&s->fb);
    vnc_render_free(s->ren);
    free(s->vbuf[0]);
    free(s->vbuf[1]);
    free(s->rd);
    free(s->stage);
    free(s->tile);
    free(s->grad);
    free(s->jbuf);
    zs_free(&s->zrle);
    for (int i = 0; i < 4; i++) zs_free(&s->tz[i]);
    free(s);
    /* the mutex stays: the HAL has no call to free one (a few bytes) */
}

static void worker(void *arg)
{
    vnc_sess_t *s = arg;
    s->sock = -1;
    if (setjmp(s->jb) == 0) {
        s->rd = vnc_psram(RD_CAP);
        s->stage = vnc_psram(STAGE_CAP);
        s->tile = vnc_psram(64 * 64 * 2 > 8192 * 2 ? 64 * 64 * 2 : 8192 * 2);
        s->grad = vnc_psram(2 * 3 * 2048 * sizeof(int16_t));
        s->ren = vnc_render_new();
        if (!s->rd || !s->stage || !s->tile || !s->grad || !s->ren) fail(s, _("Sin memoria"));

        int r = connect_to(s);
        if (r <= 0) {
            fail(s, r == AOS_TCP_ERR_DNS     ? _("No se encontró la computadora (DNS)")
                  : r == AOS_TCP_ERR_SLOTS   ? _("Sin conexiones libres en la placa")
                  : r == AOS_TCP_ERR_TIMEOUT ? _("La computadora no contesta")
                                             : _("No se pudo conectar (¿VNC activado? ¿puerto?)"));
        }
        s->sock = r;
        s->last_in = aos_hal_uptime_ms();
        handshake(s);
        s->stats_t0 = aos_hal_uptime_ms();
        for (;;) {
            if (stopping(s)) fail(s, NULL);
            pump(s);
            present(s);
            stats(s);
            if (s->rd_len == s->rd_pos) {
                /* between messages the server may stay quiet for as long as
                 * the screen does not change */
                s->rd_pos = s->rd_len = 0;
                int n = aos_hal_tcp_recv(s->sock, s->rd, RD_CAP, WAIT_SLICE_MS);
                if (n < 0) fail(s, _("El servidor cerró la conexión"));
                if (n == 0) continue;
                s->rd_len = n;
                s->n_bytes += (uint32_t)n;
                s->last_in = aos_hal_uptime_ms();
            }
            handle_message(s);
        }
    }
    /* here from fail(): an error, or the app asked to stop */
    if (s->sock > 0) aos_hal_tcp_close(s->sock);
    s->sock = -1;
    if (s->jpeg) {
        aos_hal_jpeg_close(s->jpeg);
        s->jpeg = NULL;
    }
    if (!__sync_bool_compare_and_swap(&s->owner, 0, 1)) sess_free(s);
}

/* ---- the UI's side ------------------------------------------------------------------ */

vnc_sess_t *vnc_sess_start(const vnc_server_t *srv)
{
    vnc_sess_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->mx = aos_hal_mutex_create();
    if (!s->mx) {
        free(s);
        return NULL;
    }
    s->srv = *srv;
    s->zone = srv->zone;
    s->front = -1;
    s->ui_front = -1;
    s->state = VNC_ST_CONNECTING;
    if (!aos_hal_worker_start("vnc", worker, s, STACK_BYTES)) {
        free(s);
        return NULL;
    }
    return s;
}

void vnc_sess_stop(vnc_sess_t *s)
{
    if (!s) return;
    s->stop = true;
    __sync_synchronize();
    aos_hal_worker_stop();
    if (!__sync_bool_compare_and_swap(&s->owner, 0, 2)) sess_free(s);
}

vnc_state_t vnc_sess_state(vnc_sess_t *s, char *detail, size_t n)
{
    aos_hal_mutex_lock(s->mx);
    vnc_state_t st = (vnc_state_t)s->state;
    if (detail) snprintf(detail, n, "%s", s->detail);
    aos_hal_mutex_unlock(s->mx);
    return st;
}

bool vnc_sess_desktop(vnc_sess_t *s, int *w, int *h, char *name, size_t n)
{
    aos_hal_mutex_lock(s->mx);
    bool ok = s->have_desktop;
    if (w) *w = s->rw;
    if (h) *h = s->rh;
    if (name) snprintf(name, n, "%s", s->name);
    aos_hal_mutex_unlock(s->mx);
    return ok;
}

int vnc_sess_screens(vnc_sess_t *s, int *full_w, int *full_h, vnc_rect_t *zone, vnc_rect_t *scr, int max)
{
    aos_hal_mutex_lock(s->mx);
    if (full_w) *full_w = s->full_w;
    if (full_h) *full_h = s->full_h;
    if (zone) *zone = s->cur;
    int n = s->nscr < max ? s->nscr : max;
    if (scr && n > 0) memcpy(scr, s->scr, (size_t)n * sizeof *scr);
    aos_hal_mutex_unlock(s->mx);
    return n;
}

void vnc_sess_set_zone(vnc_sess_t *s, const vnc_rect_t *z)
{
    if (!s) return;
    aos_hal_mutex_lock(s->mx);
    s->zone = *z;
    s->want_zone = true;
    aos_hal_mutex_unlock(s->mx);
}

uint32_t vnc_sess_zone_gen(vnc_sess_t *s)
{
    aos_hal_mutex_lock(s->mx);
    uint32_t g = s->zone_gen;
    aos_hal_mutex_unlock(s->mx);
    return g;
}

void vnc_sess_stats(vnc_sess_t *s, char *out, size_t n)
{
    aos_hal_mutex_lock(s->mx);
    snprintf(out, n, "%s", s->stats);
    aos_hal_mutex_unlock(s->mx);
}

uint32_t vnc_sess_bells(vnc_sess_t *s)
{
    return s->bells;
}

int vnc_sess_shift(vnc_sess_t *s)
{
    return s->shift;
}

void vnc_sess_set_view(vnc_sess_t *s, const vnc_view_t *v)
{
    aos_hal_mutex_lock(s->mx);
    s->want = *v;
    aos_hal_mutex_unlock(s->mx);
}

bool vnc_sess_take(vnc_sess_t *s, const uint16_t **px, int *w, int *h, bool *changed, lv_area_t *area)
{
    bool got = false;
    aos_hal_mutex_lock(s->mx);
    if (s->front >= 0) {
        int f = s->front;
        *px = s->vbuf[f];
        *w = s->bw[f];
        *h = s->bh[f];
        if (s->new_front || s->ui_front != f) {
            s->ui_front = f;
            s->new_front = false;
            s->has_dirty = false;
            *changed = true;
            got = true;
        } else if (s->has_dirty) {
            *area = s->dirty;
            s->has_dirty = false;
            *changed = false;
            got = true;
        }
    }
    aos_hal_mutex_unlock(s->mx);
    return got;
}

static void queue(vnc_sess_t *s, const ev_t *e)
{
    aos_hal_mutex_lock(s->mx);
    /* a move with the same buttons replaces the last one still waiting */
    if (e->type == EV_POINTER && s->qn && s->q[s->qn - 1].type == EV_POINTER &&
        s->q[s->qn - 1].buttons == e->buttons) {
        s->q[s->qn - 1] = *e;
    } else if (s->qn < EVQ) {
        s->q[s->qn++] = *e;
    }
    aos_hal_mutex_unlock(s->mx);
}

void vnc_sess_pointer(vnc_sess_t *s, int x, int y, uint8_t buttons)
{
    if (!s) return;
    ev_t e = { .type = EV_POINTER, .buttons = buttons };
    e.x = (uint16_t)(x < 0 ? 0 : x > 65535 ? 65535 : x);
    e.y = (uint16_t)(y < 0 ? 0 : y > 65535 ? 65535 : y);
    queue(s, &e);
}

void vnc_sess_key(vnc_sess_t *s, uint32_t keysym, bool down)
{
    if (!s) return;
    ev_t e = { .type = EV_KEY, .key = keysym, .down = down };
    queue(s, &e);
}

void vnc_sess_pause(vnc_sess_t *s, bool paused)
{
    if (s) s->paused = paused;
}

void vnc_sess_refresh(vnc_sess_t *s)
{
    if (!s) return;
    aos_hal_mutex_lock(s->mx);
    s->want_refresh = true;
    aos_hal_mutex_unlock(s->mx);
}
