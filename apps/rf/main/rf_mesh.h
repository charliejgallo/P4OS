/*
 * RF - decoding a LoRa packet, and reading its Meshtastic frame (rf_mesh.c).
 * A C port of the prototype in apps/rf/test/ (Python), tested on the Mac
 * against a recording made on the board.
 *
 * It demodulates one LoRa packet from I/Q - sync, dechirp, Hamming, CRC -
 * and reads the Meshtastic header, which travels unencrypted (who to whom,
 * hops). If a channel key is given it decrypts the payload (AES-CTR, the
 * open scheme of Meshtastic's firmware) and, for a text message, hands back
 * the text. Keys are the caller's to supply - the public default, or one
 * the user loads on the board; none are built in or kept here.
 *
 * It reads no direct messages encrypted to another node's public key: those
 * need that node's private key, which is not here.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool crc_ok;                 /* the LoRa packet came out whole */
    int length;                  /* the Meshtastic packet's length */
    /* the header, unencrypted */
    uint32_t to, from, id;
    uint8_t hop_limit, chan_hash, next_hop, relay;
    bool want_ack;
    /* with a key: the decrypted payload */
    bool decrypted;
    int portnum;                 /* 1 = text; -1 if not decrypted */
    char text[240];              /* the UTF-8 text when portnum == 1 */
    uint8_t payload[240];
    int payload_len;
} rf_mesh_t;

/* Demodulate and decode one LoRa packet from cu8 I/Q.
 *   iq, n   : n cu8 pairs holding the packet (with its preamble)
 *   rate    : the I/Q sample rate
 *   fc      : the packet's offset from the tuner's centre, Hz
 *   freq_hz : the tuned centre (for the sender's crystal drift)
 *   bw, sf  : the LoRa bandwidth and spreading factor (from the meter)
 *   key,keylen : the channel key (16 or 32 bytes), or NULL to only read the header
 * Returns true if a CRC-ok packet was decoded (out filled). Heavy float
 * work (tens to a few hundred ms on the board): off the real-time path. */
bool rf_mesh_decode(const uint8_t *iq, int n, uint32_t rate, int32_t fc, uint32_t freq_hz,
                    uint32_t bw, int sf, const uint8_t *key, int keylen, rf_mesh_t *out);

/* A Meshtastic channel key from its string: base64 (e.g. "AQ==" for the
 * public channel) or hex. Writes the AES key into key[32], returns its
 * length (16 or 32), or 0 for "none"/invalid. */
int rf_mesh_key(const char *s, uint8_t key[32]);
