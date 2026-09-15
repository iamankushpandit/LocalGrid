/*
 * lg_roster.h - users (one per handheld) and groups.
 *
 * In Phase 1 proper the roster comes from master-issued configuration.
 * The prototype uses the hardcoded roster from docs/DESIGN_REVIEW.md answer 46.
 */
#pragma once

#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t    device;   /* device index = user address; one user per handheld */
    const char *name;
    uint32_t    groups;   /* bit i set: member of roster->groups[i] */
} lg_user_t;

typedef struct {
    uint16_t    id;
    const char *name;
} lg_group_t;

typedef struct {
    const lg_user_t  *users;
    size_t            n_users;
    const lg_group_t *groups;
    size_t            n_groups;
    uint16_t          version;
} lg_roster_t;

const lg_user_t *lg_roster_user(const lg_roster_t *r, uint32_t device);
int              lg_roster_user_index(const lg_roster_t *r, uint32_t device);   /* -1 if unknown */
int              lg_roster_group_index(const lg_roster_t *r, uint16_t group_id); /* -1 if unknown */
const lg_group_t*lg_roster_group(const lg_roster_t *r, uint16_t group_id);
bool             lg_roster_is_member(const lg_roster_t *r, uint32_t device, uint16_t group_id);

/* Prototype roster: Dad(1) Emma(2) Alex(3) Ranger(4); FAMILY(1) KIDS(2) LEADERS(3). */
enum { LG_PROTO_DAD = 1, LG_PROTO_EMMA = 2, LG_PROTO_ALEX = 3, LG_PROTO_RANGER = 4 };
enum { LG_PROTO_FAMILY = 1, LG_PROTO_KIDS = 2, LG_PROTO_LEADERS = 3 };
const lg_roster_t *lg_roster_prototype(void);

#ifdef __cplusplus
}
#endif
