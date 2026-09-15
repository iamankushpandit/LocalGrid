#include "lg_crypto.h"

#include <string.h>

#include "psa/crypto.h"

void lg_secure_zero(void *p, size_t n)
{
    volatile unsigned char *v = (volatile unsigned char *)p;
    while (n--) {
        *v++ = 0;
    }
}

int lg_crypto_init(void)
{
    return psa_crypto_init() == PSA_SUCCESS ? 0 : LG_CRYPTO_ERR;
}

int lg_crypto_random(uint8_t *out, size_t len)
{
    return psa_generate_random(out, len) == PSA_SUCCESS ? 0 : LG_CRYPTO_ERR;
}

static psa_status_t import_key(psa_key_type_t type, size_t bits, psa_key_usage_t usage, psa_algorithm_t alg,
                               const uint8_t *data, size_t len, psa_key_id_t *id)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, type);
    psa_set_key_bits(&attr, bits);
    psa_set_key_usage_flags(&attr, usage);
    psa_set_key_algorithm(&attr, alg);
    psa_status_t st = psa_import_key(&attr, data, len, id);
    psa_reset_key_attributes(&attr);
    return st;
}

#define X25519_PAIR_TYPE PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY)

int lg_x25519_public(const uint8_t priv[LG_X25519_LEN], uint8_t pub[LG_X25519_LEN])
{
    psa_key_id_t id = 0;
    if (import_key(X25519_PAIR_TYPE, 255, PSA_KEY_USAGE_DERIVE, PSA_ALG_ECDH, priv, LG_X25519_LEN, &id) != PSA_SUCCESS) {
        return LG_CRYPTO_ERR;
    }
    size_t olen = 0;
    psa_status_t st = psa_export_public_key(id, pub, LG_X25519_LEN, &olen);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && olen == LG_X25519_LEN) ? 0 : LG_CRYPTO_ERR;
}

int lg_x25519_keypair(uint8_t priv[LG_X25519_LEN], uint8_t pub[LG_X25519_LEN])
{
    if (lg_crypto_random(priv, LG_X25519_LEN) != 0) {
        return LG_CRYPTO_ERR;
    }
    /* RFC 7748 section 5 clamping, so the stored key is canonical. */
    priv[0]  &= 248u;
    priv[31] &= 127u;
    priv[31] |= 64u;
    return lg_x25519_public(priv, pub);
}

int lg_x25519_shared(const uint8_t priv[LG_X25519_LEN], const uint8_t peer_pub[LG_X25519_LEN],
                     uint8_t shared[LG_X25519_LEN])
{
    psa_key_id_t id = 0;
    if (import_key(X25519_PAIR_TYPE, 255, PSA_KEY_USAGE_DERIVE, PSA_ALG_ECDH, priv, LG_X25519_LEN, &id) != PSA_SUCCESS) {
        return LG_CRYPTO_ERR;
    }
    size_t olen = 0;
    psa_status_t st = psa_raw_key_agreement(PSA_ALG_ECDH, id, peer_pub, LG_X25519_LEN, shared, LG_X25519_LEN, &olen);
    psa_destroy_key(id);
    if (st != PSA_SUCCESS || olen != LG_X25519_LEN) {
        return LG_CRYPTO_ERR;
    }
    uint8_t acc = 0;
    for (size_t i = 0; i < LG_X25519_LEN; i++) {
        acc |= shared[i];
    }
    if (acc == 0) {
        lg_secure_zero(shared, LG_X25519_LEN);
        return LG_CRYPTO_ERR;
    }
    return 0;
}

static int hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32])
{
    static const uint8_t zeros[32] = { 0 };
    if (key_len == 0) {
        /* HMAC pads keys with zeros to the block size, so an empty key equals 32 zero bytes. */
        key = zeros;
        key_len = sizeof(zeros);
    }
    psa_key_id_t id = 0;
    if (import_key(PSA_KEY_TYPE_HMAC, key_len * 8u, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                   key, key_len, &id) != PSA_SUCCESS) {
        return LG_CRYPTO_ERR;
    }
    size_t olen = 0;
    psa_status_t st = psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), msg, msg_len, out, 32, &olen);
    psa_destroy_key(id);
    return (st == PSA_SUCCESS && olen == 32) ? 0 : LG_CRYPTO_ERR;
}

int lg_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                   const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len)
{
    enum { MAX_INFO = 64 };
    if (out == NULL || out_len == 0 || out_len > 255u * 32u || info_len > MAX_INFO ||
        (salt_len > 0 && salt == NULL) || (info_len > 0 && info == NULL)) {
        return LG_CRYPTO_ERR;
    }
    uint8_t prk[32];
    if (hmac_sha256(salt, salt_len, ikm, ikm_len, prk) != 0) {
        return LG_CRYPTO_ERR;
    }

    uint8_t t[32];
    size_t t_len = 0;
    uint8_t block[32 + MAX_INFO + 1];
    size_t done = 0;
    uint8_t counter = 1;
    int rc = 0;
    while (done < out_len) {
        size_t m = 0;
        memcpy(block, t, t_len);
        m += t_len;
        if (info_len > 0) {
            memcpy(block + m, info, info_len);
            m += info_len;
        }
        block[m++] = counter++;
        if (hmac_sha256(prk, sizeof(prk), block, m, t) != 0) {
            rc = LG_CRYPTO_ERR;
            break;
        }
        t_len = sizeof(t);
        size_t take = (out_len - done) < sizeof(t) ? (out_len - done) : sizeof(t);
        memcpy(out + done, t, take);
        done += take;
    }
    lg_secure_zero(prk, sizeof(prk));
    lg_secure_zero(t, sizeof(t));
    lg_secure_zero(block, sizeof(block));
    return rc;
}

int lg_pbkdf2_sha256(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t *out, size_t out_len)
{
    enum { MAX_SALT = 64, MAX_OUT = 64 };
    if (password == NULL || password_len == 0 || password_len > 128 || (salt_len > 0 && salt == NULL) ||
        salt_len > MAX_SALT || iterations == 0 || out == NULL || out_len == 0 || out_len > MAX_OUT) {
        return LG_CRYPTO_ERR;
    }
    /* Import the password once as the HMAC key; every iteration reuses the key handle. */
    psa_key_id_t id = 0;
    if (import_key(PSA_KEY_TYPE_HMAC, password_len * 8u, PSA_KEY_USAGE_SIGN_MESSAGE, PSA_ALG_HMAC(PSA_ALG_SHA_256),
                   password, password_len, &id) != PSA_SUCCESS) {
        return LG_CRYPTO_ERR;
    }
    uint8_t block[MAX_SALT + 4];
    uint8_t u_prev[32], u_next[32], t[32];
    size_t done = 0;
    uint32_t index = 1;
    int rc = 0;
    while (done < out_len && rc == 0) {
        if (salt_len > 0) {
            memcpy(block, salt, salt_len);
        }
        block[salt_len]     = (uint8_t)(index >> 24);
        block[salt_len + 1] = (uint8_t)(index >> 16);
        block[salt_len + 2] = (uint8_t)(index >> 8);
        block[salt_len + 3] = (uint8_t)index;
        size_t olen = 0;
        if (psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), block, salt_len + 4, u_prev, 32, &olen) != PSA_SUCCESS) {
            rc = LG_CRYPTO_ERR;
            break;
        }
        memcpy(t, u_prev, 32);
        for (uint32_t j = 1; j < iterations; j++) {
            if (psa_mac_compute(id, PSA_ALG_HMAC(PSA_ALG_SHA_256), u_prev, 32, u_next, 32, &olen) != PSA_SUCCESS) {
                rc = LG_CRYPTO_ERR;
                break;
            }
            for (size_t k = 0; k < 32; k++) {
                t[k] ^= u_next[k];
            }
            memcpy(u_prev, u_next, 32);
        }
        size_t take = (out_len - done) < 32 ? (out_len - done) : 32;
        memcpy(out + done, t, take);
        done += take;
        index++;
    }
    psa_destroy_key(id);
    lg_secure_zero(block, sizeof(block));
    lg_secure_zero(u_prev, sizeof(u_prev));
    lg_secure_zero(u_next, sizeof(u_next));
    lg_secure_zero(t, sizeof(t));
    if (rc != 0) {
        lg_secure_zero(out, out_len);
    }
    return rc;
}

bool lg_ct_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < len; i++) {
        acc |= (uint8_t)(a[i] ^ b[i]);
    }
    return acc == 0;
}

int lg_aead_seal(const uint8_t key[LG_AEAD_KEY_LEN], const uint8_t nonce[LG_AEAD_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out)
{
    psa_key_id_t id = 0;
    if (import_key(PSA_KEY_TYPE_CHACHA20, 256, PSA_KEY_USAGE_ENCRYPT, PSA_ALG_CHACHA20_POLY1305,
                   key, LG_AEAD_KEY_LEN, &id) != PSA_SUCCESS) {
        return LG_CRYPTO_ERR;
    }
    size_t olen = 0;
    psa_status_t st = psa_aead_encrypt(id, PSA_ALG_CHACHA20_POLY1305, nonce, LG_AEAD_NONCE_LEN, aad, aad_len,
                                       pt, pt_len, out, pt_len + LG_AEAD_TAG_BYTES, &olen);
    psa_destroy_key(id);
    return st == PSA_SUCCESS ? (int)olen : LG_CRYPTO_ERR;
}

int lg_aead_open(const uint8_t key[LG_AEAD_KEY_LEN], const uint8_t nonce[LG_AEAD_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out)
{
    if (ct_len < LG_AEAD_TAG_BYTES) {
        return LG_CRYPTO_ERR;
    }
    psa_key_id_t id = 0;
    if (import_key(PSA_KEY_TYPE_CHACHA20, 256, PSA_KEY_USAGE_DECRYPT, PSA_ALG_CHACHA20_POLY1305,
                   key, LG_AEAD_KEY_LEN, &id) != PSA_SUCCESS) {
        return LG_CRYPTO_ERR;
    }
    size_t olen = 0;
    psa_status_t st = psa_aead_decrypt(id, PSA_ALG_CHACHA20_POLY1305, nonce, LG_AEAD_NONCE_LEN, aad, aad_len,
                                       ct, ct_len, out, ct_len - LG_AEAD_TAG_BYTES, &olen);
    psa_destroy_key(id);
    return st == PSA_SUCCESS ? (int)olen : LG_CRYPTO_ERR;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int lg_e2e_init(lg_e2e_t *ctx, uint32_t self_device, const uint8_t priv[LG_X25519_LEN])
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->self = self_device;
    memcpy(ctx->priv, priv, LG_X25519_LEN);
    return lg_x25519_public(ctx->priv, ctx->pub);
}

static const uint8_t *pair_key(lg_e2e_t *ctx, uint32_t peer, const uint8_t *peer_pub)
{
    for (size_t i = 0; i < LG_E2E_CACHE_SIZE; i++) {
        lg_e2e_pair_t *p = &ctx->cache[i];
        if (p->valid && p->peer == peer && memcmp(p->peer_pub, peer_pub, LG_X25519_LEN) == 0) {
            return p->key;
        }
    }
    uint8_t shared[LG_X25519_LEN];
    if (lg_x25519_shared(ctx->priv, peer_pub, shared) != 0) {
        return NULL;
    }
    static const char label[] = "lg direct v1";
    uint8_t info[sizeof(label) - 1 + 8];
    uint32_t lo = ctx->self < peer ? ctx->self : peer;
    uint32_t hi = ctx->self < peer ? peer : ctx->self;
    memcpy(info, label, sizeof(label) - 1);
    wr32(info + sizeof(label) - 1, lo);
    wr32(info + sizeof(label) - 1 + 4, hi);

    lg_e2e_pair_t *slot = &ctx->cache[ctx->next];
    ctx->next = (uint8_t)((ctx->next + 1u) % LG_E2E_CACHE_SIZE);
    int rc = lg_hkdf_sha256(NULL, 0, shared, sizeof(shared), info, sizeof(info), slot->key, LG_AEAD_KEY_LEN);
    lg_secure_zero(shared, sizeof(shared));
    if (rc != 0) {
        lg_secure_zero(slot, sizeof(*slot));
        return NULL;
    }
    slot->peer = peer;
    memcpy(slot->peer_pub, peer_pub, LG_X25519_LEN);
    slot->valid = 1;
    return slot->key;
}

int lg_e2e_seal(lg_e2e_t *ctx, uint32_t peer, const uint8_t *peer_pub, const uint8_t *nonce,
                const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out)
{
    const uint8_t *key = pair_key(ctx, peer, peer_pub);
    return key == NULL ? LG_CRYPTO_ERR : lg_aead_seal(key, nonce, aad, aad_len, pt, pt_len, out);
}

int lg_e2e_open(lg_e2e_t *ctx, uint32_t peer, const uint8_t *peer_pub, const uint8_t *nonce,
                const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out)
{
    const uint8_t *key = pair_key(ctx, peer, peer_pub);
    return key == NULL ? LG_CRYPTO_ERR : lg_aead_open(key, nonce, aad, aad_len, ct, ct_len, out);
}
