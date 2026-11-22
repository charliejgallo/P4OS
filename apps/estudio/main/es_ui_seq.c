/*
 * Estudio - Ritmo (the drum steps) and Notas (the piano roll).
 *
 * Both grids are one object each, drawn in one draw event and touched
 * through LVGL's pointer (one finger is what editing wants, and that way
 * the portal's and the simulator's taps work too).
 *
 * The drum grid: lying down the tracks are rows and the sixteen steps of
 * the page run across; standing up it turns, the tracks are columns and the
 * steps run down the screen. Touching a step turns it on or off and a drag
 * along the track paints the same; in Velocidad and Probabilidad the height
 * of the touch in the cell is the value, and a drag across cells draws it.
 *
 * The roll: a touch on an empty cell writes a note and a drag to the right
 * stretches it; a touch on a note and a drag moves it (in time and pitch), a
 * touch from its last cell stretches it, a touch that does not move removes
 * it. The lane under the grid is the velocity or the probability of the
 * notes that start on each step. Dragging the keys on the left scrolls.
 */
#include "es_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define U es_ui

static void rect(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color,
                 lv_opa_t opa, int32_t radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void frame(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, uint32_t color, int32_t w,
                  int32_t radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = lv_color_hex(color);
    d.border_width = w;
    d.border_opa = LV_OPA_COVER;
    d.radius = radius;
    lv_area_t a = { x1, y1, x2, y2 };
    lv_draw_rect(layer, &d, &a);
}

static void label(lv_layer_t *layer, int32_t x1, int32_t y1, int32_t x2, int32_t y2, const char *s,
                  const lv_font_t *f, uint32_t color, lv_text_align_t al)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s;
    d.font = f;
    d.color = lv_color_hex(color);
    d.align = al;
    d.text_local = 1;
    int32_t fh = lv_font_get_line_height(f);
    int32_t cy = (y1 + y2) / 2;
    lv_area_t a = { x1, cy - fh / 2, x2, cy + fh / 2 + 1 };
    lv_draw_label(layer, &d, &a);
}

/* ---- the pattern chips, A-H ---- */

static void pat_chip_cb(lv_event_t *e)
{
    es_ui_select_pat((int)(intptr_t)lv_event_get_user_data(e));
    es_ui_show_view(U->view);
}

void es_pat_chips_refresh(lv_obj_t *row)
{
    if (!row) return;
    int play = es_playing(U->e) ? es_play_pat(U->e) : -1, cue = es_cued_pat(U->e);
    for (int i = 0; i < ES_PATS; i++) {
        lv_obj_t *b = lv_obj_get_child(row, i);
        if (!b) break;
        ui_btn_on(b, i == U->pat, aos_theme_accent_rgb());
        lv_obj_set_style_border_width(b, (i == play || i == cue) ? 3 : 0, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(i == play ? C_PLAY : 0xFFD60A), 0);
        lv_obj_t *l = lv_obj_get_child(b, 0);
        if (i != U->pat) lv_obj_set_style_text_color(l, lv_color_hex(ui_pat_used(&U->p->pat[i]) ? C_TEXT : C_DIM), 0);
    }
}

lv_obj_t *es_pat_chips(lv_obj_t *parent, int32_t h)
{
    lv_obj_t *row = ui_box(parent, LV_SIZE_CONTENT, h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 4, 0);
    for (int i = 0; i < ES_PATS; i++) {
        char b[2] = { (char)('A' + i), 0 };
        ui_btn(row, b, aos_font_small, U->land ? 50 : 52, h, pat_chip_cb, (void *)(intptr_t)i);
    }
    es_pat_chips_refresh(row);
    return row;
}

/* ---- shared header bits ---- */

static lv_obj_t *s_chips, *s_len_lbl, *s_pages;

static int npages(void)
{
    int len = U->p->pat[U->pat].len;
    return (len + 15) / 16;
}

static void page_cb(lv_event_t *e)
{
    U->page = (int)(intptr_t)lv_event_get_user_data(e);
    es_ui_show_view(U->view);
}

static void pages_paint(void)
{
    if (!s_pages) return;
    int n = npages();
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_get_child(s_pages, i);
        if (!b) break;
        lv_obj_set_flag(b, LV_OBJ_FLAG_HIDDEN, i >= n || n == 1);
        ui_btn_on(b, i == U->page, C_CELL2);
        lv_obj_set_style_border_width(b, i == U->page ? 2 : 0, 0);
        lv_obj_set_style_border_color(b, AOS_C_ACCENT, 0);
    }
}

static void len_set(int len)
{
    es_pat_t *t = &U->p->pat[U->pat];
    t->len = (uint8_t)(len < 1 ? 1 : len > ES_STEPS ? ES_STEPS : len);
    if (U->page >= npages()) U->page = npages() - 1;
    if (s_len_lbl) lv_label_set_text_fmt(s_len_lbl, "%d", t->len);
    pages_paint();
    if (U->grid) lv_obj_invalidate(U->grid);
    es_ui_dirty();
}

static void len_minus(lv_event_t *e) { (void)e; len_set(U->p->pat[U->pat].len - 1); }
static void len_plus(lv_event_t *e)  { (void)e; len_set(U->p->pat[U->pat].len + 1); }

/* the chips, the pages and the length: what both views start with */
static lv_obj_t *header(lv_obj_t *body, int32_t W)
{
    lv_obj_t *row = ui_row(body, W - 24, 8);
    lv_obj_set_pos(row, 12, 8);
    s_chips = es_pat_chips(row, 54);
    s_pages = ui_box(row, LV_SIZE_CONTENT, 54);
    lv_obj_set_flex_flow(s_pages, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_pages, 4, 0);
    static const char *const PG[4] = { "1", "2", "3", "4" };
    for (int i = 0; i < 4; i++) ui_btn(s_pages, PG[i], aos_font_small, 50, 54, page_cb, (void *)(intptr_t)i);
    pages_paint();
    char b[8];
    snprintf(b, sizeof b, "%d", U->p->pat[U->pat].len);
    lv_obj_t *st = ui_stepper(row, b, 168, 54, len_minus, len_plus, &s_len_lbl);
    (void)st;
    return row;
}

static void forget_common(void)
{
    s_chips = s_len_lbl = s_pages = NULL;
    U->grid = NULL;
}

/* ======================================================================
 * Ritmo
 * ====================================================================== */

static struct {
    int32_t  gx, gy, cw, ch, lab;       /* the grid's origin in the object, a cell, the labels' band */
    int      paint_track, paint_on, last_tr, last_col;
    lv_obj_t *mode_seg, *snd_btn, *mute_btn;
    uint32_t hits[ES_DRUMS], flash[ES_DRUMS];
} D;

#define GAP 6           /* between groups of four */

static void d_cell(int tr, int col, lv_area_t *r)
{
    if (U->land) {
        int32_t x = D.gx + col * D.cw + (col / 4) * GAP;
        int32_t y = D.gy + tr * D.ch;
        *r = (lv_area_t){ x + 2, y + 2, x + D.cw - 3, y + D.ch - 3 };
    } else {
        int32_t x = D.gx + tr * D.cw;
        int32_t y = D.gy + col * D.ch + (col / 4) * GAP;
        *r = (lv_area_t){ x + 2, y + 2, x + D.cw - 3, y + D.ch - 3 };
    }
}

/* which cell is under (x, y) in the object; *tr = -1 none; *col = -1 on the
 * track's label */
static void d_at(int32_t x, int32_t y, int *tr, int *col)
{
    *tr = -1;
    *col = -1;
    if (U->land) {
        if (y < D.gy) return;
        int t = (y - D.gy) / D.ch;
        if (t < 0 || t >= ES_DRUMS) return;
        *tr = t;
        if (x < D.gx) return;
        for (int c = 0; c < 16; c++) {
            int32_t x0 = D.gx + c * D.cw + (c / 4) * GAP;
            if (x >= x0 && x < x0 + D.cw + GAP) { *col = c; return; }
        }
        *tr = -1;
    } else {
        if (x < D.gx) return;
        int t = (x - D.gx) / D.cw;
        if (t < 0 || t >= ES_DRUMS) return;
        *tr = t;
        if (y < D.gy) return;
        for (int c = 0; c < 16; c++) {
            int32_t y0 = D.gy + c * D.ch + (c / 4) * GAP;
            if (y >= y0 && y < y0 + D.ch + GAP) { *col = c; return; }
        }
        *tr = -1;
    }
}

static void d_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *g = lv_event_get_current_target_obj(e);
    lv_area_t o;
    lv_obj_get_coords(g, &o);
    es_pat_t *t = &U->p->pat[U->pat];
    int len = t->len, base = U->page * 16;
    int play = es_play_step(U->e);
    bool here = es_play_pat(U->e) == U->pat && play >= 0;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    char buf[16];
    for (int tr = 0; tr < ES_DRUMS; tr++) {
        uint32_t tc = ui_drum_color(tr);
        bool sel = tr == U->track, mute = U->p->dt[tr].mute;
        bool lit = (int32_t)(D.flash[tr] - now) > 0;
        /* the track's label */
        lv_area_t lr;
        if (U->land) lr = (lv_area_t){ o.x1 + 4, o.y1 + D.gy + tr * D.ch + 2, o.x1 + D.gx - 10, o.y1 + D.gy + (tr + 1) * D.ch - 3 };
        else lr = (lv_area_t){ o.x1 + D.gx + tr * D.cw + 2, o.y1 + 2, o.x1 + D.gx + (tr + 1) * D.cw - 3, o.y1 + D.gy - 8 };
        rect(layer, lr.x1, lr.y1, lr.x2, lr.y2, sel ? tc : C_PANEL, sel ? LV_OPA_COVER : LV_OPA_COVER, 10);
        if (lit && !sel) frame(layer, lr.x1, lr.y1, lr.x2, lr.y2, tc, 3, 10);
        label(layer, lr.x1 + 8, lr.y1, lr.x2 - 4, lr.y2, ui_track_name(tr, !U->land),
              U->land ? aos_font_caption : aos_font_small, sel ? 0x000000 : mute ? C_DIM : C_TEXT,
              U->land ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_CENTER);
        for (int c = 0; c < 16; c++) {
            int s = base + c;
            lv_area_t r;
            d_cell(tr, c, &r);
            r.x1 += o.x1; r.x2 += o.x1; r.y1 += o.y1; r.y2 += o.y1;
            if (s >= len) { rect(layer, r.x1, r.y1, r.x2, r.y2, C_PANEL, LV_OPA_50, 8); continue; }
            es_dstep_t st = t->d[tr][s];
            rect(layer, r.x1, r.y1, r.x2, r.y2, (c / 4) & 1 ? C_CELL : C_CELL2, LV_OPA_COVER, 8);
            if (st.vel) {
                lv_opa_t op = mute ? LV_OPA_40 : LV_OPA_COVER;
                if (U->dmode == 0) {
                    lv_opa_t a = (lv_opa_t)(110 + st.vel);
                    rect(layer, r.x1, r.y1, r.x2, r.y2, tc, mute ? LV_OPA_40 : a, 8);
                    if (st.prob < 100) {
                        int32_t d = (r.x2 - r.x1) / 5;
                        rect(layer, r.x2 - d - 4, r.y1 + 4, r.x2 - 4, r.y1 + 4 + d, 0x000000, LV_OPA_60, d);
                    }
                } else {
                    float v = U->dmode == 1 ? (float)st.vel / 127.0f : (float)st.prob / 100.0f;
                    int32_t hh = (int32_t)((float)(r.y2 - r.y1) * v);
                    rect(layer, r.x1, r.y2 - hh, r.x2, r.y2, tc, op, 6);
                    if (U->dmode == 2 && r.x2 - r.x1 > 34) {
                        snprintf(buf, sizeof buf, "%d", st.prob);
                        label(layer, r.x1, r.y1, r.x2, r.y1 + 26, buf, aos_font_tiny, C_TEXT, LV_TEXT_ALIGN_CENTER);
                    }
                }
            }
            if (here && s == play) frame(layer, r.x1, r.y1, r.x2, r.y2, 0xFFFFFF, 3, 8);
        }
    }
    /* the step numbers along the steps */
    for (int c = 0; c < 16; c++) {
        if (base + c >= len || (c % 4)) continue;
        lv_area_t r;
        d_cell(0, c, &r);
        snprintf(buf, sizeof buf, "%d", base + c + 1);
        if (U->land) label(layer, o.x1 + r.x1, o.y1 + D.gy + ES_DRUMS * D.ch + 2, o.x1 + r.x2, o.y1 + D.gy + ES_DRUMS * D.ch + 26, buf, aos_font_tiny, C_DIM, LV_TEXT_ALIGN_LEFT);
        else label(layer, o.x1 + 2, o.y1 + r.y1, o.x1 + D.gx - 6, o.y1 + r.y2, buf, aos_font_tiny, C_DIM, LV_TEXT_ALIGN_CENTER);
    }
}

static void d_track_panel(void);

static void d_select(int tr)
{
    if (tr == U->track) return;
    U->track = tr;
    d_track_panel();
}

static void d_value(int tr, int c, int32_t y_in_cell, int32_t ch)
{
    es_dstep_t *st = &U->p->pat[U->pat].d[tr][U->page * 16 + c];
    float v = 1.0f - (float)y_in_cell / (float)ch;
    v = es_clampf(v, 0.05f, 1.0f);
    if (U->dmode == 1) {
        st->vel = (uint8_t)(v * 127.0f);
        if (!st->prob) st->prob = 100;
    } else if (st->vel) {
        st->prob = (uint8_t)(v * 100.0f);
        if (st->prob < 1) st->prob = 1;
    }
}

static void d_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *g = lv_event_get_current_target_obj(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t o;
    lv_obj_get_coords(g, &o);
    int32_t x = pt.x - o.x1, y = pt.y - o.y1;
    int tr, c;
    d_at(x, y, &tr, &c);
    es_pat_t *t = &U->p->pat[U->pat];
    if (code == LV_EVENT_PRESSED) {
        D.paint_track = -1;
        D.last_tr = tr;
        D.last_col = c;
        if (tr < 0) return;
        if (c < 0) {
            d_select(tr);
            es_cmd(U->e, EC_DRUM, tr, 100, 0, SRC_LIVE);
            lv_obj_invalidate(g);
            return;
        }
        int s = U->page * 16 + c;
        if (s >= t->len) return;
        d_select(tr);
        if (U->dmode == 0) {
            es_dstep_t *st = &t->d[tr][s];
            D.paint_on = st->vel ? 0 : 1;
            *st = D.paint_on ? (es_dstep_t){ 100, 100 } : (es_dstep_t){ 0, 0 };
            D.paint_track = tr;
            if (D.paint_on && !es_playing(U->e)) es_cmd(U->e, EC_DRUM, tr, 100, 0, SRC_LIVE);
        } else {
            lv_area_t r;
            d_cell(tr, c, &r);
            d_value(tr, c, U->land ? y - r.y1 : y - r.y1, r.y2 - r.y1);
            D.paint_track = tr;
        }
        es_ui_dirty();
        lv_obj_invalidate(g);
    } else if (code == LV_EVENT_PRESSING) {
        if (D.paint_track < 0 || c < 0 || tr < 0) return;
        int s = U->page * 16 + c;
        if (s >= t->len) return;
        if (U->dmode == 0) {
            if (tr != D.paint_track || (tr == D.last_tr && c == D.last_col)) return;
            t->d[tr][s] = D.paint_on ? (es_dstep_t){ 100, 100 } : (es_dstep_t){ 0, 0 };
        } else {
            lv_area_t r;
            d_cell(tr, c, &r);
            d_value(tr, c, y - r.y1, r.y2 - r.y1);
        }
        D.last_tr = tr;
        D.last_col = c;
        es_ui_dirty();
        lv_obj_invalidate(g);
    }
}

static void dmode_cb(lv_event_t *e)
{
    U->dmode = (int)(intptr_t)lv_event_get_user_data(e);
    ui_seg_set(D.mode_seg, U->dmode, aos_theme_accent_rgb());
    if (U->grid) lv_obj_invalidate(U->grid);
}

/* ---- the selected track: its sound, tune, decay and level ---- */

static lv_obj_t *s_panel;

static void snd_pick_cb(lv_event_t *e)
{
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    es_dtrack_t *d = &U->p->dt[U->track];
    if (v < DS_COUNT) {
        d->src = (uint8_t)v;
        d->sample[0] = 0;
    } else {
        lv_obj_t *b = lv_event_get_target_obj(e);
        const char *name = lv_label_get_text(lv_obj_get_child(b, 0));
        char err[64];
        if (!es_bank_load(name, err, sizeof err)) {
            char m[120];
            snprintf(m, sizeof m, "%s: %s", name, _(err));
            aos_ui_toast(m, 2500);
            return;
        }
        snprintf(d->sample, sizeof d->sample, "%s", name);
        d->src = DS_COUNT;
    }
    ui_sheet_close();
    es_cmd(U->e, EC_DRUM, U->track, 100, 0, SRC_LIVE);
    es_ui_dirty();
    d_track_panel();
    if (U->grid) lv_obj_invalidate(U->grid);
}

#include <dirent.h>

static void snd_sheet(lv_event_t *e)
{
    (void)e;
    lv_obj_t *c = ui_sheet(_("Sonido de la pista"));
    int32_t w = lv_obj_get_width(c);
    lv_obj_t *grid = ui_row(c, w, 8);
    int32_t bw = (w - 16) / 3;
    for (int i = 0; i < DS_COUNT; i++) {
        lv_obj_t *b = ui_btn(grid, _(ES_DRUM_NAMES[i]), aos_font_caption, bw, 60, snd_pick_cb, (void *)(intptr_t)i);
        ui_btn_on(b, U->p->dt[U->track].src == i, ui_drum_color(U->track));
    }
    ui_label(c, _("Muestras de la tarjeta (estudio/muestras)"), aos_font_small, C_DIM);
    char dir[300];
    es_path(dir, sizeof dir, "muestras", "");
    DIR *d = opendir(dir);
    int n = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && n < 64) {
            const char *nm = de->d_name;
            size_t l = strlen(nm);
            if (nm[0] == '.' || l < 5 || strcasecmp(nm + l - 4, ".wav") || l >= 48) continue;
            lv_obj_t *b = ui_btn(c, nm, aos_font_caption, w, 56, snd_pick_cb, (void *)(intptr_t)DS_COUNT);
            ui_btn_on(b, U->p->dt[U->track].src == DS_COUNT && !strcmp(U->p->dt[U->track].sample, nm),
                      ui_drum_color(U->track));
            n++;
        }
        closedir(d);
    }
    if (!n) {
        lv_obj_t *l = ui_label(c, _("No hay WAV todavía. Se suben desde la página del Estudio en el portal."),
                               aos_font_caption, C_DIM);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_width(l, w);
    }
}

static void mute_cb(lv_event_t *e)
{
    (void)e;
    es_dtrack_t *d = &U->p->dt[U->track];
    d->mute = !d->mute;
    ui_btn_on(D.mute_btn, d->mute, C_REC);
    if (U->grid) lv_obj_invalidate(U->grid);
    es_ui_dirty();
}

static void d_track_panel(void)
{
    if (!s_panel) return;
    lv_obj_clean(s_panel);
    U->nbinds = 0;
    es_dtrack_t *d = &U->p->dt[U->track];
    lv_obj_update_layout(s_panel);
    int32_t W = lv_obj_get_width(s_panel);
    int32_t sw = U->land ? (W - 300 - 160 - 4 * 10) / 4 : (W - 20) / 2;
    char b[64];
    snprintf(b, sizeof b, "%s  " LV_SYMBOL_DOWN, ui_track_name(U->track, false));
    D.snd_btn = ui_btn(s_panel, b, aos_font_caption, U->land ? 300 : W / 2 - 10, 58, snd_sheet, NULL);
    lv_obj_set_style_border_width(D.snd_btn, 3, 0);
    lv_obj_set_style_border_side(D.snd_btn, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(D.snd_btn, lv_color_hex(ui_drum_color(U->track)), 0);
    D.mute_btn = ui_btn(s_panel, _("Silenciar"), aos_font_caption, U->land ? 150 : W / 2 - 10, 58, mute_cb, NULL);
    ui_btn_on(D.mute_btn, d->mute, C_REC);
    ui_slider(s_panel, _("Afinación"), &d->tune, -24, 24, U_SEMI, sw, NULL);
    ui_slider(s_panel, _("Caída"), &d->decay, 0.1f, 2.0f, U_X, sw, NULL);
    ui_slider(s_panel, _("Nivel"), &d->level, 0, 1, U_PCT, sw, NULL);
    ui_slider(s_panel, _("Swing"), &U->p->swing, 0.5f, 0.75f, U_PCT, sw, NULL);
    if (U->grid) lv_obj_invalidate(U->grid);
}

static void d_refresh(void)
{
    static int last = -2, lastpat = -1;
    int s = es_play_step(U->e);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    bool inv = s != last || es_play_pat(U->e) != lastpat;
    for (int t = 0; t < ES_DRUMS; t++) {
        uint32_t h = es_drum_hits(U->e, t);
        if (h != D.hits[t]) { D.hits[t] = h; D.flash[t] = now + 110; inv = true; }
    }
    last = s;
    lastpat = es_play_pat(U->e);
    es_pat_chips_refresh(s_chips);
    if (inv && U->grid) lv_obj_invalidate(U->grid);
}

static void d_forget(void)
{
    forget_common();
    s_panel = NULL;
    D.mode_seg = D.snd_btn = D.mute_btn = NULL;
}

void es_view_drums(lv_obj_t *body)
{
    int32_t W = lv_obj_get_width(body), H = lv_obj_get_height(body);
    lv_obj_t *hdr = header(body, W);
    static const char *MODES[3];
    MODES[0] = _("Pasos"); MODES[1] = _("Velocidad"); MODES[2] = _("Probab.");
    D.mode_seg = ui_seg(hdr, MODES, 3, U->dmode, U->land ? 390 : W - 24, 54, dmode_cb, aos_theme_accent_rgb());
    lv_obj_update_layout(body);
    int32_t top = 8 + lv_obj_get_height(hdr) + 10;
    int32_t ph = U->land ? 74 : 180;          /* the track's panel */
    int32_t gh = H - top - ph - 52;           /* clear of the swipe home */
    lv_obj_t *g = ui_box(body, W - 24, gh);
    lv_obj_set_pos(g, 12, top);
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g, d_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(g, d_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g, d_touch, LV_EVENT_PRESSING, NULL);
    U->grid = g;
    int32_t gw = W - 24;
    if (U->land) {
        D.gx = 170;
        D.gy = 0;
        D.cw = (gw - D.gx - 3 * GAP) / 16;
        D.ch = (gh - 26) / ES_DRUMS;
    } else {
        D.gx = 54;
        D.gy = 58;
        D.cw = (gw - D.gx) / ES_DRUMS;
        D.ch = (gh - D.gy - 3 * GAP) / 16;
    }
    s_panel = ui_row(body, W - 24, 10);
    lv_obj_set_pos(s_panel, 12, top + gh + 8);
    for (int t = 0; t < ES_DRUMS; t++) D.hits[t] = es_drum_hits(U->e, t);
    d_track_panel();
    U->refresh = d_refresh;
    U->forget = d_forget;
}

/* ======================================================================
 * Notas: the piano roll
 * ====================================================================== */

enum { R_NONE, R_NEW, R_MOVE, R_SIZE, R_LANE, R_SCROLL };

static struct {
    int32_t  kx, gx, gy, cw, rh, rows, lane_y, lane_h;
    int      mode, idx, off, moved, start_step, start_note;
    int32_t  scroll_y0;
    int      scroll_low0;
    lv_obj_t *part_btn[ES_PARTS], *lane_seg;
} R;

static int r_low(void) { return U->roll_low[U->part]; }

static void r_cell(int col, int row_from_bottom, lv_area_t *a)
{
    int32_t x = R.gx + col * R.cw + (col / 4) * 3;
    int32_t y = R.gy + (R.rows - 1 - row_from_bottom) * R.rh;
    *a = (lv_area_t){ x, y, x + R.cw - 1, y + R.rh - 1 };
}

static int r_col_at(int32_t x)
{
    for (int c = 0; c < 16; c++) {
        int32_t x0 = R.gx + c * R.cw + (c / 4) * 3;
        if (x >= x0 && x < x0 + R.cw + 3) return c;
    }
    return x < R.gx ? -1 : 15;
}

static void r_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *g = lv_event_get_current_target_obj(e);
    lv_area_t o;
    lv_obj_get_coords(g, &o);
    es_pat_t *t = &U->p->pat[U->pat];
    int len = t->len, base = U->page * 16, low = r_low();
    uint32_t pc = ui_part_color(U->part);
    char buf[16];
    /* the rows, the black keys darker, and the keys on the left */
    for (int r = 0; r < R.rows; r++) {
        int note = low + r;
        bool black = (0x54A >> (note % 12)) & 1;
        lv_area_t a;
        r_cell(0, r, &a);
        int32_t y1 = o.y1 + a.y1, y2 = o.y1 + a.y2;
        rect(layer, o.x1 + R.gx, y1, o.x1 + R.gx + 16 * R.cw + 9, y2, black ? 0x15151B : 0x1E1E26, LV_OPA_COVER, 0);
        rect(layer, o.x1 + R.kx, y1 + 1, o.x1 + R.gx - 6, y2 - 1, black ? 0x2A2A33 : 0xD8D8DE, LV_OPA_COVER, 3);
        if (note % 12 == 0) {
            snprintf(buf, sizeof buf, "C%d", note / 12 - 1);
            label(layer, o.x1 + R.kx + 4, y1 - 4, o.x1 + R.gx - 10, y2 + 4, buf, aos_font_tiny, 0x303038, LV_TEXT_ALIGN_RIGHT);
            rect(layer, o.x1 + R.gx, y2, o.x1 + R.gx + 16 * R.cw + 9, y2, C_LINE, LV_OPA_COVER, 0);
        }
    }
    for (int c = 0; c <= 16; c += 4) {
        int32_t x = o.x1 + R.gx + c * R.cw + (c / 4) * 3 - 2;
        rect(layer, x, o.y1 + R.gy, x, o.y1 + R.gy + R.rows * R.rh - 1, C_LINE, LV_OPA_COVER, 0);
    }
    if (base + 16 > len) {
        int c0 = len - base;
        lv_area_t a;
        r_cell(c0 < 0 ? 0 : c0, R.rows - 1, &a);
        rect(layer, o.x1 + a.x1, o.y1 + R.gy, o.x1 + R.gx + 16 * R.cw + 9, o.y1 + R.gy + R.rows * R.rh - 1, 0x000000, LV_OPA_60, 0);
    }
    /* the other parts, faint, so a line can be written against them */
    for (int part = 0; part < ES_PARTS; part++) {
        bool mine = part == U->part;
        if (mine) continue;
        for (int i = 0; i < t->nn[part]; i++) {
            const es_note_t *n = &t->n[part][i];
            int r = n->note - low;
            if (r < 0 || r >= R.rows) continue;
            int s0 = n->step - base, s1 = n->step + n->len - 1 - base;
            if (s1 < 0 || s0 > 15) continue;
            lv_area_t a0, a1;
            r_cell(s0 < 0 ? 0 : s0, r, &a0);
            r_cell(s1 > 15 ? 15 : s1, r, &a1);
            rect(layer, o.x1 + a0.x1 + 2, o.y1 + a0.y1 + 3, o.x1 + a1.x2 - 2, o.y1 + a0.y2 - 3, ui_part_color(part), LV_OPA_20, 4);
        }
    }
    for (int i = 0; i < t->nn[U->part]; i++) {
        const es_note_t *n = &t->n[U->part][i];
        int r = n->note - low;
        if (r < 0 || r >= R.rows) continue;
        int s0 = n->step - base, s1 = n->step + n->len - 1 - base;
        if (s1 < 0 || s0 > 15) continue;
        lv_area_t a0, a1;
        r_cell(s0 < 0 ? 0 : s0, r, &a0);
        r_cell(s1 > 15 ? 15 : s1, r, &a1);
        lv_opa_t op = (lv_opa_t)(120 + n->vel);
        rect(layer, o.x1 + a0.x1 + 1, o.y1 + a0.y1 + 1, o.x1 + a1.x2 - 1, o.y1 + a0.y2 - 1, pc, op, 5);
        if (n->prob < 100) rect(layer, o.x1 + a0.x1 + 4, o.y1 + a0.y1 + 4, o.x1 + a0.x1 + 9, o.y1 + a0.y2 - 4, 0x000000, LV_OPA_70, 2);
    }
    /* the playhead */
    int play = es_play_step(U->e);
    if (play >= 0 && es_play_pat(U->e) == U->pat && play >= base && play < base + 16) {
        lv_area_t a;
        r_cell(play - base, 0, &a);
        rect(layer, o.x1 + a.x1, o.y1 + R.gy, o.x1 + a.x1 + 2, o.y1 + R.gy + R.rows * R.rh - 1, 0xFFFFFF, LV_OPA_80, 0);
    }
    /* the lane: velocity or probability of what starts on each step */
    int32_t ly = o.y1 + R.lane_y;
    rect(layer, o.x1 + R.gx, ly, o.x1 + R.gx + 16 * R.cw + 9, ly + R.lane_h, C_PANEL, LV_OPA_COVER, 8);
    label(layer, o.x1 + R.kx, ly, o.x1 + R.gx - 8, ly + R.lane_h, U->lane ? _("Prob.") : _("Vel."), aos_font_tiny, C_DIM, LV_TEXT_ALIGN_RIGHT);
    for (int c = 0; c < 16 && base + c < len; c++) {
        int vmax = -1;
        for (int i = 0; i < t->nn[U->part]; i++) {
            const es_note_t *n = &t->n[U->part][i];
            if (n->step != base + c) continue;
            int v = U->lane ? n->prob * 127 / 100 : n->vel;
            if (v > vmax) vmax = v;
        }
        if (vmax < 0) continue;
        lv_area_t a;
        r_cell(c, 0, &a);
        int32_t hh = (R.lane_h - 8) * vmax / 127;
        rect(layer, o.x1 + a.x1 + 4, ly + R.lane_h - 4 - hh, o.x1 + a.x2 - 4, ly + R.lane_h - 4, pc, LV_OPA_COVER, 3);
    }
}

static void r_lane_set(int c, int32_t y)
{
    es_pat_t *t = &U->p->pat[U->pat];
    float v = 1.0f - (float)(y - R.lane_y - 4) / (float)(R.lane_h - 8);
    v = es_clampf(v, 0.05f, 1.0f);
    for (int i = 0; i < t->nn[U->part]; i++) {
        es_note_t *n = &t->n[U->part][i];
        if (n->step != U->page * 16 + c) continue;
        if (U->lane) n->prob = (uint8_t)(v * 100.0f);
        else n->vel = (uint8_t)(v * 127.0f);
    }
}

static void r_touch(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *g = lv_event_get_current_target_obj(e);
    lv_point_t pt;
    lv_indev_get_point(lv_indev_active(), &pt);
    lv_area_t o;
    lv_obj_get_coords(g, &o);
    int32_t x = pt.x - o.x1, y = pt.y - o.y1;
    es_pat_t *t = &U->p->pat[U->pat];
    int base = U->page * 16;
    int c = r_col_at(x);
    int row = R.rows - 1 - (y - R.gy) / R.rh;
    int note = r_low() + row;
    bool in_grid = y >= R.gy && y < R.gy + R.rows * R.rh && c >= 0;

    if (code == LV_EVENT_PRESSED) {
        R.mode = R_NONE;
        R.moved = 0;
        if (y >= R.lane_y && c >= 0) { R.mode = R_LANE; r_lane_set(c, y); goto changed; }
        if (x < R.gx) { R.mode = R_SCROLL; R.scroll_y0 = y; R.scroll_low0 = r_low(); return; }
        if (!in_grid || base + c >= t->len) return;
        int s = base + c;
        /* a note under the finger: its body moves it, its last cell stretches it */
        for (int i = 0; i < t->nn[U->part]; i++) {
            es_note_t *n = &t->n[U->part][i];
            if (n->note != note || s < n->step || s >= n->step + n->len) continue;
            R.idx = i;
            R.off = s - n->step;
            R.start_step = n->step;
            R.start_note = n->note;
            R.mode = (n->len > 1 && s == n->step + n->len - 1) ? R_SIZE : R_MOVE;
            es_cmd(U->e, EC_AUDITION, U->part, n->note, n->vel, 180);
            return;
        }
        int i = es_pat_add_note(t, U->part, s, U->last_len < 1 ? 1 : U->last_len, note, 100);
        if (i < 0) { aos_ui_toast(_("Esta parte ya tiene todas las notas que entran"), 1500); return; }
        if (s + t->n[U->part][i].len > t->len) t->n[U->part][i].len = (uint8_t)(t->len - s);
        R.idx = i;
        R.off = 0;
        R.mode = R_SIZE;
        es_cmd(U->e, EC_AUDITION, U->part, note, 100, 180);
        goto changed;
    } else if (code == LV_EVENT_PRESSING) {
        if (R.mode == R_SCROLL) {
            int d = (y - R.scroll_y0) / R.rh;
            int low = R.scroll_low0 + d;
            low = low < 0 ? 0 : low > 127 - R.rows ? 127 - R.rows : low;
            if (low != r_low()) { U->roll_low[U->part] = low; lv_obj_invalidate(g); }
            return;
        }
        if (R.mode == R_LANE) { if (c >= 0) r_lane_set(c, y < R.lane_y ? R.lane_y : y); goto changed; }
        if (R.mode != R_SIZE && R.mode != R_MOVE) return;
        if (R.idx >= t->nn[U->part]) { R.mode = R_NONE; return; }
        es_note_t *n = &t->n[U->part][R.idx];
        int s = base + (c < 0 ? 0 : c);
        if (R.mode == R_SIZE) {
            int l = s - n->step + 1;
            if (l < 1) l = 1;
            if (n->step + l > t->len) l = t->len - n->step;
            if (l != n->len) { n->len = (uint8_t)l; U->last_len = l; R.moved = 1; goto changed; }
        } else {
            int ns = s - R.off;
            if (ns < 0) ns = 0;
            if (ns + n->len > t->len) ns = t->len - n->len;
            if (ns < 0) ns = 0;
            int nn = row < 0 ? r_low() : row >= R.rows ? r_low() + R.rows - 1 : note;
            if (ns != n->step || nn != n->note) {
                if (nn != n->note) es_cmd(U->e, EC_AUDITION, U->part, nn, n->vel, 120);
                n->step = (uint8_t)ns;
                n->note = (uint8_t)nn;
                R.moved = 1;
                goto changed;
            }
        }
        return;
    } else if (code == LV_EVENT_RELEASED) {
        if (R.mode == R_MOVE && !R.moved && R.idx < t->nn[U->part]) {
            es_pat_del_note(t, U->part, R.idx);
            R.mode = R_NONE;
            goto changed;
        }
        if (R.mode == R_MOVE && R.idx < t->nn[U->part]) U->last_len = t->n[U->part][R.idx].len;
        R.mode = R_NONE;
        return;
    }
    return;
changed:
    es_ui_dirty();
    lv_obj_invalidate(g);
}

static void r_part_cb(lv_event_t *e)
{
    U->part = (int)(intptr_t)lv_event_get_user_data(e);
    es_set_live_part(U->e, U->part);
    es_ui_show_view(V_ROLL);
}

static void r_lane_cb(lv_event_t *e)
{
    U->lane = (int)(intptr_t)lv_event_get_user_data(e);
    ui_seg_set(R.lane_seg, U->lane, aos_theme_accent_rgb());
    if (U->grid) lv_obj_invalidate(U->grid);
}

static void r_clear_yes(void)
{
    es_pat_t *t = &U->p->pat[U->pat];
    t->nn[U->part] = 0;
    es_ui_dirty();
    if (U->grid) lv_obj_invalidate(U->grid);
}

static void r_clear_cb(lv_event_t *e)
{
    (void)e;
    char m[120];
    snprintf(m, sizeof m, _("Borrar las notas de la parte %d en el patrón %c."), U->part + 1, 'A' + U->pat);
    ui_confirm(m, _("Borrar"), r_clear_yes);
}

static void r_oct_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    int low = r_low() + d * 12;
    low = low < 0 ? 0 : low > 127 - R.rows ? 127 - R.rows : low;
    U->roll_low[U->part] = low;
    if (U->grid) lv_obj_invalidate(U->grid);
}

static void r_refresh(void)
{
    static int last = -2;
    int s = es_play_step(U->e);
    es_pat_chips_refresh(s_chips);
    if (s != last && U->grid) lv_obj_invalidate(U->grid);
    last = s;
}

static void r_forget(void)
{
    forget_common();
    memset(R.part_btn, 0, sizeof R.part_btn);
    R.lane_seg = NULL;
}

void es_view_roll(lv_obj_t *body)
{
    int32_t W = lv_obj_get_width(body), H = lv_obj_get_height(body);
    lv_obj_t *hdr = header(body, W);
    if (!U->land) ui_box(hdr, lv_pct(100), 0);     /* standing up the parts start a line */
    for (int i = 0; i < ES_PARTS; i++) {
        char b[4];
        snprintf(b, sizeof b, "%d", i + 1);
        R.part_btn[i] = ui_btn(hdr, b, aos_font_small, 54, 54, r_part_cb, (void *)(intptr_t)i);
        ui_btn_on(R.part_btn[i], i == U->part, ui_part_color(i));
    }
    static const char *LANES[2];
    LANES[0] = _("Velocidad"); LANES[1] = _("Probab.");
    R.lane_seg = ui_seg(hdr, LANES, 2, U->lane, 250, 54, r_lane_cb, aos_theme_accent_rgb());
    ui_btn(hdr, AOS_SYM_CHEVRON_UP, &aos_sym_28, 54, 54, r_oct_cb, (void *)1);
    ui_btn(hdr, AOS_SYM_CHEVRON_DOWN, &aos_sym_28, 54, 54, r_oct_cb, (void *)-1);
    ui_btn(hdr, AOS_SYM_DELETE, &aos_sym_28, 54, 54, r_clear_cb, NULL);
    lv_obj_update_layout(body);
    int32_t top = 8 + lv_obj_get_height(hdr) + 10;
    int32_t gh = H - top - 50;
    lv_obj_t *g = ui_box(body, W - 24, gh);
    lv_obj_set_pos(g, 12, top);
    lv_obj_add_flag(g, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g, r_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_add_event_cb(g, r_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g, r_touch, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g, r_touch, LV_EVENT_RELEASED, NULL);
    U->grid = g;
    int32_t gw = W - 24;
    R.kx = 0;
    R.gx = U->land ? 74 : 64;
    R.cw = (gw - R.gx - 12) / 16;
    R.lane_h = U->land ? 80 : 110;
    R.gy = 0;
    R.rows = U->land ? 24 : 36;
    R.rh = (gh - R.lane_h - 10) / R.rows;
    R.lane_y = R.gy + R.rows * R.rh + 10;
    if (r_low() > 127 - R.rows) U->roll_low[U->part] = 127 - R.rows;
    U->refresh = r_refresh;
    U->forget = r_forget;
}
