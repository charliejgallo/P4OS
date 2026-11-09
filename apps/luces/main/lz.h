/*
 * P4OS - Luces: a DMX lighting controller (what the app's files share).
 *
 * The model, from the bottom:
 *
 *   type      what a fixture is: its channels, each with a meaning
 *             (dimmer, red, pan, strobe, a constant...). Built in, and
 *             from CSV files in <card>/luces/aparatos (lz_core.c says the
 *             format).
 *   fixture   a type patched at an address, with a name ("PAR 1").
 *   look      what a fixture shows: intensity, red, green, blue, white,
 *             amber, UV, strobe, pan, tilt (16 bits). The programmer
 *             changes the looks of the selected fixtures; a scene keeps
 *             every fixture's look.
 *   output    every 25 ms the looks (after a fade, a chase step or an
 *             effect) become the universe: each channel from its meaning,
 *             the intensity through the master, colours dimmed by the
 *             intensity when the type has no dimmer channel, blackout on
 *             top. aos_io_dmx sends it.
 *
 * Everything runs in LVGL's thread from the app's timer: the firmware's
 * DMX task does the sending. The show is kept on the card:
 * luces/patch.csv and luces/escenas.txt.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define LZ_TYPES     48
#define LZ_FIX       64
#define LZ_SCENES    48
#define LZ_CH        32         /* channels a type may have */

/* channel meanings */
enum {
    LZ_A_DIM = 0, LZ_A_R, LZ_A_G, LZ_A_B, LZ_A_W, LZ_A_AMBER, LZ_A_UV, LZ_A_STROBE,
    LZ_A_PAN, LZ_A_PAN_F, LZ_A_TILT, LZ_A_TILT_F, LZ_A_CONST, LZ_A_COUNT
};

typedef struct {
    char    name[40];
    char    file[40];           /* "" built in */
    uint8_t n;                  /* channels */
    uint8_t attr[LZ_CH];
    uint8_t def[LZ_CH];         /* the value of a constant channel */
    bool    has_dim, has_color, has_pos, has_strobe, has_w;
} lz_type_t;

typedef struct {
    uint8_t  dim, r, g, b, w, amber, uv, strobe;
    uint16_t pan, tilt;
} lz_look_t;

typedef struct {
    char     name[24];
    int      type;              /* index into the types */
    uint16_t addr;              /* 1..512 */
} lz_fix_t;

typedef struct {
    char      name[24];
    uint16_t  fade_ms;
    lz_look_t look[LZ_FIX];
} lz_scene_t;

/* effects, and test modes (from WALK on): what they are for is in lz_core.c, lz_tick() */
enum { LZ_FX_NONE = 0, LZ_FX_RAINBOW, LZ_FX_WAVE, LZ_FX_SPARKLE, LZ_FX_WALK, LZ_FX_RGBTEST, LZ_FX_BREATHE, LZ_FX_COUNT };

typedef struct {
    /* the library and the show */
    lz_type_t  types[LZ_TYPES];
    int        ntypes;
    lz_fix_t   fix[LZ_FIX];
    int        nfix;
    lz_scene_t *scenes;         /* LZ_SCENES, PSRAM */
    int        nscenes;
    /* the live state */
    lz_look_t  look[LZ_FIX];    /* what the programmer and the scenes set */
    bool       sel[LZ_FIX];
    uint8_t    master;          /* 0..255 */
    bool       blackout;
    /* a fade in progress: from 'from' to 'look' */
    lz_look_t  from[LZ_FIX];
    uint32_t   fade_t0, fade_ms;
    int        cur_scene;       /* the last one recalled, -1 none */
    uint16_t   scene_fade_ms;   /* the fade a recall uses */
    /* the chase: every scene in turn */
    bool       chase;
    uint16_t   bpm;
    uint8_t    chase_fade;      /* % of a step spent fading */
    uint32_t   chase_t;         /* when the current step started */
    uint32_t   taps[4];
    int        ntaps;
    /* the effect, on the fixtures selected when it started */
    int        fx;
    bool       fx_on[LZ_FIX];
    uint8_t    fx_speed;        /* 1..100 */
    /* what goes out */
    uint8_t    uni[513];
    lz_look_t  shown[LZ_FIX];   /* the looks after fades and effects, for the screen */
    uint32_t   changed;         /* bumps when the show (patch, scenes) changes */
} lz_t;

extern lz_t LZ;

/* lz_core.c */
const char *lz_dir(void);                       /* <card>/luces, made if missing */
void lz_types_load(void);                       /* built in + the card's */
int  lz_types_reload(void);                     /* again, the patch kept by type name; how many were lost */
void lz_show_load(void);                        /* patch and scenes */
bool lz_patch_save(void);
bool lz_scenes_save(void);
int  lz_fix_add(int type, uint16_t addr, const char *name);   /* index or -1 */
void lz_fix_remove(int i);
int  lz_next_free(int nch);                     /* the first address where nch channels fit */
bool lz_fix_overlaps(int i);                    /* shares a channel with another fixture */
int  lz_scene_store(const char *name);          /* the current looks as a new scene */
void lz_scene_recall(int i, uint16_t fade_ms);
void lz_scene_delete(int i);
int  lz_scenes_demo(void);                      /* adds five scenes for the patch there is; how many */
void lz_tap(void);                              /* tap tempo */
/* the programmer: on the selected fixtures */
void lz_set_dim(uint8_t v);
void lz_set_rgb(uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void lz_set_strobe(uint8_t v);
void lz_set_pos(uint16_t pan, uint16_t tilt);
void lz_effect(int fx);                         /* on the selected fixtures */
/* every tick (~25-50 ms): fades, chase, effects, the universe */
void lz_tick(void);
const char *lz_attr_name(int a);
lz_look_t lz_shown_color(int i, uint8_t *r, uint8_t *g, uint8_t *b);   /* what fixture i looks like */
