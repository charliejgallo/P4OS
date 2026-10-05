/*
 * Claudito - a virtual pet for AmoledOS
 *
 * A tamagotchi with the Claude Code critter inside. It is all pixel art of our
 * own: perfectly square blocks without a single smoothed line.
 *
 * P4OS: the art is the OS's retro canvas (aos_retro.h), and the canvas is the
 * whole screen: 90x160 art pixels x8 = 720x1280 in portrait, 160x90 x8 =
 * 1280x720 in landscape. x8 is the biggest factor at which the watch's
 * 92-column scene still fits across 720 px (it loses one column each side);
 * everything is a third bigger than the first port's x6, and nothing is
 * letterboxed. The three layers are three rectangles of that one canvas.
 * The HUD and the bar are presented when they change; the stage is redrawn
 * every frame but only what moved on it is presented (stage_present()):
 *
 *                  portrait              landscape
 *   HUD            90x30 on top          50x90 on the left    name, day, the four bars
 *   stage          90x106                92x90 in the middle  background, character, objects
 *   bar            90x24 at the bottom   18x90 on the right   the six action buttons
 *
 * The stage keeps the watch's 92x78 frame for every position (see CL_ART_W);
 * the extra rows are more wall or sky above and more floor below.
 *
 * It is played with the finger only: the P4 board has no motion sensor and
 * no buttons. The touch layers are LVGL hit areas on a transparent object
 * exactly over the canvas, at the same art positions x the canvas's scale:
 * the events and their meaning (PRESSED, PRESSING, CLICKED, PRESS_LOST) are
 * the watch's. A USB gamepad plays it too ("The gamepad", near the end).
 *
 * Each scene's background is drawn once into a separate buffer, and again
 * when night falls or ends, and copied on every frame: redrawing the parquet
 * plank by plank fourteen times a second adds nothing.
 *
 * The same source builds two ways:
 *
 *   .so for the board       tools/build_apps.sh claudito
 *   inside the simulator    it builds itself (AOS_SIM_BUILTIN)
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_retro.h"
#include "aos_pad.h"
#include "aos_ui.h"

#include "cl_pixel.h"
#include "cl_pet.h"
#include "cl_scene.h"
#include "cl_sprites.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ~14 frames per second, nicely retro. The watch ran an lv_timer of 70 ms;
 * here the OS ticks FPS times per second of real time (71.4 ms, 2 % slower
 * than the watch's nominal period, which LVGL's timer never quite kept
 * either). FRAME_MS is the step's length for the constants counted in
 * frames; the stats clock adds each step's exact share (see step()). */
#define FPS             14
#define FRAME_MS        (1000 / FPS)
#define TRAY_ITEMS      4
#define ACT_COUNT       6
#define PARTS_MAX       24
/* What changed on the stage in one frame, as rectangles: enough for the
 * critter, its toy, the bubble, a dozen particles and the scene's clouds.
 * Beyond that they are folded into one (see rect_merge()). */
#define DIRTY_MAX       24
/* What is presented in one frame, after comparing with what the screen shows
 * (present_changed()): at most this many rectangles for the stage and a few
 * for the HUD and the bar, well inside LVGL's 32 invalid areas a refresh. */
#define OUT_STAGE_MAX   20
#define OUT_BOX_MAX     4
/* A band of changed rows may carry this many unchanged art pixels (x8 = 64
 * screen pixels each) before it is cut in two: each rectangle presented is
 * one more pass of LVGL's refresh, worth about a hundred art pixels scaled. */
#define BAND_SLACK      64

/* How long the name has to be held down to reset the day counter: ~2 s, long
 * enough not to happen by accident and short enough not to leave you wondering
 * whether it is doing anything. The little bar growing under the name is what
 * says it is. */
#define HOLD_FRAMES     (2000 / FRAME_MS)

/* The stats are stored in hundredths so the wear is integral: with integers
 * there is no need to carry floating-point accumulators around or to depend on
 * libm in an app loaded with dlopen. */
#define STAT_MAX        10000
#define STAT_PCT(v)     ((v) / 100)

/* --------------------------------------------------------------------------
 * State
 * -------------------------------------------------------------------------- */

typedef enum {
    MODE_IDLE = 0,
    MODE_FOOD_TRAY,
    MODE_TOY_TRAY,
    MODE_EATING,
    MODE_PLAYING,
    MODE_WASH,
    MODE_TICKLE,
    MODE_SLEEP,
} cl_mode_t;

typedef enum {
    ACT_FEED = 0,
    ACT_PLAY,
    ACT_WASH,
    ACT_TICKLE,
    ACT_SLEEP,
    ACT_SCENE,
} action_t;

typedef enum {
    TOY_BALL = 0,
    TOY_BALLOON,
    TOY_BUBBLES,
    TOY_BLOCKS,
} toy_t;

typedef enum {
    P_HEART = 0,
    P_SPARK,
    P_NOTE,
    P_BUBBLE,
    P_CRUMB,
    P_SUDS,
    P_ZZZ,
    P_DROP,
} pkind_t;

/* The particles move in sixteenths of a pixel: at fourteen frames a second, a
 * heart rising a whole pixel per frame flies off. */
typedef struct {
    uint8_t kind;
    uint8_t life;
    uint8_t life0;
    int16_t x, y;
    int16_t vx, vy;
} part_t;

/* A rectangle of the canvas, in art pixels */
typedef struct {
    int16_t x, y, w, h;
} box_t;

typedef struct {
    const aos_retro_t *r;   /* the OS's canvas: the three layers are views of it */
    bool   land;            /* laid out for landscape (the root is wider than tall) */
    box_t  hud_box, stage_box, bar_box;
    int    tray_x, tray_y;  /* the tray's corner, on the stage's 92x78 frame */

    /* The touch layers, on a transparent object exactly over the canvas, at
     * the art positions x the canvas's scale. */
    lv_obj_t   *touch_root;
    lv_obj_t   *stage_touch;
    lv_obj_t   *name_touch;
    lv_obj_t   *tray_btn[TRAY_ITEMS];
    lv_obj_t   *act_btn[ACT_COUNT];

    uint16_t *mem_bg;
    uint16_t *shown;        /* the canvas as the screen shows it: what was last
                               presented, to present only what differs */
    cl_buf_t  hud, stage, bar, bg;

    cl_pet_t       pet;
    cl_scene_id_t  scene;
    cl_mode_t         mode;

    int frame;
    int anim;               /* frames within the current mode */
    int hop, hop_v;         /* jump: the critter's floor goes up and down */
    int walk_target;        /* -1 = still */
    int idle_timer;
    int blink;
    int flip;               /* facing left */

    /* stats in hundredths */
    int32_t hunger, happy, clean, energy;
    int32_t last_min;       /* clock minute of the last save */
    int32_t born_day;
    int     acc_ms;
    int     ms_x;           /* remainder of the step's exact milliseconds */
    int     sec;            /* seconds lived, for the slow rhythms */
    int     save_timer;

    /* food travelling to the mouth */
    int food_kind, fly_t, fly_x0, fly_y0;

    /* toys */
    toy_t toy;
    int   ball_x, ball_y, ball_vx, ball_vy;
    int   blocks, block_t, block_fall;
    int   balloon_t;

    /* sponge */
    int  sponge_x, sponge_y, sponge_px, sponge_py;
    bool sponge_down;
    int  scrub;

    /* speech bubble */
    char msg1[20];
    char msg2[20];
    int  msg_t;

    /* sustained press on the name */
    int  hold_t;

    part_t parts[PARTS_MAX];

    /* The cached background: which scene it holds, and whether it is the
     * night version. frame_draw() redraws it when either no longer matches. */
    int    bg_scene;
    bool   bg_night;
    /* What this frame drew over the background and what the previous one
     * did, in stage pixels. Presenting both covers what appeared and what
     * went away; the rest of the stage is the same as it was. */
    cl_rect_t dirty[DIRTY_MAX];
    cl_rect_t dirty_prev[DIRTY_MAX];
    int    ndirty, ndirty_prev;
    bool   stage_full;          /* present the whole stage this frame */
#ifdef AOS_SIM
    uint16_t *verify;           /* CLAUDITO_VERIFY: the stage as presented */
#endif

    /* A USB gamepad (see "The gamepad"): its state, and the cursors it
     * moves, which only show once it has been used */
    aos_pad_t gp;
    bool   gp_shown;
    int8_t gp_act;          /* the action bar's button under the cursor */
    int8_t gp_item;         /* the tray's item under the cursor */

    bool   hud_dirty;
    bool   bar_dirty;
    bool   want_exit;
} app_t;

/* --------------------------------------------------------------------------
 * Catalogues
 * -------------------------------------------------------------------------- */

typedef struct {
    const char *const *rows;
    int  nrows;
    const char *name;
    int  hunger, happy, clean;      /* what it adds or subtracts, in hundredths */
} food_t;

static const food_t FOODS[TRAY_ITEMS] = {
        /* The food names do NOT carry N_: today they are drawn nowhere
     * -unlike the toys, which appear in the bubble- and an N_ without its
     * matching _() leaves an orphan key in the catalogue, which translates
     * nothing and never fails. If they are ever shown, they get marked.
     *
     * What IS drawn goes through cl_text(), the 5x7 font of our own: ASCII
     * 32-126, no accents, and it silently discards anything outside that. And
     * the bubbles are char[20]: 19 characters per line. */
    { CL_SPRITE(cl_spr_apple),  "MANZANA", 2000,  400,    0 },
    { CL_SPRITE(cl_spr_pizza),  "PIZZA",   3000,  800, -400 },
    { CL_SPRITE(cl_spr_cookie), "GALLETA", 1200, 1000, -200 },
    { CL_SPRITE(cl_spr_cake),   "TORTA",   1800, 1600, -700 },
};

typedef struct {
    const char *const *rows;
    int  nrows;
    const char *name;
} toy_info_t;

static const toy_info_t TOYS[TRAY_ITEMS] = {
    { CL_SPRITE(cl_spr_ball),    N_("PELOTA")   },
    { CL_SPRITE(cl_spr_balloon), N_("GLOBO")    },
    { CL_SPRITE(cl_spr_bubbles), N_("BURBUJAS") },
    { CL_SPRITE(cl_spr_dice),    N_("CUBOS")    },
};

/* --------------------------------------------------------------------------
 * Random numbers of our own: rand() would force the firmware to export it and
 * this way the background comes out the same every time when captures have to
 * be compared.
 * -------------------------------------------------------------------------- */
static uint32_t s_rng = 0x1234567;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int rnd_range(int lo, int hi)
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* --------------------------------------------------------------------------
 * What changed on the stage
 *
 * The stage is redrawn whole into the canvas every frame (a copy of the
 * cached background, then everything on top), but only what changed is
 * presented: on the board that is what the PPA or the CPU scales, and an idle
 * critter is a few percent of the stage where it used to be all of it.
 * -------------------------------------------------------------------------- */

/* Adds r to the list, fused with any rectangle it touches or nearly touches
 * (2 px): two presents over the same pixels would scale them twice. A full
 * list folds r into the first one. */
static void rect_merge(cl_rect_t *list, int *n, cl_rect_t r)
{
    for (;;) {
        bool merged = false;
        for (int i = 0; i < *n; i++) {
            cl_rect_t *q = &list[i];
            if (r.x <= q->x + q->w + 2 && q->x <= r.x + r.w + 2 &&
                r.y <= q->y + q->h + 2 && q->y <= r.y + r.h + 2) {
                int x0 = r.x < q->x ? r.x : q->x;
                int y0 = r.y < q->y ? r.y : q->y;
                int x1 = (r.x + r.w > q->x + q->w) ? r.x + r.w : q->x + q->w;
                int y1 = (r.y + r.h > q->y + q->h) ? r.y + r.h : q->y + q->h;
                r.x = (int16_t)x0;
                r.y = (int16_t)y0;
                r.w = (int16_t)(x1 - x0);
                r.h = (int16_t)(y1 - y0);
                list[i] = list[--(*n)];
                merged = true;
                break;
            }
        }
        if (!merged) {
            break;
        }
    }
    if (*n < DIRTY_MAX) {
        list[(*n)++] = r;
        return;
    }
    cl_rect_t *q = &list[0];
    int x0 = r.x < q->x ? r.x : q->x;
    int y0 = r.y < q->y ? r.y : q->y;
    int x1 = (r.x + r.w > q->x + q->w) ? r.x + r.w : q->x + q->w;
    int y1 = (r.y + r.h > q->y + q->h) ? r.y + r.h : q->y + q->h;
    q->x = (int16_t)x0;
    q->y = (int16_t)y0;
    q->w = (int16_t)(x1 - x0);
    q->h = (int16_t)(y1 - y0);
}

/* Something was drawn over the background at x, y, w x h, on the stage's
 * 92x78 frame (the coordinates everything on the stage uses). */
static void dirty(app_t *a, int x, int y, int w, int h)
{
    x += a->stage.ox;
    y += a->stage.oy;
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > a->stage.w) x1 = a->stage.w;
    if (y1 > a->stage.h) y1 = a->stage.h;
    if (x1 <= x || y1 <= y) {
        return;
    }
    cl_rect_t r = { (int16_t)x, (int16_t)y, (int16_t)(x1 - x), (int16_t)(y1 - y) };
    rect_merge(a->dirty, &a->ndirty, r);
}

/* --------------------------------------------------------------------------
 * Presenting only the pixels that changed
 *
 * The dirty rectangles above say where something was drawn over the
 * background. Most of what is drawn there is the same as the frame before:
 * an idle critter breathes and blinks, but its body stays put. So each
 * rectangle is compared, row by row, with what the screen shows (a->shown,
 * the canvas as last presented), and only the rows that differ are
 * presented, cut to the columns that differ and gathered into bands. What
 * is presented is copied into a->shown, so a->shown is always the canvas
 * once the frame's presents are made.
 *
 * It is exact: LVGL may redraw any part of the canvas at any time (the root
 * sliding in, a panel closing), and it draws r->px, which after every
 * draw() equals a->shown.
 * -------------------------------------------------------------------------- */

static int rect_area(const cl_rect_t *r)
{
    return r->w * r->h;
}

static cl_rect_t rect_union(const cl_rect_t *p, const cl_rect_t *q)
{
    int x0 = p->x < q->x ? p->x : q->x;
    int y0 = p->y < q->y ? p->y : q->y;
    int x1 = (p->x + p->w > q->x + q->w) ? p->x + p->w : q->x + q->w;
    int y1 = (p->y + p->h > q->y + q->h) ? p->y + p->h : q->y + q->h;
    return (cl_rect_t){ (int16_t)x0, (int16_t)y0, (int16_t)(x1 - x0), (int16_t)(y1 - y0) };
}

/* Adds r to a list of at most 'max'; a full list takes it into the
 * rectangle that grows least. */
static void out_add(cl_rect_t *list, int *n, int max, cl_rect_t r)
{
    if (*n < max) {
        list[(*n)++] = r;
        return;
    }
    int best = 0, grow = 0x7FFFFFFF;
    for (int i = 0; i < *n; i++) {
        cl_rect_t u = rect_union(&list[i], &r);
        int g = rect_area(&u) - rect_area(&list[i]);
        if (g < grow) {
            grow = g;
            best = i;
        }
    }
    list[best] = rect_union(&list[best], &r);
}

/* Compares the canvas rectangle x, y, w, h with what the screen shows and
 * adds the bands that differ to 'out' (canvas coordinates). */
static void diff_rect(app_t *a, int x, int y, int w, int h, cl_rect_t *out, int *n, int max)
{
    const int cw = a->r->w;
    const uint16_t *px = a->r->px;
    uint16_t *sh = a->shown;
    int bx0 = 0, bx1 = 0, by0 = -1, by1 = 0, used = 0;

    for (int yy = y; yy < y + h; yy++) {
        const uint16_t *p = px + (size_t)yy * cw + x;
        uint16_t *q = sh + (size_t)yy * cw + x;
        int x0 = 0;
        while (x0 < w && p[x0] == q[x0]) {
            x0++;
        }
        if (x0 == w) {
            continue;
        }
        int x1 = w - 1;
        while (p[x1] == q[x1]) {
            x1--;
        }
        memcpy(q + x0, p + x0, (size_t)(x1 - x0 + 1) * sizeof(uint16_t));
        int span = x1 - x0 + 1;
        if (by0 >= 0) {
            /* this row joins the band if what it adds that did not change
             * (the columns the union widens to, the unchanged rows between)
             * stays small next to what did */
            int ux0 = x0 < bx0 ? x0 : bx0, ux1 = x1 > bx1 ? x1 : bx1;
            int area = (ux1 - ux0 + 1) * (yy - by0 + 1), u = used + span;
            int slack = u / 2 > BAND_SLACK ? u / 2 : BAND_SLACK;
            if (area - u <= slack) {
                bx0 = ux0;
                bx1 = ux1;
                by1 = yy;
                used = u;
                continue;
            }
            out_add(out, n, max, (cl_rect_t){ (int16_t)(x + bx0), (int16_t)by0,
                                              (int16_t)(bx1 - bx0 + 1), (int16_t)(by1 - by0 + 1) });
        }
        bx0 = x0;
        bx1 = x1;
        by0 = by1 = yy;
        used = span;
    }
    if (by0 >= 0) {
        out_add(out, n, max, (cl_rect_t){ (int16_t)(x + bx0), (int16_t)by0,
                                          (int16_t)(bx1 - bx0 + 1), (int16_t)(by1 - by0 + 1) });
    }
}

/* Presents what changed in a box of the canvas (the HUD, the bar). */
static void present_changed(app_t *a, const box_t *b)
{
    cl_rect_t out[OUT_BOX_MAX];
    int n = 0;
    diff_rect(a, b->x, b->y, b->w, b->h, out, &n, OUT_BOX_MAX);
    for (int i = 0; i < n; i++) {
        aos_retro_present_rect(out[i].x, out[i].y, out[i].w, out[i].h);
    }
}

/* --------------------------------------------------------------------------
 * Stats and persistence
 * -------------------------------------------------------------------------- */

/* Minutes since an arbitrary epoch. There is no time() in the loader's symbol
 * table, so it is built from the struct tm: tm_yday never reaches 366, so
 * year*366 + yday always grows. */
static int32_t wall_minutes(void)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    aos_hal_time_now(&t);
    return (((int32_t)t.tm_year * 366 + t.tm_yday) * 24 + t.tm_hour) * 60 + t.tm_min;
}

static int32_t wall_days(void)
{
    struct tm t;
    memset(&t, 0, sizeof(t));
    aos_hal_time_now(&t);
    return (int32_t)t.tm_year * 366 + t.tm_yday;
}

/* Modes that survive closing the app.
 *
 * Leaving does not cancel what it was doing: if you left it sleeping, it goes
 * on sleeping (and goes on recovering energy with the app closed). Eating is a
 * two-second animation and the trays are a half-chosen menu: those two are not
 * worth restoring. */
static cl_mode_t mode_saved(cl_mode_t mode)
{
    switch (mode) {
    case MODE_SLEEP:
    case MODE_PLAYING:
    case MODE_WASH:
    case MODE_TICKLE:
        return mode;
    default:
        return MODE_IDLE;
    }
}

static void stats_save(app_t *a)
{
    aos_hal_pref_set_i32("pet_hunger", a->hunger);
    aos_hal_pref_set_i32("pet_happy",  a->happy);
    aos_hal_pref_set_i32("pet_clean",  a->clean);
    aos_hal_pref_set_i32("pet_energy", a->energy);
    aos_hal_pref_set_i32("pet_min",    a->last_min);
    aos_hal_pref_set_i32("pet_born",   a->born_day);
    aos_hal_pref_set_i32("pet_scene",  (int32_t)a->scene);
    aos_hal_pref_set_i32("pet_mode",   (int32_t)mode_saved(a->mode));
    aos_hal_pref_set_i32("pet_toy",    (int32_t)a->toy);
}

static void stats_load(app_t *a)
{
    int32_t v;

    a->hunger = a->happy = a->clean = a->energy = 8000;

    if (aos_hal_pref_get_i32("pet_hunger", &v)) a->hunger = clampi(v, 0, STAT_MAX);
    if (aos_hal_pref_get_i32("pet_happy",  &v)) a->happy  = clampi(v, 0, STAT_MAX);
    if (aos_hal_pref_get_i32("pet_clean",  &v)) a->clean  = clampi(v, 0, STAT_MAX);
    if (aos_hal_pref_get_i32("pet_energy", &v)) a->energy = clampi(v, 0, STAT_MAX);
    if (aos_hal_pref_get_i32("pet_scene",  &v)) a->scene  = (v == CL_SCENE_PARK) ? CL_SCENE_PARK
                                                                                 : CL_SCENE_HOME;

    a->born_day = wall_days();
    if (aos_hal_pref_get_i32("pet_born", &v) && v > 0 && v <= a->born_day) {
        a->born_day = v;
    }

    /* How it was left last time. It is restored later, once the buttons exist,
     * but it is needed here because it changes how time runs. */
    if (aos_hal_pref_get_i32("pet_mode", &v)) {
        a->mode = mode_saved((cl_mode_t)v);
    }
    if (aos_hal_pref_get_i32("pet_toy", &v) && v >= 0 && v < TRAY_ITEMS) {
        a->toy = (toy_t)v;
    }

    /* What happened while the app was closed. It is clamped to twelve hours:
     * coming back after a week must not find a ruined critter, and if the
     * clock was never set the sum can come out as anything. */
    a->last_min = wall_minutes();
    if (aos_hal_pref_get_i32("pet_min", &v) && v > 0) {
        int32_t elapsed = a->last_min - v;
        if (elapsed > 0) {
            if (elapsed > 12 * 60) {
                elapsed = 12 * 60;
            }
            if (a->mode == MODE_SLEEP) {
                /* it slept the whole time, at the same rate as stats_second() */
                a->energy = clampi(a->energy + elapsed * 1500, 0, STAT_MAX);
                a->hunger = clampi(a->hunger - elapsed * 20,   0, STAT_MAX);
            } else {
                a->hunger = clampi(a->hunger - elapsed * 60, 0, STAT_MAX);
                a->happy  = clampi(a->happy  - elapsed * 60, 0, STAT_MAX);
                a->clean  = clampi(a->clean  - elapsed * 30, 0, STAT_MAX);
                a->energy = clampi(a->energy - elapsed * 30, 0, STAT_MAX);
            }
        }
    }
}

/* One second's wear. The different rhythms come out of the seconds counter
 * instead of fractional accumulators. */
static void stats_second(app_t *a)
{
    a->sec++;

    if (a->mode == MODE_SLEEP) {
        a->energy = clampi(a->energy + 25, 0, STAT_MAX);
        if ((a->sec % 3) == 0) {
            a->hunger = clampi(a->hunger - 1, 0, STAT_MAX);
        }
        return;
    }

    a->hunger = clampi(a->hunger - 1, 0, STAT_MAX);
    a->happy  = clampi(a->happy  - 1, 0, STAT_MAX);
    if ((a->sec & 1) == 0) {
        a->clean  = clampi(a->clean  - 1, 0, STAT_MAX);
        a->energy = clampi(a->energy - 1, 0, STAT_MAX);
    }
    /* hungry or covered in dirt, the mood falls faster */
    if (a->hunger < 2500 || a->clean < 2500) {
        a->happy = clampi(a->happy - 1, 0, STAT_MAX);
    }

    a->hud_dirty = true;
}

/* --------------------------------------------------------------------------
 * Particles
 * -------------------------------------------------------------------------- */

static void spawn(app_t *a, pkind_t kind, int x, int y, int vx, int vy, int life)
{
    for (int i = 0; i < PARTS_MAX; i++) {
        part_t *p = &a->parts[i];
        if (p->life) {
            continue;
        }
        p->kind  = (uint8_t)kind;
        p->life  = (uint8_t)life;
        p->life0 = (uint8_t)life;
        p->x     = (int16_t)(x * 16);
        p->y     = (int16_t)(y * 16);
        p->vx    = (int16_t)vx;
        p->vy    = (int16_t)vy;
        return;
    }
}

static void parts_clear(app_t *a)
{
    memset(a->parts, 0, sizeof(a->parts));
}

static void parts_step(app_t *a)
{
    for (int i = 0; i < PARTS_MAX; i++) {
        part_t *p = &a->parts[i];
        if (!p->life) {
            continue;
        }
        p->x += p->vx;
        p->y += p->vy;

        switch (p->kind) {
        case P_CRUMB:
        case P_DROP:
            p->vy += 3;                     /* gravity */
            break;
        case P_HEART:
        case P_ZZZ:
        case P_BUBBLE:
            /* they rise evenly, zigzagging a little */
            p->x += (int16_t)(((a->frame + i) % 8) < 4 ? 1 : -1);
            break;
        default:
            break;
        }
        p->life--;
    }
}

static void draw_heart(cl_buf_t *b, int x, int y, uint16_t c, int size)
{
    if (size <= 1) {
        cl_px(b, x + 1, y, c);
        cl_px(b, x, y + 1, c);
        cl_px(b, x + 2, y + 1, c);
        return;
    }
    cl_px(b, x + 1, y, c);
    cl_px(b, x + 3, y, c);
    cl_rect(b, x, y + 1, 5, 2, c);
    cl_rect(b, x + 1, y + 3, 3, 1, c);
    cl_px(b, x + 2, y + 4, c);
}

static void draw_spark(cl_buf_t *b, int x, int y, uint16_t c, int size)
{
    cl_px(b, x, y, c);
    if (size > 0) {
        cl_px(b, x - 1, y, c);
        cl_px(b, x + 1, y, c);
        cl_px(b, x, y - 1, c);
        cl_px(b, x, y + 1, c);
    }
    if (size > 1) {
        cl_px(b, x - 2, y, c);
        cl_px(b, x + 2, y, c);
        cl_px(b, x, y - 2, c);
        cl_px(b, x, y + 2, c);
    }
}

/* A circle of radius 2 or 3 taken from the equation comes out diamond-shaped:
 * at this scale the bubbles are drawn by hand. */
static void draw_bubble(cl_buf_t *b, int x, int y, int big)
{
    const uint16_t skin = cl_rgb(0xBFEFFA);

    if (big) {
        cl_hline(b, x - 1, y - 3, 3, skin);
        cl_hline(b, x - 1, y + 3, 3, skin);
        cl_px(b, x - 2, y - 2, skin);   cl_px(b, x + 2, y - 2, skin);
        cl_px(b, x - 2, y + 2, skin);   cl_px(b, x + 2, y + 2, skin);
        cl_vline(b, x - 3, y - 1, 3, skin);
        cl_vline(b, x + 3, y - 1, 3, skin);
        cl_px(b, x - 1, y - 1, cl_rgb(0xFFFFFF));
        return;
    }
    cl_hline(b, x - 1, y - 2, 3, skin);
    cl_hline(b, x - 1, y + 2, 3, skin);
    cl_vline(b, x - 2, y - 1, 3, skin);
    cl_vline(b, x + 2, y - 1, 3, skin);
    cl_px(b, x - 1, y - 1, cl_rgb(0xFFFFFF));
}

static void parts_draw(app_t *a)
{
    for (int i = 0; i < PARTS_MAX; i++) {
        const part_t *p = &a->parts[i];
        if (!p->life) {
            continue;
        }
        int x = p->x / 16;
        int y = p->y / 16;
        int fade = p->life * 3 / (p->life0 ? p->life0 : 1);   /* 0..2 */
        dirty(a, x - 3, y - 3, 9, 11);      /* the biggest is the Z, 5x7 */

        switch (p->kind) {
        case P_HEART:
            draw_heart(&a->stage, x, y, cl_rgb(0xFF4D6D), fade);
            break;

        case P_SPARK:
            draw_spark(&a->stage, x, y, cl_rgb(fade > 1 ? 0xFFFFFF : 0xFFE066), fade);
            break;

        case P_NOTE:
            cl_rect(&a->stage, x, y + 3, 3, 2, cl_rgb(0xFFFFFF));
            cl_vline(&a->stage, x + 2, y, 4, cl_rgb(0xFFFFFF));
            cl_hline(&a->stage, x + 2, y, 3, cl_rgb(0xFFFFFF));
            break;

        case P_BUBBLE:
            draw_bubble(&a->stage, x, y, i & 1);
            break;

        case P_CRUMB:
            cl_rect(&a->stage, x, y, 2, 2, cl_rgb(0x8A5A32));
            break;

        case P_SUDS:
            cl_disc(&a->stage, x, y, fade, cl_rgb(0xFFFFFF));
            cl_px(&a->stage, x - 1, y - 1, cl_rgb(0xDFF3FA));
            break;

        case P_DROP:
            cl_px(&a->stage, x, y, cl_rgb(0x8FD4F7));
            cl_rect(&a->stage, x, y + 1, 2, 2, cl_rgb(0x67B8EA));
            break;

        case P_ZZZ:
            cl_text(&a->stage, x, y, "Z", cl_rgb(fade > 1 ? 0xFFFFFF : 0xA8BCD0));
            break;

        default:
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * Speech bubble
 * -------------------------------------------------------------------------- */

static void say(app_t *a, const char *l1, const char *l2, int frames)
{
    snprintf(a->msg1, sizeof(a->msg1), "%s", l1 ? l1 : "");
    snprintf(a->msg2, sizeof(a->msg2), "%s", l2 ? l2 : "");
    a->msg_t = frames;
}

/* Returns the box it covered, tail included */
static cl_rect_t bubble_draw(cl_buf_t *b, int cx, int bottom, const char *l1, const char *l2)
{
    int w1 = cl_text_w(l1);
    int w2 = l2[0] ? cl_text_w(l2) : 0;
    int tw = w1 > w2 ? w1 : w2;
    int w  = tw + 8;
    int h  = l2[0] ? 22 : 13;

    int x = cx - w / 2;
    if (x < cl_left(b) + 1)       x = cl_left(b) + 1;
    if (x + w > cl_right(b) - 1)  x = cl_right(b) - 1 - w;
    int y = bottom - h - 3;

    cl_round(b, x, y, w, h, 2, cl_rgb(0xFFFFFF));
    /* border: the same rounding one pixel outside, in grey */
    cl_hline(b, x + 2, y - 1, w - 4, cl_rgb(0x3A3A3C));
    cl_hline(b, x + 2, y + h, w - 4, cl_rgb(0x3A3A3C));
    cl_vline(b, x - 1, y + 2, h - 4, cl_rgb(0x3A3A3C));
    cl_vline(b, x + w, y + 2, h - 4, cl_rgb(0x3A3A3C));

    /* tail pointing at the critter */
    cl_rect(b, cx - 2, y + h, 4, 1, cl_rgb(0xFFFFFF));
    cl_rect(b, cx - 1, y + h + 1, 3, 1, cl_rgb(0xFFFFFF));
    cl_px(b, cx, y + h + 2, cl_rgb(0xFFFFFF));
    cl_px(b, cx - 2, y + h + 1, cl_rgb(0x3A3A3C));
    cl_px(b, cx + 2, y + h + 1, cl_rgb(0x3A3A3C));

    cl_text(b, x + (w - w1) / 2, y + 3, l1, cl_rgb(0x2C2C2E));
    if (l2[0]) {
        cl_text(b, x + (w - w2) / 2, y + 12, l2, cl_rgb(0x2C2C2E));
    }

    cl_rect_t box = { (int16_t)(x - 1), (int16_t)(y - 1), (int16_t)(w + 2), (int16_t)(h + 5) };
    if (cx - 3 < box.x) {               /* the tail, when the box was pushed in */
        box.w = (int16_t)(box.w + box.x - (cx - 3));
        box.x = (int16_t)(cx - 3);
    }
    if (cx + 3 >= box.x + box.w) {
        box.w = (int16_t)(cx + 4 - box.x);
    }
    return box;
}

/* --------------------------------------------------------------------------
 * HUD
 * -------------------------------------------------------------------------- */

/* A status bar w pixels long (the watch's were 15) */
static void gauge(cl_buf_t *b, int x, int y, int w, int value, uint32_t color)
{
    int pct = STAT_PCT(value);
    cl_rect(b, x, y, w, 5, cl_rgb(0x1C1C1E));
    cl_rect(b, x + 1, y + 1, w - 2, 3, cl_rgb(0x3A3A3C));

    int fill = pct * (w - 2) / 100;
    if (fill > 0) {
        uint32_t c = color;
        if (pct < 25) {
            c = 0xFF453A;
        } else if (pct < 50) {
            c = 0xFF9F0A;
        }
        cl_rect(b, x + 1, y + 1, fill, 3, cl_rgb(c));
        cl_hline(b, x + 1, y + 1, fill, cl_rgb(0xFFFFFF));
        cl_shade(b, x + 1, y + 1, fill, 1, 6);
    }
}

static void icon_food(cl_buf_t *b, int x, int y)
{
    cl_rect(b, x, y + 1, 5, 4, cl_rgb(0xE5484D));
    cl_px(b, x, y + 1, cl_rgb(0x3A3A3C));
    cl_px(b, x + 4, y + 1, cl_rgb(0x3A3A3C));
    cl_px(b, x + 2, y, cl_rgb(0x45C463));
    cl_px(b, x + 1, y + 2, cl_rgb(0xFF9F8A));
}

static void icon_heart(cl_buf_t *b, int x, int y)
{
    draw_heart(b, x, y, cl_rgb(0xFF4D6D), 2);
}

static void icon_drop(cl_buf_t *b, int x, int y)
{
    cl_px(b, x + 2, y, cl_rgb(0x67DCEA));
    cl_rect(b, x + 1, y + 1, 3, 1, cl_rgb(0x67DCEA));
    cl_rect(b, x, y + 2, 5, 2, cl_rgb(0x4A9DF5));
    cl_rect(b, x + 1, y + 4, 3, 1, cl_rgb(0x4A9DF5));
    cl_px(b, x + 1, y + 2, cl_rgb(0xFFFFFF));
}

static void icon_bolt(cl_buf_t *b, int x, int y)
{
    cl_rect(b, x + 2, y, 2, 2, cl_rgb(0xFFD60A));
    cl_rect(b, x + 1, y + 2, 3, 1, cl_rgb(0xFFD60A));
    cl_rect(b, x, y + 2, 2, 2, cl_rgb(0xFFD60A));
    cl_rect(b, x + 2, y + 3, 2, 2, cl_rgb(0xFFD60A));
}

typedef void (*icon_fn_t)(cl_buf_t *b, int x, int y);

static void hud_draw(app_t *a)
{
    cl_buf_t *b = &a->hud;
    const int nx = 3;                   /* where the name starts */

    cl_fill(b, cl_rgb(0x101014));
    if (a->land) {
        cl_vline(b, b->w - 1, 0, b->h, cl_rgb(0x2C2C2E));
    } else {
        cl_hline(b, 0, b->h - 1, b->w, cl_rgb(0x2C2C2E));
    }

    cl_text(b, nx, 4, "CLAUDITO", cl_rgb(0xD97757));

    /* charging bar while the name is held down */
    if (a->hold_t > 0) {
        int full = cl_text_w("CLAUDITO");
        int done = full * a->hold_t / HOLD_FRAMES;
        cl_rect(b, nx, 12, full, 2, cl_rgb(0x3A3A3C));
        cl_rect(b, nx, 12, done, 2, cl_rgb(0xFFD60A));
    }

    /* the day: beside the name in portrait, under it in the landscape
       column, which is only as wide as the name */
    char day[16];
    snprintf(day, sizeof(day), _("DIA %d"), (int)(wall_days() - a->born_day + 1));
    if (a->land) {
        cl_text(b, nx, 16, day, cl_rgb(0x8E8E93));
    } else {
        cl_text(b, b->w - 3 - cl_text_w(day), 4, day, cl_rgb(0x8E8E93));
    }

    /* The four bars: two by two under the name in portrait, one under the
     * other in landscape. Longer than the watch's 15 px either way, so a
     * point of hunger is easier to see go. */
    static const icon_fn_t icon[4] = { icon_food, icon_heart, icon_drop, icon_bolt };
    const int32_t value[4] = { a->hunger, a->happy, a->clean, a->energy };
    for (int k = 0; k < 4; k++) {
        int x, y, w;
        if (a->land) {
            x = 2;
            y = 34 + k * 13;
            w = b->w - 13;
        } else {
            x = 3 + (k % 2) * (b->w / 2);
            y = 16 + (k / 2) * 7;
            w = b->w / 2 - 13;
        }
        icon[k](b, x, y);
        gauge(b, x + 7, y, w, value[k], 0x30D158);
    }

    present_changed(a, &a->hud_box);
}

/* --------------------------------------------------------------------------
 * Action bar
 * -------------------------------------------------------------------------- */

static const uint32_t ACT_COLOR[ACT_COUNT] = {
    0xE5484D,   /* eat      */
    0x30D158,   /* play     */
    0x4A9DF5,   /* clean    */
    0xFFD60A,   /* tickle   */
    0xB072F0,   /* sleep    */
    0xFF9F0A,   /* scene    */
};

static bool act_is_on(const app_t *a, int act)
{
    switch (act) {
    case ACT_FEED:   return a->mode == MODE_FOOD_TRAY || a->mode == MODE_EATING;
    case ACT_PLAY:   return a->mode == MODE_TOY_TRAY  || a->mode == MODE_PLAYING;
    case ACT_WASH:   return a->mode == MODE_WASH;
    case ACT_TICKLE: return a->mode == MODE_TICKLE;
    case ACT_SLEEP:  return a->mode == MODE_SLEEP;
    default:         return false;
    }
}

/* Where button i is drawn, and the slot it answers to (both in bar pixels).
 * The buttons are 14 px across with a 1 px gap, as on the watch: a row along
 * the bottom in portrait, taller there because there is room; a column on
 * the right in landscape. The slots take the whole band, gaps and edges
 * included, so that no tap between two buttons falls through to nothing. */
static void act_box(const app_t *a, int i, box_t *btn, box_t *slot)
{
    const box_t *bar = &a->bar_box;
    int lo = (i == 0) ? 0 : 1 + i * 15;
    int hi = (i == ACT_COUNT - 1) ? (a->land ? bar->h : bar->w) : 1 + (i + 1) * 15;

    if (a->land) {
        *btn = (box_t){ 2, (int16_t)(1 + i * 15), 14, 14 };
        if (slot) *slot = (box_t){ 0, (int16_t)lo, bar->w, (int16_t)(hi - lo) };
    } else {
        *btn = (box_t){ (int16_t)(1 + i * 15), 3, 14, 18 };
        if (slot) *slot = (box_t){ (int16_t)lo, 0, (int16_t)(hi - lo), bar->h };
    }
}

static void bar_draw(app_t *a)
{
    cl_buf_t *b = &a->bar;

    cl_fill(b, cl_rgb(0x101014));
    if (a->land) {
        cl_vline(b, 0, 0, b->h, cl_rgb(0x2C2C2E));
    } else {
        cl_hline(b, 0, 0, b->w, cl_rgb(0x2C2C2E));
    }

    for (int i = 0; i < ACT_COUNT; i++) {
        box_t k;
        act_box(a, i, &k, NULL);
        int x = k.x;
        int y = k.y + (k.h - 10) / 2;   /* the icons are 9-11 rows tall */
        bool on = act_is_on(a, i);

        if (on) {
            cl_round(b, k.x, k.y, k.w, k.h, 2, cl_rgb(ACT_COLOR[i]));
            cl_shade(b, k.x, k.y, k.w, k.h, -3);
        } else {
            cl_round(b, k.x, k.y, k.w, k.h, 2, cl_rgb(0x1C1C1E));
        }
        cl_hline(b, k.x + 2, k.y, k.w - 4, cl_rgb(on ? 0xFFFFFF : 0x3A3A3C));

        switch (i) {
        case ACT_FEED:   cl_blit(b, x + 2, y, CL_SPRITE(cl_spr_apple),  false); break;
        case ACT_PLAY:   cl_blit(b, x + 2, y, CL_SPRITE(cl_spr_ball),   false); break;
        case ACT_WASH:   cl_blit(b, x + 2, y, CL_SPRITE(cl_spr_sponge), false); break;
        case ACT_TICKLE: cl_blit(b, x + 3, y, CL_SPRITE(cl_spr_hand),   false); break;
        case ACT_SLEEP:  cl_blit(b, x + 3, y, CL_SPRITE(cl_spr_moon),   false); break;
        default:
            if (a->scene == CL_SCENE_HOME) {
                cl_blit(b, x + 2, y, CL_SPRITE(cl_spr_tree_icon), false);
            } else {
                cl_blit(b, x + 2, y, CL_SPRITE(cl_spr_door), false);
            }
            break;
        }
    }

    /* the pad's cursor: a frame in the gaps round the button */
    if (a->gp_shown) {
        box_t k;
        act_box(a, a->gp_act, &k, NULL);
        cl_frame(b, k.x - 1, k.y - 1, k.w + 2, k.h + 2, cl_rgb(0xFFFFFF));
    }

    present_changed(a, &a->bar_box);
}

/* --------------------------------------------------------------------------
 * Item tray
 * -------------------------------------------------------------------------- */

/* The tray hangs from the top of the stage, over the wall, and leaves the
 * critter in sight. a->tray_x/y is its corner on the stage's frame; the
 * items are 18 px cells 21 px apart inside it, as on the watch. */
#define TRAY_W      88
#define TRAY_H      34
#define TRAY_ITEM_X 4                   /* first cell, from the tray's corner */
#define TRAY_ITEM_Y 12
#define TRAY_STEP   21
#define TRAY_SIZE   18

static int tray_item_x(const app_t *a, int i) { return a->tray_x + TRAY_ITEM_X + i * TRAY_STEP; }
static int tray_item_y(const app_t *a)        { return a->tray_y + TRAY_ITEM_Y; }

static void tray_draw(app_t *a)
{
    cl_buf_t *b = &a->stage;
    bool food = (a->mode == MODE_FOOD_TRAY);
    const int tx = a->tray_x, ty = a->tray_y;

    cl_round(b, tx, ty, TRAY_W, TRAY_H, 3, cl_rgb(0x1C1C1E));
    cl_round(b, tx + 1, ty + 1, TRAY_W - 2, TRAY_H - 2, 3, cl_rgb(0x2C2C2E));

    cl_text_center(b, tx + TRAY_W / 2, ty + 3,
                   food ? _("QUE COMEMOS?") : _("A QUE JUGAMOS?"),
                   cl_rgb(0xFFFFFF), cl_rgb(0x101014));

    for (int i = 0; i < TRAY_ITEMS; i++) {
        int x = tray_item_x(a, i);
        int y = tray_item_y(a);
        cl_round(b, x, y, TRAY_SIZE, TRAY_SIZE, 2, cl_rgb(0x3A3A3C));
        cl_round(b, x + 1, y + 1, TRAY_SIZE - 2, TRAY_SIZE - 2, 2, cl_rgb(0x4A4A50));

        if (food) {
            cl_blit(b, x + 4, y + 4, FOODS[i].rows, FOODS[i].nrows, false);
        } else {
            /* the balloon and the die are taller or narrower than the cell */
            int ox = (i == TOY_BALLOON) ? 4 : (i == TOY_BLOCKS ? 5 : 4);
            int oy = (i == TOY_BALLOON) ? 2 : (i == TOY_BLOCKS ? 5 : 4);
            cl_blit(b, x + ox, y + oy, TOYS[i].rows, TOYS[i].nrows, false);
        }
    }
    if (a->gp_shown) {
        cl_frame(b, tray_item_x(a, a->gp_item) - 1, tray_item_y(a) - 1,
                 TRAY_SIZE + 2, TRAY_SIZE + 2, cl_rgb(0xFFFFFF));
    }
}

static void tray_buttons(app_t *a, bool visible)
{
    for (int i = 0; i < TRAY_ITEMS; i++) {
        if (visible) {
            lv_obj_remove_flag(a->tray_btn[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(a->tray_btn[i]);
        } else {
            lv_obj_add_flag(a->tray_btn[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* --------------------------------------------------------------------------
 * Modes
 * -------------------------------------------------------------------------- */

static void mode_set(app_t *a, cl_mode_t mode)
{
    a->mode = mode;
    a->anim = 0;
    a->sponge_down = false;
    a->scrub = 0;
    tray_buttons(a, mode == MODE_FOOD_TRAY || mode == MODE_TOY_TRAY);
    a->bar_dirty = true;
}

/* Only the toy's state: shared by starting to play and coming back to the app
 * with a half-used toy. */
static void toy_reset(app_t *a, toy_t toy)
{
    a->toy = toy;

    switch (toy) {
    case TOY_BALL:
        a->ball_x  = 20 * 16;
        a->ball_y  = 20 * 16;
        a->ball_vx = 22;
        a->ball_vy = 0;
        break;
    case TOY_BLOCKS:
        a->blocks = 0;
        a->block_t = 0;
        a->block_fall = 0;
        break;
    case TOY_BALLOON:
        a->balloon_t = 0;
        break;
    default:
        break;
    }
}

static void toy_start(app_t *a, toy_t toy)
{
    mode_set(a, MODE_PLAYING);
    parts_clear(a);
    toy_reset(a, toy);

    char line[20];
    snprintf(line, sizeof(line), "%s!", _(TOYS[toy].name));
    say(a, line, "", 24);
    aos_hal_beep(1400, 25);
}

static void feed_start(app_t *a, int kind)
{
    if (a->hunger > 9400) {
        say(a, _("NO ENTRA"), _("MAS!"), 26);
        /* it turns its face the other way; it comes back by itself on the
           boredom routine's next decision */
        a->pet.face_dx = -3;
        aos_hal_beep(300, 90);
        mode_set(a, MODE_IDLE);
        return;
    }

    a->food_kind = kind;
    a->fly_x0 = tray_item_x(a, kind) + 4;
    a->fly_y0 = tray_item_y(a) + 4;
    a->fly_t  = 0;
    mode_set(a, MODE_EATING);
    aos_hal_beep(900, 30);
}

static void feed_finish(app_t *a)
{
    const food_t *f = &FOODS[a->food_kind];

    a->hunger = clampi(a->hunger + f->hunger, 0, STAT_MAX);
    a->happy  = clampi(a->happy  + f->happy,  0, STAT_MAX);
    a->clean  = clampi(a->clean  + f->clean,  0, STAT_MAX);
    a->hud_dirty = true;

    /* the hearts come out above the head: over the mouth they look like a pink
       moustache */
    int px, py, pw, ph;
    cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);
    for (int i = 0; i < 5; i++) {
        spawn(a, P_HEART, px + 4 + i * (pw - 8) / 4, py - 3,
              rnd_range(-6, 6), -12, rnd_range(14, 22));
    }
    say(a, _("RICO!"), "", 22);
    aos_hal_beep(1600, 40);

    a->hop_v = 9;
    mode_set(a, MODE_IDLE);
}

static void action_do(app_t *a, int act)
{
    switch (act) {
    case ACT_FEED:
        mode_set(a, a->mode == MODE_FOOD_TRAY ? MODE_IDLE : MODE_FOOD_TRAY);
        break;

    case ACT_PLAY:
        mode_set(a, a->mode == MODE_TOY_TRAY ? MODE_IDLE : MODE_TOY_TRAY);
        break;

    case ACT_WASH:
        if (a->mode == MODE_WASH) {
            mode_set(a, MODE_IDLE);
        } else {
            mode_set(a, MODE_WASH);
            a->sponge_x = CL_ART_W / 2;
            a->sponge_y = 30;
            say(a, _("FROTAME!"), "", 30);
        }
        break;

    case ACT_TICKLE:
        if (a->mode == MODE_TICKLE) {
            mode_set(a, MODE_IDLE);
        } else {
            mode_set(a, MODE_TICKLE);
            /* the hand starts beside the critter: left at 0,0 the first frame
               draws it in the screen's corner */
            a->sponge_x = a->pet.x + 20;
            a->sponge_y = CL_FLOOR_Y - 14;
            say(a, _("TOCAME LA"), _("PANZA!"), 30);
        }
        break;

    case ACT_SLEEP:
        if (a->mode == MODE_SLEEP) {
            mode_set(a, MODE_IDLE);
            say(a, _("BUEN DIA!"), "", 24);
            aos_hal_beep(1200, 40);
        } else {
            mode_set(a, MODE_SLEEP);
            parts_clear(a);
            say(a, _("A DORMIR..."), "", 26);
            aos_hal_beep(500, 120);
        }
        break;

    case ACT_SCENE:
    default:
        a->scene = (a->scene == CL_SCENE_HOME) ? CL_SCENE_PARK : CL_SCENE_HOME;
        mode_set(a, MODE_IDLE);         /* frame_draw() redraws the background */
        say(a, a->scene == CL_SCENE_PARK ? _("AL PATIO!") : _("A CASA!"), "", 24);
        aos_hal_beep(1100, 40);
        aos_hal_pref_set_i32("pet_scene", (int32_t)a->scene);
        break;
    }
    a->bar_dirty = true;
}

/* --------------------------------------------------------------------------
 * Behaviour
 * -------------------------------------------------------------------------- */

static void pet_idle_face(app_t *a)
{
    cl_pet_t *p = &a->pet;

    if (a->blink > 0) {
        p->eye = CL_EYE_BLINK;
        return;
    }
    if (STAT_PCT(a->energy) < 20) {
        p->eye = CL_EYE_SLEEP;
    } else if (STAT_PCT(a->happy) > 70) {
        p->eye = CL_EYE_HAPPY;
    } else {
        p->eye = CL_EYE_OPEN;
    }

    if (STAT_PCT(a->hunger) < 25 || STAT_PCT(a->happy) < 25) {
        p->mouth = CL_MOUTH_SAD;
    } else if (STAT_PCT(a->happy) > 70) {
        p->mouth = CL_MOUTH_TOOTH;
    } else {
        p->mouth = CL_MOUTH_SMILE;
    }
}

/* What it says by itself when bored, according to what it is short of. */
static void idle_talk(app_t *a)
{
    if (a->msg_t > 0) {
        return;
    }
    if (STAT_PCT(a->hunger) < 25) {
        say(a, _("TENGO"), _("HAMBRE"), 30);
    } else if (STAT_PCT(a->clean) < 25) {
        say(a, _("ESTOY"), _("SUCIO"), 30);
    } else if (STAT_PCT(a->energy) < 20) {
        say(a, _("QUE SUENO"), "", 30);
    } else if (STAT_PCT(a->happy) < 30) {
        say(a, _("JUGAMOS?"), "", 30);
    } else if ((rnd() & 3) == 0) {
        static const char *const hi[] = { N_("HOLA!"), N_("TODO BIEN"),
                                          N_("QUE LINDO"), N_("HOLA HOLA") };
        say(a, _(hi[rnd() % 4]), "", 26);
    }
}

static void behave_idle(app_t *a)
{
    cl_pet_t *p = &a->pet;

    if (--a->idle_timer <= 0) {
        a->idle_timer = rnd_range(20, 60);

        switch (rnd() % 5) {
        case 0:
            a->walk_target = rnd_range(24, 68);
            break;
        case 1:
            if (STAT_PCT(a->happy) > 40 && STAT_PCT(a->energy) > 25) {
                a->hop_v = 8;
                aos_hal_beep(1500, 15);
            }
            break;
        case 2:
            p->face_dx = rnd_range(-2, 2);
            break;
        case 3:
            idle_talk(a);
            break;
        default:
            break;
        }
    }

    if (a->walk_target >= 0) {
        int dx = a->walk_target - p->x;
        if (dx > 1) {
            p->x++;
            a->flip = 0;
        } else if (dx < -1) {
            p->x--;
            a->flip = 1;
        } else {
            a->walk_target = -1;
            p->step = 0;
        }
        if (a->walk_target >= 0) {
            p->step = (a->frame / 2) % 2 + 1;
            p->lean = a->flip ? -1 : 1;
        }
    } else {
        p->step = 0;
        p->lean = 0;
    }

    pet_idle_face(a);
    p->arm_l = ((a->frame / 7) % 8 == 0) ? 1 : 0;
    p->arm_r = 0;
}

static void behave_play(app_t *a)
{
    cl_pet_t *p = &a->pet;

    p->eye   = CL_EYE_HAPPY;
    p->mouth = CL_MOUTH_OPEN;

    switch (a->toy) {
    case TOY_BALL: {
        a->ball_x += a->ball_vx;
        a->ball_y += a->ball_vy;
        a->ball_vy += 5;                             /* gravity */

        if (a->ball_x < 6 * 16) {
            a->ball_x = 6 * 16;
            a->ball_vx = -a->ball_vx;
        }
        if (a->ball_x > (CL_ART_W - 17) * 16) {
            a->ball_x = (CL_ART_W - 17) * 16;
            a->ball_vx = -a->ball_vx;
        }
        if (a->ball_y > (CL_FLOOR_Y - 11) * 16) {
            a->ball_y = (CL_FLOOR_Y - 11) * 16;
            a->ball_vy = -(a->ball_vy * 3) / 4;
            if (a->ball_vy > -20) {
                a->ball_vy = -46;                    /* so it never sits completely still */
            }
            aos_hal_beep(700, 12);
        }

        int bx = a->ball_x / 16 + 5;
        if (bx > p->x + 4 && p->x < 70) {
            p->x++;
            a->flip = 0;
            p->step = (a->frame / 2) % 2 + 1;
        } else if (bx < p->x - 4 && p->x > 22) {
            p->x--;
            a->flip = 1;
            p->step = (a->frame / 2) % 2 + 1;
        } else {
            p->step = 0;
            if (a->hop == 0 && (a->frame % 12) == 0) {
                a->hop_v = 10;
                a->happy = clampi(a->happy + 60, 0, STAT_MAX);
                a->hud_dirty = true;
            }
        }
        p->arm_l = p->arm_r = (a->hop > 2) ? 2 : 1;
        break;
    }

    case TOY_BALLOON:
        a->balloon_t++;
        p->arm_r = 2;
        p->arm_l = (a->balloon_t / 6) % 2 ? 1 : 0;
        if ((a->frame % 10) == 0) {
            a->happy = clampi(a->happy + 40, 0, STAT_MAX);
            a->hud_dirty = true;
        }
        if ((a->frame % 26) == 0) {
            int hx, hy;
            cl_pet_hand_at(p, 1, &hx, &hy);
            spawn(a, P_NOTE, hx + 4, hy - 20, rnd_range(-4, 4), -10, 18);
        }
        break;

    case TOY_BUBBLES: {
        if ((a->frame % 5) == 0) {
            spawn(a, P_BUBBLE, rnd_range(8, CL_ART_W - 8), CL_FLOOR_Y - 2,
                  rnd_range(-6, 6), -rnd_range(8, 16), 40);
        }
        /* the ones passing near its head it pops */
        int px, py, pw, ph;
        cl_pet_bbox(p, &px, &py, &pw, &ph);
        for (int i = 0; i < PARTS_MAX; i++) {
            part_t *q = &a->parts[i];
            if (!q->life || q->kind != P_BUBBLE) {
                continue;
            }
            int qx = q->x / 16, qy = q->y / 16;
            if (qx > px - 2 && qx < px + pw + 2 && qy > py - 6 && qy < py + 10) {
                q->life = 0;
                for (int k = 0; k < 3; k++) {
                    spawn(a, P_SPARK, qx, qy, rnd_range(-14, 14), rnd_range(-14, 4), 8);
                }
                a->happy = clampi(a->happy + 90, 0, STAT_MAX);
                a->hud_dirty = true;
                a->hop_v = 7;
                aos_hal_beep(2000, 10);
            }
        }
        p->arm_l = p->arm_r = 2;
        break;
    }

    case TOY_BLOCKS:
    default:
        if (a->block_fall > 0) {
            a->block_fall--;
            p->mouth = CL_MOUTH_LAUGH;
            p->eye   = CL_EYE_SQUINT;
            if (a->block_fall == 0) {
                a->blocks = 0;
            }
        } else if (++a->block_t > 16) {
            a->block_t = 0;
            a->blocks++;
            a->happy = clampi(a->happy + 120, 0, STAT_MAX);
            a->hud_dirty = true;
            aos_hal_beep(900 + a->blocks * 200, 20);
            if (a->blocks >= 5) {
                a->block_fall = 22;
                int bx = p->x + 20;
                for (int i = 0; i < 6; i++) {
                    spawn(a, P_SPARK, bx, CL_FLOOR_Y - 10,
                          rnd_range(-16, 16), rnd_range(-20, -4), 12);
                }
                aos_hal_beep(300, 120);
            }
        }
        p->arm_r = 2;
        p->arm_l = 1;
        break;
    }
}

static void behave_wash(app_t *a)
{
    cl_pet_t *p = &a->pet;

    p->eye   = a->sponge_down ? CL_EYE_SQUINT : CL_EYE_OPEN;
    p->mouth = a->sponge_down ? CL_MOUTH_SMILE : CL_MOUTH_OH;
    p->step  = 0;
    p->lean  = a->sponge_down ? ((a->frame / 2) % 2 ? 1 : -1) : 0;

    if (a->scrub >= 20) {
        a->scrub = 0;
        a->clean = clampi(a->clean + 900, 0, STAT_MAX);
        a->happy = clampi(a->happy + 120, 0, STAT_MAX);
        a->hud_dirty = true;

        if (STAT_PCT(a->clean) >= 99) {
            int px, py, pw, ph;
            cl_pet_bbox(p, &px, &py, &pw, &ph);
            for (int i = 0; i < 6; i++) {
                spawn(a, P_SPARK, px + rnd_range(0, pw), py + rnd_range(0, ph / 2),
                      0, -4, 14);
            }
            say(a, _("LIMPIO!"), "", 26);
            aos_hal_beep(2200, 60);
        }
    }
}

static void behave_tickle(app_t *a)
{
    cl_pet_t *p = &a->pet;

    if (a->anim > 0) {
        a->anim--;
        p->eye   = CL_EYE_SQUINT;
        p->mouth = CL_MOUTH_LAUGH;
        p->lean  = (a->frame % 2) ? 2 : -2;
        p->blush = true;
        p->arm_l = p->arm_r = 2;
    } else {
        p->blush = false;
        p->lean  = 0;
        pet_idle_face(a);
        p->arm_l = p->arm_r = 0;
    }
}

static void behave_sleep(app_t *a)
{
    cl_pet_t *p = &a->pet;

    p->eye    = CL_EYE_SLEEP;
    p->mouth  = CL_MOUTH_FLAT;
    p->step   = 0;
    p->lean   = 0;
    p->arm_l  = p->arm_r = 0;
    /* it breathes: it squashes and stretches slowly */
    p->squash = ((a->frame / 10) % 2) ? 4 : 3;

    if ((a->frame % 20) == 0) {
        int px, py, pw, ph;
        cl_pet_bbox(p, &px, &py, &pw, &ph);
        spawn(a, P_ZZZ, px + pw - 4, py - 4, 2, -6, 26);
    }
}

/* --------------------------------------------------------------------------
 * Objects drawn on top of the stage
 * -------------------------------------------------------------------------- */

static void draw_flies(app_t *a)
{
    /* the flies only appear when it is filthy: they communicate the state
     * better than the cleanliness bar */
    if (STAT_PCT(a->clean) >= 30 || a->mode == MODE_SLEEP) {
        return;
    }
    int px, py, pw, ph;
    cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);

    dirty(a, px + pw / 2 - 17, py - 6, 35, 14);
    for (int i = 0; i < 3; i++) {
        int t = a->frame * 2 + i * 40;
        int x = px + pw / 2 + ((t / 3) % 30) - 15;
        int y = py - 4 + ((t / 5 + i * 3) % 10);
        cl_px(&a->stage, x, y, cl_rgb(0x1B1210));
        cl_px(&a->stage, x + 1, y, cl_rgb(0x1B1210));
        cl_px(&a->stage, x + ((a->frame & 1) ? 1 : -1), y - 1, cl_rgb(0x8A8A90));
    }
}

static void draw_toy(app_t *a)
{
    cl_pet_t *p = &a->pet;

    switch (a->toy) {
    case TOY_BALL:
        dirty(a, a->ball_x / 16 - 1, a->ball_y / 16 - 1, 13, 13);
        dirty(a, a->ball_x / 16, CL_FLOOR_Y, 11, 1);
        cl_shade(&a->stage, a->ball_x / 16 + 1, CL_FLOOR_Y, 9, 1, -4);
        cl_blit(&a->stage, a->ball_x / 16, a->ball_y / 16,
                CL_SPRITE(cl_spr_ball), false);
        break;

    case TOY_BALLOON: {
        int hx, hy;
        cl_pet_hand_at(p, 1, &hx, &hy);
        int sway = ((a->balloon_t / 5) % 4) - 2;
        int bx = hx + sway - 3;
        int by = hy - 26;

        dirty(a, bx - 3, by - 1, 17, hy - by + 3);
        cl_blit(&a->stage, bx, by, CL_SPRITE(cl_spr_balloon), false);
        /* the string goes from the knot to the hand, with a sag that follows
         * the swing */
        for (int y = by + 11; y <= hy; y++) {
            int t = y - (by + 11);
            cl_px(&a->stage, bx + 4 - (sway * t) / 14, y, cl_rgb(0xFFFFFF));
        }
        break;
    }

    case TOY_BLOCKS: {
        static const uint32_t col[5] = { 0xE5484D, 0x4A9DF5, 0xFFD60A, 0x45C463, 0xB072F0 };
        int bx = p->x + 18;
        if (bx > CL_ART_W - 14) {
            bx = CL_ART_W - 14;
        }
        dirty(a, bx - 11, CL_FLOOR_Y - 31, 34, 32);     /* the tower, wobble included */
        for (int i = 0; i < a->blocks && i < 5; i++) {
            int wob = 0;
            if (a->block_fall > 0) {
                wob = (i + 1) * ((a->block_fall / 2 % 2) ? 2 : -2);
            } else if (a->blocks >= 4) {
                wob = ((a->frame / 3) % 2) ? i : -i;
            }
            int y = CL_FLOOR_Y - 6 - i * 6;
            cl_rect(&a->stage, bx + wob, y, 12, 6, cl_rgb(col[i]));
            cl_shade(&a->stage, bx + wob + 1, y + 1, 10, 1, 6);
            cl_shade(&a->stage, bx + wob + 1, y + 4, 10, 1, -5);
            cl_frame(&a->stage, bx + wob, y, 12, 6, cl_rgb(0x1B1210));
        }
        break;
    }

    default:
        break;
    }
}

static void draw_food_flight(app_t *a)
{
    /* After the bite the food no longer exists: if it went on being drawn at
     * the destination a sprite would stay stuck on its face while it chews. */
    if (a->fly_t > 12) {
        return;
    }

    const food_t *f = &FOODS[a->food_kind];
    int mx, my;
    cl_pet_mouth_at(&a->pet, &mx, &my);

    int total = 12;
    int t = a->fly_t;
    int x = a->fly_x0 + (mx - 5 - a->fly_x0) * t / total;
    int y = a->fly_y0 + (my - 4 - a->fly_y0) * t / total;

    dirty(a, x - 1, y - 1, 13, 13);
    cl_blit(&a->stage, x, y, f->rows, f->nrows, false);
}

static void draw_sponge(app_t *a)
{
    int x = a->sponge_x - 5;
    int y = a->sponge_y - 7;      /* the sponge's body starts on row 4 */

    dirty(a, x - 2, y - 1, 15, 14);
    if (a->sponge_down) {
        cl_shade(&a->stage, x - 1, y + 3, 13, 9, 4);
    }
    cl_blit(&a->stage, x, y, CL_SPRITE(cl_spr_sponge), false);
}

static void draw_hand(app_t *a)
{
    dirty(a, a->sponge_x - 5, a->sponge_y, 11, 11);
    cl_blit(&a->stage, a->sponge_x - 4, a->sponge_y + 1, CL_SPRITE(cl_spr_hand),
            a->sponge_x > a->pet.x);
}

static void day_counter_reset(app_t *a)
{
    a->born_day = wall_days();
    a->last_min = wall_minutes();
    stats_save(a);                  /* so it is not lost if the power goes */

    a->hud_dirty = true;
    a->hop_v = 9;
    say(a, _("DIA 1!"), _("DE NUEVO"), 28);
    aos_hal_beep(2200, 70);

    int px, py, pw, ph;
    cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);
    for (int i = 0; i < 6; i++) {
        spawn(a, P_SPARK, px + rnd_range(0, pw), py + rnd_range(0, ph / 2),
              rnd_range(-10, 10), -rnd_range(4, 12), 14);
    }
}

/* --------------------------------------------------------------------------
 * One frame
 * -------------------------------------------------------------------------- */

/* The critter, with the jump applied to the floor. On the watch frame_draw()
 * did this; here drawing happens once per displayed frame and not per step,
 * so the logic (the finger's hit test, where the hearts come out) sets it at
 * the end of each step instead. */
static void pet_place(app_t *a)
{
    a->pet.y = CL_FLOOR_Y - a->hop;
    a->pet.dirt = 3 - clampi(STAT_PCT(a->clean) / 25, 0, 3);
}

#ifdef AOS_SIM
/* CLAUDITO_VERIFY=1 (simulator): keeps the stage as the screen shows it -only
 * the presented rectangles are copied in- and counts the pixels of the stage
 * that differ from it after a present. Anything but 0 is something drawn
 * that the dirty rectangles missed. */
static void verify_present(app_t *a, const cl_rect_t *list, int n, int ox, int oy, bool full)
{
    static int checked = -1;
    if (checked < 0) {
        checked = getenv("CLAUDITO_VERIFY") ? 1 : 0;
    }
    if (!checked) {
        return;
    }
    const cl_buf_t *s = &a->stage;
    if (!a->verify) {
        a->verify = (uint16_t *)calloc((size_t)s->w * s->h, sizeof(uint16_t));
        full = true;
        if (!a->verify) {
            return;
        }
    }
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            bool in = full;
            for (int i = 0; i < n && !in; i++) {
                in = x + ox >= list[i].x && x + ox < list[i].x + list[i].w &&
                     y + oy >= list[i].y && y + oy < list[i].y + list[i].h;
            }
            if (in) {
                a->verify[y * s->w + x] = s->px[y * s->stride + x];
            }
        }
    }
    int wrong = 0, area = 0;
    for (int y = 0; y < s->h; y++) {
        for (int x = 0; x < s->w; x++) {
            wrong += a->verify[y * s->w + x] != s->px[y * s->stride + x];
        }
    }
    for (int i = 0; i < n; i++) {
        area += list[i].w * list[i].h;
    }
    static int frames, sum_area;
    frames++;
    sum_area += full ? s->w * s->h : area;
    if (wrong || (frames % 70) == 0) {
        aos_hal_log("claudito", "verify: %d px missed; presented %d%% of the stage on average",
                    wrong, sum_area * 100 / (frames * s->w * s->h));
    }
}
#endif

/* Presents what changed where this frame and the previous one drew over
 * the background, or anywhere on the stage when the background itself
 * changed: only the pixels that differ from the screen (diff_rect()). */
static void stage_present(app_t *a)
{
    const box_t *g = &a->stage_box;
    cl_rect_t all[DIRTY_MAX];
    cl_rect_t out[OUT_STAGE_MAX];
    int n = 0, nout = 0;

    if (a->stage_full) {
        /* a new background changes nearly all of it: a few big rectangles */
        diff_rect(a, g->x, g->y, g->w, g->h, out, &nout, OUT_BOX_MAX);
    } else {
        for (int i = 0; i < a->ndirty; i++)      rect_merge(all, &n, a->dirty[i]);
        for (int i = 0; i < a->ndirty_prev; i++) rect_merge(all, &n, a->dirty_prev[i]);
        for (int i = 0; i < n; i++) {
            diff_rect(a, g->x + all[i].x, g->y + all[i].y, all[i].w, all[i].h,
                      out, &nout, OUT_STAGE_MAX);
        }
    }
    for (int i = 0; i < nout; i++) {
        aos_retro_present_rect(out[i].x, out[i].y, out[i].w, out[i].h);
    }
#ifdef AOS_SIM
    verify_present(a, out, nout, g->x, g->y, false);
#endif

    memcpy(a->dirty_prev, a->dirty, sizeof(a->dirty));
    a->ndirty_prev = a->ndirty;
    a->ndirty = 0;
    a->stage_full = false;
}

static void frame_draw(app_t *a)
{
    cl_buf_t *b = &a->stage;

    /* the background, once per scene and once more when night falls */
    bool night = (a->mode == MODE_SLEEP);
    if ((int)a->scene != a->bg_scene || night != a->bg_night) {
        cl_scene_draw(&a->bg, a->scene, night);
        a->bg_scene   = (int)a->scene;
        a->bg_night   = night;
        a->stage_full = true;
    }

    cl_copy(b, &a->bg);

    cl_rect_t anim[16];
    int nanim = cl_scene_anim(b, a->scene, a->frame, night, anim, 16);
    for (int i = 0; i < nanim; i++) {
        dirty(a, anim[i].x, anim[i].y, anim[i].w, anim[i].h);
    }

    cl_pet_draw(b, &a->pet);
    {
        /* its arms go 3 px past the box when raised, the shadow 2 below */
        int px, py, pw, ph;
        cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);
        dirty(a, px - 4, py - 5, pw + 8, a->pet.y + 3 - (py - 5));
    }

    draw_flies(a);

    switch (a->mode) {
    case MODE_EATING:
        draw_food_flight(a);
        break;
    case MODE_PLAYING:
        draw_toy(a);
        break;
    case MODE_WASH:
        draw_sponge(a);
        break;
    case MODE_TICKLE:
        draw_hand(a);
        break;
    default:
        break;
    }

    parts_draw(a);

    if (a->mode == MODE_FOOD_TRAY || a->mode == MODE_TOY_TRAY) {
        tray_draw(a);
        dirty(a, a->tray_x, a->tray_y, TRAY_W, TRAY_H);
    } else if (a->msg_t > 0) {
        int px, py, pw, ph;
        cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);
        cl_rect_t r = bubble_draw(b, px + pw / 2, py - 2, a->msg1, a->msg2);
        dirty(a, r.x, r.y, r.w, r.h);
    }

    stage_present(a);
}

static void pad_step(app_t *a);

static void step(void *user)
{
    app_t *a = (app_t *)user;

    if (a->want_exit) {
        /* aos_ui_back() destroys the app: after this call 'a' no longer exists
         * and nothing else can be touched (the OS's tick sees it ended) */
        a->want_exit = false;
        aos_ui_back();
        return;
    }

    a->frame++;
    pad_step(a);

    /* the stats clock: each step's exact share of a second (71 or 72 ms), so
       the wear is by the wall clock whatever the frame rate */
    a->ms_x += 1000;
    a->acc_ms += a->ms_x / FPS;
    a->ms_x %= FPS;
    while (a->acc_ms >= 1000) {
        a->acc_ms -= 1000;
        stats_second(a);
    }
    if (++a->save_timer >= (20000 / FRAME_MS)) {
        a->save_timer = 0;
        a->last_min = wall_minutes();
        stats_save(a);
    }

    /* sustained press on the name */
    if (a->hold_t > 0) {
        a->hold_t++;
        a->hud_dirty = true;
        if (a->hold_t >= HOLD_FRAMES) {
            a->hold_t = 0;          /* not repeated while the finger stays down */
            day_counter_reset(a);
        }
    }

    /* blinking */
    if (a->blink > 0) {
        a->blink--;
    } else if ((rnd() % 40) == 0) {
        a->blink = 2;
    }

    /* jump */
    if (a->hop > 0 || a->hop_v > 0) {
        a->hop += a->hop_v;
        a->hop_v -= 3;
        if (a->hop <= 0) {
            a->hop = 0;
            a->hop_v = 0;
            a->pet.squash = 3;
        }
    } else if (a->pet.squash > 0 && a->mode != MODE_SLEEP) {
        a->pet.squash--;
    }
    if (a->hop > 2 && a->mode != MODE_SLEEP) {
        a->pet.squash = -2;
    }

    if (a->msg_t > 0) {
        a->msg_t--;
    }

    switch (a->mode) {
    case MODE_EATING:
        a->fly_t++;
        if (a->fly_t == 12) {
            a->pet.mouth = CL_MOUTH_BIG;
            a->pet.eye   = CL_EYE_SQUINT;
            aos_hal_beep(600, 40);
            int mx, my;
            cl_pet_mouth_at(&a->pet, &mx, &my);
            for (int i = 0; i < 4; i++) {
                spawn(a, P_CRUMB, mx + rnd_range(-4, 4), my,
                      rnd_range(-10, 10), -8, 12);
            }
        } else if (a->fly_t > 12 && a->fly_t < 26) {
            a->pet.mouth = CL_MOUTH_CHEW;
            a->pet.step  = (a->fly_t / 2) % 2;
            a->pet.eye   = CL_EYE_HAPPY;
        } else if (a->fly_t >= 26) {
            feed_finish(a);
        }
        break;

    case MODE_PLAYING:
        behave_play(a);
        break;

    case MODE_WASH:
        behave_wash(a);
        break;

    case MODE_TICKLE:
        behave_tickle(a);
        break;

    case MODE_SLEEP:
        behave_sleep(a);
        break;

    case MODE_FOOD_TRAY:
    case MODE_TOY_TRAY:
        pet_idle_face(a);
        a->pet.step = 0;
        break;

    case MODE_IDLE:
    default:
        behave_idle(a);
        break;
    }

    parts_step(a);
    pet_place(a);
}

/* Once per displayed frame (after one step, or several if the OS had to
 * catch up): the stage always, the HUD and the bar when they changed. Each
 * layer is presented on its own, and the stage only where something moved,
 * so the OS scales only what changed. */
static void draw(void *user)
{
    app_t *a = (app_t *)user;

    frame_draw(a);

    if (a->hud_dirty) {
        a->hud_dirty = false;
        hud_draw(a);
    }
    if (a->bar_dirty) {
        a->bar_dirty = false;
        bar_draw(a);
    }
}


/* --------------------------------------------------------------------------
 * Input
 * -------------------------------------------------------------------------- */

/* Converts the finger's point to stage art coordinates, on the watch's 92x78
 * frame. It is measured against the object and not against the screen because
 * when the app opens the runtime slides the root: during that animation the
 * two do not coincide. (The same as aos_retro_to_canvas(), which takes the
 * root where it was at begin().) */
static bool touch_point(app_t *a, int *ax, int *ay)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return false;
    }
    lv_point_t point;
    lv_indev_get_point(indev, &point);

    lv_area_t area;
    lv_obj_get_coords(a->stage_touch, &area);
    *ax = (point.x - area.x1) / a->r->scale - a->stage.ox;
    *ay = (point.y - area.y1) / a->r->scale - a->stage.oy;
    return true;
}

static void tickle_hit(app_t *a)
{
    a->anim = 12;
    a->happy = clampi(a->happy + 250, 0, STAT_MAX);
    a->energy = clampi(a->energy - 30, 0, STAT_MAX);
    a->hud_dirty = true;

    int px, py, pw, ph;
    cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);
    for (int i = 0; i < 3; i++) {
        spawn(a, P_HEART, px + rnd_range(2, pw - 4), py, rnd_range(-8, 8), -11,
              rnd_range(12, 20));
    }
    static const char *const laugh[] = { N_("JI JI JI!"), N_("JA JA!"),
                                         N_("BASTA!"), N_("MAS!") };
    say(a, _(laugh[rnd() % 4]), "", 16);
    aos_hal_beep(1800 + (int)(rnd() % 400), 25);
}

/* The finger (or the pad, see "The gamepad") at x, y on the stage. */
static void stage_at(app_t *a, lv_event_code_t code, int x, int y)
{
    int px, py, pw, ph;
    cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);
    bool on_pet = (x >= px - 2 && x <= px + pw + 2 && y >= py - 2 && y <= py + ph + 2);

    switch (a->mode) {
    case MODE_WASH:
        a->sponge_px = a->sponge_x;
        a->sponge_py = a->sponge_y;
        a->sponge_x  = x;
        a->sponge_y  = y;

        if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
            a->sponge_down = false;
            break;
        }
        a->sponge_down = true;

        if (on_pet) {
            int dx = a->sponge_x - a->sponge_px;
            int dy = a->sponge_y - a->sponge_py;
            int moved = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
            if (moved > 0) {
                a->scrub += moved;
                if ((a->frame % 2) == 0) {
                    spawn(a, P_SUDS, x + rnd_range(-4, 4), y + rnd_range(-4, 2),
                          rnd_range(-6, 6), -rnd_range(2, 8), 12);
                }
                if ((a->frame % 7) == 0) {
                    aos_hal_beep(400 + (int)(rnd() % 300), 8);
                }
            }
        }
        break;

    case MODE_TICKLE:
        a->sponge_x = x;
        a->sponge_y = y;
        if (code == LV_EVENT_PRESSED && on_pet) {
            tickle_hit(a);
        }
        break;

    case MODE_SLEEP:
        if (code == LV_EVENT_PRESSED) {
            action_do(a, ACT_SLEEP);
        }
        break;

    case MODE_FOOD_TRAY:
    case MODE_TOY_TRAY:
        /* touching below the tray closes it */
        if (code == LV_EVENT_PRESSED && y > a->tray_y + TRAY_H + 4) {
            mode_set(a, MODE_IDLE);
        }
        break;

    default:
        if (code == LV_EVENT_PRESSED && on_pet) {
            /* a stroke: it always adds something */
            a->happy = clampi(a->happy + 120, 0, STAT_MAX);
            a->hud_dirty = true;
            a->pet.eye = CL_EYE_LOVE;
            a->hop_v = 7;
            spawn(a, P_HEART, px + pw / 2, py - 2, rnd_range(-5, 5), -10, 18);
            aos_hal_beep(1700, 20);
            if ((rnd() & 1) == 0) {
                say(a, "MMM...", "", 14);
            }
        }
        break;
    }
}

static void stage_event(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    int x, y;
    if (touch_point(a, &x, &y)) {
        stage_at(a, lv_event_get_code(event), x, y);
    }
}

/* A swipe to the right over the stage leaves, as on the watch, except while
 * the finger is at work (rubbing and tickling are drags). LVGL sends the
 * gesture to the first ancestor of the pressed object without
 * GESTURE_BUBBLE, so the stage's touch layer has it taken off. P4OS's own
 * back gesture works too: it is a drag that STARTS in the screen's left
 * 28 px, and a rub starts on the critter, which never walks that far left. */
static void stage_gesture(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    lv_indev_t *indev = lv_indev_active();

    if (!indev || a->mode == MODE_WASH || a->mode == MODE_TICKLE) {
        return;
    }
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) {
        return;
    }

    /* on release, LVGL sends a CLICKED to the object below anyway */
    lv_indev_wait_release(indev);

    /* same rule as claudito_back(): the tray closes, not the action */
    if (a->mode == MODE_FOOD_TRAY || a->mode == MODE_TOY_TRAY) {
        mode_set(a, MODE_IDLE);
    } else {
        a->want_exit = true;        /* it closes on the next frame, not here */
    }
}

/* Holding the name down resets the day counter. It is counted in frames and
 * not with LV_EVENT_LONG_PRESSED because LVGL's threshold belongs to the input
 * device (global, changing it would affect the whole system) and because this
 * way how much is left can be drawn. */
static void name_event(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);

    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        a->hold_t = 1;
        a->hud_dirty = true;
        return;
    }

    /* releasing early: count so it is of some use, since pressing the name
       does nothing else */
    if (a->hold_t > 0) {
        say(a, _("MANTENE PARA"), _("REINICIAR"), 30);
    }
    a->hold_t = 0;
    a->hud_dirty = true;
}

static void action_event(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    int act = (int)(lv_uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));
    action_do(a, act);
}

static void tray_event(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    int idx = (int)(lv_uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(event));

    if (a->mode == MODE_FOOD_TRAY) {
        feed_start(a, idx);
    } else if (a->mode == MODE_TOY_TRAY) {
        toy_start(a, (toy_t)idx);
    }
}

/* --------------------------------------------------------------------------
 * The gamepad
 *
 * A USB pad on the board's host (aos_pad.h) does what the finger does, with
 * a cursor that only shows once the pad is used:
 *
 *  - Left and right go along the action bar, A presses the button there.
 *    With a tray open they go along the tray instead, A picks and B closes.
 *  - Washing and tickling, the d-pad or the stick moves the sponge or the
 *    hand over the stage; A held is the finger on the glass (rub by moving
 *    with it held, a press on the belly tickles), and B puts the sponge or
 *    the hand away.
 *  - B otherwise strokes the critter, and wakes it when it sleeps.
 *  - START held is the finger held on the name: two seconds reset the days.
 * -------------------------------------------------------------------------- */

#define PAD_DEAD        6000        /* the stick's dead zone, of 32767 */

/* The sponge or the hand, moved by the pad: up to 3 art pixels a step with
 * the stick all the way, 2 on the d-pad. */
static void pad_pointer(app_t *a, const aos_pad_t *p)
{
    int sx = p->x, sy = p->y, dx, dy;
    bool stick = sx <= -PAD_DEAD || sx >= PAD_DEAD || sy <= -PAD_DEAD || sy >= PAD_DEAD;
    if (!stick && !(p->held & (AOS_PAD_A | AOS_PAD_DIRS)) && !((p->pressed | p->released) & AOS_PAD_A)) {
        return;     /* the pad is idle: the finger may be at work */
    }
    if (sx <= -PAD_DEAD || sx >= PAD_DEAD) dx = sx * 3 / 32767;
    else dx = (aos_pad_held(p, AOS_PAD_RIGHT) - aos_pad_held(p, AOS_PAD_LEFT)) * 2;
    if (sy <= -PAD_DEAD || sy >= PAD_DEAD) dy = sy * 3 / 32767;
    else dy = (aos_pad_held(p, AOS_PAD_DOWN) - aos_pad_held(p, AOS_PAD_UP)) * 2;

    int x = clampi(a->sponge_x + dx, cl_left(&a->stage) + 5, cl_right(&a->stage) - 6);
    int y = clampi(a->sponge_y + dy, cl_top(&a->stage) + 8, cl_bottom(&a->stage) - 8);

    if (aos_pad_held(p, AOS_PAD_A)) {
        stage_at(a, aos_pad_pressed(p, AOS_PAD_A) ? LV_EVENT_PRESSED : LV_EVENT_PRESSING, x, y);
    } else {
        if (aos_pad_pressed(p, AOS_PAD_A) || (p->released & AOS_PAD_A)) {
            stage_at(a, LV_EVENT_RELEASED, x, y);     /* a press shorter than a step too */
        }
        a->sponge_x = x;
        a->sponge_y = y;
        a->sponge_down = false;
    }
}

static void pad_step(app_t *a)
{
    aos_pad_t *p = &a->gp;

    aos_pad_update(p, lv_tick_get());
    /* in the background, or under the app switcher, the pad is not ours */
    if (!p->connected || !lv_obj_is_visible(a->r->view)) {
        return;
    }
    if (!a->gp_shown) {
        /* the first press only shows where the cursor is */
        if (p->pressed) {
            a->gp_shown = true;
            a->bar_dirty = true;
        }
        return;
    }

    /* START held: the name held down (step() counts the frames) */
    if (aos_pad_pressed(p, AOS_PAD_START)) {
        a->hold_t = 1;
        a->hud_dirty = true;
    } else if ((p->released & AOS_PAD_START) && a->hold_t > 0) {
        say(a, _("MANTENE PARA"), _("REINICIAR"), 30);
        a->hold_t = 0;
        a->hud_dirty = true;
    }

    switch (a->mode) {
    case MODE_WASH:
    case MODE_TICKLE:
        if (aos_pad_pressed(p, AOS_PAD_B)) {
            mode_set(a, MODE_IDLE);
            return;
        }
        pad_pointer(a, p);
        return;

    case MODE_FOOD_TRAY:
    case MODE_TOY_TRAY:
        if (aos_pad_repeat(p, AOS_PAD_LEFT) && a->gp_item > 0) a->gp_item--;
        if (aos_pad_repeat(p, AOS_PAD_RIGHT) && a->gp_item < TRAY_ITEMS - 1) a->gp_item++;
        if (aos_pad_pressed(p, AOS_PAD_A)) {
            if (a->mode == MODE_FOOD_TRAY) feed_start(a, a->gp_item);
            else toy_start(a, (toy_t)a->gp_item);
        } else if (aos_pad_pressed(p, AOS_PAD_B)) {
            mode_set(a, MODE_IDLE);
        }
        return;

    default:
        break;
    }

    int act = a->gp_act;
    if (aos_pad_repeat(p, AOS_PAD_LEFT) && act > 0) act--;
    if (aos_pad_repeat(p, AOS_PAD_RIGHT) && act < ACT_COUNT - 1) act++;
    if (act != a->gp_act) {
        a->gp_act = (int8_t)act;
        a->bar_dirty = true;
    }
    if (aos_pad_pressed(p, AOS_PAD_A)) {
        action_do(a, a->gp_act);
        /* the hand and the sponge start where the action put them */
    } else if (aos_pad_pressed(p, AOS_PAD_B)) {
        if (a->mode == MODE_SLEEP) {
            action_do(a, ACT_SLEEP);
        } else {
            int px, py, pw, ph;
            cl_pet_bbox(&a->pet, &px, &py, &pw, &ph);
            stage_at(a, LV_EVENT_PRESSED, px + pw / 2, py + ph / 2);
        }
    }
}

/* --------------------------------------------------------------------------
 * Building the UI
 * -------------------------------------------------------------------------- */

static lv_obj_t *hit_area(lv_obj_t *parent, int x, int y, int w, int h,
                          lv_event_cb_t cb, uint32_t codes, void *user, int index)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(obj, (void *)(lv_uintptr_t)index);

    if (codes & 1) lv_obj_add_event_cb(obj, cb, LV_EVENT_PRESSED, user);
    if (codes & 2) lv_obj_add_event_cb(obj, cb, LV_EVENT_PRESSING, user);
    if (codes & 4) lv_obj_add_event_cb(obj, cb, LV_EVENT_RELEASED, user);
    if (codes & 8) lv_obj_add_event_cb(obj, cb, LV_EVENT_CLICKED, user);
    /* PRESS_LOST arrives when the finger moves off the object without
       releasing: for a sustained press that is as much a cancellation as
       releasing */
    if (codes & 16) lv_obj_add_event_cb(obj, cb, LV_EVENT_PRESS_LOST, user);
    return obj;
}

/* A box of the canvas (art pixels) to the touch root's
 * coordinates, which are the screen's from the canvas's corner. */
static lv_obj_t *hit_box(app_t *a, box_t box, lv_event_cb_t cb, uint32_t codes, int index)
{
    int k = a->r->scale;
    return hit_area(a->touch_root, box.x * k, box.y * k, box.w * k, box.h * k,
                    cb, codes, a, index);
}

/* The touch layers, over the canvas. */
static void build_touch(app_t *a, lv_obj_t *root)
{
    const aos_retro_t *r = a->r;

    a->touch_root = lv_obj_create(root);
    lv_obj_remove_style_all(a->touch_root);
    lv_obj_set_pos(a->touch_root, r->x, r->y);
    lv_obj_set_size(a->touch_root, r->w * r->scale, r->h * r->scale);
    lv_obj_remove_flag(a->touch_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->touch_root, LV_OBJ_FLAG_CLICKABLE);

    /* the stage's touch layer */
    a->stage_touch = hit_box(a, a->stage_box, stage_event, 1 | 2 | 4 | 16, 0);
    lv_obj_remove_flag(a->stage_touch, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(a->stage_touch, stage_gesture, LV_EVENT_GESTURE, a);

    /* the name and the day counter: holding it down resets the days. It
       stops short of the bars (in portrait they start on row 16 of the HUD,
       in landscape on row 34). */
    box_t name = a->hud_box;
    name.h = a->land ? 26 : 14;
    a->name_touch = hit_box(a, name, name_event, 1 | 4 | 16, 0);

    for (int i = 0; i < TRAY_ITEMS; i++) {
        box_t cell = {
            (int16_t)(a->stage_box.x + a->stage.ox + tray_item_x(a, i)),
            (int16_t)(a->stage_box.y + a->stage.oy + tray_item_y(a)),
            TRAY_SIZE, TRAY_SIZE,
        };
        a->tray_btn[i] = hit_box(a, cell, tray_event, 8, i);
        lv_obj_add_flag(a->tray_btn[i], LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < ACT_COUNT; i++) {
        box_t btn, slot;
        act_box(a, i, &btn, &slot);
        slot.x = (int16_t)(slot.x + a->bar_box.x);
        slot.y = (int16_t)(slot.y + a->bar_box.y);
        a->act_btn[i] = hit_box(a, slot, action_event, 8, i);
    }
}

/* The canvas, in art pixels: 90 x 160 upright, 160 x 90 on its side. */
#define CANVAS_SHORT    90
#define CANVAS_LONG     160
/* How far down the stage the watch's 92x78 frame starts: the rows above it
 * are more wall (or sky), the ones below more floor. The critter's feet end
 * up about three quarters of the way down either way. */
#define STAGE_OY_PORT   18
#define STAGE_OY_LAND   8

static void layout(app_t *a, bool land)
{
    a->land = land;
    if (land) {
        /* HUD column | stage | bar column: 52 + 90 + 18 = 160. The stage is
         * as wide as in portrait, the HUD as wide as "CLAUDITO" and a margin. */
        a->hud_box   = (box_t){ 0,   0, 52, CANVAS_SHORT };
        a->stage_box = (box_t){ 52,  0, CANVAS_SHORT, CANVAS_SHORT };
        a->bar_box   = (box_t){ 142, 0, 18, CANVAS_SHORT };
    } else {
        /* HUD, stage, bar, top to bottom: 30 + 106 + 24 = 160 */
        a->hud_box   = (box_t){ 0, 0,   CANVAS_SHORT, 30 };
        a->stage_box = (box_t){ 0, 30,  CANVAS_SHORT, 106 };
        a->bar_box   = (box_t){ 0, 136, CANVAS_SHORT, 24 };
    }
}

static void *claudito_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) {
        return NULL;
    }

    /* The canvas is the OS's and it is the whole screen: 90x160 in portrait,
     * 160x90 in landscape, at the biggest factor that fits (x8 on the P4's
     * 720x1280; see the top of the file). No controls of the OS: the six
     * buttons are the game's own, drawn in its bar, and a pet has no pause.
     * Nor AOS_RETRO_TOUCH: the finger is read by LVGL hit areas
     * (build_touch()), which know which object a finger came down on; the
     * OS's canvas finger counts one that slides in from outside as a new
     * tap, and the left-edge back swipe does exactly that. The cached
     * background is ours, through malloc(), which sends it to PSRAM. */
    lv_obj_update_layout(root);
    layout(a, lv_obj_get_width(root) > lv_obj_get_height(root));
    int cw = a->land ? CANVAS_LONG : CANVAS_SHORT;
    int ch = a->land ? CANVAS_SHORT : CANVAS_LONG;

    a->r = aos_retro_begin(root, cw, ch, 0, AOS_RETRO_CENTER);
    a->mem_bg = (uint16_t *)malloc((size_t)a->stage_box.w * a->stage_box.h * sizeof(uint16_t));
    /* the canvas starts black, and so does what the screen shows of it */
    a->shown = (uint16_t *)calloc((size_t)cw * ch, sizeof(uint16_t));
    if (!a->r || !a->mem_bg || !a->shown) {
        aos_hal_log("claudito", "out of memory for the canvas");
        aos_retro_end();
        free(a->mem_bg);
        free(a->shown);
        lv_free(a);
        return NULL;
    }
    /* whatever a screen of another size leaves around the canvas */
    aos_retro_set_border(0x101014);

    /* the three layers are rectangles of the canvas (stride = its width);
       the stage and its cached background share the frame's origin */
    uint16_t *px = a->r->px;
    const box_t *h = &a->hud_box, *g = &a->stage_box, *k = &a->bar_box;
    cl_view(&a->hud,   px + (size_t)h->y * cw + h->x, h->w, h->h, cw);
    cl_view(&a->stage, px + (size_t)g->y * cw + g->x, g->w, g->h, cw);
    cl_view(&a->bar,   px + (size_t)k->y * cw + k->x, k->w, k->h, cw);
    cl_view(&a->bg,    a->mem_bg, g->w, g->h, g->w);
    a->stage.ox = a->bg.ox = (int16_t)((g->w - CL_ART_W) / 2);
    a->stage.oy = a->bg.oy = (int16_t)(a->land ? STAGE_OY_LAND : STAGE_OY_PORT);
    a->tray_x = (cl_left(&a->stage) + cl_right(&a->stage)) / 2 - TRAY_W / 2;
    a->tray_y = cl_top(&a->stage) + 2;

    s_rng = (uint32_t)aos_hal_uptime_ms() | 1u;

    cl_pet_init(&a->pet);
    a->pet.y = CL_FLOOR_Y;
    a->walk_target = -1;
    a->idle_timer  = 20;
    a->mode        = MODE_IDLE;

    stats_load(a);
    a->bg_scene = -1;               /* frame_draw() draws it */

    build_touch(a, root);

    /* Only now can the mode it was left in be restored: mode_set() touches the
       tray's buttons, which did not exist until this line. */
    switch (a->mode) {
    case MODE_PLAYING:
        toy_reset(a, a->toy);
        break;
    case MODE_WASH:
    case MODE_TICKLE:
        a->sponge_x = a->pet.x + 20;
        a->sponge_y = CL_FLOOR_Y - 14;
        break;
    default:
        break;
    }
    mode_set(a, a->mode);

    pet_place(a);
    hud_draw(a);
    bar_draw(a);
    frame_draw(a);
    a->hud_dirty = a->bar_dirty = false;

    /* one that is still sleeping is not greeted */
    if (a->mode != MODE_SLEEP) {
        say(a, _("HOLA!"), "", 30);
    }

    aos_retro_run(FPS, step, draw, a);

    aos_hal_log("claudito", "ready | canvas %dx%d x%d at %d,%d", a->r->w, a->r->h,
                a->r->scale, a->r->x, a->r->y);
    return a;
}

static void claudito_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return;
    }
    aos_retro_stop();
    a->last_min = wall_minutes();
    stats_save(a);
    aos_retro_end();
    free(a->mem_bg);
    free(a->shown);
#ifdef AOS_SIM
    free(a->verify);
#endif
    lv_free(a);
}

/* Back -the left-edge swipe, or the back key- does NOT cancel whatever the
 * critter is doing: leaving is leaving. If you left it sleeping the app closes
 * and it goes on sleeping, and when you open it again you find it asleep with
 * the energy it recovered meanwhile (see stats_load).
 *
 * The one exception is an open tray, which is not an action but a half-chosen
 * menu: there, yes, one step back closes it. */
static bool claudito_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;

    if (a && (a->mode == MODE_FOOD_TRAY || a->mode == MODE_TOY_TRAY)) {
        mode_set(a, MODE_IDLE);
        return true;
    }
    return false;                   /* let the runtime close the app */
}

static bool claudito_init(aos_app_t *app)
{
    app->desc.id      = "demo.claudito";
    app->desc.name    = "Claudito";
    app->desc.icon    = "C";
    app->desc.icon_vec = AOS_ICON_PET;
    app->desc.color_a = 0xD97757;
    app->desc.color_b = 0x9C4A32;
    app->desc.order   = 140;
    /* The watch also asked for NO_SWIPE and LONG_DRAG, because rubbing the
     * critter with the sponge started the runtime's back gesture and its
     * lv_indev_wait_release() cut the rubbing short. Neither applies here:
     * P4OS's edge gestures are drags that start ON the edge (the left 28 px,
     * the bottom 36), a rub starts on the critter, and there is no global
     * gesture handler cutting long drags. So the system's back and home work
     * as in every app.
     *
     * Neither orientation flag: the layout follows the screen (portrait is
     * the one it was drawn for, landscape puts the HUD and the bar in
     * columns). Turning the screen recreates the app, which keeps its state
     * in the preferences. */
    app->desc.flags   = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN;

    app->create  = claudito_create;
    app->destroy = claudito_destroy;
    app->back    = claudito_back;
    return true;
}

AOS_APP_ENTRY(claudito_init);
