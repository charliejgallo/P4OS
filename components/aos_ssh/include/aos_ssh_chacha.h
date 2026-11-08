/*
 * P4OS - chacha20-poly1305@openssh.com (OpenSSH's PROTOCOL.chacha20poly1305),
 * on Monocypher alone, so that the SSH server (components/aos_ssh) and the
 * SSH client app (apps/ssh) share it: the app has no mbedTLS.
 *
 * 64 bytes of key from the exchange: the first 32 drive the payload and the
 * Poly1305 key, the last 32 the packet length, which goes encrypted. The
 * nonce is the packet's sequence number. A packet is 4 bytes of length and
 * len bytes, with the 16-byte tag after them.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t main_key[32], header_key[32];
} ssh_chachapoly_t;

void ssh_chachapoly_init(ssh_chachapoly_t *c, const uint8_t key[64]);
void ssh_chachapoly_wipe(ssh_chachapoly_t *c);
/* the packet length hidden in the first 4 bytes, without touching them */
uint32_t ssh_chachapoly_length(const ssh_chachapoly_t *c, const uint8_t enc[4], uint32_t seq);
/* encrypts the length and the payload in place and writes the tag after them */
void ssh_chachapoly_seal(const ssh_chachapoly_t *c, uint8_t *buf, size_t len, uint32_t seq);
/* checks the tag, then decrypts the payload (and the length) in place */
bool ssh_chachapoly_open(const ssh_chachapoly_t *c, uint8_t *buf, size_t len, uint32_t seq);
