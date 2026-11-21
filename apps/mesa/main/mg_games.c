/*
 * Juegos de mesa - the games there are: the six that come with the app,
 * then whatever modules the card has in apps/mesa/ (mesa_game.h, at the
 * end). A module stays open while the app lives: its tables are its own
 * memory. Its art is apps/mesa_<word>_p4.pak, found by mg_art.c.
 */
#include "mg_app.h"

#include "aos_hal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

mg_slot_t mg_games[MG_GAMES_MAX];
int        mg_ngames;

#define MODS_MAX 8
static aos_module_t *s_mod[MODS_MAX];
static int           s_nmod;

const mg_slot_t *mg_game_find(const char *word)
{
    for (int i = 0; i < mg_ngames; i++) {
        if (!strcmp(mg_games[i].g->word, word)) return &mg_games[i];
    }
    return NULL;
}

static void add(const mesa_game_t *g, const char *file)
{
    if (mg_ngames >= MG_GAMES_MAX || !g || !g->word || mg_game_find(g->word)) return;
    mg_slot_t *e = &mg_games[mg_ngames++];
    memset(e, 0, sizeof *e);
    e->g = g;
    snprintf(e->file, sizeof e->file, "%s", file ? file : "");
}

#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
typedef int (*abi_fn_t)(void);
typedef const mesa_game_t *(*game_fn_t)(int);
typedef void (*host_fn_t)(const mesa_host_t *);

typedef struct {
    char names[MODS_MAX][24];
    int  n;
} found_t;

static bool on_file(const aos_dir_entry_t *e, void *ctx)
{
    found_t *f = (found_t *)ctx;
    size_t l = strlen(e->name);
    if (e->dir || e->name[0] == '.' || l < 4 || l > 22 || strcmp(e->name + l - 3, ".so")) return true;
    if (f->n >= MODS_MAX) return false;
    snprintf(f->names[f->n], sizeof f->names[0], "%.*s", (int)(l - 3), e->name);
    f->n++;
    return true;
}

static void scan_modules(void)
{
    char dir[160];
    snprintf(dir, sizeof dir, "%s/mesa", aos_hal_path_apps());
    found_t f;
    memset(&f, 0, sizeof f);
    aos_hal_dir_scan(dir, on_file, &f);
    for (int i = 0; i < f.n && s_nmod < MODS_MAX; i++) {
        char path[200], err[96];
        snprintf(path, sizeof path, "%s/%s.so", dir, f.names[i]);
        aos_module_t *m = aos_hal_module_open(path, err, sizeof err);
        if (!m) {
            aos_hal_log("mesa", "%s: %s", path, err);
            continue;
        }
        abi_fn_t abi = (abi_fn_t)aos_hal_module_sym(m, "mesa_module_abi");
        game_fn_t game = (game_fn_t)aos_hal_module_sym(m, "mesa_module_game");
        host_fn_t host = (host_fn_t)aos_hal_module_sym(m, "mesa_module_host");
        if (!abi || !game || abi() != MESA_ABI) {
            aos_hal_log("mesa", "%s: not a game of this version (abi %d, the app's %d)", path, abi ? abi() : -1,
                        MESA_ABI);
            aos_hal_module_close(m);
            continue;
        }
        if (host) host(mesa_host);
        int before = mg_ngames;
        for (int k = 0; k < 8; k++) {
            const mesa_game_t *g = game(k);
            if (!g) break;
            add(g, f.names[i]);
        }
        if (mg_ngames == before) {
            aos_hal_module_close(m);
            continue;
        }
        s_mod[s_nmod++] = m;
        aos_hal_log("mesa", "%s: %d games", path, mg_ngames - before);
    }
}
#endif

void mg_games_scan(void)
{
    mg_games_free();
    add(&mesa_ajedrez, NULL);
    add(&mesa_damas, NULL);
    add(&mesa_reversi, NULL);
    add(&mesa_cuatro, NULL);
    add(&mesa_ludo, NULL);
    add(&mesa_backgammon, NULL);
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    scan_modules();
#endif
}

void mg_games_free(void)
{
#if !defined(AOS_SIM) && !defined(AOS_SIM_BUILTIN)
    for (int i = 0; i < s_nmod; i++) aos_hal_module_close(s_mod[i]);
#endif
    s_nmod = 0;
    mg_ngames = 0;
}
