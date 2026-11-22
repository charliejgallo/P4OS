/*
 * METEGOL - the art: the common pack (figures, rods, ball, fonts, sounds)
 * and the stadium of the match (its picture with the table in it, the
 * two bands drawn again over the rods, its crowd), and the two teams
 * painted on the figures.
 *
 * The stadiums are found by name: every metegol*_p4.pak in /apps is
 * opened and each "<id>.def" in it is a stadium (two travel in the common
 * pack, the rest one a pack, optional).
 */
#pragma once

#include "mg_gfx.h"
#include "mg_kit.h"
#include "mg_pak.h"

#include <stdbool.h>
#include <stdint.h>

#define MG_STADIUMS_MAX 16
#define MG_FLASH_MAX    12

enum { MG_ST_BOTH, MG_ST_CLUBS, MG_ST_NATIONS };

typedef struct {
    char     id[24];
    char     name[3][48];       /* es, en, de */
    int      league;            /* MG_ST_* */
    uint32_t accent;
    char     path[160];
} mg_stadium_t;

typedef struct {
    mg_pak_t  pak;              /* the common pack, open while the app is */
    bool      ok;
    uint8_t  *figs;
    size_t    figs_len;
    mg_profile_t rod_prof;
    int       rod_prof_x;       /* the profile's left edge from the rod's axis */
    mg_sheet_t bumper, hnear, hfar, enear, efar, ball;
    mg_font_t f_small, f_score, f_big;
    uint8_t  *blk[16];
    int       nblk;

    mg_stadium_t st[MG_STADIUMS_MAX];
    int       nst;

    /* the match */
    int       cur_st;
    uint16_t *bg;               /* 1280 x 720 */
    mg_sheet_t near, far, crowd;
    uint8_t  *near_d, *far_d, *crowd_d;
    int       crowd_x, crowd_y, crowd_n;
    int       crowd_strips;     /* the band in strips, crowd_n frames each */
    int       nflash;
    int16_t   flash[MG_FLASH_MAX][2];
    /* the two teams, each as it looks from either end (the mirror) */
    mg_sheet_t fig[2][2];
    uint8_t  *fig_d[2][2];
    mg_sheet_t badge[2];
    uint8_t  *badge_d[2];
} mg_art_t;

/* the common pack and the list of stadiums */
bool mg_art_open(mg_art_t *a, const char *apps_dir);
void mg_art_close(mg_art_t *a);
void mg_stadiums_scan(mg_art_t *a, const char *apps_dir);
int  mg_stadium_find(const mg_art_t *a, const char *id);
/* a stadium's picture and bands (on a thread of the app's own: it decodes a JPEG) */
bool mg_art_stadium(mg_art_t *a, int st);
void mg_art_stadium_free(mg_art_t *a);
/* the teams of the match: [0] the home side's, [1] the visitor's */
bool mg_art_teams(mg_art_t *a, const mg_team_t *t0, const mg_team_t *t1);
void mg_art_teams_free(mg_art_t *a);
