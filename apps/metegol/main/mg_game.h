/*
 * METEGOL - a match: the table, its rods and the ball, in millimetres
 *
 * Seen from above, the field is 1200 x 680 mm with its centre at the
 * origin: x along it (the goals at -600 and +600), y across it, growing
 * towards the camera (the bottom of the screen). Side 0 defends the goal
 * at -600 and attacks towards +x; side 1 the other way.
 *
 * Each side has four rods, as on a real table: the keeper (1 figure), the
 * defence (2), the midfield (5) and the attack (3). A rod slides across
 * the field between its stops and turns about its own axis; a figure's
 * foot reaches the ball's height only while the rod is within ~55 degrees
 * of hanging straight down, and there it is a capsule whose x depends on
 * the angle (D tan a) and whose speed comes from the rod's turn and slide.
 *
 * The simulation runs in fixed steps of 1/240 s (MG_HZ), with more
 * substeps when the ball or a foot is fast, in float. Nothing here knows
 * LVGL, the screen or the board: tools/harness.c links it to let the
 * computer play itself.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define MG_HZ        240
#define MG_DT        (1.0f / MG_HZ)

#define MG_FIELD_X   600.0f      /* half length */
#define MG_FIELD_Y   340.0f      /* half width */
#define MG_GOAL_Y    100.0f      /* half the mouth */
#define MG_GOAL_D    70.0f       /* the goal's depth behind the line */
#define MG_CORNER_R  55.0f
#define MG_BALL_R    17.5f
#define MG_ROD_Z     58.0f       /* the rods' axis above the field */
#define MG_FOOT_D    (MG_ROD_Z - MG_BALL_R)   /* rod axis to the ball's centre height */
#define MG_FOOT_R    10.0f       /* the foot's capsule: radius ... */
#define MG_FOOT_HALF 8.0f        /* ... and half its length across the field */
#define MG_FOOT_MAX  0.97f       /* radians: past this the foot is over the ball */
#define MG_RAISED    1.45f       /* the angle of raised figures (feet forward) */

#define MG_RODS      8
#define MG_FIGS_MAX  5
#define MG_SIDE_RODS 4

enum { MG_GK, MG_DEF, MG_MID, MG_ATT };

/* what the match is doing */
enum {
    MG_ST_SERVE,      /* the ball comes in from the side wall */
    MG_ST_PLAY,
    MG_ST_GOAL,       /* the ball is in the net: the shout */
    MG_ST_REPLAY,     /* the goal again */
    MG_ST_HALF,       /* half time */
    MG_ST_END,
};

/* sounds the step asks for (mg_audio plays them) */
enum {
    SND_KICK, SND_KICK_SOFT, SND_WALL, SND_POST, SND_NET, SND_WHISTLE, SND_WHISTLE_END, SND_ROAR, SND_SERVE,
    SND_STOP, SND_TAP, SND_OOH, SND_N
};

/* difficulty of the computer */
enum { MG_EASY, MG_NORMAL, MG_HARD, MG_CRACK, MG_DIFF_N };

typedef struct {
    float x;            /* where it crosses the field */
    int   side;
    int   role;         /* MG_GK ... */
    int   n;            /* figures */
    float base[MG_FIGS_MAX];  /* each figure's y with the rod centred */
    float lo, hi;       /* the stops: the offset's range */
    /* state */
    float off, voff;    /* slide (mm) and its speed */
    float ang, w;       /* turn (rad, + towards the side's attack) and its speed */
    float tgt_off;      /* where the hand puts it */
    float raise_t;      /* held up for this long (a team-mate's shot) */
    int   kick;         /* 0 none, 1 turning (a kick is a whole turn) */
    float kick_turn;    /* how much of it is done */
    float kick_w;       /* the turn's speed of this kick */
    float kick_spin;
    float kick_slide;   /* mm/s of slide during the kick (a pull shot) */
    bool  hold;         /* raised by the hand */
    float hit_t;        /* time since it last touched the ball */
} mg_rod_t;

/* what a side's hand (a finger, a gamepad, the computer) asks for */
typedef struct {
    bool  all;          /* moves every rod at once; false: only 'rod' */
    int   rod;          /* the rod a lone hand moves (0..3 of the side) */
    float move;         /* mm to slide this step (relative) */
    bool  absolute;     /* set: 'target' per rod instead of 'move' */
    float target[MG_SIDE_RODS];
    float speed;        /* the most it may slide, mm/s */
    int   kick_rod;     /* -1: none; else the rod (0..3 of the side) that kicks now */
    float kick_power;   /* 0..1 */
    float kick_spin;    /* -1..1: effect */
    float kick_slide;   /* mm/s: lateral sweep during the kick */
    bool  raise;        /* hold the figures up */
    uint8_t raise_mask; /* or only these rods (bit per rod of the side) */
} mg_hand_t;

typedef struct {
    int   diff;
    float react;        /* seconds behind the ball */
    float err;          /* mm of error following it */
    float speed;        /* mm/s of slide */
    float power;        /* 0..1 of a full kick */
    float aim;          /* 0..1: how well it aims (pull shots) */
    float miss;         /* chance of a kick at the wrong moment */
    float wait;         /* seconds it lets a ball sit before it plays it */
    float delay;        /* seconds a ball must be at a foot before it kicks */
    float cool;         /* seconds after a kick before the next */
} mg_ai_cfg_t;

#define MG_HIST 96      /* the ball's last positions, for the computer's lag */

typedef struct {
    float noise[MG_SIDE_RODS], noise_v[MG_SIDE_RODS];
    float think;        /* seconds to the next decision */
    float hold_t;
    uint32_t rnd;
    float aim_y;
    int   plan_rod;
    float reach_t;      /* how long the ball has been at one of its feet */
} mg_ai_t;

/* one frame of the replay: 60 a second */
typedef struct {
    int16_t bx, by;             /* 0.1 mm */
    int16_t off[MG_RODS];       /* 0.1 mm */
    int16_t ang[MG_RODS];       /* 1/4096 rad */
    uint8_t roll;
    uint8_t vis;
} mg_rec_t;

#define MG_REC_N 300            /* five seconds */

typedef struct {
    /* the rules of this match */
    int   halves_min;           /* minutes a half; 0: to 'goals' */
    int   goals;                /* to this many; 0: by time */
    bool  swap_sides;           /* change ends at half time */
    float ball_speed;           /* 0.8 .. 1.25 */

    /* the table */
    mg_rod_t rod[MG_RODS];      /* side 0's four, then side 1's, each in MG_GK.. order */
    float bx, by, vx, vy, spin;
    float roll;                 /* distance rolled, for the ball's picture */
    bool  ball_in;              /* on the field (or in a net) */

    int   state;
    float state_t;
    int   score[2];
    int   half;                 /* 0, 1 */
    bool  swapped;              /* the teams changed ends at half time */
    float clock;                /* seconds played in this half */
    int   last_goal;            /* the side that scored */
    int   serve_side;           /* which wall the ball comes from: 0 far, 1 near */
    float idle_t;               /* the ball has hardly moved for this long */
    float far_t;                /* the ball is where nobody reaches it */
    int   last_touch;           /* side, -1 none */
    int   last_rod;             /* the rod that touched it last, -1 none */
    float near_goal;            /* 0..1: how close the ball is to some goal (the crowd) */

    /* the computer */
    bool  ai_on[2];
    mg_ai_cfg_t ai_cfg[2];
    mg_ai_t ai[2];
    float hist_x[MG_HIST], hist_y[MG_HIST], hist_vx[MG_HIST], hist_vy[MG_HIST];
    int   hist_at;

    /* the replay */
    mg_rec_t rec[MG_REC_N];
    int   rec_at, rec_n;
    float rec_acc;
    int   rp_from, rp_len;
    float rp_t;
    mg_rod_t rod_keep[MG_RODS];  /* the rods as the goal left them */

    /* counts (the harness, the log) */
    int   kicks, stuck, deads, shots[2];
    float max_speed;

    /* sounds asked for by the step, and their loudness 0..255 */
    uint8_t snd[16], snd_vol[16];
    int   nsnd;
    float wall_t, kick_t;       /* the same sound again not too soon */

    uint32_t rnd;
    float t;                    /* seconds since the match began */
} mg_game_t;

void mg_ai_config(mg_ai_cfg_t *c, int diff);

/* a new match: the rules, who the computer plays (ai[side]) and how well */
void mg_game_start(mg_game_t *g, int halves_min, int goals, bool swap, bool ai0, int diff0, bool ai1, int diff1);
/* one step of 1/240 s; hands[side] for the sides played by people (NULL: the computer, or nothing) */
void mg_game_step(mg_game_t *g, const mg_hand_t *hand0, const mg_hand_t *hand1);
/* the computer's hand for a side (mg_ai.c), once a step */
void mg_ai_hand(mg_game_t *g, int side, mg_hand_t *out);
/* the side's rod nearest the ball (0..3) */
int  mg_nearest_rod(const mg_game_t *g, int side);
/* the figure of a rod nearest to y */
int  mg_nearest_fig(const mg_rod_t *r, float y);
/* where a figure's foot is (x of its contact with the ball, y) and whether it reaches the ball */
bool mg_foot(const mg_rod_t *r, int fig, float *fx, float *fy);
/* the replay: frame k of the last goal (0..rp_len-1) into the game's table */
void mg_replay_apply(mg_game_t *g, int k);
/* the replay is over (or skipped): the rods as they were, and a serve */
void mg_replay_end(mg_game_t *g);
bool mg_over(const mg_game_t *g);
/* the side ahead, -1 a draw */
int  mg_winner(const mg_game_t *g);
/* an angle into (-pi, pi] */
static inline float mg_wrap(float a)
{
    while (a > 3.14159265f) a -= 6.28318531f;
    while (a <= -3.14159265f) a += 6.28318531f;
    return a;
}
/* rod index of side s, role r */
static inline int mg_rod_ix(int side, int role) { return side * MG_SIDE_RODS + role; }
