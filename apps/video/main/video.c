/*
 * P4OS (from AmoledOS) - Video: the app, and the player's controls.
 * video.h has how a frame gets from the card to the screen.
 *
 * Two views on one root, both rebuilt when the screen turns (resize()), with
 * everything else kept: the list, and the player.
 *
 *   list    vd_list.c: one card per video, with its first frame.
 *   player  the picture, full screen, and two bars that come and go: on top
 *           the way back, the name and the numbers; below, the seek bar with
 *           the time and three buttons (10 s back, play/pause, 10 s on).
 *           A tap on the picture pauses and brings the bars up; another one
 *           plays on, and the bars go away after a moment. Dragging the seek
 *           bar shows the frames it passes. At the end the picture stays on
 *           its last frame with the bars up, and play starts it over.
 *
 * The bars are opaque on purpose: LVGL does not know the picture is there
 * (it is blitted past it), so nothing of LVGL's can be see-through over it.
 * While they are up, only the rows between them are blitted (vd_play.c).
 * When they go, LVGL paints the place they were, and the frame on screen is
 * blitted again right after (the display's REFR_READY), so the picture does
 * not wait for the next frame to be whole.
 *
 * The status bar shows over the list, and over the player only with the
 * bars up (it sits on the top bar's first strip). The app is FULLSCREEN so
 * the root is the whole screen and screen coordinates are the root's.
 *
 * It builds two ways from the same source:
 *   .so for the board      tools/build_apps.sh video
 *   simulator app          the simulator builds it (AOS_SIM_BUILTIN)
 */
#include "video.h"

#include "aos_app.h"
#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_internal.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VD_TICK_MS        4         /* how often the timer looks at the clock */
#define VD_UI_MS          250       /* the time label and the seek bar */
#define VD_CONTROLS_MS    3000      /* the bars stay up this long while it plays */
#define VD_RESUME_MS      1200      /* ...and this long after a tap plays on */
#define VD_SCRUB_MS       120       /* dragging the seek bar: a frame this often */
#define VD_TOP_BTN        88
#define VD_PLAY_BTN       104

/* A strip of film: a rounded card with the perforations along both edges. */
static const uint8_t VIDEO_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 0, 72, 56, 8, AIC_C_TEXT, 255),
    AIC_INTO,
    AIC_RECT(AIC_TOP_MID, -24,  5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_TOP_MID,  -8,  5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_TOP_MID,   8,  5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_TOP_MID,  24,  5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_BOTTOM_MID, -24, -5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_BOTTOM_MID,  -8, -5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_BOTTOM_MID,   8, -5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_BOTTOM_MID,  24, -5, 9, 7, 2, AIC_C_BG, 255),
    AIC_RECT(AIC_CENTER, 0, 0, 22, 22, AIC_CIRCLE, AIC_C_RED, 255),
    AIC_OUT,
    AIC_END
};

static vd_t s_vd;

/* What the UI last put on screen, so a label is only set when it changes
 * (every set is a redraw). Rebuilt views start from -1. */
static int s_ui_pos_s = -1, s_ui_slider = -1, s_ui_icon = -1;
static uint64_t s_ui_last;
static bool s_leave;

/* ---- controls ---------------------------------------------------------------- */

void vd_ui_controls(vd_t *v, bool up)
{
    if (up) {
        v->controls_until = aos_hal_uptime_ms() + VD_CONTROLS_MS;
    }
    if (v->controls == up) {
        return;
    }
    v->controls = up;
    if (v->top_bar) {
        lv_obj_set_flag(v->top_bar, LV_OBJ_FLAG_HIDDEN, !up);
        lv_obj_set_flag(v->bottom_bar, LV_OBJ_FLAG_HIDDEN, !up);
    }
    aos_ui_statusbar_set_visible(up || !v->in_player);
    if (!up) {
        v->reblit = true;           /* LVGL paints black where the bars were */
    }
}

void vd_ui_message(vd_t *v, const char *text)
{
    if (!v->message) {
        return;
    }
    if (text) {
        lv_label_set_text(v->message, text);
        lv_obj_remove_flag(v->message, LV_OBJ_FLAG_HIDDEN);
    } else if (!lv_obj_has_flag(v->message, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(v->message, LV_OBJ_FLAG_HIDDEN);
        v->reblit = true;
    }
}

void vd_ui_stats(vd_t *v, const char *text)
{
    if (v->stats && v->controls) {
        lv_label_set_text(v->stats, text);
    }
}

void vd_ui_update(vd_t *v)
{
    if (!v->slider || v->open_state != OPEN_OK) {
        return;
    }
    uint32_t t = vd_play_now_ms(v);
    if (v->ended) {
        t = vd_duration_ms(v);
    }
    if ((int)(t / 1000) != s_ui_pos_s) {
        s_ui_pos_s = (int)(t / 1000);
        char buf[16];
        vd_fmt_time(buf, sizeof buf, t);
        lv_label_set_text(v->pos_label, buf);
    }
    int pos = v->ended ? (int)v->frames - 1 : v->shown < 0 ? 0 : (int)v->shown;
    if (!lv_slider_is_dragged(v->slider) && pos != s_ui_slider) {
        s_ui_slider = pos;
        lv_slider_set_value(v->slider, pos, LV_ANIM_OFF);
    }
    int icon = v->paused ? 1 : 0;
    if (icon != s_ui_icon) {
        s_ui_icon = icon;
        lv_label_set_text(v->play_icon, icon ? AOS_SYM_PLAY : AOS_SYM_PAUSE);
    }
}

/* The worker has the file open: the rest of the player can start. */
void vd_ui_opened(vd_t *v)
{
    vd_ui_message(v, NULL);
    lv_slider_set_range(v->slider, 0, v->frames > 1 ? (int32_t)v->frames - 1 : 1);
    char buf[16];
    vd_fmt_time(buf, sizeof buf, vd_duration_ms(v));
    lv_label_set_text(v->len_label, buf);
    s_ui_pos_s = s_ui_slider = s_ui_icon = -1;

    uint64_t now = aos_hal_uptime_ms();
    v->stats_t0 = now;
    v->t_hold   = 0;
    v->t_anchor = now;
    v->has_audio = v->audio_path[0] != '\0';
    v->audio_wait = false;
    v->paused = true;               /* vd_play_pause() starts the clock and the sound */
    vd_play_pause(v, false);
    aos_hal_log("video", "%s: %ux%u, %u frames at %u.%02u fps, %u KB biggest, sound %s",
                v->files[v->cur].name, (unsigned)v->vw, (unsigned)v->vh, (unsigned)v->frames,
                (unsigned)(v->rate / v->scale), (unsigned)(v->rate * 100u / v->scale % 100),
                (unsigned)(v->avi.max_size / 1024), v->has_audio ? v->audio_path : "none");
    vd_ui_controls(v, true);
    vd_ui_update(v);
}

/* ---- events ---------------------------------------------------------------- */

static void touch_controls(vd_t *v)
{
    vd_ui_controls(v, true);
}

static void play_toggle(vd_t *v)
{
    if (!v->playing || v->open_state != OPEN_OK) {
        return;
    }
    bool pause = !v->paused;
    vd_play_pause(v, pause);
    vd_ui_controls(v, true);
    if (!pause) {
        v->controls_until = aos_hal_uptime_ms() + VD_RESUME_MS;
    }
    vd_ui_update(v);
}

static void tap_cb(lv_event_t *e)
{
    play_toggle(lv_event_get_user_data(e));
}

static void play_cb(lv_event_t *e)
{
    vd_t *v = lv_event_get_user_data(e);
    play_toggle(v);
    v->controls_until = aos_hal_uptime_ms() + VD_CONTROLS_MS;
}

static void jump_cb(lv_event_t *e)
{
    vd_t *v = lv_event_get_user_data(e);
    if (!v->playing || v->open_state != OPEN_OK) {
        return;
    }
    int32_t  step = (int32_t)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    int64_t  t    = (int64_t)(v->ended ? vd_duration_ms(v) : vd_play_now_ms(v)) + step;
    uint32_t f    = vd_frame_at(v, t < 0 ? 0 : (uint64_t)t);
    bool was_ended = v->ended;
    vd_play_seek(v, f, true);
    if (was_ended) {
        v->paused = true;           /* sought back from the end: paused there */
        v->ended  = false;
    }
    touch_controls(v);
    vd_ui_update(v);
}

static void slider_cb(lv_event_t *e)
{
    vd_t *v = lv_event_get_user_data(e);
    if (!v->playing || v->open_state != OPEN_OK) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t frame = (uint32_t)lv_slider_get_value(v->slider);
    uint64_t now   = aos_hal_uptime_ms();
    if (code == LV_EVENT_PRESSED) {
        v->scrub        = true;
        v->scrub_paused = v->paused && !v->ended;
        vd_play_pause(v, true);
    } else if (code == LV_EVENT_VALUE_CHANGED && v->scrub) {
        char buf[16];
        vd_fmt_time(buf, sizeof buf, vd_frame_ms(v, frame));
        lv_label_set_text(v->pos_label, buf);
        s_ui_pos_s = -1;
        if (now - v->scrub_last >= VD_SCRUB_MS) {
            v->scrub_last = now;
            vd_play_seek(v, frame, false);      /* the picture follows; the sound on release */
        }
    } else if ((code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) && v->scrub) {
        v->scrub = false;
        v->paused = true;
        vd_play_seek(v, frame, true);
        if (!v->scrub_paused) {
            vd_play_pause(v, false);
        }
        s_ui_slider = -1;
    }
    touch_controls(v);
}

static void back_cb(lv_event_t *e)
{
    (void)e;
    s_leave = true;                 /* on the next tick, not inside the event */
}

/* ---- the player's screen --------------------------------------------------- */

static lv_obj_t *bar_create(lv_obj_t *parent, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, 0, y);
    lv_obj_set_size(bar, w, h);
    lv_obj_set_style_bg_color(bar, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);    /* a tap on a bar is not a tap on the picture */
    return bar;
}

static lv_obj_t *round_button(lv_obj_t *parent, int32_t w, int32_t h, lv_color_t bg,
                              lv_event_cb_t cb, void *arg)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, AOS_C_DIM, LV_STATE_PRESSED);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, arg);
    return b;
}

static lv_obj_t *jump_button(vd_t *v, lv_obj_t *parent, const char *text, int32_t step)
{
    lv_obj_t *b = round_button(parent, 136, 80, AOS_C_CARD, jump_cb, v);
    lv_obj_set_user_data(b, (void *)(intptr_t)step);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return b;
}

static void player_build(vd_t *v)
{
    const aos_geo_t *geo = aos_ui_geo();
    int32_t W = v->scr_w, H = v->scr_h;
    v->top_h    = geo->bar_h + VD_TOP_BTN + 8;
    v->bottom_h = v->land ? 36 + VD_PLAY_BTN + 16 : 36 + VD_PLAY_BTN + 112;

    lv_obj_t *pv = lv_obj_create(v->root);
    v->play_view = pv;
    lv_obj_remove_style_all(pv);
    lv_obj_set_size(pv, W, H);
    lv_obj_set_style_bg_color(pv, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(pv, LV_OPA_COVER, 0);
    lv_obj_remove_flag(pv, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(pv, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pv, tap_cb, LV_EVENT_CLICKED, v);

    /* Under the bars: the LVGL path's picture. */
    v->frame_img = lv_image_create(pv);
    lv_obj_add_flag(v->frame_img, LV_OBJ_FLAG_HIDDEN);
    aos_make_decorative(v->frame_img);

    v->message = aos_label(pv, "", aos_font_body, AOS_C_DIM);
    lv_obj_set_width(v->message, W - 80);
    lv_label_set_long_mode(v->message, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(v->message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(v->message);
    lv_obj_add_flag(v->message, LV_OBJ_FLAG_HIDDEN);
    aos_make_decorative(v->message);

    /* ---- top: back, name, numbers (clear of the status bar's strip) ---- */
    v->top_bar = bar_create(pv, 0, W, v->top_h);
    int32_t cy = geo->bar_h + (v->top_h - geo->bar_h) / 2;
    lv_obj_t *back = round_button(v->top_bar, VD_TOP_BTN, VD_TOP_BTN, AOS_C_CARD, back_cb, v);
    lv_obj_set_pos(back, 16, cy - VD_TOP_BTN / 2);
    lv_obj_t *chev = aos_label(back, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(chev);
    aos_make_decorative(chev);

    int32_t tx = 16 + VD_TOP_BTN + 20;
    v->title = aos_label(v->top_bar, "", aos_font_body, AOS_C_TEXT);
    lv_label_set_long_mode(v->title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(v->title, W - tx - 24);
    lv_obj_set_pos(v->title, tx, cy - 36);
    aos_make_decorative(v->title);
    if (v->cur >= 0 && v->cur < v->count) {
        const char *name = v->files[v->cur].name;
        const char *dot  = strrchr(name, '.');
        lv_label_set_text_fmt(v->title, "%.*s", dot ? (int)(dot - name) : (int)strlen(name), name);
    }
    v->stats = aos_label(v->top_bar, "", aos_font_tiny, AOS_C_DIM);
    lv_label_set_long_mode(v->stats, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(v->stats, W - tx - 24);
    lv_obj_set_pos(v->stats, tx, cy + 8);
    aos_make_decorative(v->stats);

    /* ---- bottom: seek bar, time, buttons (clear of the home strip) ---- */
    v->bottom_bar = bar_create(pv, H - v->bottom_h, W, v->bottom_h);
    lv_obj_t *bb = v->bottom_bar;
    v->slider = lv_slider_create(bb);
    lv_obj_remove_style_all(v->slider);
    lv_obj_set_style_bg_color(v->slider, AOS_C_CARD2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(v->slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(v->slider, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(v->slider, AOS_C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(v->slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(v->slider, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(v->slider, AOS_C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(v->slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(v->slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(v->slider, 12, LV_PART_KNOB);
    lv_obj_set_ext_click_area(v->slider, 28);
    lv_slider_set_range(v->slider, 0, 1);
    lv_obj_add_event_cb(v->slider, slider_cb, LV_EVENT_PRESSED, v);
    lv_obj_add_event_cb(v->slider, slider_cb, LV_EVENT_VALUE_CHANGED, v);
    lv_obj_add_event_cb(v->slider, slider_cb, LV_EVENT_RELEASED, v);
    lv_obj_add_event_cb(v->slider, slider_cb, LV_EVENT_PRESS_LOST, v);

    v->pos_label = aos_label(bb, "0:00", aos_font_small, AOS_C_TEXT);
    v->len_label = aos_label(bb, "0:00", aos_font_small, AOS_C_DIM);
    aos_make_decorative(v->pos_label);
    aos_make_decorative(v->len_label);

    lv_obj_t *rew  = jump_button(v, bb, _("-10 s"), -10000);
    lv_obj_t *play = round_button(bb, VD_PLAY_BTN, VD_PLAY_BTN, AOS_C_TEXT, play_cb, v);
    lv_obj_t *fwd  = jump_button(v, bb, _("+10 s"), 10000);
    v->play_icon = aos_label(play, AOS_SYM_PAUSE, &aos_sym_44, AOS_C_BG);
    lv_obj_center(v->play_icon);
    aos_make_decorative(v->play_icon);

    if (!v->land) {
        /* Upright: the seek bar across, the times under its ends, the
         * three buttons in a row below. */
        lv_obj_set_size(v->slider, W - 96, 8);
        lv_obj_set_pos(v->slider, 48, 36);
        lv_obj_set_pos(v->pos_label, 48, 64);
        lv_obj_align(v->len_label, LV_ALIGN_TOP_RIGHT, -48, 64);
        int32_t by = 112 + VD_PLAY_BTN / 2;
        lv_obj_align(play, LV_ALIGN_TOP_MID, 0, by - VD_PLAY_BTN / 2);
        lv_obj_align(rew, LV_ALIGN_TOP_MID, -(VD_PLAY_BTN / 2 + 40 + 68), by - 40);
        lv_obj_align(fwd, LV_ALIGN_TOP_MID, VD_PLAY_BTN / 2 + 40 + 68, by - 40);
    } else {
        /* Lying down, one row: the buttons, then the time and the bar. */
        int32_t by = 8 + VD_PLAY_BTN / 2;
        lv_obj_set_pos(rew, 32, by - 40);
        lv_obj_set_pos(play, 32 + 136 + 20, 8);
        lv_obj_set_pos(fwd, 32 + 136 + 20 + VD_PLAY_BTN + 20, by - 40);
        int32_t x0 = 32 + 136 + 20 + VD_PLAY_BTN + 20 + 136 + 40;
        lv_obj_set_pos(v->pos_label, x0, by - 14);
        lv_obj_align(v->len_label, LV_ALIGN_TOP_RIGHT, -40, by - 14);
        int32_t sx = x0 + 100;
        lv_obj_set_size(v->slider, W - sx - 40 - 100, 8);
        lv_obj_set_pos(v->slider, sx, by - 4);
    }
    lv_obj_set_flag(v->top_bar, LV_OBJ_FLAG_HIDDEN, !v->controls);
    lv_obj_set_flag(v->bottom_bar, LV_OBJ_FLAG_HIDDEN, !v->controls);
}

/* Both views, for the screen as it is now. */
static void build(vd_t *v)
{
    v->scr_w = AOS_SCREEN_W;
    v->scr_h = AOS_SCREEN_H;
    v->land  = v->scr_w > v->scr_h;
    lv_obj_clean(v->root);
    lv_obj_set_style_bg_color(v->root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(v->root, LV_OPA_COVER, 0);
    v->frame_img = NULL;
    vd_list_build(v);
    player_build(v);
    lv_obj_set_flag(v->list_view, LV_OBJ_FLAG_HIDDEN, v->in_player);
    lv_obj_set_flag(v->play_view, LV_OBJ_FLAG_HIDDEN, !v->in_player);
    s_ui_pos_s = s_ui_slider = s_ui_icon = -1;
    if (v->in_player && v->open_state == OPEN_OK) {
        lv_slider_set_range(v->slider, 0, v->frames > 1 ? (int32_t)v->frames - 1 : 1);
        char buf[16];
        vd_fmt_time(buf, sizeof buf, vd_duration_ms(v));
        lv_label_set_text(v->len_label, buf);
        vd_ui_update(v);
    }
    v->reblit = true;
}

void vd_ui_open_file(vd_t *v, int index)
{
    aos_hal_worker_stop();          /* the list's worker: the player needs it */
    v->cur = index;
    snprintf(v->path, sizeof v->path, "%s/%s", v->dir, v->files[index].name);
    vd_audio_path(v, index, v->audio_path, sizeof v->audio_path);

    v->in_player  = true;
    v->controls   = false;
    v->ended      = false;
    v->scrub      = false;
    v->lvgl_mode  = false;
    v->blit_fails = 0;
    v->gen        = 1;
    v->req_frame  = 0;
    __sync_synchronize();
    v->req_gen    = 1;
    v->skip_gen   = 0;
    v->skip_to    = 0;
    v->frames     = 0;
    v->open_state = OPEN_BUSY;
    v->open_seen  = false;
    v->playing    = true;
    v->paused     = false;
    build(v);                       /* the player's title is the new file's */
    vd_ui_controls(v, true);
    vd_ui_message(v, _("Abriendo..."));
    if (!vd_play_start(v)) {
        v->playing = false;
        vd_ui_message(v, _("No hay lugar para la tarea de video"));
    }
}

static void leave_player(vd_t *v)
{
    vd_play_stop(v);
    v->in_player  = false;
    v->open_state = OPEN_NONE;
    lv_obj_add_flag(v->play_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(v->list_view, LV_OBJ_FLAG_HIDDEN);
    aos_ui_statusbar_set_visible(true);
    vd_list_thumbs_start(v);
}

/* ---- the timer and the display's hook --------------------------------------- */

static void open_result(vd_t *v)
{
    if (v->open_state == OPEN_OK) {
        vd_ui_opened(v);
        return;
    }
    const char *why = v->open_state == OPEN_NOMEM  ? _("No hay memoria para este video")
                    : v->open_state == OPEN_NOTASK ? _("No hay lugar para la tarea de video")
                    :                                _("No es un AVI de MJPEG que se pueda leer");
    vd_play_stop(v);
    v->playing = false;
    vd_ui_message(v, why);
}

static void timer_cb(lv_timer_t *t)
{
    vd_t *v = lv_timer_get_user_data(t);
    if (v->bar_again) {
        v->bar_again = false;
        aos_ui_statusbar_set_visible(!v->in_player || v->controls);
    }
    if (s_leave) {
        s_leave = false;
        if (v->in_player) {
            leave_player(v);
        }
        return;
    }
    if (!v->in_player) {
        vd_list_tick(v);
        return;
    }
    if (v->playing && v->open_state != OPEN_BUSY && !v->open_seen) {
        v->open_seen = true;        /* the worker is done opening, one way or the other */
        open_result(v);
    }
    vd_play_tick(v);
    uint64_t now = aos_hal_uptime_ms();
    if (v->controls && v->playing && !v->paused && !v->scrub && now >= v->controls_until) {
        vd_ui_controls(v, false);
    }
    if (now - s_ui_last >= VD_UI_MS) {
        s_ui_last = now;
        vd_ui_update(v);
    }
}

/* Right after LVGL has drawn: if what it drew may have covered the picture,
 * the frame on screen goes back up at once. */
static void refr_cb(lv_event_t *e)
{
    vd_t *v = lv_event_get_user_data(e);
    if (v->reblit && v->in_player && !v->overlay) {
        v->reblit = false;
        vd_play_blit_current(v);
    }
}

/* ---- lifecycle ------------------------------------------------------------- */

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    vd_t *v = inst;
    if (v->in_player) {
        s_leave = true;             /* on the next timer tick, not in the gesture */
        return true;
    }
    return false;
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    vd_t *v = inst;
    v->hidden = true;
    v->ov_paused = false;           /* back in front it stays paused, bars up */
    if (v->playing && v->open_state == OPEN_OK && !v->paused) {
        vd_play_pause(v, true);     /* and nothing is blitted over whatever is in front */
        vd_ui_controls(v, true);
    }
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    vd_t *v = inst;
    v->hidden    = false;
    v->reblit    = true;
    v->bar_again = true;            /* the runtime sets the status bar after show() */
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    vd_t *v = inst;
    v->root = root;
    build(v);
    if (v->in_player) {
        vd_ui_controls(v, true);
        if (v->open_state == OPEN_OK) {
            v->show_next = true;    /* the frame, turned, once the worker has one */
        } else if (!v->playing) {
            vd_ui_message(v, _("No es un AVI de MJPEG que se pueda leer"));
        }
    }
    v->bar_again = true;
    return true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    vd_t *v = &s_vd;
    memset(v, 0, sizeof *v);
    v->root     = root;
    v->cur      = -1;
    v->cur_slot = -1;
    v->shown    = -1;
    v->eof_gen  = UINT32_MAX;
    s_leave     = false;
    vd_list_scan(v);
    build(v);
    v->timer = lv_timer_create(timer_cb, VD_TICK_MS, v);
    lv_display_add_event_cb(lv_display_get_default(), refr_cb, LV_EVENT_REFR_READY, v);
    vd_list_thumbs_start(v);
    v->bar_again = true;
    return v;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    vd_t *v = inst;
    if (v->in_player) {
        vd_play_stop(v);            /* joins the worker before anything is freed */
    } else {
        aos_hal_worker_stop();
    }
    lv_display_remove_event_cb_with_user_data(lv_display_get_default(), refr_cb, v);
    if (v->timer) {
        lv_timer_delete(v->timer);
        v->timer = NULL;
    }
    /* The rows' images point at the thumbnails: they go before them. */
    lv_obj_clean(v->root);
    vd_list_free(v);
}

static bool video_init(aos_app_t *app)
{
    app->desc.id       = "aos.video";
    app->desc.name     = "Video";
    app->desc.icon     = LV_SYMBOL_VIDEO;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xFF453A;
    app->desc.color_b  = 0x8E1A12;
    app->desc.order    = 126;
    app->desc.flags    = AOS_APP_FLAG_KEEP_AWAKE | AOS_APP_FLAG_FULLSCREEN;
    aos_icon_set_ops(app, VIDEO_ICON, sizeof VIDEO_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->hide    = hide;
    app->show    = show;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(video_init);
