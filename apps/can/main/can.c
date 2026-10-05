/*
 * P4OS - CAN: a bus analyser for the TWAI controllers on the header.
 *
 * A bar on top - the connection (pins, speed, mode; tap it to change it),
 * connect, record, pause, the recordings, clear, the wiring - then the
 * controller's state, always in view, and four tabs:
 *
 *   Tramas    every frame as it comes, with its time from the connection
 *   Por id    one row per id: the last data, how many a second, and the
 *             bytes that are changing lit up; tap one for its bits
 *   Enviar    a frame by hand on a hex pad, the saved ones, periodic ones
 *   Señales   the signals of a .dbc decoded, and one of them drawn in time
 *
 * It opens in "solo escucha" every time: on a bus that is not ours (a car)
 * the board must not acknowledge nor send anything until the user says so.
 * The bus stays open with the app in the background (the spy keeps
 * counting, a recording keeps going); closing the app closes it.
 *
 * The BOOT button pauses and resumes the views.
 */
#include "can.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdlib.h>
#include <string.h>

cn_ui_t  U;
cn_cfg_t CN_CFG = { .tx = 28, .rx = 29, .rate = 500000, .mode = CN_MODE_LISTEN };

/* Three nodes hanging from the two wires of the bus. */
static const uint8_t CAN_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER,   0,  10, 76,  5,  2,         AIC_C_TEXT, 255),   /* CANH */
    AIC_RECT(AIC_CENTER,   0,  22, 76,  5,  2,         AIC_C_TEXT, 150),   /* CANL */
    AIC_RECT(AIC_CENTER, -24,   0,  4, 26,  1,         AIC_C_TEXT, 255),   /* stubs */
    AIC_RECT(AIC_CENTER,   0,   0,  4, 26,  1,         AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER,  24,   0,  4, 26,  1,         AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -24, -18, 18, 16,  4,         AIC_C_TEXT, 255),   /* nodes */
    AIC_RECT(AIC_CENTER,   0, -18, 18, 16,  4,         AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER,  24, -18, 18, 16,  4,         AIC_C_TEXT, 255),
    AIC_END
};

static const char *const TAB_NAME[CN_TAB_COUNT] = { N_("Tramas"), N_("Por id"), N_("Enviar"), N_("Señales") };

/* ---- settings ---- */

static void cfg_load(void)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;
    int32_t v;
    if (aos_hal_pref_get_i32("can_tx", &v) && aos_io_pin_of_gpio(v)) CN_CFG.tx = v;
    if (aos_hal_pref_get_i32("can_rx", &v) && aos_io_pin_of_gpio(v)) CN_CFG.rx = v;
    if (aos_hal_pref_get_i32("can_rate", &v) && v >= 25000 && v <= 1000000) CN_CFG.rate = (uint32_t)v;
    if (aos_hal_pref_get_i32("can_fk", &v) && v >= 0 && v <= CN_FILT_EXT) CN_CFG.fkind = (cn_filt_t)v;
    if (aos_hal_pref_get_i32("can_fid", &v)) CN_CFG.fid = (uint32_t)v;
    if (aos_hal_pref_get_i32("can_fm", &v)) CN_CFG.fmask = (uint32_t)v;
    if (aos_hal_pref_get_i32("can_tab", &v) && v >= 0 && v < CN_TAB_COUNT) U.tab = v;
}

void cn_cfg_save(void)
{
    aos_hal_pref_set_i32("can_tx", CN_CFG.tx);
    aos_hal_pref_set_i32("can_rx", CN_CFG.rx);
    aos_hal_pref_set_i32("can_rate", (int32_t)CN_CFG.rate);
    aos_hal_pref_set_i32("can_fk", CN_CFG.fkind);
    aos_hal_pref_set_i32("can_fid", (int32_t)CN_CFG.fid);
    aos_hal_pref_set_i32("can_fm", (int32_t)CN_CFG.fmask);
}

/* ---- widgets shared by the tabs ---- */

lv_obj_t *cn_btn(lv_obj_t *parent, const char *glyph, const char *text, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 72);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, AOS_C_CARD2, 0);
    lv_obj_set_style_bg_color(b, AOS_C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(b, 18, 0);
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
    }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    return b;
}

lv_obj_t *cn_pill(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, LV_SIZE_CONTENT, 64);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_set_style_radius(b, 32, 0);
    lv_obj_set_style_bg_color(b, on ? AOS_C_ACCENT : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = aos_label(b, text, aos_font_small, AOS_C_TEXT);
    lv_obj_center(l);
    return b;
}

/* ---- sheets ---- */

static void sheet_close_cb(lv_event_t *e) { (void)e; cn_sheet_close(); }

void cn_sheet_close(void)
{
    if (U.sheet) lv_obj_delete(U.sheet);
    U.sheet = NULL;
}

lv_obj_t *cn_sheet_open(const char *title)
{
    cn_sheet_close();
    U.sheet = lv_obj_create(U.root);
    lv_obj_remove_style_all(U.sheet);
    lv_obj_set_size(U.sheet, U.W, U.H);
    lv_obj_set_style_bg_color(U.sheet, lv_color_hex(0x101318), 0);
    lv_obj_set_style_bg_opa(U.sheet, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(U.sheet, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = aos_label(U.sheet, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_width(t, U.W - 2 * AOS_UI_PAD - 100);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, AOS_UI_PAD, 22);
    lv_obj_t *x = cn_btn(U.sheet, AOS_SYM_CLOSE, NULL, sheet_close_cb, NULL);
    lv_obj_set_width(x, 80);
    lv_obj_align(x, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, 10);

    lv_obj_t *body = lv_obj_create(U.sheet);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, U.W, U.H - 96);
    lv_obj_set_pos(body, 0, 96);
    lv_obj_set_style_pad_hor(body, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_bottom(body, 24, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    return body;
}

static void (*s_text_done)(const char *);
static lv_obj_t *s_text_ta;

static void text_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c != LV_EVENT_READY && c != LV_EVENT_CANCEL) return;
    char v[96];
    snprintf(v, sizeof v, "%s", lv_textarea_get_text(s_text_ta));
    void (*done)(const char *) = c == LV_EVENT_READY ? s_text_done : NULL;
    cn_sheet_close();
    if (done) done(v);
}

void cn_text_entry(const char *title, const char *value, lv_keyboard_mode_t mode, void (*done)(const char *))
{
    lv_obj_t *body = cn_sheet_open(title);
    s_text_done = done;
    s_text_ta = lv_textarea_create(body);
    lv_textarea_set_one_line(s_text_ta, true);
    lv_textarea_set_max_length(s_text_ta, 90);
    lv_textarea_set_text(s_text_ta, value);
    lv_obj_set_size(s_text_ta, LV_PCT(100), 88);
    lv_obj_set_style_text_font(s_text_ta, &aos_mono_22, 0);
    lv_obj_set_style_bg_color(s_text_ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(s_text_ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(s_text_ta, 0, 0);
    lv_obj_set_style_radius(s_text_ta, 20, 0);
    lv_obj_set_style_pad_all(s_text_ta, 24, 0);
    lv_obj_t *kb = lv_keyboard_create(U.sheet);
    lv_obj_set_size(kb, U.W, U.land ? U.H / 2 : U.H * 2 / 5);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(kb, aos_font_body);
    lv_keyboard_set_mode(kb, mode);
    lv_keyboard_set_textarea(kb, s_text_ta);
    lv_obj_add_event_cb(kb, text_cb, LV_EVENT_ALL, NULL);
}

/* ---- view filters ---- */

bool cn_view_hidden(uint32_t id, uint8_t fl)
{
    fl &= CN_EXT;
    if (U.only_on) return id != U.only_id || fl != U.only_fl;
    for (int i = 0; i < U.nhidden; i++)
        if (U.hidden[i] == id && U.hidden_fl[i] == fl) return true;
    return false;
}

/* ---- the bar and the status ---- */

static void conn_cb(lv_event_t *e) { (void)e; cn_conn_sheet(); }
static void wiring_cb(lv_event_t *e) { (void)e; cn_wiring_sheet(); }
static void files_cb(lv_event_t *e) { (void)e; cn_files_sheet(); }

static void connect_cb(lv_event_t *e)
{
    (void)e;
    if (CB.open) {
        cn_bus_close();
        if (U.paused) cn_toggle_pause();
    } else {
        const char *why = cn_bus_open(&CN_CFG);
        if (why) aos_ui_toast(why, 3000);
    }
    cn_ui_status();
}

static void rec_cb(lv_event_t *e)
{
    (void)e;
    if (CB.rec) cn_rec_stop();
    else if (!CB.open) aos_ui_toast(_("Conectá el bus primero"), 1800);
    else if (!aos_hal_path_sd_root()) aos_ui_toast(_("No hay tarjeta"), 1800);
    else if (cn_rec_start()) aos_ui_toast(_("Grabando en la tarjeta, carpeta can"), 1800);
    cn_ui_status();
}

void cn_toggle_pause(void)
{
    U.paused = !U.paused;
    cn_live_pause(U.paused);
    if (U.pause_lbl) lv_label_set_text(U.pause_lbl, U.paused ? AOS_SYM_PLAY : AOS_SYM_PAUSE);
    if (U.pause_btn) {
        if (U.paused) lv_obj_add_state(U.pause_btn, LV_STATE_CHECKED);
        else lv_obj_remove_state(U.pause_btn, LV_STATE_CHECKED);
    }
    cn_live_refresh(true);
    cn_ids_refresh(true);
}

static void pause_cb(lv_event_t *e) { (void)e; cn_toggle_pause(); }

static void clear_cb(lv_event_t *e)
{
    (void)e;
    if (U.paused) cn_toggle_pause();
    cn_bus_clear();
    U.only_on = false;
    U.nhidden = 0;
    cn_live_refresh(true);
    cn_ids_refresh(true);
}

static void recover_cb(lv_event_t *e)
{
    (void)e;
    cn_lock();
    CB.want_recover = true;
    cn_unlock();
    aos_ui_toast(_("Recuperando: 128 silencios de 11 bits y vuelve"), 2000);
}

static void fmt_rate(uint32_t r, char *out, size_t cap)
{
    if (r % 1000000 == 0) snprintf(out, cap, "%u Mbit/s", (unsigned)(r / 1000000));
    else if (r % 1000 == 0) snprintf(out, cap, "%u kbit/s", (unsigned)(r / 1000));
    else snprintf(out, cap, "%u bit/s", (unsigned)r);
}

void cn_ui_status(void)
{
    if (!U.status) return;
    cn_lock();
    bool open = CB.open, rec = CB.rec, replay = CB.replay;
    cn_cfg_t cfg = open ? CB.cfg : CN_CFG;
    char state[12];
    snprintf(state, sizeof state, "%s", CB.state);
    aos_can_status_t st = CB.st;
    float fps = CB.fps, load = CB.load;
    uint32_t tx_fail = CB.tx_fail, rec_frames = CB.rec_frames, rec_lost = CB.rec_lost, rsent = CB.replay_sent;
    uint64_t rec_t0 = CB.rec_t0;
    char rec_name[48], rp_name[48];
    snprintf(rec_name, sizeof rec_name, "%s", CB.rec_name);
    snprintf(rp_name, sizeof rp_name, "%s", CB.replay_name);
    cn_unlock();

    char rate[24], buf[200];
    fmt_rate(cfg.rate, rate, sizeof rate);
    if (cfg.mode == CN_MODE_SELFTEST && cfg.tx == cfg.rx)
        snprintf(buf, sizeof buf, "%s · %s · GPIO%d", rate, cn_mode_name(cfg.mode), cfg.tx);
    else
        snprintf(buf, sizeof buf, "%s · %s · TX %d · RX %d", rate, cn_mode_name(cfg.mode), cfg.tx, cfg.rx);
    lv_label_set_text(U.conn_lbl, buf);
    lv_obj_set_style_bg_color(U.conn_dot, cn_state_color(open ? state : NULL), 0);
    lv_label_set_text(U.connect_lbl, open ? _("Desconectar") : _("Conectar"));
    lv_obj_set_style_bg_color(U.connect_btn, open ? AOS_C_CARD2 : AOS_C_GREEN, 0);
    if (rec) lv_obj_add_state(U.rec_btn, LV_STATE_CHECKED);
    else lv_obj_remove_state(U.rec_btn, LV_STATE_CHECKED);
    bool off = open && !strcmp(state, "bus_off");
    if (off) lv_obj_remove_flag(U.recover_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.recover_btn, LV_OBJ_FLAG_HIDDEN);

    if (!open) {
        lv_label_set_text(U.status, _("Desconectado. Tocá la conexión para elegir pines, velocidad y modo."));
        lv_obj_set_style_text_color(U.status, AOS_C_DIM, 0);
        lv_label_set_text(U.status2, "");
        return;
    }
    snprintf(buf, sizeof buf, _("%s · errores TX %u RX %u · de bus %u · %.0f tramas/s · carga %.0f %%"),
             cn_state_name(state), st.tx_errors, st.rx_errors, (unsigned)st.bus_errors, (double)fps,
             (double)(load * 100));
    lv_label_set_text(U.status, buf);
    lv_obj_set_style_text_color(U.status, cn_state_color(state), 0);
    int n = snprintf(buf, sizeof buf, _("%u recibidas · %u enviadas · %u perdidas"), (unsigned)st.received,
                     (unsigned)st.sent, (unsigned)st.dropped);
    if (tx_fail) n += snprintf(buf + n, sizeof buf - n, _(" · %u sin confirmar"), (unsigned)tx_fail);
    if (rec) {
        unsigned s = (unsigned)((cn_us() - rec_t0) / 1000000);
        n += snprintf(buf + n, sizeof buf - n, _(" · grabando %s, %u:%02u, %u tramas"), rec_name, s / 60, s % 60,
                      (unsigned)rec_frames);
        if (rec_lost) n += snprintf(buf + n, sizeof buf - n, _(" (%u perdidas)"), (unsigned)rec_lost);
    }
    if (replay) snprintf(buf + n, sizeof buf - n, _(" · repitiendo %s, %u"), rp_name, (unsigned)rsent);
    lv_label_set_text(U.status2, buf);
}

/* ---- tabs ---- */

static void tab_cb(lv_event_t *e) { cn_ui_set_tab((int)(intptr_t)lv_event_get_user_data(e)); }

void cn_ui_set_tab(int tab)
{
    U.tab = tab;
    aos_hal_pref_set_i32("can_tab", tab);
    for (int i = 0; i < CN_TAB_COUNT; i++) {
        lv_obj_set_style_bg_color(U.tabs[i], i == tab ? AOS_C_ACCENT : AOS_C_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), i == tab ? lv_color_white() : AOS_C_DIM, 0);
    }
    lv_obj_clean(U.content);
    lv_obj_update_layout(U.content);    /* the tabs size themselves from it */
    switch (tab) {
    case CN_TAB_LIVE: cn_live_build(U.content); break;
    case CN_TAB_IDS:  cn_ids_build(U.content); break;
    case CN_TAB_SEND: cn_send_build(U.content); break;
    default:          cn_sig_build(U.content); break;
    }
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t ticks;
    ticks++;
    if (U.boot_pause) {
        U.boot_pause = false;
        cn_toggle_pause();
    }
    cn_sig_sample();
    if (CB.want_reload) {
        /* the portal rewrote the saved frames or the signals */
        cn_lock();
        CB.want_reload = false;
        cn_unlock();
        cn_periodic_stop_all();
        cn_saved_load();
        cn_sig_reload();
        if ((U.tab == CN_TAB_SEND || U.tab == CN_TAB_SIG) && !U.sheet) cn_ui_set_tab(U.tab);
    }
    if (ticks % 3 == 0) cn_ui_status();
    switch (U.tab) {
    case CN_TAB_LIVE: cn_live_refresh(false); break;
    case CN_TAB_IDS:  if (ticks % 2 == 0) cn_ids_refresh(false); break;
    case CN_TAB_SEND: if (ticks % 5 == 0) cn_send_refresh(); break;
    default:          if (ticks % 3 == 0) cn_sig_refresh(); break;
    }
}

void cn_ui_rebuild(void)
{
    lv_obj_t *root = U.root;
    cn_sheet_close();
    lv_obj_clean(root);
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    const int32_t pad = 16;
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0B0E12), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(root);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, U.W - 2 * pad, LV_SIZE_CONTENT);
    lv_obj_set_pos(bar, pad, 8);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(bar, 10, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    /* the connection: a card with the state's dot, tap to change it */
    lv_obj_t *conn = lv_obj_create(bar);
    lv_obj_remove_style_all(conn);
    lv_obj_set_height(conn, 72);
    lv_obj_set_style_radius(conn, 20, 0);
    lv_obj_set_style_bg_color(conn, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(conn, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(conn, 18, 0);
    lv_obj_add_flag(conn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(conn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(conn, conn_cb, LV_EVENT_CLICKED, NULL);
    if (U.land) lv_obj_set_flex_grow(conn, 1);
    else lv_obj_set_width(conn, LV_PCT(100));
    U.conn_dot = lv_obj_create(conn);
    lv_obj_remove_style_all(U.conn_dot);
    lv_obj_set_size(U.conn_dot, 18, 18);
    lv_obj_set_style_radius(U.conn_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(U.conn_dot, LV_OPA_COVER, 0);
    lv_obj_align(U.conn_dot, LV_ALIGN_LEFT_MID, 0, 0);
    U.conn_lbl = aos_label(conn, "", aos_font_small, AOS_C_TEXT);
    lv_label_set_long_mode(U.conn_lbl, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(U.conn_lbl, LV_PCT(90));
    lv_obj_align(U.conn_lbl, LV_ALIGN_LEFT_MID, 32, 0);
    aos_make_decorative(U.conn_dot);
    aos_make_decorative(U.conn_lbl);

    U.connect_btn = cn_btn(bar, NULL, _("Conectar"), connect_cb, NULL);
    U.connect_lbl = lv_obj_get_child(U.connect_btn, 0);
    if (!U.land) lv_obj_set_flex_grow(U.connect_btn, 1);
    U.recover_btn = cn_btn(bar, AOS_SYM_RESTART, _("Recuperar"), recover_cb, NULL);
    lv_obj_set_style_bg_color(U.recover_btn, AOS_C_RED, 0);
    U.rec_btn = cn_btn(bar, AOS_SYM_RECORD_REC, NULL, rec_cb, NULL);
    lv_obj_set_style_bg_color(U.rec_btn, AOS_C_RED, LV_STATE_CHECKED);
    U.pause_btn = cn_btn(bar, U.paused ? AOS_SYM_PLAY : AOS_SYM_PAUSE, NULL, pause_cb, NULL);
    U.pause_lbl = lv_obj_get_child(U.pause_btn, 0);
    lv_obj_set_style_bg_color(U.pause_btn, AOS_C_ORANGE, LV_STATE_CHECKED);
    if (U.paused) lv_obj_add_state(U.pause_btn, LV_STATE_CHECKED);
    cn_btn(bar, AOS_SYM_FOLDER, NULL, files_cb, NULL);
    cn_btn(bar, AOS_SYM_DELETE, NULL, clear_cb, NULL);
    cn_btn(bar, AOS_SYM_CONNECTION, NULL, wiring_cb, NULL);
    lv_obj_update_layout(bar);
    int32_t y = 8 + lv_obj_get_height(bar) + 8;

    int32_t lh = lv_font_get_line_height(aos_font_caption);
    U.status = aos_label(root, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_size(U.status, U.W - 2 * pad, lh);
    lv_label_set_long_mode(U.status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.status, pad, y);
    y += 28;
    U.status2 = aos_label(root, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_size(U.status2, U.W - 2 * pad, lh);
    lv_label_set_long_mode(U.status2, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.status2, pad, y);
    y += 34;

    /* the tabs */
    int32_t tw = (U.W - 2 * pad - 3 * 8) / CN_TAB_COUNT;
    for (int i = 0; i < CN_TAB_COUNT; i++) {
        lv_obj_t *t = lv_obj_create(root);
        lv_obj_remove_style_all(t);
        lv_obj_set_size(t, tw, 60);
        lv_obj_set_pos(t, pad + i * (tw + 8), y);
        lv_obj_set_style_radius(t, 16, 0);
        lv_obj_set_style_bg_opa(t, LV_OPA_COVER, 0);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_center(aos_label(t, _(TAB_NAME[i]), aos_font_small, AOS_C_TEXT));
        U.tabs[i] = t;
    }
    y += 60 + 12;

    U.cx = pad;
    U.cy = y;
    U.cw = U.W - 2 * pad;
    U.ch = U.H - y - 12;
    U.content = lv_obj_create(root);
    lv_obj_remove_style_all(U.content);
    lv_obj_set_pos(U.content, U.cx, U.cy);
    lv_obj_set_size(U.content, U.cw, U.ch);
    lv_obj_remove_flag(U.content, LV_OBJ_FLAG_SCROLLABLE);

    cn_ui_set_tab(U.tab);
    cn_ui_status();
}

/* ---- the app ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    cfg_load();
    CN_CFG.mode = CN_MODE_LISTEN;       /* every time: see the top of this file */
    cn_bus_init();
    if (!CB.ring || !CB.ids) {
        lv_obj_center(aos_label(root, _("No hay memoria para el espía"), aos_font_body, AOS_C_RED));
        return NULL;
    }
    cn_saved_load();
    cn_sig_write_example();
    cn_sig_reload();
    U.root = root;
    U.paused = false;
    U.only_on = false;
    U.nhidden = 0;
    cn_ui_rebuild();
    U.timer = lv_timer_create(timer_cb, 100, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.timer) lv_timer_delete(U.timer);
    U.timer = NULL;
    U.sheet = NULL;
    U.status = NULL;
    cn_bus_deinit();
    cn_spy_free();
    U.paused = false;
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    U.root = root;
    cn_ui_rebuild();
    return true;
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.timer) lv_timer_resume(U.timer);
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.timer) lv_timer_pause(U.timer);
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (U.sheet) {
        cn_sheet_close();
        return true;
    }
    return false;
}

static bool button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    (void)inst;
    if (action != AOS_BUTTON_CLICK) return action == AOS_BUTTON_PRESS;
    U.boot_pause = true;            /* acted on in the next timer round */
    return true;
}

static bool init(aos_app_t *app)
{
    app->desc.id       = "aos.can";
    app->desc.name     = "CAN";
    app->desc.icon     = AOS_SYM_LAN;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x1B9C8F;
    app->desc.color_b  = 0x0B4A44;
    app->desc.flags    = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 120;
    aos_icon_set_ops(app, CAN_ICON, sizeof CAN_ICON);

    app->create  = create;
    app->destroy = destroy;
    app->resize  = resize;
    app->show    = show;
    app->hide    = hide;
    app->back    = back;
    app->button  = button;
    return true;
}

AOS_APP_ENTRY(init);
