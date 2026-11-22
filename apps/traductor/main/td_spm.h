/*
 * TRADUCTOR - SentencePiece (unigram) without the library: the vocabulary
 * of a model's pack entry, Viterbi over its pieces to encode, and the pieces
 * joined back to decode. The normalization is whitespace only (the models'
 * nmt_nfkc changes nothing in the text people type in these languages).
 */
#pragma once

#include <stdint.h>

typedef struct {
    int       n;
    uint32_t *off;      /* piece i is pool[off[i] .. off[i] + len[i]]       */
    uint8_t  *len;
    uint8_t  *type;     /* 1 normal, 2 unknown, 3 control, 4 user, 6 byte    */
    float    *score;
    char     *pool;
    int32_t  *hash;     /* open addressing, -1 empty                        */
    uint32_t  hmask;
    int       maxlen;   /* bytes of the longest piece                        */
    int       unk, eos;
    float     unk_score;
} td_spm_t;

/* from the entry's bytes (u32 n, then f32 score, u8 type, u8 len, bytes) */
int  td_spm_init(td_spm_t *s, const uint8_t *data, uint32_t len);
void td_spm_free(td_spm_t *s);
/* the piece's id, or -1 */
int  td_spm_find(const td_spm_t *s, const char *p, int len);
/* text -> ids (no </s>); returns the count, at most max */
int  td_spm_encode(const td_spm_t *s, const char *text, int32_t *ids, int max);
/* ids -> text; returns its length */
int  td_spm_decode(const td_spm_t *s, const int32_t *ids, int n, char *out, int size);
