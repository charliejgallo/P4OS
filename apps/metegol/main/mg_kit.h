/*
 * METEGOL - the teams and their kits
 *
 * Two leagues: national teams (with their flag) and clubs of Argentina
 * (their name and colours: no crests, nothing that imitates one). The
 * list lives in /data/metegol/equipos.txt, which the app writes the first
 * time with what is below and which the portal's page edits; a team is
 * a block:
 *
 *     [boca]
 *     nombre = Boca Juniors
 *     corto = BOC
 *     liga = clubes
 *     diseño = franja
 *     color1 = 0B3D91
 *     color2 = FCD116
 *     mangas = 0B3D91
 *     cuello = FCD116
 *     pantalon = 0B3D91
 *     medias = 0B3D91
 *     estadio = laboca
 *     bandera = h:75AADB,FFFFFF,75AADB;disc:F6B40E:0.5,0.5,0.13   (teams of the national league)
 *
 * The figures are rendered once (tools/blender/figures.py): every pixel
 * says which part of the figure it is (skin, hair, shirt, sleeve, collar,
 * shorts, socks, boots), how much light it takes, and where on the torso
 * it lies (forward, left, height, in the torso's own space). A kit is
 * then painted at load time: the shirt's design is a function of that
 * place (stripes go round the body, Boca's band at the chest's height,
 * River's sash on a slanted plane ...), so it wraps the body and turns
 * with it.
 *
 * The masks (PK_FIGS):
 *   'MGFG' u16 frames u16 cols
 *   frames x { i16 ox, oy; u16 w, h; u32 off }
 *   at off: w*h x { u8 alpha, shade, part, fwd, left, height }
 *   shade: 160 is the colour as it is, above it a highlight; fwd and left
 *   are -1..1 as 1..255 (128 the middle), height 0..1 as 0..255.
 */
#pragma once

#include "mg_gfx.h"

#include <stdbool.h>
#include <stdint.h>

#define MG_TEAMS_MAX 40

enum { MG_LEAGUE_NAT, MG_LEAGUE_CLUB, MG_LEAGUE_N };

/* the shirt's design */
enum {
    MG_D_PLAIN,     /* lisa */
    MG_D_STRIPES,   /* rayas: thin vertical stripes */
    MG_D_BARS,      /* bastones: wide vertical stripes */
    MG_D_BAND,      /* franja: a band across the chest */
    MG_D_SASH,      /* banda: diagonal, shoulder to hip */
    MG_D_V,         /* v: a V from the shoulders */
    MG_D_HALVES,    /* mitades: left and right */
    MG_D_CHECKS,    /* cuadros */
    MG_D_TRIM,      /* vivos: collar, cuffs and the sides */
    MG_D_HOOPS,     /* aros: horizontal hoops */
    MG_D_N
};

/* the parts of a figure, as the masks have them */
enum { MG_P_NONE, MG_P_SKIN, MG_P_HAIR, MG_P_SHIRT, MG_P_SLEEVE, MG_P_COLLAR, MG_P_SHORTS, MG_P_SOCKS, MG_P_BOOTS,
       MG_P_RAW, MG_P_N };

typedef struct {
    char     id[20];
    char     name[36];
    char     abbr[4];
    uint8_t  league;
    uint8_t  design;
    uint32_t c1, c2, sleeves, collar, shorts, socks;
    char     stadium[24];
    char     flag[160];
} mg_team_t;

typedef struct {
    mg_team_t t[MG_TEAMS_MAX];
    int       n;
    uint32_t  mtime;            /* of the file it came from */
} mg_teams_t;

extern const char *const MG_DESIGN_WORD[MG_D_N];

/* the teams from the file, written first if it is not there */
void mg_teams_load(mg_teams_t *ts, const char *dir);
/* the file changed since (the portal's page)? */
bool mg_teams_changed(const mg_teams_t *ts, const char *dir);
int  mg_teams_of(const mg_teams_t *ts, int league, int *out, int max);
int  mg_teams_find(const mg_teams_t *ts, const char *id);

/* the shirt's colour at a point of the torso (f, l in -1..1, z in 0..1) */
uint32_t mg_kit_shirt(const mg_team_t *t, float f, float l, float z);
/* a figure's sheet in the team's kit; mirrored for the side that attacks
 * towards -x (the same figure turned round). A new block (mg_free) */
uint8_t *mg_kit_paint(const uint8_t *masks, size_t len, const mg_team_t *t, bool mirror, size_t *out_len);
/* the flag (national teams) or a patch of the shirt (clubs), w x h RGB565 */
void mg_kit_badge(const mg_team_t *t, uint16_t *px, int w, int h);
/* the closest of two kits clash? (the same main colour): then the visitor
 * changes, mg_kit_away() gives its other kit */
bool mg_kit_clash(const mg_team_t *a, const mg_team_t *b);
void mg_kit_away(const mg_team_t *t, mg_team_t *out);
