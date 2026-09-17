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

static void n_on_time(void *ctx, uint16_t origin_node, uint32_t grid_time, uint8_t quality)
{
    (void)origin_node;
    sim_node_t *sn = ctx;
    if (grid_time != 0 && quality > LG_TIME_UNSET) {
        sn->time = grid_time;
    }
}

static void n_on_grid_state(void *ctx, uint16_t origin_node, const uint8_t *body, size_t len)
{
    sim_node_t *sn = ctx;
    sn->grid_state_count++;
    sn->grid_state_origin = origin_node;
    sn->grid_state_len = len <= sizeof(sn->grid_state_last) ? len : sizeof(sn->grid_state_last);
    memcpy(sn->grid_state_last, body, sn->grid_state_len);
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
    }
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
    const lg_roster_t *roster = lg_roster_prototype();

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
            .on_grid_state = n_on_grid_state,
        };
        lg_node_init(&sn->node, (uint16_t)i, 1, roster, &io);
    }

    for (int i = 0; i < SIM_CLIENTS; i++) {
        sim_client_t *c = &s->clients[i];
        c->sim = s;
        c->index = i;
        c->node = -1;
        uint32_t device = roster->users[i].device;
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
        };
        lg_client_init(&c->client, device, 1, c->e2e.pub, roster, &io);
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
