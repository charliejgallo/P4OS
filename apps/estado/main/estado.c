/*
 * P4OS - Estado de la casa (aos.estado): a board of the services at home.
 *
 * Each service (Home Assistant, the cameras' recorder, a NAS, a printer,
 * the router, a web page...) is tried every so often, over HTTP(S) or as a
 * TCP port, and shows up with a traffic light: green when it answers,
 * yellow when it is slow, red when it is down; its latency, and how long
 * ago that changed. A service opens into its detail: the latency of the
 * last hours, how much of the last day and week it was up, its last
 * outages and its certificate's days. Desk mode shows it all in big tiles
 * over the whole screen, to leave the board on a shelf.
 *
 * The app lives in the background (BACKGROUND) and starts with the board
 * (aos_ui_boot_start_set) while it has services: when one goes down or
 * comes back it says so over whatever is in front, with two beeps, and
 * while something is down a red glyph stays in the status bar and the icon
 * carries how many.
 *
 * The services are in estado.txt (ec_cfg.c), which the portal's page
 * (web/estado.js) edits too; the app looks at the file's date every two
 * seconds. The page reads the state live (aos_hal_live_*) and the history
 * from estado_eventos.txt.
 *
 * Development: EC_OPEN=<n> opens the n-th service, EC_DESK=1 desk mode,
 * EC_EDIT=<n> the editor (EC_EDIT=new a new one), EC_CONFIG=<path> another
 * estado.txt.
 */
#include "ec.h"
#include "aos_icon_ops.h"

#include <stdlib.h>

#define TIMER_MS      250
#define CFG_POLL_MS   2000
#define LIVE_MS       1000
#define NOTE_DOWN_MS  12000
#define NOTE_UP_MS    7000
#define NOTE_LINES    3
#define STOP_WAIT_MS  20000

/* A house with a traffic light in it */
static const uint8_t EC_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, 0, -11, 40, 40, 5, AIC_C_TEXT, 255), AIC_ROT(450),     /* roof */
    AIC_RECT(AIC_CENTER, 0, 11, 54, 40, 6, AIC_C_TEXT, 255),                    /* walls */
    AIC_RECT(AIC_CENTER, 0, 12, 42, 16, AIC_CIRCLE, AIC_C_LIT(0x1C1C1E), 255),  /* the light */
    AIC_RECT(AIC_CENTER, -13, 12, 10, 10, AIC_CIRCLE, AIC_C_GREEN, 255),
    AIC_RECT(AIC_CENTER, 0, 12, 10, 10, AIC_CIRCLE, AIC_C_YELLOW, 255),
    AIC_RECT(AIC_CENTER, 13, 12, 10, 10, AIC_CIRCLE, AIC_C_RED, 255),
    AIC_END
};

ec_app_t ec_app;
#define A ec_app

static struct {
    lv_timer_t *timer;
    uint32_t    cfg_poll, live_t;
    int         badge;
    bool        icon_on;
    lv_obj_t   *note;
    uint32_t    note_until;
    ec_event_t  lines[NOTE_LINES];
    int         nlines;
    int         live_detail;
} L;

/* ---- screens ------------------------------------------------------------------------------- */

void ec_build(void)
{
    lv_obj_clean(A.root);
    A.W = lv_obj_get_width(A.root);
    A.H = lv_obj_get_height(A.root);
    A.land = A.W > A.H;
    lv_obj_set_style_bg_color(A.root, AOS_C_BG, 0);
    lv_obj_set_style_bg_opa(A.root, LV_OPA_COVER, 0);
    if (A.screen == EC_SCR_DETAIL) ec_detail_build();
    else if (A.screen == EC_SCR_EDIT) ec_edit_build();
    else ec_list_build();
}

void ec_go_list(void)
{
    if (A.screen == EC_SCR_EDIT) ec_edit_close();
    A.screen = EC_SCR_LIST;
    ec_build();
}

void ec_go_detail(int i)
{
    if (i < 0 || i >= A.cfg.n) return;
    if (A.screen == EC_SCR_EDIT) ec_edit_close();
    A.screen = EC_SCR_DETAIL;
    A.detail = i;
    ec_build();
}

void ec_go_edit(int i)
{
    ec_edit_open(i);
    A.screen = EC_SCR_EDIT;
    ec_build();
}

static void boot_sync(void)
{
    bool want = A.cfg.boot && A.cfg.n > 0;
    if (aos_ui_boot_start_get(EC_APP_ID) != want) aos_ui_boot_start_set(EC_APP_ID, want);
}

bool ec_cfg_commit(void)
{
    bool ok = ec_cfg_save(&A.cfg);
    A.cfg_stamp = ec_cfg_stamp();
    ec_core_sync(&A.cfg);
    boot_sync();
    return ok;
}

/* ---- notices -------------------------------------------------------------------------------- */

static void note_close(void)
{
    if (L.note) lv_obj_delete(L.note);
    L.note = NULL;
    L.nlines = 0;
}

static void note_cb(lv_event_t *e)
{
    (void)e;
    int i = L.nlines ? L.lines[L.nlines - 1].i : -1;
    char name[EC_NAME];
    ec_copy(name, sizeof name, L.nlines ? L.lines[L.nlines - 1].name : "");
    note_close();
    if (A.hidden) aos_ui_open(EC_APP_ID);
    if (ec_desk_on()) ec_desk_close();
    /* by name: the list may have moved since */
    for (int k = 0; k < A.cfg.n; k++)
        if (!strcmp(A.cfg.s[k].name, name)) i = k;
    if (i >= 0 && i < A.cfg.n) ec_go_detail(i);
}

static void note_line(lv_obj_t *parent, const ec_event_t *ev, int32_t w)
{
    lv_obj_t *r = ec_box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    const char *g = ev->kind == EC_EV_DOWN ? AOS_SYM_LAN_DISCONNECT : ev->kind == EC_EV_UP ? AOS_SYM_CHECK : AOS_SYM_LOCK;
    lv_color_t c = ev->kind == EC_EV_DOWN ? AOS_C_RED : ev->kind == EC_EV_UP ? AOS_C_GREEN : AOS_C_ORANGE;
    aos_make_decorative(aos_label(r, g, &aos_sym_44, c));
    lv_obj_t *col = ec_box(r, w - 60, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    char name[EC_NAME * 2], b[160], d[48];
    aos_text_safe(name, sizeof name, ev->name);
    if (ev->kind == EC_EV_DOWN) snprintf(b, sizeof b, _("%s se cayó"), name);
    else if (ev->kind == EC_EV_UP) snprintf(b, sizeof b, _("%s volvió"), name);
    else snprintf(b, sizeof b, _("Certificado de %s"), name);
    ec_text(col, b, aos_font_body, AOS_C_TEXT, w - 60);
    if (ev->kind == EC_EV_DOWN) {
        if (ev->err == EC_E_CODE) snprintf(b, sizeof b, _("Contestó %d"), ev->code);
        else ec_copy(b, sizeof b, ec_err_text(ev->err));
    } else if (ev->kind == EC_EV_UP) {
        ec_fmt_dur(d, sizeof d, (uint32_t)ev->n);
        snprintf(b, sizeof b, _("Estuvo caído %s"), d);
    } else if (ev->n < 0) {
        ec_copy(b, sizeof b, _("Venció"));
    } else {
        snprintf(b, sizeof b, _("Vence en %d días"), (int)ev->n);
    }
    ec_text(col, b, aos_font_small, AOS_C_DIM, w - 60);
}

static void note_build(void)
{
    if (L.note) lv_obj_delete(L.note);
    L.note = NULL;
    if (!L.nlines) return;
    lv_obj_t *top = lv_layer_top();
    int32_t sw = lv_obj_get_width(top);
    int32_t w = sw - 2 * AOS_UI_PAD;
    if (w > 680) w = 680;
    L.note = ec_card(top, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(L.note, lv_color_hex(0x2C2C2E), 0);
    lv_obj_set_style_border_width(L.note, 2, 0);
    bool any_down = false;
    for (int k = 0; k < L.nlines; k++) any_down |= L.lines[k].kind == EC_EV_DOWN;
    lv_obj_set_style_border_color(L.note, any_down ? AOS_C_RED : lv_color_hex(0x48484A), 0);
    lv_obj_set_style_shadow_width(L.note, 40, 0);
    lv_obj_set_style_shadow_color(L.note, lv_color_black(), 0);
    lv_obj_set_style_shadow_opa(L.note, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(L.note, 22, 0);
    lv_obj_set_style_pad_row(L.note, 14, 0);
    lv_obj_set_flex_flow(L.note, LV_FLEX_FLOW_COLUMN);
    lv_obj_align(L.note, LV_ALIGN_TOP_MID, 0, 60);
    lv_obj_add_flag(L.note, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(L.note, note_cb, LV_EVENT_CLICKED, NULL);
    for (int k = L.nlines - 1; k >= 0; k--) note_line(L.note, &L.lines[k], w - 44);
}

static void notices(void)
{
    ec_event_t ev;
    bool got = false;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    while (ec_event_pop(&ev)) {
        if (L.nlines == NOTE_LINES) {
            memmove(&L.lines[0], &L.lines[1], sizeof L.lines[0] * (NOTE_LINES - 1));
            L.nlines--;
        }
        L.lines[L.nlines++] = ev;
        got = true;
        aos_hal_log(EC_TAG, "%s: %s", ev.name, ev.kind == EC_EV_DOWN ? "down" : ev.kind == EC_EV_UP ? "up" : "cert");
        if (A.cfg.sound) ec_beep(ev.kind != EC_EV_UP);
        uint32_t until = now + (ev.kind == EC_EV_UP ? NOTE_UP_MS : NOTE_DOWN_MS);
        if (!L.note || (int32_t)(until - L.note_until) > 0) L.note_until = until;
    }
    if (got) note_build();
    else if (L.note && (int32_t)(now - L.note_until) >= 0) note_close();
    if (L.note) lv_obj_move_foreground(L.note);
    ec_beep_tick();

    ec_lock();
    int down = ec_count(EC_ST_DOWN);
    ec_unlock();
    if (down != L.badge) {
        L.badge = down;
        aos_ui_set_badge(EC_APP_ID, down);
    }
    if ((down > 0) != L.icon_on) {
        L.icon_on = down > 0;
        aos_ui_status_icon_set(EC_APP_ID, L.icon_on ? AOS_SYM_LAN_DISCONNECT : NULL, 0xFF453A);
    }
}

/* ---- the portal's page ------------------------------------------------------------------------ */

static size_t jstr(char *o, size_t n, const char *s)
{
    size_t k = 0;
    if (k < n) o[k++] = '"';
    for (; s && *s && k + 3 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            o[k++] = '\\';
            o[k++] = (char)c;
        } else if (c < 0x20) {
            o[k++] = ' ';
        } else {
            o[k++] = (char)c;
        }
    }
    if (k < n) o[k++] = '"';
    return k;
}

#define JS_MAX (60 * 1024)

static void jpct(char *o, size_t n, float p)
{
    if (p < 0) snprintf(o, n, "null");
    else snprintf(o, n, "%.3f", (double)p);
}

static void live_put(void)
{
    char *js = ec_psram(JS_MAX);
    if (!js) return;
    size_t o = 0;
    uint32_t now = (uint32_t)aos_hal_uptime_ms(), e = ec_now_epoch();
    o += (size_t)snprintf(js + o, JS_MAX - o, "{\"v\":1,\"net\":%s,\"clock\":%s,\"fails\":%d,\"sound\":%s,\"boot\":%s,\"now\":%lu,\"services\":[",
                          ec_net_ok() ? "true" : "false", e ? "true" : "false", A.cfg.fails,
                          A.cfg.sound ? "true" : "false", A.cfg.boot ? "true" : "false", (unsigned long)e);
    ec_lock();
    for (int i = 0; i < A.cfg.n && o + 1500 < JS_MAX; i++) {
        const ec_svc_t *s = &A.cfg.s[i];
        const ec_rt_t *rt = ec_rt(i);
        const ec_hist_t *h = ec_hist(i);
        char addr[EC_URL], a24[16], a7[16];
        jpct(a24, sizeof a24, h && e ? ec_hist_avail5(h, EC_B5_N, e / 300) : -1);
        jpct(a7, sizeof a7, h && e ? ec_hist_availh(h, EC_H_N, e / 3600) : -1);
        o += (size_t)snprintf(js + o, JS_MAX - o, "%s{\"i\":%d,\"name\":", i ? "," : "", i);
        o += jstr(js + o, JS_MAX - o, s->name);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"group\":");
        o += jstr(js + o, JS_MAX - o, s->group);
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"addr\":");
        o += jstr(js + o, JS_MAX - o, ec_svc_addr(s, addr, sizeof addr));
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"err\":\"%s\",\"why\":", ec_err_word(rt->err));
        o += jstr(js + o, JS_MAX - o, rt->why);
        o += (size_t)snprintf(js + o, JS_MAX - o,
                              ",\"tmpl\":\"%s\",\"kind\":\"%s\",\"state\":%d,\"ms\":%d,\"conn\":%d,\"code\":%d,"
                              "\"since\":%lu,\"checked\":%ld,\"fails\":%d,\"paused\":%s,\"trust\":%s,\"a24\":%s,\"a7\":%s",
                              ec_tmpl(s->tmpl)->word, s->kind == EC_TCP ? "tcp" : "http", rt->state, rt->ms, rt->conn_ms,
                              rt->code, (unsigned long)((now - rt->since_ms) / 1000u),
                              rt->checked_ms ? (long)((now - rt->checked_ms) / 1000u) : -1L, rt->fails_row,
                              s->paused ? "true" : "false", s->trust ? "true" : "false", a24, a7);
        if (rt->cert_state == EC_CERT_OK || rt->cert_state == EC_CERT_SOON || rt->cert_state == EC_CERT_EXPIRED) {
            o += (size_t)snprintf(js + o, JS_MAX - o, ",\"cert\":{\"state\":%d,\"days\":%d,\"self\":%s,\"name\":",
                                  rt->cert_state, rt->cert_days, rt->cert_self ? "true" : "false");
            o += jstr(js + o, JS_MAX - o, rt->cert_name);
            int dl = 0;
            o += (size_t)snprintf(js + o, JS_MAX - o, ",\"can_trust\":%s}", ec_rt_der(i, &dl) ? "true" : "false");
        } else if (rt->cert_state == EC_CERT_UNKNOWN) {
            o += (size_t)snprintf(js + o, JS_MAX - o, ",\"cert\":{\"state\":%d}", rt->cert_state);
        }
        /* the last six hours, a five-minute bucket each: the average, -1 none, -2 all failed */
        o += (size_t)snprintf(js + o, JS_MAX - o, ",\"spark\":[");
        for (int k = 71; k >= 0 && o + 16 < JS_MAX; k--) {
            const ec_bk_t *b = h && e ? ec_hist_b5(h, e / 300 - (uint32_t)k) : NULL;
            int v = !b ? -1 : !b->ok ? -2 : b->avg_ms;
            o += (size_t)snprintf(js + o, JS_MAX - o, "%s%d", k == 71 ? "" : ",", v);
        }
        o += (size_t)snprintf(js + o, JS_MAX - o, "]}");
    }
    ec_unlock();
    o += (size_t)snprintf(js + o, JS_MAX - o, "]}");
    if (o >= JS_MAX) o = JS_MAX - 1;
    aos_hal_live_put(EC_APP_ID, "state", "application/json", js, o);

    /* the one the page has open: its whole day, week and outages */
    int i = L.live_detail;
    if (i >= 0 && i < A.cfg.n) {
        o = (size_t)snprintf(js, JS_MAX, "{\"i\":%d,\"now\":%lu,\"b5\":[", i, (unsigned long)e);
        ec_lock();
        const ec_hist_t *h = ec_hist(i);
        for (int k = EC_B5_N - 1; k >= 0 && o + 40 < JS_MAX; k--) {
            const ec_bk_t *b = h && e ? ec_hist_b5(h, e / 300 - (uint32_t)k) : NULL;
            if (b) o += (size_t)snprintf(js + o, JS_MAX - o, "%s[%u,%u,%u,%u]", k == EC_B5_N - 1 ? "" : ",", b->n, b->ok, b->avg_ms, b->max_ms);
            else o += (size_t)snprintf(js + o, JS_MAX - o, "%snull", k == EC_B5_N - 1 ? "" : ",");
        }
        o += (size_t)snprintf(js + o, JS_MAX - o, "],\"h\":[");
        for (int k = EC_H_N - 1; k >= 0 && o + 40 < JS_MAX; k--) {
            const ec_bk_t *b = h && e ? ec_hist_h(h, e / 3600 - (uint32_t)k) : NULL;
            if (b) o += (size_t)snprintf(js + o, JS_MAX - o, "%s[%u,%u,%u,%u]", k == EC_H_N - 1 ? "" : ",", b->n, b->ok, b->avg_ms, b->max_ms);
            else o += (size_t)snprintf(js + o, JS_MAX - o, "%snull", k == EC_H_N - 1 ? "" : ",");
        }
        o += (size_t)snprintf(js + o, JS_MAX - o, "],\"out\":[");
        for (int k = h ? h->out_n - 1 : -1; k >= 0 && o + 60 < JS_MAX; k--)
            o += (size_t)snprintf(js + o, JS_MAX - o, "%s[%lu,%lu,\"%s\"]", k == h->out_n - 1 ? "" : ",",
                                  (unsigned long)h->out[k].start, (unsigned long)h->out[k].end, ec_err_word(h->out[k].err));
        ec_unlock();
        o += (size_t)snprintf(js + o, JS_MAX - o, "]}");
        if (o >= JS_MAX) o = JS_MAX - 1;
        aos_hal_live_put(EC_APP_ID, "detail", "application/json", js, o);
    }
    free(js);
}

static void cfg_check(bool force);

/* "reload", "check 3", "check all", "pause 3 1", "trust 3", "detail 3" */
static void live_take(void)
{
    char msg[AOS_LIVE_MSG_MAX + 1];
    int n;
    while ((n = aos_hal_live_take(EC_APP_ID, msg, sizeof msg - 1)) > 0) {
        msg[n] = '\0';
        char op[16] = "", arg[16] = "";
        int b = 0;
        if (sscanf(msg, "%15s %15s %d", op, arg, &b) < 1) continue;
        int i = atoi(arg);
        if (!strcmp(op, "reload")) cfg_check(true);
        else if (!strcmp(op, "check")) ec_core_check_now(!strcmp(arg, "all") ? -1 : i);
        else if (!strcmp(op, "detail")) L.live_detail = i;
        else if (!strcmp(op, "pause") && i >= 0 && i < A.cfg.n) {
            A.cfg.s[i].paused = b != 0;
            ec_cfg_commit();
            if (!A.hidden && A.screen != EC_SCR_EDIT) ec_build();
        } else if (!strcmp(op, "trust") && i >= 0 && i < A.cfg.n) {
            if (ec_cert_trust(i)) {
                A.cfg.s[i].trust = true;
                ec_cfg_commit();
                ec_core_check_now(i);
            }
        }
    }
}

/* ---- the timer and the life cycle ------------------------------------------------------------- */

static void cfg_check(bool force)
{
    uint32_t stamp = ec_cfg_stamp();
    if (stamp == A.cfg_stamp && !force) return;
    A.cfg_stamp = stamp;
    ec_cfg_load(&A.cfg);
    ec_core_sync(&A.cfg);
    boot_sync();
    if (A.screen == EC_SCR_DETAIL && A.detail >= A.cfg.n) A.screen = EC_SCR_LIST;
    if (!A.hidden && A.screen != EC_SCR_EDIT) ec_build();
    if (ec_desk_on()) ec_desk_relayout();
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (A.hidden) return;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - L.cfg_poll >= CFG_POLL_MS) {
        L.cfg_poll = now;
        if (A.screen != EC_SCR_EDIT) cfg_check(false);
    }
    if (ec_desk_on()) ec_desk_tick();
    else if (A.screen == EC_SCR_LIST) ec_list_tick();
    else if (A.screen == EC_SCR_DETAIL) ec_detail_tick();
}

static void tick(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    notices();
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    if (now - L.live_t >= LIVE_MS) {
        L.live_t = now;
        if (A.hidden && now - L.cfg_poll >= CFG_POLL_MS * 5) {
            L.cfg_poll = now;
            cfg_check(false);
        }
        if (aos_hal_live_idle_ms(EC_APP_ID) < 6000) live_put();
        live_take();
    }
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (ec_desk_on()) {
        ec_desk_close();
        return true;
    }
    if (A.screen == EC_SCR_DETAIL) {
        if (!ec_detail_back()) ec_go_list();
        return true;
    }
    if (A.screen == EC_SCR_EDIT) {
        if (!ec_edit_back()) ec_go_list();
        return true;
    }
    return ec_settings_back();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A.hidden = true;
    ec_desk_close();
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    A.hidden = false;
    cfg_check(false);
    if (A.screen == EC_SCR_LIST) ec_list_tick();
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    A.root = root;
    if (A.screen == EC_SCR_EDIT) ec_edit_keep();
    ec_build();
    if (ec_desk_on()) ec_desk_relayout();
    if (L.note) note_build();
    return true;
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&A, 0, sizeof A);
    memset(&L, 0, sizeof L);
    L.live_detail = -1;
    A.self = self;
    A.root = root;
    ec_i18n_load();
    ec_core_init();
    ec_hist_init();
    ec_cfg_load(&A.cfg);
    A.cfg_stamp = ec_cfg_stamp();
    ec_core_sync(&A.cfg);
    boot_sync();
    L.cfg_poll = (uint32_t)aos_hal_uptime_ms();
    A.screen = EC_SCR_LIST;
    ec_build();
    L.timer = lv_timer_create(timer_cb, TIMER_MS, NULL);

    const char *open = getenv("EC_OPEN");
    if (open && open[0] >= '0' && open[0] <= '9') ec_go_detail(atoi(open));
    const char *edit = getenv("EC_EDIT");
    if (edit && edit[0]) ec_go_edit(!strcmp(edit, "new") ? -1 : atoi(edit));
    const char *desk = getenv("EC_DESK");
    if (desk && desk[0] == '1') ec_desk_open();
    return &A;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (L.timer) {
        lv_timer_delete(L.timer);
        L.timer = NULL;
    }
    ec_desk_close();
    note_close();
    if (A.screen == EC_SCR_EDIT) ec_edit_close();
    lv_obj_clean(A.root);
    /* the loader unloads the .so on the next tick: no thread may be left in it */
    ec_core_stop(STOP_WAIT_MS);
    ec_hist_save_if(true);
    ec_hist_free();
    aos_hal_live_clear(EC_APP_ID);
    aos_ui_set_badge(EC_APP_ID, 0);
    if (L.icon_on) aos_ui_status_icon_set(EC_APP_ID, NULL, 0);
}

static bool ec_init(aos_app_t *app)
{
    app->desc.id       = "aos.estado";      /* literal: gen_lang.py looks for it here */
    app->desc.name     = "Estado de la casa";
    app->desc.icon     = AOS_SYM_SERVER_NETWORK;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x34D399;
    app->desc.color_b  = 0x047857;
    app->desc.order    = 134;
    app->desc.flags    = AOS_APP_FLAG_BACKGROUND;
    aos_icon_set_ops(app, EC_ICON, sizeof EC_ICON);
    app->create  = create;
    app->destroy = destroy;
    app->back    = back;
    app->show    = show;
    app->hide    = hide;
    app->tick    = tick;
    app->resize  = resize;
    return true;
}

AOS_APP_ENTRY(ec_init);
