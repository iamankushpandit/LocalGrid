/*
 * The quick boot check (decision D24).
 *
 * Every check is cheap and allocation-free: no simulated grid, no PBKDF2, nothing that
 * needs a big contiguous block. Known-answer vectors are the same ones the full suite uses:
 * ChaCha20-Poly1305 from RFC 8439 section 2.8.2, X25519 from RFC 7748 section 6.1, and
 * HKDF-SHA256 from RFC 5869 appendix A.1.
 */
#include "lg_selftest.h"

#include <inttypes.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "lg_body.h"
#include "lg_crypto.h"
#include "lg_dedup.h"
#include "lg_envelope.h"

static lg_selftest_result_t s_result;
static bool                 s_ran;
static char                 s_summary[72] = "not run";

static void check(bool ok, const char *name)
{
    s_result.checks++;
    if (!ok) {
        s_result.failures++;
        if (s_result.first_failure[0] == '\0') {
            snprintf(s_result.first_failure, sizeof(s_result.first_failure), "%s", name);
        }
    }
}

/* ---- protocol core ---- */

static void test_envelope(void)
{
    lg_env_t e = {
        .type = LG_T_TEXT,
        .scope = LG_SCOPE_DIRECT,
        .flags = LG_FLAG_E2E_PAYLOAD,
        .ttl = LG_TTL_DEFAULT,
        .origin_id = 2,
        .origin_boot = 7,
        .origin_seq = 9,
        .origin_node = 1,
        .target = 3,
        .grid_time = 1790000000u,
    };
    const uint8_t body[5] = { 1, 2, 3, 4, 5 };
    uint8_t frame[LG_ENV_SIZE + sizeof(body)];
    int len = lg_frame_build(&e, body, sizeof(body), frame, sizeof(frame));
    check(len == (int)(LG_ENV_SIZE + sizeof(body)), "envelope build");

    lg_env_t back;
    check(lg_env_decode(frame, (size_t)len, &back) == LG_OK, "envelope decode");
    check(back.type == LG_T_TEXT && back.scope == LG_SCOPE_DIRECT, "envelope type");
    check(back.origin_id == 2 && back.origin_boot == 7 && back.origin_seq == 9, "envelope id");
    check(back.target == 3 && back.grid_time == 1790000000u, "envelope target");
    check(back.body_len == sizeof(body), "envelope body length");
    check(memcmp(lg_frame_body(frame), body, sizeof(body)) == 0, "envelope body");
    check(lg_env_decode(frame, LG_ENV_SIZE - 1u, &back) != LG_OK, "short frame refused");
}

static void test_dedup(void)
{
    lg_dedup_entry_t slots[2];
    lg_dedup_t d;
    lg_dedup_init(&d, slots, 2);
    check(lg_dedup_check(&d, 5, 1, 10) == LG_DEDUP_NEW, "dedup new");
    check(lg_dedup_mark(&d, 5, 1, 10) == LG_DEDUP_NEW, "dedup mark");
    check(lg_dedup_check(&d, 5, 1, 10) == LG_DEDUP_DUPLICATE, "dedup duplicate");
    check(lg_dedup_check(&d, 5, 1, 11) == LG_DEDUP_NEW, "dedup next");
}

static void test_bodies(void)
{
    lg_register_t reg = { .device = 2, .attach_epoch = 0x1002u, .client_time = 1790000000u, .caps = 0 };
    memset(reg.pubkey, 0xA5, sizeof(reg.pubkey));
    uint8_t buf[LG_REGISTER_LEN];
    check(lg_register_enc(&reg, buf) == LG_REGISTER_LEN, "register encode");
    lg_register_t reg_back;
    check(lg_register_dec(buf, sizeof(buf), &reg_back), "register decode");
    check(reg_back.device == 2 && reg_back.attach_epoch == 0x1002u, "register fields");
    check(memcmp(reg_back.pubkey, reg.pubkey, LG_PUBKEY_LEN) == 0, "register key");
    check(!lg_register_dec(buf, sizeof(buf) - 1u, &reg_back), "register length check");

    lg_presence_t pres = { .device = 3, .node = 1, .epoch = 9, .state = LG_PRES_ONLINE };
    uint8_t pbuf[LG_PRESENCE_LEN];
    check(lg_presence_enc(&pres, pbuf) == LG_PRESENCE_LEN, "presence encode");
    lg_presence_t pres_back;
    check(lg_presence_dec(pbuf, sizeof(pbuf), &pres_back), "presence decode");
    check(pres_back.device == 3 && pres_back.node == 1 && pres_back.state == LG_PRES_ONLINE, "presence fields");

    lg_msg_ack_t ack = { .author = 2, .boot = 1, .seq = 4, .status = LG_ACK_DELIVERED };
    uint8_t abuf[LG_MSG_ACK_LEN];
    check(lg_msg_ack_enc(&ack, abuf) == LG_MSG_ACK_LEN, "ack encode");
    lg_msg_ack_t ack_back;
    check(lg_msg_ack_dec(abuf, sizeof(abuf), &ack_back), "ack decode");
    check(ack_back.seq == 4 && ack_back.status == LG_ACK_DELIVERED, "ack fields");
}

static void test_text_rules(void)
{
    const uint8_t good[] = { 'H', 'i', 0xE2, 0x98, 0x95 };            /* "Hi" and a hot drink */
    const uint8_t overlong[] = { 0xC0, 0xAF };                        /* overlong slash */
    const uint8_t surrogate[] = { 0xED, 0xA0, 0x80 };                 /* UTF-16 surrogate */
    check(lg_text_valid(good, sizeof(good)), "utf8 accepted");
    check(!lg_text_valid(overlong, sizeof(overlong)), "utf8 overlong refused");
    check(!lg_text_valid(surrogate, sizeof(surrogate)), "utf8 surrogate refused");
    check(!lg_text_valid((const uint8_t *)"", 0), "empty text refused");
}

/* ---- crypto known answers ---- */

static const uint8_t AEAD_KEY[32] = {
    0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e, 0x8f,
    0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b, 0x9c, 0x9d, 0x9e, 0x9f,
};
static const uint8_t AEAD_NONCE[12] = { 0x07, 0x00, 0x00, 0x00, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47 };
static const uint8_t AEAD_AAD[12] = { 0x50, 0x51, 0x52, 0x53, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7 };
static const char AEAD_PT[] =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
static const uint8_t AEAD_TAG[16] = {
    0x1a, 0xe1, 0x0b, 0x59, 0x4f, 0x09, 0xe2, 0x6a, 0x7e, 0x90, 0x2e, 0xcb, 0xd0, 0x60, 0x06, 0x91,
};
/* The first 16 bytes of the RFC's 114-byte ciphertext: a quick catch for a wrong cipher or a
 * wrong counter start. The tag below is Poly1305 over the AAD and all 114 ciphertext bytes, so
 * it already proves every one of them; no more of the vector is carried here. */
static const uint8_t AEAD_CT_HEAD[16] = {
    0xd3, 0x1a, 0x8d, 0x34, 0x64, 0x8e, 0x60, 0xdb, 0x7b, 0x86, 0xaf, 0xbc, 0x53, 0xef, 0x7e, 0xc2,
};

static void test_aead(void)
{
    size_t pt_len = strlen(AEAD_PT);
    static uint8_t ct[114 + LG_AEAD_TAG_BYTES];
    static uint8_t back[114];
    check(pt_len == 114, "aead plaintext length");
    int n = lg_aead_seal(AEAD_KEY, AEAD_NONCE, AEAD_AAD, sizeof(AEAD_AAD), (const uint8_t *)AEAD_PT, pt_len, ct);
    check(n == (int)(pt_len + LG_AEAD_TAG_BYTES), "aead seal length");
    check(memcmp(ct, AEAD_CT_HEAD, sizeof(AEAD_CT_HEAD)) == 0, "aead ciphertext start");
    check(memcmp(ct + 114, AEAD_TAG, sizeof(AEAD_TAG)) == 0, "aead tag");
    check(lg_aead_open(AEAD_KEY, AEAD_NONCE, AEAD_AAD, sizeof(AEAD_AAD), ct, (size_t)n, back) == 114, "aead open");
    check(memcmp(back, AEAD_PT, 114) == 0, "aead plaintext");
    ct[n - 1] ^= 0x01;
    check(lg_aead_open(AEAD_KEY, AEAD_NONCE, AEAD_AAD, sizeof(AEAD_AAD), ct, (size_t)n, back) < 0, "aead tamper refused");
}

static const uint8_t X_A_PRIV[32] = {
    0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d, 0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
    0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a, 0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
static const uint8_t X_A_PUB[32] = {
    0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54, 0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
    0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4, 0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
};
static const uint8_t X_B_PUB[32] = {
    0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4, 0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
    0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d, 0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
};
static const uint8_t X_SHARED[32] = {
    0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1, 0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
    0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33, 0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42,
};

static void test_x25519(void)
{
    uint8_t pub[32];
    uint8_t shared[32];
    check(lg_x25519_public(X_A_PRIV, pub) == 0, "x25519 public");
    check(memcmp(pub, X_A_PUB, 32) == 0, "x25519 public value");
    check(lg_x25519_shared(X_A_PRIV, X_B_PUB, shared) == 0, "x25519 shared");
    check(memcmp(shared, X_SHARED, 32) == 0, "x25519 shared value");
    uint8_t zero[32] = { 0 };
    check(lg_x25519_shared(X_A_PRIV, zero, shared) < 0, "x25519 small order refused");
}

static const uint8_t HKDF_IKM[22] = {
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
    0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b,
};
static const uint8_t HKDF_SALT[13] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c };
static const uint8_t HKDF_INFO[10] = { 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9 };
static const uint8_t HKDF_OKM[16] = {
    0x3c, 0xb2, 0x5f, 0x25, 0xfa, 0xac, 0xd5, 0x7a, 0x90, 0x43, 0x4f, 0x64, 0xd0, 0x36, 0x2f, 0x2a,
};

static void test_hkdf(void)
{
    uint8_t out[16];
    check(lg_hkdf_sha256(HKDF_SALT, sizeof(HKDF_SALT), HKDF_IKM, sizeof(HKDF_IKM), HKDF_INFO, sizeof(HKDF_INFO),
                         out, sizeof(out)) == 0, "hkdf");
    check(memcmp(out, HKDF_OKM, sizeof(out)) == 0, "hkdf value");
}

/* One 1:1 message sealed and opened, the way two handhelds do it. */
static void test_e2e(void)
{
    /* On the heap for the length of the test: kept static they held 1.3 KB for good. */
    lg_e2e_t *a = calloc(1, sizeof(lg_e2e_t));
    lg_e2e_t *b = calloc(1, sizeof(lg_e2e_t));
    if (a == NULL || b == NULL) {
        check(false, "e2e memory");
        free(a);
        free(b);
        return;
    }
#define alice (*a)
#define bob   (*b)
    uint8_t apriv[LG_X25519_LEN];
    uint8_t apub[LG_X25519_LEN];
    uint8_t bpriv[LG_X25519_LEN];
    uint8_t bpub[LG_X25519_LEN];
    if (lg_x25519_keypair(apriv, apub) != 0 || lg_x25519_keypair(bpriv, bpub) != 0) {
        check(false, "e2e keypairs");
        free(a);
        free(b);
        return;
    }
    check(lg_e2e_init(&alice, 1, apriv) == 0, "e2e init sender");
    check(lg_e2e_init(&bob, 2, bpriv) == 0, "e2e init recipient");

    lg_env_t e = { .type = LG_T_TEXT, .scope = LG_SCOPE_DIRECT, .flags = LG_FLAG_E2E_PAYLOAD, .origin_id = 1,
                   .origin_boot = 1, .origin_seq = 1, .target = 2, .grid_time = 1790000000u, .body_len = 5 };
    uint8_t nonce[LG_E2E_NONCE_LEN];
    uint8_t aad[LG_E2E_AAD_LEN];
    lg_e2e_nonce(&e, nonce);
    lg_e2e_aad(&e, aad);
    const uint8_t text[5] = { 'h', 'e', 'l', 'l', 'o' };
    uint8_t sealed[5 + LG_AEAD_TAG_BYTES];
    uint8_t opened[5];
    int n = lg_e2e_seal(&alice, 2, bob.pub, nonce, aad, sizeof(aad), text, sizeof(text), sealed);
    check(n == (int)(sizeof(text) + LG_AEAD_TAG_BYTES), "e2e seal");
    check(lg_e2e_open(&bob, 1, alice.pub, nonce, aad, sizeof(aad), sealed, (size_t)n, opened) == (int)sizeof(text),
          "e2e open");
    check(memcmp(opened, text, sizeof(text)) == 0, "e2e plaintext");
    aad[0] ^= 0x01;
    check(lg_e2e_open(&bob, 1, alice.pub, nonce, aad, sizeof(aad), sealed, (size_t)n, opened) < 0,
          "e2e aad tamper refused");
    lg_secure_zero(apriv, sizeof(apriv));
    lg_secure_zero(bpriv, sizeof(bpriv));
#undef alice
#undef bob
    lg_secure_zero(a, sizeof(*a));
    lg_secure_zero(b, sizeof(*b));
    free(a);
    free(b);
}

const lg_selftest_result_t *lg_selftest_quick(void)
{
    memset(&s_result, 0, sizeof(s_result));
    int64_t started = esp_timer_get_time();
    test_envelope();
    test_dedup();
    test_bodies();
    test_text_rules();
    test_hkdf();
    test_aead();
    test_x25519();
    test_e2e();
    s_result.ms = (uint32_t)((esp_timer_get_time() - started) / 1000);
    s_ran = true;
    if (s_result.failures == 0) {
        snprintf(s_summary, sizeof(s_summary), "%u checks passed in %" PRIu32 " ms", s_result.checks, s_result.ms);
    } else {
        snprintf(s_summary, sizeof(s_summary), "%u of %u failed: %s", s_result.failures, s_result.checks,
                 s_result.first_failure);
    }
    return &s_result;
}

const lg_selftest_result_t *lg_selftest_last(void)
{
    return s_ran ? &s_result : NULL;
}

const char *lg_selftest_summary(void)
{
    return s_summary;
}
