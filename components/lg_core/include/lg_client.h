/*
 * lg_client.h - handheld messaging core (transport- and UI-free).
 *
 * One user per handheld: the device index is the user's address.
 *
 * Owns: registration, outbox with delivery states and retransmission,
 * bounded inbox, duplicate suppression, peer presence and public keys
 * (trust on first use), the owner's time rule, and end-to-end encryption
 * of 1:1 messages through the seal/open callbacks.
 *
 * Threading: not thread-safe; call from the network task only.
 * Memory: fixed-size struct, no heap use.
 */
#pragma once

#include "lg_body.h"
#include "lg_dedup.h"
#include "lg_envelope.h"
#include "lg_roster.h"
#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LG_OUTBOX_SIZE          16u
/* A firmware that copies each message out as it arrives can build with a smaller inbox: the
 * handheld service keeps its own list and reads only the newest entry, so it uses 1 (8.3 KB less
 * RAM). The on-board simulator keeps 32 and reads the inbox directly. */
#ifndef LG_INBOX_SIZE
#define LG_INBOX_SIZE           32u
#endif
#define LG_RESEND_MS            3000u
#define LG_CLIENT_DEDUP_SLOTS   LG_MAX_DEVICES

typedef enum {
    LG_OUT_EMPTY     = 0,
    LG_OUT_PENDING   = 1,   /* not yet accepted by a node; retransmitted */
    LG_OUT_ACCEPTED  = 2,   /* a node took it; group/broadcast count deliveries here */
    LG_OUT_DELIVERED = 3,   /* 1:1 recipient's device confirmed */
    LG_OUT_REJECTED  = 4,   /* see reject_reason (lg_ack_status_t) */
    LG_OUT_READ      = 5,   /* 1:1 recipient's device showed it to them */
} lg_out_state_t;

typedef struct {
    uint8_t  state;
    uint8_t  scope;
    uint8_t  reject_reason;
    uint16_t flags;
    uint32_t target;
    uint32_t boot;
    uint32_t seq;
    uint32_t grid_time;        /* fixed per message id; see lg_e2e_aad */
    uint32_t last_tx_ms;
    uint32_t delivered_mask;   /* bit i: roster user i confirmed delivery */
    uint16_t delivered_count;
    uint32_t read_mask;        /* bit i: roster user i reported reading it */
    uint16_t read_count;
    uint16_t len;
    uint8_t  text[LG_TEXT_MAX];
} lg_out_msg_t;

typedef struct {
    uint32_t author;
    uint32_t target;
    uint32_t grid_time;
    uint32_t boot;             /* with author and seq, the message identity a read report names */
    uint32_t seq;
    uint8_t  scope;
    uint16_t flags;
    uint16_t len;
    uint8_t  text[LG_TEXT_MAX];
} lg_in_msg_t;

typedef struct {
    uint32_t device;
    uint32_t epoch;
    uint16_t node;
    uint8_t  state;
    uint8_t  in_use;
    uint8_t  has_key;
    uint8_t  pubkey[LG_PUBKEY_LEN];   /* pinned on first sight */
} lg_peer_t;

typedef enum {
    LG_CEV_REGISTERED,       /* value: node index */
    LG_CEV_PRESENCE,         /* value: device */
    LG_CEV_MESSAGE,          /* value: 0 (newest inbox entry) */
    LG_CEV_OUTBOX,           /* value: outbox index */
    LG_CEV_TIME,             /* value: grid time, 0 when unset */
    LG_CEV_DECRYPT_FAILED,   /* value: author device */
    LG_CEV_KEY_CHANGED,      /* value: device whose advertised key differs from the pinned key */
    LG_CEV_GROUPS,           /* value: number of groups removed; the table in the roster is newer */
    LG_CEV_GROUP_REFUSED,    /* value: lg_ack_status_t the AP refused our last group edit with */
    LG_CEV_NAME,             /* value: device whose name changed, this handheld's own included */
} lg_client_event_type_t;

typedef struct {
    lg_client_event_type_t type;
    uint32_t               value;
} lg_client_event_t;

typedef struct {
    void *ctx;
    bool     (*send)(void *ctx, const uint8_t *frame, size_t len);
    void     (*on_event)(void *ctx, const lg_client_event_t *ev);
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*local_time)(void *ctx);                 /* Unix seconds, 0 if unknown */
    /* Optional. A newer group table replaced removed_n groups, listed in removed. Their
     * messages have already left the outbox and inbox; the glue deletes its own copies (D52). */
    void     (*on_groups_removed)(void *ctx, const uint16_t *removed, size_t removed_n);
    void     (*set_time)(void *ctx, uint32_t unix_s);
    /*
     * End-to-end encryption for 1:1 messages. seal writes pt_len + 16 bytes and
     * returns that length; open writes ct_len - 16 bytes and returns that length.
     * Both return a negative value on failure. 1:1 sends fail without them.
     */
    int (*seal)(void *ctx, uint32_t peer, const uint8_t *peer_pub, const uint8_t *nonce,
                const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out);
    int (*open)(void *ctx, uint32_t peer, const uint8_t *peer_pub, const uint8_t *nonce,
                const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out);
} lg_client_io_t;

typedef struct {
    uint32_t           device;
    uint32_t           boot;
    uint32_t           seq;
    uint32_t           attach_count;
    uint8_t            pubkey[LG_PUBKEY_LEN];
    lg_roster_t       *roster;             /* owned by the glue; replaced by newer group tables */
    uint32_t           group_edit_seq;     /* our last GROUP_EDIT, to report its refusal */
    lg_client_io_t     io;
    bool               connected;
    bool               registered;
    bool               grid_time_known;
    uint16_t           node;
    lg_out_msg_t       outbox[LG_OUTBOX_SIZE];
    lg_in_msg_t        inbox[LG_INBOX_SIZE];
    uint16_t           inbox_head;
    uint16_t           inbox_count;
    lg_peer_t          peers[LG_MAX_DEVICES];
    lg_name_t          names[LG_MAX_DEVICES];   /* indexed by roster user index; version 0 = none */
    lg_dedup_entry_t   dedup_slots[LG_CLIENT_DEDUP_SLOTS];
    lg_dedup_t         dedup;
    uint32_t           decrypt_failures;
    uint32_t           last_pong_ms;       /* now_ms when the node last answered a PING, 0 never */
} lg_client_t;

void lg_client_init(lg_client_t *c, uint32_t device, uint32_t boot, const uint8_t *pubkey,
                    lg_roster_t *roster, const lg_client_io_t *io);

/* The transport reached the node: sends REGISTER. */
void lg_client_connected(lg_client_t *c);
void lg_client_disconnected(lg_client_t *c);

/* Sends a keepalive PING while connected. The node answers PONG, recorded in last_pong_ms.
 * Nodes close sessions that stay silent for 30 s, so call it every few seconds. */
void lg_client_ping(lg_client_t *c);

void lg_client_on_frame(lg_client_t *c, const uint8_t *frame, size_t len);

/*
 * Queues a text message. scope is LG_SCOPE_DIRECT (target = device),
 * LG_SCOPE_GROUP (target = group ID), or LG_SCOPE_BROADCAST (target ignored).
 * Returns the outbox index, or LG_ERR_ARG, LG_ERR_TIME (owner rule), LG_ERR_FULL.
 */
int lg_client_send_text(lg_client_t *c, uint8_t scope, uint32_t target, uint16_t flags,
                        const uint8_t *text, size_t len);

/*
 * Asks the AP to make, change, or remove a group (D52). Not retried: the AP answers with the new
 * table (LG_CEV_GROUPS) or a refusal (LG_CEV_GROUP_REFUSED). LG_ERR_ARG for an unknown op,
 * LG_ERR_SHORT when no session is registered.
 */
int lg_client_edit_group(lg_client_t *c, const lg_group_edit_t *edit);

/* Retransmits pending messages; call about once a second. */
void lg_client_tick(lg_client_t *c);

/* True while grid time is unset or unknown: only receiving and URGENT broadcasts work. */
bool lg_client_time_restricted(const lg_client_t *c);

const lg_in_msg_t *lg_client_inbox(const lg_client_t *c, size_t newest_index);

/*
 * Reports that a received 1:1 message has been shown to its reader, so the author can mark it
 * read. Names the message by its identity, as every ack does. Refuses our own messages and a
 * session that is not open; the caller decides when a message counts as read, and reports once.
 */
bool lg_client_mark_read(lg_client_t *c, uint32_t author, uint32_t boot, uint32_t seq);
const lg_peer_t   *lg_client_peer(const lg_client_t *c, uint32_t device);

/*
 * Names (D50). A handheld chooses its own name; the grid carries the newest version to everyone.
 *
 * lg_client_set_name takes 1..LG_NAME_MAX - 1 bytes of UTF-8, gives it a version above every one
 * this handheld used before, emits LG_CEV_NAME, and sends it now if registered (and again on every
 * registration). The version is at least boot << 12, and the boot counter is kept in flash, so it
 * grows across reboots without a second counter. Returns LG_OK or LG_ERR_ARG.
 */
int lg_client_set_name(lg_client_t *c, const uint8_t *text, size_t len);

/* Loads a name kept in flash at boot, this handheld's or another's. Taken only if newer. */
bool lg_client_restore_name(lg_client_t *c, const lg_name_t *name);

/* The newest name known for device, or NULL when none was chosen (the roster name applies). */
const lg_name_t *lg_client_name(const lg_client_t *c, uint32_t device);

#ifdef __cplusplus
}
#endif
