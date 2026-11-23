/*
 * DERIVA - the menus (LVGL), over a still of a sector:
 *   the title (the campaign, going on or new; the arcade, alone or two;
 *   the settings), the settings, how to play, and the workshop between
 *   the campaign's sectors. Every control is a button, so the gamepad's
 *   frame (aos_pad_menu) reaches all of them.
 */
#include "ast_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>

#define ACCENT 0x5CF2FF

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

lv_obj_t *ast_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, C(col), 0);
    return l;
}

static void btn_style(lv_obj_t *b, bool primary)
{
    lv_obj_set_style_bg_color(b, C(primary ? 0x0C3A48 : 0x10141C), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_80, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, C(primary ? ACCENT : 0x3A4658), 0);
    lv_obj_set_style_text_color(b, C(primary ? 0xD8FCFF : 0xE8ECF2), 0);
    lv_obj_set_style_bg_color(b, C(0x1E6A7A), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(b, primary ? 18 : 0, 0);
    lv_obj_set_style_shadow_color(b, C(ACCENT), 0);
    lv_obj_set_style_shadow_opa(b, LV_OPA_40, 0);
}

lv_obj_t *ast_button(lv_obj_t *parent, const char *text, bool primary, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    btn_style(b, primary);
    lv_obj_set_style_text_font(b, aos_font_body, 0);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, w - 32);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

/* the buttons of a container (and of its rows), for the gamepad's frame */
static void collect(lv_obj_t *o, lv_obj_t **items, int *n, int max)
{
    uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt && *n < max; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_check_type(c, &lv_button_class)) items[(*n)++] = c;
        else collect(c, items, n, max);
    }
}

static void pad_menu(ast_app_t *a, lv_obj_t *o, int sel)
{
    lv_obj_t *items[24];
    int n = 0;
    collect(o, items, &n, 24);
    aos_pad_menu_set(&a->menu, items, n, sel);
}

void ast_menu_free(ast_app_t *a)
{
    aos_pad_menu_clear(&a->menu);
    if (a->scr) lv_obj_delete(a->scr);
    a->scr = NULL;
}

/* ---- the title ----------------------------------------------------------------------------- */

static void ev_continue(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ast_audio_play(SND_UI, 200);
    ast_game_start(a, AG_CAMPAIGN, 1, true);
}

static void ev_new(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ast_audio_play(SND_UI, 200);
    ast_game_start(a, AG_CAMPAIGN, 1, false);
}

static void ev_arcade(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ast_audio_play(SND_UI, 200);
    ast_game_start(a, AG_ARCADE, 1, false);
}

static void ev_two(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ast_audio_play(SND_UI, 200);
    aos_gamepad_t p;
    if (!aos_hal_hid_gamepad_get(0, &p) && !aos_hal_hid_gamepad_get(1, &p)) {
        aos_ui_toast(_("El segundo jugador juega con un joystick USB: enchufá uno"), 3500);
        return;
    }
    ast_game_start(a, AG_ARCADE, 2, false);
}

static void ev_settings(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ast_audio_play(SND_UI, 200);
    ast_menu_show(a, SC_SETTINGS);
}

static void ev_help(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ast_menu_show(a, SC_HELP);
}

static void ev_back(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    ast_audio_play(SND_UI, 200);
    ast_menu_show(a, SC_TITLE);
}

static void title(ast_app_t *a)
{
    const ast_layout_t *L = &a->lay;
    lv_obj_t *col = plain(a->scr);
    int w = L->land ? 500 : 520;
    int bh = L->land ? 70 : 84;
    lv_obj_set_size(col, w + 40, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, L->land ? 10 : 16, 0);
    lv_obj_set_style_pad_all(col, 10, 0);
    if (L->land) lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 176);
    else lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 640);
    char line[96];
    bool saved = a->cfg.camp_sector > 0 && !a->cfg.camp_won;
    if (saved) {
        snprintf(line, sizeof line, _("Seguir: sector %d"), a->cfg.camp_sector);
        ast_button(col, line, true, w, bh, ev_continue, a);
    }
    ast_button(col, saved ? _("Campaña nueva") : _("Campaña"), !saved, w, bh, ev_new, a);
    ast_button(col, _("Arcade"), false, w, bh, ev_arcade, a);
    ast_button(col, _("Arcade de a dos"), false, w, bh, ev_two, a);
    ast_button(col, _("Ajustes"), false, w, bh, ev_settings, a);
    if (a->cfg.best[0] > 0) {
        snprintf(line, sizeof line, _("Récord del arcade: %d (oleada %d)"), a->cfg.best[0], a->cfg.best_wave[0]);
        lv_obj_t *l = ast_label(col, line, aos_font_small, 0x9AB0C4);
        lv_obj_set_style_pad_top(l, 6, 0);
    }
    pad_menu(a, col, 0);
}

/* ---- the settings ---------------------------------------------------------------------------- */

static const char *diff_name(int d)
{
    return d == AG_EASY ? _("Fácil") : d == AG_HARD ? _("Difícil") : _("Normal");
}

static void setting_text(ast_app_t *a, int which, char *s, size_t n)
{
    const ast_cfg_t *c = &a->cfg;
    switch (which) {
    case 0: snprintf(s, n, _("Dificultad: %s"), diff_name(c->diff)); break;
    case 1: snprintf(s, n, "%s", c->controls == CTL_STICK ? _("Controles: palanca") : _("Controles: botones")); break;
    case 2: snprintf(s, n, "%s", c->lefty ? _("Disparo a la izquierda") : _("Disparo a la derecha")); break;
    case 3: snprintf(s, n, "%s", c->sfx ? _("Sonido: sí") : _("Sonido: no")); break;
    case 4: snprintf(s, n, "%s", c->music ? _("Música: sí") : _("Música: no")); break;
    case 5: snprintf(s, n, "%s", c->shake ? _("Sacudidas: sí") : _("Sacudidas: no")); break;
    default: s[0] = 0; break;
    }
}

static void ev_setting(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    int which = (int)(intptr_t)lv_obj_get_user_data(b);
    ast_cfg_t *c = &a->cfg;
    switch (which) {
    case 0: c->diff = (c->diff + 1) % AG_DIFF_N; break;
    case 1: c->controls = c->controls == CTL_STICK ? CTL_BUTTONS : CTL_STICK; break;
    case 2:
        c->lefty = !c->lefty;
        ast_layout(a, a->lay.W, a->lay.H);
        break;
    case 3: c->sfx = !c->sfx; break;
    case 4: c->music = !c->music; break;
    case 5: c->shake = !c->shake; break;
    default: break;
    }
    ast_audio_set(c->sfx, c->music);
    ast_audio_play(SND_UI, 200);
    ast_cfg_save(c);
    char s[96];
    setting_text(a, which, s, sizeof s);
    lv_label_set_text(lv_obj_get_child(b, 0), s);
}

static void ev_forget(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    a->cfg.camp_sector = 0;
    a->cfg.camp_max = 0;
    a->cfg.camp_won = 0;
    ast_cfg_save(&a->cfg);
    aos_ui_toast(_("La campaña vuelve a empezar"), 2000);
}

static lv_obj_t *sheet(ast_app_t *a, const char *title_text)
{
    lv_obj_t *dim = plain(a->scr);
    lv_obj_set_size(dim, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(dim, C(0x02040A), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_80, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = ast_label(dim, title_text, aos_font_title, ACCENT);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, a->lay.land ? 24 : 80);
    return dim;
}

static void settings(ast_app_t *a)
{
    lv_obj_t *dim = sheet(a, _("Ajustes"));
    bool land = a->lay.land;
    lv_obj_t *box = plain(dim);
    int w = land ? 500 : 560;
    lv_obj_set_size(box, land ? 2 * w + 60 : w + 40, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 14, 0);
    lv_obj_set_style_pad_column(box, 20, 0);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, land ? 96 : 190);
    for (int i = 0; i < 6; i++) {
        char s[96];
        setting_text(a, i, s, sizeof s);
        lv_obj_t *b = ast_button(box, s, false, w, 76, ev_setting, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
    }
    ast_button(box, _("Cómo se juega"), false, w, 76, ev_help, a);
    ast_button(box, _("Empezar la campaña de cero"), false, w, 76, ev_forget, a);
    ast_button(box, _("Volver"), true, w, 76, ev_back, a);
    pad_menu(a, box, 0);
}

/* ---- how to play ------------------------------------------------------------------------------ */

static void help(ast_app_t *a)
{
    lv_obj_t *dim = sheet(a, _("Cómo se juega"));
    bool land = a->lay.land;
    lv_obj_t *box = lv_obj_create(dim);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, land ? 1100 : 640, land ? 500 : 960);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, land ? 90 : 170);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 14, 0);
    static const char *const LINES[] = {
        N_("Con un pulgar en la palanca la nave gira hacia donde apuntás; llevada más allá de la mitad, además empuja. "
           "Sin nada que la frene, la nave sigue de largo: para parar hay que darse vuelta y empujar."),
        N_("Con el otro pulgar: cualquier lugar de ese lado dispara, y el botón redondo es el salto, que te lleva a un "
           "lugar despejado y tarda en volver a cargarse."),
        N_("Las rocas comunes se parten en dos, las violetas en tres. Las de metal aguantan varios tiros. Las naranjas "
           "explotan y se llevan lo que tienen cerca. Las verdes se te pegan: cada una te frena y tres te aplastan; "
           "un salto o un escudo te las sacan."),
        N_("Los enemigos y las rocas grandes dejan mejoras: disparo triple, escudo, rayo e imán. Los cristales se "
           "juntan pasando cerca y en la campaña se gastan en el taller entre sectores."),
        N_("La campaña son doce sectores, con un jefe cada cuatro. El arcade son oleadas sin fin y el récord. Con dos "
           "joysticks USB, o uno más la pantalla, juegan dos."),
        N_("Joystick: la palanca o la cruz para girar (arriba empuja), A dispara, B salta, START pausa."),
    };
    for (size_t i = 0; i < sizeof LINES / sizeof LINES[0]; i++) {
        lv_obj_t *l = ast_label(box, _(LINES[i]), aos_font_small, 0xDDE6F0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
    }
    lv_obj_t *row = plain(dim);
    lv_obj_set_size(row, 400, 90);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, land ? -8 : -40);
    lv_obj_t *b = ast_button(row, _("Volver"), true, 360, 76, ev_settings, a);
    lv_obj_center(b);
    pad_menu(a, row, 0);
}

void ast_menu_show(ast_app_t *a, int screen)
{
    ast_menu_free(a);
    a->screen = screen;
    a->scr = plain(a->root);
    lv_obj_set_size(a->scr, lv_pct(100), lv_pct(100));
    switch (screen) {
    case SC_SETTINGS: settings(a); break;
    case SC_HELP: help(a); break;
    default: title(a); break;
    }
    aos_pad_reset(&a->pad, lv_tick_get());
}

void ast_menu_pad(ast_app_t *a)
{
    if (!a->scr) return;
    if (a->screen != SC_TITLE && aos_pad_pressed(&a->pad, AOS_PAD_B)) {
        ast_menu_show(a, a->screen == SC_HELP ? SC_SETTINGS : SC_TITLE);
        return;
    }
    aos_pad_menu_step(&a->menu, &a->pad);
}

bool ast_menu_back(ast_app_t *a)
{
    if (a->screen == SC_HELP) {
        ast_menu_show(a, SC_SETTINGS);
        return true;
    }
    if (a->screen != SC_TITLE) {
        ast_menu_show(a, SC_TITLE);
        return true;
    }
    return false;
}

/* ---- the workshop ------------------------------------------------------------------------------- */

static const char *const UP_NAME[UP_N] = { N_("Motor"), N_("Cañón"), N_("Blindaje"), N_("Salto"), N_("Imán") };
static const char *const UP_WHAT[UP_N] = {
    N_("empuja más y llega más rápido"),
    N_("dispara más seguido y más lejos"),
    N_("cada sector empieza con escudo"),
    N_("el salto carga antes"),
    N_("los cristales vienen de más lejos"),
};
#define LIFE_COST 60

static void shop_refresh(ast_app_t *a, lv_obj_t *p);

static void up_text(ast_app_t *a, int u, char *s, size_t n)
{
    const ag_game_t *g = a->game;
    if (u == UP_N) {
        snprintf(s, n, _("Nave extra (%d)  ·  %d"), g->ship[0].lives, LIFE_COST);
        return;
    }
    int lv = g->upg[u], mx = ag_upg_max(u);
    if (lv >= mx) snprintf(s, n, "%s %d/%d  ·  %s", _(UP_NAME[u]), lv, mx, _("completo"));
    else snprintf(s, n, "%s %d/%d  ·  %d", _(UP_NAME[u]), lv, mx, ag_upg_cost(u, lv));
}

static void ev_buy(lv_event_t *e)
{
    ast_app_t *a = (ast_app_t *)lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    int u = (int)(intptr_t)lv_obj_get_user_data(b);
    ag_game_t *g = a->game;
    int cost = u == UP_N ? (g->ship[0].lives < 6 ? LIFE_COST : 0) : ag_upg_cost(u, g->upg[u]);
    if (!cost) return;
    if (g->crystals < cost) {
        aos_ui_toast(_("Faltan cristales"), 1500);
        return;
    }
    g->crystals -= cost;
    if (u == UP_N) {
        for (int s = 0; s < g->players; s++) {
            if (g->ship[s].in_play) g->ship[s].lives++;
        }
    } else {
        g->upg[u]++;
    }
    ast_audio_play(SND_PICK, 220);
    /* the button, its row, the list, the panel */
    shop_refresh(a, lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b))));
}

static void shop_refresh(ast_app_t *a, lv_obj_t *p)
{
    const ag_game_t *g = a->game;
    lv_obj_t *head = lv_obj_get_child(p, 1);
    char s[96];
    snprintf(s, sizeof s, _("Cristales: %d"), g->crystals);
    lv_label_set_text(head, s);
    lv_obj_t *list = lv_obj_get_child(p, 2);
    for (int u = 0; u <= UP_N; u++) {
        lv_obj_t *row = lv_obj_get_child(list, u);
        lv_obj_t *b = lv_obj_get_child(row, 0);
        up_text(a, u, s, sizeof s);
        lv_label_set_text(lv_obj_get_child(b, 0), s);
        int cost = u == UP_N ? LIFE_COST : ag_upg_cost(u, g->upg[u]);
        bool can = cost && cost <= g->crystals;
        lv_obj_set_style_border_color(b, C(can ? 0x6CFF8E : 0x3A4658), 0);
    }
}

void ast_shop_fill(ast_app_t *a, lv_obj_t *p)
{
    bool land = a->lay.land;
    ast_label(p, _("Taller"), aos_font_title, ACCENT);
    ast_label(p, "", aos_font_body, 0xB8FBFF);
    lv_obj_t *list = plain(p);
    int w = land ? 520 : 560;
    lv_obj_set_size(list, land ? 2 * w + 30 : w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(list, land ? 6 : 10, 0);
    lv_obj_set_style_pad_column(list, 30, 0);
    for (int u = 0; u <= UP_N; u++) {
        lv_obj_t *row = plain(list);
        lv_obj_set_size(row, w, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *b = ast_button(row, "", false, w, land ? 62 : 70, ev_buy, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)u);
        lv_obj_t *l = ast_label(row, u == UP_N ? _("una vida más para lo que queda") : _(UP_WHAT[u]), aos_font_caption,
                                0x8A9AAA);
        lv_obj_set_style_pad_left(l, 30, 0);
        lv_obj_set_style_pad_top(l, 2, 0);
    }
    shop_refresh(a, p);
}
