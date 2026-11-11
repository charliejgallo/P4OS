/*
 * P4OS - Ethernet through a W5500 on the header (aos_eth.h), on the board:
 * ESP-IDF's esp_eth driver for the chip, given our SPI instead of its own.
 *
 * The driver takes "a custom SPI driver": four calls it makes for every
 * register and every frame. Ours go through aos_io_spi_*, so the port is
 * opened, owned and shared like any other device on the header. A W5500
 * frame is two bytes of address, one of control, then the data, in one chip
 * select; reads and writes are built in two buffers of our own, in PSRAM,
 * behind one mutex (the driver's receive task and lwIP's sender both call).
 *
 * No interrupt pin: the driver polls the chip (POLL_MS). It costs a few
 * milliseconds of latency and saves a pin of a header that has few.
 *
 * Internal RAM this takes, and why (docs/MEMORY.md): the driver's receive
 * task, whose stack ESP-IDF allocates itself (RX_STACK); the SPI service's
 * DMA buffers are already there for the port.
 */
#include "aos_eth.h"
#include "aos_io.h"
#include "aos_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_timer.h"

#define POLL_MS     10
#define RX_STACK    4096
#define FRAME_MAX   1600                /* an Ethernet frame and the chip's two-byte length in front */
#define ROUTE_PRIO  150                 /* the Wi-Fi station has 100, the USB cable 5: the wire goes first */
#define PREF_ON     "eth"
#define PREF_HZ     "eth_hz"

typedef struct {
    aos_io_spi_t     *spi;
    SemaphoreHandle_t lock;
    uint8_t          *tx, *rx;          /* 3 + FRAME_MAX each, in PSRAM */
} eth_spi_t;

static struct {
    SemaphoreHandle_t  lock;            /* start and stop, one at a time */
    esp_eth_handle_t   eth;
    esp_eth_mac_t     *mac;
    esp_eth_phy_t     *phy;
    esp_netif_t       *netif;
    esp_eth_netif_glue_handle_t glue;
    bool               events;
    aos_eth_info_t     info;
    int64_t            link_at;
    char               module[24];      /* "w5500" when modules.txt has it, "" for spi.a's own CS */
    uint32_t           want_hz;         /* the clock asked for since the boot, kept or not */
} E;

/* ---- the driver's SPI, on ours ---- */

static void *spi_init(const void *config)
{
    (void)config;
    eth_spi_t *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    aos_io_spi_cfg_t cfg = { .clock_hz = E.info.spi_hz, .mode = 0, .cs = AOS_IO_SPI_CS_PORT,
                             .module = E.module[0] ? E.module : NULL };
    s->spi = aos_io_spi_open(E.info.port, AOS_ETH_OWNER, &cfg);
    s->lock = xSemaphoreCreateMutex();
    s->tx = heap_caps_malloc(3 + FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s->rx = heap_caps_malloc(3 + FRAME_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s->spi || !s->lock || !s->tx || !s->rx) {
        if (!s->spi) snprintf(E.info.err, sizeof E.info.err, "el puerto %s no se pudo abrir (¿lo tiene otra app?)", E.info.port);
        if (s->spi) aos_io_spi_close(s->spi);
        if (s->lock) vSemaphoreDelete(s->lock);
        heap_caps_free(s->tx);
        heap_caps_free(s->rx);
        free(s);
        return NULL;
    }
    E.info.spi_hz = aos_io_spi_clock(s->spi);
    E.info.cs = aos_io_spi_cs_gpio(s->spi);
    return s;
}

static esp_err_t spi_deinit(void *ctx)
{
    eth_spi_t *s = ctx;
    aos_io_spi_close(s->spi);
    vSemaphoreDelete(s->lock);
    heap_caps_free(s->tx);
    heap_caps_free(s->rx);
    free(s);
    return ESP_OK;
}

/* cmd is the chip's 16-bit address and addr its control byte: the names are the IDF's, for its own SPI. */
static esp_err_t spi_rw(eth_spi_t *s, uint32_t cmd, uint32_t addr, const void *out, void *in, uint32_t len)
{
    if (len > FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    xSemaphoreTake(s->lock, portMAX_DELAY);
    s->tx[0] = (uint8_t)(cmd >> 8);
    s->tx[1] = (uint8_t)cmd;
    s->tx[2] = (uint8_t)addr;
    if (out) memcpy(s->tx + 3, out, len);
    else memset(s->tx + 3, 0xFF, len);
    bool ok = aos_io_spi_xfer(s->spi, s->tx, in ? s->rx : NULL, 3 + len, 1000);
    if (ok && in) memcpy(in, s->rx + 3, len);
    xSemaphoreGive(s->lock);
    return ok ? ESP_OK : ESP_FAIL;
}

static esp_err_t spi_read(void *ctx, uint32_t cmd, uint32_t addr, void *data, uint32_t len)
{
    return spi_rw(ctx, cmd, addr, NULL, data, len);
}

static esp_err_t spi_write(void *ctx, uint32_t cmd, uint32_t addr, const void *data, uint32_t len)
{
    return spi_rw(ctx, cmd, addr, data, NULL, len);
}

/* ---- what the link and the address are doing ---- */

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == ETH_EVENT && id == ETHERNET_EVENT_CONNECTED) {
        eth_speed_t sp = ETH_SPEED_10M;
        eth_duplex_t dx = ETH_DUPLEX_HALF;
        esp_eth_ioctl(E.eth, ETH_CMD_G_SPEED, &sp);
        esp_eth_ioctl(E.eth, ETH_CMD_G_DUPLEX_MODE, &dx);
        E.info.speed_mbps = sp == ETH_SPEED_100M ? 100 : 10;
        E.info.full_duplex = dx == ETH_DUPLEX_FULL;
        E.info.link = true;
        E.info.links++;
        E.link_at = esp_timer_get_time();
        aos_hal_log("eth", "link up: %d Mbit/s %s duplex", E.info.speed_mbps, E.info.full_duplex ? "full" : "half");
    } else if (base == ETH_EVENT && id == ETHERNET_EVENT_DISCONNECTED) {
        E.info.link = false;
        E.info.speed_mbps = 0;
        E.info.ip[0] = E.info.mask[0] = E.info.gw[0] = 0;
        aos_hal_net_wired_set(false, NULL);
        aos_hal_log("eth", "link down");
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        const ip_event_got_ip_t *ev = data;
        if (ev->esp_netif != E.netif) return;
        snprintf(E.info.ip, sizeof E.info.ip, IPSTR, IP2STR(&ev->ip_info.ip));
        snprintf(E.info.mask, sizeof E.info.mask, IPSTR, IP2STR(&ev->ip_info.netmask));
        snprintf(E.info.gw, sizeof E.info.gw, IPSTR, IP2STR(&ev->ip_info.gw));
        aos_hal_log("eth", "address %s, gateway %s", E.info.ip, E.info.gw);
        aos_hal_net_wired_set(true, E.info.ip);
    } else if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP) {
        E.info.ip[0] = E.info.mask[0] = E.info.gw[0] = 0;
        aos_hal_net_wired_set(false, NULL);
        aos_hal_log("eth", "address lost");
    }
}

static void teardown(void)
{
    if (E.eth) esp_eth_stop(E.eth);
    if (E.glue) esp_eth_del_netif_glue(E.glue);
    if (E.netif) esp_netif_destroy(E.netif);
    if (E.eth) esp_eth_driver_uninstall(E.eth);
    if (E.phy) E.phy->del(E.phy);
    if (E.mac) E.mac->del(E.mac);       /* closes the SPI port: spi_deinit() */
    E.eth = NULL;
    E.glue = NULL;
    E.netif = NULL;
    E.phy = NULL;
    E.mac = NULL;
    E.info.running = E.info.link = false;
    E.info.speed_mbps = 0;
    E.info.ip[0] = E.info.mask[0] = E.info.gw[0] = 0;
    aos_hal_net_wired_set(false, NULL);
}

static bool fail(const char *why, esp_err_t e)
{
    if (!E.info.err[0]) {
        if (e != ESP_OK) snprintf(E.info.err, sizeof E.info.err, "%s (%s)", why, esp_err_to_name(e));
        else snprintf(E.info.err, sizeof E.info.err, "%s", why);
    }
    aos_hal_log("eth", "not started: %s", E.info.err);
    teardown();
    return false;
}

static bool start(void)
{
    if (E.info.running) return true;
    E.info.err[0] = 0;
    E.info.links = 0;

    /* where the chip is: its module line, or spi.a with the port's own chip select */
    const aos_io_module_t *m = aos_io_module_find("w5500");
    snprintf(E.module, sizeof E.module, "%s", m ? "w5500" : "");
    snprintf(E.info.port, sizeof E.info.port, "%s", m ? m->port : "spi.a");
    int32_t hz = 0;
    bool kept = aos_hal_pref_get_i32(PREF_HZ, &hz);
    if (E.want_hz) { hz = (int32_t)E.want_hz; kept = true; }
    E.info.spi_hz = kept && hz >= 100000 ? (uint32_t)hz : AOS_ETH_DEFAULT_HZ;
    uint32_t asked = E.info.spi_hz;

    /* whoever came first made these: a second call says so and changes nothing */
    esp_netif_init();
    esp_event_loop_create_default();
    if (!E.events) {
        esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_event, NULL);
        esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP, on_event, NULL);
        E.events = true;
    }

    eth_w5500_config_t wc = {
        .int_gpio_num = -1,
        .poll_period_ms = POLL_MS,
        .custom_spi_driver = { .config = NULL, .init = spi_init, .deinit = spi_deinit, .read = spi_read, .write = spi_write },
    };
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    mc.rx_task_stack_size = RX_STACK;
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.reset_gpio_num = m ? aos_io_module_gpio("w5500", "rst") : -1;
    pc.autonego_timeout_ms = 0;         /* no cable is not an error: the link comes when it comes */

    /* the chip is asked who it is here: no module, or one that is not a W5500, ends this */
    E.mac = esp_eth_mac_new_w5500(&wc, &mc);
    if (!E.mac) return fail("no hay un W5500 que conteste en el puerto", ESP_OK);
    E.phy = esp_eth_phy_new_w5500(&pc);
    if (!E.phy) return fail("no se pudo preparar el W5500", ESP_OK);
    esp_eth_config_t ec = ETH_DEFAULT_CONFIG(E.mac, E.phy);
    esp_err_t e = esp_eth_driver_install(&ec, &E.eth);
    if (e != ESP_OK) { E.eth = NULL; return fail("no hay un W5500 que conteste en el puerto", e); }

    /* the chip has no address of its own: the one the P4 keeps for a wired port */
    esp_read_mac(E.info.mac, ESP_MAC_ETH);
    esp_eth_ioctl(E.eth, ETH_CMD_S_MAC_ADDR, E.info.mac);

    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = ROUTE_PRIO;
    esp_netif_config_t nc = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    E.netif = esp_netif_new(&nc);
    if (!E.netif) return fail("no se pudo crear la interfaz de red", ESP_OK);
    esp_netif_set_hostname(E.netif, aos_hal_device_name());
    E.glue = esp_eth_new_netif_glue(E.eth);
    e = E.glue ? esp_netif_attach(E.netif, E.glue) : ESP_ERR_NO_MEM;
    if (e != ESP_OK) return fail("no se pudo unir el W5500 a la red", e);
    e = esp_eth_start(E.eth);
    if (e != ESP_OK) return fail("el W5500 no arrancó", e);

    E.info.running = true;
    aos_hal_log("eth", "W5500 on %s CS%d at %u Hz (%u asked%s), %02X:%02X:%02X:%02X:%02X:%02X, polled every %d ms",
                E.info.port, E.info.cs, (unsigned)E.info.spi_hz, (unsigned)asked, kept ? "" : ", the default", E.info.mac[0], E.info.mac[1], E.info.mac[2],
                E.info.mac[3], E.info.mac[4], E.info.mac[5], POLL_MS);
    return true;
}

static void take(void)
{
    if (!E.lock) E.lock = xSemaphoreCreateMutex();
    xSemaphoreTake(E.lock, portMAX_DELAY);
}

void aos_eth_boot(void)
{
    int32_t on = 0;
    take();
    E.info.enabled = aos_hal_pref_get_i32(PREF_ON, &on) && on;
    if (E.info.enabled) start();
    xSemaphoreGive(E.lock);
}

bool aos_eth_enable(bool on)
{
    take();
    E.info.enabled = on;
    aos_hal_pref_set_i32(PREF_ON, on ? 1 : 0);
    bool ok = true;
    if (on) ok = start();
    else if (E.info.running) { teardown(); aos_hal_log("eth", "stopped"); }
    xSemaphoreGive(E.lock);
    return ok;
}

void aos_eth_set_clock(uint32_t hz)
{
    if (hz < AOS_IO_SPI_MIN_HZ) hz = AOS_IO_SPI_MIN_HZ;
    if (hz > AOS_IO_SPI_MAX_HZ) hz = AOS_IO_SPI_MAX_HZ;
    E.want_hz = hz;
    if (!aos_hal_pref_set_i32(PREF_HZ, (int32_t)hz)) aos_hal_log("eth", "the SPI clock could not be kept (%u Hz)", (unsigned)hz);
}

void aos_eth_info(aos_eth_info_t *out)
{
    *out = E.info;
    out->link_s = E.info.link ? (uint32_t)((esp_timer_get_time() - E.link_at) / 1000000) : 0;
}
