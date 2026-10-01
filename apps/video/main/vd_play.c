/*
 * P4OS (from AmoledOS) - Video: the three stages and the clock. video.h has
 * the shape of it.
 */
#include "video.h"

#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VD_WORKER_STACK  8192
#define VD_READER_STACK  6144
#define VD_READER_PRIO   3          /* the apps' threads' level (docs/MEMORY.md) */
#define VD_SYNC_MS       100        /* how often the clock asks the player */
#define VD_DRIFT_MS      45         /* past this the clock jumps to the sound */
#define VD_AUDIO_WAIT_MS 2000       /* the sound gets this long to start moving */
#define VD_STATS_MS      500
#define VD_LOG_EVERY     4          /* stats lines per log line: one every 2 s */

/* LVGL's, and in the firmware's table, but lvgl.h does not declare it (the
 * cache headers are private): the LVGL path reuses one descriptor per slot
 * for new pixels, and a cached copy of the old ones would be drawn. */
extern void lv_image_cache_drop(const void *src);

/* ---- time ------------------------------------------------------------------- */

uint32_t vd_frame_ms(const vd_t *v, uint32_t frame)
{
    return v->rate ? (uint32_t)((uint64_t)frame * v->scale * 1000u / v->rate) : 0;
}

uint32_t vd_frame_at(const vd_t *v, uint64_t ms)
{
    if (!v->scale || !v->frames) {
        return 0;
    }
    uint64_t f = ms * v->rate / ((uint64_t)v->scale * 1000u);
    return f >= v->frames ? v->frames - 1 : (uint32_t)f;
}

uint32_t vd_duration_ms(const vd_t *v)
{
    return vd_frame_ms(v, v->frames);
}

/* ---- the reader ------------------------------------------------------------- */

/* Reads frames off the card into the packet ring, in order, from wherever
 * the UI last asked. It never decodes and never touches LVGL. */
static void reader(void *arg)
{
    vd_t    *v    = arg;
    uint32_t gen  = 0;
    uint32_t next = 0;
    int      put  = 0;
    bool     first = true;
    while (!v->r_stop) {
        uint32_t g = v->req_gen;
        if (first || g != gen) {
            first = false;
            gen   = g;
            __sync_synchronize();
            next  = v->req_frame;
        }
        /* Behind? The frames in between are never read. */
        if (v->skip_gen == gen && v->skip_to > next) {
            v->r_skipped += v->skip_to - next;
            next = v->skip_to;
        }
        if (next >= v->frames) {
            v->eof_gen = gen;
            aos_hal_sleep_ms(5);
            continue;
        }
        vd_pkt_t *p = &v->pkts[put];
        if (p->state != PKT_FREE) {
            aos_hal_sleep_ms(2);
            v->r_wait_ms += 2;
            continue;
        }
        uint64_t t0 = aos_hal_uptime_ms();
        int n = vd_avi_read(&v->avi, next, p->buf, &p->jpeg);
        if (n <= 0) {
            /* 0: an empty chunk, the previous frame stays up; -1: a bad one */
            if (n < 0) {
                v->r_errors++;
            }
            next++;
            continue;
        }
        p->len     = n;
        p->index   = (int32_t)next;
        p->gen     = gen;
        p->read_ms = (uint32_t)(aos_hal_uptime_ms() - t0);
        __sync_synchronize();
        p->state = PKT_FULL;
        put = (put + 1) % VD_PKTS;
        next++;
    }
    __sync_synchronize();
    v->r_alive = false;
}

/* ---- the worker ------------------------------------------------------------- */

/* The decoder's pixels go to the screen as they are: the blit takes the
 * engine's padded stride and scales to any size (aos_hal_display_blit_fit),
 * so nothing is halved or repacked here any more. The board measured what
 * that halving cost (2026-09-29): 1280x720 upright was d52 ms and 18 fps. */
static void finish(vd_slot_t *s, const uint16_t *px, int w, int h, int stride)
{
    s->px     = px;
    s->w      = w;
    s->h      = h;
    s->stride = stride;
}

static vd_slot_t *free_slot(vd_t *v)
{
    for (int i = 0; i < VD_SLOTS; i++) {
        if (v->slots[i].state == SLOT_FREE) {
            return &v->slots[i];
        }
    }
    return NULL;
}

/* The reader goes first (it is in the middle of one read at most), then
 * the memory it wrote into. */
static void stages_close(vd_t *v)
{
    v->r_stop = true;
    for (int i = 0; i < 1000 && v->r_alive; i++) {
        aos_hal_worker_sleep(2);
    }
    if (v->r_alive) {
        /* Freeing under it would be worse than keeping it all. */
        aos_hal_log("video", "the reader did not stop: its buffers are kept");
        return;
    }
    for (int i = 0; i < VD_PKTS; i++) {
        aos_hal_io_free(v->pkts[i].buf);
        v->pkts[i].buf   = NULL;
        v->pkts[i].state = PKT_FREE;
    }
    vd_avi_close(&v->avi);
}

static bool stages_open(vd_t *v)
{
    if (!vd_avi_open(&v->avi, v->path, true)) {
        v->open_state = OPEN_BAD;
        return false;
    }
    size_t sz = vd_avi_buf_size(&v->avi);
    for (int i = 0; i < VD_PKTS; i++) {
        v->pkts[i].state = PKT_FREE;
        v->pkts[i].buf   = aos_hal_io_alloc(sz);
        if (!v->pkts[i].buf) {
            v->open_state = OPEN_NOMEM;
            return false;
        }
    }
    v->rate   = v->avi.rate;
    v->scale  = v->avi.scale;
    v->frames = v->avi.frames;
    v->vw     = v->avi.width;
    v->vh     = v->avi.height;
    v->r_stop  = false;
    v->r_alive = true;
    if (!aos_hal_thread_start("vd_read", reader, v, VD_READER_STACK, VD_READER_PRIO)) {
        v->r_alive    = false;
        v->open_state = OPEN_NOTASK;
        return false;
    }
    __sync_synchronize();
    v->open_state = OPEN_OK;
    return true;
}

/* Decodes the packets in order into free slots, until told to stop. Never
 * touches LVGL. Whatever belongs to an older gen, or is already late, is
 * dropped without decoding. */
static void worker(void *arg)
{
    vd_t *v    = arg;
    int   take = 0;
    if (stages_open(v)) {
        while (!aos_hal_worker_should_stop()) {
            vd_pkt_t *p = &v->pkts[take];
            if (p->state != PKT_FULL) {
                aos_hal_worker_sleep(2);
                v->w_starve_ms += 2;
                continue;
            }
            __sync_synchronize();
            uint32_t gen  = v->req_gen;
            bool     late = p->gen == gen && v->skip_gen == gen && (uint32_t)p->index < v->skip_to;
            if (p->gen != gen || late) {
                if (late) {
                    v->w_skipped++;
                }
                p->state = PKT_FREE;
                take = (take + 1) % VD_PKTS;
                continue;
            }
            vd_slot_t *s = free_slot(v);
            if (!s) {
                aos_hal_worker_sleep(2);
                v->w_wait_ms += 2;
                continue;
            }
            s->state = SLOT_BUSY;
            uint64_t t0 = aos_hal_uptime_ms();
            if (!s->jpeg) {
                s->jpeg = aos_hal_jpeg_open();
            }
            int w = 0, h = 0, stride = 0;
            const uint16_t *px = s->jpeg ? aos_hal_jpeg_decode(s->jpeg, p->jpeg, (size_t)p->len,
                                                               &w, &h, &stride) : NULL;
            int32_t  index   = p->index;
            uint32_t read_ms = p->read_ms;
            /* The decoder has taken what it needed from the packet. */
            __sync_synchronize();
            p->state = PKT_FREE;
            take = (take + 1) % VD_PKTS;
            if (!px) {
                v->w_errors++;
                s->state = SLOT_FREE;
                continue;
            }
            finish(s, px, w, h, stride);
            s->index     = index;
            s->gen       = gen;
            s->read_ms   = read_ms;
            s->decode_ms = (uint32_t)(aos_hal_uptime_ms() - t0);
            __sync_synchronize();
            s->state = SLOT_READY;
        }
    }
    stages_close(v);
}

bool vd_play_start(vd_t *v)
{
    for (int i = 0; i < VD_SLOTS; i++) {
        v->slots[i].state = SLOT_FREE;
    }
    v->cur_slot  = -1;
    v->shown     = -1;
    v->eof_gen   = UINT32_MAX;
    v->r_skipped = v->w_skipped = v->r_errors = v->w_errors = 0;
    v->r_wait_ms = v->w_starve_ms = v->w_wait_ms = 0;
    v->dst_w     = v->dst_h = 0;
    return aos_hal_worker_start("aos_video", worker, v, VD_WORKER_STACK);
}

void vd_play_stop(vd_t *v)
{
    aos_hal_worker_stop();              /* the worker takes the reader and the file with it */
    if (v->has_audio) {
        aos_hal_player_stop();
    }
    /* frame_img may point at a slot's pixels: it lets go first */
    if (v->frame_img) {
        lv_image_set_src(v->frame_img, NULL);
        lv_obj_add_flag(v->frame_img, LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < VD_SLOTS; i++) {
        vd_slot_t *s = &v->slots[i];
        if (s->jpeg) {
            aos_hal_jpeg_close(s->jpeg);
        }
        memset(s, 0, sizeof *s);
    }
    v->cur_slot = -1;
    v->playing  = false;
    v->paused   = false;
}

/* ---- the clock -------------------------------------------------------------- */

/* The sound, where the UI wants it: 'ms' in, playing or not. The player
 * stops by itself at the end of the file, so it may have to start again. */
static void audio_to(vd_t *v, uint32_t ms, bool play)
{
    if (!v->has_audio) {
        return;
    }
    aos_player_info_t in;
    bool known = aos_hal_player_info(&in);
    if (!known || in.state == AOS_PLAYER_STOPPED || strcmp(in.path, v->audio_path) != 0) {
        if (!aos_hal_player_play(v->audio_path)) {
            v->has_audio = false;
            return;
        }
        if (ms > 0) {
            aos_hal_player_seek(ms);    /* the decoder opens the file, then seeks */
        }
        if (!play) {
            aos_hal_player_pause();
        }
        in.position_ms = 0;
    } else {
        aos_hal_player_seek(ms);
        if (play && in.state == AOS_PLAYER_PAUSED) {
            aos_hal_player_resume();
        } else if (!play && in.state == AOS_PLAYER_PLAYING) {
            aos_hal_player_pause();
        }
    }
    if (play) {
        v->audio_wait    = true;
        v->audio_target  = ms;
        v->audio_p0      = in.position_ms;
        v->audio_wait_t0 = aos_hal_uptime_ms();
    }
}

/* Milliseconds into the video. */
static uint32_t clock_ms(vd_t *v, uint64_t now)
{
    if (v->paused) {
        return v->t_hold;
    }
    aos_player_info_t in;
    if (v->audio_wait) {
        /* Held where it is until the sound really moves from there: the
         * player takes a moment to open, seek or resume. */
        bool moved = aos_hal_player_info(&in) && in.state == AOS_PLAYER_PLAYING &&
                     in.position_ms != v->audio_p0 &&
                     in.position_ms + 2000 > v->audio_target && in.position_ms < v->audio_target + 2000;
        if (moved) {
            v->audio_wait = false;
            v->t_anchor   = now - in.position_ms;
            v->sync_last  = now;
        } else if (now - v->audio_wait_t0 > VD_AUDIO_WAIT_MS) {
            aos_hal_log("video", "the sound did not start: the picture goes on by itself");
            v->audio_wait = false;
            v->t_anchor   = now - v->t_hold;
        } else {
            return v->t_hold;
        }
    }
    int64_t t = (int64_t)(now - v->t_anchor);
    if (v->has_audio && now - v->sync_last >= VD_SYNC_MS) {
        v->sync_last = now;
        if (aos_hal_player_info(&in) && in.state == AOS_PLAYER_PLAYING &&
            strcmp(in.path, v->audio_path) == 0) {
            int64_t drift = (int64_t)in.position_ms - t;
            if (drift > VD_DRIFT_MS || drift < -VD_DRIFT_MS) {
                v->t_anchor = now - in.position_ms;
                t = in.position_ms;
            }
        }
        /* STOPPED: the sound ran out a little before the frames do (or
         * someone else took the player); the frame clock finishes alone. */
    }
    return t < 0 ? 0 : (uint32_t)t;
}

uint32_t vd_play_now_ms(vd_t *v)
{
    if (v->paused || v->audio_wait) {
        return v->t_hold;
    }
    int64_t t = (int64_t)(aos_hal_uptime_ms() - v->t_anchor);
    return t < 0 ? 0 : (uint32_t)t;
}

void vd_play_seek(vd_t *v, uint32_t frame, bool audio)
{
    if (!v->frames) {
        return;
    }
    if (frame >= v->frames) {
        frame = v->frames - 1;
    }
    v->gen++;
    v->req_frame = frame;
    __sync_synchronize();
    v->req_gen   = v->gen;
    v->t_hold    = vd_frame_ms(v, frame);
    v->t_anchor  = aos_hal_uptime_ms() - v->t_hold;
    v->ended     = false;
    v->show_next = true;            /* even paused, the frame sought goes up */
    v->audio_wait = false;
    if (audio) {
        audio_to(v, v->t_hold, !v->paused);
    }
}

void vd_play_pause(vd_t *v, bool paused)
{
    if (paused == v->paused && !v->ended) {
        return;
    }
    uint64_t now = aos_hal_uptime_ms();
    if (paused) {
        v->t_hold = clock_ms(v, now);
        v->paused = true;
        v->audio_wait = false;
        if (v->has_audio) {
            aos_hal_player_pause();
        }
        return;
    }
    if (v->ended) {
        v->paused = true;           /* so the seek below does not start the sound twice */
        vd_play_seek(v, 0, false);
    }
    v->paused   = false;
    v->t_anchor = now - v->t_hold;
    audio_to(v, v->t_hold, true);
}

/* ---- to the screen ---------------------------------------------------------- */

/* Where a w x h frame goes: the biggest scale that fits, in the PPA's
 * steps of 1/16 (what aos_hal_display_blit_fit rounds to, so the LVGL path
 * and the board agree to the pixel), at most VD_MAX_SCALE, centred. */
typedef struct {
    int x, y, w, h;         /* on screen */
    int s16;                /* the scale, in 16ths */
} vd_geo_t;

static bool fit(const vd_t *v, int w, int h, vd_geo_t *g)
{
    if (w <= 0 || h <= 0) {
        return false;
    }
    int sx = v->scr_w * 16 / w, sy = v->scr_h * 16 / h;
    int s  = sx < sy ? sx : sy;
    if (s > VD_MAX_SCALE * 16) {
        s = VD_MAX_SCALE * 16;
    }
    if (s < 1) {
        return false;
    }
    g->s16 = s;
    g->w   = w * s / 16;
    g->h   = h * s / 16;
    g->x   = (v->scr_w - g->w) / 2;
    g->y   = (v->scr_h - g->h) / 2;
    return true;
}

/* Source rows [r0, r1) of the frame, to where the whole frame puts them. */
static bool blit_rows(vd_t *v, const vd_slot_t *s, const vd_geo_t *g, int r0, int r1)
{
    int n = r1 - r0;
    if (g->s16 % 16 == 0 && s->stride == s->w) {
        /* A whole number and packed rows: the plain integer blit (1:1
         * unrotated goes by DMA2D there). */
        int k = g->s16 / 16;
        return aos_hal_display_blit_scaled(g->x, g->y + r0 * k, s->w, n,
                                           s->px + (size_t)r0 * s->stride, k, false);
    }
    /* The HAL rounds the factor down from dst/src, so a height rounded UP
     * here comes back to exactly this scale (for bands of 16 rows or more). */
    int y  = g->y + r0 * g->s16 / 16;
    int dh = (n * g->s16 + 15) / 16;
    if (y + dh > v->scr_h) {
        dh = v->scr_h - y;
    }
    return aos_hal_display_blit_fit(g->x, y, g->w, dh, s->px + (size_t)r0 * s->stride,
                                    s->w, n, s->stride, false);
}

/* Nothing of LVGL's may show over the picture: the bars down, no message,
 * no toast. Then a flip shows no less than the buffer on screen does. */
static bool nothing_over(const vd_t *v)
{
    return !v->controls && !(v->over & AOS_UI_OVER_TOAST) &&
           (!v->message || lv_obj_has_flag(v->message, LV_OBJ_FLAG_HIDDEN));
}

static void clear_panel(uint16_t *fb, int rot, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    /* the screen's box as the panel's (aos_hal_display_blit_native's mapping) */
    int px = x, py = y, pw = w, ph = h;
    if (rot == 90) {
        px = AOS_PANEL_W - (y + h); py = x; pw = h; ph = w;
    } else if (rot == 180) {
        px = AOS_PANEL_W - (x + w); py = AOS_PANEL_H - (y + h);
    } else if (rot == 270) {
        px = y; py = AOS_PANEL_H - (x + w); pw = h; ph = w;
    }
    for (int r = 0; r < ph; r++) {
        memset(fb + (size_t)(py + r) * AOS_PANEL_W + px, 0, (size_t)pw * 2);   /* AOS_C_BG, black */
    }
}

/* A 1:1 upright frame the screen's width into the panel's buffer 'fb':
 * whole rows. The HAL copies rows by the AXI DMA (19 ms for 720x1280 on the
 * board, against ~48 by the PPA) when they start and end on a 128-byte
 * cache line, which a run of rows from a multiple of 8 to a multiple of 8
 * does (8 rows of 720 px are 90 lines); the few rows before and after that
 * run, when the picture's place is not so aligned, go by the CPU, and the
 * flip writes the cache back. */
static bool rows_into(uint16_t *fb, const vd_slot_t *s, const vd_geo_t *g)
{
    int y = g->y, h = s->h;
    int a = (y + 7) & ~7, b = (y + h) & ~7;
    if (b <= a) {
        a = b = y + h;                      /* too short for the DMA: all by the CPU */
    }
    for (int r = y; r < a; r++) {
        memcpy(fb + (size_t)r * AOS_PANEL_W, s->px + (size_t)(r - y) * s->stride, (size_t)s->w * 2);
    }
    for (int r = b; r < y + h; r++) {
        memcpy(fb + (size_t)r * AOS_PANEL_W, s->px + (size_t)(r - y) * s->stride, (size_t)s->w * 2);
    }
    return b <= a || aos_hal_display_blit_into_fit(fb, 0, a, s->w, b - a, s->px + (size_t)(a - y) * s->stride,
                                                   s->w, b - a, s->stride, false);
}

/* The whole frame into the panel's free buffer, which is then flipped to.
 * false: not here (the simulator, pref "fbs" = 1), or not worth it, and the
 * caller blits into the buffer on screen. */
static bool flip_frame(vd_t *v, const vd_slot_t *s, const vd_geo_t *g)
{
    int rot = aos_hal_display_get_rotation();
    bool rows = g->s16 == 16 && rot == 0 && s->stride == s->w;
    if (rows && (s->w != AOS_PANEL_W || v->scr_w != AOS_PANEL_W)) {
        /* 1:1 upright but narrower than the screen: the PPA would do it
         * (~48 ms a screen), the DMA2D into the buffer on screen is faster */
        return false;
    }
    uint16_t *fb = aos_hal_display_back();
    uint32_t  age = 0;
    bool      touched = false;
    if (!fb || !aos_hal_display_back_age(fb, &age, &touched)) {
        return false;
    }
    int k = 0;
    while (k < 3 && v->fbr[k].fb && v->fbr[k].fb != fb) {
        k++;
    }
    if (k == 3) {
        return false;
    }
    /* The size asked rounded up, so the HAL's factor (rounded down from
     * dst/src) comes back to this one; what it really covers, the same
     * way the HAL works it out. */
    int dw = (s->w * g->s16 + 15) / 16, dh = (s->h * g->s16 + 15) / 16;
    if (g->x + dw > v->scr_w) {
        dw = v->scr_w - g->x;
    }
    if (g->y + dh > v->scr_h) {
        dh = v->scr_h - g->y;
    }
    int ow = s->w * (dw * 16 / s->w) / 16, oh = s->h * (dh * 16 / s->h) / 16;
    if (rows ? !rows_into(fb, s, g)
             : !aos_hal_display_blit_into_fit(fb, g->x, g->y, dw, dh, s->px, s->w, s->h, s->stride, false)) {
        return false;
    }
    /* The letterbox after the PPA (or the DMA): before it runs it drops
     * the cache over the rows it writes, and the side bars share those
     * rows. */
    if (v->fbr[k].fb != fb || age == UINT32_MAX || touched || v->fbr[k].x != g->x || v->fbr[k].y != g->y ||
        v->fbr[k].w != ow || v->fbr[k].h != oh) {
        clear_panel(fb, rot, 0, 0, v->scr_w, g->y);
        clear_panel(fb, rot, 0, g->y + oh, v->scr_w, v->scr_h - g->y - oh);
        clear_panel(fb, rot, 0, g->y, g->x, oh);
        clear_panel(fb, rot, g->x + ow, g->y, v->scr_w - g->x - ow, oh);
        v->fbr[k].fb = fb;
        v->fbr[k].x  = g->x;
        v->fbr[k].y  = g->y;
        v->fbr[k].w  = ow;
        v->fbr[k].h  = oh;
        v->st_clears++;
    }
    if (!aos_hal_display_flip(fb)) {
        v->fbr[k].fb = NULL;
        return false;
    }
    return true;
}

/* 'flip': a new frame, which may go by page flipping. Putting the frame on
 * screen back where LVGL just drew (vd_play_blit_current) goes into the
 * buffer on screen: that is the one LVGL drew into, and mending it now is
 * quicker than the next refresh a flip waits for. */
static bool draw(vd_t *v, vd_slot_t *s, bool flip)
{
    vd_geo_t g;
    if (!fit(v, s->w, s->h, &g) || v->overlay) {
        return false;
    }
    v->dst_w = g.w;
    v->dst_h = g.h;
    if (!v->lvgl_mode && flip && nothing_over(v) && flip_frame(v, s, &g)) {
        v->blit_fails = 0;
        v->st_flips++;
        return true;
    }
    if (!v->lvgl_mode) {
        /* With the controls up, only the rows between the bars: LVGL does
         * not know about the picture, and whatever the blit covers of its
         * bars would stay covered until LVGL redraws them. */
        int r0 = 0, r1 = s->h;
        if (v->controls) {
            int top = v->top_h - g.y, bottom = v->scr_h - v->bottom_h - g.y;
            if (top > 0) {
                r0 = (top * 16 + g.s16 - 1) / g.s16;
            }
            if (bottom < g.h) {
                int limit = v->scr_h - v->bottom_h;
                r1 = bottom * 16 / g.s16;
                /* blit_rows() rounds the band's height up: keep it off the bar */
                while (r1 > r0 && g.y + r0 * g.s16 / 16 + ((r1 - r0) * g.s16 + 15) / 16 > limit) {
                    r1--;
                }
            }
        }
        if (r1 <= r0) {
            return true;                /* all of it under the bars */
        }
        if (blit_rows(v, s, &g, r0, r1)) {
            v->blit_fails = 0;
            v->st_direct++;
            return true;
        }
        if (++v->blit_fails < 3) {
            return false;
        }
        v->lvgl_mode = true;
        aos_hal_log("video", "no direct blit here: the frames go through LVGL");
    }
    /* The simulator: an RGB565 image of the slot's pixels, stretched to the
     * same place the blit would have put it. */
    lv_image_dsc_t *d = &s->dsc;
    memset(d, 0, sizeof *d);
    d->header.magic  = LV_IMAGE_HEADER_MAGIC;
    d->header.cf     = LV_COLOR_FORMAT_RGB565;
    d->header.w      = (uint32_t)s->w;
    d->header.h      = (uint32_t)s->h;
    d->header.stride = (uint32_t)s->stride * 2;
    d->data_size     = (uint32_t)(s->stride * s->h * 2);
    d->data          = (const uint8_t *)s->px;
    lv_image_cache_drop(d);
    lv_image_set_src(v->frame_img, d);
    lv_obj_set_pos(v->frame_img, g.x, g.y);
    lv_obj_set_size(v->frame_img, g.w, g.h);
    lv_image_set_inner_align(v->frame_img, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_remove_flag(v->frame_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(v->frame_img);
    return true;
}

bool vd_play_blit_current(vd_t *v)
{
    if (v->cur_slot < 0 || v->lvgl_mode) {
        return false;
    }
    return draw(v, &v->slots[v->cur_slot], false);
}

static void present(vd_t *v, int idx)
{
    vd_slot_t *s  = &v->slots[idx];
    uint64_t   t0 = aos_hal_uptime_us();
    draw(v, s, true);
    v->st_blit_us += (uint32_t)(aos_hal_uptime_us() - t0);

    /* The one on screen stays SHOWN until the next takes its place: the
     * LVGL path reads it, and a redraw may need to blit it again. */
    if (v->cur_slot >= 0 && v->cur_slot != idx) {
        v->slots[v->cur_slot].state = SLOT_FREE;
    }
    s->state    = SLOT_SHOWN;
    v->cur_slot = idx;
    v->shown    = s->index;
    v->st_frames++;
    v->st_read_ms += s->read_ms;
    v->st_dec_ms  += s->decode_ms;
}

static void stats(vd_t *v, uint64_t now)
{
    uint64_t dt = now - v->stats_t0;
    if (dt < VD_STATS_MS) {
        return;
    }
    uint32_t n      = v->st_frames;
    uint32_t fps10  = (uint32_t)(n * 10000ull / dt);
    uint32_t blit10 = n ? v->st_blit_us / n / 100 : 0;
    uint32_t starve = v->w_starve_ms - v->st_starve0, wait = v->w_wait_ms - v->st_wait0;
    char buf[128];
    snprintf(buf, sizeof buf, "%u.%u fps  r%u d%u b%u.%u l%u ms  -%u  %ux%u>%dx%d",
             (unsigned)(fps10 / 10), (unsigned)(fps10 % 10),
             (unsigned)(n ? v->st_read_ms / n : 0), (unsigned)(n ? v->st_dec_ms / n : 0),
             (unsigned)(blit10 / 10), (unsigned)(blit10 % 10),
             (unsigned)(n ? v->st_late_ms / n : 0),
             (unsigned)(v->r_skipped + v->w_skipped), (unsigned)v->vw, (unsigned)v->vh, v->dst_w, v->dst_h);
    if (n) {
        vd_ui_stats(v, buf);        /* paused: the last numbers stay */
    }
    /* The same line to the log every 2 s: on the board /api/log is where
     * the numbers get read from. */
    if (n && ++v->log_every % VD_LOG_EVERY == 0) {
        aos_hal_log("video", "%s | flips %u, direct %u, cleared %u | starved %u, waited %u ms in %u, "
                    "errors r%u w%u%s", buf, (unsigned)v->st_flips, (unsigned)v->st_direct,
                    (unsigned)v->st_clears, (unsigned)starve, (unsigned)wait, (unsigned)dt,
                    (unsigned)v->r_errors, (unsigned)v->w_errors, v->lvgl_mode ? ", via LVGL" : "");
    }
    v->st_flips = v->st_direct = v->st_clears = 0;
    v->stats_t0   = now;
    v->st_frames  = v->st_read_ms = v->st_dec_ms = v->st_blit_us = v->st_late_ms = 0;
    v->st_starve0 = v->w_starve_ms;
    v->st_wait0   = v->w_wait_ms;
}

void vd_play_tick(vd_t *v)
{
    /* What of the system's is over the player: under anything but a toast
     * nothing is blitted, and the playback pauses; when it goes, LVGL has
     * drawn the app again, the frame goes back up over it, and what the
     * overlay paused plays on. Read here, in LVGL's thread. */
    v->over = aos_ui_overlay();
    if (v->hidden) {
        v->over |= AOS_UI_OVER_NOT_FRONT;
    }
    bool ov  = (v->over & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    bool can = v->playing && v->open_state == OPEN_OK;
    if (ov && !v->overlay && can && !v->paused) {
        vd_play_pause(v, true);
        v->ov_paused = true;
        aos_hal_log("video", "paused: 0x%x over the player", (unsigned)v->over);
    }
    if (!ov && v->overlay) {
        v->reblit = true;
        if (v->ov_paused && can && v->paused && !v->ended && !v->scrub) {
            vd_play_pause(v, false);
            aos_hal_log("video", "playing again");
        }
        v->ov_paused = false;
    }
    v->overlay = ov;

    if (!can) {
        return;
    }
    uint64_t now = aos_hal_uptime_ms();

    /* Drop what an older seek left, then take the oldest of this one. */
    int idx = -1;
    for (int i = 0; i < VD_SLOTS; i++) {
        vd_slot_t *c = &v->slots[i];
        if (c->state != SLOT_READY) {
            continue;
        }
        if (c->gen != v->gen) {
            c->state = SLOT_FREE;
            continue;
        }
        if (idx < 0 || c->index < v->slots[idx].index) {
            idx = i;
        }
    }
    __sync_synchronize();

    if (v->paused) {
        if (idx >= 0 && v->show_next) {
            v->show_next = false;
            present(v, idx);
        }
    } else {
        uint32_t t = clock_ms(v, now);
        if (idx >= 0) {
            vd_slot_t *s   = &v->slots[idx];
            uint32_t   due = vd_frame_at(v, t);
            if ((uint32_t)s->index <= due) {
                v->show_next = false;
                present(v, idx);
                uint32_t at = vd_frame_ms(v, (uint32_t)s->index);
                v->st_late_ms += t > at ? t - at : 0;
                if (due > (uint32_t)s->index + 1) {
                    /* Behind: whatever is read next, let it be the frame
                     * that is due by the time it is decoded. */
                    v->skip_to  = due + 1;
                    __sync_synchronize();
                    v->skip_gen = v->gen;
                }
            }
        }
        /* The end: everything read, nothing left to show, time is up. */
        bool left = false;
        for (int i = 0; i < VD_SLOTS; i++) {
            left |= v->slots[i].state == SLOT_READY && v->slots[i].gen == v->gen;
        }
        if (v->eof_gen == v->gen && !left && t + 40 >= vd_duration_ms(v)) {
            v->ended  = true;
            v->paused = true;
            v->t_hold = vd_duration_ms(v);
            aos_hal_log("video", "the end: frame %d of %u, skipped %u", (int)v->shown,
                        (unsigned)v->frames, (unsigned)(v->r_skipped + v->w_skipped));
            vd_ui_controls(v, true);
        }
    }
    stats(v, now);
}
