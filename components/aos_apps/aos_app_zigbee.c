/*
 * P4OS - Zigbee: the C6's 802.15.4 radio as a workshop tool
 * (components/aos_154, docs/plan/ZIGBEE.md, stage 2).
 *
 * Three pages:
 *   Canales    the sixteen channels' energy and traffic, as bars, with
 *              Wi-Fi's channels 1, 6 and 11 drawn over them: where a network
 *              is, and where to put a new one
 *   Escuchar   every frame heard on one channel, decoded (802.15.4, and on
 *              top Zigbee's network layer or Thread's 6LoWPAN), and a
 *              capture to the card for Wireshark
 *   Equipos    every device heard while listening: its addresses, how much
 *              it talks and how well it is heard
 *   Mi red     the board's own Zigbee network (the stack on the P4, the C6
 *              its radio: aos_zigbee.h): start it, let devices join, and
 *              each one with what it is and what it says, named where the
 *              board knows the cluster and raw where it does not, so any
 *              device can be tried
 *
 * In the first three nothing is sent on the air: the radio only listens, and
 * leaving the app stops it. The network of the fourth stays up when the app
 * closes (until the board restarts). A C6 with an older firmware does not answer, and the app says how to
 * update it.
 */
#include "aos_apps.h"
#include "aos_154.h"
#include "aos_154_proto.h"
#include "aos_zigbee.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_sys_glyphs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define C_ZB        lv_color_hex(0xEB0443)
#define C_WIFI      lv_color_hex(0x0A84FF)
#define ROWS_MAX    60                  /* frames shown in the list */
#define DB_MIN      (-100)
#define DB_MAX      (-35)

enum { P_CHANNELS = 0, P_SNIFF, P_DEVICES, P_NET, P_COUNT };
#define ZB_CHANNEL  26                  /* the board's network: away from Wi-Fi's 1, 6 and 11 */

static struct {
    lv_obj_t *root, *col, *sub, *warn, *page, *tabs[P_COUNT], *devs, *devs_note;
    lv_obj_t *bars[16], *bar_lbl[16], *scan_btn, *scan_lbl, *scan_note;
    lv_obj_t *ch_chips[16], *rec_sw, *go_btn, *go_lbl, *counts, *list;
    lv_timer_t *timer;
    int32_t W, H;
    int page_no;
    int chan;                           /* the channel to listen on */
    bool pcap;
    uint32_t seen_scan, from;
    uint32_t start;                     /* the first frame of this listening */
    uint32_t t_open;
    int rows;
    bool deep;                          /* a scan of 2 s a channel, not 250 ms */
    uint32_t devs_tick;
    lv_obj_t *net, *net_note, *net_btn, *net_lbl, *close_btn;
    volatile bool starting;             /* the network being started, off LVGL's task */
    bool keep_open;                     /* "Sumar equipos": open again each time it closes, until Cerrar */
    uint32_t asked_ms;
    uint8_t rm_ieee[AOS_ZB_DEVICES_MAX][8];     /* each card's device, for its Sacar */
    uint8_t armed[8];                   /* the one whose Sacar was tapped once */
    uint32_t armed_ms;
    struct { uint8_t ieee[8], ep, state; } out[24];     /* each output chip on the page; state: what a tap sends */
    int nout;
    uint8_t pend_ieee[8], pend_ep;      /* the output just tapped, "..." until it answers */
    uint32_t pend_ms;
    struct { uint8_t ieee[8], ep, kind; } lvl[12];      /* each slider on the page: SL_* */
    struct { uint8_t ieee[8], ep, kind; int32_t arg; } ctl[40];     /* each control button: B_* */
    int nctl;
    int nlvl;
    bool dragging;                      /* a slider held: the page is not redrawn under the finger */
} U;

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

static lv_obj_t *wrap(lv_obj_t *parent, const char *text, const lv_font_t *f, lv_color_t c, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, f, c);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, bool strong, lv_event_cb_t cb, void *ud, lv_obj_t **lbl)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_pad_hor(b, 28, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(b, strong ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    if (lbl) *lbl = l;
    return b;
}

/* -------------------------------------------------------------------------- */
/* Decoding a frame into two lines                                             */
/* -------------------------------------------------------------------------- */

static int addr(const uint8_t *f, int at, int mode, char *out, size_t n)
{
    if (mode == 2) {
        snprintf(out, n, "0x%04X", f[at] | f[at + 1] << 8);
        return at + 2;
    }
    if (mode == 3) {
        snprintf(out, n, "%02X%02X:%02X%02X", f[at + 7], f[at + 6], f[at + 1], f[at]);
        return at + 8;
    }
    snprintf(out, n, "-");
    return at;
}

static const char *mac_cmd(uint8_t c)
{
    switch (c) {
    case 1: return _("pedido de asociación");
    case 2: return _("respuesta de asociación");
    case 3: return _("aviso de desasociación");
    case 4: return _("pedido de datos");
    case 7: return _("pedido de beacon");
    case 8: return _("realineación");
    default: return _("comando");
    }
}

/* line 1: what it is and between whom; line 2: what rides on it */
static void decode(const aos_154_frame_t *fr, char *l1, size_t n1, char *l2, size_t n2)
{
    const uint8_t *f = fr->data;
    int len = fr->len - 2;              /* without the FCS */
    l2[0] = '\0';
    if (len < 3) { snprintf(l1, n1, _("trama corta (%d bytes)"), fr->len); return; }
    uint16_t fcf = f[0] | f[1] << 8;
    int type = fcf & 7, sec = fcf >> 3 & 1, comp = fcf >> 6 & 1;
    int dm = fcf >> 10 & 3, sm = fcf >> 14 & 3;
    int at = 3;
    char dpan[8] = "", da[24], sa[24];
    if (dm) {
        if (at + 2 > len) goto short_;
        snprintf(dpan, sizeof dpan, "%04X", f[at] | f[at + 1] << 8);
        at += 2;
        at = addr(f, at, dm, da, sizeof da);
    } else snprintf(da, sizeof da, "-");
    if (sm) {
        if (!comp) at += 2;
        if (at + (sm == 3 ? 8 : 2) > len) goto short_;
        at = addr(f, at, sm, sa, sizeof sa);
    } else snprintf(sa, sizeof sa, "-");
    switch (type) {
    case 0:
        snprintf(l1, n1, "Beacon  %s", sa);
        if (len - at >= 4) {
            const uint8_t *p = f + at + 4;          /* after the superframe, GTS and pending fields, roughly */
            int left = len - at - 4;
            if (left >= 2 && p[0] == 0x00 && (p[1] & 0x0f) == 2)
                snprintf(l2, n2, "Zigbee PRO, %s", p[2] & 0x80 ? _("acepta equipos nuevos") : _("cerrada"));
            else if (left >= 1 && p[0] == 0x03) snprintf(l2, n2, "Thread");
        }
        return;
    case 2:
        snprintf(l1, n1, "%s  #%u", _("Acuse"), f[2]);
        return;
    case 3:
        snprintf(l1, n1, "%s  %s → %s", _("Comando"), sa, da);
        if (at < len) snprintf(l2, n2, "%s", mac_cmd(f[at]));
        return;
    case 1:
        snprintf(l1, n1, "%s  %s → %s%s%s", _("Datos"), sa, da, dpan[0] ? "  PAN " : "", dpan);
        if (sec) { snprintf(l2, n2, _("cifrada en la capa MAC")); return; }
        if (at + 8 <= len && ((f[at] >> 2) & 0x0f) == 2 && (f[at] & 3) <= 1) {
            uint16_t nf = f[at] | f[at + 1] << 8;
            uint16_t nd = f[at + 2] | f[at + 3] << 8, ns = f[at + 4] | f[at + 5] << 8;
            snprintf(l2, n2, "Zigbee %s  0x%04X → 0x%04X  %s %u%s%s", (nf & 3) ? _("comando de red") : _("datos"),
                     ns, nd, _("radio"), f[at + 6], nf >> 9 & 1 ? ", " : "", nf >> 9 & 1 ? _("cifrada") : "");
        } else if (at < len && (f[at] & 0xe0) == 0x60) snprintf(l2, n2, _("6LoWPAN (Thread)"));
        else if (at < len && (f[at] & 0xc0) == 0x80) snprintf(l2, n2, _("6LoWPAN, malla"));
        return;
    default:
        snprintf(l1, n1, _("trama de tipo %d"), type);
        return;
    }
short_:
    snprintf(l1, n1, _("trama de %d bytes, cortada"), fr->len);
}

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void build(void);

static void tab_cb(lv_event_t *e)
{
    U.page_no = (int)(intptr_t)lv_event_get_user_data(e);
    build();
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    aos_154_info_t in;
    aos_154_info(&in);
    if (in.state == AOS_154_SNIFFING) aos_154_stop();
    if (in.guarded) aos_154_unguard();  /* a tap is the person asking to try again */
    if (!aos_154_scan(AOS_154_CH_FIRST, AOS_154_CH_LAST, U.deep ? 2000 : 250))
        aos_ui_toast(_("El C6 no tomó el pedido"), 2500);
}

static void deep_cb(lv_event_t *e)
{
    U.deep = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

/* WiFi channel w (1..13) over the 802.15.4 channels: its centre in MHz */
static int wifi_mhz(int w) { return 2407 + 5 * w; }
static int zb_mhz(int c) { return 2405 + 5 * (c - 11); }

static void build_channels(lv_obj_t *page, int32_t w)
{
    lv_obj_t *c = card(page, w, U.H > U.W ? 520 : 400);
    lv_obj_set_style_pad_all(c, 20, 0);
    int32_t iw = w - 40, ih = (U.H > U.W ? 520 : 400) - 40;
    int32_t bottom = ih - 40, top = 40, colw = iw / 16;
    /* Wi-Fi 1, 6 and 11, each 22 MHz wide, as bands behind the bars */
    for (int k = 0; k < 3; k++) {
        int wc = k == 0 ? 1 : k == 1 ? 6 : 11;
        float x0 = (float)(wifi_mhz(wc) - 11 - zb_mhz(11) + 2.5f) / 5.0f, x1 = x0 + 22.0f / 5.0f;
        lv_obj_t *band = box(c, (int32_t)((x1 - x0) * colw), bottom - top + 10);
        lv_obj_set_pos(band, (int32_t)(x0 * colw), top - 10);
        lv_obj_set_style_bg_color(band, C_WIFI, 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_10, 0);
        lv_obj_set_style_radius(band, 8, 0);
        char t[16];
        snprintf(t, sizeof t, "Wi-Fi %d", wc);
        lv_obj_t *bl = aos_label(c, t, aos_font_tiny, C_WIFI);
        lv_obj_set_pos(bl, (int32_t)(x0 * colw) + 6, top - 36);
    }
    for (int i = 0; i < 16; i++) {
        U.bars[i] = box(c, colw - 10, 4);
        lv_obj_set_style_bg_opa(U.bars[i], LV_OPA_COVER, 0);
        lv_obj_set_style_radius(U.bars[i], 6, 0);
        lv_obj_set_style_bg_color(U.bars[i], AOS_C_CARD2, 0);
        lv_obj_set_pos(U.bars[i], i * colw + 5, bottom - 4);
        U.bar_lbl[i] = aos_label(c, "", aos_font_tiny, AOS_C_TEXT);
        lv_obj_set_width(U.bar_lbl[i], colw);
        lv_obj_set_style_text_align(U.bar_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(U.bar_lbl[i], i * colw, bottom - 30);
        char n[4];
        snprintf(n, sizeof n, "%d", 11 + i);
        lv_obj_t *cl = aos_label(c, n, aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(cl, colw);
        lv_obj_set_style_text_align(cl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(cl, i * colw, bottom + 8);
    }
    lv_obj_t *row = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);
    U.scan_btn = pill(row, _("Escanear"), true, scan_cb, NULL, &U.scan_lbl);
    lv_obj_t *sw = lv_switch_create(row);
    if (U.deep) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, deep_cb, LV_EVENT_VALUE_CHANGED, NULL);
    aos_label(row, _("A fondo (2 s por canal)"), aos_font_small, AOS_C_TEXT);
    U.scan_note = wrap(page, "", aos_font_caption, AOS_C_DIM, w);
    wrap(page, _("La barra es la energía que hay en el canal; el número, los paquetes que se oyeron mientras se escuchaba. "
                 "Una red Zigbee o Thread se ve como un canal con paquetes. Para una red nueva conviene uno sin paquetes y lejos "
                 "del Wi-Fi: Zigbee suele usar el 11, 15, 20 o 25."),
         aos_font_caption, AOS_C_DIM, w);
}

static void chan_cb(lv_event_t *e)
{
    U.chan = (int)(intptr_t)lv_event_get_user_data(e);
    aos_154_info_t in;
    aos_154_info(&in);
    if (in.state == AOS_154_SNIFFING) aos_154_sniff(U.chan, U.pcap);
    build();
}

static void rec_cb(lv_event_t *e)
{
    U.pcap = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    aos_154_info_t in;
    aos_154_info(&in);
    if (in.state == AOS_154_SNIFFING) {
        aos_154_stop();
        return;
    }
    U.start = in.last_seq + 1;          /* the count starts over */
    if (in.guarded) aos_154_unguard();  /* a tap is the person asking to try again */
    if (!aos_154_sniff(U.chan, U.pcap)) aos_ui_toast(_("El C6 no tomó el pedido"), 2500);
}

static void build_sniff(lv_obj_t *page, int32_t w)
{
    lv_obj_t *chips = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(chips, 10, 0);
    for (int i = 0; i < 16; i++) {
        lv_obj_t *b = box(chips, 76, 56);
        lv_obj_set_style_radius(b, 28, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(b, 11 + i == U.chan ? AOS_C_ACCENT : AOS_C_CARD, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, chan_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(11 + i));
        char n[4];
        snprintf(n, sizeof n, "%d", 11 + i);
        lv_obj_center(aos_label(b, n, aos_font_small, AOS_C_TEXT));
        U.ch_chips[i] = b;
    }
    lv_obj_t *row = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);
    U.go_btn = pill(row, _("Escuchar"), true, go_cb, NULL, &U.go_lbl);
    U.rec_sw = lv_switch_create(row);
    if (U.pcap) lv_obj_add_state(U.rec_sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(U.rec_sw, rec_cb, LV_EVENT_VALUE_CHANGED, NULL);
    aos_label(row, _("Guardar para Wireshark"), aos_font_small, AOS_C_TEXT);
    U.counts = wrap(page, "", aos_font_caption, AOS_C_DIM, w);
    U.list = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.list, 6, 0);
    U.rows = 0;
    U.from = 0;                         /* the list starts over with what comes next */
    aos_154_info_t in;
    aos_154_info(&in);
    U.from = in.last_seq + 1;
    if (!U.start) U.start = U.from;
}

static void add_row(const aos_154_frame_t *fr, int32_t w)
{
    char l1[96], l2[96];
    decode(fr, l1, sizeof l1, l2, sizeof l2);
    lv_obj_t *r = card(U.list, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(r, 18, 0);
    lv_obj_set_style_pad_ver(r, 10, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_t *top = box(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_t *a = aos_label(top, l1, aos_font_caption, AOS_C_TEXT);
    lv_obj_set_width(a, w - 36 - 150);
    lv_label_set_long_mode(a, LV_LABEL_LONG_MODE_DOTS);
    char rs[24];
    snprintf(rs, sizeof rs, "%d dBm · %u B", fr->rssi, fr->len);
    lv_obj_t *b = aos_label(top, rs, aos_font_tiny, AOS_C_DIM);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, 0, 2);
    if (l2[0]) {
        lv_obj_t *c = aos_label(r, l2, aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(c, w - 36);
        lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    }
    lv_obj_move_to_index(r, 0);         /* the newest on top */
    if (++U.rows > ROWS_MAX) {
        lv_obj_delete(lv_obj_get_child(U.list, -1));
        U.rows--;
    }
}

static void clear_cb(lv_event_t *e)
{
    (void)e;
    aos_154_devices_clear();
    U.devs_tick = 0;
}

static void build_devices(lv_obj_t *page, int32_t w)
{
    lv_obj_t *row = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);
    pill(row, _("Olvidarlos"), false, clear_cb, NULL, NULL);
    U.devs_note = wrap(page, "", aos_font_caption, AOS_C_DIM, w);
    U.devs = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.devs, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.devs, 6, 0);
    U.devs_tick = 0;
}

static void fill_devices(int32_t w)
{
    static aos_154_device_t d[AOS_154_DEVICES];
    int n = aos_154_devices(d, AOS_154_DEVICES);
    lv_obj_clean(U.devs);
    lv_label_set_text(U.devs_note, n ? _("Lo que se oyó mientras se escuchaba, el más reciente arriba. La dirección larga "
                                         "de una corta aparece cuando la red la manda.")
                                     : _("Todavía nadie: escuchá un canal con tráfico."));
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = 0; i < n; i++) {
        char a[96], b[120], ext[24] = "";
        if (d[i].has_ext)
            snprintf(ext, sizeof ext, "%02X%02X:%02X%02X:%02X%02X:%02X%02X", d[i].ext[0], d[i].ext[1], d[i].ext[2], d[i].ext[3],
                     d[i].ext[4], d[i].ext[5], d[i].ext[6], d[i].ext[7]);
        if (d[i].short_addr != 0xffff)
            snprintf(a, sizeof a, "0x%04X%s%s", d[i].short_addr, d[i].short_addr == 0 ? "  " : "",
                     d[i].short_addr == 0 ? _("coordinador") : "");
        else snprintf(a, sizeof a, "%s", ext);
        if (d[i].vendor[0]) {
            size_t l = strlen(a);
            snprintf(a + l, sizeof a - l, "  %s", d[i].vendor);
        }
        uint32_t ago = (now - d[i].last_ms) / 1000;
        int k = snprintf(b, sizeof b, _("%u paquetes · %d dBm, promedio %d · hace %u s"), (unsigned)d[i].frames,
                         d[i].rssi_last, d[i].rssi_avg, (unsigned)ago);
        if (d[i].short_addr != 0xffff && ext[0]) snprintf(b + k, sizeof b - (size_t)k, " · %s", ext);
        lv_obj_t *r = card(U.devs, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(r, 18, 0);
        lv_obj_set_style_pad_ver(r, 10, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_t *top = box(r, lv_pct(100), LV_SIZE_CONTENT);
        aos_label(top, a, aos_font_caption, AOS_C_TEXT);
        char pc[24];
        if (d[i].pan != 0xffff) snprintf(pc, sizeof pc, "PAN %04X · %u", d[i].pan, d[i].channel);
        else snprintf(pc, sizeof pc, "%s %u", _("canal"), d[i].channel);
        lv_obj_align(aos_label(top, pc, aos_font_tiny, AOS_C_DIM), LV_ALIGN_TOP_RIGHT, 0, 2);
        lv_obj_t *c = aos_label(r, b, aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(c, w - 36);
        lv_label_set_long_mode(c, LV_LABEL_LONG_MODE_DOTS);
    }
}

/* -------------------------------------------------------------------------- */
/* Mi red: the board's own network                                             */
/* -------------------------------------------------------------------------- */

/* starting it waits for the C6 (seconds): not in LVGL's task */
static void start_task(void *arg)
{
    (void)arg;
    if (!aos_zigbee_start(ZB_CHANNEL)) aos_ui_request_toast(_("No se pudo armar la red: ¿el C6 tiene su firmware 154.15?"));
    U.starting = false;
}

static void net_cb(lv_event_t *e)
{
    (void)e;
    aos_zigbee_info_t z;
    aos_zigbee_info(&z);
    if (z.state == AOS_ZB_FORMED) {
        /* open until Cerrar or until the app closes: a device's reset is
         * not always on the first try, and 254 s (the most one opening
         * lasts) ran out in the middle of pairing several */
        U.keep_open = true;
        U.asked_ms = (uint32_t)aos_hal_uptime_ms();
        if (!aos_zigbee_permit(254)) aos_ui_toast(_("La red no tomó el pedido"), 2000);
    } else if (!U.starting && z.state != AOS_ZB_STARTING) {
        aos_154_info_t in;
        aos_154_info(&in);
        if (in.state == AOS_154_SCANNING || in.state == AOS_154_SNIFFING) aos_154_stop();
        U.starting = true;
        if (!aos_hal_thread_start("zb_start", start_task, NULL, 4096, 4)) U.starting = false;
    }
}

static void close_cb(lv_event_t *e)
{
    (void)e;
    U.keep_open = false;
    aos_zigbee_permit(0);
}

static void build_net(lv_obj_t *page, int32_t w)
{
    lv_obj_t *row = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 20, 0);
    U.net_btn = pill(row, "", true, net_cb, NULL, &U.net_lbl);
    U.close_btn = pill(row, _("Cerrar"), false, close_cb, NULL, NULL);
    U.net_note = wrap(page, "", aos_font_caption, AOS_C_DIM, w);
    U.net = box(page, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.net, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.net, 10, 0);
    U.devs_tick = 0;
}

/* Sacar: a first tap asks, a second one within 4 s does it */
static void rm_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (!memcmp(U.armed, U.rm_ieee[k], 8) && now - U.armed_ms < 4000) {
        if (!aos_zigbee_remove(U.rm_ieee[k])) aos_ui_toast(_("La red no tomó el pedido"), 2000);
        memset(U.armed, 0, sizeof U.armed);
    } else {
        memcpy(U.armed, U.rm_ieee[k], 8);
        U.armed_ms = now;
    }
    U.devs_tick = 0;                    /* redrawn at once */
}

static const char *zone_name(uint16_t t)
{
    switch (t) {
    case 0x0015: return _("contacto");
    case 0x000d: return _("movimiento");
    case 0x002a: return _("agua");
    case 0x0028: return _("humo");
    case 0x002b: return _("gas");
    default:     return _("alarma");
    }
}

static const char *zone_state(uint16_t t, bool alarm)
{
    switch (t) {
    case 0x0015: return alarm ? _("abierto") : _("cerrado");
    case 0x000d: return alarm ? _("movimiento") : _("sin movimiento");
    case 0x002a: return alarm ? _("¡agua!") : _("seco");
    default:     return alarm ? _("alarma") : _("normal");
    }
}

/* the names of the clusters most devices have, for the raw lines */
static const char *cluster_name(uint16_t c)
{
    switch (c) {
    case 0x0001: return _("batería");
    case 0x0006: return _("encendido");
    case 0x0008: return _("nivel");
    case 0x0300: return _("color");
    case 0x0402: return _("temperatura");
    case 0x0403: return _("presión");
    case 0x0405: return _("humedad");
    case 0x0406: return _("presencia");
    case 0x0400: return _("luz");
    case 0x0500: return "IAS";
    case 0x0702: return _("medidor");
    case 0x0b04: return _("eléctrico");
    case 0xef00: return "Tuya";
    default:     return NULL;
    }
}

/* an output's chip, tapped: toggled (the On/Off cluster's Toggle, which any
 * switch takes without the board knowing its state first) */
static void out_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= U.nout) return;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    /* a second tap while the first goes is not another command */
    if (!memcmp(U.pend_ieee, U.out[k].ieee, 8) && U.pend_ep == U.out[k].ep && now - U.pend_ms < 1500) return;
    /* On or Off, from the state the chip shows; Toggle only for one not
     * known yet: two Toggles that cross undo each other (on the board,
     * 2026-11-24, a relay that went on and would not go off) */
    if (!aos_zigbee_onoff(U.out[k].ieee, U.out[k].ep, U.out[k].state)) {
        aos_ui_toast(_("La red no tomó el pedido"), 2000);
        return;
    }
    memcpy(U.pend_ieee, U.out[k].ieee, 8);
    U.pend_ep = U.out[k].ep;
    U.pend_ms = (uint32_t)aos_hal_uptime_ms();
    U.devs_tick = 0;
}

/* A slider: held, the page waits; let go, the value is sent. A dimmer's
 * level (Move to Level with On/Off: 0 turns it off), a curtain's position
 * (Go To Lift Percentage), a light's color temperature (Move to Color
 * Temperature, in mireds) */
enum { SL_LEVEL = 0, SL_COVER, SL_CT };

static void lvl_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_PRESSED) U.dragging = true;
    else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
        U.dragging = false;
        if (k < 0 || k >= U.nlvl) return;
        int v = lv_slider_get_value(lv_event_get_target(e));
        bool ok;
        if (U.lvl[k].kind == SL_COVER) {
            uint8_t pct = (uint8_t)v;
            ok = aos_zigbee_command(U.lvl[k].ieee, U.lvl[k].ep, 0x0102, 0x05, &pct, 1, 0x0008);
        } else if (U.lvl[k].kind == SL_CT) {
            uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), 5, 0 };     /* mireds, 0.5 s */
            ok = aos_zigbee_command(U.lvl[k].ieee, U.lvl[k].ep, 0x0300, 0x0A, b, 4, 0x0007);
        } else ok = aos_zigbee_level(U.lvl[k].ieee, U.lvl[k].ep, v);
        if (!ok) aos_ui_toast(_("La red no tomó el pedido"), 2000);
        U.devs_tick = 1;                /* redrawn in a moment, with its answer */
    }
}

static void add_slider(lv_obj_t *card, int32_t w, const uint8_t ieee[8], uint8_t ep, int kind, int min, int max, int val,
                       lv_color_t c)
{
    if (U.nlvl >= (int)(sizeof U.lvl / sizeof U.lvl[0])) return;
    lv_obj_t *sl = lv_slider_create(card);
    lv_obj_set_width(sl, w - 36 - 40);
    lv_obj_set_style_margin_hor(sl, 20, 0);
    lv_obj_set_style_margin_ver(sl, 14, 0);
    lv_slider_set_range(sl, min, max);
    lv_slider_set_value(sl, val < min ? min : val > max ? max : val, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(sl, c, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sl, AOS_C_TEXT, LV_PART_KNOB);
    memcpy(U.lvl[U.nlvl].ieee, ieee, 8);
    U.lvl[U.nlvl].ep = ep;
    U.lvl[U.nlvl].kind = (uint8_t)kind;
    lv_obj_add_event_cb(sl, lvl_cb, LV_EVENT_PRESSED, (void *)(intptr_t)U.nlvl);
    lv_obj_add_event_cb(sl, lvl_cb, LV_EVENT_RELEASED, (void *)(intptr_t)U.nlvl);
    lv_obj_add_event_cb(sl, lvl_cb, LV_EVENT_PRESS_LOST, (void *)(intptr_t)U.nlvl);
    U.nlvl++;
}

/* A control's button: a curtain's up, stop and down; a thermostat's setpoint
 * half a degree either way; a lock; a fan's mode, the next one; a Tuya
 * datapoint (a bool, flipped); the IR blaster's learning and sending */
enum { B_COVER = 0, B_SETPOINT, B_LOCK, B_FAN, B_DP, B_IR_LEARN, B_IR_SEND };

static void ctl_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k < 0 || k >= U.nctl) return;
    const uint8_t *ie = U.ctl[k].ieee;
    uint8_t ep = U.ctl[k].ep;
    int32_t a = U.ctl[k].arg;
    bool ok = false;
    switch (U.ctl[k].kind) {
    case B_COVER: ok = aos_zigbee_command(ie, ep, 0x0102, (uint8_t)a, NULL, 0, 0x0008); break;
    case B_SETPOINT: {
        int16_t sp = (int16_t)a;
        uint8_t b[2] = { (uint8_t)sp, (uint8_t)(sp >> 8) };
        ok = aos_zigbee_write(ie, ep, 0x0201, 0x0012, 0x29, b, 2);
        break;
    }
    case B_LOCK: {
        uint8_t pin = 0;                /* an empty code: those that need none take it */
        ok = aos_zigbee_command(ie, ep, 0x0101, (uint8_t)a, &pin, 1, 0x0000);
        break;
    }
    case B_FAN: {
        uint8_t m = (uint8_t)a;
        ok = aos_zigbee_write(ie, ep, 0x0202, 0x0000, 0x30, &m, 1);
        break;
    }
    case B_DP: ok = aos_zigbee_tuya_dp(ie, (uint8_t)ep, 1, a); break;
    case B_IR_LEARN: ok = aos_zigbee_ir_learn(ie, a != 0); break;
    case B_IR_SEND: {
        static char code[3000];
        uint8_t who[8];
        aos_zigbee_ir_state(who, code, sizeof code);
        ok = code[0] && aos_zigbee_ir_send(ie, code);
        break;
    }
    }
    if (!ok) aos_ui_toast(_("La red no tomó el pedido"), 2000);
    U.devs_tick = 1;
}

static void add_ctl(lv_obj_t *row, const char *text, const uint8_t ieee[8], uint8_t ep, int kind, int32_t arg, bool strong)
{
    if (U.nctl >= (int)(sizeof U.ctl / sizeof U.ctl[0])) return;
    memcpy(U.ctl[U.nctl].ieee, ieee, 8);
    U.ctl[U.nctl].ep = ep;
    U.ctl[U.nctl].kind = (uint8_t)kind;
    U.ctl[U.nctl].arg = arg;
    lv_obj_t *b = pill(row, text, strong, ctl_cb, (void *)(intptr_t)U.nctl, NULL);
    lv_obj_set_height(b, 52);
    U.nctl++;
}

/* a measured value of a device, by cluster and attribute (or def) */
static int32_t meas_of(const aos_zigbee_dev_t *d, uint16_t cl, uint16_t at, int32_t def)
{
    for (int i = 0; i < d->nmeas; i++)
        if (d->meas[i].cluster == cl && d->meas[i].attr == at) return d->meas[i].v;
    return def;
}

/* what the board already shows by name, left out of the raw lines */
static bool named_attr(const aos_zigbee_attr_t *a)
{
    if (a->type == 0xFF) return a->cluster == 0x0500 && a->attr == 0x00;   /* IAS zone status change */
    if (aos_zigbee_meas_known(a->cluster, a->attr)) return true;            /* the table's, shown by name */
    switch (a->cluster) {
    case 0x0001: return a->attr == 0x0020 || a->attr == 0x0021;
    case 0x0006: case 0x0008: case 0x0402: case 0x0405: case 0x0406: return a->attr == 0x0000;
    case 0x0500: return a->attr == 0x0001 || a->attr == 0x0002;
    default: return false;
    }
}

static lv_obj_t *add_value(lv_obj_t *row, const char *text, lv_color_t c)
{
    lv_obj_t *v = box(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(v, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(v, AOS_C_CARD2, 0);
    lv_obj_set_style_radius(v, 16, 0);
    lv_obj_set_style_pad_hor(v, 14, 0);
    lv_obj_set_style_pad_ver(v, 6, 0);
    aos_label(v, text, aos_font_small, c);
    return v;
}

static void fill_net(int32_t w)
{
    aos_zigbee_info_t z;
    aos_zigbee_info(&z);
    int open = aos_zigbee_permit_left();
    uint32_t t = (uint32_t)aos_hal_uptime_ms();
    if (U.keep_open && z.state == AOS_ZB_FORMED && open < 5 && t - U.asked_ms > 5000) {
        U.asked_ms = t;
        aos_zigbee_permit(254);
    }
    if (U.starting || z.state == AOS_ZB_STARTING) lv_label_set_text(U.net_lbl, _("Armando la red..."));
    else if (z.state != AOS_ZB_FORMED) lv_label_set_text(U.net_lbl, _("Armar la red"));
    else if (U.keep_open) lv_label_set_text(U.net_lbl, _("Abierta para sumar"));
    else if (open) lv_label_set_text_fmt(U.net_lbl, _("Abierta: %d s"), open);
    else lv_label_set_text(U.net_lbl, _("Sumar equipos"));
    if (open || U.keep_open) lv_obj_remove_flag(U.close_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.close_btn, LV_OBJ_FLAG_HIDDEN);

    static aos_zigbee_dev_t d[AOS_ZB_DEVICES_MAX];
    int n = aos_zigbee_devices(d, AOS_ZB_DEVICES_MAX);
    if (z.state != AOS_ZB_FORMED)
        lv_label_set_text(U.net_note, _("La placa arma su propia red Zigbee, con el C6 de radio, y los equipos se suman a "
                                        "ella: así se prueba cualquiera. Un equipo está en una sola red: para sumarlo acá "
                                        "hay que resetearlo (en general, el botón apretado hasta que parpadea)."));
    else if (open || U.keep_open)
        lv_label_set_text(U.net_note, _("La red queda abierta mientras estés acá, hasta Cerrar. Reseteá los equipos de a "
                                        "uno: la pantalla avisa cuando entra cada uno y qué es. Si es a pila, tocale el "
                                        "botón cada tanto mientras se configura."));
    else
        lv_label_set_text_fmt(U.net_note, _("Red en el canal %u. %d equipos. Los valores llegan cuando el equipo los manda: "
                                            "uno a pila puede tardar en dar noticias (tocale el botón para despertarlo). "
                                            "Una salida se prende o se apaga tocándola."),
                              z.channel, n);
    lv_obj_clean(U.net);
    U.nout = 0;
    U.nlvl = 0;
    U.nctl = 0;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = n - 1; i >= 0; i--) {
        const aos_zigbee_dev_t *e = &d[i];
        lv_obj_t *r = card(U.net, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(r, 18, 0);
        lv_obj_set_style_pad_ver(r, 12, 0);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(r, 6, 0);
        char a[96], b[160];
        if (e->manuf[0] || e->model[0]) snprintf(a, sizeof a, "%s %s", e->manuf, e->model);
        else snprintf(a, sizeof a, "%02X%02X:%02X%02X:%02X%02X:%02X%02X", e->ieee[7], e->ieee[6], e->ieee[5], e->ieee[4],
                      e->ieee[3], e->ieee[2], e->ieee[1], e->ieee[0]);
        bool news = e->heard_ms != 0;
        lv_obj_t *top = box(r, w - 36, LV_SIZE_CONTENT);
        lv_obj_t *t = aos_label(top, a, aos_font_small, e->step == AOS_ZB_DEV_LEFT || !news ? AOS_C_DIM : AOS_C_TEXT);
        lv_obj_set_width(t, w - 36 - 170);
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        memcpy(U.rm_ieee[i], e->ieee, 8);
        bool armed = !memcmp(U.armed, e->ieee, 8) && now - U.armed_ms < 4000;
        lv_obj_t *rm = pill(top, armed ? _("¿Seguro?") : _("Sacar"), armed, rm_cb, (void *)(intptr_t)i, NULL);
        lv_obj_set_height(rm, 48);
        lv_obj_align(rm, LV_ALIGN_TOP_RIGHT, 0, 0);
        const char *kind = (e->cap & 0x02) ? _("router") : (e->power == 1 ? _("a red eléctrica") : _("a pila"));
        int k = snprintf(b, sizeof b, "0x%04X · %s", e->nwk, kind);
        if (e->rssi) k += snprintf(b + k, sizeof b - (size_t)k, " · %d dBm", e->rssi);
        k += snprintf(b + k, sizeof b - (size_t)k, " · ");
        if (!news) k += snprintf(b + k, sizeof b - (size_t)k, "%s", _("sin noticias desde que arrancó la placa"));
        else {
            uint32_t ago = (now - e->heard_ms) / 1000;
            if (ago < 120) k += snprintf(b + k, sizeof b - (size_t)k, _("hace %u s"), (unsigned)ago);
            else k += snprintf(b + k, sizeof b - (size_t)k, _("hace %u min"), (unsigned)(ago / 60));
        }
        if (e->step == AOS_ZB_DEV_LEFT) snprintf(b + k, sizeof b - (size_t)k, " · %s", _("se fue"));
        else if (e->step == AOS_ZB_DEV_FAILED) snprintf(b + k, sizeof b - (size_t)k, " · %s", _("no contestó"));
        else if (e->setup) snprintf(b + k, sizeof b - (size_t)k, " · %s", _("configurándolo"));
        lv_obj_t *s2 = aos_label(r, b, aos_font_tiny, AOS_C_DIM);
        lv_obj_set_width(s2, w - 36);
        lv_label_set_long_mode(s2, LV_LABEL_LONG_MODE_DOTS);

        /* what the board names */
        lv_obj_t *vals = box(r, w - 36, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(vals, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(vals, 8, 0);
        lv_obj_set_style_pad_row(vals, 8, 0);
        char v[112];
        if (e->temp != INT16_MIN) {
            snprintf(v, sizeof v, "%d.%d °C", e->temp / 100, abs(e->temp % 100) / 10);
            add_value(vals, v, AOS_C_ORANGE);
        }
        if (e->hum != 0xFFFF) {
            snprintf(v, sizeof v, "%u.%u %%", e->hum / 100, (e->hum % 100) / 10);
            add_value(vals, v, AOS_C_TEAL);
        }
        if (e->zone_known) {
            bool al = e->zone_status & 1;
            snprintf(v, sizeof v, "%s: %s", zone_name(e->zone_type), zone_state(e->zone_type, al));
            add_value(vals, v, al ? AOS_C_RED : AOS_C_GREEN);
        }
        if (e->occupancy != 0xFF) add_value(vals, e->occupancy & 1 ? _("hay alguien") : _("nadie"), AOS_C_TEXT);
        for (int q = 0; q < e->neps; q++)
            if ((e->onoff_known >> q) & 1) {
                bool on = (e->onoff >> q) & 1;
                bool pend = !memcmp(U.pend_ieee, e->ieee, 8) && U.pend_ep == e->eps_list[q] && now - U.pend_ms < 1500;
                snprintf(v, sizeof v, _("salida %d: %s"), q + 1, pend ? "..." : on ? _("prendida") : _("apagada"));
                lv_obj_t *chip = add_value(vals, v, on ? AOS_C_YELLOW : AOS_C_DIM);
                if (U.nout < (int)(sizeof U.out / sizeof U.out[0])) {
                    /* a button: tapped, the output switches */
                    memcpy(U.out[U.nout].ieee, e->ieee, 8);
                    U.out[U.nout].ep = e->eps_list[q];
                    U.out[U.nout].state = on ? 0 : 1;           /* on: off; off: on */
                    lv_obj_set_style_border_width(chip, 2, 0);
                    lv_obj_set_style_border_color(chip, on ? AOS_C_YELLOW : AOS_C_DIM, 0);
                    lv_obj_set_style_pad_hor(chip, 22, 0);
                    lv_obj_set_style_pad_ver(chip, 12, 0);
                    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_add_event_cb(chip, out_cb, LV_EVENT_CLICKED, (void *)(intptr_t)U.nout);
                    U.nout++;
                }
            }
        if (e->level_ep && e->level != 0xFF) {
            snprintf(v, sizeof v, "%s %d %%", _("brillo"), (e->level * 100 + 127) / 254);
            add_value(vals, v, AOS_C_YELLOW);
        }
        if (e->battery != 0xFF) {
            snprintf(v, sizeof v, "%s %u %%", _("batería"), e->battery / 2);
            add_value(vals, v, e->battery < 40 ? AOS_C_RED : AOS_C_DIM);
        } else if (e->voltage) {
            snprintf(v, sizeof v, "%s %u.%u V", _("batería"), e->voltage / 10, e->voltage % 10);
            add_value(vals, v, AOS_C_DIM);
        }
        /* what the table names: light, pressure, CO2, energy, a thermostat's... */
        for (int q = 0; q < e->nmeas; q++) {
            const char *nm;
            char val[48];
            if (!aos_zigbee_meas_text(e, &e->meas[q], &nm, val, sizeof val)) continue;
            snprintf(v, sizeof v, "%s %s", nm, val);
            add_value(vals, v, AOS_C_TEXT);
        }
        /* Tuya's datapoints: number, type and value (a bool one is a button) */
        for (int q = 0; q < e->ndp; q++) {
            const aos_zigbee_dp_t *dp = &e->dps[q];
            if (dp->type == 1) {
                snprintf(v, sizeof v, "dp %u: %s", dp->dp, dp->v ? _("sí") : _("no"));
                lv_obj_t *chip = add_value(vals, v, dp->v ? AOS_C_YELLOW : AOS_C_DIM);
                if (U.nctl < (int)(sizeof U.ctl / sizeof U.ctl[0])) {
                    memcpy(U.ctl[U.nctl].ieee, e->ieee, 8);
                    U.ctl[U.nctl].ep = dp->dp;                      /* the dp, in ep's place */
                    U.ctl[U.nctl].kind = B_DP;
                    U.ctl[U.nctl].arg = !dp->v;
                    lv_obj_set_style_border_width(chip, 2, 0);
                    lv_obj_set_style_border_color(chip, dp->v ? AOS_C_YELLOW : AOS_C_DIM, 0);
                    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_add_event_cb(chip, ctl_cb, LV_EVENT_CLICKED, (void *)(intptr_t)U.nctl);
                    U.nctl++;
                }
            } else if (dp->type == 2 || dp->type == 4 || dp->type == 5)
                snprintf(v, sizeof v, "dp %u: %ld", dp->dp, (long)dp->v), add_value(vals, v, AOS_C_TEXT);
            else {
                int l = dp->len < (int)sizeof dp->raw ? dp->len : (int)sizeof dp->raw, k2 = snprintf(v, sizeof v, "dp %u: ", dp->dp);
                for (int m = 0; m < l && k2 < (int)sizeof v - 3; m++) k2 += snprintf(v + k2, sizeof v - (size_t)k2, "%02x", dp->raw[m]);
                add_value(vals, v, AOS_C_DIM);
            }
        }
        if (!lv_obj_get_child_count(vals)) lv_obj_add_flag(vals, LV_OBJ_FLAG_HIDDEN);
        /* the last thing it did itself: a button, a remote */
        if (e->event[0]) {
            uint32_t ago = (now - e->event_ms) / 1000;
            snprintf(b, sizeof b, "%s · %s", e->event, ago < 60 ? _("recién") : _("antes"));
            lv_obj_t *l = aos_label(r, b, aos_font_small, ago < 60 ? AOS_C_ACCENT : AOS_C_DIM);
            lv_obj_set_width(l, w - 36);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        }
        /* its controls */
        lv_obj_t *ctls = box(r, w - 36, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ctls, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_column(ctls, 10, 0);
        lv_obj_set_style_pad_row(ctls, 10, 0);
        uint8_t cep;
        if ((cep = e->ctl_ep[AOS_ZB_CTL_COVER])) {
            add_ctl(ctls, _("Subir"), e->ieee, cep, B_COVER, 0x00, false);
            add_ctl(ctls, _("Parar"), e->ieee, cep, B_COVER, 0x02, false);
            add_ctl(ctls, _("Bajar"), e->ieee, cep, B_COVER, 0x01, false);
        }
        if ((cep = e->ctl_ep[AOS_ZB_CTL_THERM])) {
            int32_t sp = meas_of(e, 0x0201, 0x0012, 2000);
            add_ctl(ctls, "-0.5 °C", e->ieee, cep, B_SETPOINT, sp - 50, false);
            add_ctl(ctls, "+0.5 °C", e->ieee, cep, B_SETPOINT, sp + 50, false);
        }
        if ((cep = e->ctl_ep[AOS_ZB_CTL_LOCK])) {
            add_ctl(ctls, _("Cerrar con llave"), e->ieee, cep, B_LOCK, 0x00, false);
            add_ctl(ctls, _("Abrir"), e->ieee, cep, B_LOCK, 0x01, false);
        }
        if ((cep = e->ctl_ep[AOS_ZB_CTL_FAN])) {
            static const int8_t NEXT[] = { 1, 2, 3, 5, 5, 0, 0 };      /* off, low, medium, high, auto, off */
            int32_t m = meas_of(e, 0x0202, 0x0000, 0);
            add_ctl(ctls, _("Ventilador: otro modo"), e->ieee, cep, B_FAN, m >= 0 && m <= 6 ? NEXT[m] : 0, false);
        }
        if (e->clusters & AOS_ZB_CL_IR) {
            uint8_t who[8];
            static char code[8];
            int st = aos_zigbee_ir_state(who, code, sizeof code);
            bool mine = !memcmp(who, e->ieee, 8);
            bool learning = mine && (st == AOS_ZB_IR_LEARNING || st == AOS_ZB_IR_RECEIVING);
            add_ctl(ctls, learning ? _("Esperando el control remoto... (tocá para cancelar)") : _("Aprender un código"),
                    e->ieee, e->ep, B_IR_LEARN, !learning, learning);
            if (code[0]) add_ctl(ctls, _("Enviar el último aprendido"), e->ieee, e->ep, B_IR_SEND, 0, false);
            if (mine && st == AOS_ZB_IR_SENDING) add_value(vals, _("enviando..."), AOS_C_ACCENT);
        }
        if (!lv_obj_get_child_count(ctls)) lv_obj_add_flag(ctls, LV_OBJ_FLAG_HIDDEN);
        if (e->level_ep) add_slider(r, w, e->ieee, e->level_ep, SL_LEVEL, 0, 254, e->level == 0xFF ? 0 : e->level, AOS_C_YELLOW);
        if ((cep = e->ctl_ep[AOS_ZB_CTL_COVER]))
            add_slider(r, w, e->ieee, cep, SL_COVER, 0, 100, (int)meas_of(e, 0x0102, 0x0008, 0), AOS_C_ACCENT);
        if ((cep = e->ctl_ep[AOS_ZB_CTL_COLOR]))
            add_slider(r, w, e->ieee, cep, SL_CT, 153, 500, (int)meas_of(e, 0x0300, 0x0007, 300), AOS_C_ORANGE);

        /* and what it said raw, the newest first: any device's */
        int order[AOS_ZB_ATTRS];
        for (int q = 0; q < e->nattr; q++) {
            int m = q;
            while (m > 0 && e->attr[order[m - 1]].ms < e->attr[q].ms) {
                order[m] = order[m - 1];
                m--;
            }
            order[m] = q;
        }
        for (int q = 0; q < e->nattr; q++) {
            int best = order[q];
            const aos_zigbee_attr_t *at = &e->attr[best];
            if (named_attr(at)) continue;
            char hex[2 * sizeof at->v + 1];
            for (int m = 0; m < at->len; m++) snprintf(hex + 2 * m, 3, "%02x", at->v[m]);
            hex[2 * at->len] = '\0';
            const char *cn = cluster_name(at->cluster);
            char cl[32];
            if (cn) snprintf(cl, sizeof cl, "%s 0x%04x", cn, at->cluster);
            else snprintf(cl, sizeof cl, "0x%04x", at->cluster);
            if (at->type == 0xFF)
                snprintf(b, sizeof b, "ep%u %s %s 0x%02x: %s · %s", at->ep, cl, _("comando"), at->attr, hex,
                         (now - at->ms) < 60000 ? _("recién") : _("antes"));
            else
                snprintf(b, sizeof b, "ep%u %s/0x%04x = %s · %s", at->ep, cl, at->attr, hex,
                         (now - at->ms) < 60000 ? _("recién") : _("antes"));
            lv_obj_t *l = aos_label(r, b, aos_font_tiny, AOS_C_DIM);
            lv_obj_set_width(l, w - 36);
            lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        }
    }
}

/* -------------------------------------------------------------------------- */
/* Refresh                                                                     */
/* -------------------------------------------------------------------------- */

static void refresh(void)
{
    if (U.page_no == P_DEVICES && U.devs && U.devs_tick++ % 4 == 0) fill_devices(U.W - 2 * AOS_UI_PAD);
    if (U.page_no == P_NET && U.net && !U.dragging && U.devs_tick++ % 4 == 0) fill_net(U.W - 2 * AOS_UI_PAD);
    aos_154_info_t in;
    aos_154_info(&in);
    int32_t w = U.W - 2 * AOS_UI_PAD;
    bool old = !in.reachable && aos_hal_uptime_ms() - U.t_open > 2000;
    lv_label_set_text(U.warn, old ? _("El C6 no contesta. Necesita su firmware 3.0.9-154.1 o más nuevo: Ajustes, Actualización, "
                                      "Instalar en el C6. Se baja de la Tienda, en Sistema.")
                                  : in.guarded ? _("El C6 se cayó con la radio prendida y la dejó apagada para no perder el Wi-Fi. "
                                                   "Tocá Escanear o Escuchar para probar otra vez.") : "");
    char sub[96];
    if (in.state == AOS_154_SCANNING) snprintf(sub, sizeof sub, _("Escaneando, canal %u"), in.channel);
    else if (in.state == AOS_154_SNIFFING) snprintf(sub, sizeof sub, _("Escuchando el canal %u"), in.channel);
    else snprintf(sub, sizeof sub, "%s", in.reachable ? _("La radio, apagada") : "");
    lv_label_set_text(U.sub, sub);

    if (U.page_no == P_CHANNELS && U.bars[0]) {
        aos_154_chan_t ch[16];
        aos_154_channels(ch);
        int32_t ih = (U.H > U.W ? 520 : 400) - 40, bottom = ih - 40, top = 40;
        int best = -1, best_frames = 0;
        for (int i = 0; i < 16; i++) {
            if (!ch[i].when_ms) continue;
            int db = ch[i].ed_max < DB_MIN ? DB_MIN : ch[i].ed_max > DB_MAX ? DB_MAX : ch[i].ed_max;
            int32_t h = 4 + (bottom - top - 40) * (db - DB_MIN) / (DB_MAX - DB_MIN);
            lv_obj_set_height(U.bars[i], h);
            lv_obj_set_y(U.bars[i], bottom - h);
            lv_obj_set_style_bg_color(U.bars[i], ch[i].frames ? C_ZB : db > -75 ? AOS_C_ORANGE : AOS_C_GREEN, 0);
            char t[12];
            if (ch[i].frames) snprintf(t, sizeof t, "%u", ch[i].frames);
            else t[0] = '\0';
            lv_label_set_text(U.bar_lbl[i], t);
            lv_obj_set_y(U.bar_lbl[i], bottom - h - 28);
            if (ch[i].frames > best_frames) { best_frames = ch[i].frames; best = 11 + i; }
        }
        lv_label_set_text(U.scan_lbl, in.state == AOS_154_SCANNING ? _("Escaneando...") : _("Escanear"));
        if (best > 0) {
            lv_label_set_text_fmt(U.scan_note, _("Hay tráfico en el canal %d: en Escuchar se ve qué es."), best);
            if (in.state != AOS_154_SNIFFING) U.chan = best;
        } else if (in.scan_seq) lv_label_set_text(U.scan_note, _("No se oyó ningún paquete. Probá a fondo: las redes tranquilas mandan poco."));
    }
    if (U.page_no == P_SNIFF && U.list) {
        lv_label_set_text(U.go_lbl, in.state == AOS_154_SNIFFING ? _("Parar") : _("Escuchar"));
        char c[220];
        int n = snprintf(c, sizeof c, _("%u paquetes"), (unsigned)(in.last_seq >= U.start ? in.last_seq - U.start + 1 : 0));
        if (in.dropped) {
            n += snprintf(c + n, sizeof c - (size_t)n, ", ");
            n += snprintf(c + n, sizeof c - (size_t)n, _("%u perdidos en el C6"), (unsigned)in.dropped);
        }
        if (in.pcap[0]) {
            const char *base = strrchr(in.pcap, '/');
            n += snprintf(c + n, sizeof c - (size_t)n, " · ");
            snprintf(c + n, sizeof c - (size_t)n, _("guardando %u en %s"), (unsigned)in.pcap_frames, base ? base + 1 : in.pcap);
        }
        lv_label_set_text(U.counts, c);
        static aos_154_frame_t buf[16];
        int k;
        while ((k = aos_154_frames(U.from, buf, 16)) > 0) {
            for (int i = 0; i < k; i++) add_row(&buf[i], w);
            U.from = buf[k - 1].seq + 1;
            if (k < 16) break;
        }
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

static void build(void)
{
    if (U.page) lv_obj_delete(U.page);
    memset(U.bars, 0, sizeof U.bars);
    U.list = NULL;
    U.devs = NULL;
    U.net = NULL;
    int32_t w = U.W - 2 * AOS_UI_PAD;
    for (int i = 0; i < P_COUNT; i++)
        lv_obj_set_style_bg_color(U.tabs[i], i == U.page_no ? AOS_C_ACCENT : AOS_C_CARD, 0);
    U.page = box(U.col, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(U.page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.page, 16, 0);
    if (U.page_no == P_CHANNELS) build_channels(U.page, w);
    else if (U.page_no == P_SNIFF) build_sniff(U.page, w);
    else if (U.page_no == P_DEVICES) build_devices(U.page, w);
    else build_net(U.page, w);
    refresh();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    memset(&U, 0, sizeof U);
    U.root = root;
    U.chan = 15;
    U.t_open = (uint32_t)aos_hal_uptime_ms();
    lv_obj_update_layout(root);
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    int32_t w = U.W - 2 * AOS_UI_PAD;
    U.col = lv_obj_create(root);
    lv_obj_remove_style_all(U.col);
    lv_obj_set_size(U.col, U.W, U.H);
    lv_obj_set_style_pad_hor(U.col, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.col, 8, 0);
    lv_obj_set_style_pad_bottom(U.col, 24, 0);
    lv_obj_set_flex_flow(U.col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(U.col, 16, 0);
    lv_obj_set_scroll_dir(U.col, LV_DIR_VER);

    lv_obj_t *head = box(U.col, w, 72);
    lv_obj_align(aos_label(head, AOS_SYM_ACCESS_POINT, &aos_sym_44, C_ZB), LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_align(aos_label(head, _("Zigbee y Thread"), aos_font_title, AOS_C_TEXT), LV_ALIGN_LEFT_MID, 60, -12);
    U.sub = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    lv_obj_align(U.sub, LV_ALIGN_LEFT_MID, 62, 22);
    U.warn = wrap(U.col, "", aos_font_caption, AOS_C_ORANGE, w);

    lv_obj_t *tabs = box(U.col, w, 60);
    lv_obj_set_flex_flow(tabs, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(tabs, 10, 0);
    static const char *const TABS[P_COUNT] = { N_("Canales"), N_("Escuchar"), N_("Equipos"), N_("Mi red") };
    for (int i = 0; i < P_COUNT; i++) {
        lv_obj_t *c = box(tabs, LV_SIZE_CONTENT, 60);
        lv_obj_set_style_radius(c, 30, 0);
        lv_obj_set_style_pad_hor(c, 24, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(aos_label(c, aos_tr(TABS[i]), aos_font_small, AOS_C_TEXT));
        U.tabs[i] = c;
    }
    aos_154_hello();
    build();
    U.timer = lv_timer_create(timer_cb, 250, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.timer) lv_timer_delete(U.timer);
    aos_154_info_t in;
    aos_154_info(&in);
    /* the scan and the sniffer stop; the board's network (the RCP) stays */
    if (in.state == AOS_154_SCANNING || in.state == AOS_154_SNIFFING) aos_154_stop();
    memset(&U, 0, sizeof U);
}

void aos_app_zigbee_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.zigbee", .name = "Zigbee", .icon = AOS_SYM_ACCESS_POINT,
            .color_a = 0xEB0443, .color_b = 0x9E0230,
            .order = 215,
        },
        .create = create, .destroy = destroy,
    };
}
