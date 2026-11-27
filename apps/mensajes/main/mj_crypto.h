/*
 * P4OS - Mensajes: the cryptography MTProto 2.0 needs.
 *
 * The firmware lends the apps SHA-256 and HMAC-SHA256 (aos_hal.h) and
 * Monocypher (SHA-512 and its HMAC, among others), but none of mbedTLS:
 * neither AES, nor SHA-1, nor its bignum are in the symbol table. Asking for
 * them is a request to the firmware (they would come with the P4's AES and
 * RSA engines); until then they are here, small and in plain C:
 *
 *   SHA-1          the handshake's hashes and the auth key's id
 *   AES-256        T-tables, encryption and decryption of a block
 *   AES-256-IGE    the mode every MTProto message is sealed in
 *   bignum         numbers of up to 5120 bits: RSA with the server's key,
 *                  Diffie-Hellman for the auth key, SRP for the two-step
 *                  password. Exponentiation is Montgomery's, with a window of
 *                  four bits: about 0.5 s for 2048 bits on the board.
 *   pq             the handshake's proof of work: a 64-bit number into two
 *                  primes, by Pollard-Brent
 *   PBKDF2         HMAC-SHA512, 100 000 rounds for the password
 *   SRP            Telegram's variant (core.telegram.org/api/srp)
 *
 * tests/test_crypto.c checks all of it against vectors computed by a
 * reference in Python (tests/vectors.py), and core.telegram.org's own.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- SHA-1 ---- */

typedef struct {
    uint32_t h[5];
    uint64_t len;
    uint8_t buf[64];
    uint32_t n;
} mj_sha1_t;

void mj_sha1_init(mj_sha1_t *c);
void mj_sha1_update(mj_sha1_t *c, const void *d, size_t n);
void mj_sha1_final(mj_sha1_t *c, uint8_t out[20]);
void mj_sha1(const void *d, size_t n, uint8_t out[20]);
void mj_sha1_2(const void *a, size_t an, const void *b, size_t bn, uint8_t out[20]);

/* ---- AES-256 and IGE ---- */

typedef struct {
    uint32_t ek[60];            /* the encryption round keys                 */
    uint32_t dk[60];            /* and the decryption ones (equivalent inverse cipher) */
} mj_aes_t;

void mj_aes_setkey(mj_aes_t *a, const uint8_t key[32]);
void mj_aes_encrypt(const mj_aes_t *a, const uint8_t in[16], uint8_t out[16]);
void mj_aes_decrypt(const mj_aes_t *a, const uint8_t in[16], uint8_t out[16]);

/* In place, n a multiple of 16. iv is 32 bytes: the first half stands for
 * the "previous ciphertext" and the second for the "previous plaintext",
 * Telegram's order. The iv is not updated. */
void mj_ige_encrypt(const uint8_t key[32], const uint8_t iv[32], uint8_t *d, size_t n);
void mj_ige_decrypt(const uint8_t key[32], const uint8_t iv[32], uint8_t *d, size_t n);

/* ---- bignum ---- */

#define MJ_BN_LIMBS 168         /* 5376 bits: a product of two 2048-bit numbers, and SRP's a + u*x */

typedef struct {
    uint32_t d[MJ_BN_LIMBS];    /* little-endian limbs                        */
    int n;                      /* limbs in use (no leading zero limbs)       */
} mj_bn_t;

void mj_bn_zero(mj_bn_t *r);
void mj_bn_set_u32(mj_bn_t *r, uint32_t v);
bool mj_bn_from_be(mj_bn_t *r, const uint8_t *b, size_t len);
/* Big-endian into exactly len bytes, zeros on the left; false when it does not fit. */
bool mj_bn_to_be(const mj_bn_t *a, uint8_t *out, size_t len);
int  mj_bn_cmp(const mj_bn_t *a, const mj_bn_t *b);
int  mj_bn_bits(const mj_bn_t *a);
bool mj_bn_is_zero(const mj_bn_t *a);
void mj_bn_add(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *b);
void mj_bn_sub(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *b);   /* a >= b */
void mj_bn_mul(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *b);
void mj_bn_mod(mj_bn_t *r, const mj_bn_t *a, const mj_bn_t *m);
/* base^exp mod m, m odd. false for an even or zero modulus, or no memory. */
bool mj_bn_modexp(mj_bn_t *r, const mj_bn_t *base, const mj_bn_t *exp, const mj_bn_t *m);

/* big-endian helpers: out = b^e mod m, all of len bytes */
bool mj_modexp_be(uint8_t *out, size_t out_len, const uint8_t *b, size_t bl, const uint8_t *e, size_t el,
                  const uint8_t *m, size_t ml);

/* ---- the handshake's helpers ---- */

/* pq into p < q, both primes. false if it does not split (it always does). */
bool mj_factor_pq(uint64_t pq, uint32_t *p, uint32_t *q);

/* Telegram's well-known 2048-bit safe prime with g 3, 4, 5... (the one every
 * server sends): when dh_prime is this one, it needs no primality test. */
extern const uint8_t MJ_DH_PRIME[256];

/* The checks of core.telegram.org/mtproto/auth_key on g, the prime and g_a
 * (or g_b): 1 < g_a < p - 1, and 2^(2048-64) <= g_a <= p - 2^(2048-64). */
bool mj_dh_check(int g, const uint8_t prime[256], const uint8_t *ga, size_t ga_len);

/* ---- PBKDF2 and SRP ---- */

void mj_pbkdf2_sha512(const uint8_t *pass, size_t pl, const uint8_t *salt, size_t sl, uint32_t rounds,
                      uint8_t *out, size_t out_len);

/* The input of inputCheckPasswordSRP, from what account.getPassword gave
 * (passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000AndModPow): the
 * password, salt1, salt2, g, p (256 bytes) and srp_B (256 bytes). a is 256
 * random bytes (the caller's, so the tests can fix it). A and M1 come out.
 * false if B or p fails the checks. */
bool mj_srp(const char *password, const uint8_t *salt1, size_t s1, const uint8_t *salt2, size_t s2, int g,
            const uint8_t p[256], const uint8_t B[256], const uint8_t a[256], uint8_t A_out[256],
            uint8_t M1_out[32]);
