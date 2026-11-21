/*
 * P4OS - an SSH client in the firmware, and SFTP over it (aos_sshc.c): what
 * the board needs to leave a file on a server of the person's own. Not a
 * terminal: that is the SSH app's (apps/ssh), which has its own transport.
 *
 * It blocks: every call returns when it is done or failed, and whoever
 * calls has a thread for it (the backups' own). One connection, one
 * channel, one request at a time.
 *
 * The transport is the server's, the other way round (aos_ssh_crypto.c):
 * curve25519-sha256, a host key of any kind ssh_key_verify() takes,
 * aes256-gcm@openssh.com or chacha20-poly1305@openssh.com, strict KEX. It
 * logs in with an Ed25519 key, the board's own, and with nothing else: no
 * password is ever asked for or kept.
 *
 * The server's key is pinned by whoever calls: the first connection reports
 * the key it saw (aos_sshc_hostkey), the person looks at its fingerprint on
 * the board's screen, and from then on that key is handed in and any other
 * is refused before anything is sent.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aos_sshc aos_sshc_t;

typedef enum {
    AOS_SSHC_OK = 0,
    AOS_SSHC_E_CONNECT,     /* no such host, or nobody answers there */
    AOS_SSHC_E_PROTOCOL,    /* not an SSH server, or nothing in common with it */
    AOS_SSHC_E_HOSTKEY,     /* the server's key is not the one that was kept */
    AOS_SSHC_E_AUTH,        /* the server does not let this key in */
    AOS_SSHC_E_IO,          /* the connection was cut, or it stopped answering */
    AOS_SSHC_E_REMOTE,      /* the server said no to a file operation */
    AOS_SSHC_E_LOCAL,       /* the file to send could not be read */
    AOS_SSHC_E_MEMORY,
    AOS_SSHC_E_STOPPED,     /* 'cancel' was set */
} aos_sshc_err_t;

typedef struct {
    const char *host;
    int port;                       /* 22 when 0 */
    const char *user;
    const uint8_t *seed;            /* the Ed25519 seed to log in with, 32 bytes */
    const char *hostkey;            /* the server's key as it was kept (aos_sshc_hostkey), or NULL/"" for whichever comes */
    volatile bool *cancel;          /* looked at while waiting; may be NULL */
} aos_sshc_opts_t;

/* Connects, checks the server's key and logs in. NULL when it could not:
 * 'err' says which kind and 'why' gets a line for the log. With a kept key
 * that does not match, 'seen' (if not NULL, 600 bytes) still gets the key
 * the server showed, for the screen to say so. */
aos_sshc_t *aos_sshc_open(const aos_sshc_opts_t *o, int *err, char *why, size_t why_len, char *seen);

/* The server's key as it showed it (base64 of its wire form): what to keep
 * and hand back in 'hostkey'. And what a person compares: "ED25519
 * SHA256:…", as ssh-keygen -lf prints it. */
const char *aos_sshc_hostkey(const aos_sshc_t *c);
const char *aos_sshc_fingerprint(const aos_sshc_t *c);
/* The same line for a key that was kept, without connecting. */
bool aos_sshc_fingerprint_of(const char *hostkey, char *out, size_t n);

void aos_sshc_close(aos_sshc_t *c);
const char *aos_sshc_why(const aos_sshc_t *c);      /* the last failure, a line for the log */

/* ---- SFTP over it: each returns an aos_sshc_err_t ---- */

int aos_sshc_sftp(aos_sshc_t *c);                               /* once, before the others */
/* A folder and the ones above it that are missing. A relative path is from
 * the user's home, as the server sees it. */
int aos_sshc_mkdirs(aos_sshc_t *c, const char *path);
/* A local file to the server: written beside its name and renamed over it
 * when it is whole, so a cut never leaves half a file under a good name. */
int aos_sshc_put(aos_sshc_t *c, const char *local, const char *remote);
/* Whether a file is there, and its size. AOS_SSHC_OK with *there false
 * when it is not. */
int aos_sshc_stat(aos_sshc_t *c, const char *path, bool *there, uint64_t *size);
/* The names in a folder, a call each (not "." nor ".."). */
typedef void (*aos_sshc_name_t)(void *ctx, const char *name, bool dir, uint64_t size, uint32_t mtime);
int aos_sshc_list(aos_sshc_t *c, const char *path, aos_sshc_name_t each, void *ctx);
int aos_sshc_remove(aos_sshc_t *c, const char *path);

/* ---- the key this side logs in with ---- */

/* The line for a server's ~/.ssh/authorized_keys: "ssh-ed25519 AAAA…
 * comment". */
bool aos_sshc_key_line(const uint8_t seed[32], const char *comment, char *out, size_t n);

#ifdef __cplusplus
}
#endif
