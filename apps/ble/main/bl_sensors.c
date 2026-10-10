/*
 * BLE - "Sensores": what thermometers and the like broadcast.
 *
 * One card per device whose advertisement carries readings in a format
 * bl_decode.c knows: the temperature big, the humidity beside it, the rest
 * in a line, and the last two hours of temperature (one point a minute,
 * kept by bl_scan.c). Nothing here connects to anything: these readings are
 * in the air for anyone to hear, which is the point of those formats.
 */
#include "bl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define CARDS_MAX 32

static struct {
    lv_obj_t *col, *note, *empty;
    lv_obj_t *card[CARDS_MAX], *name[CARDS_MAX], *fmt[CARDS_MAX], *temp[CARDS_MAX], *hum[CARDS_MAX], *line[CARDS_MAX],
        *age[CARDS_MAX], *graph[CARDS_MAX];
    int dev[CARDS_MAX];
    int n;
    uint32_t gen;
} S;

static void card_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= 0 && i < S.n && S.dev[i] >= 0) bl_open_detail(S.dev[i]);
}

/* the temperature's two hours as a line, with its range */
static void graph_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    int i = (int)(intptr_t)lv_obj_get_user_data(o);
    if (i < 0 || i >= S.n || S.dev[i] < 0 || S.dev[i] >= BL.ndev) return;
    const bl_dev_t *d = &BL.dev[S.dev[i]];
    uint32_t newest = (uint32_t)(aos_hal_uptime_ms() / 60000);
    /* the two hours, oldest first: the temperature, or the humidity when
     * there is none */
    float v[BL_SEN_HIST];
    bool hum = false;
    float lo = 1e9f, hi = -1e9f;
    for (int pass = 0; pass < 2 && lo > hi; pass++) {
        hum = pass == 1;
        for (int k = 0; k < BL_SEN_HIST; k++) {
            v[k] = bl_sen_at(d, hum, newest - (BL_SEN_HIST - 1 - k));
            if (!isnan(v[k])) { if (v[k] < lo) lo = v[k]; if (v[k] > hi) hi = v[k]; }
        }
    }
    if (lo > hi) return;
    if (hi - lo < 1) { float m = (hi + lo) / 2; lo = m - 0.5f; hi = m + 0.5f; }
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    int32_t w = lv_area_get_width(&a) - 70, h = lv_area_get_height(&a);
    int32_t px = -1, py = 0;
    lv_color_t c = hum ? AOS_C_TEAL : AOS_C_ORANGE;
    for (int k = 0; k < BL_SEN_HIST; k++) {
        float val = v[k];
        if (isnan(val)) { px = -1; continue; }
        int32_t x = a.x1 + k * w / (BL_SEN_HIST - 1);
        int32_t y = a.y2 - 4 - (int32_t)((val - lo) / (hi - lo) * (h - 8));
        if (px >= 0) bl_draw_line(layer, px, py, x, y, c, 3, LV_OPA_COVER);
        px = x;
        py = y;
    }
    char t[16];
    bl_fmt_num(t, sizeof t, hi, 1);
    bl_draw_text(layer, t, aos_font_tiny, AOS_C_DIM, a.x2 - 66, a.y1, 66, LV_TEXT_ALIGN_RIGHT);
    bl_fmt_num(t, sizeof t, lo, 1);
    bl_draw_text(layer, t, aos_font_tiny, AOS_C_DIM, a.x2 - 66, a.y2 - 18, 66, LV_TEXT_ALIGN_RIGHT);
}

static int sensors_now(int *out, int max)
{
    static int idx[BL_DEV_MAX];
    int n = bl_sorted(idx, BL_DEV_MAX, BL_FILT_SENSOR, BL_SORT_NAME);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) out[k++] = idx[i];
    return k;
}

static void card_new(lv_obj_t *wrap, int i, int32_t cwid)
{
    lv_obj_t *c = bl_card(wrap, cwid, 300);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_color(c, AOS_C_CARD2, LV_STATE_PRESSED);
    lv_obj_add_event_cb(c, card_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    S.card[i] = c;
    int32_t iw = cwid - 40;
    S.name[i] = aos_label(c, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_size(S.name[i], iw, lv_font_get_line_height(aos_font_body));
    lv_label_set_long_mode(S.name[i], LV_LABEL_LONG_MODE_DOTS);
    S.fmt[i] = aos_label(c, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_set_size(S.fmt[i], iw, lv_font_get_line_height(aos_font_tiny));
    lv_label_set_long_mode(S.fmt[i], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_y(S.fmt[i], 38);
    S.temp[i] = aos_label(c, "", aos_font_large, AOS_C_TEXT);
    lv_obj_set_y(S.temp[i], 62);
    S.hum[i] = aos_label(c, "", aos_font_title, AOS_C_TEAL);
    lv_obj_align(S.hum[i], LV_ALIGN_TOP_RIGHT, 0, 70);
    S.line[i] = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_size(S.line[i], iw, lv_font_get_line_height(aos_font_caption));
    lv_label_set_long_mode(S.line[i], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_y(S.line[i], 124);
    S.graph[i] = bl_box(c, iw, 90);
    lv_obj_set_y(S.graph[i], 158);
    lv_obj_set_user_data(S.graph[i], (void *)(intptr_t)i);
    lv_obj_add_event_cb(S.graph[i], graph_draw, LV_EVENT_DRAW_MAIN, NULL);
    S.age[i] = aos_label(c, "", aos_font_tiny, AOS_C_DIM);
    lv_obj_align(S.age[i], LV_ALIGN_BOTTOM_LEFT, 0, 0);
    for (uint32_t k = 0; k < lv_obj_get_child_count(c); k++) aos_make_decorative(lv_obj_get_child(c, k));
}

static void card_fill(int i)
{
    const bl_dev_t *d = &BL.dev[S.dev[i]];
    const bl_sensor_t *s = &d->sen;
    lv_label_set_text(S.name[i], bl_dev_name(d));
    char t[160], v[24];
    char a[20];
    bl_fmt_addr(d->addr, a, sizeof a);
    snprintf(t, sizeof t, "%s · %s", s->format ? s->format : "?", a);
    lv_label_set_text(S.fmt[i], t);
    if (s->encrypted) lv_label_set_text(S.temp[i], _("cifrado"));
    else if (s->mask & BL_V_TEMP) {
        bl_fmt_num(v, sizeof v, s->temp, 1);
        snprintf(t, sizeof t, "%s°", v);
        lv_label_set_text(S.temp[i], t);
    } else if (s->mask & BL_V_OPEN) lv_label_set_text(S.temp[i], s->open ? _("abierto") : _("cerrado"));
    else if (s->mask & BL_V_HR) { snprintf(t, sizeof t, "%d lpm", s->hr); lv_label_set_text(S.temp[i], t); }
    else lv_label_set_text(S.temp[i], "");
    if (s->mask & BL_V_HUM) {
        bl_fmt_num(v, sizeof v, s->hum, 0);
        snprintf(t, sizeof t, "%s %%", v);
        lv_label_set_text(S.hum[i], t);
    } else {
        lv_label_set_text(S.hum[i], "");
    }
    /* the line without what is already big */
    bl_sensor_t rest = *s;
    rest.mask &= ~(BL_V_TEMP | BL_V_HUM);
    if (!(s->mask & BL_V_TEMP)) rest.mask &= ~(BL_V_OPEN | BL_V_HR);   /* already big */
    bl_sensor_line(&rest, t, sizeof t);
    lv_label_set_text(S.line[i], s->encrypted ? _("tocá para cargar su clave") : t[0] ? t : " ");
    char age[24];
    bl_fmt_age(age, sizeof age, (uint32_t)aos_hal_uptime_ms() - d->sen_ms);
    snprintf(t, sizeof t, _("%s · %d dBm · 2 h"), age, d->rssi);
    lv_label_set_text(S.age[i], t);
    bool fresh = (uint32_t)aos_hal_uptime_ms() - d->sen_ms < 120000;
    lv_obj_set_style_text_color(S.temp[i], fresh ? AOS_C_TEXT : AOS_C_DIM, 0);
    lv_obj_invalidate(S.graph[i]);
}

static void note_set(void)
{
    if (!S.note) return;
    char t[160];
    const char *csv = BL.log_csv ? _("guardando en CSV") : _("sin CSV");
    const char *mq = !BL.mqtt ? _("sin MQTT") : bl_mqtt_ready() ? _("publicando por MQTT") : _("MQTT sin conectar");
    snprintf(t, sizeof t, "%s · %s · %s", csv, mq, _("se cambia en Ajustes del escaneo"));
    lv_label_set_text(S.note, t);
}

void bl_sens_refresh(void)
{
    if (!S.col) return;
    int now[CARDS_MAX];
    int n = sensors_now(now, CARDS_MAX);
    /* a sensor came or went: the cards again */
    if (n != S.n || memcmp(now, S.dev, sizeof(int) * n)) {
        bl_rebuild();
        return;
    }
    for (int i = 0; i < S.n; i++) card_fill(i);
    note_set();
}

void bl_sens_build(lv_obj_t *page)
{
    memset(&S, 0, sizeof S);
    int32_t w = BL.cw, h = lv_obj_get_height(page);
    S.col = bl_column(page, w, h);
    S.gen = BL.gen;
    S.note = bl_caption(S.col, "", w);
    note_set();
    S.n = sensors_now(S.dev, CARDS_MAX);
    if (!S.n) {
        lv_obj_t *c = bl_vcard(S.col, w, 28, 14);
        aos_label(c, _("Ningún sensor a la vista"), aos_font_body, AOS_C_TEXT);
        bl_caption(c, _("Aparecen solos los que anuncian sus lecturas en un formato conocido: BTHome (Shelly y firmwares abiertos), pvvx y ATC (los termómetros de Xiaomi con firmware cambiado), MiBeacon sin cifrar, Govee, Ruuvi, SwitchBot, Qingping, Inkbird y la telemetría de Eddystone."),
                   w - 56);
        return;
    }
    int cols = BL.land ? 3 : 2;
    int32_t cwid = (w - (cols - 1) * 16) / cols;
    lv_obj_t *wrap = bl_wrap(S.col, w, 16);
    for (int i = 0; i < S.n; i++) {
        card_new(wrap, i, cwid);
        card_fill(i);
    }
}

void bl_sens_gone(void);
void bl_sens_gone(void)
{
    memset(&S, 0, sizeof S);
}
