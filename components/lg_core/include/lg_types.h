/*
 * lg_types.h - shared limits, error codes, and little-endian helpers.
 * Every limit here is a hard bound: no structure in lg_core grows past it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LG_MAX_DEVICES          32u          /* handheld users in one grid (one user per device) */
#define LG_MAX_GROUPS           8u
#define LG_MAX_NODES            8u
#define LG_NODE_NONE            0xFFFFu      /* "no node" / "not from the backbone" */
#define LG_NODE_ID_BASE         0x80000000u  /* node-authored messages use origin_id = base | node index */
#define LG_TARGET_ALL           0xFFFFFFFFu  /* broadcast target */
#define LG_TEXT_MAX             240u         /* UTF-8 bytes in one text message */
#define LG_FRAME_MAX            1024u        /* envelope + body on any Phase 1 link */
#define LG_TTL_DEFAULT          3u           /* backbone hop limit */
#define LG_TIME_TOLERANCE_S     120u         /* owner rule: device clock vs node clock */
#define LG_AEAD_TAG_LEN         16u          /* ChaCha20-Poly1305 tag */
#define LG_PUBKEY_LEN           32u          /* X25519 public key */
#define LG_DIRECT_BODY_MAX      (LG_TEXT_MAX + LG_AEAD_TAG_LEN)
#define LG_GRID_STATE_MAX       256u         /* bytes in one GRID_STATE body; its layout belongs to AP firmware */

typedef enum {
    LG_OK             = 0,
    LG_ERR_SHORT      = -1,   /* buffer shorter than required */
    LG_ERR_VERSION    = -2,   /* unsupported protocol major version */
    LG_ERR_RESERVED   = -3,   /* reserved field not zero */
    LG_ERR_LENGTH     = -4,   /* body length inconsistent or too long */
    LG_ERR_SCOPE      = -5,   /* scope, target, and type do not agree */
    LG_ERR_TYPE       = -6,   /* unexpected message type */
    LG_ERR_ARG        = -7,   /* invalid argument */
    LG_ERR_FULL       = -8,   /* bounded table or queue is full */
    LG_ERR_ID         = -9,   /* invalid message identity (sequence 0) */
    LG_ERR_TIME       = -10,  /* sending restricted until grid time is correct */
} lg_err_t;

const char *lg_err_str(int err);

static inline void lg_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void lg_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline uint16_t lg_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static inline uint32_t lg_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint32_t lg_node_origin_id(uint16_t node)
{
    return LG_NODE_ID_BASE | node;
}

#ifdef __cplusplus
}
#endif
