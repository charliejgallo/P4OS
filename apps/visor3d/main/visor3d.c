/*
 * P4OS (from AmoledOS) - VISOR 3D: 3D models on the whole screen.
 *
 * The files live in <card>/3d/: STL, binary or ASCII, straight from any CAD
 * or slicer, and M3D, what AmoledOS's portal page /3d makes out of STL, OBJ
 * and GLB (with colours). A grid of the models, each with a picture of
 * itself; then the model, on the whole screen:
 *
 *   one finger ....... turn it (and let go with a flick: it keeps turning)
 *   two fingers ...... zoom about the point between them, move it, and
 *                      twist it in the screen's plane
 *   tap .............. the bars on or off (and a spinning model stops)
 *   double tap ....... back to the first view
 *   long press ....... solid or wireframe
 *
 * The bars: on top the way back, the name with the numbers, and the first
 * view again (↻); below, the background (dark, grey, light: a black cat on
 * a black background is a hole), solid or wireframe, and the turntable.
 * They are up while a model loads and go away as soon as a finger turns or
 * zooms it; a tap brings them back.
 *
 * How it is built, and why:
 *
 *   - The model is drawn by the app's worker (core 0, priority 3: the HAL's
 *     defaults, below LVGL's 4, which is pinned to core 1), never in LVGL's
 *     task: a busy LVGL task steals the touch's samples. LVGL's side only
 *     moves the view and puts the newest finished frame on the screen, from
 *     three slots, with the HAL's direct blit, which returns with the
 *     picture in the framebuffer.
 *
 *   - The resolution follows the cost. While anything moves, the worker
 *     draws at one of LEVELS - the whole screen, or 1/1.25, 1/2, 1/2.5 or
 *     1/4 of it each way - and the blit scales it up (the PPA on the board:
 *     bilinear, in 1/16 steps, turned for the orientation); it starts at
 *     half (360x640, doubled) and moves a step finer or coarser from what
 *     the frames really cost, render and blit. Once the view has been still
 *     for STILL_MS, one frame at full size: 1:1 upright, whole rows the
 *     HAL copies by the AXI DMA.
 *
 *   - Nothing of LVGL's lies over the picture: LVGL does not know it is
 *     there, and whatever it draws next over it wins. The bars are opaque,
 *     and while they are up only the rows between them are blitted into
 *     the buffer on screen (a flip would hide the bars: they live only in
 *     the buffer LVGL drew them into). With the bars down the whole frame
 *     is scaled into the panel's free buffer and flipped to (no tearing);
 *     upright at full size it goes into it 1:1 as whole rows, which the
 *     HAL copies by the AXI DMA (19 ms, against the PPA's ~48 and the
 *     DMA2D's ~20 into the buffer on screen). When LVGL has painted
 *     something the picture should cover (the bars going, a panel
 *     closing), the frame on screen goes up again right after LVGL's
 *     refresh. While aos_ui_overlay() says anything but a toast is over
 *     the app, nothing is pushed at all, and the turntable and a flick's
 *     spin wait.
 *
 *   - Clearing a full frame is 3.7 MB of PSRAM, which the panel's refresh
 *     reads too (docs/MEMORY.md): each slot and the z-buffer remember the
 *     box the last frame drew in them, and only that is cleared.
 *
 *   - The grid's pictures are the same renderer: the worker loads each model
 *     while the grid is showing, draws it once, and lets it go. Opening a
 *     model cancels the one on the way, which comes back later.
 *
 *   - Loading (v3_mesh.c) welds the STL's loose triangles and reduces a model
 *     over V3_BUDGET triangles, in the worker, with its progress on top.
 *
 *   - In the simulator the blit says no, and the frame goes through an LVGL
 *     canvas instead, scaled on the CPU to the place the blit would use.
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_gesture.h"
#include "aos_sys_glyphs.h"
#include "lvgl.h"

#include "v3_mesh.h"
#include "v3_raster.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define NSLOT       3
#define MAX_FILES   48
#define NAME_LEN    64
#define TICK_MS     10
#define STILL_MS    160                 /* still this long: draw at full size */
#define FB_PX       (AOS_PANEL_W * AOS_PANEL_H)
/* 16 KB like Doom's. With 8 the watch panicked loading a 4 MB STL, inside
 * fread -> FATFS -> the SD driver (2026-09-24): the whole card path runs on
 * this stack, and in the simulator every thread has megabytes. */
#define WORKER_STACK (16 * 1024)

/* The render sizes, as the blit's factor in 16ths (the PPA's step). Each
 * divides both 720 and 1280 into whole pixels. */
static const uint8_t LEVELS[] = { 16, 20, 32, 40, 64 };
#define NLEVEL      ((int)sizeof LEVELS)
#define LEVEL_START 2                   /* 360x640 doubled                    */
#define LEVEL_SLOW  40                  /* ms a frame: a step coarser (25 fps) */
#define LEVEL_FAST  26                  /* the finer one would cost less: up  */
#define LEVEL_AVG   6                   /* frames per decision                */

/* The bars, in pixels. Multiples of 20, so that the rows between them are
 * whole rows of the picture at every level (20 = 16 x 1.25, and x 2.5 x 4). */
#define BAR_TOP_P   120
#define BAR_BOT_P   160
#define BAR_TOP_L   100
#define BAR_BOT_L   120

/* The grid */
#define THUMB       268
#define CARD_W      300
#define THUMB_QUICK (2 * 1024 * 1024)   /* bigger files get their picture last */
#define THUMB_BG    0x4A505E            /* the grey background */
#define CARD_RGB    0x1C1C1E            /* AOS_C_CARD */

enum { SLOT_FREE = 0, SLOT_BUSY, SLOT_READY, SLOT_SHOWN };
enum { PRESENT_NONE = 0, PRESENT_BLIT, PRESENT_FLIP, PRESENT_CANVAS, PRESENT_FLIP_ROWS };
enum { TH_TODO = 0, TH_BUSY, TH_DONE, TH_BAD = -1 };

typedef struct {
    char            name[NAME_LEN];
    uint32_t        bytes;
    volatile int    thumb;              /* TH_*: the worker's, until DONE     */
    int             nt, nt_file;
    uint16_t       *px;                 /* THUMB x THUMB, LVGL's byte order   */
    lv_image_dsc_t  dsc;
    bool            on_card;            /* the UI has shown what the worker found */
    lv_obj_t       *img, *ph, *meta;
} file_t;

typedef struct {
    uint16_t   *px;
    volatile uint8_t  state;
    volatile uint32_t seq;
    uint32_t    gen;                    /* the model it shows (open_cb bumps it) */
    int         fw, fh;                 /* the screen it was drawn for        */
    int         w, h, lvl;              /* its own size, and LEVELS[lvl]      */
    uint32_t    bg;
    v3_box_t    box;                    /* what it drew: the next clear       */
} slot_t;

typedef struct {
    aos_app_t  *self;
    lv_obj_t   *root;
    lv_obj_t   *list, *viewer, *touch, *canvas;
    lv_obj_t   *top, *bottom, *title, *stats;
    lv_obj_t   *btn_bg, *btn_mode, *btn_spin;
    uint16_t   *cv;                     /* the simulator's frame, FB_PX       */
    lv_timer_t *timer;

    file_t      files[MAX_FILES];
    int         count;
    int         cur;
    char        path[200];

    int         scr_w, scr_h, top_h, bot_h;

    /* the worker's side */
    v3_mesh_t   mesh, tmesh;            /* the one on screen, a thumbnail's   */
    v3_scratch_t scr, tscr;
    volatile bool want_load, want_unload, loading, mesh_ready, load_failed;
    volatile bool thumbing, cancelled;
    volatile int  progress;
    slot_t      slot[NSLOT];
    uint32_t    seq;
    volatile uint32_t gen;
    uint16_t   *zb;
    int         zb_w, zb_h;             /* the layout of what is in zb        */
    v3_box_t    zb_box;
    volatile int tgt_w, tgt_h;          /* the screen, as the UI last saw it  */
    volatile uint32_t view_seq;         /* bumped by LVGL whenever the view moves */
    uint32_t    drawn_seq;
    bool        drawn_full;
    volatile int level;                 /* LEVELS index for moving frames     */
    uint32_t    lv_sum, lv_n;
    volatile uint32_t blit_us[NLEVEL];  /* the UI's measure of each size's blit */
    uint32_t    drawn_tris;
    uint32_t    t_mov, n_mov, t_full, n_full, t_log;  /* render timing, for the log */

    /* LVGL's side */
    v3_view_t   view;
    float       vyaw, vpitch;           /* spin after a flick, rad/s */
    bool        turntable;
    bool        touching, pinching;
    uint16_t    tw_key;                 /* the two fingers' ids: the twist's anchor */
    float       tw_ang, tw_acc;
    bool        tw_on;
    volatile uint32_t moved_ms;
    int         shown;
    int         bg;                     /* index in BG */
    uint32_t    fps_ms, fps_frames;
    unsigned    fps;
    bool        viewing;
    bool        bars;
    bool        reblit;                 /* LVGL drew over the picture: put it back */
    bool        overlay;                /* something of the system is over the app */
    uint32_t    over_bits;              /* aos_ui_overlay() as the tick last read it */
    uint16_t   *flipped;                /* the panel buffer flipped to last, NULL after a blit */
    int         present;                /* PRESENT_*: how the last frame went up, for the log */
    volatile uint32_t n_flip, n_blit, n_retouch;    /* since the last log line */
    bool        hidden;
    bool        no_blit;                /* the simulator: frames through the canvas */
    char        stats_txt[128];
} app_t;

/* The three backgrounds: a dark one, a grey one, and a light one for dark
 * models. Grey is the first: on this LCD black is not special, and it is
 * the one where both a black cat and a white car read. */
static const uint32_t BG[3] = { 0x0B0E14, THUMB_BG, 0xE4E6EA };

static app_t *s_app;                    /* for give_core: one viewer at a time */

/* ---------------------------------------------------------------------------
 * Files
 * ------------------------------------------------------------------------- */

static const char *models_dir(void)
{
    static char dir[96];
    if (!dir[0]) {
        const char *root = aos_hal_path_sd_root();
        snprintf(dir, sizeof dir, "%s/3d", root ? root : aos_hal_path_data());
    }
    return dir;
}

static bool is_model(const char *n)
{
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".stl") || !strcasecmp(dot, ".m3d"));
}

static int file_cmp(const void *x, const void *y)
{
    return strcasecmp(((const file_t *)x)->name, ((const file_t *)y)->name);
}

static void file_path(const app_t *a, int i, char *out, size_t len)
{
    snprintf(out, len, "%.90s/%.63s", models_dir(), a->files[i].name);
}

/* the models in the folder, by name (readdir gives them in any order) */
static void scan(app_t *a)
{
    a->count = 0;
    mkdir(models_dir(), 0777);
    DIR *d = opendir(models_dir());
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && a->count < MAX_FILES) {
        if (e->d_name[0] == '.' || !is_model(e->d_name)) continue;
        file_t *f = &a->files[a->count++];
        snprintf(f->name, NAME_LEN, "%.63s", e->d_name);
        char p[200];
        file_path(a, a->count - 1, p, sizeof p);
        struct stat st;
        f->bytes = stat(p, &st) == 0 ? (uint32_t)st.st_size : 0;
    }
    closedir(d);
    qsort(a->files, (size_t)a->count, sizeof(file_t), file_cmp);
}

/* 15690 -> "15.690" */
static void fmt_int(char *out, size_t len, int v)
{
    char raw[16];
    snprintf(raw, sizeof raw, "%d", v < 0 ? 0 : v);
    int n = (int)strlen(raw), o = 0;
    for (int i = 0; i < n && o < (int)len - 2; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = '.';
        out[o++] = raw[i];
    }
    out[o] = 0;
}

/* ---------------------------------------------------------------------------
 * The worker
 * ------------------------------------------------------------------------- */

static void unload(app_t *a)
{
    a->mesh_ready = false;
    v3_scratch_free(&a->scr);
    v3_mesh_free(&a->mesh);
}

/* A millisecond back to the core, and whether to go on loading: not if the
 * app is closing (its code is about to be unloaded), not a model the user
 * went back from, and not a grid picture once a model was opened. */
static bool give_core(void)
{
    aos_hal_worker_sleep(1);
    app_t *a = s_app;
    bool go = !aos_hal_worker_should_stop();
    if (go && a) go = a->thumbing ? !(a->want_load || a->viewing) : !a->want_unload;
    if (!go && a) a->cancelled = true;
    return go;
}

static int free_slot(app_t *a)
{
    for (int i = 0; i < NSLOT; i++)
        if (a->slot[i].state == SLOT_FREE) return i;
    return -1;
}

static void load(app_t *a)
{
    unload(a);
    a->loading = true;
    a->load_failed = false;
    a->cancelled = false;
    a->progress = 0;
    uint32_t discard[3];
    v3_prof(discard);                   /* the grid's pictures are not this model's */
    a->t_mov = a->n_mov = a->t_full = a->n_full = 0;
    uint64_t t0 = aos_hal_uptime_ms();
    bool ok = v3_mesh_load(&a->mesh, a->path, &a->progress) &&
              v3_scratch_alloc(&a->scr, a->mesh.nv);
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    if (ok) {
        aos_hal_log("visor3d", "%s: %d triangles in the file, %d drawn, %d vertices%s, "
                    "%u ms | psram %u", a->path, a->mesh.nt_file, a->mesh.nt, a->mesh.nv,
                    a->mesh.closed ? " (solid)" : "",
                    (unsigned)(aos_hal_uptime_ms() - t0), (unsigned)fp);
        a->drawn_seq = a->view_seq - 1;     /* draw at once */
        a->mesh_ready = true;
    } else {
        aos_hal_log("visor3d", "%s: %s", a->path, a->mesh.err[0] ? a->mesh.err : "no memory");
        a->load_failed = true;
        unload(a);
    }
    a->loading = false;
}

/* Rounded corners for a grid picture: the card's colour outside them. */
static void thumb_corners(uint16_t *px)
{
    const int R = 18;
    uint16_t c = (uint16_t)(((CARD_RGB >> 8) & 0xF800) | ((CARD_RGB >> 5) & 0x07E0) | ((CARD_RGB >> 3) & 0x1F));
    for (int y = 0; y < R; y++) {
        for (int x = 0; x < R; x++) {
            float dx = (float)(R - x) - 0.5f, dy = (float)(R - y) - 0.5f;
            if (dx * dx + dy * dy <= (float)(R * R)) continue;
            px[y * THUMB + x] = c;
            px[y * THUMB + THUMB - 1 - x] = c;
            px[(THUMB - 1 - y) * THUMB + x] = c;
            px[(THUMB - 1 - y) * THUMB + THUMB - 1 - x] = c;
        }
    }
}

/* One grid picture: the next model without one, the small files first (a
 * big STL takes seconds to weld). false if there is none left. */
static bool thumb_job(app_t *a)
{
    int i = -1;
    for (int k = 0; k < a->count && i < 0; k++)
        if (a->files[k].thumb == TH_TODO && a->files[k].bytes <= THUMB_QUICK) i = k;
    for (int k = 0; k < a->count && i < 0; k++)
        if (a->files[k].thumb == TH_TODO) i = k;
    if (i < 0) return false;

    file_t *f = &a->files[i];
    f->thumb = TH_BUSY;
    char p[200];
    file_path(a, i, p, sizeof p);
    a->cancelled = false;
    a->thumbing = true;
    uint64_t t0 = aos_hal_uptime_ms();
    bool ok = v3_mesh_load(&a->tmesh, p, NULL) && v3_scratch_alloc(&a->tscr, a->tmesh.nv);
    a->thumbing = false;
    if (ok && !f->px) f->px = malloc((size_t)THUMB * THUMB * 2);
    if (!ok || !f->px) {
        bool again = a->cancelled;
        if (!again) aos_hal_log("visor3d", "picture of %s: %s", f->name, a->tmesh.err[0] ? a->tmesh.err : "no memory");
        v3_scratch_free(&a->tscr);
        v3_mesh_free(&a->tmesh);
        f->thumb = again ? TH_TODO : TH_BAD;
        return true;
    }
    uint64_t t1 = aos_hal_uptime_ms();
    v3_view_t v = { .yaw = 0.6f, .pitch = 0.35f, .scale = 1.0f, .mode = V3_SOLID, .bg = THUMB_BG };
    v3_box_t fb_all = { 0, 0, THUMB, THUMB }, zb_all = fb_all;
    v3_render(&a->tmesh, &v, &a->tscr, f->px, a->zb, THUMB, THUMB, &fb_all, &zb_all);
    a->zb_w = 0;                        /* the viewer's z-buffer is someone else's now */
    thumb_corners(f->px);
    f->nt = a->tmesh.nt;
    f->nt_file = a->tmesh.nt_file;
    aos_hal_log("visor3d", "picture of %s: %d tri, loaded in %u ms, drawn in %u ms", f->name,
                f->nt, (unsigned)(t1 - t0), (unsigned)(aos_hal_uptime_ms() - t1));
    v3_scratch_free(&a->tscr);
    v3_mesh_free(&a->tmesh);
    f->thumb = TH_DONE;
    return true;
}

/* A step finer or coarser, from what the last LEVEL_AVG moving frames cost:
 * the render here, the blit as the UI measured it (they overlap, on two
 * cores, so a frame costs the slower of the two). A finer size is guessed
 * from the pixels, a little under (the triangles' own cost does not grow),
 * and its blit from what it measured the last time it ran. */
static void level_adapt(app_t *a, uint32_t render_ms)
{
    a->lv_sum += render_ms;
    if (++a->lv_n < LEVEL_AVG) return;
    uint32_t r = a->lv_sum / a->lv_n;
    a->lv_sum = a->lv_n = 0;
    int l = a->level;
    uint32_t b = a->blit_us[l] / 1000;
    uint32_t cost = r > b ? r : b;
    if (cost > LEVEL_SLOW && l < NLEVEL - 1) {
        a->level = l + 1;
    } else if (l > 0) {
        float k = (float)LEVELS[l] / LEVELS[l - 1];
        uint32_t est = (uint32_t)((float)r * k * k * 0.85f);
        uint32_t bf = a->blit_us[l - 1] / 1000;
        if (est < LEVEL_FAST && bf < LEVEL_FAST) a->level = l - 1;
    }
    if (a->level != l) {
        aos_hal_log("visor3d", "moving frames: %dx%d -> %dx%d (render %u ms, blit %u ms)",
                    a->tgt_w * 16 / LEVELS[l], a->tgt_h * 16 / LEVELS[l],
                    a->tgt_w * 16 / LEVELS[a->level], a->tgt_h * 16 / LEVELS[a->level],
                    (unsigned)r, (unsigned)b);
    }
}

static void frame(app_t *a, int i, bool moving)
{
    slot_t *s = &a->slot[i];
    int W = a->tgt_w, H = a->tgt_h;
    int lvl = moving ? a->level : 0;
    int num = LEVELS[lvl];
    int w = W * 16 / num, h = H * 16 / num;
    v3_view_t v = a->view;
    v.px = v.px * 16 / num;
    v.py = v.py * 16 / num;

    /* only the last frame's box is cleared, when the buffer holds a frame
     * of this size and background; the whole of it otherwise */
    v3_box_t all = { 0, 0, w, h };
    if (s->w != w || s->h != h || s->bg != v.bg) s->box = all;
    if (a->zb_w != w || a->zb_h != h) a->zb_box = all;
    s->w = w;
    s->h = h;
    s->bg = v.bg;
    s->lvl = lvl;
    s->fw = W;
    s->fh = H;
    s->gen = a->gen;

    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    a->drawn_tris = (uint32_t)v3_render(&a->mesh, &v, &a->scr, s->px, a->zb, w, h, &s->box, &a->zb_box);
    if (v.mode == V3_SOLID) {
        a->zb_w = w;
        a->zb_h = h;
    }
    uint32_t t1 = (uint32_t)aos_hal_uptime_ms();
    a->drawn_full = !moving;
    if (moving) {
        a->t_mov += t1 - t0;
        a->n_mov++;
        level_adapt(a, t1 - t0);
    } else {
        a->t_full += t1 - t0;
        a->n_full++;
    }
    if (t1 - a->t_log >= 5000 && (a->n_mov || a->n_full)) {
        uint32_t pc[3];
        v3_prof(pc);
        uint32_t nf = a->n_mov + a->n_full;
        int ml = a->level;
        aos_hal_log("visor3d", "%s, %d tri%s: %u ms a frame moving at %dx%d (%u frames, blit %u us), "
                    "%u ms at full (%u) | per frame: clear %u + vertices %u + triangles %u us | "
                    "presented: %u flipped, %u blitted, %u again after LVGL",
                    a->files[a->cur].name, a->mesh.nt, a->mesh.closed ? " (solid)" : "",
                    (unsigned)(a->n_mov ? a->t_mov / a->n_mov : 0),
                    W * 16 / LEVELS[ml], H * 16 / LEVELS[ml], (unsigned)a->n_mov,
                    (unsigned)a->blit_us[ml],
                    (unsigned)(a->n_full ? a->t_full / a->n_full : 0), (unsigned)a->n_full,
                    (unsigned)(pc[0] / nf), (unsigned)(pc[1] / nf), (unsigned)(pc[2] / nf),
                    (unsigned)a->n_flip, (unsigned)a->n_blit, (unsigned)a->n_retouch);
        a->n_flip = a->n_blit = a->n_retouch = 0;
        a->t_mov = a->n_mov = a->t_full = a->n_full = 0;
        a->t_log = t1;
    }
}

static void worker(void *arg)
{
    app_t *a = (app_t *)arg;
    v3_mesh_set_yield(give_core);
    while (!aos_hal_worker_should_stop()) {
        if (a->want_unload) {
            unload(a);
            a->want_unload = false;
            continue;
        }
        if (a->want_load) {
            a->want_load = false;
            load(a);
            continue;
        }
        if (!a->viewing) {
            if (!thumb_job(a)) aos_hal_worker_sleep(20);
            continue;
        }
        if (!a->mesh_ready) {
            aos_hal_worker_sleep(20);
            continue;
        }
        /* What to draw: a moving view at the moving size; the same view,
         * still for a moment, once more at full size; nothing new, nothing. */
        uint32_t vs = a->view_seq;
        bool moving = vs != a->drawn_seq;
        bool settle = !moving && !a->drawn_full &&
                      (uint32_t)aos_hal_uptime_ms() - a->moved_ms > STILL_MS;
        if (!moving && !settle) {
            aos_hal_worker_sleep(8);
            continue;
        }
        int i = free_slot(a);
        if (i < 0) {
            aos_hal_worker_sleep(3);
            continue;
        }
        a->slot[i].state = SLOT_BUSY;
        frame(a, i, moving);
        a->drawn_seq = vs;
        a->slot[i].seq = ++a->seq;
        a->slot[i].state = SLOT_READY;
        /* A millisecond back to the core after every frame. While a finger
         * turns the model there is always a next frame to draw, and a worker
         * that never blocks starves IDLE0: the task watchdog fired on the
         * watch after ~5 s of turning (2026-09-24). */
        aos_hal_worker_sleep(1);
    }
}

/* ---------------------------------------------------------------------------
 * LVGL's side: the frames on the screen
 * ------------------------------------------------------------------------- */

static void view_home(app_t *a)
{
    a->view.yaw = 0.6f;
    a->view.pitch = 0.35f;
    a->view.roll = 0;
    a->view.scale = 1.0f;
    a->view.px = a->view.py = 0;
    a->vyaw = a->vpitch = 0;
}

static void view_moved(app_t *a)
{
    a->moved_ms = (uint32_t)aos_hal_uptime_ms();
    a->view_seq++;
}

/* The simulator (or a blit the HAL refused): the canvas, the rows y0..y1 of
 * the screen, nearest pixel. Never on a board that blits: a canvas under the
 * picture would be redrawn by LVGL over the blits. */
static void canvas_frame(app_t *a, const slot_t *s, int y0, int y1)
{
    if (!a->cv) {
        a->cv = malloc((size_t)FB_PX * 2);
        if (!a->cv) return;
    }
    if (!a->canvas) {
        y0 = 0;                         /* a new canvas: all of it, bars or not */
        y1 = a->scr_h;
        a->canvas = lv_canvas_create(a->viewer);
        lv_canvas_set_buffer(a->canvas, a->cv, a->scr_w, a->scr_h, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(a->canvas, 0, 0);
        lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_move_to_index(a->canvas, 0);             /* behind the bars */
    }
    int num = LEVELS[s->lvl];
    for (int y = y0; y < y1; y++) {
        const uint16_t *src = s->px + (size_t)(y * 16 / num) * s->w;
        uint16_t *d = a->cv + (size_t)y * a->scr_w;
        for (int x = 0; x < a->scr_w; x++) d[x] = src[x * 16 / num];
    }
    lv_obj_invalidate(a->canvas);
}

/* The rows of the screen the picture may cover: all of them, or those
 * between the bars. Multiples of 20 (BAR_*), so whole rows at every level. */
static void picture_rows(const app_t *a, int *y0, int *y1)
{
    *y0 = a->bars ? a->top_h : 0;
    *y1 = a->bars ? a->scr_h - a->bot_h : a->scr_h;
}

static void present_log(app_t *a, int mode)
{
    if (a->present == mode) return;
    a->present = mode;
    static const char *const WHAT[] = {
        "", "blitted into the buffer on screen (the bars are up)",
        "scaled into the panel's free buffer and flipped to", "through LVGL's canvas",
        "1:1 into the panel's free buffer (whole rows, the AXI DMA) and flipped to",
    };
    aos_hal_log("visor3d", "frames: %s", WHAT[mode]);
}

/* The bars down: nothing of LVGL's is over the picture, so the whole frame
 * is scaled into the panel's free buffer (aos_hal_display_blit_into_fit,
 * the PPA, turned as the screen is) and flipped to: no tearing, and the
 * same PPA pass the blit made into the buffer on screen. Upright at full
 * size (the still frame) it is 1:1 rows the screen's width from row 0: the
 * HAL copies those by the AXI DMA into the free buffer (19 ms on the
 * board; the destination on 128-byte lines, which whole rows from row 0
 * are), where the blit into the buffer on screen was the DMA2D's ~20 and
 * could tear. With the bars up a flip would hide them (they are only in
 * the buffer LVGL drew them into): the rows between them are blitted as
 * before. */
static bool flip_slot(app_t *a, const slot_t *s)
{
    if (a->bars) return false;
    uint16_t *back = aos_hal_display_back();
    if (!back) return false;
    if (!aos_hal_display_blit_into_fit(back, 0, 0, a->scr_w, a->scr_h, s->px, s->w, s->h, s->w, false))
        return false;
    if (!aos_hal_display_flip(back)) return false;
    a->flipped = back;
    return true;
}

static bool show_slot(app_t *a, slot_t *s)
{
    int y0, y1;
    picture_rows(a, &y0, &y1);
    if (a->no_blit) {
        present_log(a, PRESENT_CANVAS);
        canvas_frame(a, s, y0, y1);
        return true;
    }
    int num = LEVELS[s->lvl];
    int r0 = y0 * 16 / num, r1 = y1 * 16 / num;
    const uint16_t *src = s->px + (size_t)r0 * s->w;
    uint64_t t0 = aos_hal_uptime_us();
    bool flipped = flip_slot(a, s);
    bool ok = flipped;
    if (!ok) {
        ok = num % 16 == 0
            ? aos_hal_display_blit_scaled(0, y0, s->w, r1 - r0, src, num / 16, false)
            : aos_hal_display_blit_fit(0, y0, a->scr_w, y1 - y0, src, s->w, r1 - r0, s->w, false);
        if (ok) a->flipped = NULL;      /* the buffer on screen is LVGL's and ours now */
    }
    if (ok) {
        present_log(a, !flipped ? PRESENT_BLIT
                       : num == 16 && aos_hal_display_get_rotation() == 0 ? PRESENT_FLIP_ROWS : PRESENT_FLIP);
        if (flipped) a->n_flip++;
        else a->n_blit++;
        uint32_t us = (uint32_t)(aos_hal_uptime_us() - t0);
        /* scaled to the whole screen: a blit between the bars is shorter */
        if (y1 > y0) us = (uint32_t)((uint64_t)us * (uint32_t)a->scr_h / (uint32_t)(y1 - y0));
        uint32_t old = a->blit_us[s->lvl];
        a->blit_us[s->lvl] = old ? (old * 3 + us) / 4 : us;
        return true;
    }
    aos_hal_log("visor3d", "no direct blit here: the frames go through LVGL");
    a->no_blit = true;
    present_log(a, PRESENT_CANVAS);
    canvas_frame(a, s, y0, y1);
    return true;
}

static bool slot_fits(const app_t *a, const slot_t *s)
{
    return s->gen == a->gen && s->fw == a->scr_w && s->fh == a->scr_h;
}

static void push_frame(app_t *a)
{
    int best = -1;
    uint32_t bs = 0;
    for (int i = 0; i < NSLOT; i++) {
        slot_t *s = &a->slot[i];
        if (s->state != SLOT_READY) continue;
        if (!slot_fits(a, s)) {
            s->state = SLOT_FREE;           /* another model, or the screen turned */
            continue;
        }
        if (best < 0 || s->seq > bs) {
            best = i;
            bs = s->seq;
        }
    }
    if (best < 0) return;
    for (int i = 0; i < NSLOT; i++)
        if (i != best && a->slot[i].state == SLOT_READY) a->slot[i].state = SLOT_FREE;
    show_slot(a, &a->slot[best]);
    if (a->shown >= 0 && a->shown != best) a->slot[a->shown].state = SLOT_FREE;
    a->slot[best].state = SLOT_SHOWN;
    a->shown = best;
    a->fps_frames++;
}

/* The frame on screen, again: LVGL just painted over it. */
static void reblit(app_t *a)
{
    a->reblit = false;
    if (a->shown < 0) return;
    slot_t *s = &a->slot[a->shown];
    if (s->state == SLOT_SHOWN && slot_fits(a, s)) show_slot(a, s);
}

static void refr_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    if (a->reblit && a->viewing && !a->overlay) reblit(a);
}

/* ---------------------------------------------------------------------------
 * The bars
 * ------------------------------------------------------------------------- */

static void stats_set(app_t *a, const char *t)
{
    if (!strcmp(a->stats_txt, t)) return;
    snprintf(a->stats_txt, sizeof a->stats_txt, "%s", t);
    lv_label_set_text(a->stats, t);
}

static void stats_text(app_t *a)
{
    char buf[128];
    if (a->loading) {
        int p = a->progress;
        snprintf(buf, sizeof buf, "%s  %d%%", p >= 3000 ? _("Reduciendo...") : _("Cargando..."),
                 p % 1000);
    } else if (a->load_failed) {
        snprintf(buf, sizeof buf, "%s: %s", _("No se pudo abrir"), a->mesh.err[0] ? _(a->mesh.err) : "?");
    } else if (a->mesh_ready) {
        char nt[16], nf[16];
        fmt_int(nt, sizeof nt, a->mesh.nt);
        fmt_int(nf, sizeof nf, a->mesh.nt_file);
        int n = a->mesh.nt < a->mesh.nt_file
            ? snprintf(buf, sizeof buf, "%s/%s tri", nt, nf)
            : snprintf(buf, sizeof buf, "%s tri", nt);
        /* the rate and the size only while something moves */
        if (a->fps >= 2 && a->shown >= 0 && n > 0 && n < (int)sizeof buf) {
            const slot_t *s = &a->slot[a->shown];
            snprintf(buf + n, sizeof buf - (size_t)n, " · %u fps · %dx%d", a->fps, s->w, s->h);
        }
    } else {
        buf[0] = 0;
    }
    stats_set(a, buf);
}

static void bars_set(app_t *a, bool up)
{
    if (a->loading || a->load_failed) up = true;    /* the progress lives there */
    if (a->bars == up) return;
    a->bars = up;
    if (a->top) {
        lv_obj_set_flag(a->top, LV_OBJ_FLAG_HIDDEN, !up);
        lv_obj_set_flag(a->bottom, LV_OBJ_FLAG_HIDDEN, !up);
    }
    if (!up) a->reblit = true;          /* LVGL paints the background where they were */
}

/* The buttons say what they are set to. */
static void buttons_refresh(app_t *a)
{
    static const char *const BGN[3] = { N_("Fondo oscuro"), N_("Fondo gris"), N_("Fondo claro") };
    if (!a->btn_bg) return;
    lv_label_set_text(lv_obj_get_child(a->btn_bg, 1), _(BGN[a->bg]));
    lv_label_set_text(lv_obj_get_child(a->btn_mode, 1),
                      a->view.mode == V3_WIRE ? _("alambre") : _("sólido"));
    lv_obj_set_style_bg_color(a->btn_spin, a->turntable ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(a->viewer, lv_color_hex(BG[a->bg]), 0);
}

static void bg_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->bg = (a->bg + 1) % 3;
    a->view.bg = BG[a->bg];
    aos_hal_pref_set_i32("v3_bg", a->bg);
    buttons_refresh(a);
    view_moved(a);
}

static void mode_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->view.mode = (a->view.mode + 1) % V3_MODES;
    buttons_refresh(a);
    view_moved(a);
}

static void spin_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->turntable = !a->turntable;
    buttons_refresh(a);
}

static void home_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    view_home(a);                       /* the turntable stays as it was */
    view_moved(a);
}

static void show_list(app_t *a);

static void back_cb(lv_event_t *e)
{
    show_list((app_t *)lv_event_get_user_data(e));
}

/* ---------------------------------------------------------------------------
 * Fingers
 * ------------------------------------------------------------------------- */

#define TURN_K      0.0085f             /* radians per pixel: the width is one turn */
#define TWIST_START 0.12f               /* rad of twist before it counts (~7 degrees) */

/* A finger's movement on the glass to the model's two turns. The twist
 * turned the picture, so the movement is turned back first: a finger along
 * the model's (turned) horizontal spins it about its own up. */
static void finger_turn(const app_t *a, float dx, float dy, float *dyaw, float *dpitch)
{
    float cr = cosf(a->view.roll), sr = sinf(a->view.roll);
    float ux = dx, uy = -dy;            /* y up, like the model */
    float px = cr * ux + sr * uy, py = -sr * ux + cr * uy;
    *dyaw = px * TURN_K;
    *dpitch = -py * TURN_K;
}

/* The picture turned by d (radians, clockwise on the glass) about the point
 * between the fingers: the model twists, and its centre goes round that
 * point, so what is under the fingers stays there. */
static void twist(app_t *a, float d, float fx, float fy)
{
    float W = (float)a->scr_w, H = (float)a->scr_h;
    float mx = W * 0.5f + a->view.px - fx, my = H * 0.5f + a->view.py - fy;
    float c = cosf(d), s = sinf(d);
    a->view.roll -= d;
    a->view.px = fx + c * mx - s * my - W * 0.5f;
    a->view.py = fy + s * mx + c * my - H * 0.5f;
    view_moved(a);
}

/* The twist, from the two fingers themselves: aos_gesture gives a pinch's
 * scale and centre but no angle (the watch's chip swapped the fingers' X on
 * one diagonal; the GT911 reports both cleanly). The angle of the line
 * between them, against the last sample with the same two fingers; jumps
 * are dropped, and nothing turns until the fingers have twisted a little,
 * so a plain pinch does not turn the model askew. */
static void twist_poll(app_t *a)
{
    aos_touch_point_t p[2];
    if (aos_touch_points(p) < 2 || !p[0].down || !p[1].down) {
        a->tw_key = 0;
        return;
    }
    float ang = atan2f(p[1].y - p[0].y, p[1].x - p[0].x);
    uint16_t key = (uint16_t)(p[0].id << 8 | p[1].id);
    if (key == a->tw_key) {
        float d = ang - a->tw_ang;
        if (d > 3.14159265f) d -= 6.2831853f;
        if (d < -3.14159265f) d += 6.2831853f;
        if (fabsf(d) < 0.35f) {
            if (!a->tw_on) {
                a->tw_acc += d;
                if (fabsf(a->tw_acc) >= TWIST_START) {
                    a->tw_on = true;
                    d = a->tw_acc;      /* what it took to start, at once */
                } else {
                    d = 0;
                }
            }
            if (d != 0) twist(a, d, (p[0].x + p[1].x) * 0.5f, (p[0].y + p[1].y) * 0.5f);
        }
    }
    a->tw_key = key;
    a->tw_ang = ang;
}

static void gesture_cb(const aos_gesture_event_t *ev, void *user)
{
    app_t *a = (app_t *)user;
    if (!a->mesh_ready) return;
    switch (ev->type) {
    case AOS_GESTURE_DRAG_BEGIN:
        a->touching = true;
        a->vyaw = a->vpitch = 0;
        bars_set(a, false);
        break;
    case AOS_GESTURE_PINCH_BEGIN:
        a->touching = true;
        a->pinching = true;
        a->vyaw = a->vpitch = 0;
        a->tw_key = 0;
        a->tw_acc = 0;
        a->tw_on = false;
        bars_set(a, false);
        break;
    case AOS_GESTURE_DRAG: {
        float dy, dp;
        finger_turn(a, ev->dx, ev->dy, &dy, &dp);
        a->view.yaw += dy;
        a->view.pitch += dp;
        view_moved(a);
        break;
    }
    case AOS_GESTURE_DRAG_END:
        a->touching = false;
        finger_turn(a, ev->vx, ev->vy, &a->vyaw, &a->vpitch);
        break;
    case AOS_GESTURE_PINCH: {
        float ns = a->view.scale * ev->scale;
        if (ns < 0.3f) ns = 0.3f;
        if (ns > 10.0f) ns = 10.0f;
        float k = ns / a->view.scale;
        /* the point between the fingers stays under them */
        float W = (float)a->scr_w, H = (float)a->scr_h;
        float ox = W * 0.5f + a->view.px, oy = H * 0.5f + a->view.py;
        a->view.px = ev->x - W * 0.5f - (ev->x - ox) * k + ev->dx;
        a->view.py = ev->y - H * 0.5f - (ev->y - oy) * k + ev->dy;
        a->view.scale = ns;
        view_moved(a);
        break;
    }
    case AOS_GESTURE_PINCH_END:
        a->touching = false;
        a->pinching = false;
        break;
    case AOS_GESTURE_DOUBLE_TAP:
        view_home(a);
        a->turntable = false;
        buttons_refresh(a);
        view_moved(a);
        break;
    case AOS_GESTURE_TAP:
        a->vyaw = a->vpitch = 0;        /* a finger on a spinning model catches it */
        bars_set(a, !a->bars);
        break;
    case AOS_GESTURE_LONG_PRESS:
        a->view.mode = (a->view.mode + 1) % V3_MODES;
        buttons_refresh(a);
        view_moved(a);
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * The grid
 * ------------------------------------------------------------------------- */

static void card_meta(app_t *a, int i)
{
    file_t *f = &a->files[i];
    if (!f->meta) return;
    char buf[96], kb[16];
    fmt_int(kb, sizeof kb, (int)((f->bytes + 1023) / 1024));
    if (f->thumb == TH_DONE) {
        char nt[16], nf[16];
        fmt_int(nt, sizeof nt, f->nt);
        fmt_int(nf, sizeof nf, f->nt_file);
        if (f->nt < f->nt_file) snprintf(buf, sizeof buf, "%s/%s tri · %s KB", nt, nf, kb);
        else snprintf(buf, sizeof buf, "%s tri · %s KB", nt, kb);
    } else if (f->thumb == TH_BAD) {
        snprintf(buf, sizeof buf, "%s · %s KB", _("no se pudo leer"), kb);
    } else {
        snprintf(buf, sizeof buf, "%s KB", kb);
    }
    lv_label_set_text(f->meta, buf);
}

/* What the worker drew goes onto the cards. */
static void list_tick(app_t *a)
{
    for (int i = 0; i < a->count; i++) {
        file_t *f = &a->files[i];
        if (f->on_card || !f->img) continue;
        if (f->thumb == TH_DONE) {
            f->dsc = (lv_image_dsc_t){
                .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_RGB565,
                            .w = THUMB, .h = THUMB, .stride = THUMB * 2 },
                .data_size = THUMB * THUMB * 2,
                .data = (const uint8_t *)f->px,
            };
            lv_image_set_src(f->img, &f->dsc);
            lv_obj_remove_flag(f->img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(f->ph, LV_OBJ_FLAG_HIDDEN);
        } else if (f->thumb != TH_BAD) {
            continue;
        }
        f->on_card = true;
        card_meta(a, i);
    }
}

static void open_cb(lv_event_t *e);

static void build_card(app_t *a, lv_obj_t *grid, int i)
{
    file_t *f = &a->files[i];
    lv_obj_t *card = lv_obj_create(grid);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, CARD_W, 16 + THUMB + 12 + 36 + 30 + 14);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(card, 26, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)i);
    lv_obj_add_event_cb(card, open_cb, LV_EVENT_CLICKED, a);

    int tx = (CARD_W - THUMB) / 2;
    f->ph = lv_obj_create(card);
    lv_obj_remove_style_all(f->ph);
    lv_obj_set_size(f->ph, THUMB, THUMB);
    lv_obj_set_pos(f->ph, tx, 16);
    lv_obj_set_style_bg_color(f->ph, lv_color_hex(THUMB_BG), 0);
    lv_obj_set_style_bg_opa(f->ph, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f->ph, 18, 0);
    lv_obj_t *g = lv_label_create(f->ph);
    lv_label_set_text(g, AOS_SYM_SHAPE_OUTLINE);
    lv_obj_set_style_text_font(g, &aos_sym_72, 0);
    lv_obj_set_style_text_color(g, lv_color_hex(0x7A8090), 0);
    lv_obj_center(g);

    f->img = lv_image_create(card);
    lv_obj_set_pos(f->img, tx, 16);
    lv_obj_add_flag(f->img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *l = aos_label(card, f->name, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, CARD_W - 32, 36);
    lv_obj_set_pos(l, 16, 16 + THUMB + 12);

    f->meta = aos_label(card, "", aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(f->meta, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(f->meta, CARD_W - 32, 28);
    lv_obj_set_pos(f->meta, 16, 16 + THUMB + 12 + 38);

    aos_make_decorative(f->ph);
    lv_obj_remove_flag(f->img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(f->meta, LV_OBJ_FLAG_CLICKABLE);
    f->on_card = false;
    card_meta(a, i);
}

static void build_list(app_t *a)
{
    a->list = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->list);
    lv_obj_set_size(a->list, a->scr_w, a->scr_h);
    lv_obj_set_style_bg_color(a->list, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(a->list, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(a->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a->list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(a->list, 14, 0);
    lv_obj_set_style_pad_top(a->list, 56, 0);
    lv_obj_set_style_pad_bottom(a->list, 60, 0);
    lv_obj_set_scroll_dir(a->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->list, LV_SCROLLBAR_MODE_OFF);

    aos_label(a->list, _("Visor 3D"), aos_font_title, AOS_C_TEXT);
    lv_obj_t *hint = aos_label(a->list, _("Un dedo lo gira; dos lo acercan, lo mueven y lo tuercen"),
                               aos_font_small, AOS_C_DIM);
    lv_obj_set_width(hint, a->scr_w - 64);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);

    if (a->count == 0) {
        char msg[240];
        snprintf(msg, sizeof msg, _("No hay modelos.\nCopiá archivos .stl o .m3d a\n%s\n(o subilos desde el portal: Archivos)."),
                 models_dir());
        lv_obj_t *l = aos_label(a->list, msg, aos_font_body, AOS_C_DIM);
        lv_obj_set_width(l, a->scr_w - 80);
        lv_obj_set_style_pad_top(l, 80, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    lv_obj_t *grid = lv_obj_create(a->list);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, a->scr_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 28, 0);
    lv_obj_set_style_pad_top(grid, 20, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (int i = 0; i < a->count; i++) build_card(a, grid, i);

    char foot[160];
    snprintf(foot, sizeof foot, "%s: %s", _("Carpeta"), models_dir());
    lv_obj_t *l = aos_label(a->list, foot, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_top(l, 16, 0);
}

/* ---------------------------------------------------------------------------
 * The viewer
 * ------------------------------------------------------------------------- */

static lv_obj_t *round_button(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, app_t *a)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 76, 76);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, a);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, &aos_sym_44, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

/* A bottom button: a glyph and a word; child 1 is the word. */
static lv_obj_t *bar_button(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, app_t *a,
                            int w, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, a);
    lv_obj_t *g = lv_label_create(b);
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_font(g, &aos_sym_28, 0);
    lv_obj_set_style_text_color(g, AOS_C_TEXT, 0);
    lv_obj_t *t = aos_label(b, "", aos_font_small, AOS_C_TEXT);
    (void)t;
    aos_make_decorative(g);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    return b;
}

static lv_obj_t *bar(lv_obj_t *parent, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x0E0F12), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);      /* opaque: the picture is not LVGL's */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);         /* a touch on it is not the model's */
    return o;
}

static void build_viewer(app_t *a)
{
    int W = a->scr_w, H = a->scr_h;
    bool land = W > H;
    a->top_h = land ? BAR_TOP_L : BAR_TOP_P;
    a->bot_h = land ? BAR_BOT_L : BAR_BOT_P;

    a->viewer = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->viewer);
    lv_obj_set_size(a->viewer, W, H);
    lv_obj_set_style_bg_color(a->viewer, lv_color_hex(BG[a->bg]), 0);
    lv_obj_set_style_bg_opa(a->viewer, LV_OPA_COVER, 0);
    lv_obj_remove_flag(a->viewer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(a->viewer, LV_OBJ_FLAG_HIDDEN);
    a->canvas = NULL;                   /* made on the first frame that needs it */

    a->touch = lv_obj_create(a->viewer);
    lv_obj_remove_style_all(a->touch);
    lv_obj_set_size(a->touch, W, H);
    aos_gesture_attach(a->touch, 0, gesture_cb, a);

    /* on top: the way back, the name and the numbers, the first view */
    a->top = bar(a->viewer, 0, W, a->top_h);
    int by = (a->top_h - 76) / 2;
    lv_obj_t *back = round_button(a->top, AOS_SYM_CHEVRON_LEFT, back_cb, a);
    lv_obj_set_pos(back, 22, by);
    lv_obj_t *home = round_button(a->top, AOS_SYM_RESTART, home_cb, a);
    lv_obj_set_pos(home, W - 22 - 76, by);
    a->title = aos_label(a->top, a->cur >= 0 ? a->files[a->cur].name : "", aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(a->title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(a->title, W - 2 * 120, 36);
    lv_obj_set_style_text_align(a->title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(a->title, 120, a->top_h / 2 - 36);
    a->stats = aos_label(a->top, "", aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(a->stats, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(a->stats, W - 2 * 120, 28);
    lv_obj_set_style_text_align(a->stats, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(a->stats, 120, a->top_h / 2 + 4);
    a->stats_txt[0] = 0;

    /* below: background, solid or wireframe, the turntable */
    a->bottom = bar(a->viewer, H - a->bot_h, W, a->bot_h);
    int bw = land ? 260 : (W - 4 * 20) / 3;
    int bh = land ? 80 : 96;
    int gap = land ? 24 : 20;
    int x0 = (W - 3 * bw - 2 * gap) / 2;
    lv_event_cb_t cbs[3] = { bg_cb, mode_cb, spin_cb };
    const char *glyphs[3] = { AOS_SYM_PALETTE, AOS_SYM_VECTOR_SQUARE, AOS_SYM_UPDATE };
    lv_obj_t **btn[3] = { &a->btn_bg, &a->btn_mode, &a->btn_spin };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = bar_button(a->bottom, glyphs[i], cbs[i], a, bw, bh);
        lv_obj_set_pos(b, x0 + i * (bw + gap), (a->bot_h - bh) / 2);
        *btn[i] = b;
    }
    lv_label_set_text(lv_obj_get_child(a->btn_spin, 1), _("Girar"));

    lv_obj_set_flag(a->top, LV_OBJ_FLAG_HIDDEN, !a->bars);
    lv_obj_set_flag(a->bottom, LV_OBJ_FLAG_HIDDEN, !a->bars);
    buttons_refresh(a);
}

static void build(app_t *a)
{
    lv_obj_clean(a->root);
    a->scr_w = AOS_SCREEN_W;            /* FULLSCREEN: the root is the whole screen */
    a->scr_h = AOS_SCREEN_H;
    a->tgt_w = a->scr_w;
    a->tgt_h = a->scr_h;
    for (int i = 0; i < a->count; i++) {
        a->files[i].img = a->files[i].ph = a->files[i].meta = NULL;
    }
    build_list(a);
    build_viewer(a);
    if (a->viewing) {
        lv_obj_add_flag(a->list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(a->viewer, LV_OBJ_FLAG_HIDDEN);
        a->reblit = true;               /* as in open_cb */
    }
    list_tick(a);
}

/* ---------------------------------------------------------------------------
 * The two screens
 * ------------------------------------------------------------------------- */

static void show_list(app_t *a)
{
    a->viewing = false;
    a->want_unload = true;
    a->gen++;
    a->self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
    lv_obj_add_flag(a->viewer, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(a->list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(a->root);         /* what was blitted is not LVGL's */
    a->flipped = NULL;
    if (a->canvas) {                    /* the simulator's: the next model starts blank */
        lv_obj_delete(a->canvas);
        a->canvas = NULL;
    }
}

static void open_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= a->count || a->loading) return;
    a->cur = i;
    file_path(a, i, a->path, sizeof a->path);
    view_home(a);
    a->view.mode = V3_SOLID;
    a->view.bg = BG[a->bg];
    a->turntable = false;
    a->gen++;
    if (a->shown >= 0) a->slot[a->shown].state = SLOT_FREE;
    a->shown = -1;
    a->fps = 0;
    a->mesh_ready = false;
    a->load_failed = false;
    a->loading = true;                  /* until the worker says otherwise */
    a->want_load = true;
    a->viewing = true;
    a->self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    lv_label_set_text(a->title, a->files[i].name);
    bars_set(a, true);
    buttons_refresh(a);
    lv_obj_add_flag(a->list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(a->viewer, LV_OBJ_FLAG_HIDDEN);
    /* LVGL paints the viewer at its next refresh, which may come after the
     * first frame is blitted: that frame goes up again once it has */
    a->reblit = true;
    stats_text(a);
}

static void tick(lv_timer_t *t)
{
    app_t *a = (app_t *)lv_timer_get_user_data(t);
    if (!a->viewing) {
        list_tick(a);
        return;
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    float dt = TICK_MS / 1000.0f;

    /* A system panel, the switcher, a banner, the home gesture sliding the
     * app, the zoom from its icon: none of them gets a picture pushed over
     * it, and nothing turns under them. A toast is let be covered. When
     * they go, LVGL has drawn the app again, and the frame goes back on
     * top, whole. */
    a->over_bits = aos_ui_overlay();
    bool ov = a->hidden || (a->over_bits & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    if (a->overlay && !ov) a->reblit = true;
    a->overlay = ov;
    /* The frame we flipped to painted over by LVGL since (the tail of a
     * panel closing, the viewer's background): up again. Not for a toast,
     * which may stay over the picture until the next frame covers it. */
    if (!ov && a->flipped && !a->bars && !(a->over_bits & AOS_UI_OVER_TOAST)) {
        bool touched = false;
        if (aos_hal_display_back_age(a->flipped, NULL, &touched) && touched && !a->reblit) {
            a->reblit = true;
            a->n_retouch++;
        }
    }

    if (a->pinching) twist_poll(a);

    /* the flick's spin, dying out; the turntable, steady; both wait while
     * something of the system is over the app */
    if (!ov && !a->touching) {
        if (fabsf(a->vyaw) > 0.02f || fabsf(a->vpitch) > 0.02f) {
            a->view.yaw += a->vyaw * dt;
            a->view.pitch += a->vpitch * dt;
            a->vyaw *= 0.95f;
            a->vpitch *= 0.95f;
            view_moved(a);
        } else if (a->turntable) {
            a->view.yaw += 0.6f * dt;
            view_moved(a);
        }
    }
    if (a->loading || a->load_failed) bars_set(a, true);
    if (!a->overlay) push_frame(a);

    if (now - a->fps_ms >= 500) {
        a->fps = (unsigned)(a->fps_frames * 1000 / (now - a->fps_ms ? now - a->fps_ms : 1));
        a->fps_frames = 0;
        a->fps_ms = now;
        if (a->bars) stats_text(a);
    } else if (a->loading && a->bars) {
        stats_text(a);
    }
}

/* ---------------------------------------------------------------------------
 * Life cycle
 * ------------------------------------------------------------------------- */

static void v3_destroy(aos_app_t *self, void *inst);

static void *v3_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) return NULL;
    a->self = self;
    a->root = root;
    a->shown = -1;
    a->cur = -1;
    a->bg = 1;
    a->level = LEVEL_START;
    s_app = a;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* three full-screen slots and a full-screen z-buffer: 7.4 MB of PSRAM */
    bool ok = true;
    for (int i = 0; i < NSLOT; i++) {
        a->slot[i].px = malloc((size_t)FB_PX * 2);
        ok &= a->slot[i].px != NULL;
    }
    a->zb = malloc((size_t)FB_PX * 2);
    ok &= a->zb != NULL;
    if (!ok) {
        v3_destroy(self, a);
        aos_ui_toast(_("Sin memoria"), 2000);
        return NULL;
    }

    int32_t v = 0;
    if (aos_hal_pref_get_i32("v3_bg", &v) && v >= 0 && v < 3) a->bg = (int)v;
    scan(a);
    build(a);

    if (!aos_hal_worker_start("visor3d", worker, a, WORKER_STACK)) {
        aos_hal_log("visor3d", "no worker");
    }
    a->timer = lv_timer_create(tick, TICK_MS, a);
    lv_display_add_event_cb(lv_display_get_default(), refr_cb, LV_EVENT_REFR_READY, a);
    return a;
}

static void v3_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return;
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), refr_cb, a);
    if (a->timer) lv_timer_delete(a->timer);
    aos_hal_worker_stop();
    unload(a);
    v3_scratch_free(&a->tscr);
    v3_mesh_free(&a->tmesh);
    s_app = NULL;
    for (int i = 0; i < NSLOT; i++) free(a->slot[i].px);
    free(a->zb);
    if (a->root) lv_obj_clean(a->root);  /* the canvas uses cv, the images the pictures */
    for (int i = 0; i < a->count; i++) free(a->files[i].px);
    free(a->cv);
    lv_free(a);
}

static bool v3_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a && a->viewing) {
        show_list(a);
        return true;
    }
    return false;
}

static void v3_hide(aos_app_t *self, void *inst)
{
    (void)self;
    ((app_t *)inst)->hidden = true;
}

static void v3_show(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    a->hidden = false;
    a->reblit = true;
}

static bool v3_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) return false;
    a->root = root;
    a->view.px = a->view.py = 0;        /* the pan was for the other shape */
    build(a);
    if (a->viewing) view_moved(a);      /* a frame of the new size */
    return true;
}

/* A cube seen from a corner: three faces, three blues. */
static const uint8_t V3_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, -14, 40, 40, 6, AIC_C_LIT(0x9ED8FF), 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, -14, 14, 28, 38, 4, AIC_C_LIT(0x3A7BD5), 255),
    AIC_RECT(AIC_CENTER, 14, 14, 28, 38, 4, AIC_C_LIT(0x1D4E9E), 255),
    AIC_END
};

static bool v3_init(aos_app_t *app)
{
    app->desc.id       = "demo.visor3d";
    app->desc.name     = "Visor 3D";
    app->desc.icon     = LV_SYMBOL_IMAGE;
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, V3_ICON, sizeof V3_ICON);
    app->desc.color_a  = 0x1C3A6B;
    app->desc.color_b  = 0x0A1428;
    app->desc.order    = 163;
    app->desc.flags    = AOS_APP_FLAG_FULLSCREEN;

    app->create  = v3_create;
    app->destroy = v3_destroy;
    app->back    = v3_back;
    app->hide    = v3_hide;
    app->show    = v3_show;
    app->resize  = v3_resize;
    return true;
}

AOS_APP_ENTRY(v3_init);
