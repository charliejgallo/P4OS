/*
 * SUDOKU - what is kept on the card, in /data/sudoku (aos_hal_path_data()),
 * so an update of the app loses nothing:
 *
 *   datos.txt     coins, the themes bought, settings and statistics, as
 *                 key=value lines (a new key in a later version is just
 *                 missing in an old file, and gets its default)
 *   partida.bin   the game in progress, as it is in memory, behind a magic
 *                 and its size (a different size is a different version:
 *                 it is dropped rather than read wrong)
 *   reserva.bin   the generator's stock (sd_gen.c)
 */
#include "sd_app.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

sd_data_t sd_D;

void sd_save_dir(char *out, size_t n)
{
    snprintf(out, n, "%.200s/sudoku", aos_hal_path_data());
    mkdir(out, 0755);
}

static void path_of(char *out, size_t n, const char *name)
{
    char dir[240];
    sd_save_dir(dir, sizeof dir);
    snprintf(out, n, "%.240s/%.30s", dir, name);
}

static void defaults(void)
{
    memset(&sd_D, 0, sizeof sd_D);
    sd_D.owned = 1;             /* Clásico is free */
    sd_D.theme = 0;
    sd_D.mark_errors = true;
    sd_D.hl_same = true;
    sd_D.hl_peers = true;
    sd_D.auto_clean = true;
    sd_D.sound = true;
    sd_D.anim = true;
}

void sd_data_load(void)
{
    defaults();
    sd_D.loaded = true;
    char p[300];
    path_of(p, sizeof p, "datos.txt");
    FILE *f = fopen(p, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *k = line, *v = eq + 1;
        uint32_t x = (uint32_t)strtoul(v, NULL, 10);
        if (!strcmp(k, "coins")) sd_D.coins = x;
        else if (!strcmp(k, "coins_total")) sd_D.coins_total = x;
        else if (!strcmp(k, "owned")) sd_D.owned = x | 1u;
        else if (!strcmp(k, "theme")) sd_D.theme = (uint8_t)(x < SD_THEMES ? x : 0);
        else if (!strcmp(k, "mark_errors")) sd_D.mark_errors = x != 0;
        else if (!strcmp(k, "hl_same")) sd_D.hl_same = x != 0;
        else if (!strcmp(k, "hl_peers")) sd_D.hl_peers = x != 0;
        else if (!strcmp(k, "auto_clean")) sd_D.auto_clean = x != 0;
        else if (!strcmp(k, "sound")) sd_D.sound = x != 0;
        else if (!strcmp(k, "anim")) sd_D.anim = x != 0;
        else if (!strcmp(k, "variant")) sd_D.menu_variant = (uint8_t)(x < SD_M_DAILY ? x : 0);
        else if (!strcmp(k, "last_day")) sd_D.last_day = x;
        else if (!strcmp(k, "streak")) sd_D.streak = (uint16_t)x;
        else if (!strcmp(k, "best_streak")) sd_D.best_streak = (uint16_t)x;
        else if (!strcmp(k, "daily_done")) sd_D.daily_done = x;
        else if (!strcmp(k, "daily_time")) sd_D.daily_time_s = x;
        else if (!strcmp(k, "daily_streak")) sd_D.daily_streak = (uint16_t)x;
        else if (!strncmp(k, "st.", 3)) {
            int m = -1, l = -1;
            unsigned a = 0, b = 0, c = 0, d = 0;
            if (sscanf(k + 3, "%d.%d", &m, &l) == 2 && m >= 0 && m < SD_MODES && l >= 0 && l < SD_LEVELS &&
                sscanf(v, "%u,%u,%u,%u", &a, &b, &c, &d) == 4) {
                sd_D.st[m][l].played = (uint16_t)a;
                sd_D.st[m][l].won = (uint16_t)b;
                sd_D.st[m][l].best_s = c;
                sd_D.st[m][l].sum_s = d;
            }
        }
    }
    fclose(f);
    sd_D.owned |= 1u << sd_D.theme;     /* the one in use is always yours */
}

void sd_data_save(void)
{
    char p[300], tmp[310];
    path_of(p, sizeof p, "datos.txt");
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "coins=%u\ncoins_total=%u\nowned=%u\ntheme=%u\n", (unsigned)sd_D.coins,
            (unsigned)sd_D.coins_total, (unsigned)sd_D.owned, (unsigned)sd_D.theme);
    fprintf(f, "mark_errors=%d\nhl_same=%d\nhl_peers=%d\nauto_clean=%d\nsound=%d\nanim=%d\nvariant=%u\n",
            sd_D.mark_errors, sd_D.hl_same, sd_D.hl_peers, sd_D.auto_clean, sd_D.sound, sd_D.anim,
            (unsigned)sd_D.menu_variant);
    fprintf(f, "last_day=%u\nstreak=%u\nbest_streak=%u\ndaily_done=%u\ndaily_time=%u\ndaily_streak=%u\n",
            (unsigned)sd_D.last_day, (unsigned)sd_D.streak, (unsigned)sd_D.best_streak,
            (unsigned)sd_D.daily_done, (unsigned)sd_D.daily_time_s, (unsigned)sd_D.daily_streak);
    for (int m = 0; m < SD_MODES; m++) {
        for (int l = 0; l < SD_LEVELS; l++) {
            const sd_stat_t *s = &sd_D.st[m][l];
            if (!s->played && !s->won) continue;
            fprintf(f, "st.%d.%d=%u,%u,%u,%u\n", m, l, (unsigned)s->played, (unsigned)s->won,
                    (unsigned)s->best_s, (unsigned)s->sum_s);
        }
    }
    fclose(f);
    remove(p);
    rename(tmp, p);
}

#define GAME_MAGIC 0x31474453u          /* "SDG1" */

bool sd_game_load(void)
{
    char p[300];
    path_of(p, sizeof p, "partida.bin");
    FILE *f = fopen(p, "rb");
    if (!f) return false;
    uint32_t h[2] = { 0, 0 };
    bool ok = fread(h, 4, 2, f) == 2 && h[0] == GAME_MAGIC && h[1] == sizeof(sd_game_t) &&
              fread(&sd_G, sizeof sd_G, 1, f) == 1;
    fclose(f);
    if (!ok || sd_G.puz.n == 0) {
        memset(&sd_G, 0, sizeof sd_G);
        return false;
    }
    sd_unpack_shape(&sd_G.puz, &sd_shape);
    return sd_G.active;
}

void sd_game_save(void)
{
    char p[300], tmp[310];
    path_of(p, sizeof p, "partida.bin");
    if (!sd_G.active) {
        remove(p);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    uint32_t h[2] = { GAME_MAGIC, sizeof(sd_game_t) };
    bool ok = fwrite(h, 4, 2, f) == 2 && fwrite(&sd_G, sizeof sd_G, 1, f) == 1;
    fclose(f);
    if (ok) {
        remove(p);
        rename(tmp, p);
    } else {
        remove(tmp);
    }
}

void sd_game_forget(void)
{
    char p[300];
    path_of(p, sizeof p, "partida.bin");
    remove(p);
}

/* ---- dates ------------------------------------------------------------------- */

/* Howard Hinnant's days_from_civil: no mktime, no time zone involved. */
uint32_t sd_ymd_days(uint32_t ymd)
{
    int y = (int)(ymd / 10000), m = (int)(ymd / 100 % 100), d = (int)(ymd % 100);
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (uint32_t)(era * 146097 + doe - 719468);
}

bool sd_today(uint32_t *ymd, int *wday)
{
    if (!aos_hal_time_is_valid()) return false;
    struct tm t;
    memset(&t, 0, sizeof t);
    aos_hal_time_now(&t);
    if (t.tm_year < 120) return false;
    if (ymd) *ymd = (uint32_t)((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday);
    if (wday) *wday = t.tm_wday;
    return true;
}

/* ---- puzzles, packed -------------------------------------------------------------- */

void sd_pack(const sd_puzzle_t *p, sd_packed_t *out)
{
    memset(out, 0, sizeof *out);
    out->n = p->shape.n;
    out->diag = p->shape.diag;
    out->ncages = p->shape.ncages;
    out->level = (uint8_t)p->level;
    memcpy(out->cage_of, p->shape.cage_of, SD_MAXC);
    memcpy(out->cage_sum, p->shape.cage_sum, SD_MAXCAGES);
    memcpy(out->given, p->given, SD_MAXC);
    memcpy(out->sol, p->sol, SD_MAXC);
}

void sd_unpack_shape(const sd_packed_t *in, sd_shape_t *s)
{
    sd_shape_init(s, in->n ? in->n : 9, in->diag != 0);
    if (in->ncages) {
        s->ncages = in->ncages;
        memset(s->cage_len, 0, sizeof s->cage_len);
        for (int i = 0; i < s->ncells; i++) {
            int g = in->cage_of[i];
            s->cage_of[i] = (uint8_t)g;
            if (g < SD_MAXCAGES && s->cage_len[g] < SD_CAGE_MAX) s->cage_cell[g][s->cage_len[g]++] = (uint8_t)i;
        }
        memcpy(s->cage_sum, in->cage_sum, SD_MAXCAGES);
        sd_shape_finish(s);
    }
}
