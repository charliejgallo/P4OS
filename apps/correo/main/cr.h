/*
 * P4OS - Correo: a mail client over IMAP and SMTP (apps/correo/README.md).
 *
 * The pieces, each in a file of its own:
 *
 *   cr_util.c     memory in PSRAM, text helpers, base64, quoted-printable,
 *                 charsets to UTF-8, RFC 2047 words, dates, addresses
 *   cr_mime.c     a message taken apart (multipart, encodings, the parts to
 *                 show and the ones to save) and a message put together
 *   cr_html.c     HTML turned into formatted text, without a web engine
 *   cr_net.c      the connection (aos_tls), IMAP and SMTP
 *   cr_sync.c     the network thread: every account's connection, IDLE,
 *                 incremental sync, the jobs the screens ask for, the cache
 *   cr_store.c    the accounts, the folders' headers on the card, contacts
 *   cr_ui*.c      the screens
 *   cr_portal.c   the portal's page, over the live channel
 *   correo.c      the app's life cycle
 *
 * Threads: LVGL's (the screens, correo.c's timer) and the network thread
 * (cr_sync.c). They share the model below behind one mutex (cr_lock) and two
 * queues: jobs going to the network, events and finished jobs coming back.
 * The network thread never touches LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CR_APP_ID      "aos.correo"
#define CR_TAG         "correo"

#define CR_ACCT_MAX    6
#define CR_FOLDER_MAX  80
#define CR_WINDOW      300          /* headers fetched the first time a folder is opened */
#define CR_MORE        200          /* and each "load more" */
#define CR_BODY_MAX    (1024 * 1024)   /* a message bigger than this is fetched in part */
#define CR_PART_MAX    (24 * 1024 * 1024) /* an attachment streamed to the card, at most */

/* ---- memory and text (cr_util.c) -------------------------------------------------------------- */

void *cr_alloc(size_t n);                 /* zeroed, PSRAM on the board */
void *cr_realloc(void *p, size_t n);
void  cr_free(void *p);
char *cr_strdup(const char *s);
char *cr_strndup(const char *s, size_t n);
void  cr_copy(char *dst, size_t n, const char *src);
void  cr_ncopy(char *dst, size_t n, const char *src, size_t len);
int   cr_lower(int c);
bool  cr_space(int c);
bool  cr_istarts(const char *s, const char *prefix);
const char *cr_istr(const char *hay, const char *needle);      /* case-insensitive strstr */
const char *cr_imem(const char *hay, size_t n, const char *needle);
void  cr_trim(char *s);
uint32_t cr_hash(const char *s, size_t n);                   /* FNV-1a, 0 never */

/* A growing buffer. */
typedef struct {
    char  *p;
    size_t n, cap;
    bool   oom;
} cr_buf_t;
void cr_buf_add(cr_buf_t *b, const void *d, size_t n);
void cr_buf_str(cr_buf_t *b, const char *s);
void cr_buf_printf(cr_buf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void cr_buf_free(cr_buf_t *b);
char *cr_buf_take(cr_buf_t *b);              /* NUL-terminated, the buffer emptied */

/* An arena for a message's texts: freed in one go. */
typedef struct cr_chunk cr_chunk_t;
typedef struct {
    cr_chunk_t *head;
} cr_arena_t;
void *cr_arena_alloc(cr_arena_t *a, size_t n);
char *cr_arena_strn(cr_arena_t *a, const char *s, size_t n);
void  cr_arena_free(cr_arena_t *a);

/* Encodings. The decoders return the length written; out may be the input. */
size_t cr_b64_dec(char *out, const char *in, size_t n);
size_t cr_qp_dec(char *out, const char *in, size_t n, bool header);
void   cr_b64_enc(cr_buf_t *b, const void *in, size_t n, int line);   /* line 0: no breaks */
void   cr_qp_enc(cr_buf_t *b, const char *in, size_t n);
/* Streaming base64 for an attachment that goes to a file. */
typedef struct {
    uint32_t acc;
    int      bits;
} cr_b64s_t;
size_t cr_b64_stream(cr_b64s_t *s, uint8_t *out, const char *in, size_t n);

/* Text in a charset to UTF-8 (UTF-8, US-ASCII, ISO-8859-1/15, Windows-1252,
 * others as Latin-1 with what is not ASCII made into '?'-free guesses). The
 * result is valid UTF-8 whatever comes in. */
char *cr_to_utf8(const char *in, size_t n, const char *charset, size_t *out_n);
bool  cr_utf8_valid(const char *s, size_t n);
/* An RFC 2047 header value (with its =?cs?Q|B?...?= words) into UTF-8. */
void  cr_hdr_decode(char *out, size_t on, const char *in, size_t n);
/* And back: words that need it become =?UTF-8?Q?...?=. */
void  cr_hdr_encode(cr_buf_t *b, const char *in, bool phrase);

/* "Fri, 27 Nov 2026 10:22:03 -0300" into seconds since 1970 (UTC), 0 if not. */
uint32_t cr_date_parse(const char *s);
/* IMAP's INTERNALDATE, "27-Nov-2026 10:22:03 -0300". */
uint32_t cr_idate_parse(const char *s);
void     cr_date_rfc(char *out, size_t n, uint32_t t);      /* for a Date: header */
void     cr_date_short(char *out, size_t n, uint32_t t);    /* "10:22", "ayer", "27 nov" */
void     cr_date_long(char *out, size_t n, uint32_t t);     /* "vie 27 nov 2026, 10:22" */
uint32_t cr_now(void);                                       /* 0 without a valid clock */

/* An address list ("Ana <a@x>, b@y") split. Names are decoded. */
typedef struct {
    char name[80];
    char addr[96];
} cr_addr_t;
int  cr_addr_parse(const char *in, cr_addr_t *out, int max);
void cr_addr_show(char *out, size_t n, const cr_addr_t *a);  /* the name, or the address */
bool cr_addr_valid(const char *addr);

/* A file. */
char *cr_file_read(const char *path, size_t *n, size_t max);
bool  cr_file_write(const char *path, const void *d, size_t n);
bool  cr_mkdirs(const char *path);
uint32_t cr_file_size(const char *path);
void  cr_rmtree(const char *path);

/* JSON string escaping for the portal. */
void cr_json_str(cr_buf_t *b, const char *s);

/* IMAP's modified UTF-7 folder names. */
void cr_mutf7_dec(char *out, size_t n, const char *in);
void cr_mutf7_enc(char *out, size_t n, const char *in);

/* ---- accounts and the model ------------------------------------------------------------------- */

enum { CR_SEC_TLS = 0, CR_SEC_STARTTLS = 1, CR_SEC_NONE = 2 };

typedef struct {
    char     id[8];            /* stable, names its password and its cache */
    char     label[32];        /* "Personal" */
    char     name[64];         /* the sender's name */
    char     email[96];
    char     imap_host[96];
    int      imap_port;
    uint8_t  imap_sec;
    char     imap_user[96];
    char     smtp_host[96];
    int      smtp_port;
    uint8_t  smtp_sec;
    char     smtp_user[96];
    char     sig[512];         /* the signature, '\n' inside */
    bool     save_sent;        /* APPEND what is sent (Gmail keeps it itself) */
    char     provider[16];     /* "gmail", "outlook", "icloud", ... or "" */
    uint32_t color;
} cr_acct_cfg_t;

enum { CR_SP_NONE, CR_SP_INBOX, CR_SP_SENT, CR_SP_DRAFTS, CR_SP_ARCHIVE, CR_SP_TRASH, CR_SP_JUNK, CR_SP_ALL,
       CR_SP_FLAGGED };

/* Header flags. The first five are IMAP's own. */
#define CR_F_SEEN      0x0001
#define CR_F_ANSWERED  0x0002
#define CR_F_FLAGGED   0x0004
#define CR_F_DELETED   0x0008
#define CR_F_DRAFT     0x0010
#define CR_F_FORWARDED 0x0020
#define CR_F_ATTACH    0x0040      /* multipart/mixed: a guess from the Content-Type */
#define CR_F_RE        0x0080      /* the subject starts with Re:/Fwd: */

typedef struct {
    uint32_t uid;
    uint32_t date;
    uint32_t size;
    uint32_t mid_h;            /* Message-ID */
    uint32_t root_h;           /* the conversation's first Message-ID */
    uint32_t subj_h;           /* the subject without Re:/Fwd: */
    uint16_t flags;
    uint8_t  acct, folder;
    char     from[56];         /* who, as shown */
    char     from_addr[72];
    char     to[72];           /* the first recipient, as shown (Sent, Drafts) */
    char     subject[124];
} cr_hdr_t;                    /* 352 bytes */

typedef struct {
    char     raw[128];         /* as the server names it (modified UTF-7) */
    char     name[96];         /* the last level, in UTF-8, as shown */
    char     delim;
    uint8_t  depth;
    uint8_t  special;
    bool     noselect;
    uint32_t uidvalidity, uidnext, exists, unseen;
    char     modseq[24];       /* HIGHESTMODSEQ as text: 64 bits the app never does sums on */
    cr_hdr_t *h;               /* headers, by UID ascending */
    int      n, cap;
    bool     loaded;           /* the card's copy was read */
    bool     synced;           /* at least one sync this run */
    bool     all;              /* nothing older on the server */
    bool     dirty;            /* to be saved to the card */
    bool     gone;             /* no longer on the server (kept until the next start) */
} cr_folder_t;

enum { CR_ST_OFF, CR_ST_NO_PASS, CR_ST_CONNECTING, CR_ST_ONLINE, CR_ST_IDLE, CR_ST_ERROR };

typedef struct {
    cr_acct_cfg_t cfg;
    cr_folder_t  *f;           /* CR_FOLDER_MAX of them */
    int           nf;
    int           state;
    char          err[160];    /* the server's or the network's words, for the screen */
    int           err_kind;    /* CR_E_* */
    uint32_t      unseen;      /* in the INBOX */
} cr_acct_t;

enum { CR_E_NONE, CR_E_NET, CR_E_TLS, CR_E_CLOCK, CR_E_AUTH, CR_E_PROTO, CR_E_STARTTLS, CR_E_NOPASS };

typedef struct {
    cr_acct_t a[CR_ACCT_MAX];
    int       na;
    uint32_t  ver;             /* bumped whenever anything a screen shows changes */
    uint32_t  gen;             /* bumped when accounts are added, changed or removed */
    /* options (cr_store.c, opciones.txt) */
    bool      threads;         /* group the lists by conversation */
    bool      notify;          /* notices with the app left, while IDLE is open */
    int       cache_mb;
    bool      boot;            /* open with the board */
} cr_model_t;

#define CR cr_model               /* a short name for a global every app is linked beside */
extern cr_model_t CR;

void cr_lock(void);
void cr_unlock(void);
void cr_touch(void);                       /* CR.ver++ (with the lock held) */

int  cr_folder_special(const cr_acct_t *a, int special);       /* index, -1 */
int  cr_folder_find(const cr_acct_t *a, const char *raw);
cr_hdr_t *cr_folder_hdr(cr_folder_t *f, uint32_t uid);
void cr_folder_put(cr_folder_t *f, const cr_hdr_t *h);          /* insert or replace, by UID */
void cr_folder_drop(cr_folder_t *f, uint32_t uid);
const char *cr_folder_label(const cr_folder_t *f);              /* translated for the special ones */
const char *cr_folder_glyph(const cr_folder_t *f);

/* ---- the store (cr_store.c) ------------------------------------------------------------------- */

const char *cr_dir(void);                  /* <data>/correo */
void cr_store_load(void);                  /* accounts and options, into CR */
bool cr_store_save_accounts(void);
bool cr_store_save_options(void);
bool cr_pass_get(const char *id, char *out, size_t n);
bool cr_pass_set(const char *id, const char *pass);
void cr_pass_erase(const char *id);
void cr_acct_new_id(char *id);

void cr_cache_path(char *out, size_t n, const cr_acct_t *a, const cr_folder_t *f, uint32_t uid);
bool cr_idx_load(cr_acct_t *a, cr_folder_t *f);
bool cr_idx_save(cr_acct_t *a, cr_folder_t *f);
bool cr_folders_load(cr_acct_t *a);
bool cr_folders_save(cr_acct_t *a);
void cr_cache_forget(cr_acct_t *a, cr_folder_t *f);
void cr_cache_account_drop(const char *id);
uint32_t cr_cache_trim(uint32_t cap_bytes);   /* the bytes left */
uint32_t cr_cache_used(void);
void     cr_cache_add(uint32_t n);

/* Contacts, from what was received and sent. */
typedef struct {
    char     name[64];
    char     addr[96];
    uint16_t n;
    uint32_t last;
} cr_contact_t;
void cr_contacts_load(void);
void cr_contacts_add(const char *name, const char *addr, uint32_t when);
void cr_contacts_save(void);
int  cr_contacts_find(const char *prefix, cr_contact_t *out, int max);

/* The autoconfiguration of the common providers. */
typedef struct {
    const char *key;           /* "gmail" */
    const char *name;          /* "Gmail" */
    const char *domains;       /* "gmail.com googlemail.com" */
    const char *imap;
    int         imap_port;
    int         imap_sec;
    const char *smtp;
    int         smtp_port;
    int         smtp_sec;
    bool        user_local;    /* IMAP's user is the part before the @ (iCloud) */
    bool        save_sent;
    const char *help;          /* N_(): how to get an app password */
} cr_provider_t;
extern const cr_provider_t CR_PROVIDERS[];
extern const int CR_NPROVIDERS;
const cr_provider_t *cr_provider_for(const char *email);
const cr_provider_t *cr_provider_key(const char *key);

/* ---- a message (cr_mime.c, cr_html.c) --------------------------------------------------------- */

enum { CR_B_PARA, CR_B_H1, CR_B_H2, CR_B_QUOTE, CR_B_LI, CR_B_PRE, CR_B_HR, CR_B_IMG, CR_B_NOTE, CR_B_FWD };

#define CR_S_BOLD   0x01
#define CR_S_ITAL   0x02
#define CR_S_UNDER  0x04
#define CR_S_LINK   0x08
#define CR_S_CODE   0x10

typedef struct {
    uint32_t start, len;
    uint8_t  style;
    int16_t  link;
} cr_span_t;

typedef struct {
    uint8_t    type;
    uint8_t    lvl;            /* quote depth, list depth */
    char       bullet[12];     /* "•", "3." */
    char      *text;           /* UTF-8, in the message's arena */
    uint32_t   len;
    cr_span_t *sp;
    int        nsp;
    int16_t    img;            /* attachment index, CR_B_IMG */
} cr_block_t;

typedef struct {
    char     name[128];
    char     type[64];
    char     cid[96];
    char     part[24];         /* IMAP section, "2.1" */
    uint32_t off, len;         /* the encoded body, in raw */
    uint8_t  enc;              /* CR_ENC_* */
    uint32_t size;             /* decoded, estimated */
    bool     inl;              /* shown in the text */
    bool     truncated;        /* not all of it in raw: fetched on its own */
    char     charset[24];
} cr_att_t;

enum { CR_ENC_7BIT, CR_ENC_QP, CR_ENC_B64 };

typedef struct {
    char      *raw;
    size_t     raw_len;
    bool       partial;        /* raw is the first CR_BODY_MAX bytes */
    uint32_t   full_size;
    char       from[200], to[600], cc[600], reply_to[200], subject[300];
    char       msgid[200], irt[200], refs[1000];
    char       list_unsub[16];
    uint32_t   date;
    cr_block_t *b;
    int        nb, capb;
    char     **links;
    int        nlinks;
    cr_att_t  *att;
    int        natt;
    bool       html;
    bool       remote_img;     /* there were pictures from the network, not loaded */
    bool       cut;            /* too long: the rest not shown */
    cr_arena_t ar;
    /* for the screen */
    int        acct, folder;
    uint32_t   uid;
    uint16_t   flags;
} cr_msg_t;

cr_msg_t *cr_mime_parse(char *raw, size_t n, bool partial);  /* takes raw */
void      cr_msg_free(cr_msg_t *m);
/* The text, as plain lines (quoting a reply, the portal). */
char     *cr_msg_plain(const cr_msg_t *m, const char *prefix);
/* An attachment's bytes, decoded (NULL if truncated). */
uint8_t  *cr_att_bytes(const cr_msg_t *m, int i, size_t *n);
/* Headers into a cr_hdr_t (a FETCH of HEADER.FIELDS). */
void      cr_hdr_from_fields(cr_hdr_t *h, const char *fields, size_t n);

/* The rich-text builder both decoders write with. */
typedef struct {
    cr_msg_t *m;
    cr_buf_t  t;               /* the open block's text */
    cr_span_t *sp;
    int       nsp, capsp;
    uint8_t   style;
    int16_t   link;
    int       type, lvl;
    char      bullet[12];
    bool      space;           /* the last thing written was white space */
    uint32_t  total;           /* text written in all */
} cr_rt_t;
void cr_rt_init(cr_rt_t *r, cr_msg_t *m);
void cr_rt_block(cr_rt_t *r, int type, int lvl, const char *bullet);   /* ends the open one */
void cr_rt_text(cr_rt_t *r, const char *s, size_t n);
void cr_rt_end(cr_rt_t *r);
int  cr_rt_link(cr_rt_t *r, const char *url, size_t n);
void cr_rt_plain(cr_rt_t *r, const char *s, size_t n, bool flowed);   /* text/plain */
void cr_rt_img(cr_rt_t *r, int att);
void cr_rt_free(cr_rt_t *r);

void cr_html_render(cr_rt_t *r, const char *html, size_t n, cr_msg_t *m);

/* Putting a message together. */
typedef struct {
    char  path[256];
    char  name[128];
    char  type[64];
    uint32_t size;
    /* or bytes taken from another message (forwarding) */
    uint8_t *data;
    size_t   data_n;
} cr_out_att_t;

typedef struct {
    int      acct;
    char     to[1024], cc[1024], bcc[512];
    char     subject[300];
    char    *body;             /* UTF-8, '\n' lines */
    char     irt[200];
    char    *refs;
    cr_out_att_t *att;
    int      natt;
    bool     draft;
    /* what to do after sending */
    int      reply_folder;
    uint32_t reply_uid;
    bool     forward;
    int      draft_folder;
    uint32_t draft_uid;        /* the draft this replaces, to be deleted */
} cr_out_t;

char *cr_mime_build(const cr_out_t *o, const cr_acct_cfg_t *from, size_t *n, char *msgid, size_t mn);
void  cr_out_free(cr_out_t *o);
const char *cr_mime_type_for(const char *name);

/* ---- the network (cr_net.c, cr_sync.c) -------------------------------------------------------- */

enum {
    CR_J_OPEN,          /* acct, folder, uid -> out: cr_msg_t* */
    CR_J_FLAGS,         /* acct, folder, uid, arg: flags to add, arg2: to take away */
    CR_J_MOVE,          /* acct, folder, uid, arg: destination folder */
    CR_J_DELETE,        /* acct, folder, uid */
    CR_J_SYNC,          /* acct, folder */
    CR_J_MORE,          /* acct, folder: older headers */
    CR_J_SEARCH,        /* acct, folder, text -> out: cr_hdr_t array, n */
    CR_J_SEND,          /* out: cr_out_t* (taken) */
    CR_J_DRAFT,         /* same, saved */
    CR_J_PART,          /* acct, folder, uid, text: section, path: file */
    CR_J_TEST,          /* cfg + pass: try both servers */
    CR_J_RECONNECT,     /* acct */
    CR_J_EMPTY,         /* acct, folder: everything \Deleted and expunged */
    CR_J_IMAGE,         /* in: a picture's bytes, arg: max width, arg2: max height -> out: RGB565, n: w | h << 16 */
};

typedef struct cr_job {
    int       kind;
    uint32_t  token;           /* the screen's, to tell a stale answer */
    int       acct, folder;
    uint32_t  uid;
    int       arg, arg2;
    char      text[300];
    char      path[256];
    cr_out_t *out_msg;
    cr_acct_cfg_t *cfg;        /* CR_J_TEST */
    uint8_t  *in;              /* CR_J_IMAGE */
    size_t    in_n;
    char      pass[128];
    /* answers */
    int       ok;              /* 1 done, 0 failed */
    int       err_kind;
    char      why[200];
    void     *out;
    int       n;
    struct cr_job *next;
} cr_job_t;

void      cr_sync_start(void);
void      cr_sync_stop(void);              /* waits for the thread */
bool      cr_sync_running(void);
cr_job_t *cr_job_new(int kind);
void      cr_job_put(cr_job_t *j);
cr_job_t *cr_job_done(void);               /* the next finished one, NULL none */
void      cr_job_free(cr_job_t *j);
void      cr_sync_kick(int acct);          /* reconnect now (a password was typed) */
void      cr_sync_reload(void);            /* the accounts changed */
void      cr_sync_bg(bool hidden);         /* the app was left or came back */

/* Events for the screens. */
enum { CR_EV_NEW_MAIL, CR_EV_STATE, CR_EV_SENT };
typedef struct {
    int      kind;
    int      acct;
    int      n;
    char     text[200];
} cr_event_t;
bool cr_event_take(cr_event_t *e);
void cr_event_put(const cr_event_t *e);

/* Set while the app closes: every wait of the connection gives up. */
extern volatile bool cr_net_abort;

/* The protocol, for cr_sync.c. */
typedef struct cr_conn cr_conn_t;
cr_conn_t *cr_conn_new(void);
void       cr_conn_free(cr_conn_t *c);
int        cr_conn_open(cr_conn_t *c, const char *host, int port, int sec, int timeout_ms);
bool       cr_conn_is_open(const cr_conn_t *c);
void       cr_conn_close(cr_conn_t *c);
int        cr_conn_starttls(cr_conn_t *c, const char *host, int timeout_ms);
int        cr_conn_line(cr_conn_t *c, char *out, int max, int timeout_ms);  /* len, -2 timeout, -1 closed */
int        cr_conn_read(cr_conn_t *c, void *out, int n, int timeout_ms);
bool       cr_conn_send(cr_conn_t *c, const void *d, int n);
bool       cr_conn_puts(cr_conn_t *c, const char *s);
const char *cr_conn_why(const cr_conn_t *c);
int        cr_conn_err(const cr_conn_t *c);    /* CR_E_* of the last failure */
bool       cr_conn_pending(cr_conn_t *c);      /* bytes already buffered */

/* SMTP: one message, the whole conversation. */
int cr_smtp_send(const cr_acct_cfg_t *cfg, const char *pass, const char *from, const char **rcpt, int nrcpt,
                 const char *msg, size_t n, char *why, size_t wn);
int cr_smtp_test(const cr_acct_cfg_t *cfg, const char *pass, char *why, size_t wn);

/* ---- the screens (cr_ui*.c) ------------------------------------------------------------------- */

void cr_ui_create(void *root);
void cr_ui_destroy(void);
void cr_ui_tick(void);                     /* correo.c's timer, 10 times a second */
bool cr_ui_back(void);
void cr_ui_resize(void *root);
void cr_ui_shown(bool shown);
void cr_ui_open_arg(const char *arg);      /* "mailto:..." from another app */

/* cr_portal.c */
void cr_portal_tick(void);
void cr_portal_job(cr_job_t *j);           /* a finished job the page asked for */
bool cr_portal_owns(const cr_job_t *j);
void cr_portal_stop(void);
