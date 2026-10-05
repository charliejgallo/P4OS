/*
 * P4OS - PWM generator: the sheets that open over the screen.
 *
 *   pins      the header's free pins, who holds the others
 *   settings  invert, the servo's stops and travel, the LED's gamma, delete
 *   steps     the values of the "steps" pattern, as vertical sliders
 *   presets   save the channels to /sdcard/pwm, load, delete
 *   wiring    LED, LED to 3V3, servo, IR LED and the RC filter, drawn
 *
 * A sheet is an overlay on the app's root. It is closed with
 * lv_obj_delete_async (a sheet's own button closes it), and that pending
 * deletion is cancelled if the screen is rebuilt first (pw_sheet_forget):
 * the rebuild deletes the sheet itself, and a second delete would be of
 * freed memory (HANDOFF-APPS, the trap of Notas).
 */
#include "pw.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static lv_obj_t *s_panel;
static int32_t   s_panel_w;     /* set before the layout runs: get_width would be 0 */
static uint32_t  s_gen;         /* which sheet: async work checks it is still the same */

/* ---------------------------------------------------------------- the sheet */

static void dying_cb(void *o)
{
    if (o == pw_g->sheet_dying) pw_g->sheet_dying = NULL;
    lv_obj_delete(o);
}

void pw_sheet_close(void)
{
    if (!pw_g->sheet) return;
    lv_obj_t *o = pw_g->sheet;
    pw_g->sheet = NULL;
    pw_g->sheet_ta = pw_g->sheet_kb = NULL;
    s_panel = NULL;
    s_gen++;
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (pw_g->sheet_dying) {
        /* an older one still waiting: it is hidden and idle, delete it now */
        lv_async_call_cancel(dying_cb, pw_g->sheet_dying);
        lv_obj_delete(pw_g->sheet_dying);
    }
    pw_g->sheet_dying = o;
    lv_async_call(dying_cb, o);
}

void pw_sheet_forget(void)
{
    if (pw_g->sheet_dying) {
        lv_async_call_cancel(dying_cb, pw_g->sheet_dying);
        pw_g->sheet_dying = NULL;
    }
    pw_g->sheet = NULL;
    pw_g->sheet_ta = pw_g->sheet_kb = NULL;
    s_panel = NULL;
    s_gen++;
}

static void ov_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) {
        pw_sheet_close();
        pw_ui_refresh();
    }
}

static void done_cb(lv_event_t *e)
{
    (void)e;
    pw_sheet_close();
    pw_ui_refresh();
}

static int32_t inner_w(void) { return s_panel_w - 48; }

lv_obj_t *pw_sheet_open(const char *title)
{
    pw_sheet_close();
    s_gen++;
    lv_obj_t *ov = lv_obj_create(pw_g->root);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, pw_g->W, pw_g->H);
    lv_obj_set_pos(ov, 0, 0);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_FLOATING | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_70, 0);
    lv_obj_add_event_cb(ov, ov_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *p = lv_obj_create(ov);
    lv_obj_remove_style_all(p);
    int32_t w = pw_g->W - 48;
    if (w > (pw_g->land ? 820 : 672)) w = pw_g->land ? 820 : 672;
    lv_obj_set_size(p, w, LV_SIZE_CONTENT);
    s_panel_w = w;
    lv_obj_set_style_max_height(p, pw_g->H - 32, 0);
    lv_obj_center(p);
    lv_obj_set_style_bg_color(p, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(p, 16, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_OFF);
    s_panel = p;

    lv_obj_t *top = lv_obj_create(p);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, w - 48, 72);
    lv_obj_remove_flag(top, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *t = aos_label(top, title, aos_font_title, AOS_C_TEXT);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *ok = pw_pill(top, _("Listo"), AOS_C_ACCENT, done_cb, NULL);
    lv_obj_align(ok, LV_ALIGN_RIGHT_MID, 0, 0);

    pw_g->sheet = ov;
    return p;
}

static lv_obj_t *note(lv_obj_t *p, const char *txt)
{
    lv_obj_t *l = aos_label(p, txt, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, inner_w());
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *row(lv_obj_t *p, int32_t h)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, inner_w(), h);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(r, 12, 0);
    return r;
}

/* ---------------------------------------------------------------- pins */

static void pin_pick_cb(lv_event_t *e)
{
    int gpio = (int)(intptr_t)lv_event_get_user_data(e);
    pw_ch_t *h = pw_cur();
    if (!h) return;
    char m[96];
    int other = pw_gpio_user(gpio, pw_g->sel);
    if (other >= 0) {
        snprintf(m, sizeof m, _("Ese pin lo usa el canal %d"), other + 1);
        aos_ui_toast(m, 1800);
        return;
    }
    const char *o = aos_io_owner(gpio);
    if (o && strcmp(o, PW_OWNER)) {
        snprintf(m, sizeof m, _("El pin lo tiene %s"), o);
        aos_ui_toast(m, 1800);
        return;
    }
    pw_set_gpio(h, gpio);
    pw_sheet_close();
    pw_ui_refresh();
    if (h->err[0]) aos_ui_toast(h->err, 2500);
}

void pw_sheet_pins(void)
{
    lv_obj_t *b = pw_sheet_open(_("Pin del canal"));
    note(b, _("Los pines libres del header. Andan a 3,3 V: nunca 5 V en una pata. La pata es el número en el conector de 40."));
    int32_t w = inner_w();
    lv_obj_t *grid = lv_obj_create(b);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, w, LV_SIZE_CONTENT);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 10, 0);
    int cols = w > 600 ? 4 : 3;
    int32_t cw = (w - (cols - 1) * 10) / cols;

    const aos_io_pin_t *hdr = aos_io_header();
    for (int i = 0; i < 40; i++) {
        const aos_io_pin_t *p = &hdr[i];
        if (p->gpio < 0 || (p->flags & (AOS_PIN_RESERVED | AOS_PIN_BOARD))) continue;
        int us = pw_gpio_user(p->gpio, -1);
        const char *o = aos_io_owner(p->gpio);
        bool other = o && strcmp(o, PW_OWNER);
        lv_obj_t *c = lv_obj_create(grid);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, cw, 100);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(c, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_set_style_radius(c, 16, 0);
        lv_obj_set_style_pad_all(c, 10, 0);
        lv_obj_set_style_border_width(c, 3, 0);
        lv_obj_set_style_border_color(c, us == pw_g->sel ? AOS_C_ACCENT : AOS_C_CARD2, 0);
        if ((us >= 0 && us != pw_g->sel) || other) lv_obj_set_style_opa(c, LV_OPA_50, 0);
        lv_obj_t *n = aos_label(c, p->label, aos_font_small, AOS_C_TEXT);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_t *pn = aos_label(c, "", aos_font_tiny, AOS_C_DIM);
        lv_label_set_text_fmt(pn, _("pata %d"), p->pin);
        lv_obj_align(pn, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        char st[48];
        lv_color_t sc = AOS_C_DIM;
        if (us == pw_g->sel) { snprintf(st, sizeof st, "%s", _("este canal")); sc = AOS_C_ACCENT; }
        else if (us >= 0) { snprintf(st, sizeof st, _("canal %d"), us + 1); sc = AOS_C_ORANGE; }
        else if (other) { snprintf(st, sizeof st, "%s", o); sc = AOS_C_RED; }
        else if (p->flags & AOS_PIN_VO4) { snprintf(st, sizeof st, "%s", _("VO4: medí antes")); sc = AOS_C_ORANGE; }
        else if (p->flags & AOS_PIN_STRAPPING) snprintf(st, sizeof st, "%s", _("strapping"));
        else if (p->flags & AOS_PIN_USB_JTAG) snprintf(st, sizeof st, "USB-JTAG");
        else if (p->flags & AOS_PIN_ADC) snprintf(st, sizeof st, "ADC");
        else snprintf(st, sizeof st, "%s", _("libre"));
        lv_obj_t *sl = aos_label(c, st, aos_font_tiny, sc);
        lv_obj_set_width(sl, cw - 20 - 66);
        lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_add_event_cb(c, pin_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)p->gpio);
    }
}

/* ---------------------------------------------------------------- settings */

enum { ST_SMIN, ST_SMAX, ST_SDEG, ST_GAMMA, ST_COUNT };
static lv_obj_t *s_st_val[ST_COUNT];
static bool s_del_armed;

static void st_text(int id)
{
    pw_ch_t *h = pw_cur();
    if (!h || !s_st_val[id]) return;
    char n[16];
    switch (id) {
    case ST_SMIN:  lv_label_set_text_fmt(s_st_val[id], "%u µs", h->c.smin); break;
    case ST_SMAX:  lv_label_set_text_fmt(s_st_val[id], "%u µs", h->c.smax); break;
    case ST_SDEG:  lv_label_set_text_fmt(s_st_val[id], "%u°", h->c.sdeg); break;
    case ST_GAMMA: pw_fnum(n, sizeof n, h->c.gamma, 1); lv_label_set_text(s_st_val[id], n); break;
    default: break;
    }
}

static void st_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    int code = (int)(intptr_t)lv_event_get_user_data(e);
    int id = code >> 1, dir = code & 1 ? 1 : -1;
    pw_cfg_t *c = &h->c;
    float v = pw_val(c);
    switch (id) {
    case ST_SMIN: {
        int x = (int)c->smin + dir * 10;
        x = x < 200 ? 200 : x > (int)c->smax - 100 ? (int)c->smax - 100 : x;
        c->smin = (uint16_t)x;
        break;
    }
    case ST_SMAX: {
        int x = (int)c->smax + dir * 10;
        x = x < (int)c->smin + 100 ? (int)c->smin + 100 : x > 3000 ? 3000 : x;
        c->smax = (uint16_t)x;
        break;
    }
    case ST_SDEG: {
        int x = (int)c->sdeg + dir * 10;
        c->sdeg = (uint16_t)(x < 10 ? 10 : x > 360 ? 360 : x);
        break;
    }
    case ST_GAMMA: {
        float g = c->gamma + (float)dir * 0.1f;
        c->gamma = g < 1.0f ? 1.0f : g > 3.0f ? 3.0f : g;
        break;
    }
    default: break;
    }
    /* the servo keeps its angle within the new stops */
    if (c->mode == PW_M_SERVO) pw_val_set(c, v);
    st_text(id);
    pw_touch(h);
}

static void stepper(lv_obj_t *p, const char *label, int id)
{
    lv_obj_t *r = row(p, 76);
    lv_obj_t *l = aos_label(r, label, aos_font_small, AOS_C_TEXT);
    lv_obj_set_flex_grow(l, 1);
    for (int k = 0; k < 2; k++) {
        if (k == 1) {
            s_st_val[id] = aos_label_boxed(r, "", aos_font_small, AOS_C_TEXT, 130, 40);
            st_text(id);
        }
        lv_obj_t *b = pw_pill(r, k ? "+" : "−", AOS_C_CARD2, NULL, NULL);
        lv_obj_set_width(b, 72);
        lv_obj_set_style_pad_hor(b, 0, 0);
        void *code = (void *)(intptr_t)(id << 1 | k);
        lv_obj_add_event_cb(b, st_cb, LV_EVENT_PRESSED, code);
        lv_obj_add_event_cb(b, st_cb, LV_EVENT_LONG_PRESSED_REPEAT, code);
    }
}

static void inv_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    h->c.invert = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    h->inv_dirty = h->pwm != NULL;
    pw_touch(h);
}

static void del_cb(lv_event_t *e)
{
    if (!s_del_armed) {
        s_del_armed = true;
        lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), _("Tocá otra vez para borrarlo"));
        return;
    }
    pw_remove(pw_g->sel);
    pw_sheet_close();
    pw_ui_select(pw_g->sel);
}

void pw_sheet_settings(void)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    char t[48];
    snprintf(t, sizeof t, _("Canal %d"), pw_g->sel + 1);
    lv_obj_t *b = pw_sheet_open(t);
    memset(s_st_val, 0, sizeof s_st_val);
    s_del_armed = false;
    int m = h->c.mode;
    if (m != PW_M_DAC) {
        lv_obj_t *r = row(b, 76);
        lv_obj_t *l = aos_label(r, _("Invertir la salida"), aos_font_small, AOS_C_TEXT);
        lv_obj_set_flex_grow(l, 1);
        lv_obj_t *sw = lv_switch_create(r);
        lv_obj_set_size(sw, 96, 54);
        lv_obj_set_style_bg_color(sw, AOS_C_CARD2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(sw, AOS_C_GREEN, LV_PART_INDICATOR | LV_STATE_CHECKED);
        if (h->c.invert) lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_add_event_cb(sw, inv_cb, LV_EVENT_VALUE_CHANGED, NULL);
        note(b, _("Para un LED entre 3V3 y la pata: prende cuando la pata va a 0."));
    }
    if (m == PW_M_SERVO) {
        stepper(b, _("Pulso mínimo"), ST_SMIN);
        stepper(b, _("Pulso máximo"), ST_SMAX);
        stepper(b, _("Recorrido"), ST_SDEG);
        note(b, _("La mayoría de los servos van de 500 a 2500 µs en 180°. Si el servo zumba o golpea en un extremo, acercá los topes."));
    }
    if (m == PW_M_LED) {
        stepper(b, _("Gamma"), ST_GAMMA);
        note(b, _("El ojo ve la luz en escala logarítmica: con gamma 2,2, el brillo al 50 % es un ciclo del 22 %."));
    }
    if (m == PW_M_DAC) note(b, _("El nivel analógico no tiene ajustes: 256 pasos de 0 a 3,3 V, con el filtro RC del cableado."));
    if (pw_g->n > 1) {
        lv_obj_t *d = pw_pill(b, _("Borrar el canal"), AOS_C_RED, del_cb, NULL);
        (void)d;
    }
}

/* ---------------------------------------------------------------- steps */

static lv_obj_t *s_steps_box, *s_steps_count;

static void step_label(lv_obj_t *l, float v)
{
    char big[40], sub[64];
    pw_fmt_value(pw_cur(), v, big, sizeof big, sub, sizeof sub);
    lv_label_set_text(l, big);
}

static void step_val_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *s = lv_event_get_target(e);
    h->c.step[k] = (float)lv_slider_get_value(s) / 1000.0f;
    step_label(lv_obj_get_child(lv_obj_get_parent(s), 0), h->c.step[k]);
    pw_touch(h);
}

static void steps_build(void)
{
    pw_ch_t *h = pw_cur();
    if (!h || !s_steps_box) return;
    lv_obj_clean(s_steps_box);
    int n = h->c.nsteps;
    lv_label_set_text_fmt(s_steps_count, _("%d pasos"), n);
    int32_t w = inner_w();
    int32_t sw = (w - (n - 1) * 8) / n;
    lv_color_t col = lv_color_hex(pw_mode_color(h->c.mode));
    for (int k = 0; k < n; k++) {
        lv_obj_t *c = lv_obj_create(s_steps_box);
        lv_obj_remove_style_all(c);
        lv_obj_set_size(c, sw, 340);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(c, 28, 0);
        lv_obj_t *l = aos_label(c, "", aos_font_tiny, AOS_C_TEXT);
        step_label(l, h->c.step[k]);
        lv_obj_t *s = lv_slider_create(c);
        lv_slider_set_range(s, 0, 1000);
        lv_obj_set_size(s, 28, 260);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
        lv_slider_set_value(s, (int32_t)(h->c.step[k] * 1000.0f + 0.5f), LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s, AOS_C_CARD2, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s, col, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(s, AOS_C_TEXT, LV_PART_KNOB);
        lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);
        lv_obj_set_ext_click_area(s, 12);
        lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_add_event_cb(s, step_val_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)k);
    }
}

static void steps_n_cb(lv_event_t *e)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    int n = h->c.nsteps + dir;
    if (n < 2 || n > PW_STEPS_MAX) return;
    if (dir > 0) h->c.step[n - 1] = h->c.step[n - 2];
    h->c.nsteps = (uint8_t)n;
    pw_touch(h);
    steps_build();
}

void pw_sheet_steps(void)
{
    pw_ch_t *h = pw_cur();
    if (!h) return;
    lv_obj_t *b = pw_sheet_open(_("Pasos"));
    note(b, _("Cada paso dura lo mismo: el período dividido por la cantidad de pasos."));
    lv_obj_t *r = row(b, 76);
    lv_obj_t *m = pw_pill(r, "−", AOS_C_CARD2, steps_n_cb, (void *)(intptr_t)-1);
    lv_obj_set_width(m, 72);
    lv_obj_set_style_pad_hor(m, 0, 0);
    s_steps_count = aos_label_boxed(r, "", aos_font_small, AOS_C_TEXT, 160, 40);
    lv_obj_t *p = pw_pill(r, "+", AOS_C_CARD2, steps_n_cb, (void *)(intptr_t)1);
    lv_obj_set_width(p, 72);
    lv_obj_set_style_pad_hor(p, 0, 0);
    s_steps_box = row(b, 340);
    lv_obj_set_style_pad_gap(s_steps_box, 8, 0);
    steps_build();
}

/* ---------------------------------------------------------------- presets */

#define PRESETS_MAX 32
static lv_obj_t *s_list;
static char      s_names[PRESETS_MAX][40];
static int       s_count, s_armed = -1;

static int name_cmp(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static void preset_path(char *out, size_t n, const char *name)
{
    char dir[128];
    pw_store_dir(dir, sizeof dir);
    snprintf(out, n, "%s/%s.pwm", dir, name);
}

static void presets_list(void);

static void relist_cb(void *gen)
{
    if ((uint32_t)(uintptr_t)gen == s_gen && pw_g && pw_g->sheet) presets_list();
}

static void load_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_count) return;
    char path[200], name[40];
    snprintf(name, sizeof name, "%s", s_names[i]);
    preset_path(path, sizeof path, name);
    pw_cfg_t cfg[PW_CH_MAX];
    int n = pw_store_load(path, cfg, PW_CH_MAX, NULL);
    if (n <= 0) {
        aos_ui_toast(_("No se pudo leer el preset"), 2000);
        return;
    }
    pw_sheet_close();
    pw_apply(cfg, n);
    pw_g->sel = 0;
    pw_ui_select(0);
    char m[80];
    snprintf(m, sizeof m, _("Cargado: %s"), name);
    aos_ui_toast(m, 1500);
}

static void del_preset_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_count) return;
    if (s_armed != i) {
        s_armed = i;
        lv_label_set_text(lv_obj_get_child(lv_event_get_target(e), 0), _("¿Borrar?"));
        return;
    }
    char path[200];
    preset_path(path, sizeof path, s_names[i]);
    remove(path);
    s_armed = -1;
    lv_async_call(relist_cb, (void *)(uintptr_t)s_gen);
}

static void presets_list(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    s_armed = -1;
    s_count = 0;
    char dir[128];
    pw_store_dir(dir, sizeof dir);
    DIR *d = dir[0] ? opendir(dir) : NULL;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) && s_count < PRESETS_MAX) {
            const char *n = de->d_name;
            size_t l = strlen(n);
            if (n[0] == '.' || n[0] == '_' || l < 5 || l - 4 >= sizeof s_names[0] || strcmp(n + l - 4, ".pwm")) continue;
            memcpy(s_names[s_count], n, l - 4);
            s_names[s_count][l - 4] = 0;
            s_count++;
        }
        closedir(d);
    }
    qsort(s_names, (size_t)s_count, sizeof s_names[0], name_cmp);
    if (!s_count) {
        lv_obj_t *l = aos_label(s_list, dir[0] ? _("Todavía no hay presets guardados.") : _("No hay tarjeta."), aos_font_caption, AOS_C_DIM);
        (void)l;
        return;
    }
    int32_t w = inner_w();
    for (int i = 0; i < s_count; i++) {
        lv_obj_t *r = lv_obj_create(s_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, w, 84);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, AOS_C_CARD2, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_set_style_radius(r, 18, 0);
        lv_obj_set_style_pad_hor(r, 18, 0);
        lv_obj_add_event_cb(r, load_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *n = aos_label(r, s_names[i], aos_font_small, AOS_C_TEXT);
        lv_obj_set_width(n, w - 220);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *x = pw_pill(r, _("Borrar"), AOS_C_RED, del_preset_cb, (void *)(intptr_t)i);
        lv_obj_set_height(x, 60);
        lv_obj_align(x, LV_ALIGN_RIGHT_MID, 0, 0);
    }
}

static void slug(char *out, size_t n, const char *in)
{
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && k + 1 < n && k < 32; p++) {
        unsigned char c = *p;
        if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') out[k++] = (char)c;
        else if (c == ' ' && k && out[k - 1] != '-') out[k++] = '-';
    }
    while (k && out[k - 1] == '-') k--;
    out[k] = 0;
}

static void kb_show(bool on)
{
    if (!pw_g->sheet_kb || !s_panel) return;
    if (on) {
        lv_obj_remove_flag(pw_g->sheet_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_max_height(s_panel, pw_g->H - lv_obj_get_height(pw_g->sheet_kb) - 32, 0);
        lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 16);
    } else {
        lv_obj_add_flag(pw_g->sheet_kb, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_max_height(s_panel, pw_g->H - 32, 0);
        lv_obj_center(s_panel);
    }
}

static void save_now(void)
{
    if (!pw_g->sheet_ta) return;
    char name[40];
    slug(name, sizeof name, lv_textarea_get_text(pw_g->sheet_ta));
    if (!name[0]) snprintf(name, sizeof name, "preset-%d", s_count + 1);
    char dir[128], path[200];
    pw_store_dir(dir, sizeof dir);
    if (!dir[0]) {
        aos_ui_toast(_("No hay tarjeta"), 1500);
        return;
    }
    mkdir(dir, 0777);
    preset_path(path, sizeof path, name);
    char m[80];
    if (pw_store_save(path)) {
        snprintf(m, sizeof m, _("Guardado: %s"), name);
        lv_textarea_set_text(pw_g->sheet_ta, "");
        kb_show(false);
        lv_obj_remove_state(pw_g->sheet_ta, LV_STATE_FOCUSED);
        presets_list();
    } else {
        snprintf(m, sizeof m, "%s", _("No se pudo guardar"));
    }
    aos_ui_toast(m, 1500);
}

static void save_cb(lv_event_t *e) { (void)e; save_now(); }

static void ta_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_FOCUSED || c == LV_EVENT_CLICKED) kb_show(true);
    else if (c == LV_EVENT_READY) save_now();
    else if (c == LV_EVENT_CANCEL) kb_show(false);
}

void pw_sheet_presets(void)
{
    lv_obj_t *b = pw_sheet_open(_("Presets"));
    note(b, _("Todos los canales con sus pines, modos y patrones, en /sdcard/pwm. Al cargar uno, los canales encendidos se encienden."));
    lv_obj_t *r = row(b, 76);
    lv_obj_t *ta = lv_textarea_create(r);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_max_length(ta, 32);
    lv_textarea_set_placeholder_text(ta, _("nombre del preset"));
    lv_obj_set_height(ta, 72);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_style_text_font(ta, aos_font_small, 0);
    lv_obj_set_style_bg_color(ta, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 18, 0);
    lv_obj_add_event_cb(ta, ta_cb, LV_EVENT_ALL, NULL);

    pw_pill(r, _("Guardar"), AOS_C_ACCENT, save_cb, NULL);
    pw_g->sheet_ta = ta;

    s_list = lv_obj_create(b);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, inner_w(), LV_SIZE_CONTENT);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(s_list, 10, 0);
    presets_list();

    lv_obj_t *kb = lv_keyboard_create(pw_g->sheet);
    lv_obj_set_size(kb, pw_g->W, pw_g->land ? pw_g->H * 45 / 100 : 380);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_small);
    lv_keyboard_set_textarea(kb, ta);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    pw_g->sheet_kb = kb;
}

/* ---------------------------------------------------------------- wiring */

static const char *s_wire_map[6];
static lv_obj_t *s_wire_draw, *s_wire_cap;

static const char *const WIRE_NOTES[5] = {
    N_("La pata da 3,3 V y pocos mA: el LED con su resistencia (220 a 470 Ω) a GND. En modo LED, el brillo va con la curva de gamma."),
    N_("El LED prende cuando la pata va a 0: activá Invertir en los ajustes del canal."),
    N_("El servo se alimenta aparte, con 5 V externos y no de la placa: GND en común con el header y sólo la señal a la pata. Modo Servo: 50 Hz y un pulso de 500 a 2500 µs."),
    N_("Un LED infrarrojo pide más corriente que la pata: un transistor NPN con 1 kΩ en la base. Para la portadora de un control remoto, PWM a 38 kHz con ciclo del 33 %."),
    N_("El nivel analógico es un tren de pulsos a 1 MHz: 1 kΩ en serie y 1 µF a GND lo vuelven una tensión de 0 a 3,3 V, con pocos mV de ripple y ~5 ms para asentarse. Sin carga pesada a la salida."),
};

static void wire_show(void)
{
    lv_label_set_text(s_wire_cap, _(WIRE_NOTES[pw_g->wire_tab]));
    lv_obj_invalidate(s_wire_draw);
}

static void wire_tab_cb(lv_event_t *e)
{
    uint32_t i = lv_buttonmatrix_get_selected_button(lv_event_get_target(e));
    if (i >= 5) return;
    pw_g->wire_tab = (int)i;
    wire_show();
}

static void pin_list(char *out, size_t n, const char *label)
{
    const aos_io_pin_t *hdr = aos_io_header();
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < 40 && k + 8 < n; i++) {
        if (strcmp(hdr[i].label, label)) continue;
        k += (size_t)snprintf(out + k, n - k, "%s%d", k ? ", " : "", hdr[i].pin);
    }
}

void pw_sheet_wiring(void)
{
    pw_ch_t *h = pw_cur();
    lv_obj_t *b = pw_sheet_open(_("Cableado"));
    s_wire_map[0] = _("LED");
    s_wire_map[1] = _("LED a 3V3");
    s_wire_map[2] = _("Servo");
    s_wire_map[3] = _("LED IR");
    s_wire_map[4] = _("Filtro RC");
    s_wire_map[5] = "";
    int tab = 0;
    if (h) {
        switch (h->c.mode) {
        case PW_M_SERVO: tab = 2; break;
        case PW_M_DAC:   tab = 4; break;
        case PW_M_LED:   tab = h->c.invert ? 1 : 0; break;
        default:         tab = h->c.freq >= 30000 && h->c.freq <= 60000 ? 3 : h->c.invert ? 1 : 0; break;
        }
    }
    pw_g->wire_tab = tab;
    int32_t w = inner_w();
    lv_obj_t *bm = pw_bm(b, s_wire_map, w, 64, aos_font_caption, wire_tab_cb);
    pw_bm_set(bm, 5, tab, 0);

    lv_obj_t *d = lv_obj_create(b);
    lv_obj_remove_style_all(d);
    /* lying down the sheet is short: the drawing gives up height, keeping its shape */
    int32_t dh = w * 6 / 10;
    if (pw_g->land && dh > pw_g->H - 330) dh = pw_g->H - 330;
    lv_obj_set_size(d, w, dh);
    lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(d, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(d, 18, 0);
    lv_obj_add_event_cb(d, pw_draw_schematic, LV_EVENT_DRAW_MAIN_END, NULL);
    s_wire_draw = d;

    s_wire_cap = aos_label(b, "", aos_font_caption, AOS_C_TEXT);
    lv_obj_set_width(s_wire_cap, w);
    lv_label_set_long_mode(s_wire_cap, LV_LABEL_LONG_MODE_WRAP);

    char gnd[64], v33[32], v5[32], txt[256], sig[64] = "";
    pin_list(gnd, sizeof gnd, "GND");
    pin_list(v33, sizeof v33, "3V3");
    pin_list(v5, sizeof v5, "5V");
    if (h && h->c.gpio >= 0) {
        const aos_io_pin_t *p = aos_io_pin_of_gpio(h->c.gpio);
        snprintf(sig, sizeof sig, _("Señal: pata %d (GPIO%d)."), p ? p->pin : 0, h->c.gpio);
        strcat(sig, " ");
    }
    size_t k = (size_t)snprintf(txt, sizeof txt, "%s", sig);
    snprintf(txt + k, sizeof txt - k, _("GND: patas %s. 3V3: patas %s. 5V: patas %s (sólo para alimentar algo chico; un servo, mejor aparte)."),
             gnd, v33, v5);
    note(b, txt);
    wire_show();
}
