/*
 * P4OS - Office: hashes and ciphers for PDF's security handler (of_crypt.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct { uint32_t h[4]; uint64_t n; uint8_t buf[64]; size_t used; } of_md5_t;
void of_md5_init(of_md5_t *m);
void of_md5_update(of_md5_t *m, const void *data, size_t n);
void of_md5_final(of_md5_t *m, uint8_t out[16]);
void of_md5(const void *data, size_t n, uint8_t out[16]);

void of_rc4(const uint8_t *key, size_t klen, uint8_t *data, size_t n);

typedef struct { uint8_t rk[240]; int rounds; } of_aes_t;
void   of_aes_init(of_aes_t *a, const uint8_t *key, int bits);     /* 128 or 256 */
void   of_aes_encrypt(const of_aes_t *a, uint8_t block[16]);
void   of_aes_decrypt(const of_aes_t *a, uint8_t block[16]);
/* In place; returns the length left (without the padding when unpad). */
size_t of_aes_cbc_decrypt(const uint8_t *key, int bits, const uint8_t *iv, uint8_t *data, size_t n, bool unpad);
void   of_aes_cbc_encrypt(const uint8_t *key, int bits, const uint8_t *iv, uint8_t *data, size_t n);

void of_sha256(const void *data, size_t n, uint8_t out[32]);
void of_sha384(const void *data, size_t n, uint8_t out[48]);
void of_sha512(const void *data, size_t n, uint8_t out[64]);
