/*
 * P4OS - Conversor (from AmoledOS)
 *
 * Twelve magnitudes, each with its base unit, and every conversion solved
 * with a straight line: base = v * factor + offset. The offset exists for a
 * single family -temperature, where Fahrenheit and Kelvin do not pass
 * through zero at the same time as Celsius- but having it in the table
 * avoids the special case in the code, which is where the mistake always
 * creeps in.
 *
 * On the watch the magnitude was a button that opened a list, and the two
 * rows and the keypad had to share 448 px. Here the twelve magnitudes are
 * always on screen as a grid of tiles (six by two upright, three by four in
 * a column lying down), the two values get cards of their own with the
 * number in the big digit face, and the keypad has room for keys of 100 px.
 * The units are still picked from a sheet, now a grid with the symbol and
 * the whole name, because "nmi" alone is not understood and "milla
 * náutica" is.
 *
 * The keypad borrows Electrónica's look (dark rounded keys, the accent on
 * press) but not its calculators: engineering prefixes, Ohm and the rest
 * live there. Two families were added for the bench that Electrónica does
 * not cover: power (W, hp, CV, BTU/h) and angle.
 *
 * The numbers are grouped the Spanish way, 1.073.741.824 and 0,0393701,
 * both what you type and what comes out. The magnitude and both units are
 * kept in the watch's preference keys; the typed value lives in statics, so
 * leaving the app (KEEP) or turning the screen loses nothing.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_fonts.h"
#include "aos_sys_glyphs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_ACC       lv_color_hex(0x40C8E0)      /* the app's teal */
#define C_NUM       lv_color_hex(0x2C2C2E)
#define C_FN        lv_color_hex(0x1D3A45)
#define C_TILE      lv_color_hex(0x1C1C1E)
#define C_TILE_ON   lv_color_hex(0x154B59)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ENTRY_MAX   14
#define MAX_UNITS   9

typedef struct {
    const char *sym;        /* SI notation or the usual abbreviation: not translated */
    const char *full;       /* the whole name, translated */
    double      factor;     /* base = v * factor + offset */
    double      offset;
} unit_t;

typedef struct {
    const char *name;
    const char *glyph;
    int         count;
    unit_t      unit[MAX_UNITS];
} family_t;

/* Exact factors where the unit is defined exactly (the inch is 25.4 mm since
 * 1959, the pound 0.45359237 kg), not rounded to four figures, which is where
 * the annoying differences on converting back and forth come from. */
static const family_t FAMILY[] = {
    { N_("Longitud"), AOS_SYM_RULER, 9, {
        { "mm",  N_("milímetro"),       0.001,          0 },
        { "cm",  N_("centímetro"),      0.01,           0 },
        { "m",   N_("metro"),           1.0,            0 },
        { "km",  N_("kilómetro"),       1000.0,         0 },
        { "in",  N_("pulgada"),         0.0254,         0 },
        { "ft",  N_("pie"),             0.3048,         0 },
        { "yd",  N_("yarda"),           0.9144,         0 },
        { "mi",  N_("milla"),           1609.344,       0 },
        { "nmi", N_("milla náutica"),   1852.0,         0 },
    } },
    { N_("Masa"), AOS_SYM_WEIGHT_KILOGRAM, 7, {
        { "mg",  N_("miligramo"),       0.000001,       0 },
        { "g",   N_("gramo"),           0.001,          0 },
        { "kg",  N_("kilogramo"),       1.0,            0 },
        { "t",   N_("tonelada"),        1000.0,         0 },
        { "oz",  N_("onza"),            0.028349523125, 0 },
        { "lb",  N_("libra"),           0.45359237,     0 },
        { "st",  N_("stone"),           6.35029318,     0 },
    } },
    { N_("Temperatura"), AOS_SYM_THERMOMETER, 3, {
        { "°C",  N_("grado Celsius"),   1.0,            0.0 },
        { "°F",  N_("grado Fahrenheit"),5.0 / 9.0,     -160.0 / 9.0 },
        { "K",   N_("kelvin"),          1.0,           -273.15 },
    } },
    { N_("Volumen"), AOS_SYM_CUP_WATER, 9, {
        { "ml",  N_("mililitro"),       0.001,          0 },
        { "cl",  N_("centilitro"),      0.01,           0 },
        { "l",   N_("litro"),           1.0,            0 },
        { "m³",  N_("metro cúbico"),    1000.0,         0 },
        { "fl oz", N_("onza líquida (EE. UU.)"), 0.0295735295625, 0 },
        { "cup", N_("taza (EE. UU.)"),  0.2365882365,   0 },
        { "pt",  N_("pinta (EE. UU.)"), 0.473176473,    0 },
        { "qt",  N_("cuarto (EE. UU.)"),0.946352946,    0 },
        { "gal", N_("galón (EE. UU.)"), 3.785411784,    0 },
    } },
    { N_("Velocidad"), AOS_SYM_SPEEDOMETER, 5, {
        { "m/s", N_("metro por segundo"),   1.0,             0 },
        { "km/h",N_("kilómetro por hora"),  1.0 / 3.6,       0 },
        { "mph", N_("milla por hora"),      0.44704,         0 },
        { "ft/s",N_("pie por segundo"),     0.3048,          0 },
        { "kn",  N_("nudo"),                1852.0 / 3600.0, 0 },
    } },
    { N_("Área"), AOS_SYM_VECTOR_SQUARE, 9, {
        { "cm²", N_("centímetro cuadrado"), 0.0001,         0 },
        { "m²",  N_("metro cuadrado"),      1.0,            0 },
        { "ha",  N_("hectárea"),            10000.0,        0 },
        { "km²", N_("kilómetro cuadrado"),  1000000.0,      0 },
        { "in²", N_("pulgada cuadrada"),    0.00064516,     0 },
        { "ft²", N_("pie cuadrado"),        0.09290304,     0 },
        { "yd²", N_("yarda cuadrada"),      0.83612736,     0 },
        { "ac",  N_("acre"),                4046.8564224,   0 },
        { "mi²", N_("milla cuadrada"),      2589988.110336, 0 },
    } },
    { N_("Presión"), AOS_SYM_GAUGE, 7, {
        { "Pa",  N_("pascal"),              1.0,            0 },
        { "hPa", N_("hectopascal"),         100.0,          0 },
        { "kPa", N_("kilopascal"),          1000.0,         0 },
        { "bar", N_("bar"),                 100000.0,       0 },
        { "atm", N_("atmósfera"),           101325.0,       0 },
        { "psi", N_("libra por pulgada²"),  6894.757293168, 0 },
        { "mmHg",N_("milímetro de mercurio"),133.322387415, 0 },
    } },
    { N_("Datos"), AOS_SYM_DATABASE, 8, {
        { "B",   N_("byte"),                1.0,            0 },
        { "kB",  N_("kilobyte"),            1000.0,         0 },
        { "MB",  N_("megabyte"),            1000000.0,      0 },
        { "GB",  N_("gigabyte"),            1000000000.0,   0 },
        { "TB",  N_("terabyte"),            1000000000000.0, 0 },
        { "KiB", N_("kibibyte"),            1024.0,         0 },
        { "MiB", N_("mebibyte"),            1048576.0,      0 },
        { "GiB", N_("gibibyte"),            1073741824.0,   0 },
    } },
    { N_("Energía"), AOS_SYM_LIGHTNING_BOLT, 7, {
        { "J",   N_("joule"),               1.0,            0 },
        { "kJ",  N_("kilojoule"),           1000.0,         0 },
        { "cal", N_("caloría"),             4.184,          0 },
        { "kcal",N_("kilocaloría"),         4184.0,         0 },
        { "Wh",  N_("watt-hora"),           3600.0,         0 },
        { "kWh", N_("kilowatt-hora"),       3600000.0,      0 },
        { "BTU", N_("BTU"),                 1055.05585262,  0 },
    } },
    { N_("Tiempo"), AOS_SYM_CLOCK_OUTLINE, 7, {
        { "ms",  N_("milisegundo"),         0.001,          0 },
        { "s",   N_("segundo"),             1.0,            0 },
        { "min", N_("minuto"),              60.0,           0 },
        { "h",   N_("hora"),                3600.0,         0 },
        { "d",   N_("día"),                 86400.0,        0 },
        { "sem", N_("semana"),              604800.0,       0 },
        { "año", N_("año (365,25 días)"),   31557600.0,     0 },
    } },
    { N_("Potencia"), AOS_SYM_ENGINE, 8, {
        { "mW",  N_("miliwatt"),            0.001,          0 },
        { "W",   N_("watt"),                1.0,            0 },
        { "kW",  N_("kilowatt"),            1000.0,         0 },
        { "MW",  N_("megawatt"),            1000000.0,      0 },
        { "hp",  N_("caballo de fuerza"),   745.69987158227022, 0 },
        { "CV",  N_("caballo de vapor"),    735.49875,      0 },
        { "BTU/h", N_("BTU por hora"),      0.29307107017,  0 },
        { "kcal/h", N_("kilocaloría por hora"), 1.163,      0 },
    } },
    { N_("Ángulo"), AOS_SYM_ANGLE_ACUTE, 7, {
        { "°",   N_("grado"),               M_PI / 180.0,   0 },
        { "rad", N_("radián"),              1.0,            0 },
        { "mrad",N_("milirradián"),         0.001,          0 },
        { "gon", N_("gradián"),             M_PI / 200.0,   0 },
        { "vuelta", N_("vuelta completa"),  2.0 * M_PI,     0 },
        { "'",   N_("minuto de arco"),      M_PI / 10800.0, 0 },
        { "\"",  N_("segundo de arco"),     M_PI / 648000.0, 0 },
    } },
};
#define FAMILY_COUNT    ((int)(sizeof(FAMILY) / sizeof(FAMILY[0])))

/* -------------------------------------------------------------------------- */
/* State (survives leaving the app and turning the screen)                     */
/* -------------------------------------------------------------------------- */

static struct {
    bool started;
    int  family, from, to;
    char entry[ENTRY_MAX + 1];      /* digits and at most one ',' */
    bool negative;
} S;

static struct {
    lv_obj_t *root, *page;
    int32_t W, H;
    bool land;
    lv_obj_t *tile[FAMILY_COUNT];
    lv_obj_t *chip_a, *chip_b, *full_a, *full_b, *val_a, *val_b, *formula;
    int32_t val_w;
    lv_obj_t *sheet;                /* the unit picker, NULL when closed */
    int       sheet_mode;           /* 1 source, 2 target */
} U;

/* -------------------------------------------------------------------------- */
/* Numbers                                                                     */
/* -------------------------------------------------------------------------- */

/* "1234567,5" -> "1.234.567,5": dots every three digits of the integer part.
 * 'in' uses ',' as the decimal mark already. */
static void group(const char *in, char *out, size_t n)
{
    const char *comma = strchr(in, ',');
    const char *e = strpbrk(in, "eE");
    size_t ilen = comma ? (size_t)(comma - in) : e ? (size_t)(e - in) : strlen(in);
    size_t o = 0, start = 0;
    if (in[0] == '-') { if (o + 1 < n) out[o++] = '-'; start = 1; }
    size_t digits = ilen - start;
    for (size_t i = start; i < ilen && o + 2 < n; i++) {
        out[o++] = in[i];
        size_t left = ilen - i - 1;
        if (left && left % 3 == 0 && digits > 4) out[o++] = '.';   /* 1234 stays 1234 */
    }
    for (size_t i = ilen; in[i] && o + 1 < n; i++) out[o++] = in[i];
    out[o] = 0;
}

/* Nine significant figures, the decimal comma, grouped. Tiny rounding tails
 * (0,30000000004) are gone at nine figures; only the truly huge or tiny keep
 * an exponent. */
static void format_number(double v, char *out, size_t n)
{
    if (!isfinite(v)) { snprintf(out, n, "---"); return; }
    if (fabs(v) < 1e-300) v = 0;            /* no "-0" */
    char raw[40];
    double a = fabs(v);
    if (a == 0 || (a >= 1e-6 && a < 1e15)) {
        /* nine significant figures, but never an exponent for a number a
         * person would write out: 4,01234568e+13 is 40.123.456.789.000 */
        int dec = 8 - (a > 0 ? (int)floor(log10(a)) : 0);
        if (dec < 0) dec = 0;
        if (dec > 12) dec = 12;
        snprintf(raw, sizeof raw, "%.*f", dec, v);
        if (strchr(raw, '.')) {
            char *z = raw + strlen(raw) - 1;
            while (*z == '0') *z-- = 0;
            if (*z == '.') *z = 0;
        }
    } else {
        snprintf(raw, sizeof raw, "%.9g", v);
    }
    for (char *p = raw; *p; p++) if (*p == '.') *p = ',';
    group(raw, out, n);
}

static double entry_value(void)
{
    char buf[ENTRY_MAX + 2];
    snprintf(buf, sizeof buf, "%s", S.entry[0] ? S.entry : "0");
    for (char *p = buf; *p; p++) if (*p == ',') *p = '.';
    double v = strtod(buf, NULL);
    return S.negative ? -v : v;
}

static double convert_v(double v, int from, int to)
{
    const family_t *f = &FAMILY[S.family];
    const unit_t *a = &f->unit[from], *b = &f->unit[to];
    return (v * a->factor + a->offset - b->offset) / b->factor;
}

/* The big number in the digit face if it fits and has only its glyphs
 * (0-9 , . - and space); otherwise the text face one size down, then two. */
static void set_big(lv_obj_t *l, const char *txt)
{
    const lv_font_t *fonts[] = { &aos_inter_num_96, aos_font_huge, aos_font_large, aos_font_title };
    bool digits_only = strpbrk(txt, "eE") == NULL;
    for (size_t i = digits_only ? 0 : 1; i < sizeof fonts / sizeof fonts[0]; i++) {
        lv_point_t sz;
        lv_text_get_size(&sz, txt, fonts[i], 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= U.val_w || i == sizeof fonts / sizeof fonts[0] - 1) {
            lv_obj_set_style_text_font(l, fonts[i], 0);
            break;
        }
    }
    lv_label_set_text(l, txt);
}

/* -------------------------------------------------------------------------- */
/* Preferences (the watch's keys)                                              */
/* -------------------------------------------------------------------------- */

static void save_prefs(void)
{
    aos_hal_pref_set_i32("conv_fam", S.family);
    aos_hal_pref_set_i32("conv_from", S.from);
    aos_hal_pref_set_i32("conv_to", S.to);
}

static void load_prefs(void)
{
    int32_t v = 0;
    S.family = 0; S.from = 3; S.to = 7;             /* km -> mi, the first time */
    if (aos_hal_pref_get_i32("conv_fam", &v) && v >= 0 && v < FAMILY_COUNT) {
        S.family = (int)v;
        S.from = 0; S.to = 1;
    }
    int n = FAMILY[S.family].count;
    if (aos_hal_pref_get_i32("conv_from", &v) && v >= 0 && v < n) S.from = (int)v;
    if (aos_hal_pref_get_i32("conv_to", &v) && v >= 0 && v < n) S.to = (int)v;
    if (S.to == S.from) S.to = (S.from + 1) % n;
}

/* -------------------------------------------------------------------------- */
/* Refresh                                                                     */
/* -------------------------------------------------------------------------- */

static void refresh(void)
{
    if (!U.val_a) return;
    const family_t *f = &FAMILY[S.family];
    char raw[ENTRY_MAX + 4], buf[48];

    snprintf(raw, sizeof raw, "%s%s", S.negative ? "-" : "", S.entry[0] ? S.entry : "0");
    group(raw, buf, sizeof buf);
    set_big(U.val_a, buf);

    format_number(convert_v(entry_value(), S.from, S.to), buf, sizeof buf);
    set_big(U.val_b, buf);

    lv_label_set_text(lv_obj_get_child(U.chip_a, 0), f->unit[S.from].sym);
    lv_label_set_text(lv_obj_get_child(U.chip_b, 0), f->unit[S.to].sym);
    lv_label_set_text(U.full_a, _(f->unit[S.from].full));
    lv_label_set_text(U.full_b, _(f->unit[S.to].full));

    char one[40];
    format_number(convert_v(1.0, S.from, S.to), one, sizeof one);
    snprintf(buf, sizeof buf, "1 %s = %s %s", f->unit[S.from].sym, one, f->unit[S.to].sym);
    lv_label_set_text(U.formula, buf);

    for (int i = 0; i < FAMILY_COUNT; i++) {
        if (!U.tile[i]) continue;
        bool on = i == S.family;
        lv_obj_set_style_bg_color(U.tile[i], on ? C_TILE_ON : C_TILE, 0);
        lv_obj_set_style_border_width(U.tile[i], on ? 3 : 0, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tile[i], 0), on ? C_ACC : AOS_C_DIM, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tile[i], 1), on ? AOS_C_TEXT : AOS_C_DIM, 0);
    }
}

/* -------------------------------------------------------------------------- */
/* The unit sheet                                                              */
/* -------------------------------------------------------------------------- */

static void sheet_close(void)
{
    if (U.sheet) lv_obj_delete(U.sheet);
    U.sheet = NULL;
    U.sheet_mode = 0;
}

static void sheet_pick_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    int n = FAMILY[S.family].count;
    aos_hal_activity();
    if (U.sheet_mode == 1) {
        if (idx == S.to) S.to = S.from;             /* picking the other one swaps them */
        S.from = idx;
    } else {
        if (idx == S.from) S.from = S.to;
        S.to = idx;
    }
    if (S.to == S.from) S.to = (S.from + 1) % n;
    save_prefs();
    sheet_close();
    refresh();
}

static void sheet_bg_cb(lv_event_t *e)
{
    if (lv_event_get_target(e) == U.sheet) sheet_close();
}

static void sheet_open(int mode)
{
    sheet_close();
    const family_t *f = &FAMILY[S.family];
    const int cur = mode == 1 ? S.from : S.to;
    U.sheet_mode = mode;

    U.sheet = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.sheet);
    lv_obj_set_size(U.sheet, U.W, U.H);
    lv_obj_set_style_bg_color(U.sheet, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.sheet, LV_OPA_70, 0);
    lv_obj_add_flag(U.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(U.sheet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(U.sheet, sheet_bg_cb, LV_EVENT_CLICKED, NULL);

    const int cols = 3;
    const int rows = (f->count + cols - 1) / cols;
    const int32_t pw = U.land ? 900 : U.W;
    const int32_t bh = 112, gap = 14;
    const int32_t ph = 24 + 56 + rows * bh + (rows - 1) * gap + 40;

    lv_obj_t *p = lv_obj_create(U.sheet);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, pw, ph);
    lv_obj_align(p, U.land ? LV_ALIGN_CENTER : LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x16161B), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, 36, 0);
    lv_obj_set_style_pad_all(p, 24, 0);
    lv_obj_add_flag(p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(p, LV_OBJ_FLAG_SCROLLABLE);

    char t[64];
    snprintf(t, sizeof t, "%s  ·  %s", mode == 1 ? _("Convertir de") : _("Convertir a"), _(f->name));
    lv_obj_t *title = aos_label(p, t, aos_font_body, AOS_C_TEXT);
    lv_obj_set_pos(title, 4, 4);

    const int32_t bw = (pw - 48 - (cols - 1) * gap) / cols;
    for (int i = 0; i < f->count; i++) {
        lv_obj_t *b = lv_obj_create(p);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, bw, bh);
        lv_obj_set_pos(b, (i % cols) * (bw + gap), 56 + (i / cols) * (bh + gap));
        lv_obj_set_style_radius(b, 22, 0);
        lv_obj_set_style_bg_color(b, i == cur ? C_TILE_ON : C_NUM, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, C_ACC, 0);
        lv_obj_set_style_border_width(b, i == cur ? 3 : 0, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_60, LV_STATE_PRESSED);
        lv_obj_set_style_pad_hor(b, 16, 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, sheet_pick_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *s = aos_label(b, f->unit[i].sym, aos_font_title, i == cur ? C_ACC : AOS_C_TEXT);
        lv_obj_align(s, LV_ALIGN_TOP_LEFT, 0, 14);
        lv_obj_t *n = aos_label(b, _(f->unit[i].full), aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(n, bw - 32);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 0, -14);
        aos_make_decorative(s);
        aos_make_decorative(n);
    }
}

/* -------------------------------------------------------------------------- */
/* Events                                                                      */
/* -------------------------------------------------------------------------- */

static void family_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    aos_hal_activity();
    if (i == S.family) return;
    S.family = i;
    S.from = 0;
    S.to = 1;
    /* the typed value stays: 100 of one thing is often 100 of another */
    if (i == 2) { S.from = 0; S.to = 1; }        /* °C -> °F */
    save_prefs();
    refresh();
}

static void chip_a_cb(lv_event_t *e) { (void)e; aos_hal_activity(); sheet_open(1); }
static void chip_b_cb(lv_event_t *e) { (void)e; aos_hal_activity(); sheet_open(2); }

static void key_cb(lv_event_t *e)
{
    char code = (char)(intptr_t)lv_event_get_user_data(e);
    size_t len = strlen(S.entry);
    aos_hal_activity();

    switch (code) {
    case 'C':
        S.entry[0] = 0;
        S.negative = false;
        break;
    case '<':
        if (len) S.entry[len - 1] = 0;
        else S.negative = false;
        break;
    case '~':
        S.negative = !S.negative;
        break;
    case 'S': {
        /* the result becomes what you typed, in the other unit: 1 km -> mi
         * shows 0,621371, and after the swap 0,621371 mi -> km shows 1 */
        int t = S.from;
        S.from = S.to;
        S.to = t;
        save_prefs();
        break;
    }
    case ',':
        if (len < ENTRY_MAX && !strchr(S.entry, ',')) {
            if (!len) S.entry[len++] = '0';
            S.entry[len] = ',';
            S.entry[len + 1] = 0;
        }
        break;
    default:
        if (len < ENTRY_MAX) {
            if (len == 1 && S.entry[0] == '0') {
                if (code == '0') break;             /* no "00" */
                len = 0;                            /* no "07" */
            }
            S.entry[len] = code;
            S.entry[len + 1] = 0;
        }
        break;
    }
    refresh();
}

/* -------------------------------------------------------------------------- */
/* Construction                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_tiles(int32_t x, int32_t y, int cols, int32_t tw, int32_t th, int32_t gap)
{
    for (int i = 0; i < FAMILY_COUNT; i++) {
        lv_obj_t *t = box(U.page);
        lv_obj_set_size(t, tw, th);
        lv_obj_set_pos(t, x + (i % cols) * (tw + gap), y + (i / cols) * (th + gap));
        lv_obj_set_style_radius(t, 22, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(t, C_ACC, 0);
        lv_obj_set_style_bg_color(t, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t, family_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *g = aos_label(t, FAMILY[i].glyph, &aos_sym_44, AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, -16);
        const char *name = _(FAMILY[i].name);
        lv_obj_t *n = aos_label_boxed(t, name, aos_font_tiny, AOS_C_DIM, tw, 22);
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_point_t sz;
        lv_text_get_size(&sz, name, aos_font_tiny, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x > tw - 6) lv_obj_set_style_text_letter_space(n, -1, 0);   /* "Temperatura" */
        lv_obj_align(n, LV_ALIGN_CENTER, 0, 28);
        aos_make_decorative(g);
        aos_make_decorative(n);
        U.tile[i] = t;
    }
}

/* A value card: the unit chip (tap = the sheet) and its whole name on top,
 * the number underneath, right-aligned. */
static void build_card(int32_t x, int32_t y, int32_t w, int32_t h, bool input,
                       lv_event_cb_t chip_cb, lv_obj_t **chip, lv_obj_t **full, lv_obj_t **val)
{
    lv_obj_t *c = box(U.page);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, input ? LV_OPA_COVER : LV_OPA_60, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    if (input) {
        /* where you type: marked, so you do not have to guess which of the
         * two numbers the keys change */
        lv_obj_set_style_border_width(c, 3, 0);
        lv_obj_set_style_border_color(c, C_ACC, 0);
        lv_obj_set_style_border_opa(c, LV_OPA_70, 0);
    }

    lv_obj_t *ch = box(c);
    lv_obj_set_size(ch, LV_SIZE_CONTENT, 76);
    lv_obj_set_style_min_width(ch, 150, 0);
    lv_obj_set_style_pad_left(ch, 26, 0);
    lv_obj_set_style_pad_right(ch, 18, 0);
    lv_obj_set_style_radius(ch, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ch, C_NUM, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(ch, LV_OPA_60, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(ch, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ch, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ch, 12, 0);
    lv_obj_add_flag(ch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ch, chip_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sym = aos_label(ch, "", aos_font_title, AOS_C_TEXT);
    lv_obj_t *dn = aos_label(ch, AOS_SYM_CHEVRON_DOWN, &aos_sym_28, C_ACC);
    aos_make_decorative(sym);
    aos_make_decorative(dn);
    lv_obj_align(ch, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *fl = aos_label(c, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(fl, w - 40 - 150 - 24);
    lv_obj_set_style_text_align(fl, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(fl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(fl, LV_ALIGN_TOP_RIGHT, 0, 22);

    lv_obj_t *v = aos_label(c, "0", &aos_inter_num_96, input ? AOS_C_TEXT : C_ACC);
    lv_obj_set_width(v, w - 40);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_CLIP);
    lv_obj_align(v, LV_ALIGN_BOTTOM_RIGHT, 0, 8);
    U.val_w = w - 40;

    *chip = ch;
    *full = fl;
    *val = v;
}

static void key(lv_obj_t *parent, const char *text, bool glyph, char code, lv_color_t bg, lv_color_t fg,
                int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *b = box(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    /* the pressed look by colour, not by transform: a transform is a layer */
    lv_obj_set_style_bg_color(b, C_ACC, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)code);
    lv_obj_t *l = aos_label(b, text, glyph ? &aos_sym_44 : aos_font_large, fg);
    lv_obj_center(l);
    aos_make_decorative(l);
}

static void build_keypad(int32_t x, int32_t y, int32_t w, int32_t h, int32_t gap)
{
    const int32_t kw = (w - 3 * gap) / 4, kh = (h - 3 * gap) / 4;
#define KX(c) (x + (c) * (kw + gap))
#define KY(r) (y + (r) * (kh + gap))
    static const char DIG[3][3] = { { '7', '8', '9' }, { '4', '5', '6' }, { '1', '2', '3' } };
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            char t[2] = { DIG[r][c], 0 };
            key(U.page, t, false, DIG[r][c], C_NUM, AOS_C_TEXT, KX(c), KY(r), kw, kh);
        }
    key(U.page, AOS_SYM_BACKSPACE_OUTLINE, true, '<', C_FN, AOS_C_TEXT, KX(3), KY(0), kw, kh);
    key(U.page, "C", false, 'C', C_FN, AOS_C_ORANGE, KX(3), KY(1), kw, kh);
    key(U.page, AOS_SYM_PLUS_MINUS_VARIANT, true, '~', C_FN, AOS_C_TEXT, KX(3), KY(2), kw, kh);
    key(U.page, "0", false, '0', C_NUM, AOS_C_TEXT, KX(0), KY(3), 2 * kw + gap, kh);
    key(U.page, ",", false, ',', C_NUM, AOS_C_TEXT, KX(2), KY(3), kw, kh);
    key(U.page, AOS_SYM_SWAP_VERTICAL, true, 'S', lv_color_hex(0x1E6F80), AOS_C_TEXT, KX(3), KY(3), kw, kh);
#undef KX
#undef KY
}

static void build(lv_obj_t *root)
{
    lv_obj_clean(root);
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    U.page = aos_page(root);

    const int32_t pad = AOS_UI_PAD, gap = 14;
    if (!U.land) {
        /* tiles 6 x 2, the two cards, the formula, the keypad to the bottom */
        const int32_t tw = (U.W - 2 * pad - 5 * 8) / 6, th = 108;
        build_tiles(pad, 8, 6, tw, th, 8);
        int32_t y = 8 + 2 * th + 8 + 20;
        const int32_t ch = 200, cw = U.W - 2 * pad;
        build_card(pad, y, cw, ch, true, chip_a_cb, &U.chip_a, &U.full_a, &U.val_a);
        y += ch + gap;
        build_card(pad, y, cw, ch, false, chip_b_cb, &U.chip_b, &U.full_b, &U.val_b);
        y += ch + 6;
        U.formula = aos_label_boxed(U.page, "", aos_font_small, AOS_C_DIM, cw, 34);
        lv_obj_set_pos(U.formula, pad, y + 8);
        y += 56;
        build_keypad(pad, y, cw, U.H - y - pad, gap);
    } else {
        /* tiles 3 x 4 on the left, the cards in the middle, the keypad right */
        const int32_t tw = 104, tgap = 10;
        const int32_t th = (U.H - 2 * pad - 3 * tgap) / 4;
        build_tiles(pad, pad, 3, tw, th, tgap);
        const int32_t tiles_w = 3 * tw + 2 * tgap;
        const int32_t kpw = 420;
        const int32_t cx = pad + tiles_w + pad;
        const int32_t cw = U.W - cx - pad - kpw - pad;
        const int32_t ch = (U.H - 2 * pad - gap - 44) / 2;
        build_card(cx, pad, cw, ch, true, chip_a_cb, &U.chip_a, &U.full_a, &U.val_a);
        build_card(cx, pad + ch + gap, cw, ch, false, chip_b_cb, &U.chip_b, &U.full_b, &U.val_b);
        U.formula = aos_label_boxed(U.page, "", aos_font_small, AOS_C_DIM, cw, 34);
        lv_obj_set_pos(U.formula, cx, U.H - pad - 34);
        build_keypad(U.W - pad - kpw, pad, kpw, U.H - 2 * pad, 12);
    }
    refresh();
}

/* -------------------------------------------------------------------------- */
/* Life cycle                                                                  */
/* -------------------------------------------------------------------------- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    if (!S.started) {
        S.started = true;
        load_prefs();
        S.entry[0] = '1';                   /* something to read the first time */
        S.entry[1] = 0;
    }
    build(root);
    return &U;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self; (void)inst;
    int mode = U.sheet_mode;
    build(root);
    if (mode) sheet_open(mode);
    return true;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)inst;
    if (self && self->root) lv_obj_clean(self->root);
    memset(&U, 0, sizeof U);            /* the magnitude, units and value stay in S */
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self; (void)inst;
    if (U.sheet) { sheet_close(); return true; }
    return false;
}

void aos_app_convert_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id       = "aos.convert",
            .name     = "Conversor",
            .icon     = AOS_SYM_SWAP_HORIZONTAL,
            .icon_vec = AOS_ICON_CONVERT,
            .color_a  = 0x40C8E0,
            .color_b  = 0x1C5C77,
            .flags    = AOS_APP_FLAG_KEEP,
            .order    = 56,
        },
        .create  = create,
        .destroy = destroy,
        .back    = back,
        .resize  = resize,
    };
}
