/*
 * P4OS - Dibujo: a drawing and design program for the 5" screen.
 *
 * A drawing ("dibujo") is a document of the screen's size, an A4 at the
 * screen's resolution or a square, made of up to eight layers. It is painted
 * with the finger, a USB mouse or a joystick: a hard pencil, a soft brush, an
 * airbrush, a translucent marker and an eraser, with size, opacity and a
 * smoothing that calms the line; a fill, an eyedropper; lines, rectangles,
 * ellipses, polygons, arrows and texts that stay editable (moved, scaled,
 * turned) until they are fixed; a rectangular selection to move or copy; a
 * grid and guides with a magnet, rulers, mirror symmetry, and deep undo.
 * Two fingers (or the mouse's wheel) zoom and pan.
 *
 * Everything is on the card under /sdcard/dibujo: the drawings with their
 * layers (.dib, dib_doc.h), the PNGs exported from them, the palettes
 * (paletas.txt) and the pictures to import (importar/). The portal page
 * (web/dibujo.js) shows them, renders a .dib in the browser and downloads
 * it as PNG, and uploads pictures to import.
 *
 * The engine (dib_doc, dib_paint, dib_raster, dib_png) has no LVGL and is
 * checked on the Mac by tools/dib_harness.c; this file and the dib_editor,
 * dib_panels and dib_icons ones are the app around it.
 *
 * Memory: the layers' tiles, the history and the floating layer share one
 * budget, decided at start from the free PSRAM; the screen copy of the
 * document (RGB565, 2.6 MB at most) and the canvas window (1.4 MB) are apart.
 * All of it is PSRAM; the internal RAM is only LVGL's objects.
 */
#include "dib_app.h"
#include "dib_png.h"
#include "aos_icon_ops.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define KEY_SEEDED  "dib_seed"
#define GAL_COLS_P  3
#define GAL_COLS_L  5

static app_t *s_app;    /* the keyboard's handler has no user pointer */

/* --------------------------------------------------------------------------
 * Paths and small things
 * -------------------------------------------------------------------------- */

const char *dj_dir(void)
{
    static char dir[96];
    static bool made;
    const char *sd = aos_hal_path_sd_root();
    snprintf(dir, sizeof dir, "%s/dibujo", sd ? sd : aos_hal_path_data());
    if (!made) {
        mkdir(dir, 0777);
        char sub[128];
        snprintf(sub, sizeof sub, "%s/importar", dir);
        mkdir(sub, 0777);
        made = true;
    }
    return dir;
}

static void file_path(const char *file, char *out, size_t n)
{
    snprintf(out, n, "%s/%s", dj_dir(), file);
}

static uint32_t now_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

void dj_toast(const char *msg)
{
    aos_ui_toast(msg, 1600);
}

lv_color_t dj_ui_color(dib_px_t p)
{
    return lv_color_make(DIB_R(p), DIB_G(p), DIB_B(p));
}

lv_obj_t *dj_ui_button(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, lv_color_t c,
                    lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_bg_color(b, c, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    /* feedback by opacity, never by scale: a transform is a layer */
    lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *dj_ui_text_button(lv_obj_t *parent, const char *text, const lv_font_t *font, int32_t w,
                         int32_t h, lv_color_t c, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = dj_ui_button(parent, 0, 0, w, h, c, cb, ud);
    /* a size smaller rather than a word cut in two ("Duplizieren") */
    if (!font) font = aos_font_body;
    lv_point_t sz;
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    if (sz.x > w - 16 && font != aos_font_tiny) {
        lv_text_get_size(&sz, text, aos_font_caption, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        font = sz.x > w - 16 ? aos_font_tiny : aos_font_caption;
    }
    lv_obj_t *l = aos_label(b, text, font, AOS_C_TEXT);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(l, w - 16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *dj_ui_card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_bg_color(c, C_PANEL, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, lv_color_hex(0x3A3A3E), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_shadow_width(c, 0, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);      /* swallows touches underneath */
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* --------------------------------------------------------------------------
 * Palettes: /sdcard/dibujo/paletas.txt, one per line, "name: RRGGBB ...",
 * and a line "recientes: ..." for the colours used last. A text file so the
 * portal (or a person) can write it too.
 * -------------------------------------------------------------------------- */

static const uint32_t DEF_PAL[3][PAL_COLORS] = {
    { 0x000000, 0xFFFFFF, 0x8E8E93, 0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759, 0x00C7BE,
      0x30B0C7, 0x007AFF, 0x5856D6, 0xAF52DE, 0xFF2D55, 0xA2845E, 0x1C1C1E, 0xD1D1D6 },
    { 0xFFB3BA, 0xFFDFBA, 0xFFFFBA, 0xBAFFC9, 0xBAE1FF, 0xD7BAFF, 0xFFC6E8, 0xC9F2EF,
      0xF1E3C8, 0xE2F0CB, 0xB5EAD7, 0xC7CEEA, 0xFFDAC1, 0xE0BBE4, 0x957DAD, 0xD291BC },
    { 0x3E2723, 0x5D4037, 0x795548, 0xA1887F, 0xD7CCC8, 0x8D6E63, 0xBF8040, 0xC9A26B,
      0xE6C79C, 0x556B2F, 0x6B8E23, 0x8F9779, 0x2F4F4F, 0x708090, 0xB22222, 0xCD853F },
};

static void palettes_default(app_t *a)
{
    static const char *const names[NPALETTES] = { N_("Básica"), N_("Pastel"), N_("Tierra"), N_("Mía") };
    for (int p = 0; p < NPALETTES; p++) {
        snprintf(a->pal[p].name, sizeof a->pal[p].name, "%s", names[p]);
        a->pal[p].n = p < 3 ? PAL_COLORS : 0;
        for (int i = 0; i < a->pal[p].n; i++) a->pal[p].c[i] = 0xFF000000u | DEF_PAL[p][i];
    }
}

static int parse_colors(const char *s, dib_px_t *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '#' || *s == ',') s++;
        char *end;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s) break;
        out[n++] = 0xFF000000u | (dib_px_t)(v & 0xFFFFFF);
        s = end;
    }
    return n;
}

static void palettes_load(app_t *a)
{
    palettes_default(a);
    char path[160];
    file_path("paletas.txt", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    int p = 0;
    while (fgets(line, sizeof line, f)) {
        char *colon = strchr(line, ':');
        if (!colon || line[0] == '#') continue;
        *colon = 0;
        if (!strcmp(line, "recientes")) {
            a->nrecent = parse_colors(colon + 1, a->recent, NRECENT);
        } else if (p < NPALETTES) {
            snprintf(a->pal[p].name, sizeof a->pal[p].name, "%.23s", line);
            a->pal[p].n = parse_colors(colon + 1, a->pal[p].c, PAL_COLORS);
            p++;
        }
    }
    fclose(f);
}

void dj_palettes_save(app_t *a)
{
    char path[160];
    file_path("paletas.txt", path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("# Dibujo: paletas (nombre: colores en hexa) y los colores recientes\n", f);
    for (int p = 0; p < NPALETTES; p++) {
        char line[256];
        int o = snprintf(line, sizeof line, "%s:", a->pal[p].name);
        for (int i = 0; i < a->pal[p].n && o < (int)sizeof line - 8; i++) {
            o += snprintf(line + o, sizeof line - o, " %06X", (unsigned)(a->pal[p].c[i] & 0xFFFFFF));
        }
        fputs(line, f);
        fputs("\n", f);
    }
    char line[160];
    int o = snprintf(line, sizeof line, "recientes:");
    for (int i = 0; i < a->nrecent; i++) o += snprintf(line + o, sizeof line - o, " %06X", (unsigned)(a->recent[i] & 0xFFFFFF));
    fputs(line, f);
    fputs("\n", f);
    fclose(f);
}

/* --------------------------------------------------------------------------
 * Settings kept between sessions
 * -------------------------------------------------------------------------- */

static int32_t pref_i(const char *k, int32_t def)
{
    int32_t v = def;
    aos_hal_pref_get_i32(k, &v);
    return v;
}

static void prefs_load(app_t *a)
{
    for (int k = 0; k < DIB_BR_COUNT; k++) {
        a->brush_size[k] = dib_brush_defs[k].def_size;
        a->brush_opacity[k] = dib_brush_defs[k].def_opacity;
    }
    a->brush.kind = pref_i("dib_brush", DIB_BR_SOFT);
    if (a->brush.kind < 0 || a->brush.kind >= DIB_BR_COUNT || a->brush.kind == DIB_BR_ERASER) a->brush.kind = DIB_BR_SOFT;
    a->brush.smooth = (float)pref_i("dib_smooth", 35) / 100.0f;
    a->color = 0xFF000000u | (dib_px_t)pref_i("dib_color", 0x1C1C1E);
    a->color2 = 0xFF000000u | (dib_px_t)pref_i("dib_color2", 0x5AC8FA);
    a->grid = pref_i("dib_grid", 0) != 0;
    a->grid_step = pref_i("dib_gstep", 32);
    a->rulers = pref_i("dib_rulers", 0) != 0;
    a->snap = pref_i("dib_snap", 1) != 0;
    a->shape = DIB_OBJ_RECT;
    a->shape_fill = false;
    a->shape_stroke = true;
    a->shape_w = 6;
    a->shape_radius = 0;
    a->fill_tol = 32;
    a->sample_all = true;
    a->font_idx = 6;
    a->eraser_size = dib_brush_defs[DIB_BR_ERASER].def_size;
    a->eraser_opacity = 1.0f;
    a->tool = T_BRUSH;
}

static void prefs_save(app_t *a)
{
    aos_hal_pref_set_i32("dib_brush", a->brush.kind);
    aos_hal_pref_set_i32("dib_smooth", (int32_t)(a->brush.smooth * 100.0f + 0.5f));
    aos_hal_pref_set_i32("dib_color", (int32_t)(a->color & 0xFFFFFF));
    aos_hal_pref_set_i32("dib_color2", (int32_t)(a->color2 & 0xFFFFFF));
    aos_hal_pref_set_i32("dib_grid", a->grid);
    aos_hal_pref_set_i32("dib_gstep", a->grid_step);
    aos_hal_pref_set_i32("dib_rulers", a->rulers);
    aos_hal_pref_set_i32("dib_snap", a->snap);
}

/* --------------------------------------------------------------------------
 * The document's life
 * -------------------------------------------------------------------------- */

void dj_mark_dirty(app_t *a)
{
    a->dirty = true;
    a->changed_ms = now_ms();
}

bool dj_save(app_t *a)
{
    if (!a->doc || !a->cur_file[0]) return false;
    char path[160];
    file_path(a->cur_file, path, sizeof path);
    uint32_t t0 = now_ms();
    bool ok = dib_save(a->doc, path);
    aos_hal_log(TAG, "%s %s in %u ms (%u KB of tiles)", ok ? "saved" : "SAVE FAILED", path,
                (unsigned)(now_ms() - t0), (unsigned)(a->doc->tile_bytes >> 10));
    if (ok) a->dirty = false;
    else dj_toast(_("No se pudo guardar"));
    return ok;
}

static void doc_close(app_t *a)
{
    if (a->has_obj) dj_ed_obj_commit(a);
    if (a->dirty) dj_save(a);
    dib_doc_free(a->doc);
    a->doc = NULL;
    a->cur_file[0] = 0;
}

static void next_name(char *out, size_t n)
{
    for (int i = 1; i < 1000; i++) {
        char path[160];
        snprintf(out, n, "dibujo%d.dib", i);
        file_path(out, path, sizeof path);
        struct stat st;
        if (stat(path, &st) != 0) return;
    }
}

/* The name shown for "dibujo12.dib": "Dibujo 12". */
static void pretty_name(const char *file, char *out, size_t n)
{
    int k = 0;
    if (sscanf(file, "dibujo%d.dib", &k) == 1) {
        snprintf(out, n, "%s %d", _("Dibujo"), k);
    } else {
        snprintf(out, n, "%s", file);
        char *dot = strrchr(out, '.');
        if (dot) *dot = 0;
    }
}

static void open_doc(app_t *a, dib_doc_t *d, const char *file)
{
    a->doc = d;
    snprintf(a->cur_file, sizeof a->cur_file, "%s", file);
    a->dirty = false;
    a->sel = (dib_rect_t){ 0, 0, 0, 0 };
    a->has_obj = false;
    a->npoly = 0;
    lv_obj_add_flag(a->gal, LV_OBJ_FLAG_HIDDEN);
    dj_ed_enter(a);
    aos_hal_log(TAG, "opened %s: %dx%d, %d layers, %u KB", file, d->w, d->h, d->nlayers,
                (unsigned)(d->tile_bytes >> 10));
}

static bool open_file(app_t *a, const char *file)
{
    char path[160];
    file_path(file, path, sizeof path);
    uint32_t t0 = now_ms();
    dib_doc_t *d = dib_load(path, a->budget);
    if (!d) {
        aos_hal_log(TAG, "cannot open %s", path);
        dj_toast(_("No se pudo abrir"));
        return false;
    }
    aos_hal_log(TAG, "loaded in %u ms", (unsigned)(now_ms() - t0));
    open_doc(a, d, file);
    return true;
}

void dj_sizes_chosen(app_t *a, int w, int h)
{
    dib_doc_t *d = dib_doc_new(w, h, 0xFFFFFFFFu, a->budget);
    if (!d) {
        dj_toast(_("No hay memoria para un dibujo de ese tamaño"));
        return;
    }
    snprintf(d->layers[0].name, DIB_NAME_MAX, "%s 1", _("Capa"));
    char file[32];
    next_name(file, sizeof file);
    open_doc(a, d, file);
    a->dirty = true;
    dj_save(a);
}

void dj_go_gallery(app_t *a)
{
    dj_ed_leave(a);
    doc_close(a);
    lv_obj_add_flag(a->ed, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(a->gal, LV_OBJ_FLAG_HIDDEN);
    a->gal_dirty = true;
}

/* --------------------------------------------------------------------------
 * The gallery
 * -------------------------------------------------------------------------- */

typedef struct {
    app_t *a;
} scan_ctx_t;

static bool scan_cb(const aos_dir_entry_t *e, void *ctx)
{
    app_t *a = ((scan_ctx_t *)ctx)->a;
    if (e->dir || e->name[0] == '.' || a->nproj >= MAX_PROJECTS) return a->nproj < MAX_PROJECTS;
    size_t n = strlen(e->name);
    if (n < 5 || n >= sizeof a->proj[0].file || strcmp(e->name + n - 4, ".dib")) return true;
    project_t *p = &a->proj[a->nproj++];
    memset(p, 0, sizeof(*p));
    snprintf(p->file, sizeof p->file, "%s", e->name);
    p->mtime = (long)e->mtime;
    return true;
}

static int proj_cmp(const void *x, const void *y)
{
    const project_t *a = x, *b = y;
    if (a->mtime != b->mtime) return a->mtime > b->mtime ? -1 : 1;
    return strcmp(a->file, b->file);
}

static void gallery_free(app_t *a)
{
    for (int i = 0; i < a->nproj; i++) {
        dib_free(a->proj[i].prev);
        a->proj[i].prev = NULL;
    }
    a->nproj = 0;
}

static void gallery_scan(app_t *a)
{
    gallery_free(a);
    scan_ctx_t c = { a };
    aos_hal_dir_scan(dj_dir(), scan_cb, &c);
    qsort(a->proj, a->nproj, sizeof(project_t), proj_cmp);
    for (int i = 0; i < a->nproj; i++) {
        project_t *p = &a->proj[i];
        char path[160];
        file_path(p->file, path, sizeof path);
        p->prev = dib_alloc(DIB_PREVIEW * DIB_PREVIEW * 2);
        if (p->prev && !dib_peek(path, &p->w, &p->h, p->prev, &p->pw, &p->ph)) {
            dib_free(p->prev);
            p->prev = NULL;
        }
    }
}

static void sheet_close(app_t *a)
{
    a->sheet_close_req = true;
}

static void card_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing || a->gal_sheet) return;
    int i = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    if (i < 0 || i >= a->nproj) return;
    if (lv_event_get_code(e) == LV_EVENT_LONG_PRESSED) {
        a->sheet_req = i;               /* the frame timer builds the sheet */
        lv_indev_wait_release(lv_indev_active());
        return;
    }
    char file[32];
    snprintf(file, sizeof file, "%s", a->proj[i].file);
    open_file(a, file);
}

static void new_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    if (a->closing) return;
    dj_sizes_open(a);
}

static void gal_back_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    a->exit_req = true;
}

/* Exports a drawing of the gallery: loads it, then the same path as the
 * editor's. */
static void sheet_export_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    int i = a->sheet_proj;
    sheet_close(a);
    if (i < 0 || i >= a->nproj || a->job) return;
    char file[32];
    snprintf(file, sizeof file, "%s", a->proj[i].file);
    if (open_file(a, file)) dj_export_png(a);
}

static void sheet_dup_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    int i = a->sheet_proj;
    sheet_close(a);
    if (i < 0 || i >= a->nproj) return;
    char src[160], dst[160], name[32];
    file_path(a->proj[i].file, src, sizeof src);
    next_name(name, sizeof name);
    file_path(name, dst, sizeof dst);
    dib_doc_t *d = dib_load(src, a->budget);
    bool ok = d && dib_save(d, dst);
    dib_doc_free(d);
    dj_toast(ok ? _("Duplicado") : _("No se pudo duplicar"));
    a->gal_dirty = true;
}

static void sheet_del_cb(lv_event_t *e)
{
    app_t *a = lv_event_get_user_data(e);
    int i = a->sheet_proj;
    if (i < 0 || i >= a->nproj) return;
    if (!a->sheet_confirm) {
        a->sheet_confirm = true;
        lv_obj_t *l = lv_obj_get_child(lv_event_get_current_target(e), 0);
        if (l) lv_label_set_text(l, _("¿Seguro? Tocá de nuevo"));
        return;
    }
    char path[160];
    file_path(a->proj[i].file, path, sizeof path);
    remove(path);
    aos_hal_log(TAG, "deleted %s", path);
    sheet_close(a);
    a->gal_dirty = true;
}

static void sheet_cancel_cb(lv_event_t *e)
{
    sheet_close(lv_event_get_user_data(e));
}

static void gallery_sheet(app_t *a)
{
    const layout_t *L = &a->L;
    project_t *p = &a->proj[a->sheet_proj];
    int32_t w = L->land ? 520 : L->W - 2 * 24, h = 420;
    lv_obj_t *dim = lv_obj_create(a->gal);
    lv_obj_remove_style_all(dim);
    lv_obj_set_size(dim, L->W, L->H);
    lv_obj_set_style_bg_color(dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(dim, LV_OPA_60, 0);
    lv_obj_add_flag(dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(dim, sheet_cancel_cb, LV_EVENT_CLICKED, a);
    a->gal_sheet = dim;
    lv_obj_t *c = dj_ui_card(dim, (L->W - w) / 2, (L->H - h) / 2, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_style_pad_row(c, 12, 0);
    char name[48];
    pretty_name(p->file, name, sizeof name);
    aos_label(c, name, aos_font_body, AOS_C_TEXT);
    int32_t bw = w - 40;
    dj_ui_text_button(c, _("Exportar PNG"), NULL, bw, 72, lv_color_hex(0x0A5A9E), sheet_export_cb, a);
    dj_ui_text_button(c, _("Duplicar"), NULL, bw, 72, C_BTN, sheet_dup_cb, a);
    dj_ui_text_button(c, _("Borrar"), NULL, bw, 72, lv_color_hex(0x8A1E22), sheet_del_cb, a);
    dj_ui_text_button(c, _("Cancelar"), NULL, bw, 72, C_BTN, sheet_cancel_cb, a);
}

static void gallery_build_list(app_t *a)
{
    const layout_t *L = &a->L;
    lv_obj_clean(a->gal_list);
    int cols = L->land ? GAL_COLS_L : GAL_COLS_P;
    int32_t cw = (L->W - 2 * 12 - (cols - 1) * 12) / cols;
    int32_t th = cw - 16;
    if (!a->nproj) {
        lv_obj_t *l = aos_label(a->gal_list, _("Todavía no hay dibujos. Tocá «Nuevo» para empezar uno."),
                                aos_font_body, AOS_C_DIM);
        lv_obj_set_width(l, L->W - 48);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }
    for (int i = 0; i < a->nproj; i++) {
        project_t *p = &a->proj[i];
        lv_obj_t *card = dj_ui_button(a->gal_list, 0, 0, cw, th + 80, C_BTN, card_cb, a);
        lv_obj_add_event_cb(card, card_cb, LV_EVENT_LONG_PRESSED, a);
        lv_obj_set_user_data(card, (void *)(intptr_t)i);
        if (p->prev) {
            lv_obj_t *cv = lv_canvas_create(card);
            lv_canvas_set_buffer(cv, p->prev, p->pw, p->ph, LV_COLOR_FORMAT_RGB565);
            lv_obj_remove_flag(cv, LV_OBJ_FLAG_CLICKABLE);
            /* the preview is 160 px; the card shows it a bit bigger or smaller */
            int32_t scale = (int32_t)(256 * (th - 8) / (p->pw > p->ph ? p->pw : p->ph));
            lv_image_set_scale(cv, scale);
            lv_image_set_antialias(cv, true);
            lv_obj_align(cv, LV_ALIGN_TOP_MID, 0, 8 + (th - 8 - p->ph * scale / 256) / 2);
        } else {
            lv_obj_t *l = aos_label(card, "?", aos_font_title, AOS_C_DIM);
            lv_obj_align(l, LV_ALIGN_TOP_MID, 0, th / 2 - 20);
        }
        char name[48], info[48];
        pretty_name(p->file, name, sizeof name);
        lv_obj_t *l = aos_label_boxed(card, name, aos_font_small, AOS_C_TEXT, cw - 12, 30);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -40);
        snprintf(info, sizeof info, "%d × %d", p->w, p->h);
        l = aos_label_boxed(card, info, aos_font_tiny, AOS_C_DIM, cw - 12, 24);
        lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -12);
    }
}

static void gallery_refresh(app_t *a)
{
    if (a->gal_sheet) {
        lv_obj_delete(a->gal_sheet);
        a->gal_sheet = NULL;
    }
    gallery_scan(a);
    gallery_build_list(a);
}

static void gallery_build(app_t *a, lv_obj_t *root)
{
    const layout_t *L = &a->L;
    a->gal = lv_obj_create(root);
    lv_obj_remove_style_all(a->gal);
    lv_obj_set_size(a->gal, L->W, L->H);
    lv_obj_remove_flag(a->gal, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a->gal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *b = dj_ui_button(a->gal, 12, 8, BAR_H, BAR_H, C_BTN, gal_back_cb, a);
    lv_obj_t *l = aos_label(b, LV_SYMBOL_LEFT, aos_font_body, AOS_C_TEXT);
    lv_obj_center(l);
    l = aos_label(a->gal, _("Dibujo"), aos_font_title, AOS_C_TEXT);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 8 + (BAR_H - 44) / 2);
    int32_t nw = 200;
    b = dj_ui_button(a->gal, L->W - 12 - nw, 8, nw, BAR_H, C_SEL, new_cb, a);
    l = aos_label(b, LV_SYMBOL_PLUS "  ", aos_font_body, AOS_C_TEXT);
    lv_label_set_text_fmt(l, LV_SYMBOL_PLUS "  %s", _("Nuevo"));
    lv_obj_center(l);

    a->gal_list = lv_obj_create(a->gal);
    lv_obj_remove_style_all(a->gal_list);
    lv_obj_set_pos(a->gal_list, 0, 8 + BAR_H + 12);
    lv_obj_set_size(a->gal_list, L->W, L->H - (8 + BAR_H + 12));
    lv_obj_set_flex_flow(a->gal_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_hor(a->gal_list, 12, 0);
    lv_obj_set_style_pad_bottom(a->gal_list, 24, 0);
    lv_obj_set_style_pad_row(a->gal_list, 12, 0);
    lv_obj_set_style_pad_column(a->gal_list, 12, 0);
    lv_obj_set_scroll_dir(a->gal_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(a->gal_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(a->gal_list, LV_OBJ_FLAG_CLICKABLE);
    gallery_build_list(a);
}

/* --------------------------------------------------------------------------
 * The worker: exporting and importing take a second or more on the board,
 * so they run on the other core while a card says so and swallows touches.
 * The document is not touched by anyone else meanwhile: the card is modal
 * and the keyboard and the pad are ignored.
 * -------------------------------------------------------------------------- */

static bool export_rows(void *user, int y, uint8_t *rgba)
{
    if (aos_hal_worker_should_stop()) return false;
    dib_compose_row_rgba((const dib_doc_t *)user, y, rgba);
    return true;
}

static void job_fn(void *arg)
{
    app_t *a = arg;
    bool ok = false;
    if (a->job == JOB_EXPORT) {
        ok = dib_png_write(a->job_path, a->doc->w, a->doc->h, export_rows, a->doc, &a->job_progress);
    } else if (a->job == JOB_IMPORT) {
        int w = 0, h = 0;
        a->job_img = aos_hal_image_decode(a->job_path, 0, 0, a->doc->w, a->doc->h, false, &w, &h, NULL, NULL);
        a->job_w = w;
        a->job_h = h;
        ok = a->job_img != NULL;
    }
    a->job_ok = ok;
    a->job_done = true;
}

static void busy_show(app_t *a, const char *text)
{
    const layout_t *L = &a->L;
    a->busy = lv_obj_create(a->root);
    lv_obj_remove_style_all(a->busy);
    lv_obj_set_size(a->busy, L->W, L->H);
    lv_obj_set_style_bg_color(a->busy, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(a->busy, LV_OPA_50, 0);
    lv_obj_add_flag(a->busy, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *c = dj_ui_card(a->busy, (L->W - 460) / 2, (L->H - 160) / 2, 460, 160);
    a->busy_lbl = aos_label(c, text, aos_font_body, AOS_C_TEXT);
    lv_obj_center(a->busy_lbl);
}

static bool job_start(app_t *a, job_t job, const char *path, const char *text)
{
    if (a->job || aos_hal_worker_running()) return false;
    snprintf(a->job_path, sizeof a->job_path, "%s", path);
    a->job = job;
    a->job_done = false;
    a->job_ok = false;
    a->job_progress = 0;
    a->job_img = NULL;
    if (!aos_hal_worker_start("dibujo", job_fn, a, 12 * 1024)) {
        a->job = JOB_NONE;
        dj_toast(_("Ocupado, probá de nuevo"));
        return false;
    }
    busy_show(a, text);
    return true;
}

static void job_poll(app_t *a)
{
    if (!a->job) return;
    if (!a->job_done) {
        if (a->job == JOB_EXPORT && a->busy_lbl) {
            lv_label_set_text_fmt(a->busy_lbl, "%s %d %%", _("Exportando…"), a->job_progress / 10);
        }
        return;
    }
    aos_hal_worker_stop();
    job_t job = a->job;
    a->job = JOB_NONE;
    if (a->busy) {
        lv_obj_delete(a->busy);
        a->busy = NULL;
        a->busy_lbl = NULL;
    }
    if (job == JOB_EXPORT) {
        const char *name = strrchr(a->job_path, '/');
        char msg[96];
        snprintf(msg, sizeof msg, "%s %s", a->job_ok ? _("Exportado:") : _("No se pudo exportar"),
                 a->job_ok && name ? name + 1 : "");
        aos_hal_log(TAG, "export %s: %s", a->job_ok ? "ok" : "FAILED", a->job_path);
        dj_toast(msg);
    } else if (job == JOB_IMPORT) {
        if (!a->job_ok || !a->doc) {
            dj_toast(_("No se pudo leer la imagen"));
            return;
        }
        /* RGB565 -> ARGB, a floating picture on a new layer at the bottom */
        int w = a->job_w, h = a->job_h;
        dib_px_t *bmp = dib_alloc((size_t)w * h * 4);
        if (!bmp) {
            aos_hal_image_free(a->job_img);
            a->job_img = NULL;
            dj_toast(_("No hay memoria para la imagen"));
            return;
        }
        for (int i = 0; i < w * h; i++) {
            uint16_t c = a->job_img[i];
            unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
            bmp[i] = DIB_ARGB(255, (r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
        }
        aos_hal_image_free(a->job_img);
        a->job_img = NULL;
        if (a->has_obj) dj_ed_obj_commit(a);
        if (dib_layer_add(a->doc, 0, _("Imagen")) < 0) {
            dib_free(bmp);
            dj_toast(_("Ya hay 8 capas"));
            return;
        }
        dj_ed_all_changed(a);
        dj_ed_obj_from_bitmap(a, bmp, w, h, a->doc->w * 0.5f, a->doc->h * 0.5f, false, true);
        aos_hal_log(TAG, "imported %s: %dx%d", a->job_path, w, h);
    }
}

void dj_export_png(app_t *a)
{
    if (!a->doc) return;
    if (a->has_obj) dj_ed_obj_commit(a);
    if (a->dirty) dj_save(a);
    char base[32], path[160];
    snprintf(base, sizeof base, "%s", a->cur_file);
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    snprintf(path, sizeof path, "%s/%s.png", dj_dir(), base);
    job_start(a, JOB_EXPORT, path, _("Exportando…"));
}

void dj_import(app_t *a, const char *path)
{
    if (!a->doc) return;
    if (a->doc->nlayers >= DIB_MAX_LAYERS) {
        dj_toast(_("Ya hay 8 capas"));
        return;
    }
    job_start(a, JOB_IMPORT, path, _("Leyendo la imagen…"));
}

/* --------------------------------------------------------------------------
 * The example drawing, written once on the first visit: a sunset with a
 * house, made with the very tools of the app (soft strokes, shapes, a
 * fill) so it doubles as a test that they all reach the card.
 * -------------------------------------------------------------------------- */

static void seed_stroke(dib_doc_t *d, int kind, float size, float op, dib_px_t c,
                        const float *pts, int n)
{
    dib_brush_t b = { kind, size, op, 0, c, DIB_SYM_NONE };
    dib_stroke_t s;
    if (!dib_stroke_begin(&s, d, &b, pts[0], pts[1])) return;
    for (int i = 1; i < n; i++) dib_stroke_to(&s, pts[2 * i], pts[2 * i + 1]);
    dib_stroke_end(&s);
}

static void seed_obj(dib_doc_t *d, dib_obj_t *o)
{
    dib_obj_render(d, o, NULL);
    dib_flt_commit(d, NULL);
}

static void seed_example(app_t *a)
{
    dib_doc_t *d = dib_doc_new(720, 1280, 0xFFFFFFFFu, a->budget);
    if (!d) return;
    snprintf(d->layers[0].name, DIB_NAME_MAX, "%s", _("Cielo"));
    /* the sky, in bands of soft brush from deep blue to orange */
    static const uint32_t sky[] = { 0x1B2A6B, 0x3B3F8F, 0x7A4D9E, 0xC2557E, 0xF0795A, 0xFBA74C };
    for (int i = 0; i < 6; i++) {
        float y = 60.0f + i * 130.0f;
        const float pts[] = { -80, y, 240, y + 10, 480, y - 10, 800, y };
        seed_stroke(d, DIB_BR_SOFT, 260, 1, 0xFF000000u | sky[i], pts, 4);
    }
    /* the sun, an ellipse with no stroke */
    dib_obj_t o;
    memset(&o, 0, sizeof o);
    o.kind = DIB_OBJ_ELLIPSE;
    o.cx = 360; o.cy = 780; o.w = o.h = 260;
    o.fill = true;
    o.fill_c = 0xFFFFD60A;
    seed_obj(d, &o);

    dib_layer_add(d, -1, _("Paisaje"));
    /* two hills as polygons */
    o.kind = DIB_OBJ_POLY;
    o.fill = true;
    o.stroke = false;
    o.cx = 360; o.cy = 1100; o.w = 900; o.h = 380; o.ang = 0;
    static const dib_pt_t hill[] = { { -0.5f, 0.5f }, { -0.5f, -0.1f }, { -0.3f, -0.4f }, { -0.1f, -0.3f },
                                     { 0.1f, -0.05f }, { 0.3f, -0.2f }, { 0.5f, -0.35f }, { 0.5f, 0.5f } };
    o.npoly = 8;
    memcpy(o.poly, hill, sizeof hill);
    o.fill_c = 0xFF2E6B3A;
    seed_obj(d, &o);
    o.cy = 1180; o.h = 260; o.fill_c = 0xFF3F8F4A;
    o.w = -900;     /* mirrored */
    seed_obj(d, &o);
    /* a house: rectangle, roof, door, window */
    o.kind = DIB_OBJ_RECT;
    o.cx = 470; o.cy = 1010; o.w = 200; o.h = 150; o.w = 200;
    o.fill = o.stroke = true;
    o.stroke_w = 6;
    o.stroke_c = 0xFF3A2418;
    o.fill_c = 0xFFF2E3C6;
    seed_obj(d, &o);
    o.kind = DIB_OBJ_POLY;
    static const dib_pt_t roof[] = { { -0.5f, 0.5f }, { 0, -0.5f }, { 0.5f, 0.5f } };
    o.npoly = 3;
    memcpy(o.poly, roof, sizeof roof);
    o.cx = 470; o.cy = 885; o.w = 250; o.h = 110;
    o.fill_c = 0xFFB22222;
    seed_obj(d, &o);
    o.kind = DIB_OBJ_RECT;
    o.cx = 430; o.cy = 1045; o.w = 50; o.h = 80; o.radius = 6;
    o.fill_c = 0xFF795548;
    seed_obj(d, &o);
    o.cx = 520; o.cy = 1000; o.w = 56; o.h = 50; o.radius = 0;
    o.fill_c = 0xFFFFE07A;
    seed_obj(d, &o);
    /* a few birds, pencil */
    for (int i = 0; i < 3; i++) {
        float x = 150.0f + i * 70.0f, y = 330.0f + (i % 2) * 40.0f;
        const float pts[] = { x - 22, y - 8, x - 10, y - 2, x, y + 4, x + 10, y - 2, x + 22, y - 8 };
        seed_stroke(d, DIB_BR_PENCIL, 5, 1, 0xFF1C1C1E, pts, 5);
    }
    /* the marker underlines a title */
    dib_layer_add(d, -1, _("Texto"));
    const float hl[] = { 120, 200, 600, 200 };
    seed_stroke(d, DIB_BR_MARKER, 40, 0.5f, 0xFFFFD60A, hl, 2);
    dib_px_t *bmp;
    int bw, bh;
    if (dj_text_render(a, _("¡Hola, Dibujo!"), 6, 0xFFFFFFFFu, &bmp, &bw, &bh)) {
        dib_obj_t t;
        memset(&t, 0, sizeof t);
        t.kind = DIB_OBJ_BITMAP;
        t.bmp = bmp;
        t.bw = bw; t.bh = bh;
        t.cx = 360; t.cy = 180; t.w = bw * 1.3f; t.h = bh * 1.3f; t.ang = -0.05f;
        seed_obj(d, &t);
        dib_obj_free(&t);
    }
    d->active = 2;
    dib_hist_clear(d);
    dib_compose(d, (dib_rect_t){ 0, 0, d->w, d->h });
    char path[160], file[32];
    next_name(file, sizeof file);
    file_path(file, path, sizeof path);
    bool ok = dib_save(d, path);
    aos_hal_log(TAG, "example %s: %s", path, ok ? "written" : "FAILED");
    dib_doc_free(d);
}

static bool any_dib(const aos_dir_entry_t *e, void *ctx)
{
    size_t n = strlen(e->name);
    if (!e->dir && e->name[0] != '.' && n > 4 && !strcmp(e->name + n - 4, ".dib")) {
        *(bool *)ctx = true;
        return false;
    }
    return true;
}

static bool folder_has_drawings(void)
{
    bool any = false;
    aos_hal_dir_scan(dj_dir(), any_dib, &any);
    return any;
}

/* --------------------------------------------------------------------------
 * Layout
 * -------------------------------------------------------------------------- */

static void layout_compute(app_t *a, lv_obj_t *root)
{
    layout_t *L = &a->L;
    L->W = lv_obj_get_width(root);
    L->H = lv_obj_get_height(root);
    L->land = L->W > L->H;
    if (!L->land) {
        /* top bar, canvas, options, tools */
        L->top_x = GAP;
        L->top_y = GAP;
        L->top_w = L->W - 2 * GAP;
        L->tool_sz = (L->W - 2 * GAP - (T_COUNT - 1) * 4) / T_COUNT;
        if (L->tool_sz > BAR_H) L->tool_sz = BAR_H;
        L->tool_w = L->W - 2 * GAP;
        L->tool_h = L->tool_sz;
        L->tool_x = GAP;
        L->tool_y = L->H - GAP - L->tool_h;
        L->opt_x = GAP;
        L->opt_w = L->W - 2 * GAP;
        L->opt_y = L->tool_y - GAP - BAR_H;
        L->cv_x = 0;
        L->cv_y = L->top_y + BAR_H + GAP;
        L->cv_w = L->W;
        L->cv_h = L->opt_y - GAP - L->cv_y;
    } else {
        /* the tools in a column on the left; bar on top, options below */
        L->tool_sz = (L->H - 2 * GAP - (T_COUNT - 1) * 4) / T_COUNT;
        if (L->tool_sz > BAR_H) L->tool_sz = BAR_H;
        L->tool_x = GAP;
        L->tool_y = GAP;
        L->tool_w = L->tool_sz;
        L->tool_h = L->H - 2 * GAP;
        int32_t x0 = L->tool_x + L->tool_w + GAP;
        L->top_x = x0;
        L->top_y = GAP;
        L->top_w = L->W - x0 - GAP;
        L->opt_x = x0;
        L->opt_w = L->W - x0 - GAP;
        L->opt_y = L->H - GAP - BAR_H;
        L->cv_x = x0;
        L->cv_y = L->top_y + BAR_H + GAP;
        L->cv_w = L->W - x0;
        L->cv_h = L->opt_y - GAP - L->cv_y;
    }
}

static bool build_ui(app_t *a, lv_obj_t *root)
{
    layout_compute(a, root);
    size_t need = (size_t)a->L.cv_w * a->L.cv_h;
    if (!a->vbuf || a->vbuf_px < need) {
        dib_free(a->vbuf);
        a->vbuf = dib_alloc(need * 2);
        a->vbuf_px = a->vbuf ? need : 0;
        if (!a->vbuf) return false;
    }
    gallery_build(a, root);
    dj_ed_build(a, root);
    if (a->doc) {
        lv_obj_add_flag(a->gal, LV_OBJ_FLAG_HIDDEN);
        dj_ed_enter(a);
    } else {
        lv_obj_add_flag(a->ed, LV_OBJ_FLAG_HIDDEN);
    }
    return true;
}

/* --------------------------------------------------------------------------
 * The frame
 * -------------------------------------------------------------------------- */

static void frame_cb(lv_timer_t *t)
{
    app_t *a = lv_timer_get_user_data(t);
    if (a->closing) return;
    if (a->exit_req) {
        a->exit_req = false;
        a->exiting = true;                  /* so back() lets the system leave */
        aos_ui_back();
        return;
    }
    job_poll(a);
    if (a->sheet_close_req) {
        a->sheet_close_req = false;
        if (a->gal_sheet) {
            lv_obj_delete(a->gal_sheet);
            a->gal_sheet = NULL;
        }
    }
    dj_panel_frame(a);
    dj_text_frame(a);
    if (!a->doc) {
        if (a->gal_dirty) {
            a->gal_dirty = false;
            gallery_refresh(a);
        }
        if (a->size_req >= 0) {
            int i = a->size_req;
            a->size_req = -1;
            if (a->gal_sheet) {
                lv_obj_delete(a->gal_sheet);
                a->gal_sheet = NULL;
            }
            dj_sizes_chosen(a, dj_sizes[i][0], dj_sizes[i][1]);
            return;
        }
        if (a->sheet_req >= 0) {
            a->sheet_proj = a->sheet_req;
            a->sheet_req = -1;
            a->sheet_confirm = false;
            if (!a->gal_sheet && a->sheet_proj < a->nproj) gallery_sheet(a);
        }
        return;
    }
    dj_ed_frame(a);
    uint32_t now = now_ms();
    if (a->dirty && !a->ptr_down && !a->job && now - a->changed_ms > AUTOSAVE_MS) dj_save(a);
}

/* --------------------------------------------------------------------------
 * The keyboard, while the editor is up
 * -------------------------------------------------------------------------- */

static bool hwkbd_cb(uint32_t key, uint8_t mods)
{
    app_t *a = s_app;
    if (!a || a->closing || !a->doc || a->job) return false;
    return dj_ed_key(a, key, mods);
}

void dj_ed_keys_take(app_t *a, bool on)
{
    if (on == a->kb_on) return;
    a->kb_on = on;
    aos_ui_hwkbd_handler(on ? hwkbd_cb : NULL);
}

/* --------------------------------------------------------------------------
 * Life cycle
 * -------------------------------------------------------------------------- */

static size_t pick_budget(void)
{
#ifdef AOS_SIM_BUILTIN
    return 20u << 20;
#else
    aos_sys_stats_t st;
    size_t b = 12u << 20;
    if (aos_hal_sys_stats(&st) && st.psram_total) {
        /* leave room for the system and the other apps that live with us */
        long free_b = (long)st.psram_free - (8L << 20);
        b = free_b > 0 ? (size_t)free_b : 0;
        if (b < (6u << 20)) b = 6u << 20;
        if (b > (20u << 20)) b = 20u << 20;
    }
    return b;
#endif
}

static void free_all(app_t *a)
{
    gallery_free(a);
    dib_free(a->vbuf);
    dib_free(a->xmap);
    dib_free(a->rbuf_top);
    dib_free(a->rbuf_left);
    dib_free(a->sv_buf);
    dib_free(a->hue_buf);
    dib_free(a->clip);
    dib_free(a->imp_files);
    for (int i = 0; i < DIB_MAX_LAYERS; i++) dib_free(a->lthumb[i]);
    if (a->has_obj) dib_obj_free(&a->obj);
    dj_icons_free();
    lv_free(a);
}

static void *dib_create(aos_app_t *self, lv_obj_t *root)
{
    app_t *a = lv_malloc_zeroed(sizeof(app_t));
    if (!a) return NULL;
    s_app = a;
    a->self = self;
    a->root = root;
    a->sheet_proj = a->sheet_req = -1;
    a->size_req = a->imp_req = -1;
    a->budget = pick_budget();
    prefs_load(a);
    palettes_load(a);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);

    bool force = false;
#ifdef AOS_SIM_BUILTIN
    const char *env = getenv("DIB_DEMO");
    force = env && env[0];
#endif
    if (force || (!pref_i(KEY_SEEDED, 0) && !folder_has_drawings())) {
        if (!folder_has_drawings()) seed_example(a);
        aos_hal_pref_set_i32(KEY_SEEDED, 1);
    }
    gallery_scan(a);
    if (!build_ui(a, root)) {
        aos_hal_log(TAG, "out of memory for the canvas window");
        lv_obj_clean(root);
        free_all(a);
        s_app = NULL;
        return NULL;
    }
    a->frame = lv_timer_create(frame_cb, FRAME_MS, a);
    aos_hal_log(TAG, "budget %u MB", (unsigned)(a->budget >> 20));

#ifdef AOS_SIM_BUILTIN
    /* Development switches, simulator only (on the board getenv() is NULL):
     *   DIB_OPEN=n     open drawing n of the gallery (1 = the newest)
     *   DIB_NEW=WxH    a new drawing of that size
     *   DIB_TOOL=t     that tool (0..7)
     *   DIB_PANEL=p    that panel open (1 brushes, 2 colour, 3 layers, 4 menu, 5 shapes)
     *   DIB_SIZES=1    the size chooser
     *   DIB_SHEET=n    the actions of drawing n in the gallery
     *   DIB_KEYS=...   keys typed into the editor, ',' between them: z, ^z
     *                  (ctrl), +, -, [, ], ...
     *   DIB_WHEEL=n    the mouse wheel, n notches at the window's centre
     *   DIB_SCRIPT=... editor actions (dib_editor.c, sim_script) */
    env = getenv("DIB_OPEN");
    if (env && env[0]) {
        int i = atoi(env) - 1;
        if (i >= 0 && i < a->nproj) {
            char file[32];
            snprintf(file, sizeof file, "%s", a->proj[i].file);
            open_file(a, file);
        }
    }
    env = getenv("DIB_NEW");
    if (env && env[0]) {
        int w = 0, h = 0;
        if (sscanf(env, "%dx%d", &w, &h) == 2) dj_sizes_chosen(a, w, h);
    }
    env = getenv("DIB_SIZES");
    if (env && env[0]) dj_sizes_open(a);
    env = getenv("DIB_SHEET");
    if (env && env[0] && !a->doc) a->sheet_req = atoi(env) - 1;
#endif
    return a;
}

static bool dib_resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    app_t *a = inst;
    if (!a) return false;
    if (a->job) return false;               /* not now: be created again */
    panel_t open_panel = a->panel;          /* an open panel opens again, laid out anew */
    dj_ed_resize_cleanup(a);
    a->closing = true;
    lv_obj_clean(root);
    a->closing = false;
    a->gal_sheet = NULL;
    a->busy = NULL;
    a->pnl = NULL;
    a->panel = P_NONE;
    a->txt_card = a->txt_kb = a->txt_ta = NULL;
    a->txt_canvas = NULL;
    if (!build_ui(a, root)) return false;
    if (a->doc && open_panel != P_NONE && open_panel != P_IMPORT) a->pnl_open_req = open_panel;
    return true;
}

static void dib_destroy(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = inst;
    if (!a) return;
    if (a->frame) lv_timer_delete(a->frame);
    a->frame = NULL;
    if (a->job) {
        aos_hal_worker_stop();
        if (a->job_img) aos_hal_image_free(a->job_img);
        a->job = JOB_NONE;
    }
    dj_ed_keys_take(a, false);
    if (a->doc) {
        if (a->grab == G_STROKE) dib_stroke_end(&a->stroke);
        doc_close(a);
    }
    prefs_save(a);
    a->closing = true;
    if (a->root) lv_obj_clean(a->root);
    free_all(a);
    if (s_app == a) s_app = NULL;
}

static void dib_hide(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = inst;
    if (!a) return;
    if (a->doc && a->grab == G_STROKE) {
        dib_stroke_end(&a->stroke);
        a->grab = G_NONE;
        dj_mark_dirty(a);
    }
    a->ptr_down = false;
    dj_ed_keys_take(a, false);
    if (a->dirty && !a->job) dj_save(a);
    prefs_save(a);
    if (a->frame && !a->job) lv_timer_pause(a->frame);
}

static void dib_show(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = inst;
    if (!a) return;
    if (a->frame) lv_timer_resume(a->frame);
    if (a->doc) dj_ed_keys_take(a, !a->txt_card);
    else if (!a->gal_sheet) a->gal_dirty = true;    /* the portal may have changed the folder */
}

static bool dib_back(aos_app_t *self, void *inst)
{
    (void)self;
    app_t *a = inst;
    if (!a || a->exiting) return false;
    if (a->job) return true;
    if (a->gal_sheet) {
        sheet_close(a);
        return true;
    }
    if (a->panel != P_NONE) {
        dj_panel_close(a);
        return true;
    }
    if (a->doc) return dj_ed_back(a);
    return false;
}

/* The icon: a brush over a palette. */
static const uint8_t DIB_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -2, 4, 62, 50, AIC_CIRCLE, AIC_C_TEXT, 255),           /* palette */
    AIC_INTO,
    AIC_RECT(AIC_CENTER, -16, -8, 11, 11, AIC_CIRCLE, AIC_C_RED, 255),
    AIC_RECT(AIC_CENTER, 0, -14, 11, 11, AIC_CIRCLE, AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER, 16, -8, 11, 11, AIC_CIRCLE, AIC_C_GREEN, 255),
    AIC_RECT(AIC_CENTER, -18, 8, 11, 11, AIC_CIRCLE, AIC_C_ACCENT, 255),
    AIC_RECT(AIC_CENTER, 10, 10, 13, 13, AIC_CIRCLE, AIC_C_LIT(0x7A2E8C), 255),  /* thumb hole */
    AIC_OUT,
    AIC_RECT(AIC_CENTER, 22, -16, 7, 40, 4, AIC_C_LIT(0x6B4226), 255),         /* brush */
    AIC_ROT(450),
    AIC_END
};

static bool dib_init(aos_app_t *app)
{
    app->desc.id       = "aos.dibujo";         /* before the icon: it is keyed by id */
    app->desc.name     = "Dibujo";
    app->desc.icon     = LV_SYMBOL_EDIT;       /* the fallback, if the blob were refused */
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0xF0795A;
    app->desc.color_b  = 0x7A2E8C;
    app->desc.order    = 80;
    /* NO_SWIPE and LONG_DRAG: a stroke is a long drag and neither the edge's
     * back gesture nor lv_indev_wait_release() may cut it; back is the
     * arrow in the bar. KEEP: a drawing half done, its tool and its zoom are
     * worth finding again. */
    app->desc.flags    = AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG | AOS_APP_FLAG_KEEP;
    aos_icon_set_ops(app, DIB_ICON, sizeof DIB_ICON);

    app->create  = dib_create;
    app->destroy = dib_destroy;
    app->show    = dib_show;
    app->hide    = dib_hide;
    app->back    = dib_back;
    app->resize  = dib_resize;
    return true;
}

AOS_APP_ENTRY(dib_init);
