/*
 * TANQUES - the hangar: the six tanks on a turntable, to buy, upgrade, arm
 * and paint (see tq_app.h)
 */
#include "tq_app.h"

#include <stdio.h>
#include <string.h>

enum { TAB_UP = 0, TAB_ARMS, TAB_PAINT, TABS };

static lv_obj_t *s_body;
static lv_obj_t *s_emblem_cv[TQ_EMBLEMS];
static uint16_t *s_emblem_px;
static bool s_drag_spin;

/* The board's tanks carry these */
static const uint8_t RIVAL_LOAD[TQ_TANK_TYPES][TQ_SLOTS_MAX] = {
    { TQ_W_CANNON, TQ_W_MORTAR, TQ_W_BOUNCER, 0xFF },
    { TQ_W_CANNON, TQ_W_SCATTER, 0xFF, 0xFF },
    { TQ_W_CANNON, TQ_W_MORTAR, TQ_W_BIGBOMB, TQ_W_SHIELD },
    { TQ_W_MORTAR, TQ_W_CLUSTER, TQ_W_NAPALM, 0xFF },
    { TQ_W_CANNON, TQ_W_MISSILE, TQ_W_LASER, 0xFF },
    { TQ_W_CANNON, TQ_W_DRILL, TQ_W_REPAIR, 0xFF },
    { TQ_W_CANNON, TQ_W_TESLA, TQ_W_SHIELD, 0xFF },
    { TQ_W_TWIN, TQ_W_MORTAR, TQ_W_MISSILE, 0xFF },
};

void tqh_player_tank(app_t *a, int slot, int type, bool full)
{
    tq_setup_t *su = &a->setup;
    /* a DLC's tank whose pack is gone: its art too */
    if (!tq_dlc_have(tq_dlc_of_tank(type))) type = TQ_T_BOMBO;
    memset(&su->t[slot], 0, sizeof su->t[slot]);
    su->t[slot].type = type;
    su->t[slot].team = slot == 0 ? 0 : 1;
    memset(su->t[slot].load, 0xFF, sizeof su->t[slot].load);
    if (full) {
        const tq_garage_t *g = &a->save.g[type];
        memcpy(su->t[slot].up, g->up, sizeof g->up);
        int n = 0;
        for (int w = 0; w < TQ_SLOTS_MAX; w++)
            if (g->load[w] < TQ_WEAPONS && a->save.w_owned[g->load[w]]) su->t[slot].load[n++] = g->load[w];
        if (!n) su->t[slot].load[0] = TQ_W_CANNON;
        memcpy(su->t[slot].level, a->save.w_level, sizeof su->t[slot].level);
        snprintf(su->t[slot].name, sizeof su->t[slot].name, "%s", _("Vos"));
    } else {
        int lv = su->diff;
        for (int u = 0; u < TQ_UPGRADES; u++) su->t[slot].up[u] = (uint8_t)lv;
        memcpy(su->t[slot].load, RIVAL_LOAD[type], sizeof su->t[slot].load);
        for (int w = 0; w < TQ_WEAPONS; w++) su->t[slot].level[w] = (uint8_t)lv;
        snprintf(su->t[slot].name, sizeof su->t[slot].name, "%s", _(TQ_TANK[type].name));
    }
}

/* --------------------------------------------------------------------------
 * The turntable
 * -------------------------------------------------------------------------- */

static void paint_lut(app_t *a)
{
    const tq_garage_t *g = &a->save.g[a->h_tank];
    tq_art_tank_lut(&a->lut[0], TQ_PAINT[g->paint1 % TQ_PAINTS], TQ_PAINT[g->paint2 % TQ_PAINTS], g->emblem,
                    TQ_PAINT[g->emblem_paint % TQ_PAINTS]);
    a->dirty = true;
}

void tqh_fill(app_t *a, tq_req_t *r)
{
    bool land = a->W > a->H;
    r->mode = TQ_VIEW_SHOW;
    r->show_type = a->h_tank;
    float f = a->h_spin * TQ_SHOWS;
    r->show_frame = ((int)floorf(f) % TQ_SHOWS + TQ_SHOWS) % TQ_SHOWS;
    r->show_x = land ? a->W * 0.28f : a->W * 0.5f;
    r->show_y = land ? a->H * 0.62f : a->H * 0.36f;
}

void tqh_touch(app_t *a, int ev, int x, int y, int dx, int dy)
{
    (void)dy;
    bool land = a->W > a->H;
    if (ev == 0 || (ev == 1 && !s_drag_spin)) s_drag_spin = land ? x < a->W / 2 : y < a->H * 0.5f;
    if (ev == 1 && s_drag_spin) {
        a->h_spin -= dx / 900.0f;
        a->dirty = true;
    }
}

/* --------------------------------------------------------------------------
 * The tabs
 * -------------------------------------------------------------------------- */

static void build_body(app_t *a);
static void refresh_head(app_t *a);

static bool spend(app_t *a, int price)
{
    if (a->save.coins < price) {
        aos_ui_toast(_("No te alcanzan las monedas"), 1200);
        tq_snd(TQ_SND_DENY);
        return false;
    }
    a->save.coins -= price;
    tq_snd(TQ_SND_BUY);
    return true;
}

static void on_upgrade(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int u = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    tq_garage_t *g = &a->save.g[a->h_tank];
    if (!g->owned || g->up[u] >= TQ_UP_MAX) return;
    if (!spend(a, tq_up_price(a->h_tank, u, g->up[u] + 1))) return;
    g->up[u]++;
    tqa_save(a);
    build_body(a);
    refresh_head(a);
}

static int equipped_slot(const tq_garage_t *g, int w)
{
    for (int s = 0; s < TQ_SLOTS_MAX; s++)
        if (g->load[s] == w) return s;
    return -1;
}

static void on_weapon(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int v = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    int w = v & 0xFF, what = v >> 8;            /* 0 buy, 1 equip/unequip, 2 level up */
    tq_garage_t *g = &a->save.g[a->h_tank];
    int slots = TQ_TANK[a->h_tank].slots;
    if (what == 0) {
        if (!spend(a, TQ_WEAPON[w].price)) return;
        a->save.w_owned[w] = 1;
    } else if (what == 2) {
        int lv = a->save.w_level[w];
        if (lv >= TQ_W_LEVELS || !spend(a, tq_w_price(w, lv + 1))) return;
        a->save.w_level[w]++;
    } else {
        int s = equipped_slot(g, w);
        if (s >= 0) {
            int n = 0;
            for (int k = 0; k < TQ_SLOTS_MAX; k++)
                if (g->load[k] < TQ_WEAPONS) n++;
            if (n <= 1) {
                aos_ui_toast(_("Tiene que llevar por lo menos un arma"), 1200);
                return;
            }
            for (int k = s; k + 1 < TQ_SLOTS_MAX; k++) g->load[k] = g->load[k + 1];
            g->load[TQ_SLOTS_MAX - 1] = 0xFF;
        } else {
            int free_slot = -1;
            for (int k = 0; k < slots; k++)
                if (g->load[k] >= TQ_WEAPONS) {
                    free_slot = k;
                    break;
                }
            if (free_slot < 0) free_slot = slots - 1;           /* the last one makes room */
            g->load[free_slot] = (uint8_t)w;
        }
    }
    tqa_save(a);
    build_body(a);
    refresh_head(a);
}

static void on_paint(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int v = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    int which = v >> 8, k = v & 0xFF;
    tq_garage_t *g = &a->save.g[a->h_tank];
    if (which == 0) g->paint1 = (uint8_t)k;
    if (which == 1) g->paint2 = (uint8_t)k;
    if (which == 2) g->emblem = (uint8_t)k;
    if (which == 3) g->emblem_paint = (uint8_t)k;
    tqa_save(a);
    paint_lut(a);
    build_body(a);
}

static lv_obj_t *row_box(lv_obj_t *list, int w, int h)
{
    lv_obj_t *row = lv_obj_create(list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, w, h);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x223040), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

static lv_obj_t *swatch(lv_obj_t *parent, uint32_t rgb, bool on, int size, lv_event_cb_t cb, void *ud, int v)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, size / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(rgb), 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(b, on ? 5 : 0, 0);
    lv_obj_set_user_data(b, (void *)(intptr_t)v);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static void build_body(app_t *a)
{
    lv_obj_clean(s_body);
    int w = lv_obj_get_width(s_body) - 10;
    tq_garage_t *g = &a->save.g[a->h_tank];
    char txt[160];
    switch (a->h_tab) {
    case TAB_UP:
        for (int u = 0; u < TQ_UPGRADES; u++) {
            lv_obj_t *row = row_box(s_body, w, 104);
            lv_obj_t *n = tqa_label(row, _(TQ_UP_NAME[u]), aos_font_body, 0xFFFFFF);
            lv_obj_align(n, LV_ALIGN_TOP_LEFT, 18, 10);
            lv_obj_t *i = tqa_label(row, _(TQ_UP_INFO[u]), aos_font_tiny, 0xB8C4CF);
            lv_obj_align(i, LV_ALIGN_BOTTOM_LEFT, 18, -12);
            for (int l = 0; l < TQ_UP_MAX; l++) {
                lv_obj_t *d = lv_obj_create(row);
                lv_obj_remove_style_all(d);
                lv_obj_set_size(d, 26, 12);
                lv_obj_set_style_radius(d, 6, 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(d, lv_color_hex(l < g->up[u] ? 0xFFD23C : 0x3A4652), 0);
                lv_obj_align(d, LV_ALIGN_TOP_LEFT, 200 + l * 32, 22);
            }
            if (g->up[u] >= TQ_UP_MAX) snprintf(txt, sizeof txt, "%s", _("Al máximo"));
            else snprintf(txt, sizeof txt, _("Mejorar\n%d"), tq_up_price(a->h_tank, u, g->up[u] + 1));
            lv_obj_t *b = tqa_button(row, txt, 0x5E8F3E, 150, 80, on_upgrade, a);
            lv_obj_set_style_text_font(lv_obj_get_child(b, 0), aos_font_caption, 0);
            lv_obj_set_user_data(b, (void *)(intptr_t)u);
            lv_obj_align(b, LV_ALIGN_RIGHT_MID, -10, 0);
            if (!g->owned || g->up[u] >= TQ_UP_MAX) lv_obj_add_state(b, LV_STATE_DISABLED);
        }
        break;
    case TAB_ARMS: {
        int slots = TQ_TANK[a->h_tank].slots, used = 0;
        for (int s = 0; s < TQ_SLOTS_MAX; s++)
            if (g->load[s] < TQ_WEAPONS) used++;
        snprintf(txt, sizeof txt, _("Lleva %d de %d armas"), used, slots);
        lv_obj_t *hd = tqa_label(s_body, txt, aos_font_caption, 0xFFE9A8);
        (void)hd;
        for (int wpn = 0; wpn < TQ_WEAPONS; wpn++) {
            const tq_weapon_t *W = &TQ_WEAPON[wpn];
            bool own = a->save.w_owned[wpn], on = equipped_slot(g, wpn) >= 0;
            lv_obj_t *row = row_box(s_body, w, 104);
            if (on) {
                lv_obj_set_style_border_color(row, lv_color_hex(0xFFD23C), 0);
                lv_obj_set_style_border_width(row, 3, 0);
            }
            lv_obj_t *dot = lv_obj_create(row);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 22, 22);
            lv_obj_set_style_radius(dot, 11, 0);
            lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            lv_obj_set_style_bg_color(dot, lv_color_hex(W->rgb), 0);
            lv_obj_align(dot, LV_ALIGN_TOP_LEFT, 14, 16);
            int lv = a->save.w_level[wpn];
            if (lv) snprintf(txt, sizeof txt, "%s  %d AP  " LV_SYMBOL_UP "%d", _(W->name), W->ap, lv);
            else snprintf(txt, sizeof txt, "%s  %d AP", _(W->name), W->ap);
            lv_obj_t *n = tqa_label(row, txt, aos_font_caption, 0xFFFFFF);
            lv_obj_align(n, LV_ALIGN_TOP_LEFT, 46, 12);
            lv_obj_t *i = tqa_label(row, _(W->info), aos_font_tiny, 0xB8C4CF);
            lv_obj_set_width(i, w - 300);
            lv_label_set_long_mode(i, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_align(i, LV_ALIGN_BOTTOM_LEFT, 46, -12);
            int dlc = tq_dlc_of_weapon(wpn);
            if (!tq_dlc_have(dlc)) {
                /* a DLC's, not installed: what brings it */
                snprintf(txt, sizeof txt, _("Viene con\n%s"), _(TQ_DLC_NAME[dlc]));
                lv_obj_t *l = tqa_label(row, txt, aos_font_tiny, 0x8A96A4);
                lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
                lv_obj_align(l, LV_ALIGN_RIGHT_MID, -14, 0);
                lv_obj_set_style_opa(row, LV_OPA_60, 0);
            } else if (!own) {
                snprintf(txt, sizeof txt, _("Comprar\n%d"), W->price);
                lv_obj_t *b = tqa_button(row, txt, 0xE8572E, 130, 80, on_weapon, a);
                lv_obj_set_style_text_font(lv_obj_get_child(b, 0), aos_font_caption, 0);
                lv_obj_set_user_data(b, (void *)(intptr_t)(wpn));
                lv_obj_align(b, LV_ALIGN_RIGHT_MID, -10, 0);
            } else {
                lv_obj_t *b = tqa_button(row, on ? _("Quitar") : _("Llevar"), on ? 0x4A5560 : 0x2E6FB0, 120, 80, on_weapon, a);
                lv_obj_set_style_text_font(lv_obj_get_child(b, 0), aos_font_caption, 0);
                lv_obj_set_user_data(b, (void *)(intptr_t)(wpn | 1 << 8));
                lv_obj_align(b, LV_ALIGN_RIGHT_MID, -10, 0);
                if (lv < TQ_W_LEVELS) {
                    snprintf(txt, sizeof txt, LV_SYMBOL_UP "\n%d", tq_w_price(wpn, lv + 1));
                    lv_obj_t *u = tqa_button(row, txt, 0x5E8F3E, 100, 80, on_weapon, a);
                    lv_obj_set_style_text_font(lv_obj_get_child(u, 0), aos_font_caption, 0);
                    lv_obj_set_user_data(u, (void *)(intptr_t)(wpn | 2 << 8));
                    lv_obj_align(u, LV_ALIGN_RIGHT_MID, -140, 0);
                }
                if (!g->owned) lv_obj_add_state(b, LV_STATE_DISABLED);
            }
        }
        break;
    }
    case TAB_PAINT: {
        static const char *const NAME[4] = { N_("Color"), N_("Segundo color"), N_("Emblema"), N_("Color del emblema") };
        int per = w >= 600 ? 12 : 6;
        int sz = (w - 20) / per - 8;
        if (sz > 56) sz = 56;
        for (int which = 0; which < 4; which++) {
            lv_obj_t *t = tqa_label(s_body, _(NAME[which]), aos_font_caption, 0xFFE9A8);
            (void)t;
            lv_obj_t *grid = lv_obj_create(s_body);
            lv_obj_remove_style_all(grid);
            lv_obj_set_size(grid, w, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
            lv_obj_set_style_pad_column(grid, 8, 0);
            lv_obj_set_style_pad_row(grid, 8, 0);
            lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
            int cur = which == 0 ? g->paint1 : (which == 1 ? g->paint2 : (which == 2 ? g->emblem : g->emblem_paint));
            for (int k = 0; k < (which == 2 ? TQ_EMBLEMS : TQ_PAINTS); k++) {
                if (which != 2) {
                    swatch(grid, TQ_PAINT[k], k == cur, sz, on_paint, a, which << 8 | k);
                    continue;
                }
                /* an emblem: its bits on a small canvas inside the button */
                lv_obj_t *b = swatch(grid, 0x2A3A4C, k == cur, sz, on_paint, a, which << 8 | k);
                lv_obj_set_style_radius(b, 12, 0);
                if (!s_emblem_px) continue;
                uint16_t *px = s_emblem_px + k * 32 * 32;
                uint16_t bits[16];
                tq_emblem(k, bits);
                uint16_t fg = tq_565(TQ_PAINT[g->emblem_paint % TQ_PAINTS]), bg = tq_565(0xF7F7F2);
                for (int y = 0; y < 32; y++)
                    for (int x = 0; x < 32; x++)
                        px[y * 32 + x] = (bits[y / 2] & (0x8000u >> (x / 2))) ? fg : bg;
                lv_obj_t *cv = lv_canvas_create(b);
                lv_canvas_set_buffer(cv, px, 32, 32, LV_COLOR_FORMAT_RGB565);
                lv_obj_center(cv);
                lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
                s_emblem_cv[k] = cv;
            }
        }
        break;
    }
    default:
        break;
    }
}

/* --------------------------------------------------------------------------
 * The head: which tank, buy or pick it
 * -------------------------------------------------------------------------- */

static void refresh_head(app_t *a)
{
    char txt[240];
    if (a->coins_lbl) {
        snprintf(txt, sizeof txt, _("%ld monedas"), (long)a->save.coins);
        lv_label_set_text(a->coins_lbl, txt);
    }
    const tq_tank_type_t *T = &TQ_TANK[a->h_tank];
    const tq_garage_t *g = &a->save.g[a->h_tank];
    tq_stats_t st;
    tq_stats(a->h_tank, g->up, &st);
    lv_label_set_text(a->h_name, _(T->name));
    snprintf(txt, sizeof txt, _("%s\nVida %d · Blindaje %d%% · %d AP · %d armas"), _(T->role), st.hpmax, st.armor, st.apmax,
             T->slots);
    lv_label_set_text(a->h_role, txt);
    if (!g->owned) {
        snprintf(txt, sizeof txt, _("Comprar · %d"), T->price);
        tqa_btn_text(a->h_buy, txt);
        lv_obj_set_style_bg_color(a->h_buy, lv_color_hex(0xE8572E), 0);
    } else if (a->save.tank == a->h_tank) {
        tqa_btn_text(a->h_buy, _("El tuyo"));
        lv_obj_set_style_bg_color(a->h_buy, lv_color_hex(0x4A5560), 0);
    } else {
        tqa_btn_text(a->h_buy, _("Usar este"));
        lv_obj_set_style_bg_color(a->h_buy, lv_color_hex(0x2E6FB0), 0);
    }
    for (int t = 0; t < TABS; t++)
        lv_obj_set_style_border_width(a->h_tabs[t], t == a->h_tab ? 4 : 0, 0);
}

static void set_tank(app_t *a, int t)
{
    a->h_tank = (t + TQ_TANK_TYPES) % TQ_TANK_TYPES;
    if (!tq_dlc_have(tq_dlc_of_tank(a->h_tank))) a->h_tank = TQ_T_BOMBO;
    tqa_wait_worker(a);
    /* the hangar draws only its turntable: the menu's battle gives its
     * tanks back first (the Mamut's turntable alone is 5.4 MB) */
    tq_art_use_tanks(0, 0);
    if (!tq_art_use_show(a->h_tank)) aos_ui_toast(_("No hay memoria para el arte"), 1500);
    aos_hal_log("tanques", "hangar: %s, art %u KB, game %u KB", TQ_TANK[a->h_tank].key, (unsigned)(tq_art_bytes() / 1024),
                (unsigned)(tq_mem_used() / 1024));
    paint_lut(a);
    refresh_head(a);
    build_body(a);
}

void tqh_cycle(app_t *a, int d)
{
    /* the DLCs' tanks only with their packs */
    int t = a->h_tank;
    for (int i = 0; i < TQ_TANK_TYPES; i++) {
        t = (t + TQ_TANK_TYPES + d) % TQ_TANK_TYPES;
        if (tq_dlc_have(tq_dlc_of_tank(t))) break;
    }
    set_tank(a, t);
}

static void on_arrow(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    int d = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    set_tank(a, a->h_tank + d);
}

static void on_buy(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    tq_garage_t *g = &a->save.g[a->h_tank];
    if (!g->owned) {
        if (!spend(a, TQ_TANK[a->h_tank].price)) return;
        g->owned = 1;
    }
    a->save.tank = (uint8_t)a->h_tank;
    tqa_save(a);
    refresh_head(a);
    build_body(a);
}

static void on_tab(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    a->h_tab = (int)(intptr_t)lv_obj_get_user_data((lv_obj_t *)lv_event_get_target(e));
    refresh_head(a);
    build_body(a);
}

static void on_menu(lv_event_t *e)
{
    app_t *a = (app_t *)lv_event_get_user_data(e);
    tq_art_use_show(-1);
    tqa_screen(a, SC_MENU);
}

void tqh_free(void)
{
    tq_free(s_emblem_px);
    s_emblem_px = NULL;
}

void tqh_build(app_t *a)
{
    lv_obj_t *ui = a->ui;
    bool land = a->W > a->H;
    int W = a->W, H = a->H, m = 14;
    if (!s_emblem_px) s_emblem_px = (uint16_t *)tq_malloc(TQ_EMBLEMS * 32 * 32 * 2);

    lv_obj_t *back = tqa_button(ui, LV_SYMBOL_LEFT, 0x2A3440, 80, 70, on_menu, a);
    lv_obj_align(back, LV_ALIGN_TOP_LEFT, m, m);
    a->coins_lbl = tqa_label(ui, "", aos_font_body, 0xFFD23C);
    lv_obj_align(a->coins_lbl, LV_ALIGN_TOP_RIGHT, -m - 10, m + 18);
    lv_obj_t *title = tqa_label(ui, _("Hangar"), aos_font_title, 0xFFE9A8);
    lv_obj_align(title, LV_ALIGN_TOP_MID, land ? -W / 4 : 0, m + 14);

    /* the tank's name, arrows, and buy or pick */
    int hx = land ? 0 : 0, hw = land ? W / 2 : W;
    int hy = land ? 96 : (int)(H * 0.42f);
    a->h_name = tqa_label(ui, "", aos_font_large, 0xFFFFFF);
    lv_obj_set_width(a->h_name, hw);
    lv_obj_set_style_text_align(a->h_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(a->h_name, hx, hy);
    a->h_role = tqa_label(ui, "", aos_font_tiny, 0xB8C4CF);
    lv_obj_set_width(a->h_role, hw - 40);
    lv_obj_set_style_text_align(a->h_role, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(a->h_role, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(a->h_role, hx + 20, hy + 60);
    for (int d = -1; d <= 1; d += 2) {
        lv_obj_t *b = tqa_button(ui, d < 0 ? LV_SYMBOL_LEFT : LV_SYMBOL_RIGHT, 0x2A3440, 80, 80, on_arrow, a);
        lv_obj_set_user_data(b, (void *)(intptr_t)d);
        int y = land ? (int)(H * 0.55f) : (int)(H * 0.24f);
        lv_obj_set_pos(b, d < 0 ? hx + m : hx + hw - 80 - m, y);
    }
    a->h_buy = tqa_button(ui, "", 0x2E6FB0, land ? 300 : 320, 76, on_buy, a);
    lv_obj_set_pos(a->h_buy, hx + (hw - (land ? 300 : 320)) / 2, land ? H - 76 - m : hy + 130);

    /* the tabs and their list */
    int bx = land ? W / 2 + m : m, by = land ? 96 : hy + 220, bw = land ? W / 2 - 2 * m : W - 2 * m;
    int bh = H - by - m;
    static const char *const TAB[TABS] = { N_("Mejoras"), N_("Armas"), N_("Pintura") };
    int tw = (bw - 2 * 10) / TABS;
    for (int t = 0; t < TABS; t++) {
        a->h_tabs[t] = tqa_button(ui, _(TAB[t]), 0x2A3A4C, tw, 64, on_tab, a);
        lv_obj_set_user_data(a->h_tabs[t], (void *)(intptr_t)t);
        lv_obj_set_pos(a->h_tabs[t], bx + t * (tw + 10), by);
    }
    s_body = lv_obj_create(ui);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_pos(s_body, bx, by + 76);
    lv_obj_set_size(s_body, bw, bh - 76);
    lv_obj_set_style_bg_color(s_body, lv_color_hex(0x16202A), 0);
    lv_obj_set_style_bg_opa(s_body, LV_OPA_80, 0);
    lv_obj_set_style_radius(s_body, 24, 0);
    lv_obj_set_style_pad_all(s_body, 8, 0);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 8, 0);
    lv_obj_add_flag(s_body, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(s_body, LV_DIR_VER);
    lv_obj_update_layout(s_body);
    set_tank(a, a->h_tank ? a->h_tank : a->save.tank);
}
