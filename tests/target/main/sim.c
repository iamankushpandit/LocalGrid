#include "sim.h"

#include <stdlib.h>
#include <string.h>

#include "lg_test.h"

static void enqueue(sim_t *s, uint8_t kind, int a, int b, const uint8_t *frame, size_t len)
{
    if (len > SIM_FRAME_MAX || s->count >= SIM_QUEUE) {
        s->overflow = true;
        return;
    }
    sim_ev_t *ev = &s->queue[(s->head + s->count) % SIM_QUEUE];
    ev->kind = kind;
    ev->a = (uint8_t)a;
    ev->b = (uint8_t)b;
    ev->len = (uint16_t)len;
    memcpy(ev->data, frame, len);
    s->count++;
    if (kind == SIM_EV_BACKBONE && s->capture_count < SIM_CAPTURE) {
        s->capture[s->capture_count++] = *ev;
    }
}

/* ---- node io ---- */

static void n_to_client(void *ctx, uint32_t device, const uint8_t *frame, size_t len)
{
    sim_node_t *sn = ctx;
    sim_t *s = sn->sim;
    for (int i = 0; i < SIM_CLIENTS; i++) {
        sim_client_t *c = &s->clients[i];
        if (c->node == sn->index && c->session_device == device) {
            enqueue(s, SIM_EV_TO_CLIENT, sn->index, i, frame, len);
            return;
        }
    }
}

static void n_unicast(void *ctx, uint16_t node, const uint8_t *frame, size_t len)
{
    sim_node_t *sn = ctx;
    if (node < SIM_NODES && sn->sim->link[sn->index][node]) {
        enqueue(sn->sim, SIM_EV_BACKBONE, sn->index, node, frame, len);
    }
}

static void n_flood(void *ctx, uint16_t except, const uint8_t *frame, size_t len)
{
    sim_node_t *sn = ctx;
    for (int j = 0; j < SIM_NODES; j++) {
        if (j != sn->index && j != except && sn->sim->link[sn->index][j]) {
            enqueue(sn->sim, SIM_EV_BACKBONE, sn->index, j, frame, len);
        }
    }
}

static bool n_is_neighbor(void *ctx, uint16_t node)
{
    sim_node_t *sn = ctx;
    return node < SIM_NODES && sn->sim->link[sn->index][node];
}

static uint32_t n_now(void *ctx)
{
    return ((sim_node_t *)ctx)->sim->now_ms;
}

static uint32_t n_grid_time(void *ctx)
{
    sim_node_t *sn = ctx;
    return sn->time != 0 ? sn->time : sn->sim->grid_time;
}

static void n_on_diag(void *ctx, uint16_t origin_node, uint8_t hops, const uint8_t *text, size_t len)
{
    (void)origin_node;
    (void)text;
    (void)len;
    sim_node_t *sn = ctx;
    sn->diag_count++;
    sn->diag_hops = hops;
}

static void n_on_time(void *ctx, uint16_t origin_node, const lg_time_sync_t *t)
{
    (void)origin_node;
    sim_node_t *sn = ctx;
    if (t->grid_time != 0 && t->quality > LG_TIME_UNSET) {
        sn->time = t->grid_time;
        sn->time_millis = t->millis;
        sn->time_stratum = t->stratum;
    }
}

static void n_on_client_time(void *ctx, uint32_t device, uint32_t unix_s)
{
    (void)device;
    sim_node_t *sn = ctx;
    sn->time = unix_s;
    sn->time_millis = 0;
    sn->time_stratum = LG_STRATUM_UNKNOWN;
    sn->client_time_count++;
}

static void n_time_now(void *ctx, lg_time_sync_t *out)
{
    sim_node_t *sn = ctx;
    out->grid_time = sn->time != 0 ? sn->time : sn->sim->grid_time;
    out->millis = 250;   /* a fixed, recognisable fraction for the tests */
    out->stratum = (uint8_t)sn->index;
}

static void n_on_grid_state(void *ctx, uint16_t origin_node, const uint8_t *body, size_t len)
{
    sim_node_t *sn = ctx;
    sn->grid_state_count++;
    sn->grid_state_origin = origin_node;
    sn->grid_state_len = len <= sizeof(sn->grid_state_last) ? len : sizeof(sn->grid_state_last);
    memcpy(sn->grid_state_last, body, sn->grid_state_len);
}

static void n_on_groups_changed(void *ctx)
{
    ((sim_node_t *)ctx)->groups_changed++;
}

/* The fixture table: FAMILY = Dad, Emma, Alex; KIDS = Emma, Alex; LEADERS = Dad, Ranger. */
static void seed_groups(lg_roster_t *r)
{
    static const lg_group_t fixture[] = {
        { LG_PROTO_FAMILY,  (1u << 0) | (1u << 1) | (1u << 2), "FAMILY"  },
        { LG_PROTO_KIDS,    (1u << 1) | (1u << 2),             "KIDS"    },
        { LG_PROTO_LEADERS, (1u << 0) | (1u << 3),             "LEADERS" },
    };
    r->groups.seq = 1;
    r->groups.author = 0;
    r->groups.next_id = 4;
    r->groups.count = 3;
    memcpy(r->groups.groups, fixture, sizeof(fixture));
}

static void n_on_name(void *ctx, const lg_name_t *name)
{
    (void)name;
    ((sim_node_t *)ctx)->name_count++;
}

/* ---- client io ---- */

static bool c_send(void *ctx, const uint8_t *frame, size_t len)
{
    sim_client_t *c = ctx;
    if (c->node < 0) {
        return false;
    }
    enqueue(c->sim, SIM_EV_TO_NODE, c->index, c->node, frame, len);
    return true;
}

static void c_event(void *ctx, const lg_client_event_t *ev)
{
    sim_client_t *c = ctx;
    if (ev->type == LG_CEV_MESSAGE) {
        c->messages++;
    } else if (ev->type == LG_CEV_KEY_CHANGED) {
        c->key_changes++;
    } else if (ev->type == LG_CEV_GROUPS) {
        c->groups_events++;
    } else if (ev->type == LG_CEV_GROUP_REFUSED) {
        c->group_refusals++;
        c->last_refusal = (uint8_t)ev->value;
    } else if (ev->type == LG_CEV_NAME) {
        c->name_events++;
    } else if (ev->type == LG_CEV_VOICE_REFUSED) {
        c->voice_refusals++;
        c->last_voice_refusal = (uint8_t)ev->value;
    } else if (ev->type == LG_CEV_POSITION) {
        c->position_events++;
        c->last_position = ev->value;
    }
}

static void c_voice(void *ctx, const lg_env_t *env, const uint8_t *payload, size_t len)
{
    sim_client_t *c = ctx;
    c->voice_frames++;
    c->voice_author = env->origin_id;
    c->voice_scope = env->scope;
    c->voice_len = (uint16_t)len;
    memcpy(c->voice_last, payload, len < sizeof(c->voice_last) ? len : sizeof(c->voice_last));
}

static void c_groups_removed(void *ctx, const uint16_t *removed, size_t n)
{
    sim_client_t *c = ctx;
    c->removed_n = n;
    memcpy(c->removed, removed, n * sizeof(removed[0]));
}

static uint32_t c_now(void *ctx)
{
    return ((sim_client_t *)ctx)->sim->now_ms;
}

static uint32_t c_local_time(void *ctx)
{
    return ((sim_client_t *)ctx)->clock;
}

static void c_set_time(void *ctx, uint32_t t)
{
    ((sim_client_t *)ctx)->clock = t;
}

static int c_seal(void *ctx, uint32_t peer, const uint8_t *pub, const uint8_t *nonce,
                  const uint8_t *aad, size_t aad_len, const uint8_t *pt, size_t pt_len, uint8_t *out)
{
    return lg_e2e_seal(&((sim_client_t *)ctx)->e2e, peer, pub, nonce, aad, aad_len, pt, pt_len, out);
}

static int c_open(void *ctx, uint32_t peer, const uint8_t *pub, const uint8_t *nonce,
                  const uint8_t *aad, size_t aad_len, const uint8_t *ct, size_t ct_len, uint8_t *out)
{
    return lg_e2e_open(&((sim_client_t *)ctx)->e2e, peer, pub, nonce, aad, aad_len, ct, ct_len, out);
}

static sim_ev_t s_capture[SIM_CAPTURE];   /* one simulation at a time, so one capture buffer */
static sim_ev_t s_queue[SIM_QUEUE];       /* and one event queue */
static sim_t *s_reserved;       /* one simulation exists at a time; tests create and destroy in turn */
static bool   s_reserved_in_use;

bool sim_reserve(void)
{
    if (s_reserved == NULL) {
        s_reserved = calloc(1, sizeof(sim_t));
    }
    return s_reserved != NULL;
}

sim_t *sim_create(void)
{
    sim_t *s;
    if (s_reserved != NULL && !s_reserved_in_use) {
        s = s_reserved;
        s_reserved_in_use = true;
        memset(s, 0, sizeof(*s));
    } else {
        s = calloc(1, sizeof(sim_t));
    }
    if (s == NULL) {
        printf("FAIL sim_create: out of memory (%u bytes)\n", (unsigned)sizeof(sim_t));
        lg_failures++;
        return NULL;
    }
    s->capture = s_capture;
    s->queue = s_queue;
    for (int i = 0; i < SIM_NODES; i++) {
        sim_node_t *sn = &s->nodes[i];
        sn->sim = s;
        sn->index = (uint16_t)i;
        lg_node_io_t io = {
            .ctx = sn,
            .to_client = n_to_client,
            .backbone_unicast = n_unicast,
            .backbone_flood = n_flood,
            .is_neighbor = n_is_neighbor,
            .now_ms = n_now,
            .grid_time = n_grid_time,
            .on_diag = n_on_diag,
            .on_time = n_on_time,
            .time_now = n_time_now,
            .on_grid_state = n_on_grid_state,
            .on_groups_changed = n_on_groups_changed,
            .on_client_time = n_on_client_time,
            .on_name = n_on_name,
        };
        lg_roster_init_prototype(&sn->roster);
        seed_groups(&sn->roster);
        lg_node_init(&sn->node, (uint16_t)i, 1, &sn->roster, &io);
    }

    for (int i = 0; i < SIM_CLIENTS; i++) {
        sim_client_t *c = &s->clients[i];
        c->sim = s;
        c->index = i;
        c->node = -1;
        lg_roster_init_prototype(&c->roster);
        seed_groups(&c->roster);
        uint32_t device = c->roster.users[i].device;
        uint8_t priv[LG_X25519_LEN], pub[LG_X25519_LEN];
        CHECK_EQ(lg_x25519_keypair(priv, pub), 0);
        CHECK_EQ(lg_e2e_init(&c->e2e, device, priv), 0);
        lg_secure_zero(priv, sizeof(priv));
        lg_client_io_t io = {
            .ctx = c,
            .send = c_send,
            .on_event = c_event,
            .now_ms = c_now,
            .local_time = c_local_time,
            .set_time = c_set_time,
            .seal = c_seal,
            .open = c_open,
            .on_groups_removed = c_groups_removed,
            .on_voice = c_voice,
        };
        lg_client_init(&c->client, device, 1, c->e2e.pub, &c->roster, &io);
    }
    return s;
}

void sim_destroy(sim_t *s)
{
    if (s != NULL) {
        lg_secure_zero(s, sizeof(*s));
        if (s == s_reserved) {
            s_reserved_in_use = false;
        } else {
            free(s);
        }
    }
}

bool sim_pump(sim_t *s)
{
    static sim_ev_t ev;   /* copied out so handlers can enqueue freely */
    int guard = 0;
    while (s->count > 0) {
        if (++guard > 20000) {
            printf("FAIL sim_pump: event loop did not settle\n");
            return false;
        }
        ev = s->queue[s->head];
        s->head = (s->head + 1) % SIM_QUEUE;
        s->count--;
        switch (ev.kind) {
        case SIM_EV_TO_NODE: {
            sim_client_t *c = &s->clients[ev.a];
            if (c->node == ev.b) {
                lg_node_on_session_frame(&s->nodes[ev.b].node, &c->session_device, ev.data, ev.len);
            }
            break;
        }
        case SIM_EV_TO_CLIENT: {
            sim_client_t *c = &s->clients[ev.b];
            if (c->node == ev.a) {
                lg_client_on_frame(&c->client, ev.data, ev.len);
            }
            break;
        }
        case SIM_EV_BACKBONE:
            if (s->link[ev.a][ev.b]) {
                lg_node_on_backbone_frame(&s->nodes[ev.b].node, ev.a, ev.data, ev.len);
                if (s->duplicate_backbone) {
                    lg_node_on_backbone_frame(&s->nodes[ev.b].node, ev.a, ev.data, ev.len);
                }
            }
            break;
        default:
            break;
        }
    }
    return !s->overflow;
}

void sim_link(sim_t *s, int a, int b, bool up)
{
    s->link[a][b] = up;
    s->link[b][a] = up;
    if (up) {
        lg_node_on_neighbor_up(&s->nodes[a].node, (uint16_t)b);
        lg_node_on_neighbor_up(&s->nodes[b].node, (uint16_t)a);
    }
    (void)sim_pump(s);
}

void sim_attach(sim_t *s, int client, int node)
{
    sim_client_t *c = &s->clients[client];
    c->node = node;
    c->session_device = 0;
    lg_client_connected(&c->client);
    (void)sim_pump(s);
}

void sim_detach(sim_t *s, int client)
{
    sim_client_t *c = &s->clients[client];
    if (c->node >= 0 && c->session_device != 0) {
        lg_node_on_session_closed(&s->nodes[c->node].node, c->session_device);
    }
    c->node = -1;
    c->session_device = 0;
    lg_client_disconnected(&c->client);
    (void)sim_pump(s);
}

bool sim_captured_contains(const sim_t *s, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i < s->capture_count; i++) {
        const sim_ev_t *ev = &s->capture[i];
        for (size_t k = 0; k + n <= ev->len; k++) {
            if (memcmp(ev->data + k, needle, n) == 0) {
                return true;
            }
        }
    }
    return false;
}
