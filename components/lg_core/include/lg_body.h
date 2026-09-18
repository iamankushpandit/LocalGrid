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
    LG_ACK_REJ_NOT_ALLOWED    = 10,  /* the admin page does not let this handheld announce (D56) */
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

/*
 * NAME: handheld -> its AP when renamed or on registering, AP -> APs (flooded), AP -> handhelds.
 *   u32 device | u32 version | u8 length (1..LG_NAME_MAX - 1) | that many bytes of UTF-8, no NUL
 * Only the handheld itself sets its name. Its version always grows, across reboots too (see
 * lg_client_set_name), and every holder keeps the highest version it has seen, so copies that
 * met in any order agree. A record no newer than the one held is dropped and not forwarded.
 */
#define LG_NAME_LEN_MIN 10u
#define LG_NAME_LEN_MAX (9u + LG_NAME_MAX - 1u)
typedef struct {
    uint32_t device;
    uint32_t version;          /* 0: no name chosen; the roster name applies */
    uint8_t  len;
    char     text[LG_NAME_MAX];   /* NUL-terminated after decoding */
} lg_name_t;

/*
 * TIME_SYNC: AP -> APs (flooded), AP -> its handhelds.
 *   u32 grid time, Unix seconds | u16 milliseconds into that second (0..999) |
 *   u8 quality | u8 stratum
 * Stratum is the distance from where grid time was set: 0 on the AP an admin set it on, one more
 * on each AP that took it from another, LG_STRATUM_UNKNOWN when the sender does not say. APs take
 * corrections only from a lower stratum, so time flows outward from its source and never loops
 * between APs that carry it. The 5-byte form without milliseconds or stratum still decodes.
 */
#define LG_TIME_SYNC_LEN        8u
#define LG_TIME_SYNC_LEN_V1     5u
#define LG_STRATUM_UNKNOWN      255u
typedef enum { LG_TIME_UNSET = 0, LG_TIME_CARRIED = 1, LG_TIME_AUTHORITATIVE = 2 } lg_time_quality_t;
typedef struct {
    uint32_t grid_time;
    uint16_t millis;
    uint8_t  quality;
    uint8_t  stratum;
} lg_time_sync_t;

/* NODE_HELLO: node -> neighbors, every 2 s. */
#define LG_HELLO_LEN 4u
typedef struct {
    uint16_t node;
    uint8_t  role;           /* 0 node, 1 master */
    uint8_t  clients;
} lg_hello_t;

/*
 * GROUPS: the whole group table (D52) and who may announce (D56), little-endian:
 *     0  u32  seq
 *     4  u16  author (AP index)
 *     6  u16  next_id
 *     8  u32  announcers (roster user bits; all bits set = everyone, the default)
 *    12  u8   count, 0..LG_MAX_GROUPS
 *    13  count x LG_GROUP_ENTRY_LEN:
 *          0  u16   id (not 0, below next_id, unique)
 *          2  u32   members (roster user bits)
 *          6  16 B  name, 1..15 bytes of UTF-8, NUL-padded
 * The length is exactly 13 + count x 22: 189 bytes at most, inside even an ESP-NOW v1 frame.
 * One version covers both, because both are admin-owned policy carried by (seq, author).
 */
#define LG_GROUPS_HEAD_LEN   13u
#define LG_GROUP_ENTRY_LEN   22u
#define LG_GROUPS_MAX_LEN    (LG_GROUPS_HEAD_LEN + LG_MAX_GROUPS * LG_GROUP_ENTRY_LEN)

/* GROUP_EDIT: u8 op, u16 id, u32 members, 16 B name (NUL-padded). Exactly 23 bytes.
 * LG_GROUP_ANNOUNCERS carries the announcer bits in `members`; id and name are unused. */
#define LG_GROUP_EDIT_LEN    23u

/*
 * VOICE (D61): live push-to-talk, 1:1 or group, never broadcast, stored, or retried.
 * Plaintext payload = 10-byte header + 0..LG_VOICE_DATA_MAX bytes of codec data:
 *     0  u8   codec        LG_VOICE_CODEC_IMA_8K: IMA ADPCM, 8 kHz mono, two samples a byte
 *     1  u8   flags        LG_VOICE_END on the last frame of a talk (release); other bits 0 today
 *     2  u16  talk         which press of the talk button, so a receiver tells talks apart
 *     4  u16  frame        frame number within the talk, from 0
 *     6  i16  predictor    ADPCM decoder state at the first sample of this frame
 *     8  u8   step_index   ADPCM step index, 0..88
 *     9  u8   reserved     0
 * Each frame carries the decoder state, so a lost frame costs 100 ms and nothing after it.
 * DIRECT bodies are that payload sealed like 1:1 text (LG_FLAG_E2E_PAYLOAD, 16-byte tag);
 * GROUP bodies are the plain payload. origin_seq always has LG_VOICE_SEQ_BIT set.
 */
#define LG_VOICE_HDR_LEN      10u
#define LG_VOICE_CODEC_IMA_8K 1u
#define LG_VOICE_END          0x01u        /* last frame of a talk (release) */
#define LG_VOICE_STEP_MAX     88u
#define LG_VOICE_PAYLOAD_MAX  (LG_VOICE_HDR_LEN + LG_VOICE_DATA_MAX)
typedef struct {
    uint8_t  codec;
    uint8_t  flags;
    uint16_t talk;
    uint16_t frame;
    int16_t  predictor;
    uint8_t  step_index;
} lg_voice_hdr_t;

/* Writes LG_VOICE_HDR_LEN bytes and returns that length. */
size_t lg_voice_hdr_enc(const lg_voice_hdr_t *h, uint8_t *out);
/* body = header + 0..LG_VOICE_DATA_MAX data bytes. Rejects unknown codec, step_index > 88,
 * reserved != 0, and any other length. out may be NULL to check only. */
bool   lg_voice_hdr_dec(const uint8_t *body, size_t len, lg_voice_hdr_t *out);

/* The newest voice frame seen from one author: voice is only ever played forward, so a frame
 * no newer than this within the same boot is dropped. seq 0 means none seen (voice seqs carry
 * LG_VOICE_SEQ_BIT, so they are never 0). */
typedef struct {
    uint32_t boot;
    uint32_t seq;
} lg_voice_seen_t;

static inline bool lg_voice_newer(const lg_voice_seen_t *s, uint32_t boot, uint32_t seq)
{
    return s->seq == 0 || boot > s->boot || (boot == s->boot && seq > s->seq);
}

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
size_t lg_name_enc(const lg_name_t *v, uint8_t *out);   /* out holds LG_NAME_LEN_MAX bytes */
bool   lg_name_dec(const uint8_t *in, size_t len, lg_name_t *v);
/* True if text is 1..LG_NAME_MAX - 1 bytes of valid UTF-8. */
bool   lg_name_valid(const uint8_t *text, size_t len);
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
