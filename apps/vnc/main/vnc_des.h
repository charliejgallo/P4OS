/*
 * P4OS - VNC viewer: DES for the VNC password (vnc_des.c).
 */
#pragma once

#include <stdint.h>

/* One block, the key as DES has it (FIPS 46-3). */
void vnc_des_block(const uint8_t key[8], const uint8_t in[8], uint8_t out[8]);

/* VNC authentication: the 16-byte challenge encrypted with the password
 * (its first 8 characters, each byte's bits mirrored). */
void vnc_des_response(const char *password, const uint8_t challenge[16], uint8_t response[16]);
