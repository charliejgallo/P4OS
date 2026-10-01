/*
 * P4OS - the board's audio block (aos_audio_p4.c), as seen from the rest of
 * the HAL. Internal: apps see aos_hal_beep / _player_* / _radio_* / _rec_* /
 * _mic_* / _spk_* in aos_hal.h.
 */
#pragma once

#include <stdbool.h>

/* From aos_hal_init(), last: creates the audio task and returns at once. The
 * codecs are brought up by that task (I2C, a few tens of ms); until then the
 * capabilities do not include audio and the player refuses to start. */
void aos_audio_p4_start(void);

/* Both codecs answered (ES8311 speaker, ES7210 microphones): what
 * aos_hal_caps() turns into AOS_CAP_MIC | AOS_CAP_SPEAKER | AOS_CAP_DUPLEX. */
bool aos_audio_p4_up(void);

/* For the app worker, when the board gets one (aos_hal_worker_start_on):
 * the worker started on 'core', or ended (-1). The MP3/AAC decoder moves to
 * the other core, and with no worker it sits on core 0, away from LVGL. The
 * AmoledOS lesson: a task that decodes in float is anchored to the core of
 * its first FPU instruction, so it has to be placed, it cannot float away. */
void aos_audio_p4_worker_core(int core);
