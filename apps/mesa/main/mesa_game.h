/*
 * P4OS - Juegos de mesa: what a game is to the app.
 *
 * Every game the app plays (chess, draughts, reversi... and whatever a
 * module brings later) is a table of this kind. The app knows nothing of a
 * game but what the table says: how big its board is, where its cells sit,
 * which moves are legal, how a move changes the game, which pieces stand
 * where, and how the board plays it. A game knows nothing of P4OS: it never
 * touches LVGL, the screen, the card or the speaker; what it needs from
 * outside comes through the host table (mesa_host_t).
 *
 * The state. A game's whole state is a plain block of state_size bytes
 * with no pointers in it, so the app copies it with memcpy (to think on a
 * copy, to keep the position before a move). Everything is deterministic
 * from init() and the moves and rolls applied after it: the app saves a
 * game as its options, its seed and that list, and an undo is the list
 * played again one entry shorter.
 *
 * A move is what a finger does on the board: the cells it touches, in
 * order (from, to; from, over, over, to for a draughts capture; the one
 * cell of a reversi disc). The app offers the legal moves as the person
 * taps: a first tap picks up what can move from that cell, the next ones
 * narrow the moves down until one is complete. Two moves that touch the
 * same cells (a pawn that promotes) differ in 'tag', and the app asks
 * which (choice_kind() says what to show). A move with no steps is a pass,
 * which the app plays by itself after saying why (no move after a roll).
 *
 * Dice. A game that rolls says so with need_roll(); the app animates the
 * dice and calls roll() with a random number; the game turns it into its
 * dice, which dice() shows. A roll is kept in the game's list like a move.
 *
 * Pieces. pieces() lists what stands on the board and beside it (a piece
 * taken goes to a tray, which the app lays out beside the board), each with
 * an id that stays the same from one position to the next (the app animates
 * a piece whose cell changed from its old place to its new one, a piece
 * that is gone off the board, a piece whose kind changed in place). Where
 * a cell is, is the game's: cell_pos() gives the point a piece stands on in
 * board units (u to the right, v away from the first player, the board
 * from (0, 0) to (board_w, board_h)); the app turns board units into pixels
 * with the board's picture.
 *
 * Threads. ai_move() runs on a thread of the app's, on a copy of the state,
 * and must look at ctl->stop often (every few thousand nodes) and return
 * soon once it is set. Everything else is called from LVGL's thread.
 *
 * Modules. A game that does not come with the app is a module of its own,
 * the card's apps/mesa/<name>.so (aos_hal_module_open), with its art in
 * apps/mesa_<name>_p4.pak. The loader finds functions only, so a module
 * exports mesa_module_abi() (MESA_ABI, which the app compares with its
 * own) and mesa_module_game(i), its tables (NULL past the last), and is
 * handed the host table with mesa_module_host() before anything else.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The version of this contract: bump it whenever a struct below changes. */
#define MESA_ABI 1

#define MESA_MAX_STEPS   24     /* cells one move may touch                */
#define MESA_MAX_MOVES   320    /* legal moves in one position             */
#define MESA_MAX_PIECES  96     /* on the board and off it                 */
#define MESA_MAX_PLAYERS 4
#define MESA_MAX_OPTS    4
#define MESA_NO_CELL     0xFF

typedef struct {
    uint8_t  n;                     /* steps; 0 is a pass                    */
    uint8_t  tag;                   /* tells apart moves with the same steps */
    uint16_t spare;
    uint32_t data;                  /* the game's own encoding               */
    uint8_t  step[MESA_MAX_STEPS];
} mesa_move_t;

/* what apply() says happened, for the sounds and the animation */
enum {
    MESA_EV_MOVE    = 1u << 0,
    MESA_EV_CAPTURE = 1u << 1,
    MESA_EV_CHECK   = 1u << 2,
    MESA_EV_PROMOTE = 1u << 3,
    MESA_EV_CASTLE  = 1u << 4,
    MESA_EV_FLIP    = 1u << 5,      /* reversi: discs turned                 */
    MESA_EV_DROP    = 1u << 6,      /* a disc that falls (four in a row)     */
    MESA_EV_HOME    = 1u << 7,      /* a piece got home (ludo, backgammon)   */
    MESA_EV_END     = 1u << 8,      /* the game is over                      */
    MESA_EV_AGAIN   = 1u << 9,      /* the same player goes on (a six)       */
};

enum {
    MESA_PF_LIFT = 1u << 0,         /* drawn a little up (a ludo token waiting) */
    MESA_PF_LAST = 1u << 1,         /* the last move's: marked                  */
    MESA_PF_DIM  = 1u << 2,         /* shown faded                              */
};

typedef struct {
    uint8_t id;                     /* the same piece from one position to the next */
    uint8_t kind;                   /* index into the game's kinds              */
    uint8_t cell;                   /* MESA_NO_CELL: beside the board, in 'tray' */
    uint8_t slot;                   /* its place in a stack on its cell, or in the tray */
    uint8_t tray;                   /* which tray when off the board (a player's) */
    uint8_t flags;                  /* MESA_PF_*                                */
} mesa_piece_t;

/* How a kind of piece looks. 'sprite' names its sheet in the art pack
 * after the board's name: "wk" on the board "aj" is the sheet "aj_wk",
 * and the flat view's is "aj_wk_f". Without a sheet the app draws a disc
 * of 'color' (and 'color2' for its rim), so a game always shows. */
enum {
    MESA_KF_FACING = 1u << 0,       /* frame 1 faces the other way: the app picks
                                       the frame that looks at the board's middle */
    MESA_KF_FLAT   = 1u << 1,       /* a disc lying down (draughts, reversi)       */
    MESA_KF_TALL   = 1u << 2,       /* a standing piece (chess, ludo)              */
};

typedef struct {
    const char *sprite;
    const char *name;               /* a Spanish key ("Dama"), for a choice */
    uint32_t    color, color2;
    const char *glyph;              /* a letter for the stand-in, or NULL       */
    uint8_t     flags;
    uint8_t     height;             /* in tenths of a cell, for the arc         */
} mesa_kind_t;

/* An option of a new game (a variant, how many play). Labels and values
 * are Spanish keys for the host's tr(); 'values' ends with NULL. */
typedef struct {
    const char         *label;
    const char *const  *values;
    uint8_t             def;
} mesa_opt_t;

enum {
    MESA_GF_ROTATE  = 1u << 0,      /* the board may turn half round (passing the board) */
    MESA_GF_CLOCK   = 1u << 1,      /* a clock makes sense                       */
    MESA_GF_PGN     = 1u << 2,      /* the moves as text: PGN files              */
    MESA_GF_DICE    = 1u << 3,
    MESA_GF_UPRIGHT = 1u << 4,      /* a standing board seen from the front (four in a row) */
    MESA_GF_HINTS   = 1u << 5,      /* the legal targets are worth marking       */
    MESA_GF_GRID    = 1u << 6,      /* cells are the unit squares around their middles */
};

/* How the board thinks: given to ai_move(). */
typedef struct {
    volatile bool stop;             /* the app wants it back now                 */
    uint32_t      ms;               /* about how long it may think               */
    uint32_t      seed;             /* for whatever it chooses at random         */
    uint32_t      nodes;            /* what it looked at, for the log            */
    int           depth;            /* how deep it got                           */
    uint32_t    (*now_ms)(void);    /* a clock                                   */
} mesa_ai_t;

/* What the app gives a module. */
typedef struct {
    uint32_t     abi;
    void        (*log)(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    const char *(*tr)(const char *spanish);     /* the app's translation      */
    void       *(*alloc)(size_t n);             /* PSRAM; NULL when there is none */
    void        (*free)(void *p);
    uint32_t    (*now_ms)(void);
} mesa_host_t;

typedef struct mesa_game {
    const char *word;               /* "ajedrez": the art's prefix, the saves'
                                       folder, the module's name              */
    const char *name;               /* "Ajedrez": a Spanish key for tr()      */
    const char *about;              /* one line under the name, a key too     */
    const char *prefix;             /* the art's entries: "aj"                */
    uint32_t    flags;              /* MESA_GF_*                              */
    int         min_players, max_players;
    int         levels;             /* how strong the board can play: 1..levels */
    size_t      state_size;
    const mesa_kind_t *kinds;
    int         nkinds;
    const mesa_opt_t  *opts;
    int         nopts;

    /* The board for these options: its picture's name in the pack ("aj",
     * "dm10"), and its size in board units. */
    const char *(*board)(const uint8_t *opts, float *w, float *h);
    int   (*players)(const uint8_t *opts);
    /* A player's name ("Blancas", "Rojo"), and the colour of a dot for it. */
    const char *(*side)(const uint8_t *opts, int player, uint32_t *rgb);

    void  (*init)(void *st, const uint8_t *opts, uint32_t seed);
    /* whose turn it is: 0.. players-1, or -1 once the game is over */
    int   (*turn)(const void *st);
    /* -1 still on, -2 a draw, else the winner; why (a Spanish key) in *why */
    int   (*result)(const void *st, const char **why);
    /* A line about the position ("Jaque"), a Spanish key, or NULL. */
    const char *(*status)(const void *st);
    /* A short line about a player for its card ("+3", "12 fichas"), or
     * false for none. Optional. */
    bool  (*info)(const void *st, int player, char *out, size_t n);

    int   (*moves)(const void *st, mesa_move_t *out, int max);
    uint32_t (*apply)(void *st, const mesa_move_t *m);      /* MESA_EV_* */
    /* For moves that differ only in 'tag': the kind to show for each. */
    int   (*choice_kind)(const void *st, const mesa_move_t *m);
    /* The move as a person writes it ("Cf3", "11-15"); before apply(). */
    bool  (*notation)(const void *st, const mesa_move_t *m, char *out, size_t n);

    int   (*need_roll)(const void *st);                     /* dice to roll, 0: none */
    void  (*roll)(void *st, uint32_t rnd);
    int   (*dice)(const void *st, int *vals, bool *used);   /* what shows, up to 4 */

    int   (*pieces)(const void *st, mesa_piece_t *out, int max);
    int   (*cells)(const void *st);                         /* how many there are */
    void  (*cell_pos)(const void *st, int cell, int slot, float *u, float *v);
    /* the cell under a point of the board, MESA_NO_CELL for none */
    int   (*cell_at)(const void *st, float u, float v);
    /* Where piece 'id' passes on its way for this move, cells in order
     * (a ludo token's squares); 0: straight. Optional. */
    int   (*path)(const void *st, const mesa_move_t *m, int id, uint8_t *cells, int max);
    /* Where the dice lie, in board units. Optional: the board's middle. */
    void  (*dice_pos)(const void *st, float *u, float *v);
    bool  (*ai_move)(const void *st, int level, mesa_ai_t *ai, mesa_move_t *out);

    /* A position from text (a FEN), optional. */
    bool  (*setup)(void *st, const uint8_t *opts, const char *text);
} mesa_game_t;

/* The games that come with the app (jm_games.c). */
extern const mesa_game_t mesa_ajedrez, mesa_damas, mesa_reversi, mesa_cuatro, mesa_ludo, mesa_backgammon;

/* A module's two functions and its host. */
int                mesa_module_abi(void);
const mesa_game_t *mesa_module_game(int index);
void               mesa_module_host(const mesa_host_t *host);

#ifdef __cplusplus
}
#endif
