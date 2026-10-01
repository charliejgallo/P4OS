/*
 * P4OS - Electrónica: the calculators of the bench.
 *
 *   Colores   resistor colour code, 4 or 5 bands, both ways: pick the bands
 *             and read the value, or type a value and see the bands
 *   Ohm       V, I, R, P: any two give the other two
 *   Divisor   Vin, R1, R2 -> Vout, current, power
 *   LED       supply, forward voltage and current -> the resistor, the
 *             nearest E12/E24 value above it and the power it burns
 *   555       astable: R1, R2, C -> frequency, times, duty cycle
 *   SMD       the code printed on a chip resistor (472, 4R7, 01C) -> value
 *
 * Values are typed on a keypad that knows engineering notation: "4k7",
 * "4,7k", "100n", "2.2u" (µ appears as u on the keys), and the results come
 * back the same way ("4,7 kΩ").
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { P_COLORS = 0, P_OHM, P_DIV, P_LED, P_555, P_SMD, P_COUNT };
static const char *const PAGE_NAME[P_COUNT] = { N_("Colores"), N_("Ohm"), N_("Divisor"), N_("LED"), N_("555"), N_("SMD") };

#define C_ACC lv_color_hex(0xFACC15)

/* -------------------------------------------------------------------------- */
/* Engineering notation                                                        */
/* -------------------------------------------------------------------------- */

static const struct { char c; double f; } PREFIX[] = {
    { 'p', 1e-12 }, { 'n', 1e-9 }, { 'u', 1e-6 }, { 'm', 1e-3 }, { 'k', 1e3 }, { 'K', 1e3 }, { 'M', 1e6 }, { 'G', 1e9 },
};

/* "4k7", "4,7k", "100n", "2.2u", "1e3", "470" -> number; NAN if not one. */
static double parse_eng(const char *s)
{
    char buf[40];
    int n = 0;
    double mult = 1;
    bool seen_prefix = false;
    for (const char *p = s; *p && n < (int)sizeof buf - 1; p++) {
        char c = *p;
        if (c == ' ') continue;
        if (c == ',') c = '.';
        if ((unsigned char)c == 0xC2 && (unsigned char)p[1] == 0xB5) { c = 'u'; p++; }  /* µ */
        bool is_prefix = false;
        for (size_t i = 0; i < sizeof PREFIX / sizeof PREFIX[0]; i++)
            if (c == PREFIX[i].c && !(c == 'm' && 0)) { is_prefix = true; if (!seen_prefix) { mult = PREFIX[i].f; seen_prefix = true; } break; }
        if (c == 'R' || c == 'r') is_prefix = true, seen_prefix = true;     /* 4R7 */
        if (is_prefix) {
            /* the prefix works as the decimal point when digits follow (4k7) */
            if (p[1] && strchr(buf, '.') == NULL) buf[n++] = '.';
            continue;
        }
        if ((c >= '0' && c <= '9') || c == '.' || c == '-' || c == 'e' || c == 'E' || c == '+') buf[n++] = c;
        else return NAN;
    }
    buf[n] = 0;
    if (!n) return NAN;
    char *end;
    double v = strtod(buf, &end);
    if (*end) return NAN;
    return v * mult;
}

/* 4700 -> "4,7 k" + unit; three significant figures, the decimal comma. */
static void fmt_eng(double v, const char *unit, char *out, size_t n)
{
    if (!isfinite(v)) { snprintf(out, n, "—"); return; }
    static const char *pre[] = { "p", "n", "µ", "m", "", "k", "M", "G" };
    double a = fabs(v);
    int e = 4;
    if (a > 0) {
        e = (int)floor(log10(a) / 3) + 4;
        if (e < 0) e = 0;
        if (e > 7) e = 7;
    }
    double s = v / pow(1000, e - 4);
    char num[24];
    if (fabs(s) >= 100) snprintf(num, sizeof num, "%.0f", s);
    else if (fabs(s) >= 10) snprintf(num, sizeof num, "%.1f", s);
    else snprintf(num, sizeof num, "%.2f", s);
    /* trim zeros after the point, then the point */
    if (strchr(num, '.')) {
        char *z = num + strlen(num) - 1;
        while (*z == '0') *z-- = 0;
        if (*z == '.') *z = 0;
    }
    for (char *p = num; *p; p++) if (*p == '.') *p = ',';
    snprintf(out, n, "%s %s%s", num, pre[e], unit);
}

/* the E12 / E24 series, one decade */
static const double E12[] = { 1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2 };
static const double E24[] = { 1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                              3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1 };

static double series_above(double v, const double *s, int n)
{
    if (!(v > 0)) return NAN;
    double dec = pow(10, floor(log10(v)));
    for (int k = 0; k < 2; k++, dec *= 10)
        for (int i = 0; i < n; i++) if (s[i] * dec >= v * 0.9999) return s[i] * dec;
    return NAN;
}

/* -------------------------------------------------------------------------- */
/* State                                                                       */
/* -------------------------------------------------------------------------- */

static struct {
    int page;
    /* colours */
    int bands;              /* 4 or 5 */
    int band[5];            /* colour index per band */
    int sel_band;
    /* Ohm: which two were typed last */
    double ohm[4];          /* V I R P */
    int ohm_a, ohm_b;
    /* the other pages' inputs */
    double div_vin, div_r1, div_r2;
    double led_vs, led_vf, led_i;
    double t_r1, t_r2, t_c;
    char smd[12];
} S = {
    .bands = 4, .band = { 4, 7, 2, 10, 10 },
    .ohm = { 12, 0.02, NAN, NAN }, .ohm_a = 0, .ohm_b = 1,
    .div_vin = 12, .div_r1 = 10e3, .div_r2 = 4.7e3,
    .led_vs = 5, .led_vf = 2.0, .led_i = 0.02,
    .t_r1 = 1e3, .t_r2 = 10e3, .t_c = 10e-6,
    .smd = "472",
};

static struct {
    lv_obj_t *root, *content, *tabs[P_COUNT], *kp;
    lv_obj_t *resistor, *band_obj[5], *value, *extra, *chips;
    lv_obj_t *fields[6], *out[6];
    int32_t W, H;
    bool land;
    /* the keypad's target */
    double *target;
    const char *target_unit;
    int target_ohm;         /* Ohm page: which field, -1 otherwise */
    char *target_str;
    lv_obj_t *kp_text;
} U;

static void build_page(void);
static void refresh(void);

/* -------------------------------------------------------------------------- */
/* Pieces                                                                      */
/* -------------------------------------------------------------------------- */

static lv_obj_t *card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 22, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

/* A field: name on the left, value on the right, tap to type. */
static void field_cb(lv_event_t *e);

static lv_obj_t *field(lv_obj_t *parent, const char *name, int idx)
{
    lv_obj_t *c = card(parent);
    lv_obj_set_height(c, 104);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_t *n = aos_label(c, name, aos_font_body, AOS_C_DIM);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = aos_label(c, "", aos_font_large, AOS_C_TEXT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(c, field_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    U.fields[idx] = c;
    U.out[idx] = v;
    return c;
}

static lv_obj_t *result(lv_obj_t *parent, const char *name, int idx)
{
    lv_obj_t *c = card(parent);
    lv_obj_set_height(c, 96);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x1F2A1F), 0);
    lv_obj_t *n = aos_label(c, name, aos_font_body, lv_color_hex(0x9CCC9C));
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *v = aos_label(c, "", aos_font_large, AOS_C_TEXT);
    lv_obj_align(v, LV_ALIGN_RIGHT_MID, 0, 0);
    U.out[idx] = v;
    return c;
}

static void set_out(int idx, double v, const char *unit)
{
    if (!U.out[idx]) return;
    char b[40];
    fmt_eng(v, unit, b, sizeof b);
    lv_label_set_text(U.out[idx], b);
    lv_obj_align(U.out[idx], LV_ALIGN_RIGHT_MID, 0, 0);
}

/* -------------------------------------------------------------------------- */
/* Keypad                                                                      */
/* -------------------------------------------------------------------------- */

static void kp_close(void)
{
    if (U.kp) lv_obj_delete(U.kp);
    U.kp = NULL;
}

static const char *KP_MAP[] = {
    "7", "8", "9", "k", "M", "\n",
    "4", "5", "6", "m", "u", "\n",
    "1", "2", "3", "n", "p", "\n",
    ",", "0", "-", LV_SYMBOL_BACKSPACE, LV_SYMBOL_OK, ""
};

static void kp_event(lv_event_t *e)
{
    lv_obj_t *m = lv_event_get_target(e);
    const char *t = lv_buttonmatrix_get_button_text(m, lv_buttonmatrix_get_selected_button(m));
    if (!t) return;
    char cur[40];
    snprintf(cur, sizeof cur, "%s", lv_label_get_text(U.kp_text));
    if (!strcmp(t, LV_SYMBOL_BACKSPACE)) {
        size_t n = strlen(cur);
        if (n) cur[n - 1] = 0;
    } else if (!strcmp(t, LV_SYMBOL_OK)) {
        if (U.target_str) {
            snprintf(U.target_str, 12, "%s", cur);
        } else {
            double v = parse_eng(cur);
            if (isnan(v)) { aos_ui_toast(_("No es un número"), 1500); return; }
            *U.target = v;
            if (U.target_ohm >= 0) {
                /* the two most recent inputs define the other two */
                if (U.target_ohm != S.ohm_b) { S.ohm_a = S.ohm_b; S.ohm_b = U.target_ohm; }
            }
        }
        kp_close();
        refresh();
        return;
    } else if (strlen(cur) < 16) {
        strlcat(cur, t, sizeof cur);
    }
    lv_label_set_text(U.kp_text, cur);
}

static void kp_bg_cb(lv_event_t *e) { if (lv_event_get_target(e) == U.kp) kp_close(); }

static void kp_open(const char *title, double *target, const char *unit, int ohm_idx, char *target_str)
{
    kp_close();
    U.target = target;
    U.target_unit = unit;
    U.target_ohm = ohm_idx;
    U.target_str = target_str;
    U.kp = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.kp);
    lv_obj_set_size(U.kp, U.W, U.H);
    lv_obj_set_style_bg_color(U.kp, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.kp, LV_OPA_70, 0);
    lv_obj_add_flag(U.kp, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.kp, kp_bg_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(U.kp);
    lv_obj_remove_style_all(panel);
    int32_t pw = U.land ? 640 : U.W, ph = U.land ? U.H : 700;
    lv_obj_set_size(panel, pw, ph);
    lv_obj_align(panel, U.land ? LV_ALIGN_RIGHT_MID : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x16161B), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 36, 0);
    lv_obj_set_style_pad_all(panel, 24, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = aos_label(panel, title, aos_font_small, AOS_C_DIM);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    U.kp_text = aos_label(panel, "", aos_font_large, AOS_C_TEXT);
    lv_obj_align(U.kp_text, LV_ALIGN_TOP_RIGHT, 0, 30);
    lv_obj_t *u = aos_label(panel, unit, aos_font_body, AOS_C_DIM);
    lv_obj_align(u, LV_ALIGN_TOP_LEFT, 0, 50);

    lv_obj_t *m = lv_buttonmatrix_create(panel);
    lv_buttonmatrix_set_map(m, KP_MAP);
    lv_obj_set_size(m, lv_pct(100), ph - 170);
    lv_obj_align(m, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_pad_all(m, 0, 0);
    lv_obj_set_style_pad_gap(m, 12, 0);
    lv_obj_set_style_bg_color(m, AOS_C_CARD2, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(m, AOS_C_ACCENT, LV_PART_ITEMS | LV_STATE_PRESSED);
    lv_obj_set_style_text_color(m, AOS_C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(m, aos_font_title, LV_PART_ITEMS);
    lv_obj_set_style_radius(m, 22, LV_PART_ITEMS);
    lv_obj_set_style_border_width(m, 0, LV_PART_ITEMS);
    lv_obj_set_style_shadow_width(m, 0, LV_PART_ITEMS);
    lv_obj_add_event_cb(m, kp_event, LV_EVENT_VALUE_CHANGED, NULL);
}

/* -------------------------------------------------------------------------- */
/* Colours                                                                     */
/* -------------------------------------------------------------------------- */

static const struct { const char *name; uint32_t rgb; double tol; } COL[12] = {
    { N_("negro"), 0x111111, -1 }, { N_("marrón"), 0x7A3E12, 1 }, { N_("rojo"), 0xD32F2F, 2 },
    { N_("naranja"), 0xF57C00, -1 }, { N_("amarillo"), 0xFBC02D, -1 }, { N_("verde"), 0x388E3C, 0.5 },
    { N_("azul"), 0x1976D2, 0.25 }, { N_("violeta"), 0x7B1FA2, 0.1 }, { N_("gris"), 0x8E8E93, 0.05 },
    { N_("blanco"), 0xF5F5F5, -1 }, { N_("dorado"), 0xC9A227, 5 }, { N_("plateado"), 0xBDBDBD, 10 },
};

static int nbands(void) { return S.bands; }

/* digits bands 0..(n-3), multiplier n-2, tolerance n-1 */
static double band_value(void)
{
    int nd = nbands() - 2, n = nbands();
    double v = 0;
    for (int i = 0; i < nd; i++) {
        if (S.band[i] > 9) return NAN;
        v = v * 10 + S.band[i];
    }
    int mul = S.band[n - 2];
    double f = mul <= 9 ? pow(10, mul) : mul == 10 ? 0.1 : 0.01;
    return v * f;
}

static void value_to_bands(double v)
{
    int nd = nbands() - 2;
    if (!(v > 0)) return;
    int e = (int)floor(log10(v)) - (nd - 1);
    long digits = lround(v / pow(10, e));
    if (digits >= (long)pow(10, nd)) { digits /= 10; e++; }
    for (int i = nd - 1; i >= 0; i--) { S.band[i] = (int)(digits % 10); digits /= 10; }
    S.band[nd] = e >= 0 ? e : (e == -1 ? 10 : 11);
}

static void draw_resistor(void)
{
    if (!U.resistor) return;
    int n = nbands();
    for (int i = 0; i < 5; i++) {
        if (!U.band_obj[i]) continue;
        lv_obj_set_flag(U.band_obj[i], LV_OBJ_FLAG_HIDDEN, i >= n);
        if (i >= n) continue;
        lv_obj_set_style_bg_color(U.band_obj[i], lv_color_hex(COL[S.band[i]].rgb), 0);
        lv_obj_set_style_border_width(U.band_obj[i], i == S.sel_band ? 5 : 0, 0);
    }
}

static void band_cb(lv_event_t *e) { S.sel_band = (int)(intptr_t)lv_event_get_user_data(e); build_page(); }

static void chip_cb(lv_event_t *e)
{
    int c = (int)(intptr_t)lv_event_get_user_data(e);
    S.band[S.sel_band] = c;
    if (S.sel_band < nbands() - 1) S.sel_band++;
    build_page();
}

static void nb_cb(lv_event_t *e)
{
    double v = band_value();
    int tol = S.band[nbands() - 1];
    S.bands = S.bands == 4 ? 5 : 4;
    value_to_bands(v);
    S.band[nbands() - 1] = tol;
    S.sel_band = 0;
    build_page();
}

static double s_typed_r = NAN;
static void type_r_cb(lv_event_t *e) { kp_open(_("Valor de la resistencia"), &s_typed_r, "Ω", -1, NULL); }

static bool chip_ok(int band, int c)
{
    int n = nbands();
    if (band < n - 2) return c <= 9 && !(band == 0 && c == 0);
    if (band == n - 2) return c <= 11;           /* the multiplier: gold x0.1, silver x0.01 */
    return COL[c].tol > 0;                       /* the tolerance */
}

static void build_colors(lv_obj_t *p)
{
    /* the resistor: body, leads, bands */
    lv_obj_t *box = lv_obj_create(p);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), 190);
    lv_obj_t *lead = lv_obj_create(box);
    lv_obj_remove_style_all(lead);
    lv_obj_set_size(lead, lv_pct(100), 10);
    lv_obj_set_style_bg_color(lead, lv_color_hex(0xB0B0B0), 0);
    lv_obj_set_style_bg_opa(lead, LV_OPA_COVER, 0);
    lv_obj_center(lead);
    U.resistor = lv_obj_create(box);
    lv_obj_remove_style_all(U.resistor);
    int32_t rw = 480, rh = 150;
    lv_obj_set_size(U.resistor, rw, rh);
    lv_obj_center(U.resistor);
    lv_obj_set_style_radius(U.resistor, 60, 0);
    lv_obj_set_style_bg_color(U.resistor, lv_color_hex(0xD9C29A), 0);
    lv_obj_set_style_bg_grad_color(U.resistor, lv_color_hex(0xA88A5A), 0);
    lv_obj_set_style_bg_grad_dir(U.resistor, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(U.resistor, LV_OPA_COVER, 0);
    int n = nbands();
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = lv_obj_create(U.resistor);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 44, rh);
        int x = i < n - 1 ? 80 + i * (n == 4 ? 70 : 58) : rw - 110;
        lv_obj_set_pos(b, x, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, C_ACC, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, band_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.band_obj[i] = b;
    }

    U.value = aos_label(p, "", aos_font_huge, AOS_C_TEXT);   /* has k and Ω, the digit font does not */
    U.extra = aos_label(p, "", aos_font_body, AOS_C_DIM);

    lv_obj_t *row = lv_obj_create(p);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_t *b1 = lv_button_create(row);
    lv_obj_t *l1 = aos_label(b1, S.bands == 4 ? _("4 bandas") : _("5 bandas"), aos_font_small, AOS_C_TEXT);
    (void)l1;
    lv_obj_set_style_bg_color(b1, AOS_C_CARD2, 0);
    lv_obj_set_style_radius(b1, 24, 0);
    lv_obj_set_style_pad_all(b1, 20, 0);
    lv_obj_add_event_cb(b1, nb_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = lv_button_create(row);
    aos_label(b2, _("Escribir valor"), aos_font_small, AOS_C_TEXT);
    lv_obj_set_style_bg_color(b2, AOS_C_ACCENT, 0);
    lv_obj_set_style_radius(b2, 24, 0);
    lv_obj_set_style_pad_all(b2, 20, 0);
    lv_obj_add_event_cb(b2, type_r_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *hint = aos_label(p, S.sel_band < n - 2 ? _("Cifra") : S.sel_band == n - 2 ? _("Multiplicador") : _("Tolerancia"),
                               aos_font_small, AOS_C_DIM);
    (void)hint;
    U.chips = lv_obj_create(p);
    lv_obj_remove_style_all(U.chips);
    lv_obj_set_size(U.chips, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(U.chips, 14, 0);
    for (int c = 0; c < 12; c++) {
        if (!chip_ok(S.sel_band, c)) continue;
        lv_obj_t *ch = lv_obj_create(U.chips);
        lv_obj_remove_style_all(ch);
        lv_obj_set_size(ch, 150, 84);
        lv_obj_set_style_radius(ch, 20, 0);
        lv_obj_set_style_bg_color(ch, lv_color_hex(COL[c].rgb), 0);
        lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ch, S.band[S.sel_band] == c ? 4 : 0, 0);
        lv_obj_set_style_border_color(ch, lv_color_white(), 0);
        lv_obj_add_flag(ch, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(ch, chip_cb, LV_EVENT_CLICKED, (void *)(intptr_t)c);
        lv_obj_t *l = aos_label(ch, aos_tr(COL[c].name), aos_font_caption,
                                (c == 4 || c == 9 || c == 11 || c == 10 || c == 8) ? lv_color_black() : lv_color_white());
        lv_obj_center(l);
    }
}

static void refresh_colors(void)
{
    if (!isnan(s_typed_r)) { value_to_bands(s_typed_r); s_typed_r = NAN; build_page(); return; }
    draw_resistor();
    double v = band_value();
    double tol = COL[S.band[nbands() - 1]].tol;
    char b[40];
    fmt_eng(v, "Ω", b, sizeof b);
    lv_label_set_text(U.value, b);
    char e12[24], e24[24];
    fmt_eng(series_above(v, E12, 12), "", e12, sizeof e12);
    fmt_eng(series_above(v, E24, 24), "", e24, sizeof e24);
    char t[16];
    snprintf(t, sizeof t, "%g", tol);
    for (char *c = t; *c; c++) if (*c == '.') *c = ',';
    lv_label_set_text_fmt(U.extra, "±%s %%   ·   E12 %s   ·   E24 %s", t, e12, e24);
}

/* -------------------------------------------------------------------------- */
/* Ohm, divider, LED, 555, SMD                                                 */
/* -------------------------------------------------------------------------- */

static const char *const OHM_NAME[4] = { N_("Tensión (V)"), N_("Corriente (A)"), N_("Resistencia (Ω)"), N_("Potencia (W)") };
static const char *const OHM_UNIT[4] = { "V", "A", "Ω", "W" };

static void ohm_solve(void)
{
    double V = NAN, I = NAN, R = NAN, P = NAN;
    double a = S.ohm[S.ohm_a], b = S.ohm[S.ohm_b];
    int k = (1 << S.ohm_a) | (1 << S.ohm_b);
    switch (k) {
    case 0x3: V = S.ohm[0]; I = S.ohm[1]; R = V / I; P = V * I; break;
    case 0x5: V = S.ohm[0]; R = S.ohm[2]; I = V / R; P = V * V / R; break;
    case 0x9: V = S.ohm[0]; P = S.ohm[3]; I = P / V; R = V * V / P; break;
    case 0x6: I = S.ohm[1]; R = S.ohm[2]; V = I * R; P = I * I * R; break;
    case 0xA: I = S.ohm[1]; P = S.ohm[3]; V = P / I; R = P / (I * I); break;
    case 0xC: R = S.ohm[2]; P = S.ohm[3]; V = sqrt(P * R); I = sqrt(P / R); break;
    default: (void)a; (void)b; break;
    }
    S.ohm[0] = V; S.ohm[1] = I; S.ohm[2] = R; S.ohm[3] = P;
}

static void field_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    switch (S.page) {
    case P_OHM: kp_open(aos_tr(OHM_NAME[i]), &S.ohm[i], OHM_UNIT[i], i, NULL); break;
    case P_DIV: kp_open(i == 0 ? "Vin" : i == 1 ? "R1" : "R2", i == 0 ? &S.div_vin : i == 1 ? &S.div_r1 : &S.div_r2,
                        i == 0 ? "V" : "Ω", -1, NULL); break;
    case P_LED: kp_open(i == 0 ? _("Alimentación") : i == 1 ? _("Tensión del LED") : _("Corriente"),
                        i == 0 ? &S.led_vs : i == 1 ? &S.led_vf : &S.led_i, i == 2 ? "A" : "V", -1, NULL); break;
    case P_555: kp_open(i == 0 ? "R1" : i == 1 ? "R2" : "C", i == 0 ? &S.t_r1 : i == 1 ? &S.t_r2 : &S.t_c,
                        i == 2 ? "F" : "Ω", -1, NULL); break;
    case P_SMD: kp_open(_("Código SMD"), NULL, "", -1, S.smd); break;
    default: break;
    }
}

static void led_preset_cb(lv_event_t *e)
{
    S.led_vf = (double)(intptr_t)lv_event_get_user_data(e) / 100.0;
    refresh();
}

static lv_obj_t *vcol(lv_obj_t *p)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, U.land ? (U.W - 2 * AOS_UI_PAD - 20) / 2 : lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void two_cols(lv_obj_t *p, lv_obj_t **a, lv_obj_t **b)
{
    if (!U.land) { *a = *b = p; return; }
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(p, 20, 0);
    *a = vcol(p);
    *b = vcol(p);
}

static void build_ohm(lv_obj_t *p)
{
    lv_obj_t *a, *b;
    two_cols(p, &a, &b);
    aos_label(a, _("Escribí dos valores; los otros dos salen solos."), aos_font_small, AOS_C_DIM);
    for (int i = 0; i < 4; i++) field(i < 2 ? a : b, aos_tr(OHM_NAME[i]), i);
}

static void build_div(lv_obj_t *p)
{
    lv_obj_t *a, *b;
    two_cols(p, &a, &b);
    field(a, _("Vin (V)"), 0);
    field(a, "R1 (Ω)", 1);
    field(a, "R2 (Ω)", 2);
    result(b, "Vout", 3);
    result(b, _("Corriente"), 4);
    result(b, _("Potencia R1+R2"), 5);
    aos_label(b, _("Vout = Vin · R2 / (R1 + R2)"), aos_font_small, AOS_C_DIM);
}

static void build_led(lv_obj_t *p)
{
    lv_obj_t *a, *b;
    two_cols(p, &a, &b);
    field(a, _("Alimentación (V)"), 0);
    field(a, _("Tensión del LED (V)"), 1);
    lv_obj_t *pr = lv_obj_create(a);
    lv_obj_remove_style_all(pr);
    lv_obj_set_size(pr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pr, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(pr, 10, 0);
    static const struct { const char *n; int vf; uint32_t c; } LEDS[] = {
        { N_("Rojo"), 200, 0xD32F2F }, { N_("Amarillo"), 210, 0xFBC02D }, { N_("Verde"), 220, 0x388E3C },
        { N_("Azul"), 300, 0x1976D2 }, { N_("Blanco"), 310, 0xE0E0E0 }, { N_("IR"), 130, 0x5D4037 },
    };
    for (int i = 0; i < 6; i++) {
        lv_obj_t *bt = lv_button_create(pr);
        lv_obj_set_style_bg_color(bt, lv_color_hex(LEDS[i].c), 0);
        lv_obj_set_style_radius(bt, 20, 0);
        lv_obj_set_style_pad_all(bt, 14, 0);
        aos_label(bt, aos_tr(LEDS[i].n), aos_font_caption, i == 4 || i == 1 ? lv_color_black() : lv_color_white());
        lv_obj_add_event_cb(bt, led_preset_cb, LV_EVENT_CLICKED, (void *)(intptr_t)LEDS[i].vf);
    }
    field(a, _("Corriente (A)"), 2);
    result(b, _("Resistencia"), 3);
    result(b, _("E12 más cercana"), 4);
    result(b, _("Potencia en R"), 5);
}

static void build_555(lv_obj_t *p)
{
    lv_obj_t *a, *b;
    two_cols(p, &a, &b);
    aos_label(a, _("Astable: R1 entre Vcc y descarga, R2 entre descarga y umbral."), aos_font_small, AOS_C_DIM);
    field(a, "R1 (Ω)", 0);
    field(a, "R2 (Ω)", 1);
    field(a, "C (F)", 2);
    result(b, _("Frecuencia"), 3);
    result(b, _("Tiempo alto"), 4);
    result(b, _("Ciclo útil"), 5);
}

static void build_smd(lv_obj_t *p)
{
    aos_label(p, _("El código impreso en una resistencia SMD: 472, 4R7, 1002, 01C (EIA-96)."), aos_font_small, AOS_C_DIM);
    field(p, _("Código"), 0);
    result(p, _("Valor"), 1);
}

/* EIA-96: 2 digits index into the E96 series + a letter multiplier */
static const int E96[96] = {
    100, 102, 105, 107, 110, 113, 115, 118, 121, 124, 127, 130, 133, 137, 140, 143, 147, 150, 154, 158,
    162, 165, 169, 174, 178, 182, 187, 191, 196, 200, 205, 210, 215, 221, 226, 232, 237, 243, 249, 255,
    261, 267, 274, 280, 287, 294, 301, 309, 316, 324, 332, 340, 348, 357, 365, 374, 383, 392, 402, 412,
    422, 432, 442, 453, 464, 475, 487, 499, 511, 523, 536, 549, 562, 576, 590, 604, 619, 634, 649, 665,
    681, 698, 715, 732, 750, 768, 787, 806, 825, 845, 866, 887, 909, 931, 953, 976,
};

static double smd_value(const char *c)
{
    size_t n = strlen(c);
    if (n == 3 && c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9' && strchr("ZYRXSABHCDEF", c[2]) && c[2] > '9') {
        int idx = (c[0] - '0') * 10 + (c[1] - '0');
        if (idx < 1 || idx > 96) return NAN;
        static const char L[] = "ZYRXSABHCDEF";
        static const double M[] = { 0.001, 0.01, 0.01, 0.1, 0.1, 1, 10, 10, 100, 1e3, 1e4, 1e5 };
        return E96[idx - 1] * M[strchr(L, c[2]) - L];
    }
    if (strchr(c, 'R') || strchr(c, 'r')) return parse_eng(c);
    if (n == 3 || n == 4) {
        double d = 0;
        for (size_t i = 0; i < n - 1; i++) { if (c[i] < '0' || c[i] > '9') return NAN; d = d * 10 + c[i] - '0'; }
        if (c[n - 1] < '0' || c[n - 1] > '9') return NAN;
        return d * pow(10, c[n - 1] - '0');
    }
    return parse_eng(c);
}

static void refresh(void)
{
    char b[40];
    switch (S.page) {
    case P_COLORS: refresh_colors(); break;
    case P_OHM:
        ohm_solve();
        for (int i = 0; i < 4; i++) {
            set_out(i, S.ohm[i], OHM_UNIT[i]);
            bool input = i == S.ohm_a || i == S.ohm_b;
            lv_obj_set_style_text_color(U.out[i], input ? C_ACC : AOS_C_TEXT, 0);
        }
        break;
    case P_DIV: {
        set_out(0, S.div_vin, "V"); set_out(1, S.div_r1, "Ω"); set_out(2, S.div_r2, "Ω");
        double i = S.div_vin / (S.div_r1 + S.div_r2);
        set_out(3, S.div_vin * S.div_r2 / (S.div_r1 + S.div_r2), "V");
        set_out(4, i, "A");
        set_out(5, S.div_vin * i, "W");
        break;
    }
    case P_LED: {
        set_out(0, S.led_vs, "V"); set_out(1, S.led_vf, "V"); set_out(2, S.led_i, "A");
        double r = (S.led_vs - S.led_vf) / S.led_i;
        double e = series_above(r, E12, 12);
        set_out(3, r > 0 ? r : NAN, "Ω");
        set_out(4, e, "Ω");
        set_out(5, r > 0 ? (S.led_vs - S.led_vf) * (S.led_vs - S.led_vf) / e : NAN, "W");
        break;
    }
    case P_555: {
        set_out(0, S.t_r1, "Ω"); set_out(1, S.t_r2, "Ω"); set_out(2, S.t_c, "F");
        double th = 0.693 * (S.t_r1 + S.t_r2) * S.t_c, tl = 0.693 * S.t_r2 * S.t_c;
        set_out(3, 1.0 / (th + tl), "Hz");
        set_out(4, th, "s");
        if (U.out[5]) {
            snprintf(b, sizeof b, "%.1f %%", 100.0 * th / (th + tl));
            for (char *c = b; *c; c++) if (*c == '.') *c = ',';
            lv_label_set_text(U.out[5], b);
            lv_obj_align(U.out[5], LV_ALIGN_RIGHT_MID, 0, 0);
        }
        break;
    }
    case P_SMD:
        lv_label_set_text(U.out[0], S.smd);
        lv_obj_align(U.out[0], LV_ALIGN_RIGHT_MID, 0, 0);
        set_out(1, smd_value(S.smd), "Ω");
        break;
    default: break;
    }
}

/* -------------------------------------------------------------------------- */
/* Tabs and life cycle                                                         */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    kp_close();
    lv_obj_clean(U.content);
    memset(U.fields, 0, sizeof U.fields);
    memset(U.out, 0, sizeof U.out);
    memset(U.band_obj, 0, sizeof U.band_obj);
    U.resistor = U.value = U.extra = U.chips = NULL;
    lv_obj_set_flex_flow(U.content, LV_FLEX_FLOW_COLUMN);
    switch (S.page) {
    case P_COLORS: build_colors(U.content); break;
    case P_OHM: build_ohm(U.content); break;
    case P_DIV: build_div(U.content); break;
    case P_LED: build_led(U.content); break;
    case P_555: build_555(U.content); break;
    default: build_smd(U.content); break;
    }
    for (int i = 0; i < P_COUNT; i++) {
        lv_obj_set_style_bg_opa(U.tabs[i], i == S.page ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), i == S.page ? lv_color_black() : AOS_C_TEXT, 0);
    }
    refresh();
}

static void tab_cb(lv_event_t *e)
{
    S.page = (int)(intptr_t)lv_event_get_user_data(e);
    build_page();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;

    lv_obj_t *tabs = lv_obj_create(root);
    lv_obj_remove_style_all(tabs);
    lv_obj_set_size(tabs, U.W - 2 * AOS_UI_PAD, 72);
    lv_obj_set_pos(tabs, AOS_UI_PAD, 10);
    lv_obj_set_style_bg_color(tabs, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tabs, 36, 0);
    lv_obj_set_style_pad_all(tabs, 6, 0);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(tabs, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < P_COUNT; i++) {
        lv_obj_t *t = lv_obj_create(tabs);
        lv_obj_remove_style_all(t);
        lv_obj_set_height(t, lv_pct(100));
        lv_obj_set_flex_grow(t, 1);
        lv_obj_set_style_radius(t, 30, 0);
        lv_obj_set_style_bg_color(t, C_ACC, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *l = aos_label(t, aos_tr(PAGE_NAME[i]), aos_font_small, AOS_C_TEXT);
        lv_obj_center(l);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }

    U.content = lv_obj_create(root);
    lv_obj_remove_style_all(U.content);
    lv_obj_set_size(U.content, U.W, U.H - 96);
    lv_obj_set_pos(U.content, 0, 96);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_row(U.content, 16, 0);
    lv_obj_set_scroll_dir(U.content, LV_DIR_VER);
    build_page();
    return &U;
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.kp) { kp_close(); return true; }
    return false;
}

void aos_app_elec_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.elec", .name = "Electrónica", .icon = AOS_SYM_LIGHTNING_BOLT,
            .color_a = 0xFACC15, .color_b = 0xCA8A04,
            .flags = AOS_APP_FLAG_KEEP, .order = 160,
        },
        .create = create, .back = back,
    };
}
