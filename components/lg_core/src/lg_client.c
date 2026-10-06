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
    if (c->io.send == NULL) {
        return false;
    }
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(e, body, len, buf, sizeof(buf));
    if (flen < 0) {
        return false;
    }
    /*
     * D77, D80: offered even with no session, and the answer still means "a session took it".
     *
     * The refusal used to be the first thing here, on `!c->connected`. That is right for a socket
     * and wrong for the second radio, in exactly the way it was already wrong for the announcement
     * (lg_client_lora_announce, and the note above it). A handheld whose Wi-Fi has gone has no
     * session and never will on LoRa - it is not a backbone peer (D77) - so refusing here left a
     * handheld on LoRa alone able to *receive* a message and an SOS while able to originate nothing
     * but "I am here": no reply, no acknowledgement, no position. Measured on the bench on
     * 2026-09-30: 150 s with Wi-Fi off, one frame on the air, and that one was the announcement.
     *
     * What a radio actually carries stays the glue's decision - D74's one radio at a time, D77's
     * allow-list - which is where it belongs. The core's part was only to stop hiding the frame from
     * it. The return value is deliberately unchanged: false unless a session carried it, so the
     * message stays PENDING in the outbox and keeps its stored (boot, seq, grid time). A later Wi-Fi
     * attempt at a frame LoRa already delivered is then a duplicate the grid drops by identity, not
     * a second message - which is why reusing those three fields is load-bearing here.
     */
    bool taken = c->io.send(c->io.ctx, buf, (size_t)flen);
    return c->connected && taken;
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
    if (c == NULL || c->roster == NULL) {
        return NULL;   /* asked before the client was set up: no name to give, and not a crash */
    }
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

/* ---- trails (D99) --------------------------------------------------------- */

int lg_client_send_trail(lg_client_t *c, const lg_trail_t *trail)
{
    if (c == NULL || trail == NULL || trail->count > LG_TRAIL_POINTS - 1u ||
        !lg_position_valid(trail->lat_u, trail->lon_u, trail->fix_time)) {
        return LG_ERR_ARG;
    }
    lg_trail_t mine = *trail;
    mine.subject = c->device;
    uint8_t body[LG_TRAIL_LEN_MAX];
    size_t blen = lg_trail_enc(&mine, body);
    lg_env_t e;
    base_env(c, &e, LG_T_TRAIL, LG_SCOPE_SYSTEM, c->device);
    return send_frame(c, &e, body, blen) ? LG_OK : LG_ERR_SHORT;
}

int lg_client_request_trail(lg_client_t *c, uint32_t subject)
{
    if (c == NULL || subject == 0) {
        return LG_ERR_ARG;
    }
    if (c->trail_want != subject) {
        c->trail_want = subject;
        memset(&c->trail, 0, sizeof(c->trail));
    }
    uint8_t body[LG_TRAIL_REQ_LEN];
    lg_wr32(body, subject);
    lg_env_t e;
    base_env(c, &e, LG_T_TRAIL_REQ, LG_SCOPE_SYSTEM, subject);
    return send_frame(c, &e, body, sizeof(body)) ? LG_OK : LG_ERR_SHORT;
}

int lg_client_send_fw_get(lg_client_t *c, const uint8_t *body, size_t len)
{
    if (c == NULL || body == NULL || len == 0 || len > LG_FW_BODY_MAX) {
        return LG_ERR_ARG;
    }
    if (!c->connected || !c->registered) {
        return LG_ERR_SHORT;   /* not put to a radio that could never carry the answer */
    }
    lg_env_t e;
    base_env(c, &e, LG_T_FW_GET, LG_SCOPE_SYSTEM, 0);
    e.ttl = 1;
    return send_frame(c, &e, body, len) ? LG_OK : LG_ERR_SHORT;
}

const lg_trail_t *lg_client_trail(const lg_client_t *c, uint32_t subject)
{
    return c != NULL && subject != 0 && c->trail.fix_time != 0 && c->trail.subject == subject ? &c->trail : NULL;
}

static void handle_trail(lg_client_t *c, const lg_env_t *e, const uint8_t *body)
{
    lg_trail_t t;
    if (e->scope != LG_SCOPE_SYSTEM || !lg_trail_dec(body, e->body_len, &t)) {
        return;
    }
    if (t.subject != c->trail_want || t.fix_time <= c->trail.fix_time) {
        return;   /* not the one asked for, or no newer than the one held */
    }
    c->trail = t;
    emit(c, LG_CEV_TRAIL, t.subject);
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

/*
 * The announcement over LoRa (D77, D80). Deliberately does not go through send_frame, whose first
 * act is to refuse when `connected` is false -- see the header for why that gate is right for a
 * socket and wrong for this radio.
 */
bool lg_client_lora_announce(lg_client_t *c, uint8_t battery)
{
    if (c == NULL || c->io.send == NULL) {
        return false;
    }
    if (battery > 100u) {
        battery = LG_BATTERY_UNKNOWN;
    }
    uint8_t body[LG_PING_LEN];
    size_t blen = lg_ping_enc(battery, body);
    lg_env_t e;
    base_env(c, &e, LG_T_PING, LG_SCOPE_SYSTEM, 0);
    uint8_t buf[LG_FRAME_MAX];
    int flen = lg_frame_build(&e, body, blen, buf, sizeof(buf));
    if (flen < 0) {
        return false;
    }
    (void)c->io.send(c->io.ctx, buf, (size_t)flen);
    return true;   /* offered: on a handheld io.send reports the socket, which is the thing missing */
}

void lg_client_ping(lg_client_t *c, uint8_t battery)
{
    if (!c->connected) {
        return;
    }
    if (battery > 100u) {
        battery = LG_BATTERY_UNKNOWN;
    }
    uint8_t body[LG_PING_LEN];
    size_t blen = lg_ping_enc(battery, body);
    lg_env_t e;
    base_env(c, &e, LG_T_PING, LG_SCOPE_SYSTEM, 0);
    (void)send_frame(c, &e, body, blen);
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

/*
 * D86: is this an alert, and has it waited long enough that an access point would refuse it for the
 * clock rule? Only an urgent broadcast, an SOS or an all clear qualifies - they carry LG_FLAG_URGENT or
 * LG_FLAG_ALL_CLEAR - and only while there is a clock to compare against.
 *
 * The threshold is deliberately short of LG_TIME_TOLERANCE_S: a frame re-stamped at the very edge would
 * spend its remaining margin in the air and on the access point's queue, and arrive just as stale as
 * the one it replaced.
 */
static bool alert_is_too_old(const lg_client_t *c, const lg_out_msg_t *m)
{
    if ((m->flags & (LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR)) == 0u || m->restamps >= LG_RESTAMP_MAX) {
        return false;
    }
    uint32_t now = c_local_time(c);
    if (now == 0u || m->grid_time == 0u || now < m->grid_time) {
        return false;   /* no clock, or a stamp from the future: not something to act on here */
    }
    return (now - m->grid_time) > (LG_TIME_TOLERANCE_S - LG_TIME_RESTAMP_MARGIN_S);
}

/*
 * A pending message that no access point will ever accept again is failed and its slot freed.
 *
 * Found by a chaos night on 2026-10-01, and it is the worst thing the project has found: a handheld
 * went **permanently mute** - `[MSG] Not sent (outbox full)` twenty-seven times, still refusing to send
 * twenty minutes after it had rejoined Wi-Fi and while looking perfectly healthy on screen. The chain:
 *
 *   1. A message nothing acknowledges stays LG_OUT_PENDING for ever; nothing aged it out.
 *   2. D6 refuses a message whose grid time is more than LG_TIME_TOLERANCE_S from the access point's,
 *      and a retransmission must reuse that stamp - so past that window it can never be accepted.
 *   3. Such a message therefore held its outbox slot for ever.
 *   4. There are LG_OUTBOX_SIZE slots. After that the handheld cannot send anything at all, **including
 *      an SOS**, until it is restarted.
 *
 * Keeping it served nobody: it could not be delivered, and holding it disabled the device. So it is
 * failed with the reason the sender would have been given anyway, which both tells the person and frees
 * the slot. An alert is exempt while it still has a re-stamp left (D86), because it is about to be given
 * a new identity and a fresh stamp; once those are spent it ages out like anything else rather than
 * silently filling the outbox.
 *
 * The threshold is the tolerance plus the same margin a re-stamp keeps, so a message that is merely in
 * flight is never thrown away.
 */
static bool give_up_on_stale(lg_client_t *c, lg_out_msg_t *m)
{
    uint32_t now = c_local_time(c);
    if (now == 0u || m->grid_time == 0u || now < m->grid_time) {
        return false;   /* no clock, or a stamp from the future: not something to judge age by */
    }
    if ((now - m->grid_time) <= (uint32_t)(LG_TIME_TOLERANCE_S + LG_TIME_RESTAMP_MARGIN_S)) {
        return false;
    }
    if ((m->flags & (LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR)) != 0u && m->restamps < LG_RESTAMP_MAX) {
        return false;   /* D86 is about to give it a new id; it is not stale yet */
    }
    m->state = LG_OUT_REJECTED;
    m->reject_reason = LG_ACK_REJ_TIME;
    return true;
}

/* D86: a new id and a new stamp, so the grid may accept it. The text is untouched. */
static void restamp(lg_client_t *c, lg_out_msg_t *m)
{
    m->seq       = next_seq(c);
    m->grid_time = c_local_time(c);
    m->restamps++;
    m->delivered_mask = 0;
    m->delivered_count = 0;
    m->read_mask = 0;
    m->read_count = 0;
}

/*
 * `may_reject` is false for an attempt made with no session (D80). A missing peer key means "not
 * learned yet", and before registration it is the normal state, so rejecting the message there would
 * fail a message for the one reason that is about to stop being true. With a session it is a real
 * refusal and is reported as one.
 */
static void transmit_ex(lg_client_t *c, size_t i, bool may_reject)
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
            if (!may_reject) {
                return;   /* stays PENDING: no session yet, so no key yet is not a refusal */
            }
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

static void transmit(lg_client_t *c, size_t i)
{
    transmit_ex(c, i, true);
}

/*
 * Is this slot finished with, so a new message may take it?
 *
 * `urgent` is the second chance an alert gets. A 1:1 message waiting to be confirmed is held because a
 * delivery tick is worth something - but it is worth less than being able to raise an alarm, and a
 * handheld whose outbox is full of them cannot send anything at all. So an ordinary message waits
 * LG_ACCEPTED_HOLD_S before it may take such a slot, and an urgent one or an all clear takes the oldest
 * immediately. Nothing is lost that was not already delivered to the grid: ACCEPTED means a node has it.
 */
static bool slot_spent(const lg_client_t *c, const lg_out_msg_t *m, bool urgent)
{
    if (m->state == LG_OUT_DELIVERED || m->state == LG_OUT_READ || m->state == LG_OUT_REJECTED) {
        return true;
    }
    if (m->state != LG_OUT_ACCEPTED) {
        return false;   /* EMPTY is handled by the caller; PENDING is still trying */
    }
    if (m->scope != LG_SCOPE_DIRECT) {
        return true;    /* group and broadcast count deliveries as they come; they never hold a slot */
    }
    if (urgent) {
        return true;
    }
    uint32_t now = c_local_time(c);
    return now != 0u && m->grid_time != 0u && now > m->grid_time &&
           (now - m->grid_time) > LG_ACCEPTED_HOLD_S;
}

static int pick_slot(const lg_client_t *c, bool urgent)
{
    int best = -1;
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        if (c->outbox[i].state == LG_OUT_EMPTY) {
            return (int)i;
        }
    }
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        const lg_out_msg_t *m = &c->outbox[i];
        if (slot_spent(c, m, urgent) && (best < 0 || m->seq < c->outbox[best].seq)) {
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

    /* An alert may take a slot an ordinary message must wait for: see slot_spent. */
    bool alert = (flags & (LG_FLAG_URGENT | LG_FLAG_ALL_CLEAR)) != 0u;
    int slot = pick_slot(c, alert);
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

    /*
     * D77, D80: attempted whether or not this handheld is registered. A handheld on LoRa alone is
     * unregistered by definition - registration is one of the types that radio refuses, because an
     * AP would have to trust a handheld about it - so gating the first attempt on `registered` meant
     * a message typed with Wi-Fi down reached no radio at all. It waited in the outbox and left over
     * Wi-Fi the moment a session returned, which the bench measured as a 70-second silence on
     * 2026-09-30. The attempt cannot reject the message (see transmit_ex) and cannot lose it: the
     * slot stays PENDING until something acknowledges it.
     */
    transmit_ex(c, (size_t)slot, c->registered);
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
    case LG_SCOPE_BROADCAST:
        /*
         * Talking to everyone (owner, 2026-10-02: "I didn't see a way of broadcasting a voice message
         * to everyone ... so that announcements can be made that way"). Gated by the same permission as
         * a written announcement, D56, which every handheld holds by default - so this is open to
         * everybody until an admin takes it away from somebody, and the rule is one rule rather than
         * two. Voice is still never offered to LoRa (D71): one 100 ms frame would be seconds of
         * airtime, and that exclusion is by message type, so it covers this too.
         */
        if (!lg_roster_may_announce(c->roster, c->device)) {
            return LG_ERR_ARG;
        }
        target = LG_TARGET_ALL;
        break;
    default:
        return LG_ERR_ARG;
    }
    if (lg_client_time_restricted(c)) {
        return LG_ERR_TIME;   /* voice follows the time rule of non-urgent text (D6) */
    }
    /*
     * The compact codec may leave without a session (D95): it is the one kind of voice LoRa carries,
     * so a handheld that has lost its AP offers it to the radio exactly as it offers text (D80). Live
     * frames still need a session, because nothing else can carry them.
     */
    bool compact = payload[0] == LG_VOICE_CODEC_C2_1200;
    if (!c->registered && !compact) {
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
    e.flags       = compact ? LG_FLAG_COMPACT : 0;

    if (scope == LG_SCOPE_DIRECT) {
        e.flags |= LG_FLAG_E2E_PAYLOAD;
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
    } else if (e->scope == LG_SCOPE_BROADCAST) {
        /*
         * A spoken announcement (D88). The fifth and last gate the scope had to pass: the sender, the
         * handheld service, the node's body check and its validator were all opened to it on 2026-10-02
         * while this one still dropped it, so an announcement that crossed the whole grid was discarded
         * by every listener. Like a group it is not sealed end to end - there is no one recipient - and
         * unlike a group there is no membership to test, because everybody is addressed. The permission
         * lives at the sender and the AP (D56); a listener plays what the grid accepted.
         */
        if ((e->flags & LG_FLAG_E2E_PAYLOAD) != 0 || e->body_len > sizeof(plain)) {
            return;
        }
        memcpy(plain, body, e->body_len);
        plen = e->body_len;
    } else {
        return;
    }
    if (!lg_voice_hdr_dec(plain, plen, NULL) ||
        ((e->flags & LG_FLAG_COMPACT) != 0) != (plain[0] == LG_VOICE_CODEC_C2_1200)) {
        return;   /* the envelope and the payload must agree about the codec (D95) */
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
    case LG_T_TRAIL:
        handle_trail(c, &e, body);
        break;
    case LG_T_FW_OFFER:
    case LG_T_FW_DATA:
        /* D107: only from this handheld's own AP, over its session, addressed to it. */
        if (c->registered && e.scope == LG_SCOPE_SYSTEM && e.target == c->device && (e.origin_id & LG_NODE_ID_BASE) != 0 &&
            e.body_len != 0 && e.body_len <= LG_FW_BODY_MAX && c->io.on_fw != NULL) {
            c->io.on_fw(c->io.ctx, e.type, body, e.body_len);
        }
        break;
    default:
        break;
    }
}

void lg_client_tick(lg_client_t *c)
{
    uint32_t now = c_now_ms(c);
    /*
     * D77, D80: a handheld with no session retries too, because one LoRa attempt is not a delivery -
     * the radio is lossy and nothing acknowledges at the link layer. It retries far more slowly.
     * LG_RESEND_MS is 3 s, tuned for a socket; a full outbox retried that fast would offer five
     * messages a second to a radio that carries about one every three (D74), and the glue's bounded
     * queue would spend the airtime dropping them - in front of the alert this radio exists for.
     * LG_RESEND_LORA_MS is the same mechanism at a cadence the air can hold.
     */
    uint32_t every = c->registered ? LG_RESEND_MS : LG_RESEND_LORA_MS;
    if (c->registered) {
        for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
            lg_out_msg_t *m = &c->outbox[i];
            if (m->state != LG_OUT_PENDING) {
                continue;
            }
            if (now - m->last_tx_ms >= every) {
                if (alert_is_too_old(c, m)) {
                    restamp(c, m);   /* D86 */
                    emit(c, LG_CEV_OUTBOX, (uint32_t)i);
                }
                transmit(c, i);
            }
            if (give_up_on_stale(c, m)) {
                emit(c, LG_CEV_OUTBOX, (uint32_t)i);   /* the slot is free and the sender is told */
            }
        }
        return;
    }
    /*
     * With no session, at most LG_RESEND_LORA_MAX_PER_TICK of them, oldest first. The outbox is not in
     * order, so the oldest is the smallest sequence number among those due - the same ordering the grid
     * would have seen had they gone out over a socket, and the one a reader expects.
     */
    for (unsigned sent = 0; sent < LG_RESEND_LORA_MAX_PER_TICK; sent++) {
        int oldest = -1;
        for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
            const lg_out_msg_t *m = &c->outbox[i];
            if (m->state != LG_OUT_PENDING || now - m->last_tx_ms < every) {
                continue;
            }
            if (oldest < 0 || m->seq < c->outbox[oldest].seq) {
                oldest = (int)i;
            }
        }
        if (oldest < 0) {
            return;
        }
        if (alert_is_too_old(c, &c->outbox[oldest])) {
            restamp(c, &c->outbox[oldest]);   /* D86 */
            emit(c, LG_CEV_OUTBOX, (uint32_t)oldest);
        }
        transmit_ex(c, (size_t)oldest, false);
    }
    /*
     * Aged out last, and over every slot rather than only the one just sent: the outbox filling is what
     * takes a handheld off the air, and it fills with messages this pass would otherwise never look at
     * again. After the attempt, not before, so an alert that has just been given a new stamp (D86) is
     * never judged on the old one - which is the ordering the first version of this got wrong, and a
     * scenario caught.
     */
    for (size_t i = 0; i < LG_OUTBOX_SIZE; i++) {
        if (c->outbox[i].state == LG_OUT_PENDING && give_up_on_stale(c, &c->outbox[i])) {
            emit(c, LG_CEV_OUTBOX, (uint32_t)i);
        }
    }
}
