#include "lg_roster.h"

#include <string.h>

#include "lg_body.h"

/* Placeholder names until handhelds are paired and named by the admin. */
static const lg_user_t proto_users[] = {
    { 1, "Handheld 1" },
    { 2, "Handheld 2" },
    { 3, "Handheld 3" },
    { 4, "Handheld 4" },
    { 5, "Handheld 5" },   /* appended: a user's index is its bit in group and announcer masks */
    { 6, "Handheld 6" },
    { 7, "Handheld 7" },   /* the alert and distress unit (D66); urgent broadcasts need a roster index */
    { 8, "Handheld 8" },   /* a second E28 joined the bench when one board's cloned identity was undone */
    { 9, "Handheld 9" },   /* a third Freenove. Room to add a handheld without a firmware change is */
    { 10, "Handheld 10" }, /* worth having: a device number with no roster entry cannot join at all */
    { 11, "Handheld 11" },
    { 12, "Handheld 12" },
};

void lg_roster_init_prototype(lg_roster_t *r)
{
    memset(r, 0, sizeof(*r));
    r->users   = proto_users;
    r->n_users = sizeof(proto_users) / sizeof(proto_users[0]);
    r->groups.next_id = 1;
    r->groups.announcers = LG_ANNOUNCE_EVERYONE;   /* everyone announces until the admin narrows it (D56) */
    r->version = 6;   /* 3: Handheld 5 and 6; 4: Handheld 7; 5: Handheld 8; 6: 9 to 12, for headroom */
}

int lg_roster_user_index(const lg_roster_t *r, uint32_t device)
{
    /*
     * A roster nobody has filled in yet. This is not hypothetical: a handheld whose device number
     * is not in the roster refuses to start and says so, and saying so asks for a name before the
     * roster exists. On the bench, 2026-09-20, that read through a null table and left the board
     * in a boot loop, so the message explaining the problem never reached anyone.
     */
    if (r == NULL || r->users == NULL) {
        return -1;
    }
    for (size_t i = 0; i < r->n_users; i++) {
        if (r->users[i].device == device) {
            return (int)i;
        }
    }
    return -1;
}

const lg_user_t *lg_roster_user(const lg_roster_t *r, uint32_t device)
{
    int i = lg_roster_user_index(r, device);
    return i < 0 ? NULL : &r->users[i];
}

int lg_roster_group_index(const lg_roster_t *r, uint16_t group_id)
{
    for (size_t i = 0; i < r->groups.count && i < LG_MAX_GROUPS; i++) {
        if (r->groups.groups[i].id == group_id) {
            return (int)i;
        }
    }
    return -1;
}

const lg_group_t *lg_roster_group(const lg_roster_t *r, uint16_t group_id)
{
    int i = lg_roster_group_index(r, group_id);
    return i < 0 ? NULL : &r->groups.groups[i];
}

bool lg_roster_is_member(const lg_roster_t *r, uint32_t device, uint16_t group_id)
{
    int u = lg_roster_user_index(r, device);
    const lg_group_t *g = lg_roster_group(r, group_id);
    return u >= 0 && u < 32 && g != NULL && (g->members & (1u << (unsigned)u)) != 0;
}

bool lg_roster_may_announce(const lg_roster_t *r, uint32_t device)
{
    int ui = lg_roster_user_index(r, device);
    return ui >= 0 && ui < 32 && (r->groups.announcers & (1u << (unsigned)ui)) != 0;
}

uint32_t lg_roster_user_mask(const lg_roster_t *r)
{
    size_t n = r->n_users < 32u ? r->n_users : 32u;
    return n == 32u ? 0xFFFFFFFFu : (1u << n) - 1u;
}

bool lg_groups_newer(const lg_groups_t *a, const lg_groups_t *b)
{
    return a->seq > b->seq || (a->seq == b->seq && a->author > b->author);
}

/* strnlen is POSIX, not C11. */
static size_t bounded_len(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

static bool name_ok(const char *name)
{
    size_t n = bounded_len(name, LG_GROUP_NAME_MAX + 1u);
    return n >= 1u && n <= LG_GROUP_NAME_MAX && lg_utf8_valid((const uint8_t *)name, n);
}

uint8_t lg_groups_apply_edit(lg_roster_t *r, uint32_t editor, const lg_group_edit_t *e, uint16_t author)
{
    lg_groups_t *t = &r->groups;
    uint32_t known = lg_roster_user_mask(r);
    int editor_ui = -1;
    if (editor != 0) {
        editor_ui = lg_roster_user_index(r, editor);
        if (editor_ui < 0 || editor_ui >= 32) {
            return LG_ACK_REJ_INVALID;
        }
    }
    uint32_t editor_bit = editor_ui >= 0 ? 1u << (unsigned)editor_ui : 0u;

    switch (e->op) {
    case LG_GROUP_CREATE: {
        if (!name_ok(e->name) || (e->members & ~known) != 0 || t->count >= LG_MAX_GROUPS || t->next_id == 0) {
            return LG_ACK_REJ_INVALID;   /* next_id 0: every id has been used once */
        }
        lg_group_t *g = &t->groups[t->count++];
        memset(g, 0, sizeof(*g));
        g->id = t->next_id++;
        g->members = e->members | editor_bit;   /* whoever makes a group is in it */
        memcpy(g->name, e->name, bounded_len(e->name, LG_GROUP_NAME_MAX));
        break;
    }
    case LG_GROUP_UPDATE:
    case LG_GROUP_DELETE: {
        int gi = lg_roster_group_index(r, e->id);
        if (gi < 0) {
            return LG_ACK_REJ_UNKNOWN_TARGET;
        }
        lg_group_t *g = &t->groups[gi];
        if (editor != 0 && (g->members & editor_bit) == 0) {
            return LG_ACK_REJ_NOT_MEMBER;
        }
        if (e->op == LG_GROUP_DELETE) {
            for (size_t i = (size_t)gi; i + 1u < t->count; i++) {
                t->groups[i] = t->groups[i + 1u];
            }
            t->count--;
            memset(&t->groups[t->count], 0, sizeof(t->groups[0]));
            break;
        }
        if (!name_ok(e->name) || (e->members & ~known) != 0 || e->members == 0) {
            return LG_ACK_REJ_INVALID;   /* an empty group is removed, not kept */
        }
        g->members = e->members;
        memset(g->name, 0, sizeof(g->name));
        memcpy(g->name, e->name, bounded_len(e->name, LG_GROUP_NAME_MAX));
        break;
    }
    case LG_GROUP_ANNOUNCERS:
        if (editor != 0) {
            return LG_ACK_REJ_NOT_ALLOWED;   /* who may announce is the admin page's to set (D56) */
        }
        if ((e->members & ~known) != 0 && e->members != LG_ANNOUNCE_EVERYONE) {
            return LG_ACK_REJ_INVALID;
        }
        t->announcers = e->members;
        break;
    default:
        return LG_ACK_REJ_INVALID;
    }
    t->seq++;
    t->author = author;
    return 0;
}

size_t lg_groups_removed(const lg_groups_t *old, const lg_groups_t *new_groups, uint16_t *ids)
{
    size_t n = 0;
    for (size_t i = 0; i < old->count && i < LG_MAX_GROUPS; i++) {
        bool kept = false;
        for (size_t k = 0; k < new_groups->count && k < LG_MAX_GROUPS; k++) {
            if (new_groups->groups[k].id == old->groups[i].id) {
                kept = true;
                break;
            }
        }
        if (!kept) {
            ids[n++] = old->groups[i].id;
        }
    }
    return n;
}
