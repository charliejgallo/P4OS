/*
 * HOCKEY - the art: the common pack (fonts, sounds) and the table of the
 * match (its picture, its two ends drawn again over what they hide, its
 * mallets in three sizes, its puck).
 *
 * The tables are found by name: every hockey*_p4.pak in /apps is opened
 * and each "<id>.def" in it is a table (the bar's travels in the common
 * pack, the neon one and the ice rink one a pack each, optional).
 */
#pragma once

#include "hk_gfx.h"
#include "hk_pak.h"

#include <stdbool.h>
#include <stdint.h>

#define HK_TABLES_MAX 8

typedef struct {
    char     id[16];
    char     name[3][40];       /* es, en, de */
    uint32_t accent;
    uint32_t side[2];           /* each side's colour: its mallet, its score */
    uint32_t puck;
    bool     glow;              /* neon: the pieces shine and the puck leaves a trail */
    char     path[160];
} hk_table_t;

typedef struct {
    hk_pak_t  pak;              /* the common pack, open while the app is */
    bool      ok;
    hk_font_t f_small, f_score, f_big, f_clock;
    uint8_t  *blk[8];
    int       nblk;

    hk_table_t tb[HK_TABLES_MAX];
    int       ntb;

    /* the match's table */
    int       cur;
    uint16_t *bg;               /* 720 x 1280 */
    hk_sheet_t near, far, puck;
    hk_sheet_t mallet[2][3];    /* side, size */
    uint8_t  *d_near, *d_far, *d_puck, *d_mallet[2][3];
} hk_art_t;

bool hk_art_open(hk_art_t *a, const char *apps_dir);
void hk_art_close(hk_art_t *a);
void hk_tables_scan(hk_art_t *a, const char *apps_dir);
int  hk_table_find(const hk_art_t *a, const char *id);
/* a table's picture and pieces (on a thread of the app's own: it decodes a JPEG) */
bool hk_art_table(hk_art_t *a, int tb);
void hk_art_table_free(hk_art_t *a);
