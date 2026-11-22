/*
 * P4OS - Traductor: the languages, and the packs on the card.
 *
 * A pack is traductor_<a>_<b>_p4.pak in the apps folder (maybe in parts,
 * .pak.1 ...), and says what it brings in its "info" entry:
 *
 *   pair=es-en
 *   nmt=esen,enes          a model each direction
 *   dict=esen,enes         a dictionary each direction
 *   version=1
 *
 * The models all go to or from English, so two packs translate between
 * their other languages through it (es -> en -> pt): twice the time.
 */
#include "td.h"
#include "td_pak.h"

#include <sys/stat.h>

static const td_lang_t LANGS[] = {
    {"es", N_("Español")},   {"en", N_("Inglés")},    {"pt", N_("Portugués")},
    {"it", N_("Italiano")},  {"fr", N_("Francés")},   {"de", N_("Alemán")},
    {"ca", N_("Catalán")},   {"gl", N_("Gallego")},   {"eu", N_("Euskera")},
    {"nl", N_("Neerlandés")}, {"sv", N_("Sueco")},    {"pl", N_("Polaco")},
    {"ru", N_("Ruso")},      {"uk", N_("Ucraniano")}, {"el", N_("Griego")},
    {"tr", N_("Turco")},     {"ar", N_("Árabe")},     {"he", N_("Hebreo")},
    {"hi", N_("Hindi")},     {"ja", N_("Japonés")},   {"ko", N_("Coreano")},
    {"zh", N_("Chino")},
};
#define NLANGS ((int)(sizeof LANGS / sizeof LANGS[0]))

void td_scpy(char *dst, size_t n, const char *src)
{
    if (!n) return;
    size_t i = 0;
    if (src)
        for (; src[i] && i + 1 < n; i++) dst[i] = src[i];
    dst[i] = 0;
}

uint32_t td_now_ms(void)
{
    return (uint32_t)aos_hal_uptime_ms();
}

const char *td_data_dir(void)
{
    static char dir[160];
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "%.140s/traductor", aos_hal_path_data());
        mkdir(aos_hal_path_data(), 0775);
        mkdir(dir, 0775);
    }
    return dir;
}

int td_lang_count(void) { return NLANGS; }

const td_lang_t *td_lang_at(int i)
{
    return i >= 0 && i < NLANGS ? &LANGS[i] : NULL;
}

int td_lang_find(const char *code)
{
    for (int i = 0; i < NLANGS; i++)
        if (!strcmp(LANGS[i].code, code)) return i;
    return -1;
}

const char *td_lang_name(const char *code)
{
    if (!strcmp(code, "auto")) return _("Detectar");
    int i = td_lang_find(code);
    return i >= 0 ? _(LANGS[i].name) : code;
}

/* ---- packs ---- */

static td_pack_t s_packs[TD_PACKS_MAX];
static int s_npacks;

static void split_list(const char *v, char out[4][5], int *n)
{
    *n = 0;
    while (*v && *n < 4) {
        int k = 0;
        while (v[k] && v[k] != ',' && v[k] != '\n' && k < 4) k++;
        if (k == 4) {
            memcpy(out[*n], v, 4);
            out[*n][4] = 0;
            (*n)++;
        }
        while (*v && *v != ',' && *v != '\n') v++;
        if (*v == ',') v++;
        else break;
    }
}

static void read_info(td_pack_t *k)
{
    td_pak_t p;
    if (!td_pak_open(&p, k->path)) return;
    const td_entry_t *e = td_pak_find(&p, "info");
    if (e && e->type == TD_T_INFO && e->len < 2048) {
        char *t = (char *)td_pak_load(&p, e);
        if (t) {
            char *buf = (char *)malloc(e->len + 1);
            if (buf) {
                memcpy(buf, t, e->len);
                buf[e->len] = 0;
                for (char *l = buf; l && *l;) {
                    char *nl = strchr(l, '\n');
                    if (nl) *nl = 0;
                    if (!strncmp(l, "nmt=", 4)) split_list(l + 4, k->nmt, &k->n_nmt);
                    else if (!strncmp(l, "dict=", 5)) split_list(l + 5, k->dict, &k->n_dict);
                    else if (!strncmp(l, "version=", 8)) td_scpy(k->version, sizeof k->version, l + 8);
                    l = nl ? nl + 1 : NULL;
                }
                free(buf);
            }
            td_free(t);
        }
    }
    for (int i = 0; i < p.parts; i++) k->bytes += p.part_len[i];
    td_pak_close(&p);
}

static bool scan_cb(const aos_dir_entry_t *e, void *ctx)
{
    (void)ctx;
    const char *n = e->name;
    size_t l = strlen(n);
    /* traductor_es_en_p4.pak: the first part only, no "._" of the Mac */
    if (e->dir || n[0] == '.' || l != strlen("traductor_es_en_p4.pak")) return true;
    if (strncmp(n, "traductor_", 10) || strcmp(n + l - 7, "_p4.pak") || n[12] != '_') return true;
    if (s_npacks >= TD_PACKS_MAX) return false;
    td_pack_t *k = &s_packs[s_npacks];
    memset(k, 0, sizeof *k);
    snprintf(k->path, sizeof k->path, "%.150s/%s", aos_hal_path_apps(), n);
    memcpy(k->pair, n + 10, 5);
    k->pair[2] = '-';
    k->pair[5] = 0;
    read_info(k);
    if (k->n_nmt || k->n_dict) s_npacks++;
    return true;
}

void td_packs_scan(void)
{
    s_npacks = 0;
    if (aos_hal_path_apps()) aos_hal_dir_scan(aos_hal_path_apps(), scan_cb, NULL);
    aos_hal_log("traductor", "%d language pack(s)", s_npacks);
}

int td_packs_count(void) { return s_npacks; }

const td_pack_t *td_pack_at(int i)
{
    return i >= 0 && i < s_npacks ? &s_packs[i] : NULL;
}

static const td_pack_t *find_dir(const char *src, const char *dst, bool dict)
{
    char d[5];
    snprintf(d, sizeof d, "%.2s%.2s", src, dst);
    for (int i = 0; i < s_npacks; i++) {
        const td_pack_t *k = &s_packs[i];
        int n = dict ? k->n_dict : k->n_nmt;
        for (int j = 0; j < n; j++)
            if (!strcmp(dict ? k->dict[j] : k->nmt[j], d)) return k;
    }
    return NULL;
}

const td_pack_t *td_pack_nmt(const char *src, const char *dst) { return find_dir(src, dst, false); }
const td_pack_t *td_pack_dict(const char *src, const char *dst) { return find_dir(src, dst, true); }

int td_local_steps(const char *src, const char *dst)
{
    if (!strcmp(src, dst)) return 0;
    if (td_pack_nmt(src, dst)) return 1;
    if (strcmp(src, "en") && strcmp(dst, "en") && td_pack_nmt(src, "en") && td_pack_nmt("en", dst)) return 2;
    return 0;
}

int td_local_langs(const char **out, int max)
{
    int n = 0;
    for (int i = 0; i < NLANGS && n < max; i++) {
        const char *c = LANGS[i].code;
        bool any = false;
        for (int j = 0; j < NLANGS && !any; j++)
            if (j != i && (td_pack_nmt(c, LANGS[j].code) || td_pack_dict(c, LANGS[j].code))) any = true;
        if (any) out[n++] = c;
    }
    return n;
}
