/*
 * P4OS - SSH's data types on the wire (RFC 4251, 5) and the check of a
 * public-key signature: what the SSH server (components/aos_ssh) and the
 * SSH client app (apps/ssh) share. The firmware lends these to the apps
 * (tools/gen_symbols.py), with aos_ssh_chacha.h and Monocypher.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool bad;               /* ran out of room: the message is dropped */
} wbuf_t;

void wb_byte(wbuf_t *b, uint8_t v);
void wb_u32(wbuf_t *b, uint32_t v);
void wb_bytes(wbuf_t *b, const void *d, size_t n);
void wb_string(wbuf_t *b, const void *d, size_t n);
void wb_cstr(wbuf_t *b, const char *s);
void wb_mpint(wbuf_t *b, const uint8_t *be, size_t n);  /* unsigned, big-endian */

typedef struct {
    const uint8_t *p;
    size_t len, off;
    bool bad;               /* read past the end: everything after reads as empty */
} rbuf_t;

uint8_t  rb_byte(rbuf_t *b);
uint32_t rb_u32(rbuf_t *b);
bool     rb_bool(rbuf_t *b);
/* a string, as a pointer into the buffer; NULL and *n = 0 past the end */
const uint8_t *rb_string(rbuf_t *b, size_t *n);
/* the same, copied as C text, cut to fit */
bool rb_cstr(rbuf_t *b, char *out, size_t n);
/* an mpint's magnitude without its leading zero bytes */
const uint8_t *rb_mpint(rbuf_t *b, size_t *n);
/* whether a comma-separated name-list has this name */
bool namelist_has(const uint8_t *list, size_t n, const char *name);

/* ---- public keys (aos_ssh_crypto.c) ---- */

/* A public key in its wire form: whether it parses and what it is
 * ("ED25519", "ECDSA P-256", "RSA 3072") */
bool ssh_key_describe(const uint8_t *blob, size_t n, char *out, size_t on);
/* Whether sig (the "signature" blob: string alg, string bytes) over msg was
 * made by the key in blob with the algorithm named: ssh-ed25519,
 * ecdsa-sha2-nistp256, rsa-sha2-256 or rsa-sha2-512 (RSA of 2048 bits or
 * more). */
bool ssh_key_verify(const char *alg, const uint8_t *blob, size_t bn, const uint8_t *sig, size_t sn,
                    const uint8_t *msg, size_t mn);
/* the algorithms ssh_key_verify takes, as a name-list */
extern const char SSH_SIG_ALGS[];

#ifdef __cplusplus
}
#endif
