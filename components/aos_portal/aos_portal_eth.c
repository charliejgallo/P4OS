/*
 * P4OS - the portal's Ethernet API (aos_eth.h, docs/plan/ETHERNET.md).
 *
 *   GET  /api/eth                       the state: on, running, link, address, why not
 *   POST /api/eth {"on": true|false}    switch it, kept across restarts
 *   POST /api/eth {"hz": 20000000}      the SPI clock, for the next start
 */
#include "aos_portal_eth.h"
#include "aos_eth.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void send_state(aos_httpd_req_t *r, int status, bool ok)
{
    aos_eth_info_t i;
    aos_eth_info(&i);
    char mac[18];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", i.mac[0], i.mac[1], i.mac[2], i.mac[3], i.mac[4], i.mac[5]);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddBoolToObject(o, "on", i.enabled);
    cJSON_AddBoolToObject(o, "running", i.running);
    cJSON_AddBoolToObject(o, "link", i.link);
    cJSON_AddNumberToObject(o, "speed", i.speed_mbps);
    cJSON_AddBoolToObject(o, "full_duplex", i.full_duplex);
    cJSON_AddStringToObject(o, "ip", i.ip);
    cJSON_AddStringToObject(o, "mask", i.mask);
    cJSON_AddStringToObject(o, "gw", i.gw);
    cJSON_AddStringToObject(o, "mac", i.running ? mac : "");
    cJSON_AddStringToObject(o, "port", i.port);
    cJSON_AddNumberToObject(o, "cs", i.cs);
    cJSON_AddNumberToObject(o, "spi_hz", i.spi_hz);
    cJSON_AddNumberToObject(o, "link_s", i.link_s);
    cJSON_AddNumberToObject(o, "links", i.links);
    cJSON_AddStringToObject(o, "error", i.err);
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

bool aos_portal_eth(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strcmp(p, "eth")) return false;
    if (!strcmp(m, "GET")) { send_state(r, 200, true); return true; }
    if (strcmp(m, "POST")) return false;
    char *b = aos_httpd_body_all(r, 512);
    cJSON *o = b ? cJSON_Parse(b) : NULL;
    free(b);
    if (!o) { aos_httpd_send_json(r, 400, "{\"ok\":false,\"error\":\"JSON\"}"); return true; }
    bool ok = true;
    const cJSON *hz = cJSON_GetObjectItem(o, "hz"), *on = cJSON_GetObjectItem(o, "on");
    if (cJSON_IsNumber(hz)) aos_eth_set_clock((uint32_t)hz->valuedouble);
    if (cJSON_IsBool(on)) ok = aos_eth_enable(cJSON_IsTrue(on));
    cJSON_Delete(o);
    send_state(r, 200, ok);
    return true;
}
