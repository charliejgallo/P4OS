/*
 * P4OS - Mensajes: the protocol's pieces against vectors.h (tests/vectors.py),
 * on the computer.
 *
 *     make -C apps/mensajes/tests test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mj_crypto.h"
#include "mj_port.h"
#include "mj_tl.h"
#include "vectors.h"

static int s_fail, s_pass;

static void check(const char *what, bool ok)
{
    if (ok) s_pass++;
    else s_fail++;
    printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
}

static bool eq(const void *a, const void *b, size_t n)
{
    return memcmp(a, b, n) == 0;
}

static uint32_t ms(void) { return mj_now_ms(); }

static void test_hashes_and_aes(void)
{
    uint8_t h[20];
    mj_sha1("abc", 3, h);
    check("SHA-1 of abc (FIPS 180)", eq(h, V_SHA1_ABC, 20));

    mj_aes_t a;
    uint8_t o[16], p[16];
    mj_aes_setkey(&a, V_AES_KEY);
    mj_aes_encrypt(&a, V_AES_PT, o);
    check("AES-256 encrypts (FIPS 197 C.3)", eq(o, V_AES_CT, 16));
    mj_aes_decrypt(&a, V_AES_CT, p);
    check("AES-256 decrypts (FIPS 197 C.3)", eq(p, V_AES_PT, 16));

    uint8_t buf[sizeof V_IGE_PT];
    memcpy(buf, V_IGE_PT, sizeof buf);
    mj_ige_encrypt(V_IGE_KEY, V_IGE_IV, buf, sizeof buf);
    check("AES-256-IGE encrypts", eq(buf, V_IGE_CT, sizeof buf));
    mj_ige_decrypt(V_IGE_KEY, V_IGE_IV, buf, sizeof buf);
    check("AES-256-IGE decrypts", eq(buf, V_IGE_PT, sizeof buf));
}

/* the same as mj_mtp.c's seal, written out again from the description */
static void seal(const uint8_t *ak, const uint8_t *pt, size_t n, int x, uint8_t *out)
{
    uint8_t big[32], a[32], b[32], key[32], iv[32];
    mj_sha256_2(ak + 88 + x, 32, pt, n, big);
    const uint8_t *msg_key = big + 8;
    mj_sha256_2(msg_key, 16, ak + x, 36, a);
    mj_sha256_2(ak + 40 + x, 36, msg_key, 16, b);
    memcpy(key, a, 8);
    memcpy(key + 8, b + 8, 16);
    memcpy(key + 24, a + 24, 8);
    memcpy(iv, b, 8);
    memcpy(iv + 8, a + 8, 16);
    memcpy(iv + 24, b + 24, 8);
    uint8_t h[20];
    mj_sha1(ak, 256, h);
    memcpy(out, h + 12, 8);
    memcpy(out + 8, msg_key, 16);
    memcpy(out + 24, pt, n);
    mj_ige_encrypt(key, iv, out + 24, n);
}

static void test_mtproto_keys(void)
{
    uint8_t h[20];
    mj_sha1(V_AK, 256, h);
    check("auth_key_id is SHA-1's last 8 bytes", eq(h + 12, V_AK_ID, 8));
    uint8_t o[24 + sizeof V_SEAL_PT];
    seal(V_AK, V_SEAL_PT, sizeof V_SEAL_PT, 0, o);
    check("msg_key and AES key/iv, client to server", eq(o, V_SEAL_C2S, sizeof o));
    seal(V_AK, V_SEAL_PT, sizeof V_SEAL_PT, 8, o);
    check("msg_key and AES key/iv, server to client", eq(o, V_SEAL_S2C, sizeof o));

    /* the handshake's temporary key, as mj_mtp.c derives it */
    uint8_t s1[20], s2[20], s3[20], key[32], iv[32];
    mj_sha1_2(V_NEW_NONCE, 32, V_SERVER_NONCE, 16, s1);
    mj_sha1_2(V_SERVER_NONCE, 16, V_NEW_NONCE, 32, s2);
    mj_sha1_2(V_NEW_NONCE, 32, V_NEW_NONCE, 32, s3);
    memcpy(key, s1, 20);
    memcpy(key + 20, s2, 12);
    memcpy(iv, s2 + 12, 8);
    memcpy(iv + 8, s3, 20);
    memcpy(iv + 28, V_NEW_NONCE, 4);
    check("tmp_aes_key", eq(key, V_TMP_KEY, 32));
    check("tmp_aes_iv", eq(iv, V_TMP_IV, 32));
    uint8_t nnh[41];
    memcpy(nnh, V_NEW_NONCE, 32);
    nnh[32] = 1;
    mj_sha1(V_AK, 256, h);
    memcpy(nnh + 33, h, 8);
    mj_sha1(nnh, 41, h);
    check("new_nonce_hash1", eq(h + 4, V_NNH1, 16));
}

static void test_rsa_dh_pq(void)
{
    /* RSA_PAD by hand from the fixed parts, then the modexp */
    uint8_t dwp[192], dwh[224], kae[256], out[256], e[3] = {1, 0, 1};
    memcpy(dwp, V_RSA_DATA, sizeof V_RSA_DATA);
    memcpy(dwp + sizeof V_RSA_DATA, V_RSA_PAD, sizeof V_RSA_PAD);
    for (int i = 0; i < 192; i++) dwh[i] = dwp[191 - i];
    mj_sha256_2(V_RSA_TK, 32, dwp, 192, dwh + 192);
    uint8_t zero[32] = {0};
    mj_ige_encrypt(V_RSA_TK, zero, dwh, 224);
    uint8_t hh[32];
    mj_sha256(dwh, 224, hh);
    for (int i = 0; i < 32; i++) kae[i] = V_RSA_TK[i] ^ hh[i];
    memcpy(kae + 32, dwh, 224);
    uint32_t t0 = ms();
    bool ok = mj_modexp_be(out, 256, kae, 256, e, 3, V_RSA_N, 256);
    check("RSA_PAD with Telegram's key", ok && eq(out, V_RSA_ENC, 256));
    printf("        (RSA: %u ms)\n", (unsigned)(ms() - t0));

    uint8_t g = 3;
    t0 = ms();
    ok = mj_modexp_be(out, 256, &g, 1, V_DH_B, 256, MJ_DH_PRIME, 256);
    check("DH: g^b mod p", ok && eq(out, V_DH_GB, 256));
    printf("        (2048-bit modexp: %u ms)\n", (unsigned)(ms() - t0));
    ok = mj_modexp_be(out, 256, V_DH_GA, 256, V_DH_B, 256, MJ_DH_PRIME, 256);
    check("DH: the auth key, (g^a)^b mod p", ok && eq(out, V_DH_KEY, 256));
    check("DH checks g_a", mj_dh_check(3, MJ_DH_PRIME, V_DH_GA, 256));
    uint8_t small[256] = {0};
    small[255] = 5;
    check("DH refuses a small g_a", !mj_dh_check(3, MJ_DH_PRIME, small, 256));
    check("DH refuses g = 2 for this prime", !mj_dh_check(2, MJ_DH_PRIME, V_DH_GA, 256));

    uint32_t p = 0, q = 0;
    t0 = ms();
    ok = mj_factor_pq(V_PQ, &p, &q);
    check("pq of core.telegram.org's example", ok && p == V_P && q == V_Q);
    printf("        (pq: %u ms)\n", (unsigned)(ms() - t0));
    ok = mj_factor_pq(0x5fffffffbfffffffull, &p, &q);
    check("another pq splits", ok && p == 2147483647u && q == 3221225473u);
    ok = mj_factor_pq(0xffffffea00000055ull, &p, &q);
    check("a pq near 2^64 splits", ok && p == 4294967279u && q == 4294967291u);
}

static void test_srp(void)
{
    uint8_t k[64];
    mj_pbkdf2_sha512((const uint8_t *)"contrasenia", 11, (const uint8_t *)"sal-de-prueba", 13, 1000, k, 64);
    check("PBKDF2-HMAC-SHA512", eq(k, V_PBKDF2, 64));
    uint8_t A[256], M1[32];
    uint32_t t0 = ms();
    bool ok = mj_srp(V_SRP_PASSWORD, V_SRP_SALT1, sizeof V_SRP_SALT1, V_SRP_SALT2, sizeof V_SRP_SALT2, 3,
                     MJ_DH_PRIME, V_SRP_B, V_SRP_A_RAND, A, M1);
    check("SRP: A", ok && eq(A, V_SRP_A, 256));
    check("SRP: M1", ok && eq(M1, V_SRP_M1, 32));
    printf("        (SRP with 100 000 rounds: %u ms)\n", (unsigned)(ms() - t0));
}

static void test_tl(void)
{
    mj_arena_t a = {0};
    tl_obj_t *m = tl_read_obj(&a, V_TL_MSG, sizeof V_TL_MSG, NULL);
    check("TL: reads a message", m && m->id == TLC_message);
    if (m) {
        check("TL: the id and the out flag", tl_i(m, TLN_id) == 4321 && tl_t(m, TLN_out) && !tl_t(m, TLN_pinned));
        tl_obj_t *peer = tl_o(m, TLN_peer_id);
        check("TL: the peer, a long", peer && peer->id == TLC_peerUser && tl_l(peer, TLN_user_id) == 777000123456LL);
        check("TL: the text, UTF-8", !strcmp(tl_s(m, TLN_message), "hola, ¿qué tal? 👋"));
        tl_obj_t *rt = tl_o(m, TLN_reply_to);
        check("TL: the reply header", rt && tl_i(rt, TLN_reply_to_msg_id) == 4300);
        tl_vec_t *ents = tl_v(m, TLN_entities);
        check("TL: a vector of entities", tl_vn(ents) == 1 && tl_vo(ents, 0)->id == TLC_messageEntityBold);
        check("TL: two fields on one bit", tl_i(m, TLN_views) == 12 && tl_i(m, TLN_forwards) == 3);
        check("TL: absent fields", !tl_has(m, TLN_media) && !tl_has(m, TLN_edit_date));
    }

    mj_buf_t b = {0};
    tl_obj_t *req = tl_new(&a, TLC_messages_getHistory);
    tl_obj_t *peer = tl_new(&a, TLC_inputPeerChannel);
    tl_set_l(peer, TLN_channel_id, 1234567890123LL);
    tl_set_l(peer, TLN_access_hash, -5555555555555LL);
    tl_set_o(req, TLN_peer, peer);
    tl_set_i(req, TLN_offset_id, 0);
    tl_set_i(req, TLN_offset_date, 0);
    tl_set_i(req, TLN_add_offset, 0);
    tl_set_i(req, TLN_limit, 40);
    tl_set_i(req, TLN_max_id, 0);
    tl_set_i(req, TLN_min_id, 0);
    tl_set_l(req, TLN_hash, 0);
    bool ok = tl_write(&b, req);
    check("TL: writes getHistory as Python does", ok && b.n == sizeof V_TL_REQ && eq(b.p, V_TL_REQ, b.n));
    mj_buf_free(&b);

    char text[301];
    memset(text, 'x', 300);
    text[300] = 0;
    tl_obj_t *s = tl_new(&a, TLC_messages_sendMessage);
    tl_set_t(s, TLN_silent);
    tl_set_o(s, TLN_peer, tl_new(&a, TLC_inputPeerSelf));
    tl_set_s(s, TLN_message, text);
    tl_set_l(s, TLN_random_id, 99);
    tl_obj_t *rt = tl_new(&a, TLC_inputReplyToMessage);
    tl_set_i(rt, TLN_reply_to_msg_id, 5);
    tl_set_o(s, TLN_reply_to, rt);
    ok = tl_write(&b, s);
    check("TL: flags worked out, a long string", ok && b.n == sizeof V_TL_SEND && eq(b.p, V_TL_SEND, b.n));
    mj_buf_free(&b);

    tl_obj_t *ng = tl_new(&a, TLC_messages_getHistory);
    check("TL: a required field missing is refused", !tl_write(&b, ng));
    mj_buf_free(&b);

    size_t on = 0;
    uint8_t *g = tl_gunzip(&a, V_GZ_IN, sizeof V_GZ_IN, &on);
    check("gzip opens", g && on == V_GZ_OUT_LEN && !memcmp(g, "Mensajes Mensajes", 17));
    tl_obj_t *e = tl_read_obj(&a, V_TL_GZ, sizeof V_TL_GZ, NULL);
    check("TL: gzip_packed read through", e && e->id == TLC_rpc_error && tl_i(e, TLN_error_code) == 420 &&
                                               !strcmp(tl_s(e, TLN_error_message), "FLOOD_WAIT_7"));
    tl_obj_t *c = m ? tl_clone(&a, m) : NULL;
    check("TL: a clone reads the same", c && !strcmp(tl_s(c, TLN_message), tl_s(m, TLN_message)));
    printf("        (arena: %u bytes used)\n", (unsigned)a.used);
    mj_arena_free(&a);
}

int main(void)
{
    test_hashes_and_aes();
    test_mtproto_keys();
    test_rsa_dh_pq();
    test_srp();
    test_tl();
    printf("%d passed, %d failed; memory left %u bytes (peak %u)\n", s_pass, s_fail, (unsigned)mj_mem_used(),
           (unsigned)mj_mem_peak());
    return s_fail ? 1 : 0;
}
