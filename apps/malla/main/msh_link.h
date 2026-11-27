/*
 * Malla - the way to the node.
 *
 * Copyright (C) 2026 P4OS contributors. GPL-3.0-or-later (apps/malla/LICENSE).
 *
 * Meshtastic's client API is the same everywhere: the client writes ToRadio
 * protobufs and reads FromRadio ones. What changes is how they travel:
 *
 *   serial  (USB CDC/CH34x/CP210x as usb0/usb1, or a header UART) and TCP
 *           to port 4403: a byte stream, each protobuf framed as
 *           0x94 0xC3 <length, 16 bits big-endian> <bytes>. Anything between
 *           frames is the node's own debug log.
 *   BLE     one GATT service: a write to ToRadio is one protobuf, a read of
 *           FromRadio is one protobuf (empty when there is no more), and
 *           FromNum notifies when there is something to read.
 *
 * A link hides that: it hands whole FromRadio payloads up and takes whole
 * ToRadio payloads down. The session (msh_sess.c) speaks the protocol over
 * any of them and never knows which. A node of the board's own (phase 2: an
 * SX1262 on spi.a, apps/malla/README.md) would be one more link.
 *
 * Every call is made from the session's thread, never from LVGL's.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MSH_FRAME_MAX 512           /* the API's own limit for one protobuf */

typedef enum {
    MSH_LINK_NONE = 0,
    MSH_LINK_SERIAL,
    MSH_LINK_TCP,
    MSH_LINK_BLE,
} msh_link_kind_t;

/* Where to go: what the connection sheet fills and the app remembers. */
typedef struct {
    msh_link_kind_t kind;
    char     port[16];              /* serial: "usb0", "usb1", "uart.a"... */
    uint32_t baud;                  /* serial: 115200 unless the node was told otherwise */
    char     host[64];              /* TCP: name or address */
    int      tcp_port;              /* TCP: 4403 */
    uint8_t  addr[6];               /* BLE */
    uint8_t  addr_type;
    char     name[32];              /* what to call it: the BLE name, or empty */
} msh_target_t;

/* What a link can ask of the person (BLE pairing): the session copies it
 * for the UI, and the UI answers through msh_sess_passkey(). */
typedef enum {
    MSH_ASK_NONE = 0,
    MSH_ASK_PIN,                    /* the node shows a six-digit PIN */
} msh_ask_t;

typedef struct msh_link msh_link_t;

typedef struct {
    /* false with a reason for the person in err (a key for _()) */
    bool (*open)(msh_link_t *l, char *err, size_t err_len);
    /* One FromRadio payload: its length (> 0), 0 when nothing came within
     * timeout_ms, < 0 when the link is gone (err says why). */
    int  (*recv)(msh_link_t *l, uint8_t *buf, int max, int timeout_ms, char *err, size_t err_len);
    /* One ToRadio payload; false when the link is gone. */
    bool (*send)(msh_link_t *l, const uint8_t *buf, int len);
    void (*close)(msh_link_t *l);
} msh_link_ops_t;

struct msh_link {
    const msh_link_ops_t *ops;
    msh_target_t t;
    /* what the link asks of the person, and the answer */
    volatile msh_ask_t ask;
    volatile bool      answered;
    volatile uint32_t  pin;
    /* set by the session when it is told to stop: a link that waits long
     * (a BLE connection, a pairing) gives up when it sees it */
    volatile bool cancel;
    /* a line on how it goes ("Emparejando…"): a key for _(), or "" */
    const char *volatile note;
    /* called by the link right after it changes ask or note, from the
     * session's thread (an open that pairs is still inside open()) */
    void (*changed)(msh_link_t *l);
    void *priv;                     /* the transport's own */
    /* bytes of the node's log that came between frames (serial, TCP) */
    void (*log_line)(void *ctx, const char *line);
    void *log_ctx;
};

/* A link for the target, closed; NULL without memory or for MSH_LINK_NONE. */
msh_link_t *msh_link_new(const msh_target_t *t);
void        msh_link_free(msh_link_t *l);

/* The transports (msh_link_stream.c, msh_link_ble.c) */
extern const msh_link_ops_t msh_link_serial_ops;
extern const msh_link_ops_t msh_link_tcp_ops;
extern const msh_link_ops_t msh_link_ble_ops;

/* The 0x94 0xC3 framing of the stream links, apart so it can be tested on
 * its own. feed() takes bytes as they come; each complete frame is handed
 * to on_frame, and the bytes outside frames, cut into lines, to on_text. */
typedef struct {
    int      state;                 /* 0 hunting 0x94, 1 want 0xC3, 2-3 length, 4 body */
    uint16_t want, have;
    uint8_t  buf[MSH_FRAME_MAX];
    char     text[160];
    int      text_len;
} msh_framer_t;

void msh_framer_reset(msh_framer_t *f);
/* Returns how many bytes of in were taken (all of them, unless a frame
 * completed: then it stops right after it, so the caller can take it). *got
 * is the frame's length when one completed, else 0. */
size_t msh_framer_feed(msh_framer_t *f, const uint8_t *in, size_t len, int *got,
                       void (*on_text)(void *ctx, const char *line), void *ctx);
/* Writes the 4-byte header for a payload of len bytes. */
void msh_frame_header(uint8_t hdr[4], int len);

/* true when an advertisement (AD structures as on the air) lists
 * Meshtastic's service: what the connection sheet's scan looks for. */
bool msh_ble_adv_is_meshtastic(const uint8_t *data, int len);
