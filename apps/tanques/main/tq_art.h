/*
 * TANQUES - the art pack (tools/pack.py): the tank, the trees, the stones
 * and the gunner's view, as sprites of four planes the app colours itself.
 */
#pragma once

#include "tq_gfx.h"

enum { TQ_SET_TOP = 0, TQ_SET_LOW, TQ_SETS };
enum { TQ_PART_HULL = 0, TQ_PART_TURRET, TQ_PARTS };
enum { TQ_PROP_ROUND = 0, TQ_PROP_PINE, TQ_PROP_STUMP, TQ_PROP_BLOCK1, TQ_PROP_BLOCK2, TQ_PROPS };

#define TQ_SCHEMES 3
#define TQ_MIPS    3            /* the low set: full, half, quarter */

/* Loads the pack whole into PSRAM and makes the low set's smaller copies.
 * false: no pack (the app says so on screen). */
bool tq_art_load(void);
void tq_art_free(void);
bool tq_art_ready(void);
size_t tq_art_bytes(void);

/* NULL if the pack lacks it */
const tq_spr_t *tq_art_tank(int set, int part, int yaw_index, int mip);
/* props: the low set's blocks come in 8 yaws over 90 degrees */
const tq_spr_t *tq_art_prop(int set, int prop, int yaw_index, int mip);
const tq_spr_t *tq_art_roof(void);
const tq_spr_t *tq_art_barrel(int pitch_index);

/* The paints: 0 orange, 1 blue, 2 green. The scenery's regions are the same
 * in all three. */
const tq_lut_t *tq_art_lut(int scheme);
uint32_t tq_art_scheme_rgb(int scheme);
