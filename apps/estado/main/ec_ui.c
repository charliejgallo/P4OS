/*
 * P4OS - Mi casa: the pieces the screens share, the way numbers
 * and times are written, and the notice's beeps.
 */
#include "ec.h"

#include <stdlib.h>
#include <time.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

void *ec_psram(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

lv_obj_t *ec_box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t *ec_card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = ec_box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

/* one line, cut with dots at w (no cut: w = 0) */
lv_obj_t *ec_text(lv_obj_t *parent, const char *t, const lv_font_t *f, lv_color_t c, int32_t w)
{
    lv_obj_t *l = aos_label(parent, t, f, c);
    if (w > 0) {
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_size(l, w, lv_font_get_line_height(f));
    }
    aos_make_decorative(l);
    return l;
}

lv_obj_t *ec_round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ec_box(parent, AOS_UI_TAP_MIN, AOS_UI_TAP_MIN);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x48484A), LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return b;
}

/* a wide button: a glyph and a word, on a colour */
lv_obj_t *ec_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = ec_box(parent, LV_SIZE_CONTENT, 80);
    lv_obj_set_style_radius(b, 40, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 10, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, AOS_C_TEXT));
    if (text) aos_make_decorative(aos_label(b, text, aos_font_body, AOS_C_TEXT));
    return b;
}

void ec_set_text(lv_obj_t *l, const char *t)
{
    if (!l) return;
    const char *cur = lv_label_get_text(l);
    if (!cur || strcmp(cur, t)) lv_label_set_text(l, t);
}

static char dec_point(void)
{
    const char *lang = aos_i18n_current();
    return lang && !strncmp(lang, "en", 2) ? '.' : ',';
}

void ec_fmt_ms(char *out, size_t n, int ms)
{
    if (ms < 0) ec_copy(out, n, "--");
    else if (ms == 0) ec_copy(out, n, "<1 ms");
    else if (ms < 10000) snprintf(out, n, "%d ms", ms);
    else snprintf(out, n, "%d%c%d s", ms / 1000, dec_point(), ms % 1000 / 100);
}

/* a duration, whole: each unit through _(), since "d" is "j" in French and "g" in Italian */
void ec_fmt_dur(char *out, size_t n, uint32_t s)
{
    int d = (int)(s / 86400), h = (int)(s % 86400 / 3600), m = (int)(s % 3600 / 60);
    if (d) snprintf(out, n, _("%d d %d h"), d, h);
    else if (h) snprintf(out, n, _("%d h %d min"), h, m);
    else if (m) snprintf(out, n, _("%d min"), m);
    else snprintf(out, n, _("%d s"), (int)s);
}

/* "hace 5 min", a phrase of its own; one that says since when is a sentence
 * of its own too ("Caído desde hace %s" with ec_fmt_dur), never this glued
 * after another word */
void ec_fmt_ago(char *out, size_t n, uint32_t s)
{
    char d[40];
    if (s < 10) {
        ec_copy(out, n, _("recién"));
        return;
    }
    ec_fmt_dur(d, sizeof d, s);
    snprintf(out, n, _("hace %s"), d);
}

void ec_fmt_pct(char *out, size_t n, float pct)
{
    if (pct < 0) {
        ec_copy(out, n, "--");
        return;
    }
    /* cut, never rounded up: 99.96 is not 100 */
    int t = (int)(pct * 100.0f);
    if (t >= 10000) snprintf(out, n, "100 %%");
    else if (t >= 9900) snprintf(out, n, "%d%c%02d %%", t / 100, dec_point(), t % 100);
    else snprintf(out, n, "%d%c%d %%", t / 100, dec_point(), t % 100 / 10);
}

void ec_fmt_when(char *out, size_t n, uint32_t epoch)
{
    if (!epoch) {
        ec_copy(out, n, "--");
        return;
    }
    time_t t = (time_t)epoch, now = time(NULL);
    struct tm a, b;
    localtime_r(&t, &a);
    localtime_r(&now, &b);
    if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year)
        snprintf(out, n, _("hoy %02d:%02d"), a.tm_hour, a.tm_min);
    else if (now - t < 7 * 86400)
        snprintf(out, n, "%s %02d:%02d", aos_tr(aos_day_name(a.tm_wday)), a.tm_hour, a.tm_min);
    else
        snprintf(out, n, "%d %s %02d:%02d", a.tm_mday, aos_tr(aos_month_name(a.tm_mon)), a.tm_hour, a.tm_min);
}

const char *ec_glyph(const ec_svc_t *s)
{
    return ec_tmpl(s->tmpl)->glyph;
}

/* ---- beeps: two notes, the second a fifth of a second later ---- */

static struct { int freq; uint32_t at; } B;

void ec_beep(bool down)
{
    aos_hal_beep(down ? 1318 : 880, 140);
    B.freq = down ? 880 : 1318;
    B.at = (uint32_t)aos_hal_uptime_ms() + 190;
}

void ec_beep_tick(void)
{
    if (!B.freq || (int32_t)((uint32_t)aos_hal_uptime_ms() - B.at) < 0) return;
    aos_hal_beep(B.freq, 180);
    B.freq = 0;
}
