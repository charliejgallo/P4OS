/*
 * P4OS - CAN: the two views of the spy.
 *
 * "Tramas" is one label per visible row, refilled from the ring when a frame
 * comes (as Terminal does with its lines): it costs the same at ten frames a
 * second as at two thousand. Dragging it pauses it and scrolls back.
 *
 * "Por id" is a row per id, sorted, with each data byte in a label of its
 * own so the ones that change can be lit: amber behind it for the third of
 * a second after it changed, amber letters for three seconds. A tap opens
 * the id: its 64 bits, the ones that ever moved marked, and what to do with
 * it (show only it, hide it, send it, draw a byte).
 *
 * Paused, both show a copy taken at the moment of the pause, so what is on
 * screen holds still while the bus goes on.
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

#define MAX_ROWS 48

/* the pause's copy */
static cn_frame_t *s_snap;
static uint32_t s_snap_seq;
static cn_id_t s_snap_ids[CN_IDS];
static int s_snap_nids;
static uint32_t s_snap_ms;

static void *snap_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

void cn_spy_free(void)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    heap_caps_free(s_snap);
#else
    free(s_snap);
#endif
    s_snap = NULL;
}

void cn_live_pause(bool on)
{
    if (!on) return;
    if (!s_snap) s_snap = snap_alloc(sizeof(cn_frame_t) * CN_RING);
    cn_lock();
    if (s_snap) {
        uint32_t lo = CB.seq > CN_RING ? CB.seq - CN_RING : 0;
        for (uint32_t s = lo; s < CB.seq; s++) s_snap[s % CN_RING] = CB.ring[s % CN_RING];
    }
    s_snap_seq = CB.seq;
    s_snap_nids = CB.nids;
    memcpy(s_snap_ids, CB.ids, sizeof(cn_id_t) * (size_t)CB.nids);
    cn_unlock();
    s_snap_ms = cn_ms();
}

/* ================= Tramas ================= */

static struct {
    lv_obj_t *view, *rows[MAX_ROWS], *follow, *empty;
    int nrows, row_h;
    uint32_t back;              /* frames skipped from the newest (paused) */
    uint32_t shown_seq, shown_back;
    int32_t drag_acc;
    bool ascii;
} L;

static void follow_cb(lv_event_t *e)
{
    (void)e;
    L.back = 0;
    if (U.paused) cn_toggle_pause();
}

/* The newest n frames that pass the view's filter, 'back' of them skipped,
 * oldest first. */
static int collect(cn_frame_t *out, int n, uint32_t back, uint32_t *seq_out)
{
    static cn_frame_t tmp[MAX_ROWS];
    const cn_frame_t *ring;
    uint32_t seq;
    bool live = !U.paused || !s_snap;
    if (live) {
        cn_lock();
        ring = CB.ring;
        seq = CB.seq;
    } else {
        ring = s_snap;
        seq = s_snap_seq;
    }
    uint32_t lo = seq > CN_RING ? seq - CN_RING : 0;
    int got = 0;
    for (uint32_t s = seq; s > lo && got < n;) {
        s--;
        const cn_frame_t *f = &ring[s % CN_RING];
        if (cn_view_hidden(f->id, f->fl)) continue;
        if (back) { back--; continue; }
        tmp[got++] = *f;
    }
    if (live) cn_unlock();
    for (int i = 0; i < got; i++) out[i] = tmp[got - 1 - i];
    *seq_out = seq;
    return got;
}

void cn_live_refresh(bool force)
{
    if (U.tab != CN_TAB_LIVE || !L.view) return;
    static cn_frame_t rows[MAX_ROWS];
    uint32_t seq;
    if (!force && U.paused && L.back == L.shown_back) return;
    int n = collect(rows, L.nrows, U.paused ? L.back : 0, &seq);
    if (U.paused && n < L.nrows && L.back) {
        /* scrolled past the oldest: keep the view full */
        uint32_t short_by = (uint32_t)(L.nrows - n);
        L.back = L.back > short_by ? L.back - short_by : 0;
        n = collect(rows, L.nrows, L.back, &seq);
    }
    if (!force && seq == L.shown_seq && L.back == L.shown_back) return;
    L.shown_seq = seq;
    L.shown_back = L.back;
    uint64_t t0 = CB.t0_us;
    char line[128], id[12], data[32];
    for (int r = 0; r < L.nrows; r++) {
        lv_obj_t *l = L.rows[r];
        if (r >= n) {
            lv_label_set_text(l, "");
            continue;
        }
        const cn_frame_t *f = &rows[r];
        uint64_t dt = f->t_us > t0 ? f->t_us - t0 : 0;
        cn_fmt_id(f, id, sizeof id);
        cn_fmt_data(f, data, sizeof data);
        int k = snprintf(line, sizeof line, "%5u.%03u %s %-8s  %u  %-23s", (unsigned)(dt / 1000000),
                         (unsigned)(dt / 1000 % 1000), f->fl & CN_TX ? ">" : " ", id, f->len, data);
        if (L.ascii && !(f->fl & CN_RTR) && k < (int)sizeof line - 12) {
            line[k++] = ' ';
            line[k++] = ' ';
            for (int i = 0; i < f->len; i++) line[k++] = f->data[i] >= 32 && f->data[i] < 127 ? (char)f->data[i] : '.';
            line[k] = 0;
        }
        lv_label_set_text(l, line);
        lv_obj_set_style_text_color(l, f->fl & CN_TX ? lv_color_hex(0x7FB8FF)
                                       : f->fl & CN_RTR ? lv_color_hex(0xC9A3FF)
                                       : lv_color_hex(0xE6EDF3), 0);
    }
    if (n == 0) lv_obj_remove_flag(L.empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(L.empty, LV_OBJ_FLAG_HIDDEN);
    if (U.paused) lv_obj_remove_flag(L.follow, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(L.follow, LV_OBJ_FLAG_HIDDEN);
}

static void live_view_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) { L.drag_acc = 0; return; }
    if (c != LV_EVENT_PRESSING) return;
    lv_point_t v;
    lv_indev_get_vect(lv_indev_active(), &v);
    L.drag_acc += v.y;
    int steps = L.drag_acc / L.row_h;
    if (!steps) return;
    L.drag_acc -= steps * L.row_h;
    if (!U.paused) {
        L.back = 0;
        cn_toggle_pause();
    }
    int64_t b = (int64_t)L.back + steps;        /* drag down = older */
    if (b < 0) b = 0;
    if (b > CN_RING) b = CN_RING;
    L.back = (uint32_t)b;
    cn_live_refresh(true);
}

void cn_live_build(lv_obj_t *parent)
{
    memset(&L, 0, sizeof L);
    int32_t w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
    L.view = lv_obj_create(parent);
    lv_obj_remove_style_all(L.view);
    lv_obj_set_size(L.view, w, h);
    lv_obj_set_style_bg_color(L.view, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(L.view, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(L.view, 16, 0);
    lv_obj_remove_flag(L.view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(L.view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(L.view, live_view_cb, LV_EVENT_ALL, NULL);

    const lv_font_t *mono = &aos_mono_18;
    int32_t cw = lv_font_get_glyph_width(mono, '0', 0);
    L.ascii = cw > 0 && (w - 24) / cw >= 58;
    lv_obj_t *hd = aos_label(L.view, "", mono, AOS_C_DIM);
    lv_label_set_text(hd, L.ascii ? _("   tiempo   id        n  datos                    ascii")
                                  : _("   tiempo   id        n  datos"));
    lv_obj_set_pos(hd, 10, 8);
    L.row_h = lv_font_get_line_height(mono) + 3;
    L.nrows = LV_MIN((h - 50) / L.row_h, MAX_ROWS);
    for (int r = 0; r < L.nrows; r++) {
        lv_obj_t *l = lv_label_create(L.view);
        lv_obj_set_style_text_font(l, mono, 0);
        lv_obj_set_size(l, w - 20, L.row_h);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_CLIP);
        lv_label_set_text(l, "");
        lv_obj_set_pos(l, 10, 40 + r * L.row_h);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        L.rows[r] = l;
    }
    L.empty = aos_label(L.view, "", aos_font_small, AOS_C_DIM);
    lv_label_set_text(L.empty, CB.open ? _("Esperando tramas…") : _("Sin bus. Conectalo arriba."));
    lv_obj_center(L.empty);
    L.follow = cn_btn(parent, AOS_SYM_PLAY, _("Seguir"), follow_cb, NULL);
    lv_obj_set_style_bg_color(L.follow, AOS_C_ACCENT, 0);
    lv_obj_align(L.follow, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
    lv_obj_add_flag(L.follow, LV_OBJ_FLAG_HIDDEN);
    cn_live_refresh(true);
}

/* ================= Por id ================= */

#define ID_ROWS 40

static struct {
    lv_obj_t *view, *empty, *hint;
    struct { lv_obj_t *row, *id, *b[8], *hz, *n; } r[ID_ROWS];
    int nrows, row_h, top;
    int32_t drag_acc;
    bool dragged;
    cn_id_t *ids;               /* a sorted copy */
    int nids;
    const lv_font_t *font;
    /* the open id */
    bool detail;
    uint32_t d_id;
    uint8_t d_fl;
    lv_obj_t *d_sum, *d_data, *d_bits[64];
    bool d_intel;
    lv_obj_t *d_order;
} I;

static int id_cmp(const void *a, const void *b)
{
    const cn_id_t *x = a, *y = b;
    if ((x->fl & CN_EXT) != (y->fl & CN_EXT)) return (x->fl & CN_EXT) ? 1 : -1;
    return x->id < y->id ? -1 : x->id > y->id;
}

static uint32_t copy_ids(void)
{
    static cn_id_t all[CN_IDS];
    int n;
    uint32_t now;
    if (U.paused) {
        n = s_snap_nids;
        memcpy(all, s_snap_ids, sizeof(cn_id_t) * (size_t)n);
        now = s_snap_ms;
    } else {
        cn_lock();
        n = CB.nids;
        memcpy(all, CB.ids, sizeof(cn_id_t) * (size_t)n);
        cn_unlock();
        now = cn_ms();
    }
    if (!I.ids) I.ids = all;
    I.nids = 0;
    for (int i = 0; i < n; i++)
        if (!cn_view_hidden(all[i].id, all[i].fl)) all[I.nids++] = all[i];
    qsort(all, (size_t)I.nids, sizeof(cn_id_t), id_cmp);
    return now;
}

static void detail_refresh(uint32_t now);

void cn_ids_refresh(bool force)
{
    (void)force;
    if (U.tab != CN_TAB_IDS || !I.view) return;
    uint32_t now = copy_ids();
    if (I.top > I.nids - I.nrows) I.top = LV_MAX(0, I.nids - I.nrows);
    char t[24];
    for (int r = 0; r < I.nrows; r++) {
        int k = I.top + r;
        if (k >= I.nids) {
            lv_obj_add_flag(I.r[r].row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(I.r[r].row, LV_OBJ_FLAG_HIDDEN);
        const cn_id_t *e = &I.ids[k];
        cn_frame_t f = { .id = e->id, .fl = e->fl };
        cn_fmt_id(&f, t, sizeof t);
        lv_label_set_text(I.r[r].id, t);
        for (int b = 0; b < 8; b++) {
            lv_obj_t *l = I.r[r].b[b];
            if (b >= e->len) {
                lv_label_set_text(l, "");
                lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
                continue;
            }
            snprintf(t, sizeof t, "%02X", e->data[b]);
            lv_label_set_text(l, t);
            uint32_t age = e->chg_ms[b] ? now - e->chg_ms[b] : UINT32_MAX;
            bool hot = age < 300, warm = age < 3000;
            lv_obj_set_style_bg_opa(l, hot ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(l, hot ? lv_color_black() : warm ? AOS_C_ORANGE : lv_color_hex(0xE6EDF3), 0);
        }
        unsigned hz10 = (unsigned)(e->hz * 10 + 0.5f);
        snprintf(t, sizeof t, "%u.%u", hz10 / 10, hz10 % 10);
        lv_label_set_text(I.r[r].hz, t);
        snprintf(t, sizeof t, "%u", (unsigned)e->count);
        lv_label_set_text(I.r[r].n, t);
    }
    if (I.nids) lv_obj_add_flag(I.empty, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(I.empty, LV_OBJ_FLAG_HIDDEN);
    char hint[96];
    if (U.only_on || U.nhidden) {
        snprintf(hint, sizeof hint, U.only_on ? _("Filtro: sólo un id") : _("Filtro: %d ocultos"), U.nhidden);
        lv_label_set_text(I.hint, hint);
    } else {
        if (I.nids == 1) snprintf(hint, sizeof hint, "%s", _("1 id"));
        else snprintf(hint, sizeof hint, _("%d ids"), I.nids);
        lv_label_set_text(I.hint, hint);
    }
    if (I.detail && U.sheet) detail_refresh(now);
    else I.detail = false;
}

static void open_detail(const cn_id_t *e);

static void ids_view_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) {
        I.drag_acc = 0;
        I.dragged = false;
        return;
    }
    if (c == LV_EVENT_PRESSING) {
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        I.drag_acc += v.y;
        if (LV_ABS(I.drag_acc) > 12) I.dragged = true;
        int steps = I.drag_acc / I.row_h;
        if (!steps) return;
        I.drag_acc -= steps * I.row_h;
        I.top -= steps;
        if (I.top > I.nids - I.nrows) I.top = I.nids - I.nrows;
        if (I.top < 0) I.top = 0;
        cn_ids_refresh(true);
        return;
    }
    if (c == LV_EVENT_CLICKED && !I.dragged) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_area_t a;
        lv_obj_get_coords(I.r[0].row, &a);
        int r = (p.y - a.y1) / I.row_h;
        if (p.y >= a.y1 && r >= 0 && r < I.nrows && I.top + r < I.nids) open_detail(&I.ids[I.top + r]);
    }
}

void cn_ids_build(lv_obj_t *parent)
{
    cn_id_t *keep = I.ids;
    memset(&I, 0, sizeof I);
    I.ids = keep;
    int32_t w = lv_obj_get_width(parent), h = lv_obj_get_height(parent);
    I.view = lv_obj_create(parent);
    lv_obj_remove_style_all(I.view);
    lv_obj_set_size(I.view, w, h);
    lv_obj_set_style_bg_color(I.view, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(I.view, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(I.view, 16, 0);
    lv_obj_remove_flag(I.view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(I.view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(I.view, ids_view_cb, LV_EVENT_ALL, NULL);

    /* the widest that fits: 8 + 2 + 24 + 8 + 9 characters */
    I.font = &aos_mono_22;
    int32_t cw = lv_font_get_glyph_width(I.font, '0', 0);
    if (cw <= 0 || 53 * cw > w - 24) {
        I.font = &aos_mono_18;
        cw = lv_font_get_glyph_width(I.font, '0', 0);
    }
    if (cw <= 0) cw = 11;
    int32_t x_id = 12, x_b = x_id + 10 * cw, bw = 3 * cw, x_hz = x_b + 8 * bw + cw, x_n = x_hz + 8 * cw;
    int32_t lh = lv_font_get_line_height(I.font);
    I.row_h = lh + 10;

    lv_obj_t *hd = lv_obj_create(I.view);
    lv_obj_remove_style_all(hd);
    lv_obj_set_size(hd, w, 34);
    lv_obj_set_pos(hd, 0, 6);
    aos_make_decorative(hd);
    lv_obj_set_pos(aos_label(hd, _("id"), aos_font_caption, AOS_C_DIM), x_id, 4);
    lv_obj_set_pos(aos_label(hd, _("datos"), aos_font_caption, AOS_C_DIM), x_b, 4);
    lv_obj_set_pos(aos_label(hd, _("por s"), aos_font_caption, AOS_C_DIM), x_hz, 4);
    lv_obj_set_pos(aos_label(hd, _("vistas"), aos_font_caption, AOS_C_DIM), x_n, 4);

    I.nrows = LV_MIN((h - 44 - 40) / I.row_h, ID_ROWS);
    for (int r = 0; r < I.nrows; r++) {
        lv_obj_t *row = lv_obj_create(I.view);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, w - 8, I.row_h);
        lv_obj_set_pos(row, 4, 44 + r * I.row_h);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x0E131A), 0);
        lv_obj_set_style_bg_opa(row, r % 2 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(row, 8, 0);
        aos_make_decorative(row);
        I.r[r].row = row;
        I.r[r].id = aos_label(row, "", I.font, AOS_C_TEAL);
        lv_obj_set_pos(I.r[r].id, x_id - 4, 5);
        for (int b = 0; b < 8; b++) {
            lv_obj_t *l = aos_label(row, "", I.font, AOS_C_TEXT);
            lv_obj_set_size(l, 2 * cw + 6, lh);
            lv_obj_set_style_pad_hor(l, 3, 0);
            lv_obj_set_style_radius(l, 4, 0);
            lv_obj_set_style_bg_color(l, AOS_C_ORANGE, 0);
            lv_obj_set_pos(l, x_b - 4 + b * bw - 3, 5);
            I.r[r].b[b] = l;
        }
        I.r[r].hz = aos_label(row, "", I.font, AOS_C_DIM);
        lv_obj_set_pos(I.r[r].hz, x_hz - 4, 5);
        I.r[r].n = aos_label(row, "", I.font, AOS_C_DIM);
        lv_obj_set_pos(I.r[r].n, x_n - 4, 5);
        aos_make_decorative(row);
    }
    I.hint = aos_label(I.view, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(I.hint, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    I.empty = aos_label(I.view, "", aos_font_small, AOS_C_DIM);
    lv_label_set_text(I.empty, CB.open ? _("Esperando tramas…") : _("Sin bus. Conectalo arriba."));
    lv_obj_center(I.empty);
    cn_ids_refresh(true);
}

/* ---- one id ---- */

static const cn_id_t *find_id(uint32_t id, uint8_t fl)
{
    for (int i = 0; i < I.nids; i++)
        if (I.ids[i].id == id && I.ids[i].fl == fl) return &I.ids[i];
    return NULL;
}

static void detail_refresh(uint32_t now)
{
    const cn_id_t *e = find_id(I.d_id, I.d_fl);
    if (!e) return;
    char buf[160];
    unsigned hz10 = (unsigned)(e->hz * 10 + 0.5f), per = e->period_us / 100;
    snprintf(buf, sizeof buf, _("%u bytes · %u.%u por segundo · cada %u.%u ms · %u vistas"), e->len, hz10 / 10,
             hz10 % 10, per / 10, per % 10, (unsigned)e->count);
    lv_label_set_text(I.d_sum, buf);
    cn_frame_t f = { .id = e->id, .fl = e->fl, .len = e->len };
    memcpy(f.data, e->data, 8);
    cn_fmt_data(&f, buf, sizeof buf);
    lv_label_set_text(I.d_data, buf[0] ? buf : "—");
    for (int b = 0; b < 8; b++) {
        for (int k = 0; k < 8; k++) {
            lv_obj_t *c = I.d_bits[b * 8 + k];
            if (!c) continue;
            int bit = 7 - k;
            bool in = b < e->len, moved = e->changed[b] >> bit & 1;
            bool hot = moved && e->chg_ms[b] && now - e->chg_ms[b] < 300;
            lv_label_set_text(lv_obj_get_child(c, 0), in ? (e->data[b] >> bit & 1 ? "1" : "0") : "");
            lv_obj_set_style_bg_color(c, !in ? lv_color_hex(0x15191F) : hot ? AOS_C_YELLOW : moved ? AOS_C_ORANGE
                                                                                                    : AOS_C_CARD2, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(c, 0), moved && in ? lv_color_black() : AOS_C_TEXT, 0);
        }
    }
}

static void only_cb(lv_event_t *e)
{
    (void)e;
    U.only_on = true;
    U.only_id = I.d_id;
    U.only_fl = I.d_fl;
    cn_sheet_close();
    I.detail = false;
    cn_ids_refresh(true);
}

static void hide_cb(lv_event_t *e)
{
    (void)e;
    if (U.nhidden < 16) {
        U.hidden[U.nhidden] = I.d_id;
        U.hidden_fl[U.nhidden++] = I.d_fl;
    }
    cn_sheet_close();
    I.detail = false;
    cn_ids_refresh(true);
}

static void showall_cb(lv_event_t *e)
{
    (void)e;
    U.only_on = false;
    U.nhidden = 0;
    cn_sheet_close();
    I.detail = false;
    cn_ids_refresh(true);
}

static void tosend_cb(lv_event_t *e)
{
    (void)e;
    const cn_id_t *d = find_id(I.d_id, I.d_fl);
    cn_frame_t f = { .id = I.d_id, .fl = I.d_fl };
    if (d) {
        f.len = d->len;
        memcpy(f.data, d->data, 8);
    }
    cn_sheet_close();
    I.detail = false;
    cn_ui_set_tab(CN_TAB_SEND);
    cn_send_load(&f);
}

static void order_cb(lv_event_t *e)
{
    (void)e;
    I.d_intel = !I.d_intel;
    lv_label_set_text(lv_obj_get_child(I.d_order, 0), I.d_intel ? _("16 bits: Intel") : _("16 bits: Motorola"));
}

static void graph_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    int byte = v & 7, bits = v & 8 ? 16 : 8;
    uint32_t id = I.d_id;
    uint8_t fl = I.d_fl;
    bool motorola = !I.d_intel;
    cn_sheet_close();
    I.detail = false;
    cn_sig_adhoc(id, fl, byte, bits, motorola);
    cn_ui_set_tab(CN_TAB_SIG);
}

static lv_obj_t *row_of(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

static void open_detail(const cn_id_t *e)
{
    char title[48], id[12];
    cn_frame_t f = { .id = e->id, .fl = e->fl };
    cn_fmt_id(&f, id, sizeof id);
    snprintf(title, sizeof title, e->fl & CN_EXT ? _("Id %s (29 bits)") : _("Id %s"), id);
    lv_obj_t *body = cn_sheet_open(title);
    I.detail = true;
    I.d_id = e->id;
    I.d_fl = e->fl;
    memset(I.d_bits, 0, sizeof I.d_bits);

    I.d_sum = aos_label(body, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(I.d_sum, LV_PCT(100));
    lv_label_set_long_mode(I.d_sum, LV_LABEL_LONG_MODE_WRAP);
    I.d_data = aos_label(body, "", &aos_mono_22, AOS_C_TEXT);

    /* the 64 bits: a row a byte, bit 7 on the left */
    int32_t cell = LV_MIN((U.W - 2 * AOS_UI_PAD - 70) / 8 - 6, 64);
    lv_obj_t *grid = lv_obj_create(body);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(grid, 6, 0);
    lv_obj_t *hr = row_of(grid);
    lv_obj_set_width(hr, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(hr, 6, 0);
    lv_obj_t *sp = aos_label(hr, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(sp, 60);
    for (int k = 0; k < 8; k++) {
        char t[4];
        snprintf(t, sizeof t, "%d", 7 - k);
        lv_obj_t *l = aos_label(hr, t, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(l, cell);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    for (int b = 0; b < 8; b++) {
        lv_obj_t *r = row_of(grid);
        lv_obj_set_width(r, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_gap(r, 6, 0);
        char t[8];
        snprintf(t, sizeof t, "B%d", b);
        lv_obj_t *l = aos_label(r, t, aos_font_small, AOS_C_DIM);
        lv_obj_set_width(l, 60);
        for (int k = 0; k < 8; k++) {
            lv_obj_t *c = lv_obj_create(r);
            lv_obj_remove_style_all(c);
            lv_obj_set_size(c, cell, LV_MIN(cell, 44));
            lv_obj_set_style_radius(c, 6, 0);
            lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
            lv_obj_center(aos_label(c, "", &aos_mono_18, AOS_C_TEXT));
            I.d_bits[b * 8 + k] = c;
        }
    }
    lv_obj_t *legend = aos_label(body, _("Naranja: bits que cambiaron alguna vez; amarillo: recién."), aos_font_caption,
                                 AOS_C_DIM);
    lv_obj_set_width(legend, LV_PCT(100));
    lv_label_set_long_mode(legend, LV_LABEL_LONG_MODE_WRAP);

    lv_obj_t *acts = row_of(body);
    cn_btn(acts, AOS_SYM_MAGNIFY, _("Sólo este id"), only_cb, NULL);
    cn_btn(acts, AOS_SYM_CLOSE, _("Ocultarlo"), hide_cb, NULL);
    if (U.only_on || U.nhidden) cn_btn(acts, NULL, _("Mostrar todos"), showall_cb, NULL);
    cn_btn(acts, AOS_SYM_SEND, _("Llevar a Enviar"), tosend_cb, NULL);

    lv_obj_t *gt = aos_label(body, _("Graficar un byte, o dos juntos:"), aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(gt, LV_PCT(100));
    lv_obj_t *g8 = row_of(body);
    for (int b = 0; b < e->len; b++) {
        char t[8];
        snprintf(t, sizeof t, "B%d", b);
        cn_pill(g8, t, false, graph_cb, (void *)(intptr_t)b);
    }
    lv_obj_t *g16 = row_of(body);
    I.d_intel = false;
    I.d_order = cn_pill(g16, _("16 bits: Motorola"), true, order_cb, NULL);
    for (int b = 0; b + 1 < e->len; b++) {
        char t[12];
        snprintf(t, sizeof t, "B%d-%d", b, b + 1);
        cn_pill(g16, t, false, graph_cb, (void *)(intptr_t)(b | 8));
    }
    detail_refresh(U.paused ? s_snap_ms : cn_ms());
}
