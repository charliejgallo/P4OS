/*
 * BASTION - what the common pack holds, loaded: the towers' and aliens'
 * sheets (only the ones a battle needs: the towers owned, the aliens its
 * waves bring, turned to the world's hue), the fort, the icons, the fonts.
 */
#pragma once

#include "bs_data.h"
#include "bs_gfx.h"
#include "bs_pak.h"

enum { FONT_S, FONT_M, FONT_L, FONT_N };

/* the frames of the "ui" sheet, in tools/pack_p4.py's order */
enum {
    IC_COIN, IC_HEART, IC_WAVE, IC_PAUSE, IC_SPEED1, IC_SPEED2, IC_SPEED3, IC_PLAY, IC_UPGRADE, IC_SELL,
    IC_TARGET, IC_STRIKE, IC_FREEZE, IC_REPAIR, IC_LOCK, IC_STAR, IC_STAR_OFF, IC_CORE, IC_CLOSE, IC_JAM, IC_N
};

typedef struct {
    bs_pak_t   pak;
    bool       open;
    bs_sheet_t tw_base[TW_N][BS_TIERS], tw_head[TW_N][BS_TIERS];
    bs_sheet_t alien[AL_N];
    bs_sheet_t fort, missile, drone, ui, tw_icon, al_icon;
    bs_font_t  font[FONT_N];
    int        hue;
    uint32_t   aliens;          /* the ones loaded */
    int        missing;         /* sheets a battle asked for and the pack lacks */
} bs_art_t;

/* path: /apps/bastion_p4.pak */
bool bs_art_open(bs_art_t *a, const char *path);
void bs_art_close(bs_art_t *a);
/* what a battle needs; what the last one needed and this one does not goes */
bool bs_art_battle(bs_art_t *a, uint32_t alien_mask, const uint8_t *owned, int hue);
void bs_art_battle_free(bs_art_t *a);
