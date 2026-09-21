/*
 * lg_crypto.h - the only cryptography interface LocalGrid code uses.
 *
 * Primitives (docs/DESIGN_REVIEW.md answer 26):
 *   X25519 (RFC 7748), HKDF-SHA256 (RFC 5869), ChaCha20-Poly1305 (RFC 8439).
 * Backend: PSA Crypto in ESP-IDF v6.x. HKDF is built from PSA HMAC-SHA256
 * exactly as RFC 5869 specifies, so it does not depend on PSA's optional HKDF.
 *
 * All functions return 0 or a positive length on success and a negative value
 * on failure. Randomness is only strong after Wi-Fi or Bluetooth has started.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LG_CRYPTO_ERR        (-1)
#define LG_X25519_LEN        32u
#define LG_AEAD_KEY_LEN      32u
#define LG_AEAD_NONCE_LEN    12u
#define LG_AEAD_TAG_BYTES    16u
#define LG_E2E_CACHE_SIZE    8u

int lg_crypto_init(void);
int lg_crypto_random(uint8_t *out, size_t len);

/* Generates a clamped X25519 private key and its public key. */
int lg_x25519_keypair(uint8_t priv[LG_X25519_LEN], uint8_t pub[LG_X25519_LEN]);
int lg_x25519_public(const uint8_t priv[LG_X25519_LEN], uint8_t pub[LG_X25519_LEN]);
/* Rejects the all-zero shared secret produced by small-order peer keys. */
int lg_x25519_shared(const uint8_t priv[LG_X25519_LEN], const uint8_t peer_pub[LG_X25519_LEN],
                     uint8_t shared[LG_X25519_LEN]);

int lg_hkdf_sha256(const uint8_t *salt, size_t salt_len, const uint8_t *ikm, size_t ikm_len,
                   const uint8_t *info, size_t info_len, uint8_t *out, size_t out_len);

/*
 * HMAC-SHA256 (RFC 2104), 32 bytes out. Used for the BLE admin link's login proof (D70), where the
 * key is the stored PBKDF2 hash and the message is the AP's challenge. An empty key is padded with
 * zeros to the block size, as HMAC specifies.
 */
int lg_hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/*
 * SHA-256 (FIPS 180-4), 32 bytes out. Used by lg_identity to recompute a device ID from the
 * board's own MAC and check the stored identity was minted for this board.
 */
int lg_sha256(const uint8_t *msg, size_t msg_len, uint8_t out[32]);

/*
 * PBKDF2-HMAC-SHA256 (RFC 8018 section 5.2) for the admin password.
 * password 1..128 bytes, salt 0..64 bytes, out 1..64 bytes, iterations >= 1.
 */
int lg_pbkdf2_sha256(const uint8_t *password, size_t password_len, const uint8_t *salt, size_t salt_len,
                     uint32_t iterations, uint8_t *out, size_t out_len);

/* Constant-time equality for secrets such as password hashes and session tokens. */
bool lg_ct_equal(const uint8_t *a, const uint8_t *b, size_t len);

/* Returns pt_len + 16. out must hold that many bytes. */
int lg_aead_seal(const uint8_t key[LG_AEAD_KEY_LEN], const uint8_t nonce[LG_AEAD_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out);
/* Returns ct_len - 16, or negative if the tag does not verify. */
int lg_aead_open(const uint8_t key[LG_AEAD_KEY_LEN], const uint8_t nonce[LG_AEAD_NONCE_LEN],
                 const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out);

/*
 * End-to-end 1:1 encryption context for one handheld.
 * Pairwise key = HKDF-SHA256(X25519(priv, peer_pub), salt = none,
 *                            info = "lg direct v1" || min(device) || max(device)).
 * Keys are cached per peer (least recently derived slot is replaced).
 */
typedef struct {
    uint32_t peer;
    uint8_t  peer_pub[LG_X25519_LEN];
    uint8_t  key[LG_AEAD_KEY_LEN];
    uint8_t  valid;
} lg_e2e_pair_t;

typedef struct {
    uint32_t      self;
    uint8_t       priv[LG_X25519_LEN];
    uint8_t       pub[LG_X25519_LEN];
    lg_e2e_pair_t cache[LG_E2E_CACHE_SIZE];
    uint8_t       next;
} lg_e2e_t;

int lg_e2e_init(lg_e2e_t *ctx, uint32_t self_device, const uint8_t priv[LG_X25519_LEN]);
int lg_e2e_seal(lg_e2e_t *ctx, uint32_t peer, const uint8_t *peer_pub, const uint8_t *nonce,
                const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out);
int lg_e2e_open(lg_e2e_t *ctx, uint32_t peer, const uint8_t *peer_pub, const uint8_t *nonce,
                const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out);

/* Overwrites sensitive memory in a way the compiler will not optimise away. */
void lg_secure_zero(void *p, size_t n);

#ifdef __cplusplus
}
#endif
