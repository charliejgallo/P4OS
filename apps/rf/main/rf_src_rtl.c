/*
 * RF - the RTL-SDR source: an RTL2832U stick on the USB host, driven by
 * librtlsdr (rtlsdr/, GPL) over the board's raw USB (port/), streaming its
 * bulk endpoint through aos_hal_usb_raw_stream_* (rf.h).
 *
 * Every call here makes USB transfers (a retune is a few dozen of them
 * through the RTL2832's I2C bridge to the tuner): the worker makes them,
 * never LVGL's task.
 */
#include "rf.h"
#include "port/rfusb.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aos_hal.h"
#include "aos_i18n.h"

#define RTL_EP_IN 0x81

typedef struct {
    rf_src_t base;
    rtlsdr_dev_t *dev;
    aos_usb_raw_t *raw;
    uint8_t addr;
    bool streaming;
} rtl_t;

static rf_src_t *rtl_open(const char **why)
{
    if (!rtlsdr_get_device_count()) {
        *why = N_("No hay ninguna RTL-SDR en el host USB");
        return NULL;
    }
    rtl_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    if (rtlsdr_open(&r->dev, 0) < 0 || !r->dev) {
        free(r);
        *why = N_("La RTL-SDR no respondió al abrirla");
        return NULL;
    }
    struct rfusb_device_handle *h = rtlsdr_p4os_usb_handle(r->dev);
    r->raw = rfusb_raw(h);
    r->addr = rfusb_addr(h);
    r->base.ops = &rf_src_rtl;
    return &r->base;
}

static void rtl_info(rf_src_t *s, rf_src_info_t *o)
{
    rtl_t *r = (rtl_t *)s;
    memset(o, 0, sizeof *o);
    o->kind = "RTL-SDR";
    char m[256], p[256];
    if (rtlsdr_get_usb_strings(r->dev, m, p, NULL) == 0)
        snprintf(o->name, sizeof o->name, "%.30s%s%.30s", m, *m && *p ? " " : "", p);
    switch (rtlsdr_get_tuner_type(r->dev)) {
    case RTLSDR_TUNER_E4000:  snprintf(o->tuner, sizeof o->tuner, "E4000");  o->fmin = 52000000;  o->fmax = 2200000000u; break;
    case RTLSDR_TUNER_FC0012: snprintf(o->tuner, sizeof o->tuner, "FC0012"); o->fmin = 22000000;  o->fmax = 948000000;   break;
    case RTLSDR_TUNER_FC0013: snprintf(o->tuner, sizeof o->tuner, "FC0013"); o->fmin = 22000000;  o->fmax = 1100000000;  break;
    case RTLSDR_TUNER_FC2580: snprintf(o->tuner, sizeof o->tuner, "FC2580"); o->fmin = 146000000; o->fmax = 924000000;   break;
    case RTLSDR_TUNER_R820T:  snprintf(o->tuner, sizeof o->tuner, "R820T");  o->fmin = 24000000;  o->fmax = 1766000000;  break;
    case RTLSDR_TUNER_R828D:  snprintf(o->tuner, sizeof o->tuner, "R828D");  o->fmin = 24000000;  o->fmax = 1766000000;  break;
    default:                  snprintf(o->tuner, sizeof o->tuner, "?");      o->fmin = 24000000;  o->fmax = 1700000000;  break;
    }
    int n = rtlsdr_get_tuner_gains(r->dev, NULL);
    if (n > 0 && n <= RF_GAINS_MAX) o->ngains = rtlsdr_get_tuner_gains(r->dev, o->gains);
    /* the port's speed, from the host's list */
    aos_usb_dev_t *d = malloc(12 * sizeof *d);
    if (d) {
        int k = aos_hal_usb_devices(d, 12);
        for (int i = 0; i < k; i++)
            if (d[i].addr == r->addr) o->high_speed = d[i].speed == 2;
        free(d);
    }
}

static bool rtl_set_freq(rf_src_t *s, uint32_t hz)
{
    return rtlsdr_set_center_freq(((rtl_t *)s)->dev, hz) == 0;
}

static uint32_t rtl_set_rate(rf_src_t *s, uint32_t sps)
{
    rtl_t *r = (rtl_t *)s;
    if (rtlsdr_set_sample_rate(r->dev, sps) < 0) return 0;
    return rtlsdr_get_sample_rate(r->dev);
}

static bool rtl_set_gain(rf_src_t *s, int tenth_db)
{
    rtl_t *r = (rtl_t *)s;
    if (tenth_db < 0) return rtlsdr_set_tuner_gain_mode(r->dev, 0) == 0;
    return rtlsdr_set_tuner_gain_mode(r->dev, 1) == 0 && rtlsdr_set_tuner_gain(r->dev, tenth_db) == 0;
}

static bool rtl_start(rf_src_t *s)
{
    rtl_t *r = (rtl_t *)s;
    if (r->streaming) return true;
    rtlsdr_reset_buffer(r->dev);
    /* 16 KB transfers, as librtlsdr's own default; 4 in flight is ~13 ms
     * at 2.4 Msps, and the 2 MB ring ~0.4 s of the worker being late */
    r->streaming = aos_hal_usb_raw_stream_start(r->raw, RTL_EP_IN, 16384, 4, 2 * 1024 * 1024);
    return r->streaming;
}

static int rtl_read(rf_src_t *s, uint8_t *iq, int bytes, int timeout_ms)
{
    rtl_t *r = (rtl_t *)s;
    int n = aos_hal_usb_raw_stream_read(r->raw, iq, bytes & ~1, timeout_ms);
    return n == AOS_USB_RAW_GONE ? -1 : n < 0 ? 0 : n;
}

static void rtl_stats(rf_src_t *s, uint64_t *bytes, uint32_t *dropped)
{
    aos_hal_usb_raw_stream_stats(((rtl_t *)s)->raw, bytes, dropped, NULL);
}

static void rtl_stop(rf_src_t *s)
{
    rtl_t *r = (rtl_t *)s;
    if (!r->streaming) return;
    aos_hal_usb_raw_stream_stop(r->raw);
    r->streaming = false;
}

static void rtl_close(rf_src_t *s)
{
    rtl_t *r = (rtl_t *)s;
    rtl_stop(s);
    rtlsdr_close(r->dev);           /* closes the raw device too (port/rfusb.c) */
    free(r);
}

const rf_src_ops_t rf_src_rtl = {
    .kind = "RTL-SDR",
    .open = rtl_open,
    .info = rtl_info,
    .set_freq = rtl_set_freq,
    .set_rate = rtl_set_rate,
    .set_gain = rtl_set_gain,
    .start = rtl_start,
    .read = rtl_read,
    .stats = rtl_stats,
    .stop = rtl_stop,
    .close = rtl_close,
};
