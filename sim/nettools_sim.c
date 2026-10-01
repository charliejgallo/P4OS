/*
 * P4OS simulator - the "aos.net" block of aos_hal.h (the Red app).
 *
 * The desktop has no radio this program may drive, and browsing mDNS would
 * mean talking to the real LAN, so both are a made-up neighbourhood: the same
 * networks hal_sim.c's aos_hal_net_scan() invents, now with channels, BSSIDs
 * and security, plus two 5 GHz ones so the second graph can be seen; and a
 * house's worth of mDNS services. The RSSI of the "connected" AP wanders a
 * little so the live trace has something to draw.
 *
 * What is NOT faked is the app's sockets side (ping, hosts, ports): that runs
 * for real, against 127.0.0.1 or whatever P4_SIM_NET_SCAN says.
 */
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#define B(a, b, c, d, e, f) { 0x##a, 0x##b, 0x##c, 0x##d, 0x##e, 0x##f }

static const aos_wifi_ap_ex_t FAKE[] = {
    /* ssid                       bssid                       rssi ch  w  2nd auth                       phy */
    { "simulator",                B(3C,84,6A,12,9E,01), -54,  6, 20,  0, AOS_WIFI_AUTH_WPA2,      0x07 },
    { "simulator",                B(3C,84,6A,12,9E,05), -71, 11, 20,  0, AOS_WIFI_AUTH_WPA2,      0x07 },
    { "Taller",                   B(A0,B5,49,03,77,10), -61,  1, 40,  1, AOS_WIFI_AUTH_WPA2_WPA3, 0x27 },
    { "Vecino 2.4GHz",            B(02,6B,EF,20,11,C2), -70,  6, 20,  0, AOS_WIFI_AUTH_WPA_WPA2,  0x07 },
    { "Depto-3B",                 B(02,07,B6,7C,10,33), -78, 11, 20,  0, AOS_WIFI_AUTH_WPA2,      0x07 },
    { "Cafe libre",               B(00,1A,2B,3C,4D,5E), -83,  3, 20,  0, AOS_WIFI_AUTH_OPEN,      0x03 },
    { "ESP_4F21A0",               B(C8,2B,96,4F,21,A0), -88,  1, 20,  0, AOS_WIFI_AUTH_OPEN,      0x07 },
    { "",                         B(3C,84,6A,12,9E,07), -80,  9, 20,  0, AOS_WIFI_AUTH_WPA2,      0x07 },
    { "Vecino 5GHz",              B(02,6B,EF,20,11,C6), -74, 44, 80,  0, AOS_WIFI_AUTH_WPA2,      0x3C },
    { "Taller-5G",                B(A0,B5,49,03,77,14), -66,149, 80,  0, AOS_WIFI_AUTH_WPA3,      0x3C },
};
#define N_FAKE (int)(sizeof FAKE / sizeof FAKE[0])

static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static int s_rssi = -54;

/* A small random walk around -54 dBm, as a hand on a real board would make. */
static int wander(void)
{
    pthread_mutex_lock(&s_mx);
    int step = (rand() % 5) - 2;
    if (s_rssi < -60) step = 1;
    if (s_rssi > -48) step = -1;
    s_rssi += step;
    int v = s_rssi;
    pthread_mutex_unlock(&s_mx);
    return v;
}

int aos_hal_net_scan_ex(aos_wifi_ap_ex_t *out, int max)
{
    if (!out || max <= 0) return -1;
    usleep(1500 * 1000);                    /* a real scan takes that long */
    int n = 0;
    for (; n < max && n < N_FAKE; n++) {
        out[n] = FAKE[n];
        out[n].rssi = (int8_t)(FAKE[n].rssi + (rand() % 5) - 2);
    }
    for (int i = 1; i < n; i++)             /* strongest first, like the board */
        for (int j = i; j > 0 && out[j].rssi > out[j - 1].rssi; j--) {
            aos_wifi_ap_ex_t t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

bool aos_hal_net_ap_info(aos_wifi_ap_ex_t *out)
{
    if (!out) return false;
    *out = FAKE[0];
    out->rssi = (int8_t)wander();
    return true;
}

#define IP4(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static const aos_mdns_svc_t SVC[] = {
    { "_home-assistant._tcp", "Casa",                 "homeassistant",   IP4(192, 168, 1, 10), 8123, "version=2026.9.2;base_url=http://192.168.1.10:8123" },
    { "_http._tcp",           "Home Assistant",       "homeassistant",   IP4(192, 168, 1, 10), 8123, "" },
    { "_mqtt._tcp",           "Mosquitto",            "homeassistant",   IP4(192, 168, 1, 10), 1883, "" },
    { "_esphomelib._tcp",     "gateway-modbus",       "gateway-modbus",  IP4(192, 168, 1, 31), 6053, "version=2026.8.3;platform=ESP32;board=esp32dev" },
    { "_esphomelib._tcp",     "luces-patio",          "luces-patio",     IP4(192, 168, 1, 32), 6053, "version=2026.8.3;platform=ESP32C3" },
    { "_http._tcp",           "gateway-modbus",       "gateway-modbus",  IP4(192, 168, 1, 31),   80, "" },
    { "_http._tcp",           "reloj",                "reloj",           IP4(192, 168, 1, 41),   80, "path=/" },
    { "_hap._tcp",            "Termostato 4F2A",      "termostato",      IP4(192, 168, 1, 44), 5001, "md=Termostato;ci=9" },
    { "_googlecast._tcp",     "Living",               "chromecast-living", IP4(192, 168, 1, 50), 8009, "md=Chromecast;fn=Living" },
    { "_ipp._tcp",            "Brother HL-L2350DW",   "brother",         IP4(192, 168, 1, 60),  631, "ty=Brother HL-L2350DW" },
    { "_smb._tcp",            "NAS",                  "nas",             IP4(192, 168, 1, 5),   445, "" },
    { "_ssh._tcp",            "NAS",                  "nas",             IP4(192, 168, 1, 5),    22, "" },
    { "_rtsp._tcp",           "Timbre",               "timbre",          IP4(192, 168, 1, 64),  554, "" },
};
#define N_SVC (int)(sizeof SVC / sizeof SVC[0])

int aos_hal_mdns_browse(const char *const *types, int ntypes, uint32_t timeout_ms,
                        aos_mdns_svc_t *out, int max)
{
    if (!types || ntypes <= 0 || !out || max <= 0) return -1;
    usleep((timeout_ms < 1500 ? timeout_ms : 1500) * 1000);
    int k = 0;
    for (int t = 0; t < ntypes; t++)
        for (int i = 0; i < N_SVC && k < max; i++)
            if (!strcmp(SVC[i].type, types[t])) out[k++] = SVC[i];
    return k;
}
