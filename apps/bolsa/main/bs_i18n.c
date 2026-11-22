/*
 * P4OS - Bolsa: the app's own copy of its catalog.
 *
 * aos_tr() looks up the catalog of the app in FRONT (aos_i18n.h). Bolsa
 * speaks while it is hidden: a price alert is a toast over whatever app is
 * in front, and with aos_tr() it would come out in that app's words (or in
 * Spanish). So the app reads <lang>/<code>/aos.bolsa.lang itself, the same
 * way Datalog does (apps/datalog/main/dl_i18n.c, where this comes from),
 * and _() in its sources means bs_tr().
 */
#include "bs.h"

#include <sys/stat.h>

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
#include "esp_heap_caps.h"
#endif

/* PSRAM on the board (free() takes it back); the simulator has one heap */
static void *ps_alloc(size_t n)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (p) return p;
#endif
    return malloc(n);
}

static struct {
    char  code[16];
    char *blob;
    const char **es, **tr;
    int   n;
} I;

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void unescape(char *s)
{
    char *w = s;
    for (const char *r = s; *r;) {
        if (*r != '\\' || !r[1]) { *w++ = *r++; continue; }
        switch (r[1]) {
        case 'n': *w++ = '\n'; r += 2; break;
        case 't': *w++ = '\t'; r += 2; break;
        case 'r': *w++ = '\r'; r += 2; break;
        case 'x': {
            int hi = hexval(r[2]), lo = hi >= 0 ? hexval(r[3]) : -1;
            if (lo >= 0) { *w++ = (char)((hi << 4) | lo); r += 4; }
            else { *w++ = *r++; }
            break;
        }
        default: *w++ = r[1]; r += 2; break;    /* \\ \" \' */
        }
    }
    *w = 0;
}

static void drop(void)
{
    free(I.blob);
    free(I.es);
    free(I.tr);
    memset(&I, 0, sizeof I);
}

void bs_i18n_load(void)
{
    const char *code = aos_i18n_current();
    if (!code) code = "es";
    if (I.code[0] && !strcmp(code, I.code)) return;
    drop();
    bs_scpy(I.code, sizeof I.code, code);
    const char *dir = aos_hal_path_lang();
    if (!strcmp(code, "es") || !dir) return;
    char path[200];
    snprintf(path, sizeof path, "%.150s/%.15s/aos.bolsa.lang", dir, code);
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 256 * 1024) { fclose(f); return; }
    I.blob = ps_alloc((size_t)size + 1);
    if (!I.blob) { fclose(f); return; }
    size_t got = fread(I.blob, 1, (size_t)size, f);
    fclose(f);
    I.blob[got] = 0;
    int lines = 1;
    for (size_t i = 0; i < got; i++) lines += I.blob[i] == '\n';
    I.es = ps_alloc(sizeof *I.es * (size_t)lines);
    I.tr = ps_alloc(sizeof *I.tr * (size_t)lines);
    if (!I.es || !I.tr) { drop(); bs_scpy(I.code, sizeof I.code, code); return; }
    for (char *line = I.blob; line;) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[ll - 1] = 0;
        char *tab = *line != '#' ? strchr(line, '\t') : NULL;
        if (tab) {
            *tab = 0;
            unescape(line);
            unescape(tab + 1);
            if (*line && strcmp(line, tab + 1)) {
                I.es[I.n] = line;
                I.tr[I.n] = tab + 1;
                I.n++;
            }
        }
        line = nl ? nl + 1 : NULL;
    }
}

const char *bs_tr(const char *es)
{
    for (int i = 0; i < I.n; i++)
        if (!strcmp(I.es[i], es)) return I.tr[i];
    return es;
}
