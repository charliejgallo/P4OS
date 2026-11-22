/*
 * METEGOL - the menus (LVGL), over a still of the table:
 *   the title (against the board, two players, settings),
 *   the match (league, the two teams from a grid, the stadium, by time or
 *   to N goals) and the settings. Every control is a button, so the
 *   gamepad's frame (aos_pad_menu) reaches all of them.
 */
#include "mg_app.h"

#include "aos_fonts.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"

#include <stdio.h>
#include <string.h>

#define ACCENT 0x2FBF5A

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

lv_obj_t *mg_label(lv_obj_t *parent, const char *text, const lv_font_t *f, uint32_t col)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, C(col), 0);
    return l;
}

lv_obj_t *mg_button(lv_obj_t *parent, const char *text, bool primary, int w, int h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 18, 0);
    uint32_t base = primary ? ACCENT : 0x1C2A22;
    lv_obj_set_style_bg_color(b, C(base), 0);
    lv_obj_set_style_bg_grad_color(b, C(mixc(base, 0x000000, 90)), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, C(primary ? 0xA8F0BE : 0x3E5C48), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_text_color(b, C(primary ? 0x08200F : 0xFFFFFF), 0);
    lv_obj_set_style_text_font(b, aos_font_body, 0);
    lv_obj_set_style_bg_color(b, C(mixc(base, 0xFFFFFF, 60)), LV_STATE_PRESSED);
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

uint8_t *mg_badge_image(const mg_team_t *t, int w, int h, lv_image_dsc_t *d)
{
    uint8_t *px = (uint8_t *)mg_malloc((size_t)w * h * 2);
    if (!px) return NULL;
    mg_kit_badge(t, (uint16_t *)px, w, h);
    memset(d, 0, sizeof *d);
    d->header.magic = LV_IMAGE_HEADER_MAGIC;
    d->header.cf = LV_COLOR_FORMAT_RGB565;
    d->header.w = (uint32_t)w;
    d->header.h = (uint32_t)h;
    d->header.stride = (uint32_t)w * 2;
    d->data = px;
    d->data_size = (uint32_t)(w * h * 2);
    return px;
}

void mg_menu_free(mg_app_t *a)
{
    aos_pad_menu_clear(&a->menu);
    if (a->scr) lv_obj_delete(a->scr);
    a->scr = NULL;
    for (int i = 0; i < MG_TEAMS_MAX; i++) {
        mg_free(a->badge_px[i]);
        a->badge_px[i] = NULL;
    }
    for (int i = 0; i < 2; i++) {
        mg_free(a->big_px[i]);
        a->big_px[i] = NULL;
    }
    a->m_home = a->m_away = a->m_home_img = a->m_away_img = a->m_stadium = a->m_format = a->m_value = NULL;
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

static void pad_items(mg_app_t *a, int sel)
{
    lv_obj_t *items[64];
    int n = 0;
    collect(a->scr, items, &n, 64);
    aos_pad_menu_set(&a->menu, items, n, sel < n ? sel : 0);
}

static lv_obj_t *screen_new(mg_app_t *a, int dim)
{
    mg_menu_free(a);
    lv_obj_t *s = plain(a->root);
    lv_obj_set_size(s, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s, C(0x040806), 0);
    lv_obj_set_style_bg_opa(s, (lv_opa_t)dim, 0);
    a->scr = s;
    return s;
}

static void header(mg_app_t *a, lv_obj_t *s, const char *title, lv_event_cb_t back)
{
    lv_obj_t *b = mg_button(s, LV_SYMBOL_LEFT, false, 76, 64, back, a);
    lv_obj_set_pos(b, 20, 16);
    lv_obj_t *l = mg_label(s, title, aos_font_title, 0xFFFFFF);
    lv_obj_set_pos(l, 112, 26);
}

/* ---- the title -------------------------------------------------------------------------------------------- */

static void ev_cpu(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    a->cfg.mode = MODE_CPU;
    mg_menu_show(a, SC_SETUP);
}

static void ev_two(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    a->cfg.mode = MODE_TWO;
    mg_menu_show(a, SC_SETUP);
}

static void ev_settings(lv_event_t *e)
{
    mg_menu_show((mg_app_t *)lv_event_get_user_data(e), SC_SETTINGS);
}

static void ev_title(lv_event_t *e)
{
    mg_menu_show((mg_app_t *)lv_event_get_user_data(e), SC_TITLE);
}

static void title(mg_app_t *a)
{
    lv_obj_t *s = screen_new(a, 150);
    lv_obj_t *band = plain(s);
    lv_obj_set_size(band, 560, lv_pct(100));
    lv_obj_set_style_bg_color(band, C(0x040806), 0);
    lv_obj_set_style_bg_opa(band, 200, 0);
    lv_obj_t *t = mg_label(band, "Metegol", aos_font_huge, 0xFFFFFF);
    lv_obj_set_pos(t, 60, 150);
    lv_obj_t *st = mg_label(band, _("Fútbol de mesa"), aos_font_title, ACCENT);
    lv_obj_set_pos(st, 64, 240);
    lv_obj_t *col = plain(band);
    lv_obj_set_size(col, 440, LV_SIZE_CONTENT);
    lv_obj_set_pos(col, 60, 330);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 16, 0);
    mg_button(col, _("Contra la placa"), true, 440, 84, ev_cpu, a);
    mg_button(col, _("Dos jugadores"), false, 440, 76, ev_two, a);
    mg_button(col, _("Ajustes"), false, 440, 76, ev_settings, a);
    if (!a->art_ok) {
        lv_obj_t *w = mg_label(band, _("Falta el paquete del juego: bajalo de la Tienda"), aos_font_small, 0xFF8070);
        lv_obj_set_pos(w, 60, 640);
    }
    pad_items(a, 0);
}

/* ---- the match's setup ------------------------------------------------------------------------------------ */

static void setup_refresh(mg_app_t *a);

static void ev_league(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    int lg = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (lg == a->cfg.league) return;
    a->cfg.league = lg;
    int list[MG_TEAMS_MAX];
    int n = mg_teams_of(&a->teams, lg, list, MG_TEAMS_MAX);
    for (int t = 0; t < 2 && n > t; t++) snprintf(a->cfg.team[t], sizeof a->cfg.team[t], "%s", a->teams.t[list[t]].id);
    a->cfg.stadium[0] = 0;
    a->pick_slot = 0;
    mg_menu_show(a, SC_SETUP);
}

static void ev_team(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    int other = mg_teams_find(&a->teams, a->cfg.team[a->pick_slot ^ 1]);
    if (i == other) {
        /* the same team on both sides: swap them */
        char keep[sizeof a->cfg.team[0]];
        memcpy(keep, a->cfg.team[a->pick_slot], sizeof keep);
        memcpy(a->cfg.team[a->pick_slot ^ 1], keep, sizeof keep);
    }
    snprintf(a->cfg.team[a->pick_slot], sizeof a->cfg.team[0], "%s", a->teams.t[i].id);
    a->pick_slot ^= 1;
    setup_refresh(a);
}

static void ev_slot(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    a->pick_slot = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    setup_refresh(a);
}

/* the stadiums a league may play in, and the one the setup shows */
static int st_ok(const mg_app_t *a, int i)
{
    int want = a->cfg.league == MG_LEAGUE_CLUB ? MG_ST_CLUBS : MG_ST_NATIONS;
    return a->art.st[i].league == MG_ST_BOTH || a->art.st[i].league == want;
}

static void ev_stadium(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    int cur = a->cfg.stadium[0] ? mg_stadium_find(&a->art, a->cfg.stadium) : -1;
    /* "the home side's", then each one the league has */
    for (int k = 0; k <= a->art.nst; k++) {
        cur++;
        if (cur >= a->art.nst) {
            a->cfg.stadium[0] = 0;
            break;
        }
        if (st_ok(a, cur)) {
            snprintf(a->cfg.stadium, sizeof a->cfg.stadium, "%s", a->art.st[cur].id);
            break;
        }
    }
    setup_refresh(a);
}

static void ev_format(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    a->cfg.format = a->cfg.format == FMT_TIME ? FMT_GOALS : FMT_TIME;
    setup_refresh(a);
}

static void ev_value(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
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

static void ev_play(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    mg_cfg_save(&a->cfg);
    mg_match_start(a);
}

static void set_btn_text(lv_obj_t *b, const char *t)
{
    if (b) lv_label_set_text(lv_obj_get_child(b, 0), t);
}

static void setup_refresh(mg_app_t *a)
{
    for (int t = 0; t < 2; t++) {
        int i = mg_teams_find(&a->teams, a->cfg.team[t]);
        if (i < 0) continue;
        lv_obj_t *name = t ? a->m_away : a->m_home;
        lv_obj_t *img = t ? a->m_away_img : a->m_home_img;
        if (name) lv_label_set_text(name, a->teams.t[i].name);
        mg_free(a->big_px[t]);
        a->big_px[t] = mg_badge_image(&a->teams.t[i], 120, 80, &a->big_dsc[t]);
        if (img && a->big_px[t]) lv_image_set_src(img, &a->big_dsc[t]);
        if (a->m_slot[t]) {
            bool on = a->pick_slot == t;
            lv_obj_set_style_border_color(a->m_slot[t], C(on ? 0xFFE14A : 0x3E5C48), 0);
            lv_obj_set_style_border_width(a->m_slot[t], on ? 4 : 2, 0);
        }
    }
    char line[80];
    int s = a->cfg.stadium[0] ? mg_stadium_find(&a->art, a->cfg.stadium) : -1;
    if (s >= 0) {
        const mg_stadium_t *st = &a->art.st[s];
        snprintf(line, sizeof line, "%s", st->name[mg_lang()][0] ? st->name[mg_lang()] : st->name[0]);
    } else {
        snprintf(line, sizeof line, "%s", _("Estadio: el del local"));
    }
    set_btn_text(a->m_stadium, line);
    set_btn_text(a->m_format, a->cfg.format == FMT_TIME ? _("Por tiempo") : _("A goles"));
    if (a->cfg.format == FMT_TIME) snprintf(line, sizeof line, _("Dos tiempos de %d min"), a->cfg.minutes);
    else snprintf(line, sizeof line, _("El primero a %d"), a->cfg.goals);
    set_btn_text(a->m_value, line);
}

static lv_obj_t *slot_box(mg_app_t *a, lv_obj_t *parent, int t, const char *label)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 470, 112);
    lv_obj_set_style_radius(b, 18, 0);
    lv_obj_set_style_bg_color(b, C(0x0E1712), 0);
    lv_obj_set_style_bg_opa(b, 230, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 12, 0);
    lv_obj_set_user_data(b, (void *)(intptr_t)t);
    lv_obj_add_event_cb(b, ev_slot, LV_EVENT_CLICKED, a);
    lv_obj_t *img = lv_image_create(b);
    lv_obj_align(img, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *l = mg_label(b, label, aos_font_small, 0x9FC8A8);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 136, 2);
    lv_obj_t *n = mg_label(b, "", aos_font_body, 0xFFFFFF);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(n, 300);
    lv_obj_align(n, LV_ALIGN_TOP_LEFT, 136, 40);
    if (t) {
        a->m_away = n;
        a->m_away_img = img;
    } else {
        a->m_home = n;
        a->m_home_img = img;
    }
    a->m_slot[t] = b;
    return b;
}

static void setup(mg_app_t *a)
{
    lv_obj_t *s = screen_new(a, 175);
    header(a, s, a->cfg.mode == MODE_CPU ? _("Contra la placa") : _("Dos jugadores"), ev_title);
    /* the leagues */
    static const char *const LG[2] = { N_("Selecciones"), N_("Clubes") };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *b = mg_button(s, _(LG[i]), a->cfg.league == i, 260, 60, ev_league, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_set_pos(b, 24 + i * 272, 100);
        a->m_tab[i] = b;
    }
    /* the grid of teams */
    lv_obj_t *grid = plain(s);
    lv_obj_set_pos(grid, 24, 176);
    lv_obj_set_size(grid, 740, 530);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    a->nlist = mg_teams_of(&a->teams, a->cfg.league, a->list, MG_TEAMS_MAX);
    int cols = 4, rows = (a->nlist + cols - 1) / cols;
    int ch = rows > 0 ? (520 - (rows - 1) * 10) / rows : 100;
    if (ch > 116) ch = 116;
    for (int k = 0; k < a->nlist; k++) {
        int i = a->list[k];
        const mg_team_t *t = &a->teams.t[i];
        lv_obj_t *b = lv_button_create(grid);
        lv_obj_set_size(b, 177, ch);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_bg_color(b, C(0x16241B), 0);
        lv_obj_set_style_bg_opa(b, 235, 0);
        lv_obj_set_style_bg_color(b, C(0x2C4A36), LV_STATE_PRESSED);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, C(0x2E4636), 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_pad_all(b, 6, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, ev_team, LV_EVENT_CLICKED, a);
        a->badge_px[k] = mg_badge_image(t, 60, 40, &a->badge_dsc[k]);
        if (a->badge_px[k]) {
            lv_obj_t *im = lv_image_create(b);
            lv_image_set_src(im, &a->badge_dsc[k]);
            lv_obj_align(im, LV_ALIGN_TOP_MID, 0, 2);
        }
        lv_obj_t *l = mg_label(b, t->name, aos_font_caption, 0xFFFFFF);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_width(l, 160);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -2);
    }
    /* the right column: the two teams, the stadium, the format, play */
    lv_obj_t *col = plain(s);
    lv_obj_set_pos(col, 786, 20);
    lv_obj_set_size(col, 470, 690);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    slot_box(a, col, 0, a->cfg.mode == MODE_CPU ? _("Vos (local)") : _("Jugador 1 (local)"));
    slot_box(a, col, 1, a->cfg.mode == MODE_CPU ? _("La placa (visitante)") : _("Jugador 2 (visitante)"));
    a->m_stadium = mg_button(col, "", false, 470, 66, ev_stadium, a);
    lv_obj_t *row = plain(col);
    lv_obj_set_size(row, 470, 66);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 10, 0);
    a->m_format = mg_button(row, "", false, 170, 66, ev_format, a);
    a->m_value = mg_button(row, "", false, 290, 66, ev_value, a);
    lv_obj_set_style_text_font(lv_obj_get_child(a->m_format, 0), aos_font_small, 0);
    lv_obj_set_style_text_font(lv_obj_get_child(a->m_value, 0), aos_font_small, 0);
    mg_button(col, _("¡A jugar!"), true, 470, 92, ev_play, a);
    setup_refresh(a);
    pad_items(a, 0);
}

/* ---- the settings ------------------------------------------------------------------------------------------ */

static void settings(mg_app_t *a);

typedef struct {
    const char *name;
    size_t off;
    int n;
    const char *val[4];
} opt_t;

static const opt_t OPTS[] = {
    { N_("Dificultad"), offsetof(mg_cfg_t, diff), 4, { N_("fácil"), N_("normal"), N_("difícil"), N_("crack") } },
    { N_("Varillas"), offsetof(mg_cfg_t, one_rod), 2, { N_("todas juntas"), N_("de a una") } },
    { N_("Jugador 1"), offsetof(mg_cfg_t, p1_side), 2, { N_("arco izquierdo"), N_("arco derecho") } },
    { N_("Pelota"), offsetof(mg_cfg_t, speed), 3, { N_("lenta"), N_("normal"), N_("rápida") } },
    { N_("Ayuda para patear"), offsetof(mg_cfg_t, assist), 2, { N_("no"), N_("sí") } },
    { N_("Sonido"), offsetof(mg_cfg_t, sfx), 2, { N_("no"), N_("sí") } },
    { N_("Música"), offsetof(mg_cfg_t, music), 2, { N_("no"), N_("sí") } },
    { N_("Hinchada"), offsetof(mg_cfg_t, crowd), 2, { N_("no"), N_("sí") } },
    { N_("Vibración al gol"), offsetof(mg_cfg_t, shake), 2, { N_("no"), N_("sí") } },
    { N_("Cambio de lado"), offsetof(mg_cfg_t, swap), 2, { N_("no"), N_("sí") } },
    { N_("Marcador"), offsetof(mg_cfg_t, board_lang), 3, { N_("como el sistema"), N_("castellano"), N_("inglés") } },
};
#define NOPTS ((int)(sizeof OPTS / sizeof OPTS[0]))

static void opt_text(const mg_app_t *a, int i, char *s, size_t n)
{
    int v = *(const int *)((const char *)&a->cfg + OPTS[i].off);
    if (v < 0 || v >= OPTS[i].n) v = 0;
    snprintf(s, n, "%s: %s", _(OPTS[i].name), _(OPTS[i].val[v]));
}

static void ev_opt(lv_event_t *e)
{
    mg_app_t *a = (mg_app_t *)lv_event_get_user_data(e);
    lv_obj_t *b = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(b);
    int *v = (int *)((char *)&a->cfg + OPTS[i].off);
    *v = (*v + 1) % OPTS[i].n;
    mg_audio_set(a->cfg.sfx, a->cfg.music, a->cfg.crowd);
    mg_cfg_save(&a->cfg);
    char line[96];
    opt_text(a, i, line, sizeof line);
    set_btn_text(b, line);
}

static void settings(mg_app_t *a)
{
    lv_obj_t *s = screen_new(a, 200);
    header(a, s, _("Ajustes"), ev_title);
    lv_obj_t *grid = plain(s);
    lv_obj_set_pos(grid, 40, 110);
    lv_obj_set_size(grid, 1200, 600);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN_WRAP);
    lv_obj_set_style_pad_row(grid, 12, 0);
    lv_obj_set_style_pad_column(grid, 24, 0);
    for (int i = 0; i < NOPTS; i++) {
        char line[96];
        opt_text(a, i, line, sizeof line);
        lv_obj_t *b = mg_button(grid, line, false, 580, 80, ev_opt, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
    }
    pad_items(a, 0);
}

/* ---- the switchboard -------------------------------------------------------------------------------------- */

void mg_menu_show(mg_app_t *a, int screen)
{
    a->screen = screen;
    a->state = a->state == ST_MATCH ? ST_MATCH : ST_MENU;
    if (screen == SC_SETUP) setup(a);
    else if (screen == SC_SETTINGS) settings(a);
    else title(a);
    aos_pad_reset(&a->pad, lv_tick_get());
}

void mg_menu_pad(mg_app_t *a)
{
    if (!a->scr) return;
    if (aos_pad_pressed(&a->pad, AOS_PAD_B)) {
        mg_menu_back(a);
        return;
    }
    if (a->screen == SC_SETUP && aos_pad_pressed(&a->pad, AOS_PAD_START)) {
        mg_match_start(a);
        return;
    }
    aos_pad_menu_step(&a->menu, &a->pad);
}

bool mg_menu_back(mg_app_t *a)
{
    if (a->screen == SC_TITLE) return false;
    mg_menu_show(a, SC_TITLE);
    return true;
}
