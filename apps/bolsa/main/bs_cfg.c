/*
 * P4OS - Bolsa: the configuration, <data>/bolsa/bolsa.txt.
 *
 * One text file that the app and its page in the portal both write, one
 * thing per line:
 *
 *   refresh 5                       quotes every 5 minutes
 *   prov twelve                     the preferred provider for shares
 *   key twelve <the user's key>     typed by the user on the page
 *   view type=1 range=2 vol=1 ma=0 rsi=0 list=0
 *   list Mías: cg:bitcoin,USD.BLUE,RIESGO.AR
 *   alert cg:bitcoin > 100000
 *   alert USD.BLUE < 1400 fired
 *
 * The keys live only here, on the card, where the user put them: never in
 * the code. The app looks at the file's date in its tick and reloads it
 * when the page saved.
 *
 * names.txt next to it remembers the names the searches found
 * ("YERBA.BA\tYERBA|Yerba Norte S.A."), so a share added once keeps its
 * name with no network.
 */
#include "bs.h"

#include <sys/stat.h>
#include <errno.h>

static bs_cfg_t s_cfg;
static long s_mtime = -1;

bs_cfg_t *bs_cfg(void) { return &s_cfg; }

void bs_scpy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t i = 0;
    if (src)
        for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

const char *bs_data_dir(void)
{
    static char dir[160];
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "%.140s/bolsa", aos_hal_path_data());
        mkdir(aos_hal_path_data(), 0775);
        mkdir(dir, 0775);
        char c[180];
        snprintf(c, sizeof c, "%s/c", dir);
        mkdir(c, 0775);
    }
    return dir;
}

static void path(char *out, size_t n, const char *file)
{
    snprintf(out, n, "%.150s/%s", bs_data_dir(), file);
}

static long file_mtime(void)
{
    char p[200];
    struct stat st;
    path(p, sizeof p, "bolsa.txt");
    return stat(p, &st) == 0 ? (long)st.st_mtime : -1;
}

static const char *PROV_WORD[BS_P_COUNT] = { "auto", "coingecko", "argdatos", "stooq", "twelve", "alpha", "finnhub" };

static int prov_from_word(const char *w)
{
    for (int i = 0; i < BS_P_COUNT; i++)
        if (!strcmp(w, PROV_WORD[i])) return i;
    return -1;
}

static void add_list(bs_cfg_t *c, const char *name, const char *syms)
{
    if (c->nlists >= BS_MAX_LISTS) return;
    bs_list_t *l = &c->list[c->nlists++];
    memset(l, 0, sizeof *l);
    bs_scpy(l->name, sizeof l->name, name);
    char buf[BS_MAX_SYMS * BS_SYM_LEN];
    bs_scpy(buf, sizeof buf, syms);
    for (char *p = buf; *p && l->n < BS_MAX_SYMS;) {
        char *comma = strchr(p, ',');
        if (comma) *comma = 0;
        char s[BS_SYM_LEN];
        while (*p == ' ') p++;
        if (bs_sym_clean(p, s, sizeof s)) {
            bool dup = false;
            for (int i = 0; i < l->n; i++) dup |= !strcmp(l->sym[i], s);
            if (!dup) bs_scpy(l->sym[l->n++], BS_SYM_LEN, s);
        }
        if (!comma) break;
        p = comma + 1;
    }
}

void bs_cfg_defaults(bs_cfg_t *c)
{
    memset(c, 0, sizeof *c);
    c->refresh_min = 5;
    c->chart_type = BS_CT_AREA;
    c->range = 2;               /* 1M */
    c->show_vol = true;
    /* The factory lists hold no companies (bs_sym.c): the user's shares
     * are added with the search. The names are keys of the catalog, so
     * they are shown translated while the user does not rename them. */
    add_list(c, N_("Mías"), "cg:bitcoin,USD.BLUE,RIESGO.AR,^MERV,^GSPC");
    add_list(c, N_("Argentina"), "^MERV,USD.OFICIAL,USD.BLUE,USD.MEP,USD.CCL,USD.CRIPTO,USD.MAYOR,RIESGO.AR,EUR.ARS,BRL.ARS");
    add_list(c, N_("EE. UU."), "^GSPC,^DJI,^IXIC");
    add_list(c, N_("Cripto"), "cg:bitcoin,cg:ethereum,cg:solana,cg:ripple,cg:cardano,cg:dogecoin,cg:litecoin,cg:polkadot");
    add_list(c, N_("Índices"), "^GSPC,^DJI,^IXIC,^MERV,^BVSP,^FTSE,^GDAXI,^N225");
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static void parse_view(bs_cfg_t *c, char *v)
{
    for (char *tok = v; tok && *tok;) {
        char *sp = strchr(tok, ' ');
        if (sp) *sp = 0;
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = 0;
            int x = atoi(eq + 1);
            if (!strcmp(tok, "type") && x >= 0 && x < BS_CT_COUNT) c->chart_type = (uint8_t)x;
            else if (!strcmp(tok, "range") && x >= 0 && x < 7) c->range = (uint8_t)x;
            else if (!strcmp(tok, "vol")) c->show_vol = x != 0;
            else if (!strcmp(tok, "ma")) c->show_ma = x != 0;
            else if (!strcmp(tok, "rsi")) c->show_rsi = x != 0;
            else if (!strcmp(tok, "list")) c->cur_list = x;
        }
        tok = sp ? sp + 1 : NULL;
    }
}

static void parse_alert(bs_cfg_t *c, char *v)
{
    if (c->nalerts >= BS_MAX_ALERTS) return;
    char sym[BS_SYM_LEN] = {0}, op[4] = {0}, rest[16] = {0};
    char num[32] = {0};
    if (sscanf(v, "%23s %3s %31s %15s", sym, op, num, rest) < 3) return;
    bs_alert_t *a = &c->alert[c->nalerts];
    if (!bs_sym_clean(sym, a->sym, sizeof a->sym)) return;
    if (!strcmp(op, ">")) a->dir = BS_AL_ABOVE;
    else if (!strcmp(op, "<")) a->dir = BS_AL_BELOW;
    else return;
    for (char *p = num; *p; p++) if (*p == ',') *p = '.';
    char *end = NULL;
    a->value = strtof(num, &end);
    if (end == num || !(a->value > 0)) return;
    a->fired = !strcmp(rest, "fired");
    c->nalerts++;
}

void bs_cfg_load(void)
{
    char p[200];
    path(p, sizeof p, "bolsa.txt");
    FILE *f = fopen(p, "r");
    int keep_list = s_cfg.cur_list;
    bool had = s_mtime >= 0;
    if (!f) {
        bs_cfg_defaults(&s_cfg);
        s_mtime = -1;
        bs_cfg_save();
        return;
    }
    bs_cfg_t *c = &s_cfg;
    bs_cfg_defaults(c);
    c->nlists = 0;
    bool any_list = false;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (!line[0] || line[0] == '#') continue;
        char *sp = strchr(line, ' ');
        if (!sp) continue;
        *sp = 0;
        char *v = sp + 1;
        while (*v == ' ') v++;
        if (!strcmp(line, "refresh")) {
            int m = atoi(v);
            c->refresh_min = m < 1 ? 1 : m > 60 ? 60 : m;
        } else if (!strcmp(line, "prov")) {
            int pv = prov_from_word(v);
            c->prov = (uint8_t)(pv > 0 ? pv : 0);
        } else if (!strcmp(line, "key")) {
            char *k = strchr(v, ' ');
            if (!k) continue;
            *k++ = 0;
            int pv = prov_from_word(v);
            /* the key is kept as typed, but nothing that would break the
             * URL or a header: keys are letters, digits and a few signs */
            if (pv > 0) {
                char clean[BS_KEY_LEN];
                size_t w = 0;
                for (char *q = k; *q && w + 1 < sizeof clean; q++)
                    if ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z') || (*q >= '0' && *q <= '9') ||
                        *q == '-' || *q == '_' || *q == '.')
                        clean[w++] = *q;
                clean[w] = 0;
                bs_scpy(c->key[pv], BS_KEY_LEN, clean);
            }
        } else if (!strcmp(line, "view")) {
            parse_view(c, v);
        } else if (!strcmp(line, "list")) {
            char *colon = strchr(v, ':');
            if (!colon) continue;
            *colon = 0;
            trim(v);
            any_list = true;
            add_list(c, v, colon + 1);
        } else if (!strcmp(line, "alert")) {
            parse_alert(c, v);
        }
    }
    fclose(f);
    if (!any_list) {
        bs_cfg_t d;
        bs_cfg_defaults(&d);
        memcpy(c->list, d.list, sizeof d.list);
        c->nlists = d.nlists;
    }
    if (had) c->cur_list = keep_list;
    if (c->cur_list < 0 || c->cur_list >= c->nlists) c->cur_list = 0;
    s_mtime = file_mtime();
}

bool bs_cfg_save(void)
{
    char p[200], tmp[210];
    path(p, sizeof p, "bolsa.txt");
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    bs_cfg_t *c = &s_cfg;
    fputs("# Bolsa (aos.bolsa): written by the app and by its page in the portal.\n", f);
    fprintf(f, "refresh %d\n", c->refresh_min);
    fprintf(f, "prov %s\n", PROV_WORD[c->prov < BS_P_COUNT ? c->prov : 0]);
    for (int i = 1; i < BS_P_COUNT; i++)
        if (c->key[i][0]) fprintf(f, "key %s %s\n", PROV_WORD[i], c->key[i]);
    fprintf(f, "view type=%d range=%d vol=%d ma=%d rsi=%d list=%d\n", c->chart_type, c->range,
            c->show_vol, c->show_ma, c->show_rsi, c->cur_list);
    for (int i = 0; i < c->nlists; i++) {
        fprintf(f, "list %s:", c->list[i].name);
        for (int k = 0; k < c->list[i].n; k++) fprintf(f, "%s%s", k ? "," : "", c->list[i].sym[k]);
        fputs("\n", f);
    }
    char num[32];
    for (int i = 0; i < c->nalerts; i++) {
        bs_alert_t *a = &c->alert[i];
        /* plain decimal point here, whatever the language */
        int32_t ip = (int32_t)a->value;
        int32_t fr = (int32_t)roundf((a->value - (float)ip) * 10000.0f);
        if (fr >= 10000) { ip++; fr -= 10000; }
        snprintf(num, sizeof num, "%ld.%04ld", (long)ip, (long)fr);
        fprintf(f, "alert %s %s %s%s\n", a->sym, a->dir == BS_AL_ABOVE ? ">" : "<", num,
                a->fired ? " fired" : "");
    }
    fclose(f);
    remove(p);
    bool ok = rename(tmp, p) == 0;
    s_mtime = file_mtime();
    return ok;
}

bool bs_cfg_changed(void)
{
    long m = file_mtime();
    return m != s_mtime;
}

bool bs_cfg_has(int list, const char *sym)
{
    if (list < 0 || list >= s_cfg.nlists) return false;
    for (int i = 0; i < s_cfg.list[list].n; i++)
        if (!strcmp(s_cfg.list[list].sym[i], sym)) return true;
    return false;
}

bool bs_cfg_add(int list, const char *sym)
{
    if (list < 0 || list >= s_cfg.nlists || bs_cfg_has(list, sym)) return false;
    bs_list_t *l = &s_cfg.list[list];
    if (l->n >= BS_MAX_SYMS) return false;
    bs_scpy(l->sym[l->n++], BS_SYM_LEN, sym);
    return true;
}

void bs_cfg_remove(int list, int idx)
{
    if (list < 0 || list >= s_cfg.nlists) return;
    bs_list_t *l = &s_cfg.list[list];
    if (idx < 0 || idx >= l->n) return;
    memmove(l->sym[idx], l->sym[idx + 1], (size_t)(l->n - idx - 1) * BS_SYM_LEN);
    l->n--;
}

void bs_cfg_move(int list, int idx, int to)
{
    if (list < 0 || list >= s_cfg.nlists) return;
    bs_list_t *l = &s_cfg.list[list];
    if (idx < 0 || idx >= l->n || to < 0 || to >= l->n || to == idx) return;
    char s[BS_SYM_LEN];
    bs_scpy(s, sizeof s, l->sym[idx]);
    if (to < idx) memmove(l->sym[to + 1], l->sym[to], (size_t)(idx - to) * BS_SYM_LEN);
    else memmove(l->sym[idx], l->sym[idx + 1], (size_t)(to - idx) * BS_SYM_LEN);
    bs_scpy(l->sym[to], BS_SYM_LEN, s);
}

int bs_alert_find(const char *sym, int from)
{
    for (int i = from < 0 ? 0 : from; i < s_cfg.nalerts; i++)
        if (!strcmp(s_cfg.alert[i].sym, sym)) return i;
    return -1;
}

/* ---- learned names ---- */

#define NAMES_MAX 128
static struct { char sym[BS_SYM_LEN]; char name[BS_NAME_LEN + 16]; } s_names[NAMES_MAX];
static int s_nnames;
static bool s_names_loaded;

void bs_names_load(void)
{
    s_nnames = 0;
    s_names_loaded = true;
    char p[200];
    path(p, sizeof p, "names.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[160];
    while (s_nnames < NAMES_MAX && fgets(line, sizeof line, f)) {
        trim(line);
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        bs_scpy(s_names[s_nnames].sym, BS_SYM_LEN, line);
        bs_scpy(s_names[s_nnames].name, sizeof s_names[0].name, tab + 1);
        s_nnames++;
    }
    fclose(f);
}

const char *bs_names_get(const char *sym)
{
    if (!s_names_loaded) bs_names_load();
    for (int i = 0; i < s_nnames; i++)
        if (!strcmp(s_names[i].sym, sym)) return s_names[i].name;
    return NULL;
}

void bs_names_put(const char *sym, const char *name)
{
    if (!s_names_loaded) bs_names_load();
    int i;
    for (i = 0; i < s_nnames; i++)
        if (!strcmp(s_names[i].sym, sym)) break;
    if (i == s_nnames) {
        if (s_nnames >= NAMES_MAX) {        /* the oldest goes */
            memmove(&s_names[0], &s_names[1], sizeof s_names[0] * (NAMES_MAX - 1));
            i = NAMES_MAX - 1;
        } else {
            s_nnames++;
        }
        bs_scpy(s_names[i].sym, BS_SYM_LEN, sym);
    } else if (!strcmp(s_names[i].name, name)) {
        return;
    }
    bs_scpy(s_names[i].name, sizeof s_names[0].name, name);
    char p[200];
    path(p, sizeof p, "names.txt");
    FILE *f = fopen(p, "w");
    if (!f) return;
    for (int k = 0; k < s_nnames; k++) fprintf(f, "%s\t%s\n", s_names[k].sym, s_names[k].name);
    fclose(f);
}
