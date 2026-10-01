/*
 * P4OS - Radio (from AmoledOS): the front panel. See radio.h.
 *
 * Upright (720 x 1204):            Lying down (1280 x 660):
 *   the dial                         the dial          | the nine keys
 *   mute, prev, play, next, info     the transport     | all the stations
 *   the volume                       the volume        |
 *   the nine keys
 *   all the stations
 *
 * The dial is backlit amber glass with the cover, the station, the song,
 * the stream's format and state, and the tuning scale with its needle over
 * the key that plays. Everything is LVGL objects that stand still except
 * the needle, moved by a 30 ms timer only while it travels, and the song's
 * title, which scrolls by itself when it does not fit.
 */
#include "radio.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <string.h>

#define C_BODY_TOP   0x1C1410
#define C_BODY_BOT   0x070504
#define C_GLASS_TOP  0x2E1C08
#define C_GLASS_BOT  0x130B03
#define C_BRASS      0x8A6A36
#define C_AMBER      0xFFB547
#define C_AMBER_DIM  0xA07A3E
#define C_WARM       0xF4E7CF
#define C_WARM_DIM   0xCDBB98
#define C_NEEDLE     0xFF3B30
#define C_METAL_TOP  0x302824
#define C_METAL_BOT  0x151110
#define C_METAL_EDGE 0x5A4632
#define C_IVORY_TOP  0xF1E8D4
#define C_IVORY_BOT  0xCBBD9C
#define C_IVORY_TXT  0x2B2014
#define C_LED_OFF    0x5C4A30
#define C_LED_ON     0xFFB000
#define C_ROW        0x231A14
#define C_ROW_ON     0x3A2A18

#define PAD          AOS_UI_PAD
#define SCALE_MARGIN 30

/* x of key i's mark on the scale, in the scale's own pixels */
static int mark_x(const radio_t *r, int i)
{
    return SCALE_MARGIN + i * (r->scale_w - 2 * SCALE_MARGIN) / (RADIO_KEYS - 1);
}

static lv_obj_t *plain(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                      int x, int y, int w, lv_label_long_mode_t mode)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, mode);
    if (mode != LV_LABEL_LONG_MODE_WRAP) {
        lv_obj_set_height(l, lv_font_get_line_height(font));   /* one line, never two */
    }
    lv_label_set_text(l, "");
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static void grad(lv_obj_t *o, uint32_t top, uint32_t bot)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(o, lv_color_hex(bot), 0);
    lv_obj_set_style_bg_grad_dir(o, LV_GRAD_DIR_VER, 0);
}

/* ---- events ------------------------------------------------------------------ */

static void on_click(lv_event_t *e)
{
    radio_t *r = lv_event_get_user_data(e);
    if (r->closing) {
        return;
    }
    lv_obj_t *t = lv_event_get_current_target(e);
    if (t == r->b_play) {
        radio_on_play(r);
    } else if (t == r->b_prev) {
        radio_on_step(r, -1);
    } else if (t == r->b_next) {
        radio_on_step(r, 1);
    } else if (t == r->b_mute) {
        radio_on_mute(r);
    } else if (t == r->b_info || t == r->dial) {
        radio_ui_info_open(r);
    } else if (t == r->info) {
        radio_ui_info_close(r);
    }
}

/* A key: a short press tunes it, holding it down stores what plays - the
 * way a car radio's presets work. SHORT_CLICKED because CLICKED would
 * follow the long press too. */
static void on_key(lv_event_t *e)
{
    radio_t *r = lv_event_get_user_data(e);
    if (r->closing) {
        return;
    }
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int i = 0; i < RADIO_KEYS; i++) {
        if (t == r->key[i]) {
            if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
                radio_on_key_store(r, i);
            } else {
                radio_on_key(r, i);
            }
            return;
        }
    }
}

static void on_row(lv_event_t *e)
{
    radio_t *r = lv_event_get_user_data(e);
    if (r->closing) {
        return;
    }
    lv_obj_t *t = lv_event_get_current_target(e);
    for (int i = 0; i < r->lib.count && i < RADIO_LIB_MAX; i++) {
        if (t == r->row[i]) {
            radio_on_station(r, i);
            return;
        }
    }
}

static void on_volume(lv_event_t *e)
{
    radio_t *r = lv_event_get_user_data(e);
    if (!r->closing) {
        radio_on_volume(r, (int)lv_slider_get_value(r->vol));
    }
}

/* ---- the dial ---------------------------------------------------------------- */

static void scale_draw(radio_t *r)
{
    uint32_t *px = (uint32_t *)r->scale_px;
    const int W = r->scale_w, H = r->scale_h;
    memset(px, 0, (size_t)W * (size_t)H * 4);
    const uint32_t amber = 0xFF000000u | C_AMBER;
    const uint32_t dim   = 0xB0000000u | C_AMBER_DIM;
    int base = H - 4;
    for (int x = 8; x < W - 8; x++) {
        px[base * W + x] = dim;
        px[(base + 1) * W + x] = dim;
    }
    /* minor ticks, four between keys, a major one on every key */
    int step = (mark_x(r, 1) - mark_x(r, 0)) / 5;
    if (step < 4) {
        step = 4;
    }
    for (int k = 0; k < RADIO_KEYS; k++) {
        for (int m = (k == 0 ? -2 : 0); m < (k == RADIO_KEYS - 1 ? 3 : 5); m++) {
            int x = mark_x(r, k) + m * step;
            if (k < RADIO_KEYS - 1 && m > 0) {
                x = mark_x(r, k) + m * (mark_x(r, k + 1) - mark_x(r, k)) / 5;
            }
            if (x < 2 || x > W - 4) {
                continue;
            }
            bool major = m == 0;
            int h = major ? 20 : 9;
            for (int y = base - h; y < base; y++) {
                px[y * W + x] = major ? amber : dim;
                px[y * W + x + 1] = major ? amber : dim;
                if (major) {
                    px[y * W + x + 2] = amber;
                }
            }
        }
    }
}

static void build_dial(radio_t *r, lv_obj_t *root, int x, int y, int w, int h)
{
    r->dial_w = w;
    r->dial = plain(root, x, y, w, h);
    grad(r->dial, C_GLASS_TOP, C_GLASS_BOT);
    lv_obj_set_style_radius(r->dial, AOS_UI_RADIUS, 0);
    lv_obj_set_style_border_width(r->dial, 3, 0);
    lv_obj_set_style_border_color(r->dial, lv_color_hex(C_BRASS), 0);
    lv_obj_add_flag(r->dial, LV_OBJ_FLAG_CLICKABLE);       /* tap: the info card */
    lv_obj_add_event_cb(r->dial, on_click, LV_EVENT_CLICKED, r);

    /* the scale takes the bottom: numbers above it, then the ticks */
    r->scale_h = 34;
    r->scale_x = 20;
    r->scale_w = w - 2 * r->scale_x;
    int num_h = lv_font_get_line_height(aos_font_caption);
    r->scale_y = h - r->scale_h - 14;
    int top_h = r->scale_y - num_h - 8;         /* what is above the scale */

    /* the cover, in a brass frame */
    r->cover_sz = top_h - 24;
    lv_obj_t *frame = plain(r->dial, 18, 18, r->cover_sz + 6, r->cover_sz + 6);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(C_BRASS), 0);
    lv_obj_set_style_radius(frame, 10, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    r->cover_px = art_big_alloc((unsigned)(r->cover_sz * r->cover_sz * 2));
    r->cover = lv_canvas_create(frame);
    lv_obj_remove_flag(r->cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(r->cover, 3, 3);
    if (r->cover_px) {
        lv_canvas_set_buffer(r->cover, r->cover_px, r->cover_sz, r->cover_sz,
                             LV_COLOR_FORMAT_RGB565);
    }

    int tx = r->cover_sz + 48, tw = w - tx - 24;
    int ty = 18;
    r->station = text(r->dial, aos_font_title, C_AMBER, tx, ty, tw - 30, LV_LABEL_LONG_MODE_DOTS);
    r->onair = plain(r->dial, w - 40, ty + 12, 16, 16);
    lv_obj_set_style_radius(r->onair, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(r->onair, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r->onair, lv_color_hex(0x401010), 0);
    ty += lv_font_get_line_height(aos_font_title) + 6;
    r->title = text(r->dial, aos_font_body, C_WARM, tx, ty, tw, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    ty += lv_font_get_line_height(aos_font_body) + 2;
    r->artist = text(r->dial, aos_font_small, C_WARM_DIM, tx, ty, tw, LV_LABEL_LONG_MODE_DOTS);
    ty += lv_font_get_line_height(aos_font_small) + 8;
    r->meta = text(r->dial, aos_font_caption, C_AMBER_DIM, tx, ty, tw, LV_LABEL_LONG_MODE_DOTS);
    ty += lv_font_get_line_height(aos_font_caption) + 4;
    r->status = text(r->dial, aos_font_caption, C_AMBER_DIM, tx, ty, tw, LV_LABEL_LONG_MODE_DOTS);

    r->scale_px = art_big_alloc((unsigned)(r->scale_w * r->scale_h * 4));
    r->scale = lv_canvas_create(r->dial);
    lv_obj_remove_flag(r->scale, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(r->scale, r->scale_x, r->scale_y);
    if (r->scale_px) {
        scale_draw(r);
        lv_canvas_set_buffer(r->scale, r->scale_px, r->scale_w, r->scale_h,
                             LV_COLOR_FORMAT_ARGB8888);
    }
    for (int i = 0; i < RADIO_KEYS; i++) {
        char n[4];
        snprintf(n, sizeof(n), "%d", i + 1);
        r->scale_num[i] = text(r->dial, aos_font_caption, C_AMBER_DIM,
                               r->scale_x + mark_x(r, i) - 20, r->scale_y - num_h - 2, 42,
                               LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(r->scale_num[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text(r->scale_num[i], n);
    }
    r->needle = plain(r->dial, r->scale_x, r->scale_y - 6, 5, r->scale_h + 4);
    lv_obj_set_style_bg_opa(r->needle, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(r->needle, lv_color_hex(C_NEEDLE), 0);
    lv_obj_set_style_radius(r->needle, 2, 0);
    lv_obj_set_style_shadow_width(r->needle, 12, 0);
    lv_obj_set_style_shadow_color(r->needle, lv_color_hex(C_NEEDLE), 0);
    lv_obj_set_style_shadow_opa(r->needle, LV_OPA_40, 0);
    r->needle_x = r->needle_to = (r->scale_x + 4) * 16;
    lv_obj_set_x(r->needle, r->needle_x / 16);
}

/* ---- the panel ---------------------------------------------------------------- */

static lv_obj_t *knob(radio_t *r, lv_obj_t *root, int cx, int cy, int d, const char *sym,
                      bool accent, lv_obj_t **icon)
{
    lv_obj_t *b = plain(root, cx - d / 2, cy - d / 2, d, d);
    grad(b, C_METAL_TOP, C_METAL_BOT);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(b, accent ? 4 : 3, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent ? 0xE0A040 : C_METAL_EDGE), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4A3A2C), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(0x2A2018), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, on_click, LV_EVENT_CLICKED, r);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, accent ? &aos_sym_72 : &aos_sym_44, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(accent ? C_AMBER : C_WARM), 0);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    if (icon) {
        *icon = l;
    }
    return b;
}

static void build_transport(radio_t *r, lv_obj_t *root, int x, int y, int w, int d_big)
{
    int d = d_big * 3 / 4;
    int cy = y + d_big / 2;
    int step = w / 5;
    int cx = x + step / 2;
    r->b_mute = knob(r, root, cx, cy, d, AOS_SYM_VOLUME_HIGH, false, &r->i_mute);
    r->b_prev = knob(r, root, cx + step, cy, d + 8, AOS_SYM_SKIP_PREVIOUS, false, NULL);
    r->b_play = knob(r, root, cx + 2 * step, cy, d_big, AOS_SYM_PLAY, true, &r->i_play);
    r->b_next = knob(r, root, cx + 3 * step, cy, d + 8, AOS_SYM_SKIP_NEXT, false, NULL);
    r->b_info = knob(r, root, cx + 4 * step, cy, d, AOS_SYM_INFORMATION_OUTLINE, false, NULL);
}

static void build_volume(radio_t *r, lv_obj_t *root, int x, int y, int w)
{
    /* a fader with a brass knob */
    int h = 64;
    lv_obj_t *spk = text(root, &aos_sym_44, C_AMBER_DIM, x, y + (h - 44) / 2 - 2, 48,
                         LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(spk, AOS_SYM_VOLUME_MEDIUM);
    r->vol = lv_slider_create(root);
    lv_obj_set_pos(r->vol, x + 72, y + h / 2 - 7);
    lv_obj_set_size(r->vol, w - 72 - 90, 14);
    lv_slider_set_range(r->vol, 0, 100);
    lv_obj_set_ext_click_area(r->vol, 26);
    lv_obj_set_style_bg_opa(r->vol, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r->vol, lv_color_hex(0x2A2118), LV_PART_MAIN);
    lv_obj_set_style_radius(r->vol, 7, LV_PART_MAIN);
    lv_obj_set_style_bg_color(r->vol, lv_color_hex(C_AMBER_DIM), LV_PART_INDICATOR);
    lv_obj_set_style_radius(r->vol, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(r->vol, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(r->vol, lv_color_hex(0xE8C27A), LV_PART_KNOB);
    lv_obj_set_style_bg_grad_color(r->vol, lv_color_hex(0x8A6A36), LV_PART_KNOB);
    lv_obj_set_style_bg_grad_dir(r->vol, LV_GRAD_DIR_VER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(r->vol, 14, LV_PART_KNOB);
    lv_obj_set_style_radius(r->vol, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(r->vol, 10, LV_PART_KNOB);
    lv_obj_set_style_shadow_opa(r->vol, LV_OPA_50, LV_PART_KNOB);
    lv_obj_add_event_cb(r->vol, on_volume, LV_EVENT_VALUE_CHANGED, r);
    r->vol_lbl = text(root, aos_font_body, C_AMBER_DIM, x + w - 76, y + (h - 34) / 2, 76,
                      LV_LABEL_LONG_MODE_CLIP);
    lv_obj_set_style_text_align(r->vol_lbl, LV_TEXT_ALIGN_RIGHT, 0);
}

static void build_keys(radio_t *r, lv_obj_t *root, int x, int y, int w, int kh)
{
    const int gap = 12;
    int kw = (w - 2 * gap) / 3;
    for (int i = 0; i < RADIO_KEYS; i++) {
        int col = i % 3, row = i / 3;
        lv_obj_t *k = plain(root, x + col * (kw + gap), y + row * (kh + gap), kw, kh);
        lv_obj_set_style_radius(k, 14, 0);
        lv_obj_set_style_border_width(k, 2, 0);
        lv_obj_set_style_border_color(k, lv_color_hex(0x8C7B5A), 0);
        lv_obj_set_style_shadow_width(k, 8, 0);
        lv_obj_set_style_shadow_offset_y(k, 4, 0);
        lv_obj_set_style_shadow_opa(k, LV_OPA_60, 0);
        lv_obj_add_flag(k, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(k, on_key, LV_EVENT_SHORT_CLICKED, r);
        lv_obj_add_event_cb(k, on_key, LV_EVENT_LONG_PRESSED, r);
        r->key[i] = k;
        r->key_led[i] = plain(k, 14, kh / 2 - 7, 14, 14);
        lv_obj_set_style_radius(r->key_led[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(r->key_led[i], LV_OPA_COVER, 0);
        r->key_lbl[i] = text(k, aos_font_small, C_IVORY_TXT, 38, 0, kw - 46,
                             LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(r->key_lbl[i], LV_ALIGN_LEFT_MID, 38, 0);
    }
}

static void build_list(radio_t *r, lv_obj_t *root, int x, int y, int w, int h)
{
    lv_obj_t *t = text(root, aos_font_body, C_AMBER, x + 6, y, w - 140, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(t, _("Todas las radios"));
    r->list_count = text(root, aos_font_small, C_AMBER_DIM, x + 260, y + 3, w - 266,
                         LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(r->list_count, LV_TEXT_ALIGN_RIGHT, 0);

    int ly = y + lv_font_get_line_height(aos_font_body) + 8;
    r->list = lv_obj_create(root);
    lv_obj_remove_style_all(r->list);
    lv_obj_set_pos(r->list, x, ly);
    lv_obj_set_size(r->list, w, h - (ly - y));
    lv_obj_set_flex_flow(r->list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r->list, 8, 0);
    lv_obj_set_scroll_dir(r->list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(r->list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(r->list, lv_color_hex(C_AMBER_DIM), LV_PART_SCROLLBAR);
    radio_ui_list(r);
}

void radio_ui_build(radio_t *r, lv_obj_t *root)
{
    r->root = root;
    lv_obj_update_layout(root);
    r->W = lv_obj_get_width(root);
    r->H = lv_obj_get_height(root);
    r->land = r->W > r->H;
    grad(root, C_BODY_TOP, C_BODY_BOT);

    if (!r->land) {
        int w = r->W - 2 * PAD;
        int y = 12;
        build_dial(r, root, PAD, y, w, 316);
        y += 316 + 12;
        build_transport(r, root, PAD, y, w, 120);
        y += 120 + 8;
        build_volume(r, root, PAD, y, w);
        y += 64 + 8;
        build_keys(r, root, PAD, y, w, 80);
        y += 3 * 80 + 2 * 12 + 20;
        build_list(r, root, PAD, y, w, r->H - y - 8);
    } else {
        int lw = r->W * 52 / 100;
        int y = 10;
        int dh = r->H - 10 - 12 - 116 - 12 - 64 - 24;
        build_dial(r, root, PAD, y, lw, dh);
        y += dh + 12;
        build_transport(r, root, PAD, y, lw, 116);
        y += 116 + 12;
        build_volume(r, root, PAD, y, lw);

        int rx = PAD + lw + 28, rw = r->W - rx - PAD;
        build_keys(r, root, rx, 10, rw, 78);
        int ly = 10 + 3 * 78 + 2 * 12 + 20;
        build_list(r, root, rx, ly, rw, r->H - ly - 8);
    }

    radio_ui_cover(r, NULL);
    r->s_active = -2;
    r->s_state = -1;
    r->s_vol = -1;
    r->s_muted = -1;
    r->s_onair = -1;
    r->s_cover_kind = -1;
    r->s_cover_slot = -1;
    r->s_row_on = -2;
    memset(r->s_station, 0, sizeof(r->s_station));
    memset(r->s_title, 0, sizeof(r->s_title));
    memset(r->s_artist, 0, sizeof(r->s_artist));
    memset(r->s_meta, 0, sizeof(r->s_meta));
    memset(r->s_status, 0, sizeof(r->s_status));
    r->anim = lv_timer_create(radio_ui_anim, 30, r);
}

void radio_ui_free(radio_t *r)
{
    if (r->anim) {
        lv_timer_delete(r->anim);
        r->anim = NULL;
    }
    art_big_free(r->cover_px);
    art_big_free(r->scale_px);
    art_big_free(r->info_px);
    r->cover_px = NULL;
    r->scale_px = NULL;
    r->info_px = NULL;
    r->info = NULL;
    r->list = NULL;
    memset(r->row, 0, sizeof(r->row));
    memset(r->row_air, 0, sizeof(r->row_air));
}

/* The keys: a lit LED and a key pushed in for the one on air; a dark key
 * for one with nothing on it. */
void radio_ui_keys(radio_t *r)
{
    for (int i = 0; i < RADIO_KEYS; i++) {
        lv_obj_t *k = r->key[i];
        bool used = r->st[i].url[0] != '\0';
        bool on = i == r->s_active;
        char label[64];
        if (used) {
            snprintf(label, sizeof(label), "%d  %s", i + 1, r->st[i].name);
            if (on) {
                grad(k, 0xC7B48C, 0xAE9A72);
            } else {
                grad(k, C_IVORY_TOP, C_IVORY_BOT);
            }
            lv_obj_set_style_text_color(r->key_lbl[i], lv_color_hex(C_IVORY_TXT), 0);
        } else {
            snprintf(label, sizeof(label), "%d  -", i + 1);
            grad(k, 0x2A2420, 0x1E1916);
            lv_obj_set_style_text_color(r->key_lbl[i], lv_color_hex(0x6A5A48), 0);
        }
        lv_obj_set_style_translate_y(k, on ? 3 : 0, 0);
        lv_obj_set_style_shadow_opa(k, on ? LV_OPA_20 : LV_OPA_60, 0);
        lv_label_set_text(r->key_lbl[i], label);
        lv_obj_set_style_bg_color(r->key_led[i], lv_color_hex(on ? C_LED_ON : (used ? C_LED_OFF : 0x2E2620)), 0);
        lv_obj_set_style_shadow_width(r->key_led[i], on ? 12 : 0, 0);
        lv_obj_set_style_shadow_color(r->key_led[i], lv_color_hex(C_LED_ON), 0);
        lv_obj_set_style_text_color(r->scale_num[i], lv_color_hex(on ? C_AMBER : C_AMBER_DIM), 0);
    }
}

/* The rows of the list: the name, and under it where it is from, what it
 * streams and which key has it. Built again only when the list or the keys
 * change, never per tick. */
void radio_ui_list(radio_t *r)
{
    if (!r->list) {
        return;
    }
    lv_obj_clean(r->list);
    memset(r->row, 0, sizeof(r->row));
    memset(r->row_air, 0, sizeof(r->row_air));
    r->s_row_on = -2;
    lv_obj_update_layout(r->list);
    int w = lv_obj_get_content_width(r->list);
    const int h = 84;

    for (int i = 0; i < r->lib.count && i < RADIO_LIB_MAX; i++) {
        const radio_lib_station_t *s = &r->lib.s[i];
        lv_obj_t *row = plain(r->list, 0, 0, w, h);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_ROW), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_ROW_ON), LV_STATE_PRESSED);
        lv_obj_set_style_border_color(row, lv_color_hex(C_AMBER), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row, LV_EVENT_CLICKED, r);
        r->row[i] = row;

        lv_obj_t *n = text(row, aos_font_body, C_WARM, 20, 10, w - 110, LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(n, s->name);

        char sub[64];
        int o = snprintf(sub, sizeof(sub), "%s%s%s", s->cc, s->cc[0] ? "  ·  " : "",
                         s->codec[0] ? s->codec : "MP3");
        if (s->kbps && o > 0 && o < (int)sizeof(sub)) {
            o += snprintf(sub + o, sizeof(sub) - (size_t)o, " %u kbps", (unsigned)s->kbps);
        }
        if (s->hls && o > 0 && o < (int)sizeof(sub)) {
            o += snprintf(sub + o, sizeof(sub) - (size_t)o, "  ·  HLS");
        }
        int key = radio_key_of_url(r, s->url);
        if (key >= 0 && o > 0 && o < (int)sizeof(sub)) {
            snprintf(sub + o, sizeof(sub) - (size_t)o, _("  ·  tecla %d"), key + 1);
        }
        lv_obj_t *d = text(row, aos_font_caption, C_AMBER_DIM, 20, 48, w - 110,
                           LV_LABEL_LONG_MODE_DOTS);
        lv_label_set_text(d, sub);

        lv_obj_t *air = text(row, &aos_sym_44, C_AMBER, w - 70, (h - 44) / 2 - 2, 50,
                             LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(air, AOS_SYM_VOLUME_HIGH);
        lv_obj_add_flag(air, LV_OBJ_FLAG_HIDDEN);
        r->row_air[i] = air;
    }

    char c[48];
    if (r->lib.from_card) {
        snprintf(c, sizeof(c), "%d", r->lib.count);
    } else {
        snprintf(c, sizeof(c), _("%d  ·  editalas en /radio"), r->lib.count);
    }
    lv_label_set_text(r->list_count, c);
}

void radio_ui_list_on(radio_t *r, int index)
{
    if (index == r->s_row_on) {
        return;
    }
    int old = r->s_row_on;
    if (old >= 0 && old < r->lib.count && r->row[old]) {
        lv_obj_set_style_border_width(r->row[old], 0, 0);
        lv_obj_set_style_bg_color(r->row[old], lv_color_hex(C_ROW), 0);
        lv_obj_add_flag(r->row_air[old], LV_OBJ_FLAG_HIDDEN);
    }
    if (index >= 0 && index < r->lib.count && r->row[index]) {
        lv_obj_set_style_border_width(r->row[index], 2, 0);
        lv_obj_set_style_bg_color(r->row[index], lv_color_hex(C_ROW_ON), 0);
        lv_obj_remove_flag(r->row_air[index], LV_OBJ_FLAG_HIDDEN);
        /* the first time it is seen, bring it into view */
        if (old == -2 || old == -1) {
            lv_obj_scroll_to_view(r->row[index], LV_ANIM_OFF);
        }
    }
    r->s_row_on = index;
}

/* The dial's cover from an ART_PX square, or the drawn one. */
void radio_ui_cover(radio_t *r, const uint16_t *px)
{
    if (!r->cover_px) {
        return;
    }
    const int S = r->cover_sz;
    if (px) {
        for (int y = 0; y < S; y++) {
            const uint16_t *row = px + (y * ART_PX / S) * ART_PX;
            for (int x = 0; x < S; x++) {
                r->cover_px[y * S + x] = row[x * ART_PX / S];
            }
        }
    } else {
        /* a speaker grille: warm dark cloth with rows of holes */
        for (int y = 0; y < S; y++) {
            for (int x = 0; x < S; x++) {
                int dx = (x % 12) - 6, dy = (y % 12) - 6;
                bool hole = dx * dx + dy * dy <= 10 && x > 10 && x < S - 10 &&
                            y > 10 && y < S - 10;
                r->cover_px[y * S + x] = hole ? 0x1061 : 0x3922;
            }
        }
    }
    lv_obj_invalidate(r->cover);
}

void radio_ui_needle_to(radio_t *r, int key)
{
    int x = key < 0 ? r->scale_x + 4 : r->scale_x + mark_x(r, key) - 1;
    r->needle_to = x * 16;
}

void radio_ui_anim(lv_timer_t *t)
{
    radio_t *r = lv_timer_get_user_data(t);
    if (r->closing || r->needle_x == r->needle_to) {
        return;
    }
    int d = r->needle_to - r->needle_x;
    int step = d / 5;
    if (step == 0) {
        step = d > 0 ? 1 : -1;
    }
    r->needle_x += step;
    lv_obj_set_x(r->needle, r->needle_x / 16);
}

/* ---- the info card -------------------------------------------------------------- */

void radio_ui_info_open(radio_t *r)
{
    if (r->info) {
        return;
    }
    r->info = plain(r->root, 0, 0, r->W, r->H);
    grad(r->info, 0x120C08, 0x050403);
    lv_obj_add_flag(r->info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r->info, on_click, LV_EVENT_CLICKED, r);

    int fx, fy, tx, ty, tw;
    if (r->land) {
        fx = PAD + 16;
        fy = (r->H - ART_PX - 8) / 2;
        tx = fx + ART_PX + 48;
        ty = fy;
        tw = r->W - tx - PAD;
    } else {
        fx = (r->W - ART_PX - 8) / 2;
        fy = 40;
        tx = PAD + 16;
        ty = fy + ART_PX + 32;
        tw = r->W - 2 * tx;
    }
    lv_obj_t *frame = plain(r->info, fx, fy, ART_PX + 8, ART_PX + 8);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(C_BRASS), 0);
    lv_obj_set_style_radius(frame, 14, 0);
    if (!r->info_px) {
        r->info_px = art_big_alloc(ART_PX * ART_PX * 2);   /* kept until destroy */
    }
    r->info_cover = lv_canvas_create(frame);
    lv_obj_remove_flag(r->info_cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(r->info_cover, 4, 4);
    if (r->info_px) {
        lv_canvas_set_buffer(r->info_cover, r->info_px, ART_PX, ART_PX, LV_COLOR_FORMAT_RGB565);
        /* what the dial shows, at its size: the art if there is art */
        if (r->s_cover_kind == 2 && r->art.have) {
            memcpy(r->info_px, r->art.px, ART_PX * ART_PX * 2);
        } else if (r->s_cover_kind == 1 && art_logo(r->s_cover_slot, r->info_px)) {
            /* decoded again: the dial kept only its own size */
        } else {
            const int S = r->cover_sz;
            for (int y = 0; y < ART_PX; y++) {
                for (int x = 0; x < ART_PX; x++) {
                    r->info_px[y * ART_PX + x] =
                        r->cover_px[(y * S / ART_PX) * S + x * S / ART_PX];
                }
            }
        }
    }
    r->info_title = text(r->info, aos_font_title, C_AMBER, tx, ty, tw, LV_LABEL_LONG_MODE_DOTS);
    if (!r->land) {
        lv_obj_set_style_text_align(r->info_title, LV_TEXT_ALIGN_CENTER, 0);
    }
    r->info_text = text(r->info, r->land ? aos_font_small : aos_font_body, C_WARM, tx, ty + 60,
                        tw, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_line_space(r->info_text, 6, 0);
    lv_obj_t *hint = text(r->info, aos_font_caption, C_AMBER_DIM, PAD, r->H - 40,
                          r->W - 2 * PAD, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(hint, _("Tocá para volver"));
    r->info_refresh = 0;
}

void radio_ui_info_close(radio_t *r)
{
    if (!r->info) {
        return;
    }
    lv_obj_add_flag(r->info, LV_OBJ_FLAG_HIDDEN);
    lv_obj_delete_async(r->info);           /* we are inside its own event */
    r->info = NULL;
    r->info_cover = NULL;
    r->info_title = NULL;
    r->info_text = NULL;
}
