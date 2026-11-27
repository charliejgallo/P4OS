/*
 * Malla - small helpers shared by every part.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Every snprintf here cuts on purpose: names, texts and lines from the mesh
 * go into the fields they fit, never past them. The board's GCC takes a
 * possible cut for an error (-Werror=format-truncation, HANDOFF-APPS.md);
 * in this app it is the intended behaviour. */
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wformat-truncation"
#endif

#define MSH_APP_ID "aos.malla"
#define MSH_TAG    "malla"

/* Zeroed memory from PSRAM (malloc in the simulator); free() it. */
void *msh_alloc(size_t n);

/* Copies src cutting at a whole UTF-8 character, always terminated. */
void msh_scpy(char *dst, size_t cap, const char *src);

/* Base64 without padding, '-' and '_' (the alphabet of a channel's URL);
 * decoding also takes '+', '/' and '=' so a pasted standard one works.
 * Return the length written, -1 when it does not fit or is not base64. */
int msh_b64url_encode(const uint8_t *in, int len, char *out, int cap);
int msh_b64_decode(const char *in, uint8_t *out, int cap);

/* "!a1b2c3d4", the way Meshtastic names a node by its number */
void msh_node_id(uint32_t num, char out[12]);

/* Great-circle distance in metres between two positions in 1e-7 degrees,
 * and the bearing from the first to the second in degrees (0 north). */
float msh_distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);
float msh_bearing_deg(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2);

/* A random 32-bit number that is never 0 (packet ids, the config nonce). */
uint32_t msh_random(void);

/* The node's colour: Meshtastic's apps colour a node by its number. */
uint32_t msh_node_rgb(uint32_t num);

/* Seconds on a clock that only goes forward (the uptime). */
uint32_t msh_now_s(void);
/* Seconds since 1970 when the board knows the time, else 0. */
uint32_t msh_epoch(void);
