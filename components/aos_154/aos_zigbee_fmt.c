/*
 * P4OS - what a Zigbee device's measured values mean (aos_zigbee.h), for
 * the screen and the portal alike, on the board and in the simulator.
 *
 * A table of the standard ZCL attributes the board names, from the ZCL
 * specification: which ones are read at setup, which are only a multiplier
 * or a divisor for another, and how each raw value becomes text with its
 * unit. Anything not here is still shown, raw (aos_zigbee_dev_t.attr).
 */
#include "aos_zigbee.h"
#include "aos_i18n.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t    cl, at;
    const char *name;               /* N_(), drawn through aos_tr */
    uint8_t     kind;
} row_t;

enum {
    K_HIDDEN, K_LUX, K_HPA, K_PPM_FRAC, K_UGM3, K_PCT100, K_FLOW, K_KWH, K_DEMAND, K_VOLT, K_AMP, K_WATT, K_COVER,
    K_C100, K_SETPOINT, K_THMODE, K_MIREDS, K_LOCK, K_FAN, K_FLOAT, K_MULTI
};

static const row_t ROWS[] = {
    { 0x0400, 0x0000, N_("luz"), K_LUX },                 /* Illuminance: 10000 log10(lux) + 1 */
    { 0x0403, 0x0000, N_("presión"), K_HPA },             /* Pressure: 0.1 kPa = 1 hPa */
    { 0x040D, 0x0000, "CO2", K_PPM_FRAC },                /* CO2: a fraction (float) */
    { 0x042A, 0x0000, "PM2.5", K_UGM3 },                  /* PM2.5: µg/m³ (float) */
    { 0x0408, 0x0000, N_("humedad del suelo"), K_PCT100 },/* soil moisture: 0.01 % */
    { 0x0404, 0x0000, N_("caudal"), K_FLOW },             /* flow: 0.1 m³/h */
    { 0x0702, 0x0000, N_("energía"), K_KWH },             /* Metering: summation, × mul ÷ div kWh */
    { 0x0702, 0x0400, N_("consumo"), K_DEMAND },          /* instantaneous demand, × mul ÷ div kW */
    { 0x0702, 0x0301, NULL, K_HIDDEN },                   /* its multiplier */
    { 0x0702, 0x0302, NULL, K_HIDDEN },                   /* its divisor */
    { 0x0B04, 0x0505, N_("tensión"), K_VOLT },            /* Electrical Measurement: RMS voltage */
    { 0x0B04, 0x0508, N_("corriente"), K_AMP },           /* RMS current */
    { 0x0B04, 0x050B, N_("potencia"), K_WATT },           /* active power */
    { 0x0B04, 0x0600, NULL, K_HIDDEN }, { 0x0B04, 0x0601, NULL, K_HIDDEN },     /* voltage × ÷ */
    { 0x0B04, 0x0602, NULL, K_HIDDEN }, { 0x0B04, 0x0603, NULL, K_HIDDEN },     /* current × ÷ */
    { 0x0B04, 0x0604, NULL, K_HIDDEN }, { 0x0B04, 0x0605, NULL, K_HIDDEN },     /* power × ÷ */
    { 0x0102, 0x0008, N_("cortina"), K_COVER },           /* Window Covering: lift %, 0 open */
    { 0x0201, 0x0000, N_("ambiente"), K_C100 },           /* Thermostat: local temperature */
    { 0x0201, 0x0012, N_("consigna"), K_SETPOINT },       /* occupied heating setpoint */
    { 0x0201, 0x001C, N_("modo"), K_THMODE },             /* system mode */
    { 0x0300, 0x0007, N_("tono"), K_MIREDS },             /* Color Control: color temperature */
    { 0x0101, 0x0000, N_("cerradura"), K_LOCK },          /* Door Lock: lock state */
    { 0x0202, 0x0000, N_("ventilador"), K_FAN },          /* Fan Control: fan mode */
    { 0x000C, 0x0055, N_("valor"), K_FLOAT },             /* Analog Input: present value */
    { 0x0012, 0x0055, N_("estado"), K_MULTI },            /* Multistate Input: present value */
};

static const row_t *row(uint16_t cl, uint16_t at)
{
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0]; i++)
        if (ROWS[i].cl == cl && ROWS[i].at == at) return &ROWS[i];
    return NULL;
}

bool aos_zigbee_meas_known(uint16_t cluster, uint16_t attr) { return row(cluster, attr) != NULL; }

bool aos_zigbee_meas_hidden(uint16_t cluster, uint16_t attr)
{
    const row_t *r = row(cluster, attr);
    return r && r->kind == K_HIDDEN;
}

int aos_zigbee_meas_attrs(uint16_t cluster, uint16_t *out, int max)
{
    int n = 0;
    for (size_t i = 0; i < sizeof ROWS / sizeof ROWS[0] && n < max; i++)
        if (ROWS[i].cl == cluster) out[n++] = ROWS[i].at;
    return n;
}

/* another value of the same device and endpoint, or def */
static int32_t other(const aos_zigbee_dev_t *d, uint8_t ep, uint16_t cl, uint16_t at, int32_t def)
{
    for (int i = 0; i < d->nmeas; i++)
        if (d->meas[i].cluster == cl && d->meas[i].attr == at && d->meas[i].ep == ep) return d->meas[i].v;
    return def;
}

static float as_float(int32_t v)
{
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

/* × mul ÷ div, with 1 for one not given (or given as 0) */
static double scaled(const aos_zigbee_dev_t *d, const aos_zigbee_meas_t *m, uint16_t mul_at, uint16_t div_at, double def_div)
{
    int32_t mul = other(d, m->ep, m->cluster, mul_at, 1), div = other(d, m->ep, m->cluster, div_at, 0);
    if (mul == 0) mul = 1;
    return (double)m->v * mul / (div ? (double)div : def_div);
}

bool aos_zigbee_meas_text(const aos_zigbee_dev_t *d, const aos_zigbee_meas_t *m, const char **name, char *val, size_t n)
{
    const row_t *r = row(m->cluster, m->attr);
    if (!r || r->kind == K_HIDDEN) return false;
    *name = r->name ? aos_tr(r->name) : "";
    int32_t v = m->v;
    switch (r->kind) {
    case K_LUX:
        snprintf(val, n, "%.0f lx", v <= 0 ? 0.0 : pow(10.0, (v - 1) / 10000.0));
        break;
    case K_HPA: snprintf(val, n, "%d hPa", (int)(int16_t)v); break;
    case K_PPM_FRAC: snprintf(val, n, "%.0f ppm", as_float(v) * 1e6); break;
    case K_UGM3: snprintf(val, n, "%.0f µg/m³", as_float(v)); break;
    case K_PCT100: snprintf(val, n, "%.1f %%", v / 100.0); break;
    case K_FLOW: snprintf(val, n, "%.1f m³/h", v / 10.0); break;
    case K_KWH: snprintf(val, n, "%.2f kWh", scaled(d, m, 0x0301, 0x0302, 1.0)); break;
    case K_DEMAND: snprintf(val, n, "%.0f W", scaled(d, m, 0x0301, 0x0302, 1.0) * 1000.0); break;
    case K_VOLT: snprintf(val, n, "%.1f V", scaled(d, m, 0x0600, 0x0601, 1.0)); break;
    case K_AMP: snprintf(val, n, "%.2f A", scaled(d, m, 0x0602, 0x0603, 1000.0)); break;    /* most: mA */
    case K_WATT: snprintf(val, n, "%.1f W", scaled(d, m, 0x0604, 0x0605, 1.0)); break;
    case K_COVER: snprintf(val, n, _("%d %% cerrada"), (int)v); break;
    case K_C100:
    case K_SETPOINT: snprintf(val, n, "%.1f °C", (int16_t)v / 100.0); break;
    case K_THMODE: {
        static const char *const M[] = { N_("apagado"), N_("automático"), "", N_("frío"), N_("calor") };
        snprintf(val, n, "%s", v >= 0 && v <= 4 && M[v][0] ? aos_tr(M[v]) : "?");
        break;
    }
    case K_MIREDS: snprintf(val, n, "%d K", v > 0 ? (int)(1000000 / v) : 0); break;
    case K_LOCK: snprintf(val, n, "%s", v == 1 ? _("con llave") : v == 2 ? _("abierta") : _("a medias")); break;
    case K_FAN: {
        static const char *const F[] = { N_("apagado"), N_("baja"), N_("media"), N_("alta"), N_("prendido"),
                                         N_("automático"), N_("inteligente") };
        snprintf(val, n, "%s", v >= 0 && v <= 6 ? aos_tr(F[v]) : "?");
        break;
    }
    case K_FLOAT: snprintf(val, n, "%.2f", as_float(v)); break;
    default: snprintf(val, n, "%d", (int)v); break;
    }
    return true;
}
