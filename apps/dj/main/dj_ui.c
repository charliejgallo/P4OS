/*
 * DJ - the surface: every deck and mixer control, laid out for the
 * orientation, drawn by hand and touched by hand (five fingers).
 *
 * Landscape (the main view): the two waveforms across the top, deck A on
 * the left, the mixer in the middle, deck B on the right, mirrored so the
 * transport sits on the outer sides and the hot cues near the mixer.
 * Portrait: deck A on top, the waveforms and a compact mixer in the
 * middle, deck B below.
 *
 * The left edge and the bottom edge of the screen belong to the system's
 * gestures (back, home): no control starts there.
 *
 * Every 33 ms the UI timer asks each control for a signature of what it
 * shows (a value, a lit state, the seconds of a clock) and invalidates only
 * those that changed; the platters change while they turn, the meters
 * while there is sound. The waveforms are redrawn every time, and the
 * overviews when their playhead moves a pixel.
 */
#include "dj_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define U dj_ui

dj_ui_t *dj_ui;

static const char *const FX_NAMES[FX_COUNT] = { N_("Eco"), N_("Flanger"), N_("Filtro LFO"), N_("Bitcrush") };
static const char *const FX_BEATS_TXT[] = { "1/4", "1/2", "3/4", "1", "2", "4" };

#define REV_SECS 1.8f           /* a platter turn: 33 1/3 rpm */

/* ---- small helpers ---- */

const char *dj_fmt_time(char *buf, size_t n, float secs)
{
    if (secs < 0.0f) secs = 0.0f;
    int s = (int)secs;
    snprintf(buf, n, "%d:%02d", s / 60, s % 60);
    return buf;
}

static uint32_t deck_color(int d) { return d == 0 ? DJ_COL_A : DJ_COL_B; }

int dj_ui_find(int id, int deck)
{
    if (!U || id < 0 || id >= C_COUNT) return -1;
    return U->idx[id][deck < 0 ? DJ_NDECK : deck];
}

static int add(int kind, int id, int deck, int32_t x, int32_t y, int32_t w, int32_t h, const char *label)
{
    if (U->nctl >= DJ_MAXCTL) return -1;
    int i = U->nctl++;
    ctl_t *c = &U->ctl[i];
    memset(c, 0, sizeof *c);
    c->kind = (uint8_t)kind;
    c->id = (uint8_t)id;
    c->deck = (int8_t)deck;
    c->r.x1 = x;
    c->r.y1 = y;
    c->r.x2 = x + w - 1;
    c->r.y2 = y + h - 1;
    c->label = label;
    c->color = deck >= 0 ? deck_color(deck) : 0xFFFFFF;
    c->sig = 0xFFFFFFFFu;
    U->idx[id][deck < 0 ? DJ_NDECK : deck] = i;
    return i;
}

static void set_val(int i, float *v, float lo, float hi, float def)
{
    if (i < 0) return;
    U->ctl[i].val = v;
    U->ctl[i].lo = lo;
    U->ctl[i].hi = hi;
    U->ctl[i].def = def;
}

/* ---- the layout ---- */

/* deck A's layout in landscape, in a panel of 440 px; deck B mirrors it */
static void land_deck(int d, int32_t px, int32_t y0)
{
    const int32_t PW = 440;
    bool mir = d == 1;
#define MX(rx, w) (mir ? px + PW - (rx) - (w) : px + (rx))
    dj_strip_t *s = &U->m->s[d];
    add(K_INFO, C_INFO, d, px + (mir ? 8 : 36), y0, PW - 44, 54, NULL);
    add(K_OVERVIEW, C_OVERVIEW, d, px + (mir ? 8 : 36), y0 + 58, PW - 44, 36, NULL);
    int32_t py = y0 + 102;
    int i = add(K_VFADER, C_PITCH, d, MX(36, 52), py, 52, 250, N_("Pitch"));
    set_val(i, &s->pitch, -1.0f, 1.0f, 0.0f);
    add(K_PLATTER, C_PLATTER, d, MX(98, 252), py - 1, 252, 252, NULL);
    int32_t cx = 358, cw = 76;
    add(K_BTN, C_SYNC, d, MX(cx, cw), py, cw, 46, N_("SYNC"));
    add(K_BTN, C_LOOP, d, MX(cx, cw), py + 52, cw, 46, N_("LOOP"));
    add(K_BTN, mir ? C_LOOP_DBL : C_LOOP_HALF, d, MX(cx, 36), py + 104, 36, 46, mir ? "x2" : "1/2");
    add(K_BTN, mir ? C_LOOP_HALF : C_LOOP_DBL, d, MX(cx + 40, 36), py + 104, 36, 46, mir ? "1/2" : "x2");
    add(K_BTN, mir ? C_JUMP_FWD : C_JUMP_BACK, d, MX(cx, 36), py + 156, 36, 46, NULL);
    add(K_BTN, mir ? C_JUMP_BACK : C_JUMP_FWD, d, MX(cx + 40, 36), py + 156, 36, 46, NULL);
    add(K_BTN, C_LOAD, d, MX(cx, cw), py + 208, cw, 46, N_("CARGAR"));
    int32_t ty = py + 262;
    add(K_BTN, C_CUE, d, MX(36, 92), ty, 92, 66, N_("CUE"));
    add(K_BTN, C_PLAY, d, MX(134, 92), ty, 92, 66, NULL);
    for (int h = 0; h < DJ_HOT; h++) {
        int hh = mir ? DJ_HOT - 1 - h : h;
        add(K_BTN, C_HOT1 + hh, d, MX(232 + h * 51, 46), ty, 46, 66, NULL);
    }
    int32_t fy = ty + 74;
    add(K_BTN, C_FX_TYPE, d, MX(36, 128), fy, 128, 52, NULL);
    add(K_BTN, C_FX_ON, d, MX(170, 64), fy, 64, 52, N_("FX"));
    i = add(K_KNOB, C_FX_AMT, d, MX(240, 60), fy - 6, 60, 64, NULL);
    set_val(i, &s->fx_amt, 0.0f, 1.0f, 0.5f);
    add(K_BTN, C_FX_BEATS, d, MX(306, 128), fy, 128, 52, NULL);
#undef MX
}

static void land_mixer(int32_t mx, int32_t y0)
{
    const int32_t MW = 400;
    for (int d = 0; d < DJ_NDECK; d++) {
        bool mir = d == 1;
#define MX(rx, w) (mir ? mx + MW - (rx) - (w) : mx + (rx))
        dj_strip_t *s = &U->m->s[d];
        static const int IDS[4] = { C_EQ_HI, C_EQ_MID, C_EQ_LO, C_FILTER };
        static const char *const LBL[4] = { N_("AGUDOS"), N_("MEDIOS"), N_("GRAVES"), N_("FILTRO") };
        for (int k = 0; k < 4; k++) {
            int i = add(K_KNOB, IDS[k], d, MX(10, 68), y0 + 4 + k * 84, 68, 82, LBL[k]);
            set_val(i, k < 3 ? &s->eq[2 - k] : &s->filt, -1.0f, 1.0f, 0.0f);
        }
        int i = add(K_VFADER, C_VOL, d, MX(94, 52), y0 + 10, 52, 310, NULL);
        set_val(i, &s->vol, 0.0f, 1.0f, 0.8f);
        add(K_VU, C_VU, d, MX(152, 14), y0 + 10, 14, 310, NULL);
#undef MX
    }
    int i = add(K_KNOB, C_MASTER, -1, mx + 166, y0 + 252, 68, 82, N_("MÁSTER"));
    set_val(i, &U->m->master, 0.0f, 1.0f, 0.85f);
    add(K_VU, C_MVU, -1, mx + 186, y0 + 10, 28, 230, NULL);
    add(K_BTN, C_LIB, -1, mx + 10, y0 + 346, 120, 52, NULL);
    add(K_BTN, C_REC, -1, mx + 140, y0 + 346, 120, 52, NULL);
    add(K_BTN, C_SETTINGS, -1, mx + 270, y0 + 346, 120, 52, NULL);
    i = add(K_HFADER, C_XFADE, -1, mx + 30, y0 + 418, MW - 60, 66, NULL);
    set_val(i, &U->m->xfade, -1.0f, 1.0f, 0.0f);
}

/* a deck upright: the full width, 440 px tall */
static void port_deck(int d, int32_t y0)
{
    const int32_t W = U->W;
    dj_strip_t *s = &U->m->s[d];
    add(K_INFO, C_INFO, d, 36, y0 + 4, W - 44, 54, NULL);
    add(K_OVERVIEW, C_OVERVIEW, d, 36, y0 + 62, W - 44, 34, NULL);
    int32_t py = y0 + 102;
    add(K_PLATTER, C_PLATTER, d, 36, py, 262, 262, NULL);
    int i = add(K_VFADER, C_PITCH, d, W - 58, py, 52, 262, N_("Pitch"));
    set_val(i, &s->pitch, -1.0f, 1.0f, 0.0f);
    int32_t gx = 312, gw = W - 58 - 10 - gx;      /* the column between */
    int32_t hw = (gw - 8) / 2, qw = (gw - 24) / 4;
    add(K_BTN, C_SYNC, d, gx, py, hw, 50, N_("SYNC"));
    add(K_BTN, C_LOOP, d, gx + hw + 8, py, hw, 50, N_("LOOP"));
    add(K_BTN, C_LOOP_HALF, d, gx, py + 56, qw, 50, "1/2");
    add(K_BTN, C_LOOP_DBL, d, gx + qw + 8, py + 56, qw, 50, "x2");
    add(K_BTN, C_JUMP_BACK, d, gx + 2 * (qw + 8), py + 56, qw, 50, NULL);
    add(K_BTN, C_JUMP_FWD, d, gx + 3 * (qw + 8), py + 56, qw, 50, NULL);
    add(K_BTN, C_FX_TYPE, d, gx, py + 112, hw, 50, NULL);
    add(K_BTN, C_FX_BEATS, d, gx + hw + 8, py + 112, hw, 50, NULL);
    add(K_BTN, C_FX_ON, d, gx, py + 168, hw - 70, 50, N_("FX"));
    i = add(K_KNOB, C_FX_AMT, d, gx + hw - 64, py + 162, 60, 64, NULL);
    set_val(i, &s->fx_amt, 0.0f, 1.0f, 0.5f);
    add(K_BTN, C_LOAD, d, gx + hw + 8, py + 168, hw, 50, N_("CARGAR"));
    int32_t ty = py + 272;
    int32_t bw = (W - 44 - 5 * 8) / 6;
    add(K_BTN, C_CUE, d, 36, ty, bw, 60, N_("CUE"));
    add(K_BTN, C_PLAY, d, 36 + (bw + 8), ty, bw, 60, NULL);
    for (int h = 0; h < DJ_HOT; h++) add(K_BTN, C_HOT1 + h, d, 36 + (2 + h) * (bw + 8), ty, bw, 60, NULL);
}

static void port_mixer(int32_t y0)
{
    const int32_t W = U->W;
    static const int IDS[4] = { C_EQ_HI, C_EQ_MID, C_EQ_LO, C_FILTER };
    static const char *const LBL[4] = { N_("AGUDOS"), N_("MEDIOS"), N_("GRAVES"), N_("FILTRO") };
    for (int d = 0; d < DJ_NDECK; d++) {
        dj_strip_t *s = &U->m->s[d];
        for (int k = 0; k < 4; k++) {
            int32_t x = d == 0 ? 32 + k * 70 : W - 6 - (4 - k) * 70;
            int i = add(K_KNOB, IDS[k], d, x, y0, 64, 80, LBL[k]);
            set_val(i, k < 3 ? &s->eq[2 - k] : &s->filt, -1.0f, 1.0f, 0.0f);
        }
        int32_t fx = d == 0 ? 36 : W / 2 + 14;
        int i = add(K_HFADER, C_VOL, d, fx, y0 + 84, W / 2 - 50, 42, NULL);
        set_val(i, &s->vol, 0.0f, 1.0f, 0.8f);
        add(K_VU, C_VU, d, fx + 20, y0 + 128, W / 2 - 90, 6, NULL);
    }
    int i = add(K_KNOB, C_MASTER, -1, W / 2 - 34, y0, 68, 80, N_("MÁSTER"));
    set_val(i, &U->m->master, 0.0f, 1.0f, 0.85f);
    add(K_BTN, C_LIB, -1, 36, y0 + 140, 86, 52, NULL);
    i = add(K_HFADER, C_XFADE, -1, 130, y0 + 138, W - 130 - 206, 56, NULL);
    set_val(i, &U->m->xfade, -1.0f, 1.0f, 0.0f);
    add(K_BTN, C_REC, -1, W - 198, y0 + 140, 112, 52, NULL);
    add(K_BTN, C_SETTINGS, -1, W - 80, y0 + 140, 72, 52, NULL);
}

static void layout(void)
{
    U->nctl = 0;
    for (int i = 0; i < C_COUNT; i++)
        for (int d = 0; d <= DJ_NDECK; d++) U->idx[i][d] = -1;
    if (U->land) {
        int32_t wh = 176;
        U->wave_r = (lv_area_t){ 36, 4, U->W - 9, wh - 1 };
        add(K_WAVE, C_WAVE, 0, 36, 4, U->W - 44, wh / 2 - 6, NULL);
        add(K_WAVE, C_WAVE, 1, 36, wh / 2 + 2, U->W - 44, wh / 2 - 6, NULL);
        land_deck(0, 0, wh + 6);
        land_deck(1, U->W - 440, wh + 6);
        land_mixer(440, wh + 6);
    } else {
        port_deck(0, 0);
        int32_t wy = 444;
        U->wave_r = (lv_area_t){ 36, wy, U->W - 9, wy + 125 };
        add(K_WAVE, C_WAVE, 0, 36, wy, U->W - 44, 60, NULL);
        add(K_WAVE, C_WAVE, 1, 36, wy + 66, U->W - 44, 60, NULL);
        port_mixer(wy + 134);
        port_deck(1, U->H - 440 - 48);
    }
}

/* ---- drawing ---- */

static lv_area_t org;           /* the surface's corner on the screen */

static lv_area_t abs_r(const lv_area_t *r)
{
    lv_area_t a = { r->x1 + org.x1, r->y1 + org.y1, r->x2 + org.x1, r->y2 + org.y1 };
    return a;
}

static void rect(lv_layer_t *l, const lv_area_t *r, uint32_t color, lv_opa_t opa, int32_t radius)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex(color);
    d.bg_opa = opa;
    d.radius = radius;
    lv_draw_rect(l, &d, r);
}

static void ring(lv_layer_t *l, int32_t cx, int32_t cy, int32_t rad, int32_t w, uint32_t color, lv_opa_t opa)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_TRANSP;
    d.border_color = lv_color_hex(color);
    d.border_opa = opa;
    d.border_width = w;
    d.radius = LV_RADIUS_CIRCLE;
    lv_area_t a = { cx - rad, cy - rad, cx + rad, cy + rad };
    lv_draw_rect(l, &d, &a);
}

static void disc(lv_layer_t *l, int32_t cx, int32_t cy, int32_t rad, uint32_t color, lv_opa_t opa)
{
    lv_area_t a = { cx - rad, cy - rad, cx + rad, cy + rad };
    rect(l, &a, color, opa, LV_RADIUS_CIRCLE);
}

static void text(lv_layer_t *l, const lv_area_t *r, const char *s, const lv_font_t *f, uint32_t color,
                 lv_text_align_t align, bool local)
{
    lv_draw_label_dsc_t d;
    lv_draw_label_dsc_init(&d);
    d.text = s;
    d.font = f;
    d.color = lv_color_hex(color);
    d.align = align;
    d.text_local = local;
    /* one line: what does not fit is cut at the area's edge */
    d.flag = LV_TEXT_FLAG_EXPAND;
    lv_draw_label(l, &d, r);
}

/* a text centred vertically in 'r' */
static void text_mid(lv_layer_t *l, const lv_area_t *r, const char *s, const lv_font_t *f, uint32_t color, bool local)
{
    int32_t fh = lv_font_get_line_height(f);
    lv_area_t a = *r;
    a.y1 = r->y1 + (lv_area_get_height(r) - fh) / 2;
    a.y2 = a.y1 + fh;
    text(l, &a, s, f, color, LV_TEXT_ALIGN_CENTER, local);
}

static void line(lv_layer_t *l, float x1, float y1, float x2, float y2, int32_t w, uint32_t color, lv_opa_t opa)
{
    lv_draw_line_dsc_t d;
    lv_draw_line_dsc_init(&d);
    d.p1.x = x1;
    d.p1.y = y1;
    d.p2.x = x2;
    d.p2.y = y2;
    d.width = w;
    d.color = lv_color_hex(color);
    d.opa = opa;
    d.round_start = d.round_end = 1;
    lv_draw_line(l, &d);
}

static void arc(lv_layer_t *l, int32_t cx, int32_t cy, int32_t rad, int32_t w, float a0, float a1, uint32_t color)
{
    if (a1 < a0) { float t = a0; a0 = a1; a1 = t; }
    if (a1 - a0 < 0.5f) return;
    lv_draw_arc_dsc_t d;
    lv_draw_arc_dsc_init(&d);
    d.center.x = cx;
    d.center.y = cy;
    d.radius = (uint16_t)rad;
    d.width = (uint16_t)w;
    d.start_angle = a0;
    d.end_angle = a1;
    d.color = lv_color_hex(color);
    d.rounded = 1;
    lv_draw_arc(l, &d);
}

static uint32_t mixc(uint32_t a, uint32_t b, int t)     /* t of 256 */
{
    int r = (((a >> 16) & 255) * (256 - t) + ((b >> 16) & 255) * t) >> 8;
    int g = (((a >> 8) & 255) * (256 - t) + ((b >> 8) & 255) * t) >> 8;
    int bl = ((a & 255) * (256 - t) + (b & 255) * t) >> 8;
    return (uint32_t)(r << 16 | g << 8 | bl);
}

static const dj_pub_t *P(int d) { return dj_pub(U->e, d); }
static dj_track_t *T(int d) { return dj_deck_track(U->e, d); }
static bool loaded(int d) { return T(d)->state == DJ_TS_READY && P(d)->gen == T(d)->gen && P(d)->gen != 0; }

static const uint32_t HOTC[DJ_HOT] = { 0xFF453A, 0x30D158, 0xBF5AF2, 0xFFD60A };

/* the label and the colours of a button now */
static void btn_state(const ctl_t *c, char *lbl, size_t n, uint32_t *bg, uint32_t *fg, const lv_font_t **font, bool *lit)
{
    int d = c->deck;
    *bg = DJ_COL_BTN;
    *fg = 0xFFFFFF;
    *font = aos_font_caption;
    *lit = false;
    snprintf(lbl, n, "%s", c->label ? _(c->label) : "");
    switch (c->id) {
    case C_PLAY:
        snprintf(lbl, n, "%s", P(d)->playing ? AOS_SYM_PAUSE : AOS_SYM_PLAY);
        *font = &aos_sym_44;
        if (P(d)->playing && !P(d)->cue_held) { *bg = 0x1E6B34; *fg = 0x30D158; *lit = true; }
        else if (loaded(d) && (U->blink & 16)) *fg = 0x30D158;
        break;
    case C_CUE:
        *font = aos_font_small;
        if (P(d)->cue_held) { *bg = 0x7A4A00; *fg = 0xFFB340; *lit = true; }
        else if (loaded(d) && !P(d)->playing && fabsf((float)P(d)->pos - P(d)->cue) < 64.0f) *fg = 0xFFB340;
        break;
    case C_SYNC:
        if (P(d)->sync) { *bg = 0x0E5A66; *fg = 0x40C8E0; *lit = true; }
        break;
    case C_HOT1: case C_HOT2: case C_HOT3: case C_HOT4: {
        int h = c->id - C_HOT1;
        snprintf(lbl, n, "%d", h + 1);
        *font = aos_font_small;
        if (loaded(d) && P(d)->hot[h] >= 0.0f) { *bg = mixc(HOTC[h], 0x000000, 150); *fg = 0xFFFFFF; *lit = true; }
        else *fg = DJ_COL_DIM;
        break;
    }
    case C_LOOP: {
        float b = P(d)->loop_beats;
        if (b >= 1.0f) snprintf(lbl, n, "%s %d", _("LOOP"), (int)(b + 0.5f));
        else snprintf(lbl, n, "%s 1/%d", _("LOOP"), (int)(1.0f / b + 0.5f));
        if (P(d)->loop_on) { *bg = 0x1E6B34; *fg = 0x30D158; *lit = true; }
        break;
    }
    case C_JUMP_BACK: case C_JUMP_FWD:
        snprintf(lbl, n, "%s", c->id == C_JUMP_BACK ? AOS_SYM_CHEVRON_LEFT : AOS_SYM_CHEVRON_RIGHT);
        *font = &aos_sym_28;
        break;
    case C_LOAD:
        *font = aos_font_tiny;
        break;
    case C_FX_TYPE: {
        int t = U->m->s[d].fx_type;
        snprintf(lbl, n, "%s", _(FX_NAMES[t < 0 || t >= FX_COUNT ? 0 : t]));
        break;
    }
    case C_FX_ON:
        if (U->m->s[d].fx_on) { *bg = mixc(c->color, 0x000000, 120); *lit = true; }
        break;
    case C_FX_BEATS: {
        int b = U->m->s[d].fx_beats;
        snprintf(lbl, n, "%s %s", FX_BEATS_TXT[b < 0 || b >= DJ_FX_NBEATS ? 3 : b], _("tiempo"));
        break;
    }
    case C_LIB:
        snprintf(lbl, n, "%s", AOS_SYM_FOLDER_MUSIC);
        *font = &aos_sym_28;
        break;
    case C_SETTINGS:
        snprintf(lbl, n, "%s", AOS_SYM_COG);
        *font = &aos_sym_28;
        break;
    case C_REC:
        if (dj_rec_on(U->e)) {
            char t[16];
            snprintf(lbl, n, "%s", dj_fmt_time(t, sizeof t, (float)dj_rec_ms(U->e) / 1000.0f));
            *bg = (U->blink & 16) ? 0x8A1F1A : 0x5A1410;
            *fg = 0xFFFFFF;
            *font = aos_font_small;
            *lit = true;
        } else {
            snprintf(lbl, n, "%s %s", "\xE2\x97\x8F", _("GRABAR"));
            *fg = 0xFF453A;
        }
        break;
    default:
        break;
    }
    if (U->learn == 2 && U->learn_ctl >= 0 && &U->ctl[U->learn_ctl] == c) *bg = 0x5E3A8A;
}

static void draw_btn(lv_layer_t *l, const ctl_t *c, const lv_area_t *r)
{
    char lbl[48];
    uint32_t bg, fg;
    const lv_font_t *font;
    bool lit;
    btn_state(c, lbl, sizeof lbl, &bg, &fg, &font, &lit);
    if (c->lit) bg = mixc(bg, 0xFFFFFF, 40);
    rect(l, r, bg, LV_OPA_COVER, 10);
    if (c->id >= C_HOT1 && c->id <= C_HOT4) {
        int h = c->id - C_HOT1;
        lv_area_t b = { r->x1 + 6, r->y2 - 7, r->x2 - 6, r->y2 - 4 };
        rect(l, &b, HOTC[h], loaded(c->deck) && P(c->deck)->hot[h] >= 0.0f ? LV_OPA_COVER : LV_OPA_30, 2);
    }
    text_mid(l, r, lbl, font, fg, true);
}

static void draw_knob(lv_layer_t *l, const ctl_t *c, const lv_area_t *r)
{
    int32_t w = lv_area_get_width(r);
    int32_t rad = w / 2 - 6;
    int32_t cx = r->x1 + w / 2, cy = r->y1 + rad + 6;
    float v = c->val ? dj_clampf((*c->val - c->lo) / (c->hi - c->lo), 0.0f, 1.0f) : 0.0f;
    bool bip = c->lo < 0.0f;
    uint32_t col = c->deck >= 0 ? c->color : 0xFFFFFF;
    if (c->id == C_FILTER) col = 0xBF5AF2;
    if (c->id == C_FX_AMT) col = U->m->s[c->deck].fx_on ? col : DJ_COL_DIM;
    disc(l, cx, cy, rad, c->lit ? 0x3A3A40 : 0x26262A, LV_OPA_COVER);
    arc(l, cx, cy, rad, 5, 135.0f, 405.0f, 0x3A3A3E);
    float a = 135.0f + 270.0f * v;
    if (bip) arc(l, cx, cy, rad, 5, 270.0f, a, col);
    else arc(l, cx, cy, rad, 5, 135.0f, a, col);
    float rr = a * 3.14159265f / 180.0f;
    line(l, (float)cx + cosf(rr) * (float)(rad - 18), (float)cy + sinf(rr) * (float)(rad - 18),
         (float)cx + cosf(rr) * (float)(rad - 6), (float)cy + sinf(rr) * (float)(rad - 6), 4, 0xFFFFFF, LV_OPA_COVER);
    if (c->label && lv_area_get_height(r) > w) {
        lv_area_t t = { r->x1 - 8, r->y2 - 17, r->x2 + 8, r->y2 };
        text(l, &t, _(c->label), aos_font_tiny, DJ_COL_DIM, LV_TEXT_ALIGN_CENTER, false);
    }
    if (c->id == C_FX_AMT) {
        char b[8];
        snprintf(b, sizeof b, "%d", (int)(v * 100.0f + 0.5f));
        lv_area_t t = { cx - 20, cy - 10, cx + 20, cy + 10 };
        text(l, &t, b, aos_font_tiny, 0xFFFFFF, LV_TEXT_ALIGN_CENTER, true);
    }
}

static void draw_vfader(lv_layer_t *l, const ctl_t *c, const lv_area_t *r)
{
    int32_t w = lv_area_get_width(r), h = lv_area_get_height(r);
    int32_t cx = r->x1 + w / 2;
    bool pitch = c->id == C_PITCH;
    int32_t top = r->y1 + 18, bot = r->y2 - (pitch ? 24 : 18);
    lv_area_t tr = { cx - 3, top, cx + 2, bot };
    rect(l, &tr, 0x050505, LV_OPA_COVER, 3);
    /* ticks */
    for (int k = 0; k <= 10; k++) {
        int32_t y = top + (bot - top) * k / 10;
        lv_area_t tk = { r->x1 + 4, y, r->x1 + (k == 5 && pitch ? 16 : 10), y + 1 };
        rect(l, &tk, 0x555555, LV_OPA_COVER, 0);
        tk.x1 = r->x2 - (k == 5 && pitch ? 16 : 10);
        tk.x2 = r->x2 - 4;
        rect(l, &tk, 0x555555, LV_OPA_COVER, 0);
    }
    float v = c->val ? dj_clampf((*c->val - c->lo) / (c->hi - c->lo), 0.0f, 1.0f) : 0.0f;
    int32_t y = bot - (int32_t)((float)(bot - top) * v);
    lv_area_t cap = { r->x1 + 2, y - 14, r->x2 - 2, y + 14 };
    rect(l, &cap, c->lit ? 0x5A5A60 : 0x3A3A3E, LV_OPA_COVER, 6);
    lv_area_t mark = { r->x1 + 6, y - 1, r->x2 - 6, y + 1 };
    rect(l, &mark, c->deck >= 0 ? c->color : 0xFFFFFF, LV_OPA_COVER, 1);
    if (pitch) {
        char b[16];
        float pct = *c->val * U->m->range * 100.0f;
        snprintf(b, sizeof b, "%+.1f", (double)pct);
        lv_area_t t = { r->x1 - 10, r->y2 - 20, r->x2 + 10, r->y2 };
        text(l, &t, b, aos_font_tiny, fabsf(pct) < 0.05f ? DJ_COL_DIM : 0xFFFFFF, LV_TEXT_ALIGN_CENTER, true);
        lv_area_t t2 = { r->x1, r->y1, r->x2, r->y1 + 16 };
        char rb[12];
        snprintf(rb, sizeof rb, "%d%%", (int)(U->m->range * 100.0f + 0.5f));
        text(l, &t2, rb, aos_font_tiny, DJ_COL_DIM, LV_TEXT_ALIGN_CENTER, true);
    }
    (void)h;
}

static void draw_hfader(lv_layer_t *l, const ctl_t *c, const lv_area_t *r)
{
    int32_t h = lv_area_get_height(r);
    int32_t cy = r->y1 + h / 2;
    bool xf = c->id == C_XFADE;
    int32_t left = r->x1 + 20, right = r->x2 - 20;
    lv_area_t tr = { left, cy - 3, right, cy + 2 };
    rect(l, &tr, 0x050505, LV_OPA_COVER, 3);
    if (xf) {
        lv_area_t a = { r->x1, r->y1, r->x1 + 16, r->y2 };
        text_mid(l, &a, "A", aos_font_caption, DJ_COL_A, false);
        lv_area_t b = { r->x2 - 16, r->y1, r->x2, r->y2 };
        text_mid(l, &b, "B", aos_font_caption, DJ_COL_B, false);
        lv_area_t mid = { (left + right) / 2 - 1, r->y1 + 6, (left + right) / 2 + 1, r->y2 - 6 };
        rect(l, &mid, 0x555555, LV_OPA_COVER, 0);
    }
    float v = c->val ? dj_clampf((*c->val - c->lo) / (c->hi - c->lo), 0.0f, 1.0f) : 0.0f;
    int32_t x = left + (int32_t)((float)(right - left) * v);
    int32_t cw = xf ? 22 : 16;
    lv_area_t cap = { x - cw, r->y1 + 2, x + cw, r->y2 - 2 };
    rect(l, &cap, c->lit ? 0x5A5A60 : 0x3A3A3E, LV_OPA_COVER, 6);
    lv_area_t mark = { x - 1, r->y1 + 8, x + 1, r->y2 - 8 };
    rect(l, &mark, c->deck >= 0 ? c->color : 0xFFFFFF, LV_OPA_COVER, 1);
}

static void draw_vu(lv_layer_t *l, const ctl_t *c, const lv_area_t *r)
{
    float v = c->deck >= 0 ? P(c->deck)->vu : dj_master_vu(U->e);
    /* dB, -36..+3 */
    float db = v > 1e-4f ? 20.0f * log10f(v) : -60.0f;
    float lvl = dj_clampf((db + 36.0f) / 39.0f, 0.0f, 1.0f);
    int32_t w = lv_area_get_width(r), h = lv_area_get_height(r);
    bool vert = h > w;
    const int N = vert ? 16 : 20;
    int on = (int)(lvl * (float)N + 0.5f);
    for (int k = 0; k < N; k++) {
        uint32_t col = k >= N - 2 ? 0xFF453A : k >= N - 5 ? 0xFFD60A : 0x30D158;
        lv_area_t s;
        if (vert) {
            int32_t sh = h / N;
            s = (lv_area_t){ r->x1, r->y2 - (k + 1) * sh + 2, r->x2, r->y2 - k * sh };
        } else {
            int32_t sw = w / N;
            s = (lv_area_t){ r->x1 + k * sw, r->y1, r->x1 + (k + 1) * sw - 2, r->y2 };
        }
        rect(l, &s, k < on ? col : 0x222224, LV_OPA_COVER, 1);
    }
}

static void draw_platter(lv_layer_t *l, const ctl_t *c, const lv_area_t *r)
{
    int d = c->deck;
    int32_t w = lv_area_get_width(r);
    int32_t rad = w / 2 - 2;
    int32_t cx = r->x1 + w / 2, cy = r->y1 + w / 2;
    bool on = loaded(d);
    disc(l, cx, cy, rad, 0x2A2A2E, LV_OPA_COVER);                 /* the outer ring: the nudge */
    disc(l, cx, cy, (int32_t)((float)rad * 0.86f), c->lit ? 0x141416 : 0x0C0C0E, LV_OPA_COVER);
    for (int g = 0; g < 4; g++)
        ring(l, cx, cy, (int32_t)((float)rad * (0.78f - 0.09f * (float)g)), 1, 0x2C2C30, LV_OPA_COVER);
    int32_t lab = (int32_t)((float)rad * 0.36f);
    disc(l, cx, cy, lab, on ? mixc(c->color, 0x000000, 70) : 0x3A3A3E, LV_OPA_COVER);
    disc(l, cx, cy, 5, 0xDDDDDD, LV_OPA_COVER);
    if (on) {
        float secs = (float)P(d)->pos / (float)T(d)->sr;
        float turns = secs / REV_SECS;
        float a = (turns - floorf(turns)) * 2.0f * 3.14159265f - 1.5707963f;
        float r0 = (float)lab + 6.0f, r1 = (float)rad * 0.84f;
        line(l, (float)cx + cosf(a) * r0, (float)cy + sinf(a) * r0, (float)cx + cosf(a) * r1, (float)cy + sinf(a) * r1,
             6, 0xFFFFFF, LV_OPA_COVER);
        /* the deck's letter on the label, turning with it */
        lv_area_t t = { cx - 30, cy - 34, cx + 30, cy - 8 };
        text(l, &t, d == 0 ? "A" : "B", aos_font_small, 0xFFFFFF, LV_TEXT_ALIGN_CENTER, false);
    } else {
        lv_area_t t = { cx - 30, cy - 34, cx + 30, cy - 8 };
        text(l, &t, d == 0 ? "A" : "B", aos_font_small, DJ_COL_DIM, LV_TEXT_ALIGN_CENTER, false);
    }
}

static void draw_info(lv_layer_t *l, const ctl_t *c, const lv_area_t *r)
{
    int d = c->deck;
    dj_track_t *t = T(d);
    rect(l, r, DJ_COL_PNL, LV_OPA_COVER, 8);
    lv_area_t tag = { r->x1, r->y1, r->x1 + 5, r->y2 };
    rect(l, &tag, c->color, LV_OPA_COVER, 2);
    int32_t rw = 150;
    lv_area_t a1 = { r->x1 + 14, r->y1 + 3, r->x2 - rw, r->y1 + 30 };
    lv_area_t a2 = { r->x1 + 14, r->y1 + 31, r->x2 - rw, r->y2 - 2 };
    lv_area_t b1 = { r->x2 - rw, r->y1 + 1, r->x2 - 10, r->y1 + 30 };
    lv_area_t b2 = { r->x2 - rw - 40, r->y1 + 31, r->x2 - 10, r->y2 - 2 };
    if (t->state == DJ_TS_LOADING) {
        text(l, &a1, _("Cargando..."), aos_font_small, DJ_COL_DIM, LV_TEXT_ALIGN_LEFT, false);
        return;
    }
    if (t->state == DJ_TS_ERROR) {
        text(l, &a1, t->err, aos_font_small, 0xFF453A, LV_TEXT_ALIGN_LEFT, true);
        text(l, &a2, t->path, aos_font_tiny, DJ_COL_DIM, LV_TEXT_ALIGN_LEFT, true);
        return;
    }
    if (!loaded(d)) {
        text(l, &a1, _("Sin tema"), aos_font_small, DJ_COL_DIM, LV_TEXT_ALIGN_LEFT, false);
        text(l, &a2, _("Tocá CARGAR para elegir uno"), aos_font_tiny, DJ_COL_DIM, LV_TEXT_ALIGN_LEFT, false);
        return;
    }
    text(l, &a1, t->title, aos_font_small, 0xFFFFFF, LV_TEXT_ALIGN_LEFT, true);
    char b[64], t1[16], t2[16];
    float pos = (float)P(d)->pos / (float)t->sr, len = dj_track_secs(t);
    text(l, &a2, t->artist, aos_font_tiny, DJ_COL_DIM, LV_TEXT_ALIGN_LEFT, true);
    float bpm = P(d)->bpm;
    if (bpm > 0.0f) {
        snprintf(b, sizeof b, "%.1f", (double)(bpm * P(d)->rate));
        text(l, &b1, b, aos_font_small, P(d)->sync ? 0x40C8E0 : 0xFFFFFF, LV_TEXT_ALIGN_RIGHT, true);
    } else if (t->ana == DJ_ANA_RUN || t->ana == DJ_ANA_WAIT) {
        int pct = t->nwave ? (int)((float)t->wave_n * 100.0f / (float)t->nwave) : 0;
        snprintf(b, sizeof b, "%s %d%%", _("BPM"), pct);
        text(l, &b1, b, aos_font_caption, DJ_COL_DIM, LV_TEXT_ALIGN_RIGHT, true);
    } else {
        text(l, &b1, "--", aos_font_small, DJ_COL_DIM, LV_TEXT_ALIGN_RIGHT, false);
    }
    snprintf(b, sizeof b, "%s  -%s", dj_fmt_time(t1, sizeof t1, pos), dj_fmt_time(t2, sizeof t2, len - pos));
    bool end = P(d)->playing && len - pos < 30.0f;
    text(l, &b2, b, aos_font_tiny, end && (U->blink & 16) ? 0xFF453A : 0xCCCCCC, LV_TEXT_ALIGN_RIGHT, true);
}

static void draw_cb(lv_event_t *ev)
{
    lv_layer_t *l = lv_event_get_layer(ev);
    uint64_t t0 = aos_hal_uptime_us();
    lv_obj_get_coords(U->surf, &org);
    const lv_area_t *clip = &l->_clip_area;
    if (U->land) {
        /* the mixer's panel */
        lv_area_t mp = { 440 + 4, 182, U->W - 444 - 1, U->H - 4 };
        mp = abs_r(&mp);
        if (!(mp.x2 < clip->x1 || mp.x1 > clip->x2 || mp.y2 < clip->y1 || mp.y1 > clip->y2))
            rect(l, &mp, DJ_COL_PNL, LV_OPA_COVER, 12);
    }
    for (int i = 0; i < U->nctl; i++) {
        ctl_t *c = &U->ctl[i];
        lv_area_t r = abs_r(&c->r);
        if (r.x2 < clip->x1 || r.x1 > clip->x2 || r.y2 < clip->y1 || r.y1 > clip->y2) continue;
        switch (c->kind) {
        case K_BTN:     draw_btn(l, c, &r); break;
        case K_KNOB:    draw_knob(l, c, &r); break;
        case K_VFADER:  draw_vfader(l, c, &r); break;
        case K_HFADER:  draw_hfader(l, c, &r); break;
        case K_VU:      draw_vu(l, c, &r); break;
        case K_PLATTER: draw_platter(l, c, &r); break;
        case K_INFO:    draw_info(l, c, &r); break;
        default: break;
        }
    }
    U->draw_us += (uint32_t)(aos_hal_uptime_us() - t0);
    U->draw_n++;
}

/* ---- what each control shows, as a number ---- */

static uint32_t hash(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

static uint32_t signature(const ctl_t *c)
{
    uint32_t h = 2166136261u ^ (c->lit ? 7u : 0u);
    int d = c->deck;
    if (U->learn == 2 && U->learn_ctl >= 0 && &U->ctl[U->learn_ctl] == c) h = hash(h, 99);
    if (c->val) h = hash(h, (uint32_t)(int32_t)(*c->val * 2000.0f));
    switch (c->kind) {
    case K_BTN: {
        char lbl[48];
        uint32_t bg, fg;
        const lv_font_t *font;
        bool lit;
        btn_state(c, lbl, sizeof lbl, &bg, &fg, &font, &lit);
        h = hash(h, bg);
        h = hash(h, fg);
        for (const char *p = lbl; *p; p++) h = hash(h, (uint8_t)*p);
        break;
    }
    case K_KNOB:
        if (c->id == C_FX_AMT) h = hash(h, U->m->s[d].fx_on);
        break;
    case K_VFADER:
        if (c->id == C_PITCH) h = hash(h, (uint32_t)(U->m->range * 100.0f));
        break;
    case K_VU: {
        float v = d >= 0 ? P(d)->vu : dj_master_vu(U->e);
        float db = v > 1e-4f ? 20.0f * log10f(v) : -60.0f;
        h = hash(h, (uint32_t)(int)(dj_clampf((db + 36.0f) / 39.0f, 0.0f, 1.0f) * 20.0f + 0.5f));
        break;
    }
    case K_PLATTER: {
        h = hash(h, loaded(d));
        if (loaded(d)) {
            float secs = (float)P(d)->pos / (float)T(d)->sr;
            float turns = secs / REV_SECS;
            h = hash(h, (uint32_t)((turns - floorf(turns)) * 360.0f));
        }
        break;
    }
    case K_INFO: {
        dj_track_t *t = T(d);
        h = hash(h, (uint32_t)t->state);
        h = hash(h, t->gen);
        h = hash(h, P(d)->gen);
        h = hash(h, (uint32_t)t->ana);
        h = hash(h, (uint32_t)(P(d)->bpm * P(d)->rate * 10.0f));
        h = hash(h, P(d)->sync);
        if (loaded(d)) h = hash(h, (uint32_t)(P(d)->pos / (int32_t)t->sr));
        if (t->ana != DJ_ANA_DONE && t->nwave) h = hash(h, (uint32_t)((float)t->wave_n * 100.0f / (float)t->nwave));
        if (P(d)->playing) h = hash(h, U->blink & 16);
        break;
    }
    default:
        break;
    }
    return h;
}

void dj_ui_dirty(int i)
{
    if (i < 0 || i >= U->nctl || !U->surf) return;
    lv_area_t r = abs_r(&U->ctl[i].r);
    lv_obj_invalidate_area(U->surf, &r);
}

/* ---- the actions ---- */

static void meta_mark(int d)
{
    U->mt[d].dirty = true;
    U->mt[d].dirty_ms = (uint32_t)aos_hal_uptime_ms();
}

void dj_ui_grid_by_hand(int d)
{
    U->mt[d].hand = true;
    meta_mark(d);
}

void dj_act_press(int i)
{
    if (i < 0 || i >= U->nctl) return;
    ctl_t *c = &U->ctl[i];
    int d = c->deck;
    dj_engine_t *e = U->e;
    switch (c->id) {
    case C_PLAY:       dj_cmd(e, DC_PLAY, d, 0, 0.0f); break;
    case C_CUE:        dj_cmd(e, DC_CUE_DOWN, d, 0, 0.0f); meta_mark(d); break;
    case C_SYNC:       dj_cmd(e, DC_SYNC, d, P(d)->sync ? 0 : 1, 0.0f); break;
    case C_HOT1: case C_HOT2: case C_HOT3: case C_HOT4:
        dj_cmd(e, DC_HOT, d, c->id - C_HOT1, 0.0f);
        meta_mark(d);
        break;
    case C_LOOP:       dj_cmd(e, DC_LOOP, d, 0, P(d)->loop_on ? 0.0f : P(d)->loop_beats); break;
    case C_LOOP_HALF:  dj_cmd(e, DC_LOOP_SIZE, d, 0, 0.0f); break;
    case C_LOOP_DBL:   dj_cmd(e, DC_LOOP_SIZE, d, 1, 0.0f); break;
    case C_JUMP_BACK:  dj_cmd(e, DC_BEATJUMP, d, 0, -P(d)->loop_beats); break;
    case C_JUMP_FWD:   dj_cmd(e, DC_BEATJUMP, d, 0, P(d)->loop_beats); break;
    case C_LOAD:       dj_lib_open(d); break;
    case C_FX_TYPE:
        U->m->s[d].fx_type = (U->m->s[d].fx_type + 1) % FX_COUNT;
        if (U->m->s[d].fx_on && U->m->s[d].fx_type == FX_ECHO && !dj_engine_echo_mem(e, d)) U->m->s[d].fx_on = false;
        break;
    case C_FX_ON:
        if (!U->m->s[d].fx_on && U->m->s[d].fx_type == FX_ECHO && !dj_engine_echo_mem(e, d)) {
            aos_ui_toast(_("Sin memoria"), 1500);
            break;
        }
        U->m->s[d].fx_on = !U->m->s[d].fx_on;
        break;
    case C_FX_BEATS:   U->m->s[d].fx_beats = (U->m->s[d].fx_beats + 1) % DJ_FX_NBEATS; break;
    case C_INFO:       if (loaded(d)) dj_grid_open(d); break;
    case C_LIB:        dj_lib_open(-1); break;
    case C_SETTINGS:   dj_settings_open(); break;
    case C_REC: {
        bool start = !dj_rec_on(e);
        if (start && !U->audio_ok) break;
        if (start && !dj_engine_rec_mem(e)) { aos_ui_toast(_("Sin memoria"), 1500); break; }
        dj_cmd(e, DC_REC, 0, start ? 1 : 0, 0.0f);
        if (start) aos_ui_toast(_("Grabando la mezcla en Grabaciones"), 2500);
        break;
    }
    default:
        break;
    }
    dj_ui_dirty(i);
}

void dj_act_release(int i)
{
    if (i < 0 || i >= U->nctl) return;
    ctl_t *c = &U->ctl[i];
    if (c->id == C_CUE) dj_cmd(U->e, DC_CUE_UP, c->deck, 0, 0.0f);
    dj_ui_dirty(i);
}

void dj_act_value(int i, float v)
{
    if (i < 0 || i >= U->nctl) return;
    ctl_t *c = &U->ctl[i];
    if (!c->val) return;
    *c->val = dj_clampf(v, c->lo, c->hi);
    if (c->id == C_PITCH && P(c->deck)->sync && fabsf(*c->val) > 0.0f) {
        /* the fader takes over from sync */
        dj_cmd(U->e, DC_SYNC, c->deck, 0, 0.0f);
    }
}

/* ---- the fingers ---- */

static int hit(float x, float y)
{
    for (int i = U->nctl - 1; i >= 0; i--) {
        const ctl_t *c = &U->ctl[i];
        if (c->kind == K_VU || c->kind == K_TEXT) continue;
        int32_t pad = c->kind == K_BTN ? 3 : 0;
        if (x >= c->r.x1 - pad && x <= c->r.x2 + pad && y >= c->r.y1 - pad && y <= c->r.y2 + pad) return i;
    }
    return -1;
}

static float platter_angle(const ctl_t *c, float x, float y, float *radius_frac)
{
    float cx = (float)(c->r.x1 + c->r.x2) * 0.5f, cy = (float)(c->r.y1 + c->r.y2) * 0.5f;
    float dx = x - cx, dy = y - cy;
    float R = (float)lv_area_get_width(&c->r) * 0.5f;
    if (radius_frac) *radius_frac = sqrtf(dx * dx + dy * dy) / R;
    return atan2f(dy, dx);
}

/* the source samples a pixel of the waveform is, for deck d */
static float wave_spp(int d)
{
    static const float SECS_PER_PX[4] = { 0.0029f, 0.0058f, 0.0116f, 0.0232f };
    float spx = SECS_PER_PX[U->zoom & 3] * (U->land ? 1.0f : 1280.0f / 720.0f);
    float rate = P(d)->rate > 0.1f ? P(d)->rate : 1.0f;
    return spx * (float)(T(d)->sr ? T(d)->sr : 44100) * rate;
}

static void finger_down(finger_t *f, int i, float x, float y)
{
    f->ctl = (int16_t)i;
    f->x0 = f->lx = x;
    f->y0 = f->ly = y;
    f->t = (uint32_t)aos_hal_uptime_ms();
    if (i < 0) return;
    ctl_t *c = &U->ctl[i];
    c->lit = true;
    if (U->learn == 1) {
        if (c->kind == K_BTN || c->kind == K_KNOB || c->kind == K_VFADER || c->kind == K_HFADER || c->kind == K_PLATTER) {
            U->learn = 2;
            U->learn_ctl = i;
            dj_midi_learn_ctl(i);
            aos_ui_toast(_("Ahora mové o apretá el control del aparato MIDI"), 3000);
        }
        f->ctl = -1;
        c->lit = false;
        dj_ui_dirty(i);
        return;
    }
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    bool dbl = now - U->tap_ms[i] < 320;
    U->tap_ms[i] = now;
    int d = c->deck;
    switch (c->kind) {
    case K_BTN:
        dj_act_press(i);
        break;
    case K_KNOB:
        if (dbl && c->val) { dj_act_value(i, c->def); U->tap_ms[i] = 0; }
        f->v0 = c->val ? *c->val : 0.0f;
        break;
    case K_VFADER: case K_HFADER: {
        if (!c->val) break;
        if (dbl) { dj_act_value(i, c->def); U->tap_ms[i] = 0; f->v0 = c->def; break; }
        /* on the cap: drag it from there; elsewhere: it jumps under the finger */
        float v = (*c->val - c->lo) / (c->hi - c->lo);
        bool vert = c->kind == K_VFADER;
        float a = vert ? (float)c->r.y1 + 18.0f : (float)c->r.x1 + 20.0f;
        float b = vert ? (float)c->r.y2 - (c->id == C_PITCH ? 24.0f : 18.0f) : (float)c->r.x2 - 20.0f;
        float capc = vert ? b - (b - a) * v : a + (b - a) * v;
        float p = vert ? y : x;
        if (fabsf(p - capc) > 26.0f) {
            float nv = vert ? (b - p) / (b - a) : (p - a) / (b - a);
            dj_act_value(i, c->lo + dj_clampf(nv, 0.0f, 1.0f) * (c->hi - c->lo));
        }
        f->v0 = *c->val;
        break;
    }
    case K_PLATTER: {
        float rf;
        f->ang = platter_angle(c, x, y, &rf);
        f->ring = rf > 0.86f || !U->m->vinyl;
        if (!loaded(d)) break;
        if (!f->ring) dj_cmd(U->e, DC_TOUCH, d, 1, 0.0f);
        break;
    }
    case K_WAVE:
        if (loaded(d)) dj_cmd(U->e, DC_TOUCH, d, 1, 0.0f);
        break;
    case K_OVERVIEW:
        if (loaded(d)) {
            float fr = (x - (float)c->r.x1) / (float)lv_area_get_width(&c->r);
            dj_cmd(U->e, DC_SEEK, d, 0, fr);
        }
        break;
    case K_INFO:
        dj_act_press(i);
        break;
    default:
        break;
    }
    dj_ui_dirty(i);
}

static void finger_move(finger_t *f, float x, float y)
{
    int i = f->ctl;
    if (i < 0) return;
    ctl_t *c = &U->ctl[i];
    int d = c->deck;
    switch (c->kind) {
    case K_KNOB:
        if (c->val) dj_act_value(i, f->v0 + (f->y0 - y) / 240.0f * (c->hi - c->lo));
        break;
    case K_VFADER: case K_HFADER: {
        if (!c->val) break;
        bool vert = c->kind == K_VFADER;
        float len = vert ? (float)lv_area_get_height(&c->r) - 42.0f : (float)lv_area_get_width(&c->r) - 40.0f;
        float dv = vert ? (f->y0 - y) / len : (x - f->x0) / len;
        dj_act_value(i, f->v0 + dv * (c->hi - c->lo));
        break;
    }
    case K_PLATTER: {
        if (!loaded(d)) break;
        float a = platter_angle(c, x, y, NULL);
        float da = a - f->ang;
        if (da > 3.14159265f) da -= 6.2831853f;
        if (da < -3.14159265f) da += 6.2831853f;
        f->ang = a;
        if (!f->ring) {
            float samples = da / 6.2831853f * REV_SECS * (float)T(d)->sr;
            if (samples != 0.0f) dj_cmd(U->e, DC_SCRATCH, d, 0, samples);
        } else {
            /* the outer ring: how fast it turns bends the pitch */
            float omega = da / 0.01f;                       /* rad/s, one touch sample */
            float b = dj_clampf(omega / 6.2831853f * 0.5f, -1.0f, 1.0f);
            U->jog_bend[d] = U->jog_bend[d] * 0.6f + b * 0.4f;
            U->m->s[d].bend = U->jog_bend[d];
            f->t = (uint32_t)aos_hal_uptime_ms();
        }
        break;
    }
    case K_WAVE:
        if (loaded(d) && x != f->lx) dj_cmd(U->e, DC_SCRATCH, d, 0, -(x - f->lx) * wave_spp(d));
        break;
    default:
        break;
    }
    f->lx = x;
    f->ly = y;
}

static void finger_up(finger_t *f)
{
    int i = f->ctl;
    f->ctl = -1;
    if (i < 0 || i >= U->nctl) return;
    ctl_t *c = &U->ctl[i];
    c->lit = false;
    int d = c->deck;
    switch (c->kind) {
    case K_BTN:
        dj_act_release(i);
        if (c->id >= C_HOT1 && c->id <= C_HOT4 && (uint32_t)aos_hal_uptime_ms() - f->t > 900) {
            dj_cmd(U->e, DC_HOT_CLEAR, d, c->id - C_HOT1, 0.0f);
            aos_ui_toast(_("Hot cue borrado"), 1200);
            meta_mark(d);
        }
        break;
    case K_PLATTER:
        if (!f->ring) dj_cmd(U->e, DC_TOUCH, d, 0, 0.0f);
        U->jog_bend[d] = 0.0f;
        U->m->s[d].bend = 0.0f;
        break;
    case K_WAVE:
        dj_cmd(U->e, DC_TOUCH, d, 0, 0.0f);
        break;
    default:
        break;
    }
    dj_ui_dirty(i);
}

static void mark_stale(void)
{
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    for (int i = 0; i < AOS_TOUCH_MAX; i++) U->stale[i] = pts[i].down ? pts[i].id : 0;
}

void dj_ui_release_all(void)
{
    if (!U) return;
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        if (U->f[i].id) finger_up(&U->f[i]);
        U->f[i].id = 0;
        U->f[i].ctl = -1;
    }
    for (int d = 0; d < DJ_NDECK; d++) { U->m->s[d].bend = 0.0f; U->jog_bend[d] = 0.0f; }
    mark_stale();
}

static void touch_timer(lv_timer_t *t)
{
    (void)t;
    if (!U || !U->surf) return;
    dj_midi_poll();
    /* a toast is a line at the bottom, not a reason to drop the platters */
    bool covered = U->sheet || (aos_ui_overlay() & ~(uint32_t)AOS_UI_OVER_TOAST) != 0;
    if (covered != U->covered) {
        U->covered = covered;
        if (covered) dj_ui_release_all();
        else mark_stale();
    }
    if (covered) return;
    aos_touch_point_t pts[AOS_TOUCH_MAX];
    aos_touch_points_n(pts, AOS_TOUCH_MAX);
    for (int s = 0; s < AOS_TOUCH_MAX; s++) {
        bool there = false;
        for (int i = 0; i < AOS_TOUCH_MAX; i++) if (pts[i].down && pts[i].id == U->stale[s]) there = true;
        if (!there) U->stale[s] = 0;
    }
    float ox = (float)org.x1, oy = (float)org.y1;
    for (int i = 0; i < AOS_TOUCH_MAX; i++) {
        uint8_t id = pts[i].down ? pts[i].id : 0;
        for (int s = 0; s < AOS_TOUCH_MAX && id; s++) if (U->stale[s] == id) id = 0;
        float x = pts[i].x - ox, y = pts[i].y - oy;
        finger_t *f = &U->f[i];
        if (id != f->id) {
            if (f->id) finger_up(f);
            f->id = id;
            f->ctl = -1;
            if (id) {
                U->touch_n++;
                finger_down(f, hit(x, y), x, y);
            }
        } else if (id && (x != f->lx || y != f->ly)) {
            finger_move(f, x, y);
        } else if (id && f->ctl >= 0 && U->ctl[f->ctl].kind == K_PLATTER && f->ring) {
            /* a finger resting on the ring: the bend fades */
            int d = U->ctl[f->ctl].deck;
            U->jog_bend[d] *= 0.6f;
            U->m->s[d].bend = U->jog_bend[d];
        }
    }
}

/* ---- the UI timer ---- */

void dj_ui_meta_flush(bool force)
{
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int d = 0; d < DJ_NDECK; d++) {
        meta_track_t *m = &U->mt[d];
        dj_track_t *t = T(d);
        if (!loaded(d)) continue;
        const dj_pub_t *p = P(d);
        if (m->gen != t->gen) {
            /* a new track: what it came with is what is saved */
            m->gen = t->gen;
            m->cue = p->cue;
            for (int h = 0; h < DJ_HOT; h++) m->hot[h] = p->hot[h];
            m->bpm = p->bpm;
            m->beat0 = p->beat0;
            m->hand = t->meta_bpm;
            m->dirty = false;
            continue;
        }
        bool diff = fabsf(m->cue - p->cue) > 0.5f || fabsf(m->bpm - p->bpm) > 0.0005f || fabsf(m->beat0 - p->beat0) > 0.5f;
        for (int h = 0; h < DJ_HOT; h++) if (fabsf(m->hot[h] - p->hot[h]) > 0.5f) diff = true;
        if (diff && !m->dirty) { m->dirty = true; m->dirty_ms = now; }
        if (m->dirty && (force || now - m->dirty_ms > 3000)) {
            m->cue = p->cue;
            for (int h = 0; h < DJ_HOT; h++) m->hot[h] = p->hot[h];
            m->bpm = p->bpm;
            m->beat0 = p->beat0;
            float hot[DJ_HOT];
            for (int h = 0; h < DJ_HOT; h++) hot[h] = p->hot[h];
            dj_meta_save(t->path, t->fsize, p->bpm, p->beat0, p->cue, hot, m->hand);
            m->dirty = false;
        }
    }
}

static void ui_timer(lv_timer_t *t)
{
    (void)t;
    if (!U || !U->surf) return;
    U->blink++;
    U->frames++;
    for (int i = 0; i < U->nctl; i++) {
        ctl_t *c = &U->ctl[i];
        if (c->kind == K_WAVE || c->kind == K_OVERVIEW) continue;
        uint32_t s = signature(c);
        if (s != c->sig) {
            c->sig = s;
            dj_ui_dirty(i);
        }
    }
    if (!U->sheet) {
        dj_wave_render();
        for (int d = 0; d < DJ_NDECK; d++) dj_ov_render(d, false);
    } else {
        dj_sheet_tick();
    }
    dj_ui_meta_flush(false);
}

void dj_ui_tick(void)
{
    if (!U) return;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - U->log_ms < 5000) return;
    dj_stats_t s;
    dj_stats(U->e, &s);
    float secs = (float)(now - U->log_ms) / 1000.0f;
    aos_hal_log(DJ_TAG, "audio: %.0f us avg, %.0f us max per %d-sample block (%.1f%% of %d us), queue %d + out %d ms, "
                "dry %u, misses %u; ui: %.1f fps, draw %.2f ms x %u, %u touches",
                (double)s.blk_us_avg, (double)s.blk_us_max, DJ_BLK, (double)s.load_pct, DJ_BLK * 1000000 / DJ_SR,
                s.queued, aos_hal_spk_latency_ms(), (unsigned)s.underruns, (unsigned)s.misses, (double)((float)U->frames / secs),
                (double)(U->draw_n ? (float)U->draw_us / (float)U->draw_n / 1000.0f : 0.0f), (unsigned)U->draw_n,
                (unsigned)U->touch_n);
    U->frames = U->draw_us = U->draw_n = U->touch_n = 0;
    U->log_ms = now;
}

void dj_ui_timers(bool on)
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

/* ---- settings ---- */

void dj_ui_load_settings(void)
{
    int32_t v;
    dj_mix_t *m = U->m;
    if (aos_hal_pref_get_i32("dj_range", &v)) m->range = v == 1 ? 0.16f : v == 2 ? 0.5f : 0.08f;
    if (aos_hal_pref_get_i32("dj_xcurve", &v)) m->xcurve = v != 0;
    if (aos_hal_pref_get_i32("dj_quant", &v)) m->quant = v != 0;
    if (aos_hal_pref_get_i32("dj_vinyl", &v)) m->vinyl = v != 0;
    if (aos_hal_pref_get_i32("dj_zoom", &v)) U->zoom = (int)v & 3;
    else U->zoom = 1;
}

void dj_ui_save_settings(void)
{
    dj_mix_t *m = U->m;
    aos_hal_pref_set_i32("dj_range", m->range > 0.3f ? 2 : m->range > 0.1f ? 1 : 0);
    aos_hal_pref_set_i32("dj_xcurve", m->xcurve);
    aos_hal_pref_set_i32("dj_quant", m->quant);
    aos_hal_pref_set_i32("dj_vinyl", m->vinyl);
    aos_hal_pref_set_i32("dj_zoom", U->zoom);
}

/* ---- building ---- */

void dj_ui_build(void)
{
    dj_ui_release_all();
    /* the objects go with the root (cleaned here, or already gone with the
     * old root on a turn): only forget them */
    dj_lib_forget();
    dj_wave_destroy();
    U->surf = NULL;
    lv_obj_clean(U->root);
    lv_obj_update_layout(U->root);
    U->W = lv_obj_get_width(U->root);
    U->H = lv_obj_get_height(U->root);
    U->land = U->W > U->H;
    lv_obj_set_style_bg_color(U->root, lv_color_hex(DJ_COL_BG), 0);
    lv_obj_set_style_bg_opa(U->root, LV_OPA_COVER, 0);
    U->surf = lv_obj_create(U->root);
    lv_obj_remove_style_all(U->surf);
    lv_obj_set_size(U->surf, U->W, U->H);
    lv_obj_set_pos(U->surf, 0, 0);
    lv_obj_set_style_bg_color(U->surf, lv_color_hex(DJ_COL_BG), 0);
    lv_obj_set_style_bg_opa(U->surf, LV_OPA_COVER, 0);
    lv_obj_remove_flag(U->surf, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(U->surf, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    layout();
    lv_obj_update_layout(U->root);
    lv_obj_get_coords(U->surf, &org);
    dj_wave_create();
    for (int i = 0; i < U->nctl; i++) U->ctl[i].sig = 0xFFFFFFFFu;
    mark_stale();
}
