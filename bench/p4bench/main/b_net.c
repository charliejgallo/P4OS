/*
 * Wi-Fi through the ESP32-C6 (HARDWARE.md tests 9, 10).
 *
 * esp_wifi_remote turns the ordinary esp_wifi_* calls into RPCs to the C6
 * over SDIO (esp_hosted 1.4). Besides connecting and measuring, this file
 * gives the bench a small HTTP server once there is an IP:
 *
 *   GET /api/bench         every BENCH line so far
 *   GET /api/cmd?c=<cmd>   runs a console command and returns its BENCH lines
 *                          (so the Mac can drive the board without the cable)
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_console.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_http_server.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "mdns.h"
#include "ping/ping_sock.h"
#include "iperf_cmd.h"
#include "esp_hosted.h"
#include "bench.h"

static const char *TAG = "net";

static EventGroupHandle_t s_ev;
#define EV_GOT_IP BIT0
static esp_netif_t *s_sta;
static bool s_wifi_inited;
static uint32_t s_disconnects, s_reconnects;
static bool s_want_connected;
static httpd_handle_t s_httpd;
static esp_ip4_addr_t s_ip, s_gw;

static void start_http(void);

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, EV_GOT_IP);
        s_disconnects++;
        wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "disconnected, reason %d", d->reason);
        if (s_want_connected) { s_reconnects++; esp_wifi_connect(); }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *g = data;
        s_ip = g->ip_info.ip;
        s_gw = g->ip_info.gw;
        xEventGroupSetBits(s_ev, EV_GOT_IP);
        char st[96];
        snprintf(st, sizeof st, "Wi-Fi " IPSTR "  (p4bench.local)", IP2STR(&s_ip));
        lvport_ui_status(st);
        start_http();
    }
}

static esp_err_t wifi_init_once(void)
{
    if (s_wifi_inited) return ESP_OK;
    s_ev = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    s_sta = esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    int64_t t = bench_us();
    esp_err_t e = esp_wifi_init(&cfg);
    bench_report("wifi.init", "ok=%d err=%s ms=%lld", e == ESP_OK, esp_err_to_name(e), (bench_us() - t) / 1000);
    if (e != ESP_OK) return e;
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
    s_wifi_inited = true;

    esp_hosted_coprocessor_fwver_t v = { 0 };
    e = esp_hosted_get_coprocessor_fwversion(&v);
    bench_report("hosted.fw", "ok=%d major=%lu minor=%lu patch=%lu", e == ESP_OK,
                 (unsigned long)v.major1, (unsigned long)v.minor1, (unsigned long)v.patch1);
    return ESP_OK;
}

static void wifi_up(int timeout_s)
{
    char ssid[33] = { 0 }, pass[65] = { 0 };
    if (!bench_cfg_get_str("ssid", ssid, sizeof ssid)) { bench_say("no network saved: wifi set <ssid> <password>"); return; }
    bench_cfg_get_str("pass", pass, sizeof pass);
    if (wifi_init_once() != ESP_OK) return;
    wifi_config_t wc = { 0 };
    strlcpy((char *)wc.sta.ssid, ssid, sizeof wc.sta.ssid);
    strlcpy((char *)wc.sta.password, pass, sizeof wc.sta.password);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    s_want_connected = true;
    int64_t t = bench_us();
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_ev, EV_GOT_IP, false, true, pdMS_TO_TICKS(timeout_s * 1000));
    wifi_ap_record_t ap = { 0 };
    esp_wifi_sta_get_ap_info(&ap);
    bench_report("wifi.up", "ok=%d ms=%lld ip=" IPSTR " rssi=%d channel=%d ax=%d n=%d",
                 (b & EV_GOT_IP) != 0, (bench_us() - t) / 1000, IP2STR(&s_ip), ap.rssi, ap.primary,
                 ap.phy_11ax, ap.phy_11n);
    if (b & EV_GOT_IP) {
        mdns_init();
        mdns_hostname_set("p4bench");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }
}

/* ---- ping soak ---- */
static uint32_t s_ping_ok, s_ping_lost, s_ping_ms_sum, s_ping_ms_max;
static void ping_ok(esp_ping_handle_t h, void *a)
{
    uint32_t ms;
    esp_ping_get_profile(h, ESP_PING_PROF_TIMEGAP, &ms, sizeof ms);
    s_ping_ok++;
    s_ping_ms_sum += ms;
    if (ms > s_ping_ms_max) s_ping_ms_max = ms;
}
static void ping_lost(esp_ping_handle_t h, void *a) { s_ping_lost++; }

static void soak(int minutes)
{
    if (!(xEventGroupGetBits(s_ev) & EV_GOT_IP)) { bench_say("not connected"); return; }
    s_ping_ok = s_ping_lost = s_ping_ms_sum = s_ping_ms_max = 0;
    uint32_t d0 = s_disconnects;
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.target_addr.type = IPADDR_TYPE_V4;
    pc.target_addr.u_addr.ip4.addr = s_gw.addr;
    pc.count = minutes * 60;
    pc.interval_ms = 1000;
    esp_ping_callbacks_t cb = { .on_ping_success = ping_ok, .on_ping_timeout = ping_lost };
    esp_ping_handle_t ph;
    esp_ping_new_session(&pc, &cb, &ph);
    esp_ping_start(ph);
    bench_say("soak: pinging the gateway once a second for %d min", minutes);
    for (int m = 0; m < minutes; m++) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        wifi_ap_record_t ap = { 0 };
        esp_wifi_sta_get_ap_info(&ap);
        bench_report("wifi.soak", "minute=%d ok=%lu lost=%lu avg_ms=%lu max_ms=%lu disconnects=%lu rssi=%d",
                     m + 1, (unsigned long)s_ping_ok, (unsigned long)s_ping_lost,
                     (unsigned long)(s_ping_ok ? s_ping_ms_sum / s_ping_ok : 0), (unsigned long)s_ping_ms_max,
                     (unsigned long)(s_disconnects - d0), ap.rssi);
    }
    esp_ping_stop(ph);
    esp_ping_delete_session(ph);
}

static void tls_test(const char *url)
{
    esp_http_client_config_t c = { .url = url, .crt_bundle_attach = esp_crt_bundle_attach, .timeout_ms = 15000 };
    esp_http_client_handle_t h = esp_http_client_init(&c);
    int64_t t = bench_us();
    esp_err_t e = esp_http_client_open(h, 0);
    int64_t t_open = bench_us() - t;
    int status = 0, total = 0;
    if (e == ESP_OK) {
        esp_http_client_fetch_headers(h);
        status = esp_http_client_get_status_code(h);
        static char buf[2048];
        int n;
        while ((n = esp_http_client_read(h, buf, sizeof buf)) > 0) total += n;
    }
    double secs = (bench_us() - t) / 1e6;
    bench_report("tls", "url=%s ok=%d status=%d handshake_ms=%lld bytes=%d total_ms=%.0f kBps=%.0f",
                 url, e == ESP_OK, status, t_open / 1000, total, secs * 1000, total / secs / 1000);
    esp_http_client_cleanup(h);
}

/* ---- HTTP ---- */
static esp_err_t h_bench(httpd_req_t *r)
{
    char *buf = heap_caps_malloc(64 * 1024 + 1, MALLOC_CAP_SPIRAM);
    size_t n = bench_log_dump(buf, 64 * 1024 + 1);
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_send(r, buf, n);
    free(buf);
    return ESP_OK;
}

static esp_err_t h_cmd(httpd_req_t *r)
{
    char q[256] = { 0 }, cmd[200] = { 0 };
    if (httpd_req_get_url_query_str(r, q, sizeof q) != ESP_OK || httpd_query_key_value(q, "c", cmd, sizeof cmd) != ESP_OK) {
        httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "use /api/cmd?c=<command>");
        return ESP_OK;
    }
    /* decode + and %XX */
    char dec[200];
    int j = 0;
    for (int i = 0; cmd[i] && j < (int)sizeof dec - 1; i++) {
        if (cmd[i] == '+') dec[j++] = ' ';
        else if (cmd[i] == '%' && cmd[i + 1] && cmd[i + 2]) { char h[3] = { cmd[i + 1], cmd[i + 2], 0 }; dec[j++] = strtol(h, NULL, 16); i += 2; }
        else dec[j++] = cmd[i];
    }
    dec[j] = 0;
    char *before = heap_caps_malloc(64 * 1024 + 1, MALLOC_CAP_SPIRAM);
    size_t n0 = bench_log_dump(before, 64 * 1024 + 1);
    int ret = 0;
    esp_err_t e = esp_console_run(dec, &ret);
    char *after = heap_caps_malloc(64 * 1024 + 1, MALLOC_CAP_SPIRAM);
    size_t n1 = bench_log_dump(after, 64 * 1024 + 1);
    httpd_resp_set_type(r, "text/plain");
    /* new lines only (the ring is append-only until it wraps) */
    if (n1 >= n0 && !memcmp(before, after, n0)) httpd_resp_send(r, after + n0, n1 - n0);
    else httpd_resp_send(r, after, n1);
    if (e != ESP_OK) ESP_LOGW(TAG, "command '%s': %s", dec, esp_err_to_name(e));
    free(before);
    free(after);
    return ESP_OK;
}

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/plain");
    httpd_resp_sendstr(r, "p4bench\n  /api/bench      all results\n  /api/cmd?c=... run a console command\n");
    return ESP_OK;
}

static void start_http(void)
{
    if (s_httpd) return;
    httpd_config_t c = HTTPD_DEFAULT_CONFIG();
    c.stack_size = 12288;
    c.recv_wait_timeout = 30;
    c.send_wait_timeout = 30;
    if (httpd_start(&s_httpd, &c) != ESP_OK) return;
    httpd_uri_t u1 = { .uri = "/", .method = HTTP_GET, .handler = h_root };
    httpd_uri_t u2 = { .uri = "/api/bench", .method = HTTP_GET, .handler = h_bench };
    httpd_uri_t u3 = { .uri = "/api/cmd", .method = HTTP_GET, .handler = h_cmd };
    httpd_register_uri_handler(s_httpd, &u1);
    httpd_register_uri_handler(s_httpd, &u2);
    httpd_register_uri_handler(s_httpd, &u3);
}

static int cmd_wifi(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "status");
    if (!strcmp(sub, "set") && argc >= 3) {
        bench_cfg_set_str("ssid", argv[2]);
        bench_cfg_set_str("pass", arg_str(argc, argv, 3, ""));
        bench_cfg_set("autowifi", 1);
        bench_say("saved network '%s' (connects on boot from now on)", argv[2]);
        return 0;
    }
    if (!strcmp(sub, "up")) { wifi_up(arg_int(argc, argv, 2, 20)); return 0; }
    if (!strcmp(sub, "init")) { wifi_init_once(); return 0; }
    if (!strcmp(sub, "scan")) {
        if (wifi_init_once() != ESP_OK) return 0;
        int64_t t = bench_us();
        esp_wifi_scan_start(NULL, true);
        uint16_t n = 20;
        wifi_ap_record_t recs[20];
        esp_wifi_scan_get_ap_records(&n, recs);
        bench_report("wifi.scan", "found=%d ms=%lld", n, (bench_us() - t) / 1000);
        for (int i = 0; i < n; i++) printf("  %-32s ch %2d  %4d dBm  %s\n", recs[i].ssid, recs[i].primary, recs[i].rssi, recs[i].phy_11ax ? "ax" : "");
        return 0;
    }
    if (!strcmp(sub, "soak")) { soak(arg_int(argc, argv, 2, 60)); return 0; }
    if (!strcmp(sub, "status")) {
        wifi_ap_record_t ap = { 0 };
        esp_err_t e = s_wifi_inited ? esp_wifi_sta_get_ap_info(&ap) : ESP_ERR_INVALID_STATE;
        bench_report("wifi.status", "connected=%d ip=" IPSTR " rssi=%d channel=%d disconnects=%lu reconnects=%lu",
                     e == ESP_OK, IP2STR(&s_ip), ap.rssi, ap.primary, (unsigned long)s_disconnects, (unsigned long)s_reconnects);
        return 0;
    }
    printf("wifi set <ssid> [pass]|up [timeout]|init|scan|status|soak [min]\n");
    return 0;
}

static int cmd_ntp(int argc, char **argv)
{
    esp_sntp_config_t c = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&c);
    int64_t t = bench_us();
    esp_err_t e = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000));
    time_t now = time(NULL);
    bench_report("ntp", "ok=%d ms=%lld epoch=%lld", e == ESP_OK, (bench_us() - t) / 1000, (long long)now);
    esp_netif_sntp_deinit();
    return 0;
}

static int cmd_tls(int argc, char **argv)
{
    tls_test(arg_str(argc, argv, 1, "https://www.howsmyssl.com/a/check"));
    return 0;
}

static int cmd_hosted(int argc, char **argv)
{
    const char *sub = arg_str(argc, argv, 1, "fw");
    if (!strcmp(sub, "fw")) {
        if (wifi_init_once() != ESP_OK) return 0;
        esp_hosted_coprocessor_fwver_t v = { 0 };
        esp_err_t e = esp_hosted_get_coprocessor_fwversion(&v);
        bench_report("hosted.fw", "ok=%d major=%lu minor=%lu patch=%lu", e == ESP_OK,
                     (unsigned long)v.major1, (unsigned long)v.minor1, (unsigned long)v.patch1);
        return 0;
    }
    if (!strcmp(sub, "ota") && argc >= 4 && !strcmp(argv[3], "yes")) {
        /* plan B for the C6: flash a new esp_hosted slave image from a URL.
         * Only on purpose: `hosted ota http://<mac>:8000/network_adapter.bin yes` */
        bench_say("updating the C6 from %s ...", argv[2]);
        esp_err_t e = esp_hosted_slave_ota(argv[2]);
        bench_report("hosted.ota", "url=%s result=%s", argv[2], esp_err_to_name(e));
        return 0;
    }
    printf("hosted fw | hosted ota <url> yes\n");
    return 0;
}

static void autowifi_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    wifi_up(20);
    vTaskDelete(NULL);
}

void reg_net(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "wifi", .help = "Wi-Fi through the C6 (set up scan status soak)", .func = cmd_wifi },
        { .command = "ntp", .help = "SNTP sync time", .func = cmd_ntp },
        { .command = "tls", .help = "tls [https url]: handshake and download time", .func = cmd_tls },
        { .command = "hosted", .help = "hosted fw | hosted ota <url> yes", .func = cmd_hosted },
    };
    for (int i = 0; i < sizeof cmds / sizeof cmds[0]; i++) esp_console_cmd_register(&cmds[i]);
    iperf_cmd_register_iperf();
    if (bench_cfg_get("autowifi", 0)) xTaskCreate(autowifi_task, "autowifi", 6144, NULL, 3, NULL);
}
