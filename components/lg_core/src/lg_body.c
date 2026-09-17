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
    return v->status >= LG_ACK_ACCEPTED && v->status <= LG_ACK_READ;
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
    return v->state <= LG_PRES_ONLINE;
}

size_t lg_time_sync_enc(const lg_time_sync_t *v, uint8_t *out)
{
    lg_wr32(out, v->grid_time);
    out[4] = v->quality;
    return LG_TIME_SYNC_LEN;
}

bool lg_time_sync_dec(const uint8_t *in, size_t len, lg_time_sync_t *v)
{
    if (len != LG_TIME_SYNC_LEN) {
        return false;
    }
    v->grid_time = lg_rd32(in);
    v->quality   = in[4];
    return v->quality <= LG_TIME_AUTHORITATIVE;
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
    out[8] = count;
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
    uint8_t count = in[8];
    if (count > LG_MAX_GROUPS || len != LG_GROUPS_HEAD_LEN + count * LG_GROUP_ENTRY_LEN) {
        return false;
    }
    lg_groups_t t;
    memset(&t, 0, sizeof(t));
    t.seq     = lg_rd32(in);
    t.author  = lg_rd16(in + 4);
    t.next_id = lg_rd16(in + 6);
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
