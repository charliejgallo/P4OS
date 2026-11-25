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
 *                                  that long (0 closes it, at most 254);
 *                                  "board": true, through the board alone;
 *                                  "strict": true, the link key exchange
 *                                  required of whoever joins
 *   POST /api/154/zbremove         {"ieee": "a4c13848f30143a5"}: asks that
 *                                  device to leave and forgets it
 *   POST /api/154/zbonoff          {"ieee": "...", "ep": 1, "on": true}:
 *                                  switches an output ("on" left out: toggles)
 *   POST /api/154/zblevel          {"ieee": "...", "ep": 1, "level": 127}:
 *                                  a dimmer's level, 0-254 (0: off)
 *   POST /api/154/zbcmd            {"ieee", "ep", "cluster", "cmd", "data": "hex",
 *                                  "read": attr}: any cluster-specific command,
 *                                  and that attribute read back
 *   POST /api/154/zbwrite          {"ieee", "ep", "cluster", "attr", "type",
 *                                  "value": "hex" (little-endian, as on the air)}
 *   POST /api/154/zbdp             {"ieee", "dp", "type": 1 bool / 2 value / 4 enum,
 *                                  "value"}: a Tuya datapoint
 *   POST /api/154/zbemu            the board emulating a device on another
 *                                  network (docs/plan/ZIGBEE-EMULAR.md):
 *                                  {"join": true[, "restart": true]}, {"set":
 *                                  {"temp", "hum", "battery", "on"}},
 *                                  {"report": true}, {"leave": true},
 *                                  {"home": true}; its state and log under
 *                                  "zbemu" in GET /api/154
 *   POST /api/154/zbname           {"ieee", "name"}: the user's name for a
 *                                  device ("" takes it away)
 *   POST /api/154/zbir             {"ieee", "learn": true|false} or {"ieee",
 *                                  "send": "<base64>"}: the IR blaster; its
 *                                  state and last code under "zbhost.ir".
 *                                  Also {"ieee", "raw": [µs, mark first],
 *                                  "freq"}: any code; {"ieee", "save":
 *                                  "<name>"}: the last learned kept by name;
 *                                  {"ieee", "saved": i} sends one kept and
 *                                  {"ieee", "forget": i} drops it (the list
 *                                  in "zbhost.ir.codes")
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
        cJSON_AddBoolToObject(h, "emulating", z.emu);
        /* the board emulating a device on another network (aos_zigbee_emu.c) */
        {
            static const char *const ES[] = { "off", "searching", "joined", "left", "failed" };
            aos_zigbee_emu_t m;
            aos_zigbee_emu_info(&m);
            cJSON *em = cJSON_AddObjectToObject(o, "zbemu");
            cJSON_AddStringToObject(em, "state", m.state < 5 ? ES[m.state] : "?");
            cJSON_AddStringToObject(em, "kind", "light_temp");
            cJSON_AddBoolToObject(em, "saved", m.saved);
            cJSON_AddNumberToObject(em, "tries", m.tries);
            if (m.state == AOS_ZBE_JOINED) {
                cJSON_AddNumberToObject(em, "channel", m.channel);
                cJSON_AddNumberToObject(em, "pan", m.pan);
                cJSON_AddNumberToObject(em, "nwk", m.nwk);
                cJSON_AddBoolToObject(em, "tclk", m.tclk);
                cJSON_AddNumberToObject(em, "identify_s", m.identify_s);
            }
            cJSON_AddNumberToObject(em, "temp", m.v[AOS_ZBE_V_TEMP] / 100.0);
            cJSON_AddNumberToObject(em, "hum", m.v[AOS_ZBE_V_HUM] / 100.0);
            cJSON_AddNumberToObject(em, "battery", m.v[AOS_ZBE_V_BATTERY]);
            cJSON_AddBoolToObject(em, "on", m.v[AOS_ZBE_V_ON] != 0);
            static aos_zigbee_emu_log_t lg[160];
            uint32_t next;
            int n = aos_zigbee_emu_log(lg, 0, 160, &next);
            cJSON *la = cJSON_AddArrayToObject(em, "log");
            uint32_t now_ms = (uint32_t)aos_hal_uptime_ms();
            for (int k = n > 60 ? n - 60 : 0; k < n; k++) {
                cJSON *x = cJSON_CreateObject();
                cJSON_AddNumberToObject(x, "ago_s", (double)((now_ms - lg[k].ms) / 1000));
                cJSON_AddBoolToObject(x, "board", lg[k].out);
                cJSON_AddStringToObject(x, "text", lg[k].text);
                cJSON_AddItemToArray(la, x);
            }
        }
        static aos_zigbee_dev_t dev[AOS_ZB_DEVICES_MAX];     /* the portal's one task */
        static const char *STEP[] = { "endpoints", "basic", "done", "failed", "left", "removed" };
        int n = aos_zigbee_devices(dev, AOS_ZB_DEVICES_MAX);
        {
            static const char *const IRS[] = { "idle", "learning", "receiving", "learned", "sending", "sent", "failed" };
            static char code[3000];
            uint8_t ie[8];
            int st = aos_zigbee_ir_state(ie, code, sizeof code);
            cJSON *ir = cJSON_AddObjectToObject(h, "ir");
            cJSON_AddStringToObject(ir, "state", st >= 0 && st < 7 ? IRS[st] : "?");
            char ieee[17];
            snprintf(ieee, sizeof ieee, "%02x%02x%02x%02x%02x%02x%02x%02x", ie[7], ie[6], ie[5], ie[4], ie[3], ie[2], ie[1], ie[0]);
            cJSON_AddStringToObject(ir, "ieee", ieee);
            cJSON_AddStringToObject(ir, "learned", code);
            static aos_zigbee_ir_code_t sv[96];
            int ns = aos_zigbee_ir_codes(sv, 96);
            cJSON *ca = cJSON_AddArrayToObject(ir, "codes");
            for (int k = 0; k < ns; k++) {
                cJSON *c = cJSON_CreateObject();
                snprintf(ieee, sizeof ieee, "%02x%02x%02x%02x%02x%02x%02x%02x", sv[k].ieee[7], sv[k].ieee[6], sv[k].ieee[5],
                         sv[k].ieee[4], sv[k].ieee[3], sv[k].ieee[2], sv[k].ieee[1], sv[k].ieee[0]);
                cJSON_AddStringToObject(c, "ieee", ieee);
                cJSON_AddStringToObject(c, "name", sv[k].name);
                cJSON_AddItemToArray(ca, c);
            }
        }
        cJSON *a = cJSON_AddArrayToObject(h, "devices");
        for (int i = 0; i < n; i++) {
            const aos_zigbee_dev_t *d = &dev[i];
            char ieee[17];
            snprintf(ieee, sizeof ieee, "%02x%02x%02x%02x%02x%02x%02x%02x", d->ieee[7], d->ieee[6], d->ieee[5],
                     d->ieee[4], d->ieee[3], d->ieee[2], d->ieee[1], d->ieee[0]);
            cJSON *e = cJSON_CreateObject();
            cJSON_AddStringToObject(e, "ieee", ieee);
            cJSON_AddNumberToObject(e, "nwk", d->nwk);
            cJSON_AddStringToObject(e, "step", d->step < 6 ? STEP[d->step] : "?");
            cJSON_AddBoolToObject(e, "router", (d->cap & 0x02) != 0);
            cJSON_AddNumberToObject(e, "ep", d->ep);
            cJSON_AddNumberToObject(e, "eps", d->eps);
            cJSON_AddStringToObject(e, "manufacturer", d->manuf);
            cJSON_AddStringToObject(e, "model", d->model);
            cJSON_AddStringToObject(e, "name", d->name);
            char iv[24], im[40], idesc[64];
            if (aos_zigbee_ident(d, iv, sizeof iv, im, sizeof im, idesc, sizeof idesc)) {
                cJSON *id = cJSON_AddObjectToObject(e, "ident");
                cJSON_AddStringToObject(id, "vendor", iv);
                cJSON_AddStringToObject(id, "model", im);
                cJSON_AddStringToObject(id, "description", idesc);
            }
            cJSON_AddNumberToObject(e, "power", d->power);
            if (d->rssi) cJSON_AddNumberToObject(e, "rssi", d->rssi);
            uint32_t now = (uint32_t)aos_hal_uptime_ms();
            cJSON_AddNumberToObject(e, "ago_s", (double)((now - d->seen_ms) / 1000));
            if (d->heard_ms) cJSON_AddNumberToObject(e, "heard_s", (double)((now - d->heard_ms) / 1000));
            else cJSON_AddNullToObject(e, "heard_s");             /* no news since the start */
            cJSON_AddNumberToObject(e, "setup", d->setup);
            cJSON *eps = cJSON_AddArrayToObject(e, "endpoints");
            for (int k = 0; k < d->neps; k++) cJSON_AddItemToArray(eps, cJSON_CreateNumber(d->eps_list[k]));
            /* the clusters it serves, "ep:cluster" in hex */
            cJSON *cl = cJSON_AddArrayToObject(e, "clusters");
            for (int k = 0; k < d->ncl; k++) {
                char t[12];
                snprintf(t, sizeof t, "%u:%04x", (unsigned)(d->cl[k] >> 16), (unsigned)(d->cl[k] & 0xFFFF));
                cJSON_AddItemToArray(cl, cJSON_CreateString(t));
            }
            /* what the board names */
            if (d->level_ep) cJSON_AddNumberToObject(e, "level_ep", d->level_ep);   /* a dimmer, its level known or not */
            cJSON *v = cJSON_AddObjectToObject(e, "values");
            if (d->temp != INT16_MIN) cJSON_AddNumberToObject(v, "temperature", d->temp / 100.0);
            if (d->hum != 0xFFFF) cJSON_AddNumberToObject(v, "humidity", d->hum / 100.0);
            if (d->battery != 0xFF) cJSON_AddNumberToObject(v, "battery", d->battery / 2.0);
            if (d->voltage) cJSON_AddNumberToObject(v, "voltage", d->voltage / 10.0);
            if (d->occupancy != 0xFF) cJSON_AddBoolToObject(v, "occupancy", d->occupancy & 1);
            if (d->level_ep && d->level != 0xFF) {
                cJSON *l = cJSON_AddObjectToObject(v, "level");
                cJSON_AddNumberToObject(l, "ep", d->level_ep);
                cJSON_AddNumberToObject(l, "value", d->level);
            }
            if (d->onoff_known) {
                cJSON *o = cJSON_AddArrayToObject(v, "on");
                for (int k = 0; k < d->neps; k++)
                    cJSON_AddItemToArray(o, (d->onoff_known >> k) & 1 ? cJSON_CreateBool((d->onoff >> k) & 1) : cJSON_CreateNull());
            }
            if (d->zone_known || d->zone_type != 0xFFFF) {
                cJSON *z = cJSON_AddObjectToObject(v, "zone");
                if (d->zone_type != 0xFFFF) cJSON_AddNumberToObject(z, "type", d->zone_type);
                if (d->zone_known) {
                    cJSON_AddBoolToObject(z, "alarm", d->zone_status & 1);
                    cJSON_AddBoolToObject(z, "battery_low", (d->zone_status >> 3) & 1);
                    cJSON_AddNumberToObject(z, "status", d->zone_status);
                }
            }
            /* what the table names (aos_zigbee_fmt.c) */
            cJSON *ms = cJSON_AddArrayToObject(e, "measures");
            for (int k = 0; k < d->nmeas; k++) {
                const char *nm;
                char val[48];
                if (!aos_zigbee_meas_text(d, &d->meas[k], &nm, val, sizeof val)) continue;
                cJSON *x = cJSON_CreateObject();
                cJSON_AddStringToObject(x, "name", nm);
                cJSON_AddStringToObject(x, "value", val);
                cJSON_AddNumberToObject(x, "ep", d->meas[k].ep);
                cJSON_AddNumberToObject(x, "cluster", d->meas[k].cluster);
                cJSON_AddNumberToObject(x, "attr", d->meas[k].attr);
                cJSON_AddNumberToObject(x, "raw", d->meas[k].v);
                cJSON_AddItemToArray(ms, x);
            }
            static const char *const CTL[AOS_ZB_CTL_N] = { "cover", "thermostat", "color", "lock", "fan" };
            cJSON *ct = cJSON_AddObjectToObject(e, "controls");
            for (int k = 0; k < AOS_ZB_CTL_N; k++)
                if (d->ctl_ep[k]) cJSON_AddNumberToObject(ct, CTL[k], d->ctl_ep[k]);
            if (d->clusters & AOS_ZB_CL_IR) cJSON_AddBoolToObject(ct, "ir", true);
            if (d->clusters & AOS_ZB_CL_TUYA) cJSON_AddBoolToObject(ct, "tuya", true);
            if (d->event[0]) {
                cJSON *ev = cJSON_AddObjectToObject(e, "event");
                cJSON_AddStringToObject(ev, "text", d->event);
                cJSON_AddNumberToObject(ev, "ago_s", (double)((now - d->event_ms) / 1000));
            }
            if (d->ndp) {
                cJSON *dps = cJSON_AddArrayToObject(e, "datapoints");
                for (int k = 0; k < d->ndp; k++) {
                    const aos_zigbee_dp_t *q = &d->dps[k];
                    cJSON *x = cJSON_CreateObject();
                    cJSON_AddNumberToObject(x, "dp", q->dp);
                    char dn[40], dv[64];
                    bool flip;
                    if (aos_zigbee_dp_text(d, q, dn, sizeof dn, dv, sizeof dv, &flip)) {
                        cJSON_AddStringToObject(x, "name", dn);
                        cJSON_AddStringToObject(x, "text", dv);
                    }
                    cJSON_AddNumberToObject(x, "type", q->type);
                    if (q->type == 0 || q->type == 3) {
                        char hex[2 * sizeof q->raw + 1];
                        int l = q->len < sizeof q->raw ? q->len : (int)sizeof q->raw;
                        for (int m = 0; m < l; m++) snprintf(hex + 2 * m, 3, "%02x", q->raw[m]);
                        hex[2 * l] = '\0';
                        cJSON_AddStringToObject(x, "hex", hex);
                        cJSON_AddNumberToObject(x, "len", q->len);
                    } else cJSON_AddNumberToObject(x, "value", q->v);
                    cJSON_AddNumberToObject(x, "ago_s", (double)((now - q->ms) / 1000));
                    cJSON_AddItemToArray(dps, x);
                }
            }
            /* and everything it said, raw */
            cJSON *at = cJSON_AddArrayToObject(e, "attributes");
            for (int k = 0; k < d->nattr; k++) {
                const aos_zigbee_attr_t *r = &d->attr[k];
                char hex[2 * sizeof r->v + 1];
                for (int m = 0; m < r->len; m++) snprintf(hex + 2 * m, 3, "%02x", r->v[m]);
                hex[2 * r->len] = '\0';
                cJSON *x = cJSON_CreateObject();
                cJSON_AddNumberToObject(x, "ep", r->ep);
                cJSON_AddNumberToObject(x, "cluster", r->cluster);
                if (r->type == 0xFF) cJSON_AddNumberToObject(x, "command", r->attr);
                else {
                    cJSON_AddNumberToObject(x, "attr", r->attr);
                    cJSON_AddNumberToObject(x, "type", r->type);
                }
                cJSON_AddStringToObject(x, "hex", hex);
                cJSON_AddNumberToObject(x, "ago_s", (double)((now - r->ms) / 1000));
                cJSON_AddItemToArray(at, x);
            }
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
    char *raw = aos_httpd_body_all(r, 4096);     /* an IR code can be a few KB */
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
    else if (!strcmp(what, "zbpermit")) {
        aos_zigbee_static_kn(cJSON_IsTrue(cJSON_GetObjectItem(b, "static")));
        ok = cJSON_IsTrue(cJSON_GetObjectItem(b, "strict"))
                 ? aos_zigbee_permit_strict((int)num(b, "seconds", 180), cJSON_IsTrue(cJSON_GetObjectItem(b, "board")))
             : cJSON_IsTrue(cJSON_GetObjectItem(b, "board")) ? aos_zigbee_permit_board((int)num(b, "seconds", 180))
                                                              : aos_zigbee_permit((int)num(b, "seconds", 180));
    }
    else if (!strcmp(what, "zbemu")) {
        /* {"join": true, "restart": true} | {"set": {"temp", "hum", "battery", "on"}} | {"report": true} |
         * {"leave": true} | {"home": true} */
        const cJSON *st = cJSON_GetObjectItem(b, "set");
        if (cJSON_IsTrue(cJSON_GetObjectItem(b, "join"))) {
            bool restart = false;
            ok = aos_zigbee_emu_join(AOS_ZBE_LIGHT_TEMP, cJSON_IsTrue(cJSON_GetObjectItem(b, "restart")), &restart);
            if (!ok && restart) {
                cJSON_Delete(b);
                api_state(r, false, "the board's own network is running: ask again with \"restart\": true");
                return true;
            }
        } else if (cJSON_IsObject(st)) {
            const cJSON *x;
            ok = true;
            if (cJSON_IsNumber(x = cJSON_GetObjectItem(st, "temp"))) ok &= aos_zigbee_emu_set(AOS_ZBE_V_TEMP, (int32_t)(x->valuedouble * 100));
            if (cJSON_IsNumber(x = cJSON_GetObjectItem(st, "hum"))) ok &= aos_zigbee_emu_set(AOS_ZBE_V_HUM, (int32_t)(x->valuedouble * 100));
            if (cJSON_IsNumber(x = cJSON_GetObjectItem(st, "battery"))) ok &= aos_zigbee_emu_set(AOS_ZBE_V_BATTERY, x->valueint);
            if (cJSON_IsBool(x = cJSON_GetObjectItem(st, "on"))) ok &= aos_zigbee_emu_set(AOS_ZBE_V_ON, cJSON_IsTrue(x));
        } else if (cJSON_IsTrue(cJSON_GetObjectItem(b, "report"))) ok = aos_zigbee_emu_report_now();
        else if (cJSON_IsTrue(cJSON_GetObjectItem(b, "leave"))) ok = aos_zigbee_emu_leave();
        else if (cJSON_IsTrue(cJSON_GetObjectItem(b, "home"))) ok = aos_zigbee_emu_home();
        else ok = false;
    }
    else if (!strcmp(what, "zbname") || !strcmp(what, "zbremove") || !strcmp(what, "zbonoff") || !strcmp(what, "zblevel") || !strcmp(what, "zbcmd") ||
             !strcmp(what, "zbwrite") || !strcmp(what, "zbdp") || !strcmp(what, "zbir")) {
        const cJSON *ie = cJSON_GetObjectItem(b, "ieee");
        uint8_t ieee[8];
        ok = cJSON_IsString(ie) && strlen(ie->valuestring) == 16;
        for (int k = 0; ok && k < 8; k++) {
            unsigned v;
            ok = sscanf(ie->valuestring + 2 * k, "%2x", &v) == 1;
            ieee[7 - k] = (uint8_t)v;
        }
        /* "data" / "value": hex bytes, as they go on the air */
        uint8_t hx[16];
        int hn = 0;
        const cJSON *hs = cJSON_GetObjectItem(b, !strcmp(what, "zbcmd") ? "data" : "value");
        if (cJSON_IsString(hs))
            for (const char *c = hs->valuestring; c[0] && c[1] && hn < (int)sizeof hx; c += 2) {
                unsigned v;
                if (sscanf(c, "%2x", &v) != 1) break;
                hx[hn++] = (uint8_t)v;
            }
        if (ok && !strcmp(what, "zbname")) {
            const cJSON *nm = cJSON_GetObjectItem(b, "name");
            ok = aos_zigbee_set_name(ieee, cJSON_IsString(nm) ? nm->valuestring : "");
        }
        else if (ok && !strcmp(what, "zbremove")) ok = aos_zigbee_remove(ieee);
        else if (ok && !strcmp(what, "zblevel")) ok = aos_zigbee_level(ieee, (uint8_t)num(b, "ep", 1), (int)num(b, "level", 254));
        else if (ok && !strcmp(what, "zbcmd"))
            ok = aos_zigbee_command(ieee, (uint8_t)num(b, "ep", 1), (uint16_t)num(b, "cluster", 0), (uint8_t)num(b, "cmd", 0),
                                    hx, hn, (uint16_t)num(b, "read", 0xFFFF));
        else if (ok && !strcmp(what, "zbwrite"))
            ok = aos_zigbee_write(ieee, (uint8_t)num(b, "ep", 1), (uint16_t)num(b, "cluster", 0), (uint16_t)num(b, "attr", 0),
                                  (uint8_t)num(b, "type", 0x20), hx, hn);
        else if (ok && !strcmp(what, "zbdp"))
            ok = aos_zigbee_tuya_dp(ieee, (uint8_t)num(b, "dp", 1), (uint8_t)num(b, "type", 1), (int32_t)num(b, "value", 0));
        else if (ok && !strcmp(what, "zbir")) {
            const cJSON *code = cJSON_GetObjectItem(b, "send"), *learn = cJSON_GetObjectItem(b, "learn");
            const cJSON *raw = cJSON_GetObjectItem(b, "raw"), *save = cJSON_GetObjectItem(b, "save");
            const cJSON *saved = cJSON_GetObjectItem(b, "saved"), *forget = cJSON_GetObjectItem(b, "forget");
            if (cJSON_IsString(code)) ok = aos_zigbee_ir_send(ieee, code->valuestring);
            else if (cJSON_IsArray(raw)) {
                static uint32_t d[1024];
                int n = 0;
                const cJSON *x;
                cJSON_ArrayForEach(x, raw) if (cJSON_IsNumber(x) && n < 1024) d[n++] = x->valuedouble > 0 ? (uint32_t)x->valuedouble : 0;
                ok = n > 0 && aos_zigbee_ir_send_raw(ieee, d, n, (uint32_t)num(b, "freq", 38000));
            }
            else if (cJSON_IsString(save)) ok = aos_zigbee_ir_save(ieee, save->valuestring);
            else if (cJSON_IsNumber(saved)) ok = aos_zigbee_ir_send_saved(saved->valueint);
            else if (cJSON_IsNumber(forget)) ok = aos_zigbee_ir_forget(forget->valueint);
            else ok = aos_zigbee_ir_learn(ieee, !cJSON_IsFalse(learn));
        }
        else if (ok) {
            const cJSON *on = cJSON_GetObjectItem(b, "on");
            ok = aos_zigbee_onoff(ieee, (uint8_t)num(b, "ep", 1), cJSON_IsBool(on) ? cJSON_IsTrue(on) : 2);
        }
    }
    else if (!strcmp(what, "c6log")) ok = aos_154_c6_log(cJSON_IsTrue(cJSON_GetObjectItem(b, "on")));
    else if (!strcmp(what, "zigbee"))
        ok = cJSON_IsTrue(cJSON_GetObjectItem(b, "on")) ? aos_154_zigbee_start((int)num(b, "channel", 26)) : aos_154_zigbee_stop();
    else { cJSON_Delete(b); return false; }
    cJSON_Delete(b);
    api_state(r, ok, ok ? NULL : "the C6 did not take it");
    return true;
}
