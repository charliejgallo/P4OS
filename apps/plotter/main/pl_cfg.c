/*
 * P4OS - Monitor serie: profiles and alarms.
 *
 * Kept in <data>/plotter.txt, a text file the portal's page edits too (the
 * app looks at its date in the timer and reads it again when it changed):
 *
 *   profile "Multimetro"
 *   match ""
 *   comma 0
 *   field 1 "Lectura" "" scale=1 offset=0 dp=2 map="0:0,1:3,2:2,4:1"
 *   end
 *   alarm "temp" lo=- hi=30 beep=1
 *
 * Names lose their quotes when saved. The rest of the settings (channel,
 * window, the profile in use) are preferences.
 */
#include "pl.h"

#include <sys/stat.h>

pl_cfg_t PLC;
static long s_mtime;

static void path(char *out, size_t cap)
{
    const char *d = aos_hal_path_data();
    snprintf(out, cap, "%.200s/plotter.txt", d ? d : ".");
}

static long file_mtime(void)
{
    char p[220];
    path(p, sizeof p);
    struct stat st;
    if (stat(p, &st) != 0) return -1;
    return (long)st.st_mtime ^ (long)st.st_size;
}

/* one token: a run of non-spaces, with "quoted" parts kept whole */
static const char *token(const char *p, char *out, size_t cap)
{
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    bool q = false;
    while (*p && *p != '\n' && *p != '\r' && (q || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') { q = !q; p++; continue; }
        if (n + 1 < cap) out[n++] = *p;
        p++;
    }
    out[n] = 0;
    return p;
}

static float fval(const char *s)
{
    if (!s[0] || !strcmp(s, "-")) return NAN;
    return strtof(s, NULL);
}

void pl_profile_default(pl_profile_t *p, int tmpl)
{
    memset(p, 0, sizeof *p);
    p->nf = 1;
    p->f[0].field = 1;
    p->f[0].scale = 1;
    if (tmpl == 1) {
        pl_scpy(p->name, sizeof p->name, _("Multímetro"));
        pl_scpy(p->f[0].name, PL_NAME, _("Lectura"));
        p->f[0].dp_field = 2;
        pl_scpy(p->f[0].dp_map, sizeof p->f[0].dp_map, "0:0,1:3,2:2,4:1");
    } else {
        snprintf(p->name, sizeof p->name, _("Perfil %d"), PLC.np + 1);
    }
}

void pl_cfg_load(void)
{
    char p[220];
    path(p, sizeof p);
    FILE *f = fopen(p, "r");
    s_mtime = file_mtime();
    memset(&PLC, 0, sizeof PLC);
    if (!f) {
        /* the first time: an example for a meter that says where its point goes */
        pl_profile_default(&PLC.p[0], 1);
        PLC.np = 1;
        pl_cfg_save();
        return;
    }
    char line[256], tk[64];
    pl_profile_t *cur = NULL;
    while (fgets(line, sizeof line, f)) {
        const char *s = token(line, tk, sizeof tk);
        if (!tk[0] || tk[0] == '#') continue;
        if (!strcmp(tk, "profile")) {
            cur = PLC.np < PL_MAXP ? &PLC.p[PLC.np++] : NULL;
            if (!cur) continue;
            memset(cur, 0, sizeof *cur);
            token(s, cur->name, sizeof cur->name);
        } else if (!strcmp(tk, "end")) {
            cur = NULL;
        } else if (!strcmp(tk, "match") && cur) {
            token(s, cur->match, sizeof cur->match);
        } else if (!strcmp(tk, "comma") && cur) {
            token(s, tk, sizeof tk);
            cur->comma = atoi(tk) != 0;
        } else if (!strcmp(tk, "field") && cur && cur->nf < PL_MAXF) {
            pl_pfield_t *fl = &cur->f[cur->nf];
            memset(fl, 0, sizeof *fl);
            fl->scale = 1;
            s = token(s, tk, sizeof tk);
            fl->field = atoi(tk);
            s = token(s, fl->name, sizeof fl->name);
            s = token(s, fl->unit, sizeof fl->unit);
            while (*s && *s != '\n') {
                s = token(s, tk, sizeof tk);
                if (!tk[0]) break;
                char *eq = strchr(tk, '=');
                if (!eq) continue;
                *eq = 0;
                const char *v = eq + 1;
                if (!strcmp(tk, "scale")) fl->scale = strtof(v, NULL);
                else if (!strcmp(tk, "offset")) fl->offset = strtof(v, NULL);
                else if (!strcmp(tk, "dp")) fl->dp_field = atoi(v);
                else if (!strcmp(tk, "map")) pl_scpy(fl->dp_map, sizeof fl->dp_map, v);
            }
            if (fl->field < 1) fl->field = 1;
            if (fl->scale == 0) fl->scale = 1;
            cur->nf++;
        } else if (!strcmp(tk, "alarm") && PLC.na < (int)(sizeof PLC.al / sizeof PLC.al[0])) {
            pl_alarm_t *a = &PLC.al[PLC.na];
            memset(a, 0, sizeof *a);
            s = token(s, a->name, sizeof a->name);
            if (!a->name[0]) continue;
            while (*s && *s != '\n') {
                s = token(s, tk, sizeof tk);
                if (!tk[0]) break;
                char *eq = strchr(tk, '=');
                if (!eq) continue;
                *eq = 0;
                float v = fval(eq + 1);
                if (!strcmp(tk, "lo")) { a->lo_on = !PL_ISNAN(v); a->lo = a->lo_on ? v : 0; }
                else if (!strcmp(tk, "hi")) { a->hi_on = !PL_ISNAN(v); a->hi = a->hi_on ? v : 0; }
                else if (!strcmp(tk, "beep")) a->beep = atoi(eq + 1) != 0;
            }
            PLC.na++;
        }
    }
    fclose(f);
    for (int i = 0; i < PLC.np; i++) if (!PLC.p[i].name[0]) snprintf(PLC.p[i].name, sizeof PLC.p[i].name, "%d", i + 1);
}

static void clean(char *dst, size_t cap, const char *src)
{
    size_t n = 0;
    for (; *src && n + 1 < cap; src++)
        if (*src != '"' && *src != '\n' && *src != '\r') dst[n++] = *src;
    dst[n] = 0;
}

static void num(char *out, size_t cap, float v)
{
    char b[32];
    snprintf(b, sizeof b, "%.6g", (double)v);
    pl_scpy(out, cap, b);
}

void pl_cfg_save(void)
{
    char p[220], tmp[230];
    path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs("# Monitor serie: perfiles y avisos (los edita la app y la página del portal)\n", f);
    char a[64], b[64], c[64], s1[32], s2[32];
    for (int i = 0; i < PLC.np; i++) {
        const pl_profile_t *pr = &PLC.p[i];
        clean(a, sizeof a, pr->name);
        clean(b, sizeof b, pr->match);
        fprintf(f, "\nprofile \"%s\"\nmatch \"%s\"\ncomma %d\n", a, b, pr->comma ? 1 : 0);
        for (int k = 0; k < pr->nf; k++) {
            const pl_pfield_t *fl = &pr->f[k];
            clean(a, sizeof a, fl->name);
            clean(b, sizeof b, fl->unit);
            clean(c, sizeof c, fl->dp_map);
            num(s1, sizeof s1, fl->scale);
            num(s2, sizeof s2, fl->offset);
            fprintf(f, "field %d \"%s\" \"%s\" scale=%s offset=%s dp=%d map=\"%s\"\n", fl->field, a, b, s1, s2,
                    fl->dp_field, c);
        }
        fputs("end\n", f);
    }
    if (PLC.na) fputs("\n", f);
    for (int i = 0; i < PLC.na; i++) {
        clean(a, sizeof a, PLC.al[i].name);
        if (PLC.al[i].lo_on) num(s1, sizeof s1, PLC.al[i].lo); else pl_scpy(s1, sizeof s1, "-");
        if (PLC.al[i].hi_on) num(s2, sizeof s2, PLC.al[i].hi); else pl_scpy(s2, sizeof s2, "-");
        fprintf(f, "alarm \"%s\" lo=%s hi=%s beep=%d\n", a, s1, s2, PLC.al[i].beep ? 1 : 0);
    }
    fclose(f);
    remove(p);
    rename(tmp, p);
    s_mtime = file_mtime();
}

bool pl_cfg_changed(void)
{
    long m = file_mtime();
    if (m == s_mtime) return false;
    s_mtime = m;
    return true;
}

void pl_cfg_alarm_apply(pl_series_t *s)
{
    s->lo_on = s->hi_on = s->beep = false;
    for (int i = 0; i < PLC.na; i++)
        if (!strcmp(PLC.al[i].name, s->name)) {
            s->lo_on = PLC.al[i].lo_on;
            s->hi_on = PLC.al[i].hi_on;
            s->lo = PLC.al[i].lo;
            s->hi = PLC.al[i].hi;
            s->beep = PLC.al[i].beep;
            return;
        }
}

void pl_cfg_alarm_store(const pl_series_t *s)
{
    int k = -1;
    for (int i = 0; i < PLC.na; i++)
        if (!strcmp(PLC.al[i].name, s->name)) { k = i; break; }
    bool any = s->lo_on || s->hi_on;
    if (k < 0) {
        if (!any) return;
        if (PLC.na >= (int)(sizeof PLC.al / sizeof PLC.al[0])) {
            /* full: the oldest goes */
            memmove(&PLC.al[0], &PLC.al[1], sizeof PLC.al[0] * (size_t)(PLC.na - 1));
            PLC.na--;
        }
        k = PLC.na++;
    }
    if (!any) {
        memmove(&PLC.al[k], &PLC.al[k + 1], sizeof PLC.al[0] * (size_t)(PLC.na - k - 1));
        PLC.na--;
    } else {
        pl_scpy(PLC.al[k].name, PL_NAME, s->name);
        PLC.al[k].lo_on = s->lo_on;
        PLC.al[k].hi_on = s->hi_on;
        PLC.al[k].lo = s->lo;
        PLC.al[k].hi = s->hi;
        PLC.al[k].beep = s->beep;
    }
    pl_cfg_save();
}
