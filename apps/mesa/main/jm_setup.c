/*
 * Juegos de mesa - a new game's sheet, the settings, and the chess clock
 * on its own (for a game on a real board: each half of the screen is a
 * player's, tapped to end the turn, the far one turned half round).
 *
 * Every choice is a row with a button that steps through its values, so
 * the gamepad goes through a sheet with the d-pad and A like a finger.
 */
#include "jm_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static lv_obj_t *s_sheet, *s_col;
static int s_gi;
static jm_new_t s_new;
static int s_mode;          /* 0 against the board, 1 two people (two players' games) */
static int s_color;         /* 0 the first side, 1 the second, 2 at random */
static int s_level = 3;
static int s_clock;
static int s_inc_kind;      /* 0 Fischer, 1 Bronstein */
static int s_np;            /* players, for more than two */
static uint8_t s_people[MESA_MAX_PLAYERS];
static bool s_from_game;

/* time, increment, kind: the presets of the clock */
const int jm_clock_presets[][2] = {
    { 0, 0 }, { 60, 0 }, { 180, 0 }, { 180, 2 }, { 300, 0 }, { 300, 3 }, { 600, 0 }, { 900, 10 }, { 1800, 0 },
    { 3600, 0 }, { -30, 0 }, { -60, 0 },
};
const int jm_nclock_presets = (int)(sizeof jm_clock_presets / sizeof jm_clock_presets[0]);

static const char *preset_text(int i, char *b, size_t n)
{
    int t = jm_clock_presets[i][0], inc = jm_clock_presets[i][1];
    if (t == 0) snprintf(b, n, "%s", _("Sin reloj"));
    else if (t < 0) snprintf(b, n, "%d s %s", -t, _("por jugada"));
    else if (inc) snprintf(b, n, "%d + %d", t / 60, inc);
    else snprintf(b, n, "%d min", t / 60);
    return b;
}

const char *jm_clock_text(int32_t ms, char *buf, size_t n)
{
    if (ms < 0) ms = 0;
    int s = (int)(ms / 1000);
    if (ms < 10000) snprintf(buf, n, "%d.%d", s, (int)(ms / 100) % 10);
    else if (s >= 3600) snprintf(buf, n, "%d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, n, "%d:%02d", s / 60, s % 60);
    return buf;
}

/* ---- the sheet's pieces ---------------------------------------------------------------- */

static void sheet_close(void)
{
    aos_pad_menu_clear(&jm->menu);
    if (s_sheet) lv_obj_delete(s_sheet);
    s_sheet = s_col = NULL;
}

static lv_obj_t *sheet_open(const char *title)
{
    sheet_close();
    lv_obj_t *s = lv_obj_create(jm->root);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, jm->W, jm->H);
    lv_obj_set_style_bg_color(s, C(0x120D09), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *col = lv_obj_create(s);
    lv_obj_remove_style_all(col);
    int w = jm->portrait ? jm->W - 64 : 760;
    lv_obj_set_size(col, w, jm->H);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_pad_top(col, 40, 0);
    lv_obj_set_style_pad_bottom(col, 60, 0);
    lv_obj_set_style_pad_hor(col, 12, 0);
    lv_obj_set_style_pad_row(col, 18, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_t *t = jm_label(col, title, aos_font_title, C(0xF4E8D4));
    lv_obj_set_width(t, lv_pct(100));
    s_sheet = s;
    s_col = col;
    return col;
}

static void sheet_pad(void)
{
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int n = 0;
    uint32_t cnt = lv_obj_get_child_count(s_col);
    for (uint32_t i = 0; i < cnt && n < AOS_PAD_MENU_MAX; i++) {
        lv_obj_t *row = lv_obj_get_child(s_col, (int32_t)i);
        if (lv_obj_check_type(row, &lv_button_class)) {
            items[n++] = row;
            continue;
        }
        uint32_t k = lv_obj_get_child_count(row);
        for (uint32_t j = 0; j < k && n < AOS_PAD_MENU_MAX; j++) {
            lv_obj_t *c = lv_obj_get_child(row, (int32_t)j);
            if (lv_obj_check_type(c, &lv_button_class)) items[n++] = c;
        }
    }
    aos_pad_menu_set(&jm->menu, items, n, 0);
    aos_pad_reset(&jm->pad, lv_tick_get());
}

/* a row: its name on the left, a button with the value on the right */
static lv_obj_t *row(lv_obj_t *col, const char *name, const char *value, lv_event_cb_t cb, intptr_t ud)
{
    lv_obj_t *r = lv_obj_create(col);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), 96);
    lv_obj_set_style_bg_color(r, C(0x221A13), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(r, 20, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(r, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_t *l = jm_label(r, name, aos_font_body, C(0xF4E8D4));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_t *b = lv_button_create(r);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 72);
    lv_obj_set_style_min_width(b, 200, 0);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_bg_color(b, C(0x3A2C20), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C(0x5A4430), LV_STATE_PRESSED);
    lv_obj_t *v = jm_label(b, value, aos_font_body, AOS_C_ACCENT);
    lv_obj_center(v);
    lv_obj_set_width(b, LV_SIZE_CONTENT);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)ud);
    return r;
}

/* ---- a new game ------------------------------------------------------------------------- */

static void setup_build(void);

static void ev_mode(lv_event_t *e)
{
    (void)e;
    s_mode ^= 1;
    setup_build();
}

static void ev_color(lv_event_t *e)
{
    (void)e;
    s_color = (s_color + 1) % 3;
    setup_build();
}

static void ev_level(lv_event_t *e)
{
    (void)e;
    int lv = jm_games[s_gi].g->levels;
    s_level = s_level % (lv > 0 ? lv : 1) + 1;
    setup_build();
}

static void ev_two(lv_event_t *e)
{
    (void)e;
    s_new.two = s_new.two == TWO_FACE ? TWO_PASS : TWO_FACE;
    setup_build();
}

static void ev_opt(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    const mesa_opt_t *o = &jm_games[s_gi].g->opts[i];
    int n = 0;
    while (o->values[n]) n++;
    s_new.opts[i] = (uint8_t)((s_new.opts[i] + 1) % (n ? n : 1));
    s_np = jm_games[s_gi].g->players(s_new.opts);
    setup_build();
}

static void ev_clock(lv_event_t *e)
{
    (void)e;
    s_clock = (s_clock + 1) % jm_nclock_presets;
    setup_build();
}

static void ev_inc(lv_event_t *e)
{
    (void)e;
    s_inc_kind ^= 1;
    setup_build();
}

static void ev_person(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_people[i] ^= 1;
    setup_build();
}

static void ev_play(lv_event_t *e)
{
    (void)e;
    const mesa_game_t *g = jm_games[s_gi].g;
    jm_new_t n = s_new;
    n.gi = s_gi;
    int np = g->players(n.opts);
    if (np <= 2 && g->max_players >= 2) {
        int me = s_color == 2 ? (int)(jm_rand() & 1) : s_color;
        for (int i = 0; i < np; i++) {
            n.human[i] = s_mode == 1 || i == me || g->max_players == 1 ? 1 : 0;
            n.level[i] = (uint8_t)s_level;
        }
    } else {
        for (int i = 0; i < np; i++) {
            n.human[i] = s_people[i];
            n.level[i] = (uint8_t)s_level;
        }
    }
    int t = jm_clock_presets[s_clock][0], inc = jm_clock_presets[s_clock][1];
    n.clock_mode = t == 0 ? CLK_NONE : t < 0 ? CLK_MOVE : inc ? (s_inc_kind ? CLK_BRONSTEIN : CLK_FISCHER) : CLK_SUDDEN;
    n.clock_base = (t < 0 ? -t : t) * 1000;
    n.clock_inc = inc * 1000;
    /* remembered for next time */
    char key[24];
    snprintf(key, sizeof key, "jm_%.6s", g->prefix);
    int32_t packed = s_mode | (s_color << 1) | (s_level << 3) | (s_clock << 8) | (s_inc_kind << 13) | (s_new.two << 14) |
                     (s_new.opts[0] << 16) | (s_new.opts[1] << 20) | ((int32_t)s_people[1] << 24) |
                     ((int32_t)s_people[2] << 25) | ((int32_t)s_people[3] << 26) | ((int32_t)(s_new.opts[2] & 7) << 27);
    aos_hal_pref_set_i32(key, packed);
    sheet_close();
    if (jm->home) lv_obj_add_flag(jm->home, LV_OBJ_FLAG_HIDDEN);
    jm_play_start(&n);
}

static void ev_back(lv_event_t *e)
{
    (void)e;
    sheet_close();
    if (s_from_game && jm->playing) return;
    jm_home_show();
}

static void setup_build(void)
{
    const mesa_game_t *g = jm_games[s_gi].g;
    lv_obj_t *col = sheet_open(_(g->name));
    lv_obj_t *ab = jm_label(col, _(g->about), aos_font_body, C(0xB8A890));
    lv_obj_set_width(ab, lv_pct(100));
    lv_label_set_long_mode(ab, LV_LABEL_LONG_MODE_WRAP);
    char b[64];
    int np = g->players(s_new.opts);
    for (int i = 0; i < g->nopts && i < MESA_MAX_OPTS; i++) {
        const mesa_opt_t *o = &g->opts[i];
        row(col, _(o->label), _(o->values[s_new.opts[i]]), ev_opt, i);
    }
    if (np <= 2) {
        row(col, _("Jugadores"), s_mode ? _("De a dos") : _("Contra la placa"), ev_mode, 0);
        if (!s_mode) {
            const char *a = g->side(s_new.opts, 0, NULL), *c = g->side(s_new.opts, 1, NULL);
            row(col, _("Jugás con"), s_color == 2 ? _("Al azar") : _(s_color ? c : a), ev_color, 0);
            snprintf(b, sizeof b, "%d / %d", s_level, g->levels);
            row(col, _("Nivel de la placa"), b, ev_level, 0);
        } else {
            bool turn = (g->flags & MESA_GF_ROTATE) != 0;
            row(col, _("Cómo"), s_new.two == TWO_PASS && turn ? _("Pasando la placa") : _("Cara a cara"), ev_two, 0);
        }
    } else {
        for (int i = 0; i < np; i++) {
            const char *nm = g->side(s_new.opts, i, NULL);
            row(col, _(nm), s_people[i] ? _("Persona") : _("Placa"), ev_person, i);
        }
        snprintf(b, sizeof b, "%d / %d", s_level, g->levels);
        row(col, _("Nivel de la placa"), b, ev_level, 0);
    }
    if ((g->flags & MESA_GF_CLOCK) && np == 2) {
        row(col, _("Reloj"), preset_text(s_clock, b, sizeof b), ev_clock, 0);
        if (jm_clock_presets[s_clock][1]) row(col, _("Incremento"), s_inc_kind ? _("Bronstein") : _("Fischer"), ev_inc, 0);
    }
    lv_obj_t *go = jm_button(col, _("Jugar"), ev_play, NULL);
    lv_obj_set_width(go, lv_pct(100));
    lv_obj_set_height(go, 96);
    lv_obj_set_style_bg_color(go, AOS_C_ACCENT, 0);
    lv_obj_t *bk = jm_button(col, _("Volver"), ev_back, NULL);
    lv_obj_set_width(bk, lv_pct(100));
    sheet_pad();
    /* the gamepad starts on Jugar */
    for (int i = 0; i < jm->menu.n; i++) {
        if (jm->menu.item[i] == go) jm->menu.sel = (uint8_t)i;
    }
}

void jm_setup_open(int gi)
{
    if (gi < 0 || gi >= jm_ngames) return;
    s_from_game = jm->playing;
    s_gi = gi;
    const mesa_game_t *g = jm_games[gi].g;
    memset(&s_new, 0, sizeof s_new);
    for (int i = 0; i < g->nopts && i < MESA_MAX_OPTS; i++) s_new.opts[i] = g->opts[i].def;
    char key[24];
    snprintf(key, sizeof key, "jm_%.6s", g->prefix);
    int32_t v = 0;
    s_mode = 0;
    s_color = 0;
    s_level = g->levels >= 3 ? 3 : 1;
    s_clock = 0;
    s_inc_kind = 0;
    s_people[0] = 1;
    s_people[1] = s_people[2] = s_people[3] = 0;
    if (aos_hal_pref_get_i32(key, &v)) {
        s_mode = v & 1;
        s_color = (v >> 1) & 3;
        if (s_color > 2) s_color = 0;
        s_level = (v >> 3) & 31;
        if (s_level < 1 || s_level > g->levels) s_level = g->levels >= 3 ? 3 : 1;
        s_clock = (v >> 8) & 31;
        if (s_clock >= jm_nclock_presets) s_clock = 0;
        s_inc_kind = (v >> 13) & 1;
        s_new.two = (v >> 14) & 1;
        if (g->nopts > 0) s_new.opts[0] = (uint8_t)((v >> 16) & 15);
        if (g->nopts > 1) s_new.opts[1] = (uint8_t)((v >> 20) & 15);
        if (g->nopts > 2) s_new.opts[2] = (uint8_t)((v >> 27) & 7);
        s_people[1] = (uint8_t)((v >> 24) & 1);
        s_people[2] = (uint8_t)((v >> 25) & 1);
        s_people[3] = (uint8_t)((v >> 26) & 1);
        for (int i = 0; i < g->nopts && i < MESA_MAX_OPTS; i++) {
            int n = 0;
            while (g->opts[i].values[n]) n++;
            if (s_new.opts[i] >= n) s_new.opts[i] = g->opts[i].def;
        }
    }
    if (g->max_players < 2) s_mode = 0;
    s_np = g->players(s_new.opts);
    setup_build();
}

/* ---- the settings -------------------------------------------------------------------------- */

static void settings_build(void);
static const char *const SET_NAME[SET_N] = { N_("Madera"), N_("Mármol"), N_("Vidrio"), N_("Metal") };

static void ev_set(lv_event_t *e)
{
    (void)e;
    for (int k = 1; k <= SET_N; k++) {
        int s = (jm->prefs.set + k) % SET_N;
        if (s == SET_MADERA || jm_art_have_set(s)) {
            jm->prefs.set = s;
            break;
        }
    }
    settings_build();
}

static void ev_toggle(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    jm_prefs_t *p = &jm->prefs;
    switch (i) {
    case 0: p->flat = !p->flat; break;
    case 1: p->hints = !p->hints; break;
    case 2: p->coords = !p->coords; break;
    case 3: p->sound = !p->sound; break;
    case 4: p->speed = (p->speed + 1) % 3; break;
    default: break;
    }
    settings_build();
}

static void ev_settings_done(lv_event_t *e)
{
    (void)e;
    jm_prefs_save();
    sheet_close();
    if (jm->playing) {
        jm_play_relayout();
    } else {
        jm_home_show();
    }
}

static void settings_build(void)
{
    lv_obj_t *col = sheet_open(_("Ajustes"));
    jm_prefs_t *p = &jm->prefs;
    char b[48];
    bool others = false;
    for (int s = 1; s < SET_N; s++) others |= jm_art_have_set(s);
    snprintf(b, sizeof b, "%s", _(SET_NAME[p->set]));
    row(col, _("Piezas y tablero"), b, others ? ev_set : ev_set, 0);
    if (!others) {
        lv_obj_t *n = jm_label(col, _("Los juegos de mármol, vidrio y metal se bajan aparte desde la Tienda."),
                               aos_font_small, C(0x9C8C78));
        lv_obj_set_width(n, lv_pct(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    }
    row(col, _("Vista"), p->flat ? _("Plana") : _("En 3D"), ev_toggle, 0);
    row(col, _("Marcar las jugadas posibles"), p->hints ? _("Sí") : _("No"), ev_toggle, 1);
    row(col, _("Coordenadas"), p->coords ? _("Sí") : _("No"), ev_toggle, 2);
    row(col, _("Sonido"), p->sound ? _("Sí") : _("No"), ev_toggle, 3);
    static const char *const SP[3] = { N_("Lentas"), N_("Normales"), N_("Rápidas") };
    row(col, _("Las piezas se mueven"), _(SP[p->speed % 3]), ev_toggle, 4);
    lv_obj_t *ok = jm_button(col, _("Listo"), ev_settings_done, NULL);
    lv_obj_set_width(ok, lv_pct(100));
    lv_obj_set_style_bg_color(ok, AOS_C_ACCENT, 0);
    sheet_pad();
}

void jm_settings_open(void)
{
    settings_build();
}

/* ---- the clock on its own ------------------------------------------------------------------------ */

static int s_clk_preset = 4;
static lv_obj_t *s_clk_root, *s_clk_mid;

static void clk_reset(void)
{
    jm_clock_t *c = &jm->clk;
    int t = jm_clock_presets[s_clk_preset][0], inc = jm_clock_presets[s_clk_preset][1];
    if (t == 0) t = 300;
    memset(c, 0, sizeof *c);
    c->mode = t < 0 ? CLK_MOVE : inc ? (s_inc_kind ? CLK_BRONSTEIN : CLK_FISCHER) : CLK_SUDDEN;
    c->base_ms = (t < 0 ? -t : t) * 1000;
    c->inc_ms = inc * 1000;
    c->left[0] = c->left[1] = c->base_ms;
    c->running = -1;
    c->flagged = -1;
    jm->clk_moves_n[0] = jm->clk_moves_n[1] = 0;
}

static void clk_show(void)
{
    jm_clock_t *c = &jm->clk;
    for (int i = 0; i < 2; i++) {
        if (!jm->clk_time[i]) continue;
        char b[24];
        lv_label_set_text(jm->clk_time[i], jm_clock_text(c->left[i], b, sizeof b));
        snprintf(b, sizeof b, "%s: %d", _("Jugadas"), jm->clk_moves_n[i]);
        lv_label_set_text(jm->clk_moves[i], b);
        uint32_t bg = c->flagged == i ? 0x8A1E14 : c->running == i ? 0x2E6B3A : 0x221A13;
        lv_obj_set_style_bg_color(jm->clk_half[i], C(bg), 0);
    }
}

void jm_clock_tick(void)
{
    if (jm->screen != SCR_CLOCK) return;
    jm_clock_t *c = &jm->clk;
    uint32_t now = jm_now();
    if (c->running >= 0 && c->flagged < 0) {
        int32_t d = (int32_t)(now - c->last);
        c->last = now;
        c->left[c->running] -= d;
        c->spent += d;
        if (c->left[c->running] <= 0) {
            c->left[c->running] = 0;
            c->flagged = c->running;
            jm_audio_play(SND_LOSE);
        }
    }
    clk_show();
}

static void ev_half(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    jm_clock_t *c = &jm->clk;
    if (c->flagged >= 0) return;
    /* a tap on one's own half ends one's turn; the first tap starts the other */
    if (c->running < 0 || c->running == i) {
        if (c->running == i) {
            if (c->mode == CLK_FISCHER) c->left[i] += c->inc_ms;
            else if (c->mode == CLK_BRONSTEIN) c->left[i] += c->spent < c->inc_ms ? c->spent : c->inc_ms;
            else if (c->mode == CLK_MOVE) c->left[i] = c->base_ms;
            jm->clk_moves_n[i]++;
        }
        c->running = 1 - i;
        c->spent = 0;
        c->last = jm_now();
        jm_audio_play(SND_TAP);
    }
    clk_show();
}

static void ev_clk_pause(lv_event_t *e)
{
    (void)e;
    jm->clk.running = -1;
    clk_show();
}

static void ev_clk_reset(lv_event_t *e)
{
    (void)e;
    clk_reset();
    clk_show();
}

static void clk_build(void);

static void ev_clk_preset(lv_event_t *e)
{
    (void)e;
    s_clk_preset = s_clk_preset % (jm_nclock_presets - 1) + 1;
    clk_reset();
    clk_build();
}

static void ev_clk_back(lv_event_t *e)
{
    (void)e;
    jm_clock_free();
    jm_home_show();
}

static lv_obj_t *round_btn(lv_obj_t *p, const char *glyph, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 96, 96);
    lv_obj_set_style_radius(b, 48, 0);
    lv_obj_set_style_bg_color(b, C(0x3A2C20), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, glyph);
    lv_obj_set_style_text_font(l, &aos_sym_44, 0);
    lv_obj_set_style_text_color(l, C(0xF4E8D4), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void jm_clock_free(void)
{
    aos_pad_menu_clear(&jm->menu);
    if (s_clk_root) lv_obj_delete(s_clk_root);
    s_clk_root = s_clk_mid = NULL;
    memset(jm->clk_half, 0, sizeof jm->clk_half);
    memset(jm->clk_time, 0, sizeof jm->clk_time);
    memset(jm->clk_moves, 0, sizeof jm->clk_moves);
    if (jm->screen == SCR_CLOCK) jm->screen = SCR_HOME;
}

static void clk_build(void)
{
    jm_clock_free();
    jm->screen = SCR_CLOCK;
    lv_obj_t *r = lv_obj_create(jm->root);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, jm->W, jm->H);
    lv_obj_set_style_bg_color(r, C(0x0C0906), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    s_clk_root = r;
    int mid = 130;
    bool port = jm->portrait;
    int hw = port ? jm->W - 32 : (jm->W - mid - 48) / 2;
    int hh = port ? (jm->H - mid - 48) / 2 : jm->H - 32;
    for (int i = 0; i < 2; i++) {
        lv_obj_t *h = lv_button_create(r);
        lv_obj_remove_style_all(h);
        lv_obj_set_size(h, hw, hh);
        lv_obj_set_style_radius(h, 36, 0);
        lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
        if (port) lv_obj_set_pos(h, 16, i == 0 ? jm->H - 16 - hh : 16);
        else lv_obj_set_pos(h, i == 0 ? jm->W - 16 - hw : 16, 16);
        lv_obj_add_event_cb(h, ev_half, LV_EVENT_PRESSED, (void *)(intptr_t)i);
        lv_obj_t *t = lv_label_create(h);
        lv_obj_set_style_text_font(t, aos_font_huge, 0);
        lv_obj_set_style_text_color(t, C(0xFFFFFF), 0);
        lv_obj_center(t);
        lv_obj_t *m = lv_label_create(h);
        lv_obj_set_style_text_font(m, aos_font_body, 0);
        lv_obj_set_style_text_color(m, C(0xD8C8B0), 0);
        lv_obj_align(m, LV_ALIGN_BOTTOM_MID, 0, -30);
        jm->clk_half[i] = h;
        jm->clk_time[i] = t;
        jm->clk_moves[i] = m;
        if (i == 1 && port) {
            lv_obj_set_style_transform_pivot_x(h, hw / 2, 0);
            lv_obj_set_style_transform_pivot_y(h, hh / 2, 0);
            lv_obj_set_style_transform_rotation(h, 1800, 0);
        }
    }
    lv_obj_t *m = lv_obj_create(r);
    lv_obj_remove_style_all(m);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(m, port ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(m, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (port) {
        lv_obj_set_size(m, jm->W, mid);
        lv_obj_align(m, LV_ALIGN_CENTER, 0, 0);
    } else {
        lv_obj_set_size(m, mid, jm->H);
        lv_obj_align(m, LV_ALIGN_CENTER, 0, 0);
    }
    s_clk_mid = m;
    lv_obj_t *items[5];
    items[0] = round_btn(m, AOS_SYM_CHEVRON_LEFT, ev_clk_back);
    items[1] = round_btn(m, AOS_SYM_TIMER_OUTLINE, ev_clk_preset);
    items[2] = round_btn(m, AOS_SYM_RESTART, ev_clk_reset);
    items[3] = round_btn(m, AOS_SYM_LOCK, ev_clk_pause);
    char b[32];
    lv_obj_t *pl = jm_label(m, preset_text(s_clk_preset, b, sizeof b), aos_font_small, C(0xD8C8B0));
    (void)pl;
    aos_pad_menu_set(&jm->menu, items, 4, 1);
    clk_show();
}

void jm_clock_screen(void)
{
    if (jm_clock_presets[s_clk_preset][0] == 0) s_clk_preset = 4;
    if (jm->home) lv_obj_add_flag(jm->home, LV_OBJ_FLAG_HIDDEN);
    clk_reset();
    clk_build();
}

bool jm_setup_back(void)
{
    if (s_sheet) {
        sheet_close();
        if (!(s_from_game && jm->playing)) jm_home_show();
        return true;
    }
    if (s_clk_root) {
        jm_clock_free();
        jm_home_show();
        return true;
    }
    return false;
}

bool jm_setup_open_now(void)
{
    return s_sheet != NULL;
}
