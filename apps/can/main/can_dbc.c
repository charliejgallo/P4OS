/*
 * P4OS - CAN: the .dbc files.
 *
 * The signals come from a DBC file, the format of Vector's CANdb++ that
 * SavvyCAN, cantools and opendbc use too. The one in use is
 * <card>/can/senales.dbc (the app writes an example on its first run) or
 * any of <card>/can/dbc/, a folder the user fills with downloaded files.
 * Which one, and which of its messages the Signals tab shows, is in
 * <data>/can_dbc.txt, which the portal page writes as well:
 *
 *     file dbc/toyota_nodsu_pt_generated.dbc
 *     show bus                    the messages seen on the bus (the default)
 *     show 0B4 1C4 18FEF100       these ones (candump's form of the id)
 *
 * What is read of a .dbc:
 *   BO_ <id> <name>: <length> <sender>     a message; bit 31 of the id says
 *                                          29-bit
 *   SG_ <name> [M|m<k>] : <start>|<length>@<order><sign> (<scale>,<offset>)
 *       [<min>|<max>] "<unit>" <receivers>
 *                                          its signals; "M" is the
 *                                          multiplexor, "m3" a signal that
 *                                          is there only when it reads 3
 *   SG_MUL_VAL_ <id> <signal> <multiplexor> 1-1;
 *                                          extended multiplexing: which
 *                                          multiplexor ("m1M", itself
 *                                          multiplexed) and which values a
 *                                          signal is there for (the first
 *                                          range)
 *   VAL_ <id> <signal> 0 "P" 1 "R" ... ;   names for a signal's values
 * Everything else is skipped, the comments' strings across lines included.
 * Signals over 32 bits are counted and skipped (the firmware's table has no
 * 64-bit shifts).
 *
 * The file is read whole into PSRAM (up to 4 MB: opendbc's biggest are a
 * few hundred KB), counted once, and parsed into tables of the sizes it
 * needs, so lines have no length limit and a big file costs what it has.
 */
#include "can.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

cn_dbc_t CN_DBC;

static const char *sel_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/can_dbc.txt", aos_hal_path_data());
    return out;
}

static bool is_sp(char c) { return c == ' ' || c == '\t' || c == '\r'; }
static const char *skip_sp(const char *s) { while (is_sp(*s)) s++; return s; }

/* ---- the selection ---- */

static void sel_defaults(void)
{
    snprintf(CN_DBC.file, sizeof CN_DBC.file, "%s", "senales.dbc");
    CN_DBC.show_bus = true;
    CN_DBC.nshow = 0;
}

static void sel_load(void)
{
    char path[128], line[600];
    sel_defaults();
    FILE *f = fopen(sel_path(path, sizeof path), "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        const char *s = skip_sp(line);
        if (!strncmp(s, "file ", 5)) {
            s = skip_sp(s + 5);
            /* only names under <card>/can, and nothing that climbs out */
            if (*s && *s != '/' && !strstr(s, "..")) snprintf(CN_DBC.file, sizeof CN_DBC.file, "%.63s", s);
        } else if (!strncmp(s, "show ", 5)) {
            s = skip_sp(s + 5);
            CN_DBC.nshow = 0;
            CN_DBC.show_bus = !strncmp(s, "bus", 3);
            while (!CN_DBC.show_bus && *s && CN_DBC.nshow < CN_SHOW_MAX) {
                char *e;
                unsigned long v = strtoul(s, &e, 16);
                if (e == s) break;
                CN_DBC.show[CN_DBC.nshow] = (uint32_t)v & 0x1FFFFFFFu;
                CN_DBC.show_ext[CN_DBC.nshow] = e - s > 3 || v > 0x7FF;
                CN_DBC.nshow++;
                s = skip_sp(e);
            }
            if (!CN_DBC.show_bus && !CN_DBC.nshow) CN_DBC.show_bus = true;
        }
    }
    fclose(f);
}

void cn_dbc_save_sel(void)
{
    char path[128], part[136];
    mkdir(aos_hal_path_data(), 0777);
    snprintf(part, sizeof part, "%s.part", sel_path(path, sizeof path));
    FILE *f = fopen(part, "w");
    if (!f) return;
    fputs("# CAN: el .dbc de la pestana Senales (en <tarjeta>/can) y los mensajes que muestra.\n"
          "# show bus: los que pasan por el bus. show 0B4 1C4: esos ids.\n", f);
    fprintf(f, "file %s\n", CN_DBC.file);
    if (CN_DBC.show_bus || !CN_DBC.nshow) fputs("show bus\n", f);
    else {
        fputs("show", f);
        for (int i = 0; i < CN_DBC.nshow; i++)
            fprintf(f, CN_DBC.show_ext[i] ? " %08X" : " %03X", (unsigned)CN_DBC.show[i]);
        fputs("\n", f);
    }
    fclose(f);
    unlink(path);
    rename(part, path);
}

/* ---- the files ---- */

static int name_cmp(const void *a, const void *b) { return strcasecmp(a, b); }

int cn_dbc_files(char (*out)[64], uint32_t *sizes, int max)
{
    char dir[128], p[320];
    struct stat sb;
    int n = 0;
    snprintf(p, sizeof p, "%s/senales.dbc", cn_dir());
    if (max > 0 && stat(p, &sb) == 0) {
        snprintf(out[n], 64, "%s", "senales.dbc");
        sizes[n++] = (uint32_t)sb.st_size;
    }
    snprintf(dir, sizeof dir, "%s/dbc", cn_dir());
    mkdir(dir, 0777);
    DIR *d = opendir(dir);
    if (!d) return n;
    int first = n;
    struct dirent *de;
    while ((de = readdir(d)) && n < max) {
        const char *nm = de->d_name;
        size_t l = strlen(nm);
        if (nm[0] == '.' || l < 5 || l > 59 || strcasecmp(nm + l - 4, ".dbc")) continue;
        snprintf(out[n++], 64, "dbc/%.59s", nm);
    }
    closedir(d);
    qsort(out[first], (size_t)(n - first), 64, name_cmp);
    for (int i = first; i < n; i++) {
        snprintf(p, sizeof p, "%s/%s", cn_dir(), out[i]);
        sizes[i] = stat(p, &sb) == 0 ? (uint32_t)sb.st_size : 0;
    }
    return n;
}

/* ---- parsing ---- */

/* BO_ 192 Motor: 8 Motor */
static bool parse_bo(const char *s, cn_msg_t *m)
{
    char *e;
    s = skip_sp(s + 4);
    uint32_t raw = (uint32_t)strtoul(s, &e, 10);
    if (e == s) return false;
    s = skip_sp(e);
    int n = 0;
    while (*s && !is_sp(*s) && *s != ':') {
        if (n < (int)sizeof m->name - 1) m->name[n++] = *s;
        s++;
    }
    m->name[n] = 0;
    /* Vector's holder for the signals of no message */
    if (!strcmp(m->name, "VECTOR__INDEPENDENT_SIG_MSG")) return false;
    s = skip_sp(s);
    if (*s == ':') s = skip_sp(s + 1);
    unsigned long dlc = strtoul(s, &e, 10);
    m->dlc = (uint8_t)(e != s && dlc <= 64 ? dlc : 8);
    m->ext = raw & 0x80000000u;
    m->id = raw & 0x1FFFFFFFu;
    if (!m->ext && m->id > 0x7FF) m->ext = true;
    m->mux = -1;
    return true;
}

/* SG_ RPM : 23|16@0+ (0.25,0) [0|16000] "rpm" Vector__XXX
 * 0: not a signal line; -1: a signal the app cannot decode; 1: ok */
static int parse_sg(const char *s, cn_sig_t *g)
{
    s = skip_sp(s + 4);
    int n = 0;
    while (*s && !is_sp(*s) && *s != ':') {
        if (n < (int)sizeof g->name - 1) g->name[n++] = *s;
        s++;
    }
    g->name[n] = 0;
    if (!n) return 0;
    s = skip_sp(s);
    g->muxsig = -1;
    if (*s == 'M') {
        g->is_mux = true;
        s = skip_sp(s + 1);
    } else if (*s == 'm') {
        char *e;
        long k = strtol(s + 1, &e, 10);
        if (e == s + 1) return 0;
        g->muxed = true;
        g->mux_lo = g->mux_hi = (int32_t)k;
        s = e;
        if (*s == 'M') {                /* extended multiplexing: a multiplexor itself */
            g->is_mux = true;
            s++;
        }
        s = skip_sp(s);
    }
    if (*s != ':') return 0;
    char *e;
    s = skip_sp(s + 1);
    unsigned long start = strtoul(s, &e, 10);
    if (e == s || *e != '|') return 0;
    s = e + 1;
    unsigned long len = strtoul(s, &e, 10);
    if (e == s || *e != '@' || (e[1] != '0' && e[1] != '1') || (e[2] != '+' && e[2] != '-')) return 0;
    g->start = (uint16_t)start;
    g->len = (uint16_t)len;
    g->motorola = e[1] == '0';
    g->is_signed = e[2] == '-';
    s = strchr(e, '(');
    if (!s) return 0;
    g->scale = strtof(s + 1, &e);
    if (*e != ',') return 0;
    g->offset = strtof(e + 1, &e);
    g->unit[0] = 0;
    s = strchr(e, '"');
    if (s) {
        s++;
        n = 0;
        while (*s && *s != '"') {
            if (n < (int)sizeof g->unit - 1) g->unit[n++] = *s;
            s++;
        }
        g->unit[n] = 0;
    }
    if (g->len < 1 || g->len > 32 || start > 511) return -1;
    return 1;
}

/* the entries of VAL_ <id> <signal> 3 "D" 2 "N" ... ; NULL out: only count */
static int parse_val(const char *s, cn_val_t *out, int cap)
{
    int n = 0;
    char *e;
    for (;;) {
        s = skip_sp(s);
        long v = strtol(s, &e, 10);
        if (e == s) break;
        s = skip_sp(e);
        if (*s != '"') break;
        s++;
        const char *q = s;
        while (*s && *s != '"') s++;
        if (*s != '"') break;
        if (out && n < cap) {
            int l = (int)(s - q);
            if (l > (int)sizeof out[n].txt - 1) l = (int)sizeof out[n].txt - 1;
            memcpy(out[n].txt, q, (size_t)l);
            out[n].txt[l] = 0;
            out[n].raw = (int32_t)v;
        }
        n++;
        s++;
    }
    return out && n > cap ? cap : n;
}

/* a signal of message mi by its name (nl characters at nm), or -1 */
static int sig_by_name(int mi, const char *nm, int nl)
{
    if (mi < 0 || nl <= 0) return -1;
    const cn_msg_t *mm = &CN_DBC.msg[mi];
    for (uint32_t i = mm->sig0; i < mm->sig0 + mm->nsig; i++) {
        const char *sn = CN_DBC.sig[i].name;
        int sl = (int)strlen(sn);
        /* the names are cut at 31 */
        if (!strncmp(sn, nm, (size_t)sl) && (sl == nl || (sl == (int)sizeof CN_DBC.sig[i].name - 1 && nl > sl)))
            return (int)i;
    }
    return -1;
}

static int count_quotes(const char *s)
{
    int n = 0;
    for (; *s; s++) {
        if (*s == '\\' && s[1]) s++;
        else if (*s == '"') n++;
    }
    return n;
}

/* One walk over the file. With fill false it only counts what the tables
 * need; with it true it fills them (sized by the first walk). Each line is
 * cut at its end while it is looked at, and put back. */
static void walk(char *buf, bool fill, int *nmsg, int *nsig, int *nval, int *skipped)
{
    cn_dbc_t *D = &CN_DBC;
    int m = 0, g = 0, v = 0, sk = 0;
    bool in_msg = false, in_str = false;
    for (char *s = buf; *s;) {
        char *le = strchr(s, '\n');
        char save = 0;
        if (le) { save = *le; *le = 0; }
        const char *t = skip_sp(s);

        if (in_str) {
            if (count_quotes(t) & 1) in_str = false;
        } else if (!strncmp(t, "BO_ ", 4)) {
            cn_msg_t msg;
            memset(&msg, 0, sizeof msg);
            in_msg = parse_bo(t, &msg) && m < CN_DBC_MSG_MAX;
            if (in_msg) {
                if (fill) {
                    msg.sig0 = (uint32_t)g;
                    D->msg[m] = msg;
                }
                m++;
            }
        } else if (!strncmp(t, "SG_ ", 4)) {
            if (in_msg && g < CN_DBC_SIG_MAX) {
                cn_sig_t sig;
                memset(&sig, 0, sizeof sig);
                int r = parse_sg(t, &sig);
                if (r < 0) sk++;
                if (r > 0) {
                    if (fill) {
                        cn_msg_t *mm = &D->msg[m - 1];
                        sig.msg = (uint16_t)(m - 1);
                        sig.id = mm->id;
                        sig.ext = mm->ext;
                        sig.val0 = 0;
                        if (sig.is_mux && !sig.muxed && mm->mux < 0) mm->mux = g;
                        D->sig[g] = sig;
                        mm->nsig++;
                    }
                    g++;
                }
            }
        } else if (*t) {
            in_msg = false;
            if (!strncmp(t, "VAL_ ", 5)) {
                char *e;
                const char *p = skip_sp(t + 5);
                uint32_t raw = (uint32_t)strtoul(p, &e, 10);
                if (e != p) {
                    p = skip_sp(e);
                    const char *nm = p;
                    while (*p && !is_sp(*p)) p++;
                    int nl = (int)(p - nm);
                    if (!fill) {
                        int k = parse_val(p, NULL, 0);
                        if (v + k <= CN_DBC_VAL_MAX) v += k;
                    } else {
                        uint32_t id = raw & 0x1FFFFFFFu;
                        bool ext = (raw & 0x80000000u) || id > 0x7FF;
                        int k = sig_by_name(cn_dbc_find(id, ext), nm, nl);
                        cn_sig_t *hit = k >= 0 ? &D->sig[k] : NULL;
                        if (hit && !hit->nval) {
                            int k = parse_val(p, &D->val[v], D->nval - v);
                            hit->val0 = (uint32_t)v;
                            hit->nval = (uint16_t)k;
                            v += k;
                        }
                    }
                }
            } else if (!strncmp(t, "SG_MUL_VAL_ ", 12) && fill) {
                /* SG_MUL_VAL_ 2024 S1_PID_0C_EngineRPM ParameterID_Service01 12-12; */
                char *e;
                const char *p = skip_sp(t + 12);
                uint32_t raw = (uint32_t)strtoul(p, &e, 10);
                const char *nm[2];
                int nl[2] = { 0, 0 };
                p = skip_sp(e);
                for (int w = 0; w < 2; w++) {
                    nm[w] = p;
                    while (*p && !is_sp(*p)) p++;
                    nl[w] = (int)(p - nm[w]);
                    p = skip_sp(p);
                }
                uint32_t id = raw & 0x1FFFFFFFu;
                int mi = e != t + 12 ? cn_dbc_find(id, (raw & 0x80000000u) || id > 0x7FF) : -1;
                int a = sig_by_name(mi, nm[0], nl[0]), b = sig_by_name(mi, nm[1], nl[1]);
                long lo = strtol(p, &e, 10);
                if (a >= 0 && b >= 0 && a != b && e != p && *e == '-') {
                    p = e + 1;
                    long hi = strtol(p, &e, 10);
                    if (e != p) {
                        D->sig[a].muxed = true;
                        D->sig[a].muxsig = b;
                        D->sig[a].mux_lo = (int32_t)lo;
                        D->sig[a].mux_hi = (int32_t)hi;
                    }
                }
            } else if (count_quotes(t) & 1) {
                in_str = true;          /* a comment's string that goes on */
            }
        }

        if (!le) break;
        *le = save;
        s = le + 1;
    }
    *nmsg = m;
    *nsig = g;
    *nval = v;
    *skipped = sk;
}

void cn_dbc_free(void)
{
    cn_mux_set(NULL, 0);
    cn_big_free(CN_DBC.msg);
    cn_big_free(CN_DBC.sig);
    cn_big_free(CN_DBC.val);
    CN_DBC.msg = NULL;
    CN_DBC.sig = NULL;
    CN_DBC.val = NULL;
    CN_DBC.nmsg = CN_DBC.nsig = CN_DBC.nval = CN_DBC.skipped = 0;
    CN_DBC.bytes = 0;
}

void cn_dbc_load(char *err, int err_cap)
{
    char path[160];
    if (err) err[0] = 0;
    cn_dbc_free();
    sel_load();
    snprintf(path, sizeof path, "%s/%s", cn_dir(), CN_DBC.file);
    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err) snprintf(err, err_cap, _("No está can/%s"), CN_DBC.file);
        return;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > CN_DBC_FILE_MAX) {
        fclose(f);
        if (err) snprintf(err, err_cap, "%s", size > 0 ? _("El archivo pasa de 4 MB") : _("El archivo está vacío"));
        return;
    }
    char *buf = cn_big_alloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        if (err) snprintf(err, err_cap, "%s", _("No hay memoria para leerlo"));
        return;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = 0;
    CN_DBC.bytes = (uint32_t)got;

    int nm, ns, nv, sk;
    walk(buf, false, &nm, &ns, &nv, &sk);
    CN_DBC.msg = cn_big_alloc(sizeof(cn_msg_t) * (size_t)(nm ? nm : 1));
    CN_DBC.sig = cn_big_alloc(sizeof(cn_sig_t) * (size_t)(ns ? ns : 1));
    CN_DBC.val = cn_big_alloc(sizeof(cn_val_t) * (size_t)(nv ? nv : 1));
    if (!CN_DBC.msg || !CN_DBC.sig || !CN_DBC.val) {
        cn_big_free(buf);
        cn_dbc_free();
        if (err) snprintf(err, err_cap, "%s", _("No hay memoria para leerlo"));
        return;
    }
    /* the room parse_val may fill; and cn_dbc_find, which VAL_ and
     * SG_MUL_VAL_ use, looks as far as the messages go */
    CN_DBC.nval = nv;
    CN_DBC.nmsg = nm;
    CN_DBC.nsig = ns;
    walk(buf, true, &CN_DBC.nmsg, &CN_DBC.nsig, &CN_DBC.nval, &CN_DBC.skipped);
    CN_DBC.skipped = sk;
    cn_big_free(buf);

    /* the multiplexed messages, for the bus thread's cache */
    static cn_muxspec_t spec[CN_MUXSPEC];
    int ns_ = 0;
    for (int i = 0; i < CN_DBC.nmsg && ns_ < CN_MUXSPEC; i++) {
        const cn_msg_t *m = &CN_DBC.msg[i];
        cn_muxspec_t *sp = &spec[ns_];
        memset(sp, 0, sizeof *sp);
        for (uint32_t k = m->sig0; k < m->sig0 + m->nsig && sp->n < CN_MUXSIG; k++) {
            const cn_sig_t *g = &CN_DBC.sig[k];
            if (!g->is_mux) continue;
            sp->start[sp->n] = g->start;
            sp->len[sp->n] = g->len;
            sp->motorola[sp->n] = g->motorola;
            sp->n++;
        }
        if (!sp->n) continue;
        sp->id = m->id;
        sp->ext = m->ext;
        ns_++;
    }
    cn_mux_set(spec, ns_);
    aos_hal_log("can", "%s: %d messages, %d signals, %d value names, %d skipped", CN_DBC.file, CN_DBC.nmsg,
                CN_DBC.nsig, CN_DBC.nval, CN_DBC.skipped);
    if (!CN_DBC.nsig && err) snprintf(err, err_cap, "%s", _("El archivo no tiene señales (BO_ y SG_)"));
}

void cn_dbc_choose(const char *file)
{
    snprintf(CN_DBC.file, sizeof CN_DBC.file, "%.63s", file);
    CN_DBC.show_bus = true;
    CN_DBC.nshow = 0;
    cn_dbc_save_sel();
}

int cn_dbc_find(uint32_t id, bool ext)
{
    for (int i = 0; i < CN_DBC.nmsg; i++)
        if (CN_DBC.msg[i].id == id && CN_DBC.msg[i].ext == ext) return i;
    return -1;
}

bool cn_dbc_shown(int mi)
{
    if (mi < 0 || mi >= CN_DBC.nmsg) return false;
    const cn_msg_t *m = &CN_DBC.msg[mi];
    for (int i = 0; i < CN_DBC.nshow; i++)
        if (CN_DBC.show[i] == m->id && !CN_DBC.show_ext[i] == !m->ext) return true;
    return false;
}

/* ---- decoding ---- */

static int bit_at(const uint8_t *d, int pos) { return d[pos >> 3] >> (pos & 7) & 1; }

static bool bits(const cn_sig_t *g, const uint8_t *data, int len, uint32_t *out)
{
    /* 32 bits at most (parse_sg skips longer ones): 64-bit shifts and
     * conversions are not in the firmware's table */
    uint32_t raw = 0;
    if (!g->len) return false;
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
    *out = raw;
    return true;
}

bool cn_sig_raw(const cn_sig_t *g, const uint8_t *data, int len, int32_t *raw)
{
    uint32_t u;
    if (!bits(g, data, len, &u)) return false;
    if (g->is_signed && g->len < 32 && (u >> (g->len - 1) & 1)) u |= ~0u << g->len;
    *raw = (int32_t)u;
    return true;
}

static bool present(const cn_sig_t *g, const uint8_t *data, int len, int depth)
{
    if (!g->muxed || !CN_DBC.msg || g->msg >= CN_DBC.nmsg) return true;
    int32_t k = g->muxsig >= 0 ? g->muxsig : CN_DBC.msg[g->msg].mux;
    if (k < 0 || k >= CN_DBC.nsig || &CN_DBC.sig[k] == g) return true;
    const cn_sig_t *m = &CN_DBC.sig[k];
    /* a multiplexor that is multiplexed itself (m1M) has to be there too */
    if (depth < 4 && !present(m, data, len, depth + 1)) return false;
    int32_t raw;
    return cn_sig_raw(m, data, len, &raw) && raw >= g->mux_lo && raw <= g->mux_hi;
}

bool cn_sig_present(const cn_sig_t *g, const uint8_t *data, int len) { return present(g, data, len, 0); }

bool cn_sig_value(const cn_sig_t *g, const uint8_t *data, int len, float *out)
{
    uint32_t u;
    if (!bits(g, data, len, &u)) return false;
    float v;
    if (g->is_signed && g->len < 32 && (u >> (g->len - 1) & 1)) v = (float)(int32_t)(u | (~0u << g->len));
    else if (g->is_signed && g->len == 32) v = (float)(int32_t)u;
    else v = (float)u;
    *out = v * g->scale + g->offset;
    return true;
}

const char *cn_sig_val_name(const cn_sig_t *g, int32_t raw)
{
    if (!g->nval || !CN_DBC.val || g->val0 + g->nval > (uint32_t)CN_DBC.nval) return NULL;
    for (uint32_t i = g->val0; i < g->val0 + g->nval; i++)
        if (CN_DBC.val[i].raw == raw) return CN_DBC.val[i].txt;
    return NULL;
}

/* ---- the example ---- */

void cn_sig_write_example(void)
{
    char path[128];
    struct stat sb;
    snprintf(path, sizeof path, "%s/senales.dbc", cn_dir());
    if (stat(path, &sb) == 0) return;
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
