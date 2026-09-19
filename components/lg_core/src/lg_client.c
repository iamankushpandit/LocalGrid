#include "lg_client.h"

#include <string.h>

static uint32_t absdiff(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static uint32_t c_now_ms(const lg_client_t *c)
{
    return c->io.now_ms ? c->io.now_ms(c->io.ctx) : 0;
}

static uint32_t c_local_time(const lg_client_t *c)
{
    return c->io.local_time ? c->io.local_time(c->io.ctx) : 0;
}

static void emit(lg_client_t *c, lg_client_event_type_t type, uint32_t value)
{
    if (c->io.on_event != NULL) {
        lg_client_event_t ev = { .type = type, .value = value };
        c->io.on_event(c->io.ctx, &ev);
    }
}

static lg_peer_t *peer_find(lg_client_t *c, uint32_t device)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (c->peers[i].in_use && c->peers[i].device == device) {
            return &c->peers[i];
        }
    }
    return NULL;
}

const lg_peer_t *lg_client_peer(const lg_client_t *c, uint32_t device)
{
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (c->peers[i].in_use && c->peers[i].device == device) {
            return &c->peers[i];
        }
    }
    return NULL;
}

static lg_peer_t *peer_get(lg_client_t *c, uint32_t device)
{
    lg_peer_t *p = peer_find(c, device);
    if (p != NULL) {
        return p;
    }
    for (size_t i = 0; i < LG_MAX_DEVICES; i++) {
        if (!c->peers[i].in_use) {
            p = &c->peers[i];
            memset(p, 0, sizeof(*p));
            p->in_use = 1;
            p->device = device;
            return p;
        }
    }
    return NULL;
}

static bool all_zero(const uint8_t *b, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++) {
        acc |= b[i];
    }
    return acc == 0;
}

static uint32_t next_seq(lg_client_t *c)
{
    if (++c->seq == 0) {
        c->seq = 1;
    }
    return c->seq;
}

static void base_env(lg_client_t *c, lg_env_t *e, uint8_t type, uint8_t scope, uint32_t target)
{
    memset(e, 0, sizeof(*e));
    e->type        = type;
    e->scope       = scope;
    e->target      = target;
    e->origin_id   = c->device;
    e->origin_boot = c->boot;
    e->origin_seq  = next_seq(c);
    e->grid_time   = c_local_time(c);
}

static bool send_frame(lg_client_t *c, lg_env_t *e, const uint8_t *body, size_t len)
{
    if (!c->connected || c->io.send == NULL) {
        return false;
    }
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(e, body, len, buf, sizeof(buf));
    if (flen < 0) {
        return false;
    }
    return c->io.send(c->io.ctx, buf, (size_t)flen);
}

/* ---- names (D50) --------------------------------------------------------- */

static lg_name_t *name_slot(lg_client_t *c, uint32_t device)
{
    int ui = lg_roster_user_index(c->roster, device);
    return ui >= 0 && ui < (int)LG_MAX_DEVICES ? &c->names[ui] : NULL;
}

static bool name_take(lg_client_t *c, const lg_name_t *name)
{
    lg_name_t *slot = name_slot(c, name->device);
    if (slot == NULL || name->version <= slot->version) {
        return false;
    }
    *slot = *name;
    return true;
}

static void send_own_name(lg_client_t *c)
{
    const lg_name_t *own = lg_client_name(c, c->device);
    if (own == NULL || !c->registered) {
        return;
    }
    uint8_t body[LG_NAME_LEN_MAX];
    size_t blen = lg_name_enc(own, body);
    lg_env_t e;
    base_env(c, &e, LG_T_NAME, LG_SCOPE_SYSTEM, c->device);
    (void)send_frame(c, &e, body, blen);
}

int lg_client_set_name(lg_client_t *c, const uint8_t *text, size_t len)
{
    lg_name_t *slot = name_slot(c, c->device);
    if (slot == NULL || !lg_name_valid(text, len)) {
        return LG_ERR_ARG;
    }
    uint32_t floor = c->boot << 12;
    lg_name_t name = {
        .device  = c->device,
        .version = slot->version >= floor ? slot->version + 1u : floor,
        .len     = (uint8_t)len,
    };
    if (name.version == 0) {
        name.version = 1;   /* boot 0 on a fresh board */
    }
    memcpy(name.text, text, len);
    name.text[len] = 0;
    *slot = name;
    emit(c, LG_CEV_NAME, c->device);
    send_own_name(c);
    return LG_OK;
}

bool lg_client_restore_name(lg_client_t *c, const lg_name_t *name)
{
    if (name == NULL || name->version == 0 || !lg_name_valid((const uint8_t *)name->text, name->len)) {
        return false;
    }
    lg_name_t copy = *name;
    copy.text[copy.len] = 0;
    return name_take(c, &copy);
}

const lg_name_t *lg_client_name(const lg_client_t *c, uint32_t device)
{
    int ui = lg_roster_user_index(c->roster, device);
    if (ui < 0 || ui >= (int)LG_MAX_DEVICES || c->names[ui].version == 0) {
        return NULL;
    }
    return &c->names[ui];
}

/* ---- positions (D65) ------------------------------------------------------ */

int lg_client_send_position(lg_client_t *c, int32_t lat_u, int32_t lon_u, uint32_t fix_time,
                            uint8_t sats, uint8_t flags)
{
    if (c == NULL || !lg_position_valid(lat_u, lon_u, fix_time)) {
        return LG_ERR_ARG;
    }
    int slot = lg_position_slot(c->roster, c->device);
    if (slot < 0) {
        return LG_ERR_ARG;
    }
    lg_position_t pos = {
        .subject  = c->device,
        .lat_u    = lat_u,
        .lon_u    = lon_u,
        .fix_time = fix_time,
        .sats     = sats,
        .flags    = flags,
    };
    if (fix_time >= c->positions[slot].fix_time) {
        c->positions[slot] = pos;
    }
    if (!c->registered) {
        return LG_ERR_SHORT;
    }
    uint8_t body[LG_POSITION_LEN];
    size_t blen = lg_position_enc(&pos, body);
    lg_env_t e;
    base_env(c, &e, LG_T_POSITION, LG_SCOPE_SYSTEM, c->device);
    return send_frame(c, &e, body, blen) ? LG_OK : LG_ERR_SHORT;
}

const lg_position_t *lg_client_position(const lg_client_t *c, uint32_t subject)
{
    int slot = lg_position_slot(c->roster, subject);
    if (slot < 0 || c->positions[slot].fix_time == 0) {
        return NULL;
    }
    return &c->positions[slot];
}

static void handle_position(lg_client_t *c, const lg_env_t *e, const uint8_t *body)
{
    lg_position_t pos;
    if (e->scope != LG_SCOPE_SYSTEM || !lg_position_dec(body, e->body_len, &pos)) {
        return;
    }
    int slot = lg_position_slot(c->roster, pos.subject);
    if (slot < 0 || pos.fix_time <= c->positions[slot].fix_time) {
        return;   /* an older or equal fix changes nothing */
    }
    c->positions[slot] = pos;
    emit(c, LG_CEV_POSITION, pos.subject);
}

void lg_client_init(lg_client_t *c, uint32_t device, uint32_t boot, const uint8_t *pubkey,
                    lg_roster_t *roster, const lg_client_io_t *io)
{
    memset(c, 0, sizeof(*c));
    c->device = device;
    c->boot   = boot;
    c->roster = roster;
    c->io     = *io;
    if (pubkey != NULL) {
        memcpy(c->pubkey, pubkey, LG_PUBKEY_LEN);
    }
    lg_dedup_init(&c->dedup, c->dedup_slots, LG_CLIENT_DEDUP_SLOTS);
}

void lg_client_connected(lg_client_t *c)
{
    c->connected  = true;
    c->registered = false;
    c->attach_count++;

    lg_register_t reg = {
        .device       = c->device,
        .attach_epoch = (c->boot << 12) | (c->attach_count & 0xFFFu),
        .client_time  = c_local_time(c),
        .caps         = 0,
    };
    memcpy(reg.pubkey, c->pubkey, LG_PUBKEY_LEN);
    uint8_t body[LG_REGISTER_LEN];
    size_t blen = lg_register_enc(&reg, body);
    lg_env_t e;
    base_env(c, &e, LG_T_REGISTER, LG_SCOPE_SYSTEM, 0);
    (void)send_frame(c, &e, body, blen);
}

void lg_client_disconnected(lg_client_t *c)
{
    c->connected  = false;
    c->registered = false;
}

void lg_client_ping(lg_client_t *c)
{
    if (!c->connected) {
        return;
    }
    lg_env_t e;
    base_env(c, &e, LG_T_PING, LG_SCOPE_SYSTEM, 0);
    (void)send_frame(c, &e, NULL, 0);
}

bool lg_client_time_from_gps(const lg_client_t *c)
{
    return c->grid_time_known && c->time_from_gps;
}

const char *lg_client_time_zone(const lg_client_t *c)
{
    return c->tz;
}

bool lg_client_time_restricted(const lg_client_t *c)
{
    return !c->grid_time_known || c_local_time(c) == 0;
}

static void transmit(lg_client_t *c, size_t i)
{
    lg_out_msg_t *m = &c->outbox[i];
    lg_env_t e;
    memset(&e, 0, sizeof(e));
    e.type        = LG_T_TEXT;
    e.scope       = m->scope;
    e.flags       = m->flags;
    e.target      = m->target;
    e.origin_id   = c->device;
    e.origin_boot = m->boot;
    e.origin_seq  = m->seq;
    e.grid_time   = m->grid_time;

    if (m->scope == LG_SCOPE_DIRECT) {
        const lg_peer_t *p = peer_find(c, m->target);
        if (p == NULL || !p->has_key || c->io.seal == NULL) {
            m->state = LG_OUT_REJECTED;
            m->reject_reason = LG_ACK_REJ_UNKNOWN_TARGET;   /* no key for this person yet */
            emit(c, LG_CEV_OUTBOX, (uint32_t)i);
            return;
        }
        e.flags |= LG_FLAG_E2E_PAYLOAD;
        uint8_t nonce[LG_E2E_NONCE_LEN];
        uint8_t aad[LG_E2E_AAD_LEN];
        uint8_t ct[LG_DIRECT_BODY_MAX];
        lg_e2e_nonce(&e, nonce);
        lg_e2e_aad(&e, aad);
        int n = c->io.seal(c->io.ctx, m->target, p->pubkey, nonce, aad, sizeof(aad), m->text, m->len, ct);
        if (n != (int)(m->len + LG_AEAD_TAG_LEN)) {
            m->state = LG_OUT_REJECTED;
            m->reject_reason = LG_ACK_REJ_INVALID;
            emit(c, LG_CEV_OUTBOX, (uint32_t)i);
            return;
        }
        (void)send_frame(c, &e, ct, (size_t)n);
    } else {
        (void)send_frame(c, &e, m->text, m->len);
    }
    m->last_tx_ms = c_now_ms(c);
}

static int pick_slot(const lg_client_t *c)
{
    int best = -1;
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        if (c->outbox[i].state == LG_OUT_EMPTY) {
            return (int)i;
        }
    }
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        const lg_out_msg_t *m = &c->outbox[i];
        bool reusable = m->state == LG_OUT_DELIVERED || m->state == LG_OUT_READ || m->state == LG_OUT_REJECTED ||
                        (m->state == LG_OUT_ACCEPTED && m->scope != LG_SCOPE_DIRECT);
        if (reusable && (best < 0 || m->seq < c->outbox[best].seq)) {
            best = (int)i;
        }
    }
    return best;
}

int lg_client_send_text(lg_client_t *c, uint8_t scope, uint32_t target, uint16_t flags,
                        const uint8_t *text, size_t len)
{
    if (!lg_text_valid(text, len)) {
        return LG_ERR_ARG;
    }
    switch (scope) {
    case LG_SCOPE_DIRECT:
        if (target == LG_TARGET_ALL || target == c->device || lg_roster_user(c->roster, target) == NULL ||
            c->io.seal == NULL) {
            return LG_ERR_ARG;
        }
        break;
    case LG_SCOPE_GROUP:
        if (target > 0xFFFFu || lg_roster_group_index(c->roster, (uint16_t)target) < 0) {
            return LG_ERR_ARG;
        }
        break;
    case LG_SCOPE_BROADCAST:
        target = LG_TARGET_ALL;
        break;
    default:
        return LG_ERR_ARG;
    }

    bool urgent_broadcast = scope == LG_SCOPE_BROADCAST && (flags & LG_FLAG_URGENT) != 0;
    if (lg_client_time_restricted(c) && !urgent_broadcast) {
        return LG_ERR_TIME;
    }
    /* The AP refuses it anyway (D56); refusing here says so before the message leaves. */
    if (scope == LG_SCOPE_BROADCAST && !urgent_broadcast && !lg_roster_may_announce(c->roster, c->device)) {
        return LG_ERR_DENIED;
    }

    int slot = pick_slot(c);
    if (slot < 0) {
        return LG_ERR_FULL;
    }
    lg_out_msg_t *m = &c->outbox[slot];
    memset(m, 0, sizeof(*m));
    m->state     = LG_OUT_PENDING;
    m->scope     = scope;
    m->flags     = (uint16_t)(flags & (LG_FLAG_URGENT | LG_FLAG_ACK_REQUESTED | LG_FLAG_ALL_CLEAR));
    m->target    = target;
    m->boot      = c->boot;
    m->seq       = next_seq(c);
    m->grid_time = c_local_time(c);
    m->len       = (uint16_t)len;
    memcpy(m->text, text, len);

    if (c->registered) {
        transmit(c, (size_t)slot);
    }
    emit(c, LG_CEV_OUTBOX, (uint32_t)slot);
    return slot;
}

/* ---- voice (D61) --------------------------------------------------------- */

int lg_client_send_voice(lg_client_t *c, uint8_t scope, uint32_t target, const uint8_t *payload, size_t len)
{
    if (c == NULL || !lg_voice_hdr_dec(payload, len, NULL)) {
        return LG_ERR_ARG;
    }
    const lg_peer_t *p = NULL;
    switch (scope) {
    case LG_SCOPE_DIRECT:
        if (target == LG_TARGET_ALL || target == c->device || lg_roster_user(c->roster, target) == NULL ||
            c->io.seal == NULL) {
            return LG_ERR_ARG;
        }
        break;
    case LG_SCOPE_GROUP:
        if (target > 0xFFFFu || lg_roster_group_index(c->roster, (uint16_t)target) < 0) {
            return LG_ERR_ARG;
        }
        break;
    default:
        return LG_ERR_ARG;   /* never broadcast */
    }
    if (lg_client_time_restricted(c)) {
        return LG_ERR_TIME;   /* voice follows the time rule of non-urgent text (D6) */
    }
    if (!c->registered) {
        return LG_ERR_SHORT;
    }
    if (scope == LG_SCOPE_DIRECT) {
        p = peer_find(c, target);
        if (p == NULL || !p->has_key) {
            return LG_ERR_ARG;
        }
    }

    c->voice_seq = (c->voice_seq + 1u) & ~LG_VOICE_SEQ_BIT;
    if (c->voice_seq == 0) {
        c->voice_seq = 1;
    }
    lg_env_t e;
    memset(&e, 0, sizeof(e));
    e.type        = LG_T_VOICE;
    e.scope       = scope;
    e.target      = target;
    e.origin_id   = c->device;
    e.origin_boot = c->boot;
    e.origin_seq  = LG_VOICE_SEQ_BIT | c->voice_seq;
    e.grid_time   = c_local_time(c);

    if (scope == LG_SCOPE_DIRECT) {
        e.flags = LG_FLAG_E2E_PAYLOAD;
        uint8_t nonce[LG_E2E_NONCE_LEN];
        uint8_t aad[LG_E2E_AAD_LEN];
        uint8_t ct[LG_VOICE_PAYLOAD_MAX + LG_AEAD_TAG_LEN];
        lg_e2e_nonce(&e, nonce);
        lg_e2e_aad(&e, aad);
        int n = c->io.seal(c->io.ctx, target, p->pubkey, nonce, aad, sizeof(aad), payload, len, ct);
        if (n != (int)(len + LG_AEAD_TAG_LEN)) {
            return LG_ERR_ARG;
        }
        return send_frame(c, &e, ct, (size_t)n) ? LG_OK : LG_ERR_FULL;
    }
    return send_frame(c, &e, payload, len) ? LG_OK : LG_ERR_FULL;
}

static void handle_voice(lg_client_t *c, const lg_env_t *e, const uint8_t *body)
{
    if (e->origin_id == c->device || (e->origin_seq & LG_VOICE_SEQ_BIT) == 0 || c->io.on_voice == NULL) {
        return;
    }
    int ui = lg_roster_user_index(c->roster, e->origin_id);
    if (ui < 0 || ui >= (int)LG_MAX_DEVICES) {
        return;
    }
    lg_voice_seen_t *seen = &c->voice_seen[ui];
    if (!lg_voice_newer(seen, e->origin_boot, e->origin_seq)) {
        return;   /* a copy by another path, or late: voice only plays forward */
    }

    uint8_t plain[LG_VOICE_PAYLOAD_MAX];
    size_t plen;
    if (e->scope == LG_SCOPE_DIRECT) {
        if (e->target != c->device || (e->flags & LG_FLAG_E2E_PAYLOAD) == 0 ||
            e->body_len < LG_VOICE_HDR_LEN + LG_AEAD_TAG_LEN ||
            e->body_len > LG_VOICE_PAYLOAD_MAX + LG_AEAD_TAG_LEN) {
            return;
        }
        const lg_peer_t *p = peer_find(c, e->origin_id);
        if (p == NULL || !p->has_key || c->io.open == NULL) {
            c->decrypt_failures++;   /* counted, but ten frames a second raise no events */
            return;
        }
        uint8_t nonce[LG_E2E_NONCE_LEN];
        uint8_t aad[LG_E2E_AAD_LEN];
        lg_e2e_nonce(e, nonce);
        lg_e2e_aad(e, aad);
        int n = c->io.open(c->io.ctx, e->origin_id, p->pubkey, nonce, aad, sizeof(aad), body, e->body_len, plain);
        if (n < 0) {
            c->decrypt_failures++;
            return;
        }
        plen = (size_t)n;
    } else if (e->scope == LG_SCOPE_GROUP) {
        if ((e->flags & LG_FLAG_E2E_PAYLOAD) != 0 || e->target > 0xFFFFu ||
            !lg_roster_is_member(c->roster, c->device, (uint16_t)e->target) ||
            e->body_len > sizeof(plain)) {
            return;
        }
        memcpy(plain, body, e->body_len);
        plen = e->body_len;
    } else {
        return;
    }
    if (!lg_voice_hdr_dec(plain, plen, NULL)) {
        return;
    }
    seen->boot = e->origin_boot;
    seen->seq  = e->origin_seq;
    c->io.on_voice(c->io.ctx, e, plain, plen);
}

static void apply_time(lg_client_t *c, uint32_t grid_time)
{
    if (grid_time == 0) {
        c->grid_time_known = false;
        c->time_from_gps = false;
        emit(c, LG_CEV_TIME, 0);
        return;
    }
    c->grid_time_known = true;
    uint32_t lt = c_local_time(c);
    if ((lt == 0 || absdiff(lt, grid_time) > 1u) && c->io.set_time != NULL) {
        c->io.set_time(c->io.ctx, grid_time);
    }
    emit(c, LG_CEV_TIME, grid_time);
}

static void send_delivered(lg_client_t *c, const lg_env_t *orig)
{
    lg_msg_ack_t a = {
        .author = orig->origin_id,
        .boot   = orig->origin_boot,
        .seq    = orig->origin_seq,
        .status = LG_ACK_DELIVERED,
    };
    uint8_t body[LG_MSG_ACK_LEN];
    size_t blen = lg_msg_ack_enc(&a, body);
    lg_env_t e;
    base_env(c, &e, LG_T_MSG_ACK, LG_SCOPE_DIRECT, orig->origin_id);
    (void)send_frame(c, &e, body, blen);
}

/* The same ack the recipient sends for delivery, with the status that says it was read. */
bool lg_client_mark_read(lg_client_t *c, uint32_t author, uint32_t boot, uint32_t seq)
{
    if (c == NULL || author == c->device || !c->registered) {
        return false;
    }
    lg_msg_ack_t a = {
        .author = author,
        .boot   = boot,
        .seq    = seq,
        .status = LG_ACK_READ,
    };
    uint8_t body[LG_MSG_ACK_LEN];
    size_t blen = lg_msg_ack_enc(&a, body);
    lg_env_t e;
    base_env(c, &e, LG_T_MSG_ACK, LG_SCOPE_DIRECT, author);
    return send_frame(c, &e, body, blen);
}

static void inbox_store(lg_client_t *c, const lg_env_t *e, const uint8_t *plain, size_t plen)
{
    size_t pos;
    if (c->inbox_count < LG_INBOX_SIZE) {
        pos = (c->inbox_head + c->inbox_count) % LG_INBOX_SIZE;
        c->inbox_count++;
    } else {
        pos = c->inbox_head;
        c->inbox_head = (uint16_t)((c->inbox_head + 1u) % LG_INBOX_SIZE);
    }
    lg_in_msg_t *m = &c->inbox[pos];
    m->author    = e->origin_id;
    m->target    = e->target;
    m->grid_time = e->grid_time;
    m->boot      = e->origin_boot;
    m->seq       = e->origin_seq;
    m->scope     = e->scope;
    m->flags     = e->flags;
    m->len       = (uint16_t)plen;
    memcpy(m->text, plain, plen);
}

const lg_in_msg_t *lg_client_inbox(const lg_client_t *c, size_t newest_index)
{
    if (newest_index >= c->inbox_count) {
        return NULL;
    }
    size_t pos = (c->inbox_head + c->inbox_count - 1u - newest_index) % LG_INBOX_SIZE;
    return &c->inbox[pos];
}

/* A removed group's messages go everywhere this core holds them (D52). */
static void forget_group(lg_client_t *c, uint16_t id)
{
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        lg_out_msg_t *m = &c->outbox[i];
        if (m->state != LG_OUT_EMPTY && m->scope == LG_SCOPE_GROUP && m->target == id) {
            memset(m, 0, sizeof(*m));
        }
    }
    for (size_t i = 0; i < LG_INBOX_SIZE; i++) {
        lg_in_msg_t *m = &c->inbox[i];
        if (m->scope == LG_SCOPE_GROUP && m->target == id) {
            memset(m->text, 0, sizeof(m->text));
            m->len = 0;
            m->scope = LG_SCOPE_SYSTEM;   /* an emptied slot, never shown as a message */
        }
    }
}

static void handle_groups(lg_client_t *c, const lg_env_t *e, const uint8_t *body)
{
    lg_groups_t in;
    if (e->scope != LG_SCOPE_SYSTEM || !lg_groups_dec(body, e->body_len, &in) ||
        !lg_groups_newer(&in, &c->roster->groups)) {
        return;   /* an older or equal table changes nothing here */
    }
    uint16_t removed[LG_MAX_GROUPS];
    size_t n = lg_groups_removed(&c->roster->groups, &in, removed);
    c->roster->groups = in;
    for (size_t i = 0; i < n; i++) {
        forget_group(c, removed[i]);
    }
    if (n > 0 && c->io.on_groups_removed != NULL) {
        c->io.on_groups_removed(c->io.ctx, removed, n);
    }
    emit(c, LG_CEV_GROUPS, (uint32_t)n);
}

int lg_client_edit_group(lg_client_t *c, const lg_group_edit_t *edit)
{
    if (edit == NULL || edit->op < LG_GROUP_CREATE || edit->op > LG_GROUP_DELETE) {
        return LG_ERR_ARG;
    }
    if (!c->registered) {
        return LG_ERR_SHORT;
    }
    uint8_t body[LG_GROUP_EDIT_LEN];
    size_t blen = lg_group_edit_enc(edit, body);
    lg_env_t e;
    base_env(c, &e, LG_T_GROUP_EDIT, LG_SCOPE_SYSTEM, 0);
    if (!send_frame(c, &e, body, blen)) {
        return LG_ERR_SHORT;
    }
    c->group_edit_seq = e.origin_seq;
    return LG_OK;
}

static void handle_text(lg_client_t *c, const lg_env_t *e, const uint8_t *body)
{
    if (e->origin_id == c->device) {
        return;
    }
    lg_dedup_result_t d = lg_dedup_check(&c->dedup, e->origin_id, e->origin_boot, e->origin_seq);
    if (d == LG_DEDUP_STALE) {
        return;
    }
    if (d == LG_DEDUP_DUPLICATE) {
        send_delivered(c, e);   /* the author may have missed our first confirmation */
        return;
    }

    uint8_t plain[LG_TEXT_MAX];
    size_t plen;
    if (e->scope == LG_SCOPE_DIRECT) {
        if (e->target != c->device || (e->flags & LG_FLAG_E2E_PAYLOAD) == 0 ||
            e->body_len < 1u + LG_AEAD_TAG_LEN || e->body_len > LG_DIRECT_BODY_MAX) {
            return;
        }
        const lg_peer_t *p = peer_find(c, e->origin_id);
        if (p == NULL || !p->has_key || c->io.open == NULL) {
            c->decrypt_failures++;
            emit(c, LG_CEV_DECRYPT_FAILED, e->origin_id);
            return;
        }
        uint8_t nonce[LG_E2E_NONCE_LEN];
        uint8_t aad[LG_E2E_AAD_LEN];
        lg_e2e_nonce(e, nonce);
        lg_e2e_aad(e, aad);
        int n = c->io.open(c->io.ctx, e->origin_id, p->pubkey, nonce, aad, sizeof(aad), body, e->body_len, plain);
        if (n < 0 || !lg_text_valid(plain, (size_t)n)) {
            c->decrypt_failures++;
            emit(c, LG_CEV_DECRYPT_FAILED, e->origin_id);
            return;
        }
        plen = (size_t)n;
    } else {
        if ((e->flags & LG_FLAG_E2E_PAYLOAD) != 0 || !lg_text_valid(body, e->body_len)) {
            return;
        }
        if (e->scope == LG_SCOPE_GROUP && !lg_roster_is_member(c->roster, c->device, (uint16_t)e->target)) {
            return;
        }
        memcpy(plain, body, e->body_len);
        plen = e->body_len;
    }

    (void)lg_dedup_mark(&c->dedup, e->origin_id, e->origin_boot, e->origin_seq);
    inbox_store(c, e, plain, plen);
    send_delivered(c, e);
    emit(c, LG_CEV_MESSAGE, 0);
}

static void handle_ack(lg_client_t *c, const lg_env_t *e, const uint8_t *body)
{
    lg_msg_ack_t a;
    if (!lg_msg_ack_dec(body, e->body_len, &a) || a.author != c->device) {
        return;
    }
    if ((a.seq & LG_VOICE_SEQ_BIT) != 0) {
        /* Voice is never acknowledged when taken; an ack naming a voice frame is a refusal (D61). */
        if (a.boot == c->boot && a.status != LG_ACK_ACCEPTED && a.status != LG_ACK_DELIVERED &&
            a.status != LG_ACK_READ) {
            emit(c, LG_CEV_VOICE_REFUSED, a.status);
        }
        return;
    }
    if (c->group_edit_seq != 0 && a.boot == c->boot && a.seq == c->group_edit_seq) {
        if (a.status != LG_ACK_ACCEPTED) {
            emit(c, LG_CEV_GROUP_REFUSED, a.status);
        }
        c->group_edit_seq = 0;
        return;
    }
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        lg_out_msg_t *m = &c->outbox[i];
        if (m->state == LG_OUT_EMPTY || m->boot != a.boot || m->seq != a.seq) {
            continue;
        }
        switch (a.status) {
        case LG_ACK_ACCEPTED:
            if (m->state == LG_OUT_PENDING) {
                m->state = LG_OUT_ACCEPTED;
            }
            break;
        case LG_ACK_DELIVERED: {
            int ui = lg_roster_user_index(c->roster, e->origin_id);
            if (ui >= 0 && ui < 32 && (m->delivered_mask & (1u << (unsigned)ui)) == 0) {
                m->delivered_mask |= 1u << (unsigned)ui;
                m->delivered_count++;
            }
            if (m->scope == LG_SCOPE_DIRECT) {
                if (m->state != LG_OUT_READ) {   /* a read message stays read */
                    m->state = LG_OUT_DELIVERED;
                }
            } else if (m->state == LG_OUT_PENDING) {
                m->state = LG_OUT_ACCEPTED;
            }
            break;
        }
        case LG_ACK_READ: {
            int ui = lg_roster_user_index(c->roster, e->origin_id);
            if (ui >= 0 && ui < 32 && (m->read_mask & (1u << (unsigned)ui)) == 0) {
                m->read_mask |= 1u << (unsigned)ui;
                m->read_count++;
            }
            /* Only 1:1 carries a read state: a group would report one per member. */
            if (m->scope == LG_SCOPE_DIRECT && (m->state == LG_OUT_DELIVERED || m->state == LG_OUT_ACCEPTED)) {
                m->state = LG_OUT_READ;
            }
            break;
        }
        default:
            if (m->state == LG_OUT_PENDING) {
                m->state = LG_OUT_REJECTED;
                m->reject_reason = a.status;
            }
            break;
        }
        emit(c, LG_CEV_OUTBOX, (uint32_t)i);
        return;
    }
}

void lg_client_on_frame(lg_client_t *c, const uint8_t *frame, size_t len)
{
    lg_env_t e;
    if (lg_env_decode(frame, len, &e) != LG_OK) {
        return;
    }
    const uint8_t *body = lg_frame_body(frame);

    switch (e.type) {
    case LG_T_REGISTER_ACK: {
        lg_register_ack_t a;
        if (!lg_register_ack_dec(body, e.body_len, &a) || a.status != LG_REG_OK) {
            return;
        }
        c->registered = true;
        c->node = a.node;
        apply_time(c, a.grid_time);
        emit(c, LG_CEV_REGISTERED, a.node);
        send_own_name(c);   /* the AP keeps the newest; an older copy it holds is replaced */
        for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
            if (c->outbox[i].state == LG_OUT_PENDING) {
                transmit(c, i);   /* re-offer with the original id; nodes deduplicate */
            }
        }
        break;
    }
    case LG_T_PRESENCE_UPDATE: {
        lg_presence_t p;
        if (!lg_presence_dec(body, e.body_len, &p)) {
            return;
        }
        lg_peer_t *peer = peer_get(c, p.device);
        if (peer == NULL) {
            return;
        }
        peer->node  = p.node;
        peer->state = p.state;
        peer->epoch = p.epoch;
        if (!all_zero(p.pubkey, LG_PUBKEY_LEN)) {
            if (!peer->has_key) {
                memcpy(peer->pubkey, p.pubkey, LG_PUBKEY_LEN);
                peer->has_key = 1;
            } else if (memcmp(peer->pubkey, p.pubkey, LG_PUBKEY_LEN) != 0) {
                emit(c, LG_CEV_KEY_CHANGED, p.device);   /* pinned key kept */
            }
        }
        emit(c, LG_CEV_PRESENCE, p.device);
        break;
    }
    case LG_T_NAME: {
        lg_name_t name;
        if (e.scope == LG_SCOPE_SYSTEM && lg_name_dec(body, e.body_len, &name) && name_take(c, &name)) {
            emit(c, LG_CEV_NAME, name.device);
        }
        break;
    }
    case LG_T_PONG:
        c->last_pong_ms = c_now_ms(c);
        break;
    case LG_T_TIME_SYNC: {
        lg_time_sync_t t;
        if (lg_time_sync_dec(body, e.body_len, &t)) {
            /* Before apply_time, so its LG_CEV_TIME already sees where the time came from (D67). */
            c->time_from_gps = t.grid_time != 0 && (t.flags & LG_TIME_FROM_GPS) != 0;
            apply_time(c, t.grid_time);
        }
        break;
    }
    case LG_T_TIME_ZONE:
        if (e.scope == LG_SCOPE_SYSTEM && lg_tz_valid(body, e.body_len) &&
            (strlen(c->tz) != e.body_len || memcmp(c->tz, body, e.body_len) != 0)) {
            memcpy(c->tz, body, e.body_len);
            c->tz[e.body_len] = '\0';
            emit(c, LG_CEV_TIME_ZONE, e.body_len);
        }
        break;
    case LG_T_TEXT:
        handle_text(c, &e, body);
        break;
    case LG_T_MSG_ACK:
        handle_ack(c, &e, body);
        break;
    case LG_T_VOICE:
        handle_voice(c, &e, body);
        break;
    case LG_T_GROUPS:
        handle_groups(c, &e, body);
        break;
    case LG_T_POSITION:
        handle_position(c, &e, body);
        break;
    default:
        break;
    }
}

void lg_client_tick(lg_client_t *c)
{
    if (!c->registered) {
        return;
    }
    uint32_t now = c_now_ms(c);
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        lg_out_msg_t *m = &c->outbox[i];
        if (m->state == LG_OUT_PENDING && now - m->last_tx_ms >= LG_RESEND_MS) {
            transmit(c, i);
        }
    }
}
