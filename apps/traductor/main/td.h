/*
 * P4OS - Traductor: what the app's files share.
 *
 *   traductor.c   life cycle, the screen (languages, text, result, lists)
 *   td_lang.c     the languages, and the packs on the card (which pairs
 *                 can be translated offline, directly or through English)
 *   td_job.c      a translation in the background: the local model in the
 *                 worker (both cores), the online engines by HTTP
 *   td_online.c   MyMemory and LibreTranslate: the request and the answer
 *   td_cfg.c      <data>/traductor/traductor.txt, shared with the portal
 *   td_hist.c     the history and the favourites (hist.txt beside it)
 *   td_detect.c   which language a text is in, by its common words
 *   td_dict.c     the pack's dictionary, for a word or two
 *   td_nmt.c, td_spm.c, td_pak.c   the offline translator (plain C)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aos_app.h"
#include "aos_hal.h"
#include "aos_i18n.h"
#include "aos_sys_glyphs.h"
#include "aos_theme.h"
#include "aos_ui.h"
#include "lvgl.h"

#define TD_TEXT_MAX  2000       /* bytes of text a translation takes         */
#define TD_OUT_MAX   4096

void td_scpy(char *dst, size_t n, const char *src);
uint32_t td_now_ms(void);
const char *td_data_dir(void);

/* ---- languages ---- */

typedef struct {
    const char *code;           /* ISO 639-1                                 */
    const char *name;           /* Spanish, the key of _()                   */
} td_lang_t;

int  td_lang_count(void);
const td_lang_t *td_lang_at(int i);
int  td_lang_find(const char *code);
const char *td_lang_name(const char *code);     /* translated; "auto" too   */

/* ---- packs (traductor_<a>_<b>_p4.pak in the apps folder) ---- */

#define TD_PACKS_MAX 24

typedef struct {
    char     path[200];
    char     pair[8];           /* "es-en"                                   */
    uint32_t bytes;
    char     nmt[4][5];         /* the model's directions: "esen", "enes"    */
    int      n_nmt;
    char     dict[4][5];        /* the dictionaries' directions              */
    int      n_dict;
    char     version[16];
} td_pack_t;

void td_packs_scan(void);
int  td_packs_count(void);
const td_pack_t *td_pack_at(int i);
/* the pack with a model for src -> dst, or NULL */
const td_pack_t *td_pack_nmt(const char *src, const char *dst);
const td_pack_t *td_pack_dict(const char *src, const char *dst);
/* how many models the way from src to dst takes offline: 0 none, 1 direct,
 * 2 through English */
int  td_local_steps(const char *src, const char *dst);
/* the codes the packs on the card can translate, for the detector */
int  td_local_langs(const char **out, int max);

/* ---- configuration ---- */

enum { TD_E_AUTO = 0, TD_E_LOCAL, TD_E_SERVER, TD_E_ONLINE, TD_E_COUNT };

typedef struct {
    int  engine;
    char server[160];           /* LibreTranslate, e.g. http://host:5000    */
    char server_key[96];
    char email[96];             /* MyMemory's raised limit                   */
    char mm_url[128];           /* MyMemory's base, to test against the fake */
    bool hist_off;              /* the page can turn the history off         */
    bool nmt_int8;              /* "nmt int8": int8 activations (to measure) */
    bool nmt_one_core;          /* "nmt one-core": no split (to measure)     */
} td_cfg_t;

td_cfg_t *td_cfg(void);
void td_cfg_load(void);
void td_cfg_save(void);
bool td_cfg_changed(void);      /* the file is newer than what was read     */
const char *td_engine_name(int e);
/* MyMemory's characters today, counted here (it does not say) */
int  td_quota_used(void);
int  td_quota_limit(void);
void td_quota_add(int chars);

/* ---- a translation ---- */

typedef enum {
    TD_OK = 0,
    TD_ERR_NO_ROUTE,            /* no engine can do this pair              */
    TD_ERR_NO_NET,
    TD_ERR_NET,                 /* the request failed                       */
    TD_ERR_SERVER,              /* the server answered with an error        */
    TD_ERR_QUOTA,               /* MyMemory's daily limit                   */
    TD_ERR_KEY,                 /* the server wants a key, or another one   */
    TD_ERR_MEM,
    TD_ERR_CANCEL,
    TD_ERR_NO_SERVER,           /* engine "server" with no address           */
} td_err_t;

typedef struct {
    td_err_t err;
    char     msg[120];          /* the server's words, when it said some    */
    int      engine;            /* the one that answered: TD_E_LOCAL ...    */
    uint32_t ms;
    char     src[8];            /* the source; detected when it was "auto"  */
    char     dst[8];
    const char *text;           /* valid inside the callback                */
    bool     pivot;             /* through English                          */
} td_result_t;

typedef void (*td_done_fn)(const td_result_t *r);

void td_job_init(void);
void td_job_stop(void);
bool td_job_start(const char *text, const char *src, const char *dst, td_done_fn cb);
void td_job_poll(void);         /* from the UI's timer                       */
bool td_job_busy(void);
void td_job_cancel(void);
const char *td_job_stage(void); /* what it is doing now, for the screen      */
void td_job_unload(void);       /* frees the models (the app went away)     */

/* ---- online ---- */

/* the request; *method is "GET" or "POST", body may stay empty */
bool td_online_request(int engine, const char *text, const char *src, const char *dst,
                       char *url, size_t url_n, const char **method, char *body, size_t body_n,
                       char *headers, size_t headers_n);
/* the answer into out; returns TD_OK or an error with msg */
td_err_t td_online_parse(int engine, int status, const char *body, char *out, size_t out_n,
                         char *detected, char *msg, size_t msg_n);
size_t td_url_encode(char *out, size_t n, const char *in);
size_t td_json_escape(char *out, size_t n, const char *in);
/* the string value of "key" in a JSON text, unescaped; false when missing */
bool td_json_str(const char *json, const char *key, char *out, size_t n);
bool td_json_num(const char *json, const char *key, double *out);

/* ---- history ---- */

#define TD_HIST_MAX 100

typedef struct {
    char     src[8], dst[8];
    bool     fav;
    uint32_t when;              /* epoch seconds, 0 unknown                 */
    char    *a, *b;             /* the text and its translation             */
} td_hist_t;

void td_hist_load(void);
void td_hist_save(void);
void td_hist_free(void);
int  td_hist_count(void);
td_hist_t *td_hist_at(int i);       /* 0 is the newest                      */
/* a new translation goes on top; the same text and pair replaces the old */
int  td_hist_add(const char *src, const char *dst, const char *a, const char *b);
void td_hist_del(int i);
bool td_hist_changed(void);

/* ---- detection ---- */

/* the likeliest of the codes given, or NULL when nothing is clear */
const char *td_detect(const char *text, const char *const *codes, int n);

/* ---- dictionary ---- */

/* the entries for a word (or two) in out, one per line: "word: meanings";
 * the number of entries */
int td_dict_lookup(const char *src, const char *dst, const char *word, char *out, size_t n);
void td_dict_close(void);
