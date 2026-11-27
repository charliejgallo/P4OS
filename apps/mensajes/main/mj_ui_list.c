/*
 * P4OS - Mensajes: the chat list, the search and the settings sheet.
 *
 * The list is drawn again whole when the store's dialogs move (a message
 * arrived, one was read): fifty rows of a few objects each, cheaper than
 * keeping them in step one by one, and the scroll is kept. Each row has
 * its photo (mj_avatar), the name, the time, the last message ("Ana: ...",
 * "✓✓" when ours was read) and the unread count, or a pin.
 */
#include "aos_text_safe.h"
#include "mj_ui.h"

#define ROW_H 112

typedef struct {
    mj_pkey_t key;
    char name[MJ_NAME];
    char preview[120], who[40];
    uint8_t kind, action;
    bool out, read, pinned, muted, broadcast;
    int unread;
    int32_t date;
} row_t;

static row_t *s_rows;
static int s_n;

static bool match(const char *name, const char *f)
{
    if (!*f) return true;
    /* case-insensitive for ASCII; the rest as it is */
    size_t fl = strlen(f);
    for (const char *p = name; *p; p++) {
        size_t i = 0;
        while (i < fl && p[i]) {
            char a = p[i], b = f[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
            i++;
        }
        if (i == fl) return true;
    }
    return false;
}

static void copy_rows(void)
{
    mj_store_lock();
    row_t *n = (row_t *)mj_realloc(s_rows, (size_t)(mj_S.ndialogs + 1) * sizeof *n);
    if (n) {
        s_rows = n;
        s_n = 0;
        for (int i = 0; i < mj_S.ndialogs; i++) {
            mj_dialog_t *d = &mj_S.dialogs[i];
            row_t *r = &s_rows[s_n];
            memset(r, 0, sizeof *r);
            r->key = d->key;
            snprintf(r->name, sizeof r->name, "%s", mj_peer_name(d->key));
            if (!match(r->name, U.filter)) continue;
            snprintf(r->preview, sizeof r->preview, "%s", d->preview);
            snprintf(r->who, sizeof r->who, "%s", d->who);
            r->kind = d->preview_kind;
            r->action = d->preview_action;
            r->out = d->preview_out;
            r->read = d->preview_out && d->read_out_max >= d->top_id;
            r->pinned = d->pinned;
            r->muted = d->muted;
            r->unread = d->unread;
            r->date = d->date;
            mj_peer_t *p = mj_store_peer(d->key);
            r->broadcast = p && p->broadcast;
            if (p && p->self) snprintf(r->name, sizeof r->name, "%s", _("Mensajes guardados"));
            s_n++;
        }
    }
    mj_store_unlock();
}

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    mj_kb_hide();
    mj_ui_open_chat(s_rows[i].key);
}

/* What a row says of its last message, in words. */
static void preview_text(const row_t *r, char *out, size_t n)
{
    char body[160];
    if (r->action) {
        mj_msg_t f = {0};
        f.action = r->action;
        snprintf(f.action_arg, sizeof f.action_arg, "%s", r->preview);
        mj_action_words(&f, r->who[0] ? r->who : "", out, n);
        return;
    }
    if (r->kind == MJ_MEDIA_STICKER) {
        snprintf(body, sizeof body, "%s %s", r->preview, _("Sticker"));
    } else if (r->kind && r->kind != MJ_MEDIA_WEB) {
        mj_media_t md = {0};
        mj_msg_t f = {0};
        md.kind = (uint8_t)r->kind;
        f.media = &md;
        mj_media_words(&f, body, sizeof body);
        if (r->preview[0]) {
            /* the caption, or the file's name, after the media's emoji */
            char *sp = strchr(body, ' ');
            size_t keep = sp ? (size_t)(sp - body) : strlen(body);
            snprintf(body + keep, sizeof body - keep, " %s", r->preview);
        }
    } else {
        snprintf(body, sizeof body, "%s", r->preview);
    }
    if (r->out && r->key.type != MJ_PEER_USER) snprintf(out, n, "%s: %s", _("Vos"), body);
    else if (!r->out && r->who[0]) snprintf(out, n, "%s: %s", r->who, body);
    else snprintf(out, n, "%s", body);
}

static void build_row(lv_obj_t *list, int i, int32_t w)
{
    row_t *r = &s_rows[i];
    lv_obj_t *o = mj_box(list, w, ROW_H);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(o, AOS_C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
    if (U.land && mj_pkey_eq(r->key, U.open)) {
        lv_obj_set_style_bg_color(o, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    }
    lv_obj_add_event_cb(o, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *av = mj_avatar(o, r->key, 80);
    lv_obj_align(av, LV_ALIGN_LEFT_MID, 20, 0);
    int32_t tx = 120, tw = w - tx - 20;
    char t[16];
    mj_time_short(r->date, t, sizeof t);
    lv_obj_t *tl = mj_label(o, t, aos_font_caption, r->unread && !r->muted ? AOS_C_ACCENT : AOS_C_DIM);
    lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_t *nl = lv_label_create(o);
    lv_obj_set_style_text_font(nl, aos_font_body, 0);
    lv_obj_set_style_text_color(nl, AOS_C_TEXT, 0);
    char name[MJ_NAME + 8];
    snprintf(name, sizeof name, "%s%s", r->broadcast ? "\xF0\x9F\x93\xA2 " : "", r->name);
    mj_label_safe(nl, name);
    lv_obj_set_size(nl, tw - 110, lv_font_get_line_height(aos_font_body));
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(nl, tx, 16);
    char pv[200];
    preview_text(r, pv, sizeof pv);
    lv_obj_t *pl = lv_label_create(o);
    lv_obj_set_style_text_font(pl, aos_font_small, 0);
    lv_obj_set_style_text_color(pl, AOS_C_DIM, 0);
    mj_label_safe(pl, pv);
    lv_obj_set_size(pl, tw - 70, lv_font_get_line_height(aos_font_small));
    lv_label_set_long_mode(pl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(pl, tx, 60);
    if (r->unread) {
        char u[12];
        snprintf(u, sizeof u, "%d", r->unread > 999 ? 999 : r->unread);
        lv_obj_t *b = mj_box(o, LV_SIZE_CONTENT, 38);
        lv_obj_set_style_min_width(b, 38, 0);
        lv_obj_set_style_radius(b, 19, 0);
        lv_obj_set_style_bg_color(b, r->muted ? AOS_C_DIM : AOS_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_t *ul = mj_label(b, u, aos_font_caption, AOS_C_TEXT);
        lv_obj_center(ul);
        lv_obj_align(b, LV_ALIGN_BOTTOM_RIGHT, -20, -16);
    } else if (r->pinned) {
        lv_obj_t *pin = mj_label(o, AOS_SYM_PIN, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(pin, LV_ALIGN_BOTTOM_RIGHT, -22, -18);
    } else if (r->out) {
        lv_obj_t *ck = mj_label(o, r->read ? AOS_SYM_CHECK AOS_SYM_CHECK : AOS_SYM_CHECK, &aos_sym_28,
                                r->read ? AOS_C_ACCENT : AOS_C_DIM);
        lv_obj_set_style_text_letter_space(ck, -16, 0);
        lv_obj_align(ck, LV_ALIGN_BOTTOM_RIGHT, -20, -18);
    }
    /* the separator, under the text only */
    lv_obj_t *sep = mj_box(o, w - tx, 1);
    lv_obj_set_style_bg_color(sep, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    lv_obj_align(sep, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void found_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    mj_pkey_t k = {0, 0};
    mj_store_lock();
    if (i >= 1000 && i - 1000 < mj_S.nfound) k = mj_S.found[i - 1000]->chat;
    else if (i >= 0 && i < mj_S.nfound_peers) k = mj_S.found_peers[i];
    mj_store_unlock();
    if (!k.type) return;
    mj_kb_hide();
    mj_ui_open_chat(k);
}

static void section(lv_obj_t *list, const char *t, int32_t w)
{
    lv_obj_t *s = mj_box(list, w, 52);
    lv_obj_t *l = mj_label(s, t, aos_font_caption, AOS_C_DIM);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 24, -8);
}

static void build_found(lv_obj_t *list, int32_t w)
{
    mj_store_lock();
    int np = mj_S.nfound_peers, nm = mj_S.nfound;
    bool busy = mj_S.searching;
    mj_store_unlock();
    if (busy) {
        section(list, _("Buscando…"), w);
        return;
    }
    if (!np && !nm) {
        section(list, _("No se encontró nada"), w);
        return;
    }
    if (np) section(list, _("Chats y contactos"), w);
    for (int i = 0; i < np; i++) {
        mj_store_lock();
        mj_pkey_t k = mj_S.found_peers[i];
        char name[MJ_NAME];
        snprintf(name, sizeof name, "%s", mj_peer_name(k));
        mj_store_unlock();
        lv_obj_t *o = mj_box(list, w, 88);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(o, AOS_C_CARD, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(o, found_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *av = mj_avatar(o, k, 60);
        lv_obj_align(av, LV_ALIGN_LEFT_MID, 24, 0);
        lv_obj_t *l = lv_label_create(o);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
        mj_label_safe(l, name);
        lv_obj_set_width(l, w - 140);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 104, 0);
    }
    if (nm) section(list, _("Mensajes"), w);
    for (int i = 0; i < nm; i++) {
        mj_store_lock();
        mj_msg_t *m = mj_S.found[i];
        char name[MJ_NAME], line[160], t[16];
        snprintf(name, sizeof name, "%s", mj_peer_name(m->chat));
        mj_msg_line(m, line, sizeof line);
        mj_time_short(m->date, t, sizeof t);
        mj_pkey_t k = m->chat;
        mj_store_unlock();
        lv_obj_t *o = mj_box(list, w, 100);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(o, AOS_C_CARD, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(o, found_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(1000 + i));
        lv_obj_t *av = mj_avatar(o, k, 60);
        lv_obj_align(av, LV_ALIGN_LEFT_MID, 24, 0);
        lv_obj_t *nl = lv_label_create(o);
        lv_obj_set_style_text_font(nl, aos_font_small, 0);
        lv_obj_set_style_text_color(nl, AOS_C_TEXT, 0);
        mj_label_safe(nl, name);
        lv_obj_set_width(nl, w - 220);
        lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(nl, 104, 14);
        lv_obj_t *tl = mj_label(o, t, aos_font_caption, AOS_C_DIM);
        lv_obj_align(tl, LV_ALIGN_TOP_RIGHT, -24, 16);
        lv_obj_t *ml = lv_label_create(o);
        lv_obj_set_style_text_font(ml, aos_font_small, 0);
        lv_obj_set_style_text_color(ml, AOS_C_DIM, 0);
        mj_label_safe(ml, line);
        lv_obj_set_width(ml, w - 140);
        lv_label_set_long_mode(ml, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_pos(ml, 104, 54);
    }
}

static void list_scroll_cb(lv_event_t *e)
{
    lv_obj_t *l = lv_event_get_target(e);
    if (lv_obj_get_scroll_bottom(l) < 300) mj_api_more_dialogs();
}

void mj_list_refresh(void)
{
    if (!U.list) return;
    lv_obj_update_layout(U.list);
    int32_t keep = lv_obj_get_scroll_y(U.list);
    lv_obj_clean(U.list);
    int32_t w = lv_obj_get_width(U.list);
    if (U.show_found) {
        build_found(U.list, w);
        return;
    }
    copy_rows();
    for (int i = 0; i < s_n; i++) build_row(U.list, i, w);
    mj_store_lock();
    bool loading = mj_S.dialogs_loading;
    int nd = mj_S.ndialogs;
    mj_store_unlock();
    if (!s_n) section(U.list, loading ? _("Cargando los chats…") : nd ? _("Ningún chat se llama así") : _("Todavía no hay chats"), w);
    lv_obj_update_layout(U.list);
    lv_obj_scroll_to_y(U.list, keep, LV_ANIM_OFF);
}

void mj_list_net(void)
{
    if (!U.net_lbl) return;
    mj_store_lock();
    int net = mj_S.net;
    bool loading = mj_S.dialogs_loading;
    mj_store_unlock();
    const char *t = net == MJ_NET_UP ? (loading ? _("Actualizando…") : "")
                    : net == MJ_NET_KEY ? _("Conectando de forma segura…")
                                        : _("Esperando la red…");
    lv_label_set_text(U.net_lbl, t);
}

/* ---- the search ---- */

static void search_ready(lv_event_t *e)
{
    (void)e;
    char q[64];
    mj_scpy(q, sizeof q, lv_textarea_get_text(U.search_ta));
    mj_kb_hide();
    if (!q[0]) return;
    U.show_found = true;
    mj_api_search(q);
    mj_list_refresh();
}

static void search_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_CLICKED) {
        mj_kb_show(U.search_ta, LV_KEYBOARD_MODE_TEXT_LOWER, search_ready);
    } else if (c == LV_EVENT_VALUE_CHANGED) {
        snprintf(U.filter, sizeof U.filter, "%s", lv_textarea_get_text(U.search_ta));
        U.show_found = false;
        mj_list_refresh();
    }
}

static void gear_cb(lv_event_t *e)
{
    (void)e;
    mj_settings_open();
}

void mj_list_build(lv_obj_t *pane)
{
    lv_obj_update_layout(pane);
    int32_t w = lv_obj_get_width(pane), h = lv_obj_get_height(pane);
    lv_obj_t *t = mj_label(pane, _("Chats"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, AOS_UI_PAD, 18);
    U.list_title = t;
    U.net_lbl = mj_label(pane, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.net_lbl, LV_ALIGN_TOP_LEFT, AOS_UI_PAD + 150, 34);
    lv_obj_t *g = mj_icon_btn(pane, AOS_SYM_COG, 64, AOS_C_CARD2, gear_cb, NULL);
    lv_obj_align(g, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 14);
    U.search_ta = lv_textarea_create(pane);
    lv_textarea_set_one_line(U.search_ta, true);
    lv_obj_set_scrollbar_mode(U.search_ta, LV_SCROLLBAR_MODE_OFF);
    lv_textarea_set_placeholder_text(U.search_ta, _("Buscar"));
    lv_obj_set_size(U.search_ta, w - 2 * AOS_UI_PAD, 64);
    lv_obj_set_pos(U.search_ta, AOS_UI_PAD, 92);
    lv_obj_set_style_text_font(U.search_ta, aos_font_small, 0);
    lv_obj_set_style_text_color(U.search_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_color(U.search_ta, AOS_C_CARD2, 0);
    lv_obj_set_style_border_width(U.search_ta, 0, 0);
    lv_obj_set_style_radius(U.search_ta, 32, 0);
    lv_obj_set_style_pad_hor(U.search_ta, 24, 0);
    lv_obj_set_style_pad_ver(U.search_ta, 16, 0);
    if (U.filter[0]) lv_textarea_set_text(U.search_ta, U.filter);
    lv_obj_add_event_cb(U.search_ta, search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(U.search_ta, search_cb, LV_EVENT_VALUE_CHANGED, NULL);
    U.list = mj_box(pane, w, h - 172);
    lv_obj_set_pos(U.list, 0, 172);
    lv_obj_add_flag(U.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(U.list, LV_DIR_VER);
    lv_obj_set_flex_flow(U.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_event_cb(U.list, list_scroll_cb, LV_EVENT_SCROLL_END, NULL);
    mj_list_refresh();
    mj_list_net();
}

/* ---- settings ---- */

static void sw_notif_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    U.notif_on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    aos_hal_pref_set_i32("msj_notif", U.notif_on ? 1 : 0);
}

static void sw_boot_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    aos_ui_boot_start_set(MJ_APP_ID, lv_obj_has_state(sw, LV_STATE_CHECKED));
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    mj_api_cache_clear();
    mj_sheet_close();
    aos_ui_toast(_("Se vació la caché"), 2000);
}

static void logout_yes(lv_event_t *e)
{
    (void)e;
    mj_sheet_close();
    mj_ui_close_chat();
    mj_api_logout();
}

static void logout_cb(lv_event_t *e)
{
    (void)e;
    lv_obj_t *b = mj_sheet_open(_("¿Cerrar la sesión?"), 420);
    lv_obj_t *l = mj_label(b, _("La placa se desconecta de tu cuenta. Para volver hay que entrar de nuevo con el código."),
                           aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    mj_pill_btn(b, _("Cerrar la sesión"), AOS_C_RED, logout_yes, NULL);
}

static lv_obj_t *set_row(lv_obj_t *b, const char *name, const char *val)
{
    lv_obj_t *r = mj_box(b, lv_pct(100), 84);
    lv_obj_t *n = mj_label(r, name, aos_font_body, AOS_C_TEXT);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
    if (val) {
        lv_obj_t *v = lv_label_create(r);
        lv_obj_set_style_text_font(v, aos_font_small, 0);
        lv_obj_set_style_text_color(v, AOS_C_DIM, 0);
        mj_label_safe(v, val);
        lv_obj_set_size(v, U.land ? 380 : 400, lv_font_get_line_height(aos_font_small));
        lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    return r;
}

static void set_switch(lv_obj_t *b, const char *name, bool on, lv_event_cb_t cb)
{
    lv_obj_t *r = set_row(b, name, NULL);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
}

void mj_settings_open(void)
{
    lv_obj_t *b = mj_sheet_open(_("Ajustes"), U.land ? 680 : 980);
    char v[120];
    mj_store_lock();
    mj_peer_t *me = mj_store_peer(mj_S.self);
    char name[MJ_NAME] = "", phone[24] = "", user[40] = "";
    if (me) {
        snprintf(name, sizeof name, "%s", me->name);
        if (me->phone[0]) snprintf(phone, sizeof phone, "+%s", me->phone);
        if (me->username[0]) snprintf(user, sizeof user, "@%s", me->username);
    }
    int dc = mj_S.home_dc;
    mj_store_unlock();
    set_row(b, _("Cuenta"), name);
    if (phone[0]) set_row(b, _("Teléfono"), phone);
    if (user[0]) set_row(b, _("Usuario"), user);
    set_switch(b, _("Avisos de mensajes nuevos"), U.notif_on, sw_notif_cb);
    set_switch(b, _("Arrancar con la placa"), aos_ui_boot_start_get(MJ_APP_ID), sw_boot_cb);
    uint32_t files = 0, bytes = 0;
    mj_api_cache_info(&files, &bytes);
    char sz[24];
    mj_size_words(bytes, sz, sizeof sz);
    snprintf(v, sizeof v, _("%u archivos, %s"), (unsigned)files, sz);
    set_row(b, _("Caché de fotos"), v);
    lv_obj_t *clr = mj_pill_btn(b, _("Vaciar la caché"), AOS_C_CARD2, clear_cb, NULL);
    (void)clr;
    snprintf(v, sizeof v, _("%u KB (pico %u) y %u KB de fotos"), (unsigned)(mj_mem_used() / 1024),
             (unsigned)(mj_mem_peak() / 1024), (unsigned)(mj_img_bytes() / 1024));
    set_row(b, _("Memoria"), v);
    char srv[200];
    snprintf(srv, sizeof srv, "%s/servidor.txt", U.data_dir);
    snprintf(v, sizeof v, mj_file_exists(srv) ? _("de prueba, DC %d") : _("Telegram, DC %d"), dc);
    set_row(b, _("Servidor"), v);
    lv_obj_t *note = mj_label(b, _("Un cliente no oficial para Telegram, escrito para esta placa."), aos_font_caption,
                              AOS_C_DIM);
    lv_obj_set_width(note, lv_pct(100));
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    mj_pill_btn(b, _("Cerrar la sesión"), AOS_C_RED, logout_cb, NULL);
}

/* ---- forwarding: to which chat ---- */

static mj_pkey_t s_fw_from;
static int32_t s_fw_id;

static void fw_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_n) return;
    mj_api_forward(s_fw_from, s_fw_id, s_rows[i].key);
    mj_sheet_close();
}

void mj_forward_pick(mj_pkey_t from, int32_t id)
{
    s_fw_from = from;
    s_fw_id = id;
    char keep[64];
    snprintf(keep, sizeof keep, "%s", U.filter);
    U.filter[0] = 0;
    copy_rows();
    snprintf(U.filter, sizeof U.filter, "%s", keep);
    lv_obj_t *b = mj_sheet_open(_("Reenviar a…"), U.land ? 680 : 1000);
    for (int i = 0; i < s_n; i++) {
        if (s_rows[i].broadcast) continue;
        lv_obj_t *o = mj_box(b, lv_pct(100), 88);
        lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(o, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(o, fw_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *av = mj_avatar(o, s_rows[i].key, 60);
        lv_obj_align(av, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *l = lv_label_create(o);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, AOS_C_TEXT, 0);
        mj_label_safe(l, s_rows[i].name);
        lv_obj_set_width(l, (U.land ? 720 : U.W) - 160);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_LEFT_MID, 80, 0);
    }
}

void mj_list_free(void)
{
    mj_free(s_rows);
    s_rows = NULL;
    s_n = 0;
}
