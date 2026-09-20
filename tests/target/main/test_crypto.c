#include <string.h>

#include "esp_timer.h"
#include "lg_crypto.h"
#include "lg_test.h"

/* RFC 8439 section 2.8.2 */
static void test_chachapoly_rfc8439(void)
{
    static const char *pt_str =
        "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    uint8_t key[32], nonce[12], aad[12], expect_ct[114], expect_tag[16];
    CHECK_EQ(hex2bin("808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f", key, sizeof(key)), 32);
    CHECK_EQ(hex2bin("070000004041424344454647", nonce, sizeof(nonce)), 12);
    CHECK_EQ(hex2bin("50515253c0c1c2c3c4c5c6c7", aad, sizeof(aad)), 12);
    CHECK_EQ(hex2bin("d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6"
                     "3dbea45e8ca9671282fafb69da92728b1a71de0a9e060b2905d6a5b67ecd3b36"
                     "92ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b4831d7bc"
                     "3ff4def08e4b7a9de576d26586cec64b6116", expect_ct, sizeof(expect_ct)), 114);
    CHECK_EQ(hex2bin("1ae10b594f09e26a7e902ecbd0600691", expect_tag, sizeof(expect_tag)), 16);

    size_t pt_len = strlen(pt_str);
    CHECK_EQ(pt_len, 114);
    uint8_t ct[114 + 16];
    int n = lg_aead_seal(key, nonce, aad, sizeof(aad), (const uint8_t *)pt_str, pt_len, ct);
    CHECK_EQ(n, 130);
    CHECK(memcmp(ct, expect_ct, 114) == 0);
    CHECK(memcmp(ct + 114, expect_tag, 16) == 0);

    uint8_t back[114];
    CHECK_EQ(lg_aead_open(key, nonce, aad, sizeof(aad), ct, 130, back), 114);
    CHECK(memcmp(back, pt_str, 114) == 0);

    ct[129] ^= 0x01;   /* tamper with the tag */
    CHECK(lg_aead_open(key, nonce, aad, sizeof(aad), ct, 130, back) < 0);
    ct[129] ^= 0x01;
    aad[0] ^= 0x01;    /* tamper with associated data */
    CHECK(lg_aead_open(key, nonce, aad, sizeof(aad), ct, 130, back) < 0);
}

/* RFC 7748 section 6.1 */
static void test_x25519_rfc7748(void)
{
    uint8_t a[32], a_pub_expect[32], b[32], b_pub_expect[32], k_expect[32];
    hex2bin("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a", a, 32);
    hex2bin("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", a_pub_expect, 32);
    hex2bin("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb", b, 32);
    hex2bin("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", b_pub_expect, 32);
    hex2bin("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742", k_expect, 32);

    uint8_t a_pub[32], b_pub[32], k1[32], k2[32];
    CHECK_EQ(lg_x25519_public(a, a_pub), 0);
    CHECK(memcmp(a_pub, a_pub_expect, 32) == 0);
    CHECK_EQ(lg_x25519_public(b, b_pub), 0);
    CHECK(memcmp(b_pub, b_pub_expect, 32) == 0);
    CHECK_EQ(lg_x25519_shared(a, b_pub_expect, k1), 0);
    CHECK(memcmp(k1, k_expect, 32) == 0);
    CHECK_EQ(lg_x25519_shared(b, a_pub_expect, k2), 0);
    CHECK(memcmp(k2, k_expect, 32) == 0);

    uint8_t zero_pub[32] = { 0 };   /* small-order point gives an all-zero secret */
    CHECK(lg_x25519_shared(a, zero_pub, k1) < 0);

    uint8_t priv[32], pub[32], pub2[32];
    CHECK_EQ(lg_x25519_keypair(priv, pub), 0);
    CHECK_EQ(priv[0] & 7, 0);
    CHECK_EQ(priv[31] & 0xC0, 0x40);
    CHECK_EQ(lg_x25519_public(priv, pub2), 0);
    CHECK(memcmp(pub, pub2, 32) == 0);
}

/* RFC 5869 appendix A.1 */
static void test_hkdf_rfc5869(void)
{
    uint8_t ikm[22], salt[13], info[10], expect[42], out[42];
    hex2bin("0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", ikm, sizeof(ikm));
    hex2bin("000102030405060708090a0b0c", salt, sizeof(salt));
    hex2bin("f0f1f2f3f4f5f6f7f8f9", info, sizeof(info));
    hex2bin("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
            expect, sizeof(expect));
    CHECK_EQ(lg_hkdf_sha256(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), out, sizeof(out)), 0);
    CHECK(memcmp(out, expect, sizeof(out)) == 0);
}

/* RFC 7914 section 11, plus a timing measurement for the admin password cost. */
static void test_pbkdf2_rfc7914(void)
{
    uint8_t expect[64], out[64];
    hex2bin("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
            "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783", expect, sizeof(expect));
    CHECK_EQ(lg_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, out, sizeof(out)), 0);
    CHECK(memcmp(out, expect, sizeof(out)) == 0);

    hex2bin("4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56"
            "a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d", expect, sizeof(expect));
    int64_t t0 = esp_timer_get_time();
    CHECK_EQ(lg_pbkdf2_sha256((const uint8_t *)"Password", 8, (const uint8_t *)"NaCl", 4, 80000, out, sizeof(out)), 0);
    int64_t t1 = esp_timer_get_time();
    CHECK(memcmp(out, expect, sizeof(out)) == 0);
    printf("pbkdf2: 80000 iterations x 2 blocks took %lld ms (%lld ms per 1000 iterations per block)\n",
           (long long)((t1 - t0) / 1000), (long long)((t1 - t0) / 160000));

    uint8_t salt[16] = { 0 }, hash[32];
    t0 = esp_timer_get_time();
    CHECK_EQ(lg_pbkdf2_sha256((const uint8_t *)"campfire-password", 17, salt, sizeof(salt), 8000, hash, sizeof(hash)), 0);
    t1 = esp_timer_get_time();
    printf("pbkdf2: admin login cost, 8000 iterations, %lld ms\n", (long long)((t1 - t0) / 1000));

    CHECK(lg_pbkdf2_sha256((const uint8_t *)"", 0, salt, sizeof(salt), 1, hash, sizeof(hash)) < 0);
    CHECK(lg_ct_equal((const uint8_t *)"abcd", (const uint8_t *)"abcd", 4));
    CHECK(!lg_ct_equal((const uint8_t *)"abcd", (const uint8_t *)"abce", 4));
}

/* RFC 4231 test case 2, and the admin link's login proof and chunk sealing (D70). */
static void test_hmac_and_ble_link(void)
{
    uint8_t out[32], expect[32];
    hex2bin("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", expect, sizeof(expect));
    CHECK_EQ(lg_hmac_sha256((const uint8_t *)"Jefe", 4, (const uint8_t *)"what do ya want for nothing?", 28, out), 0);
    CHECK(memcmp(out, expect, sizeof(out)) == 0);

    /* The login proof: HMAC-SHA256(key = the stored PBKDF2 hash, challenge || "lg-ble-admin").
     * The password never crosses the link, and a proof for another challenge does not verify. */
    uint8_t hash[32], challenge[32], msg[32 + 12], proof[32], other[32];
    for (size_t i = 0; i < 32; i++) {
        hash[i] = (uint8_t)(0xA0 + i);
        challenge[i] = (uint8_t)(i * 7u);
    }
    memcpy(msg, challenge, 32);
    memcpy(msg + 32, "lg-ble-admin", 12);
    CHECK_EQ(lg_hmac_sha256(hash, sizeof(hash), msg, sizeof(msg), proof), 0);
    CHECK(lg_ct_equal(proof, proof, 32));
    msg[0] ^= 0x01;   /* a different challenge */
    CHECK_EQ(lg_hmac_sha256(hash, sizeof(hash), msg, sizeof(msg), other), 0);
    CHECK(!lg_ct_equal(proof, other, 32));

    /* One sealed chunk: K_link from the beacon key, the 4-byte header as AAD, and the
     * (direction, session, counter) nonce. The same counter in the other direction is a
     * different nonce, and a chunk cannot be replayed as the next counter. */
    uint8_t backbone[32], klink[32];
    memset(backbone, 0x5A, sizeof(backbone));
    CHECK_EQ(lg_hkdf_sha256((const uint8_t *)"LG-BLE-LINK-1", 13, backbone, 32,
                            (const uint8_t *)"admin link", 10, klink, sizeof(klink)), 0);
    uint8_t hdr[4] = { 0x83, 0x01, 0x40, 0x00 };   /* STATUS, more follows, 64 bytes */
    uint8_t body[64], sealed[64 + 16], back[64];
    memset(body, 0x42, sizeof(body));
    uint8_t nonce[12] = { 0 }, nonce_in[12] = { 0 }, nonce_next[12] = { 0 };
    nonce[0] = 1; nonce[1] = 0x34; nonce[2] = 0x12; nonce[4] = 7;   /* AP to client, session 0x1234, counter 7 */
    memcpy(nonce_in, nonce, 12);  nonce_in[0] = 0;
    memcpy(nonce_next, nonce, 12); nonce_next[4] = 8;
    CHECK_EQ(lg_aead_seal(klink, nonce, hdr, sizeof(hdr), body, sizeof(body), sealed), 64 + 16);
    CHECK_EQ(lg_aead_open(klink, nonce, hdr, sizeof(hdr), sealed, sizeof(sealed), back), 64);
    CHECK(memcmp(back, body, sizeof(body)) == 0);
    CHECK(lg_aead_open(klink, nonce_in, hdr, sizeof(hdr), sealed, sizeof(sealed), back) < 0);
    CHECK(lg_aead_open(klink, nonce_next, hdr, sizeof(hdr), sealed, sizeof(sealed), back) < 0);
    hdr[1] = 0;   /* the "more follows" flag is covered by the tag */
    CHECK(lg_aead_open(klink, nonce, hdr, sizeof(hdr), sealed, sizeof(sealed), back) < 0);
}

/* Pairwise keys agree in both directions and differ for a third party. */
static void test_e2e_pairs(void)
{
    uint8_t p1[32], q1[32], p2[32], q2[32], p3[32], q3[32];
    CHECK_EQ(lg_x25519_keypair(p1, q1), 0);
    CHECK_EQ(lg_x25519_keypair(p2, q2), 0);
    CHECK_EQ(lg_x25519_keypair(p3, q3), 0);

    static lg_e2e_t dad, emma, ranger;
    CHECK_EQ(lg_e2e_init(&dad, 1, p1), 0);
    CHECK_EQ(lg_e2e_init(&emma, 2, p2), 0);
    CHECK_EQ(lg_e2e_init(&ranger, 4, p3), 0);

    const uint8_t nonce[12] = { 1, 0, 0, 0, 1, 0, 0, 0, 5, 0, 0, 0 };
    const uint8_t aad[4] = { 9, 9, 9, 9 };
    const char *msg = "At the fire.";
    uint8_t ct[64], pt[64];
    int n = lg_e2e_seal(&dad, 2, emma.pub, nonce, aad, sizeof(aad), (const uint8_t *)msg, strlen(msg), ct);
    CHECK_EQ(n, (int)strlen(msg) + 16);
    CHECK_EQ(lg_e2e_open(&emma, 1, dad.pub, nonce, aad, sizeof(aad), ct, (size_t)n, pt), (int)strlen(msg));
    CHECK(memcmp(pt, msg, strlen(msg)) == 0);
    CHECK(lg_e2e_open(&ranger, 1, dad.pub, nonce, aad, sizeof(aad), ct, (size_t)n, pt) < 0);
}

void test_crypto(void)
{
    test_chachapoly_rfc8439();
    test_x25519_rfc7748();
    test_hkdf_rfc5869();
    test_pbkdf2_rfc7914();
    test_hmac_and_ble_link();
    test_e2e_pairs();
}
