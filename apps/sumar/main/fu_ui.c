/*
 * P4OS - Fusión: the screens, the animation and the input
 *
 * Two screens, the menu and the game, and panels over the game (pause, the
 * end, the achievements). The board is a container with one rounded slot per
 * cell and a pool of LVGL images, one per tile: a move hands each image where
 * it goes, and the frame timer (16 ms) slides them there, then bounces the
 * merged ones and grows the new one. Everything runs on the clock, not on
 * frame counts, so a slow frame makes a step longer, not the move slower.
 *
 * A swipe given while a move is still animating finishes that one at once
 * and plays: fast hands never wait for the animation.
 *
 * The geometry: the board is the same size upright and lying down (the short
 * side minus a margin), so the tiles' sprites survive turning the screen.
 * Upright the HUD is above the board; lying down it is a column at its right.
 */
#include "fu_app.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define T_SLIDE     105     /* ms */
#define T_POP       170
#define T_PLUS      750
#define T_BANNER    1500
#define SWIPE_PX    34
#define MARGIN      24
#define NVIEWS      (FU_CELLS * 2)

/* FUSION_SLOW=8 in the simulator stretches the animation, to look at it */
static int s_slow = 1;
#define TS          (T_SLIDE * s_slow)
#define TP          (T_POP * s_slow)

enum { SCR_MENU, SCR_GAME };
enum { PANEL_NONE, PANEL_PAUSE, PANEL_OVER, PANEL_ACH, PANEL_HELP };
enum { POP_NONE, POP_MERGE, POP_SPAWN };

typedef struct {
    lv_obj_t *img;
    uint16_t  id;
    bool      used, gone, moving;
    uint8_t   exp, exp_after;
    int16_t   x0, y0, x1, y1;
    uint8_t   pop;
} view_t;

static struct {
    aos_app_t *self;
    lv_obj_t  *root;
    int        W, H;
    bool       land;
    int        scr, panel;

    /* the board */
    int        bx, by, bpx, inner, cell;
    lv_obj_t  *game, *board, *touch;
    view_t     v[NVIEWS];

    /* the HUD */
    lv_obj_t  *card[3], *card_t[3], *card_v[3], *goal_img;
    lv_obj_t  *btn_pause, *btn_undo, *lb_undo, *lb_info, *lb_hint;
    lv_obj_t  *bar, *bar_fill;
    lv_obj_t  *plus, *banner, *banner_sub;
    uint32_t   plus_t0, banner_t0;
    int        hx, hy, hw, hh;

    /* the menu */
    lv_obj_t  *menu;
    lv_obj_t  *chip_n[4], *chip_m[4], *chip_t[4], *lb_mode_desc, *lb_rec;
    lv_obj_t  *btn_play, *btn_resume, *btn_ach, *btn_snd, *btn_exit;
    lv_obj_t  *panel_obj;

    /* input */
    aos_pad_t      pad;
    aos_pad_menu_t pm;
    uint32_t       kb_bits;
    bool           pressing, swiped;
    lv_point_t     p0;

    /* animation */
    bool       animating;
    int        phase;               /* 0 sliding, 1 popping */
    uint32_t   t0;
    uint32_t   over_at;             /* when to show the end panel, 0 = no */

    lv_timer_t *timer;
    bool       hidden, want_exit;
    uint32_t   last_ms;
    int32_t    last_sec;

    /* fps, logged every few seconds while a game shows */
    uint32_t   fps_t0;
    int        fps_n, fps_anim;
} U;

static void build(void);
static void panel_close(void);
static void panel_over(void);
static void menu_refresh(void);
static void hud_refresh(void);
static void go_menu(void);

/* ------------------------------------------------------------------ helpers */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *f, uint32_t color, const char *txt)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, txt);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, int w, int h, uint32_t bg, uint32_t fg,
                        lv_event_cb_t cb, intptr_t arg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2 > 36 ? 36 : h / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), LV_STATE_PRESSED);
    lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    if (txt) {
        lv_obj_t *l = label(b, aos_font_body, fg, txt);
        lv_obj_center(l);
    }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)arg);
    return b;
}

static uint32_t mixhex(uint32_t a, uint32_t b, int f256)
{
    int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255;
    int br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255;
    return ((uint32_t)(ar + (br - ar) * f256 / 256) << 16) | ((uint32_t)(ag + (bg - ag) * f256 / 256) << 8) |
           (uint32_t)(ab + (bb - ab) * f256 / 256);
}

/* text on an accent colour: dark on light ones */
static uint32_t on_color(uint32_t c)
{
    int l = (int)(((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000;
    return l > 150 ? 0x10131C : 0xFFFFFF;
}

static const char *mode_name(int m)
{
    static const char *const N[FU_MODES] = { N_("Clásico"), N_("Contra reloj"), N_("Bloques"), N_("De a dos") };
    return _(N[m >= 0 && m < FU_MODES ? m : 0]);
}

static const char *mode_desc(int m)
{
    static const char *const D[FU_MODES] = {
        N_("Juntá fichas iguales hasta la meta. Cada meta cumplida da otra más alta y un deshacer más."),
        N_("Dos minutos para sumar todo lo que puedas. Cada meta da 20 segundos; deshacer cuesta 10."),
        N_("Piedras fijas que no se mueven y cortan las filas. Las fichas frenan contra ellas."),
        N_("Una jugada cada uno, pasándose la placa. Gana quien llega a la meta, o quien sume más."),
    };
    return _(D[m >= 0 && m < FU_MODES ? m : 0]);
}

static void fmt_num(char *out, size_t len, uint32_t v)
{
    /* thin groups with a narrow no-break space would be nicer, but the fonts
     * have the plain no-break space only: use it */
    char raw[16];
    snprintf(raw, sizeof raw, "%lu", (unsigned long)v);
    int n = (int)strlen(raw), o = 0;
    for (int i = 0; i < n && o < (int)len - 3; i++) {
        if (i > 0 && (n - i) % 3 == 0) {
            out[o++] = (char)0xC2;
            out[o++] = (char)0xA0;
        }
        out[o++] = raw[i];
    }
    out[o] = 0;
}

/* --------------------------------------------------------------- geometry */

static void cell_xy(int r, int c, int *x, int *y)
{
    *x = U.inner + c * U.cell;
    *y = U.inner + r * U.cell;
}

static void layout(void)
{
    U.W = (int)lv_obj_get_width(U.root);
    U.H = (int)lv_obj_get_height(U.root);
    U.land = U.W > U.H;
    int shortside = U.land ? U.H : U.W;
    U.bpx = shortside - 2 * MARGIN;
    int n = fu_G.active ? fu_G.n : fu_P.n;
    U.inner = U.bpx / 48;
    U.cell = (U.bpx - 2 * U.inner) / n;
    U.inner = (U.bpx - U.cell * n) / 2;
    if (U.land) {
        U.bx = MARGIN + 8;
        U.by = (U.H - U.bpx) / 2;
        U.hx = U.bx + U.bpx + 44;
        U.hy = MARGIN;
        U.hw = U.W - U.hx - MARGIN - 8;
        U.hh = U.H - 2 * MARGIN;
    } else {
        U.bx = MARGIN;
        U.by = U.H - U.bpx - 150;
        U.hx = MARGIN;
        U.hy = MARGIN + 8;
        U.hw = U.bpx;
        U.hh = U.by - U.hy - 24;
    }
}

/* ------------------------------------------------------------------ views */

static view_t *view_by_id(uint16_t id)
{
    for (int i = 0; i < NVIEWS; i++)
        if (U.v[i].used && U.v[i].id == id) return &U.v[i];
    return NULL;
}

static view_t *view_alloc(uint16_t id, uint8_t exp, int r, int c)
{
    for (int i = 0; i < NVIEWS; i++) {
        view_t *v = &U.v[i];
        if (v->used) continue;
        v->used = true;
        v->id = id;
        v->exp = v->exp_after = exp;
        v->gone = v->moving = false;
        v->pop = POP_NONE;
        int x, y;
        cell_xy(r, c, &x, &y);
        v->x0 = v->x1 = (int16_t)x;
        v->y0 = v->y1 = (int16_t)y;
        lv_image_set_src(v->img, fu_art_tile(exp));
        lv_image_set_scale(v->img, 256);
        lv_obj_set_pos(v->img, x, y);
        lv_obj_remove_flag(v->img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(v->img);
        return v;
    }
    return NULL;
}

static void view_free(view_t *v)
{
    v->used = false;
    v->id = 0;
    lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
}

/* Every view from the grid as it is, with no animation. */
static void views_sync(void)
{
    U.animating = false;
    for (int i = 0; i < NVIEWS; i++)
        if (U.v[i].img) view_free(&U.v[i]);
    if (!fu_G.active) return;
    for (int r = 0; r < fu_G.n; r++)
        for (int c = 0; c < fu_G.n; c++)
            if (fu_G.g.v[r][c]) view_alloc(fu_G.g.id[r][c], fu_G.g.v[r][c], r, c);
}

static float ease_out(float p)
{
    float q = 1 - p;
    return 1 - q * q * q;
}

static float ease_back(float p)
{
    const float c1 = 2.0f, c3 = c1 + 1;
    float q = p - 1;
    return 1 + c3 * q * q * q + c1 * q * q;
}

/* The end of the slide: the swallowed tiles go, the merged ones change. */
static void slide_end(void)
{
    for (int i = 0; i < NVIEWS; i++) {
        view_t *v = &U.v[i];
        if (!v->used) continue;
        if (v->moving) {
            v->x0 = v->x1;
            v->y0 = v->y1;
            v->moving = false;
            lv_obj_set_pos(v->img, v->x1, v->y1);
        }
        if (v->gone) {
            view_free(v);
            continue;
        }
        if (v->exp_after != v->exp) {
            v->exp = v->exp_after;
            lv_image_set_src(v->img, fu_art_tile(v->exp));
        }
        if (v->pop == POP_SPAWN) lv_obj_remove_flag(v->img, LV_OBJ_FLAG_HIDDEN);
        if (v->pop != POP_NONE) lv_image_set_scale(v->img, v->pop == POP_SPAWN ? 0 : 256);
    }
}

static void pop_end(void)
{
    for (int i = 0; i < NVIEWS; i++) {
        view_t *v = &U.v[i];
        if (!v->used) continue;
        if (v->pop != POP_NONE) lv_image_set_scale(v->img, 256);
        v->pop = POP_NONE;
    }
    U.animating = false;
}

static void anim_finish(void)
{
    if (!U.animating) return;
    if (U.phase == 0) slide_end();
    pop_end();
}

static void anim_step(uint32_t now)
{
    if (!U.animating) return;
    uint32_t e = now - U.t0;
    if (U.phase == 0) {
        float p = (float)e / TS;
        if (p >= 1) {
            slide_end();
            U.phase = 1;
            U.t0 = now;
            return;
        }
        float k = ease_out(p);
        for (int i = 0; i < NVIEWS; i++) {
            view_t *v = &U.v[i];
            if (!v->used || !v->moving) continue;
            lv_obj_set_pos(v->img, v->x0 + (int)((float)(v->x1 - v->x0) * k),
                           v->y0 + (int)((float)(v->y1 - v->y0) * k));
        }
        return;
    }
    float p = (float)e / TP;
    if (p >= 1) {
        pop_end();
        return;
    }
    for (int i = 0; i < NVIEWS; i++) {
        view_t *v = &U.v[i];
        if (!v->used || v->pop == POP_NONE) continue;
        float s;
        if (v->pop == POP_MERGE) s = 1 + 0.20f * sinf(3.14159f * p) * (1 - 0.3f * p);
        else s = ease_back(p);
        if (s < 0) s = 0;
        lv_image_set_scale(v->img, (uint32_t)(s * 256));
    }
}

/* ------------------------------------------------------------------- game */

static void ach_unlock(int i)
{
    if (fu_P.ach & (1u << i)) return;
    fu_P.ach |= 1u << i;
    fu_prefs_save();
    char msg[96];
    snprintf(msg, sizeof msg, _("Logro: %s"), _(fu_ach_title(i)));
    aos_ui_toast(msg, 2200);
}

static void banner_show(const char *big, const char *small)
{
    if (!U.banner) return;
    lv_label_set_text(U.banner, big);
    lv_label_set_text(U.banner_sub, small ? small : "");
    lv_obj_remove_flag(U.banner, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(U.banner_sub, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(U.banner);
    lv_obj_move_foreground(U.banner_sub);
    U.banner_t0 = lv_tick_get();
}

static void plus_show(uint32_t pts)
{
    if (!U.plus || !pts) return;
    lv_label_set_text_fmt(U.plus, "+%lu", (unsigned long)pts);
    U.plus_t0 = lv_tick_get();
    lv_obj_remove_flag(U.plus, LV_OBJ_FLAG_HIDDEN);
}

static void snapshot_push(void)
{
    if (fu_G.mode == FU_MODE_DUO) return;
    if (fu_G.nundo == FU_UNDO_DEPTH) {
        memmove(&fu_G.undo[0], &fu_G.undo[1], sizeof(fu_snap_t) * (FU_UNDO_DEPTH - 1));
        fu_G.nundo--;
    }
    fu_snap_t *s = &fu_G.undo[fu_G.nundo++];
    s->g = fu_G.g;
    s->score = fu_G.score;
    s->score2 = fu_G.score2;
    s->turn = fu_G.turn;
    s->moves = fu_G.moves;
}

static bool can_undo(void)
{
    if (!fu_G.active || fu_G.over || !fu_G.nundo || fu_G.mode == FU_MODE_DUO) return false;
    if (fu_G.mode == FU_MODE_TIME) return fu_G.time_ms > 10000;
    return fu_G.undo_left > 0;
}

static void do_undo(void)
{
    if (U.scr != SCR_GAME || U.panel != PANEL_NONE) return;
    if (!can_undo()) {
        fu_snd_play(FU_S_DENY, 0);
        return;
    }
    anim_finish();
    fu_snap_t *s = &fu_G.undo[--fu_G.nundo];
    fu_G.g = s->g;
    fu_G.score = s->score;
    fu_G.score2 = s->score2;
    fu_G.turn = s->turn;
    fu_G.moves = s->moves;
    if (fu_G.mode == FU_MODE_TIME) fu_G.time_ms -= 10000;
    else fu_G.undo_left--;
    fu_G.used_undo = true;
    views_sync();
    fu_snd_play(FU_S_UNDO, 0);
    hud_refresh();
}

static void game_end(void)
{
    fu_G.over = true;
    uint8_t top = (uint8_t)fu_core_top(&fu_G.g, fu_G.n);
    uint32_t sc = fu_G.score;
    if (fu_G.mode == FU_MODE_DUO) {
        sc = fu_G.score > fu_G.score2 ? fu_G.score : fu_G.score2;
        ach_unlock(FU_ACH_DUO);
    }
    fu_record_offer(fu_G.n, fu_G.mode, sc, top);
    fu_game_forget();
    U.over_at = lv_tick_get() + 650;
    fu_snd_play(FU_S_OVER, 0);
}

static void goal_reached(void)
{
    char big[48], small[96], v[12];
    fu_value_text(fu_G.goal, v, sizeof v);
    snprintf(big, sizeof big, _("¡Meta %s!"), v);
    if (!fu_G.used_undo) ach_unlock(FU_ACH_CLEAN);
    fu_G.used_undo = false;
    fu_G.level++;
    fu_G.goal++;
    fu_value_text(fu_G.goal, v, sizeof v);
    if (fu_G.mode == FU_MODE_TIME) {
        fu_G.time_ms += 20000;
        snprintf(small, sizeof small, _("+20 segundos. Ahora, %s."), v);
    } else {
        if (fu_G.undo_left < 5) fu_G.undo_left++;
        snprintf(small, sizeof small, _("Ahora, %s. Un deshacer más."), v);
    }
    banner_show(big, small);
    fu_snd_play(FU_S_GOAL, 0);
}

static void check_achievements(const fu_move_t *m, int top)
{
    if (top >= 7) ach_unlock(FU_ACH_128);
    if (top >= 9) ach_unlock(FU_ACH_512);
    if (top >= 10) ach_unlock(FU_ACH_1024);
    if (top >= 12) ach_unlock(FU_ACH_4096);
    if (fu_G.n == 3 && top >= 8) ach_unlock(FU_ACH_POCKET);
    if (m->nmerges >= 3) ach_unlock(FU_ACH_TRIPLE);
    if (fu_G.mode == FU_MODE_TIME && fu_G.score >= 3000) ach_unlock(FU_ACH_CLOCK);
    if (fu_G.mode == FU_MODE_BLOCKS && top >= 9) ach_unlock(FU_ACH_STONES);
    if (fu_G.moves >= 500) ach_unlock(FU_ACH_MARATHON);
}

static void do_move(int dir)
{
    if (U.scr != SCR_GAME || U.panel != PANEL_NONE || !fu_G.active || fu_G.over) return;
    if (fu_G.mode == FU_MODE_TIME && fu_G.time_ms <= 0) return;
    anim_finish();

    fu_grid_t before = fu_G.g;
    fu_move_t m;
    if (!fu_core_move(&fu_G.g, fu_G.n, dir, &m)) {
        /* nothing moves that way: a little nudge of the board says so */
        fu_snd_play(FU_S_DENY, 0);
        return;
    }
    /* the snapshot is of the grid before the move */
    fu_grid_t after = fu_G.g;
    fu_G.g = before;
    snapshot_push();
    fu_G.g = after;

    fu_G.moves++;
    if (fu_G.mode == FU_MODE_DUO && fu_G.turn) fu_G.score2 += m.points;
    else fu_G.score += m.points;

    for (int i = 0; i < m.nslides; i++) {
        view_t *v = view_by_id(m.slide[i].id);
        if (!v) continue;
        int x, y;
        cell_xy(m.slide[i].r1, m.slide[i].c1, &x, &y);
        v->x1 = (int16_t)x;
        v->y1 = (int16_t)y;
        v->moving = true;
        v->gone = m.slide[i].gone;
    }
    for (int i = 0; i < m.nmerges; i++) {
        view_t *v = view_by_id(m.merge[i].id);
        if (!v) continue;
        v->exp_after = m.merge[i].exp;
        v->pop = POP_MERGE;
        lv_obj_move_foreground(v->img);
    }
    int sr, sc;
    if (fu_core_spawn(&fu_G.g, fu_G.n, &sr, &sc)) {
        view_t *v = view_alloc(fu_G.g.id[sr][sc], fu_G.g.v[sr][sc], sr, sc);
        if (v) {
            v->pop = POP_SPAWN;
            lv_obj_add_flag(v->img, LV_OBJ_FLAG_HIDDEN);
        }
    }
    U.animating = true;
    U.phase = 0;
    U.t0 = lv_tick_get();

    if (m.nmerges) fu_snd_play(FU_S_MERGE, m.top);
    else fu_snd_play(FU_S_SLIDE, 0);
    plus_show(m.points);

    int top = fu_core_top(&fu_G.g, fu_G.n);
    check_achievements(&m, top);

    if (fu_G.mode == FU_MODE_DUO) {
        if (top >= fu_G.goal) {
            fu_G.winner = fu_G.turn;
            game_end();
        } else if (!fu_core_can_move(&fu_G.g, fu_G.n)) {
            fu_G.winner = fu_G.score == fu_G.score2 ? 2 : (fu_G.score > fu_G.score2 ? 0 : 1);
            game_end();
        } else {
            fu_G.turn ^= 1;
        }
    } else {
        if (top >= fu_G.goal) goal_reached();
        if (!fu_core_can_move(&fu_G.g, fu_G.n)) game_end();
    }
    hud_refresh();
}

static void game_start(bool resume)
{
    if (!resume) {
        fu_game_new(fu_P.n, fu_P.mode);
        fu_P.themes_used |= (uint8_t)(1u << fu_P.theme);
        fu_P.sizes_played |= (uint8_t)(1u << (fu_P.n - FU_NMIN));
        fu_prefs_save();
        if ((fu_P.themes_used & 0x0F) == 0x0F) ach_unlock(FU_ACH_THEMES);
    }
    U.scr = SCR_GAME;
    U.panel = PANEL_NONE;
    U.over_at = 0;
    build();
}

/* ------------------------------------------------------------------- HUD */

static void card_set(int i, const char *title, const char *value, uint32_t border)
{
    if (!U.card[i]) return;
    const fu_theme_t *t = fu_theme();
    lv_label_set_text(U.card_t[i], title);
    if (value) {
        lv_label_set_text(U.card_v[i], value);
        int len = (int)strlen(value);
        lv_obj_set_style_text_font(U.card_v[i], len > 8 ? aos_font_body : len > 6 ? aos_font_title : aos_font_large, 0);
    }
    lv_obj_set_style_border_color(U.card[i], lv_color_hex(border ? border : t->card_line), 0);
    lv_obj_set_style_border_width(U.card[i], border ? 4 : 2, 0);
}

static void hud_refresh(void)
{
    if (U.scr != SCR_GAME || !U.card[0]) return;
    const fu_theme_t *t = fu_theme();
    char a[24], b[24], v[12];
    bool duo = fu_G.mode == FU_MODE_DUO;
    if (duo) {
        fmt_num(a, sizeof a, fu_G.score);
        fmt_num(b, sizeof b, fu_G.score2);
        bool on = !fu_G.over;
        card_set(0, _("Jugador 1"), a, on && fu_G.turn == 0 ? t->accent : 0);
        card_set(1, _("Jugador 2"), b, on && fu_G.turn == 1 ? t->accent2 : 0);
    } else {
        uint32_t rec = fu_record(fu_G.n, fu_G.mode);
        fmt_num(a, sizeof a, fu_G.score);
        fmt_num(b, sizeof b, rec > fu_G.score ? rec : fu_G.score);
        card_set(0, _("Puntos"), a, 0);
        card_set(1, _("Récord"), b, 0);
    }
    if (fu_G.mode == FU_MODE_TIME) {
        int32_t s = fu_G.time_ms > 0 ? (fu_G.time_ms + 999) / 1000 : 0;
        snprintf(a, sizeof a, "%ld:%02ld", (long)(s / 60), (long)(s % 60));
        card_set(2, _("Tiempo"), a, s <= 10 ? 0xFF453A : 0);
        lv_obj_add_flag(U.goal_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(U.card_v[2], LV_OBJ_FLAG_HIDDEN);
    } else {
        card_set(2, _("Meta"), NULL, 0);
        lv_image_set_src(U.goal_img, fu_art_tile(fu_G.goal));
        lv_obj_remove_flag(U.goal_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(U.card_v[2], LV_OBJ_FLAG_HIDDEN);
    }

    /* the line under the cards */
    char info[128];
    fu_value_text(fu_G.goal, v, sizeof v);
    if (duo) {
        snprintf(info, sizeof info, _("%d×%d · De a dos · gana quien llegue a %s"), fu_G.n, fu_G.n, v);
    } else if (fu_G.mode == FU_MODE_TIME) {
        snprintf(info, sizeof info, _("%d×%d · Contra reloj · meta %s"), fu_G.n, fu_G.n, v);
    } else {
        snprintf(info, sizeof info, _("%d×%d · %s · nivel %d"), fu_G.n, fu_G.n, mode_name(fu_G.mode), fu_G.level + 1);
    }
    lv_label_set_text(U.lb_info, info);

    /* undo: how many are left, or what it costs */
    if (duo) {
        lv_obj_add_flag(U.btn_undo, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(U.btn_undo, LV_OBJ_FLAG_HIDDEN);
        if (fu_G.mode == FU_MODE_TIME) snprintf(a, sizeof a, "%s", _("-10 s"));
        else snprintf(a, sizeof a, "%d", fu_G.undo_left);
        lv_label_set_text(U.lb_undo, a);
        lv_obj_set_style_text_font(U.lb_undo, fu_G.mode == FU_MODE_TIME ? aos_font_small : aos_font_body, 0);
        lv_obj_set_style_opa(U.btn_undo, can_undo() ? LV_OPA_COVER : LV_OPA_40, 0);
    }

    /* whose turn, in a game of two: the board's frame takes their colour */
    if (duo) {
        uint32_t col = fu_G.turn ? t->accent2 : t->accent;
        lv_obj_set_style_border_color(U.board, lv_color_hex(fu_G.over ? t->card_line : col), 0);
        lv_obj_set_style_border_width(U.board, 5, 0);
        if (!fu_G.over) {
            lv_label_set_text(U.lb_hint, fu_G.turn ? _("Turno del jugador 2") : _("Turno del jugador 1"));
            lv_obj_set_style_text_color(U.lb_hint, lv_color_hex(col), 0);
        }
    }
    if (U.bar) {
        /* 32 bits on purpose: an int64_t division is __divdi3, not in the table */
        int32_t left = fu_G.time_ms < 0 ? 0 : fu_G.time_ms > 120000 ? 120000 : fu_G.time_ms;
        int w = (int)((int32_t)U.hw * (left / 10) / 12000);
        lv_obj_set_width(U.bar_fill, w < 0 ? 0 : w);
        lv_obj_set_style_bg_color(U.bar_fill, lv_color_hex(fu_G.time_ms <= 10000 ? 0xFF453A : t->accent), 0);
    }
}

/* -------------------------------------------------------------- callbacks */

static void cb_pause(lv_event_t *e);
static void cb_undo(lv_event_t *e) { (void)e; do_undo(); }

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (code == LV_EVENT_PRESSED) {
        U.pressing = true;
        U.swiped = false;
        U.p0 = p;
        return;
    }
    if (!U.pressing) return;
    int dx = (int)(p.x - U.p0.x), dy = (int)(p.y - U.p0.y);
    int ax = dx < 0 ? -dx : dx, ay = dy < 0 ? -dy : dy;
    bool done = code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST;
    int need = done ? SWIPE_PX / 2 : SWIPE_PX;
    if (!U.swiped && (ax >= need || ay >= need)) {
        U.swiped = true;
        int dir = ax > ay ? (dx > 0 ? FU_RIGHT : FU_LEFT) : (dy > 0 ? FU_DOWN : FU_UP);
        do_move(dir);
    }
    if (done) U.pressing = false;
}

static void gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in || lv_indev_get_gesture_dir(in) != LV_DIR_RIGHT) return;
    if (U.scr != SCR_MENU || U.panel != PANEL_NONE) return;
    lv_indev_wait_release(in);
    U.want_exit = true;
}

static void cb_play(lv_event_t *e)   { (void)e; game_start(false); }
static void cb_resume(lv_event_t *e) { (void)e; game_start(true); }
static void cb_exit(lv_event_t *e)   { (void)e; U.want_exit = true; }

static void cb_size(lv_event_t *e)
{
    fu_P.n = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    fu_prefs_save();
    menu_refresh();
}

static void cb_mode(lv_event_t *e)
{
    fu_P.mode = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    fu_prefs_save();
    menu_refresh();
}

static void cb_theme(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    if (t == fu_P.theme) return;
    fu_P.theme = (uint8_t)t;
    fu_prefs_save();
    build();            /* every colour changes: the menu is built again */
}

static void cb_sound(lv_event_t *e)
{
    (void)e;
    fu_P.sound = !fu_P.sound;
    fu_prefs_save();
    if (fu_P.sound) fu_snd_open();
    else fu_snd_close();
    if (U.btn_snd) {
        lv_obj_t *l = lv_obj_get_child(U.btn_snd, 0);
        lv_label_set_text(l, fu_P.sound ? _("Sonido: sí") : _("Sonido: no"));
    }
}

/* ------------------------------------------------------------------ panels */

static lv_obj_t *panel_open(int kind, const char *title, int w)
{
    panel_close();
    const fu_theme_t *t = fu_theme();
    U.panel = kind;
    lv_obj_t *dim = box(U.root);
    lv_obj_set_size(dim, U.W, U.H);
    lv_obj_set_style_bg_color(dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    U.panel_obj = dim;

    lv_obj_t *card = lv_obj_create(dim);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, w);
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, U.H - 80, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(t->card), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 32, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(t->card_line), 0);
    lv_obj_set_style_pad_left(card, 32, 0);
    lv_obj_set_style_pad_right(card, 32, 0);
    lv_obj_set_style_pad_top(card, U.land ? 24 : 32, 0);
    lv_obj_set_style_pad_bottom(card, U.land ? 24 : 32, 0);
    lv_obj_set_style_pad_row(card, U.land ? 12 : 18, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(card);
    if (title) label(card, aos_font_title, t->text, title);
    return card;
}

static void panel_close(void)
{
    if (U.panel_obj) {
        lv_obj_delete(U.panel_obj);
        U.panel_obj = NULL;
    }
    U.panel = PANEL_NONE;
    aos_pad_menu_clear(&U.pm);
    if (U.scr == SCR_MENU) menu_refresh();
}

static void cb_panel_close(lv_event_t *e) { (void)e; panel_close(); }
static void cb_to_menu(lv_event_t *e)     { (void)e; fu_game_save(); go_menu(); }

static void cb_restart(lv_event_t *e)
{
    (void)e;
    if (fu_G.active && !fu_G.over) {
        uint8_t top = (uint8_t)fu_core_top(&fu_G.g, fu_G.n);
        if (fu_G.mode != FU_MODE_DUO) fu_record_offer(fu_G.n, fu_G.mode, fu_G.score, top);
    }
    fu_P.n = fu_G.n;
    fu_P.mode = fu_G.mode;
    game_start(false);
}

static void pause_open(void)
{
    if (U.scr != SCR_GAME || U.panel != PANEL_NONE || fu_G.over) return;
    anim_finish();
    const fu_theme_t *t = fu_theme();
    int w = U.W < 600 ? U.W - 60 : 520;
    lv_obj_t *c = panel_open(PANEL_PAUSE, _("Pausa"), w);
    lv_obj_t *items[4];
    items[0] = button(c, _("Seguir"), w - 64, 96, t->accent, on_color(t->accent), cb_panel_close, 0);
    items[1] = button(c, _("Empezar de nuevo"), w - 64, 96, t->card_line, t->text, cb_restart, 0);
    items[2] = button(c, _("Menú"), w - 64, 96, t->card_line, t->text, cb_to_menu, 0);
    items[3] = button(c, fu_P.sound ? _("Sonido: sí") : _("Sonido: no"), w - 64, 96, t->card_line, t->text, cb_sound, 0);
    U.btn_snd = items[3];
    aos_pad_menu_set(&U.pm, items, 4, 0);
}

static void cb_pause(lv_event_t *e) { (void)e; pause_open(); }

static void panel_over(void)
{
    const fu_theme_t *t = fu_theme();
    int w = U.W < 600 ? U.W - 60 : 560;
    char title[64], line[96], num[24], v[12];
    uint32_t col = t->text;
    if (fu_G.mode == FU_MODE_DUO) {
        if (fu_G.winner == 2) snprintf(title, sizeof title, "%s", _("¡Empate!"));
        else snprintf(title, sizeof title, _("¡Gana el jugador %d!"), fu_G.winner + 1);
        if (fu_G.winner < 2) col = fu_G.winner ? t->accent2 : t->accent;
    } else if (fu_G.mode == FU_MODE_TIME && fu_G.time_ms <= 0) {
        snprintf(title, sizeof title, "%s", _("¡Se acabó el tiempo!"));
    } else {
        snprintf(title, sizeof title, "%s", _("No quedan movimientos"));
    }
    lv_obj_t *c = panel_open(PANEL_OVER, NULL, w);
    lv_obj_t *lt = label(c, aos_font_title, col, title);
    lv_obj_set_width(lt, w - 64);
    lv_label_set_long_mode(lt, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(lt, LV_TEXT_ALIGN_CENTER, 0);

    int top = fu_core_top(&fu_G.g, fu_G.n);
    lv_obj_t *img = lv_image_create(c);
    lv_image_set_src(img, fu_art_tile((uint8_t)top));
    int is = U.land ? 100 : 150;
    lv_image_set_scale(img, (uint32_t)(is * 256 / U.cell));
    lv_obj_set_size(img, is + 10, is + 10);
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);

    if (fu_G.mode == FU_MODE_DUO) {
        char a[24], b[24];
        fmt_num(a, sizeof a, fu_G.score);
        fmt_num(b, sizeof b, fu_G.score2);
        snprintf(line, sizeof line, _("Jugador 1: %s   Jugador 2: %s"), a, b);
        label(c, aos_font_body, t->text, line);
    } else {
        fmt_num(num, sizeof num, fu_G.score);
        snprintf(line, sizeof line, _("%s puntos"), num);
        label(c, aos_font_large, t->text, line);
        uint32_t rec = fu_record(fu_G.n, fu_G.mode);
        if (fu_G.score >= rec && fu_G.score > 0) {
            label(c, aos_font_body, t->accent, _("¡Récord nuevo!"));
        } else {
            fmt_num(num, sizeof num, rec);
            snprintf(line, sizeof line, _("Récord: %s"), num);
            label(c, aos_font_body, t->dim, line);
        }
    }
    fu_value_text((uint8_t)top, v, sizeof v);
    snprintf(line, sizeof line, _("Ficha más alta: %s · %lu movimientos"), v, (unsigned long)fu_G.moves);
    lv_obj_t *ll = label(c, aos_font_small, t->dim, line);
    lv_obj_set_width(ll, w - 64);
    lv_label_set_long_mode(ll, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(ll, LV_TEXT_ALIGN_CENTER, 0);

    /* lying down the two buttons go side by side: there is no height for a column */
    lv_obj_t *items[2];
    lv_obj_t *bp = c;
    int bw = w - 64;
    if (U.land) {
        bp = box(c);
        lv_obj_set_size(bp, w - 64, 96);
        lv_obj_set_flex_flow(bp, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(bp, 16, 0);
        lv_obj_add_flag(bp, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        bw = (w - 64 - 16) / 2;
    }
    items[0] = button(bp, _("Otra vez"), bw, 96, t->accent, on_color(t->accent), cb_restart, 0);
    items[1] = button(bp, _("Menú"), bw, 96, t->card_line, t->text, cb_to_menu, 0);
    aos_pad_menu_set(&U.pm, items, 2, 0);
}

static void ach_open(lv_event_t *e)
{
    (void)e;
    const fu_theme_t *t = fu_theme();
    int w = U.land ? 860 : U.W - 60;
    lv_obj_t *c = panel_open(PANEL_ACH, _("Logros"), w);
    int got = 0;
    for (int i = 0; i < FU_ACHS; i++) got += (fu_P.ach >> i) & 1;
    char head[48];
    snprintf(head, sizeof head, _("%d de %d"), got, FU_ACHS);
    label(c, aos_font_small, t->dim, head);

    lv_obj_t *list = lv_obj_create(c);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, w - 64);
    lv_obj_set_height(list, U.H - 420 < 900 ? U.H - 420 : 900);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int i = 0; i < FU_ACHS; i++) {
        bool on = (fu_P.ach >> i) & 1;
        lv_obj_t *row = box(list);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(row, lv_color_hex(on ? mixhex(t->card, t->accent, 40) : t->card), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_border_color(row, lv_color_hex(on ? t->accent : t->card_line), 0);
        lv_obj_set_style_pad_left(row, 96, 0);
        lv_obj_set_style_pad_right(row, 16, 0);
        lv_obj_set_style_pad_top(row, 12, 0);
        lv_obj_set_style_pad_bottom(row, 12, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *mark = box(row);
        lv_obj_add_flag(mark, LV_OBJ_FLAG_IGNORE_LAYOUT);
        lv_obj_set_size(mark, 48, 48);
        lv_obj_align(mark, LV_ALIGN_LEFT_MID, -74, 0);
        lv_obj_set_style_radius(mark, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(mark, 3, 0);
        lv_obj_set_style_border_color(mark, lv_color_hex(on ? t->accent : t->card_line), 0);
        if (on) {
            lv_obj_set_style_bg_color(mark, lv_color_hex(t->accent), 0);
            lv_obj_set_style_bg_opa(mark, LV_OPA_COVER, 0);
            lv_obj_t *tick = label(mark, aos_font_body, on_color(t->accent), LV_SYMBOL_OK);
            lv_obj_center(tick);
        }
        label(row, aos_font_body, on ? t->text : t->dim, _(fu_ach_title(i)));
        lv_obj_t *d = label(row, aos_font_small, t->dim, _(fu_ach_text(i)));
        lv_obj_set_width(d, lv_pct(100));
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
    }
    lv_obj_t *items[1];
    items[0] = button(c, _("Cerrar"), w - 64, 96, t->accent, on_color(t->accent), cb_panel_close, 0);
    aos_pad_menu_set(&U.pm, items, 1, 0);
}

/* ------------------------------------------------------------------- menu */

static lv_obj_t *chip(lv_obj_t *parent, const char *txt, int w, lv_event_cb_t cb, intptr_t arg)
{
    lv_obj_t *b = button(parent, txt, w, 84, 0, 0, cb, arg);
    lv_obj_set_style_radius(b, 22, 0);
    return b;
}

static void chip_mark(lv_obj_t *b, bool on)
{
    const fu_theme_t *t = fu_theme();
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? t->accent : t->card), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(on ? t->accent : t->card_line), 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(on ? on_color(t->accent) : t->text), 0);
}

static void menu_refresh(void)
{
    if (U.scr != SCR_MENU || !U.menu) return;
    const fu_theme_t *t = fu_theme();
    for (int i = 0; i < 4; i++) {
        chip_mark(U.chip_n[i], fu_P.n == FU_NMIN + i);
        chip_mark(U.chip_m[i], fu_P.mode == i);
        lv_obj_set_style_border_color(U.chip_t[i], lv_color_hex(i == fu_P.theme ? t->accent : t->card_line), 0);
        lv_obj_set_style_border_width(U.chip_t[i], i == fu_P.theme ? 5 : 2, 0);
    }
    lv_label_set_text(U.lb_mode_desc, mode_desc(fu_P.mode));
    char line[128], num[24], v[12];
    uint32_t rec = fu_record(fu_P.n, fu_P.mode);
    uint8_t bt = fu_record_tile(fu_P.n, fu_P.mode);
    if (rec) {
        fmt_num(num, sizeof num, rec);
        fu_value_text(bt, v, sizeof v);
        snprintf(line, sizeof line, _("Récord en %d×%d, %s: %s puntos, ficha %s"), fu_P.n, fu_P.n,
                 mode_name(fu_P.mode), num, v);
    } else {
        snprintf(line, sizeof line, _("Todavía sin récord en %d×%d, %s"), fu_P.n, fu_P.n, mode_name(fu_P.mode));
    }
    lv_label_set_text(U.lb_rec, line);

    /* a game left half way: offer to carry on with it */
    if (fu_G.active && !fu_G.over) {
        snprintf(line, sizeof line, _("Seguir (%d×%d, %s)"), fu_G.n, fu_G.n, mode_name(fu_G.mode));
        lv_label_set_text(lv_obj_get_child(U.btn_resume, 0), line);
        lv_obj_remove_flag(U.btn_resume, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(U.btn_play, 0), _("Partida nueva"));
    } else {
        lv_obj_add_flag(U.btn_resume, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(lv_obj_get_child(U.btn_play, 0), _("Jugar"));
    }

    lv_obj_t *items[20];
    int n = 0;
    items[n++] = U.btn_exit;
    for (int i = 0; i < 4; i++) items[n++] = U.chip_n[i];
    for (int i = 0; i < 4; i++) items[n++] = U.chip_m[i];
    for (int i = 0; i < 4; i++) items[n++] = U.chip_t[i];
    items[n++] = U.btn_resume;
    items[n++] = U.btn_play;
    items[n++] = U.btn_ach;
    items[n++] = U.btn_snd;
    int sel = (fu_G.active && !fu_G.over) ? n - 4 : n - 3;
    if (U.pm.n == n && U.pm.sel < n) sel = U.pm.sel;
    aos_pad_menu_set(&U.pm, items, n, sel);
}

static lv_obj_t *section(lv_obj_t *parent, const char *title, int w)
{
    const fu_theme_t *t = fu_theme();
    lv_obj_t *l = label(parent, aos_font_small, t->dim, title);
    lv_obj_set_width(l, w);
    lv_obj_t *row = box(parent);
    lv_obj_set_size(row, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_set_style_pad_row(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    return row;
}

/* The little logo: three tiles of the theme, 2 4 8, a touch apart. */
static void logo(lv_obj_t *parent, int size)
{
    lv_obj_t *row = box(parent);
    lv_obj_set_size(row, size * 3 + 24, size);
    for (int i = 0; i < 3; i++) {
        lv_obj_t *img = lv_image_create(row);
        lv_image_set_src(img, fu_art_tile((uint8_t)(i + 1)));
        lv_image_set_scale(img, (uint32_t)(size * 256 / U.cell));
        lv_obj_set_size(img, size, size);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_pos(img, i * (size + 12), 0);
    }
}

static void build_menu(void)
{
    const fu_theme_t *t = fu_theme();
    U.menu = box(U.root);
    lv_obj_set_size(U.menu, U.W, U.H);

    /* upright one column; lying down the title and buttons left, the
     * choices right */
    lv_obj_t *left, *right;
    int lw, rw;
    if (U.land) {
        lw = 470;
        rw = U.W - lw - 3 * 40;
        left = box(U.menu);
        lv_obj_set_size(left, lw, U.H - 40);
        lv_obj_set_pos(left, 40, 20);
        right = box(U.menu);
        lv_obj_set_size(right, rw, U.H - 40);
        lv_obj_set_pos(right, 40 + lw + 40, 20);
    } else {
        lw = rw = U.W - 2 * 40;
        left = right = box(U.menu);
        lv_obj_set_size(left, lw, U.H - 40);
        lv_obj_set_pos(left, 40, 20);
    }
    lv_obj_t *cols[2] = { left, right };
    for (int i = 0; i < (U.land ? 2 : 1); i++) {
        lv_obj_set_flex_flow(cols[i], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(cols[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(cols[i], U.land ? 12 : 14, 0);
        lv_obj_add_flag(cols[i], LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }

    U.btn_exit = button(U.menu, LV_SYMBOL_CLOSE, 84, 84, t->card, t->text, cb_exit, 0);
    lv_obj_add_flag(U.btn_exit, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_pos(U.btn_exit, 20, 20);

    logo(left, U.land ? 110 : 118);
    lv_obj_t *title = label(left, aos_font_huge, t->text, "Fusión");
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_obj_t *tag = label(left, aos_font_small, t->dim, _("Deslizá, juntá dos iguales, llegá más alto."));
    lv_obj_set_width(tag, lw);
    lv_label_set_long_mode(tag, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_CENTER, 0);

    int cw4 = (rw - 3 * 12) / 4;
    lv_obj_t *row = section(right, _("Tablero"), rw);
    for (int i = 0; i < 4; i++) {
        char s[12];
        snprintf(s, sizeof s, "%d×%d", FU_NMIN + i, FU_NMIN + i);
        U.chip_n[i] = chip(row, s, cw4, cb_size, FU_NMIN + i);
    }
    row = section(right, _("Modo"), rw);
    int cw2 = (rw - 12) / 2;
    for (int i = 0; i < FU_MODES; i++) U.chip_m[i] = chip(row, mode_name(i), cw2, cb_mode, i);
    U.lb_mode_desc = label(right, aos_font_small, t->text, "");
    lv_obj_set_width(U.lb_mode_desc, rw);
    lv_label_set_long_mode(U.lb_mode_desc, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(U.lb_mode_desc, 70);

    row = section(right, _("Tema"), rw);
    for (int i = 0; i < FU_THEMES; i++) {
        const fu_theme_t *th = fu_theme_at(i);
        lv_obj_t *b = button(row, NULL, cw4, 116, th->bg_top, th->text, cb_theme, i);
        lv_obj_set_style_radius(b, 22, 0);
        lv_obj_set_style_bg_grad_color(b, lv_color_hex(th->board), 0);
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_t *sw = box(b);
        lv_obj_set_size(sw, 40, 40);
        lv_obj_align(sw, LV_ALIGN_TOP_MID, 0, 12);
        lv_obj_set_style_radius(sw, th->style == FU_ST_FOREST ? 14 : 8, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(th->ramp[4]), 0);
        lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(sw, th->style == FU_ST_NEON ? 3 : 0, 0);
        lv_obj_set_style_border_color(sw, lv_color_hex(th->ramp[0]), 0);
        if (th->style == FU_ST_NEON) lv_obj_set_style_bg_color(sw, lv_color_hex(0x161B4A), 0);
        lv_obj_t *l = label(b, aos_font_caption, th->text, _(th->name));
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -10);
        U.chip_t[i] = b;
    }

    U.lb_rec = label(right, aos_font_small, t->dim, "");
    lv_obj_set_width(U.lb_rec, rw);
    lv_label_set_long_mode(U.lb_rec, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(U.lb_rec, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *bcol = U.land ? left : right;
    int bw = U.land ? lw : rw;
    U.btn_resume = button(bcol, "", bw, 100, t->accent2, on_color(t->accent2), cb_resume, 0);
    U.btn_play = button(bcol, _("Jugar"), bw, 100, t->accent, on_color(t->accent), cb_play, 0);
    lv_obj_t *duo = box(bcol);
    lv_obj_set_size(duo, bw, 84);
    lv_obj_add_flag(duo, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    U.btn_ach = button(duo, _("Logros"), (bw - 12) / 2, 84, t->card, t->text, ach_open, 0);
    U.btn_snd = button(duo, fu_P.sound ? _("Sonido: sí") : _("Sonido: no"), (bw - 12) / 2, 84, t->card,
                       t->text, cb_sound, 0);
    lv_obj_set_pos(U.btn_snd, (bw - 12) / 2 + 12, 0);
    lv_obj_set_style_border_width(U.btn_ach, 2, 0);
    lv_obj_set_style_border_color(U.btn_ach, lv_color_hex(t->card_line), 0);
    lv_obj_set_style_border_width(U.btn_snd, 2, 0);
    lv_obj_set_style_border_color(U.btn_snd, lv_color_hex(t->card_line), 0);

    lv_obj_add_flag(U.menu, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.menu, gesture_cb, LV_EVENT_GESTURE, NULL);
    menu_refresh();
}

/* ------------------------------------------------------------------- game */

static void build_game(void)
{
    const fu_theme_t *t = fu_theme();

    /* the touch layer is the screen itself, behind everything: a swipe
     * anywhere counts, not only on the board */
    U.touch = box(U.root);
    lv_obj_set_size(U.touch, U.W, U.H);
    lv_obj_add_flag(U.touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.touch, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(U.touch, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(U.touch, touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(U.touch, touch_cb, LV_EVENT_PRESS_LOST, NULL);

    U.board = box(U.root);
    lv_obj_set_size(U.board, U.bpx, U.bpx);
    lv_obj_set_pos(U.board, U.bx, U.by);
    lv_obj_set_style_bg_color(U.board, lv_color_hex(t->board), 0);
    lv_obj_set_style_bg_opa(U.board, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(U.board, t->style == FU_ST_FOREST ? 40 : t->style == FU_ST_PAPER ? 14 : 28, 0);
    lv_obj_set_style_border_width(U.board, 2, 0);
    lv_obj_set_style_border_color(U.board, lv_color_hex(t->card_line), 0);
    lv_obj_add_flag(U.board, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    int n = fu_G.n;
    int m = U.cell * 45 / 1000 < 4 ? 4 : U.cell * 45 / 1000;
    int rad = t->style == FU_ST_FOREST ? U.cell * 24 / 100 : t->style == FU_ST_PAPER ? U.cell * 9 / 100
            : t->style == FU_ST_GEM ? U.cell * 12 / 100 : U.cell * 16 / 100;
    for (int r = 0; r < n; r++)
        for (int c = 0; c < n; c++) {
            lv_obj_t *s = box(U.board);
            int x, y;
            cell_xy(r, c, &x, &y);
            lv_obj_set_pos(s, x + m, y + m);
            lv_obj_set_size(s, U.cell - 2 * m, U.cell - 2 * m);
            lv_obj_set_style_radius(s, rad, 0);
            lv_obj_set_style_bg_color(s, lv_color_hex(t->slot), 0);
            lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
            if (t->style == FU_ST_NEON) {
                lv_obj_set_style_border_width(s, 2, 0);
                lv_obj_set_style_border_color(s, lv_color_hex(0x232A66), 0);
            }
        }
    for (int i = 0; i < NVIEWS; i++) {
        U.v[i].img = lv_image_create(U.board);
        lv_obj_remove_flag(U.v[i].img, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(U.v[i].img, U.cell, U.cell);
        lv_image_set_pivot(U.v[i].img, U.cell / 2, U.cell / 2);
        lv_obj_add_flag(U.v[i].img, LV_OBJ_FLAG_HIDDEN);
        U.v[i].used = false;
    }

    /* the HUD: a row of buttons, three cards, a line of text */
    int y = U.hy;
    U.btn_pause = button(U.root, LV_SYMBOL_PAUSE, 92, 92, t->card, t->text, cb_pause, 0);
    lv_obj_set_pos(U.btn_pause, U.hx, y);
    lv_obj_set_style_border_width(U.btn_pause, 2, 0);
    lv_obj_set_style_border_color(U.btn_pause, lv_color_hex(t->card_line), 0);
    lv_obj_t *ttl = label(U.root, aos_font_title, t->text, "Fusión");
    lv_obj_align_to(ttl, U.btn_pause, LV_ALIGN_OUT_RIGHT_MID, 24, 0);

    U.btn_undo = button(U.root, NULL, 172, 92, t->card, t->text, cb_undo, 0);
    lv_obj_set_pos(U.btn_undo, U.hx + U.hw - 172, y);
    lv_obj_set_style_border_width(U.btn_undo, 2, 0);
    lv_obj_set_style_border_color(U.btn_undo, lv_color_hex(t->card_line), 0);
    lv_obj_t *ul = label(U.btn_undo, aos_font_title, t->text, LV_SYMBOL_LOOP);
    lv_obj_align(ul, LV_ALIGN_LEFT_MID, 18, 0);
    U.lb_undo = label(U.btn_undo, aos_font_body, t->text, "");
    lv_obj_align(U.lb_undo, LV_ALIGN_RIGHT_MID, -20, 0);

    y += 92 + (U.land ? 28 : 36);
    int gap = 16, cw = (U.hw - 2 * gap) / 3, ch = U.land ? 150 : 160;
    for (int i = 0; i < 3; i++) {
        lv_obj_t *c = box(U.root);
        lv_obj_set_size(c, cw, ch);
        lv_obj_set_pos(c, U.hx + i * (cw + gap), y);
        lv_obj_set_style_bg_color(c, lv_color_hex(t->card), 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(c, 24, 0);
        lv_obj_set_style_border_width(c, 2, 0);
        lv_obj_set_style_border_color(c, lv_color_hex(t->card_line), 0);
        U.card[i] = c;
        U.card_t[i] = label(c, aos_font_small, t->dim, "");
        lv_obj_align(U.card_t[i], LV_ALIGN_TOP_MID, 0, 14);
        U.card_v[i] = label(c, aos_font_large, t->text, "");
        lv_obj_align(U.card_v[i], LV_ALIGN_BOTTOM_MID, 0, -18);
    }
    U.goal_img = lv_image_create(U.card[2]);
    int gs = ch - 64;
    lv_image_set_scale(U.goal_img, (uint32_t)(gs * 256 / U.cell));
    lv_obj_set_size(U.goal_img, gs + 8, gs + 8);
    lv_image_set_inner_align(U.goal_img, LV_IMAGE_ALIGN_CENTER);
    lv_obj_align(U.goal_img, LV_ALIGN_BOTTOM_MID, 0, -6);

    U.plus = label(U.root, aos_font_body, t->accent, "");
    lv_obj_add_flag(U.plus, LV_OBJ_FLAG_HIDDEN);

    y += ch + (U.land ? 56 : 22);      /* lying down the "+N" needs the room above the line */
    U.lb_info = label(U.root, aos_font_small, t->dim, "");
    lv_obj_set_width(U.lb_info, U.hw);
    lv_obj_set_pos(U.lb_info, U.hx, y);
    lv_label_set_long_mode(U.lb_info, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(U.lb_info, U.land ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER, 0);

    U.bar = NULL;
    if (fu_G.mode == FU_MODE_TIME) {
        U.bar = box(U.root);
        lv_obj_set_size(U.bar, U.hw, 14);
        lv_obj_set_pos(U.bar, U.hx, y + 50);
        lv_obj_set_style_bg_color(U.bar, lv_color_hex(t->slot), 0);
        lv_obj_set_style_bg_opa(U.bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(U.bar, 7, 0);
        U.bar_fill = box(U.bar);
        lv_obj_set_size(U.bar_fill, U.hw, 14);
        lv_obj_set_style_bg_opa(U.bar_fill, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(U.bar_fill, 7, 0);
    }

    /* the hint: under the board upright, at the foot of the column lying down */
    U.lb_hint = label(U.root, aos_font_small, t->dim,
                      _("Deslizá en cualquier dirección. Dos fichas iguales se funden en una."));
    lv_obj_set_width(U.lb_hint, U.land ? U.hw : U.bpx);
    lv_label_set_long_mode(U.lb_hint, LV_LABEL_LONG_MODE_WRAP);
    if (U.land) {
        lv_obj_set_style_text_align(U.lb_hint, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(U.lb_hint, LV_ALIGN_BOTTOM_LEFT, U.hx, -MARGIN - 8);
    } else {
        lv_obj_set_style_text_align(U.lb_hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(U.lb_hint, U.bx, U.by + U.bpx + 34);
    }

    /* the banner of a goal reached, over the board */
    U.banner = label(U.root, aos_font_huge, t->accent, "");
    lv_obj_set_style_text_align(U.banner, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(U.banner, LV_OBJ_FLAG_HIDDEN);
    U.banner_sub = label(U.root, aos_font_body, t->text, "");
    lv_obj_set_style_text_align(U.banner_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(U.banner_sub, LV_OBJ_FLAG_HIDDEN);

    views_sync();
    hud_refresh();
}

/* Everything again: a new screen, a theme, a turn of the screen. */
static void build(void)
{
    aos_pad_menu_clear(&U.pm);
    U.panel_obj = NULL;
    U.panel = PANEL_NONE;
    lv_obj_clean(U.root);
    memset(U.v, 0, sizeof U.v);
    memset(U.card, 0, sizeof U.card);
    U.menu = NULL;
    U.board = NULL;
    U.bar = NULL;
    U.plus = U.banner = U.banner_sub = NULL;
    U.btn_snd = NULL;
    U.animating = false;
    U.pressing = false;

    layout();
    fu_art_setup(fu_P.theme, U.cell);
    const fu_theme_t *t = fu_theme();
    lv_obj_set_style_bg_color(U.root, lv_color_hex(t->bg_top), 0);
    lv_obj_set_style_bg_grad_color(U.root, lv_color_hex(t->bg_bot), 0);
    lv_obj_set_style_bg_grad_dir(U.root, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(U.root, LV_OPA_COVER, 0);

    if (U.scr == SCR_GAME && fu_G.active) {
        build_game();
        if (fu_G.over) panel_over();
    } else {
        U.scr = SCR_MENU;
        build_menu();
    }
}

static void go_menu(void)
{
    U.scr = SCR_MENU;
    build();
}

/* ------------------------------------------------------------------ frame */

static void pad_step(void)
{
    aos_pad_update(&U.pad, lv_tick_get());
    if (U.kb_bits) {
        U.pad.pressed |= U.kb_bits;
        U.pad.repeat |= U.kb_bits;
        U.kb_bits = 0;
    }
    if (!U.pad.pressed && !U.pad.repeat) return;
    bool start = aos_pad_pressed(&U.pad, AOS_PAD_START);
    bool b = aos_pad_pressed(&U.pad, AOS_PAD_B);

    if (U.panel != PANEL_NONE) {
        if ((start || b) && U.panel == PANEL_PAUSE) panel_close();
        else if (b && U.panel == PANEL_ACH) panel_close();
        else aos_pad_menu_step(&U.pm, &U.pad);
        return;
    }
    if (U.scr == SCR_MENU) {
        if (start) game_start(fu_G.active && !fu_G.over);
        else aos_pad_menu_step(&U.pm, &U.pad);
        return;
    }
    if (start) {
        pause_open();
        return;
    }
    if (b || aos_pad_pressed(&U.pad, AOS_PAD_L)) {
        do_undo();
        return;
    }
    uint32_t d = U.pad.pressed & AOS_PAD_DIRS;
    if (d & AOS_PAD_UP) do_move(FU_UP);
    else if (d & AOS_PAD_DOWN) do_move(FU_DOWN);
    else if (d & AOS_PAD_LEFT) do_move(FU_LEFT);
    else if (d & AOS_PAD_RIGHT) do_move(FU_RIGHT);
}

static void fx_step(uint32_t now)
{
    if (U.plus && !lv_obj_has_flag(U.plus, LV_OBJ_FLAG_HIDDEN)) {
        uint32_t e = now - U.plus_t0;
        if (e >= T_PLUS) {
            lv_obj_add_flag(U.plus, LV_OBJ_FLAG_HIDDEN);
        } else {
            float p = (float)e / T_PLUS;
            /* under the card of whoever scored, rising a little as it fades */
            lv_obj_align_to(U.plus, U.card[fu_G.mode == FU_MODE_DUO && !fu_G.turn ? 1 : 0], LV_ALIGN_OUT_BOTTOM_MID,
                            0, 14 - (int)(16 * p));
            lv_obj_set_style_opa(U.plus, (lv_opa_t)(255 * (1 - p * p)), 0);
        }
    }
    if (U.banner && !lv_obj_has_flag(U.banner, LV_OBJ_FLAG_HIDDEN)) {
        uint32_t e = now - U.banner_t0;
        if (e >= T_BANNER) {
            lv_obj_add_flag(U.banner, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(U.banner_sub, LV_OBJ_FLAG_HIDDEN);
        } else {
            float p = (float)e / T_BANNER;
            float s = p < 0.15f ? ease_back(p / 0.15f) : 1;
            lv_opa_t o = (lv_opa_t)(p > 0.75f ? 255 * (1 - (p - 0.75f) / 0.25f) : 255);
            int cy = U.by + U.bpx / 2;
            lv_obj_set_style_transform_scale(U.banner, (int32_t)(256 * s), 0);
            lv_obj_set_style_transform_pivot_x(U.banner, lv_pct(50), 0);
            lv_obj_set_style_transform_pivot_y(U.banner, lv_pct(50), 0);
            lv_obj_align(U.banner, LV_ALIGN_TOP_MID, U.bx + U.bpx / 2 - U.W / 2, cy - 90);
            lv_obj_align(U.banner_sub, LV_ALIGN_TOP_MID, U.bx + U.bpx / 2 - U.W / 2, cy + 6);
            lv_obj_set_style_opa(U.banner, o, 0);
            lv_obj_set_style_opa(U.banner_sub, o, 0);
        }
    }
}

static void timer_step(uint32_t now)
{
    uint32_t dt = now - U.last_ms;
    U.last_ms = now;
    if (dt > 200) dt = 200;
    if (fu_G.mode != FU_MODE_TIME || U.scr != SCR_GAME || U.panel != PANEL_NONE || fu_G.over || !fu_G.active)
        return;
    fu_G.time_ms -= (int32_t)dt;
    int32_t sec = fu_G.time_ms > 0 ? (fu_G.time_ms + 999) / 1000 : 0;
    if (sec != U.last_sec) {
        U.last_sec = sec;
        if (sec <= 10 && sec > 0) fu_snd_play(FU_S_TICK, 0);
        hud_refresh();
    } else if (U.bar) {
        hud_refresh();
    }
    if (fu_G.time_ms <= 0) {
        fu_G.time_ms = 0;
        anim_finish();
        game_end();
        hud_refresh();
    }
}

void fu_ui_frame(void)
{
    if (U.want_exit) {
        U.want_exit = false;
        aos_ui_back();          /* destroys the app: nothing after this */
        return;
    }
    uint32_t now = lv_tick_get();
    fu_snd_pump();
    if (U.hidden) {
        U.last_ms = now;
        return;
    }
    pad_step();
    if (U.want_exit) return;
    if (U.animating) U.fps_anim++;
    anim_step(now);
    fx_step(now);
    timer_step(now);
    if (U.over_at && (int32_t)(now - U.over_at) >= 0 && U.scr == SCR_GAME) {
        U.over_at = 0;
        anim_finish();
        hud_refresh();
        panel_over();
    }

    /* fps: frames the timer ran, over the last five seconds of a game */
    U.fps_n++;
    if (now - U.fps_t0 >= 5000) {
        if (U.scr == SCR_GAME && U.fps_anim)
            aos_hal_log("fusion", "%d fps (%d frames animating), %d×%d, cell %d px", U.fps_n * 1000 / (int)(now - U.fps_t0),
                        U.fps_anim, fu_G.n, fu_G.n, U.cell);
        U.fps_t0 = now;
        U.fps_n = U.fps_anim = 0;
    }
}

/* ---------------------------------------------------------------- the app */

void fu_ui_create(lv_obj_t *root, aos_app_t *self)
{
    memset(&U, 0, sizeof U);
    U.self = self;
    U.root = root;
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(root);
    aos_pad_reset(&U.pad, lv_tick_get());
    U.last_ms = U.fps_t0 = lv_tick_get();
    U.scr = SCR_MENU;
#ifdef AOS_SIM_BUILTIN
    /* Simulator shortcuts for screenshots and for looking at things calmly:
     *   FUSION_SLOW=8          the animation eight times slower
     *   FUSION_THEME=2         the theme
     *   FUSION_TEST="4 0 2480 1 2 3 4 8 7 6 5 ..."
     *                          a game of that size and mode with that score,
     *                          the cells row by row as exponents (0 empty,
     *                          255 a stone); only on the first create */
    static bool tested;
    if (getenv("FUSION_SLOW")) s_slow = atoi(getenv("FUSION_SLOW")) > 0 ? atoi(getenv("FUSION_SLOW")) : 1;
    if (!tested && getenv("FUSION_THEME")) fu_P.theme = (uint8_t)(atoi(getenv("FUSION_THEME")) % FU_THEMES);
    const char *test = tested ? NULL : getenv("FUSION_TEST");
    tested = true;
    if (test) {
        char *end;
        int n = (int)strtol(test, &end, 10);
        int mode = (int)strtol(end, &end, 10);
        fu_game_new(n, mode);
        fu_G.score = (uint32_t)strtol(end, &end, 10);
        for (int r = 0; r < fu_G.n; r++)
            for (int c = 0; c < fu_G.n; c++) {
                int e = (int)strtol(end, &end, 10);
                fu_G.g.v[r][c] = (uint8_t)e;
                fu_G.g.id[r][c] = e ? fu_new_id() : 0;
            }
        int top = fu_core_top(&fu_G.g, fu_G.n);
        while (fu_G.goal <= top) {
            fu_G.goal++;
            fu_G.level++;
        }
        if (mode == FU_MODE_DUO) fu_G.score2 = fu_G.score * 9 / 10;
        fu_G.moves = fu_G.score / 40;
        U.scr = SCR_GAME;
    }
#endif
    build();
#ifdef AOS_SIM_BUILTIN
    if (test && !fu_core_can_move(&fu_G.g, fu_G.n)) game_end();    /* to look at the end panel */
#endif
}

void fu_ui_rebuild(lv_obj_t *root)
{
    U.root = root;
    lv_obj_update_layout(root);
    int panel = U.panel;
    anim_finish();
    build();
    if (U.scr == SCR_GAME && panel == PANEL_PAUSE) pause_open();
}

void fu_ui_destroy(void)
{
    if (fu_G.active && !fu_G.over && fu_G.mode != FU_MODE_DUO) {
        fu_record_offer(fu_G.n, fu_G.mode, fu_G.score, (uint8_t)fu_core_top(&fu_G.g, fu_G.n));
    }
    fu_game_save();
    aos_pad_menu_clear(&U.pm);
    if (U.root) lv_obj_clean(U.root);
    memset(&U, 0, sizeof U);
}

void fu_ui_hide(void)
{
    U.hidden = true;
    if (U.scr == SCR_GAME && U.panel == PANEL_NONE && fu_G.active && !fu_G.over) pause_open();
    fu_game_save();
}

void fu_ui_show(void)
{
    U.hidden = false;
    U.last_ms = lv_tick_get();
    aos_pad_reset(&U.pad, lv_tick_get());
}

bool fu_ui_back(void)
{
    if (U.panel == PANEL_PAUSE || U.panel == PANEL_ACH) {
        panel_close();
        return true;
    }
    if (U.panel == PANEL_OVER) {
        go_menu();
        return true;
    }
    if (U.scr == SCR_GAME) {
        pause_open();
        return true;
    }
    return false;
}

bool fu_ui_key(uint32_t key, uint8_t mods)
{
    (void)mods;
    if (U.hidden) return false;
    bool play = U.scr == SCR_GAME && U.panel == PANEL_NONE;
    if (play) {
        switch (key) {
        case AOS_KEY_UP: case 'w': case 'W': case 'k': do_move(FU_UP); return true;
        case AOS_KEY_DOWN: case 's': case 'S': case 'j': do_move(FU_DOWN); return true;
        case AOS_KEY_LEFT: case 'a': case 'A': case 'h': do_move(FU_LEFT); return true;
        case AOS_KEY_RIGHT: case 'd': case 'D': case 'l': do_move(FU_RIGHT); return true;
        case 'u': case 'U': case 'z': case 'Z': case AOS_KEY_BACKSPACE: do_undo(); return true;
        case AOS_KEY_ESC: case 'p': case 'P': pause_open(); return true;
        default: return false;
        }
    }
    switch (key) {
    case AOS_KEY_UP: U.kb_bits |= AOS_PAD_UP; return true;
    case AOS_KEY_DOWN: U.kb_bits |= AOS_PAD_DOWN; return true;
    case AOS_KEY_LEFT: U.kb_bits |= AOS_PAD_LEFT; return true;
    case AOS_KEY_RIGHT: U.kb_bits |= AOS_PAD_RIGHT; return true;
    case AOS_KEY_ENTER: case ' ': U.kb_bits |= AOS_PAD_A; return true;
    case AOS_KEY_ESC: case AOS_KEY_BACKSPACE: case 'p': case 'P':
        if (U.panel != PANEL_NONE) U.kb_bits |= AOS_PAD_B;
        else return false;
        return true;
    default: return false;
    }
}
