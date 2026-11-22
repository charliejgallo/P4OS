/*
 * HOCKEY - the menus (LVGL), over a still of the table:
 *   the title (against the board, two players, settings),
 *   the match (the table, to N goals or by time, the board's level) and
 *   the settings. Every control is a button, so the gamepad's frame
 *   (aos_pad_menu) reaches all of them. Choosing another table shows it
 *   behind the menu.
 */
#include "hk_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <stdio.h>
#include <string.h>

#define ACCENT 0xE8443A

static lv_color_t C(uint32_t rgb)
{
    return lv_color_hex(rgb);
}

static uint32_t mixc(uint32_t a, uint32_t b, int k)
{
    int r = (int)((a >> 16) & 255) + (((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * k >> 8);
    int g = (int)((a >> 8) & 255) + (((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * k >> 8);
    int bb = (int)(a & 255) + (((int)(b & 255) - (int)(a & 255)) * k >> 8);
    return (uint32_t)((r << 16) | (g << 8) | bb);
}

lv_obj_t *hk_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, C(col), 0);
    return l;
}

static void btn_style(lv_obj_t *b, bool primary)
{
    uint32_t base = primary ? ACCENT : 0x22252F;
    lv_obj_set_style_bg_color(b, C(base), 0);
    lv_obj_set_style_bg_grad_color(b, C(mixc(base, 0x000000, 90)), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, C(primary ? 0xFFB0A8 : 0x464B5C), 0);
    lv_obj_set_style_text_color(b, C(primary ? 0x200806 : 0xFFFFFF), 0);
    lv_obj_set_style_bg_color(b, C(mixc(base, 0xFFFFFF, 60)), LV_STATE_PRESSED);
}

lv_obj_t *hk_button(lv_obj_t *parent, const char *text, bool primary, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 18, 0);
    btn_style(b, primary);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_font(b, aos_font_body, 0);
    lv_obj_set_style_pad_all(b, 4, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(l, w - 24);
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

void hk_menu_free(hk_app_t *a)
{
    aos_pad_menu_clear(&a->menu);
    if (a->scr) lv_obj_delete(a->scr);
    a->scr = NULL;
    a->m_table = a->m_format = a->m_value = NULL;
    for (int i = 0; i < HK_DIFF_N; i++) a->m_diff[i] = NULL;
}

/* the buttons of the screen, in the order they were made, for the gamepad */
static void collect(lv_obj_t *o, lv_obj_t **items, int *n, int max)
{
    uint32_t cnt = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < cnt && *n < max; i++) {
        lv_obj_t *c = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_check_type(c, &lv_button_class)) items[(*n)++] = c;
        else collect(c, items, n, max);
    }
}

static void pad_items(hk_app_t *a, int sel)
{
    lv_obj_t *items[32];
    int n = 0;
    collect(a->scr, items, &n, 32);
    aos_pad_menu_set(&a->menu, items, n, sel < n ? sel : 0);
}

static lv_obj_t *screen_new(hk_app_t *a, int dim)
{
    hk_menu_free(a);
    lv_obj_t *s = plain(a->root);
    lv_obj_set_size(s, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s, C(0x05060A), 0);
    lv_obj_set_style_bg_opa(s, (lv_opa_t)dim, 0);
    a->scr = s;
    return s;
}

static lv_obj_t *card(lv_obj_t *s, int y, int h)
{
    lv_obj_t *c = plain(s);
    lv_obj_set_size(c, 640, h);
    lv_obj_set_pos(c, 40, y);
    lv_obj_set_style_radius(c, 28, 0);
    lv_obj_set_style_bg_color(c, C(0x0B0C11), 0);
    lv_obj_set_style_bg_opa(c, 215, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, C(0x2A2E3A), 0);
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return c;
}

static void header(hk_app_t *a, lv_obj_t *s, const char *title, lv_event_cb_t back)
{
    lv_obj_t *b = hk_button(s, LV_SYMBOL_LEFT, false, 80, 68, back, a);
    lv_obj_set_pos(b, 24, 40);
    lv_obj_t *l = hk_label(s, title, aos_font_title, 0xFFFFFF);
    lv_obj_set_pos(l, 124, 52);
}

/* ---- the title -------------------------------------------------------------------------------------------- */

static void ev_cpu(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    a->cfg.mode = MODE_CPU;
    hk_menu_show(a, SC_SETUP);
}

static void ev_two(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    a->cfg.mode = MODE_TWO;
    hk_menu_show(a, SC_SETUP);
}

static void ev_settings(lv_event_t *e)
{
    hk_menu_show((hk_app_t *)lv_event_get_user_data(e), SC_SETTINGS);
}

static void ev_title(lv_event_t *e)
{
    hk_menu_show((hk_app_t *)lv_event_get_user_data(e), SC_TITLE);
}

static void title(hk_app_t *a)
{
    lv_obj_t *s = screen_new(a, 110);
    lv_obj_t *band = plain(s);
    lv_obj_set_size(band, lv_pct(100), 250);
    lv_obj_set_pos(band, 0, 150);
    lv_obj_set_style_bg_color(band, C(0x05060A), 0);
    lv_obj_set_style_bg_opa(band, 200, 0);
    lv_obj_t *t = hk_label(band, _("Hockey de mesa"), aos_font_huge, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_t *st = hk_label(band, _("Air hockey en 3D"), aos_font_title, ACCENT);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_t *c = card(s, 720, LV_SIZE_CONTENT);
    hk_button(c, _("Contra la placa"), true, 560, 96, ev_cpu, a);
    hk_button(c, _("Dos jugadores"), false, 560, 84, ev_two, a);
    hk_button(c, _("Ajustes"), false, 560, 84, ev_settings, a);
    if (!a->art_ok) {
        lv_obj_t *w = hk_label(c, _("Falta el paquete del juego: bajalo de la Tienda"), aos_font_small, 0xFF8070);
        lv_obj_set_width(w, 560);
        lv_label_set_long_mode(w, LV_LABEL_LONG_MODE_WRAP);
    }
    pad_items(a, 0);
}

/* ---- the match's setup ------------------------------------------------------------------------------------ */

static void setup_refresh(hk_app_t *a);

static void set_btn_text(lv_obj_t *b, const char *t)
{
    if (b) lv_label_set_text(lv_obj_get_child(b, 0), t);
}

static void ev_table(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    if (a->art.ntb <= 0) return;
    int i = hk_table_find(&a->art, a->cfg.table);
    i = (i + 1) % a->art.ntb;
    snprintf(a->cfg.table, sizeof a->cfg.table, "%s", a->art.tb[i].id);
    hk_cfg_save(&a->cfg);
    setup_refresh(a);
    hk_still(a);
}

static void ev_format(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    a->cfg.format = a->cfg.format == FMT_TIME ? FMT_GOALS : FMT_TIME;
    setup_refresh(a);
}

static void ev_value(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    if (a->cfg.format == FMT_TIME) {
        a->cfg.minutes = a->cfg.minutes >= 10 ? 1 : a->cfg.minutes + 1;
    } else {
        static const int G[4] = { 3, 5, 7, 10 };
        int k = 0;
        while (k < 4 && G[k] != a->cfg.goals) k++;
        a->cfg.goals = G[(k + 1) % 4];
    }
    setup_refresh(a);
}

static void ev_diff(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    a->cfg.diff = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    setup_refresh(a);
}

static void ev_play(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    hk_cfg_save(&a->cfg);
    hk_match_start(a);
}

static const char *const DIFF[HK_DIFF_N] = { N_("fácil"), N_("normal"), N_("difícil"), N_("crack") };

static void setup_refresh(hk_app_t *a)
{
    char line[96];
    int i = hk_table_find(&a->art, a->cfg.table);
    if (i < 0 && a->art.ntb) i = 0;
    if (i >= 0) {
        const hk_table_t *t = &a->art.tb[i];
        snprintf(line, sizeof line, _("Mesa: %s"), t->name[hk_lang()][0] ? t->name[hk_lang()] : t->name[0]);
        set_btn_text(a->m_table, line);
        if (a->m_table) lv_obj_set_style_border_color(a->m_table, C(t->accent), 0);
    }
    set_btn_text(a->m_format, a->cfg.format == FMT_TIME ? _("Por tiempo") : _("A goles"));
    if (a->cfg.format == FMT_TIME) snprintf(line, sizeof line, _("%d minutos"), a->cfg.minutes);
    else snprintf(line, sizeof line, _("El primero a %d"), a->cfg.goals);
    set_btn_text(a->m_value, line);
    for (int d = 0; d < HK_DIFF_N; d++) {
        if (a->m_diff[d]) btn_style(a->m_diff[d], d == a->cfg.diff);
    }
}

static void setup(hk_app_t *a)
{
    lv_obj_t *s = screen_new(a, 90);
    header(a, s, a->cfg.mode == MODE_CPU ? _("Contra la placa") : _("Dos jugadores"), ev_title);
    lv_obj_t *c = card(s, 560, LV_SIZE_CONTENT);
    a->m_table = hk_button(c, "", false, 580, 84, ev_table, a);
    lv_obj_set_style_border_width(a->m_table, 3, 0);
    if (a->art.ntb < 3) {
        lv_obj_t *w = hk_label(c, _("Más mesas: bajalas de la Tienda"), aos_font_caption, 0x9098A8);
        (void)w;
    }
    lv_obj_t *row = plain(c);
    lv_obj_set_size(row, 580, 76);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    a->m_format = hk_button(row, "", false, 220, 76, ev_format, a);
    a->m_value = hk_button(row, "", false, 348, 76, ev_value, a);
    lv_obj_set_style_text_font(lv_obj_get_child(a->m_format, 0), aos_font_small, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(a->m_value, 0), aos_font_small, 0);
    if (a->cfg.mode == MODE_CPU) {
        hk_label(c, _("La placa juega en"), aos_font_small, 0xB8BECC);
        lv_obj_t *dr = plain(c);
        lv_obj_set_size(dr, 580, 70);
        lv_obj_set_flex_flow(dr, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(dr, 10, 0);
        for (int d = 0; d < HK_DIFF_N; d++) {
            a->m_diff[d] = hk_button(dr, _(DIFF[d]), d == a->cfg.diff, 137, 70, ev_diff, a);
            lv_obj_set_user_data(a->m_diff[d], (void *)(intptr_t)d);
            lv_obj_set_style_text_font(lv_obj_get_child(a->m_diff[d], 0), aos_font_small, 0);
        }
    } else {
        lv_obj_t *l = hk_label(c, _("Cada uno juega con un dedo en su mitad"), aos_font_small, 0xB8BECC);
        lv_obj_set_width(l, 580);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    hk_button(c, _("¡A jugar!"), true, 580, 100, ev_play, a);
    setup_refresh(a);
    pad_items(a, 0);
}

/* ---- the settings ------------------------------------------------------------------------------------------ */

typedef struct {
    const char *name;
    size_t off;
    int n;
    const char *val[4];
} opt_t;

static const opt_t OPTS[] = {
    { N_("Dificultad"), offsetof(hk_cfg_t, diff), 4, { N_("fácil"), N_("normal"), N_("difícil"), N_("crack") } },
    { N_("Paleta"), offsetof(hk_cfg_t, paddle), 3, { N_("chica"), N_("mediana"), N_("grande") } },
    { N_("Disco"), offsetof(hk_cfg_t, speed), 3, { N_("lento"), N_("normal"), N_("rápido") } },
    { N_("Sonido"), offsetof(hk_cfg_t, sfx), 2, { N_("no"), N_("sí") } },
    { N_("Ruido del aire"), offsetof(hk_cfg_t, air), 2, { N_("no"), N_("sí") } },
};
#define NOPTS ((int)(sizeof OPTS / sizeof OPTS[0]))

static void opt_text(const hk_app_t *a, int i, char *s, size_t n)
{
    int v = *(const int *)((const char *)&a->cfg + OPTS[i].off);
    if (v < 0 || v >= OPTS[i].n) v = 0;
    snprintf(s, n, "%s: %s", _(OPTS[i].name), _(OPTS[i].val[v]));
}

static void ev_opt(lv_event_t *e)
{
    hk_app_t *a = (hk_app_t *)lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(b);
    int *v = (int *)((char *)&a->cfg + OPTS[i].off);
    *v = (*v + 1) % OPTS[i].n;
    hk_audio_set(a->cfg.sfx, a->cfg.air);
    hk_cfg_save(&a->cfg);
    char line[96];
    opt_text(a, i, line, sizeof line);
    set_btn_text(b, line);
    /* a new size of mallet shows behind */
    if (OPTS[i].off == offsetof(hk_cfg_t, paddle)) hk_still(a);
}

static void settings(hk_app_t *a)
{
    lv_obj_t *s = screen_new(a, 150);
    header(a, s, _("Ajustes"), ev_title);
    lv_obj_t *c = card(s, 180, LV_SIZE_CONTENT);
    for (int i = 0; i < NOPTS; i++) {
        char line[96];
        opt_text(a, i, line, sizeof line);
        lv_obj_t *b = hk_button(c, line, false, 580, 86, ev_opt, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    }
    pad_items(a, 0);
}

/* ---- the switchboard -------------------------------------------------------------------------------------- */

void hk_menu_show(hk_app_t *a, int screen)
{
    a->screen = screen;
    a->state = a->state == ST_MATCH ? ST_MATCH : ST_MENU;
    if (screen == SC_SETUP) setup(a);
    else if (screen == SC_SETTINGS) settings(a);
    else title(a);
    aos_pad_reset(&a->pad, lv_tick_get());
}

void hk_menu_pad(hk_app_t *a)
{
    if (!a->scr) return;
    if (aos_pad_pressed(&a->pad, AOS_PAD_B)) {
        hk_menu_back(a);
        return;
    }
    if (a->screen == SC_SETUP && aos_pad_pressed(&a->pad, AOS_PAD_START)) {
        hk_match_start(a);
        return;
    }
    aos_pad_menu_step(&a->menu, &a->pad);
}

bool hk_menu_back(hk_app_t *a)
{
    if (a->screen == SC_TITLE) return false;
    hk_menu_show(a, SC_TITLE);
    return true;
}
