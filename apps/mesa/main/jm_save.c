/*
 * Juegos de mesa - what is kept on the card.
 *
 * The game left open: mesa_partida.bin in the data folder, written after
 * every move (it is small: its options and its list), read by "Seguir la
 * partida". A game that ended is forgotten.
 *
 * PGN: a chess game written to and read from /sdcard/mesa/ajedrez/, the
 * moves in SAN as the game writes them. Reading one plays it from the
 * start (or its FEN), each move found among the legal ones by its text.
 */
#include "jm_app.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define P (&jm->play)
#define MAGIC 0x3153474Du      /* "MGS1" */

typedef struct {
    uint32_t magic;
    uint16_t hist_size, move_size;
    char     word[16];
    uint8_t  opts[MESA_MAX_OPTS];
    uint32_t seed;
    char     fen[100];
    uint8_t  np, two, bottom, over;
    uint8_t  human[MESA_MAX_PLAYERS];
    uint8_t  level[MESA_MAX_PLAYERS];
    int32_t  clock_mode, clock_base, clock_inc, left[2];
    int32_t  n, top;
} save_t;

static void save_path(char *out, size_t n)
{
    snprintf(out, n, "%s/mesa_partida.bin", aos_hal_path_data());
}

void jm_save_forget(void)
{
    char p[160];
    save_path(p, sizeof p);
    remove(p);
}

bool jm_save_game(void)
{
    if (!jm->playing || !P->g) return false;
    if (P->over) {
        jm_save_forget();
        return true;
    }
    if (P->n == 0) {
        /* nothing to keep yet; an old game stays until this one moves */
        return true;
    }
    save_t h;
    memset(&h, 0, sizeof h);
    h.magic = MAGIC;
    h.hist_size = (uint16_t)sizeof(jm_hist_t);
    h.move_size = (uint16_t)sizeof(mesa_move_t);
    snprintf(h.word, sizeof h.word, "%s", P->g->word);
    memcpy(h.opts, P->opts, sizeof h.opts);
    h.seed = P->seed;
    memcpy(h.fen, P->fen, sizeof h.fen);
    h.np = (uint8_t)P->np;
    h.two = (uint8_t)P->two;
    h.bottom = (uint8_t)P->bottom;
    memcpy(h.human, P->human, sizeof h.human);
    memcpy(h.level, P->level, sizeof h.level);
    h.clock_mode = P->clock.mode;
    h.clock_base = P->clock.base_ms;
    h.clock_inc = P->clock.inc_ms;
    h.left[0] = P->clock.left[0];
    h.left[1] = P->clock.left[1];
    h.n = P->n;
    h.top = P->top;
    char p[160], tmp[168];
    save_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 &&
              fwrite(P->hist, sizeof(jm_hist_t), (size_t)P->top, f) == (size_t)P->top;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        remove(tmp);
        return false;
    }
    remove(p);
    return rename(tmp, p) == 0;
}

bool jm_save_load(jm_new_t *n, bool apply)
{
    char p[160];
    save_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    save_t h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == MAGIC && h.hist_size == sizeof(jm_hist_t) &&
              h.move_size == sizeof(mesa_move_t) && h.n > 0 && h.n <= JM_HIST_MAX && h.top >= h.n &&
              h.top <= JM_HIST_MAX;
    h.word[sizeof h.word - 1] = 0;
    const jm_slot_t *s = ok ? jm_game_find(h.word) : NULL;
    if (!s) {
        fclose(f);
        return false;
    }
    memset(n, 0, sizeof *n);
    n->gi = (int)(s - jm_games);
    memcpy(n->opts, h.opts, sizeof n->opts);
    memcpy(n->human, h.human, sizeof n->human);
    memcpy(n->level, h.level, sizeof n->level);
    n->two = h.two;
    n->clock_mode = h.clock_mode;
    n->clock_base = h.clock_base;
    n->clock_inc = h.clock_inc;
    if (!apply) {
        fclose(f);
        return true;
    }
    jm_hist_t *hist = (jm_hist_t *)jm_malloc(sizeof(jm_hist_t) * (size_t)h.top);
    ok = hist && fread(hist, sizeof(jm_hist_t), (size_t)h.top, f) == (size_t)h.top;
    fclose(f);
    h.fen[sizeof h.fen - 1] = 0;
    n->fen = h.fen[0] ? h.fen : NULL;
    if (!ok || !jm_play_start(n)) {
        jm_free(hist);
        return false;
    }
    if (jm->home) lv_obj_add_flag(jm->home, LV_OBJ_FLAG_HIDDEN);
    jm_play_ai_cancel();
    P->seed = h.seed;
    memcpy(P->hist, hist, sizeof(jm_hist_t) * (size_t)h.top);
    jm_free(hist);
    P->top = h.top;
    jm_play_replay(h.n);
    P->top = h.top;
    if (P->clock.mode != CLK_NONE) {
        P->clock.left[0] = h.left[0];
        P->clock.left[1] = h.left[1];
    }
    jm_play_relayout();
    P->wait_until = jm_now() + 600;
    return true;
}

/* ---- PGN ---------------------------------------------------------------------------------- */

static void pgn_dir(char *out, size_t n)
{
    const char *root = aos_hal_path_sd_root();
    snprintf(out, n, "%s/mesa/ajedrez", root ? root : aos_hal_path_data());
}

static void mkdirs(const char *path)
{
    char b[200];
    snprintf(b, sizeof b, "%s", path);
    for (char *s = b + 1; *s; s++) {
        if (*s != '/') continue;
        *s = 0;
        mkdir(b, 0777);
        *s = '/';
    }
    mkdir(b, 0777);
}

bool jm_pgn_write(char *name_out, size_t nn)
{
    if (!jm->playing || !(P->g->flags & MESA_GF_PGN)) return false;
    char dir[180];
    pgn_dir(dir, sizeof dir);
    mkdirs(dir);
    char date[40] = "????.??.??", name[96];
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    if (aos_hal_time_is_valid()) {
        aos_hal_time_now(&tm);
        snprintf(date, sizeof date, "%04d.%02d.%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        snprintf(name, sizeof name, "partida-%04d%02d%02d-%02d%02d.pgn", tm.tm_year + 1900, tm.tm_mon + 1,
                 tm.tm_mday, tm.tm_hour, tm.tm_min);
    } else {
        snprintf(name, sizeof name, "partida-%u.pgn", (unsigned)(jm_rand() % 100000));
    }
    char path[300];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "w");
    if (!f) return false;
    const char *who[2];
    for (int i = 0; i < 2; i++) who[i] = P->human[i] ? "Persona" : "P4OS";
    const char *why;
    int r = P->g->result(P->st, &why);
    if (P->resigned >= 0) r = 1 - P->resigned;
    if (P->clock.flagged >= 0) r = 1 - P->clock.flagged;
    const char *res = r == 0 ? "1-0" : r == 1 ? "0-1" : r == -2 ? "1/2-1/2" : "*";
    char line[160];
    snprintf(line, sizeof line, "[Event \"Juegos de mesa\"]\n[Site \"P4OS\"]\n[Date \"%s\"]\n[Round \"-\"]\n", date);
    fputs(line, f);
    snprintf(line, sizeof line, "[White \"%s\"]\n[Black \"%s\"]\n[Result \"%s\"]\n", who[0], who[1], res);
    fputs(line, f);
    if (P->fen[0]) {
        snprintf(line, sizeof line, "[SetUp \"1\"]\n[FEN \"%s\"]\n", P->fen);
        fputs(line, f);
    }
    fputs("\n", f);
    P->g->init(P->tmp, P->opts, P->seed);
    if (P->fen[0] && P->g->setup) P->g->setup(P->tmp, P->opts, P->fen);
    int col = 0, moveno = 1;
    bool first = true;
    for (int i = 0; i < P->n; i++) {
        const jm_hist_t *h = &P->hist[i];
        if (h->roll) continue;
        char san[24], tok[40];
        P->g->notation(P->tmp, &h->m, san, sizeof san);
        int who2 = P->g->turn(P->tmp);
        if (who2 == 0) snprintf(tok, sizeof tok, "%d. %s", moveno, san);
        else if (first) snprintf(tok, sizeof tok, "%d... %s", moveno, san);
        else snprintf(tok, sizeof tok, "%s", san);
        if (who2 == 1) moveno++;
        first = false;
        P->g->apply(P->tmp, &h->m);
        int l = (int)strlen(tok);
        if (col + l + 1 > 78) {
            fputs("\n", f);
            col = 0;
        } else if (col) {
            fputs(" ", f);
            col++;
        }
        fputs(tok, f);
        col += l;
    }
    snprintf(line, sizeof line, "%s%s\n", col ? " " : "", res);
    fputs(line, f);
    bool ok = fclose(f) == 0;
    if (name_out) snprintf(name_out, nn, "%s", name);
    return ok;
}

typedef struct {
    char (*names)[40];
    int  n, max;
} list_t;

static bool on_pgn(const aos_dir_entry_t *e, void *ctx)
{
    list_t *l = (list_t *)ctx;
    size_t k = strlen(e->name);
    if (e->dir || e->name[0] == '.' || k < 5 || k >= 40 || strcasecmp(e->name + k - 4, ".pgn")) return true;
    if (l->n >= l->max) return false;
    snprintf(l->names[l->n++], 40, "%s", e->name);
    return true;
}

static int name_cmp(const void *a, const void *b)
{
    /* the newest first: the names carry the date */
    return -strcmp((const char *)a, (const char *)b);
}

int jm_pgn_list(char names[][40], int max)
{
    char dir[180];
    pgn_dir(dir, sizeof dir);
    list_t l = { names, 0, max };
    aos_hal_dir_scan(dir, on_pgn, &l);
    qsort(names, (size_t)l.n, 40, name_cmp);
    return l.n;
}

/* the tag's value: [FEN "..."] */
static bool tag(const char *text, const char *name, char *out, size_t n)
{
    char key[24];
    snprintf(key, sizeof key, "[%s \"", name);
    const char *s = strstr(text, key);
    if (!s) return false;
    s += strlen(key);
    size_t k = 0;
    while (*s && *s != '"' && k + 1 < n) out[k++] = *s++;
    out[k] = 0;
    return true;
}

bool jm_pgn_read(const char *name)
{
    const jm_slot_t *s = jm_game_find("ajedrez");
    if (!s) return false;
    char dir[180], path[240];
    pgn_dir(dir, sizeof dir);
    snprintf(path, sizeof path, "%.179s/%.40s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 256 * 1024) {
        fclose(f);
        return false;
    }
    char *text = (char *)jm_malloc((size_t)len + 1);
    if (!text) {
        fclose(f);
        return false;
    }
    size_t got = fread(text, 1, (size_t)len, f);
    fclose(f);
    text[got] = 0;
    char fen[100] = "";
    tag(text, "FEN", fen, sizeof fen);
    /* two people at the board: whoever reads a game goes on as they like */
    jm_new_t n;
    memset(&n, 0, sizeof n);
    n.gi = (int)(s - jm_games);
    n.human[0] = n.human[1] = 1;
    n.level[0] = n.level[1] = 3;
    n.two = TWO_FACE;
    n.fen = fen[0] ? fen : NULL;
    if (!jm_play_start(&n)) {
        jm_free(text);
        return false;
    }
    jm_play_ai_cancel();
    /* the moves: skip the tags, comments, variations, numbers and results */
    const char *q = text;
    int depth_var = 0, played = 0;
    mesa_move_t *ms = P->moves;
    while (*q) {
        if (*q == '[' && depth_var == 0) {
            while (*q && *q != '\n') q++;
            continue;
        }
        if (*q == '{') {
            while (*q && *q != '}') q++;
            if (*q) q++;
            continue;
        }
        if (*q == ';') {
            while (*q && *q != '\n') q++;
            continue;
        }
        if (*q == '(') {
            depth_var++;
            q++;
            continue;
        }
        if (*q == ')') {
            if (depth_var) depth_var--;
            q++;
            continue;
        }
        if (isspace((unsigned char)*q)) {
            q++;
            continue;
        }
        char tok[24];
        int k = 0;
        while (*q && !isspace((unsigned char)*q) && *q != '{' && *q != '(' && *q != ')' && k < 23) tok[k++] = *q++;
        tok[k] = 0;
        if (depth_var) continue;
        /* "12." "12..." */
        char *t = tok;
        while (isdigit((unsigned char)*t)) t++;
        while (*t == '.') t++;
        if (!*t || !strcmp(t, "1-0") || !strcmp(t, "0-1") || !strcmp(t, "1/2-1/2") || !strcmp(t, "*") ||
            *t == '$')
            continue;
        /* a move: the legal one whose text it is */
        int nm = P->g->moves(P->st, ms, MESA_MAX_MOVES);
        bool found = false;
        char want[24];
        int w = 0;
        for (const char *c = t; *c && w < 23; c++) {
            if (*c != '+' && *c != '#' && *c != '!' && *c != '?') want[w++] = *c == '0' ? 'O' : *c;
        }
        want[w] = 0;
        for (int i = 0; i < nm && !found; i++) {
            char san[24], have[24];
            P->g->notation(P->st, &ms[i], san, sizeof san);
            int h = 0;
            for (const char *c = san; *c && h < 23; c++) {
                if (*c != '+' && *c != '#') have[h++] = *c;
            }
            have[h] = 0;
            if (!strcmp(have, want)) {
                jm_hist_t e;
                memset(&e, 0, sizeof e);
                e.m = ms[i];
                P->g->apply(P->st, &ms[i]);
                if (P->n < JM_HIST_MAX) P->hist[P->n++] = e;
                P->top = P->n;
                found = true;
                played++;
            }
        }
        if (!found) break;
    }
    jm_free(text);
    jm_play_replay(P->n);
    P->top = P->n;
    jm_play_relayout();
    char b[80];
    snprintf(b, sizeof b, "%s: %d %s", name, played, _("jugadas"));
    jm_hud_status(b, 2500);
    return true;
}
