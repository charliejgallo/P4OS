/*
 * Marcador - the model: players, scores, rules, history, the winner, and the
 * games saved on the card.
 *
 * Every change goes through here and leaves an entry in the history with a
 * picture of every score as it was before (mr_hist_t.before), so undoing is
 * putting that picture back, whatever the rule was: a game point in padel
 * that also closed a set, a bean that went from one player to another, or
 * the reset of the whole table. Entries of one gesture share a group number
 * and go back together.
 *
 * A game is a text file in the data folder, marcador/g<id>.txt, one thing
 * per line, written to a .tmp and renamed, so a card pulled in the middle
 * keeps the previous one. The portal page reads the same files.
 */
#include "mr_app.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

const uint32_t mr_palette[10] = { 0xE5484D, 0x3E8FE0, 0x2FB872, 0xF2B035, 0x9C6ADE,
                                  0xF07CC0, 0x2EC4C4, 0xF27935, 0xE8E4D8, 0x6E7B8B };

int mr_type_minp(int type)
{
    switch (type) {
    case MR_T_CUENTA:
    case MR_T_ABACO: return 1;
    default: return 2;
    }
}

int mr_type_maxp(int type)
{
    switch (type) {
    case MR_T_ESTADIO:
    case MR_T_CARTEL: return 2;
    case MR_T_TIZA: return 5;
    default: return 4;
    }
}

uint32_t mr_now(void)
{
    return aos_hal_time_is_valid() ? (uint32_t)time(NULL) : 0;
}

static void name_default(mr_game_t *g, int i)
{
    const char *n = NULL;
    char buf[MR_NAME];
    if (g->type == MR_T_PANO || g->type == MR_T_POROTOS) {
        if (g->np == 2) n = i == 0 ? _("Nosotros") : _("Ellos");
    } else if (g->type == MR_T_ESTADIO) {
        n = i == 0 ? _("Local") : _("Visitante");
    }
    if (!n) {
        snprintf(buf, sizeof buf, g->type == MR_T_CUENTA ? _("Contador %d") : _("Jugador %d"), i + 1);
        n = buf;
    }
    snprintf(g->p[i].name, MR_NAME, "%s", n);
}

void mr_game_defaults(mr_game_t *g, int type, int np)
{
    g->type = (uint8_t)type;
    int lo = mr_type_minp(type), hi = mr_type_maxp(type);
    if (np < lo) np = lo;
    if (np > hi) np = hi;
    g->np = (uint8_t)np;
    memset(g->opt, 0, sizeof g->opt);
    switch (type) {
    case MR_T_PANO:
        g->target = 30;
        g->opt[MR_PANO_SPLIT] = 1;
        break;
    case MR_T_POROTOS:
        g->target = 30;
        g->opt[MR_POR_KIND] = 0;
        g->opt[MR_POR_TOTAL] = 0;  /* 0 = enough for everybody */
        break;
    case MR_T_CUENTA: g->target = 0; break;
    case MR_T_ESTADIO:
        g->target = 0;
        g->opt[MR_EST_SPORT] = 0;
        g->opt[MR_EST_MIN] = 45;
        g->opt[MR_EST_PER] = 2;
        g->opt[MR_EST_DOWN] = 0;
        break;
    case MR_T_CARTEL:
        g->target = 0;
        g->opt[MR_CAR_MODE] = MR_CAR_PING;
        g->opt[MR_CAR_BEST] = 5;
        break;
    case MR_T_ABACO: g->target = 100; break;
    case MR_T_TIZA:
        g->target = 0;
        g->opt[MR_TIZA_MODE] = MR_TIZA_TALLY;
        g->opt[MR_TIZA_START] = 501;
        g->opt[MR_TIZA_SUB] = 0;
        break;
    default: break;
    }
    static const int COLOR0[MR_T_N] = { 8, 3, 0, 0, 0, 3, 8 };
    for (int i = 0; i < MR_MAXP; i++) {
        name_default(g, i);
        g->p[i].color = mr_palette[(COLOR0[type] + (i ? (type == MR_T_PANO || type == MR_T_TIZA ? i + 2 : i) : 0)) % 10];
    }
    if (type != MR_T_PANO && type != MR_T_TIZA) {
        g->p[0].color = mr_palette[0];
        g->p[1].color = mr_palette[1];
        g->p[2].color = mr_palette[2];
        g->p[3].color = mr_palette[3];
    }
    snprintf(g->name, sizeof g->name, "%s", mr_type_name(type));
}

mr_game_t *mr_game_new(int type)
{
    mr_game_t *g = (mr_game_t *)mr_alloc(sizeof *g);
    if (!g) return NULL;
    g->hist = (mr_hist_t *)mr_alloc(sizeof(mr_hist_t) * MR_HIST);
    if (!g->hist) {
        mr_free(g);
        return NULL;
    }
    g->winner = -1;
    g->period = 1;
    mr_game_defaults(g, type, 2);
    return g;
}

void mr_game_free(mr_game_t *g)
{
    if (!g) return;
    mr_free(g->hist);
    mr_free(g);
}

/* --- rules --------------------------------------------------------------------- */

static int start_score(const mr_game_t *g)
{
    if (g->type == MR_T_TIZA && g->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS && g->opt[MR_TIZA_SUB])
        return g->opt[MR_TIZA_START];
    return 0;
}

void mr_hist_note(mr_game_t *g, int who, int kind, int delta)
{
    if (g->nhist >= MR_HIST) {
        memmove(&g->hist[0], &g->hist[1], sizeof(mr_hist_t) * (MR_HIST - 1));
        g->nhist = MR_HIST - 1;
    }
    mr_hist_t *h = &g->hist[g->nhist++];
    memset(h, 0, sizeof *h);
    h->who = (int8_t)who;
    h->kind = (uint8_t)kind;
    h->delta = (int16_t)delta;
    h->t = g->played_s;
    h->group = g->group;
    for (int i = 0; i < MR_MAXP; i++) {
        h->before[i] = g->p[i].score;
        h->aux_before[i][0] = g->p[i].aux[0];
        h->aux_before[i][1] = g->p[i].aux[1];
    }
}

static int need_sets(const mr_game_t *g)
{
    int best = g->opt[MR_CAR_BEST];
    if (best < 1) best = 1;
    return best / 2 + 1;
}

static bool won_by(const mr_game_t *g, int i)
{
    const mr_player_t *p = &g->p[i];
    if (g->type == MR_T_CARTEL && g->opt[MR_CAR_MODE] != MR_CAR_FREE) return p->aux[1] >= need_sets(g);
    if (g->type == MR_T_TIZA && g->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS && g->opt[MR_TIZA_SUB])
        return p->score == 0 && g->nhist > 0;
    return g->target > 0 && p->score >= g->target;
}

void mr_check_winner(mr_game_t *g)
{
    int w = -1;
    /* the one who just scored wins a tie of two crossing the line at once */
    int last = g->nhist ? g->hist[g->nhist - 1].who : -1;
    if (last >= 0 && last < g->np && won_by(g, last)) w = last;
    for (int i = 0; i < g->np && w < 0; i++)
        if (won_by(g, i)) w = i;
    if (w >= 0 && !g->announced) {
        g->winner = (int8_t)w;
        g->announced = true;
        g->p[w].wins++;
        mr_ui_winner(w);
    } else if (w < 0 && g->announced) {
        if (g->winner >= 0 && g->p[g->winner].wins > 0) g->p[g->winner].wins--;
        g->winner = -1;
        g->announced = false;
    }
}

static void changed(mr_game_t *g, int why, int who)
{
    if (mr_A && mr_A->scene_on && mr_A->g == g && mr_A->scene.ops->sync) mr_A->scene.ops->sync(&mr_A->scene, why, who);
    mr_check_winner(g);
    mr_set_dirty();
    mr_ui_bar_refresh();
}

void mr_add(mr_game_t *g, int who, int delta)
{
    if (who < 0 || who >= g->np || !delta) return;
    int32_t s = g->p[who].score + delta;
    if (g->type == MR_T_TIZA && g->opt[MR_TIZA_MODE] == MR_TIZA_TOTALS && g->opt[MR_TIZA_SUB]) {
        if (s < 0) {
            mr_toast(_("Se pasa: no cuenta"));
            mr_snd_play(MR_S_UNDO, 0.8f, 0.8f);
            return;
        }
    } else if (s < 0) {
        if (g->type != MR_T_TIZA) s = 0;
    }
    if (s > 99999) s = 99999;
    if (s == g->p[who].score) return;
    g->group++;
    mr_hist_note(g, who, MR_H_POINTS, (int)(s - g->p[who].score));
    g->p[who].score = s;
    changed(g, MR_SYNC_ADD, who);
}

static void padel_point(mr_game_t *g, int a)
{
    int b = 1 - a;
    mr_player_t *A = &g->p[a], *B = &g->p[b];
    bool tiebreak = A->aux[0] == 6 && B->aux[0] == 6;
    bool game = false;
    A->score++;
    if (tiebreak) {
        game = A->score >= 7 && A->score - B->score >= 2;
    } else if (A->score >= 4 && A->score - B->score >= 2) {
        game = true;
    } else if (A->score == 4 && B->score == 4) {
        /* the advantage is lost: deuce again, forty-forty */
        A->score = B->score = 3;
    }
    if (!game) return;
    A->score = B->score = 0;
    A->aux[0]++;
    bool set = (A->aux[0] >= 6 && A->aux[0] - B->aux[0] >= 2) || A->aux[0] == 7;
    if (set) {
        A->aux[1]++;
        A->aux[0] = B->aux[0] = 0;
    }
}

static void ping_point(mr_game_t *g, int a)
{
    int b = 1 - a;
    mr_player_t *A = &g->p[a], *B = &g->p[b];
    A->score++;
    if (A->score >= 11 && A->score - B->score >= 2) {
        A->score = B->score = 0;
        A->aux[1]++;
    }
}

void mr_point(mr_game_t *g, int who)
{
    if (who < 0 || who >= g->np) return;
    int mode = g->type == MR_T_CARTEL ? g->opt[MR_CAR_MODE] : MR_CAR_FREE;
    if (mode == MR_CAR_FREE) {
        mr_add(g, who, 1);
        return;
    }
    if (g->winner >= 0) return;    /* the match is over: undo or rematch */
    g->group++;
    mr_hist_note(g, who, MR_H_POINTS, 1);
    if (mode == MR_CAR_PADEL) padel_point(g, who);
    else ping_point(g, who);
    changed(g, MR_SYNC_ADD, who);
}

void mr_move(mr_game_t *g, int from, int to)
{
    if (from == to) return;
    g->group++;
    if (from >= 0 && from < g->np && g->p[from].score > 0) {
        mr_hist_note(g, from, MR_H_MOVE, -1);
        g->p[from].score--;
    }
    if (to >= 0 && to < g->np) {
        mr_hist_note(g, to, MR_H_MOVE, 1);
        g->p[to].score++;
    }
    /* the scene moved the bean itself: it is told, not asked */
    mr_check_winner(g);
    mr_set_dirty();
    mr_ui_bar_refresh();
}

bool mr_undo(mr_game_t *g)
{
    if (!g->nhist) return false;
    uint16_t grp = g->hist[g->nhist - 1].group;
    int first = g->nhist - 1;
    while (first > 0 && g->hist[first - 1].group == grp) first--;
    mr_hist_t *h = &g->hist[first];
    int who = h->who;
    for (int i = 0; i < MR_MAXP; i++) {
        g->p[i].score = h->before[i];
        g->p[i].aux[0] = h->aux_before[i][0];
        g->p[i].aux[1] = h->aux_before[i][1];
    }
    g->nhist = first;
    changed(g, MR_SYNC_UNDO, who);
    return true;
}

void mr_reset(mr_game_t *g, bool rematch)
{
    if (rematch) {
        g->nhist = 0;
    } else {
        g->group++;
        mr_hist_note(g, -1, MR_H_RESET, 0);
    }
    for (int i = 0; i < MR_MAXP; i++) {
        g->p[i].score = start_score(g);
        g->p[i].aux[0] = g->p[i].aux[1] = 0;
    }
    g->winner = -1;
    g->announced = false;
    g->clock_ms = 0;
    g->period = 1;
    g->running = false;
    if (mr_A && mr_A->scene_on && mr_A->g == g && mr_A->scene.ops->sync)
        mr_A->scene.ops->sync(&mr_A->scene, MR_SYNC_RESET, -1);
    mr_set_dirty();
    mr_ui_bar_refresh();
}

const char *mr_tennis_pts(const mr_game_t *g, int who)
{
    static char buf[2][8];
    const mr_player_t *A = &g->p[who], *B = &g->p[1 - who];
    if (A->aux[0] == 6 && B->aux[0] == 6) {
        snprintf(buf[who & 1], 8, "%d", (int)A->score);
        return buf[who & 1];
    }
    if (A->score >= 3 && B->score >= 3) {
        if (A->score > B->score) return "AD";
        if (A->score < B->score) return "40";
        return "40";
    }
    static const char *const P[] = { "0", "15", "30", "40", "AD" };
    return P[A->score < 0 ? 0 : A->score > 4 ? 4 : A->score];
}

/* --- the card ----------------------------------------------------------------------- */

static void dir_path(char *out, size_t n)
{
    snprintf(out, n, "%s/marcador", aos_hal_path_data());
}

static void file_path(char *out, size_t n, uint32_t id, const char *ext)
{
    snprintf(out, n, "%s/marcador/g%lu.%s", aos_hal_path_data(), (unsigned long)id, ext);
}

uint32_t mr_new_id(void)
{
    uint32_t ids[64];
    int n = mr_list(ids, 64);
    uint32_t m = 0;
    for (int i = 0; i < n; i++)
        if (ids[i] > m) m = ids[i];
    int32_t last = 0;
    aos_hal_pref_get_i32("mr_lastid", &last);
    if ((uint32_t)last > m) m = (uint32_t)last;
    aos_hal_pref_set_i32("mr_lastid", (int32_t)(m + 1));
    return m + 1;
}

static void clean_line(char *s)
{
    for (; *s; s++)
        if (*s == '\n' || *s == '\r') *s = ' ';
}

bool mr_save(mr_game_t *g)
{
    char dir[128], tmp[160], fin[160];
    dir_path(dir, sizeof dir);
    mkdir(dir, 0777);
    file_path(tmp, sizeof tmp, g->id, "tmp");
    file_path(fin, sizeof fin, g->id, "txt");
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    int32_t seq = 0;
    aos_hal_pref_get_i32("mr_seq", &seq);
    seq++;
    aos_hal_pref_set_i32("mr_seq", seq);
    g->updated = mr_now();
    clean_line(g->name);
    fprintf(f, "marcador 1\ntype %d\nname %s\ntarget %ld\nopt", g->type, g->name, (long)g->target);
    for (int i = 0; i < 8; i++) fprintf(f, " %ld", (long)g->opt[i]);
    fprintf(f, "\ncreated %lu\nupdated %lu\nseq %ld\nplayed %lu\nclock %ld %d %d\nwinner %d %d\ngroup %u\n",
            (unsigned long)g->created, (unsigned long)g->updated, (long)seq, (unsigned long)g->played_s,
            (long)g->clock_ms, g->period, g->running ? 1 : 0, g->winner, g->announced ? 1 : 0, g->group);
    for (int i = 0; i < g->np; i++) {
        mr_player_t *p = &g->p[i];
        clean_line(p->name);
        fprintf(f, "p %06lX %ld %d %d %d %s\n", (unsigned long)p->color, (long)p->score, p->aux[0], p->aux[1], p->wins,
                p->name);
    }
    for (int k = 0; k < g->nhist; k++) {
        mr_hist_t *h = &g->hist[k];
        fprintf(f, "h %d %d %d %lu %u", h->who, h->kind, h->delta, (unsigned long)h->t, h->group);
        for (int i = 0; i < g->np; i++) fprintf(f, " %ld %d %d", (long)h->before[i], h->aux_before[i][0], h->aux_before[i][1]);
        fputs("\n", f);
    }
    fclose(f);
    remove(fin);
    return rename(tmp, fin) == 0;
}

static char *next_tok(char **s)
{
    while (**s == ' ') (*s)++;
    if (!**s) return NULL;
    char *t = *s;
    while (**s && **s != ' ') (*s)++;
    if (**s) *(*s)++ = 0;
    return t;
}

static long tok_l(char **s)
{
    char *t = next_tok(s);
    return t ? strtol(t, NULL, 10) : 0;
}

bool mr_load(mr_game_t *g, uint32_t id)
{
    char path[160];
    file_path(path, sizeof path, id, "txt");
    FILE *f = fopen(path, "r");
    if (!f) return false;
    mr_hist_t *hist = g->hist;
    memset(g, 0, sizeof *g);
    g->hist = hist;
    g->id = id;
    g->winner = -1;
    g->period = 1;
    char line[512];
    bool ok = false;
    int np = 0;
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = 0;
        char *s = line;
        char *k = next_tok(&s);
        if (!k) continue;
        if (!strcmp(k, "marcador")) ok = true;
        else if (!strcmp(k, "type")) g->type = (uint8_t)tok_l(&s);
        else if (!strcmp(k, "name")) snprintf(g->name, sizeof g->name, "%s", s);
        else if (!strcmp(k, "target")) g->target = (int32_t)tok_l(&s);
        else if (!strcmp(k, "opt")) for (int i = 0; i < 8; i++) g->opt[i] = (int32_t)tok_l(&s);
        else if (!strcmp(k, "created")) g->created = (uint32_t)tok_l(&s);
        else if (!strcmp(k, "updated")) g->updated = (uint32_t)tok_l(&s);
        else if (!strcmp(k, "played")) g->played_s = (uint32_t)tok_l(&s);
        else if (!strcmp(k, "clock")) {
            g->clock_ms = (int32_t)tok_l(&s);
            g->period = (uint8_t)tok_l(&s);
            g->running = false;    /* a clock never runs by itself across closing */
            (void)tok_l(&s);
        } else if (!strcmp(k, "winner")) {
            g->winner = (int8_t)tok_l(&s);
            g->announced = tok_l(&s) != 0;
        } else if (!strcmp(k, "group")) g->group = (uint16_t)tok_l(&s);
        else if (!strcmp(k, "p") && np < MR_MAXP) {
            mr_player_t *p = &g->p[np++];
            char *c = next_tok(&s);
            p->color = c ? (uint32_t)strtoul(c, NULL, 16) : 0xFFFFFF;
            p->score = (int32_t)tok_l(&s);
            p->aux[0] = (int16_t)tok_l(&s);
            p->aux[1] = (int16_t)tok_l(&s);
            p->wins = (int16_t)tok_l(&s);
            snprintf(p->name, MR_NAME, "%s", s);
        } else if (!strcmp(k, "h") && g->nhist < MR_HIST) {
            mr_hist_t *h = &g->hist[g->nhist++];
            memset(h, 0, sizeof *h);
            h->who = (int8_t)tok_l(&s);
            h->kind = (uint8_t)tok_l(&s);
            h->delta = (int16_t)tok_l(&s);
            h->t = (uint32_t)tok_l(&s);
            h->group = (uint16_t)tok_l(&s);
            for (int i = 0; i < MR_MAXP; i++) {
                h->before[i] = (int32_t)tok_l(&s);
                h->aux_before[i][0] = (int16_t)tok_l(&s);
                h->aux_before[i][1] = (int16_t)tok_l(&s);
            }
        }
    }
    fclose(f);
    if (g->type >= MR_T_N) ok = false;
    g->np = (uint8_t)np;
    if (np < mr_type_minp(g->type)) ok = false;
    /* players that are not in the file keep sensible defaults for the form */
    for (int i = np; i < MR_MAXP; i++) {
        name_default(g, i);
        g->p[i].color = mr_palette[i % 10];
    }
    return ok;
}

void mr_delete(uint32_t id)
{
    char path[160];
    file_path(path, sizeof path, id, "txt");
    remove(path);
}

typedef struct {
    uint32_t id;
    int32_t  seq;
} ent_t;

static int ent_cmp(const void *a, const void *b)
{
    const ent_t *x = (const ent_t *)a, *y = (const ent_t *)b;
    return x->seq < y->seq ? 1 : x->seq > y->seq ? -1 : 0;
}

int mr_list(uint32_t *ids, int max)
{
    char dir[128];
    dir_path(dir, sizeof dir);
    DIR *d = opendir(dir);
    if (!d) return 0;
    ent_t *e = (ent_t *)mr_alloc(sizeof(ent_t) * 256);
    if (!e) {
        closedir(d);
        return 0;
    }
    int n = 0;
    struct dirent *de;
    while ((de = readdir(d)) && n < 256) {
        const char *nm = de->d_name;
        if (nm[0] != 'g') continue;
        size_t L = strlen(nm);
        if (L < 6 || strcmp(nm + L - 4, ".txt")) continue;
        uint32_t id = (uint32_t)strtoul(nm + 1, NULL, 10);
        if (!id) continue;
        e[n].id = id;
        e[n].seq = (int32_t)id;
        /* the order is the last time each was saved: its seq line */
        char path[320];
        snprintf(path, sizeof path, "%s/%.40s", dir, nm);
        FILE *f = fopen(path, "r");
        if (f) {
            char line[96];
            for (int k = 0; k < 12 && fgets(line, sizeof line, f); k++) {
                if (!strncmp(line, "seq ", 4)) {
                    e[n].seq = (int32_t)strtol(line + 4, NULL, 10);
                    break;
                }
            }
            fclose(f);
        }
        n++;
    }
    closedir(d);
    qsort(e, (size_t)n, sizeof(ent_t), ent_cmp);
    if (n > max) n = max;
    for (int i = 0; i < n; i++) ids[i] = e[i].id;
    mr_free(e);
    return n;
}
