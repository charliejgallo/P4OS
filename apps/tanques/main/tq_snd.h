/*
 * TANQUES - the sound: shots, blasts, the engine and the shells in the air,
 * synthesised on the streaming speaker (16 kHz, mono). Nothing is recorded:
 * every sound is noise and tones shaped here, so the pack does not grow.
 *
 * The board's speaker is small: nothing below ~150 Hz is heard, so the
 * blasts are a crack and a mid-range roar over a falling tone, not a bass
 * thump. Without the speaker (another app holds it) a few events fall back
 * to the system's beep. Off (the menu's speaker button, saved) closes the
 * speaker.
 */
#pragma once

#include <stdbool.h>

enum {
    TQ_SND_CLICK = 0,       /* a button */
    TQ_SND_DENY,            /* can't: no coins, no AP */
    TQ_SND_BUY,
    TQ_SND_TURN,            /* a person's turn starts */
    TQ_SND_FIRE,            /* the cannon and the like */
    TQ_SND_FIRE_LOB,        /* a mortar's thoomp */
    TQ_SND_FIRE_ROCKET,     /* a missile's whoosh */
    TQ_SND_FIRE_LASER,
    TQ_SND_FIRE_SNIPER,
    TQ_SND_FIRE_SCATTER,
    TQ_SND_FIRE_DRILL,
    TQ_SND_FIRE_CALL,       /* the air strike's radio and plane */
    TQ_SND_BLAST_S,
    TQ_SND_BLAST,
    TQ_SND_BLAST_L,
    TQ_SND_CLANG,           /* a direct hit on a tank */
    TQ_SND_KO,              /* a tank knocked out */
    TQ_SND_MISS,            /* off the map */
    TQ_SND_HEAL,
    TQ_SND_SHIELD,
    TQ_SND_EMP,
    TQ_SND_TELEPORT,
    TQ_SND_BURN,
    TQ_SND_MINES,           /* laid */
    TQ_SND_SMOKE,
    TQ_SND_WALL,
    TQ_SND_QUAKE,
    TQ_SND_WIN,
    TQ_SND_LOSE,
    /* the DLCs' */
    TQ_SND_TESLA,           /* the coil's lightning */
    TQ_SND_SPLASH,          /* into the sea */
    TQ_SND_POINT,           /* a point on the hill */
    TQ_SND_FLAG,            /* a flag taken or brought back */
    TQ_SND_CAPTURE,
    TQ_SND_COIN,            /* a chest */
    TQ_SND_WAVE,            /* a wave comes */
};

void tq_snd_open(void);
void tq_snd_close(void);
/* On or off (closes the speaker when off) */
void tq_snd_enable(bool on);
bool tq_snd_enabled(void);
void tq_snd(int id);
/* The engine while a tank drives: 0 quiet .. 1 */
void tq_snd_engine(float level);
/* A shell in the air: kind 0 a whistle, 1 a rocket's hiss; level 0 quiet;
 * height over the ground in metres (the whistle falls with it) */
void tq_snd_flight(int kind, float level, float height);
/* Tops the speaker up: every frame, and while LVGL's thread waits */
void tq_snd_tick(void);
