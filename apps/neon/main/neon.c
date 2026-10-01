/*
 * P4OS - NEON SNAKES
 *
 * Neon snakes eat neon fruit on a screen that is black everywhere else. No
 * score, no HUD: the snakes, the fruit and the frame of the arena.
 *
 *   Normal   one snake, cells of 24 px: the classic. Hit a wall or yourself
 *            and it is over.
 *   Combate  cells of 18 px and six snakes: you against five bots. A snake
 *            that dies flashes and leaves its body behind as sparks;
 *            everybody comes back after a while.
 *
 * It came from AmoledOS, where the arena was fixed to the watch's 368x448
 * (21x24 and 34x40 cells of 16 and 10 px). Here the arena is the WHOLE
 * screen, edge to edge, less only the home strip at the bottom: 28x49 cells
 * in Normal and 38x67 in Combate standing up, 51x26 and 69x36 lying down.
 * The watch's cells were a speck on a 5" panel; these read from arm's
 * length. A fruit per ~600 cells in Normal, one per ~130 in Combate
 * (layout(), start_play()).
 *
 * The control is the finger on the whole screen: swipe the way you want to
 * go - a short flick is enough, and one drag can turn several times - or
 * tap on the side of the head you want to turn to. There are no on-screen
 * arrows (an earlier version had them floating over the corners; on the
 * board swiping won). A small pause pill floats in the top-right corner, and
 * the system's back pauses too. Touches are read from the panel's own
 * samples (aos_hal_touch_frames), both fingers, not as LVGL events: a flick
 * shorter than a frame still turns, and a second finger landing while the
 * first drags is a swipe of its own.
 *
 * The files:
 *   ns_game.c  the rules: deterministic, integers only, no LVGL
 *   ns_art.c   every sprite, drawn by code at the cell size of the mode
 *   ns_draw.c  the compositor: the screen repainted cell by cell, only
 *              where something changed
 *   neon.c     this: menus, input, the link, the timer
 *   tools/ns_harness.c  checks the rules, the compositor and the lockstep
 *
 * TWO DEVICES are one match run twice (lockstep, as in Truco). The host (the
 * lower MAC) owns the clock: on every step it decides where both humans turn
 * - its own finger and the guest's TURN messages - and sends that decision
 * in a STEP on the reliable channel before applying it; the guest applies
 * exactly the STEPs it receives, in order, and nothing else. Both engines
 * start from the same seed, so the boards stay equal; each STEP carries the
 * host's hash of the board after it, and the guest checks it. That match
 * keeps the watch's arena, four snakes and protocol, drawn with bigger
 * cells, so a P4 could play a watch once the P4 has the link. It does not
 * yet: when aos_hal_link_start() fails the option is greyed out and says
 * why, and Normal and Combate against the bots are the game.
 */
#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ns_art.h"
#include "ns_draw.h"
#include "ns_game.h"

#define FRAME_MS        33
/* A drag this long is a turn. Short on purpose: at 18-24 px cells a flick
 * of the thumb has to be enough, and each leg of a long drag is measured
 * again from where the last turn was taken. */
#define SWIPE_PX        18
#define STEP_COMBAT_MS  125
#define PROTO           1
#define LINK_APP        "neon"

/* The cells a new game gets. The watch's 16 and 10 px were tiny on this
 * panel; the arena is the whole screen at these. */
#define CELL_NORMAL     24
#define CELL_COMBAT     18
/* When an arena has to fit a screen it was not laid out for - the screen
 * turned mid-game, or the two-device match's 34x40 - the cell is whatever
 * fits, within these. */
#define CELL_MIN        8
#define CELL_MAX        28

/* The two-device match: the watch's arena, so both engines agree. */
#define MULTI_COLS      34
#define MULTI_ROWS      40
#define MULTI_SNAKES    4
#define MULTI_FRUITS    9

#define CELLS_PER_FRUIT_NORMAL  600
#define CELLS_PER_FRUIT_COMBAT  130

/* The OS's edges: a drag up from the bottom 36 px goes home, so the field
 * stops above it; a drag down from the top strip opens the panels, so the
 * pause pill sits below it (the field itself runs under it: a swipe down
 * that starts that high is the panels', the rest of the field is ours). */
#define EDGE_TOP_TALL   48
#define EDGE_TOP_WIDE   40
#define EDGE_HOME       36
/* The pause pill's target is this much bigger than its drawing. */
#define PAUSE_HIT       22

/* -------------------------------------------------------------------------- */
/* What goes over the air                                                      */

enum { MSG_HELLO = 1, MSG_START, MSG_READY, MSG_STEP, MSG_TURN, MSG_BYE };

typedef struct __attribute__((packed)) {
    uint8_t  type, proto;
    uint32_t nonce;
    uint8_t  mac[6];
} msg_hello_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, proto;
    uint32_t seed, host_nonce, guest_nonce;
} msg_start_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, proto;
    uint32_t host_nonce;
} msg_ready_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, proto;
    uint32_t step;
    uint8_t  d0, d1;
    uint32_t hash;
} msg_step_t;

typedef struct __attribute__((packed)) {
    uint8_t  type, proto;
    uint8_t  dir;
} msg_turn_t;

/* -------------------------------------------------------------------------- */
/* The app                                                                     */

typedef enum {
    SCR_MENU = 0,
    SCR_COMBAT,         /* one player or two                       */
    SCR_LOBBY,          /* looking for the other device            */
    SCR_PLAY,
} screen_t;

typedef enum {
    OV_NONE = 0,
    OV_PAUSE,
    OV_OVER,            /* normal mode: the snake is gone           */
    OV_GONE,            /* two devices: the other one left          */
} overlay_t;

enum { PLAY_NORMAL, PLAY_SOLO, PLAY_MULTI };

typedef struct { int16_t x, y, w, h; } box_t;

/* Where everything goes for the current size (layout()). */
typedef struct {
    int   w, h;
    bool  tall;
    box_t field;                /* the arena and its margin ring go in here  */
    box_t pause;
} layout_t;

/* One finger, followed from sample to sample. */
enum { TK_FIELD = 0, TK_PAUSE };

typedef struct {
    bool     on;
    bool     dead;              /* landed on a menu, or before a game began  */
    bool     dragged;
    uint8_t  kind;              /* TK_*                                      */
    int16_t  x, y;              /* the last sample                           */
    int16_t  ax, ay;            /* where the current swipe leg started       */
} track_t;

typedef struct {
    lv_obj_t   *root, *canvas, *msg, *help;
    lv_obj_t   *btn[3], *btn_lbl[3];
    lv_obj_t   *pause;
    lv_timer_t *timer;
    layout_t    L;

    uint16_t   *fb, *bg, *menu;     /* menu: the title screen, drawn once */
    size_t      npx;                /* pixels in each buffer              */
    bool        menu_ok;            /* 'menu' holds this size's title     */
    ns_art_t    art_normal, art_combat;
    bool        have_normal, have_combat;
    ns_game_t  *g;
    ns_view_t  *view;
    uint8_t     cover[(NS_MAX_CELLS + 7) / 8];   /* scratch for cover_for() */

    screen_t    scr;
    overlay_t   ov;
    uint8_t     play;               /* PLAY_*                                */
    uint8_t     me;                 /* my snake's index                      */
    bool        autoplay;           /* NS_AUTO: the bot steers my snake      */

    /* input */
    uint8_t     q[2], nq;           /* turns waiting for the next step       */
    track_t     tk[2];
    uint32_t    touch_seq;
    bool        pause_held;

    uint32_t    last_step_ms, over_at_ms;

    /* the link */
    bool        no_link;            /* NS_LINK=0, simulator only             */
    bool        link_up, link_failed, have_partner, is_host, role_known, started;
    uint32_t    nonce, their_nonce, lobby_ms, hello_ms, last_rx_ms, away_ms;
    uint8_t     their_mac[6];
    char        their_name[AOS_LINK_NAME_MAX + 1];
    uint8_t     guest_q[4], guest_nq;
    bool        desync_told;

    /* deferred from events */
    bool        want_exit, leaving, closing, want_pause, want_back;
    int8_t      want_btn;           /* a button was clicked: its index      */

    /* stats */
    uint32_t    stat_t0, stat_frames, stat_px;
} app_t;

static void show_screen(app_t *a, screen_t s);
static void show_overlay(app_t *a, overlay_t ov);
static void start_play(app_t *a, uint8_t play);
static void link_down(app_t *a);
static void guest_apply(app_t *a, const msg_step_t *m);

static const uint32_t CYAN = 0x19F5FF, MAGENTA = 0xFF2FD0, LIME = 0x8CFF2E,
                      VIOLET = 0x8A6CFF, GREY = 0x505066;

/* -------------------------------------------------------------------------- */
/* Layout                                                                      */

static inline box_t box(int x, int y, int w, int h)
{
    box_t b = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
    return b;
}

static void layout(app_t *a)
{
    layout_t *L = &a->L;
    lv_obj_update_layout(a->root);
    int W = lv_obj_get_width(a->root), H = lv_obj_get_height(a->root);
    L->w = W;
    L->h = H;
    L->tall = H >= W;
    /* The field is the screen; only the pause pill floats over it. */
    L->field = box(0, 0, W, H - EDGE_HOME);
    int top = L->tall ? EDGE_TOP_TALL : EDGE_TOP_WIDE;
    L->pause = box(W - 30 - 96, top + 12, 96, 64);
}

static bool boxes_meet(const box_t *b, int x0, int y0, int x1, int y1)
{
    return x0 < b->x + b->w && x1 > b->x && y0 < b->y + b->h && y1 > b->y;
}

/* The cells the pause pill covers, for an arena of cols x rows cells of
 * 'cell' px in the field: the engine keeps fruit and new snakes off them,
 * so nothing worth reaching hides under it. */
static const uint8_t *cover_for(app_t *a, int cols, int rows, int cell)
{
    const layout_t *L = &a->L;
    int ox, oy;
    ns_arena_origin(cell, cols, rows, L->field.x, L->field.y, L->field.w, L->field.h, &ox, &oy);
    memset(a->cover, 0, sizeof a->cover);
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            int x0 = ox + x * cell, y0 = oy + y * cell, x1 = x0 + cell, y1 = y0 + cell;
            if (boxes_meet(&L->pause, x0, y0, x1, y1)) {
                int i = y * cols + x;
                a->cover[i >> 3] |= (uint8_t)(1u << (i & 7));
            }
        }
    }
    return a->cover;
}

/* The arena a new game gets: as many cells of 'cell' px as the field holds,
 * less the ring of margin cells the frame lives in. */
static void arena_for(const app_t *a, int cell, int *cols, int *rows)
{
    *cols = a->L.field.w / cell - 2;
    *rows = a->L.field.h / cell - 2;
    if (*cols > NS_MAX_COLS) *cols = NS_MAX_COLS;
    if (*rows > NS_MAX_ROWS) *rows = NS_MAX_ROWS;
    while (*cols * *rows > NS_MAX_CELLS) (*rows)--;
}

/* The cell that fits an existing arena in the field, up to 'cap'. */
static int fit_cell(const app_t *a, int cols, int rows, int cap)
{
    int cw = a->L.field.w / (cols + 2), ch = a->L.field.h / (rows + 2);
    int c = cw < ch ? cw : ch;
    if (c > cap) c = cap;
    if (c < CELL_MIN) c = CELL_MIN;
    return c;
}

/* -------------------------------------------------------------------------- */
/* Pushing pixels                                                              */

static void invalidate(app_t *a, int x0, int y0, int x1, int y1)
{
    lv_area_t co;
    lv_obj_get_coords(a->canvas, &co);
    lv_area_t ar = { co.x1 + x0, co.y1 + y0, co.x1 + x1 - 1, co.y1 + y1 - 1 };
    lv_obj_invalidate_area(a->canvas, &ar);
}

static void push_view(app_t *a)
{
    for (int i = 0; i < a->view->nrects; i++) {
        const ns_rect_t *r = &a->view->rects[i];
        invalidate(a, r->x0, r->y0, r->x1, r->y1);
    }
    a->stat_px += a->view->pixels;
}

/* Where the menus go. Standing up everything is one column down the whole
 * screen; lying down the title and the fruit take the left half and the
 * buttons the right. */
typedef struct {
    int   title_cx, title_y;
    float title_scale;
    int   fruit_cx, fruit_y, fruit_cell, fruit_pitch;
    int   btn_cx, btn_w, btn_h, btn_y0, btn_step;
    int   text_y, text_w;
} menu_geo_t;

static menu_geo_t menu_geo(const app_t *a)
{
    const int W = a->L.w;
    menu_geo_t m;
    if (a->L.tall) {
        m = (menu_geo_t){ W / 2, 110, 2.4f, W / 2, 500, 36, 96,
                          W / 2, 580, 124, 660, 152, 1116, 640 };
    } else {
        m = (menu_geo_t){ W / 4, 110, 1.9f, W / 4, 430, 30, 82,
                          W * 3 / 4, 520, 108, 120, 136, 528, 540 };
    }
    return m;
}

/* The menus' background: the title and a row of fruit, on black. */
static void draw_menu_bg(app_t *a)
{
    const int W = a->L.w, H = a->L.h;
    const size_t px = (size_t)W * H * 2;
    /* the tube letters are a few hundred thousand distances: once per size
     * of the screen is enough */
    if (a->menu && a->menu_ok) {
        memcpy(a->fb, a->menu, px);
        invalidate(a, 0, 0, W, H);
        return;
    }
    memset(a->fb, 0, px);
    const menu_geo_t m = menu_geo(a);
    ns_art_title(a->fb, W, H, m.title_cx, m.title_y, m.title_scale);
    const int fruit_y = m.fruit_y;
    /* the row of fruit bigger than in play: it is decoration */
    const int C = m.fruit_cell, S = 2 * C, pitch = m.fruit_pitch;
    uint16_t *spr = malloc((size_t)S * S * 2);
    if (spr) {
        int x0 = m.fruit_cx - NS_FRUIT_COUNT * pitch / 2 + (pitch - S) / 2;
        for (int k = 0; k < NS_FRUIT_COUNT; k++) {
            ns_art_fruit(spr, k, C, 1);
            for (int y = 0; y < S; y++) {
                uint16_t *d = a->fb + (fruit_y + y) * W + x0 + k * pitch;
                for (int x = 0; x < S; x++) d[x] = ns_max565(d[x], spr[y * S + x]);
            }
        }
        free(spr);
    }
    if (!a->menu) a->menu = malloc(a->npx * 2);
    if (a->menu) {
        memcpy(a->menu, a->fb, px);
        a->menu_ok = true;
    }
    invalidate(a, 0, 0, W, H);
}

/* -------------------------------------------------------------------------- */
/* Menu buttons: three neon outlines, reused by every screen                   */

/* cx: the column it is centred on (the menus' or the screen's) */
static void btn_set(app_t *a, int i, const char *text, uint32_t rgb, int cx, int y)
{
    lv_obj_t *b = a->btn[i];
    if (!text) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(b, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(b, cx - menu_geo(a).btn_w / 2, y);
    lv_obj_set_style_border_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_shadow_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_shadow_opa(b, rgb == GREY ? LV_OPA_TRANSP : LV_OPA_50, 0);
    lv_obj_set_style_text_color(a->btn_lbl[i], lv_color_hex(rgb == GREY ? 0x8A8AA0 : rgb), 0);
    /* the long ones ("CONTROL: DESLIZAR", and every German word) drop to
     * the body font rather than spill over the outline */
    lv_obj_set_style_text_font(a->btn_lbl[i], strlen(text) > 20 ? aos_font_body : aos_font_title, 0);
    lv_label_set_text(a->btn_lbl[i], text);
}

static void btn_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing) return;
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int i = 0; i < 3; i++) {
        if (t == a->btn[i]) a->want_btn = (int8_t)i;       /* acted on in the timer */
    }
}

static lv_obj_t *make_btn(app_t *a, int i)
{
    const menu_geo_t m = menu_geo(a);
    const int w = m.btn_w, h = m.btn_h;
    lv_obj_t *b = lv_obj_create(a->root);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 4, 0);
    lv_obj_set_style_shadow_width(b, 28, 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1A1A28), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b, btn_cb, LV_EVENT_CLICKED, a);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, aos_font_title, 0);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    a->btn_lbl[i] = l;
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

/* How to play, under the menu's buttons. */
static const char *help_text(void)
{
    return _("Deslizá el dedo hacia donde quieras ir: alcanza con un toque corto, y un "
             "mismo trazo puede doblar varias veces. También podés tocar al costado "
             "de la cabeza.");
}

static void set_label(lv_obj_t *l, const char *text)
{
    if (!text || !*text) {
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (strcmp(lv_label_get_text(l), text) != 0) {
        lv_label_set_text(l, text);
    }
    lv_obj_remove_flag(l, LV_OBJ_FLAG_HIDDEN);
}

static void set_msg(app_t *a, const char *text)
{
    set_label(a->msg, text);
}

/* -------------------------------------------------------------------------- */
/* The pause pill                                                              */

/* A neon outline floating over the field, translucent so a snake passing
 * under it still shows, and lit while a finger is on it. Not clickable:
 * touch_poll() reads it from the panel's samples. No shadow: a glow over
 * the game would be drawn again on every frame a snake passes under. */
static lv_obj_t *make_ctrl(app_t *a, box_t b, const char *glyph, const lv_font_t *font,
                           uint32_t rgb)
{
    lv_obj_t *o = lv_obj_create(a->root);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, b.x, b.y);
    lv_obj_set_size(o, b.w, b.h);
    lv_obj_set_style_radius(o, 26, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_10, 0);
    lv_obj_set_style_border_width(o, 3, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_border_opa(o, LV_OPA_50, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_opa(l, LV_OPA_70, 0);
    lv_obj_center(l);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void controls_show(app_t *a, bool show)
{
    if (a->pause) {
        if (show) lv_obj_remove_flag(a->pause, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(a->pause, LV_OBJ_FLAG_HIDDEN);
    }
}

/* Every finger down now stops counting until it lifts: the one that tapped
 * a menu button, or that was on the field when a panel came up, must not
 * turn the snake of the game that starts under it. */
static void input_reset(app_t *a)
{
    for (int i = 0; i < 2; i++) a->tk[i].dead = true;
    a->pause_held = false;
    if (a->pause) lv_obj_remove_state(a->pause, LV_STATE_PRESSED);
}

/* -------------------------------------------------------------------------- */
/* Screens                                                                     */

static int btn_y(const app_t *a, int i)
{
    const menu_geo_t m = menu_geo(a);
    return m.btn_y0 + i * m.btn_step;
}

/* A label centred on column cx, its top at y. */
static void place_text(app_t *a, lv_obj_t *l, int cx, int y)
{
    lv_obj_align(l, LV_ALIGN_TOP_MID, cx - a->L.w / 2, y);
}

static void menu_labels(app_t *a, bool help)
{
    set_label(a->help, help ? help_text() : NULL);
}

static void show_screen(app_t *a, screen_t s)
{
    const menu_geo_t m = menu_geo(a);
    const int cx = m.btn_cx;
    a->scr = s;
    a->ov  = OV_NONE;
    set_msg(a, NULL);
    place_text(a, a->msg, cx, m.text_y);
    menu_labels(a, s == SCR_MENU);
    controls_show(a, s == SCR_PLAY);
    input_reset(a);
    switch (s) {
    case SCR_MENU:
        draw_menu_bg(a);
        btn_set(a, 0, _("NORMAL"), CYAN, cx, btn_y(a, 0));
        btn_set(a, 1, _("COMBATE"), MAGENTA, cx, btn_y(a, 1));
        btn_set(a, 2, NULL, 0, 0, 0);
        break;
    case SCR_COMBAT:
        draw_menu_bg(a);
        btn_set(a, 0, _("UN JUGADOR"), MAGENTA, cx, btn_y(a, 0));
        /* greyed once the link would not start: this device has none */
        btn_set(a, 1, _("MULTIJUGADOR"), a->link_failed ? GREY : LIME, cx, btn_y(a, 1));
        btn_set(a, 2, _("VOLVER"), VIOLET, cx, btn_y(a, 2));
        if (a->link_failed) {
            set_msg(a, _("El juego de a dos necesita el enlace, y este equipo todavía no lo tiene"));
        }
        break;
    case SCR_LOBBY:
        draw_menu_bg(a);
        btn_set(a, 0, NULL, 0, 0, 0);
        btn_set(a, 1, NULL, 0, 0, 0);
        btn_set(a, 2, _("CANCELAR"), VIOLET, cx, btn_y(a, 2));
        break;
    case SCR_PLAY:
        btn_set(a, 0, NULL, 0, 0, 0);
        btn_set(a, 1, NULL, 0, 0, 0);
        btn_set(a, 2, NULL, 0, 0, 0);
        break;
    }
}

static void show_overlay(app_t *a, overlay_t ov)
{
    a->ov = ov;
    /* over the middle of the screen, whatever the layout */
    const menu_geo_t m = menu_geo(a);
    const int cx = a->L.w / 2, y0 = a->L.h / 2 - m.btn_h - 14, y1 = a->L.h / 2 + 14;
    controls_show(a, ov == OV_NONE);
    input_reset(a);
    place_text(a, a->msg, cx, y0 - 120);
    switch (ov) {
    case OV_NONE:
        btn_set(a, 0, NULL, 0, 0, 0);
        btn_set(a, 1, NULL, 0, 0, 0);
        btn_set(a, 2, NULL, 0, 0, 0);
        set_msg(a, NULL);
        break;
    case OV_PAUSE:
        btn_set(a, 0, _("SEGUIR"), CYAN, cx, y0);
        btn_set(a, 1, _("MENÚ"), MAGENTA, cx, y1);
        btn_set(a, 2, NULL, 0, 0, 0);
        break;
    case OV_OVER:
        btn_set(a, 0, _("OTRA VEZ"), CYAN, cx, y0);
        btn_set(a, 1, _("MENÚ"), MAGENTA, cx, y1);
        btn_set(a, 2, NULL, 0, 0, 0);
        break;
    case OV_GONE:
        btn_set(a, 0, NULL, 0, 0, 0);
        btn_set(a, 1, _("MENÚ"), MAGENTA, cx, y1);
        btn_set(a, 2, NULL, 0, 0, 0);
        break;
    }
}

/* -------------------------------------------------------------------------- */
/* Steering                                                                    */

/* The direction the snake will have once the queue is done: what a new turn
 * is measured against. */
static uint8_t last_dir(const app_t *a)
{
    if (a->nq) return a->q[a->nq - 1];
    return a->g->s[a->me].dir;
}

static bool steering(const app_t *a)
{
    return a->scr == SCR_PLAY && a->ov == OV_NONE && a->g && !a->g->over;
}

static void queue_turn(app_t *a, uint8_t d)
{
    if (!steering(a)) return;
    uint8_t cur = last_dir(a);
    if (d == cur || d == ((cur + 2) & 3)) return;
    if (a->nq < 2) {
        a->q[a->nq++] = d;
    } else {
        a->q[1] = d;            /* a third turn replaces the second */
    }
    if (a->play == PLAY_MULTI && !a->is_host) {
        msg_turn_t m = { MSG_TURN, PROTO, d };
        aos_hal_link_send_reliable(&m, sizeof m);
    }
}

static uint8_t pop_turn(app_t *a)
{
    if (!a->nq) return NS_NODIR;
    uint8_t d = a->q[0];
    a->q[0] = a->q[1];
    a->nq--;
    return d;
}

static bool in_box(const box_t *b, int x, int y, int grow)
{
    return x >= b->x - grow && x < b->x + b->w + grow && y >= b->y - grow && y < b->y + b->h + grow;
}

/* A tap on the field: turn towards the side of the head it landed on. */
static void tap_turn(app_t *a, int x, int y)
{
    const ns_snake_t *s = &a->g->s[a->me];
    if (!s->alive || s->dying) return;
    const ns_view_t *v = a->view;
    int hx = v->ox + ns_seg_x(s, 0) * v->cell + v->cell / 2;
    int hy = v->oy + ns_seg_y(s, 0) * v->cell + v->cell / 2;
    uint8_t cur = last_dir(a);
    if (cur == NS_UP || cur == NS_DOWN) {
        queue_turn(a, x < hx ? NS_LEFT : NS_RIGHT);
    } else {
        queue_turn(a, y < hy ? NS_UP : NS_DOWN);
    }
}

static void track_press(app_t *a, track_t *t, int x, int y)
{
    memset(t, 0, sizeof *t);
    t->on = true;
    t->x = t->ax = (int16_t)x;
    t->y = t->ay = (int16_t)y;
    t->dead = !steering(a);
    if (in_box(&a->L.pause, x, y, PAUSE_HIT)) {
        t->kind = TK_PAUSE;
        if (!t->dead) a->want_pause = true;
    } else {
        t->kind = TK_FIELD;
    }
}

static void track_move(app_t *a, track_t *t, int x, int y)
{
    t->x = (int16_t)x;
    t->y = (int16_t)y;
    if (t->kind != TK_FIELD || t->dead) return;
    int dx = x - t->ax, dy = y - t->ay;
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    if (ax >= SWIPE_PX || ay >= SWIPE_PX) {
        /* one drag can turn several times: each leg is measured from
         * where the last turn was taken */
        queue_turn(a, ax > ay ? (dx > 0 ? NS_RIGHT : NS_LEFT) : (dy > 0 ? NS_DOWN : NS_UP));
        t->ax = (int16_t)x;
        t->ay = (int16_t)y;
        t->dragged = true;
    }
}

static void track_release(app_t *a, track_t *t)
{
    t->on = false;
    if (t->kind == TK_FIELD && !t->dead && !t->dragged && steering(a)) {
        tap_turn(a, t->x, t->y);
    }
}

/* Every touch sample since the last frame, both fingers. The panel does not
 * say which finger is which, so each point goes to the track that was
 * nearest to it in the previous sample; a track left without a point has
 * lifted, a point left without a track has landed. Samples only come on
 * change, so what the last one said holds until the next. */
static void touch_poll(app_t *a)
{
    aos_touch_frame_t fr[16];
    uint32_t n = aos_hal_touch_frames(a->touch_seq, fr, 16);
    lv_area_t rc;
    lv_obj_get_coords(a->root, &rc);    /* the runtime slides the root */

    for (uint32_t k = 0; k < n; k++) {
        const aos_touch_frame_t *f = &fr[k];
        a->touch_seq = f->seq;
        int px[2], py[2], np = f->count > 2 ? 2 : f->count;
        bool used[2] = { false, false };
        for (int i = 0; i < np; i++) {
            px[i] = f->x[i] - rc.x1;
            py[i] = f->y[i] - rc.y1;
        }
        for (int t = 0; t < 2; t++) {
            track_t *tk = &a->tk[t];
            if (!tk->on) continue;
            int best = -1, best_d = 240 * 240;
            for (int i = 0; i < np; i++) {
                if (used[i]) continue;
                int dx = px[i] - tk->x, dy = py[i] - tk->y, d = dx * dx + dy * dy;
                if (d < best_d) { best_d = d; best = i; }
            }
            if (best >= 0) {
                used[best] = true;
                track_move(a, tk, px[best], py[best]);
            } else {
                track_release(a, tk);
            }
        }
        for (int i = 0; i < np; i++) {
            if (used[i]) continue;
            for (int t = 0; t < 2; t++) {
                if (!a->tk[t].on) {
                    track_press(a, &a->tk[t], px[i], py[i]);
                    break;
                }
            }
        }
    }

    /* the pill lights while a finger is on it, like the OS's buttons */
    bool pause = false;
    for (int t = 0; t < 2; t++) {
        const track_t *tk = &a->tk[t];
        if (tk->on && !tk->dead && tk->kind == TK_PAUSE) pause = true;
    }
    if (pause != a->pause_held && a->pause) {
        if (pause) lv_obj_add_state(a->pause, LV_STATE_PRESSED);
        else lv_obj_remove_state(a->pause, LV_STATE_PRESSED);
        a->pause_held = pause;
    }
}

/* Samples from now on only (the finger that opened the app, or that turned
 * the screen, is not a tap), and whatever finger is down stays dead. */
static void touch_sync(app_t *a)
{
    aos_touch_frame_t now;
    if (aos_hal_touch_frame(&now)) a->touch_seq = now.seq;
    input_reset(a);
}

/* -------------------------------------------------------------------------- */
/* Playing                                                                     */

static bool ensure_art(app_t *a, bool combat, int cell)
{
    ns_art_t *art = combat ? &a->art_combat : &a->art_normal;
    bool *have = combat ? &a->have_combat : &a->have_normal;
    if (*have && art->cell == cell) return true;
    if (*have) {
        ns_art_free(art);
        *have = false;
    }
    uint32_t t0 = (uint32_t)aos_hal_uptime_ms();
    *have = ns_art_build(art, cell, combat ? NS_MAX_SNAKES : 1);
    aos_hal_log("neon", "%s sprites at %d px drawn in %u ms", combat ? "combat" : "normal", cell,
                (unsigned)((uint32_t)aos_hal_uptime_ms() - t0));
    return *have;
}

static void begin_view(app_t *a, int halo)
{
    bool combat = a->g->mode == NS_MODE_COMBAT;
    const box_t *f = &a->L.field;
    ns_view_init(a->view, a->fb, a->bg, a->L.w, a->L.h, combat ? &a->art_combat : &a->art_normal,
                 a->g, f->x, f->y, f->w, f->h);
    ns_view_full(a->view, a->g);
    if (halo) ns_view_halo(a->view, a->me, halo);
    push_view(a);
}

/* The cell a game in progress is drawn at on the current layout: its
 * mode's own if the arena fits (it was laid out for it), smaller if the
 * screen turned under it; the two-device match's small arena grows to fill
 * the field. */
static int play_cell(const app_t *a)
{
    const ns_game_t *g = a->g;
    int cap = a->play == PLAY_MULTI ? CELL_MAX : g->mode == NS_MODE_COMBAT ? CELL_COMBAT : CELL_NORMAL;
    return fit_cell(a, g->cols, g->rows, cap);
}

static void start_play(app_t *a, uint8_t play)
{
    bool combat = play != PLAY_NORMAL;
    uint32_t seed = (uint32_t)aos_hal_uptime_ms() * 2654435761u;
    if (play == PLAY_NORMAL || play == PLAY_SOLO) {
        int cell = combat ? CELL_COMBAT : CELL_NORMAL, cols, rows;
        arena_for(a, cell, &cols, &rows);
        int cells = cols * rows;
        const uint8_t *cover = cover_for(a, cols, rows, cell);
        if (play == PLAY_NORMAL) {
            int fruits = cells / CELLS_PER_FRUIT_NORMAL;
            ns_init(a->g, NS_MODE_NORMAL, seed, 1, 1, cols, rows, fruits < 1 ? 1 : fruits, cover);
        } else {
            int fruits = cells / CELLS_PER_FRUIT_COMBAT;
            ns_init(a->g, NS_MODE_COMBAT, seed, 1, cells > 1800 ? NS_MAX_SNAKES : MULTI_SNAKES,
                    cols, rows, fruits < MULTI_FRUITS ? MULTI_FRUITS : fruits, cover);
        }
        a->me = 0;
    }
    /* PLAY_MULTI: the engine was already started by the handshake */
    a->play = play;
    if (!ensure_art(a, combat, play_cell(a))) {
        aos_ui_toast(_("Sin memoria"), 2000);
        show_screen(a, SCR_MENU);
        return;
    }
    a->nq = 0;
    a->guest_nq = 0;
    a->last_step_ms = (uint32_t)aos_hal_uptime_ms();
    /* the partner's clocks start now, not at zero: a beacon that has not
     * come round yet is not a partner that left */
    a->away_ms = a->last_rx_ms = a->last_step_ms;
    a->over_at_ms = 0;
    show_screen(a, SCR_PLAY);
    touch_sync(a);
    begin_view(a, 75);
    aos_hal_log("neon", "%s: %dx%d cells of %d px, %d snakes, %d fruits",
                play == PLAY_NORMAL ? "normal" : play == PLAY_SOLO ? "combat" : "multi",
                a->g->cols, a->g->rows, a->view->cell, a->g->nsnakes, a->g->fruit_target);
}

static uint32_t step_ms(const app_t *a)
{
    if (a->g->mode == NS_MODE_COMBAT) return STEP_COMBAT_MS;
    /* the classic gets faster as the snake grows, down to 85 ms */
    int len = a->g->s[0].len;
    int ms = 150 - (len - 4) * 2;
    return (uint32_t)(ms < 85 ? 85 : ms);
}

/* What the bot would do for my snake, without touching the dice the engine
 * shares with the other device. */
static uint8_t auto_choice(app_t *a)
{
    const ns_snake_t *s = &a->g->s[a->me];
    if (!s->alive || s->dying) return NS_NODIR;
    uint32_t keep = a->g->rng;
    uint8_t d = ns_bot_choice(a->g, a->me);
    a->g->rng = keep;
    return d;
}

static void after_step(app_t *a)
{
    ns_game_t *g = a->g;
    for (int i = 0; i < g->nev; i++) {
        const ns_event_t *e = &g->ev[i];
        bool mine = e->snake == a->me;
        switch (e->type) {
        case NS_EV_EAT:
            ns_view_fx(a->view, NS_FX_RING, e->x, e->y, ns_fruit_rgb(e->kind));
            if (mine) aos_hal_beep(e->kind >= NS_SPARK_0 ? 1500 : 1100, 25);
            break;
        case NS_EV_DIE:
            ns_view_fx(a->view, NS_FX_BURST, e->x, e->y, ns_snake_rgb(g->s[e->snake].colour));
            if (mine) {
                aos_hal_beep(220, 90);
                aos_hal_beep(140, 160);
            }
            break;
        case NS_EV_SPAWN:
            if (mine && g->step_no > 1) ns_view_halo(a->view, a->me, 60);
            break;
        }
    }
    g->nev = 0;
}

static void local_step(app_t *a)
{
    uint8_t dirs[NS_MAX_SNAKES];
    memset(dirs, NS_NODIR, sizeof dirs);
    dirs[a->me] = a->autoplay ? auto_choice(a) : pop_turn(a);
    ns_step(a->g, dirs);
    after_step(a);
}

/* -------------------------------------------------------------------------- */
/* Two devices                                                                 */

static void send_hello(app_t *a)
{
    aos_link_stats_t st;
    aos_hal_link_stats(&st);
    msg_hello_t h = { .type = MSG_HELLO, .proto = PROTO, .nonce = a->nonce };
    memcpy(h.mac, st.own_mac, 6);
    aos_hal_link_send_partner(&h, sizeof h);
}

static void link_down(app_t *a)
{
    if (!a->link_up) return;
    uint8_t bye[2] = { MSG_BYE, PROTO };
    for (int i = 0; i < 3; i++) aos_hal_link_send_partner(bye, sizeof bye);
    aos_hal_link_offer("");
    aos_hal_link_stop();
    a->link_up = false;
    a->role_known = a->started = false;
}

static void enter_lobby(app_t *a)
{
    if (!a->link_up) {
        a->link_up = !a->no_link && aos_hal_link_start();
        if (!a->link_up) {
            /* No link on this device (the P4's radio is behind the C6 and
             * ESP-NOW is not there yet): the option greys out and says so,
             * and stays that way while the app is open. */
            a->link_failed = true;
            aos_hal_log("neon", "the link did not start: two-player mode off");
            show_screen(a, SCR_COMBAT);
            return;
        }
        aos_hal_link_offer(LINK_APP);
    }
    show_screen(a, SCR_LOBBY);
    a->have_partner = false;
    a->role_known = a->started = false;
    a->their_nonce = 0;
    a->desync_told = false;
    a->nonce = ((uint32_t)aos_hal_uptime_ms() * 2654435761u) | 1u;
    a->lobby_ms = (uint32_t)aos_hal_uptime_ms();
    a->hello_ms = 0;
    aos_hal_link_reliable_reset();
    set_msg(a, _("Buscando al otro equipo..."));
}

static void gone(app_t *a, const char *why)
{
    if (a->scr != SCR_PLAY && a->scr != SCR_LOBBY) return;
    char buf[96];
    snprintf(buf, sizeof buf, why, a->their_name[0] ? a->their_name : "?");
    if (a->scr == SCR_PLAY) {
        show_overlay(a, OV_GONE);
    }
    set_msg(a, buf);
    a->started = false;
}

/* Host: the other device is here and in the app - deal. */
static void host_start(app_t *a)
{
    msg_start_t m = { .type = MSG_START, .proto = PROTO,
                      .seed = ((uint32_t)aos_hal_uptime_ms() * 2246822519u) ^ a->nonce,
                      .host_nonce = a->nonce, .guest_nonce = a->their_nonce };
    ns_init(a->g, NS_MODE_COMBAT, m.seed, 2, MULTI_SNAKES, MULTI_COLS, MULTI_ROWS, MULTI_FRUITS, NULL);
    aos_hal_link_send_reliable(&m, sizeof m);
    a->started = false;                 /* until READY */
}

static void handle_frame(app_t *a, const uint8_t *d, int len, bool reliable)
{
    if (len < 2 || d[1] != PROTO) return;
    a->last_rx_ms = (uint32_t)aos_hal_uptime_ms();
    switch (d[0]) {
    case MSG_HELLO: {
        if (len < (int)sizeof(msg_hello_t)) return;
        const msg_hello_t *h = (const msg_hello_t *)d;
        if (a->role_known && a->their_nonce && h->nonce != a->their_nonce && a->scr == SCR_PLAY) {
            /* the other one left and came back: that match is over */
            gone(a, _("%s salió del juego"));
            return;
        }
        if (a->scr != SCR_LOBBY) return;
        bool fresh = h->nonce != a->their_nonce;
        a->their_nonce = h->nonce;
        memcpy(a->their_mac, h->mac, 6);
        if (!a->role_known) {
            aos_link_stats_t st;
            aos_hal_link_stats(&st);
            a->is_host = memcmp(st.own_mac, h->mac, 6) < 0;
            a->role_known = true;
            send_hello(a);              /* so the other decides too */
        }
        if (a->is_host && fresh) host_start(a);
        break;
    }
    case MSG_START: {
        if (!reliable || a->is_host || a->scr != SCR_LOBBY || len < (int)sizeof(msg_start_t)) return;
        const msg_start_t *m = (const msg_start_t *)d;
        if (m->guest_nonce != a->nonce) return;     /* a deal for an older me */
        /* The START (reliable) can arrive BEFORE the host's HELLO (fast):
         * whoever the host is comes from here, or a HELLO that shows up
         * later looks like the host re-entering the app. */
        a->their_nonce = m->host_nonce;
        ns_init(a->g, NS_MODE_COMBAT, m->seed, 2, MULTI_SNAKES, MULTI_COLS, MULTI_ROWS, MULTI_FRUITS, NULL);
        a->me = 1;
        a->role_known = true;
        a->started = true;
        msg_ready_t r = { MSG_READY, PROTO, m->host_nonce };
        aos_hal_link_send_reliable(&r, sizeof r);
        start_play(a, PLAY_MULTI);
        break;
    }
    case MSG_READY: {
        if (!reliable || !a->is_host || a->scr != SCR_LOBBY || len < (int)sizeof(msg_ready_t)) return;
        const msg_ready_t *r = (const msg_ready_t *)d;
        if (r->host_nonce != a->nonce) return;
        a->me = 0;
        a->started = true;
        start_play(a, PLAY_MULTI);
        break;
    }
    case MSG_STEP: {
        if (!reliable || a->is_host || a->scr != SCR_PLAY || len < (int)sizeof(msg_step_t)) return;
        msg_step_t m;
        memcpy(&m, d, sizeof m);
        guest_apply(a, &m);
        break;
    }
    case MSG_TURN:
        if (!reliable || !a->is_host || len < (int)sizeof(msg_turn_t)) return;
        if (a->guest_nq < 4) a->guest_q[a->guest_nq++] = ((const msg_turn_t *)d)->dir;
        break;
    case MSG_BYE:
        gone(a, _("%s salió del juego"));
        break;
    }
}

static void link_tick(app_t *a)
{
    if (!a->link_up) return;
    aos_link_frame_t f;
    while (aos_hal_link_recv_reliable(&f) > 0) handle_frame(a, f.data, f.len, true);
    while (aos_hal_link_recv(&f) > 0) handle_frame(a, f.data, f.len, false);
    if (aos_hal_link_reliable_lost()) {
        aos_hal_link_reliable_reset();
        if (a->scr == SCR_PLAY && a->play == PLAY_MULTI) gone(a, _("Sin señal de %s"));
    }

    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    aos_link_partner_t p;
    if (aos_hal_link_partner(&p) && p.valid) {
        a->have_partner = true;
        /* the partner's MAC from Link, not from a HELLO that may never have
         * been read: the beacon check below compares against it */
        memcpy(a->their_mac, p.mac, 6);
        snprintf(a->their_name, sizeof a->their_name, "%s", p.name);
    }

    if (a->scr == SCR_LOBBY) {
        if (!a->have_partner) {
            if (now - a->lobby_ms > 2000) set_msg(a, _("Sin pareja: apareá los equipos en Enlace"));
            return;
        }
        if (now - a->hello_ms > 400) {
            a->hello_ms = now;
            send_hello(a);
        }
        char buf[96];
        snprintf(buf, sizeof buf, _("Esperando a %s..."), a->their_name);
        set_msg(a, buf);
        return;
    }

    if (a->scr == SCR_PLAY && a->play == PLAY_MULTI && a->ov != OV_GONE) {
        /* the other device left the app: its beacon stops offering us */
        aos_link_neighbour_t nb[AOS_LINK_NEIGHBOURS];
        int n = aos_hal_link_neighbours(nb, AOS_LINK_NEIGHBOURS);
        bool offering = false;
        for (int i = 0; i < n; i++) {
            if (memcmp(nb[i].mac, a->their_mac, 6) == 0 && strcmp(nb[i].app, LINK_APP) == 0) offering = true;
        }
        if (offering) a->away_ms = now;
        else if (now - a->away_ms > 6000) gone(a, _("%s salió del juego"));
        if (!a->is_host && now - a->last_rx_ms > 4000) gone(a, _("Sin señal de %s"));
    }
}

static void host_step(app_t *a)
{
    if (aos_hal_link_reliable_pending() > 8) return;     /* let the air catch up */
    uint8_t dirs[NS_MAX_SNAKES];
    memset(dirs, NS_NODIR, sizeof dirs);
    dirs[0] = a->autoplay ? auto_choice(a) : pop_turn(a);
    if (a->guest_nq) {
        dirs[1] = a->guest_q[0];
        memmove(a->guest_q, a->guest_q + 1, --a->guest_nq);
    }
    ns_step(a->g, dirs);
    msg_step_t m = { .type = MSG_STEP, .proto = PROTO, .step = a->g->step_no,
                     .d0 = dirs[0], .d1 = dirs[1], .hash = ns_hash(a->g) };
    aos_hal_link_send_reliable(&m, sizeof m);
    after_step(a);
}

/* Guest: a STEP from the host, applied as soon as it arrives. There is no
 * inbox to overflow: if this device falls behind, the link's own receive
 * ring fills, stops acknowledging, and the host - which will not step with
 * more than eight frames unacknowledged - waits for it. */
static void guest_apply(app_t *a, const msg_step_t *m)
{
    if (m->step != a->g->step_no + 1) {
        aos_hal_log("neon", "step %u arrived, expected %u", (unsigned)m->step,
                    (unsigned)(a->g->step_no + 1));
        return;
    }
    uint8_t dirs[NS_MAX_SNAKES];
    memset(dirs, NS_NODIR, sizeof dirs);
    dirs[0] = m->d0;
    dirs[1] = m->d1;
    ns_step(a->g, dirs);
    /* my own turns only count once the host has decided them; one the host
     * applied, or one that no longer makes sense (already the heading, or a
     * reversal), leaves the queue, or it would block every turn after it */
    if (m->d1 != NS_NODIR && a->nq) pop_turn(a);
    const ns_snake_t *me = &a->g->s[a->me];
    while (a->nq && (a->q[0] == me->dir || a->q[0] == ((me->dir + 2) & 3))) pop_turn(a);
    if (ns_hash(a->g) != m->hash && !a->desync_told) {
        a->desync_told = true;
        aos_hal_log("neon", "DESYNC at step %u", (unsigned)m->step);
    }
    after_step(a);
}

/* NS_AUTO on the guest: the bot's choice goes to the host as a turn, like a
 * finger would. */
static void guest_auto(app_t *a)
{
    if (a->autoplay && a->nq == 0) {
        uint8_t d = auto_choice(a);
        if (d != NS_NODIR && d != a->g->s[a->me].dir) queue_turn(a, d);
    }
}

/* -------------------------------------------------------------------------- */
/* The timer                                                                   */

static void resume(app_t *a)
{
    show_overlay(a, OV_NONE);
    a->last_step_ms = (uint32_t)aos_hal_uptime_ms();
    ns_view_full(a->view, a->g);    /* the buttons leave their glow */
    push_view(a);
}

static void on_button(app_t *a, int i)
{
    if (a->scr == SCR_PLAY && a->ov == OV_PAUSE) {
        if (i == 0) {
            resume(a);
        } else {
            link_down(a);
            show_screen(a, SCR_MENU);
        }
        return;
    }
    if (a->scr == SCR_PLAY && a->ov == OV_OVER) {
        if (i == 0) start_play(a, PLAY_NORMAL);
        else show_screen(a, SCR_MENU);
        return;
    }
    if (a->scr == SCR_PLAY && a->ov == OV_GONE) {
        link_down(a);
        show_screen(a, SCR_MENU);
        return;
    }
    switch (a->scr) {
    case SCR_MENU:
        if (i == 0) start_play(a, PLAY_NORMAL);
        else if (i == 1) show_screen(a, SCR_COMBAT);
        break;
    case SCR_COMBAT:
        if (i == 0) start_play(a, PLAY_SOLO);
        else if (i == 1) {
            if (a->link_failed) set_msg(a, _("El juego de a dos necesita el enlace, y este equipo todavía no lo tiene"));
            else enter_lobby(a);
        } else show_screen(a, SCR_MENU);
        break;
    case SCR_LOBBY:
        link_down(a);
        show_screen(a, SCR_COMBAT);
        break;
    default:
        break;
    }
}

static void go_back(app_t *a)
{
    if (a->scr == SCR_PLAY) {
        if (a->ov == OV_NONE) show_overlay(a, OV_PAUSE);
        else on_button(a, 1);           /* from any overlay: to the menu */
    } else if (a->scr == SCR_LOBBY) {
        on_button(a, 2);
    } else if (a->scr == SCR_COMBAT) {
        show_screen(a, SCR_MENU);
    } else {
        a->want_exit = true;
    }
}

/* A swipe to the right on a menu is "back", the watch's habit; in play the
 * swipes are the snake's, and the runtime's record of them is dropped. */
static void take_gesture(app_t *a)
{
    int dir = aos_ui_take_gesture();
    if (dir == LV_DIR_RIGHT && a->scr != SCR_PLAY) go_back(a);
}

static void frame(lv_timer_t *timer)
{
    app_t *a = lv_timer_get_user_data(timer);
    if (a->want_exit) {
        a->want_exit = false;
        a->leaving = true;
        aos_ui_back();                  /* destroys the app: nothing after */
        return;
    }
    touch_poll(a);
    if (a->want_btn >= 0) {
        int i = a->want_btn;
        a->want_btn = -1;
        on_button(a, i);
    }
    if (a->want_pause || a->want_back) {
        a->want_pause = a->want_back = false;
        go_back(a);
    }
    take_gesture(a);
    link_tick(a);

    if (a->scr != SCR_PLAY) return;
    ns_game_t *g = a->g;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();

    bool running = a->ov == OV_NONE || (a->play == PLAY_MULTI && a->ov == OV_PAUSE);
    if (running && !g->over) {
        if (a->play == PLAY_MULTI) {
            if (!a->is_host) {
                guest_auto(a);
            } else if (now - a->last_step_ms >= step_ms(a)) {
                a->last_step_ms += step_ms(a);
                if (now - a->last_step_ms > 500) a->last_step_ms = now;
                host_step(a);
            }
        } else if (now - a->last_step_ms >= step_ms(a)) {
            a->last_step_ms += step_ms(a);
            if (now - a->last_step_ms > 500) a->last_step_ms = now;
            local_step(a);
        }
    }
    if (g->over && a->ov == OV_NONE) {
        if (!a->over_at_ms) a->over_at_ms = now;
        if (now - a->over_at_ms > 700) {
            if (a->autoplay) start_play(a, PLAY_NORMAL);
            else show_overlay(a, OV_OVER);
        }
    }

    ns_view_frame(a->view, g, (uint8_t)((now / 150) & 3));
    push_view(a);

    a->stat_frames++;
    if (now - a->stat_t0 >= 5000) {
        aos_hal_log("neon", "%u fps, %u %% of the screen per frame%s",
                    (unsigned)(a->stat_frames * 1000 / (now - a->stat_t0)),
                    (unsigned)(a->stat_frames ? a->stat_px / a->stat_frames * 100 / ((uint32_t)a->L.w * a->L.h) : 0),
                    a->play == PLAY_MULTI ? (a->is_host ? " (host)" : " (guest)") : "");
        a->stat_t0 = now;
        a->stat_frames = a->stat_px = 0;
    }
}

/* -------------------------------------------------------------------------- */
/* The view: everything that hangs from the root, for one size of it          */

static void view_build(app_t *a)
{
    lv_obj_t *root = a->root;
    layout(a);
    const int W = a->L.w, H = a->L.h;

    a->canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(a->canvas, a->fb, W, H, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(a->canvas, W, H);
    lv_obj_set_pos(a->canvas, 0, 0);
    lv_image_set_antialias(a->canvas, false);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a->canvas, LV_OBJ_FLAG_SCROLLABLE);

    a->pause = make_ctrl(a, a->L.pause, AOS_SYM_PAUSE, &aos_sym_44, VIOLET);

    a->msg = lv_label_create(root);
    const menu_geo_t m = menu_geo(a);
    lv_obj_set_width(a->msg, m.text_w);
    lv_label_set_long_mode(a->msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(a->msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(a->msg, aos_font_body, 0);
    lv_obj_set_style_text_color(a->msg, lv_color_hex(0xC8C8FF), 0);
    /* over a game it sits on the snakes: a dark plate keeps it legible */
    lv_obj_set_style_bg_color(a->msg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->msg, LV_OPA_80, 0);
    lv_obj_set_style_radius(a->msg, 14, 0);
    lv_obj_set_style_pad_ver(a->msg, 8, 0);
    lv_obj_set_style_pad_hor(a->msg, 12, 0);
    place_text(a, a->msg, m.btn_cx, m.text_y);
    lv_obj_remove_flag(a->msg, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(a->msg, "");
    lv_obj_add_flag(a->msg, LV_OBJ_FLAG_HIDDEN);

    /* how the chosen control works, under the menu's buttons */
    a->help = lv_label_create(root);
    lv_obj_set_width(a->help, m.text_w);
    lv_label_set_long_mode(a->help, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(a->help, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(a->help, a->L.tall ? aos_font_body : aos_font_small, 0);
    lv_obj_set_style_text_color(a->help, lv_color_hex(0x9A9AC8), 0);
    /* the main menu has two buttons: the text takes the third's place */
    place_text(a, a->help, m.btn_cx, m.btn_y0 + 2 * m.btn_step + 10);
    lv_obj_remove_flag(a->help, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(a->help, "");
    lv_obj_add_flag(a->help, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 3; i++) a->btn[i] = make_btn(a, i);
    a->menu_ok = false;                 /* the title is laid out per size */
}

/* After a turn of the screen: the same screen, over a canvas laid out anew.
 * A game keeps its arena, drawn at whatever cell fits the new field (the
 * next game gets a field of its own), and comes back paused so the new
 * layout is seen before it goes on - except the two-device match, which
 * never stops for one side. */
static void view_restore(app_t *a)
{
    screen_t s = a->scr;
    overlay_t ov = a->ov;
    if (s != SCR_PLAY) {
        show_screen(a, s);
        if (s == SCR_LOBBY) set_msg(a, _("Buscando al otro equipo..."));
        return;
    }
    bool combat = a->g->mode == NS_MODE_COMBAT;
    if (!ensure_art(a, combat, play_cell(a))) {
        aos_ui_toast(_("Sin memoria"), 2000);
        link_down(a);
        show_screen(a, SCR_MENU);
        return;
    }
    /* the pause pill moved: what it covers now is what stays clear (never
     * in lockstep, where the other engine could not know) */
    if (a->play != PLAY_MULTI) ns_set_cover(a->g, cover_for(a, a->g->cols, a->g->rows, play_cell(a)));
    show_screen(a, SCR_PLAY);
    begin_view(a, 0);
    if (ov == OV_NONE && a->play != PLAY_MULTI) ov = OV_PAUSE;
    if (ov != OV_NONE) show_overlay(a, ov);
    touch_sync(a);
}

/* Everything hanging from the root goes and is made again for its size;
 * 'closing' keeps the events LVGL sends while deleting
 * away from the handlers. */
static void view_rebuild(app_t *a)
{
    a->closing = true;
    lv_obj_clean(a->root);
    a->closing = false;
    a->pause = NULL;
    view_build(a);
    view_restore(a);
}

static bool app_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = inst;
    if (!a || root != a->root || a->leaving) return false;
    lv_obj_update_layout(root);
    if ((size_t)lv_obj_get_width(root) * (size_t)lv_obj_get_height(root) > a->npx) {
        return false;                   /* bigger than the buffers: start over */
    }
    view_rebuild(a);
    aos_hal_log("neon", "turned | %dx%d, field %dx%d", a->L.w, a->L.h, a->L.field.w, a->L.field.h);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */

static bool app_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = inst;
    if (!a || a->leaving) return false;
    if (a->scr == SCR_MENU) return false;
    a->want_back = true;
    return true;
}

static void app_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = inst;
    if (a && a->scr == SCR_PLAY && a->ov == OV_NONE && a->play != PLAY_MULTI) {
        show_overlay(a, OV_PAUSE);
    }
}

static void app_free(app_t *a)
{
    if (a->have_normal) ns_art_free(&a->art_normal);
    if (a->have_combat) ns_art_free(&a->art_combat);
    free(a->fb);
    free(a->bg);
    free(a->menu);
    free(a->g);
    free(a->view);
    lv_free(a);
}

static void *app_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    app_t *a = lv_malloc_zeroed(sizeof *a);
    if (!a) return NULL;
    a->want_btn = -1;
    a->root = root;

    /* The buffers hold the whole screen either way up (720x1280 = 1280x720):
     * 1.8 MB each, from malloc(), which sends them to PSRAM. */
    lv_obj_update_layout(root);
    size_t scr = (size_t)aos_hal_screen_w() * (size_t)aos_hal_screen_h();
    size_t rt = (size_t)lv_obj_get_width(root) * (size_t)lv_obj_get_height(root);
    a->npx = scr > rt ? scr : rt;
    a->fb = malloc(a->npx * 2);
    a->bg = malloc(a->npx * 2);
    a->g  = calloc(1, sizeof(ns_game_t));
    a->view = calloc(1, sizeof(ns_view_t));
    if (!a->fb || !a->bg || !a->g || !a->view) {
        aos_hal_log("neon", "out of memory for the buffers");
        app_free(a);
        return NULL;
    }
    memset(a->fb, 0, a->npx * 2);
    memset(a->bg, 0, a->npx * 2);

    /* An older build kept a choice of control in "neon_ctrl"; swiping is
     * the only one now, so a saved value is simply not read. */

    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

#ifdef AOS_SIM_BUILTIN
    /* Development switches. They only exist in the simulator.
     *   NS_AUTO=1                   the bot steers my snake too
     *   NS_MODE=normal|solo|multi|combat   straight into a game or a screen
     *   NS_LINK=0                   aos_hal_link_start() fails, as on the P4 */
    const char *env;
    if ((env = getenv("NS_AUTO")) && env[0] == '1') a->autoplay = true;
    if ((env = getenv("NS_LINK")) && env[0] == '0') a->no_link = true;
#endif

    view_build(a);
    a->stat_t0 = (uint32_t)aos_hal_uptime_ms();
    show_screen(a, SCR_MENU);
    touch_sync(a);

#ifdef AOS_SIM_BUILTIN
    if ((env = getenv("NS_MODE")) && env[0]) {
        if (strcmp(env, "normal") == 0) start_play(a, PLAY_NORMAL);
        else if (strcmp(env, "solo") == 0) start_play(a, PLAY_SOLO);
        else if (strcmp(env, "multi") == 0) enter_lobby(a);
        else if (strcmp(env, "combat") == 0) show_screen(a, SCR_COMBAT);
    }
#endif

    a->timer = lv_timer_create(frame, FRAME_MS, a);
    uint32_t heap_int = 0, heap_psram = 0;
    aos_hal_heap_info(&heap_int, &heap_psram);
    aos_hal_log("neon", "ready | %dx%d, field %dx%d | internal %u B, psram %u B", a->L.w, a->L.h,
                a->L.field.w, a->L.field.h, (unsigned)heap_int, (unsigned)heap_psram);
    return a;
}

static void app_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = inst;
    if (!a) return;
    if (a->timer) lv_timer_delete(a->timer);
    a->closing = true;
    link_down(a);
    /* The objects go HERE and not with the runtime: it calls destroy() and
     * only then deletes the root, so the events LVGL sends on deleting would
     * reach callbacks with the context already freed. */
    if (a->root) lv_obj_clean(a->root);
    app_free(a);
}

/* The launcher icon: an S of neon snake with its head and a cherry, on a
 * night-blue circle. Percent coordinates. */
static const uint8_t NEON_ICON[] = {
    AIC_HEADER,
    /* the body: two bowls that make an S, from the tail (lower left) up to
     * the head (upper right); arc angles clockwise from three o'clock */
    AIC_ARC(AIC_CENTER, 0, 16, 34, 0, 9, 0, 0, 270, 135, 0,
            AIC_C_BG, 0, AIC_C_LIT(0x19F5FF), 255),
    AIC_ARC(AIC_CENTER, 0, -16, 34, 0, 9, 0, 0, 90, 315, 0,
            AIC_C_BG, 0, AIC_C_LIT(0x19F5FF), 255),
    /* the head at the top end, with an eye and the tongue */
    AIC_RECT(AIC_CENTER, 13, -28, 17, 17, AIC_CIRCLE, AIC_C_LIT(0x19F5FF), 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, 2, -2, 5, 5, AIC_CIRCLE, AIC_C_BG, 255),
    AIC_OUT,
    AIC_RECT(AIC_CENTER, 25, -28, 8, 2, 1, AIC_C_LIT(0xFF3060), 255),
    /* a cherry on the left, where the S leaves room */
    AIC_RECT(AIC_CENTER, -21, 4, 3, 13, 1, AIC_C_LIT(0x8CFF2E), 255),
    AIC_RECT(AIC_CENTER, -24, 12, 15, 15, AIC_CIRCLE, AIC_C_LIT(0xFF2FD0), 255),
    AIC_END
};

static bool neon_init(aos_app_t *app)
{
    app->desc.id       = "demo.neon";
    app->desc.name     = "Neon Snakes";
    app->desc.icon     = LV_SYMBOL_SHUFFLE;     /* the fallback, if ever refused */
    app->desc.icon_vec = AOS_ICON_NONE;
    aos_icon_set_ops(app, NEON_ICON, sizeof NEON_ICON);
    app->desc.color_a  = 0x1A1040;
    app->desc.color_b  = 0x05030F;
    app->desc.order    = 157;
    /* No orientation flag: portrait is home, landscape lays the arena and the
     * menus out lying down, and resize() keeps the game. */
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN |
                         AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    app->create  = app_create;
    app->destroy = app_destroy;
    app->back    = app_back;
    app->hide    = app_hide;
    app->resize  = app_resize;
    return true;
}

AOS_APP_ENTRY(neon_init);
