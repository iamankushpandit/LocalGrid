#include "lg_body.h"

#include <string.h>

size_t lg_register_enc(const lg_register_t *v, uint8_t *out)
{
    lg_wr32(out, v->device);
    lg_wr32(out + 4, v->attach_epoch);
    lg_wr32(out + 8, v->client_time);
    lg_wr16(out + 12, v->caps);
    memcpy(out + 14, v->pubkey, LG_PUBKEY_LEN);
    return LG_REGISTER_LEN;
}

bool lg_register_dec(const uint8_t *in, size_t len, lg_register_t *v)
{
    if (len != LG_REGISTER_LEN) {
        return false;
    }
    v->device       = lg_rd32(in);
    v->attach_epoch = lg_rd32(in + 4);
    v->client_time  = lg_rd32(in + 8);
    v->caps         = lg_rd16(in + 12);
    memcpy(v->pubkey, in + 14, LG_PUBKEY_LEN);
    return true;
}

size_t lg_register_ack_enc(const lg_register_ack_t *v, uint8_t *out)
{
    lg_wr16(out, v->node);
    out[2] = v->status;
    lg_wr32(out + 3, v->grid_time);
    lg_wr16(out + 7, v->roster_version);
    return LG_REGISTER_ACK_LEN;
}

bool lg_register_ack_dec(const uint8_t *in, size_t len, lg_register_ack_t *v)
{
    if (len != LG_REGISTER_ACK_LEN) {
        return false;
    }
    v->node           = lg_rd16(in);
    v->status         = in[2];
    v->grid_time      = lg_rd32(in + 3);
    v->roster_version = lg_rd16(in + 7);
    return true;
}

size_t lg_ping_enc(uint8_t battery, uint8_t *out)
{
    out[0] = battery;
    return LG_PING_LEN;
}

bool lg_ping_dec(const uint8_t *in, size_t len, uint8_t *battery)
{
    if (len == 0) {
        *battery = LG_BATTERY_UNKNOWN;   /* a handheld from before D68 */
        return true;
    }
    if (len != LG_PING_LEN || (in[0] > 100u && in[0] != LG_BATTERY_UNKNOWN)) {
        return false;
    }
    *battery = in[0];
    return true;
}

size_t lg_msg_ack_enc(const lg_msg_ack_t *v, uint8_t *out)
{
    lg_wr32(out, v->author);
    lg_wr32(out + 4, v->boot);
    lg_wr32(out + 8, v->seq);
    out[12] = v->status;
    return LG_MSG_ACK_LEN;
}

bool lg_msg_ack_dec(const uint8_t *in, size_t len, lg_msg_ack_t *v)
{
    if (len != LG_MSG_ACK_LEN) {
        return false;
    }
    v->author = lg_rd32(in);
    v->boot   = lg_rd32(in + 4);
    v->seq    = lg_rd32(in + 8);
    v->status = in[12];
    /* The bound is the highest status this build knows, so appending a status to the enum
     * means naming it here as well, or every frame carrying it is counted malformed. */
    return v->status >= LG_ACK_ACCEPTED && v->status <= LG_ACK_REJ_NOT_ALLOWED;
}

size_t lg_voice_hdr_enc(const lg_voice_hdr_t *h, uint8_t *out)
{
    out[0] = h->codec;
    out[1] = h->flags;
    lg_wr16(out + 2, h->talk);
    lg_wr16(out + 4, h->frame);
    lg_wr16(out + 6, (uint16_t)h->predictor);
    out[8] = h->step_index;
    out[9] = 0;
    return LG_VOICE_HDR_LEN;
}

bool lg_voice_hdr_dec(const uint8_t *body, size_t len, lg_voice_hdr_t *out)
{
    if (body == NULL || len < LG_VOICE_HDR_LEN || len > LG_VOICE_PAYLOAD_MAX) {
        return false;
    }
    if (body[0] == LG_VOICE_CODEC_C2_1200) {
        size_t data = len - LG_VOICE_HDR_LEN;
        if (data > LG_VOICE_C2_DATA_MAX || data % LG_VOICE_C2_FRAME_LEN != 0 || body[6] != 0 || body[7] != 0 ||
            body[8] != 0 || body[9] != 0) {
            return false;
        }
    } else if (body[0] != LG_VOICE_CODEC_IMA_8K || body[8] > LG_VOICE_STEP_MAX || body[9] != 0) {
        return false;
    }
    if (out != NULL) {
        out->codec      = body[0];
        out->flags      = body[1];
        out->talk       = lg_rd16(body + 2);
        out->frame      = lg_rd16(body + 4);
        out->predictor  = (int16_t)lg_rd16(body + 6);
        out->step_index = body[8];
    }
    return true;
}

size_t lg_presence_enc(const lg_presence_t *v, uint8_t *out)
{
    lg_wr32(out, v->device);
    lg_wr16(out + 4, v->node);
    lg_wr32(out + 6, v->epoch);
    out[10] = v->state;
    memcpy(out + 11, v->pubkey, LG_PUBKEY_LEN);
    return LG_PRESENCE_LEN;
}

bool lg_presence_dec(const uint8_t *in, size_t len, lg_presence_t *v)
{
    if (len != LG_PRESENCE_LEN) {
        return false;
    }
    v->device = lg_rd32(in);
    v->node   = lg_rd16(in + 4);
    v->epoch  = lg_rd32(in + 6);
    v->state  = in[10];
    memcpy(v->pubkey, in + 11, LG_PUBKEY_LEN);
    return v->state <= LG_PRES_LORA;   /* D80: OFFLINE, ONLINE, or reachable over LoRa */
}

bool lg_name_valid(const uint8_t *text, size_t len)
{
    return text != NULL && len >= 1u && len <= LG_NAME_MAX - 1u && lg_utf8_valid(text, len);
}

size_t lg_name_enc(const lg_name_t *v, uint8_t *out)
{
    lg_wr32(out, v->device);
    lg_wr32(out + 4, v->version);
    out[8] = v->len;
    memcpy(out + 9, v->text, v->len);
    return 9u + v->len;
}

bool lg_name_dec(const uint8_t *in, size_t len, lg_name_t *v)
{
    if (len < LG_NAME_LEN_MIN || len > LG_NAME_LEN_MAX || in[8] != len - 9u) {
        return false;
    }
    v->device  = lg_rd32(in);
    v->version = lg_rd32(in + 4);
    v->len     = in[8];
    if (v->device == 0 || v->version == 0 || !lg_name_valid(in + 9, v->len)) {
        return false;
    }
    memcpy(v->text, in + 9, v->len);
    v->text[v->len] = '\0';
    return true;
}

bool lg_position_valid(int32_t lat_u, int32_t lon_u, uint32_t fix_time)
{
    return lat_u >= -LG_POS_LAT_MAX && lat_u <= LG_POS_LAT_MAX &&
           lon_u >= -LG_POS_LON_MAX && lon_u <= LG_POS_LON_MAX &&
           fix_time >= LG_POS_TIME_MIN;
}

size_t lg_position_enc(const lg_position_t *p, uint8_t *out)
{
    lg_wr32(out, p->subject);
    lg_wr32(out + 4, (uint32_t)p->lat_u);
    lg_wr32(out + 8, (uint32_t)p->lon_u);
    lg_wr32(out + 12, p->fix_time);
    out[16] = p->sats;
    out[17] = p->flags;
    return LG_POSITION_LEN;
}

bool lg_position_dec(const uint8_t *body, size_t len, lg_position_t *out)
{
    if (body == NULL || out == NULL || len != LG_POSITION_LEN) {
        return false;
    }
    lg_position_t p = {
        .subject  = lg_rd32(body),
        .lat_u    = (int32_t)lg_rd32(body + 4),
        .lon_u    = (int32_t)lg_rd32(body + 8),
        .fix_time = lg_rd32(body + 12),
        .sats     = body[16],
        .flags    = body[17],
    };
    if (p.subject == 0 || !lg_position_valid(p.lat_u, p.lon_u, p.fix_time)) {
        return false;
    }
    *out = p;
    return true;
}

int lg_position_slot(const lg_roster_t *r, uint32_t subject)
{
    if ((subject & LG_NODE_ID_BASE) != 0) {
        uint32_t node = subject & ~LG_NODE_ID_BASE;
        return node < LG_MAX_NODES ? (int)(LG_MAX_DEVICES + node) : -1;
    }
    int ui = lg_roster_user_index(r, subject);
    return ui >= 0 && ui < (int)LG_MAX_DEVICES ? ui : -1;
}

size_t lg_trail_enc(const lg_trail_t *t, uint8_t *out)
{
    uint8_t count = t->count < LG_TRAIL_POINTS - 1u ? t->count : (uint8_t)(LG_TRAIL_POINTS - 1u);
    lg_wr32(out, t->subject);
    lg_wr32(out + 4, (uint32_t)t->lat_u);
    lg_wr32(out + 8, (uint32_t)t->lon_u);
    lg_wr32(out + 12, t->fix_time);
    out[16] = count;
    uint8_t *p = out + LG_TRAIL_HDR_LEN;
    for (uint8_t i = 0; i < count; i++, p += LG_TRAIL_POINT_LEN) {
        lg_wr16(p, (uint16_t)t->older[i].dlat);
        lg_wr16(p + 2, (uint16_t)t->older[i].dlon);
        lg_wr16(p + 4, t->older[i].age_s);
    }
    return LG_TRAIL_HDR_LEN + (size_t)count * LG_TRAIL_POINT_LEN;
}

size_t lg_devfw_enc(const lg_devfw_t *v, uint8_t *out)
{
    memset(out, 0, LG_DEVFW_LEN);
    lg_wr32(out, v->device);
    lg_wr32(out + 4, v->serial);
    out[8] = v->chip;
    size_t n = strnlen(v->version, LG_DEVFW_VERSION_MAX);
    memcpy(out + 9, v->version, n);
    return LG_DEVFW_LEN;
}

bool lg_devfw_dec(const uint8_t *body, size_t len, lg_devfw_t *out)
{
    if (body == NULL || out == NULL || len != LG_DEVFW_LEN) {
        return false;
    }
    lg_devfw_t v;
    memset(&v, 0, sizeof(v));
    v.device = lg_rd32(body);
    v.serial = lg_rd32(body + 4);
    v.chip = body[8];
    if (v.device == 0 || (v.device & LG_NODE_ID_BASE) != 0 || v.chip == 0 || v.chip > 3u) {
        return false;
    }
    bool ended = false;
    for (size_t i = 0; i < LG_DEVFW_VERSION_MAX; i++) {
        uint8_t ch = body[9 + i];
        if (ch == 0) {
            ended = true;
        } else if (ended || ch < 0x20u || ch > 0x7Eu) {
            return false;   /* text after the padding, or not printable: a version is shown on screens */
        } else {
            v.version[i] = (char)ch;
        }
    }
    *out = v;
    return true;
}

bool lg_trail_dec(const uint8_t *body, size_t len, lg_trail_t *out)
{
    if (body == NULL || out == NULL || len < LG_TRAIL_HDR_LEN || body[16] > LG_TRAIL_POINTS - 1u ||
        len != LG_TRAIL_HDR_LEN + (size_t)body[16] * LG_TRAIL_POINT_LEN) {
        return false;
    }
    lg_trail_t t;
    memset(&t, 0, sizeof(t));
    t.subject  = lg_rd32(body);
    t.lat_u    = (int32_t)lg_rd32(body + 4);
    t.lon_u    = (int32_t)lg_rd32(body + 8);
    t.fix_time = lg_rd32(body + 12);
    t.count    = body[16];
    if (t.subject == 0 || (t.subject & LG_NODE_ID_BASE) != 0 || !lg_position_valid(t.lat_u, t.lon_u, t.fix_time)) {
        return false;   /* an AP stays where it is put: it has a position and no trail */
    }
    const uint8_t *p = body + LG_TRAIL_HDR_LEN;
    for (uint8_t i = 0; i < t.count; i++, p += LG_TRAIL_POINT_LEN) {
        t.older[i].dlat  = (int16_t)lg_rd16(p);
        t.older[i].dlon  = (int16_t)lg_rd16(p + 2);
        t.older[i].age_s = lg_rd16(p + 4);
        if (i > 0 && t.older[i].age_s < t.older[i - 1u].age_s) {
            return false;   /* most recent first, or it is not a path */
        }
    }
    *out = t;
    return true;
}

size_t lg_time_sync_enc_v2(const lg_time_sync_t *v, uint8_t *out)
{
    lg_wr32(out, v->grid_time);
    lg_wr16(out + 4, v->millis);
    out[6] = v->quality;
    out[7] = v->stratum;
    return LG_TIME_SYNC_LEN_V2;
}

size_t lg_time_sync_enc(const lg_time_sync_t *v, uint8_t *out)
{
    (void)lg_time_sync_enc_v2(v, out);
    out[8] = v->flags;
    return LG_TIME_SYNC_LEN;
}

bool lg_time_sync_dec(const uint8_t *in, size_t len, lg_time_sync_t *v)
{
    if (len == LG_TIME_SYNC_LEN_V1) {
        v->grid_time = lg_rd32(in);
        v->millis    = 0;
        v->quality   = in[4];
        v->stratum   = LG_STRATUM_UNKNOWN;
        v->flags     = 0;
        return v->quality <= LG_TIME_AUTHORITATIVE;
    }
    if (len != LG_TIME_SYNC_LEN_V2 && len != LG_TIME_SYNC_LEN) {
        return false;
    }
    v->grid_time = lg_rd32(in);
    v->millis    = lg_rd16(in + 4);
    v->quality   = in[6];
    v->stratum   = in[7];
    v->flags     = len == LG_TIME_SYNC_LEN ? in[8] : 0u;   /* unknown bits are ignored, not refused */
    return v->quality <= LG_TIME_AUTHORITATIVE && v->millis < 1000u;
}

bool lg_tz_valid(const uint8_t *s, size_t n)
{
    if (s == NULL || n == 0 || n > LG_TZ_MAX || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z') || s[0] == '<')) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        uint8_t ch = s[i];
        bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '<' ||
                  ch == '>' || ch == '+' || ch == '-' || ch == ',' || ch == '.' || ch == ':' || ch == '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

size_t lg_hello_enc(const lg_hello_t *v, uint8_t *out)
{
    lg_wr16(out, v->node);
    out[2] = v->role;
    out[3] = v->clients;
    return LG_HELLO_LEN;
}

bool lg_hello_dec(const uint8_t *in, size_t len, lg_hello_t *v)
{
    if (len != LG_HELLO_LEN) {
        return false;
    }
    v->node    = lg_rd16(in);
    v->role    = in[2];
    v->clients = in[3];
    return true;
}

bool lg_utf8_valid(const uint8_t *s, size_t n)
{
    size_t i = 0;
    while (i < n) {
        uint8_t c = s[i];
        if (c == 0x00) {
            return false;
        }
        if (c < 0x80) {
            i++;
            continue;
        }
        size_t need;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            need = 1;
            cp = c & 0x1Fu;
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
            cp = c & 0x0Fu;
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            cp = c & 0x07u;
        } else {
            return false;
        }
        if (n - i <= need) {
            return false; /* truncated sequence */
        }
        for (size_t k = 1; k <= need; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if ((need == 1 && cp < 0x80u) || (need == 2 && cp < 0x800u) || (need == 3 && cp < 0x10000u)) {
            return false; /* overlong */
        }
        if (cp > 0x10FFFFu || (cp >= 0xD800u && cp <= 0xDFFFu)) {
            return false;
        }
        i += need + 1;
    }
    return true;
}

bool lg_text_valid(const uint8_t *s, size_t n)
{
    return s != NULL && n >= 1 && n <= LG_TEXT_MAX && lg_utf8_valid(s, n);
}

/* A name field is 16 bytes: 1..15 bytes of valid UTF-8, then NUL padding to the end. */
static bool name_field_ok(const uint8_t *f)
{
    size_t n = 0;
    while (n < LG_GROUP_NAME_MAX && f[n] != 0) {
        n++;
    }
    if (n == 0 || !lg_utf8_valid(f, n)) {
        return false;
    }
    for (size_t i = n; i < LG_GROUP_NAME_MAX + 1u; i++) {
        if (f[i] != 0) {
            return false;
        }
    }
    return true;
}

size_t lg_groups_enc(const lg_groups_t *v, uint8_t *out)
{
    uint8_t count = v->count <= LG_MAX_GROUPS ? v->count : (uint8_t)LG_MAX_GROUPS;
    lg_wr32(out, v->seq);
    lg_wr16(out + 4, v->author);
    lg_wr16(out + 6, v->next_id);
    lg_wr32(out + 8, v->announcers);
    out[12] = count;
    for (size_t i = 0; i < count; i++) {
        uint8_t *p = out + LG_GROUPS_HEAD_LEN + i * LG_GROUP_ENTRY_LEN;
        const lg_group_t *g = &v->groups[i];
        lg_wr16(p, g->id);
        lg_wr32(p + 2, g->members);
        memset(p + 6, 0, LG_GROUP_NAME_MAX + 1u);
        for (size_t k = 0; k < LG_GROUP_NAME_MAX && g->name[k] != '\0'; k++) {
            p[6 + k] = (uint8_t)g->name[k];
        }
    }
    return LG_GROUPS_HEAD_LEN + count * LG_GROUP_ENTRY_LEN;
}

bool lg_groups_dec(const uint8_t *in, size_t len, lg_groups_t *v)
{
    if (len < LG_GROUPS_HEAD_LEN) {
        return false;
    }
    uint8_t count = in[12];
    if (count > LG_MAX_GROUPS || len != LG_GROUPS_HEAD_LEN + count * LG_GROUP_ENTRY_LEN) {
        return false;
    }
    lg_groups_t t;
    memset(&t, 0, sizeof(t));
    t.seq     = lg_rd32(in);
    t.author  = lg_rd16(in + 4);
    t.next_id = lg_rd16(in + 6);
    t.announcers = lg_rd32(in + 8);
    t.count   = count;
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = in + LG_GROUPS_HEAD_LEN + i * LG_GROUP_ENTRY_LEN;
        lg_group_t *g = &t.groups[i];
        g->id = lg_rd16(p);
        g->members = lg_rd32(p + 2);
        if (g->id == 0 || (t.next_id != 0 && g->id >= t.next_id) || !name_field_ok(p + 6)) {
            return false;
        }
        for (size_t k = 0; k < i; k++) {
            if (t.groups[k].id == g->id) {
                return false;
            }
        }
        memcpy(g->name, p + 6, LG_GROUP_NAME_MAX + 1u);
    }
    *v = t;
    return true;
}

size_t lg_group_edit_enc(const lg_group_edit_t *v, uint8_t *out)
{
    out[0] = v->op;
    lg_wr16(out + 1, v->id);
    lg_wr32(out + 3, v->members);
    memset(out + 7, 0, LG_GROUP_NAME_MAX + 1u);
    for (size_t k = 0; k < LG_GROUP_NAME_MAX && v->name[k] != '\0'; k++) {
        out[7 + k] = (uint8_t)v->name[k];
    }
    return LG_GROUP_EDIT_LEN;
}

bool lg_group_edit_dec(const uint8_t *in, size_t len, lg_group_edit_t *v)
{
    if (len != LG_GROUP_EDIT_LEN) {
        return false;
    }
    memset(v, 0, sizeof(*v));
    v->op = in[0];
    v->id = lg_rd16(in + 1);
    v->members = lg_rd32(in + 3);
    if (v->op == LG_GROUP_DELETE) {
        return true;   /* a removal names only the group */
    }
    if (!name_field_ok(in + 7)) {
        return false;
    }
    memcpy(v->name, in + 7, LG_GROUP_NAME_MAX + 1u);
    return true;
}
