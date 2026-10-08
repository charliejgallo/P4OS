/*
 * AES-CTR, runtime key size (16 or 32 bytes), encryption only (CTR needs
 * only the forward cipher). A compact standard AES; see aes.c for the
 * provenance (tiny-AES-c, public domain / Unlicense). Used by rf_mesh.c to
 * decrypt a Meshtastic packet with a channel key the user provides.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

/* XORs the AES-CTR keystream over data in place. key is 16 or 32 bytes;
 * iv is the 16-byte initial counter block, incremented big-endian per
 * block. */
void aes_ctr_xcrypt(const uint8_t *key, int keylen, const uint8_t iv[16], uint8_t *data, size_t len);
