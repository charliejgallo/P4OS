/*
 * P4OS - EEPROM: what the app's files share.
 *
 *   eeprom.c       the app: tabs, the job bar, the timer that picks jobs up
 *   ee_chips.c     the table of chips
 *   ee_drv.c       the wire protocols and the jobs, in the worker
 *   ee_hash.c      CRC-32, MD5 and SHA-256
 *   ee_store.c     the versions on the card
 *   ee_portal.c    requests from the portal's page, through files
 *   ee_ui.c        small pieces: cards, rows, sheets, keypads
 *   ee_pg_*.c      the four pages: chip, memory, versions, wiring
 */
#pragma once

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_io.h"
#include "aos_mono.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* The simulator links every app into one program: the three globals the
 * files share get names of their own there, and short ones here. */
#define S ee_S
#define U ee_U
#define J ee_J

#define EE_ID     "aos.eeprom"
#define EE_OWNER  "EEPROM"
#define EE_DIR    "eeprom"              /* under the card's root */

#define C_AMBER   lv_color_hex(0xF59E0B)
#define C_EDIT    lv_color_hex(0xFF9F0A)
#define C_DIFF    lv_color_hex(0xFF453A)
#define C_PANEL   lv_color_hex(0x121216)

/* -------------------------------------------------------------------------- */
/* Chips (ee_chips.c)                                                          */
/* -------------------------------------------------------------------------- */

typedef enum { EE_I2C = 0, EE_SPI, EE_FLASH, EE_MW, EE_FAM_COUNT } ee_fam_t;

typedef struct {
    const char *name;       /* "24LC256"; its versions go in a folder of this name */
    const char *also;       /* other makers' names for the same part */
    uint8_t  fam;           /* ee_fam_t */
    uint8_t  abytes;        /* I2C, SPI: address bytes. Microwire: address bits at x16 (x8 takes one more) */
    uint8_t  apins;         /* I2C: the A0..A2 inputs it decodes, bits 0..2 */
    uint8_t  blk;           /* I2C past 64 KB: the device-address step of the upper half */
    uint16_t page;          /* bytes one write may carry, never crossing a page */
    uint8_t  twc_ms;        /* longest write cycle of the datasheet */
    uint32_t size;          /* bytes */
} ee_chip_t;

int              ee_chip_count(void);
const ee_chip_t *ee_chip_at(int i);
int              ee_chip_find(const char *name);         /* -1: none */
int              ee_chip_by_size(int fam, uint32_t size, uint8_t abytes);   /* -1: none */
const char      *ee_fam_name(int fam);                   /* "I2C 24xx" */
void             ee_size_text(uint32_t n, char *out, size_t len);   /* "32 KB" */
/* The device address that holds byte 'addr' of an I2C chip at 'base'. */
uint8_t          ee_i2c_dev(const ee_chip_t *c, uint8_t base, uint32_t addr);
/* The bases an I2C chip may sit at, from the A pins it decodes. */
int              ee_i2c_bases(const ee_chip_t *c, uint8_t *out, int max);

/* -------------------------------------------------------------------------- */
/* Where the chip hangs                                                        */
/* -------------------------------------------------------------------------- */

enum { MW_CS = 0, MW_SK, MW_DI, MW_DO };

typedef struct {
    int      chip;                              /* index in the table */
    char     i2c_port[AOS_IO_PORT_NAME_MAX];
    uint8_t  i2c_addr;                          /* device address of the first block */
    char     spi_port[AOS_IO_PORT_NAME_MAX];
    int      spi_cs;                            /* -1: the port's own cs=, else a GPIO */
    uint32_t spi_hz;
    int      mw[4];                             /* CS, SK, DI, DO */
    int      mw_org;                            /* 8 or 16 */
} ee_conf_t;

void ee_conf_load(ee_conf_t *c);
void ee_conf_save(const ee_conf_t *c);

/* -------------------------------------------------------------------------- */
/* Hashes (ee_hash.c)                                                          */
/* -------------------------------------------------------------------------- */

typedef struct {
    uint32_t crc;
    uint32_t md5[4];
    uint32_t sha[8];
    uint64_t len;
    uint8_t  mbuf[64];
    uint32_t mn;
} ee_hash_t;

typedef struct {
    uint32_t crc;
    char     md5[33];
    char     sha[65];
} ee_sums_t;

void ee_hash_init(ee_hash_t *h);
void ee_hash_update(ee_hash_t *h, const uint8_t *p, size_t n);
void ee_hash_final(ee_hash_t *h, ee_sums_t *out);
void ee_sums_of(const uint8_t *p, size_t n, ee_sums_t *out);

/* -------------------------------------------------------------------------- */
/* Versions on the card (ee_store.c)                                           */
/* -------------------------------------------------------------------------- */

#define EE_VER_MAX 160

typedef struct {
    char     file[64];      /* "20261005-140322-lectura.bin" */
    char     date[20];      /* "2026-10-05 14:03:22", "" without a clock */
    char     source[16];    /* "lectura", "antes", "escrita", "portal", "vista" */
    char     note[96];
    uint32_t size;
    uint32_t crc;
    char     md5[33];
    char     sha[65];
} ee_ver_t;

typedef struct {
    void      *f;
    ee_hash_t  h;
    uint32_t   n;
    char       chip[24];
    char       path[200];
} ee_vw_t;

void *ee_alloc(size_t n);               /* PSRAM first; free() it */
bool ee_card_ok(void);
/* <card>/eeprom/<chip>, made if missing; false without a card. */
bool ee_ver_dir(const char *chip, char *out, size_t len);
int  ee_ver_list(const char *chip, ee_ver_t *out, int max);      /* newest first */
bool ee_ver_meta(const char *chip, const char *file, ee_ver_t *out);
/* Writing one: open, put the bytes as they come, close. Close writes its
 * .json; a copy equal to the newest version is not kept twice: *same says
 * so and 'file' names the one that was already there. */
bool ee_vw_open(ee_vw_t *w, const char *chip, const char *source);
bool ee_vw_put(ee_vw_t *w, const uint8_t *p, size_t n);
bool ee_vw_close(ee_vw_t *w, const char *source, const char *note, char *file, size_t len, bool *same);
void ee_vw_abort(ee_vw_t *w);
/* A version's bytes, into a PSRAM buffer of its size (the caller frees). */
uint8_t *ee_ver_load(const char *chip, const char *file, uint32_t *size);
bool ee_ver_delete(const char *chip, const char *file);
bool ee_ver_set_note(const char *chip, const char *file, const char *note);
/* Small flat JSON: a quoted, escaped string; the value of "key" (a string,
 * unescaped, or a number as text); a small file read whole (free it). */
void  ee_json_str(char *out, size_t len, const char *s);
bool  ee_json_get(const char *js, const char *key, char *out, size_t len);
char *ee_read_small(const char *path, size_t max);
/* The .bin of a version, whole path. */
void ee_ver_path(const char *chip, const char *file, char *out, size_t len);

/* -------------------------------------------------------------------------- */
/* The jobs (ee_drv.c), one at a time in the app's worker                      */
/* -------------------------------------------------------------------------- */

enum { JOB_NONE = 0, JOB_DETECT, JOB_SIZE, JOB_READ, JOB_WRITE, JOB_STATUS, JOB_PROTECT };
/* What a write writes */
enum { WR_DIRTY = 0, WR_FULL, WR_ERASE };
/* Where a job is, for the bar */
enum { PH_OPEN = 0, PH_READ, PH_BACKUP, PH_WRITE, PH_VERIFY, PH_SAVE, PH_DETECT };

typedef struct {
    volatile bool     busy, cancel;
    volatile uint32_t done, total;      /* bytes, for the bar */
    volatile int      phase;
    volatile uint32_t seq;              /* bumped when one ends */
    int        job, mode;
    bool       from_portal;
    ee_conf_t  conf;
    /* the image a read fills and a write takes its bytes from (owned by S) */
    uint8_t   *img;
    uint8_t   *dirty;
    uint32_t   size;
    uint8_t    new_sr;                  /* PROTECT: the status register wanted */
    /* results */
    bool       ok;
    char       msg[200];
    int        sugg_chip;               /* DETECT, SIZE: -1 or a table index */
    int        sugg_addr;               /* DETECT, I2C: -1 or a base */
    int        sugg_org;                /* DETECT, Microwire: 0, 8 or 16 */
    bool       have_sr;
    uint8_t    sr;                      /* the status register read */
    uint32_t   units, bad_at;           /* WRITE: pages written; first bad byte, or ~0 */
    ee_sums_t  sums;
    char       saved[64];               /* READ: the version; WRITE: the copy before */
    char       saved2[64];              /* WRITE: the version after */
    bool       same, same2;
} ee_job_t;

extern ee_job_t J;

bool ee_job_start(int job, int mode);
void ee_job_stop(void);
const char *ee_phase_text(int phase);

/* -------------------------------------------------------------------------- */
/* The app's state (eeprom.c)                                                  */
/* -------------------------------------------------------------------------- */

enum { TAB_CHIP = 0, TAB_MEM, TAB_VER, TAB_WIRE, TAB_COUNT };

#define EE_UNDO 256

typedef struct {
    ee_conf_t  conf;
    int        tab;
    /* the image on screen */
    uint8_t   *img;
    uint8_t   *dirty;                   /* a bit a byte: edited and not written */
    uint32_t   size;
    bool       valid;
    char       img_chip[24];            /* the chip it belongs to (the folder of its versions) */
    char       img_from[80];            /* where it came from, for the info line */
    uint32_t   ndirty;
    struct { uint32_t a; uint8_t old; } undo[EE_UNDO];
    int        nundo;
    ee_sums_t  sums;                    /* of the image as read or loaded */
    /* a version to compare with */
    uint8_t   *ref;
    uint32_t   ref_size;
    char       ref_name[64];
    uint32_t   ndiff;
    /* the hex view */
    uint32_t   cursor;
    uint32_t   mark_len;                /* bytes lit from the cursor (a search hit) */
    /* the last detection, for the chip page */
    char       det[200];
    int        sugg_chip, sugg_addr, sugg_org;
    bool       have_sr;
    uint8_t    sr;
    /* the version list */
    ee_ver_t  *vers;
    int        nvers;
    bool       vers_stale;
} ee_state_t;

extern ee_state_t S;

const ee_chip_t *ee_cur(void);                  /* the chosen chip */
bool ee_img_alloc(uint32_t size);
void ee_img_free(void);
void ee_ref_set(uint8_t *buf, uint32_t size, const char *name);   /* takes buf */
void ee_ref_clear(void);
void ee_diff_count(void);
void ee_dirty_clear(void);
bool ee_is_dirty(uint32_t a);
void ee_poke(uint32_t a, uint8_t v);            /* an edit, with its undo */
bool ee_undo(void);
void ee_rebuild(void);                          /* the current page again */
void ee_vers_reload(void);
/* Starts a read/write after the checks a person needs (busy, a card). */
bool ee_start_read(bool from_portal);
bool ee_start_write(int mode, bool from_portal);

/* -------------------------------------------------------------------------- */
/* UI pieces (ee_ui.c)                                                         */
/* -------------------------------------------------------------------------- */

typedef struct {
    lv_obj_t *root;                     /* the app's root */
    lv_obj_t *content;                  /* the page area */
    lv_obj_t *tabs[TAB_COUNT];
    lv_obj_t *jobbar, *job_text, *job_fill, *job_cancel;
    int32_t   W, H;                     /* root */
    int32_t   CW, CH;                   /* content, inside its padding */
    bool      land;
} ee_ui_t;

extern ee_ui_t U;

lv_obj_t *ee_box(lv_obj_t *parent, int32_t w, int32_t h);
lv_obj_t *ee_card(lv_obj_t *parent, int32_t w);               /* height: its content */
lv_obj_t *ee_column(lv_obj_t *parent, int32_t w, int32_t h);  /* a flex column that scrolls */
lv_obj_t *ee_pill(lv_obj_t *parent, const char *glyph, const char *text, lv_color_t bg,
                  lv_event_cb_t cb, void *ud);
lv_obj_t *ee_caption(lv_obj_t *parent, const char *text, int32_t w);
lv_obj_t *ee_title(lv_obj_t *parent, const char *text);
/* A settings row: label on the left, value on the right (cut with dots),
 * a chevron when it does something. Returns the value label. */
lv_obj_t *ee_row(lv_obj_t *parent, int32_t w, const char *label, const char *value,
                 lv_event_cb_t cb, void *ud);
lv_obj_t *ee_seg(lv_obj_t *parent, int32_t w, const char *const *items, int n, int sel,
                 lv_event_cb_t cb);                           /* ud of the event: the index */
void      ee_set_disabled(lv_obj_t *o, bool off);

/* Sheets: one at a time, over the app. */
typedef void (*ee_pick_cb_t)(int index, void *ud);
typedef struct { const char *text; const char *sub; bool danger; bool checked; } ee_item_t;
lv_obj_t *ee_sheet(const char *title, const ee_item_t *items, int n, ee_pick_cb_t cb, void *ud);
lv_obj_t *ee_sheet_custom(const char *title, int32_t *content_w);
void      ee_sheet_close(void);
void      ee_sheet_forget(void);
bool      ee_sheet_open(void);
void      ee_confirm(const char *title, const char *text, const char *yes, bool danger,
                     void (*cb)(void *ud), void *ud);
/* A line of text typed on the system keyboard. */
void      ee_ask_text(const char *title, const char *initial, int max,
                      void (*cb)(const char *text, void *ud), void *ud);
/* Hex digits on a keypad: up to 'max_digits'. */
void      ee_ask_hex(const char *title, uint32_t initial, int max_digits,
                     void (*cb)(uint32_t value, void *ud), void *ud);

/* -------------------------------------------------------------------------- */
/* Pages                                                                       */
/* -------------------------------------------------------------------------- */

void ee_pg_chip(lv_obj_t *parent);
void ee_pg_mem(lv_obj_t *parent);
void ee_pg_ver(lv_obj_t *parent);
void ee_pg_wire(lv_obj_t *parent);
void ee_pg_mem_forget(void);                    /* the page's objects are gone */
void ee_pg_mem_refresh(void);                   /* the image changed */
void ee_pg_mem_goto(uint32_t addr, uint32_t len);
bool ee_pg_mem_back(void);                      /* closes the editor if open */
void ee_pg_chip_job_done(void);                 /* a detection or status came */

/* -------------------------------------------------------------------------- */
/* The portal (ee_portal.c)                                                    */
/* -------------------------------------------------------------------------- */

void ee_portal_poll(void);                      /* every second, from the timer */
void ee_portal_progress(void);                  /* while a portal job runs */
void ee_portal_done(void);                      /* when it ends */
void ee_portal_chip(void);                      /* the chosen chip changed */
void ee_portal_closed(void);                    /* the app is closing */
