/*
 * Estudio - the shell of the interface: the bar with the transport and the
 * views, the timers, the project on the card, and the small widgets every
 * view builds with.
 */
#include "es_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

es_ui_t *es_ui;
#define U es_ui

/* ---- colours and names ---- */

uint32_t ui_part_color(int part)
{
    static const uint32_t C[ES_PARTS] = { 0x0A84FF, 0x30D158, 0xBF5AF2 };
    return C[part < 0 || part >= ES_PARTS ? 0 : part];
}

uint32_t ui_drum_color(int track)
{
    static const uint32_t C[ES_DRUMS] = { 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x40C8E0, 0x64D2FF,
                                          0xFF6482, 0x5E5CE6, 0xFF375F };
    return C[track & 7];
}

const char *ui_track_name(int track, bool shortname)
{
    static const char *const SHORT[DS_COUNT] = {
        N_("BOM"), N_("808"), N_("RED"), N_("APL"), N_("HHC"), N_("HHA"), N_("TG"), N_("TA"),
        N_("PLA"), N_("CEN"), N_("CLV"), N_("ARO"), N_("SHK"),
    };
    const es_dtrack_t *d = &U->p->dt[track];
    if (d->src >= DS_COUNT) {
        static char buf[ES_DRUMS][24];
        const char *n = d->sample;
        size_t l = strlen(n);
        if (l > 4 && !strcasecmp(n + l - 4, ".wav")) l -= 4;
        size_t max = shortname ? 4 : 18;
        if (l > max) l = max;
        memcpy(buf[track], n, l);
        buf[track][l] = 0;
        return buf[track];
    }
    return shortname ? _(SHORT[d->src]) : _(ES_DRUM_NAMES[d->src]);
}

int ui_pat_used(const es_pat_t *t)
{
    int n = t->nn[0] + t->nn[1] + t->nn[2];
    for (int tr = 0; tr < ES_DRUMS; tr++)
        for (int s = 0; s < ES_STEPS; s++) if (t->d[tr][s].vel) n++;
    return n;
}

/* ---- widgets ---- */

lv_obj_t *ui_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_col(lv_obj_t *parent, int32_t w, int32_t gap)
{
    lv_obj_t *o = ui_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

lv_obj_t *ui_row(lv_obj_t *parent, int32_t w, int32_t gap)
{
    lv_obj_t *o = ui_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(o, gap, 0);
    lv_obj_set_style_pad_row(o, gap, 0);
    return o;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *txt, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

lv_obj_t *ui_btn(lv_obj_t *parent, const char *txt, const lv_font_t *f, int32_t w, int32_t h,
                 lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CELL), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt ? txt : "");
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_TEXT), 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

void ui_btn_on(lv_obj_t *b, bool on, uint32_t color)
{
    if (!b) return;
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? color : C_CELL), 0);
    lv_obj_t *l = lv_obj_get_child(b, 0);
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(on && color != C_CELL2 ? 0x000000 : C_TEXT), 0);
}

void ui_btn_text(lv_obj_t *b, const char *txt)
{
    lv_obj_t *l = b ? lv_obj_get_child(b, 0) : NULL;
    if (l) lv_label_set_text(l, txt);
}

lv_obj_t *ui_seg(lv_obj_t *parent, const char *const *opts, int n, int sel, int32_t w, int32_t h,
                 lv_event_cb_t cb, uint32_t color)
{
    lv_obj_t *row = ui_box(parent, w, h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);
    int32_t bw = (w - 4 * (n - 1)) / n;
    for (int i = 0; i < n; i++) {
        const lv_font_t *f = aos_font_caption;
        lv_obj_t *b = ui_btn(row, opts[i], f, bw, h, NULL, NULL);
        lv_obj_set_style_radius(b, 10, 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(l, bw - 6, lv_font_get_line_height(f));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        ui_btn_on(b, i == sel, color);
    }
    return row;
}

void ui_seg_set(lv_obj_t *seg, int sel, uint32_t color)
{
    uint32_t n = lv_obj_get_child_count(seg);
    for (uint32_t i = 0; i < n; i++) ui_btn_on(lv_obj_get_child(seg, (int32_t)i), (int)i == sel, color);
}

lv_obj_t *ui_card(lv_obj_t *parent, const char *title, int32_t w)
{
    lv_obj_t *c = ui_col(parent, w, 8);
    lv_obj_set_style_bg_color(c, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, 16, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    if (title) ui_label(c, title, aos_font_small, C_DIM);
    return c;
}

void ui_fmt(int fmt, float v, char *out, size_t n)
{
    switch (fmt) {
    case U_PCT:  snprintf(out, n, "%d %%", (int)roundf(v * 100.0f)); break;
    case U_SEMI: snprintf(out, n, "%+d", (int)roundf(v)); break;
    case U_X:    snprintf(out, n, "x%.2f", (double)v); break;
    case U_NONE: snprintf(out, n, "%d", (int)roundf(v)); break;
    case U_MFILT:
        if (fabsf(v) <= 0.02f) snprintf(out, n, "%s", _("abierto"));
        else if (v < 0) snprintf(out, n, "LP %d Hz", (int)(20000.0f * exp2f(v * 9.0f)));
        else snprintf(out, n, "HP %d Hz", (int)(20.0f * exp2f(v * 9.5f)));
        break;
    default:     snprintf(out, n, "%.2f", (double)v); break;
    }
}

static void bind_text(es_bind_t *b)
{
    char buf[32];
    if (b->param >= 0) es_param_format(b->param, *b->v, buf, sizeof buf);
    else ui_fmt(b->fmt, *b->v, buf, sizeof buf);
    if (b->val) lv_label_set_text(b->val, buf);
}

static void slider_cb(lv_event_t *e)
{
    es_bind_t *b = lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target_obj(e);
    float t = (float)lv_slider_get_value(s) / 1000.0f;
    if (b->param >= 0) *b->v = es_param_from01(b->param, t);
    else *b->v = b->min + (b->max - b->min) * t;
    bind_text(b);
    if (b->after) b->after();
    es_ui_dirty();
}

static es_bind_t *bind_new(void)
{
    if (U->nbinds >= ES_BINDS) return NULL;
    es_bind_t *b = &U->binds[U->nbinds++];
    memset(b, 0, sizeof *b);
    b->param = -1;
    return b;
}

static lv_obj_t *slider_row(lv_obj_t *parent, const char *name, es_bind_t *b, float t, int32_t w)
{
    lv_obj_t *col = ui_col(parent, w, 2);
    lv_obj_t *top = ui_box(col, w, 30);
    lv_obj_t *nl = ui_label(top, name, aos_font_caption, C_TEXT);
    lv_obj_align(nl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_size(nl, w * 7 / 10, lv_font_get_line_height(aos_font_caption));
    b->val = ui_label(top, "", aos_font_caption, C_DIM);
    lv_obj_align(b->val, LV_ALIGN_RIGHT_MID, -4, 0);
    lv_obj_t *s = lv_slider_create(col);
    lv_obj_set_size(s, w - 28, 18);
    lv_obj_set_style_margin_left(s, 14, 0);
    lv_obj_set_style_margin_top(s, 6, 0);
    lv_obj_set_style_margin_bottom(s, 8, 0);
    lv_slider_set_range(s, 0, 1000);
    lv_slider_set_value(s, (int32_t)roundf(t * 1000.0f), LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s, 16);
    lv_obj_add_event_cb(s, slider_cb, LV_EVENT_VALUE_CHANGED, b);
    bind_text(b);
    return s;
}

lv_obj_t *ui_slider(lv_obj_t *parent, const char *name, float *v, float min, float max, int fmt,
                    int32_t w, void (*after)(void))
{
    es_bind_t *b = bind_new();
    if (!b) return NULL;
    b->v = v; b->min = min; b->max = max; b->fmt = (uint8_t)fmt; b->after = after;
    return slider_row(parent, name, b, (*v - min) / (max - min), w);
}

static void enum_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target_obj(e);
    lv_obj_t *seg = lv_obj_get_parent(btn);
    es_bind_t *b = lv_obj_get_user_data(seg);
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    *b->v = ES_PDESC[b->param].min + (float)i;
    ui_seg_set(seg, i, aos_theme_accent_rgb());
    if (b->after) b->after();
    es_ui_dirty();
}

lv_obj_t *ui_param(lv_obj_t *parent, int param, float *v, int32_t w, void (*after)(void))
{
    const es_pdesc_t *d = &ES_PDESC[param];
    es_bind_t *b = bind_new();
    if (!b) return NULL;
    b->v = v; b->param = param; b->after = after;
    if (d->kind != PK_ENUM) return slider_row(parent, _(d->name), b, es_param_to01(param, *v), w);
    lv_obj_t *col = ui_col(parent, w, 4);
    ui_label(col, _(d->name), aos_font_caption, C_TEXT);
    const char *opts[8];
    int n = 0;
    for (; d->opts[n] && n < 8; n++) opts[n] = _(d->opts[n]);
    lv_obj_t *seg = ui_seg(col, opts, n, (int)roundf(*v - d->min), w, 48, enum_cb, aos_theme_accent_rgb());
    lv_obj_set_user_data(seg, b);
    return seg;
}

lv_obj_t *ui_stepper(lv_obj_t *parent, const char *txt, int32_t w, int32_t h, lv_event_cb_t minus,
                     lv_event_cb_t plus, lv_obj_t **label)
{
    lv_obj_t *row = ui_box(parent, w, h);
    lv_obj_t *m = ui_btn(row, AOS_SYM_MINUS, &aos_sym_28, h, h, minus, NULL);
    lv_obj_align(m, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(m, minus, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *p = ui_btn(row, AOS_SYM_PLUS, &aos_sym_28, h, h, plus, NULL);
    lv_obj_align(p, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(p, plus, LV_EVENT_LONG_PRESSED_REPEAT, NULL);
    lv_obj_t *l = ui_label(row, txt, aos_font_small, C_TEXT);
    lv_obj_center(l);
    if (label) *label = l;
    return row;
}

/* ---- sheets ---- */

static void sheet_kill(void *o)
{
    lv_obj_delete(o);
}

void ui_sheet_close(void)
{
    if (!U->sheet) return;
    lv_obj_add_flag(U->sheet, LV_OBJ_FLAG_HIDDEN);
    lv_async_call(sheet_kill, U->sheet);
    U->sheet = NULL;
}

/* a rebuild of the whole screen must not leave a delete pending on an
 * object that lv_obj_clean() already took */
static void sheet_forget(void)
{
    if (!U->sheet) return;
    lv_async_call_cancel(sheet_kill, U->sheet);
    U->sheet = NULL;
}

static void sheet_close_cb(lv_event_t *e)
{
    (void)e;
    ui_sheet_close();
}

static void sheet_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == lv_event_get_current_target_obj(e)) ui_sheet_close();
}

lv_obj_t *ui_sheet(const char *title)
{
    ui_sheet_close();
    es_play_release_all();
    lv_obj_t *o = lv_obj_create(U->root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, U->W, U->H);
    lv_obj_set_pos(o, 0, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_60, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, sheet_bg_cb, LV_EVENT_CLICKED, NULL);
    U->sheet = o;
    int32_t w = U->W - 40 < 760 ? U->W - 40 : 760;
    int32_t h = U->H - 60;
    lv_obj_t *card = ui_box(o, w, h);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_center(card);
    lv_obj_t *t = ui_label(card, title, aos_font_body, C_TEXT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 22, 18);
    lv_obj_t *x = ui_btn(card, AOS_SYM_CLOSE, &aos_sym_28, 56, 56, sheet_close_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_t *c = lv_obj_create(card);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w - 24, h - 86);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_style_pad_bottom(c, 12, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    lv_obj_update_layout(o);            /* the callers size things from it */
    return c;
}

static void (*s_text_done)(const char *);
static lv_obj_t *s_ta;

static void ask_ok_cb(lv_event_t *e)
{
    (void)e;
    char buf[ES_NAME];
    snprintf(buf, sizeof buf, "%s", lv_textarea_get_text(s_ta));
    void (*done)(const char *) = s_text_done;
    ui_sheet_close();
    if (done && buf[0]) done(buf);
}

static void kb_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_READY) ask_ok_cb(e);
    else if (lv_event_get_code(e) == LV_EVENT_CANCEL) ui_sheet_close();
}

void ui_ask_text(const char *title, const char *init, void (*done)(const char *text))
{
    lv_obj_t *c = ui_sheet(title);
    lv_obj_t *card = lv_obj_get_parent(c);
    s_text_done = done;
    s_ta = lv_textarea_create(c);
    lv_obj_set_width(s_ta, lv_pct(100));
    lv_textarea_set_one_line(s_ta, true);
    lv_textarea_set_max_length(s_ta, ES_NAME - 8);
    lv_textarea_set_text(s_ta, init ? init : "");
    lv_obj_set_style_text_font(s_ta, aos_font_body, 0);
    lv_obj_t *ok = ui_btn(c, _("Listo"), aos_font_body, 200, 60, ask_ok_cb, NULL);
    ui_btn_on(ok, true, aos_theme_accent_rgb());
    lv_obj_t *kb = lv_keyboard_create(card);
    aos_keyboard_style(kb, aos_font_body);
    lv_obj_set_size(kb, lv_obj_get_width(card), U->land ? lv_obj_get_height(card) * 55 / 100 : 420);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(kb, s_ta);
    lv_obj_add_event_cb(kb, kb_cb, LV_EVENT_ALL, NULL);
}

static void (*s_yes)(void);

static void confirm_cb(lv_event_t *e)
{
    (void)e;
    void (*y)(void) = s_yes;
    ui_sheet_close();
    if (y) y();
}

void ui_confirm(const char *text, const char *ok, void (*yes)(void))
{
    lv_obj_t *c = ui_sheet(_("¿Seguro?"));
    s_yes = yes;
    lv_obj_t *l = ui_label(c, text, aos_font_body, C_TEXT);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_t *b = ui_btn(c, ok, aos_font_body, 260, 64, confirm_cb, NULL);
    ui_btn_on(b, true, C_REC);
}

/* ---- the project ---- */

void es_ui_dirty(void)
{
    if (!U->dirty) U->dirty_ms = (uint32_t)aos_hal_uptime_ms();
    U->dirty = true;
}

void es_ui_project_save(void)
{
    if (!U || !U->p) return;
    char path[320];
    es_path(path, sizeof path, NULL, U->file);
    if (es_proj_save(U->p, path)) U->dirty = false;
    else aos_hal_log(ES_TAG, "could not save %s", path);
    aos_hal_pref_set_str("es_proj", U->file);
}

/* the file name for a project name: letters, digits and a few signs */
static void file_for(const char *name, char *out, size_t n)
{
    size_t j = 0;
    for (const char *s = name; *s && j + 6 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')
            out[j++] = (char)c;
        else if (c == ' ') out[j++] = '_';
        else if (c >= 0x80) { /* an accented letter: keep its UTF-8 bytes */ out[j++] = (char)c; }
    }
    if (!j) { snprintf(out, n, "proyecto.json"); return; }
    snprintf(out + j, n - j, ".json");
}

void es_ui_preload_samples(void)
{
    for (int t = 0; t < ES_DRUMS; t++) {
        es_dtrack_t *d = &U->p->dt[t];
        if (d->src < DS_COUNT) continue;
        char err[64];
        if (!es_bank_load(d->sample, err, sizeof err))
            aos_hal_log(ES_TAG, "sample %s: %s", d->sample, err);
    }
}

static void project_loaded(void)
{
    U->pat = 0;
    U->page = 0;
    es_cmd(U->e, EC_PANIC, 0, 0, 0, 0);
    es_cmd(U->e, EC_CUE_PAT, 0, 0, 0, 0);
    es_ui_preload_samples();
    for (int i = 0; i < ES_PARTS; i++) U->roll_low[i] = i == 0 ? 28 : 48;
}

void es_ui_project_new(void)
{
    if (U->dirty) es_ui_project_save();
    es_proj_default(U->p);
    /* the first free "Proyecto N" */
    for (int i = 1; i < 1000; i++) {
        char nm[ES_NAME], f[ES_NAME + 8], path[320];
        snprintf(nm, sizeof nm, "%s %d", _("Proyecto"), i);
        file_for(nm, f, sizeof f);
        es_path(path, sizeof path, NULL, f);
        FILE *x = fopen(path, "rb");
        if (x) { fclose(x); continue; }
        snprintf(U->p->name, sizeof U->p->name, "%s", nm);
        snprintf(U->file, sizeof U->file, "%s", f);
        break;
    }
    project_loaded();
    es_ui_project_save();
}

bool es_ui_project_open(const char *file)
{
    char path[320], err[64];
    es_path(path, sizeof path, NULL, file);
    es_proj_t *tmp = es_alloc(sizeof *tmp);
    if (!tmp) return false;
    if (!es_proj_load(tmp, path, err, sizeof err)) {
        es_free(tmp);
        return false;
    }
    if (U->dirty) es_ui_project_save();
    es_cmd(U->e, EC_PANIC, 0, 0, 0, 0);
    aos_hal_sleep_ms(5);
    memcpy(U->p, tmp, sizeof *tmp);
    es_free(tmp);
    snprintf(U->file, sizeof U->file, "%s", file);
    project_loaded();
    aos_hal_pref_set_str("es_proj", U->file);
    return true;
}

/* ---- settings ---- */

void es_ui_apply_settings(void)
{
    es_set_latency(U->e, U->latency);
    es_set_voices(U->e, U->voices);
    es_set_mout(U->e, U->mo_dest, U->mo_clock != 0, U->mo_pc);
    es_set_live_part(U->e, U->part);
    aos_hal_pref_set_i32("es_lat", U->latency);
    aos_hal_pref_set_i32("es_voices", U->voices);
    aos_hal_pref_set_i32("es_mout", U->mo_dest | (U->mo_clock ? 16 : 0) | (U->mo_pc << 8));
    /* the computer only hears the board while the port is the keyboard and
     * MIDI device (the Macro pad's mode); an idle port becomes one */
    if ((U->mo_dest & 1) && aos_hal_usb_mode() == AOS_HAL_USB_CONSOLE) aos_hal_usb_mode_set(AOS_HAL_USB_KEYS);
}

/* ---- the bar ---- */

static void play_cb(lv_event_t *e)
{
    (void)e;
    es_cmd(U->e, es_playing(U->e) ? EC_STOP : EC_PLAY, 0, 0, 0, 0);
}

static void rec_cb(lv_event_t *e)
{
    (void)e;
    es_cmd(U->e, EC_REC, !es_recording(U->e), 0, 0, 0);
}

void es_ui_bpm_set(float bpm)
{
    U->p->bpm = es_clampf(roundf(bpm), 40, 240);
    es_ui_dirty();
}

static void bpm_minus(lv_event_t *e) { (void)e; es_ui_bpm_set(U->p->bpm - 1); }
static void bpm_plus(lv_event_t *e)  { (void)e; es_ui_bpm_set(U->p->bpm + 1); }

/* tap tempo: the mean of the last taps, starting over after a pause */
static void tap_cb(lv_event_t *e)
{
    (void)e;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (U->ntap && now - U->tap_ms[U->ntap - 1] > 2000) U->ntap = 0;
    if (U->ntap == 6) { memmove(U->tap_ms, U->tap_ms + 1, 5 * sizeof U->tap_ms[0]); U->ntap = 5; }
    U->tap_ms[U->ntap++] = now;
    if (U->ntap >= 2) {
        float ms = (float)(U->tap_ms[U->ntap - 1] - U->tap_ms[0]) / (float)(U->ntap - 1);
        if (ms > 0) es_ui_bpm_set(60000.0f / ms);
    }
}

static void met_cb(lv_event_t *e)
{
    (void)e;
    U->p->met_on = !U->p->met_on;
    ui_btn_on(U->b_met, U->p->met_on, 0xFFD60A);
    es_ui_dirty();
}

static void tab_cb(lv_event_t *e)
{
    es_ui_show_view((int)(intptr_t)lv_event_get_user_data(e));
}

static void bar_refresh(void)
{
    int st = (es_playing(U->e) ? 1 : 0) | (es_recording(U->e) ? 2 : 0) | (es_countin(U->e) ? 4 : 0);
    if ((st & 4) && ((aos_hal_uptime_ms() / 250) & 1)) st |= 8;     /* the count-in blinks */
    if (st != U->shown_state) {
        U->shown_state = st;
        ui_btn_on(U->b_play, st & 1, C_PLAY);
        ui_btn_text(U->b_play, (st & 1) ? AOS_SYM_STOP : AOS_SYM_PLAY);
        ui_btn_on(U->b_rec, (st & 2) && !(st & 8), C_REC);
    }
    int bpm = (int)roundf(U->p->bpm);
    if (bpm != U->shown_bpm) {
        U->shown_bpm = bpm;
        lv_label_set_text_fmt(U->l_bpm, "%d", bpm);
    }
    if (U->meter) {
        es_stats_t s;
        es_stats(U->e, &s);
        int v = (int)(s.peak * 100.0f);
        lv_bar_set_value(U->meter, v > 100 ? 100 : v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(U->meter, lv_color_hex(s.peak > 0.94f ? C_REC : C_PLAY), LV_PART_INDICATOR);
    }
}

static void build_bar(void)
{
    static const char *const TABS[V_COUNT] = { N_("Tocar"), N_("Ritmo"), N_("Notas"), N_("Sonido"),
                                               N_("Mezcla"), N_("Canción") };
    int32_t bh = U->land ? 76 : 150;
    U->bar = ui_box(U->root, U->W, bh);
    lv_obj_set_style_bg_color(U->bar, lv_color_hex(C_PANEL), 0);
    lv_obj_set_style_bg_opa(U->bar, LV_OPA_COVER, 0);
    int32_t h = 60, y = 8, x = 12;
    U->b_play = ui_btn(U->bar, AOS_SYM_PLAY, &aos_sym_44, 84, h, play_cb, NULL);
    lv_obj_set_pos(U->b_play, x, y);
    x += 92;
    U->b_rec = ui_btn(U->bar, AOS_SYM_RECORD, &aos_sym_44, 72, h, rec_cb, NULL);
    lv_obj_set_pos(U->b_rec, x, y);
    x += 80;
    lv_obj_t *st = ui_stepper(U->bar, "120", 200, h, bpm_minus, bpm_plus, &U->l_bpm);
    lv_obj_set_pos(st, x, y);
    lv_obj_set_style_text_font(U->l_bpm, aos_font_body, 0);
    x += 208;
    lv_obj_t *tap = ui_btn(U->bar, AOS_SYM_GESTURE_TAP_BUTTON, &aos_sym_28, 68, h, NULL, NULL);
    lv_obj_add_event_cb(tap, tap_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_set_pos(tap, x, y);
    x += 76;
    U->b_met = ui_btn(U->bar, AOS_SYM_TIMER_OUTLINE, &aos_sym_28, 68, h, met_cb, NULL);
    lv_obj_set_pos(U->b_met, x, y);
    ui_btn_on(U->b_met, U->p->met_on, 0xFFD60A);
    x += 80;
    /* the output level, small, after the transport */
    U->meter = lv_bar_create(U->bar);
    lv_obj_set_size(U->meter, 10, h - 8);
    lv_obj_set_pos(U->meter, x, y + 4);
    lv_bar_set_range(U->meter, 0, 100);
    lv_obj_set_style_bg_color(U->meter, lv_color_hex(C_CELL), 0);
    lv_obj_set_style_radius(U->meter, 4, 0);
    lv_obj_set_style_radius(U->meter, 4, LV_PART_INDICATOR);

    int32_t tw = U->land ? 104 : (U->W - 24 - 5 * 6) / 6;
    int32_t tx = U->land ? U->W - 12 - 6 * tw - 5 * 6 : 12, ty = U->land ? y : y + h + 12;
    for (int i = 0; i < V_COUNT; i++) {
        U->tab[i] = ui_btn(U->bar, _(TABS[i]), aos_font_caption, tw, h, tab_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(U->tab[i], tx + i * (tw + 6), ty);
        ui_btn_on(U->tab[i], i == U->view, C_CELL2);
        if (i == U->view) lv_obj_set_style_border_width(U->tab[i], 2, 0);
        lv_obj_set_style_border_color(U->tab[i], AOS_C_ACCENT, 0);
    }
    U->shown_bpm = -1;
    U->shown_state = -1;
    bar_refresh();
}

/* ---- the views ---- */

static void body_build(void)
{
    if (U->forget) U->forget();
    U->forget = NULL;
    U->refresh = NULL;
    U->grid = NULL;
    U->nbinds = 0;
    es_play_release_all();
    if (U->body) lv_obj_delete(U->body);
    int32_t bh = U->land ? 76 : 150;
    U->body = ui_box(U->root, U->W, U->H - bh);
    lv_obj_set_pos(U->body, 0, bh);
    lv_obj_update_layout(U->body);      /* the views read its size right away */
    switch (U->view) {
    case V_PLAY:  es_view_play(U->body); break;
    case V_DRUMS: es_view_drums(U->body); break;
    case V_ROLL:  es_view_roll(U->body); break;
    case V_SOUND: es_view_sound(U->body); break;
    case V_MIX:   es_view_mix(U->body); break;
    default:      es_view_song(U->body); break;
    }
    if (U->sheet) lv_obj_move_to_index(U->sheet, -1);
}

void es_ui_show_view(int v)
{
    if (v < 0 || v >= V_COUNT) v = V_PLAY;
    U->view = v;
    for (int i = 0; i < V_COUNT; i++) {
        ui_btn_on(U->tab[i], i == v, C_CELL2);
        lv_obj_set_style_border_width(U->tab[i], i == v ? 2 : 0, 0);
    }
    aos_hal_pref_set_i32(U->land ? "es_view_l" : "es_view_p", v);
    body_build();
}

void es_ui_select_pat(int pat)
{
    U->pat = pat & 7;
    if (U->page * 16 >= U->p->pat[U->pat].len) U->page = 0;
    es_cmd(U->e, EC_CUE_PAT, U->pat, 0, 0, 0);
}

void es_ui_build(void)
{
    sheet_forget();
    if (U->forget) U->forget();
    U->forget = NULL;
    U->refresh = NULL;
    U->grid = NULL;
    U->body = NULL;
    lv_obj_clean(U->root);
    lv_obj_update_layout(U->root);
    U->W = lv_obj_get_width(U->root);
    U->H = lv_obj_get_height(U->root);
    U->land = U->W > U->H;
    lv_obj_set_style_bg_color(U->root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(U->root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(U->root, LV_OBJ_FLAG_SCROLLABLE);
    build_bar();
    body_build();
}

/* ---- the timers ---- */

static void take_events(void)
{
    es_ev_t ev;
    bool any = false;
    while (es_ev_take(U->e, &ev)) {
        es_pat_t *t = &U->p->pat[ev.pat & 7];
        switch (ev.type) {
        case EV_REC_ON:
            if (ev.a < ES_PARTS) es_pat_add_note(t, ev.a, ev.step, 1, ev.b, ev.c);
            break;
        case EV_REC_OFF: {
            if (ev.a >= ES_PARTS) break;
            int i = es_pat_find_note(t, ev.a, ev.step, ev.b);
            if (i >= 0) t->n[ev.a][i].len = ev.c;
            break;
        }
        case EV_REC_DRUM:
            if (ev.a < ES_DRUMS && ev.step < ES_STEPS) t->d[ev.a][ev.step] = (es_dstep_t){ ev.c ? ev.c : 100, 100 };
            break;
        }
        any = true;
    }
    if (any) {
        es_ui_dirty();
        if (U->grid) lv_obj_invalidate(U->grid);
    }
}

static void ui_timer(lv_timer_t *t)
{
    (void)t;
    if (!U || !U->bar) return;
    take_events();
    bar_refresh();
    /* in song mode the views follow the pattern that plays */
    if (U->p->song_mode && es_playing(U->e) && es_play_pat(U->e) != U->pat) {
        U->pat = es_play_pat(U->e);
        if (U->page * 16 >= U->p->pat[U->pat].len) U->page = 0;
    }
    if (U->refresh) U->refresh();
    if (U->load_test_until && (int32_t)((uint32_t)aos_hal_uptime_ms() - U->load_test_until) >= 0) {
        U->load_test_until = 0;
        es_cmd(U->e, EC_PANIC, 0, 0, 0, 0);
        for (int i = 0; i < ES_PARTS; i++) U->p->part[i].p[P_MONO] = U->load_test_mono[i];
    }
}

static void touch_timer(lv_timer_t *t)
{
    (void)t;
    if (U && U->view == V_PLAY) es_play_touch();
}

void es_ui_timers(bool on);
void es_ui_timers(bool on)
{
    if (on) {
        if (!U->t_ui) U->t_ui = lv_timer_create(ui_timer, 33, NULL);
        if (!U->t_touch) U->t_touch = lv_timer_create(touch_timer, 10, NULL);
    } else {
        if (U->t_ui) lv_timer_delete(U->t_ui);
        if (U->t_touch) lv_timer_delete(U->t_touch);
        U->t_ui = U->t_touch = NULL;
    }
}

/* every few seconds the audio thread's timing goes to the log, so the
 * board can be measured from the portal (tools/plog.sh) */
void es_ui_log_stats(void);
void es_ui_log_stats(void)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - U->log_ms < 5000) return;
    U->log_ms = now;
    if (!U->audio_ok) return;
    es_stats_t s;
    es_stats(U->e, &s);
    aos_hal_log(ES_TAG, "audio: block %d smp (%d us), avg %d us, max %d us, load %d %%, voices %d, "
                "underruns %u, queue %d smp, peak %d %%",
                ES_BLK, ES_BLK * 1000000 / ES_SR, (int)s.blk_us_avg, (int)s.blk_us_max, (int)s.load_pct,
                s.voices, (unsigned)s.underruns, s.queued, (int)(s.peak * 100.0f));
}

/* the heaviest it gets: every voice on a chord, every drum, the effects
 * open; five seconds, and the log says what it cost */
void es_ui_load_test(void)
{
    es_proj_t *p = U->p;
    if (U->load_test_until) return;
    /* every part polyphonic for the test, or a mono one would keep voices idle */
    for (int i = 0; i < ES_PARTS; i++) { U->load_test_mono[i] = p->part[i].p[P_MONO]; p->part[i].p[P_MONO] = 0; }
    for (int c = 0; c < ES_CH; c++) { p->ch[c].dly = es_clampf(p->ch[c].dly, 0.3f, 1); p->ch[c].rev = es_clampf(p->ch[c].rev, 0.3f, 1); }
    for (int i = 0; i < ES_VOICES; i++) es_cmd(U->e, EC_AUDITION, i % ES_PARTS, 48 + i * 4, 100, 5000);
    for (int t = 0; t < ES_DRUMS; t++) es_cmd(U->e, EC_DRUM, t, 100, 0, 0);
    if (!es_playing(U->e)) es_cmd(U->e, EC_PLAY, 0, 0, 0, 0);
    U->load_test_until = (uint32_t)aos_hal_uptime_ms() + 5000;
    U->log_ms = 0;
    aos_ui_toast(_("Prueba de carga: cinco segundos, el resultado va al registro"), 2500);
}
