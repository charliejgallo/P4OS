/*
 * Botonera - a grid of big buttons that play the sounds of a folder on the
 * card (<card>/sonidos), several at once.
 *
 * Pieces:
 *   bn_eng.c    the mixer: its own thread, up to BN_VOICES sounds at once,
 *               mixed here and sent to the streaming speaker as one stream
 *               (the firmware's speaker is one mono stream; there is no
 *               mixer of the system's to hand several to)
 *   bn_dec.c    WAV and MP3 to PCM at the mixer's rate, in PSRAM
 *   bn_synth.c  the example sounds, synthesised the first time
 *   bn_grid.c   the pages and the buttons, in <data>/botonera.txt, which the
 *               portal's page writes too
 *   bn_ui.c     the grid, played with every finger (raw touch points)
 *   bn_edit.c   the sheets: a button's editor, the file list, the recorder,
 *               a page's name
 *   botonera.c  the app's entry, its icon, its life cycle
 */
#pragma once

#include "lvgl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BN_TAG          "botonera"
#define BN_APP_ID       "aos.botonera"

#define BN_RATE         32000           /* the mixer's rate, and every clip's */
#define BN_VOICES       16              /* sounds at once */
#define BN_CLIP_MAX_S   60              /* longer files are cut there */
#define BN_CACHE_BYTES  (12u * 1024u * 1024u)   /* decoded sounds kept in PSRAM */

#define BN_COLS_P       3               /* portrait: 3 x 4 */
#define BN_ROWS_P       4
#define BN_PER_PAGE     12              /* and 4 x 3 lying down: the same 12 */
#define BN_PAGES        8
#define BN_PADS         (BN_PAGES * BN_PER_PAGE)

#define BN_NAME_MAX     40              /* bytes, UTF-8 */
#define BN_FILE_MAX     96

/* How a button behaves when it is touched */
typedef enum {
    BN_MODE_CUT = 0,        /* a touch plays it, another stops it */
    BN_MODE_OVER,           /* every touch starts it again, over the others */
    BN_MODE_HOLD,           /* it plays while the finger is on it */
    BN_MODE_COUNT
} bn_mode_t;

typedef struct {
    char     file[BN_FILE_MAX];         /* relative to the sounds folder; "" empty */
    char     name[BN_NAME_MAX];
    uint32_t color;                     /* 0xRRGGBB */
    uint8_t  icon;                      /* index into bn_icons */
    uint8_t  mode;                      /* bn_mode_t */
    uint8_t  loop;
    uint8_t  vol;                       /* 0..100 */
} bn_pad_t;

typedef struct {
    char     name[BN_NAME_MAX];
    bn_pad_t pad[BN_PER_PAGE];
} bn_page_t;

typedef struct {
    int       n_pages;
    bn_page_t page[BN_PAGES];
} bn_grid_t;

/* ---- memory ---- */
void *bn_alloc(size_t bytes);           /* PSRAM, zeroed */
void  bn_free(void *p);

/* ---- paths ---- */
const char *bn_dir(void);               /* <card>/sonidos */
const char *bn_grid_path(void);         /* <data>/botonera.txt */

/* ---- the grid (bn_grid.c) ---- */
void bn_grid_defaults(bn_grid_t *g);    /* the example pages */
bool bn_grid_load(bn_grid_t *g);        /* false: no file (or no usable one) */
bool bn_grid_save(const bn_grid_t *g);
uint32_t bn_grid_mtime(void);
void bn_pad_clear(bn_pad_t *p);
bool bn_pad_empty(const bn_pad_t *p);
/* A name for a file: "grabacion_003.wav" -> "Grabacion 003" */
void bn_pretty(const char *file, char *out, size_t n);

/* The icons a button can have: the key in the file, the glyph on the board */
typedef struct {
    const char *key;
    const char *glyph;
} bn_icon_t;
extern const bn_icon_t bn_icons[];
extern const int bn_n_icons;
int bn_icon_find(const char *key);

extern const uint32_t bn_colors[];
extern const int bn_n_colors;

/* The sound files of the folder, sorted */
typedef struct {
    int   n, cap;
    char (*name)[BN_FILE_MAX];
    uint32_t *size;
    uint32_t *mtime;
} bn_files_t;
int bn_files_find(const bn_files_t *f, const char *name);   /* -1 */
bool bn_files_scan(bn_files_t *f);
void bn_files_free(bn_files_t *f);
bool bn_is_sound(const char *name);

/* ---- decoding (bn_dec.c) ---- */
typedef struct bn_clip {
    int16_t *pcm;                       /* BN_RATE, mono */
    uint32_t n;                         /* samples */
    bool     cut;                       /* the file was longer than BN_CLIP_MAX_S */
    /* who holds it: 'starts' is the UI's (plays it sent), 'ends' the mixer's
     * (voices of it that ended, or plays it could not take); free only when
     * they meet */
    volatile uint32_t starts, ends;
} bn_clip_t;

/* set while the app goes: a decode in progress gives up */
extern volatile bool bn_dec_abort;

/* NULL with a reason ("formato", "no abre"...) in 'why' */
bn_clip_t *bn_decode(const char *path, char *why, size_t why_n);
void bn_clip_free(bn_clip_t *c);

/* ---- the example sounds (bn_synth.c) ---- */
typedef struct {
    const char *file;
    const char *name;                   /* Spanish, N_(): shown through _() */
    const char *icon;
    uint32_t    color;
    uint8_t     mode, loop;
} bn_example_t;
extern const bn_example_t bn_examples[];
extern const int bn_n_examples;
/* Writes the ones that are not in the folder; returns how many it wrote */
int bn_synth_missing(void);

/* ---- the mixer (bn_eng.c) ---- */
bool bn_eng_start(void);                /* opens the speaker and the thread */
void bn_eng_stop(void);                 /* everything stops, the speaker goes */
bool bn_eng_running(void);
/* 'pad' is the global index (page * BN_PER_PAGE + slot), or -1 for a
 * preview. gain 0..1. */
bool bn_eng_play(int pad, bn_clip_t *clip, float gain, bool loop);
void bn_eng_stop_pad(int pad);
void bn_eng_stop_all(void);
void bn_eng_gain(int pad, float gain);
/* What the mixer says: voices of a pad, and how far the newest one is
 * (0..1000) */
int  bn_eng_voices(int pad);
int  bn_eng_progress(int pad);
bool bn_eng_busy(int pad);              /* playing, or a play on its way */
uint32_t bn_eng_load_pct(void);         /* the thread's time per block, % */
/* a clip that nothing plays any more can go */
bool bn_eng_clip_idle(const bn_clip_t *c);

/* ---- the UI (bn_ui.c, bn_edit.c) ---- */
typedef struct bn_ui bn_ui_t;
bn_ui_t *bn_ui_new(lv_obj_t *root);
void bn_ui_free(bn_ui_t *u);
void bn_ui_show(bn_ui_t *u);
void bn_ui_hide(bn_ui_t *u);
void bn_ui_tick(bn_ui_t *u);
bool bn_ui_back(bn_ui_t *u);
void bn_ui_relayout(bn_ui_t *u, lv_obj_t *root);
