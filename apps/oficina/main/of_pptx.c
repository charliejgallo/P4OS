/*
 * P4OS - Office: PowerPoint (.pptx, .ppsx, .potx, .pptm), read.
 *
 * presentation.xml gives the slide size and order; each slide points to its
 * layout, each layout to its master, each master to its theme. A shape's
 * place comes from its own xfrm or from the placeholder it fills (the
 * layout's, then the master's, matched by idx, then by type). Its text
 * styles come, level by level, from the paragraph, the shape's lstStyle,
 * the layout's and master's placeholder lstStyle, the master's title, body
 * or other text style, and the presentation's default text style. Colours
 * from the theme through the master's colour map, with lumMod, lumOff, tint
 * and shade. Groups are flattened with their child offsets; tables read as
 * grids; charts, SmartArt and objects become labelled boxes; the master's
 * and the layout's own shapes are drawn under the slide's unless the slide
 * says not to. Speaker notes from the notes slide's body.
 */
#include "of_slides.h"
#include "of_doc.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define EMU 12700.0

extern const char *(*of_tr)(const char *es);
#define _(s) (of_tr ? of_tr(s) : (s))

/* -------------------------------------------------------------------------- */
/* Parts and relationships                                                     */
/* -------------------------------------------------------------------------- */

typedef struct { char id[32], target[260], type[100]; bool ext; } rel_t;
typedef struct { rel_t *r; int n; } rels_t;

typedef struct {
    char    name[200];
    char   *src;
    xn_t   *root;
    rels_t  rels;
} part_t;

static void join_path(const char *from_part, const char *target, char *out, size_t len)
{
    if (target[0] == '/') { snprintf(out, len, "%s", target + 1); return; }
    char t[400];
    snprintf(t, sizeof t, "%s", from_part);
    char *sl = strrchr(t, '/');
    if (sl) sl[1] = 0;
    else t[0] = 0;
    size_t tl = strlen(t);
    snprintf(t + tl, sizeof t - tl, "%s", target);
    char *p;
    while ((p = strstr(t, "/../"))) {
        char *q = p;
        while (q > t && q[-1] != '/') q--;
        memmove(q, p + 4, strlen(p + 4) + 1);
    }
    while ((p = strstr(t, "/./"))) memmove(p, p + 2, strlen(p + 2) + 1);
    snprintf(out, len, "%s", t);
}

static bool part_load(of_deck_t *dk, const char *name, part_t *pt)
{
    memset(pt, 0, sizeof *pt);
    snprintf(pt->name, sizeof pt->name, "%s", name);
    size_t n;
    pt->src = (char *)of_deck_part(dk, name, &n);
    if (!pt->src) return false;
    pt->root = xn_parse(pt->src, n);
    /* its relationships */
    char rn[260];
    const char *b = strrchr(name, '/');
    snprintf(rn, sizeof rn, "%.*s_rels/%s.rels", b ? (int)(b - name + 1) : 0, name, b ? b + 1 : name);
    size_t rl;
    char *rs = (char *)of_deck_part(dk, rn, &rl);
    if (rs) {
        xn_t *rt = xn_parse(rs, rl);
        xn_t *top = xn_kid(rt, "Relationships");
        for (xn_t *c = top ? top->kid : NULL; c; c = c->next) {
            if (strcmp(c->local, "Relationship")) continue;
            rel_t *nr = of_realloc(pt->rels.r, (size_t)(pt->rels.n + 1) * sizeof *nr);
            if (!nr) break;
            pt->rels.r = nr;
            rel_t *r = &nr[pt->rels.n++];
            memset(r, 0, sizeof *r);
            xn_attr(c, "Id", r->id, sizeof r->id);
            xn_attr(c, "Type", r->type, sizeof r->type);
            char tg[260] = "", mode[16] = "";
            xn_attr(c, "Target", tg, sizeof tg);
            xn_attr(c, "TargetMode", mode, sizeof mode);
            r->ext = !strcmp(mode, "External");
            if (r->ext) snprintf(r->target, sizeof r->target, "%s", tg);
            else join_path(name, tg, r->target, sizeof r->target);
        }
        xn_free(rt);
        of_free(rs);
    }
    return pt->root != NULL;
}

static void part_free(part_t *pt)
{
    xn_free(pt->root);
    of_free(pt->src);
    of_free(pt->rels.r);
    memset(pt, 0, sizeof *pt);
}

/* the part's root element (not the white space before it) */
static xn_t *doc_el(const part_t *pt)
{
    xn_t *n = pt->root ? pt->root->kid : NULL;
    while (n && n->text) n = n->next;
    return n;
}

static const rel_t *rel_id(const part_t *pt, const char *id)
{
    for (int i = 0; i < pt->rels.n; i++) if (!strcmp(pt->rels.r[i].id, id)) return &pt->rels.r[i];
    return NULL;
}

static const rel_t *rel_type(const part_t *pt, const char *suffix)
{
    size_t sl = strlen(suffix);
    for (int i = 0; i < pt->rels.n; i++) {
        size_t l = strlen(pt->rels.r[i].type);
        if (l >= sl && !strcmp(pt->rels.r[i].type + l - sl, suffix)) return &pt->rels.r[i];
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/* Colours                                                                     */
/* -------------------------------------------------------------------------- */

static const char *const SCHEME[12] = { "dk1", "lt1", "dk2", "lt2", "accent1", "accent2", "accent3", "accent4", "accent5", "accent6", "hlink", "folHlink" };

typedef struct {
    uint32_t sch[12];           /* the theme's */
    char     map[12][12];       /* the master's clrMap: bg1 -> lt1... */
} colors_t;

static void rgb_hsl(uint32_t c, float *h, float *s, float *l)
{
    float r = ((c >> 16) & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = (c & 255) / 255.0f;
    float mx = r > g ? (r > b ? r : b) : (g > b ? g : b), mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    *l = (mx + mn) / 2;
    if (mx == mn) { *h = *s = 0; return; }
    float d = mx - mn;
    *s = *l > 0.5f ? d / (2 - mx - mn) : d / (mx + mn);
    if (mx == r) *h = (g - b) / d + (g < b ? 6 : 0);
    else if (mx == g) *h = (b - r) / d + 2;
    else *h = (r - g) / d + 4;
    *h /= 6;
}

static float hue(float p, float q, float t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0f / 6) return p + (q - p) * 6 * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3) return p + (q - p) * (2.0f / 3 - t) * 6;
    return p;
}

static uint32_t hsl_rgb(float h, float s, float l)
{
    float r, g, b;
    if (s == 0) r = g = b = l;
    else {
        float q = l < 0.5f ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
        r = hue(p, q, h + 1.0f / 3);
        g = hue(p, q, h);
        b = hue(p, q, h - 1.0f / 3);
    }
    int R = (int)(r * 255 + 0.5f), G = (int)(g * 255 + 0.5f), B = (int)(b * 255 + 0.5f);
    return (uint32_t)(R << 16 | G << 8 | B);
}

static uint32_t scheme_color(const colors_t *C, const char *name, uint32_t ph)
{
    if (!strcmp(name, "phClr")) return ph;
    char nm[16];
    snprintf(nm, sizeof nm, "%s", name);
    /* bg1, tx1, bg2, tx2 through the map */
    static const char *const M[] = { "bg1", "tx1", "bg2", "tx2" };
    for (int i = 0; i < 4; i++)
        if (!strcmp(nm, M[i])) {
            const char *to = C->map[i][0] ? C->map[i] : (i == 0 ? "lt1" : i == 1 ? "dk1" : i == 2 ? "lt2" : "dk2");
            snprintf(nm, sizeof nm, "%s", to);
        }
    for (int i = 0; i < 12; i++) if (!strcmp(nm, SCHEME[i])) return C->sch[i];
    return 0x1000000;
}

/* the colour of a node that is one (srgbClr, schemeClr...), or that holds one (solidFill) */
static uint32_t color_of(const colors_t *C, const xn_t *n, uint32_t ph)
{
    if (!n) return 0;
    if (!strcmp(n->local, "solidFill") || !strcmp(n->local, "fgClr") || !strcmp(n->local, "buClr")) {
        for (xn_t *c = n->kid; c; c = c->next) { uint32_t v = color_of(C, c, ph); if (v) return v; }
        return 0;
    }
    char v[40] = "";
    uint32_t c;
    if (!strcmp(n->local, "srgbClr")) { xn_attr(n, "val", v, sizeof v); c = (uint32_t)strtoul(v, NULL, 16) | 0x1000000u; }
    else if (!strcmp(n->local, "schemeClr")) { xn_attr(n, "val", v, sizeof v); c = scheme_color(C, v, ph); }
    else if (!strcmp(n->local, "sysClr")) { if (!xn_attr(n, "lastClr", v, sizeof v)) xn_attr(n, "val", v, sizeof v); c = !strcmp(v, "window") ? 0x1FFFFFF : !strcmp(v, "windowText") ? 0x1000000 : ((uint32_t)strtoul(v, NULL, 16) | 0x1000000u); }
    else if (!strcmp(n->local, "prstClr")) {
        xn_attr(n, "val", v, sizeof v);
        static const struct { const char *n; uint32_t c; } P[] = { { "black", 0 }, { "white", 0xFFFFFF }, { "red", 0xFF0000 }, { "green", 0x008000 },
            { "blue", 0x0000FF }, { "yellow", 0xFFFF00 }, { "gray", 0x808080 }, { "orange", 0xFFA500 } };
        c = 0x1000000;
        for (size_t i = 0; i < sizeof P / sizeof P[0]; i++) if (!strcmp(v, P[i].n)) c = P[i].c | 0x1000000u;
    } else if (!strcmp(n->local, "scrgbClr")) {
        int r = (int)(xn_num(n, "r", 0) / 100000 * 255), g = (int)(xn_num(n, "g", 0) / 100000 * 255), b = (int)(xn_num(n, "b", 0) / 100000 * 255);
        c = 0x1000000u | (uint32_t)(r << 16 | g << 8 | b);
    } else {
        return 0;
    }
    /* modifiers */
    uint32_t rgb = c & 0xFFFFFF;
    float lm = 1, lo = 0;
    bool hsl = false;
    for (xn_t *m = n->kid; m; m = m->next) {
        double val = xn_num(m, "val", 100000) / 100000.0;
        if (!strcmp(m->local, "lumMod")) { lm = (float)val; hsl = true; }
        else if (!strcmp(m->local, "lumOff")) { lo = (float)val; hsl = true; }
        else if (!strcmp(m->local, "tint")) {
            int R = (rgb >> 16) & 255, G = (rgb >> 8) & 255, B = rgb & 255;
            R = 255 - (int)((255 - R) * val); G = 255 - (int)((255 - G) * val); B = 255 - (int)((255 - B) * val);
            rgb = (uint32_t)(R << 16 | G << 8 | B);
        } else if (!strcmp(m->local, "shade")) {
            int R = (int)(((rgb >> 16) & 255) * val), G = (int)(((rgb >> 8) & 255) * val), B = (int)((rgb & 255) * val);
            rgb = (uint32_t)(R << 16 | G << 8 | B);
        } else if (!strcmp(m->local, "alpha") && val < 0.15) {
            return 0;       /* nearly transparent: not drawn */
        }
    }
    if (hsl) {
        float h, s, l;
        rgb_hsl(rgb, &h, &s, &l);
        l = l * lm + lo;
        if (l > 1) l = 1;
        if (l < 0) l = 0;
        rgb = hsl_rgb(h, s, l);
    }
    return 0x1000000u | rgb;
}

/* -------------------------------------------------------------------------- */
/* Text styles                                                                 */
/* -------------------------------------------------------------------------- */

typedef struct {
    float    marL, indent, size, before, after, line;
    int8_t   algn, b, i, bu;    /* -1 unset; bu: 0 none, 1 char, 2 auto */
    char     buc[8];
    uint8_t  autonum;
    int      start;
    uint32_t color, bucolor;
    bool     has_marL, has_indent;
} lvl_t;

static void lvl_clear(lvl_t *l)
{
    memset(l, 0, sizeof *l);
    l->algn = l->b = l->i = l->bu = -1;
}

static uint8_t autonum_of(const char *t)
{
    if (!strncmp(t, "arabicParen", 11)) return 6;
    if (!strncmp(t, "arabic", 6)) return 1;
    if (!strncmp(t, "alphaLc", 7)) return 2;
    if (!strncmp(t, "alphaUc", 7)) return 3;
    if (!strncmp(t, "romanLc", 7)) return 4;
    if (!strncmp(t, "romanUc", 7)) return 5;
    return 1;
}

/* a pPr or lvlNpPr node into a level's values (only what it sets) */
static void read_ppr(const colors_t *C, const xn_t *n, lvl_t *l)
{
    if (!n) return;
    char v[32];
    if (xn_attr(n, "marL", v, sizeof v)) { l->marL = (float)(strtod(v, NULL) / EMU); l->has_marL = true; }
    if (xn_attr(n, "indent", v, sizeof v)) { l->indent = (float)(strtod(v, NULL) / EMU); l->has_indent = true; }
    if (xn_attr(n, "algn", v, sizeof v)) l->algn = (int8_t)(!strcmp(v, "ctr") ? OF_AL_CENTER : !strcmp(v, "r") ? OF_AL_RIGHT : !strncmp(v, "just", 4) || !strcmp(v, "dist") ? OF_AL_JUSTIFY : OF_AL_LEFT);
    for (xn_t *c = n->kid; c; c = c->next) {
        if (!strcmp(c->local, "buNone")) l->bu = 0;
        else if (!strcmp(c->local, "buChar")) {
            l->bu = 1;
            char ch[16] = "";
            xn_attr(c, "char", ch, sizeof ch);
            snprintf(l->buc, sizeof l->buc, "%.7s", ch);
        } else if (!strcmp(c->local, "buAutoNum")) {
            l->bu = 2;
            char t[32] = "";
            xn_attr(c, "type", t, sizeof t);
            l->autonum = autonum_of(t);
            l->start = (int)xn_num(c, "startAt", 1);
        } else if (!strcmp(c->local, "buFont")) {
            char tf[48] = "";
            xn_attr(c, "typeface", tf, sizeof tf);
            /* Wingdings and Symbol bullets: their letters mean other things */
            if (strstr(tf, "Wingdings") || strstr(tf, "Symbol")) snprintf(l->buc, sizeof l->buc, "%s", "\x01");
        } else if (!strcmp(c->local, "buClr")) {
            l->bucolor = color_of(C, c, 0);
        } else if (!strcmp(c->local, "spcBef") || !strcmp(c->local, "spcAft")) {
            xn_t *pts = xn_kid(c, "spcPts"), *pct = xn_kid(c, "spcPct");
            float val = pts ? (float)(xn_num(pts, "val", 0) / 100.0) : pct ? (float)(xn_num(pct, "val", 0) / 100000.0) * -1 : 0;
            if (!strcmp(c->local, "spcBef")) l->before = val;
            else l->after = val;
        } else if (!strcmp(c->local, "lnSpc")) {
            xn_t *pct = xn_kid(c, "spcPct");
            if (pct) l->line = (float)(xn_num(pct, "val", 100000) / 100000.0);
        } else if (!strcmp(c->local, "defRPr")) {
            if (xn_attr(c, "sz", v, sizeof v)) l->size = (float)(strtod(v, NULL) / 100.0);
            if (xn_attr(c, "b", v, sizeof v)) l->b = (int8_t)(v[0] == '1' || v[0] == 't');
            if (xn_attr(c, "i", v, sizeof v)) l->i = (int8_t)(v[0] == '1' || v[0] == 't');
            xn_t *f = xn_kid(c, "solidFill");
            if (f) l->color = color_of(C, f, 0);
        }
    }
    if (l->buc[0] == '\x01' && l->bu == 1) snprintf(l->buc, sizeof l->buc, "%s", "\xE2\x80\xA2");
}

/* a whole list style (lstStyle, titleStyle, bodyStyle...): nine levels */
typedef struct { lvl_t l[9]; } lst_t;

static void read_lst(const colors_t *C, const xn_t *n, lst_t *s)
{
    for (int i = 0; i < 9; i++) lvl_clear(&s->l[i]);
    if (!n) return;
    for (xn_t *c = n->kid; c; c = c->next) {
        if (strncmp(c->local, "lvl", 3) || !strstr(c->local, "pPr")) continue;
        int k = atoi(c->local + 3) - 1;
        if (k >= 0 && k < 9) read_ppr(C, c, &s->l[k]);
    }
    /* defPPr: under level 1 */
    xn_t *def = xn_kid(n, "defPPr");
    if (def) { lvl_t d; lvl_clear(&d); read_ppr(C, def, &d); if (!s->l[0].size && d.size) s->l[0].size = d.size; }
}

/* the first value set along a chain of levels */
static void merge(lvl_t *out, const lvl_t *const *chain, int n)
{
    lvl_clear(out);
    for (int i = n - 1; i >= 0; i--) {
        const lvl_t *l = chain[i];
        if (!l) continue;
        if (l->has_marL) { out->marL = l->marL; out->has_marL = true; }
        if (l->has_indent) { out->indent = l->indent; out->has_indent = true; }
        if (l->size) out->size = l->size;
        if (l->before) out->before = l->before;
        if (l->after) out->after = l->after;
        if (l->line) out->line = l->line;
        if (l->algn >= 0) out->algn = l->algn;
        if (l->b >= 0) out->b = l->b;
        if (l->i >= 0) out->i = l->i;
        if (l->bu >= 0) { out->bu = l->bu; snprintf(out->buc, sizeof out->buc, "%s", l->buc); out->autonum = l->autonum; out->start = l->start; }
        if (l->color) out->color = l->color;
        if (l->bucolor) out->bucolor = l->bucolor;
    }
}

/* -------------------------------------------------------------------------- */
/* Placeholders of layouts and masters                                         */
/* -------------------------------------------------------------------------- */

enum { PH_NONE = 0, PH_TITLE, PH_BODY, PH_SUBTITLE, PH_DATE, PH_FOOTER, PH_NUMBER, PH_PIC, PH_OTHER };

static int ph_type(const char *t)
{
    if (!t[0] || !strcmp(t, "obj") || !strcmp(t, "body")) return PH_BODY;
    if (!strcmp(t, "title") || !strcmp(t, "ctrTitle")) return PH_TITLE;
    if (!strcmp(t, "subTitle")) return PH_SUBTITLE;
    if (!strcmp(t, "dt")) return PH_DATE;
    if (!strcmp(t, "ftr")) return PH_FOOTER;
    if (!strcmp(t, "sldNum")) return PH_NUMBER;
    if (!strcmp(t, "pic")) return PH_PIC;
    return PH_OTHER;
}

typedef struct {
    int    type, idx;
    bool   has_xfrm;
    float  x, y, w, h;
    lst_t  lst;
    const xn_t *body;           /* bodyPr */
    const xn_t *sp;
} ph_t;

typedef struct {
    part_t  pt;
    ph_t   *ph;
    int     nph;
    lst_t   title, body, other; /* masters only */
    bool    show_master;        /* layouts: draw the master's shapes */
    int     master;             /* layouts: the index of their master */
    colors_t col;               /* masters: the theme with their map */
} layer_t;

typedef struct {
    of_deck_t *dk;
    layer_t   *masters, *layouts;
    int        nm, nl;
    lst_t      deftext;         /* presentation.xml's defaultTextStyle */
    xn_t      *pres_root;
    char      *pres_src;
} ctx_t;

static bool xfrm_of(const xn_t *x, float *px, float *py, float *pw, float *ph)
{
    xn_t *off = xn_kid(x, "off"), *ext = xn_kid(x, "ext");
    if (!off || !ext) return false;
    *px = (float)(xn_num(off, "x", 0) / EMU);
    *py = (float)(xn_num(off, "y", 0) / EMU);
    *pw = (float)(xn_num(ext, "cx", 0) / EMU);
    *ph = (float)(xn_num(ext, "cy", 0) / EMU);
    return true;
}

static void collect_ph(layer_t *L, const colors_t *C)
{
    xn_t *tree = xn_path(doc_el(&L->pt), "cSld/spTree");
    for (xn_t *c = tree ? tree->kid : NULL; c; c = c->next) {
        if (strcmp(c->local, "sp") && strcmp(c->local, "pic")) continue;
        xn_t *phn = xn_path(c, strcmp(c->local, "sp") ? "nvPicPr/nvPr/ph" : "nvSpPr/nvPr/ph");
        if (!phn) continue;
        ph_t *n = of_realloc(L->ph, (size_t)(L->nph + 1) * sizeof *n);
        if (!n) return;
        L->ph = n;
        ph_t *p = &L->ph[L->nph++];
        memset(p, 0, sizeof *p);
        char t[24] = "";
        xn_attr(phn, "type", t, sizeof t);
        p->type = ph_type(t);
        p->idx = (int)xn_num(phn, "idx", -1);
        xn_t *x = xn_path(c, "spPr/xfrm");
        p->has_xfrm = x && xfrm_of(x, &p->x, &p->y, &p->w, &p->h);
        read_lst(C, xn_path(c, "txBody/lstStyle"), &p->lst);
        p->body = xn_path(c, "txBody/bodyPr");
        p->sp = c;
    }
}

static const ph_t *find_ph(const layer_t *L, int type, int idx)
{
    if (!L) return NULL;
    if (idx >= 0)
        for (int i = 0; i < L->nph; i++) if (L->ph[i].idx == idx && (L->ph[i].type == type || type == PH_BODY || L->ph[i].type == PH_BODY)) return &L->ph[i];
    for (int i = 0; i < L->nph; i++) if (L->ph[i].type == type) return &L->ph[i];
    /* a subtitle falls back to the body */
    if (type == PH_SUBTITLE) for (int i = 0; i < L->nph; i++) if (L->ph[i].type == PH_BODY) return &L->ph[i];
    return NULL;
}

static int load_master(ctx_t *X, const char *name)
{
    for (int i = 0; i < X->nm; i++) if (!strcmp(X->masters[i].pt.name, name)) return i;
    layer_t *n = of_realloc(X->masters, (size_t)(X->nm + 1) * sizeof *n);
    if (!n) return -1;
    X->masters = n;
    layer_t *M = &X->masters[X->nm];
    memset(M, 0, sizeof *M);
    if (!part_load(X->dk, name, &M->pt)) { part_free(&M->pt); return -1; }
    X->nm++;
    /* the theme's colours */
    static const uint32_t DEF[12] = { 0x000000, 0xFFFFFF, 0x44546A, 0xE7E6E6, 0x4472C4, 0xED7D31, 0xA5A5A5, 0xFFC000, 0x5B9BD5, 0x70AD47, 0x0563C1, 0x954F72 };
    for (int i = 0; i < 12; i++) M->col.sch[i] = DEF[i] | 0x1000000u;
    const rel_t *th = rel_type(&M->pt, "/theme");
    if (th) {
        part_t tp;
        if (part_load(X->dk, th->target, &tp)) {
            xn_t *cs = xn_path(doc_el(&tp), "themeElements/clrScheme");
            for (int i = 0; cs && i < 12; i++) {
                xn_t *e = xn_kid(cs, SCHEME[i]);
                if (e && e->kid) {
                    /* the first element child */
                    xn_t *k = e->kid;
                    while (k && k->text) k = k->next;
                    uint32_t v = k ? color_of(&M->col, k, 0) : 0;
                    if (v) M->col.sch[i] = v;
                }
            }
        }
        part_free(&tp);
    }
    xn_t *map = xn_kid(doc_el(&M->pt), "clrMap");
    static const char *const K[] = { "bg1", "tx1", "bg2", "tx2" };
    for (int i = 0; i < 4; i++) xn_attr(map, K[i], M->col.map[i], sizeof M->col.map[i]);
    xn_t *ts = xn_kid(doc_el(&M->pt), "txStyles");
    read_lst(&M->col, xn_kid(ts, "titleStyle"), &M->title);
    read_lst(&M->col, xn_kid(ts, "bodyStyle"), &M->body);
    read_lst(&M->col, xn_kid(ts, "otherStyle"), &M->other);
    collect_ph(M, &M->col);
    return X->nm - 1;
}

static int load_layout(ctx_t *X, const char *name)
{
    for (int i = 0; i < X->nl; i++) if (!strcmp(X->layouts[i].pt.name, name)) return i;
    layer_t *n = of_realloc(X->layouts, (size_t)(X->nl + 1) * sizeof *n);
    if (!n) return -1;
    X->layouts = n;
    layer_t *L = &X->layouts[X->nl];
    memset(L, 0, sizeof *L);
    if (!part_load(X->dk, name, &L->pt)) { part_free(&L->pt); return -1; }
    X->nl++;
    const rel_t *mr = rel_type(&L->pt, "/slideMaster");
    int m = mr ? load_master(X, mr->target) : -1;
    L = &X->layouts[X->nl - 1];
    L->master = m;
    char v[8] = "";
    L->show_master = !(xn_attr(doc_el(&L->pt), "showMasterSp", v, sizeof v) && (v[0] == '0' || v[0] == 'f'));
    collect_ph(L, m >= 0 ? &X->masters[m].col : NULL);
    return X->nl - 1;
}

/* -------------------------------------------------------------------------- */
/* Shapes                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct {
    float ox, oy, sx, sy;       /* a group's mapping: x' = ox + x * sx */
} grp_t;

typedef struct {
    ctx_t         *X;
    const part_t  *pt;          /* the part the shapes are in (for its relationships) */
    const layer_t *layout, *master;
    const colors_t *col;
    sl_slide_t    *slide;
    bool           own;         /* the slide's own shapes (placeholders filled) */
} sctx_t;

static int geom_of(const char *p)
{
    if (!strcmp(p, "rect") || !strcmp(p, "snip1Rect") || !strcmp(p, "flowChartProcess")) return SG_RECT;
    if (!strcmp(p, "roundRect") || !strcmp(p, "round1Rect") || !strcmp(p, "round2SameRect") || !strcmp(p, "flowChartAlternateProcess")) return SG_ROUND;
    if (!strcmp(p, "ellipse") || !strcmp(p, "flowChartConnector") || !strcmp(p, "donut")) return SG_ELLIPSE;
    if (!strcmp(p, "triangle") || !strcmp(p, "rtTriangle")) return SG_TRIANGLE;
    if (!strcmp(p, "diamond") || !strcmp(p, "flowChartDecision")) return SG_DIAMOND;
    if (strstr(p, "Arrow") || strstr(p, "arrow") || !strcmp(p, "chevron") || !strcmp(p, "homePlate")) return SG_ARROW;
    return SG_RECT;
}

static void text_body(sctx_t *S, const xn_t *txb, sl_text_t *out, const ph_t *lph, const ph_t *mph, int phtype, uint32_t font_color)
{
    if (!txb) return;
    const colors_t *C = S->col;
    /* the body: insets, anchor, autofit (from the shape, else its placeholders) */
    const xn_t *bp = xn_kid(txb, "bodyPr");
    const xn_t *bps[3] = { bp, lph ? lph->body : NULL, mph ? mph->body : NULL };
    char v[24];
    for (int i = 2; i >= 0; i--) {
        const xn_t *b = bps[i];
        if (!b) continue;
        if (xn_attr(b, "lIns", v, sizeof v)) out->l = (float)(strtod(v, NULL) / EMU);
        if (xn_attr(b, "tIns", v, sizeof v)) out->t = (float)(strtod(v, NULL) / EMU);
        if (xn_attr(b, "rIns", v, sizeof v)) out->r = (float)(strtod(v, NULL) / EMU);
        if (xn_attr(b, "bIns", v, sizeof v)) out->b = (float)(strtod(v, NULL) / EMU);
        if (xn_attr(b, "anchor", v, sizeof v)) out->anchor = (uint8_t)(!strcmp(v, "ctr") ? 1 : !strcmp(v, "b") ? 2 : 0);
        if (xn_attr(b, "wrap", v, sizeof v)) out->wrap = strcmp(v, "none") != 0;
    }
    float scale = 1, lred = 0;
    xn_t *af = bp ? xn_kid(bp, "normAutofit") : NULL;
    if (af) {
        scale = (float)(xn_num(af, "fontScale", 100000) / 100000.0);
        lred = (float)(xn_num(af, "lnSpcReduction", 0) / 100000.0);
    }
    lst_t own;
    read_lst(C, xn_kid(txb, "lstStyle"), &own);
    const lst_t *master_style = !S->master ? NULL : phtype == PH_TITLE ? &S->master->title
                              : phtype == PH_BODY || phtype == PH_SUBTITLE ? &S->master->body : &S->master->other;
    for (xn_t *pn = txb->kid; pn; pn = pn->next) {
        if (strcmp(pn->local, "p")) continue;
        sl_para_t *p = sl_add_para(out);
        if (!p) return;
        xn_t *ppr = xn_kid(pn, "pPr");
        int lv = (int)xn_num(ppr, "lvl", 0);
        if (lv < 0 || lv > 8) lv = 0;
        p->level = (uint8_t)lv;
        lvl_t here;
        lvl_clear(&here);
        read_ppr(C, ppr, &here);
        const lvl_t *chain[6] = { &here, &own.l[lv], lph ? &lph->lst.l[lv] : NULL, mph ? &mph->lst.l[lv] : NULL,
                                  master_style ? &master_style->l[lv] : NULL, &S->X->deftext.l[lv] };
        lvl_t L;
        merge(&L, chain, 6);
        p->align = (uint8_t)(L.algn >= 0 ? L.algn : OF_AL_LEFT);
        p->margin = L.marL;
        p->indent = L.indent;
        if (L.bu == 1) snprintf(p->bullet, sizeof p->bullet, "%s", L.buc[0] ? L.buc : "\xE2\x80\xA2");
        else if (L.bu == 2) { p->autonum = L.autonum; p->start = L.start > 0 ? L.start : 1; }
        p->line = L.line > 0 ? L.line - lred : 1;
        float base = (L.size > 0 ? L.size : 18) * scale;
        p->before = L.before < 0 ? -L.before * base * 1.2f : L.before;
        p->after = L.after < 0 ? -L.after * base * 1.2f : L.after;
        p->empty_size = base;
        /* the shape's own font colour (its style's fontRef) under the paragraph's and the shape's list style,
         * over the placeholders' and the master's */
        uint32_t defcol = here.color ? here.color : own.l[lv].color ? own.l[lv].color : font_color ? font_color : L.color ? L.color : scheme_color(C, "tx1", 0);
        p->bullet_color = L.bucolor ? L.bucolor : 0;
        for (xn_t *r = pn->kid; r; r = r->next) {
            bool br = !strcmp(r->local, "br");
            if (strcmp(r->local, "r") && strcmp(r->local, "fld") && !br) {
                if (!strcmp(r->local, "endParaRPr")) {
                    char sz[16];
                    if (xn_attr(r, "sz", sz, sizeof sz)) p->empty_size = (float)(strtod(sz, NULL) / 100.0) * scale;
                }
                continue;
            }
            of_sb_t tx = { 0 };
            if (br) of_sb_putc(&tx, '\n');
            else xn_text(xn_kid(r, "t"), &tx);
            sl_run_t *run = sl_add_run(p, tx.s ? tx.s : "", tx.n);
            of_sb_free(&tx);
            if (!run) break;
            xn_t *rpr = xn_kid(r, "rPr");
            run->size = base;
            run->b = L.b > 0;
            run->i = L.i > 0;
            run->color = defcol;
            if (rpr) {
                if (xn_attr(rpr, "sz", v, sizeof v)) run->size = (float)(strtod(v, NULL) / 100.0) * scale;
                if (xn_attr(rpr, "b", v, sizeof v)) run->b = v[0] == '1' || v[0] == 't';
                if (xn_attr(rpr, "i", v, sizeof v)) run->i = v[0] == '1' || v[0] == 't';
                if (xn_attr(rpr, "u", v, sizeof v)) run->u = strcmp(v, "none") != 0;
                if (xn_attr(rpr, "strike", v, sizeof v)) run->s = strcmp(v, "noStrike") != 0;
                if (xn_attr(rpr, "baseline", v, sizeof v)) { int bl = atoi(v); run->sup = bl > 0 ? 1 : bl < 0 ? 2 : 0; }
                xn_t *f = xn_kid(rpr, "solidFill");
                if (f) { uint32_t c = color_of(C, f, 0); if (c) run->color = c; }
                xn_t *hl = xn_kid(rpr, "hlinkClick");
                char id[32];
                if (hl && xn_attr(hl, "r:id", id, sizeof id)) {
                    const rel_t *rl = rel_id(S->pt, id);
                    if (rl && rl->ext) run->link = of_strdup(rl->target);
                    if (!f) run->color = scheme_color(C, "hlink", 0);
                    run->u = 1;
                }
            }
        }
        /* a paragraph's first run gives the bullet its size */
        if (!p->bullet_color && p->nr) p->bullet_color = p->r[0].color;
        if (!p->bullet_color) p->bullet_color = defcol;
    }
}

static void place(const grp_t *g, sl_shape_t *sh)
{
    sh->x = g->ox + sh->x * g->sx;
    sh->y = g->oy + sh->y * g->sy;
    sh->w *= g->sx;
    sh->h *= g->sy;
}

static void shape_style(sctx_t *S, const xn_t *node, sl_shape_t *sh, uint32_t *font_color)
{
    const colors_t *C = S->col;
    xn_t *sp = xn_kid(node, "spPr");
    xn_t *st = xn_kid(node, "style");
    bool fill_set = false, line_set = false;
    for (xn_t *c = sp ? sp->kid : NULL; c; c = c->next) {
        if (!strcmp(c->local, "solidFill")) { sh->fill = color_of(C, c, 0); fill_set = true; }
        else if (!strcmp(c->local, "noFill")) { sh->fill = 0; fill_set = true; }
        else if (!strcmp(c->local, "gradFill")) {
            xn_t *gs = xn_path(c, "gsLst/gs");
            for (xn_t *k = gs ? gs->kid : NULL; k && !sh->fill; k = k->next) sh->fill = color_of(C, k, 0);
            fill_set = true;
        } else if (!strcmp(c->local, "pattFill")) { sh->fill = color_of(C, xn_kid(c, "fgClr"), 0); fill_set = true; }
        else if (!strcmp(c->local, "blipFill")) {
            xn_t *bl = xn_kid(c, "blip");
            char id[32];
            if (bl && xn_attr(bl, "r:embed", id, sizeof id)) {
                const rel_t *r = rel_id(S->pt, id);
                if (r && !r->ext) sh->pic = of_strdup(r->target);
            }
            fill_set = true;
        } else if (!strcmp(c->local, "ln")) {
            line_set = true;
            sh->line_w = (float)(xn_num(c, "w", 9525) / EMU);
            if (xn_kid(c, "noFill")) sh->line = 0;
            else {
                xn_t *f = xn_kid(c, "solidFill");
                if (f) sh->line = color_of(C, f, 0);
                else line_set = false;
            }
        } else if (!strcmp(c->local, "prstGeom")) {
            char p[48] = "";
            xn_attr(c, "prst", p, sizeof p);
            sh->geom = geom_of(p);
            if (!strcmp(p, "line") || !strcmp(p, "straightConnector1")) sh->kind = SL_LINE;
        } else if (!strcmp(c->local, "custGeom")) {
            sh->geom = SG_RECT;
        }
    }
    /* the theme's style references, for what the shape does not say */
    if (st) {
        xn_t *fr = xn_kid(st, "fillRef"), *lr = xn_kid(st, "lnRef"), *fo = xn_kid(st, "fontRef");
        if (!fill_set && fr && xn_num(fr, "idx", 0) > 0) {
            for (xn_t *k = fr->kid; k && !sh->fill; k = k->next) sh->fill = color_of(C, k, 0);
        }
        if (!line_set && lr && xn_num(lr, "idx", 0) > 0) {
            for (xn_t *k = lr->kid; k && !sh->line; k = k->next) sh->line = color_of(C, k, 0);
            if (!sh->line_w) sh->line_w = 0.75f * (float)xn_num(lr, "idx", 1);
        }
        if (fo) for (xn_t *k = fo->kid; k && !*font_color; k = k->next) *font_color = color_of(C, k, 0);
    }
}

static void read_tree(sctx_t *S, const xn_t *tree, const grp_t *g, int depth);

static void read_table(sctx_t *S, const xn_t *tbl, sl_shape_t *sh)
{
    const colors_t *C = S->col;
    xn_t *grid = xn_kid(tbl, "tblGrid");
    for (xn_t *c = grid ? grid->kid : NULL; c; c = c->next) {
        if (strcmp(c->local, "gridCol")) continue;
        float *n = of_realloc(sh->colw, (size_t)(sh->cols + 1) * sizeof *n);
        if (!n) return;
        sh->colw = n;
        sh->colw[sh->cols++] = (float)(xn_num(c, "w", 0) / EMU);
    }
    xn_t *pr = xn_kid(tbl, "tblPr");
    char v[8];
    bool first = pr && xn_attr(pr, "firstRow", v, sizeof v) && v[0] == '1';
    bool band = pr && xn_attr(pr, "bandRow", v, sizeof v) && v[0] == '1';
    uint32_t acc = scheme_color(C, "accent1", 0);
    for (xn_t *tr = tbl->kid; tr; tr = tr->next) {
        if (strcmp(tr->local, "tr")) continue;
        float *n = of_realloc(sh->rowh, (size_t)(sh->rows + 1) * sizeof *n);
        if (!n) return;
        sh->rowh = n;
        int r = sh->rows++;
        sh->rowh[r] = (float)(xn_num(tr, "h", 0) / EMU);
        int col = 0;
        for (xn_t *tc = tr->kid; tc; tc = tc->next) {
            if (strcmp(tc->local, "tc")) continue;
            sl_cell_t *nc = of_realloc(sh->cells, (size_t)(sh->ncells + 1) * sizeof *nc);
            if (!nc) return;
            sh->cells = nc;
            sl_cell_t *cl = &sh->cells[sh->ncells++];
            memset(cl, 0, sizeof *cl);
            cl->row = r;
            cl->col = col;
            cl->cspan = (int)xn_num(tc, "gridSpan", 1);
            cl->rspan = (int)xn_num(tc, "rowSpan", 1);
            cl->hidden = (xn_attr(tc, "hMerge", v, sizeof v) && v[0] == '1') || (xn_attr(tc, "vMerge", v, sizeof v) && v[0] == '1');
            cl->tx.l = cl->tx.r = 7.2f;
            cl->tx.t = cl->tx.b = 3.6f;
            cl->tx.wrap = true;
            /* the table style's look, roughly: a coloured first row, banded rows */
            uint32_t fc = 0;
            if (first && r == 0) { cl->fill = acc; fc = 0x1FFFFFF; }
            else if (band && r % 2 == (first ? 1 : 0)) {
                uint32_t a = acc & 0xFFFFFF;
                int R = 255 - (255 - (int)((a >> 16) & 255)) / 5, G = 255 - (255 - (int)((a >> 8) & 255)) / 5, B = 255 - (255 - (int)(a & 255)) / 5;
                cl->fill = 0x1000000u | (uint32_t)(R << 16 | G << 8 | B);
            }
            xn_t *tcpr = xn_kid(tc, "tcPr");
            xn_t *f = tcpr ? xn_kid(tcpr, "solidFill") : NULL;
            if (f) cl->fill = color_of(C, f, 0);
            text_body(S, xn_kid(tc, "txBody"), &cl->tx, NULL, NULL, PH_NONE, fc);
            if (first && r == 0)
                for (int pi = 0; pi < cl->tx.np; pi++)
                    for (int k = 0; k < cl->tx.p[pi].nr; k++) cl->tx.p[pi].r[k].b = 1;
            col += cl->cspan > 0 ? cl->cspan : 1;
        }
    }
}

static void read_shape(sctx_t *S, const xn_t *n, const grp_t *g, int depth)
{
    const char *k = n->local;
    if (!strcmp(k, "AlternateContent")) {
        /* the fallback is what any reader can draw */
        xn_t *fb = xn_kid(n, "Fallback");
        if (!fb) fb = xn_kid(n, "Choice");
        if (fb) read_tree(S, fb, g, depth + 1);
        return;
    }
    if (!strcmp(k, "grpSp")) {
        xn_t *x = xn_path(n, "grpSpPr/xfrm");
        grp_t ng = *g;
        if (x) {
            float ox, oy, w, h;
            xn_t *co = xn_kid(x, "chOff"), *ce = xn_kid(x, "chExt");
            if (xfrm_of(x, &ox, &oy, &w, &h) && co && ce) {
                float cx = (float)(xn_num(co, "x", 0) / EMU), cy = (float)(xn_num(co, "y", 0) / EMU);
                float cw = (float)(xn_num(ce, "cx", 0) / EMU), ch = (float)(xn_num(ce, "cy", 0) / EMU);
                float sx = cw > 0 ? w / cw : 1, sy = ch > 0 ? h / ch : 1;
                /* child point p -> ox + (p - cx) * sx, then the outer group */
                ng.ox = g->ox + (ox - cx * sx) * g->sx;
                ng.oy = g->oy + (oy - cy * sy) * g->sy;
                ng.sx = g->sx * sx;
                ng.sy = g->sy * sy;
            }
        }
        read_tree(S, n, &ng, depth + 1);
        return;
    }
    bool is_sp = !strcmp(k, "sp"), is_pic = !strcmp(k, "pic"), is_frame = !strcmp(k, "graphicFrame"), is_cxn = !strcmp(k, "cxnSp");
    if (!is_sp && !is_pic && !is_frame && !is_cxn) return;
    /* placeholder? */
    const char *nv = is_sp ? "nvSpPr/nvPr/ph" : is_pic ? "nvPicPr/nvPr/ph" : is_frame ? "nvGraphicFramePr/nvPr/ph" : "nvCxnSpPr/nvPr/ph";
    xn_t *phn = xn_path(n, nv);
    int phtype = PH_NONE, idx = -1;
    if (phn) {
        char t[24] = "";
        xn_attr(phn, "type", t, sizeof t);
        phtype = ph_type(t);
        idx = (int)xn_num(phn, "idx", -1);
        if (!S->own) return;                    /* a layout's or master's placeholder is only a frame */
        /* date, footer and number placeholders: drawn only if the slide fills them */
    }
    const ph_t *lph = phn ? find_ph(S->layout, phtype, idx) : NULL;
    const ph_t *mph = phn ? find_ph(S->master, phtype, lph ? lph->idx : idx) : NULL;
    if (phn && !mph && lph) mph = find_ph(S->master, lph->type, -1);
    sl_shape_t *sh = sl_add_shape(S->slide);
    if (!sh) return;
    sh->placeholder = S->own;
    sh->kind = is_pic ? SL_PIC : is_frame ? SL_TABLE : is_cxn ? SL_LINE : SL_TEXT;
    /* place */
    xn_t *x = is_frame ? xn_kid(n, "xfrm") : xn_path(n, is_pic ? "spPr/xfrm" : "spPr/xfrm");
    bool has = x && xfrm_of(x, &sh->x, &sh->y, &sh->w, &sh->h);
    if (!has && lph && lph->has_xfrm) { sh->x = lph->x; sh->y = lph->y; sh->w = lph->w; sh->h = lph->h; has = true; }
    if (!has && mph && mph->has_xfrm) { sh->x = mph->x; sh->y = mph->y; sh->w = mph->w; sh->h = mph->h; has = true; }
    if (x) {
        sh->rot = (float)(xn_num(x, "rot", 0) / 60000.0);
        char v[8];
        sh->flip_h = xn_attr(x, "flipH", v, sizeof v) && v[0] == '1';
        sh->flip_v = xn_attr(x, "flipV", v, sizeof v) && v[0] == '1';
    }
    place(g, sh);
    uint32_t font_color = 0;
    if (is_sp || is_cxn) shape_style(S, n, sh, &font_color);
    if (sh->pic && is_sp) {
        /* a shape filled with a picture: drawn as the picture */
        sh->kind = SL_PIC;
    }
    if (is_pic) {
        xn_t *bl = xn_path(n, "blipFill/blip");
        char id[32];
        if (bl && xn_attr(bl, "r:embed", id, sizeof id)) {
            const rel_t *r = rel_id(S->pt, id);
            if (r && !r->ext) sh->pic = of_strdup(r->target);
        }
        uint32_t fc = 0;
        shape_style(S, n, sh, &fc);
        sh->kind = SL_PIC;
    }
    if (is_frame) {
        xn_t *gd = xn_path(n, "graphic/graphicData");
        char uri[120] = "";
        xn_attr(gd, "uri", uri, sizeof uri);
        xn_t *tbl = gd ? xn_kid(gd, "tbl") : NULL;
        if (tbl) read_table(S, tbl, sh);
        else {
            sh->kind = SL_TEXT;
            sh->geom = SG_RECT;
            sh->line = 0x1A0A0A0;
            sh->line_w = 0.75f;
            sh->label = of_strdup(strstr(uri, "chart") ? _("Gráfico") : strstr(uri, "diagram") ? _("Diagrama (SmartArt)") : _("Objeto"));
            /* the label as the box's text, centred */
            sh->tx.anchor = 1;
            sl_para_t *p = sl_add_para(&sh->tx);
            if (p) {
                p->align = OF_AL_CENTER;
                sl_run_t *r = sl_add_run(p, sh->label, strlen(sh->label));
                if (r) { r->color = 0x1808080; r->size = 14; r->i = 1; }
            }
        }
    }
    if (is_sp) {
        text_body(S, xn_kid(n, "txBody"), &sh->tx, lph, mph, phn ? phtype : PH_NONE, font_color);
        if (phn && !sh->tx.np) {
            /* an empty placeholder shows nothing in a slide show */
            sl_text_free(&sh->tx);
            if (!sh->fill && !sh->line) { S->slide->nsh--; return; }
        }
        if (!sh->geom && (sh->fill || sh->line)) sh->geom = SG_RECT;
        if (phtype == PH_TITLE && !S->slide->title) {
            of_sb_t t = { 0 };
            for (int i = 0; i < sh->tx.np; i++)
                for (int r = 0; r < sh->tx.p[i].nr; r++) of_sb_puts(&t, sh->tx.p[i].r[r].t);
            S->slide->title = of_sb_take(&t);
        }
        if (!sh->tx.np && !sh->fill && !sh->line && !sh->pic) S->slide->nsh--;
    }
}

static void read_tree(sctx_t *S, const xn_t *tree, const grp_t *g, int depth)
{
    if (!tree || depth > 12) return;
    for (xn_t *c = tree->kid; c; c = c->next) read_shape(S, c, g, depth);
}

static uint32_t background(sctx_t *S, const xn_t *csld, char **pic)
{
    xn_t *bg = xn_kid(csld, "bg");
    if (!bg) return 0;
    xn_t *pr = xn_kid(bg, "bgPr");
    if (pr) {
        for (xn_t *c = pr->kid; c; c = c->next) {
            if (!strcmp(c->local, "solidFill")) return color_of(S->col, c, 0);
            if (!strcmp(c->local, "gradFill")) {
                xn_t *gs = xn_path(c, "gsLst/gs");
                for (xn_t *k = gs ? gs->kid : NULL; k; k = k->next) { uint32_t v = color_of(S->col, k, 0); if (v) return v; }
            }
            if (!strcmp(c->local, "blipFill")) {
                xn_t *bl = xn_kid(c, "blip");
                char id[32];
                if (bl && xn_attr(bl, "r:embed", id, sizeof id)) {
                    const rel_t *r = rel_id(S->pt, id);
                    if (r && !r->ext && pic && !*pic) *pic = of_strdup(r->target);
                }
                return 0x1FFFFFF;
            }
        }
    }
    xn_t *ref = xn_kid(bg, "bgRef");
    for (xn_t *k = ref ? ref->kid : NULL; k; k = k->next) { uint32_t v = color_of(S->col, k, 0); if (v) return v; }
    return 0;
}

static char *notes_of(of_deck_t *dk, const part_t *slide)
{
    const rel_t *nr = rel_type(slide, "/notesSlide");
    if (!nr) return NULL;
    part_t np;
    if (!part_load(dk, nr->target, &np)) { part_free(&np); return NULL; }
    of_sb_t sb = { 0 };
    xn_t *tree = xn_path(doc_el(&np), "cSld/spTree");
    for (xn_t *c = tree ? tree->kid : NULL; c; c = c->next) {
        xn_t *ph = xn_path(c, "nvSpPr/nvPr/ph");
        char t[16] = "";
        if (!ph || !xn_attr(ph, "type", t, sizeof t) || strcmp(t, "body")) continue;
        xn_t *tb = xn_kid(c, "txBody");
        for (xn_t *p = tb ? tb->kid : NULL; p; p = p->next) {
            if (strcmp(p->local, "p")) continue;
            of_sb_t line = { 0 };
            for (xn_t *r = p->kid; r; r = r->next) {
                if (!strcmp(r->local, "r") || !strcmp(r->local, "fld")) xn_text(xn_kid(r, "t"), &line);
                else if (!strcmp(r->local, "br")) of_sb_putc(&line, '\n');
            }
            if (line.n) {
                if (sb.n) of_sb_putc(&sb, '\n');
                of_sb_put(&sb, line.s, line.n);
            }
            of_sb_free(&line);
        }
    }
    part_free(&np);
    return sb.n ? of_sb_take(&sb) : (of_sb_free(&sb), NULL);
}

bool sl_pptx_read(of_deck_t *dk, char *err, size_t errlen)
{
    ctx_t X = { .dk = dk };
    snprintf(dk->kind, sizeof dk->kind, "PPTX");
    /* the main part */
    part_t top;
    memset(&top, 0, sizeof top);
    char main[200] = "ppt/presentation.xml";
    {
        size_t rl;
        char *rs = (char *)of_deck_part(dk, "_rels/.rels", &rl);
        if (rs) {
            xn_t *rt = xn_parse(rs, rl);
            xn_t *rel = xn_kid(rt, "Relationships");
            for (xn_t *c = rel ? rel->kid : NULL; c; c = c->next) {
                char ty[120] = "", tg[200] = "";
                xn_attr(c, "Type", ty, sizeof ty);
                xn_attr(c, "Target", tg, sizeof tg);
                if (strstr(ty, "/officeDocument")) snprintf(main, sizeof main, "%s", tg[0] == '/' ? tg + 1 : tg);
            }
            xn_free(rt);
            of_free(rs);
        }
    }
    part_t pres;
    if (!part_load(dk, main, &pres)) { part_free(&pres); snprintf(err, errlen, "format"); return false; }
    xn_t *P = pres.root->kid;
    while (P && P->text) P = P->next;
    xn_t *sz = xn_kid(P, "sldSz");
    dk->w = (float)(xn_num(sz, "cx", 9144000) / EMU);
    dk->h = (float)(xn_num(sz, "cy", 6858000) / EMU);
    /* the default text style needs a colour map: the first master's */
    xn_t *ml = xn_kid(P, "sldMasterIdLst");
    if (ml && ml->kid) {
        char id[32];
        for (xn_t *m = ml->kid; m; m = m->next)
            if (xn_attr(m, "r:id", id, sizeof id)) {
                const rel_t *r = rel_id(&pres, id);
                if (r) load_master(&X, r->target);
                break;
            }
    }
    read_lst(X.nm ? &X.masters[0].col : NULL, xn_kid(P, "defaultTextStyle"), &X.deftext);
    xn_t *list = xn_kid(P, "sldIdLst");
    for (xn_t *c = list ? list->kid : NULL; c; c = c->next) {
        if (strcmp(c->local, "sldId")) continue;
        char id[32];
        if (!xn_attr(c, "r:id", id, sizeof id)) continue;
        const rel_t *r = rel_id(&pres, id);
        if (!r) continue;
        part_t sp;
        if (!part_load(dk, r->target, &sp)) { part_free(&sp); continue; }
        sl_slide_t *ns = of_realloc(dk->s, (size_t)(dk->ns + 1) * sizeof *ns);
        if (!ns) { part_free(&sp); break; }
        dk->s = ns;
        sl_slide_t *s = &dk->s[dk->ns++];
        memset(s, 0, sizeof *s);
        xn_t *sld = sp.root->kid;
        while (sld && sld->text) sld = sld->next;
        char v[8] = "";
        s->hidden = xn_attr(sld, "show", v, sizeof v) && (v[0] == '0' || v[0] == 'f');
        bool show_master = !(xn_attr(sld, "showMasterSp", v, sizeof v) && (v[0] == '0' || v[0] == 'f'));
        const rel_t *lr = rel_type(&sp, "/slideLayout");
        int li = lr ? load_layout(&X, lr->target) : -1;
        layer_t *L = li >= 0 ? &X.layouts[li] : NULL;
        layer_t *M = L && L->master >= 0 ? &X.masters[L->master] : X.nm ? &X.masters[0] : NULL;
        static const colors_t DEFC = { { 0x1000000, 0x1FFFFFF, 0x144546A, 0x1E7E6E6, 0x14472C4, 0x1ED7D31, 0x1A5A5A5, 0x1FFC000, 0x15B9BD5, 0x170AD47, 0x10563C1, 0x1954F72 }, { "" } };
        const colors_t *col = M ? &M->col : &DEFC;
        grp_t g0 = { 0, 0, 1, 1 };
        /* background: the slide's, the layout's, the master's */
        sctx_t S = { &X, &sp, L, M, col, s, false };
        s->bg = background(&S, xn_kid(sld, "cSld"), &s->bg_pic);
        if (!s->bg && L) { sctx_t S2 = { &X, &L->pt, L, M, col, s, false }; s->bg = background(&S2, xn_kid(doc_el(&L->pt), "cSld"), &s->bg_pic); }
        if (!s->bg && M) { sctx_t S3 = { &X, &M->pt, NULL, M, col, s, false }; s->bg = background(&S3, xn_kid(doc_el(&M->pt), "cSld"), &s->bg_pic); }
        if (!s->bg) s->bg = scheme_color(col, "bg1", 0);
        /* the master's and the layout's own shapes under the slide's */
        if (show_master && M && (!L || L->show_master)) {
            sctx_t SM = { &X, &M->pt, NULL, M, col, s, false };
            read_tree(&SM, xn_path(doc_el(&M->pt), "cSld/spTree"), &g0, 0);
        }
        if (show_master && L) {
            sctx_t SL = { &X, &L->pt, L, M, col, s, false };
            read_tree(&SL, xn_path(doc_el(&L->pt), "cSld/spTree"), &g0, 0);
        }
        S.own = true;
        read_tree(&S, xn_path(sld, "cSld/spTree"), &g0, 0);
        s->notes = notes_of(dk, &sp);
        part_free(&sp);
    }
    for (int i = 0; i < X.nl; i++) { part_free(&X.layouts[i].pt); of_free(X.layouts[i].ph); }
    for (int i = 0; i < X.nm; i++) { part_free(&X.masters[i].pt); of_free(X.masters[i].ph); }
    of_free(X.layouts);
    of_free(X.masters);
    part_free(&pres);
    part_free(&top);
    if (!dk->ns) { snprintf(err, errlen, "empty"); return false; }
    return true;
}
