/*
 * lg_dedup.h - per-author duplicate suppression (RFC 4303 style sliding window).
 *
 * Each author gets one slot: the highest accepted (boot, seq) and a 128-bit
 * bitmap of the sequences just below it. Memory is caller-provided and fixed.
 * When all slots are used, the least recently used author is evicted; a
 * message replayed after its author was evicted is treated as new (documented
 * limit, bounded by slot count).
 */
#pragma once

#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LG_DEDUP_WINDOW 128u

typedef struct {
    uint32_t author;
    uint32_t boot;
    uint32_t top;     /* highest sequence accepted in this boot */
    uint64_t lo;      /* bit n set: sequence (top - n) seen, n in 0..63 */
    uint64_t hi;      /* bit n set: sequence (top - 64 - n) seen */
    uint32_t used;    /* LRU clock */
    uint8_t  in_use;
} lg_dedup_entry_t;

typedef struct {
    lg_dedup_entry_t *slots;
    size_t            count;
    uint32_t          clock;
} lg_dedup_t;

typedef enum {
    LG_DEDUP_NEW       = 0,
    LG_DEDUP_DUPLICATE = 1,
    LG_DEDUP_STALE     = 2,   /* older boot, sequence 0, or below the window */
} lg_dedup_result_t;

void lg_dedup_init(lg_dedup_t *d, lg_dedup_entry_t *slots, size_t count);

/* Classifies without recording. */
lg_dedup_result_t lg_dedup_check(lg_dedup_t *d, uint32_t author, uint32_t boot, uint32_t seq);

/* Classifies and records the id if it is new. */
lg_dedup_result_t lg_dedup_mark(lg_dedup_t *d, uint32_t author, uint32_t boot, uint32_t seq);

#ifdef __cplusplus
}
#endif
