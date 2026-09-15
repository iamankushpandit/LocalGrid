#include <string.h>

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
    test_e2e_pairs();
}
