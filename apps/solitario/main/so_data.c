/*
 * SOLITARIO - what is kept: the settings (preferences), the statistics and
 * achievements (a small file), and the hand in progress (its seed and the
 * moves made, which replayed give back the hand and its undo).
 *
 * Everything lives in the app's data folder, so an update of the app does
 * not lose it, and a card where it never ran simply starts from zero.
 */
#include "so_app.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define DATA_MAGIC  0x534F4C31u     /* "SOL1" */
#define SAVE_MAGIC  0x534F4C47u     /* "SOLG" */
#define DATA_VER    1
#define SAVE_VER    1

/* --- names ---------------------------------------------------------------------- */

const char *const so_felt_name[SO_NFELTS] = {
    N_("Verde de siempre"), N_("Azul"), N_("Rojo"), N_("Madera"),
    N_("Cuero"), N_("Noche"), N_("Océano"), N_("Damasco"),
};

const char *const so_back_name[SO_NBACKS] = {
    N_("Clásico rojo"), N_("Clásico azul"), N_("Art déco"), N_("Luna"),
    N_("Olas"), N_("Escocés"), N_("Arlequín"), N_("Dorado"),
};

const char *const so_deck_name[SO_NDECKS] = {
    N_("Clásica"), N_("Cuatro colores"), N_("Grande"),
};

const char *const so_scoring_name[SO_NSCORING] = {
    N_("Estándar"), N_("Vegas"), N_("Vegas acumulativo"), N_("Por tiempo"), N_("Sin puntaje"),
};

const char *const so_win_name[SO_NWINS] = {
    N_("Rebote"), N_("Fuegos artificiales"), N_("Espiral"), N_("Al azar"),
};

const so_ach_t so_ach[SO_NACH] = {
    { N_("Primera mano"),      N_("Ganá una mano."),                                SO_UNL_FELT, 1 },
    { N_("De a tres"),         N_("Ganá Klondike de a 3."),                         SO_UNL_BACK, 1 },
    { N_("Tejedora"),          N_("Ganá Spider de un palo."),                       SO_UNL_FELT, 3 },
    { N_("Dos palos"),         N_("Ganá Spider de dos palos."),                     SO_UNL_BACK, 5 },
    { N_("Cuatro palos"),      N_("Ganá Spider de cuatro palos."),                  SO_UNL_FELT, 7 },
    { N_("Celdas libres"),     N_("Ganá FreeCell."),                                SO_UNL_FELT, 4 },
    { N_("Faraón"),            N_("Ganá la Pirámide."),                             SO_UNL_BACK, 2 },
    { N_("Tres cumbres"),      N_("Ganá TriPeaks."),                                SO_UNL_FELT, 6 },
    { N_("Hoyo en uno"),       N_("Ganá Golf."),                                    SO_UNL_BACK, 4 },
    { N_("Al norte"),          N_("Ganá Yukon."),                                   SO_UNL_FELT, 2 },
    { N_("Con la española"),   N_("Ganá la Española."),                             SO_UNL_BACK, 6 },
    { N_("Todos los días"),    N_("Ganá una mano del día."),                        SO_UNL_FELT, 5 },
    { N_("Racha de cinco"),    N_("Ganá cinco manos seguidas de un mismo juego."),  SO_UNL_BACK, 7 },
    { N_("Diez victorias"),    N_("Ganá diez manos en total."),                     SO_UNL_DECK, 1 },
    { N_("A pulmón"),          N_("Ganá sin deshacer ni pedir pistas."),            SO_UNL_BACK, 3 },
    { N_("Contrarreloj"),      N_("Ganá Klondike en menos de tres minutos."),       SO_UNL_NONE, 0 },
    { N_("Casa ganadora"),     N_("Terminá una mano de Vegas con ganancia."),       SO_UNL_NONE, 0 },
};

/* --- settings ---------------------------------------------------------------------- */

static int pref(const char *k, int def)
{
    int32_t v;
    return aos_hal_pref_get_i32(k, &v) ? (int)v : def;
}

void so_set_load(so_set_t *s)
{
    s->mode = (uint8_t)pref("so_mode", SO_M_KLON1);
    s->scoring = (uint8_t)pref("so_score", SO_SC_STANDARD);
    s->limited = (uint8_t)pref("so_limit", 0);
    s->undo = (uint8_t)pref("so_undo", 1);
    s->hints = (uint8_t)pref("so_hints", 1);
    s->solvable = (uint8_t)pref("so_solv", 1);
    s->win_anim = (uint8_t)pref("so_wanim", SO_WIN_BOUNCE);
    s->sound = (uint8_t)pref("so_sound", 1);
    s->autocomplete = (uint8_t)pref("so_auto", 1);
    s->felt = (uint8_t)pref("so_felt", 0);
    s->back = (uint8_t)pref("so_back", 0);
    s->deck = (uint8_t)pref("so_deck", 0);
    if (s->mode >= SO_NMODES) s->mode = SO_M_KLON1;
    if (s->scoring >= SO_NSCORING) s->scoring = SO_SC_STANDARD;
    if (s->win_anim >= SO_NWINS) s->win_anim = SO_WIN_BOUNCE;
    if (s->felt >= SO_NFELTS) s->felt = 0;
    if (s->back >= SO_NBACKS) s->back = 0;
    if (s->deck >= SO_NDECKS) s->deck = 0;
}

void so_set_save(const so_set_t *s)
{
    aos_hal_pref_set_i32("so_mode", s->mode);
    aos_hal_pref_set_i32("so_score", s->scoring);
    aos_hal_pref_set_i32("so_limit", s->limited);
    aos_hal_pref_set_i32("so_undo", s->undo);
    aos_hal_pref_set_i32("so_hints", s->hints);
    aos_hal_pref_set_i32("so_solv", s->solvable);
    aos_hal_pref_set_i32("so_wanim", s->win_anim);
    aos_hal_pref_set_i32("so_sound", s->sound);
    aos_hal_pref_set_i32("so_auto", s->autocomplete);
    aos_hal_pref_set_i32("so_felt", s->felt);
    aos_hal_pref_set_i32("so_back", s->back);
    aos_hal_pref_set_i32("so_deck", s->deck);
}

/* --- files ------------------------------------------------------------------------- */

static void path(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", aos_hal_path_data(), name);
}

void so_data_load(so_app_t *a)
{
    memset(&a->data, 0, sizeof a->data);
    char p[160];
    path(p, sizeof p, "solitario.dat");
    FILE *f = fopen(p, "rb");
    if (!f) {
        return;
    }
    uint32_t hdr[2] = { 0 };
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == DATA_MAGIC && hdr[1] == DATA_VER) {
        so_data_t d;
        if (fread(&d, sizeof d, 1, f) == 1) {
            a->data = d;
        }
    }
    fclose(f);
}

void so_data_save(so_app_t *a)
{
    char p[160], t[168];
    mkdir(aos_hal_path_data(), 0777);
    path(p, sizeof p, "solitario.dat");
    snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "wb");
    if (!f) {
        return;
    }
    uint32_t hdr[2] = { DATA_MAGIC, DATA_VER };
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(&a->data, sizeof a->data, 1, f) == 1;
    fclose(f);
    if (ok) {
        remove(p);
        rename(t, p);
    }
}

/* The hand in progress: which game, which deal, and every move. */
typedef struct {
    uint32_t magic, ver;
    uint8_t  mode, scoring, limited, daily;
    uint32_t seed, day, elapsed;
    uint8_t  started, used_help, pad[2];
    uint32_t nmoves;
} save_hdr_t;

bool so_game_save(so_app_t *a)
{
    char p[160];
    path(p, sizeof p, "solitario_mano.dat");
    if (a->finished || !a->started || so_won(&a->g)) {
        remove(p);
        return false;
    }
    mkdir(aos_hal_path_data(), 0777);
    FILE *f = fopen(p, "wb");
    if (!f) {
        return false;
    }
    save_hdr_t h = { SAVE_MAGIC, SAVE_VER, a->g.mode, a->g.scoring, a->g.passes_max >= 0, a->daily,
                     a->g.seed, a->daily_day, a->elapsed_ms, a->started, a->used_help, { 0, 0 },
                     a->g.nrecs };
    bool ok = fwrite(&h, sizeof h, 1, f) == 1;
    for (uint32_t i = 0; ok && i < a->g.nrecs; i++) {
        ok = fwrite(&a->g.recs[i].mv, sizeof(so_move_t), 1, f) == 1;
    }
    fclose(f);
    return ok;
}

bool so_game_load(so_app_t *a)
{
    char p[160];
    path(p, sizeof p, "solitario_mano.dat");
    FILE *f = fopen(p, "rb");
    if (!f) {
        return false;
    }
    save_hdr_t h;
    bool ok = fread(&h, sizeof h, 1, f) == 1 && h.magic == SAVE_MAGIC && h.ver == SAVE_VER &&
              h.mode < SO_NMODES && h.nmoves < 20000;
    if (ok) {
        so_opts_t o = { h.scoring, h.limited != 0 };
        so_new(&a->g, h.mode, h.seed, &o);
        for (uint32_t i = 0; ok && i < h.nmoves; i++) {
            so_move_t m;
            ok = fread(&m, sizeof m, 1, f) == 1 && so_legal(&a->g, &m);
            if (ok) so_apply(&a->g, &m);
        }
        a->daily = h.daily != 0;
        a->daily_day = h.day;
        a->elapsed_ms = h.elapsed;
        a->started = h.started != 0;
        a->used_help = h.used_help != 0;
        a->finished = false;
    }
    fclose(f);
    if (!ok) {
        remove(p);
    }
    return ok;
}

void so_game_forget(void)
{
    char p[160];
    path(p, sizeof p, "solitario_mano.dat");
    remove(p);
}

/* --- the end of a hand ------------------------------------------------------------- */

static uint32_t grant(so_app_t *a, int ach)
{
    uint32_t bit = 1u << ach;
    if (a->data.ach & bit) {
        return 0;
    }
    a->data.ach |= bit;
    return bit;
}

uint32_t so_data_won(so_app_t *a)
{
    so_game_t *g = &a->g;
    so_stat_t *s = &a->data.st[g->mode];
    uint32_t secs = a->elapsed_ms / 1000;
    uint32_t got = 0;
    if (!a->started) {
        s->played++;
    }
    s->won++;
    s->streak++;
    if (s->streak > s->best_streak) s->best_streak = s->streak;
    if (!s->best_time || secs < s->best_time) s->best_time = secs;
    s->total_time += secs;
    if (g->scoring != SO_SC_NONE) {
        if (!s->has_score || g->score > s->best_score) s->best_score = g->score;
        s->has_score = 1;
    }
    a->finished = true;

    uint32_t total = 0;
    for (int m = 0; m < SO_NMODES; m++) total += a->data.st[m].won;
    got |= grant(a, SO_ACH_FIRST);
    switch (g->mode) {
    case SO_M_KLON3:    got |= grant(a, SO_ACH_KLON3); break;
    case SO_M_SPIDER1:  got |= grant(a, SO_ACH_SPIDER1); break;
    case SO_M_SPIDER2:  got |= grant(a, SO_ACH_SPIDER2); break;
    case SO_M_SPIDER4:  got |= grant(a, SO_ACH_SPIDER4); break;
    case SO_M_FREECELL: got |= grant(a, SO_ACH_FREECELL); break;
    case SO_M_PYRAMID:  got |= grant(a, SO_ACH_PYRAMID); break;
    case SO_M_TRIPEAKS: got |= grant(a, SO_ACH_TRIPEAKS); break;
    case SO_M_GOLF:     got |= grant(a, SO_ACH_GOLF); break;
    case SO_M_YUKON:    got |= grant(a, SO_ACH_YUKON); break;
    case SO_M_SPANISH:  got |= grant(a, SO_ACH_SPANISH); break;
    default: break;
    }
    if ((g->mode == SO_M_KLON1 || g->mode == SO_M_KLON3) && secs < 180) got |= grant(a, SO_ACH_FAST);
    if (a->daily) {
        got |= grant(a, SO_ACH_DAILY);
        if (a->data.daily_day != a->daily_day) {
            a->data.daily_streak++;
            a->data.daily_day = a->daily_day;
        }
    }
    if (s->streak >= 5) got |= grant(a, SO_ACH_STREAK5);
    if (total >= 10) got |= grant(a, SO_ACH_TEN);
    if (!a->used_help) got |= grant(a, SO_ACH_CLEAN);
    if ((g->scoring == SO_SC_VEGAS || g->scoring == SO_SC_VEGAS_CUM) && g->score > 0) {
        got |= grant(a, SO_ACH_VEGAS);
    }
    if (g->scoring == SO_SC_VEGAS_CUM) {
        a->data.vegas_bank += g->score;
    }
    so_data_save(a);
    so_game_forget();
    return got;
}

/* A hand left unfinished (a new one dealt over it): it counts as lost. */
void so_data_lost(so_app_t *a)
{
    if (!a->started || a->finished) {
        return;
    }
    so_stat_t *s = &a->data.st[a->g.mode];
    s->streak = 0;
    s->total_time += a->elapsed_ms / 1000;
    if (a->g.scoring == SO_SC_VEGAS_CUM) {
        a->data.vegas_bank += a->g.score;
        if (a->g.score > 0) grant(a, SO_ACH_VEGAS);
    } else if (a->g.scoring == SO_SC_VEGAS && a->g.score > 0) {
        grant(a, SO_ACH_VEGAS);
    }
    a->finished = true;
    so_data_save(a);
}

bool so_unlocked(const so_app_t *a, int kind, int item)
{
    if (item == 0) {
        return true;                            /* the first of each is free */
    }
    if (kind == SO_UNL_DECK && item == SO_DECK_LARGE) {
        return true;                            /* reading well is not a prize */
    }
    for (int i = 0; i < SO_NACH; i++) {
        if (so_ach[i].kind == kind && so_ach[i].item == item) {
            return (a->data.ach >> i) & 1;
        }
    }
    return false;
}

/* --- the day's hand ------------------------------------------------------------------ */

uint32_t so_today(void)
{
    if (!aos_hal_time_is_valid()) {
        return 0;
    }
    struct tm t;
    aos_hal_time_now(&t);
    return (uint32_t)((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday);
}

/* days since 2000, from yyyymmdd: the game turns with the week */
static uint32_t day_number(uint32_t day)
{
    int y = (int)(day / 10000), m = (int)(day / 100 % 100), d = (int)(day % 100);
    if (m < 3) {
        y--;
        m += 12;
    }
    return (uint32_t)(365 * y + y / 4 - y / 100 + y / 400 + (153 * (m - 3) + 2) / 5 + d - 730426);
}

int so_daily_mode(uint32_t day)
{
    static const uint8_t WEEK[7] = { SO_M_KLON1, SO_M_SPIDER2, SO_M_FREECELL, SO_M_PYRAMID,
                                     SO_M_TRIPEAKS, SO_M_YUKON, SO_M_KLON3 };
    return WEEK[day_number(day) % 7];
}

uint32_t so_daily_seed(uint32_t day, int mode)
{
    uint32_t n = so_seed_n[mode];
    if (!n) {
        return day;
    }
    /* the same for everyone that day: a walk through the table */
    uint32_t k = day_number(day) * 2654435761u;
    return so_seed_tab[mode][(k >> 7) % n];
}
