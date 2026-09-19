/*
 * hh_service.h - the handheld's network service, and the only interface the UI uses (D27).
 *
 * One task owns Wi-Fi, the node session, and lg_client. The UI reads a status snapshot
 * and a message list, and sends commands; it never touches sockets or lg_client, and the
 * service never touches LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "lg_identity.h"
#include "lg_types.h"

#define HH_MAX_NODES    LG_MAX_NODES
#define HH_NAME_MAX     24
#define HH_SSID_MAX     33
#define HH_PROBLEM_MAX  72
#define HH_MESSAGES     24
#define HH_TEXT_MAX     LG_TEXT_MAX

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
    bool     has_time;      /* the AP holds grid time (its beacon says so) */
} hh_node_seen_t;

typedef struct {
    uint32_t device;
    char     name[HH_NAME_MAX];
    uint16_t node;
    bool     online;
} hh_person_t;

typedef struct {
    uint16_t id;
    char     name[HH_NAME_MAX];
    bool     member;        /* this handheld belongs to the group */
    uint8_t  members;       /* handhelds in the group, the denominator for a delivery count (D42) */
    uint32_t member_devices;   /* bit (device - 1) set for each member, for the group editor (D52) */
} hh_group_t;

/* This handheld and every handheld the grid has told it about: who a group can include (D52). */
typedef struct {
    uint32_t device;
    char     name[HH_NAME_MAX];
} hh_user_t;

#define HH_GROUP_NAME_MAX  15   /* UTF-8 bytes, as LG_GROUP_NAME_MAX */

typedef enum {
    HH_MSG_IN = 0,          /* received */
    HH_MSG_PENDING,         /* ours, no node has taken it yet */
    HH_MSG_ACCEPTED,        /* ours, a node took it */
    HH_MSG_DELIVERED,       /* ours, the recipient's handheld confirmed */
    HH_MSG_REJECTED,        /* ours, the grid rejected it; reject is an lg_ack_status_t */
    HH_MSG_REFUSED,         /* this handheld would not send it; reject is an hh_refuse_t */
    HH_MSG_READ,            /* ours, 1:1, and the recipient's handheld showed it to them */
} hh_msg_state_t;

typedef enum {
    HH_REFUSE_NONE = 0,
    HH_REFUSE_TIME,         /* grid time unset: only urgent broadcasts (D6) */
    HH_REFUSE_FULL,         /* outbox full */
    HH_REFUSE_INVALID,      /* unknown target, empty or bad text, or 1:1 without a key */
    HH_REFUSE_ANNOUNCE,     /* the admin page does not let this handheld announce (D56) */
} hh_refuse_t;

typedef struct {
    uint32_t id;            /* grows with every message this handheld records; 0 is never used */
    uint32_t author;        /* device that wrote it */
    uint32_t target;        /* device, group id, or LG_TARGET_ALL */
    uint8_t  scope;         /* lg_scope_t */
    uint8_t  state;         /* hh_msg_state_t */
    uint8_t  reject;        /* see HH_MSG_REJECTED and HH_MSG_REFUSED */
    bool     mine;
    bool     urgent;
    bool     all_clear;     /* an urgent broadcast standing down its author's earlier SOS (D66) */
    uint32_t grid_time;     /* 0 when grid time was unset */
    uint32_t seq;           /* sequence number: ours when sending, the author's when receiving */
    uint32_t origin_boot;   /* received messages: the author's boot, to report them read */
    bool     read_sent;     /* received 1:1: we have already told the author it was read */
    /*
     * Group and broadcast markers count people instead of naming a state (D42): a 1:1
     * message has one recipient, so its state says everything, but "food is ready" to a
     * group is only meaningful as how many have it and how many have opened it. Both stay
     * 0 for 1:1, where state carries the answer.
     */
    uint8_t  delivered_count;   /* handhelds that confirmed delivery */
    uint8_t  read_count;        /* handhelds that reported showing it to their reader */
    uint32_t read_mask;         /* our own messages: bit per roster user who reported reading it */
    uint16_t len;
    char     text[HH_TEXT_MAX + 1];
} hh_message_t;

typedef struct {
    uint32_t       version;                  /* changes whenever the snapshot is republished */
    uint32_t       messages_version;         /* changes when the message list changes */
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
    bool           time_from_gps;            /* D67: grid_time set, and a GPS keeps it (the grid's, or this handheld's own) */
    char           time_zone[LG_TZ_MAX + 1]; /* D67: the grid's POSIX TZ in use; "" none yet, clocks show UTC */
    bool           may_announce;             /* D56: the admin page lets this handheld announce */
    int            preferred_node;           /* -1 automatic */
    uint8_t        n_nodes;
    hh_node_seen_t nodes[HH_MAX_NODES];
    uint8_t        n_people;
    hh_person_t    people[LG_MAX_DEVICES];
    uint8_t        n_groups;
    hh_group_t     groups[LG_MAX_GROUPS];
    uint32_t       groups_version;           /* changes when any group is made, changed, or removed */
    char           group_problem[HH_PROBLEM_MAX];   /* why the AP refused our last group edit; empty if none */
    uint8_t        n_users;
    hh_user_t      users[LG_MAX_DEVICES];
    uint32_t       free_heap;                /* internal RAM: what runs out (PSRAM is counted apart) */
    uint32_t       min_free_heap;
    uint32_t       psram_total;              /* 0 on a board without PSRAM */
    uint32_t       psram_free;
    uint32_t       psram_min_free;
    uint32_t       positions_version;        /* D65: changes when any position, or this handheld's GPS, changes */
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

/*
 * Queues a text message. scope is LG_SCOPE_DIRECT (target = device index),
 * LG_SCOPE_GROUP (target = group id), or LG_SCOPE_BROADCAST. urgent marks a broadcast
 * that may go out while grid time is unset (decision D6).
 *
 * Returns ESP_OK once queued; the outcome then appears in the message list, because only
 * the network task may touch lg_client. ESP_ERR_INVALID_ARG for text that is empty or too
 * long, ESP_ERR_NO_MEM when the queue is full.
 */
esp_err_t hh_service_send(uint8_t scope, uint32_t target, bool urgent, const char *text);

/* An urgent broadcast marked all clear (D66): "<name> is safe". Receivers take down this handheld's
 * SOS alert and show it calmly instead of as another emergency. Same results as hh_service_send. */
esp_err_t hh_service_send_all_clear(const char *text);

/* Copies one message, 0 being the newest; false when there are not that many. For readers that
 * need one at a time and should not hold a copy of the whole list (the console). */
bool hh_service_message(size_t newest_index, hh_message_t *out);

/* Copies up to max messages, newest first; returns how many were copied. */
size_t hh_service_messages(hh_message_t *out, size_t max);

/* One short sentence for why this handheld may not announce, for the screens to show; empty
 * when it may. Urgent broadcasts always go out (D6, D56). */
const char *hh_announce_problem(const hh_status_t *st);

/* One short sentence for a message's state, including why the grid rejected it or why this
 * handheld would not send it. Used by both the screens and the console, so the wording and
 * the reason codes live in one place. */
const char *hh_message_state_text(const hh_message_t *m);

/* Names the handhelds in a read mask ("Pinky, Bluey"), newest roster names, into out. Returns
 * how many were named; writes an empty string for an empty mask. */
uint8_t hh_service_reader_names(uint32_t mask, char *out, size_t cap);

/* Tells the author that one received message has been shown. Reports each message once. */
void hh_service_mark_read(uint32_t message_id);

/*
 * Groups (D52). Asks the AP to make a group (id 0), change one (name and members replace the
 * old), or remove one (remove true; its messages are deleted on every handheld). members has
 * bit (device - 1) set for each handheld in it; whoever makes a group is always added. Only a
 * member may change or remove a group. The answer arrives as a new groups_version, or as
 * group_problem when the AP refused. ESP_ERR_INVALID_ARG for a name of 0 or more than
 * HH_GROUP_NAME_MAX bytes, ESP_ERR_NO_MEM when the queue is full.
 */
esp_err_t hh_service_edit_group(uint16_t id, const char *name, uint32_t members, bool remove);

/*
 * Renames this handheld (D50): 1..HH_NAME_MAX - 1 bytes of UTF-8. The name is kept in flash and
 * carried to every AP and handheld, now if online or at the next registration. The new name shows
 * in the snapshot once the service task has taken it. ESP_ERR_INVALID_ARG for an empty, too long,
 * or malformed name; ESP_ERR_NO_MEM when the queue is full.
 */
esp_err_t hh_service_set_name(const char *name);

/*
 * Battery (D62), sampled by the service (service/hh_battery.c) every 2 s, filtered over about
 * 40 s, mapped through a LiPo curve, and held in a 2-point deadband. Returns 0..100, or -1 for
 * no badge: the board cannot measure, or the reading is outside what a cell can be. Without a
 * battery fitted it reads the charger's output, about 4.1 to 4.2 V, and says so. Any task; it
 * only copies a published byte, so screens may call it every refresh and repaint on a change.
 */
#define HH_BATTERY_LOW_PERCENT 15   /* at or below: the badge turns the error colour */

int8_t hh_service_battery_percent(void);

/* ---- push-to-talk frames (D61) ----
 *
 * The voice module (main/voice) owns the microphone and speaker; the service only carries its
 * frames. A frame is a voice header and its ADPCM data (lg_body.h), at most HH_VOICE_FRAME_MAX
 * bytes, and it is sent once: no outbox, no retry, no ack.
 */
#define HH_VOICE_FRAME_MAX 410   /* LG_VOICE_HDR_LEN + LG_VOICE_DATA_MAX */

typedef struct {
    /* A frame for this handheld: author, and the conversation it belongs to (scope DIRECT with
     * target = author, or GROUP with the group id). Called on the service task: copy and return. */
    void (*on_frame)(uint32_t author, uint32_t boot, uint8_t scope, uint32_t target, const uint8_t *frame,
                     size_t len);
    /* A talk could not go out: a positive lg_ack_status_t when an AP refused it, a negative
     * lg_err_t when this handheld would not send it (LG_ERR_TIME, LG_ERR_SHORT when offline).
     * Called on the service task. */
    void (*on_refused)(int reason);
} hh_voice_io_t;

/* Registers the voice module's callbacks; io must outlive the service. */
void hh_service_set_voice_io(const hh_voice_io_t *io);

/* Queues one frame for the AP. ESP_ERR_NO_MEM when the queue is full (the frame is dropped),
 * ESP_ERR_INVALID_ARG for a bad scope or length. */
esp_err_t hh_service_voice_send(uint8_t scope, uint32_t target, const uint8_t *frame, size_t len);

/* ---- GPS and positions (D65) ----
 *
 * A handheld with a GPS fitted takes its clock from it while it has a fix, and sends its own
 * position to the grid: at registration, every 30 s, at once after moving more than 25 m, and
 * just before an urgent broadcast. Positions are kept in RAM only. Every call here copies under
 * the service lock, so screens may call them from their own task on every refresh.
 */
#define HH_SUBJECT_MAIN (LG_NODE_ID_BASE | 0u)   /* MAIN's own position */

typedef struct {
    bool     valid;
    int32_t  lat_u;          /* microdegrees */
    int32_t  lon_u;
    uint32_t fix_time;       /* Unix s of the fix */
    uint8_t  sats;
    uint32_t age_s;          /* now - fix_time by this handheld's clock; 0 if unknown */
} hh_position_t;

/* This handheld's own GPS: false when no GPS is fitted or it has no fix. */
bool hh_service_own_position(hh_position_t *out);

/* Last known position of a device (handheld index) or HH_SUBJECT_MAIN; false if unknown. */
bool hh_service_position(uint32_t subject, hh_position_t *out);

/* A GPS is fitted and talking (for the Status screen: "GPS: 7 satellites" / "no fix"). */
bool hh_service_gps(uint8_t *sats, bool *fix);

/* Everything this handheld's GPS reports, for Status. Unknown numbers are HH_GPS_UNKNOWN. */
#define HH_GPS_UNKNOWN 0xFFFFu
typedef struct {
    bool     fitted;        /* a GPS has been heard on this board since boot */
    bool     talking;       /* ...and within the last 5 s */
    bool     fix;           /* a lock: the module reports a valid position now */
    uint8_t  fix_type;      /* 2 2D, 3 3D, 0 unknown */
    bool     corrected;     /* differential (SBAS/WAAS) */
    uint8_t  used;          /* satellites in use */
    uint8_t  in_view;
    uint8_t  tracked;       /* in view with a signal */
    uint8_t  best_snr;      /* dB-Hz */
    uint16_t hdop_c;        /* x100 */
    uint16_t pdop_c;
    bool     has_pos;
    int32_t  lat_u;
    int32_t  lon_u;
    bool     has_alt;
    int32_t  alt_dm;
    uint16_t speed_cms;
    uint16_t course_cd;     /* centidegrees true */
    uint32_t utc;           /* the last fix's UTC, 0 never */
    uint32_t fix_age_ms;    /* UINT32_MAX never */
    uint32_t sentences;
    uint32_t bad;
} hh_gps_info_t;

/* false (and out->fitted false) on a board with no GPS pin or none heard. Any task. */
bool hh_service_gps_info(hh_gps_info_t *out);

/* Before hh_service_start: the board profile's GPS UART pins, LG_PIN_NONE (-1) for none. With no
 * rx pin, or none of this called, nothing about a GPS runs. */
void hh_service_set_gps_pins(int rx_gpio, int tx_gpio);

/*
 * D67: Unix seconds (grid time is UTC) as local wall-clock time in the grid's zone, which the AP
 * sends and this handheld keeps in flash; UTC until one is known (status time_zone ""). Every clock a
 * screen or the console shows goes through this. Any task.
 */
void hh_local_time(uint32_t unix_s, struct tm *out);

/* Where this handheld's clock came from, for the console: "the GPS", "the grid", "kept across a
 * restart", or "not set". */
const char *hh_service_clock_source(void);
