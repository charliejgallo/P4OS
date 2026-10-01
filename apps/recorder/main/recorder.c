/*
 * P4OS - Recorder (from AmoledOS)
 *
 * Records from the microphone to WAV on the microSD, shows the time and the
 * waveform live, and lets you listen to, seek in and delete what was
 * recorded.
 *
 * The capture does not live here: the HAL does it in its own task
 * (aos_hal_rec_*) and it goes on running even if you leave the app, or if the
 * app is destroyed. This app is the face: it asks to start, draws what the HAL
 * tells it and manages the files.
 *
 * Two views (the watch had three: on the 5" screen the list of recordings is
 * part of the main view, beside the recorder in landscape and under it
 * upright):
 *   main      the recorder + every recording
 *   detail    one recording: whole waveform (tap to seek), play, delete
 *
 * On rotation resize() rebuilds the screen and keeps everything else: the
 * view, the recording open in the detail, the playback and the last seconds
 * of the live waveform.
 *
 * It builds two ways from the same source:
 *   .so for the board      cd apps/recorder && idf.py so
 *   simulator app          the simulator builds it (AOS_SIM_BUILTIN)
 */
#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_ui.h"
#include "aos_theme.h"
#include "aos_fonts.h"
#include "aos_sys_glyphs.h"

#include "rec_files.h"
#include "rec_wave.h"

#include <stdio.h>
#include <string.h>

#define LIVE_BARS_MAX   160
#define DETAIL_BARS_MAX 160
#define BAR_PITCH       7           /* px per bar of the live waveform */
#define ROW_H           AOS_UI_ROW_H
#define DELETE_ARM_MS   3000        /* how long the "are you sure?" waits before giving up */

typedef enum {
    VIEW_MAIN = 0,
    VIEW_DETAIL,
} view_t;

/* The screen: rebuilt on every resize. */
typedef struct {
    int32_t   W, H;
    bool      land;

    lv_obj_t *main_view;
    lv_obj_t *pill;
    lv_obj_t *pill_dot;
    lv_obj_t *pill_text;
    lv_obj_t *time_label;
    lv_obj_t *status_label;
    lv_obj_t *info_label;
    rec_wave_t *live_wave;
    int       live_bars;
    lv_obj_t *rec_btn;
    lv_obj_t *stop_btn;
    lv_obj_t *pause_btn;
    lv_obj_t *pause_icon;
    lv_obj_t *list_body;
    lv_obj_t *list_count;
    lv_obj_t *empty_hint;

    lv_obj_t *detail_view;
    lv_obj_t *detail_title;
    lv_obj_t *detail_meta;
    lv_obj_t *detail_play_icon;
    lv_obj_t *detail_position;
    lv_obj_t *detail_total;
    lv_obj_t *detail_del;
    lv_obj_t *detail_del_text;
    rec_wave_t *detail_wave;
    int       detail_bars;
} rec_ui_t;

typedef struct {
    lv_obj_t *root;
    rec_ui_t  ui;

    view_t    view;
    int       detail_index;
    bool      delete_armed;
    uint32_t  delete_armed_ms;
    bool      playing;

    rec_file_t files[REC_MAX_FILES];
    int        file_count;

    /* The last seconds of the live waveform, right-aligned: what a new
     * screen starts from after a turn. */
    uint8_t   live_hist[LIVE_BARS_MAX];

    lv_timer_t *timer;
    uint32_t   last_shown_s;
    uint32_t   last_info_ms;
    uint32_t   blink_ms;
    bool       blink_on;
    bool       button_toggle;   /* the physical button asks to change state */
    bool       ui_recording;    /* what the screen is showing */
    bool       ui_paused;
} rec_ctx_t;

static rec_ctx_t *s_ctx;
#define UI (s_ctx->ui)

/* -------------------------------------------------------------------------- */

static void format_time(char *out, size_t len, uint32_t seconds)
{
    if (seconds >= 3600) {
        snprintf(out, len, "%u:%02u:%02u", (unsigned)(seconds / 3600),
                 (unsigned)(seconds / 60 % 60), (unsigned)(seconds % 60));
    } else {
        snprintf(out, len, "%02u:%02u", (unsigned)(seconds / 60),
                 (unsigned)(seconds % 60));
    }
}

static bool is_recording(void)
{
    aos_rec_status_t status;
    return aos_hal_rec_status(&status) && status.state != AOS_REC_IDLE;
}

static lv_obj_t *plain(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *o = plain(parent, x, y, w, h);
    lv_obj_set_style_bg_color(o, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, AOS_UI_RADIUS, 0);
    return o;
}

/* One-line label in a fixed box. */
static lv_obj_t *text(lv_obj_t *parent, const char *txt, const lv_font_t *font,
                      lv_color_t color, int32_t x, int32_t y, int32_t w,
                      lv_text_align_t align)
{
    lv_obj_t *l = aos_label(parent, txt, font, color);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(l, w, lv_font_get_line_height(font));
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *glyph(lv_obj_t *parent, const char *sym, const lv_font_t *font,
                       lv_color_t color)
{
    lv_obj_t *l = aos_label(parent, sym, font, color);
    lv_obj_center(l);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* -------------------------------------------------------------------------- */
/* The list                                                                    */
/* -------------------------------------------------------------------------- */

static void open_detail(int index);

static void row_click_cb(lv_event_t *event)
{
    open_detail((int)(intptr_t)lv_event_get_user_data(event));
}

static void rescan(void)
{
    s_ctx->file_count = rec_files_scan(s_ctx->files, REC_MAX_FILES);
}

/* A row: the newest one with the green mark, the name, when and how big,
 * and the duration on the right. Rows are rebuilt only when the list
 * changes (a recording closed, one deleted), never per frame. */
static void row_build(lv_obj_t *parent, int index, int32_t w)
{
    const rec_file_t *file = &s_ctx->files[index];

    lv_obj_t *row = plain(parent, 0, 0, w, ROW_H);
    lv_obj_set_style_bg_color(row, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(row, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_radius(row, 22, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)index);

    lv_obj_t *ico = plain(row, 16, (ROW_H - 60) / 2, 60, 60);
    lv_obj_set_style_radius(ico, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ico, index == 0 ? AOS_C_GREEN : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(ico, index == 0 ? LV_OPA_30 : LV_OPA_COVER, 0);
    glyph(ico, AOS_SYM_MICROPHONE, &aos_sym_28, index == 0 ? AOS_C_GREEN : AOS_C_DIM);

    char pretty[REC_NAME_LEN];
    rec_files_pretty(pretty, sizeof(pretty), file->name);
    int32_t tx = 92, tw = w - tx - 150;
    text(row, pretty, aos_font_body, AOS_C_TEXT, tx, 14, tw, LV_TEXT_ALIGN_LEFT);

    char when[32], size[16], detail[64];
    rec_files_when(when, sizeof(when), file->mtime);
    rec_files_size(size, sizeof(size), file->bytes);
    snprintf(detail, sizeof(detail), "%s  ·  %s", when, size);
    text(row, detail, aos_font_caption, AOS_C_DIM, tx, 54, tw, LV_TEXT_ALIGN_LEFT);

    char duration[16];
    format_time(duration, sizeof(duration), file->seconds);
    text(row, duration, aos_font_body, AOS_C_DIM, w - 150, (ROW_H - 34) / 2, 100,
         LV_TEXT_ALIGN_RIGHT);
    lv_obj_t *chev = aos_label(row, AOS_SYM_CHEVRON_RIGHT, &aos_sym_28, AOS_C_DIM);
    lv_obj_align(chev, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_remove_flag(chev, LV_OBJ_FLAG_CLICKABLE);
}

static void refresh_list(void)
{
    if (!UI.list_body) {
        return;
    }
    lv_obj_clean(UI.list_body);
    lv_obj_update_layout(UI.list_body);
    int32_t w = lv_obj_get_content_width(UI.list_body);
    for (int i = 0; i < s_ctx->file_count; i++) {
        row_build(UI.list_body, i, w);
    }

    char count[48];
    if (s_ctx->file_count == 0) {
        count[0] = '\0';
    } else {
        snprintf(count, sizeof(count), "%d", s_ctx->file_count);
    }
    lv_label_set_text(UI.list_count, count);
    if (s_ctx->file_count == 0) {
        lv_obj_remove_flag(UI.empty_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(UI.empty_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

/* -------------------------------------------------------------------------- */
/* Navigation                                                                  */
/* -------------------------------------------------------------------------- */

static void stop_playback(void)
{
    if (s_ctx->playing) {
        aos_hal_player_stop();
        s_ctx->playing = false;
    }
}

static void show_view(view_t view)
{
    s_ctx->view = view;
    if (view == VIEW_MAIN) {
        lv_obj_remove_flag(UI.main_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(UI.detail_view, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(UI.main_view, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(UI.detail_view, LV_OBJ_FLAG_HIDDEN);
    }
}

static void detail_disarm(void)
{
    s_ctx->delete_armed = false;
    lv_label_set_text(UI.detail_del_text, _("Borrar"));
    lv_obj_set_style_bg_color(UI.detail_del, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(UI.detail_del_text, AOS_C_RED, 0);
}

static void detail_show_stopped(void)
{
    const rec_file_t *file = &s_ctx->files[s_ctx->detail_index];
    char duration[16];
    format_time(duration, sizeof(duration), file->seconds);
    lv_label_set_text(UI.detail_play_icon, AOS_SYM_PLAY);
    lv_label_set_text(UI.detail_position, "00:00");
    lv_label_set_text(UI.detail_total, duration);
    rec_wave_set_progress(UI.detail_wave, -1);
}

static void refresh_detail(void)
{
    if (s_ctx->detail_index < 0 || s_ctx->detail_index >= s_ctx->file_count) {
        return;
    }
    const rec_file_t *file = &s_ctx->files[s_ctx->detail_index];

    char pretty[REC_NAME_LEN];
    rec_files_pretty(pretty, sizeof(pretty), file->name);
    lv_label_set_text(UI.detail_title, pretty);

    char when[32], size[16], meta[160];   /* the name alone may take 63 */
    rec_files_when(when, sizeof(when), file->mtime);
    rec_files_size(size, sizeof(size), file->bytes);
    snprintf(meta, sizeof(meta), "%s  ·  %s  ·  %u kHz  ·  %s", when, size,
             (unsigned)(file->sample_rate / 1000), file->name);
    lv_label_set_text(UI.detail_meta, meta);

    uint8_t envelope[DETAIL_BARS_MAX];
    if (rec_files_envelope(file->name, envelope, UI.detail_bars)) {
        rec_wave_set(UI.detail_wave, envelope, UI.detail_bars);
    } else {
        rec_wave_clear(UI.detail_wave);
    }
    detail_show_stopped();
    detail_disarm();
}

static void open_detail(int index)
{
    if (index < 0 || index >= s_ctx->file_count) {
        return;
    }
    stop_playback();
    s_ctx->detail_index = index;
    refresh_detail();
    show_view(VIEW_DETAIL);
}

/* -------------------------------------------------------------------------- */
/* Recording                                                                   */
/* -------------------------------------------------------------------------- */

static void set_recording_ui(bool recording, bool paused)
{
    s_ctx->ui_recording = recording;
    s_ctx->ui_paused    = paused;

    if (recording) {
        lv_obj_add_flag(UI.rec_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(UI.stop_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(UI.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(UI.pause_icon, paused ? AOS_SYM_MICROPHONE : AOS_SYM_PAUSE);
        lv_label_set_text(UI.status_label, paused ? _("en pausa: el archivo sigue abierto")
                                                  : _("grabando"));
        lv_label_set_text(UI.pill_text, paused ? _("PAUSA") : "REC");
        lv_obj_set_style_bg_color(UI.pill, paused ? AOS_C_ORANGE : AOS_C_RED, 0);
        lv_obj_set_style_bg_opa(UI.pill_dot, LV_OPA_COVER, 0);
        rec_wave_set_color(UI.live_wave, paused ? AOS_C_ORANGE : AOS_C_RED);
    } else {
        lv_obj_remove_flag(UI.rec_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(UI.stop_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(UI.pause_btn, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(UI.status_label, _("tocá el botón rojo para grabar"));
        lv_label_set_text(UI.pill_text, _("LISTO"));
        lv_obj_set_style_bg_color(UI.pill, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(UI.pill_dot, LV_OPA_TRANSP, 0);
        rec_wave_set_color(UI.live_wave, AOS_C_CARD2);
    }
    s_ctx->last_info_ms = 0;
}

/* One line with what is being written and how much room is left. */
static void refresh_info(const aos_rec_status_t *status)
{
    char line[128], size[16] = "", left[48] = "";
    uint32_t rate = status && status->sample_rate ? status->sample_rate : AOS_REC_RATE_HZ;
    int channels = status && status->channels ? status->channels : 1;

    uint64_t total = 0, free_b = 0;
    if (aos_hal_sd_usage(&total, &free_b) && free_b) {
        uint64_t per_s = (uint64_t)rate * 2u * (uint64_t)channels;
        uint64_t secs = free_b / per_s;
        if (secs >= 3600) {
            snprintf(left, sizeof(left), _("quedan %u h"), (unsigned)(secs / 3600));
        } else {
            snprintf(left, sizeof(left), _("quedan %u min"), (unsigned)(secs / 60));
        }
    } else if (!aos_hal_sd_present()) {
        snprintf(left, sizeof(left), "%s", _("sin tarjeta"));
    }
    if (status && status->state != AOS_REC_IDLE) {
        rec_files_size(size, sizeof(size), status->bytes);
    }

    int n = snprintf(line, sizeof(line), "WAV  ·  %u kHz  ·  %s",
                     (unsigned)(rate / 1000), channels == 2 ? _("estéreo") : _("mono"));
    if (size[0] && n > 0 && n < (int)sizeof(line)) {
        n += snprintf(line + n, sizeof(line) - (size_t)n, "  ·  %s", size);
    }
    if (left[0] && n > 0 && n < (int)sizeof(line)) {
        snprintf(line + n, sizeof(line) - (size_t)n, "  ·  %s", left);
    }
    lv_label_set_text(UI.info_label, line);
}

static void live_push(const uint8_t *peaks, int got)
{
    if (got >= LIVE_BARS_MAX) {
        memcpy(s_ctx->live_hist, peaks + got - LIVE_BARS_MAX, LIVE_BARS_MAX);
    } else {
        memmove(s_ctx->live_hist, s_ctx->live_hist + got, (size_t)(LIVE_BARS_MAX - got));
        memcpy(s_ctx->live_hist + LIVE_BARS_MAX - got, peaks, (size_t)got);
    }
    rec_wave_push_many(UI.live_wave, peaks, got);
}

static void start_recording(void)
{
    if (!aos_hal_sd_present()) {
        aos_ui_toast(_("No hay tarjeta"), 1600);
        return;
    }
    stop_playback();

    char name[REC_NAME_LEN];
    char path[REC_PATH_LEN];
    rec_files_next_name(name, sizeof(name));
    rec_files_path(path, sizeof(path), name);

    if (!aos_hal_rec_start(path, 0)) {
        aos_ui_toast(_("No se pudo grabar"), 1800);
        return;
    }

    memset(s_ctx->live_hist, 0, sizeof(s_ctx->live_hist));
    rec_wave_clear(UI.live_wave);
    s_ctx->last_shown_s = 0;
    lv_label_set_text(UI.time_label, "00:00");
    set_recording_ui(true, false);

    /* No beep on starting, on purpose: on a board where the speaker and the
     * microphone share a codec, the note would tear the capture's input
     * channel away (and it would end up in the file). On stopping, yes,
     * because by then the microphone has been closed. */
}

static void stop_recording(void)
{
    bool saved = aos_hal_rec_stop();
    aos_hal_beep(700, 60);
    set_recording_ui(false, false);

    rescan();
    refresh_list();

    if (saved && s_ctx->file_count > 0) {
        char pretty[REC_NAME_LEN], message[96];
        rec_files_pretty(pretty, sizeof(pretty), s_ctx->files[0].name);
        snprintf(message, sizeof(message), "%s\n%s", _("Guardado"), pretty);
        aos_ui_toast(message, 1500);
    }
}

static void toggle_recording(void)
{
    if (is_recording()) {
        stop_recording();
    } else {
        start_recording();
    }
}

/* -------------------------------------------------------------------------- */
/* Events                                                                      */
/* -------------------------------------------------------------------------- */

static void rec_cb(lv_event_t *event)
{
    (void)event;
    start_recording();
}

static void stop_cb(lv_event_t *event)
{
    (void)event;
    stop_recording();
}

static void pause_cb(lv_event_t *event)
{
    (void)event;
    aos_rec_status_t status;
    if (!aos_hal_rec_status(&status)) {
        return;
    }
    if (status.state == AOS_REC_PAUSED) {
        aos_hal_rec_resume();
        set_recording_ui(true, false);
    } else if (status.state == AOS_REC_RECORDING) {
        aos_hal_rec_pause();
        set_recording_ui(true, true);
    }
}

static bool start_playback(uint32_t from_ms)
{
    char path[REC_PATH_LEN];
    rec_files_path(path, sizeof(path), s_ctx->files[s_ctx->detail_index].name);
    if (!aos_hal_player_play(path)) {
        aos_ui_toast(_("No se pudo reproducir"), 1600);
        return false;
    }
    if (from_ms) {
        aos_hal_player_seek(from_ms);
    }
    s_ctx->playing = true;
    lv_label_set_text(UI.detail_play_icon, AOS_SYM_STOP);
    return true;
}

static void play_cb(lv_event_t *event)
{
    (void)event;
    if (s_ctx->playing) {
        stop_playback();
        detail_show_stopped();
        return;
    }
    if (is_recording()) {
        aos_ui_toast(_("Primero pará la grabación"), 1600);
        return;
    }
    start_playback(0);
}

/* Five seconds back or forward. Stopped, forward starts from there. */
static void skip_cb(lv_event_t *event)
{
    int step = (int)(intptr_t)lv_event_get_user_data(event);
    if (s_ctx->detail_index >= s_ctx->file_count || is_recording()) {
        return;
    }
    uint32_t total_ms = s_ctx->files[s_ctx->detail_index].seconds * 1000u;
    aos_player_info_t player;
    if (!s_ctx->playing || !aos_hal_player_info(&player)) {
        if (step > 0) {
            start_playback((uint32_t)step < total_ms ? (uint32_t)step : 0);
        }
        return;
    }
    int64_t ms = (int64_t)player.position_ms + step;
    if (ms < 0) {
        ms = 0;
    }
    if (ms >= (int64_t)total_ms) {
        ms = total_ms > 500 ? total_ms - 500 : 0;
    }
    aos_hal_player_seek((uint32_t)ms);
}

/* A tap on the waveform jumps there, playing or not. */
static void wave_cb(lv_event_t *event)
{
    lv_obj_t *target = lv_event_get_current_target(event);
    lv_indev_t *indev = lv_indev_active();
    if (!indev || s_ctx->detail_index >= s_ctx->file_count || is_recording()) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_area_t a;
    lv_obj_get_coords(target, &a);
    int32_t w = lv_area_get_width(&a);
    if (w <= 0) {
        return;
    }
    int32_t x = p.x - a.x1;
    if (x < 0) x = 0;
    if (x > w) x = w;
    uint32_t total_ms = s_ctx->files[s_ctx->detail_index].seconds * 1000u;
    uint32_t ms = (uint32_t)((uint64_t)total_ms * (uint32_t)x / (uint32_t)w);
    if (s_ctx->playing) {
        aos_hal_player_seek(ms);
    } else {
        start_playback(ms);
    }
}

/* Deleting asks for confirmation on the same button: the first tap arms it
 * (red, "tap again") and the second deletes. */
static void delete_cb(lv_event_t *event)
{
    (void)event;

    if (!s_ctx->delete_armed) {
        s_ctx->delete_armed = true;
        s_ctx->delete_armed_ms = (uint32_t)aos_hal_uptime_ms();
        lv_label_set_text(UI.detail_del_text, _("Tocá de nuevo para borrar"));
        lv_obj_set_style_bg_color(UI.detail_del, AOS_C_RED, 0);
        lv_obj_set_style_text_color(UI.detail_del_text, AOS_C_TEXT, 0);
        return;
    }

    stop_playback();
    if (!rec_files_delete(s_ctx->files[s_ctx->detail_index].name)) {
        aos_ui_toast(_("No se pudo borrar"), 1600);
        detail_disarm();
        return;
    }
    aos_ui_toast(_("Borrada"), 1200);

    rescan();
    refresh_list();
    show_view(VIEW_MAIN);
}

static void back_cb(lv_event_t *event)
{
    (void)event;
    stop_playback();
    show_view(VIEW_MAIN);
}

/* -------------------------------------------------------------------------- */
/* Refresh                                                                     */
/* -------------------------------------------------------------------------- */

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_ctx || !UI.main_view) {
        return;
    }

    if (s_ctx->button_toggle) {
        s_ctx->button_toggle = false;
        toggle_recording();
    }

    aos_rec_status_t status;
    bool have = aos_hal_rec_status(&status);
    bool active = have && status.state != AOS_REC_IDLE;

    /* The capture can end by itself (the microphone is cut off, the card fills
     * up). If the screen was still showing "recording", say so instead of
     * leaving the stopwatch stuck. */
    if (!active && s_ctx->ui_recording) {
        set_recording_ui(false, false);
        rescan();
        refresh_list();
        aos_ui_toast(_("Se cortó la grabación"), 2000);
    }
    /* Paused or resumed from somewhere else (the portal, the side button). */
    if (active && (status.state == AOS_REC_PAUSED) != s_ctx->ui_paused) {
        set_recording_ui(true, status.state == AOS_REC_PAUSED);
    }

    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (active) {
        /* The envelope is drained whole even if there is more than one sample:
         * if the frame fell behind, the waveform advances faster but nothing
         * is skipped. */
        uint8_t peaks[32];
        int got = aos_hal_rec_peaks(peaks, (int)sizeof(peaks));
        if (got > 0) {
            live_push(peaks, got);
        }

        uint32_t seconds = status.elapsed_ms / 1000;
        if (seconds != s_ctx->last_shown_s) {
            s_ctx->last_shown_s = seconds;
            char buf[16];
            format_time(buf, sizeof(buf), seconds);
            lv_label_set_text(UI.time_label, buf);
        }

        /* The REC dot pulses once a second, like the tally light on a real
         * recorder. */
        if (now - s_ctx->blink_ms >= 500) {
            s_ctx->blink_ms = now;
            s_ctx->blink_on = !s_ctx->blink_on;
            lv_obj_set_style_bg_opa(UI.pill_dot,
                                    status.state == AOS_REC_PAUSED ? LV_OPA_COVER
                                    : (s_ctx->blink_on ? LV_OPA_COVER : LV_OPA_20), 0);
        }
    }
    if (now - s_ctx->last_info_ms >= 1000 || s_ctx->last_info_ms == 0) {
        s_ctx->last_info_ms = now ? now : 1;
        refresh_info(have ? &status : NULL);
    }

    if (s_ctx->view == VIEW_DETAIL) {
        if (s_ctx->delete_armed && now - s_ctx->delete_armed_ms > DELETE_ARM_MS) {
            detail_disarm();
        }

        if (s_ctx->playing) {
            aos_player_info_t player;
            uint32_t total = s_ctx->files[s_ctx->detail_index].seconds * 1000u;
            if (aos_hal_player_info(&player)) {
                if (player.state == AOS_PLAYER_STOPPED || (total && player.position_ms >= total)) {
                    s_ctx->playing = false;
                    detail_show_stopped();
                } else if (total) {
                    int bars = (int)((uint64_t)player.position_ms * (uint32_t)UI.detail_bars / total);
                    rec_wave_set_progress(UI.detail_wave, bars);

                    char elapsed[16];
                    format_time(elapsed, sizeof(elapsed), player.position_ms / 1000);
                    lv_label_set_text(UI.detail_position, elapsed);
                }
            }
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Building the views                                                          */
/* -------------------------------------------------------------------------- */

static lv_obj_t *round_button(lv_obj_t *parent, int32_t size, lv_color_t color,
                              lv_event_cb_t cb)
{
    lv_obj_t *button = plain(parent, 0, 0, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, NULL);
    return button;
}

/* The recorder proper: pill, time, state, info, waveform, buttons. Returns
 * the height it took. */
static int32_t build_recorder(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t avail_h)
{
    const lv_font_t *time_font = UI.land ? &aos_inter_num_96 : &aos_inter_num_144;
    int32_t tlh = lv_font_get_line_height(time_font);
    int32_t hero_h = 20 + 48 + 8 + tlh + 44 + 34 + 12;
    lv_obj_t *hero = card(parent, x, y, w, hero_h);

    /* the pill: REC / PAUSA / LISTO */
    UI.pill = plain(hero, (w - 150) / 2, 20, 150, 48);
    lv_obj_set_style_radius(UI.pill, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(UI.pill, LV_OPA_COVER, 0);
    UI.pill_dot = plain(UI.pill, 22, 17, 14, 14);
    lv_obj_set_style_radius(UI.pill_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(UI.pill_dot, AOS_C_TEXT, 0);
    UI.pill_text = text(UI.pill, "REC", aos_font_small, AOS_C_TEXT, 44, 10, 96,
                        LV_TEXT_ALIGN_CENTER);

    UI.time_label = text(hero, "00:00", time_font, AOS_C_TEXT, 0, 76, w, LV_TEXT_ALIGN_CENTER);
    UI.status_label = text(hero, "", aos_font_body, AOS_C_DIM, 16, 76 + tlh + 4, w - 32,
                           LV_TEXT_ALIGN_CENTER);
    UI.info_label = text(hero, "", aos_font_caption, AOS_C_DIM, 16, 76 + tlh + 48, w - 32,
                         LV_TEXT_ALIGN_CENTER);

    /* the live waveform */
    int32_t btn = UI.land ? 120 : 136;
    int32_t wy = y + hero_h + 16;
    int32_t wave_h = avail_h - (wy - y) - btn - 32;
    if (wave_h > 240) {
        wave_h = 240;
    }
    lv_obj_t *wc = card(parent, x, wy, w, wave_h);
    UI.live_bars = (int)((w - 40) / BAR_PITCH);
    if (UI.live_bars > LIVE_BARS_MAX) {
        UI.live_bars = LIVE_BARS_MAX;
    }
    UI.live_wave = rec_wave_create(wc, UI.live_bars * BAR_PITCH, wave_h - 40, UI.live_bars,
                                   AOS_C_CARD2);
    lv_obj_center(rec_wave_obj(UI.live_wave));
    rec_wave_set(UI.live_wave, s_ctx->live_hist + LIVE_BARS_MAX - UI.live_bars, UI.live_bars);

    /* the buttons: REC alone, or pause + stop */
    int32_t by = wy + wave_h + 16;
    int32_t cx = x + w / 2;
    UI.rec_btn = round_button(parent, btn, AOS_C_RED, rec_cb);
    lv_obj_set_pos(UI.rec_btn, cx - btn / 2, by);
    lv_obj_set_style_border_width(UI.rec_btn, 6, 0);
    lv_obj_set_style_border_color(UI.rec_btn, lv_color_hex(0x5A1512), 0);
    lv_obj_t *dot = plain(UI.rec_btn, 0, 0, btn / 2, btn / 2);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_center(dot);

    UI.stop_btn = round_button(parent, btn, AOS_C_RED, stop_cb);
    lv_obj_set_pos(UI.stop_btn, cx - btn / 2, by);
    lv_obj_t *square = plain(UI.stop_btn, 0, 0, btn * 3 / 10, btn * 3 / 10);
    lv_obj_set_style_radius(square, 8, 0);
    lv_obj_set_style_bg_color(square, AOS_C_TEXT, 0);
    lv_obj_set_style_bg_opa(square, LV_OPA_COVER, 0);
    lv_obj_center(square);

    int32_t small = btn * 3 / 4;
    UI.pause_btn = round_button(parent, small, AOS_C_CARD2, pause_cb);
    lv_obj_set_pos(UI.pause_btn, cx - btn / 2 - 48 - small, by + (btn - small) / 2);
    UI.pause_icon = glyph(UI.pause_btn, AOS_SYM_PAUSE, &aos_sym_44, AOS_C_TEXT);

    return by + btn - y;
}

static void build_list(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    text(parent, _("Grabaciones"), aos_font_title, AOS_C_TEXT, x + 8, y, w - 120,
         LV_TEXT_ALIGN_LEFT);
    UI.list_count = text(parent, "", aos_font_body, AOS_C_DIM, x + w - 120, y + 4, 112,
                         LV_TEXT_ALIGN_RIGHT);

    int32_t ly = y + 56;
    UI.list_body = lv_obj_create(parent);
    lv_obj_remove_style_all(UI.list_body);
    lv_obj_set_pos(UI.list_body, x, ly);
    lv_obj_set_size(UI.list_body, w, h - (ly - y));
    lv_obj_set_flex_flow(UI.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(UI.list_body, 10, 0);
    lv_obj_set_scroll_dir(UI.list_body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(UI.list_body, LV_SCROLLBAR_MODE_ACTIVE);

    UI.empty_hint = aos_label(parent, _("Todavía no grabaste nada.\nLo que grabes aparece acá."),
                              aos_font_body, AOS_C_DIM);
    lv_obj_set_width(UI.empty_hint, w - 32);
    lv_obj_set_style_text_align(UI.empty_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(UI.empty_hint, x + 16, ly + 40);
    lv_obj_remove_flag(UI.empty_hint, LV_OBJ_FLAG_CLICKABLE);
}

static void build_main(lv_obj_t *parent)
{
    const int32_t pad = AOS_UI_PAD;
    UI.main_view = plain(parent, 0, 0, UI.W, UI.H);

    if (!UI.land) {
        int32_t used = build_recorder(UI.main_view, pad, 12, UI.W - 2 * pad, 720);
        int32_t ly = 12 + used + 24;
        build_list(UI.main_view, pad, ly, UI.W - 2 * pad, UI.H - ly - 8);
    } else {
        int32_t lw = UI.W * 55 / 100;
        build_recorder(UI.main_view, pad, 8, lw, UI.H - 16);
        int32_t rx = pad + lw + 28;
        build_list(UI.main_view, rx, 8, UI.W - rx - pad, UI.H - 16);
    }
}

static void build_detail(lv_obj_t *parent)
{
    const int32_t pad = AOS_UI_PAD;
    int32_t w = UI.W - 2 * pad;
    UI.detail_view = plain(parent, 0, 0, UI.W, UI.H);
    lv_obj_set_style_bg_color(UI.detail_view, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(UI.detail_view, LV_OPA_COVER, 0);

    /* back */
    lv_obj_t *back = plain(UI.detail_view, pad - 8, 8, 300, AOS_UI_TAP_MIN);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_style_bg_color(back, AOS_C_CARD, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_radius(back, 22, 0);
    lv_obj_t *chev = aos_label(back, AOS_SYM_CHEVRON_LEFT, &aos_sym_44, AOS_C_ACCENT);
    lv_obj_align(chev, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_remove_flag(chev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *bl = aos_label(back, _("Grabaciones"), aos_font_body, AOS_C_ACCENT);
    lv_obj_align(bl, LV_ALIGN_LEFT_MID, 48, 0);
    lv_obj_remove_flag(bl, LV_OBJ_FLAG_CLICKABLE);

    int32_t y = UI.land ? 16 : 112;
    int32_t tx = UI.land ? pad + 320 : pad;
    int32_t tw = UI.land ? UI.W - tx - pad : w;
    UI.detail_title = text(UI.detail_view, "", UI.land ? aos_font_title : aos_font_large,
                           AOS_C_TEXT, tx, y, tw, LV_TEXT_ALIGN_LEFT);
    y += UI.land ? 44 : 64;
    UI.detail_meta = text(UI.detail_view, "", aos_font_small, AOS_C_DIM, tx, y, tw,
                          LV_TEXT_ALIGN_LEFT);
    y += UI.land ? 48 : 60;

    /* the whole file */
    int32_t btn = UI.land ? 120 : 140;
    int32_t wave_h = UI.land ? UI.H - y - 40 - btn - 28 : 440;
    lv_obj_t *wc = card(UI.detail_view, pad, y, w, wave_h);
    lv_obj_add_flag(wc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wc, wave_cb, LV_EVENT_CLICKED, NULL);
    UI.detail_bars = (int)((w - 40) / BAR_PITCH);
    if (UI.detail_bars > DETAIL_BARS_MAX) {
        UI.detail_bars = DETAIL_BARS_MAX;
    }
    UI.detail_wave = rec_wave_create(wc, UI.detail_bars * BAR_PITCH, wave_h - 48,
                                     UI.detail_bars, AOS_C_TEAL);
    lv_obj_center(rec_wave_obj(UI.detail_wave));
    y += wave_h + 8;

    UI.detail_position = text(UI.detail_view, "00:00", aos_font_small, AOS_C_TEXT, pad + 8, y,
                              200, LV_TEXT_ALIGN_LEFT);
    UI.detail_total = text(UI.detail_view, "", aos_font_small, AOS_C_DIM, pad + w - 208, y,
                           200, LV_TEXT_ALIGN_RIGHT);
    text(UI.detail_view, _("tocá la onda para saltar"), aos_font_caption, AOS_C_DIM,
         pad + 220, y + 2, w - 440, LV_TEXT_ALIGN_CENTER);
    y += UI.land ? 36 : 64;

    lv_obj_t *play = round_button(UI.detail_view, btn, AOS_C_TEAL, play_cb);
    UI.detail_play_icon = glyph(play, AOS_SYM_PLAY, &aos_sym_72, AOS_C_BG);
    int32_t sk = btn * 3 / 4;
    lv_obj_t *back5 = round_button(UI.detail_view, sk, AOS_C_CARD2, skip_cb);
    lv_obj_remove_event_cb(back5, skip_cb);
    lv_obj_add_event_cb(back5, skip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-5000);
    glyph(back5, "-5 s", aos_font_body, AOS_C_TEXT);
    lv_obj_t *fwd5 = round_button(UI.detail_view, sk, AOS_C_CARD2, skip_cb);
    lv_obj_remove_event_cb(fwd5, skip_cb);
    lv_obj_add_event_cb(fwd5, skip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)5000);
    glyph(fwd5, "+5 s", aos_font_body, AOS_C_TEXT);
    lv_obj_set_pos(back5, UI.W / 2 - btn / 2 - 40 - sk, y + (btn - sk) / 2);
    lv_obj_set_pos(fwd5, UI.W / 2 + btn / 2 + 40, y + (btn - sk) / 2);

    int32_t dw = UI.land ? 400 : w;
    UI.detail_del = plain(UI.detail_view, 0, 0, dw, AOS_UI_TAP_MIN);
    lv_obj_set_style_radius(UI.detail_del, AOS_UI_TAP_MIN / 2, 0);
    lv_obj_set_style_bg_color(UI.detail_del, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(UI.detail_del, LV_OPA_COVER, 0);
    lv_obj_add_flag(UI.detail_del, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(UI.detail_del, delete_cb, LV_EVENT_CLICKED, NULL);
    UI.detail_del_text = glyph(UI.detail_del, "", aos_font_body, AOS_C_RED);

    if (UI.land) {
        lv_obj_set_pos(play, (UI.W - btn) / 2, y);
        lv_obj_set_pos(UI.detail_del, UI.W - pad - dw, y + (btn - AOS_UI_TAP_MIN) / 2);
    } else {
        lv_obj_set_pos(play, (UI.W - btn) / 2, y);
        lv_obj_set_pos(UI.detail_del, pad, UI.H - AOS_UI_TAP_MIN - 24);
    }
}

/* Everything that depends on the size of the screen. */
static void build_ui(lv_obj_t *root)
{
    rec_wave_delete(UI.live_wave);
    rec_wave_delete(UI.detail_wave);
    memset(&UI, 0, sizeof(UI));

    lv_obj_update_layout(root);
    UI.W = lv_obj_get_width(root);
    UI.H = lv_obj_get_height(root);
    UI.land = UI.W > UI.H;

    lv_obj_t *page = aos_page(root);
    build_main(page);
    build_detail(page);

    refresh_list();

    /* If it is reopened with a recording under way (the HAL goes on recording
     * even when the app is not there), the screen starts by showing it. */
    aos_rec_status_t status = { 0 };
    bool recording = aos_hal_rec_status(&status) && status.state != AOS_REC_IDLE;
    set_recording_ui(recording, status.state == AOS_REC_PAUSED);
    if (recording) {
        char buf[16];
        format_time(buf, sizeof(buf), status.elapsed_ms / 1000);
        lv_label_set_text(UI.time_label, buf);
        s_ctx->last_shown_s = status.elapsed_ms / 1000;
    }

    if (s_ctx->view == VIEW_DETAIL && s_ctx->detail_index < s_ctx->file_count) {
        bool was_playing = s_ctx->playing;
        s_ctx->playing = false;             /* refresh_detail must not stop it */
        refresh_detail();
        s_ctx->playing = was_playing;
        if (was_playing) {
            lv_label_set_text(UI.detail_play_icon, AOS_SYM_STOP);
        }
        show_view(VIEW_DETAIL);
    } else {
        show_view(VIEW_MAIN);
    }
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static void *rec_create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;

    rec_ctx_t *ctx = lv_malloc_zeroed(sizeof(rec_ctx_t));
    if (!ctx) {
        return NULL;
    }
    s_ctx = ctx;
    ctx->root = root;

    rescan();
    build_ui(root);

    ctx->timer = lv_timer_create(tick_cb, 50, NULL);
    return ctx;
}

static bool rec_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    if (!inst) {
        return false;
    }
    lv_obj_clean(root);
    build_ui(root);
    return true;
}

static void rec_show(aos_app_t *self, void *inst)
{
    (void)self;
    rec_ctx_t *ctx = (rec_ctx_t *)inst;
    if (!ctx) {
        return;
    }
    /* Something may have changed on the card meanwhile (the portal deletes,
     * the recording went on with the app hidden). */
    if (ctx->view == VIEW_MAIN) {
        rescan();
        refresh_list();
    }
    if (ctx->timer) {
        lv_timer_resume(ctx->timer);
    }
}

static void rec_hide(aos_app_t *self, void *inst)
{
    (void)self;
    rec_ctx_t *ctx = (rec_ctx_t *)inst;
    if (ctx && ctx->timer) {
        lv_timer_pause(ctx->timer);
    }
}

static void rec_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    rec_ctx_t *ctx = (rec_ctx_t *)inst;
    if (!ctx) {
        return;
    }

    if (ctx->timer) {
        lv_timer_delete(ctx->timer);
    }
    stop_playback();
    rec_wave_delete(ctx->ui.live_wave);
    rec_wave_delete(ctx->ui.detail_wave);

    /* The recording carries on: it lives in the HAL. On reopening the app, the
     * screen picks it up where it was. */
    lv_free(ctx);
    s_ctx = NULL;
}

static bool rec_back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;

    if (s_ctx && s_ctx->view == VIEW_DETAIL) {
        stop_playback();
        show_view(VIEW_MAIN);
        return true;
    }
    return false;
}

/* The side button starts and stops the recording without looking at the
 * screen, which is what you want a physical button on a recorder for. It is
 * noted and acted on next frame: the callback arrives from the HAL's task. */
static bool rec_button(aos_app_t *self, void *inst, int action)
{
    (void)self; (void)inst;

    if (!s_ctx || action != AOS_BUTTON_CLICK || s_ctx->view != VIEW_MAIN) {
        return false;
    }
    s_ctx->button_toggle = true;
    return true;
}

static bool rec_init(aos_app_t *app)
{
    app->desc.id       = "app.recorder";
    app->desc.name     = "Grabadora";
    app->desc.icon     = LV_SYMBOL_AUDIO;
    app->desc.icon_vec = AOS_ICON_MIC;
    app->desc.color_a  = 0xFF453A;
    app->desc.color_b  = 0x7A1512;
    app->desc.order    = 46;
    /* KEEP: leaving and coming back finds the same view. A recording under
     * way does not need it - it lives in the HAL and survives even destroy -
     * so the watch's trick of raising BACKGROUND while recording is gone. */
    app->desc.flags    = AOS_APP_FLAG_KEEP;

    app->create  = rec_create;
    app->destroy = rec_destroy;
    app->show    = rec_show;
    app->hide    = rec_hide;
    app->back    = rec_back;
    app->button  = rec_button;
    app->resize  = rec_resize;
    return true;
}

AOS_APP_ENTRY(rec_init);
