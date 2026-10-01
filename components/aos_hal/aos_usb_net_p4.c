/*
 * P4OS - the board as a network interface of the computer on its OTG port
 * (CDC-NCM), so the portal answers at http://192.168.7.1 over the cable with
 * no Wi-Fi at all: the log, the files, the OTA. It comes up with the
 * keyboard (aos_usb_p4.c, KEYS mode), docs/USB.md.
 *
 * Ported from the watch (AmoledOS aos_usb_net.c), which measured every trap
 * below. Two halves glued together: TinyUSB's NCM class (frames in and out
 * of the cable) and an esp_netif of our own on lwIP's Ethernet stack, with
 * lwIP's DHCP server, so the computer gets 192.168.7.2 by itself. The server
 * offers no router and no DNS: the computer must not think the board is its
 * way to the internet.
 *
 * Frames from the cable arrive in TinyUSB's own buffer, which it reuses the
 * moment the callback returns, while lwIP keeps the frame until it is done
 * with it: so every frame is copied, into PSRAM.
 *
 * Not in aos_hal.h: the USB driver calls these (aos_p4_usb_net_*).
 */
#include "aos_hal.h"

#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_heap_caps.h"
#include "dhcpserver/dhcpserver.h"
#include "tinyusb_net.h"

static const char *TAG = "usb";

static esp_netif_t            *s_netif;
static esp_netif_driver_base_t s_drv;
static volatile bool           s_up;
static bool                    s_mdns;

static esp_err_t usb_tx(void *h, void *buffer, size_t len)
{
    (void)h;
    return tinyusb_net_send_sync(buffer, (uint16_t)len, NULL, pdMS_TO_TICKS(200));
}

static esp_err_t usb_tx_wrap(void *h, void *buffer, size_t len, void *netstack_buffer)
{
    (void)netstack_buffer;
    return usb_tx(h, buffer, len);
}

static void usb_free_rx(void *h, void *buffer)
{
    (void)h;
    free(buffer);
}

static esp_err_t post_attach(esp_netif_t *netif, esp_netif_iodriver_handle h)
{
    const esp_netif_driver_ifconfig_t cfg = {
        .handle = h,
        .transmit = usb_tx,
        .transmit_wrap = usb_tx_wrap,
        .driver_free_rx_buffer = usb_free_rx,
    };
    s_drv.netif = netif;
    return esp_netif_set_driver_config(netif, &cfg);
}

/* From the TinyUSB task. */
static esp_err_t usb_rx(void *buffer, uint16_t len, void *ctx)
{
    (void)ctx;
    if (!s_netif || !s_up) return ESP_OK;
    void *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) return ESP_ERR_NO_MEM;
    memcpy(copy, buffer, len);
    esp_err_t e = esp_netif_receive(s_netif, copy, len, NULL);
    if (e != ESP_OK) free(copy);
    return e;
}

/* The bus came back (the computer re-enumerated the board: a wake from
 * sleep, a replug) or went away. On the watch, after the Mac slept, the
 * keyboard was fine and the network dead for hours: this interface never
 * heard about the bus and the computer's DHCP renewals fell on a link reset
 * under it. So on attach the netif goes down and up again, which restarts
 * the DHCP server and lwIP's view of the link. From the TinyUSB task. */
void aos_p4_usb_net_relink(bool up)
{
    if (!s_netif) return;
    esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
    if (up) esp_netif_action_connected(s_netif, NULL, 0, NULL);
    s_up = up;
    ESP_LOGI(TAG, "network over the cable %s", up ? "re-armed: the bus came back" : "down: the bus went away");
}

static void ncm_init_cb(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "the computer opened the network interface");
}

/* The address in the descriptor (the iMACAddress string) is the one the
 * COMPUTER gives its own interface - macOS did exactly that on the watch -
 * so the board's side is another one, the last bit flipped, as TinyUSB's
 * own lwIP example does. From the Ethernet MAC the P4 derives from its
 * base one: never the Wi-Fi's (that is the C6's anyway). */
void aos_p4_usb_net_mac(uint8_t mac[6])
{
    esp_read_mac(mac, ESP_MAC_ETH);
}

bool aos_p4_usb_net_start(void)
{
    if (s_netif) return true;
    /* esp_netif comes up with the Wi-Fi; with the Wi-Fi off it may not be
     * yet. INVALID_STATE is "already done". */
    esp_err_t e = esp_netif_init();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(e));
        return false;
    }
    static esp_netif_ip_info_t ip;
    IP4_ADDR(&ip.ip, 192, 168, 7, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    IP4_ADDR(&ip.gw, 192, 168, 7, 1);
    esp_netif_inherent_config_t base = {
        .flags = (esp_netif_flags_t)(ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP),
        .ip_info = &ip,
        .if_key = "USB",
        .if_desc = "usb",
        .route_prio = 5,        /* below the Wi-Fi station: the internet stays on Wi-Fi */
    };
    esp_netif_config_t cfg = { .base = &base, .driver = NULL, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    s_netif = esp_netif_new(&cfg);
    if (!s_netif) {
        ESP_LOGE(TAG, "esp_netif_new failed");
        return false;
    }
    uint8_t mac[6], ours[6];
    aos_p4_usb_net_mac(mac);
    memcpy(ours, mac, 6);
    ours[5] ^= 0x01;
    esp_netif_set_mac(s_netif, ours);
    s_drv.post_attach = post_attach;
    e = esp_netif_attach(s_netif, &s_drv);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_attach: %s", esp_err_to_name(e));
        esp_netif_destroy(s_netif);
        s_netif = NULL;
        return false;
    }
    /* before the server starts (action_start): no router, no DNS */
    dhcps_offer_t none = 0;
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &none, sizeof none);
    esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &none, sizeof none);

    tinyusb_net_config_t nc = { .on_recv_callback = usb_rx, .on_init_callback = ncm_init_cb };
    memcpy(nc.mac_addr, mac, 6);
    e = tinyusb_net_init(&nc);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "tinyusb_net_init: %s", esp_err_to_name(e));
        esp_netif_destroy(s_netif);
        s_netif = NULL;
        return false;
    }
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    esp_netif_action_connected(s_netif, NULL, 0, NULL);
    s_up = true;
    s_mdns = aos_hal_mdns_add_netif(s_netif);
    ESP_LOGI(TAG, "network over the cable: the board is 192.168.7.1, the computer gets .2 by DHCP");
    return true;
}

void aos_p4_usb_net_stop(void)
{
    if (!s_netif) return;
    s_up = false;
    if (s_mdns) {
        aos_hal_mdns_remove_netif(s_netif);
        s_mdns = false;
    }
    esp_netif_action_disconnected(s_netif, NULL, 0, NULL);
    esp_netif_action_stop(s_netif, NULL, 0, NULL);
    tinyusb_net_deinit();
    esp_netif_destroy(s_netif);
    s_netif = NULL;
    ESP_LOGI(TAG, "network over the cable down");
}

bool aos_hal_usb_net_up(void) { return s_up; }
