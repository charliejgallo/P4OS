/*
 * P4OS - the SSH server's insides, shared by its files:
 *
 *   aos_ssh.c        the listener, a thread per connection, the transport
 *                    (RFC 4253), user authentication (RFC 4252) and the
 *                    session channels (RFC 4254)
 *   aos_ssh_crypto.c the algorithms: curve25519-sha256, ssh-ed25519 host key,
 *                    aes128/256-gcm@openssh.com, and checking the client's
 *                    ssh-ed25519, ecdsa-sha2-nistp256 and rsa-sha2-256/512
 *   aos_ssh_shell.c  what runs on a channel: the line editor, the jobs that
 *                    keep writing (cat, log -f, top) and exec's one command
 *   aos_ssh_cmds.c   the commands
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* ---- the wire: SSH's own types (RFC 4251, 5) ---- */

typedef struct {
    uint8_t *p;
    size_t len, cap;
    bool bad;               /* ran out of room: the message is dropped */
} wbuf_t;

void wb_byte(wbuf_t *b, uint8_t v);
void wb_u32(wbuf_t *b, uint32_t v);
void wb_bytes(wbuf_t *b, const void *d, size_t n);
void wb_string(wbuf_t *b, const void *d, size_t n);
void wb_cstr(wbuf_t *b, const char *s);
void wb_mpint(wbuf_t *b, const uint8_t *be, size_t n);  /* unsigned, big-endian */

typedef struct {
    const uint8_t *p;
    size_t len, off;
    bool bad;               /* read past the end: everything after reads as empty */
} rbuf_t;

uint8_t  rb_byte(rbuf_t *b);
uint32_t rb_u32(rbuf_t *b);
bool     rb_bool(rbuf_t *b);
/* a string, as a pointer into the buffer; NULL and *n = 0 past the end */
const uint8_t *rb_string(rbuf_t *b, size_t *n);
/* the same, copied as C text, cut to fit */
bool rb_cstr(rbuf_t *b, char *out, size_t n);
/* an mpint's magnitude without its leading zero bytes */
const uint8_t *rb_mpint(rbuf_t *b, size_t *n);
/* whether a comma-separated name-list has this name */
bool namelist_has(const uint8_t *list, size_t n, const char *name);

/* ---- crypto (aos_ssh_crypto.c) ---- */

void ssh_random(void *buf, size_t n);
void ssh_sha256(const void *d, size_t n, uint8_t out[32]);

typedef struct ssh_sha256_ctx ssh_sha256_ctx_t;
ssh_sha256_ctx_t *ssh_sha256_new(void);
void ssh_sha256_add(ssh_sha256_ctx_t *c, const void *d, size_t n);
void ssh_sha256_end(ssh_sha256_ctx_t *c, uint8_t out[32]);  /* and frees it */

/* AES-GCM as OpenSSH does it: a 4-byte fixed part and an 8-byte counter
 * that goes up by one each packet; the packet length is the additional
 * data, sent in clear. */
typedef struct ssh_gcm ssh_gcm_t;
ssh_gcm_t *ssh_gcm_new(const uint8_t *key, size_t key_len, const uint8_t iv[12]);
void ssh_gcm_free(ssh_gcm_t *g);
/* buf = 4 bytes of length (clear) then len bytes, encrypted in place; the
 * 16-byte tag goes after them */
bool ssh_gcm_seal(ssh_gcm_t *g, uint8_t *buf, size_t len);
bool ssh_gcm_open(ssh_gcm_t *g, uint8_t *buf, size_t len);

/* X25519 */
void ssh_x25519_keypair(uint8_t sk[32], uint8_t pk[32]);
bool ssh_x25519(uint8_t shared[32], const uint8_t sk[32], const uint8_t peer[32]);

/* the host key's signature over the exchange hash: the whole "signature"
 * blob (string "ssh-ed25519", string 64 bytes) into b */
void ssh_host_sign(wbuf_t *b, const uint8_t seed[32], const uint8_t pub[32], const uint8_t *msg, size_t n);

/* A client's public key: whether it parses and what it is ("ED25519",
 * "ECDSA", "RSA 3072"), for the board's question and the log */
bool ssh_key_describe(const uint8_t *blob, size_t n, char *out, size_t on);
/* Whether sig (the "signature" blob: string alg, string bytes) over msg was
 * made by the key in blob with the algorithm the client named. */
bool ssh_key_verify(const char *alg, const uint8_t *blob, size_t bn, const uint8_t *sig, size_t sn,
                    const uint8_t *msg, size_t mn);
/* the public-key algorithms ssh_key_verify takes, for server-sig-algs */
extern const char SSH_SIG_ALGS[];

/* ---- a session channel's side: the shell (aos_ssh_shell.c) ---- */

typedef struct sh sh_t;

typedef struct {
    const char *user;       /* what the client said */
    const char *from;       /* its address */
    bool pty;               /* a terminal: line editing, \r\n, colours */
    int cols, rows;
    const char *term;       /* $TERM, "" without a terminal */
} sh_opts_t;

sh_t *sh_new(const sh_opts_t *o);
void  sh_free(sh_t *s);
void  sh_start_shell(sh_t *s);
void  sh_start_exec(sh_t *s, const char *cmd);
void  sh_input(sh_t *s, const uint8_t *d, size_t n);
void  sh_eof(sh_t *s);
void  sh_resize(sh_t *s, int cols, int rows);
/* Runs whatever is pending. SH_DONE once the shell has ended (exit, or
 * exec's command is done): the transport sends what is left of its output
 * and closes the channel. SH_BUSY: a job wants to run again as soon as
 * there is room for its output. */
enum { SH_DONE, SH_IDLE, SH_BUSY };
int   sh_poll(sh_t *s);
int   sh_exit_status(sh_t *s);
/* what it wrote and the transport has not sent yet */
size_t sh_out_peek(sh_t *s, const uint8_t **p);
void   sh_out_take(sh_t *s, size_t n);
/* the transport is not taking output (no window, a key exchange): jobs wait */
void   sh_set_blocked(sh_t *s, bool blocked);

/* ---- for the commands (aos_ssh_cmds.c) ---- */

void sh_write(sh_t *s, const void *d, size_t n);
void sh_puts(sh_t *s, const char *t);
void sh_printf(sh_t *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
size_t sh_out_room(sh_t *s);
bool sh_is_pty(sh_t *s);
int  sh_cols(sh_t *s);
int  sh_rows(sh_t *s);
const char *sh_user(sh_t *s);
const char *sh_from(sh_t *s);
/* the shell's working directory, "/" being the card */
const char *sh_cwd(sh_t *s);
void sh_set_cwd(sh_t *s, const char *v);
/* A path as typed to the shell's own form ("/photos/a.jpg") and to the
 * file system's ("/sdcard/photos/a.jpg"). false: outside what may be
 * reached, or no card. */
bool sh_path(sh_t *s, const char *in, char *virt, size_t vn, char *real, size_t rn);
/* `exit`: the shell ends once its output is sent, with this status */
void sh_request_exit(sh_t *s, int status);
/* ANSI colour, only on a terminal: sh_color(s, "1;34") ... sh_color(s, NULL) */
void sh_color(sh_t *s, const char *sgr);
/* A command that keeps going: step() is called whenever there is room for
 * more output, until it returns true; stop() when it ends either way, or is
 * cut short with Ctrl-C or by the channel closing. A key typed while it
 * runs goes to key() if there is one (top's q), else Ctrl-C stops it. */
typedef struct {
    bool (*step)(sh_t *s, void *st);
    void (*stop)(sh_t *s, void *st);
    bool (*key)(sh_t *s, void *st, uint8_t c);  /* true: it ends the job */
    void *st;
    uint32_t every_ms;      /* 0: as fast as output drains; else that often */
} sh_job_t;
void sh_job(sh_t *s, const sh_job_t *j);

typedef struct {
    const char *name;
    const char *args;       /* the usage after the name, "" for none */
    const char *help;       /* a line, marked N_() and translated on show */
    int (*run)(sh_t *s, int argc, char **argv);   /* the exit status */
} sh_cmd_t;
extern const sh_cmd_t SH_CMDS[];
extern const int SH_CMD_COUNT;

/* ---- the server, for `who` (aos_ssh.c) ---- */
typedef struct {
    char user[32], from[48];
    uint32_t since_s;       /* uptime when it logged in */
    const char *how;        /* "clave", "contraseña", "abierta" */
} ssh_who_t;
int ssh_who(ssh_who_t *out, int max);
