/*
 * P4OS - Correo: what is kept on the card and in the preferences (cr.h).
 *
 *   <data>/correo/cuentas.txt      the accounts, as text (no passwords)
 *   <data>/correo/opciones.txt     the options
 *   <data>/correo/contactos.txt    addresses seen, with how often
 *   <data>/correo/cache/<id>/      per account: carpetas.txt (the folder
 *                                  list), <folder>.idx (its headers) and
 *                                  <folder>/<uid>.eml (bodies opened)
 *
 * The password of each account is a preference of its own, cr_pass_<id>:
 * "pass" in the name is how a backup without a passphrase knows to leave it
 * out (docs/BACKUP.md). It is never written to a file, a log or the portal.
 *
 * The bodies are the only big thing: the cache keeps them under the cap
 * the options say (100 MB unless changed), dropping the oldest first.
 */
#include "cr.h"

#include "aos_hal.h"
#include "aos_i18n.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

const char *cr_dir(void)
{
    static char d[160];
    if (!d[0]) snprintf(d, sizeof d, "%s/correo", aos_hal_path_data());
    return d;
}

/* ---- the providers ----------------------------------------------------------------------------- */

const cr_provider_t CR_PROVIDERS[] = {
    {"gmail", "Gmail", "gmail.com googlemail.com", "imap.gmail.com", 993, CR_SEC_TLS, "smtp.gmail.com", 465,
     CR_SEC_TLS, false, false,
     N_("Gmail no deja entrar con tu contraseña de siempre: pide una contraseña de aplicación. Activá la "
        "verificación en dos pasos en tu cuenta de Google, buscá \"Contraseñas de aplicaciones\" en Seguridad, "
        "creá una para Correo y escribila acá (son 16 letras; los espacios no importan).")},
    {"outlook", "Outlook", "outlook.com hotmail.com live.com msn.com outlook.es hotmail.es live.com.ar",
     "outlook.office365.com", 993, CR_SEC_TLS, "smtp-mail.outlook.com", 587, CR_SEC_STARTTLS, false, true,
     N_("Outlook pide una contraseña de aplicación: con la verificación en dos pasos activada, creala en "
        "account.microsoft.com, en Seguridad. Microsoft está pasando sus cuentas a OAuth2, que Correo todavía "
        "no tiene: si la contraseña de aplicación no anda, la cuenta no se puede usar por ahora. Para mandar "
        "usa el puerto 587 con STARTTLS.")},
    {"icloud", "iCloud", "icloud.com me.com mac.com", "imap.mail.me.com", 993, CR_SEC_TLS, "smtp.mail.me.com", 587,
     CR_SEC_STARTTLS, true, true,
     N_("iCloud pide una contraseña de app: entrá a account.apple.com, Inicio de sesión y seguridad, "
        "Contraseñas de apps, y creá una para Correo. El usuario del correo entrante es lo que va antes de "
        "la @. Para mandar usa el puerto 587 con STARTTLS.")},
    {"fastmail", "Fastmail", "fastmail.com fastmail.fm messagingengine.com", "imap.fastmail.com", 993, CR_SEC_TLS,
     "smtp.fastmail.com", 465, CR_SEC_TLS, false, true,
     N_("Fastmail pide una contraseña de app: Configuración, Privacidad y seguridad, Contraseñas de apps; "
        "elegí acceso por IMAP y SMTP.")},
    {"yahoo", "Yahoo", "yahoo.com yahoo.com.ar ymail.com rocketmail.com", "imap.mail.yahoo.com", 993, CR_SEC_TLS,
     "smtp.mail.yahoo.com", 465, CR_SEC_TLS, false, true,
     N_("Yahoo pide una contraseña de aplicación: Información de la cuenta, Seguridad de la cuenta, Generar "
        "contraseña de aplicación.")},
    {"propio", N_("Otro"), "", "", 993, CR_SEC_TLS, "", 465, CR_SEC_TLS, false, true,
     N_("Con un servidor propio o de tu proveedor, los datos te los da quien te da el correo. Correo prueba "
        "imap.<dominio> y smtp.<dominio> con SSL/TLS; si no son esos, cambialos abajo.")},
};
const int CR_NPROVIDERS = sizeof CR_PROVIDERS / sizeof CR_PROVIDERS[0];

const cr_provider_t *cr_provider_key(const char *key)
{
    for (int i = 0; i < CR_NPROVIDERS; i++) {
        if (!strcmp(CR_PROVIDERS[i].key, key)) return &CR_PROVIDERS[i];
    }
    return NULL;
}

const cr_provider_t *cr_provider_for(const char *email)
{
    const char *at = strchr(email, '@');
    if (!at) return NULL;
    const char *dom = at + 1;
    size_t dl = strlen(dom);
    for (int i = 0; i < CR_NPROVIDERS; i++) {
        const char *d = CR_PROVIDERS[i].domains;
        while (*d) {
            size_t l = strcspn(d, " ");
            if (l == dl && !strncasecmp(d, dom, l)) return &CR_PROVIDERS[i];
            d += l;
            while (*d == ' ') d++;
        }
    }
    return NULL;
}

/* ---- passwords --------------------------------------------------------------------------------- */

static void pass_key(char *k, size_t n, const char *id)
{
    snprintf(k, n, "cr_pass_%.6s", id);
}

bool cr_pass_get(const char *id, char *out, size_t n)
{
    char k[20];
    pass_key(k, sizeof k, id);
    return aos_hal_pref_get_str(k, out, n);
}

bool cr_pass_set(const char *id, const char *pass)
{
    char k[20];
    pass_key(k, sizeof k, id);
    return aos_hal_pref_set_str(k, pass);
}

void cr_pass_erase(const char *id)
{
    char k[20];
    pass_key(k, sizeof k, id);
    aos_hal_pref_erase(k);
}

void cr_acct_new_id(char *id)
{
    static const char A[] = "abcdefghijkmnpqrstuvwxyz23456789";
    for (;;) {
        uint8_t r[5];
        aos_hal_random(r, sizeof r);
        for (int i = 0; i < 5; i++) id[i] = A[r[i] % (sizeof A - 1)];
        id[5] = '\0';
        bool used = false;
        for (int i = 0; i < CR.na; i++) {
            if (!strcmp(CR.a[i].cfg.id, id)) used = true;
        }
        if (!used) return;
    }
}

/* ---- accounts and options ---------------------------------------------------------------------- */

static void esc(cr_buf_t *b, const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') cr_buf_str(b, "\\n");
        else if (*s == '\\') cr_buf_str(b, "\\\\");
        else if (*s != '\r') cr_buf_add(b, s, 1);
    }
}

static void unesc(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '\\' && s[1] == 'n') {
            *o++ = '\n';
            s++;
        } else if (*s == '\\' && s[1] == '\\') {
            *o++ = '\\';
            s++;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

static const char *SEC[] = {"tls", "starttls", "ninguna"};

static void server_put(cr_buf_t *b, const char *k, const char *host, int port, int sec)
{
    cr_buf_printf(b, "%s = %s:%d:%s\n", k, host, port, SEC[sec >= 0 && sec <= 2 ? sec : 0]);
}

static void server_get(const char *v, char *host, size_t hn, int *port, uint8_t *sec)
{
    const char *c1 = strchr(v, ':');
    if (!c1) {
        cr_copy(host, hn, v);
        return;
    }
    cr_ncopy(host, hn, v, (size_t)(c1 - v));
    *port = atoi(c1 + 1);
    const char *c2 = strchr(c1 + 1, ':');
    *sec = CR_SEC_TLS;
    if (c2) {
        for (int i = 0; i < 3; i++) {
            if (!strcmp(c2 + 1, SEC[i])) *sec = (uint8_t)i;
        }
    }
}

bool cr_store_save_accounts(void)
{
    cr_mkdirs(cr_dir());
    cr_buf_t b = {0};
    cr_buf_str(&b, "# Correo: las cuentas. Las contraseñas no están acá: son preferencias de la placa.\n");
    for (int i = 0; i < CR.na; i++) {
        const cr_acct_cfg_t *c = &CR.a[i].cfg;
        cr_buf_printf(&b, "\n[cuenta]\nid = %s\n", c->id);
        cr_buf_str(&b, "etiqueta = ");
        esc(&b, c->label);
        cr_buf_str(&b, "\nnombre = ");
        esc(&b, c->name);
        cr_buf_printf(&b, "\nemail = %s\n", c->email);
        server_put(&b, "imap", c->imap_host, c->imap_port, c->imap_sec);
        cr_buf_printf(&b, "imap_usuario = %s\n", c->imap_user);
        server_put(&b, "smtp", c->smtp_host, c->smtp_port, c->smtp_sec);
        cr_buf_printf(&b, "smtp_usuario = %s\n", c->smtp_user);
        cr_buf_str(&b, "firma = ");
        esc(&b, c->sig);
        cr_buf_printf(&b, "\nguardar_enviados = %d\nproveedor = %s\ncolor = %06lX\n", c->save_sent ? 1 : 0,
                      c->provider, (unsigned long)c->color);
    }
    char path[200];
    snprintf(path, sizeof path, "%s/cuentas.txt", cr_dir());
    bool ok = !b.oom && cr_file_write(path, b.p ? b.p : "", b.n);
    cr_buf_free(&b);
    return ok;
}

bool cr_store_save_options(void)
{
    cr_mkdirs(cr_dir());
    char t[300];
    int n = snprintf(t, sizeof t,
                     "# Correo: las opciones\nhilos = %d\navisos = %d\ncache_mb = %d\narranque = %d\n",
                     CR.threads, CR.notify, CR.cache_mb, CR.boot);
    char path[200];
    snprintf(path, sizeof path, "%s/opciones.txt", cr_dir());
    return cr_file_write(path, t, (size_t)n);
}

static void kv_each(const char *path, void (*fn)(const char *k, const char *v, void *ctx), void *ctx)
{
    size_t n;
    char *t = cr_file_read(path, &n, 256 * 1024);
    if (!t) return;
    char *p = t;
    while (*p) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        char *line = p;
        p = e ? e + 1 : p + strlen(p);
        cr_trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (line[0] == '[') {
            fn(line, "", ctx);
            continue;
        }
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = line, *v = eq + 1;
        cr_trim(k);
        cr_trim(v);
        fn(k, v, ctx);
    }
    cr_free(t);
}

static void acct_kv(const char *k, const char *v, void *ctx)
{
    (void)ctx;
    if (!strcmp(k, "[cuenta]")) {
        if (CR.na < CR_ACCT_MAX) {
            cr_acct_t *a = &CR.a[CR.na++];
            cr_folder_t *f = a->f;
            memset(a, 0, sizeof *a);
            a->f = f;
            a->cfg.imap_port = 993;
            a->cfg.smtp_port = 465;
            a->cfg.save_sent = true;
            a->cfg.color = 0x0A84FF;
        }
        return;
    }
    if (!CR.na) return;
    cr_acct_cfg_t *c = &CR.a[CR.na - 1].cfg;
    char tmp[600];
    cr_copy(tmp, sizeof tmp, v);
    unesc(tmp);
    if (!strcmp(k, "id")) cr_copy(c->id, sizeof c->id, tmp);
    else if (!strcmp(k, "etiqueta")) cr_copy(c->label, sizeof c->label, tmp);
    else if (!strcmp(k, "nombre")) cr_copy(c->name, sizeof c->name, tmp);
    else if (!strcmp(k, "email")) cr_copy(c->email, sizeof c->email, tmp);
    else if (!strcmp(k, "imap")) server_get(tmp, c->imap_host, sizeof c->imap_host, &c->imap_port, &c->imap_sec);
    else if (!strcmp(k, "imap_usuario")) cr_copy(c->imap_user, sizeof c->imap_user, tmp);
    else if (!strcmp(k, "smtp")) server_get(tmp, c->smtp_host, sizeof c->smtp_host, &c->smtp_port, &c->smtp_sec);
    else if (!strcmp(k, "smtp_usuario")) cr_copy(c->smtp_user, sizeof c->smtp_user, tmp);
    else if (!strcmp(k, "firma")) cr_copy(c->sig, sizeof c->sig, tmp);
    else if (!strcmp(k, "guardar_enviados")) c->save_sent = atoi(tmp) != 0;
    else if (!strcmp(k, "proveedor")) cr_copy(c->provider, sizeof c->provider, tmp);
    else if (!strcmp(k, "color")) c->color = (uint32_t)strtoul(tmp, NULL, 16);
}

static void opt_kv(const char *k, const char *v, void *ctx)
{
    (void)ctx;
    int n = atoi(v);
    if (!strcmp(k, "hilos")) CR.threads = n != 0;
    else if (!strcmp(k, "avisos")) CR.notify = n != 0;
    else if (!strcmp(k, "cache_mb")) CR.cache_mb = n >= 10 && n <= 4000 ? n : 100;
    else if (!strcmp(k, "arranque")) CR.boot = n != 0;
}

void cr_store_load(void)
{
    for (int i = 0; i < CR_ACCT_MAX; i++) {
        if (!CR.a[i].f) CR.a[i].f = cr_alloc(sizeof(cr_folder_t) * CR_FOLDER_MAX);
    }
    CR.na = 0;
    CR.threads = true;
    CR.notify = true;
    CR.cache_mb = 100;
    CR.boot = false;
    char path[200];
    snprintf(path, sizeof path, "%s/cuentas.txt", cr_dir());
    kv_each(path, acct_kv, NULL);
    /* an account without an id or an address is not one */
    int k = 0;
    for (int i = 0; i < CR.na; i++) {
        if (!CR.a[i].cfg.id[0] || !CR.a[i].cfg.email[0]) continue;
        if (k != i) {
            cr_folder_t *f = CR.a[k].f;
            CR.a[k] = CR.a[i];
            CR.a[i].f = f;
        }
        k++;
    }
    CR.na = k;
    snprintf(path, sizeof path, "%s/opciones.txt", cr_dir());
    kv_each(path, opt_kv, NULL);
}

/* ---- the cache --------------------------------------------------------------------------------- */

static void acct_dir(char *out, size_t n, const char *id)
{
    snprintf(out, n, "%s/cache/%.8s", cr_dir(), id);
}

static uint32_t fhash(const cr_folder_t *f)
{
    return cr_hash(f->raw, strlen(f->raw));
}

void cr_cache_path(char *out, size_t n, const cr_acct_t *a, const cr_folder_t *f, uint32_t uid)
{
    char d[200];
    acct_dir(d, sizeof d, a->cfg.id);
    snprintf(out, n, "%s/%08lx/%lu.eml", d, (unsigned long)fhash(f), (unsigned long)uid);
}

typedef struct {
    char     magic[4];
    uint32_t version;
    uint32_t uidvalidity, uidnext, unseen, exists;
    char     modseq[24];
    uint32_t n;
    uint8_t  all;
    uint8_t  pad[3];
    uint32_t hdr_size;
} idx_head_t;

#define IDX_VERSION 1

bool cr_idx_load(cr_acct_t *a, cr_folder_t *f)
{
    char d[200], path[260];
    acct_dir(d, sizeof d, a->cfg.id);
    snprintf(path, sizeof path, "%s/%08lx.idx", d, (unsigned long)fhash(f));
    FILE *fp = fopen(path, "rb");
    f->loaded = true;
    if (!fp) return false;
    idx_head_t h;
    bool ok = fread(&h, sizeof h, 1, fp) == 1 && !memcmp(h.magic, "CRX1", 4) && h.version == IDX_VERSION &&
              h.hdr_size == sizeof(cr_hdr_t) && h.n < 200000;
    if (ok) {
        cr_hdr_t *arr = h.n ? cr_alloc(sizeof(cr_hdr_t) * h.n) : NULL;
        if (h.n && (!arr || fread(arr, sizeof(cr_hdr_t), h.n, fp) != h.n)) {
            cr_free(arr);
            ok = false;
        } else {
            cr_free(f->h);
            f->h = arr;
            f->n = f->cap = (int)h.n;
            f->uidvalidity = h.uidvalidity;
            f->uidnext = h.uidnext;
            f->unseen = h.unseen;
            f->exists = h.exists;
            f->all = h.all != 0;
            cr_copy(f->modseq, sizeof f->modseq, h.modseq);
        }
    }
    fclose(fp);
    return ok;
}

bool cr_idx_save(cr_acct_t *a, cr_folder_t *f)
{
    char d[200], path[260];
    acct_dir(d, sizeof d, a->cfg.id);
    cr_mkdirs(d);
    snprintf(path, sizeof path, "%s/%08lx.idx", d, (unsigned long)fhash(f));
    idx_head_t h;
    memset(&h, 0, sizeof h);
    memcpy(h.magic, "CRX1", 4);
    h.version = IDX_VERSION;
    h.uidvalidity = f->uidvalidity;
    h.uidnext = f->uidnext;
    h.unseen = f->unseen;
    h.exists = f->exists;
    cr_copy(h.modseq, sizeof h.modseq, f->modseq);
    h.n = f->h ? (uint32_t)f->n : 0;
    h.all = f->all;
    h.hdr_size = sizeof(cr_hdr_t);
    char tmp[270];
    snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return false;
    bool ok = fwrite(&h, sizeof h, 1, fp) == 1 && (!h.n || fwrite(f->h, sizeof(cr_hdr_t), h.n, fp) == h.n);
    ok = fclose(fp) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return false;
    }
    unlink(path);
    return rename(tmp, path) == 0;
}

bool cr_folders_save(cr_acct_t *a)
{
    char d[200], path[260];
    acct_dir(d, sizeof d, a->cfg.id);
    cr_mkdirs(d);
    snprintf(path, sizeof path, "%s/carpetas.txt", d);
    cr_buf_t b = {0};
    for (int i = 0; i < a->nf; i++) {
        const cr_folder_t *f = &a->f[i];
        if (f->gone) continue;
        cr_buf_printf(&b, "%d\t%d\t%d\t%lu\t%lu\t%s\n", f->delim ? f->delim : '/', f->special, f->noselect,
                      (unsigned long)f->unseen, (unsigned long)f->exists, f->raw);
    }
    bool ok = !b.oom && cr_file_write(path, b.p ? b.p : "", b.n);
    cr_buf_free(&b);
    return ok;
}

bool cr_folders_load(cr_acct_t *a)
{
    char d[200], path[260];
    acct_dir(d, sizeof d, a->cfg.id);
    snprintf(path, sizeof path, "%s/carpetas.txt", d);
    size_t n;
    char *t = cr_file_read(path, &n, 64 * 1024);
    if (!t) return false;
    char *p = t;
    while (*p && a->nf < CR_FOLDER_MAX) {
        char *e = strchr(p, '\n');
        if (e) *e = '\0';
        int delim, sp, ns;
        unsigned long un, ex;
        int used = 0;
        if (sscanf(p, "%d\t%d\t%d\t%lu\t%lu\t%n", &delim, &sp, &ns, &un, &ex, &used) == 5 && used) {
            cr_folder_t *f = &a->f[a->nf++];
            memset(f, 0, sizeof *f);
            cr_copy(f->raw, sizeof f->raw, p + used);
            f->delim = (char)delim;
            f->special = (uint8_t)sp;
            f->noselect = ns != 0;
            f->unseen = (uint32_t)un;
            f->exists = (uint32_t)ex;
            char utf[256];
            cr_mutf7_dec(utf, sizeof utf, f->raw);
            const char *base = utf;
            int depth = 0;
            for (const char *q = utf; *q; q++) {
                if (*q == f->delim) {
                    depth++;
                    base = q + 1;
                }
            }
            cr_copy(f->name, sizeof f->name, base);
            f->depth = (uint8_t)(depth > 6 ? 6 : depth);
        }
        if (!e) break;
        p = e + 1;
    }
    cr_free(t);
    return true;
}

void cr_cache_forget(cr_acct_t *a, cr_folder_t *f)
{
    char d[200], path[260];
    acct_dir(d, sizeof d, a->cfg.id);
    snprintf(path, sizeof path, "%s/%08lx", d, (unsigned long)fhash(f));
    cr_rmtree(path);
    snprintf(path, sizeof path, "%s/%08lx.idx", d, (unsigned long)fhash(f));
    unlink(path);
}

void cr_cache_account_drop(const char *id)
{
    char d[200];
    acct_dir(d, sizeof d, id);
    cr_rmtree(d);
}

/* The bodies: every .eml and .part under cache/<id>/<folder>/, the oldest
 * first out. */
typedef struct {
    char     path[96];
    uint32_t size, mtime;
} ce_t;

typedef struct {
    ce_t    *e;
    int      n, cap;
    uint32_t total;
    char     base[200];
} cscan_t;

static uint32_t s_used;
static bool     s_used_known;

static bool scan_file(const aos_dir_entry_t *e, void *ctx)
{
    cscan_t *s = ctx;
    if (e->dir || e->name[0] == '.') return true;
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 256;
        ce_t *ne = cr_realloc(s->e, sizeof(ce_t) * (size_t)c);
        if (!ne) return false;
        s->e = ne;
        s->cap = c;
    }
    ce_t *c = &s->e[s->n++];
    snprintf(c->path, sizeof c->path, "%.60s/%.30s", s->base + strlen(cr_dir()) + 7, e->name);
    c->size = e->size;
    c->mtime = e->mtime;
    s->total += e->size;
    return true;
}

typedef struct {
    cscan_t *s;
    char     dir[200];
    int      depth;
} cwalk_t;

static bool scan_dir(const aos_dir_entry_t *e, void *ctx)
{
    cwalk_t *w = ctx;
    if (!e->dir || e->name[0] == '.') return true;
    char sub[200];
    snprintf(sub, sizeof sub, "%.150s/%.40s", w->dir, e->name);
    if (w->depth == 0) {
        cwalk_t inner = {w->s, "", 1};
        cr_copy(inner.dir, sizeof inner.dir, sub);
        aos_hal_dir_scan(sub, scan_dir, &inner);
    } else {
        cr_copy(w->s->base, sizeof w->s->base, sub);
        aos_hal_dir_scan(sub, scan_file, w->s);
    }
    return true;
}

static int cmp_age(const void *a, const void *b)
{
    const ce_t *x = a, *y = b;
    return x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
}

uint32_t cr_cache_trim(uint32_t cap)
{
    cscan_t s = {0};
    cwalk_t w = {&s, "", 0};
    snprintf(w.dir, sizeof w.dir, "%s/cache", cr_dir());
    aos_hal_dir_scan(w.dir, scan_dir, &w);
    if (s.total > cap && s.n) {
        qsort(s.e, (size_t)s.n, sizeof(ce_t), cmp_age);
        uint32_t goal = cap / 10 * 9;
        int gone = 0;
        for (int i = 0; i < s.n && s.total > goal; i++) {
            char p[300];
            snprintf(p, sizeof p, "%s/cache/%s", cr_dir(), s.e[i].path);
            if (unlink(p) == 0) {
                s.total -= s.e[i].size;
                gone++;
            }
        }
        aos_hal_log(CR_TAG, "cache: %d bodies dropped, %lu KB left", gone, (unsigned long)(s.total / 1024));
    }
    cr_free(s.e);
    s_used = s.total;
    s_used_known = true;
    return s.total;
}

uint32_t cr_cache_used(void)
{
    return s_used;
}

void cr_cache_add(uint32_t n)
{
    s_used += n;
}

/* ---- contacts ---------------------------------------------------------------------------------- */

#define CONTACTS_MAX 800

static cr_contact_t *s_ct;
static int           s_nct;
static bool          s_ct_dirty;
static void         *s_ct_mx;

static void ct_lock(void)
{
    if (!s_ct_mx) s_ct_mx = aos_hal_mutex_create();
    aos_hal_mutex_lock(s_ct_mx);
}

static void ct_unlock(void)
{
    aos_hal_mutex_unlock(s_ct_mx);
}

void cr_contacts_load(void)
{
    ct_lock();
    if (!s_ct) s_ct = cr_alloc(sizeof(cr_contact_t) * CONTACTS_MAX);
    s_nct = 0;
    char path[200];
    snprintf(path, sizeof path, "%s/contactos.txt", cr_dir());
    size_t n;
    char *t = s_ct ? cr_file_read(path, &n, 512 * 1024) : NULL;
    if (t) {
        char *p = t;
        while (*p && s_nct < CONTACTS_MAX) {
            char *e = strchr(p, '\n');
            if (e) *e = '\0';
            /* count \t last \t address \t name */
            char *f1 = strchr(p, '\t');
            char *f2 = f1 ? strchr(f1 + 1, '\t') : NULL;
            char *f3 = f2 ? strchr(f2 + 1, '\t') : NULL;
            if (f1 && f2) {
                cr_contact_t *c = &s_ct[s_nct++];
                memset(c, 0, sizeof *c);
                c->n = (uint16_t)atoi(p);
                c->last = (uint32_t)strtoul(f1 + 1, NULL, 10);
                cr_ncopy(c->addr, sizeof c->addr, f2 + 1, f3 ? (size_t)(f3 - f2 - 1) : strlen(f2 + 1));
                if (f3) cr_copy(c->name, sizeof c->name, f3 + 1);
            }
            if (!e) break;
            p = e + 1;
        }
        cr_free(t);
    }
    ct_unlock();
}

void cr_contacts_add(const char *name, const char *addr, uint32_t when)
{
    if (!addr || !cr_addr_valid(addr)) return;
    /* no-reply addresses are not people one writes to */
    if (cr_istr(addr, "noreply") || cr_istr(addr, "no-reply") || cr_istr(addr, "donotreply") ||
        cr_istr(addr, "mailer-daemon"))
        return;
    ct_lock();
    if (!s_ct) {
        ct_unlock();
        return;
    }
    for (int i = 0; i < s_nct; i++) {
        if (!strcasecmp(s_ct[i].addr, addr)) {
            if (s_ct[i].n < 60000) s_ct[i].n++;
            if (when > s_ct[i].last) s_ct[i].last = when;
            if (name && name[0] && strcmp(name, addr)) cr_copy(s_ct[i].name, sizeof s_ct[i].name, name);
            s_ct_dirty = true;
            ct_unlock();
            return;
        }
    }
    int slot = s_nct;
    if (s_nct == CONTACTS_MAX) {
        /* full: the least used goes */
        slot = 0;
        for (int i = 1; i < s_nct; i++) {
            if (s_ct[i].n < s_ct[slot].n || (s_ct[i].n == s_ct[slot].n && s_ct[i].last < s_ct[slot].last)) slot = i;
        }
    } else {
        s_nct++;
    }
    cr_contact_t *c = &s_ct[slot];
    memset(c, 0, sizeof *c);
    cr_copy(c->addr, sizeof c->addr, addr);
    if (name && strcmp(name, addr)) cr_copy(c->name, sizeof c->name, name);
    c->n = 1;
    c->last = when;
    s_ct_dirty = true;
    ct_unlock();
}

void cr_contacts_save(void)
{
    ct_lock();
    if (!s_ct_dirty || !s_ct) {
        ct_unlock();
        return;
    }
    cr_buf_t b = {0};
    for (int i = 0; i < s_nct; i++) {
        cr_buf_printf(&b, "%u\t%lu\t%s\t%s\n", s_ct[i].n, (unsigned long)s_ct[i].last, s_ct[i].addr, s_ct[i].name);
    }
    s_ct_dirty = false;
    ct_unlock();
    cr_mkdirs(cr_dir());
    char path[200];
    snprintf(path, sizeof path, "%s/contactos.txt", cr_dir());
    if (!b.oom) cr_file_write(path, b.p ? b.p : "", b.n);
    cr_buf_free(&b);
}

static bool word_starts(const char *s, const char *p)
{
    if (cr_istarts(s, p)) return true;
    for (const char *q = s; *q; q++) {
        if ((*q == ' ' || *q == '.' || *q == '_' || *q == '-') && cr_istarts(q + 1, p)) return true;
    }
    return false;
}

int cr_contacts_find(const char *prefix, cr_contact_t *out, int max)
{
    int k = 0;
    if (!prefix || !prefix[0]) return 0;
    ct_lock();
    for (int i = 0; i < s_nct; i++) {
        const cr_contact_t *c = &s_ct[i];
        if (!word_starts(c->addr, prefix) && !word_starts(c->name, prefix)) continue;
        /* kept by how often, then how lately */
        int pos = k;
        while (pos > 0 && (out[pos - 1].n < c->n || (out[pos - 1].n == c->n && out[pos - 1].last < c->last))) pos--;
        if (pos >= max) continue;
        int last = k < max ? k : max - 1;
        memmove(&out[pos + 1], &out[pos], sizeof(cr_contact_t) * (size_t)(last - pos));
        out[pos] = *c;
        if (k < max) k++;
    }
    ct_unlock();
    return k;
}
