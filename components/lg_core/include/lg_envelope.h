/*
 * lg_envelope.h - the fixed 32-byte LocalGrid envelope (PROTOCOL v0.1).
 *
 * Layout, little-endian:
 *   0  u8  version      major << 4 | minor
 *   1  u8  type         lg_msg_type_t
 *   2  u16 flags        LG_FLAG_*
 *   4  u8  scope        lg_scope_t
 *   5  u8  ttl          backbone hop limit
 *   6  u16 body_len     bytes following the envelope
 *   8  u32 origin_id    author device index, or LG_NODE_ID_BASE | node index
 *  12  u32 origin_boot  author's boot counter
 *  16  u32 origin_seq   author's per-boot sequence, never 0
 *  20  u32 target       device index, group ID, or LG_TARGET_ALL
 *  24  u32 grid_time    author's clock, Unix seconds, 0 if unknown
 *  28  u16 origin_node  node where the message entered the backbone
 *  30  u16 reserved     must be 0
 *
 * A frame is exactly envelope + body. Message identity is
 * (origin_id, origin_boot, origin_seq); wall-clock time is never part of it.
 */
#pragma once

#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LG_PROTO_MAJOR  0u
#define LG_PROTO_MINOR  1u
#define LG_ENV_SIZE     32u
#define LG_BODY_MAX     (LG_FRAME_MAX - LG_ENV_SIZE)

typedef enum {
    LG_T_NODE_HELLO      = 0x01,
    LG_T_NODE_ACK        = 0x02,
    LG_T_REGISTER        = 0x10,
    LG_T_REGISTER_ACK    = 0x11,
    LG_T_PING            = 0x12,
    LG_T_PONG            = 0x13,
    LG_T_PRESENCE_UPDATE = 0x20,
    LG_T_NAME            = 0x23,   /* a handheld's chosen name, versioned; newest wins everywhere */
    LG_T_POSITION        = 0x24,   /* a handheld's or AP's GPS position, newest fix wins; RAM only (D65) */
    LG_T_TEXT            = 0x30,
    LG_T_MSG_ACK         = 0x31,
    LG_T_VOICE           = 0x40,   /* live push-to-talk frame, 1:1 or group; never stored or retried (D61) */
    LG_T_TIME_SYNC       = 0x50,
    LG_T_GRID_STATE      = 0x51,   /* AP to AP only: replicated admin settings and time generation (D45) */
    LG_T_GROUPS          = 0x52,   /* AP to AP and AP to handheld: the whole group table and its version (D52) */
    LG_T_GROUP_EDIT      = 0x53,   /* handheld to AP: make, change, or remove one group (D52) */
    LG_T_DIAG_ECHO       = 0x70,   /* node console test: flooded once, reported by every node */
    LG_T_ERROR           = 0x7F,
} lg_msg_type_t;

typedef enum {
    LG_SCOPE_SYSTEM    = 0,
    LG_SCOPE_DIRECT    = 1,
    LG_SCOPE_GROUP     = 2,
    LG_SCOPE_BROADCAST = 3,
} lg_scope_t;

enum {
    LG_FLAG_ACK_REQUESTED = 0x0001,
    LG_FLAG_URGENT        = 0x0002,
    LG_FLAG_E2E_PAYLOAD   = 0x0004,
    LG_FLAG_FRAGMENT      = 0x0008,
    LG_FLAG_RELAYED       = 0x0010,
};

typedef struct {
    uint8_t  major;
    uint8_t  minor;
    uint8_t  type;
    uint16_t flags;
    uint8_t  scope;
    uint8_t  ttl;
    uint16_t body_len;
    uint32_t origin_id;
    uint32_t origin_boot;
    uint32_t origin_seq;
    uint32_t target;
    uint32_t grid_time;
    uint16_t origin_node;
} lg_env_t;

/* Writes the 32-byte envelope. Returns LG_ENV_SIZE or a negative lg_err_t. */
int lg_env_encode(const lg_env_t *env, uint8_t *out, size_t cap);

/* Parses and validates a complete frame (len must equal 32 + body_len). */
int lg_env_decode(const uint8_t *frame, size_t len, lg_env_t *env);

/* Sets env->body_len, writes envelope + body. Returns total length or negative. */
int lg_frame_build(lg_env_t *env, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap);

static inline const uint8_t *lg_frame_body(const uint8_t *frame)
{
    return frame + LG_ENV_SIZE;
}

/*
 * End-to-end encryption inputs for DIRECT messages.
 * Nonce (12 B)  = origin_id || origin_boot || origin_seq, little-endian. Unique per author per key.
 * AAD   (24 B)  = type, scope, flags without RELAYED, origin_id, origin_boot, origin_seq,
 *                 target, grid_time. Excludes ttl and origin_node, which nodes rewrite.
 * A message id must always be sealed with the same grid_time and plaintext, so a
 * retransmission produces an identical ciphertext and never reuses a nonce with
 * different inputs.
 */
#define LG_E2E_NONCE_LEN 12u
#define LG_E2E_AAD_LEN   24u
void lg_e2e_nonce(const lg_env_t *env, uint8_t out[LG_E2E_NONCE_LEN]);
void lg_e2e_aad(const lg_env_t *env, uint8_t out[LG_E2E_AAD_LEN]);

#ifdef __cplusplus
}
#endif
