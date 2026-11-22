/*
 * P4OS - Office: formulas.
 *
 * Excel's syntax, parsed into a tree and evaluated. The file keeps the
 * English names (xlsx stores "SUM", whatever the language of the program
 * that wrote it); the person can type the Spanish or German ones too
 * (SUMA, PROMEDIO, BUSCARV, SUMME...) and ';' between arguments, and the
 * formula is written back in the file's own words.
 *
 * Recalculation does not recurse from cell to cell: a long chain (A2=A1+1
 * down a few thousand rows) would overflow the LVGL task's stack. A cell
 * whose formula needs another one not yet computed gives up, that one goes
 * on an explicit stack, and the first is tried again when it is done. A
 * cell found on the stack again is a circular reference (#CIRC!, shown and
 * not saved as such: an error cell keeps the file valid).
 *
 * A formula with a function the app does not have keeps the value the file
 * had cached and its XML, and the cells that use it see that value.
 *
 * Everything is in double, which is soft-float on the board: slow, but
 * sheets are small and money wants the 15 digits. No libm in double there:
 * rounding, powers and square roots are done here.
 */
#include "of.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

/* -------------------------------------------------------------------------- */
/* The tree                                                                    */
/* -------------------------------------------------------------------------- */

enum { N_NUM, N_STR, N_BOOL, N_ERR, N_REF, N_RANGE, N_FN, N_BIN, N_NEG, N_POS, N_PCT, N_PAREN, N_NAME };
enum { OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_POW, OP_CAT, OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE };

/* absolute bits of a reference */
#define AB_C1 1
#define AB_R1 2
#define AB_C2 4
#define AB_R2 8
#define WHOLE_COL 16            /* A:A */
#define WHOLE_ROW 32            /* 1:1 */

struct of_node {
    uint8_t     k, op, abs, xlfn;
    int16_t     fn;             /* index in FNS, -1 unknown */
    int16_t     sheet;          /* -1: the formula's own sheet */
    int         nargs;
    double      n;
    char       *s;              /* number's text, string, error name, function or name */
    int32_t     r1, c1, r2, c2;
    of_node_t **a;
};

void of_formula_free(of_node_t *n)
{
    if (!n) return;
    for (int i = 0; i < n->nargs; i++) of_formula_free(n->a[i]);
    of_free(n->a);
    of_free(n->s);
    of_free(n);
}

static of_node_t *node(int k)
{
    of_node_t *n = of_alloc(sizeof *n);
    if (n) {
        n->k = (uint8_t)k;
        n->fn = -1;
        n->sheet = -1;
    }
    return n;
}

static bool add_arg(of_node_t *n, of_node_t *a)
{
    of_node_t **na = of_realloc(n->a, (size_t)(n->nargs + 1) * sizeof *na);
    if (!na) return false;
    n->a = na;
    n->a[n->nargs++] = a;
    return true;
}

/* -------------------------------------------------------------------------- */
/* Functions                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct ctx ctx_t;
typedef void (*fn_t)(ctx_t *x, of_node_t *n, of_val_t *out);

typedef struct {
    const char *name;           /* English, as the file has it */
    const char *es, *de;        /* what the person may type     */
    int8_t      min, max;       /* -1: any                      */
    uint8_t     xlfn;           /* written with "_xlfn." in xlsx */
    fn_t        fn;
} fndef_t;

static const fndef_t *fn_table(int *count);

/* -------------------------------------------------------------------------- */
/* Lexer and parser                                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *s;
    int         p;
    int         sheet_ix;
    const of_book_t *b;
    bool        unknown;
    bool        fail;
    bool        semi;           /* ';' between arguments: ',' is the decimal mark */
} ps_t;

static void sp(ps_t *P)
{
    while (P->s[P->p] == ' ' || P->s[P->p] == '\n' || P->s[P->p] == '\r' || P->s[P->p] == '\t') P->p++;
}

static of_node_t *p_expr(ps_t *P);

static bool is_ident(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' ||
           (unsigned char)c >= 0x80;
}

static int find_sheet(const of_book_t *b, const char *name, int len)
{
    if (!b) return -2;
    for (int i = 0; i < b->nsh; i++)
        if (b->sh[i].name && (int)strlen(b->sh[i].name) == len && strncasecmp(b->sh[i].name, name, (size_t)len) == 0)
            return i;
    return -2;
}

/* "A1", "$A$1", "A:A", "1:1", "A1:B2" at P->p; false if not a reference. */
static bool p_ref(ps_t *P, of_node_t *n)
{
    const char *s = P->s + P->p;
    int32_t r, c;
    int used;
    /* whole columns: A:B */
    {
        int i = 0, l = 0;
        uint8_t ab = 0;
        if (s[i] == '$') { ab |= AB_C1; i++; }
        int32_t c1 = 0;
        while (((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) && l < 4) {
            c1 = c1 * 26 + ((s[i] | 0x20) - 'a' + 1);
            i++;
            l++;
        }
        if (l && s[i] == ':') {
            int j = i + 1, l2 = 0;
            if (s[j] == '$') { ab |= AB_C2; j++; }
            int32_t c2 = 0;
            while (((s[j] >= 'A' && s[j] <= 'Z') || (s[j] >= 'a' && s[j] <= 'z')) && l2 < 4) {
                c2 = c2 * 26 + ((s[j] | 0x20) - 'a' + 1);
                j++;
                l2++;
            }
            if (l2 && !is_ident(s[j]) && s[j] != '(') {
                n->k = N_RANGE;
                n->c1 = c1 - 1;
                n->c2 = c2 - 1;
                n->r1 = 0;
                n->r2 = 1048575;
                n->abs = ab | WHOLE_COL;
                P->p += j;
                return true;
            }
        }
    }
    /* whole rows: 1:3 */
    if ((s[0] >= '0' && s[0] <= '9') || (s[0] == '$' && s[1] >= '0' && s[1] <= '9')) {
        int i = 0;
        uint8_t ab = 0;
        if (s[i] == '$') { ab |= AB_R1; i++; }
        long r1 = strtol(s + i, NULL, 10);
        while (s[i] >= '0' && s[i] <= '9') i++;
        if (s[i] != ':') return false;
        i++;
        if (s[i] == '$') { ab |= AB_R2; i++; }
        if (!(s[i] >= '0' && s[i] <= '9')) return false;
        long r2 = strtol(s + i, NULL, 10);
        while (s[i] >= '0' && s[i] <= '9') i++;
        if (r1 < 1 || r2 < 1) return false;
        n->k = N_RANGE;
        n->r1 = (int32_t)r1 - 1;
        n->r2 = (int32_t)r2 - 1;
        n->c1 = 0;
        n->c2 = 16383;
        n->abs = ab | WHOLE_ROW;
        P->p += i;
        return true;
    }
    if (!of_parse_ref(s, &r, &c, &used)) return false;
    if (is_ident(s[used]) || s[used] == '(') return false;     /* a name like "AB12C" or LOG10( */
    n->k = N_REF;
    n->r1 = n->r2 = r;
    n->c1 = n->c2 = c;
    n->abs = (uint8_t)((s[0] == '$' ? AB_C1 : 0) | (strchr(s, '$') && strchr(s, '$') < s + used && s[0] != '$' ? AB_R1 : 0));
    /* the second $ (or the only one, after the letters) marks the row */
    {
        int i = 0;
        if (s[i] == '$') i++;
        while ((s[i] >= 'A' && s[i] <= 'Z') || (s[i] >= 'a' && s[i] <= 'z')) i++;
        n->abs = (uint8_t)((s[0] == '$' ? AB_C1 : 0) | (s[i] == '$' ? AB_R1 : 0));
    }
    P->p += used;
    if (P->s[P->p] == ':') {
        int32_t r2, c2;
        int u2;
        const char *t = P->s + P->p + 1;
        if (of_parse_ref(t, &r2, &c2, &u2) && !is_ident(t[u2])) {
            int i = 0;
            if (t[i] == '$') i++;
            while ((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= 'a' && t[i] <= 'z')) i++;
            n->abs |= (uint8_t)((t[0] == '$' ? AB_C2 : 0) | (t[i] == '$' ? AB_R2 : 0));
            n->k = N_RANGE;
            n->r2 = r2;
            n->c2 = c2;
            P->p += 1 + u2;
        }
    }
    if (n->k == N_REF) n->abs |= (uint8_t)((n->abs & AB_C1 ? AB_C2 : 0) | (n->abs & AB_R1 ? AB_R2 : 0));
    if (n->k == N_RANGE) {
        if (n->r1 > n->r2) { int32_t t = n->r1; n->r1 = n->r2; n->r2 = t; }
        if (n->c1 > n->c2) { int32_t t = n->c1; n->c1 = n->c2; n->c2 = t; }
    }
    return true;
}

static int fn_lookup(const char *name, int len, bool *xlfn)
{
    int count;
    const fndef_t *F = fn_table(&count);
    *xlfn = false;
    if (len > 6 && strncasecmp(name, "_xlfn.", 6) == 0) {
        name += 6;
        len -= 6;
        *xlfn = true;
    }
    if (len > 7 && strncasecmp(name, "_xlws.", 6) == 0) {
        name += 6;
        len -= 6;
    }
    for (int i = 0; i < count; i++) {
        const char *names[3] = { F[i].name, F[i].es, F[i].de };
        for (int k = 0; k < 3; k++)
            if (names[k] && (int)strlen(names[k]) == len && strncasecmp(names[k], name, (size_t)len) == 0)
                return i;
    }
    /* AÑO typed in lower case: año (UTF-8 ñ is not folded by strncasecmp) */
    if (len == 4 && (strncmp(name, "a\xC3\xB1o", 4) == 0 || strncmp(name, "A\xC3\xB1o", 4) == 0 ||
                     strncmp(name, "a\xC3\x91o", 4) == 0)) {
        for (int i = 0; i < count; i++)
            if (!strcmp(F[i].name, "YEAR")) return i;
    }
    return -1;
}

static of_node_t *p_primary(ps_t *P)
{
    sp(P);
    const char *s = P->s;
    char c = s[P->p];
    if (!c) { P->fail = true; return NULL; }
    if (c == '(') {
        P->p++;
        of_node_t *e = p_expr(P);
        sp(P);
        if (s[P->p] != ')') { P->fail = true; of_formula_free(e); return NULL; }
        P->p++;
        of_node_t *n = node(N_PAREN);
        if (!n) { of_formula_free(e); return NULL; }
        add_arg(n, e);
        return n;
    }
    if (c == '"') {
        of_sb_t sb = { 0 };
        P->p++;
        for (;;) {
            char ch = s[P->p];
            if (!ch) { P->fail = true; break; }
            if (ch == '"') {
                if (s[P->p + 1] == '"') { of_sb_putc(&sb, '"'); P->p += 2; continue; }
                P->p++;
                break;
            }
            of_sb_putc(&sb, ch);
            P->p++;
        }
        of_node_t *n = node(N_STR);
        if (n) n->s = of_sb_take(&sb);
        else of_sb_free(&sb);
        return n;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && s[P->p + 1] >= '0' && s[P->p + 1] <= '9')) {
        /* a row range "1:3" first */
        int save = P->p;
        of_node_t *n = node(N_NUM);
        if (!n) return NULL;
        if (p_ref(P, n) && n->k == N_RANGE) return n;
        P->p = save;
        n->k = N_NUM;
        const char *st = s + P->p;
        char buf[64];
        int k = 0, i = 0;
        bool dot = false;
        while (k < 60) {
            char ch = st[i];
            if (ch >= '0' && ch <= '9') buf[k++] = ch;
            else if (!dot && (ch == '.' || (P->semi && ch == ',' && st[i + 1] >= '0' && st[i + 1] <= '9'))) { buf[k++] = '.'; dot = true; }
            else if ((ch == 'e' || ch == 'E') && (st[i + 1] == '+' || st[i + 1] == '-' || (st[i + 1] >= '0' && st[i + 1] <= '9'))) {
                buf[k++] = 'E';
                if (st[i + 1] == '+' || st[i + 1] == '-') buf[k++] = st[++i];
            } else break;
            i++;
        }
        buf[k] = 0;
        char *end;
        n->n = strtod(buf, &end);
        if (end == buf) { of_formula_free(n); P->fail = true; return NULL; }
        n->s = of_strdup(buf);
        P->p += i;
        return n;
    }
    if (c == '#') {
        int i = P->p + 1;
        while (s[i] && s[i] != '!' && s[i] != '?' && i - P->p < 8) i++;
        if (s[i] == '!' || s[i] == '?') i++;
        /* #DIV/0! has a slash and a zero inside */
        if (strncmp(s + P->p, "#DIV/0!", 7) == 0) i = P->p + 7;
        if (strncmp(s + P->p, "#N/A", 4) == 0) i = P->p + 4;
        of_node_t *n = node(N_ERR);
        if (!n) return NULL;
        n->s = of_strndup(s + P->p, (size_t)(i - P->p));
        n->n = of_err_from_name(n->s);
        P->p = i;
        return n;
    }
    /* a sheet: 'My sheet'!A1 or Sheet1!A1 */
    int sheet = -1;
    if (c == '\'') {
        int i = P->p + 1;
        of_sb_t nm = { 0 };
        while (s[i]) {
            if (s[i] == '\'') {
                if (s[i + 1] == '\'') { of_sb_putc(&nm, '\''); i += 2; continue; }
                break;
            }
            of_sb_putc(&nm, s[i++]);
        }
        if (s[i] == '\'' && s[i + 1] == '!') {
            sheet = find_sheet(P->b, nm.s ? nm.s : "", (int)nm.n);
            P->p = i + 2;
        }
        of_sb_free(&nm);
        if (sheet == -1) { P->fail = true; return NULL; }
    } else {
        int i = P->p;
        while (is_ident(s[i])) i++;
        if (s[i] == '!' && i > P->p) {
            sheet = find_sheet(P->b, s + P->p, i - P->p);
            P->p = i + 1;
        }
    }
    of_node_t *n = node(N_REF);
    if (!n) return NULL;
    if (p_ref(P, n)) {
        n->sheet = (int16_t)(sheet == -2 ? -2 : sheet);
        if (sheet == -2) { n->k = N_ERR; n->n = OE_REF; n->s = of_strdup("#REF!"); }
        return n;
    }
    if (sheet != -1) { of_formula_free(n); P->fail = true; return NULL; }
    /* a name: a function, TRUE/FALSE, or a defined name */
    int i = P->p;
    while (is_ident(s[i])) i++;
    if (i == P->p) { of_formula_free(n); P->fail = true; return NULL; }
    int len = i - P->p;
    const char *name = s + P->p;
    P->p = i;
    sp(P);
    if (s[P->p] == '(') {
        P->p++;
        n->k = N_FN;
        bool xlfn;
        n->fn = (int16_t)fn_lookup(name, len, &xlfn);
        n->xlfn = xlfn;
        n->s = of_strndup(name, (size_t)len);
        if (n->fn < 0) P->unknown = true;
        sp(P);
        if (s[P->p] == ')') { P->p++; return n; }
        for (;;) {
            sp(P);
            of_node_t *a;
            if ((s[P->p] == ',' && !P->semi) || s[P->p] == ';' || s[P->p] == ')') a = node(N_NAME);   /* an empty argument */
            else a = p_expr(P);
            if (!a || P->fail) { of_formula_free(a); P->fail = true; break; }
            add_arg(n, a);
            sp(P);
            if ((s[P->p] == ',' && !P->semi) || s[P->p] == ';') { P->p++; continue; }
            if (s[P->p] == ')') { P->p++; break; }
            P->fail = true;
            break;
        }
        return n;
    }
    if ((len == 4 && !strncasecmp(name, "TRUE", 4)) || (len == 9 && !strncasecmp(name, "VERDADERO", 9)) ||
        (len == 4 && !strncasecmp(name, "WAHR", 4))) {
        n->k = N_BOOL;
        n->n = 1;
        return n;
    }
    if ((len == 5 && !strncasecmp(name, "FALSE", 5)) || (len == 5 && !strncasecmp(name, "FALSO", 5)) ||
        (len == 6 && !strncasecmp(name, "FALSCH", 6))) {
        n->k = N_BOOL;
        n->n = 0;
        return n;
    }
    n->k = N_NAME;
    n->s = of_strndup(name, (size_t)len);
    P->unknown = true;
    return n;
}

static of_node_t *p_postfix(ps_t *P)
{
    of_node_t *a = p_primary(P);
    for (;;) {
        sp(P);
        if (a && P->s[P->p] == '%') {
            P->p++;
            of_node_t *n = node(N_PCT);
            if (!n) break;
            add_arg(n, a);
            a = n;
            continue;
        }
        break;
    }
    return a;
}

static of_node_t *p_unary(ps_t *P)
{
    sp(P);
    char c = P->s[P->p];
    if (c == '-' || c == '+') {
        P->p++;
        of_node_t *e = p_unary(P);
        of_node_t *n = node(c == '-' ? N_NEG : N_POS);
        if (!n) { of_formula_free(e); return NULL; }
        add_arg(n, e);
        return n;
    }
    return p_postfix(P);
}

static of_node_t *bin(int op, of_node_t *a, of_node_t *b)
{
    of_node_t *n = node(N_BIN);
    if (!n) { of_formula_free(a); of_formula_free(b); return NULL; }
    n->op = (uint8_t)op;
    add_arg(n, a);
    add_arg(n, b);
    return n;
}

static of_node_t *p_pow(ps_t *P)
{
    of_node_t *a = p_unary(P);
    for (;;) {
        sp(P);
        if (P->s[P->p] != '^' || P->fail) return a;
        P->p++;
        a = bin(OP_POW, a, p_unary(P));
    }
}

static of_node_t *p_mul(ps_t *P)
{
    of_node_t *a = p_pow(P);
    for (;;) {
        sp(P);
        char c = P->s[P->p];
        if ((c != '*' && c != '/') || P->fail) return a;
        P->p++;
        a = bin(c == '*' ? OP_MUL : OP_DIV, a, p_pow(P));
    }
}

static of_node_t *p_add(ps_t *P)
{
    of_node_t *a = p_mul(P);
    for (;;) {
        sp(P);
        char c = P->s[P->p];
        if ((c != '+' && c != '-') || P->fail) return a;
        P->p++;
        a = bin(c == '+' ? OP_ADD : OP_SUB, a, p_mul(P));
    }
}

static of_node_t *p_cat(ps_t *P)
{
    of_node_t *a = p_add(P);
    for (;;) {
        sp(P);
        if (P->s[P->p] != '&' || P->fail) return a;
        P->p++;
        a = bin(OP_CAT, a, p_add(P));
    }
}

static of_node_t *p_expr(ps_t *P)
{
    of_node_t *a = p_cat(P);
    for (;;) {
        sp(P);
        const char *s = P->s + P->p;
        int op = 0, l = 1;
        if (s[0] == '<' && s[1] == '>') { op = OP_NE; l = 2; }
        else if (s[0] == '<' && s[1] == '=') { op = OP_LE; l = 2; }
        else if (s[0] == '>' && s[1] == '=') { op = OP_GE; l = 2; }
        else if (s[0] == '=') op = OP_EQ;
        else if (s[0] == '<') op = OP_LT;
        else if (s[0] == '>') op = OP_GT;
        if (!op || P->fail) return a;
        P->p += l;
        a = bin(op, a, p_cat(P));
    }
}

of_node_t *of_formula_parse(const char *src, int sheet_ix, const of_book_t *b, bool *unknown_fn)
{
    ps_t P = { .s = src, .sheet_ix = sheet_ix, .b = b };
    /* a ';' outside strings: the European way of writing it */
    bool q = false;
    for (const char *c = src; *c; c++) {
        if (*c == '"') q = !q;
        else if (!q && *c == ';') { P.semi = true; break; }
    }
    of_node_t *n = p_expr(&P);
    sp(&P);
    if (P.fail || !n || P.s[P.p]) {
        of_formula_free(n);
        if (unknown_fn) *unknown_fn = P.unknown;
        return NULL;
    }
    if (unknown_fn) *unknown_fn = P.unknown;
    return n;
}

/* -------------------------------------------------------------------------- */
/* Writing it back                                                             */
/* -------------------------------------------------------------------------- */

static void w_sheet(const of_book_t *b, int sheet, of_sb_t *o, bool odf)
{
    if (sheet < 0 || !b || sheet >= b->nsh) return;
    const char *nm = b->sh[sheet].name ? b->sh[sheet].name : "";
    bool quote = false;
    for (const char *p = nm; *p; p++)
        if (!is_ident(*p) || *p == '.') quote = true;
    if (!*nm || (nm[0] >= '0' && nm[0] <= '9')) quote = true;
    if (odf) of_sb_putc(o, '$');
    if (quote) {
        of_sb_putc(o, '\'');
        for (const char *p = nm; *p; p++) {
            if (*p == '\'') of_sb_putc(o, '\'');
            of_sb_putc(o, *p);
        }
        of_sb_putc(o, '\'');
    } else {
        of_sb_puts(o, nm);
    }
    if (!odf) of_sb_putc(o, '!');
}

static void w_cell(of_sb_t *o, int32_t r, int32_t c, bool ac, bool ar)
{
    char col[8];
    of_col_name(c, col);
    of_sb_printf(o, "%s%s%s%ld", ac ? "$" : "", col, ar ? "$" : "", (long)r + 1);
}

static void w_node(const of_node_t *n, const of_book_t *b, of_sb_t *o, bool odf)
{
    static const char *const OPS[] = { "", "+", "-", "*", "/", "^", "&", "=", "<>", "<", ">", "<=", ">=" };
    if (!n) return;
    switch (n->k) {
    case N_NUM: of_sb_puts(o, n->s ? n->s : "0"); break;
    case N_STR:
        of_sb_putc(o, '"');
        for (const char *p = n->s ? n->s : ""; *p; p++) {
            if (*p == '"') of_sb_putc(o, '"');
            of_sb_putc(o, *p);
        }
        of_sb_putc(o, '"');
        break;
    case N_BOOL: of_sb_puts(o, n->n ? "TRUE" : "FALSE"); break;
    case N_ERR: of_sb_puts(o, n->s ? n->s : of_err_name((int)n->n)); break;
    case N_REF: case N_RANGE: {
        if (odf) of_sb_putc(o, '[');
        if (n->sheet >= 0) w_sheet(b, n->sheet, o, odf);
        if (odf) of_sb_putc(o, '.');
        if (n->abs & WHOLE_COL) {
            char c1[8], c2[8];
            of_col_name(n->c1, c1);
            of_col_name(n->c2, c2);
            of_sb_printf(o, "%s%s:%s%s%s", n->abs & AB_C1 ? "$" : "", c1, odf ? "." : "", n->abs & AB_C2 ? "$" : "", c2);
        } else if (n->abs & WHOLE_ROW) {
            of_sb_printf(o, "%s%ld:%s%s%ld", n->abs & AB_R1 ? "$" : "", (long)n->r1 + 1, odf ? "." : "",
                         n->abs & AB_R2 ? "$" : "", (long)n->r2 + 1);
        } else {
            w_cell(o, n->r1, n->c1, n->abs & AB_C1, n->abs & AB_R1);
            if (n->k == N_RANGE) {
                of_sb_puts(o, odf ? ":." : ":");
                w_cell(o, n->r2, n->c2, n->abs & AB_C2, n->abs & AB_R2);
            }
        }
        if (odf) of_sb_putc(o, ']');
        break;
    }
    case N_FN: {
        int count;
        const fndef_t *F = fn_table(&count);
        if (n->fn >= 0) {
            if ((n->xlfn || F[n->fn].xlfn) && !odf) of_sb_puts(o, "_xlfn.");
            of_sb_puts(o, F[n->fn].name);
        } else {
            const char *nm = n->s ? n->s : "";
            if (odf && !strncasecmp(nm, "_xlfn.", 6)) nm += 6;
            of_sb_puts(o, nm);
        }
        of_sb_putc(o, '(');
        for (int i = 0; i < n->nargs; i++) {
            if (i) of_sb_putc(o, odf ? ';' : ',');
            w_node(n->a[i], b, o, odf);
        }
        of_sb_putc(o, ')');
        break;
    }
    case N_BIN:
        w_node(n->a[0], b, o, odf);
        of_sb_puts(o, OPS[n->op]);
        w_node(n->a[1], b, o, odf);
        break;
    case N_NEG: of_sb_putc(o, '-'); w_node(n->a[0], b, o, odf); break;
    case N_POS: of_sb_putc(o, '+'); w_node(n->a[0], b, o, odf); break;
    case N_PCT: w_node(n->a[0], b, o, odf); of_sb_putc(o, '%'); break;
    case N_PAREN: of_sb_putc(o, '('); w_node(n->a[0], b, o, odf); of_sb_putc(o, ')'); break;
    case N_NAME: of_sb_puts(o, n->s ? n->s : ""); break;
    }
}

int of_formula_result_hint(const of_node_t *n)
{
    while (n && n->k == N_PAREN) n = n->a[0];
    if (!n || n->k != N_FN || n->fn < 0) return 0;
    int count;
    const fndef_t *F = fn_table(&count);
    const char *nm = F[n->fn].name;
    if (!strcmp(nm, "DATE") || !strcmp(nm, "TODAY")) return OH_DATE;
    if (!strcmp(nm, "NOW")) return OH_DATETIME;
    return 0;
}

void of_formula_write(const of_node_t *n, const of_book_t *b, of_sb_t *out)
{
    w_node(n, b, out, false);
}

static void shift(of_node_t *n, int dr, int dc)
{
    if (!n) return;
    if (n->k == N_REF || n->k == N_RANGE) {
        bool bad = false;
        if (!(n->abs & WHOLE_ROW)) {
            if (!(n->abs & AB_C1)) n->c1 += dc;
            if (!(n->abs & AB_C2)) n->c2 += dc;
            if (n->c1 < 0 || n->c2 > 16383) bad = true;
        }
        if (!(n->abs & WHOLE_COL)) {
            if (!(n->abs & AB_R1)) n->r1 += dr;
            if (!(n->abs & AB_R2)) n->r2 += dr;
            if (n->r1 < 0 || n->r2 > 1048575) bad = true;
        }
        if (bad) {
            n->k = N_ERR;
            n->n = OE_REF;
            of_free(n->s);
            n->s = of_strdup("#REF!");
        }
    }
    for (int i = 0; i < n->nargs; i++) shift(n->a[i], dr, dc);
}

char *of_formula_shift(const char *src, const of_book_t *b, int sheet_ix, int dr, int dc)
{
    bool unk;
    of_node_t *n = of_formula_parse(src, sheet_ix, b, &unk);
    if (!n) return NULL;
    shift(n, dr, dc);
    of_sb_t sb = { 0 };
    w_node(n, b, &sb, false);
    of_formula_free(n);
    if (sb.oom) { of_sb_free(&sb); return NULL; }
    return of_sb_take(&sb);
}

/* ODF: "of:=SUM([.A1:.B2];[Sheet2.C3])" */
char *of_odf_to_excel(const char *src)
{
    const char *s = src;
    if (!strncmp(s, "of:", 3)) s += 3;
    else if (!strncmp(s, "oooc:", 5)) s += 5;
    else if (!strncmp(s, "msoxl:", 6)) s += 6;
    if (*s == '=') s++;
    of_sb_t o = { 0 };
    while (*s) {
        if (*s == '"') {                /* strings as they are */
            of_sb_putc(&o, *s++);
            while (*s) {
                if (*s == '"') {
                    if (s[1] == '"') { of_sb_put(&o, s, 2); s += 2; continue; }
                    break;
                }
                of_sb_putc(&o, *s++);
            }
            if (*s) of_sb_putc(&o, *s++);
            continue;
        }
        if (*s == '[') {
            const char *e = strchr(s, ']');
            if (!e) break;
            /* [$'My sheet'.A1:.B2] -> 'My sheet'!A1:B2 */
            const char *p = s + 1;
            bool first = true;
            while (p < e) {
                if (*p == ':') { of_sb_putc(&o, ':'); p++; first = false; continue; }
                if (*p == '$' && (p[1] == '\'' || (p[1] != '.' && !(p[1] >= 'A' && p[1] <= 'Z' && strchr(p, '.') > e)))) {
                    /* a sheet's absolute mark, if a sheet name follows */
                    const char *dot = NULL;
                    for (const char *q = p + 1; q < e; q++) {
                        if (*q == '\'') { q++; while (q < e && *q != '\'') q++; continue; }
                        if (*q == '.') { dot = q; break; }
                        if (*q == ':') break;
                    }
                    if (dot && dot != p + 1) p++;
                }
                const char *dot = NULL;
                for (const char *q = p; q < e; q++) {
                    if (*q == '\'') { q++; while (q < e && *q != '\'') q++; continue; }
                    if (*q == '.') { dot = q; break; }
                    if (*q == ':') break;
                }
                if (dot) {
                    if (dot > p && first) {
                        of_sb_put(&o, p, (size_t)(dot - p));
                        of_sb_putc(&o, '!');
                    }
                    p = dot + 1;
                }
                while (p < e && *p != ':') of_sb_putc(&o, *p++);
            }
            s = e + 1;
            continue;
        }
        if (*s == ';') { of_sb_putc(&o, ','); s++; continue; }
        of_sb_putc(&o, *s++);
    }
    return of_sb_take(&o);
}

char *of_excel_to_odf(const char *src)
{
    bool unk;
    of_node_t *n = of_formula_parse(src, 0, NULL, &unk);
    if (!n) return NULL;
    of_sb_t sb = { 0 };
    of_sb_puts(&sb, "of:=");
    w_node(n, NULL, &sb, true);
    of_formula_free(n);
    return of_sb_take(&sb);
}

/* The ODF form needs the sheet names: this is the one the writers use. */
char *of_excel_to_odf_book(const char *src, const of_book_t *b, int sheet_ix)
{
    bool unk;
    of_node_t *n = of_formula_parse(src, sheet_ix, b, &unk);
    if (!n) return NULL;
    of_sb_t sb = { 0 };
    of_sb_puts(&sb, "of:=");
    w_node(n, b, &sb, true);
    of_formula_free(n);
    return of_sb_take(&sb);
}

/* -------------------------------------------------------------------------- */
/* Numbers in double without libm                                              */
/* -------------------------------------------------------------------------- */

static double d_trunc(double x)
{
    if (x >= 9.2e18 || x <= -9.2e18) return x;
    return (double)(long long)x;
}

static double d_floor(double x)
{
    double t = d_trunc(x);
    return t > x ? t - 1 : t;
}

static double d_pow10(int e)
{
    double r = 1, b = 10;
    bool neg = e < 0;
    if (neg) e = -e;
    while (e) {
        if (e & 1) r *= b;
        b *= b;
        e >>= 1;
    }
    return neg ? 1 / r : r;
}

/* Rounds half away from zero at 'digits' decimals, with the nudge Excel
 * gives so that 1.005 rounds to 1.01 (it is 1.00499999... in binary). */
static double d_round(double x, int digits, int mode)   /* 0 round, 1 up, -1 down */
{
    double m = d_pow10(digits);
    double y = x * m;
    double ay = y < 0 ? -y : y;
    double t = d_trunc(ay);
    double frac = ay - t;
    double r;
    if (mode == 0) r = frac >= 0.5 - 1e-9 * (ay > 1 ? 1 : ay) ? t + 1 : t;
    else if (mode > 0) r = frac > 1e-12 ? t + 1 : t;
    else r = t;
    if (y < 0) r = -r;
    return r / m;
}

static double d_sqrt(double x)
{
    if (x <= 0) return 0;
    double g = sqrtf((float)x);
    for (int i = 0; i < 4; i++) g = 0.5 * (g + x / g);
    return g;
}

static double d_mod(double a, double b)
{
    return a - b * d_floor(a / b);
}

static double d_pow(double a, double b, bool *bad)
{
    *bad = false;
    double ib = d_trunc(b);
    if (ib == b && ib > -1e6 && ib < 1e6) {
        long long e = (long long)ib;
        bool neg = e < 0;
        if (neg) e = -e;
        double r = 1, p = a;
        while (e) {
            if (e & 1) r *= p;
            p *= p;
            e >>= 1;
        }
        if (neg) {
            if (r == 0) { *bad = true; return 0; }
            r = 1 / r;
        }
        return r;
    }
    if (a < 0) { *bad = true; return 0; }
    if (a == 0) return 0;
    return (double)powf((float)a, (float)b);
}

/* -------------------------------------------------------------------------- */
/* Evaluation                                                                  */
/* -------------------------------------------------------------------------- */

struct ctx {
    of_book_t *b;
    int        sheet;           /* the formula's */
    int32_t    row, col;
    of_cell_t *need;            /* a cell not computed yet: try again later */
};

static void set_err(of_val_t *v, int e)
{
    of_val_free(v);
    v->t = OV_ERR;
    v->err = (uint8_t)e;
}

static void set_num(of_val_t *v, double n)
{
    of_val_free(v);
    v->t = OV_NUM;
    v->n = n;
}

static void set_bool(of_val_t *v, bool b)
{
    of_val_free(v);
    v->t = OV_BOOL;
    v->n = b ? 1 : 0;
}

static void set_str(of_val_t *v, char *owned)
{
    of_val_free(v);
    v->t = OV_STR;
    v->s = owned ? owned : of_strdup("");
}

/* The value of a cell, or empty while it waits for its turn. */
static const of_val_t *cell_val(ctx_t *x, int sheet, int32_t r, int32_t c)
{
    static const of_val_t EMPTY = { 0 };
    static const of_val_t CIRC = { .t = OV_ERR, .err = OE_CIRC };
    if (sheet < 0) sheet = x->sheet;
    if (sheet >= x->b->nsh) return &EMPTY;
    of_cell_t *cell = of_cell_get(&x->b->sh[sheet], r, c);
    if (!cell) return &EMPTY;
    if (cell->f && cell->state != 2) {
        if (cell->state == 1) return &CIRC;
        if (!x->need) x->need = cell;
        return &EMPTY;
    }
    return &cell->v;
}

static void eval(ctx_t *x, of_node_t *n, of_val_t *out);

/* For every value of an argument: the cells of a range (only those that have
 * something, unless with_empty) or the one value of an expression. */
typedef bool (*each_cb)(const of_val_t *v, bool from_ref, void *user);

static bool each(ctx_t *x, of_node_t *n, each_cb cb, void *user, bool with_empty)
{
    if (n->k == N_REF || n->k == N_RANGE) {
        int sh = n->sheet < 0 ? x->sheet : n->sheet;
        if (sh >= x->b->nsh) return true;
        of_sheet_t *s = &x->b->sh[sh];
        if (with_empty) {
            int32_t mr, mc;
            of_sheet_extent(s, &mr, &mc);
            int32_t r2 = n->r2 > mr ? mr : n->r2, c2 = n->c2 > mc ? mc : n->c2;
            static const of_val_t EMPTY = { 0 };
            for (int32_t r = n->r1; r <= n->r2 && r <= 1048575; r++) {
                for (int32_t c = n->c1; c <= n->c2; c++) {
                    if (r > r2 || c > c2) {
                        /* past the used area: empties, counted without looking */
                        if (!cb(&EMPTY, true, user)) return false;
                        if (r > r2 && c == n->c2 && (n->abs & WHOLE_COL)) return true;
                        continue;
                    }
                    if (!cb(cell_val(x, sh, r, c), true, user)) return false;
                    if (x->need) return false;
                }
                if (r > r2 && (n->abs & WHOLE_COL)) return true;
            }
            return true;
        }
        for (int i = 0; i < s->nrows; i++) {
            of_row_t *row = &s->rows[i];
            if (row->row < n->r1) continue;
            if (row->row > n->r2) break;
            for (int k = 0; k < row->n; k++) {
                of_cell_t *c = &row->c[k];
                if (c->col < n->c1) continue;
                if (c->col > n->c2) break;
                const of_val_t *v = cell_val(x, sh, row->row, c->col);
                if (x->need) return false;
                if (v->t == OV_EMPTY) continue;
                if (!cb(v, true, user)) return false;
            }
        }
        return true;
    }
    of_val_t v = { 0 };
    eval(x, n, &v);
    bool r = cb(&v, false, user);
    of_val_free(&v);
    return r;
}

/* Text to number, for arithmetic on "12". */
static bool str_num(const char *s, double *out)
{
    if (!s) return false;
    while (*s == ' ') s++;
    if (!*s) return false;
    char *end;
    double v = strtod(s, &end);
    while (*end == ' ') end++;
    if (*end == '%' && !end[1]) { *out = v / 100; return true; }
    if (*end) return false;
    *out = v;
    return true;
}

static bool to_num(const of_val_t *v, double *out, int *err)
{
    switch (v->t) {
    case OV_EMPTY: *out = 0; return true;
    case OV_NUM: case OV_BOOL: *out = v->n; return true;
    case OV_STR:
        if (str_num(v->s, out)) return true;
        *err = OE_VALUE;
        return false;
    default: *err = v->err; return false;
    }
}

static void to_text(const of_val_t *v, of_sb_t *o)
{
    char buf[64];
    switch (v->t) {
    case OV_NUM: of_fmt_general(v->n, buf, sizeof buf); of_sb_puts(o, buf); break;
    case OV_STR: of_sb_puts(o, v->s ? v->s : ""); break;
    case OV_BOOL: of_sb_puts(o, v->n ? "TRUE" : "FALSE"); break;
    default: break;
    }
}

static bool truthy(const of_val_t *v, int *err)
{
    double d;
    if (v->t == OV_STR) {
        if (!strcasecmp(v->s ? v->s : "", "TRUE")) return true;
        if (!strcasecmp(v->s ? v->s : "", "FALSE")) return false;
        *err = OE_VALUE;
        return false;
    }
    if (!to_num(v, &d, err)) return false;
    return d != 0;
}

/* A single value from a reference (the top-left cell of a range of one). */
static void eval_scalar(ctx_t *x, of_node_t *n, of_val_t *out)
{
    eval(x, n, out);
}

static int cmp_vals(const of_val_t *a, const of_val_t *b)
{
    /* Excel: numbers < text < booleans; empty is 0 or "" */
    of_val_t ea = *a, eb = *b;
    if (ea.t == OV_EMPTY) { ea.t = eb.t == OV_STR ? OV_STR : OV_NUM; ea.s = ""; ea.n = 0; }
    if (eb.t == OV_EMPTY) { eb.t = ea.t == OV_STR ? OV_STR : OV_NUM; eb.s = ""; eb.n = 0; }
    int ra = ea.t == OV_NUM ? 0 : ea.t == OV_STR ? 1 : 2;
    int rb = eb.t == OV_NUM ? 0 : eb.t == OV_STR ? 1 : 2;
    if (ra != rb) return ra - rb;
    if (ra == 1) return strcasecmp(ea.s ? ea.s : "", eb.s ? eb.s : "");
    return ea.n < eb.n ? -1 : ea.n > eb.n ? 1 : 0;
}

static void eval(ctx_t *x, of_node_t *n, of_val_t *out)
{
    of_val_free(out);
    if (!n || x->need) return;
    switch (n->k) {
    case N_NUM: set_num(out, n->n); return;
    case N_STR: set_str(out, of_strdup(n->s)); return;
    case N_BOOL: set_bool(out, n->n != 0); return;
    case N_ERR: set_err(out, (int)n->n); return;
    case N_NAME: set_err(out, OE_NAME); return;
    case N_PAREN: eval(x, n->a[0], out); return;
    case N_REF: {
        const of_val_t *v = cell_val(x, n->sheet, n->r1, n->c1);
        of_val_copy(out, v);
        return;
    }
    case N_RANGE: {
        /* a range where one value is wanted: the cell on the formula's row
         * or column (implicit intersection), else #VALUE! */
        if (n->c1 == n->c2 && x->row >= n->r1 && x->row <= n->r2) {
            of_val_copy(out, cell_val(x, n->sheet, x->row, n->c1));
        } else if (n->r1 == n->r2 && x->col >= n->c1 && x->col <= n->c2) {
            of_val_copy(out, cell_val(x, n->sheet, n->r1, x->col));
        } else if (n->r1 == n->r2 && n->c1 == n->c2) {
            of_val_copy(out, cell_val(x, n->sheet, n->r1, n->c1));
        } else {
            set_err(out, OE_VALUE);
        }
        return;
    }
    case N_NEG: case N_POS: case N_PCT: {
        of_val_t a = { 0 };
        eval(x, n->a[0], &a);
        double d;
        int e = 0;
        if (!to_num(&a, &d, &e)) set_err(out, e);
        else set_num(out, n->k == N_NEG ? -d : n->k == N_PCT ? d / 100 : d);
        of_val_free(&a);
        return;
    }
    case N_BIN: {
        of_val_t a = { 0 }, b = { 0 };
        eval(x, n->a[0], &a);
        eval(x, n->a[1], &b);
        if (x->need) { of_val_free(&a); of_val_free(&b); return; }
        if (n->op == OP_CAT) {
            if (a.t == OV_ERR) set_err(out, a.err);
            else if (b.t == OV_ERR) set_err(out, b.err);
            else {
                of_sb_t sb = { 0 };
                to_text(&a, &sb);
                to_text(&b, &sb);
                set_str(out, of_sb_take(&sb));
            }
        } else if (n->op >= OP_EQ) {
            if (a.t == OV_ERR) set_err(out, a.err);
            else if (b.t == OV_ERR) set_err(out, b.err);
            else {
                int c = cmp_vals(&a, &b);
                bool r = n->op == OP_EQ ? c == 0 : n->op == OP_NE ? c != 0 : n->op == OP_LT ? c < 0
                       : n->op == OP_GT ? c > 0 : n->op == OP_LE ? c <= 0 : c >= 0;
                set_bool(out, r);
            }
        } else {
            double da, db;
            int e = 0;
            if (!to_num(&a, &da, &e) || !to_num(&b, &db, &e)) set_err(out, e);
            else {
                double r = 0;
                bool bad = false;
                switch (n->op) {
                case OP_ADD: r = da + db; break;
                case OP_SUB: r = da - db; break;
                case OP_MUL: r = da * db; break;
                case OP_DIV: if (db == 0) bad = true; else r = da / db; break;
                case OP_POW: r = d_pow(da, db, &bad); break;
                }
                if (bad) set_err(out, n->op == OP_DIV ? OE_DIV0 : OE_NUM);
                else set_num(out, r);
            }
        }
        of_val_free(&a);
        of_val_free(&b);
        return;
    }
    case N_FN: {
        int count;
        const fndef_t *F = fn_table(&count);
        if (n->fn < 0) { set_err(out, OE_NAME); return; }
        const fndef_t *f = &F[n->fn];
        if (n->nargs < f->min || (f->max >= 0 && n->nargs > f->max)) { set_err(out, OE_VALUE); return; }
        f->fn(x, n, out);
        return;
    }
    }
}

/* Arguments as numbers: evaluated, or an error to return. */
static bool arg_num(ctx_t *x, of_node_t *n, int i, double *d, of_val_t *out)
{
    of_val_t v = { 0 };
    eval_scalar(x, n->a[i], &v);
    int e = 0;
    bool ok = to_num(&v, d, &e);
    of_val_free(&v);
    if (!ok) set_err(out, e);
    return ok && !x->need;
}

static char *arg_str(ctx_t *x, of_node_t *n, int i, of_val_t *out)
{
    of_val_t v = { 0 };
    eval_scalar(x, n->a[i], &v);
    if (v.t == OV_ERR) {
        set_err(out, v.err);
        of_val_free(&v);
        return NULL;
    }
    of_sb_t sb = { 0 };
    to_text(&v, &sb);
    of_val_free(&v);
    return of_sb_take(&sb);
}

/* --- aggregates ----------------------------------------------------------- */

typedef struct {
    double sum, prod, min, max;
    long   count, counta, blank;
    int    err;
    bool   any;
    double *vals;
    long   nvals, cap;
    bool   collect;
} agg_t;

static bool agg_cb(const of_val_t *v, bool from_ref, void *u)
{
    agg_t *a = u;
    if (v->t == OV_ERR) { if (!a->err) a->err = v->err; return false; }
    if (v->t == OV_EMPTY) { a->blank++; return true; }
    a->counta++;
    double d;
    if (v->t == OV_NUM) d = v->n;
    else if (!from_ref && v->t == OV_BOOL) d = v->n;
    else if (!from_ref && v->t == OV_STR && str_num(v->s, &d)) {}
    else if (!from_ref && v->t == OV_STR) { if (!a->err) a->err = OE_VALUE; return false; }
    else return true;               /* text and booleans in ranges are skipped */
    a->sum += d;
    a->prod *= d;
    if (!a->any || d < a->min) a->min = d;
    if (!a->any || d > a->max) a->max = d;
    a->any = true;
    a->count++;
    if (a->collect) {
        if (a->nvals == a->cap) {
            long cap = a->cap ? a->cap * 2 : 64;
            double *nv = of_realloc(a->vals, (size_t)cap * sizeof *nv);
            if (!nv) return false;
            a->vals = nv;
            a->cap = cap;
        }
        a->vals[a->nvals++] = d;
    }
    return true;
}

static bool aggregate(ctx_t *x, of_node_t *n, agg_t *a, of_val_t *out, bool counting)
{
    memset(a, 0, sizeof *a);
    a->prod = 1;
    for (int i = 0; i < n->nargs; i++) {
        each(x, n->a[i], agg_cb, a, false);
        if (x->need) return false;
        if (a->err && !counting) {
            set_err(out, a->err);
            of_free(a->vals);
            return false;
        }
        a->err = 0;
    }
    return true;
}

static void f_sum(ctx_t *x, of_node_t *n, of_val_t *o) { agg_t a; if (aggregate(x, n, &a, o, false)) set_num(o, a.sum); }
static void f_product(ctx_t *x, of_node_t *n, of_val_t *o) { agg_t a; if (aggregate(x, n, &a, o, false)) set_num(o, a.any ? a.prod : 0); }
static void f_min(ctx_t *x, of_node_t *n, of_val_t *o) { agg_t a; if (aggregate(x, n, &a, o, false)) set_num(o, a.any ? a.min : 0); }
static void f_max(ctx_t *x, of_node_t *n, of_val_t *o) { agg_t a; if (aggregate(x, n, &a, o, false)) set_num(o, a.any ? a.max : 0); }
static void f_count(ctx_t *x, of_node_t *n, of_val_t *o) { agg_t a; if (aggregate(x, n, &a, o, true)) set_num(o, (double)a.count); }
static void f_average(ctx_t *x, of_node_t *n, of_val_t *o)
{
    agg_t a;
    if (!aggregate(x, n, &a, o, false)) return;
    if (!a.count) set_err(o, OE_DIV0);
    else set_num(o, a.sum / (double)a.count);
}

static bool counta_cb(const of_val_t *v, bool from_ref, void *u)
{
    (void)from_ref;
    if (v->t != OV_EMPTY) (*(long *)u)++;
    return true;
}

static void f_counta(ctx_t *x, of_node_t *n, of_val_t *o)
{
    long cnt = 0;
    for (int i = 0; i < n->nargs; i++) each(x, n->a[i], counta_cb, &cnt, false);
    if (!x->need) set_num(o, (double)cnt);
}

static bool blank_cb(const of_val_t *v, bool from_ref, void *u)
{
    (void)from_ref;
    if (v->t == OV_EMPTY || (v->t == OV_STR && (!v->s || !*v->s))) (*(long *)u)++;
    return true;
}

static void f_countblank(ctx_t *x, of_node_t *n, of_val_t *o)
{
    long cnt = 0;
    each(x, n->a[0], blank_cb, &cnt, true);
    if (!x->need) set_num(o, (double)cnt);
}

static int dcmp(const void *a, const void *b)
{
    double da = *(const double *)a, db = *(const double *)b;
    return da < db ? -1 : da > db ? 1 : 0;
}

static void f_median(ctx_t *x, of_node_t *n, of_val_t *o)
{
    agg_t a;
    memset(&a, 0, sizeof a);
    a.prod = 1;
    a.collect = true;
    for (int i = 0; i < n->nargs && !x->need; i++) {
        each(x, n->a[i], agg_cb, &a, false);
        if (a.err) { set_err(o, a.err); of_free(a.vals); return; }
    }
    if (x->need) { of_free(a.vals); return; }
    if (!a.nvals) { set_err(o, OE_NUM); return; }
    qsort(a.vals, (size_t)a.nvals, sizeof(double), dcmp);
    double m = a.nvals % 2 ? a.vals[a.nvals / 2] : (a.vals[a.nvals / 2 - 1] + a.vals[a.nvals / 2]) / 2;
    of_free(a.vals);
    set_num(o, m);
}

/* --- criteria: SUMIF, COUNTIF, AVERAGEIF ------------------------------------ */

typedef struct {
    int     op;                 /* OP_EQ ... */
    bool    num;
    double  n;
    char   *s;                  /* lower case, may hold * and ? */
} crit_t;

static void crit_make(const of_val_t *v, crit_t *c)
{
    memset(c, 0, sizeof *c);
    c->op = OP_EQ;
    if (v->t == OV_NUM || v->t == OV_BOOL) { c->num = true; c->n = v->n; return; }
    const char *s = v->t == OV_STR && v->s ? v->s : "";
    if (!strncmp(s, "<>", 2)) { c->op = OP_NE; s += 2; }
    else if (!strncmp(s, "<=", 2)) { c->op = OP_LE; s += 2; }
    else if (!strncmp(s, ">=", 2)) { c->op = OP_GE; s += 2; }
    else if (*s == '<') { c->op = OP_LT; s++; }
    else if (*s == '>') { c->op = OP_GT; s++; }
    else if (*s == '=') { s++; }
    double d;
    if (str_num(s, &d)) { c->num = true; c->n = d; return; }
    c->s = of_strdup(s);
}

static bool wild(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return true;
            for (; *s; s++)
                if (wild(p + 1, s)) return true;
            return false;
        }
        if (!*s) return false;
        if (*p == '~' && (p[1] == '*' || p[1] == '?')) p++;
        else if (*p == '?') continue;
        char a = *p, b = *s;
        if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
        if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
        if (a != b) return false;
    }
    return !*s;
}

static bool crit_match(const crit_t *c, const of_val_t *v)
{
    if (c->num) {
        double d;
        if (v->t == OV_NUM) d = v->n;
        else if (v->t == OV_EMPTY && c->op == OP_NE) return true;
        else return c->op == OP_NE;
        switch (c->op) {
        case OP_EQ: return d == c->n;
        case OP_NE: return d != c->n;
        case OP_LT: return d < c->n;
        case OP_GT: return d > c->n;
        case OP_LE: return d <= c->n;
        default: return d >= c->n;
        }
    }
    const char *s = v->t == OV_STR && v->s ? v->s : "";
    if (v->t == OV_NUM || v->t == OV_BOOL) return c->op == OP_NE;
    if (c->op == OP_EQ) return wild(c->s, s);
    if (c->op == OP_NE) return !wild(c->s, s);
    int r = strcasecmp(s, c->s);
    return c->op == OP_LT ? r < 0 : c->op == OP_GT ? r > 0 : c->op == OP_LE ? r <= 0 : r >= 0;
}

static void f_ifs(ctx_t *x, of_node_t *n, of_val_t *o, int kind)   /* 0 sum, 1 count, 2 average */
{
    of_node_t *range = n->a[0];
    of_node_t *sum = n->nargs > 2 ? n->a[2] : range;
    if ((range->k != N_REF && range->k != N_RANGE) || (sum->k != N_REF && sum->k != N_RANGE)) {
        set_err(o, OE_VALUE);
        return;
    }
    of_val_t cv = { 0 };
    eval_scalar(x, n->a[1], &cv);
    if (x->need) { of_val_free(&cv); return; }
    crit_t c;
    crit_make(&cv, &c);
    of_val_free(&cv);
    double total = 0;
    long cnt = 0;
    int sh = range->sheet < 0 ? x->sheet : range->sheet;
    int32_t mr, mc;
    of_sheet_extent(&x->b->sh[sh < x->b->nsh ? sh : 0], &mr, &mc);
    int32_t r2 = range->r2 > mr ? mr : range->r2, c2 = range->c2 > mc ? mc : range->c2;
    for (int32_t r = range->r1; r <= r2 && !x->need; r++) {
        for (int32_t cc = range->c1; cc <= c2; cc++) {
            const of_val_t *v = cell_val(x, sh, r, cc);
            if (x->need) break;
            if (!crit_match(&c, v)) continue;
            cnt++;
            if (kind != 1) {
                const of_val_t *s = cell_val(x, sum->sheet, sum->r1 + (r - range->r1), sum->c1 + (cc - range->c1));
                if (s->t == OV_NUM) total += s->n;
                else if (kind == 2 && s->t != OV_NUM) cnt--;
            }
        }
    }
    of_free(c.s);
    if (x->need) return;
    if (kind == 0) set_num(o, total);
    else if (kind == 1) set_num(o, (double)cnt);
    else if (!cnt) set_err(o, OE_DIV0);
    else set_num(o, total / (double)cnt);
}

static void f_sumif(ctx_t *x, of_node_t *n, of_val_t *o) { f_ifs(x, n, o, 0); }
static void f_countif(ctx_t *x, of_node_t *n, of_val_t *o) { f_ifs(x, n, o, 1); }
static void f_averageif(ctx_t *x, of_node_t *n, of_val_t *o) { f_ifs(x, n, o, 2); }

static void f_sumproduct(ctx_t *x, of_node_t *n, of_val_t *o)
{
    for (int i = 0; i < n->nargs; i++)
        if (n->a[i]->k != N_REF && n->a[i]->k != N_RANGE) { set_err(o, OE_VALUE); return; }
    of_node_t *a0 = n->a[0];
    int32_t h = a0->r2 - a0->r1 + 1, w = a0->c2 - a0->c1 + 1;
    for (int i = 1; i < n->nargs; i++)
        if (n->a[i]->r2 - n->a[i]->r1 + 1 != h || n->a[i]->c2 - n->a[i]->c1 + 1 != w) { set_err(o, OE_VALUE); return; }
    if ((double)h * w > 4e6) { set_err(o, OE_NUM); return; }
    double total = 0;
    for (int32_t r = 0; r < h; r++)
        for (int32_t c = 0; c < w; c++) {
            double p = 1;
            for (int i = 0; i < n->nargs; i++) {
                const of_val_t *v = cell_val(x, n->a[i]->sheet, n->a[i]->r1 + r, n->a[i]->c1 + c);
                if (x->need) return;
                if (v->t == OV_ERR) { set_err(o, v->err); return; }
                p *= v->t == OV_NUM ? v->n : 0;
            }
            total += p;
        }
    set_num(o, total);
}

/* --- maths ---------------------------------------------------------------- */

static void f_round_k(ctx_t *x, of_node_t *n, of_val_t *o, int mode)
{
    double v, d = 0;
    if (!arg_num(x, n, 0, &v, o)) return;
    if (n->nargs > 1 && !arg_num(x, n, 1, &d, o)) return;
    int digits = (int)d_trunc(d);
    if (digits > 15) digits = 15;
    if (digits < -15) digits = -15;
    set_num(o, d_round(v, digits, mode));
}
static void f_round(ctx_t *x, of_node_t *n, of_val_t *o) { f_round_k(x, n, o, 0); }
static void f_roundup(ctx_t *x, of_node_t *n, of_val_t *o) { f_round_k(x, n, o, 1); }
static void f_rounddown(ctx_t *x, of_node_t *n, of_val_t *o) { f_round_k(x, n, o, -1); }
static void f_trunc(ctx_t *x, of_node_t *n, of_val_t *o) { f_round_k(x, n, o, -1); }
static void f_int(ctx_t *x, of_node_t *n, of_val_t *o) { double v; if (arg_num(x, n, 0, &v, o)) set_num(o, d_floor(v)); }
static void f_abs(ctx_t *x, of_node_t *n, of_val_t *o) { double v; if (arg_num(x, n, 0, &v, o)) set_num(o, v < 0 ? -v : v); }
static void f_sign(ctx_t *x, of_node_t *n, of_val_t *o) { double v; if (arg_num(x, n, 0, &v, o)) set_num(o, v > 0 ? 1 : v < 0 ? -1 : 0); }
static void f_pi(ctx_t *x, of_node_t *n, of_val_t *o) { (void)x; (void)n; set_num(o, 3.14159265358979323846); }
static void f_sqrt(ctx_t *x, of_node_t *n, of_val_t *o)
{
    double v;
    if (!arg_num(x, n, 0, &v, o)) return;
    if (v < 0) set_err(o, OE_NUM);
    else set_num(o, d_sqrt(v));
}
static void f_mod(ctx_t *x, of_node_t *n, of_val_t *o)
{
    double a, b;
    if (!arg_num(x, n, 0, &a, o) || !arg_num(x, n, 1, &b, o)) return;
    if (b == 0) set_err(o, OE_DIV0);
    else set_num(o, d_mod(a, b));
}
static void f_power(ctx_t *x, of_node_t *n, of_val_t *o)
{
    double a, b;
    bool bad;
    if (!arg_num(x, n, 0, &a, o) || !arg_num(x, n, 1, &b, o)) return;
    double r = d_pow(a, b, &bad);
    if (bad) set_err(o, OE_NUM);
    else set_num(o, r);
}

/* --- logic ---------------------------------------------------------------- */

static void f_if(ctx_t *x, of_node_t *n, of_val_t *o)
{
    of_val_t c = { 0 };
    eval_scalar(x, n->a[0], &c);
    if (x->need) { of_val_free(&c); return; }
    if (c.t == OV_ERR) { set_err(o, c.err); of_val_free(&c); return; }
    int e = 0;
    bool t = truthy(&c, &e);
    of_val_free(&c);
    if (e) { set_err(o, e); return; }
    if (t) {
        if (n->nargs > 1 && n->a[1]->k != N_NAME) eval(x, n->a[1], o);
        else if (n->nargs > 1) set_num(o, 0);
        else set_bool(o, true);
    } else {
        if (n->nargs > 2 && n->a[2]->k != N_NAME) eval(x, n->a[2], o);
        else if (n->nargs > 2) set_num(o, 0);
        else set_bool(o, false);
    }
    if (o->t == OV_EMPTY && !x->need) set_num(o, 0);
}

typedef struct { int mode; bool acc; bool any; int err; } logic_t;    /* mode 0 AND, 1 OR */

static bool logic_cb(const of_val_t *v, bool from_ref, void *u)
{
    logic_t *l = u;
    if (v->t == OV_ERR) { l->err = v->err; return false; }
    if (v->t == OV_EMPTY || (from_ref && v->t == OV_STR)) return true;
    int e = 0;
    bool t = truthy(v, &e);
    if (e) { l->err = e; return false; }
    l->any = true;
    if (l->mode == 0) l->acc = l->acc && t;
    else l->acc = l->acc || t;
    return true;
}

static void f_logic(ctx_t *x, of_node_t *n, of_val_t *o, int mode)
{
    logic_t l = { .mode = mode, .acc = mode == 0 };
    for (int i = 0; i < n->nargs && !l.err; i++) each(x, n->a[i], logic_cb, &l, false);
    if (x->need) return;
    if (l.err) set_err(o, l.err);
    else if (!l.any) set_err(o, OE_VALUE);
    else set_bool(o, l.acc);
}
static void f_and(ctx_t *x, of_node_t *n, of_val_t *o) { f_logic(x, n, o, 0); }
static void f_or(ctx_t *x, of_node_t *n, of_val_t *o) { f_logic(x, n, o, 1); }
static void f_not(ctx_t *x, of_node_t *n, of_val_t *o)
{
    of_val_t c = { 0 };
    eval_scalar(x, n->a[0], &c);
    int e = 0;
    bool t = c.t == OV_ERR ? (e = c.err, false) : truthy(&c, &e);
    of_val_free(&c);
    if (x->need) return;
    if (e) set_err(o, e);
    else set_bool(o, !t);
}
static void f_true(ctx_t *x, of_node_t *n, of_val_t *o) { (void)x; (void)n; set_bool(o, true); }
static void f_false(ctx_t *x, of_node_t *n, of_val_t *o) { (void)x; (void)n; set_bool(o, false); }

static void f_iferror(ctx_t *x, of_node_t *n, of_val_t *o)
{
    eval(x, n->a[0], o);
    if (x->need) return;
    if (o->t == OV_ERR) eval(x, n->a[1], o);
    if (o->t == OV_EMPTY && !x->need) set_num(o, 0);
}

static void f_ifna(ctx_t *x, of_node_t *n, of_val_t *o)
{
    eval(x, n->a[0], o);
    if (x->need) return;
    if (o->t == OV_ERR && o->err == OE_NA) eval(x, n->a[1], o);
}

static void f_is(ctx_t *x, of_node_t *n, of_val_t *o, int what)
{
    of_val_t v = { 0 };
    eval_scalar(x, n->a[0], &v);
    if (x->need) { of_val_free(&v); return; }
    bool r = what == 0 ? v.t == OV_EMPTY : what == 1 ? v.t == OV_NUM : what == 2 ? v.t == OV_STR
           : what == 3 ? v.t == OV_ERR : v.t == OV_ERR && v.err == OE_NA;
    of_val_free(&v);
    set_bool(o, r);
}
static void f_isblank(ctx_t *x, of_node_t *n, of_val_t *o) { f_is(x, n, o, 0); }
static void f_isnumber(ctx_t *x, of_node_t *n, of_val_t *o) { f_is(x, n, o, 1); }
static void f_istext(ctx_t *x, of_node_t *n, of_val_t *o) { f_is(x, n, o, 2); }
static void f_iserror(ctx_t *x, of_node_t *n, of_val_t *o) { f_is(x, n, o, 3); }
static void f_isna(ctx_t *x, of_node_t *n, of_val_t *o) { f_is(x, n, o, 4); }

/* --- text ----------------------------------------------------------------- */

static bool cat_cb(const of_val_t *v, bool from_ref, void *u)
{
    (void)from_ref;
    void **p = u;
    if (v->t == OV_ERR) { *(int *)p[1] = v->err; return false; }
    to_text(v, (of_sb_t *)p[0]);
    return true;
}

static void f_concat(ctx_t *x, of_node_t *n, of_val_t *o)
{
    of_sb_t sb = { 0 };
    int err = 0;
    void *p[2] = { &sb, &err };
    for (int i = 0; i < n->nargs && !err && !x->need; i++) each(x, n->a[i], cat_cb, p, false);
    if (x->need) { of_sb_free(&sb); return; }
    if (err) { of_sb_free(&sb); set_err(o, err); return; }
    set_str(o, of_sb_take(&sb));
}

static int u8_count(const char *s)
{
    int n = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) n++;
    return n;
}

static const char *u8_at(const char *s, int chars)
{
    while (*s && chars > 0) {
        s += of_utf8_len((uint8_t)*s);
        chars--;
    }
    return s;
}

static void f_len(ctx_t *x, of_node_t *n, of_val_t *o)
{
    char *s = arg_str(x, n, 0, o);
    if (!s) return;
    set_num(o, u8_count(s));
    of_free(s);
}

static void f_lr(ctx_t *x, of_node_t *n, of_val_t *o, bool left)
{
    char *s = arg_str(x, n, 0, o);
    if (!s) return;
    double k = 1;
    if (n->nargs > 1 && !arg_num(x, n, 1, &k, o)) { of_free(s); return; }
    if (k < 0) { of_free(s); set_err(o, OE_VALUE); return; }
    int total = u8_count(s), cnt = (int)k;
    if (cnt > total) cnt = total;
    const char *a = left ? s : u8_at(s, total - cnt);
    const char *b = left ? u8_at(s, cnt) : s + strlen(s);
    set_str(o, of_strndup(a, (size_t)(b - a)));
    of_free(s);
}
static void f_left(ctx_t *x, of_node_t *n, of_val_t *o) { f_lr(x, n, o, true); }
static void f_right(ctx_t *x, of_node_t *n, of_val_t *o) { f_lr(x, n, o, false); }

static void f_mid(ctx_t *x, of_node_t *n, of_val_t *o)
{
    char *s = arg_str(x, n, 0, o);
    if (!s) return;
    double st, k;
    if (!arg_num(x, n, 1, &st, o) || !arg_num(x, n, 2, &k, o)) { of_free(s); return; }
    if (st < 1 || k < 0) { of_free(s); set_err(o, OE_VALUE); return; }
    const char *a = u8_at(s, (int)st - 1);
    const char *b = u8_at(a, (int)k);
    set_str(o, of_strndup(a, (size_t)(b - a)));
    of_free(s);
}

static void f_case(ctx_t *x, of_node_t *n, of_val_t *o, bool up)
{
    char *s = arg_str(x, n, 0, o);
    if (!s) return;
    for (unsigned char *p = (unsigned char *)s; *p; p++) {
        if (up && *p >= 'a' && *p <= 'z') *p = (unsigned char)(*p - 32);
        else if (!up && *p >= 'A' && *p <= 'Z') *p = (unsigned char)(*p + 32);
        /* the Latin-1 letters, as two bytes C3 xx */
        else if (*p == 0xC3 && p[1]) {
            if (up && p[1] >= 0xA0 && p[1] <= 0xBE && p[1] != 0xB7) p[1] = (unsigned char)(p[1] - 0x20);
            else if (!up && p[1] >= 0x80 && p[1] <= 0x9E && p[1] != 0x97) p[1] = (unsigned char)(p[1] + 0x20);
            p++;
        }
    }
    set_str(o, s);
}
static void f_upper(ctx_t *x, of_node_t *n, of_val_t *o) { f_case(x, n, o, true); }
static void f_lower(ctx_t *x, of_node_t *n, of_val_t *o) { f_case(x, n, o, false); }

static void f_trim(ctx_t *x, of_node_t *n, of_val_t *o)
{
    char *s = arg_str(x, n, 0, o);
    if (!s) return;
    of_sb_t sb = { 0 };
    bool space = false;
    for (const char *p = s; *p; p++) {
        if (*p == ' ') { space = sb.n > 0; continue; }
        if (space) of_sb_putc(&sb, ' ');
        space = false;
        of_sb_putc(&sb, *p);
    }
    of_free(s);
    set_str(o, of_sb_take(&sb));
}

static void f_rept(ctx_t *x, of_node_t *n, of_val_t *o)
{
    char *s = arg_str(x, n, 0, o);
    double k;
    if (!s) return;
    if (!arg_num(x, n, 1, &k, o)) { of_free(s); return; }
    if (k < 0 || k * (double)strlen(s) > 32767) { of_free(s); set_err(o, OE_VALUE); return; }
    of_sb_t sb = { 0 };
    for (int i = 0; i < (int)k; i++) of_sb_puts(&sb, s);
    of_free(s);
    set_str(o, of_sb_take(&sb));
}

static void f_substitute(ctx_t *x, of_node_t *n, of_val_t *o)
{
    char *s = arg_str(x, n, 0, o);
    char *a = s ? arg_str(x, n, 1, o) : NULL;
    char *b = a ? arg_str(x, n, 2, o) : NULL;
    double which = 0;
    if (b && n->nargs > 3 && !arg_num(x, n, 3, &which, o)) { of_free(s); of_free(a); of_free(b); return; }
    if (!b) { of_free(s); of_free(a); return; }
    of_sb_t sb = { 0 };
    size_t al = strlen(a);
    int k = 0;
    for (const char *p = s; *p;) {
        if (al && !strncmp(p, a, al)) {
            k++;
            if (which <= 0 || k == (int)which) {
                of_sb_puts(&sb, b);
                p += al;
                continue;
            }
        }
        of_sb_putc(&sb, *p++);
    }
    of_free(s);
    of_free(a);
    of_free(b);
    set_str(o, of_sb_take(&sb));
}

static void f_find(ctx_t *x, of_node_t *n, of_val_t *o, bool icase)
{
    char *a = arg_str(x, n, 0, o);
    char *s = a ? arg_str(x, n, 1, o) : NULL;
    double st = 1;
    if (s && n->nargs > 2 && !arg_num(x, n, 2, &st, o)) { of_free(a); of_free(s); return; }
    if (!s) { of_free(a); return; }
    const char *from = u8_at(s, (int)st - 1);
    const char *hit = NULL;
    size_t al = strlen(a);
    for (const char *p = from; *p && !hit; p++)
        if (icase ? !strncasecmp(p, a, al) : !strncmp(p, a, al)) hit = p;
    if (!al) hit = from;
    if (!hit) set_err(o, OE_VALUE);
    else {
        char *pre = of_strndup(s, (size_t)(hit - s));
        set_num(o, u8_count(pre) + 1);
        of_free(pre);
    }
    of_free(a);
    of_free(s);
}
static void f_findc(ctx_t *x, of_node_t *n, of_val_t *o) { f_find(x, n, o, false); }
static void f_search(ctx_t *x, of_node_t *n, of_val_t *o) { f_find(x, n, o, true); }

static void f_value(ctx_t *x, of_node_t *n, of_val_t *o)
{
    of_val_t v = { 0 };
    eval_scalar(x, n->a[0], &v);
    if (x->need) { of_val_free(&v); return; }
    double d;
    int e = 0;
    if (v.t == OV_STR) {
        /* the person's way of writing numbers too */
        of_book_t tmp = { 0 };
        of_sheet_t sh = { 0 };
        tmp.sh = &sh;
        tmp.nsh = 1;
        tmp.fmt = OF_FMT_CSV;
        of_cell_input(&tmp, &sh, 0, 0, v.s ? v.s : "");
        of_cell_t *c = of_cell_get(&sh, 0, 0);
        if (c && c->v.t == OV_NUM) set_num(o, c->v.n);
        else set_err(o, OE_VALUE);
        for (int i = 0; i < sh.nrows; i++) {
            for (int k = 0; k < sh.rows[i].n; k++) {
                of_val_free(&sh.rows[i].c[k].v);
                of_free(sh.rows[i].c[k].f);
            }
            of_free(sh.rows[i].c);
        }
        of_free(sh.rows);
    } else if (to_num(&v, &d, &e)) {
        set_num(o, d);
    } else {
        set_err(o, e);
    }
    of_val_free(&v);
}

static void f_text(ctx_t *x, of_node_t *n, of_val_t *o)
{
    of_val_t v = { 0 };
    eval_scalar(x, n->a[0], &v);
    char *code = arg_str(x, n, 1, o);
    if (x->need || !code) { of_val_free(&v); of_free(code); return; }
    if (v.t == OV_ERR) { set_err(o, v.err); of_val_free(&v); of_free(code); return; }
    double d;
    if (v.t == OV_STR && str_num(v.s, &d)) { of_val_free(&v); v.t = OV_NUM; v.n = d; }
    char buf[128];
    bool right;
    of_numfmt_format(code, &v, x->b->date1904, buf, sizeof buf, &right);
    of_free(code);
    of_val_free(&v);
    set_str(o, of_strdup(buf));
}

/* --- lookup --------------------------------------------------------------- */

static void f_xlookup(ctx_t *x, of_node_t *n, of_val_t *o, bool vertical)
{
    of_node_t *t = n->a[1];
    if (t->k != N_REF && t->k != N_RANGE) { set_err(o, OE_VALUE); return; }
    of_val_t key = { 0 };
    eval_scalar(x, n->a[0], &key);
    double idx;
    if (x->need) { of_val_free(&key); return; }
    if (key.t == OV_ERR) { set_err(o, key.err); of_val_free(&key); return; }
    if (!arg_num(x, n, 2, &idx, o)) { of_val_free(&key); return; }
    bool approx = true;
    if (n->nargs > 3) {
        of_val_t a = { 0 };
        eval_scalar(x, n->a[3], &a);
        int e = 0;
        approx = n->a[3]->k == N_NAME ? false : truthy(&a, &e);
        of_val_free(&a);
    }
    int ix = (int)idx;
    int32_t span = vertical ? t->c2 - t->c1 + 1 : t->r2 - t->r1 + 1;
    if (ix < 1) { of_val_free(&key); set_err(o, OE_VALUE); return; }
    if (ix > span) { of_val_free(&key); set_err(o, OE_REF); return; }
    int sh = t->sheet < 0 ? x->sheet : t->sheet;
    int32_t mr, mc;
    of_sheet_extent(&x->b->sh[sh < x->b->nsh ? sh : 0], &mr, &mc);
    int32_t last = vertical ? (t->r2 > mr ? mr : t->r2) : (t->c2 > mc ? mc : t->c2);
    int32_t first = vertical ? t->r1 : t->c1;
    int32_t hit = -1;
    for (int32_t i = first; i <= last; i++) {
        const of_val_t *v = vertical ? cell_val(x, sh, i, t->c1) : cell_val(x, sh, t->r1, i);
        if (x->need) { of_val_free(&key); return; }
        if (v->t == OV_EMPTY) continue;
        if (approx) {
            if ((v->t == OV_NUM) != (key.t == OV_NUM)) continue;
            int c = cmp_vals(v, &key);
            if (c <= 0) hit = i;
            else break;
        } else {
            bool eq;
            if (key.t == OV_STR && v->t == OV_STR) eq = wild(key.s ? key.s : "", v->s ? v->s : "");
            else eq = cmp_vals(v, &key) == 0 && (v->t == OV_NUM) == (key.t == OV_NUM);
            if (eq) { hit = i; break; }
        }
    }
    of_val_free(&key);
    if (hit < 0) { set_err(o, OE_NA); return; }
    const of_val_t *r = vertical ? cell_val(x, sh, hit, t->c1 + ix - 1) : cell_val(x, sh, t->r1 + ix - 1, hit);
    if (x->need) return;
    of_val_copy(o, r);
    if (o->t == OV_EMPTY) set_num(o, 0);
}
static void f_vlookup(ctx_t *x, of_node_t *n, of_val_t *o) { f_xlookup(x, n, o, true); }
static void f_hlookup(ctx_t *x, of_node_t *n, of_val_t *o) { f_xlookup(x, n, o, false); }

static void f_index(ctx_t *x, of_node_t *n, of_val_t *o)
{
    of_node_t *t = n->a[0];
    if (t->k != N_REF && t->k != N_RANGE) { set_err(o, OE_VALUE); return; }
    double r = 0, c = 0;
    if (!arg_num(x, n, 1, &r, o)) return;
    if (n->nargs > 2 && !arg_num(x, n, 2, &c, o)) return;
    int32_t h = t->r2 - t->r1 + 1, w = t->c2 - t->c1 + 1;
    int ir = (int)r, ic = (int)c;
    if (n->nargs < 3) {
        if (h == 1) { ic = ir; ir = 1; }
        else ic = 1;
    }
    if (ir == 0) ir = 1;
    if (ic == 0) ic = 1;
    if (ir < 1 || ic < 1 || ir > h || ic > w) { set_err(o, OE_REF); return; }
    of_val_copy(o, cell_val(x, t->sheet, t->r1 + ir - 1, t->c1 + ic - 1));
    if (o->t == OV_EMPTY && !x->need) set_num(o, 0);
}

static void f_match(ctx_t *x, of_node_t *n, of_val_t *o)
{
    of_node_t *t = n->a[1];
    if (t->k != N_REF && t->k != N_RANGE) { set_err(o, OE_VALUE); return; }
    of_val_t key = { 0 };
    eval_scalar(x, n->a[0], &key);
    double mt = 1;
    if (n->nargs > 2 && !arg_num(x, n, 2, &mt, o)) { of_val_free(&key); return; }
    if (x->need) { of_val_free(&key); return; }
    bool vertical = t->c1 == t->c2;
    int32_t cnt = vertical ? t->r2 - t->r1 + 1 : t->c2 - t->c1 + 1;
    int sh = t->sheet < 0 ? x->sheet : t->sheet;
    int32_t mr, mc;
    of_sheet_extent(&x->b->sh[sh < x->b->nsh ? sh : 0], &mr, &mc);
    int32_t lim = vertical ? mr - t->r1 + 1 : mc - t->c1 + 1;
    if (lim < cnt) cnt = lim;
    int32_t hit = -1;
    for (int32_t i = 0; i < cnt; i++) {
        const of_val_t *v = vertical ? cell_val(x, sh, t->r1 + i, t->c1) : cell_val(x, sh, t->r1, t->c1 + i);
        if (x->need) { of_val_free(&key); return; }
        if (v->t == OV_EMPTY) continue;
        int c = cmp_vals(v, &key);
        if (mt == 0) {
            bool eq = key.t == OV_STR && v->t == OV_STR ? wild(key.s ? key.s : "", v->s ? v->s : "") : c == 0;
            if (eq) { hit = i; break; }
        } else if (mt > 0) {
            if (c <= 0) hit = i;
            else break;
        } else {
            if (c >= 0) hit = i;
            else break;
        }
    }
    of_val_free(&key);
    if (hit < 0) set_err(o, OE_NA);
    else set_num(o, hit + 1);
}

/* --- dates ---------------------------------------------------------------- */

double of_today(void)
{
    time_t now = time(NULL);
    if (now < 1600000000) return 0;
    struct tm tm;
    localtime_r(&now, &tm);
    return of_date_serial(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static void f_today(ctx_t *x, of_node_t *n, of_val_t *o)
{
    (void)x; (void)n;
    double t = of_today();
    if (t == 0) set_err(o, OE_NA);
    else set_num(o, t);
}

static void f_now(ctx_t *x, of_node_t *n, of_val_t *o)
{
    (void)x; (void)n;
    time_t now = time(NULL);
    if (now < 1600000000) { set_err(o, OE_NA); return; }
    struct tm tm;
    localtime_r(&now, &tm);
    set_num(o, of_date_serial(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) +
                 (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) / 86400.0);
}

static void f_date(ctx_t *x, of_node_t *n, of_val_t *o)
{
    double y, m, d;
    if (!arg_num(x, n, 0, &y, o) || !arg_num(x, n, 1, &m, o) || !arg_num(x, n, 2, &d, o)) return;
    int iy = (int)y, im = (int)m;
    if (iy < 1900) iy += 1900;
    /* months past 12 or below 1 roll the year */
    iy += (im - 1) >= 0 ? (im - 1) / 12 : -((12 - im) / 12);
    im = ((im - 1) % 12 + 12) % 12 + 1;
    set_num(o, of_date_serial(iy, im, 1) + (int)d - 1);
}

static void f_ymd(ctx_t *x, of_node_t *n, of_val_t *o, int what)
{
    double s;
    if (!arg_num(x, n, 0, &s, o)) return;
    int y, m, d;
    of_serial_date(s, &y, &m, &d);
    set_num(o, what == 0 ? y : what == 1 ? m : d);
}
static void f_year(ctx_t *x, of_node_t *n, of_val_t *o) { f_ymd(x, n, o, 0); }
static void f_month(ctx_t *x, of_node_t *n, of_val_t *o) { f_ymd(x, n, o, 1); }
static void f_day(ctx_t *x, of_node_t *n, of_val_t *o) { f_ymd(x, n, o, 2); }

static void f_weekday(ctx_t *x, of_node_t *n, of_val_t *o)
{
    double s, t = 1;
    if (!arg_num(x, n, 0, &s, o)) return;
    if (n->nargs > 1 && !arg_num(x, n, 1, &t, o)) return;
    int w = (int)d_mod(d_floor(s) + 6, 7);      /* 0 = Sunday */
    if ((int)t == 2) set_num(o, (w + 6) % 7 + 1);
    else if ((int)t == 3) set_num(o, (w + 6) % 7);
    else set_num(o, w + 1);
}

/* --- the table ------------------------------------------------------------ */

static const fndef_t FNS[] = {
    { "SUM", "SUMA", "SUMME", 1, -1, 0, f_sum },
    { "AVERAGE", "PROMEDIO", "MITTELWERT", 1, -1, 0, f_average },
    { "MIN", NULL, NULL, 1, -1, 0, f_min },
    { "MAX", NULL, NULL, 1, -1, 0, f_max },
    { "COUNT", "CONTAR", "ANZAHL", 1, -1, 0, f_count },
    { "COUNTA", "CONTARA", "ANZAHL2", 1, -1, 0, f_counta },
    { "COUNTBLANK", "CONTAR.BLANCO", "ANZAHLLEEREZELLEN", 1, 1, 0, f_countblank },
    { "PRODUCT", "PRODUCTO", "PRODUKT", 1, -1, 0, f_product },
    { "MEDIAN", "MEDIANA", NULL, 1, -1, 0, f_median },
    { "SUMIF", "SUMAR.SI", "SUMMEWENN", 2, 3, 0, f_sumif },
    { "COUNTIF", "CONTAR.SI", "ZÄHLENWENN", 2, 2, 0, f_countif },
    { "AVERAGEIF", "PROMEDIO.SI", "MITTELWERTWENN", 2, 3, 0, f_averageif },
    { "SUMPRODUCT", "SUMAPRODUCTO", "SUMMENPRODUKT", 1, -1, 0, f_sumproduct },
    { "ROUND", "REDONDEAR", "RUNDEN", 1, 2, 0, f_round },
    { "ROUNDUP", "REDONDEAR.MAS", "AUFRUNDEN", 1, 2, 0, f_roundup },
    { "ROUNDDOWN", "REDONDEAR.MENOS", "ABRUNDEN", 1, 2, 0, f_rounddown },
    { "TRUNC", "TRUNCAR", "KÜRZEN", 1, 2, 0, f_trunc },
    { "INT", "ENTERO", "GANZZAHL", 1, 1, 0, f_int },
    { "ABS", NULL, NULL, 1, 1, 0, f_abs },
    { "SIGN", "SIGNO", "VORZEICHEN", 1, 1, 0, f_sign },
    { "MOD", "RESIDUO", "REST", 2, 2, 0, f_mod },
    { "POWER", "POTENCIA", "POTENZ", 2, 2, 0, f_power },
    { "SQRT", "RAIZ", "WURZEL", 1, 1, 0, f_sqrt },
    { "PI", NULL, NULL, 0, 0, 0, f_pi },
    { "IF", "SI", "WENN", 1, 3, 0, f_if },
    { "AND", "Y", "UND", 1, -1, 0, f_and },
    { "OR", "O", "ODER", 1, -1, 0, f_or },
    { "NOT", "NO", "NICHT", 1, 1, 0, f_not },
    { "TRUE", "VERDADERO", "WAHR", 0, 0, 0, f_true },
    { "FALSE", "FALSO", "FALSCH", 0, 0, 0, f_false },
    { "IFERROR", "SI.ERROR", "WENNFEHLER", 2, 2, 0, f_iferror },
    { "IFNA", "SI.ND", "WENNNV", 2, 2, 1, f_ifna },
    { "ISBLANK", "ESBLANCO", "ISTLEER", 1, 1, 0, f_isblank },
    { "ISNUMBER", "ESNUMERO", "ISTZAHL", 1, 1, 0, f_isnumber },
    { "ISTEXT", "ESTEXTO", "ISTTEXT", 1, 1, 0, f_istext },
    { "ISERROR", "ESERROR", "ISTFEHLER", 1, 1, 0, f_iserror },
    { "ISNA", "ESNOD", "ISTNV", 1, 1, 0, f_isna },
    { "CONCATENATE", "CONCATENAR", "VERKETTEN", 1, -1, 0, f_concat },
    { "CONCAT", NULL, "TEXTKETTE", 1, -1, 1, f_concat },
    { "LEN", "LARGO", "LÄNGE", 1, 1, 0, f_len },
    { "LEFT", "IZQUIERDA", "LINKS", 1, 2, 0, f_left },
    { "RIGHT", "DERECHA", "RECHTS", 1, 2, 0, f_right },
    { "MID", "EXTRAE", "TEIL", 3, 3, 0, f_mid },
    { "UPPER", "MAYUSC", "GROSS", 1, 1, 0, f_upper },
    { "LOWER", "MINUSC", "KLEIN", 1, 1, 0, f_lower },
    { "TRIM", "ESPACIOS", "GLÄTTEN", 1, 1, 0, f_trim },
    { "REPT", "REPETIR", "WIEDERHOLEN", 2, 2, 0, f_rept },
    { "SUBSTITUTE", "SUSTITUIR", "WECHSELN", 3, 4, 0, f_substitute },
    { "FIND", "ENCONTRAR", "FINDEN", 2, 3, 0, f_findc },
    { "SEARCH", "HALLAR", "SUCHEN", 2, 3, 0, f_search },
    { "VALUE", "VALOR", "WERT", 1, 1, 0, f_value },
    { "TEXT", "TEXTO", NULL, 2, 2, 0, f_text },
    { "VLOOKUP", "BUSCARV", "SVERWEIS", 3, 4, 0, f_vlookup },
    { "HLOOKUP", "BUSCARH", "WVERWEIS", 3, 4, 0, f_hlookup },
    { "INDEX", "INDICE", "INDEX", 2, 3, 0, f_index },
    { "MATCH", "COINCIDIR", "VERGLEICH", 2, 3, 0, f_match },
    { "TODAY", "HOY", "HEUTE", 0, 0, 0, f_today },
    { "NOW", "AHORA", "JETZT", 0, 0, 0, f_now },
    { "DATE", "FECHA", "DATUM", 3, 3, 0, f_date },
    { "YEAR", "A\xC3\x91O", "JAHR", 1, 1, 0, f_year },
    { "MONTH", "MES", "MONAT", 1, 1, 0, f_month },
    { "DAY", "DIA", "TAG", 1, 1, 0, f_day },
    { "WEEKDAY", "DIASEM", "WOCHENTAG", 1, 2, 0, f_weekday },
};

static const fndef_t *fn_table(int *count)
{
    *count = (int)(sizeof FNS / sizeof FNS[0]);
    return FNS;
}

/* The names the app knows, for the help sheet: English and the person's. */
int of_formula_fn_count(void)
{
    return (int)(sizeof FNS / sizeof FNS[0]);
}

const char *of_formula_fn_name(int i, int lang)     /* 0 en, 1 es, 2 de */
{
    if (i < 0 || i >= of_formula_fn_count()) return NULL;
    const char *n = lang == 1 ? FNS[i].es : lang == 2 ? FNS[i].de : NULL;
    return n ? n : FNS[i].name;
}

/* -------------------------------------------------------------------------- */
/* Recalculation                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    of_cell_t *cell;
    int        sheet;
    int32_t    row;
} stk_t;

static bool locate(of_book_t *b, of_cell_t *c, int *sheet, int32_t *row)
{
    for (int s = 0; s < b->nsh; s++) {
        of_sheet_t *sh = &b->sh[s];
        for (int r = 0; r < sh->nrows; r++) {
            of_row_t *rw = &sh->rows[r];
            if (rw->n && c >= rw->c && c < rw->c + rw->n) {
                *sheet = s;
                *row = rw->row;
                return true;
            }
        }
    }
    return false;
}

void of_book_recalc(of_book_t *b)
{
    /* everything stale, the ones the app cannot compute keep the file's value */
    for (int s = 0; s < b->nsh; s++)
        for (int r = 0; r < b->sh[s].nrows; r++)
            for (int k = 0; k < b->sh[s].rows[r].n; k++) {
                of_cell_t *c = &b->sh[s].rows[r].c[k];
                if (!c->f) { c->state = 2; continue; }
                if (!c->ast || (c->flags & (OC_UNKNOWN_F | OC_ARRAY))) {
                    if (!(c->flags & OC_EDITED) || (c->flags & OC_ARRAY)) {
                        of_val_free(&c->v);
                        of_val_copy(&c->v, &c->cached);
                    }
                    c->state = 2;
                    continue;
                }
                c->state = 0;
            }
    int cap = 64, top = 0;
    stk_t *st = of_alloc((size_t)cap * sizeof *st);
    if (!st) return;
    for (int s = 0; s < b->nsh; s++)
        for (int r = 0; r < b->sh[s].nrows; r++)
            for (int k = 0; k < b->sh[s].rows[r].n; k++) {
                of_cell_t *c = &b->sh[s].rows[r].c[k];
                if (c->state == 2) continue;
                top = 0;
                st[top++] = (stk_t){ c, s, b->sh[s].rows[r].row };
                c->state = 1;
                while (top) {
                    stk_t *t = &st[top - 1];
                    ctx_t x = { .b = b, .sheet = t->sheet, .row = t->row, .col = t->cell->col };
                    of_val_t v = { 0 };
                    eval(&x, t->cell->ast, &v);
                    if (x.need) {
                        of_val_free(&v);
                        if (top == cap) {
                            cap *= 2;
                            stk_t *ns = of_realloc(st, (size_t)cap * sizeof *ns);
                            if (!ns) { of_free(st); return; }
                            st = ns;
                        }
                        int ns_sheet = 0;
                        int32_t ns_row = 0;
                        locate(b, x.need, &ns_sheet, &ns_row);
                        x.need->state = 1;
                        st[top++] = (stk_t){ x.need, ns_sheet, ns_row };
                        continue;
                    }
                    if (v.t == OV_EMPTY) { v.t = OV_NUM; v.n = 0; }
                    of_val_free(&t->cell->v);
                    t->cell->v = v;
                    t->cell->state = 2;
                    top--;
                }
            }
    of_free(st);
}
