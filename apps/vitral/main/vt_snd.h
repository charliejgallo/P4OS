/*
 * VITRAL - the sound (see vt_snd.c)
 */
#pragma once

#include <stdbool.h>

enum {
    VT_SND_MOVE, VT_SND_ROTATE, VT_SND_SOFT, VT_SND_LOCK, VT_SND_HARD, VT_SND_HOLD,
    VT_SND_CLEAR1, VT_SND_CLEAR2, VT_SND_CLEAR3, VT_SND_VITRAL, VT_SND_LEVEL,
    VT_SND_OVER, VT_SND_REJECT, VT_SND_WIN, VT_SND_GARBAGE, VT_SND_TAP, VT_SND_TICK,
    VT_SND_N
};

void vt_snd_open(void);         /* renders the sounds the first time, opens the speaker */
void vt_snd_close(void);        /* gives the speaker back (the app goes to the background) */
void vt_snd_free(void);         /* and the sounds */
void vt_snd_enable(bool on);
bool vt_snd_enabled(void);
void vt_snd_play(int snd, int vol);     /* vol 0..256 */
void vt_snd_pump(void);                 /* every frame */
#ifdef AOS_SIM_BUILTIN
void vt_snd_dump(const char *dir);
#endif
