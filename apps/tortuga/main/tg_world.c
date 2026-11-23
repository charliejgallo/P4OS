/*
 * Tortuga - the orders, the levels (the .tl format of DESIGN.md, 3.1) and
 * the world the turtle walks: the rules of DESIGN.md, 3.2 and 3.3, the same
 * as World in tools/tg_ref.py.
 */
#include "tg_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const tg_order_t tg_orders[TG_O_N] = {
    [TG_O_FORWARD]  = { "avanzar", "forward", "avanzar", "forward", { "av", "fd" }, 0, 0, 1 },
    [TG_O_BACK]     = { "retroceder", "back", "retroceder", "back", { "re", "bk" }, 0, 0, 1 },
    [TG_O_RIGHT]    = { "girar_derecha", "turn_right", "girar derecha", "turn right",
                        { "derecha", "right", "de", "rt" }, 0, 0, 1 },
    [TG_O_LEFT]     = { "girar_izquierda", "turn_left", "girar izquierda", "turn left",
                        { "izquierda", "left", "iz", "lt" }, 0, 0, 1 },
    [TG_O_EAT]      = { "comer", "eat", "comer", "eat", { 0 }, 0, 0, 0 },
    [TG_O_SAY]      = { "decir", "say", "decir", "say", { 0 }, 0, 1, 1 },
    [TG_O_PENDOWN]  = { "bajar_lapiz", "pen_down", "bajar_lapiz", "pen_down", { "bl", "pd" }, 0, 0, 0 },
    [TG_O_PENUP]    = { "subir_lapiz", "pen_up", "subir_lapiz", "pen_up", { "sl", "pu" }, 0, 0, 0 },
    [TG_O_COLOR]    = { "color", "color", "color", "color", { 0 }, 0, 1, 1 },
    [TG_S_PATH_AHEAD]  = { "camino_adelante", "path_ahead", "camino_adelante", "path_ahead", { 0 }, 1, 0, 0 },
    [TG_S_PATH_RIGHT]  = { "camino_derecha", "path_right", "camino_derecha", "path_right", { 0 }, 1, 0, 0 },
    [TG_S_PATH_LEFT]   = { "camino_izquierda", "path_left", "camino_izquierda", "path_left", { 0 }, 1, 0, 0 },
    [TG_S_WATER_AHEAD] = { "agua_adelante", "water_ahead", "agua_adelante", "water_ahead", { 0 }, 1, 0, 0 },
    [TG_S_LETTUCE]     = { "hay_lechuga", "on_lettuce", "hay_lechuga", "on_lettuce", { 0 }, 1, 0, 0 },
    [TG_S_AT_GOAL]     = { "en_meta", "at_goal", "en_meta", "at_goal", { 0 }, 1, 0, 0 },
    [TG_S_HAS_KEY]     = { "tengo_llave", "has_key", "tengo_llave", "has_key", { 0 }, 1, 0, 0 },
};

int tg_order_find(const char *name)
{
    for (int i = 0; i < TG_O_N; i++) {
        const tg_order_t *o = &tg_orders[i];
        if (!strcmp(name, o->es) || !strcmp(name, o->en)) return i;
        for (int k = 0; k < 4; k++) {
            if (o->alias[k] && !strcmp(name, o->alias[k])) return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------ levels */

static int lang_of(const char *s, const char **rest)
{
    static const char *const L[TG_NL] = { "es", "en", "de" };
    for (int i = 0; i < TG_NL; i++) {
        size_t n = strlen(L[i]);
        if (!strncmp(s, L[i], n) && s[n] == ':') {
            s += n + 1;
            while (*s == ' ') s++;
            *rest = s;
            return i;
        }
    }
    return -1;
}

static void trim_end(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
}

static int dir_word(const char *s)
{
    if (!strcmp(s, "norte") || !strcmp(s, "north")) return TG_DIR_N;
    if (!strcmp(s, "este") || !strcmp(s, "east")) return TG_DIR_E;
    if (!strcmp(s, "sur") || !strcmp(s, "south")) return TG_DIR_S;
    return TG_DIR_W;
}

bool tg_levels_parse(tg_levels_t *s, const char *text, size_t len)
{
    memset(s, 0, sizeof(*s));
    int cap = 1;
    for (size_t i = 0; i + 6 < len; i++) {
        if (text[i] == '@' && (i == 0 || text[i - 1] == '\n') && !strncmp(text + i, "@nivel", 6)) cap++;
    }
    s->buf = tg_malloc(len + 2);
    s->lv = tg_malloc(sizeof(tg_level_t) * (size_t)cap);
    if (!s->buf || !s->lv) {
        tg_levels_free(s);
        return false;
    }
    memcpy(s->buf, text, len);
    s->buf[len] = 0;

    tg_level_t *cur = NULL;
    enum { S_NONE, S_MAP, S_BLOCK } sect = S_NONE;
    const char **block = NULL;      /* where a multi-line section goes */
    char *block_start = NULL;
    int hint_n[TG_NL] = { 0 };
    char *p = s->buf;
    while (*p) {
        char *line = p;
        char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
        bool at = line[0] == '@';
        if (at && sect == S_BLOCK && block) {
            /* the block ends before this line */
            line[-1] = 0;
            trim_end(block_start);
            *block = block_start;
            block = NULL;
        }
        if (at) sect = S_NONE;
        if (at && line[1] == '-' && line[2] == '-') continue;
        if (at) {
            if (nl) *nl = 0;
            trim_end(line);
            char *key = line + 1;
            char *val = strchr(key, ' ');
            if (val) {
                *val++ = 0;
                while (*val == ' ') val++;
            } else {
                val = key + strlen(key);
            }
            if (!strcmp(key, "nivel")) {
                cur = &s->lv[s->n++];
                memset(cur, 0, sizeof(*cur));
                cur->id = val;
                cur->world = atoi(val);
                const char *d = strchr(val, '-');
                cur->num = d ? atoi(d + 1) : 0;
                memset(hint_n, 0, sizeof(hint_n));
                continue;
            }
            if (!cur) continue;
            const char *rest;
            if (!strcmp(key, "titulo")) {
                int l = lang_of(val, &rest);
                if (l >= 0) cur->title[l] = rest;
            } else if (!strcmp(key, "pista")) {
                int l = lang_of(val, &rest);
                if (l >= 0 && hint_n[l] < TG_HINTS) {
                    cur->hint[hint_n[l]++][l] = rest;
                    if (hint_n[l] > cur->nhint) cur->nhint = hint_n[l];
                }
            } else if (!strcmp(key, "ordenes")) {
                char *t = val;
                while (*t) {
                    char *e = t;
                    while (*e && *e != ' ') e++;
                    char c = *e;
                    *e = 0;
                    int o = tg_order_find(t);
                    if (o >= 0) cur->orders |= 1u << o;
                    if (!c) break;
                    t = e + 1;
                    while (*t == ' ') t++;
                }
            } else if (!strcmp(key, "meta")) {
                if (strstr(val, "llegar")) cur->goals |= TG_G_REACH;
                if (strstr(val, "decir=lechugas")) cur->goals |= TG_G_SAY_LETTUCE;
                else if (strstr(val, "lechugas")) cur->goals |= TG_G_LETTUCE;
                if (strstr(val, "dibujo")) cur->goals |= TG_G_DRAW;
            } else if (!strcmp(key, "estrellas")) {
                int a = 0, b = 0;
                sscanf(val, "%d %d", &a, &b);
                cur->star3 = (uint8_t)a;
                cur->star2 = (uint8_t)b;
            } else if (!strcmp(key, "inicio")) {
                char d[16] = "norte";
                float x = 0, y = 0;
                sscanf(val, "%f %f %15s", &x, &y, d);
                cur->has_start = true;
                cur->sx = x;
                cur->sy = y;
                cur->sdir = (uint8_t)dir_word(d);
            } else if (!strcmp(key, "mapa")) {
                if (cur->nmap < TG_MAXVAR) {
                    sect = S_MAP;
                    cur->nmap++;
                }
            } else {
                const char **dst = NULL;
                if (!strcmp(key, "figura")) dst = &cur->figure;
                else if (!strcmp(key, "logo")) dst = &cur->sol[TG_SOL_LOGO];
                else if (!strcmp(key, "lua")) dst = &cur->sol[TG_SOL_LUA];
                else if (!strcmp(key, "python")) dst = &cur->sol[TG_SOL_PY];
                if (dst) {
                    sect = S_BLOCK;
                    block = dst;
                    block_start = p;
                }
            }
            continue;
        }
        if (sect == S_MAP && cur) {
            if (nl) *nl = 0;
            trim_end(line);
            int m = cur->nmap - 1;
            if (!line[0]) {
                sect = S_NONE;      /* a blank line ends the map */
                continue;
            }
            if (cur->h[m] < TG_MAXH) {
                cur->row[m][cur->h[m]++] = line;
                int wl = (int)strlen(line);
                if (wl > TG_MAXW) wl = TG_MAXW;
                if (wl > cur->w[m]) cur->w[m] = (uint8_t)wl;
            }
        }
    }
    if (sect == S_BLOCK && block) {
        trim_end(block_start);
        *block = block_start;
    }
    return true;
}

void tg_levels_free(tg_levels_t *s)
{
    tg_free(s->buf);
    tg_free(s->lv);
    memset(s, 0, sizeof(*s));
}

const char *tg_level_text(const char *const *by_lang, int lang)
{
    if (lang >= 0 && lang < TG_NL && by_lang[lang]) return by_lang[lang];
    if (by_lang[TG_EN] && lang == TG_DE) return by_lang[TG_EN];
    return by_lang[TG_ES] ? by_lang[TG_ES] : "";
}

/* ------------------------------------------------------------------- world */

static const int DX[4] = { 0, 1, 0, -1 };
static const int DY[4] = { -1, 0, 1, 0 };

static bool walkable(char c)
{
    return c == '.' || c == '=' || c == 'L' || c == 'k' || c == 'M' || c == 'p';
}

static char tile(const tg_world_t *w, int x, int y)
{
    if (x < 0 || y < 0 || x >= w->w || y >= w->h) return ' ';
    return w->t[y][x];
}

static bool tile_free(const tg_world_t *w, int x, int y)
{
    char c = tile(w, x, y);
    return walkable(c) || (c == 'P' && w->keys > 0);
}

void tg_world_init(tg_world_t *w, const tg_level_t *lv, int variant, bool hard, bool paper,
                   tg_ev_t *ev, int cap)
{
    memset(w, 0, sizeof(*w));
    w->lv = lv;
    w->variant = variant;
    w->hard = hard;
    w->paper = paper;
    w->ev = ev;
    w->cap = cap;
    w->state = TG_ST_RUN;
    w->color = 0;
    if (paper) {
        w->x = lv->has_start ? lv->sx : 0;
        w->y = lv->has_start ? lv->sy : 0;
        w->hd = 90.0f * (float)(lv->has_start ? lv->sdir : TG_DIR_N);
        w->pen = true;
        return;
    }
    if (variant >= lv->nmap) variant = 0;
    w->w = lv->w[variant];
    w->h = lv->h[variant];
    for (int y = 0; y < w->h; y++) {
        const char *r = lv->row[variant][y];
        int n = (int)strlen(r);
        for (int x = 0; x < w->w; x++) {
            char c = x < n ? r[x] : ' ';
            if (c == '^' || c == '>' || c == 'v' || c == '<') {
                w->tx = x;
                w->ty = y;
                w->dir = c == '^' ? TG_DIR_N : c == '>' ? TG_DIR_E : c == 'v' ? TG_DIR_S : TG_DIR_W;
                c = '.';
            } else if (c == 'L') {
                w->lettuce_total++;
            }
            w->t[y][x] = c;
        }
    }
    if (lv->has_start) {
        w->tx = (int)lv->sx;
        w->ty = (int)lv->sy;
        w->dir = lv->sdir;
    }
}

static tg_ev_t *push(tg_world_t *w, int kind)
{
    static tg_ev_t spare;
    if (w->nev >= w->cap) {
        memset(&spare, 0, sizeof(spare));
        return &spare;
    }
    tg_ev_t *e = &w->ev[w->nev++];
    memset(e, 0, sizeof(*e));
    e->kind = (uint8_t)kind;
    e->line = (int16_t)w->line;
    if (w->paper) {
        e->x0 = e->x1 = w->x;
        e->y0 = e->y1 = w->y;
        e->h0 = e->h1 = w->hd;
    } else {
        e->x0 = e->x1 = (float)w->tx;
        e->y0 = e->y1 = (float)w->ty;
        e->h0 = e->h1 = 90.0f * (float)w->dir;
    }
    e->pen = w->pen;
    e->color = w->color;
    return e;
}

void tg_world_fail(tg_world_t *w, int reason, int line, const char *msg)
{
    if (w->state != TG_ST_RUN) return;
    w->state = TG_ST_FAIL;
    w->reason = reason;
    w->fail_line = line;
    if (msg) snprintf(w->msg, sizeof(w->msg), "%s", msg);
    tg_ev_t *e = push(w, TG_EV_FAIL);
    e->a = (uint8_t)reason;
    e->line = (int16_t)line;
}

static void win(tg_world_t *w)
{
    w->state = TG_ST_WIN;
    push(w, TG_EV_WIN);
}

static bool goals_met(const tg_world_t *w)
{
    const tg_level_t *lv = w->lv;
    if ((lv->goals & TG_G_REACH) && tile(w, w->tx, w->ty) != 'M') return false;
    if (lv->goals & TG_G_LETTUCE) {
        for (int y = 0; y < w->h; y++)
            for (int x = 0; x < w->w; x++)
                if (w->t[y][x] == 'L') return false;
    }
    if ((lv->goals & TG_G_SAY_LETTUCE) && (!w->said || w->said_num != (float)w->lettuce_total)) return false;
    return true;
}

static bool check_win(tg_world_t *w)
{
    if (!w->hard && goals_met(w)) {
        win(w);
        return false;
    }
    return true;
}

int tg_world_steps_to_goal(const tg_world_t *w)
{
    /* a breadth-first walk over what can be stepped on, keys aside */
    int16_t dist[TG_MAXH][TG_MAXW];
    int qx[TG_MAXW * TG_MAXH], qy[TG_MAXW * TG_MAXH], qh = 0, qt = 0;
    for (int y = 0; y < TG_MAXH; y++)
        for (int x = 0; x < TG_MAXW; x++) dist[y][x] = -1;
    dist[w->ty][w->tx] = 0;
    qx[qt] = w->tx;
    qy[qt++] = w->ty;
    while (qh < qt) {
        int x = qx[qh], y = qy[qh++];
        if (w->t[y][x] == 'M') return dist[y][x];
        for (int d = 0; d < 4; d++) {
            int nx = x + DX[d], ny = y + DY[d];
            char c = tile(w, nx, ny);
            if ((walkable(c) || c == 'P') && dist[ny][nx] < 0) {
                dist[ny][nx] = (int16_t)(dist[y][x] + 1);
                qx[qt] = nx;
                qy[qt++] = ny;
            }
        }
    }
    return -1;
}

static bool call_grid(tg_world_t *w, int o, float n, bool has_n, bool *out)
{
    switch (o) {
    case TG_O_FORWARD:
    case TG_O_BACK: {
        if (!has_n) n = 1;
        if (n != floorf(n) || n < 0) {
            tg_world_fail(w, TG_F_WHOLE, w->line, NULL);
            return false;
        }
        int d = o == TG_O_BACK ? (w->dir + 2) & 3 : w->dir;
        for (int i = 0; i < (int)n; i++) {
            int x = w->tx + DX[d], y = w->ty + DY[d];
            char c = tile(w, x, y);
            if (c == ' ' || c == '#' || (c == 'P' && !w->keys)) {
                tg_ev_t *e = push(w, TG_EV_BUMP);
                e->x1 = (float)x;
                e->y1 = (float)y;
                int why = c == ' ' ? TG_F_EDGE : c == '#' ? TG_F_ROCK : TG_F_DOOR;
                e->a = (uint8_t)why;
                tg_world_fail(w, why, w->line, NULL);
                return false;
            }
            if (c == '~') {
                tg_ev_t *e = push(w, TG_EV_SPLASH);
                e->x1 = (float)x;
                e->y1 = (float)y;
                tg_world_fail(w, TG_F_WATER, w->line, NULL);
                return false;
            }
            if (c == 'P') {
                w->keys--;
                w->t[y][x] = 'p';
                tg_ev_t *e = push(w, TG_EV_DOOR);
                e->x1 = (float)x;
                e->y1 = (float)y;
            }
            tg_ev_t *e = push(w, TG_EV_MOVE);
            e->x1 = (float)x;
            e->y1 = (float)y;
            w->tx = x;
            w->ty = y;
            if (c == 'k') {
                w->keys++;
                w->t[y][x] = '.';
                tg_ev_t *k = push(w, TG_EV_KEY);
                k->x1 = (float)x;
                k->y1 = (float)y;
            }
            if (c == 'M') w->passed_goal = true;
            if (!check_win(w)) return false;
        }
        return true;
    }
    case TG_O_RIGHT:
    case TG_O_LEFT: {
        if (!has_n) n = 90;
        if (fmodf(n, 90.0f) != 0) {
            tg_world_fail(w, TG_F_TURN90, w->line, NULL);
            return false;
        }
        int q = (int)(n / 90.0f) * (o == TG_O_RIGHT ? 1 : -1);
        tg_ev_t *e = push(w, TG_EV_TURN);
        w->dir = ((w->dir + q) % 4 + 4) % 4;
        e->h1 = e->h0 + (float)(q * 90);
        return true;
    }
    case TG_O_EAT:
        if (w->t[w->ty][w->tx] != 'L') {
            push(w, TG_EV_CHEW);
            return true;
        }
        push(w, TG_EV_EAT);
        w->t[w->ty][w->tx] = '.';
        return check_win(w);
    case TG_O_PENDOWN:
    case TG_O_PENUP:
    case TG_O_COLOR:
        return true;
    case TG_S_PATH_AHEAD:
        *out = tile_free(w, w->tx + DX[w->dir], w->ty + DY[w->dir]);
        return true;
    case TG_S_PATH_RIGHT: {
        int d = (w->dir + 1) & 3;
        *out = tile_free(w, w->tx + DX[d], w->ty + DY[d]);
        return true;
    }
    case TG_S_PATH_LEFT: {
        int d = (w->dir + 3) & 3;
        *out = tile_free(w, w->tx + DX[d], w->ty + DY[d]);
        return true;
    }
    case TG_S_WATER_AHEAD:
        *out = tile(w, w->tx + DX[w->dir], w->ty + DY[w->dir]) == '~';
        return true;
    case TG_S_LETTUCE:
        *out = w->t[w->ty][w->tx] == 'L';
        return true;
    case TG_S_AT_GOAL:
        *out = w->t[w->ty][w->tx] == 'M';
        return true;
    case TG_S_HAS_KEY:
        *out = w->keys > 0;
        return true;
    }
    return true;
}

static bool call_paper(tg_world_t *w, int o, float n, bool has_n, const char *str, bool *out)
{
    switch (o) {
    case TG_O_FORWARD:
    case TG_O_BACK: {
        if (!has_n) n = 1;
        if (o == TG_O_BACK) n = -n;
        float r = w->hd * 3.14159265f / 180.0f;
        float nx = w->x + n * sinf(r), ny = w->y + n * cosf(r);
        if (fabsf(nx) > TG_PAPER + 0.01f || fabsf(ny) > TG_PAPER + 0.01f) {
            tg_world_fail(w, TG_F_OFF_PAPER, w->line, NULL);
            return false;
        }
        tg_ev_t *e = push(w, TG_EV_MOVE);
        e->x1 = nx;
        e->y1 = ny;
        w->x = nx;
        w->y = ny;
        return true;
    }
    case TG_O_RIGHT:
    case TG_O_LEFT: {
        if (!has_n) n = 90;
        tg_ev_t *e = push(w, TG_EV_TURN);
        float d = o == TG_O_RIGHT ? n : -n;
        e->h1 = e->h0 + d;
        w->hd = fmodf(w->hd + d, 360.0f);
        if (w->hd < 0) w->hd += 360.0f;
        return true;
    }
    case TG_O_PENDOWN:
    case TG_O_PENUP: {
        w->pen = o == TG_O_PENDOWN;
        tg_ev_t *e = push(w, TG_EV_PEN);
        e->pen = w->pen;
        return true;
    }
    case TG_O_COLOR: {
        static const char *const C[][2] = {
            { "negro", "black" }, { "rojo", "red" }, { "azul", "blue" }, { "verde", "green" },
            { "amarillo", "yellow" }, { "naranja", "orange" }, { "violeta", "purple" },
            { "rosa", "pink" }, { "marron", "brown" }, { "blanco", "white" },
        };
        int c = has_n ? (int)n : 0;
        if (str) {
            for (int i = 0; i < (int)(sizeof(C) / sizeof(C[0])); i++) {
                if (!strcmp(str, C[i][0]) || !strcmp(str, C[i][1])) c = i;
            }
        }
        if (c < 0) c = 0;
        w->color = (uint8_t)(c % 10);
        tg_ev_t *e = push(w, TG_EV_COLOR);
        e->color = w->color;
        return true;
    }
    case TG_O_EAT:
        push(w, TG_EV_CHEW);
        return true;
    default:
        if (tg_orders[o].sense) {
            *out = false;
            tg_world_fail(w, TG_F_NOT_HERE, w->line, tg_orders[o].es);
            return false;
        }
        return true;
    }
}

bool tg_world_call(tg_world_t *w, int o, const float *num, const char *const *str, int nargs,
                   int line, bool *out)
{
    bool dummy;
    if (!out) out = &dummy;
    *out = false;
    if (w->state != TG_ST_RUN) return false;
    w->line = line;
    if (++w->actions > TG_MAX_ACTIONS) {
        tg_world_fail(w, TG_F_LOOP, line, NULL);
        return false;
    }
    if (w->lv->orders && !(w->lv->orders & (1u << o))) {
        tg_world_fail(w, TG_F_NOT_YET, line, tg_orders[o].es);
        return false;
    }
    const tg_order_t *spec = &tg_orders[o];
    if (nargs < spec->amin || nargs > spec->amax) {
        tg_world_fail(w, TG_F_ARGS, line, spec->es);
        return false;
    }
    bool has_n = nargs > 0 && !(str && str[0]);
    float n = has_n ? num[0] : 0;
    const char *s = nargs > 0 && str ? str[0] : NULL;
    if (o == TG_O_SAY) {
        tg_ev_t *e = push(w, TG_EV_SAY);
        e->val = n;
        e->str = s;
        w->said = !s;
        w->said_num = n;
        return w->paper ? true : check_win(w);
    }
    bool ok = w->paper ? call_paper(w, o, n, has_n, s, out) : call_grid(w, o, n, has_n, out);
    if (ok && spec->sense) {
        tg_ev_t *e = push(w, TG_EV_SENSE);
        e->a = (uint8_t)o;
        e->val = *out ? 1.0f : 0.0f;
    }
    return ok;
}

void tg_world_finish(tg_world_t *w)
{
    if (w->state != TG_ST_RUN) return;
    w->line = 0;
    if (w->paper) {
        win(w);         /* the app compares the drawing afterwards */
        return;
    }
    if (goals_met(w)) {
        win(w);
        return;
    }
    const tg_level_t *lv = w->lv;
    if ((lv->goals & TG_G_REACH) && w->t[w->ty][w->tx] != 'M') {
        if (w->hard && w->passed_goal) {
            tg_world_fail(w, TG_F_PASSED, 0, NULL);
            return;
        }
        w->arg = tg_world_steps_to_goal(w);
        tg_world_fail(w, TG_F_SHORT, 0, NULL);
        return;
    }
    if (lv->goals & TG_G_LETTUCE) {
        int left = 0;
        for (int y = 0; y < w->h; y++)
            for (int x = 0; x < w->w; x++) left += w->t[y][x] == 'L';
        if (left) {
            w->arg = left;
            tg_world_fail(w, TG_F_LETTUCE_LEFT, 0, NULL);
            return;
        }
    }
    tg_world_fail(w, TG_F_SAID_WRONG, 0, NULL);
}

/* ---------------------------------------------------------- the drawing */

static float seg_dist(float px, float py, const tg_ev_t *s)
{
    float dx = s->x1 - s->x0, dy = s->y1 - s->y0;
    float l2 = dx * dx + dy * dy;
    float t = l2 == 0 ? 0 : ((px - s->x0) * dx + (py - s->y0) * dy) / l2;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    float ex = px - (s->x0 + t * dx), ey = py - (s->y0 + t * dy);
    return sqrtf(ex * ex + ey * ey);
}

static bool inked(const tg_ev_t *e)
{
    return e->kind == TG_EV_MOVE && e->pen;
}

/* every point of a's ink is within tol of b's ink */
static bool covered(const tg_world_t *a, const tg_world_t *b, float tol)
{
    for (int i = 0; i < a->nev; i++) {
        const tg_ev_t *s = &a->ev[i];
        if (!inked(s)) continue;
        float len = sqrtf((s->x1 - s->x0) * (s->x1 - s->x0) + (s->y1 - s->y0) * (s->y1 - s->y0));
        int n = (int)(len / 2.0f);
        if (n < 1) n = 1;
        for (int k = 0; k <= n; k++) {
            float px = s->x0 + (s->x1 - s->x0) * (float)k / (float)n;
            float py = s->y0 + (s->y1 - s->y0) * (float)k / (float)n;
            bool near = false;
            for (int j = 0; j < b->nev && !near; j++) {
                if (inked(&b->ev[j]) && seg_dist(px, py, &b->ev[j]) <= tol) near = true;
            }
            if (!near) return false;
        }
    }
    return true;
}

bool tg_world_same_drawing(const tg_world_t *drawn, const tg_world_t *figure)
{
    bool any = false;
    for (int i = 0; i < drawn->nev; i++) any = any || inked(&drawn->ev[i]);
    return any && covered(figure, drawn, 3.0f) && covered(drawn, figure, 3.0f);
}
