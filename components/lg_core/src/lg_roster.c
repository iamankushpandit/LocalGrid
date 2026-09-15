#include "lg_roster.h"

#define BIT_FAMILY  (1u << 0)
#define BIT_KIDS    (1u << 1)
#define BIT_LEADERS (1u << 2)

static const lg_group_t proto_groups[] = {
    { LG_PROTO_FAMILY,  "FAMILY"  },
    { LG_PROTO_KIDS,    "KIDS"    },
    { LG_PROTO_LEADERS, "LEADERS" },
};

/* Placeholder names until handhelds are paired and named by the admin. */
static const lg_user_t proto_users[] = {
    { LG_PROTO_DAD,    "Handheld 1", BIT_FAMILY | BIT_LEADERS },
    { LG_PROTO_EMMA,   "Handheld 2", BIT_FAMILY | BIT_KIDS    },
    { LG_PROTO_ALEX,   "Handheld 3", BIT_FAMILY | BIT_KIDS    },
    { LG_PROTO_RANGER, "Handheld 4", BIT_LEADERS              },
};

static const lg_roster_t proto_roster = {
    .users    = proto_users,
    .n_users  = sizeof(proto_users) / sizeof(proto_users[0]),
    .groups   = proto_groups,
    .n_groups = sizeof(proto_groups) / sizeof(proto_groups[0]),
    .version  = 1,
};

const lg_roster_t *lg_roster_prototype(void)
{
    return &proto_roster;
}

int lg_roster_user_index(const lg_roster_t *r, uint32_t device)
{
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
    for (size_t i = 0; i < r->n_groups && i < LG_MAX_GROUPS; i++) {
        if (r->groups[i].id == group_id) {
            return (int)i;
        }
    }
    return -1;
}

const lg_group_t *lg_roster_group(const lg_roster_t *r, uint16_t group_id)
{
    int i = lg_roster_group_index(r, group_id);
    return i < 0 ? NULL : &r->groups[i];
}

bool lg_roster_is_member(const lg_roster_t *r, uint32_t device, uint16_t group_id)
{
    const lg_user_t *u = lg_roster_user(r, device);
    int g = lg_roster_group_index(r, group_id);
    return u != NULL && g >= 0 && (u->groups & (1u << (unsigned)g)) != 0;
}
