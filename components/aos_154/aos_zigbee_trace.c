/*
 * P4OS - a measuring aid while the formation does not finish (branch
 * zigbee-p4): counters on the calls between esp-zigbee-lib's core and its
 * platform (alarms, radio), each through -Wl,--wrap (CMakeLists.txt), and the
 * last values they carried. The probe in aos_zigbee_p4.c prints them. Only
 * counting: nothing here changes what the calls do.
 */
#include <stdbool.h>
#include <stdint.h>
#include "ezbee/platform/radio.h"
#include "ezbee/platform/alarm.h"
#include "aos_zigbee_trace.h"
#include <stdio.h>
#include "aos_hal.h"

aos_zb_trace_t g_zb_trace;

void __real_ezb_plat_milli_alarm_start_at(uint32_t t0, uint32_t dt);
void __real_ezb_plat_micro_alarm_start_at(uint32_t t0, uint32_t dt);
void __real_ezb_plat_signal_milli_alarm_fired(void);
void __real_ezb_plat_signal_micro_alarm_fired(void);
ezb_err_t __real_ezb_plat_radio_enable(void);
ezb_err_t __real_ezb_plat_radio_receive(uint8_t channel);
ezb_err_t __real_ezb_plat_radio_transmit(ezb_radio_frame_t *frame);
ezb_err_t __real_ezb_plat_radio_energy_detect(uint8_t channel, uint32_t duration);
void __real_ezb_plat_radio_energy_detect_done(int8_t max_rssi);
void __real_ezb_plat_radio_receive_done(ezb_radio_frame_t *frame, ezb_err_t error);
void __real_ezb_plat_radio_transmit_done(ezb_radio_frame_t *frame, ezb_radio_frame_t *ack, ezb_err_t error);

void __wrap_ezb_plat_milli_alarm_start_at(uint32_t t0, uint32_t dt)
{
    g_zb_trace.ms_start++;
    g_zb_trace.ms_last_dt = dt;
    __real_ezb_plat_milli_alarm_start_at(t0, dt);
}

void __wrap_ezb_plat_micro_alarm_start_at(uint32_t t0, uint32_t dt)
{
    g_zb_trace.us_start++;
    __real_ezb_plat_micro_alarm_start_at(t0, dt);
}

void __wrap_ezb_plat_signal_milli_alarm_fired(void)
{
    g_zb_trace.ms_fired++;
    __real_ezb_plat_signal_milli_alarm_fired();
}

void __wrap_ezb_plat_signal_micro_alarm_fired(void)
{
    g_zb_trace.us_fired++;
    __real_ezb_plat_signal_micro_alarm_fired();
}

ezb_err_t __wrap_ezb_plat_radio_enable(void)
{
    ezb_err_t e = __real_ezb_plat_radio_enable();
    g_zb_trace.enable++;
    g_zb_trace.enable_err = e;
    return e;
}

ezb_err_t __wrap_ezb_plat_radio_receive(uint8_t channel)
{
    ezb_err_t e = __real_ezb_plat_radio_receive(channel);
    g_zb_trace.receive++;
    g_zb_trace.receive_ch = channel;
    g_zb_trace.receive_err = e;
    return e;
}

ezb_err_t __wrap_ezb_plat_radio_transmit(ezb_radio_frame_t *frame)
{
    ezb_err_t e = __real_ezb_plat_radio_transmit(frame);
    g_zb_trace.transmit++;
    g_zb_trace.transmit_err = e;
    return e;
}

ezb_err_t __wrap_ezb_plat_radio_energy_detect(uint8_t channel, uint32_t duration)
{
    ezb_err_t e = __real_ezb_plat_radio_energy_detect(channel, duration);
    g_zb_trace.ed++;
    g_zb_trace.ed_ch = channel;
    g_zb_trace.ed_dur = duration;
    g_zb_trace.ed_err = e;
    return e;
}

void __wrap_ezb_plat_radio_energy_detect_done(int8_t max_rssi)
{
    g_zb_trace.ed_done++;
    g_zb_trace.ed_rssi = max_rssi;
    __real_ezb_plat_radio_energy_detect_done(max_rssi);
}

void __wrap_ezb_plat_radio_receive_done(ezb_radio_frame_t *frame, ezb_err_t error)
{
    g_zb_trace.rx_done++;
    __real_ezb_plat_radio_receive_done(frame, error);
}

void __wrap_ezb_plat_radio_transmit_done(ezb_radio_frame_t *frame, ezb_radio_frame_t *ack, ezb_err_t error)
{
    g_zb_trace.tx_done++;
    g_zb_trace.tx_done_err = error;
    __real_ezb_plat_radio_transmit_done(frame, ack, error);
}

aos_zb_tail_t g_zb_tail_to, g_zb_tail_from;

void aos_zb_tail_add(aos_zb_tail_t *t, const uint8_t *d, size_t n)
{
    for (size_t i = 0; i < n; i++) t->b[(t->n + i) % AOS_ZB_TAIL] = d[i];
    t->n += (uint32_t)n;
}

void aos_zb_tail_log(const char *what, const aos_zb_tail_t *t)
{
    uint32_t n = t->n < AOS_ZB_TAIL ? t->n : AOS_ZB_TAIL;
    uint32_t from = t->n - n;
    char line[3 * 40 + 1];
    for (uint32_t i = 0; i < n; i += 40) {
        size_t k = 0;
        for (uint32_t j = i; j < n && j < i + 40; j++)
            k += (size_t)snprintf(line + k, sizeof line - k, "%02x ", t->b[(from + j) % AOS_ZB_TAIL]);
        aos_hal_log("zigbee", "  %s +%u: %s", what, (unsigned)(from + i), line);
    }
}
