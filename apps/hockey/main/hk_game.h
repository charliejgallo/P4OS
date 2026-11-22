/*
 * HOCKEY - a match: the table, the two mallets and the puck, in millimetres
 *
 * Seen from above, the playing surface is 560 x 1040 mm with its centre at
 * the origin: x across it, y along it, growing towards the camera (the
 * bottom of the screen). Side 0 is player 1, at the bottom: it defends the
 * goal at y = +520 and attacks towards -y. Side 1 is the top, the board or
 * player 2. Each mallet stays whole in its own half.
 *
 * The puck rides on air: it hardly slows down, it bounces off the rails
 * and the round corners, and a mallet hands it its own speed when it hits
 * it. Mallet and puck are swept against each other through every substep
 * (the time of contact is solved, not looked for at the end of the step),
 * so a fast mallet never passes through the puck; a puck squeezed against
 * a rail pushes the mallet back instead.
 *
 * Fixed steps of 1/240 s (HK_HZ), more substeps when something is fast, in
 * float. Nothing here knows LVGL, the screen or the board: tools/harness.c
 * links it to let the computer play itself.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HK_HZ        240
#define HK_DT        (1.0f / HK_HZ)

#define HK_FX        280.0f     /* half the width */
#define HK_FY        520.0f     /* half the length */
#define HK_GOAL_X    92.0f      /* half the goal's mouth */
#define HK_CORNER_R  60.0f
#define HK_PUCK_R    26.0f
#define HK_POST_R    4.0f
#define HK_HOME_Y    (HK_FY - 95.0f)   /* where a mallet waits, from the centre line */

/* the mallet's three sizes (Ajustes) */
#define HK_MALLET_S  33.0f
#define HK_MALLET_M  40.0f
#define HK_MALLET_L  49.0f

/* what the match is doing */
enum {
    HK_ST_SERVE,      /* the puck lies on the table for the side that lost the last goal */
    HK_ST_PLAY,
    HK_ST_GOAL,       /* the puck went in: the light and the sound */
    HK_ST_REPLAY,     /* the goal again */
    HK_ST_END,
};

/* sounds the step asks for (hk_audio plays them) */
enum { SND_HIT, SND_HIT_SOFT, SND_WALL, SND_POST, SND_GOAL, SND_PLACE, SND_BUZZ, SND_BUZZ_END, SND_TAP, SND_N };

/* difficulty of the computer */
enum { HK_EASY, HK_NORMAL, HK_HARD, HK_CRACK, HK_DIFF_N };

typedef struct {
    float x, y;         /* the centre */
    float vx, vy;       /* as it moved in the last step */
    float r;
    float tx, ty;       /* where its hand wants it */
    float speed;        /* the most it moves, mm/s */
    float hit_t;        /* since it last touched the puck */
} hk_mallet_t;

/* what a side's hand (a finger, a gamepad, the computer) asks for */
typedef struct {
    bool  on;           /* false: the mallet stays where it is */
    float x, y;         /* the table's millimetres; kept in the side's half */
    float speed;        /* mm/s */
} hk_hand_t;

typedef struct {
    int   diff;
    float react;        /* seconds behind the puck */
    float err;          /* mm of error, drifting */
    float speed;        /* mm/s it moves about */
    float strike;       /* mm/s of a shot */
    float aim;          /* 0..1: aims at the mouth's corners and off the rails */
    float predict;      /* 0..1: how well it reads a puck that will bounce */
    float reach;        /* mm from its goal it comes out to attack */
    float cool;         /* seconds after a shot before the next */
    float hesitate;     /* seconds a puck in its half waits before it goes for it */
} hk_ai_cfg_t;

#define HK_HIST 160     /* the puck's last positions, for the computer's lag */

enum { AI_DEFEND, AI_SET, AI_SHOT, AI_BACK };

typedef struct {
    int   mode;
    float mode_t;
    float err_x, err_v;
    float aim_x;        /* where in the far goal this shot goes */
    bool  bank;
    float cool;
    float wait;         /* the puck has been in its half for this long */
    uint32_t rnd;
} hk_ai_t;

/* one frame of the replay: 60 a second */
typedef struct {
    int16_t px, py;             /* 0.1 mm */
    int16_t mx[2], my[2];       /* 0.1 mm */
    uint8_t vis;
    uint8_t hit;                /* a mallet hit the puck in this frame */
} hk_rec_t;

#define HK_REC_N 300            /* five seconds */

typedef struct {
    /* the rules of this match */
    int   minutes;              /* by time; 0: to 'goals' */
    int   goals;                /* to this many; 0: by time */
    float puck_speed;           /* 0.8 .. 1.25 */

    hk_mallet_t m[2];
    float px, py, pvx, pvy;
    bool  puck_in;              /* on the table (or going into a goal) */

    int   state;
    float state_t;
    int   score[2];
    float clock;                /* seconds played */
    bool  golden;               /* time ran out level: the next goal wins */
    int   last_goal;            /* the side that scored */
    int   serve_side;           /* who gets the puck on the table */
    float idle_t;               /* the puck has hardly moved for this long */
    int   last_touch;           /* side, -1 none */
    float near_goal;            /* 0..1: how near the puck is to some goal */

    /* the last hit, for the picture */
    float hit_x, hit_y, hit_v;
    uint32_t hits;

    /* the computer */
    bool  ai_on[2];
    hk_ai_cfg_t ai_cfg[2];
    hk_ai_t ai[2];
    float hist_x[HK_HIST], hist_y[HK_HIST], hist_vx[HK_HIST], hist_vy[HK_HIST];
    int   hist_at;

    /* the replay */
    hk_rec_t rec[HK_REC_N];
    int   rec_at, rec_n;
    float rec_acc;
    bool  rec_hit;
    int   rp_from, rp_len;
    float rp_t;
    hk_mallet_t m_keep[2];

    /* counts (the harness, the log) */
    int   shots, rescues;
    float max_speed;

    /* sounds asked for by the step, and their loudness 0..255 */
    uint8_t snd[16], snd_vol[16];
    int   nsnd;
    float wall_t, hit_t;        /* the same sound again not too soon */

    uint32_t rnd;
    float t;                    /* seconds since the match began */
} hk_game_t;

void hk_ai_config(hk_ai_cfg_t *c, int diff);

/* a new match: the rules, who the computer plays (ai[side]) and how well,
 * and the mallets' radius */
void hk_game_start(hk_game_t *g, int minutes, int goals, bool ai0, int diff0, bool ai1, int diff1, float mallet_r);
/* one step of 1/240 s; hands[side] for the sides played by people (NULL: the computer, or nothing) */
void hk_game_step(hk_game_t *g, const hk_hand_t *hand0, const hk_hand_t *hand1);
/* the computer's hand for a side (hk_ai.c), once a step */
void hk_ai_hand(hk_game_t *g, int side, hk_hand_t *out);
/* a point kept where a side's mallet may be */
void hk_mallet_clamp(const hk_game_t *g, int side, float *x, float *y);
/* the replay: frame k of the last goal (0..rp_len-1) into the table */
void hk_replay_apply(hk_game_t *g, int k);
/* the replay is over (or skipped): the mallets as they were, and the serve */
void hk_replay_end(hk_game_t *g);
bool hk_over(const hk_game_t *g);
/* the side ahead, -1 a draw */
int  hk_winner(const hk_game_t *g);
/* +1 for side 0 (its goal at +y), -1 for side 1 */
static inline float hk_dir(int side) { return side ? -1.0f : 1.0f; }
