/*
 * BLE - "Aire": the statistics of what is heard.
 *
 * How busy the air is (packets a second and devices, over two minutes), and
 * who fills it: what kind of things, which companies, which kinds of
 * address (a public one is a fixed identity; the private resolvable ones
 * change every few minutes, which is how phones avoid being followed), which
 * kinds of advertisement, and how the signals spread. Everything is counted
 * over the devices heard in the last 30 seconds.
 */
#include "bl.h"

#include <stdio.h>
#include <string.h>

#define BARS_MAX 10

typedef struct {
    const char *label[BARS_MAX];
    char own[BARS_MAX][40];         /* labels made here (a company with its id) */
    int val[BARS_MAX];
    lv_color_t col[BARS_MAX];
    int n;
} bars_t;

static struct {
    lv_obj_t *col, *kpi[4], *chart, *cls, *co, *addr, *kinds, *hist;
    bars_t b_cls, b_co, b_addr, b_kinds;
    int rhist[15];                  /* -100..-30 in 5 dB bins */
} A;

static void kpi_tile(lv_obj_t *parent, int i, int32_t w, const char *name)
{
    lv_obj_t *c = bl_card(parent, w, 130);
    lv_obj_set_style_pad_all(c, 18, 0);
    A.kpi[i] = aos_label(c, "0", aos_font_large, AOS_C_TEXT);
    lv_obj_t *l = aos_label(c, name, aos_font_caption, AOS_C_DIM);
    lv_obj_align(l, LV_ALIGN_BOTTOM_LEFT, 0, 0);
}

static void chart_draw(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    const bl_air_t *air = &BL.air;
    uint32_t s = (uint32_t)(aos_hal_uptime_ms() / 1000);
    int maxp = 10, maxd = 5;
    for (int k = 1; k < BL_AIR_HIST; k++) {
        uint32_t sec = s - k;
        if (sec > air->sec || air->sec - sec >= BL_AIR_HIST) continue;
        if (air->pkts[sec % BL_AIR_HIST] > maxp) maxp = air->pkts[sec % BL_AIR_HIST];
        if (air->devs[sec % BL_AIR_HIST] > maxd) maxd = air->devs[sec % BL_AIR_HIST];
    }
    int32_t top = a.y1 + 30, h = a.y2 - top, w = lv_area_get_width(&a);
    float bw = (float)w / (BL_AIR_HIST - 1);
    int32_t px = -1, py = 0;
    /* the packets as bars, the devices as a line over them; the current
     * second is still filling, so it is left out */
    for (int k = BL_AIR_HIST - 1; k >= 1; k--) {
        uint32_t sec = s - k;
        int32_t x = a.x1 + (int32_t)((BL_AIR_HIST - 1 - k) * bw);
        if (sec > air->sec || air->sec - sec >= BL_AIR_HIST) { px = -1; continue; }
        int p = air->pkts[sec % BL_AIR_HIST], d = air->devs[sec % BL_AIR_HIST];
        int32_t bh = p * h / maxp;
        if (bh) bl_fill(layer, x, a.y2 - bh, x + (int32_t)bw - 1, a.y2, BL_C, LV_OPA_60, 0);
        int32_t y = a.y2 - d * h / maxd;
        if (px >= 0) bl_draw_line(layer, px, py, x, y, AOS_C_ORANGE, 3, LV_OPA_COVER);
        px = x;
        py = y;
    }
    char t[64];
    snprintf(t, sizeof t, _("paquetes/s (máx. %d)"), maxp);
    bl_draw_text(layer, t, aos_font_tiny, BL_C, a.x1, a.y1, w / 2, LV_TEXT_ALIGN_LEFT);
    snprintf(t, sizeof t, _("equipos/s (máx. %d)"), maxd);
    bl_draw_text(layer, t, aos_font_tiny, AOS_C_ORANGE, a.x1 + w / 2, a.y1, w / 2, LV_TEXT_ALIGN_RIGHT);
}

static void bars_draw(lv_event_t *e)
{
    lv_obj_t *o = lv_event_get_target(e);
    const bars_t *b = lv_obj_get_user_data(o);
    if (!b) return;
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    int32_t w = lv_area_get_width(&a), lw = w * 42 / 100, nw = 70, bw = w - lw - nw - 16;
    int max = 1;
    for (int i = 0; i < b->n; i++) if (b->val[i] > max) max = b->val[i];
    for (int i = 0; i < b->n; i++) {
        int32_t y = a.y1 + i * 44;
        bl_draw_text(layer, b->label[i], aos_font_small, AOS_C_TEXT, a.x1, y + 4, lw - 8, LV_TEXT_ALIGN_LEFT);
        int32_t x = a.x1 + lw;
        bl_fill(layer, x, y + 10, x + bw, y + 32, AOS_C_CARD2, LV_OPA_COVER, 6);
        int32_t f = b->val[i] * bw / max;
        if (f > 0) bl_fill(layer, x, y + 10, x + (f < 12 ? 12 : f), y + 32, b->col[i], LV_OPA_COVER, 6);
        char t[12];
        snprintf(t, sizeof t, "%d", b->val[i]);
        bl_draw_text(layer, t, aos_font_small, AOS_C_DIM, x + bw + 8, y + 4, nw, LV_TEXT_ALIGN_RIGHT);
    }
}

static void hist_draw(lv_event_t *e)
{
    lv_area_t a;
    lv_obj_get_coords(lv_event_get_target(e), &a);
    lv_layer_t *layer = lv_event_get_layer(e);
    int max = 1;
    for (int i = 0; i < 15; i++) if (A.rhist[i] > max) max = A.rhist[i];
    int32_t w = lv_area_get_width(&a), h = lv_area_get_height(&a) - 30;
    float bw = w / 15.0f;
    for (int i = 0; i < 15; i++) {
        int32_t x = a.x1 + (int32_t)(i * bw);
        int32_t bh = A.rhist[i] * h / max;
        if (bh) bl_fill(layer, x + 2, a.y1 + h - bh, x + (int32_t)bw - 2, a.y1 + h, bl_rssi_color(-100 + i * 5 + 2), LV_OPA_COVER, 4);
        if (i % 3 == 0) {
            char t[8];
            snprintf(t, sizeof t, "%d", -100 + i * 5);
            bl_draw_text(layer, t, aos_font_tiny, AOS_C_DIM, x, a.y1 + h + 6, 70, LV_TEXT_ALIGN_LEFT);
        }
    }
}

static lv_obj_t *bars_card(const char *title, const char *hint, bars_t *b, int rows)
{
    int32_t w = BL.cw;
    lv_obj_t *c = bl_vcard(A.col, w, 22, 10);
    aos_label(c, title, aos_font_body, AOS_C_TEXT);
    if (hint) bl_caption(c, hint, w - 44);
    lv_obj_t *o = bl_box(c, w - 44, rows * 44);
    lv_obj_set_user_data(o, b);
    lv_obj_add_event_cb(o, bars_draw, LV_EVENT_DRAW_MAIN, NULL);
    return o;
}

static void bars_add(bars_t *b, const char *label, int v, lv_color_t c)
{
    if (b->n >= BARS_MAX) return;
    b->label[b->n] = label;
    b->val[b->n] = v;
    b->col[b->n] = c;
    b->n++;
}

static void compute(void)
{
    int cls[BL_CLS_COUNT] = { 0 }, ak[4] = { 0 }, kinds[5] = { 0 };
    memset(A.rhist, 0, sizeof A.rhist);
    /* companies: a small tally */
    uint16_t cid[48];
    int ccount[48], ncid = 0, nomfg = 0;
    for (int i = 0; i < BL.ndev; i++) {
        const bl_dev_t *d = &BL.dev[i];
        if (!bl_alive(d)) continue;
        cls[d->cls]++;
        ak[bl_addr_kind(d->addr, d->addr_type)]++;
        for (int k = 0; k < 5; k++) if (d->kinds & (1u << k)) kinds[k]++;
        int r = (int)d->rssi_avg;
        int bin = (r + 100) / 5;
        if (bin < 0) bin = 0;
        if (bin > 14) bin = 14;
        A.rhist[bin]++;
        if (!d->ad.nmfg) { nomfg++; continue; }
        uint16_t c = d->ad.mfg[0].company;
        int j = 0;
        while (j < ncid && cid[j] != c) j++;
        if (j == ncid && ncid < 48) { cid[ncid] = c; ccount[ncid] = 0; ncid++; }
        if (j < ncid) ccount[j]++;
    }
    A.b_cls.n = 0;
    for (int pass = 0; pass < BL_CLS_COUNT && A.b_cls.n < BARS_MAX; pass++) {
        int best = -1;
        for (int c = 0; c < BL_CLS_COUNT; c++) if (cls[c] && (best < 0 || cls[c] > cls[best])) best = c;
        if (best < 0) break;
        bars_add(&A.b_cls, _(bl_class_name((bl_class_t)best)), cls[best], bl_class_color((bl_class_t)best));
        cls[best] = 0;
    }
    A.b_co.n = 0;
    for (int pass = 0; pass < 7; pass++) {
        int best = -1;
        for (int j = 0; j < ncid; j++) if (ccount[j] && (best < 0 || ccount[j] > ccount[best])) best = j;
        if (best < 0) break;
        const char *nm = bl_company_name(cid[best]);
        int k = A.b_co.n;
        if (nm) snprintf(A.b_co.own[k], sizeof A.b_co.own[k], "%.39s", nm);
        else snprintf(A.b_co.own[k], sizeof A.b_co.own[k], "0x%04X", cid[best]);
        bars_add(&A.b_co, A.b_co.own[k], ccount[best], BL_C);
        ccount[best] = 0;
    }
    if (nomfg) bars_add(&A.b_co, _("Sin datos de fabricante"), nomfg, AOS_C_DIM);
    A.b_addr.n = 0;
    static const uint32_t AK_C[4] = { 0x30D158, 0x0A84FF, 0xBF5AF2, 0x8E8E93 };
    for (int k = 0; k < 4; k++) bars_add(&A.b_addr, _(bl_addr_kind_name((bl_addr_kind_t)k)), ak[k], lv_color_hex(AK_C[k]));
    A.b_kinds.n = 0;
    static const char *const KN[5] = { N_("Conectable"), N_("Dirigido"), N_("Escaneable"), N_("No conectable"), N_("Responde al escaneo") };
    for (int k = 0; k < 5; k++) bars_add(&A.b_kinds, _(KN[k]), kinds[k], k == 3 ? AOS_C_TEAL : BL_C);
}

void bl_air_refresh(void)
{
    if (!A.col) return;
    compute();
    int alive = 0;
    for (int i = 0; i < BL.ndev; i++) alive += bl_alive(&BL.dev[i]);
    char t[24];
    snprintf(t, sizeof t, "%d", alive);
    lv_label_set_text(A.kpi[0], t);
    bl_fmt_num(t, sizeof t, BL.air.pps, BL.air.pps < 10 ? 1 : 0);
    lv_label_set_text(A.kpi[1], t);
    snprintf(t, sizeof t, "%d", BL.ndev);
    lv_label_set_text(A.kpi[2], t);
    snprintf(t, sizeof t, "%u", (unsigned)BL.air.lost);
    lv_label_set_text(A.kpi[3], t);
    /* the bar cards grow and shrink with what there is to count */
    lv_obj_t *bo[] = { A.cls, A.co, A.addr, A.kinds };
    const bars_t *bb[] = { &A.b_cls, &A.b_co, &A.b_addr, &A.b_kinds };
    for (int i = 0; i < 4; i++) {
        int32_t hh = (bb[i]->n ? bb[i]->n : 1) * 44;
        if (bo[i] && lv_obj_get_height(bo[i]) != hh) lv_obj_set_height(bo[i], hh);
    }
    lv_obj_t *o[] = { A.chart, A.cls, A.co, A.addr, A.kinds, A.hist };
    for (int i = 0; i < 6; i++) if (o[i]) lv_obj_invalidate(o[i]);
}

void bl_air_build(lv_obj_t *page)
{
    memset(&A, 0, sizeof A);
    int32_t w = BL.cw, h = lv_obj_get_height(page);
    A.col = bl_column(page, w, h);
    if (BL.bt_off) bl_bt_off_card(A.col, w);
    compute();
    lv_obj_t *k = bl_wrap(A.col, w, 12);
    int per = BL.land ? 4 : 2;
    int32_t tw = (w - (per - 1) * 12) / per;
    kpi_tile(k, 0, tw, _("cerca ahora"));
    kpi_tile(k, 1, tw, _("paquetes por segundo"));
    kpi_tile(k, 2, tw, _("vistos desde que abrió"));
    kpi_tile(k, 3, tw, _("paquetes perdidos"));
    lv_obj_t *cc = bl_vcard(A.col, w, 22, 10);
    aos_label(cc, _("Los últimos dos minutos"), aos_font_body, AOS_C_TEXT);
    A.chart = bl_box(cc, w - 44, 220);
    lv_obj_add_event_cb(A.chart, chart_draw, LV_EVENT_DRAW_MAIN, NULL);
    A.cls = bars_card(_("Qué son"), _("Por lo que anuncian: servicios, apariencia, mensajes de Apple, Microsoft y Google, formatos de sensores."), &A.b_cls, 6);
    A.co = bars_card(_("Fabricantes"), _("Por el identificador de compañía de sus datos de fabricante."), &A.b_co, 8);
    A.addr = bars_card(_("Direcciones"), _("Las privadas resolubles cambian cada pocos minutos: así los teléfonos evitan que los sigan. Las públicas y las estáticas son siempre las mismas."), &A.b_addr, 4);
    A.kinds = bars_card(_("Tipos de anuncio"), NULL, &A.b_kinds, 5);
    lv_obj_t *hc = bl_vcard(A.col, w, 22, 10);
    aos_label(hc, _("Señal (dBm)"), aos_font_body, AOS_C_TEXT);
    A.hist = bl_box(hc, w - 44, 200);
    lv_obj_add_event_cb(A.hist, hist_draw, LV_EVENT_DRAW_MAIN, NULL);
    bl_air_refresh();
}

void bl_air_gone(void);
void bl_air_gone(void)
{
    memset(&A, 0, sizeof A);
}
