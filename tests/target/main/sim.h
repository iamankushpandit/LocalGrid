/*
 * sim.h - in-memory LocalGrid network for tests.
 *
 * Nodes and clients run the real lg_core and lg_crypto code. Links are FIFO
 * queues, so delivery order is deterministic and nothing recurses.
 * Clients 0..3 are the prototype roster: Dad, Emma, Alex, Ranger.
 * Every node and client owns its roster, as the boards do, and starts with the fixture groups
 * below already replicated (version 1), so scenarios that are not about editing groups can
 * send to them at once (D52: real grids start with no groups).
 */
#pragma once

#include "lg_client.h"
#include "lg_crypto.h"
#include "lg_node.h"

/* Fixture devices and groups. Test names only: the firmware has no built-in groups (D52). */
enum { LG_PROTO_DAD = 1, LG_PROTO_EMMA = 2, LG_PROTO_ALEX = 3, LG_PROTO_RANGER = 4 };
enum { LG_PROTO_FAMILY = 1, LG_PROTO_KIDS = 2, LG_PROTO_LEADERS = 3 };

#define SIM_NODES      3
#define SIM_CLIENTS    4
#define SIM_QUEUE      128
#define SIM_FRAME_MAX  320
#define SIM_CAPTURE    64

typedef enum { SIM_EV_TO_NODE, SIM_EV_TO_CLIENT, SIM_EV_BACKBONE } sim_ev_kind_t;

typedef struct {
    uint8_t  kind;
    uint8_t  a;
    uint8_t  b;
    uint16_t len;
    uint8_t  data[SIM_FRAME_MAX];
} sim_ev_t;

typedef struct sim sim_t;

typedef struct {
    sim_t    *sim;
    uint16_t  index;
    uint32_t  time;          /* this node's grid time; 0 falls back to sim->grid_time */
    uint16_t  time_millis;   /* milliseconds and stratum of the last TIME_SYNC heard */
    uint8_t   time_stratum;
    uint32_t  diag_count;    /* diagnostic echoes reported to this node */
    uint8_t   diag_hops;     /* hops of the last echo */
    uint32_t  grid_state_count;    /* grid state bodies reported to this node */
    uint16_t  grid_state_origin;   /* origin node of the last one */
    uint8_t   grid_state_last[LG_GRID_STATE_MAX];
    size_t    grid_state_len;
    uint32_t  groups_changed;      /* times on_groups_changed was called */
    lg_roster_t roster;
    uint32_t  name_count;    /* names this node was asked to keep (io.on_name) */
    lg_node_t node;
} sim_node_t;

typedef struct {
    sim_t      *sim;
    int         index;
    int         node;             /* attached node, -1 when detached */
    uint32_t    session_device;   /* node-side session state */
    uint32_t    clock;            /* this handheld's wall clock */
    uint32_t    messages;
    uint32_t    key_changes;
    uint32_t    groups_events;      /* LG_CEV_GROUPS */
    uint32_t    group_refusals;     /* LG_CEV_GROUP_REFUSED */
    uint8_t     last_refusal;
    uint16_t    removed[LG_MAX_GROUPS];   /* the last on_groups_removed call */
    size_t      removed_n;
    lg_roster_t roster;
    uint32_t    name_events;      /* LG_CEV_NAME */
    lg_e2e_t    e2e;
    lg_client_t client;
} sim_client_t;

struct sim {
    sim_node_t   nodes[SIM_NODES];
    bool         link[SIM_NODES][SIM_NODES];
    sim_client_t clients[SIM_CLIENTS];
    sim_ev_t    *queue;                  /* SIM_QUEUE events, static like the capture buffer */
    size_t       head;
    size_t       count;
    bool         overflow;
    uint32_t     now_ms;
    uint32_t     grid_time;
    bool         duplicate_backbone;
    sim_ev_t    *capture;                /* SIM_CAPTURE backbone frames, for eavesdropping checks: static,
                                          * so the heap block the simulation needs stays under 136 KB */
    size_t       capture_count;
};

/* Reserves the simulation's memory once at boot. On the classic ESP32 the state needs
 * one block of about 136 KB, and the display driver fragments the heap region that
 * holds it, so the reservation must come before the display starts. */
bool   sim_reserve(void);
sim_t *sim_create(void);
void   sim_destroy(sim_t *s);
void   sim_link(sim_t *s, int a, int b, bool up);
void   sim_attach(sim_t *s, int client, int node);
void   sim_detach(sim_t *s, int client);
bool   sim_pump(sim_t *s);
bool   sim_captured_contains(const sim_t *s, const char *needle);
