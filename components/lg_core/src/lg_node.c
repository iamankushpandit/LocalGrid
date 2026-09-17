#include "lg_node.h"

#include <string.h>

/* ---- small helpers ------------------------------------------------------ */

static uint32_t absdiff(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static uint32_t node_now_ms(const lg_node_t *n)
{
    return n->io.now_ms ? n->io.now_ms(n->io.ctx) : 0;
}

static uint32_t node_grid_time(const lg_node_t *n)
{
    return n->io.grid_time ? n->io.grid_time(n->io.ctx) : 0;
}

static void time_sync_now(const lg_node_t *n, uint8_t quality, lg_time_sync_t *t)
{
    memset(t, 0, sizeof(*t));
    if (n->io.time_now != NULL) {
        n->io.time_now(n->io.ctx, t);
    } else {
        t->grid_time = node_grid_time(n);
        t->stratum = LG_STRATUM_UNKNOWN;
    }
    t->quality = quality;
}

static lg_presence_entry_t *presence_find(lg_node_t *n, uint32_t device)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (n->presence[i].in_use && n->presence[i].device == device) {
            return &n->presence[i];
        }
    }
    return NULL;
}

static lg_presence_entry_t *presence_get(lg_node_t *n, uint32_t device)
{
    lg_presence_entry_t *p = presence_find(n, device);
    if (p != NULL) {
        return p;
    }
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (!n->presence[i].in_use) {
            p = &n->presence[i];
            memset(p, 0, sizeof(*p));
            p->in_use = 1;
            p->device = device;
            p->node   = LG_NODE_NONE;
            p->state  = LG_PRES_OFFLINE;
            return p;
        }
    }
    return NULL;
}

const lg_presence_entry_t *lg_node_presence(const lg_node_t *n, uint32_t device)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (n->presence[i].in_use && n->presence[i].device == device) {
            return &n->presence[i];
        }
    }
    return NULL;
}

static bool is_local_online(const lg_node_t *n, const lg_presence_entry_t *p)
{
    return p != NULL && p->state == LG_PRES_ONLINE && p->node == n->self;
}

static void env_from_node(lg_node_t *n, lg_env_t *e, uint8_t type, uint8_t scope, uint32_t target)
{
    memset(e, 0, sizeof(*e));
    e->type        = type;
    e->scope       = scope;
    e->target      = target;
    e->origin_id   = lg_node_origin_id(n->self);
    e->origin_boot = n->boot;
    e->origin_seq  = ++n->seq;
    e->grid_time   = node_grid_time(n);
    e->origin_node = n->self;
}

static void send_client(lg_node_t *n, uint32_t device, lg_env_t *e, const uint8_t *body, size_t len)
{
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(e, body, len, buf, sizeof(buf));
    if (flen > 0 && n->io.to_client != NULL) {
        n->io.to_client(n->io.ctx, device, buf, (size_t)flen);
    }
}

static void flood_frame(lg_node_t *n, uint16_t except, const uint8_t *frame, size_t len)
{
    if (n->io.backbone_flood != NULL) {
        n->io.backbone_flood(n->io.ctx, except, frame, len);
    }
    n->stats.forwarded++;
}

static size_t presence_body(const lg_presence_entry_t *p, uint8_t *out)
{
    lg_presence_t pr;
    pr.device = p->device;
    pr.node   = p->node;
    pr.epoch  = p->epoch;
    pr.state  = p->state;
    memcpy(pr.pubkey, p->pubkey, LG_PUBKEY_LEN);
    return lg_presence_enc(&pr, out);
}

static void send_presence_to_client(lg_node_t *n, uint32_t device, const lg_presence_entry_t *p)
{
    uint8_t body[LG_PRESENCE_LEN];
    size_t blen = presence_body(p, body);
    lg_env_t e;
    env_from_node(n, &e, LG_T_PRESENCE_UPDATE, LG_SCOPE_SYSTEM, p->device);
    send_client(n, device, &e, body, blen);
}

/* Pushes a presence change to every locally attached device except except_device. */
static void notify_local_clients(lg_node_t *n, const lg_presence_entry_t *p, uint32_t except_device)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        const lg_presence_entry_t *q = &n->presence[i];
        if (q->in_use && is_local_online(n, q) && q->device != except_device) {
            send_presence_to_client(n, q->device, p);
        }
    }
}

static void flood_presence(lg_node_t *n, const lg_presence_entry_t *p)
{
    uint8_t body[LG_PRESENCE_LEN];
    size_t blen = presence_body(p, body);
    lg_env_t e;
    env_from_node(n, &e, LG_T_PRESENCE_UPDATE, LG_SCOPE_SYSTEM, p->device);
    e.ttl = LG_TTL_DEFAULT;
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, blen, buf, sizeof(buf));
    if (flen < 0) {
        return;
    }
    (void)lg_dedup_mark(&n->dedup, e.origin_id, e.origin_boot, e.origin_seq);
    flood_frame(n, LG_NODE_NONE, buf, (size_t)flen);
}

static void send_time_to_client(lg_node_t *n, uint32_t device, uint8_t quality)
{
    lg_time_sync_t t;
    time_sync_now(n, quality, &t);
    uint8_t body[LG_TIME_SYNC_LEN];
    size_t blen = lg_time_sync_enc(&t, body);
    lg_env_t e;
    env_from_node(n, &e, LG_T_TIME_SYNC, LG_SCOPE_SYSTEM, device);
    send_client(n, device, &e, body, blen);
}

static void push_time_to_local_clients(lg_node_t *n, uint8_t quality)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        const lg_presence_entry_t *q = &n->presence[i];
        if (q->in_use && is_local_online(n, q)) {
            send_time_to_client(n, q->device, quality);
        }
    }
}

static void send_groups_to_client(lg_node_t *n, uint32_t device)
{
    uint8_t body[LG_GROUPS_MAX_LEN];
    size_t blen = lg_groups_enc(&n->roster->groups, body);
    lg_env_t e;
    env_from_node(n, &e, LG_T_GROUPS, LG_SCOPE_SYSTEM, device);
    send_client(n, device, &e, body, blen);
}

static void push_groups_to_local_clients(lg_node_t *n)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        const lg_presence_entry_t *q = &n->presence[i];
        if (q->in_use && is_local_online(n, q)) {
            send_groups_to_client(n, q->device);
        }
    }
}

/* Floods a node-authored frame (TTL = default) after marking it seen locally. */
static int flood_new(lg_node_t *n, uint8_t type, const uint8_t *body, size_t blen)
{
    lg_env_t e;
    env_from_node(n, &e, type, LG_SCOPE_SYSTEM, 0);
    e.ttl = LG_TTL_DEFAULT;
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, blen, buf, sizeof(buf));
    if (flen < 0) {
        return flen;
    }
    (void)lg_dedup_mark(&n->dedup, e.origin_id, e.origin_boot, e.origin_seq);
    flood_frame(n, LG_NODE_NONE, buf, (size_t)flen);
    return LG_OK;
}

static void reply_ack(lg_node_t *n, const lg_env_t *orig, uint8_t status)
{
    lg_msg_ack_t a = {
        .author = orig->origin_id,
        .boot   = orig->origin_boot,
        .seq    = orig->origin_seq,
        .status = status,
    };
    uint8_t body[LG_MSG_ACK_LEN];
    size_t blen = lg_msg_ack_enc(&a, body);
    lg_env_t e;
    env_from_node(n, &e, LG_T_MSG_ACK, LG_SCOPE_DIRECT, orig->origin_id);
    send_client(n, orig->origin_id, &e, body, blen);
}

/* Re-emits a backbone frame with TTL - 1 to every neighbor except the sender. */
static void forward(lg_node_t *n, const lg_env_t *e, const uint8_t *body, uint16_t from_node)
{
    if (e->ttl <= 1) {
        return;
    }
    lg_env_t f = *e;
    f.ttl = (uint8_t)(e->ttl - 1);
    f.flags |= LG_FLAG_RELAYED;
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(&f, body, e->body_len, buf, sizeof(buf));
    if (flen > 0) {
        flood_frame(n, from_node, buf, (size_t)flen);
    }
}

static bool direct_body_ok(const lg_env_t *e)
{
    return (e->flags & LG_FLAG_E2E_PAYLOAD) != 0 &&
           e->body_len >= 1u + LG_AEAD_TAG_LEN &&
           e->body_len <= LG_DIRECT_BODY_MAX;
}

static bool plain_body_ok(const lg_env_t *e, const uint8_t *body)
{
    return (e->flags & LG_FLAG_E2E_PAYLOAD) == 0 && lg_text_valid(body, e->body_len);
}

/* ---- names (D50) --------------------------------------------------------- */

static lg_name_t *name_slot(lg_node_t *n, uint32_t device)
{
    int ui = lg_roster_user_index(n->roster, device);
    return ui >= 0 && ui < (int)LG_MAX_DEVICES ? &n->names[ui] : NULL;
}

/* Keeps name if it is newer than the one held. Returns true when it was taken. */
static bool name_take(lg_node_t *n, const lg_name_t *name)
{
    lg_name_t *slot = name_slot(n, name->device);
    if (slot == NULL || name->version <= slot->version) {
        return false;
    }
    *slot = *name;
    return true;
}

static void send_name_to_client(lg_node_t *n, uint32_t device, const lg_name_t *name)
{
    uint8_t body[LG_NAME_LEN_MAX];
    size_t blen = lg_name_enc(name, body);
    lg_env_t e;
    env_from_node(n, &e, LG_T_NAME, LG_SCOPE_SYSTEM, name->device);
    send_client(n, device, &e, body, blen);
}

static void push_name_to_local_clients(lg_node_t *n, const lg_name_t *name)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        const lg_presence_entry_t *q = &n->presence[i];
        if (q->in_use && is_local_online(n, q)) {
            send_name_to_client(n, q->device, name);
        }
    }
}

static void flood_name(lg_node_t *n, const lg_name_t *name)
{
    uint8_t body[LG_NAME_LEN_MAX];
    size_t blen = lg_name_enc(name, body);
    lg_env_t e;
    env_from_node(n, &e, LG_T_NAME, LG_SCOPE_SYSTEM, name->device);
    e.ttl = LG_TTL_DEFAULT;
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, blen, buf, sizeof(buf));
    if (flen < 0) {
        return;
    }
    (void)lg_dedup_mark(&n->dedup, e.origin_id, e.origin_boot, e.origin_seq);
    flood_frame(n, LG_NODE_NONE, buf, (size_t)flen);
}

static void handle_client_name(lg_node_t *n, uint32_t session_device, const lg_env_t *e, const uint8_t *body)
{
    lg_name_t name;
    if (e->scope != LG_SCOPE_SYSTEM || !lg_name_dec(body, e->body_len, &name)) {
        n->stats.malformed++;
        return;
    }
    if (name.device != session_device || lg_roster_user(n->roster, name.device) == NULL) {
        n->stats.rejected++;   /* a handheld names only itself */
        return;
    }
    if (name_take(n, &name)) {
        if (n->io.on_name != NULL) {
            n->io.on_name(n->io.ctx, &name);
        }
        flood_name(n, &name);
        push_name_to_local_clients(n, &name);
        return;
    }
    /* Not newer. If the grid holds a newer one (the handheld lost its flash), hand it back. */
    const lg_name_t *held = name_slot(n, name.device);
    if (held != NULL && held->version > name.version) {
        send_name_to_client(n, session_device, held);
    }
}

/* ---- delivery ----------------------------------------------------------- */

static void deliver_direct(lg_node_t *n, const lg_env_t *e, const uint8_t *body,
                           const uint8_t *frame, size_t len, uint16_t from_node)
{
    lg_presence_entry_t *p = presence_find(n, e->target);
    if (is_local_online(n, p)) {
        n->io.to_client(n->io.ctx, e->target, frame, len);
        n->stats.delivered_local++;
        return;
    }
    if (from_node == LG_NODE_NONE) {
        if (p != NULL && p->state == LG_PRES_ONLINE && n->io.is_neighbor != NULL &&
            n->io.is_neighbor(n->io.ctx, p->node) && n->io.backbone_unicast != NULL) {
            n->io.backbone_unicast(n->io.ctx, p->node, frame, len);
            n->stats.forwarded++;
        } else {
            flood_frame(n, LG_NODE_NONE, frame, len);
        }
        return;
    }
    forward(n, e, body, from_node);
}

static void deliver_fanout(lg_node_t *n, const lg_env_t *e, const uint8_t *body,
                           const uint8_t *frame, size_t len, uint16_t from_node)
{
    const lg_roster_t *r = n->roster;
    for (size_t i = 0; i < r->n_users; i++) {
        uint32_t dev = r->users[i].device;
        if (dev == e->origin_id) {
            continue;
        }
        if (e->scope == LG_SCOPE_GROUP && !lg_roster_is_member(r, dev, (uint16_t)e->target)) {
            continue;
        }
        if (is_local_online(n, presence_find(n, dev))) {
            n->io.to_client(n->io.ctx, dev, frame, len);
            n->stats.delivered_local++;
        }
    }
    if (from_node == LG_NODE_NONE) {
        flood_frame(n, LG_NODE_NONE, frame, len);
    } else {
        forward(n, e, body, from_node);
    }
}

/* ---- client-originated traffic ----------------------------------------- */

static uint8_t validate_text(lg_node_t *n, const lg_env_t *e, const uint8_t *body)
{
    const lg_roster_t *r = n->roster;

    if (e->scope == LG_SCOPE_DIRECT) {
        if (!direct_body_ok(e)) {
            return LG_ACK_REJ_INVALID;      /* 1:1 text must be end-to-end encrypted */
        }
        if (lg_roster_user(r, e->target) == NULL || e->target == e->origin_id) {
            return LG_ACK_REJ_UNKNOWN_TARGET;
        }
    } else {
        if (!plain_body_ok(e, body)) {
            return LG_ACK_REJ_INVALID;
        }
        if (e->scope == LG_SCOPE_GROUP) {
            if (e->target > 0xFFFFu || lg_roster_group_index(r, (uint16_t)e->target) < 0) {
                return LG_ACK_REJ_UNKNOWN_TARGET;
            }
            if (!lg_roster_is_member(r, e->origin_id, (uint16_t)e->target)) {
                return LG_ACK_REJ_NOT_MEMBER;
            }
        }
    }

    /* Owner rule: wrong or unset time allows only URGENT broadcasts. */
    bool urgent_broadcast = e->scope == LG_SCOPE_BROADCAST && (e->flags & LG_FLAG_URGENT) != 0;
    if (!urgent_broadcast) {
        uint32_t now = node_grid_time(n);
        if (now == 0 || e->grid_time == 0 || absdiff(now, e->grid_time) > LG_TIME_TOLERANCE_S) {
            return LG_ACK_REJ_TIME;
        }
    }

    if (e->scope == LG_SCOPE_BROADCAST) {
        int ui = lg_roster_user_index(r, e->origin_id);
        if (ui < 0 || ui >= (int)LG_MAX_DEVICES) {
            return LG_ACK_REJ_INVALID;
        }
        if (!urgent_broadcast && !lg_roster_may_announce(r, e->origin_id)) {
            return LG_ACK_REJ_NOT_ALLOWED;   /* the admin page decides who may announce (D56) */
        }
        uint32_t interval = urgent_broadcast ? LG_URGENT_INTERVAL_MS : LG_BROADCAST_INTERVAL_MS;
        if (n->has_broadcast[ui] && node_now_ms(n) - n->last_broadcast_ms[ui] < interval) {
            return LG_ACK_REJ_RATE;
        }
    }

    if (e->scope == LG_SCOPE_DIRECT) {
        const lg_presence_entry_t *p = presence_find(n, e->target);
        if (p == NULL || p->state != LG_PRES_ONLINE) {
            return LG_ACK_REJ_OFFLINE;      /* no store-and-forward in the prototype */
        }
    }
    return 0;
}

static void handle_client_text(lg_node_t *n, const lg_env_t *e, const uint8_t *body)
{
    lg_dedup_result_t d = lg_dedup_check(&n->dedup, e->origin_id, e->origin_boot, e->origin_seq);
    if (d == LG_DEDUP_DUPLICATE) {
        reply_ack(n, e, LG_ACK_ACCEPTED);   /* retransmission: already taken */
        n->stats.duplicates++;
        return;
    }
    if (d == LG_DEDUP_STALE) {
        n->stats.rejected++;
        return;
    }

    uint8_t status = validate_text(n, e, body);
    if (status != 0) {
        reply_ack(n, e, status);
        n->stats.rejected++;
        return;
    }

    (void)lg_dedup_mark(&n->dedup, e->origin_id, e->origin_boot, e->origin_seq);
    if (e->scope == LG_SCOPE_BROADCAST) {
        int ui = lg_roster_user_index(n->roster, e->origin_id);
        n->last_broadcast_ms[ui] = node_now_ms(n);
        n->has_broadcast[ui] = true;
    }
    reply_ack(n, e, LG_ACK_ACCEPTED);

    lg_env_t f = *e;
    f.ttl = LG_TTL_DEFAULT;
    f.origin_node = n->self;
    f.flags &= (uint16_t)~LG_FLAG_RELAYED;
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(&f, body, e->body_len, buf, sizeof(buf));
    if (flen < 0) {
        return;
    }
    if (f.scope == LG_SCOPE_DIRECT) {
        deliver_direct(n, &f, body, buf, (size_t)flen, LG_NODE_NONE);
    } else {
        deliver_fanout(n, &f, body, buf, (size_t)flen, LG_NODE_NONE);
    }
}

static void handle_client_ack(lg_node_t *n, const lg_env_t *e, const uint8_t *body)
{
    lg_msg_ack_t a;
    if (!lg_msg_ack_dec(body, e->body_len, &a) || a.author != e->target ||
        (a.status != LG_ACK_DELIVERED && a.status != LG_ACK_READ) ||
        lg_roster_user(n->roster, e->target) == NULL) {
        n->stats.malformed++;
        return;
    }
    if (lg_dedup_mark(&n->dedup, e->origin_id, e->origin_boot, e->origin_seq) != LG_DEDUP_NEW) {
        n->stats.duplicates++;
        return;
    }
    lg_env_t f = *e;
    f.ttl = LG_TTL_DEFAULT;
    f.origin_node = n->self;
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(&f, body, e->body_len, buf, sizeof(buf));
    if (flen > 0) {
        deliver_direct(n, &f, body, buf, (size_t)flen, LG_NODE_NONE);
    }
}

/* The table changed on this node: save it, then send it everywhere it has to go. */
static void groups_changed(lg_node_t *n)
{
    if (n->io.on_groups_changed != NULL) {
        n->io.on_groups_changed(n->io.ctx);
    }
    (void)lg_node_announce_groups(n);
    push_groups_to_local_clients(n);
}

static void handle_client_group_edit(lg_node_t *n, const lg_env_t *e, const uint8_t *body)
{
    lg_group_edit_t edit;
    if (e->scope != LG_SCOPE_SYSTEM || !lg_group_edit_dec(body, e->body_len, &edit)) {
        n->stats.malformed++;
        return;
    }
    lg_dedup_result_t d = lg_dedup_check(&n->dedup, e->origin_id, e->origin_boot, e->origin_seq);
    if (d != LG_DEDUP_NEW) {
        n->stats.duplicates++;   /* applied once already; the table it made has been sent */
        return;
    }
    uint8_t status = lg_groups_apply_edit(n->roster, e->origin_id, &edit, n->self);
    if (status != 0) {
        reply_ack(n, e, status);
        n->stats.rejected++;
        return;
    }
    (void)lg_dedup_mark(&n->dedup, e->origin_id, e->origin_boot, e->origin_seq);
    reply_ack(n, e, LG_ACK_ACCEPTED);
    groups_changed(n);
}

/* ---- public API --------------------------------------------------------- */

void lg_node_init(lg_node_t *n, uint16_t self, uint32_t boot, lg_roster_t *roster, const lg_node_io_t *io)
{
    memset(n, 0, sizeof(*n));
    n->self   = self;
    n->boot   = boot;
    n->roster = roster;
    n->io     = *io;
    lg_dedup_init(&n->dedup, n->dedup_slots, LG_NODE_DEDUP_SLOTS);
}

void lg_node_on_session_frame(lg_node_t *n, uint32_t *session_device, const uint8_t *frame, size_t len)
{
    lg_env_t e;
    if (lg_env_decode(frame, len, &e) != LG_OK) {
        n->stats.malformed++;
        return;
    }
    const uint8_t *body = lg_frame_body(frame);

    if (*session_device == 0) {
        lg_register_t reg;
        if (e.type != LG_T_REGISTER || !lg_register_dec(body, e.body_len, &reg) ||
            reg.device != e.origin_id || reg.device == 0 || (reg.device & LG_NODE_ID_BASE) != 0 ||
            lg_roster_user(n->roster, reg.device) == NULL) {
            n->stats.rejected++;
            return;
        }
        lg_presence_entry_t *p = presence_get(n, reg.device);
        if (p == NULL) {
            n->stats.rejected++;
            return;
        }
        *session_device = reg.device;
        p->node  = n->self;
        p->epoch = reg.attach_epoch;
        p->state = LG_PRES_ONLINE;
        memcpy(p->pubkey, reg.pubkey, LG_PUBKEY_LEN);

        /* Time is sticky (D48, D53): an AP that restarted with no clock takes it back from a
         * handheld that kept running. A handheld reports 0 unless the grid set its clock. */
        if (reg.client_time != 0 && node_grid_time(n) == 0 && n->io.on_client_time != NULL) {
            n->io.on_client_time(n->io.ctx, reg.device, reg.client_time);
        }

        lg_register_ack_t ack = {
            .node = n->self,
            .status = LG_REG_OK,
            .grid_time = node_grid_time(n),
            .roster_version = n->roster->version,
        };
        uint8_t abody[LG_REGISTER_ACK_LEN];
        size_t alen = lg_register_ack_enc(&ack, abody);
        lg_env_t ae;
        env_from_node(n, &ae, LG_T_REGISTER_ACK, LG_SCOPE_SYSTEM, reg.device);
        send_client(n, reg.device, &ae, abody, alen);
        send_groups_to_client(n, reg.device);

        for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
            if (n->presence[i].in_use) {
                send_presence_to_client(n, reg.device, &n->presence[i]);
            }
        }
        for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
            if (n->names[i].version != 0) {
                send_name_to_client(n, reg.device, &n->names[i]);
            }
        }
        flood_presence(n, p);
        notify_local_clients(n, p, reg.device);
        return;
    }

    if (e.origin_id != *session_device) {
        n->stats.rejected++;   /* author spoofing on an established session */
        return;
    }
    n->stats.rx_client++;

    switch (e.type) {
    case LG_T_PING: {
        lg_env_t pe;
        env_from_node(n, &pe, LG_T_PONG, LG_SCOPE_SYSTEM, *session_device);
        send_client(n, *session_device, &pe, NULL, 0);
        break;
    }
    case LG_T_TEXT:
        handle_client_text(n, &e, body);
        break;
    case LG_T_MSG_ACK:
        handle_client_ack(n, &e, body);
        break;
    case LG_T_GROUP_EDIT:
        handle_client_group_edit(n, &e, body);
        break;
    case LG_T_NAME:
        handle_client_name(n, *session_device, &e, body);
        break;
    default:
        break;  /* unknown or not client-originated: ignored */
    }
}

void lg_node_on_session_closed(lg_node_t *n, uint32_t device)
{
    lg_presence_entry_t *p = presence_find(n, device);
    if (is_local_online(n, p)) {
        p->state = LG_PRES_OFFLINE;
        flood_presence(n, p);
        notify_local_clients(n, p, device);
    }
}

static void apply_presence(lg_node_t *n, const lg_presence_t *pr)
{
    if (pr->device == 0 || lg_roster_user(n->roster, pr->device) == NULL) {
        return;
    }
    lg_presence_entry_t *p = presence_get(n, pr->device);
    if (p == NULL) {
        return;
    }
    if (pr->epoch > p->epoch || (pr->epoch == p->epoch && pr->node == p->node)) {
        p->epoch = pr->epoch;
        p->node  = pr->node;
        p->state = pr->state;
        memcpy(p->pubkey, pr->pubkey, LG_PUBKEY_LEN);
        notify_local_clients(n, p, 0);
    }
}

void lg_node_on_backbone_frame(lg_node_t *n, uint16_t from_node, const uint8_t *frame, size_t len)
{
    lg_env_t e;
    if (lg_env_decode(frame, len, &e) != LG_OK) {
        n->stats.malformed++;
        return;
    }
    n->stats.rx_backbone++;
    if (lg_dedup_mark(&n->dedup, e.origin_id, e.origin_boot, e.origin_seq) != LG_DEDUP_NEW) {
        n->stats.duplicates++;
        return;
    }
    const uint8_t *body = lg_frame_body(frame);

    switch (e.type) {
    case LG_T_PRESENCE_UPDATE: {
        lg_presence_t pr;
        if (!lg_presence_dec(body, e.body_len, &pr)) {
            n->stats.malformed++;
            break;
        }
        apply_presence(n, &pr);
        forward(n, &e, body, from_node);
        break;
    }
    case LG_T_TEXT:
        if (e.scope == LG_SCOPE_DIRECT) {
            if (!direct_body_ok(&e)) {
                n->stats.malformed++;
                break;
            }
            deliver_direct(n, &e, body, frame, len, from_node);
        } else {
            if (!plain_body_ok(&e, body)) {
                n->stats.malformed++;
                break;
            }
            deliver_fanout(n, &e, body, frame, len, from_node);
        }
        break;
    case LG_T_MSG_ACK:
        deliver_direct(n, &e, body, frame, len, from_node);
        break;
    case LG_T_NAME: {
        lg_name_t name;
        if (e.scope != LG_SCOPE_SYSTEM || !lg_name_dec(body, e.body_len, &name)) {
            n->stats.malformed++;
            break;
        }
        /* Only a newer name goes further: an AP that already holds it announced it itself. */
        if (name_take(n, &name)) {
            if (n->io.on_name != NULL) {
                n->io.on_name(n->io.ctx, &name);
            }
            push_name_to_local_clients(n, &name);
            forward(n, &e, body, from_node);
        }
        break;
    }
    case LG_T_TIME_SYNC: {
        lg_time_sync_t t;
        if (!lg_time_sync_dec(body, e.body_len, &t)) {
            n->stats.malformed++;
            break;
        }
        if (n->io.on_time != NULL) {
            n->io.on_time(n->io.ctx, e.origin_node, &t);
        }
        push_time_to_local_clients(n, t.quality);
        forward(n, &e, body, from_node);
        break;
    }
    case LG_T_GRID_STATE:
        if (e.scope != LG_SCOPE_SYSTEM || e.body_len == 0 || e.body_len > LG_GRID_STATE_MAX) {
            n->stats.malformed++;
            break;
        }
        if (n->io.on_grid_state != NULL) {
            n->io.on_grid_state(n->io.ctx, e.origin_node, body, e.body_len);
        }
        forward(n, &e, body, from_node);
        break;
    case LG_T_GROUPS: {
        lg_groups_t in;
        if (e.scope != LG_SCOPE_SYSTEM || !lg_groups_dec(body, e.body_len, &in)) {
            n->stats.malformed++;
            break;
        }
        if (lg_groups_newer(&in, &n->roster->groups)) {
            n->roster->groups = in;
            if (n->io.on_groups_changed != NULL) {
                n->io.on_groups_changed(n->io.ctx);
            }
            push_groups_to_local_clients(n);
        }
        forward(n, &e, body, from_node);
        break;
    }
    case LG_T_DIAG_ECHO:
        if (!lg_text_valid(body, e.body_len)) {
            n->stats.malformed++;
            break;
        }
        if (n->io.on_diag != NULL) {
            uint8_t hops = (uint8_t)(e.ttl <= LG_TTL_DEFAULT ? LG_TTL_DEFAULT - e.ttl + 1u : 0u);
            n->io.on_diag(n->io.ctx, e.origin_node, hops, body, e.body_len);
        }
        forward(n, &e, body, from_node);
        break;
    default:
        break;
    }
}

void lg_node_on_neighbor_up(lg_node_t *n, uint16_t neighbor)
{
    (void)neighbor;
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (n->presence[i].in_use && n->presence[i].node == n->self) {
            flood_presence(n, &n->presence[i]);
        }
    }
    lg_node_announce_names(n);
}

void lg_node_send_time(lg_node_t *n, uint32_t device, uint8_t quality)
{
    send_time_to_client(n, device, quality);
}

void lg_node_announce_time(lg_node_t *n, uint8_t quality)
{
    lg_time_sync_t t;
    time_sync_now(n, quality, &t);
    uint8_t body[LG_TIME_SYNC_LEN];
    size_t blen = lg_time_sync_enc(&t, body);
    (void)flood_new(n, LG_T_TIME_SYNC, body, blen);
    push_time_to_local_clients(n, quality);
}

int lg_node_send_diag(lg_node_t *n, const uint8_t *text, size_t len)
{
    if (!lg_text_valid(text, len)) {
        return LG_ERR_ARG;
    }
    return flood_new(n, LG_T_DIAG_ECHO, text, len);
}

int lg_node_announce_grid_state(lg_node_t *n, const uint8_t *body, size_t len)
{
    if (body == NULL || len == 0 || len > LG_GRID_STATE_MAX) {
        return LG_ERR_ARG;
    }
    return flood_new(n, LG_T_GRID_STATE, body, len);
}

uint8_t lg_node_edit_groups(lg_node_t *n, const lg_group_edit_t *edit)
{
    uint8_t status = lg_groups_apply_edit(n->roster, 0, edit, n->self);
    if (status == 0) {
        groups_changed(n);
    }
    return status;
}

int lg_node_announce_groups(lg_node_t *n)
{
    uint8_t body[LG_GROUPS_MAX_LEN];
    size_t blen = lg_groups_enc(&n->roster->groups, body);
    return flood_new(n, LG_T_GROUPS, body, blen);
}

bool lg_node_restore_name(lg_node_t *n, const lg_name_t *name)
{
    if (name == NULL || name->version == 0 || !lg_name_valid((const uint8_t *)name->text, name->len)) {
        return false;
    }
    lg_name_t copy = *name;
    copy.text[copy.len] = 0;
    return name_take(n, &copy);
}

void lg_node_announce_names(lg_node_t *n)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (n->names[i].version != 0) {
            flood_name(n, &n->names[i]);
        }
    }
}

const lg_name_t *lg_node_name(const lg_node_t *n, uint32_t device)
{
    int ui = lg_roster_user_index(n->roster, device);
    if (ui < 0 || ui >= (int)LG_MAX_DEVICES || n->names[ui].version == 0) {
        return NULL;
    }
    return &n->names[ui];
}
