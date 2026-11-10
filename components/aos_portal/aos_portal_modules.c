/*
 * P4OS - the portal's Expansión page: the header, the ports and modules of
 * modules.txt, its editor and the sensors (aos_portal.c hands every
 * /api/expansion* and /api/sensors* request here).
 *
 *   GET  /api/expansion                the 40 pins with owner and port, the ports,
 *                                      the modules, modules.txt and its check,
 *                                      the chips with a driver
 *   POST /api/expansion/check          body: a modules.txt; the check, in words
 *   POST /api/expansion/save           body: a modules.txt; checked, written, reloaded
 *   GET  /api/sensors                  every sensor and its latest values, the
 *                                      candidates, how each port went
 *   GET  /api/sensors/hist?i=&k=       value k of sensor i, the last five minutes
 *   POST /api/sensors/rescan
 *   POST /api/expansion/selftest?gpio=&gpio2=
 *                                      PWM, the analog level, IR and CAN tried
 *                                      on a free pin with nothing wired: CAN in
 *                                      its self test, IR out (and in on gpio2
 *                                      when the two are jumpered), what each
 *                                      answered. For the developer.
 *
 * The words are Spanish, like the rest of the portal (the Módulos app says
 * the same through the catalogs).
 */
#include "aos_portal_modules.h"
#include "aos_i18n.h"
#include "aos_hal.h"
#include "aos_io.h"
#include "aos_modules.h"
#include "aos_sensors.h"
#include "aos_sensor_text.h"
#include "cJSON.h"

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

static void send_cjson(aos_httpd_req_t *r, int status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    aos_httpd_send_json(r, status, s ? s : "{}");
    free(s);
    cJSON_Delete(o);
}

static void send_err(aos_httpd_req_t *r, int status, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddFalseToObject(o, "ok");
    cJSON_AddStringToObject(o, "error", msg);
    send_cjson(r, status, o);
}

/* A float with the digits it has; NAN has no JSON: null. */
static void add_num(cJSON *o, const char *k, double v)
{
    if (isnan(v) || isinf(v)) { cJSON_AddNullToObject(o, k); return; }
    char t[24];
    snprintf(t, sizeof t, "%.6g", v);
    cJSON_AddNumberToObject(o, k, strtod(t, NULL));
}

/* -------------------------------------------------------------------------- */
/* Words                                                                       */
/* -------------------------------------------------------------------------- */

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
    case 11: return "JTAG MTDO";
    case 12: return "JTAG MTCK";
    case 14: return "JTAG MTDI";
    case 15: return _("ADC1 canal 5");
    case 17: return _("ADC1 canal 6");
    case 21: return _("USB-Serial-JTAG D−: libre si no se usa");
    case 23: return _("USB-Serial-JTAG D+: libre si no se usa");
    case 25: case 27: return _("USB 2.0 HS, en paralelo con el puerto OTG: no es un GPIO");
    case 28: return _("Strapping de la fuente del JTAG: usable después del arranque");
    case 30: return _("BOOT: strapping, con pull-up de 4,7K");
    case 32: return _("ADC2 canal 0");
    case 34: return _("ADC2 canal 1");
    case 36: return _("ADC2 canal 2, comparador");
    case 35: case 37: case 39: return _("Dominio VO4");
    case 38: return _("ADC2 canal 3, comparador");
    default: return "";
    }
}

static void fmt_msg(const aos_mod_msg_t *m, char *out, size_t n)
{
    char t[128];
    switch (m->code) {
    case AOS_MOD_E_LONG: snprintf(t, sizeof t, "%s", _("la línea es demasiado larga")); break;
    case AOS_MOD_E_SYNTAX: snprintf(t, sizeof t, "%s", _("se esperaba «port» o «module», un nombre y lo demás")); break;
    case AOS_MOD_E_TOO_MANY_PORTS: snprintf(t, sizeof t, _("demasiados puertos (%d como mucho)"), AOS_IO_PORT_MAX); break;
    case AOS_MOD_E_PORT_NAME: snprintf(t, sizeof t, "%s", _("el nombre del puerto es demasiado largo")); break;
    case AOS_MOD_E_PORT_KIND: snprintf(t, sizeof t, "%s", _("los puertos se llaman uart…, i2c…, spi… o gpio…")); break;
    case AOS_MOD_E_NOT_HEADER: snprintf(t, sizeof t, "%s", _("un GPIO que no está en el header")); break;
    case AOS_MOD_E_RESERVED: snprintf(t, sizeof t, "%s", _("un pin reservado (la consola o BOOT)")); break;
    case AOS_MOD_E_MISSING_PIN: snprintf(t, sizeof t, "%s", _("le falta un pin (tx/rx, sda/scl, sck/mosi o pin)")); break;
    case AOS_MOD_E_DUP_PORT: snprintf(t, sizeof t, _("hay dos puertos que se llaman %s"), m->a); break;
    case AOS_MOD_E_TOO_MANY_MODULES: snprintf(t, sizeof t, _("demasiados módulos (%d como mucho)"), AOS_IO_MODULE_MAX); break;
    case AOS_MOD_E_MODULE_PORT: snprintf(t, sizeof t, "%s", _("al módulo le falta el puerto")); break;
    case AOS_MOD_E_UNKNOWN: snprintf(t, sizeof t, "%s", _("no es ni «port» ni «module»")); break;
    case AOS_MOD_W_PIN_TWICE: snprintf(t, sizeof t, _("GPIO%d está en %s y en %s"), m->gpio, m->a, m->b); break;
    case AOS_MOD_W_NO_PORT: snprintf(t, sizeof t, _("%s está en %s, que no está declarado"), m->a, m->b); break;
    case AOS_MOD_W_NOT_I2C: {
        static const char *const K[] = { "UART", "I2C", "SPI", "GPIO" };
        snprintf(t, sizeof t, _("%s va en un puerto %s, y %s no lo es"), m->a,
                 m->gpio >= 0 && m->gpio < 4 ? K[m->gpio] : "I2C", m->b);
        break;
    }
    default: scpy(t, sizeof t, m->raw); break;
    }
    if (m->line > 0) snprintf(out, n, _("Línea %d: %s"), m->line, t);
    else scpy(out, n, t);
}



/* -------------------------------------------------------------------------- */
/* The header, the ports, the file                                             */
/* -------------------------------------------------------------------------- */

static const aos_io_port_t *port_of(int gpio, const char **role)
{
    static const char *const ROLE[4][4] = { { "TX", "RX", "DE", "" }, { "SDA", "SCL", "", "" },
                                            { "SCK", "MOSI", "MISO", "CS" }, { "", "", "", "" } };
    for (int i = 0; gpio >= 0 && i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        for (int k = 0; k < 4; k++)
            if (pt->pins[k] == gpio) { if (role) *role = ROLE[pt->kind & 3][k]; return pt; }
    }
    return NULL;
}

/* The same classes, in the same order of precedence, as the app's drawing */
static const char *pin_class(const aos_io_pin_t *p)
{
    if (p->gpio < 0) return p->label[0] == '5' ? "5v" : p->label[0] == '3' ? "3v3" : p->label[0] == 'G' ? "gnd" : "rsv";
    if (aos_io_owner(p->gpio)) return "taken";
    if (p->flags & AOS_PIN_RESERVED) return "rsv";
    if (p->flags & AOS_PIN_BOARD) return "board";
    if (port_of(p->gpio, NULL)) return "port";
    if (p->flags & (AOS_PIN_STRAPPING | AOS_PIN_USB_JTAG)) return "care";
    if (p->flags & AOS_PIN_ADC) return "adc";
    return "free";
}

static cJSON *check_json(const aos_mod_check_t *ck)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", ck->ok);
    cJSON_AddNumberToObject(o, "ports", ck->ports);
    cJSON_AddNumberToObject(o, "modules", ck->modules);
    char m[200];
    if (!ck->ok) {
        fmt_msg(&ck->err, m, sizeof m);
        cJSON_AddStringToObject(o, "error", m);
        cJSON_AddNumberToObject(o, "line", ck->err.line);
    }
    cJSON *w = cJSON_AddArrayToObject(o, "warnings");
    for (int i = 0; i < ck->nwarn; i++) {
        fmt_msg(&ck->warn[i], m, sizeof m);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "msg", m);
        cJSON_AddNumberToObject(e, "line", ck->warn[i].line);
        cJSON_AddItemToArray(w, e);
    }
    return o;
}

static void api_expansion(aos_httpd_req_t *r)
{
    cJSON *o = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddArrayToObject(o, "header");
    const aos_io_pin_t *h = aos_io_header();
    for (int i = 0; i < 40; i++) {
        const aos_io_pin_t *p = &h[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "pin", p->pin);
        cJSON_AddNumberToObject(e, "gpio", p->gpio);
        cJSON_AddStringToObject(e, "label", p->label);
        cJSON_AddStringToObject(e, "note", pin_note(p->pin));
        cJSON_AddStringToObject(e, "cls", pin_class(p));
        cJSON *f = cJSON_AddArrayToObject(e, "flags");
        if (p->flags & AOS_PIN_ADC) cJSON_AddItemToArray(f, cJSON_CreateString("ADC"));
        if (p->flags & AOS_PIN_STRAPPING) cJSON_AddItemToArray(f, cJSON_CreateString("strapping"));
        if (p->flags & AOS_PIN_VO4) cJSON_AddItemToArray(f, cJSON_CreateString(_("dominio VO4")));
        if (p->flags & AOS_PIN_USB_JTAG) cJSON_AddItemToArray(f, cJSON_CreateString("USB-JTAG"));
        if (p->flags & AOS_PIN_BOARD) cJSON_AddItemToArray(f, cJSON_CreateString(_("compartido con la placa")));
        if ((p->flags & AOS_PIN_RESERVED) && p->gpio >= 0) cJSON_AddItemToArray(f, cJSON_CreateString(_("reservado")));
        const char *ow = p->gpio >= 0 ? aos_io_owner(p->gpio) : NULL, *role = "";
        if (ow) cJSON_AddStringToObject(e, "owner", ow);
        const aos_io_port_t *pt = port_of(p->gpio, &role);
        if (pt) { cJSON_AddStringToObject(e, "port", pt->name); cJSON_AddStringToObject(e, "role", role); }
        cJSON_AddItemToArray(hdr, e);
    }
    static const char *const KIND[4] = { "uart", "i2c", "spi", "gpio" };
    static const char *const ROLE[4][4] = { { "TX", "RX", "DE", "" }, { "SDA", "SCL", "", "" },
                                            { "SCK", "MOSI", "MISO", "" }, { "GPIO", "", "", "" } };
    cJSON *ports = cJSON_AddArrayToObject(o, "ports");
    for (int i = 0; i < aos_io_port_count(); i++) {
        const aos_io_port_t *pt = aos_io_port_at(i);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", pt->name);
        cJSON_AddStringToObject(e, "kind", KIND[pt->kind & 3]);
        cJSON_AddNumberToObject(e, "freq", pt->freq);
        cJSON *ps = cJSON_AddArrayToObject(e, "pins");
        const char *owner = NULL;
        for (int k = 0; k < 4; k++) {
            if (pt->pins[k] < 0) continue;
            const aos_io_pin_t *pin = aos_io_pin_of_gpio(pt->pins[k]);
            cJSON *pe = cJSON_CreateObject();
            cJSON_AddStringToObject(pe, "role", ROLE[pt->kind & 3][k]);
            cJSON_AddNumberToObject(pe, "gpio", pt->pins[k]);
            cJSON_AddNumberToObject(pe, "pin", pin ? pin->pin : 0);
            cJSON_AddItemToArray(ps, pe);
            if (!owner) owner = aos_io_owner(pt->pins[k]);
        }
        if (owner) cJSON_AddStringToObject(e, "owner", owner);
        cJSON_AddItemToArray(ports, e);
    }
    cJSON *mods = cJSON_AddArrayToObject(o, "modules");
    for (int i = 0; i < aos_io_module_count(); i++) {
        const aos_io_module_t *m = aos_io_module_at(i);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", m->name);
        cJSON_AddStringToObject(e, "port", m->port);
        cJSON_AddStringToObject(e, "args", m->args);
        char cs[24] = "";
        const char *cp = strstr(m->args, "chip=");
        if (cp) sscanf(cp + 5, "%23[^ \t#]", cs);
        const aos_sensor_chip_t *sc = aos_sensor_chip_find(cs[0] ? cs : m->name);
        if (sc) cJSON_AddStringToObject(e, "chip", sc->name);
        cJSON_AddItemToArray(mods, e);
    }
    bool from_file = false;
    char *text = aos_modules_read(&from_file);
    cJSON_AddBoolToObject(o, "file", from_file);
    cJSON_AddBoolToObject(o, "card", aos_hal_path_sd_root() != NULL);
    if (text) {
        cJSON_AddStringToObject(o, "text", text);
        aos_mod_check_t ck;
        aos_modules_check(text, strlen(text), &ck);
        cJSON_AddItemToObject(o, "check", check_json(&ck));
        free(text);
    }
    cJSON *cats = cJSON_AddArrayToObject(o, "cats");
    for (int k = 0; k < AOS_SENSOR_CAT_COUNT; k++) cJSON_AddItemToArray(cats, cJSON_CreateString(aos_sensor_cat_name(k)));
    cJSON *chips = cJSON_AddArrayToObject(o, "chips");
    for (int i = 0; i < aos_sensor_chip_count(); i++) {
        const aos_sensor_chip_t *c = aos_sensor_chip_at(i);
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", c->id);
        cJSON_AddStringToObject(e, "name", c->name);
        cJSON *a = cJSON_AddArrayToObject(e, "addrs");
        for (int k = 0; k < c->naddr; k++) cJSON_AddItemToArray(a, cJSON_CreateNumber(c->addrs[k]));
        cJSON_AddBoolToObject(e, "shunt", c->shunt);
        cJSON_AddBoolToObject(e, "gain", c->gain);
        cJSON_AddBoolToObject(e, "spi", c->spi);
        cJSON_AddBoolToObject(e, "onewire", c->onewire);
        cJSON_AddNumberToObject(e, "bus", c->bus);
        cJSON_AddNumberToObject(e, "pins", c->pins);
        cJSON_AddNumberToObject(e, "cat", aos_sensor_chip_cat(c));
        cJSON_AddItemToArray(chips, e);
    }
    send_cjson(r, 200, o);
}

static void api_check_or_save(aos_httpd_req_t *r, bool save)
{
    char *b = aos_httpd_body_all(r, 8192);
    if (!b) { send_err(r, 400, _("falta el texto (8 KB como mucho)")); return; }
    aos_mod_check_t ck;
    bool ok = save ? aos_modules_save(b, strlen(b), &ck) : (aos_modules_check(b, strlen(b), &ck), ck.ok);
    free(b);
    cJSON *o = check_json(&ck);
    if (save) {
        cJSON_AddBoolToObject(o, "saved", ok);
        if (!ok && ck.ok) cJSON_AddStringToObject(o, "error", aos_hal_path_sd_root() ? _("no se pudo escribir en la tarjeta") : _("no hay tarjeta"));
    }
    send_cjson(r, 200, o);
}

/* -------------------------------------------------------------------------- */
/* Sensors                                                                     */
/* -------------------------------------------------------------------------- */

static void api_sensors(aos_httpd_req_t *r)
{
    aos_sensors_keep();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "seq", aos_sensors_seq());
    uint32_t now = (uint32_t)aos_hal_uptime_ms();
    cJSON *arr = cJSON_AddArrayToObject(o, "sensors");
    static const char *const ST[4] = { "wait", "ok", "err", "busy" };
    int n = aos_sensor_count();
    for (int i = 0; i < n; i++) {
        aos_sensor_t s;
        if (!aos_sensor_at(i, &s)) continue;
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "name", s.name);
        cJSON_AddStringToObject(e, "chip", s.chip);
        cJSON_AddStringToObject(e, "port", s.port);
        cJSON_AddNumberToObject(e, "addr", s.addr);
        /* an I2C address, or an SPI chip's chip select */
        const aos_io_port_t *sp = aos_io_port_find(s.port);
        char where[8];
        if (sp && sp->kind == AOS_PORT_SPI) snprintf(where, sizeof where, "CS%u", (unsigned)s.addr);
        else if (sp && sp->kind == AOS_PORT_GPIO) snprintf(where, sizeof where, "GPIO%u", (unsigned)s.addr);
        else if (sp && sp->kind == AOS_PORT_UART) where[0] = 0;
        else snprintf(where, sizeof where, "0x%02X", s.addr);
        cJSON_AddStringToObject(e, "where", where);
        cJSON_AddBoolToObject(e, "declared", s.declared);
        cJSON_AddStringToObject(e, "state", ST[s.state & 3]);
        if (s.err[0]) {
            char m[96];
            aos_sensor_err_text(s.err, m, sizeof m);
            cJSON_AddStringToObject(e, "err", s.err);
            cJSON_AddStringToObject(e, "msg", m);
        }
        cJSON_AddNumberToObject(e, "reads", s.reads);
        cJSON_AddNumberToObject(e, "errors", s.errors);
        if (s.t_ms) cJSON_AddNumberToObject(e, "age_ms", now - s.t_ms);
        cJSON *vs = cJSON_AddArrayToObject(e, "values");
        aos_sensor_value_t xv[AOS_SENSOR_XVALUES];
        int xn = aos_sensor_values(i, xv, AOS_SENSOR_XVALUES);
        for (int k = 0; k < xn; k++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "key", xv[k].key);
            cJSON_AddStringToObject(v, "name", aos_sensor_key_name(xv[k].key));
            cJSON_AddNumberToObject(v, "q", xv[k].q);
            cJSON_AddStringToObject(v, "unit", aos_sensor_unit(xv[k].q));
            add_num(v, "v", s.state == AOS_SENSOR_OK ? xv[k].v : NAN);
            cJSON_AddItemToArray(vs, v);
        }
        cJSON_AddItemToArray(arr, e);
    }
    aos_sensor_cand_t c[AOS_SENSOR_CAND_MAX];
    int nc = aos_sensor_candidates(c, AOS_SENSOR_CAND_MAX);
    cJSON *ca = cJSON_AddArrayToObject(o, "candidates");
    for (int i = 0; i < nc; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "port", c[i].port);
        cJSON_AddNumberToObject(e, "addr", c[i].addr);
        cJSON_AddStringToObject(e, "chip", c[i].chip);
        cJSON_AddStringToObject(e, "guess", c[i].guess);
        cJSON_AddItemToArray(ca, e);
    }
    aos_sensor_port_t ps[AOS_IO_PORT_MAX];
    int np = aos_sensors_ports(ps, AOS_IO_PORT_MAX);
    cJSON *pa = cJSON_AddArrayToObject(o, "ports");
    for (int i = 0; i < np; i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "port", ps[i].port);
        cJSON_AddStringToObject(e, "state", ST[ps[i].state & 3]);
        if (ps[i].owner[0]) cJSON_AddStringToObject(e, "owner", ps[i].owner);
        cJSON_AddNumberToObject(e, "found", ps[i].found);
        if (ps[i].scanned) cJSON_AddNumberToObject(e, "scan_age_ms", now - ps[i].scan_ms);
        cJSON_AddItemToArray(pa, e);
    }
    send_cjson(r, 200, o);
}

static void api_hist(aos_httpd_req_t *r)
{
    int i = (int)aos_httpd_query_int(r, "i", -1), k = (int)aos_httpd_query_int(r, "k", 0);
    float *h = malloc(sizeof(float) * AOS_SENSOR_HIST);
    if (!h) { send_err(r, 500, _("sin memoria")); return; }
    int n = aos_sensor_history(i, k, h, AOS_SENSOR_HIST);
    /* by hand: 300 numbers through cJSON cost more than the page wants to wait */
    size_t cap = (size_t)n * 12 + 32;
    char *out = malloc(cap);
    if (!out) { free(h); send_err(r, 500, _("sin memoria")); return; }
    size_t o = (size_t)snprintf(out, cap, "{\"v\":[");
    for (int j = 0; j < n && o + 14 < cap; j++) {
        if (isnan(h[j])) o += (size_t)snprintf(out + o, cap - o, "%snull", j ? "," : "");
        else o += (size_t)snprintf(out + o, cap - o, "%s%.5g", j ? "," : "", h[j]);
    }
    snprintf(out + o, cap - o, "]}");
    aos_httpd_send_json(r, 200, out);
    free(out);
    free(h);
}

/* The workshop's drivers on the board, one after the other, each opened
 * and closed again: what an app will meet, without the app. */
static void api_selftest(aos_httpd_req_t *r)
{
    static const char OWNER[] = "prueba";
    int g = (int)aos_httpd_query_int(r, "gpio", 28), g2 = (int)aos_httpd_query_int(r, "gpio2", -1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "gpio", g);

    cJSON *pw = cJSON_AddObjectToObject(o, "pwm");
    aos_io_pwm_t *p = aos_io_pwm_open(g, 1000, OWNER);
    cJSON_AddBoolToObject(pw, "open", p != NULL);
    if (p) {
        cJSON_AddNumberToObject(pw, "hz_1000", aos_io_pwm_freq(p));
        cJSON_AddNumberToObject(pw, "bits_1000", aos_io_pwm_bits(p));
        cJSON_AddBoolToObject(pw, "duty", aos_io_pwm_set_duty(p, 0.5f));
        cJSON_AddBoolToObject(pw, "fade", aos_io_pwm_fade(p, 0.1f, 50));
        aos_hal_sleep_ms(60);
        cJSON_AddBoolToObject(pw, "set_freq", aos_io_pwm_set_freq(p, 50));
        cJSON_AddNumberToObject(pw, "hz_50", aos_io_pwm_freq(p));
        cJSON_AddNumberToObject(pw, "bits_50", aos_io_pwm_bits(p));
        cJSON_AddBoolToObject(pw, "servo", aos_io_pwm_set_pulse_us(p, 1500));
        cJSON_AddNumberToObject(pw, "servo_duty", aos_io_pwm_duty(p));
        aos_io_pwm_close(p);
    }

    cJSON *da = cJSON_AddObjectToObject(o, "dac");
    aos_io_dac_t *d = aos_io_dac_open(g, OWNER);
    cJSON_AddBoolToObject(da, "open", d != NULL);
    if (d) {
        cJSON_AddBoolToObject(da, "set", aos_io_dac_set(d, 0.5f));
        aos_io_dac_close(d);
    }

    cJSON *ir = cJSON_AddObjectToObject(o, "ir");
    aos_io_ir_t *rx = g2 >= 0 ? aos_io_ir_rx_open(g2, 0, OWNER) : NULL;
    aos_io_ir_t *tx = aos_io_ir_tx_open(g, OWNER);
    cJSON_AddBoolToObject(ir, "tx_open", tx != NULL);
    if (g2 >= 0) cJSON_AddBoolToObject(ir, "rx_open", rx != NULL);
    if (tx) {
        /* NEC, address 0x04 command 0x08, sent as plain levels so a jumper
         * carries it to the input */
        uint16_t f[67];
        int n = 0;
        f[n++] = 9000; f[n++] = 4500;
        uint32_t word = 0x04u | 0xFBu << 8 | 0x08u << 16 | 0xF7u << 24;
        for (int b = 0; b < 32; b++) { f[n++] = 560; f[n++] = (word >> b & 1) ? 1690 : 560; }
        f[n++] = 560;
        if (rx) aos_io_ir_set_carrier(tx, 0, 50);
        uint64_t t0 = aos_hal_uptime_ms();
        cJSON_AddBoolToObject(ir, "send", aos_io_ir_send(tx, f, n));
        cJSON_AddNumberToObject(ir, "send_ms", (double)(aos_hal_uptime_ms() - t0));
        if (rx) {
            uint16_t *got = malloc(sizeof(uint16_t) * 128);
            int k = got ? aos_io_ir_read(rx, got, 128, 200) : -1;
            cJSON_AddNumberToObject(ir, "read", k);
            if (k > 1) { cJSON_AddNumberToObject(ir, "first_mark", got[0]); cJSON_AddNumberToObject(ir, "first_space", got[1]); }
            free(got);
        }
        aos_io_ir_close(tx);
    }
    aos_io_ir_close(rx);

    cJSON *ca = cJSON_AddObjectToObject(o, "can");
    aos_io_can_t *c = aos_io_can_open(g, g, 500000, AOS_CAN_SELFTEST, OWNER);
    cJSON_AddBoolToObject(ca, "open", c != NULL);
    if (c) {
        int sent = 0, back = 0;
        for (int i = 0; i < 3; i++) {
            aos_can_frame_t f = { .id = 0x100 + (uint32_t)i, .len = 4, .data = { 0xCA, 0xFE, 0, (uint8_t)i } };
            sent += aos_io_can_send(c, &f, 100);
        }
        aos_can_frame_t e = { .id = 0x18FEF100, .ext = true, .len = 8, .data = { 1, 2, 3, 4, 5, 6, 7, 8 } };
        sent += aos_io_can_send(c, &e, 100);
        aos_can_frame_t f;
        uint32_t last_id = 0;
        while (aos_io_can_recv(c, &f, 50) == 1) { back++; last_id = f.id; }
        cJSON_AddNumberToObject(ca, "sent", sent);
        cJSON_AddNumberToObject(ca, "received", back);
        cJSON_AddNumberToObject(ca, "last_id", last_id);
        aos_can_status_t st;
        if (aos_io_can_status(c, &st)) {
            cJSON_AddStringToObject(ca, "state", st.state);
            cJSON_AddNumberToObject(ca, "tx_errors", st.tx_errors);
            cJSON_AddNumberToObject(ca, "bus_errors", st.bus_errors);
        }
        aos_io_can_close(c);
    }
    send_cjson(r, 200, o);
}

bool aos_portal_modules(aos_httpd_req_t *r, const char *method, const char *p)
{
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (get && !strcmp(p, "expansion")) api_expansion(r);
    else if (post && !strcmp(p, "expansion/check")) api_check_or_save(r, false);
    else if (post && !strcmp(p, "expansion/save")) api_check_or_save(r, true);
    else if (get && !strcmp(p, "sensors")) api_sensors(r);
    else if (get && !strcmp(p, "sensors/hist")) api_hist(r);
    else if (post && !strcmp(p, "expansion/selftest")) api_selftest(r);
    else if (post && !strcmp(p, "sensors/rescan")) { aos_sensors_rescan(); aos_httpd_send_json(r, 200, "{\"ok\":true}"); }
    else return false;
    return true;
}
