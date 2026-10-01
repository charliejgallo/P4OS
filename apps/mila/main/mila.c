/*
 * MILA - a sweet black kitten pushes things back to their place, one cell
 * at a time, around the house: the app (see ml_app.h for the map of files)
 *
 * Every word on screen is wrapped in _(): the LVGL panels directly, and the
 * words inside the frames are rendered from _() into masks, so they follow
 * the language too. World and level names come from the pack in the three
 * languages (ml_level.h), so a new world needs no new strings in the code.
 *
 * P4OS: the watch's game on the 5" screen, upright or lying down, with the
 * art rendered again from Blender at 1.5 x (tools/pack_p4.py makes
 * mila_p4.pak). Touch only: the watch's BOOT button (undo, pause) is the
 * HUD's buttons here. The link to another board (visits, races) only shows
 * when aos_hal_link_start() works.
 */
#include "ml_app.h"
#include "ml_audio.h"
#include "ml_casita.h"
#include "ml_link.h"
#include "ml_map.h"
#include "ml_ui.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "aos_gesture.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TICK_MS         8
#define WORKER_STACK    (12 * 1024)
/* a finger that travels this far is a swipe (the watch's 22 px were for a
 * 1.8" face: the glass here is 5") */
#define SWIPE_PX        40
#define REPEAT_MS       190
#define REPEAT_FIRST_MS 550         /* a swipe held this long starts repeating */
#define PEEK_MS         380
#define ZOOM_S          0.9f
#define PERF_MS         3000        /* the log's line while a scene runs     */
#define IDLE_MS         10          /* nothing changed: the worker waits this */
#define FRAME_MIN_US    16000       /* at most ~60 frames a second: a small change
                                     * draws in a millisecond, and faster than the
                                     * panel shows is only PSRAM traffic */
/* the overview's margins for its title and its hint */
#define OV_TOP          (ML_W > ML_H ? 150 : 170)
#define OV_BOT          (ML_W > ML_H ? 90 : 130)

/* ---- worlds and levels ---- */

bool mla_world_open(const app_t *a, int world)
{
    if (world < 0 || world >= a->worlds.nworlds) return false;
    if (a->dev_unlock) return true;
    return ml_prog_total_stars(&a->prog, &a->worlds) >= a->worlds.w[world].need;
}

bool mla_level_open(const app_t *a, int world, int level)
{
    if (!mla_world_open(a, world)) return false;
    if (level < 0 || level >= a->worlds.w[world].nlevels) return false;
    if (a->dev_unlock || level == 0) return true;
    return a->prog.stars[world][level - 1] > 0;
}

const char *mla_world_name(const app_t *a, int world, char *buf, int n)
{
    buf[0] = 0;
    if (world < 0 || world >= a->worlds.nworlds) return buf;
    return ml_pick_lang(a->worlds.w[world].name, buf, n);
}

const char *mla_level_name(const app_t *a, int world, int level, char *buf, int n)
{
    buf[0] = 0;
    if (world < 0 || world >= a->worlds.nworlds || level < 0 || level >= a->worlds.w[world].nlevels) return buf;
    return ml_pick_lang(a->worlds.w[world].lv[level].title, buf, n);
}

void mla_save(app_t *a)
{
    ml_prog_save(&a->prog, &a->worlds);
}

/* --------------------------------------------------------------------------
 * The worker
 * -------------------------------------------------------------------------- */

static uint64_t s_last_yield;

static void worker_yield(void)
{
    uint64_t now = aos_hal_uptime_ms();
    if ((uint32_t)(now - s_last_yield) > 1000) {
        aos_hal_worker_sleep(10);
        s_last_yield = aos_hal_uptime_ms();
    }
}

static uint32_t clock_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

/* The whole-level picture is only needed while the level shows it (the
 * overview, the zoom onto Mila, a peek): 1.8 MB of PSRAM the rest of the
 * time. Worker only. */
static bool ov_ensure(app_t *a)
{
    if (!a->ov) a->ov = (uint16_t *)ml_malloc((size_t)ML_W * ML_H * 2);
    return a->ov != NULL;
}

static void ov_drop(app_t *a)
{
    free(a->ov);
    a->ov = NULL;
}

static void level_free(app_t *a)
{
    a->level_ok = false;
    ml_play_free(&a->play);
    ml_world_free(&a->w);
    ov_drop(a);
}

static void paint_overview(app_t *a, bool things)
{
    static ml_ov_item_t extra[48];
    if (!ov_ensure(a)) return;
    int n = things ? ml_play_ov_items(&a->play, extra, 48) : 0;
    ml_overview_paint(&a->w, &a->ovv, a->ov, extra, n);
}

/* the rows the level may use and the overview's fit, for the screen as it is */
static void level_view(app_t *a)
{
    a->play.view_top = HUD_TOP_H;
    a->play.view_bot = ML_H - HUD_BOT_H;
    ml_overview_fit(&a->w, ML_W, ML_H, OV_TOP, OV_BOT, &a->ovv);
}

static bool load_level(app_t *a, int wi, int li)
{
    level_free(a);
    const ml_world_info_t *wd = &a->worlds.w[wi];
    int n1, n2;
    ml_kit_counts(wd->kit, &n1, &n2);
    if (!ml_level_load(&a->lv, wd->lv[li].id, n1, n2)) {
        aos_hal_log("mila", "level %s: missing or malformed", wd->lv[li].id);
        return false;
    }
    if (!ov_ensure(a)) return false;
    /* the rules' state lives in play; the world reads it */
    ml_cache_fit();
    if (!ml_world_init(&a->w, &a->lv, &a->play.st, wd->kit)) {
        aos_hal_log("mila", "level %s: no memory for the cache", wd->lv[li].id);
        return false;
    }
    ml_mila_load(&a->mila, ML_SET_GAME, a->prog.hat, (uint32_t)a->prog.hat_col, a->prog.neck,
                 (uint32_t)a->prog.neck_col);
    ml_play_init(&a->play, &a->lv, &a->w, &a->mila, wd->kit);
    ml_world_sync(&a->w);
    level_view(a);
    ml_play_camera(&a->play, 0, true);
    paint_overview(a, true);
    a->world = wi;
    a->level = li;
    a->lmode = LM_OVERVIEW;
    a->mode_t = 0;
    a->result_shown = false;
    a->level_ok = true;
    a->want_full = true;
    return true;
}

/* ---- the frames ---- */

static void pend_all(app_t *a)
{
    for (int i = 0; i < ML_NFB; i++) ml_dmg_full(&a->pend[i]);
    ml_dmg_full(&a->panel_acc);
}

/* Page flipping's record of the panel's buffers (ml_app.h): nothing known.
 * Worker (frames_fit), or the timer while the worker waits. */
static void flip_reset(app_t *a)
{
    memset(a->pb, 0, sizeof a->pb);
    memset(a->pb_at, 0, sizeof a->pb_at);
    a->flips = 0;
    ml_dmg_clear(&a->flip_acc);
    a->front = NULL;
    for (int i = 0; i < ML_NFB; i++) a->back[i] = NULL;
}

/* The bands, internal RAM the sprites are blended in, copied out once, one
 * per core; when there is no room for two, two of half the rows, and one if
 * even that fails. */
static void bands_fit(app_t *a)
{
    if (a->band_w == ML_W && a->band) return;
    free(a->band);
    free(a->band2);
    a->band = a->band2 = NULL;
    a->band_rows = ML_BAND_BYTES / (ML_W * 2);
    for (int tries = 0; tries < 3; tries++) {
        a->band = (uint16_t *)ml_malloc_band((size_t)ML_W * a->band_rows * 2);
        a->band2 = a->band ? (uint16_t *)ml_malloc_band((size_t)ML_W * a->band_rows * 2) : NULL;
        if (a->band2 || tries == 2) break;
        free(a->band);
        a->band = NULL;
        if (a->band_rows <= 4) break;
        a->band_rows /= 2;
    }
    if (!a->band) {
        a->band_rows = ML_BAND_BYTES / (ML_W * 2);
        a->band = (uint16_t *)ml_malloc_band((size_t)ML_W * a->band_rows * 2);
    }
    if (!a->band) a->band_rows = 32;           /* straight into PSRAM */
    a->band_w = (int16_t)ML_W;
    aos_hal_log("mila", "bands of %d rows, %s, in %s", a->band_rows,
                a->band2 ? "two cores" : a->band ? "one core" : "no internal RAM",
                ml_bands_psram() ? "PSRAM" : "internal RAM");
}

/* Where the frames go for the screen as it is now (worker, or create):
 *   - page flipping, when the panel has a free buffer (not the simulator,
 *     nor pref "fbs" = 1): upright the bands are copied into it as they are;
 *     lying down the PPA turns each band into it (bands of 8 rows or more:
 *     4 hung the PPA, docs/APPS-P4.md). One frame of our own is enough
 *     then: the worker hands it to the timer only for the flip's moment;
 *   - otherwise the timer pushes the frame's changes into the buffer on
 *     screen, as before page flipping. */
static void flip_fit(app_t *a)
{
    int rot = aos_hal_display_get_rotation();
    bool upright = rot == 0 && ML_W == AOS_PANEL_W && ML_H == AOS_PANEL_H;
    bool turned = (rot == 90 || rot == 270) && ML_W == AOS_PANEL_H && ML_H == AOS_PANEL_W && a->band &&
                  a->band_rows >= 8 && (ML_H % a->band_rows == 0 || ML_H % a->band_rows >= 8);
    bool was = a->flip, was_turn = a->flip_turn;
    a->flip = (upright || turned) && aos_hal_display_back() != NULL;
    a->flip_turn = a->flip && !upright;
    a->flip_hint = false;
    flip_reset(a);
    if (a->flip != was || a->flip_turn != was_turn || !a->fw)
        aos_hal_log("mila", "frames: %s", !a->flip ? "their changes pushed into the buffer on screen"
                                          : a->flip_turn ? "bands turned into the panel's free buffer by the PPA, flipped to"
                                                         : "bands copied into the panel's free buffer, flipped to");
}

/* The frames for the screen as it is now. Page flipping (flip_fit): one,
 * the canvas's picture and what the worker draws into. Otherwise two, one
 * on the screen and one being drawn (the blit copies the shown one into the
 * panel's framebuffer and returns, so two keep the worker busy but for the
 * push), or one in the casita, whose art is the biggest: there the worker
 * waits for the push (a few ms: little changes there). Worker (or create,
 * before it starts). A frame taken away must not be the canvas's
 * (mla_set_state points the canvas at fb[0] first). */
static bool frames_fit(app_t *a, int n)
{
    ml_vw = AOS_SCREEN_W;
    ml_vh = AOS_SCREEN_H;
    bool ok = true;
    bool resize = a->fw * a->fh != ML_W * ML_H;
    bands_fit(a);
    flip_fit(a);
    if (a->flip) n = 1;
    for (int i = 0; i < ML_NFB; i++) {
        bool want = i < n;
        if (a->fb[i] && (resize || !want)) {
            aos_hal_io_free(a->fb[i]);
            a->fb[i] = NULL;
        }
        if (want && !a->fb[i]) {
            /* on a 128-byte line (aos_hal_io_alloc): the DMA2D and the PPA
             * read whole cache lines, and every run of rows the push takes
             * starts on one (32 rows of 720 or 1280 px are whole lines) */
            a->fb[i] = (uint16_t *)aos_hal_io_alloc((size_t)ML_W * ML_H * 2);
            if (a->fb[i]) memset(a->fb[i], 0, (size_t)ML_W * ML_H * 2);
            else ok = false;
        }
    }
    a->nfb = n;
    a->fw = (int16_t)ML_W;
    a->fh = (int16_t)ML_H;
    for (int i = 0; i < ML_NFB; i++) a->fb_state[i] = a->fb[i] ? FB_FREE : FB_NONE;
    a->shown = -1;
    a->canvas_wait = false;
    pend_all(a);
    a->want_full = true;
    aos_hal_log("mila", "frames %dx%d: %d", ML_W, ML_H, n);
    for (int i = 0; i < n; i++) ok = ok && a->fb[i];
    return ok;
}

/* What the app holds in PSRAM right now, in KB, for the logs: the art
 * unpacked from the pack, the background cache, the frames, the whole-level
 * picture (the simulator's heap numbers are made up: this is what counts) */
static void mem_log(app_t *a, const char *what)
{
    unsigned art = (unsigned)(ml_art_bytes() / 1024);
    unsigned unz = (unsigned)(ml_zbytes() / 1024);
    unsigned cache = (a->w.cc || a->casita ? 1u : 0u) * (unsigned)((size_t)ML_CW * ML_CH * 4 / 1024);
    int nf = 0;
    for (int i = 0; i < ML_NFB; i++) nf += a->fb[i] != NULL;
    unsigned frames = (unsigned)((size_t)a->fw * a->fh * 2 * (size_t)nf / 1024);
    unsigned ov = a->ov ? (unsigned)((size_t)ML_W * ML_H * 2 / 1024) : 0;
    unsigned band = (unsigned)((a->band ? 1 : 0) + (a->band2 ? 1 : 0)) * (unsigned)((size_t)ML_W * a->band_rows * 2 / 1024);
    aos_hal_log("mila", "memory in %s: art %u KB + unpacked %u + cache %u + frames %u (%d) + overview %u = %u KB of PSRAM; bands %u KB internal",
                what, art, unz, cache, frames, nf, ov, art + unz + cache + frames + ov, band);
}

static void run_job(app_t *a, int j)
{
    uint32_t hi = 0, hp = 0;
    uint64_t t0 = aos_hal_uptime_ms();
    switch (j) {
    case JOB_BOOT: {
        char path[96];
        snprintf(path, sizeof path, "%s/mila_p4.pak", aos_hal_path_apps());
        bool ok = ml_art_open(path) && ml_worlds_load(&a->worlds);
        if (ok) {
            ml_ui_job(a, UJ_ICONS);
            ml_hud_icons_load(&a->hud);
        }
        aos_hal_heap_info(&hi, &hp);
        aos_hal_log("mila", "pack %s, %d worlds in %u ms | psram %u", ok ? "open" : "MISSING", a->worlds.nworlds,
                    (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0), (unsigned)hp);
        a->job_ok = ok;
        break;
    }
    case JOB_LEVEL:
        a->scene = SC_NONE;
        mlc_close(a);
        mlm_close(a);
        a->job_ok = frames_fit(a, 2) && load_level(a, a->job_world, a->job_level);
        if (a->job_ok) {
            a->scene_seq = a->seq;
            a->scene = SC_LEVEL;
        }
        aos_hal_heap_info(&hi, &hp);
        aos_hal_log("mila", "level %s %s in %u ms, art %u KB | internal %u, psram %u",
                    a->worlds.w[a->job_world].lv[a->job_level].id, a->job_ok ? "ok" : "FAILED",
                    (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0), (unsigned)(ml_art_bytes() / 1024),
                    (unsigned)hi, (unsigned)hp);
        mem_log(a, "the level");
        break;
    case JOB_LEAVE:
        a->scene = SC_NONE;
        level_free(a);
        a->job_ok = true;
        break;
    case JOB_PEEK:
        paint_overview(a, true);
        a->peek_ready = a->ov != NULL;
        aos_hal_log("mila", "peek painted in %u ms", (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0));
        a->job_ok = true;
        break;
    case JOB_UI:
        ml_ui_job(a, a->ui_job);
        a->job_ok = true;
        break;
    case JOB_OUTFIT:
        ml_mila_load(&a->mila, a->mila.sets, a->prog.hat, (uint32_t)a->prog.hat_col, a->prog.neck,
                     (uint32_t)a->prog.neck_col);
        a->want_full = true;
        a->job_ok = true;
        break;
    case JOB_CASITA:
        a->scene = SC_NONE;
        level_free(a);
        mlm_close(a);
        /* one frame here: the casita's art is the biggest (see frames_fit) */
        frames_fit(a, 1);
        a->job_ok = mlc_open(a);
        if (a->job_ok) {
            a->scene_seq = a->seq;
            a->scene = SC_CASITA;
            a->want_full = true;
        }
        aos_hal_heap_info(&hi, &hp);
        aos_hal_log("mila", "casita in %u ms, art %u KB | internal %u, psram %u",
                    (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0), (unsigned)(ml_art_bytes() / 1024),
                    (unsigned)hi, (unsigned)hp);
        mem_log(a, "the casita");
        break;
    case JOB_MAP:
        a->scene = SC_NONE;
        level_free(a);
        mlc_close(a);
        /* the map has no kitten of hers: her frames make room for the panels */
        ml_mila_free(&a->mila);
        a->job_ok = frames_fit(a, 2) && mlm_open(a);
        if (a->job_ok) {
            a->scene_seq = a->seq;
            a->scene = SC_MAP;
            a->want_full = true;
        }
        aos_hal_heap_info(&hi, &hp);
        aos_hal_log("mila", "map in %u ms, art %u KB | internal %u, psram %u",
                    (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0), (unsigned)(ml_art_bytes() / 1024),
                    (unsigned)hi, (unsigned)hp);
        mem_log(a, "the map");
        break;
    case JOB_FIT: {
        /* the screen turned: frames, bands and cache of the new shape; the
         * art stays, the camera and the overview are worked out again */
        bool ok = frames_fit(a, a->casita ? 1 : 2);
        ml_cache_fit();
        if (a->level_ok) {
            ok = ml_world_refit(&a->w) && ok;
            level_view(a);
            ml_play_camera(&a->play, 0, true);
            if (a->ov) paint_overview(a, true);
        }
        if (a->casita) ok = mlc_refit(a) && ok;
        if (a->map) mlm_refit(a);
        a->job_ok = ok;
        aos_hal_heap_info(&hi, &hp);
        aos_hal_log("mila", "the screen turned: %d x %d, %s in %u ms | psram %u", ML_W, ML_H, ok ? "ok" : "NO MEMORY",
                    (unsigned)(uint32_t)(aos_hal_uptime_ms() - t0), (unsigned)hp);
        break;
    }
    default:
        break;
    }
}

static int free_fb(app_t *a)
{
    for (int i = 0; i < ML_NFB; i++)
        if (a->fb_state[i] == FB_FREE) return i;
    return -1;
}

/* ---- a level's frame ---- */

static float ease(float u)
{
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    return u * u * (3 - 2 * u);
}

static void level_step(app_t *a, float dt)
{
    ml_play_t *p = &a->play;
    a->mode_t += dt;
    switch (a->lmode) {
    case LM_ZOOM:
        if (a->mode_t >= ZOOM_S) {
            a->lmode = LM_PLAY;
            a->mode_t = 0;
        }
        break;
    case LM_PLAY:
    case LM_PEEK:
    case LM_WON:
        if (a->lmode != LM_PEEK) ml_play_step(p, dt);
        ml_play_camera(p, dt, false);
        if (p->won && a->lmode == LM_PLAY) {
            a->lmode = LM_WON;
            a->mode_t = 0;
        }
        break;
    default:
        break;
    }
    /* the whole-level picture goes while the level is played (a peek paints it again) */
    if ((a->lmode == LM_PLAY || a->lmode == LM_WON) && a->ov) {
        a->peek_ready = false;
        ov_drop(a);
    }
    if (a->hud_flash_undo > 0) a->hud_flash_undo -= dt;
    if (a->hud_flash_restart > 0) a->hud_flash_restart -= dt;
    uint32_t ev = p->events;
    p->events = 0;
    if (ev) {
        a->ev_ring[a->ev_w & 15] = ev;
        a->ev_w++;
    }
}

/* the frame's snapshot of what the bands draw (both cores read it) */
static struct {
    int  mode;
    bool peek;
    ml_hud_level_t hs;
} s_fr;

static void level_hud_state(app_t *a, ml_hud_level_t *hs)
{
    ml_play_t *p = &a->play;
    ml_hud_level_t h = {
        .moves = p->moves, .par = a->lv.par, .on = ml_play_on_target(p), .targets = a->lv.map.ntargets,
        .msg_t = a->lmode == LM_WON ? a->mode_t : -1, .undo_flash = a->hud_flash_undo,
        .restart_flash = a->hud_flash_restart,
        .race = a->race, .rival_on = a->rival_on, .rival_moves = a->rival_moves,
    };
    *hs = h;
}

static uint32_t hud_sig(const ml_hud_level_t *h)
{
    int msg = h->msg_t < 0 ? -1 : h->msg_t < 0.2f ? (int)(h->msg_t * 100) : 100;
    uint32_t v[] = { (uint32_t)h->moves, (uint32_t)h->par, (uint32_t)h->on, (uint32_t)h->targets, (uint32_t)msg,
                     h->undo_flash > 0, h->restart_flash > 0, h->race, (uint32_t)h->rival_on,
                     (uint32_t)h->rival_moves };
    uint32_t s = 2166136261u;
    for (size_t i = 0; i < sizeof v / sizeof v[0]; i++) s = (s ^ v[i]) * 16777619u;
    return s;
}

static void level_band(app_t *a, ml_img_t *im, int y0, int y1)
{
    ml_play_t *p = &a->play;
    int mode = s_fr.mode;
    int xa = im->cx0, n = im->cx1 - im->cx0;
    if ((mode == LM_OVERVIEW || s_fr.peek) && a->ov) {
        for (int y = y0; y < y1; y++)
            memcpy(im->px + (size_t)y * im->w + xa, a->ov + (size_t)y * ML_W + xa, (size_t)n * 2);
        if (mode == LM_OVERVIEW) ml_hud_overview(&a->hud, im, a->lv.par, a->mode_t, a->mode_t > 0.6f);
        return;
    }
    ml_render_band(&a->w, im, p->icam_x, p->icam_y, y0, y1, &a->dl);
    if (mode == LM_ZOOM && a->ov) {
        float e = ease(a->mode_t / ZOOM_S);
        /* the whole-level picture flies in onto where the camera will be;
         * the real frame fades in at the end */
        float cx1 = (float)p->icam_x + ML_W / 2.0f, cy1 = (float)p->icam_y + ML_H / 2.0f;
        float cx0 = (ML_W / 2.0f - a->ovv.ox) / a->ovv.scale, cy0 = (ML_H / 2.0f - a->ovv.oy) / a->ovv.scale;
        float cx = cx0 + (cx1 - cx0) * e, cy = cy0 + (cy1 - cy0) * e;
        float sc = a->ovv.scale * powf(1.0f / a->ovv.scale, e);  /* screen px per LP px */
        int k = e < 0.6f ? 255 : (int)(255 * (1 - (e - 0.6f) / 0.4f));
        ml_zoom_band(a->ov, &a->ovv, im, y0, y1, cx, cy, sc, k);
        return;
    }
    ml_hud_level(&a->hud, im, &s_fr.hs);
}

/* the level's part of a frame: step, draw list, what changed */
static void level_frame(app_t *a, float dt, ml_dmg_t *D)
{
    ml_play_t *p = &a->play;
    level_step(a, dt);
    memcpy(&a->dl_prev, &a->dl, sizeof a->dl);
    ml_play_draw(p, &a->dl);
    int mode = a->lmode;
    bool peek = mode == LM_PEEK && a->peek_ready;
    static bool s_prev_peek;
    int dirx = p->icam_x > a->prev_cam_x ? 1 : p->icam_x < a->prev_cam_x ? -1 : 0;
    int diry = p->icam_y > a->prev_cam_y ? 1 : p->icam_y < a->prev_cam_y ? -1 : 0;
    bool moved = dirx || diry;
    if (mode != LM_OVERVIEW) ml_world_prepare(&a->w, p->icam_x, p->icam_y, dirx, diry, 2, moved ? NULL : D);
    level_hud_state(a, &s_fr.hs);
    uint32_t sig = hud_sig(&s_fr.hs);
    if (mode != a->prev_mode || peek != s_prev_peek) {
        ml_dmg_full(D);
    } else if (mode == LM_OVERVIEW) {
        if (a->mode_t > 0.6f) ml_hud_hint_box(&a->hud, D);
    } else if (mode == LM_ZOOM) {
        ml_dmg_full(D);
    } else if (mode == LM_PEEK) {
        /* the picture stands still */
    } else if (moved) {
        ml_dmg_full(D);
    } else {
        ml_dlist_damage(&a->dl_prev, &a->dl, p->icam_x, p->icam_y, D);
        if (sig != a->hud_sig) ml_hud_level_boxes(&a->hud, &s_fr.hs, D);
        /* a region drawn again gets the HUD over it anyway: only a HUD that
         * changed (a number, a flash, the banner fading in) needs its boxes */
    }
    a->hud_sig = sig;
    a->prev_mode = mode;
    s_prev_peek = peek;
    a->prev_cam_x = p->icam_x;
    a->prev_cam_y = p->icam_y;
    s_fr.mode = mode;
    s_fr.peek = peek;
}

/* ---- the bands, on both cores ---- */

typedef struct {
    app_t    *a;
    uint16_t *fb;
    uint16_t *back;         /* the panel's free buffer, a copy of each band too */
    bool      turn;         /* ... turned into it by the PPA (lying down) */
    uint16_t *band[2];
    const ml_dmg_t *r;
    int       sc;           /* the scene */
    int       parts;        /* 0 = two, one per core; 1 = all here */
    uint32_t  px[2];        /* pixels drawn, per part (the logs) */
} band_job_t;

static void band_half(void *arg, int part)
{
    band_job_t *j = (band_job_t *)arg;
    app_t *a = j->a;
    int br = a->band_rows, step = j->parts == 1 ? 1 : 2;
    uint16_t *own = j->band[part];
    uint32_t px = 0;
    for (int y0 = part * br; y0 < ML_H; y0 += step * br) {
        int y1 = y0 + br > ML_H ? ML_H : y0 + br;
        int x0, x1;
        if (!ml_dmg_span(j->r, y0, y1, &x0, &x1)) continue;
        ml_img_t bim;
        uint16_t *band = own ? own : j->fb + (size_t)y0 * ML_W;
        ml_img_init(&bim, band - (size_t)y0 * ML_W, ML_W, ML_H);
        ml_img_clip(&bim, x0, y0, x1, y1);
        switch (j->sc) {
        case SC_LEVEL: level_band(a, &bim, y0, y1); break;
        case SC_CASITA: mlc_band(a, &bim, y0, y1); break;
        case SC_MAP: mlm_band(a, &bim, y0, y1); break;
        default: ml_rect(&bim, x0, y0, x1 - x0, y1 - y0, 0); break;
        }
        size_t n = (size_t)(x1 - x0) * 2;
        if (own) {
            for (int y = y0; y < y1; y++)
                memcpy(j->fb + (size_t)y * ML_W + x0, own + (size_t)(y - y0) * ML_W + x0, n);
        }
        if (j->back) {
            /* the same band into the panel's free buffer: upright its rows
             * are the frame's (ML_W == AOS_PANEL_W); turned, the PPA turns
             * the band's box on the way, from internal RAM */
            const uint16_t *src = own ? own : j->fb + (size_t)y0 * ML_W;
            if (!j->turn) {
                for (int y = y0; y < y1; y++)
                    memcpy(j->back + (size_t)y * ML_W + x0, src + (size_t)(y - y0) * ML_W + x0, n);
            } else {
                aos_hal_display_blit_into_fit(j->back, x0, y0, x1 - x0, y1 - y0, src + x0, x1 - x0, y1 - y0, ML_W,
                                              false);
            }
        }
        px += (uint32_t)((x1 - x0) * (y1 - y0));
    }
    j->px[part] = px;
}

/* What a frame costs, logged every few seconds while a scene runs (the
 * portal's /api/log on the board): the frames drawn, the worker's time for
 * one (step, what changed, bands), the share of the screen redrawn, and the
 * push on the LVGL side */
static struct {
    uint64_t t0, work_us, blit_us;
    uint32_t frames, idle, blits, rects;
    uint32_t unpacks;       /* packed frames unpacked (the casita) */
    uint64_t px;
    /* how the frames went: flipped, pushed into the buffer on screen, or
     * through LVGL's canvas (under a toast); and the whole frames flipped
     * because LVGL had drawn into the buffer on screen */
    uint32_t flips, pushes, lvgl, retouch;
} s_perf;

static void perf_log(app_t *a)
{
    uint64_t now = aos_hal_uptime_us();
    if (!s_perf.t0) {
        memset(&s_perf, 0, sizeof s_perf);
        s_perf.t0 = now;
        return;
    }
    if (now - s_perf.t0 < PERF_MS * 1000ull) return;
    uint32_t ms = (uint32_t)((now - s_perf.t0) / 1000);
    unsigned fps10 = ms ? (unsigned)((uint64_t)s_perf.frames * 10000u / ms) : 0;
    unsigned pct = s_perf.frames ? (unsigned)(s_perf.px * 100 / ((uint64_t)s_perf.frames * ML_W * ML_H)) : 0;
    static const char *const nm[] = { "-", "level", "casita", "map" };
    int sc = a->scene >= 0 && a->scene <= SC_MAP ? a->scene : 0;
    char more[48] = "";
    if (sc == SC_CASITA)
        snprintf(more, sizeof more, ", %u unpacks, %u KB unpacked", (unsigned)s_perf.unpacks,
                 (unsigned)(ml_zbytes() / 1024));
    aos_hal_log("mila", "%s: %u.%u fps %dx%d, render %u ms, %u%% redrawn, push %u us (%u rects), %u idle steps%s"
                " | %u flipped, %u pushed, %u via LVGL, %u whole after LVGL",
                nm[sc], fps10 / 10, fps10 % 10, ML_W, ML_H,
                (unsigned)(s_perf.frames ? s_perf.work_us / s_perf.frames / 1000 : 0), pct,
                (unsigned)(s_perf.blits ? s_perf.blit_us / s_perf.blits : 0),
                (unsigned)(s_perf.blits ? s_perf.rects / s_perf.blits : 0), (unsigned)s_perf.idle, more,
                (unsigned)s_perf.flips, (unsigned)s_perf.pushes, (unsigned)s_perf.lvgl, (unsigned)s_perf.retouch);
    memset(&s_perf, 0, sizeof s_perf);
    s_perf.t0 = now;
}

/* ---- page flipping's record (ml_app.h) ---- */

static int pb_slot(app_t *a, const uint16_t *b)
{
    for (int s = 0; s < 3; s++)
        if (a->pb[s] == b) return s;
    for (int s = 0; s < 3; s++) {
        if (!a->pb[s]) {
            a->pb[s] = (uint16_t *)b;
            a->pb_at[s] = 0;
            return s;
        }
    }
    return -1;
}

/* What the panel's buffer 'b' lacks of the frame being drawn, besides what
 * this frame changed, into R: the frame of our flip that last showed it,
 * so what every flip since changed, and the frames since the last flip that
 * went another way. Everything when that is not known: never shown by us,
 * LVGL drew into it since (aos_hal_display_back_age), someone else flipped
 * meanwhile, or older than the ring. Worker, while its frame is FREE. */
static void back_region(app_t *a, uint16_t *b, ml_dmg_t *R)
{
    int s = pb_slot(a, b);
    uint32_t since = 0;
    bool touched = true;
    bool known = s >= 0 && a->pb_at[s] && aos_hal_display_back_age(b, &since, &touched) && !touched &&
                 since != UINT32_MAX && since == a->flips - a->pb_at[s] && since <= ML_FLIP_RING;
    if (!known) {
        ml_dmg_full(R);
        return;
    }
    ml_dmg_or(R, &a->flip_acc);
    for (uint32_t f = a->pb_at[s] + 1; f <= a->flips; f++) ml_dmg_or(R, &a->flip_dmg[f % ML_FLIP_RING]);
}

/* The timer flipped to 'b' a frame that changed D since the frame before */
static void flip_done(app_t *a, uint16_t *b, const ml_dmg_t *D)
{
    a->flips++;
    ml_dmg_t e = a->flip_acc;
    ml_dmg_or(&e, D);
    a->flip_dmg[a->flips % ML_FLIP_RING] = e;
    ml_dmg_clear(&a->flip_acc);
    int s = pb_slot(a, b);
    if (s >= 0) a->pb_at[s] = a->flips;
    a->front = b;
}

/* A frame that changed D went another way (pushed, through LVGL) or
 * nowhere: the next flip carries its changes, and the panel buffer it was
 * drawn into, if any, holds a frame no flip of ours names */
static void flip_drop(app_t *a, int i)
{
    if (!a->flip) return;
    ml_dmg_or(&a->flip_acc, &a->dmg[i]);
    if (a->back[i]) {
        int s = pb_slot(a, a->back[i]);
        if (s >= 0) a->pb_at[s] = 0;
        a->back[i] = NULL;
    }
}

/* ... and what goes into the buffer on screen other than by a flip (a
 * push, LVGL's canvas) leaves it holding no flip's frame either */
static void flip_front_dirty(app_t *a)
{
    if (!a->flip || !a->front) return;
    int s = pb_slot(a, a->front);
    if (s >= 0) a->pb_at[s] = 0;
}

static uint64_t s_frame_us;         /* when the last frame was drawn */

static void play_frame(app_t *a)
{
    uint64_t t0_us = aos_hal_uptime_us();
    if (t0_us - s_frame_us < FRAME_MIN_US) {
        uint32_t ms = (uint32_t)((FRAME_MIN_US - (t0_us - s_frame_us)) / 1000);
        aos_hal_worker_sleep(ms ? ms : 1);
        s_last_yield = aos_hal_uptime_ms();
        return;
    }
    int i = free_fb(a);
    if (i < 0) {
        aos_hal_worker_sleep(4);
        s_last_yield = aos_hal_uptime_ms();
        return;
    }
    uint64_t now = aos_hal_uptime_ms();
    float dt = a->w_last_ms ? (float)(uint32_t)(now - a->w_last_ms) / 1000.0f : 0.033f;
    if (dt > 0.1f) dt = 0.1f;
    a->w_last_ms = now;
    int sc = a->scene;
    ml_dmg_t D;
    ml_dmg_clear(&D);
    if (sc == SC_LEVEL && a->level_ok) {
        level_frame(a, dt, &D);
    } else if (sc == SC_CASITA) {
        uint32_t u0 = ml_zunpacks();
        mlc_step(a, dt);
        s_perf.unpacks += ml_zunpacks() - u0;
        mlc_damage(a, &D);
    } else if (sc == SC_MAP) {
        mlm_step(a, dt);
        mlm_damage(a, &D);
    }
    if (a->want_full) {
        a->want_full = false;
        ml_dmg_full(&D);
    }
    /* Page flipping: the panel's free buffer, unless the timer would not
     * flip now (a panel of ours, the loader, something of the system's).
     * LVGL drew into the buffer on screen since our flip (a panel closing,
     * the loader going, a toast): what it drew there is the canvas's
     * picture, maybe caught halfway through a frame of ours, so a whole
     * frame goes up over it. */
    uint16_t *back = a->flip && a->flip_hint ? aos_hal_display_back() : NULL;
    bool retouch = false;
    if (back && a->front) {
        bool t = false;
        retouch = aos_hal_display_back_age(a->front, NULL, &t) && t;
    }
    if (ml_dmg_empty(&D) && !retouch) {
        /* nothing changed: nothing to draw nor to push */
        s_perf.idle++;
        aos_hal_worker_sleep(IDLE_MS);
        s_last_yield = aos_hal_uptime_ms();
        return;
    }
    a->fb_state[i] = FB_BUSY;
    s_frame_us = t0_us;
    /* this buffer lacks what changed since it was drawn, and this frame;
     * the panel's, what the flips since its own changed (back_region) */
    ml_dmg_t R = a->pend[i];
    ml_dmg_or(&R, &D);
    if (back) back_region(a, back, &R);
    if (retouch) {
        ml_dmg_full(&R);
        s_perf.retouch++;
    }
    band_job_t j = { .a = a, .fb = a->fb[i], .back = back, .turn = a->flip_turn, .band = { a->band, a->band2 },
                     .r = &R, .sc = sc };
    if (!a->band || !a->band2 || !aos_hal_worker_split(band_half, &j)) {
        j.parts = 1;
        j.band[1] = NULL;
        band_half(&j, 0);
    }
    for (int k = 0; k < ML_NFB; k++) {
        if (k == i) ml_dmg_clear(&a->pend[k]);
        else ml_dmg_or(&a->pend[k], &D);
    }
    a->dmg[i] = D;
    a->back[i] = back;
    s_perf.work_us += aos_hal_uptime_us() - t0_us;
    s_perf.frames++;
    s_perf.px += j.px[0] + j.px[1];
    a->w_frames++;
    a->fb_seq[i] = ++a->seq;
    a->fb_state[i] = FB_READY;
    worker_yield();
}

static void worker_fn(void *arg)
{
    app_t *a = (app_t *)arg;
    ml_set_clock(clock_ms);
    ml_set_yield(worker_yield);
    while (!aos_hal_worker_should_stop()) {
        if (a->job) {
            s_last_yield = aos_hal_uptime_ms();
            run_job(a, a->job);
            a->job = JOB_NONE;
            a->job_done = true;
            continue;
        }
        /* under the system's panels, the switcher, the gesture home, the
         * zoom from the icon or a banner the scene waits, as under our own
         * panels: nothing may go up over them (aos_ui.h) */
        if (a->scene != SC_NONE && !a->fitting && !a->over_wait && a->state != ST_PAUSE && a->state != ST_RESULT &&
            a->state != ST_SHOP && a->state != ST_SETTINGS && a->state != ST_LOADING && a->state != ST_LOBBY) {
            play_frame(a);
            continue;
        }
        if (a->want_frame && a->scene != SC_NONE && !a->fitting && free_fb(a) >= 0) {
            /* the scene as it stands, for the canvas under a panel: no step */
            a->want_frame = false;
            int i = free_fb(a);
            a->fb_state[i] = FB_BUSY;
            ml_dmg_t R;
            ml_dmg_clear(&R);
            ml_dmg_full(&R);
            band_job_t j = { .a = a, .fb = a->fb[i], .band = { a->band, a->band2 }, .r = &R, .sc = a->scene };
            if (a->scene == SC_LEVEL && a->level_ok) {
                s_fr.mode = a->lmode == LM_PEEK && !a->ov ? LM_PLAY : a->lmode;
                if (s_fr.mode != LM_OVERVIEW) ml_world_prepare(&a->w, a->play.icam_x, a->play.icam_y, 0, 0, 0, NULL);
            }
            if (!a->band || !a->band2 || !aos_hal_worker_split(band_half, &j)) {
                j.parts = 1;
                j.band[1] = NULL;
                band_half(&j, 0);
            }
            for (int k = 0; k < ML_NFB; k++) ml_dmg_full(&a->pend[k]);
            ml_dmg_clear(&a->pend[i]);
            ml_dmg_full(&a->dmg[i]);
            a->back[i] = NULL;
            a->fb_seq[i] = ++a->seq;
            a->fb_state[i] = FB_READY;
            continue;
        }
        a->w_last_ms = 0;
        aos_hal_worker_sleep(20);
    }
    ml_set_yield(NULL);
}

static int s_last_job;

static bool job(app_t *a, int j)
{
    if (a->job != JOB_NONE) return false;
    a->job_done = false;
    a->job_ok = false;
    s_last_job = j;
    a->job = j;
    return true;
}

/* --------------------------------------------------------------------------
 * Frames to the panel
 * -------------------------------------------------------------------------- */

/* The canvas shows a frame under the panels (in the simulator every frame:
 * it has no panel to blit to). It shows the frame buffer itself, which is in
 * LVGL's order already: the frame on the screen is FB_SHOWN, and the worker
 * leaves it alone until the next one replaces it. */
static void canvas_show(app_t *a, int i)
{
    if (!a->canvas || i < 0 || !a->fb[i] || a->fitting || a->want_fit) return;
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_buffer(a->canvas, a->fb[i], a->fw, a->fh, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(a->canvas, a->fw, a->fh);
    lv_obj_invalidate(a->canvas);
    a->canvas_refr = a->refr_n;
    a->canvas_wait = true;
}

/* LVGL finished a refresh: a frame the canvas showed has been read */
static void disp_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_REFR_READY) a->refr_n++;
}

/* With one frame, the one on the screen goes back to the worker once the
 * panel has it (the blit returned) and LVGL is not about to read it for
 * the canvas. LVGL thread. */
static void release_shown(app_t *a)
{
    if (a->nfb != 1 || a->shown < 0 || a->fb_state[a->shown] != FB_SHOWN) return;
    if (a->canvas_wait && a->refr_n == a->canvas_refr) return;
    a->canvas_wait = false;
    a->fb_state[a->shown] = FB_FREE;
}

/* What changed, from the frame buffer to the panel. Upright, runs of whole
 * rows: the DMA2D copy LVGL's own flush uses. Lying down, the PPA turns each
 * run's rectangle on the way (a whole frame is 62 ms there; a kitten
 * walking a cell is a few). */
static bool push_region(app_t *a, int i, const ml_dmg_t *r, int *rects)
{
    const uint16_t *fb = a->fb[i];
    int fw = a->fw, fh = a->fh;
    bool wide = fw > fh;
    *rects = 0;
    if (r->full) {
        *rects = 1;
        return aos_hal_display_blit_scaled(0, 0, fw, fh, fb, 1, false);
    }
    int th = (fh + ML_TILE - 1) / ML_TILE;
    for (int ty = 0; ty < th;) {
        if (!r->row[ty]) {
            ty++;
            continue;
        }
        /* a run: rows that changed, and gaps of a few rows between them
         * (one DMA2D call instead of two is worth a few rows more; the
         * PPA's are not, lying down) */
        int gap = wide ? 1 : 3;
        uint64_t m = 0;
        int t0 = ty, t1 = ty;
        while (ty < th && ty - t1 <= gap) {
            if (r->row[ty]) {
                m |= r->row[ty];
                t1 = ty;
            }
            ty++;
        }
        ty = t1 + 1;
        int y = t0 * ML_TILE, h = (ty * ML_TILE > fh ? fh : ty * ML_TILE) - y;
        bool ok;
        if (!wide) {
            ok = aos_hal_display_blit_scaled(0, y, fw, h, fb + (size_t)y * fw, 1, false);
        } else {
            int x0 = ml_bit_lo(m) * ML_TILE, x1 = (ml_bit_hi(m) + 1) * ML_TILE;
            if (x1 > fw) x1 = fw;
            ok = aos_hal_display_blit_fit(x0, y, x1 - x0, h, fb + (size_t)y * fw + x0, x1 - x0, h, fw, false);
        }
        if (!ok) return false;
        (*rects)++;
    }
    return true;
}

static bool worker_scene_state(int st)
{
    return st == ST_CASITA || st == ST_MAP || st == ST_LEVEL;
}

/* The timer would flip now: page flipping, a scene on, nothing over it,
 * neither ours (the loader; the panels are other states) nor the system's,
 * not even a toast (the flip would hide it until it fades). LVGL thread. */
static bool may_flip(const app_t *a)
{
    return a->flip && worker_scene_state(a->state) && !ml_ui_loader_on() && !a->over && !a->fitting &&
           !a->want_fit && !a->closing;
}

/* The newest READY frame, or -1 */
static int newest_ready(app_t *a)
{
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < ML_NFB; i++) {
        if (a->fb_state[i] == FB_READY && (best < 0 || a->fb_seq[i] > bs)) {
            best = i;
            bs = a->fb_seq[i];
        }
    }
    return best;
}

/* The newest frame to the panel. Page flipping: the worker drew it into
 * the panel's free buffer too, and a flip shows it whole at the next
 * refresh (no tearing, no copy); our frame goes straight back to the
 * worker, and stays the canvas's picture. Otherwise, or when the flip is
 * not taken, its changes go into the buffer on screen (push_region), or,
 * under a toast (which that would paint over) or where there is no blit,
 * through the canvas. Under the system's panels nothing goes up: the frame
 * waits (and so does the worker). A scene's first frame, drawn while the
 * loader was up, has no panel buffer (flip_hint was off): it is pushed. */
static void push_frame(app_t *a)
{
    if (a->fitting || a->want_fit) return;
    int best = newest_ready(a);
    if (best < 0) return;
    if (a->over_wait) return;
    /* frames the panel never got: what they changed goes with this one */
    for (int i = 0; i < ML_NFB; i++) {
        if (i != best && a->fb_state[i] == FB_READY) {
            ml_dmg_or(&a->panel_acc, &a->dmg[i]);
            flip_drop(a, i);
            a->fb_state[i] = FB_FREE;
        }
    }
    uint64_t t0 = aos_hal_uptime_us();
    if (a->flip) {
        uint16_t *b = a->back[best];
        if (b && may_flip(a) && aos_hal_display_flip(b)) {
            a->back[best] = NULL;
            flip_done(a, b, &a->dmg[best]);
            /* the buffer on screen is whole now: a push after this pushes all */
            ml_dmg_full(&a->panel_acc);
            s_perf.blit_us += aos_hal_uptime_us() - t0;
            s_perf.blits++;
            s_perf.rects++;
            s_perf.flips++;
            a->fb_state[best] = FB_FREE;
            a->shown = best;
            /* after a turn the canvas is hidden: it shows the frame again,
             * for what LVGL composes over the scene */
            if (a->canvas && lv_obj_has_flag(a->canvas, LV_OBJ_FLAG_HIDDEN)) canvas_show(a, best);
            return;
        }
        flip_drop(a, best);
        flip_front_dirty(a);
    }
    ml_dmg_t r = a->panel_acc;
    ml_dmg_or(&r, &a->dmg[best]);
    ml_dmg_clear(&a->panel_acc);
    int rects = 0;
    if ((a->over & AOS_UI_OVER_TOAST) || !push_region(a, best, &r, &rects)) {
        canvas_show(a, best);
        s_perf.lvgl++;
        /* the buffer on screen gets what LVGL composes; after the toast a
         * push must be whole */
        if (a->over & AOS_UI_OVER_TOAST) ml_dmg_full(&a->panel_acc);
    } else {
        s_perf.pushes++;
    }
    s_perf.blit_us += aos_hal_uptime_us() - t0;
    s_perf.blits++;
    s_perf.rects += (uint32_t)rects;
    if (a->shown >= 0 && a->shown != best) a->fb_state[a->shown] = FB_FREE;
    a->fb_state[best] = FB_SHOWN;
    a->shown = best;
    release_shown(a);
}

/* the newest frame, to the canvas only (under the panels a blit would
 * paint over them on the board) */
static void take_frame(app_t *a)
{
    if (a->fitting || a->want_fit) return;
    int best = newest_ready(a);
    if (best < 0) return;
    for (int i = 0; i < ML_NFB; i++) {
        if (i != best && a->fb_state[i] == FB_READY) {
            flip_drop(a, i);
            a->fb_state[i] = FB_FREE;
        }
    }
    flip_drop(a, best);
    if (a->shown >= 0 && a->shown != best) a->fb_state[a->shown] = FB_FREE;
    a->fb_state[best] = FB_SHOWN;
    a->shown = best;
    ml_dmg_full(&a->panel_acc);         /* the panel has not got it */
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    canvas_show(a, best);
}

/* ---- words into masks (LVGL thread) ---- */

bool mla_text_mask(ml_mask_t *m, const char *txt, const lv_font_t *font)
{
    lv_point_t sz;
    free(m->a);
    memset(m, 0, sizeof(*m));
    lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    int w = sz.x + 2, h = sz.y;
    if (w <= 2 || h <= 0) return false;
    lv_draw_buf_t *db = lv_draw_buf_create((uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_L8, 0);
    if (!db) return false;
    lv_obj_t *c = lv_canvas_create(lv_layer_top());
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(c, db);
    lv_canvas_fill_bg(c, lv_color_hex(0x000000), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(c, &layer);
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.color = lv_color_hex(0xFFFFFF);
    d.font = font;
    d.text = txt;
    lv_area_t area = { 1, 0, w - 1, h - 1 };
    lv_draw_label(&layer, &d, &area);
    lv_canvas_finish_layer(c, &layer);
    m->a = (uint8_t *)ml_malloc((size_t)w * h);
    if (m->a) {
        m->w = (int16_t)w;
        m->h = (int16_t)h;
        for (int y = 0; y < h; y++) memcpy(m->a + (size_t)y * w, db->data + (size_t)y * db->header.stride, (size_t)w);
    }
    lv_obj_delete(c);
    lv_draw_buf_destroy(db);
    return m->a != NULL;
}

static void hud_build(app_t *a)
{
    static const char digits[] = "0123456789:/";
    char s[2] = { 0, 0 };
    for (int i = 0; i < 12; i++) {
        s[0] = digits[i];
        mla_text_mask(&a->hud.dig[i], s, aos_font_title);
        mla_text_mask(&a->hud.sdig[i], s, aos_font_body);
    }
    mla_text_mask(&a->hud.msg[MSG_TAP], _("Tocá para empezar"), aos_font_body);
    mla_text_mask(&a->hud.msg[MSG_PAR], _("Par"), aos_font_body);
    mla_text_mask(&a->hud.msg[MSG_WELL], _("¡Muy bien!"), aos_font_large);
    mla_text_mask(&a->hud.sym[SYM_PAUSE], LV_SYMBOL_PAUSE, aos_font_body);
    mla_text_mask(&a->hud.sym[SYM_UNDO], LV_SYMBOL_PREV, aos_font_title);
    mla_text_mask(&a->hud.sym[SYM_RESTART], LV_SYMBOL_REFRESH, aos_font_title);
    mla_text_mask(&a->hud.sym[SYM_PLAY], LV_SYMBOL_PLAY, aos_font_title);
    mla_text_mask(&a->hud.sym[SYM_HOME], LV_SYMBOL_HOME, aos_font_title);
    mla_text_mask(&a->hud.sym[SYM_SHOP], LV_SYMBOL_TINT, aos_font_title);
    mla_text_mask(&a->hud.sym[SYM_GEAR], LV_SYMBOL_SETTINGS, aos_font_title);
    mla_text_mask(&a->hud.sym[SYM_FRIEND], LV_SYMBOL_WIFI, aos_font_title);
    mla_text_mask(&a->hud.sym[SYM_LOCK], LV_SYMBOL_EYE_CLOSE, aos_font_body);
    mla_text_mask(&a->hud.msg[MSG_PLAY], _("Jugar"), aos_font_small);
    mla_text_mask(&a->hud.msg[MSG_SHOP], _("Tienda"), aos_font_small);
    mla_text_mask(&a->hud.msg[MSG_SETTINGS], _("Ajustes"), aos_font_small);
    mla_text_mask(&a->hud.msg[MSG_FRIEND], _("Amigo"), aos_font_small);
    mla_text_mask(&a->hud.msg[MSG_SOON], _("Próximamente"), aos_font_body);
}

/* --------------------------------------------------------------------------
 * States
 * -------------------------------------------------------------------------- */

/* ---- the loader's bar ---- */

static void load_begin(app_t *a, const char *key, uint32_t guess)
{
    int32_t v = 0;
    snprintf(a->ld_key, sizeof a->ld_key, "%s", key);
    a->ld_from = ml_art_read();
    a->ld_est = aos_hal_pref_get_i32(key, &v) && v > 0 ? (uint32_t)v : guess;
    if (a->ld_est < 1) a->ld_est = 1;
}

static int load_pct(const app_t *a)
{
    return (int)((uint64_t)(ml_art_read() - a->ld_from) * 100 / a->ld_est);
}

static void load_end(app_t *a)
{
    if (!a->ld_key[0]) return;
    /* what it really read is next time's measure */
    uint32_t used = ml_art_read() - a->ld_from;
    uint32_t d = used > a->ld_est ? used - a->ld_est : a->ld_est - used;
    if (used > 0 && d * 20 > a->ld_est) aos_hal_pref_set_i32(a->ld_key, (int32_t)used);
    a->ld_key[0] = 0;
}

static int scene_of(int st)
{
    return st == ST_CASITA ? SC_CASITA : st == ST_MAP ? SC_MAP : st == ST_LEVEL ? SC_LEVEL : SC_NONE;
}

static void music_for(app_t *a, int st)
{
    switch (st) {
    case ST_CASITA: case ST_SHOP: case ST_SETTINGS: case ST_LOBBY: ml_music(MUS_CASITA); break;
    case ST_MAP: ml_music(MUS_MAP); break;
    case ST_LEVEL: case ST_PAUSE: {
        int w = a->job_world >= 0 && a->job_world < a->worlds.nworlds ? a->worlds.w[a->job_world].music : 0;
        ml_music(w >= 0 && w < 5 ? w : 0);
        break;
    }
    default: ml_music(MUS_NONE); break;
    }
}

void mla_set_state(app_t *a, int st)
{
    int prev = a->state;
    a->state = st;
    a->st_ms = 0;
    if (!worker_scene_state(st) && a->shown >= 0) canvas_show(a, a->shown);
    /* back from a panel (or into a new scene): LVGL drew over the screen,
     * the panel gets a whole frame again */
    if (worker_scene_state(st) && st != prev) {
        a->want_full = true;
        ml_dmg_full(&a->panel_acc);
    }
    if (st == ST_CASITA && prev != ST_CASITA && a->scene != SC_CASITA) {
        /* the casita keeps one frame: the canvas lets go of the second
         * before the worker frees it (the loader covers the screen) */
        if (a->canvas && a->fb[0]) {
            lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
            lv_canvas_set_buffer(a->canvas, a->fb[0], a->fw, a->fh, LV_COLOR_FORMAT_RGB565);
        }
        if (!job(a, JOB_CASITA)) a->pending_job = JOB_CASITA;
        ml_ui_loading_text(a, "");
        ml_ui_loader(a, true, 0);
        load_begin(a, "ml_ldc", 5400u * 1024u);
    }
    if (st == ST_MAP && prev != ST_MAP && a->scene != SC_MAP) {
        if (!job(a, JOB_MAP)) a->pending_job = JOB_MAP;
        ml_ui_loading_text(a, "");
        ml_ui_loader(a, true, 0);
        load_begin(a, "ml_ldm", 4600u * 1024u);
    }
    music_for(a, st);
    ml_ui_show(a, st);
    /* at once, not at the next tick: the worker would otherwise draw one
     * more frame for a flip over the loader or the panel just asked for */
    a->flip_hint = may_flip(a);
}

void mla_level_start(app_t *a, int wi, int li)
{
    char nm[64], wn[48];
    a->job_world = wi;
    a->job_level = li;
    mla_level_name(a, wi, li, nm, sizeof nm);
    mla_world_name(a, wi, wn, sizeof wn);
    char sub[96];
    snprintf(sub, sizeof sub, "%.40s  %d-%d", wn, wi + 1, li + 1);
    mla_text_mask(&a->hud.title, nm, aos_font_title);
    mla_text_mask(&a->hud.sub, sub, aos_font_body);
    ml_ui_loading_text(a, nm);
    snprintf(a->prog.last, sizeof a->prog.last, "%s %d", a->worlds.w[wi].id, li);
    load_begin(a, "ml_ldl", 2000u * 1024u);
    mla_set_state(a, ST_LOADING);
    if (!job(a, JOB_LEVEL)) a->pending_job = JOB_LEVEL;
}

void mla_level_leave(app_t *a)
{
    mla_set_state(a, ST_MAP);
}

void mla_resume(app_t *a)
{
    if (a->state == ST_PAUSE) mla_set_state(a, ST_LEVEL);
}

static int s_ui_pending;

void mla_ui_job(app_t *a, int what)
{
    if (a->job == JOB_NONE && !a->pending_job) {
        a->ui_job = what;
        job(a, JOB_UI);
    } else {
        s_ui_pending = what;
    }
}

void mla_outfit(app_t *a)
{
    if (!job(a, JOB_OUTFIT)) a->pending_job = JOB_OUTFIT;
}

/* ---- results ---- */

static void result_show(app_t *a)
{
    ml_play_t *p = &a->play;
    ml_prog_t *pr = &a->prog;
    int wi = a->world, li = a->level;
    int stars = ml_play_stars(p);
    int before = pr->stars[wi][li];
    int earned = 0;
    if (!before) earned += 20;
    if (stars > before) earned += 10 * (stars - before);
    if (stars > before) pr->stars[wi][li] = (uint8_t)stars;
    pr->coins += earned;
    pr->stat[SX_MOVES] += p->moves;
    pr->stat[SX_PUSHES] += p->pushes;
    if (!before) pr->stat[SX_SOLVED]++;
    /* the world's gift, the first time every level is solved */
    const char *gift = NULL;
    const ml_world_info_t *wd = &a->worlds.w[wi];
    bool all = true;
    for (int k = 0; k < wd->nlevels; k++)
        if (!pr->stars[wi][k]) all = false;
    if (all && wd->gift[0] && !ml_prog_owns(pr, wd->gift)) {
        ml_prog_add(pr, wd->gift);
        gift = wd->gift;
    }
    mla_save(a);
    ml_ui_result_fill(a, stars, earned, p->moves, a->lv.par, gift);
    mla_set_state(a, ST_RESULT);
    uint32_t ms = (uint32_t)(aos_hal_uptime_ms() - a->w_fps_t0);
    unsigned fps10 = ms ? (unsigned)((uint64_t)a->w_frames * 10000u / ms) : 0;
    aos_hal_log("mila", "level %s won in %d moves (par %d), %d stars | %u.%u frames/s drawn", wd->lv[li].id, p->moves,
                a->lv.par, stars, fps10 / 10, fps10 % 10);
}

static void race_result(app_t *a)
{
    ml_play_t *p = &a->play;
    bool won = p->won && !a->race_lost;
    int earned = won ? 40 : 15;
    a->prog.coins += earned;
    a->prog.stat[SX_MOVES] += p->moves;
    mla_save(a);
    ml_ui_race_fill(a, won, p->moves, a->rival_moves, earned);
    mla_set_state(a, ST_RESULT);
    aos_hal_log("mila", "race on %s: %s, %d moves against %d", a->worlds.w[a->world].lv[a->level].id,
                won ? "won" : "lost", p->moves, a->rival_moves);
}

/* ---- input ---- */

static app_t *app_of(lv_event_t *e)
{
    return (app_t *)lv_event_get_user_data(e);
}

static int swipe_dir(int dx, int dy)
{
    if (abs(dx) > abs(dy)) return dx > 0 ? D_RIGHT : D_LEFT;
    return dy > 0 ? D_DOWN : D_UP;
}

static bool in_circle(int x, int y, int cx, int cy, int r)
{
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
}

/* is the point on Mila (her cell, a little generous) */
static bool on_mila(app_t *a, int x, int y)
{
    float gx, gy;
    ml_play_mila_pos(&a->play, &gx, &gy);
    float sx = ml_lpx(&a->w, gx) - a->play.icam_x, sy = ml_lpy(&a->w, gy, 0.25f) - a->play.icam_y;
    return fabsf(x - sx) < 70 && fabsf(y - sy) < 70;
}

static void level_touch(app_t *a, lv_event_code_t code, lv_point_t p)
{
    if (a->lmode == LM_OVERVIEW) {
        if (code == LV_EVENT_RELEASED && a->mode_t > 0.25f) {
            a->mode_t = 0;
            a->lmode = LM_ZOOM;
            a->w_fps_t0 = aos_hal_uptime_ms();
            a->w_frames = 0;
            ml_snd(SND_ZOOM);
        }
        return;
    }
    if (a->lmode != LM_PLAY && a->lmode != LM_PEEK) return;
    if (a->pinching) return;            /* two fingers: pinch_cb has it */
    if (code == LV_EVENT_PRESSED) {
        a->p0 = p;
        a->pressed = true;
        a->dragged = false;
        a->held_mila = on_mila(a, p.x, p.y);
        a->press_ms = lv_tick_get();
        a->held_dir = -1;
        return;
    }
    if (!a->pressed) return;
    if (code == LV_EVENT_PRESSING) {
        int dx = p.x - a->p0.x, dy = p.y - a->p0.y;
        if (!a->dragged && dx * dx + dy * dy >= SWIPE_PX * SWIPE_PX) {
            a->dragged = true;
            a->held_dir = swipe_dir(dx, dy);
            a->repeating = false;
            ml_play_push_dir(&a->play, a->held_dir);
            a->last_step_ms = lv_tick_get();
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        a->pressed = false;
        a->held_dir = -1;
        if (a->lmode == LM_PEEK) {
            a->lmode = LM_PLAY;
            a->peek_ready = false;
            return;
        }
        if (a->dragged || code == LV_EVENT_PRESS_LOST) return;
        int x = a->p0.x, y = a->p0.y;
        if (in_circle(x, y, HUD_PAUSE_X, HUD_PAUSE_Y, HUD_PAUSE_R + 18)) {
            a->want_pause = true;
        } else if (in_circle(x, y, HUD_UNDO_X, HUD_UNDO_Y, HUD_BTN_R + 14)) {
            ml_play_undo(&a->play);
            a->hud_flash_undo = 0.15f;
            a->prog.stat[SX_UNDOS]++;
        } else if (in_circle(x, y, HUD_RESTART_X, HUD_RESTART_Y, HUD_BTN_R + 14)) {
            ml_play_restart(&a->play);
            a->hud_flash_restart = 0.15f;
        } else {
            int c = ml_play_cell_at(&a->play, x, y);
            if (c >= 0) ml_play_walk_to(&a->play, c);
        }
    }
}

static void touch_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    if (a->closing) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_event_code_t code = lv_event_get_code(e);
    switch (a->state) {
    case ST_LEVEL:
        if (a->level_ok) level_touch(a, code, p);
        break;
    case ST_CASITA:
        mlc_touch(a, code, p.x, p.y);
        break;
    case ST_MAP:
        mlm_touch(a, code, p.x, p.y);
        break;
    default:
        break;
    }
}

/* Two fingers: pinching them together pulls the camera back to the whole
 * room -the same view a finger held on Mila gives, but it stays after the
 * fingers lift-; spreading them flies back down to her, and so does a tap.
 * The first finger of a pinch must not become a step: the pinch takes the
 * touch over as soon as the second finger is sure. */
#define PINCH_OUT       0.80f
#define PINCH_IN        1.25f

static void pinch_cb(const aos_gesture_event_t *ev, void *user)
{
    app_t *a = (app_t *)user;
    if (a->closing || a->state != ST_LEVEL || !a->level_ok) return;
    if (a->lmode != LM_PLAY && a->lmode != LM_PEEK) return;
    switch (ev->type) {
    case AOS_GESTURE_PINCH_BEGIN:
        a->pinching = true;
        a->pinch_acc = 1.0f;
        a->pressed = false;             /* whatever the first finger started */
        a->held_dir = -1;
        break;
    case AOS_GESTURE_PINCH:
        a->pinch_acc *= ev->scale;
        if (a->lmode == LM_PLAY && a->pinch_acc < PINCH_OUT) {
            a->lmode = LM_PEEK;
            a->peek_ready = false;
            if (!job(a, JOB_PEEK)) a->pending_job = JOB_PEEK;
            a->pinch_acc = 1.0f;
            ml_snd(SND_ZOOM);
        } else if (a->lmode == LM_PEEK && a->pinch_acc > PINCH_IN) {
            a->lmode = LM_PLAY;
            a->peek_ready = false;
            a->pinch_acc = 1.0f;
            ml_snd(SND_ZOOM);
        }
        break;
    case AOS_GESTURE_PINCH_END:
        a->pinching = false;
        break;
    default:
        break;
    }
}

/* held: a swipe that stays down repeats, a finger that stays on Mila peeks */
static void hold_tick(app_t *a)
{
    if (a->state != ST_LEVEL || !a->pressed) return;
    uint32_t now = lv_tick_get();
    uint32_t wait = a->repeating ? REPEAT_MS : REPEAT_FIRST_MS;
    if (a->dragged && a->held_dir >= 0 && now - a->last_step_ms >= wait && !ml_play_busy(&a->play)) {
        a->repeating = true;
        ml_play_push_dir(&a->play, a->held_dir);
        a->last_step_ms = now;
    }
    if (!a->dragged && a->held_mila && a->lmode == LM_PLAY && now - a->press_ms >= PEEK_MS) {
        a->lmode = LM_PEEK;
        a->peek_ready = false;
        if (!job(a, JOB_PEEK)) a->pending_job = JOB_PEEK;
    }
}

/* back, from a swipe right over a panel or the runtime */
static bool go_back(app_t *a)
{
    switch (a->state) {
    case ST_CASITA: case ST_BOOT:
        return false;
    case ST_LEVEL:
        if (a->race) {
            ml_link_end(a);
            mla_set_state(a, ST_CASITA);
            return true;
        }
        a->want_pause = true;
        return true;
    case ST_PAUSE:
        mla_resume(a);
        return true;
    case ST_LOADING:
        return true;
    case ST_MAP:
        mla_set_state(a, ST_CASITA);
        return true;
    case ST_RESULT:
        mla_set_state(a, ST_MAP);
        return true;
    case ST_LOBBY:
        ml_link_end(a);
        mla_set_state(a, ST_CASITA);
        return true;
    case ST_SHOP: case ST_SETTINGS:
        mla_set_state(a, ST_CASITA);
        return true;
    default:
        return true;
    }
}

static void gesture_cb(lv_event_t *e)
{
    app_t *a = app_of(e);
    lv_indev_t *indev = lv_indev_active();
    if (a->closing || !indev || worker_scene_state(a->state)) return;
    if (lv_indev_get_gesture_dir(indev) == LV_DIR_RIGHT) {
        if (!go_back(a)) a->want_exit = true;
    }
}

static void play_events(app_t *a)
{
    while (a->ev_r != a->ev_w) {
        uint32_t ev = a->ev_ring[a->ev_r & 15];
        a->ev_r++;
        static const struct { uint32_t ev; uint8_t snd; } map[] = {
            { PE_WIN, SND_WIN }, { PE_TARGET, SND_TARGET }, { PE_FALL, SND_FALL }, { PE_GATE, SND_GATE },
            { PE_GATE_SHUT, SND_GATE }, { PE_FLAP, SND_FLAP }, { PE_SLIDE, SND_SLIDE }, { PE_ROLL, SND_ROLL },
            { PE_PUSH, SND_PUSH }, { PE_UNDO, SND_UNDO }, { PE_RESTART, SND_UNDO }, { PE_BUMP, SND_BUMP },
            { PE_STEP, SND_STEP },
        };
        int played = 0;
        for (size_t k = 0; k < sizeof map / sizeof map[0] && played < 2; k++) {
            if (ev & map[k].ev) {
                ml_snd(map[k].snd);
                played++;
            }
        }
    }
}

/* ---- the timer ---- */

#ifdef AOS_SIM_BUILTIN
static bool s_sim_link;
#endif
static char s_dev_level[24];       /* mila_dev.txt "level=roofs,8": open it */

static bool open_named_level(app_t *a, const char *e)
{
    char id[24];
    int n = 1;
    const char *comma = strchr(e, ',');
    snprintf(id, sizeof id, "%.*s", comma ? (int)(comma - e) : (int)strlen(e), e);
    if (comma) n = atoi(comma + 1);
    int wi = ml_worlds_find(&a->worlds, id);
    if (wi < 0 && id[0] >= '0' && id[0] <= '9') wi = atoi(id);
    if (wi >= 0 && wi < a->worlds.nworlds && n >= 1 && n <= a->worlds.w[wi].nlevels) {
        mla_level_start(a, wi, n - 1);
        return true;
    }
    return false;
}

/* The link to another board rides on ESP-NOW, which the P4 does not have
 * (esp_hosted brings no ESP-NOW): asked once, and with no link the friend's
 * button, visits and races are simply not there. */
static void link_probe(app_t *a)
{
    bool was = aos_hal_link_running();
    bool started = !was && aos_hal_link_start();
    a->link_hw = was || started;
    if (started) aos_hal_link_stop();
    aos_hal_log("mila", "link %s", a->link_hw ? "available" : "not on this board: no visits nor races");
}

static void boot_done(app_t *a)
{
    if (!a->job_ok) {
        ml_ui_boot_text(a, _("Falta mila_p4.pak en la tarjeta"));
        return;
    }
    ml_ui_job_done(a, UJ_ICONS);
    ml_prog_load(&a->prog, &a->worlds);
    ml_audio_enable(a->prog.snd & 1, (a->prog.snd >> 1) & 1);
    for (int i = 0; i < a->worlds.nworlds; i++) {
        char nm[48];
        mla_text_mask(&a->wname[i], mla_world_name(a, i, nm, sizeof nm), aos_font_body);
        mla_text_mask(&a->wname_s[i], nm, aos_font_small);
    }
    link_probe(a);
#ifdef AOS_SIM_BUILTIN
    /* Development switches (getenv() is NULL on the board):
     *   ML_UNLOCK=1             every world and level open
     *   ML_COINS=n              coins for the shop
     *   ML_LEVEL=<world>,<n>    straight into that level (world id or index, n from 1)
     *   ML_SCREEN=map|shop|settings
     */
    const char *e;
    if ((e = getenv("ML_UNLOCK")) && e[0]) a->dev_unlock = true;
    if ((e = getenv("ML_COINS")) && e[0]) a->prog.coins = atoi(e);
    if ((e = getenv("ML_LEVEL")) && e[0] && open_named_level(a, e)) return;
    if ((e = getenv("ML_LINK")) && e[0] && a->link_hw) {
        /* two sims (AOS_SIM_LINK_PORT/_PARTNER): into the lobby as soon as
         * the link has its partner (the sim sets it on its first poll) */
        aos_hal_link_start();
        s_sim_link = true;
        mla_set_state(a, ST_CASITA);
        return;
    }
    if ((e = getenv("ML_SCREEN")) && e[0]) {
        if (!strcmp(e, "map")) {
            mla_set_state(a, ST_MAP);
            return;
        }
        if (!strcmp(e, "shop") || !strcmp(e, "settings")) {
            mla_set_state(a, ST_CASITA);
            mla_set_state(a, !strcmp(e, "shop") ? ST_SHOP : ST_SETTINGS);
            return;
        }
    }
#endif
    if (s_dev_level[0] && open_named_level(a, s_dev_level)) return;
    mla_set_state(a, ST_CASITA);
}

static void frame(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (a->want_exit) {
        a->want_exit = false;
        aos_ui_back();
        return;
    }
    /* What of the system's is over the app, read here in LVGL's thread:
     * under anything but a toast the scene waits and nothing goes up (a
     * push would paint over it, a flip hide it); under a toast the frames
     * go through LVGL, which draws it over them. When it goes, LVGL has
     * drawn the canvas where it was, and a whole frame goes up again. */
    uint32_t over = aos_ui_overlay();
    if (over != a->over) {
        aos_hal_log("mila", over ? "0x%02x over the app: %s" : "nothing over the app (was 0x%02x)%s",
                    (unsigned)(over ? over : a->over),
                    !over ? "" : over & ~(uint32_t)AOS_UI_OVER_TOAST ? "the scene waits" : "frames through LVGL");
        if (!over) {
            a->want_full = true;
            ml_dmg_full(&a->panel_acc);
        }
        a->over = over;
        a->over_wait = (over & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    }
    /* the screen turned: the worker makes the frames again between two of
     * them (or after the job it is on) */
    if (a->want_fit && a->job == JOB_NONE) {
        a->want_fit = false;
        a->fitting = true;
        job(a, JOB_FIT);
    }
    a->flip_hint = may_flip(a);
    release_shown(a);
    uint64_t now = aos_hal_uptime_ms();
    int dt = a->prev_ms ? (int)(uint32_t)(now - a->prev_ms) : TICK_MS;
    if (dt > 100) dt = 100;
    a->prev_ms = now;
    a->st_ms += (uint32_t)dt;
#ifdef AOS_SIM_BUILTIN
    if (s_sim_link && a->state == ST_CASITA && !a->link_on && ml_link_available()) {
        s_sim_link = false;
        ml_link_begin(a);
    }
#endif
    if (a->link_hw) ml_link_tick(a);
    play_events(a);
    ml_audio_tick();
    ml_ui_tick(a, dt);
    hold_tick(a);

    if (a->job_done && a->job == JOB_NONE) {
        a->job_done = false;
        int j = s_last_job;
        if (j == JOB_UI) {
            ml_ui_job_done(a, a->ui_job);
        } else if (j == JOB_FIT) {
            a->fitting = false;
            if (!a->job_ok) aos_ui_toast(_("No hay memoria para girar la pantalla"), 1800);
            /* under a panel: the worker draws the scene once in the new shape */
            if (!worker_scene_state(a->state) && a->scene != SC_NONE) a->want_frame = true;
        } else if (a->state == ST_BOOT) {
            boot_done(a);
        } else if (j == JOB_LEVEL) {
            if (a->job_ok) {
                mla_set_state(a, ST_LEVEL);
            } else {
                aos_ui_toast(_("No se pudo cargar el nivel"), 1800);
                mla_set_state(a, ST_MAP);
            }
        }
    }
    if (a->pending_job && a->job == JOB_NONE) {
        int j = a->pending_job;
        a->pending_job = 0;
        job(a, j);
    } else if (s_ui_pending && a->job == JOB_NONE) {
        int w = s_ui_pending;
        s_ui_pending = 0;
        a->ui_job = w;
        job(a, JOB_UI);
    }
    /* every few seconds in a level: where it is */
    static uint32_t s_rep_ms;
    if (a->state == ST_LEVEL && a->st_ms - s_rep_ms >= 10000) {
        aos_hal_log("mila", "level %s: mode %d, %d moves", a->level_ok ? a->worlds.w[a->world].lv[a->level].id : "-",
                    a->lmode, a->play.moves);
        s_rep_ms = a->st_ms;
    } else if (a->state != ST_LEVEL || a->st_ms < 100) {
        s_rep_ms = a->state == ST_LEVEL ? a->st_ms : 0;
    }
    /* the loader stays until the scene's first frame is ready, then that
     * frame goes to the panel and to the canvas under the LVGL panels */
    if (ml_ui_loader_on() && a->state != ST_BOOT) {
        int want = scene_of(a->state);
        bool ready = false;
        if (want == SC_NONE && a->state != ST_LOADING) {
            /* a panel was asked for meanwhile (the shop): it goes over */
            ml_ui_loader(a, false, 0);
            a->ld_key[0] = 0;
        }
        /* Frames the worker finished for the PREVIOUS scene are never shown
         * now: give their slots back (the watch, 2026-09-24: with every slot
         * taken the loader waited forever for the new scene's first frame) */
        for (int i = 0; i < ML_NFB; i++) {
            if (a->fb_state[i] == FB_READY && a->fb_seq[i] <= a->scene_seq) {
                flip_drop(a, i);
                a->fb_state[i] = FB_FREE;
            }
        }
        if (want != SC_NONE && a->scene == want)
            for (int i = 0; i < ML_NFB; i++)
                if (a->fb_state[i] == FB_READY && a->fb_seq[i] > a->scene_seq) ready = true;
#ifdef AOS_SIM_BUILTIN
        /* ML_LOADER=1: the loader stays up (to look at it: the sim loads in ms) */
        if (getenv("ML_LOADER")) ready = false;
#endif
        if (ready) {
            ml_dmg_full(&a->panel_acc);
            int ri = newest_ready(a);
            push_frame(a);
            /* the canvas under the loader's place: LVGL redraws it all now */
            canvas_show(a, a->shown >= 0 ? a->shown : ri);
            ml_ui_loader(a, false, 100);
            a->flip_hint = may_flip(a);
            load_end(a);
        } else if (ml_ui_loader_on()) {
            ml_ui_loader(a, true, load_pct(a));
            return;
        }
    }
    switch (a->state) {
    case ST_LEVEL:
        if (a->want_pause) {
            a->want_pause = false;
            mla_set_state(a, ST_PAUSE);
            break;
        }
        push_frame(a);
        if (a->race) {
            /* a race ends when someone solved it (a moment to see it) */
            bool mine = a->lmode == LM_WON && a->mode_t > 1.8f;
            if ((mine || (a->race_lost && a->st_ms > 0)) && !a->result_shown) {
                a->result_shown = true;
                race_result(a);
            }
            break;
        }
        if (a->lmode == LM_WON && a->mode_t > 1.8f && !a->result_shown) {
            a->result_shown = true;
            result_show(a);
        }
        break;
    case ST_CASITA:
    case ST_MAP:
        push_frame(a);
        break;
    default:
        a->want_pause = false;
        take_frame(a);
        break;
    }
    if (worker_scene_state(a->state)) perf_log(a);
    else s_perf.t0 = 0;
    a->flip_hint = may_flip(a);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    return a ? go_back(a) : false;
}

/* The screen turned. The game carries on in the new shape: a level being
 * played pauses (a race cannot: its clock is the other board's too), the
 * panels are laid out again at once, and the worker makes the frames and
 * the cache again when it can (JOB_FIT, frame()). */
static bool app_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a || a->closing) return false;
    if (a->state == ST_LEVEL && !a->race && a->lmode == LM_PLAY) mla_set_state(a, ST_PAUSE);
    a->want_fit = true;
    a->flip_hint = false;
    lv_obj_set_size(a->touch, lv_obj_get_width(root), lv_obj_get_height(root));
    /* its buffer is a frame of the old shape until the worker makes the new ones */
    lv_obj_add_flag(a->canvas, LV_OBJ_FLAG_HIDDEN);
    ml_ui_layout(a);
    return true;
}

static void app_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a && a->link_on) ml_link_end(a);
    if (a && a->state == ST_LEVEL) mla_set_state(a, ST_PAUSE);
}

static void free_all(app_t *a)
{
    level_free(a);
    mlc_close(a);
    mlm_close(a);
    ml_mila_free(&a->mila);
    ml_ui_free(a);
    for (int i = 0; i < ML_NFB; i++) {
        aos_hal_io_free(a->fb[i]);
        a->fb[i] = NULL;
    }
    free(a->band);
    free(a->band2);
    a->band = a->band2 = NULL;
    ml_hud_free(&a->hud);
    for (int i = 0; i < ML_MAX_WORLDS; i++) {
        free(a->wname[i].a);
        free(a->wname_s[i].a);
    }
    ml_worlds_free(&a->worlds);
    ml_art_close();
}

static void *ml_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)ml_calloc(1, sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    uint32_t hi = 0, hp = 0;
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("mila", "opening | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);
    if (!frames_fit(a, 2)) {
        aos_hal_log("mila", "out of memory");
        free_all(a);
        free(a);
        return NULL;
    }
    a->world = a->level = -1;
    a->prev_mode = -1;
    a->root = root;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    a->canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(a->canvas, a->fb[0], a->fw, a->fh, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);

    a->touch = lv_obj_create(root);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, AOS_SCREEN_W, AOS_SCREEN_H);
    lv_obj_add_flag(a->touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESSING, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_RELEASED, a);
    lv_obj_add_event_cb(a->touch, touch_cb, LV_EVENT_PRESS_LOST, a);
    aos_gesture_attach(a->touch, 0, pinch_cb, a);

    hud_build(a);
    ml_ui_build(a, root);

    lv_obj_remove_flag(root, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(root, gesture_cb, LV_EVENT_GESTURE, a);

    /* apps/mila_dev.txt on the card: "unlock" opens every level,
     * "level=roofs,8" opens straight into that one */
    {
        char path[96], buf[64] = "";
        snprintf(path, sizeof path, "%s/mila_dev.txt", aos_hal_path_apps());
        FILE *f = fopen(path, "r");
        if (f) {
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            fclose(f);
            if (strstr(buf, "unlock")) a->dev_unlock = true;
            const char *lv = strstr(buf, "level=");
            if (lv) snprintf(s_dev_level, sizeof s_dev_level, "%.23s", lv + 6);
            for (char *q = s_dev_level; *q; q++)
                if (*q == '\n' || *q == '\r' || *q == ' ') *q = 0;
        }
    }
    ml_audio_open();
    a->state = ST_BOOT;
    ml_ui_show(a, ST_BOOT);
    s_last_job = JOB_BOOT;
    a->job = JOB_BOOT;
    /* core 0 below LVGL's 4: busy all the time it must not starve app_main
     * there (aos_hal.h); its halves of a frame go to core 1 through
     * aos_hal_worker_split, at the same priority */
    if (!aos_hal_worker_start_on("mila", worker_fn, a, WORKER_STACK, 0, 3)) aos_hal_log("mila", "no worker");
    lv_display_add_event_cb(lv_display_get_default(), disp_cb, LV_EVENT_REFR_READY, a);
    a->timer = lv_timer_create(frame, TICK_MS, a);
    aos_hal_heap_info(&hi, &hp);
    aos_hal_log("mila", "ready | internal %u B, psram %u B", (unsigned)hi, (unsigned)hp);
    return a;
}

static void ml_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    a->closing = true;
    a->scene = SC_NONE;
    if (a->link_on) ml_link_end(a);
    if (a->timer) lv_timer_delete(a->timer);
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), disp_cb, a);
    aos_hal_worker_stop();
    ml_audio_close();
    if (a->root) lv_obj_clean(a->root);
    if (a->worlds.nworlds) mla_save(a);
    free_all(a);
    free(a);
}

/* The launcher icon: a black cat's head with amber eyes. */
static const uint8_t ML_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 4, 44, 38, 19, AIC_C_LIT(0x1A1822), 255),
    AIC_RECT(AIC_CENTER, -14, -16, 14, 16, 4, AIC_C_LIT(0x1A1822), 255),
    AIC_RECT(AIC_CENTER, 14, -16, 14, 16, 4, AIC_C_LIT(0x1A1822), 255),
    AIC_RECT(AIC_CENTER, -9, 2, 10, 12, 5, AIC_C_LIT(0xFFB21A), 255),
    AIC_RECT(AIC_CENTER, 9, 2, 10, 12, 5, AIC_C_LIT(0xFFB21A), 255),
    AIC_RECT(AIC_CENTER, -9, 2, 3, 9, 1, AIC_C_LIT(0x000000), 255),
    AIC_RECT(AIC_CENTER, 9, 2, 3, 9, 1, AIC_C_LIT(0x000000), 255),
    AIC_RECT(AIC_CENTER, 0, 11, 5, 3, 1, AIC_C_LIT(0xF07A96), 255),
    AIC_END
};

static bool ml_init(aos_app_t *app)
{
    app->desc.id       = "demo.mila";
    app->desc.name     = "Mila";
    app->desc.icon     = LV_SYMBOL_HOME;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, ML_ICON, sizeof ML_ICON);
    app->desc.color_a  = 0xF07AA0;
    app->desc.color_b  = 0x3A2A5A;
    app->desc.order    = 161;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    app->create  = ml_create;
    app->destroy = ml_destroy;
    app->hide    = app_hide;
    app->back    = app_back;
    app->resize  = app_resize;
    return true;
}

AOS_APP_ENTRY(ml_init);
