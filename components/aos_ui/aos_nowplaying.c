/*
 * P4OS - What is playing, for the control centre and the lock screen.
 *
 * Two sources: the board's own player (the card's music and the radio) and
 * the iPhone's, over AMS (aos_hal_media_*, off until Settings, Bluetooth,
 * turns it on). One row shows one of them, chosen the way AmoledOS's control
 * centre chose: the board when it is playing, since it is what the speaker
 * says; else the phone with a track, playing or paused; else the board,
 * paused. The buttons go to whichever is shown, decided again at the tap so
 * they never act on a source the row stopped showing.
 */
#include "aos_internal.h"
#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <string.h>

typedef enum { SRC_NONE, SRC_BOARD, SRC_PHONE } src_t;

static src_t pick(aos_player_info_t *pi, aos_media_info_t *mi)
{
    bool board = aos_hal_player_info(pi) && pi->state != AOS_PLAYER_STOPPED;
    bool phone = aos_hal_media_link() == AOS_MEDIA_CONNECTED && aos_hal_media_info(mi) &&
                 (mi->playing || (mi->has_metadata && mi->title[0]));
    if (board && pi->state == AOS_PLAYER_PLAYING) return SRC_BOARD;
    if (phone && (mi->playing || !board)) return SRC_PHONE;
    return board ? SRC_BOARD : SRC_NONE;
}

bool aos_np_get(aos_np_t *o)
{
    static aos_player_info_t pi;        /* 400 bytes: off the LVGL task's stack */
    aos_media_info_t mi;
    memset(o, 0, sizeof *o);
    switch (pick(&pi, &mi)) {
    case SRC_BOARD:
        o->on = true;
        o->playing = pi.state == AOS_PLAYER_PLAYING;
        /* a station between songs says its own name */
        snprintf(o->title, sizeof o->title, "%s", pi.title[0] ? pi.title : pi.live && pi.album[0] ? pi.album : "");
        snprintf(o->artist, sizeof o->artist, "%s", pi.artist);
        return true;
    case SRC_PHONE: {
        o->on = o->phone = true;
        o->playing = mi.playing;
        snprintf(o->title, sizeof o->title, "%s", mi.title);
        snprintf(o->artist, sizeof o->artist, "%s", mi.artist);
        const char *who = aos_hal_media_player();
        snprintf(o->from, sizeof o->from, "%s", who && who[0] ? who : "iPhone");
        return true;
    }
    default:
        return false;
    }
}

void aos_np_command(aos_media_cmd_t cmd)
{
    static aos_player_info_t pi;
    aos_media_info_t mi;
    aos_hal_activity();
    switch (pick(&pi, &mi)) {
    case SRC_PHONE:
        aos_hal_media_command(cmd);
        return;
    case SRC_BOARD:
        if (cmd == AOS_MEDIA_NEXT) aos_hal_player_next();
        else if (cmd == AOS_MEDIA_PREV) aos_hal_player_prev();
        else if (pi.state == AOS_PLAYER_PLAYING) aos_hal_player_pause();
        else aos_hal_player_resume();
        return;
    default:
        if (cmd == AOS_MEDIA_PLAY_PAUSE) aos_hal_player_resume_last();   /* nothing yet: the last one */
        return;
    }
}
