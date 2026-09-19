/*
 * lg_node.h - infrastructure node messaging core (transport- and crypto-free).
 *
 * The node core owns presence, routing, group enforcement, broadcast rate
 * limits, the owner's time rule, and duplicate suppression. Glue code owns
 * sockets, ESP-NOW, and timers, and calls in with complete frames.
 *
 * Threading: not thread-safe. Call every function from one task.
 * Memory: fixed-size struct, no heap use.
 *
 * Routing rules (docs/DESIGN_REVIEW.md answers 20-23):
 *   DIRECT     local session if attached here; else backbone unicast to the
 *              recipient's node if it is a neighbor; else flood with TTL.
 *   GROUP      flood once; every node delivers to its locally attached members.
 *   BROADCAST  flood once; every node delivers to all its local users.
 *   Loops end at the dedup window and TTL; there is no forwarding state.
 *
 * VOICE (D61) routes like TEXT, DIRECT or GROUP only, but is never acknowledged when taken and
 * is suppressed by a newest-only table per author instead of the dedup window. A refused voice
 * frame is answered with MSG_ACK at most once a second per author.
 *
 * DIRECT bodies are end-to-end ciphertext. Nodes never see 1:1 plaintext and
 * reject DIRECT text that is not marked LG_FLAG_E2E_PAYLOAD.
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

#define LG_NODE_DEDUP_SLOTS       48u
#define LG_BROADCAST_INTERVAL_MS  10000u   /* per user, routine broadcasts */
#define LG_URGENT_INTERVAL_MS     2000u    /* per user, URGENT broadcasts */
#define LG_VOICE_REFUSE_MS        1000u    /* per user: at most one voice refusal this often (D61) */

typedef struct {
    void *ctx;
    /* Deliver a frame to the session of a locally attached device. */
    void     (*to_client)(void *ctx, uint32_t device, const uint8_t *frame, size_t len);
    /* Send a frame to one neighbor node. */
    void     (*backbone_unicast)(void *ctx, uint16_t node, const uint8_t *frame, size_t len);
    /* Send a frame to every working neighbor except except_node (LG_NODE_NONE for all). */
    void     (*backbone_flood)(void *ctx, uint16_t except_node, const uint8_t *frame, size_t len);
    bool     (*is_neighbor)(void *ctx, uint16_t node);
    uint32_t (*now_ms)(void *ctx);
    uint32_t (*grid_time)(void *ctx);  /* Unix seconds, 0 when grid time is unset */
    /* Optional. A diagnostic echo arrived; hops = backbone hops travelled. */
    void     (*on_diag)(void *ctx, uint16_t origin_node, uint8_t hops, const uint8_t *text, size_t len);
    /* Optional. Another node announced grid time; glue decides whether to adopt it. */
    void     (*on_time)(void *ctx, uint16_t origin_node, const lg_time_sync_t *t);
    /*
     * Optional. A handheld registered, reporting a grid-set clock (unix_s, never 0), while this
     * node has no grid time: glue may take it, so time survives an AP restart as long as any
     * handheld stayed on (D48, D53). Called before REGISTER_ACK is built, so the ack carries
     * whatever the glue decided. The sender's distance from the source is unknown.
     */
    void     (*on_client_time)(void *ctx, uint32_t device, uint32_t unix_s);
    /* Optional. Fills grid time with milliseconds, this node's stratum, and flags (LG_TIME_FROM_GPS,
     * D67) for TIME_SYNC; the core sets quality. Without it the core sends whole seconds from
     * grid_time, stratum unknown, and no flags. */
    void     (*time_now)(void *ctx, lg_time_sync_t *out);
    /* Optional. Another node flooded its grid state (D45). The body is opaque to the core:
     * 1..LG_GRID_STATE_MAX bytes whose layout the AP firmware defines and checks. */
    void     (*on_grid_state)(void *ctx, uint16_t origin_node, const uint8_t *body, size_t len);
    /* Optional. The roster's group table changed, by an edit here or a newer copy from another
     * AP (D52). The glue saves it; the core has already sent it on. */
    void     (*on_groups_changed)(void *ctx);
    /* Optional. A newer handheld name was taken (from a handheld or the backbone): glue keeps it
     * in flash so it survives this AP restarting (D48). Not called for lg_node_restore_name. */
    void     (*on_name)(void *ctx, const lg_name_t *name);
} lg_node_io_t;

typedef struct {
    uint32_t device;
    uint32_t epoch;
    uint16_t node;
    uint8_t  state;     /* lg_presence_state_t */
    uint8_t  in_use;
    uint8_t  pubkey[LG_PUBKEY_LEN];
} lg_presence_entry_t;

typedef struct {
    uint32_t rx_client;
    uint32_t rx_backbone;
    uint32_t delivered_local;
    uint32_t forwarded;
    uint32_t duplicates;
    uint32_t rejected;
    uint32_t malformed;
    uint32_t voice;             /* voice frames taken here and delivered or passed on (D61) */
} lg_node_stats_t;

typedef struct {
    uint16_t            self;
    uint32_t            boot;
    uint32_t            seq;
    lg_roster_t        *roster;    /* owned by the glue; the core edits its group table */
    lg_node_io_t        io;
    lg_presence_entry_t presence[LG_MAX_DEVICES];
    lg_name_t           names[LG_MAX_DEVICES];              /* indexed by roster user index; version 0 = none */
    lg_position_t       positions[LG_POS_SLOTS];            /* by lg_position_slot; fix_time 0 = none; RAM only (D65) */
    uint32_t            last_broadcast_ms[LG_MAX_DEVICES];  /* indexed by roster user index */
    bool                has_broadcast[LG_MAX_DEVICES];
    lg_dedup_entry_t    dedup_slots[LG_NODE_DEDUP_SLOTS];
    lg_dedup_t          dedup;
    lg_node_stats_t     stats;
    /* Voice (D61) keeps out of the text dedup: its sequences carry LG_VOICE_SEQ_BIT, which would
     * push a text window past every text retransmission. Indexed by roster user index. */
    lg_voice_seen_t     voice_seen[LG_MAX_DEVICES];
    uint32_t            voice_refused_ms[LG_MAX_DEVICES];
    bool                has_voice_refused[LG_MAX_DEVICES];
    uint8_t             time_quality;               /* last quality this node announced or sent */
    uint8_t             tz_len;                     /* D67: 0 = no zone known */
    char                tz[LG_TZ_MAX + 1];
} lg_node_t;

void lg_node_init(lg_node_t *n, uint16_t self, uint32_t boot, lg_roster_t *roster, const lg_node_io_t *io);

/*
 * A frame arrived on a client session. *session_device is 0 until the session
 * registers; the core sets it on a valid REGISTER. Frames whose author does not
 * match the session's device are dropped.
 */
void lg_node_on_session_frame(lg_node_t *n, uint32_t *session_device, const uint8_t *frame, size_t len);

/* The session for device closed or timed out. */
void lg_node_on_session_closed(lg_node_t *n, uint32_t device);

/* A frame arrived from neighbor node from_node. */
void lg_node_on_backbone_frame(lg_node_t *n, uint16_t from_node, const uint8_t *frame, size_t len);

/* A neighbor link became usable: re-announce local presence so partitions reconcile. */
void lg_node_on_neighbor_up(lg_node_t *n, uint16_t neighbor);

/* Sends TIME_SYNC to one attached device. A registering device gets one after REGISTER_ACK when
 * this node has grid time, carrying the quality last announced or sent (CARRIED if none yet). */
void lg_node_send_time(lg_node_t *n, uint32_t device, uint8_t quality);

/*
 * The grid's time zone (D67), a POSIX TZ string the glue takes from the grid settings. A zone that
 * differs from the one held is kept and sent to every locally attached device; each registering
 * device gets it after REGISTER_ACK. NULL or "" forgets it and sends nothing, so an AP that has not
 * yet heard the settings never clears a zone a handheld remembers. Returns LG_OK, or LG_ERR_ARG
 * (not lg_tz_valid; the zone held is kept).
 */
int lg_node_set_time_zone(lg_node_t *n, const char *tz);

/*
 * Floods this node's current grid time to every node and pushes it to every
 * locally attached device. Receiving nodes call io.on_time, then push the time
 * they hold afterwards to their own devices.
 */
void lg_node_announce_time(lg_node_t *n, uint8_t quality);

/* Floods a diagnostic echo (1..LG_TEXT_MAX bytes of UTF-8); each other node reports it once. */
int lg_node_send_diag(lg_node_t *n, const uint8_t *text, size_t len);

/*
 * Floods this node's grid state (1..LG_GRID_STATE_MAX opaque bytes) to every other node, each of
 * which reports it once through io.on_grid_state. Never sent to a handheld session.
 */
int lg_node_announce_grid_state(lg_node_t *n, const uint8_t *body, size_t len);

/*
 * Groups (D52). A handheld's GROUP_EDIT and the admin page's edits both end here, become a new
 * version of the table, and go to every AP and every attached handheld. A newer table from the
 * backbone replaces this node's and is passed on the same way; an older one is only forwarded.
 * Every handheld gets the table when it registers.
 */
/* Applies an admin edit (no membership rule). Returns 0 or the lg_ack_status_t refusing it. */
uint8_t lg_node_edit_groups(lg_node_t *n, const lg_group_edit_t *edit);

/* Floods this node's group table, so APs that missed a change catch up. */
int lg_node_announce_groups(lg_node_t *n);

const lg_presence_entry_t *lg_node_presence(const lg_node_t *n, uint32_t device);

/*
 * Handheld names (D50). A handheld sends its own NAME; the AP keeps the newest version per
 * device, floods a newer one to every AP, and pushes it to its handhelds. Every name held is
 * sent to a handheld when it registers and flooded when a neighbour link comes up.
 */

/* Loads a name kept in flash at boot. Takes it only if newer; sends nothing. */
bool lg_node_restore_name(lg_node_t *n, const lg_name_t *name);

/* Floods every name held, for the periodic announcement D48 asks for. */
void lg_node_announce_names(lg_node_t *n);

/*
 * Floods the presence of every handheld registered here, again (D48). Presence was sent only when a
 * handheld registered and when a link came up, so an AP that missed that one flood (a restart whose link
 * the peer confirmed first, a full backbone queue) refused 1:1 messages to handhelds it had never heard
 * of until something else made the peer send again: the chaos run of 2026-09-18 lost 20 messages this
 * way. Receivers pass on only changes. Call every LG_PRESENCE_ANNOUNCE_MS.
 */
#define LG_PRESENCE_ANNOUNCE_MS 60000u
void lg_node_announce_presence(lg_node_t *n);

/* The newest name held for device, or NULL when it has none. */
const lg_name_t *lg_node_name(const lg_node_t *n, uint32_t device);

/*
 * Positions (D65). A handheld sends its own POSITION; the AP keeps the newest fix per handheld and
 * per AP in RAM (never flash), floods a newer one to every AP, and pushes it to its other handhelds.
 * An older or equal fix is dropped and not passed on. Every position held is sent to a handheld
 * when it registers and flooded when a neighbour link comes up. A handheld POSITION with
 * LG_POS_LIVE gives an AP that has no grid time a clock, through io.on_client_time as REGISTER's
 * client_time does. Stored copies have LG_POS_LIVE cleared.
 */

/* This AP's own fix (subject LG_NODE_ID_BASE | self). Taken, flooded, and pushed to the attached
 * handhelds only if newer. Returns LG_OK, LG_ERR_ARG (out of range, or self >= LG_MAX_NODES),
 * or LG_ERR_ID when it is no newer than the one held. */
int lg_node_set_own_position(lg_node_t *n, int32_t lat_u, int32_t lon_u, uint32_t fix_time, uint8_t sats);

/* The newest position held for subject (device, or LG_NODE_ID_BASE | node), or NULL. */
const lg_position_t *lg_node_position(const lg_node_t *n, uint32_t subject);

/* Copies up to max held positions into out, handhelds by roster order then APs by index.
 * Returns how many were written. */
size_t lg_node_positions(const lg_node_t *n, lg_position_t *out, size_t max);

#ifdef __cplusplus
}
#endif
