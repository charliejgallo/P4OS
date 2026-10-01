/*
 * P4OS - Video: the list of videos on the card.
 *
 * One card per file, with its first frame, its size, frame rate and length.
 * None of that is known when the list goes up: the app's worker (the same
 * one that decodes while a video plays, idle here) opens each file for its
 * headers and decodes its first frame with aos_hal_image_decode(), which
 * takes the JPEG right out of the AVI by offset. The LVGL timer puts on the
 * rows whatever the worker has finished. Opening a video stops this worker;
 * back in the list it picks up where it was.
 */
#include "video.h"

#include "aos_fonts.h"
#include "aos_i18n.h"
#include "aos_internal.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CARD_H          (VD_THUMB_H + 32)
#define CARD_GAP        16
#define THUMB_STACK     8192

static bool has_ext(const char *name, const char *ext)
{
    const char *dot = strrchr(name, '.');
    return dot && strcasecmp(dot, ext) == 0;
}

/* ---- the card --------------------------------------------------------------- */

static bool scan_cb(const aos_dir_entry_t *e, void *ctx)
{
    vd_t *v = ctx;
    if (e->dir || e->name[0] == '.' || !has_ext(e->name, ".avi")) {
        return true;                /* ("._x.avi": macOS's shadow files, hidden) */
    }
    if (v->count >= VD_MAX_FILES) {
        return false;
    }
    vd_file_t *f = &v->files[v->count++];
    snprintf(f->name, sizeof f->name, "%s", e->name);
    f->bytes = e->size;
    return true;
}

static int by_name(const void *a, const void *b)
{
    return strcasecmp(((const vd_file_t *)a)->name, ((const vd_file_t *)b)->name);
}

void vd_list_scan(vd_t *v)
{
    const char *root = aos_hal_path_sd_root();
    snprintf(v->dir, sizeof v->dir, "%s/videos", root ? root : "");
    v->count = 0;
    aos_hal_dir_scan(v->dir, scan_cb, v);
    qsort(v->files, (size_t)v->count, sizeof v->files[0], by_name);
}

/* The sound beside a video: same name, .wav or .mp3. */
bool vd_audio_path(const vd_t *v, int file, char *out, size_t len)
{
    static const char *const EXT[] = { ".wav", ".mp3", ".WAV", ".MP3" };
    for (size_t i = 0; i < sizeof EXT / sizeof EXT[0]; i++) {
        snprintf(out, len, "%s/%s", v->dir, v->files[file].name);
        char *dot = strrchr(out, '.');
        if (!dot || (size_t)(dot - out) + strlen(EXT[i]) + 1 > len) {
            break;
        }
        strcpy(dot, EXT[i]);
        FILE *probe = fopen(out, "rb");
        if (probe) {
            fclose(probe);
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

/* ---- the worker: headers and first frames ----------------------------------- */

static void thumbs_worker(void *arg)
{
    vd_t *v = arg;
    for (int i = 0; i < v->count && !aos_hal_worker_should_stop(); i++) {
        vd_file_t *f = &v->files[i];
        if (f->info != 0) {
            continue;
        }
        char path[256];
        snprintf(path, sizeof path, "%s/%s", v->dir, f->name);
        vd_avi_t a;
        if (!vd_avi_open(&a, path, false)) {
            f->info = -1;
            continue;
        }
        f->w        = a.width;
        f->h        = a.height;
        f->ms       = vd_avi_duration_ms(&a);
        f->fps_x100 = a.scale ? (uint32_t)((uint64_t)a.rate * 100 / a.scale) : 0;
        vd_frame_t first = a.first;
        vd_avi_close(&a);
        char sound[256];
        f->audio = vd_audio_path(v, i, sound, sizeof sound);
        int tw = 0, th = 0;
        f->thumb = aos_hal_image_decode(path, first.off + 8, first.size, VD_THUMB_W, VD_THUMB_H,
                                        false, &tw, &th, NULL, NULL);
        f->tw = tw;
        f->th = th;
        __sync_synchronize();
        f->info = 1;
    }
    bool all = true;
    for (int i = 0; i < v->count; i++) {
        all &= v->files[i].info != 0;
    }
    v->thumbs_done = all;
}

bool vd_list_thumbs_start(vd_t *v)
{
    if (v->thumbs_done || v->count == 0) {
        return true;
    }
    return aos_hal_worker_start("aos_video", thumbs_worker, v, THUMB_STACK);
}

void vd_list_free(vd_t *v)
{
    for (int i = 0; i < v->count; i++) {
        if (v->files[i].thumb) {
            aos_hal_image_free(v->files[i].thumb);
            v->files[i].thumb = NULL;
        }
    }
}

/* ---- the rows ---------------------------------------------------------------- */

void vd_fmt_time(char *out, size_t len, uint32_t ms)
{
    uint32_t s = ms / 1000;
    if (s >= 3600) {
        snprintf(out, len, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    } else {
        snprintf(out, len, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
    }
}

static void fill_row(vd_t *v, vd_file_t *f)
{
    (void)v;
    if (!f->meta) {
        return;
    }
    if (f->info < 0) {
        lv_label_set_text(f->meta, _("No es un AVI de MJPEG"));
        lv_obj_set_style_text_color(f->meta, AOS_C_ORANGE, 0);
        return;
    }
    char len[16], fps[16], line[80];
    vd_fmt_time(len, sizeof len, f->ms);
    if (f->fps_x100 % 100) {
        snprintf(fps, sizeof fps, "%u,%02u", (unsigned)(f->fps_x100 / 100), (unsigned)(f->fps_x100 % 100));
    } else {
        snprintf(fps, sizeof fps, "%u", (unsigned)(f->fps_x100 / 100));
    }
    snprintf(line, sizeof line, "%s  ·  %ux%u  ·  %s fps", len, (unsigned)f->w, (unsigned)f->h, fps);
    lv_label_set_text(f->meta, line);
    if (!f->audio) {
        lv_label_set_text(f->note, _("Sin sonido"));
    }
    if (f->thumb && f->img) {
        lv_image_dsc_t *d = &f->tdsc;
        memset(d, 0, sizeof *d);
        d->header.magic  = LV_IMAGE_HEADER_MAGIC;
        d->header.cf     = LV_COLOR_FORMAT_RGB565;
        d->header.w      = (uint32_t)f->tw;
        d->header.h      = (uint32_t)f->th;
        d->header.stride = (uint32_t)f->tw * 2;
        d->data_size     = (uint32_t)(f->tw * f->th * 2);
        d->data          = (const uint8_t *)f->thumb;
        lv_image_set_src(f->img, d);
        lv_obj_center(f->img);
        lv_obj_remove_flag(f->img, LV_OBJ_FLAG_HIDDEN);
    }
}

void vd_list_tick(vd_t *v)
{
    for (int i = 0; i < v->count; i++) {
        vd_file_t *f = &v->files[i];
        if (f->info != 0 && !f->on_row) {
            __sync_synchronize();
            fill_row(v, f);
            f->on_row = true;
        }
    }
}

static void open_cb(lv_event_t *e)
{
    vd_t *v = lv_event_get_user_data(e);
    lv_obj_t *card = lv_event_get_current_target(e);
    vd_ui_open_file(v, (int)(intptr_t)lv_obj_get_user_data(card));
}

static lv_obj_t *card_create(vd_t *v, lv_obj_t *parent, int i, int32_t w)
{
    vd_file_t *f = &v->files[i];
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, CARD_H);
    lv_obj_set_style_bg_color(card, AOS_C_CARD, 0);
    lv_obj_set_style_bg_color(card, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, AOS_UI_RADIUS, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_user_data(card, (void *)(intptr_t)i);
    lv_obj_add_event_cb(card, open_cb, LV_EVENT_CLICKED, v);

    /* The first frame, fitted into a black 16:9 box: a tall video gets
     * bands on the sides, as it would upright on a wide screen. */
    lv_obj_t *box = lv_obj_create(card);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, VD_THUMB_W, VD_THUMB_H);
    lv_obj_align(box, LV_ALIGN_LEFT_MID, 16, 0);
    lv_obj_set_style_bg_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(box, 18, 0);
    lv_obj_set_style_clip_corner(box, true, 0);
    lv_obj_t *glyph = aos_label(box, AOS_SYM_VIDEO, &aos_sym_44, AOS_C_CARD2);
    lv_obj_center(glyph);
    f->img = lv_image_create(box);
    lv_obj_add_flag(f->img, LV_OBJ_FLAG_HIDDEN);

    int32_t tx = 16 + VD_THUMB_W + 20, tw = w - tx - 20;
    lv_obj_t *name = aos_label(card, f->name, aos_font_body, AOS_C_TEXT);
    char *dot = strrchr(f->name, '.');
    if (dot) {
        lv_label_set_text_fmt(name, "%.*s", (int)(dot - f->name), f->name);
    }
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(name, tw, 72);          /* two lines at most */
    lv_obj_set_pos(name, tx, 18);

    f->meta = aos_label(card, "...", aos_font_small, AOS_C_DIM);
    lv_label_set_long_mode(f->meta, LV_LABEL_LONG_DOT);
    lv_obj_set_size(f->meta, tw, lv_font_get_line_height(aos_font_small));    /* one line */
    lv_obj_set_pos(f->meta, tx, 96);

    f->note = aos_label(card, "", aos_font_caption, AOS_C_ORANGE);
    lv_obj_set_pos(f->note, tx, 130);
    aos_make_decorative(box);
    aos_make_decorative(name);
    aos_make_decorative(f->meta);
    aos_make_decorative(f->note);
    f->on_row = false;
    return card;
}

void vd_list_build(vd_t *v)
{
    const aos_geo_t *geo = aos_ui_geo();
    lv_obj_t *lv = lv_obj_create(v->root);
    v->list_view = lv;
    lv_obj_remove_style_all(lv);
    lv_obj_set_size(lv, v->scr_w, v->scr_h);
    lv_obj_set_style_bg_color(lv, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(lv, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(lv, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_top(lv, geo->bar_h + 16, 0);
    lv_obj_set_style_pad_hor(lv, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(lv, 48, 0);
    lv_obj_set_style_pad_row(lv, CARD_GAP, 0);
    lv_obj_set_style_pad_column(lv, CARD_GAP, 0);
    lv_obj_set_scroll_dir(lv, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(lv, LV_SCROLLBAR_MODE_OFF);

    int32_t inner = v->scr_w - 2 * AOS_UI_PAD;
    lv_obj_t *title = aos_label(lv, _("Videos"), aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(title, inner);
    lv_obj_add_flag(title, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    aos_make_decorative(title);

    char sub[200];
    if (v->count == 1) {
        snprintf(sub, sizeof sub, _("1 video en %s"), v->dir);
    } else {
        snprintf(sub, sizeof sub, _("%d videos en %s"), v->count, v->dir);
    }
    lv_obj_t *info = aos_label(lv, sub, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(info, inner);
    lv_label_set_long_mode(info, LV_LABEL_LONG_DOT);
    lv_obj_add_flag(info, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    lv_obj_set_style_pad_bottom(info, 8, 0);
    aos_make_decorative(info);

    if (v->count == 0) {
        lv_obj_t *empty = aos_label(lv, _("No hay videos en la tarjeta.\n\n"
                                          "Copiá archivos .avi de MJPEG a la carpeta videos, "
                                          "con el sonido en un .wav del mismo nombre.\n\n"
                                          "Para convertirlos: apps/video/convert.sh (ver el README)."),
                                    aos_font_body, AOS_C_DIM);
        lv_obj_set_width(empty, inner);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_pad_top(empty, 80, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(empty, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        aos_make_decorative(empty);
        return;
    }
    /* One column upright, two lying down. */
    int cols = v->land ? 2 : 1;
    int32_t w = (inner - (cols - 1) * CARD_GAP) / cols;
    for (int i = 0; i < v->count; i++) {
        lv_obj_t *card = card_create(v, lv, i, w);
        if (i == 0) {
            lv_obj_add_flag(card, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        }
    }
}
