/*
 * Malla - the frame of the screens: the header, the tabs, the sheets.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Standing, the tabs are a bar at the bottom, as on a phone; lying down
 * they are a rail on the left, so the body keeps its height. A conversation
 * hides the bar: the keyboard needs the room.
 */
#include "msh_ui.h"

#include "aos_text_safe.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

msh_ui_t UI;

#define HEAD_H   92
#define BAR_H    112
#define RAIL_W   150

static const char *const TAB_NAME[MSH_TAB_N] = { N_("Chats"), N_("Nodos"), N_("Mapa"), N_("Nodo") };
static const char *const TAB_SYM[MSH_TAB_N] = { LV_SYMBOL_ENVELOPE, LV_SYMBOL_LIST, LV_SYMBOL_GPS, LV_SYMBOL_SETTINGS };

/* ---- small builders ---- */

lv_obj_t *msh_ui_col(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return c;
}

lv_obj_t *msh_ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 24, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *msh_ui_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_CLICKABLE);
    return r;
}

lv_obj_t *msh_ui_label(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, f ? f : aos_font_body, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

lv_obj_t *msh_ui_btn(lv_obj_t *parent, const char *text, lv_color_t c, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 80);
    lv_obj_set_style_bg_color(b, c, 0);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, aos_font_small, 0);
    lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *msh_ui_avatar(lv_obj_t *parent, uint32_t num, int size)
{
    lv_obj_t *a = lv_obj_create(parent);
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, size, size);
    lv_obj_set_style_radius(a, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(a, lv_color_hex(msh_node_rgb(num)), 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    char sn[8], safe[24];
    msh_node_short(num, sn, sizeof sn);
    msh_ui_safe(safe, sizeof safe, sn);
    /* black on a light colour, white on a dark one */
    uint32_t rgb = msh_node_rgb(num);
    uint32_t lum = ((rgb >> 16) & 0xFF) * 30 + ((rgb >> 8) & 0xFF) * 59 + (rgb & 0xFF) * 11;
    lv_obj_t *l = msh_ui_label(a, safe, size >= 100 ? aos_font_title : size >= 72 ? aos_font_small : aos_font_caption,
                               lv_color_hex(lum > 14000 ? 0x000000 : 0xFFFFFF));
    lv_obj_center(l);
    return a;
}

void msh_ui_age(int32_t s, char *out, size_t cap)
{
    if (s < 0) snprintf(out, cap, "%s", _("nunca"));
    else if (s < 60) snprintf(out, cap, "%s", _("recién"));
    else if (s < 3600) snprintf(out, cap, _("hace %d min"), (int)(s / 60));
    else if (s < 86400) snprintf(out, cap, _("hace %d h"), (int)(s / 3600));
    else snprintf(out, cap, _("hace %d d"), (int)(s / 86400));
}

void msh_ui_when(const msh_msg_t *m, char *out, size_t cap)
{
    if (m->epoch) {
        time_t t = (time_t)m->epoch, now = (time_t)msh_epoch_guess();
        struct tm a, b;
        localtime_r(&t, &a);
        localtime_r(&now, &b);
        if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) snprintf(out, cap, "%02d:%02d", a.tm_hour, a.tm_min);
        else snprintf(out, cap, "%02d/%02d %02d:%02d", a.tm_mday, a.tm_mon + 1, a.tm_hour, a.tm_min);
    } else if (m->up) {
        msh_ui_age((int32_t)(msh_now_s() - m->up), out, cap);
    } else if (cap) {
        out[0] = 0;
    }
}

void msh_ui_safe(char *out, size_t cap, const char *in)
{
    aos_text_safe(out, cap, in ? in : "");
}

void msh_ui_num(char *out, size_t cap, float v, int dec)
{
    snprintf(out, cap, dec == 0 ? "%.0f" : dec == 1 ? "%.1f" : "%.2f", (double)v);
    for (char *p = out; *p; p++) if (*p == '.') *p = ',';
}

void msh_ui_dist(char *out, size_t cap, float m)
{
    if (m < 0) { snprintf(out, cap, "?"); return; }
    if (m < 1000) { snprintf(out, cap, "%d m", (int)(m + 0.5f)); return; }
    char n[16];
    msh_ui_num(n, sizeof n, m / 1000.0f, m < 100000 ? 1 : 0);
    snprintf(out, cap, "%s km", n);
}

/* ---- sheets ---- */

static void sheet_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) msh_ui_sheet_close();
}

lv_obj_t *msh_ui_sheet(int kind, const char *title)
{
    msh_ui_sheet_close();
    lv_obj_t *bg = lv_obj_create(UI.root);
    lv_obj_remove_style_all(bg);
    lv_obj_set_size(bg, UI.W, UI.H);
    lv_obj_set_style_bg_color(bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(bg, LV_OPA_60, 0);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(bg, sheet_bg_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *p = lv_obj_create(bg);
    lv_obj_remove_style_all(p);
    lv_obj_set_width(p, UI.land ? 760 : UI.W);
    lv_obj_set_height(p, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p, UI.H * 9 / 10, 0);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 28, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    lv_obj_set_style_pad_bottom(p, 36, 0);
    lv_obj_set_style_pad_row(p, 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_align(p, UI.land ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, 0);
    if (title) msh_ui_label(p, title, aos_font_title, AOS_C_TEXT);
    UI.sheet = bg;
    UI.sheet_kind = kind;
    return p;
}

void msh_ui_sheet_close(void)
{
    if (!UI.sheet) return;
    if (UI.sheet_kind == MSH_SHEET_CONN) msh_ui_scan_stop();
    lv_obj_delete(UI.sheet);
    UI.sheet = NULL;
    UI.sheet_kind = MSH_SHEET_NONE;
    UI.scan_list = NULL;
}

/* ---- the header ---- */

static void chip_cb(lv_event_t *e)
{
    (void)e;
    msh_ui_conn_sheet();
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    msh_ui_back();
}

void msh_ui_header(void)
{
    if (!UI.head) return;
    char t[80], safe[140];
    if (UI.view == MSH_V_CHAT) {
        msh_lock();
        if (UI.chat_peer) msh_node_name(UI.chat_peer, t, sizeof t);
        else msh_channel_name(UI.chat_ch, t, sizeof t);
        msh_unlock();
    } else if (UI.view == MSH_V_NODE) {
        snprintf(t, sizeof t, "%s", _("Nodo"));
    } else {
        snprintf(t, sizeof t, "%s", _(TAB_NAME[UI.tab]));
    }
    /* the arrow after the filter, which would take it for a missing glyph */
    snprintf(safe, sizeof safe, "%s", UI.view != MSH_V_TAB ? LV_SYMBOL_LEFT "  " : "");
    msh_ui_safe(safe + strlen(safe), sizeof safe - strlen(safe), t);
    lv_label_set_text(UI.title, safe);

    msh_lock();
    msh_state_t st = MSH.state;
    const char *note = MSH.note;
    int n = MSH.n_nodes;
    bool has_target = MSH.target.kind != MSH_LINK_NONE;
    msh_unlock();
    lv_color_t dot = AOS_C_DIM;
    char txt[64];
    switch (st) {
    case MSH_ST_READY:
        dot = AOS_C_GREEN;
        snprintf(txt, sizeof txt, "%s", _("Conectado"));
        break;
    case MSH_ST_CONFIG:
        dot = AOS_C_ORANGE;
        snprintf(txt, sizeof txt, _("Cargando %d nodos"), n);
        break;
    case MSH_ST_OPENING:
        dot = AOS_C_ORANGE;
        snprintf(txt, sizeof txt, "%s", note && note[0] ? _(note) : _("Conectando…"));
        break;
    case MSH_ST_RETRY:
        dot = AOS_C_RED;
        snprintf(txt, sizeof txt, "%s", _("Reintentando"));
        break;
    default:
        snprintf(txt, sizeof txt, "%s", has_target ? _("Sin conexión") : _("Conectar"));
        break;
    }
    lv_obj_set_style_bg_color(UI.chip_dot, dot, 0);
    lv_label_set_text(UI.chip_txt, txt);
}

/* ---- the tabs ---- */

static void tab_cb(lv_event_t *e)
{
    msh_ui_show_tab((msh_tab_t)(intptr_t)lv_event_get_user_data(e));
}

static void update_badge(void)
{
    if (!UI.badge) return;
    msh_lock();
    int u = msh_unread_all();
    msh_unlock();
    if (u > 0) {
        char b[8];
        snprintf(b, sizeof b, "%d", u > 99 ? 99 : u);
        lv_label_set_text(lv_obj_get_child(UI.badge, 0), b);
        lv_obj_remove_flag(UI.badge, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(UI.badge, LV_OBJ_FLAG_HIDDEN);
    }
}

static void style_tabs(void)
{
    for (int i = 0; i < MSH_TAB_N; i++) {
        if (!UI.tab_btn[i]) continue;
        lv_color_t c = (int)UI.tab == i && UI.view != MSH_V_CHAT ? AOS_C_ACCENT : AOS_C_DIM;
        lv_obj_t *b = UI.tab_btn[i];
        for (uint32_t k = 0; k < lv_obj_get_child_count(b); k++) {
            lv_obj_t *ch = lv_obj_get_child(b, (int32_t)k);
            if (ch != UI.badge) lv_obj_set_style_text_color(ch, c, 0);
        }
    }
}

static void build_body(void)
{
    if (UI.view != MSH_V_CHAT) msh_ui_chat_close();
    lv_obj_clean(UI.body);
    UI.map_obj = NULL;
    if (UI.bar) {
        if (UI.view == MSH_V_CHAT && !UI.land) lv_obj_add_flag(UI.bar, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(UI.bar, LV_OBJ_FLAG_HIDDEN);
    }
    int head_h = UI.land ? 80 : HEAD_H;
    int bar_h = !UI.land && !(UI.view == MSH_V_CHAT) ? BAR_H : 0;
    int x0 = UI.land ? RAIL_W : 0;
    lv_obj_set_pos(UI.body, x0, head_h);
    lv_obj_set_size(UI.body, UI.W - x0, UI.H - head_h - bar_h);
    switch (UI.view) {
    case MSH_V_CHAT: msh_ui_chat_build(UI.body); break;
    case MSH_V_NODE: msh_ui_node_build(UI.body); break;
    default:
        switch (UI.tab) {
        case MSH_TAB_CHATS: msh_ui_chats_build(UI.body); break;
        case MSH_TAB_NODES: msh_ui_nodes_build(UI.body); break;
        case MSH_TAB_MAP: msh_ui_map_build(UI.body); break;
        default: msh_ui_cfg_build(UI.body); break;
        }
    }
    msh_ui_header();
    style_tabs();
    update_badge();
    UI.dirty = false;
}

void msh_ui_show_tab(msh_tab_t t)
{
    UI.tab = t;
    UI.view = MSH_V_TAB;
    build_body();
}

void msh_ui_open_chat(uint32_t peer, int ch)
{
    UI.chat_peer = peer;
    UI.chat_ch = ch;
    UI.view = MSH_V_CHAT;
    msh_lock();
    msh_mark_read(peer, ch);
    msh_unlock();
    build_body();
}

void msh_ui_open_node(uint32_t num)
{
    UI.back_to = UI.view == MSH_V_CHAT ? MSH_V_CHAT : MSH_V_TAB;
    UI.node_num = num;
    UI.view = MSH_V_NODE;
    build_body();
}

bool msh_ui_back(void)
{
    if (UI.sheet) {
        msh_ui_sheet_close();
        return true;
    }
    if (UI.view == MSH_V_NODE && UI.back_to == MSH_V_CHAT) {
        UI.view = MSH_V_CHAT;
        UI.back_to = MSH_V_TAB;
        build_body();
        return true;
    }
    if (UI.view != MSH_V_TAB) {
        UI.view = MSH_V_TAB;
        build_body();
        return true;
    }
    return false;
}

void msh_ui_build(void)
{
    lv_obj_t *root = UI.root;
    lv_obj_clean(root);
    UI.sheet = NULL;
    UI.badge = NULL;
    memset(UI.tab_btn, 0, sizeof UI.tab_btn);
    UI.W = (int)lv_obj_get_width(root);
    UI.H = (int)lv_obj_get_height(root);
    UI.land = UI.W > UI.H;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    int head_h = UI.land ? 80 : HEAD_H;
    int x0 = UI.land ? RAIL_W : 0;

    /* the header: the title (a back arrow in a view) and the link's chip */
    UI.head = lv_obj_create(root);
    lv_obj_remove_style_all(UI.head);
    lv_obj_set_pos(UI.head, x0, 0);
    lv_obj_set_size(UI.head, UI.W - x0, head_h);
    lv_obj_remove_flag(UI.head, LV_OBJ_FLAG_SCROLLABLE);
    UI.title = msh_ui_label(UI.head, "", aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(UI.title, (UI.W - x0) - 340);
    lv_label_set_long_mode(UI.title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(UI.title, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_add_flag(UI.title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(UI.title, 20);
    lv_obj_add_event_cb(UI.title, back_cb, LV_EVENT_CLICKED, NULL);

    UI.chip = lv_obj_create(UI.head);
    lv_obj_remove_style_all(UI.chip);
    lv_obj_set_size(UI.chip, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_bg_color(UI.chip, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(UI.chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(UI.chip, 30, 0);
    lv_obj_set_style_pad_hor(UI.chip, 20, 0);
    lv_obj_set_flex_flow(UI.chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(UI.chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(UI.chip, 10, 0);
    lv_obj_align(UI.chip, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_obj_add_flag(UI.chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(UI.chip, chip_cb, LV_EVENT_CLICKED, NULL);
    UI.chip_dot = lv_obj_create(UI.chip);
    lv_obj_remove_style_all(UI.chip_dot);
    lv_obj_set_size(UI.chip_dot, 16, 16);
    lv_obj_set_style_radius(UI.chip_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(UI.chip_dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(UI.chip_dot, LV_OBJ_FLAG_CLICKABLE);
    UI.chip_txt = msh_ui_label(UI.chip, "", aos_font_small, AOS_C_TEXT);
    lv_obj_remove_flag(UI.chip_txt, LV_OBJ_FLAG_CLICKABLE);

    UI.body = lv_obj_create(root);
    lv_obj_remove_style_all(UI.body);
    lv_obj_remove_flag(UI.body, LV_OBJ_FLAG_SCROLLABLE);

    /* the tabs */
    UI.bar = lv_obj_create(root);
    lv_obj_remove_style_all(UI.bar);
    lv_obj_set_style_bg_color(UI.bar, lv_color_hex(0x111113), 0);
    lv_obj_set_style_bg_opa(UI.bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(UI.bar, LV_OBJ_FLAG_SCROLLABLE);
    if (UI.land) {
        lv_obj_set_pos(UI.bar, 0, 0);
        lv_obj_set_size(UI.bar, RAIL_W, UI.H);
        lv_obj_set_flex_flow(UI.bar, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_top(UI.bar, 12, 0);
    } else {
        lv_obj_set_pos(UI.bar, 0, UI.H - BAR_H);
        lv_obj_set_size(UI.bar, UI.W, BAR_H);
        lv_obj_set_flex_flow(UI.bar, LV_FLEX_FLOW_ROW);
    }
    for (int i = 0; i < MSH_TAB_N; i++) {
        lv_obj_t *b = lv_obj_create(UI.bar);
        lv_obj_remove_style_all(b);
        if (UI.land) lv_obj_set_size(b, RAIL_W, 150);
        else lv_obj_set_size(b, UI.W / MSH_TAB_N, BAR_H);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *s = msh_ui_label(b, TAB_SYM[i], aos_font_title, AOS_C_DIM);
        lv_obj_align(s, LV_ALIGN_TOP_MID, 0, UI.land ? 30 : 14);
        lv_obj_t *l = msh_ui_label(b, _(TAB_NAME[i]), aos_font_caption, AOS_C_DIM);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, UI.land ? 82 : 66);
        UI.tab_btn[i] = b;
        if (i == MSH_TAB_CHATS) {
            UI.badge = lv_obj_create(b);
            lv_obj_remove_style_all(UI.badge);
            lv_obj_set_size(UI.badge, LV_SIZE_CONTENT, 34);
            lv_obj_set_style_min_width(UI.badge, 34, 0);
            lv_obj_set_style_pad_hor(UI.badge, 8, 0);
            lv_obj_set_style_radius(UI.badge, 17, 0);
            lv_obj_set_style_bg_color(UI.badge, AOS_C_RED, 0);
            lv_obj_set_style_bg_opa(UI.badge, LV_OPA_COVER, 0);
            lv_obj_align(UI.badge, LV_ALIGN_TOP_MID, 30, UI.land ? 22 : 6);
            lv_obj_t *bl = msh_ui_label(UI.badge, "", aos_font_tiny, AOS_C_TEXT);
            lv_obj_center(bl);
            lv_obj_add_flag(UI.badge, LV_OBJ_FLAG_HIDDEN);
        }
    }
    build_body();
}

/* ---- from the timer ---- */

static bool finger_down(void)
{
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(in) == LV_INDEV_STATE_PRESSED)
            return true;
    return false;
}

void msh_ui_refresh(void)
{
    msh_lock();
    uint32_t sn = MSH.seq_nodes, sm = MSH.seq_msgs, sc = MSH.seq_cfg, ss = MSH.seq_state, st = MSH.seq_trace;
    uint32_t sno = MSH.seq_notice;
    msh_ask_t ask = MSH.ask;
    char notice[160];
    msh_scpy(notice, sizeof notice, MSH.notice);
    msh_unlock();

    if (ss != UI.seen_state) {
        UI.seen_state = ss;
        msh_ui_header();
        if (UI.view == MSH_V_TAB && UI.tab == MSH_TAB_NODE) UI.dirty = true;
    }
    if (ask != UI.seen_ask) {
        UI.seen_ask = ask;
        if (ask == MSH_ASK_PIN) msh_ui_pin_sheet();
        else if (UI.sheet_kind == MSH_SHEET_PIN) msh_ui_sheet_close();
    }
    if (sno != UI.seen_notice) {
        UI.seen_notice = sno;
        if (notice[0] && !UI.hidden) aos_ui_toast(notice, 3000);
    }
    bool nodes = sn != UI.seen_nodes, msgs = sm != UI.seen_msgs, cfg = sc != UI.seen_cfg, trace = st != UI.seen_trace;
    UI.seen_nodes = sn;
    UI.seen_msgs = sm;
    UI.seen_cfg = sc;
    UI.seen_trace = st;
    if (msgs) update_badge();
    if (UI.hidden) return;

    uint32_t now = msh_now_s();
    bool aged = now - UI.last_age_redraw >= 30;
    switch (UI.view) {
    case MSH_V_CHAT:
        if (msgs || nodes) {
            msh_lock();
            msh_mark_read(UI.chat_peer, UI.chat_ch);
            msh_unlock();
            msh_ui_chat_refresh();
        }
        if (cfg) msh_ui_header();
        break;
    case MSH_V_NODE:
        if (nodes || trace || aged) UI.dirty = true;
        break;
    default:
        if (UI.tab == MSH_TAB_CHATS && (msgs || cfg || nodes || aged)) UI.dirty = true;
        if (UI.tab == MSH_TAB_NODES && (nodes || aged)) UI.dirty = true;
        if (UI.tab == MSH_TAB_MAP && nodes && UI.map_obj) lv_obj_invalidate(UI.map_obj);
        if (UI.tab == MSH_TAB_NODE && (cfg || nodes)) UI.dirty = true;
        break;
    }
    if (aged) UI.last_age_redraw = now;
    /* nothing is pulled from under the finger: not with a sheet open, not
     * while the screen is pressed (a row rebuilt between press and release
     * loses its click) */
    if (UI.dirty && !UI.sheet && !finger_down()) {
        lv_obj_t *col = lv_obj_get_child(UI.body, 0);
        int32_t sy = col ? lv_obj_get_scroll_y(col) : 0;
        build_body();
        col = lv_obj_get_child(UI.body, 0);
        if (col && sy) {
            lv_obj_update_layout(col);
            lv_obj_scroll_to_y(col, sy, LV_ANIM_OFF);
        }
    }
}
