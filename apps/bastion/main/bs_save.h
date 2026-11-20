/*
 * BASTION - what the player keeps between battles: the stars of each
 * sector, the cores to spend in the laboratory and what was bought there,
 * the experience that makes the rank. A small file in the data folder.
 */
#pragma once

#include "bs_data.h"
#include "bs_game.h"

typedef struct {
    uint32_t magic;
    uint16_t version, size;
    int32_t  cores, xp, kills, wins;
    uint8_t  stars[BS_WORLDS_MAX][BS_SECTORS];
    uint8_t  owned[TW_N], mastery[TW_N], tech[TECH_N], power[PW_N];
    uint8_t  sfx, music, speed, spare;
    /* since version 2: the mode without end's record a world (waves
     * held), the difficulty and the mode last played */
    uint16_t best[BS_WORLDS_MAX];
    uint8_t  diff, mode, pad[2];
    uint32_t seen;              /* the aliens met in battle (bits of AL_*): the gallery's cards */
    uint32_t check;
} bs_save_t;

void bs_save_load(bs_save_t *s);
void bs_save_store(bs_save_t *s);
void bs_save_reset(bs_save_t *s);
/* what the laboratory gives a battle */
void bs_save_kit(const bs_save_t *s, bs_kit_t *kit);
/* world: 0 is the first. A sector opens when the one before it is won,
 * a world when the last sector of the one before it is. */
bool bs_save_open(const bs_save_t *s, int world, int sector);
int  bs_save_stars(const bs_save_t *s);
