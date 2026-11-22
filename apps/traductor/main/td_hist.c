/*
 * P4OS - Traductor: the history and the favourites, <data>/traductor/hist.txt.
 *
 * One translation a line, the newest first, tab-separated:
 *
 *   <fav 0|1> <epoch> <src> <dst> <text> <translation>
 *
 * with \\, \t and \n escaped. The portal's page reads it, and can delete
 * lines or star them; the app reloads it when its date changes. Up to
 * TD_HIST_MAX lines; the favourites are never pushed out.
 */
#include "td.h"
#include "td_pak.h"

#include <sys/stat.h>
#include <time.h>

static td_hist_t s_h[TD_HIST_MAX];
static int s_n;
static long s_mtime = -1;

static void hist_path(char *out, size_t n)
{
    snprintf(out, n, "%.150s/hist.txt", td_data_dir());
}

static long file_mtime(void)
{
    char p[180];
    hist_path(p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long)st.st_mtime;
}

static char *dup_unescape(const char *s, size_t len)
{
    char *o = (char *)malloc(len + 1);
    if (!o) return NULL;
    size_t k = 0;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len) {
            char c = s[++i];
            o[k++] = c == 'n' ? '\n' : c == 't' ? '\t' : c;
        } else {
            o[k++] = s[i];
        }
    }
    o[k] = 0;
    return o;
}

static void put_escaped(FILE *f, const char *s)
{
    for (; *s; s++) {
        if (*s == '\\') fputs("\\\\", f);
        else if (*s == '\n') fputs("\\n", f);
        else if (*s == '\t') fputs("\\t", f);
        else if (*s != '\r') fputc(*s, f);
    }
}

static char *dupstr(const char *s)
{
    size_t l = strlen(s);
    char *o = (char *)malloc(l + 1);
    if (o) memcpy(o, s, l + 1);
    return o;
}

static void entry_free(td_hist_t *h)
{
    free(h->a);
    free(h->b);
    memset(h, 0, sizeof *h);
}

void td_hist_free(void)
{
    for (int i = 0; i < s_n; i++) entry_free(&s_h[i]);
    s_n = 0;
}

void td_hist_load(void)
{
    td_hist_free();
    char p[180];
    hist_path(p, sizeof p);
    s_mtime = file_mtime();
    FILE *f = fopen(p, "r");
    if (!f) return;
    char *line = (char *)malloc(16384);
    if (!line) {
        fclose(f);
        return;
    }
    while (s_n < TD_HIST_MAX && fgets(line, 16384, f)) {
        size_t l = strlen(line);
        while (l && (line[l - 1] == '\n' || line[l - 1] == '\r')) line[--l] = 0;
        char *fld[6];
        int nf = 0;
        char *s = line;
        fld[nf++] = s;
        for (; *s && nf < 6; s++)
            if (*s == '\t') {
                *s = 0;
                fld[nf++] = s + 1;
            }
        if (nf < 6) continue;
        td_hist_t *h = &s_h[s_n];
        memset(h, 0, sizeof *h);
        h->fav = fld[0][0] == '1';
        h->when = (uint32_t)strtol(fld[1], NULL, 10);
        td_scpy(h->src, sizeof h->src, fld[2]);
        td_scpy(h->dst, sizeof h->dst, fld[3]);
        h->a = dup_unescape(fld[4], strlen(fld[4]));
        h->b = dup_unescape(fld[5], strlen(fld[5]));
        if (!h->a || !h->b) {
            entry_free(h);
            continue;
        }
        s_n++;
    }
    free(line);
    fclose(f);
}

void td_hist_save(void)
{
    char p[180], tmp[190];
    hist_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < s_n; i++) {
        td_hist_t *h = &s_h[i];
        fprintf(f, "%d\t%lu\t%s\t%s\t", h->fav ? 1 : 0, (unsigned long)h->when, h->src, h->dst);
        put_escaped(f, h->a);
        fputc('\t', f);
        put_escaped(f, h->b);
        fputc('\n', f);
    }
    fclose(f);
    remove(p);
    rename(tmp, p);
    s_mtime = file_mtime();
}

bool td_hist_changed(void)
{
    return file_mtime() != s_mtime;
}

int td_hist_count(void) { return s_n; }

td_hist_t *td_hist_at(int i)
{
    return i >= 0 && i < s_n ? &s_h[i] : NULL;
}

void td_hist_del(int i)
{
    if (i < 0 || i >= s_n) return;
    entry_free(&s_h[i]);
    memmove(&s_h[i], &s_h[i + 1], (size_t)(s_n - i - 1) * sizeof s_h[0]);
    s_n--;
    memset(&s_h[s_n], 0, sizeof s_h[0]);
}

int td_hist_add(const char *src, const char *dst, const char *a, const char *b)
{
    bool fav = false;
    for (int i = 0; i < s_n; i++)
        if (!strcmp(s_h[i].src, src) && !strcmp(s_h[i].dst, dst) && !strcmp(s_h[i].a, a)) {
            fav = s_h[i].fav;
            td_hist_del(i);
            break;
        }
    if (s_n >= TD_HIST_MAX) {
        /* the oldest that is not a favourite goes */
        int old = -1;
        for (int i = s_n - 1; i >= 0; i--)
            if (!s_h[i].fav) {
                old = i;
                break;
            }
        if (old < 0) return -1;
        td_hist_del(old);
    }
    char *da = dupstr(a), *db = dupstr(b);
    if (!da || !db) {
        free(da);
        free(db);
        return -1;
    }
    memmove(&s_h[1], &s_h[0], (size_t)s_n * sizeof s_h[0]);
    td_hist_t *h = &s_h[0];
    memset(h, 0, sizeof *h);
    td_scpy(h->src, sizeof h->src, src);
    td_scpy(h->dst, sizeof h->dst, dst);
    h->a = da;
    h->b = db;
    h->fav = fav;
    h->when = aos_hal_time_is_valid() ? (uint32_t)time(NULL) : 0;
    s_n++;
    return 0;
}
