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
#define VERSION 2

/* version 1, as 0.28's first Bastión wrote it: the same fields up to
 * 'spare', then the check. A save of that version is read and kept. */
typedef struct {
    uint32_t magic;
    uint16_t version, size;
    int32_t  cores, xp, kills, wins;
    uint8_t  stars[BS_WORLDS_MAX][BS_SECTORS];
    uint8_t  owned[TW_N], mastery[TW_N], tech[TECH_N], power[PW_N];
    uint8_t  sfx, music, speed, spare;
    uint32_t check;
} save_v1_t;

static uint32_t sum_of(const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x01000193u;
    return h;
}

static uint32_t sum(const bs_save_t *s)
{
    return sum_of(s, offsetof(bs_save_t, check));
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
    memset(&t, 0, sizeof t);
    size_t n = fread(&t, 1, sizeof t, f);
    fclose(f);
    bool ok = n == sizeof t && t.magic == MAGIC && t.version == VERSION && t.size == sizeof t && t.check == sum(&t);
    if (!ok && n == sizeof(save_v1_t)) {
        /* a version 1 save: its fields are this one's first ones */
        save_v1_t o;
        memcpy(&o, &t, sizeof o);
        if (o.magic == MAGIC && o.version == 1 && o.size == sizeof o && o.check == sum_of(&o, offsetof(save_v1_t, check))) {
            bs_save_reset(&t);
            memcpy(&t.cores, &o.cores, offsetof(save_v1_t, check) - offsetof(save_v1_t, cores));
            ok = true;
            aos_hal_log("bastion", "a save of version 1 brought up to date");
        }
    }
    if (ok) {
        *s = t;
        s->version = VERSION;
        s->size = (uint16_t)sizeof *s;
        if (s->diff >= BS_DIFF_N) s->diff = BS_DIFF_NORMAL;
        if (s->mode >= BS_MODE_N) s->mode = BS_MODE_CAMPAIGN;
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
    kit->diff = s->diff;
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
