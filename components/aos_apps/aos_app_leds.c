/*
 * P4OS - Tiras LED: an addressable strip on any free pin of the header,
 * WLED's way.
 *
 *   Efectos  on/off, brightness, the effect, its speed and intensity, the
 *            two colours, and the strip drawn live as it is being sent
 *   Tira     the GPIO, the kind of LED, the colour order, how many LEDs,
 *            and whether the effects run from the far end
 *   Consumo  what the strip draws now and at full white, the supply's
 *            budget (the brightness limiter keeps the estimate under it),
 *            the mA a LED draws, and how to wire and power it
 *
 * The strip lives in a service (aos_io/aos_leds.c), not here: it goes on
 * with the app closed, and comes back at boot as it was left. This app only
 * reads and writes its configuration.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_io.h"
#include "aos_leds.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_LED   lv_color_hex(0xFF9F0A)

enum { TAB_FX, TAB_STRIP, TAB_POWER, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { N_("Efectos"), N_("Tira"), N_("Consumo") };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_AUTO_FIX, AOS_SYM_LED_ON, AOS_SYM_LIGHTNING_BOLT };

static const char *const FX_NAME[AOS_FX_COUNT] = {
    N_("Sólido"), N_("Parpadeo"), N_("Respirar"), N_("Barrido"), N_("Arcoíris"), N_("Arcoíris en ciclo"),
    N_("Teatro"), N_("Escáner"), N_("Destellos"), N_("Fuego"), N_("Meteoro"), N_("Degradé"), N_("Ondas"),
};
/* which effects use the second colour, and what the intensity slider means */
static const bool FX_TWO[AOS_FX_COUNT] = { [AOS_FX_BLINK] = true, [AOS_FX_WIPE] = true, [AOS_FX_THEATER] = true,
                                           [AOS_FX_TWINKLE] = true, [AOS_FX_GRADIENT] = true, [AOS_FX_RUNNING] = true };
static const char *const FX_INT[AOS_FX_COUNT] = {
    [AOS_FX_BLINK] = N_("Tiempo prendido"), [AOS_FX_RAINBOW_CYCLE] = N_("Arcoíris en la tira"),
    [AOS_FX_SCANNER] = N_("Ancho"), [AOS_FX_TWINKLE] = N_("Cantidad"), [AOS_FX_FIRE] = N_("Llamas"),
    [AOS_FX_METEOR] = N_("Cola"), [AOS_FX_RUNNING] = N_("Ondas en la tira"),
};

static const uint32_t PALETTE[] = {
    0xFF0000, 0xFF4000, 0xFF7A00, 0xFFB000, 0xFFE000, 0xA0FF00, 0x00FF00, 0x00FF80,
    0x00FFFF, 0x0080FF, 0x0000FF, 0x6000FF, 0xB000FF, 0xFF00C0, 0xFF0060, 0xFFFFFF,
    0xFFD8A0, 0xFFB070, 0x000000,
};

static struct { int tab; } S = { .tab = TAB_FX };

static struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT], *overlay;
    lv_timer_t *timer;
    int32_t W, H, CW;
    bool land;
    lv_obj_t *canvas, *status, *ma_now, *ma_full, *ma_bar, *limited;
    uint16_t *cbuf;
    int32_t cw, ch;
    uint8_t *frame;
    int pick;                       /* the colour being picked: 0 or 1 */
} U;

static void build_page(void);

/* ---- pieces ---- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    return c;
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *chips_row(lv_obj_t *parent)
{
    lv_obj_t *r = box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, bool enabled, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(c, 32, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_bg_color(c, on ? C_LED : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    if (cb && enabled) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x1C1404) : enabled ? AOS_C_TEXT : AOS_C_DIM);
    lv_obj_center(l);
    aos_make_decorative(l);
    return c;
}

static lv_obj_t *slider_row(lv_obj_t *parent, const char *name, int value, lv_event_cb_t cb)
{
    lv_obj_t *r = box(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, 12, 0);
    aos_label(r, name, aos_font_small, AOS_C_TEXT);
    lv_obj_t *s = lv_slider_create(r);
    lv_obj_set_size(s, lv_pct(96), 16);
    lv_obj_set_style_margin_left(s, 12, 0);
    lv_slider_set_range(s, 0, 255);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s, C_LED, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, C_LED, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 12, LV_PART_KNOB);
    lv_obj_set_ext_click_area(s, 20);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return r;
}

static void overlay_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = NULL;
}

static void dim_cb(lv_event_t *e) { if (lv_event_get_target(e) == U.overlay) overlay_close(); }

/* ---- the live strip ---- */

static void preview_draw(void)
{
    if (!U.canvas || !U.frame) return;
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    int n = aos_leds_frame(U.frame, AOS_STRIP_MAX_LEDS);
    lv_memset(U.cbuf, 0, (size_t)U.cw * U.ch * 2);
    if (n <= 0) { lv_obj_invalidate(U.canvas); return; }
    /* one cell a LED while they fit at 6 px; beyond that each cell is the
     * brightest of the LEDs it covers */
    int cells = n < U.cw / 6 ? n : U.cw / 6;
    int32_t cell = U.cw / cells;
    for (int k = 0; k < cells; k++) {
        int a = k * n / cells, b = (k + 1) * n / cells;
        if (b <= a) b = a + 1;
        int r = 0, g = 0, bl = 0, w = 0;
        for (int i = a; i < b; i++) {
            const uint8_t *p = U.frame + 4 * i;
            if (p[0] + p[1] + p[2] + p[3] > r + g + bl + w) { r = p[0]; g = p[1]; bl = p[2]; w = p[3]; }
        }
        r += w; g += w; bl += w;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (bl > 255) bl = 255;
        /* the dim end lifted, so a strip at 10 % still shows on the screen */
        int mx = r > g ? (r > bl ? r : bl) : (g > bl ? g : bl);
        if (mx > 0 && mx < 90) { r = r * 90 / mx; g = g * 90 / mx; bl = bl * 90 / mx; }
        uint16_t px = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | (bl >> 3));
        int32_t x0 = k * cell + (cell > 4 ? 1 : 0), x1 = (k + 1) * cell - (cell > 4 ? 1 : 0);
        for (int32_t y = 4; y < U.ch - 4; y++)
            for (int32_t x = x0; x < x1 && x < U.cw; x++) U.cbuf[y * U.cw + x] = px;
    }
    lv_obj_invalidate(U.canvas);
}

static void status_text(char *t, size_t n)
{
    aos_leds_cfg_t c;
    aos_leds_status_t st;
    aos_leds_get(&c);
    aos_leds_status(&st);
    if (c.gpio < 0) { snprintf(t, n, "%s", _("Sin tira: elegí el pin en Tira. Mientras, se ve acá.")); return; }
    if (st.error[0]) { snprintf(t, n, _("No se pudo abrir la tira en %s"), st.error); return; }
    if (!c.on) { snprintf(t, n, _("Apagada · GPIO%d · %d LEDs"), c.gpio, c.count); return; }
    snprintf(t, n, _("GPIO%d · %s · %d LEDs · %d fps · %u mA%s"), c.gpio, aos_io_strip_type_name(c.type), c.count, st.fps,
             (unsigned)st.ma_out, st.limited ? _(" (limitada)") : "");
}

/* ---- Efectos ---- */

static void cfg_change(void (*fn)(aos_leds_cfg_t *, intptr_t), intptr_t v)
{
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    fn(&c, v);
    aos_leds_set(&c);
}

static void set_on(aos_leds_cfg_t *c, intptr_t v) { c->on = v != 0; }
static void set_bri(aos_leds_cfg_t *c, intptr_t v) { c->brightness = (uint8_t)v; }
static void set_fx(aos_leds_cfg_t *c, intptr_t v) { c->effect = (uint8_t)v; }
static void set_speed(aos_leds_cfg_t *c, intptr_t v) { c->speed = (uint8_t)v; }
static void set_int(aos_leds_cfg_t *c, intptr_t v) { c->intensity = (uint8_t)v; }
static void set_white(aos_leds_cfg_t *c, intptr_t v) { c->white = (uint8_t)v; }

static void power_cb(lv_event_t *e)
{
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    cfg_change(set_on, !c.on);
    build_page();
}
static void bri_cb(lv_event_t *e) { cfg_change(set_bri, lv_slider_get_value(lv_event_get_target(e))); }
static void speed_cb(lv_event_t *e) { cfg_change(set_speed, lv_slider_get_value(lv_event_get_target(e))); }
static void int_cb(lv_event_t *e) { cfg_change(set_int, lv_slider_get_value(lv_event_get_target(e))); }
static void white_cb(lv_event_t *e) { cfg_change(set_white, lv_slider_get_value(lv_event_get_target(e))); }
static void fx_cb(lv_event_t *e) { cfg_change(set_fx, (intptr_t)lv_event_get_user_data(e)); build_page(); }

static void color_pick_cb(lv_event_t *e)
{
    uint32_t col = (uint32_t)(uintptr_t)lv_event_get_user_data(e);
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    c.color[U.pick] = col;
    aos_leds_set(&c);
    overlay_close();
    build_page();
}

static void color_sheet(int which)
{
    U.pick = which;
    overlay_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_70, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.overlay, dim_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sh = card(U.overlay);
    int32_t w = U.land ? 640 : U.W - 2 * AOS_UI_PAD;
    lv_obj_set_width(sh, w);
    lv_obj_center(sh);
    aos_label(sh, which ? _("Segundo color") : _("Color"), aos_font_title, AOS_C_TEXT);
    lv_obj_t *grid = chips_row(sh);
    lv_obj_set_style_pad_gap(grid, 14, 0);
    int32_t sw = (w - 40 - 5 * 14) / 6;
    for (size_t i = 0; i < sizeof PALETTE / sizeof PALETTE[0]; i++) {
        lv_obj_t *b = box(grid, sw, sw);
        lv_obj_set_style_radius(b, sw / 2, 0);
        lv_obj_set_style_bg_color(b, lv_color_hex(PALETTE[i]), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x48484A), 0);
        lv_obj_set_style_border_width(b, PALETTE[i] == 0 ? 2 : 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, color_pick_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)PALETTE[i]);
    }
}

static void color_cb(lv_event_t *e) { color_sheet((int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *swatch(lv_obj_t *parent, const char *name, uint32_t col, int which)
{
    lv_obj_t *r = box(parent, LV_SIZE_CONTENT, 72);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 14, 0);
    lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(r, color_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    lv_obj_t *d = box(r, 60, 60);
    lv_obj_set_style_radius(d, 30, 0);
    lv_obj_set_style_bg_color(d, lv_color_hex(col), 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(d, lv_color_white(), 0);
    lv_obj_set_style_border_width(d, 2, 0);
    lv_obj_set_style_border_opa(d, LV_OPA_40, 0);
    aos_label(r, name, aos_font_small, AOS_C_TEXT);
    return r;
}

static void build_fx(lv_obj_t *col)
{
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    /* the strip, live */
    lv_obj_t *pv = card(col);
    U.cw = lv_obj_get_content_width(U.content) - 40;
    if (U.cw < 100) U.cw = U.CW - 2 * AOS_UI_PAD - 40;
    U.ch = 44;
    U.cbuf = aos_hal_io_alloc((size_t)U.cw * U.ch * 2);
    if (U.cbuf) {
        U.canvas = lv_canvas_create(pv);
        lv_canvas_set_buffer(U.canvas, U.cbuf, U.cw, U.ch, LV_COLOR_FORMAT_RGB565);
    }
    U.status = caption(pv, "");

    lv_obj_t *top = card(col);
    lv_obj_t *row = box(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    aos_label(row, c.on ? _("Prendida") : _("Apagada"), aos_font_title, AOS_C_TEXT);
    lv_obj_t *pw = box(row, 120, 120);
    lv_obj_set_style_radius(pw, 60, 0);
    lv_obj_set_style_bg_color(pw, c.on ? C_LED : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(pw, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(pw, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_add_flag(pw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(pw, power_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pg = aos_label(pw, AOS_SYM_POWER, &aos_sym_44, c.on ? lv_color_hex(0x1C1404) : AOS_C_TEXT);
    lv_obj_center(pg);
    slider_row(top, _("Brillo"), c.brightness, bri_cb);

    section(col, _("EFECTO"));
    lv_obj_t *fx = card(col);
    lv_obj_t *chips = chips_row(fx);
    for (int i = 0; i < AOS_FX_COUNT; i++) chip(chips, aos_tr(FX_NAME[i]), c.effect == i, true, fx_cb, (void *)(intptr_t)i);
    if (c.effect != AOS_FX_SOLID) slider_row(fx, _("Velocidad"), c.speed, speed_cb);
    if (FX_INT[c.effect]) slider_row(fx, aos_tr(FX_INT[c.effect]), c.intensity, int_cb);

    bool uses_color = c.effect != AOS_FX_RAINBOW && c.effect != AOS_FX_RAINBOW_CYCLE && c.effect != AOS_FX_FIRE;
    if (uses_color || aos_io_strip_type_rgbw(c.type)) {
        section(col, _("COLORES"));
        lv_obj_t *cc = card(col);
        if (uses_color) {
            lv_obj_t *sw = chips_row(cc);
            lv_obj_set_style_pad_gap(sw, 40, 0);
            swatch(sw, _("Color"), c.color[0], 0);
            if (FX_TWO[c.effect]) swatch(sw, _("Segundo"), c.color[1], 1);
        }
        if (aos_io_strip_type_rgbw(c.type)) slider_row(cc, _("Blanco (canal W)"), c.white, white_cb);
    }
    preview_draw();
}

/* ---- Tira ---- */

static void set_gpio(aos_leds_cfg_t *c, intptr_t v) { c->gpio = (int8_t)v; }
static void set_type(aos_leds_cfg_t *c, intptr_t v) { c->type = (aos_strip_type_t)v; }
static void set_order(aos_leds_cfg_t *c, intptr_t v) { c->order = (aos_strip_order_t)v; }
static void set_count(aos_leds_cfg_t *c, intptr_t v) { c->count = (uint16_t)(v < 1 ? 1 : v > AOS_STRIP_MAX_LEDS ? AOS_STRIP_MAX_LEDS : v); }
static void set_rev(aos_leds_cfg_t *c, intptr_t v) { c->reverse = v != 0; }

static void gpio_cb(lv_event_t *e) { cfg_change(set_gpio, (intptr_t)lv_event_get_user_data(e)); build_page(); }
static void type_cb(lv_event_t *e) { cfg_change(set_type, (intptr_t)lv_event_get_user_data(e)); build_page(); }
static void order_cb(lv_event_t *e) { cfg_change(set_order, (intptr_t)lv_event_get_user_data(e)); build_page(); }
static void count_cb(lv_event_t *e) { cfg_change(set_count, (intptr_t)lv_event_get_user_data(e)); build_page(); }
static void step_cb(lv_event_t *e)
{
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    cfg_change(set_count, (intptr_t)c.count + (intptr_t)lv_event_get_user_data(e));
    build_page();
}
static void rev_cb(lv_event_t *e)
{
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    cfg_change(set_rev, !c.reverse);
    build_page();
}

static void build_strip(lv_obj_t *col)
{
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    char t[48];
    section(col, _("PIN DE DATOS"));
    lv_obj_t *pc = card(col);
    lv_obj_t *pins = chips_row(pc);
    chip(pins, _("Ninguno"), c.gpio < 0, true, gpio_cb, (void *)(intptr_t)-1);
    const aos_io_pin_t *hdr = aos_io_header();
    for (int i = 0; i < 40; i++) {
        const aos_io_pin_t *p = &hdr[i];
        if (p->gpio < 0 || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) continue;
        const char *who = aos_io_owner(p->gpio);
        bool mine = p->gpio == c.gpio;
        bool free_ = !who || mine || !strcmp(who, "LEDs");
        snprintf(t, sizeof t, "GPIO%d · %d%s", p->gpio, p->pin, (p->flags & (AOS_PIN_STRAPPING | AOS_PIN_VO4 | AOS_PIN_USB_JTAG)) ? " *" : "");
        chip(pins, t, mine, free_, gpio_cb, (void *)(intptr_t)p->gpio);
    }
    caption(pc, _("GPIO · pin del conector. Los que tiene otra app aparecen apagados. * con cuidado: arranque, dominio VO4 o USB (Ajustes, Expansión)."));

    section(col, _("TIPO DE LED"));
    lv_obj_t *tc = card(col);
    lv_obj_t *types = chips_row(tc);
    for (int i = 0; i < AOS_STRIP_TYPE_COUNT; i++)
        chip(types, aos_io_strip_type_name((aos_strip_type_t)i), c.type == i, true, type_cb, (void *)(intptr_t)i);
    caption(tc, _("WS2812B también sirve para WS2813, WS2815 y WS2811 de 800 kHz. Las RGBW llevan un cuarto LED blanco."));

    section(col, _("ORDEN DE LOS COLORES"));
    lv_obj_t *oc = card(col);
    lv_obj_t *orders = chips_row(oc);
    for (int i = 0; i < AOS_ORDER_COUNT; i++)
        chip(orders, aos_io_strip_order_name((aos_strip_order_t)i), c.order == i, true, order_cb, (void *)(intptr_t)i);
    caption(oc, _("Si el rojo sale verde, probá otro orden: las WS2812B son GRB."));

    section(col, _("CANTIDAD DE LEDS"));
    lv_obj_t *nc = card(col);
    lv_obj_t *nr = box(nc, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    chip(nr, "−10", false, true, step_cb, (void *)(intptr_t)-10);
    chip(nr, "−1", false, true, step_cb, (void *)(intptr_t)-1);
    snprintf(t, sizeof t, "%d", c.count);
    aos_label(nr, t, aos_font_large, AOS_C_TEXT);
    chip(nr, "+1", false, true, step_cb, (void *)(intptr_t)1);
    chip(nr, "+10", false, true, step_cb, (void *)(intptr_t)10);
    lv_obj_t *qs = chips_row(nc);
    static const int QUICK[] = { 8, 16, 30, 60, 144, 150, 300 };
    for (size_t i = 0; i < sizeof QUICK / sizeof QUICK[0]; i++) {
        snprintf(t, sizeof t, "%d", QUICK[i]);
        chip(qs, t, c.count == QUICK[i], true, count_cb, (void *)(intptr_t)QUICK[i]);
    }

    lv_obj_t *rc = card(col);
    lv_obj_t *rr = box(rc, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(rr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(rr, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    aos_label(rr, _("Efectos al revés"), aos_font_body, AOS_C_TEXT);
    lv_obj_t *sw = lv_switch_create(rr);
    lv_obj_set_size(sw, 100, 56);
    lv_obj_set_style_bg_color(sw, C_LED, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_state(sw, LV_STATE_CHECKED, c.reverse);
    lv_obj_add_event_cb(sw, rev_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---- Consumo ---- */

static void set_psu(aos_leds_cfg_t *c, intptr_t v) { c->psu_ma = (uint16_t)v; }
static void set_ma(aos_leds_cfg_t *c, intptr_t v) { c->ma_per_led = (uint16_t)v; }
static void psu_cb(lv_event_t *e) { cfg_change(set_psu, (intptr_t)lv_event_get_user_data(e)); build_page(); }
static void ma_cb(lv_event_t *e) { cfg_change(set_ma, (intptr_t)lv_event_get_user_data(e)); build_page(); }

static void power_refresh(void)
{
    if (!U.ma_now) return;
    aos_leds_cfg_t c;
    aos_leds_status_t st;
    aos_leds_get(&c);
    aos_leds_status(&st);
    char t[64];
    snprintf(t, sizeof t, "%u mA", (unsigned)(c.on ? st.ma_out : 0));
    lv_label_set_text(U.ma_now, t);
    if (st.limited) snprintf(t, sizeof t, _("Limitada: pedía %u mA, brillo %d %%"), (unsigned)st.ma_estimate, st.brightness_out * 100 / 255);
    else snprintf(t, sizeof t, "%s", c.on ? _("Dentro del límite") : _("Apagada"));
    lv_label_set_text(U.limited, t);
    lv_obj_set_style_text_color(U.limited, st.limited ? C_LED : AOS_C_DIM, 0);
    int pct = c.psu_ma ? (int)(st.ma_out * 100 / c.psu_ma) : 0;
    lv_bar_set_value(U.ma_bar, pct > 100 ? 100 : pct, LV_ANIM_OFF);
}

static void build_power(lv_obj_t *col)
{
    aos_leds_cfg_t c;
    aos_leds_get(&c);
    char t[96];
    lv_obj_t *now = card(col);
    aos_label(now, _("Consumo estimado ahora"), aos_font_small, AOS_C_DIM);
    U.ma_now = aos_label(now, "", aos_font_large, AOS_C_TEXT);
    U.ma_bar = lv_bar_create(now);
    lv_obj_set_size(U.ma_bar, lv_pct(100), 18);
    lv_bar_set_range(U.ma_bar, 0, 100);
    lv_obj_set_style_bg_color(U.ma_bar, C_LED, LV_PART_INDICATOR);
    U.limited = caption(now, "");
    uint32_t full = (uint32_t)c.count * c.ma_per_led + c.count;
    snprintf(t, sizeof t, _("Toda la tira en blanco al máximo: %u mA (%.1f A)"), (unsigned)full, full / 1000.0);
    caption(now, t);

    section(col, _("LA FUENTE"));
    lv_obj_t *pc = card(col);
    lv_obj_t *ps = chips_row(pc);
    static const struct { uint16_t ma; const char *name; } PSU[] = {
        { 450, N_("USB 2.0 (450 mA)") }, { 850, N_("USB 3 (850 mA)") }, { 1500, "1,5 A" }, { 2000, "2 A" },
        { 3000, "3 A" }, { 5000, "5 A" }, { 10000, "10 A" }, { 0, N_("Sin límite") },
    };
    for (size_t i = 0; i < sizeof PSU / sizeof PSU[0]; i++)
        chip(ps, aos_tr(PSU[i].name), c.psu_ma == PSU[i].ma, true, psu_cb, (void *)(intptr_t)PSU[i].ma);
    caption(pc, _("Lo que la fuente le puede dar a la tira. Si un efecto pediría más, se baja el brillo de todo el cuadro para que entre, como el limitador de WLED. Alimentada desde el USB de la placa, dejá USB 2.0."));

    section(col, _("CONSUMO DE CADA LED"));
    lv_obj_t *mc = card(col);
    lv_obj_t *ms = chips_row(mc);
    static const struct { uint16_t ma; const char *name; } MA[] = {
        { 55, N_("5 V típica (55 mA)") }, { 35, N_("5 V eco (35 mA)") }, { 30, N_("12 V (30 mA)") },
        { 15, N_("WS2815 (15 mA)") }, { 80, N_("RGBW (80 mA)") },
    };
    for (size_t i = 0; i < sizeof MA / sizeof MA[0]; i++)
        chip(ms, aos_tr(MA[i].name), c.ma_per_led == MA[i].ma, true, ma_cb, (void *)(intptr_t)MA[i].ma);
    caption(mc, _("Un LED en blanco al máximo, sus tres colores prendidos: los valores de WLED. Cada canal suma su parte según cuánto brilla."));

    section(col, _("CÓMO CONECTARLA"));
    lv_obj_t *wc = card(col);
    const aos_io_pin_t *pin = c.gpio >= 0 ? aos_io_pin_of_gpio(c.gpio) : NULL;
    if (pin) snprintf(t, sizeof t, _("Datos: GPIO%d, pin %d del conector, con una resistencia de 330 Ω en serie."), c.gpio, pin->pin);
    else snprintf(t, sizeof t, "%s", _("Datos: el GPIO que elijas en Tira, con una resistencia de 330 Ω en serie."));
    caption(wc, t);
    caption(wc, _("La placa da 3,3 V y muchas tiras de 5 V quieren 3,5 V o más para entender los datos: con un level shifter (74AHCT125 o similar) anda siempre."));
    caption(wc, _("Masa común: la de la tira, la de la fuente y la de la placa (pin 13, por ejemplo)."));
    caption(wc, _("Los 5 V de la tira, de una fuente aparte salvo con pocos LEDs. Los pines 1 y 3 de la placa tienen 5 V aun con la placa apagada por POWER. Una tira larga lleva alimentación por los dos extremos."));
    power_refresh();
}

/* ---- the page ---- */

static void build_page(void)
{
    overlay_close();
    lv_obj_clean(U.content);
    U.canvas = U.status = U.ma_now = U.ma_bar = U.limited = U.ma_full = NULL;
    if (U.cbuf) { aos_hal_io_free(U.cbuf); U.cbuf = NULL; }
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == S.tab ? C_LED : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 1), c, 0);
    }
    lv_obj_t *col = lv_obj_create(U.content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 14, 0);
    lv_obj_set_style_pad_bottom(col, 30, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_update_layout(U.content);
    if (S.tab == TAB_FX) build_fx(col);
    else if (S.tab == TAB_STRIP) build_strip(col);
    else build_power(col);
}

static void tab_cb(lv_event_t *e)
{
    S.tab = (int)(intptr_t)lv_event_get_user_data(e);
    build_page();
}

static void timer_cb(lv_timer_t *t)
{
    if (U.overlay) return;
    preview_draw();
    if (U.status) {
        char s[120];
        status_text(s, sizeof s);
        if (strcmp(lv_label_get_text(U.status), s)) lv_label_set_text(U.status, s);
    }
    power_refresh();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.CW = U.land ? (U.W > 900 ? 900 : U.W) : U.W;
    U.frame = aos_hal_io_alloc(AOS_STRIP_MAX_LEDS * 4);
    const int32_t tab_h = U.land ? 96 : 116;
    U.content = box(root, U.CW, U.H - tab_h);
    lv_obj_set_x(U.content, (U.W - U.CW) / 2);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.content, 12, 0);

    lv_obj_t *bar = box(root, U.W, tab_h);
    lv_obj_set_pos(bar, 0, U.H - tab_h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *tb = box(bar, U.W / TAB_COUNT, tab_h);
        lv_obj_add_flag(tb, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *g = aos_label(tb, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, U.land ? -14 : -16);
        lv_obj_t *n = aos_label(tb, aos_tr(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, U.land ? 26 : 30);
        lv_obj_add_event_cb(tb, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = tb;
    }
    build_page();
    U.timer = lv_timer_create(timer_cb, 60, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    if (U.cbuf) aos_hal_io_free(U.cbuf);
    if (U.frame) aos_hal_io_free(U.frame);
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { overlay_close(); return true; }
    return false;
}

void aos_app_leds_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.leds", .name = "Tiras LED", .icon = AOS_SYM_LED_ON,
            .color_a = 0xFF9F0A, .color_b = 0xB45309,
            .order = 517,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
