/*
 * P4OS - Mi casa: the board.
 *
 * The services in their groups, in the order of the file, as tiles: the
 * traffic light, the name, the latency in big (or "Caído" in red, with the
 * reason), and how long ago that changed. Two columns upright, four lying
 * down. Over them, how many are up, slow and down; and the buttons for desk
 * mode, the options and a new service.
 */
#include "ec.h"

#include <stdlib.h>

#define A ec_app
#define TILE_H 168

typedef struct {
    lv_obj_t *card, *dot, *name, *big, *sub, *corner, *corner_g, *corner_t;
    uint32_t  seq;
} tile_t;

static struct {
    tile_t    t[EC_MAX];
    lv_obj_t *count[3];         /* up, slow, down */
    lv_obj_t *net;
    lv_obj_t *sheet;
    lv_obj_t *fails_l;
    uint32_t  last_s;
} V;

/* ---- the tiles ---------------------------------------------------------------------------------- */

static void open_cb(lv_event_t *e)
{
    ec_go_detail((int)(intptr_t)lv_event_get_user_data(e));
}

static void tile_update(int i, const ec_rt_t *rt, uint32_t now)
{
    tile_t *t = &V.t[i];
    if (!t->card) return;
    const ec_svc_t *s = &A.cfg.s[i];
    int st = rt->state;
    lv_color_t col = ec_state_color(st);
    lv_obj_set_style_bg_color(t->dot, col, 0);
    bool down = st == EC_ST_DOWN;
    lv_color_t bg = down ? lv_color_hex(0x3A1618) : AOS_C_CARD;
    lv_obj_set_style_bg_color(t->card, bg, 0);
    lv_obj_set_style_bg_color(t->corner, bg, 0);
    lv_obj_set_style_border_width(t->card, down ? 2 : 0, 0);
    char big[48], sub[160], ago[48], d[48];
    uint32_t since = (now - rt->since_ms) / 1000u;
    ec_fmt_ago(ago, sizeof ago, since);
    switch (st) {
    case EC_ST_UP:
    case EC_ST_SLOW:
        ec_fmt_ms(big, sizeof big, rt->ms);
        if (rt->fails_row > 0) ec_copy(sub, sizeof sub, rt->fails_row == 1 ? _("falló una vez, probando de nuevo") : _("falló varias veces, probando de nuevo"));
        else if (since < 10) ec_copy(sub, sizeof sub, _("anda"));
        else {
            ec_fmt_dur(d, sizeof d, since);
            snprintf(sub, sizeof sub, _("anda desde hace %s"), d);
        }
        break;
    case EC_ST_DOWN:
        ec_copy(big, sizeof big, _("Caído"));
        if (rt->err == EC_E_CODE) snprintf(d, sizeof d, _("Contestó %d"), rt->code);
        else ec_copy(d, sizeof d, ec_err_text(rt->err));
        snprintf(sub, sizeof sub, "%s · %s", d, ago);
        break;
    case EC_ST_PAUSED:
        ec_copy(big, sizeof big, _("En pausa"));
        ec_svc_addr(s, sub, sizeof sub);
        break;
    default:
        ec_copy(big, sizeof big, "...");
        if (rt->fails_row > 0) ec_copy(sub, sizeof sub, rt->fails_row == 1 ? _("falló una vez, probando de nuevo") : _("falló varias veces, probando de nuevo"));
        else ec_copy(sub, sizeof sub, _("Probando"));
        break;
    }
    ec_set_text(t->big, big);
    lv_obj_set_style_text_color(t->big, st == EC_ST_UP ? AOS_C_TEXT : st == EC_ST_UNKNOWN || st == EC_ST_PAUSED ? AOS_C_DIM : col, 0);
    ec_set_text(t->sub, sub);
    lv_obj_set_style_text_color(t->sub, rt->fails_row > 0 && !down ? AOS_C_ORANGE : AOS_C_DIM, 0);
    /* the corner: the certificate when it is running out, else the kind of thing */
    if (rt->cert_state == EC_CERT_SOON || rt->cert_state == EC_CERT_EXPIRED) {
        lv_color_t cc = rt->cert_days < 0 ? AOS_C_RED : AOS_C_ORANGE;
        if (rt->cert_days < 0) ec_copy(d, sizeof d, "!");
        else snprintf(d, sizeof d, _("%d d"), rt->cert_days);
        ec_set_text(t->corner_g, AOS_SYM_LOCK);
        ec_set_text(t->corner_t, d);
        lv_obj_set_style_text_color(t->corner_g, cc, 0);
        lv_obj_set_style_text_color(t->corner_t, cc, 0);
    } else {
        ec_set_text(t->corner_g, ec_glyph(s));
        ec_set_text(t->corner_t, "");
        lv_obj_set_style_text_color(t->corner_g, lv_color_hex(0x636366), 0);
    }
}

static void tile_make(lv_obj_t *parent, int i, int32_t w)
{
    tile_t *t = &V.t[i];
    const int32_t in = 22;
    t->card = ec_card(parent, w, TILE_H);
    lv_obj_set_style_radius(t->card, 24, 0);
    lv_obj_set_style_border_color(t->card, AOS_C_RED, 0);
    lv_obj_set_style_bg_color(t->card, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->card, open_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    t->dot = ec_box(t->card, 18, 18);
    lv_obj_set_style_radius(t->dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(t->dot, LV_OPA_COVER, 0);
    lv_obj_set_pos(t->dot, in, 28);
    aos_make_decorative(t->dot);

    char name[EC_NAME * 2];
    aos_text_safe(name, sizeof name, A.cfg.s[i].name);
    t->name = ec_text(t->card, name, aos_font_body, AOS_C_TEXT, w - in * 2 - 30 - 40);
    lv_obj_set_pos(t->name, in + 30, 18);

    /* the corner: a glyph, and the certificate's days beside it when they run out */
    t->corner = ec_box(t->card, LV_SIZE_CONTENT, 40);
    lv_obj_set_flex_flow(t->corner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t->corner, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(t->corner, 4, 0);
    lv_obj_set_style_bg_color(t->corner, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(t->corner, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t->corner, 12, 0);
    lv_obj_set_style_pad_left(t->corner, 6, 0);
    lv_obj_align(t->corner, LV_ALIGN_TOP_RIGHT, -in + 4, 16);
    t->corner_g = aos_label(t->corner, "", &aos_sym_28, AOS_C_DIM);
    t->corner_t = aos_label(t->corner, "", aos_font_small, AOS_C_ORANGE);
    aos_make_decorative(t->corner);

    t->big = ec_text(t->card, "", aos_font_title, AOS_C_TEXT, w - in * 2);
    lv_obj_set_pos(t->big, in, 66);
    t->sub = ec_text(t->card, "", aos_font_caption, AOS_C_DIM, w - in * 2);
    lv_obj_set_pos(t->sub, in, 122);
    t->seq = 0;
}

/* ---- the summary --------------------------------------------------------------------------------- */

static lv_obj_t *count_item(lv_obj_t *parent, lv_color_t c)
{
    lv_obj_t *r = ec_box(parent, LV_SIZE_CONTENT, 40);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_t *d = ec_box(r, 14, 14);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(d, c, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    ec_text(r, "", aos_font_small, AOS_C_TEXT, 0);
    aos_make_decorative(r);
    return r;
}

static void summary_update(void)
{
    int up, slow, down;
    ec_lock();
    up = ec_count(EC_ST_UP);
    slow = ec_count(EC_ST_SLOW);
    down = ec_count(EC_ST_DOWN);
    ec_unlock();
    int n[3] = { up, slow, down };
    const char *one[3] = { _("anda"), _("lento"), _("caído") };
    const char *many[3] = { _("andan"), _("lentos"), _("caídos") };
    for (int k = 0; k < 3; k++) {
        if (!V.count[k]) continue;
        char b[48];
        snprintf(b, sizeof b, "%d %s", n[k], n[k] == 1 ? one[k] : many[k]);
        ec_set_text(lv_obj_get_child(V.count[k], 1), b);
        if (n[k] || k == 0) lv_obj_remove_flag(V.count[k], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(V.count[k], LV_OBJ_FLAG_HIDDEN);
    }
    if (V.net) {
        if (ec_net_ok()) lv_obj_add_flag(V.net, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(V.net, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- the buttons -------------------------------------------------------------------------------- */

static void add_cb(lv_event_t *e)
{
    (void)e;
    if (A.cfg.n >= EC_MAX) {
        aos_ui_toast(_("No entran más servicios"), 2000);
        return;
    }
    ec_go_edit(-1);
}

static void desk_cb(lv_event_t *e)
{
    (void)e;
    ec_desk_open();
}

static void gear_cb(lv_event_t *e)
{
    (void)e;
    ec_settings_open();
}

/* ---- the board ---------------------------------------------------------------------------------- */

void ec_list_build(void)
{
    memset(&V, 0, sizeof V);
    lv_obj_t *page = ec_box(A.root, A.W, A.H);
    const int32_t head = A.land ? 112 : 140;

    int32_t bw = 3 * AOS_UI_TAP_MIN + 2 * 12;
    lv_obj_t *title = ec_text(page, _("Mi casa"), aos_font_title, AOS_C_TEXT, A.W - 2 * AOS_UI_PAD - bw - 12);
    lv_obj_set_pos(title, AOS_UI_PAD, A.land ? 14 : 22);
    lv_obj_t *row = ec_box(page, A.W - 2 * AOS_UI_PAD - bw, 40);
    lv_obj_set_pos(row, AOS_UI_PAD, A.land ? 62 : 74);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 22, 0);
    V.count[0] = count_item(row, AOS_C_GREEN);
    V.count[1] = count_item(row, AOS_C_YELLOW);
    V.count[2] = count_item(row, AOS_C_RED);

    lv_obj_t *btns = ec_box(page, bw, AOS_UI_TAP_MIN);
    lv_obj_align(btns, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, A.land ? 12 : 20);
    lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btns, 12, 0);
    ec_round_btn(btns, AOS_SYM_MONITOR, desk_cb, NULL);
    ec_round_btn(btns, AOS_SYM_COG, gear_cb, NULL);
    ec_round_btn(btns, AOS_SYM_PLUS, add_cb, NULL);

    lv_obj_t *list = ec_box(page, A.W, A.H - head);
    lv_obj_set_pos(list, 0, head);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(list, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(list, 40, 0);
    lv_obj_set_style_pad_column(list, 16, 0);
    lv_obj_set_style_pad_row(list, 16, 0);
    int32_t inner = A.W - 2 * AOS_UI_PAD;

    V.net = ec_card(list, inner, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(V.net, 20, 0);
    lv_obj_set_style_bg_color(V.net, lv_color_hex(0x3A2A10), 0);
    lv_obj_t *nl = aos_label(V.net, _("La placa no tiene red: los chequeos esperan a que vuelva."), aos_font_small, AOS_C_ORANGE);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(nl, inner - 40);
    aos_make_decorative(V.net);

    int cols = inner >= 1100 ? 4 : inner >= 640 ? 2 : 1;
    if (A.land && inner < 1100) cols = 3;
    int32_t tw = (inner - (cols - 1) * 16) / cols;
    /* the groups in the order they first appear, each as wide as its tiles, side by side
     * when they fit: the tiles keep the grid */
    bool done[EC_MAX] = { false };
    for (int i = 0; i < A.cfg.n; i++) {
        if (done[i]) continue;
        const char *g = A.cfg.s[i].group;
        int k = 0;
        for (int j = i; j < A.cfg.n; j++) k += !strcmp(A.cfg.s[j].group, g);
        int gc = k < cols ? k : cols;
        int32_t gw = gc * tw + (gc - 1) * 16;
        lv_obj_t *gb = ec_box(list, gw, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(gb, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(gb, 16, 0);
        lv_obj_set_style_pad_row(gb, 16, 0);
        lv_obj_add_flag(gb, LV_OBJ_FLAG_EVENT_BUBBLE);
        char gs[EC_GROUP * 2];
        aos_text_safe(gs, sizeof gs, g[0] ? g : _("Sin grupo"));
        lv_obj_t *gl = ec_text(gb, gs, aos_font_small, AOS_C_DIM, gw);
        lv_obj_set_style_margin_top(gl, 14, 0);
        lv_obj_set_style_margin_bottom(gl, -6, 0);
        for (int j = i; j < A.cfg.n; j++) {
            if (done[j] || strcmp(A.cfg.s[j].group, g)) continue;
            done[j] = true;
            tile_make(gb, j, tw);
        }
    }
    if (!A.cfg.n) {
        lv_obj_t *l = aos_label(list, _("Todavía no hay servicios.\n\nAgregá uno con + (hay plantillas para Home Assistant, Frigate, un NAS, una impresora 3D, el router o una página), o cargalos cómodo desde la página Mi casa del portal."),
                                aos_font_body, AOS_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, inner);
        aos_make_decorative(l);
        lv_obj_t *b = ec_pill(list, AOS_SYM_PLUS, _("Agregar un servicio"), AOS_C_ACCENT, add_cb, NULL);
        lv_obj_set_style_margin_top(b, 20, 0);
    }
    ec_list_tick();
}

void ec_list_tick(void)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    bool second = now / 1000u != V.last_s;
    V.last_s = now / 1000u;
    ec_lock();
    for (int i = 0; i < A.cfg.n; i++) {
        const ec_rt_t *rt = ec_rt(i);
        if (!V.t[i].card || (rt->seq == V.t[i].seq && !second)) continue;
        V.t[i].seq = rt->seq;
        ec_rt_t copy = *rt;
        ec_unlock();
        tile_update(i, &copy, now);
        ec_lock();
    }
    ec_unlock();
    summary_update();
}

/* ---- the options ------------------------------------------------------------------------------- */

static void sheet_close(void)
{
    if (V.sheet) lv_obj_delete(V.sheet);
    V.sheet = NULL;
    V.fails_l = NULL;
}

bool ec_settings_back(void)
{
    if (!V.sheet) return false;
    sheet_close();
    return true;
}

static void sw_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (lv_event_get_user_data(e)) A.cfg.boot = on;
    else A.cfg.sound = on;
    ec_cfg_commit();
}

static void fails_show(void)
{
    if (!V.fails_l) return;
    char b[16];
    snprintf(b, sizeof b, "%d", A.cfg.fails);
    ec_set_text(V.fails_l, b);
}

static void fails_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int f = A.cfg.fails + d;
    if (f < 1 || f > 5) return;
    A.cfg.fails = f;
    ec_cfg_commit();
    fails_show();
}

static void all_cb(lv_event_t *e)
{
    (void)e;
    ec_core_check_now(-1);
    sheet_close();
    aos_ui_toast(_("Probando todos"), 1500);
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    sheet_close();
}

static lv_obj_t *opt_row(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *r = ec_box(parent, w, 88);
    lv_obj_t *l = aos_label(r, text, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, w - 190);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    aos_make_decorative(l);
    return r;
}

static void opt_switch(lv_obj_t *row, bool on, bool boot)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 100, 56);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, sw_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)boot);
}

void ec_settings_open(void)
{
    sheet_close();
    V.sheet = ec_box(A.root, A.W, A.H);
    lv_obj_set_style_bg_color(V.sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(V.sheet, LV_OPA_70, 0);
    lv_obj_add_flag(V.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(V.sheet, close_cb, LV_EVENT_CLICKED, NULL);
    int32_t w = A.W - 2 * AOS_UI_PAD;
    if (w > 680) w = 680;
    lv_obj_t *c = ec_card(V.sheet, w, LV_SIZE_CONTENT);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);   /* taps inside do not close it */
    lv_obj_set_style_pad_all(c, 28, 0);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_center(c);
    int32_t iw = w - 56;
    ec_text(c, _("Opciones"), aos_font_title, AOS_C_TEXT, iw);
    lv_obj_t *r = opt_row(c, _("Pitido con cada aviso"), iw);
    opt_switch(r, A.cfg.sound, false);
    r = opt_row(c, _("Vigilar desde que prende la placa"), iw);
    opt_switch(r, A.cfg.boot, true);
    r = opt_row(c, _("Fallas seguidas para darlo por caído"), iw);
    lv_obj_t *box = ec_box(r, 180, 64);
    lv_obj_align(box, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *m = ec_round_btn(box, AOS_SYM_MINUS, fails_cb, (void *)(intptr_t)-1);
    lv_obj_set_size(m, 64, 64);
    lv_obj_align(m, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *p = ec_round_btn(box, AOS_SYM_PLUS, fails_cb, (void *)(intptr_t)1);
    lv_obj_set_size(p, 64, 64);
    lv_obj_align(p, LV_ALIGN_RIGHT_MID, 0, 0);
    V.fails_l = aos_label(box, "", aos_font_body, AOS_C_TEXT);
    lv_obj_center(V.fails_l);
    fails_show();
    lv_obj_t *hint = aos_label(c, _("Las direcciones se cargan más cómodo desde la página Mi casa del portal."),
                               aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(hint, iw);
    lv_obj_set_style_margin_top(hint, 8, 0);
    lv_obj_t *btns = ec_box(c, iw, 96);
    lv_obj_set_style_margin_top(btns, 12, 0);
    lv_obj_t *b = ec_pill(btns, AOS_SYM_UPDATE, _("Probar todos"), AOS_C_CARD2, all_cb, NULL);
    lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
    b = ec_pill(btns, AOS_SYM_CHECK, _("Listo"), AOS_C_ACCENT, close_cb, NULL);
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
}
