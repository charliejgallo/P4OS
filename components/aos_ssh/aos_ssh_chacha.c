/*
 * P4OS - chacha20-poly1305@openssh.com. The contract is in aos_ssh_chacha.h.
 *
 * As OpenSSH does it: ChaCha20 in its original form (64-bit nonce, 64-bit
 * block counter), the nonce being the sequence number big-endian. The
 * length is ChaCha20 under the header key at block 0; the Poly1305 key is
 * the main key's block 0; the payload starts at the main key's block 1. The
 * tag covers the encrypted length and the encrypted payload.
 */
#include "aos_ssh_chacha.h"

#include "monocypher.h"

#include <string.h>

void ssh_chachapoly_init(ssh_chachapoly_t *c, const uint8_t key[64])
{
    memcpy(c->main_key, key, 32);
    memcpy(c->header_key, key + 32, 32);
}

void ssh_chachapoly_wipe(ssh_chachapoly_t *c) { crypto_wipe(c, sizeof *c); }

static void nonce_of(uint32_t seq, uint8_t n[8])
{
    n[0] = n[1] = n[2] = n[3] = 0;
    n[4] = (uint8_t)(seq >> 24);
    n[5] = (uint8_t)(seq >> 16);
    n[6] = (uint8_t)(seq >> 8);
    n[7] = (uint8_t)seq;
}

uint32_t ssh_chachapoly_length(const ssh_chachapoly_t *c, const uint8_t enc[4], uint32_t seq)
{
    uint8_t n[8], l[4];
    nonce_of(seq, n);
    crypto_chacha20_djb(l, enc, 4, c->header_key, n, 0);
    return (uint32_t)l[0] << 24 | (uint32_t)l[1] << 16 | (uint32_t)l[2] << 8 | l[3];
}

static void poly_key(const ssh_chachapoly_t *c, const uint8_t n[8], uint8_t pk[32])
{
    uint8_t zero[32] = { 0 };
    crypto_chacha20_djb(pk, zero, 32, c->main_key, n, 0);
}

void ssh_chachapoly_seal(const ssh_chachapoly_t *c, uint8_t *buf, size_t len, uint32_t seq)
{
    uint8_t n[8], pk[32];
    nonce_of(seq, n);
    crypto_chacha20_djb(buf, buf, 4, c->header_key, n, 0);
    crypto_chacha20_djb(buf + 4, buf + 4, len, c->main_key, n, 1);
    poly_key(c, n, pk);
    crypto_poly1305(buf + 4 + len, buf, 4 + len, pk);
    crypto_wipe(pk, sizeof pk);
}

bool ssh_chachapoly_open(const ssh_chachapoly_t *c, uint8_t *buf, size_t len, uint32_t seq)
{
    uint8_t n[8], pk[32], tag[16];
    nonce_of(seq, n);
    poly_key(c, n, pk);
    crypto_poly1305(tag, buf, 4 + len, pk);
    crypto_wipe(pk, sizeof pk);
    if (crypto_verify16(tag, buf + 4 + len)) return false;
    crypto_chacha20_djb(buf, buf, 4, c->header_key, n, 0);
    crypto_chacha20_djb(buf + 4, buf + 4, len, c->main_key, n, 1);
    return true;
}
