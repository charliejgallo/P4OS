/*
 * P4OS - the lock screen (aos_lock.h).
 *
 * A layer over everything but the status bar and the system's own sheets:
 * the wallpaper's gradient, the time and date large, the last messages,
 * the music with its three buttons, and at the bottom a hint to swipe up.
 * The layer follows the finger up; let go past a quarter of the screen (or
 * flicked) it unlocks, or brings up the code pad when there is a code.
 *
 * The gesture is a free swipe up from anywhere, as phones do it today, not
 * a control dragged along a track (docs: the 2005 patent on that was the
 * reason to choose this one).
 *
 * When it locks: the screen off for the chosen time, checked from the tick
 * while it is still dark, so waking shows the lock screen and never a
 * frame of what was under it; and at boot. Wrong codes: after 5 the pad
 * waits 30 s, then twice as long each further 3; the count survives a
 * restart. In the BOOT button's safe mode the code is not asked for, so a
 * forgotten one can be removed from Settings.
 */
#include "aos_lock.h"
#include "aos_ui.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_internal.h"
#include "aos_sys_glyphs.h"
#include "aos_fonts.h"
#include "aos_text_safe.h"
#include "mbedtls/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FAIL_FREE    5          /* wrong codes before the pad makes you wait */
#define FAIL_STEP    3
#define WAIT_FIRST_S 30

static aos_lock_cfg_t s_cfg = { .enabled = false, .after_s = 0, .show_notifs = true, .show_music = true,
                                .hide_content = true };
static bool s_locked, s_inited;
static uint64_t s_off_since;    /* when the screen went dark, 0 while it is on */
static int s_fails;
static uint64_t s_wait_until;

static struct {
    lv_obj_t *layer, *time, *date, *msgs, *music, *m_title, *m_artist, *m_play, *hint, *chev;
    int32_t drag0;
    bool dragging;
    int notif_n;
    uint32_t notif_sig;
    int music_state;
} L;

/* the pad, shared with Settings */
static struct {
    lv_obj_t *sheet, *title, *dots[6], *msg;
    char buf[8];
    int len, n;
    aos_lock_pad_cb done;
    void (*cancel)(void *);
    void *ud;
} P;

static void build(void);
static void unlock(void);

/* ---- the configuration and the code, in the preferences ---- */

static void cfg_load(void)
{
    int32_t v;
    if (aos_hal_pref_get_i32("lock_on", &v)) s_cfg.enabled = v != 0;
    if (aos_hal_pref_get_i32("lock_after", &v) && v >= 0) s_cfg.after_s = (uint32_t)v;
    if (aos_hal_pref_get_i32("lock_notifs", &v)) s_cfg.show_notifs = v != 0;
    if (aos_hal_pref_get_i32("lock_music", &v)) s_cfg.show_music = v != 0;
    if (aos_hal_pref_get_i32("lock_hide", &v)) s_cfg.hide_content = v != 0;
    if (aos_hal_pref_get_i32("lock_fails", &v) && v >= 0) s_fails = (int)v;
}

void aos_lock_get_cfg(aos_lock_cfg_t *out) { *out = s_cfg; }

void aos_lock_set_cfg(const aos_lock_cfg_t *c)
{
    s_cfg = *c;
    aos_hal_pref_set_i32("lock_on", c->enabled);
    aos_hal_pref_set_i32("lock_after", (int32_t)c->after_s);
    aos_hal_pref_set_i32("lock_notifs", c->show_notifs);
    aos_hal_pref_set_i32("lock_music", c->show_music);
    aos_hal_pref_set_i32("lock_hide", c->hide_content);
}

static void hash_hex(const uint8_t salt[8], const char *pin, char out[65])
{
    uint8_t h[32];
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    mbedtls_sha256_update(&c, salt, 8);
    mbedtls_sha256_update(&c, (const uint8_t *)pin, strlen(pin));
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", h[i]);
}

int aos_lock_pin_len(void)
{
    int32_t n = 0;
    char v[96];
    if (!aos_hal_pref_get_str("lock_pin", v, sizeof v) || strlen(v) < 17 + 64) return 0;
    aos_hal_pref_get_i32("lock_len", &n);
    return n == 6 ? 6 : 4;
}

bool aos_lock_pin_check(const char *pin)
{
    char v[96], h[65];
    if (!pin || !aos_hal_pref_get_str("lock_pin", v, sizeof v) || strlen(v) < 17 + 64 || v[16] != ':') return false;
    uint8_t salt[8];
    for (int i = 0; i < 8; i++) {
        unsigned b;
        if (sscanf(v + 2 * i, "%2x", &b) != 1) return false;
        salt[i] = (uint8_t)b;
    }
    hash_hex(salt, pin, h);
    /* every byte compared, not stopping at the first difference */
    unsigned diff = 0;
    for (int i = 0; i < 64; i++) diff |= (unsigned)(h[i] ^ v[17 + i]);
    return diff == 0;
}

void aos_lock_pin_set(const char *pin)
{
    if (!pin || !pin[0]) {
        aos_hal_pref_erase("lock_pin");
        aos_hal_pref_erase("lock_len");
        return;
    }
    uint8_t salt[8];
    for (int i = 0; i < 8; i++) salt[i] = (uint8_t)lv_rand(0, 255);
    salt[0] ^= (uint8_t)aos_hal_uptime_ms();
    char v[96], h[65];
    for (int i = 0; i < 8; i++) snprintf(v + 2 * i, 3, "%02x", salt[i]);
    hash_hex(salt, pin, h);
    v[16] = ':';
    memcpy(v + 17, h, 65);
    aos_hal_pref_set_str("lock_pin", v);
    aos_hal_pref_set_i32("lock_len", (int32_t)strlen(pin));
    s_fails = 0;
    aos_hal_pref_set_i32("lock_fails", 0);
}

/* ---- the code pad ---- */

static void pad_dots(void)
{
    for (int i = 0; i < 6; i++) {
        if (!P.dots[i]) continue;
        lv_obj_set_flag(P.dots[i], LV_OBJ_FLAG_HIDDEN, i >= P.len);
        lv_obj_set_style_bg_opa(P.dots[i], i < P.n ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    }
}

static void shake_x(void *o, int32_t v) { lv_obj_set_style_translate_x(o, v, 0); }

static uint32_t wait_left_s(void)
{
    uint64_t now = aos_hal_uptime_ms();
    return s_wait_until > now ? (uint32_t)((s_wait_until - now + 999) / 1000) : 0;
}

static void pad_msg_refresh(void)
{
    if (!P.msg) return;
    uint32_t w = wait_left_s();
    if (!w) return;
    char t[80];
    snprintf(t, sizeof t, _("Demasiados intentos: esperá %u s"), (unsigned)w);
    lv_label_set_text(P.msg, t);
}

static void pad_key_cb(lv_event_t *e)
{
    int k = (int)(intptr_t)lv_event_get_user_data(e);
    if (k == -2) {                                  /* cancel */
        void (*cancel)(void *) = P.cancel;
        void *ud = P.ud;
        aos_lock_pad_close();
        if (cancel) cancel(ud);
        return;
    }
    if (wait_left_s()) { pad_msg_refresh(); return; }
    if (k == -1) {                                  /* delete */
        if (P.n) P.n--;
        pad_dots();
        return;
    }
    if (P.n >= P.len) return;
    P.buf[P.n++] = (char)('0' + k);
    P.buf[P.n] = 0;
    pad_dots();
    if (P.n < P.len) return;
    aos_lock_pad_cb done = P.done;
    void *ud = P.ud;
    lv_obj_t *sheet = P.sheet;
    char pin[8];
    memcpy(pin, P.buf, sizeof pin);
    P.n = 0;
    memset(P.buf, 0, sizeof P.buf);
    bool ok = done(pin, ud);
    memset(pin, 0, sizeof pin);
    /* the callback may have closed this sheet, or opened the next step's
     * ("repeat the code"): then this one has nothing more to do */
    if (P.sheet != sheet) return;
    if (ok) {
        aos_lock_pad_close();
        return;
    }
    /* wrong: the dots shake and empty */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, P.dots[0] ? lv_obj_get_parent(P.dots[0]) : P.sheet);
    lv_anim_set_exec_cb(&a, shake_x);
    lv_anim_set_values(&a, -24, 0);
    lv_anim_set_duration(&a, 380);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
    pad_dots();
}

void aos_lock_pad_close(void)
{
    /* async: it is usually closed from one of its own keys' events */
    if (P.sheet) lv_obj_delete_async(P.sheet);
    memset(&P, 0, sizeof P);
}

void aos_lock_pad_open(lv_obj_t *parent, const char *title, int len, aos_lock_pad_cb done, void (*cancel)(void *),
                       void *ud)
{
    aos_lock_pad_close();
    const aos_geo_t *g = aos_ui_geo();
    P.len = len == 6 ? 6 : 4;
    P.done = done;
    P.cancel = cancel;
    P.ud = ud;
    P.sheet = lv_obj_create(parent);
    lv_obj_remove_style_all(P.sheet);
    lv_obj_set_size(P.sheet, g->w, g->h);
    lv_obj_set_style_bg_color(P.sheet, lv_color_hex(0x0B0B10), 0);
    lv_obj_set_style_bg_opa(P.sheet, 245, 0);
    lv_obj_add_flag(P.sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(P.sheet, LV_OBJ_FLAG_SCROLLABLE);

    int32_t key = g->landscape ? 96 : 150, gap = g->landscape ? 18 : 28;
    int32_t padw = 3 * key + 2 * gap;
    /* landscape: the title and dots on the left, the keys on the right */
    int32_t kx = g->landscape ? g->w / 2 + (g->w / 2 - padw) / 2 : (g->w - padw) / 2;
    int32_t ky = g->landscape ? (g->h - (4 * key + 3 * gap)) / 2 : g->h - (4 * key + 3 * gap) - 110;
    int32_t tx = g->landscape ? g->w / 4 : g->w / 2, ty = g->landscape ? g->h / 2 - 80 : ky - 260;

    P.title = aos_label(P.sheet, title, aos_font_title, AOS_C_TEXT);
    lv_obj_set_style_text_align(P.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(P.title, g->landscape ? g->w / 2 - 40 : g->w - 80);
    lv_label_set_long_mode(P.title, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_pos(P.title, tx - lv_obj_get_style_width(P.title, 0) / 2, ty);

    lv_obj_t *dots = lv_obj_create(P.sheet);
    lv_obj_remove_style_all(dots);
    lv_obj_set_size(dots, LV_SIZE_CONTENT, 40);
    lv_obj_set_flex_flow(dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(dots, 28, 0);
    for (int i = 0; i < 6; i++) {
        lv_obj_t *d = lv_obj_create(dots);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 28, 28);
        lv_obj_set_style_radius(d, 14, 0);
        lv_obj_set_style_border_color(d, lv_color_white(), 0);
        lv_obj_set_style_border_width(d, 3, 0);
        lv_obj_set_style_bg_color(d, lv_color_white(), 0);
        P.dots[i] = d;
    }
    pad_dots();
    lv_obj_update_layout(dots);
    lv_obj_set_pos(dots, tx - lv_obj_get_width(dots) / 2, ty + 100);

    P.msg = aos_label(P.sheet, "", aos_font_small, AOS_C_ORANGE);
    lv_obj_set_width(P.msg, g->landscape ? g->w / 2 - 40 : g->w - 80);
    lv_obj_set_style_text_align(P.msg, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(P.msg, tx - lv_obj_get_style_width(P.msg, 0) / 2, ty + 170);

    static const char *const LBL[12] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "", "0", "" };
    for (int i = 0; i < 12; i++) {
        int digit = i == 10 ? 0 : i + 1;
        bool del = i == 11, cancel_key = i == 9;
        if (cancel_key && !cancel) continue;
        lv_obj_t *b = lv_obj_create(P.sheet);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, key, key);
        lv_obj_set_pos(b, kx + (i % 3) * (key + gap), ky + (i / 3) * (key + gap));
        lv_obj_set_style_radius(b, key / 2, 0);
        lv_obj_set_style_bg_color(b, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, (del || cancel_key) ? LV_OPA_TRANSP : LV_OPA_20, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_50, LV_STATE_PRESSED);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, pad_key_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(del ? -1 : cancel_key ? -2 : digit));
        lv_obj_t *l = aos_label(b, del ? AOS_SYM_BACKSPACE_OUTLINE : cancel_key ? _("Cancelar") : LBL[i],
                                del ? &aos_sym_44 : cancel_key ? aos_font_small : aos_font_large, AOS_C_TEXT);
        lv_obj_center(l);
    }
    pad_msg_refresh();
}

/* ---- the lock screen ---- */

static bool unlock_pin_cb(const char *pin, void *ud)
{
    (void)ud;
    if (aos_lock_pin_check(pin)) {
        s_fails = 0;
        aos_hal_pref_set_i32("lock_fails", 0);
        unlock();
        return true;
    }
    s_fails++;
    aos_hal_pref_set_i32("lock_fails", s_fails);
    if (s_fails >= FAIL_FREE) {
        int steps = (s_fails - FAIL_FREE) / FAIL_STEP;
        uint32_t w = WAIT_FIRST_S << (steps > 6 ? 6 : steps);
        s_wait_until = aos_hal_uptime_ms() + (uint64_t)w * 1000;
    }
    if (P.msg) {
        if (wait_left_s()) pad_msg_refresh();
        else lv_label_set_text(P.msg, _("Código incorrecto"));
    }
    return false;
}

static void pad_cancel_cb(void *ud) { (void)ud; }

static void slide_y(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }
static void slide_done(lv_anim_t *a)
{
    (void)a;
    if (L.layer) {
        lv_obj_delete(L.layer);
        memset(&L, 0, sizeof L);
    }
    aos_hal_activity();
}

static void unlock(void)
{
    s_locked = false;
    aos_lock_pad_close();
    if (!L.layer) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, L.layer);
    lv_anim_set_exec_cb(&a, slide_y);
    lv_anim_set_values(&a, lv_obj_get_style_translate_y(L.layer, 0), -aos_ui_geo()->h);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, slide_done);
    lv_anim_start(&a);
}

/* the swipe: the layer follows the finger up; let go past a quarter of the
 * height, or faster than a flick, it goes */
static void layer_ev(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    lv_indev_t *in = lv_indev_active();
    if (!in || P.sheet) return;
    lv_point_t p;
    lv_indev_get_point(in, &p);
    if (c == LV_EVENT_PRESSED) {
        L.drag0 = p.y;
        L.dragging = true;
    } else if (c == LV_EVENT_PRESSING && L.dragging) {
        int32_t up = L.drag0 - p.y;
        lv_obj_set_style_translate_y(L.layer, up > 0 ? -up : 0, 0);
        aos_hal_activity();
    } else if ((c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) && L.dragging) {
        L.dragging = false;
        int32_t up = L.drag0 - p.y;
        lv_point_t v;
        lv_indev_get_vect(in, &v);
        if (up > aos_ui_geo()->h / 4 || (up > 60 && v.y < -18)) {
            if (aos_lock_pin_len() && !aos_ui_safe_mode()) {
                lv_obj_set_style_translate_y(L.layer, 0, 0);
                aos_lock_pad_open(L.layer, _("Ingresá el código"), aos_lock_pin_len(), unlock_pin_cb, pad_cancel_cb, NULL);
                return;
            }
            unlock();
            return;
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, L.layer);
        lv_anim_set_exec_cb(&a, slide_y);
        lv_anim_set_values(&a, lv_obj_get_style_translate_y(L.layer, 0), 0);
        lv_anim_set_duration(&a, 220);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

/* the board's player or the iPhone's, whichever the row shows (aos_nowplaying.c) */
static void music_prev_cb(lv_event_t *e) { aos_np_command(AOS_MEDIA_PREV); }
static void music_next_cb(lv_event_t *e) { aos_np_command(AOS_MEDIA_NEXT); }
static void music_play_cb(lv_event_t *e) { aos_np_command(AOS_MEDIA_PLAY_PAUSE); }

static lv_obj_t *round_btn(lv_obj_t *parent, const char *glyph, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 88, 88);
    lv_obj_set_style_radius(b, 44, 0);
    lv_obj_set_style_bg_color(b, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_30, LV_STATE_PRESSED);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = aos_label(b, glyph, &aos_sym_44, AOS_C_TEXT);
    lv_obj_center(l);
    return b;
}

static lv_obj_t *glass(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_40, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    lv_obj_set_style_pad_all(c, 22, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void clock_refresh(void)
{
    if (!L.time) return;
    struct tm t;
    aos_hal_time_now(&t);
    char s[16];
    if (aos_hal_time_is_valid()) snprintf(s, sizeof s, "%d:%02d", t.tm_hour, t.tm_min);
    else snprintf(s, sizeof s, "--:--");
    if (strcmp(lv_label_get_text(L.time), s)) lv_label_set_text(L.time, s);
    static const char *const DAY[7] = { N_("domingo"), N_("lunes"), N_("martes"), N_("miércoles"), N_("jueves"),
                                        N_("viernes"), N_("sábado") };
    static const char *const MON[12] = { N_("enero"), N_("febrero"), N_("marzo"), N_("abril"), N_("mayo"), N_("junio"),
                                         N_("julio"), N_("agosto"), N_("septiembre"), N_("octubre"), N_("noviembre"),
                                         N_("diciembre") };
    char d[64];
    if (aos_hal_time_is_valid())
        snprintf(d, sizeof d, _("%s %d de %s"), aos_tr(DAY[t.tm_wday % 7]), t.tm_mday, aos_tr(MON[t.tm_mon % 12]));
    else
        snprintf(d, sizeof d, "%s", _("sin hora todavía"));
    if (strcmp(lv_label_get_text(L.date), d)) lv_label_set_text(L.date, d);
}

static void msgs_refresh(bool force)
{
    if (!L.msgs) return;
    int n = aos_hal_notif_count();
    uint32_t sig = (uint32_t)n;
    aos_notif_t nt;
    if (n > 0 && aos_hal_notif_at(0, &nt)) sig = sig * 31u + nt.uid;
    if (!force && sig == L.notif_sig) return;
    L.notif_sig = sig;
    lv_obj_clean(L.msgs);
    lv_obj_set_flag(L.msgs, LV_OBJ_FLAG_HIDDEN, n <= 0);
    bool hide = s_cfg.hide_content && aos_lock_pin_len() > 0;
    int shown = 0;
    for (int i = 0; i < n && shown < 4; i++) {
        if (!aos_hal_notif_at(i, &nt)) continue;
        lv_obj_t *row = lv_obj_create(L.msgs);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 2, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *a = aos_label(row, nt.app[0] ? nt.app : _("Mensaje"), aos_font_caption, AOS_C_DIM);
        (void)a;
        /* the phone's text through the same filter as the banners: emoji
         * the font lacks become words, line breaks spaces */
        char safe[256];
        aos_text_safe(safe, sizeof safe, nt.title);
        lv_obj_t *t = aos_label(row, hide ? _("Notificación") : safe, aos_font_body, AOS_C_TEXT);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_DOTS);
        if (!hide && nt.message[0]) {
            aos_text_safe(safe, sizeof safe, nt.message);
            for (char *c = safe; *c; c++) if (*c == '\n' || *c == '\r') *c = ' ';
            lv_obj_t *m = aos_label(row, safe, aos_font_small, lv_color_hex(0xD0D3DA));
            lv_obj_set_width(m, lv_pct(100));
            lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_DOTS);
        }
        shown++;
    }
    if (n > shown) {
        char more[48];
        snprintf(more, sizeof more, _("y %d más"), n - shown);
        aos_label(L.msgs, more, aos_font_caption, AOS_C_DIM);
    }
}

static void music_refresh(void)
{
    if (!L.music) return;
    aos_np_t np;
    bool on = s_cfg.show_music && aos_np_get(&np);
    lv_obj_set_flag(L.music, LV_OBJ_FLAG_HIDDEN, !on);
    if (!on) return;
    char safe[192];
    aos_text_safe(safe, sizeof safe, np.title[0] ? np.title : np.from[0] ? np.from : _("Música"));
    if (strcmp(lv_label_get_text(L.m_title), safe)) lv_label_set_text(L.m_title, safe);
    /* the phone's says which app plays it */
    if (np.phone && np.title[0]) snprintf(safe, sizeof safe, "%s%s%s", np.artist, np.artist[0] ? "  ·  " : "", np.from);
    else aos_text_safe(safe, sizeof safe, np.artist);
    if (strcmp(lv_label_get_text(L.m_artist), safe)) lv_label_set_text(L.m_artist, safe);
    if ((int)np.playing != L.music_state) {
        L.music_state = (int)np.playing;
        lv_label_set_text(lv_obj_get_child(L.m_play, 0), np.playing ? AOS_SYM_PAUSE : AOS_SYM_PLAY);
    }
}

static void hint_breathe(void *o, int32_t v) { lv_obj_set_style_opa(o, (lv_opa_t)v, 0); }

static void build(void)
{
    if (L.layer) lv_obj_delete(L.layer);
    memset(&L, 0, sizeof L);
    L.music_state = -1;
    const aos_geo_t *g = aos_ui_geo();
    L.layer = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(L.layer);
    lv_obj_set_size(L.layer, g->w, g->h);
    uint32_t top, bottom;
    aos_ui_wallpaper_colors(aos_ui_wallpaper(), &top, &bottom);
    lv_obj_set_style_bg_color(L.layer, lv_color_hex(top), 0);
    lv_obj_set_style_bg_grad_color(L.layer, lv_color_hex(bottom), 0);
    lv_obj_set_style_bg_grad_dir(L.layer, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_opa(L.layer, LV_OPA_COVER, 0);
    lv_obj_add_flag(L.layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(L.layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(L.layer, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(L.layer, layer_ev, LV_EVENT_ALL, NULL);

    /* the clock: big, under the status bar; in landscape on the left, the
     * messages on the right */
    int32_t colw = g->landscape ? g->w / 2 - 60 : g->w - 2 * AOS_UI_PAD;
    int32_t cx = g->landscape ? 40 : AOS_UI_PAD;
    L.time = aos_label(L.layer, "", g->landscape ? &aos_inter_num_96 : &aos_inter_num_144, lv_color_white());
    lv_obj_set_width(L.time, colw);
    lv_obj_set_style_text_align(L.time, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(L.time, cx, g->landscape ? g->h / 2 - 130 : g->bar_h + 70);
    L.date = aos_label(L.layer, "", aos_font_title, lv_color_hex(0xE8EAF0));
    lv_obj_set_width(L.date, colw);
    lv_obj_set_style_text_align(L.date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(L.date, cx, g->landscape ? g->h / 2 - 10 : g->bar_h + 70 + 170);
    clock_refresh();

    int32_t mx = g->landscape ? g->w / 2 + 20 : AOS_UI_PAD;
    int32_t mw = g->landscape ? g->w / 2 - 60 : g->w - 2 * AOS_UI_PAD;
    lv_obj_t *col = lv_obj_create(L.layer);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, mw, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 16, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(col, mx, g->landscape ? g->bar_h + 30 : g->bar_h + 330);
    /* the space for the messages: notifications today, Bluetooth and Home
     * Assistant's when they come, through the same store */
    if (s_cfg.show_notifs) {
        L.msgs = glass(col, mw);
        msgs_refresh(true);
    }
    L.music = glass(col, mw);
    L.m_title = aos_label(L.music, "", aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(L.m_title, lv_pct(100));
    lv_label_set_long_mode(L.m_title, LV_LABEL_LONG_MODE_DOTS);
    L.m_artist = aos_label(L.music, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_width(L.m_artist, lv_pct(100));
    lv_label_set_long_mode(L.m_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_t *row = lv_obj_create(L.music);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), 96);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    round_btn(row, AOS_SYM_SKIP_PREVIOUS, music_prev_cb);
    L.m_play = round_btn(row, AOS_SYM_PLAY, music_play_cb);
    round_btn(row, AOS_SYM_SKIP_NEXT, music_next_cb);
    music_refresh();

    /* the hint, breathing */
    L.chev = aos_label(L.layer, AOS_SYM_CHEVRON_UP, &aos_sym_44, lv_color_white());
    lv_obj_align(L.chev, LV_ALIGN_BOTTOM_MID, 0, -96);
    L.hint = aos_label(L.layer, aos_ui_safe_mode() && aos_lock_pin_len() ? _("Modo seguro: deslizá hacia arriba, sin código")
                                                                       : _("Deslizá hacia arriba para desbloquear"),
                       aos_font_small, lv_color_white());
    lv_obj_align(L.hint, LV_ALIGN_BOTTOM_MID, 0, -50);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, L.hint);
    lv_anim_set_exec_cb(&a, hint_breathe);
    lv_anim_set_values(&a, LV_OPA_40, LV_OPA_COVER);
    lv_anim_set_duration(&a, 1400);
    lv_anim_set_playback_duration(&a, 1400);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    /* the status bar stays above it, as on a phone */
    lv_obj_t *bar = aos_statusbar_obj();
    if (bar) lv_obj_move_foreground(bar);
}

void aos_lock_now(void)
{
    if (!s_cfg.enabled) return;
    s_locked = true;
    aos_switcher_close();
    aos_panel_close();
    build();
}

bool aos_lock_is_locked(void) { return s_locked; }

void aos_lock_layout(void)
{
    if (!s_locked) return;
    bool pad = P.sheet != NULL;
    build();
    if (pad) aos_lock_pad_open(L.layer, _("Ingresá el código"), aos_lock_pin_len(), unlock_pin_cb, pad_cancel_cb, NULL);
}

void aos_lock_init(void)
{
    cfg_load();
    s_inited = true;
    if (s_fails >= FAIL_FREE) {
        int steps = (s_fails - FAIL_FREE) / FAIL_STEP;
        s_wait_until = aos_hal_uptime_ms() + (uint64_t)(WAIT_FIRST_S << (steps > 6 ? 6 : steps)) * 1000;
    }
    if (s_cfg.enabled) aos_lock_now();          /* at boot */
}

void aos_lock_tick(void)
{
    if (!s_inited) return;
    uint64_t now = aos_hal_uptime_ms();
    bool on = aos_hal_display_is_on();
    if (!on) {
        if (!s_off_since) s_off_since = now;
        /* while it is still dark: waking shows the lock screen, never a
         * frame of what was under it */
        if (s_cfg.enabled && !s_locked && now - s_off_since >= (uint64_t)s_cfg.after_s * 1000) aos_lock_now();
    } else {
        s_off_since = 0;
    }
    if (!s_locked) return;
    static uint32_t n;
    if (++n % 5 == 0) {
        clock_refresh();
        msgs_refresh(false);
        music_refresh();
    }
    if (P.msg && wait_left_s()) pad_msg_refresh();
    else if (P.msg && s_wait_until && !wait_left_s()) {
        s_wait_until = 0;
        lv_label_set_text(P.msg, "");
    }
}
