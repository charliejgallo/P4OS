/*
 * P4OS - CAN: frames as text, the signals file and the saved frames.
 *
 * A frame is written the way candump and cansend write it: the id in hex,
 * three digits for an 11-bit one and eight for a 29-bit one, a '#', and the
 * data in hex ("0C0#00000C80", "18FEF100#FF0012FFFFFFFFFF"); "123#R" is a
 * remote frame, "123#R4" one asking for four bytes. Spaces and dots between
 * the bytes are allowed when reading.
 *
 * The signals come from <card>/can/senales.dbc, a subset of the DBC format
 * (what Vector's CANdb++, SavvyCAN and cantools write): the BO_ lines give a
 * message's id, the SG_ lines under it its signals, with the start bit,
 * length, byte order (@1 Intel, @0 Motorola), sign, scale, offset and unit.
 * Multiplexed signals and everything else in the file are skipped.
 *
 * The saved frames are <card>/can/enviar.txt, one a line:
 *     <frame> <period ms, 0 = by hand> <name>
 * The portal page edits the same file.
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

cn_saved_t CN_SAVED[CN_SAVED_MAX];
int        CN_NSAVED;

int cn_fmt_id(const cn_frame_t *f, char *out, int cap)
{
    return snprintf(out, cap, f->fl & CN_EXT ? "%08X" : "%03X", (unsigned)f->id);
}

int cn_fmt_data(const cn_frame_t *f, char *out, int cap)
{
    int n = 0;
    out[0] = 0;
    if (f->fl & CN_RTR) return snprintf(out, cap, "R%u", f->len);
    for (int i = 0; i < f->len && n < cap - 3; i++) n += snprintf(out + n, cap - n, i ? " %02X" : "%02X", f->data[i]);
    return n;
}

int cn_fmt_candump(const cn_frame_t *f, char *out, int cap)
{
    int n = cn_fmt_id(f, out, cap);
    if (n >= cap - 2) return n;
    out[n++] = '#';
    if (f->fl & CN_RTR) return n + snprintf(out + n, cap - n, f->len ? "R%u" : "R", f->len);
    for (int i = 0; i < f->len && n < cap - 2; i++) n += snprintf(out + n, cap - n, "%02X", f->data[i]);
    out[n] = 0;
    return n;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = toupper(c);
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool cn_parse_hex(const char *s, uint32_t *out)
{
    while (*s == ' ') s++;
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    uint32_t v = 0;
    int n = 0;
    for (; *s && *s != ' '; s++, n++) {
        int h = hexval((unsigned char)*s);
        if (h < 0 || n >= 8) return false;
        v = v << 4 | (uint32_t)h;
    }
    if (!n) return false;
    *out = v;
    return true;
}

bool cn_parse_candump(const char *s, cn_frame_t *f)
{
    memset(f, 0, sizeof *f);
    while (*s == ' ' || *s == '\t') s++;
    const char *hash = strchr(s, '#');
    if (!hash) return false;
    int digits = 0;
    uint32_t id = 0;
    for (const char *p = s; p < hash; p++) {
        int h = hexval((unsigned char)*p);
        if (h < 0) return false;
        id = id << 4 | (uint32_t)h;
        if (++digits > 8) return false;
    }
    if (!digits) return false;
    if (digits > 3 || id > 0x7FF) f->fl |= CN_EXT;
    if (id > 0x1FFFFFFF) return false;
    f->id = id;
    const char *p = hash + 1;
    if (*p == 'R' || *p == 'r') {
        f->fl |= CN_RTR;
        if (p[1] >= '0' && p[1] <= '8') f->len = (uint8_t)(p[1] - '0');
        return true;
    }
    int half = -1;
    while (*p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
        if (*p == '.' || *p == ':') { p++; continue; }
        int h = hexval((unsigned char)*p++);
        if (h < 0) return false;
        if (half < 0) half = h;
        else {
            if (f->len >= 8) return false;
            f->data[f->len++] = (uint8_t)(half << 4 | h);
            half = -1;
        }
    }
    return half < 0;
}

const char *cn_state_name(const char *state)
{
    if (!state || !state[0]) return _("desconectado");
    if (!strcmp(state, "active")) return _("activo");
    if (!strcmp(state, "warning")) return _("con avisos");
    if (!strcmp(state, "passive")) return _("pasivo");
    if (!strcmp(state, "bus_off")) return _("fuera del bus");
    return state;
}

lv_color_t cn_state_color(const char *state)
{
    if (!state || !state[0]) return AOS_C_DIM;
    if (!strcmp(state, "active")) return AOS_C_GREEN;
    if (!strcmp(state, "warning")) return AOS_C_YELLOW;
    if (!strcmp(state, "passive")) return AOS_C_ORANGE;
    return AOS_C_RED;
}

const char *cn_mode_name(cn_mode_t m)
{
    switch (m) {
    case CN_MODE_NORMAL:   return _("Normal");
    case CN_MODE_SELFTEST: return _("Autoprueba");
    default:               return _("Solo escucha");
    }
}

/* ---- signals ---- */

static const char *sig_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/senales.dbc", cn_dir());
    return out;
}

/* SG_ name : 23|16@0+ (0.25,0) [0|16000] "rpm" receivers */
static bool parse_sg(const char *s, cn_sig_t *g)
{
    while (*s == ' ' || *s == '\t') s++;
    if (strncmp(s, "SG_ ", 4)) return false;
    s += 4;
    while (*s == ' ') s++;
    int n = 0;
    while (*s && *s != ' ' && *s != ':' && n < (int)sizeof g->name - 1) g->name[n++] = *s++;
    g->name[n] = 0;
    while (*s == ' ') s++;
    if (*s != ':') return false;                /* "M" or "m3": multiplexed, skipped */
    s++;
    char *e;
    g->start = (uint16_t)strtoul(s, &e, 10);
    if (*e != '|') return false;
    g->len = (uint16_t)strtoul(e + 1, &e, 10);
    if (*e != '@' || g->len < 1 || g->len > 32) return false;      /* longer ones: skipped */
    g->motorola = e[1] == '0';
    g->is_signed = e[2] == '-';
    s = strchr(e, '(');
    if (!s) return false;
    g->scale = strtof(s + 1, &e);
    if (*e != ',') return false;
    g->offset = strtof(e + 1, &e);
    g->unit[0] = 0;
    s = strchr(e, '"');
    if (s) {
        s++;
        n = 0;
        while (*s && *s != '"' && n < (int)sizeof g->unit - 1) g->unit[n++] = *s++;
        g->unit[n] = 0;
    }
    return true;
}

int cn_sig_load(cn_sig_t *out, int max, char *err, int err_cap)
{
    char path[128];
    FILE *f = fopen(sig_path(path, sizeof path), "r");
    if (err) err[0] = 0;
    if (!f) {
        if (err) snprintf(err, err_cap, _("No está %s"), "can/senales.dbc");
        return 0;
    }
    char line[200];
    int n = 0;
    bool in_msg = false;
    uint32_t id = 0;
    bool ext = false;
    while (n < max && fgets(line, sizeof line, f)) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!strncmp(s, "BO_ ", 4)) {
            char *e;
            uint32_t raw = (uint32_t)strtoul(s + 4, &e, 10);
            in_msg = e != s + 4;
            ext = raw & 0x80000000u;
            id = raw & 0x1FFFFFFFu;
            if (!ext && id > 0x7FF) ext = true;
            continue;
        }
        if (!strncmp(s, "SG_ ", 4) && in_msg) {
            cn_sig_t g;
            memset(&g, 0, sizeof g);
            if (!parse_sg(s, &g)) continue;
            g.id = id;
            g.ext = ext;
            out[n++] = g;
            continue;
        }
        if (*s && *s != '\r' && *s != '\n') in_msg = false;
    }
    fclose(f);
    if (!n && err) snprintf(err, err_cap, "%s", _("El archivo no tiene señales (BO_ y SG_)"));
    return n;
}

static int bit_at(const uint8_t *d, int pos) { return d[pos >> 3] >> (pos & 7) & 1; }

bool cn_sig_value(const cn_sig_t *g, const uint8_t *data, int len, float *out)
{
    /* 32 bits at most (parse_sg skips longer ones): 64-bit shifts and
     * conversions are not in the firmware's table */
    uint32_t raw = 0;
    if (g->motorola) {
        /* the start bit is the most significant one; walking down a byte
         * and on to bit 7 of the next */
        int pos = g->start;
        for (int i = 0; i < g->len; i++) {
            if ((pos >> 3) >= len) return false;
            raw = raw << 1 | (uint32_t)bit_at(data, pos);
            if ((pos & 7) == 0) pos += 15;
            else pos--;
        }
    } else {
        for (int i = 0; i < g->len; i++) {
            int pos = g->start + i;
            if ((pos >> 3) >= len) return false;
            raw |= (uint32_t)bit_at(data, pos) << i;
        }
    }
    float v;
    if (g->is_signed && g->len < 32 && (raw >> (g->len - 1) & 1)) v = (float)(int32_t)(raw | (~0u << g->len));
    else if (g->is_signed && g->len == 32) v = (float)(int32_t)raw;
    else v = (float)raw;
    *out = v * g->scale + g->offset;
    return true;
}

void cn_sig_write_example(void)
{
    char path[128];
    struct stat sb;
    if (stat(sig_path(path, sizeof path), &sb) == 0) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("VERSION \"\"\n\n"
          "NS_ :\n\nBS_:\n\nBU_: Motor\n\n"
          "CM_ \"P4OS: ejemplo, las senales del auto del simulador. Reemplazalo por el .dbc de tu bus.\";\n\n"
          "BO_ 192 Motor: 8 Motor\n"
          " SG_ RPM : 23|16@0+ (0.25,0) [0|16000] \"rpm\" Vector__XXX\n"
          " SG_ Contador : 56|8@1+ (1,0) [0|255] \"\" Vector__XXX\n\n"
          "BO_ 416 Velocidad: 4 Motor\n"
          " SG_ Velocidad : 7|16@0+ (0.01,0) [0|300] \"km/h\" Vector__XXX\n\n"
          "BO_ 1000 Temperaturas: 2 Motor\n"
          " SG_ Agua : 0|8@1+ (1,-40) [-40|215] \"\xC2\xB0" "C\" Vector__XXX\n"
          " SG_ Aceite : 8|8@1+ (1,-40) [-40|215] \"\xC2\xB0" "C\" Vector__XXX\n\n"
          "BO_ 2566844672 J1939_FEF1: 8 Motor\n"
          " SG_ Contador_J : 15|16@0+ (1,0) [0|65535] \"\" Vector__XXX\n", f);
    fclose(f);
}

/* ---- saved frames ---- */

static const char *saved_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/enviar.txt", cn_dir());
    return out;
}

void cn_saved_load(void)
{
    char path[128], line[160];
    FILE *f = fopen(saved_path(path, sizeof path), "r");
    CN_NSAVED = 0;
    if (!f) return;
    while (CN_NSAVED < CN_SAVED_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') continue;
        cn_saved_t *v = &CN_SAVED[CN_NSAVED];
        memset(v, 0, sizeof *v);
        if (!cn_parse_candump(s, &v->f)) continue;
        s += strcspn(s, " \t");
        while (*s == ' ' || *s == '\t') s++;
        char *e;
        unsigned long p = strtoul(s, &e, 10);
        if (e != s) {
            v->period_ms = p > 60000 ? 60000 : (uint32_t)p;
            s = e;
            while (*s == ' ' || *s == '\t') s++;
        }
        snprintf(v->name, sizeof v->name, "%s", s);
        CN_NSAVED++;
    }
    fclose(f);
}

void cn_saved_save(void)
{
    char path[128], part[136], fr[40];
    snprintf(part, sizeof part, "%s.part", saved_path(path, sizeof path));
    FILE *f = fopen(part, "w");
    if (!f) {
        aos_ui_toast(_("No se pudo guardar en la tarjeta"), 2000);
        return;
    }
    fputs("# CAN: tramas guardadas. Una por linea: trama  periodo_ms  nombre\n"
          "# La trama como en candump: 123#11223344, 18FEF100#FF00, 7DF#R\n", f);
    for (int i = 0; i < CN_NSAVED; i++) {
        cn_fmt_candump(&CN_SAVED[i].f, fr, sizeof fr);
        fprintf(f, "%s %u%s%s\n", fr, (unsigned)CN_SAVED[i].period_ms, CN_SAVED[i].name[0] ? " " : "",
                CN_SAVED[i].name);
    }
    fclose(f);
    unlink(path);
    rename(part, path);
}
