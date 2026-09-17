/*
 * lg_roster.h - users (one per handheld) and groups.
 *
 * Users are still the prototype's fixed list (docs/DESIGN_REVIEW.md answer 46). Groups are
 * not: they are made, renamed, given members, and removed on the admin page or on a handheld,
 * and every AP and handheld holds a copy (D52). A copy carries a version, (seq, author), and
 * the higher version wins everywhere, as the admin settings do (D45).
 *
 * The table lives inside the roster, so the owner of a roster (AP or handheld firmware) keeps
 * one mutable lg_roster_t and hands the core a pointer to it.
 */
#pragma once

#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LG_GROUP_NAME_MAX  15u   /* UTF-8 bytes in a group name */

typedef struct {
    uint32_t    device;   /* device index = user address; one user per handheld */
    const char *name;
} lg_user_t;

typedef struct {
    uint16_t id;                              /* never 0, never reused once removed */
    uint32_t members;                         /* bit i set: roster user i belongs */
    char     name[LG_GROUP_NAME_MAX + 1u];    /* NUL-terminated */
} lg_group_t;

typedef struct {
    uint32_t   seq;       /* 0 = never changed anywhere; each change adds one */
    uint16_t   author;    /* AP index that made this version */
    uint16_t   next_id;   /* the id the next new group gets; only ever grows */
    uint8_t    count;
    lg_group_t groups[LG_MAX_GROUPS];
} lg_groups_t;

typedef struct {
    const lg_user_t *users;
    size_t           n_users;
    lg_groups_t      groups;
    uint16_t         version;
} lg_roster_t;

/* Group edits, from a handheld (LG_T_GROUP_EDIT) or from the admin page. */
typedef enum {
    LG_GROUP_CREATE = 1,   /* name and members; the new id is assigned */
    LG_GROUP_UPDATE = 2,   /* id, and its new name and members */
    LG_GROUP_DELETE = 3,   /* id */
} lg_group_op_t;

typedef struct {
    uint8_t  op;
    uint16_t id;
    uint32_t members;
    char     name[LG_GROUP_NAME_MAX + 1u];
} lg_group_edit_t;

const lg_user_t *lg_roster_user(const lg_roster_t *r, uint32_t device);
int              lg_roster_user_index(const lg_roster_t *r, uint32_t device);   /* -1 if unknown */
int              lg_roster_group_index(const lg_roster_t *r, uint16_t group_id); /* -1 if unknown */
const lg_group_t*lg_roster_group(const lg_roster_t *r, uint16_t group_id);
bool             lg_roster_is_member(const lg_roster_t *r, uint32_t device, uint16_t group_id);

/* Bits for every user in the roster: the only member bits a group may carry. */
uint32_t lg_roster_user_mask(const lg_roster_t *r);

/* True if version a is newer than version b. */
bool lg_groups_newer(const lg_groups_t *a, const lg_groups_t *b);

/*
 * Applies one edit to r's groups as a new version authored by AP `author`. editor is the
 * handheld asking, or 0 for the admin page. A handheld that creates a group is always a member
 * of it, and only a member may rename, change, or remove a group; the admin may do anything.
 * Returns 0 on success, or the lg_ack_status_t that refuses it (REJ_INVALID for a bad name or
 * members, a full table, or an unknown op; REJ_UNKNOWN_TARGET for no such group;
 * REJ_NOT_MEMBER). On refusal nothing changes.
 */
uint8_t lg_groups_apply_edit(lg_roster_t *r, uint32_t editor, const lg_group_edit_t *e, uint16_t author);

/* Group ids in old that new no longer has; returns how many were written (at most LG_MAX_GROUPS). */
size_t lg_groups_removed(const lg_groups_t *old, const lg_groups_t *new_groups, uint16_t *ids);

/* The prototype's fixed users with no groups: groups are made at run time (D52). */
void lg_roster_init_prototype(lg_roster_t *r);

#ifdef __cplusplus
}
#endif
