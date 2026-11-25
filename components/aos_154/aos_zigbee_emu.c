/*
 * P4OS - the board emulating a Zigbee device on someone else's network
 * (aos_zigbee.h, docs/plan/ZIGBEE-EMULAR.md, stage 1).
 *
 * The stack (aos_zigbee_p4.c) starts as a router instead of a coordinator,
 * with the emulated device's endpoints instead of the gateway's, and joins
 * any network open for joining (BDB network steering on every channel,
 * again every 5 s for 3 minutes). Its network lives in the stack's NVS
 * namespace "zbemu" instead of "zigbee" (aos_zigbee_nvs.c), so the board's
 * own network is kept as it was, and the stack comes back to the other one
 * by itself after a restart (DEVICE_REBOOT, status 0).
 *
 * Stage 1's one kind, AOS_ZBE_LIGHT_TEMP:
 *   ep 1  on/off light (0x0100): Basic, Identify, Groups, Scenes, On/Off
 *   ep 2  temperature sensor (0x0302): Basic, Identify, Temperature 0x0402,
 *         Humidity 0x0405, Power Configuration 0x0001 (a make-believe battery)
 * Basic says manufacturer "P4OS", model "P4OS-EMU-LT", mains powered.
 *
 * The coordinator's binds and reporting configurations are answered by the
 * stack; this file only watches them for the log: every ZCL frame (the raw
 * handler, before the stack), every ZDO request (the APS indication hook)
 * and the attributes the coordinator changed (the core's action handler),
 * all letting the stack go on with them. Not the APS confirm hook: with it
 * the application must handle every confirm (aps.h).
 *
 * Switching between the board's own network and this one restarts the
 * board (the stack does not start twice in one boot): "zbe_boot" says what
 * to start at the next boot (aos_zigbee_boot, from main.c).
 */
#include "aos_zigbee.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_zigbee.h"
#include "ezbee/aps.h"
#include "ezbee/bdb.h"
#include "ezbee/nwk.h"
#include "ezbee/zha.h"
#include "ezbee/zcl.h"
#include "ezbee/zcl/cluster/basic_desc.h"
#include "ezbee/zcl/cluster/power_config_desc.h"
#include "ezbee/zcl/cluster/rel_humidity_measurement_desc.h"
#include "ezbee/zcl/cluster/temperature_measurement_desc.h"

#define TAG "zigbee"
#define LOG_N 160

/* aos_zigbee_p4.c */
bool aos_zigbee_p4_start(int channel, bool emu);
bool aos_zigbee_p4_running(void);

static volatile uint8_t s_state;
static uint8_t s_kind;
static bool s_active, s_tclk, s_leaving;
static uint16_t s_tries;
static uint32_t s_joined_ms, s_identify_until;
static int32_t s_v[AOS_ZBE_V_N] = { 2150, 5000, 100, 0 };

static aos_zigbee_emu_log_t *s_log;     /* PSRAM, a ring of LOG_N */
static uint32_t s_log_n;
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;

static esp_timer_handle_t s_retry, s_reboot;

/* ---- the log ------------------------------------------------------------- */

static void logf_(bool out, const char *fmt, ...)
{
    char t[sizeof s_log[0].text];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(t, sizeof t, fmt, ap);
    va_end(ap);
    aos_hal_log(TAG, "emu: %s", t);
    pthread_mutex_lock(&s_mx);
    if (!s_log) s_log = heap_caps_calloc(LOG_N, sizeof *s_log, MALLOC_CAP_SPIRAM);
    if (s_log) {
        aos_zigbee_emu_log_t *e = &s_log[s_log_n % LOG_N];
        e->ms = (uint32_t)aos_hal_uptime_ms();
        e->out = out;
        snprintf(e->text, sizeof e->text, "%s", t);
        s_log_n++;
    }
    pthread_mutex_unlock(&s_mx);
}

int aos_zigbee_emu_log(aos_zigbee_emu_log_t *out, uint32_t from, int max, uint32_t *next)
{
    pthread_mutex_lock(&s_mx);
    uint32_t first = s_log_n > LOG_N ? s_log_n - LOG_N : 0;
    if (from < first) from = first;
    int n = 0;
    for (uint32_t i = from; i < s_log_n && n < max && s_log; i++) out[n++] = s_log[i % LOG_N];
    if (next) *next = from + (uint32_t)n;
    pthread_mutex_unlock(&s_mx);
    return n;
}

static const char *cluster_name(uint16_t c)
{
    switch (c) {
    case 0x0000: return "Basic";
    case 0x0001: return _("batería");
    case 0x0003: return "Identify";
    case 0x0004: return "Groups";
    case 0x0005: return "Scenes";
    case 0x0006: return "On/Off";
    case 0x0008: return "Level";
    case 0x0019: return "OTA";
    case 0x0402: return _("temperatura");
    case 0x0405: return _("humedad");
    default: {
        static char b[8];
        snprintf(b, sizeof b, "0x%04x", c);
        return b;
    }
    }
}

/* ---- the values ---------------------------------------------------------- */

static void save_vals(void)
{
    char v[64];
    snprintf(v, sizeof v, "%ld,%ld,%ld,%ld", (long)s_v[0], (long)s_v[1], (long)s_v[2], (long)s_v[3]);
    aos_hal_pref_set_str("zbe_vals", v);
}

static void load_vals(void)
{
    char v[64];
    long a, b, c, d;
    if (aos_hal_pref_get_str("zbe_vals", v, sizeof v) && sscanf(v, "%ld,%ld,%ld,%ld", &a, &b, &c, &d) == 4) {
        s_v[0] = (int32_t)a, s_v[1] = (int32_t)b, s_v[2] = (int32_t)c, s_v[3] = (int32_t)d;
    }
}

/* one value into its attribute: in the stack's task */
static void put_value(void *ctx)
{
    int k = (int)(intptr_t)ctx;
    ezb_zcl_status_t st = EZB_ZCL_STATUS_SUCCESS;
    switch (k) {
    case AOS_ZBE_V_TEMP: {
        int16_t t = (int16_t)s_v[k];
        st = ezb_zcl_set_attr_value(2, 0x0402, EZB_ZCL_CLUSTER_SERVER, 0x0000, EZB_ZCL_STD_MANUF_CODE, &t, false);
        break;
    }
    case AOS_ZBE_V_HUM: {
        uint16_t h = (uint16_t)s_v[k];
        st = ezb_zcl_set_attr_value(2, 0x0405, EZB_ZCL_CLUSTER_SERVER, 0x0000, EZB_ZCL_STD_MANUF_CODE, &h, false);
        break;
    }
    case AOS_ZBE_V_BATTERY: {
        uint8_t b = (uint8_t)(s_v[k] * 2);     /* half percents */
        uint8_t mv = (uint8_t)(24 + s_v[k] * 6 / 100);     /* 2.4 to 3.0 V, in 100 mV */
        st = ezb_zcl_set_attr_value(2, 0x0001, EZB_ZCL_CLUSTER_SERVER, 0x0021, EZB_ZCL_STD_MANUF_CODE, &b, false);
        ezb_zcl_set_attr_value(2, 0x0001, EZB_ZCL_CLUSTER_SERVER, 0x0020, EZB_ZCL_STD_MANUF_CODE, &mv, false);
        break;
    }
    case AOS_ZBE_V_ON: {
        bool on = s_v[k] != 0;
        st = ezb_zcl_set_attr_value(1, 0x0006, EZB_ZCL_CLUSTER_SERVER, 0x0000, EZB_ZCL_STD_MANUF_CODE, &on, false);
        break;
    }
    }
    if (st != EZB_ZCL_STATUS_SUCCESS) aos_hal_log(TAG, "emu: value %d not set: 0x%02x", k, st);
}

bool aos_zigbee_emu_set(int value, int32_t v)
{
    if (value < 0 || value >= AOS_ZBE_V_N) return false;
    switch (value) {
    case AOS_ZBE_V_TEMP: v = v < -4000 ? -4000 : v > 8500 ? 8500 : v; break;
    case AOS_ZBE_V_HUM: v = v < 0 ? 0 : v > 10000 ? 10000 : v; break;
    case AOS_ZBE_V_BATTERY: v = v < 0 ? 0 : v > 100 ? 100 : v; break;
    case AOS_ZBE_V_ON: v = v != 0; break;
    }
    s_v[value] = v;
    save_vals();
    if (!s_active) return true;
    if (value == AOS_ZBE_V_ON) logf_(true, _("la luz, tocada en la placa: %s"), v ? _("prendida") : _("apagada"));
    return esp_zigbee_task_queue_post(put_value, (void *)(intptr_t)value) == ESP_OK;
}

/* every value to the coordinator now, whatever reporting it configured */
static void report_all(void *ctx)
{
    (void)ctx;
    static const struct { uint8_t ep; uint16_t cl, at; } R[] = {
        { 1, 0x0006, 0x0000 }, { 2, 0x0402, 0x0000 }, { 2, 0x0405, 0x0000 }, { 2, 0x0001, 0x0021 } };
    for (size_t i = 0; i < sizeof R / sizeof R[0]; i++) {
        ezb_zcl_report_attr_cmd_t r = { 0 };
        r.cmd_ctrl.dst_addr.addr_mode = EZB_ADDR_MODE_SHORT;
        r.cmd_ctrl.dst_addr.u.short_addr = 0x0000;
        r.cmd_ctrl.dst_ep = 1;
        r.cmd_ctrl.src_ep = R[i].ep;
        r.cmd_ctrl.cluster_id = R[i].cl;
        r.cmd_ctrl.manuf_code = EZB_ZCL_STD_MANUF_CODE;
        r.cmd_ctrl.fc.direction = EZB_ZCL_CMD_DIRECTION_TO_CLI;
        r.payload.attr_id = R[i].at;
        ezb_err_t e = ezb_zcl_report_attr_cmd_req(&r);
        if (e != EZB_ERR_NONE) aos_hal_log(TAG, "emu: report of 0x%04x: %d", R[i].cl, e);
    }
}

bool aos_zigbee_emu_report_now(void)
{
    if (!s_active || s_state != AOS_ZBE_JOINED) return false;
    logf_(true, "%s", _("mandó todos los valores al coordinador"));
    return esp_zigbee_task_queue_post(report_all, NULL) == ESP_OK;
}

/* ---- the device's endpoints, before the stack starts ---------------------- */

static void basic_attrs(ezb_af_ep_desc_t ep)
{
    ezb_zcl_cluster_desc_t b = ezb_af_endpoint_get_cluster_desc(ep, EZB_ZCL_CLUSTER_ID_BASIC, EZB_ZCL_CLUSTER_SERVER);
    /* ZCL strings: their length first */
    ezb_zcl_basic_cluster_desc_add_attr(b, EZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)"\x04P4OS");
    ezb_zcl_basic_cluster_desc_add_attr(b, EZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)"\x0bP4OS-EMU-LT");
    ezb_zcl_basic_cluster_desc_add_attr(b, EZB_ZCL_ATTR_BASIC_DATE_CODE_ID, (void *)"\x08" "20261125");
    ezb_zcl_basic_cluster_desc_add_attr(b, EZB_ZCL_ATTR_BASIC_SW_BUILD_ID_ID, (void *)"\x04P4OS");
}

void aos_zigbee_emu_endpoints(ezb_af_device_desc_t dev)
{
    load_vals();
    ezb_zha_on_off_light_config_t lc = EZB_ZHA_ON_OFF_LIGHT_CONFIG();
    lc.basic_cfg.power_source = 0x01;   /* mains: a router is, whatever it shows */
    lc.on_off_cfg.on_off = s_v[AOS_ZBE_V_ON] != 0;
    ezb_af_ep_desc_t e1 = ezb_zha_create_on_off_light(1, &lc);
    basic_attrs(e1);
    ezb_af_device_add_endpoint_desc(dev, e1);

    ezb_zha_temperature_sensor_config_t tc = EZB_ZHA_TEMPERATURE_SENSOR_CONFIG();
    tc.basic_cfg.power_source = 0x01;
    tc.temp_meas_cfg.measured_value = (int16_t)s_v[AOS_ZBE_V_TEMP];
    tc.temp_meas_cfg.min_measured_value = -4000;
    tc.temp_meas_cfg.max_measured_value = 8500;
    ezb_af_ep_desc_t e2 = ezb_zha_create_temperature_sensor(2, &tc);
    basic_attrs(e2);
    ezb_zcl_rel_humidity_measurement_cluster_server_config_t hc = {
        .measured_value = (uint16_t)s_v[AOS_ZBE_V_HUM], .min_measured_value = 0, .max_measured_value = 10000 };
    ezb_af_endpoint_add_cluster_desc(e2, ezb_zcl_rel_humidity_measurement_create_cluster_desc(&hc, EZB_ZCL_CLUSTER_SERVER));
    ezb_zcl_power_config_cluster_server_config_t pc = { 0 };
    ezb_zcl_cluster_desc_t pw = ezb_zcl_power_config_create_cluster_desc(&pc, EZB_ZCL_CLUSTER_SERVER);
    uint8_t pct = (uint8_t)(s_v[AOS_ZBE_V_BATTERY] * 2), mv = 30;
    ezb_zcl_power_config_cluster_desc_add_attr(pw, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_PERCENTAGE_REMAINING_ID, &pct);
    ezb_zcl_power_config_cluster_desc_add_attr(pw, EZB_ZCL_ATTR_POWER_CONFIG_BATTERY_VOLTAGE_ID, &mv);
    ezb_af_endpoint_add_cluster_desc(e2, pw);
    ezb_af_device_add_endpoint_desc(dev, e2);
}

/* ---- what the coordinator asks, into words -------------------------------- */

static uint16_t u16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* the size of a reportable change for a ZCL data type (analog ones) */
static int change_size(uint8_t t)
{
    if (t >= 0x20 && t <= 0x27) return t - 0x1F;       /* uint8..uint64 */
    if (t >= 0x28 && t <= 0x2F) return t - 0x27;       /* int8..int64 */
    if (t == 0x39) return 4;
    if (t == 0x38) return 2;
    if (t == 0x3A) return 8;
    return 0;                                           /* discrete: none */
}

static bool on_raw(const ezb_zcl_raw_frame_t *f)
{
    const ezb_zcl_cmd_hdr_t *h = f ? f->header : NULL;
    if (!h) return false;
    const uint8_t *p = f->payload;
    int n = f->payload_length;
    const char *cl = cluster_name(h->cluster_id);
    if ((h->fc & 0x03) == 0) {
        switch (h->cmd_id) {
        case 0x00: {                    /* read attributes */
            char at[48] = "";
            int k = 0;
            for (int i = 0; i + 1 < n && k < (int)sizeof at - 6; i += 2) k += snprintf(at + k, sizeof at - (size_t)k, " %04x", u16(p + i));
            logf_(false, _("leyó %s (ep %u):%s"), cl, h->dst_ep, at);
            break;
        }
        case 0x02: logf_(false, _("escribió en %s (ep %u)"), cl, h->dst_ep); break;
        case 0x06: {                    /* configure reporting: each record */
            for (int i = 0; i + 3 <= n;) {
                uint8_t dir = p[i];
                uint16_t at = u16(p + i + 1);
                if (dir == 0 && i + 8 <= n) {
                    uint8_t t = p[i + 3];
                    uint16_t mn = u16(p + i + 4), mx = u16(p + i + 6);
                    int cs = change_size(t);
                    logf_(false, _("configuró reportes de %s 0x%04x: cada %u s a %u s"), cl, at, mn, mx);
                    i += 8 + cs;
                } else
                    i += dir == 0 ? n : 5;
            }
            break;
        }
        case 0x08: logf_(false, _("leyó la configuración de reportes de %s"), cl); break;
        case 0x0C: logf_(false, _("preguntó qué atributos tiene %s"), cl); break;
        case 0x11:
        case 0x13: logf_(false, _("preguntó qué comandos tiene %s"), cl); break;
        case 0x0B: break;               /* a default response */
        default: logf_(false, _("comando general 0x%02x en %s"), h->cmd_id, cl); break;
        }
        return false;
    }
    if (h->cluster_id == 0x0006 && h->cmd_id <= 2) {
        static const char *const W[] = { N_("Apagar"), N_("Prender"), N_("Cambiar") };
        logf_(false, _("mandó %s a la luz"), aos_tr(W[h->cmd_id]));
    } else if (h->cluster_id == 0x0003 && h->cmd_id == 0x00 && n >= 2) {
        logf_(false, _("pidió que se identifique %u s"), u16(p));
    } else
        logf_(false, _("comando 0x%02x de %s"), h->cmd_id, cl);
    return false;
}

static bool on_aps(const ezb_apsde_data_ind_t *ind)
{
    if (!ind || ind->profile_id != 0x0000 || ind->dst_endpoint != 0) return false;
    const uint8_t *p = ind->asdu;
    int n = ind->asdu_length;
    switch (ind->cluster_id) {
    case 0x0000: logf_(false, "%s", _("pidió la dirección corta")); break;
    case 0x0001: logf_(false, "%s", _("pidió la dirección larga")); break;
    case 0x0002: logf_(false, "%s", _("pidió el descriptor del nodo")); break;
    case 0x0003: logf_(false, "%s", _("pidió el descriptor de energía")); break;
    case 0x0004: logf_(false, _("pidió el descriptor del endpoint %u"), n >= 4 ? p[3] : 0); break;
    case 0x0005: logf_(false, "%s", _("pidió la lista de endpoints")); break;
    case 0x0021:
    case 0x0022:
        if (n >= 12) logf_(false, ind->cluster_id == 0x21 ? _("ató %s (ep %u) a sí mismo") : _("desató %s (ep %u)"),
                           cluster_name(u16(p + 10)), p[9]);
        break;
    case 0x0031: logf_(false, "%s", _("pidió la tabla de vecinos")); break;
    case 0x0032: logf_(false, "%s", _("pidió la tabla de rutas")); break;
    case 0x0034: logf_(false, "%s", _("le pidió a la placa que se vaya de la red")); break;
    case 0x0036: if (n >= 2) logf_(false, _("abrió la red para sumar por %u s"), p[1]); break;
    case 0x0013: break;                 /* someone's announce */
    default:
        if (ind->cluster_id < 0x8000) logf_(false, _("pedido ZDO 0x%04x"), ind->cluster_id);
        break;
    }
    return false;
}

static void on_action(ezb_zcl_core_action_callback_id_t id, void *msg)
{
    if (id != EZB_ZCL_CORE_SET_ATTR_VALUE_CB_ID || !msg) return;
    ezb_zcl_set_attr_value_message_t *m = msg;
    m->out.result = EZB_ZCL_STATUS_SUCCESS;
    const ezb_zcl_attribute_t *a = &m->in.attribute;
    if (m->info.cluster_id == 0x0006 && a->id == 0x0000 && a->data.value) {
        s_v[AOS_ZBE_V_ON] = *(const uint8_t *)a->data.value != 0;
        save_vals();
        logf_(false, _("la luz quedó %s"), s_v[AOS_ZBE_V_ON] ? _("prendida") : _("apagada"));
    } else if (m->info.cluster_id == 0x0003 && a->id == 0x0000 && a->data.value) {
        uint16_t s = *(const uint16_t *)a->data.value;
        s_identify_until = (uint32_t)aos_hal_uptime_ms() + s * 1000u;
    }
}

void aos_zigbee_emu_handlers(void)
{
    ezb_zcl_raw_command_handler_register(on_raw);
    ezb_apsde_data_indication_handler_register(on_aps);
    ezb_zcl_core_action_handler_register(on_action);
}

/* ---- joining --------------------------------------------------------------- */

static void steer(void *ctx)
{
    (void)ctx;
    ezb_bdb_set_primary_channel_set(0x07FFF800u);
    ezb_bdb_set_secondary_channel_set(0x07FFF800u);
    ezb_err_t e = ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_NETWORK_STEERING);
    aos_hal_log(TAG, "emu: looking for a network to join (try %u): %d", s_tries, e);
}

static void retry_cb(void *arg)
{
    (void)arg;
    esp_zigbee_task_queue_post(steer, NULL);
}

static void steer_later(void)
{
    if (++s_tries > 36) {               /* 3 minutes */
        s_state = AOS_ZBE_FAILED;
        logf_(true, "%s", _("no encontró una red abierta en 3 minutos"));
        return;
    }
    s_state = AOS_ZBE_SEARCHING;
    const esp_timer_create_args_t a = { .callback = retry_cb, .name = "zbe_retry" };
    if (!s_retry && esp_timer_create(&a, &s_retry) != ESP_OK) return;
    esp_timer_stop(s_retry);
    esp_timer_start_once(s_retry, s_tries == 1 ? 100000 : 5000000);
}

static void joined(void)
{
    s_state = AOS_ZBE_JOINED;
    s_joined_ms = (uint32_t)aos_hal_uptime_ms();
    aos_hal_pref_set_i32("zbe_saved", 1);
    logf_(true, _("adentro: canal %u, PAN 0x%04x, la placa es 0x%04x"), ezb_nwk_get_current_channel(), ezb_nwk_get_panid(),
          ezb_nwk_get_short_address());
    /* the values as last left, in the attributes */
    for (int k = 0; k < AOS_ZBE_V_N; k++) put_value((void *)(intptr_t)k);
}

static void home_cb(void *arg)
{
    (void)arg;
    aos_hal_pref_set_i32("zbe_mode", 0);
    aos_hal_pref_set_i32("zbe_boot", 1);
    aos_hal_reboot();
}

static void home_later(uint32_t ms)
{
    const esp_timer_create_args_t a = { .callback = home_cb, .name = "zbe_home" };
    if (!s_reboot && esp_timer_create(&a, &s_reboot) != ESP_OK) return;
    esp_timer_stop(s_reboot);
    esp_timer_start_once(s_reboot, (uint64_t)ms * 1000);
}

bool aos_zigbee_emu_signal(const ezb_app_signal_t *sig)
{
    ezb_app_signal_type_t t = ezb_app_signal_get_type(sig);
    const ezb_bdb_signal_simple_params_t *p = ezb_app_signal_get_params(sig);
    int st = p ? p->status : -1;
    switch (t) {
    case EZB_ZDO_SIGNAL_SKIP_STARTUP:
        ezb_bdb_start_top_level_commissioning(EZB_BDB_MODE_INITIALIZATION);
        return true;
    case EZB_BDB_SIGNAL_DEVICE_FIRST_START:
        logf_(true, "%s", _("buscando una red abierta para sumarse"));
        steer_later();
        return true;
    case EZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (st == 0) {
            logf_(true, "%s", _("de vuelta en la red de antes"));
            joined();
        } else {
            logf_(true, "%s", _("la red de antes no está: buscando una abierta"));
            steer_later();
        }
        return true;
    case EZB_BDB_SIGNAL_STEERING:
        if (st == 0) joined();
        else steer_later();
        return true;
    case EZB_SECUR_SIGNAL_TCLK_READY:
        s_tclk = true;
        logf_(true, "%s", _("el coordinador le dio su propia clave de enlace (Zigbee 3.0)"));
        return true;
    case EZB_ZDO_SIGNAL_LEAVE: {
        const ezb_zdo_signal_leave_params_t *l = ezb_app_signal_get_params(sig);
        bool reset = !l || l->leave_type == EZB_ZDO_LEAVE_TYPE_RESET;
        s_state = AOS_ZBE_LEFT;
        if (reset) aos_hal_pref_set_i32("zbe_saved", 0);
        logf_(true, "%s", s_leaving ? _("salió de la red") : _("el coordinador la sacó de la red"));
        if (s_leaving) home_later(1500);
        return true;
    }
    default:
        return false;
    }
}

/* ---- the API --------------------------------------------------------------- */

bool aos_zigbee_emu_active(void) { return s_active; }

static bool start_now(int kind)
{
    s_kind = (uint8_t)kind;
    s_active = true;
    s_tries = 0;
    s_state = AOS_ZBE_SEARCHING;
    aos_hal_pref_set_i32("zbe_kind", kind);
    aos_hal_pref_set_i32("zbe_mode", 1);    /* until Back to My network: a restart comes back here */
    logf_(true, "%s", _("arrancando la pila como router"));
    if (aos_zigbee_p4_start(26, true)) return true;
    s_active = false;
    s_state = AOS_ZBE_FAILED;
    logf_(true, "%s", _("la pila no arrancó (¿el C6 tiene su firmware 154?)"));
    return false;
}

bool aos_zigbee_emu_join(int kind, bool restart_ok, bool *restart)
{
    if (restart) *restart = false;
    if (kind < 0 || kind >= AOS_ZBE_KINDS) return false;
    if (s_active) {
        /* out of a network, or tired of looking: looking again */
        if (s_state == AOS_ZBE_FAILED || s_state == AOS_ZBE_LEFT) {
            s_tries = 0;
            s_leaving = false;
            logf_(true, "%s", _("buscando una red abierta para sumarse"));
            steer_later();
        }
        return true;
    }
    if (aos_zigbee_p4_running()) {
        /* the board's own network: the switch is a restart */
        if (restart) *restart = true;
        if (!restart_ok) return false;
        aos_hal_pref_set_i32("zbe_kind", kind);
        aos_hal_pref_set_i32("zbe_boot", 2);
        aos_hal_log(TAG, "emu: restarting to emulate");
        aos_hal_reboot();
        return false;
    }
    return start_now(kind);
}

static void leave_task(void *ctx)
{
    (void)ctx;
    ezb_bdb_reset_via_local_action();
}

bool aos_zigbee_emu_leave(void)
{
    if (!s_active) return false;
    s_leaving = true;
    logf_(true, "%s", _("pidió irse de la red y olvidarla"));
    home_later(6000);                   /* home even if the leave never says it went */
    return esp_zigbee_task_queue_post(leave_task, NULL) == ESP_OK;
}

bool aos_zigbee_emu_home(void)
{
    if (!s_active) return aos_zigbee_start(26);
    logf_(true, "%s", _("vuelve a la red propia (se reinicia)"));
    home_later(300);
    return true;
}

void aos_zigbee_emu_info(aos_zigbee_emu_t *out)
{
    memset(out, 0, sizeof *out);
    out->state = s_active ? s_state : AOS_ZBE_OFF;
    out->kind = s_kind;
    int32_t saved = 0;
    aos_hal_pref_get_i32("zbe_saved", &saved);
    out->saved = saved != 0;
    out->tclk = s_tclk;
    out->tries = s_tries;
    if (!s_active) load_vals();
    memcpy(out->v, s_v, sizeof s_v);
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    out->identify_s = (uint16_t)((int32_t)(s_identify_until - now) > 0 ? (s_identify_until - now + 999) / 1000 : 0);
    if (s_active && s_state == AOS_ZBE_JOINED) {
        out->channel = ezb_nwk_get_current_channel();
        out->pan = ezb_nwk_get_panid();
        out->nwk = ezb_nwk_get_short_address();
        ezb_extpanid_t x;
        ezb_nwk_get_extended_panid(&x);
        memcpy(out->xpan, &x, 8);
        out->joined_ms = s_joined_ms;
    }
}

/* ---- at boot --------------------------------------------------------------- */

static void boot_task(void *arg)
{
    int mode = (int)(intptr_t)arg;
    extern bool aos_hal_c6_link_wait(uint32_t ms);
    aos_hal_c6_link_wait(15000);
    vTaskDelay(pdMS_TO_TICKS(3000));    /* the Wi-Fi up first, as when the person starts it */
    if (mode == 2) {
        int32_t kind = 0;
        aos_hal_pref_get_i32("zbe_kind", &kind);
        start_now((int)kind);
    } else {
        aos_hal_log(TAG, "back to the board's own network");
        aos_zigbee_start(26);
    }
    vTaskDelete(NULL);
}

void aos_zigbee_boot(void)
{
    /* a switch asked for this boot ("zbe_boot", once: a restart that hangs
     * does not loop), or the board was emulating when it went down
     * ("zbe_mode", until Back to My network): it goes back to that network
     * by itself, as a real device would */
    int32_t mode = 0, emu = 0;
    if (aos_hal_pref_get_i32("zbe_boot", &mode) && mode) aos_hal_pref_erase("zbe_boot");
    else if (aos_hal_pref_get_i32("zbe_mode", &emu) && emu) mode = 2;
    if (!mode) return;
    xTaskCreate(boot_task, "zb_boot", 4096, (void *)(intptr_t)mode, 4, NULL);
}
