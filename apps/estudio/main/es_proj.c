/*
 * Estudio - the project: its defaults, the pattern edits, and its JSON.
 *
 * The firmware lends the apps no JSON library, so there is a small one
 * here: a parser into a tree in one arena (freed in one go), and the
 * writer is fprintf. The format is ours and stays readable by hand:
 *
 *   { "estudio": 1, "name": ..., "bpm": 120, "swing": 0.5, ...,
 *     "parts":    [ { "name": "Bajo redondo", "corte": 220, ... } x3 ],
 *     "mix":      [ { "vol", "drive", "delay", "reverb", "mute" } x4 ],
 *     "drums":    [ { "src", "sample", "tune", "decay", "level", "mute" } x8 ],
 *     "fx":       { ... },
 *     "chain":    [ 0, 0, 1, ... ],
 *     "patterns": [ { "len": 16,
 *                     "steps": [ "100 0 0 0 90:50 ..." x8 ],   vel[:prob]
 *                     "notes": [ [ [step, len, note, vel, prob], ... ] x3 ] } x8 ] }
 *
 * Every number read is clamped to its range: a file edited by hand, or one
 * from a later version, cannot put the engine somewhere it does not go.
 */
#include "es.h"

#include "aos_hal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ---- defaults and the pattern edits ---- */

void es_pat_clear(es_pat_t *t)
{
    memset(t, 0, sizeof *t);
    t->len = 16;
}

void es_proj_default(es_proj_t *p)
{
    memset(p, 0, sizeof *p);
    snprintf(p->name, sizeof p->name, "%s", "Proyecto");
    p->bpm = 120.0f;
    p->swing = 0.5f;
    p->beats = 4;
    p->met_vol = 0.6f;
    static const char *const PARTS[ES_PARTS] = { "Bajo redondo", "Pluck", "Pad cálido" };
    for (int i = 0; i < ES_PARTS; i++) {
        int f = 0;
        for (int k = 0; k < es_patch_factory_n(); k++)
            if (!strcmp(es_patch_factory(k)->name, PARTS[i])) f = k;
        p->part[i] = *es_patch_factory(f);
        p->ch[i] = (es_chan_t){ 0.8f, 0.0f, 0.15f, 0.15f, 0 };
    }
    p->ch[2].rev = 0.35f;
    p->ch[3] = (es_chan_t){ 0.85f, 0.0f, 0.0f, 0.08f, 0 };
    static const uint8_t KIT[ES_DRUMS] = { DS_KICK, DS_SNARE, DS_CLAP, DS_HATC, DS_HATO, DS_TOML, DS_CYM, DS_BELL };
    static const float LV[ES_DRUMS] = { 0.95f, 0.8f, 0.75f, 0.6f, 0.55f, 0.75f, 0.5f, 0.6f };
    for (int t = 0; t < ES_DRUMS; t++) p->dt[t] = (es_dtrack_t){ KIT[t], "", 0.0f, 1.0f, LV[t], 0 };
    p->fx = (es_fx_t){ DD_8D, 0.35f, 0.5f, 0.45f, 0.55f, 0.4f, 0.5f, 0.0f, 0.2f, 0.8f };
    for (int i = 0; i < ES_PATS; i++) es_pat_clear(&p->pat[i]);
    p->arp_rate = AR_16;
    p->arp_oct = 1;
}

int es_pat_find_note(const es_pat_t *t, int part, int step, int note)
{
    for (int i = 0; i < t->nn[part]; i++)
        if (t->n[part][i].step == step && t->n[part][i].note == note) return i;
    return -1;
}

int es_pat_add_note(es_pat_t *t, int part, int step, int len, int note, int vel)
{
    int i = es_pat_find_note(t, part, step, note);
    if (i >= 0) return i;
    if (t->nn[part] >= ES_NOTES) return -1;
    i = t->nn[part];
    t->n[part][i] = (es_note_t){ (uint8_t)step, (uint8_t)(len < 1 ? 1 : len), (uint8_t)note,
                                 (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel), 100 };
    t->nn[part] = (uint8_t)(i + 1);
    return i;
}

/* the last one takes the hole: the audio thread, reading at the same time,
 * misses or repeats at most that one note for one step */
void es_pat_del_note(es_pat_t *t, int part, int idx)
{
    int n = t->nn[part];
    if (idx < 0 || idx >= n) return;
    t->n[part][idx].vel = 0;
    t->n[part][idx] = t->n[part][n - 1];
    t->nn[part] = (uint8_t)(n - 1);
}

/* a beat and a bass line, so that the first play already sounds like
 * something; B is the same with a fill */
void es_pat_demo(es_proj_t *p)
{
    for (int k = 0; k < 2; k++) {
        es_pat_t *t = &p->pat[k];
        es_pat_clear(t);
        for (int s = 0; s < 16; s += 4) t->d[0][s] = (es_dstep_t){ 118, 100 };
        t->d[1][4] = t->d[1][12] = (es_dstep_t){ 100, 100 };
        t->d[2][12] = (es_dstep_t){ 80, 100 };
        for (int s = 0; s < 16; s += 2) t->d[3][s] = (es_dstep_t){ (uint8_t)(s % 4 ? 70 : 95), 100 };
        t->d[3][15] = (es_dstep_t){ 60, 50 };
        t->d[4][14] = (es_dstep_t){ 80, 100 };
        static const uint8_t BASS[][3] = { { 0, 2, 36 }, { 3, 1, 36 }, { 6, 1, 43 }, { 8, 2, 41 },
                                           { 11, 1, 39 }, { 14, 2, 36 } };
        for (size_t i = 0; i < sizeof BASS / sizeof BASS[0]; i++)
            es_pat_add_note(t, 0, BASS[i][0], BASS[i][1], BASS[i][2], 100);
        static const uint8_t CH[][2] = { { 2, 60 }, { 2, 63 }, { 2, 67 }, { 10, 58 }, { 10, 62 }, { 10, 65 } };
        for (size_t i = 0; i < sizeof CH / sizeof CH[0]; i++)
            es_pat_add_note(t, 1, CH[i][0], 2, CH[i][1], 84);
        es_pat_add_note(t, 2, 0, 16, 48, 70);
        es_pat_add_note(t, 2, 0, 16, 55, 64);
        if (k == 1) {
            t->d[1][13] = t->d[1][14] = (es_dstep_t){ 70, 100 };
            t->d[1][15] = (es_dstep_t){ 110, 100 };
            t->d[5][10] = (es_dstep_t){ 90, 100 };
            t->d[5][11] = (es_dstep_t){ 80, 100 };
            t->d[6][0] = (es_dstep_t){ 90, 100 };
        }
    }
    p->chain_n = 4;
    p->chain[0] = 0; p->chain[1] = 0; p->chain[2] = 0; p->chain[3] = 1;
}

/* ---- a small JSON tree ---- */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

typedef struct jv {
    uint8_t     t;
    float       num;
    const char *str;            /* J_STR */
    const char *key;            /* in an object */
    struct jv  *kid, *next;
} jv_t;

typedef struct {
    const char *s, *end;
    char       *arena, *atop, *aend;
    bool        bad;
} jp_t;

static void *jp_take(jp_t *j, size_t n)
{
    n = (n + 7) & ~(size_t)7;
    if (j->atop + n > j->aend) { j->bad = true; return NULL; }
    void *p = j->atop;
    j->atop += n;
    memset(p, 0, n);
    return p;
}

static bool sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

static void jp_ws(jp_t *j)
{
    while (j->s < j->end && sp(*j->s)) j->s++;
}

static const char *jp_string(jp_t *j)
{
    if (j->s >= j->end || *j->s != '"') { j->bad = true; return NULL; }
    j->s++;
    const char *b = j->s;
    size_t n = 0;
    while (j->s < j->end && *j->s != '"') { if (*j->s == '\\') j->s++; j->s++; n++; }
    if (j->s >= j->end) { j->bad = true; return NULL; }
    char *o = jp_take(j, n * 3 + 1);
    if (!o) return NULL;
    char *w = o;
    for (const char *r = b; r < j->s; r++) {
        if (*r != '\\') { *w++ = *r; continue; }
        r++;
        switch (*r) {
        case 'n': *w++ = '\n'; break;
        case 't': *w++ = '\t'; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u': {
            unsigned cp = 0;
            for (int k = 0; k < 4 && r + 1 < j->s; k++) {
                char c = *++r;
                cp = cp * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                                          : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0);
            }
            if (cp < 0x80) *w++ = (char)cp;
            else if (cp < 0x800) { *w++ = (char)(0xC0 | (cp >> 6)); *w++ = (char)(0x80 | (cp & 63)); }
            else { *w++ = (char)(0xE0 | (cp >> 12)); *w++ = (char)(0x80 | ((cp >> 6) & 63)); *w++ = (char)(0x80 | (cp & 63)); }
            break;
        }
        default: *w++ = *r; break;
        }
    }
    *w = 0;
    j->s++;
    return o;
}

static jv_t *jp_value(jp_t *j, int depth)
{
    jp_ws(j);
    if (j->bad || j->s >= j->end || depth > 16) { j->bad = true; return NULL; }
    jv_t *v = jp_take(j, sizeof *v);
    if (!v) return NULL;
    char c = *j->s;
    if (c == '{' || c == '[') {
        bool obj = c == '{';
        v->t = obj ? J_OBJ : J_ARR;
        j->s++;
        jv_t **tail = &v->kid;
        jp_ws(j);
        if (j->s < j->end && *j->s == (obj ? '}' : ']')) { j->s++; return v; }
        for (;;) {
            const char *key = NULL;
            if (obj) {
                jp_ws(j);
                key = jp_string(j);
                jp_ws(j);
                if (j->bad || j->s >= j->end || *j->s != ':') { j->bad = true; return NULL; }
                j->s++;
            }
            jv_t *k = jp_value(j, depth + 1);
            if (!k) return NULL;
            k->key = key;
            *tail = k;
            tail = &k->next;
            jp_ws(j);
            if (j->s >= j->end) { j->bad = true; return NULL; }
            if (*j->s == ',') { j->s++; continue; }
            if (*j->s == (obj ? '}' : ']')) { j->s++; return v; }
            j->bad = true;
            return NULL;
        }
    }
    if (c == '"') { v->t = J_STR; v->str = jp_string(j); return v; }
    if (c == 't' || c == 'f' || c == 'n') {
        v->t = c == 'n' ? J_NULL : J_BOOL;
        v->num = c == 't' ? 1.0f : 0.0f;
        while (j->s < j->end && *j->s >= 'a' && *j->s <= 'z') j->s++;
        return v;
    }
    char *e;
    v->t = J_NUM;
    v->num = strtof(j->s, &e);
    if (e == j->s) { j->bad = true; return NULL; }
    j->s = e;
    return v;
}

static jv_t *jget(const jv_t *o, const char *key)
{
    if (!o || o->t != J_OBJ) return NULL;
    for (jv_t *k = o->kid; k; k = k->next) if (k->key && !strcmp(k->key, key)) return k;
    return NULL;
}

static jv_t *jat(const jv_t *a, int i)
{
    if (!a || a->t != J_ARR) return NULL;
    jv_t *k = a->kid;
    while (k && i-- > 0) k = k->next;
    return k;
}

static float jnum(const jv_t *o, const char *key, float def, float lo, float hi)
{
    jv_t *v = jget(o, key);
    if (!v || (v->t != J_NUM && v->t != J_BOOL) || v->num != v->num) return def;
    return es_clampf(v->num, lo, hi);
}

static void jstr(const jv_t *o, const char *key, char *out, size_t n)
{
    jv_t *v = jget(o, key);
    if (v && v->t == J_STR && v->str) snprintf(out, n, "%s", v->str);
}

/* the whole file into memory and parsed; *arena to free */
static jv_t *jload(const char *path, char **arena)
{
    *arena = NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 2 * 1024 * 1024) { fclose(f); return NULL; }
    size_t asz = (size_t)sz * 16 + 16384;
    char *buf = es_alloc((size_t)sz + 1 + asz);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    jp_t j = { buf, buf + got, buf + sz + 1, NULL, NULL, false };
    j.atop = (char *)(((uintptr_t)j.arena + 7) & ~(uintptr_t)7);
    j.aend = buf + sz + 1 + asz;
    jv_t *root = jp_value(&j, 0);
    if (j.bad || !root) { es_free(buf); return NULL; }
    *arena = buf;
    return root;
}

/* ---- writing ---- */

static void wstr(FILE *f, const char *s)
{
    fputc('"', f);
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { fputc('\\', f); fputc(c, f); }
        else if (c == '\n') fputs("\\n", f);
        else if (c < 0x20) fprintf(f, "\\u%04x", c);
        else fputc(c, f);
    }
    fputc('"', f);
}

static void wnum(FILE *f, float v)
{
    if (v == roundf(v) && fabsf(v) < 1e6f) fprintf(f, "%d", (int)v);
    else fprintf(f, "%.5g", (double)v);
}

static void wpatch_fields(FILE *f, const es_patch_t *p, const char *ind)
{
    fprintf(f, "%s\"name\": ", ind);
    wstr(f, p->name);
    for (int i = 0; i < P_COUNT; i++) {
        fprintf(f, ",\n%s\"%s\": ", ind, ES_PDESC[i].key);
        wnum(f, p->p[i]);
    }
}

static void rpatch_fields(const jv_t *o, es_patch_t *p)
{
    es_patch_default(p);
    jstr(o, "name", p->name, sizeof p->name);
    for (int i = 0; i < P_COUNT; i++) {
        const es_pdesc_t *d = &ES_PDESC[i];
        p->p[i] = jnum(o, d->key, d->def, d->min, d->max);
        if (d->kind == PK_INT || d->kind == PK_ENUM) p->p[i] = roundf(p->p[i]);
    }
}

/* written beside and renamed over, so a full card or a pulled card never
 * leaves half a project */
static FILE *wopen(const char *path, char *tmp, size_t n)
{
    snprintf(tmp, n, "%s.part", path);
    return fopen(tmp, "wb");
}

static bool wclose(FILE *f, const char *tmp, const char *path)
{
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (!ok) { unlink(tmp); return false; }
    unlink(path);
    if (rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

bool es_proj_save(const es_proj_t *p, const char *path)
{
    char tmp[340];
    FILE *f = wopen(path, tmp, sizeof tmp);
    if (!f) return false;
    fputs("{\n \"estudio\": 1,\n \"name\": ", f);
    wstr(f, p->name);
    fprintf(f, ",\n \"bpm\": "); wnum(f, p->bpm);
    fprintf(f, ",\n \"swing\": "); wnum(f, p->swing);
    fprintf(f, ",\n \"beats\": %d,\n \"met_on\": %d,\n \"met_vol\": ", p->beats, p->met_on);
    wnum(f, p->met_vol);
    fprintf(f, ",\n \"song_mode\": %d,\n \"arp\": { \"on\": %d, \"mode\": %d, \"rate\": %d, \"oct\": %d, \"hold\": %d }",
            p->song_mode, p->arp_on, p->arp_mode, p->arp_rate, p->arp_oct, p->arp_hold);
    fputs(",\n \"chain\": [", f);
    for (int i = 0; i < p->chain_n; i++) fprintf(f, "%s%d", i ? ", " : "", p->chain[i]);
    fputs("],\n \"parts\": [\n", f);
    for (int i = 0; i < ES_PARTS; i++) {
        fputs("  {\n", f);
        wpatch_fields(f, &p->part[i], "   ");
        fprintf(f, "\n  }%s\n", i < ES_PARTS - 1 ? "," : "");
    }
    fputs(" ],\n \"mix\": [\n", f);
    for (int i = 0; i < ES_CH; i++) {
        const es_chan_t *c = &p->ch[i];
        fputs("  { \"vol\": ", f); wnum(f, c->vol);
        fputs(", \"drive\": ", f); wnum(f, c->drive);
        fputs(", \"delay\": ", f); wnum(f, c->dly);
        fputs(", \"reverb\": ", f); wnum(f, c->rev);
        fprintf(f, ", \"mute\": %d }%s\n", c->mute, i < ES_CH - 1 ? "," : "");
    }
    fputs(" ],\n \"drums\": [\n", f);
    for (int i = 0; i < ES_DRUMS; i++) {
        const es_dtrack_t *d = &p->dt[i];
        fprintf(f, "  { \"src\": %d, \"sample\": ", d->src);
        wstr(f, d->sample);
        fputs(", \"tune\": ", f); wnum(f, d->tune);
        fputs(", \"decay\": ", f); wnum(f, d->decay);
        fputs(", \"level\": ", f); wnum(f, d->level);
        fprintf(f, ", \"mute\": %d }%s\n", d->mute, i < ES_DRUMS - 1 ? "," : "");
    }
    const es_fx_t *x = &p->fx;
    fprintf(f, " ],\n \"fx\": { \"delay_div\": %d", x->dly_div);
    fputs(", \"delay_fb\": ", f); wnum(f, x->dly_fb);
    fputs(", \"delay_tone\": ", f); wnum(f, x->dly_tone);
    fputs(", \"delay_ret\": ", f); wnum(f, x->dly_ret);
    fputs(",\n   \"rev_size\": ", f); wnum(f, x->rev_size);
    fputs(", \"rev_damp\": ", f); wnum(f, x->rev_damp);
    fputs(", \"rev_ret\": ", f); wnum(f, x->rev_ret);
    fputs(",\n   \"master_filter\": ", f); wnum(f, x->mfilt);
    fputs(", \"master_res\": ", f); wnum(f, x->mres);
    fputs(", \"master\": ", f); wnum(f, x->master);
    fputs(" },\n \"patterns\": [\n", f);
    for (int k = 0; k < ES_PATS; k++) {
        const es_pat_t *t = &p->pat[k];
        int len = t->len < 1 ? 16 : t->len;
        fprintf(f, "  { \"len\": %d,\n    \"steps\": [\n", len);
        for (int tr = 0; tr < ES_DRUMS; tr++) {
            fputs("     \"", f);
            for (int s = 0; s < len; s++) {
                es_dstep_t st = t->d[tr][s];
                if (st.vel && st.prob && st.prob < 100) fprintf(f, "%s%d:%d", s ? " " : "", st.vel, st.prob);
                else fprintf(f, "%s%d", s ? " " : "", st.vel);
            }
            fprintf(f, "\"%s\n", tr < ES_DRUMS - 1 ? "," : "");
        }
        fputs("    ],\n    \"notes\": [", f);
        for (int part = 0; part < ES_PARTS; part++) {
            fputs(part ? ", [" : "[", f);
            for (int i = 0; i < t->nn[part]; i++) {
                const es_note_t *n = &t->n[part][i];
                fprintf(f, "%s[%d,%d,%d,%d,%d]", i ? "," : "", n->step, n->len, n->note, n->vel, n->prob);
            }
            fputs("]", f);
        }
        fprintf(f, "] }%s\n", k < ES_PATS - 1 ? "," : "");
    }
    fputs(" ]\n}\n", f);
    return wclose(f, tmp, path);
}

bool es_proj_load(es_proj_t *p, const char *path, char *err, size_t errn)
{
    char *arena;
    jv_t *r = jload(path, &arena);
    if (!r || !jget(r, "estudio")) {
        if (err) snprintf(err, errn, "%s", r ? "no es un proyecto" : "no se pudo leer");
        if (arena) es_free(arena);
        return false;
    }
    es_proj_default(p);
    jstr(r, "name", p->name, sizeof p->name);
    p->bpm = jnum(r, "bpm", 120, 40, 240);
    p->swing = jnum(r, "swing", 0.5f, 0.5f, 0.75f);
    p->beats = (uint8_t)jnum(r, "beats", 4, 2, 7);
    p->met_on = (uint8_t)jnum(r, "met_on", 0, 0, 1);
    p->met_vol = jnum(r, "met_vol", 0.6f, 0, 1);
    p->song_mode = (uint8_t)jnum(r, "song_mode", 0, 0, 1);
    jv_t *a = jget(r, "arp");
    p->arp_on = (uint8_t)jnum(a, "on", 0, 0, 1);
    p->arp_mode = (uint8_t)jnum(a, "mode", 0, 0, ARP_COUNT - 1);
    p->arp_rate = (uint8_t)jnum(a, "rate", AR_16, 0, AR_COUNT - 1);
    p->arp_oct = (uint8_t)jnum(a, "oct", 1, 1, 3);
    p->arp_hold = (uint8_t)jnum(a, "hold", 0, 0, 1);
    jv_t *ch = jget(r, "chain");
    p->chain_n = 0;
    for (jv_t *k = ch && ch->t == J_ARR ? ch->kid : NULL; k && p->chain_n < ES_CHAIN; k = k->next)
        if (k->t == J_NUM) p->chain[p->chain_n++] = (uint8_t)es_clampf(k->num, 0, ES_PATS - 1);
    for (int i = 0; i < ES_PARTS; i++) {
        jv_t *o = jat(jget(r, "parts"), i);
        if (o) rpatch_fields(o, &p->part[i]);
    }
    for (int i = 0; i < ES_CH; i++) {
        jv_t *o = jat(jget(r, "mix"), i);
        if (!o) continue;
        es_chan_t *c = &p->ch[i];
        c->vol = jnum(o, "vol", c->vol, 0, 1);
        c->drive = jnum(o, "drive", c->drive, 0, 1);
        c->dly = jnum(o, "delay", c->dly, 0, 1);
        c->rev = jnum(o, "reverb", c->rev, 0, 1);
        c->mute = (uint8_t)jnum(o, "mute", 0, 0, 1);
    }
    for (int i = 0; i < ES_DRUMS; i++) {
        jv_t *o = jat(jget(r, "drums"), i);
        if (!o) continue;
        es_dtrack_t *d = &p->dt[i];
        d->src = (uint8_t)jnum(o, "src", d->src, 0, DS_COUNT);
        jstr(o, "sample", d->sample, sizeof d->sample);
        if (d->src == DS_COUNT && !d->sample[0]) d->src = DS_KICK;
        d->tune = jnum(o, "tune", 0, -24, 24);
        d->decay = jnum(o, "decay", 1, 0.1f, 2);
        d->level = jnum(o, "level", d->level, 0, 1);
        d->mute = (uint8_t)jnum(o, "mute", 0, 0, 1);
    }
    jv_t *x = jget(r, "fx");
    if (x) {
        es_fx_t *f = &p->fx;
        f->dly_div = (uint8_t)jnum(x, "delay_div", f->dly_div, 0, DD_COUNT - 1);
        f->dly_fb = jnum(x, "delay_fb", f->dly_fb, 0, 1);
        f->dly_tone = jnum(x, "delay_tone", f->dly_tone, 0, 1);
        f->dly_ret = jnum(x, "delay_ret", f->dly_ret, 0, 1);
        f->rev_size = jnum(x, "rev_size", f->rev_size, 0, 1);
        f->rev_damp = jnum(x, "rev_damp", f->rev_damp, 0, 1);
        f->rev_ret = jnum(x, "rev_ret", f->rev_ret, 0, 1);
        f->mfilt = jnum(x, "master_filter", 0, -1, 1);
        f->mres = jnum(x, "master_res", f->mres, 0, 1);
        f->master = jnum(x, "master", f->master, 0, 1);
    }
    for (int k = 0; k < ES_PATS; k++) {
        jv_t *o = jat(jget(r, "patterns"), k);
        if (!o) continue;
        es_pat_t *t = &p->pat[k];
        es_pat_clear(t);
        t->len = (uint8_t)jnum(o, "len", 16, 1, ES_STEPS);
        for (int tr = 0; tr < ES_DRUMS; tr++) {
            jv_t *s = jat(jget(o, "steps"), tr);
            if (!s || s->t != J_STR || !s->str) continue;
            const char *c = s->str;
            for (int st = 0; st < ES_STEPS && *c; st++) {
                char *e;
                long v = strtol(c, &e, 10);
                if (e == c) break;
                long pr = 100;
                if (*e == ':') { c = e + 1; pr = strtol(c, &e, 10); }
                t->d[tr][st].vel = (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
                t->d[tr][st].prob = (uint8_t)(pr < 1 ? 1 : pr > 100 ? 100 : pr);
                c = e;
                while (*c == ' ') c++;
            }
        }
        for (int part = 0; part < ES_PARTS; part++) {
            jv_t *l = jat(jget(o, "notes"), part);
            for (jv_t *n = l && l->t == J_ARR ? l->kid : NULL; n && t->nn[part] < ES_NOTES; n = n->next) {
                jv_t *a0 = jat(n, 0), *a1 = jat(n, 1), *a2 = jat(n, 2), *a3 = jat(n, 3), *a4 = jat(n, 4);
                if (!a0 || !a1 || !a2 || !a3) continue;
                es_note_t *w = &t->n[part][t->nn[part]];
                w->step = (uint8_t)es_clampf(a0->num, 0, ES_STEPS - 1);
                w->len = (uint8_t)es_clampf(a1->num, 1, ES_STEPS);
                w->note = (uint8_t)es_clampf(a2->num, 0, 127);
                w->vel = (uint8_t)es_clampf(a3->num, 1, 127);
                w->prob = (uint8_t)(a4 ? es_clampf(a4->num, 1, 100) : 100);
                t->nn[part]++;
            }
        }
    }
    es_free(arena);
    return true;
}

bool es_patch_save(const es_patch_t *p, const char *path)
{
    char tmp[340];
    FILE *f = wopen(path, tmp, sizeof tmp);
    if (!f) return false;
    fputs("{\n \"estudio_preset\": 1,\n", f);
    wpatch_fields(f, p, " ");
    fputs("\n}\n", f);
    return wclose(f, tmp, path);
}

bool es_patch_load(es_patch_t *p, const char *path)
{
    char *arena;
    jv_t *r = jload(path, &arena);
    bool ok = r && jget(r, "estudio_preset");
    if (ok) rpatch_fields(r, p);
    if (arena) es_free(arena);
    return ok;
}

/* ---- paths ---- */

const char *es_dir(void)
{
    static char d[96];
    const char *root = aos_hal_path_sd_root();
    snprintf(d, sizeof d, "%s/estudio", root ? root : "/sdcard");
    return d;
}

void es_path(char *out, size_t n, const char *sub, const char *file)
{
    if (sub && *sub) snprintf(out, n, "%s/%s/%s", es_dir(), sub, file ? file : "");
    else snprintf(out, n, "%s/%s", es_dir(), file ? file : "");
}

void es_mkdirs(void)
{
    char p[160];
    mkdir(es_dir(), 0777);
    static const char *const SUB[] = { "muestras", "presets", "export" };
    for (size_t i = 0; i < sizeof SUB / sizeof SUB[0]; i++) {
        snprintf(p, sizeof p, "%s/%s", es_dir(), SUB[i]);
        mkdir(p, 0777);
    }
}
