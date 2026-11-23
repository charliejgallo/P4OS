/*
 * TANQUES - the menus: the title, the story's missions, the free battle's
 * settings and the results (see tq_app.h)
 */
#include "tq_app.h"

#include <stdio.h>
#include <string.h>

static float s_t;

/* --------------------------------------------------------------------------
 * The battle behind the title
 * -------------------------------------------------------------------------- */

void tqm_demo(app_t *a)
{
    if (!a->art_ok) return;
    tqa_wait_worker(a);
    tq_setup_t su;
    memset(&su, 0, sizeof su);
    su.biome = (int)(a->save.battles % TQ_BIOMES);
    su.size = 1;
    su.seed = 7 + a->save.battles;
    su.ntank = 2;
    for (int k = 0; k < 2; k++) {
        su.t[k].type = k ? TQ_T_MAMUT : a->save.tank;
        su.t[k].team = k;
        su.t[k].ai = true;
        memset(su.t[k].load, 0xFF, sizeof su.t[k].load);
        su.t[k].load[0] = TQ_W_CANNON;
    }
    a->setup = su;
    uint32_t types = (1u << su.t[0].type) | (1u << su.t[1].type);
    tq_art_use_show(-1);
    tq_art_use_biome(su.biome);
    tq_art_use_tanks(types, 0);
    tqa_paints(a);
    tq_game_new(&a->st, a->ter, &su);
    tq_scene_world_all();
    s_t = 0;
}

void tqm_menu_tick(app_t *a, float dt)
{
    /* the map stands still behind the menus: a whole frame of it costs
     * ~130 ms on the board */
    float M = a->ter->n * TQ_CELL;
    if (s_t == 0) {
        a->cam_x = M * 0.55f;
        a->cam_y = M * 0.5f;
        a->dirty = true;
        s_t = 1;
    }
    (void)dt;
}

static lv_obj_t *card(app_t *a, int w, int h)
{
    lv_obj_t *p = tqa_panel(a->ui, 0, 0, w, h, 36);
    lv_obj_set_style_bg_opa(p, LV_OPA_90, 0);
    lv_obj_center(p);
    return p;
}

static void coins_text(app_t *a, lv_obj_t *l)
{
    char txt[96];
    snprintf(txt, sizeof txt, _("%ld monedas"), (long)a->save.coins);
    lv_label_set_text(l, txt);
}

/* --------------------------------------------------------------------------
 * The title
 * -------------------------------------------------------------------------- */

static void go(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int to = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    tqa_screen(a, to);
}

void tqm_build_menu(app_t *a)
{
    bool land = a->W > a->H;
    int w = land ? 600 : a->W - 80;
    lv_obj_t *p = card(a, w, 560);
    lv_obj_t *t = tqa_label(p, _("Tanques"), aos_font_huge, 0xFFE9A8);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);
    a->coins_lbl = tqa_label(p, "", aos_font_caption, 0xFFD23C);
    coins_text(a, a->coins_lbl);
    lv_obj_align(a->coins_lbl, LV_ALIGN_TOP_MID, 0, 116);
    static const struct { const char *txt; uint32_t rgb; int to; } B[] = {
        { N_("Historia"), 0xE8572E, SC_STORY },
        { N_("Batalla libre"), 0x2E6FB0, SC_FREE },
        { N_("Hangar"), 0x5E8F3E, SC_HANGAR },
    };
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = tqa_button(p, _(B[i].txt), B[i].rgb, w - 120, 90, go, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)B[i].to);
        lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 180 + i * 116);
    }
}

/* --------------------------------------------------------------------------
 * The story
 * -------------------------------------------------------------------------- */

static void play_mission(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int m = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    const tq_mission_t *M = &TQ_MISSION[m];
    tq_setup_t su;
    memset(&su, 0, sizeof su);
    su.biome = M->biome;
    su.size = M->size;
    su.diff = M->diff;
    su.weather = M->weather;
    su.seed = M->seed;
    su.ntank = 1 + M->rivals;
    a->setup = su;
    tqh_player_tank(a, 0, a->save.tank, true);
    for (int k = 1; k < su.ntank; k++) {
        tqh_player_tank(a, k, M->rival_tank[k - 1], false);
        a->setup.t[k].ai = true;
        a->setup.t[k].team = 1;
    }
    su = a->setup;
    tqb_start(a, &su, true, m);
}

void tqm_build_story(app_t *a)
{
    bool land = a->W > a->H;
    int w = land ? 760 : a->W - 40, h = land ? a->H - 40 : a->H - 100;
    lv_obj_t *p = card(a, w, h);
    lv_obj_t *t = tqa_label(p, _("Historia"), aos_font_title, 0xFFE9A8);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 30, 22);
    a->coins_lbl = tqa_label(p, "", aos_font_caption, 0xFFD23C);
    coins_text(a, a->coins_lbl);
    lv_obj_align(a->coins_lbl, LV_ALIGN_TOP_RIGHT, -30, 34);
    lv_obj_t *list = lv_obj_create(p);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, w - 30, h - 100);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    for (int m = 0; m < TQ_STORY_MISSIONS; m++) {
        const tq_mission_t *M = &TQ_MISSION[m];
        bool open = m == 0 || a->save.stars[m - 1] > 0;
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, w - 50, 100);
        lv_obj_set_style_radius(row, 22, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(open ? 0x2A3A4C : 0x20262E), 0);
        lv_obj_set_user_data(row, (void *)(intptr_t)m);
        if (open) lv_obj_add_event_cb(row, play_mission, LV_EVENT_CLICKED, a);
        char txt[160];
        snprintf(txt, sizeof txt, "%d. %s", m + 1, _(M->name));
        lv_obj_t *n = tqa_label(row, txt, aos_font_body, open ? 0xFFFFFF : 0x6A7684);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 10, 4);
        snprintf(txt, sizeof txt, _("%s · %d rivales · %d monedas"), _(TQ_BIOME_NAME[M->biome]), M->rivals, M->reward);
        if (M->rivals == 1)
            snprintf(txt, sizeof txt, _("%s · 1 rival · %d monedas"), _(TQ_BIOME_NAME[M->biome]), M->reward);
        lv_obj_t *d = tqa_label(row, open ? txt : _("Ganá la anterior para jugarla"), aos_font_tiny, 0xB8C4CF);
        lv_obj_align(d, LV_ALIGN_BOTTOM_LEFT, 10, -6);
        for (int s = 0; s < 3; s++) {
            lv_obj_t *st = lv_obj_create(row);
            lv_obj_remove_style_all(st);
            lv_obj_set_size(st, 22, 22);
            lv_obj_set_style_radius(st, 11, 0);
            lv_obj_set_style_bg_opa(st, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(st, lv_color_hex(s < a->save.stars[m] ? 0xFFD23C : 0x3A4652), 0);
            lv_obj_align(st, LV_ALIGN_RIGHT_MID, -10 - (2 - s) * 28, 0);
            lv_obj_remove_flag(st, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

/* --------------------------------------------------------------------------
 * The free battle
 * -------------------------------------------------------------------------- */

enum { F_BIOME = 0, F_SIZE, F_RIVALS, F_DIFF, F_WEATHER, F_CAM, F_PLAYERS, F_P2, F_ROWS };
static lv_obj_t *s_fval[F_ROWS];
static int s_p2_type = TQ_T_BOMBO;

/* The biggest map takes two rivals at most: four tanks on it would need
 * more PSRAM than the board has free */
static int max_rivals(const tq_save_t *s)
{
    return s->f_size % TQ_SIZES == TQ_SIZES - 1 ? 2 : 3;
}

static void free_text(app_t *a)
{
    tq_save_t *s = &a->save;
    if (s->f_rivals > max_rivals(s)) s->f_rivals = (uint8_t)max_rivals(s);
    char txt[96];
    lv_label_set_text(s_fval[F_BIOME], _(TQ_BIOME_NAME[s->f_biome % TQ_BIOMES]));
    lv_label_set_text(s_fval[F_SIZE], _(TQ_SIZE_NAME[s->f_size % TQ_SIZES]));
    snprintf(txt, sizeof txt, "%d", s->f_rivals);
    lv_label_set_text(s_fval[F_RIVALS], txt);
    lv_label_set_text(s_fval[F_DIFF], _(TQ_DIFF_NAME[s->f_diff % TQ_DIFFS]));
    lv_label_set_text(s_fval[F_WEATHER], _(TQ_WEATHER_NAME[s->f_weather % TQ_WEATHERS]));
    lv_label_set_text(s_fval[F_CAM], s->cam_jump ? _("Salta") : _("Se desliza"));
    snprintf(txt, sizeof txt, "%d", s->f_players);
    lv_label_set_text(s_fval[F_PLAYERS], txt);
    lv_label_set_text(s_fval[F_P2], _(TQ_TANK[s_p2_type].name));
    lv_obj_t *row = lv_obj_get_parent(s_fval[F_P2]);
    tqa_show(row, s->f_players > 1);
}

static void free_step(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int v = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    int row = v / 2, d = (v & 1) ? 1 : -1;
    tq_save_t *s = &a->save;
    switch (row) {
    case F_BIOME: s->f_biome = (uint8_t)((s->f_biome + TQ_BIOMES + d) % TQ_BIOMES); break;
    case F_SIZE: s->f_size = (uint8_t)((s->f_size + TQ_SIZES + d) % TQ_SIZES); break;
    case F_RIVALS: {
        int n = max_rivals(s);
        s->f_rivals = (uint8_t)((s->f_rivals - 1 + n + d) % n + 1);
        break;
    }
    case F_DIFF: s->f_diff = (uint8_t)((s->f_diff + TQ_DIFFS + d) % TQ_DIFFS); break;
    case F_WEATHER: s->f_weather = (uint8_t)((s->f_weather + TQ_WEATHERS + d) % TQ_WEATHERS); break;
    case F_PLAYERS: s->f_players = s->f_players == 1 ? 2 : 1; break;
    case F_CAM: s->cam_jump = !s->cam_jump; break;
    case F_P2: s_p2_type = (s_p2_type + TQ_TANK_TYPES + d) % TQ_TANK_TYPES; break;
    default: break;
    }
    free_text(a);
}

static void free_go(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    tq_save_t *s = &a->save;
    tq_setup_t su;
    memset(&su, 0, sizeof su);
    su.biome = s->f_biome % TQ_BIOMES;
    su.size = s->f_size % TQ_SIZES;
    su.diff = s->f_diff % TQ_DIFFS;
    su.weather = s->f_weather % TQ_WEATHERS;
    su.seed = lv_tick_get() ^ (s->battles * 7919u);
    a->setup = su;
    int k = 0;
    tqh_player_tank(a, k++, s->tank, true);
    if (s->f_players > 1) {
        tqh_player_tank(a, k, s_p2_type, false);
        a->setup.t[k].team = 1;
        snprintf(a->setup.t[0].name, sizeof a->setup.t[0].name, "%s", _("Jugador 1"));
        snprintf(a->setup.t[k].name, sizeof a->setup.t[k].name, "%s", _("Jugador 2"));
        k++;
    }
    int rivals = s->f_players > 1 ? s->f_rivals - 1 : s->f_rivals;
    for (int r = 0; r < rivals && k < TQ_MAX_TANKS; r++, k++) {
        int type = (int)((su.seed >> (r * 3)) % TQ_TANK_TYPES);
        tqh_player_tank(a, k, type, false);
        a->setup.t[k].ai = true;
        a->setup.t[k].team = 2;
    }
    a->setup.ntank = k;
    tqa_save(a);
    su = a->setup;
    tqb_start(a, &su, false, 0);
}

void tqm_build_free(app_t *a)
{
    bool land = a->W > a->H;
    int w = land ? 760 : a->W - 40, rh = land ? 64 : 84;
    int h = 130 + F_ROWS * rh + 120;
    if (h > a->H - 30) h = a->H - 30;
    lv_obj_t *p = card(a, w, h);
    lv_obj_t *t = tqa_label(p, _("Batalla libre"), aos_font_title, 0xFFE9A8);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 30, 18);
    static const char *const NAME[F_ROWS] = { N_("Lugar"), N_("Tamaño"), N_("Rivales"), N_("Dificultad"), N_("Clima"),
                                              N_("Cámara"), N_("Jugadores"), N_("Tanque del 2") };
    for (int r = 0; r < F_ROWS; r++) {
        lv_obj_t *row = lv_obj_create(p);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w - 40, rh - 8);
        lv_obj_set_pos(row, 20, (land ? 70 : 90) + r * rh);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = tqa_label(row, _(NAME[r]), aos_font_body, 0xFFFFFF);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 10, 0);
        for (int d = 0; d < 2; d++) {
            lv_obj_t *b = tqa_button(row, d ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT, 0x2A3A4C, 70, rh - 14, free_step, a);
            lv_obj_set_user_data(b, (void *)(intptr_t)(r * 2 + d));
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, d ? 0 : -(land ? 300 : 280), 0);
        }
        s_fval[r] = tqa_label(row, "", aos_font_body, 0xFFE9A8);
        lv_obj_set_width(s_fval[r], land ? 220 : 200);
        lv_obj_set_style_text_align(s_fval[r], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_fval[r], LV_ALIGN_RIGHT_MID, -(land ? 80 : 75), 0);
    }
    lv_obj_t *b = tqa_button(p, _("¡A la batalla!"), 0xE8572E, w - 120, 90, free_go, a);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -20);
    free_text(a);
}

/* --------------------------------------------------------------------------
 * The results
 * -------------------------------------------------------------------------- */

static void result_go(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    tqa_screen(a, a->story ? SC_STORY : SC_MENU);
}

void tqm_build_result(app_t *a)
{
    bool land = a->W > a->H;
    int w = land ? 640 : a->W - 80;
    lv_obj_t *p = card(a, w, 470);
    const tq_tank_t *me = &a->st.tank[0];
    char txt[160];
    if (a->humans > 1) {
        const char *name = "";
        for (int k = 0; k < a->st.ntank; k++)
            if (a->st.tank[k].alive) name = a->st.tank[k].name;
        if (a->winner < 0) snprintf(txt, sizeof txt, "%s", _("Empate"));
        else snprintf(txt, sizeof txt, _("¡Ganó %s!"), name);
    } else {
        snprintf(txt, sizeof txt, "%s", a->winner == me->team ? _("¡Ganaste!") : _("Perdiste"));
    }
    lv_obj_t *t = tqa_label(p, txt, aos_font_large, 0xFFE9A8);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 30);
    snprintf(txt, sizeof txt, _("Daño hecho: %d   Tanques fuera: %d"), me->dealt, me->kills);
    lv_obj_t *d = tqa_label(p, txt, aos_font_caption, 0xB8C4CF);
    lv_obj_align(d, LV_ALIGN_TOP_MID, 0, 120);
    if (a->coins_won) {
        snprintf(txt, sizeof txt, _("+%d monedas"), a->coins_won);
        lv_obj_t *c = tqa_label(p, txt, aos_font_title, 0xFFD23C);
        lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 170);
    }
    if (a->story && a->stars_won) {
        for (int s = 0; s < 3; s++) {
            lv_obj_t *st = lv_obj_create(p);
            lv_obj_remove_style_all(st);
            lv_obj_set_size(st, 48, 48);
            lv_obj_set_style_radius(st, 24, 0);
            lv_obj_set_style_bg_opa(st, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(st, lv_color_hex(s < a->stars_won ? 0xFFD23C : 0x3A4652), 0);
            lv_obj_align(st, LV_ALIGN_TOP_MID, (s - 1) * 66, 236);
        }
    }
    lv_obj_t *b = tqa_button(p, _("Seguir"), 0xE8572E, 300, 86, result_go, a);
    lv_obj_align(b, LV_ALIGN_BOTTOM_MID, 0, -30);
}
