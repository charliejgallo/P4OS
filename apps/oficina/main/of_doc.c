/*
 * P4OS - Office: the document model (of_doc.h).
 */
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

of_doc_t *of_doc_new(int fmt)
{
    of_doc_t *d = of_alloc(sizeof *d);
    if (!d) return NULL;
    d->fmt = fmt;
    d->ps_normal = -1;
    d->def_half_pt = 22;
    of_rstyle_t plain = { .link = -1 };
    of_doc_style(d, &plain);            /* style 0: nothing set */
    return d;
}

void of_para_free_fields(of_para_t *p)
{
    of_free(p->txt);
    of_free(p->at);
    of_free(p->obj);
    of_free(p->raw_open);
    of_free(p->raw_ppr);
}

void of_doc_free(of_doc_t *d)
{
    if (!d) return;
    for (int i = 0; i < d->np; i++) of_para_free_fields(&d->p[i]);
    of_free(d->p);
    for (int i = 0; i < d->nrs; i++) of_free(d->rs[i].raw);
    of_free(d->rs);
    for (int i = 0; i < d->nobj; i++) {
        of_free(d->obj[i].raw);
        of_free(d->obj[i].text);
        of_free(d->obj[i].target);
    }
    of_free(d->obj);
    for (int i = 0; i < d->nlinks; i++) of_free(d->links[i]);
    of_free(d->links);
    for (int i = 0; i < d->nps; i++) {
        of_free(d->ps[i].id);
        of_free(d->ps[i].name);
    }
    of_free(d->ps);
    of_free(d->cells);
    of_free(d->tables);
    of_free(d->nums);
    of_zip_close(&d->zip);
    of_free(d->file);
    of_free(d->part);
    of_free(d->src);
    of_free(d->span0);
    of_free(d->span1);
    of_free(d->note);
    of_free(d);
}

of_para_t *of_doc_insert_para(of_doc_t *d, int at)
{
    if (at < 0) at = 0;
    if (at > d->np) at = d->np;
    if (d->np == d->pcap) {
        int cap = d->pcap ? d->pcap * 2 : 64;
        of_para_t *n = of_realloc(d->p, (size_t)cap * sizeof *n);
        if (!n) return NULL;
        d->p = n;
        d->pcap = cap;
    }
    memmove(&d->p[at + 1], &d->p[at], (size_t)(d->np - at) * sizeof *d->p);
    d->np++;
    of_para_t *p = &d->p[at];
    memset(p, 0, sizeof *p);
    p->orig = -1;
    p->slot = -1;
    p->numid = -1;
    p->pstyle = -1;
    p->txt = of_alloc(16);
    p->at = of_alloc(16 * sizeof(uint16_t));
    p->cap = 16;
    return p;
}

void of_doc_remove_para(of_doc_t *d, int at)
{
    if (at < 0 || at >= d->np) return;
    of_para_free_fields(&d->p[at]);
    memmove(&d->p[at], &d->p[at + 1], (size_t)(d->np - at - 1) * sizeof *d->p);
    d->np--;
}

static bool same_style(const of_rstyle_t *a, const of_rstyle_t *b)
{
    if (a->b != b->b || a->i != b->i || a->u != b->u || a->s != b->s || a->sup != b->sup ||
        a->half_pt != b->half_pt || a->color != b->color || a->hl != b->hl || a->link != b->link || a->mask != b->mask)
        return false;
    if (!a->raw || !b->raw) return a->raw == b->raw;
    return strcmp(a->raw, b->raw) == 0;
}

int of_doc_style(of_doc_t *d, const of_rstyle_t *s)
{
    for (int i = 0; i < d->nrs; i++)
        if (same_style(&d->rs[i], s)) return i;
    if (d->nrs >= 65000) return 0;
    of_rstyle_t *n = of_realloc(d->rs, (size_t)(d->nrs + 1) * sizeof *n);
    if (!n) return 0;
    d->rs = n;
    d->rs[d->nrs] = *s;
    d->rs[d->nrs].raw = s->raw ? of_strdup(s->raw) : NULL;
    return d->nrs++;
}

int of_doc_add_obj(of_doc_t *d, int kind, const char *raw, size_t n)
{
    of_obj_t *o = of_realloc(d->obj, (size_t)(d->nobj + 1) * sizeof *o);
    if (!o) return -1;
    d->obj = o;
    memset(&d->obj[d->nobj], 0, sizeof *o);
    d->obj[d->nobj].kind = (uint8_t)kind;
    d->obj[d->nobj].raw = of_strndup(raw, n);
    return d->nobj++;
}

static bool grow(of_para_t *p, int more)
{
    if (p->len + more + 1 <= p->cap) return true;
    int cap = p->cap ? p->cap : 16;
    while (cap < p->len + more + 1) cap *= 2;
    char *t = of_realloc(p->txt, (size_t)cap);
    if (!t) return false;
    p->txt = t;
    uint16_t *a = of_realloc(p->at, (size_t)cap * sizeof *a);
    if (!a) return false;
    p->at = a;
    p->cap = cap;
    return true;
}

static int objs_before(const of_para_t *p, int pos)
{
    int k = 0;
    for (int i = 0; i + 2 < p->len && i < pos; i++)
        if (!memcmp(p->txt + i, OF_OBJ_CHAR, 3)) { k++; i += 2; }
    return k;
}

void of_para_append(of_doc_t *d, of_para_t *p, const char *s, size_t n, uint16_t style)
{
    (void)d;
    if (!grow(p, (int)n)) return;
    memcpy(p->txt + p->len, s, n);
    for (size_t i = 0; i < n; i++) p->at[p->len + (int)i] = style;
    p->len += (int)n;
    p->txt[p->len] = 0;
}

void of_para_append_obj(of_doc_t *d, of_para_t *p, int obj, uint16_t style)
{
    if (obj < 0) return;
    if (p->nobj == p->objcap) {
        int cap = p->objcap ? p->objcap * 2 : 4;
        int *n = of_realloc(p->obj, (size_t)cap * sizeof *n);
        if (!n) return;
        p->obj = n;
        p->objcap = cap;
    }
    p->obj[p->nobj++] = obj;
    of_para_append(d, p, OF_OBJ_CHAR, 3, style);
}

of_obj_t *of_para_obj_at(const of_doc_t *d, const of_para_t *p, int pos)
{
    int k = objs_before(p, pos);
    if (k >= p->nobj) return NULL;
    int o = p->obj[k];
    return o >= 0 && o < d->nobj ? &d->obj[o] : NULL;
}

void of_para_insert(of_doc_t *d, int pi, int pos, const char *s, int n, uint16_t style)
{
    (void)d;
    of_para_t *p = &d->p[pi];
    if (pos < 0) pos = 0;
    if (pos > p->len) pos = p->len;
    if (!grow(p, n)) return;
    memmove(p->txt + pos + n, p->txt + pos, (size_t)(p->len - pos));
    memmove(p->at + pos + n, p->at + pos, (size_t)(p->len - pos) * sizeof *p->at);
    memcpy(p->txt + pos, s, (size_t)n);
    for (int i = 0; i < n; i++) p->at[pos + i] = style;
    p->len += n;
    p->txt[p->len] = 0;
    p->dirty = true;
}

void of_para_delete(of_doc_t *d, int pi, int pos, int n)
{
    (void)d;
    of_para_t *p = &d->p[pi];
    if (pos < 0 || n <= 0 || pos >= p->len) return;
    if (pos + n > p->len) n = p->len - pos;
    /* the objects that go */
    int k0 = objs_before(p, pos), k1 = objs_before(p, pos + n);
    if (k1 > k0) {
        memmove(&p->obj[k0], &p->obj[k1], (size_t)(p->nobj - k1) * sizeof *p->obj);
        p->nobj -= k1 - k0;
    }
    memmove(p->txt + pos, p->txt + pos + n, (size_t)(p->len - pos - n));
    memmove(p->at + pos, p->at + pos + n, (size_t)(p->len - pos - n) * sizeof *p->at);
    p->len -= n;
    p->txt[p->len] = 0;
    p->dirty = true;
}

bool of_doc_split(of_doc_t *d, int pi, int pos)
{
    if (pi < 0 || pi >= d->np || d->p[pi].locked) return false;
    if (pos < 0) pos = 0;
    if (pos > d->p[pi].len) pos = d->p[pi].len;
    of_para_t *q = of_doc_insert_para(d, pi + 1);
    if (!q) return false;
    of_para_t *p = &d->p[pi];
    /* the new paragraph is like this one: style, list, cell */
    q->heading = p->heading;
    q->align = p->align;
    q->list = p->list;
    q->ilvl = p->ilvl;
    q->numid = p->numid;
    q->pstyle = p->pstyle;
    q->pmask = p->pmask;
    q->cell = p->cell;
    q->in_list = p->in_list;
    q->slot = p->slot >= 0 ? p->slot : p->orig;
    q->raw_open = p->raw_open ? of_strdup(p->raw_open) : NULL;
    /* the section break stays with the paragraph that had it: the second
     * half, because Word keeps sectPr on the last paragraph of a section */
    q->raw_ppr = p->raw_ppr ? of_strdup(p->raw_ppr) : NULL;
    q->dirty = true;
    q->mark = pos < p->len ? p->at[pos] : (p->len ? p->at[p->len - 1] : p->mark);
    /* a heading's next paragraph is body text, as in Word */
    if (pos == p->len && p->heading) {
        q->heading = 0;
        q->pstyle = d->ps_normal;
        q->pmask |= OF_PP_STYLE;
    }
    int n = p->len - pos;
    if (n > 0) {
        of_para_append(d, q, p->txt + pos, (size_t)n, 0);
        memcpy(q->at, p->at + pos, (size_t)n * sizeof *q->at);
        int k = objs_before(p, pos);
        for (int i = k; i < p->nobj; i++) {
            if (q->nobj == q->objcap) {
                int cap = q->objcap ? q->objcap * 2 : 4;
                int *nn = of_realloc(q->obj, (size_t)cap * sizeof *nn);
                if (!nn) break;
                q->obj = nn;
                q->objcap = cap;
            }
            q->obj[q->nobj++] = p->obj[i];
        }
        p->nobj = k;
        p->len = pos;
        p->txt[pos] = 0;
    }
    if (p->sect) {
        /* docx: the sectPr belongs to the second half; the first loses it */
        q->sect = true;
        p->sect = false;
        if (p->raw_ppr) {
            char *a = strstr(p->raw_ppr, "<w:sectPr"), *z = strstr(p->raw_ppr, "</w:sectPr>");
            if (a && z) memmove(a, z + 11, strlen(z + 11) + 1);
        }
    }
    p->dirty = true;
    return true;
}

bool of_doc_can_merge(const of_doc_t *d, int pi)
{
    if (pi <= 0 || pi >= d->np) return false;
    const of_para_t *a = &d->p[pi - 1], *b = &d->p[pi];
    return a->cell == b->cell && !a->locked && !b->locked && !a->sect;
}

bool of_doc_merge(of_doc_t *d, int pi)
{
    if (!of_doc_can_merge(d, pi)) return false;
    of_para_t *a = &d->p[pi - 1], *b = &d->p[pi];
    int base = a->len;
    of_para_append(d, a, b->txt, (size_t)b->len, 0);
    memcpy(a->at + base, b->at, (size_t)b->len * sizeof *a->at);
    for (int i = 0; i < b->nobj; i++) {
        if (a->nobj == a->objcap) {
            int cap = a->objcap ? a->objcap * 2 : 4;
            int *n = of_realloc(a->obj, (size_t)cap * sizeof *n);
            if (!n) break;
            a->obj = n;
            a->objcap = cap;
        }
        a->obj[a->nobj++] = b->obj[i];
    }
    if (b->sect) {
        /* the section break moves to the paragraph that stays */
        a->sect = true;
        of_free(a->raw_ppr);
        a->raw_ppr = b->raw_ppr;
        b->raw_ppr = NULL;
    }
    a->dirty = true;
    of_doc_remove_para(d, pi);
    return true;
}

void of_para_restyle(of_doc_t *d, int pi, int pos, int n, uint8_t mask, const of_rstyle_t *val)
{
    of_para_t *p = &d->p[pi];
    if (p->locked) return;
    if (pos < 0) pos = 0;
    if (pos + n > p->len) n = p->len - pos;
    for (int i = pos; i < pos + n; i++) {
        of_rstyle_t s = d->rs[p->at[i]];
        if (mask & OF_RS_B) s.b = val->b;
        if (mask & OF_RS_I) s.i = val->i;
        if (mask & OF_RS_U) s.u = val->u;
        if (mask & OF_RS_S) s.s = val->s;
        if (mask & OF_RS_COLOR) s.color = val->color;
        if (mask & OF_RS_SIZE) s.half_pt = val->half_pt;
        if (mask & OF_RS_HL) s.hl = val->hl;
        s.mask |= mask;
        p->at[i] = (uint16_t)of_doc_style(d, &s);
    }
    if (!p->len && n <= 0) {
        of_rstyle_t s = d->rs[p->mark];
        if (mask & OF_RS_B) s.b = val->b;
        if (mask & OF_RS_I) s.i = val->i;
        if (mask & OF_RS_U) s.u = val->u;
        if (mask & OF_RS_S) s.s = val->s;
        s.mask |= mask;
        p->mark = (uint16_t)of_doc_style(d, &s);
    }
    p->dirty = true;
}

void of_doc_effective(const of_doc_t *d, const of_para_t *p, uint16_t rs, of_rstyle_t *out)
{
    of_rstyle_t e = { .link = -1 };
    e.half_pt = d->def_half_pt;
    /* the paragraph style, its parents first */
    int chain[8], n = 0;
    for (int s = p->pstyle; s >= 0 && s < d->nps && n < 8; s = d->ps[s].parent) chain[n++] = s;
    if (!n && d->ps_normal >= 0) chain[n++] = d->ps_normal;
    for (int k = n - 1; k >= 0; k--) {
        const of_pstyle_t *ps = &d->ps[chain[k]];
        if (ps->b) e.b = ps->b;
        if (ps->i) e.i = ps->i;
        if (ps->u) e.u = ps->u;
        if (ps->s) e.s = ps->s;
        if (ps->half_pt) e.half_pt = ps->half_pt;
        if (ps->color) e.color = ps->color;
    }
    const of_rstyle_t *r = rs < d->nrs ? &d->rs[rs] : &d->rs[0];
    if (r->b) e.b = r->b;
    if (r->i) e.i = r->i;
    if (r->u) e.u = r->u;
    if (r->s) e.s = r->s;
    if (r->half_pt) e.half_pt = r->half_pt;
    if (r->color) e.color = r->color;
    if (r->hl) e.hl = r->hl;
    e.sup = r->sup;
    e.link = r->link;
    if (e.link >= 0 && !r->color) e.color = 0x10563C1;     /* links in blue */
    if (e.link >= 0 && !r->u) e.u = OF_TRI_ON;
    /* headings without a size of their own: bigger, bold */
    if (p->heading && !r->half_pt) {
        bool sized = false;
        for (int k = 0; k < n; k++) if (d->ps[chain[k]].half_pt) sized = true;
        if (!sized) {
            static const uint16_t H[4] = { 0, 40, 32, 28 };
            e.half_pt = p->heading < 4 ? H[p->heading] : 24;
            if (!e.b) e.b = OF_TRI_ON;
        }
    }
    *out = e;
}

static void roman(int n, bool up, char *out, size_t len)
{
    static const char *const R[] = { "m", "cm", "d", "cd", "c", "xc", "l", "xl", "x", "ix", "v", "iv", "i" };
    static const int V[] = { 1000, 900, 500, 400, 100, 90, 50, 40, 10, 9, 5, 4, 1 };
    size_t k = 0;
    out[0] = 0;
    for (int i = 0; i < 13 && n > 0; i++)
        while (n >= V[i] && k + 3 < len) {
            for (const char *c = R[i]; *c; c++) out[k++] = up ? (char)(*c - 32) : *c;
            n -= V[i];
        }
    out[k] = 0;
}

void of_doc_number(of_doc_t *d)
{
    /* counters per list and level, restarted below a level that moved */
    int ids[32], cnt[32][9];
    int nid = 0;
    memset(cnt, 0, sizeof cnt);
    for (int i = 0; i < d->np; i++) {
        of_para_t *p = &d->p[i];
        p->label[0] = 0;
        if (p->list == OF_LIST_NONE) continue;
        int lv = p->ilvl < 9 ? p->ilvl : 8;
        int fmt = p->list == OF_LIST_BULLET ? 0 : 1;
        const char *bullet = NULL;
        for (int k = 0; k < d->nnums; k++)
            if (d->nums[k].id == p->numid) {
                fmt = d->nums[k].fmt[lv];
                if (d->nums[k].bullet[lv][0]) bullet = d->nums[k].bullet[lv];
            }
        if (fmt == 0) {
            static const char *const B[3] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };
            /* Word's bullets are often Symbol or Wingdings characters the
             * fonts do not have: the usual ones instead */
            const char *bb = bullet && (unsigned char)bullet[0] < 0x80 && bullet[0] != '\xEF' ? bullet : B[lv % 3];
            if (bullet && !strcmp(bullet, "-")) bb = "\xE2\x80\x93";
            snprintf(p->label, sizeof p->label, "%s", bb);
            continue;
        }
        int slot = -1;
        for (int k = 0; k < nid; k++) if (ids[k] == p->numid) slot = k;
        if (slot < 0) {
            /* past 32 lists, the oldest slot is reused */
            slot = nid < 32 ? nid++ : (int)((unsigned)p->numid % 32);
            ids[slot] = p->numid;
            memset(cnt[slot], 0, sizeof cnt[slot]);
        }
        cnt[slot][lv]++;
        for (int k = lv + 1; k < 9; k++) cnt[slot][k] = 0;
        int n = cnt[slot][lv];
        char num[16];
        switch (fmt) {
        case 2: snprintf(num, sizeof num, "%c", 'a' + (n - 1) % 26); break;
        case 3: snprintf(num, sizeof num, "%c", 'A' + (n - 1) % 26); break;
        case 4: roman(n, false, num, sizeof num); break;
        case 5: roman(n, true, num, sizeof num); break;
        default: snprintf(num, sizeof num, "%d", n); break;
        }
        snprintf(p->label, sizeof p->label, fmt >= 2 && fmt <= 3 ? "%s)" : "%s.", num);
    }
}

char *of_doc_plain(const of_doc_t *d)
{
    of_sb_t sb = { 0 };
    for (int i = 0; i < d->np; i++) {
        const of_para_t *p = &d->p[i];
        if (p->label[0]) { of_sb_puts(&sb, p->label); of_sb_putc(&sb, ' '); }
        for (int k = 0; k < p->len; k++) {
            if (k + 2 < p->len && !memcmp(p->txt + k, OF_OBJ_CHAR, 3)) {
                of_obj_t *o = of_para_obj_at(d, p, k);
                if (o && (o->kind == OF_OBJ_TEXT || o->kind == OF_OBJ_NOTE) && o->text) of_sb_puts(&sb, o->text);
                k += 2;
                continue;
            }
            of_sb_putc(&sb, p->txt[k]);
        }
        /* cells of a row on one line, separated by tabs */
        bool next_same_row = i + 1 < d->np && p->cell && d->p[i + 1].cell && p->cell != d->p[i + 1].cell &&
                             d->cells[p->cell - 1].table == d->cells[d->p[i + 1].cell - 1].table &&
                             d->cells[p->cell - 1].row == d->cells[d->p[i + 1].cell - 1].row;
        of_sb_putc(&sb, next_same_row ? '\t' : '\n');
    }
    return of_sb_take(&sb);
}

of_para_t *of_para_clone(const of_para_t *s)
{
    of_para_t *p = of_alloc(sizeof *p);
    if (!p) return NULL;
    *p = *s;
    p->txt = of_strndup(s->txt ? s->txt : "", (size_t)s->len);
    p->at = of_alloc((size_t)(s->len + 1) * sizeof *p->at);
    if (p->at && s->len) memcpy(p->at, s->at, (size_t)s->len * sizeof *p->at);
    p->cap = s->len + 1;
    p->obj = s->nobj ? of_alloc((size_t)s->nobj * sizeof(int)) : NULL;
    if (p->obj) memcpy(p->obj, s->obj, (size_t)s->nobj * sizeof(int));
    p->objcap = s->nobj;
    p->raw_open = s->raw_open ? of_strdup(s->raw_open) : NULL;
    p->raw_ppr = s->raw_ppr ? of_strdup(s->raw_ppr) : NULL;
    return p;
}

bool of_is_doc_ext(const char *e)
{
    return !strcmp(e, "docx") || !strcmp(e, "docm") || !strcmp(e, "dotx") || !strcmp(e, "odt") || !strcmp(e, "ott") ||
           !strcmp(e, "fodt") || !strcmp(e, "doc") || !strcmp(e, "dot");
}

of_doc_t *of_doc_load(const char *path, char *err, size_t errlen)
{
    const char *e = of_ext(path);
    of_doc_t *d = NULL;
    if (!strcmp(e, "doc") || !strcmp(e, "dot")) {
        extern of_doc_t *of_doc97_load(const char *path, char *err, size_t errlen);
        d = of_doc97_load(path, err, errlen);
        if (d) of_doc_number(d);
        return d;
    }
    if (!strcmp(e, "odt") || !strcmp(e, "ott") || !strcmp(e, "fodt")) d = of_odt_load(path, err, errlen);
    else d = of_docx_load(path, err, errlen);
    if (d) of_doc_number(d);
    return d;
}

bool of_doc_save(of_doc_t *d, int fmt, const char *path, char *err, size_t errlen)
{
    if (fmt == OF_DOC_TEXT) {
        char *t = of_doc_plain(d);
        bool ok = t && of_file_write(path, t, strlen(t));
        of_free(t);
        if (!ok) snprintf(err, errlen, "write");
        return ok;
    }
    return fmt == OF_DOC_ODT ? of_odt_save(d, path, err, errlen) : of_docx_save(d, path, err, errlen);
}

uint8_t *of_doc_obj_bytes(const of_doc_t *d, const of_obj_t *o, size_t *len, const char **ext)
{
    if (!o->target) return NULL;
    const char *e = of_ext(o->target);
    uint8_t *b = o->file ? of_file_read(o->target, len, 16u * 1024 * 1024) : of_doc_part(d, o->target, len);
    if (!b || *len < 4) { of_free(b); return NULL; }
    /* the kind from the bytes, the name may lie */
    if (!memcmp(b, "\x89PNG", 4)) e = "png";
    else if (b[0] == 0xFF && b[1] == 0xD8) e = "jpeg";
    else if (!memcmp(b, "GIF8", 4)) e = "gif";
    else if (b[0] == 'B' && b[1] == 'M') e = "bmp";
    else { of_free(b); return NULL; }
    if (ext) *ext = e;
    return b;
}

bool of_link_is_url(const char *l)
{
    return l && l[0] != '<' && l[0];
}

uint8_t *of_doc_part(const of_doc_t *d, const char *name, size_t *len)
{
    int i = of_zip_find(&d->zip, name);
    return i < 0 ? NULL : of_zip_read(&d->zip, i, len);
}
