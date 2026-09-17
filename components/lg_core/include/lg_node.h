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
    void     (*on_time)(void *ctx, uint16_t origin_node, uint32_t grid_time, uint8_t quality);
    /* Optional. Another node flooded its grid state (D45). The body is opaque to the core:
     * 1..LG_GRID_STATE_MAX bytes whose layout the AP firmware defines and checks. */
    void     (*on_grid_state)(void *ctx, uint16_t origin_node, const uint8_t *body, size_t len);
    /* Optional. The roster's group table changed, by an edit here or a newer copy from another
     * AP (D52). The glue saves it; the core has already sent it on. */
    void     (*on_groups_changed)(void *ctx);
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
} lg_node_stats_t;

typedef struct {
    uint16_t            self;
    uint32_t            boot;
    uint32_t            seq;
    lg_roster_t        *roster;    /* owned by the glue; the core edits its group table */
    lg_node_io_t        io;
    lg_presence_entry_t presence[LG_MAX_DEVICES];
    uint32_t            last_broadcast_ms[LG_MAX_DEVICES];  /* indexed by roster user index */
    bool                has_broadcast[LG_MAX_DEVICES];
    lg_dedup_entry_t    dedup_slots[LG_NODE_DEDUP_SLOTS];
    lg_dedup_t          dedup;
    lg_node_stats_t     stats;
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

/* Sends TIME_SYNC to one attached device. */
void lg_node_send_time(lg_node_t *n, uint32_t device, uint8_t quality);

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

#ifdef __cplusplus
}
#endif
