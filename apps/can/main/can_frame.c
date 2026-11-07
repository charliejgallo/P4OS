/*
 * P4OS - CAN: frames as text and the saved frames.
 *
 * A frame is written the way candump and cansend write it: the id in hex,
 * three digits for an 11-bit one and eight for a 29-bit one, a '#', and the
 * data in hex ("0C0#00000C80", "18FEF100#FF0012FFFFFFFFFF"); "123#R" is a
 * remote frame, "123#R4" one asking for four bytes. Spaces and dots between
 * the bytes are allowed when reading.
 *
 * The signals' file, the .dbc, is can_dbc.c's.
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
