/*
 * P4OS - Correo: the screens' frame, the mailboxes and the lists (cr_ui.h).
 *
 * The lists are made from the model's headers each time it changes
 * (CR.ver): every INBOX for the unified one, or a folder, grouped by
 * conversation when the option says so, newest first. Rows are built in
 * pages of PAGE_ROWS as the list scrolls, so a folder of thousands opens as
 * fast as one of ten. Typing in the search field filters what is on the
 * board at once; "search the server" asks IMAP (UID SEARCH TEXT) and shows
 * its answer instead.
 */
#include "cr_ui.h"

#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>

#define PAGE_ROWS 60

cr_ui_t U;

/* ---- building blocks --------------------------------------------------------------------------- */

lv_obj_t *ui_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = aos_label(parent, t, f, c);
    aos_make_decorative(l);
    return l;
}

lv_obj_t *ui_label_dots(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c, int32_t w)
{
    lv_obj_t *l = ui_label(parent, t, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, lv_font_get_line_height(f));
    return l;
}

lv_obj_t *ui_label_wrap(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = ui_label(parent, t, f, c);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

lv_obj_t *ui_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ui_box(parent, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) ui_label(b, glyph, &aos_sym_28, AOS_C_TEXT);
    if (text) ui_label(b, text, aos_font_body, AOS_C_TEXT);
    return b;
}

lv_obj_t *ui_icon_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ui_box(parent, 76, 76);
    lv_obj_set_style_radius(b, 38, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = ui_label(b, glyph, &aos_sym_28, AOS_C_ACCENT);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = ui_box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = ui_label(c, text, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    return c;
}

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = ui_box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    return c;
}

lv_obj_t *ui_col(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = ui_box(parent, w, h);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_hor(c, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(c, 8, 0);
    lv_obj_set_style_pad_bottom(c, 60, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    return c;
}

int32_t ui_page_w(void)
{
    return U.page ? lv_obj_get_style_width(U.page, 0) : U.W;
}

int32_t ui_page_h(void)
{
    return U.page ? lv_obj_get_style_height(U.page, 0) : U.H;
}

#define BAR_H 96

/* A title bar: "‹ back" on the left, the title, and a row for buttons on
 * the right. */
lv_obj_t *ui_bar(lv_obj_t *page, const char *back, const char *title, lv_event_cb_t back_cb)
{
    int32_t w = lv_obj_get_style_width(page, 0);
    lv_obj_t *bar = ui_box(page, w, BAR_H);
    lv_obj_set_style_pad_hor(bar, 12, 0);
    int32_t used = 0;
    if (back) {
        lv_obj_t *b = ui_box(bar, LV_SIZE_CONTENT, 80);
        lv_obj_set_style_pad_hor(b, 8, 0);
        lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(b, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, back_cb, LV_EVENT_CLICKED, NULL);
        ui_label(b, AOS_SYM_CHEVRON_LEFT, &aos_sym_28, AOS_C_ACCENT);
        lv_obj_t *bl = ui_label(b, back, aos_font_body, AOS_C_ACCENT);
        lv_obj_update_layout(bl);
        int32_t bw = lv_obj_get_width(bl);
        int32_t bmax = w < 600 ? w / 3 : w / 4;
        if (bw > bmax) {
            lv_label_set_long_mode(bl, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_size(bl, bmax, lv_font_get_line_height(aos_font_body));
            bw = bmax;
        }
        lv_obj_align(b, LV_ALIGN_LEFT_MID, 0, 0);
        used = bw + 60;
    }
    if (title) {
        /* centred between the back button and the right ones, whichever is wider */
        int32_t side = used > 190 ? used : 190;
        int32_t tw = w - 2 * side;
        lv_obj_t *t = ui_label_dots(bar, title, aos_font_body, AOS_C_TEXT, tw > 160 ? tw : 160);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(t, LV_ALIGN_CENTER, 0, 0);
    }
    lv_obj_t *r = ui_box(bar, LV_SIZE_CONTENT, 80);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 4, 0);
    lv_obj_align(r, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_user_data(bar, r);
    return bar;
}

lv_obj_t *ui_bar_right(lv_obj_t *bar)
{
    return lv_obj_get_user_data(bar);
}

/* A labelled text area. */
lv_obj_t *ui_field(lv_obj_t *parent, const char *label, const char *text, const char *hint, bool one_line)
{
    if (label) ui_label(parent, label, aos_font_small, AOS_C_DIM);
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, lv_pct(100));
    lv_obj_set_style_text_font(ta, aos_font_body, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 18, 0);
    lv_obj_set_style_pad_all(ta, 16, 0);
    lv_textarea_set_one_line(ta, one_line);
    if (hint) lv_textarea_set_placeholder_text(ta, hint);
    if (text) lv_textarea_set_text(ta, text);
    ui_kb_watch(ta);
    return ta;
}

lv_obj_t *ui_switch_row(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb)
{
    lv_obj_t *row = ui_box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(row, 84, 0);
    lv_obj_t *l = ui_label(row, text, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(78));
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 96, 52);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, AOS_C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

/* ---- the keyboard ------------------------------------------------------------------------------ */

static struct {
    lv_obj_t *page, *content, *kb, *ta;
} KB;

static void kb_done_cb(lv_event_t *e)
{
    (void)e;
    ui_kb_hide();
}

void ui_kb_attach(lv_obj_t *page, lv_obj_t *content)
{
    KB.page = page;
    KB.content = content;
    KB.kb = lv_keyboard_create(page);
    aos_keyboard_style(KB.kb, aos_font_body);
    int32_t pw = lv_obj_get_style_width(page, 0);
    lv_obj_set_size(KB.kb, pw, U.land ? U.H * 46 / 100 : U.H * 32 / 100);
    lv_obj_align(KB.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(KB.kb, kb_done_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(KB.kb, kb_done_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
    KB.ta = NULL;
}

static void kb_show(lv_obj_t *ta)
{
    if (!KB.kb || !ta) return;
    KB.ta = ta;
    lv_keyboard_set_textarea(KB.kb, ta);
    lv_keyboard_set_mode(KB.kb, LV_KEYBOARD_MODE_TEXT_LOWER);
    lv_obj_remove_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(KB.kb);
    if (KB.content) {
        int32_t y = lv_obj_get_y(KB.content);
        lv_obj_set_height(KB.content, lv_obj_get_style_height(KB.page, 0) - y - lv_obj_get_height(KB.kb));
        lv_obj_update_layout(KB.content);
        lv_obj_scroll_to_view_recursive(ta, LV_ANIM_ON);
    }
}

static void ta_focus_cb(lv_event_t *e)
{
    kb_show(lv_event_get_target(e));
}

void ui_kb_watch(lv_obj_t *ta)
{
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, ta_focus_cb, LV_EVENT_CLICKED, NULL);
}

void ui_kb_hide(void)
{
    if (!KB.kb) return;
    lv_obj_add_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(KB.kb, NULL);
    if (KB.ta) lv_obj_remove_state(KB.ta, LV_STATE_FOCUSED);
    KB.ta = NULL;
    if (KB.content) lv_obj_set_height(KB.content, lv_obj_get_style_height(KB.page, 0) - lv_obj_get_y(KB.content));
}

bool ui_kb_visible(void)
{
    return KB.kb && !lv_obj_has_flag(KB.kb, LV_OBJ_FLAG_HIDDEN);
}

static void kb_forget(void)
{
    memset(&KB, 0, sizeof KB);
}

/* ---- sheets ------------------------------------------------------------------------------------ */

static struct {
    lv_obj_t    *ov;
    ui_pick_cb_t cb;
    void        *ud;
    int          choice;
    bool         pending;
} SH;

static void sheet_fire(void *unused)
{
    (void)unused;
    SH.pending = false;
    lv_obj_t *ov = SH.ov;
    SH.ov = NULL;
    if (ov) lv_obj_delete(ov);
    ui_pick_cb_t cb = SH.cb;
    SH.cb = NULL;
    if (cb && SH.choice >= 0) cb(SH.choice, SH.ud);
}

static void sheet_finish(int choice)
{
    if (SH.pending) return;
    SH.choice = choice;
    if (SH.ov) lv_obj_add_flag(SH.ov, LV_OBJ_FLAG_HIDDEN);
    SH.pending = true;
    lv_async_call(sheet_fire, NULL);
}

void ui_sheet_close(void)
{
    if (!SH.ov || SH.pending) return;
    SH.cb = NULL;
    sheet_finish(-1);
}

bool ui_sheet_open(void)
{
    return SH.ov && !SH.pending;
}

static void sheet_forget(void)
{
    if (SH.pending) lv_async_call_cancel(sheet_fire, NULL);
    memset(&SH, 0, sizeof SH);
}

static void outside_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) != lv_event_get_current_target(e)) return;
    SH.cb = NULL;
    sheet_finish(-1);
}

static lv_obj_t *sheet_panel(void)
{
    if (SH.ov) lv_obj_delete(SH.ov);
    SH.ov = ui_box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(SH.ov, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(SH.ov, LV_OPA_60, 0);
    lv_obj_add_flag(SH.ov, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(SH.ov, outside_cb, LV_EVENT_CLICKED, NULL);
    int32_t w = U.land ? U.W * 55 / 100 : U.W;
    lv_obj_t *p = ui_box(SH.ov, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(p, U.H * 85 / 100, 0);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    lv_obj_set_style_pad_bottom(p, 40, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(p, 10, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_align(p, LV_ALIGN_BOTTOM_MID, 0, AOS_UI_RADIUS);
    return p;
}

static void item_cb(lv_event_t *e)
{
    sheet_finish((int)(intptr_t)lv_event_get_user_data(e));
}

static lv_obj_t *sheet_row(lv_obj_t *p, const char *text, int i, lv_color_t c)
{
    int32_t pw = U.land ? U.W * 55 / 100 : U.W;
    lv_obj_t *row = ui_box(p, lv_pct(100), 84);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x3A3A3C), LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_t *l = ui_label_dots(row, text, aos_font_body, c, pw - 100);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 22, 0);
    return row;
}

void ui_sheet(const char *title, const char *const *items, int n, ui_pick_cb_t cb, void *ud)
{
    if (SH.pending) return;
    ui_kb_hide();
    lv_obj_t *p = sheet_panel();
    SH.cb = cb;
    SH.ud = ud;
    if (title) ui_label_wrap(p, title, aos_font_small, AOS_C_DIM);
    for (int i = 0; i < n; i++) {
        bool red = items[i][0] == '!';
        sheet_row(p, red ? items[i] + 1 : items[i], i, red ? AOS_C_RED : AOS_C_TEXT);
    }
}

void ui_confirm(const char *text, const char *yes, bool danger, ui_pick_cb_t cb, void *ud)
{
    if (SH.pending) return;
    ui_kb_hide();
    lv_obj_t *p = sheet_panel();
    SH.cb = cb;
    SH.ud = ud;
    ui_label_wrap(p, text, aos_font_body, AOS_C_TEXT);
    sheet_row(p, yes, 0, danger ? AOS_C_RED : AOS_C_ACCENT);
    sheet_row(p, _("Cancelar"), -1, AOS_C_TEXT);
}

void ui_qr_sheet(const char *title, const char *url)
{
    if (SH.pending) return;
    lv_obj_t *p = sheet_panel();
    SH.cb = NULL;
    lv_obj_set_flex_align(p, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (title) ui_label_wrap(p, title, aos_font_small, AOS_C_DIM);
    int32_t side = U.land ? 300 : 380;
    lv_obj_t *q = lv_qrcode_create(p);
    lv_qrcode_set_size(q, side);
    lv_qrcode_set_dark_color(q, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(q, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_border_color(q, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(q, 12, 0);
    lv_qrcode_update(q, url, (uint32_t)strlen(url));
    char safe[600];
    aos_text_safe(safe, sizeof safe, url);
    lv_obj_t *l = ui_label_wrap(p, safe, aos_font_small, AOS_C_ACCENT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    ui_label_wrap(p, _("La placa no abre enlaces: escanealo con el teléfono para abrirlo ahí."), aos_font_caption,
                  AOS_C_DIM);
    sheet_row(p, _("Cerrar"), -1, AOS_C_TEXT);
}

/* ---- words ------------------------------------------------------------------------------------- */

const char *ui_err_text(int kind)
{
    switch (kind) {
    case CR_E_NET: return _("No se pudo llegar al servidor");
    case CR_E_TLS: return _("La conexión segura falló (certificado o cifrado)");
    case CR_E_CLOCK: return _("La placa todavía no tiene hora: sin hora no se puede verificar el certificado");
    case CR_E_AUTH: return _("El servidor no aceptó el usuario o la contraseña");
    case CR_E_PROTO: return _("El servidor contestó algo inesperado");
    case CR_E_STARTTLS:
        return _("Este firmware todavía no cifra con STARTTLS (puertos 143 y 587). Usá SSL/TLS (993 y 465) si el "
                 "servidor lo tiene, o esperá la próxima versión.");
    case CR_E_NOPASS: return _("Falta la contraseña");
    }
    return "";
}

const char *ui_state_text(const cr_acct_t *a)
{
    switch (a->state) {
    case CR_ST_NO_PASS: return _("Falta la contraseña");
    case CR_ST_CONNECTING: return _("Conectando...");
    case CR_ST_ONLINE: return _("Al día");
    case CR_ST_IDLE: return _("Al día, esperando correo");
    case CR_ST_ERROR: return ui_err_text(a->err_kind);
    }
    return aos_hal_net_state() == AOS_NET_CONNECTED ? _("Sin conexión") : _("Sin red");
}

void ui_size(char *out, size_t n, uint32_t b)
{
    if (b < 1024) snprintf(out, n, "%lu B", (unsigned long)b);
    else if (b < 1024 * 1024) snprintf(out, n, "%lu KB", (unsigned long)((b + 512) / 1024));
    else snprintf(out, n, "%lu,%lu MB", (unsigned long)(b / 1048576), (unsigned long)(b % 1048576 * 10 / 1048576));
}

const char *ui_acct_name(int i)
{
    const cr_acct_cfg_t *c = &CR.a[i].cfg;
    return c->label[0] ? c->label : c->email;
}

const char *cr_folder_label(const cr_folder_t *f)
{
    switch (f->special) {
    case CR_SP_INBOX: return _("Entrada");
    case CR_SP_SENT: return _("Enviados");
    case CR_SP_DRAFTS: return _("Borradores");
    case CR_SP_ARCHIVE: return _("Archivo");
    case CR_SP_TRASH: return _("Papelera");
    case CR_SP_JUNK: return _("No deseado");
    case CR_SP_ALL: return _("Todos los mensajes");
    case CR_SP_FLAGGED: return _("Destacados");
    }
    return f->name;
}

const char *cr_folder_glyph(const cr_folder_t *f)
{
    switch (f->special) {
    case CR_SP_INBOX: return AOS_SYM_DOWNLOAD;
    case CR_SP_SENT: return AOS_SYM_SEND;
    case CR_SP_DRAFTS: return AOS_SYM_PENCIL;
    case CR_SP_ARCHIVE: return AOS_SYM_PACKAGE_VARIANT;
    case CR_SP_TRASH: return AOS_SYM_DELETE;
    case CR_SP_JUNK: return AOS_SYM_ALERT_OUTLINE;
    case CR_SP_ALL: return AOS_SYM_DATABASE;
    case CR_SP_FLAGGED: return AOS_SYM_STAR;
    }
    return AOS_SYM_FOLDER;
}

/* ---- navigation -------------------------------------------------------------------------------- */

uint32_t ui_token(int own)
{
    return TOKEN(own, ++U.seq);
}

int ui_top(void)
{
    return U.depth ? U.stack[U.depth - 1] : SCR_HOME;
}

static void build_into(int scr, lv_obj_t *page)
{
    U.page = page;
    switch (scr) {
    case SCR_WELCOME: cr_welcome_build(page); break;
    case SCR_HOME: cr_home_build(page); break;
    case SCR_LIST: cr_list_build(page); break;
    case SCR_MSG: cr_msg_build(page); break;
    case SCR_COMPOSE: cr_compose_build(page); break;
    case SCR_SETTINGS: cr_settings_build(page); break;
    case SCR_ACCT: cr_acct_build(page); break;
    case SCR_PICK: cr_pick_build(page); break;
    }
}

static lv_obj_t *page_at(int32_t x, int32_t w)
{
    lv_obj_t *p = ui_box(U.root, w, U.H);
    lv_obj_set_pos(p, x, 0);
    lv_obj_set_style_bg_color(p, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    return p;
}

void ui_rebuild(void)
{
    if (!U.root || U.building) return;
    U.building = true;
    sheet_forget();
    kb_forget();
    U.l_list = U.l_more = U.l_search = NULL;
    lv_obj_clean(U.root);
    U.W = lv_obj_get_width(U.root);
    U.H = lv_obj_get_height(U.root);
    U.land = U.W > U.H;
    U.left = NULL;
    int top = ui_top();
    if (!CR.na && top != SCR_ACCT && top != SCR_SETTINGS) {
        U.depth = 1;
        U.stack[0] = SCR_WELCOME;
        top = SCR_WELCOME;
    }
    /* a message beside its list, when the screen is wide */
    if (U.land && top == SCR_MSG && U.depth >= 2 && U.stack[U.depth - 2] == SCR_LIST) {
        int32_t lw = U.W * 40 / 100;
        U.left = page_at(0, lw);
        build_into(SCR_LIST, U.left);
        lv_obj_t *sep = ui_box(U.root, 2, U.H);
        lv_obj_set_pos(sep, lw, 0);
        lv_obj_set_style_bg_color(sep, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        build_into(SCR_MSG, page_at(lw + 2, U.W - lw - 2));
    } else {
        build_into(top, page_at(0, U.W));
    }
    U.ver_seen = CR.ver;
    U.building = false;
}

void ui_go(int scr)
{
    if (U.depth < (int)(sizeof U.stack / sizeof U.stack[0])) U.stack[U.depth++] = scr;
    ui_rebuild();
}

void ui_pop(void)
{
    int top = ui_top();
    if (top == SCR_MSG) cr_msg_close();
    if (top == SCR_COMPOSE) cr_compose_free();
    if (top == SCR_LIST) cr_list_free();
    if (U.depth > 1) U.depth--;
    else U.stack[0] = CR.na ? SCR_HOME : SCR_WELCOME;
    ui_rebuild();
}


/* ---- actions on a message ---------------------------------------------------------------------- */

void cr_act_flags(int acct, int folder, uint32_t uid, uint16_t add, uint16_t del)
{
    cr_lock();
    if (acct < CR.na && folder < CR.a[acct].nf) {
        cr_folder_t *f = &CR.a[acct].f[folder];
        cr_hdr_t *h = cr_folder_hdr(f, uid);
        if (h) {
            bool was_seen = h->flags & CR_F_SEEN;
            h->flags = (uint16_t)((h->flags | add) & ~del);
            bool seen = h->flags & CR_F_SEEN;
            if (was_seen && !seen) f->unseen++;
            if (!was_seen && seen && f->unseen) f->unseen--;
            if (f->special == CR_SP_INBOX) CR.a[acct].unseen = f->unseen;
            f->dirty = true;
        }
        cr_touch();
    }
    cr_unlock();
    cr_job_t *j = cr_job_new(CR_J_FLAGS);
    if (!j) return;
    j->acct = acct;
    j->folder = folder;
    j->uid = uid;
    j->arg = add;
    j->arg2 = del;
    j->token = ui_token(OWN_NONE);
    cr_job_put(j);
}

void cr_act_move(int acct, int folder, uint32_t uid, int dest)
{
    if (dest < 0 || dest == folder) return;
    cr_lock();
    cr_folder_drop(&CR.a[acct].f[folder], uid);
    cr_touch();
    cr_unlock();
    cr_job_t *j = cr_job_new(CR_J_MOVE);
    if (!j) return;
    j->acct = acct;
    j->folder = folder;
    j->uid = uid;
    j->arg = dest;
    j->token = ui_token(OWN_LIST);
    cr_job_put(j);
}

void cr_act_archive(int acct, int folder, uint32_t uid)
{
    cr_lock();
    int a = cr_folder_special(&CR.a[acct], CR_SP_ARCHIVE);
    if (a < 0) a = cr_folder_special(&CR.a[acct], CR_SP_ALL);
    cr_unlock();
    if (a < 0) {
        aos_ui_toast(_("La cuenta no tiene carpeta de archivo"), 2500);
        return;
    }
    cr_act_move(acct, folder, uid, a);
    aos_ui_toast(_("Archivado"), 1200);
}

void cr_act_delete(int acct, int folder, uint32_t uid)
{
    cr_lock();
    cr_folder_drop(&CR.a[acct].f[folder], uid);
    cr_touch();
    cr_unlock();
    cr_job_t *j = cr_job_new(CR_J_DELETE);
    if (!j) return;
    j->acct = acct;
    j->folder = folder;
    j->uid = uid;
    j->token = ui_token(OWN_LIST);
    cr_job_put(j);
    aos_ui_toast(_("Borrado"), 1200);
}

static struct {
    int      acct, folder;
    uint32_t uid;
    int      map[CR_FOLDER_MAX];
} MP;

static void move_pick_cb(int i, void *ud)
{
    (void)ud;
    cr_act_move(MP.acct, MP.folder, MP.uid, MP.map[i]);
    aos_ui_toast(_("Movido"), 1200);
    if (ui_top() == SCR_MSG) ui_pop();
}

void cr_act_move_pick(int acct, int folder, uint32_t uid)
{
    static char names[CR_FOLDER_MAX][110];
    static const char *items[CR_FOLDER_MAX];
    int n = 0;
    MP.acct = acct;
    MP.folder = folder;
    MP.uid = uid;
    cr_lock();
    cr_acct_t *a = &CR.a[acct];
    for (int i = 0; i < a->nf && n < CR_FOLDER_MAX; i++) {
        if (i == folder || a->f[i].noselect || a->f[i].gone) continue;
        snprintf(names[n], sizeof names[n], "%*s%s", a->f[i].depth * 2, "", cr_folder_label(&a->f[i]));
        items[n] = names[n];
        MP.map[n++] = i;
    }
    cr_unlock();
    ui_sheet(_("Mover a"), items, n, move_pick_cb, NULL);
}

/* ---- welcome and the mailboxes ----------------------------------------------------------------- */

static void add_acct_cb(lv_event_t *e)
{
    (void)e;
    cr_acct_edit(-1);
}

void cr_welcome_build(lv_obj_t *page)
{
    lv_obj_t *col = ui_col(page, ui_page_w(), U.H);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(col, U.land ? 200 : 60, 0);
    lv_obj_t *g = ui_label(col, AOS_SYM_SEND, &aos_sym_72, AOS_C_ACCENT);
    (void)g;
    ui_label(col, _("Correo"), aos_font_title, AOS_C_TEXT);
    lv_obj_t *t = ui_label_wrap(col,
                                _("Tu correo en la placa: varias cuentas, la bandeja de todas juntas, conversaciones y "
                                  "avisos cuando llega algo. Funciona con Gmail, Outlook, iCloud, Fastmail o cualquier "
                                  "servidor IMAP y SMTP."),
                                aos_font_body, AOS_C_DIM);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    ui_btn(col, AOS_SYM_PLUS, _("Agregar una cuenta"), AOS_C_ACCENT, add_acct_cb, NULL);
}

static void open_folder_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    cr_list_open((int)(v >> 8) - 1, (int)(v & 0xFF));
}

static void settings_cb(lv_event_t *e)
{
    (void)e;
    ui_go(SCR_SETTINGS);
}

static void compose_cb(lv_event_t *e)
{
    (void)e;
    cr_compose_new(-1, NULL);
}

static void state_cb(lv_event_t *e)
{
    int ai = (int)(intptr_t)lv_event_get_user_data(e);
    cr_lock();
    char t[400];
    cr_acct_t *a = &CR.a[ai];
    snprintf(t, sizeof t, "%s\n\n%s", ui_state_text(a), a->err);
    int st = a->state;
    cr_unlock();
    if (st == CR_ST_NO_PASS || (st == CR_ST_ERROR && CR.a[ai].err_kind == CR_E_AUTH)) {
        cr_acct_edit(ai);
        return;
    }
    if (st == CR_ST_ERROR) {
        cr_job_t *j = cr_job_new(CR_J_RECONNECT);
        if (j) {
            j->acct = ai;
            j->token = ui_token(OWN_NONE);
            cr_job_put(j);
        }
        aos_ui_toast(_("Probando de nuevo..."), 1500);
    }
}

static lv_obj_t *folder_row(lv_obj_t *col, const char *glyph, const char *name, uint32_t unseen, int depth,
                            intptr_t ud)
{
    int32_t w = ui_page_w() - 2 * AOS_UI_PAD;
    lv_obj_t *row = ui_box(col, lv_pct(100), 84);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, open_folder_cb, LV_EVENT_CLICKED, (void *)ud);
    int32_t x = 20 + depth * 28;
    lv_obj_t *g = ui_label(row, glyph, &aos_sym_28, AOS_C_ACCENT);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, x, 0);
    lv_obj_t *l = ui_label_dots(row, name, aos_font_body, AOS_C_TEXT, w - x - 170);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, x + 50, 0);
    if (unseen) {
        char b[16];
        snprintf(b, sizeof b, "%lu", (unsigned long)unseen);
        lv_obj_t *c = ui_label(row, b, aos_font_body, AOS_C_DIM);
        lv_obj_align(c, LV_ALIGN_RIGHT_MID, -50, 0);
    }
    lv_obj_t *ch = ui_label(row, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
    lv_obj_align(ch, LV_ALIGN_RIGHT_MID, -14, 0);
    return row;
}

static int sp_order(int sp)
{
    static const int O[] = {[CR_SP_INBOX] = 0, [CR_SP_DRAFTS] = 1, [CR_SP_SENT] = 2, [CR_SP_ARCHIVE] = 3,
                            [CR_SP_FLAGGED] = 4, [CR_SP_ALL] = 5, [CR_SP_JUNK] = 6, [CR_SP_TRASH] = 7,
                            [CR_SP_NONE] = 8};
    return O[sp];
}

static cr_acct_t *s_sort_acct;

static int cmp_folder(const void *x, const void *y)
{
    const cr_folder_t *a = &s_sort_acct->f[*(const int *)x], *b = &s_sort_acct->f[*(const int *)y];
    int oa = sp_order(a->special), ob = sp_order(b->special);
    if (oa != ob) return oa - ob;
    return strcmp(a->raw, b->raw);
}

void cr_home_build(lv_obj_t *page)
{
    lv_obj_t *bar = ui_bar(page, NULL, NULL, NULL);
    lv_obj_t *t = ui_label(bar, _("Correo"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 12, 0);
    lv_obj_t *r = ui_bar_right(bar);
    ui_icon_btn(r, AOS_SYM_COG, settings_cb, NULL);
    ui_icon_btn(r, AOS_SYM_PENCIL, compose_cb, NULL);
    lv_obj_t *col = ui_col(page, ui_page_w(), U.H - BAR_H);
    lv_obj_set_y(col, BAR_H);
    int32_t w = ui_page_w() - 2 * AOS_UI_PAD;

    cr_lock();
    uint32_t total = 0;
    for (int i = 0; i < CR.na; i++) total += CR.a[i].unseen;
    if (CR.na > 1) folder_row(col, AOS_SYM_DOWNLOAD, _("Todas las entradas"), total, 0, 0);
    for (int ai = 0; ai < CR.na; ai++) {
        cr_acct_t *a = &CR.a[ai];
        /* the account: its name and how it is doing */
        lv_obj_t *head = ui_box(col, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_top(head, 18, 0);
        lv_obj_add_flag(head, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(head, state_cb, LV_EVENT_CLICKED, (void *)(intptr_t)ai);
        lv_obj_t *dot = ui_box(head, 18, 18);
        lv_obj_set_style_radius(dot, 9, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(a->cfg.color), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 4, 30);
        lv_obj_t *n = ui_label_dots(head, ui_acct_name(ai), aos_font_body, AOS_C_TEXT, w - 40);
        lv_obj_set_pos(n, 34, 18);
        lv_color_t sc = a->state == CR_ST_ERROR || a->state == CR_ST_NO_PASS ? AOS_C_ORANGE : AOS_C_DIM;
        lv_obj_t *s = ui_label_wrap(head, ui_state_text(a), aos_font_caption, sc);
        lv_obj_set_width(s, w - 40);
        lv_obj_set_pos(s, 34, 58);
        int idx[CR_FOLDER_MAX], k = 0;
        for (int i = 0; i < a->nf; i++) {
            if (!a->f[i].gone) idx[k++] = i;
        }
        s_sort_acct = a;
        qsort(idx, (size_t)k, sizeof idx[0], cmp_folder);
        if (!k) ui_label(col, _("Todavía sin carpetas: conectando..."), aos_font_small, AOS_C_DIM);
        for (int q = 0; q < k; q++) {
            cr_folder_t *f = &a->f[idx[q]];
            if (f->noselect) {
                lv_obj_t *l = ui_label(col, f->name, aos_font_small, AOS_C_DIM);
                lv_obj_set_style_pad_left(l, 20 + f->depth * 28, 0);
                continue;
            }
            uint32_t un = f->special == CR_SP_SENT || f->special == CR_SP_TRASH ? 0 :
                          f->special == CR_SP_DRAFTS ? f->exists : f->unseen;
            folder_row(col, cr_folder_glyph(f), cr_folder_label(f), un, f->special ? 0 : f->depth,
                       ((intptr_t)(ai + 1) << 8) | idx[q]);
        }
    }
    cr_unlock();
}

/* ---- the list ---------------------------------------------------------------------------------- */

static uint32_t row_key(const cr_hdr_t *h)
{
    if (!CR.threads) return h->uid ^ ((uint32_t)h->acct << 24) ^ 0x9E3779B9u;
    if (h->root_h && h->root_h != h->mid_h) return h->root_h ^ h->acct;
    if ((h->flags & CR_F_RE) && h->subj_h) return h->subj_h ^ 0x5BD1E995u ^ h->acct;
    return (h->mid_h ? h->mid_h : h->uid) ^ h->acct;
}

static bool matches(const cr_hdr_t *h, const char *q)
{
    if (!q[0]) return true;
    return cr_istr(h->subject, q) || cr_istr(h->from, q) || cr_istr(h->from_addr, q) || cr_istr(h->to, q);
}

typedef struct {
    const cr_hdr_t *h;
} ref_t;

static int cmp_date(const void *x, const void *y)
{
    const cr_hdr_t *a = ((const ref_t *)x)->h, *b = ((const ref_t *)y)->h;
    if (a->date != b->date) return a->date < b->date ? 1 : -1;
    return a->uid < b->uid ? 1 : a->uid > b->uid ? -1 : 0;
}

static void row_add(const cr_hdr_t *h, bool sent_like)
{
    if (U.nrows == U.caprows) {
        int c = U.caprows ? U.caprows * 2 : 256;
        cr_row_t *nr = cr_realloc(U.rows, sizeof(cr_row_t) * (size_t)c);
        if (!nr) return;
        U.rows = nr;
        U.caprows = c;
    }
    cr_row_t *r = &U.rows[U.nrows++];
    memset(r, 0, sizeof *r);
    r->uid = h->uid;
    r->date = h->date;
    r->flags = h->flags;
    r->acct = h->acct;
    r->folder = h->folder;
    r->count = 1;
    r->unread = !(h->flags & CR_F_SEEN);
    r->attach = (h->flags & CR_F_ATTACH) != 0;
    r->key = row_key(h);
    if (sent_like) snprintf(r->who, sizeof r->who, "%s %s", _("Para:"), h->to);
    else cr_copy(r->who, sizeof r->who, h->from[0] ? h->from : h->from_addr);
    cr_copy(r->subject, sizeof r->subject, h->subject[0] ? h->subject : _("(sin asunto)"));
}

/* The rows, from the model (with the lock held). */
static void rows_make(void)
{
    U.nrows = 0;
    int total = 0;
    if (U.l_server) {
        total = U.l_nres;
    } else if (U.l_acct < 0) {
        for (int ai = 0; ai < CR.na; ai++) {
            int fi = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
            if (fi >= 0) total += CR.a[ai].f[fi].n;
        }
    } else if (U.l_acct < CR.na && U.l_folder < CR.a[U.l_acct].nf) {
        total = CR.a[U.l_acct].f[U.l_folder].n;
        if (U.l_key) {
            /* a conversation also shows the answers in Sent */
            int s = cr_folder_special(&CR.a[U.l_acct], CR_SP_SENT);
            if (s >= 0 && s != U.l_folder) total += CR.a[U.l_acct].f[s].n;
        }
    }
    ref_t *refs = total ? cr_alloc(sizeof(ref_t) * (size_t)total) : NULL;
    int n = 0;
    if (refs) {
        if (U.l_server) {
            for (int i = 0; i < U.l_nres; i++) refs[n++].h = &U.l_res[i];
        } else if (U.l_acct < 0) {
            for (int ai = 0; ai < CR.na; ai++) {
                int fi = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
                if (fi < 0) continue;
                cr_folder_t *f = &CR.a[ai].f[fi];
                for (int i = 0; i < f->n && n < total; i++) {
                    f->h[i].acct = (uint8_t)ai;
                    f->h[i].folder = (uint8_t)fi;
                    refs[n++].h = &f->h[i];
                }
            }
        } else if (U.l_acct < CR.na) {
            cr_acct_t *a = &CR.a[U.l_acct];
            cr_folder_t *f = &a->f[U.l_folder];
            for (int i = 0; i < f->n && n < total; i++) {
                f->h[i].acct = (uint8_t)U.l_acct;
                f->h[i].folder = (uint8_t)U.l_folder;
                refs[n++].h = &f->h[i];
            }
            int s = U.l_key ? cr_folder_special(a, CR_SP_SENT) : -1;
            if (s >= 0 && s != U.l_folder) {
                for (int i = 0; i < a->f[s].n && n < total; i++) {
                    a->f[s].h[i].acct = (uint8_t)U.l_acct;
                    a->f[s].h[i].folder = (uint8_t)s;
                    refs[n++].h = &a->f[s].h[i];
                }
            }
        }
        qsort(refs, (size_t)n, sizeof(ref_t), cmp_date);
    }
    bool sent_like = false;
    if (U.l_acct >= 0 && U.l_acct < CR.na && U.l_folder < CR.a[U.l_acct].nf) {
        int sp = CR.a[U.l_acct].f[U.l_folder].special;
        sent_like = sp == CR_SP_SENT || sp == CR_SP_DRAFTS;
    }
    /* grouped: the newest of each conversation stands for it */
    int hcap = 1;
    while (hcap < n * 2 + 2) hcap <<= 1;
    int32_t *hmap = (CR.threads && !U.l_key && !U.l_server) ? cr_alloc(sizeof(int32_t) * (size_t)hcap) : NULL;
    if (hmap) memset(hmap, 0xFF, sizeof(int32_t) * (size_t)hcap);
    for (int i = 0; i < n; i++) {
        const cr_hdr_t *h = refs[i].h;
        if (h->flags & CR_F_DELETED) continue;
        if (U.l_key && row_key(h) != U.l_key) continue;
        if (!U.l_server && !matches(h, U.l_query)) continue;
        bool sl = sent_like || (U.l_key && h->folder != U.l_folder);
        if (hmap) {
            uint32_t k = row_key(h);
            uint32_t s = (k * 2654435761u) & (uint32_t)(hcap - 1);
            while (hmap[s] >= 0 && U.rows[hmap[s]].key != k) s = (s + 1) & (uint32_t)(hcap - 1);
            if (hmap[s] >= 0) {
                cr_row_t *r = &U.rows[hmap[s]];
                r->count++;
                if (!(h->flags & CR_F_SEEN)) r->unread++;
                if (h->flags & CR_F_ATTACH) r->attach = true;
                continue;
            }
            hmap[s] = U.nrows;
        }
        row_add(h, sl);
    }
    cr_free(hmap);
    cr_free(refs);
}

static void list_back_cb(lv_event_t *e)
{
    (void)e;
    if (U.l_key) {
        U.l_key = 0;
        U.l_scroll = 0;
        ui_rebuild();
        return;
    }
    ui_pop();
}

static void row_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= U.nrows) return;
    cr_row_t *r = &U.rows[i];
    if (U.l_list) U.l_scroll = lv_obj_get_scroll_y(U.l_list);
    if (r->count > 1 && !U.l_key) {
        U.l_key = r->key;
        if (U.l_acct < 0) {
            /* a conversation of the unified inbox lives in its own account */
            U.l_acct = r->acct;
            U.l_folder = r->folder;
            U.l_key = r->key;
            U.l_scroll = 0;
            /* remember to come back to the unified one */
            U.l_acct = r->acct;
        }
        U.l_scroll = 0;
        ui_rebuild();
        return;
    }
    cr_lock();
    bool draft = r->acct < CR.na && r->folder < CR.a[r->acct].nf && CR.a[r->acct].f[r->folder].special == CR_SP_DRAFTS;
    cr_unlock();
    (void)draft;
    cr_msg_open(r->acct, r->folder, r->uid);
}

static int s_sheet_row;

static void row_menu_pick(int k, void *ud)
{
    (void)ud;
    int i = s_sheet_row;
    if (i < 0 || i >= U.nrows) return;
    cr_row_t r = U.rows[i];
    switch (k) {
    case 0: cr_act_flags(r.acct, r.folder, r.uid, r.flags & CR_F_SEEN ? 0 : CR_F_SEEN, r.flags & CR_F_SEEN ? CR_F_SEEN : 0); break;
    case 1: cr_act_flags(r.acct, r.folder, r.uid, r.flags & CR_F_FLAGGED ? 0 : CR_F_FLAGGED, r.flags & CR_F_FLAGGED ? CR_F_FLAGGED : 0); break;
    case 2: cr_act_archive(r.acct, r.folder, r.uid); break;
    case 3: cr_act_move_pick(r.acct, r.folder, r.uid); break;
    case 4: cr_act_delete(r.acct, r.folder, r.uid); break;
    }
}

static void row_long_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= U.nrows) return;
    s_sheet_row = i;
    cr_row_t *r = &U.rows[i];
    static const char *items[5];
    items[0] = r->flags & CR_F_SEEN ? _("Marcar como no leído") : _("Marcar como leído");
    items[1] = r->flags & CR_F_FLAGGED ? _("Quitar la estrella") : _("Destacar");
    items[2] = _("Archivar");
    items[3] = _("Mover a...");
    static char del[64];
    snprintf(del, sizeof del, "!%s", _("Borrar"));
    items[4] = del;
    ui_sheet(r->subject, items, 5, row_menu_pick, NULL);
    lv_indev_wait_release(lv_indev_active());
}

static void row_build(int i)
{
    cr_row_t *r = &U.rows[i];
    int32_t w = lv_obj_get_style_width(U.l_list, 0) - 2 * (U.left ? 12 : AOS_UI_PAD);
    lv_obj_t *row = ui_box(U.l_list, lv_pct(100), 112);
    lv_obj_set_style_radius(row, 18, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    bool sel = ui_top() == SCR_MSG && U.left && r->uid == U.m_uid && r->acct == U.m_acct;
    if (sel) {
        lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    }
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    lv_obj_add_event_cb(row, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);
    if (r->unread) {
        lv_obj_t *dot = ui_box(row, 16, 16);
        lv_obj_set_style_radius(dot, 8, 0);
        lv_obj_set_style_bg_color(dot, AOS_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, 4, 24);
    }
    char d[32];
    cr_date_short(d, sizeof d, r->date);
    lv_obj_t *dl = ui_label(row, d, aos_font_caption, AOS_C_DIM);
    lv_obj_align(dl, LV_ALIGN_TOP_RIGHT, -6, 14);
    int32_t right = 120;
    lv_obj_t *who = ui_label_dots(row, r->who, aos_font_body, r->unread ? AOS_C_TEXT : lv_color_hex(0xD1D1D6),
                                  w - 30 - right);
    lv_obj_set_pos(who, 28, 8);
    if (CR.na > 1 && U.l_acct < 0) {
        lv_obj_t *ad = ui_box(row, 8, 34);
        lv_obj_set_style_radius(ad, 4, 0);
        lv_obj_set_style_bg_color(ad, lv_color_hex(CR.a[r->acct].cfg.color), 0);
        lv_obj_set_style_bg_opa(ad, LV_OPA_COVER, 0);
        lv_obj_set_pos(ad, 6, 62);
    }
    char extra[64] = "";
    int32_t ew = 0;
    if (r->count > 1) {
        snprintf(extra, sizeof extra, "%u", r->count);
        lv_obj_t *cnt = ui_box(row, LV_SIZE_CONTENT, 38);
        lv_obj_set_style_radius(cnt, 19, 0);
        lv_obj_set_style_pad_hor(cnt, 14, 0);
        lv_obj_set_style_bg_color(cnt, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(cnt, LV_OPA_COVER, 0);
        lv_obj_t *cl = ui_label(cnt, extra, aos_font_caption, AOS_C_DIM);
        lv_obj_center(cl);
        lv_obj_align(cnt, LV_ALIGN_BOTTOM_RIGHT, -6, -14);
        ew = 70;
    }
    if (r->flags & CR_F_FLAGGED) {
        lv_obj_t *st = ui_label(row, AOS_SYM_STAR, &aos_sym_28, AOS_C_ORANGE);
        lv_obj_align(st, LV_ALIGN_BOTTOM_RIGHT, -6 - ew, -16);
        ew += 40;
    }
    if (r->attach) {
        lv_obj_t *at = ui_label(row, AOS_SYM_FILE_OUTLINE, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(at, LV_ALIGN_BOTTOM_RIGHT, -6 - ew, -16);
        ew += 40;
    }
    if (r->flags & CR_F_ANSWERED) {
        lv_obj_t *an = ui_label(row, AOS_SYM_ARROW_LEFT_BOLD, &aos_sym_28, AOS_C_DIM);
        lv_obj_align(an, LV_ALIGN_BOTTOM_RIGHT, -6 - ew, -16);
        ew += 40;
    }
    lv_obj_t *s = ui_label_dots(row, r->subject, aos_font_small, AOS_C_DIM, w - 30 - ew - 10);
    lv_obj_set_pos(s, 28, 62);
}

static void more_rows(void)
{
    if (!U.l_list) return;
    if (U.l_more) {
        lv_obj_delete(U.l_more);
        U.l_more = NULL;
    }
    int end = U.l_shown + PAGE_ROWS;
    if (end > U.nrows) end = U.nrows;
    for (int i = U.l_shown; i < end; i++) row_build(i);
    U.l_shown = end;
}

static void more_server_cb(lv_event_t *e)
{
    (void)e;
    if (U.l_acct < 0 || U.l_loading) return;
    cr_job_t *j = cr_job_new(CR_J_MORE);
    if (!j) return;
    j->acct = U.l_acct;
    j->folder = U.l_folder;
    U.l_token = j->token = ui_token(OWN_LIST);
    U.l_loading = true;
    cr_job_put(j);
    if (U.l_more) {
        lv_obj_t *l = lv_obj_get_child(U.l_more, 0);
        if (l) lv_label_set_text(l, _("Trayendo..."));
    }
}

static void list_tail(void)
{
    if (!U.l_list || U.l_shown < U.nrows) return;
    /* the end of what the board has: more from the server? */
    bool all = true;
    cr_lock();
    if (U.l_acct >= 0 && U.l_acct < CR.na && U.l_folder < CR.a[U.l_acct].nf) all = CR.a[U.l_acct].f[U.l_folder].all;
    cr_unlock();
    if (U.l_key || U.l_server || U.l_query[0]) all = true;
    if (!U.nrows) {
        lv_obj_t *l = ui_label(U.l_list, U.l_query[0] ? _("Nada que coincida") : _("No hay mensajes"), aos_font_body,
                               AOS_C_DIM);
        lv_obj_set_style_pad_top(l, 40, 0);
    }
    if (all) return;
    U.l_more = ui_btn(U.l_list, NULL, U.l_loading ? _("Trayendo...") : _("Traer más viejos"), AOS_C_CARD2,
                      more_server_cb, NULL);
}

static void scroll_cb(lv_event_t *e)
{
    (void)e;
    if (!U.l_list) return;
    if (lv_obj_get_scroll_bottom(U.l_list) < 400 && U.l_shown < U.nrows) {
        more_rows();
        list_tail();
    }
}

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    if (U.l_acct < 0) {
        for (int ai = 0; ai < CR.na; ai++) {
            int fi = cr_folder_special(&CR.a[ai], CR_SP_INBOX);
            if (fi < 0) continue;
            cr_job_t *j = cr_job_new(CR_J_SYNC);
            if (!j) continue;
            j->acct = ai;
            j->folder = fi;
            j->arg = 1;
            j->token = ui_token(OWN_NONE);
            cr_job_put(j);
        }
    } else {
        cr_job_t *j = cr_job_new(CR_J_SYNC);
        if (j) {
            j->acct = U.l_acct;
            j->folder = U.l_folder;
            j->arg = 1;
            j->token = ui_token(OWN_NONE);
            cr_job_put(j);
        }
    }
    aos_ui_toast(_("Actualizando..."), 1000);
}

static void search_changed_cb(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    cr_copy(U.l_query, sizeof U.l_query, lv_textarea_get_text(ta));
    if (U.l_server) {
        U.l_server = false;
        cr_free(U.l_res);
        U.l_res = NULL;
        U.l_nres = 0;
    }
    cr_list_refresh();
}

static void server_search_cb(lv_event_t *e)
{
    (void)e;
    if (!U.l_query[0] || U.l_acct < 0) {
        if (U.l_acct < 0) aos_ui_toast(_("Para buscar en el servidor, abrí una carpeta de una cuenta"), 2500);
        return;
    }
    cr_job_t *j = cr_job_new(CR_J_SEARCH);
    if (!j) return;
    j->acct = U.l_acct;
    j->folder = U.l_folder;
    cr_copy(j->text, sizeof j->text, U.l_query);
    U.l_token = j->token = ui_token(OWN_LIST);
    U.l_searching = true;
    cr_job_put(j);
    ui_kb_hide();
    aos_ui_toast(_("Buscando en el servidor..."), 1500);
}

static void list_compose_cb(lv_event_t *e)
{
    (void)e;
    cr_compose_new(U.l_acct, NULL);
}

static const char *list_title(void)
{
    static char t[120];
    if (U.l_key && U.nrows) {
        cr_copy(t, sizeof t, U.rows[U.nrows - 1].subject);
        return t;
    }
    if (U.l_acct < 0) return _("Todas las entradas");
    if (U.l_acct >= CR.na || U.l_folder >= CR.a[U.l_acct].nf) return "";
    const char *f = cr_folder_label(&CR.a[U.l_acct].f[U.l_folder]);
    if (CR.na > 1) snprintf(t, sizeof t, "%s - %s", f, ui_acct_name(U.l_acct));
    else cr_copy(t, sizeof t, f);
    return t;
}

void cr_list_build(lv_obj_t *page)
{
    int32_t pw = lv_obj_get_style_width(page, 0);
    cr_lock();
    rows_make();
    const char *title = list_title();
    char tcopy[120];
    cr_copy(tcopy, sizeof tcopy, title);
    cr_unlock();
    lv_obj_t *bar = ui_bar(page, U.l_key ? _("Lista") : _("Buzones"), U.left ? NULL : tcopy, list_back_cb);
    lv_obj_t *r = ui_bar_right(bar);
    if (!U.l_key) ui_icon_btn(r, AOS_SYM_RESTART, refresh_cb, NULL);
    ui_icon_btn(r, AOS_SYM_PENCIL, list_compose_cb, NULL);
    int32_t y = BAR_H;
    if (U.left) {
        lv_obj_t *t = ui_label_dots(page, tcopy, aos_font_body, AOS_C_TEXT, pw - 40);
        lv_obj_set_pos(t, 20, y);
        y += 44;
    }
    if (!U.l_key) {
        /* the search field and its button */
        lv_obj_t *sr = ui_box(page, pw, 84);
        lv_obj_set_y(sr, y);
        lv_obj_set_style_pad_hor(sr, U.left ? 12 : AOS_UI_PAD, 0);
        lv_obj_t *ta = lv_textarea_create(sr);
        lv_obj_set_size(ta, pw - 2 * (U.left ? 12 : AOS_UI_PAD) - (U.l_acct >= 0 ? 90 : 0), 72);
        lv_obj_set_style_text_font(ta, aos_font_small, 0);
        lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
        lv_obj_set_style_bg_color(ta, AOS_C_CARD, 0);
        lv_obj_set_style_border_width(ta, 0, 0);
        lv_obj_set_style_radius(ta, 36, 0);
        lv_obj_set_style_pad_hor(ta, 24, 0);
        lv_obj_set_style_pad_ver(ta, 18, 0);
        lv_textarea_set_one_line(ta, true);
        lv_textarea_set_placeholder_text(ta, _("Buscar"));
        lv_textarea_set_text(ta, U.l_query);
        lv_obj_align(ta, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_add_event_cb(ta, search_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
        lv_obj_add_event_cb(ta, server_search_cb, LV_EVENT_READY, NULL);
        ui_kb_watch(ta);
        U.l_search = ta;
        if (U.l_acct >= 0) {
            lv_obj_t *sb = ui_icon_btn(sr, AOS_SYM_MAGNIFY, server_search_cb, NULL);
            lv_obj_align(sb, LV_ALIGN_RIGHT_MID, 0, 0);
        }
        y += 88;
        if (U.l_server) {
            char t[120];
            snprintf(t, sizeof t, _("%d en el servidor"), U.l_nres);
            lv_obj_t *l = ui_label(page, t, aos_font_caption, AOS_C_ACCENT);
            lv_obj_set_pos(l, AOS_UI_PAD + 8, y);
            y += 34;
        }
    }
    U.l_list = ui_col(page, pw, U.H - y);
    lv_obj_set_y(U.l_list, y);
    lv_obj_set_style_pad_row(U.l_list, 2, 0);
    if (U.left) lv_obj_set_style_pad_hor(U.l_list, 12, 0);
    lv_obj_add_event_cb(U.l_list, scroll_cb, LV_EVENT_SCROLL, NULL);
    U.l_shown = 0;
    U.l_more = NULL;
    /* as many rows as the place it was left needs */
    while (U.l_shown < U.nrows && (U.l_shown < PAGE_ROWS || U.l_shown * 114 < U.l_scroll + U.H)) more_rows();
    list_tail();
    if (U.l_scroll) {
        lv_obj_update_layout(U.l_list);
        lv_obj_scroll_to_y(U.l_list, U.l_scroll, LV_ANIM_OFF);
    }
    if (!U.left) ui_kb_attach(page, U.l_list);
}

void cr_list_refresh(void)
{
    if (!U.l_list) return;
    int32_t sy = lv_obj_get_scroll_y(U.l_list);
    cr_lock();
    rows_make();
    cr_unlock();
    lv_obj_clean(U.l_list);
    U.l_more = NULL;
    U.l_shown = 0;
    while (U.l_shown < U.nrows && (U.l_shown < PAGE_ROWS || U.l_shown * 114 < sy + U.H)) more_rows();
    list_tail();
    lv_obj_update_layout(U.l_list);
    lv_obj_scroll_to_y(U.l_list, sy, LV_ANIM_OFF);
}

void cr_list_open(int acct, int folder)
{
    U.l_acct = acct;
    U.l_folder = folder;
    U.l_key = 0;
    U.l_query[0] = '\0';
    U.l_server = false;
    U.l_scroll = 0;
    U.l_loading = false;
    cr_free(U.l_res);
    U.l_res = NULL;
    U.l_nres = 0;
    /* the folder up to date with the server, if it is not */
    if (acct >= 0) {
        cr_lock();
        bool synced = acct < CR.na && folder < CR.a[acct].nf && CR.a[acct].f[folder].synced;
        bool loaded = acct < CR.na && folder < CR.a[acct].nf && CR.a[acct].f[folder].loaded;
        cr_unlock();
        if (!loaded) {
            /* the card's copy first, so the list shows before the network */
            cr_lock();
            cr_acct_t ac = CR.a[acct];
            cr_folder_t tmp = {0};
            cr_copy(tmp.raw, sizeof tmp.raw, CR.a[acct].f[folder].raw);
            cr_unlock();
            cr_idx_load(&ac, &tmp);
            cr_lock();
            cr_folder_t *f = &CR.a[acct].f[folder];
            if (!f->loaded) {
                cr_free(f->h);
                f->h = tmp.h;
                f->n = tmp.n;
                f->cap = tmp.cap;
                f->uidvalidity = tmp.uidvalidity;
                f->uidnext = tmp.uidnext;
                f->all = tmp.all;
                cr_copy(f->modseq, sizeof f->modseq, tmp.modseq);
                f->loaded = true;
            } else {
                cr_free(tmp.h);
            }
            cr_unlock();
        }
        if (!synced) {
            cr_job_t *j = cr_job_new(CR_J_SYNC);
            if (j) {
                j->acct = acct;
                j->folder = folder;
                j->token = ui_token(OWN_NONE);
                cr_job_put(j);
            }
        }
    }
    ui_go(SCR_LIST);
}

void cr_list_free(void)
{
    cr_free(U.l_res);
    U.l_res = NULL;
    U.l_nres = 0;
    U.l_server = false;
    U.l_key = 0;
    U.l_list = NULL;
}

void cr_list_job(cr_job_t *j)
{
    if (j->kind == CR_J_SEARCH && j->token == U.l_token) {
        U.l_searching = false;
        if (!j->ok) {
            char t[260];
            snprintf(t, sizeof t, "%s: %s", _("No se pudo buscar"), j->why);
            aos_ui_toast(t, 3000);
        } else {
            cr_free(U.l_res);
            U.l_res = j->out;
            U.l_nres = j->n;
            j->out = NULL;
            for (int i = 0; i < U.l_nres; i++) {
                U.l_res[i].acct = (uint8_t)j->acct;
                U.l_res[i].folder = (uint8_t)j->folder;
            }
            U.l_server = true;
            if (ui_top() == SCR_LIST || U.left) ui_rebuild();
        }
    } else if (j->kind == CR_J_MORE && j->token == U.l_token) {
        U.l_loading = false;
        if (!j->ok) aos_ui_toast(j->why, 2500);
        cr_list_refresh();
    } else if ((j->kind == CR_J_MOVE || j->kind == CR_J_DELETE) && !j->ok) {
        char t[260];
        snprintf(t, sizeof t, "%s: %s", _("No se pudo"), j->why);
        aos_ui_toast(t, 3000);
        /* what was taken off the list comes back with the next sync */
        cr_job_t *k = cr_job_new(CR_J_SYNC);
        if (k) {
            k->acct = j->acct;
            k->folder = j->folder;
            k->token = ui_token(OWN_NONE);
            cr_job_put(k);
        }
    }
}

/* ---- the app's frame --------------------------------------------------------------------------- */

void cr_ui_create(void *root)
{
    memset(&U, 0, sizeof U);
    U.root = root;
    U.depth = 1;
    U.stack[0] = CR.na ? SCR_HOME : SCR_WELCOME;
    U.l_acct = -1;
    lv_obj_set_style_bg_color(root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    /* straight to the unified inbox, as a phone does */
    if (CR.na) {
        U.stack[0] = SCR_HOME;
        U.stack[1] = SCR_LIST;
        U.depth = 2;
        if (CR.na == 1) {
            cr_lock();
            int fi = cr_folder_special(&CR.a[0], CR_SP_INBOX);
            cr_unlock();
            U.l_acct = fi >= 0 ? 0 : -1;
            U.l_folder = fi >= 0 ? fi : 0;
        }
    }
    ui_rebuild();
}

void cr_ui_destroy(void)
{
    cr_msg_close();
    cr_compose_free();
    cr_list_free();
    sheet_forget();
    kb_forget();
    cr_free(U.rows);
    U.rows = NULL;
    U.root = NULL;
}

bool cr_ui_back(void)
{
    if (ui_sheet_open()) {
        ui_sheet_close();
        return true;
    }
    if (ui_kb_visible()) {
        ui_kb_hide();
        return true;
    }
    int top = ui_top();
    if (top == SCR_COMPOSE) return cr_compose_back();
    if (top == SCR_ACCT && cr_acct_back()) return true;
    if (top == SCR_PICK && cr_pick_back()) return true;
    if (top == SCR_LIST && U.l_key) {
        U.l_key = 0;
        ui_rebuild();
        return true;
    }
    if (top == SCR_LIST && (U.l_query[0] || U.l_server)) {
        U.l_query[0] = '\0';
        U.l_server = false;
        ui_rebuild();
        return true;
    }
    if (U.depth <= 1) return false;
    ui_pop();
    return true;
}

void cr_ui_resize(void *root)
{
    U.root = root;
    if (U.l_list) U.l_scroll = lv_obj_get_scroll_y(U.l_list);
    if (ui_top() == SCR_COMPOSE) {
        /* the compose screen keeps what was typed: cr_compose_build reads it back */
    }
    ui_rebuild();
}

void cr_ui_shown(bool shown)
{
    U.hidden = !shown;
    if (shown) {
        U.badge = 0;
        if (U.root) ui_rebuild();
    }
}

static int total_unseen(void)
{
    int t = 0;
    cr_lock();
    for (int i = 0; i < CR.na; i++) t += (int)CR.a[i].unseen;
    cr_unlock();
    return t;
}

void cr_ui_tick(void)
{
    cr_job_t *j;
    while ((j = cr_job_done())) {
        if (cr_portal_owns(j)) {
            cr_portal_job(j);
            continue;
        }
        switch (TOKEN_OWN(j->token)) {
        case OWN_LIST: cr_list_job(j); break;
        case OWN_MSG: cr_msg_job(j); break;
        case OWN_COMPOSE: cr_compose_job(j); break;
        case OWN_ACCT: cr_acct_job(j); break;
        case OWN_IMG: cr_img_job(j); break;
        case OWN_PART: cr_part_job(j); break;
        default:
            if (!j->ok && j->kind == CR_J_FLAGS) aos_hal_log(CR_TAG, "flags: %s", j->why);
            break;
        }
        cr_job_free(j);
    }
    cr_event_t ev;
    while (cr_event_take(&ev)) {
        if (ev.kind == CR_EV_NEW_MAIL && ev.n > 0 && CR.notify) {
            char t[240];
            if (ev.n == 1) {
                /* "who\nsubject" */
                char *nl = strchr(ev.text, '\n');
                if (nl) *nl = '\0';
                snprintf(t, sizeof t, "%s: %s", ev.text, nl ? nl + 1 : "");
            } else {
                snprintf(t, sizeof t, _("%d correos nuevos"), ev.n);
            }
            bool looking = !U.hidden && ui_top() == SCR_LIST && (U.l_acct < 0 || U.l_acct == ev.acct) && !U.l_key;
            if (!looking) {
                aos_ui_toast(t, 4000);
                aos_ui_sound(AOS_SND_NOTIF);
            }
            aos_hal_log(CR_TAG, "new mail: %d (%s)", ev.n, ui_acct_name(ev.acct));
        }
    }
    /* the icon's number: unread in the inboxes */
    int un = total_unseen();
    if (un != U.badge) {
        U.badge = un;
        aos_ui_set_badge(CR_APP_ID, un);
    }
    if (U.hidden || !U.root) return;
    if (CR.gen != U.gen_seen) {
        U.gen_seen = CR.gen;
        ui_rebuild();
        return;
    }
    if (CR.ver != U.ver_seen && !ui_sheet_open()) {
        U.ver_seen = CR.ver;
        int top = ui_top();
        if (top == SCR_HOME) ui_rebuild();
        else if (top == SCR_LIST && !ui_kb_visible()) cr_list_refresh();
        else if (top == SCR_MSG && U.left) cr_list_refresh();
    }
}

void cr_ui_open_arg(const char *arg)
{
    if (!arg || !arg[0]) return;
    if (cr_istarts(arg, "mailto:")) cr_compose_new(-1, arg + 7);
}
