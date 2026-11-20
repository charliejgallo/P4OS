/*
 * BASTION - the player's progress (see bs_save.h)
 */
#include "bs_save.h"

#include "aos_hal.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC   0x56415342u     /* "BSAV" */
#define VERSION 1

static uint32_t sum(const bs_save_t *s)
{
    const uint8_t *b = (const uint8_t *)s;
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < offsetof(bs_save_t, check); i++) h = (h ^ b[i]) * 0x01000193u;
    return h;
}

static void path(char *p, size_t n, const char *ext)
{
    const char *d = aos_hal_path_data();
    snprintf(p, n, "%.120s/bastion.%s", d ? d : ".", ext);
}

void bs_save_reset(bs_save_t *s)
{
    memset(s, 0, sizeof *s);
    s->magic = MAGIC;
    s->version = VERSION;
    s->size = (uint16_t)sizeof *s;
    s->sfx = s->music = 1;
    s->speed = 1;
    for (int i = 0; i < TW_N; i++) s->owned[i] = BS_TOWER[i].unlock == 0;
}

void bs_save_load(bs_save_t *s)
{
    char p[160];
    path(p, sizeof p, "sav");
    bs_save_reset(s);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    bs_save_t t;
    size_t n = fread(&t, 1, sizeof t, f);
    fclose(f);
    if (n == sizeof t && t.magic == MAGIC && t.version == VERSION && t.size == sizeof t && t.check == sum(&t)) {
        *s = t;
        /* whatever the file says, the towers of the start are owned */
        for (int i = 0; i < TW_N; i++) {
            if (BS_TOWER[i].unlock == 0) s->owned[i] = 1;
            if (s->mastery[i] > BS_MASTERY_MAX) s->mastery[i] = BS_MASTERY_MAX;
        }
        for (int i = 0; i < TECH_N; i++) {
            if (s->tech[i] > BS_TECH_MAX) s->tech[i] = BS_TECH_MAX;
        }
        for (int i = 0; i < PW_N; i++) {
            if (s->power[i] > BS_POWER_MAX) s->power[i] = BS_POWER_MAX;
        }
    } else {
        aos_hal_log("bastion", "the save is not ours or is damaged: starting over");
    }
}

void bs_save_store(bs_save_t *s)
{
    char p[160], tmp[160];
    path(p, sizeof p, "sav");
    path(tmp, sizeof tmp, "tmp");
    s->check = sum(s);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    bool ok = fwrite(s, 1, sizeof *s, f) == sizeof *s;
    fclose(f);
    if (ok) {
        remove(p);
        rename(tmp, p);
    }
}

void bs_save_kit(const bs_save_t *s, bs_kit_t *kit)
{
    memset(kit, 0, sizeof *kit);
    memcpy(kit->owned, s->owned, sizeof kit->owned);
    memcpy(kit->mastery, s->mastery, sizeof kit->mastery);
    memcpy(kit->tech, s->tech, sizeof kit->tech);
    memcpy(kit->power, s->power, sizeof kit->power);
    kit->rank = bs_rank_of(s->xp);
}

bool bs_save_open(const bs_save_t *s, int world, int sector)
{
    if (world < 0 || world >= BS_WORLDS_MAX || sector < 0 || sector >= BS_SECTORS) return false;
#if defined(AOS_SIM) || defined(AOS_SIM_BUILTIN)
    if (getenv("BS_OPEN")) return true;
#endif
    if (sector > 0) return s->stars[world][sector - 1] > 0;
    return world == 0 || s->stars[world - 1][BS_SECTORS - 1] > 0;
}

int bs_save_stars(const bs_save_t *s)
{
    int n = 0;
    for (int w = 0; w < BS_WORLDS_MAX; w++) {
        for (int k = 0; k < BS_SECTORS; k++) n += s->stars[w][k];
    }
    return n;
}
