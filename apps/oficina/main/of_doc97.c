/*
 * P4OS - Office: Word 97-2003 (.doc, .dot), read into a read-only document.
 *
 * The FIB says where things are: the piece table (CLX) in the table
 * stream gives the text, piece by piece, 8-bit (Windows-1252) or UTF-16;
 * the bin tables of character and paragraph properties point to FKP pages
 * whose sprms give bold, italics, underline, strikethrough, colour, size,
 * super- and subscript, hidden text, alignment, table cells and rows, list
 * items and outline levels; the style sheet's built-in identifiers (sti 1-9,
 * title) make headings whatever the language of their names. Fields show
 * their result (a HYPERLINK's is a link); footnotes go at the end. Pictures
 * and drawings are not read. Word 6/95 and encrypted files are said so.
 */
#include "of_cfb.h"
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

typedef struct {
    uint32_t fc0, fc1;
    uint8_t  b, i, u, s, sup, hidden;
    uint16_t hps;
    uint32_t color;
} chp_t;

typedef struct {
    uint32_t fc0, fc1;
    uint16_t istd;
    uint8_t  jc, intable, ttp, outlvl, list, ilvl;
} pap_t;

typedef struct {
    const uint8_t *wd;
    size_t         wn;
    const uint8_t *tb;
    size_t         tn;
    chp_t         *chp;
    int            nchp;
    pap_t         *pap;
    int            npap;
    int8_t        *heading;     /* by istd */
    int            nsty;
} d97_t;

static const uint32_t ICO[17] = { 0, 0x000000, 0x0000FF, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0xFFFF00, 0xFFFFFF,
                                  0x000080, 0x008080, 0x008000, 0x800080, 0x800000, 0x808000, 0x808080, 0xC0C0C0 };

/* the length of a sprm's operand */
static int sprm_len(uint16_t sprm, const uint8_t *op, const uint8_t *end)
{
    switch (sprm >> 13) {
    case 0: case 1: return 1;
    case 2: case 4: case 5: return 2;
    case 3: return 4;
    case 7: return 3;
    default:
        if (sprm == 0xD608) return op + 2 <= end ? 2 + rd16(op) - 1 : 1;
        return op < end ? 1 + op[0] : 1;
    }
}

static void chp_sprms(chp_t *c, const uint8_t *g, int n)
{
    const uint8_t *e = g + n;
    while (g + 2 <= e) {
        uint16_t sprm = rd16(g);
        const uint8_t *op = g + 2;
        int l = sprm_len(sprm, op, e);
        if (op + l > e) break;
        uint8_t v = op[0];
        uint8_t on = v == 1 || v == 129;
        switch (sprm) {
        case 0x0835: c->b = on; break;
        case 0x0836: c->i = on; break;
        case 0x0837: case 0x2A53: c->s = on; break;
        case 0x083C: c->hidden = on; break;
        case 0x2A3E: c->u = v != 0; break;
        case 0x2A42: if (v < 17) c->color = v > 1 ? ICO[v] | 0x1000000u : 0; break;
        case 0x6870: if (!(op[3] & 0xFF)) c->color = ((uint32_t)op[0] << 16 | (uint32_t)op[1] << 8 | op[2]) | 0x1000000u; break;
        case 0x4A43: c->hps = rd16(op); break;
        case 0x2A48: c->sup = v == 1 ? 1 : v == 2 ? 2 : 0; break;
        default: break;
        }
        g = op + l;
    }
}

static void pap_sprms(pap_t *p, const uint8_t *g, int n)
{
    const uint8_t *e = g + n;
    while (g + 2 <= e) {
        uint16_t sprm = rd16(g);
        const uint8_t *op = g + 2;
        int l = sprm_len(sprm, op, e);
        if (op + l > e) break;
        switch (sprm) {
        case 0x2416: p->intable = op[0]; break;
        case 0x2417: p->ttp = op[0]; break;
        case 0x244B: if (op[0]) p->intable = 1; break;
        case 0x244C: if (op[0]) p->ttp = 1; break;
        case 0x6649: if (rd32(op)) p->intable = 1; break;
        case 0x2403: case 0x2461: p->jc = op[0]; break;
        case 0x2640: p->outlvl = (uint8_t)(op[0] < 9 ? op[0] + 1 : 0); break;
        case 0x460B: p->list = rd16(op) ? 1 : 0; break;
        case 0x260A: p->ilvl = op[0]; break;
        default: break;
        }
        g = op + l;
    }
}

/* the bin tables: every FKP page of character or paragraph properties */
static void read_fkps(d97_t *D, uint32_t fc, uint32_t lcb, bool para)
{
    if (!lcb || fc + lcb > D->tn || lcb < 12) return;
    int n = (int)((lcb - 4) / 8);
    const uint8_t *pn = D->tb + fc + 4 * (n + 1);
    for (int i = 0; i < n; i++) {
        uint32_t page = rd32(pn + i * 4) & 0x3FFFFF;
        if ((size_t)page * 512 + 512 > D->wn) continue;
        const uint8_t *pg = D->wd + (size_t)page * 512;
        int cnt = pg[511];
        if (cnt <= 0 || 4 * (cnt + 1) > 511) continue;
        for (int k = 0; k < cnt; k++) {
            uint32_t a = rd32(pg + k * 4), z = rd32(pg + (k + 1) * 4);
            if (!para) {
                int off = pg[4 * (cnt + 1) + k] * 2;
                chp_t c;
                memset(&c, 0, sizeof c);
                c.fc0 = a;
                c.fc1 = z;
                if (off && off < 511) chp_sprms(&c, pg + off + 1, pg[off] < 511 - off ? pg[off] : 0);
                chp_t *nc = of_realloc(D->chp, (size_t)(D->nchp + 1) * sizeof *nc);
                if (!nc) return;
                D->chp = nc;
                D->chp[D->nchp++] = c;
            } else {
                size_t bx = 4 * (size_t)(cnt + 1) + (size_t)k * 13;
                if (bx >= 511) break;
                int off = pg[bx] * 2;
                pap_t p;
                memset(&p, 0, sizeof p);
                p.fc0 = a;
                p.fc1 = z;
                if (off && off < 510) {
                    int cb = pg[off], len, start;
                    if (cb == 0) { len = 2 * pg[off + 1]; start = off + 2; }
                    else { len = 2 * cb - 1; start = off + 1; }
                    if (start + len <= 511 && len >= 2) {
                        p.istd = rd16(pg + start);
                        pap_sprms(&p, pg + start + 2, len - 2);
                    }
                }
                pap_t *np = of_realloc(D->pap, (size_t)(D->npap + 1) * sizeof *np);
                if (!np) return;
                D->pap = np;
                D->pap[D->npap++] = p;
            }
        }
    }
}

/* the runs are sorted by where they start: found by halves */
static const chp_t *chp_at(const d97_t *D, uint32_t fc)
{
    int lo = 0, hi = D->nchp - 1, best = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (D->chp[m].fc0 <= fc) { best = m; lo = m + 1; }
        else hi = m - 1;
    }
    return best >= 0 && fc < D->chp[best].fc1 ? &D->chp[best] : NULL;
}

static const pap_t *pap_at(const d97_t *D, uint32_t fc)
{
    int lo = 0, hi = D->npap - 1, best = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (D->pap[m].fc0 <= fc) { best = m; lo = m + 1; }
        else hi = m - 1;
    }
    return best >= 0 && fc < D->pap[best].fc1 ? &D->pap[best] : NULL;
}

static int chp_cmp(const void *a, const void *b) { uint32_t x = ((const chp_t *)a)->fc0, y = ((const chp_t *)b)->fc0; return x < y ? -1 : x > y; }
static int pap_cmp(const void *a, const void *b) { uint32_t x = ((const pap_t *)a)->fc0, y = ((const pap_t *)b)->fc0; return x < y ? -1 : x > y; }

/* headings by built-in style identifier: sti 1-9 are "heading 1-9", 62 "Title" */
static void read_styles(d97_t *D, uint32_t fc, uint32_t lcb)
{
    if (!lcb || fc + lcb > D->tn || lcb < 6) return;
    const uint8_t *s = D->tb + fc, *e = s + lcb;
    uint16_t cbstshi = rd16(s);
    if (2 + (size_t)cbstshi > lcb) return;
    int cstd = rd16(s + 2);
    uint16_t base = rd16(s + 4);
    if (cstd > 4000) return;
    D->heading = of_alloc((size_t)cstd + 1);
    if (!D->heading) return;
    D->nsty = cstd;
    const uint8_t *p = s + 2 + cbstshi;
    for (int i = 0; i < cstd && p + 2 <= e; i++) {
        uint16_t cb = rd16(p);
        if (cb && p + 2 + cb <= e && cb >= 4) {
            uint16_t sti = rd16(p + 2) & 0x0FFF;
            if (sti >= 1 && sti <= 9) D->heading[i] = (int8_t)sti;
            else if (sti == 62) D->heading[i] = 1;
            else if (sti == 0x0FFE && base && 2 + base + 2 < cb) {
                /* a style of the person's: its name may still say it */
                const uint8_t *nm = p + 2 + base;
                int cch = rd16(nm);
                char n[40];
                int k = 0;
                for (int j = 0; j < cch && k < 39 && nm + 2 + j * 2 + 1 < p + 2 + cb; j++) {
                    uint16_t ch = rd16(nm + 2 + j * 2);
                    n[k++] = ch < 128 ? (char)ch : '?';
                }
                n[k] = 0;
                (void)n;
            }
        }
        p += 2 + cb;
    }
}

typedef struct {
    of_bld_t  b;
    bool      in_table, in_row, in_cell;
    int       field;            /* depth */
    bool      show[16];         /* at that depth: showing the result */
    of_sb_t   inst;             /* the instruction being read */
    char      url[300];
    of_sb_t   run;              /* text waiting, in run_style */
    chp_t     run_chp;
    bool      run_set;
    uint16_t  max_hps;          /* the paragraph's biggest letters */
    int       chars;
} out_t;

static void flush(out_t *O)
{
    if (!O->run.n) return;
    of_rstyle_t st = { .link = -1 };
    const chp_t *c = &O->run_chp;
    if (c->b) st.b = OF_TRI_ON;
    if (c->i) st.i = OF_TRI_ON;
    if (c->u) st.u = OF_TRI_ON;
    if (c->s) st.s = OF_TRI_ON;
    st.sup = c->sup;
    st.color = c->color;
    if (c->hps && c->hps != 24 && c->hps != 22) st.half_pt = c->hps;
    st.link = O->b.st.link;
    O->b.st = st;
    of_bld_text(&O->b, O->run.s, O->run.n);
    O->run.n = 0;
}

static void put(out_t *O, uint32_t cp, const chp_t *c)
{
    chp_t z;
    memset(&z, 0, sizeof z);
    if (!c) c = &z;
    O->chars++;
    if (c->hps > O->max_hps && cp != ' ') O->max_hps = c->hps;
    if (O->run.n && (c->b != O->run_chp.b || c->i != O->run_chp.i || c->u != O->run_chp.u || c->s != O->run_chp.s ||
                     c->sup != O->run_chp.sup || c->color != O->run_chp.color || c->hps != O->run_chp.hps))
        flush(O);
    O->run_chp = *c;
    char u[4];
    of_sb_put(&O->run, u, (size_t)of_utf8_put(u, cp));
}

static void cell_state(out_t *O, bool intable)
{
    if (!intable) {
        if (O->in_table) {
            of_bld_table_end(&O->b);
            O->in_table = O->in_row = O->in_cell = false;
        }
        return;
    }
    if (!O->in_table) { of_bld_table(&O->b); O->in_table = true; O->in_row = false; }
    if (!O->in_row) { of_bld_row(&O->b); O->in_row = true; O->in_cell = false; }
    if (!O->in_cell) { of_bld_cell(&O->b, 1); O->in_cell = true; }
}

typedef struct { uint32_t cp0, cp1, fc; bool comp; } piece_t;

/* the text from cp a to cp b, through the pieces, with its properties */
static void text_range(d97_t *D, out_t *O, const piece_t *pc, int npc, uint32_t a, uint32_t b, bool notes)
{
    of_doc_t *d = O->b.d;
    bool para_open = false;
    for (int i = 0; i < npc; i++) {
        uint32_t c0 = pc[i].cp0 > a ? pc[i].cp0 : a, c1 = pc[i].cp1 < b ? pc[i].cp1 : b;
        for (uint32_t cp = c0; cp < c1; cp++) {
            uint32_t fc = pc[i].fc + (cp - pc[i].cp0) * (pc[i].comp ? 1 : 2);
            if (fc + (pc[i].comp ? 1 : 2) > D->wn) break;
            uint32_t ch = pc[i].comp ? D->wd[fc] : rd16(D->wd + fc);
            if (pc[i].comp && ch >= 0x80 && ch < 0xA0) {
                size_t m;
                uint8_t bt = (uint8_t)ch;
                char *u8 = of_latin1_to_utf8((const char *)&bt, 1, &m);
                int l;
                if (u8) { ch = of_utf8_get(u8, &l); of_free(u8); }
            }
            const chp_t *c = chp_at(D, fc);
            /* fields: the instruction hidden, the result shown */
            if (ch == 0x13) { if (O->field < 15) { O->field++; O->show[O->field] = false; O->inst.n = 0; } continue; }
            if (ch == 0x14) {
                if (O->field) {
                    O->show[O->field] = true;
                    const char *s = O->inst.s ? O->inst.s : "";
                    const char *h = strstr(s, "HYPERLINK");
                    if (h && O->field == 1) {
                        const char *q = strchr(h, '"'), *z = q ? strchr(q + 1, '"') : NULL;
                        if (q && z) {
                            snprintf(O->url, sizeof O->url, "%.*s", (int)(z - q - 1), q + 1);
                            flush(O);
                            of_bld_link(&O->b, O->url);
                        }
                    }
                }
                continue;
            }
            if (ch == 0x15) {
                if (O->field) {
                    if (O->field == 1 && O->url[0]) { flush(O); of_bld_link(&O->b, NULL); O->url[0] = 0; }
                    O->field--;
                }
                continue;
            }
            bool hidden_field = false;
            for (int k = 1; k <= O->field; k++) if (!O->show[k]) hidden_field = true;
            if (hidden_field) {
                if (ch >= 32) { char u[4]; of_sb_put(&O->inst, u, (size_t)of_utf8_put(u, ch)); }
                continue;
            }
            if (c && c->hidden) continue;
            if (ch == 0x0D || ch == 0x07) {
                /* the paragraph's end: its properties are the mark's */
                const pap_t *p = pap_at(D, fc);
                bool tbl = p && p->intable && !notes;
                if (ch == 0x07 && p && p->ttp) {
                    /* the end of a row */
                    flush(O);
                    O->in_row = O->in_cell = false;
                    O->b.cur = -1;
                    para_open = false;
                    continue;
                }
                if (!para_open) { cell_state(O, tbl); of_bld_para(&O->b); O->max_hps = 0; O->chars = 0; }
                flush(O);
                if (O->b.cur >= 0 && p) {
                    of_para_t *pp = &d->p[O->b.cur];
                    int h = p->istd < D->nsty && D->heading ? D->heading[p->istd] : 0;
                    if (!h && p->outlvl) h = p->outlvl;
                    /* no heading style: a short paragraph in big letters is one */
                    if (!h && !O->in_cell && O->chars > 0 && O->chars < 120 && O->max_hps >= 28)
                        h = O->max_hps >= 40 ? 1 : O->max_hps >= 32 ? 2 : 3;
                    pp->heading = (uint8_t)(h > 0 && h < 10 ? h : 0);
                    pp->align = (uint8_t)(p->jc == 1 ? OF_AL_CENTER : p->jc == 2 ? OF_AL_RIGHT : p->jc == 3 || p->jc == 4 ? OF_AL_JUSTIFY : OF_AL_LEFT);
                    if (p->list && !pp->heading) { pp->list = OF_LIST_BULLET; pp->ilvl = p->ilvl < 9 ? p->ilvl : 8; pp->numid = 5000; }
                }
                if (ch == 0x07 && tbl) O->in_cell = false;
                O->b.cur = -1;
                para_open = false;
                continue;
            }
            if (!para_open) {
                const pap_t *p = NULL;
                /* the table state of this paragraph: its mark's, found ahead */
                uint32_t fcn = fc;
                for (uint32_t k = cp; k < c1 && k < cp + 4000; k++) {
                    uint32_t f2 = pc[i].fc + (k - pc[i].cp0) * (pc[i].comp ? 1 : 2);
                    if (f2 + 2 > D->wn) break;
                    uint32_t c2 = pc[i].comp ? D->wd[f2] : rd16(D->wd + f2);
                    if (c2 == 0x0D || c2 == 0x07) { fcn = f2; break; }
                }
                p = pap_at(D, fcn);
                cell_state(O, p && p->intable && !notes);
                of_bld_para(&O->b);
                para_open = true;
                O->max_hps = 0;
                O->chars = 0;
            }
            switch (ch) {
            case 0x0B: flush(O); if (O->b.cur >= 0) of_para_append(d, &d->p[O->b.cur], "\n", 1, 0); O->b.space = false; break;
            case 0x0C: flush(O); of_bld_break(&O->b); para_open = false; break;
            case 0x09: put(O, ' ', c); break;
            case 0x1E: put(O, '-', c); break;
            case 0xA0: put(O, ' ', c); break;
            case 0x1F: case 0x01: case 0x08: case 0x05: case 0x02: case 0x03: case 0x04: break;
            default: if (ch >= 32) put(O, ch, c); break;
            }
        }
    }
    flush(O);
    cell_state(O, false);
}

struct of_doc_s *of_doc97_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, 64u * 1024 * 1024);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    of_cfb_t C;
    if (!of_cfb_open(&C, raw, len)) { of_free(raw); snprintf(err, errlen, "format"); return NULL; }
    size_t wn = 0, tn = 0;
    int wi = of_cfb_find(&C, "WordDocument");
    uint8_t *wd = wi >= 0 ? of_cfb_read(&C, wi, &wn) : NULL;
    if (!wd || wn < 0x1AA || rd16(wd) != 0xA5EC) {
        bool enc = of_cfb_find(&C, "EncryptedPackage") >= 0;
        of_free(wd);
        of_cfb_close(&C);
        of_free(raw);
        snprintf(err, errlen, "%s", enc ? "encrypted" : "format");
        return NULL;
    }
    uint16_t nfib = rd16(wd + 2), flags = rd16(wd + 0x0A);
    if (nfib < 0x00C1) { of_free(wd); of_cfb_close(&C); of_free(raw); snprintf(err, errlen, "old"); return NULL; }
    if (flags & 0x0100) { of_free(wd); of_cfb_close(&C); of_free(raw); snprintf(err, errlen, "encrypted"); return NULL; }
    int ti = of_cfb_find(&C, flags & 0x0200 ? "1Table" : "0Table");
    uint8_t *tb = ti >= 0 ? of_cfb_read(&C, ti, &tn) : NULL;
    if (!tb) { of_free(wd); of_cfb_close(&C); of_free(raw); snprintf(err, errlen, "format"); return NULL; }
    d97_t D = { .wd = wd, .wn = wn, .tb = tb, .tn = tn };
    uint32_t ccp_text = rd32(wd + 0x4C), ccp_ftn = rd32(wd + 0x50);
    read_styles(&D, rd32(wd + 0xA2), rd32(wd + 0xA6));
    read_fkps(&D, rd32(wd + 0xFA), rd32(wd + 0xFE), false);
    read_fkps(&D, rd32(wd + 0x102), rd32(wd + 0x106), true);
    if (D.nchp) qsort(D.chp, (size_t)D.nchp, sizeof *D.chp, chp_cmp);
    if (D.npap) qsort(D.pap, (size_t)D.npap, sizeof *D.pap, pap_cmp);
    /* the piece table */
    piece_t *pc = NULL;
    int npc = 0;
    uint32_t fclx = rd32(wd + 0x1A2), lclx = rd32(wd + 0x1A6);
    if (fclx + lclx <= tn && lclx > 5) {
        const uint8_t *p = tb + fclx, *e = p + lclx;
        while (p < e && *p == 0x01 && p + 3 <= e) p += 3 + rd16(p + 1);
        if (p + 5 <= e && *p == 0x02) {
            uint32_t lcb = rd32(p + 1);
            const uint8_t *pl = p + 5;
            if (pl + lcb <= e && lcb >= 16) {
                int n = (int)((lcb - 4) / 12);
                pc = of_alloc((size_t)(n + 1) * sizeof *pc);
                for (int i = 0; pc && i < n; i++) {
                    const uint8_t *pcd = pl + 4 * (n + 1) + 8 * i;
                    uint32_t fc = rd32(pcd + 2);
                    pc[npc].cp0 = rd32(pl + 4 * i);
                    pc[npc].cp1 = rd32(pl + 4 * (i + 1));
                    pc[npc].comp = (fc & 0x40000000u) != 0;
                    pc[npc].fc = pc[npc].comp ? (fc & ~0x40000000u) / 2 : fc;
                    npc++;
                }
            }
        }
    }
    if (!npc) {
        /* no piece table: one piece from the FIB's fcMin */
        pc = of_alloc(sizeof *pc);
        if (pc) { pc[0].cp0 = 0; pc[0].cp1 = ccp_text; pc[0].fc = rd32(wd + 0x18); pc[0].comp = false; npc = 1; }
    }
    of_doc_t *d = of_doc_new(OF_DOC_RO);
    out_t O;
    memset(&O, 0, sizeof O);
    of_bld_init(&O.b, d);
    text_range(&D, &O, pc, npc, 0, ccp_text, false);
    if (ccp_ftn > 1) {
        O.b.heading = 2;
        O.b.cur = -1;
        of_bld_para(&O.b);
        const char *h = _("Notas al pie");
        of_bld_text(&O.b, h, strlen(h));
        O.b.heading = 0;
        O.b.cur = -1;
        text_range(&D, &O, pc, npc, ccp_text, ccp_text + ccp_ftn, true);
    }
    of_bld_done(&O.b);
    snprintf(d->kind, sizeof d->kind, "DOC");
    of_sb_free(&O.run);
    of_sb_free(&O.inst);
    of_free(pc);
    of_free(D.chp);
    of_free(D.pap);
    of_free(D.heading);
    of_free(wd);
    of_free(tb);
    of_cfb_close(&C);
    of_free(raw);
    return d;
}
