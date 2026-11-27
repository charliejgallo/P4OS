/*
 * P4OS - the portal's Bot page: the Telegram bot's settings from the
 * browser (aos_portal.c hands every /api/bot* request here).
 *
 *   GET  /api/bot          state, chats, commands, rules (each with its
 *                          sentence), the log, and the Zigbee devices and IR
 *                          codes a rule can name. Never the token.
 *   POST /api/bot          {on?, token?, cmds?, link?, link_cancel?, forget?}
 *   POST /api/bot/rule     {i (-1 new), on, trig, ieee, var, op, value, at_min,
 *                          days, filter, act, act_ieee, ir, text, gap_s}
 *   POST /api/bot/delete   {i}
 *
 * A token typed here travels to the board in the request: over the portal's
 * HTTPS when it is on (docs/SECURITY.md), as every other secret the portal
 * takes (Home Assistant's, the Wi-Fi's).
 */
#include "aos_portal_bot.h"
#include "aos_bot.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_zigbee.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(AOS_SIM)
#include "esp_heap_caps.h"
static void *big_calloc(size_t n)
{
    void *p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : calloc(1, n);
}
#else
static void *big_calloc(size_t n) { return calloc(1, n); }
#endif

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

static cJSON *body_json(aos_httpd_req_t *r)
{
    char *b = aos_httpd_body_all(r, 4096);
    cJSON *o = b ? cJSON_Parse(b) : NULL;
    free(b);
    return o;
}

static void hex_of(const uint8_t ieee[8], char out[17])
{
    for (int i = 0; i < 8; i++) snprintf(out + 2 * i, 3, "%02x", ieee[i]);
}

static void hex_in(const char *s, uint8_t out[8])
{
    memset(out, 0, 8);
    for (int i = 0; i < 8 && s && s[2 * i] && s[2 * i + 1]; i++) {
        char h[3] = { s[2 * i], s[2 * i + 1], 0 };
        out[i] = (uint8_t)strtoul(h, NULL, 16);
    }
}

static const char *state_name(aos_bot_state_t s)
{
    switch (s) {
    case AOS_BOT_OFF: return "off";
    case AOS_BOT_NO_TOKEN: return "no_token";
    case AOS_BOT_WAITING_NET: return "waiting_net";
    case AOS_BOT_CONNECTING: return "connecting";
    case AOS_BOT_ONLINE: return "online";
    case AOS_BOT_BAD_TOKEN: return "bad_token";
    default: return "error";
    }
}

static void api_status(aos_httpd_req_t *r)
{
    aos_bot_info_t in;
    aos_bot_info(&in);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", state_name(in.state));
    cJSON_AddStringToObject(o, "username", in.username);
    cJSON_AddStringToObject(o, "why", in.why);
    cJSON_AddBoolToObject(o, "on", aos_bot_enabled());
    cJSON_AddBoolToObject(o, "token", aos_bot_has_token());
    cJSON_AddNumberToObject(o, "received", in.received);
    cJSON_AddNumberToObject(o, "sent", in.sent);
    cJSON_AddNumberToObject(o, "code", in.code);
    cJSON_AddNumberToObject(o, "code_left", in.code_left_s);
    cJSON_AddNumberToObject(o, "cmds", aos_bot_cmds());
    aos_bot_chat_t ch[AOS_BOT_CHATS_MAX];
    int n = aos_bot_chats(ch, AOS_BOT_CHATS_MAX);
    cJSON *a = cJSON_AddArrayToObject(o, "chats");
    for (int i = 0; i < n; i++) {
        cJSON *c = cJSON_CreateObject();
        char id[24];
        snprintf(id, sizeof id, "%lld", (long long)ch[i].id);
        cJSON_AddStringToObject(c, "id", id);
        cJSON_AddStringToObject(c, "name", ch[i].name);
        cJSON_AddItemToArray(a, c);
    }
    static aos_bot_rule_t rl[AOS_BOT_RULES_MAX];
    n = aos_bot_rules(rl, AOS_BOT_RULES_MAX);
    a = cJSON_AddArrayToObject(o, "rules");
    for (int i = 0; i < n; i++) {
        cJSON *c = cJSON_CreateObject();
        char h[17], t[300];
        cJSON_AddBoolToObject(c, "on", rl[i].on);
        cJSON_AddNumberToObject(c, "trig", rl[i].trig);
        hex_of(rl[i].ieee, h);
        cJSON_AddStringToObject(c, "ieee", h);
        cJSON_AddNumberToObject(c, "var", rl[i].var);
        cJSON_AddNumberToObject(c, "op", rl[i].op);
        cJSON_AddNumberToObject(c, "value", rl[i].value);
        cJSON_AddNumberToObject(c, "at_min", rl[i].at_min);
        cJSON_AddNumberToObject(c, "days", rl[i].days);
        cJSON_AddStringToObject(c, "filter", rl[i].filter);
        cJSON_AddNumberToObject(c, "act", rl[i].act);
        hex_of(rl[i].act_ieee, h);
        cJSON_AddStringToObject(c, "act_ieee", h);
        cJSON_AddStringToObject(c, "ir", rl[i].ir);
        cJSON_AddStringToObject(c, "text", rl[i].text);
        cJSON_AddNumberToObject(c, "gap_s", rl[i].gap_s);
        aos_bot_rule_text(&rl[i], t, sizeof t);
        cJSON_AddStringToObject(c, "desc", t);
        cJSON_AddItemToArray(a, c);
    }
    static char lines[40][96];
    n = aos_bot_log(lines, 40);
    a = cJSON_AddArrayToObject(o, "log");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(lines[i]));
    /* what a rule can name */
    aos_zigbee_dev_t *zb = big_calloc(sizeof(aos_zigbee_dev_t) * AOS_ZB_DEVICES_MAX);
    a = cJSON_AddArrayToObject(o, "zigbee");
    int nd = zb ? aos_zigbee_devices(zb, AOS_ZB_DEVICES_MAX) : 0;
    for (int i = 0; i < nd; i++) {
        cJSON *c = cJSON_CreateObject();
        char h[17], name[40];
        hex_of(zb[i].ieee, h);
        if (zb[i].name[0]) snprintf(name, sizeof name, "%s", zb[i].name);
        else if (zb[i].model[0]) snprintf(name, sizeof name, "%s", zb[i].model);
        else snprintf(name, sizeof name, "0x%02x%02x%02x", zb[i].ieee[2], zb[i].ieee[1], zb[i].ieee[0]);
        cJSON_AddStringToObject(c, "ieee", h);
        cJSON_AddStringToObject(c, "name", name);
        cJSON_AddBoolToObject(c, "out", zb[i].onoff_eps != 0);
        cJSON_AddItemToArray(a, c);
    }
    free(zb);
    static aos_zigbee_ir_code_t ir[24];
    n = aos_zigbee_ir_codes(ir, 24);
    a = cJSON_AddArrayToObject(o, "ir");
    for (int i = 0; i < n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(ir[i].name));
    send_cjson(r, 200, o);
}

static void api_set(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    if (!b) { send_err(r, 400, _("falta el cuerpo")); return; }
    const cJSON *v;
    if ((v = cJSON_GetObjectItemCaseSensitive(b, "token")) && cJSON_IsString(v) && !aos_bot_set_token(v->valuestring)) {
        cJSON_Delete(b);
        send_err(r, 400, _("Eso no parece un token de bot (número:letras)"));
        return;
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(b, "on"))) aos_bot_enable(cJSON_IsTrue(v));
    if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(b, "cmds"))) aos_bot_set_cmds((uint32_t)v->valuedouble);
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "link"))) aos_bot_link_code();
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(b, "link_cancel"))) aos_bot_link_cancel();
    if (cJSON_IsString(v = cJSON_GetObjectItemCaseSensitive(b, "forget"))) aos_bot_chat_forget(strtoll(v->valuestring, NULL, 10));
    cJSON_Delete(b);
    aos_httpd_send_json(r, 200, "{\"ok\":true}");
}

static int num(const cJSON *b, const char *k, int def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(b, k);
    return cJSON_IsNumber(v) ? v->valueint : def;
}

static void str(const cJSON *b, const char *k, char *out, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(b, k);
    snprintf(out, n, "%s", cJSON_IsString(v) ? v->valuestring : "");
}

static void api_rule(aos_httpd_req_t *r)
{
    cJSON *b = body_json(r);
    if (!b) { send_err(r, 400, _("falta el cuerpo")); return; }
    aos_bot_rule_t x = { 0 };
    const cJSON *v;
    x.on = !cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(b, "on"));
    x.trig = (uint8_t)num(b, "trig", 0);
    x.var = (uint8_t)num(b, "var", 0);
    x.op = (uint8_t)num(b, "op", 0);
    x.value = cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(b, "value")) ? (float)v->valuedouble : 0;
    x.at_min = (uint16_t)num(b, "at_min", 480);
    x.days = (uint8_t)num(b, "days", 0x7F);
    x.act = (uint8_t)num(b, "act", 0);
    x.gap_s = (uint16_t)num(b, "gap_s", 30);
    char h[24];
    str(b, "ieee", h, sizeof h);
    hex_in(h, x.ieee);
    str(b, "act_ieee", h, sizeof h);
    hex_in(h, x.act_ieee);
    str(b, "filter", x.filter, sizeof x.filter);
    str(b, "ir", x.ir, sizeof x.ir);
    str(b, "text", x.text, sizeof x.text);
    int i = num(b, "i", -1);
    cJSON_Delete(b);
    if (!aos_bot_rule_set(i, &x)) { send_err(r, 400, _("No pude guardar la regla")); return; }
    aos_httpd_send_json(r, 200, "{\"ok\":true}");
}

bool aos_portal_bot(aos_httpd_req_t *r, const char *m, const char *p)
{
    bool post = !strcmp(m, "POST"), get = !strcmp(m, "GET");
    if (strncmp(p, "bot", 3) || (p[3] && p[3] != '/')) return false;
    p += 3;
    if (get && !*p) api_status(r);
    else if (post && !*p) api_set(r);
    else if (post && !strcmp(p, "/rule")) api_rule(r);
    else if (post && !strcmp(p, "/delete")) {
        cJSON *b = body_json(r);
        int i = b ? num(b, "i", -1) : -1;
        cJSON_Delete(b);
        if (aos_bot_rule_delete(i)) aos_httpd_send_json(r, 200, "{\"ok\":true}");
        else send_err(r, 400, _("Esa regla ya no está"));
    } else return false;
    return true;
}
