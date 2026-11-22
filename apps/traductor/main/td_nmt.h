/*
 * TRADUCTOR - the neural translator, offline: a Bergamot "tiny" model
 * (Firefox Translations, MPL-2.0) from a pair's pack, run in plain C with
 * float activations over its int8 weights. No BLAS, no threads of its own:
 * the app lends it a way to run a job on both cores (td_nmt_par_fn).
 *
 * The model (tools/marian2pak.py checks the shape): a transformer encoder of
 * 6 layers, 256 wide, 8 heads, feed-forward 1536, post-norm; a decoder of 2
 * layers whose self-attention is an SSRU (a gated running sum, so a step
 * costs the same at the 1st word and the 40th); tied embeddings of 32000
 * pieces, and a shortlist that keeps the output to the ~50 most frequent
 * pieces plus the 50 likeliest for each source piece. Greedy search, like
 * Firefox's beam of 1.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "td_pak.h"

typedef struct td_nmt td_nmt_t;

typedef void (*td_job_fn)(void *arg, int part, int parts);
/* run fn(arg, i, parts) for every i, maybe at the same time; return when all are done */
typedef void (*td_nmt_par_fn)(td_job_fn fn, void *arg, void *ctx);

typedef struct {
    uint32_t src_tokens, out_tokens;
    uint32_t macs_m;        /* multiply-adds, millions                       */
    uint32_t cand;          /* the shortlist's size, last sentence           */
    uint32_t ms_enc, ms_dec, ms_total;
} td_nmt_stats_t;

enum {
    TD_NMT_EMB_RAM = 0,     /* the 8 MB embedding table in memory           */
    TD_NMT_EMB_CARD = 1,    /* only the rows a sentence needs, from the pack */
};

/* dir is "esen" (es -> en); the pack stays open while the model lives.
 * NULL and *err set when the pack has no model for it or memory is short. */
td_nmt_t *td_nmt_load(td_pak_t *pak, const char *dir, int emb_mode, const char **err);
void      td_nmt_free(td_nmt_t *m);
size_t    td_nmt_bytes(const td_nmt_t *m);

void td_nmt_set_par(td_nmt_t *m, td_nmt_par_fn fn, void *ctx);
void td_nmt_set_int8(td_nmt_t *m, bool on);    /* int8 activations, like Marian's */
/* checked between words; set it to stop a translation halfway */
void td_nmt_set_cancel(td_nmt_t *m, volatile bool *flag);
/* a clock in ms for the stats (the app's uptime); NULL: no times */
void td_nmt_set_clock(td_nmt_t *m, uint32_t (*ms)(void));

/* text (any length: cut in sentences and lines) -> out; false if cancelled
 * or out of memory. The stats add up over the sentences. */
bool td_nmt_translate(td_nmt_t *m, const char *text, char *out, int size, td_nmt_stats_t *st);
