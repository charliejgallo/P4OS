/*
 * DJ - the sheets: the library, the settings and a deck's tempo and grid.
 *
 * The library lists the card's music folder (three folders deep) and the
 * recordings: MP3, WAV and FLAC, by name, with the tempo of the tracks the
 * app already knows. A search field filters as it is typed (the name, the
 * artist and the folder). Each row loads on deck A or B. The folders are
 * read in one pass each (aos_hal_dir_scan) and kept while the app lives;
 * the refresh button reads them again.
 *
 * These are ordinary LVGL: while one is up the surface takes no fingers.
 */
#include "dj_ui.h"

#include "aos_text_safe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define U dj_ui

#define LIB_MAX   800
#define ROWS_MAX  120

typedef struct {
    char     path[200];
    char     name[100];
    uint32_t size;
    float    bpm;
    bool     rec;
} lib_entry_t;

static struct {
    lib_entry_t *e;
    int          n;
    bool         scanned;
    int          tab;           /* 0 music, 1 recordings */
    int          deck;          /* where a tap on the row loads, -1 ask */
    lv_obj_t    *list, *ta, *kb, *count, *tabs[2];
    char        *meta;
} L;

/* ---- sheet helpers ---- */

static lv_obj_t *s_gbpm;           /* the tempo sheet's number */

static void close_cb(lv_event_t *e) { (void)e; dj_sheet_close(); }

/* a tap on the dimmed screen around the card */
static void bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) dj_sheet_close();
}

void dj_lib_forget(void)
{
    U->sheet = NULL;
    s_gbpm = NULL;
    L.list = L.ta = L.kb = L.count = NULL;
    L.tabs[0] = L.tabs[1] = NULL;
}

void dj_sheet_close(void)
{
    if (!U || !U->sheet) return;
    lv_obj_t *s = U->sheet;
    dj_lib_forget();
    lv_obj_delete(s);
    dj_free(L.meta);
    L.meta = NULL;
    /* the canvases come back to life */
    for (int i = 0; i < U->nctl; i++) U->ctl[i].sig = 0xFFFFFFFFu;
    if (U->surf) lv_obj_invalidate(U->surf);
}

static lv_obj_t *label(lv_obj_t *p, const char *t, const lv_font_t *f, uint32_t color)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *p, const char *t, const lv_font_t *f, int32_t w, int32_t h, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(p);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, lv_color_hex(DJ_COL_BTN), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x4A4A50), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = label(b, t, f, 0xFFFFFF);
    lv_obj_center(l);
    return b;
}

static void btn_on(lv_obj_t *b, bool on, uint32_t color)
{
    lv_obj_set_style_bg_color(b, lv_color_hex(on ? color : DJ_COL_BTN), 0);
}

/* a dimmed screen with a card; returns the card's scrolling column */
static lv_obj_t *sheet(const char *title, int32_t maxw, lv_obj_t **card_out)
{
    dj_ui_release_all();
    lv_obj_t *o = lv_obj_create(U->root);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, U->W, U->H);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_70, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(o, bg_cb, LV_EVENT_CLICKED, NULL);
    U->sheet = o;
    int32_t w = U->W - 48 < maxw ? U->W - 48 : maxw;
    int32_t h = U->H - 40;
    lv_obj_t *card = lv_obj_create(o);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x1C1C1E), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 20, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(card, LV_ALIGN_CENTER, 12, 0);
    lv_obj_t *t = label(card, title, aos_font_body, 0xFFFFFF);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 22, 18);
    lv_obj_t *x = button(card, AOS_SYM_CLOSE, &aos_sym_28, 56, 56, close_cb, NULL);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -12, 10);
    lv_obj_t *c = lv_obj_create(card);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w - 24, h - 86);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 76);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_set_style_pad_bottom(c, 12, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    if (card_out) *card_out = card;
    return c;
}

static lv_obj_t *row(lv_obj_t *p, int32_t h)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), h);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 8, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

/* ---- the library ---- */

typedef struct {
    const char  *dir;
    bool         rec;
    int          depth;
    char       (*sub)[200];
    int         *nsub;
} scan_ctx_t;

static bool audio_ext(const char *n)
{
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".mp3") || !strcasecmp(d, ".wav") || !strcasecmp(d, ".flac"));
}

static bool scan_cb(const aos_dir_entry_t *de, void *ctx)
{
    scan_ctx_t *s = ctx;
    if (de->name[0] == '.') return true;
    if (de->dir) {
        if (s->depth < 3 && *s->nsub < 24) snprintf(s->sub[(*s->nsub)++], 200, "%.120s/%.70s", s->dir, de->name);
        return true;
    }
    if (!audio_ext(de->name) || L.n >= LIB_MAX || de->size < 1024) return true;
    lib_entry_t *e = &L.e[L.n++];
    snprintf(e->path, sizeof e->path, "%.120s/%.78s", s->dir, de->name);
    snprintf(e->name, sizeof e->name, "%.99s", de->name);
    char *dot = strrchr(e->name, '.');
    if (dot) *dot = 0;
    dj_nfc(e->name);
    e->size = de->size;
    e->rec = s->rec;
    e->bpm = 0.0f;
    return true;
}

static void scan_dir(const char *dir, bool rec, int depth)
{
    char (*sub)[200] = dj_alloc(24 * 200);
    if (!sub) return;
    int nsub = 0;
    scan_ctx_t s = { dir, rec, depth, sub, &nsub };
    aos_hal_dir_scan(dir, scan_cb, &s);
    for (int i = 0; i < nsub && L.n < LIB_MAX; i++) scan_dir(sub[i], rec, depth + 1);
    dj_free(sub);
}

static int cmp_entry(const void *a, const void *b)
{
    return strcasecmp(((const lib_entry_t *)a)->name, ((const lib_entry_t *)b)->name);
}

static void lib_scan(void)
{
    if (!L.e) L.e = dj_alloc(LIB_MAX * sizeof(lib_entry_t));
    if (!L.e) return;
    L.n = 0;
    scan_dir(aos_hal_path_music(), false, 1);
    int nm = L.n;
    scan_dir(aos_hal_path_recordings(), true, 3);
    qsort(L.e, (size_t)nm, sizeof(lib_entry_t), cmp_entry);
    qsort(L.e + nm, (size_t)(L.n - nm), sizeof(lib_entry_t), cmp_entry);
    L.scanned = true;
}

static bool match(const lib_entry_t *e, const char *q)
{
    if (!q || !q[0]) return true;
    /* every word of the search somewhere in the path */
    char w[64];
    const char *p = q;
    while (*p) {
        while (*p == ' ') p++;
        int n = 0;
        while (*p && *p != ' ' && n < 63) w[n++] = *p++;
        w[n] = 0;
        if (!n) break;
        bool found = false;
        for (const char *s = e->path; *s && !found; s++)
            if (!strncasecmp(s, w, (size_t)n)) found = true;
        if (!found) return false;
    }
    return true;
}

static void load_to(int idx, int deck)
{
    if (idx < 0 || idx >= L.n) return;
    dj_load(deck, L.e[idx].path);
    dj_sheet_close();
}

static void row_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    load_to(v >> 2, v & 3);
}

static void lib_fill(void);

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_VALUE_CHANGED) lib_fill();
    else if (c == LV_EVENT_FOCUSED && L.kb) lv_obj_remove_flag(L.kb, LV_OBJ_FLAG_HIDDEN);
}

static void kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if ((c == LV_EVENT_READY || c == LV_EVENT_CANCEL) && L.kb) lv_obj_add_flag(L.kb, LV_OBJ_FLAG_HIDDEN);
}

static void tab_cb(lv_event_t *e)
{
    L.tab = (int)(intptr_t)lv_event_get_user_data(e);
    lib_fill();
}

static void rescan_cb(lv_event_t *e)
{
    (void)e;
    lib_scan();
    dj_free(L.meta);
    L.meta = dj_meta_read_all(NULL);
    lib_fill();
}

static void lib_fill(void)
{
    if (!L.list) return;
    lv_obj_clean(L.list);
    const char *q = L.ta ? lv_textarea_get_text(L.ta) : "";
    for (int k = 0; k < 2; k++) if (L.tabs[k]) btn_on(L.tabs[k], L.tab == k, 0x0A84FF);
    int shown = 0, total = 0;
    for (int i = 0; i < L.n; i++) {
        lib_entry_t *en = &L.e[i];
        if (en->rec != (L.tab == 1) || !match(en, q)) continue;
        total++;
        if (shown >= ROWS_MAX) continue;
        shown++;
        if (en->bpm == 0.0f && L.meta) {
            const char *b = strrchr(en->path, '/');
            en->bpm = dj_meta_bpm(b ? b + 1 : en->path, en->size, L.meta, 0);
            if (en->bpm == 0.0f) en->bpm = -1.0f;
        }
        lv_obj_t *r = row(L.list, 62);
        lv_obj_set_style_bg_color(r, lv_color_hex(0x26262A), 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(r, 12, 0);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        if (L.deck >= 0) lv_obj_add_event_cb(r, row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i << 2 | L.deck));
        char nm[120];
        aos_text_safe(nm, sizeof nm, en->name);
        lv_obj_t *n = label(r, nm, aos_font_small, 0xFFFFFF);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_flex_grow(n, 1);
        lv_obj_set_height(n, lv_font_get_line_height(aos_font_small));
        char b[24] = "";
        if (en->bpm > 0.0f) snprintf(b, sizeof b, "%.1f", (double)en->bpm);
        lv_obj_t *bl = label(r, b, aos_font_caption, DJ_COL_DIM);
        lv_obj_set_width(bl, 70);
        lv_obj_set_style_text_align(bl, LV_TEXT_ALIGN_RIGHT, 0);
        for (int d = 0; d < DJ_NDECK; d++) {
            lv_obj_t *bt = button(r, d == 0 ? "A" : "B", aos_font_small, 62, 48, row_cb, (void *)(intptr_t)(i << 2 | d));
            if (L.deck == d) btn_on(bt, true, d == 0 ? 0x1D4F7A : 0x7A3A1D);
            else lv_obj_set_style_text_color(lv_obj_get_child(bt, 0), lv_color_hex(d == 0 ? DJ_COL_A : DJ_COL_B), 0);
        }
    }
    if (!total) {
        lv_obj_t *l = label(L.list, L.n == 0 ? _("No hay temas. Copiá MP3, WAV o FLAC a la carpeta music de la tarjeta.")
                                             : _("Nada coincide con la búsqueda."), aos_font_small, DJ_COL_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, lv_pct(100));
    }
    if (L.count) {
        char b[64];
        if (total > shown) snprintf(b, sizeof b, _("%d de %d"), shown, total);
        else snprintf(b, sizeof b, _("%d temas"), total);
        lv_label_set_text(L.count, b);
    }
}

void dj_lib_open(int deck)
{
    if (!L.scanned) lib_scan();
    L.deck = deck;
    char title[64];
    if (deck >= 0) snprintf(title, sizeof title, _("Cargar en la bandeja %c"), 'A' + deck);
    else snprintf(title, sizeof title, "%s", _("Temas"));
    lv_obj_t *card;
    lv_obj_t *c = sheet(title, 980, &card);
    L.meta = dj_meta_read_all(NULL);
    for (int i = 0; i < L.n; i++) L.e[i].bpm = 0.0f;
    lv_obj_t *top = row(c, 60);
    L.ta = lv_textarea_create(top);
    lv_textarea_set_one_line(L.ta, true);
    lv_textarea_set_placeholder_text(L.ta, _("Buscar"));
    lv_obj_set_style_text_font(L.ta, aos_font_small, 0);
    lv_obj_set_flex_grow(L.ta, 1);
    lv_obj_add_event_cb(L.ta, ta_cb, LV_EVENT_ALL, NULL);
    int32_t tw = U->land ? 170 : 120;
    L.tabs[0] = button(top, _("Música"), aos_font_caption, tw, 52, tab_cb, (void *)(intptr_t)0);
    L.tabs[1] = button(top, _("Grabaciones"), aos_font_caption, tw, 52, tab_cb, (void *)(intptr_t)1);
    button(top, AOS_SYM_RESTART, &aos_sym_28, 56, 52, rescan_cb, NULL);
    L.count = label(c, "", aos_font_caption, DJ_COL_DIM);
    L.list = lv_obj_create(c);
    lv_obj_remove_style_all(L.list);
    lv_obj_set_width(L.list, lv_pct(100));
    lv_obj_set_flex_grow(L.list, 1);
    lv_obj_set_flex_flow(L.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(L.list, 6, 0);
    lv_obj_add_flag(L.list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(L.list, LV_DIR_VER);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    L.kb = lv_keyboard_create(card);
    aos_keyboard_style(L.kb, aos_font_body);
    lv_obj_set_size(L.kb, lv_obj_get_width(card), U->land ? 300 : 420);
    lv_obj_align(L.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(L.kb, L.ta);
    lv_obj_add_event_cb(L.kb, kb_cb, LV_EVENT_ALL, NULL);
    lv_obj_add_flag(L.kb, LV_OBJ_FLAG_HIDDEN);
    lib_fill();
}

/* ---- the settings ---- */

enum { S_RANGE, S_XCURVE, S_QUANT, S_VINYL, S_ZOOM };

static lv_obj_t *s_seg[5][4];

static int setting_now(int s)
{
    dj_mix_t *m = U->m;
    switch (s) {
    case S_RANGE:  return m->range > 0.3f ? 2 : m->range > 0.1f ? 1 : 0;
    case S_XCURVE: return m->xcurve;
    case S_QUANT:  return m->quant ? 0 : 1;
    case S_VINYL:  return m->vinyl ? 0 : 1;
    case S_ZOOM:   return U->zoom;
    default:       return 0;
    }
}

static void seg_paint(int s)
{
    for (int k = 0; k < 4; k++) if (s_seg[s][k]) btn_on(s_seg[s][k], setting_now(s) == k, 0x0A84FF);
}

static void seg_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int s = v >> 4, k = v & 15;
    dj_mix_t *m = U->m;
    switch (s) {
    case S_RANGE:  m->range = k == 2 ? 0.5f : k == 1 ? 0.16f : 0.08f; break;
    case S_XCURVE: m->xcurve = k; break;
    case S_QUANT:  m->quant = k == 0; break;
    case S_VINYL:  m->vinyl = k == 0; break;
    case S_ZOOM:   U->zoom = k; break;
    default: break;
    }
    seg_paint(s);
    dj_ui_save_settings();
}

static void seg_row(lv_obj_t *c, int s, const char *title, const char *const *opts, int n)
{
    lv_obj_t *l = label(c, title, aos_font_caption, DJ_COL_DIM);
    (void)l;
    lv_obj_t *r = row(c, 58);
    int32_t w = (lv_obj_get_width(c) - 8 * (n - 1)) / n;
    if (w > 220) w = 220;
    for (int k = 0; k < 4; k++) s_seg[s][k] = NULL;
    for (int k = 0; k < n; k++)
        s_seg[s][k] = button(r, _(opts[k]), aos_font_small, w, 54, seg_cb, (void *)(intptr_t)(s << 4 | k));
    seg_paint(s);
}

static void learn_cb(lv_event_t *e)
{
    (void)e;
    U->learn = 1;
    U->learn_ctl = -1;
    dj_sheet_close();
    aos_ui_toast(_("Tocá el control de la pantalla que querés manejar"), 3000);
}

static void midi_clear_cb(lv_event_t *e)
{
    (void)e;
    dj_midi_clear();
    dj_sheet_close();
    aos_ui_toast(_("Controles MIDI borrados"), 1500);
}

void dj_settings_open(void)
{
    lv_obj_t *c = sheet(_("Ajustes"), 760, NULL);
    lv_obj_update_layout(c);
    static const char *const RANGE[] = { "8 %", "16 %", "50 %" };
    static const char *const XC[] = { N_("Suave"), N_("Corte (scratch)") };
    static const char *const YN[] = { N_("Sí"), N_("No") };
    static const char *const VIN[] = { N_("Scratch"), N_("Sólo empujar") };
    static const char *const ZM[] = { N_("Muy cerca"), N_("Cerca"), N_("Lejos"), N_("Muy lejos") };
    seg_row(c, S_RANGE, _("Rango del pitch"), RANGE, 3);
    seg_row(c, S_XCURVE, _("Curva del crossfader"), XC, 2);
    seg_row(c, S_QUANT, _("Cue, hot cues y loops a tiempo"), YN, 2);
    seg_row(c, S_VINYL, _("Al tocar el centro del plato"), VIN, 2);
    seg_row(c, S_ZOOM, _("Forma de onda"), ZM, 4);
    lv_obj_t *l = label(c, _("Controlador MIDI por USB"), aos_font_caption, DJ_COL_DIM);
    (void)l;
    lv_obj_t *r = row(c, 58);
    button(r, _("Aprender"), aos_font_small, 220, 54, learn_cb, NULL);
    button(r, _("Borrar"), aos_font_small, 160, 54, midi_clear_cb, NULL);
    char b[48];
    snprintf(b, sizeof b, _("%d controles"), dj_midi_count());
    label(r, b, aos_font_caption, DJ_COL_DIM);
    lv_obj_t *n = label(c, _("Aprender: tocá un control de la pantalla y después movelo en el aparato. "
                             "La placa tiene un solo parlante: suena la suma de las dos bandejas; "
                             "la mezcla se graba en estéreo."),
                        aos_font_caption, DJ_COL_DIM);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(n, lv_pct(100));
}

/* ---- a deck's tempo and grid ---- */

static int s_gdeck;

static void grid_paint(void)
{
    if (!s_gbpm) return;
    const dj_pub_t *p = dj_pub(U->e, s_gdeck);
    char b[48];
    if (p->bpm > 0.0f) snprintf(b, sizeof b, "%.2f BPM", (double)p->bpm);
    else snprintf(b, sizeof b, "%s", _("Sin tempo todavía"));
    lv_label_set_text(s_gbpm, b);
}

/* from the UI timer, while a sheet is up */
void dj_sheet_tick(void)
{
    grid_paint();
}

static void grid_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int d = s_gdeck;
    const dj_pub_t *p = dj_pub(U->e, d);
    dj_track_t *t = dj_deck_track(U->e, d);
    dj_ui_grid_by_hand(d);
    switch (v) {
    case 0: dj_cmd(U->e, DC_BPM_SCALE, d, 0, 0.5f); break;
    case 1: dj_cmd(U->e, DC_BPM_SCALE, d, 0, 2.0f); break;
    case 2: dj_cmd(U->e, DC_GRID, d, 0, -1.0f); break;
    case 3: dj_cmd(U->e, DC_GRID, d, 0, -1.0f / 32.0f); break;
    case 4: dj_cmd(U->e, DC_GRID, d, 0, 1.0f / 32.0f); break;
    case 5: dj_cmd(U->e, DC_GRID, d, 0, 1.0f); break;
    case 6:
        if (p->bpm > 0.0f && t->sr) {
            /* a beat right under the playhead */
            float P = 60.0f / p->bpm * (float)t->sr;
            float x = ((float)p->pos - p->beat0) / P;
            dj_cmd(U->e, DC_GRID, d, 0, x - floorf(x));
        }
        break;
    case 7: case 8: {
        /* the tempo by hand: a tenth of a BPM */
        if (p->bpm > 0.0f) dj_cmd(U->e, DC_BPM_SCALE, d, 0, (p->bpm + (v == 7 ? -0.1f : 0.1f)) / p->bpm);
        break;
    }
    default: break;
    }
}

void dj_grid_open(int deck)
{
    s_gdeck = deck;
    char title[64];
    snprintf(title, sizeof title, _("Tempo y grilla: bandeja %c"), 'A' + deck);
    lv_obj_t *c = sheet(title, 760, NULL);
    s_gbpm = label(c, "", aos_font_title, 0xFFFFFF);
    lv_obj_t *r = row(c, 58);
    button(r, "/2", aos_font_small, 110, 54, grid_cb, (void *)0);
    button(r, "x2", aos_font_small, 110, 54, grid_cb, (void *)1);
    button(r, "-0.1", aos_font_small, 110, 54, grid_cb, (void *)7);
    button(r, "+0.1", aos_font_small, 110, 54, grid_cb, (void *)8);
    label(c, _("Mover la grilla"), aos_font_caption, DJ_COL_DIM);
    r = row(c, 58);
    button(r, _("< 1 tiempo"), aos_font_small, 150, 54, grid_cb, (void *)2);
    button(r, "<", aos_font_small, 90, 54, grid_cb, (void *)3);
    button(r, ">", aos_font_small, 90, 54, grid_cb, (void *)4);
    button(r, _("1 tiempo >"), aos_font_small, 150, 54, grid_cb, (void *)5);
    r = row(c, 58);
    button(r, _("Un tiempo acá"), aos_font_small, 260, 54, grid_cb, (void *)6);
    lv_obj_t *n = label(c, _("La grilla se guarda con el tema. \"Un tiempo acá\" pone una línea de la grilla "
                             "donde está el cabezal: pausá el tema justo en un golpe y tocalo."),
                        aos_font_caption, DJ_COL_DIM);
    lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(n, lv_pct(100));
    grid_paint();
}
