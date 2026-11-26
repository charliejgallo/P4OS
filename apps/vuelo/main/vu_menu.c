/*
 * PLANEO - the screens: the title, the missions of each world with their
 * stars and records, the free flight (a world and an hour), the settings,
 * the pause and the results. LVGL panels over a still frame of the world
 * (the canvas, vuelo.c). The gamepad walks the buttons (aos_pad_menu.h):
 * A presses, B goes back.
 */
#include "vu_app.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <string.h>

const char *const VN_WORLD_NAME[VW_BIOMES] = { N_("Islas"), N_("Cañones"), N_("Nieve"), N_("Volcán"), N_("Ciudad") };
const char *const VN_HOUR_NAME[VW_HOURS] = { N_("Mañana"), N_("Mediodía"), N_("Atardecer"), N_("Noche") };
static const char *const KIND_NAME[VG_KINDS] = { N_("Anillos"), N_("Carrera"), N_("Entregas") };
static const char *const KIND_ABOUT[VG_KINDS] = {
    N_("Pasá por los anillos en orden, lo más rápido que puedas."),
    N_("Una carrera por los anillos contra otro avión."),
    N_("Pasá bajo por la luz verde a buscar cada paquete y llevalo a la naranja."),
};
static const char *const QUALITY_NAME[VA_QUALITIES] = { N_("Rápida"), N_("Normal"), N_("Fina") };

#define ACCENT  0xFF8A1E
#define PANEL   0x0E1A28
#define BTN     0x24384E
#define DIM     0x9AA8B8

/* --------------------------------------------------------------------------
 * Pieces
 * -------------------------------------------------------------------------- */

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_hex(PANEL), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_80, 0);
    lv_obj_set_style_radius(p, 28, 0);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t rgb)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    return l;
}

static void click_cue(lv_event_t *e)
{
    (void)e;
    vs_cue(VS_CLICK);
}

static lv_obj_t *button(lv_obj_t *parent, const char *txt, int w, int h, uint32_t rgb, lv_event_cb_t cb, app_t *a,
                        int tag)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, h / 2 < 24 ? h / 2 : 24, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3A4652), LV_STATE_DISABLED);
    lv_obj_set_user_data(b, (void *)(intptr_t)tag);
    if (txt) {
        lv_obj_t *l = label(b, txt, aos_font_body, 0xFFFFFF);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(l);
    }
    lv_obj_add_event_cb(b, click_cue, LV_EVENT_CLICKED, NULL);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, a);
    return b;
}

static int tag_of(lv_event_t *e)
{
    return (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
}

/* three stars, filled as many as earned */
static lv_obj_t *stars(lv_obj_t *parent, int n, const lv_font_t *font)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 3; i++) label(row, i < n ? AOS_SYM_STAR : AOS_SYM_STAR_OUTLINE, font, i < n ? 0xFFC83C : DIM);
    return row;
}

/* a star and a number beside it: the glyph font has no digits */
static lv_obj_t *star_count(lv_obj_t *parent, const char *txt)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    label(row, AOS_SYM_STAR, &aos_sym_28, 0xFFC83C);
    label(row, txt, aos_font_body, 0xFFFFFF);
    return row;
}

/* --------------------------------------------------------------------------
 * The screens
 * -------------------------------------------------------------------------- */

static void go_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    vn_show(a, tag_of(e));
}

static void build_main(app_t *a)
{
    lv_obj_t *p = panel(a->ui, 60, 70, 500, 580);
    lv_obj_t *t = label(p, "Planeo", &aos_inter_64, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 44, 36);
    lv_obj_t *s = label(p, _("Volá sobre mundos que el P4 dibuja en vivo"), aos_font_small, DIM);
    lv_obj_set_width(s, 420);
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 44, 122);
    char b[32];
    snprintf(b, sizeof b, "%d / %d", vg_total_stars(&a->save), VG_MISSIONS * 3);
    lv_obj_t *sc = star_count(p, b);
    lv_obj_align(sc, LV_ALIGN_TOP_LEFT, 44, 196);
    lv_obj_t *b1 = button(p, _("Misiones"), 412, 84, ACCENT, go_cb, a, VN_MISSIONS);
    lv_obj_align(b1, LV_ALIGN_TOP_LEFT, 44, 262);
    lv_obj_t *b2 = button(p, _("Vuelo libre"), 412, 84, BTN, go_cb, a, VN_FREE);
    lv_obj_align(b2, LV_ALIGN_TOP_LEFT, 44, 364);
    lv_obj_t *b3 = button(p, _("Ajustes"), 412, 72, BTN, go_cb, a, VN_SETTINGS);
    lv_obj_align(b3, LV_ALIGN_TOP_LEFT, 44, 466);
}

/* missions */

static void world_tab_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->menu_world = tag_of(e);
    vn_show(a, VN_MISSIONS);
}

static void mission_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    va_start_mission(a, tag_of(e));
}

static lv_obj_t *back_button(app_t *a, lv_obj_t *p, int screen)
{
    lv_obj_t *b = button(p, NULL, 72, 72, BTN, go_cb, a, screen);
    lv_obj_t *l = label(b, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, 0xFFFFFF);
    lv_obj_center(l);
    lv_obj_align(b, LV_ALIGN_TOP_LEFT, 28, 24);
    return b;
}

static void build_missions(app_t *a)
{
    lv_obj_t *p = panel(a->ui, 40, 30, 1200, 660);
    back_button(a, p, VN_MAIN);
    lv_obj_t *t = label(p, _("Misiones"), aos_font_title, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 124, 40);
    char b[64];
    snprintf(b, sizeof b, "%d / %d", vg_total_stars(&a->save), VG_MISSIONS * 3);
    lv_obj_t *sc = star_count(p, b);
    lv_obj_align(sc, LV_ALIGN_TOP_RIGHT, -40, 44);
    /* the worlds */
    int total = vg_total_stars(&a->save);
    for (int w = 0; w < VW_BIOMES; w++) {
        bool open = va_world_open(a, w);
        lv_obj_t *tb = button(p, NULL, 212, 74, w == a->menu_world ? ACCENT : BTN, world_tab_cb, a, w);
        lv_obj_align(tb, LV_ALIGN_TOP_LEFT, 40 + w * 226, 124);
        lv_obj_t *row = lv_obj_create(tb);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
        if (!open) label(row, AOS_SYM_LOCK, &aos_sym_28, DIM);
        label(row, _(VN_WORLD_NAME[w]), aos_font_body, open ? 0xFFFFFF : DIM);
        lv_obj_center(row);
    }
    int w = a->menu_world;
    if (!va_world_open(a, w)) {
        snprintf(b, sizeof b, _("Se abre con %d estrellas (tenés %d)."), vg_world_needs(w), total);
        lv_obj_t *l = label(p, b, aos_font_body, DIM);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 70);
        return;
    }
    /* its three missions */
    for (int k = 0; k < VG_KINDS; k++) {
        int m = w * VG_KINDS + k;
        int bi, hr;
        vg_mission_world(m, &bi, &hr);
        lv_obj_t *c = button(p, NULL, 360, 390, BTN, mission_cb, a, m);
        lv_obj_align(c, LV_ALIGN_TOP_LEFT, 40 + k * 380, 224);
        lv_obj_set_style_radius(c, 24, 0);
        lv_obj_t *n = label(c, _(KIND_NAME[k]), aos_font_title, 0xFFFFFF);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 14, 14);
        lv_obj_t *h = label(c, _(VN_HOUR_NAME[hr]), aos_font_small, 0xFFC83C);
        lv_obj_align(h, LV_ALIGN_TOP_LEFT, 14, 72);
        lv_obj_t *d = label(c, _(KIND_ABOUT[k]), aos_font_small, DIM);
        lv_obj_set_width(d, 318);
        lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(d, LV_ALIGN_TOP_LEFT, 14, 110);
        lv_obj_t *st = stars(c, a->save.stars[m], &aos_sym_44);
        lv_obj_align(st, LV_ALIGN_BOTTOM_LEFT, 10, -62);
        char tb[32];
        if (a->save.best_ms[m]) {
            char tt[24];
            vg_fmt_time(tt, sizeof tt, (float)a->save.best_ms[m] / 1000.0f);
            snprintf(tb, sizeof tb, _("Récord: %s"), tt);
        } else {
            snprintf(tb, sizeof tb, "%s", _("Sin récord"));
        }
        lv_obj_t *r = label(c, tb, aos_font_body, 0xFFFFFF);
        lv_obj_align(r, LV_ALIGN_BOTTOM_LEFT, 14, -16);
    }
}

/* the free flight */

static void free_world_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->free_world = tag_of(e);
    vn_show(a, VN_FREE);
}

static void free_hour_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->free_hour = tag_of(e);
    vn_show(a, VN_FREE);
}

static void free_go_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    va_start_free(a, a->free_world, a->free_hour);
}

static void build_free(app_t *a)
{
    lv_obj_t *p = panel(a->ui, 40, 30, 1200, 660);
    back_button(a, p, VN_MAIN);
    lv_obj_t *t = label(p, _("Vuelo libre"), aos_font_title, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 124, 40);
    lv_obj_t *s = label(p, _("Sin reloj ni anillos: elegí un mundo y una hora, y volá."), aos_font_small, DIM);
    lv_obj_align(s, LV_ALIGN_TOP_LEFT, 124, 92);
    lv_obj_t *l1 = label(p, _("Mundo"), aos_font_body, 0xFFFFFF);
    lv_obj_align(l1, LV_ALIGN_TOP_LEFT, 40, 160);
    /* every world, from the start: the free flight is for looking around
     * (the user's ask, 2026-11-25); the missions still open them one by one */
    if (a->free_world < 0 || a->free_world >= VW_BIOMES) a->free_world = 0;
    for (int w = 0; w < VW_BIOMES; w++) {
        lv_obj_t *b = button(p, _(VN_WORLD_NAME[w]), 212, 74, w == a->free_world ? ACCENT : BTN, free_world_cb, a, w);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 40 + w * 226, 206);
    }
    lv_obj_t *l2 = label(p, _("Hora"), aos_font_body, 0xFFFFFF);
    lv_obj_align(l2, LV_ALIGN_TOP_LEFT, 40, 316);
    for (int h = 0; h < VW_HOURS; h++) {
        lv_obj_t *b = button(p, _(VN_HOUR_NAME[h]), 270, 74, h == a->free_hour ? ACCENT : BTN, free_hour_cb, a, h);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 40 + h * 284, 362);
    }
    lv_obj_t *go = button(p, _("Volar"), 420, 90, ACCENT, free_go_cb, a, 0);
    lv_obj_align(go, LV_ALIGN_BOTTOM_MID, 0, -44);
}

/* the settings */

static void set_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int tag = tag_of(e);
    int what = tag / 10, v = tag % 10;
    switch (what) {
    case 0: a->cfg.sound = v; break;
    case 1: a->cfg.invert = v; break;
    case 2:
        if (v != a->cfg.quality) {
            va_quality(a, v);
            /* the menu's picture at the new size */
            __atomic_store_n(&a->still_req, 1, __ATOMIC_RELEASE);
        }
        break;
    case 3: a->cfg.view = v; break;
    default: a->cfg.fps = v; break;
    }
    va_cfg_store(a);
    vn_show(a, VN_SETTINGS);
}

static void setting_row(app_t *a, lv_obj_t *p, int y, const char *name, int what, int cur, const char *const *opts,
                        int nopt, int w)
{
    lv_obj_t *l = label(p, name, aos_font_body, 0xFFFFFF);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 40, y + 22);
    for (int i = 0; i < nopt; i++) {
        lv_obj_t *b = button(p, _(opts[i]), w, 70, i == cur ? ACCENT : BTN, set_cb, a, what * 10 + i);
        lv_obj_align(b, LV_ALIGN_TOP_LEFT, 330 + i * (w + 14), y);
    }
}

static void build_settings(app_t *a)
{
    lv_obj_t *p = panel(a->ui, 40, 30, 1200, 660);
    back_button(a, p, VN_MAIN);
    lv_obj_t *t = label(p, _("Ajustes"), aos_font_title, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 124, 40);
    static const char *const ONOFF[2] = { N_("Apagado"), N_("Prendido") };
    static const char *const STICK[2] = { N_("Arriba sube"), N_("Arriba baja") };
    static const char *const VIEW[2] = { N_("Detrás"), N_("Cabina") };
    setting_row(a, p, 130, _("Sonido"), 0, a->cfg.sound, ONOFF, 2, 220);
    setting_row(a, p, 220, _("Palanca"), 1, a->cfg.invert, STICK, 2, 300);
    setting_row(a, p, 310, _("Calidad"), 2, a->cfg.quality, QUALITY_NAME, VA_QUALITIES, 240);
    setting_row(a, p, 400, _("Cámara"), 3, a->cfg.view, VIEW, 2, 220);
    setting_row(a, p, 490, _("Cuadros por segundo"), 4, a->cfg.fps, ONOFF, 2, 220);
    lv_obj_t *n = label(p, _("\"Arriba baja\" es como en un avión: tirar hacia vos levanta la nariz. La calidad "
                             "fina dibuja más puntos y va más lenta."),
                        aos_font_small, DIM);
    lv_obj_set_width(n, 1100);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 40, -28);
}

/* the pause */

static void resume_cb(lv_event_t *e)
{
    va_resume((app_t *)lv_event_get_user_data(e));
}

static void restart_cb(lv_event_t *e)
{
    va_restart((app_t *)lv_event_get_user_data(e));
}

static void menu_cb(lv_event_t *e)
{
    va_to_menu((app_t *)lv_event_get_user_data(e));
}

static void build_pause(app_t *a)
{
    lv_obj_t *p = panel(a->ui, (a->SW - 520) / 2, (a->SH - 470) / 2, 520, 470);
    lv_obj_t *t = label(p, _("Pausa"), aos_font_title, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 34);
    lv_obj_t *b1 = button(p, _("Seguir"), 420, 84, ACCENT, resume_cb, a, 0);
    lv_obj_align(b1, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_t *b2 = button(p, _("Empezar de nuevo"), 420, 76, BTN, restart_cb, a, 0);
    lv_obj_align(b2, LV_ALIGN_TOP_MID, 0, 212);
    if (a->mission < 0) lv_obj_add_flag(b2, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *b3 = button(p, _("Salir al menú"), 420, 76, BTN, menu_cb, a, 0);
    lv_obj_align(b3, LV_ALIGN_TOP_MID, 0, a->mission < 0 ? 212 : 306);
}

/* the results */

static void next_cb(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    va_start_mission(a, tag_of(e));
}

static void build_result(app_t *a)
{
    const vg_game_t *g = &a->game;
    lv_obj_t *p = panel(a->ui, (a->SW - 640) / 2, (a->SH - 560) / 2, 640, 560);
    const char *title = g->kind == VG_RACE ? (g->won ? _("¡Ganaste la carrera!") : _("Te ganó por poco"))
                                           : _("¡Misión cumplida!");
    lv_obj_t *t = label(p, title, aos_font_title, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);
    int m = a->mission;
    char b[96];
    if (m >= 0) {
        snprintf(b, sizeof b, "%s · %s", _(VN_WORLD_NAME[m / VG_KINDS]), _(KIND_NAME[m % VG_KINDS]));
        lv_obj_t *s = label(p, b, aos_font_small, DIM);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, 86);
    }
    char tt[24];
    vg_fmt_time(tt, sizeof tt, a->result_time);
    lv_obj_t *tl = label(p, tt, &aos_inter_64, 0xFFFFFF);
    lv_obj_align(tl, LV_ALIGN_TOP_MID, 0, 120);
    lv_obj_t *st = stars(p, a->result_stars, &aos_sym_72);
    lv_obj_align(st, LV_ALIGN_TOP_MID, 0, 206);
    if (a->result_record) {
        lv_obj_t *r = label(p, _("¡Récord nuevo!"), aos_font_body, 0xFFC83C);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 294);
    } else if (a->result_stars < 3) {
        /* what the next star asks */
        char want[24];
        if (g->kind == VG_RACE) {
            if (!g->won) snprintf(b, sizeof b, "%s", _("Ganale para la segunda estrella."));
            else {
                vg_fmt_time(want, sizeof want, g->par[2]);
                snprintf(b, sizeof b, _("Tercera estrella: ganale por seis segundos (%s)."), want);
            }
        } else {
            vg_fmt_time(want, sizeof want, g->par[a->result_stars >= 2 ? 2 : 1]);
            snprintf(b, sizeof b, _("Próxima estrella: %s"), want);
        }
        lv_obj_t *r = label(p, b, aos_font_small, DIM);
        lv_obj_set_width(r, 560);
        lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(r, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_align(r, LV_ALIGN_TOP_MID, 0, 294);
    }
    lv_obj_t *b1 = button(p, _("Otra vez"), 180, 78, BTN, restart_cb, a, 0);
    lv_obj_align(b1, LV_ALIGN_BOTTOM_LEFT, 28, -30);
    int next = m + 1;
    lv_obj_t *b2 = button(p, _("Siguiente"), 200, 78, ACCENT, next_cb, a, next);
    lv_obj_align(b2, LV_ALIGN_BOTTOM_MID, 0, -30);
    if (m < 0 || next >= VG_MISSIONS || !va_world_open(a, next / VG_KINDS)) lv_obj_add_state(b2, LV_STATE_DISABLED);
    lv_obj_t *b3 = button(p, _("Menú"), 180, 78, BTN, menu_cb, a, 0);
    lv_obj_align(b3, LV_ALIGN_BOTTOM_RIGHT, -28, -30);
}

/* --------------------------------------------------------------------------
 * Showing them
 * -------------------------------------------------------------------------- */

void vn_close(app_t *a)
{
    if (a->ui) lv_obj_delete(a->ui);
    a->ui = NULL;
    a->screen = VN_NONE;
    aos_pad_menu_clear(&a->pad_menu);
}

void vn_show(app_t *a, int screen)
{
    if (a->ui) lv_obj_delete(a->ui);
    a->ui = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->ui);
    lv_obj_set_size(a->ui, a->SW, a->SH);
    lv_obj_remove_flag(a->ui, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    a->screen = screen;
    aos_pad_menu_clear(&a->pad_menu);
    switch (screen) {
    case VN_MISSIONS: build_missions(a); break;
    case VN_FREE: build_free(a); break;
    case VN_SETTINGS: build_settings(a); break;
    case VN_PAUSE: build_pause(a); break;
    case VN_RESULT: build_result(a); break;
    default: build_main(a); break;
    }
    /* the loading box stays on top */
    if (a->load_box) lv_obj_move_foreground(a->load_box);
}

static void gather(lv_obj_t *o, lv_obj_t **it, int *n)
{
    uint32_t c = lv_obj_get_child_count(o);
    for (uint32_t i = 0; i < c && *n < AOS_PAD_MENU_MAX; i++) {
        lv_obj_t *ch = lv_obj_get_child(o, (int32_t)i);
        if (lv_obj_has_flag(ch, LV_OBJ_FLAG_HIDDEN)) continue;
        if (lv_obj_check_type(ch, &lv_button_class)) it[(*n)++] = ch;
        else gather(ch, it, n);
    }
}

void vn_pad(app_t *a)
{
    if (!a->pad.connected || !a->ui) return;
    lv_obj_t *it[AOS_PAD_MENU_MAX];
    int n = 0;
    gather(a->ui, it, &n);
    bool same = n == a->pad_menu.n;
    for (int i = 0; i < n && same; i++) same = it[i] == a->pad_menu.item[i];
    if (!same) aos_pad_menu_set(&a->pad_menu, it, n, a->pad_menu.sel < n ? a->pad_menu.sel : 0);
    if (aos_pad_menu_step(&a->pad_menu, &a->pad)) return;
    if (aos_pad_pressed(&a->pad, AOS_PAD_B)) {
        if (a->state == VA_PAUSE) va_resume(a);
        else if (a->state == VA_RESULT) va_to_menu(a);
        else if (a->screen != VN_MAIN) vn_show(a, VN_MAIN);
    } else if (aos_pad_pressed(&a->pad, AOS_PAD_START) && a->state == VA_PAUSE) {
        va_resume(a);
    }
}
