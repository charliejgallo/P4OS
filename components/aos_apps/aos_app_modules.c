/*
 * P4OS - Módulos: what is connected to the header, and what it reads.
 *
 *   Header    the 40 pins drawn as the connector is (two columns, pin 1
 *             square, 5 V at the top left), coloured by what each one is:
 *             power, free, ADC, in a port, taken (with its owner's name),
 *             the board's, strapping/VO4/JTAG, reserved. A tap opens a
 *             sheet with the GPIO, its flags, the note, the owner and the
 *             port and modules it belongs to.
 *   Puertos   the ports and modules of modules.txt, an editor for the file
 *             that checks it before saving (aos_modules.c), and a form
 *             that adds the common modules without typing.
 *   Sensores  the sensor service (aos_sensors.c): every sensor declared or
 *             found on the I2C ports, its readings live with their units,
 *             a chart of the one picked, and the addresses where something
 *             answered that could be a sensor ("¿es un BH1750?").
 *
 * The Bus app is the raw tool (a scan of all 112 addresses, registers,
 * GPIO levels); this one is about WHAT is there. Neither holds a port for
 * long, so both can be open.
 */
#include "aos_apps.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_hal.h"
#include "aos_ui.h"
#include "aos_io.h"
#include "aos_modules.h"
#include "aos_sensors.h"
#include "aos_sys_glyphs.h"
#include "aos_mono.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* strncpy that always terminates, and that GCC's format-truncation check
 * leaves alone (it flags every snprintf of a longer buffer into a shorter) */
static void scpy(char *d, size_t n, const char *s)
{
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

#define C_MOD       lv_color_hex(0x94A3B8)
#define C_MOD_DARK  lv_color_hex(0x475569)
#define C_BAD       lv_color_hex(0xFF6B60)
#define C_GOOD      lv_color_hex(0x30D158)
#define C_WARN      lv_color_hex(0xFFB340)

enum { TAB_HDR, TAB_PORTS, TAB_SENS, TAB_COUNT };
static const char *const TAB_NAME[TAB_COUNT] = { N_("Header"), N_("Puertos"), N_("Sensores") };
static const char *const TAB_GLYPH[TAB_COUNT] = { AOS_SYM_DEVELOPER_BOARD, AOS_SYM_EXPANSION_CARD, AOS_SYM_THERMOMETER };

/* What survives closing the app and turning the screen */
static struct {
    int  tab;
    char sel_name[24], sel_key[12];     /* the reading on the chart */
} S = { .tab = TAB_SENS };

static struct {
    lv_obj_t *root, *content, *tabs[TAB_COUNT], *overlay, *scroller;
    lv_timer_t *timer;
    int32_t W, H;
    bool land;
    /* Sensores */
    uint32_t seen_seq;
    char sig[320];
    lv_obj_t *status, *chart, *ch_title, *ch_value, *ch_range;
    lv_chart_series_t *ser;
    lv_obj_t *tile[AOS_SENSOR_MAX][AOS_SENSOR_VALUES], *tval[AOS_SENSOR_MAX][AOS_SENSOR_VALUES];
    lv_obj_t *sstate[AOS_SENSOR_MAX];
    /* the editor */
    lv_obj_t *ta, *kb, *ed_status, *ed_save;
    bool ed_ok;
    /* the form */
    lv_obj_t *f_preview;
    int sel_i, sel_k;               /* the reading on the chart, as found this time */
} U;

/* The form's choices: a sensor chip (0 .. chips-1) or RS485 */
static struct { int kind, port, addr, opt; } F;
#define F_RS485 aos_sensor_chip_count()

static void build_page(void);

/* -------------------------------------------------------------------------- */
/* Small pieces                                                                */
/* -------------------------------------------------------------------------- */

static lv_obj_t *box(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *card(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = box(parent, w, h);
    lv_obj_set_style_bg_color(c, AOS_C_CARD, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(c, AOS_UI_RADIUS, 0);
    return c;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = box(parent, LV_SIZE_CONTENT, 72);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b, 36, 0);
    lv_obj_set_style_pad_hor(b, 26, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 12, 0);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    if (glyph) aos_make_decorative(aos_label(b, glyph, &aos_sym_28, lv_color_white()));
    aos_make_decorative(aos_label(b, text, aos_font_body, lv_color_white()));
    return b;
}

static lv_obj_t *chip(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *c = box(parent, LV_SIZE_CONTENT, 60);
    lv_obj_set_style_radius(c, 30, 0);
    lv_obj_set_style_pad_hor(c, 20, 0);
    lv_obj_set_style_bg_color(c, on ? C_MOD : AOS_C_CARD2, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_60, LV_STATE_PRESSED);
    if (cb) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_t *l = aos_label(c, text, aos_font_small, on ? lv_color_hex(0x0F172A) : AOS_C_TEXT);
    lv_obj_center(l);
    aos_make_decorative(l);
    return c;
}

static lv_obj_t *chips_row(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *r = box(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(r, 10, 0);
    return r;
}

static lv_obj_t *caption(lv_obj_t *parent, const char *text, int32_t w)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *section(lv_obj_t *parent, const char *text)
{
    lv_obj_t *t = aos_label(parent, text, aos_font_caption, AOS_C_DIM);
    lv_obj_set_style_pad_left(t, 10, 0);
    lv_obj_set_style_pad_top(t, 6, 0);
    return t;
}

static lv_obj_t *column(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 16, 0);
    lv_obj_set_style_pad_bottom(c, 24, 0);
    lv_obj_set_scroll_dir(c, LV_DIR_VER);
    return c;
}

static void comma(char *s) { for (; *s; s++) if (*s == '.') *s = ','; }

static void overlay_close(void)
{
    if (U.overlay) lv_obj_delete(U.overlay);
    U.overlay = U.ta = U.kb = U.ed_status = U.ed_save = U.f_preview = NULL;
}

/* A dimmed full-screen layer with a sheet in the middle; a tap outside closes it. */
static void dim_cb(lv_event_t *e) { if (lv_event_get_target(e) == U.overlay) overlay_close(); }

static lv_obj_t *sheet_open(int32_t w)
{
    overlay_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_70, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(U.overlay, dim_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *sh = card(U.overlay, w, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(sh, U.H - 40, 0);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(sh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(sh, LV_DIR_VER);
    lv_obj_set_style_pad_all(sh, 28, 0);
    lv_obj_set_flex_flow(sh, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(sh, 14, 0);
    lv_obj_center(sh);
    return sh;
}

/* -------------------------------------------------------------------------- */
/* Words                                                                       */
/* -------------------------------------------------------------------------- */

/* The header's notes, in Spanish (aos_io.c has them in English) */
static const char *pin_note(int pin)
{
    switch (pin) {
    case 1:  return _("VCC_5V: vivo aun con la placa apagada");
    case 2:  return _("Buck de 3 A: se apaga con POWER");
    case 4:  return _("SDA del bus I2C de la placa (táctil, códecs)");
    case 6:  return _("SCL del bus I2C de la placa (táctil, códecs)");
    case 7:  return _("UART0 TX al CH343: la consola");
    case 8:  return _("La INT del táctil, si R108 está puesta");
    case 9:  return _("UART0 RX desde el CH343: la consola");
    case 11: return _("JTAG MTDO");
    case 12: return _("JTAG MTCK");
    case 14: return _("JTAG MTDI");
    case 15: return _("ADC1 canal 5");
    case 17: return _("ADC1 canal 6");
    case 21: return _("USB-Serial-JTAG D−: libre si no se usa");
    case 23: return _("USB-Serial-JTAG D+: libre si no se usa");
    case 25: case 27: return _("USB 2.0 HS, en paralelo con el puerto OTG: no es un GPIO");
    case 28: return _("Strapping de la fuente del JTAG: usable después del arranque");
    case 30: return _("BOOT: strapping, con pull-up de 4,7K");
    case 32: return _("ADC2 canal 0");
    case 34: return _("ADC2 canal 1");
    case 35: return _("Dominio VO4: medí la tensión antes de usarlo");
    case 36: return _("ADC2 canal 2, comparador");
    case 37: case 39: return _("Dominio VO4");
    case 38: return _("ADC2 canal 3, comparador");
    default: return "";
    }
}

static const char *chip_what(const char *id)
{
    if (!strcmp(id, "bme280")) return _("Temperatura, presión y humedad");
    if (!strcmp(id, "bmp280")) return _("Temperatura y presión");
    if (!strcmp(id, "sht3x") || !strcmp(id, "sht4x") || !strcmp(id, "aht20")) return _("Temperatura y humedad");
    if (!strcmp(id, "bh1750")) return _("Luz, en lux");
    if (!strcmp(id, "ina219")) return _("Tensión, corriente y potencia, hasta 26 V");
    if (!strcmp(id, "ina226")) return _("Tensión, corriente y potencia, hasta 36 V");
    if (!strcmp(id, "ads1115")) return _("ADC de 16 bits, 4 entradas");
    return "";
}

static const char *key_name(const char *key)
{
    if (!strcmp(key, "temp")) return _("Temperatura");
    if (!strcmp(key, "hum")) return _("Humedad");
    if (!strcmp(key, "press")) return _("Presión");
    if (!strcmp(key, "lux")) return _("Luz");
    if (!strcmp(key, "bus")) return _("Tensión");
    if (!strcmp(key, "current")) return _("Corriente");
    if (!strcmp(key, "power")) return _("Potencia");
    if (!strcmp(key, "shunt")) return _("En el shunt");
    if (key[0] == 'a' && key[1] >= '0' && key[1] <= '3' && !key[2]) {
        static char an[4][4];
        int i = key[1] - '0';
        snprintf(an[i], sizeof an[i], "A%d", i);
        return an[i];
    }
    return key;
}

static const char *q_glyph(int q, const char *key)
{
    switch (q) {
    case AOS_SQ_TEMP: return AOS_SYM_THERMOMETER;
    case AOS_SQ_HUM: return AOS_SYM_WATER_PERCENT;
    case AOS_SQ_PRESS: return AOS_SYM_GAUGE;
    case AOS_SQ_LUX: return AOS_SYM_BRIGHTNESS_6;
    case AOS_SQ_CURR: return AOS_SYM_SINE_WAVE;
    case AOS_SQ_POWER: return AOS_SYM_POWER_PLUG;
    default: return key && key[0] == 'a' ? AOS_SYM_WAVEFORM : AOS_SYM_LIGHTNING_BOLT;
    }
}

/* A value with its unit, scaled the way a bench wants it, decimal comma */
static void fmt_value(int q, float v, char *out, size_t n)
{
    if (isnan(v)) { snprintf(out, n, "--"); return; }
    float a = fabsf(v);
    switch (q) {
    case AOS_SQ_TEMP: snprintf(out, n, "%.1f °C", v); break;
    case AOS_SQ_HUM: snprintf(out, n, "%.1f %%", v); break;
    case AOS_SQ_PRESS: snprintf(out, n, "%.1f hPa", v); break;
    case AOS_SQ_LUX: snprintf(out, n, a < 100 ? "%.1f lx" : "%.0f lx", v); break;
    case AOS_SQ_VOLT: if (a < 1) snprintf(out, n, "%.1f mV", v * 1000); else snprintf(out, n, "%.3f V", v); break;
    case AOS_SQ_CURR: if (a < 1) snprintf(out, n, "%.1f mA", v * 1000); else snprintf(out, n, "%.3f A", v); break;
    case AOS_SQ_POWER: if (a < 1) snprintf(out, n, "%.0f mW", v * 1000); else snprintf(out, n, "%.2f W", v); break;
    default: snprintf(out, n, "%g", v); break;
    }
    comma(out);
}

/* The service's "why:detail" in words */
static void fmt_err(const char *err, char *out, size_t n)
{
    const char *d = strchr(err, ':');
    d = d ? d + 1 : "";
    if (!strncmp(err, "nack", 4)) scpy(out, n, _("No contesta en esa dirección"));
    else if (!strncmp(err, "crc", 3)) scpy(out, n, _("Llegó una lectura con el CRC mal"));
    else if (!strncmp(err, "id", 2)) snprintf(out, n, _("En esa dirección hay otro chip (dice %s)"), d);
    else if (!strncmp(err, "notready", 8)) scpy(out, n, _("Todavía no midió"));
    else if (!strncmp(err, "busy", 4)) snprintf(out, n, _("Los pines los tiene %s"), d);
    else if (!strncmp(err, "port", 4)) scpy(out, n, _("No se pudo abrir el puerto"));
    else if (!strncmp(err, "board", 5)) snprintf(out, n, _("Ahí está el %s de la placa"), d);
    else scpy(out, n, err);
}

static void fmt_msg(const aos_mod_msg_t *m, char *out, size_t n)
{
    char t[128];
    switch (m->code) {
    case AOS_MOD_E_LONG: scpy(t, sizeof t, _("la línea es demasiado larga")); break;
    case AOS_MOD_E_SYNTAX: scpy(t, sizeof t, _("se esperaba «port» o «module», un nombre y lo demás")); break;
    case AOS_MOD_E_TOO_MANY_PORTS: snprintf(t, sizeof t, _("demasiados puertos (%d como mucho)"), AOS_IO_PORT_MAX); break;
    case AOS_MOD_E_PORT_NAME: scpy(t, sizeof t, _("el nombre del puerto es demasiado largo")); break;
    case AOS_MOD_E_PORT_KIND: scpy(t, sizeof t, _("los puertos se llaman uart…, i2c…, spi… o gpio…")); break;
    case AOS_MOD_E_NOT_HEADER: scpy(t, sizeof t, _("un GPIO que no está en el header")); break;
    case AOS_MOD_E_RESERVED: scpy(t, sizeof t, _("un pin reservado (la consola o BOOT)")); break;
    case AOS_MOD_E_MISSING_PIN: scpy(t, sizeof t, _("le falta un pin (tx/rx, sda/scl, sck/mosi o pin)")); break;
    case AOS_MOD_E_DUP_PORT: snprintf(t, sizeof t, _("hay dos puertos que se llaman %s"), m->a); break;
    case AOS_MOD_E_TOO_MANY_MODULES: snprintf(t, sizeof t, _("demasiados módulos (%d como mucho)"), AOS_IO_MODULE_MAX); break;
    case AOS_MOD_E_MODULE_PORT: scpy(t, sizeof t, _("al módulo le falta el puerto")); break;
    case AOS_MOD_E_UNKNOWN: scpy(t, sizeof t, _("no es ni «port» ni «module»")); break;
    case AOS_MOD_W_PIN_TWICE: snprintf(t, sizeof t, _("GPIO%d está en %s y en %s"), m->gpio, m->a, m->b); break;
    case AOS_MOD_W_NO_PORT: snprintf(t, sizeof t, _("%s está en %s, que no está declarado"), m->a, m->b); break;
    case AOS_MOD_W_NOT_I2C: snprintf(t, sizeof t, _("%s es un sensor I2C y %s no es un puerto I2C"), m->a, m->b); break;
    default: scpy(t, sizeof t, m->raw); break;
    }
    if (m->line > 0) snprintf(out, n, _("Línea %d: %s"), m->line, t);
    else scpy(out, n, t);
}

/* -------------------------------------------------------------------------- */
/* The header                                                                  */
/* -------------------------------------------------------------------------- */

enum { K_5V, K_3V3, K_GND, K_FREE, K_ADC, K_PORT, K_TAKEN, K_BOARD, K_CARE, K_RSV, K_N };
static const uint32_t K_COL[K_N] = { 0xDC2626, 0xF97316, 0x3F3F46, 0x16A34A, 0x0D9488, 0x2563EB,
                                     0x9333EA, 0xDB2777, 0xA16207, 0x52525B };
static const char *const K_NAME[K_N] = { "5 V", "3V3", "GND", N_("libre"), "ADC", N_("en un puerto"),
                                         N_("tomado"), N_("de la placa"), N_("con cuidado"), N_("reservado") };

/* Which port a GPIO belongs to, and as what ("i2c.ext", "SDA") */
static const aos_io_port_t *port_of(int gpio, const char **role)
{
    static const char *const ROLE[4][4] = {
        [AOS_PORT_UART] = { "TX", "RX", "DE", "" }, [AOS_PORT_I2C] = { "SDA", "SCL", "", "" },
        [AOS_PORT_SPI] = { "SCK", "MOSI", "MISO", "CS" }, [AOS_PORT_GPIO] = { "", "", "", "" },
    };
    for (int i = 0; gpio >= 0 && i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        for (int k = 0; k < 4; k++)
            if (pt->pins[k] == gpio) {
                if (role) *role = pt->kind <= AOS_PORT_GPIO ? ROLE[pt->kind][k] : "";
                return pt;
            }
    }
    return NULL;
}

static int pin_class(const aos_io_pin_t *p)
{
    if (p->gpio < 0) return p->label[0] == '5' ? K_5V : p->label[0] == '3' ? K_3V3 : p->label[0] == 'G' ? K_GND : K_RSV;
    if (aos_io_owner(p->gpio)) return K_TAKEN;
    if (p->flags & AOS_PIN_RESERVED) return K_RSV;
    if (p->flags & AOS_PIN_BOARD) return K_BOARD;
    if (port_of(p->gpio, NULL)) return K_PORT;
    if (p->flags & (AOS_PIN_STRAPPING | AOS_PIN_VO4 | AOS_PIN_USB_JTAG)) return K_CARE;
    if (p->flags & AOS_PIN_ADC) return K_ADC;
    return K_FREE;
}

/* What a pin says under its name in the drawing */
static void pin_sub(const aos_io_pin_t *p, char *out, size_t n)
{
    const char *o = p->gpio >= 0 ? aos_io_owner(p->gpio) : NULL, *role = "";
    const aos_io_port_t *pt = p->gpio >= 0 ? port_of(p->gpio, &role) : NULL;
    if (o) scpy(out, n, o);
    else if (pt) snprintf(out, n, "%s %s", pt->name, role);
    else if (p->gpio < 0) scpy(out, n, !strncmp(p->label, "USB", 3) ? _("USB 2.0, no es un GPIO") : "");
    else if (p->gpio == 37 || p->gpio == 38) scpy(out, n, _("la consola"));
    else if (p->gpio == 35) snprintf(out, n, "BOOT");
    else if (p->flags & AOS_PIN_ADC) scpy(out, n, pin_note(p->pin));
    else if (p->flags & AOS_PIN_USB_JTAG) snprintf(out, n, "USB-JTAG");
    else if (p->flags & AOS_PIN_VO4) scpy(out, n, _("dominio VO4"));
    else if (p->flags & AOS_PIN_STRAPPING) scpy(out, n, _("strapping"));
    else if (p->flags & AOS_PIN_BOARD) scpy(out, n, _("bus de la placa"));
    else out[0] = 0;
}

static void pad_text(const aos_io_pin_t *p, char *out, size_t n)
{
    if (p->gpio >= 0) snprintf(out, n, "%d", p->gpio);
    else if (!strncmp(p->label, "USB D", 5)) snprintf(out, n, "D%c", p->label[5]);
    else scpy(out, n, p->label);
}

static void go_ports_cb(lv_event_t *e) { overlay_close(); S.tab = TAB_PORTS; build_page(); }
static void close_cb(lv_event_t *e) { overlay_close(); }

static void kv(lv_obj_t *sh, int32_t w, const char *k, const char *v)
{
    lv_obj_t *r = box(sh, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(r, 2, 0);
    aos_label(r, k, aos_font_caption, AOS_C_DIM);
    lv_obj_t *l = aos_label(r, v, aos_font_body, AOS_C_TEXT);
    lv_obj_set_width(l, w);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
}

static void pin_sheet(int idx)
{
    const aos_io_pin_t *p = &aos_io_header()[idx];
    int32_t w = U.W - 48 < 640 ? U.W - 48 : 640, iw = w - 56;
    lv_obj_t *sh = sheet_open(w);
    lv_obj_t *t = aos_label(sh, "", aos_font_title, AOS_C_TEXT);
    lv_label_set_text_fmt(t, _("Pin %d · %s"), p->pin, p->label);
    int k = pin_class(p);
    lv_obj_t *cr = chips_row(sh, iw);
    lv_obj_t *c = chip(cr, aos_tr(K_NAME[k]), false, NULL, NULL);
    lv_obj_set_style_bg_color(c, lv_color_hex(K_COL[k]), 0);
    if (p->flags & AOS_PIN_ADC) chip(cr, "ADC", false, NULL, NULL);
    if (p->flags & AOS_PIN_STRAPPING) chip(cr, _("strapping"), false, NULL, NULL);
    if (p->flags & AOS_PIN_VO4) chip(cr, _("dominio VO4"), false, NULL, NULL);
    if (p->flags & AOS_PIN_USB_JTAG) chip(cr, "USB-JTAG", false, NULL, NULL);
    if (p->flags & AOS_PIN_BOARD) chip(cr, _("compartido con la placa"), false, NULL, NULL);
    if ((p->flags & AOS_PIN_RESERVED) && p->gpio >= 0) chip(cr, _("reservado"), false, NULL, NULL);

    char v[96];
    if (p->gpio >= 0) snprintf(v, sizeof v, "GPIO%d", p->gpio);
    else scpy(v, sizeof v, _("No es un GPIO"));
    kv(sh, iw, "GPIO", v);
    if (pin_note(p->pin)[0]) kv(sh, iw, _("Qué es"), pin_note(p->pin));
    if (p->gpio >= 0) {
        const char *o = aos_io_owner(p->gpio), *role = "";
        kv(sh, iw, _("Lo tiene"), o ? o : _("Nadie, ahora"));
        const aos_io_port_t *pt = port_of(p->gpio, &role);
        if (pt) {
            snprintf(v, sizeof v, "%s · %s", pt->name, role);
            kv(sh, iw, _("Puerto"), v);
            char mods[96] = "";
            for (int i = 0; i < aos_io_module_count(); i++) {
                const aos_io_module_t *m = aos_io_module_at(i);
                if (strcmp(m->port, pt->name)) continue;
                size_t o2 = strlen(mods);
                snprintf(mods + o2, sizeof mods - o2, "%s%s", o2 ? ", " : "", m->name);
            }
            kv(sh, iw, _("Módulos en ese puerto"), mods[0] ? mods : _("Ninguno"));
        } else {
            kv(sh, iw, _("Puerto"), _("Ninguno en modules.txt"));
        }
    }
    lv_obj_t *br = box(sh, iw, 84);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(br, 12, 0);
    if (p->gpio >= 0 && port_of(p->gpio, NULL)) pill(br, NULL, _("Ver el puerto"), AOS_C_CARD2, go_ports_cb, NULL);
    pill(br, NULL, _("Cerrar"), C_MOD_DARK, close_cb, NULL);
}

static void pin_cb(lv_event_t *e) { pin_sheet((int)(intptr_t)lv_event_get_user_data(e)); }

static lv_obj_t *pad(lv_obj_t *parent, const aos_io_pin_t *p, int32_t x, int32_t y, int32_t d)
{
    int k = pin_class(p);
    lv_obj_t *o = box(parent, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, p->pin == 1 ? 6 : LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(K_COL[k]), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 3, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0xC9A227), 0);     /* the gold of the pad */
    lv_obj_set_style_border_opa(o, p->pin == 1 ? LV_OPA_COVER : LV_OPA_50, 0);
    char t[8];
    pad_text(p, t, sizeof t);
    lv_obj_t *l = aos_label(o, t, aos_font_tiny, lv_color_white());
    lv_obj_center(l);
    aos_make_decorative(o);
    return o;
}

static void legend(lv_obj_t *parent, int32_t w)
{
    int count[K_N] = { 0 };
    for (int i = 0; i < 40; i++) count[pin_class(&aos_io_header()[i])]++;
    lv_obj_t *r = chips_row(parent, w);
    lv_obj_set_style_pad_gap(r, 16, 0);
    for (int k = 0; k < K_N; k++) {
        if (!count[k]) continue;
        lv_obj_t *c = box(r, LV_SIZE_CONTENT, 34);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(c, 8, 0);
        lv_obj_t *sq = box(c, 22, 22);
        lv_obj_set_style_radius(sq, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sq, lv_color_hex(K_COL[k]), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_t *l = aos_label(c, "", aos_font_caption, AOS_C_DIM);
        lv_label_set_text_fmt(l, "%s %d", aos_tr(K_NAME[k]), count[k]);
    }
}

/* Portrait: the connector upright as on the board, the odd pins (5 V's
 * column) on the right, labels outside */
static void header_upright(lv_obj_t *parent, int32_t w)
{
    const aos_io_pin_t *hdr = aos_io_header();
    const int32_t pitch = 50, d = 40, g = 10, bw = 2 * d + 3 * g, top = 14;
    lv_obj_t *cd = card(parent, w, 20 * pitch + 2 * top + 8);
    const int32_t bx = (w - bw) / 2;
    lv_obj_t *body = box(cd, bw, 20 * pitch + 8);
    lv_obj_set_pos(body, bx, top);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0B0B0D), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 10, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x3A3A3C), 0);
    aos_make_decorative(body);
    const int32_t lw = bx - 24;
    for (int i = 0; i < 40; i++) {
        const aos_io_pin_t *p = &hdr[i];
        int r = i / 2, side = !(i % 2);       /* pin 1 (index 0) on the right */
        int32_t y = top + 4 + r * pitch;
        /* the whole half row is the target */
        lv_obj_t *hit = box(cd, w / 2, pitch);
        lv_obj_set_pos(hit, side ? w / 2 : 0, y);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hit, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(hit, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(hit, pin_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        pad(cd, p, bx + g + side * (d + g), y + (pitch - d) / 2, d);
        char sub[48];
        pin_sub(p, sub, sizeof sub);
        int k = pin_class(p);
        lv_obj_t *nm = aos_label(cd, p->label, aos_font_small, k == K_GND ? AOS_C_DIM : AOS_C_TEXT);
        lv_obj_set_width(nm, lw);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_set_style_text_align(nm, side ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(nm, side ? bx + bw + 14 : 10, y + (sub[0] ? 1 : 10));
        aos_make_decorative(nm);
        lv_obj_t *pn = aos_label(cd, "", aos_font_tiny, AOS_C_DIM);
        lv_label_set_text_fmt(pn, "%d", p->pin);
        lv_obj_set_width(pn, 34);
        lv_obj_set_style_text_align(pn, side ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_pos(pn, side ? w - 44 : 10, y + 14);
        aos_make_decorative(pn);
        if (sub[0]) {
            lv_obj_t *sl = aos_label(cd, sub, aos_font_tiny, k == K_TAKEN ? lv_color_hex(0xD8B4FE) : k == K_PORT ? lv_color_hex(0x93C5FD) : AOS_C_DIM);
            lv_obj_set_width(sl, lw - 44);
            lv_label_set_long_mode(sl, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_style_text_align(sl, side ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_pos(sl, side ? bx + bw + 14 : 54, y + 27);
            aos_make_decorative(sl);
        }
    }
}

/* Landscape: the connector lying down, pin 1 bottom left, even pins on top */
static void header_flat(lv_obj_t *parent, int32_t w)
{
    const aos_io_pin_t *hdr = aos_io_header();
    const int32_t pitch = (w - 40) / 20, d = pitch - 14, lab = 26;
    const int32_t bw = 20 * pitch + 8, bh = 2 * pitch + 8, bx = (w - bw) / 2, by = lab + 12;
    lv_obj_t *cd = card(parent, w, bh + 2 * lab + 30);
    lv_obj_t *body = box(cd, bw, bh);
    lv_obj_set_pos(body, bx, by);
    lv_obj_set_style_bg_color(body, lv_color_hex(0x0B0B0D), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(body, 10, 0);
    lv_obj_set_style_border_width(body, 2, 0);
    lv_obj_set_style_border_color(body, lv_color_hex(0x3A3A3C), 0);
    aos_make_decorative(body);
    for (int i = 0; i < 40; i++) {
        const aos_io_pin_t *p = &hdr[i];
        int col = i / 2, top = !(i % 2);       /* odd pins (index 0, 2...) on top: the upright drawing turned left */
        int32_t x = bx + 4 + col * pitch, y = by + 4 + (top ? 0 : pitch);
        lv_obj_t *hit = box(cd, pitch, pitch + lab);
        lv_obj_set_pos(hit, x, top ? y - lab : y);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(hit, AOS_C_CARD2, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(hit, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_set_style_radius(hit, 10, 0);
        lv_obj_add_event_cb(hit, pin_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        pad(cd, p, x + (pitch - d) / 2, y + (pitch - d) / 2, d);
        lv_obj_t *pn = aos_label(cd, "", aos_font_tiny, AOS_C_DIM);
        lv_label_set_text_fmt(pn, "%d", p->pin);
        lv_obj_set_width(pn, pitch);
        lv_obj_set_style_text_align(pn, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(pn, x, top ? by - lab + 2 : by + bh + 6);
        aos_make_decorative(pn);
    }
}

/* In landscape, under the drawing: each port and its pins */
static void port_refs(lv_obj_t *parent, int32_t w)
{
    lv_obj_t *r = chips_row(parent, w);
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        char t[96] = "";
        size_t o = snprintf(t, sizeof t, "%s  ", pt->name);
        for (int k = 0; k < 4; k++) {
            if (pt->pins[k] < 0) continue;
            const char *role = "";
            port_of(pt->pins[k], &role);
            const aos_io_pin_t *pin = aos_io_pin_of_gpio(pt->pins[k]);
            if (o < sizeof t) o += (size_t)snprintf(t + o, sizeof t - o, "%s%s %d", k ? " · " : "", role, pin ? pin->pin : 0);
        }
        lv_obj_t *c = chip(r, t, false, go_ports_cb, NULL);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x172554), 0);
    }
}

static void build_header(void)
{
    int32_t w = U.W - 2 * AOS_UI_PAD, ch = lv_obj_get_height(U.content);
    lv_obj_t *col = column(U.content, w, ch);
    U.scroller = col;
    caption(col, U.land ? _("El conector de atrás, acostado: el pin 1 (5 V) es el cuadrado de arriba a la izquierda. Tocá un pin; abajo, los pines de cada puerto (número de pin físico).")
                        : _("El conector de atrás, como en el dibujo de Waveshare: el pin 1 (5 V) es el cuadrado, arriba a la derecha. Tocá un pin para ver qué es y quién lo tiene."), w);
    legend(col, w);
    if (U.land) {
        header_flat(col, w);
        port_refs(col, w);
    } else {
        header_upright(col, w);
    }
}

/* -------------------------------------------------------------------------- */
/* Ports and modules                                                           */
/* -------------------------------------------------------------------------- */

static bool file_exists(void)
{
    const char *root = aos_hal_path_sd_root();
    if (!root) return false;
    char path[256];
    struct stat st;
    snprintf(path, sizeof path, "%s/modules.txt", root);
    return stat(path, &st) == 0;
}

static void port_card(lv_obj_t *parent, const aos_io_port_t *pt, int32_t w)
{
    static const char *const G[4] = { AOS_SYM_SERIAL_PORT, AOS_SYM_CONNECTION, AOS_SYM_CHIP, AOS_SYM_TOGGLE_SWITCH };
    static const char *const ROLE[4][4] = { { "TX", "RX", "DE", "" }, { "SDA", "SCL", "", "" },
                                            { "SCK", "MOSI", "MISO", "" }, { "GPIO", "", "", "" } };
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 8, 0);
    lv_obj_t *head = box(c, lv_pct(100), 52);
    lv_obj_t *g = aos_label(head, G[pt->kind & 3], &aos_sym_44, C_MOD);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *n = aos_label(head, pt->name, aos_font_title, AOS_C_TEXT);
    lv_obj_align(n, LV_ALIGN_LEFT_MID, 60, 0);
    char k[40];
    switch (pt->kind) {
    case AOS_PORT_UART: snprintf(k, sizeof k, "UART · %u", (unsigned)pt->freq); break;
    case AOS_PORT_I2C: snprintf(k, sizeof k, "I2C · %u kHz", (unsigned)(pt->freq / 1000)); break;
    case AOS_PORT_SPI: snprintf(k, sizeof k, "SPI · %u MHz", (unsigned)(pt->freq / 1000000)); break;
    default: snprintf(k, sizeof k, "GPIO"); break;
    }
    lv_obj_t *kl = aos_label(head, k, aos_font_small, AOS_C_DIM);
    lv_obj_align(kl, LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *pins = chips_row(c, lv_pct(100));
    const char *owner = NULL;
    for (int i = 0; i < 4; i++) {
        if (pt->pins[i] < 0) continue;
        const aos_io_pin_t *pin = aos_io_pin_of_gpio(pt->pins[i]);
        char t[40];
        snprintf(t, sizeof t, _("%s  GPIO%d · pin %d"), ROLE[pt->kind & 3][i], pt->pins[i], pin ? pin->pin : 0);
        lv_obj_t *pc = chip(pins, t, false, NULL, NULL);
        lv_obj_set_height(pc, 48);
        if (!owner) owner = aos_io_owner(pt->pins[i]);
    }
    if (!strcmp(pt->name, "i2c.board")) caption(c, _("El bus de la placa: lo comparte con el táctil y los códecs, y ya tiene 0x14/0x5D, 0x18 y 0x40."), w - 40);
    if (owner) {
        lv_obj_t *l = aos_label(c, "", aos_font_small, lv_color_hex(0xD8B4FE));
        lv_label_set_text_fmt(l, _("Ahora lo usa: %s"), owner);
    }
    int nm = 0;
    for (int i = 0; i < aos_io_module_count(); i++) {
        const aos_io_module_t *m = aos_io_module_at(i);
        if (strcmp(m->port, pt->name)) continue;
        if (!nm++) {
            lv_obj_t *sep = box(c, lv_pct(100), 1);
            lv_obj_set_style_bg_color(sep, AOS_C_CARD2, 0);
            lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        }
        lv_obj_t *r = box(c, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r, 14, 0);
        const aos_sensor_chip_t *sc = aos_sensor_chip_find(m->name);
        char cs[24] = "";
        const char *cp = strstr(m->args, "chip=");
        if (cp) sscanf(cp + 5, "%23[^ \t#]", cs);
        if (!sc && cs[0]) sc = aos_sensor_chip_find(cs);
        aos_label(r, sc ? AOS_SYM_THERMOMETER : !strcmp(m->name, "target") ? AOS_SYM_DEVELOPER_BOARD : AOS_SYM_PUZZLE, &aos_sym_28, AOS_C_DIM);
        aos_label(r, m->name, aos_font_body, AOS_C_TEXT);
        if (sc && strcmp(sc->id, m->name)) aos_label(r, sc->name, aos_font_small, C_MOD);
        if (m->args[0]) {
            lv_obj_t *a = lv_label_create(r);
            lv_obj_set_style_text_font(a, &aos_mono_18, 0);
            lv_obj_set_style_text_color(a, AOS_C_DIM, 0);
            lv_label_set_text(a, m->args);
        }
    }
    if (!nm) caption(c, _("Sin módulos declarados en este puerto."), w - 40);
    if (pt->kind == AOS_PORT_I2C) {
        /* what autodetect found on it and nobody declared */
        char det[128] = "";
        size_t o = 0;
        for (int i = 0; i < aos_sensor_count() && o < sizeof det; i++) {
            aos_sensor_t s;
            if (!aos_sensor_at(i, &s) || s.declared || strcmp(s.port, pt->name)) continue;
            o += (size_t)snprintf(det + o, sizeof det - o, "%s%s 0x%02X", o ? ", " : "", s.chip, s.addr);
        }
        if (det[0]) {
            char t[160];
            snprintf(t, sizeof t, _("Detectados ahora: %s"), det);
            lv_obj_t *l = caption(c, t, w - 40);
            lv_obj_set_style_text_color(l, lv_color_hex(0x93C5FD), 0);
        }
    }
}

static void edit_cb(lv_event_t *e);
static void add_cb(lv_event_t *e);

static void reload_cb(lv_event_t *e)
{
    int n = aos_io_load();
    aos_sensors_rescan();
    char t[64];
    snprintf(t, sizeof t, _("modules.txt: %d puertos, %d módulos"), n, aos_io_module_count());
    aos_ui_toast(t, 1600);
    build_page();
}

static void build_ports(void)
{
    int32_t w = U.W - 2 * AOS_UI_PAD, ch = lv_obj_get_height(U.content);
    lv_obj_t *col = column(U.content, w, ch);
    U.scroller = col;
    lv_obj_t *row = chips_row(col, w);
    lv_obj_set_style_pad_gap(row, 12, 0);
    pill(row, AOS_SYM_PENCIL, _("Editar"), C_MOD_DARK, edit_cb, NULL);
    pill(row, AOS_SYM_PLUS, _("Agregar"), C_MOD_DARK, add_cb, (void *)(intptr_t)-1);
    pill(row, AOS_SYM_RESTART, _("Recargar"), AOS_C_CARD2, reload_cb, NULL);
    caption(col, file_exists() ? _("Leído de modules.txt, en la raíz de la tarjeta. Las apps abren un puerto por su nombre, así que mover un módulo de pines es editar este archivo.")
                               : _("No hay modules.txt en la tarjeta: esto es el perfil por defecto. Editar o agregar un módulo crea el archivo."), w);

    /* what the loaded file will not do */
    bool from_file;
    char *txt = aos_modules_read(&from_file);
    if (txt) {
        aos_mod_check_t ck;
        aos_modules_check(txt, strlen(txt), &ck);
        free(txt);
        if (ck.nwarn || !ck.ok) {
            lv_obj_t *wc = card(col, w, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_all(wc, 20, 0);
            lv_obj_set_flex_flow(wc, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(wc, 6, 0);
            lv_obj_set_style_border_width(wc, 2, 0);
            lv_obj_set_style_border_color(wc, C_WARN, 0);
            aos_label(wc, _("Ojo con modules.txt"), aos_font_body, C_WARN);
            char m[160];
            if (!ck.ok) { fmt_msg(&ck.err, m, sizeof m); caption(wc, m, w - 40); }
            for (int i = 0; i < ck.nwarn; i++) { fmt_msg(&ck.warn[i], m, sizeof m); caption(wc, m, w - 40); }
        }
    }

    section(col, _("PUERTOS"));
    lv_obj_t *grid = chips_row(col, w);
    lv_obj_set_style_pad_gap(grid, 16, 0);
    int32_t cw = U.land ? (w - 16) / 2 : w;
    for (int i = 0; i < aos_io_port_count(); i++) port_card(grid, aos_io_port_at(i), cw);
}

/* ---- the editor ---- */

static void ed_validate(void)
{
    if (!U.ta || !U.ed_status) return;
    const char *t = lv_textarea_get_text(U.ta);
    aos_mod_check_t ck;
    aos_modules_check(t, strlen(t), &ck);
    char m[200];
    if (!ck.ok) {
        fmt_msg(&ck.err, m, sizeof m);
        lv_obj_set_style_text_color(U.ed_status, C_BAD, 0);
    } else if (ck.nwarn) {
        char w1[160];
        fmt_msg(&ck.warn[0], w1, sizeof w1);
        snprintf(m, sizeof m, _("Se puede guardar, pero: %s"), w1);
        lv_obj_set_style_text_color(U.ed_status, C_WARN, 0);
    } else {
        snprintf(m, sizeof m, _("Bien: %d puertos y %d módulos"), ck.ports, ck.modules);
        lv_obj_set_style_text_color(U.ed_status, C_GOOD, 0);
    }
    lv_label_set_text(U.ed_status, m);
    U.ed_ok = ck.ok;
    if (U.ed_save) lv_obj_set_style_bg_opa(U.ed_save, ck.ok ? LV_OPA_COVER : LV_OPA_30, 0);
}

static void ed_changed_cb(lv_event_t *e) { ed_validate(); }

static void ed_save(void)
{
    if (!U.ta) return;
    const char *t = lv_textarea_get_text(U.ta);
    aos_mod_check_t ck;
    if (!aos_modules_save(t, strlen(t), &ck)) {
        if (ck.ok) aos_ui_toast(_("No se pudo escribir en la tarjeta"), 2000);
        ed_validate();
        return;
    }
    char m[64];
    snprintf(m, sizeof m, _("Guardado: %d puertos, %d módulos"), ck.ports, ck.modules);
    overlay_close();
    aos_ui_toast(m, 1600);
    build_page();
}

static void ed_save_cb(lv_event_t *e) { if (U.ed_ok) ed_save(); }
static void ed_cancel_cb(lv_event_t *e) { overlay_close(); }

static void ed_kb_cb(lv_event_t *e)
{
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) { if (U.ed_ok) ed_save(); }
    else if (c == LV_EVENT_CANCEL) overlay_close();
}

static void edit_cb(lv_event_t *e)
{
    bool from_file;
    char *text = aos_modules_read(&from_file);
    if (!text) return;
    overlay_close();
    U.overlay = box(U.root, U.W, U.H);
    lv_obj_set_style_bg_color(U.overlay, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(U.overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(U.overlay, LV_OBJ_FLAG_CLICKABLE);
    const int32_t top = U.land ? 16 : 24;
    lv_obj_t *t = aos_label(U.overlay, "modules.txt", aos_font_title, AOS_C_TEXT);
    lv_obj_set_pos(t, AOS_UI_PAD, top + 14);
    lv_obj_t *br = box(U.overlay, LV_SIZE_CONTENT, 76);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(br, 12, 0);
    lv_obj_align(br, LV_ALIGN_TOP_RIGHT, -AOS_UI_PAD, top);
    pill(br, NULL, _("Cancelar"), AOS_C_CARD2, ed_cancel_cb, NULL);
    U.ed_save = pill(br, AOS_SYM_CHECK, _("Guardar"), lv_color_hex(0x2563EB), ed_save_cb, NULL);
    U.ed_status = aos_label(U.overlay, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.ed_status, U.W - 2 * AOS_UI_PAD);
    lv_label_set_long_mode(U.ed_status, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.ed_status, AOS_UI_PAD, top + 84);
    int32_t kh = U.land ? U.H / 2 : U.H * 2 / 5;
    int32_t ty = top + 84 + 34;
    U.ta = lv_textarea_create(U.overlay);
    lv_textarea_set_one_line(U.ta, false);
    lv_textarea_set_max_length(U.ta, 8000);
    lv_textarea_set_text(U.ta, text);
    lv_textarea_set_cursor_pos(U.ta, 0);
    free(text);
    lv_obj_set_size(U.ta, U.W - 2 * AOS_UI_PAD, U.H - kh - ty - 12);
    lv_obj_set_pos(U.ta, AOS_UI_PAD, ty);
    lv_obj_set_style_text_font(U.ta, &aos_mono_22, 0);
    lv_obj_set_style_bg_color(U.ta, AOS_C_CARD, 0);
    lv_obj_set_style_text_color(U.ta, AOS_C_TEXT, 0);
    lv_obj_set_style_border_width(U.ta, 0, 0);
    lv_obj_set_style_radius(U.ta, 20, 0);
    lv_obj_set_style_pad_all(U.ta, 18, 0);
    lv_obj_add_event_cb(U.ta, ed_changed_cb, LV_EVENT_VALUE_CHANGED, NULL);
    U.kb = lv_keyboard_create(U.overlay);
    lv_obj_set_size(U.kb, U.W, kh);
    lv_obj_align(U.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    aos_keyboard_style(U.kb, aos_font_body);
    lv_keyboard_set_textarea(U.kb, U.ta);
    lv_obj_add_event_cb(U.kb, ed_kb_cb, LV_EVENT_ALL, NULL);
    ed_validate();
    if (!from_file) {
        lv_label_set_text(U.ed_status, _("Es el perfil por defecto: al guardar se crea modules.txt en la tarjeta."));
        lv_obj_set_style_text_color(U.ed_status, AOS_C_DIM, 0);
    }
}

/* ---- the form: a module without typing ---- */

static int ports_of_kind(int kind, const aos_io_port_t **out, int max)
{
    int n = 0;
    for (int i = 0; i < aos_io_port_count() && n < max; i++)
        if ((int)aos_io_port_at(i)->kind == kind) out[n++] = aos_io_port_at(i);
    return n;
}

static void form_line(char *out, size_t n, bool *ok)
{
    const aos_io_port_t *pts[AOS_IO_PORT_MAX];
    bool rs = F.kind == F_RS485;
    int np = ports_of_kind(rs ? AOS_PORT_UART : AOS_PORT_I2C, pts, AOS_IO_PORT_MAX);
    *ok = np > 0;
    if (!np) { out[0] = 0; return; }
    if (F.port >= np) F.port = 0;
    char name[24];
    if (rs) {
        static const int BAUD[4] = { 9600, 19200, 38400, 115200 };
        aos_modules_free_name("rs485", name, sizeof name);
        snprintf(out, n, "module %s %s baud=%d", name, pts[F.port]->name, BAUD[F.opt & 3]);
        return;
    }
    const aos_sensor_chip_t *c = aos_sensor_chip_at(F.kind);
    if (F.addr >= c->naddr) F.addr = 0;
    aos_modules_free_name(c->id, name, sizeof name);
    size_t o = (size_t)snprintf(out, n, "module %s %s addr=0x%02X", name, pts[F.port]->name, c->addrs[F.addr]);
    static const int SHUNT[3] = { 100, 50, 10 };
    static const int GAIN[4] = { 6144, 4096, 2048, 1024 };
    if (c->shunt && o < n) snprintf(out + o, n - o, " shunt=%d", SHUNT[F.opt % 3]);
    if (c->gain && o < n) snprintf(out + o, n - o, " gain=%d", GAIN[F.opt % 4]);
}

static void form_build(void);
static void f_kind_cb(lv_event_t *e) { F.kind = (int)(intptr_t)lv_event_get_user_data(e); F.addr = 0; F.opt = F.kind == F_RS485 ? 0 : (aos_sensor_chip_at(F.kind)->gain ? 1 : 0); form_build(); }
static void f_port_cb(lv_event_t *e) { F.port = (int)(intptr_t)lv_event_get_user_data(e); form_build(); }
static void f_addr_cb(lv_event_t *e) { F.addr = (int)(intptr_t)lv_event_get_user_data(e); form_build(); }
static void f_opt_cb(lv_event_t *e) { F.opt = (int)(intptr_t)lv_event_get_user_data(e); form_build(); }

static void f_add_cb(lv_event_t *e)
{
    char line[120];
    bool ok;
    form_line(line, sizeof line, &ok);
    if (!ok) return;
    aos_mod_check_t ck;
    if (aos_modules_append(line, &ck)) {
        overlay_close();
        aos_ui_toast(_("Agregado a modules.txt"), 1600);
        build_page();
        return;
    }
    char m[160];
    if (!ck.ok) fmt_msg(&ck.err, m, sizeof m);
    else scpy(m, sizeof m, _("No se pudo escribir en la tarjeta"));
    aos_ui_toast(m, 2400);
}

static void form_build(void)
{
    lv_obj_t *prev_scroll = NULL;
    int32_t sy = 0;
    if (U.overlay && lv_obj_get_child_count(U.overlay)) {
        prev_scroll = lv_obj_get_child(U.overlay, 0);
        sy = lv_obj_get_scroll_y(prev_scroll);
    }
    int32_t w = U.W - 48 < 680 ? U.W - 48 : 680, iw = w - 56;
    if (U.land) w = U.W - 120 < 1000 ? U.W - 120 : 1000, iw = w - 56;
    lv_obj_t *sh = sheet_open(w);
    aos_label(sh, _("Agregar módulo"), aos_font_title, AOS_C_TEXT);
    section(sh, _("QUÉ ES"));
    lv_obj_t *r = chips_row(sh, iw);
    for (int i = 0; i < aos_sensor_chip_count(); i++) chip(r, aos_sensor_chip_at(i)->name, F.kind == i, f_kind_cb, (void *)(intptr_t)i);
    chip(r, "RS485", F.kind == F_RS485, f_kind_cb, (void *)(intptr_t)F_RS485);
    bool rs = F.kind == F_RS485;
    const aos_sensor_chip_t *c = rs ? NULL : aos_sensor_chip_at(F.kind);
    caption(sh, rs ? _("Un transceptor RS485 (MAX485 o uno automático) para Modbus RTU; el DE va en el pin de del puerto.")
                   : chip_what(c->id), iw);

    const aos_io_port_t *pts[AOS_IO_PORT_MAX];
    int np = ports_of_kind(rs ? AOS_PORT_UART : AOS_PORT_I2C, pts, AOS_IO_PORT_MAX);
    section(sh, _("PUERTO"));
    if (!np) caption(sh, rs ? _("No hay puertos UART en modules.txt.") : _("No hay puertos I2C en modules.txt."), iw);
    r = chips_row(sh, iw);
    for (int i = 0; i < np; i++) chip(r, pts[i]->name, F.port == i, f_port_cb, (void *)(intptr_t)i);
    if (np && !rs && !strcmp(pts[F.port < np ? F.port : 0]->name, "i2c.board"))
        caption(sh, _("Ojo: el bus de la placa ya tiene el ES7210 en 0x40, donde suelen estar los INA219."), iw);
    char t[32];
    if (!rs) {
        section(sh, _("DIRECCIÓN"));
        r = chips_row(sh, iw);
        for (int i = 0; i < c->naddr; i++) {
            snprintf(t, sizeof t, "0x%02X", c->addrs[i]);
            chip(r, t, F.addr == i, f_addr_cb, (void *)(intptr_t)i);
        }
        if (c->shunt) {
            section(sh, _("RESISTENCIA DEL SHUNT"));
            r = chips_row(sh, iw);
            static const char *const SH[3] = { "0,1 Ω (R100)", "0,05 Ω (R050)", "0,01 Ω (R010)" };
            for (int i = 0; i < 3; i++) chip(r, SH[i], F.opt % 3 == i, f_opt_cb, (void *)(intptr_t)i);
        }
        if (c->gain) {
            section(sh, _("RANGO"));
            r = chips_row(sh, iw);
            static const char *const G[4] = { "±6,144 V", "±4,096 V", "±2,048 V", "±1,024 V" };
            for (int i = 0; i < 4; i++) chip(r, G[i], F.opt % 4 == i, f_opt_cb, (void *)(intptr_t)i);
        }
    } else {
        section(sh, _("VELOCIDAD"));
        r = chips_row(sh, iw);
        static const char *const B[4] = { "9600", "19200", "38400", "115200" };
        for (int i = 0; i < 4; i++) chip(r, B[i], (F.opt & 3) == i, f_opt_cb, (void *)(intptr_t)i);
    }
    section(sh, _("LA LÍNEA"));
    char line[120];
    bool ok;
    form_line(line, sizeof line, &ok);
    lv_obj_t *pv = card(sh, iw, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(pv, lv_color_hex(0x0B0B0D), 0);
    lv_obj_set_style_pad_all(pv, 16, 0);
    lv_obj_set_style_radius(pv, 16, 0);
    U.f_preview = lv_label_create(pv);
    lv_obj_set_style_text_font(U.f_preview, &aos_mono_22, 0);
    lv_obj_set_style_text_color(U.f_preview, lv_color_hex(0xFBBF24), 0);
    lv_obj_set_width(U.f_preview, iw - 32);
    lv_label_set_long_mode(U.f_preview, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(U.f_preview, ok ? line : "");
    lv_obj_t *br = box(sh, iw, 84);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(br, 12, 0);
    pill(br, NULL, _("Cancelar"), AOS_C_CARD2, close_cb, NULL);
    lv_obj_t *ab = pill(br, AOS_SYM_PLUS, _("Agregar"), lv_color_hex(0x2563EB), ok ? f_add_cb : NULL, NULL);
    if (!ok) lv_obj_set_style_bg_opa(ab, LV_OPA_30, 0);
    if (prev_scroll) {
        lv_obj_update_layout(sh);
        lv_obj_scroll_to_y(sh, sy, LV_ANIM_OFF);
    }
}

/* user data: -1 opens the form fresh, else a candidate's index */
static void add_cb(lv_event_t *e)
{
    int ci = (int)(intptr_t)lv_event_get_user_data(e);
    memset(&F, 0, sizeof F);
    aos_sensor_cand_t cand[AOS_SENSOR_CAND_MAX];
    int nc = ci >= 0 ? aos_sensor_candidates(cand, AOS_SENSOR_CAND_MAX) : 0;
    if (ci >= 0 && ci < nc) {
        for (int i = 0; i < aos_sensor_chip_count(); i++)
            if (!strcmp(aos_sensor_chip_at(i)->id, cand[ci].chip)) F.kind = i;
        const aos_sensor_chip_t *c = aos_sensor_chip_at(F.kind);
        for (int i = 0; i < c->naddr; i++) if (c->addrs[i] == cand[ci].addr) F.addr = i;
        const aos_io_port_t *pts[AOS_IO_PORT_MAX];
        int np = ports_of_kind(AOS_PORT_I2C, pts, AOS_IO_PORT_MAX);
        for (int i = 0; i < np; i++) if (!strcmp(pts[i]->name, cand[ci].port)) F.port = i;
        if (c->gain) F.opt = 1;
    }
    form_build();
}

/* -------------------------------------------------------------------------- */
/* Sensors                                                                     */
/* -------------------------------------------------------------------------- */

/* What the page's shape depends on: when it changes, it is built again */
static void sens_sig(char *out, size_t n)
{
    size_t o = 0;
    int cnt = aos_sensor_count();
    for (int i = 0; i < cnt && o < n; i++) {
        aos_sensor_t s;
        if (!aos_sensor_at(i, &s)) continue;
        o += (size_t)snprintf(out + o, n - o, "%s/%s/%d/%d;", s.name, s.chip, s.n, s.state == AOS_SENSOR_OK || s.n > 0);
    }
    aos_sensor_cand_t c[AOS_SENSOR_CAND_MAX];
    int nc = aos_sensor_candidates(c, AOS_SENSOR_CAND_MAX);
    for (int i = 0; i < nc && o < n; i++) o += (size_t)snprintf(out + o, n - o, "c%02x;", c[i].addr);
}

static bool sel_find(int *si, int *sk, aos_sensor_t *out)
{
    int cnt = aos_sensor_count();
    for (int i = 0; i < cnt; i++) {
        aos_sensor_t s;
        if (!aos_sensor_at(i, &s) || strcmp(s.name, S.sel_name)) continue;
        for (int k = 0; k < s.n; k++)
            if (!strcmp(s.v[k].key, S.sel_key)) { *si = i; *sk = k; if (out) *out = s; return true; }
    }
    /* nothing picked (or it went away): a temperature if there is one, else
     * the first reading there is */
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < cnt; i++) {
            aos_sensor_t s;
            if (!aos_sensor_at(i, &s) || !s.n) continue;
            for (int k = 0; k < s.n; k++) {
                if (pass == 0 && s.v[k].q != AOS_SQ_TEMP) continue;
                *si = i;
                *sk = k;
                if (out) *out = s;
                return true;
            }
        }
    return false;
}

/* The smallest span the chart shows, so noise does not fill it */
static float min_span(int q)
{
    static const float M[AOS_SQ_COUNT] = { 1.0f, 4.0f, 1.0f, 20.0f, 0.05f, 0.01f, 0.05f };
    return q >= 0 && q < AOS_SQ_COUNT ? M[q] : 1.0f;
}

static void chart_refresh(void)
{
    int si = -1, sk = -1;
    aos_sensor_t s;
    bool have = sel_find(&si, &sk, &s);
    U.sel_i = have ? si : -1;
    U.sel_k = have ? sk : -1;
    if (!U.chart) return;
    if (!have) {
        lv_label_set_text(U.ch_title, _("Sin lecturas todavía"));
        lv_label_set_text(U.ch_value, "--");
        lv_label_set_text(U.ch_range, "");
        lv_chart_set_all_values(U.chart, U.ser, LV_CHART_POINT_NONE);
        return;
    }
    char t[64];
    snprintf(t, sizeof t, "%s · %s", s.name, key_name(s.v[sk].key));
    lv_label_set_text(U.ch_title, t);
    fmt_value(s.v[sk].q, s.state == AOS_SENSOR_OK ? s.v[sk].v : NAN, t, sizeof t);
    lv_label_set_text(U.ch_value, t);
    static float h[AOS_SENSOR_HIST];
    int n = aos_sensor_history(si, sk, h, AOS_SENSOR_HIST);
    float lo = INFINITY, hi = -INFINITY;
    for (int i = 0; i < n; i++) if (!isnan(h[i])) { if (h[i] < lo) lo = h[i]; if (h[i] > hi) hi = h[i]; }
    int32_t *y = lv_chart_get_series_y_array(U.chart, U.ser);
    for (int i = 0; i < AOS_SENSOR_HIST; i++) {
        int j = i - (AOS_SENSOR_HIST - n);
        y[i] = j < 0 || isnan(h[j]) ? LV_CHART_POINT_NONE : (int32_t)lrintf(h[j] * 1000.0f);
    }
    if (lo <= hi) {
        char a[32], b[32];
        fmt_value(s.v[sk].q, lo, a, sizeof a);
        fmt_value(s.v[sk].q, hi, b, sizeof b);
        int secs = n;
        char r[96];
        if (secs >= 120) snprintf(r, sizeof r, _("mín %s · máx %s · últimos %d min"), a, b, secs / 60);
        else snprintf(r, sizeof r, _("mín %s · máx %s · últimos %d s"), a, b, secs);
        lv_label_set_text(U.ch_range, r);
        float span = hi - lo, ms = min_span(s.v[sk].q);
        if (span < ms) { float m = (hi + lo) / 2; lo = m - ms / 2; hi = m + ms / 2; }
        else { lo -= span * 0.1f; hi += span * 0.1f; }
        lv_chart_set_axis_range(U.chart, LV_CHART_AXIS_PRIMARY_Y, (int32_t)floorf(lo * 1000.0f), (int32_t)ceilf(hi * 1000.0f));
    } else {
        lv_label_set_text(U.ch_range, "");
    }
    lv_chart_refresh(U.chart);
}

static void ports_status(char *out, size_t n)
{
    aos_sensor_port_t ps[AOS_IO_PORT_MAX];
    int np = aos_sensors_ports(ps, AOS_IO_PORT_MAX);
    size_t o = 0;
    out[0] = 0;
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = 0; i < np && o < n; i++) {
        char t[96];
        if (ps[i].state == AOS_SENSOR_BUSY) snprintf(t, sizeof t, _("%s: los pines los tiene %s"), ps[i].port, ps[i].owner);
        else if (ps[i].state == AOS_SENSOR_ERR) snprintf(t, sizeof t, _("%s: no se pudo abrir"), ps[i].port);
        else if (ps[i].scanned)
            snprintf(t, sizeof t, _("%s: %d sensores · buscó hace %u s"), ps[i].port, ps[i].found, (unsigned)((now - ps[i].scan_ms) / 1000));
        else snprintf(t, sizeof t, _("%s: %d sensores"), ps[i].port, ps[i].found);
        o += (size_t)snprintf(out + o, n - o, "%s%s", o ? "\n" : "", t);
    }
    if (!np) scpy(out, n, _("Buscando…"));
}

static void sens_values_refresh(void)
{
    chart_refresh();
    int cnt = aos_sensor_count();
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    for (int i = 0; i < cnt && i < AOS_SENSOR_MAX; i++) {
        aos_sensor_t s;
        if (!aos_sensor_at(i, &s)) continue;
        for (int k = 0; k < s.n && k < AOS_SENSOR_VALUES; k++) {
            if (!U.tval[i][k]) continue;
            char t[32];
            fmt_value(s.v[k].q, s.state == AOS_SENSOR_OK ? s.v[k].v : NAN, t, sizeof t);
            lv_label_set_text(U.tval[i][k], t);
            bool sel = i == U.sel_i && k == U.sel_k;
            lv_obj_set_style_border_width(U.tile[i][k], sel ? 3 : 0, 0);
        }
        if (U.sstate[i]) {
            char t[96];
            if (s.state == AOS_SENSOR_OK) {
                lv_obj_set_style_text_color(U.sstate[i], AOS_C_DIM, 0);
                snprintf(t, sizeof t, _("%u lecturas · %u errores"), (unsigned)s.reads, (unsigned)s.errors);
                (void)now;
            } else if (s.state == AOS_SENSOR_WAIT) {
                lv_obj_set_style_text_color(U.sstate[i], AOS_C_DIM, 0);
                scpy(t, sizeof t, s.err[0] ? "" : _("Esperando la primera lectura"));
                if (s.err[0]) fmt_err(s.err, t, sizeof t);
            } else {
                lv_obj_set_style_text_color(U.sstate[i], C_BAD, 0);
                fmt_err(s.err, t, sizeof t);
            }
            lv_label_set_text(U.sstate[i], t);
        }
    }
    if (U.status) {
        char st[256];
        ports_status(st, sizeof st);
        lv_label_set_text(U.status, st);
    }
}

static void tile_cb(lv_event_t *e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e), i = v / AOS_SENSOR_VALUES, k = v % AOS_SENSOR_VALUES;
    aos_sensor_t s;
    if (!aos_sensor_at(i, &s) || k >= s.n) return;
    scpy(S.sel_name, sizeof S.sel_name, s.name);
    scpy(S.sel_key, sizeof S.sel_key, s.v[k].key);
    sens_values_refresh();
}

static void rescan_cb(lv_event_t *e)
{
    aos_sensors_rescan();
    aos_ui_toast(_("Buscando sensores…"), 1200);
}

static void build_chart(lv_obj_t *parent, int32_t w, int32_t h)
{
    lv_obj_t *c = card(parent, w, h);
    lv_obj_set_style_pad_all(c, 20, 0);
    U.ch_title = aos_label(c, "", aos_font_small, AOS_C_DIM);
    lv_obj_set_pos(U.ch_title, 0, 0);
    U.ch_value = aos_label(c, "", aos_font_large, AOS_C_TEXT);
    lv_obj_set_pos(U.ch_value, 0, 30);
    U.ch_range = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.ch_range, w - 40);
    lv_label_set_long_mode(U.ch_range, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_pos(U.ch_range, 0, 92);
    U.chart = lv_chart_create(c);
    lv_obj_set_size(U.chart, w - 40, h - 40 - 126);
    lv_obj_set_pos(U.chart, 0, 126);
    lv_obj_set_style_bg_opa(U.chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(U.chart, 0, 0);
    lv_obj_set_style_pad_all(U.chart, 4, 0);
    lv_obj_set_style_line_color(U.chart, AOS_C_CARD2, LV_PART_MAIN);
    lv_chart_set_type(U.chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(U.chart, AOS_SENSOR_HIST);
    lv_chart_set_div_line_count(U.chart, 4, 0);
    lv_obj_set_style_size(U.chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(U.chart, 4, LV_PART_ITEMS);
    U.ser = lv_chart_add_series(U.chart, C_MOD, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(U.chart, U.ser, LV_CHART_POINT_NONE);
    aos_make_decorative(U.chart);
}

static void sensor_card(lv_obj_t *parent, int i, const aos_sensor_t *s, int32_t w)
{
    lv_obj_t *c = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_t *head = box(c, lv_pct(100), 48);
    lv_obj_t *g = aos_label(head, q_glyph(s->n ? s->v[0].q : AOS_SQ_TEMP, s->n ? s->v[0].key : NULL), &aos_sym_28, C_MOD);
    lv_obj_align(g, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *nm = aos_label(head, s->name, aos_font_body, AOS_C_TEXT);
    lv_obj_align(nm, LV_ALIGN_LEFT_MID, 44, 0);
    char t[64];
    snprintf(t, sizeof t, "%s · %s 0x%02X", s->chip, s->port, s->addr);
    lv_obj_t *info = aos_label(head, t, aos_font_caption, AOS_C_DIM);
    lv_obj_align(info, LV_ALIGN_RIGHT_MID, 0, -12);
    lv_obj_t *tag = aos_label(head, s->declared ? "modules.txt" : _("detectado"), aos_font_tiny, s->declared ? C_MOD : lv_color_hex(0x93C5FD));
    lv_obj_align(tag, LV_ALIGN_RIGHT_MID, 0, 13);
    if (s->n) {
        int cols = U.land ? (s->n < 3 ? s->n : 3) : (s->n < 2 ? s->n : 2);
        int32_t iw = w - 36, tw = (iw - (cols - 1) * 10) / cols;
        lv_obj_t *row = chips_row(c, iw);
        for (int k = 0; k < s->n && k < AOS_SENSOR_VALUES; k++) {
            lv_obj_t *tl = box(row, tw, 104);
            lv_obj_set_style_bg_color(tl, AOS_C_CARD2, 0);
            lv_obj_set_style_bg_opa(tl, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(tl, 18, 0);
            lv_obj_set_style_border_color(tl, C_MOD, 0);
            lv_obj_set_style_pad_hor(tl, 16, 0);
            lv_obj_set_style_bg_opa(tl, LV_OPA_60, LV_STATE_PRESSED);
            lv_obj_add_flag(tl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(tl, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i * AOS_SENSOR_VALUES + k));
            lv_obj_t *gl = aos_label(tl, q_glyph(s->v[k].q, s->v[k].key), &aos_sym_28, AOS_C_DIM);
            lv_obj_align(gl, LV_ALIGN_TOP_LEFT, -4, 10);
            lv_obj_t *kl = aos_label(tl, key_name(s->v[k].key), aos_font_caption, AOS_C_DIM);
            lv_obj_align(kl, LV_ALIGN_TOP_LEFT, 30, 12);
            U.tval[i][k] = aos_label(tl, "", cols >= 3 ? aos_font_body : aos_font_title, AOS_C_TEXT);
            lv_obj_align(U.tval[i][k], LV_ALIGN_BOTTOM_LEFT, 0, -12);
            U.tile[i][k] = tl;
            aos_make_decorative(gl);
            aos_make_decorative(kl);
            aos_make_decorative(U.tval[i][k]);
        }
    }
    U.sstate[i] = aos_label(c, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(U.sstate[i], w - 36);
    lv_label_set_long_mode(U.sstate[i], LV_LABEL_LONG_MODE_WRAP);
}

static void cand_rows(lv_obj_t *parent, int32_t w)
{
    aos_sensor_cand_t c[AOS_SENSOR_CAND_MAX];
    int nc = aos_sensor_candidates(c, AOS_SENSOR_CAND_MAX);
    if (!nc) return;
    section(parent, _("PARECE QUE HAY"));
    lv_obj_t *cd = card(parent, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cd, LV_FLEX_FLOW_COLUMN);
    for (int i = 0; i < nc; i++) {
        lv_obj_t *r = box(cd, lv_pct(100), 104);
        lv_obj_set_style_pad_hor(r, 20, 0);
        if (i) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, AOS_C_CARD2, 0);
        }
        lv_obj_t *a = lv_label_create(r);
        lv_obj_set_style_text_font(a, &aos_mono_22, 0);
        lv_obj_set_style_text_color(a, lv_color_hex(0xF59E0B), 0);
        lv_label_set_text_fmt(a, "0x%02X", c[i].addr);
        lv_obj_align(a, LV_ALIGN_LEFT_MID, 0, -14);
        lv_obj_t *pl = aos_label(r, c[i].port, aos_font_tiny, AOS_C_DIM);
        lv_obj_align(pl, LV_ALIGN_LEFT_MID, 0, 18);
        const aos_sensor_chip_t *sc = aos_sensor_chip_find(c[i].chip);
        char t[96];
        snprintf(t, sizeof t, _("¿Es un %s?"), sc ? sc->name : c[i].chip);
        lv_obj_t *q = aos_label(r, t, aos_font_body, AOS_C_TEXT);
        lv_obj_align(q, LV_ALIGN_LEFT_MID, 100, -14);
        lv_obj_t *gl = aos_label(r, c[i].guess[0] ? c[i].guess : _("no dice qué es"), aos_font_caption, AOS_C_DIM);
        lv_obj_set_width(gl, w - 100 - 40 - 200);
        lv_label_set_long_mode(gl, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(gl, LV_ALIGN_LEFT_MID, 100, 18);
        lv_obj_t *b = pill(r, AOS_SYM_PLUS, _("Agregar"), C_MOD_DARK, add_cb, (void *)(intptr_t)i);
        lv_obj_set_height(b, 64);
        lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    caption(parent, _("Estos chips no tienen cómo decir qué son sin que alguien les escriba: agregalos a modules.txt si son lo que parecen."), w);
}

static void build_sensors(void)
{
    int32_t w = U.W - 2 * AOS_UI_PAD, ch = lv_obj_get_height(U.content);
    memset(U.tile, 0, sizeof U.tile);
    memset(U.tval, 0, sizeof U.tval);
    memset(U.sstate, 0, sizeof U.sstate);
    lv_obj_t *left, *right;
    int32_t lw, rw;
    if (U.land) {
        lw = 560;
        rw = w - lw - AOS_UI_PAD;
        left = column(U.content, lw, ch);
        right = column(U.content, rw, ch);
        lv_obj_set_x(right, lw + AOS_UI_PAD);
        build_chart(left, lw, 380);
    } else {
        lw = rw = w;
        left = right = column(U.content, w, ch);
        build_chart(left, w, 380);
    }
    U.scroller = right;
    lv_obj_t *row = box(left, lw, 80);
    lv_obj_t *rb = pill(row, AOS_SYM_MAGNIFY, _("Buscar de nuevo"), C_MOD_DARK, rescan_cb, NULL);
    lv_obj_align(rb, LV_ALIGN_LEFT_MID, 0, 0);
    U.status = caption(left, "", lw);

    int cnt = aos_sensor_count();
    for (int i = 0; i < cnt && i < AOS_SENSOR_MAX; i++) {
        aos_sensor_t s;
        if (aos_sensor_at(i, &s)) sensor_card(right, i, &s, rw);
    }
    if (!cnt) {
        lv_obj_t *e = card(right, rw, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(e, 24, 0);
        lv_obj_set_flex_flow(e, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(e, 10, 0);
        aos_label(e, _("No hay sensores"), aos_font_body, AOS_C_TEXT);
        caption(e, _("Conectá uno a i2c.ext: SDA al GPIO21 (pin 15), SCL al GPIO22 (pin 17), 3V3 (pin 18) y GND (pin 19). Los BME280, SHT3x/4x, AHT20, INA219/226 y ADS1115 se reconocen solos; también se pueden declarar en modules.txt."), rw - 48);
    }
    cand_rows(right, rw);
    sens_sig(U.sig, sizeof U.sig);
    U.seen_seq = aos_sensors_seq();
    sens_values_refresh();
}

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

static void build_page(void)
{
    overlay_close();
    lv_obj_clean(U.content);
    U.scroller = U.status = U.chart = NULL;
    memset(U.tval, 0, sizeof U.tval);
    memset(U.sstate, 0, sizeof U.sstate);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_color_t c = i == S.tab ? C_MOD : AOS_C_DIM;
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 0), c, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(U.tabs[i], 1), c, 0);
    }
    if (S.tab == TAB_HDR) build_header();
    else if (S.tab == TAB_PORTS) build_ports();
    else build_sensors();
}

static void tab_cb(lv_event_t *e)
{
    S.tab = (int)(intptr_t)lv_event_get_user_data(e);
    build_page();
}

static void timer_cb(lv_timer_t *t)
{
    aos_sensors_keep();
    if (S.tab != TAB_SENS || aos_sensors_seq() == U.seen_seq) return;
    U.seen_seq = aos_sensors_seq();
    char sig[sizeof U.sig];
    sens_sig(sig, sizeof sig);
    if (strcmp(sig, U.sig) && !U.overlay) {
        /* a sensor came or went: build it again where the list was */
        int32_t y = U.scroller ? lv_obj_get_scroll_y(U.scroller) : 0;
        build_page();
        if (U.scroller) {
            lv_obj_update_layout(U.scroller);
            lv_obj_scroll_to_y(U.scroller, y, LV_ANIM_OFF);
        }
        return;
    }
    sens_values_refresh();
}

static void *create(aos_app_t *self, lv_obj_t *root)
{
    memset(&U, 0, sizeof U);
    U.root = root;
    U.W = lv_obj_get_width(root);
    U.H = lv_obj_get_height(root);
    U.land = U.W > U.H;
    aos_sensors_keep();
    const int32_t tab_h = U.land ? 96 : 116;
    U.content = box(root, U.W, U.H - tab_h);
    lv_obj_set_style_pad_hor(U.content, AOS_UI_PAD, 0);
    lv_obj_set_style_pad_top(U.content, 8, 0);
    lv_obj_update_layout(U.content);

    lv_obj_t *bar = box(root, U.W, tab_h);
    lv_obj_set_pos(bar, 0, U.H - tab_h);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x121216), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < TAB_COUNT; i++) {
        lv_obj_t *t = box(bar, U.W / TAB_COUNT, tab_h);
        lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t *g = aos_label(t, TAB_GLYPH[i], &aos_sym_44, AOS_C_DIM);
        lv_obj_align(g, LV_ALIGN_CENTER, 0, U.land ? -14 : -16);
        lv_obj_t *n = aos_label(t, aos_tr(TAB_NAME[i]), aos_font_tiny, AOS_C_DIM);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, U.land ? 26 : 30);
        lv_obj_add_event_cb(t, tab_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        U.tabs[i] = t;
    }
    build_page();
    U.timer = lv_timer_create(timer_cb, 250, NULL);
    return &U;
}

static void destroy(aos_app_t *self, void *inst)
{
    if (U.timer) lv_timer_delete(U.timer);
    memset(&U, 0, sizeof U);
}

static bool back(aos_app_t *self, void *inst)
{
    if (U.overlay) { overlay_close(); return true; }
    return false;
}

void aos_app_modules_get(aos_app_t *app)
{
    *app = (aos_app_t){
        .desc = {
            .id = "aos.modules", .name = "Módulos", .icon = AOS_SYM_EXPANSION_CARD,
            .color_a = 0x94A3B8, .color_b = 0x475569,
            .order = 515,
        },
        .create = create, .destroy = destroy, .back = back,
    };
}
