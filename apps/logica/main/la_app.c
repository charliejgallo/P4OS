/*
 * P4OS - Logic: the app (aos.logica), its settings and its timers.
 *
 * The settings (rate, depth, trigger, channel names, decoders) live in a
 * text file in the data folder, logica.txt, one key=value a line, which the
 * portal's page also changes through the live channel (la_live.c). The
 * captures go to <card>/logica as sigrok sessions.
 *
 * Two timers: the view's (30 ms, it only draws when something moved) and
 * the app's (100 ms: the status line, the jobs that ended, repeating a
 * capture, the USB devices coming and going, the portal).
 */
#include "la.h"

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_icon_ops.h"
#include "aos_sys_glyphs.h"
#include "aos_ui.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef AOS_SIM
#include "esp_heap_caps.h"
#endif

la_t LA;

/* A square wave over a bus: the launcher's icon. */
static const uint8_t LA_ICON[] = {
    AIC_HEADER,
    AIC_RECT(AIC_CENTER, -27, -14, 16, 4, 1, AIC_C_TEXT, 255),     /* high */
    AIC_RECT(AIC_CENTER, -19, -4, 4, 24, 1, AIC_C_TEXT, 255),      /* edge */
    AIC_RECT(AIC_CENTER, -9, 6, 24, 4, 1, AIC_C_TEXT, 255),        /* low */
    AIC_RECT(AIC_CENTER, 1, -4, 4, 24, 1, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 11, -14, 24, 4, 1, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 21, -4, 4, 24, 1, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, 28, 6, 14, 4, 1, AIC_C_TEXT, 255),
    AIC_RECT(AIC_CENTER, -16, 22, 26, 10, 3, AIC_C_YELLOW, 255),   /* decoded bytes */
    AIC_RECT(AIC_CENTER, 14, 22, 26, 10, 3, AIC_C_YELLOW, 160),
    AIC_END
};

static void *s_mx;
static lv_timer_t *s_timer, *s_view_timer;
static uint32_t s_job_seen;
static bool s_was_capture;
static uint32_t s_again_at;
static int s_ndev_seen = -1;
static uint32_t s_usb_sig;

void la_lock(void)
{
    if (s_mx) aos_hal_mutex_lock(s_mx);
}

void la_unlock(void)
{
    if (s_mx) aos_hal_mutex_unlock(s_mx);
}

void la_set_msg(bool err, const char *fmt, ...)
{
    char b[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    la_lock();
    snprintf(LA.msg, sizeof LA.msg, "%s", b);
    LA.msg_err = err;
    la_unlock();
}

void *la_big_alloc(size_t n)
{
#ifdef AOS_SIM
    return malloc(n ? n : 1);
#else
    return heap_caps_malloc(n ? n : 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#endif
}

void la_big_free(void *p)
{
#ifdef AOS_SIM
    free(p);
#else
    heap_caps_free(p);
#endif
}

uint32_t la_ms(void) { return (uint32_t)(aos_hal_uptime_us() / 1000); }

const char *la_dir(void)
{
    static char d[64];
    const char *root = aos_hal_path_sd_root();
    if (!root) return NULL;
    snprintf(d, sizeof d, "%s/logica", root);
    mkdir(d, 0775);
    return d;
}

/* ---- settings ---- */

static const char TRIG_CH[LA_T_COUNT] = { 'x', '0', '1', 'r', 'f', 'e' };

static void cfg_path(char *out, size_t n) { snprintf(out, n, "%s/logica.txt", aos_hal_path_data()); }

static void cfg_defaults(void)
{
    LA.cfg.rate = 8000000;
    LA.cfg.samples = 1000000;
    LA.cfg.pre = 10;
    memset(LA.cfg.trig, LA_T_X, sizeof LA.cfg.trig);
    LA.cfg.repeat = false;
    LA.cfg.demo = 0;
    la_demo_decoders(0);
}

/* one line of the settings, also what the portal sends */
bool la_cfg_line(const char *line)
{
    char k[16];
    const char *eq = strchr(line, '=');
    if (!eq || eq - line >= (int)sizeof k) return false;
    memcpy(k, line, (size_t)(eq - line));
    k[eq - line] = 0;
    const char *v = eq + 1;
    if (!strcmp(k, "rate")) LA.cfg.rate = (uint32_t)strtoul(v, NULL, 10);
    else if (!strcmp(k, "samples")) LA.cfg.samples = (uint32_t)strtoul(v, NULL, 10);
    else if (!strcmp(k, "pre")) LA.cfg.pre = (uint8_t)(atoi(v) > 95 ? 95 : atoi(v) < 0 ? 0 : atoi(v));
    else if (!strcmp(k, "repeat")) LA.cfg.repeat = atoi(v) != 0;
    else if (!strcmp(k, "demo")) LA.cfg.demo = (uint8_t)(atoi(v) % la_demo_scenes());
    else if (!strcmp(k, "trig")) {
        for (int i = 0; i < LA_NCH && v[i]; i++) {
            const char *p = memchr(TRIG_CH, v[i], LA_T_COUNT);
            LA.cfg.trig[i] = p ? (uint8_t)(p - TRIG_CH) : LA_T_X;
        }
    } else if (!strcmp(k, "show")) {
        for (int i = 0; i < LA_NCH && v[i]; i++) LA.chshow[i] = v[i] == '1';
    } else if (!strncmp(k, "name", 4) && k[4] >= '0' && k[4] < '0' + LA_NCH && !k[5]) {
        int i = k[4] - '0';
        size_t j = 0;
        for (; v[j] && v[j] != '\n' && v[j] != '\r' && j < sizeof LA.chname[i] - 1; j++) LA.chname[i][j] = v[j];
        LA.chname[i][j] = 0;
    } else if (!strncmp(k, "dec", 3) && k[3] >= '0' && k[3] < '0' + LA_DEC_MAX && !k[4]) {
        la_dec_cfg_t *d = &LA.dec[k[3] - '0'];
        int f[9 + 10];
        int n = 0;
        const char *p = v;
        while (*p && n < (int)(sizeof f / sizeof f[0])) {
            f[n++] = (int)strtol(p, (char **)&p, 10);
            if (*p == ',') p++;
            else break;
        }
        if (n < 19 || f[0] < 0 || f[0] >= LA_DEC_COUNT) return false;
        la_dec_defaults(d, f[0]);
        d->on = f[1] != 0;
        for (int i = 0; i < LA_DEC_PINS; i++) d->ch[i] = (int8_t)(f[2 + i] >= -1 && f[2 + i] < LA_NCH ? f[2 + i] : -1);
        d->baud = (uint32_t)f[11];
        d->bits = (uint8_t)f[12];
        d->parity = f[13] == 'E' || f[13] == 'O' ? (char)f[13] : 'N';
        d->stop = f[14] == 2 ? 2 : 1;
        d->mode = (uint8_t)(f[15] & 3);
        d->lsb = f[16] != 0;
        d->inv = f[17] != 0;
        d->fmt = (uint8_t)(f[18] < LA_FMT_COUNT && f[18] >= 0 ? f[18] : 0);
    } else {
        return false;
    }
    return true;
}

int la_cfg_text(char *out, size_t n)
{
    size_t k = 0;
    char trig[LA_NCH + 1], show[LA_NCH + 1];
    for (int i = 0; i < LA_NCH; i++) {
        trig[i] = TRIG_CH[LA.cfg.trig[i] < LA_T_COUNT ? LA.cfg.trig[i] : 0];
        show[i] = LA.chshow[i] ? '1' : '0';
    }
    trig[LA_NCH] = show[LA_NCH] = 0;
    k += (size_t)snprintf(out + k, n - k, "rate=%u\nsamples=%u\npre=%u\nrepeat=%d\ndemo=%u\ntrig=%s\nshow=%s\n",
                          (unsigned)LA.cfg.rate, (unsigned)LA.cfg.samples, LA.cfg.pre, LA.cfg.repeat, LA.cfg.demo,
                          trig, show);
    for (int i = 0; i < LA_NCH && k < n; i++) k += (size_t)snprintf(out + k, n - k, "name%d=%s\n", i, LA.chname[i]);
    for (int r = 0; r < LA_DEC_MAX && k < n; r++) {
        const la_dec_cfg_t *d = &LA.dec[r];
        k += (size_t)snprintf(out + k, n - k, "dec%d=%d,%d", r, d->type, d->on);
        for (int i = 0; i < LA_DEC_PINS && k < n; i++) k += (size_t)snprintf(out + k, n - k, ",%d", d->ch[i]);
        if (k < n)
            k += (size_t)snprintf(out + k, n - k, ",%u,%d,%d,%d,%d,%d,%d,%d\n", (unsigned)d->baud, d->bits, d->parity,
                                  d->stop, d->mode, d->lsb, d->inv, d->fmt);
    }
    return k < n ? (int)k : (int)n - 1;
}

void la_cfg_save(void)
{
    char *b = malloc(2048);
    if (!b) return;
    la_cfg_text(b, 2048);
    char p[160];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "w");
    if (f) {
        fputs(b, f);
        fclose(f);
    }
    free(b);
}

static void cfg_load(void)
{
    cfg_defaults();
    char p[160];
    cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        la_cfg_line(line);
    }
    fclose(f);
}

/* ---- timers ---- */

static void view_timer_cb(lv_timer_t *t)
{
    (void)t;
    la_view_tick();
}

/* a cheap signature of the host's devices: rescan when it changes */
static uint32_t usb_sig(void)
{
    aos_usb_dev_t *d = malloc(12 * sizeof *d);
    if (!d) return 0;
    int n = aos_hal_usb_devices(d, 12);
    uint32_t s = (uint32_t)n * 2654435761u;
    for (int i = 0; i < n; i++) s = (s ^ ((uint32_t)d[i].addr << 24 | (uint32_t)d[i].vid << 8 | d[i].pid)) * 16777619u;
    free(d);
    return s;
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    static uint32_t ticks;
    ticks++;
    la_lock();
    uint32_t done = LA.job_done;
    bool busy = LA.busy;
    la_unlock();
    if (done != s_job_seen) {
        s_job_seen = done;
        la_ui_after_job();
        la_view_dirty();
        if (s_was_capture && LA.cfg.repeat && !LA.stop && LA.cap_ok && !LA.msg_err) s_again_at = la_ms() + 250;
        s_was_capture = false;
    }
    if (s_again_at && !busy && (int32_t)(la_ms() - s_again_at) >= 0) {
        s_again_at = 0;
        if (LA.cfg.repeat && la_capture_start()) s_was_capture = true;
    }
    if (!busy && (la_rescan || ticks % 20 == 0)) {
        uint32_t sig = usb_sig();
        if (la_rescan || sig != s_usb_sig) {
            la_rescan = false;
            s_usb_sig = sig;
            la_devs_scan(false);
            if (LA.ndevs != s_ndev_seen) s_ndev_seen = LA.ndevs;
            la_ui_status();
        }
    }
    if (busy || ticks % 3 == 0) la_ui_status();
    /* a capture waiting for its trigger, or repeating, goes on behind
     * another app: the shell must not close this one for its PSRAM */
    static bool was_busy;
    bool now_busy = busy || s_again_at;
    if (now_busy != was_busy) {
        was_busy = now_busy;
        aos_ui_set_busy(LA_APP_ID, now_busy);
    }
    la_live_tick();
}

bool la_capture_ui(void)
{
    s_again_at = 0;
    if (la_capture_start()) {
        s_was_capture = true;
        la_view_dirty();
        return true;
    }
    return false;
}

void la_capture_cancel_repeat(void) { s_again_at = 0; }

/* ---- the app ---- */

static void *create(aos_app_t *self, lv_obj_t *root)
{
    (void)self;
    if (!s_mx) s_mx = aos_hal_mutex_create();
    if (!LA.cfg.rate) cfg_load();
    s_usb_sig = usb_sig();
    la_devs_scan(false);
    la_ui_build(root);
    s_timer = lv_timer_create(timer_cb, 100, NULL);
    s_view_timer = lv_timer_create(view_timer_cb, 30, NULL);
    s_job_seen = LA.job_done;
    return &LA;
}

static void destroy(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_timer) lv_timer_delete(s_timer);
    if (s_view_timer) lv_timer_delete(s_view_timer);
    s_timer = s_view_timer = NULL;
    s_again_at = 0;
    la_job_wait();
    aos_ui_set_busy(LA_APP_ID, false);
    la_cfg_save();
    la_live_clear();
    la_lock();
    LA.cap_ok = LA.an_ok = false;
    la_unlock();
    la_cap_free(&LA.cap);
    la_cap_free(&LA.nxt);
    la_anns_free();
    LA.ndevs = 0;
    LA.cfg.rate = 0;            /* read again at the next open */
}

static bool resize(aos_app_t *self, void *inst, lv_obj_t *root)
{
    (void)self;
    (void)inst;
    la_ui_build(root);
    return true;
}

static void show(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_timer) lv_timer_resume(s_timer);
    if (s_view_timer) lv_timer_resume(s_view_timer);
    la_view_dirty();
}

static void hide(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (s_view_timer) lv_timer_pause(s_view_timer);
    la_cfg_save();
}

static bool back(aos_app_t *self, void *inst)
{
    (void)self;
    (void)inst;
    if (la_sheet_is_open()) {
        la_sheet_close();
        return true;
    }
    return false;
}

static bool button(aos_app_t *self, void *inst, int action)
{
    (void)self;
    (void)inst;
    /* BOOT: capture, or stop the one running */
    if (action != AOS_BUTTON_CLICK) return action == AOS_BUTTON_PRESS;
    if (LA.busy) {
        la_capture_cancel_repeat();
        la_job_stop();
    } else {
        la_capture_ui();
    }
    return true;
}

static bool init(aos_app_t *app)
{
    app->desc.id       = "aos.logica";
    app->desc.name     = "Lógica";
    app->desc.icon     = AOS_SYM_SQUARE_WAVE;
    app->desc.icon_vec = AOS_ICON_NONE;
    app->desc.color_a  = 0x2E7D32;
    app->desc.color_b  = 0x103A14;
    app->desc.flags    = AOS_APP_FLAG_KEEP | AOS_APP_FLAG_KEEP_AWAKE;
    app->desc.order    = 121;
    aos_icon_set_ops(app, LA_ICON, sizeof LA_ICON);

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
