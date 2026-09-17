/*
 * lg_body.h - fixed-layout message bodies for the prototype (PROTOCOL v0.1).
 *
 * Prototype bodies are hand-specified little-endian structs so the prototype
 * needs no code generator. Phase 1 proper moves them to nanopb (answer 16 of
 * docs/DESIGN_REVIEW.md) without changing the envelope.
 *
 * TEXT bodies:
 *   GROUP / BROADCAST : raw UTF-8, 1..LG_TEXT_MAX bytes, no NUL.
 *   DIRECT            : ChaCha20-Poly1305 ciphertext of that UTF-8 plus a 16-byte tag,
 *                       with LG_FLAG_E2E_PAYLOAD set (see lg_e2e_nonce / lg_e2e_aad).
 */
#pragma once

#include "lg_roster.h"
#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* REGISTER: client -> node, first frame of every session. */
#define LG_REGISTER_LEN (14u + LG_PUBKEY_LEN)
typedef struct {
    uint32_t device;
    uint32_t attach_epoch;   /* (boot << 12) | attach counter: always grows, even across reboots */
    uint32_t client_time;    /* client clock, Unix seconds, 0 if unknown */
    uint16_t caps;           /* capability bits, reserved in the prototype */
    uint8_t  pubkey[LG_PUBKEY_LEN];  /* X25519 public key for end-to-end 1:1 messages */
} lg_register_t;

/* REGISTER_ACK: node -> client. */
#define LG_REGISTER_ACK_LEN 9u
enum { LG_REG_OK = 0, LG_REG_UNKNOWN_DEVICE = 1 };
typedef struct {
    uint16_t node;
    uint8_t  status;
    uint32_t grid_time;      /* node clock, 0 when grid time is unset */
    uint16_t roster_version;
} lg_register_ack_t;

/*
 * MSG_ACK: node -> author (ACCEPTED / REJECTED_*), recipient -> author (DELIVERED, then READ).
 * The body is unchanged by READ: it is a status value, appended so the existing ones keep
 * their numbers, and older code ignores a status it does not know (design review answer 24).
 */
#define LG_MSG_ACK_LEN 13u
typedef enum {
    LG_ACK_ACCEPTED           = 1,
    LG_ACK_DELIVERED          = 2,
    LG_ACK_REJ_OFFLINE        = 3,
    LG_ACK_REJ_NOT_MEMBER     = 4,
    LG_ACK_REJ_RATE           = 5,
    LG_ACK_REJ_TIME           = 6,
    LG_ACK_REJ_UNKNOWN_TARGET = 7,
    LG_ACK_REJ_INVALID        = 8,
    LG_ACK_READ               = 9,   /* the recipient's handheld showed it to them */
} lg_ack_status_t;
typedef struct {
    uint32_t author;
    uint32_t boot;
    uint32_t seq;
    uint8_t  status;
} lg_msg_ack_t;

/* PRESENCE_UPDATE: node -> nodes (flood), node -> client. */
#define LG_PRESENCE_LEN (11u + LG_PUBKEY_LEN)
typedef enum { LG_PRES_OFFLINE = 0, LG_PRES_ONLINE = 1 } lg_presence_state_t;
typedef struct {
    uint32_t device;
    uint16_t node;
    uint32_t epoch;
    uint8_t  state;
    uint8_t  pubkey[LG_PUBKEY_LEN];
} lg_presence_t;

/* TIME_SYNC: master -> nodes, node -> clients. */
#define LG_TIME_SYNC_LEN 5u
typedef enum { LG_TIME_UNSET = 0, LG_TIME_CARRIED = 1, LG_TIME_AUTHORITATIVE = 2 } lg_time_quality_t;
typedef struct {
    uint32_t grid_time;
    uint8_t  quality;
} lg_time_sync_t;

/* NODE_HELLO: node -> neighbors, every 2 s. */
#define LG_HELLO_LEN 4u
typedef struct {
    uint16_t node;
    uint8_t  role;           /* 0 node, 1 master */
    uint8_t  clients;
} lg_hello_t;

/*
 * GROUPS: the whole group table (D52), little-endian:
 *     0  u32  seq
 *     4  u16  author (AP index)
 *     6  u16  next_id
 *     8  u8   count, 0..LG_MAX_GROUPS
 *     9  count x LG_GROUP_ENTRY_LEN:
 *          0  u16   id (not 0, below next_id, unique)
 *          2  u32   members (roster user bits)
 *          6  16 B  name, 1..15 bytes of UTF-8, NUL-padded
 * The length is exactly 9 + count x 22: 185 bytes at most, inside even an ESP-NOW v1 frame.
 */
#define LG_GROUPS_HEAD_LEN   9u
#define LG_GROUP_ENTRY_LEN   22u
#define LG_GROUPS_MAX_LEN    (LG_GROUPS_HEAD_LEN + LG_MAX_GROUPS * LG_GROUP_ENTRY_LEN)

/* GROUP_EDIT: u8 op, u16 id, u32 members, 16 B name (NUL-padded). Exactly 23 bytes. */
#define LG_GROUP_EDIT_LEN    23u

size_t lg_register_enc(const lg_register_t *v, uint8_t *out);
bool   lg_register_dec(const uint8_t *in, size_t len, lg_register_t *v);
size_t lg_register_ack_enc(const lg_register_ack_t *v, uint8_t *out);
bool   lg_register_ack_dec(const uint8_t *in, size_t len, lg_register_ack_t *v);
size_t lg_msg_ack_enc(const lg_msg_ack_t *v, uint8_t *out);
bool   lg_msg_ack_dec(const uint8_t *in, size_t len, lg_msg_ack_t *v);
size_t lg_presence_enc(const lg_presence_t *v, uint8_t *out);
bool   lg_presence_dec(const uint8_t *in, size_t len, lg_presence_t *v);
size_t lg_time_sync_enc(const lg_time_sync_t *v, uint8_t *out);
bool   lg_time_sync_dec(const uint8_t *in, size_t len, lg_time_sync_t *v);
size_t lg_hello_enc(const lg_hello_t *v, uint8_t *out);
bool   lg_hello_dec(const uint8_t *in, size_t len, lg_hello_t *v);

size_t lg_groups_enc(const lg_groups_t *v, uint8_t *out);
bool   lg_groups_dec(const uint8_t *in, size_t len, lg_groups_t *v);
size_t lg_group_edit_enc(const lg_group_edit_t *v, uint8_t *out);
bool   lg_group_edit_dec(const uint8_t *in, size_t len, lg_group_edit_t *v);

/* Strict UTF-8: rejects overlong forms, surrogates, code points above U+10FFFF, and NUL. */
bool lg_utf8_valid(const uint8_t *s, size_t n);

/* True if s is 1..LG_TEXT_MAX bytes of valid UTF-8. */
bool lg_text_valid(const uint8_t *s, size_t n);

#ifdef __cplusplus
}
#endif
