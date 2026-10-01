/*
 * P4OS - the portal's side of the Macro pad: its layout, edited with a
 * mouse and a real keyboard, and a way to try a button from the browser.
 *
 *   GET  /api/macropad              {layout, glyphs, saved, usb, status}
 *   PUT  /api/macropad              the body is the whole layout (checked, saved)
 *   POST /api/macropad/reset        back to the default layout
 *   POST /api/macropad/run?page=&slot=   plays a saved button, as a tap would
 *   GET  /api/macropad/key?name=    {valid}: whether the board knows that key
 *
 * The layout is aos_macropad.c's document (aos_macropad.h has the format);
 * the app on the screen notices the new version and redraws.
 */
#include "aos_portal_macropad.h"
#include "aos_macropad.h"
#include "aos_hal.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void send_err(aos_httpd_req_t *r, int status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddFalseToObject(o, "ok");
    cJSON_AddStringToObject(o, "error", msg);
    send_cjson(r, status, o);
}

static void api_get(aos_httpd_req_t *r)
{
    char *txt = aos_macropad_print();
    cJSON *layout = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "layout", layout ? layout : cJSON_CreateNull());
    cJSON *g = cJSON_AddArrayToObject(o, "glyphs");
    for (int i = 0; i < aos_macropad_glyph_count(); i++) cJSON_AddItemToArray(g, cJSON_CreateString(aos_macropad_glyph_name(i)));
    cJSON_AddBoolToObject(o, "saved", aos_macropad_saved());
    cJSON_AddNumberToObject(o, "slots", AOS_MP_SLOTS);
    cJSON_AddNumberToObject(o, "max_pages", AOS_MP_PAGES);
    cJSON_AddStringToObject(o, "usb", aos_hal_usb_keys_ready() ? "ready" : aos_hal_usb_busy() ? "busy"
                                     : aos_hal_usb_mode() == AOS_HAL_USB_KEYS ? "waiting" : "off");
    aos_mp_status_t st;
    aos_macropad_status(&st);
    cJSON *s = cJSON_AddObjectToObject(o, "status");
    cJSON_AddStringToObject(s, "msg", st.msg);
    cJSON_AddBoolToObject(s, "ok", st.ok);
    cJSON_AddNumberToObject(s, "seq", st.seq);
    cJSON_AddBoolToObject(s, "busy", st.busy);
    cJSON_AddNumberToObject(o, "version", aos_macropad_version());
    send_cjson(r, 200, o);
}

bool aos_portal_macropad(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strncmp(p, "macropad", 8)) return false;
    p += 8;
    bool post = !strcmp(m, "POST"), get = !strcmp(m, "GET"), put = !strcmp(m, "PUT");
    if (*p && *p != '/') return false;
    if (get && !*p) { aos_macropad_load(); api_get(r); return true; }
    if ((put || post) && !*p) {
        char *b = aos_httpd_body_all(r, 128 * 1024);
        char err[96];
        if (!b) { send_err(r, 400, "sin cuerpo, o de más de 128 KB"); return true; }
        bool ok = aos_macropad_replace(b, err, sizeof err);
        free(b);
        if (!ok) { send_err(r, 400, err); return true; }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddTrueToObject(o, "ok");
        cJSON_AddBoolToObject(o, "saved", aos_macropad_saved());
        cJSON_AddNumberToObject(o, "version", aos_macropad_version());
        send_cjson(r, 200, o);
        return true;
    }
    if (post && !strcmp(p, "/reset")) { aos_macropad_reset(); aos_httpd_send_json(r, 200, "{\"ok\":true}"); return true; }
    if (post && !strcmp(p, "/run")) {
        long page = aos_httpd_query_int(r, "page", -1), slot = aos_httpd_query_int(r, "slot", -1);
        if (page < 0 || page >= AOS_MP_PAGES || slot < 0 || slot >= AOS_MP_SLOTS) { send_err(r, 400, "page y slot"); return true; }
        aos_macropad_lock();
        bool there = aos_macropad_button((int)page, (int)slot) != NULL;
        aos_macropad_unlock();
        if (!there) { send_err(r, 404, "ese lugar está vacío (¿guardaste?)"); return true; }
        aos_macropad_request_run((int)page, (int)slot);
        aos_httpd_send_json(r, 200, "{\"ok\":true}");
        return true;
    }
    if (get && !strcmp(p, "/key")) {
        char name[64] = "";
        aos_httpd_query(r, "name", name, sizeof name);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "valid", aos_hal_usb_key_valid(name));
        send_cjson(r, 200, o);
        return true;
    }
    return false;
}
