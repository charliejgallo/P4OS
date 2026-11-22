/*
 * P4OS - Traductor: the configuration, <data>/traductor/traductor.txt.
 *
 * One text file that the app and its page in the portal both write, one
 * thing per line:
 *
 *   engine auto                     auto | local | server | online
 *   server http://host:5000         a LibreTranslate of the user's
 *   server_key <the user's key>     if that server asks for one
 *   email <an address>              MyMemory gives 50 000 characters a day
 *                                   with one, 5 000 without
 *   mymemory <base url>             to test against tools/fake_translate.py
 *   history off                     keep no history
 *   nmt int8 one-core               to measure: int8 activations, one core
 *
 * The key lives only here, on the card, where the user typed it on the
 * page: never in the code. The app looks at the file's date in its timer
 * and reloads it when the page saved. What the app alone keeps (the two
 * languages, MyMemory's count for today) goes in its preferences.
 */
#include "td.h"

#include <sys/stat.h>
#include <time.h>

#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
/* the simulator never reaches the real service: the fake one answers */
#define MM_DEFAULT "http://127.0.0.1:8790"
#else
#define MM_DEFAULT "https://api.mymemory.translated.net"
#endif

static td_cfg_t s_cfg;
static long s_mtime = -1;

static const char *const ENGINE_KEY[TD_E_COUNT] = {"auto", "local", "server", "online"};

td_cfg_t *td_cfg(void) { return &s_cfg; }

const char *td_engine_name(int e)
{
    switch (e) {
    case TD_E_AUTO: return _("Automático");
    case TD_E_LOCAL: return _("En la placa");
    case TD_E_SERVER: return _("Servidor propio");
    case TD_E_ONLINE: return _("En línea");
    }
    return "?";
}

static void cfg_path(char *out, size_t n)
{
    snprintf(out, n, "%.150s/traductor.txt", td_data_dir());
}

static long file_mtime(void)
{
    char p[180];
    cfg_path(p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long)st.st_mtime;
}

static void trim(char *s)
{
    size_t l = strlen(s);
    while (l && (s[l - 1] == '\n' || s[l - 1] == '\r' || s[l - 1] == ' ' || s[l - 1] == '\t')) s[--l] = 0;
}

void td_cfg_load(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    td_scpy(s_cfg.mm_url, sizeof s_cfg.mm_url, MM_DEFAULT);
    char p[180];
    cfg_path(p, sizeof p);
    s_mtime = file_mtime();
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[320];
    while (fgets(line, sizeof line, f)) {
        trim(line);
        char *v = strchr(line, ' ');
        if (!v || line[0] == '#') continue;
        *v++ = 0;
        while (*v == ' ') v++;
        if (!strcmp(line, "engine")) {
            for (int e = 0; e < TD_E_COUNT; e++)
                if (!strcmp(v, ENGINE_KEY[e])) s_cfg.engine = e;
        } else if (!strcmp(line, "server")) {
            td_scpy(s_cfg.server, sizeof s_cfg.server, v);
            size_t l = strlen(s_cfg.server);
            while (l && s_cfg.server[l - 1] == '/') s_cfg.server[--l] = 0;
        } else if (!strcmp(line, "server_key")) {
            td_scpy(s_cfg.server_key, sizeof s_cfg.server_key, v);
        } else if (!strcmp(line, "email")) {
            td_scpy(s_cfg.email, sizeof s_cfg.email, v);
        } else if (!strcmp(line, "mymemory") && v[0]) {
            td_scpy(s_cfg.mm_url, sizeof s_cfg.mm_url, v);
            size_t l = strlen(s_cfg.mm_url);
            while (l && s_cfg.mm_url[l - 1] == '/') s_cfg.mm_url[--l] = 0;
        } else if (!strcmp(line, "history")) {
            s_cfg.hist_off = !strcmp(v, "off");
        } else if (!strcmp(line, "nmt")) {
            /* for measuring on the board; the page does not show them */
            if (strstr(v, "int8")) s_cfg.nmt_int8 = true;
            if (strstr(v, "one-core")) s_cfg.nmt_one_core = true;
        }
    }
    fclose(f);
}

void td_cfg_save(void)
{
    char p[180], tmp[190];
    cfg_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "engine %s\n", ENGINE_KEY[s_cfg.engine]);
    if (s_cfg.server[0]) fprintf(f, "server %s\n", s_cfg.server);
    if (s_cfg.server_key[0]) fprintf(f, "server_key %s\n", s_cfg.server_key);
    if (s_cfg.email[0]) fprintf(f, "email %s\n", s_cfg.email);
    if (strcmp(s_cfg.mm_url, MM_DEFAULT)) fprintf(f, "mymemory %s\n", s_cfg.mm_url);
    if (s_cfg.hist_off) fputs("history off\n", f);
    if (s_cfg.nmt_int8 || s_cfg.nmt_one_core)
        fprintf(f, "nmt%s%s\n", s_cfg.nmt_int8 ? " int8" : "", s_cfg.nmt_one_core ? " one-core" : "");
    fclose(f);
    remove(p);
    rename(tmp, p);
    s_mtime = file_mtime();
}

bool td_cfg_changed(void)
{
    return file_mtime() != s_mtime;
}

/* ---- MyMemory's count: it says nothing until the limit, so the app counts */

static int32_t today(void)
{
    if (!aos_hal_time_is_valid()) return 0;
    return (int32_t)(time(NULL) / 86400);
}

int td_quota_used(void)
{
    int32_t d = 0, c = 0;
    aos_hal_pref_get_i32("trad_mm_day", &d);
    if (d != today()) return 0;
    aos_hal_pref_get_i32("trad_mm_chars", &c);
    return c;
}

int td_quota_limit(void)
{
    return s_cfg.email[0] ? 50000 : 5000;
}

void td_quota_add(int chars)
{
    int32_t d = today();
    int32_t c = td_quota_used() + chars;
    aos_hal_pref_set_i32("trad_mm_day", d);
    aos_hal_pref_set_i32("trad_mm_chars", c);
}
