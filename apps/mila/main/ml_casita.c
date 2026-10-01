/*
 * MILA - the casita (see ml_casita.h)
 *
 * World coordinates are metres on the room's floor: x 0..3.4 left to right,
 * y 0..3.0 from the front edge to the back wall, z up. The screen is fixed
 * (the room fits): (1.7, 0, 0) lands on (s_ox, s_oy), at 207 px per metre
 * across, 155.5 per metre back, 137.1 per metre up (the watch's 108, 81 and
 * 71.4 at ML_CASITA_RES; tools/blender/casita_sample.py). P4OS: the room's
 * picture is 719 x 801 px on a screen of 720 x 1280 or 1280 x 720, so where
 * it lands, and where the buttons go, depend on how the screen stands
 * (layout()). The cache holds only the room's box on the screen (s_box):
 * its pixels (LP) are the screen's less the box's corner, and the camera
 * sits at minus that corner.
 */
#include "ml_app.h"
#include "ml_audio.h"
#include "ml_casita.h"
#include "ml_link.h"
#include "ml_shop.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PXX         (108.0f * ML_CASITA_RES)    /* px per metre across       */
#define PXY         (81.0f * ML_CASITA_RES)     /* back                      */
#define PXZ         (71.43f * ML_CASITA_RES)    /* up                        */
#define DPLANE      (-21.166f / PXY)            /* depth per px row on the floor */
#define CK          (ML_CASITA_RES / ML_RES)    /* sizes that were set at 1.5 */
#define SX(x)       (s_ox + PXX * ((x) - 1.7f))
#define SY(y, z)    (s_oy - PXY * (y) - PXZ * (z))
#define WX(sx)      (1.7f + ((sx) - s_ox) / PXX)       /* the floor under a screen point */
#define WY(sy)      ((s_oy - (sy)) / PXY)
#define DOFS        400
#define DEPTH(y, z) (DOFS + ml_iround(21.166f * (y) - 24.0f * (z)))

/* where the room and the buttons are (layout(), for the screen as it is) */
static float s_ox = 360, s_oy = 850;
static struct { int x, y, w, h; bool column; } s_bar;
static int s_pill_y, s_pill_l, s_pill_r;       /* the coins' and the stars' pills */
static struct { int x, y, w, h; } s_box;       /* the room's picture on the screen: the cache */
static struct { int w, h, ax, ay; } s_room;     /* the room's picture as rendered (0: not seen yet) */
#define LOGO_Y      40                          /* upright: the logo's box, 190 px tall */
#define LOGO_H      190

enum { B_SIT = 0, B_GROOM, B_WANDER, B_SLEEP, B_EAT, B_TOY, B_CHASE, B_REACT, B_GIFT, B_N };

enum {
    TY_MOUSE = 0, TY_YARN, TY_BALL, TY_FEATHER, TY_BOX, TY_CATNIP, TY_TUNNEL, TY_POST, TY_HAMMOCK,
    TY_FISHBOWL, TY_N,
};

typedef struct {
    const char *id;
    float x, y, z;              /* its spot (free toys: where it starts)   */
    bool  free;                 /* can be dragged and rolls                 */
} toy_def_t;

static const toy_def_t s_toys[TY_N] = {
    [TY_MOUSE] = { "toy_mouse", 2.5f, 1.8f, 0, true },
    [TY_YARN] = { "toy_yarn", 1.1f, 0.95f, 0, true },
    [TY_BALL] = { "toy_ball", 2.2f, 1.0f, 0, true },
    [TY_FEATHER] = { "toy_feather", 1.7f, 1.35f, 0, true },
    [TY_BOX] = { "toy_box", 2.9f, 0.6f, 0, false },
    [TY_CATNIP] = { "toy_catnip", 3.15f, 1.4f, 0, false },
    [TY_TUNNEL] = { "toy_tunnel", 1.6f, 2.5f, 0, false },
    [TY_POST] = { "toy_post", 3.0f, 2.4f, 0, false },
    [TY_HAMMOCK] = { "toy_hammock", 2.4f, 2.6f, 0, false },
    [TY_FISHBOWL] = { "toy_fishbowl", 0.35f, 1.5f, 0, false },
};

typedef struct {
    bool      owned;
    ml_anim_t spr, sh, roll;
    float     x, y, z, vx, vy;
    float     t;                /* animation time                           */
    bool      held;             /* the finger has it                        */
} toy_t;

typedef struct {
    float x, y, z;
    int   face, anim;
    float anim_t;
    bool  loop, hidden;
    float tx, ty, speed;
    bool  walking;
    int   beh;
    float beh_t, beh_dur;
    int   toy;
    int   stage;
} cat_t;

typedef struct {
    ml_world_t w;
    ml_dlist_t dl, dl_prev;         /* this frame's and the last one's (what changed) */
    ml_anim_t  logo;                /* upright, over the room                 */
    int        hud_coins, hud_stars, hud_n;     /* what the HUD showed        */
    struct { int x, y; bool on; } heart_was[10];
    ml_anim_t  room, bed, bed_sh, bowls, bowls_sh, gift, gift_sh, gift_open, window_day, door;
    float      door_t;             /* > 0 while the door swings open        */
    toy_t      toys[TY_N];
    cat_t      m;
    cat_t      g;                  /* the friend's Mila, visiting            */
    bool       guest_on, guest_want, guest_leaving;
    ml_mila_t  guest_art;          /* her hat and collar (her body is ours)  */
    float      t;
    struct { float x, y, t; } hearts[10];
    bool       gift_ready, gift_opening;
    float      gift_t;
    int        coins_shown;
    uint32_t   rng;
    bool       daytime;
    /* input from the LVGL thread */
    volatile bool tap;
    volatile int  tap_x, tap_y;
    volatile bool drag;
    volatile int  drag_x, drag_y;
    int           drag_toy;
    int           press_x, press_y;
    bool          pressed, dragged;
    /* Mila's and the guest's frames unpacked (ML_ZIP): body, shadow, hat,
     * neck, each */
    ml_zstream_t  zs[8];
    uint32_t      zgen;             /* ml_mila_generation() they belong to */
} casita_t;

static casita_t *C(app_t *a) { return (casita_t *)a->casita; }

/* The room in the middle of what the buttons leave: upright the logo above
 * it and the bar under it, lying down the room on the left and the buttons
 * in a column on the right. The room's picture is 719 x 801 at 1.92 (its
 * anchor, the floor's front left corner, is in s_room once it was loaded):
 * upright it fits between the logo and the bar; lying down it is taller
 * than the screen, and what is left out is 3/5 wallpaper and trim at the
 * top and 2/5 floor at the front (the watch's HUD covered both). */
static void layout(void)
{
    int rw = s_room.w, rh = s_room.h, ax = s_room.ax, ay = s_room.ay;
    if (!rw) {
        /* not loaded yet: what the art should be */
        rw = ml_iround(3.47f * PXX);
        rh = ml_iround(3.0f * PXY + 2.3f * PXZ);
        ax = ml_iround(0.03f * PXX);
        ay = rh - ml_iround(0.02f * PXY);
    }
    int left, top;
    if (ML_W > ML_H) {
        s_bar.column = true;
        s_bar.w = 300;
        /* the room, the column and the margins around them in thirds */
        int free = ML_W - rw - s_bar.w;
        if (free < 0) free = 0;
        left = free / 3;
        s_bar.x = left * 2 + rw;
        s_bar.y = 120;
        s_bar.h = ML_H - s_bar.y - 36;
        top = rh > ML_H ? -((rh - ML_H) * 3 / 5) : (ML_H - rh) / 2;
        s_pill_y = 36;
        s_pill_l = s_bar.x;
        s_pill_r = s_bar.x + s_bar.w;
    } else {
        s_bar.column = false;
        s_bar.x = 20;
        s_bar.w = ML_W - 40;
        s_bar.h = 150;
        s_bar.y = ML_H - s_bar.h - 44;
        left = (ML_W - rw) / 2;
        int t0 = LOGO_Y + LOGO_H, t1 = s_bar.y;
        top = t0 + (t1 - t0 - rh) / 2;
        s_pill_y = 28;
        s_pill_l = 20;
        s_pill_r = ML_W - 20;
    }
    s_ox = (float)(left + ax) + 1.7f * PXX;
    s_oy = (float)(top + ay);
    int x0 = left < 0 ? 0 : left, y0 = top < 0 ? 0 : top;
    int x1 = left + rw > ML_W ? ML_W : left + rw, y1 = top + rh > ML_H ? ML_H : top + rh;
    s_box.x = x0;
    s_box.y = y0;
    s_box.w = x1 > x0 ? x1 - x0 : 1;
    s_box.h = y1 > y0 ? y1 - y0 : 1;
}

/* the room into a cache the size of its box on the screen */
static bool backdrop(casita_t *c)
{
    const ml_spr_t *room = c->room.n ? &c->room.f[0] : NULL;
    if (room) {
        s_room.w = room->w;
        s_room.h = room->h;
        s_room.ax = room->ax;
        s_room.ay = room->ay;
    }
    layout();
    ml_cache_fit_box(s_box.w, s_box.h);
    bool ok = ml_world_backdrop(&c->w, room, ml_iround(SX(0)) - s_box.x, ml_iround(SY(0, 0)) - s_box.y, DOFS, DPLANE);
    c->w.still_w = s_box.w;
    c->w.still_h = s_box.h;
    return ok;
}

static bool in_bar(int x, int y)
{
    return x >= s_bar.x && x < s_bar.x + s_bar.w && y >= s_bar.y - 10 && y < s_bar.y + s_bar.h;
}

/* which of the n buttons a point on the bar is */
static int bar_index(int x, int y, int n)
{
    int i = s_bar.column ? (y - s_bar.y) * n / s_bar.h : (x - s_bar.x) * n / s_bar.w;
    return i < 0 ? 0 : i >= n ? n - 1 : i;
}

/* the centre of button i */
static void bar_centre(int i, int n, int *cx, int *cy)
{
    if (s_bar.column) {
        int step = s_bar.h / n;
        *cx = s_bar.x + s_bar.w / 2;
        *cy = s_bar.y + step * i + step / 2;
    } else {
        int step = s_bar.w / n;
        *cx = s_bar.x + step * i + step / 2;
        *cy = s_bar.y + s_bar.h / 2;
    }
}
static void heart(casita_t *c);
static uint32_t rnd(casita_t *c);
static float rndf(casita_t *c, float a, float b);

static uint32_t rnd(casita_t *c)
{
    return ml_rand(&c->rng);
}

static float rndf(casita_t *c, float a, float b)
{
    return a + (b - a) * (float)(rnd(c) >> 8) / 16777216.0f;
}

static int today(void)
{
    if (!aos_hal_time_is_valid()) return -1;
    struct tm t;
    aos_hal_time_now(&t);
    return t.tm_year * 400 + t.tm_yday;
}

/* ---- life ---- */

static void mlc_free_art(casita_t *c);

static void load(const char *name, ml_anim_t *a)
{
    if (ml_art_has(name)) ml_art_load(name, a);
}

bool mlc_open(app_t *a)
{
    mlc_close(a);
    casita_t *c = (casita_t *)ml_calloc(1, sizeof(casita_t));
    if (!c) return false;
    c->rng = (uint32_t)aos_hal_uptime_ms() | 1u;
    load("casita_room", &c->room);
    load("casita_bed", &c->bed);
    load("casita_bed_sh", &c->bed_sh);
    load("casita_bowls", &c->bowls);
    load("casita_bowls_sh", &c->bowls_sh);
    load("casita_gift", &c->gift);
    load("casita_gift_sh", &c->gift_sh);
    load("casita_gift_open", &c->gift_open);
    load("casita_door", &c->door);
    if (aos_hal_time_is_valid()) {
        struct tm t;
        aos_hal_time_now(&t);
        c->daytime = t.tm_hour >= 7 && t.tm_hour < 19;
        if (c->daytime) load("casita_window_day", &c->window_day);
    }
    ml_yield();
    for (int i = 0; i < TY_N; i++) {
        toy_t *t = &c->toys[i];
        t->owned = ml_prog_owns(&a->prog, s_toys[i].id) || a->dev_unlock;
        t->x = s_toys[i].x;
        t->y = s_toys[i].y;
        t->z = s_toys[i].z;
        if (!t->owned) continue;
        char nm[40];
        snprintf(nm, sizeof nm, "%s", s_toys[i].id);
        if (ml_art_has(nm)) ml_art_load(nm, &t->spr);
        snprintf(nm, sizeof nm, "%s_sh", s_toys[i].id);
        load(nm, &t->sh);
        snprintf(nm, sizeof nm, "%s_roll", s_toys[i].id);
        load(nm, &t->roll);
        ml_yield();
    }
    load("ui_logo", &c->logo);
    if (!backdrop(c)) {
        ml_world_free(&c->w);
        mlc_free_art(c);
        free(c);
        return false;
    }
    /* the room is in the cache now: its picture only comes back to turn */
    ml_anim_free(&c->room);
    ml_mila_load(&a->mila, ML_SET_CASITA, a->prog.hat, (uint32_t)a->prog.hat_col, a->prog.neck,
                 (uint32_t)a->prog.neck_col);
    int d = today();
    c->gift_ready = d >= 0 && d != a->prog.gift_day;
    c->m.x = 1.75f;
    c->m.y = 1.25f;
    c->m.face = MD_S;
    c->m.anim = MA_C_SIT;
    c->m.loop = true;
    c->m.beh = B_SIT;
    c->m.beh_dur = 2.5f;
    c->drag_toy = -1;
    c->guest_want = a->visit;
#ifdef AOS_SIM_BUILTIN
    /* ML_GUEST=<hat>,<neck>: a friend's Mila walks in without a second
     * simulator (to look at her frames) */
    const char *ge = getenv("ML_GUEST");
    if (ge && ge[0]) {
        const char *cm = strchr(ge, ',');
        snprintf(a->guest_hat, sizeof a->guest_hat, "%.*s", cm ? (int)(cm - ge) : (int)strlen(ge), ge);
        snprintf(a->guest_neck, sizeof a->guest_neck, "%s", cm ? cm + 1 : "");
        c->guest_want = true;
    }
#endif
    a->casita = c;
    return true;
}

static void mlc_free_art(casita_t *c)
{
    ml_anim_free(&c->logo);
    ml_anim_t *all[] = { &c->room, &c->bed, &c->bed_sh, &c->bowls, &c->bowls_sh, &c->gift, &c->gift_sh,
                         &c->gift_open, &c->window_day, &c->door };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) ml_anim_free(all[i]);
    for (int i = 0; i < TY_N; i++) {
        ml_anim_free(&c->toys[i].spr);
        ml_anim_free(&c->toys[i].sh);
        ml_anim_free(&c->toys[i].roll);
    }
    for (int i = 0; i < 8; i++) ml_zstream_free(&c->zs[i]);
    ml_zscratch_free();
}

void mlc_close(app_t *a)
{
    casita_t *c = C(a);
    if (!c) return;
    a->casita = NULL;
    mlc_free_art(c);
    ml_mila_free(&c->guest_art);
    free(c->w.cc);
    free(c->w.cd);
    free(c);
}

void mlc_guest(app_t *a, const char *hat, uint32_t hat_col, const char *neck, uint32_t neck_col, bool on)
{
    /* the LVGL thread only raises the flag: the worker loads her layers and
     * walks her in (ml_link.c keeps the outfit in the app) */
    (void)hat;
    (void)hat_col;
    (void)neck;
    (void)neck_col;
    casita_t *c = C(a);
    if (c) c->guest_want = on;
}

/* the guest arrives through the door, plays near Mila, and leaves by it */
static void guest_step(app_t *a, casita_t *c, float dt)
{
    cat_t *g = &c->g;
    if (c->guest_want && !c->guest_on && !c->guest_leaving) {
        c->guest_art.layers_only = true;
        ml_mila_load(&c->guest_art, ML_SET_CASITA, a->guest_hat, a->guest_hat_col, a->guest_neck, a->guest_neck_col);
        c->guest_on = true;
        memset(g, 0, sizeof *g);
        g->x = 0.65f;
        g->y = 2.85f;
        g->face = MD_S;
        g->anim = MA_C_WALK;
        g->loop = true;
        g->tx = 1.3f;
        g->ty = 1.5f;
        g->speed = 0.8f;
        g->walking = true;
        c->door_t = 0.01f;
        ml_snd(SND_MEOW);
    }
    if (!c->guest_want && c->guest_on && !c->guest_leaving) {
        c->guest_leaving = true;
        g->tx = 0.65f;
        g->ty = 2.85f;
        g->speed = 0.9f;
        g->walking = true;
        c->door_t = 0.01f;
    }
    if (!c->guest_on) return;
    g->anim_t += dt;
    g->beh_t += dt;
    if (g->walking) {
        float dx = g->tx - g->x, dy = g->ty - g->y;
        float dist = sqrtf(dx * dx + dy * dy), st = g->speed * dt;
        if (dist <= st) {
            g->x = g->tx;
            g->y = g->ty;
            g->walking = false;
            g->beh_t = 0;
            g->beh_dur = rndf(c, 2.0f, 5.0f);
            if (getenv("ML_DEBUG")) aos_hal_log("mila", "guest at %.2f,%.2f", (double)g->x, (double)g->y);
            if (c->guest_leaving) {
                c->guest_on = c->guest_leaving = false;
                ml_mila_free(&c->guest_art);
                return;
            }
            /* face Mila and do something friendly */
            g->face = c->m.x > g->x ? MD_E : MD_W;
            uint32_t r = rnd(c) % 3;
            g->anim = r == 0 ? MA_C_SIT : r == 1 ? MA_C_BAT : MA_C_POUNCE;
            g->loop = r == 0;
            g->anim_t = 0;
            if (rnd(c) % 2) heart(c);
        } else {
            g->x += dx / dist * st;
            g->y += dy / dist * st;
            g->face = fabsf(dx) > fabsf(dy) * 0.8f ? (dx > 0 ? MD_E : MD_W) : (dy > 0 ? MD_N : MD_S);
            g->anim = g->speed > 1.2f ? MA_C_RUN : MA_C_WALK;
            g->loop = true;
        }
    } else if (g->beh_t > g->beh_dur && !c->guest_leaving) {
        /* follow Mila around, a little to her side */
        float side = g->x < c->m.x ? -0.45f : 0.45f;
        g->tx = c->m.x + side + rndf(c, -0.2f, 0.2f);
        g->ty = c->m.y + rndf(c, -0.25f, 0.25f);
        if (g->tx < 0.3f) g->tx = 0.3f;
        if (g->tx > 3.1f) g->tx = 3.1f;
        if (g->ty < 0.25f) g->ty = 0.25f;
        if (g->ty > 2.1f) g->ty = 2.1f;
        g->speed = rnd(c) % 4 == 0 ? 1.7f : 0.8f;
        g->walking = true;
        g->anim_t = 0;
    }
}

/* ---- Mila's life ---- */

static void set_anim(cat_t *m, int anim, bool loop)
{
    if (m->anim != anim) m->anim_t = 0;
    m->anim = anim;
    m->loop = loop;
}

static float anim_len(app_t *a, int anim, int face)
{
    const ml_frames_t *f = ml_mila_frames(&a->mila, anim, face);
    if (!f || !f->body.n) return 1.0f;
    return f->body.n * (f->body.ms ? f->body.ms : 120) / 1000.0f;
}

static void walk_to(cat_t *m, float x, float y, float speed)
{
    if (x < 0.25f) x = 0.25f;
    if (x > 3.15f) x = 3.15f;
    if (y < 0.2f) y = 0.2f;
    if (y > 2.2f) y = 2.2f;
    m->tx = x;
    m->ty = y;
    m->speed = speed;
    m->walking = true;
    m->z = 0;
    m->hidden = false;
}

static void begin(app_t *a, casita_t *c, int beh)
{
    cat_t *m = &c->m;
    m->beh = beh;
    m->beh_t = 0;
    m->stage = 0;
    m->walking = false;
    switch (beh) {
    case B_SIT:
        m->face = rnd(c) % 3 == 0 ? (rnd(c) & 1 ? MD_E : MD_W) : MD_S;
        set_anim(m, MA_C_SIT, true);
        m->beh_dur = rndf(c, 2.5f, 6.0f);
        break;
    case B_GROOM:
        m->face = MD_S;
        set_anim(m, MA_C_GROOM, false);
        m->beh_dur = anim_len(a, MA_C_GROOM, MD_S) * 2;
        break;
    case B_WANDER:
        walk_to(m, rndf(c, 0.5f, 3.0f), rndf(c, 0.4f, 1.9f), 0.8f);
        m->beh_dur = 8;
        break;
    case B_SLEEP:
        walk_to(m, 0.6f, 2.0f, 0.8f);
        m->beh_dur = rndf(c, 12, 22);
        break;
    case B_EAT:
        walk_to(m, 0.55f, 0.8f, 0.8f);
        m->beh_dur = 4;
        break;
    case B_GIFT:
        walk_to(m, 1.35f, 1.7f, 0.8f);
        m->beh_dur = 5;
        break;
    default:
        break;
    }
}

/* a toy of hers to go and use, -1 none */
static int pick_toy(casita_t *c)
{
    int got[TY_N], n = 0;
    for (int i = 0; i < TY_N; i++)
        if (c->toys[i].owned && i != TY_FEATHER) got[n++] = i;
    return n ? got[rnd(c) % (uint32_t)n] : -1;
}

static void use_toy(app_t *a, casita_t *c, int toy)
{
    cat_t *m = &c->m;
    const toy_t *t = &c->toys[toy];
    m->beh = B_TOY;
    m->toy = toy;
    m->beh_t = 0;
    m->stage = 0;
    switch (toy) {
    case TY_POST: walk_to(m, t->x, t->y - 0.35f, 0.9f); break;
    case TY_BOX: walk_to(m, t->x, t->y - 0.45f, 0.9f); break;
    case TY_TUNNEL: walk_to(m, t->x - 0.6f, t->y - 0.1f, 0.9f); break;
    case TY_HAMMOCK: walk_to(m, t->x, t->y - 0.55f, 0.9f); break;
    case TY_FISHBOWL: walk_to(m, t->x + 0.45f, t->y, 0.9f); break;
    case TY_CATNIP: walk_to(m, t->x - 0.4f, t->y, 0.9f); break;
    default: walk_to(m, t->x + (t->x > m->x ? -0.3f : 0.3f), t->y, 1.6f); break;
    }
    (void)a;
}

static void next_behaviour(app_t *a, casita_t *c)
{
    if (c->gift_ready && rnd(c) % 3 == 0) {
        begin(a, c, B_GIFT);
        return;
    }
    uint32_t r = rnd(c) % 100;
    if (r < 22) begin(a, c, B_SIT);
    else if (r < 34) begin(a, c, B_GROOM);
    else if (r < 58) begin(a, c, B_WANDER);
    else if (r < 66) begin(a, c, B_SLEEP);
    else if (r < 74) begin(a, c, B_EAT);
    else {
        int t = pick_toy(c);
        if (t >= 0) use_toy(a, c, t);
        else begin(a, c, B_WANDER);
    }
}

static void heart(casita_t *c)
{
    for (int i = 0; i < 10; i++) {
        if (c->hearts[i].t <= 0) {
            c->hearts[i].x = SX(c->m.x) + rndf(c, -27 * CK, 27 * CK);
            c->hearts[i].y = SY(c->m.y, 0.55f);
            c->hearts[i].t = 1.2f;
            return;
        }
    }
}

/* the arrival at the place the behaviour walked to */
static void arrive(app_t *a, casita_t *c)
{
    cat_t *m = &c->m;
    switch (m->beh) {
    case B_SLEEP:
        m->x = 0.6f;
        m->y = 2.3f;
        m->z = 0.08f;
        m->face = MD_S;
        set_anim(m, MA_C_SLEEP, true);
        break;
    case B_EAT:
        m->face = MD_N;
        set_anim(m, MA_C_EAT, true);
        break;
    case B_GIFT:
        m->face = MD_W;
        set_anim(m, MA_C_SIT, true);
        break;
    case B_WANDER:
        m->beh_dur = m->beh_t + rndf(c, 0.5f, 1.5f);
        m->face = MD_S;
        set_anim(m, MA_C_SIT, true);
        break;
    case B_TOY: {
        toy_t *t = &c->toys[m->toy];
        m->beh_t = 0;
        switch (m->toy) {
        case TY_POST: m->face = MD_N; set_anim(m, MA_C_SCRATCH, true); m->beh_dur = 3.5f; break;
        case TY_BOX:
            m->x = t->x;
            m->y = t->y;
            m->z = 0.02f;
            m->face = MD_S;
            set_anim(m, MA_C_PEEK, true);
            m->beh_dur = 5;
            break;
        case TY_TUNNEL:
            m->hidden = true;
            m->beh_dur = 3.5f;
            break;
        case TY_HAMMOCK:
            m->x = t->x;
            m->y = t->y;
            m->z = 0.55f;
            m->face = MD_S;
            set_anim(m, MA_C_LIE, true);
            m->beh_dur = 9;
            break;
        case TY_FISHBOWL: m->face = MD_W; set_anim(m, MA_C_SIT, true); m->beh_dur = 5; break;
        case TY_CATNIP: m->face = MD_S; set_anim(m, MA_C_BELLY, true); m->beh_dur = 4; heart(c); break;
        default:
            /* a free toy: a swat and it rolls away */
            m->face = t->x > m->x ? MD_E : MD_W;
            set_anim(m, MA_C_BAT, false);
            m->beh_dur = anim_len(a, MA_C_BAT, m->face);
            t->vx = (m->face == MD_E ? 1 : -1) * rndf(c, 0.8f, 1.4f);
            t->vy = rndf(c, -0.4f, 0.4f);
            ml_snd(SND_TOY);
            break;
        }
        break;
    }
    default:
        break;
    }
}

static void react(app_t *a, casita_t *c)
{
    cat_t *m = &c->m;
    m->walking = false;
    m->beh = B_REACT;
    m->beh_t = 0;
    m->face = MD_S;
    uint32_t r = rnd(c) % 3;
    if (r == 0) {
        set_anim(m, MA_C_MEOW, false);
        m->beh_dur = anim_len(a, MA_C_MEOW, MD_S) + 0.4f;
        ml_snd(SND_MEOW);
    } else if (r == 1) {
        set_anim(m, MA_C_PURR, true);
        m->beh_dur = 3.0f;
        ml_snd(SND_PURR);
        heart(c);
        heart(c);
    } else {
        set_anim(m, MA_C_BELLY, true);
        m->beh_dur = 2.6f;
        heart(c);
    }
    a->prog.stat[SX_PETS]++;
}

static void step_toys(casita_t *c, float dt)
{
    for (int i = 0; i < TY_N; i++) {
        toy_t *t = &c->toys[i];
        t->t += dt;
        if (!t->owned || !s_toys[i].free || t->held) continue;
        t->x += t->vx * dt;
        t->y += t->vy * dt;
        float k = expf(-dt * 2.2f);
        t->vx *= k;
        t->vy *= k;
        if (t->x < 0.2f) { t->x = 0.2f; t->vx = -t->vx; }
        if (t->x > 3.2f) { t->x = 3.2f; t->vx = -t->vx; }
        if (t->y < 0.15f) { t->y = 0.15f; t->vy = -t->vy; }
        if (t->y > 2.1f) { t->y = 2.1f; t->vy = -t->vy; }
    }
}

void mlc_step(app_t *a, float dt)
{
    casita_t *c = C(a);
    if (!c) return;
    cat_t *m = &c->m;
    c->t += dt;
    m->anim_t += dt;
    m->beh_t += dt;
    /* the finger */
    if (c->tap) {
        c->tap = false;
        float wx = WX(c->tap_x);
        float wy = WY(c->tap_y);
        float mx = SX(m->x), my = SY(m->y, m->z + 0.2f);
        if (fabsf(c->tap_x - mx) < 70 * CK && fabsf(c->tap_y - my) < 70 * CK && !m->hidden) {
            react(a, c);
        } else if (c->gift_ready && fabsf(wx - 1.3f) < 0.35f && fabsf(wy - 2.05f) < 0.35f) {
            c->gift_ready = false;
            c->gift_opening = true;
            c->gift_t = 0;
            int coins = 15 + (int)(rnd(c) % 16);
            a->prog.coins += coins;
            a->prog.gift_day = today();
            ml_snd(SND_GIFT);
            mla_save(a);
        } else {
            for (int i = 0; i < TY_N; i++) {
                toy_t *t = &c->toys[i];
                if (!t->owned || i == TY_FEATHER) continue;
                if (fabsf(wx - t->x) < 0.35f && fabsf(wy - t->y) < 0.35f) {
                    use_toy(a, c, i);
                    a->prog.stat[SX_TOYS]++;
                    break;
                }
            }
        }
    }
    if (c->drag) {
        float wx = WX(c->drag_x);
        float wy = WY(c->drag_y);
        if (c->drag_toy < 0) {
            /* grab the free toy under the finger, or the feather */
            for (int i = 0; i < TY_N && c->drag_toy < 0; i++) {
                toy_t *t = &c->toys[i];
                if (t->owned && s_toys[i].free && i != TY_FEATHER && fabsf(wx - t->x) < 0.4f && fabsf(wy - t->y) < 0.4f)
                    c->drag_toy = i;
            }
            if (c->drag_toy < 0 && c->toys[TY_FEATHER].owned) c->drag_toy = TY_FEATHER;
            if (c->drag_toy >= 0) c->toys[c->drag_toy].held = true;
        }
        if (c->drag_toy >= 0) {
            toy_t *t = &c->toys[c->drag_toy];
            if (c->drag_toy == TY_FEATHER) wy = (s_oy - c->drag_y + PXZ * 0.45f) / PXY;
            t->vx = (wx - t->x) / (dt > 0.001f ? dt : 0.001f) * 0.3f;
            t->vy = (wy - t->y) / (dt > 0.001f ? dt : 0.001f) * 0.3f;
            t->x = wx;
            t->y = wy;
            if (m->beh != B_CHASE && m->beh != B_SLEEP) {
                m->beh = B_CHASE;
                m->beh_t = 0;
                m->stage = 0;
            }
        }
    } else if (c->drag_toy >= 0) {
        c->toys[c->drag_toy].held = false;
        if (c->drag_toy == TY_FEATHER) c->toys[TY_FEATHER].vx = c->toys[TY_FEATHER].vy = 0;
        c->drag_toy = -1;
    }
    step_toys(c, dt);
    guest_step(a, c, dt);
    /* the behaviour */
    if (m->beh == B_CHASE) {
        int ti = c->drag_toy;
        if (ti < 0) {
            if (m->beh_t > 1.2f) next_behaviour(a, c);
        } else {
            toy_t *t = &c->toys[ti];
            float dx = t->x - m->x, dy = t->y - m->y;
            float dist = sqrtf(dx * dx + dy * dy);
            if (dist > 0.45f) {
                walk_to(m, t->x - dx / dist * 0.35f, t->y - dy / dist * 0.35f, 1.8f);
            } else if (m->stage == 0 || m->anim_t > 0.6f) {
                m->walking = false;
                m->face = dx > 0 ? MD_E : MD_W;
                set_anim(m, ti == TY_FEATHER ? MA_C_JUMP : MA_C_POUNCE, false);
                m->anim_t = 0;
                m->stage = 1;
                ml_snd(SND_POUNCE);
            }
            m->beh_t = 0;
        }
    }
    if (m->walking) {
        float dx = m->tx - m->x, dy = m->ty - m->y;
        float dist = sqrtf(dx * dx + dy * dy);
        float stepd = m->speed * dt;
        if (dist <= stepd || dist < 0.01f) {
            m->x = m->tx;
            m->y = m->ty;
            m->walking = false;
            if (m->beh != B_CHASE) arrive(a, c);
        } else {
            m->x += dx / dist * stepd;
            m->y += dy / dist * stepd;
            m->face = fabsf(dx) > fabsf(dy) * 0.8f ? (dx > 0 ? MD_E : MD_W) : (dy > 0 ? MD_N : MD_S);
            bool run = m->speed > 1.2f;
            set_anim(m, run && (m->face == MD_E || m->face == MD_W) ? MA_C_RUN : MA_C_WALK, true);
        }
    } else if (m->beh != B_CHASE && m->beh_t >= m->beh_dur) {
        if (m->beh == B_TOY && m->toy == TY_TUNNEL && m->hidden) {
            /* out at the other end */
            m->hidden = false;
            m->x = 2.05f;          /* the tunnel's right mouth */
            m->y = 2.5f;
            m->face = MD_S;
            set_anim(m, MA_C_PEEK, true);
            m->beh_t = 0;
            m->beh_dur = 2.0f;
            m->toy = -1;
        } else {
            m->z = 0;
            m->hidden = false;
            next_behaviour(a, c);
        }
    }
    if (m->beh == B_SLEEP && !m->walking && m->anim != MA_C_SLEEP) m->z = 0;
    for (int i = 0; i < 10; i++)
        if (c->hearts[i].t > 0) {
            c->hearts[i].t -= dt;
            c->hearts[i].y -= 42 * CK * dt;
        }
    if (c->door_t > 0) {
        c->door_t += dt;
        if (c->door_t > 2.4f) c->door_t = 0;
    }
    if (c->gift_opening) {
        c->gift_t += dt;
        if (c->gift_t > 1.6f) c->gift_opening = false;
    }
    /* the draw list (the last one kept: what changed is the difference) */
    ml_dlist_t *dl = &c->dl;
    memcpy(&c->dl_prev, dl, sizeof *dl);
    ml_dlist_clear(dl);
    /* LP: the screen less the room box's corner (the cache is the box) */
#define ADD(spr, fmt_, x_, y_, z_, lut_, fl_, pr_) do { \
        const ml_spr_t *s__ = (spr); \
        if (s__) { ml_draw_t *e = ml_dlist_add(dl); \
            if (e) { e->s = s__; e->lut = (lut_); e->x = (int16_t)(ml_iround(SX(x_)) - s_box.x); \
                e->y = (int16_t)(ml_iround(SY(y_, z_)) - s_box.y); e->d = (int16_t)DEPTH(y_, z_); e->fmt = (fmt_); \
                e->flags = (fl_); e->prio = (pr_); e->xray = ml_rgb(120, 140, 210); } } } while (0)
#define F0(an) ((an).n ? &(an).f[0] : NULL)
    /* Mila's frames are packed: each layer is unpacked into a stream of
     * its own (a frame already there is not unpacked again) */
    if (ml_mila_generation() != c->zgen) {
        for (int i = 0; i < 8; i++) ml_zstream_reset(&c->zs[i]);
        c->zgen = ml_mila_generation();
    }
#define Z(i, spr) ml_zstream_get(&c->zs[i], (spr))
    ADD(F0(c->bed), ML_PX_COL, 0.6f, 2.3f, 0, NULL, 0, 0);
    ADD(F0(c->bed_sh), ML_PX_PLANE, 0.6f, 2.3f, 0, NULL, 0, 0);
    ADD(F0(c->bowls), ML_PX_COL, 0.45f, 0.45f, 0, NULL, 0, 0);
    ADD(F0(c->bowls_sh), ML_PX_PLANE, 0.45f, 0.45f, 0, NULL, 0, 0);
    if (c->window_day.n) ADD(F0(c->window_day), ML_PX_COL, 2.5f, 3.0f, 0.75f, NULL, 0, 0);
    if (c->door.n) {
        /* 00 closed .. 03 open: it opens for a visit and closes behind */
        int k = 0;
        if (c->door_t > 0) {
            float t = c->door_t;
            k = t < 0.36f ? (int)(t / 0.09f) : t < 2.0f ? 3 : 3 - (int)((t - 2.0f) / 0.09f);
            if (k < 0) k = 0;
            if (k >= c->door.n) k = c->door.n - 1;
        }
        ADD(&c->door.f[k], ML_PX_COL, 0.65f, 3.0f, 0, NULL, 0, 0);
    }
    if (c->gift_ready) {
        ADD(F0(c->gift), ML_PX_COL, 1.3f, 2.05f, 0, NULL, 0, 0);
        ADD(F0(c->gift_sh), ML_PX_PLANE, 1.3f, 2.05f, 0, NULL, 0, 0);
    } else if (c->gift_opening && c->gift_open.n) {
        int k = (int)(c->gift_t / 0.12f);
        if (k >= c->gift_open.n) k = c->gift_open.n - 1;
        ADD(&c->gift_open.f[k], ML_PX_COL, 1.3f, 2.05f, 0, NULL, 0, 0);
    }
    for (int i = 0; i < TY_N; i++) {
        toy_t *t = &c->toys[i];
        if (!t->owned || !t->spr.n) continue;
        bool moving = fabsf(t->vx) + fabsf(t->vy) > 0.05f;
        const ml_spr_t *s;
        if (moving && t->roll.n) {
            /* the frames roll towards +x: backwards for -x */
            int k = (int)(t->t * 14) % t->roll.n;
            s = &t->roll.f[t->vx >= 0 ? k : t->roll.n - 1 - k];
        }
        else s = &t->spr.f[t->spr.n > 1 ? (int)(t->t * 6) % t->spr.n : 0];
        if (i == TY_FEATHER && !t->held) continue;      /* only while dragged */
        ADD(s, ML_PX_COL, t->x, t->y, t->z, NULL, 0, 0);
        if (t->sh.n) ADD(F0(t->sh), ML_PX_PLANE, t->x, t->y, 0, NULL, 0, 0);
    }
    if (!m->hidden) {
        const ml_frames_t *fr = ml_mila_frames(&a->mila, m->anim, m->face);
        if (!fr) fr = ml_mila_frames(&a->mila, MA_C_SIT, MD_S);
        if (fr) {
            int k = ml_mila_frame(fr, m->anim_t, m->loop);
            ADD(Z(0, &fr->body.f[k % fr->body.n]), ML_PX_COL, m->x, m->y, m->z, NULL, DR_XRAY, 2);
            if (fr->sh.n) ADD(Z(1, &fr->sh.f[k % fr->sh.n]), ML_PX_PLANE, m->x, m->y, m->z, NULL, 0, 0);
            if (fr->neck.n) ADD(Z(2, &fr->neck.f[k % fr->neck.n]), ML_PX_LID, m->x, m->y, m->z, &a->mila.neck_lut, 0, 3);
            if (fr->hat.n) ADD(Z(3, &fr->hat.f[k % fr->hat.n]), ML_PX_LID, m->x, m->y, m->z, &a->mila.hat_lut, 0, 4);
        }
    }
    if (c->guest_on) {
        cat_t *g = &c->g;
        const ml_frames_t *fr = ml_mila_frames(&a->mila, g->anim, g->face);
        const ml_frames_t *gl = ml_mila_frames(&c->guest_art, g->anim, g->face);
        if (fr) {
            int k = ml_mila_frame(fr, g->anim_t, g->loop);
            ADD(Z(4, &fr->body.f[k % fr->body.n]), ML_PX_COL, g->x, g->y, 0, NULL, DR_XRAY, 2);
            if (fr->sh.n) ADD(Z(5, &fr->sh.f[k % fr->sh.n]), ML_PX_PLANE, g->x, g->y, 0, NULL, 0, 0);
            if (gl && gl->neck.n) ADD(Z(6, &gl->neck.f[k % gl->neck.n]), ML_PX_LID, g->x, g->y, 0, &c->guest_art.neck_lut, 0, 3);
            if (gl && gl->hat.n) ADD(Z(7, &gl->hat.f[k % gl->hat.n]), ML_PX_LID, g->x, g->y, 0, &c->guest_art.hat_lut, 0, 4);
        }
    }
#undef ADD
#undef F0
#undef Z
    ml_dlist_sort(dl);
}

/* ---- drawing ---- */

static void heart_draw(ml_img_t *im, int cx, int cy, int r, int alpha)
{
    uint16_t c = ml_rgb(255, 110, 150);
    ml_disc(im, (cx - r / 2) * 16, cy * 16, (r * 16) * 6 / 10, c, alpha);
    ml_disc(im, (cx + r / 2) * 16, cy * 16, (r * 16) * 6 / 10, c, alpha);
    for (int y = 0; y < r; y++) {
        int half = r - y;
        ml_rect_blend(im, cx - half, cy + y, half * 2, 1, c, alpha);
    }
}

#define HEART_R     ml_iround(11 * CK)

static void bar_button(app_t *a, ml_img_t *im, int cx, int cy, int ico, const ml_mask_t *sym, const ml_mask_t *lbl, bool lit)
{
    int dy = s_bar.column ? 22 : 30;
    if (!ml_hud_icon(im, &a->hud, ico, cx, cy - dy, 255))
        ml_hud_mask_centered(im, sym, cx, cy - dy, lit ? ml_rgb(255, 205, 80) : 0xFFFF, 255);
    ml_hud_mask_centered(im, lbl, cx, cy + (s_bar.column ? 30 : 38), 0xFFFF, 230);
}

static int nbuttons(app_t *a)
{
    return a->partner_ok || a->link_on ? 4 : 3;
}

/* a sprite of the pack (IMG) with its top-left at (x, y), no depth */
static void put_img(ml_img_t *im, const ml_spr_t *s, int x, int y)
{
    int r0 = im->cy0 - y, r1 = im->cy1 - y;
    if (r0 < 0) r0 = 0;
    if (r1 > s->h) r1 = s->h;
    for (int r = r0; r < r1; r++) {
        int a0, a1;
        const uint8_t *p = ml_spr_row(s, r, &a0, &a1);
        int c0 = x + a0, c1 = x + a1;
        if (c0 < im->cx0) { p += (size_t)(im->cx0 - c0) * 3; c0 = im->cx0; }
        if (c1 > im->cx1) c1 = im->cx1;
        uint16_t *dst = im->px + (size_t)(y + r) * im->w;
        for (int xx = c0; xx < c1; xx++, p += 3)
            if (p[2]) dst[xx] = ml_blend(dst[xx], (uint16_t)(p[0] | (p[1] << 8)), p[2]);
    }
}

static int pill_h(const ml_hud_t *h)
{
    int ph = h->sdig[0].h + 16;
    return ph < 44 ? 44 : ph;
}

void mlc_band(app_t *a, ml_img_t *im, int y0, int y1)
{
    casita_t *c = C(a);
    if (!c) {
        ml_rect(im, im->cx0, y0, im->cx1 - im->cx0, y1 - y0, 0);
        return;
    }
    ml_render_band(&c->w, im, -s_box.x, -s_box.y, y0, y1, &c->dl);
    if (c->logo.n && !s_bar.column) {
        const ml_spr_t *lg = &c->logo.f[0];
        put_img(im, lg, ML_W / 2 - lg->w / 2, LOGO_Y + (LOGO_H - lg->h) / 2);
    }
    for (int i = 0; i < 10; i++)
        if (c->hearts[i].t > 0) {
            int al = c->hearts[i].t > 0.4f ? 255 : (int)(c->hearts[i].t / 0.4f * 255);
            heart_draw(im, (int)c->hearts[i].x, (int)c->hearts[i].y, HEART_R, al);
        }
    const ml_hud_t *h = &a->hud;
    int ph = pill_h(h);
    /* coins on the left, stars on the right of the pills' row */
    if (y0 < s_pill_y + ph) {
        int cy = s_pill_y + ph / 2;
        int wn = ml_hud_number_w(h, a->prog.coins, false);
        ml_hud_pill(im, s_pill_l, s_pill_y, wn + 70, ph, 0, 150);
        ml_disc(im, (s_pill_l + 28) * 16, cy * 16, 14 * 16, ml_rgb(255, 196, 40), 255);
        ml_disc(im, (s_pill_l + 28) * 16, cy * 16, 8 * 16, ml_rgb(255, 225, 120), 255);
        ml_hud_number(h, im, a->prog.coins, s_pill_l + 50, cy - h->sdig[0].h / 2, false, ml_rgb(255, 225, 130), 255);
        int st = ml_prog_total_stars(&a->prog, &a->worlds);
        int ws = ml_hud_number_w(h, st, false);
        ml_hud_pill(im, s_pill_r - ws - 70, s_pill_y, ws + 70, ph, 0, 150);
        ml_hud_star(im, s_pill_r - ws - 42, cy, 14, ml_rgb(255, 205, 70));
        ml_hud_number(h, im, st, s_pill_r - ws - 20, cy - h->sdig[0].h / 2, false, 0xFFFF, 255);
    }
    if (y1 > s_bar.y - 10) {
        ml_rrect(im, s_bar.x, s_bar.y, s_bar.w, s_bar.h, 36, ml_rgb(44, 30, 62), 235);
        int n = nbuttons(a);
        static const int ico[4] = { ICO_PLAY, ICO_SHOP, ICO_GEAR, ICO_LINK };
        static const int sym[4] = { SYM_PLAY, SYM_SHOP, SYM_GEAR, SYM_FRIEND };
        static const int msg[4] = { MSG_PLAY, MSG_SHOP, MSG_SETTINGS, MSG_FRIEND };
        for (int i = 0; i < n; i++) {
            int cx, cy;
            bar_centre(i, n, &cx, &cy);
            bar_button(a, im, cx, cy, ico[i], &h->sym[sym[i]], &h->msg[msg[i]], i == 0);
        }
    }
}

/* what changed since the last frame (worker, after mlc_step) */
void mlc_damage(app_t *a, ml_dmg_t *d)
{
    casita_t *c = C(a);
    if (!c) return;
    ml_dlist_damage(&c->dl_prev, &c->dl, -s_box.x, -s_box.y, d);
    int r = HEART_R * 2 + 4;
    for (int i = 0; i < 10; i++) {
        if (c->heart_was[i].on) ml_dmg_rect(d, c->heart_was[i].x - r, c->heart_was[i].y - r, 2 * r, 2 * r);
        c->heart_was[i].on = c->hearts[i].t > 0;
        c->heart_was[i].x = (int)c->hearts[i].x;
        c->heart_was[i].y = (int)c->hearts[i].y;
        if (c->heart_was[i].on) ml_dmg_rect(d, c->heart_was[i].x - r, c->heart_was[i].y - r, 2 * r, 2 * r);
    }
    int st = ml_prog_total_stars(&a->prog, &a->worlds), n = nbuttons(a);
    if (st != c->hud_stars || a->prog.coins != c->hud_coins) {
        c->hud_stars = st;
        c->hud_coins = a->prog.coins;
        ml_dmg_rect(d, s_pill_l - 4, s_pill_y - 4, s_pill_r - s_pill_l + 8, pill_h(&a->hud) + 8);
    }
    if (n != c->hud_n) {
        c->hud_n = n;
        ml_dmg_rect(d, s_bar.x - 4, s_bar.y - 14, s_bar.w + 8, s_bar.h + 18);
    }
}

/* the screen turned (worker): the room again, where it goes now */
bool mlc_refit(app_t *a)
{
    casita_t *c = C(a);
    if (!c) return true;
    free(c->w.cc);
    free(c->w.cd);
    c->w.cc = c->w.cd = NULL;
    load("casita_room", &c->room);
    bool ok = backdrop(c);
    ml_anim_free(&c->room);
    for (int i = 0; i < 10; i++) c->heart_was[i].on = false;
    return ok;
}

/* ---- touch (LVGL thread) ---- */

void mlc_touch(app_t *a, int code, int x, int y)
{
    casita_t *c = C(a);
    if (code == LV_EVENT_PRESSED) {
        if (c) {
            c->press_x = x;
            c->press_y = y;
            c->pressed = true;
            c->dragged = false;
        }
        return;
    }
    if (!c || !c->pressed) return;
    if (code == LV_EVENT_PRESSING) {
        int dx = x - c->press_x, dy = y - c->press_y;
        if (!c->dragged && dx * dx + dy * dy > 20 * 20 && !in_bar(c->press_x, c->press_y)) c->dragged = true;
        if (c->dragged) {
            c->drag_x = x;
            c->drag_y = y;
            c->drag = true;
        }
        return;
    }
    if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        c->pressed = false;
        c->drag = false;
        if (c->dragged || code == LV_EVENT_PRESS_LOST) return;
        if (in_bar(c->press_x, c->press_y)) {
            int n = nbuttons(a);
            int i = bar_index(c->press_x, c->press_y, n);
            ml_snd(SND_SELECT);
            if (i <= 0) mla_set_state(a, ST_MAP);
            else if (i == 1) mla_set_state(a, ST_SHOP);
            else if (i == 2) mla_set_state(a, ST_SETTINGS);
            else ml_link_begin(a);
            return;
        }
        c->tap_x = c->press_x;
        c->tap_y = c->press_y;
        c->tap = true;
    }
}
