/*
 * P4OS - The two pull-down panels.
 *
 *   Control Centre      from the top edge, right half. Wi-Fi, Bluetooth, do
 *                       not disturb and the orientation; brightness and
 *                       volume; what is playing; shortcuts; the Home
 *                       Assistant scenes (phase 5).
 *   Notifications       from the top edge, left half. The clock, and the
 *                       history of aos_hal_notif_*.
 *
 * Both follow the finger while it pulls (aos_panel_drag from the gesture
 * code in aos_ui.c) and settle open or closed on release by distance and
 * speed. Closing: a swipe up anywhere on the panel, a tap on empty space,
 * or back.
 *
 * The background is a flat translucent dark, not a real blur: a blur of the
 * whole screen per frame is what UI.md says to avoid on this panel.
 */
#include "aos_internal.h"
#include "aos_text_safe.h"
#include "aos_hal.h"
#include "aos_theme.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static lv_obj_t *s_cc, *s_nc;           /* the two panels */
static aos_panel_t s_open;
static aos_panel_t s_dragging;

/* control centre widgets that the tick refreshes */
static lv_obj_t *s_btn_wifi, *s_btn_bt, *s_btn_dnd, *s_btn_rot;
static lv_obj_t *s_orient_lbl, *s_orient_glyph;
static lv_obj_t *s_bright, *s_vol;
static lv_obj_t *s_media_title, *s_media_artist, *s_media_play;
/* notification centre */
static lv_obj_t *s_nc_time, *s_nc_date, *s_nc_list;
static int s_nc_count = -1;

static lv_obj_t *panel(lv_obj_t *layer, aos_panel_t which);
static void cc_build(void);
static void nc_build(void);

/* -------------------------------------------------------------------------- */
/* Pull, settle                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *obj_of(aos_panel_t p) { return p == AOS_PANEL_CONTROL ? s_cc : p == AOS_PANEL_NOTIF ? s_nc : NULL; }

static void y_cb(void *var, int32_t v) { lv_obj_set_y(var, v); }

/* By the end value, not lv_obj_get_y(): the coordinates are only brought up
 * to date at the next layout, and here they still say where it started. */
static void hidden_cb(lv_anim_t *a)
{
    if (a->end_value < 0) lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN);
}

static void settle(lv_obj_t *o, bool open)
{
    const aos_geo_t *g = aos_ui_geo();
    lv_obj_update_layout(o);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(o), open ? 0 : -g->h);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, hidden_cb);
    lv_anim_start(&a);
}

void aos_panel_drag(aos_panel_t which, int32_t dy)
{
    lv_obj_t *o = obj_of(which);
    if (!o) return;
    const aos_geo_t *g = aos_ui_geo();
    if (s_dragging != which) {
        s_dragging = which;
        if (which == AOS_PANEL_CONTROL) cc_build(); else nc_build();
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(o);
    }
    lv_obj_set_y(o, LV_CLAMP(-g->h, -g->h + dy, 0));
}

void aos_panel_release(aos_panel_t which, int32_t dy, int32_t vy)
{
    lv_obj_t *o = obj_of(which);
    s_dragging = AOS_PANEL_NONE;
    if (!o) return;
    const aos_geo_t *g = aos_ui_geo();
    bool open = dy > g->h / 4 || vy > 10;
    s_open = open ? which : AOS_PANEL_NONE;
    settle(o, open);
}

void aos_panel_open(aos_panel_t which)
{
    const aos_geo_t *g = aos_ui_geo();
    aos_panel_drag(which, 0);
    s_dragging = AOS_PANEL_NONE;
    lv_obj_set_y(obj_of(which), -g->h);
    s_open = which;
    settle(obj_of(which), true);
}

bool aos_panel_close(void)
{
    if (s_open == AOS_PANEL_NONE) return false;
    lv_obj_t *o = obj_of(s_open);
    s_open = AOS_PANEL_NONE;
    if (o) settle(o, false);
    return true;
}

aos_panel_t aos_panel_current(void) { return s_open; }

/* a swipe up on an open panel, or a tap on its background, closes it */
static void panel_event(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_GESTURE) {
        if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) aos_panel_close();
    } else if (c == LV_EVENT_CLICKED && lv_event_get_target(e) == lv_event_get_current_target(e)) {
        aos_panel_close();
    }
}

static lv_obj_t *panel(lv_obj_t *layer, aos_panel_t which)
{
    lv_obj_t *p = lv_obj_create(layer);
    lv_obj_remove_style_all(p);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x0E1116), 0);
    lv_obj_set_style_bg_opa(p, 235, 0);
    lv_obj_set_style_text_color(p, lv_color_white(), 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(p, panel_event, LV_EVENT_ALL, NULL);
    (void)which;
    return p;
}

void aos_panels_create(lv_obj_t *layer)
{
    s_cc = panel(layer, AOS_PANEL_CONTROL);
    s_nc = panel(layer, AOS_PANEL_NOTIF);
    aos_panels_layout();
}

void aos_panels_layout(void)
{
    const aos_geo_t *g = aos_ui_geo();
    lv_obj_t *ps[] = { s_cc, s_nc };
    for (int i = 0; i < 2; i++) {
        lv_obj_set_size(ps[i], g->w, g->h);
        lv_obj_set_pos(ps[i], 0, -g->h);
        lv_obj_add_flag(ps[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(ps[i]);
    }
    s_open = s_dragging = AOS_PANEL_NONE;
    s_nc_count = -1;
}

/* -------------------------------------------------------------------------- */
/* Control Centre                                                              */
/* -------------------------------------------------------------------------- */

static int32_t U, GAP;          /* tile unit and gap for this orientation */

static lv_obj_t *tile(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *t = lv_obj_create(parent);
    lv_obj_remove_style_all(t);
    lv_obj_set_size(t, w * U + (w - 1) * GAP, h * U + (h - 1) * GAP);
    lv_obj_set_style_radius(t, 36, 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    return t;
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, int32_t size)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x444B57), 0);
    lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &aos_sym_44, 0);
    lv_label_set_text(l, glyph);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void wifi_cb(lv_event_t *e) { aos_hal_net_enable(!aos_hal_net_enabled()); aos_panels_tick(); }
static void bt_cb(lv_event_t *e) { aos_hal_bt_enable(!aos_hal_bt_enabled()); aos_panels_tick(); }
static void dnd_cb(lv_event_t *e) { aos_hal_notif_enable(!aos_hal_notif_enabled()); aos_panels_tick(); }
static void rot_cb(lv_event_t *e) { aos_panel_close(); aos_ui_request_landscape(-1); }
static void bright_cb(lv_event_t *e) { aos_hal_brightness_set(lv_slider_get_value(lv_event_get_target(e))); }
static void vol_cb(lv_event_t *e) { aos_hal_volume_set(lv_slider_get_value(lv_event_get_target(e))); }
static void settings_cb(lv_event_t *e) { aos_panel_close(); aos_ui_open("aos.settings"); }
static void screen_off_cb(lv_event_t *e) { aos_panel_close(); aos_hal_display_on(false); }
static void edit_cb(lv_event_t *e) { aos_panel_close(); aos_ui_edit_home(); }
static void media_play_cb(lv_event_t *e)
{
    aos_player_info_t in;
    if (aos_hal_player_info(&in) && in.state == AOS_PLAYER_PLAYING) aos_hal_player_pause();
    else if (aos_hal_player_info(&in) && in.state == AOS_PLAYER_PAUSED) aos_hal_player_resume();
    else aos_hal_player_resume_last();
    aos_panels_tick();
}
static void media_next_cb(lv_event_t *e) { aos_hal_player_next(); }
static void media_prev_cb(lv_event_t *e) { aos_hal_player_prev(); }

static lv_obj_t *vslider(lv_obj_t *parent, const char *glyph, int value, lv_event_cb_t cb)
{
    lv_obj_t *t = tile(parent, 1, 2);
    lv_obj_set_style_bg_opa(t, LV_OPA_TRANSP, 0);
    lv_obj_t *s = lv_slider_create(t);
    lv_obj_set_size(s, lv_obj_get_style_width(t, 0), lv_obj_get_style_height(t, 0));
    lv_obj_center(s);
    lv_slider_set_range(s, 0, 100);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_radius(s, 36, 0);
    lv_obj_set_style_radius(s, 0, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_clip_corner(s, true, 0);
    lv_obj_set_style_bg_opa(s, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *l = lv_label_create(t);
    lv_obj_set_style_text_font(l, &aos_sym_44, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0x5A6270), 0);
    lv_label_set_text(l, glyph);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_add_flag(l, LV_OBJ_FLAG_IGNORE_LAYOUT);
    return s;
}

static lv_obj_t *shortcut(lv_obj_t *parent, const char *glyph, const char *label, lv_event_cb_t cb)
{
    lv_obj_t *t = tile(parent, 1, 1);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(t, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t *g = lv_label_create(t);
    lv_obj_set_style_text_font(g, &aos_sym_44, 0);
    lv_label_set_text(g, glyph);
    lv_obj_align(g, LV_ALIGN_CENTER, 0, label ? -14 : 0);
    if (label) {
        lv_obj_t *l = lv_label_create(t);
        lv_obj_set_style_text_font(l, aos_font_tiny, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xB8C0CC), 0);
        lv_label_set_text(l, label);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -14);
    }
    lv_obj_add_event_cb(t, cb, LV_EVENT_CLICKED, NULL);
    return t;
}

static void cc_build(void)
{
    const aos_geo_t *g = aos_ui_geo();
    lv_obj_clean(s_cc);
    GAP = 20;
    /* upright: 4 columns (connectivity + media / orientation + two sliders /
     * four shortcuts / scenes); lying down: 8 columns in two rows */
    int cols = g->landscape ? 8 : 4;
    int32_t side = g->landscape ? 60 : 44;
    U = (g->w - 2 * side - (cols - 1) * GAP) / cols;
    if (g->landscape && U > 128) U = 128;
    int32_t grid_w = cols * U + (cols - 1) * GAP;

    lv_obj_t *grid = lv_obj_create(s_cc);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, grid_w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, GAP, 0);
    lv_obj_set_style_pad_column(grid, GAP, 0);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, g->bar_h + (g->landscape ? 16 : 60));
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    /* connectivity: 2x2 with four round buttons */
    lv_obj_t *conn = tile(grid, 2, 2);
    int32_t rb = U - 36;
    lv_obj_set_flex_flow(conn, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(conn, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_SPACE_EVENLY);
    lv_obj_set_style_pad_all(conn, 12, 0);
    s_btn_wifi = round_btn(conn, AOS_SYM_WIFI, wifi_cb, rb);
    s_btn_bt = round_btn(conn, AOS_SYM_BLUETOOTH, bt_cb, rb);
    s_btn_dnd = round_btn(conn, AOS_SYM_MOON_WANING_CRESCENT, dnd_cb, rb);
    s_btn_rot = round_btn(conn, AOS_SYM_SCREEN_ROTATION, rot_cb, rb);

    /* now playing: 2x2 */
    lv_obj_t *media = tile(grid, 2, 2);
    lv_obj_set_style_pad_all(media, 24, 0);
    s_media_title = lv_label_create(media);
    lv_obj_set_width(s_media_title, 2 * U + GAP - 48);
    lv_label_set_long_mode(s_media_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_media_title, aos_font_small, 0);
    s_media_artist = lv_label_create(media);
    lv_obj_set_width(s_media_artist, 2 * U + GAP - 48);
    lv_label_set_long_mode(s_media_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(s_media_artist, aos_font_caption, 0);
    lv_obj_set_style_text_color(s_media_artist, lv_color_hex(0xB8C0CC), 0);
    lv_obj_align(s_media_artist, LV_ALIGN_TOP_LEFT, 0, 36);
    lv_obj_t *row = lv_obj_create(media);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 80);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *pr = round_btn(row, AOS_SYM_SKIP_PREVIOUS, media_prev_cb, 72);
    s_media_play = round_btn(row, AOS_SYM_PLAY, media_play_cb, 72);
    lv_obj_t *nx = round_btn(row, AOS_SYM_SKIP_NEXT, media_next_cb, 72);
    lv_obj_set_style_bg_opa(pr, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(nx, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(s_media_play, LV_OPA_TRANSP, 0);

    /* orientation: 2x1, says what it is and turns it on a tap */
    lv_obj_t *orient = tile(grid, 2, 1);
    lv_obj_add_flag(orient, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(orient, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_event_cb(orient, rot_cb, LV_EVENT_CLICKED, NULL);
    s_orient_glyph = lv_label_create(orient);
    lv_obj_set_style_text_font(s_orient_glyph, &aos_sym_44, 0);
    lv_obj_align(s_orient_glyph, LV_ALIGN_LEFT_MID, 24, 0);
    s_orient_lbl = lv_label_create(orient);
    lv_obj_set_style_text_font(s_orient_lbl, aos_font_caption, 0);
    lv_obj_align(s_orient_lbl, LV_ALIGN_LEFT_MID, 84, 0);

    s_bright = vslider(grid, AOS_SYM_BRIGHTNESS_6, aos_hal_brightness_get(), bright_cb);
    s_vol = vslider(grid, AOS_SYM_VOLUME_HIGH, aos_hal_volume_get(), vol_cb);
    if (g->landscape) lv_obj_move_to_index(orient, -1);   /* after the sliders: row one is 8 wide */

    shortcut(grid, AOS_SYM_COG, _("Ajustes"), settings_cb);
    shortcut(grid, AOS_SYM_MONITOR, _("Apagar"), screen_off_cb);
    shortcut(grid, AOS_SYM_VIEW_GRID_OUTLINE, _("Inicio"), edit_cb);
    shortcut(grid, AOS_SYM_LOCK, _("Bloquear"), screen_off_cb);

    /* Home Assistant's scenes, when the HA service registered its tile */
    int ha_cols = g->landscape ? 4 : cols;
    lv_obj_t *ha = tile(grid, ha_cols, 1);
    aos_widget_create_t cc_ha = aos_ui_widget_find("cc.ha");
    if (cc_ha) {
        cc_ha(ha, ha_cols * U + (ha_cols - 1) * GAP, U, NULL);
        aos_panels_tick();
        return;
    }
    lv_obj_set_flex_flow(ha, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ha, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ha, 14, 0);
    lv_obj_t *hg = lv_label_create(ha);
    lv_obj_set_style_text_font(hg, &aos_sym_44, 0);
    lv_obj_set_style_text_color(hg, lv_color_hex(0x41BDF5), 0);
    lv_label_set_text(hg, AOS_SYM_HOME_ASSISTANT);
    lv_obj_t *hl = lv_label_create(ha);
    lv_obj_set_style_text_font(hl, aos_font_caption, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(0x8A93A3), 0);
    lv_label_set_text(hl, _("Home Assistant"));
    aos_make_decorative(ha);

    aos_panels_tick();
}

/* -------------------------------------------------------------------------- */
/* Notifications                                                               */
/* -------------------------------------------------------------------------- */

static void clear_cb(lv_event_t *e) { aos_hal_notif_clear(); s_nc_count = -1; aos_panels_tick(); }

/* One away; its provider hears it (HA dismisses it over there too). */
static void dismiss_cb(lv_event_t *e)
{
    aos_hal_notif_remove((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    s_nc_count = -1;
    aos_panels_tick();
}

static void nc_build(void)
{
    const aos_geo_t *g = aos_ui_geo();
    lv_obj_clean(s_nc);
    s_nc_time = lv_label_create(s_nc);
    lv_obj_set_style_text_font(s_nc_time, aos_font_huge, 0);
    lv_obj_align(s_nc_time, LV_ALIGN_TOP_MID, 0, g->bar_h + (g->landscape ? 4 : 40));
    s_nc_date = lv_label_create(s_nc);
    lv_obj_set_style_text_font(s_nc_date, aos_font_body, 0);
    lv_obj_set_style_text_color(s_nc_date, lv_color_hex(0xB8C0CC), 0);
    lv_obj_align_to(s_nc_date, s_nc_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);

    int32_t top = g->bar_h + (g->landscape ? 130 : 200);
    s_nc_list = lv_obj_create(s_nc);
    lv_obj_remove_style_all(s_nc_list);
    lv_obj_set_size(s_nc_list, LV_MIN(g->w - 48, 760), g->h - top - 40);
    lv_obj_align(s_nc_list, LV_ALIGN_TOP_MID, 0, top);
    lv_obj_set_flex_flow(s_nc_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_nc_list, 14, 0);
    lv_obj_set_scroll_dir(s_nc_list, LV_DIR_VER);
    s_nc_count = -1;
    aos_panels_tick();
}

static void nc_fill(void)
{
    lv_obj_clean(s_nc_list);
    int n = aos_hal_notif_count();
    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_nc_list);
        lv_obj_set_style_text_font(l, aos_font_body, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0x8A93A3), 0);
        lv_label_set_text(l, _("Sin notificaciones"));
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    lv_obj_t *clr = lv_button_create(s_nc_list);
    lv_obj_set_style_bg_color(clr, lv_color_hex(0x2A2F38), 0);
    lv_obj_set_style_radius(clr, 24, 0);
    lv_obj_set_style_shadow_width(clr, 0, 0);
    lv_obj_t *cl = lv_label_create(clr);
    lv_obj_set_style_text_font(cl, aos_font_caption, 0);
    lv_label_set_text(cl, _("Borrar todo"));
    lv_obj_add_event_cb(clr, clear_cb, LV_EVENT_CLICKED, NULL);
    time_t now = time(NULL);
    for (int i = n - 1; i >= 0; i--) {
        aos_notif_t nt;
        if (!aos_hal_notif_at(i, &nt)) continue;
        lv_obj_t *card = lv_obj_create(s_nc_list);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_radius(card, 28, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x2A2F38), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(card, 22, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 6, 0);
        lv_obj_t *x = lv_obj_create(card);
        lv_obj_remove_style_all(x);
        lv_obj_add_flag(x, LV_OBJ_FLAG_IGNORE_LAYOUT | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(x, 64, 64);
        lv_obj_align(x, LV_ALIGN_TOP_RIGHT, 14, -14);
        lv_obj_set_ext_click_area(x, 12);
        lv_obj_set_style_radius(x, 32, 0);
        lv_obj_set_style_bg_color(x, lv_color_hex(0x3A404B), LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(x, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(x, dismiss_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)nt.uid);
        lv_obj_t *xl = lv_label_create(x);
        lv_obj_set_style_text_font(xl, &aos_sym_28, 0);
        lv_obj_set_style_text_color(xl, lv_color_hex(0x8A93A3), 0);
        lv_label_set_text(xl, AOS_SYM_CLOSE);
        lv_obj_center(xl);
        lv_obj_t *head = lv_label_create(card);
        lv_obj_set_style_text_font(head, aos_font_caption, 0);
        lv_obj_set_style_text_color(head, lv_color_hex(0xB8C0CC), 0);
        long mins = nt.when ? (long)(now - nt.when) / 60 : 0;
        if (mins < 1) lv_label_set_text_fmt(head, "%s  ·  %s", nt.app, _("ahora"));
        else if (mins < 60) lv_label_set_text_fmt(head, "%s  ·  %ld min", nt.app, mins);
        else lv_label_set_text_fmt(head, "%s  ·  %ld h", nt.app, mins / 60);
        lv_obj_t *t = lv_label_create(card);
        lv_obj_set_style_text_font(t, aos_font_small, 0);
        char safe[320];
        aos_text_safe(safe, sizeof safe, nt.title);
        lv_label_set_text(t, safe);
        lv_obj_t *m = lv_label_create(card);
        lv_obj_set_width(m, lv_pct(100));
        lv_obj_set_style_text_font(m, aos_font_caption, 0);
        lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
        aos_text_safe(safe, sizeof safe, nt.message);
        lv_label_set_text(m, safe);
    }
}

/* -------------------------------------------------------------------------- */
/* Refresh                                                                     */
/* -------------------------------------------------------------------------- */

void aos_panels_tick(void)
{
    if (s_cc && !lv_obj_has_flag(s_cc, LV_OBJ_FLAG_HIDDEN) && s_btn_wifi) {
        lv_obj_set_state(s_btn_wifi, LV_STATE_CHECKED, aos_hal_net_enabled());
        lv_obj_set_state(s_btn_bt, LV_STATE_CHECKED, aos_hal_bt_enabled());
        lv_obj_set_state(s_btn_dnd, LV_STATE_CHECKED, !aos_hal_notif_enabled());
        bool land = aos_ui_landscape();
        lv_obj_set_state(s_btn_rot, LV_STATE_CHECKED, land);
        lv_label_set_text(s_orient_glyph, land ? AOS_SYM_PHONE_ROTATE_LANDSCAPE : AOS_SYM_PHONE_ROTATE_PORTRAIT);
        lv_label_set_text(s_orient_lbl, land ? _("Horizontal") : _("Vertical"));
        aos_player_info_t in;
        bool have = aos_hal_player_info(&in) && in.state != AOS_PLAYER_STOPPED;
        lv_label_set_text(s_media_title, have && in.title[0] ? in.title : _("Sin reproducción"));
        lv_label_set_text(s_media_artist, have ? in.artist : "");
        lv_label_set_text(lv_obj_get_child(s_media_play, 0),
                          have && in.state == AOS_PLAYER_PLAYING ? AOS_SYM_PAUSE : AOS_SYM_PLAY);
    }
    if (s_nc && !lv_obj_has_flag(s_nc, LV_OBJ_FLAG_HIDDEN) && s_nc_time) {
        struct tm t;
        aos_hal_time_now(&t);
        lv_label_set_text_fmt(s_nc_time, "%d:%02d", t.tm_hour, t.tm_min);
        lv_label_set_text_fmt(s_nc_date, "%s %d %s", aos_day_name(t.tm_wday), t.tm_mday, aos_month_name(t.tm_mon));
        lv_obj_align_to(s_nc_date, s_nc_time, LV_ALIGN_OUT_BOTTOM_MID, 0, 4);
        int n = aos_hal_notif_count();
        if (n != s_nc_count) { s_nc_count = n; nc_fill(); }
    }
}
