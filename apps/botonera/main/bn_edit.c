/*
 * Botonera - the sheets over the grid: a button's editor, the list of the
 * folder's sounds, the recorder and the keyboard for a name.
 *
 * Two levels at most: the editor (u->sheet) and, over it, the list, the
 * recorder or the keyboard (u->sheet2). A page's name opens the keyboard on
 * the first level. What the editor changes goes to the button at once and
 * is written to the grid's file when the editor closes.
 *
 * The recorder writes a WAV straight into the sounds folder with the
 * firmware's recorder (aos_hal_rec_*, the same as the Recorder app's). While
 * it records the mixer is stopped and the speaker closed: what the speaker
 * plays would end up in the recording.
 */
#include "bn_ui.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_text_safe.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

static const char *const MODE_NAME[BN_MODE_COUNT] = { N_("Cortar"), N_("Superponer"), N_("Mantener") };
static const char *const MODE_HELP[BN_MODE_COUNT] = {
    N_("Un toque lo hace sonar y otro lo corta."),
    N_("Cada toque lo hace sonar de nuevo, encima de lo que ya suena."),
    N_("Suena mientras el dedo está apoyado."),
};

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = box(parent);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_style_pad_column(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return c;
}

static lv_obj_t *section(lv_obj_t *parent, const char *title)
{
    lv_obj_t *l = aos_label(parent, title, aos_font_small, AOS_C_DIM);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_pad_left(l, 12, 0);
    lv_obj_set_style_pad_top(l, 8, 0);
    return card(parent);
}

static void sheet_close_cb(lv_event_t *e);

lv_obj_t *bn_sheet_new(bn_ui_t *u, const char *title, bool top)
{
    lv_obj_t *s = lv_obj_create(u->root);
    lv_obj_remove_style_all(s);
    lv_obj_set_size(s, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);      /* nothing goes through to the grid */
    lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (top) u->sheet2 = s;
    else u->sheet = s;

    lv_obj_t *hd = box(s);
    lv_obj_set_size(hd, lv_pct(100), 104);
    lv_obj_set_style_pad_hor(hd, 20, 0);
    lv_obj_set_flex_flow(hd, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hd, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hd, 16, 0);
    lv_obj_t *x = bn_btn(hd, top ? AOS_SYM_CHEVRON_LEFT : AOS_SYM_CHECK, top ? NULL : _("Listo"),
                         top ? AOS_C_CARD2 : AOS_C_ACCENT, sheet_close_cb, u);
    lv_obj_set_user_data(x, (void *)(intptr_t)(top ? 2 : 1));
    lv_obj_t *t = aos_label(hd, title, aos_font_title, AOS_C_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(t, lv_font_get_line_height(aos_font_title));
    lv_obj_set_flex_grow(t, 1);

    lv_obj_t *body = lv_obj_create(s);
    lv_obj_remove_style_all(body);
    lv_obj_set_width(body, lv_pct(100));
    lv_obj_set_flex_grow(body, 1);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(body, LV_OBJ_FLAG_CLICKABLE);   /* or it does not scroll with the finger */
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 14, 0);
    int32_t W = lv_obj_get_width(u->root);
    int32_t cw = W > 800 ? 760 : W - 40;
    lv_obj_set_style_pad_hor(body, (W - cw) / 2, 0);
    lv_obj_set_style_pad_bottom(body, 40, 0);
    return body;
}

static void sheet2_close(bn_ui_t *u)
{
    if (u->rc_on) {
        aos_hal_rec_stop();
        u->rc_on = false;
        bn_ui_engine(u, true);
    }
    bn_ui_preview(u, NULL);
    if (u->sheet2) lv_obj_delete(u->sheet2);
    u->sheet2 = NULL;
    u->kb = u->ta = NULL;
    u->pk_list = NULL;
    u->rc_time = u->rc_meter = u->rc_btn = u->rc_btn_l = u->rc_msg = u->rc_use = u->rc_again = NULL;
    bn_files_free(&u->files);
}

void bn_edit_close(bn_ui_t *u)
{
    sheet2_close(u);
    bn_ui_preview(u, NULL);
    if (u->sheet) {
        lv_obj_delete(u->sheet);
        u->sheet = NULL;
        if (u->ed_pad >= 0) bn_ui_save(u);
    }
    u->kb = u->ta = NULL;
    u->ed_pad = -1;
    u->ed_preview = u->ed_file = u->ed_name = u->ed_loop = u->ed_vol = u->ed_vol_v = NULL;
    u->ed_icons = u->ed_colors = u->ed_clear = u->ed_try = NULL;
    for (int m = 0; m < BN_MODE_COUNT; m++) u->ed_mode[m] = NULL;
    u->t_check = 0;
    bn_ui_refresh(u);
}

void bn_sheets_close(bn_ui_t *u)
{
    if (u->sheet2) sheet2_close(u);
    else bn_edit_close(u);
}

static void sheet_close_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int which = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (which == 2) sheet2_close(u);
    else bn_edit_close(u);
}

/* ---- a name, typed ---- */

static void kb_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_READY && code != LV_EVENT_CANCEL) return;
    char text[BN_NAME_MAX];
    snprintf(text, sizeof text, "%s", u->ta ? lv_textarea_get_text(u->ta) : "");
    bn_text_cb cb = u->text_cb;
    u->text_cb = NULL;
    /* the keyboard is still using itself after the event: the sheet goes
     * later (lv_obj_delete_async), never here */
    lv_obj_t *s = u->sheet2 ? u->sheet2 : u->sheet;
    if (s) lv_obj_delete_async(s);
    if (s == u->sheet2) u->sheet2 = NULL;
    else u->sheet = NULL;
    u->kb = u->ta = NULL;
    if (code == LV_EVENT_READY && cb) cb(u, text);
    else if (cb) cb(u, NULL);
}

void bn_text_open(bn_ui_t *u, const char *title, const char *text, bn_text_cb cb)
{
    bool top = u->sheet != NULL;
    lv_obj_t *body = bn_sheet_new(u, title, top);
    lv_obj_t *sheet = top ? u->sheet2 : u->sheet;
    /* this one has no "Done" of its own: the keyboard's OK is it */
    lv_obj_t *hd = lv_obj_get_child(sheet, 0);
    lv_obj_t *x = lv_obj_get_child(hd, 0);
    lv_obj_add_flag(x, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_pad_top(body, 20, 0);
    u->text_cb = cb;
    u->ta = lv_textarea_create(body);
    lv_textarea_set_one_line(u->ta, true);
    lv_textarea_set_max_length(u->ta, 30);
    lv_textarea_set_text(u->ta, text ? text : "");
    lv_obj_set_width(u->ta, lv_pct(100));
    lv_obj_set_style_text_font(u->ta, aos_font_body, 0);
    lv_obj_set_style_bg_color(u->ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(u->ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(u->ta, 0, 0);
    lv_obj_set_style_radius(u->ta, 20, 0);
    lv_obj_set_style_pad_all(u->ta, 20, 0);
    u->kb = lv_keyboard_create(sheet);
    aos_keyboard_style(u->kb, aos_font_body);
    lv_obj_set_size(u->kb, lv_pct(100), lv_obj_get_width(u->root) > lv_obj_get_height(u->root) ? 360 : 440);
    lv_keyboard_set_textarea(u->kb, u->ta);
    lv_obj_add_event_cb(u->kb, kb_cb, LV_EVENT_ALL, u);
}

/* ---- the editor ---- */

static bn_pad_t *ed(bn_ui_t *u) { return bn_ui_pad(u, u->ed_pad); }

static void ed_sync(bn_ui_t *u)
{
    bn_pad_t *p = ed(u);
    if (!p || !u->sheet || !u->ed_preview) return;
    bn_draw_pad(u->ed_preview, lv_obj_get_child(u->ed_preview, 0), lv_obj_get_child(u->ed_preview, 1), p,
                false, true);
    lv_label_set_text(u->ed_file, p->file[0] ? p->file : _("Ninguno"));
    char safe[BN_NAME_MAX * 2];
    aos_text_safe(safe, sizeof safe, p->name);
    lv_label_set_text(u->ed_name, p->name[0] ? safe : _("Sin nombre"));
    for (uint32_t i = 0; i < lv_obj_get_child_count(u->ed_colors); i++) {
        lv_obj_t *c = lv_obj_get_child(u->ed_colors, (int32_t)i);
        bool on = bn_colors[i] == p->color;
        lv_obj_set_style_border_width(c, on ? 6 : 0, 0);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(u->ed_icons); i++) {
        lv_obj_t *c = lv_obj_get_child(u->ed_icons, (int32_t)i);
        lv_obj_set_style_bg_color(c, (int)i == p->icon ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    }
    for (int m = 0; m < BN_MODE_COUNT; m++)
        lv_obj_set_style_bg_color(u->ed_mode[m], p->mode == m ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_t *help = lv_obj_get_child(lv_obj_get_parent(u->ed_mode[0]), -1);
    lv_label_set_text(help, _(MODE_HELP[p->mode < BN_MODE_COUNT ? p->mode : 0]));
    if (p->loop) lv_obj_add_state(u->ed_loop, LV_STATE_CHECKED);
    else lv_obj_remove_state(u->ed_loop, LV_STATE_CHECKED);
    lv_slider_set_value(u->ed_vol, p->vol, LV_ANIM_OFF);
    char v[16];
    snprintf(v, sizeof v, "%d %%", p->vol);
    lv_label_set_text(u->ed_vol_v, v);
    lv_obj_t *cl = lv_obj_get_child(u->ed_clear, 1);
    lv_label_set_text(cl, u->ed_clear_armed ? _("¿Vaciar? Tocá de nuevo") : _("Vaciar"));
}

static void ed_color_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    ed(u)->color = bn_colors[i];
    ed_sync(u);
}

static void ed_icon_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    ed(u)->icon = (uint8_t)i;
    ed_sync(u);
}

static void ed_mode_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int m = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    bn_eng_stop_pad(u->ed_pad);
    ed(u)->mode = (uint8_t)m;
    ed_sync(u);
}

static void ed_loop_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    ed(u)->loop = lv_obj_has_state(u->ed_loop, LV_STATE_CHECKED);
    bn_eng_stop_pad(u->ed_pad);
}

static void ed_vol_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(u->ed_vol);
    ed(u)->vol = (uint8_t)v;
    bn_eng_gain(u->ed_pad, bn_gain(v));
    char t[16];
    snprintf(t, sizeof t, "%d %%", v);
    lv_label_set_text(u->ed_vol_v, t);
}

static void ed_try_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    bn_pad_t *p = ed(u);
    if (bn_pad_empty(p)) return;
    /* the button itself, as a touch would: with its volume and its loop */
    if (bn_eng_busy(u->ed_pad)) {
        bn_eng_stop_pad(u->ed_pad);
        return;
    }
    uint8_t mode = p->mode;
    p->mode = BN_MODE_CUT;              /* a hold button would stop at once */
    bn_ui_press(u, u->ed_pad);
    p->mode = mode;
}

static void name_done(bn_ui_t *u, const char *text)
{
    bn_pad_t *p = ed(u);
    if (p && text && text[0]) snprintf(p->name, sizeof p->name, "%s", text);
    ed_sync(u);
}

static void ed_name_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    bn_text_open(u, _("Nombre del botón"), ed(u)->name, name_done);
}

static void ed_move_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int gi = u->ed_pad;
    bn_edit_close(u);
    u->edit = true;
    u->move_from = gi;
    bn_ui_refresh(u);
}

static void ed_clear_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    if (!u->ed_clear_armed) {
        u->ed_clear_armed = true;
        ed_sync(u);
        return;
    }
    bn_eng_stop_pad(u->ed_pad);
    bn_pad_clear(ed(u));
    bn_edit_close(u);
}

static void pick_open(bn_ui_t *u);
static void rec_open(bn_ui_t *u);

static void ed_pick_cb(lv_event_t *e) { pick_open(lv_event_get_user_data(e)); }
static void ed_rec_cb(lv_event_t *e) { rec_open(lv_event_get_user_data(e)); }

static lv_obj_t *row_label(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, lv_font_get_line_height(aos_font_body));
    lv_obj_set_flex_grow(l, 1);
    return l;
}

void bn_edit_open(bn_ui_t *u, int gi)
{
    if (u->sheet || gi < 0 || gi >= BN_PADS) return;
    bn_pad_t *p = bn_ui_pad(u, gi);
    u->ed_pad = gi;
    u->ed_clear_armed = false;
    char t[64];
    snprintf(t, sizeof t, _("Botón %d"), gi % BN_PER_PAGE + 1);
    lv_obj_t *b = bn_sheet_new(u, t, false);

    /* how it looks, and a try */
    lv_obj_t *top = box(b);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(top, 30, 0);
    lv_obj_t *pv = box(top);
    lv_obj_set_size(pv, 220, 200);
    lv_obj_set_style_radius(pv, 28, 0);
    lv_obj_set_style_bg_opa(pv, LV_OPA_COVER, 0);
    lv_obj_set_style_border_opa(pv, LV_OPA_COVER, 0);
    lv_obj_t *ic = lv_label_create(pv);
    lv_obj_set_style_text_font(ic, &aos_sym_72, 0);
    lv_obj_align(ic, LV_ALIGN_CENTER, 0, -26);
    lv_obj_t *nm = lv_label_create(pv);
    lv_obj_set_style_text_font(nm, aos_font_small, 0);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(nm, 196, lv_font_get_line_height(aos_font_small));
    lv_obj_set_style_text_align(nm, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(nm, LV_ALIGN_BOTTOM_MID, 0, -20);
    u->ed_preview = pv;
    u->ed_try = bn_btn(top, AOS_SYM_PLAY, _("Probar"), AOS_C_GREEN, ed_try_cb, u);

    /* the sound */
    lv_obj_t *c = section(b, _("Sonido"));
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    u->ed_file = row_label(c, "");
    bn_btn(c, AOS_SYM_FOLDER_MUSIC, _("Elegir"), AOS_C_CARD2, ed_pick_cb, u);
    bn_btn(c, AOS_SYM_MICROPHONE, NULL, AOS_C_RED, ed_rec_cb, u);

    /* the name */
    c = section(b, _("Nombre"));
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    u->ed_name = row_label(c, "");
    bn_btn(c, AOS_SYM_PENCIL, NULL, AOS_C_CARD2, ed_name_cb, u);

    /* the colour */
    u->ed_colors = section(b, _("Color"));
    for (int i = 0; i < bn_n_colors; i++) {
        lv_obj_t *s = lv_obj_create(u->ed_colors);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 72, 72);
        lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(s, lv_color_hex(bn_colors[i]), 0);
        lv_obj_set_style_border_color(s, lv_color_white(), 0);
        lv_obj_set_style_border_opa(s, LV_OPA_COVER, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(s, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, ed_color_cb, LV_EVENT_CLICKED, u);
    }

    /* the icon */
    u->ed_icons = section(b, _("Ícono"));
    lv_obj_set_style_pad_row(u->ed_icons, 10, 0);
    lv_obj_set_style_pad_column(u->ed_icons, 10, 0);
    for (int i = 0; i < bn_n_icons; i++) {
        lv_obj_t *s = lv_obj_create(u->ed_icons);
        lv_obj_remove_style_all(s);
        lv_obj_set_size(s, 88, 88);
        lv_obj_set_style_radius(s, 20, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(s, (void *)(intptr_t)i);
        lv_obj_add_event_cb(s, ed_icon_cb, LV_EVENT_CLICKED, u);
        lv_obj_t *g = lv_label_create(s);
        lv_obj_set_style_text_font(g, &aos_sym_44, 0);
        lv_obj_set_style_text_color(g, AOS_C_TEXT, 0);
        lv_label_set_text(g, bn_icons[i].glyph);
        lv_obj_center(g);
        aos_make_decorative(g);
    }

    /* how it plays */
    c = section(b, _("Al tocarlo"));
    for (int m = 0; m < BN_MODE_COUNT; m++) {
        u->ed_mode[m] = bn_btn(c, NULL, _(MODE_NAME[m]), AOS_C_CARD2, ed_mode_cb, u);
        lv_obj_set_user_data(u->ed_mode[m], (void *)(intptr_t)m);
    }
    lv_obj_t *help = aos_label(c, "", aos_font_small, AOS_C_DIM);
    lv_label_set_long_mode(help, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(help, lv_pct(100));

    c = card(b);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    row_label(c, _("Repetir en bucle"));
    u->ed_loop = lv_switch_create(c);
    lv_obj_set_size(u->ed_loop, 100, 56);
    lv_obj_set_style_bg_color(u->ed_loop, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(u->ed_loop, ed_loop_cb, LV_EVENT_VALUE_CHANGED, u);

    c = section(b, _("Volumen"));
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_hor(c, 30, 0);
    u->ed_vol = lv_slider_create(c);
    lv_slider_set_range(u->ed_vol, 0, 100);
    lv_obj_set_height(u->ed_vol, 16);
    lv_obj_set_flex_grow(u->ed_vol, 1);
    lv_obj_set_style_bg_color(u->ed_vol, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(u->ed_vol, AOS_C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(u->ed_vol, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(u->ed_vol, 12, LV_PART_KNOB);
    lv_obj_set_ext_click_area(u->ed_vol, 30);
    lv_obj_add_event_cb(u->ed_vol, ed_vol_cb, LV_EVENT_VALUE_CHANGED, u);
    u->ed_vol_v = aos_label(c, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(u->ed_vol_v, 110);
    lv_obj_set_style_text_align(u->ed_vol_v, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t *act = box(b);
    lv_obj_set_size(act, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(act, 10, 0);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act, 16, 0);
    lv_obj_set_style_pad_row(act, 16, 0);
    bn_btn(act, AOS_SYM_DRAG, _("Mover"), AOS_C_CARD2, ed_move_cb, u);
    u->ed_clear = bn_btn(act, AOS_SYM_DELETE, _("Vaciar"), AOS_C_CARD2, ed_clear_cb, u);
    lv_obj_set_style_text_color(lv_obj_get_child(u->ed_clear, 0), AOS_C_RED, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(u->ed_clear, 1), AOS_C_RED, 0);

    if (bn_pad_empty(p)) {
        /* a new one: something to start from */
        p->color = bn_colors[(gi * 5) % bn_n_colors];
        p->icon = (uint8_t)bn_icon_find("note");
        p->vol = 100;
    }
    ed_sync(u);
    if (bn_pad_empty(p)) pick_open(u);
}

/* the editor's button gets a file: the name follows unless one was typed */
static void assign(bn_ui_t *u, const char *file)
{
    bn_pad_t *p = ed(u);
    if (!p) return;
    char old[BN_NAME_MAX];
    bn_pretty(p->file, old, sizeof old);
    bool keep = p->name[0] && p->file[0] && strcmp(old, p->name);
    bn_eng_stop_pad(u->ed_pad);
    snprintf(p->file, sizeof p->file, "%s", file);
    if (!keep) bn_pretty(file, p->name, sizeof p->name);
    bn_cache_get(u, p->file);
    ed_sync(u);
}

/* ---- the list of sounds ---- */

static void pk_play_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= u->files.n) return;
    if (bn_eng_busy(-1) && u->preview_slot >= 0 && !strcmp(u->cache[u->preview_slot].file, u->files.name[i]))
        bn_ui_preview(u, NULL);
    else
        bn_ui_preview(u, u->files.name[i]);
}

static void pk_pick_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= u->files.n) return;
    char f[BN_FILE_MAX];
    snprintf(f, sizeof f, "%s", u->files.name[i]);
    sheet2_close(u);
    assign(u, f);
}

static void pk_fill(bn_ui_t *u)
{
    if (!u->pk_list) return;
    lv_obj_clean(u->pk_list);
    bn_files_scan(&u->files);
    if (!u->files.n) {
        char m[200];
        snprintf(m, sizeof m, _("No hay sonidos en %s. Grabá uno o subilos desde el portal."), "sonidos");
        lv_obj_t *l = aos_label(u->pk_list, m, aos_font_body, AOS_C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
        return;
    }
    const bn_pad_t *p = ed(u);
    for (int i = 0; i < u->files.n; i++) {
        lv_obj_t *r = box(u->pk_list);
        lv_obj_set_size(r, lv_pct(100), AOS_UI_ROW_H);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 16, 0);
        lv_obj_t *pb = bn_btn(r, AOS_SYM_PLAY, NULL, AOS_C_CARD2, pk_play_cb, u);
        lv_obj_set_user_data(pb, (void *)(intptr_t)i);
        lv_obj_t *t = box(r);
        lv_obj_set_height(t, AOS_UI_ROW_H);
        lv_obj_set_flex_grow(t, 1);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(t, (void *)(intptr_t)i);
        lv_obj_add_event_cb(t, pk_pick_cb, LV_EVENT_CLICKED, u);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        bool cur = p && !strcmp(p->file, u->files.name[i]);
        lv_obj_t *n = aos_label(t, u->files.name[i], aos_font_body, cur ? AOS_C_ACCENT : AOS_C_TEXT);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(n, lv_pct(100), lv_font_get_line_height(aos_font_body));
        char sz[32];
        uint32_t b = u->files.size[i];
        if (b >= 1024 * 1024) snprintf(sz, sizeof sz, "%u,%u MB", (unsigned)(b >> 20), (unsigned)((b % (1024 * 1024)) * 10 / (1024 * 1024)));
        else snprintf(sz, sizeof sz, "%u KB", (unsigned)((b + 1023) / 1024));
        aos_label(t, sz, aos_font_caption, AOS_C_DIM);
        aos_make_decorative(n);
        lv_obj_t *sep = box(u->pk_list);
        lv_obj_set_size(sep, lv_pct(100), 1);
        lv_obj_set_style_bg_color(sep, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
    }
}

static void pk_rec_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    sheet2_close(u);
    rec_open(u);
}

static void pick_open(bn_ui_t *u)
{
    if (u->sheet2) return;
    lv_obj_t *b = bn_sheet_new(u, _("Elegir un sonido"), true);
    lv_obj_t *r = bn_btn(b, AOS_SYM_MICROPHONE, _("Grabar uno nuevo"), AOS_C_RED, pk_rec_cb, u);
    lv_obj_set_style_margin_bottom(r, 6, 0);
    u->pk_list = card(b);
    lv_obj_set_flex_flow(u->pk_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(u->pk_list, 0, 0);
    lv_obj_set_style_pad_ver(u->pk_list, 6, 0);
    pk_fill(u);
}

void bn_edit_file_changed(bn_ui_t *u) { pk_fill(u); }

/* ---- the recorder ---- */

static void rec_next_name(char *file, size_t n)
{
    struct stat st;
    for (int i = 1; i < 1000; i++) {
        char path[220];
        snprintf(file, n, "grabacion_%03d.wav", i);
        snprintf(path, sizeof path, "%s/%s", bn_dir(), file);
        if (stat(path, &st) != 0) return;
    }
}

static void rec_draw(bn_ui_t *u)
{
    if (!u->rc_btn) return;
    lv_obj_set_style_bg_color(u->rc_btn, u->rc_on ? AOS_C_CARD2 : AOS_C_RED, 0);
    lv_label_set_text(u->rc_btn_l, u->rc_on ? AOS_SYM_STOP : AOS_SYM_RECORD);
    bool done = !u->rc_on && u->rc_file[0];
    if (done) {
        lv_obj_remove_flag(u->rc_use, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(u->rc_again, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(u->rc_use, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(u->rc_again, LV_OBJ_FLAG_HIDDEN);
    }
}

static void rec_stop(bn_ui_t *u)
{
    if (!u->rc_on) return;
    bool saved = aos_hal_rec_stop();
    u->rc_on = false;
    bn_ui_engine(u, true);
    if (saved) {
        char m[160];
        snprintf(m, sizeof m, _("Guardado como %s"), u->rc_file);
        lv_label_set_text(u->rc_msg, m);
    } else {
        lv_label_set_text(u->rc_msg, _("No quedó nada grabado"));
        u->rc_file[0] = 0;
    }
    rec_draw(u);
}

static void rec_btn_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    if (u->rc_on) {
        rec_stop(u);
        return;
    }
    rec_next_name(u->rc_file, sizeof u->rc_file);
    snprintf(u->rc_path, sizeof u->rc_path, "%s/%s", bn_dir(), u->rc_file);
    /* the speaker off while the microphone listens */
    bn_eng_stop_all();
    bn_ui_engine(u, false);
    if (!aos_hal_rec_start(u->rc_path, BN_RATE)) {
        bn_ui_engine(u, true);
        u->rc_file[0] = 0;
        lv_label_set_text(u->rc_msg, _("No se pudo abrir el micrófono"));
        rec_draw(u);
        return;
    }
    u->rc_on = true;
    u->rc_t0 = (uint32_t)aos_hal_uptime_ms();
    char m[96];
    snprintf(m, sizeof m, _("Grabando... (hasta %d segundos)"), BN_CLIP_MAX_S);
    lv_label_set_text(u->rc_msg, m);
    rec_draw(u);
}

static void rec_use_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    char f[BN_FILE_MAX];
    snprintf(f, sizeof f, "%s", u->rc_file);
    sheet2_close(u);
    if (f[0]) assign(u, f);
}

static void rec_play_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    if (u->rc_on || !u->rc_file[0]) return;
    bn_ui_preview(u, u->rc_file);
}

static void rec_again_cb(lv_event_t *e)
{
    bn_ui_t *u = lv_event_get_user_data(e);
    bn_ui_preview(u, NULL);
    u->rc_file[0] = 0;
    lv_label_set_text(u->rc_time, "0:00.0");
    lv_label_set_text(u->rc_msg, _("Tocá el botón rojo para grabar"));
    rec_draw(u);
}

static void rec_open(bn_ui_t *u)
{
    if (u->sheet2) return;
    lv_obj_t *b = bn_sheet_new(u, _("Grabar un sonido"), true);
    u->rc_file[0] = 0;
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 26, 0);
    bool land = lv_obj_get_width(u->root) > lv_obj_get_height(u->root);
    lv_obj_set_style_pad_top(b, land ? 0 : 60, 0);
    u->rc_time = aos_label(b, "0:00.0", aos_font_huge, AOS_C_TEXT);
    u->rc_meter = lv_bar_create(b);
    lv_obj_set_size(u->rc_meter, lv_pct(80), 22);
    lv_bar_set_range(u->rc_meter, 0, 100);
    lv_obj_set_style_bg_color(u->rc_meter, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(u->rc_meter, AOS_C_GREEN, LV_PART_INDICATOR);
    lv_obj_t *row = box(b);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 40, 0);
    u->rc_btn = lv_obj_create(row);
    lv_obj_remove_style_all(u->rc_btn);
    int d = land ? 150 : 190;
    lv_obj_set_size(u->rc_btn, d, d);
    lv_obj_set_style_radius(u->rc_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(u->rc_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(u->rc_btn, lv_color_white(), 0);
    lv_obj_set_style_border_width(u->rc_btn, 6, 0);
    lv_obj_add_flag(u->rc_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(u->rc_btn, rec_btn_cb, LV_EVENT_CLICKED, u);
    u->rc_btn_l = lv_label_create(u->rc_btn);
    lv_obj_set_style_text_font(u->rc_btn_l, &aos_sym_72, 0);
    lv_obj_set_style_text_color(u->rc_btn_l, lv_color_white(), 0);
    lv_obj_center(u->rc_btn_l);
    aos_make_decorative(u->rc_btn_l);
    u->rc_msg = aos_label(b, _("Tocá el botón rojo para grabar"), aos_font_body, AOS_C_DIM);
    lv_label_set_long_mode(u->rc_msg, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(u->rc_msg, lv_pct(100));
    lv_obj_set_style_text_align(u->rc_msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *act = box(b);
    lv_obj_set_size(act, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(act, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(act, 16, 0);
    lv_obj_set_style_pad_row(act, 16, 0);
    u->rc_use = bn_btn(act, AOS_SYM_CHECK, _("Usar este"), AOS_C_ACCENT, rec_use_cb, u);
    lv_obj_t *pl = bn_btn(act, AOS_SYM_PLAY, _("Escuchar"), AOS_C_CARD2, rec_play_cb, u);
    u->rc_again = bn_btn(act, AOS_SYM_RESTART, _("Grabar otro"), AOS_C_CARD2, rec_again_cb, u);
    (void)pl;
    /* "Listen" belongs to the same moment as "use it" */
    lv_obj_add_flag(pl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_user_data(u->rc_use, pl);
    rec_draw(u);
}

void bn_rec_tick(bn_ui_t *u)
{
    if (!u->rc_btn) return;
    lv_obj_t *pl = lv_obj_get_user_data(u->rc_use);
    if (pl) {
        if (lv_obj_has_flag(u->rc_use, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(pl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(pl, LV_OBJ_FLAG_HIDDEN);
    }
    if (!u->rc_on) {
        lv_bar_set_value(u->rc_meter, 0, LV_ANIM_OFF);
        return;
    }
    aos_rec_status_t st;
    if (!aos_hal_rec_status(&st)) return;
    uint32_t ms = st.elapsed_ms;
    char t[24];
    snprintf(t, sizeof t, "%u:%02u.%u", (unsigned)(ms / 60000), (unsigned)(ms / 1000 % 60), (unsigned)(ms / 100 % 10));
    lv_label_set_text(u->rc_time, t);
    lv_bar_set_value(u->rc_meter, st.level, LV_ANIM_OFF);
    uint8_t pk[64];
    aos_hal_rec_peaks(pk, sizeof pk);       /* drained, so the ring does not fill */
    if (ms >= (uint32_t)BN_CLIP_MAX_S * 1000u) rec_stop(u);
}
