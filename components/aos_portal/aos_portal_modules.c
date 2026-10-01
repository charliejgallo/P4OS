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
 *
 * The words are Spanish, like the rest of the portal (the Módulos app says
 * the same through the catalogs).
 */
#include "aos_portal_modules.h"
#include "aos_hal.h"
#include "aos_io.h"
#include "aos_modules.h"
#include "aos_sensors.h"
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
    case 1:  return "VCC_5V: vivo aun con la placa apagada";
    case 2:  return "Buck de 3 A: se apaga con POWER";
    case 4:  return "SDA del bus I2C de la placa (táctil, códecs)";
    case 6:  return "SCL del bus I2C de la placa (táctil, códecs)";
    case 7:  return "UART0 TX al CH343: la consola";
    case 8:  return "La INT del táctil, si R108 está puesta";
    case 9:  return "UART0 RX desde el CH343: la consola";
    case 11: return "JTAG MTDO";
    case 12: return "JTAG MTCK";
    case 14: return "JTAG MTDI";
    case 15: return "ADC1 canal 5";
    case 17: return "ADC1 canal 6";
    case 21: return "USB-Serial-JTAG D−: libre si no se usa";
    case 23: return "USB-Serial-JTAG D+: libre si no se usa";
    case 25: case 27: return "USB 2.0 HS, en paralelo con el puerto OTG: no es un GPIO";
    case 28: return "Strapping de la fuente del JTAG: usable después del arranque";
    case 30: return "BOOT: strapping, con pull-up de 4,7K";
    case 32: return "ADC2 canal 0";
    case 34: return "ADC2 canal 1";
    case 35: return "Dominio VO4: medí la tensión antes de usarlo";
    case 36: return "ADC2 canal 2, comparador";
    case 37: case 39: return "Dominio VO4";
    case 38: return "ADC2 canal 3, comparador";
    default: return "";
    }
}

static void fmt_msg(const aos_mod_msg_t *m, char *out, size_t n)
{
    char t[128];
    switch (m->code) {
    case AOS_MOD_E_LONG: snprintf(t, sizeof t, "la línea es demasiado larga"); break;
    case AOS_MOD_E_SYNTAX: snprintf(t, sizeof t, "se esperaba «port» o «module», un nombre y lo demás"); break;
    case AOS_MOD_E_TOO_MANY_PORTS: snprintf(t, sizeof t, "demasiados puertos (%d como mucho)", AOS_IO_PORT_MAX); break;
    case AOS_MOD_E_PORT_NAME: snprintf(t, sizeof t, "el nombre del puerto es demasiado largo"); break;
    case AOS_MOD_E_PORT_KIND: snprintf(t, sizeof t, "los puertos se llaman uart…, i2c…, spi… o gpio…"); break;
    case AOS_MOD_E_NOT_HEADER: snprintf(t, sizeof t, "un GPIO que no está en el header"); break;
    case AOS_MOD_E_RESERVED: snprintf(t, sizeof t, "un pin reservado (la consola o BOOT)"); break;
    case AOS_MOD_E_MISSING_PIN: snprintf(t, sizeof t, "le falta un pin (tx/rx, sda/scl, sck/mosi o pin)"); break;
    case AOS_MOD_E_DUP_PORT: snprintf(t, sizeof t, "hay dos puertos que se llaman %s", m->a); break;
    case AOS_MOD_E_TOO_MANY_MODULES: snprintf(t, sizeof t, "demasiados módulos (%d como mucho)", AOS_IO_MODULE_MAX); break;
    case AOS_MOD_E_MODULE_PORT: snprintf(t, sizeof t, "al módulo le falta el puerto"); break;
    case AOS_MOD_E_UNKNOWN: snprintf(t, sizeof t, "no es ni «port» ni «module»"); break;
    case AOS_MOD_W_PIN_TWICE: snprintf(t, sizeof t, "GPIO%d está en %s y en %s", m->gpio, m->a, m->b); break;
    case AOS_MOD_W_NO_PORT: snprintf(t, sizeof t, "%s está en %s, que no está declarado", m->a, m->b); break;
    case AOS_MOD_W_NOT_I2C: snprintf(t, sizeof t, "%s es un sensor I2C y %s no es un puerto I2C", m->a, m->b); break;
    default: scpy(t, sizeof t, m->raw); break;
    }
    if (m->line > 0) snprintf(out, n, "Línea %d: %s", m->line, t);
    else scpy(out, n, t);
}

static void fmt_err(const char *err, char *out, size_t n)
{
    const char *d = strchr(err, ':');
    d = d ? d + 1 : "";
    if (!strncmp(err, "nack", 4)) snprintf(out, n, "No contesta en esa dirección");
    else if (!strncmp(err, "crc", 3)) snprintf(out, n, "Llegó una lectura con el CRC mal");
    else if (!strncmp(err, "id", 2)) snprintf(out, n, "En esa dirección hay otro chip (dice %s)", d);
    else if (!strncmp(err, "notready", 8)) snprintf(out, n, "Todavía no midió");
    else if (!strncmp(err, "busy", 4)) snprintf(out, n, "Los pines los tiene %s", d);
    else if (!strncmp(err, "port", 4)) snprintf(out, n, "No se pudo abrir el puerto");
    else if (!strncmp(err, "board", 5)) snprintf(out, n, "Ahí está el %s de la placa", d);
    else scpy(out, n, err);
}

static const char *key_name(const char *key)
{
    static const char *const K[][2] = { { "temp", "Temperatura" }, { "hum", "Humedad" }, { "press", "Presión" },
                                        { "lux", "Luz" }, { "bus", "Tensión" }, { "current", "Corriente" },
                                        { "power", "Potencia" }, { "shunt", "En el shunt" }, { "a0", "A0" },
                                        { "a1", "A1" }, { "a2", "A2" }, { "a3", "A3" } };
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++) if (!strcmp(K[i][0], key)) return K[i][1];
    return key;
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
    if (p->flags & (AOS_PIN_STRAPPING | AOS_PIN_VO4 | AOS_PIN_USB_JTAG)) return "care";
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
        if (p->flags & AOS_PIN_VO4) cJSON_AddItemToArray(f, cJSON_CreateString("dominio VO4"));
        if (p->flags & AOS_PIN_USB_JTAG) cJSON_AddItemToArray(f, cJSON_CreateString("USB-JTAG"));
        if (p->flags & AOS_PIN_BOARD) cJSON_AddItemToArray(f, cJSON_CreateString("compartido con la placa"));
        if ((p->flags & AOS_PIN_RESERVED) && p->gpio >= 0) cJSON_AddItemToArray(f, cJSON_CreateString("reservado"));
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
        cJSON_AddItemToArray(chips, e);
    }
    send_cjson(r, 200, o);
}

static void api_check_or_save(aos_httpd_req_t *r, bool save)
{
    char *b = aos_httpd_body_all(r, 8192);
    if (!b) { send_err(r, 400, "falta el texto (8 KB como mucho)"); return; }
    aos_mod_check_t ck;
    bool ok = save ? aos_modules_save(b, strlen(b), &ck) : (aos_modules_check(b, strlen(b), &ck), ck.ok);
    free(b);
    cJSON *o = check_json(&ck);
    if (save) {
        cJSON_AddBoolToObject(o, "saved", ok);
        if (!ok && ck.ok) cJSON_AddStringToObject(o, "error", aos_hal_path_sd_root() ? "no se pudo escribir en la tarjeta" : "no hay tarjeta");
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
        cJSON_AddBoolToObject(e, "declared", s.declared);
        cJSON_AddStringToObject(e, "state", ST[s.state & 3]);
        if (s.err[0]) {
            char m[96];
            fmt_err(s.err, m, sizeof m);
            cJSON_AddStringToObject(e, "err", s.err);
            cJSON_AddStringToObject(e, "msg", m);
        }
        cJSON_AddNumberToObject(e, "reads", s.reads);
        cJSON_AddNumberToObject(e, "errors", s.errors);
        if (s.t_ms) cJSON_AddNumberToObject(e, "age_ms", now - s.t_ms);
        cJSON *vs = cJSON_AddArrayToObject(e, "values");
        for (int k = 0; k < s.n; k++) {
            cJSON *v = cJSON_CreateObject();
            cJSON_AddStringToObject(v, "key", s.v[k].key);
            cJSON_AddStringToObject(v, "name", key_name(s.v[k].key));
            cJSON_AddNumberToObject(v, "q", s.v[k].q);
            cJSON_AddStringToObject(v, "unit", aos_sensor_unit(s.v[k].q));
            add_num(v, "v", s.state == AOS_SENSOR_OK ? s.v[k].v : NAN);
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
    if (!h) { send_err(r, 500, "sin memoria"); return; }
    int n = aos_sensor_history(i, k, h, AOS_SENSOR_HIST);
    /* by hand: 300 numbers through cJSON cost more than the page wants to wait */
    size_t cap = (size_t)n * 12 + 32;
    char *out = malloc(cap);
    if (!out) { free(h); send_err(r, 500, "sin memoria"); return; }
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

bool aos_portal_modules(aos_httpd_req_t *r, const char *method, const char *p)
{
    bool get = !strcmp(method, "GET"), post = !strcmp(method, "POST");
    if (get && !strcmp(p, "expansion")) api_expansion(r);
    else if (post && !strcmp(p, "expansion/check")) api_check_or_save(r, false);
    else if (post && !strcmp(p, "expansion/save")) api_check_or_save(r, true);
    else if (get && !strcmp(p, "sensors")) api_sensors(r);
    else if (get && !strcmp(p, "sensors/hist")) api_hist(r);
    else if (post && !strcmp(p, "sensors/rescan")) { aos_sensors_rescan(); aos_httpd_send_json(r, 200, "{\"ok\":true}"); }
    else return false;
    return true;
}
