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

/* Called from the shell's tick: keeps mDNS quiet on networks that are not
 * home, and marks the first network as home when the rules are new (an
 * update must not lock the user out of the board they had open). */
void aos_access_tick(void);

#ifdef __cplusplus
}
#endif
