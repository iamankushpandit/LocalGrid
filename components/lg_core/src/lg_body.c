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
    return v->status >= LG_ACK_ACCEPTED && v->status <= LG_ACK_REJ_INVALID;
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
