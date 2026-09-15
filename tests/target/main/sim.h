/*
 * sim.h - in-memory LocalGrid network for tests.
 *
 * Nodes and clients run the real lg_core and lg_crypto code. Links are FIFO
 * queues, so delivery order is deterministic and nothing recurses.
 * Clients 0..3 are the prototype roster: Dad, Emma, Alex, Ranger.
 */
#pragma once

#include "lg_client.h"
#include "lg_crypto.h"
#include "lg_node.h"

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
    uint32_t  diag_count;    /* diagnostic echoes reported to this node */
    uint8_t   diag_hops;     /* hops of the last echo */
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
    lg_e2e_t    e2e;
    lg_client_t client;
} sim_client_t;

struct sim {
    sim_node_t   nodes[SIM_NODES];
    bool         link[SIM_NODES][SIM_NODES];
    sim_client_t clients[SIM_CLIENTS];
    sim_ev_t     queue[SIM_QUEUE];
    size_t       head;
    size_t       count;
    bool         overflow;
    uint32_t     now_ms;
    uint32_t     grid_time;
    bool         duplicate_backbone;
    sim_ev_t     capture[SIM_CAPTURE];   /* backbone frames, for eavesdropping checks */
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
