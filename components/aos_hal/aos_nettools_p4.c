/*
 * P4OS HAL - what the Red app (aos.net) needs from the radio and from mDNS
 * that the rest of the HAL does not give: the whole scan record (channel,
 * BSSID, security, width), the associated AP, and mDNS browsing.
 *
 * The contract is the "aos.net" block of aos_hal.h; the simulator's side is
 * sim/nettools_sim.c. The sockets part of the app (ping, the host and port
 * sweeps) needs nothing from here: lwIP gives the board the same POSIX
 * sockets macOS gives the simulator, so it lives in the app's own service,
 * components/aos_apps/aos_nettools.c.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "mdns.h"
#include "lwip/def.h"

extern bool aos_net_p4_up(void);        /* aos_net_p4.c: the C6 answered */

static uint8_t auth_of(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return AOS_WIFI_AUTH_OPEN;
    case WIFI_AUTH_OWE:             return AOS_WIFI_AUTH_OPEN;   /* open, encrypted */
    case WIFI_AUTH_WEP:             return AOS_WIFI_AUTH_WEP;
    case WIFI_AUTH_WPA_PSK:         return AOS_WIFI_AUTH_WPA;
    case WIFI_AUTH_WPA2_PSK:        return AOS_WIFI_AUTH_WPA2;
    case WIFI_AUTH_WPA_WPA2_PSK:    return AOS_WIFI_AUTH_WPA_WPA2;
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE: return AOS_WIFI_AUTH_WPA3;
    case WIFI_AUTH_WPA2_WPA3_PSK:   return AOS_WIFI_AUTH_WPA2_WPA3;
    case WIFI_AUTH_ENTERPRISE:
    case WIFI_AUTH_WPA3_ENT_192:
    case WIFI_AUTH_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA2_WPA3_ENTERPRISE:
    case WIFI_AUTH_WPA_ENTERPRISE:  return AOS_WIFI_AUTH_ENTERPRISE;
    default:                        return AOS_WIFI_AUTH_OTHER;
    }
}

static void fill(aos_wifi_ap_ex_t *o, const wifi_ap_record_t *r)
{
    memset(o, 0, sizeof *o);
    snprintf(o->ssid, sizeof o->ssid, "%s", (const char *)r->ssid);
    memcpy(o->bssid, r->bssid, 6);
    o->rssi = r->rssi;
    o->channel = r->primary;
    o->second = r->second == WIFI_SECOND_CHAN_ABOVE ? 1 : r->second == WIFI_SECOND_CHAN_BELOW ? -1 : 0;
    /* The width field arrived in IDF 5.x and whether esp_hosted carries it
     * over from the C6 is something only the board can say: the secondary
     * channel, which older drivers already reported, is the fallback. */
    switch (r->bandwidth) {
    case WIFI_BW40:      o->width = 40; break;
    case WIFI_BW80:      o->width = 80; break;
    case WIFI_BW160:
    case WIFI_BW80_BW80: o->width = 160; break;
    default:             o->width = o->second ? 40 : 20; break;
    }
    o->auth = auth_of(r->authmode);
    o->phy = (r->phy_11b ? 1 : 0) | (r->phy_11g ? 2 : 0) | (r->phy_11n ? 4 : 0) |
             (r->phy_11a ? 8 : 0) | (r->phy_11ac ? 16 : 0) | (r->phy_11ax ? 32 : 0);
}

static int ex_cmp(const void *a, const void *b)
{
    return ((const aos_wifi_ap_ex_t *)b)->rssi - ((const aos_wifi_ap_ex_t *)a)->rssi;
}

int aos_hal_net_scan_ex(aos_wifi_ap_ex_t *out, int max)
{
    if (!aos_net_p4_up() || !out || max <= 0) return -1;
    wifi_scan_config_t sc = { .show_hidden = true };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) return -1;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (!n) return 0;
    wifi_ap_record_t *rec = calloc(n, sizeof *rec);
    if (!rec) { esp_wifi_clear_ap_list(); return -1; }
    esp_wifi_scan_get_ap_records(&n, rec);
    int k = 0;
    for (int i = 0; i < n && k < max; i++) fill(&out[k++], &rec[i]);
    free(rec);
    qsort(out, (size_t)k, sizeof *out, ex_cmp);
    return k;
}

bool aos_hal_net_ap_info(aos_wifi_ap_ex_t *out)
{
    if (!aos_net_p4_up() || !out || aos_hal_net_state() != AOS_NET_CONNECTED) return false;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) return false;
    fill(out, &ap);
    return true;
}

/* ---- mDNS ---- */

#define MDNS_MAX_TYPES   16
#define MDNS_PER_TYPE    16

static void txt_join(char *dst, size_t n, const mdns_result_t *r)
{
    size_t k = 0;
    dst[0] = 0;
    for (size_t i = 0; i < r->txt_count && k + 2 < n; i++) {
        const char *key = r->txt[i].key ? r->txt[i].key : "";
        const char *val = r->txt[i].value ? r->txt[i].value : "";
        int w = snprintf(dst + k, n - k, "%s%s%s%s", k ? ";" : "", key, val[0] ? "=" : "", val);
        if (w < 0) break;
        k += (size_t)w;
        if (k >= n) { dst[n - 1] = 0; break; }
    }
}

int aos_hal_mdns_browse(const char *const *types, int ntypes, uint32_t timeout_ms,
                        aos_mdns_svc_t *out, int max)
{
    if (!types || ntypes <= 0 || !out || max <= 0) return -1;
    if (ntypes > MDNS_MAX_TYPES) ntypes = MDNS_MAX_TYPES;
    mdns_search_once_t *s[MDNS_MAX_TYPES] = { 0 };
    int started = 0;
    /* All the types in the air at once: one after the other would be a
     * timeout per type, and the answers come within the same second. */
    for (int i = 0; i < ntypes; i++) {
        char svc[32], proto[8];
        const char *dot = strstr(types[i], "._");
        if (!dot) continue;
        snprintf(svc, sizeof svc, "%.*s", (int)(dot - types[i]), types[i]);
        snprintf(proto, sizeof proto, "%s", dot + 1);
        s[i] = mdns_query_async_new(NULL, svc, proto, MDNS_TYPE_PTR, timeout_ms, MDNS_PER_TYPE, NULL);
        if (s[i]) started++;
    }
    if (!started) return -1;            /* mdns_init() never ran: no network yet */
    int k = 0;
    for (int i = 0; i < ntypes; i++) {
        if (!s[i]) continue;
        mdns_result_t *res = NULL;
        uint8_t num = 0;
        /* Waiting on the first one lets the others run their course too. */
        mdns_query_async_get_results(s[i], timeout_ms + 200, &res, &num);
        for (mdns_result_t *r = res; r && k < max; r = r->next) {
            if (!r->instance_name) continue;
            bool dup = false;
            for (int j = 0; j < k && !dup; j++)
                dup = !strcmp(out[j].type, types[i]) && !strcmp(out[j].instance, r->instance_name);
            if (dup) continue;
            aos_mdns_svc_t *o = &out[k++];
            memset(o, 0, sizeof *o);
            snprintf(o->type, sizeof o->type, "%s", types[i]);
            snprintf(o->instance, sizeof o->instance, "%s", r->instance_name);
            if (r->hostname) snprintf(o->host, sizeof o->host, "%s", r->hostname);
            o->port = r->port;
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    o->ip = lwip_ntohl(a->addr.u_addr.ip4.addr);
                    break;
                }
            }
            txt_join(o->txt, sizeof o->txt, r);
        }
        if (res) mdns_query_results_free(res);
        mdns_query_async_delete(s[i]);
    }
    return k;
}
