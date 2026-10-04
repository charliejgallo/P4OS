/*
 * P4OS - the web portal: the board from a browser, at http://<name>.local/
 * (port 80 on the board, 8080 in the simulator).
 *
 * What it is for, first: everything the 5" keyboard makes painful - the
 * Home Assistant token, a Wi-Fi password, files for the card (firmware for
 * the Programador, apps, photos, music), menu.txt and modules.txt - and the
 * screen itself, live, with taps from the mouse.
 *
 * One page (web/index.html + app.js + app.css, gzipped into the firmware by
 * tools/gen_web_assets.py) over a JSON API:
 *
 *   GET  /api/info                     the device, the network, memory, card
 *   GET  /api/screen.bmp[?fb=1]        the screen now (RGB565 BMP); fb=1: the framebuffer, blits included
 *   POST /api/touch?x=&y=[&x2=&y2=][&ms=]  a tap, or a drag with x2/y2
 *   POST /api/nav?to=home|back|rotate  POST /api/open?id=
 *   GET  /api/apps                     [{id,name,color_a,color_b,aic,text}], for the pages that draw icons
 *   GET  /api/apps/icon?id=            that app's AIC icon blob (aos_icon_ops.h), 404 if it has none
 *   GET  /api/player                   what plays, and the pipeline's ring and underruns
 *   GET  /api/wifi   GET /api/wifi/scan   POST /api/wifi {ssid,pass}
 *   POST /api/wifi/forget
 *   GET  /api/ha     POST /api/ha {url?,token?}   GET /api/ha/entities
 *   POST /api/ha/fav?id=&on=
 *   GET  /api/fs?path=                 a folder's listing
 *   GET  /api/fs/get?path=             a file (attachment=1 to download); a Range
 *                                      header gets that part (206)
 *   PUT  /api/fs/put?path=             the body becomes the file
 *   POST /api/fs/mkdir?path=   POST /api/fs/delete?path=   POST /api/fs/rename?path=&to=
 *   GET  /api/settings   POST /api/settings {name,tz,lang,wallpaper,brightness,volume,landscape}
 *   GET  /api/flash                    the Programador: status, ports, firmware on the card
 *   POST /api/flash/start {path,port,baud}   POST /api/flash/detect {port}   POST /api/flash/cancel
 *   GET  /api/flash/log?from=          its log lines
 *   GET  /api/serial?ch=               a channel: state, ports, triggers
 *   GET  /api/serial/lines?ch=&from=   its lines since a position (with their marks)
 *   POST /api/serial/start {ch,port,baud}   /api/serial/stop {ch}   /api/serial/send {ch,text,eol}
 *   POST /api/serial/record {ch,on}    /api/serial/trigger {add|remove|index,flags|index,text}
 *   GET  /api/bench ...                the Banco: supply, scope, logger (aos_portal_bench.c)
 *   GET  /api/claude ...               the Claude plan's usage and its sign-in (aos_portal_claude.c)
 *   GET  /api/mqtt ...                 MQTT: the topics seen, publish, settings (aos_portal_mqtt.c)
 *   GET  /api/sysmon[?hist=0]          the Monitor: CPU, tasks, memory, network, card (aos_portal_sysmon.c)
 *   GET  /api/macropad ...             the Macro pad's layout, trying a button (aos_portal_macropad.c)
 *   GET  /api/expansion ...  /api/sensors ...  the header, modules.txt, the sensors (aos_portal_modules.c)
 *   GET  /api/log?from=                the log ring since a position
 *   GET  /api/log?prev=1&from=         the previous boot's tail (kept across a restart)
 *   /api/ota, /api/coredump            firmware updates and the last panic (aos_portal_system.c)
 *   POST /api/restart
 *
 * No authentication: this is a bench device on a home network, the same
 * trust as the watch's portal. Paths are always inside the card; ".." is
 * refused.
 */
#include "aos_portal.h"
#include "aos_httpd.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_ha.h"
#include "aos_flasher.h"
#include "aos_menu.h"
#include "aos_icon_ops.h"
#include "aos_portal_bench.h"
#include "aos_portal_radio.h"
#include "aos_portal_net.h"
#include "aos_portal_access.h"
#include "aos_access.h"
#include "aos_portal_claude.h"
#include "aos_portal_mqtt.h"
#include "aos_portal_sysmon.h"
#include "aos_portal_system.h"
#include "aos_portal_macropad.h"
#include "aos_portal_modules.h"
#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>

/* the gzipped page, from tools/gen_web_assets.py */
typedef struct { const char *path, *ctype; const uint8_t *data; uint32_t len; } aos_web_asset_t;
extern const aos_web_asset_t aos_web_assets[];
extern const int aos_web_asset_count;

static void *s_shot_mx;

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void send_ok(aos_httpd_req_t *r) { aos_httpd_send_json(r, 200, "{\"ok\":true}"); }

static void send_err(aos_httpd_req_t *r, int status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddFalseToObject(o, "ok");
    cJSON_AddStringToObject(o, "error", msg);
    send_cjson(r, status, o);
}

static cJSON *body_json(aos_httpd_req_t *r)
{
    char *b = aos_httpd_body_all(r, 16 * 1024);
    cJSON *o = b ? cJSON_Parse(b) : NULL;
    free(b);
    return o;
}

static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* A path on the card from ?path=: "/firmware/app.bin" -> "<root>/firmware/app.bin".
 * false for no card or anything that tries to leave it. */
static bool card_path(aos_httpd_req_t *r, const char *key, char *out, size_t n, char *rel, size_t rn)
{
    const char *root = aos_hal_path_sd_root();
    char p[256];
    if (!root || !aos_httpd_query(r, key, p, sizeof p)) return false;
    if (strstr(p, "..")) return false;
    const char *q = p;
    while (*q == '/') q++;
    snprintf(out, n, "%s%s%s", root, *q ? "/" : "", q);
    size_t l = strlen(out);
    while (l > strlen(root) && out[l - 1] == '/') out[--l] = 0;
    if (rel) snprintf(rel, rn, "/%.*s", (int)(rn > 2 ? rn - 2 : 0), q);
    return true;
}

/* -------------------------------------------------------------------------- */
/* Device                                                                      */
/* -------------------------------------------------------------------------- */

static void api_info(aos_httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", aos_hal_device_name());
    cJSON_AddStringToObject(o, "board", aos_hal_board_name());
    cJSON_AddStringToObject(o, "firmware", aos_hal_firmware_version());
    cJSON_AddNumberToObject(o, "uptime_s", (double)(aos_hal_uptime_ms() / 1000));
    uint32_t fi = 0, fp = 0;
    aos_hal_heap_info(&fi, &fp);
    cJSON_AddNumberToObject(o, "heap_internal", fi);
    cJSON_AddNumberToObject(o, "heap_psram", fp);
    uint64_t tot = 0, fr = 0;
    if (aos_hal_sd_usage(&tot, &fr)) {
        cJSON_AddNumberToObject(o, "sd_total", (double)tot);
        cJSON_AddNumberToObject(o, "sd_free", (double)fr);
    }
    cJSON_AddNumberToObject(o, "screen_w", aos_hal_screen_w());
    cJSON_AddNumberToObject(o, "screen_h", aos_hal_screen_h());
    cJSON_AddBoolToObject(o, "landscape", aos_ui_landscape());
    cJSON_AddStringToObject(o, "ip", aos_hal_net_ip());
    cJSON_AddStringToObject(o, "ssid", aos_hal_net_ssid());
    cJSON_AddNumberToObject(o, "rssi", aos_hal_net_rssi());
    struct tm t;
    aos_hal_time_now(&t);
    char ts[64];
    snprintf(ts, sizeof ts, "%04d-%02d-%02d %02d:%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    cJSON_AddStringToObject(o, "time", aos_hal_time_is_valid() ? ts : "");
    cJSON_AddNumberToObject(o, "ha_state", aos_ha_state());
    send_cjson(r, 200, o);
}

static void send_bmp_header(aos_httpd_req_t *r, int w, int h, bool *ok)
{
    uint32_t row = (uint32_t)w * 2, img = row * h, off = 14 + 40 + 12;
    uint8_t hd[66] = { 'B', 'M' };
    #define LE32B(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); (p)[2] = (uint8_t)((v) >> 16); (p)[3] = (uint8_t)((v) >> 24); } while (0)
    LE32B(hd + 2, off + img);
    LE32B(hd + 10, off);
    LE32B(hd + 14, 40);
    LE32B(hd + 18, w);
    LE32B(hd + 22, (uint32_t)(-(int32_t)h));
    hd[26] = 1;
    hd[28] = 16;
    LE32B(hd + 30, 3);
    LE32B(hd + 34, img);
    LE32B(hd + 54, 0xF800);
    LE32B(hd + 58, 0x07E0);
    LE32B(hd + 62, 0x001F);
    #undef LE32B
    *ok = aos_httpd_begin(r, 200, "image/bmp", (long)(off + img), NULL) && aos_httpd_write(r, hd, sizeof hd);
}

static void api_screen(aos_httpd_req_t *r)
{
    /* ?fb=1: the framebuffer itself, what the glass shows, blits included,
     * in the panel's portrait (not turned) */
    const uint16_t *fb;
    int fw, fh;
    if (aos_httpd_query_int(r, "fb", 0) && aos_hal_display_fb(&fb, &fw, &fh)) {
        bool ok;
        send_bmp_header(r, fw, fh, &ok);
        for (int y = 0; ok && y < fh; y += 32) {
            int n = fh - y < 32 ? fh - y : 32;
            ok = aos_httpd_write(r, fb + (size_t)y * fw, (size_t)fw * 2 * n);
        }
        return;
    }
    aos_hal_mutex_lock(s_shot_mx);
    aos_ui_snapshot_t s;
    bool ok = aos_ui_request_snapshot(true);
    aos_snapshot_state_t st = AOS_SNAPSHOT_PENDING;
    for (int i = 0; ok && i < 60 && (st = aos_ui_snapshot_peek(&s)) == AOS_SNAPSHOT_PENDING; i++) aos_hal_sleep_ms(50);
    if (!ok || st != AOS_SNAPSHOT_READY) {
        if (st == AOS_SNAPSHOT_READY || st == AOS_SNAPSHOT_FAILED) aos_ui_snapshot_release();
        aos_hal_mutex_unlock(s_shot_mx);
        send_err(r, 503, "no capture");
        return;
    }
    /* BMP, 16 bits with RGB565 masks, rows top-down (negative height) */
    uint32_t row = (uint32_t)s.w * 2, img = row * s.h, off = 14 + 40 + 12;
    uint8_t h[66] = { 'B', 'M' };
    #define LE32(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((v) >> 8); (p)[2] = (uint8_t)((v) >> 16); (p)[3] = (uint8_t)((v) >> 24); } while (0)
    LE32(h + 2, off + img);
    LE32(h + 10, off);
    LE32(h + 14, 40);
    LE32(h + 18, s.w);
    LE32(h + 22, (uint32_t)(-(int32_t)s.h));
    h[26] = 1;
    h[28] = 16;
    LE32(h + 30, 3);                /* BI_BITFIELDS */
    LE32(h + 34, img);
    LE32(h + 54, 0xF800);
    LE32(h + 58, 0x07E0);
    LE32(h + 62, 0x001F);
    if (aos_httpd_begin(r, 200, "image/bmp", (long)(off + img), NULL) && aos_httpd_write(r, h, sizeof h))
        for (uint16_t y = 0; y < s.h; y++)
            if (!aos_httpd_write(r, s.data + (size_t)y * s.stride, row)) break;
    aos_ui_snapshot_release();
    aos_hal_mutex_unlock(s_shot_mx);
}

typedef struct { int x, y, x2, y2, ms; bool drag; } touch_t;

static void touch_now(void *arg)
{
    touch_t *t = arg;
    if (t->drag) aos_ui_inject_drag(t->x, t->y, t->x2, t->y2, t->ms);
    else aos_ui_inject_tap(t->x, t->y, t->ms);
    free(t);
}

static void api_touch(aos_httpd_req_t *r)
{
    touch_t *t = calloc(1, sizeof *t);
    if (!t) { send_err(r, 500, "memory"); return; }
    t->x = (int)aos_httpd_query_int(r, "x", -1);
    t->y = (int)aos_httpd_query_int(r, "y", -1);
    t->x2 = (int)aos_httpd_query_int(r, "x2", -1);
    t->y2 = (int)aos_httpd_query_int(r, "y2", -1);
    t->drag = t->x2 >= 0 && t->y2 >= 0;
    t->ms = (int)aos_httpd_query_int(r, "ms", t->drag ? 250 : 80);
    if (t->x < 0 || t->y < 0 || !aos_ui_request_call(touch_now, t)) { free(t); send_err(r, 400, "x, y"); return; }
    send_ok(r);
}

static void api_nav(aos_httpd_req_t *r)
{
    char to[16] = "";
    aos_httpd_query(r, "to", to, sizeof to);
    if (!strcmp(to, "home")) aos_ui_request_nav(AOS_UI_NAV_HOME);
    else if (!strcmp(to, "back")) aos_ui_request_nav(AOS_UI_NAV_BACK);
    else if (!strcmp(to, "rotate")) aos_ui_request_landscape(-1);
    else { send_err(r, 400, "to=home|back|rotate"); return; }
    send_ok(r);
}

static void api_open(aos_httpd_req_t *r)
{
    char id[48];
    if (!aos_httpd_query(r, "id", id, sizeof id)) { send_err(r, 400, "id"); return; }
    aos_ui_request_open(id);
    send_ok(r);
}

static void api_apps(aos_httpd_req_t *r)
{
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < aos_ui_app_count(); i++) {
        const aos_app_t *app = aos_ui_app_at(i);
        if (!app || !app->desc.id || !app->create) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", app->desc.id);
        cJSON_AddStringToObject(o, "name", app->desc.name ? app->desc.name : app->desc.id);
        /* enough for a page to draw the icon (#inicio): its gradient, whether
         * there is an AIC blob at /api/apps/icon, and a plain-text icon */
        char hex[8];
        snprintf(hex, sizeof hex, "%06lX", (unsigned long)(app->desc.color_a & 0xFFFFFF));
        cJSON_AddStringToObject(o, "color_a", hex);
        snprintf(hex, sizeof hex, "%06lX", (unsigned long)((app->desc.color_b ? app->desc.color_b : app->desc.color_a) & 0xFFFFFF));
        cJSON_AddStringToObject(o, "color_b", hex);
        size_t len = 0;
        if (!aos_icon_ops_for(app->desc.id, &len)) aos_icon_ops_builtin(app->desc.icon_vec, &len);
        cJSON_AddNumberToObject(o, "aic", (double)len);
        bool ascii = app->desc.icon && app->desc.icon[0];
        for (const char *c = app->desc.icon; ascii && *c; c++) ascii = (unsigned char)*c >= 0x20 && (unsigned char)*c < 0x80;
        if (ascii) cJSON_AddStringToObject(o, "text", app->desc.icon);
        cJSON_AddItemToArray(a, o);
    }
    send_cjson(r, 200, a);
}

/* The blob aos_icon_create() would draw for an app: its own or a file's,
 * then the firmware's table. A glyph icon has none. */
static void api_app_icon(aos_httpd_req_t *r)
{
    char id[48];
    if (!aos_httpd_query(r, "id", id, sizeof id)) { send_err(r, 400, "falta id"); return; }
    size_t len = 0;
    const uint8_t *ops = aos_icon_ops_for(id, &len);
    for (int i = 0; !ops && i < aos_ui_app_count(); i++) {
        const aos_app_t *a = aos_ui_app_at(i);
        if (a && a->desc.id && !strcmp(a->desc.id, id)) ops = aos_icon_ops_builtin(a->desc.icon_vec, &len);
    }
    if (!ops || !len) { send_err(r, 404, "sin ícono AIC"); return; }
    if (aos_httpd_begin(r, 200, "application/octet-stream", (long)len, NULL)) aos_httpd_write(r, ops, len);
}

/* -------------------------------------------------------------------------- */
/* Wi-Fi                                                                       */
/* -------------------------------------------------------------------------- */

static void api_wifi(aos_httpd_req_t *r)
{
    const char *m = aos_httpd_method(r);
    if (!strcmp(m, "POST")) {
        cJSON *b = body_json(r);
        const char *ssid = jstr(b, "ssid"), *pass = jstr(b, "pass");
        bool ok = ssid && aos_hal_net_set_credentials(ssid, pass ? pass : "");
        cJSON_Delete(b);
        if (ok) send_ok(r);
        else send_err(r, 400, "red o contraseña inválidas (WPA2: de 8 a 63 caracteres)");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    static const char *const ST[] = { "off", "connecting", "connected", "failed" };
    aos_net_state_t s = aos_hal_net_state();
    cJSON_AddStringToObject(o, "state", s <= AOS_NET_FAILED ? ST[s] : "?");
    cJSON_AddBoolToObject(o, "enabled", aos_hal_net_enabled());
    cJSON_AddBoolToObject(o, "has_credentials", aos_hal_net_has_credentials());
    cJSON_AddStringToObject(o, "ssid", aos_hal_net_ssid());
    cJSON_AddStringToObject(o, "ip", aos_hal_net_ip());
    cJSON_AddNumberToObject(o, "rssi", aos_hal_net_rssi());
    send_cjson(r, 200, o);
}

/* Bluetooth (components/aos_ble): GET says how it is, POST {"on": bool}
 * switches it as Settings does, {"keyboard": bool} the keyboard mode, and
 * {"forget": true} wipes the phone's keys, {"music": bool} the iPhone's music
 * (AMS) and {"media": "play"|"next"|"prev"} commands it. {"key": "volup"}
 * and {"type": "text"} send through the keyboard (the cable's, or the
 * Bluetooth one) and answer whether it went. */
static void api_bt(aos_httpd_req_t *r)
{
    if (!strcmp(aos_httpd_method(r), "POST")) {
        cJSON *b = body_json(r);
        cJSON *on = b ? cJSON_GetObjectItem(b, "on") : NULL;
        cJSON *kbd = b ? cJSON_GetObjectItem(b, "keyboard") : NULL;
        bool forget = b && cJSON_IsTrue(cJSON_GetObjectItem(b, "forget"));
        cJSON *music = b ? cJSON_GetObjectItem(b, "music") : NULL;
        const char *key = jstr(b, "key"), *text = jstr(b, "type"), *media = jstr(b, "media");
        if (media) {
            int cmd = !strcmp(media, "play") ? AOS_MEDIA_PLAY_PAUSE : !strcmp(media, "next") ? AOS_MEDIA_NEXT
                    : !strcmp(media, "prev") ? AOS_MEDIA_PREV : -1;
            bool sent = cmd >= 0 && aos_hal_media_command((aos_media_cmd_t)cmd);
            cJSON_Delete(b);
            aos_httpd_send_json(r, 200, sent ? "{\"sent\":true}" : "{\"sent\":false}");
            return;
        }
        if (key || text) {
            bool sent = key ? aos_hal_usb_key(key) : aos_hal_usb_type(text) > 0;
            cJSON_Delete(b);
            aos_httpd_send_json(r, 200, sent ? "{\"sent\":true}" : "{\"sent\":false}");
            return;
        }
        if (cJSON_IsBool(on)) aos_hal_bt_enable(cJSON_IsTrue(on));
        if (cJSON_IsBool(kbd)) aos_hal_bt_keyboard_enable(cJSON_IsTrue(kbd));
        if (forget) aos_hal_bt_forget();
        if (cJSON_IsBool(music)) aos_hal_media_enable(cJSON_IsTrue(music));
        bool any = cJSON_IsBool(on) || cJSON_IsBool(kbd) || forget || cJSON_IsBool(music);
        cJSON_Delete(b);
        if (!any) { send_err(r, 400, "falta \"on\", \"keyboard\", \"music\" o \"forget\""); return; }
    }
    static const char *const ST[] = { "off", "advertising", "pairing", "connected" };
    aos_bt_state_t st = aos_hal_bt_state();
    int pct = -1;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "enabled", aos_hal_bt_enabled());
    cJSON_AddStringToObject(o, "state", st <= AOS_BT_CONNECTED ? ST[st] : "?");
    cJSON_AddStringToObject(o, "peer", aos_hal_bt_peer());
    cJSON_AddBoolToObject(o, "bonded", aos_hal_bt_bonded());
    if (aos_hal_bt_phone_battery(&pct)) cJSON_AddNumberToObject(o, "phone_battery", pct);
    cJSON_AddBoolToObject(o, "keyboard", aos_hal_bt_keyboard_enabled());
    cJSON_AddStringToObject(o, "computer", aos_hal_bt_keyboard_host());
    cJSON_AddBoolToObject(o, "keyboard_ready", aos_hal_bt_keyboard_ready());
    cJSON_AddBoolToObject(o, "music", aos_hal_media_enabled());
    aos_media_info_t mi;
    if (aos_hal_media_link() == AOS_MEDIA_CONNECTED && aos_hal_media_info(&mi)) {
        cJSON *m = cJSON_AddObjectToObject(o, "media");
        cJSON_AddStringToObject(m, "player", aos_hal_media_player());
        cJSON_AddStringToObject(m, "title", mi.title);
        cJSON_AddStringToObject(m, "artist", mi.artist);
        cJSON_AddStringToObject(m, "album", mi.album);
        cJSON_AddBoolToObject(m, "playing", mi.playing);
        cJSON_AddNumberToObject(m, "position_s", mi.position_s);
        cJSON_AddNumberToObject(m, "duration_s", mi.duration_s);
    }
    send_cjson(r, 200, o);
}

/* The C6's firmware (docs/C6.md): GET says what runs, what waits on the
 * card and how an update goes; POST /api/c6/update starts one from the
 * card's /firmware/c6.bin (put there with fs/put first). */
static void c6_card_path(char *out, size_t n)
{
    const char *root = aos_hal_path_sd_root();
    if (root) snprintf(out, n, "%s/firmware/c6.bin", root);
    else if (n) out[0] = 0;
}

static void api_c6(aos_httpd_req_t *r)
{
    static const char *const ST[] = { "idle", "sending", "done", "failed" };
    char path[96], ver[32] = "";
    c6_card_path(path, sizeof path);
    bool img = path[0] && aos_hal_net_coprocessor_image(path, ver, sizeof ver);
    aos_c6_update_t u;
    aos_hal_net_coprocessor_status(&u);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "running", aos_hal_net_coprocessor_fw());
    if (img) cJSON_AddStringToObject(o, "card", ver);
    else cJSON_AddNullToObject(o, "card");
    cJSON_AddStringToObject(o, "state", u.state <= AOS_C6_FAILED ? ST[u.state] : "?");
    cJSON_AddNumberToObject(o, "sent", u.sent);
    cJSON_AddNumberToObject(o, "total", u.total);
    if (u.error[0]) cJSON_AddStringToObject(o, "error", u.error);
    send_cjson(r, 200, o);
}

static void api_c6_update(aos_httpd_req_t *r)
{
    char path[96];
    c6_card_path(path, sizeof path);
    if (!path[0] || !aos_hal_net_coprocessor_image(path, NULL, 0))
        send_err(r, 400, "no hay una imagen del C6 en /firmware/c6.bin");
    else if (!aos_hal_net_coprocessor_update(path))
        send_err(r, 409, "no se pudo empezar (¿ya hay una en curso?)");
    else send_ok(r);
}

static void api_wifi_scan(aos_httpd_req_t *r)
{
    aos_wifi_ap_t aps[24];
    int n = aos_hal_net_scan(aps, 24);
    if (n < 0) { send_err(r, 503, "la búsqueda falló"); return; }
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].secure);
        cJSON_AddItemToArray(a, o);
    }
    send_cjson(r, 200, a);
}

/* -------------------------------------------------------------------------- */
/* Home Assistant                                                              */
/* -------------------------------------------------------------------------- */

static const char *const HA_DOMAINS[HA_DOMAIN_COUNT] = {
    "light", "switch", "input_boolean", "fan", "cover", "climate", "lock",
    "media_player", "scene", "script", "button", "sensor", "binary_sensor",
};

static void api_ha(aos_httpd_req_t *r)
{
    aos_ha_start();
    if (!strcmp(aos_httpd_method(r), "POST")) {
        cJSON *b = body_json(r);
        const char *url = jstr(b, "url"), *token = jstr(b, "token");
        bool ok = true;
        if (url && url[0]) ok = aos_ha_set_url(url);
        if (ok && token && token[0]) aos_ha_set_token(token);
        cJSON_Delete(b);
        if (ok) send_ok(r);
        else send_err(r, 400, "la dirección empieza con http://");
        return;
    }
    static const char *const ST[] = { "unconfigured", "waiting_net", "connecting", "auth_failed", "loading", "ready", "error" };
    cJSON *o = cJSON_CreateObject();
    aos_ha_state_t s = aos_ha_state();
    cJSON_AddStringToObject(o, "state", s <= AOS_HA_ERROR ? ST[s] : "?");
    cJSON_AddStringToObject(o, "error", aos_ha_error());
    cJSON_AddStringToObject(o, "url", aos_ha_url());
    cJSON_AddBoolToObject(o, "has_token", aos_ha_has_token());
    cJSON_AddStringToObject(o, "location", aos_ha_location());
    aos_ha_lock();
    cJSON_AddNumberToObject(o, "entities", aos_ha_count());
    cJSON_AddNumberToObject(o, "areas", aos_ha_area_count());
    aos_ha_unlock();
    send_cjson(r, 200, o);
}

static void api_ha_entities(aos_httpd_req_t *r)
{
    cJSON *a = cJSON_CreateArray();
    aos_ha_lock();
    for (int i = 0; i < aos_ha_count(); i++) {
        const aos_ha_entity_t *e = aos_ha_at(i);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", e->id);
        cJSON_AddStringToObject(o, "name", e->name);
        cJSON_AddStringToObject(o, "domain", e->domain < HA_DOMAIN_COUNT ? HA_DOMAINS[e->domain] : "");
        cJSON_AddStringToObject(o, "area", e->area >= 0 ? aos_ha_area_name(e->area) : "");
        cJSON_AddStringToObject(o, "state", e->state);
        cJSON_AddStringToObject(o, "unit", e->unit);
        cJSON_AddBoolToObject(o, "fav", aos_ha_is_fav(e->id));
        cJSON_AddItemToArray(a, o);
    }
    aos_ha_unlock();
    send_cjson(r, 200, a);
}

static void api_ha_fav(aos_httpd_req_t *r)
{
    char id[64];
    if (!aos_httpd_query(r, "id", id, sizeof id)) { send_err(r, 400, "id"); return; }
    aos_ha_set_fav(id, aos_httpd_query_int(r, "on", 1) != 0);
    send_ok(r);
}

/* -------------------------------------------------------------------------- */
/* Files on the card                                                           */
/* -------------------------------------------------------------------------- */

#define FS_LIST_MAX 10000

typedef struct { cJSON *a; int n; } fs_list_t;

static bool fs_list_one(const aos_dir_entry_t *de, void *ctx)
{
    fs_list_t *l = ctx;
    if (de->name[0] == '.' && (!de->name[1] || de->name[1] == '.')) return true;
    cJSON *e = cJSON_CreateObject();
    cJSON_AddStringToObject(e, "name", de->name);
    cJSON_AddBoolToObject(e, "dir", de->dir);
    cJSON_AddNumberToObject(e, "size", (double)de->size);
    cJSON_AddNumberToObject(e, "mtime", (double)de->mtime);
    cJSON_AddItemToArray(l->a, e);
    return ++l->n < FS_LIST_MAX;
}

/* One walk of the folder (aos_hal_dir_scan): a readdir and a stat() per
 * entry is O(n^2) on FAT, and a folder of 3000 files did not answer in a
 * minute (2026-09-29). */
static void api_fs_list(aos_httpd_req_t *r)
{
    char path[320], rel[256];
    if (!card_path(r, "path", path, sizeof path, rel, sizeof rel)) { send_err(r, 400, "sin tarjeta o ruta inválida"); return; }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "path", rel);
    fs_list_t l = { cJSON_AddArrayToObject(o, "entries"), 0 };
    if (aos_hal_dir_scan(path, fs_list_one, &l) < 0) { cJSON_Delete(o); send_err(r, 404, "no existe"); return; }
    if (l.n >= FS_LIST_MAX) cJSON_AddBoolToObject(o, "truncated", true);
    uint64_t tot = 0, fr = 0;
    if (aos_hal_sd_usage(&tot, &fr)) {
        cJSON_AddNumberToObject(o, "total", (double)tot);
        cJSON_AddNumberToObject(o, "free", (double)fr);
    }
    send_cjson(r, 200, o);
}

static const char *ctype_of(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return "application/octet-stream";
    static const char *const T[][2] = {
        { ".txt", "text/plain; charset=utf-8" }, { ".json", "application/json" }, { ".log", "text/plain; charset=utf-8" },
        { ".csv", "text/plain; charset=utf-8" }, { ".md", "text/plain; charset=utf-8" }, { ".lua", "text/plain; charset=utf-8" },
        { ".jpg", "image/jpeg" }, { ".jpeg", "image/jpeg" }, { ".png", "image/png" }, { ".gif", "image/gif" },
        { ".bmp", "image/bmp" }, { ".mp3", "audio/mpeg" }, { ".wav", "audio/wav" }, { ".html", "text/html; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" }, { ".mjs", "text/javascript; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" }, { ".svg", "image/svg+xml" },
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++)
        if (!strcasecmp(dot, T[i][0])) return T[i][1];
    return "application/octet-stream";
}

/* "bytes=a-b", "bytes=a-" or "bytes=-n", one range: what a browser's <audio>
 * and <video> ask for (Safari plays nothing without it) and what a page uses
 * to read only a file's header. false: no such header, or one we do not
 * serve (several ranges), and the whole file goes. */
static bool range_of(const char *h, long size, long *from, long *to, bool *bad)
{
    *bad = false;
    if (!h || strncmp(h, "bytes=", 6) || strchr(h, ',')) return false;
    const char *p = h + 6;
    char *end;
    if (*p == '-') {                                /* the last n bytes */
        long n = strtol(p + 1, &end, 10);
        if (end == p + 1 || n <= 0) { *bad = true; return true; }
        *from = n >= size ? 0 : size - n;
        *to = size - 1;
    } else {
        *from = strtol(p, &end, 10);
        if (end == p || *end != '-') return false;
        *to = end[1] ? strtol(end + 1, NULL, 10) : size - 1;
        if (*to >= size) *to = size - 1;
    }
    if (*from < 0 || *from >= size || *to < *from) *bad = true;
    return true;
}

static void api_fs_get(aos_httpd_req_t *r)
{
    char path[320];
    if (!card_path(r, "path", path, sizeof path, NULL, 0)) { send_err(r, 400, "ruta inválida"); return; }
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (!f || stat(path, &st) || S_ISDIR(st.st_mode)) { if (f) fclose(f); send_err(r, 404, "no existe"); return; }
    const char *name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    long size = (long)st.st_size, from = 0, to = size - 1;
    bool bad, ranged = range_of(aos_httpd_header(r, "Range"), size, &from, &to, &bad);
    char extra[320];
    int el = snprintf(extra, sizeof extra, "Accept-Ranges: bytes\r\n");
    if (ranged && bad) {
        snprintf(extra + el, sizeof extra - el, "Content-Range: bytes */%ld\r\n", size);
        aos_httpd_begin(r, 416, "text/plain", 0, extra);
        fclose(f);
        return;
    }
    if (ranged) el += snprintf(extra + el, sizeof extra - el, "Content-Range: bytes %ld-%ld/%ld\r\n", from, to, size);
    if (aos_httpd_query_int(r, "attachment", 0))
        snprintf(extra + el, sizeof extra - el, "Content-Disposition: attachment; filename=\"%.150s\"\r\n", name);
    if (aos_httpd_begin(r, ranged ? 206 : 200, ctype_of(name), to - from + 1, extra)) {
        /* read() on the descriptor: 16 KB at a time straight into buf, which
         * the card DMAs to (an unbuffered fread() would go byte by byte) */
        char *buf = aos_hal_io_alloc(16384);
        long left = to - from + 1;
        ssize_t n;
        if (from && lseek(fileno(f), from, SEEK_SET) != from) left = 0;
        while (buf && left > 0 && (n = read(fileno(f), buf, left < 16384 ? (size_t)left : 16384)) > 0) {
            if (!aos_httpd_write(r, buf, (size_t)n)) break;
            left -= n;
        }
        aos_hal_io_free(buf);
    }
    fclose(f);
}

/* After a change to one of the shell's own files, the shell takes it. */
static void reload_for(const char *rel)
{
    if (!strcmp(rel, "/menu.txt")) aos_ui_request_menu();
    else if (!strcmp(rel, "/modules.txt")) aos_io_load();
    else if (!strcmp(rel, "/ha.txt")) aos_ha_import_file();
}

static bool menu_ok(aos_httpd_req_t *r, const char *path)
{
    char err[96] = "";
    bool ok = false;
    char *buf = malloc(AOS_MENU_FILE_MAX + 1);
    FILE *f = buf ? fopen(path, "rb") : NULL;
    if (f) {
        size_t len = fread(buf, 1, AOS_MENU_FILE_MAX + 1, f);
        fclose(f);
        ok = aos_menu_validate(buf, len, err, sizeof err);
    } else snprintf(err, sizeof err, "no se pudo leer");
    free(buf);
    if (!ok) {
        char msg[128];
        snprintf(msg, sizeof msg, "menu.txt inválido: %s", err);
        send_err(r, 400, msg);
    }
    return ok;
}

/* How fast the card reads a file, with no network in the way: the card
 * and the C6 share the P4's SDMMC controller (slots 0 and 1). */
static void api_fs_bench(aos_httpd_req_t *r)
{
    char path[320], rel[256];
    if (!card_path(r, "path", path, sizeof path, rel, sizeof rel)) { send_err(r, 400, "ruta inválida"); return; }
    FILE *f = fopen(path, "rb");
    char *buf = aos_hal_io_alloc(65536);
    if (!f || !buf) { if (f) fclose(f); aos_hal_io_free(buf); send_err(r, 404, "no se pudo abrir"); return; }
    uint64_t t0 = aos_hal_uptime_ms(), n = 0;
    ssize_t got;            /* read(), as fs/get: an unbuffered fread() goes byte by byte */
    while ((got = read(fileno(f), buf, 65536)) > 0 && n < 16u * 1048576) n += (uint64_t)got;
    uint64_t ms = aos_hal_uptime_ms() - t0;
    fclose(f);
    aos_hal_io_free(buf);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "bytes", (double)n);
    cJSON_AddNumberToObject(o, "ms", (double)ms);
    cJSON_AddNumberToObject(o, "kb_s", ms ? (double)n / ms * 1000 / 1024 : 0);
    send_cjson(r, 200, o);
}

static void api_fs_put(aos_httpd_req_t *r)
{
    char path[320], rel[256], tmp[340];
    if (!card_path(r, "path", path, sizeof path, rel, sizeof rel) || !strcmp(rel, "/")) { send_err(r, 400, "ruta inválida"); return; }
    if (aos_httpd_body_len(r) < 0) { send_err(r, 400, "falta Content-Length"); return; }
    uint64_t tot = 0, fr = 0;
    if (aos_hal_sd_usage(&tot, &fr) && (uint64_t)aos_httpd_body_len(r) + 65536 > fr) { send_err(r, 413, "no entra en la tarjeta"); return; }
    /* into a temporary name, renamed at the end: a cut upload leaves the old file whole */
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { send_err(r, 409, "no se pudo crear (¿existe la carpeta?)"); return; }
    setvbuf(f, NULL, _IONBF, 0);            /* 16 KB at a time straight from buf */
    char *buf = aos_hal_io_alloc(16384);
    long done = 0;
    int n = 0;
    while (buf && (n = aos_httpd_body_read(r, buf, 16384)) > 0) {
        if (fwrite(buf, 1, (size_t)n, f) != (size_t)n) { n = -1; break; }
        done += n;
    }
    aos_hal_io_free(buf);
    fclose(f);
    if (n < 0 || done != aos_httpd_body_len(r)) { remove(tmp); send_err(r, 500, "se cortó la subida"); return; }
    /* menu.txt is read all or nothing: a bad one would put the home screen
     * back to the default order, so it is refused with the parser's reason */
    if (!strcmp(rel, "/menu.txt") && !menu_ok(r, tmp)) { remove(tmp); return; }
    remove(path);
    if (rename(tmp, path)) { remove(tmp); send_err(r, 500, "no se pudo guardar"); return; }
    aos_hal_log("portal", "wrote %s (%ld bytes)", rel, done);
    reload_for(rel);
    send_ok(r);
}

static bool rm_tree(const char *path)
{
    struct stat st;
    if (stat(path, &st)) return false;
    if (!S_ISDIR(st.st_mode)) return remove(path) == 0;
    DIR *d = opendir(path);
    struct dirent *de;
    bool ok = d != NULL;
    while (d && (de = readdir(d))) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        char sub[600];
        snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
        ok &= rm_tree(sub);
    }
    if (d) closedir(d);
    return ok && rmdir(path) == 0;
}

static void api_fs_op(aos_httpd_req_t *r, const char *op)
{
    char path[320], rel[256];
    if (!card_path(r, "path", path, sizeof path, rel, sizeof rel) || !strcmp(rel, "/")) { send_err(r, 400, "ruta inválida"); return; }
    bool ok = false;
    if (!strcmp(op, "mkdir")) ok = mkdir(path, 0777) == 0;
    else if (!strcmp(op, "delete")) ok = rm_tree(path);
    else if (!strcmp(op, "rename")) {
        char to[320];
        ok = card_path(r, "to", to, sizeof to, NULL, 0) && rename(path, to) == 0;
    }
    if (ok) send_ok(r);
    else send_err(r, 409, strerror(errno));
}

/* -------------------------------------------------------------------------- */
/* Programador                                                                 */
/* -------------------------------------------------------------------------- */

static const char *const FLASH_PHASE[] = { "idle", "opening", "connecting", "stub", "baud", "info", "erasing",
                                           "writing", "verifying", "resetting", "done", "failed", "cancelled" };

static void api_flash(aos_httpd_req_t *r)
{
    aos_flasher_status_t st;
    aos_flasher_status(&st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "busy", st.busy);
    cJSON_AddStringToObject(o, "job", st.job == AOS_FLASHER_JOB_FLASH ? "flash" : st.job == AOS_FLASHER_JOB_DETECT ? "detect" : "");
    cJSON_AddStringToObject(o, "phase", st.phase <= AOS_FLASHER_CANCELLED ? FLASH_PHASE[st.phase] : "?");
    cJSON_AddNumberToObject(o, "seq", st.seq);
    cJSON_AddStringToObject(o, "port", st.port);
    cJSON_AddStringToObject(o, "source", st.source);
    cJSON_AddNumberToObject(o, "file_index", st.file_index);
    cJSON_AddNumberToObject(o, "file_count", st.file_count);
    cJSON_AddStringToObject(o, "file_name", st.file_name);
    cJSON_AddNumberToObject(o, "done", st.done);
    cJSON_AddNumberToObject(o, "total", st.total);
    cJSON_AddNumberToObject(o, "percent", st.percent);
    cJSON_AddNumberToObject(o, "bytes_per_s", st.bytes_per_s);
    cJSON_AddNumberToObject(o, "elapsed_ms", st.elapsed_ms);
    cJSON_AddNumberToObject(o, "eta_ms", st.eta_ms);
    cJSON_AddBoolToObject(o, "ok", st.ok);
    cJSON_AddBoolToObject(o, "verified", st.verified);
    cJSON_AddStringToObject(o, "error", st.error);
    if (st.chip_known) {
        cJSON *c = cJSON_AddObjectToObject(o, "chip");
        cJSON_AddStringToObject(c, "name", st.chip);
        if (st.revision != 0xFFFF) {
            char rv[16];
            snprintf(rv, sizeof rv, "v%d.%d", st.revision / 100, st.revision % 100);
            cJSON_AddStringToObject(c, "revision", rv);
        }
        if (st.mac_known) {
            char mac[20];
            snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
            cJSON_AddStringToObject(c, "mac", mac);
        }
        cJSON_AddNumberToObject(c, "flash_size", st.flash_size);
        cJSON_AddBoolToObject(c, "secure", st.secure);
    }
    cJSON *ports = cJSON_AddArrayToObject(o, "ports");
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        if (pt->kind != AOS_PORT_UART) continue;
        int en = -1, boot = -1;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", pt->name);
        cJSON_AddBoolToObject(p, "lines", aos_io_port_lines(pt->name, &en, &boot));
        const char *holder = aos_io_owner(pt->pins[0]);
        if (holder) cJSON_AddStringToObject(p, "holder", holder);
        cJSON_AddItemToArray(ports, p);
    }
    static aos_flasher_source_t src[AOS_FLASHER_SOURCES_MAX] AOS_BSS_PSRAM;     /* 24 x ~1 KB: not on a 12 KB stack, nor in internal RAM */
    static void *src_mx;
    if (!src_mx) src_mx = aos_hal_mutex_create();
    aos_hal_mutex_lock(src_mx);
    int n = aos_flasher_scan(src, AOS_FLASHER_SOURCES_MAX);
    cJSON *a = cJSON_AddArrayToObject(o, "sources");
    for (int i = 0; i < n; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", src[i].name);
        cJSON_AddStringToObject(e, "path", src[i].path);
        cJSON_AddBoolToObject(e, "project", src[i].project);
        cJSON_AddStringToObject(e, "chip", src[i].chip);
        cJSON_AddStringToObject(e, "chip_label", src[i].chip[0] ? aos_flasher_chip_label(src[i].chip) : "");
        cJSON_AddStringToObject(e, "app", src[i].app_name);
        cJSON_AddStringToObject(e, "version", src[i].app_version);
        cJSON_AddNumberToObject(e, "total", src[i].total);
        cJSON *fs = cJSON_AddArrayToObject(e, "files");
        for (int k = 0; k < src[i].nfiles; k++) {
            cJSON *f = cJSON_CreateObject();
            char off[12];
            snprintf(off, sizeof off, "0x%05X", (unsigned)src[i].files[k].offset);
            cJSON_AddStringToObject(f, "offset", off);
            cJSON_AddStringToObject(f, "name", src[i].files[k].name);
            cJSON_AddNumberToObject(f, "size", src[i].files[k].size);
            cJSON_AddItemToArray(fs, f);
        }
        cJSON_AddItemToArray(a, e);
    }
    aos_hal_mutex_unlock(src_mx);
    cJSON_AddStringToObject(o, "dir", aos_flasher_dir() ? aos_flasher_dir() : "");
    cJSON_AddNumberToObject(o, "log_first", aos_flasher_log_first());
    cJSON_AddNumberToObject(o, "log_count", aos_flasher_log_count());
    send_cjson(r, 200, o);
}

static void api_flash_start(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    const char *path = jstr(b, "path"), *port = jstr(b, "port");
    const cJSON *baud = cJSON_GetObjectItemCaseSensitive(b, "baud");
    static aos_flasher_source_t src;
    static void *mx;
    if (!mx) mx = aos_hal_mutex_create();
    char err[112] = "";
    aos_hal_mutex_lock(mx);
    bool ok = path && aos_flasher_source_load(path, &src, err, sizeof err);
    if (ok) {
        aos_flasher_opts_t o = { .baud = cJSON_IsNumber(baud) ? (uint32_t)baud->valueint : 0 };
        ok = aos_flasher_flash(port && port[0] ? port : "uart.b", &src, &o);
        if (!ok) snprintf(err, sizeof err, "%s", aos_flasher_busy() ? "ya hay una grabación en curso" : "no se pudo empezar");
    }
    aos_hal_mutex_unlock(mx);
    cJSON_Delete(b);
    if (ok) send_ok(r);
    else send_err(r, 409, err[0] ? err : "falta path");
}

static void api_flash_detect(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    const char *port = jstr(b, "port");
    bool ok = aos_flasher_detect(port && port[0] ? port : "uart.b");
    cJSON_Delete(b);
    if (ok) send_ok(r);
    else send_err(r, 409, "ya hay un trabajo en curso");
}

static void api_flash_log(aos_httpd_req_t *r)
{
    uint32_t from = (uint32_t)aos_httpd_query_int(r, "from", 0), first = aos_flasher_log_first(), count = aos_flasher_log_count();
    if (from < first) from = first;
    cJSON *o = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(o, "lines");
    char line[AOS_FLASHER_LOG_LINE + 8];
    for (uint32_t i = from; i < count; i++) {
        int lv = aos_flasher_log_line(i, line, sizeof line);
        if (lv < 0) continue;
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "t", line);
        cJSON_AddNumberToObject(l, "l", lv);
        cJSON_AddItemToArray(a, l);
    }
    cJSON_AddNumberToObject(o, "next", count);
    send_cjson(r, 200, o);
}

/* -------------------------------------------------------------------------- */
/* Terminal                                                                    */
/* -------------------------------------------------------------------------- */

static int req_ch(aos_httpd_req_t *r) { long c = aos_httpd_query_int(r, "ch", 0); return c == 1 ? 1 : 0; }

static void api_serial(aos_httpd_req_t *r)
{
    int ch = req_ch(r);
    aos_serial_stat_t st;
    aos_serial_stat(ch, &st);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ch", ch);
    cJSON_AddBoolToObject(o, "running", st.running);
    cJSON_AddStringToObject(o, "port", st.port);
    cJSON_AddStringToObject(o, "desc", st.desc);
    cJSON_AddNumberToObject(o, "baud", st.baud);
    cJSON_AddNumberToObject(o, "lines", st.lines);
    cJSON_AddNumberToObject(o, "first", st.first);
    cJSON_AddNumberToObject(o, "rx", (double)st.rx_bytes);
    cJSON_AddNumberToObject(o, "tx", (double)st.tx_bytes);
    cJSON_AddBoolToObject(o, "recording", st.recording);
    cJSON_AddStringToObject(o, "rec_path", st.rec_path);
    cJSON_AddNumberToObject(o, "rec_bytes", (double)st.rec_bytes);
    cJSON *ports = cJSON_AddArrayToObject(o, "ports");
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        if (pt->kind != AOS_PORT_UART) continue;
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "name", pt->name);
        const char *holder = aos_io_owner(pt->pins[0]);
        if (holder) cJSON_AddStringToObject(p, "holder", holder);
        cJSON_AddItemToArray(ports, p);
    }
    cJSON *tr = cJSON_AddArrayToObject(o, "triggers");
    for (int i = 0; i < aos_serial_trigger_count(); i++) {
        aos_serial_trigger_t t;
        if (!aos_serial_trigger_get(i, &t)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "text", t.text);
        cJSON_AddBoolToObject(e, "notify", t.flags & AOS_SERIAL_T_NOTIFY);
        cJSON_AddBoolToObject(e, "beep", t.flags & AOS_SERIAL_T_BEEP);
        cJSON_AddNumberToObject(e, "hits", t.hits);
        cJSON_AddItemToArray(tr, e);
    }
    send_cjson(r, 200, o);
}

static void api_serial_lines(aos_httpd_req_t *r)
{
    int ch = req_ch(r);
    aos_serial_stat_t st;
    aos_serial_stat(ch, &st);
    uint32_t from = (uint32_t)aos_httpd_query_int(r, "from", 0);
    if (from < st.first) from = st.first;
    if (st.lines - from > 800) from = st.lines - 800;       /* the tail is what a new page wants */
    /* built by hand: 800 lines through cJSON would be a lot of small mallocs */
    if (!aos_httpd_begin(r, 200, "application/json", -1, NULL)) return;
    char head[96];
    snprintf(head, sizeof head, "{\"first\":%lu,\"next\":%lu,\"lines\":[", (unsigned long)st.first, (unsigned long)st.lines);
    aos_httpd_write(r, head, strlen(head));
    char txt[AOS_SERIAL_LINE_MAX], out[AOS_SERIAL_LINE_MAX * 6 + 48];
    bool first = true;
    for (uint32_t i = from; i < st.lines; i++) {
        uint32_t t = 0;
        uint8_t f = 0;
        if (aos_serial_line_ex(ch, i, txt, sizeof txt, &t, &f) < 0) continue;
        int k = snprintf(out, sizeof out, "%s[%lu,%u,\"", first ? "" : ",", (unsigned long)t, f);
        for (const char *p = txt; *p && k < (int)sizeof out - 8; p++) {
            unsigned char c = (unsigned char)*p;
            if (c == '"' || c == '\\') { out[k++] = '\\'; out[k++] = (char)c; }
            else if (c < 0x20) k += snprintf(out + k, sizeof out - (size_t)k, "\\u%04x", c);
            else out[k++] = (char)c;
        }
        out[k++] = '"';
        out[k++] = ']';
        if (!aos_httpd_write(r, out, (size_t)k)) return;
        first = false;
    }
    aos_httpd_write(r, "]}", 2);
}

static void api_serial_post(aos_httpd_req_t *r, const char *op)
{
    cJSON *b = body_json(r);
    const cJSON *chv = cJSON_GetObjectItemCaseSensitive(b, "ch");
    int ch = cJSON_IsNumber(chv) && chv->valueint == 1 ? 1 : 0;
    bool ok = true;
    const char *err = "";
    if (!strcmp(op, "start")) {
        const char *port = jstr(b, "port");
        const cJSON *baud = cJSON_GetObjectItemCaseSensitive(b, "baud");
        ok = port && cJSON_IsNumber(baud) && aos_serial_start(ch, port, (uint32_t)baud->valueint);
        if (!ok) err = "no se pudo abrir el puerto (¿lo tiene otra app?)";
    } else if (!strcmp(op, "stop")) {
        aos_serial_stop(ch);
    } else if (!strcmp(op, "send")) {
        const char *text = jstr(b, "text"), *eol = jstr(b, "eol");
        char buf[600];
        int n = snprintf(buf, sizeof buf, "%s%s", text ? text : "", eol ? eol : "\r\n");
        if (n > (int)sizeof buf - 1) n = (int)sizeof buf - 1;
        ok = aos_serial_send(ch, buf, n) >= 0;
        if (!ok) err = "el canal no está conectado";
    } else if (!strcmp(op, "record")) {
        ok = aos_serial_record(ch, cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "on")));
        if (!ok) err = "hace falta el canal conectado y la tarjeta";
    } else if (!strcmp(op, "trigger")) {
        const char *add = jstr(b, "add");
        const cJSON *idx = cJSON_GetObjectItemCaseSensitive(b, "index");
        const cJSON *flags = cJSON_GetObjectItemCaseSensitive(b, "flags");
        if (add) ok = aos_serial_trigger_add(add, cJSON_IsNumber(flags) ? (uint8_t)flags->valueint : AOS_SERIAL_T_NOTIFY) >= 0;
        else if (cJSON_IsNumber(idx) && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "remove"))) aos_serial_trigger_remove(idx->valueint);
        else if (cJSON_IsNumber(idx) && jstr(b, "text")) { ok = aos_serial_trigger_rename(idx->valueint, jstr(b, "text")); if (!ok) err = "texto vacío"; }
        else if (cJSON_IsNumber(idx) && cJSON_IsNumber(flags)) aos_serial_trigger_set(idx->valueint, (uint8_t)flags->valueint);
        if (!ok && !err[0]) err = "ya hay 8 avisos";
    }
    cJSON_Delete(b);
    if (ok) send_ok(r);
    else send_err(r, 409, err);
}

/* -------------------------------------------------------------------------- */
/* Settings                                                                    */
/* -------------------------------------------------------------------------- */

static void set_wallpaper(void *arg) { aos_ui_set_wallpaper((int)(intptr_t)arg); }

/* What plays and how the pipeline keeps up: the ring's level and low
 * mark, the underruns, the decoder's cost and its slowest chunk. */
static void api_player(aos_httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    aos_player_info_t in;
    if (aos_hal_player_info(&in)) {
        static const char *const states[] = { "stopped", "playing", "paused" };
        cJSON_AddStringToObject(o, "state", (unsigned)in.state < 3 ? states[in.state] : "?");
        cJSON_AddStringToObject(o, "path", in.path);
        cJSON_AddStringToObject(o, "format", in.format ? in.format : "");
        cJSON_AddNumberToObject(o, "kbps", in.kbps);
        cJSON_AddNumberToObject(o, "rate", in.sample_rate);
        cJSON_AddNumberToObject(o, "position_ms", in.position_ms);
        cJSON_AddNumberToObject(o, "duration_ms", in.duration_ms);
    }
    aos_player_stats_t st;
    if (aos_hal_player_stats(&st)) {
        cJSON *p = cJSON_AddObjectToObject(o, "stats");
        cJSON_AddNumberToObject(p, "ring_ms", st.ring_ms);
        cJSON_AddNumberToObject(p, "ring_cap_ms", st.ring_cap_ms);
        cJSON_AddNumberToObject(p, "ring_min_ms", st.ring_min_ms);
        cJSON_AddNumberToObject(p, "underruns", st.underruns);
        cJSON_AddNumberToObject(p, "load_permille", st.load_permille);
        cJSON_AddNumberToObject(p, "decode_permille", st.decode_permille);
        cJSON_AddNumberToObject(p, "chunk_us_max", st.chunk_us_max);
        cJSON_AddNumberToObject(p, "decoder_prio", st.decoder_prio);
        cJSON_AddNumberToObject(p, "stack_free_dec", st.stack_free_dec);
        cJSON_AddNumberToObject(p, "stack_free_out", st.stack_free_out);
    }
    send_cjson(r, 200, o);
}

static void api_settings(aos_httpd_req_t *r)
{
    if (!strcmp(aos_httpd_method(r), "POST")) {
        cJSON *b = body_json(r);
        if (!b) { send_err(r, 400, "json"); return; }
        const char *err = NULL;
        const char *v;
        const cJSON *n;
        if ((v = jstr(b, "name")) && !aos_hal_device_name_set(v)) err = "el nombre va con a-z, 0-9 y guiones";
        if ((v = jstr(b, "tz"))) aos_hal_timezone_set(v);
        if ((v = jstr(b, "lang"))) aos_ui_request_language(v);
        if (cJSON_IsNumber(n = cJSON_GetObjectItemCaseSensitive(b, "wallpaper")))
            aos_ui_request_call(set_wallpaper, (void *)(intptr_t)n->valueint);
        if (cJSON_IsNumber(n = cJSON_GetObjectItemCaseSensitive(b, "brightness"))) aos_hal_brightness_set(n->valueint);
        if (cJSON_IsNumber(n = cJSON_GetObjectItemCaseSensitive(b, "volume"))) aos_hal_volume_set(n->valueint);
        if (cJSON_IsBool(n = cJSON_GetObjectItemCaseSensitive(b, "landscape"))) aos_ui_request_landscape(cJSON_IsTrue(n));
        cJSON_Delete(b);
        if (err) send_err(r, 400, err);
        else send_ok(r);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", aos_hal_device_name());
    cJSON_AddStringToObject(o, "tz", aos_hal_timezone_get());
    cJSON_AddStringToObject(o, "lang", aos_i18n_current());
    cJSON_AddNumberToObject(o, "wallpaper", aos_ui_wallpaper());
    cJSON *wp = cJSON_AddArrayToObject(o, "wallpapers");
    for (int i = 0; i < AOS_UI_WALLPAPERS; i++) {
        uint32_t a, b;
        aos_ui_wallpaper_colors(i, &a, &b);
        char c[24];
        snprintf(c, sizeof c, "#%06X,#%06X", (unsigned)a, (unsigned)b);
        cJSON_AddItemToArray(wp, cJSON_CreateString(c));
    }
    cJSON_AddNumberToObject(o, "brightness", aos_hal_brightness_get());
    cJSON_AddNumberToObject(o, "volume", aos_hal_volume_get());
    cJSON_AddBoolToObject(o, "landscape", aos_ui_landscape());
    int32_t cla, clo;           /* Clima's city: where #mapas starts with no zones, as the app does */
    if (aos_hal_pref_get_i32("clima_lat", &cla) && aos_hal_pref_get_i32("clima_lon", &clo)) {
        cJSON_AddNumberToObject(o, "clima_lat10k", cla);
        cJSON_AddNumberToObject(o, "clima_lon10k", clo);
    }
    aos_lang_t langs[AOS_LANG_MAX];
    int nl = aos_i18n_scan(langs, AOS_LANG_MAX);
    cJSON *la = cJSON_AddArrayToObject(o, "langs");
    for (int i = 0; i < nl; i++) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "code", langs[i].code);
        cJSON_AddStringToObject(l, "name", langs[i].name);
        cJSON_AddItemToArray(la, l);
    }
    send_cjson(r, 200, o);
}

static void api_log(aos_httpd_req_t *r)
{
    size_t from = (size_t)aos_httpd_query_int(r, "from", 0), next = 0;
    bool prev = aos_httpd_query_int(r, "prev", 0) != 0;     /* the previous boot's tail */
    char *buf = malloc(16384);
    if (!buf) { send_err(r, 500, "memory"); return; }
    size_t n = prev ? aos_hal_log_prev_read(from, buf, 16384, &next) : aos_hal_log_read(from, buf, 16384, &next);
    char extra[64];
    snprintf(extra, sizeof extra, "X-Log-Next: %lu\r\n", (unsigned long)next);
    if (aos_httpd_begin(r, 200, "text/plain; charset=utf-8", (long)n, extra)) aos_httpd_write(r, buf, n);
    free(buf);
}

static void restart_later(void *arg) { aos_hal_reboot(); }

static void api_restart(aos_httpd_req_t *r)
{
    send_ok(r);
    aos_ui_request_call(restart_later, NULL);
}

/* -------------------------------------------------------------------------- */
/* Routing                                                                     */
/* -------------------------------------------------------------------------- */

/* The apps' own portal pages: /web/<file> comes from the card's /web, where
 * tools/install_apps.sh puts each app's apps/<x>/web/ (docs/PORTAL-PAGES.md).
 * So an app brings its page the way it brings its icon, and updating it
 * needs no firmware. A name of [A-Za-z0-9._-] only, one level deep: nothing
 * outside /web can be reached through here. Not cached by the browser (the
 * server sends no-store), and app.js asks for each page with its date in
 * the address anyway. */
static void serve_card_web(aos_httpd_req_t *r, const char *name)
{
    if (!*name || name[0] == '.' || strlen(name) > 64) { aos_httpd_send_text(r, 404, "no existe"); return; }
    for (const char *c = name; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '.' && *c != '_' && *c != '-') {
            aos_httpd_send_text(r, 404, "no existe");
            return;
        }
    const char *root = aos_hal_path_sd_root();
    char path[128];
    struct stat st;
    snprintf(path, sizeof path, "%s/web/%s", root ? root : "", name);
    FILE *f = root ? fopen(path, "rb") : NULL;
    if (!f || stat(path, &st) || S_ISDIR(st.st_mode)) {
        if (f) fclose(f);
        aos_httpd_send_text(r, 404, "no existe");
        return;
    }
    if (aos_httpd_begin(r, 200, ctype_of(name), (long)st.st_size, NULL)) {      /* the server says no-store already */
        char *buf = aos_hal_io_alloc(16384);
        ssize_t n;
        while (buf && (n = read(fileno(f), buf, 16384)) > 0)
            if (!aos_httpd_write(r, buf, (size_t)n)) break;
        aos_hal_io_free(buf);
    }
    fclose(f);
}

static void serve_asset(aos_httpd_req_t *r, const char *path)
{
    if (!strcmp(path, "/")) path = "/index.html";
    for (int i = 0; i < aos_web_asset_count; i++) {
        const aos_web_asset_t *a = &aos_web_assets[i];
        if (strcmp(a->path, path)) continue;
        if (aos_httpd_begin(r, 200, a->ctype, a->len, "Content-Encoding: gzip\r\n")) aos_httpd_write(r, a->data, a->len);
        return;
    }
    if (!strncmp(path, "/web/", 5)) { serve_card_web(r, path + 5); return; }
    aos_httpd_send_text(r, 404, "no existe");
}

static void handler(aos_httpd_req_t *r)
{
    const char *p = aos_httpd_path(r), *m = aos_httpd_method(r);
    bool post = !strcmp(m, "POST"), get = !strcmp(m, "GET");
    if (!aos_portal_access(r) || !aos_portal_rules(r)) return;
    if (strncmp(p, "/api/", 5)) { serve_asset(r, p); return; }
    p += 5;
    if (get && !strcmp(p, "info")) api_info(r);
    else if (get && !strcmp(p, "screen.bmp")) api_screen(r);
    else if (post && !strcmp(p, "touch")) api_touch(r);
    else if (post && !strcmp(p, "nav")) api_nav(r);
    else if (post && !strcmp(p, "open")) api_open(r);
    else if (get && !strcmp(p, "apps")) api_apps(r);
    else if (get && !strcmp(p, "apps/icon")) api_app_icon(r);
    else if (!strcmp(p, "wifi")) api_wifi(r);
    else if (get && !strcmp(p, "wifi/scan")) api_wifi_scan(r);
    else if (post && !strcmp(p, "wifi/forget")) { aos_hal_net_forget(); send_ok(r); }
    else if (post && !strcmp(p, "wifi/ap")) {          /* {"on": true|false}, as the switch in Settings */
        cJSON *b = body_json(r);
        cJSON *on = b ? cJSON_GetObjectItem(b, "on") : NULL;
        bool want = cJSON_IsTrue(on), ok = cJSON_IsBool(on);
        cJSON_Delete(b);
        if (!ok) send_err(r, 400, "falta \"on\"");
        else if (want && !aos_hal_net_ap_start()) send_err(r, 409, "la radio está ocupada");
        else { if (!want) aos_hal_net_ap_stop(); send_ok(r); }
    }
    else if (!strcmp(p, "bt")) api_bt(r);
    else if (get && !strcmp(p, "c6")) api_c6(r);
    else if (post && !strcmp(p, "c6/update")) api_c6_update(r);
    else if (post && !strcmp(p, "wifi/linktest")) {    /* tests the link watchdog (docs/BUILDING.md) */
        if (aos_hal_net_test_freeze_link()) send_ok(r);
        else send_err(r, 404, "no hay enlace que congelar");
    }
    else if (!strcmp(p, "ha")) api_ha(r);
    else if (get && !strcmp(p, "ha/entities")) api_ha_entities(r);
    else if (post && !strcmp(p, "ha/fav")) api_ha_fav(r);
    else if (get && !strcmp(p, "fs")) api_fs_list(r);
    else if (get && !strcmp(p, "fs/get")) api_fs_get(r);
    else if (get && !strcmp(p, "fs/bench")) api_fs_bench(r);
    else if (!strcmp(m, "PUT") && !strcmp(p, "fs/put")) api_fs_put(r);
    else if (post && !strcmp(p, "fs/mkdir")) api_fs_op(r, "mkdir");
    else if (post && !strcmp(p, "fs/delete")) api_fs_op(r, "delete");
    else if (post && !strcmp(p, "fs/rename")) api_fs_op(r, "rename");
    else if (!strcmp(p, "settings")) api_settings(r);
    else if (get && !strcmp(p, "flash")) api_flash(r);
    else if (post && !strcmp(p, "flash/start")) api_flash_start(r);
    else if (post && !strcmp(p, "flash/detect")) api_flash_detect(r);
    else if (post && !strcmp(p, "flash/cancel")) { aos_flasher_cancel(); send_ok(r); }
    else if (get && !strcmp(p, "flash/log")) api_flash_log(r);
    else if (get && !strcmp(p, "serial")) api_serial(r);
    else if (get && !strcmp(p, "serial/lines")) api_serial_lines(r);
    else if (post && !strncmp(p, "serial/", 7)) api_serial_post(r, p + 7);
    else if (aos_portal_bench(r, m, p)) {}
    else if (aos_portal_radio(r, m, p)) {}
    else if (aos_portal_net(r, m, p)) {}
    else if (aos_portal_claude(r, m, p)) {}
    else if (aos_portal_mqtt(r, m, p)) {}
    else if (aos_portal_sysmon(r, m, p)) {}
    else if (aos_portal_system(r, m, p)) {}
    else if (aos_portal_macropad(r, m, p)) {}
    else if (aos_portal_modules(r, m, p)) {}
    else if (get && !strcmp(p, "player")) api_player(r);
    else if (get && !strcmp(p, "log")) api_log(r);
    else if (post && !strcmp(p, "restart")) api_restart(r);
    else send_err(r, 404, "no existe");
}

/* HTTPS, when Settings, Portal web turned it on: on 443 (in the simulator,
 * on P4_SIM_HTTPS_PORT, or not at all). In a thread of its own, because the
 * first time it makes the board's certificate (an ECDSA key, ~1 s). */
static int s_https_port;

static void https_up(void *arg)
{
    (void)arg;
    unsigned char *cert = NULL, *key = NULL;
    size_t cl = 0, kl = 0;
    if (aos_access_tls_der(&cert, &cl, &key, &kl) && aos_httpd_start_tls(s_https_port, cert, cl, key, kl))
        aos_portal_set_https_port(s_https_port);
    if (key) memset(key, 0, kl);
    free(cert);
    free(key);
}

bool aos_portal_start(int port)
{
    if (!s_shot_mx) s_shot_mx = aos_hal_mutex_create();
    bool ok = aos_httpd_start(port, handler);
#ifdef AOS_SIM
    s_https_port = getenv("P4_SIM_HTTPS_PORT") ? atoi(getenv("P4_SIM_HTTPS_PORT")) : 0;
#else
    s_https_port = 443;
#endif
    if (ok && s_https_port && aos_access_https()) aos_hal_thread_start("https_up", https_up, NULL, 8192, 3);
    return ok;
}
