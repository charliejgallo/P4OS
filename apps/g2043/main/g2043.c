/*
 * 2043 - THE BATTLE OF CERES, on the P4OS retro canvas
 *
 * A vertical shoot-'em-up, a shameless homage to Capcom's 1943: an energy bar
 * that drains by itself, capsules dropped by a formation when it falls whole,
 * and a different boss at the end of each planet. It builds two ways from the
 * same source:
 *
 *   .so for the board               apps/g2043 (project_so)
 *   built-in app of the simulator   the simulator builds it (AOS_SIM_BUILTIN)
 *
 * What changed from the watch (AmoledOS apps/g2043):
 *
 *  - The canvas is the OS's (aos_retro.h), and it is the whole screen: 240x426
 *    shown x3 (720x1278) in portrait, 240x360 x2 (480x720) centred in
 *    landscape. The game redraws all of it every frame, straight into the
 *    service's buffer, and presents only the tiles that changed
 *    (present_changed()); the OS scales those. The x2 upscale by hand
 *    (expand_gx) and its 330 KB buffer are gone; the screen shake and the
 *    white flash it applied on the way are now gx_shake_flash(), in place,
 *    only on the frames that shake or flash.
 *  - The field is 240 wide instead of 184 and nearly twice as tall, so the
 *    waves, the rows enemies settle on, the bosses' sweeps, the scenery and
 *    the HUD (now two rows at the top) were laid out again for it.
 *  - Controls, made for a touch screen with no buttons ("Controls" below):
 *    a finger dragged anywhere flies the ship, which moves with the finger
 *    instead of sitting under it, and two round buttons under the other
 *    thumb fire and do the barrel roll. The pause is a button in the top
 *    right corner. They are the app's own and not the OS's pad, because the
 *    drag and the buttons share the field and need both fingers told apart
 *    (see the comment there). The AUTO chip stays, off by default: with it
 *    the ship fires by itself and the fire button says AUTO.
 *  - The pace is the service's fixed tick (30 steps a second, the watch's
 *    speed) instead of the self-tuning timer; the expensive decorations still
 *    switch off by themselves if the frames per second drop.
 *  - Resuming from the pause goes back to the state it was in (on the watch it
 *    went to PLAY or BOSS, which from the explosion cost a second ship), and
 *    winning the last planet ends on the ending panel instead of on the
 *    banner forever.
 *
 * The record and the switches are the same preferences as on the watch.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_retro.h"
#include "aos_pad_menu.h"

#include "g2043.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* --------------------------------------------------------------------------
 * Settings
 * -------------------------------------------------------------------------- */

#define PLAYER_R        4           /* the player's hit box: deliberately small */
#define PLAYER_SPEED    80          /* top speed towards the drag, 1/16 of a pixel per frame */
#define DRAG_GAIN_NUM   3           /* the ship moves 3/2 of what the finger moves, */
#define DRAG_GAIN_DEN   2           /* so a thumb's reach crosses the whole field  */
#define ROLL_FRAMES     22
#define ROLL_COOLDOWN   70
#define HIT_INVULN      26
#define RESPAWN_INVULN  70
#define DRAIN_EVERY     8           /* frames per unit of energy */

#define KEY_HI          "g2043_hi"
/* "_auto2": a build of 2043-on-P4OS had AUTO on by default and saved it on
 * the first JUGAR, so on the board the ship fired by itself and it looked
 * like a stuck button. A new key starts every install with AUTO off. */
#define KEY_AUTO        "g2043_auto2"
#define KEY_SFX         "g2043_sfx"
#define KEY_FPS         "g2043_fps"
/* 0 = present the whole canvas every frame, as before the tiles (for
 * measuring on the board; nothing in the app writes it) */
#define KEY_DIFF        "g2043_diff"

/* The field's height, in the orientation the app was opened in (gx_pixel.h).
 * The runtime creates the app again when the screen turns. */
int gx_h = GX_H_PORTRAIT;

/* The on-screen controls (see "Controls"). */
enum { CB_FIRE = 0, CB_ROLL, CB_PAUSE, CB_COUNT };

typedef struct {
    lv_obj_t *obj;
    int16_t   cx, cy;           /* centre, root coordinates */
    int16_t   r;                /* touch radius: the drawing plus a margin */
} ctl_btn_t;

/* A rectangle of the canvas's 8x8 tiles, inclusive (see present_changed()). */
typedef struct {
    int16_t x0, y0, x1, y1;         /* tiles, inclusive */
} trect_t;

typedef struct {
    g_t         g;
    const aos_retro_t *r;
    lv_obj_t   *root;
    lv_obj_t   *stage;          /* the panels' parent: exactly over the canvas */
    bool        land;           /* opened in landscape */

    /* the controls: their layer, the buttons, and the fingers */
    lv_obj_t   *ctl;
    ctl_btn_t   btn[CB_COUNT];
    uint8_t     btn_held;       /* 1 << CB_*, held in the last touch sample */
    uint8_t     btn_down;       /* went down since the last step */
    uint8_t     btn_shown;      /* what the buttons show as pressed */
    uint8_t     roll_dim;       /* the roll button shows its cooldown */
    uint8_t     auto_shown;     /* the fire button shows AUTO (0xFF = not yet) */
    bool        live;           /* the controls were read in the last step */
    uint32_t    touch_seq;      /* the last touch sample read */
    bool        fly_on;         /* a finger is flying the ship */
    int16_t     fly_sx, fly_sy; /* where that finger was, root coordinates */
    int32_t     fly_fx, fly_fy; /* ... where it landed, canvas 1/16 px */
    int32_t     fly_px, fly_py; /* ... and where the ship was then */

    /* LVGL panels above the canvas */
    lv_obj_t   *title;
    lv_obj_t   *title_hi;
    lv_obj_t   *chip_auto;
    lv_obj_t   *chip_sfx;
    lv_obj_t   *pause;
    lv_obj_t   *chip_auto2;
    lv_obj_t   *chip_sfx2;
    lv_obj_t   *chip_fps2;
    lv_obj_t   *over;
    lv_obj_t   *over_title;
    lv_obj_t   *over_score;

    /* What the screen shows of the canvas, as last presented: each frame
     * only what differs from it is presented (present_changed()). */
    uint16_t   *shown;
    bool        shown_ok;       /* false: present all of it next time */
    bool        diff_on;        /* the preference g2043_diff (default 1) */
    uint8_t     probe_t;        /* frames to go before comparing again */
    uint16_t    share_avg;      /* the share presented, %, smoothed x16 */
    uint8_t     full_run;       /* compares in a row that presented it all */
    uint32_t    prof_n, prof_every, prof_cmp;   /* the profile, see draw() */
    uint64_t    prof_draw_us, prof_diff_us, prof_px, prof_rects, prof_tiles;

    uint8_t     paused_from;    /* the state the pause interrupted */

    /* A USB gamepad (see "The gamepad"): its state, the panel's buttons it
     * goes through, and those buttons per panel */
    aos_pad_t   pad;
    aos_pad_menu_t menu;
    uint8_t     pad_btn;        /* 1 << CB_* held on the pad (the buttons light up) */
    lv_obj_t   *pm_title[4], *pm_pause[5], *pm_over[3];
    int16_t     detail_t;       /* draws until the next quality check */

    bool        want_exit;
} app_t;

/* --------------------------------------------------------------------------
 * Utilities
 * -------------------------------------------------------------------------- */

uint32_t g_rnd(g_t *g)
{
    uint32_t x = g->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g->rng = x;
    return x;
}

int g_rnd_range(g_t *g, int lo, int hi)
{
    if (hi <= lo) {
        return lo;
    }
    return lo + (int)(g_rnd(g) % (uint32_t)(hi - lo + 1));
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* --------------------------------------------------------------------------
 * Numbers to text, by hand
 *
 * The HUD is drawn on every frame and called snprintf four times, which in
 * newlib takes several hundred bytes of stack. And the stack is the LVGL
 * task's, the same one that afterwards draws the whole screen. Putting digits
 * into a buffer does not need that much.
 * -------------------------------------------------------------------------- */

static char *num_pad(char *dst, uint32_t v, int digits)
{
    for (int i = digits - 1; i >= 0; i--) {
        dst[i] = (char)('0' + v % 10);
        v /= 10;
    }
    dst[digits] = '\0';
    return dst + digits;
}

static char *num_str(char *dst, uint32_t v)
{
    char tmp[12];
    int n = 0;
    do {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && n < (int)sizeof(tmp));

    for (int i = 0; i < n; i++) {
        dst[i] = tmp[n - 1 - i];
    }
    dst[n] = '\0';
    return dst + n;
}

/* The beeps are limited to one every 45 ms: in the middle of a firefight there
 * would be hundreds a second, and in the simulator each one also prints a
 * line. */
static bool s_sfx = true;

void g_beep(int freq, int ms)
{
    static uint64_t last;
    if (!s_sfx) {
        return;
    }
    uint64_t now = aos_hal_uptime_ms();
    if (now - last < 45) {
        return;
    }
    last = now;
    aos_hal_beep(freq, ms);
}

void g_add_score(g_t *g, int points)
{
    g->score += (uint32_t)points;
    if (g->score > g->hiscore) {
        g->hiscore = g->score;
    }
}

int g_angle_to_player(const g_t *g, int x, int y)
{
    return gx_atan2(g->py - y, g->px - x);
}

g_shot_t *g_spawn_eshot(g_t *g, int x, int y, int vx, int vy, int kind)
{
    for (int i = 0; i < G_MAX_ESHOTS; i++) {
        g_shot_t *s = &g->eshots[i];
        if (s->alive) {
            continue;
        }
        s->x = (int16_t)x;
        s->y = (int16_t)y;
        s->vx = (int16_t)vx;
        s->vy = (int16_t)vy;
        s->kind = (uint8_t)kind;
        s->t = 0;
        s->alive = 1;
        return s;
    }
    return NULL;
}

void g_shoot_at_player(g_t *g, int x, int y, int speed, int kind)
{
    int ang = g_angle_to_player(g, x, y);
    g_spawn_eshot(g, x, y, gx_cos(ang) * speed / 256, gx_sin(ang) * speed / 256, kind);
    g_beep(260, 20);
}

g_fx_t *g_spawn_fx(g_t *g, int x, int y, int vx, int vy, int kind,
                   uint16_t color, int life)
{
    g_fx_t *slot = NULL;
    int oldest = -1;

    for (int i = 0; i < G_MAX_FX; i++) {
        if (!g->fx[i].alive) {
            slot = &g->fx[i];
            break;
        }
        /* if there is no room, the oldest is overwritten: better to lose a
         * spark than to stop drawing the new explosion */
        if (g->fx[i].t > oldest) {
            oldest = g->fx[i].t;
            slot = &g->fx[i];
        }
    }
    if (!slot) {
        return NULL;
    }

    slot->x = (int16_t)x;
    slot->y = (int16_t)y;
    slot->vx = (int16_t)vx;
    slot->vy = (int16_t)vy;
    slot->kind = (uint8_t)kind;
    slot->color = color;
    slot->t = 0;
    slot->life = (int16_t)life;
    slot->text = NULL;
    slot->alive = 1;
    return slot;
}

void g_boom(g_t *g, int x, int y, int radius, uint16_t color)
{
    g_fx_t *f = g_spawn_fx(g, FX(x), FX(y), 0, 0, FX_BOOM, color, radius);
    if (f) {
        f->life = (int16_t)radius;
    }
    g_spawn_fx(g, FX(x), FX(y), 0, 0, FX_RING, color, radius + 6);

    int n = radius / 3 + 2;
    for (int i = 0; i < n; i++) {
        int ang = (int)(g_rnd(g) % 256);
        int sp = g_rnd_range(g, 8, 26);
        g_spawn_fx(g, FX(x), FX(y), gx_cos(ang) * sp / 256, gx_sin(ang) * sp / 256,
                   FX_DEBRIS, color, g_rnd_range(g, 8, 18));
    }
}

void g_drop_pickup(g_t *g, int x, int y, int kind)
{
    for (int i = 0; i < G_MAX_PICKUPS; i++) {
        g_pickup_t *p = &g->pickups[i];
        if (p->alive) {
            continue;
        }
        p->x = (int16_t)x;
        p->y = (int16_t)y;
        p->vy = 10;
        p->kind = (uint8_t)kind;
        p->t = 0;
        p->alive = 1;
        return;
    }
}

/* --------------------------------------------------------------------------
 * Capsules
 * -------------------------------------------------------------------------- */

static const uint32_t pickup_color[PU_COUNT] = {
    0x4A9DF5,   /* PU_TWIN   */
    0xFF9F0A,   /* PU_SPREAD */
    0xFF2D55,   /* PU_LASER  */
    0xB072F0,   /* PU_WAVE   */
    0x30D158,   /* PU_ENERGY */
    0x7BE9FF,   /* PU_SHIELD */
    0xFFE45E,   /* PU_LIFE   */
};

static const char pickup_letter[PU_COUNT] = { 'D', 'T', 'L', 'O', 'E', 'S', '1' };

/* Note: this is drawn with gx_text(), the game's own 5x7 font, which is ASCII
 * 0x20-0x5F and UPPER CASE ONLY -it silently discards any other byte- and
 * gx_text_w() measures with strlen, that is, in bytes. The translations of
 * everything that goes to the canvas have to stay upper case without
 * accents. */
static const char *const pickup_name[PU_COUNT] = {
    N_("DOBLE"), N_("TRIPLE"), N_("LASER"), N_("ONDA"), N_("ENERGIA"),
    N_("ESCUDO"), N_("NAVE EXTRA"),
};

static void pickup_take(g_t *g, g_pickup_t *p)
{
    static const uint8_t as_weapon[PU_COUNT] = {
        W_TWIN, W_SPREAD, W_LASER, W_WAVE, 0xFF, 0xFF, 0xFF,
    };
    uint8_t w = as_weapon[p->kind];

    if (w != 0xFF) {
        if (g->weapon == w) {
            if (g->wlevel < 3) {
                g->wlevel++;
            } else {
                g_add_score(g, 2000);
            }
        } else {
            g->weapon = w;
            g->wlevel = 1;
        }
    } else if (p->kind == PU_ENERGY) {
        g->energy = (int16_t)clampi(g->energy + 90, 0, G_ENERGY_MAX);
    } else if (p->kind == PU_SHIELD) {
        g->shield = 380;
    } else {
        if (g->lives < 6) {
            g->lives++;
        } else {
            g_add_score(g, 5000);
        }
    }

    g_fx_t *f = g_spawn_fx(g, p->x, p->y - FX(6), 0, -6, FX_TEXT,
                           gx_rgb(pickup_color[p->kind]), 34);
    if (f) {
        f->text = _(pickup_name[p->kind]);
    }
    g_add_score(g, 500);
    aos_hal_beep(1400, 40);
    p->alive = 0;
}

static void pickup_draw(g_t *g, const g_pickup_t *p)
{
    int x = UNFX(p->x), y = UNFX(p->y);
    uint16_t col = gx_rgb(pickup_color[p->kind]);
    /* it pulses so it can be seen through the gunfire */
    int f = 6 + gx_sin(p->t * 9) / 42;

    gx_glow(&g->buf, x, y, 9, col, f);
    gx_round(&g->buf, x - 6, y - 6, 13, 13, 3, gx_rgb(0x0A0A12));
    gx_round(&g->buf, x - 5, y - 5, 11, 11, 2, col);
    gx_round(&g->buf, x - 4, y - 4, 9, 9, 2, gx_rgb(0x0A0A12));

    char s[2] = { pickup_letter[p->kind], 0 };
    gx_text(&g->buf, x - 2, y - 3, s, col);
}

/* --------------------------------------------------------------------------
 * Waves
 * -------------------------------------------------------------------------- */

static g_enemy_t *enemy_spawn(g_t *g, int kind, int x, int y, int wave)
{
    for (int i = 0; i < G_MAX_ENEMIES; i++) {
        g_enemy_t *e = &g->enemies[i];
        if (e->alive) {
            continue;
        }
        memset(e, 0, sizeof(*e));
        e->kind  = (uint8_t)kind;
        e->x     = (int16_t)x;
        e->y     = (int16_t)y;
        e->hp    = (int16_t)gx_enemy_hp(kind);
        e->wave  = (uint8_t)wave;
        e->a     = (int16_t)(g_rnd(g) % 256);
        e->vy    = 34;
        e->alive = 1;
        return e;
    }
    return NULL;
}

static void wave_spawn(g_t *g, const g_wave_t *w, int id)
{
    int base = w->x * GX_W / 100;
    int n = w->count;

    g->wave_left[id] = 0;
    g->wave_gift[id] = w->gift;

    for (int i = 0; i < n; i++) {
        int x = base, y = -12;
        int vx = 0, vy = 34;

        switch (w->form) {
        case FORM_COLUMN:
            y = -12 - i * 17;
            break;
        case FORM_ROW:
            x = base - (n - 1) * 8 + i * 16;
            break;
        case FORM_V:
            x = base + (i - (n - 1) / 2) * 15;
            y = -12 - (i > (n - 1) / 2 ? i - (n - 1) / 2 : (n - 1) / 2 - i) * 11;
            break;
        /* the side waves cross at the height their 'x' says: on the tall
         * field they no longer all hug the top edge */
        case FORM_SIDE_L:
            x = -12 - i * 18;
            y = G_ROW(w->x) + i * 5;
            vx = 44;
            vy = 7;
            break;
        case FORM_SIDE_R:
            x = GX_W + 12 + i * 18;
            y = G_ROW(w->x) + i * 5;
            vx = -44;
            vy = 7;
            break;
        default:    /* FORM_SPREAD */
            x = 18 + (GX_W - 36) * i / (n > 1 ? n - 1 : 1);
            y = -12 - (int)(g_rnd(g) % 26);
            break;
        }

        if (w->kind == EN_TURRET) {
            /* the turret moves down with the terrain, it does not fly */
            vy = (int16_t)gx_levels[g->level].scroll;
            vx = 0;
        }

        g_enemy_t *e = enemy_spawn(g, w->kind, FX(clampi(x, -30, GX_W + 30)),
                                   (int16_t)(y * FX_ONE), id);
        if (!e) {
            break;
        }
        e->vx = (int16_t)vx;
        e->vy = (int16_t)vy;
        g->wave_left[id]++;
    }
}

static void waves_update(g_t *g)
{
    const g_level_t *lv = &gx_levels[g->level];
#ifdef AOS_SIM_BUILTIN
    if (getenv("G2043_TRACE") && (g->level_t % (getenv("G2043_TRACE")[0] == 'f' ? 3 : 30)) == 0) {
        int n = 0;
        for (int i = 0; i < G_MAX_ENEMIES; i++) if (g->enemies[i].alive) n++;
        printf("[2043] t=%d estado=%d oleada=%d/%d vivos=%d fuego=%d\n",
               (int)g->level_t, g->state, g->wave_next, lv->wave_count, n, g->fire_down);
    }
#endif

    while (g->wave_next < lv->wave_count &&
           lv->waves[g->wave_next].at <= g->level_t) {
        wave_spawn(g, &lv->waves[g->wave_next], (int)g->wave_next + 1);
        g->wave_next++;
    }
}

/* --------------------------------------------------------------------------
 * The player's shots
 * -------------------------------------------------------------------------- */

static g_shot_t *pshot(g_t *g, int x, int y, int vx, int vy, int kind)
{
    for (int i = 0; i < G_MAX_PSHOTS; i++) {
        g_shot_t *s = &g->pshots[i];
        if (s->alive) {
            continue;
        }
        s->x = (int16_t)x;
        s->y = (int16_t)y;
        s->vx = (int16_t)vx;
        s->vy = (int16_t)vy;
        s->kind = (uint8_t)kind;
        s->t = 0;
        s->alive = 1;
        return s;
    }
    return NULL;
}

static int pshot_damage(const g_t *g, int kind)
{
    switch (kind) {
    case PS_LASER: return 2 + g->wlevel / 2;
    case PS_WAVE:  return 3 + g->wlevel;
    default:       return 1;
    }
}

static int pshot_radius(int kind)
{
    switch (kind) {
    case PS_LASER: return 2;
    case PS_WAVE:  return 7;
    default:       return 2;
    }
}

static void player_fire(g_t *g)
{
    int x = g->px, y = g->py - FX(7);
    int lv = g->wlevel;

    switch (g->weapon) {
    case W_TWIN:
        for (int i = -1; i <= 1; i += 2) {
            pshot(g, x + i * FX(3), y, 0, -66, PS_BULLET);
            pshot(g, x + i * FX(8), y + FX(3), i * 3, -62, PS_BULLET);
        }
        g->fire_cd = (int16_t)(9 - lv);
        break;

    case W_SPREAD: {
        int n = lv >= 3 ? 5 : 3;
        for (int i = 0; i < n; i++) {
            int ang = 192 + (i - (n - 1) / 2) * 11;      /* 192 brads = up */
            pshot(g, x, y, gx_cos(ang) * 56 / 256, gx_sin(ang) * 56 / 256, PS_BULLET);
        }
        g->fire_cd = (int16_t)(12 - lv);
        break;
    }

    case W_LASER:
        pshot(g, x, y, 0, -118, PS_LASER);
        if (lv >= 2) {
            pshot(g, x - FX(6), y + FX(4), 0, -118, PS_LASER);
            pshot(g, x + FX(6), y + FX(4), 0, -118, PS_LASER);
        }
        g->fire_cd = (int16_t)(9 - lv);
        break;

    case W_WAVE:
        pshot(g, x, y, 0, -40, PS_WAVE);
        if (lv >= 3) {
            pshot(g, x - FX(10), y + FX(6), -4, -38, PS_WAVE);
            pshot(g, x + FX(10), y + FX(6), 4, -38, PS_WAVE);
        }
        g->fire_cd = (int16_t)(20 - lv * 2);
        break;

    default:    /* W_SHOT */
        pshot(g, x - FX(4), y, 0, -64, PS_BULLET);
        pshot(g, x + FX(4), y, 0, -64, PS_BULLET);
        if (lv >= 3) {
            pshot(g, x - FX(9), y + FX(4), -5, -60, PS_BULLET);
            pshot(g, x + FX(9), y + FX(4), 5, -60, PS_BULLET);
        }
        g->fire_cd = (int16_t)(10 - lv * 2);
        break;
    }

    g_beep(1500 + g->weapon * 120, 10);
}

/* --------------------------------------------------------------------------
 * The player
 * -------------------------------------------------------------------------- */

static void player_reset(g_t *g, bool full)
{
    g->px = FX(GX_W / 2);
    g->py = FX(g->home_y);
    g->pvx = g->pvy = 0;
    g->energy = G_ENERGY_MAX;
    g->invuln = RESPAWN_INVULN;
    g->roll = 0;
    g->roll_cd = 0;
    g->shield = 0;
    if (full) {
        g->weapon = W_SHOT;
        g->wlevel = 1;
    }
}

static void player_move(g_t *g)
{
    /* The drag gives a point the ship is drawn towards (read_input() works
     * it out from the finger); without a finger the ship eases to a stop
     * where it is, with the same inertia as always. */
    int tx = g->px, ty = g->py;

    if (g->touching) {
        tx = g->touch_x;
        ty = g->touch_y;
    }
#ifdef AOS_SIM_BUILTIN
    else if (g->autoplay) {
        /* G2043_AUTO: a slow figure of eight over the lower half, for leaving
         * the game running and taking screenshots mid-battle */
        int t = g->auto_t++;
        tx = FX(GX_W / 2) + gx_sin(t) * FX(GX_W / 2 - 20) / 256;
        ty = FX(g->home_y) + gx_sin(t * 2) * FX(GX_H / 8) / 256;
    }
#endif

    int tvx = clampi((tx - g->px) / 2, -PLAYER_SPEED, PLAYER_SPEED);
    int tvy = clampi((ty - g->py) / 2, -PLAYER_SPEED, PLAYER_SPEED);

    /* smoothing: the ship has some inertia and does not teleport */
    g->pvx += (int16_t)((tvx - g->pvx) / 2);
    g->pvy += (int16_t)((tvy - g->pvy) / 2);

    g->px = (int16_t)(g->px + g->pvx);
    g->py = (int16_t)(g->py + g->pvy);

    g->px = (int16_t)clampi(g->px, FX(9), FX(GX_W - 9));
    g->py = (int16_t)clampi(g->py, FX(G_PLAY_Y0 + 9), FX(G_PLAY_Y1 - 7));
}

static void player_die(g_t *g)
{
    g_boom(g, UNFX(g->px), UNFX(g->py), 18, gx_rgb(0x7BE9FF));
    g->shake = 12;
    g->flash_screen = 5;
    aos_hal_beep(90, 200);

    g->lives--;
    g->state = ST_DEAD;
    g->state_t = 0;
}

static void player_hurt(g_t *g, int amount)
{
    if (g->invuln > 0 || g->roll > 0) {
        return;
    }
    if (g->shield > 0) {
        g->shield = 0;
        g->invuln = HIT_INVULN;
        g_boom(g, UNFX(g->px), UNFX(g->py), 10, gx_rgb(0x7BE9FF));
        aos_hal_beep(700, 60);
        return;
    }

    g->energy = (int16_t)(g->energy - amount);
    g->invuln = HIT_INVULN;
    g->shake = 6;
    aos_hal_beep(200, 70);

    if (g->energy <= 0) {
        g->energy = 0;
        player_die(g);
    }
}

static void player_draw(g_t *g)
{
    int x = UNFX(g->px), y = UNFX(g->py);

    /* nor behind the end panel after the last ship blew up (the watch drew
     * it again there, at the spot where it exploded) */
    if (g->state == ST_DEAD || (g->state == ST_GAMEOVER && g->lives <= 0)) {
        return;
    }
    /* it blinks while invulnerable, except mid-barrel-roll */
    if (g->invuln > 0 && !g->roll && (g->invuln / 3) % 2) {
        return;
    }

    /* engine flame: two tongues that flicker */
    int flame = 4 + (int)(g_rnd(g) % 3);
    for (int i = -1; i <= 1; i += 2) {
        gx_glow(&g->buf, x + i * 4, y + 8, flame, gx_rgb(0xFF9F0A), 12);
        gx_vline(&g->buf, x + i * 4, y + 7, flame, gx_rgb(0xFFE45E));
    }

    if (g->roll > 0) {
        /* barrel roll: the ship spins about its axis, that is, it is seen to
         * narrow */
        int phase = (ROLL_FRAMES - g->roll) * 256 / ROLL_FRAMES;
        int w = 15 * (gx_cos(phase) < 0 ? -gx_cos(phase) : gx_cos(phase)) / 256;
        gx_glow(&g->buf, x, y, 12, gx_rgb(0x7BE9FF), 7);
        gx_blit_c_xscale(&g->buf, x, y, gx_art_ship.rows, gx_art_ship.n,
                         w < 2 ? 2 : w);
    } else {
        gx_blit_c(&g->buf, x, y, gx_art_ship.rows, gx_art_ship.n);
    }

    if (g->shield > 0 && (g->shield > 90 || (g->shield / 4) % 2)) {
        int r = 13 + gx_sin(g->state_t * 8) / 90;
        gx_ring(&g->buf, x, y, r, gx_rgb(0x7BE9FF));
        gx_glow(&g->buf, x, y, r, gx_rgb(0x7BE9FF), 4);
    }
}

/* --------------------------------------------------------------------------
 * Shots and effects: advancing and drawing
 * -------------------------------------------------------------------------- */

static void shots_update(g_t *g)
{
    for (int i = 0; i < G_MAX_PSHOTS; i++) {
        g_shot_t *s = &g->pshots[i];
        if (!s->alive) {
            continue;
        }
        s->x = (int16_t)(s->x + s->vx);
        s->y = (int16_t)(s->y + s->vy);
        s->t++;
        if (UNFX(s->y) < G_PLAY_Y0 - 8 || UNFX(s->x) < -6 || UNFX(s->x) > GX_W + 6) {
            s->alive = 0;
        }
    }

    for (int i = 0; i < G_MAX_ESHOTS; i++) {
        g_shot_t *s = &g->eshots[i];
        if (!s->alive) {
            continue;
        }
        if (s->kind == ES_HOMING && s->t < 60) {
            /* it corrects course a little at a time: you have to move, dodging
             * is not enough */
            int ang = g_angle_to_player(g, s->x, s->y);
            s->vx = (int16_t)(s->vx + (gx_cos(ang) * 22 / 256 - s->vx) / 12);
            s->vy = (int16_t)(s->vy + (gx_sin(ang) * 22 / 256 - s->vy) / 12);
        }
        s->x = (int16_t)(s->x + s->vx);
        s->y = (int16_t)(s->y + s->vy);
        s->t++;

        int x = UNFX(s->x), y = UNFX(s->y);
        if (y > G_PLAY_Y1 + 10 || y < G_PLAY_Y0 - 14 || x < -10 || x > GX_W + 10) {
            s->alive = 0;
        }
    }
}

static void shots_draw(g_t *g)
{
    for (int i = 0; i < G_MAX_PSHOTS; i++) {
        const g_shot_t *s = &g->pshots[i];
        if (!s->alive) {
            continue;
        }
        int x = UNFX(s->x), y = UNFX(s->y);

        switch (s->kind) {
        case PS_LASER:
            gx_vline(&g->buf, x, y - 5, 11, gx_rgb(0xFF2D55));
            gx_vline(&g->buf, x - 1, y - 3, 7, gx_rgb(0xFF6FAE));
            gx_vline(&g->buf, x + 1, y - 3, 7, gx_rgb(0xFF6FAE));
            gx_glow(&g->buf, x, y, 4, gx_rgb(0xFF2D55), 8);
            break;
        case PS_WAVE: {
            int r = 5 + (s->t / 4 > 3 ? 3 : s->t / 4);
            gx_glow(&g->buf, x, y, r + 3, gx_rgb(0xB072F0), 10);
            gx_ring(&g->buf, x, y, r, gx_rgb(0xE0C0FF));
            gx_ring(&g->buf, x, y, r - 2 > 0 ? r - 2 : 1, gx_rgb(0xB072F0));
            break;
        }
        default:
            if (g->detail) {
                gx_glow(&g->buf, x, y, 3, gx_rgb(0x7BE9FF), 8);
            }
            gx_rect(&g->buf, x - 1, y - 3, 3, 6, gx_rgb(0xFFFFFF));
            gx_px(&g->buf, x, y - 4, gx_rgb(0x7BE9FF));
            break;
        }
    }

    for (int i = 0; i < G_MAX_ESHOTS; i++) {
        const g_shot_t *s = &g->eshots[i];
        if (!s->alive) {
            continue;
        }
        int x = UNFX(s->x), y = UNFX(s->y);

        switch (s->kind) {
        case ES_HEAVY:
            gx_glow(&g->buf, x, y, 6, gx_rgb(0xFF4A3D), 11);
            gx_disc(&g->buf, x, y, 3, gx_rgb(0xFFE45E));
            gx_ring(&g->buf, x, y, 3, gx_rgb(0xFF4A3D));
            break;
        case ES_ROCK: {
            const gx_sprite_t *sp = &gx_art_rock[2];
            gx_blit_c(&g->buf, x, y, sp->rows, sp->n);
            break;
        }
        case ES_HOMING:
            gx_glow(&g->buf, x, y, 5, gx_rgb(0x2AF0C8), 12);
            gx_disc(&g->buf, x, y, 2, gx_rgb(0xFFFFFF));
            gx_ring(&g->buf, x, y, 3, gx_rgb(0x2AF0C8));
            break;
        default:
            if (g->detail) {
                gx_glow(&g->buf, x, y, 4, gx_rgb(0xFF9F0A), 10);
            }
            gx_disc(&g->buf, x, y, 2, gx_rgb(0xFFE45E));
            gx_ring(&g->buf, x, y, 2, gx_rgb(0xFF9F0A));
            break;
        }
    }
}

static void fx_update(g_t *g)
{
    for (int i = 0; i < G_MAX_FX; i++) {
        g_fx_t *f = &g->fx[i];
        if (!f->alive) {
            continue;
        }
        f->t++;
        f->x = (int16_t)(f->x + f->vx);
        f->y = (int16_t)(f->y + f->vy);

        switch (f->kind) {
        case FX_DEBRIS:
            f->vy = (int16_t)(f->vy + 1);       /* they are a little heavy */
            f->vx = (int16_t)(f->vx * 15 / 16);
            if (f->t > f->life) f->alive = 0;
            break;
        case FX_BOOM:
            if (f->t > f->life / 2 + 6) f->alive = 0;
            break;
        case FX_RING:
            if (f->t > 10) f->alive = 0;
            break;
        default:
            if (f->t > f->life) f->alive = 0;
            break;
        }
    }
}

static void fx_draw(g_t *g)
{
    for (int i = 0; i < G_MAX_FX; i++) {
        const g_fx_t *f = &g->fx[i];
        if (!f->alive) {
            continue;
        }
        int x = UNFX(f->x), y = UNFX(f->y);

        switch (f->kind) {
        case FX_BOOM: {
            int r = f->life * f->t / (f->life / 2 + 6);
            if (r < 1) r = 1;
            gx_glow(&g->buf, x, y, r + 4, f->color, 14);
            gx_disc(&g->buf, x, y, r / 2 + 1, gx_rgb(0xFFFFFF));
            gx_ring(&g->buf, x, y, r, f->color);
            break;
        }
        case FX_RING: {
            int r = f->life * f->t / 10;
            gx_ring(&g->buf, x, y, r, gx_mix(f->color, gx_rgb(0xFFFFFF), 8));
            break;
        }
        case FX_TEXT:
            if (f->text) {
                gx_text_center(&g->buf, x, y, f->text, f->color, gx_rgb(0x0A0A12));
            }
            break;
        default:
            gx_px(&g->buf, x, y, gx_rgb(0xFFFFFF));
            gx_px(&g->buf, x + 1, y, f->color);
            gx_px(&g->buf, x, y + 1, f->color);
            break;
        }
    }
}

/* --------------------------------------------------------------------------
 * Collisions
 * -------------------------------------------------------------------------- */

static bool near(int ax, int ay, int bx, int by, int r)
{
    int dx = ax - bx, dy = ay - by;
    return dx * dx + dy * dy <= r * r;
}

static void enemy_kill(g_t *g, g_enemy_t *e)
{
    gx_enemy_died(g, e);
    g_add_score(g, gx_enemy_score(e->kind));

    int id = e->wave;
    e->alive = 0;

    if (id && g->wave_left[id] > 0 && --g->wave_left[id] == 0 && g->wave_gift[id]) {
        /* the formation fell whole: the capsule drops, as in 1943 */
        g_drop_pickup(g, e->x, e->y, g->wave_gift[id] - 1);
        g->wave_gift[id] = 0;
    }
}

static void collisions(g_t *g)
{
    /* the player's shots against enemies and the boss */
    for (int i = 0; i < G_MAX_PSHOTS; i++) {
        g_shot_t *s = &g->pshots[i];
        if (!s->alive) {
            continue;
        }
        int sx = UNFX(s->x), sy = UNFX(s->y);
        int sr = pshot_radius(s->kind);
        int dmg = pshot_damage(g, s->kind);
        bool pierce = (s->kind == PS_LASER || s->kind == PS_WAVE);

        for (int j = 0; j < G_MAX_ENEMIES; j++) {
            g_enemy_t *e = &g->enemies[j];
            if (!e->alive) {
                continue;
            }
            if (!near(sx, sy, UNFX(e->x), UNFX(e->y), sr + gx_enemy_radius(e->kind))) {
                continue;
            }
            e->hp = (int16_t)(e->hp - dmg);
            e->flash = 2;
            if (e->hp <= 0) {
                enemy_kill(g, e);
            } else {
                g_spawn_fx(g, s->x, s->y, 0, 0, FX_SPARK, gx_rgb(0xFFE45E), 4);
            }
            if (!pierce) {
                s->alive = 0;
                break;
            }
        }
        if (!s->alive) {
            continue;
        }
        if (gx_boss_hit(g, sx, sy, dmg) && !pierce) {
            s->alive = 0;
        }
    }

    if (g->state == ST_DEAD) {
        return;
    }

    int px = UNFX(g->px), py = UNFX(g->py);

    /* enemy shots against the player */
    for (int i = 0; i < G_MAX_ESHOTS; i++) {
        g_shot_t *s = &g->eshots[i];
        if (!s->alive) {
            continue;
        }
        if (near(UNFX(s->x), UNFX(s->y), px, py, PLAYER_R + 3)) {
            s->alive = 0;
            player_hurt(g, 26);
            if (g->state == ST_DEAD) {
                return;
            }
        }
    }

    /* collision with enemies */
    for (int i = 0; i < G_MAX_ENEMIES; i++) {
        g_enemy_t *e = &g->enemies[i];
        if (!e->alive) {
            continue;
        }
        if (!near(UNFX(e->x), UNFX(e->y), px, py, PLAYER_R + gx_enemy_radius(e->kind))) {
            continue;
        }
        if (g->invuln <= 0 && g->roll <= 0 && g->shield <= 0) {
            e->hp = (int16_t)(e->hp - 3);
            if (e->hp <= 0) {
                enemy_kill(g, e);
            }
        }
        player_hurt(g, 42);
        if (g->state == ST_DEAD) {
            return;
        }
    }

    /* collision with the boss */
    if (g->boss.alive && !g->boss.dying) {
        const gx_boss_def_t *d = &gx_bosses[g->boss.def];
        for (int part = 0; part <= g->boss.parts; part++) {
            int bx, by, br;
            d->hitbox(&g->boss, part, &bx, &by, &br);
            if (br > 0 && near(bx, by, px, py, PLAYER_R + br)) {
                player_hurt(g, 46);
                break;
            }
        }
    }

    /* capsules */
    for (int i = 0; i < G_MAX_PICKUPS; i++) {
        g_pickup_t *p = &g->pickups[i];
        if (p->alive && near(UNFX(p->x), UNFX(p->y), px, py, 11)) {
            pickup_take(g, p);
        }
    }
}

/* --------------------------------------------------------------------------
 * HUD
 * -------------------------------------------------------------------------- */

/* Two rows at the top. The right end of both is left clear: the pause
 * button sits there, over the canvas, in portrait. */
#define HUD_ROW1        3
#define HUD_ROW2        14
#define HUD_RIGHT       (GX_W - 30)     /* the pause button starts here */

static void hud_draw(g_t *g)
{
    gx_buf_t *b = &g->buf;
    char buf[32];

    /* band: darken instead of covering, so the background stays alive behind */
    gx_shade(b, 0, 0, GX_W, G_HUD_H, -11);
    gx_hline(b, 0, G_HUD_H, GX_W, gx_rgb(0x2A3145));

    /* the score wraps at 999999, as it should */
    num_pad(buf, g->score % 1000000u, 6);
    gx_text_sh(b, 4, HUD_ROW1, buf, gx_rgb(0xFFFFFF), gx_rgb(0x090B14));

    buf[0] = 'H'; buf[1] = 'I'; buf[2] = ' ';
    num_pad(buf + 3, g->hiscore % 1000000u, 6);
    gx_text_sh(b, 52, HUD_ROW1, buf, gx_rgb(0xFFE45E), gx_rgb(0x090B14));

    /* the spare ships, right-aligned against the pause button */
    for (int i = 0; i < g->lives - 1 && i < 5; i++) {
        gx_blit(b, HUD_RIGHT - 12 - i * 9, HUD_ROW1, gx_art_ship_icon.rows,
                gx_art_ship_icon.n);
    }

    /* energy bar: it drains by itself, it is the level's clock */
    int y = HUD_ROW2;
    gx_text_sh(b, 4, y + 1, "E", gx_rgb(0x8E8E93), gx_rgb(0x090B14));

    int bw = 136;
    int fill = g->energy * bw / G_ENERGY_MAX;
    uint16_t col = g->energy > G_ENERGY_MAX / 2 ? gx_rgb(0x30D158)
                 : g->energy > G_ENERGY_MAX / 5 ? gx_rgb(0xFFE45E)
                                                : gx_rgb(0xFF4A3D);
    gx_rect(b, 13, y, bw, 8, gx_rgb(0x161B2B));
    gx_rect(b, 13, y, fill, 8, col);
    gx_frame(b, 13, y, bw, 8, gx_rgb(0x606B85));
    if (g->energy < G_ENERGY_MAX / 5 && (g->state_t / 6) % 2) {
        gx_frame(b, 12, y - 1, bw + 2, 10, gx_rgb(0xFF4A3D));
    }

    /* weapon and planet */
    static const char weapon_letter[W_COUNT] = { 'N', 'D', 'T', 'L', 'O' };
    buf[0] = weapon_letter[g->weapon % W_COUNT];
    buf[1] = (char)('0' + (g->wlevel > 9 ? 9 : g->wlevel));
    buf[2] = '\0';
    gx_text_sh(b, 156, y + 1, buf, gx_rgb(0x7BE9FF), gx_rgb(0x090B14));

    buf[0] = 'P';
    buf[1] = (char)('0' + g->level + 1);
    buf[2] = '\0';
    gx_text_sh(b, HUD_RIGHT - 16, y + 1, buf, gx_rgb(0xB072F0), gx_rgb(0x090B14));

    /* the frames per second, bottom left, clear of the fire buttons */
    if (g->show_fps) {
        char *e = num_str(buf, (uint32_t)(g->fps10 / 10));
        *e++ = '.';
        *e++ = (char)('0' + g->fps10 % 10);
        *e++ = ' '; *e++ = 'F'; *e++ = 'P'; *e++ = 'S'; *e = '\0';
        gx_text_sh(b, 4, GX_H - 12, buf, gx_rgb(0x2AF0C8), gx_rgb(0x090B14));
    }

    /* the boss's bar */
    if (g->boss.alive) {
        int w = GX_W - 40;
        int f = g->boss.hp * w / (g->boss.hp_max > 0 ? g->boss.hp_max : 1);
        gx_rect(b, 20, G_HUD_H + 4, w, 5, gx_rgb(0x161B2B));
        gx_rect(b, 20, G_HUD_H + 4, f, 5, gx_rgb(0xFF4A3D));
        gx_frame(b, 20, G_HUD_H + 4, w, 5, gx_rgb(0xFF9F0A));
        /* the name only during the introduction: afterwards it covers the boss */
        if (g->boss.t < 130) {
            gx_text_center(b, GX_W / 2, G_HUD_H + 12, _(gx_bosses[g->boss.def].name),
                           gx_rgb(0xFF9F0A), gx_rgb(0x090B14));
        }
    }
}

static void banner(g_t *g, const char *big, const char *small, int y)
{
    gx_shade(&g->buf, 0, y - 6, GX_W, small ? 30 : 18, -9);
    gx_text_center(&g->buf, GX_W / 2, y, big, gx_rgb(0xFFFFFF), gx_rgb(0x090B14));
    if (small) {
        gx_text_center(&g->buf, GX_W / 2, y + 12, small, gx_rgb(0xFFE45E),
                       gx_rgb(0x090B14));
    }
}

/* --------------------------------------------------------------------------
 * State machine
 * -------------------------------------------------------------------------- */

static void level_start(g_t *g, int level)
{
    g->level = (uint8_t)clampi(level, 0, gx_level_count - 1);
    g->level_t = 0;
    g->wave_next = 0;
    g->state = ST_READY;
    g->state_t = 0;

    memset(g->enemies, 0, sizeof(g->enemies));
    memset(g->pshots, 0, sizeof(g->pshots));
    memset(g->eshots, 0, sizeof(g->eshots));
    memset(g->pickups, 0, sizeof(g->pickups));
    memset(g->fx, 0, sizeof(g->fx));
    memset(g->wave_left, 0, sizeof(g->wave_left));
    memset(g->wave_gift, 0, sizeof(g->wave_gift));
    memset(&g->boss, 0, sizeof(g->boss));

    gx_bg_reset(g);
    player_reset(g, false);
}

static void game_start(g_t *g)
{
    g->score = 0;
    g->lives = G_LIVES_START;
    g->weapon = W_SHOT;
    g->wlevel = 1;
    level_start(g, 0);
}

static bool enemies_left(const g_t *g)
{
    for (int i = 0; i < G_MAX_ENEMIES; i++) {
        if (g->enemies[i].alive) {
            return true;
        }
    }
    return false;
}

static void world_update(g_t *g)
{
    gx_bg_update(g);

    for (int i = 0; i < G_MAX_ENEMIES; i++) {
        g_enemy_t *e = &g->enemies[i];
        if (!e->alive) {
            continue;
        }
        gx_enemy_update(g, e);

        int x = UNFX(e->x), y = UNFX(e->y);
        if (y > G_PLAY_Y1 + 24 || x < -34 || x > GX_W + 34) {
            e->alive = 0;
            if (e->wave && g->wave_left[e->wave] > 0) {
                /* it got away: the formation no longer counts as wiped out */
                g->wave_left[e->wave]--;
                g->wave_gift[e->wave] = 0;
            }
        }
    }

    for (int i = 0; i < G_MAX_PICKUPS; i++) {
        g_pickup_t *p = &g->pickups[i];
        if (!p->alive) {
            continue;
        }
        p->t++;
        p->y = (int16_t)(p->y + p->vy);
        p->x = (int16_t)(p->x + gx_sin(p->t * 4) * 8 / 256);
        if (UNFX(p->y) > G_PLAY_Y1 + 10) {
            p->alive = 0;
        }
    }

    shots_update(g);
    fx_update(g);
}

static void world_draw(g_t *g)
{
    gx_bg_draw(g);

    for (int i = 0; i < G_MAX_PICKUPS; i++) {
        if (g->pickups[i].alive) {
            pickup_draw(g, &g->pickups[i]);
        }
    }
    gx_boss_draw(g);
    for (int i = 0; i < G_MAX_ENEMIES; i++) {
        if (g->enemies[i].alive) {
            gx_enemy_draw(g, &g->enemies[i]);
        }
    }
    player_draw(g);
    shots_draw(g);
    fx_draw(g);
}

/* declared further down: the LVGL panels and the flight state */
static bool state_is_flying(const g_t *g);
static void show_end(app_t *a, bool won);

static void step_state(app_t *a)
{
    g_t *g = &a->g;
    const g_level_t *lv = &gx_levels[g->level];

    g->state_t++;

    switch (g->state) {
    case ST_TITLE:
        /* the background stays alive behind the panel: the ship flies itself */
        gx_bg_update(g);
        shots_update(g);
        fx_update(g);
        g->px = (int16_t)(FX(GX_W / 2) + gx_sin(g->state_t * 2) * FX(60) / 256);
        g->py = FX(GX_H - 20);
        if (g->state_t % 14 == 0) {
            player_fire(g);     /* for show: the menu's ship fires by itself */
        }
        break;

    case ST_READY:
        world_update(g);
        player_move(g);
        if (g->state_t > 70) {
            g->state = ST_PLAY;
            g->state_t = 0;
        }
        break;

    case ST_PLAY:
        g->level_t++;
        waves_update(g);
        world_update(g);
        player_move(g);
        collisions(g);

        if (g->level_t >= lv->length && !enemies_left(g)) {
            gx_boss_start(g, lv->boss);
            g->state = ST_BOSS_IN;
            g->state_t = 0;
            aos_hal_beep(300, 200);
        }
        break;

    case ST_BOSS_IN:
        world_update(g);
        gx_boss_update(g);
        player_move(g);
        collisions(g);
        if (g->state_t > 60) {
            g->state = ST_BOSS;
            g->state_t = 0;
        }
        break;

    case ST_BOSS:
        world_update(g);
        gx_boss_update(g);
        player_move(g);
        collisions(g);
        if (!g->boss.alive) {
            g->state = ST_CLEAR;
            g->state_t = 0;
            g_add_score(g, 5000 + g->energy * 20);
        }
        break;

    case ST_CLEAR:
        world_update(g);
        player_move(g);
        g->py = (int16_t)(g->py - 6);       /* the ship leaves through the top */
        if (g->state_t > 110) {
            if (g->level + 1 < gx_level_count) {
                level_start(g, g->level + 1);
            } else {
                g->state = ST_WIN;
                g->state_t = 0;
                if (g->score >= g->hiscore) {
                    aos_hal_pref_set_i32(KEY_HI, (int32_t)g->hiscore);
                }
            }
        }
        break;

    case ST_DEAD:
        world_update(g);
        if (g->state_t == 40) {
            memset(g->eshots, 0, sizeof(g->eshots));    /* room to come back */
        }
        if (g->state_t > 70) {
            if (g->lives > 0) {
                player_reset(g, true);
                g->state = ST_PLAY;
                g->state_t = 0;
            } else {
                g->state = ST_GAMEOVER;
                g->state_t = 0;
                show_end(a, false);
            }
        }
        break;

    case ST_WIN:
        world_update(g);
        /* on the watch the banner stayed up for good and the only way out was
         * the pause; after five seconds of glory, the ending panel */
        if (g->state_t == 150) {
            show_end(a, true);
        }
        break;

    default:    /* ST_GAMEOVER, ST_PAUSE: the world stays frozen */
        break;
    }

    /* energy: 1943's hourglass */
    if (g->state == ST_PLAY || g->state == ST_BOSS || g->state == ST_BOSS_IN) {
        if ((g->state_t % DRAIN_EVERY) == 0) {
            g->energy--;
            if (g->energy <= 0) {
                g->energy = 0;
                player_die(g);
            }
        }
        if ((g->fire_down || g->autofire) && g->fire_cd <= 0) {
            player_fire(g);
        }
    }

    if (g->fire_cd > 0)  g->fire_cd--;
    if (g->invuln > 0)   g->invuln--;
    if (g->shield > 0)   g->shield--;
    if (g->roll > 0)     g->roll--;
    if (g->roll_cd > 0)  g->roll_cd--;
    if (g->shake > 0)    g->shake--;
    if (g->flash_screen > 0) g->flash_screen--;
}

static void draw_all(app_t *a)
{
    g_t *g = &a->g;

    world_draw(g);

    switch (g->state) {
    case ST_READY:
        banner(g, _(gx_levels[g->level].name), _(gx_levels[g->level].tag), GX_H / 2 - 16);
        break;
    case ST_BOSS_IN:
        if ((g->state_t / 6) % 2) {
            banner(g, _("ALERTA"), _(gx_bosses[gx_levels[g->level].boss].name),
                   GX_H / 2 - 30);
        }
        break;
    case ST_CLEAR:
        banner(g, _("PLANETA LIBERADO"), _(gx_levels[g->level].name), GX_H / 2 - 16);
        break;
    case ST_WIN:
        banner(g, _("SISTEMA LIBERADO"), _("GRACIAS POR JUGAR"), GX_H / 2 - 24);
        break;
    default:
        break;
    }

    if (g->state != ST_TITLE) {
        hud_draw(g);
    }

    /* the shake and the white flash go on top, in draw(): gx_shake_flash() */
}


/* --------------------------------------------------------------------------
 * LVGL panels
 *
 * The title, the pause and the game over are built with LVGL objects and not
 * with the 5x7 font: they are screens to read and touch, and there vector text
 * reads far better than pixelated. The game itself is all canvas. They live on
 * a stage exactly over the canvas. Each is laid out top to bottom with a
 * cursor and then made as tall as what it holds, so the same code fits the
 * portrait stage (720x1278) and the landscape one (480x720).
 * -------------------------------------------------------------------------- */

#define PANEL_PAD       24          /* inside the panels, left and right */
#define CHIP_H          52

static void controls_show(app_t *a, bool show);

static void overlay_hide_all(app_t *a)
{
    lv_obj_t *const panels[] = { a->title, a->pause, a->over };
    for (unsigned i = 0; i < sizeof(panels) / sizeof(panels[0]); i++) {
        if (panels[i]) {
            lv_obj_add_flag(panels[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* the pad has no buttons to go through (an empty set also takes the
     * outline off the one it was on) */
    aos_pad_menu_set(&a->menu, NULL, 0, 0);
    controls_show(a, true);
}

static void overlay_show(app_t *a, lv_obj_t *panel)
{
    overlay_hide_all(a);
    if (panel) {
        lv_obj_remove_flag(panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(panel);
        /* and the pad goes through its buttons, from the first one */
        if (panel == a->title) {
            aos_pad_menu_set(&a->menu, a->pm_title, 4, 0);
        } else if (panel == a->pause) {
            aos_pad_menu_set(&a->menu, a->pm_pause, 5, 0);
        } else if (panel == a->over) {
            aos_pad_menu_set(&a->menu, a->pm_over, 3, 0);
        }
    }
    /* over a menu the fire buttons have nothing to do: they go with it */
    controls_show(a, panel == NULL);
}

/* As wide as the stage allows, up to 'max'. */
static int panel_width(const app_t *a, int max)
{
    int w = a->r->w * a->r->scale - 40;
    return w < max ? w : max;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int w, uint32_t border)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, w, 100);     /* the height comes from panel_fit() */
    lv_obj_set_style_pad_left(p, PANEL_PAD, 0);
    lv_obj_set_style_pad_right(p, PANEL_PAD, 0);
    /* A dark plate behind the text: the game's background goes on running,
     * but over a field of stars and rocks the text cannot be read. */
    lv_obj_set_style_bg_color(p, lv_color_hex(0x05060F), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    lv_obj_set_style_radius(p, 36, 0);
    lv_obj_set_style_border_color(p, lv_color_hex(border), 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);  /* so touches do not pass through */
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

/* The cursor: the row under 'o', 'gap' further down. */
static int below(lv_obj_t *o, int gap)
{
    lv_obj_update_layout(o);
    return lv_obj_get_y(o) + lv_obj_get_height(o) + gap;
}

/* Closes a panel: as tall as the cursor got, centred on the stage. */
static void panel_fit(lv_obj_t *p, int h)
{
    lv_obj_set_height(p, h);
    lv_obj_center(p);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                            const lv_font_t *font, uint32_t color, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_y(l, y);
    return l;
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, const char *sub,
                             int x, int y, int w, int h, uint32_t accent,
                             lv_event_cb_t cb, void *data)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1C24), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    /* The press highlight goes by colour and not by transform_scale: scaling
     * forces LVGL to build a separate layer. */
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, sub ? &aos_montserrat_36 : &aos_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_align(l, sub ? LV_ALIGN_TOP_MID : LV_ALIGN_CENTER, 0, sub ? 16 : 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);

    if (sub) {
        lv_obj_t *t = lv_label_create(b);
        lv_label_set_text(t, sub);
        lv_obj_set_style_text_font(t, &aos_montserrat_20, 0);
        lv_obj_set_style_text_color(t, lv_color_hex(0x9AA3B8), 0);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(t, lv_pct(100));
        lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -16);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    }
    return b;
}

/* The chips are flat buttons whose text states the setting: "AUTO SI" /
 * "AUTO NO". */
static void chip_set(lv_obj_t *chip, const char *text, bool on)
{
    if (!chip) {
        return;
    }
    lv_obj_t *l = lv_obj_get_child(chip, 0);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(on ? 0x0A0A12 : 0x9AA3B8), 0);
    lv_obj_set_style_bg_color(chip, lv_color_hex(on ? 0x30D158 : 0x1C1C24), 0);
}

static lv_obj_t *make_chip(lv_obj_t *parent, int x, int y, int w,
                           lv_event_cb_t cb, void *data)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, CHIP_H);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x3A3A46), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, data);

    lv_obj_t *l = lv_label_create(c);
    lv_label_set_text(l, "");
    lv_obj_set_style_text_font(l, &aos_montserrat_24, 0);
    /* the text is clipped INSIDE the chip, width and height: a long
     * translation looks ugly but stays contained */
    lv_obj_set_size(l, w - 12, 32);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void chips_refresh(app_t *a)
{
    g_t *g = &a->g;
    const char *au = g->autofire ? _("AUTO SI") : _("AUTO NO");
    const char *sf = s_sfx ? _("SONIDO SI") : _("SONIDO NO");

    chip_set(a->chip_auto,  au, g->autofire);
    chip_set(a->chip_auto2, au, g->autofire);
    chip_set(a->chip_sfx,   sf, s_sfx);
    chip_set(a->chip_sfx2,  sf, s_sfx);
    chip_set(a->chip_fps2,  g->show_fps ? _("FPS SI") : _("FPS NO"), g->show_fps);
}

static void title_refresh(app_t *a)
{
    char buf[48];

    snprintf(buf, sizeof(buf), _("RECORD  %lu"), (unsigned long)a->g.hiscore);
    lv_label_set_text(a->title_hi, buf);
    chips_refresh(a);
}

static void prefs_save(app_t *a)
{
    g_t *g = &a->g;
    aos_hal_pref_set_i32(KEY_AUTO, g->autofire);
    aos_hal_pref_set_i32(KEY_SFX, s_sfx ? 1 : 0);
    aos_hal_pref_set_i32(KEY_FPS, g->show_fps);
}

static void start_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    prefs_save(a);
    overlay_hide_all(a);
    game_start(&a->g);
}

static void chip_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    lv_obj_t *chip = lv_event_get_target_obj(event);
    g_t *g = &a->g;

    if (chip == a->chip_auto || chip == a->chip_auto2) {
        g->autofire = !g->autofire;
    } else if (chip == a->chip_sfx || chip == a->chip_sfx2) {
        s_sfx = !s_sfx;
    } else if (chip == a->chip_fps2) {
        g->show_fps = !g->show_fps;
    }
    prefs_save(a);
    chips_refresh(a);
    aos_hal_beep(1200, 20);
}

static void pause_show(app_t *a)
{
    g_t *g = &a->g;

    if (g->state == ST_PAUSE || !state_is_flying(g)) {
        return;
    }
    a->paused_from = g->state;
    g->state = ST_PAUSE;
    g->fire_down = 0;
    g->touching = 0;
    chips_refresh(a);
    overlay_show(a, a->pause);
}

static void resume_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    g_t *g = &a->g;

    overlay_hide_all(a);
    /* Back to exactly where it was. The watch went to PLAY or BOSS, which from
     * the explosion (ST_DEAD, energy at zero) killed you a second time, and
     * from the planet's panel skipped it. */
    g->state = a->paused_from;
    g->fire_down = 0;
}

static void retry_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    overlay_hide_all(a);
    game_start(&a->g);
}

static void menu_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->g.state = ST_TITLE;
    a->g.state_t = 0;
    a->g.level = 2;             /* the title's background is the debris field */
    gx_bg_reset(&a->g);
    title_refresh(a);
    overlay_show(a, a->title);
}

static void exit_cb(lv_event_t *event)
{
    app_t *a = (app_t *)lv_event_get_user_data(event);
    a->want_exit = true;        /* it exits on the next frame, not here */
}

static void build_title(app_t *a, lv_obj_t *root)
{
    int pw = panel_width(a, 600), cw = pw - 2 * PANEL_PAD;
    lv_obj_t *p = make_panel(root, pw, 0x2A3145);
    a->title = p;

    lv_obj_t *l = make_label(p, "2043", &aos_montserrat_64, 0xFF4A3D, 20);
    int y = below(l, 0);
    l = make_label(p, _("LA BATALLA DE CERES"), &aos_montserrat_28, 0xFFE45E, y);
    y = below(l, 24);

    lv_obj_t *b = make_button(p, _("JUGAR"), _("arrastrá el dedo para volar"),
                              0, y, cw, 112, 0x0A84FF, start_cb, a);
    a->pm_title[0] = b;
    y = below(b, 22);

    /* What is really on the screen: the drag, the two buttons, the pause. */
    static const char *const hints[] = {
        N_("ARRASTRÁ EL DEDO Y LA NAVE LO SIGUE"),
        N_("DISPARAR: MANTENELO APRETADO"),
        N_("TONEL: UN GIRO INVULNERABLE"),
        N_("PAUSA: ARRIBA A LA DERECHA"),
    };
    for (unsigned i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        l = make_label(p, _(hints[i]), &aos_montserrat_20,
                       i < 3 ? 0x7BE9FF : 0x6A6A78, y);
        y = below(l, 6);
    }
    y += 16;

    int chw = (cw - 16) / 2;
    a->chip_auto = make_chip(p, 0, y, chw, chip_cb, a);
    a->chip_sfx  = make_chip(p, chw + 16, y, chw, chip_cb, a);
    y += CHIP_H + 20;

    a->title_hi = make_label(p, _("RECORD 0"), &aos_montserrat_28, 0xFFFFFF, y);
    y = below(a->title_hi, 16);
    b = make_button(p, _("SALIR"), NULL, (cw - 200) / 2, y, 200, 64, 0xFF453A, exit_cb, a);
    a->pm_title[1] = a->chip_auto;
    a->pm_title[2] = a->chip_sfx;
    a->pm_title[3] = b;
    panel_fit(p, below(b, 24));
}

static void build_pause(app_t *a, lv_obj_t *root)
{
    int pw = panel_width(a, 440), cw = pw - 2 * PANEL_PAD;
    lv_obj_t *p = make_panel(root, pw, 0x3A3A46);
    a->pause = p;
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);

    lv_obj_t *l = make_label(p, _("PAUSA"), &aos_montserrat_36, 0xFFFFFF, 24);
    int y = below(l, 24);
    lv_obj_t *b = make_button(p, _("SEGUIR"), NULL, 0, y, cw, 76, 0x30D158, resume_cb, a);
    a->pm_pause[0] = b;
    y = below(b, 20);
    a->chip_auto2 = make_chip(p, 0, y, cw, chip_cb, a);
    y += CHIP_H + 12;
    a->chip_sfx2  = make_chip(p, 0, y, cw, chip_cb, a);
    y += CHIP_H + 12;
    a->chip_fps2  = make_chip(p, 0, y, cw, chip_cb, a);
    y += CHIP_H + 20;
    b = make_button(p, _("SALIR"), NULL, 0, y, cw, 76, 0xFF453A, exit_cb, a);
    a->pm_pause[1] = a->chip_auto2;
    a->pm_pause[2] = a->chip_sfx2;
    a->pm_pause[3] = a->chip_fps2;
    a->pm_pause[4] = b;
    panel_fit(p, below(b, 24));
}

static void build_over(app_t *a, lv_obj_t *root)
{
    int pw = panel_width(a, 460), cw = pw - 2 * PANEL_PAD;
    lv_obj_t *p = make_panel(root, pw, 0xFF453A);
    a->over = p;
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);

    a->over_title = make_label(p, _("FIN DEL JUEGO"), &aos_montserrat_36, 0xFF4A3D, 26);
    int y = below(a->over_title, 16);
    /* two lines, like the text show_end() puts there */
    a->over_score = make_label(p, "0\n0", &aos_montserrat_28, 0xFFFFFF, y);
    y = below(a->over_score, 28);

    lv_obj_t *b = make_button(p, _("OTRA VEZ"), NULL, 0, y, cw, 76, 0x30D158, retry_cb, a);
    a->pm_over[0] = b;
    y = below(b, 14);
    int hw = (cw - 14) / 2;
    a->pm_over[1] = make_button(p, _("MENU"), NULL, 0, y, hw, 76, 0x0A84FF, menu_cb, a);
    b = make_button(p, _("SALIR"), NULL, hw + 14, y, hw, 76, 0xFF453A, exit_cb, a);
    a->pm_over[2] = b;
    panel_fit(p, below(b, 26));
}

/* Both endings use the same panel: losing the last ship, and freeing the
 * last planet. The world stays frozen behind it. */
static void show_end(app_t *a, bool won)
{
    g_t *g = &a->g;
    char buf[64];

    g->state = ST_GAMEOVER;
    g->state_t = 0;
    g->fire_down = 0;
    if (g->score >= g->hiscore) {
        aos_hal_pref_set_i32(KEY_HI, (int32_t)g->hiscore);
    }
    lv_label_set_text(a->over_title, won ? _("SISTEMA LIBERADO") : _("FIN DEL JUEGO"));
    lv_obj_set_style_text_color(a->over_title, lv_color_hex(won ? 0x30D158 : 0xFF4A3D), 0);
    snprintf(buf, sizeof(buf), _("PUNTOS  %lu\nRECORD  %lu"),
             (unsigned long)g->score, (unsigned long)g->hiscore);
    lv_label_set_text(a->over_score, buf);
    overlay_show(a, a->over);
}

/* --------------------------------------------------------------------------
 * Controls
 *
 * The screen is the only control: no buttons, no IMU. So:
 *
 *  - A finger dragged anywhere that is not a button flies the ship. The ship
 *    moves WITH the finger, 3/2 of what it moves, from wherever it was when
 *    the finger came down: the finger never covers the ship, and a thumb
 *    resting low on the screen reaches the whole field.
 *  - DISPARAR fires while held and TONEL does the barrel roll, for the other
 *    thumb: bottom right, floating over the field in portrait, in the column
 *    right of the field in landscape. The pause is a button in the top right
 *    corner.
 *
 * They are the app's own and not the OS's pad (AOS_RETRO_A / B / TOUCH): in
 * portrait they float over the canvas, and there the service reports as the
 * canvas finger whichever finger touched first, so a thumb held on DISPARAR
 * would take the drag's place. The app reads both fingers itself from the
 * HAL's samples, every one since the last step (a tap shorter than a frame
 * still counts), and tells them apart: a finger on a button is that button,
 * any other is the drag.
 * -------------------------------------------------------------------------- */

static void ctl_make(app_t *a, int id, int cx, int cy, int d, uint32_t accent,
                     const char *text)
{
    lv_obj_t *b = lv_obj_create(a->ctl);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    /* translucent, so the field shows through where they float over it */
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1C1C24), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(b, LV_OPA_70, 0);
    lv_obj_set_style_border_width(b, 3, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    /* not LVGL's to click: they are read from the touch samples */
    lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);

    if (text) {
        lv_obj_t *l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, d > 150 ? &aos_montserrat_24 : &aos_montserrat_20, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(l);
    } else {
        /* the pause sign, two bars */
        for (int i = -1; i <= 1; i += 2) {
            lv_obj_t *bar = lv_obj_create(b);
            lv_obj_remove_style_all(bar);
            lv_obj_set_size(bar, d / 8, d * 3 / 8);
            lv_obj_set_style_bg_color(bar, lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar, 2, 0);
            lv_obj_align(bar, LV_ALIGN_CENTER, i * d / 7, 0);
        }
    }

    a->btn[id].obj = b;
    a->btn[id].cx = (int16_t)cx;
    a->btn[id].cy = (int16_t)cy;
    a->btn[id].r = (int16_t)(d / 2 + 24);   /* forgiving, like the OS's pads */
}

static void build_controls(app_t *a, lv_obj_t *root)
{
    const aos_retro_t *r = a->r;
    lv_obj_update_layout(root);
    int W = lv_obj_get_width(root), H = lv_obj_get_height(root);

    a->ctl = lv_obj_create(root);
    lv_obj_remove_style_all(a->ctl);
    lv_obj_set_size(a->ctl, W, H);
    lv_obj_remove_flag(a->ctl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->ctl, LV_OBJ_FLAG_SCROLLABLE);

    if (!a->land) {
        /* Portrait: the right thumb's corner, clear of the home swipe at the
         * bottom edge; the pause over the HUD's clear right end. */
        int fx = W - 118, fy = H - 190;
        ctl_make(a, CB_FIRE,  fx,      fy,       176, 0xFF4A3D, _("DISPARAR"));
        ctl_make(a, CB_ROLL,  fx + 22, fy - 186, 124, 0x7BE9FF, _("TONEL"));
        ctl_make(a, CB_PAUSE, W - 46,  r->y + 38, 68, 0x9AA3B8, NULL);
    } else {
        /* Landscape: the column right of the field, and a word in the left
         * one, which is where the other thumb drags without covering it. */
        int gx2 = r->x + r->w * r->scale;
        int fx = gx2 + (W - gx2) / 2 + 40, fy = H - 180;
        ctl_make(a, CB_FIRE,  fx,       fy,       184, 0xFF4A3D, _("DISPARAR"));
        ctl_make(a, CB_ROLL,  fx - 150, fy - 150, 132, 0x7BE9FF, _("TONEL"));
        ctl_make(a, CB_PAUSE, W - 64,   64,       80,  0x9AA3B8, NULL);

        lv_obj_t *l = lv_label_create(a->ctl);
        lv_label_set_text(l, _("ARRASTRÁ EL DEDO Y LA NAVE LO SIGUE"));
        lv_obj_set_style_text_font(l, &aos_montserrat_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x6A6A78), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(l, r->x - 60);
        lv_obj_set_pos(l, 30, H / 2 - 40);
    }
}

/* Which buttons are under a point (root coordinates), as 1 << CB_*. */
static uint8_t ctl_hit(const app_t *a, int x, int y)
{
    uint8_t bits = 0;
    for (int i = 0; i < CB_COUNT; i++) {
        const ctl_btn_t *c = &a->btn[i];
        if (!c->obj) {
            continue;
        }
        int dx = x - c->cx, dy = y - c->cy;
        if (dx * dx + dy * dy <= c->r * c->r) {
            bits |= (uint8_t)(1u << i);
        }
    }
    return bits;
}

/* What the buttons show: pressed while held, and the roll dimmed while it
 * recharges, so its cooldown can be seen. */
static void ctl_refresh(app_t *a)
{
    uint8_t held = a->btn_held | a->pad_btn;
    for (int i = 0; i < CB_COUNT; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!a->btn[i].obj || ((held ^ a->btn_shown) & bit) == 0) {
            continue;
        }
        if (held & bit) lv_obj_add_state(a->btn[i].obj, LV_STATE_PRESSED);
        else lv_obj_remove_state(a->btn[i].obj, LV_STATE_PRESSED);
    }
    a->btn_shown = held;

    /* With AUTO the ship fires with no finger on the button, so the button
     * says so, in amber, instead of looking pressed or stuck: it only lights
     * red while a finger is on it, AUTO or not. */
    uint8_t au = a->g.autofire ? 1 : 0;
    lv_obj_t *fire = a->btn[CB_FIRE].obj;
    if (fire && au != a->auto_shown) {
        a->auto_shown = au;
        uint32_t col = au ? 0xFF9F0A : 0xFF4A3D;
        lv_obj_set_style_border_color(fire, lv_color_hex(col), 0);
        lv_obj_set_style_bg_color(fire, lv_color_hex(au ? 0x3A2A10 : 0x1C1C24), 0);
        lv_obj_t *l = lv_obj_get_child(fire, 0);
        if (l) {
            lv_label_set_text(l, au ? _("DISPARO\nAUTO") : _("DISPARAR"));
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_color(l, lv_color_hex(au ? 0xFFD08A : 0xFFFFFF), 0);
        }
    }

    uint8_t dim = a->g.roll_cd > 0 ? 1 : 0;
    if (dim != a->roll_dim && a->btn[CB_ROLL].obj) {
        a->roll_dim = dim;
        lv_obj_set_style_opa(a->btn[CB_ROLL].obj, dim ? LV_OPA_40 : LV_OPA_COVER, 0);
    }
}

static void controls_show(app_t *a, bool show)
{
    if (!a->ctl) {
        return;
    }
    if (show) {
        lv_obj_remove_flag(a->ctl, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(a->ctl, LV_OBJ_FLAG_HIDDEN);
    }
    /* what was held under the panel does not carry over */
    a->btn_held = a->btn_down = 0;
    a->fly_on = false;
    a->g.touching = 0;
    ctl_refresh(a);
}

/* The drag: where the ship should go, in canvas 1/16 px, for the flying
 * finger at (sx, sy) on the root. At the field's edge the anchor moves with
 * it, so turning back answers at once instead of after the finger has
 * undone the overshoot. */
static void fly_target(app_t *a, int sx, int sy)
{
    const aos_retro_t *r = a->r;
    g_t *g = &a->g;
    int32_t fx = (int32_t)(sx - r->x) * FX_ONE / r->scale;
    int32_t fy = (int32_t)(sy - r->y) * FX_ONE / r->scale;
    int32_t dx = (fx - a->fly_fx) * DRAG_GAIN_NUM / DRAG_GAIN_DEN;
    int32_t dy = (fy - a->fly_fy) * DRAG_GAIN_NUM / DRAG_GAIN_DEN;
    int32_t x0 = FX(9), x1 = FX(GX_W - 9);
    int32_t y0 = FX(G_PLAY_Y0 + 9), y1 = FX(G_PLAY_Y1 - 7);
    int32_t tx = a->fly_px + dx, ty = a->fly_py + dy;

    if (tx < x0) { tx = x0; a->fly_px = x0 - dx; }
    if (tx > x1) { tx = x1; a->fly_px = x1 - dx; }
    if (ty < y0) { ty = y0; a->fly_py = y0 - dy; }
    if (ty > y1) { ty = y1; a->fly_py = y1 - dy; }
    g->touch_x = (int16_t)tx;
    g->touch_y = (int16_t)ty;
}

/* One touch sample: the buttons under the fingers, and the flying finger. */
static void touch_sample(app_t *a, const aos_touch_frame_t *f, int ox, int oy)
{
    uint8_t held = 0;
    int cx[2], cy[2], n = 0;

    for (int i = 0; i < f->count && i < 2; i++) {
        int x = f->x[i] - ox, y = f->y[i] - oy;
        uint8_t bits = ctl_hit(a, x, y);
        if (bits) {
            held |= bits;
            continue;
        }
        cx[n] = x;
        cy[n] = y;
        n++;
    }
    a->btn_down |= (uint8_t)(held & ~a->btn_held);
    a->btn_held = held;

    /* The same finger as before is the nearest one: the panel may swap the
     * two slots when the first finger lifts. None near: it lifted. */
    int pick = -1;
    if (a->fly_on) {
        int best = 160 * 160;
        for (int i = 0; i < n; i++) {
            int dx = cx[i] - a->fly_sx, dy = cy[i] - a->fly_sy;
            if (dx * dx + dy * dy < best) {
                best = dx * dx + dy * dy;
                pick = i;
            }
        }
        a->fly_on = pick >= 0;
    }
    if (!a->fly_on && n > 0) {
        /* a new drag: it starts from where the ship is now */
        const aos_retro_t *r = a->r;
        pick = 0;
        a->fly_on = true;
        a->fly_fx = (int32_t)(cx[0] - r->x) * FX_ONE / r->scale;
        a->fly_fy = (int32_t)(cy[0] - r->y) * FX_ONE / r->scale;
        a->fly_px = a->g.px;
        a->fly_py = a->g.py;
    }
    if (pick >= 0) {
        a->fly_sx = (int16_t)cx[pick];
        a->fly_sy = (int16_t)cy[pick];
        fly_target(a, cx[pick], cy[pick]);
    }
}

/* --------------------------------------------------------------------------
 * The frame: input and one step of the simulation (30 a second), then the draw
 * -------------------------------------------------------------------------- */

static bool state_is_flying(const g_t *g)
{
    return g->state == ST_READY || g->state == ST_PLAY || g->state == ST_BOSS_IN ||
           g->state == ST_BOSS || g->state == ST_CLEAR || g->state == ST_DEAD ||
           g->state == ST_WIN;
}

/* The barrel roll: invulnerable for a moment, then it has to recharge. */
static void roll_start(g_t *g)
{
    if (g->roll_cd <= 0 && g->roll <= 0) {
        g->roll = ROLL_FRAMES;
        g->roll_cd = ROLL_COOLDOWN;
        aos_hal_beep(900, 60);
    }
}

static void read_input(app_t *a)
{
    g_t *g = &a->g;
    aos_touch_frame_t fr[16];
    uint32_t n = aos_hal_touch_frames(a->touch_seq, fr, 16);
    /* Read whatever the state, so old samples never pile up; act on them
     * only while flying and in front (in the background, or under the app
     * switcher, the touches are another app's). */
    bool live = state_is_flying(g) && g->state != ST_PAUSE &&
                lv_obj_is_visible(a->r->view);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);    /* where the root is NOW: the runtime slides it */

    /* Coming back from a menu, the pause or the background, the state
     * starts from the fingers as they are now: a sample only arrives when
     * something changes, and a thumb resting still would not be seen. */
    if (live && !a->live && n == 0) {
        aos_touch_frame_t now;
        if (aos_hal_touch_frame(&now)) {
            a->touch_seq = now.seq;
            touch_sample(a, &now, rc.x1, rc.y1);
        }
    }
    a->live = live;
    /* Every sample in order, the last one included: the HAL publishes one
     * with no fingers when the last one lifts, so "held" always ends where
     * the glass is, even with a tap shorter than a step. */
    for (uint32_t i = 0; i < n; i++) {
        a->touch_seq = fr[i].seq;
        if (live) {
            touch_sample(a, &fr[i], rc.x1, rc.y1);
        }
    }
    if (!live) {
        a->btn_held = a->btn_down = 0;
        a->fly_on = false;
        g->touching = 0;
        ctl_refresh(a);
        return;
    }

    uint8_t down = a->btn_down;
    a->btn_down = 0;
    if (down & (1u << CB_PAUSE)) {
        aos_hal_beep(700, 30);
        pause_show(a);
        return;
    }
    if (down & (1u << CB_ROLL)) {
        roll_start(g);
    }
    /* a press shorter than a step still fires once */
    g->fire_down = (uint8_t)(((a->btn_held | down) & (1u << CB_FIRE)) ? 1 : 0);
    g->touching = a->fly_on ? 1 : 0;
    ctl_refresh(a);
}

/* --------------------------------------------------------------------------
 * The gamepad
 *
 * A USB pad on the board's host (aos_pad.h) plays alongside the fingers:
 * the left stick flies the ship at a speed that follows how far it leans,
 * the d-pad at full speed; A fires while held, B (or R) does the barrel
 * roll, START pauses. The on-screen buttons light up with the pad's, so the
 * cooldown and AUTO read the same. Over a panel the d-pad goes through its
 * buttons and A presses them (aos_pad_menu.h); START on the title plays, on
 * the pause it resumes and on the end panel it retries, and B is back: out
 * of the pause, or from the end panel to the menu.
 * -------------------------------------------------------------------------- */

#define PAD_DEAD        6000        /* the stick's dead zone, of 32767 */

static void pad_click(lv_obj_t *b)
{
    if (b && lv_obj_is_valid(b)) {
        lv_obj_send_event(b, LV_EVENT_CLICKED, NULL);
    }
}

static void pad_step(app_t *a)
{
    g_t *g = &a->g;
    aos_pad_t *p = &a->pad;
    uint8_t btn = 0;

    aos_pad_update(p, lv_tick_get());
    /* in the background, or under the app switcher, the pad is not ours */
    if (!p->connected || !lv_obj_is_visible(a->r->view)) {
        p->pressed = p->repeat = 0;
    } else if (a->menu.n) {
        /* a panel is up */
        if (!aos_pad_menu_step(&a->menu, p)) {
            bool start = aos_pad_pressed(p, AOS_PAD_START);
            bool back = aos_pad_pressed(p, AOS_PAD_B);
            if (a->menu.item[0] == a->pm_title[0] && start) {
                pad_click(a->pm_title[0]);
            } else if (a->menu.item[0] == a->pm_pause[0] && (start || back)) {
                pad_click(a->pm_pause[0]);
            } else if (a->menu.item[0] == a->pm_over[0]) {
                if (start) pad_click(a->pm_over[0]);
                else if (back) pad_click(a->pm_over[1]);
            }
        }
    } else if (state_is_flying(g) && g->state != ST_PAUSE) {
        if (aos_pad_pressed(p, AOS_PAD_START)) {
            aos_hal_beep(700, 30);
            pause_show(a);
            return;
        }
        /* the stick past its dead zone, or else the d-pad at full tilt */
        int sx = p->x, sy = p->y;
        if (sx > -PAD_DEAD && sx < PAD_DEAD) {
            sx = (aos_pad_held(p, AOS_PAD_RIGHT) - aos_pad_held(p, AOS_PAD_LEFT)) * 32767;
        }
        if (sy > -PAD_DEAD && sy < PAD_DEAD) {
            sy = (aos_pad_held(p, AOS_PAD_DOWN) - aos_pad_held(p, AOS_PAD_UP)) * 32767;
        }
        /* a finger flying the ship has the last word */
        if ((sx || sy) && !a->fly_on) {
            /* player_move() heads at half the distance to the target, capped
             * at PLAYER_SPEED: a target twice that far is top speed */
            g->touching = 1;
            g->touch_x = (int16_t)(g->px + sx * (2 * PLAYER_SPEED) / 32767);
            g->touch_y = (int16_t)(g->py + sy * (2 * PLAYER_SPEED) / 32767);
        }
        if (aos_pad_held(p, AOS_PAD_A)) {
            g->fire_down = 1;
            btn |= 1u << CB_FIRE;
        }
        if (aos_pad_held(p, AOS_PAD_B | AOS_PAD_R)) {
            btn |= 1u << CB_ROLL;
        }
        if (aos_pad_pressed(p, AOS_PAD_B | AOS_PAD_R)) {
            roll_start(g);
        }
    }
    if (btn != a->pad_btn) {
        a->pad_btn = btn;
        ctl_refresh(a);
    }
}

static void step(void *user)
{
    app_t *a = (app_t *)user;

    if (a->want_exit) {
        /* aos_ui_back() destroys the app: after this call 'a' no longer
         * exists, so nothing else is touched (the service sees it too) */
        a->want_exit = false;
        aos_ui_back();
        return;
    }

    read_input(a);
    pad_step(a);
    if (a->g.state != ST_PAUSE) {
        step_state(a);
    }
}

/* Adaptive quality, as on the watch: the expensive decorations (the nebulae,
 * the suspended dust, the halo on each bullet) are only on while the game
 * draws at the rate it wants. On the watch the measure was the timer's own
 * period; here it is the service's draws per second. */
static void detail_tune(app_t *a, const aos_retro_stats_t *st)
{
    if (++a->detail_t < 30) {
        return;
    }
    a->detail_t = 0;
    uint8_t detail = a->g.detail;
    if (st->fps10 && st->fps10 < 270) {
        detail = 0;
    } else if (st->fps10 >= 290) {
        detail = 1;
    }
    if (detail != a->g.detail) {
        a->g.detail = detail;
        aos_hal_log("2043", "scenery %s (%u.%u fps)", detail ? "full" : "light",
                    st->fps10 / 10, st->fps10 % 10);
    }
}

/* --------------------------------------------------------------------------
 * Presenting only what changed
 *
 * The game redraws the whole field every frame, but not all of it changes:
 * the sky's gradient does not move, and what does (the scenery, the stars,
 * the ships and the bullets) covers a part of it. On the board every
 * presented pixel is scaled x3 and flushed to the panel, and presenting the
 * whole canvas each frame is what held 2043 at 26 fps. So the frame is
 * compared with what the screen shows (a->shown) in tiles of 8x8 art
 * pixels, and only what holds changed tiles is presented.
 *
 * The changes are scattered over the whole field, a few tiles in every
 * row, so they are gathered by strips: STRIP_ROWS rows of tiles at a time,
 * at most STRIP_RUNS rectangles each, cut to the rows that changed. That
 * is at most 28 rectangles a frame, within LVGL's 32, found in one linear
 * pass. (Joining the cheapest pair again and again presented ~8 % less and
 * cost as much as drawing the frame; measured offline on 600 frames of
 * each planet, 2026-09-30.)
 *
 * It is exact, whatever drew what: after every present a->shown equals the
 * canvas, and anything LVGL redraws on its own (a panel closing, a button
 * lighting) it draws from the canvas.
 *
 * A planet where most tiles change every frame (the ocean's swell) gains
 * nothing: while comparing keeps presenting it all, it rests and the whole
 * canvas goes, as before (PROBE_EVERY). The preference "g2043_diff" = 0
 * presents everything every frame, to compare on the board.
 * -------------------------------------------------------------------------- */

#define TILE            8           /* art pixels a side                        */
#define TILES_X         (GX_W / TILE)
#define STRIP_ROWS      4           /* rows of tiles in a strip                 */
#define STRIP_RUNS      2           /* rectangles a strip at most               */
/* Unchanged tiles between two changed ones in a row that are presented
 * rather than cutting the run: each rectangle is one more pass of LVGL's
 * refresh (layers, the scaler, the flush). */
#define GAP_BRIDGE      3
/* Past this share of the canvas, all of it: one present instead of two
 * dozen. */
#define PRESENT_ALL_PCT 70
/* While what is presented stays above that, compare only one frame in this
 * many. */
#define PROBE_EVERY     30

static void present_all(app_t *a)
{
    if (a->shown) {
        memcpy(a->shown, a->r->px, (size_t)GX_W * GX_H * sizeof(uint16_t));
        a->shown_ok = true;
    }
    aos_retro_present();
}

/* Compares, presents what changed, and returns the canvas pixels presented. */
static uint32_t present_changed(app_t *a)
{
    const uint16_t *px = a->r->px;
    uint16_t *sh = a->shown;
    const int rows = (GX_H + TILE - 1) / TILE;
    trect_t out[((GX_H_PORTRAIT + TILE - 1) / TILE + STRIP_ROWS - 1) / STRIP_ROWS * STRIP_RUNS];
    int n = 0;
    uint32_t changed = 0;
    uint32_t masks[STRIP_ROWS];

    for (int sy = 0; sy < rows; sy += STRIP_ROWS) {
        int srows = rows - sy < STRIP_ROWS ? rows - sy : STRIP_ROWS;
        uint32_t any = 0;
        for (int k = 0; k < srows; k++) {
            int ty = sy + k;
            int y0 = ty * TILE, y1 = y0 + TILE > GX_H ? GX_H : y0 + TILE;
            /* Two pixels at a time (rows are 480 bytes, the canvas
             * aligned), every difference OR-ed into its tile's word: no
             * branch in the loop, one pass over the canvas and a->shown. */
            uint32_t acc[TILES_X] = { 0 };
            for (int y = y0; y < y1; y++) {
                const uint32_t *p = (const uint32_t *)(px + (size_t)y * GX_W);
                const uint32_t *q = (const uint32_t *)(sh + (size_t)y * GX_W);
                for (int tx = 0; tx < TILES_X; tx++, p += TILE / 2, q += TILE / 2) {
                    acc[tx] |= (p[0] ^ q[0]) | (p[1] ^ q[1]) | (p[2] ^ q[2]) | (p[3] ^ q[3]);
                }
            }
            uint32_t mask = 0;  /* bit tx: the tile changed (TILES_X <= 32) */
            for (int tx = 0; tx < TILES_X; tx++) {
                if (acc[tx]) {
                    mask |= 1u << tx;
                    changed++;
                    /* into what the screen will show: the changed tiles
                     * only, the others are the same already */
                    for (int y = y0; y < y1; y++) {
                        memcpy(sh + (size_t)y * GX_W + tx * TILE, px + (size_t)y * GX_W + tx * TILE,
                               TILE * sizeof(uint16_t));
                    }
                }
            }
#ifdef AOS_SIM_BUILTIN
            /* G2043_DUMP=file (simulator): the masks of 600 frames, to try
             * other ways of gathering them offline */
            {
                static FILE *dump;
                static int dump_t = -1;
                if (dump_t < 0) {
                    const char *d = getenv("G2043_DUMP");
                    dump = d ? fopen(d, "wb") : NULL;
                    dump_t = 0;
                }
                if (dump && dump_t < 600 * rows) {
                    fwrite(&mask, 4, 1, dump);
                    if (++dump_t == 600 * rows) fclose(dump);
                }
            }
#endif
            masks[k] = mask;
            any |= mask;
        }
        if (!any) {
            continue;
        }

        /* the strip's runs of changed columns, short gaps bridged */
        int16_t rx0[TILES_X], rx1[TILES_X];
        int nr = 0;
        for (int tx = 0; tx < TILES_X; tx++) {
            if (!(any & (1u << tx))) {
                continue;
            }
            if (nr && tx - rx1[nr - 1] - 1 <= GAP_BRIDGE) {
                rx1[nr - 1] = (int16_t)tx;
            } else {
                rx0[nr] = rx1[nr] = (int16_t)tx;
                nr++;
            }
        }
        /* too many: join across the narrowest gap until they fit */
        while (nr > STRIP_RUNS) {
            int best = 0;
            for (int i = 1; i < nr - 1; i++) {
                if (rx0[i + 1] - rx1[i] < rx0[best + 1] - rx1[best]) {
                    best = i;
                }
            }
            rx1[best] = rx1[best + 1];
            for (int i = best + 1; i < nr - 1; i++) {
                rx0[i] = rx0[i + 1];
                rx1[i] = rx1[i + 1];
            }
            nr--;
        }
        /* each cut to the rows of tiles that changed under it */
        for (int i = 0; i < nr; i++) {
            uint32_t cols = (rx1[i] - rx0[i] + 1 >= 32) ? 0xFFFFFFFFu
                          : (((1u << (rx1[i] - rx0[i] + 1)) - 1u) << rx0[i]);
            int k0 = -1, k1 = -1;
            for (int k = 0; k < srows; k++) {
                if (masks[k] & cols) {
                    if (k0 < 0) k0 = k;
                    k1 = k;
                }
            }
            out[n++] = (trect_t){ rx0[i], (int16_t)(sy + k0), rx1[i], (int16_t)(sy + k1) };
        }
    }

    if (changed == 0) {
        return 0;
    }
    a->prof_tiles += changed;
    uint32_t area = 0;
    for (int i = 0; i < n; i++) {
        area += (uint32_t)((out[i].x1 - out[i].x0 + 1) * (out[i].y1 - out[i].y0 + 1));
    }
    if (area * 100u >= (uint32_t)(TILES_X * rows) * PRESENT_ALL_PCT) {
        aos_retro_present();    /* a->shown already has all that changed */
        return (uint32_t)GX_W * GX_H;
    }
    uint32_t npx = 0;
    for (int i = 0; i < n; i++) {
        int x = out[i].x0 * TILE, y = out[i].y0 * TILE;
        int w = (out[i].x1 - out[i].x0 + 1) * TILE;
        int h = (out[i].y1 - out[i].y0 + 1) * TILE;
        if (y + h > GX_H) {
            h = GX_H - y;
        }
        aos_retro_present_rect(x, y, w, h);
        npx += (uint32_t)(w * h);
    }
    a->prof_rects += (uint32_t)n;
    return npx;
}

static void draw(void *user)
{
    app_t *a = (app_t *)user;
    g_t *g = &a->g;
    aos_retro_stats_t st;

    aos_retro_stats(&st);
    g->fps10 = (int16_t)st.fps10;
    detail_tune(a, &st);

    if (g->state == ST_PAUSE) {
        return;                 /* the canvas keeps the frame the pause froze */
    }

    uint64_t t0 = aos_hal_uptime_us();
    draw_all(a);

    /* The screen shake: one art pixel either way, as on the watch (there it
     * was the upscaler reading from a shifted origin). The white flash after
     * the big explosions, likewise. */
    int sx = 0, sy = 0;
    if (g->shake > 0) {
        sx = (int)(g_rnd(g) % 3) - 1;
        sy = (int)(g_rnd(g) % 3) - 1;
    }
    int flash = g->flash_screen > 0 ? g->flash_screen * 3 : 0;
    if (flash > 16) {
        flash = 16;
    }
    if (sx || sy || flash) {
        gx_shake_flash(&g->buf, sx, sy, flash);
    }
    uint64_t t1 = aos_hal_uptime_us();

    /* What goes to the screen: only the tiles that changed. A shaken or
     * flashed frame moves everything, so all of it, without comparing; and
     * while comparing has been presenting nearly all of it anyway, it
     * rests (see PROBE_EVERY), keeping a->shown only for the frame before
     * it tries again. */
    const uint32_t all = (uint32_t)GX_W * GX_H;
    uint32_t npx = all;
    bool compared = false;
    if (!a->diff_on || !a->shown) {
        aos_retro_present();
    } else if (sx || sy || flash || !a->shown_ok) {
        present_all(a);
    } else if (a->probe_t > 1) {
        a->probe_t--;
        a->shown_ok = false;
        aos_retro_present();
    } else if (a->probe_t == 1) {
        a->probe_t = 0;
        present_all(a);         /* the next frame compares against this one */
    } else {
        npx = present_changed(a);
        compared = true;
        if (npx * 100u >= all * PRESENT_ALL_PCT) {
            if (++a->full_run >= 3) {
                a->probe_t = PROBE_EVERY;
            }
        } else {
            a->full_run = 0;
        }
    }
    /* The profile: what drawing and comparing cost and how much of the
     * canvas went to the screen, every 10 s on the board (every 2 s with
     * G2043_PROF=1 in the simulator), next to the service's own numbers. */
    a->prof_draw_us += t1 - t0;
    if (compared) {
        a->prof_diff_us += aos_hal_uptime_us() - t1;
        a->prof_cmp++;
    }
    a->prof_px += npx;
    if (++a->prof_n >= a->prof_every) {
        uint32_t k = a->prof_n;
        uint32_t rows = (uint32_t)((GX_H + TILE - 1) / TILE);
        aos_hal_log("2043", "%u.%u fps | draw %u us, compare %u us (%u of %u frames), "
                    "changed %u%%, presented %u%% in %u rects | refresh %u us, scale %u us",
                    st.fps10 / 10, st.fps10 % 10,
                    (unsigned)(a->prof_draw_us / k),
                    (unsigned)(a->prof_cmp ? a->prof_diff_us / a->prof_cmp : 0),
                    (unsigned)a->prof_cmp, (unsigned)k,
                    (unsigned)(a->prof_cmp ? a->prof_tiles * 100 / ((uint64_t)a->prof_cmp * TILES_X * rows) : 100),
                    (unsigned)(a->prof_px * 100 / ((uint64_t)k * all)),
                    (unsigned)(a->prof_cmp ? a->prof_rects / a->prof_cmp : 0),
                    (unsigned)st.refresh_us_avg, (unsigned)st.scale_us_avg);
        a->prof_n = a->prof_cmp = 0;
        a->prof_draw_us = a->prof_diff_us = a->prof_px = a->prof_rects = a->prof_tiles = 0;
    }
}

/* --------------------------------------------------------------------------
 * Input the runtime delivers
 * -------------------------------------------------------------------------- */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return false;
    }
    if (state_is_flying(&a->g)) {
        pause_show(a);
        return true;        /* consumed: pause instead of leaving */
    }
    /* From the pause, back is leaving: exit_cb() asks for aos_ui_back(),
     * whose first act is to consult this callback, and consuming it here
     * would mean the pause's exit button never closed the app. */
    return false;
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static void prefs_load(app_t *a)
{
    g_t *g = &a->g;
    int32_t v = 0;

    if (aos_hal_pref_get_i32(KEY_HI, &v) && v > 0) {
        g->hiscore = (uint32_t)v;
    }
    if (aos_hal_pref_get_i32(KEY_AUTO, &v)) {
        g->autofire = (uint8_t)(v ? 1 : 0);
    }
    if (aos_hal_pref_get_i32(KEY_SFX, &v)) {
        s_sfx = (v != 0);
    }
    if (aos_hal_pref_get_i32(KEY_FPS, &v)) {
        g->show_fps = (uint8_t)(v ? 1 : 0);
    }
    if (aos_hal_pref_get_i32(KEY_DIFF, &v)) {
        a->diff_on = (v != 0);
    }
}

static void *g2043_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    app_t *a = (app_t *)lv_malloc_zeroed(sizeof(app_t));
    if (!a) {
        return NULL;
    }

    /* A trail on the serial port. If the board hangs or restarts, the last
     * line says which stage it was in: without this you have to guess. */
    uint32_t heap_int = 0, heap_psram = 0;
    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("2043", "opening | internal %u B, psram %u B",
                (unsigned)heap_int, (unsigned)heap_psram);

    /* The frame is the OS's canvas, and all of the screen: 240x426 x3 in
     * portrait, 240x360 x2 in landscape. It asks the OS for no controls, the
     * game draws and reads its own ("Controls"). */
    lv_obj_update_layout(root);
    a->root = root;
    a->land = lv_obj_get_width(root) > lv_obj_get_height(root);
    gx_h = a->land ? GX_H_LANDSCAPE : GX_H_PORTRAIT;
    a->r = aos_retro_begin(root, GX_W, GX_H, 0, 0);
    if (!a->r) {
        aos_hal_log("2043", "out of memory for the %u B canvas",
                    (unsigned)((size_t)GX_W * GX_H * sizeof(uint16_t)));
        lv_free(a);
        return NULL;
    }

    /* What the screen shows of the canvas. Without it (no memory) the game
     * presents all of it every frame, as it used to. */
    a->shown = (uint16_t *)malloc((size_t)GX_W * GX_H * sizeof(uint16_t));
    a->shown_ok = false;
    a->diff_on = true;
    a->prof_every = 300;
#ifdef AOS_SIM_BUILTIN
    if (getenv("G2043_PROF")) {
        a->prof_every = 60;
    }
#endif

    a->g.buf.px = a->r->px;
    a->g.buf.w  = GX_W;
    a->g.buf.h  = (int16_t)GX_H;
    a->g.rng    = (uint32_t)aos_hal_uptime_ms() | 1u;
    /* The watch started light and earned the decorations; the P4 scales by
     * hardware and draws the hundred thousand pixels with room to spare, so
     * it starts full and only drops them if the frames per second fall. */
    a->g.detail = 1;
    /* the ship appears above the fire buttons in portrait, lower in
     * landscape, where they are beside the field */
    a->g.home_y = (int16_t)G_ROW(a->land ? 80 : 70);
    /* AUTO starts off: the ship fires while DISPARAR is held and only then.
     * It stays in the menus, for playing with one thumb. */
    a->g.autofire = 0;
    a->auto_shown = 0xFF;

    prefs_load(a);

    /* touches from now on only: the one that opened the app is not a drag */
    aos_touch_frame_t now;
    if (aos_hal_touch_frame(&now)) {
        a->touch_seq = now.seq;
    }
    build_controls(a, root);

    /* the panels live on a stage exactly over the canvas */
    a->stage = lv_obj_create(root);
    lv_obj_remove_style_all(a->stage);
    lv_obj_set_pos(a->stage, a->r->x, a->r->y);
    lv_obj_set_size(a->stage, a->r->w * a->r->scale, a->r->h * a->r->scale);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->stage, LV_OBJ_FLAG_CLICKABLE);

    build_title(a, a->stage);
    build_pause(a, a->stage);
    build_over(a, a->stage);

    /* it starts in the menu, with the debris field behind */
    a->g.state = ST_TITLE;
    a->g.level = 2;
    a->g.lives = G_LIVES_START;
    a->g.px = FX(GX_W / 2);
    a->g.py = FX(a->g.home_y);
    gx_bg_reset(&a->g);
    title_refresh(a);
    overlay_show(a, a->title);

#ifdef AOS_SIM_BUILTIN
    /* Shortcuts for designing without playing twenty minutes to reach the
     * boss. They only exist in the simulator: there are no environment
     * variables on the board.
     *
     *   G2043_LEVEL=2     starts straight on the third planet, automatic fire
     *   G2043_BOSS=1      and jumps straight to the boss
     *   G2043_TEST=pu     drops one capsule of each type
     *   G2043_TEST=over   a single ship and little energy, for the end panel
     *   G2043_AUTO=1      the ship flies itself (implies G2043_LEVEL=0)
     *   G2043_FPS=1       the frames-per-second meter
     *   G2043_TRACE=1     prints wave, enemies alive and the trigger (waves_update);
     *                     G2043_TRACE=f ten times a second, to test the buttons
     */
    if (getenv("G2043_FPS")) {
        a->g.show_fps = 1;
    }
    const char *env_level = getenv("G2043_LEVEL");
    if (env_level || getenv("G2043_AUTO")) {
        a->g.autofire = 1;
        a->g.autoplay = getenv("G2043_AUTO") ? 1 : 0;
        overlay_hide_all(a);
        game_start(&a->g);
        level_start(&a->g, env_level ? atoi(env_level) : 0);
        if (getenv("G2043_BOSS")) {
            a->g.level_t = gx_levels[a->g.level].length;
            a->g.wave_next = gx_levels[a->g.level].wave_count;
            a->g.state = ST_PLAY;
            a->g.state_t = 0;
        }
        const char *test = getenv("G2043_TEST");
        if (test && test[0] == 'p') {
            /* one capsule of each type, to look at them all together */
            for (int i = 0; i < PU_COUNT && i < G_MAX_PICKUPS; i++) {
                g_drop_pickup(&a->g, FX(24 + i * 30), FX(G_ROW(8) + (i & 1) * 20), i);
            }
        } else if (test && test[0] == 'o') {
            a->g.lives = 1;         /* to reach the ending panel quickly */
            a->g.energy = 30;
        }
        chips_refresh(a);
    }
#endif

    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("2043", "ready | internal %u B, psram %u B | canvas %dx%d x%d",
                (unsigned)heap_int, (unsigned)heap_psram, GX_W, GX_H, a->r->scale);

    aos_retro_run(G_FPS, step, draw, a);
    return a;
}

static void g2043_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (!a) {
        return;
    }
    if (a->g.score >= a->g.hiscore) {
        aos_hal_pref_set_i32(KEY_HI, (int32_t)a->g.hiscore);
    }
    aos_retro_end();
    free(a->shown);
    lv_free(a);
}

/* Leaving mid-flight should not cost a ship: it pauses. */
static void g2043_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = (app_t *)inst;
    if (a) {
        pause_show(a);
    }
}

static bool g2043_init(aos_app_t *app)
{
    app->desc.id      = "demo.2043";
    app->desc.name    = "2043";
    app->desc.icon    = LV_SYMBOL_GPS;
    app->desc.icon_vec = AOS_ICON_SHIP;
    app->desc.color_a = 0xFF4A3D;
    app->desc.color_b = 0x6A2FB5;
    app->desc.order   = 145;
    /* LONG_DRAG: flying is a drag across the whole screen, much longer than
     * LVGL's 50 px gesture limit, and it must not lose its release.
     * No orientation flag: portrait is the game at its best (the whole
     * panel), but it also plays in landscape, centred, so it follows the
     * screen instead of turning it. Turning the screen creates it again. */
    app->desc.flags   = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                        AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;

    app->create  = g2043_create;
    app->destroy = g2043_destroy;
    app->hide    = g2043_hide;
    app->back    = app_back;
    return true;
}

AOS_APP_ENTRY(g2043_init);
