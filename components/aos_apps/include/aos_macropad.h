/*
 * P4OS - the Macro pad's layout and its runner (aos_macropad.c), shared by
 * the app (aos_app_macropad.c) and the portal (aos_portal_macropad.c).
 *
 * The layout is a JSON file on the card, <sd>/macropad.json:
 *
 *   { "version": 1,
 *     "pages": [
 *       { "name": "Mac", "buttons": [ {button} | null, ... ] },     up to 15 slots
 *       { "name": "Casa", "auto": "ha", "buttons": [] },             filled from HA once
 *       ... ] }                                                      up to 8 pages
 *
 *   button: { "label": "Copiar", "glyph": "CONTENT_COPY", "color": "#0A84FF",
 *             "type": "key",  "key": "cmd+c" }
 *           "type": "text", "text": "hola"
 *           "type": "seq",  "seq": "KEY cmd+space\nDELAY 250\nSTRING terminal\nKEY enter"
 *           "type": "ha",   "entity": "light.taller", "service": "light.toggle"   (service
 *                           optional: without it, the usual tap), "data": "{...}" optional
 *           "type": "mqtt", "topic": "taller/extractor/set", "payload": "TOGGLE",
 *                           "retain": false, "qos": 0, "state": "taller/extractor/state"
 *           "type": "app",  "app": "aos.bench"
 *
 * A slot is its index in "buttons"; null (or a missing index) is an empty
 * slot. Glyphs are the names of aos_sys_glyphs.h without AOS_SYM_.
 *
 * The sequence language is Pato goma's (the watch's HID script runner) plus
 * three verbs of the P4's:
 *
 *   # comment        STRING text     KEY cmd+shift+4   DELAY ms
 *   MOUSE dx dy      SCROLL n        CLICK [1|2]       REPEAT n
 *   HA entity | HA domain.service entity [json]
 *   MQTT topic payload...            OPEN app.id
 *
 * The document is guarded by a mutex: the app runs in the LVGL task, the
 * portal in its own threads. Take the lock around any use of the cJSON
 * pointers and never keep them past the unlock.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AOS_MP_SLOTS  15        /* 3x5 upright, 5x3 lying down */
#define AOS_MP_PAGES  8

typedef enum { AOS_MP_NONE = 0, AOS_MP_KEY, AOS_MP_TEXT, AOS_MP_SEQ, AOS_MP_HA, AOS_MP_MQTT, AOS_MP_APP } aos_mp_type_t;

/* Creates the lock: once, at boot, before any other task can call in. */
void     aos_macropad_init(void);
/* Loads the file, or the default layout when there is none (idempotent). */
void     aos_macropad_load(void);
uint32_t aos_macropad_version(void);        /* changes on every change */
bool     aos_macropad_saved(void);          /* false: no card, lives in memory only */

void   aos_macropad_lock(void);
void   aos_macropad_unlock(void);
cJSON *aos_macropad_doc(void);              /* under the lock */
int    aos_macropad_page_count(void);       /* under the lock */
cJSON *aos_macropad_page(int page);         /* under the lock; NULL if none */
cJSON *aos_macropad_button(int page, int slot);   /* under the lock; NULL = empty */
/* Puts a button in a slot (takes ownership; NULL empties it). Under the lock. */
void   aos_macropad_set_button(int page, int slot, cJSON *button);
/* Under the lock, after changing the document: saves it and bumps the version. */
void   aos_macropad_changed(void);

/* For the portal. print: malloc'd JSON (free it). replace: validates and
 * takes the whole document; false with the reason in err. */
char  *aos_macropad_print(void);
bool   aos_macropad_replace(const char *json, char *err, size_t err_len);
void   aos_macropad_reset(void);            /* back to the default layout */

/* "key" -> AOS_MP_KEY... */
aos_mp_type_t aos_macropad_type(const cJSON *button);
const char   *aos_macropad_type_name(aos_mp_type_t t);     /* "key"... */
/* NULL when the button can run, else what is wrong with it (translated). */
const char   *aos_macropad_check(const cJSON *button);
uint32_t      aos_macropad_color(const cJSON *button);     /* 0xRRGGBB */
const char   *aos_macropad_str(const cJSON *button, const char *key);   /* "" if absent */

/* The glyphs a button can wear */
int         aos_macropad_glyph_count(void);
const char *aos_macropad_glyph_name(int i);
const char *aos_macropad_glyph(const char *name);         /* the UTF-8 string; a default if unknown */

/* Fills the pages marked "auto": "ha" from Home Assistant's table, once HA
 * is ready. true if it changed the document. Any task. */
bool aos_macropad_autofill(void);

/* ---- running (LVGL task) ----
 * A button becomes a list of steps played by an lv_timer, one key or one
 * character per tick, so the screen stays alive while a long text types. */
bool aos_macropad_run(int page, int slot);          /* false if refused (the reason in the status) */
bool aos_macropad_run_button(const cJSON *button);  /* the same, for a button not in the layout */
void aos_macropad_queue_key(const char *name);      /* the trackpad page's keys and typing */
void aos_macropad_queue_text(const char *text);
void aos_macropad_stop(void);
/* Thread-safe: the portal's "Probar" goes through aos_ui_request_call. */
void aos_macropad_request_run(int page, int slot);

typedef struct {
    bool     busy;              /* a sequence is playing */
    int      pct;               /* its progress */
    bool     ok;                /* the last thing sent went out */
    char     msg[112];          /* "Enviado: cmd+c", "Sin computadora por USB"... */
    uint32_t seq;               /* changes with every new message */
    int      page, slot;        /* the button of the message, -1 if none */
} aos_mp_status_t;
void aos_macropad_status(aos_mp_status_t *out);
/* A line for the status from the app itself (the trackpad's clicks). */
void aos_macropad_say(bool ok, const char *msg);

#ifdef __cplusplus
}
#endif
