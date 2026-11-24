/*
 * P4OS - Espressif's Zigbee stack on the P4, with the C6 as its radio
 * (docs/plan/ZIGBEE.md, stage 3; aos_zigbee.h).
 *
 * The stack runs here in "radio over a UART" mode (CONFIG_ZB_RADIO_SPINEL_UART):
 * it speaks spinel to an RCP through a file descriptor, which its open
 * source part, esp_zigbee_radio_spinel_config_uart(), would open on a UART.
 * That function is wrapped (-Wl,--wrap, CMakeLists.txt) to hand the stack
 * one end of a pair of loopback UDP sockets instead: what the stack writes
 * comes out of the other end and goes to the C6's RCP over peer_data
 * (aos_154_spinel_send), and what the RCP says comes in through the other
 * end. The stack's own loop selects on the socket as it would on a UART.
 *
 * For now a measuring step: a coordinator forming a network of its own on
 * one channel, closed to joining.
 */
#include "aos_zigbee.h"
#include "aos_154.h"
#include "aos_154_proto.h"
#include "aos_hal.h"

#include <string.h>
#include "esp_log.h"
#include "esp_zigbee.h"
#include "ezbee/zha.h"
#include "esp_radio_spinel.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_vfs_eventfd.h"
#include <fcntl.h>

#define TAG      "zigbee"
#define PORT_ZB  47101                  /* the stack's end */
#define PORT_C6  47102                  /* the bridge's end */

static int s_zb_fd = -1, s_c6_fd = -1;
static volatile uint8_t s_state;        /* AOS_ZB_* */
static uint8_t s_channel;
static uint32_t s_to_c6, s_from_c6;

static int udp_bound(uint16_t port, uint16_t peer)
{
    int s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return -1;
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    struct sockaddr_in p = { .sin_family = AF_INET, .sin_port = htons(peer), .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (bind(s, (struct sockaddr *)&a, sizeof a) || connect(s, (struct sockaddr *)&p, sizeof p)) {
        close(s);
        return -1;
    }
    return s;
}

/* the RCP's spinel, from the link's task: into the stack's end */
static void from_c6(const uint8_t *d, size_t n)
{
    if (s_c6_fd >= 0) {
        send(s_c6_fd, d, n, MSG_DONTWAIT);
        s_from_c6 += n;
    }
}

/* what the stack writes, out of the bridge's end and to the C6 */
static void bridge(void *arg)
{
    (void)arg;
    static uint8_t buf[1024];
    for (;;) {
        int n = recv(s_c6_fd, buf, sizeof buf, 0);
        if (n <= 0) { aos_hal_sleep_ms(10); continue; }
        if (aos_154_spinel_send(buf, (size_t)n)) s_to_c6 += (uint32_t)n;
    }
}

static esp_err_t sock_init(const esp_radio_spinel_uart_config_t *cfg, int *fd)
{
    (void)cfg;
    *fd = s_zb_fd;
    return s_zb_fd >= 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t sock_deinit(const esp_radio_spinel_uart_config_t *cfg, int *fd)
{
    (void)cfg;
    *fd = -1;
    return ESP_OK;
}

/* the stack's link to its RCP: the socket instead of a UART */
esp_err_t __wrap_esp_zigbee_radio_spinel_config_uart(const esp_radio_spinel_uart_config_t *cfg)
{
    esp_err_t e = esp_radio_spinel_uart_interface_enable(cfg, sock_init, sock_deinit, ESP_RADIO_SPINEL_ZIGBEE);
    aos_hal_log(TAG, "the stack's radio: the C6, spinel over peer_data (%s)", esp_err_to_name(e));
    return e;
}

static void commission(void *ctx)
{
    uintptr_t mode = (uintptr_t)ctx;
    if (mode == EZB_BDB_MODE_NETWORK_FORMATION) {
        ezb_bdb_set_primary_channel_set(1u << s_channel);
        ezb_bdb_set_secondary_channel_set(0x07FFF800u);   /* as Espressif's example: all */
    }
    ezb_err_t e = ezb_bdb_start_top_level_commissioning((ezb_bdb_comm_mode_mask_t)mode);
    aos_hal_log(TAG, "commissioning 0x%x asked: %d", (unsigned)mode, e);
}

static bool on_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t t = ezb_app_signal_get_type(sig);
    const ezb_bdb_signal_simple_params_t *p = ezb_app_signal_get_params(sig);
    aos_hal_log(TAG, "signal %s (0x%04x) status %d", ezb_app_signal_to_string(t), t, p ? p->status : -1);
    /* as Espressif's coordinator example (examples/zigbee/thermostat): the
     * commissioning asked for from the handler itself */
    switch (t) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        commission((void *)(uintptr_t)EZB_BDB_MODE_INITIALIZATION);
        return true;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        commission((void *)(uintptr_t)EZB_BDB_MODE_NETWORK_FORMATION);
        return true;
    case EZB_BDB_SIGNAL_FORMATION:
        s_state = (p && p->status == 0) ? AOS_ZB_FORMED : AOS_ZB_FAILED;
        aos_hal_log(TAG, s_state == AOS_ZB_FORMED ? "the network is formed, on channel %u" : "the formation failed (channel %u)",
                    s_channel);
        return true;
    default:
        return false;
    }
}

const char *aos_hal_pref_partition(void);     /* aos_hal_p4.c */

static void stack_task(void *arg)
{
    (void)arg;
    /* the stack keeps its network in NVS: not in the table's "nvs", which
     * the apps' keys fill (aos_hal_p4.c, "Where they live"), and with no
     * room there the formation waited forever with no word (on the board,
     * 2026-11-24); in the preferences' large store instead */
    /* a measuring step: everything the stack says */
    esp_log_level_set("ESP-ZIGBEE", ESP_LOG_VERBOSE);
    esp_log_level_set("ESP_RADIO_SPINEL", ESP_LOG_DEBUG);
    const char *part = aos_hal_pref_partition();
    aos_hal_log(TAG, "the stack's storage: NVS partition %s", part ? part : "nvs");
    esp_zigbee_config_t cfg = {
        .device_config = { .device_type = EZB_NWK_DEVICE_TYPE_COORDINATOR, .install_code_policy = false,
                           .zczr_config = { .max_children = 32 } },
        .platform_config = {
            .storage_partition_name = part ? part : "nvs",
            .radio_config = {
                .radio_mode = ESP_ZIGBEE_RADIO_MODE_UART_RCP,
                /* not a UART: only its port number is read, for a buffer's size */
                .radio_uart_config = { .port = 1, .uart_config = { .baud_rate = 460800, .data_bits = UART_DATA_8_BITS,
                                                                   .parity = UART_PARITY_DISABLE,
                                                                   .stop_bits = UART_STOP_BITS_1,
                                                                   .flow_ctrl = UART_HW_FLOWCTRL_DISABLE },
                                       .rx_pin = -1, .tx_pin = -1 },
            },
        },
    };
    if (esp_zigbee_init(&cfg) != ESP_OK) {
        s_state = AOS_ZB_FAILED;
        aos_hal_log(TAG, "the stack did not start");
        vTaskDelete(NULL);
        return;
    }
    /* a device with one endpoint, a gateway, registered before the start as
     * Espressif's example does: with none the formation was asked for and
     * nothing went to the radio */
    ezb_af_device_desc_t dev = ezb_af_create_device_desc();
    ezb_zha_custom_gateway_config_t gw_cfg = EZB_ZHA_CUSTOM_GATEWAY_CONFIG();
    ezb_af_ep_desc_t ep = ezb_zha_create_custom_gateway(1, &gw_cfg);
    ezb_zcl_cluster_desc_t basic = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x04P4OS");
    ezb_zcl_basic_cluster_desc_add_attr(basic, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x03hub");
    ezb_af_device_add_endpoint_desc(dev, ep);
    ezb_af_device_desc_register(dev);
    ezb_bdb_set_primary_channel_set(1u << s_channel);
    ezb_bdb_set_secondary_channel_set(0x07FFF800u);   /* as Espressif's example: all */
    ezb_app_signal_add_handler(on_signal);
    if (esp_zigbee_start(false) != ESP_OK) s_state = AOS_ZB_FAILED;
    else esp_zigbee_launch_mainloop();
    vTaskDelete(NULL);
}

bool aos_zigbee_start(int channel)
{
    if (s_state == AOS_ZB_STARTING || s_state == AOS_ZB_FORMED) return true;
    if (channel < 11 || channel > 26) return false;
    s_channel = (uint8_t)channel;
    /* the C6 becomes the RCP, and the link must answer before the stack speaks */
    aos_154_rcp_start();
    char v[128];
    if (!aos_154_rcp_probe(v, sizeof v, 3000)) {
        aos_hal_log(TAG, "the C6's RCP does not answer: its firmware is 3.0.9-154.5 or newer?");
        return false;
    }
    if (s_zb_fd < 0) {
        /* the stack's loop waits on eventfds (its task queue, its alarms) */
        esp_vfs_eventfd_config_t ev = { .max_fds = 4 };
        esp_err_t e = esp_vfs_eventfd_register(&ev);
        if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) aos_hal_log(TAG, "eventfd: %s", esp_err_to_name(e));
        s_zb_fd = udp_bound(PORT_ZB, PORT_C6);
        s_c6_fd = udp_bound(PORT_C6, PORT_ZB);
        if (s_zb_fd < 0 || s_c6_fd < 0) return false;
        /* the stack's end as a UART opened O_NONBLOCK is: its reader reads
         * until there is nothing (esp_radio_spinel_uart_interface.cpp,
         * TryReadAndDecode), and a blocking socket would wait there forever
         * (it did, on the board) */
        fcntl(s_zb_fd, F_SETFL, fcntl(s_zb_fd, F_GETFL, 0) | O_NONBLOCK);
        aos_154_spinel_sink(from_c6);
        /* internal stacks: Espressif's example saw the stack fail with its
         * tasks' stacks in PSRAM on the P4 (examples/zigbee/thermostat) */
        xTaskCreate(bridge, "zb_bridge", 4096, NULL, 6, NULL);
    }
    s_state = AOS_ZB_STARTING;
    aos_hal_log(TAG, "starting a coordinator on channel %d (RCP %s)", channel, v);
    return xTaskCreate(stack_task, "zigbee", 8192, NULL, 5, NULL) == pdPASS;
}

void aos_zigbee_info(aos_zigbee_info_t *out)
{
    out->state = s_state;
    out->channel = s_channel;
    out->to_c6 = s_to_c6;
    out->from_c6 = s_from_c6;
}
