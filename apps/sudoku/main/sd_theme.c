/*
 * SUDOKU - the themes.
 *
 * A theme is a palette (here, in code) and its art (in the pack, painted by
 * tools/pack_p4.py): a 1280 x 1280 picture behind everything, which shows
 * its middle 720 x 1280 upright and its middle 1280 x 720 lying down, so the
 * one JPEG serves both; a 720 x 720 texture under the board's cells; the
 * given digits drawn in the theme's style at the two sizes a cell comes in;
 * up to three sprites that move over the picture (bubbles, ghosts, snow);
 * and a thumbnail for the shop. Each theme names one of the seven sound
 * sets.
 *
 * The art is read by a thread of its own: a JPEG of that size takes the
 * decoder a good part of a second on the board, which would freeze LVGL.
 * The thread hands over what it read and the LVGL side swaps it in
 * (sd_theme_poll), so a theme changes in one frame once it is all there.
 * The palette changes at once; until the picture arrives the theme's flat
 * colour stands in, and the same if the pack is missing.
 */
#include "sd_app.h"
#include "sd_pak.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sd_art_t sd_A = { .theme = -1 };

/* clang-format off */
const sd_theme_t SD_THEME[SD_THEMES] = {
    { "clasico", N_("Clásico"), 0,
      0x10141C, 0x1C1F26, 0xFFFFFF, 0x8E8E93, 0x0A84FF, 225,
      0xF4F1EA, 0x9A9A9A, 0x2B2B2B, 0x1A1A1A, 0x1F5FD0, 0x5E5E5E, 0xE0352B, 0x8DBBFF, 0xC9D9EE, 0x8DBBFF,
      0x4A4A4A, 0xFFD54A, 200, 130, 150,
      { SD_DECOR_WANDER }, { 5 }, 0 },
    { "campo", N_("Campo"), 150,
      0x7EC8F0, 0x203F1A, 0xFFFFFF, 0xD4ECC8, 0xF2B134, 200,
      0xF3F0DC, 0xB9C29A, 0x4E6B2E, 0x2F4A1A, 0x2D6FB0, 0x5E6B4A, 0xC6362A, 0xF2D66E, 0xE1E8C4, 0xF2C64A,
      0x4E6B2E, 0xF28C1F, 200, 140, 130,
      { SD_DECOR_CROSS, SD_DECOR_WANDER }, { 3, 2 }, 3 },
    { "madera", N_("Madera"), 200,
      0x3A2414, 0x2A1A0E, 0xF7E8D4, 0xC8A988, 0xE8A64A, 225,
      0xC99A63, 0x8A5E35, 0x4A2C14, 0x3A200C, 0x123E66, 0x4A2E16, 0xB32617, 0xF0C27A, 0xDDB27E, 0xEBA64A,
      0x4A2C14, 0xFFD15A, 220, 120, 140,
      { SD_DECOR_WANDER }, { 4 }, 3 },
    { "papiro", N_("Papiro"), 250,
      0x3B2A17, 0x2E2113, 0xF6E9CF, 0xC2A982, 0xC0712B, 225,
      0xE8D3A6, 0xA48552, 0x5A3F1E, 0x3A2510, 0x7A2E12, 0x5E4428, 0xB0221A, 0xD9A55A, 0xD8BE8A, 0xC98F4A,
      0x6B4A22, 0xF0C040, 200, 130, 130,
      { SD_DECOR_WANDER }, { 6 }, 3 },
    { "playa", N_("Playa"), 300,
      0x5BB8E8, 0x0E3B55, 0xFFFFFF, 0xCFE7F2, 0xFF8A3D, 195,
      0xF2E2C0, 0xC8B07E, 0x8A6A3A, 0x135D7A, 0xD9572B, 0x6A5A3A, 0xD62828, 0xFFC970, 0xE8D3A6, 0xFFB55A,
      0x8A6A3A, 0xFF7A00, 200, 140, 130,
      { SD_DECOR_CROSS, SD_DECOR_WAVE }, { 2, 4 }, 5 },
    { "nieve", N_("Nieve"), 350,
      0x16324F, 0x123049, 0xF2FAFF, 0xA8C6DB, 0x7FD8FF, 205,
      0xE8F4FB, 0xA9C8DC, 0x3D6688, 0x1E4E78, 0x0E7C7C, 0x5A7A93, 0xD23B3B, 0x9FE0FF, 0xD2E8F5, 0x8ED8FF,
      0x3D6688, 0xFFB74D, 210, 150, 130,
      { SD_DECOR_FALL, SD_DECOR_FALL }, { 8, 4 }, 2 },
    { "piedra", N_("Piedra"), 400,
      0x2B2B2E, 0x222224, 0xEDEBE6, 0xA09D96, 0x9CB46B, 225,
      0x8A8780, 0x5E5B55, 0x2E2D2A, 0x252220, 0x183A60, 0x34322E, 0xA8231B, 0xC9D69A, 0xA8A59E, 0xB4C47E,
      0x33312E, 0xE8C34A, 220, 110, 130,
      { SD_DECOR_FALL, SD_DECOR_WANDER }, { 3, 3 }, 4 },
    { "mar", N_("Bajo el mar"), 500,
      0x03263F, 0x062F4A, 0xE6FBFF, 0x8EC3D6, 0x2BD1C8, 200,
      0x0C4A66, 0x2D7A96, 0xA6F0FF, 0xFFFFFF, 0xFFD27F, 0x9BD8E8, 0xFF6B6B, 0x2BD1C8, 0x2A8FB8, 0x1C8FA0,
      0xA6F0FF, 0xFFE066, 200, 150, 170,
      { SD_DECOR_RISE, SD_DECOR_CROSS, SD_DECOR_CROSS }, { 7, 2, 1 }, 5 },
    { "ciudad", N_("Ciudad"), 600,
      0x0B1020, 0x111829, 0xE8EEF8, 0x8A97AD, 0xFFC23D, 210,
      0x161D2E, 0x2B3550, 0x6B7A99, 0xFFC23D, 0x7FD6FF, 0x8FA0BF, 0xFF5555, 0xFFC23D, 0x1E2840, 0x4A3D14,
      0x6B7A99, 0xFFE680, 220, 170, 180,
      { SD_DECOR_TWINKLE, SD_DECOR_CROSS }, { 10, 1 }, 1 },
    { "metal", N_("Metal"), 700,
      0x2A2D31, 0x1E2125, 0xEEF1F4, 0x9AA3AD, 0xFF8A00, 220,
      0x8E949B, 0x5C6168, 0x24272B, 0x1C1F22, 0x0B3F80, 0x2A2E33, 0xC0261A, 0xFFB14A, 0xB0B8C0, 0xFF9F3A,
      0x2B2E31, 0xFFD000, 220, 110, 120,
      { SD_DECOR_TWINKLE }, { 5 }, 2 },
    { "jardin", N_("Jardín japonés"), 800,
      0x2B1E2E, 0x241824, 0xFFF1F4, 0xC9AAB6, 0xE8577A, 210,
      0xF4EEE2, 0xC9BFAE, 0x3A2A2A, 0x1E1A1A, 0xB8283E, 0x6A5A56, 0xC61F2F, 0xF5B8C8, 0xEDE3D2, 0xF3A6B8,
      0x3A2A2A, 0xE8A33A, 200, 140, 130,
      { SD_DECOR_FALL }, { 9 }, 3 },
    { "desierto", N_("Desierto"), 900,
      0xE08A4A, 0x5A2E14, 0xFFF4E6, 0xF0C9A6, 0xFFB347, 200,
      0xE9C48E, 0xB98C55, 0x6A3E1C, 0x7A2E12, 0x1E5F86, 0x6A4A2A, 0xB3201A, 0xFFD08A, 0xE0B67E, 0xF2A65A,
      0x6A3E1C, 0xFF7A1A, 200, 130, 130,
      { SD_DECOR_CROSS, SD_DECOR_CROSS }, { 1, 6 }, 4 },
    { "neon", N_("Neón"), 1000,
      0x0B0416, 0x170A2A, 0xF6E8FF, 0xA58BC0, 0xFF2BD6, 210,
      0x0E0820, 0x3A1F66, 0x00E5FF, 0xFF4FE1, 0x5CF6FF, 0xB08CFF, 0xFF3355, 0xFF2BD6, 0x2A1550, 0x7A1F8A,
      0x00E5FF, 0xFFE14D, 230, 170, 160,
      { SD_DECOR_WANDER }, { 7 }, 1 },
    { "espacio", N_("Espacio"), 1100,
      0x05060F, 0x0D1024, 0xEEF0FF, 0x9198C2, 0x8C7CFF, 205,
      0x0C0F22, 0x262B55, 0x8C7CFF, 0xFFFFFF, 0x7FE3FF, 0x8E96C8, 0xFF5C7A, 0x8C7CFF, 0x171B3A, 0x33285E,
      0xAFA4FF, 0xFFE07A, 220, 170, 170,
      { SD_DECOR_TWINKLE, SD_DECOR_CROSS }, { 9, 1 }, 1 },
    { "embrujada", N_("Casa embrujada"), 1200,
      0x120A1C, 0x1A1026, 0xEDE3F7, 0x9A88AE, 0x8CFF6B, 215,
      0x241A2E, 0x46355A, 0x9C86BA, 0xB8FF9E, 0xFFB85C, 0x9C8AB4, 0xFF4D4D, 0x8CFF6B, 0x2F2340, 0x3E5C2E,
      0xB8FF9E, 0xFFD54A, 220, 170, 170,
      { SD_DECOR_CROSS, SD_DECOR_CROSS }, { 2, 3 }, 6 },
    { "aliens", N_("Aliens"), 1500,
      0x0D2A1F, 0x0B1E18, 0xE6FFE9, 0x8FBFA0, 0x6BFF5C, 210,
      0x10261D, 0x24503B, 0x6BFF5C, 0x6BFF5C, 0xD7A6FF, 0x7FB894, 0xFF5577, 0x6BFF5C, 0x15382A, 0x2D5A1F,
      0x9CFF8C, 0xF5FF6B, 220, 170, 170,
      { SD_DECOR_CROSS, SD_DECOR_TWINKLE }, { 1, 6 }, 1 },
};
/* clang-format on */

/* ==========================================================================
 * The loader thread
 * ========================================================================== */

typedef struct {
    int       theme;
    uint16_t *bg, *tex;
    uint8_t  *g9, *g6;
    int       g9w, g6w;
    uint8_t  *spr[3];
    int       sw[3], sh[3];
} loaded_t;

static struct {
    void         *mtx;
    volatile bool stop, running;
    volatile int  want;             /* the theme to show                    */
    volatile bool want_thumbs;
    int           shown;            /* sd_theme(): the palette in use       */
    bool          got;
    loaded_t      in;               /* handed over, waiting for the swap    */
    uint16_t     *thumb[SD_THEMES];
    bool          thumb_got[SD_THEMES];
    bool          pak_ok, pak_tried;
    bool          sounds;
} T = { .want = -1, .shown = 0 };

const sd_theme_t *sd_theme(void)
{
    int i = T.shown;
    return &SD_THEME[i >= 0 && i < SD_THEMES ? i : 0];
}

bool sd_theme_pak_ok(void)
{
    return T.pak_ok || !T.pak_tried;
}

static void free_loaded(loaded_t *l)
{
    if (l->bg) aos_hal_image_free(l->bg);
    if (l->tex) aos_hal_image_free(l->tex);
    sd_free(l->g9);
    sd_free(l->g6);
    for (int i = 0; i < 3; i++) sd_free(l->spr[i]);
    memset(l, 0, sizeof *l);
    l->theme = -1;
}

static uint16_t *jpeg(sd_pak_t *p, const char *name, int w, int h)
{
    const sd_pak_entry_t *e = sd_pak_find(p, name);
    if (!e || e->type != SDPK_JPEG) return NULL;
    int ow = 0, oh = 0;
    uint16_t *px = aos_hal_image_decode(p->path, e->off, e->clen, w, h, true, &ow, &oh, NULL, NULL);
    if (px && (ow != w || oh != h)) {
        aos_hal_image_free(px);
        px = NULL;
    }
    return px;
}

static uint8_t *sprite(sd_pak_t *p, const char *name, int *w, int *h)
{
    const sd_pak_entry_t *e = sd_pak_find(p, name);
    if (!e || e->type != SDPK_SPRITE) return NULL;
    *w = e->w;
    *h = e->h;
    return (uint8_t *)sd_pak_load(p, e);
}

static void load_theme(sd_pak_t *p, int idx, loaded_t *l)
{
    memset(l, 0, sizeof *l);
    l->theme = idx;
    const char *k = SD_THEME[idx].key;
    char name[SDPK_NAME_LEN];
    snprintf(name, sizeof name, "bg_%s", k);
    l->bg = jpeg(p, name, SD_BG_W, SD_BG_H);
    if (T.stop) return;
    snprintf(name, sizeof name, "tx_%s", k);
    l->tex = jpeg(p, name, SD_TEX, SD_TEX);
    int h;
    snprintf(name, sizeof name, "g9_%s", k);
    l->g9 = sprite(p, name, &l->g9w, &h);
    snprintf(name, sizeof name, "g6_%s", k);
    l->g6 = sprite(p, name, &l->g6w, &h);
    for (int i = 0; i < 3; i++) {
        if (!SD_THEME[idx].decor[i]) continue;
        snprintf(name, sizeof name, "d%d_%s", i, k);
        l->spr[i] = sprite(p, name, &l->sw[i], &l->sh[i]);
    }
}

static void thread_fn(void *arg)
{
    (void)arg;
    sd_pak_t pak;
    char path[200];
    snprintf(path, sizeof path, "%.180s/sudoku_p4.pak", aos_hal_path_apps());
    T.pak_ok = sd_pak_open(&pak, path);
    T.pak_tried = true;
    if (!T.pak_ok) aos_hal_log(SD_TAG, "no pack at %s: flat colours", path);

    while (!T.stop) {
        if (T.pak_ok && !T.sounds) {
            /* every set at once: they are small (~300 KB in all) */
            for (int s = 0; s < SD_SOUND_SETS; s++) {
                uint8_t *data[SD_SOUNDS] = { 0 };
                uint32_t len[SD_SOUNDS] = { 0 };
                extern const char *sd_snd_name(int i);
                for (int i = 0; i < SD_SOUNDS; i++) {
                    char name[SDPK_NAME_LEN];
                    snprintf(name, sizeof name, "s%d_%s", s, sd_snd_name(i));
                    const sd_pak_entry_t *e = sd_pak_find(&pak, name);
                    if (e && e->type == SDPK_PCM) {
                        data[i] = (uint8_t *)sd_pak_load(&pak, e);
                        len[i] = data[i] ? e->rawlen : 0;
                    }
                }
                sd_snd_load(s, data, len);
                for (int i = 0; i < SD_SOUNDS; i++) sd_free(data[i]);
            }
            T.sounds = true;
        }
        int want = T.want;
        aos_hal_mutex_lock(T.mtx);
        bool pending = T.got;
        int have = T.got ? T.in.theme : sd_A.theme;
        aos_hal_mutex_unlock(T.mtx);
        if (want >= 0 && want != have && !pending) {
            loaded_t l;
            if (T.pak_ok) {
                uint32_t t0 = aos_hal_uptime_ms();
                load_theme(&pak, want, &l);
                aos_hal_log(SD_TAG, "theme %s in %u ms", SD_THEME[want].key, (unsigned)(aos_hal_uptime_ms() - t0));
            } else {
                memset(&l, 0, sizeof l);
                l.theme = want;
            }
            if (T.stop) {
                free_loaded(&l);
                break;
            }
            aos_hal_mutex_lock(T.mtx);
            T.in = l;
            T.got = true;
            aos_hal_mutex_unlock(T.mtx);
            continue;
        }
        if (T.want_thumbs && T.pak_ok) {
            int todo = -1;
            for (int i = 0; i < SD_THEMES; i++)
                if (!T.thumb[i] && !T.thumb_got[i]) {
                    todo = i;
                    break;
                }
            if (todo >= 0) {
                char name[SDPK_NAME_LEN];
                snprintf(name, sizeof name, "th_%s", SD_THEME[todo].key);
                uint16_t *px = jpeg(&pak, name, SD_THUMB, SD_THUMB);
                aos_hal_mutex_lock(T.mtx);
                T.thumb[todo] = px;
                T.thumb_got[todo] = true;
                aos_hal_mutex_unlock(T.mtx);
                continue;
            }
        }
        aos_hal_sleep_ms(40);
    }
    if (T.pak_ok) sd_pak_close(&pak);
    T.running = false;
}

static void start(void)
{
    if (T.running) return;
    if (!T.mtx) T.mtx = aos_hal_mutex_create();
    T.stop = false;
    T.running = true;
    if (!aos_hal_thread_start("sd_art", thread_fn, NULL, 12 * 1024, 3)) {
        T.running = false;
        aos_hal_log(SD_TAG, "no art thread");
    }
}

void sd_theme_show(int idx)
{
    if (idx < 0 || idx >= SD_THEMES) idx = 0;
    T.shown = idx;
    T.want = idx;
    start();
}

bool sd_theme_ready(void)
{
    return sd_A.theme == T.want;
}

void sd_theme_thumbs(void)
{
    T.want_thumbs = true;
    start();
}

/* LVGL side: what the thread finished goes into sd_A. The caller rebuilds
 * whatever points at the old buffers before they are freed - which is why
 * the old ones are freed here only after sd_ui_set_bg() and the board took
 * the new ones. */
bool sd_theme_poll(void)
{
    if (!T.mtx) return false;
    bool changed = false;
    aos_hal_mutex_lock(T.mtx);
    for (int i = 0; i < SD_THEMES; i++) {
        if (T.thumb_got[i] && T.thumb[i] && !sd_A.thumb[i]) {
            sd_A.thumb[i] = T.thumb[i];
            changed = true;
        }
    }
    bool got = T.got;
    loaded_t in = T.in;
    T.got = false;
    memset(&T.in, 0, sizeof T.in);
    aos_hal_mutex_unlock(T.mtx);
    if (!got) return changed;

    loaded_t old = { .theme = sd_A.theme, .bg = sd_A.bg, .tex = sd_A.tex, .g9 = sd_A.g9, .g6 = sd_A.g6 };
    for (int i = 0; i < 3; i++) old.spr[i] = sd_A.spr[i];
    sd_decor_clear();
    sd_A.theme = in.theme;
    sd_A.bg = in.bg;
    sd_A.tex = in.tex;
    sd_A.g9 = in.g9;
    sd_A.g6 = in.g6;
    sd_A.g9w = in.g9w;
    sd_A.g6w = in.g6w;
    for (int i = 0; i < 3; i++) {
        sd_A.spr[i] = in.spr[i];
        sd_A.spr_w[i] = in.sw[i];
        sd_A.spr_h[i] = in.sh[i];
    }
    /* the descriptors are the same structs with new pixels: drop what LVGL
     * may have cached of the old ones before they are freed */
    lv_image_cache_drop(NULL);
    sd_ui_set_bg();
    sd_board_all();
    sd_board_present();
    free_loaded(&old);
    return true;
}

void sd_theme_stop(void)
{
    T.stop = true;
    for (int i = 0; i < 400 && T.running; i++) aos_hal_sleep_ms(10);
    if (T.running) aos_hal_log(SD_TAG, "art thread did not stop");
    /* whatever it handed over and nobody took */
    if (T.got) {
        free_loaded(&T.in);
        T.got = false;
    }
    loaded_t cur = { .theme = sd_A.theme, .bg = sd_A.bg, .tex = sd_A.tex, .g9 = sd_A.g9, .g6 = sd_A.g6 };
    for (int i = 0; i < 3; i++) cur.spr[i] = sd_A.spr[i];
    free_loaded(&cur);
    for (int i = 0; i < SD_THEMES; i++) {
        if (sd_A.thumb[i]) aos_hal_image_free(sd_A.thumb[i]);
        else if (T.thumb[i]) aos_hal_image_free(T.thumb[i]);
        T.thumb[i] = NULL;
        T.thumb_got[i] = false;
    }
    memset(&sd_A, 0, sizeof sd_A);
    sd_A.theme = -1;
    T.want_thumbs = false;
    T.sounds = false;
    T.pak_tried = false;
}

/* ==========================================================================
 * The moving background
 * ========================================================================== */

#define DECOR_MAX 14

typedef struct {
    lv_obj_t *obj;
    int       kind, spr;
    float     x, y, vx, vy, t, phase, period;
    int       w, h;
} mover_t;

static struct {
    lv_obj_t      *parent;
    mover_t        m[DECOR_MAX];
    int            n;
    lv_image_dsc_t dsc[3];
    uint32_t       rng;
    int            theme;
} DC = { .theme = -1 };

static float frand(void)
{
    return (float)(sd_rand(&DC.rng) & 0xFFFF) / 65535.0f;
}

static void spawn(mover_t *m, bool first)
{
    int W = sd_U.W, H = sd_U.H;
    m->t = 0;
    m->phase = frand() * 6.28f;
    m->period = 2.0f + frand() * 3.0f;
    switch (m->kind) {
    case SD_DECOR_RISE:
        m->x = frand() * (float)(W - m->w);
        m->y = first ? frand() * (float)H : (float)H + frand() * 200.0f;
        m->vy = -(30.0f + frand() * 50.0f);
        m->vx = 0;
        break;
    case SD_DECOR_FALL:
        m->x = frand() * (float)(W - m->w);
        m->y = first ? frand() * (float)H : -(float)m->h - frand() * 200.0f;
        m->vy = 25.0f + frand() * 45.0f;
        m->vx = 0;
        break;
    case SD_DECOR_CROSS: {
        bool right = (sd_rand(&DC.rng) & 1) != 0;
        float sp = 25.0f + frand() * 45.0f;
        if (m->w < 80) sp *= 1.7f;          /* small ones (bats, gulls) fly faster */
        m->vx = right ? sp : -sp;
        m->x = first ? frand() * (float)W : (right ? -(float)m->w - frand() * 300.0f : (float)W + frand() * 300.0f);
        m->y = 60.0f + frand() * (float)(H - 120 - m->h);
        m->vy = 0;
        break;
    }
    case SD_DECOR_WAVE: {
        /* foam along the bottom, where the sea is in the picture */
        bool right = (sd_rand(&DC.rng) & 1) != 0;
        float sp = 18.0f + frand() * 18.0f;
        m->vx = right ? sp : -sp;
        m->x = first ? frand() * (float)W : (right ? -(float)m->w - frand() * 200.0f : (float)W + frand() * 200.0f);
        m->y = (float)H - 70.0f - frand() * (float)H * 0.12f - (float)m->h;
        m->vy = 0;
        break;
    }
    case SD_DECOR_TWINKLE:
        m->x = frand() * (float)(W - m->w);
        m->y = frand() * (float)(H - m->h);
        m->vx = m->vy = 0;
        break;
    default:            /* wander */
        m->x = frand() * (float)(W - m->w);
        m->y = frand() * (float)(H - m->h);
        m->vx = (frand() - 0.5f) * 40.0f;
        m->vy = (frand() - 0.5f) * 40.0f;
        break;
    }
}

void sd_decor_clear(void)
{
    for (int i = 0; i < DC.n; i++)
        if (DC.m[i].obj && lv_obj_is_valid(DC.m[i].obj)) lv_obj_delete(DC.m[i].obj);
    DC.n = 0;
    DC.theme = -1;
}

void sd_decor_build(lv_obj_t *parent)
{
    sd_decor_clear();
    DC.parent = parent;
    if (!parent || !sd_D.anim || sd_A.theme < 0) return;
    if (!DC.rng) DC.rng = aos_hal_uptime_ms() | 1u;
    const sd_theme_t *t = &SD_THEME[sd_A.theme];
    DC.theme = sd_A.theme;
    for (int s = 0; s < 3; s++) {
        if (!t->decor[s] || !sd_A.spr[s]) continue;
        lv_image_dsc_t *d = &DC.dsc[s];
        memset(d, 0, sizeof *d);
        d->header.magic = LV_IMAGE_HEADER_MAGIC;
        d->header.cf = LV_COLOR_FORMAT_RGB565A8;
        d->header.w = (uint32_t)sd_A.spr_w[s];
        d->header.h = (uint32_t)sd_A.spr_h[s];
        d->header.stride = (uint32_t)sd_A.spr_w[s] * 2;
        d->data_size = (uint32_t)(sd_A.spr_w[s] * sd_A.spr_h[s] * 3);
        d->data = sd_A.spr[s];
        for (int k = 0; k < t->decor_n[s] && DC.n < DECOR_MAX; k++) {
            mover_t *m = &DC.m[DC.n++];
            memset(m, 0, sizeof *m);
            m->kind = t->decor[s];
            m->spr = s;
            m->w = sd_A.spr_w[s];
            m->h = sd_A.spr_h[s];
            m->obj = lv_image_create(parent);
            lv_image_set_src(m->obj, d);
            lv_obj_remove_flag(m->obj, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(m->obj, LV_OBJ_FLAG_IGNORE_LAYOUT);
            spawn(m, true);
            lv_obj_set_pos(m->obj, (int32_t)m->x, (int32_t)m->y);
        }
    }
}

void sd_decor_step(uint32_t dt_ms)
{
    if (!DC.n) return;
    float dt = (float)dt_ms / 1000.0f;
    if (dt > 0.2f) dt = 0.2f;
    int W = sd_U.W, H = sd_U.H;
    for (int i = 0; i < DC.n; i++) {
        mover_t *m = &DC.m[i];
        if (!m->obj) continue;
        m->t += dt;
        float ox = 0, oy = 0;
        int opa = 255;
        switch (m->kind) {
        case SD_DECOR_RISE:
            m->y += m->vy * dt;
            ox = 10.0f * sinf(m->t * 2.0f + m->phase);
            if (m->y < -(float)m->h) spawn(m, false);
            break;
        case SD_DECOR_FALL:
            m->y += m->vy * dt;
            ox = 22.0f * sinf(m->t * 1.3f + m->phase);
            if (m->y > (float)H) spawn(m, false);
            break;
        case SD_DECOR_WAVE:
            m->x += m->vx * dt;
            oy = 4.0f * sinf(m->t * 1.1f + m->phase);
            opa = (int)(150.0f + 100.0f * sinf(m->t * 0.9f + m->phase));
            if ((m->vx > 0 && m->x > (float)W + 40) || (m->vx < 0 && m->x < -(float)m->w - 40)) spawn(m, false);
            break;
        case SD_DECOR_CROSS:
            m->x += m->vx * dt;
            oy = 10.0f * sinf(m->t * 1.7f + m->phase);
            if ((m->vx > 0 && m->x > (float)W + 40) || (m->vx < 0 && m->x < -(float)m->w - 40)) spawn(m, false);
            break;
        case SD_DECOR_TWINKLE: {
            float s = 0.5f + 0.5f * sinf(m->t * 6.28f / m->period + m->phase);
            opa = (int)(255.0f * s * s);
            if (m->t > m->period * 3.0f && opa < 8) spawn(m, false);
            break;
        }
        default: {
            m->vx += (frand() - 0.5f) * 30.0f * dt;
            m->vy += (frand() - 0.5f) * 30.0f * dt;
            if (m->vx > 30) m->vx = 30;
            if (m->vx < -30) m->vx = -30;
            if (m->vy > 30) m->vy = 30;
            if (m->vy < -30) m->vy = -30;
            m->x += m->vx * dt;
            m->y += m->vy * dt;
            if (m->x < 0) { m->x = 0; m->vx = -m->vx; }
            if (m->y < 0) { m->y = 0; m->vy = -m->vy; }
            if (m->x > W - m->w) { m->x = (float)(W - m->w); m->vx = -m->vx; }
            if (m->y > H - m->h) { m->y = (float)(H - m->h); m->vy = -m->vy; }
            float s = 0.6f + 0.4f * sinf(m->t * 6.28f / m->period + m->phase);
            opa = (int)(255.0f * s);
            break;
        }
        }
        lv_obj_set_pos(m->obj, (int32_t)(m->x + ox), (int32_t)(m->y + oy));
        if (m->kind == SD_DECOR_TWINKLE || m->kind == SD_DECOR_WANDER || m->kind == SD_DECOR_WAVE)
            lv_obj_set_style_image_opa(m->obj, (lv_opa_t)opa, 0);
    }
}
