/*
 * P4OS - CAN: the OBD-II scanner.
 *
 * What every car since about 2001-2008 (OBD-II in the US, EOBD in Europe,
 * OBDBr in Brazil, all on CAN since 2008) answers, whatever its make: the
 * "current data" of SAE J1979 / ISO 15031-5, mode 01, on ISO 15765-4. A
 * question goes to 0x7DF, "every emissions unit", as a single ISO-TP frame:
 *
 *     7DF  02 01 0C 00 00 00 00 00      length 2, mode 01, PID 0C
 *
 * and each unit that knows that PID answers from 0x7E8-0x7EF (the engine is
 * usually 7E8, the gearbox 7E9) with the mode plus 0x40:
 *
 *     7E8  04 41 0C 1A F8 00 00 00      length 4, 41, PID 0C, A = 1A, B = F8
 *
 * With 29-bit ids the question goes to 18DB33F1 and the answers come from
 * 18DAF1xx, xx the unit's address.
 *
 * First the scanner asks PID 00, whose answer is a map of the PIDs 01-20 the
 * unit knows (bit 7 of A is PID 01, bit 0 of D is PID 20); if 20 is there,
 * PID 20 maps 21-40, and so on up to E0. Then it goes round the PIDs it
 * knows, one question at a time: the next one goes when the answer came (a
 * little after, so that a second unit can answer too) or after a timeout,
 * and never sooner than GAP_US after the last, so the bus sees some 20
 * questions a second at most.
 *
 * The bus thread runs it (cn_obd_step_locked, cn_obd_rx_locked, with the
 * lock held); the UI only starts, stops and reads it. Answers are decoded
 * whatever asked them: a question sent by hand from "Enviar" shows up here
 * too. Asking needs the Normal mode (the board has to send), and the app
 * opens in Solo escucha, which the tab says.
 *
 * The formulas are the public ones of J1979 (Wikipedia's "OBD-II PIDs"
 * lists them too), written here by hand; no PID database was copied.
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <stdlib.h>
#include <string.h>

#define REQ_STD      0x7DF
#define REQ_EXT      0x18DB33F1u
#define GAP_US       40000      /* between two questions */
#define WAIT_SUP_US  300000     /* for the maps of supported PIDs */
#define WAIT_VAL_US  150000
#define MORE_SUP_US  80000      /* after the first answer to a map, for the other units */
#define MORE_VAL_US  15000
#define ECU_MAX      8

/* ---- the PIDs it knows: value = raw x k + o, raw the PID's bytes, A first ---- */

enum { K_LIN = 0, K_STATUS, K_STD, K_FUEL };

typedef struct {
    uint8_t pid, nb, kind;
    const char *name, *unit;
    float k, o;
} obd_def_t;

#define PCT  (100.0f / 255.0f)
#define TRIM (100.0f / 128.0f)

static const obd_def_t DEFS[] = {
    { 0x01, 4, K_STATUS, N_("Luz de falla y códigos"), "", 1, 0 },
    { 0x04, 1, K_LIN, N_("Carga calculada"), "%", PCT, 0 },
    { 0x05, 1, K_LIN, N_("Refrigerante"), "\xC2\xB0" "C", 1, -40 },
    { 0x06, 1, K_LIN, N_("Corrección corta, banco 1"), "%", TRIM, -100 },
    { 0x07, 1, K_LIN, N_("Corrección larga, banco 1"), "%", TRIM, -100 },
    { 0x08, 1, K_LIN, N_("Corrección corta, banco 2"), "%", TRIM, -100 },
    { 0x09, 1, K_LIN, N_("Corrección larga, banco 2"), "%", TRIM, -100 },
    { 0x0A, 1, K_LIN, N_("Presión de combustible"), "kPa", 3, 0 },
    { 0x0B, 1, K_LIN, N_("Presión del múltiple"), "kPa", 1, 0 },
    { 0x0C, 2, K_LIN, N_("Régimen del motor"), "rpm", 0.25f, 0 },
    { 0x0D, 1, K_LIN, N_("Velocidad"), "km/h", 1, 0 },
    { 0x0E, 1, K_LIN, N_("Avance del encendido"), "\xC2\xB0", 0.5f, -64 },
    { 0x0F, 1, K_LIN, N_("Aire de admisión"), "\xC2\xB0" "C", 1, -40 },
    { 0x10, 2, K_LIN, N_("Caudal de aire (MAF)"), "g/s", 0.01f, 0 },
    { 0x11, 1, K_LIN, N_("Mariposa"), "%", PCT, 0 },
    { 0x14, 1, K_LIN, N_("Sonda de oxígeno, banco 1, 1"), "V", 0.005f, 0 },
    { 0x15, 1, K_LIN, N_("Sonda de oxígeno, banco 1, 2"), "V", 0.005f, 0 },
    { 0x1C, 1, K_STD, N_("Norma OBD"), "", 1, 0 },
    { 0x1F, 2, K_LIN, N_("Desde el arranque"), "s", 1, 0 },
    { 0x21, 2, K_LIN, N_("Recorrido con la luz de falla"), "km", 1, 0 },
    { 0x22, 2, K_LIN, N_("Presión del riel (relativa)"), "kPa", 0.079f, 0 },
    { 0x23, 2, K_LIN, N_("Presión del riel"), "kPa", 10, 0 },
    { 0x2C, 1, K_LIN, N_("EGR comandada"), "%", PCT, 0 },
    { 0x2D, 1, K_LIN, N_("Error de la EGR"), "%", TRIM, -100 },
    { 0x2E, 1, K_LIN, N_("Purga del cánister"), "%", PCT, 0 },
    { 0x2F, 1, K_LIN, N_("Nivel de combustible"), "%", PCT, 0 },
    { 0x30, 1, K_LIN, N_("Calentamientos desde el borrado"), "", 1, 0 },
    { 0x31, 2, K_LIN, N_("Recorrido desde el borrado"), "km", 1, 0 },
    { 0x33, 1, K_LIN, N_("Presión barométrica"), "kPa", 1, 0 },
    { 0x3C, 2, K_LIN, N_("Catalizador, banco 1, 1"), "\xC2\xB0" "C", 0.1f, -40 },
    { 0x3D, 2, K_LIN, N_("Catalizador, banco 2, 1"), "\xC2\xB0" "C", 0.1f, -40 },
    { 0x42, 2, K_LIN, N_("Tensión del módulo"), "V", 0.001f, 0 },
    { 0x43, 2, K_LIN, N_("Carga absoluta"), "%", PCT, 0 },
    { 0x44, 2, K_LIN, N_("Lambda comandada"), "", 1.0f / 32768.0f, 0 },
    { 0x45, 1, K_LIN, N_("Mariposa relativa"), "%", PCT, 0 },
    { 0x46, 1, K_LIN, N_("Temperatura ambiente"), "\xC2\xB0" "C", 1, -40 },
    { 0x47, 1, K_LIN, N_("Mariposa absoluta B"), "%", PCT, 0 },
    { 0x49, 1, K_LIN, N_("Pedal D"), "%", PCT, 0 },
    { 0x4A, 1, K_LIN, N_("Pedal E"), "%", PCT, 0 },
    { 0x4C, 1, K_LIN, N_("Mariposa comandada"), "%", PCT, 0 },
    { 0x4D, 2, K_LIN, N_("Tiempo con la luz de falla"), "min", 1, 0 },
    { 0x4E, 2, K_LIN, N_("Tiempo desde el borrado"), "min", 1, 0 },
    { 0x51, 1, K_FUEL, N_("Combustible"), "", 1, 0 },
    { 0x52, 1, K_LIN, N_("Etanol"), "%", PCT, 0 },
    { 0x59, 2, K_LIN, N_("Presión del riel (absoluta)"), "kPa", 10, 0 },
    { 0x5A, 1, K_LIN, N_("Pedal relativo"), "%", PCT, 0 },
    { 0x5B, 1, K_LIN, N_("Batería del híbrido"), "%", PCT, 0 },
    { 0x5C, 1, K_LIN, N_("Aceite"), "\xC2\xB0" "C", 1, -40 },
    { 0x5D, 2, K_LIN, N_("Inyección"), "\xC2\xB0", 1.0f / 128.0f, -210 },
    { 0x5E, 2, K_LIN, N_("Consumo"), "L/h", 0.05f, 0 },
    { 0x61, 1, K_LIN, N_("Par pedido"), "%", 1, -125 },
    { 0x62, 1, K_LIN, N_("Par real"), "%", 1, -125 },
    { 0x63, 2, K_LIN, N_("Par de referencia"), "Nm", 1, 0 },
    { 0xA6, 4, K_LIN, N_("Odómetro"), "km", 0.1f, 0 },
};
#define NDEFS ((int)(sizeof DEFS / sizeof DEFS[0]))

static const obd_def_t *def_of(int pid)
{
    for (int i = 0; i < NDEFS; i++)
        if (DEFS[i].pid == pid) return &DEFS[i];
    return NULL;
}

static bool is_map(int pid) { return (pid & 0x1F) == 0; }

/* ---- the state (cn_lock) ---- */

enum { ST_IDLE = 0, ST_MAPS, ST_LIVE, ST_NOANSWER, ST_NOTNORMAL, ST_NOACK };

typedef struct {
    uint8_t d[4], n;
    uint8_t ecu;
    bool    have;
    uint64_t t_us;
} obd_val_t;

static struct {
    bool     on, ext;
    int      st;
    bool     maps_done;
    uint8_t  asking;
    bool     waiting, answered;
    int      tries;
    uint64_t asked_us, answer_us, next_us;
    uint32_t sup[8];                    /* bit p & 31 of sup[p >> 5]: PID p, any unit */
    int      necu;
    uint32_t ecu[ECU_MAX];              /* the answers' ids */
    uint32_t gen;                       /* bumped when the PIDs or the units change */
    uint32_t asked, answers;
    obd_val_t v[256];
} O;

static bool sup_has(int p) { return O.sup[p >> 5] >> (p & 31) & 1; }

void cn_obd_stop_locked(void)
{
    if (O.on) O.st = ST_NOACK;
    O.on = false;
    O.waiting = false;
}

static int ecu_index(uint32_t id)
{
    for (int i = 0; i < O.necu; i++)
        if (O.ecu[i] == id) return i;
    if (O.necu >= ECU_MAX) return -1;
    /* kept sorted, 7E8 before 7E9 */
    int at = O.necu;
    while (at > 0 && O.ecu[at - 1] > id) at--;
    memmove(&O.ecu[at + 1], &O.ecu[at], sizeof(uint32_t) * (size_t)(O.necu - at));
    for (int p = 0; p < 256; p++)
        if (O.v[p].have && O.v[p].ecu >= at) O.v[p].ecu++;
    O.ecu[at] = id;
    O.necu++;
    O.gen++;
    return at;
}

void cn_obd_rx_locked(const cn_frame_t *f)
{
    if (f->fl & (CN_TX | CN_RTR) || f->len < 3) return;
    bool ext = f->fl & CN_EXT;
    if (ext ? (f->id & 0x1FFFFF00u) != 0x18DAF100u : (f->id < 0x7E8 || f->id > 0x7EF)) return;
    int L = f->data[0];
    if (L < 2 || L > 7 || L + 1 > f->len || f->data[1] != 0x41) return;    /* a single frame of mode 01 */
    int pid = f->data[2], n = L - 2;
    int e = ecu_index(f->id);
    if (e < 0) return;
    O.answers++;
    if (O.waiting && pid == O.asking && !O.answered) {
        O.answered = true;
        O.answer_us = cn_us();
    }
    if (is_map(pid)) {
        if (n < 4) return;
        uint32_t bits = (uint32_t)f->data[3] << 24 | (uint32_t)f->data[4] << 16 | (uint32_t)f->data[5] << 8 | f->data[6];
        for (int i = 0; i < 32; i++) {
            int p = pid + 1 + i;
            if (p > 255 || !(bits >> (31 - i) & 1) || sup_has(p)) continue;
            O.sup[p >> 5] |= 1u << (p & 31);
            O.gen++;
        }
        return;
    }
    obd_val_t *v = &O.v[pid];
    /* two units with the same PID: the lower id wins while it keeps answering */
    if (v->have && e > v->ecu && f->t_us - v->t_us < 2000000) return;
    if (!v->have) O.gen++;
    v->n = (uint8_t)(n > 4 ? 4 : n);
    memset(v->d, 0, sizeof v->d);
    memcpy(v->d, &f->data[3], v->n);
    v->ecu = (uint8_t)e;
    v->t_us = f->t_us;
    v->have = true;
}

static int next_pid(int after)
{
    for (int k = 1; k <= 255; k++) {
        int p = (after + k) & 0xFF;
        if (p && !is_map(p) && sup_has(p)) return p;
    }
    return -1;
}

bool cn_obd_step_locked(uint64_t now, cn_frame_t *out)
{
    if (!O.on) return false;
    if (CB.cfg.mode != CN_MODE_NORMAL) {
        O.on = false;
        O.st = ST_NOTNORMAL;
        return false;
    }
    if (O.waiting) {
        bool maps = !O.maps_done;
        bool done = (O.answered && now - O.answer_us >= (maps ? MORE_SUP_US : MORE_VAL_US)) ||
                    now - O.asked_us >= (maps ? WAIT_SUP_US : WAIT_VAL_US);
        if (!done) return false;
        O.waiting = false;
        O.next_us = now + GAP_US;
        if (maps) {
            if (!O.answered && ++O.tries < 3) {
                /* nobody: the same again */
            } else if (!O.answered && O.asking == 0) {
                O.on = false;
                O.st = ST_NOANSWER;
                return false;
            } else {
                O.tries = 0;
                int nx = O.asking + 0x20;
                if (nx <= 0xE0 && sup_has(nx)) O.asking = (uint8_t)nx;
                else {
                    O.maps_done = true;
                    O.st = ST_LIVE;
                    O.asking = 0;
                }
            }
        }
    }
    if (now < O.next_us) return false;
    if (O.maps_done) {
        int p = next_pid(O.asking);
        if (p < 0) return false;
        O.asking = (uint8_t)p;
    }
    memset(out, 0, sizeof *out);
    out->id = O.ext ? REQ_EXT : REQ_STD;
    out->fl = O.ext ? CN_EXT : 0;
    out->len = 8;
    out->data[0] = 2;
    out->data[1] = 0x01;
    out->data[2] = O.asking;
    O.waiting = true;
    O.answered = false;
    O.asked_us = now;
    O.asked++;
    return true;
}

void cn_obd_start(bool ext29)
{
    cn_lock();
    memset(&O, 0, sizeof O);
    O.ext = ext29;
    O.on = true;
    O.st = ST_MAPS;
    O.next_us = cn_us();
    O.gen = 1;
    cn_unlock();
}

void cn_obd_stop(void)
{
    cn_lock();
    O.st = ST_IDLE;
    O.on = false;
    O.waiting = false;
    cn_unlock();
}

/* ---- the portal's part of can_live.json ---- */

int cn_obd_json(char *out, int cap)
{
    static const char *const ST[] = { "idle", "maps", "live", "noanswer", "notnormal", "noack" };
    int n = 0;
    uint64_t now = cn_us();
    cn_lock();
    n += snprintf(out + n, (size_t)(cap - n), "{\"on\":%s,\"ext\":%s,\"st\":\"%s\",\"asked\":%u,\"answers\":%u,\"ecus\":[",
                  O.on ? "true" : "false", O.ext ? "true" : "false", ST[O.st], (unsigned)O.asked, (unsigned)O.answers);
    for (int i = 0; i < O.necu && n < cap - 16; i++)
        n += snprintf(out + n, (size_t)(cap - n), O.ecu[i] > 0x7FF ? "%s\"%08X\"" : "%s\"%03X\"", i ? "," : "",
                      (unsigned)O.ecu[i]);
    n += snprintf(out + n, (size_t)(cap - n), "],\"sup\":[");
    bool first = true;
    for (int p = 1; p < 256 && n < cap - 8; p++) {
        if (!sup_has(p) || is_map(p)) continue;
        n += snprintf(out + n, (size_t)(cap - n), "%s%d", first ? "" : ",", p);
        first = false;
    }
    n += snprintf(out + n, (size_t)(cap - n), "],\"v\":[");
    first = true;
    for (int p = 1; p < 256 && n < cap - 48; p++) {
        const obd_val_t *v = &O.v[p];
        if (!v->have) continue;
        uint32_t age = (uint32_t)((now - v->t_us) / 1000);
        n += snprintf(out + n, (size_t)(cap - n), "%s[%d,%d,\"", first ? "" : ",", p, v->ecu);
        for (int i = 0; i < v->n; i++) n += snprintf(out + n, (size_t)(cap - n), i ? " %02X" : "%02X", v->d[i]);
        n += snprintf(out + n, (size_t)(cap - n), "\",%u]", (unsigned)age);
        first = false;
    }
    cn_unlock();
    if (n < cap - 3) n += snprintf(out + n, (size_t)(cap - n), "]}");
    return n < cap ? n : cap - 1;
}

/* ---- the tab ---- */

static struct {
    lv_obj_t *state, *sub, *go_lbl, *go, *p11, *p29, *list;
    lv_obj_t *vals[256], *ages[256];
    uint32_t gen;
    bool     ext;               /* the pills' choice */
} T;

static const char *fuel_name(int a, char *buf, size_t cap)
{
    static const char *const NAMES[] = { N_("sin dato"), N_("nafta"), N_("metanol"), N_("etanol"), N_("diésel"),
                                         N_("GLP"), N_("GNC"), N_("propano"), N_("eléctrico") };
    if (a >= 0 && a < (int)(sizeof NAMES / sizeof NAMES[0])) return _(NAMES[a]);
    if ((a >= 9 && a <= 16) || a == 23) snprintf(buf, cap, _("bicombustible (%d)"), a);
    else if (a >= 17 && a <= 22) snprintf(buf, cap, _("híbrido (%d)"), a);
    else snprintf(buf, cap, _("código %d"), a);
    return buf;
}

static const char *std_name(int a, char *buf, size_t cap)
{
    static const char *const NAMES[] = { "", "OBD-II (CARB)", "OBD (EPA)", "OBD + OBD-II", "OBD-I", N_("sin OBD"),
                                         "EOBD", "EOBD + OBD-II", "EOBD + OBD", "EOBD + OBD + OBD-II", "JOBD",
                                         "JOBD + OBD-II", "JOBD + EOBD", "JOBD + EOBD + OBD-II" };
    if (a == 5) return _(NAMES[5]);
    if (a >= 1 && a < (int)(sizeof NAMES / sizeof NAMES[0])) return NAMES[a];
    snprintf(buf, cap, _("código %d"), a);
    return buf;
}

static void fmt_value(const obd_def_t *d, const obd_val_t *v, char *out, size_t cap)
{
    char b[40];
    if (v->n < (d->kind == K_STATUS ? 1 : d->nb)) {
        snprintf(out, cap, "?");
        return;
    }
    uint32_t raw = 0;
    for (int i = 0; i < d->nb && i < v->n; i++) raw = raw << 8 | v->d[i];
    switch (d->kind) {
    case K_STATUS:
        snprintf(out, cap, v->d[0] & 0x80 ? _("encendida · %d códigos") : _("apagada · %d códigos"), v->d[0] & 0x7F);
        return;
    case K_STD: snprintf(out, cap, "%s", std_name(v->d[0], b, sizeof b)); return;
    case K_FUEL: snprintf(out, cap, "%s", fuel_name(v->d[0], b, sizeof b)); return;
    default: break;
    }
    float x = (float)raw * d->k + d->o;
    int dec = d->k >= 0.99f && d->o == (float)(int32_t)d->o ? 0 : d->k >= 0.09f ? 1 : !d->unit[0] ? 3 : 2;
    if (d->k == PCT || d->k == TRIM) dec = 1;
    if (!strcmp(d->unit, "rpm")) dec = 0;                                /* 2114 rpm, not 2114.5 */
    snprintf(out, cap, "%.*f%s%s", dec, (double)x, d->unit[0] ? " " : "", d->unit);
}

static void go_cb(lv_event_t *e)
{
    (void)e;
    cn_lock();
    bool on = O.on;
    cn_unlock();
    if (on) {
        cn_obd_stop();
    } else if (!CB.open) {
        aos_ui_toast(_("Conectá el bus primero, en modo Normal"), 2200);
        return;
    } else if (CB.cfg.mode != CN_MODE_NORMAL) {
        aos_ui_toast(_("Preguntar necesita el modo Normal"), 2200);
        return;
    } else {
        cn_obd_start(T.ext);
    }
    cn_ui_set_tab(CN_TAB_OBD);
}

static void bits_cb(lv_event_t *e)
{
    T.ext = (bool)(intptr_t)lv_event_get_user_data(e);
    aos_hal_pref_set_i32("can_obd29", T.ext);
    cn_lock();
    bool on = O.on;
    cn_unlock();
    if (on) cn_obd_start(T.ext);
    cn_ui_set_tab(CN_TAB_OBD);
}

static void conn_cb(lv_event_t *e) { (void)e; cn_conn_sheet(); }

static lv_obj_t *note(lv_obj_t *parent, const char *text, lv_color_t c)
{
    lv_obj_t *l = aos_label(parent, text, aos_font_caption, c);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent, lv_color_t bg)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(c, 16, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_bg_color(c, bg, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 10, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static void ecu_text(uint32_t id, char *out, size_t cap)
{
    snprintf(out, cap, id > 0x7FF ? "%08X" : "%03X", (unsigned)id);
}

void cn_obd_refresh(void)
{
    if (U.tab != CN_TAB_OBD || !T.state) return;
    static const char *const ST[] = { N_("Detenido"), N_("Preguntando qué PIDs conoce…"), N_("En vivo"),
                                      N_("Nadie contestó"), N_("Detenido: hace falta el modo Normal"),
                                      N_("Detenido: nadie confirma lo que manda") };
    cn_lock();
    uint32_t gen = O.gen;
    int st = O.st, necu = O.necu, nsup = 0;
    bool on = O.on, ext = O.ext;
    uint32_t ecu[ECU_MAX], answers = O.answers;
    memcpy(ecu, O.ecu, sizeof ecu);
    for (int p = 1; p < 256; p++) nsup += sup_has(p) && !is_map(p);
    static obd_val_t v[256];
    memcpy(v, O.v, sizeof v);
    cn_unlock();
    if (gen != T.gen) {
        /* new PIDs or units: the list again, where it was */
        int32_t y = T.list ? lv_obj_get_scroll_y(lv_obj_get_parent(T.list)) : 0;
        cn_ui_set_tab(CN_TAB_OBD);
        if (T.list) lv_obj_scroll_to_y(lv_obj_get_parent(T.list), y, LV_ANIM_OFF);
        return;
    }
    char t[200];
    snprintf(t, sizeof t, "%s", _(ST[st]));
    lv_label_set_text(T.state, t);
    lv_obj_set_style_text_color(T.state, on ? AOS_C_GREEN : st >= ST_NOANSWER ? AOS_C_ORANGE : AOS_C_TEXT, 0);
    int n = 0;
    if (necu) {
        n = snprintf(t, sizeof t, "%s", _("Contestan:"));
        for (int i = 0; i < necu && n < (int)sizeof t - 12; i++) {
            char id[12];
            ecu_text(ecu[i], id, sizeof id);
            n += snprintf(t + n, sizeof t - (size_t)n, " %s", id);
        }
        n += snprintf(t + n, sizeof t - (size_t)n, _(" · %d PIDs · %u respuestas"), nsup, (unsigned)answers);
    } else {
        snprintf(t, sizeof t, "%s", ext ? _("Pregunta a 18DB33F1; contestan desde 18DAF1xx") : _("Pregunta a 7DF; contestan desde 7E8 a 7EF"));
    }
    lv_label_set_text(T.sub, t);
    lv_label_set_text(T.go_lbl, on ? _("Parar") : _("Preguntar"));
    lv_obj_set_style_bg_color(T.go, on ? AOS_C_CARD2 : AOS_C_GREEN, 0);

    uint64_t now = cn_us();
    for (int p = 1; p < 256; p++) {
        if (!T.vals[p]) continue;
        const obd_def_t *d = def_of(p);
        if (!v[p].have) continue;
        if (d) fmt_value(d, &v[p], t, sizeof t);
        else {
            n = 0;
            for (int i = 0; i < v[p].n; i++) n += snprintf(t + n, sizeof t - (size_t)n, i ? " %02X" : "%02X", v[p].d[i]);
        }
        lv_label_set_text(T.vals[p], t);
        uint32_t age = (uint32_t)((now - v[p].t_us) / 1000000);
        char id[12];
        ecu_text(v[p].ecu < necu ? ecu[v[p].ecu] : 0, id, sizeof id);
        if (age >= 3) snprintf(t, sizeof t, _("PID %02X · %s · hace %u s"), p, id, (unsigned)age);
        else snprintf(t, sizeof t, _("PID %02X · %s"), p, id);
        if (T.ages[p]) lv_label_set_text(T.ages[p], t);
    }
}

void cn_obd_build(lv_obj_t *parent)
{
    memset(&T, 0, sizeof T);
    int32_t v29;
    T.ext = aos_hal_pref_get_i32("can_obd29", &v29) && v29;
    cn_lock();
    bool on = O.on;
    if (on) T.ext = O.ext;
    int nsup = 0;
    bool have[256];
    for (int p = 0; p < 256; p++) {
        have[p] = p && !is_map(p) && (sup_has(p) || O.v[p].have);
        nsup += have[p];
    }
    T.gen = O.gen;
    cn_unlock();

    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_add_flag(col, LV_OBJ_FLAG_CLICKABLE);

    /* the state and the controls */
    lv_obj_t *head = box(col, AOS_C_CARD);
    T.state = aos_label(head, "", aos_font_body, AOS_C_TEXT);
    T.sub = aos_label(head, "", aos_font_caption, AOS_C_DIM);
    lv_obj_set_width(T.sub, LV_PCT(100));
    lv_label_set_long_mode(T.sub, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_t *br = lv_obj_create(head);
    lv_obj_remove_style_all(br);
    lv_obj_set_size(br, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(br, 10, 0);
    lv_obj_remove_flag(br, LV_OBJ_FLAG_SCROLLABLE);
    T.go = cn_btn(br, on ? AOS_SYM_STOP : AOS_SYM_PLAY, on ? _("Parar") : _("Preguntar"), go_cb, NULL);
    T.go_lbl = lv_obj_get_child(T.go, 1);
    T.p11 = cn_pill(br, _("11 bits"), !T.ext, bits_cb, (void *)(intptr_t)0);
    T.p29 = cn_pill(br, _("29 bits"), T.ext, bits_cb, (void *)(intptr_t)1);

    /* asking needs Normal: the app opens in Solo escucha */
    if (!CB.open || CB.cfg.mode != CN_MODE_NORMAL) {
        lv_obj_t *w = box(col, lv_color_hex(0x3A2A10));
        lv_obj_t *wl = aos_label(w, !CB.open ? _("El bus está desconectado") : CB.cfg.mode == CN_MODE_LISTEN
                                                                         ? _("La app está en Solo escucha")
                                                                         : _("La app está en Autoprueba"),
                                 aos_font_small, AOS_C_ORANGE);
        lv_obj_set_width(wl, LV_PCT(100));
        note(w, _("El escáner pregunta, así que necesita el modo Normal: la app abre siempre en Solo escucha para "
                  "no tocar un bus ajeno. Cambialo en la conexión. En un auto, casi siempre 500 kbit/s (algunos, "
                  "250), con el motor en marcha o el contacto puesto. En Solo escucha sí se ven las respuestas a "
                  "las preguntas de otro escáner enchufado al mismo conector."),
             AOS_C_TEXT);
        lv_obj_t *cb = cn_btn(w, AOS_SYM_LAN_CONNECT, _("Conexión"), conn_cb, NULL);
        lv_obj_set_width(cb, LV_SIZE_CONTENT);
    } else if (CB.cfg.fkind != CN_FILT_NONE) {
        lv_obj_t *w = box(col, lv_color_hex(0x3A2A10));
        note(w, _("El filtro del controlador está puesto: si no deja pasar 7E8-7EF (o 18DAF1xx), las respuestas no "
                  "llegan."), AOS_C_ORANGE);
    }

    /* the PIDs */
    T.list = lv_obj_create(col);
    lv_obj_remove_style_all(T.list);
    lv_obj_set_size(T.list, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(T.list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(T.list, 8, 0);
    lv_obj_remove_flag(T.list, LV_OBJ_FLAG_SCROLLABLE);
    char unknown[200];
    int nu = 0;
    unknown[0] = 0;
    for (int p = 1; p < 256; p++) {
        if (!have[p]) continue;
        const obd_def_t *d = def_of(p);
        lv_obj_t *r = lv_obj_create(T.list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, U.land ? LV_PCT(49) : LV_PCT(100), 84);
        lv_obj_set_style_radius(r, 16, 0);
        lv_obj_set_style_bg_color(r, AOS_C_CARD, 0);
        lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
        char t[48];
        if (d) snprintf(t, sizeof t, "%s", _(d->name));
        else snprintf(t, sizeof t, _("PID %02X, sin fórmula"), p);
        lv_obj_t *n = aos_label(r, t, aos_font_small, AOS_C_TEXT);
        lv_obj_set_size(n, LV_PCT(58), lv_font_get_line_height(aos_font_small));
        lv_label_set_long_mode(n, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 12);
        snprintf(t, sizeof t, "PID %02X", p);
        T.ages[p] = aos_label(r, t, aos_font_tiny, AOS_C_DIM);
        lv_obj_align(T.ages[p], LV_ALIGN_BOTTOM_LEFT, 0, -10);
        T.vals[p] = aos_label(r, "-", &aos_mono_22, AOS_C_TEAL);
        lv_obj_set_size(T.vals[p], LV_PCT(42), lv_font_get_line_height(&aos_mono_22));
        lv_label_set_long_mode(T.vals[p], LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_text_align(T.vals[p], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(T.vals[p], LV_ALIGN_RIGHT_MID, 0, 0);
        if (!d && nu < (int)sizeof unknown - 4) nu += snprintf(unknown + nu, sizeof unknown - (size_t)nu, " %02X", p);
    }
    if (!nsup)
        note(col, on ? _("Esperando el mapa de PIDs…")
                     : _("Preguntar pide primero el PID 00, el mapa de los que conoce el auto, y después da vueltas por "
                         "esos con las fórmulas de SAE J1979."),
             AOS_C_DIM);
    if (nu) {
        char t[260];
        snprintf(t, sizeof t, _("Sin fórmula en la tabla de la app (se muestran los bytes):%s"), unknown);
        note(col, t, AOS_C_DIM);
    }
    note(col, _("OBD-II es lo único igual en todos los autos: el diagnóstico de emisiones (modo 01 de SAE J1979). "
                "En muchos autos modernos el conector OBD da sólo eso: un gateway separa los buses internos (motor, "
                "carrocería, tablero), así que en Tramas se ve poco o nada más que estas respuestas, y los .dbc del "
                "auto sirven conectado detrás del gateway."),
         AOS_C_DIM);
    cn_obd_refresh();
}
