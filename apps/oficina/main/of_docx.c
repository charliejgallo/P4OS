/*
 * P4OS - Office: DOCX (Office Open XML documents).
 *
 * Reading: word/document.xml through its relationships, with styles.xml
 * (paragraph and character styles, what makes a heading, the default size)
 * and numbering.xml (what each list level shows). Every <w:p> becomes a
 * paragraph, the body's and the table cells' alike; its runs become text
 * with a style per byte, each style keeping its <w:rPr> as it was. A run
 * that holds anything but text, tabs and line breaks (a picture, a field
 * character, a footnote reference, a symbol, hidden text) is one object
 * with its XML; so are bookmarks, comment marks and the like. Tracked
 * changes, content controls, math and nested tables lock their paragraph.
 *
 * Saving into the same file: document.xml rewritten around the edited
 * paragraphs only (of_doc.h), every other part copied with its compressed
 * bytes. An edited paragraph keeps its <w:p> attributes (a new one drops the
 * w14 ids, which must be unique) and its <w:pPr>; its runs are written from
 * the text, each with its original <w:rPr>, or that one with only the
 * changed properties replaced, in the order the schema wants (Word refuses
 * a file with them out of order).
 *
 * A new document, or one converted from an odt, is a minimal package.
 */
#include "of_doc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DOCX_MAX (48u * 1024 * 1024)

/* -------------------------------------------------------------------------- */
/* Helpers                                                                     */
/* -------------------------------------------------------------------------- */

static bool attr(of_xml_t *X, const char *name, char *out, size_t len)
{
    return of_xml_attr(X, name, out, len);
}

static bool val_on(of_xml_t *X)
{
    char v[16];
    if (!attr(X, "w:val", v, sizeof v)) return true;
    return strcmp(v, "0") && strcmp(v, "false") && strcmp(v, "off") && strcmp(v, "none");
}

static uint32_t hex_color(const char *v)
{
    if (!v[0] || !strcmp(v, "auto")) return 0;
    return (uint32_t)strtoul(v, NULL, 16) | 0x1000000u;
}

static uint32_t highlight_rgb(const char *v)
{
    static const struct { const char *n; uint32_t c; } H[] = {
        { "yellow", 0xFFFF00 }, { "green", 0x00FF00 }, { "cyan", 0x00FFFF }, { "magenta", 0xFF00FF },
        { "blue", 0x0000FF }, { "red", 0xFF0000 }, { "darkBlue", 0x000080 }, { "darkCyan", 0x008080 },
        { "darkGreen", 0x008000 }, { "darkMagenta", 0x800080 }, { "darkRed", 0x800000 },
        { "darkYellow", 0x808000 }, { "darkGray", 0x808080 }, { "lightGray", 0xC0C0C0 }, { "black", 0x000000 },
        { "white", 0xFFFFFF } };
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++)
        if (!strcmp(H[i].n, v)) return H[i].c | 0x1000000u;
    return 0;
}

typedef struct {
    char id[40], target[220], type[120];
    bool external;
} rel_t;

static int read_rels(const of_doc_t *d, const char *name, rel_t **out)
{
    size_t len;
    char *x = (char *)of_doc_part(d, name, &len);
    *out = NULL;
    if (!x) return 0;
    rel_t *r = NULL;
    int n = 0;
    of_xml_t X;
    of_xml_init(&X, x, len);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if ((t != OX_EMPTY && t != OX_OPEN) || !of_xml_local_is(&X, "Relationship")) continue;
        rel_t *nr = of_realloc(r, (size_t)(n + 1) * sizeof *nr);
        if (!nr) break;
        r = nr;
        memset(&r[n], 0, sizeof r[n]);
        attr(&X, "Id", r[n].id, sizeof r[n].id);
        attr(&X, "Target", r[n].target, sizeof r[n].target);
        attr(&X, "Type", r[n].type, sizeof r[n].type);
        char mode[16] = "";
        attr(&X, "TargetMode", mode, sizeof mode);
        r[n].external = !strcmp(mode, "External");
        n++;
    }
    of_free(x);
    *out = r;
    return n;
}

static bool ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

/* -------------------------------------------------------------------------- */
/* Styles and numbering                                                        */
/* -------------------------------------------------------------------------- */

static int ps_find(const of_doc_t *d, const char *id)
{
    for (int i = 0; i < d->nps; i++)
        if (d->ps[i].id && !strcmp(d->ps[i].id, id)) return i;
    return -1;
}

/* the properties inside an rPr (current token OPEN), into st */
static void read_rpr_props(of_xml_t *X, of_pstyle_t *st, of_rstyle_t *rs, char *rstyle, size_t rlen, bool *vanish)
{
    int depth = 1, t;
    while (depth > 0 && (t = of_xml_next(X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN) depth++;
        else if (t == OX_CLOSE) { depth--; continue; }
        if (t != OX_EMPTY && t != OX_OPEN) continue;
        char v[64] = "";
        uint8_t tri = val_on(X) ? OF_TRI_ON : OF_TRI_OFF;
        if (of_xml_is(X, "w:b")) { if (st) st->b = tri; if (rs) rs->b = tri; }
        else if (of_xml_is(X, "w:i")) { if (st) st->i = tri; if (rs) rs->i = tri; }
        else if (of_xml_is(X, "w:strike") || of_xml_is(X, "w:dstrike")) { if (st) st->s = tri; if (rs) rs->s = tri; }
        else if (of_xml_is(X, "w:u")) { if (st) st->u = tri; if (rs) rs->u = tri; }
        else if (of_xml_is(X, "w:sz") && attr(X, "w:val", v, sizeof v)) {
            uint16_t hp = (uint16_t)atoi(v);
            if (st) st->half_pt = hp;
            if (rs) rs->half_pt = hp;
        } else if (of_xml_is(X, "w:color") && attr(X, "w:val", v, sizeof v)) {
            uint32_t c = hex_color(v);
            if (st) st->color = c;
            if (rs) rs->color = c;
        } else if (of_xml_is(X, "w:highlight") && attr(X, "w:val", v, sizeof v)) {
            if (rs) rs->hl = highlight_rgb(v);
        } else if (of_xml_is(X, "w:vertAlign") && attr(X, "w:val", v, sizeof v)) {
            if (rs) rs->sup = !strcmp(v, "superscript") ? 1 : !strcmp(v, "subscript") ? 2 : 0;
        } else if (of_xml_is(X, "w:rStyle") && rstyle) {
            attr(X, "w:val", rstyle, rlen);
        } else if (of_xml_is(X, "w:vanish") && vanish) {
            *vanish = tri == OF_TRI_ON;
        }
    }
}

static void load_styles(of_doc_t *d, const char *name)
{
    size_t len;
    char *x = (char *)of_doc_part(d, name, &len);
    if (!x) return;
    of_xml_t X;
    of_xml_init(&X, x, len);
    int t;
    char (*based)[40] = NULL;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && of_xml_is(&X, "w:rPrDefault")) {
            of_pstyle_t tmp = { 0 };
            int depth = 1, tt;
            while (depth > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_OPEN && of_xml_is(&X, "w:rPr")) { read_rpr_props(&X, &tmp, NULL, NULL, 0, NULL); continue; }
                if (tt == OX_OPEN) depth++;
                else if (tt == OX_CLOSE) depth--;
            }
            if (tmp.half_pt) d->def_half_pt = tmp.half_pt;
            continue;
        }
        if (t != OX_OPEN || !of_xml_is(&X, "w:style")) continue;
        char type[24] = "", id[64] = "", def[8] = "";
        attr(&X, "w:type", type, sizeof type);
        attr(&X, "w:styleId", id, sizeof id);
        attr(&X, "w:default", def, sizeof def);
        if (strcmp(type, "paragraph") && strcmp(type, "character")) { of_xml_skip(&X); continue; }
        of_pstyle_t st = { .parent = -1 };
        st.id = of_strdup(id);
        char bo[40] = "";
        int depth = 1, tt;
        while (depth > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
            if (tt == OX_CLOSE) { depth--; continue; }
            char v[96];
            if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(&X, "w:name") && attr(&X, "w:val", v, sizeof v)) {
                st.name = of_strdup(v);
                if (!strncasecmp(v, "heading ", 8)) st.heading = (uint8_t)atoi(v + 8);
                else if (!strcasecmp(v, "Title")) st.heading = 1;
            } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(&X, "w:basedOn")) {
                attr(&X, "w:val", bo, sizeof bo);
            } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(&X, "w:outlineLvl") && attr(&X, "w:val", v, sizeof v)) {
                int lv = atoi(v);
                if (lv >= 0 && lv < 9 && !st.heading) st.heading = (uint8_t)(lv + 1);
            } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(&X, "w:jc") && attr(&X, "w:val", v, sizeof v)) {
                st.align = !strcmp(v, "center") ? OF_AL_CENTER : (!strcmp(v, "right") || !strcmp(v, "end")) ? OF_AL_RIGHT
                         : (!strcmp(v, "both") || !strcmp(v, "distribute")) ? OF_AL_JUSTIFY : OF_AL_LEFT;
            } else if (tt == OX_OPEN && of_xml_is(&X, "w:rPr")) {
                read_rpr_props(&X, &st, NULL, NULL, 0, NULL);
                continue;
            }
            if (tt == OX_OPEN) depth++;
        }
        of_pstyle_t *n = of_realloc(d->ps, (size_t)(d->nps + 1) * sizeof *n);
        char (*nb)[40] = of_realloc(based, (size_t)(d->nps + 1) * sizeof *nb);
        if (!n || !nb) { of_free(st.id); of_free(st.name); if (n) d->ps = n; if (nb) based = nb; break; }
        d->ps = n;
        based = nb;
        snprintf(based[d->nps], 40, "%s", bo);
        if (!strcmp(type, "paragraph") && (def[0] == '1' || def[0] == 't')) d->ps_normal = d->nps;
        d->ps[d->nps++] = st;
    }
    for (int i = 0; i < d->nps; i++) {
        if (based[i][0]) d->ps[i].parent = ps_find(d, based[i]);
        if (d->ps[i].parent == i) d->ps[i].parent = -1;
    }
    /* headings get their level from the parent too ("Heading 1 Char"...) */
    of_free(based);
    of_free(x);
}

static int numfmt_code(const char *v)
{
    if (!strcmp(v, "bullet")) return 0;
    if (!strcmp(v, "lowerLetter")) return 2;
    if (!strcmp(v, "upperLetter")) return 3;
    if (!strcmp(v, "lowerRoman")) return 4;
    if (!strcmp(v, "upperRoman")) return 5;
    if (!strcmp(v, "none")) return 0;
    return 1;
}

static void load_numbering(of_doc_t *d, const char *name)
{
    size_t len;
    char *x = (char *)of_doc_part(d, name, &len);
    if (!x) return;
    typedef struct { int id; uint8_t fmt[9]; char bullet[9][8]; } abs_t;
    abs_t *abs = NULL;
    int nabs = 0;
    of_xml_t X;
    of_xml_init(&X, x, len);
    int t;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        char v[32];
        if (t == OX_OPEN && of_xml_is(&X, "w:abstractNum")) {
            abs_t a;
            memset(&a, 0, sizeof a);
            for (int k = 0; k < 9; k++) a.fmt[k] = 1;
            attr(&X, "w:abstractNumId", v, sizeof v);
            a.id = atoi(v);
            int depth = 1, tt, lvl = 0;
            while (depth > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_CLOSE) { depth--; continue; }
                if (tt == OX_OPEN) depth++;
                if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "w:lvl") && attr(&X, "w:ilvl", v, sizeof v)) lvl = atoi(v) & 7;
                else if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "w:numFmt") && attr(&X, "w:val", v, sizeof v)) a.fmt[lvl] = (uint8_t)numfmt_code(v);
                else if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "w:lvlText") && attr(&X, "w:val", v, sizeof v))
                    snprintf(a.bullet[lvl], sizeof a.bullet[lvl], "%.7s", v);
            }
            abs_t *n = of_realloc(abs, (size_t)(nabs + 1) * sizeof *n);
            if (n) { abs = n; abs[nabs++] = a; }
        } else if (t == OX_OPEN && of_xml_is(&X, "w:num")) {
            attr(&X, "w:numId", v, sizeof v);
            int id = atoi(v), aid = -1;
            int depth = 1, tt;
            while (depth > 0 && (tt = of_xml_next(&X)) != OX_END && tt != OX_ERROR) {
                if (tt == OX_CLOSE) { depth--; continue; }
                if (tt == OX_OPEN) depth++;
                if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(&X, "w:abstractNumId") && attr(&X, "w:val", v, sizeof v)) aid = atoi(v);
            }
            for (int k = 0; k < nabs; k++)
                if (abs[k].id == aid) {
                    void *n = of_realloc(d->nums, (size_t)(d->nnums + 1) * sizeof *d->nums);
                    if (!n) break;
                    d->nums = n;
                    d->nums[d->nnums].id = id;
                    memcpy(d->nums[d->nnums].fmt, abs[k].fmt, 9);
                    memcpy(d->nums[d->nnums].bullet, abs[k].bullet, sizeof abs[k].bullet);
                    d->nnums++;
                }
        }
    }
    of_free(abs);
    of_free(x);
}

/* -------------------------------------------------------------------------- */
/* The body                                                                    */
/* -------------------------------------------------------------------------- */

typedef struct {
    of_doc_t *d;
    rel_t    *rels;
    int       nrels;
    char      dir[64];          /* "word/" */
    int       footnote;         /* footnote marks seen */
} rd_t;

static const char *rel_target(rd_t *R, const char *id, bool *external)
{
    for (int i = 0; i < R->nrels; i++)
        if (!strcmp(R->rels[i].id, id)) {
            if (external) *external = R->rels[i].external;
            return R->rels[i].target;
        }
    return NULL;
}

/* a run: text with its style, or one object */
static void parse_run(rd_t *R, of_xml_t *X, of_para_t *p, int link)
{
    of_doc_t *d = R->d;
    size_t r0 = X->start;
    if (X->type == OX_EMPTY) {
        int o = of_doc_add_obj(d, OF_OBJ_HIDDEN, X->src + r0, X->end - r0);
        of_para_append_obj(d, p, o, 0);
        return;
    }
    /* first pass: what it holds */
    of_xml_t Y = *X;
    bool simple = true, has_drawing = false, has_note = false, has_br_page = false, vanish = false, has_fld = false;
    char sym[8] = "";
    int depth = 1, t;
    while (depth > 0 && (t = of_xml_next(&Y)) != OX_END && t != OX_ERROR) {
        if (t == OX_CLOSE) { depth--; continue; }
        if (t != OX_OPEN && t != OX_EMPTY) continue;
        if (depth == 1) {
            if (of_xml_is(&Y, "w:rPr") || of_xml_is(&Y, "w:t") || of_xml_is(&Y, "w:tab") || of_xml_is(&Y, "w:cr") ||
                of_xml_is(&Y, "w:noBreakHyphen") || of_xml_is(&Y, "w:softHyphen") || of_xml_is(&Y, "w:lastRenderedPageBreak")) {
            } else if (of_xml_is(&Y, "w:br")) {
                char ty[16] = "";
                attr(&Y, "w:type", ty, sizeof ty);
                if (ty[0] && strcmp(ty, "textWrapping")) { simple = false; has_br_page = true; }
            } else {
                simple = false;
                if (of_xml_is(&Y, "w:drawing") || of_xml_is(&Y, "w:pict") || of_xml_is(&Y, "w:object")) has_drawing = true;
                if (of_xml_is(&Y, "w:footnoteReference") || of_xml_is(&Y, "w:endnoteReference")) has_note = true;
                if (of_xml_is(&Y, "w:fldChar") || of_xml_is(&Y, "w:instrText")) has_fld = true;
                if (of_xml_is(&Y, "w:sym")) attr(&Y, "w:char", sym, sizeof sym);
            }
        } else if (depth == 2 && of_xml_is(&Y, "w:vanish") && val_on(&Y)) {
            vanish = true;
        }
        if (t == OX_OPEN) depth++;
    }
    size_t r1 = Y.end;
    if (!simple || vanish) {
        int kind = has_drawing ? OF_OBJ_IMAGE : has_note ? OF_OBJ_NOTE : has_br_page ? OF_OBJ_BREAK
                 : sym[0] ? OF_OBJ_TEXT : OF_OBJ_HIDDEN;
        (void)has_fld;
        int o = of_doc_add_obj(d, kind, X->src + r0, r1 - r0);
        if (o >= 0) {
            of_obj_t *ob = &d->obj[o];
            if (has_note) {
                char n[12];
                snprintf(n, sizeof n, "%d", ++R->footnote);
                ob->text = of_strdup(n);
            } else if (sym[0]) {
                ob->text = of_strdup("\xE2\x80\xA2");
            } else if (has_drawing) {
                /* the picture: its relationship and its size */
                const char *e = strstr(ob->raw, "r:embed=\"");
                if (!e) e = strstr(ob->raw, "r:id=\"");
                if (e) {
                    e = strchr(e, '"') + 1;
                    char id[40];
                    int k = 0;
                    while (*e && *e != '"' && k < 39) id[k++] = *e++;
                    id[k] = 0;
                    bool ext = false;
                    const char *tg = rel_target(R, id, &ext);
                    if (tg && !ext) {
                        char full[300];
                        if (tg[0] == '/') snprintf(full, sizeof full, "%s", tg + 1);
                        else snprintf(full, sizeof full, "%s%s", R->dir, tg);
                        ob->target = of_strdup(full);
                    }
                }
                const char *ex = strstr(ob->raw, "<wp:extent");
                if (ex) {
                    const char *cx = strstr(ex, "cx=\""), *cy = strstr(ex, "cy=\"");
                    if (cx) ob->w = (int32_t)(strtol(cx + 4, NULL, 10) / 12700);
                    if (cy) ob->h = (int32_t)(strtol(cy + 4, NULL, 10) / 12700);
                }
                if (!ob->w) { ob->w = 120; ob->h = 90; }
            }
        }
        of_para_append_obj(d, p, o, 0);
        *X = Y;
        return;
    }
    /* a text run */
    of_rstyle_t rs = { .link = (int16_t)link };
    char rstyle[64] = "";
    const char *rpr0 = NULL;
    size_t rprl = 0;
    of_sb_t text = { 0 };
    depth = 1;
    while (depth > 0 && (t = of_xml_next(X)) != OX_END && t != OX_ERROR) {
        if (t == OX_CLOSE) { depth--; continue; }
        if (t == OX_OPEN && of_xml_is(X, "w:rPr")) {
            size_t a = X->start;
            of_xml_t Z = *X;
            read_rpr_props(&Z, NULL, &rs, rstyle, sizeof rstyle, NULL);
            rpr0 = X->src + a;
            rprl = Z.end - a;
            *X = Z;
            continue;
        }
        if (t == OX_EMPTY && of_xml_is(X, "w:rPr")) { rpr0 = X->src + X->start; rprl = X->end - X->start; continue; }
        if (t == OX_OPEN && of_xml_is(X, "w:t")) { of_xml_text(X, &text); continue; }
        if (t == OX_EMPTY && of_xml_is(X, "w:t")) continue;
        if (t == OX_EMPTY && of_xml_is(X, "w:tab")) { of_sb_putc(&text, '\t'); continue; }
        if (t == OX_EMPTY && (of_xml_is(X, "w:br") || of_xml_is(X, "w:cr"))) { of_sb_putc(&text, '\n'); continue; }
        if (t == OX_EMPTY && of_xml_is(X, "w:noBreakHyphen")) { of_sb_putc(&text, '-'); continue; }
        if (t == OX_OPEN) depth++;
    }
    /* the character style's looks, for showing */
    if (rstyle[0]) {
        int ci = ps_find(d, rstyle);
        for (int guard = 0; ci >= 0 && guard < 8; guard++, ci = d->ps[ci].parent) {
            const of_pstyle_t *cs = &d->ps[ci];
            if (!rs.b && cs->b) rs.b = cs->b;
            if (!rs.i && cs->i) rs.i = cs->i;
            if (!rs.u && cs->u) rs.u = cs->u;
            if (!rs.color && cs->color) rs.color = cs->color;
            if (!rs.half_pt && cs->half_pt) rs.half_pt = cs->half_pt;
        }
    }
    if (rpr0) rs.raw = of_strndup(rpr0, rprl);
    int si = of_doc_style(d, &rs);
    of_free(rs.raw);
    if (text.n) of_para_append(d, p, text.s, text.n, (uint16_t)si);
    else if (!p->len) p->mark = (uint16_t)si;
    of_sb_free(&text);
}

/* a whole <w:p> */
static void parse_p(rd_t *R, of_xml_t *X, int cell, bool locked)
{
    of_doc_t *d = R->d;
    of_para_t *p = of_doc_insert_para(d, d->np);
    if (!p) { if (X->type == OX_OPEN) of_xml_skip(X); return; }
    int pi = d->np - 1;
    p->cell = cell;
    p->locked = locked;
    p->orig = p->slot = d->norig;
    p->raw_open = of_strndup(X->src + X->start, X->end - X->start);
    if (X->type == OX_EMPTY) {
        /* <w:p/>: an empty paragraph, written as <w:p></w:p> when edited */
        p->raw_open[strlen(p->raw_open) - 2] = '>';
        p->raw_open[strlen(p->raw_open) - 1] = 0;
    }
    uint32_t s0 = (uint32_t)X->start;
    if (X->type == OX_OPEN) {
        int linkstack[8], nlink = 0;
        int depth = 1, t;
        while (depth > 0 && (t = of_xml_next(X)) != OX_END && t != OX_ERROR) {
            p = &d->p[pi];
            if (t == OX_CLOSE) {
                if (of_xml_is(X, "w:hyperlink") && nlink) nlink--;
                depth--;
                continue;
            }
            if (t != OX_OPEN && t != OX_EMPTY) continue;
            if (of_xml_is(X, "w:pPr") && t == OX_OPEN) {
                size_t a = X->start;
                int dd = 1, tt;
                while (dd > 0 && (tt = of_xml_next(X)) != OX_END && tt != OX_ERROR) {
                    if (tt == OX_CLOSE) { dd--; continue; }
                    char v[64];
                    if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(X, "w:pStyle") && attr(X, "w:val", v, sizeof v)) {
                        p->pstyle = ps_find(d, v);
                    } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(X, "w:ilvl") && attr(X, "w:val", v, sizeof v)) {
                        p->ilvl = (uint8_t)atoi(v);
                    } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(X, "w:numId") && attr(X, "w:val", v, sizeof v)) {
                        p->numid = atoi(v);
                    } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(X, "w:jc") && attr(X, "w:val", v, sizeof v) && dd == 1) {
                        p->align = !strcmp(v, "center") ? OF_AL_CENTER : (!strcmp(v, "right") || !strcmp(v, "end")) ? OF_AL_RIGHT
                                 : (!strcmp(v, "both") || !strcmp(v, "distribute")) ? OF_AL_JUSTIFY : OF_AL_LEFT;
                    } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(X, "w:outlineLvl") && attr(X, "w:val", v, sizeof v)) {
                        int lv = atoi(v);
                        if (lv >= 0 && lv < 9) p->heading = (uint8_t)(lv + 1);
                    } else if ((tt == OX_EMPTY || tt == OX_OPEN) && of_xml_is(X, "w:sectPr")) {
                        p->sect = true;
                    } else if (tt == OX_OPEN && of_xml_is(X, "w:rPr") && dd == 1) {
                        of_rstyle_t rs = { .link = -1 };
                        size_t ra = X->start;
                        read_rpr_props(X, NULL, &rs, NULL, 0, NULL);
                        rs.raw = NULL;
                        (void)ra;
                        p->mark = (uint16_t)of_doc_style(d, &rs);
                        continue;
                    } else if ((tt == OX_OPEN || tt == OX_EMPTY) && of_xml_is(X, "w:pPrChange")) {
                        p->locked = true;
                        d->has_track = true;
                    }
                    if (tt == OX_OPEN) dd++;
                }
                p->raw_ppr = of_strndup(X->src + a, X->end - a);
                continue;
            }
            if (of_xml_is(X, "w:r")) {
                parse_run(R, X, p, nlink ? linkstack[nlink - 1] : -1);
                continue;
            }
            if (of_xml_is(X, "w:hyperlink")) {
                if (t == OX_EMPTY) {
                    int o = of_doc_add_obj(d, OF_OBJ_HIDDEN, X->src + X->start, X->end - X->start);
                    of_para_append_obj(d, p, o, 0);
                    continue;
                }
                char **nl = of_realloc(d->links, (size_t)(d->nlinks + 1) * sizeof *nl);
                if (nl) {
                    d->links = nl;
                    d->links[d->nlinks] = of_strndup(X->src + X->start, X->end - X->start);
                    if (nlink < 8) linkstack[nlink++] = d->nlinks;
                    d->nlinks++;
                }
                depth++;
                continue;
            }
            if (of_xml_is(X, "w:del") || of_xml_is(X, "w:moveFrom")) {
                p->locked = true;
                d->has_track = true;
                if (t == OX_OPEN) of_xml_skip(X);
                continue;
            }
            if (of_xml_is(X, "w:fldSimple") && t == OX_OPEN) {
                size_t a = X->start;
                of_sb_t tx = { 0 };
                of_xml_t Z = *X;
                of_xml_text(&Z, &tx);
                int o = of_doc_add_obj(d, OF_OBJ_TEXT, X->src + a, Z.end - a);
                if (o >= 0) d->obj[o].text = of_sb_take(&tx);
                of_sb_free(&tx);
                *X = Z;
                of_para_append_obj(d, p, o, 0);
                continue;
            }
            if (t == OX_EMPTY) {
                /* bookmarks, comment ranges, proofing marks: kept, invisible */
                int o = of_doc_add_obj(d, OF_OBJ_HIDDEN, X->src + X->start, X->end - X->start);
                of_para_append_obj(d, p, o, 0);
                continue;
            }
            /* an inline container the app does not model: shown, locked */
            if (of_xml_is(X, "w:ins") || of_xml_is(X, "w:moveTo")) d->has_track = true;
            p->locked = true;
            depth++;
        }
    }
    p = &d->p[pi];
    if (p->locked) d->any_locked = true;
    /* heading from the style */
    if (!p->heading && p->pstyle >= 0) {
        for (int s = p->pstyle, g = 0; s >= 0 && g < 8; s = d->ps[s].parent, g++)
            if (d->ps[s].heading) { p->heading = d->ps[s].heading; break; }
    }
    if (!p->align && p->pstyle >= 0) p->align = d->ps[p->pstyle].align;
    if (p->numid > 0) {
        p->list = OF_LIST_NUMBER;
        for (int k = 0; k < d->nnums; k++)
            if (d->nums[k].id == p->numid && d->nums[k].fmt[p->ilvl & 7] == 0) p->list = OF_LIST_BULLET;
    }
    /* where it was */
    uint32_t *n0 = of_realloc(d->span0, (size_t)(d->norig + 1) * sizeof *n0);
    uint32_t *n1 = of_realloc(d->span1, (size_t)(d->norig + 1) * sizeof *n1);
    if (n0) d->span0 = n0;
    if (n1) d->span1 = n1;
    if (n0 && n1) {
        d->span0[d->norig] = s0;
        d->span1[d->norig] = (uint32_t)X->end;
        d->norig++;
    }
}

of_doc_t *of_docx_load(const char *path, char *err, size_t errlen)
{
    size_t len;
    uint8_t *raw = of_file_read(path, &len, DOCX_MAX);
    if (!raw) { snprintf(err, errlen, "read"); return NULL; }
    if (of_is_encrypted_ooxml(raw, len)) { of_free(raw); snprintf(err, errlen, "encrypted"); return NULL; }
    of_doc_t *d = of_doc_new(OF_DOC_DOCX);
    if (!d) { of_free(raw); return NULL; }
    d->file = raw;
    d->file_len = len;
    if (!of_zip_open(&d->zip, raw, len)) { snprintf(err, errlen, "zip"); of_doc_free(d); return NULL; }
    rd_t R = { .d = d };
    rel_t *top;
    int ntop = read_rels(d, "_rels/.rels", &top);
    char main_part[200] = "word/document.xml";
    for (int i = 0; i < ntop; i++)
        if (ends_with(top[i].type, "/officeDocument"))
            snprintf(main_part, sizeof main_part, "%.199s", top[i].target[0] == '/' ? top[i].target + 1 : top[i].target);
    of_free(top);
    d->part = of_strdup(main_part);
    snprintf(R.dir, sizeof R.dir, "%.63s", main_part);
    char *sl = strrchr(R.dir, '/');
    if (sl) sl[1] = 0;
    else R.dir[0] = 0;
    char rn[260];
    snprintf(rn, sizeof rn, "%s_rels/%s.rels", R.dir, of_basename(main_part));
    R.nrels = read_rels(d, rn, &R.rels);
    for (int i = 0; i < R.nrels; i++) {
        char p[300];
        snprintf(p, sizeof p, "%s%s", R.dir, R.rels[i].target);
        if (ends_with(R.rels[i].type, "/styles")) load_styles(d, p);
        else if (ends_with(R.rels[i].type, "/numbering")) load_numbering(d, p);
    }
    d->src = (char *)of_doc_part(d, main_part, &d->src_len);
    if (!d->src) { of_free(R.rels); snprintf(err, errlen, "document"); of_doc_free(d); return NULL; }
    of_xml_t X;
    of_xml_init(&X, d->src, d->src_len);
    int t, tbl = 0, sdt = 0, cur_cell = 0, cur_table = -1, row = -1, col = 0;
    bool in_body = false;
    while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
        if (t == OX_OPEN && of_xml_is(&X, "w:body")) { in_body = true; continue; }
        if (!in_body) continue;
        if ((t == OX_OPEN || t == OX_EMPTY) && of_xml_is(&X, "w:p")) {
            parse_p(&R, &X, cur_cell, tbl > 1 || sdt > 0);
            continue;
        }
        if (t == OX_OPEN && of_xml_is(&X, "w:tbl")) {
            tbl++;
            if (tbl == 1) {
                of_table_t *n = of_realloc(d->tables, (size_t)(d->ntables + 1) * sizeof *n);
                if (n) {
                    d->tables = n;
                    memset(&d->tables[d->ntables], 0, sizeof *n);
                    cur_table = d->ntables++;
                }
                row = -1;
            } else {
                d->any_locked = true;
            }
            continue;
        }
        if (t == OX_CLOSE && of_xml_is(&X, "w:tbl")) { tbl--; if (!tbl) cur_cell = 0; continue; }
        if (tbl == 1 && cur_table >= 0) {
            of_table_t *T = &d->tables[cur_table];
            if (t == OX_EMPTY && of_xml_is(&X, "w:gridCol")) {
                char v[16] = "0";
                attr(&X, "w:w", v, sizeof v);
                if (T->ncols < 64) T->colw[T->ncols++] = atoi(v) / 20;
            } else if (t == OX_OPEN && of_xml_is(&X, "w:tr")) {
                row++;
                col = 0;
                T->nrows = row + 1;
            } else if (t == OX_OPEN && of_xml_is(&X, "w:tc")) {
                of_cell_t2 *n = of_realloc(d->cells, (size_t)(d->ncells + 1) * sizeof *n);
                if (n) {
                    d->cells = n;
                    d->cells[d->ncells] = (of_cell_t2){ cur_table, row, col, 1, false };
                    cur_cell = ++d->ncells;
                }
            } else if (t == OX_EMPTY && of_xml_is(&X, "w:gridSpan") && cur_cell) {
                char v[16] = "1";
                attr(&X, "w:val", v, sizeof v);
                d->cells[cur_cell - 1].span = atoi(v) > 0 ? atoi(v) : 1;
            } else if (t == OX_EMPTY && of_xml_is(&X, "w:vMerge") && cur_cell) {
                char v[16] = "";
                attr(&X, "w:val", v, sizeof v);
                d->cells[cur_cell - 1].vmerged = strcmp(v, "restart") != 0;
            } else if (t == OX_CLOSE && of_xml_is(&X, "w:tc") && cur_cell) {
                col += d->cells[cur_cell - 1].span;
            }
        }
        if (t == OX_OPEN && (of_xml_is(&X, "w:sdt") || of_xml_is(&X, "w:customXml"))) { sdt++; d->any_locked = true; continue; }
        if (t == OX_CLOSE && (of_xml_is(&X, "w:sdt") || of_xml_is(&X, "w:customXml"))) { sdt--; continue; }
        if (t == OX_OPEN && of_xml_is(&X, "w:sectPr") && !tbl) { of_xml_skip(&X); continue; }
    }
    of_free(R.rels);
    /* a table needs its column count even without a grid */
    for (int i = 0; i < d->ncells; i++) {
        of_table_t *T = &d->tables[d->cells[i].table];
        int need = d->cells[i].col + d->cells[i].span;
        while (T->ncols < need && T->ncols < 64) T->colw[T->ncols++] = 0;
    }
    if (!d->np) of_doc_insert_para(d, 0);
    for (int i = 0; i < d->np; i++) d->p[i].dirty = false;
    return d;
}

/* -------------------------------------------------------------------------- */
/* Writing: properties                                                         */
/* -------------------------------------------------------------------------- */

static const char *const RPR_ORDER[] = {
    "w:rStyle", "w:rFonts", "w:b", "w:bCs", "w:i", "w:iCs", "w:caps", "w:smallCaps", "w:strike", "w:dstrike",
    "w:outline", "w:shadow", "w:emboss", "w:imprint", "w:noProof", "w:snapToGrid", "w:vanish", "w:webHidden",
    "w:color", "w:spacing", "w:w", "w:kern", "w:position", "w:sz", "w:szCs", "w:highlight", "w:u", "w:effect",
    "w:bdr", "w:shd", "w:fitText", "w:vertAlign", "w:rtl", "w:cs", "w:em", "w:lang", "w:eastAsianLayout",
    "w:specVanish", "w:oMath", NULL };
static const char *const PPR_ORDER[] = {
    "w:pStyle", "w:keepNext", "w:keepLines", "w:pageBreakBefore", "w:framePr", "w:widowControl", "w:numPr",
    "w:suppressLineNumbers", "w:pBdr", "w:shd", "w:tabs", "w:suppressAutoHyphens", "w:kinsoku", "w:wordWrap",
    "w:overflowPunct", "w:topLinePunct", "w:autoSpaceDE", "w:autoSpaceDN", "w:bidi", "w:adjustRightInd",
    "w:snapToGrid", "w:spacing", "w:ind", "w:contextualSpacing", "w:mirrorIndents", "w:suppressOverlap", "w:jc",
    "w:textDirection", "w:textAlignment", "w:textboxTightWrap", "w:outlineLvl", "w:divId", "w:cnfStyle", "w:rPr",
    "w:sectPr", "w:pPrChange", NULL };

typedef struct {
    char  *xml;
    int    order;
} child_t;

static int order_of(const char *const *tab, const char *name, int nl)
{
    for (int i = 0; tab[i]; i++)
        if ((int)strlen(tab[i]) == nl && !strncmp(tab[i], name, (size_t)nl)) return i;
    return -1;
}

/* An element's children, some dropped and some added, back in schema order.
 * 'drop' names (space separated, with spaces around) go; 'add' is XML. */
static void patch_element(of_sb_t *o, const char *raw, const char *tag, const char *const *order, const char *drop,
                          const char *add)
{
    child_t kids[64];
    int nk = 0;
    char open_tag[256];
    snprintf(open_tag, sizeof open_tag, "<%s>", tag);
    const char *open = open_tag;
    size_t open_len = strlen(open_tag);
    if (raw) {
        of_xml_t X;
        of_xml_init(&X, raw, strlen(raw));
        int t = of_xml_next(&X);
        open = raw;
        open_len = X.end;
        if (t == OX_EMPTY) {
            /* <w:rPr/>: its attributes, opened */
            snprintf(open_tag, sizeof open_tag, "%.*s>", (int)(X.end - 2 > 250 ? 250 : X.end - 2), raw);
            open = open_tag;
            open_len = strlen(open_tag);
        } else {
            int depth = 0, last = -1;
            while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR) {
                if (t == OX_CLOSE) { if (depth == 0) break; depth--; continue; }
                if (t == OX_TEXT || t == OX_OTHER) continue;
                if (depth == 0 && nk < 64) {
                    size_t a = X.start;
                    char nm[48];
                    snprintf(nm, sizeof nm, " %.*s ", X.name_len > 44 ? 44 : X.name_len, X.name);
                    int ord = order_of(order, X.name, X.name_len);
                    if (t == OX_OPEN) of_xml_skip(&X);
                    if (drop && strstr(drop, nm)) continue;
                    kids[nk].xml = of_strndup(raw + a, X.end - a);
                    kids[nk].order = ord >= 0 ? ord : last;
                    if (ord >= 0) last = ord;
                    nk++;
                    continue;
                }
                if (t == OX_OPEN) depth++;
            }
        }
    }
    /* the added ones */
    if (add && *add) {
        of_xml_t X;
        of_xml_init(&X, add, strlen(add));
        int t;
        while ((t = of_xml_next(&X)) != OX_END && t != OX_ERROR && nk < 64) {
            if (t != OX_EMPTY && t != OX_OPEN) continue;
            size_t a = X.start;
            int ord = order_of(order, X.name, X.name_len);
            if (t == OX_OPEN) of_xml_skip(&X);
            kids[nk].xml = of_strndup(add + a, X.end - a);
            kids[nk].order = ord;
            nk++;
        }
    }
    /* stable insertion sort by order */
    for (int i = 1; i < nk; i++) {
        child_t k = kids[i];
        int j = i - 1;
        while (j >= 0 && kids[j].order > k.order) { kids[j + 1] = kids[j]; j--; }
        kids[j + 1] = k;
    }
    if (!nk) {
        for (int i = 0; i < nk; i++) of_free(kids[i].xml);
        return;
    }
    of_sb_put(o, open, open_len);
    for (int i = 0; i < nk; i++) {
        of_sb_puts(o, kids[i].xml);
        of_free(kids[i].xml);
    }
    of_sb_printf(o, "</%s>", tag);
}

static void w_rpr(of_sb_t *o, const of_doc_t *d, const of_rstyle_t *s)
{
    (void)d;
    if (!s->mask) {
        if (s->raw) of_sb_puts(o, s->raw);
        return;
    }
    of_sb_t drop = { 0 }, add = { 0 };
    of_sb_puts(&drop, " ");
    if (s->mask & OF_RS_B) {
        of_sb_puts(&drop, " w:b ");
        if (s->b) of_sb_puts(&add, s->b == OF_TRI_ON ? "<w:b/>" : "<w:b w:val=\"0\"/>");
    }
    if (s->mask & OF_RS_I) {
        of_sb_puts(&drop, " w:i ");
        if (s->i) of_sb_puts(&add, s->i == OF_TRI_ON ? "<w:i/>" : "<w:i w:val=\"0\"/>");
    }
    if (s->mask & OF_RS_S) {
        of_sb_puts(&drop, " w:strike ");
        if (s->s) of_sb_puts(&add, s->s == OF_TRI_ON ? "<w:strike/>" : "<w:strike w:val=\"0\"/>");
    }
    if (s->mask & OF_RS_U) {
        of_sb_puts(&drop, " w:u ");
        if (s->u) of_sb_puts(&add, s->u == OF_TRI_ON ? "<w:u w:val=\"single\"/>" : "<w:u w:val=\"none\"/>");
    }
    if (s->mask & OF_RS_COLOR) {
        of_sb_puts(&drop, " w:color ");
        if (s->color) of_sb_printf(&add, "<w:color w:val=\"%06lX\"/>", (unsigned long)(s->color & 0xFFFFFF));
    }
    if (s->mask & OF_RS_SIZE) {
        of_sb_puts(&drop, " w:sz w:szCs ");
        if (s->half_pt) of_sb_printf(&add, "<w:sz w:val=\"%u\"/><w:szCs w:val=\"%u\"/>", s->half_pt, s->half_pt);
    }
    if (s->mask & OF_RS_HL) {
        of_sb_puts(&drop, " w:highlight w:shd ");
        if (s->hl) of_sb_printf(&add, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06lX\"/>", (unsigned long)(s->hl & 0xFFFFFF));
    }
    patch_element(o, s->raw, "w:rPr", RPR_ORDER, drop.s, add.s);
    of_sb_free(&drop);
    of_sb_free(&add);
}

static void w_text(of_sb_t *o, const char *s, int n)
{
    int from = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && s[i] != '\t' && s[i] != '\n') continue;
        if (i > from) {
            of_sb_puts(o, "<w:t xml:space=\"preserve\">");
            of_sb_xml(o, s + from, (size_t)(i - from));
            of_sb_puts(o, "</w:t>");
        }
        if (i < n) of_sb_puts(o, s[i] == '\t' ? "<w:tab/>" : "<w:br/>");
        from = i + 1;
    }
}

static const char *pstyle_for_heading(const of_doc_t *d, int level)
{
    for (int i = 0; i < d->nps; i++)
        if (d->ps[i].heading == level && d->ps[i].id && d->ps[i].name && !strncasecmp(d->ps[i].name, "heading", 7))
            return d->ps[i].id;
    for (int i = 0; i < d->nps; i++)
        if (d->ps[i].heading == level && d->ps[i].id) return d->ps[i].id;
    return NULL;
}

static void w_para(of_sb_t *o, const of_doc_t *d, const of_para_t *p)
{
    /* <w:p ...>: a new paragraph drops the ids that must be unique */
    const char *open = p->raw_open ? p->raw_open : "<w:p>";
    if (p->orig < 0 && p->raw_open) {
        of_xml_t X;
        of_xml_init(&X, open, strlen(open));
        of_xml_next(&X);
        of_sb_puts(o, "<w:p");
        const char *a = X.attrs;
        int al = X.attrs_len, i = 0;
        while (i < al) {
            while (i < al && (a[i] == ' ' || a[i] == '\n' || a[i] == '\t' || a[i] == '\r')) i++;
            int k0 = i;
            while (i < al && a[i] != '=') i++;
            int k1 = i;
            i++;
            while (i < al && a[i] != '"' && a[i] != '\'') i++;
            if (i >= al) break;
            char q = a[i++];
            while (i < al && a[i] != q) i++;
            i++;
            if ((k1 - k0 == 11 && !strncmp(a + k0, "w14:paraId", 10)) || !strncmp(a + k0, "w14:paraId", 10) ||
                !strncmp(a + k0, "w14:textId", 10) || !strncmp(a + k0, "w:rsid", 6))
                continue;
            of_sb_putc(o, ' ');
            of_sb_put(o, a + k0, (size_t)(i - k0));
        }
        of_sb_putc(o, '>');
    } else {
        of_sb_puts(o, open);
    }
    /* <w:pPr> */
    if (p->pmask) {
        of_sb_t drop = { 0 }, add = { 0 };
        of_sb_puts(&drop, " ");
        if (p->pmask & OF_PP_STYLE) {
            of_sb_puts(&drop, " w:pStyle w:outlineLvl ");
            const char *id = p->heading ? pstyle_for_heading(d, p->heading)
                           : (p->pstyle >= 0 && p->pstyle < d->nps && !d->ps[p->pstyle].heading ? d->ps[p->pstyle].id : NULL);
            if (id) {
                of_sb_puts(&add, "<w:pStyle w:val=\"");
                of_sb_xml(&add, id, strlen(id));
                of_sb_puts(&add, "\"/>");
            } else if (p->heading) {
                of_sb_printf(&add, "<w:outlineLvl w:val=\"%d\"/>", p->heading - 1);
            }
        }
        if (p->pmask & OF_PP_ALIGN) {
            static const char *const JC[4] = { "left", "center", "right", "both" };
            of_sb_puts(&drop, " w:jc ");
            of_sb_printf(&add, "<w:jc w:val=\"%s\"/>", JC[p->align & 3]);
        }
        patch_element(o, p->raw_ppr, "w:pPr", PPR_ORDER, drop.s, add.s);
        of_sb_free(&drop);
        of_sb_free(&add);
    } else if (p->raw_ppr) {
        of_sb_puts(o, p->raw_ppr);
    }
    /* the runs */
    int link = -1;
    int i = 0, k = 0;
    while (i < p->len) {
        if (i + 2 < p->len && !memcmp(p->txt + i, OF_OBJ_CHAR, 3)) {
            int lk = d->rs[p->at[i]].link;
            if (lk != link) {
                if (link >= 0) of_sb_puts(o, "</w:hyperlink>");
                link = lk;
                if (link >= 0 && link < d->nlinks) of_sb_puts(o, d->links[link]);
                else link = -1;
            }
            int ob = k < p->nobj ? p->obj[k] : -1;
            k++;
            if (ob >= 0 && ob < d->nobj && d->obj[ob].raw) of_sb_puts(o, d->obj[ob].raw);
            i += 3;
            continue;
        }
        uint16_t st = p->at[i];
        int j = i;
        while (j < p->len && p->at[j] == st && !(j + 2 < p->len && !memcmp(p->txt + j, OF_OBJ_CHAR, 3))) j++;
        const of_rstyle_t *rs = &d->rs[st];
        if (rs->link != link) {
            if (link >= 0) of_sb_puts(o, "</w:hyperlink>");
            link = rs->link;
            if (link >= 0 && link < d->nlinks) of_sb_puts(o, d->links[link]);
            else link = -1;
        }
        of_sb_puts(o, "<w:r>");
        w_rpr(o, d, rs);
        w_text(o, p->txt + i, j - i);
        of_sb_puts(o, "</w:r>");
        i = j;
    }
    if (link >= 0) of_sb_puts(o, "</w:hyperlink>");
    of_sb_puts(o, "</w:p>");
}

static char *document_xml(const of_doc_t *d)
{
    of_sb_t o = { 0 };
    size_t pos = 0;
    /* model paragraphs grouped by slot, in model order */
    for (int i = 0; i < d->norig; i++) {
        of_sb_put(&o, d->src + pos, d->span0[i] - pos);
        for (int k = 0; k < d->np; k++) {
            const of_para_t *p = &d->p[k];
            if (p->slot != i) continue;
            if (p->orig == i && (!p->dirty || p->locked)) of_sb_put(&o, d->src + d->span0[i], d->span1[i] - d->span0[i]);
            else w_para(&o, d, p);
        }
        pos = d->span1[i];
    }
    of_sb_put(&o, d->src + pos, d->src_len - pos);
    if (o.oom) { of_sb_free(&o); return NULL; }
    return of_sb_take(&o);
}

/* -------------------------------------------------------------------------- */
/* Saving                                                                      */
/* -------------------------------------------------------------------------- */

static bool save_keeping(of_doc_t *d, const char *path, char *err, size_t errlen)
{
    char *doc = document_xml(d);
    if (!doc) { snprintf(err, errlen, "memory"); return false; }
    of_zipw_t w;
    of_zipw_init(&w);
    bool ok = true;
    for (int i = 0; i < d->zip.n && ok; i++) {
        if (!strcmp(d->zip.e[i].name, d->part)) ok = of_zipw_add(&w, d->part, doc, strlen(doc), true);
        else ok = of_zipw_copy(&w, &d->zip, i);
    }
    ok = ok && of_zipw_finish(&w, path);
    of_zipw_free(&w);
    of_free(doc);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

static const char HEAD[] = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";

/* A new package from the model alone (a new document, or from an odt). */
static bool save_new(of_doc_t *d, const char *path, char *err, size_t errlen)
{
    of_zipw_t w;
    of_zipw_init(&w);
    of_sb_t o = { 0 };
    of_sb_puts(&o, HEAD);
    of_sb_puts(&o, "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
                   "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
                   "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
                   "<Default Extension=\"png\" ContentType=\"image/png\"/><Default Extension=\"jpeg\" ContentType=\"image/jpeg\"/>"
                   "<Default Extension=\"gif\" ContentType=\"image/gif\"/><Default Extension=\"bmp\" ContentType=\"image/bmp\"/>"
                   "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>"
                   "<Override PartName=\"/word/styles.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml\"/>"
                   "<Override PartName=\"/word/numbering.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.numbering+xml\"/>"
                   "</Types>");
    of_zipw_add(&w, "[Content_Types].xml", o.s, o.n, true);
    o.n = 0;
    of_sb_puts(&o, HEAD);
    of_sb_puts(&o, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                   "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>"
                   "</Relationships>");
    of_zipw_add(&w, "_rels/.rels", o.s, o.n, true);
    o.n = 0;
    /* the document's relationships: styles, numbering, then pictures and links as the body needs them */
    of_sb_t rels = { 0 };
    of_sb_puts(&rels, HEAD);
    of_sb_puts(&rels, "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                      "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
                      "<Relationship Id=\"rId2\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/numbering\" Target=\"numbering.xml\"/>");
    int npics = 0;
    of_sb_puts(&o, HEAD);
    of_sb_puts(&o, "<w:styles xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                   "<w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii=\"Calibri\" w:hAnsi=\"Calibri\"/><w:sz w:val=\"22\"/>"
                   "</w:rPr></w:rPrDefault><w:pPrDefault><w:pPr><w:spacing w:after=\"160\" w:line=\"259\" w:lineRule=\"auto\"/></w:pPr>"
                   "</w:pPrDefault></w:docDefaults>"
                   "<w:style w:type=\"paragraph\" w:default=\"1\" w:styleId=\"Normal\"><w:name w:val=\"Normal\"/><w:qFormat/></w:style>");
    static const int HSZ[4] = { 0, 40, 32, 28 };
    for (int h = 1; h <= 3; h++)
        of_sb_printf(&o, "<w:style w:type=\"paragraph\" w:styleId=\"Heading%d\"><w:name w:val=\"heading %d\"/>"
                         "<w:basedOn w:val=\"Normal\"/><w:next w:val=\"Normal\"/><w:qFormat/><w:pPr><w:keepNext/>"
                         "<w:spacing w:before=\"240\" w:after=\"80\"/><w:outlineLvl w:val=\"%d\"/></w:pPr>"
                         "<w:rPr><w:b/><w:sz w:val=\"%d\"/></w:rPr></w:style>", h, h, h - 1, HSZ[h]);
    of_sb_puts(&o, "</w:styles>");
    of_zipw_add(&w, "word/styles.xml", o.s, o.n, true);
    o.n = 0;
    of_sb_puts(&o, HEAD);
    of_sb_puts(&o, "<w:numbering xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
                   "");
    /* bullets and numbers, nine levels each */
    for (int a = 0; a < 2; a++) {
        of_sb_printf(&o, "<w:abstractNum w:abstractNumId=\"%d\">", a);
        static const char *const BUL[3] = { "\xE2\x80\xA2", "\xE2\x97\xA6", "\xE2\x96\xAA" };
        for (int l = 0; l < 9; l++)
            of_sb_printf(&o, "<w:lvl w:ilvl=\"%d\"><w:start w:val=\"1\"/><w:numFmt w:val=\"%s\"/><w:lvlText w:val=\"%s%s\"/>"
                             "<w:lvlJc w:val=\"left\"/><w:pPr><w:ind w:left=\"%d\" w:hanging=\"360\"/></w:pPr></w:lvl>",
                         l, a ? "decimal" : "bullet", a ? "%" : BUL[l % 3], a ? (const char *[]){ "1.", "2.", "3.", "4.", "5.", "6.", "7.", "8.", "9." }[l] : "",
                         720 * (l + 1));
        of_sb_puts(&o, "</w:abstractNum>");
    }
    of_sb_puts(&o, "<w:num w:numId=\"1\"><w:abstractNumId w:val=\"0\"/></w:num><w:num w:numId=\"2\"><w:abstractNumId w:val=\"1\"/></w:num>"
                   "</w:numbering>");
    of_zipw_add(&w, "word/numbering.xml", o.s, o.n, true);
    o.n = 0;

    /* the body */
    of_sb_puts(&o, HEAD);
    of_sb_puts(&o, "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
                   "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
                   "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
                   "xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\" "
                   "xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"><w:body>");
    /* links that are URLs: one relationship each, rIdL<n> */
    for (int l = 0; l < d->nlinks; l++) {
        if (!of_link_is_url(d->links[l])) continue;
        of_sb_printf(&rels, "<Relationship Id=\"rIdL%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/hyperlink\" Target=\"", l);
        of_sb_xml(&rels, d->links[l], strlen(d->links[l]));
        of_sb_puts(&rels, "\" TargetMode=\"External\"/>");
    }
    int cur_table = -1, cur_row = -1, cur_cell = 0;
    for (int i = 0; i < d->np; i++) {
        const of_para_t *p = &d->p[i];
        const of_cell_t2 *c = p->cell ? &d->cells[p->cell - 1] : NULL;
        /* table structure, opened and closed around the cells' paragraphs */
        if (cur_cell && (!c || p->cell != cur_cell)) { of_sb_puts(&o, "</w:tc>"); cur_cell = 0; }
        if (cur_row >= 0 && (!c || c->table != cur_table || c->row != cur_row)) { of_sb_puts(&o, "</w:tr>"); cur_row = -1; }
        if (cur_table >= 0 && (!c || c->table != cur_table)) { of_sb_puts(&o, "</w:tbl>"); cur_table = -1; }
        if (c && cur_table != c->table) {
            cur_table = c->table;
            const of_table_t *T = &d->tables[c->table];
            of_sb_puts(&o, "<w:tbl><w:tblPr><w:tblStyle w:val=\"TableGrid\"/><w:tblW w:w=\"0\" w:type=\"auto\"/>"
                           "<w:tblBorders><w:top w:val=\"single\" w:sz=\"4\"/><w:left w:val=\"single\" w:sz=\"4\"/>"
                           "<w:bottom w:val=\"single\" w:sz=\"4\"/><w:right w:val=\"single\" w:sz=\"4\"/>"
                           "<w:insideH w:val=\"single\" w:sz=\"4\"/><w:insideV w:val=\"single\" w:sz=\"4\"/></w:tblBorders></w:tblPr><w:tblGrid>");
            for (int k = 0; k < T->ncols; k++) of_sb_printf(&o, "<w:gridCol w:w=\"%ld\"/>", (long)(T->colw[k] ? T->colw[k] * 20 : 9000 / T->ncols));
            of_sb_puts(&o, "</w:tblGrid>");
        }
        if (c && cur_row != c->row) { cur_row = c->row; of_sb_puts(&o, "<w:tr>"); }
        if (c && cur_cell != p->cell) {
            cur_cell = p->cell;
            of_sb_puts(&o, "<w:tc><w:tcPr>");
            if (c->span > 1) of_sb_printf(&o, "<w:gridSpan w:val=\"%d\"/>", c->span);
            if (c->vmerged) of_sb_puts(&o, "<w:vMerge/>");
            of_sb_puts(&o, "</w:tcPr>");
        }
        of_sb_puts(&o, "<w:p>");
        if (p->heading || p->align || p->list || p->indent) {
            of_sb_puts(&o, "<w:pPr>");
            if (p->heading) of_sb_printf(&o, "<w:pStyle w:val=\"Heading%d\"/>", p->heading > 3 ? 3 : p->heading);
            if (p->list) of_sb_printf(&o, "<w:numPr><w:ilvl w:val=\"%d\"/><w:numId w:val=\"%d\"/></w:numPr>", p->ilvl > 8 ? 8 : p->ilvl, p->list == OF_LIST_BULLET ? 1 : 2);
            else if (p->indent) of_sb_printf(&o, "<w:ind w:left=\"%d\"/>", 567 * p->indent);
            if (p->align) {
                static const char *const JC[4] = { "left", "center", "right", "both" };
                of_sb_printf(&o, "<w:jc w:val=\"%s\"/>", JC[p->align & 3]);
            }
            of_sb_puts(&o, "</w:pPr>");
        }
        int k = 0, link = -1;
        for (int a = 0; a < p->len;) {
            /* links open and close around the runs */
            int lk = d->rs[p->at[a]].link;
            if (lk >= 0 && (lk >= d->nlinks || !of_link_is_url(d->links[lk]))) lk = -1;
            if (lk != link) {
                if (link >= 0) of_sb_puts(&o, "</w:hyperlink>");
                link = lk;
                if (link >= 0) of_sb_printf(&o, "<w:hyperlink r:id=\"rIdL%d\">", link);
            }
            if (a + 2 < p->len && !memcmp(p->txt + a, OF_OBJ_CHAR, 3)) {
                int ob = k < p->nobj ? p->obj[k] : -1;
                k++;
                of_obj_t *obj = ob >= 0 && ob < d->nobj ? &d->obj[ob] : NULL;
                if (obj && (obj->kind == OF_OBJ_TEXT || obj->kind == OF_OBJ_NOTE) && obj->text) {
                    of_sb_puts(&o, "<w:r>");
                    w_text(&o, obj->text, (int)strlen(obj->text));
                    of_sb_puts(&o, "</w:r>");
                } else if (obj && obj->kind == OF_OBJ_BREAK) {
                    of_sb_puts(&o, "<w:r><w:br w:type=\"page\"/></w:r>");
                } else if (obj && obj->kind == OF_OBJ_IMAGE) {
                    size_t n;
                    const char *ext = "png";
                    uint8_t *bin = of_doc_obj_bytes(d, obj, &n, &ext);
                    if (bin) {
                        npics++;
                        char name[48];
                        snprintf(name, sizeof name, "word/media/image%d.%s", npics, ext);
                        of_zipw_add(&w, name, bin, n, false);
                        of_free(bin);
                        of_sb_printf(&rels, "<Relationship Id=\"rIdP%d\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" "
                                            "Target=\"media/image%d.%s\"/>", npics, npics, ext);
                        long cx = (long)(obj->w > 0 ? obj->w : 200) * 12700, cy = (long)(obj->h > 0 ? obj->h : 150) * 12700;
                        of_sb_printf(&o, "<w:r><w:drawing><wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\"><wp:extent cx=\"%ld\" cy=\"%ld\"/>"
                                         "<wp:docPr id=\"%d\" name=\"Picture %d\"/><a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">"
                                         "<pic:pic><pic:nvPicPr><pic:cNvPr id=\"%d\" name=\"image%d.%s\"/><pic:cNvPicPr/></pic:nvPicPr>"
                                         "<pic:blipFill><a:blip r:embed=\"rIdP%d\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>"
                                         "<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"%ld\" cy=\"%ld\"/></a:xfrm><a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>"
                                         "</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r>",
                                     cx, cy, npics, npics, npics, npics, ext, npics, cx, cy);
                    }
                }
                a += 3;
                continue;
            }
            int b = a;
            while (b < p->len && p->at[b] == p->at[a] && !(b + 2 < p->len && !memcmp(p->txt + b, OF_OBJ_CHAR, 3))) b++;
            of_rstyle_t e;
            of_doc_effective(d, p, p->at[a], &e);
            const of_rstyle_t *r = &d->rs[p->at[a]];
            of_sb_puts(&o, "<w:r>");
            if (r->b || r->i || r->u || r->s || r->color || r->half_pt || r->sup || r->hl || link >= 0) {
                of_sb_puts(&o, "<w:rPr>");
                if (r->b) of_sb_puts(&o, r->b == OF_TRI_ON ? "<w:b/>" : "<w:b w:val=\"0\"/>");
                if (r->i) of_sb_puts(&o, r->i == OF_TRI_ON ? "<w:i/>" : "<w:i w:val=\"0\"/>");
                if (r->s == OF_TRI_ON) of_sb_puts(&o, "<w:strike/>");
                if (r->color) of_sb_printf(&o, "<w:color w:val=\"%06lX\"/>", (unsigned long)(r->color & 0xFFFFFF));
                else if (link >= 0) of_sb_puts(&o, "<w:color w:val=\"0563C1\"/>");
                if (r->half_pt) of_sb_printf(&o, "<w:sz w:val=\"%u\"/>", r->half_pt);
                if (r->hl) of_sb_printf(&o, "<w:shd w:val=\"clear\" w:color=\"auto\" w:fill=\"%06lX\"/>", (unsigned long)(r->hl & 0xFFFFFF));
                if (r->u == OF_TRI_ON || (link >= 0 && !r->u)) of_sb_puts(&o, "<w:u w:val=\"single\"/>");
                if (r->sup) of_sb_printf(&o, "<w:vertAlign w:val=\"%s\"/>", r->sup == 1 ? "superscript" : "subscript");
                of_sb_puts(&o, "</w:rPr>");
            }
            (void)e;
            w_text(&o, p->txt + a, b - a);
            of_sb_puts(&o, "</w:r>");
            a = b;
        }
        if (link >= 0) of_sb_puts(&o, "</w:hyperlink>");
        of_sb_puts(&o, "</w:p>");
        /* a cell's last paragraph, and the table's end */
    }
    if (cur_cell) of_sb_puts(&o, "</w:tc>");
    if (cur_row >= 0) of_sb_puts(&o, "</w:tr>");
    if (cur_table >= 0) of_sb_puts(&o, "</w:tbl><w:p/>");
    of_sb_puts(&o, "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/><w:pgMar w:top=\"1417\" w:right=\"1701\" w:bottom=\"1417\" "
                   "w:left=\"1701\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/></w:sectPr></w:body></w:document>");
    of_zipw_add(&w, "word/document.xml", o.s, o.n, true);
    of_sb_puts(&rels, "</Relationships>");
    of_zipw_add(&w, "word/_rels/document.xml.rels", rels.s, rels.n, true);
    bool ok = !o.oom && !rels.oom && of_zipw_finish(&w, path);
    of_sb_free(&rels);
    of_sb_free(&o);
    of_zipw_free(&w);
    if (!ok) snprintf(err, errlen, "write");
    return ok;
}

bool of_docx_save(of_doc_t *d, const char *path, char *err, size_t errlen)
{
    err[0] = 0;
    if (d->fmt == OF_DOC_DOCX && d->zip.n && d->src) return save_keeping(d, path, err, errlen);
    return save_new(d, path, err, errlen);
}

bool of_doc_new_file(int fmt, const char *path, const char *title)
{
    of_doc_t *d = of_doc_new(fmt);
    if (!d) return false;
    of_para_t *p = of_doc_insert_para(d, 0);
    if (p && title && title[0]) {
        p->heading = 1;
        of_para_append(d, p, title, strlen(title), 0);
        p = of_doc_insert_para(d, 1);
    }
    char err[32];
    bool ok = fmt == OF_DOC_ODT ? of_odt_save(d, path, err, sizeof err) : of_docx_save(d, path, err, sizeof err);
    of_doc_free(d);
    return ok;
}
