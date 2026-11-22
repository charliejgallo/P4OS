/*
 * Estudio - Tocar: the keyboard, the scale grid and the drum pads.
 *
 * These surfaces do not use LVGL's pointer, which is one finger: a timer
 * reads every finger (aos_touch_points_n, up to five) every 10 ms and hits
 * them against the keys itself, like the Macro pad's MIDI face. Each finger
 * owns the key it is on; sliding moves the note (a glissando, note off the
 * old key and on the new one), sliding off the surface lets go. A finger
 * that was already down when the surface appeared (it opened a tab, a
 * sheet closed under it) is ignored until it lifts. The surfaces are plain
 * objects drawn in one draw event; LVGL never sees them as clickable.
 *
 * The velocity is one of three chips, not where the finger lands: on keys
 * this size the height of a touch is noise.
 */
#include "es_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define U es_ui

static const uint8_t VEL[3] = { 54, 92, 122 };
static const uint8_t WHITE[7] = { 0, 2, 4, 5, 7, 9, 11 };

static const uint8_t SC_MAJOR[] = { 0, 2, 4, 5, 7, 9, 11 };
static const uint8_t SC_MINOR[] = { 0, 2, 3, 5, 7, 8, 10 };
static const uint8_t SC_PENTA[] = { 0, 2, 4, 7, 9 };
static const uint8_t SC_PENTM[] = { 0, 3, 5, 7, 10 };
static const uint8_t SC_BLUES[] = { 0, 3, 5, 6, 7, 10 };
static const uint8_t SC_DORIC[] = { 0, 2, 3, 5, 7, 9, 10 };
static const struct { const char *name; const uint8_t *deg; int n; } SCALES[] = {
    { N_("Mayor"), SC_MAJOR, 7 }, { N_("Menor"), SC_MINOR, 7 }, { N_("Pentatónica"), SC_PENTA, 5 },
    { N_("Pentat. menor"), SC_PENTM, 5 }, { N_("Blues"), SC_BLUES, 6 }, { N_("Dórica"), SC_DORIC, 7 },
};
#define NSCALES ((int)(sizeof SCALES / sizeof SCALES[0]))

static const char *const ARP_NAMES[ARP_COUNT] = { N_("Arriba"), N_("Abajo"), N_("Ida y vuelta"),
                                                  N_("Al azar"), N_("Como se toca") };
static const char *const ARP_RATES[AR_COUNT] = { "1/8", "1/8T", "1/16", "1/16T", "1/32" };
static const char *const NOTE12[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

typedef struct { lv_area_t r; int id; bool black; } hit_t;

static struct {
    lv_obj_t *surf, *preset, *oct_lbl, *part_btn[ES_PARTS];
    lv_obj_t *arp_btn, *arp_mode, *arp_rate, *arp_oct, *hold_btn, *scale_btn, *root_btn;
    hit_t     hit[48];
    int       nhit;
    uint8_t   held[128];
    uint8_t   seq[128];
    uint32_t  hits[ES_DRUMS];
    uint32_t  flash[ES_DRUMS];
    struct { uint8_t id; int cur; } f[AOS_TOUCH_MAX];
    uint8_t   stale[AOS_TOUCH_MAX];
    bool      covered;
} P;

/* ---- what is under a finger ---- */

static int key_base(void) { return 12 * (U->octave + 1); }

static void layout_keys(int32_t w, int32_t h)
{
    P.nhit = 0;
    int nw = 15;
    float ww = (float)w / (float)nw;
    int base = key_base();
    /* the black ones first: they are on top */
    for (int i = 0; i < nw - 1; i++) {
        int m = i % 7;
        if (m == 2 || m == 6) continue;
        float cx = ww * (float)(i + 1);
        int32_t bw = (int32_t)(ww * 0.62f);
        hit_t *k = &P.hit[P.nhit++];
        k->r = (lv_area_t){ (int32_t)cx - bw / 2, 0, (int32_t)cx + bw / 2, (int32_t)((float)h * 0.6f) };
        k->id = base + 12 * (i / 7) + WHITE[m] + 1;
        k->black = true;
    }
    for (int i = 0; i < nw; i++) {
        hit_t *k = &P.hit[P.nhit++];
        k->r = (lv_area_t){ (int32_t)(ww * (float)i) + 2, 0, (int32_t)(ww * (float)(i + 1)) - 2, h - 1 };
        k->id = base + 12 * (i / 7) + WHITE[i % 7];
        k->black = false;
    }
}

static int grid_cols(void) { return U->land ? 8 : 6; }
static int grid_rows(void) { return U->land ? 4 : 6; }

static int grid_note(int r, int c)
{
    int s = U->scale < 0 || U->scale >= NSCALES ? 0 : U->scale;
    int n = SCALES[s].n;
    int step = n <= 5 ? 2 : 3;
    int deg = c + r * step;
    return key_base() + U->key_root + SCALES[s].deg[deg % n] + 12 * (deg / n);
}

static void layout_grid(int32_t w, int32_t h)
{
    P.nhit = 0;
    int cols = grid_cols(), rows = grid_rows();
    int32_t g = 8, cw = (w - g * (cols - 1)) / cols, ch = (h - g * (rows - 1)) / rows;
    for (int r = 0; r < rows; r++)
        for (int c = 0; c < cols; c++) {
            hit_t *k = &P.hit[P.nhit++];
            int32_t x = c * (cw + g), y = (rows - 1 - r) * (ch + g);
            k->r = (lv_area_t){ x, y, x + cw - 1, y + ch - 1 };
            k->id = grid_note(r, c);
            k->black = false;
        }
}

static void layout_pads(int32_t w, int32_t h)
{
    P.nhit = 0;
    int cols = U->land ? 4 : 2, rows = ES_DRUMS / cols;
    int32_t g = 10, cw = (w - g * (cols - 1)) / cols, ch = (h - g * (rows - 1)) / rows;
    for (int i = 0; i < ES_DRUMS; i++) {
        int r = i / cols, c = i % cols;
        hit_t *k = &P.hit[P.nhit++];
        int32_t x = c * (cw + g), y = (rows - 1 - r) * (ch + g);
        k->r = (lv_area_t){ x, y, x + cw - 1, y + ch - 1 };
        k->id = i;
        k->black = false;
    }
}

static void layout(void)
{
    if (!P.surf) return;
    int32_t w = lv_obj_get_width(P.surf), h = lv_obj_get_height(P.surf);
    if (U->play_mode == PM_KEYS) layout_keys(w, h);
    else if (U->play_mode == PM_GRID) layout_grid(w, h);
    else layout_pads(w, h);
}

static int hit_at(int32_t x, int32_t y)
{
    for (int i = 0; i < P.nhit; i++) {
        const lv_area_t *r = &P.hit[i].r;
        if (x >= r->x1 && x <= r->x2 && y >= r->y1 && y <= r->y2) return P.hit[i].id;
    }
    return -1;
}

/* ---- notes ---- */

static void press(int id)
{
    if (U->play_mode == PM_PADS) {
        es_cmd(U->e, EC_DRUM, id, VEL[U->vel_lvl], 0, SRC_LIVE);
        P.flash[id] = (uint32_t)aos_hal_uptime_ms() + 120;
    } else {
        if (id < 0 || id > 127) return;
        if (P.held[id]++ == 0) es_cmd(U->e, EC_NOTE_ON, U->part, id, VEL[U->vel_lvl], SRC_LIVE);
    }
    if (P.surf) lv_obj_invalidate(P.surf);
}

static void release(int id)
{
    if (U->play_mode == PM_PADS || id < 0 || id > 127) return;
    if (P.held[id] && --P.held[id] == 0) es_cmd(U->e, EC_NOTE_OFF, U->part, id, 0, SRC_LIVE);
    if (P.surf) lv_obj_invalidate(P.surf);
}

static void mark_stale(void)
{
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    for (int i = 0; i < AOS_TOUCH_MAX; i++) P.stale[i] = pts[i].down ? pts[i].id : 0;
}

void es_play_release_all(void)
{
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        if (P.f[i].id && P.f[i].cur >= 0) release(P.f[i].cur);
        P.f[i].id = 0;
        P.f[i].cur = -1;
    }
    for (int n = 0; n < 128; n++)
        if (P.held[n]) { P.held[n] = 0; if (U && U->e) es_cmd(U->e, EC_NOTE_OFF, U->part, n, 0, SRC_LIVE); }
    mark_stale();
}

void es_play_touch(void)
{
    if (!P.surf) return;
    bool covered = U->sheet || aos_ui_overlay() != 0;
    if (covered != P.covered) {
        P.covered = covered;
        if (covered) es_play_release_all();
        else mark_stale();
    }
    if (covered) return;
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    lv_area_t a;
    lv_obj_get_coords(P.surf, &a);
    for (int s = 0; s < AOS_TOUCH_MAX; s++) {
        bool there = false;
        for (int i = 0; i < AOS_TOUCH_MAX; i++) if (pts[i].down && pts[i].id == P.stale[s]) there = true;
        if (!there) P.stale[s] = 0;
    }
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        for (int s = 0; s < AOS_TOUCH_MAX && id; s++) if (P.stale[s] == id) id = 0;
        int32_t x = (int32_t)pts[i].x - a.x1, y = (int32_t)pts[i].y - a.y1;
        if (id != P.f[i].id) {
            if (P.f[i].id && P.f[i].cur >= 0) release(P.f[i].cur);
            P.f[i].id = id;
            P.f[i].cur = -1;
            if (id) {
                int t = hit_at(x, y);
                P.f[i].cur = t;
                if (t >= 0) press(t);
            }
        } else if (id) {
            int t = hit_at(x, y);
            if (t != P.f[i].cur) {
                if (P.f[i].cur >= 0) release(P.f[i].cur);
                P.f[i].cur = t;
                if (t >= 0) press(t);
            }
        }
    }
}

/* ---- drawing ---- */

static void rect(lv_layer_t *layer, const lv_area_t *r, uint32_t color, lv_opa_t opa, int32_t radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(layer, &d, r);
}

static void text(lv_layer_t *layer, const lv_area_t *r, const char *s, const lv_font_t *f, uint32_t color)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s;
    d.font = f;
    d.color = lv_color_hex(color);
    d.align = LV_TEXT_ALIGN_CENTER;
    d.text_local = 1;
    lv_area_t a = *r;
    int32_t fh = lv_font_get_line_height(f);
    a.y1 = r->y2 - fh - 8;
    a.y2 = r->y2;
    lv_draw_label(layer, &d, &a);
}

/* 'a' over 'b', t of 256 */
static uint32_t blend(uint32_t a, uint32_t b, int t)
{
    uint32_t r = 0;
    for (int sh = 0; sh < 24; sh += 8) {
        int ca = (int)(a >> sh & 255), cb = (int)(b >> sh & 255);
        r |= (uint32_t)((ca * t + cb * (256 - t)) >> 8) << sh;
    }
    return r;
}

static void surf_draw(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t o;
    lv_obj_get_coords(P.surf, &o);
    uint32_t pc = ui_part_color(U->part);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    char buf[16];
    /* white keys and pads first, black keys on top: the table has the black
     * ones first for the hit test, so it is walked backwards here */
    for (int i = P.nhit - 1; i >= 0; i--) {
        hit_t *k = &P.hit[i];
        lv_area_t r = { o.x1 + k->r.x1, o.y1 + k->r.y1, o.x1 + k->r.x2, o.y1 + k->r.y2 };
        if (U->play_mode == PM_KEYS) {
            bool down = P.held[k->id] != 0, seq = P.seq[k->id] != 0;
            if (k->black) {
                rect(layer, &r, down ? pc : seq ? blend(pc, 0x15151A, 110) : 0x15151A, LV_OPA_COVER, 6);
            } else {
                rect(layer, &r, down ? pc : seq ? blend(pc, 0xEDEDF2, 80) : 0xEDEDF2, LV_OPA_COVER, 8);
                if (k->id % 12 == 0) {
                    snprintf(buf, sizeof buf, "C%d", k->id / 12 - 1);
                    text(layer, &r, buf, aos_font_caption, down ? 0xFFFFFF : 0x6E6E78);
                }
            }
        } else if (U->play_mode == PM_GRID) {
            bool down = P.held[k->id] != 0, seq = P.seq[k->id] != 0;
            bool root = (k->id - U->key_root) % 12 == 0;
            rect(layer, &r, down ? pc : root ? pc : C_CELL2, down ? LV_OPA_COVER : root ? LV_OPA_50 : LV_OPA_COVER, 14);
            if (seq && !down) {
                lv_area_t in = { r.x1 + 6, r.y1 + 6, r.x2 - 6, r.y1 + 12 };
                rect(layer, &in, pc, LV_OPA_COVER, 3);
            }
            snprintf(buf, sizeof buf, "%s%d", NOTE12[k->id % 12], k->id / 12 - 1);
            text(layer, &r, buf, aos_font_small, C_TEXT);
        } else {
            uint32_t c = ui_drum_color(k->id);
            bool lit = (int32_t)(P.flash[k->id] - now) > 0;
            rect(layer, &r, c, lit ? LV_OPA_COVER : LV_OPA_30, 18);
            text(layer, &r, ui_track_name(k->id, false), aos_font_small, lit ? 0x000000 : C_TEXT);
        }
    }
}

/* ---- the controls ---- */

static void preset_text(void)
{
    if (!P.preset) return;
    char buf[ES_NAME + 8];
    snprintf(buf, sizeof buf, "%s", _(U->p->part[U->part].name));
    ui_btn_text(P.preset, buf);
}

static void parts_paint(void)
{
    for (int i = 0; i < ES_PARTS; i++) ui_btn_on(P.part_btn[i], i == U->part, ui_part_color(i));
}

static void part_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i == U->part) return;
    es_play_release_all();
    U->part = i;
    es_set_live_part(U->e, i);
    es_cmd(U->e, EC_ARP_RESET, 0, 0, 0, 0);
    parts_paint();
    preset_text();
    if (P.surf) lv_obj_invalidate(P.surf);
}

static void preset_cb(lv_event_t *e)
{
    (void)e;
    es_ui_show_view(V_SOUND);
}

static void mode_cb(lv_event_t *e)
{
    U->play_mode = (int)(intptr_t)lv_event_get_user_data(e);
    aos_hal_pref_set_i32("es_pmode", U->play_mode);
    es_ui_show_view(V_PLAY);
}

static void oct_text(void)
{
    if (P.oct_lbl) lv_label_set_text_fmt(P.oct_lbl, "C%d", U->octave);
}

static void oct_minus(lv_event_t *e)
{
    (void)e;
    if (U->octave <= 0) return;
    es_play_release_all();
    U->octave--;
    oct_text();
    layout();
    lv_obj_invalidate(P.surf);
}

static void oct_plus(lv_event_t *e)
{
    (void)e;
    if (U->octave >= 7) return;
    es_play_release_all();
    U->octave++;
    oct_text();
    layout();
    lv_obj_invalidate(P.surf);
}

static void vel_cb(lv_event_t *e)
{
    U->vel_lvl = (int)(intptr_t)lv_event_get_user_data(e);
    ui_seg_set(lv_obj_get_parent(lv_event_get_target_obj(e)), U->vel_lvl, aos_theme_accent_rgb());
}

static void arp_paint(void)
{
    es_proj_t *p = U->p;
    ui_btn_on(P.arp_btn, p->arp_on, 0xFF9F0A);
    ui_btn_on(P.hold_btn, p->arp_hold, 0xFF9F0A);
    ui_btn_text(P.arp_mode, _(ARP_NAMES[p->arp_mode % ARP_COUNT]));
    ui_btn_text(P.arp_rate, ARP_RATES[p->arp_rate % AR_COUNT]);
    char b[16];
    snprintf(b, sizeof b, "%d oct", p->arp_oct);
    ui_btn_text(P.arp_oct, b);
}

static void arp_cb(lv_event_t *e)
{
    int w = (int)(intptr_t)lv_event_get_user_data(e);
    es_proj_t *p = U->p;
    es_play_release_all();
    switch (w) {
    case 0: p->arp_on = !p->arp_on; es_cmd(U->e, EC_ARP_RESET, 0, 0, 0, 0); break;
    case 1: p->arp_mode = (uint8_t)((p->arp_mode + 1) % ARP_COUNT); break;
    case 2: p->arp_rate = (uint8_t)((p->arp_rate + 1) % AR_COUNT); break;
    case 3: p->arp_oct = (uint8_t)(p->arp_oct % 3 + 1); break;
    case 4: p->arp_hold = !p->arp_hold; if (!p->arp_hold) es_cmd(U->e, EC_ARP_RESET, 0, 0, 0, 0); break;
    }
    arp_paint();
    es_ui_dirty();
}

static void scale_paint(void)
{
    if (P.scale_btn) ui_btn_text(P.scale_btn, _(SCALES[U->scale % NSCALES].name));
    if (P.root_btn) ui_btn_text(P.root_btn, NOTE12[U->key_root % 12]);
}

static void scale_cb(lv_event_t *e)
{
    int w = (int)(intptr_t)lv_event_get_user_data(e);
    es_play_release_all();
    if (w == 0) U->scale = (U->scale + 1) % NSCALES;
    else U->key_root = (U->key_root + 1) % 12;
    aos_hal_pref_set_i32("es_scale", U->scale | U->key_root << 8);
    scale_paint();
    layout();
    lv_obj_invalidate(P.surf);
}

static void refresh(void)
{
    if (!P.surf) return;
    bool dirty = false;
    if (U->play_mode == PM_PADS) {
        uint32_t now = (uint32_t)aos_hal_uptime_ms();
        for (int t = 0; t < ES_DRUMS; t++) {
            uint32_t h = es_drum_hits(U->e, t);
            if (h != P.hits[t]) { P.hits[t] = h; P.flash[t] = now + 120; dirty = true; }
            if (P.flash[t] && (int32_t)(P.flash[t] - now) <= 0) { P.flash[t] = 0; dirty = true; }
        }
    } else {
        uint8_t notes[ES_VOICES], seq[128];
        memset(seq, 0, sizeof seq);
        int n = es_part_notes(U->e, U->part, notes, ES_VOICES);
        for (int i = 0; i < n; i++) seq[notes[i]] = 1;
        if (memcmp(seq, P.seq, sizeof seq)) { memcpy(P.seq, seq, sizeof seq); dirty = true; }
    }
    if (dirty) lv_obj_invalidate(P.surf);
}

static void forget(void)
{
    es_play_release_all();
    P.surf = P.preset = P.oct_lbl = NULL;
    P.arp_btn = P.arp_mode = P.arp_rate = P.arp_oct = P.hold_btn = P.scale_btn = P.root_btn = NULL;
    memset(P.part_btn, 0, sizeof P.part_btn);
    P.nhit = 0;
}

void es_view_play(lv_obj_t *body)
{
    int32_t W = lv_obj_get_width(body), H = lv_obj_get_height(body);
    lv_obj_t *ctl = ui_row(body, W - 24, 10);
    lv_obj_set_pos(ctl, 12, 10);
    int32_t h = 58;

    for (int i = 0; i < ES_PARTS; i++) {
        char b[4];
        snprintf(b, sizeof b, "%d", i + 1);
        P.part_btn[i] = ui_btn(ctl, b, aos_font_body, 58, h, part_cb, (void *)(intptr_t)i);
    }
    parts_paint();
    P.preset = ui_btn(ctl, "", aos_font_small, U->land ? 250 : 300, h, preset_cb, NULL);
    lv_obj_t *pl = lv_obj_get_child(P.preset, 0);
    lv_label_set_long_mode(pl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(pl, (U->land ? 250 : 300) - 16);
    lv_obj_set_style_text_align(pl, LV_TEXT_ALIGN_CENTER, 0);
    preset_text();
    static const char *MODES[3];
    MODES[0] = _("Teclado"); MODES[1] = _("Grilla"); MODES[2] = _("Pads");
    ui_seg(ctl, MODES, 3, U->play_mode, 330, h, mode_cb, aos_theme_accent_rgb());
    ui_stepper(ctl, "C3", 190, h, oct_minus, oct_plus, &P.oct_lbl);
    oct_text();

    static const char *VELS[3];
    VELS[0] = _("Suave"); VELS[1] = _("Medio"); VELS[2] = _("Fuerte");
    ui_seg(ctl, VELS, 3, U->vel_lvl, 300, h, vel_cb, aos_theme_accent_rgb());
    if (U->play_mode != PM_PADS) {
        P.arp_btn = ui_btn(ctl, _("Arpegio"), aos_font_caption, 112, h, arp_cb, (void *)0);
        P.arp_mode = ui_btn(ctl, "", aos_font_caption, 160, h, arp_cb, (void *)1);
        P.arp_rate = ui_btn(ctl, "", aos_font_caption, 86, h, arp_cb, (void *)2);
        P.arp_oct = ui_btn(ctl, "", aos_font_caption, 86, h, arp_cb, (void *)3);
        P.hold_btn = ui_btn(ctl, _("Sostener"), aos_font_caption, 112, h, arp_cb, (void *)4);
        arp_paint();
    }
    if (U->play_mode == PM_GRID) {
        P.scale_btn = ui_btn(ctl, "", aos_font_caption, 170, h, scale_cb, (void *)0);
        P.root_btn = ui_btn(ctl, "", aos_font_caption, 70, h, scale_cb, (void *)1);
        scale_paint();
    }
    lv_obj_update_layout(body);
    int32_t top = 10 + lv_obj_get_height(ctl) + 14;
    /* keep clear of the bottom edge: that is the system's swipe home */
    int32_t sh = H - top - 48;
    if (!U->land && sh > 560) sh = 560;
    int32_t sw = W - 24;
    P.surf = ui_box(body, sw, sh);
    lv_obj_set_pos(P.surf, 12, U->land ? top : top + (H - top - 48 - sh) / 2);
    lv_obj_add_event_cb(P.surf, surf_draw, LV_EVENT_DRAW_MAIN_END, NULL);
    lv_obj_update_layout(body);
    layout();
    memset(P.held, 0, sizeof P.held);
    memset(P.seq, 0, sizeof P.seq);
    for (int t = 0; t < ES_DRUMS; t++) P.hits[t] = es_drum_hits(U->e, t);
    mark_stale();
    U->refresh = refresh;
    U->forget = forget;
}
