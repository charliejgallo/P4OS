/*
 * P4OS - who may use the web portal (aos_access.c): the networks marked as
 * home, a password, the sessions it opens and a token for scripts.
 *
 * It lives in aos_ui because two sides use it: Settings, on the board, which
 * is the only place where the rules and the password are set, and the
 * portal, which asks it about every request. Everything is kept in
 * preferences.
 *
 * Where a request comes in decides what it needs:
 *
 *   the USB cable        nothing: whoever holds the cable holds the board
 *   the board's own AP   like home (it has its own WPA2 password)
 *   a home network       nothing, or the password if "always" is chosen
 *   any other network    closed, or the password if chosen so
 *
 * Without a password set there is nothing to ask for: home and the AP are
 * open, other networks closed.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AOS_ZONE_CABLE = 0,     /* the USB network, and the simulator's own machine */
    AOS_ZONE_AP,            /* the board's access point */
    AOS_ZONE_HOME,          /* the Wi-Fi it is on, marked as home */
    AOS_ZONE_AWAY,          /* the Wi-Fi it is on, not marked */
} aos_zone_t;

typedef enum {
    AOS_ACCESS_OPEN = 0,
    AOS_ACCESS_LOGIN,       /* a session or the token */
    AOS_ACCESS_CLOSED,
} aos_access_t;

/* ---- the rules ---- */
aos_access_t aos_access_need(aos_zone_t zone);
bool aos_access_ask_always(void);           /* also at home and on the AP */
void aos_access_set_ask_always(bool on);
bool aos_access_away_login(void);           /* other networks: the password, or closed */
void aos_access_set_away_login(bool on);

/* ---- home networks, by name ---- */
#define AOS_ACCESS_TRUST_MAX 8
int  aos_access_trusted(char names[][33], int max);
bool aos_access_is_trusted(const char *ssid);
void aos_access_trust(const char *ssid, bool on);
/* the Wi-Fi the board is on is a home one (false with none) */
bool aos_access_here_trusted(void);
/* A wired network (Ethernet on a module of the header) has no name to know
 * it by: it is trusted when this is on, set on the board like the rest, and
 * off until then. Off, a connection that comes in by the wire is one from a
 * network that is not home. */
bool aos_access_wire_trusted(void);
void aos_access_set_wire_trusted(bool on);
/* A connection came in on this address of ours: is the network it is on
 * trusted? The wire's by its switch, anything else by the Wi-Fi's name. Each
 * connection is judged by where it came in, never by the other interface. */
bool aos_access_ip_trusted(const char *local_ip);

/* ---- the password ---- */
bool aos_access_has_password(void);
bool aos_access_set_password(const char *pw);   /* NULL or "" removes it; it also closes every session */
/* Checks it, with a growing wait after three failures in a row: false and
 * *wait_s > 0 while that lasts. */
bool aos_access_check_password(const char *pw, int *wait_s);

/* ---- sessions (the portal's cookie) ---- */
#define AOS_ACCESS_SESSION_LEN 32               /* hex characters */
bool aos_access_session_new(char out[AOS_ACCESS_SESSION_LEN + 1]);
bool aos_access_session_ok(const char *id);
void aos_access_session_end(const char *id);
void aos_access_sessions_clear(void);
int  aos_access_session_count(void);

/* ---- the token for scripts (Authorization: Bearer) ---- */
const char *aos_access_token(void);             /* made the first time it is asked for */
void aos_access_token_new(void);
bool aos_access_token_ok(const char *token);

/* ---- HTTPS (port 443) ----
 * The board's own authority (a CA limited to .local names and private
 * addresses, ten years) signs the portal's certificate (800 days, renewed by
 * the board). Trusting the authority once on a computer or a phone is what
 * makes the browser's warning go; its SHA-256 fingerprint, shown in
 * Settings, is what to compare. Switching HTTPS or making a new authority
 * takes effect at the next start. */
bool aos_access_https(void);
void aos_access_set_https(bool on);
/* the portal's certificate, the authority's and the portal's key, DER, each
 * made when needed (~1 s the first time); free() the three */
bool aos_access_tls_der(unsigned char **cert, size_t *cert_len, unsigned char **ca, size_t *ca_len,
                        unsigned char **key, size_t *key_len);
/* the authority's certificate alone, DER, to download; free() it */
bool aos_access_tls_ca(unsigned char **der, size_t *len);
void aos_access_tls_forget(void);           /* a new authority at the next start */
/* "AB:CD:..." of the authority, "" if there is none yet */
void aos_access_tls_fingerprint(char *out, size_t n);

/* ---- SSH (port 22, components/aos_ssh; aos_access_ssh.c) ----
 * Off until Settings, Portal web switches it on. It follows the portal's
 * rules: closed where the portal is closed; where the portal needs the
 * password, the password or a key on the list; where it is open, anyone.
 * A key not on the list can ask to be let in: the board shows its
 * fingerprint and whoever is in front of it says yes or no. That is the
 * only way a key gets on the list, so getting into the portal is not enough
 * to add one (the rules and the password are set on the board only). */
bool aos_access_ssh(void);
void aos_access_set_ssh(bool on);
/* The board's host key, Ed25519, made the first time it is asked for: the
 * 32-byte seed (secret) and the public key. false only if it cannot be kept. */
bool aos_access_ssh_host_key(unsigned char seed[32], unsigned char pub[32]);
/* "SHA256:..." as ssh prints it; "" before the key exists */
void aos_access_ssh_host_fingerprint(char *out, size_t n);
void aos_access_ssh_host_forget(void);      /* a new host key at the next connection */

/* The keys that may log in, one OpenSSH line each: "ssh-ed25519 AAAA... comment" */
#define AOS_ACCESS_SSH_KEYS 8
bool aos_access_ssh_key_get(int i, char *line, size_t n);   /* false: no key i */
int  aos_access_ssh_key_find(const char *type, const char *b64);  /* index, or -1 */
bool aos_access_ssh_key_add(const char *line);              /* false: the list is full */
void aos_access_ssh_key_remove(int i);                      /* and closes every SSH session */
/* the "SHA256:..." of a line's key, "" if it does not parse */
void aos_access_ssh_key_fingerprint(const char *line, char *out, size_t n);

/* A key not on the list asking in. aos_ssh calls ask() from its connection's
 * thread and waits up to timeout_s for the board's answer: 1 yes (the key is
 * on the list now), 0 no or no answer, -1 another question is on screen.
 * The UI polls asking() from its tick and answers with answer(). */
typedef struct {
    char type[24];          /* "ED25519", "ECDSA", "RSA 3072" */
    char fp[56];            /* "SHA256:..." */
    char comment[48];       /* what the key's owner called it, often user@host */
    char from[48];          /* the address it comes from */
} aos_access_ssh_ask_t;
int  aos_access_ssh_ask(const aos_access_ssh_ask_t *q, const char *line, int timeout_s);
bool aos_access_ssh_asking(aos_access_ssh_ask_t *out);
void aos_access_ssh_answer(bool yes);

/* Bumped by whatever must close the SSH sessions open now: a new password,
 * "close every session", a key taken off the list, SSH switched off, a new
 * host key. Each connection compares it with the one it started with. */
unsigned aos_access_ssh_generation(void);
void     aos_access_ssh_kick(void);

/* Called from the shell's tick: keeps mDNS quiet on networks that are not
 * home, and marks the first network as home when the rules are new (an
 * update must not lock the user out of the board they had open). */
void aos_access_tick(void);

#ifdef __cplusplus
}
#endif
