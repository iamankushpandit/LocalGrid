/*
 * hh_service.h - the handheld's network service, and the only interface the UI uses (D27).
 *
 * One task owns Wi-Fi, the node session, and lg_client. The UI reads a status snapshot
 * and sends commands; it never touches sockets or lg_client, and the service never
 * touches LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_identity.h"
#include "lg_types.h"

#define HH_MAX_NODES    LG_MAX_NODES
#define HH_NAME_MAX     24
#define HH_SSID_MAX     33
#define HH_PROBLEM_MAX  72

typedef enum {
    HH_LINK_STOPPED = 0,    /* not started, or cannot run; see problem */
    HH_LINK_SEARCHING,      /* scanning for nodes */
    HH_LINK_CONNECTING,     /* joining a node's Wi-Fi and opening the session */
    HH_LINK_REGISTERING,    /* session open, waiting for the node to accept */
    HH_LINK_ONLINE,         /* registered with a node */
} hh_link_t;

typedef struct {
    uint16_t node;
    char     ssid[HH_SSID_MAX];
    int8_t   rssi;
    uint8_t  clients;
    bool     backbone;      /* node has at least one backbone link */
} hh_node_seen_t;

typedef struct {
    uint32_t device;
    char     name[HH_NAME_MAX];
    uint16_t node;
    bool     online;
} hh_person_t;

typedef struct {
    uint32_t       version;                  /* changes whenever the snapshot is republished */
    hh_link_t      link;
    char           problem[HH_PROBLEM_MAX];  /* why the handheld is not online; empty when fine */
    uint32_t       device;
    char           name[HH_NAME_MAX];
    int            node;                     /* node registered with or being joined, -1 none */
    char           node_ssid[HH_SSID_MAX];
    int8_t         rssi;
    uint8_t        ip[4];
    uint32_t       joins;                    /* registrations since boot */
    uint32_t       grid_time;                /* Unix seconds, 0 when not set */
    bool           time_restricted;          /* D6: only receiving and URGENT broadcasts */
    int            preferred_node;           /* -1 automatic */
    uint8_t        n_nodes;
    hh_node_seen_t nodes[HH_MAX_NODES];
    uint8_t        n_people;
    hh_person_t    people[LG_MAX_DEVICES];
    uint32_t       free_heap;
    uint32_t       min_free_heap;
} hh_status_t;

/* Starts the service task. On failure the snapshot carries the problem for the screen. */
esp_err_t hh_service_start(const lg_identity_t *identity);

/* Copies the latest snapshot; safe from any task, including before the service starts. */
void hh_service_status(hh_status_t *out);

/* Uses only this node from now on; -1 returns to automatic selection. */
void hh_service_prefer_node(int node);

/* Scans the grid channel now, to refresh the nodes in range. */
void hh_service_scan_now(void);

/* Drops the node session and joins again from a fresh scan. */
void hh_service_reconnect(void);
