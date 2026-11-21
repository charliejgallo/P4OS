/*
 * P4OS - the SFTP server (aos_ssh_sftp.c): the "sftp" subsystem of the SSH
 * server, apart from SSH itself. The channel's bytes go in, whole replies
 * come out.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aos_sftpd aos_sftpd_t;

/* One reply, whole (its 4 bytes of length included). */
typedef void (*aos_sftpd_send_t)(void *ctx, const uint8_t *data, size_t len);

/* A session over the tree at 'root' (the card's mount point): what the
 * client calls "/" is that folder, and nothing above it can be named.
 * 'read_only' refuses whatever writes. NULL without memory. */
aos_sftpd_t *aos_sftpd_new(const char *root, bool read_only, aos_sftpd_send_t send, void *ctx);

/* Bytes of the channel, as they come: in pieces or several packets at once.
 * false when what came is not SFTP (a length that makes no sense): the
 * channel is to be closed. */
bool aos_sftpd_feed(aos_sftpd_t *s, const uint8_t *data, size_t len);

/* The same, up to the end of one request: how many bytes it took. When a
 * request was whole, its reply has been handed to 'send' before this
 * returns, and stays where it is until the next call: whoever has a window
 * to respect sends it first. 'bad' as feed()'s false. */
size_t aos_sftpd_feed_one(aos_sftpd_t *s, const uint8_t *data, size_t len, bool *bad);

void aos_sftpd_free(aos_sftpd_t *s);

#ifdef __cplusplus
}
#endif
