/*
 * BASTION - the sound: every effect and the music synthesised by
 * tools/sounds.py (nothing recorded), mu-law at 16 kHz in the common
 * pack; a mixer of twelve voices over one track of music on the
 * streaming speaker. Pumped from LVGL's thread.
 */
#pragma once

#include "bs_pak.h"

#include <stdbool.h>

#define BS_MUSIC_MENU 4

/* loads the effects (the pack stays the caller's) */
bool bs_audio_open(bs_pak_t *pak);
void bs_audio_close(void);
void bs_audio_set(bool sfx, bool music);
/* the track that plays (0..3 the worlds', BS_MUSIC_MENU the menus', -1 none) */
void bs_audio_music(bs_pak_t *pak, int track);
void bs_audio_pause(bool paused);
void bs_audio_play(int snd, int vol);
void bs_audio_pump(void);
