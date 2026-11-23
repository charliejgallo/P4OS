/*
 * P4OS - Monitor serie (aos.plotter): the numbers that arrive on a serial
 * port, plotted live.
 *
 * It does not open the port itself: it reads a channel of the firmware's
 * capture service (aos_serial_*), the same A and B the Terminal shows, so
 * both can look at one device at once and the capture goes on with the app
 * closed. Opening the app reads the ring the service kept (its last 3000
 * lines) before going live, and changing the format reads it again.
 *
 * A timer every 40 ms takes the new lines: at 115200 baud with short lines
 * that is ~40 lines a call, and the ring holds seconds of them, so nothing
 * is lost while LVGL is busy. The app stays alive hidden
 * (AOS_APP_FLAG_BACKGROUND): the alarms, the CSV and the portal's page go on
 * with another app in front.
 *
 * The screen: the connection and the tools on top (on the right when the
 * screen lies down), the last value of one series big with the window's
 * low, high and average, and the chart. Turning the screen builds the
 * screen again and keeps the data.
 */
#include "pl.h"

#include "aos_icon_ops.h"

pl_app_t PLA;
pl_set_t PLS;

const uint32_t PL_WINDOWS[] = { 10000, 30000, 60000, 300000, 900000, 3600000, 4 * 3600000u, 8 * 3600000u };
const int PL_NWIN = (int)(sizeof PL_WINDOWS / sizeof PL_WINDOWS[0]);

/* A screen with a line going up and down on it, and the plug under it */
static const uint8_t PL_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, 34, 22, 9, 2, AIC_C_DIM, 255),                 /* plug   */
    AIC_RECT(AIC_CENTER, 0, -6, 78, 60, 10, AIC_C_LIT(0x0A1519), 255),     /* screen */
    AIC_BORDER(3, AIC_C_DIM, 255),
    AIC_INTO,
    AIC_RECT(AIC_CENTER, 0, 16, 64, 2, 1, AIC_C_LIT(0x24414A), 255),      /* base   */
    AIC_RECT(AIC_CENTER, -20, 1, 27, 6, 3, AIC_C_TEAL, 255),
    AIC_ROT(-484),
    AIC_RECT(AIC_CENTER, -5, -1, 23, 6, 3, AIC_C_TEAL, 255),
    AIC_ROT(450),
    AIC_RECT(AIC_CENTER, 9, -4, 27, 6, 3, AIC_C_TEAL, 255),
    AIC_ROT(-550),
    AIC_RECT(AIC_CENTER, 22, -9, 18, 6, 3, AIC_C_TEAL, 255),
    AIC_ROT(398),
    AIC_RECT(AIC_CENTER, 28, -4, 9, 9, AIC_CIRCLE, AIC_C_ORANGE, 255),
    AIC_OUT,
    AIC_END
};

/* ---- preferences ---- */

static void pref_f(const char *k, float *v)
{
    char b[24];
    if (aos_hal_pref_get_str(k, b, sizeof b)) *v = strtof(b, NULL);
}

static void set_load(void)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;
    PLS.fmt = -1;
    PLS.win = 2;
    PLS.eol = 1;
    PLS.ys = 1;
    int32_t v;
    if (aos_hal_pref_get_i32("pl_ch", &v) && (v == 0 || v == 1)) PLS.ch = v;
    if (aos_hal_pref_get_i32("pl_fmt", &v) && v >= -1 && v < PL_MAXP) PLS.fmt = v;
    if (aos_hal_pref_get_i32("pl_win", &v) && v >= 0 && v < PL_NWIN) PLS.win = v;
    if (aos_hal_pref_get_i32("pl_mode", &v)) PLS.mode = v == PL_MODE_XY ? PL_MODE_XY : PL_MODE_TIME;
    if (aos_hal_pref_get_i32("pl_xs", &v) && v >= 0 && v < PL_MAXS) PLS.xs = v;
    if (aos_hal_pref_get_i32("pl_ys", &v) && v >= 0 && v < PL_MAXS) PLS.ys = v;
    if (aos_hal_pref_get_i32("pl_yfix", &v)) PLS.yfix = v != 0;
    if (aos_hal_pref_get_i32("pl_eol", &v) && v >= 0 && v < 4) PLS.eol = v;
    if (aos_hal_pref_get_i32("pl_big", &v) && v >= 0 && v < PL_MAXS) PLS.big = v;
    pref_f("pl_ymin", &PLS.ymin);
    pref_f("pl_ymax", &PLS.ymax);
}

void pl_set_save(void)
{
    aos_hal_pref_set_i32("pl_ch", PLS.ch);
    aos_hal_pref_set_i32("pl_fmt", PLS.fmt);
    aos_hal_pref_set_i32("pl_win", PLS.win);
    aos_hal_pref_set_i32("pl_mode", PLS.mode);
    aos_hal_pref_set_i32("pl_xs", PLS.xs);
    aos_hal_pref_set_i32("pl_ys", PLS.ys);
    aos_hal_pref_set_i32("pl_yfix", PLS.yfix);
    aos_hal_pref_set_i32("pl_eol", PLS.eol);
    aos_hal_pref_set_i32("pl_big", PLS.big);
    char b[24];
    snprintf(b, sizeof b, "%.6g", (double)PLS.ymin);
    aos_hal_pref_set_str("pl_ymin", b);
    snprintf(b, sizeof b, "%.6g", (double)PLS.ymax);
    aos_hal_pref_set_str("pl_ymax", b);
}

const char *pl_fmt_name(void)
{
    if (PLS.fmt >= 0 && PLS.fmt < PLC.np) return PLC.p[PLS.fmt].name;
    return _("Automático");
}

/* ---- reading the channel ---- */

static int s_beeps;
static uint32_t s_next_beep;

static void alarm_check(int i, float v)
{
    pl_series_t *s = &PLD.s[i];
    uint8_t st = s->lo_on && v < s->lo ? 1 : s->hi_on && v > s->hi ? 2 : 0;
    if (st == s->state) return;
    s->state = st;
    if (!st) return;
    uint32_t now = pl_now();
    if (s->alert_t && now - s->alert_t < 10000) return;
    s->alert_t = now ? now : 1;
    char v1[24], v2[24], t[120];
    pl_fmt(v1, sizeof v1, v, s->dec);
    pl_fmt(v2, sizeof v2, st == 1 ? s->lo : s->hi, s->dec < 0 ? 2 : s->dec);
    snprintf(t, sizeof t, st == 1 ? _("%s: %s %s, por debajo de %s") : _("%s: %s %s, por encima de %s"), s->name, v1,
             s->unit, v2);
    aos_ui_toast(t, 3500);
    if (s->beep) {
        s_beeps = 3;
        s_next_beep = now;
    }
}

static void describe(const pl_parsed_t *p, int n, char *out, size_t cap)
{
    if (n < 0) { pl_scpy(out, cap, _("(no coincide: salteada)")); return; }
    if (p->header) {
        size_t k = (size_t)snprintf(out, cap, "%s ", _("encabezado:"));
        for (int i = 0; i < PL_MAXS && p->name[i][0] && k < cap; i++)
            k += (size_t)snprintf(out + k, cap - k, "%s%s", i ? ", " : "", p->name[i]);
        return;
    }
    if (!n) { pl_scpy(out, cap, _("(sin números)")); return; }
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < n && k < cap; i++) {
        char v[24];
        pl_fmt(v, sizeof v, p->v[i], p->dec[i]);
        k += (size_t)snprintf(out + k, cap - k, "%s%s=%s%s%s", i ? "  " : "", p->name[i], v, p->unit[i][0] ? " " : "",
                              p->unit[i]);
    }
}

static pl_parsed_t s_pend;
static uint32_t s_pend_t;
static int s_pend_n;

static bool known(const pl_parsed_t *p)
{
    for (int k = 0; k < p->n; k++) {
        bool f = false;
        for (int i = 0; i < PLD.ns && !f; i++) f = !strcmp(PLD.s[i].name, p->name[k]);
        if (!f) return false;
    }
    return true;
}

static bool same_shape(const pl_parsed_t *a, const pl_parsed_t *b)
{
    if (a->n != b->n) return false;
    for (int k = 0; k < a->n; k++)
        if (strcmp(a->name[k], b->name[k])) return false;
    return true;
}

static void add(const pl_parsed_t *p, uint32_t t)
{
    PLA.used++;
    pl_row_t row;
    pl_data_add(t, p, &row);
    if (pl_csv_on()) pl_csv_row(&row);
    for (int i = 0; i < PLD.ns; i++)
        if (!PL_ISNAN(row.v[i])) alarm_check(i, row.v[i]);
    PLA.dirty = true;
}

static void process(const char *text, uint32_t t)
{
    PLA.lines++;
    pl_parsed_t p;
    int n = PLS.fmt >= 0 && PLS.fmt < PLC.np ? pl_parse_profile(&PLC.p[PLS.fmt], text, &p)
                                           : pl_parse_auto(&PLA.aut, text, &p);
    pl_rawline_t *r = &PLA.raw[PLA.nraw % PL_RAW_N];
    PLA.nraw++;
    pl_scpy(r->text, sizeof r->text, text);
    describe(&p, n, r->got, sizeof r->got);
    if (n <= 0) return;
    if (PLS.fmt < 0 && !known(&p)) {
        /* new names: only once the same shape comes twice in a row, so a
         * board's boot log ("ets Jun  8 2016", "len:1344") makes no series */
        if (!s_pend_n || !same_shape(&s_pend, &p)) {
            s_pend = p;
            s_pend_t = t;
            s_pend_n = 1;
            size_t k = strlen(r->got);
            snprintf(r->got + k, sizeof r->got - k, " %s", _("(¿serie nueva?)"));
            return;
        }
        s_pend_n = 0;
        add(&s_pend, s_pend_t);
    }
    s_pend_n = 0;
    add(&p, t);
}

static void ingest(void)
{
    aos_serial_stat_t st;
    if (!aos_serial_stat(PLS.ch, &st)) return;
    if (PLA.next > st.lines) PLA.next = st.first;
    if (PLA.next < st.first) {
        aos_hal_log("plotter", "%u lines left the ring unread", (unsigned)(st.first - PLA.next));
        PLA.lost += st.first - PLA.next;
        PLA.next = st.first;
    }
    char buf[AOS_SERIAL_LINE_MAX];
    int budget = 3000;
    while (PLA.next < st.lines && budget-- > 0) {
        uint32_t t = 0;
        int len = aos_serial_line(PLS.ch, PLA.next, buf, sizeof buf, &t);
        PLA.next++;
        if (len >= 0) process(buf, t);
    }
}

void pl_restart_reading(void)
{
    aos_serial_stat_t st;
    aos_serial_stat(PLS.ch, &st);
    pl_data_clear();
    PLD.ns = 0;
    PLD.gen++;
    memset(&PLA.aut, 0, sizeof PLA.aut);
    memset(PLA.raw, 0, sizeof PLA.raw);
    PLA.nraw = 0;
    PLA.lines = PLA.used = PLA.lost = 0;
    s_pend_n = 0;
    PLA.next = st.first;
    PLA.paused = false;
    if (PLS.big >= PL_MAXS) PLS.big = 0;
    ingest();
    pl_chart_want();
    pl_refresh(true);
}

/* ---- the screen ---- */

static lv_obj_t *tool_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 72);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(b, text ? 16 : 0, 0);
    if (!text) lv_obj_set_width(b, 72);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 8, 0);
    if (glyph) {
        lv_obj_t *g = lv_label_create(b);
        lv_obj_set_style_text_font(g, &aos_sym_28, 0);
        lv_label_set_text(g, glyph);
    }
    if (text) {
        lv_obj_t *l = lv_label_create(b);
        lv_obj_set_style_text_font(l, aos_font_small, 0);
        lv_label_set_text(l, text);
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(l, 200, 0);
    }
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

static lv_obj_t *last_label(lv_obj_t *b) { return lv_obj_get_child(b, (int32_t)lv_obj_get_child_count(b) - 1); }

static void channel_cb(lv_event_t *e)
{
    int ch = (int)(intptr_t)lv_event_get_user_data(e);
    if (ch == PLS.ch) return;
    PLS.ch = ch;
    pl_set_save();
    pl_conn_load();
    pl_restart_reading();
}

static void connect_cb(lv_event_t *e)
{
    (void)e;
    aos_serial_stat_t st;
    aos_serial_stat(PLS.ch, &st);
    if (st.running) {
        aos_serial_stop(PLS.ch);
    } else {
        char port[AOS_IO_PORT_NAME_MAX];
        lv_dropdown_get_selected_str(PLA.port_dd, port, sizeof port);
        if (!aos_serial_start_ex(PLS.ch, port, PLA.baud, PLA.frame, PLA.ctl)) {
            const aos_io_port_t *p = aos_io_port_find(port);
            const char *holder = p ? aos_io_owner(p->pins[0]) : NULL;
            char t[80];
            if (holder) snprintf(t, sizeof t, _("%s lo tiene %s"), port, holder);
            else pl_scpy(t, sizeof t, _("No se pudo abrir el puerto"));
            aos_ui_toast(t, 2500);
        }
        char k[16];
        snprintf(k, sizeof k, "pl_port%d", PLS.ch);
        aos_hal_pref_set_str(k, port);
        pl_conn_apply();
    }
    pl_refresh(true);
}

static void conn_cb(lv_event_t *e) { (void)e; pl_sheet_conn(); }
static void fmt_cb(lv_event_t *e) { (void)e; pl_sheet_format(); }
static void win_cb(lv_event_t *e) { (void)e; pl_sheet_view(); }
static void al_cb(lv_event_t *e) { (void)e; pl_sheet_series(); }
static void send_cb(lv_event_t *e) { (void)e; pl_sheet_send(); }

static void pause_cb(lv_event_t *e)
{
    (void)e;
    PLA.paused = !PLA.paused;
    if (PLA.paused) PLA.end_t = pl_now();
    pl_chart_want();
    pl_refresh(true);
}

static void live_cb(lv_event_t *e)
{
    (void)e;
    PLA.paused = false;
    pl_chart_want();
    pl_refresh(true);
}

static void rec_cb(lv_event_t *e)
{
    (void)e;
    if (pl_csv_on()) {
        pl_csv_stop();
        char t[160];
        snprintf(t, sizeof t, "%s %s", _("Guardado en"), pl_csv_path());
        aos_ui_toast(t, 3000);
    } else if (!pl_csv_start()) {
        aos_ui_toast(_("Hace falta la tarjeta"), 2000);
    } else {
        aos_ui_toast(_("Grabando a CSV en la tarjeta"), 1800);
    }
    pl_refresh(true);
}

void pl_view_flags(bool chart)
{
    if (!PLA.self) return;
    if (chart) PLA.self->desc.flags |= AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG;
    else PLA.self->desc.flags &= ~(uint32_t)(AOS_APP_FLAG_NO_SWIPE | AOS_APP_FLAG_LONG_DRAG);
}

void pl_fmt_dur(char *out, size_t cap, uint32_t ms)
{
    uint32_t s = (ms + 500) / 1000;
    if (s < 60) snprintf(out, cap, "%u s", (unsigned)s);
    else if (s < 3600) snprintf(out, cap, "%u min", (unsigned)((s + 30) / 60));
    else if (s % 3600 == 0) snprintf(out, cap, "%u h", (unsigned)(s / 3600));
    else snprintf(out, cap, "%.1f h", (double)((float)s / 3600.0f));
}

static void big_update(void)
{
    if (!PLA.big_val) return;
    int s = PLS.big < PLD.ns ? PLS.big : (PLD.ns ? 0 : -1);
    char v[32], t[160];
    if (s < 0) {
        lv_label_set_text(PLA.big_name, _("Sin datos todavía"));
        lv_label_set_text(PLA.big_val, "-");
        lv_label_set_text(PLA.big_unit, "");
        lv_label_set_text(PLA.big_stats, "");
        lv_obj_set_style_bg_color(PLA.bigbox, lv_color_hex(0x14181E), 0);
        return;
    }
    const pl_series_t *se = &PLD.s[s];
    float mn, mx, avg, last;
    bool st = pl_chart_stats(s, &mn, &mx, &avg, &last);
    if (!PLA.paused && se->has) last = se->last;
    else if (!st) last = NAN;
    pl_fmt(v, sizeof v, last, se->dec);
    lv_label_set_text(PLA.big_val, v);
    /* a long number in a smaller font */
    size_t len = strlen(v);
    const lv_font_t *f = PLA.land ? (len <= 7 ? &aos_inter_64 : aos_font_large)
                                : (len <= 7 ? &aos_inter_num_96 : len <= 10 ? &aos_inter_64 : aos_font_large);
    lv_obj_set_style_text_font(PLA.big_val, f, 0);
    lv_label_set_text(PLA.big_unit, se->unit);
    lv_label_set_text(PLA.big_name, se->name);
    lv_obj_set_style_text_color(PLA.big_name, lv_color_hex(se->col), 0);
    if (st) {
        char a[24], b[24], c[24];
        int d = se->dec < 0 ? 2 : se->dec;
        pl_fmt(a, sizeof a, mn, d);
        pl_fmt(b, sizeof b, mx, d);
        pl_fmt(c, sizeof c, avg, d + 1);
        snprintf(t, sizeof t, "%s  %s\n%s  %s\n%s  %s", _("mín"), a, _("máx"), b, _("prom"), c);
    } else {
        t[0] = 0;
    }
    lv_label_set_text(PLA.big_stats, t);
    lv_color_t bg = se->state ? lv_color_hex(0x4A1A1A) : lv_color_hex(0x14181E);
    lv_obj_set_style_bg_color(PLA.bigbox, bg, 0);
}

void pl_refresh(bool force)
{
    (void)force;
    if (!PLA.root || !PLA.bar) return;
    aos_serial_stat_t st;
    aos_serial_stat(PLS.ch, &st);
    for (int c = 0; c < 2; c++) {
        bool on = c == PLS.ch;
        lv_obj_set_style_bg_color(PLA.seg[c], on ? lv_color_hex(0xF2F2F7) : AOS_C_CARD2, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(PLA.seg[c], 0), on ? lv_color_hex(0x1C1C1E) : AOS_C_TEXT, 0);
    }
    lv_label_set_text(PLA.connect_lbl, st.running ? _("Detener") : _("Conectar"));
    lv_obj_set_style_bg_color(PLA.connect, st.running ? AOS_C_CARD2 : AOS_C_ACCENT, 0);
    lv_obj_set_state(PLA.port_dd, LV_STATE_DISABLED, st.running);
    lv_label_set_text_fmt(PLA.conn_lbl, "%u · %s", (unsigned)PLA.baud, PLA.frame);
    lv_label_set_text(PLA.fmt_lbl, pl_fmt_name());
    char w[24];
    pl_fmt_dur(w, sizeof w, PLA.win_ms);
    lv_label_set_text(PLA.win_lbl, PLS.mode == PL_MODE_XY ? "XY" : w);
    lv_label_set_text(PLA.pause_lbl, PLA.paused ? AOS_SYM_PLAY : AOS_SYM_PAUSE);
    lv_obj_set_state(PLA.pause_btn, LV_STATE_CHECKED, PLA.paused);
    lv_obj_set_state(PLA.rec_btn, LV_STATE_CHECKED, pl_csv_on());
    bool alarm = false, out = false;
    for (int i = 0; i < PLD.ns; i++) {
        alarm |= PLD.s[i].lo_on || PLD.s[i].hi_on;
        out |= PLD.s[i].state != 0;
    }
    lv_obj_set_style_text_color(last_label(PLA.al_btn), out ? AOS_C_RED : alarm ? AOS_C_ORANGE : AOS_C_TEXT, 0);
    if (PLA.paused) lv_obj_remove_flag(PLA.live_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(PLA.live_btn, LV_OBJ_FLAG_HIDDEN);

    char t[200];
    if (st.running) {
        size_t k = (size_t)snprintf(t, sizeof t, "%s · %u %s · %u %s", st.port, (unsigned)st.baud, st.frame,
                                    (unsigned)PLA.lines, _("líneas"));
        if (PLA.lines > PLA.used && PLA.lines) k += (size_t)snprintf(t + k, sizeof t - k, " (%u %s)", (unsigned)(PLA.lines - PLA.used), _("sin números"));
        if (PLA.lost) k += (size_t)snprintf(t + k, sizeof t - k, " · %u %s", (unsigned)PLA.lost, _("perdidas"));
        if (pl_csv_on() && k < sizeof t) snprintf(t + k, sizeof t - k, " · %s", _("grabando"));
    } else {
        snprintf(t, sizeof t, "%s", PLD.rows ? _("Desconectado: el gráfico guarda lo que llegó") : _("Sin conexión: elegí el puerto y la velocidad"));
    }
    lv_label_set_text(PLA.status, t);
    big_update();
}

static void pl_cat(char *dst, const char *src, size_t cap)
{
    size_t n = strlen(dst);
    if (n < cap) pl_scpy(dst + n, cap - n, src);
}

void pl_port_options(char *ports, size_t cap)
{
    ports[0] = 0;
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *p = aos_io_port_at(i);
        if (p->kind != AOS_PORT_UART) continue;
        if (ports[0]) pl_cat(ports, "\n", cap);
        pl_cat(ports, p->name, cap);
    }
    PLA.usb_n = aos_hal_usb_serial_count();
    for (int i = 0; i < PLA.usb_n && i < 4; i++) {
        char u[8];
        snprintf(u, sizeof u, "%susb%d", ports[0] ? "\n" : "", i);
        pl_cat(ports, u, cap);
    }
    if (!ports[0]) snprintf(ports, cap, "uart.b");
}

static void ports_follow(void)
{
    if (!PLA.port_dd || aos_hal_usb_serial_count() == PLA.usb_n || lv_dropdown_is_open(PLA.port_dd)) return;
    char was[AOS_IO_PORT_NAME_MAX] = "", ports[160];
    lv_dropdown_get_selected_str(PLA.port_dd, was, sizeof was);
    pl_port_options(ports, sizeof ports);
    lv_dropdown_set_options(PLA.port_dd, ports);
    int sel = lv_dropdown_get_option_index(PLA.port_dd, was);
    if (sel >= 0) lv_dropdown_set_selected(PLA.port_dd, (uint32_t)sel);
}

static lv_obj_t *dropdown(lv_obj_t *parent, const char *opts, int32_t w)
{
    lv_obj_t *d = lv_dropdown_create(parent);
    lv_dropdown_set_options(d, opts);
    lv_obj_set_size(d, w, 72);
    lv_obj_set_style_text_font(d, aos_font_small, 0);
    lv_obj_set_style_bg_color(d, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(d, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_set_style_radius(d, 20, 0);
    lv_obj_t *list = lv_dropdown_get_list(d);
    lv_obj_set_style_text_font(list, aos_font_small, 0);
    lv_obj_set_style_bg_color(list, AOS_C_CARD2, 0);
    lv_obj_set_style_text_color(list, AOS_C_TEXT, 0);
    return d;
}

static lv_obj_t *box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_bigbox(lv_obj_t *parent, int32_t w, int32_t h)
{
    PLA.bigbox = box(parent);
    lv_obj_set_size(PLA.bigbox, w, h);
    lv_obj_set_style_radius(PLA.bigbox, 20, 0);
    lv_obj_set_style_bg_opa(PLA.bigbox, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(PLA.bigbox, lv_color_hex(0x14181E), 0);
    lv_obj_add_flag(PLA.bigbox, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(PLA.bigbox, al_cb, LV_EVENT_CLICKED, NULL);
    PLA.big_name = aos_label(PLA.bigbox, "", aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(PLA.big_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(PLA.big_name, PLA.land ? w - 36 : w - 260);
    lv_obj_set_pos(PLA.big_name, 20, 12);
    PLA.big_val = aos_label(PLA.bigbox, "-", &aos_inter_num_96, AOS_C_TEXT);
    PLA.big_unit = aos_label(PLA.bigbox, "", aos_font_title, AOS_C_DIM);
    PLA.big_stats = aos_label(PLA.bigbox, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_text_line_space(PLA.big_stats, 4, 0);
    if (PLA.land) {
        lv_obj_align(PLA.big_val, LV_ALIGN_TOP_LEFT, 18, 44);
        lv_obj_align(PLA.big_unit, LV_ALIGN_TOP_RIGHT, -18, 12);
        lv_obj_align(PLA.big_stats, LV_ALIGN_BOTTOM_LEFT, 20, -12);
    } else {
        lv_obj_align(PLA.big_val, LV_ALIGN_LEFT_MID, 16, 18);
        lv_obj_align_to(PLA.big_unit, PLA.big_val, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -14);
        lv_obj_align(PLA.big_stats, LV_ALIGN_RIGHT_MID, -20, 0);
        lv_obj_set_style_text_align(PLA.big_stats, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

static void unit_follow(lv_event_t *e)
{
    (void)e;
    /* the unit rides right after the number, whatever its width */
    if (!PLA.land && PLA.big_unit) lv_obj_align_to(PLA.big_unit, PLA.big_val, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -14);
}

static void build_ui(void)
{
    lv_obj_t *root = PLA.root;
    PLA.W = lv_obj_get_width(root);
    PLA.H = lv_obj_get_height(root);
    PLA.land = PLA.W > PLA.H;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    const int32_t pad = 16;

    /* the tools: on top when upright, a column on the right lying down */
    int32_t side = 360;
    PLA.bar = box(root);
    lv_obj_set_flex_flow(PLA.bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(PLA.bar, 10, 0);
    if (PLA.land) {
        lv_obj_set_size(PLA.bar, side, LV_SIZE_CONTENT);
        lv_obj_set_pos(PLA.bar, PLA.W - pad - side, 8);
    } else {
        lv_obj_set_size(PLA.bar, PLA.W - 2 * pad, LV_SIZE_CONTENT);
        lv_obj_set_pos(PLA.bar, pad, 8);
    }
    for (int c = 0; c < 2; c++) {
        lv_obj_t *s = box(PLA.bar);
        lv_obj_set_size(s, 64, 72);
        lv_obj_set_style_radius(s, 20, 0);
        lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
        lv_obj_add_flag(s, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s, channel_cb, LV_EVENT_CLICKED, (void *)(intptr_t)c);
        lv_obj_center(aos_label(s, c ? "B" : "A", aos_font_body, AOS_C_TEXT));
        PLA.seg[c] = s;
    }
    char ports[160];
    pl_port_options(ports, sizeof ports);
    if (PLA.land) {
        PLA.connect = tool_btn(PLA.bar, NULL, _("Conectar"), connect_cb, NULL);
        lv_obj_set_flex_grow(PLA.connect, 1);
        PLA.port_dd = dropdown(PLA.bar, ports, 136);
        lv_obj_add_flag(PLA.port_dd, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
        PLA.conn_btn = tool_btn(PLA.bar, NULL, "115200 · 8N1", conn_cb, NULL);
        lv_obj_set_flex_grow(PLA.conn_btn, 1);
    } else {
        PLA.port_dd = dropdown(PLA.bar, ports, 150);
        PLA.conn_btn = tool_btn(PLA.bar, NULL, "115200 · 8N1", conn_cb, NULL);
        PLA.connect = tool_btn(PLA.bar, NULL, _("Conectar"), connect_cb, NULL);
        lv_obj_set_flex_grow(PLA.connect, 1);
    }
    PLA.conn_lbl = last_label(PLA.conn_btn);
    PLA.connect_lbl = last_label(PLA.connect);
    PLA.fmt_btn = tool_btn(PLA.bar, AOS_SYM_FILE_DELIMITED_OUTLINE, "", fmt_cb, NULL);
    lv_obj_add_flag(PLA.fmt_btn, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    PLA.fmt_lbl = last_label(PLA.fmt_btn);
    lv_obj_set_style_max_width(PLA.fmt_lbl, PLA.land ? 190 : 170, 0);
    if (PLA.land) lv_obj_set_flex_grow(PLA.fmt_btn, 1);
    PLA.win_btn = tool_btn(PLA.bar, AOS_SYM_CHART_LINE, "1 min", win_cb, NULL);
    PLA.win_lbl = last_label(PLA.win_btn);
    if (!PLA.land) lv_obj_set_flex_grow(PLA.win_btn, 1);
    PLA.pause_btn = tool_btn(PLA.bar, AOS_SYM_PAUSE, NULL, pause_cb, NULL);
    PLA.pause_lbl = last_label(PLA.pause_btn);
    if (PLA.land) lv_obj_add_flag(PLA.pause_btn, LV_OBJ_FLAG_FLEX_IN_NEW_TRACK);
    PLA.rec_btn = tool_btn(PLA.bar, AOS_SYM_RECORD_REC, NULL, rec_cb, NULL);
    lv_obj_set_style_bg_color(PLA.rec_btn, AOS_C_RED, LV_STATE_CHECKED);
    PLA.al_btn = tool_btn(PLA.bar, AOS_SYM_BELL_OUTLINE, NULL, al_cb, NULL);
    PLA.send_btn = tool_btn(PLA.bar, AOS_SYM_SEND, NULL, send_cb, NULL);
    lv_obj_update_layout(PLA.bar);
    int32_t bar_h = lv_obj_get_height(PLA.bar);

    PLA.status = aos_label(root, "", aos_font_caption, AOS_C_DIM);
    lv_label_set_long_mode(PLA.status, PLA.land ? LV_LABEL_LONG_MODE_WRAP : LV_LABEL_LONG_MODE_DOTS);

    int32_t chart_x, chart_y, chart_w, chart_h;
    if (PLA.land) {
        lv_obj_set_width(PLA.status, side);
        lv_obj_set_pos(PLA.status, PLA.W - pad - side, 8 + bar_h + 10);
        int32_t by = 8 + bar_h + 10 + 60;
        build_bigbox(root, side, PLA.H - by - 8);
        lv_obj_set_pos(PLA.bigbox, PLA.W - pad - side, by);
        chart_x = pad;
        chart_y = 8;
        chart_w = PLA.W - 2 * pad - side - 14;
        chart_h = PLA.H - 16;
    } else {
        lv_obj_set_width(PLA.status, PLA.W - 2 * pad);
        lv_obj_set_pos(PLA.status, pad, 8 + bar_h + 8);
        int32_t by = 8 + bar_h + 40;
        build_bigbox(root, PLA.W - 2 * pad, 156);
        lv_obj_set_pos(PLA.bigbox, pad, by);
        chart_x = pad;
        chart_y = by + 156 + 12;
        chart_w = PLA.W - 2 * pad;
        chart_h = PLA.H - chart_y - 12;
    }
    lv_obj_add_event_cb(PLA.big_val, unit_follow, LV_EVENT_SIZE_CHANGED, NULL);
    lv_obj_t *ch = pl_chart_create(root, chart_w, chart_h);
    lv_obj_set_pos(ch, chart_x, chart_y);
    PLA.chart = ch;
    PLA.live_btn = tool_btn(root, AOS_SYM_PLAY, _("En vivo"), live_cb, NULL);
    lv_obj_set_style_bg_color(PLA.live_btn, AOS_C_ACCENT, 0);
    lv_obj_align_to(PLA.live_btn, ch, LV_ALIGN_BOTTOM_RIGHT, -130, -50);
    lv_obj_add_flag(PLA.live_btn, LV_OBJ_FLAG_HIDDEN);
    pl_conn_load();
    pl_view_flags(true);
    pl_refresh(true);
}

void pl_rebuild(void)
{
    pl_sheet_close();
    lv_obj_clean(PLA.root);
    PLA.bar = PLA.chart = PLA.big_val = NULL;
    PLA.overlay = NULL;
    build_ui();
}

/* ---- the timer ---- */

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t s_cfg_at, s_ui_at, s_raw_seen;
    uint32_t now = pl_now();
    ingest();
    if (now - s_cfg_at > 1000) {
        s_cfg_at = now;
        if (pl_cfg_changed()) {
            pl_cfg_load();
            for (int i = 0; i < PLD.ns; i++) pl_cfg_alarm_apply(&PLD.s[i]);
            if (PLS.fmt >= PLC.np) PLS.fmt = -1;
            if (PLS.fmt >= 0) pl_restart_reading();
        }
        ports_follow();
    }
    if (!PLA.hidden) pl_chart_poll();      /* hidden: reading, alarms, CSV and the portal go on */
    pl_csv_poll();
    pl_live_poll();
    if (s_beeps > 0 && (int32_t)(now - s_next_beep) >= 0) {
        aos_hal_beep(2400, 120);
        s_beeps--;
        s_next_beep = now + 300;
    }
    if (now - s_ui_at > 250 && !PLA.hidden) {
        s_ui_at = now;
        if (!PLA.overlay) pl_refresh(false);
        if (PLA.nraw != s_raw_seen) {
            s_raw_seen = PLA.nraw;
            pl_sheet_raw_update();
        }
    }
}

/* ---- life cycle ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    PLA.self = self;
    PLA.root = root;
    PLA.hidden = false;
    pl_i18n_load();
    set_load();
    bool fresh = !PLD.raw;
    if (fresh) {
        pl_cfg_load();
        if (!pl_data_init()) aos_ui_toast(_("No hay memoria para los datos"), 2500);
        PLA.win_ms = PL_WINDOWS[PLS.win];
        if (PLS.fmt >= PLC.np) PLS.fmt = -1;
    }
    build_ui();
    if (fresh) pl_restart_reading();
    if (!PLA.timer) PLA.timer = lv_timer_create(timer_cb, 40, NULL);
    return &PLA;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (PLA.timer) lv_timer_delete(PLA.timer);
    PLA.timer = NULL;
    pl_csv_stop();
    pl_live_stop();
    pl_chart_free();
    pl_data_free();
    pl_set_save();
    PLA.root = PLA.bar = PLA.overlay = PLA.big_val = NULL;
    PLA.self = NULL;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    PLA.root = root;
    pl_rebuild();
    pl_chart_want();
    return true;
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (PLA.overlay) {
        pl_sheet_close();
        pl_refresh(true);
        return true;
    }
    return false;
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    PLA.hidden = true;
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    PLA.hidden = false;
    pl_i18n_load();
    pl_chart_want();
    pl_refresh(true);
}

static bool plotter_init(aos_app_t *app)
{
    app->desc.id       = "aos.plotter";
    app->desc.name     = "Monitor serie";
    app->desc.icon     = AOS_SYM_CHART_LINE;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x125565;
    app->desc.color_b  = 0x061A20;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 112;
    aos_icon_set_ops(app, PL_ICON, sizeof PL_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    return true;
}

AOS_APP_ENTRY(plotter_init);
