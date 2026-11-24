/*
 * P4OS - /api/154: the C6's 802.15.4 radio (components/aos_154,
 * docs/plan/ZIGBEE.md), for the portal and for scripts.
 *
 *   GET  /api/154                  the radio's state and every channel's last
 *                                  measure (energy in dBm, frames heard)
 *   POST /api/154/hello            asks the C6 how it is (the answer shows in
 *                                  the state a moment later)
 *   POST /api/154/scan             {"first": 11, "last": 26, "dwell": 200}
 *   POST /api/154/sniff            {"channel": 15, "pcap": true}: listens on
 *                                  it; pcap writes a capture to the card's
 *                                  /zigbee, for Wireshark
 *   POST /api/154/stop
 *   POST /api/154/unguard          lets the radio on again after the C6's
 *                                  guard kept it off
 *   GET  /api/154/frames?from=N    the frames kept from N on (200 at most),
 *                                  each with its bytes in hex
 *   GET  /api/154/devices          every device heard: PAN, short and long
 *                                  address, channel, frames, RSSI, when
 *   POST /api/154/devices/clear    forgets them
 *   POST /api/154/zigbee           {"on": true, "channel": 26}: Espressif's
 *                                  Zigbee stack on the C6, a coordinator with
 *                                  a network of its own, closed (a measuring
 *                                  step: the state says the C6's memory before
 *                                  and after); {"on": false} stops it
 *   POST /api/154/c6log            {"on": true}: the C6's log lines into the
 *                                  board's log, as "c6: ..."
 *   GET  /api/154/slots            the C6's app slots: its partition table
 *                                  ("factory", 1.5 MB, or "grown", 1.94 MB),
 *                                  the size in use and where it runs; asks it
 *                                  again, so call twice a moment apart
 *   POST /api/154/slots/grow       writes the repository's table over the
 *                                  factory one (docs/C6.md, "The slots")
 *   POST /api/154/rcp              the C6 becomes a radio co-processor for a
 *                                  stack on the P4 (until it restarts)
 *   GET  /api/154/rcp/probe        asks the RCP its version in spinel, through
 *                                  the whole link
 *   POST /api/154/zbhost           {"channel": 26}: Espressif's Zigbee stack on
 *                                  the P4 with the C6 as its radio, a
 *                                  coordinator forming a network of its own,
 *                                  closed (a measuring step); its state in
 *                                  GET /api/154 as "zbhost", with the
 *                                  devices that joined ("devices") and the
 *                                  seconds it stays open ("open_s")
 *   POST /api/154/zbpermit         {"seconds": 180}: lets devices join for
 *                                  that long (0 closes it, at most 254)
 */
#include "aos_portal_154.h"
#include "aos_154.h"
#include "aos_154_proto.h"
#include "aos_zigbee.h"
#include "aos_hal.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FRAMES_MAX 200

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static const char *STATE[] = { "idle", "scanning", "sniffing", "zigbee", "rcp" };
static const char *ZB[] = { "off", "starting", "formed", "failed" };

static void api_state(aos_httpd_req_t *r, bool ok, const char *error)
{
    aos_154_info_t in;
    aos_154_info(&in);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    if (error) cJSON_AddStringToObject(o, "error", error);
    cJSON_AddBoolToObject(o, "reachable", in.reachable);
    cJSON_AddNumberToObject(o, "proto", in.proto);
    cJSON_AddStringToObject(o, "state", in.state < 5 ? STATE[in.state] : "?");
    cJSON_AddNumberToObject(o, "channel", in.channel);
    cJSON_AddBoolToObject(o, "guarded", in.guarded);
    cJSON_AddNumberToObject(o, "faults", in.faults);
    cJSON_AddNumberToObject(o, "err", in.err);
    cJSON_AddNumberToObject(o, "frames", in.frames);
    cJSON_AddNumberToObject(o, "dropped", in.dropped);
    cJSON_AddNumberToObject(o, "last", in.last_seq);
    cJSON_AddNumberToObject(o, "scan_seq", in.scan_seq);
    cJSON_AddStringToObject(o, "pcap", in.pcap);
    cJSON_AddNumberToObject(o, "pcap_frames", in.pcap_frames);
    cJSON_AddStringToObject(o, "c6", aos_hal_net_coprocessor_fw());
    if (in.heap_free) {
        cJSON_AddNumberToObject(o, "c6_heap_free", in.heap_free);
        cJSON_AddNumberToObject(o, "c6_heap_min", in.heap_min);
    }
    {
        static const char *ZS[] = { "off", "starting", "formed", "failed" };
        aos_zigbee_info_t z;
        aos_zigbee_info(&z);
        cJSON *h = cJSON_AddObjectToObject(o, "zbhost");
        cJSON_AddStringToObject(h, "state", z.state < 4 ? ZS[z.state] : "?");
        cJSON_AddNumberToObject(h, "channel", z.channel);
        cJSON_AddNumberToObject(h, "to_c6", z.to_c6);
        cJSON_AddNumberToObject(h, "from_c6", z.from_c6);
        cJSON_AddNumberToObject(h, "open_s", aos_zigbee_permit_left());
        static aos_zigbee_dev_t dev[AOS_ZB_DEVICES_MAX];     /* the portal's one task */
        static const char *STEP[] = { "endpoints", "basic", "done", "failed", "left" };
        int n = aos_zigbee_devices(dev, AOS_ZB_DEVICES_MAX);
        cJSON *a = cJSON_AddArrayToObject(h, "devices");
        for (int i = 0; i < n; i++) {
            const aos_zigbee_dev_t *d = &dev[i];
            char ieee[17];
            snprintf(ieee, sizeof ieee, "%02x%02x%02x%02x%02x%02x%02x%02x", d->ieee[7], d->ieee[6], d->ieee[5],
                     d->ieee[4], d->ieee[3], d->ieee[2], d->ieee[1], d->ieee[0]);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "ieee", ieee);
            cJSON_AddNumberToObject(e, "nwk", d->nwk);
            cJSON_AddStringToObject(e, "step", d->step < 5 ? STEP[d->step] : "?");
            cJSON_AddBoolToObject(e, "router", (d->cap & 0x02) != 0);
            cJSON_AddNumberToObject(e, "ep", d->ep);
            cJSON_AddNumberToObject(e, "eps", d->eps);
            cJSON_AddStringToObject(e, "manufacturer", d->manuf);
            cJSON_AddStringToObject(e, "model", d->model);
            cJSON_AddNumberToObject(e, "power", d->power);
            if (d->rssi) cJSON_AddNumberToObject(e, "rssi", d->rssi);
            cJSON_AddNumberToObject(e, "ago_s", (double)(((uint32_t)aos_hal_uptime_ms() - d->seen_ms) / 1000));
            cJSON_AddItemToArray(a, e);
        }
    }
    if (in.proto >= 3) {
        cJSON *z = cJSON_AddObjectToObject(o, "zigbee");
        cJSON_AddStringToObject(z, "state", in.zb < 4 ? ZB[in.zb] : "?");
        cJSON_AddNumberToObject(z, "channel", in.zb_channel);
        cJSON_AddNumberToObject(z, "heap_before", in.zb_heap_before);
        cJSON_AddNumberToObject(z, "heap_after", in.zb_heap_after);
    }
    aos_154_chan_t ch[16];
    aos_154_channels(ch);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    cJSON *a = cJSON_AddArrayToObject(o, "channels");
    for (int i = 0; i < 16; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddNumberToObject(c, "ch", AOS_154_CH_FIRST + i);
        if (ch[i].when_ms) {
            cJSON_AddNumberToObject(c, "ed_max", ch[i].ed_max);
            cJSON_AddNumberToObject(c, "ed_avg", ch[i].ed_avg);
            cJSON_AddNumberToObject(c, "frames", ch[i].frames);
            cJSON_AddNumberToObject(c, "bytes", ch[i].bytes);
            cJSON_AddNumberToObject(c, "dwell", ch[i].dwell_ms);
            cJSON_AddNumberToObject(c, "age_ms", now - ch[i].when_ms);
        }
        cJSON_AddItemToArray(a, c);
    }
    send_cjson(r, ok ? 200 : 400, o);
}

static void api_frames(aos_httpd_req_t *r)
{
    long from = aos_httpd_query_int(r, "from", 1);
    aos_154_frame_t *f = malloc(sizeof *f * FRAMES_MAX);
    if (!f) { aos_httpd_send_json(r, 503, "{\"ok\":false,\"error\":\"no memory\"}"); return; }
    int n = aos_154_frames(from < 1 ? 1 : (uint32_t)from, f, FRAMES_MAX);
    aos_154_info_t in;
    aos_154_info(&in);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "last", in.last_seq);
    cJSON *a = cJSON_AddArrayToObject(o, "frames");
    char hex[2 * 127 + 1];
    for (int i = 0; i < n; i++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "seq", f[i].seq);
        cJSON_AddNumberToObject(x, "t", (double)f[i].t_us);
        cJSON_AddNumberToObject(x, "rssi", f[i].rssi);
        cJSON_AddNumberToObject(x, "lqi", f[i].lqi);
        cJSON_AddNumberToObject(x, "ch", f[i].channel);
        for (int k = 0; k < f[i].len; k++) snprintf(hex + 2 * k, 3, "%02x", f[i].data[k]);
        hex[2 * f[i].len] = '\0';
        cJSON_AddStringToObject(x, "hex", hex);
        cJSON_AddItemToArray(a, x);
    }
    free(f);
    send_cjson(r, 200, o);
}

static void api_devices(aos_httpd_req_t *r)
{
    aos_154_device_t *d = malloc(sizeof *d * AOS_154_DEVICES);
    if (!d) { aos_httpd_send_json(r, 503, "{\"ok\":false,\"error\":\"no memory\"}"); return; }
    int n = aos_154_devices(d, AOS_154_DEVICES);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    cJSON *o = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(o, "devices");
    for (int i = 0; i < n; i++) {
        cJSON *x = cJSON_CreateObject();
        char t[24];
        if (d[i].pan != 0xffff) { snprintf(t, sizeof t, "0x%04X", d[i].pan); cJSON_AddStringToObject(x, "pan", t); }
        if (d[i].short_addr != 0xffff) { snprintf(t, sizeof t, "0x%04X", d[i].short_addr); cJSON_AddStringToObject(x, "short", t); }
        if (d[i].has_ext) {
            const uint8_t *e = d[i].ext;
            snprintf(t, sizeof t, "%02X%02X%02X%02X%02X%02X%02X%02X", e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7]);
            cJSON_AddStringToObject(x, "ext", t);
        }
        if (d[i].vendor[0]) cJSON_AddStringToObject(x, "vendor", d[i].vendor);
        cJSON_AddNumberToObject(x, "ch", d[i].channel);
        cJSON_AddNumberToObject(x, "frames", d[i].frames);
        cJSON_AddNumberToObject(x, "rssi", d[i].rssi_last);
        cJSON_AddNumberToObject(x, "rssi_avg", d[i].rssi_avg);
        cJSON_AddNumberToObject(x, "ago_ms", now - d[i].last_ms);
        cJSON_AddItemToArray(a, x);
    }
    free(d);
    send_cjson(r, 200, o);
}

static void api_slots(aos_httpd_req_t *r, bool grow)
{
    static const char *LAYOUT[] = { "unknown", "factory", "grown", "other" };
    bool ok = aos_154_slots_ask(grow);
    aos_154_slots_info_t s;
    aos_154_slots(&s);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ok);
    cJSON_AddBoolToObject(o, "known", s.known);
    if (s.known) {
        cJSON_AddStringToObject(o, "table", s.layout < 4 ? LAYOUT[s.layout] : "?");
        cJSON_AddNumberToObject(o, "slot_size", s.slot_size);
        cJSON_AddStringToObject(o, "running", s.running_addr == 0x10000 ? "ota_0" : "ota_1");
        cJSON_AddNumberToObject(o, "grow", s.grow);
        cJSON_AddNumberToObject(o, "seq", s.seq);
    }
    send_cjson(r, ok ? 200 : 400, o);
}

static long num(cJSON *b, const char *k, long def)
{
    cJSON *v = cJSON_GetObjectItem(b, k);
    return cJSON_IsNumber(v) ? (long)v->valuedouble : def;
}

bool aos_portal_154(aos_httpd_req_t *r, const char *m, const char *p)
{
    if (strncmp(p, "154", 3) || (p[3] && p[3] != '/')) return false;
    const char *what = p[3] ? p + 4 : "";
    bool get = !strcmp(m, "GET"), post = !strcmp(m, "POST");
    if (get && !what[0]) { api_state(r, true, NULL); return true; }
    if (get && !strcmp(what, "frames")) { api_frames(r); return true; }
    if (get && !strcmp(what, "devices")) { api_devices(r); return true; }
    if (get && !strcmp(what, "slots")) { api_slots(r, false); return true; }
    if (get && !strcmp(what, "rcp/probe")) {
        char v[200];
        bool ok = aos_154_rcp_probe(v, sizeof v, 1500);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", ok);
        if (ok) cJSON_AddStringToObject(o, "version", v);
        send_cjson(r, ok ? 200 : 504, o);
        return true;
    }
    if (post && !strcmp(what, "slots/grow")) { api_slots(r, true); return true; }
    if (post && !strcmp(what, "devices/clear")) { aos_154_devices_clear(); api_state(r, true, NULL); return true; }
    if (!post) return false;
    char *raw = aos_httpd_body_all(r, 512);
    cJSON *b = raw ? cJSON_Parse(raw) : NULL;
    free(raw);
    if (!b) b = cJSON_CreateObject();
    bool ok;
    if (!strcmp(what, "hello")) ok = aos_154_hello();
    else if (!strcmp(what, "scan")) ok = aos_154_scan((int)num(b, "first", 11), (int)num(b, "last", 26), (int)num(b, "dwell", 200));
    else if (!strcmp(what, "sniff")) ok = aos_154_sniff((int)num(b, "channel", 0), cJSON_IsTrue(cJSON_GetObjectItem(b, "pcap")));
    else if (!strcmp(what, "stop")) ok = aos_154_stop();
    else if (!strcmp(what, "unguard")) ok = aos_154_unguard();
    else if (!strcmp(what, "rcp")) ok = aos_154_rcp_start();
    else if (!strcmp(what, "zbhost")) ok = aos_zigbee_start((int)num(b, "channel", 26));
    else if (!strcmp(what, "zbpermit")) ok = aos_zigbee_permit((int)num(b, "seconds", 180));
    else if (!strcmp(what, "c6log")) ok = aos_154_c6_log(cJSON_IsTrue(cJSON_GetObjectItem(b, "on")));
    else if (!strcmp(what, "zigbee"))
        ok = cJSON_IsTrue(cJSON_GetObjectItem(b, "on")) ? aos_154_zigbee_start((int)num(b, "channel", 26)) : aos_154_zigbee_stop();
    else { cJSON_Delete(b); return false; }
    cJSON_Delete(b);
    api_state(r, ok, ok ? NULL : "the C6 did not take it");
    return true;
}
