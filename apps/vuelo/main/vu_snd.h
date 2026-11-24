/*
 * PLANEO - the sound, made as it plays: the engine (a pulse whose pitch
 * follows the throttle and the speed, with the propeller's beat), the wind
 * (noise that opens and grows with the speed) and the cues of a mission.
 * 16 kHz into the speaker's stream, kept ~100 ms ahead from LVGL's timer.
 */
#pragma once

#include <stdbool.h>

enum {
    VS_RING = 0, VS_PICK, VS_DROP, VS_COUNT, VS_GO, VS_FINISH, VS_LOST, VS_CRASH, VS_CLICK, VS_STALL,
};

void vs_open(void);
void vs_close(void);
void vs_enable(bool on);
/* thr 0..1, speed m/s; flying false: the engine idles down and the wind dies */
void vs_set(float thr, float speed, bool flying);
void vs_cue(int id);
void vs_tick(void);
