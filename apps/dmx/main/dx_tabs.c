/*
 * P4OS - DMX: the four tabs (dmx.c has the frame around them).
 */
#include "dx.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t now_ms(void) { return (uint32_t)aos_hal_uptime_ms(); }

static void cfg_apply(void)
{
    if (DX.h && DX.dir == AOS_DMX_SEND) aos_io_dmx_config(DX.h, &DX.cfg);
    dx_save();
    dx_rebuild();
}

/* ======================================================================== */
/* Consola                                                                   */
/* ======================================================================== */

static void fader_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    int ch = DX.page * 16 + k + 1;
    int v = lv_slider_get_value(lv_event_get_target(e));
    DX.uni[ch] = (uint8_t)v;
    DX.pattern = DX_PAT_NONE;
    if (DXU.fader_v[k]) lv_label_set_text_fmt(DXU.fader_v[k], "%d", v);
    dx_send_universe();
}

static void page_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    DX.page = (DX.page + d + 32) % 32;
    dx_rebuild();
}

static void goto_done(int v)
{
    if (v >= 1 && v <= 512) DX.page = (v - 1) / 16;
    dx_rebuild();
}
static void goto_cb(lv_event_t *e) { (void)e; dx_keypad(_("Ir al canal"), DX.page * 16 + 1, goto_done); }

static void all_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    memset(DX.uni + 1, v, 512);
    DX.pattern = DX_PAT_NONE;
    dx_send_universe();
    dx_rebuild();
}

static void all_done(int v)
{
    memset(DX.uni + 1, v < 0 ? 0 : v > 255 ? 255 : v, 512);
    DX.pattern = DX_PAT_NONE;
    dx_send_universe();
    dx_rebuild();
}
static void all_val_cb(lv_event_t *e) { (void)e; dx_keypad(_("Todos los canales a"), 128, all_done); }

static void pat_cb(lv_event_t *e)
{
    DX.pattern = (int)(intptr_t)lv_event_get_user_data(e);
    DX.pat_t0 = now_ms();
    if (DX.pattern != DX_PAT_NONE) memset(DX.uni + 1, 0, 512);
    dx_send_universe();
    dx_rebuild();
}

static const uint16_t SLOTS[] = { 512, 96, 24 };
static const uint16_t BREAKS[] = { 88, 176, 1000 };
static const uint16_t MABS[] = { 8, 16, 100 };
static const uint16_t PERIODS[] = { 25, 33, 100 };       /* 40, 30, 10 packets a second */

static void slots_cb(lv_event_t *e) { DX.cfg.slots = SLOTS[(intptr_t)lv_event_get_user_data(e)]; cfg_apply(); }
static void brk_cb(lv_event_t *e) { DX.cfg.break_us = BREAKS[(intptr_t)lv_event_get_user_data(e)]; cfg_apply(); }
static void mab_cb(lv_event_t *e) { DX.cfg.mab_us = MABS[(intptr_t)lv_event_get_user_data(e)]; cfg_apply(); }
static void per_cb(lv_event_t *e) { DX.cfg.period_ms = PERIODS[(intptr_t)lv_event_get_user_data(e)]; cfg_apply(); }

static void hue(float h, uint8_t *rgb)
{
    float r = fabsf(fmodf(h * 6.0f, 6.0f) - 3.0f) - 1.0f;
    float g = 2.0f - fabsf(fmodf(h * 6.0f, 6.0f) - 2.0f);
    float b = 2.0f - fabsf(fmodf(h * 6.0f, 6.0f) - 4.0f);
    rgb[0] = (uint8_t)(fminf(fmaxf(r, 0), 1) * 255);
    rgb[1] = (uint8_t)(fminf(fmaxf(g, 0), 1) * 255);
    rgb[2] = (uint8_t)(fminf(fmaxf(b, 0), 1) * 255);
}

/* every 100 ms: a pattern moves the universe, the page's faders follow */
void dx_console_tick(void)
{
    if (DX.pattern == DX_PAT_NONE) return;
    float t = (float)(now_ms() - DX.pat_t0) / 1000.0f;
    int slots = DX.cfg.slots ? DX.cfg.slots : 512;
    switch (DX.pattern) {
    case DX_PAT_CHASE: {
        int cur = (int)(t / 0.3f) % slots + 1;      /* one channel at a time, 0.3 s each */
        memset(DX.uni + 1, 0, 512);
        DX.uni[cur] = 255;
        break;
    }
    case DX_PAT_RAMP: {
        float f = fmodf(t / 4.0f, 2.0f);
        uint8_t v = (uint8_t)((f < 1 ? f : 2 - f) * 255);
        memset(DX.uni + 1, v, (size_t)slots);
        break;
    }
    case DX_PAT_RAINBOW:
        for (int p = 0; p < 8; p++) hue(fmodf(t / 6.0f + p / 8.0f, 1.0f), DX.uni + 1 + 3 * p);
        break;
    default: break;
    }
    dx_send_universe();
    for (int k = 0; k < 16; k++) {
        if (!DXU.fader[k]) continue;
        int v = DX.uni[DX.page * 16 + k + 1];
        lv_slider_set_value(DXU.fader[k], v, LV_ANIM_OFF);
        lv_label_set_text_fmt(DXU.fader_v[k], "%d", v);
    }
}

void dx_console_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = DXU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = dx_column(parent, w, ch);
    lv_obj_t *nav = dx_box(col, w, 76);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, 12, 0);
    dx_pill(nav, AOS_SYM_CHEVRON_LEFT, NULL, AOS_C_CARD2, page_cb, (void *)-1);
    DXU.page_l = aos_label(nav, "", aos_font_title, AOS_C_TEXT);
    lv_label_set_text_fmt(DXU.page_l, _("Canales %d a %d"), DX.page * 16 + 1, DX.page * 16 + 16);
    dx_pill(nav, AOS_SYM_CHEVRON_RIGHT, NULL, AOS_C_CARD2, page_cb, (void *)1);
    dx_chip(nav, _("Ir a…"), false, goto_cb, NULL);

    /* the faders: two rows of eight standing, one of sixteen lying down */
    int per_row = DXU.land ? 16 : 8;
    int32_t gap = 8, cw = (w - (per_row - 1) * gap) / per_row;
    int32_t sh = DXU.land ? 230 : 300;
    lv_obj_t *g = dx_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, gap, 0);
    for (int k = 0; k < 16; k++) {
        int chn = DX.page * 16 + k + 1;
        lv_obj_t *c = dx_card(g, cw, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_ver(c, 10, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(c, 10, 0);
        lv_obj_t *n = lv_label_create(c);
        lv_obj_set_style_text_font(n, &aos_mono_18, 0);
        lv_obj_set_style_text_color(n, DX_AMBER, 0);
        lv_label_set_text_fmt(n, "%d", chn);
        lv_obj_t *s = lv_slider_create(c);
        lv_obj_set_size(s, 18, sh);
        lv_slider_set_range(s, 0, 255);
        lv_slider_set_value(s, DX.uni[chn], LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s, DX_TEAL, LV_PART_INDICATOR);
        lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);
        lv_obj_set_ext_click_area(s, 24);
        lv_obj_add_event_cb(s, fader_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
        DXU.fader[k] = s;
        lv_obj_t *v = lv_label_create(c);
        lv_obj_set_style_text_font(v, &aos_mono_18, 0);
        lv_obj_set_style_text_color(v, AOS_C_TEXT, 0);
        lv_label_set_text_fmt(v, "%d", DX.uni[chn]);
        DXU.fader_v[k] = v;
    }

    lv_obj_t *r = dx_chips_row(col, w);
    dx_chip(r, _("Todo a 0"), false, all_cb, (void *)0);
    dx_chip(r, _("Todo a 255"), false, all_cb, (void *)255);
    dx_chip(r, _("Todo a…"), false, all_val_cb, NULL);
    dx_section(col, _("PATRONES"));
    r = dx_chips_row(col, w);
    static const char *const PAT[DX_PAT_COUNT] = { N_("Quieto"), N_("Recorrer canal por canal"), N_("Rampa en todos"), N_("Arcoíris en 8 pixeles RGB") };
    for (int p = 0; p < DX_PAT_COUNT; p++) dx_chip(r, aos_tr(PAT[p]), DX.pattern == p, pat_cb, (void *)(intptr_t)p);
    dx_caption(col, _("Recorrer prende un canal por vez a 255, tres por segundo: para encontrar qué canal mueve qué. El arcoíris va en los canales 1 a 24 (ocho pixeles RGB, como la tira del ESP32 del banco)."), w);

    dx_section(col, _("EL PAQUETE"));
    lv_obj_t *c = dx_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    char t[32];
    r = dx_chips_row(c, w - 32);
    for (size_t i = 0; i < 3; i++) { snprintf(t, sizeof t, _("%u canales"), SLOTS[i]); dx_chip(r, t, DX.cfg.slots == SLOTS[i], slots_cb, (void *)(intptr_t)i); }
    r = dx_chips_row(c, w - 32);
    for (size_t i = 0; i < 3; i++) { snprintf(t, sizeof t, "BREAK %u µs", BREAKS[i]); dx_chip(r, t, DX.cfg.break_us == BREAKS[i], brk_cb, (void *)(intptr_t)i); }
    r = dx_chips_row(c, w - 32);
    for (size_t i = 0; i < 3; i++) { snprintf(t, sizeof t, "MAB %u µs", MABS[i]); dx_chip(r, t, DX.cfg.mab_us == MABS[i], mab_cb, (void *)(intptr_t)i); }
    r = dx_chips_row(c, w - 32);
    for (size_t i = 0; i < 3; i++) { snprintf(t, sizeof t, _("%u por segundo"), 1000 / PERIODS[i]); dx_chip(r, t, DX.cfg.period_ms == PERIODS[i], per_cb, (void *)(intptr_t)i); }
    dx_caption(c, _("El estándar pide BREAK de 88 µs o más y MAB de 8 µs o más; 176 y 16 es lo de casi todas las consolas. Con los límites y con paquetes cortos se ve si un aparato los aguanta."), w - 32);
}

/* ======================================================================== */
/* Aparato                                                                   */
/* ======================================================================== */

static const struct { const char *name; int slots; const char *const *ch; } FX[DX_FX_COUNT] = {
    { N_("Dimmer (1 canal)"), 1, (const char *const[]){ N_("Intensidad") } },
    { N_("RGB (3 canales)"), 3, (const char *const[]){ N_("Rojo"), N_("Verde"), N_("Azul") } },
    { N_("RGBW (4 canales)"), 4, (const char *const[]){ N_("Rojo"), N_("Verde"), N_("Azul"), N_("Blanco") } },
    { N_("PAR de 7 canales"), 7, (const char *const[]){ N_("Dimmer"), N_("Rojo"), N_("Verde"), N_("Azul"), N_("Strobe"), N_("Macro"), N_("Velocidad") } },
    { N_("Cabeza móvil (6 canales)"), 6, (const char *const[]){ N_("Pan"), N_("Tilt"), N_("Dimmer"), N_("Rojo"), N_("Verde"), N_("Azul") } },
    { N_("Barra de 8 pixeles (24 canales)"), 24, NULL },
};

const char *dx_fx_name(int kind) { return aos_tr(FX[kind].name); }
int dx_fx_slots(int kind) { return FX[kind].slots; }

static uint8_t fx_ch(int k)
{
    int a = DX.fx_addr + k;
    bool live = DX.h && DX.dir == AOS_DMX_RECEIVE && DX.st.age_ms < 1000;
    return live && a >= 1 && a <= DX.rx_slots ? DX.rx[a] : 0;
}

static lv_color_t rgb_of(int r, int g, int b, int dim)
{
    return lv_color_make((uint8_t)(r * dim / 255), (uint8_t)(g * dim / 255), (uint8_t)(b * dim / 255));
}

static void disc(lv_layer_t *layer, int32_t cx, int32_t cy, int32_t rad, lv_color_t c)
{
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = c;
    d.bg_opa = LV_OPA_COVER;
    d.radius = LV_RADIUS_CIRCLE;
    d.border_width = 3;
    d.border_color = lv_color_hex(0x48484A);
    d.border_opa = LV_OPA_COVER;
    lv_area_t a = { cx - rad, cy - rad, cx + rad, cy + rad };
    lv_draw_rect(layer, &d, &a);
}

static void fx_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a), cx = a.x1 + w / 2, cy = a.y1 + h / 2;
    switch (DX.fx_kind) {
    case DX_FX_DIMMER: disc(layer, cx, cy, h / 3, rgb_of(255, 240, 200, fx_ch(0))); break;
    case DX_FX_RGB: disc(layer, cx, cy, h / 3, rgb_of(fx_ch(0), fx_ch(1), fx_ch(2), 255)); break;
    case DX_FX_RGBW: {
        int wv = fx_ch(3);
        int r = fx_ch(0) + wv, g = fx_ch(1) + wv, b = fx_ch(2) + wv;
        disc(layer, cx, cy, h / 3, rgb_of(r > 255 ? 255 : r, g > 255 ? 255 : g, b > 255 ? 255 : b, 255));
        break;
    }
    case DX_FX_PAR7: {
        int dim = fx_ch(0), strobe = fx_ch(4);
        if (strobe > 10) {                          /* 1 to 20 flashes a second */
            float hz = 1.0f + (strobe - 10) * 19.0f / 245.0f;
            if (fmodf((float)now_ms() / 1000.0f * hz, 1.0f) > 0.3f) dim = 0;
        }
        disc(layer, cx, cy, h / 3, rgb_of(fx_ch(1), fx_ch(2), fx_ch(3), dim));
        break;
    }
    case DX_FX_HEAD: {
        lv_draw_rect_dsc_t d;
        lv_draw_rect_dsc_init(&d);
        d.bg_opa = LV_OPA_TRANSP;
        d.border_width = 2;
        d.border_color = lv_color_hex(0x48484A);
        d.border_opa = LV_OPA_COVER;
        d.radius = 12;
        lv_area_t stage = { a.x1 + 10, a.y1 + 10, a.x2 - 10, a.y2 - 10 };
        lv_draw_rect(layer, &d, &stage);
        int32_t px = stage.x1 + 20 + (lv_area_get_width(&stage) - 40) * fx_ch(0) / 255;
        int32_t py = stage.y1 + 20 + (lv_area_get_height(&stage) - 40) * fx_ch(1) / 255;
        disc(layer, px, py, 28, rgb_of(fx_ch(3), fx_ch(4), fx_ch(5), fx_ch(2)));
        break;
    }
    default: {                                      /* the bar */
        int32_t step = w / 8, rad = step / 2 - 8;
        if (rad > h / 3) rad = h / 3;
        for (int p = 0; p < 8; p++) disc(layer, a.x1 + step / 2 + p * step, cy, rad, rgb_of(fx_ch(3 * p), fx_ch(3 * p + 1), fx_ch(3 * p + 2), 255));
        break;
    }
    }
}

void dx_fixture_refresh(void)
{
    if (!DXU.fx_draw) return;
    lv_obj_invalidate(DXU.fx_draw);
    bool live = DX.h && DX.dir == AOS_DMX_RECEIVE && DX.st.age_ms < 1000;
    if (DXU.fx_state) {
        if (!DX.h) lv_label_set_text(DXU.fx_state, _("Abrí la conexión para escuchar."));
        else if (!live) lv_label_set_text(DXU.fx_state, _("Sin señal: nadie manda DMX."));
        else if (DX.fx_addr + FX[DX.fx_kind].slots - 1 > DX.rx_slots)
            lv_label_set_text_fmt(DXU.fx_state, _("La consola manda %d canales: este aparato necesita hasta el %d."), DX.rx_slots, DX.fx_addr + FX[DX.fx_kind].slots - 1);
        else lv_label_set_text_fmt(DXU.fx_state, _("Recibiendo: canales %d a %d."), DX.fx_addr, DX.fx_addr + FX[DX.fx_kind].slots - 1);
        lv_obj_set_style_text_color(DXU.fx_state, live ? AOS_C_GREEN : AOS_C_ORANGE, 0);
    }
    if (DXU.fx_list) {
        char t[1200];
        size_t k = 0;
        int n = FX[DX.fx_kind].slots;
        for (int i = 0; i < n && k + 64 < sizeof t; i++) {
            const char *name = FX[DX.fx_kind].ch ? aos_tr(FX[DX.fx_kind].ch[i]) : NULL;
            char nm[24];
            if (!name) { snprintf(nm, sizeof nm, "%s %d %c", _("Pixel"), i / 3 + 1, "RGB"[i % 3]); name = nm; }
            k += (size_t)snprintf(t + k, sizeof t - k, "%s%3d  %-14s %3d", i ? "\n" : "", DX.fx_addr + i, name, fx_ch(i));
        }
        lv_label_set_text(DXU.fx_list, t);
    }
}

static void fx_kind_cb(lv_event_t *e) { DX.fx_kind = (int)(intptr_t)lv_event_get_user_data(e); dx_save(); dx_rebuild(); }
static void fx_addr_done(int v)
{
    int max = 512 - FX[DX.fx_kind].slots + 1;
    DX.fx_addr = v < 1 ? 1 : v > max ? max : v;
    dx_save();
    dx_rebuild();
}
static void fx_addr_cb(lv_event_t *e) { (void)e; dx_keypad(_("Dirección de inicio (1-512)"), DX.fx_addr, fx_addr_done); }

void dx_fixture_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = DXU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = dx_column(parent, w, ch);
    lv_obj_t *r = dx_chips_row(col, w);
    for (int k = 0; k < DX_FX_COUNT; k++) dx_chip(r, aos_tr(FX[k].name), DX.fx_kind == k, fx_kind_cb, (void *)(intptr_t)k);
    r = dx_chips_row(col, w);
    char t[48];
    snprintf(t, sizeof t, "%s %03d", _("Dirección"), DX.fx_addr);
    dx_chip(r, t, false, fx_addr_cb, NULL);
    DXU.fx_state = aos_label(col, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(DXU.fx_state, w);
    lv_label_set_long_mode(DXU.fx_state, LV_LABEL_LONG_MODE_WRAP);
    DXU.fx_draw = dx_card(col, w, DXU.land ? 220 : 320);
    lv_obj_add_event_cb(DXU.fx_draw, fx_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_t *lc = dx_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(lc, 16, 0);
    DXU.fx_list = lv_label_create(lc);
    lv_obj_set_style_text_font(DXU.fx_list, &aos_mono_22, 0);
    lv_obj_set_style_text_color(DXU.fx_list, AOS_C_TEXT, 0);
    dx_caption(col, _("La placa hace de aparato: escucha el bus y dibuja lo que haría uno de estos con esa dirección. Sirve para ver si una consola manda, y qué manda en cada canal."), w);
    dx_fixture_refresh();
}

/* ======================================================================== */
/* Analizador                                                                */
/* ======================================================================== */

static lv_obj_t *s_stat[10];

static void map_draw_cb(lv_event_t *e)
{
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_obj_t *o = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int cols = 32, rows = 16;
    int32_t cw = lv_area_get_width(&a) / cols, chh = lv_area_get_height(&a) / rows;
    uint32_t now = now_ms();
    bool live = DX.h && DX.dir == AOS_DMX_RECEIVE && DX.st.age_ms < 1000;
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_opa = LV_OPA_COVER;
    d.radius = 2;
    for (int i = 0; i < 512; i++) {
        int chn = i + 1;
        int v = live && chn <= DX.rx_slots ? DX.rx[chn] : 0;
        bool moving = DX.chg_ms[chn] && now - DX.chg_ms[chn] < 1500;
        bool sel = chn > DX.an_page * 32 && chn <= DX.an_page * 32 + 32;
        if (!live || chn > DX.rx_slots) d.bg_color = lv_color_hex(0x1C1C1E);
        else if (moving) d.bg_color = lv_color_make((uint8_t)(120 + v * 135 / 255), (uint8_t)(60 + v * 100 / 255), 0);
        else d.bg_color = lv_color_make((uint8_t)(30 + v * 200 / 255), (uint8_t)(30 + v * 200 / 255), (uint8_t)(40 + v * 200 / 255));
        d.border_width = sel ? 1 : 0;
        d.border_color = DX_TEAL;
        d.border_opa = LV_OPA_COVER;
        int32_t x = a.x1 + (i % cols) * cw, y = a.y1 + (i / cols) * chh;
        lv_area_t r = { x + 1, y + 1, x + cw - 2, y + chh - 2 };
        lv_draw_rect(layer, &d, &r);
    }
}

static void map_click_cb(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    int col = (p.x - a.x1) * 32 / lv_area_get_width(&a), row = (p.y - a.y1) * 16 / lv_area_get_height(&a);
    if (col < 0 || col > 31 || row < 0 || row > 15) return;
    DX.an_page = (row * 32 + col) / 32;
    dx_analyzer_refresh();
}

static void an_page_cb(lv_event_t *e)
{
    DX.an_page = (DX.an_page + (int)(intptr_t)lv_event_get_user_data(e) + 16) % 16;
    dx_analyzer_refresh();
}

void dx_analyzer_refresh(void)
{
    if (!DXU.an_map) return;
    lv_obj_invalidate(DXU.an_map);
    const aos_dmx_stats_t *s = &DX.st;
    bool live = DX.h && DX.dir == AOS_DMX_RECEIVE && s->age_ms < 1000;
    char t[64];
    for (int i = 0; i < 10; i++) {
        if (!s_stat[i]) continue;
        lv_color_t col = AOS_C_TEXT;
        switch (i) {
        case 0:
            snprintf(t, sizeof t, "%s", !DX.h ? _("cerrado") : live ? _("hay señal") : _("sin señal"));
            col = live ? AOS_C_GREEN : AOS_C_ORANGE;
            break;
        case 1: snprintf(t, sizeof t, "%.1f", (double)s->hz); break;
        case 2: snprintf(t, sizeof t, "%u", s->slots); break;
        case 3:
            snprintf(t, sizeof t, "0x%02X %s", s->start_code, s->start_code == 0 ? _("niveles") : s->start_code == 0xCC ? "RDM" : s->start_code == 0x17 ? _("texto") : "");
            col = s->start_code ? AOS_C_ORANGE : AOS_C_TEXT;
            break;
        case 4:
            if (s->break_us) snprintf(t, sizeof t, "%u µs", s->break_us); else snprintf(t, sizeof t, "—");
            col = s->break_us && s->break_us < 88 ? AOS_C_RED : AOS_C_TEXT;
            break;
        case 5:
            if (s->mab_us) snprintf(t, sizeof t, "%u µs", s->mab_us); else snprintf(t, sizeof t, "—");
            col = s->mab_us && s->mab_us < 8 ? AOS_C_RED : AOS_C_TEXT;
            break;
        case 6: snprintf(t, sizeof t, "%lu", (unsigned long)s->packets); break;
        case 7: snprintf(t, sizeof t, "%lu", (unsigned long)s->bad); col = s->bad ? AOS_C_ORANGE : AOS_C_TEXT; break;
        case 8: snprintf(t, sizeof t, "%lu", (unsigned long)s->other_code); break;
        default: snprintf(t, sizeof t, "%lu", (unsigned long)s->short_break); col = s->short_break ? AOS_C_RED : AOS_C_TEXT; break;
        }
        lv_label_set_text(s_stat[i], t);
        lv_obj_set_style_text_color(s_stat[i], col, 0);
    }
    if (DXU.an_page_l) lv_label_set_text_fmt(DXU.an_page_l, _("Canales %d a %d"), DX.an_page * 32 + 1, DX.an_page * 32 + 32);
    if (DXU.an_nums) {
        char b[600];
        size_t k = 0;
        for (int i = 0; i < 32; i++) {
            int chn = DX.an_page * 32 + i + 1;
            int v = live && chn <= DX.rx_slots ? DX.rx[chn] : -1;
            if (v >= 0) k += (size_t)snprintf(b + k, sizeof b - k, "%03d:%3d%s", chn, v, i % 4 == 3 ? "\n" : "   ");
            else k += (size_t)snprintf(b + k, sizeof b - k, "%03d:  -%s", chn, i % 4 == 3 ? "\n" : "   ");
        }
        if (k) b[k - 1] = 0;
        lv_label_set_text(DXU.an_nums, b);
    }
}

void dx_analyzer_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = DXU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = dx_column(parent, w, ch);
    static const char *const NAME[10] = { N_("Estado"), N_("Paquetes/s"), N_("Canales"), N_("Código de inicio"), "BREAK", "MAB",
                                          N_("Paquetes"), N_("Malos"), N_("Otros códigos"), N_("BREAK cortos") };
    lv_obj_t *g = dx_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(g, 10, 0);
    int cols = DXU.land ? 5 : 3;
    int32_t cw = (w - (cols - 1) * 10) / cols;
    memset(s_stat, 0, sizeof s_stat);
    for (int i = 0; i < 10; i++) {
        lv_obj_t *c = dx_card(g, cw, 96);
        lv_obj_set_style_pad_all(c, 14, 0);
        lv_obj_align(aos_label(c, aos_tr(NAME[i]), aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_LEFT, 0, 0);
        s_stat[i] = aos_label(c, "—", aos_font_body, AOS_C_TEXT);
        lv_obj_align(s_stat[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }
    dx_section(col, _("LOS 512 CANALES (NARANJA: SE MUEVEN)"));
    DXU.an_map = dx_card(col, w, (w / 32) * 16);
    lv_obj_add_flag(DXU.an_map, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(DXU.an_map, map_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
    lv_obj_add_event_cb(DXU.an_map, map_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *nav = dx_box(col, w, 76);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(nav, 12, 0);
    dx_pill(nav, AOS_SYM_CHEVRON_LEFT, NULL, AOS_C_CARD2, an_page_cb, (void *)-1);
    DXU.an_page_l = aos_label(nav, "", aos_font_body, AOS_C_TEXT);
    dx_pill(nav, AOS_SYM_CHEVRON_RIGHT, NULL, AOS_C_CARD2, an_page_cb, (void *)1);
    lv_obj_t *nc = dx_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(nc, 16, 0);
    DXU.an_nums = lv_label_create(nc);
    lv_obj_set_style_text_font(DXU.an_nums, &aos_mono_22, 0);
    lv_obj_set_style_text_color(DXU.an_nums, AOS_C_TEXT, 0);
    dx_caption(col, _("BREAK y MAB medidos en el pin, con unos pocos µs de error. Un BREAK de menos de 88 µs o un MAB de menos de 8 son de una consola fuera de norma: muchos aparatos parpadean o ignoran esos paquetes. Malos: con error de trama o cortados. Tocá el mapa para ver esos canales en números."), w);
    dx_analyzer_refresh();
}

/* ======================================================================== */
/* Red: the board as an Art-Net / sACN node                                  */
/* ======================================================================== */

static uint32_t s_net_last_n, s_net_last_ms;
static float s_net_hz;

static void net_on_cb(lv_event_t *e)
{
    DX.net_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    if (DX.net_on) {
        memset(DX.uni + 1, 0, 512);             /* until the network says otherwise */
        DX.pattern = DX_PAT_NONE;
        if (!DX.want_open) { DX.want_open = true; DX.err[0] = 0; }
    }
    dx_save();
    dx_rebuild();
}

static void net_proto_cb(lv_event_t *e)
{
    int p = (int)(intptr_t)lv_event_get_user_data(e);
    if (p != DX.net_proto) {
        DX.net_proto = p;
        if (p == DX_NET_SACN && DX.net_univ == 0) DX.net_univ = 1;   /* sACN starts at 1 */
    }
    dx_save();
    dx_rebuild();
}

static void net_univ_done(int v)
{
    int max = DX.net_proto == DX_NET_SACN ? 63999 : 32767, min = DX.net_proto == DX_NET_SACN ? 1 : 0;
    if (v >= min && v <= max) DX.net_univ = (uint16_t)v;
    dx_save();
    dx_rebuild();
}

static void net_univ_cb(lv_event_t *e)
{
    (void)e;
    dx_keypad(DX.net_proto == DX_NET_SACN ? _("Universo sACN (1 a 63999)") : _("Universo Art-Net (0 a 32767)"), DX.net_univ, net_univ_done);
}

void dx_net_refresh(void)
{
    if (!DXU.net_stats) return;
    uint32_t now = now_ms(), n = DX.net_packets;
    if (now - s_net_last_ms >= 1000) {
        s_net_hz = s_net_last_ms ? (float)(n - s_net_last_n) * 1000.0f / (float)(now - s_net_last_ms) : 0;
        s_net_last_n = n;
        s_net_last_ms = now;
    }
    char t[300];
    const char *ip = aos_hal_net_ip();
    if (!DX.net_on) snprintf(t, sizeof t, "%s", _("Apagado: prendelo para que la placa reciba de la red."));
    else if (DX.net_err[0]) snprintf(t, sizeof t, "%s", DX.net_err);
    else if (!n) snprintf(t, sizeof t, _("Esperando %s del universo %u en %s…"), DX.net_proto == DX_NET_SACN ? "sACN" : "Art-Net", DX.net_univ,
                          ip && ip[0] ? ip : "?");
    else {
        uint32_t age = now - DX.net_ms;
        snprintf(t, sizeof t, _("%.0f paquetes/s de %s · %lu en total · el último hace %lu ms · %lu de otros universos · %lu ArtPoll contestados%s"),
                 (double)s_net_hz, DX.net_from, (unsigned long)n, (unsigned long)age, (unsigned long)DX.net_other, (unsigned long)DX.net_polls,
                 age > 2000 ? _(" · SIN DATOS (la salida mantiene lo último)") : "");
    }
    lv_label_set_text(DXU.net_stats, t);
    lv_obj_set_style_text_color(DXU.net_stats, !DX.net_on ? AOS_C_DIM : DX.net_err[0] || (n && now - DX.net_ms > 2000) ? AOS_C_ORANGE : AOS_C_TEXT, 0);
    if (DXU.net_nums) {
        char b[600];
        size_t k = 0;
        for (int i = 0; i < 32; i++)
            k += (size_t)snprintf(b + k, sizeof b - k, "%03d:%3d%s", i + 1, DX.uni[i + 1], i % 4 == 3 ? "\n" : "   ");
        if (k) b[k - 1] = 0;
        lv_label_set_text(DXU.net_nums, b);
    }
}

void dx_net_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = DXU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = dx_column(parent, w, ch);
    lv_obj_t *c = dx_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 12, 0);
    /* the label wraps and the switch keeps its place: in English the text is
     * longer than the row */
    lv_obj_t *r = dx_box(c, w - 36, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 14, 0);
    lv_obj_t *nl = aos_label(r, _("Recibir de la red y sacarlo por DMX"), aos_font_body, AOS_C_TEXT);
    lv_obj_set_flex_grow(nl, 1);
    lv_label_set_long_mode(nl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 96, 52);
    lv_obj_set_style_bg_color(sw, DX_TEAL, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (DX.net_on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, net_on_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = dx_chips_row(c, w - 36);
    dx_chip(r, "Art-Net", DX.net_proto == DX_NET_ARTNET, net_proto_cb, (void *)(intptr_t)DX_NET_ARTNET);
    dx_chip(r, "sACN (E1.31)", DX.net_proto == DX_NET_SACN, net_proto_cb, (void *)(intptr_t)DX_NET_SACN);
    char u[32];
    snprintf(u, sizeof u, "%s %u", _("Universo"), DX.net_univ);
    dx_chip(r, u, false, net_univ_cb, NULL);
    DXU.net_stats = aos_label(c, "", aos_font_small, AOS_C_TEXT);
    lv_obj_set_width(DXU.net_stats, w - 36);
    lv_label_set_long_mode(DXU.net_stats, LV_LABEL_LONG_MODE_WRAP);
    dx_section(col, _("LOS PRIMEROS 32 CANALES COMO SALEN"));
    lv_obj_t *nc = dx_card(col, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(nc, 16, 0);
    DXU.net_nums = lv_label_create(nc);
    lv_obj_set_style_text_font(DXU.net_nums, &aos_mono_22, 0);
    lv_obj_set_style_text_color(DXU.net_nums, AOS_C_TEXT, 0);
    dx_caption(col, _("La placa hace de nodo: lo que una consola, un programa de la PC o Luces en otra placa mandan por la red para ese universo sale por el puerto RS485 como DMX. Art-Net por UDP 6454 (contesta el ArtPoll, así la consola la ve en su lista de nodos); sACN por multicast 239.255.x.y, puerto 5568. Si la red deja de mandar, la salida mantiene lo último. Sigue andando con la app en segundo plano."), w);
    dx_net_refresh();
}

/* ======================================================================== */
/* RDM                                                                       */
/* ======================================================================== */

static void rdm_find_cb(lv_event_t *e)
{
    (void)e;
    if (!DX.h || DX.dir != AOS_DMX_SEND) {
        DX.want_open = true;
        DX.err[0] = 0;
        snprintf(DX.rdm_msg, sizeof DX.rdm_msg, "%s", _("Abriendo el puerto: tocá Buscar otra vez."));
        dx_rdm_refresh();
        return;
    }
    if (!dx_rdm_start(DX_RDM_DISCOVER)) return;
    DXU.rdm_seen = DX.rdm_seq;
    dx_rdm_refresh();
}

static void rdm_identify_cb(lv_event_t *e)
{
    DX.rdm_act = DX_RDM_IDENTIFY;
    DX.rdm_act_dev = (int)(intptr_t)lv_event_get_user_data(e);
    dx_rdm_start(DX_RDM_IDENTIFY);
}

static void rdm_addr_done(int v)
{
    if (v < 1 || v > 512) return;
    DX.rdm_act = DX_RDM_ADDRESS;
    DX.rdm_act_val = v;
    dx_rdm_start(DX_RDM_ADDRESS);
}

static void rdm_addr_cb(lv_event_t *e)
{
    DX.rdm_act_dev = (int)(intptr_t)lv_event_get_user_data(e);
    dx_keypad(_("Dirección DMX nueva (1 a 512)"), DX.rdm_dev[DX.rdm_act_dev].addr, rdm_addr_done);
}

static void rdm_label_done(const char *v)
{
    snprintf(DX.rdm_act_text, sizeof DX.rdm_act_text, "%.32s", v);
    DX.rdm_act = DX_RDM_LABEL;
    dx_rdm_start(DX_RDM_LABEL);
}

static void rdm_label_cb(lv_event_t *e)
{
    DX.rdm_act_dev = (int)(intptr_t)lv_event_get_user_data(e);
    dx_text(_("Nombre del aparato (DEVICE_LABEL)"), DX.rdm_dev[DX.rdm_act_dev].label, rdm_label_done);
}

static void rdm_list_fill(void)
{
    lv_obj_clean(DXU.rdm_list);
    int32_t w = lv_obj_get_width(DXU.rdm_list);
    for (int i = 0; i < DX.rdm_n; i++) {
        const dx_rdm_dev_t *dv = &DX.rdm_dev[i];
        lv_obj_t *c = dx_card(DXU.rdm_list, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(c, 8, 0);
        aos_label(c, dv->label[0] ? dv->label : dv->model[0] ? dv->model : _("(sin nombre)"), aos_font_body, dv->identify ? DX_AMBER : AOS_C_TEXT);
        char t[160];
        snprintf(t, sizeof t, "%02X%02X:%02X%02X%02X%02X · %.32s%s%.32s", dv->uid[0], dv->uid[1], dv->uid[2], dv->uid[3], dv->uid[4], dv->uid[5],
                 dv->model, dv->maker[0] ? " · " : "", dv->maker);
        dx_caption(c, t, w - 32);
        if (dv->ok) snprintf(t, sizeof t, _("Canales %u a %u (%u canales)"), dv->addr, dv->addr + dv->footprint - (dv->footprint ? 1 : 0), dv->footprint);
        else snprintf(t, sizeof t, "%s", _("No contestó DEVICE_INFO."));
        aos_label(c, t, aos_font_small, DX_TEAL);
        lv_obj_t *r = dx_chips_row(c, w - 32);
        dx_chip(r, dv->identify ? _("Dejar de identificar") : _("Identificar"), dv->identify, rdm_identify_cb, (void *)(intptr_t)i);
        dx_chip(r, _("Dirección"), false, rdm_addr_cb, (void *)(intptr_t)i);
        dx_chip(r, _("Nombre"), false, rdm_label_cb, (void *)(intptr_t)i);
    }
}

void dx_rdm_refresh(void)
{
    if (!DXU.rdm_msg) return;
    char t[200];
    snprintf(t, sizeof t, "%s%s", dx_rdm_busy() ? "" : "", DX.rdm_msg[0] ? DX.rdm_msg : _("Tocá Buscar: la app pregunta en la línea quién habla RDM."));
    size_t k = strlen(t);
    if (DX.rdm_req) snprintf(t + k, sizeof t - k, _(" · %lu pedidos, %lu respuestas, %lu choques en la búsqueda, %lu respuestas malas"),
                             (unsigned long)DX.rdm_req, (unsigned long)DX.rdm_ans, (unsigned long)DX.rdm_coll, (unsigned long)DX.rdm_bad);
    lv_label_set_text(DXU.rdm_msg, t);
    if (DXU.rdm_seen != DX.rdm_seq && !dx_rdm_busy()) {
        DXU.rdm_seen = DX.rdm_seq;
        rdm_list_fill();
    }
}

void dx_rdm_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = DXU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = dx_column(parent, w, ch);
    lv_obj_t *top = dx_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(top, 10, 0);
    dx_pill(top, AOS_SYM_MAGNIFY, _("Buscar aparatos"), DX_TEAL, rdm_find_cb, NULL);
    DXU.rdm_msg = dx_caption(col, "", w);
    DXU.rdm_list = dx_box(col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(DXU.rdm_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(DXU.rdm_list, 12, 0);
    dx_caption(col, _("RDM va por el mismo par que DMX: entre paquete y paquete la placa pregunta y el aparato contesta. La búsqueda encuentra a cada uno por su UID sin conocerlo (partiendo el espacio en dos cuando contestan varios a la vez); después se le puede cambiar la dirección y el nombre o pedirle que se muestre. Sólo los aparatos con RDM contestan, y un splitter DMX común corta la vuelta."), w);
    rdm_list_fill();
    DXU.rdm_seen = DX.rdm_seq;
    dx_rdm_refresh();
}

/* ======================================================================== */
/* Cableado                                                                  */
/* ======================================================================== */

static void xlr(lv_obj_t *parent, int32_t x, int32_t y, int pins, const char *title)
{
    int32_t d = 150;
    lv_obj_t *c = dx_box(parent, d, d);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    static const int16_t P3[3][2] = { { -34, -22 }, { 34, -22 }, { 0, 34 } };
    static const int16_t P5[5][2] = { { -44, -10 }, { 44, -10 }, { 0, 44 }, { -28, 32 }, { 28, 32 } };
    static const char *const L[5] = { "1", "2", "3", "4", "5" };
    for (int i = 0; i < pins; i++) {
        lv_obj_t *p = dx_box(c, 30, 30);
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(p, i == 0 ? AOS_C_DIM : i == 1 ? lv_color_hex(0x2563EB) : i == 2 ? DX_AMBER : lv_color_hex(0x636366), 0);
        lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
        const int16_t *pp = pins == 3 ? P3[i] : P5[i];
        lv_obj_align(p, LV_ALIGN_CENTER, pp[0], pp[1]);
        lv_obj_center(aos_label(p, L[i], aos_font_tiny, lv_color_black()));
    }
    lv_obj_t *t = aos_label(parent, title, aos_font_caption, AOS_C_DIM);
    lv_obj_set_pos(t, x, y + d + 6);
}

void dx_wire_build(lv_obj_t *parent, int32_t ch)
{
    int32_t w = DXU.W - 2 * AOS_UI_PAD;
    lv_obj_t *col = dx_column(parent, w, ch);
    dx_section(col, _("LOS CONECTORES (DE FRENTE, LADO HEMBRA)"));
    lv_obj_t *dc = dx_card(col, w, 220);
    xlr(dc, 30, 20, 3, _("XLR de 3 (lo común)"));
    xlr(dc, 230, 20, 5, _("XLR de 5 (el estándar)"));
    lv_obj_t *m = dx_caption(dc, _("1: masa o malla. 2 (azul): D−, al B− del módulo. 3 (ámbar): D+, al A+. 4 y 5 no se usan."), w - 440);
    lv_obj_set_pos(m, 420, 30);
    dx_section(col, _("LO QUE HAY QUE SABER"));
    static const char *const NOTES[] = {
        N_("En cadena: consola → DMX IN del aparato 1 → su DMX OUT → aparato 2… El último lleva un terminador de 120 Ω entre los pines 2 y 3. Sin él, las luces parpadean o tiemblan."),
        N_("Nunca un cable en Y: para ramificar se usa un splitter DMX. Hasta 32 aparatos por tramo."),
        N_("Cable de 110-120 Ω (cable DMX o de datos), no de micrófono: en tramos cortos anda igual, en tramos largos da errores."),
        N_("Algunos equipos baratos traen D+ y D− al revés. Si un aparato no responde, probá invertir los pines 2 y 3."),
        N_("A 250 000 baudios, un módulo RS485 automático puede no llegar: si los paquetes salen malos, un módulo con pin DE (MAX3485, SP3485) resuelve."),
        N_("Cada aparato tiene una dirección de inicio y una cantidad de canales según su modo; dos aparatos con la misma dirección hacen lo mismo."),
    };
    for (size_t i = 0; i < sizeof NOTES / sizeof NOTES[0]; i++) {
        lv_obj_t *n = dx_card(col, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(n, 16, 0);
        lv_obj_t *l = aos_label(n, aos_tr(NOTES[i]), aos_font_caption, AOS_C_TEXT);
        lv_obj_set_width(l, w - 32);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    }
}
