/*
 * P4OS - the SSH server's algorithms. Few on purpose, each one what an
 * OpenSSH of the last years picks first among what the board does well:
 *
 *   key exchange  curve25519-sha256 (RFC 8731), X25519 from Monocypher
 *   host key      ssh-ed25519 (RFC 8709), Monocypher
 *   cipher        aes128-gcm@openssh.com, aes256-gcm@openssh.com (RFC 5647
 *                 as OpenSSH does it): mbedTLS, with the P4's AES engine;
 *                 chacha20-poly1305@openssh.com (aos_ssh_chacha.c), which
 *                 OpenSSH picks first and the client app speaks
 *   client keys   ssh-ed25519 (Monocypher), ecdsa-sha2-nistp256 and
 *                 rsa-sha2-256/512 (mbedTLS); never ssh-rsa with SHA-1
 *
 * The random numbers are the chip's: on the P4, which has no radio of its
 * own, ESP-IDF leaves the SAR ADC's noise feeding the generator from boot
 * (docs/en/api-reference/system/random.rst).
 */
#include "aos_ssh_int.h"
#include "aos_ssh_chacha.h"

#include "monocypher.h"
#include "monocypher-ed25519.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/gcm.h"
#include "mbedtls/rsa.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/bignum.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
void ssh_random(void *buf, size_t n) { esp_fill_random(buf, n); }
#else
void ssh_random(void *buf, size_t n) { arc4random_buf(buf, n); }
#endif

void ssh_sha256(const void *d, size_t n, uint8_t out[32]) { mbedtls_sha256(d, n, out, 0); }

struct ssh_sha256_ctx { mbedtls_sha256_context c; };

ssh_sha256_ctx_t *ssh_sha256_new(void)
{
    ssh_sha256_ctx_t *c = malloc(sizeof *c);
    if (!c) return NULL;
    mbedtls_sha256_init(&c->c);
    mbedtls_sha256_starts(&c->c, 0);
    return c;
}

void ssh_sha256_add(ssh_sha256_ctx_t *c, const void *d, size_t n)
{
    if (c) mbedtls_sha256_update(&c->c, d, n);
}

void ssh_sha256_end(ssh_sha256_ctx_t *c, uint8_t out[32])
{
    if (!c) {
        memset(out, 0, 32);
        return;
    }
    mbedtls_sha256_finish(&c->c, out);
    mbedtls_sha256_free(&c->c);
    free(c);
}

/* ---- the packet ciphers ---- */

typedef struct {
    mbedtls_gcm_context c;
    uint8_t iv[12];
} gcm_t;

struct ssh_cipher {
    bool chacha;
    gcm_t g;
    ssh_chachapoly_t cp;
};

int ssh_cipher_key_len(const char *name)
{
    if (!strcmp(name, "aes128-gcm@openssh.com")) return 16;
    if (!strcmp(name, "aes256-gcm@openssh.com")) return 32;
    if (!strcmp(name, "chacha20-poly1305@openssh.com")) return 64;
    return 0;
}

int ssh_cipher_iv_len(const char *name) { return strstr(name, "-gcm@") ? 12 : 0; }

ssh_cipher_t *ssh_cipher_new(const char *name, const uint8_t *key, const uint8_t *iv)
{
    int kl = ssh_cipher_key_len(name);
    if (!kl) return NULL;
    ssh_cipher_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->chacha = kl == 64;
    if (c->chacha) {
        ssh_chachapoly_init(&c->cp, key);
        return c;
    }
    mbedtls_gcm_init(&c->g.c);
    if (mbedtls_gcm_setkey(&c->g.c, MBEDTLS_CIPHER_ID_AES, key, (unsigned)(kl * 8))) {
        ssh_cipher_free(c);
        return NULL;
    }
    memcpy(c->g.iv, iv, 12);
    return c;
}

void ssh_cipher_free(ssh_cipher_t *c)
{
    if (!c) return;
    if (c->chacha) ssh_chachapoly_wipe(&c->cp);
    else mbedtls_gcm_free(&c->g.c);
    memset(c, 0, sizeof *c);
    free(c);
}

int ssh_cipher_block(const ssh_cipher_t *c) { return c->chacha ? 8 : 16; }

uint32_t ssh_cipher_length(const ssh_cipher_t *c, const uint8_t buf[4], uint32_t seq)
{
    if (c->chacha) return ssh_chachapoly_length(&c->cp, buf, seq);
    return (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 | (uint32_t)buf[2] << 8 | buf[3];
}

/* the invocation counter: the IV's last 8 bytes, big-endian, plus one */
static void iv_next(uint8_t iv[12])
{
    for (int i = 11; i >= 4; i--)
        if (++iv[i]) break;
}

bool ssh_cipher_seal(ssh_cipher_t *c, uint8_t *buf, size_t len, uint32_t seq)
{
    if (c->chacha) {
        ssh_chachapoly_seal(&c->cp, buf, len, seq);
        return true;
    }
    bool ok = !mbedtls_gcm_crypt_and_tag(&c->g.c, MBEDTLS_GCM_ENCRYPT, len, c->g.iv, 12, buf, 4, buf + 4, buf + 4,
                                         16, buf + 4 + len);
    iv_next(c->g.iv);
    return ok;
}

bool ssh_cipher_open(ssh_cipher_t *c, uint8_t *buf, size_t len, uint32_t seq)
{
    if (c->chacha) return ssh_chachapoly_open(&c->cp, buf, len, seq);
    bool ok = !mbedtls_gcm_auth_decrypt(&c->g.c, len, c->g.iv, 12, buf, 4, buf + 4 + len, 16, buf + 4, buf + 4);
    iv_next(c->g.iv);
    return ok;
}

/* ---- X25519 and the host key ---- */

void ssh_x25519_keypair(uint8_t sk[32], uint8_t pk[32])
{
    ssh_random(sk, 32);
    crypto_x25519_public_key(pk, sk);
}

bool ssh_x25519(uint8_t shared[32], const uint8_t sk[32], const uint8_t peer[32])
{
    crypto_x25519(shared, sk, peer);
    /* all zeros: the client sent a point of small order (RFC 8731, 3) */
    uint8_t acc = 0;
    for (int i = 0; i < 32; i++) acc |= shared[i];
    return acc != 0;
}

void ssh_host_sign(wbuf_t *b, const uint8_t seed[32], const uint8_t pub[32], const uint8_t *msg, size_t n)
{
    uint8_t s[32], sk[64], pk[32], sig[64];
    memcpy(s, seed, 32);
    crypto_ed25519_key_pair(sk, pk, s);     /* wipes s */
    (void)pub;
    crypto_ed25519_sign(sig, sk, msg, n);
    crypto_wipe(sk, sizeof sk);
    wb_u32(b, 4 + 11 + 4 + 64);
    wb_cstr(b, "ssh-ed25519");
    wb_string(b, sig, 64);
}

/* ---- the client's keys ---- */

const char SSH_SIG_ALGS[] = "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256";

static bool name_is(const uint8_t *p, size_t n, const char *s) { return p && n == strlen(s) && !memcmp(p, s, n); }

typedef struct {
    int kind;                       /* 1 ed25519, 2 ecdsa p-256, 3 rsa */
    const uint8_t *a; size_t an;    /* ed25519: the key; ecdsa: Q; rsa: e */
    const uint8_t *b; size_t bn;    /* rsa: n */
} ckey_t;

static bool key_parse(const uint8_t *blob, size_t n, ckey_t *k)
{
    rbuf_t r = { blob, n, 0, false };
    size_t tl;
    const uint8_t *t = rb_string(&r, &tl);
    memset(k, 0, sizeof *k);
    if (name_is(t, tl, "ssh-ed25519")) {
        k->kind = 1;
        k->a = rb_string(&r, &k->an);
        return !r.bad && r.off == n && k->an == 32;
    }
    if (name_is(t, tl, "ecdsa-sha2-nistp256")) {
        size_t cl;
        const uint8_t *c = rb_string(&r, &cl);
        k->kind = 2;
        k->a = rb_string(&r, &k->an);
        return !r.bad && r.off == n && name_is(c, cl, "nistp256") && k->an == 65 && k->a[0] == 4;
    }
    if (name_is(t, tl, "ssh-rsa")) {
        k->kind = 3;
        k->a = rb_mpint(&r, &k->an);
        k->b = rb_mpint(&r, &k->bn);
        /* 2048 to 8192 bits: under that is breakable, over that is not a key anyone uses */
        return !r.bad && r.off == n && k->an && k->an <= 8 && k->bn >= 256 && k->bn <= 1024;
    }
    return false;
}

bool ssh_key_describe(const uint8_t *blob, size_t n, char *out, size_t on)
{
    ckey_t k;
    if (!key_parse(blob, n, &k)) return false;
    if (k.kind == 1) snprintf(out, on, "ED25519");
    else if (k.kind == 2) snprintf(out, on, "ECDSA P-256");
    else {
        int bits = (int)k.bn * 8;
        for (uint8_t top = k.b[0]; !(top & 0x80); top <<= 1) bits--;
        snprintf(out, on, "RSA %d", bits);
    }
    return true;
}

static bool verify_ecdsa(const ckey_t *k, const uint8_t *s, size_t sn, const uint8_t *msg, size_t mn)
{
    rbuf_t r = { s, sn, 0, false };
    size_t rl, sl;
    const uint8_t *rv = rb_mpint(&r, &rl), *sv = rb_mpint(&r, &sl);
    if (r.bad || r.off != sn || !rl || !sl || rl > 32 || sl > 32) return false;
    uint8_t h[32];
    mbedtls_sha256(msg, mn, h, 0);
    mbedtls_ecp_group g;
    mbedtls_ecp_point q;
    mbedtls_mpi mr, ms;
    mbedtls_ecp_group_init(&g);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&mr);
    mbedtls_mpi_init(&ms);
    bool ok = !mbedtls_ecp_group_load(&g, MBEDTLS_ECP_DP_SECP256R1) &&
              !mbedtls_ecp_point_read_binary(&g, &q, k->a, k->an) && !mbedtls_ecp_check_pubkey(&g, &q) &&
              !mbedtls_mpi_read_binary(&mr, rv, rl) && !mbedtls_mpi_read_binary(&ms, sv, sl) &&
              !mbedtls_ecdsa_verify(&g, h, 32, &q, &mr, &ms);
    mbedtls_mpi_free(&mr);
    mbedtls_mpi_free(&ms);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&g);
    return ok;
}

static bool verify_rsa(const ckey_t *k, bool sha512, const uint8_t *s, size_t sn, const uint8_t *msg, size_t mn)
{
    if (sn > k->bn) return false;
    uint8_t h[64];
    if (sha512) mbedtls_sha512(msg, mn, h, 0);
    else mbedtls_sha256(msg, mn, h, 0);
    /* a signature may come without its leading zeros (RFC 8332, 3): pad it
     * to the modulus' length, which is what mbedTLS reads */
    uint8_t *full = calloc(1, k->bn);
    if (!full) return false;
    memcpy(full + k->bn - sn, s, sn);
    mbedtls_rsa_context rsa;
    mbedtls_rsa_init(&rsa);
    bool ok = !mbedtls_rsa_import_raw(&rsa, k->b, k->bn, NULL, 0, NULL, 0, NULL, 0, k->a, k->an) &&
              !mbedtls_rsa_complete(&rsa) && !mbedtls_rsa_check_pubkey(&rsa) &&
              mbedtls_rsa_get_len(&rsa) == k->bn &&
              !mbedtls_rsa_pkcs1_verify(&rsa, sha512 ? MBEDTLS_MD_SHA512 : MBEDTLS_MD_SHA256, sha512 ? 64 : 32, h, full);
    mbedtls_rsa_free(&rsa);
    free(full);
    return ok;
}

bool ssh_key_verify(const char *alg, const uint8_t *blob, size_t bn, const uint8_t *sig, size_t sn,
                    const uint8_t *msg, size_t mn)
{
    ckey_t k;
    if (!key_parse(blob, bn, &k)) return false;
    rbuf_t r = { sig, sn, 0, false };
    size_t nl, sl;
    const uint8_t *name = rb_string(&r, &nl), *s = rb_string(&r, &sl);
    if (r.bad || r.off != sn || !name_is(name, nl, alg)) return false;
    if (k.kind == 1 && !strcmp(alg, "ssh-ed25519"))
        return sl == 64 && crypto_ed25519_check(s, k.a, msg, mn) == 0;
    if (k.kind == 2 && !strcmp(alg, "ecdsa-sha2-nistp256")) return verify_ecdsa(&k, s, sl, msg, mn);
    if (k.kind == 3 && !strcmp(alg, "rsa-sha2-256")) return verify_rsa(&k, false, s, sl, msg, mn);
    if (k.kind == 3 && !strcmp(alg, "rsa-sha2-512")) return verify_rsa(&k, true, s, sl, msg, mn);
    return false;
}
