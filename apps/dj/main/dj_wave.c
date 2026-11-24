/*
 * DJ - the waveforms and the overviews, pixel by pixel.
 *
 * The two scrolling waveforms are one canvas: deck A's strip over deck B's,
 * the playhead in the middle, both at the same time scale in the speaker's
 * time (a pixel is so many milliseconds of what is heard, whatever each
 * track's tempo), so when the two are in sync their beat lines stand one
 * over the other. Each column is the loudest waveform entry under it, drawn
 * as three layers, one per band: the lows in blue, the mids in amber over
 * them, the highs in white on top (a kick is a tall blue column, a hi-hat a
 * small white one, a pad an amber band).
 *
 * The overview is the whole track in the deck's panel: what was played is
 * dimmed, with the cue, the hot cues, the loop and the playhead. It is
 * drawn again only when its playhead moves a pixel or the analysis gets
 * further.
 *
 * Both are RGB565 canvases in PSRAM; LVGL blits them over the surface.
 */
#include "dj_ui.h"

#include <math.h>
#include <string.h>

#define U dj_ui

static inline uint16_t c565(uint32_t rgb)
{
    return (uint16_t)((((rgb >> 16) & 0xF8) << 8) | (((rgb >> 8) & 0xFC) << 3) | ((rgb & 0xFF) >> 3));
}

static inline uint16_t rgb565(int r, int g, int b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xFF) >> 3));
}

static lv_obj_t *mk_canvas(uint16_t **buf, int32_t x, int32_t y, int32_t w, int32_t h)
{
    *buf = dj_alloc((size_t)w * h * 2);
    if (!*buf) return NULL;
    lv_obj_t *cv = lv_canvas_create(U->surf);
    lv_canvas_set_buffer(cv, *buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(cv, x, y);
    lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return cv;
}

bool dj_wave_create(void)
{
    U->wave_w = lv_area_get_width(&U->wave_r);
    U->wave_h = lv_area_get_height(&U->wave_r);
    U->wave_cv = mk_canvas(&U->wave_buf, U->wave_r.x1, U->wave_r.y1, U->wave_w, U->wave_h);
    int i = dj_ui_find(C_OVERVIEW, 0);
    if (i >= 0) {
        U->ov_w = lv_area_get_width(&U->ctl[i].r);
        U->ov_h = lv_area_get_height(&U->ctl[i].r);
        for (int d = 0; d < DJ_NDECK; d++) {
            int k = dj_ui_find(C_OVERVIEW, d);
            U->ov_cv[d] = mk_canvas(&U->ov_buf[d], U->ctl[k].r.x1, U->ctl[k].r.y1, U->ov_w, U->ov_h);
            U->ov_sig[d] = 0xFFFFFFFFu;
        }
    }
    return U->wave_cv != NULL;
}

void dj_wave_destroy(void)
{
    /* the canvases go with the root (lv_obj_clean, or the system's on a
     * turn); the buffers are ours. LVGL draws nothing in between: this
     * runs in its thread. */
    U->wave_cv = NULL;
    for (int d = 0; d < DJ_NDECK; d++) {
        U->ov_cv[d] = NULL;
        dj_free(U->ov_buf[d]);
        U->ov_buf[d] = NULL;
    }
    dj_free(U->wave_buf);
    U->wave_buf = NULL;
}

static void vline(uint16_t *buf, int32_t stride, int32_t x, int32_t y0, int32_t y1, uint16_t c)
{
    for (int32_t y = y0; y <= y1; y++) buf[y * stride + x] = c;
}

static void vline_blend(uint16_t *buf, int32_t stride, int32_t x, int32_t y0, int32_t y1, uint32_t rgb, int a)
{
    for (int32_t y = y0; y <= y1; y++) {
        uint16_t p = buf[y * stride + x];
        int r = ((p >> 11) & 31) << 3, g = ((p >> 5) & 63) << 2, b = (p & 31) << 3;
        r += ((int)((rgb >> 16) & 255) - r) * a >> 8;
        g += ((int)((rgb >> 8) & 255) - g) * a >> 8;
        b += ((int)(rgb & 255) - b) * a >> 8;
        buf[y * stride + x] = rgb565(r, g, b);
    }
}

/* one column of the waveform: the three bands, one over the other */
static void bands(uint16_t *buf, int32_t W, int32_t x, int32_t cy, int32_t half, const dj_wave_t *w, float gain,
                  bool played)
{
    static uint16_t C[2][3];
    if (!C[0][0]) {
        C[0][0] = c565(0x1F5FD6); C[0][1] = c565(0xF29A2E); C[0][2] = c565(0xF4F4F4);
        C[1][0] = c565(0x14336E); C[1][1] = c565(0x7A4F1A); C[1][2] = c565(0x7C7C7C);
    }
    const uint16_t *c = C[played ? 1 : 0];
    (void)gain;                 /* the analysis scales each band of the track to the height */
    /* the bands' levels, expanded (^1.5): the body of a loud track sits
     * lower and the hits stand out of it */
    static uint8_t EXP[256];
    if (!EXP[255]) for (int i = 0; i < 256; i++) { float v = (float)i / 255.0f; EXP[i] = (uint8_t)(v * sqrtf(v) * 255.0f); }
    int32_t hl = EXP[w->lo] * half / 255;
    int32_t hm = EXP[w->mid] * half * 3 / (255 * 4);
    int32_t hh = EXP[w->hi] * half / (255 * 2);
    if (hl < 1) hl = 1;
    vline(buf, W, x, cy - hl, cy + hl, c[0]);
    if (hm > 0) vline(buf, W, x, cy - hm, cy + hm, c[1]);
    if (hh > 0) vline(buf, W, x, cy - hh, cy + hh, c[2]);
}

/* the strip of deck d, rows [y0, y0 + h) of the canvas */
static void strip(int d, int32_t y0, int32_t h)
{
    uint16_t *buf = U->wave_buf;
    int32_t W = U->wave_w;
    const dj_pub_t *p = dj_pub(U->e, d);
    dj_track_t *t = dj_deck_track(U->e, d);
    uint16_t bg = c565(0x101012), mid = c565(0x2A2A2E);
    for (int32_t y = y0; y < y0 + h; y++) {
        uint16_t *row = buf + y * W;
        for (int32_t x = 0; x < W; x++) row[x] = bg;
    }
    int32_t cy = y0 + h / 2;
    bool ready = t->state == DJ_TS_READY && p->gen == t->gen && p->gen != 0 && t->wave;
    if (!ready) {
        for (int32_t x = 0; x < W; x++) buf[cy * W + x] = mid;
        return;
    }
    static const float SECS_PER_PX[4] = { 0.0029f, 0.0058f, 0.0116f, 0.0232f };
    float spx = SECS_PER_PX[U->zoom & 3] * (U->land ? 1.0f : 1280.0f / 720.0f);
    float rate = p->rate > 0.1f ? p->rate : 1.0f;
    float spp = spx * (float)t->sr * rate;                 /* source samples per pixel */
    float pos = (float)p->pos;
    int32_t cx = W / 2;
    float s_left = pos - (float)cx * spp;
    float gain = t->gain > 0.0f ? t->gain : 1.0f;
    int32_t half = h / 2 - 2;
    uint32_t nw = t->wave_n;

    /* the loop, under everything */
    if (p->loop_on) {
        int32_t xa = cx + (int32_t)(((float)p->loop_a - pos) / spp), xb = cx + (int32_t)(((float)p->loop_b - pos) / spp);
        if (xa < 0) xa = 0;
        if (xb > W - 1) xb = W - 1;
        for (int32_t x = xa; x <= xb; x++) vline(buf, W, x, y0, y0 + h - 1, c565(0x123A1E));
    }
    /* the beat grid */
    if (p->bpm > 0.0f) {
        float P = 60.0f / p->bpm * (float)t->sr;
        float n0 = ceilf((s_left - p->beat0) / P);
        for (float n = n0;; n += 1.0f) {
            float b = p->beat0 + n * P;
            int32_t x = cx + (int32_t)floorf((b - pos) / spp + 0.5f);
            if (x >= W) break;
            if (x < 0 || b < 0.0f) continue;
            int beat = ((int)n % 4 + 4) % 4;
            if (beat == 0) vline(buf, W, x, y0, y0 + h - 1, c565(0x55555C));
            else {
                vline(buf, W, x, y0, y0 + 7, c565(0x44444A));
                vline(buf, W, x, y0 + h - 8, y0 + h - 1, c565(0x44444A));
            }
        }
    }
    /* the sound */
    for (int32_t x = 0; x < W; x++) {
        float s = s_left + (float)x * spp;
        if (s < 0.0f) continue;
        uint32_t e0 = (uint32_t)(s / DJ_WHOP), e1 = (uint32_t)((s + spp) / DJ_WHOP);
        if (e0 >= nw) break;
        if (e1 > nw) e1 = nw;
        if (e1 <= e0) e1 = e0 + 1;
        const dj_wave_t *best = &t->wave[e0];
        for (uint32_t e = e0 + 1; e < e1; e++) if (t->wave[e].lo + t->wave[e].mid > best->lo + best->mid) best = &t->wave[e];
        bands(buf, W, x, cy, half, best, gain, x < cx);
    }
    /* the cue and the hot cues */
    static const uint32_t HOTC[DJ_HOT] = { 0xFF453A, 0x30D158, 0xBF5AF2, 0xFFD60A };
    for (int k = -1; k < DJ_HOT; k++) {
        float at = k < 0 ? p->cue : p->hot[k];
        if (at < 0.0f) continue;
        int32_t x = cx + (int32_t)floorf((at - pos) / spp + 0.5f);
        if (x < 0 || x >= W) continue;
        uint32_t col = k < 0 ? 0xFF9F0A : HOTC[k];
        vline(buf, W, x, y0, y0 + h - 1, c565(col));
        for (int32_t j = 0; j < 7; j++)
            for (int32_t i = -j; i <= j; i++)
                if (x + i >= 0 && x + i < W) buf[(y0 + 6 - j) * W + x + i] = c565(col);
    }
    /* not analysed yet */
    if (t->ana != DJ_ANA_DONE) {
        int32_t xs = cx + (int32_t)(((float)nw * DJ_WHOP - pos) / spp);
        if (xs < 0) xs = 0;
        for (int32_t x = xs; x < W; x++) buf[cy * W + x] = mid;
    }
    /* the playhead */
    vline_blend(buf, W, cx - 1, y0, y0 + h - 1, 0xFFFFFF, 200);
    vline(buf, W, cx, y0, y0 + h - 1, c565(0xFFFFFF));
    vline_blend(buf, W, cx + 1, y0, y0 + h - 1, 0xFFFFFF, 200);
    /* the deck's colour along the edge */
    uint16_t dc = c565(d == 0 ? DJ_COL_A : DJ_COL_B);
    for (int32_t y = y0; y < y0 + h; y++) { buf[y * W] = dc; buf[y * W + 1] = dc; buf[y * W + 2] = dc; }
}

void dj_wave_render(void)
{
    if (!U->wave_cv || !U->wave_buf) return;
    int i0 = dj_ui_find(C_WAVE, 0), i1 = dj_ui_find(C_WAVE, 1);
    if (i0 < 0 || i1 < 0) return;
    /* the gap between the strips */
    int32_t a0 = U->ctl[i0].r.y1 - U->wave_r.y1, h0 = lv_area_get_height(&U->ctl[i0].r);
    int32_t a1 = U->ctl[i1].r.y1 - U->wave_r.y1, h1 = lv_area_get_height(&U->ctl[i1].r);
    uint16_t bg = c565(DJ_COL_BG);
    for (int32_t y = a0 + h0; y < a1; y++)
        for (int32_t x = 0; x < U->wave_w; x++) U->wave_buf[y * U->wave_w + x] = bg;
    strip(0, a0, h0);
    strip(1, a1, h1);
    lv_obj_invalidate(U->wave_cv);
}

void dj_ov_render(int d, bool force)
{
    if (!U->ov_cv[d] || !U->ov_buf[d]) return;
    const dj_pub_t *p = dj_pub(U->e, d);
    dj_track_t *t = dj_deck_track(U->e, d);
    int32_t W = U->ov_w, H = U->ov_h;
    bool ready = t->state == DJ_TS_READY && p->gen == t->gen && p->gen != 0 && t->wave && t->nwave;
    int32_t px = ready ? (int32_t)((float)p->pos / (float)t->nsamp * (float)W) : -1;
    uint32_t sig = (uint32_t)px * 31u + (ready ? t->wave_n / 64u : 7u) * 7u + (uint32_t)t->gen * 131u +
                   (uint32_t)p->loop_on * 3u + (uint32_t)(p->cue / 512.0f);
    for (int h = 0; h < DJ_HOT; h++) sig = sig * 33u + (uint32_t)(int32_t)(p->hot[h] / 512.0f);
    if (!force && sig == U->ov_sig[d]) return;
    U->ov_sig[d] = sig;
    uint16_t *buf = U->ov_buf[d];
    uint16_t bg = c565(0x101012);
    for (int32_t i = 0; i < W * H; i++) buf[i] = bg;
    if (ready) {
        float per = (float)t->nwave / (float)W;
        float gain = t->gain > 0.0f ? t->gain : 1.0f;
        int32_t cy = H / 2;
        for (int32_t x = 0; x < W; x++) {
            uint32_t e0 = (uint32_t)((float)x * per), e1 = (uint32_t)((float)(x + 1) * per);
            if (e0 >= t->wave_n) break;
            if (e1 > t->wave_n) e1 = t->wave_n;
            if (e1 <= e0) e1 = e0 + 1;
            const dj_wave_t *best = &t->wave[e0];
            /* a stride: a long track has thousands of entries per pixel */
            uint32_t step = (e1 - e0) / 48 + 1;
            for (uint32_t e = e0 + 1; e < e1; e += step) if (t->wave[e].lo + t->wave[e].mid > best->lo + best->mid) best = &t->wave[e];
            bands(buf, W, x, cy, H / 2 - 1, best, gain, x < px);
        }
        if (p->loop_on) {
            int32_t xa = (int32_t)((float)p->loop_a / (float)t->nsamp * (float)W);
            int32_t xb = (int32_t)((float)p->loop_b / (float)t->nsamp * (float)W);
            for (int32_t x = xa; x <= xb && x < W; x++) if (x >= 0) vline_blend(buf, W, x, 0, H - 1, 0x30D158, 90);
        }
        static const uint32_t HOTC[DJ_HOT] = { 0xFF453A, 0x30D158, 0xBF5AF2, 0xFFD60A };
        for (int k = -1; k < DJ_HOT; k++) {
            float at = k < 0 ? p->cue : p->hot[k];
            if (at < 0.0f) continue;
            int32_t x = (int32_t)(at / (float)t->nsamp * (float)W);
            if (x < 0 || x >= W) continue;
            vline(buf, W, x, 0, H / 3, c565(k < 0 ? 0xFF9F0A : HOTC[k]));
        }
        if (px >= 0 && px < W) {
            vline(buf, W, px, 0, H - 1, c565(0xFFFFFF));
            if (px + 1 < W) vline(buf, W, px + 1, 0, H - 1, c565(0xFFFFFF));
        }
    } else {
        for (int32_t x = 0; x < W; x++) buf[(H / 2) * W + x] = c565(0x2A2A2E);
    }
    lv_obj_invalidate(U->ov_cv[d]);
}
