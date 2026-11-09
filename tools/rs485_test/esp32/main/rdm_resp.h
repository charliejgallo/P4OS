/* An RDM responder with no platform in it (rdm_resp.c says what it does). */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t     uid[6];         /* manufacturer (2 bytes) and device (4) */
    bool        muted;          /* found by discovery: quiet until un-muted */
    uint16_t    addr;           /* DMX start address, 1-512 */
    uint16_t    footprint;      /* channels it uses */
    uint16_t    model_id;
    bool        identify;       /* the controller asked it to show itself */
    char        label[33];      /* DEVICE_LABEL, settable */
    const char *model, *maker, *version;
    bool        changed;        /* a SET changed something: the owner may save it */
} rdm_dev_t;

/* One request, from the 0xCC start code to its checksum. The answer goes to
 * out (up to 64 bytes): its length, with *with_break true when it is a
 * normal packet (send a BREAK and MAB before it) and false for a discovery
 * answer (none); 0 when nothing is to be sent. */
int rdm_handle(rdm_dev_t *dv, const uint8_t *req, int len, uint8_t *out, bool *with_break);
