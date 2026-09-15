#include "lg_dedup.h"

#include <string.h>

void lg_dedup_init(lg_dedup_t *d, lg_dedup_entry_t *slots, size_t count)
{
    d->slots = slots;
    d->count = count;
    d->clock = 0;
    memset(slots, 0, count * sizeof(*slots));
}

static void shift_left(lg_dedup_entry_t *e, uint32_t n)
{
    if (n == 0) {
        return;
    }
    if (n >= 128u) {
        e->hi = 0;
        e->lo = 0;
    } else if (n >= 64u) {
        e->hi = e->lo << (n - 64u);
        e->lo = 0;
    } else {
        e->hi = (e->hi << n) | (e->lo >> (64u - n));
        e->lo <<= n;
    }
}

static bool get_bit(const lg_dedup_entry_t *e, uint32_t off)
{
    return off < 64u ? ((e->lo >> off) & 1u) != 0 : ((e->hi >> (off - 64u)) & 1u) != 0;
}

static void set_bit(lg_dedup_entry_t *e, uint32_t off)
{
    if (off < 64u) {
        e->lo |= (uint64_t)1 << off;
    } else {
        e->hi |= (uint64_t)1 << (off - 64u);
    }
}

static lg_dedup_entry_t *find(lg_dedup_t *d, uint32_t author)
{
    for (size_t i = 0; i < d->count; i++) {
        if (d->slots[i].in_use && d->slots[i].author == author) {
            return &d->slots[i];
        }
    }
    return NULL;
}

static lg_dedup_entry_t *alloc_slot(lg_dedup_t *d)
{
    lg_dedup_entry_t *victim = &d->slots[0];
    for (size_t i = 0; i < d->count; i++) {
        if (!d->slots[i].in_use) {
            return &d->slots[i];
        }
        if (d->slots[i].used < victim->used) {
            victim = &d->slots[i];
        }
    }
    return victim;
}

static void start_window(lg_dedup_entry_t *e, uint32_t boot, uint32_t seq)
{
    e->boot = boot;
    e->top  = seq;
    e->lo   = 1u;
    e->hi   = 0;
}

static lg_dedup_result_t process(lg_dedup_t *d, uint32_t author, uint32_t boot, uint32_t seq, bool record)
{
    if (seq == 0 || d->count == 0) {
        return LG_DEDUP_STALE;
    }
    d->clock++;

    lg_dedup_entry_t *e = find(d, author);
    if (e == NULL) {
        if (record) {
            e = alloc_slot(d);
            memset(e, 0, sizeof(*e));
            e->in_use = 1;
            e->author = author;
            e->used   = d->clock;
            start_window(e, boot, seq);
        }
        return LG_DEDUP_NEW;
    }

    e->used = d->clock;

    if (boot < e->boot) {
        return LG_DEDUP_STALE;
    }
    if (boot > e->boot) {
        if (record) {
            start_window(e, boot, seq);
        }
        return LG_DEDUP_NEW;
    }
    if (seq > e->top) {
        if (record) {
            shift_left(e, seq - e->top);
            e->lo |= 1u;
            e->top = seq;
        }
        return LG_DEDUP_NEW;
    }

    uint32_t off = e->top - seq;
    if (off >= LG_DEDUP_WINDOW) {
        return LG_DEDUP_STALE;
    }
    if (get_bit(e, off)) {
        return LG_DEDUP_DUPLICATE;
    }
    if (record) {
        set_bit(e, off);
    }
    return LG_DEDUP_NEW;
}

lg_dedup_result_t lg_dedup_check(lg_dedup_t *d, uint32_t author, uint32_t boot, uint32_t seq)
{
    return process(d, author, boot, seq, false);
}

lg_dedup_result_t lg_dedup_mark(lg_dedup_t *d, uint32_t author, uint32_t boot, uint32_t seq)
{
    return process(d, author, boot, seq, true);
}
