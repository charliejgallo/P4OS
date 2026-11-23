/*
 * P4OS - Monitor serie: lines into numbers.
 *
 * Automatic, for what most devices print:
 *
 *   21.5 40 1013          numbers apart: by spaces, commas, semicolons, tabs
 *   temp:21.5,hum:40      a name per value, as serial plotters take it
 *   Temp = 21.5 Hum 40    a word just before a number names it
 *   time;temp;hum         a CSV header (two words or more, between commas,
 *   12:00:01;21,5;40      semicolons or tabs): its names go to the values at
 *                         the same places. With semicolons and no point,
 *                         the comma is the decimal mark.
 *
 * A number may carry a unit stuck to it ("3.3V", "21.5°C", "45%"); hex,
 * dates and words with digits in them are not numbers. A line with no
 * number is skipped, so a board's boot log does not make series.
 *
 * A profile is for the rest: the line cut into tokens (spaces, tabs, commas,
 * semicolons, ':' and '='), and each series is a token by its number, times
 * a scale plus an offset. The decimal point can come from another token: a
 * multimeter's "+0265 4" is 265 with a code that says where the point goes
 * (4 = one decimal: 26.5), through a map like "0:0,1:3,2:2,4:1".
 */
#include "pl.h"

#include <ctype.h>

static int pl_isalpha_utf(unsigned char c) { return isalpha(c) || c >= 0x80; }

/* A number at s, with an optional unit after it that runs to the end. */
bool pl_num(const char *s, bool comma, float *v, int *dec, const char **unit)
{
    const char *p = s;
    while (*p == ' ') p++;
    const char *start = p;
    if (*p == '+' || *p == '-') p++;
    int digits = 0, frac = 0, ex = 0;
    bool point = false;
    float m = 0;
    char mark = comma ? ',' : '.';
    for (;; p++) {
        if (*p >= '0' && *p <= '9') {
            if (point) frac++;
            m = m * 10.0f + (float)(*p - '0');
            digits++;
        } else if (*p == mark && !point) {
            point = true;
        } else {
            break;
        }
    }
    if (!digits) return false;
    if ((*p == 'e' || *p == 'E') && (isdigit((unsigned char)p[1]) ||
        ((p[1] == '-' || p[1] == '+') && isdigit((unsigned char)p[2])))) {
        p++;
        int sg = 1;
        if (*p == '-' || *p == '+') { sg = *p == '-' ? -1 : 1; p++; }
        while (isdigit((unsigned char)*p)) { ex = ex * 10 + (*p - '0'); p++; if (ex > 60) return false; }
        ex *= sg;
    }
    /* the unit: letters, %, °, µ, Ω, '/', up to 7 bytes, and nothing else */
    while (*p == ' ') p++;
    const char *u = p;
    int ul = 0;
    while (*p && ul < 8) {
        unsigned char c = (unsigned char)*p;
        if (!(pl_isalpha_utf(c) || c == '%' || c == '/')) return false;
        p++;
        ul++;
    }
    while (*p == ' ') p++;
    if (*p) return false;
    /* "0x1F", "12a3": a digit after letters is not a unit */
    if (ul && (u[0] == 'x' || u[0] == 'X') && digits == 1 && !point && start[*start == '-' || *start == '+'] == '0')
        return false;
    float val = m;
    int sc = frac - ex;
    if (sc > 0) val /= powf(10.0f, (float)sc);
    else if (sc < 0) val *= powf(10.0f, (float)-sc);
    if (*start == '-') val = -val;
    if (v) *v = val;
    if (dec) *dec = sc < 0 ? 0 : sc > 6 ? 6 : sc;
    if (unit) *unit = ul ? u : NULL;
    return true;
}

static void put_unit(char *dst, const char *u)
{
    dst[0] = 0;
    if (!u) return;
    int i = 0;
    while (u[i] && u[i] != ' ' && i < PL_UNIT - 1) { dst[i] = u[i]; i++; }
    dst[i] = 0;
}

static void trim(char *s)
{
    char *a = s;
    while (*a == ' ') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n && s[n - 1] == ' ') s[--n] = 0;
}

static bool is_word(const char *t)
{
    return pl_isalpha_utf((unsigned char)t[0]) || t[0] == '_';
}

static void add(pl_parsed_t *o, const char *name, float v, int dec, const char *unit)
{
    if (o->n >= PL_MAXS) return;
    /* a name seen twice in the line: the second is another series */
    pl_scpy(o->name[o->n], PL_NAME, name);
    o->v[o->n] = v;
    o->dec[o->n] = (int8_t)dec;
    put_unit(o->unit[o->n], unit);
    o->n++;
}

int pl_parse_auto(pl_auto_t *st, const char *line, pl_parsed_t *out)
{
    memset(out, 0, sizeof *out);
    char buf[AOS_SERIAL_LINE_MAX];
    pl_scpy(buf, sizeof buf, line);
    for (char *p = buf; *p; p++) if (*p == '\t') *p = ';';
    bool comma = strchr(buf, ';') && strchr(buf, ',') && !strchr(buf, '.');
    const char *delims = comma ? ";" : ",;";
    bool delimited = strpbrk(buf, delims) != NULL;

    /* the fields: between delimiters, or the whole line */
    char *fields[PL_MAXS * 2];
    int nf = 0;
    if (delimited) {
        char *p = buf;
        while (nf < PL_MAXS * 2) {
            fields[nf++] = p;
            char *d = strpbrk(p, delims);
            if (!d) break;
            *d = 0;
            p = d + 1;
        }
    } else {
        fields[nf++] = buf;
    }
    for (int i = 0; i < nf; i++) trim(fields[i]);

    /* a header: every field a word */
    if (delimited && nf >= 2) {
        bool all = true;
        for (int i = 0; i < nf && all; i++) {
            float v;
            all = fields[i][0] && !pl_num(fields[i], comma, &v, NULL, NULL) && is_word(fields[i]) &&
                  !strpbrk(fields[i], ":=");
        }
        if (all) {
            st->nhdr = nf;
            for (int i = 0; i < nf; i++) pl_scpy(st->hdr[i], PL_NAME, fields[i]);
            out->header = true;
            for (int i = 0; i < nf && i < PL_MAXS; i++) pl_scpy(out->name[i], PL_NAME, fields[i]);
            return 0;
        }
    }

    int unnamed = 0;
    for (int fi = 0; fi < nf; fi++) {
        char *f = fields[fi];
        if (!f[0]) continue;
        const char *hname = delimited && st->nhdr == nf ? st->hdr[fi] : NULL;
        /* the whole field a number (a unit with a space inside it, "21.5 C") */
        float v;
        int dec;
        const char *unit;
        if (pl_num(f, comma, &v, &dec, &unit)) {
            char nm[PL_NAME];
            if (hname && hname[0]) pl_scpy(nm, sizeof nm, hname);
            else snprintf(nm, sizeof nm, _("Valor %d"), unnamed + 1);
            unnamed++;
            add(out, nm, v, dec, unit);
            continue;
        }
        /* words and numbers inside the field, by spaces */
        char pending[PL_NAME] = "";
        char *save = f;
        while (save && *save) {
            while (*save == ' ') save++;
            if (!*save) break;
            char *tk = save;
            char *sp = strchr(save, ' ');
            if (sp) { *sp = 0; save = sp + 1; } else save = NULL;
            char *c = strpbrk(tk, ":=");
            if (c && c != tk) {
                *c = 0;
                const char *val = c + 1;
                if (!*val) { if (is_word(tk)) pl_scpy(pending, sizeof pending, tk); continue; }
                if (is_word(tk) && pl_num(val, comma, &v, &dec, &unit)) {
                    add(out, tk, v, dec, unit);
                    pending[0] = 0;
                }
                continue;
            }
            if ((tk[0] == ':' || tk[0] == '=') && !tk[1]) continue;
            if (pl_num(tk, comma, &v, &dec, &unit)) {
                char nm[PL_NAME];
                if (pending[0]) pl_scpy(nm, sizeof nm, pending);
                else if (hname && hname[0]) pl_scpy(nm, sizeof nm, hname);
                else snprintf(nm, sizeof nm, _("Valor %d"), unnamed + 1);
                if (!pending[0]) unnamed++;
                add(out, nm, v, dec, unit);
                pending[0] = 0;
                /* a lone unit right after the number: "21.5 C" */
                continue;
            }
            if (is_word(tk)) pl_scpy(pending, sizeof pending, tk);
            else pending[0] = 0;
        }
    }
    return out->n;
}

int pl_tokens(const char *line, bool comma, char tok[][24], int max)
{
    const char *seps = comma ? " \t;:=" : " \t,;:=";
    int n = 0;
    const char *p = line;
    while (*p && n < max) {
        while (*p && strchr(seps, *p)) p++;
        if (!*p) break;
        int k = 0;
        while (*p && !strchr(seps, *p)) {
            if (k < 23) tok[n][k++] = *p;
            p++;
        }
        tok[n][k] = 0;
        n++;
    }
    return n;
}

/* "0:0,1:3,2:2,4:1" -> the decimals for code; -1 when the code is not there */
static int map_dec(const char *map, int code)
{
    if (!map[0]) return code;
    const char *p = map;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        char *e;
        long k = strtol(p, &e, 10);
        if (e == p) return -1;
        p = e;
        while (*p == ' ') p++;
        if (*p != ':' && *p != '=') return -1;
        p++;
        long d = strtol(p, &e, 10);
        if (e == p) return -1;
        p = e;
        if (k == code) return (int)d;
    }
    return -1;
}

int pl_parse_profile(const pl_profile_t *pr, const char *line, pl_parsed_t *out)
{
    memset(out, 0, sizeof *out);
    if (pr->match[0] && !pl_has_nocase(line, pr->match)) return -1;
    char tok[24][24];
    int nt = pl_tokens(line, pr->comma, tok, 24);
    for (int i = 0; i < pr->nf && out->n < PL_MAXS; i++) {
        const pl_pfield_t *f = &pr->f[i];
        if (f->field < 1 || f->field > nt) continue;
        float v;
        int dec;
        const char *unit;
        if (!pl_num(tok[f->field - 1], pr->comma, &v, &dec, &unit)) continue;
        if (f->dp_field >= 1) {
            if (f->dp_field > nt) continue;
            char *e;
            long code = strtol(tok[f->dp_field - 1], &e, 10);
            if (e == tok[f->dp_field - 1]) continue;
            int d = map_dec(f->dp_map, (int)code);
            if (d < 0 || d > 9) continue;
            v /= powf(10.0f, (float)d);
            dec += d;
        }
        float sc = f->scale == 0 ? 1.0f : f->scale;
        v = v * sc + f->offset;
        if (fabsf(sc) < 1.0f) dec += (int)ceilf(-log10f(fabsf(sc)) - 0.001f);
        if (dec > 6) dec = 6;
        if (dec < 0) dec = 0;
        char nm[PL_NAME];
        if (f->name[0]) pl_scpy(nm, sizeof nm, f->name);
        else snprintf(nm, sizeof nm, _("Valor %d"), i + 1);
        add(out, nm, v, dec, f->unit[0] ? f->unit : unit);
    }
    return out->n;
}
