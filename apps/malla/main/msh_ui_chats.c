/*
 * Malla - the conversations: the list, and one open.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * A conversation is a channel (everyone with that channel's key hears it)
 * or a node (a direct message, encrypted to its public key when both ends
 * have one: the firmware does that by itself). The list has every channel
 * the node has, then the nodes we have messages with, newest first.
 *
 * Our messages say how far they got, as the node reports it: en cola (not
 * yet out of the node), enviado (it went on the air), recibido (the other
 * end said so) or, on a channel, repetido (a neighbour was heard passing it
 * on, which is the most a broadcast can know), or falló with the reason.
 * Holding a message down puts a reaction on it, as the phone apps do (an
 * emoji with the message's id as reply_id and the emoji flag).
 */
#include "msh_ui.h"

#include "aos_emoji.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHAT_SHOW   120             /* the newest messages drawn in a conversation */
#define DM_MAX      48

/* ---- the list ---- */

typedef struct {
    uint32_t peer;
    int ch;
    int last;                       /* index of the newest message, -1 none */
} conv_t;

static void conv_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    conv_t *c = (conv_t *)v;
    msh_ui_open_chat(c->peer, c->ch);
}

static conv_t s_convs[MSH_CHANNELS + DM_MAX];

static lv_obj_t *conv_row(lv_obj_t *col, conv_t *c)
{
    lv_obj_t *card = msh_ui_card(col);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(card, 18, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, conv_cb, LV_EVENT_CLICKED, c);

    if (c->peer) {
        msh_ui_avatar(card, c->peer, 72);
    } else {
        lv_obj_t *a = lv_obj_create(card);
        lv_obj_remove_style_all(a);
        lv_obj_set_size(a, 72, 72);
        lv_obj_set_style_radius(a, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(a, AOS_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        char n[4];
        snprintf(n, sizeof n, "%d", c->ch);
        lv_obj_t *l = msh_ui_label(a, n, aos_font_title, AOS_C_TEXT);
        lv_obj_center(l);
    }
    lv_obj_t *mid = lv_obj_create(card);
    lv_obj_remove_style_all(mid);
    lv_obj_set_flex_grow(mid, 1);
    lv_obj_set_height(mid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(mid, 4, 0);
    lv_obj_remove_flag(mid, LV_OBJ_FLAG_CLICKABLE);

    char name[48], safe[120], line[300], when[32] = "";
    if (c->peer) msh_node_name(c->peer, name, sizeof name);
    else msh_channel_name(c->ch, name, sizeof name);
    msh_ui_safe(safe, sizeof safe, name);
    lv_obj_t *nl = msh_ui_label(mid, safe, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(nl, LV_PCT(100));
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);

    msh_msg_t *m = c->last >= 0 ? msh_msg_at(c->last) : NULL;
    if (m) {
        char who[12] = "";
        if (m->from == MSH.my_num) snprintf(who, sizeof who, "%s: ", _("Vos"));
        else if (!c->peer) {
            char sn[8];
            snprintf(who, sizeof who, "%s: ", msh_node_short(m->from, sn, sizeof sn));
        }
        snprintf(line, sizeof line, "%s%s", who, m->text);
        msh_ui_when(m, when, sizeof when);
    } else {
        snprintf(line, sizeof line, "%s", c->peer ? "" : _("Sin mensajes todavía"));
    }
    char sl[300];
    msh_ui_safe(sl, sizeof sl, line);
    for (char *p = sl; *p; p++) if (*p == '\n') *p = ' ';
    lv_obj_t *ll = msh_ui_label(mid, sl, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(ll, LV_PCT(100));
    lv_label_set_long_mode(ll, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *right = lv_obj_create(card);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_row(right, 8, 0);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_CLICKABLE);
    msh_ui_label(right, when, aos_font_caption, AOS_C_DIM);
    int u = msh_unread(c->peer, c->ch);
    if (u) {
        lv_obj_t *b = lv_obj_create(right);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, LV_SIZE_CONTENT, 36);
        lv_obj_set_style_min_width(b, 36, 0);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_set_style_radius(b, 18, 0);
        lv_obj_set_style_bg_color(b, AOS_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        char n[8];
        snprintf(n, sizeof n, "%d", u);
        lv_obj_center(msh_ui_label(b, n, aos_font_caption, AOS_C_TEXT));
    }
    return card;
}

static void conn_cb(lv_event_t *e)
{
    (void)e;
    msh_ui_conn_sheet();
}

void msh_ui_chats_build(lv_obj_t *body)
{
    lv_obj_t *col = msh_ui_col(body);
    msh_lock();
    int n = 0;
    bool any_ch = false;
    for (int i = 0; i < MSH_CHANNELS; i++) any_ch |= MSH.ch[i].role != meshtastic_Channel_Role_DISABLED;
    for (int i = 0; i < MSH_CHANNELS; i++) {
        if (MSH.ch[i].role == meshtastic_Channel_Role_DISABLED && (any_ch || i)) continue;
        s_convs[n++] = (conv_t){ 0, i, -1 };
    }
    int first_dm = n;
    for (int i = 0; i < MSH.msg_n; i++) {
        msh_msg_t *m = msh_msg_at(i);
        int k;
        if (m->peer) {
            for (k = first_dm; k < n && s_convs[k].peer != m->peer; k++) {}
            if (k == n) {
                if (n >= (int)(sizeof s_convs / sizeof s_convs[0])) continue;
                s_convs[n++] = (conv_t){ m->peer, 0, -1 };
            }
        } else {
            for (k = 0; k < first_dm && s_convs[k].ch != m->ch; k++) {}
            if (k == first_dm) continue;            /* a channel the node no longer has */
        }
        if (!m->reaction) s_convs[k].last = i;
    }
    /* the DMs newest first */
    for (int a = first_dm; a < n; a++)
        for (int b = a + 1; b < n; b++)
            if (s_convs[b].last > s_convs[a].last) {
                conv_t t = s_convs[a];
                s_convs[a] = s_convs[b];
                s_convs[b] = t;
            }
    for (int i = 0; i < n; i++) {
        if (i == first_dm) msh_ui_label(col, _("Mensajes directos"), aos_font_small, AOS_C_DIM);
        conv_row(col, &s_convs[i]);
    }
    bool none = MSH.state != MSH_ST_READY && !MSH.msg_n;
    msh_unlock();
    if (none) {
        lv_obj_t *l = msh_ui_label(col, _("Conectá un nodo Meshtastic por USB, Wi-Fi o Bluetooth para chatear con la malla."),
                                   aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        msh_ui_btn(col, _("Conectar"), AOS_C_ACCENT, conn_cb, NULL);
    }
}

/* ---- one conversation ---- */

static void name_cb(lv_event_t *e)
{
    uint32_t num = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    msh_ui_open_node(num);
}

static void bubble_long_cb(lv_event_t *e)
{
    uint32_t id = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    if (id) msh_reaction_sheet(id);
}

static const char *state_text(const msh_msg_t *m, char *buf, size_t cap, lv_color_t *c)
{
    *c = AOS_C_DIM;
    switch (m->state) {
    case MSH_MSG_QUEUED: snprintf(buf, cap, "%s", _("en cola")); break;
    case MSH_MSG_SENT: snprintf(buf, cap, "%s", _("enviado")); break;
    case MSH_MSG_ACKED:
        *c = AOS_C_GREEN;
        snprintf(buf, cap, LV_SYMBOL_OK " %s", m->to == MSH_BROADCAST ? _("repetido") : _("recibido"));
        break;
    case MSH_MSG_FAILED:
        *c = AOS_C_RED;
        snprintf(buf, cap, LV_SYMBOL_WARNING " %s", _(msh_routing_error(m->err)));
        break;
    default: buf[0] = 0; break;
    }
    return buf;
}

/* The reactions put on message id, as "👍 Ana  ❤ Juan". */
static void reactions_of(uint32_t id, char *out, size_t cap)
{
    size_t o = 0;
    out[0] = 0;
    for (int i = 0; i < MSH.msg_n && id; i++) {
        msh_msg_t *r = msh_msg_at(i);
        if (!r->reaction || r->reply_id != id) continue;
        char sn[8];
        if (r->from == MSH.my_num) snprintf(sn, sizeof sn, "%s", _("Vos"));
        else msh_node_short(r->from, sn, sizeof sn);
        int w = snprintf(out + o, cap - o, "%s%s %s", o ? "   " : "", r->text, sn);
        if (w < 0 || (size_t)w >= cap - o) break;
        o += (size_t)w;
    }
}

static void bubble(lv_obj_t *list, const msh_msg_t *m)
{
    bool mine = m->from == MSH.my_num;
    lv_obj_t *wrap = lv_obj_create(list);
    lv_obj_remove_style_all(wrap);
    lv_obj_set_size(wrap, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          mine ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(wrap, 4, 0);
    lv_obj_remove_flag(wrap, LV_OBJ_FLAG_CLICKABLE);

    int maxw = (int)lv_obj_get_width(UI.body) * 78 / 100;
    char safe[MSH_TEXT_MAX * 2], tmp[120];
    if (!mine && !UI.chat_peer) {
        char name[48];
        msh_node_name(m->from, name, sizeof name);
        msh_ui_safe(tmp, sizeof tmp, name);
        lv_obj_t *nl = msh_ui_label(wrap, tmp, aos_font_caption, lv_color_hex(msh_node_rgb(m->from)));
        lv_obj_set_style_pad_left(nl, 14, 0);
        lv_obj_add_flag(nl, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(nl, 12);
        lv_obj_add_event_cb(nl, name_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)m->from);
    }
    lv_obj_t *b = lv_obj_create(wrap);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(b, maxw, 0);
    lv_obj_set_style_bg_color(b, mine ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_set_style_pad_ver(b, 14, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, bubble_long_cb, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)m->id);
    msh_ui_safe(safe, sizeof safe, m->text);
    lv_obj_t *tl = msh_ui_label(b, safe, aos_font_body, AOS_C_TEXT);
    lv_obj_set_style_max_width(tl, maxw - 40, 0);
    lv_obj_set_width(tl, LV_SIZE_CONTENT);
    lv_label_set_long_mode(tl, LV_LABEL_LONG_MODE_WRAP);

    char react[200];
    reactions_of(m->id, react, sizeof react);
    if (react[0]) {
        msh_ui_safe(safe, sizeof safe, react);
        lv_obj_t *rl = msh_ui_label(wrap, safe, aos_font_small, AOS_C_TEXT);
        lv_obj_set_style_pad_hor(rl, 14, 0);
    }

    char foot[120], when[32], st[64] = "";
    msh_ui_when(m, when, sizeof when);
    lv_color_t sc = AOS_C_DIM;
    if (mine) state_text(m, st, sizeof st, &sc);
    else if (m->hops >= 0 || m->snr) {
        if (m->hops == 1) snprintf(st, sizeof st, "%s", _("1 salto"));
        else if (m->hops > 1) snprintf(st, sizeof st, _("%d saltos"), m->hops);
        else if (m->hops == 0) snprintf(st, sizeof st, "SNR %d dB", m->snr);
    }
    snprintf(foot, sizeof foot, "%s%s%s", when, when[0] && st[0] ? "  ·  " : "", st);
    lv_obj_t *fl = msh_ui_label(wrap, foot, aos_font_caption, sc);
    lv_obj_set_style_pad_hor(fl, 14, 0);
}

static void fill_list(void)
{
    if (!UI.chat_list) return;
    lv_obj_t *list = UI.chat_list;
    int32_t bottom = lv_obj_get_scroll_bottom(list);
    lv_obj_clean(list);
    msh_lock();
    int idx[CHAT_SHOW], n = 0;
    for (int i = MSH.msg_n - 1; i >= 0 && n < CHAT_SHOW; i--) {
        msh_msg_t *m = msh_msg_at(i);
        if (m->reaction) continue;
        if (m->peer != UI.chat_peer || (!UI.chat_peer && m->ch != UI.chat_ch)) continue;
        idx[n++] = i;
    }
    for (int k = n - 1; k >= 0; k--) bubble(list, msh_msg_at(idx[k]));
    msh_unlock();
    if (!n) {
        lv_obj_t *l = msh_ui_label(list, UI.chat_peer ? _("Escribí el primer mensaje directo.")
                                                       : _("Todavía no hay mensajes en este canal."),
                                   aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, LV_PCT(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_update_layout(list);
    if (bottom < 80) lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
}

void msh_ui_chat_refresh(void)
{
    fill_list();
}

static void send_now(void)
{
    if (!UI.chat_ta) return;
    const char *raw = lv_textarea_get_text(UI.chat_ta);
    char text[MSH_TEXT_MAX + 16];
    msh_emoji_to_utf8(raw, text, sizeof text);
    /* no whitespace-only messages */
    const char *p = text;
    while (*p == ' ' || *p == '\n') p++;
    if (!*p) return;
    if (strlen(text) > MSH_TEXT_MAX - 1) {
        aos_ui_toast(_("Es demasiado largo para la malla"), 2000);
        return;
    }
    if (!msh_send_text(UI.chat_peer ? UI.chat_peer : MSH_BROADCAST, UI.chat_ch, text, 0, false)) {
        aos_ui_toast(_("No se pudo encolar el mensaje"), 2000);
        return;
    }
    lv_textarea_set_text(UI.chat_ta, "");
    fill_list();
    lv_obj_scroll_to_y(UI.chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void send_cb(lv_event_t *e)
{
    (void)e;
    send_now();
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) send_now();
    if (c == LV_EVENT_CANCEL && UI.chat_kb) lv_obj_add_flag(UI.chat_kb, LV_OBJ_FLAG_HIDDEN);
}

static void ta_cb(lv_event_t *e)
{
    (void)e;
    if (UI.chat_kb) lv_obj_remove_flag(UI.chat_kb, LV_OBJ_FLAG_HIDDEN);
    if (UI.chat_list) lv_obj_scroll_to_y(UI.chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

static void emoji_cb(lv_event_t *e)
{
    (void)e;
    msh_emoji_sheet(UI.chat_ta);
}

static void peer_cb(lv_event_t *e)
{
    (void)e;
    if (UI.chat_peer) msh_ui_open_node(UI.chat_peer);
}

void msh_ui_chat_build(lv_obj_t *body)
{
    lv_obj_t *col = lv_obj_create(body);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);

    if (UI.chat_peer) {
        /* who it is, a tap away */
        lv_obj_t *r = msh_ui_row(col);
        lv_obj_set_style_pad_hor(r, 20, 0);
        lv_obj_set_style_pad_bottom(r, 6, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(r, peer_cb, LV_EVENT_CLICKED, NULL);
        msh_lock();
        msh_node_t *n = msh_node_find(UI.chat_peer);
        char line[120] = "", age[32];
        if (n) {
            msh_ui_age(msh_node_age(n), age, sizeof age);
            char id[12];
            msh_node_id(n->num, id);
            snprintf(line, sizeof line, "%s  ·  %s  ·  %s", id, age, n->has_pkc ? _("cifrado de punta a punta") : _("con la clave del canal"));
        }
        msh_unlock();
        msh_ui_label(r, line, aos_font_caption, AOS_C_DIM);
    }

    UI.chat_list = lv_obj_create(col);
    lv_obj_remove_style_all(UI.chat_list);
    lv_obj_set_width(UI.chat_list, LV_PCT(100));
    lv_obj_set_flex_grow(UI.chat_list, 1);
    lv_obj_set_flex_flow(UI.chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(UI.chat_list, 16, 0);
    lv_obj_set_style_pad_row(UI.chat_list, 14, 0);
    lv_obj_set_scroll_dir(UI.chat_list, LV_DIR_VER);
    lv_obj_add_flag(UI.chat_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *in = msh_ui_row(col);
    lv_obj_set_style_pad_all(in, 12, 0);
    lv_obj_set_style_bg_color(in, lv_color_hex(0x111113), 0);
    lv_obj_set_style_bg_opa(in, LV_OPA_COVER, 0);
    char face[16] = ":)";
    if (aos_emoji_ready()) msh_ui_safe(face, sizeof face, "\xF0\x9F\x99\x82");     /* U+1F642 */
    lv_obj_t *eb = msh_ui_btn(in, face, AOS_C_CARD2, emoji_cb, NULL);
    lv_obj_set_width(eb, 84);
    UI.chat_ta = lv_textarea_create(in);
    lv_obj_set_flex_grow(UI.chat_ta, 1);
    lv_obj_set_height(UI.chat_ta, 80);
    lv_textarea_set_one_line(UI.chat_ta, true);
    lv_textarea_set_max_length(UI.chat_ta, 200);
    lv_textarea_set_placeholder_text(UI.chat_ta, _("Mensaje"));
    lv_obj_set_style_text_font(UI.chat_ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(UI.chat_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(UI.chat_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(UI.chat_ta, 0, 0);
    lv_obj_set_style_radius(UI.chat_ta, 24, 0);
    lv_obj_set_style_pad_hor(UI.chat_ta, 20, 0);
    lv_obj_add_event_cb(UI.chat_ta, ta_cb, LV_EVENT_CLICKED, NULL);
    UI.chat_send = msh_ui_btn(in, LV_SYMBOL_UPLOAD, AOS_C_ACCENT, send_cb, NULL);
    lv_obj_set_width(UI.chat_send, 96);

    UI.chat_kb = lv_keyboard_create(col);
    aos_keyboard_style(UI.chat_kb, aos_font_body);
    lv_obj_set_size(UI.chat_kb, LV_PCT(100), UI.land ? UI.H / 2 : 420);
    lv_keyboard_set_textarea(UI.chat_kb, UI.chat_ta);
    lv_obj_add_event_cb(UI.chat_kb, kb_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(UI.chat_kb, kb_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(UI.chat_kb, LV_OBJ_FLAG_HIDDEN);

    fill_list();
}

void msh_ui_chat_close(void)
{
    UI.chat_list = NULL;
    UI.chat_ta = NULL;
    UI.chat_kb = NULL;
    UI.chat_send = NULL;
}
