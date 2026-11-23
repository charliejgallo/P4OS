/*
 * TANQUES - the art pack (tools/pack.py): the six tanks, each biome's
 * scenery, the gunner's views and the hangar's turntables, as sprites the
 * app colours itself.
 *
 * The pack is big (all six tanks from every side); a battle loads only its
 * tanks and its biome, the hangar only the tank on its turntable. Each is a
 * group read in one go and freed as a whole.
 */
#pragma once

#include "tq_gfx.h"

enum { TQ_SET_TOP = 0, TQ_SET_LOW, TQ_SETS };
enum { TQ_PART_HULL = 0, TQ_PART_TURRET, TQ_PARTS };
/* a biome's five pieces, then the modes' marks (from the DLCs' packs) */
enum { TQ_PROP_TA = 0, TQ_PROP_TB, TQ_PROP_STUMP, TQ_PROP_BLOCK1, TQ_PROP_BLOCK2, TQ_PROPS,
       TQ_PROP_FLAG = TQ_PROPS, TQ_PROP_CHEST, TQ_PROP_BEACON, TQ_PROPS_ALL };

#define TQ_MIPS    3            /* the low set: full, half, quarter */
#define TQ_SHOWS   16           /* the hangar's turntable */

/* Opens the game's pack and the DLCs' there are, reads their tables and
 * tells tq_data which DLCs are installed; false: no game pack */
bool tq_art_open(void);
void tq_art_close(void);
bool tq_art_ready(void);

/* Loads these tank types (and frees the others); with_fp: their gunner's
 * views too (bit per type). false: out of memory or a read failed. */
bool tq_art_use_tanks(uint32_t types, uint32_t with_fp);
bool tq_art_use_biome(int biome);
bool tq_art_use_show(int type);
size_t tq_art_bytes(void);      /* what is loaded */

/* NULL if not loaded */
const tq_spr_t *tq_art_tank(int set, int type, int part, int yaw_index, int mip);
const tq_spr_t *tq_art_prop(int set, int prop, int yaw_index, int mip);
const tq_spr_t *tq_art_roof(int type);
const tq_spr_t *tq_art_barrel(int type, int pitch_index);
const tq_spr_t *tq_art_show(int type, int index);

/* A tank's colours: the two paints, the emblem and its paint */
void tq_art_tank_lut(tq_lut_t *lut, uint32_t paint1, uint32_t paint2, int emblem, uint32_t emblem_paint);
/* The scenery's colours in the loaded biome */
const tq_lut_t *tq_art_scenery_lut(void);
/* The chest's and the beacon's (the flag takes its side's tank's) */
const tq_lut_t *tq_art_marks_lut(void);
