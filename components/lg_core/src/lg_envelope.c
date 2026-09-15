#include "lg_envelope.h"

#include <string.h>

const char *lg_err_str(int err)
{
    switch (err) {
    case LG_OK:          return "ok";
    case LG_ERR_SHORT:   return "buffer too short";
    case LG_ERR_VERSION: return "unsupported protocol version";
    case LG_ERR_RESERVED:return "reserved field not zero";
    case LG_ERR_LENGTH:  return "bad length";
    case LG_ERR_SCOPE:   return "scope/target/type mismatch";
    case LG_ERR_TYPE:    return "unexpected type";
    case LG_ERR_ARG:     return "invalid argument";
    case LG_ERR_FULL:    return "full";
    case LG_ERR_ID:      return "invalid message id";
    case LG_ERR_TIME:    return "time not set or mismatched";
    default:             return "unknown error";
    }
}

int lg_env_encode(const lg_env_t *env, uint8_t *out, size_t cap)
{
    if (env == NULL || out == NULL) {
        return LG_ERR_ARG;
    }
    if (cap < LG_ENV_SIZE) {
        return LG_ERR_SHORT;
    }
    if (env->body_len > LG_BODY_MAX) {
        return LG_ERR_LENGTH;
    }
    out[0] = (uint8_t)((LG_PROTO_MAJOR << 4) | (LG_PROTO_MINOR & 0x0Fu));
    out[1] = env->type;
    lg_wr16(out + 2, env->flags);
    out[4] = env->scope;
    out[5] = env->ttl;
    lg_wr16(out + 6, env->body_len);
    lg_wr32(out + 8, env->origin_id);
    lg_wr32(out + 12, env->origin_boot);
    lg_wr32(out + 16, env->origin_seq);
    lg_wr32(out + 20, env->target);
    lg_wr32(out + 24, env->grid_time);
    lg_wr16(out + 28, env->origin_node);
    lg_wr16(out + 30, 0);
    return (int)LG_ENV_SIZE;
}

int lg_env_decode(const uint8_t *f, size_t len, lg_env_t *e)
{
    if (f == NULL || e == NULL) {
        return LG_ERR_ARG;
    }
    if (len < LG_ENV_SIZE) {
        return LG_ERR_SHORT;
    }
    e->major = (uint8_t)(f[0] >> 4);
    e->minor = (uint8_t)(f[0] & 0x0F);
    if (e->major != LG_PROTO_MAJOR) {
        return LG_ERR_VERSION;
    }
    e->type        = f[1];
    e->flags       = lg_rd16(f + 2);
    e->scope       = f[4];
    e->ttl         = f[5];
    e->body_len    = lg_rd16(f + 6);
    e->origin_id   = lg_rd32(f + 8);
    e->origin_boot = lg_rd32(f + 12);
    e->origin_seq  = lg_rd32(f + 16);
    e->target      = lg_rd32(f + 20);
    e->grid_time   = lg_rd32(f + 24);
    e->origin_node = lg_rd16(f + 28);

    if (lg_rd16(f + 30) != 0) {
        return LG_ERR_RESERVED;
    }
    if (e->body_len > LG_BODY_MAX || (size_t)LG_ENV_SIZE + e->body_len != len) {
        return LG_ERR_LENGTH;
    }
    if (e->scope > LG_SCOPE_BROADCAST) {
        return LG_ERR_SCOPE;
    }
    if (e->scope == LG_SCOPE_BROADCAST && e->target != LG_TARGET_ALL) {
        return LG_ERR_SCOPE;
    }
    if ((e->scope == LG_SCOPE_DIRECT || e->scope == LG_SCOPE_GROUP) && e->target == LG_TARGET_ALL) {
        return LG_ERR_SCOPE;
    }
    if (e->type == LG_T_TEXT && e->scope == LG_SCOPE_SYSTEM) {
        return LG_ERR_SCOPE;
    }
    if (e->type == LG_T_MSG_ACK && e->scope != LG_SCOPE_DIRECT) {
        return LG_ERR_SCOPE;
    }
    if (e->origin_seq == 0) {
        return LG_ERR_ID;
    }
    return LG_OK;
}

void lg_e2e_nonce(const lg_env_t *e, uint8_t out[LG_E2E_NONCE_LEN])
{
    lg_wr32(out, e->origin_id);
    lg_wr32(out + 4, e->origin_boot);
    lg_wr32(out + 8, e->origin_seq);
}

void lg_e2e_aad(const lg_env_t *e, uint8_t out[LG_E2E_AAD_LEN])
{
    out[0] = e->type;
    out[1] = e->scope;
    lg_wr16(out + 2, (uint16_t)(e->flags & (uint16_t)~LG_FLAG_RELAYED));
    lg_wr32(out + 4, e->origin_id);
    lg_wr32(out + 8, e->origin_boot);
    lg_wr32(out + 12, e->origin_seq);
    lg_wr32(out + 16, e->target);
    lg_wr32(out + 20, e->grid_time);
}

int lg_frame_build(lg_env_t *env, const uint8_t *body, size_t body_len, uint8_t *out, size_t cap)
{
    if (env == NULL || out == NULL || (body_len > 0 && body == NULL)) {
        return LG_ERR_ARG;
    }
    if (body_len > LG_BODY_MAX) {
        return LG_ERR_LENGTH;
    }
    if (cap < LG_ENV_SIZE + body_len) {
        return LG_ERR_SHORT;
    }
    env->body_len = (uint16_t)body_len;
    int rc = lg_env_encode(env, out, cap);
    if (rc < 0) {
        return rc;
    }
    if (body_len > 0) {
        memcpy(out + LG_ENV_SIZE, body, body_len);
    }
    return (int)(LG_ENV_SIZE + body_len);
}
