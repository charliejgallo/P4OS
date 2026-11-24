/* P4OS - counters on esp-zigbee-lib's platform calls (aos_zigbee_trace.c) */
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t ms_start, ms_fired, us_start, us_fired, ms_last_dt;
    uint32_t enable, receive, transmit, ed, ed_done, rx_done, tx_done;
    int enable_err, receive_err, transmit_err, ed_err, tx_done_err;
    uint32_t ed_dur;
    uint8_t receive_ch, ed_ch;
    int8_t ed_rssi;
} aos_zb_trace_t;

extern aos_zb_trace_t g_zb_trace;

/* the last bytes each way between the stack and the C6, raw (HDLC) */
#define AOS_ZB_TAIL 400
typedef struct { uint8_t b[AOS_ZB_TAIL]; uint32_t n; } aos_zb_tail_t;
extern aos_zb_tail_t g_zb_tail_to, g_zb_tail_from;
void aos_zb_tail_add(aos_zb_tail_t *t, const uint8_t *d, size_t n);
void aos_zb_tail_log(const char *what, const aos_zb_tail_t *t);
