/*
 * Tortuga - the game's Logo (DESIGN.md, 2.3): the tokens, the parser into
 * a tree, the interpreter that walks it against the world, the pieces that
 * give the stars, and the tree written back as Logo, Lua or Python (what
 * the blocks show as text). The same rules as LogoParser and Logo in
 * tools/tg_ref.py.
 */
#include "tg_core.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ tokens */

enum { T_EOF, T_NUM, T_STR, T_OP, T_KW, T_NAME };
enum {
    K_TO, K_END, K_REPEAT, K_IF, K_ELSE, K_WHILE, K_MAKE, K_NOT, K_AND, K_OR, K_STOP, K_OUTPUT,
    K_TRUE, K_FALSE, K_N
};

static const struct { const char *w; int k; } KW[] = {
    { "para", K_TO }, { "to", K_TO }, { "fin", K_END }, { "end", K_END },
    { "repetir", K_REPEAT }, { "repeat", K_REPEAT }, { "si", K_IF }, { "if", K_IF },
    { "sino", K_ELSE }, { "else", K_ELSE }, { "mientras", K_WHILE }, { "while", K_WHILE },
    { "haz", K_MAKE }, { "make", K_MAKE }, { "no", K_NOT }, { "not", K_NOT },
    { "y", K_AND }, { "and", K_AND }, { "o", K_OR }, { "or", K_OR },
    { "alto", K_STOP }, { "stop", K_STOP }, { "devolver", K_OUTPUT }, { "output", K_OUTPUT },
    { "verdadero", K_TRUE }, { "true", K_TRUE }, { "falso", K_FALSE }, { "false", K_FALSE },
};

typedef struct {
    uint8_t t;
    uint8_t k;          /* keyword, or the operator's code */
    int16_t line;
    float num;
    char s[TG_NAME_LEN];
} tok_t;

/* operators: one byte each */
enum { O_LB = '[', O_RB = ']', O_LP = '(', O_RP = ')', O_ADD = '+', O_SUB = '-', O_MUL = '*',
       O_DIV = '/', O_LT = '<', O_GT = '>', O_EQ = '=', O_LE = 1, O_GE = 2, O_NE = 3, O_EQEQ = 4,
       O_ADDEQ = 5, O_SUBEQ = 6 };

typedef struct {
    tg_prog_t *p;
    const char *src;
    int pos, line;
    tok_t cur, nxt;
    bool fail;
    int proc_arity[TG_MAX_NAMES];   /* -1: not a procedure */
} parser_t;

static void perr(parser_t *ps, int line, const char *fmt, ...)
{
    if (ps->fail) return;
    ps->fail = true;
    ps->p->err_line = line;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(ps->p->err, sizeof(ps->p->err), fmt, ap);
    va_end(ap);
}

static bool is_name_char(unsigned char c, bool first)
{
    if (c >= 0x80) return true;         /* letters with accents, ñ */
    if (isalpha(c) || c == '_') return true;
    return !first && isdigit(c);
}

static void lex(parser_t *ps, tok_t *t)
{
    const char *s = ps->src;
    memset(t, 0, sizeof(*t));
    for (;;) {
        char c = s[ps->pos];
        if (c == '\n') {
            ps->line++;
            ps->pos++;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            ps->pos++;
        } else if (c == ';' || c == '#') {
            while (s[ps->pos] && s[ps->pos] != '\n') ps->pos++;
        } else {
            break;
        }
    }
    t->line = (int16_t)ps->line;
    char c = s[ps->pos];
    if (!c) {
        t->t = T_EOF;
        return;
    }
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)s[ps->pos + 1]))) {
        char *end;
        t->t = T_NUM;
        t->num = strtof(s + ps->pos, &end);
        ps->pos = (int)(end - s);
        return;
    }
    if (c == '"') {
        /* "word, "word" or "a few words" on one line */
        int p = ps->pos + 1, q = p;
        const char *close = NULL;
        for (int i = p; s[i] && s[i] != '\n'; i++) {
            if (s[i] == '"') {
                close = s + i;
                break;
            }
        }
        if (close) {
            q = (int)(close - s);
            ps->pos = q + 1;
        } else {
            while (s[q] && s[q] != ' ' && s[q] != '\n' && s[q] != ']' && s[q] != '\t') q++;
            ps->pos = q;
        }
        int n = q - p;
        if (n >= TG_NAME_LEN) n = TG_NAME_LEN - 1;
        memcpy(t->s, s + p, (size_t)n);
        t->s[n] = 0;
        t->t = T_STR;
        return;
    }
    if (is_name_char((unsigned char)c, true) || (c == ':' && is_name_char((unsigned char)s[ps->pos + 1], true))) {
        if (c == ':') ps->pos++;
        int n = 0;
        while (is_name_char((unsigned char)s[ps->pos], false)) {
            char ch = s[ps->pos++];
            if (n < TG_NAME_LEN - 1) t->s[n++] = (char)tolower((unsigned char)ch);
        }
        t->s[n] = 0;
        for (size_t i = 0; i < sizeof(KW) / sizeof(KW[0]); i++) {
            if (!strcmp(t->s, KW[i].w)) {
                t->t = T_KW;
                t->k = (uint8_t)KW[i].k;
                return;
            }
        }
        t->t = T_NAME;
        return;
    }
    char d = s[ps->pos + 1];
    t->t = T_OP;
    ps->pos++;
    if (c == '<' && d == '=') { t->k = O_LE; ps->pos++; return; }
    if (c == '>' && d == '=') { t->k = O_GE; ps->pos++; return; }
    if (c == '<' && d == '>') { t->k = O_NE; ps->pos++; return; }
    if (c == '!' && d == '=') { t->k = O_NE; ps->pos++; return; }
    if (c == '=' && d == '=') { t->k = O_EQEQ; ps->pos++; return; }
    if (c == '+' && d == '=') { t->k = O_ADDEQ; ps->pos++; return; }
    if (c == '-' && d == '=') { t->k = O_SUBEQ; ps->pos++; return; }
    if (strchr("[]()+-*/<>=", c)) {
        t->k = (uint8_t)c;
        return;
    }
    perr(ps, ps->line, _("no entiendo «%c»"), c);
    t->t = T_EOF;
}

static void advance(parser_t *ps)
{
    ps->cur = ps->nxt;
    lex(ps, &ps->nxt);
}

/* ------------------------------------------------------------------- names */

static int name_id(tg_prog_t *p, const char *s)
{
    for (int i = 0; i < p->nname; i++) {
        if (!strcmp(p->name[i], s)) return i;
    }
    if (p->nname >= TG_MAX_NAMES) return -1;
    snprintf(p->name[p->nname], TG_NAME_LEN, "%s", s);
    return p->nname++;
}

static tg_node_t *node(parser_t *ps, int kind, int line)
{
    tg_prog_t *p = ps->p;
    if (p->nnode >= p->cap) {
        perr(ps, line, _("el programa es demasiado largo"));
        static tg_node_t spare;
        memset(&spare, 0, sizeof(spare));
        return &spare;
    }
    tg_node_t *n = &p->node[p->nnode++];
    memset(n, 0, sizeof(*n));
    n->kind = (uint8_t)kind;
    n->line = (int16_t)line;
    n->name = -1;
    return n;
}

bool tg_prog_init(tg_prog_t *p, int cap)
{
    memset(p, 0, sizeof(*p));
    p->node = tg_malloc(sizeof(tg_node_t) * (size_t)cap);
    p->cap = p->node ? cap : 0;
    return p->node != NULL;
}

void tg_prog_free(tg_prog_t *p)
{
    tg_free(p->node);
    memset(p, 0, sizeof(*p));
}

/* ------------------------------------------------------------------ parser */

static tg_node_t *expr(parser_t *ps);
static tg_node_t *statement(parser_t *ps);

static int is_proc(parser_t *ps, const char *s)
{
    for (int i = 0; i < ps->p->nname; i++) {
        if (!strcmp(ps->p->name[i], s)) return ps->proc_arity[i];
    }
    return -1;
}

static bool is_turn(const char *s)
{
    return !strcmp(s, "girar") || !strcmp(s, "turn");
}

static bool starts_expr(parser_t *ps)
{
    const tok_t *t = &ps->cur;
    switch (t->t) {
    case T_NUM:
    case T_STR:
        return true;
    case T_OP:
        return t->k == O_LP || t->k == O_SUB;
    case T_KW:
        return t->k == K_NOT || t->k == K_TRUE || t->k == K_FALSE;
    case T_NAME: {
        if (ps->nxt.t == T_OP && (ps->nxt.k == O_EQ || ps->nxt.k == O_ADDEQ || ps->nxt.k == O_SUBEQ)) {
            return false;       /* the next statement is an assignment */
        }
        int o = tg_order_find(t->s);
        if (o >= 0 && !tg_orders[o].sense) return false;
        if (is_turn(t->s) || is_proc(ps, t->s) >= 0) return false;
        return true;
    }
    }
    return false;
}

static tg_node_t *primary(parser_t *ps)
{
    tok_t t = ps->cur;
    if (t.t == T_NUM) {
        advance(ps);
        tg_node_t *n = node(ps, TG_E_NUM, t.line);
        n->num = t.num;
        return n;
    }
    if (t.t == T_STR) {
        advance(ps);
        tg_node_t *n = node(ps, TG_E_STR, t.line);
        n->name = (int16_t)name_id(ps->p, t.s);
        return n;
    }
    if (t.t == T_KW && (t.k == K_TRUE || t.k == K_FALSE)) {
        advance(ps);
        tg_node_t *n = node(ps, TG_E_BOOL, t.line);
        n->num = t.k == K_TRUE;
        return n;
    }
    if (t.t == T_OP && t.k == O_LP) {
        advance(ps);
        tg_node_t *e = expr(ps);
        if (ps->cur.t == T_OP && ps->cur.k == O_RP) advance(ps);
        else perr(ps, ps->cur.line, _("falta cerrar el paréntesis"));
        return e;
    }
    if (t.t == T_NAME) {
        int o = tg_order_find(t.s);
        if (o >= 0 && tg_orders[o].sense) {
            advance(ps);
            tg_node_t *n = node(ps, TG_E_SENSE, t.line);
            n->op = (uint8_t)o;
            return n;
        }
        int ar = is_proc(ps, t.s);
        if (ar >= 0) {
            advance(ps);
            tg_node_t *n = node(ps, TG_E_FCALL, t.line);
            n->name = (int16_t)name_id(ps->p, t.s);
            tg_node_t **tail = &n->a;
            for (int i = 0; i < ar; i++) {
                *tail = expr(ps);
                tail = &(*tail)->next;
            }
            return n;
        }
        if (o >= 0 || is_turn(t.s)) {
            perr(ps, t.line, _("«%s» es una orden, no un valor"), t.s);
            return node(ps, TG_E_NUM, t.line);
        }
        advance(ps);
        tg_node_t *n = node(ps, TG_E_VAR, t.line);
        n->name = (int16_t)name_id(ps->p, t.s);
        return n;
    }
    perr(ps, t.line, _("falta un valor"));
    if (t.t != T_EOF) advance(ps);
    return node(ps, TG_E_NUM, t.line);
}

static tg_node_t *unary(parser_t *ps)
{
    if (ps->cur.t == T_OP && ps->cur.k == O_SUB) {
        int line = ps->cur.line;
        advance(ps);
        tg_node_t *n = node(ps, TG_E_NEG, line);
        n->a = unary(ps);
        return n;
    }
    return primary(ps);
}

static tg_node_t *bin(parser_t *ps, int op, tg_node_t *a, tg_node_t *b, int kind)
{
    tg_node_t *n = node(ps, kind, a->line);
    n->op = (uint8_t)op;
    n->a = a;
    n->b = b;
    return n;
}

static tg_node_t *mul(parser_t *ps)
{
    tg_node_t *a = unary(ps);
    while (!ps->fail && ps->cur.t == T_OP && (ps->cur.k == O_MUL || ps->cur.k == O_DIV)) {
        int op = ps->cur.k == O_MUL ? TG_OP_MUL : TG_OP_DIV;
        advance(ps);
        a = bin(ps, op, a, unary(ps), TG_E_BIN);
    }
    return a;
}

static tg_node_t *add(parser_t *ps)
{
    tg_node_t *a = mul(ps);
    while (!ps->fail && ps->cur.t == T_OP && (ps->cur.k == O_ADD || ps->cur.k == O_SUB)) {
        int op = ps->cur.k == O_ADD ? TG_OP_ADD : TG_OP_SUB;
        advance(ps);
        a = bin(ps, op, a, mul(ps), TG_E_BIN);
    }
    return a;
}

static tg_node_t *cmp(parser_t *ps)
{
    tg_node_t *a = add(ps);
    if (ps->cur.t == T_OP) {
        int op = -1;
        switch (ps->cur.k) {
        case O_LT: op = TG_OP_LT; break;
        case O_GT: op = TG_OP_GT; break;
        case O_EQ: case O_EQEQ: op = TG_OP_EQ; break;
        case O_LE: op = TG_OP_LE; break;
        case O_GE: op = TG_OP_GE; break;
        case O_NE: op = TG_OP_NE; break;
        }
        if (op >= 0) {
            advance(ps);
            a = bin(ps, op, a, add(ps), TG_E_CMP);
        }
    }
    return a;
}

static tg_node_t *not_(parser_t *ps)
{
    if (ps->cur.t == T_KW && ps->cur.k == K_NOT) {
        int line = ps->cur.line;
        advance(ps);
        tg_node_t *n = node(ps, TG_E_NOT, line);
        n->a = not_(ps);
        return n;
    }
    return cmp(ps);
}

static tg_node_t *and_(parser_t *ps)
{
    tg_node_t *a = not_(ps);
    while (!ps->fail && ps->cur.t == T_KW && ps->cur.k == K_AND) {
        advance(ps);
        a = bin(ps, 0, a, not_(ps), TG_E_AND);
    }
    return a;
}

static tg_node_t *expr(parser_t *ps)
{
    tg_node_t *a = and_(ps);
    while (!ps->fail && ps->cur.t == T_KW && ps->cur.k == K_OR) {
        advance(ps);
        a = bin(ps, 0, a, and_(ps), TG_E_OR);
    }
    return a;
}

static tg_node_t *block(parser_t *ps)
{
    if (!(ps->cur.t == T_OP && ps->cur.k == O_LB)) {
        perr(ps, ps->cur.line, _("falta abrir el bloque con ["));
        return NULL;
    }
    int line = ps->cur.line;
    advance(ps);
    tg_node_t *head = NULL, **tail = &head;
    while (!ps->fail && !(ps->cur.t == T_OP && ps->cur.k == O_RB)) {
        if (ps->cur.t == T_EOF) {
            perr(ps, line, _("falta cerrar el bloque con ]"));
            return head;
        }
        *tail = statement(ps);
        tail = &(*tail)->next;
    }
    advance(ps);
    return head;
}

static tg_node_t *statement(parser_t *ps)
{
    tok_t t = ps->cur;
    if (t.t == T_KW) {
        advance(ps);
        switch (t.k) {
        case K_TO: {
            tg_node_t *n = node(ps, TG_N_TO, t.line);
            if (ps->cur.t != T_NAME) {
                perr(ps, t.line, _("después de «para» va el nombre"));
                return n;
            }
            n->name = (int16_t)name_id(ps->p, ps->cur.s);
            advance(ps);
            tg_node_t **tail = &n->a;
            while (ps->cur.t == T_NAME && ps->cur.line == t.line) {
                tg_node_t *pr = node(ps, TG_N_PARAM, t.line);
                pr->name = (int16_t)name_id(ps->p, ps->cur.s);
                *tail = pr;
                tail = &pr->next;
                advance(ps);
            }
            tail = &n->b;
            while (!ps->fail && !(ps->cur.t == T_KW && ps->cur.k == K_END)) {
                if (ps->cur.t == T_EOF) {
                    perr(ps, t.line, _("falta el «fin» de «%s»"), ps->p->name[n->name]);
                    return n;
                }
                *tail = statement(ps);
                tail = &(*tail)->next;
            }
            advance(ps);
            return n;
        }
        case K_REPEAT: {
            tg_node_t *n = node(ps, TG_N_REPEAT, t.line);
            n->a = expr(ps);
            n->b = block(ps);
            return n;
        }
        case K_IF: {
            tg_node_t *n = node(ps, TG_N_IF, t.line);
            n->a = expr(ps);
            n->b = block(ps);
            if (ps->cur.t == T_KW && ps->cur.k == K_ELSE) {
                advance(ps);
                if (ps->cur.t == T_KW && ps->cur.k == K_IF) n->c = statement(ps);
                else n->c = block(ps);
            }
            return n;
        }
        case K_WHILE: {
            tg_node_t *n = node(ps, TG_N_WHILE, t.line);
            n->a = expr(ps);
            n->b = block(ps);
            return n;
        }
        case K_MAKE: {
            tg_node_t *n = node(ps, TG_N_SET, t.line);
            if (ps->cur.t != T_NAME) {
                perr(ps, t.line, _("después de «haz» va el nombre"));
                return n;
            }
            n->name = (int16_t)name_id(ps->p, ps->cur.s);
            advance(ps);
            n->a = expr(ps);
            return n;
        }
        case K_STOP:
            return node(ps, TG_N_STOP, t.line);
        case K_OUTPUT: {
            tg_node_t *n = node(ps, TG_N_OUTPUT, t.line);
            n->a = expr(ps);
            return n;
        }
        default:
            perr(ps, t.line, _("«%s» no va acá"), t.s);
            return node(ps, TG_N_STOP, t.line);
        }
    }
    if (t.t == T_NAME) {
        if (ps->nxt.t == T_OP && (ps->nxt.k == O_EQ || ps->nxt.k == O_ADDEQ || ps->nxt.k == O_SUBEQ)) {
            int op = ps->nxt.k;
            advance(ps);
            advance(ps);
            tg_node_t *n = node(ps, TG_N_SET, t.line);
            n->name = (int16_t)name_id(ps->p, t.s);
            tg_node_t *e = expr(ps);
            if (op != O_EQ) {
                tg_node_t *v = node(ps, TG_E_VAR, t.line);
                v->name = n->name;
                e = bin(ps, op == O_ADDEQ ? TG_OP_ADD : TG_OP_SUB, v, e, TG_E_BIN);
            }
            n->a = e;
            return n;
        }
        int o = -1;
        if (is_turn(t.s)) {
            advance(ps);
            const char *side = ps->cur.t == T_NAME ? ps->cur.s : "";
            if (!strcmp(side, "derecha") || !strcmp(side, "right")) o = TG_O_RIGHT;
            else if (!strcmp(side, "izquierda") || !strcmp(side, "left")) o = TG_O_LEFT;
            if (o < 0) {
                perr(ps, t.line, _("después de «%s» va derecha o izquierda"), t.s);
                return node(ps, TG_N_STOP, t.line);
            }
        } else {
            o = tg_order_find(t.s);
        }
        if (o >= 0 && !tg_orders[o].sense) {
            advance(ps);
            tg_node_t *n = node(ps, TG_N_CALL, t.line);
            n->op = (uint8_t)o;
            tg_node_t **tail = &n->a;
            for (int i = 0; i < tg_orders[o].amax && !ps->fail; i++) {
                if (i < tg_orders[o].amin || starts_expr(ps)) {
                    *tail = expr(ps);
                    tail = &(*tail)->next;
                }
            }
            return n;
        }
        int ar = is_proc(ps, t.s);
        if (ar >= 0) {
            advance(ps);
            tg_node_t *n = node(ps, TG_N_PCALL, t.line);
            n->name = (int16_t)name_id(ps->p, t.s);
            tg_node_t **tail = &n->a;
            for (int i = 0; i < ar && !ps->fail; i++) {
                *tail = expr(ps);
                tail = &(*tail)->next;
            }
            return n;
        }
        if (o >= 0) {
            perr(ps, t.line, _("«%s» es una pregunta: va en un «si» o un «mientras»"), t.s);
        } else {
            perr(ps, t.line, _("no conozco «%s»"), t.s);
        }
        advance(ps);
        return node(ps, TG_N_STOP, t.line);
    }
    if (t.t == T_OP && t.k == O_RB) perr(ps, t.line, _("sobra un ]"));
    else perr(ps, t.line, _("no entiendo lo que sigue"));
    advance(ps);
    return node(ps, TG_N_STOP, t.line);
}

bool tg_logo_parse(tg_prog_t *p, const char *src)
{
    parser_t *ps = tg_malloc(sizeof(parser_t));
    if (!ps) {
        snprintf(p->err, sizeof(p->err), "%s", _("no hay memoria"));
        return false;
    }
    p->nnode = 0;
    p->nname = 0;
    p->prog = NULL;
    p->err[0] = 0;
    p->err_line = 0;
    ps->p = p;
    ps->src = src;
    for (int i = 0; i < TG_MAX_NAMES; i++) ps->proc_arity[i] = -1;

    /* first pass: the procedures and how many parameters each takes (the
     * names on the line of "para"), so a call may come before them */
    ps->line = 1;
    tok_t a, b;
    lex(ps, &a);
    while (a.t != T_EOF && !ps->fail) {
        lex(ps, &b);
        if (a.t == T_KW && a.k == K_TO && b.t == T_NAME) {
            int id = name_id(p, b.s);
            int n = 0;
            tok_t c;
            lex(ps, &c);
            while (c.t == T_NAME && c.line == a.line) {
                n++;
                lex(ps, &c);
            }
            if (id >= 0) ps->proc_arity[id] = n;
            a = c;
            continue;
        }
        a = b;
    }
    ps->fail = false;
    p->err[0] = 0;
    ps->pos = 0;
    ps->line = 1;
    lex(ps, &ps->cur);
    lex(ps, &ps->nxt);
    tg_node_t **tail = &p->prog;
    while (!ps->fail && ps->cur.t != T_EOF) {
        *tail = statement(ps);
        tail = &(*tail)->next;
    }
    bool ok = !ps->fail;
    tg_free(ps);
    return ok;
}

/* ------------------------------------------------------------- interpreter */

typedef struct {
    uint8_t t;          /* 0 number, 1 bool, 2 text, 3 nothing */
    float n;
    const char *s;
} val_t;

#define MAX_DEPTH 60

typedef struct {
    tg_prog_t *p;
    tg_world_t *w;
    long steps;
    bool halt;
    val_t glob[TG_MAX_NAMES];
    bool has_glob[TG_MAX_NAMES];
    struct {
        const tg_node_t *proc;
        val_t v[8];
        int16_t name[8];
        int n;
    } frame[MAX_DEPTH];
    int depth;
    const tg_node_t *proc[TG_MAX_NAMES];
    val_t ret;
} interp_t;

enum { X_OK, X_RETURN, X_HALT };

static void rt_err(interp_t *I, int line, const char *fmt, ...)
{
    char m[120];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m, sizeof(m), fmt, ap);
    va_end(ap);
    tg_world_fail(I->w, TG_F_RUNTIME, line, m);
    I->halt = true;
}

static bool tick(interp_t *I, int line)
{
    if (I->halt) return false;
    if (++I->steps > TG_MAX_STEPS) {
        tg_world_fail(I->w, TG_F_LOOP, line, NULL);
        I->halt = true;
        return false;
    }
    return true;
}

static val_t eval(interp_t *I, const tg_node_t *e);
static int exec_list(interp_t *I, const tg_node_t *s);

static val_t num(float n)
{
    val_t v = { 0, n, NULL };
    return v;
}

static int call_proc(interp_t *I, const tg_node_t *call, val_t *out)
{
    const tg_node_t *pr = I->proc[call->name];
    if (!pr) {
        rt_err(I, call->line, _("«%s» todavía no está definido"), I->p->name[call->name]);
        return X_HALT;
    }
    if (I->depth >= MAX_DEPTH) {
        rt_err(I, call->line, _("«%s» se llama a sí mismo demasiadas veces"), I->p->name[call->name]);
        return X_HALT;
    }
    val_t args[8];
    int n = 0;
    for (const tg_node_t *a = call->a; a && n < 8; a = a->next) {
        args[n++] = eval(I, a);
        if (I->halt) return X_HALT;
    }
    int d = I->depth++;
    I->frame[d].proc = pr;
    I->frame[d].n = 0;
    int k = 0;
    for (const tg_node_t *pa = pr->a; pa && k < 8; pa = pa->next, k++) {
        I->frame[d].name[k] = pa->name;
        I->frame[d].v[k] = k < n ? args[k] : num(0);
    }
    I->frame[d].n = k;
    I->ret.t = 3;
    int r = exec_list(I, pr->b);
    I->depth--;
    if (out) *out = I->ret;
    return r == X_HALT ? X_HALT : X_OK;
}

static val_t *var_slot(interp_t *I, int name, bool create)
{
    if (I->depth > 0) {
        int d = I->depth - 1;
        for (int k = 0; k < I->frame[d].n; k++) {
            if (I->frame[d].name[k] == name) return &I->frame[d].v[k];
        }
    }
    if (I->has_glob[name] || create) {
        I->has_glob[name] = true;
        return &I->glob[name];
    }
    return NULL;
}

static float as_num(interp_t *I, val_t v, int line)
{
    if (v.t != 0) {
        rt_err(I, line, _("acá va un número"));
        return 0;
    }
    return v.n;
}

static bool as_bool(interp_t *I, val_t v, int line)
{
    if (v.t != 1) {
        rt_err(I, line, _("acá va una pregunta de sí o no"));
        return false;
    }
    return v.n != 0;
}

static val_t eval(interp_t *I, const tg_node_t *e)
{
    val_t z = { 3, 0, NULL };
    if (!e || I->halt) return z;
    switch (e->kind) {
    case TG_E_NUM:
        return num(e->num);
    case TG_E_STR: {
        val_t v = { 2, 0, I->p->name[e->name] };
        return v;
    }
    case TG_E_BOOL: {
        val_t v = { 1, e->num, NULL };
        return v;
    }
    case TG_E_VAR: {
        val_t *s = var_slot(I, e->name, false);
        if (!s) {
            rt_err(I, e->line, _("«%s» no tiene valor todavía"), I->p->name[e->name]);
            return z;
        }
        return *s;
    }
    case TG_E_SENSE: {
        bool r = false;
        if (!tg_world_call(I->w, e->op, NULL, NULL, 0, e->line, &r)) I->halt = true;
        val_t v = { 1, r ? 1.0f : 0.0f, NULL };
        return v;
    }
    case TG_E_FCALL: {
        val_t v = z;
        if (call_proc(I, e, &v) == X_HALT) I->halt = true;
        return v;
    }
    case TG_E_NEG:
        return num(-as_num(I, eval(I, e->a), e->line));
    case TG_E_NOT: {
        val_t v = { 1, as_bool(I, eval(I, e->a), e->line) ? 0.0f : 1.0f, NULL };
        return v;
    }
    case TG_E_AND: {
        bool a = as_bool(I, eval(I, e->a), e->line);
        val_t v = { 1, 0, NULL };
        if (a) v.n = as_bool(I, eval(I, e->b), e->line);
        return v;
    }
    case TG_E_OR: {
        bool a = as_bool(I, eval(I, e->a), e->line);
        val_t v = { 1, 1, NULL };
        if (!a) v.n = as_bool(I, eval(I, e->b), e->line);
        return v;
    }
    case TG_E_BIN: {
        float a = as_num(I, eval(I, e->a), e->line);
        float b = as_num(I, eval(I, e->b), e->line);
        switch (e->op) {
        case TG_OP_ADD: return num(a + b);
        case TG_OP_SUB: return num(a - b);
        case TG_OP_MUL: return num(a * b);
        default:
            if (b == 0) {
                rt_err(I, e->line, _("no se puede dividir por cero"));
                return z;
            }
            return num(a / b);
        }
    }
    case TG_E_CMP: {
        val_t va = eval(I, e->a), vb = eval(I, e->b);
        bool r = false;
        if (va.t == 2 || vb.t == 2) {
            bool eq = va.t == vb.t && va.s && vb.s && !strcmp(va.s, vb.s);
            r = e->op == TG_OP_EQ ? eq : e->op == TG_OP_NE ? !eq : false;
        } else if (va.t == 1 || vb.t == 1) {
            bool eq = va.t == vb.t && va.n == vb.n;
            r = e->op == TG_OP_EQ ? eq : e->op == TG_OP_NE ? !eq : false;
        } else {
            float a = va.n, b = vb.n;
            switch (e->op) {
            case TG_OP_LT: r = a < b; break;
            case TG_OP_GT: r = a > b; break;
            case TG_OP_EQ: r = fabsf(a - b) < 1e-4f; break;
            case TG_OP_LE: r = a <= b; break;
            case TG_OP_GE: r = a >= b; break;
            default: r = fabsf(a - b) >= 1e-4f; break;
            }
        }
        val_t v = { 1, r ? 1.0f : 0.0f, NULL };
        return v;
    }
    }
    return z;
}

static int exec(interp_t *I, const tg_node_t *s)
{
    if (!tick(I, s->line)) return X_HALT;
    switch (s->kind) {
    case TG_N_TO:
        I->proc[s->name] = s;
        return X_OK;
    case TG_N_CALL: {
        float n[2] = { 0 };
        const char *str[2] = { 0 };
        int k = 0;
        for (const tg_node_t *a = s->a; a && k < 2; a = a->next, k++) {
            val_t v = eval(I, a);
            if (I->halt) return X_HALT;
            if (v.t == 2) {
                str[k] = v.s;
            } else if (v.t == 0) {
                n[k] = v.n;
            } else {
                rt_err(I, s->line, _("acá va un número"));
                return X_HALT;
            }
        }
        if (!tg_world_call(I->w, s->op, n, str, k, s->line, NULL)) {
            I->halt = true;
            return X_HALT;
        }
        return X_OK;
    }
    case TG_N_PCALL:
        return call_proc(I, s, NULL);
    case TG_N_REPEAT: {
        float n = as_num(I, eval(I, s->a), s->line);
        if (I->halt) return X_HALT;
        for (int i = 0; i < (int)n; i++) {
            int r = exec_list(I, s->b);
            if (r != X_OK) return r;
            if (!tick(I, s->line)) return X_HALT;
        }
        return X_OK;
    }
    case TG_N_IF: {
        bool c = as_bool(I, eval(I, s->a), s->line);
        if (I->halt) return X_HALT;
        if (c) return exec_list(I, s->b);
        if (s->c) return exec_list(I, s->c);
        return X_OK;
    }
    case TG_N_WHILE:
        for (;;) {
            bool c = as_bool(I, eval(I, s->a), s->line);
            if (I->halt) return X_HALT;
            if (!c) return X_OK;
            int r = exec_list(I, s->b);
            if (r != X_OK) return r;
            if (!tick(I, s->line)) return X_HALT;
        }
    case TG_N_SET: {
        val_t v = eval(I, s->a);
        if (I->halt) return X_HALT;
        val_t *slot = var_slot(I, s->name, true);
        *slot = v;
        return X_OK;
    }
    case TG_N_STOP:
        I->ret.t = 3;
        return X_RETURN;
    case TG_N_OUTPUT:
        I->ret = eval(I, s->a);
        return I->halt ? X_HALT : X_RETURN;
    }
    return X_OK;
}

static int exec_list(interp_t *I, const tg_node_t *s)
{
    for (; s; s = s->next) {
        int r = exec(I, s);
        if (r != X_OK) return r;
    }
    return X_OK;
}

void tg_logo_run(tg_prog_t *p, tg_world_t *w)
{
    interp_t *I = tg_malloc(sizeof(interp_t));
    if (!I) {
        tg_world_fail(w, TG_F_RUNTIME, 0, _("no hay memoria"));
        return;
    }
    I->p = p;
    I->w = w;
    exec_list(I, p->prog);
    tg_free(I);
    tg_world_finish(w);
}

/* ------------------------------------------------------------------ pieces */

static int count(const tg_node_t *s)
{
    int n = 0;
    for (; s; s = s->next) {
        n++;
        if (s->kind == TG_N_TO || s->kind == TG_N_REPEAT || s->kind == TG_N_WHILE) n += count(s->b);
        if (s->kind == TG_N_IF) n += count(s->b) + count(s->c);
    }
    return n;
}

int tg_logo_pieces(const tg_prog_t *p)
{
    return count(p->prog);
}

static bool word_char(char c)
{
    return isalnum((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80;
}

int tg_text_pieces(const char *src, bool python)
{
    /* the tokens of the program, without comments and strings */
    static const char *const KW_PY[] = { "for", "while", "if", "elif", "def", "return", "break", NULL };
    static const char *const KW_LUA[] = { "for", "while", "if", "elseif", "function", "return", "break",
                                          "repeat", NULL };
    const char *const *kws = python ? KW_PY : KW_LUA;
    char defs[16][TG_NAME_LEN];
    int ndefs = 0;
    /* first the program's own functions */
    for (const char *p = src; *p; p++) {
        const char *kw = python ? "def" : "function";
        size_t kl = strlen(kw);
        if (!strncmp(p, kw, kl) && (p == src || !word_char(p[-1])) && (p[kl] == ' ' || p[kl] == '\t')) {
            const char *q = p + kl;
            while (*q == ' ' || *q == '\t') q++;
            int n = 0;
            while (word_char(*q) && n < TG_NAME_LEN - 1 && ndefs < 16) defs[ndefs][n++] = *q++;
            if (n && ndefs < 16) defs[ndefs++][n] = 0;
        }
    }
    int pieces = 0;
    char prev[TG_NAME_LEN] = "\n";
    bool in_for = false;
    const char *p = src;
    while (*p) {
        char tok[TG_NAME_LEN];
        int n = 0;
        if (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
            continue;
        }
        if ((python && *p == '#') || (!python && p[0] == '-' && p[1] == '-')) {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (*p && *p != q && *p != '\n') p++;
            if (*p == q) p++;
            snprintf(prev, sizeof(prev), "\"");
            continue;
        }
        if (word_char(*p)) {
            while (word_char(*p)) {
                if (n < TG_NAME_LEN - 1) tok[n++] = *p;
                p++;
            }
        } else if ((p[0] == '=' || p[0] == '~' || p[0] == '<' || p[0] == '>' || p[0] == '!' ||
                    p[0] == '+' || p[0] == '-' || p[0] == '*' || p[0] == '/') && p[1] == '=') {
            tok[n++] = p[0];
            tok[n++] = p[1];
            p += 2;
        } else {
            tok[n++] = *p++;
        }
        tok[n] = 0;
        /* the next significant token */
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        bool call = *q == '(';
        bool kw = false;
        for (int i = 0; kws[i]; i++) kw = kw || !strcmp(tok, kws[i]);
        if (kw) {
            pieces++;
            in_for = !strcmp(tok, "for");
        } else if ((!strcmp(tok, "=") || !strcmp(tok, "+=") || !strcmp(tok, "-=") || !strcmp(tok, "*=") ||
                    !strcmp(tok, "/=")) && !in_for) {
            pieces++;
        } else if (call && word_char(tok[0])) {
            int o = tg_order_find(tok);
            bool mine = false;
            for (int i = 0; i < ndefs; i++) mine = mine || !strcmp(tok, defs[i]);
            bool head = python ? (!strcmp(prev, "\n") || !strcmp(prev, ":") || !strcmp(prev, "else"))
                               : (!strcmp(prev, "\n") || !strcmp(prev, "do") || !strcmp(prev, "then") ||
                                  !strcmp(prev, "else") || !strcmp(prev, "end") || !strcmp(prev, ")") ||
                                  !strcmp(prev, ";"));
            if (((o >= 0 && !tg_orders[o].sense) || mine) && head) pieces++;
        }
        if (!strcmp(tok, "do") || !strcmp(tok, ":") || !strcmp(tok, "\n")) in_for = false;
        snprintf(prev, sizeof(prev), "%s", tok);
    }
    return pieces;
}

/* -------------------------------------------------------------- the text */

typedef struct {
    char *o;
    int len, at;
    int kind, lang;
    const tg_prog_t *p;
} gen_t;

static void put(gen_t *g, const char *fmt, ...)
{
    if (g->at >= g->len - 1) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g->o + g->at, (size_t)(g->len - g->at), fmt, ap);
    va_end(ap);
    if (n > 0) g->at += n;
    if (g->at > g->len - 1) g->at = g->len - 1;
}

static void indent(gen_t *g, int d)
{
    for (int i = 0; i < d; i++) put(g, g->kind == TG_GEN_PY ? "    " : "  ");
}

static const char *kw(const gen_t *g, const char *es, const char *en)
{
    return g->lang == TG_EN ? en : es;
}

static const char *oname(const gen_t *g, int o)
{
    const tg_order_t *t = &tg_orders[o];
    if (g->kind == TG_GEN_LOGO) return g->lang == TG_EN ? t->logo_en : t->logo_es;
    return g->lang == TG_EN ? t->en : t->es;
}

static void num_txt(gen_t *g, float n)
{
    if (n == floorf(n) && fabsf(n) < 1e7f) put(g, "%d", (int)n);
    else put(g, "%g", (double)n);
}

static void gexpr(gen_t *g, const tg_node_t *e, int prec);

static void args(gen_t *g, const tg_node_t *a)
{
    bool logo = g->kind == TG_GEN_LOGO;
    if (!logo) put(g, "(");
    for (int i = 0; a; a = a->next, i++) {
        if (logo) put(g, " ");
        else if (i) put(g, ", ");
        gexpr(g, a, logo ? 1 : 0);
    }
    if (!logo) put(g, ")");
}

static int prec_of(const tg_node_t *e)
{
    switch (e->kind) {
    case TG_E_OR: return 1;
    case TG_E_AND: return 2;
    case TG_E_NOT: return 3;
    case TG_E_CMP: return 4;
    case TG_E_BIN: return e->op == TG_OP_ADD || e->op == TG_OP_SUB ? 5 : 6;
    case TG_E_NEG: return 7;
    default: return 8;
    }
}

static void gexpr(gen_t *g, const tg_node_t *e, int prec)
{
    bool logo = g->kind == TG_GEN_LOGO, py = g->kind == TG_GEN_PY;
    int my = prec_of(e);
    bool par = my < prec;
    if (par) put(g, "(");
    switch (e->kind) {
    case TG_E_NUM: num_txt(g, e->num); break;
    case TG_E_STR: put(g, "\"%s\"", g->p->name[e->name]); break;
    case TG_E_BOOL:
        put(g, "%s", logo ? (e->num ? kw(g, "verdadero", "true") : kw(g, "falso", "false"))
                          : py ? (e->num ? "True" : "False") : (e->num ? "true" : "false"));
        break;
    case TG_E_VAR: put(g, "%s", g->p->name[e->name]); break;
    case TG_E_SENSE: put(g, "%s%s", oname(g, e->op), logo ? "" : "()"); break;
    case TG_E_FCALL:
        put(g, "%s", g->p->name[e->name]);
        args(g, e->a);
        break;
    case TG_E_NEG: put(g, "-"); gexpr(g, e->a, 7); break;
    case TG_E_NOT: put(g, "%s ", logo ? kw(g, "no", "not") : "not"); gexpr(g, e->a, 3); break;
    case TG_E_AND:
    case TG_E_OR:
        gexpr(g, e->a, my);
        put(g, " %s ", e->kind == TG_E_AND ? (logo ? kw(g, "y", "and") : "and") : (logo ? kw(g, "o", "or") : "or"));
        gexpr(g, e->b, my + 1);
        break;
    case TG_E_BIN:
    case TG_E_CMP: {
        static const char *const OPS[] = { "+", "-", "*", "/", "<", ">", "==", "<=", ">=", "!=" };
        const char *op = OPS[e->op];
        if (e->op == TG_OP_NE && !logo && !py) op = "~=";
        if (e->op == TG_OP_NE && logo) op = "<>";
        gexpr(g, e->a, my);
        put(g, " %s ", op);
        gexpr(g, e->b, my + 1);
        break;
    }
    }
    if (par) put(g, ")");
}

static void gbody(gen_t *g, const tg_node_t *s, int d);

static void gstmt(gen_t *g, const tg_node_t *s, int d)
{
    bool logo = g->kind == TG_GEN_LOGO, py = g->kind == TG_GEN_PY;
    indent(g, d);
    switch (s->kind) {
    case TG_N_CALL:
        put(g, "%s", oname(g, s->op));
        if (logo) {
            args(g, s->a);
        } else {
            args(g, s->a);
        }
        put(g, "\n");
        break;
    case TG_N_PCALL:
        put(g, "%s", g->p->name[s->name]);
        args(g, s->a);
        put(g, "\n");
        break;
    case TG_N_REPEAT:
        if (logo) {
            put(g, "%s ", kw(g, "repetir", "repeat"));
            gexpr(g, s->a, 1);
            put(g, " [\n");
            gbody(g, s->b, d + 1);
            indent(g, d);
            put(g, "]\n");
        } else if (py) {
            put(g, "for _ in range(");
            gexpr(g, s->a, 0);
            put(g, "):\n");
            gbody(g, s->b, d + 1);
        } else {
            put(g, "for i = 1, ");
            gexpr(g, s->a, 0);
            put(g, " do\n");
            gbody(g, s->b, d + 1);
            indent(g, d);
            put(g, "end\n");
        }
        break;
    case TG_N_IF: {
        const tg_node_t *e = s;
        bool first = true;
        while (e) {
            if (!first) indent(g, d);
            if (logo) {
                if (!first) put(g, "] %s ", kw(g, "sino", "else"));
                put(g, "%s ", kw(g, "si", "if"));
                gexpr(g, e->a, 1);
                put(g, " [\n");
            } else if (py) {
                put(g, first ? "if " : "elif ");
                gexpr(g, e->a, 0);
                put(g, ":\n");
            } else {
                put(g, first ? "if " : "elseif ");
                gexpr(g, e->a, 0);
                put(g, " then\n");
            }
            gbody(g, e->b, d + 1);
            first = false;
            const tg_node_t *c = e->c;
            if (c && !c->next && c->kind == TG_N_IF) {
                e = c;
                continue;
            }
            if (c) {
                indent(g, d);
                if (logo) put(g, "] %s [\n", kw(g, "sino", "else"));
                else put(g, py ? "else:\n" : "else\n");
                gbody(g, c, d + 1);
            }
            break;
        }
        if (!py) {      /* Python closes nothing */
            indent(g, d);
            put(g, logo ? "]\n" : "end\n");
        }
        break;
    }
    case TG_N_WHILE:
        if (logo) {
            put(g, "%s ", kw(g, "mientras", "while"));
            gexpr(g, s->a, 1);
            put(g, " [\n");
            gbody(g, s->b, d + 1);
            indent(g, d);
            put(g, "]\n");
        } else if (py) {
            put(g, "while ");
            gexpr(g, s->a, 0);
            put(g, ":\n");
            gbody(g, s->b, d + 1);
        } else {
            put(g, "while ");
            gexpr(g, s->a, 0);
            put(g, " do\n");
            gbody(g, s->b, d + 1);
            indent(g, d);
            put(g, "end\n");
        }
        break;
    case TG_N_SET:
        if (!logo && !py && d == 0 && s->a) {
            /* the first time a top-level name is set in Lua: local */
            bool before = false;
            for (const tg_node_t *q = g->p->prog; q && q != s; q = q->next) {
                if (q->kind == TG_N_SET && q->name == s->name) before = true;
            }
            if (!before) put(g, "local ");
        }
        put(g, "%s = ", g->p->name[s->name]);
        gexpr(g, s->a, 0);
        put(g, "\n");
        break;
    case TG_N_TO: {
        if (logo) put(g, "%s %s", kw(g, "para", "to"), g->p->name[s->name]);
        else put(g, "%s %s(", py ? "def" : "function", g->p->name[s->name]);
        int i = 0;
        for (const tg_node_t *pa = s->a; pa; pa = pa->next, i++) {
            if (logo) put(g, " %s", g->p->name[pa->name]);
            else put(g, "%s%s", i ? ", " : "", g->p->name[pa->name]);
        }
        put(g, logo ? "\n" : py ? "):\n" : ")\n");
        gbody(g, s->b, d + 1);
        if (!py) {
            indent(g, d);
            put(g, "%s\n", logo ? kw(g, "fin", "end") : "end");
        }
        if (d == 0) put(g, "\n");
        break;
    }
    case TG_N_STOP:
        put(g, "%s\n", logo ? kw(g, "alto", "stop") : "return");
        break;
    case TG_N_OUTPUT:
        put(g, "%s ", logo ? kw(g, "devolver", "output") : "return");
        gexpr(g, s->a, 0);
        put(g, "\n");
        break;
    }
}

static void gbody(gen_t *g, const tg_node_t *s, int d)
{
    if (!s && g->kind == TG_GEN_PY) {
        indent(g, d);
        put(g, "pass\n");
    }
    for (; s; s = s->next) gstmt(g, s, d);
}

int tg_logo_gen(const tg_prog_t *p, int kind, int lang, char *out, int len)
{
    gen_t g = { out, len, 0, kind, lang, p };
    if (len > 0) out[0] = 0;
    gbody(&g, p->prog, 0);
    while (g.at > 0 && out[g.at - 1] == '\n') out[--g.at] = 0;
    return g.at;
}
