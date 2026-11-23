/*
 * P4OS - Fusión: what is kept
 *
 *   preferences   the size, mode and theme last chosen, the sound, the
 *                 achievements (a bit each), the records per size and mode
 *   fusion.sav    the game in course, in the app's data folder, so closing
 *                 the app (or the Store updating it) does not cost the board
 *
 * The file starts with a magic and the struct's size: a file from another
 * version of the struct is ignored rather than read wrong.
 */
#include "fu_app.h"

#include <stdio.h>
#include <string.h>

fu_prefs_t fu_P = { 4, FU_MODE_CLASSIC, 0, true, 0, 0, 0 };

#define SAVE_MAGIC 0x46555331u      /* "FUS1" */

static const struct { const char *title, *text; } ACH[FU_ACHS] = {
    { N_("Primeros pasos"),      N_("Llegá a 128.") },
    { N_("Buen pulso"),          N_("Llegá a 512.") },
    { N_("Mil y pico"),          N_("Llegá a 1024.") },
    { N_("Leyenda"),             N_("Llegá a 4096.") },
    { N_("De bolsillo"),         N_("En el tablero de 3×3, llegá a 256.") },
    { N_("Sin red"),             N_("Cumplí una meta sin deshacer.") },
    { N_("Triple"),              N_("Hacé tres fusiones en un solo movimiento.") },
    { N_("A contrarreloj"),      N_("Sumá 3000 puntos contra reloj.") },
    { N_("Entre piedras"),       N_("Llegá a 512 con bloques.") },
    { N_("Mano a mano"),         N_("Terminá una partida de a dos.") },
    { N_("Maratón"),             N_("Hacé 500 movimientos en una partida.") },
    { N_("Coleccionista"),       N_("Jugá con los cuatro temas.") },
};

const char *fu_ach_title(int i) { return i >= 0 && i < FU_ACHS ? ACH[i].title : ""; }
const char *fu_ach_text(int i)  { return i >= 0 && i < FU_ACHS ? ACH[i].text : ""; }

static int32_t get(const char *key, int32_t def)
{
    int32_t v = def;
    aos_hal_pref_get_i32(key, &v);
    return v;
}

void fu_prefs_load(void)
{
    int32_t n = get("fu_n", 4), m = get("fu_mode", 0), t = get("fu_theme", 0);
    fu_P.n = (uint8_t)(n >= FU_NMIN && n <= FU_NMAX ? n : 4);
    fu_P.mode = (uint8_t)(m >= 0 && m < FU_MODES ? m : 0);
    fu_P.theme = (uint8_t)(t >= 0 && t < FU_THEMES ? t : 0);
    fu_P.sound = get("fu_snd", 1) != 0;
    fu_P.ach = (uint32_t)get("fu_ach", 0);
    fu_P.themes_used = (uint8_t)get("fu_thm", 0);
    fu_P.sizes_played = (uint8_t)get("fu_sizes", 0);
}

void fu_prefs_save(void)
{
    aos_hal_pref_set_i32("fu_n", fu_P.n);
    aos_hal_pref_set_i32("fu_mode", fu_P.mode);
    aos_hal_pref_set_i32("fu_theme", fu_P.theme);
    aos_hal_pref_set_i32("fu_snd", fu_P.sound ? 1 : 0);
    aos_hal_pref_set_i32("fu_ach", (int32_t)fu_P.ach);
    aos_hal_pref_set_i32("fu_thm", fu_P.themes_used);
    aos_hal_pref_set_i32("fu_sizes", fu_P.sizes_played);
}

static void key_of(char *out, size_t len, const char *what, int n, int mode)
{
    snprintf(out, len, "fu_%s%d%d", what, n, mode);
}

uint32_t fu_record(int n, int mode)
{
    char k[16];
    key_of(k, sizeof k, "hi", n, mode);
    int32_t v = get(k, 0);
    return v > 0 ? (uint32_t)v : 0;
}

uint8_t fu_record_tile(int n, int mode)
{
    char k[16];
    key_of(k, sizeof k, "bt", n, mode);
    int32_t v = get(k, 0);
    return (uint8_t)(v > 0 && v <= FU_EXP_MAX ? v : 0);
}

bool fu_record_offer(int n, int mode, uint32_t score, uint8_t tile)
{
    char k[16];
    bool better = false;
    if (score > fu_record(n, mode)) {
        key_of(k, sizeof k, "hi", n, mode);
        aos_hal_pref_set_i32(k, (int32_t)score);
        better = true;
    }
    if (tile > fu_record_tile(n, mode)) {
        key_of(k, sizeof k, "bt", n, mode);
        aos_hal_pref_set_i32(k, tile);
    }
    return better;
}

static void save_path(char *out, size_t len)
{
    snprintf(out, len, "%s/fusion.sav", aos_hal_path_data());
}

void fu_game_save(void)
{
    char p[160];
    save_path(p, sizeof p);
    if (!fu_G.active || fu_G.over) {
        remove(p);
        return;
    }
    FILE *f = fopen(p, "wb");
    if (!f) return;
    uint32_t head[2] = { SAVE_MAGIC, (uint32_t)sizeof fu_G };
    fwrite(head, sizeof head, 1, f);
    fwrite(&fu_G, sizeof fu_G, 1, f);
    fclose(f);
}

bool fu_game_load(void)
{
    char p[160];
    save_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    uint32_t head[2] = { 0, 0 };
    fu_game_t g;
    bool ok = fread(head, sizeof head, 1, f) == 1 && head[0] == SAVE_MAGIC &&
              head[1] == sizeof g && fread(&g, sizeof g, 1, f) == 1;
    fclose(f);
    if (!ok || !g.active || g.over || g.n < FU_NMIN || g.n > FU_NMAX || g.mode >= FU_MODES) return false;
    fu_G = g;
    /* the ids must not collide with the ones handed out from now on */
    uint16_t top = 0;
    for (int y = 0; y < g.n; y++)
        for (int x = 0; x < g.n; x++)
            if (g.g.id[y][x] > top) top = g.g.id[y][x];
    while (fu_new_id() < top) {
    }
    return true;
}

void fu_game_forget(void)
{
    char p[160];
    save_path(p, sizeof p);
    remove(p);
}
