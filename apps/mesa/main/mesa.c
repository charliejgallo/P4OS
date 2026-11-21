/*
 * P4OS - JUEGOS DE MESA: board games in 3D, against the board or two (or
 * four) at the same screen.
 *
 * Six games come with the app: chess (the whole rules, an engine of eight
 * levels, the score sheet, PGN files and a clock), draughts (ours on 8 x 8
 * with the flying king, and the international ones on 10 x 10), reversi,
 * four in a row, ludo for two to four, and backgammon. Each is a table
 * (mesa_game.h); more come as modules on the card, each with its art.
 *
 * The boards and the pieces were modelled by code in Blender and rendered
 * by a fixed, tilted orthographic camera (tools/blender), so one sprite of
 * a piece serves every cell. The table is composed by Bastion's tile
 * compositor (mg_gfx.c) into the buffer of an LVGL canvas that fills the
 * screen, so LVGL turns it with the screen and draws the buttons over it;
 * only the tiles that changed are drawn again. The board thinks on a
 * thread of its own (mg_play.c).
 *
 * Files: mg_app.h has the list. The art is in packs (mg_art.c): the wooden
 * set comes with the app, marble, glass and metal are packs apart.
 *
 * Development switches (simulator; on the board getenv() returns NULL):
 *   MESA_GAME=<word>    straight into a new game of that game (ajedrez...)
 *   MESA_PEOPLE=2       ... two people (else against the board)
 *   MESA_LEVEL=<n>      ... the board's level
 *   MESA_FEN=<fen>      ... from that position (chess)
 *   MESA_MOVES=<n>      ... and the board plays n moves of both sides first
 */
#include "mg_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

mg_app_t *mg;

/* keys the games ask the host to translate (mesa_host_t.tr) and that no
 * _() of the app names: here so the catalogue has them */
static const char *const EXTRA_KEYS[] = { N_("damas"), N_("fichas"), N_("en casa"), N_("afuera") };

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

/* ---- the host the games get ------------------------------------------------------------- */

static void host_log(const char *fmt, ...)
{
    char b[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    aos_hal_log("mesa", "%s", b);
}

static uint32_t host_now(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

static const mesa_host_t HOST = { MESA_ABI, host_log, aos_tr, mg_malloc, mg_free, host_now };
const mesa_host_t *mesa_host = &HOST;

uint32_t mg_now(void)
{
    return lv_tick_get();
}

static uint32_t s_rng;

uint32_t mg_rand(void)
{
    if (!s_rng) {
        aos_hal_random(&s_rng, sizeof s_rng);
        if (!s_rng) s_rng = 0x1234567u;
    }
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return s_rng = x;
}

/* ---- little helpers ---------------------------------------------------------------------- */

lv_obj_t *mg_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *mg_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_height(b, 84);
    lv_obj_set_style_radius(b, 42, 0);
    lv_obj_set_style_bg_color(b, C(0x3A2C20), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C(0x5A4430), LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_t *l = mg_label(b, text, aos_font_body, C(0xF4E8D4));
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

/* ---- preferences -------------------------------------------------------------------------- */

static void prefs_load(void)
{
    mg_prefs_t *p = &mg->prefs;
    p->set = SET_MADERA;
    p->flat = false;
    p->hints = true;
    p->coords = true;
    p->sound = true;
    p->speed = 1;
    int32_t v;
    if (aos_hal_pref_get_i32("mg_prefs", &v)) {
        p->set = v & 3;
        p->flat = (v >> 2) & 1;
        p->hints = (v >> 3) & 1;
        p->coords = (v >> 4) & 1;
        p->sound = (v >> 5) & 1;
        p->speed = (v >> 6) & 3;
        if (p->speed > 2) p->speed = 1;
    }
    if (p->set != SET_MADERA && !mg_art_have_set(p->set)) p->set = SET_MADERA;
}

void mg_prefs_save(void)
{
    const mg_prefs_t *p = &mg->prefs;
    int32_t v = (p->set & 3) | (p->flat << 2) | (p->hints << 3) | (p->coords << 4) | (p->sound << 5) |
                ((p->speed & 3) << 6);
    aos_hal_pref_set_i32("mg_prefs", v);
}

/* ---- the home screen --------------------------------------------------------------------- */

static void ev_game(lv_event_t *e)
{
    mg_setup_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void ev_resume(lv_event_t *e)
{
    (void)e;
    mg_new_t n;
    if (!mg_save_load(&n, true)) {
        aos_ui_toast(_("No se pudo abrir la partida"), 2000);
        mg_home_show();
    }
}

static void ev_clock(lv_event_t *e)
{
    (void)e;
    mg_clock_screen();
}

static void ev_settings(lv_event_t *e)
{
    (void)e;
    mg_settings_open();
}

static lv_obj_t *game_card(lv_obj_t *grid, int gi, int w)
{
    const mesa_game_t *g = mg_games[gi].g;
    lv_obj_t *c = lv_button_create(grid);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, w * 3 / 4 + 96);
    lv_obj_set_style_radius(c, 26, 0);
    lv_obj_set_style_bg_color(c, C(0x241B14), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(c, C(0x3A2C20), LV_STATE_PRESSED);
    lv_obj_set_style_clip_corner(c, true, 0);
    lv_obj_add_event_cb(c, ev_game, LV_EVENT_CLICKED, (void *)(intptr_t)gi);
    if (mg->thumb_ok[gi]) {
        lv_obj_t *im = lv_image_create(c);
        lv_image_set_src(im, &mg->thumb[gi]);
        lv_obj_set_size(im, w, w * 3 / 4);
        lv_image_set_inner_align(im, LV_IMAGE_ALIGN_COVER);
        lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_remove_flag(im, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t *ph = lv_obj_create(c);
        lv_obj_remove_style_all(ph);
        lv_obj_set_size(ph, w, w * 3 / 4);
        lv_obj_set_style_bg_color(ph, C(0x6A4A2C), 0);
        lv_obj_set_style_bg_grad_color(ph, C(0x2A1A0E), 0);
        lv_obj_set_style_bg_grad_dir(ph, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
        lv_obj_remove_flag(ph, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t *n = mg_label(c, _(g->name), aos_font_body, C(0xF4E8D4));
    lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 18, -48);
    lv_obj_t *a = mg_label(c, _(g->about), aos_font_tiny, C(0xB8A890));
    lv_obj_set_width(a, w - 30);
    lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(a, LV_ALIGN_BOTTOM_LEFT, 18, -18);
    return c;
}

static lv_obj_t *wide_btn(lv_obj_t *p, const char *glyph, const char *text, const char *sub, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, lv_pct(100), 110);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, C(0x241B14), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, C(0x3A2C20), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *g = lv_label_create(b);
    lv_label_set_text(g, glyph);
    lv_obj_set_style_text_font(g, &aos_sym_44, 0);
    lv_obj_set_style_text_color(g, AOS_C_ACCENT, 0);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 26, 0);
    lv_obj_t *t = mg_label(b, text, aos_font_body, C(0xF4E8D4));
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 100, sub ? -16 : 0);
    if (sub) {
        lv_obj_t *s = mg_label(b, sub, aos_font_small, C(0xB8A890));
        lv_obj_set_width(s, lv_pct(70));
        lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(s, LV_ALIGN_LEFT_MID, 100, 20);
    }
    return b;
}

void mg_home_build(void)
{
    aos_pad_menu_clear(&mg->menu);
    if (mg->home) lv_obj_delete(mg->home);
    lv_obj_t *h = lv_obj_create(mg->root);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, mg->W, mg->H);
    lv_obj_set_style_bg_color(h, C(0x120D09), 0);
    lv_obj_set_style_bg_grad_color(h, C(0x24170D), 0);
    lv_obj_set_style_bg_grad_dir(h, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(h, 44, 0);
    lv_obj_set_style_pad_bottom(h, 60, 0);
    lv_obj_set_style_pad_hor(h, 28, 0);
    lv_obj_set_style_pad_row(h, 22, 0);
    lv_obj_set_flex_flow(h, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(h, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(h, LV_DIR_VER);
    mg->home = h;
    lv_obj_t *t = mg_label(h, _("Juegos de mesa"), aos_font_title, C(0xF4E8D4));
    (void)t;
    lv_obj_t *items[AOS_PAD_MENU_MAX];
    int ni = 0;
    mg_new_t saved;
    if (mg_save_load(&saved, false) && saved.gi >= 0 && saved.gi < mg_ngames) {
        char sub[80];
        snprintf(sub, sizeof sub, "%s", _(mg_games[saved.gi].g->name));
        items[ni++] = wide_btn(h, AOS_SYM_RESTART, _("Seguir la partida"), sub, ev_resume);
    }
    lv_obj_t *grid = lv_obj_create(h);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 22, 0);
    lv_obj_set_style_pad_column(grid, 22, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    int cols = mg->portrait ? 2 : 3;
    int w = (mg->W - 56 - 22 * (cols - 1)) / cols;
    for (int i = 0; i < mg_ngames && ni < AOS_PAD_MENU_MAX - 3; i++) items[ni++] = game_card(grid, i, w);
    items[ni++] = wide_btn(h, AOS_SYM_TIMER_OUTLINE, _("Reloj de ajedrez"), _("Para una partida en un tablero de verdad"), ev_clock);
    items[ni++] = wide_btn(h, AOS_SYM_COG, _("Ajustes"), NULL, ev_settings);
    if (!mg_art_have_set(SET_MADERA)) {
        lv_obj_t *w2 = mg_label(h, _("Falta el paquete de arte mesa_p4.pak: los juegos se ven con piezas de reemplazo."),
                                aos_font_small, C(0xFF9F0A));
        lv_obj_set_width(w2, lv_pct(100));
        lv_label_set_long_mode(w2, LV_LABEL_LONG_MODE_WRAP);
    }
    aos_pad_menu_set(&mg->menu, items, ni, 0);
    aos_pad_reset(&mg->pad, lv_tick_get());
}

void mg_home_show(void)
{
    mg->screen = SCR_HOME;
    if (mg->view.canvas) lv_obj_add_flag(mg->view.canvas, LV_OBJ_FLAG_HIDDEN);
    mg_home_build();
}

/* ---- the timer ------------------------------------------------------------------------------ */

bool mg_setup_back(void);
bool mg_setup_open_now(void);

static void perf(void)
{
    uint32_t now = mg_now();
    if (!mg->perf_t0) mg->perf_t0 = now;
    if (now - mg->perf_t0 < 5000) return;
    if (mg->perf_frames && mg->playing) {
        aos_hal_log("mesa", "%u frames in %u ms, drawing %u us a frame, %u tiles a frame, %u KB held",
                    (unsigned)mg->perf_frames, (unsigned)(now - mg->perf_t0),
                    (unsigned)(mg->perf_us / mg->perf_frames), (unsigned)(mg->perf_tiles / mg->perf_frames),
                    (unsigned)(mg_mem_used() / 1024));
    }
    mg->perf_t0 = now;
    mg->perf_frames = mg->perf_us = mg->perf_tiles = 0;
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!mg) return;
    aos_pad_update(&mg->pad, lv_tick_get());
    if (mg_setup_open_now() || mg->screen != SCR_PLAY) {
        if (aos_pad_pressed(&mg->pad, AOS_PAD_B)) mg_setup_back();
        else aos_pad_menu_step(&mg->menu, &mg->pad);
    } else if (mg->playing) {
        if (mg->button_pause) {
            mg->button_pause = false;
            if (!mg->panel) mg_hud_pause();
        }
        mg_hud_pad();
    }
    if (mg->screen == SCR_CLOCK) mg_clock_tick();
    if (mg->playing) {
        mg_play_tick();
        if (mg->status_until && (int32_t)(mg_now() - mg->status_until) >= 0) mg_hud_refresh();
        static uint32_t s_clk;
        if (mg->play.clock.mode != CLK_NONE && mg_now() - s_clk > 100) {
            s_clk = mg_now();
            mg_hud_clock();
        }
        if (mg->screen == SCR_PLAY) mg_view_frame();
    }
    mg_audio_pump();
    perf();
}

/* ---- the app's life -------------------------------------------------------------------------- */

static void dev_switches(void)
{
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    const char *word = getenv("MESA_GAME");
    if (!word) return;
    const mg_slot_t *s = mg_game_find(word);
    if (!s) return;
    mg_new_t n;
    memset(&n, 0, sizeof n);
    n.gi = (int)(s - mg_games);
    for (int i = 0; i < s->g->nopts && i < MESA_MAX_OPTS; i++) n.opts[i] = s->g->opts[i].def;
    const char *o = getenv("MESA_OPT");
    if (o) n.opts[0] = (uint8_t)atoi(o);
    int np = s->g->players(n.opts);
    const char *pp = getenv("MESA_PEOPLE");
    int people = pp ? atoi(pp) : 1;
    const char *lv = getenv("MESA_LEVEL");
    for (int i = 0; i < np; i++) {
        n.human[i] = i < people;
        n.level[i] = (uint8_t)(lv ? atoi(lv) : 3);
    }
    const char *tw = getenv("MESA_TWO");
    n.two = tw ? atoi(tw) : TWO_FACE;
    const char *ck = getenv("MESA_CLOCK");
    if (ck) {
        n.clock_mode = CLK_FISCHER;
        n.clock_base = atoi(ck) * 1000;
        n.clock_inc = 2000;
    }
    n.fen = getenv("MESA_FEN");
    if (mg->home) lv_obj_add_flag(mg->home, LV_OBJ_FLAG_HIDDEN);
    mg_play_start(&n);
    const char *mv = getenv("MESA_MOVES");
    int k = mv ? atoi(mv) : 0;
    /* both sides played by the board, at once, to reach a position */
    for (int i = 0; i < k && !mg->play.over; i++) {
        mg_play_t *p = &mg->play;
        if (p->g->need_roll && p->g->need_roll(p->st) > 0) {
            mg_hist_t h;
            memset(&h, 0, sizeof h);
            h.roll = 1;
            h.rnd = mg_rand();
            p->g->roll(p->st, h.rnd);
            p->hist[p->n++] = h;
            p->top = p->n;
            i--;
            continue;
        }
        int nm = p->g->moves(p->st, p->moves, MESA_MAX_MOVES);
        if (nm <= 0) break;
        mesa_ai_t ai;
        memset(&ai, 0, sizeof ai);
        ai.now_ms = host_now;
        ai.ms = 150;
        ai.seed = mg_rand();
        mesa_move_t m = p->moves[mg_rand() % (uint32_t)nm];
        if (p->g->ai_move) p->g->ai_move(p->st, 2, &ai, &m);
        mg_hist_t h;
        memset(&h, 0, sizeof h);
        h.m = m;
        p->g->apply(p->st, &m);
        p->hist[p->n++] = h;
        p->top = p->n;
    }
    if (k) {
        mg_play_ai_cancel();
        mg_play_replay(mg->play.n);
        mg_view_sync(false);
        mg_hud_refresh();
        mg_hud_sheet_refresh();
        mg->play.wait_until = mg_now() + 500;
    }
#endif
}

static void *mesa_create(aos_app_t *self, lv_obj_t *root)
{
    (void)EXTRA_KEYS;
    mg = (mg_app_t *)calloc(1, sizeof *mg);
    if (!mg) return NULL;
    mg->self = self;
    mg->root = root;
    lv_obj_update_layout(root);
    mg->W = lv_obj_get_width(root);
    mg->H = lv_obj_get_height(root);
    mg->portrait = mg->H >= mg->W;
    lv_obj_set_style_bg_color(root, C(0x120D09), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    prefs_load();
    mg_games_scan();
    if (!mg_view_open()) {
        free(mg);
        mg = NULL;
        return NULL;
    }
    for (int i = 0; i < mg_ngames; i++) mg->thumb_ok[i] = mg_art_thumb(mg_games[i].g, &mg->thumb[i]);
    mg_audio_open();
    mg_home_show();
    mg->timer = lv_timer_create(timer_cb, MG_FRAME_MS, NULL);
    dev_switches();
    return mg;
}

static void mesa_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!mg) return;
    if (mg->timer) lv_timer_delete(mg->timer);
    mg->timer = NULL;
    if (mg->playing) mg_play_stop(true);
    mg_play_ai_cancel();
    /* a thought that did not come back still has the game's code: wait */
    for (int i = 0; i < 300 && mg->play.ai_busy; i++) aos_hal_sleep_ms(10);
    mg_clock_free();
    if (mg->root) lv_obj_clean(mg->root);
    mg->home = NULL;
    mg->view.canvas = NULL;
    mg->hud = NULL;
    mg->panel = NULL;
    mg_view_close();
    mg_art_free(&mg->art);
    mg_audio_close();
    for (int i = 0; i < mg_ngames; i++) {
        if (mg->thumb_ok[i]) mg_free((void *)mg->thumb[i].data);
    }
    mg_games_free();
    free(mg);
    mg = NULL;
}

static bool mesa_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    if (!mg) return false;
    lv_obj_update_layout(root);
    mg->W = lv_obj_get_width(root);
    mg->H = lv_obj_get_height(root);
    mg->portrait = mg->H >= mg->W;
    bool sheet = mg_setup_open_now();
    if (sheet) mg_setup_back();
    if (mg->screen == SCR_CLOCK) {
        mg_clock_screen();
        return true;
    }
    if (mg->playing) {
        int panel = mg->panel_kind;
        mg_hud_panel_close();
        mg_play_relayout();
        if (panel == PANEL_PAUSE) mg_hud_pause();
        else if (panel == PANEL_END) mg_hud_end();
        return true;
    }
    mg_home_show();
    return true;
}

static bool mesa_back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (!mg) return false;
    if (mg_setup_back()) return true;
    if (mg->playing && mg->screen == SCR_PLAY) return mg_hud_back();
    return false;
}

static void mesa_hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (mg && mg->playing) mg_save_game();
}

static bool mesa_button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    (void)inst;
    if (!mg || !mg->playing || action != AOS_BUTTON_CLICK) return false;
    mg->button_pause = true;
    return true;
}

/* a board in two woods and a pawn standing on it */
static const uint8_t MESA_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 6, 64, 52, 8, AIC_C_LIT(0x6B4426), 255),
    AIC_GRAD(AIC_C_LIT(0x3E2412), AIC_GRAD_VER),
    AIC_RECT(AIC_CENTER, -12, -3, 22, 16, 2, AIC_C_LIT(0xE9CFA0), 255),
    AIC_RECT(AIC_CENTER, 12, 15, 22, 16, 2, AIC_C_LIT(0xE9CFA0), 255),
    AIC_RECT(AIC_CENTER, 8, 20, 30, 8, AIC_CIRCLE, AIC_C_LIT(0x1A0E06), 120),
    AIC_RECT(AIC_CENTER, 8, 8, 24, 22, 8, AIC_C_LIT(0xF4F0E6), 255),
    AIC_GRAD(AIC_C_LIT(0xB8AE9C), AIC_GRAD_HOR),
    AIC_RECT(AIC_CENTER, 8, -10, 14, 16, 5, AIC_C_LIT(0xF4F0E6), 255),
    AIC_RECT(AIC_CENTER, 8, -22, 16, 16, AIC_CIRCLE, AIC_C_LIT(0xFFFFFF), 255),
    AIC_GRAD(AIC_C_LIT(0xC8BEAC), AIC_GRAD_HOR),
    AIC_END
};

static bool mesa_init(aos_app_t *app)
{
    app->desc.id       = "demo.mesa";
    app->desc.name     = "Juegos de mesa";
    app->desc.icon     = LV_SYMBOL_PLAY;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x7A4E2A;
    app->desc.color_b  = 0x2A170A;
    app->desc.order    = 160;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN;
    aos_icon_set_ops(app, MESA_ICON, sizeof MESA_ICON);
    app->create  = mesa_create;
    app->destroy = mesa_destroy;
    app->resize  = mesa_resize;
    app->back    = mesa_back;
    app->hide    = mesa_hide;
    app->button  = mesa_button;
    return true;
}

AOS_APP_ENTRY(mesa_init);
