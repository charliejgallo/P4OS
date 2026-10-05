/*
 * P4OS - Infrarrojo: what the app's files share.
 *
 * A learning remote for the bench. A receiver on one pin of the header
 * captures a button; ir_proto.c tells NEC from Sony from RC5 and the rest,
 * or keeps the frame raw; the button goes into a device on the card
 * (/sdcard/ir/<name>.json, ir_store.c), and an IR LED on another pin sends
 * it back. Air conditioners come from SmartIR's code library, packed for the
 * card (ir_pack.c, tools/pack_smartir.py): the app keeps the state and sends
 * the frame that matches it.
 *
 * Threads: everything runs in LVGL's task except the thread of ir_hw.c,
 * which owns the pins, sends, listens and runs the library search. The two
 * meet in ir_hw.c's slots, under its mutex.
 */
#pragma once

#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IR_APP_ID   "aos.infrarrojo"
#define IR_OWNER    "Infrarrojo"

/* -------------------------------------------------------------------------- */
/* Memory and strings (ir_store.c): everything the app allocates is PSRAM      */
/* -------------------------------------------------------------------------- */

void *ir_alloc(size_t n);               /* zeroed */
void *ir_realloc(void *p, size_t n);
void  ir_free(void *p);
char *ir_strdup(const char *s);
void  ir_copy(char *dst, size_t n, const char *src);

typedef struct {
    char *s;
    int   n, cap;
} ir_sb_t;

void ir_sb_put(ir_sb_t *b, const char *s, int n);   /* n < 0: strlen */
void ir_sb_printf(ir_sb_t *b, const char *fmt, ...);
void ir_sb_json(ir_sb_t *b, const char *s);         /* a quoted, escaped JSON string */
void ir_sb_free(ir_sb_t *b);

/* -------------------------------------------------------------------------- */
/* JSON (ir_json.c): parsed in place, nodes in one array                       */
/* -------------------------------------------------------------------------- */

enum { IJ_NULL = 0, IJ_BOOL, IJ_NUM, IJ_STR, IJ_ARR, IJ_OBJ };

typedef struct {
    uint8_t     type;
    int         count;      /* children of an array or object */
    const char *key;        /* inside an object */
    const char *str;
    double      num;        /* also a bool's 0/1 */
    int         kid, next;  /* indices, -1 none */
} ij_node_t;

typedef struct {
    ij_node_t *n;
    int        len, cap;
    char      *text;        /* owned: the copy the strings point into */
} ij_doc_t;

bool        ij_parse(ij_doc_t *d, const char *text, size_t len);   /* root is node 0 */
void        ij_free(ij_doc_t *d);
int         ij_get(const ij_doc_t *d, int obj, const char *key);    /* -1: missing */
int         ij_at(const ij_doc_t *d, int arr, int i);
const char *ij_str(const ij_doc_t *d, int i, const char *def);
double      ij_num(const ij_doc_t *d, int i, double def);
#define     IJ_EACH(d, parent, c) for (int c = (parent) >= 0 ? (d)->n[parent].kid : -1; c >= 0; c = (d)->n[c].next)

/* -------------------------------------------------------------------------- */
/* Protocols (ir_proto.c)                                                      */
/* -------------------------------------------------------------------------- */

#define IR_TX_MAX   2048        /* durations of one transmission (frames and gaps) */
#define IR_CAP_MAX  2048        /* of one capture */
#define IR_GAP_US   30000       /* a space this long separates two frames */

typedef struct {
    char     proto[12];         /* "NEC", "Sony12"...; "" = not recognised */
    uint32_t addr, cmd;
    uint32_t extra;             /* Kaseikyo's vendor */
    uint8_t  bits;              /* of the frame, for showing */
    bool     repeat;            /* a NEC repeat code, no data */
} ir_code_t;

typedef struct {
    const char *name;
    const char *label;          /* for the lists */
    uint32_t    carrier;        /* Hz */
    uint16_t    period_ms;      /* from frame start to frame start, held down */
    uint8_t     frames;         /* sent on a tap (Sony wants three) */
    uint32_t    addr_max, cmd_max;
} ir_proto_t;

int               ir_proto_count(void);
const ir_proto_t *ir_proto_at(int i);
const ir_proto_t *ir_proto_find(const char *name);

/* The first frame of d (mark first). false: nothing known. */
bool ir_decode(const uint32_t *d, int n, ir_code_t *out);
/* One frame of c, mark first, ending on a mark. 0: not encodable. */
int  ir_encode(const ir_code_t *c, bool repeat, uint8_t toggle, uint32_t *out, int max);
/* What a frame looks like, for an unknown one: "cabecera 9,0 + 4,5 ms,
 * 112 bits por distancia de pulsos: C4 D3 64 80..." */
void ir_describe(const uint32_t *d, int n, char *out, size_t len);
/* A one-line summary of a code: "NEC  dir 0x04  cmd 0x08". */
void ir_code_text(const ir_code_t *c, char *out, size_t len);
/* 0..1, as SmartIR's finder: the share of durations within 20 % (60 us). */
float ir_similarity(const uint32_t *a, int na, const uint32_t *b, int nb);
int   ir_frame_len(const uint32_t *d, int n);       /* durations up to the first gap */
uint32_t ir_total_us(const uint32_t *d, int n);

/* -------------------------------------------------------------------------- */
/* Devices on the card (ir_store.c)                                            */
/* -------------------------------------------------------------------------- */

enum { IR_K_TV = 0, IR_K_AUDIO, IR_K_AC, IR_K_FAN, IR_K_LIGHT, IR_K_OTHER, IR_K_COUNT };

typedef struct {
    char      name[40];
    char      role[12];         /* "power", "vol_up"... or "" (ir_roles) */
    ir_code_t code;             /* code.proto "" = raw */
    uint32_t  freq;             /* raw: the carrier, 0 = 38 kHz */
    uint32_t *raw;              /* raw: durations, mark first (gaps can pass 65 ms) */
    int       nraw;
} ir_button_t;

#define IR_AC_LEVELS 4

typedef struct {
    char        file[48];       /* "tele.json" in ir_store_dir() */
    char        name[48];
    int         kind;
    ir_button_t *btn;
    int         nbtn;
    /* an air conditioner from the library */
    int         smartir;        /* its SmartIR number, 0 none */
    char        ac_mode[20];
    char        ac_sel[IR_AC_LEVELS][24];
    float       ac_temp;
    bool        ac_on;
} ir_dev_t;

typedef struct {
    const char *role;
    const char *label;          /* N_() */
    const char *glyph;          /* LV_SYMBOL_* or short text, NULL: the label */
} ir_role_t;

const char      *ir_store_dir(void);        /* /sdcard/ir */
int              ir_kind_count(void);
const char      *ir_kind_key(int k);        /* "tv" */
const char      *ir_kind_label(int k);      /* translated */
const char      *ir_kind_glyph(int k);
int              ir_kind_from(const char *key);
int              ir_role_count(void);
const ir_role_t *ir_role_at(int i);
const ir_role_t *ir_role_find(const char *role);

/* The devices of the folder, sorted by name. Frees what was there. */
int  ir_store_scan(ir_dev_t **out);
void ir_store_free_all(ir_dev_t *devs, int n);
void ir_dev_clear(ir_dev_t *d);
bool ir_dev_load(const char *file, ir_dev_t *d);
bool ir_dev_save(ir_dev_t *d);              /* picks a file name if it has none */
bool ir_dev_delete(const ir_dev_t *d);
uint32_t ir_store_signature(void);          /* changes when a file of the folder does */
ir_button_t *ir_dev_add_button(ir_dev_t *d);
void ir_dev_remove_button(ir_dev_t *d, int i);
void ir_button_set_raw(ir_button_t *b, const uint32_t *d, int n, uint32_t freq);
void ir_button_clear(ir_button_t *b);
/* The durations to send for a tap (or a held key's repeat), and the carrier. */
int  ir_button_pulses(const ir_button_t *b, bool repeat, uint32_t *out, int max, uint32_t *carrier);
/* The device as JSON, the way it goes to the card (the portal reads it too). */
void ir_dev_json(const ir_dev_t *d, ir_sb_t *sb);
bool ir_dev_from_json(ir_dev_t *d, const ij_doc_t *j);

/* -------------------------------------------------------------------------- */
/* SmartIR's library (ir_pack.c)                                               */
/* -------------------------------------------------------------------------- */

enum { IR_C_MEDIA = 0, IR_C_CLIMATE, IR_C_FAN, IR_C_LIGHT, IR_C_COUNT };

typedef struct {
    uint16_t    id;             /* the SmartIR file's number */
    uint8_t     cls;
    const char *brand, *models;
    uint32_t    blob;
} ir_pack_dev_t;

typedef struct {
    uint8_t  *buf;
    ij_doc_t  js;
    int       npairs, ncodes, bits;
    const uint8_t *pairs, *offs, *codes;
    size_t    codes_len;
} ir_pack_blob_t;

const char *ir_pack_path(char *out, size_t n);
bool ir_pack_open(void);                    /* the index, once */
void ir_pack_close(void);
int  ir_pack_count(void);
const ir_pack_dev_t *ir_pack_at(int i);
int  ir_pack_find(uint16_t id);
bool ir_pack_load(int i, ir_pack_blob_t *b);
void ir_pack_blob_free(ir_pack_blob_t *b);
int  ir_pack_code(const ir_pack_blob_t *b, int code, uint32_t *out, int max);
/* Raw access for the search, which runs in the thread without parsing. */
bool ir_pack_parse_blob(uint8_t *buf, size_t len, ir_pack_blob_t *b, bool with_json);
/* The name of the command a code index is under ("cool · auto · 24"). */
bool ir_pack_code_name(const ir_pack_blob_t *b, int code, char *out, size_t n);

/* -------------------------------------------------------------------------- */
/* The pins and the thread (ir_hw.c)                                           */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t d[IR_CAP_MAX];
    int      n;
    int      frames;
    uint32_t seq;
} ir_capture_t;

typedef struct {
    int16_t dev;                /* index into the pack */
    int16_t code;
    float   score;
} ir_match_t;

#define IR_FIND_MAX 24

void ir_hw_start(void);
void ir_hw_stop(void);
void ir_hw_pins(int *rx, int *tx);
void ir_hw_set_pins(int rx, int tx);        /* saved; reopens what was open */
bool ir_hw_send(const uint32_t *d, int n, uint32_t carrier);   /* queued; false: full */
bool ir_hw_send_button(const ir_button_t *b, bool repeat);
enum { IR_WHO_UI = 1, IR_WHO_PORTAL = 2 };
void ir_hw_listen(int who, bool on);        /* the receiver is open while anyone wants it */
bool ir_hw_listening(void);
bool ir_hw_capture(uint32_t *seen, ir_capture_t *out);   /* one newer than *seen */
uint32_t ir_hw_capture_seq(void);           /* where a new listener starts */
uint32_t ir_hw_sent(void);                  /* transmissions done */
const char *ir_hw_error(void);              /* "" none; cleared when read */
void ir_hw_idle_close(void);                /* closes the LED's pin if nothing is queued */
bool ir_hw_find(const uint32_t *d, int n, unsigned cls_mask);
int  ir_hw_find_progress(void);             /* -1 idle, 0..99, 100 done */
int  ir_hw_find_results(ir_match_t *out, int max);
void ir_hw_loop_test(void);                 /* sends a frame and listens for it */
int  ir_hw_loop_result(void);               /* -1 waiting, 0 nothing heard, 1 heard */

/* -------------------------------------------------------------------------- */
/* The UI                                                                      */
/* -------------------------------------------------------------------------- */

typedef struct ir_app ir_app_t;

/* infrarrojo.c: the frame of the app */
lv_obj_t *ir_ui_header(lv_obj_t *parent, const char *title, const char *back,
                       lv_event_cb_t back_cb, void *user);
lv_obj_t *ir_ui_btn(lv_obj_t *parent, const char *text, lv_color_t color,
                    lv_event_cb_t cb, void *user);
lv_obj_t *ir_ui_card(lv_obj_t *parent);
lv_obj_t *ir_ui_scroll(lv_obj_t *parent);   /* the column a screen fills */
lv_obj_t *ir_ui_row(lv_obj_t *list, const char *glyph, lv_color_t gcolor, const char *text,
                    const char *sub, lv_event_cb_t cb, void *user);
void      ir_ui_toast(const char *text);
bool      ir_ui_landscape(void);
int32_t   ir_ui_width(void);
void      ir_ui_goto_tab(int tab);
void      ir_ui_learn_for(const char *file, int button);  /* the learn tab, saving into that button */

/* a sheet over the app, with a title; closed by its own buttons */
lv_obj_t *ir_sheet_open(const char *title);
void      ir_sheet_close(void);
bool      ir_sheet_is_open(void);
/* a text field in the sheet, with the keyboard */
lv_obj_t *ir_sheet_text(lv_obj_t *sheet, const char *label, const char *value, int max);
lv_obj_t *ir_sheet_buttons(lv_obj_t *sheet);
void      ir_ask(const char *title, const char *text, const char *yes, lv_color_t color,
                 void (*cb)(void *user), void *user);

/* ir_remote.c: the devices, a remote, the air conditioner */
void ir_remote_build(lv_obj_t *page);
void ir_remote_tick(void);
bool ir_remote_back(void);
void ir_remote_reload(void);
ir_dev_t *ir_remote_devices(int *n);
void ir_remote_open_file(const char *file);
ir_dev_t *ir_remote_find(const char *file);
void ir_remote_saved(const char *file);     /* changed in memory: write it, redraw */
void ir_remote_free(void);
bool ir_ac_send(ir_dev_t *d);               /* the state's frame (or off) */

/* ir_learn.c */
void ir_learn_build(lv_obj_t *page);
void ir_learn_tick(void);
void ir_learn_target(const char *file, int button);
void ir_learn_shown(bool shown);
void ir_learn_free(void);
lv_obj_t *ir_wave_create(lv_obj_t *parent, int32_t h);
void      ir_wave_set(lv_obj_t *w, const uint32_t *d, int n);

/* ir_base.c: the library */
void ir_base_build(lv_obj_t *page);
void ir_base_tick(void);
bool ir_base_back(void);
void ir_base_find(const uint32_t *d, int n);
void ir_base_free(void);
/* climate: the tree of a device and the choices at each level */
typedef struct {
    int  nlevels;
    char kind[IR_AC_LEVELS];    /* 'f' fan, 's' swing, 'p' preset, '?' */
} ir_ac_shape_t;
void ir_ac_shape(const ir_pack_blob_t *b, const char *mode, ir_ac_shape_t *s);
int  ir_ac_code(const ir_pack_blob_t *b, const ir_dev_t *d);   /* code index, -1 */
const char *ir_ac_word(const char *key);    /* "cool" -> "Frío" */

/* ir_portal.c: the page on the portal */
void ir_portal_tick(void);
