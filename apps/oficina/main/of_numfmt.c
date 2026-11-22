/*
 * P4OS - Office: number formats.
 *
 * Excel's format codes ("#,##0.00", "0%", "dd/mm/yyyy", "[$€-C0A] #,##0.00;
 * [Red]-#,##0.00", "@"), the subset sheets use: up to four sections
 * (positive; negative; zero; text), literals in quotes or after a
 * backslash, colours and locale tags skipped (the currency symbol of a
 * locale tag is kept), digit placeholders 0 # ?, grouping, scaling by
 * trailing commas, percent, scientific notation, and dates and times (y m d
 * h s, AM/PM, [h]). Fractions ("# ?/?") come out as decimals.
 *
 * The decimal and grouping marks follow the interface language: a comma and
 * a point in Spanish and German, the other way round in English. What is
 * written into the files always uses the point.
 */
#include "of.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { uint32_t id; const char *code; } BUILTIN[] = {
    { 0, "General" }, { 1, "0" }, { 2, "0.00" }, { 3, "#,##0" }, { 4, "#,##0.00" },
    { 5, "\"$\"#,##0;\\-\"$\"#,##0" }, { 6, "\"$\"#,##0;[Red]\\-\"$\"#,##0" },
    { 7, "\"$\"#,##0.00;\\-\"$\"#,##0.00" }, { 8, "\"$\"#,##0.00;[Red]\\-\"$\"#,##0.00" },
    { 9, "0%" }, { 10, "0.00%" }, { 11, "0.00E+00" }, { 12, "# ?/?" }, { 13, "# ?\?/?\?" },
    { 14, "dd/mm/yyyy" }, { 15, "d-mmm-yy" }, { 16, "d-mmm" }, { 17, "mmm-yy" },
    { 18, "h:mm AM/PM" }, { 19, "h:mm:ss AM/PM" }, { 20, "h:mm" }, { 21, "h:mm:ss" },
    { 22, "dd/mm/yyyy h:mm" }, { 37, "#,##0 ;(#,##0)" }, { 38, "#,##0 ;[Red](#,##0)" },
    { 39, "#,##0.00;(#,##0.00)" }, { 40, "#,##0.00;[Red](#,##0.00)" }, { 45, "mm:ss" },
    { 46, "[h]:mm:ss" }, { 47, "mm:ss.0" }, { 48, "##0.0E+0" }, { 49, "@" },
};

const char *of_numfmt_code(const of_book_t *b, uint32_t id)
{
    if (b)
        for (int i = 0; i < b->nnf; i++)
            if (b->nf[i].id == id) return b->nf[i].code;
    for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++)
        if (BUILTIN[i].id == id) {
            /* the short date in the person's order */
            if (id == 14 && !of_locale_comma()) return "m/d/yyyy";
            if (id == 22 && !of_locale_comma()) return "m/d/yyyy h:mm";
            return BUILTIN[i].code;
        }
    return "General";
}

/* -------------------------------------------------------------------------- */
/* Dates: Excel's serial numbers, with its 29 February 1900                   */
/* -------------------------------------------------------------------------- */

static long days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, int *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

double of_date_serial(int y, int m, int d)
{
    long days = days_from_civil(y, m, d) - days_from_civil(1899, 12, 30);
    if (days < 61) days--;          /* before Excel's fictitious 1900-02-29 */
    if (y == 1900 && m == 2 && d == 29) days = 60;
    return (double)days;
}

void of_serial_date(double s, int *y, int *m, int *d)
{
    long n = (long)s;
    if (n == 60) { *y = 1900; *m = 2; *d = 29; return; }
    if (n < 60) n++;
    civil_from_days(n + days_from_civil(1899, 12, 30), y, m, d);
}

/* -------------------------------------------------------------------------- */
/* Numbers                                                                     */
/* -------------------------------------------------------------------------- */

static void localise(char *s)
{
    /* the C locale's '.' and ',' to the person's */
    if (!of_locale_comma()) return;
    for (; *s; s++) {
        if (*s == '.') *s = ',';
        else if (*s == ',') *s = '.';
    }
}

/* %.*g without the exponent for numbers that fit, trailing zeros dropped */
static void fmt_sig(double n, int sig, char *out, size_t len)
{
    double a = n < 0 ? -n : n;
    if (a != 0 && (a >= 1e11 || a < 1e-9)) {
        char tmp[40];
        snprintf(tmp, sizeof tmp, "%.*E", sig > 6 ? 5 : sig - 1, n);
        /* 1.23450E+11 -> 1.2345E+11 */
        char *e = strchr(tmp, 'E');
        if (e) {
            char *z = e;
            while (z > tmp && z[-1] == '0') z--;
            if (z > tmp && z[-1] == '.') z--;
            memmove(z, e, strlen(e) + 1);
            /* E+011 / E+11 */
        }
        snprintf(out, len, "%s", tmp);
    } else {
        snprintf(out, len, "%.*g", sig, n);
        if (strchr(out, 'e')) {
            /* %g picked the exponent for something like 1e10 with sig 10 */
            snprintf(out, len, "%.0f", n);
        }
    }
    localise(out);
}

void of_fmt_general(double n, char *out, size_t len)
{
    fmt_sig(n, 15, out, len);
}

static void fmt_general_display(double n, char *out, size_t len)
{
    fmt_sig(n, 11, out, len);
}

/* -------------------------------------------------------------------------- */
/* The code                                                                    */
/* -------------------------------------------------------------------------- */

/* The sections, split at ';' outside quotes and brackets. */
static int sections(const char *code, const char *sec[4], int slen[4])
{
    int n = 0;
    const char *start = code;
    for (const char *p = code;; p++) {
        if (*p == '"') { p++; while (*p && *p != '"') p++; if (!*p) break; continue; }
        if (*p == '\\' && p[1]) { p++; continue; }
        if (*p == '[') { while (*p && *p != ']') p++; if (!*p) break; continue; }
        if (*p == ';' || !*p) {
            if (n < 4) { sec[n] = start; slen[n] = (int)(p - start); n++; }
            if (!*p) break;
            start = p + 1;
        }
    }
    return n;
}

static bool is_date_section(const char *s, int n)
{
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') { i++; while (i < n && s[i] != '"') i++; continue; }
        if (c == '\\' || c == '_' || c == '*') { i++; continue; }
        if (c == '[') {
            /* [h] [mm] [ss] are elapsed time; [Red], [$€-...] are not dates */
            if (i + 1 < n && (s[i + 1] == 'h' || s[i + 1] == 'H' || s[i + 1] == 'm' || s[i + 1] == 's')) return true;
            while (i < n && s[i] != ']') i++;
            continue;
        }
        if (c == 'y' || c == 'Y' || c == 'd' || c == 'D' || c == 'h' || c == 'H' || c == 's' || c == 'S') return true;
        if (c == 'm' || c == 'M') return true;
        if (c == '0' || c == '#' || c == '?') return false;
    }
    return false;
}

bool of_numfmt_is_date(const char *code)
{
    if (!code || !strcmp(code, "General")) return false;
    const char *sec[4];
    int sl[4];
    int n = sections(code, sec, sl);
    return n > 0 && is_date_section(sec[0], sl[0]);
}

static const char *const MONTHS_ES[12] = { "ene", "feb", "mar", "abr", "may", "jun", "jul", "ago", "sep", "oct", "nov", "dic" };
static const char *const MONTHS_EN[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
static const char *const MONTHL_ES[12] = { "enero", "febrero", "marzo", "abril", "mayo", "junio", "julio", "agosto",
                                           "septiembre", "octubre", "noviembre", "diciembre" };
static const char *const MONTHL_EN[12] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                           "September", "October", "November", "December" };
static const char *const DAYS_ES[7] = { "domingo", "lunes", "martes", "mi\xC3\xA9rcoles", "jueves", "viernes", ("s\xC3\xA1" "bado") };
static const char *const DAYS_EN[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };

static void put(char *out, size_t len, size_t *k, const char *s, size_t n)
{
    for (size_t i = 0; i < n && *k + 1 < len; i++) out[(*k)++] = s[i];
    out[*k < len ? *k : len - 1] = 0;
}

static void fmt_date(const char *s, int n, double v, bool date1904, char *out, size_t len)
{
    if (date1904) v += 1462;
    if (v < 0) { snprintf(out, len, "#####"); return; }
    double day = (double)(long)v;
    double frac = v - day;
    long secs = (long)(frac * 86400.0 + 0.5);
    if (secs >= 86400) { secs -= 86400; day += 1; }
    int y, m, d;
    of_serial_date(day, &y, &m, &d);
    int hh = (int)(secs / 3600), mi = (int)(secs / 60 % 60), ss = (int)(secs % 60);
    int wd = (int)(((long)day + 6) % 7);
    bool ampm = false;
    for (int i = 0; i + 1 < n; i++)
        if ((s[i] == 'A' || s[i] == 'a') && (s[i + 1] == 'M' || s[i + 1] == 'm') && i + 4 < n + 1 &&
            (s[i + 2] == '/')) ampm = true;
    bool es = of_locale_comma();
    size_t k = 0;
    out[0] = 0;
    bool last_was_h = false;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        char buf[24];
        int run = 1;
        while (i + run < n && (s[i + run] | 0x20) == (c | 0x20) && (c | 0x20) >= 'a' && (c | 0x20) <= 'z') run++;
        char lc = (char)(c | 0x20);
        if (c == '"') {
            int j = i + 1;
            while (j < n && s[j] != '"') j++;
            put(out, len, &k, s + i + 1, (size_t)(j - i - 1));
            i = j;
            continue;
        }
        if (c == '\\' && i + 1 < n) { put(out, len, &k, s + i + 1, 1); i++; continue; }
        if (c == '_' && i + 1 < n) { put(out, len, &k, " ", 1); i++; continue; }
        if (c == '*' && i + 1 < n) { i++; continue; }
        if (c == '[') {
            int j = i;
            while (j < n && s[j] != ']') j++;
            /* elapsed hours */
            if (i + 1 < n && (s[i + 1] | 0x20) == 'h') {
                snprintf(buf, sizeof buf, "%ld", (long)(v * 24));
                put(out, len, &k, buf, strlen(buf));
                last_was_h = true;
            }
            i = j;
            continue;
        }
        if (lc == 'y') {
            if (run >= 3) snprintf(buf, sizeof buf, "%04d", y);
            else snprintf(buf, sizeof buf, "%02d", y % 100);
            put(out, len, &k, buf, strlen(buf));
            i += run - 1;
            continue;
        }
        if (lc == 'd') {
            if (run >= 4) snprintf(buf, sizeof buf, "%s", es ? DAYS_ES[wd] : DAYS_EN[wd]);
            else if (run == 3) snprintf(buf, sizeof buf, "%.3s", es ? DAYS_ES[wd] : DAYS_EN[wd]);
            else snprintf(buf, sizeof buf, run == 2 ? "%02d" : "%d", d);
            put(out, len, &k, buf, strlen(buf));
            i += run - 1;
            last_was_h = false;
            continue;
        }
        if (lc == 'm') {
            /* minutes right after hours or before seconds */
            bool minutes = last_was_h;
            if (!minutes) {
                for (int j = i + run; j < n; j++) {
                    char cj = (char)(s[j] | 0x20);
                    if (cj == 's') { minutes = true; break; }
                    if (cj != ':' && cj != ' ' && cj != '.' && cj != '0') break;
                }
            }
            if (minutes && run <= 2) snprintf(buf, sizeof buf, run == 2 ? "%02d" : "%d", mi);
            else if (run >= 5) snprintf(buf, sizeof buf, "%c", (es ? MONTHL_ES : MONTHL_EN)[m - 1][0]);
            else if (run == 4) snprintf(buf, sizeof buf, "%s", (es ? MONTHL_ES : MONTHL_EN)[m - 1]);
            else if (run == 3) snprintf(buf, sizeof buf, "%s", (es ? MONTHS_ES : MONTHS_EN)[m - 1]);
            else snprintf(buf, sizeof buf, run == 2 ? "%02d" : "%d", m);
            put(out, len, &k, buf, strlen(buf));
            i += run - 1;
            last_was_h = false;
            continue;
        }
        if (lc == 'h') {
            int h = hh;
            if (ampm) { h %= 12; if (!h) h = 12; }
            snprintf(buf, sizeof buf, run >= 2 ? "%02d" : "%d", h);
            put(out, len, &k, buf, strlen(buf));
            i += run - 1;
            last_was_h = true;
            continue;
        }
        if (lc == 's') {
            snprintf(buf, sizeof buf, run >= 2 ? "%02d" : "%d", ss);
            put(out, len, &k, buf, strlen(buf));
            i += run - 1;
            continue;
        }
        if ((c == 'A' || c == 'a') && i + 4 < n + 1 && !strncasecmp(s + i, "AM/PM", 5)) {
            put(out, len, &k, hh < 12 ? "AM" : "PM", 2);
            i += 4;
            continue;
        }
        if ((c == 'A' || c == 'a') && !strncasecmp(s + i, "A/P", 3)) {
            put(out, len, &k, hh < 12 ? "A" : "P", 1);
            i += 2;
            continue;
        }
        if (c == '0' && k > 0 && out[k - 1] == ',') {   /* tenths of a second: mm:ss.0 */
            snprintf(buf, sizeof buf, "%d", (int)((frac * 86400.0 - (double)(long)(frac * 86400.0)) * 10));
            put(out, len, &k, buf, 1);
            continue;
        }
        put(out, len, &k, (c == '.' && es) ? "," : &s[i], 1);
    }
}

/* A number with a digits pattern like "#,##0.00" */
static void fmt_number(const char *s, int n, double v, bool neg_section, char *out, size_t len)
{
    /* the pattern's parts */
    int int_zero = 0, dec = 0, dec_opt = 0;
    bool group = false, pct = false, sci = false, in_dec = false;
    int scale = 0, exp_digits = 0;
    bool exp_plus = false;
    int first = -1, last = -1;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '"') { i++; while (i < n && s[i] != '"') i++; continue; }
        if (c == '\\' || c == '_' || c == '*') { i++; continue; }
        if (c == '[') { while (i < n && s[i] != ']') i++; continue; }
        if (c == '0' || c == '#' || c == '?') {
            if (first < 0) first = i;
            last = i;
            if (sci && exp_digits >= 0 && i > 0 && first >= 0) {}
            if (in_dec) { if (c == '0') dec++; else dec_opt++; }
            else if (c == '0' && !sci) int_zero++;
            continue;
        }
        if (c == '.' && first >= 0 && !sci) { in_dec = true; last = i; continue; }
        if (c == '.' && first < 0) { in_dec = true; first = last = i; continue; }
        if (c == ',' && first >= 0) {
            /* grouping between placeholders, scaling at the end */
            bool more = false;
            for (int j = i + 1; j < n; j++) {
                if (s[j] == '0' || s[j] == '#' || s[j] == '?') { more = true; break; }
                if (s[j] != ',') break;
            }
            if (more && !in_dec) group = true;
            else if (!more) scale++;
            last = i;
            continue;
        }
        if (c == '%') pct = true;
        if ((c == 'E' || c == 'e') && first >= 0 && i + 1 < n && (s[i + 1] == '+' || s[i + 1] == '-')) {
            sci = true;
            exp_plus = s[i + 1] == '+';
            int j = i + 2;
            while (j < n && (s[j] == '0' || s[j] == '#')) { exp_digits++; j++; }
            last = j - 1;
            i = j - 1;
            continue;
        }
    }
    if (pct) v *= 100;
    for (int i = 0; i < scale; i++) v /= 1000;
    bool neg = v < 0;
    if (neg) v = -v;
    char num[80];
    if (first < 0) {
        num[0] = 0;
    } else if (sci) {
        int e = 0;
        double m = v;
        if (m != 0) {
            while (m >= 10) { m /= 10; e++; }
            while (m < 1) { m *= 10; e--; }
        }
        char mant[40];
        snprintf(mant, sizeof mant, "%.*f", dec + dec_opt, m);
        if (mant[0] == '1' && mant[1] == '0' && (mant[2] == '.' || !mant[2])) {   /* 9.99 rounded up to 10.0 */
            m /= 10;
            e++;
            snprintf(mant, sizeof mant, "%.*f", dec + dec_opt, m);
        }
        snprintf(num, sizeof num, "%sE%s%0*d", mant, e < 0 ? "-" : exp_plus ? "+" : "", exp_digits ? exp_digits : 1,
                 e < 0 ? -e : e);
        localise(num);
    } else {
        char raw[64];
        int places = dec + dec_opt;
        snprintf(raw, sizeof raw, "%.*f", places, v);
        /* optional decimals drop their zeros */
        char *dot = strchr(raw, '.');
        if (dot && dec_opt) {
            char *z = raw + strlen(raw);
            while (z > dot + 1 + dec && z[-1] == '0') *--z = 0;
            if (z == dot + 1) *dot = 0;
        }
        char ip[64];
        size_t il = dot ? (size_t)(dot - raw) : strlen(raw);
        memcpy(ip, raw, il);
        ip[il] = 0;
        /* "0" in the integer part: at least that many digits; "#" alone: none for 0 */
        if (int_zero == 0 && !strcmp(ip, "0")) ip[0] = 0;
        while ((int)strlen(ip) < int_zero) {
            memmove(ip + 1, ip, strlen(ip) + 1);
            ip[0] = '0';
        }
        char g = of_locale_comma() ? '.' : ',', d = of_locale_comma() ? ',' : '.';
        size_t k = 0;
        int L = (int)strlen(ip);
        for (int i = 0; i < L && k < sizeof num - 2; i++) {
            num[k++] = ip[i];
            if (group && (L - i - 1) % 3 == 0 && i < L - 1) num[k++] = g;
        }
        if (dot && dot[1]) {
            num[k++] = d;
            for (const char *p = dot + 1; *p && k < sizeof num - 1; p++) num[k++] = *p;
        }
        num[k] = 0;
    }
    /* the literals around it */
    size_t k = 0;
    out[0] = 0;
    if (neg && !neg_section) put(out, len, &k, "-", 1);
    for (int i = 0; i < n; i++) {
        if (i == first) {
            put(out, len, &k, num, strlen(num));
            i = last;
            continue;
        }
        char c = s[i];
        if (c == '"') {
            int j = i + 1;
            while (j < n && s[j] != '"') j++;
            put(out, len, &k, s + i + 1, (size_t)(j - i - 1));
            i = j;
            continue;
        }
        if (c == '\\' && i + 1 < n) { put(out, len, &k, s + i + 1, (size_t)of_utf8_len((uint8_t)s[i + 1])); i += of_utf8_len((uint8_t)s[i + 1]); continue; }
        if (c == '_' && i + 1 < n) { put(out, len, &k, " ", 1); i++; continue; }
        if (c == '*' && i + 1 < n) { i++; continue; }
        if (c == '[') {
            int j = i;
            while (j < n && s[j] != ']') j++;
            /* [$€-C0A] or [$USD] : the symbol */
            if (i + 1 < n && s[i + 1] == '$') {
                int a = i + 2, b = a;
                while (b < j && s[b] != '-') b++;
                put(out, len, &k, s + a, (size_t)(b - a));
            }
            i = j;
            continue;
        }
        if (c == '@') continue;
        put(out, len, &k, &s[i], 1);
    }
}

void of_numfmt_format(const char *code, const of_val_t *v, bool date1904, char *out, size_t len, bool *right)
{
    out[0] = 0;
    *right = v->t == OV_NUM;
    if (v->t == OV_STR) {
        /* the fourth section, or one with "@", formats text */
        const char *sec[4];
        int sl[4];
        int n = sections(code, sec, sl);
        const char *t = v->s ? v->s : "";
        int use = n == 4 ? 3 : -1;
        for (int i = 0; i < n && use < 0; i++)
            if (memchr(sec[i], '@', (size_t)sl[i])) use = i;
        if (use < 0) { snprintf(out, len, "%s", t); return; }
        size_t k = 0;
        for (int i = 0; i < sl[use]; i++) {
            char c = sec[use][i];
            if (c == '@') put(out, len, &k, t, strlen(t));
            else if (c == '"') {
                int j = i + 1;
                while (j < sl[use] && sec[use][j] != '"') j++;
                put(out, len, &k, sec[use] + i + 1, (size_t)(j - i - 1));
                i = j;
            } else if (c == '\\' && i + 1 < sl[use]) { put(out, len, &k, sec[use] + i + 1, 1); i++; }
            else if (c == '_') { put(out, len, &k, " ", 1); i++; }
            else put(out, len, &k, &c, 1);
        }
        return;
    }
    if (v->t != OV_NUM) return;
    double n = v->n;
    if (!code || !*code || !strcasecmp(code, "General") || !strcmp(code, "@")) {
        fmt_general_display(n, out, len);
        return;
    }
    const char *sec[4];
    int sl[4];
    int ns = sections(code, sec, sl);
    if (!ns) { fmt_general_display(n, out, len); return; }
    int use = 0;
    bool neg_section = false;
    if (ns >= 3 && n == 0) use = 2;
    else if (ns >= 2 && n < 0) { use = 1; neg_section = true; }
    if (is_date_section(sec[use], sl[use])) {
        fmt_date(sec[use], sl[use], n, date1904, out, len);
        return;
    }
    /* a section that is "General" with text around it */
    const char *gp = NULL;
    for (int i = 0; i + 7 <= sl[use]; i++)
        if (!strncasecmp(sec[use] + i, "General", 7)) { gp = sec[use] + i; break; }
    if (gp) {
        char g[40];
        fmt_general_display(neg_section && n < 0 ? -n : n, g, sizeof g);
        snprintf(out, len, "%.*s%s%.*s", (int)(gp - sec[use]), sec[use], g,
                 (int)(sec[use] + sl[use] - gp - 7), gp + 7);
        return;
    }
    fmt_number(sec[use], sl[use], neg_section ? (n < 0 ? -n : n) : n, neg_section, out, len);
}

uint32_t of_book_xf_with_numfmt(of_book_t *b, uint32_t xf, const char *code)
{
    /* the format's id: a built-in one, a custom one already there, or new */
    uint32_t id = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof BUILTIN / sizeof BUILTIN[0]; i++)
        if (!strcmp(BUILTIN[i].code, code) && BUILTIN[i].id != 14 && BUILTIN[i].id != 22) id = BUILTIN[i].id;
    for (int i = 0; i < b->nnf && id == 0xFFFFFFFFu; i++)
        if (!strcmp(b->nf[i].code, code)) id = b->nf[i].id;
    if (id == 0xFFFFFFFFu) {
        uint32_t next = 164;
        for (int i = 0; i < b->nnf; i++)
            if (b->nf[i].id >= next) next = b->nf[i].id + 1;
        of_numfmt_t *nf = of_realloc(b->nf, (size_t)(b->nnf + 1) * sizeof *nf);
        if (!nf) return xf;
        b->nf = nf;
        b->nf[b->nnf].id = next;
        b->nf[b->nnf].code = of_strdup(code);
        b->nnf++;
        id = next;
        b->styles_dirty = true;
    }
    of_xf_t base = { 0 };
    if (xf < (uint32_t)b->nxf) base = b->xf[xf];
    if (base.numfmt == id) return xf;
    uint32_t root = xf < (uint32_t)b->nxf && (int)xf >= b->nxf_orig ? b->xf[xf].from : xf;
    /* one added before, from the same xf, with this format */
    for (int i = b->nxf_orig; i < b->nxf; i++)
        if (b->xf[i].from == root && b->xf[i].numfmt == id) return (uint32_t)i;
    of_xf_t *nx = of_realloc(b->xf, (size_t)(b->nxf + 1) * sizeof *nx);
    if (!nx) return xf;
    b->xf = nx;
    base.numfmt = id;
    base.from = root;
    b->xf[b->nxf++] = base;
    b->styles_dirty = true;
    return (uint32_t)(b->nxf - 1);
}

void of_numfmt_info(const char *code, of_nfinfo_t *o)
{
    memset(o, 0, sizeof *o);
    if (!code || !*code || !strcasecmp(code, "General")) return;
    if (!strcmp(code, "@")) { o->kind = 6; return; }
    const char *sec[4];
    int sl[4];
    int n = sections(code, sec, sl);
    if (!n) return;
    const char *s = sec[0];
    int len = sl[0];
    if (is_date_section(s, len)) {
        bool date = false, time = false;
        for (int i = 0; i < len; i++) {
            char c = (char)(s[i] | 0x20);
            if (s[i] == '"') { i++; while (i < len && s[i] != '"') i++; continue; }
            if (c == 'y' || c == 'd') date = true;
            if (c == 'h' || c == 's') time = true;
        }
        if (!date && !time) date = true;
        o->kind = date && time ? 5 : time ? 4 : 3;
        return;
    }
    o->kind = 1;
    int first = -1, last = -1;
    bool in_dec = false;
    for (int i = 0; i < len; i++) {
        char c = s[i];
        if (c == '"') { i++; while (i < len && s[i] != '"') i++; continue; }
        if (c == '\\' || c == '_' || c == '*') { i++; continue; }
        if (c == '[') { while (i < len && s[i] != ']') i++; continue; }
        if (c == '0' || c == '#' || c == '?') {
            if (first < 0) first = i;
            last = i;
            if (in_dec) o->decimals++;
        } else if (c == '.' && first >= 0) { in_dec = true; last = i; }
        else if (c == ',' && first >= 0 && !in_dec) o->grouping = true;
        else if (c == '%') o->kind = 2;
    }
    /* the literals before and after the digits */
    char tmp[64];
    of_val_t zero = { .t = OV_NUM, .n = 0 };
    (void)zero;
    for (int part = 0; part < 2; part++) {
        int a = part ? last + 1 : 0, z = part ? len : first;
        size_t k = 0;
        for (int i = a; i < z && i >= 0; i++) {
            char c = s[i];
            if (c == '"') {
                i++;
                while (i < z && s[i] != '"' && k < sizeof tmp - 1) tmp[k++] = s[i++];
                continue;
            }
            if (c == '\\' && i + 1 < z) { if (k < sizeof tmp - 1) tmp[k++] = s[++i]; continue; }
            if (c == '_') { i++; if (k < sizeof tmp - 1) tmp[k++] = ' '; continue; }
            if (c == '*') { i++; continue; }
            if (c == '[') {
                int j = i;
                while (j < z && s[j] != ']') j++;
                if (i + 1 < z && s[i + 1] == '$')
                    for (int q = i + 2; q < j && s[q] != '-' && k < sizeof tmp - 1; q++) tmp[k++] = s[q];
                i = j;
                continue;
            }
            if (c == '%' || c == '-' || c == '(' || c == ')') continue;
            if (k < sizeof tmp - 1) tmp[k++] = c;
        }
        tmp[k] = 0;
        snprintf(part ? o->suffix : o->prefix, 16, "%.15s", tmp);
    }
}
