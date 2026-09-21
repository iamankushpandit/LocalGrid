/*
 * lora_wire.h - the parts of the LoRa backbone (D71) that need no radio.
 *
 * Portable C11 over lg_core headers only, so tests/target exercises every one of them on a board
 * with no module fitted: base64, the part header, reassembly, the send policy and its bounded
 * queue, the RYLR998's AT replies, and the airtime one part costs. lora.c is the driver that
 * moves bytes; nothing here touches ESP-IDF, a UART, or a clock of its own.
 *
 * Wire format of one part (docs/lora.md, "Frames on the air").
 *
 * A sealed payload (backbone.c's outer 12 bytes + ciphertext + tag) is cut into slices of at most
 * LORA_SLICE_MAX bytes. Each slice gets a five-byte header in front of it:
 *
 *   0  u8  magic  LORA_MAGIC ('L')
 *   1  u8  sender: kind in the high nibble (0 an AP, 1 a handheld), index in the low nibble
 *   2  u8  message number, so parts of different payloads never mix
 *   3  u8  part number, 0-based
 *   4  u8  part count, 1..LORA_PARTS_MAX
 *
 * The sender is a whole byte because handhelds get modules later (docs/lora.md, "Ready for
 * handhelds later"): nothing here needs a format change when they do. This firmware only ever
 * sends, and only ever accepts, LORA_PEER_AP.
 *
 * The part number and the count are whole bytes for the same reason: a voice note or any other
 * payload of a few kilobytes is dozens of parts, not three, and widening the field later would be
 * a breaking change (docs/lora.md, "Push-to-talk over LoRa"). The format therefore carries
 * LORA_PAYLOAD_MAX bytes, the owner's 6 KB ceiling. A payload above what the AP has room for is
 * refused, loudly and counted, never half sent and never half kept.
 *
 * The header and the slice are then base64-encoded **together** into one token, which is what
 * `AT+SEND` carries. docs/lora.md's table describes the header as separate from the base64
 * payload; it cannot be, because `AT+SEND` takes text that ends at a newline and separates its
 * fields with commas, and a raw header byte can be either. Encoding the header with the slice
 * keeps the fields exactly as documented and keeps every byte on the air text-safe. The deviation
 * is written into docs/lora.md.
 *
 * Every size here is a hard bound. Nothing in this file allocates.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "lg_envelope.h"
#include "lg_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LORA_MAGIC          0x4Cu   /* 'L' */
#define LORA_PART_HDR       5u
/*
 * 180 base64 characters is what one AT+SEND carries (docs/lora.md). 180 characters decode to
 * exactly 135 bytes with no padding, of which five are the header.
 */
#define LORA_PART_B64_MAX   180u
#define LORA_PART_RAW_MAX   135u
#define LORA_SLICE_MAX      (LORA_PART_RAW_MAX - LORA_PART_HDR)   /* 130 */
#define LORA_PARTS_MAX      48u

/*
 * The payload ceiling the owner settled (docs/lora.md, "Push-to-talk over LoRa") is 6 KB: a
 * 30-second Codec2 voice note at 1200 bit/s, with headroom. 48 parts of 130 bytes hold 6240, which
 * is that ceiling rounded up to whole parts, so the wire format never has to change for one.
 * Anything larger is refused with a reason, never truncated.
 */
#define LORA_PAYLOAD_MAX    (LORA_PARTS_MAX * LORA_SLICE_MAX)   /* 6240 */

/*
 * Every frame the grid sends today is under 300 bytes. Slots of this size hold all of them, and
 * cost a few kilobytes of always-there memory; the 6 KB slots that a voice note would need are
 * taken once at start-up, and only on an AP that actually has a module (lora.c).
 */
#define LORA_SMALL_MAX      512u

_Static_assert(LORA_PAYLOAD_MAX >= 6144u, "the owner's ceiling is 6 KB");
_Static_assert(LORA_PART_HDR + LORA_SLICE_MAX == LORA_PART_RAW_MAX, "a part fills one AT+SEND");

/* ---- base64 ---- */

/*
 * Standard base64 with '=' padding. Returns the number of characters written (never NUL
 * terminated), or 0 when the output does not fit or len is 0.
 */
size_t lora_b64_encode(const uint8_t *in, size_t len, char *out, size_t cap);

/*
 * Returns the number of bytes written, or 0 when the input is not valid base64, is empty, or
 * does not fit. Whitespace is not accepted: what the module hands back is one token.
 */
size_t lora_b64_decode(const char *in, size_t len, uint8_t *out, size_t cap);

/* ---- parts ---- */

/* The kind nibble of the sender byte. */
#define LORA_PEER_AP        0x00u
#define LORA_PEER_HANDHELD  0x10u
#define LORA_PEER_KIND(b)   ((uint8_t)((b) & 0xF0u))
#define LORA_PEER_INDEX(b)  ((uint8_t)((b) & 0x0Fu))

typedef struct {
    uint8_t        peer;        /* kind | index, as on the air */
    uint8_t        from;        /* the index nibble on its own */
    uint8_t        id;          /* message number */
    uint8_t        part;        /* 0-based */
    uint8_t        of;          /* 1..LORA_PARTS_MAX */
    const uint8_t *slice;       /* into the caller's buffer */
    size_t         slice_len;
} lora_part_t;

/*
 * Writes header + slice into out. Returns the length written, or 0 on a bad argument. `peer` is
 * the sender byte: LORA_PEER_AP | this AP's index today.
 */
size_t lora_part_build(uint8_t peer, uint8_t id, uint8_t part, uint8_t of, const uint8_t *slice,
                       size_t slice_len, uint8_t *out, size_t cap);

/* Parses header + slice. Returns false for anything that is not one of our parts. */
bool lora_part_parse(const uint8_t *in, size_t len, lora_part_t *out);

/* How many parts a payload of this length needs, or 0 if it is beyond the format's ceiling. */
size_t lora_part_count(size_t payload_len);

/* ---- reassembly ---- */

#define LORA_ASM_SLOTS       3u
/*
 * A set is given up when no new part has arrived for LORA_ASM_TIMEOUT_MS (docs/lora.md's 10 s), or
 * when the whole set has been open for LORA_ASM_MAX_MS. The idle timeout, rather than a timeout
 * from the first part, is what lets a long payload of dozens of parts finish: each part restarts
 * the ten seconds, and the hard ceiling still stops a sender that trickles forever.
 */
#define LORA_ASM_TIMEOUT_MS  10000u
#define LORA_ASM_MAX_MS      180000u

/*
 * A slot's buffer belongs to the caller, so an AP can give reassembly two small slots that cost
 * nothing much and one large slot only when it has a module to fill it. A set is put in the
 * smallest slot that can hold it.
 *
 * A completed payload is never copied out: the slot is handed to the caller, which reads it where
 * it lies and then calls lora_asm_done. That is what lets a 6 KB payload cross without a second
 * 6 KB buffer anywhere in the AP.
 */
typedef struct {
    bool     in_use;
    bool     ready;         /* complete, and handed to the caller until lora_asm_done */
    uint8_t  peer;
    uint8_t  id;
    uint8_t  of;
    uint64_t have[(LORA_PARTS_MAX + 63u) / 64u];   /* bit n set: part n has arrived */
    uint16_t last_len;      /* length of the final part, once it has arrived */
    uint16_t total;         /* the whole payload's length, once the set is complete */
    uint32_t started_ms;
    uint32_t last_ms;       /* when the most recent part of this set arrived */
    uint8_t *data;
    size_t   cap;
} lora_asm_slot_t;

typedef struct {
    lora_asm_slot_t slots[LORA_ASM_SLOTS];
    uint32_t        timeouts;      /* incomplete sets given up */
    uint32_t        dropped;       /* parts refused: malformed, duplicate, oversized, or no slot */
} lora_asm_t;

typedef enum {
    LORA_ASM_NEED_MORE = 0,
    LORA_ASM_COMPLETE  = 1,
    LORA_ASM_DUPLICATE = 2,
    LORA_ASM_BAD       = 3,   /* not a part, or a part that disagrees with the set it claims */
    LORA_ASM_OVERSIZE  = 4,   /* larger than any slot this AP gave reassembly */
    LORA_ASM_FULL      = 5,   /* every slot is holding another set */
} lora_asm_result_t;

void lora_asm_init(lora_asm_t *a);

/* Gives slot `i` its memory. A slot with no buffer is never used. */
void lora_asm_set_slot(lora_asm_t *a, size_t i, uint8_t *buffer, size_t cap);

/* The largest payload any slot can hold, for refusing a set before it costs airtime. */
size_t lora_asm_capacity(const lora_asm_t *a);

/*
 * Takes one decoded part (header + slice). On LORA_ASM_COMPLETE *out is the slot holding the whole
 * payload, in its own ->data for ->total bytes; the caller reads it there and then calls
 * lora_asm_done. Every other result leaves *out NULL, and a->dropped counts what was refused.
 */
lora_asm_result_t lora_asm_feed(lora_asm_t *a, uint32_t now_ms, const uint8_t *part, size_t len,
                                lora_asm_slot_t **out);

/* Releases a slot lora_asm_feed handed over. */
void lora_asm_done(lora_asm_t *a, lora_asm_slot_t *slot);

/* Gives up sets that have gone quiet or run too long. Returns how many were freed this call. */
uint32_t lora_asm_expire(lora_asm_t *a, uint32_t now_ms);

/* ---- the send policy (docs/lora.md, "Always in parallel, without wasting airtime") ---- */

typedef enum {
    LORA_SEND_NEVER         = 0,   /* live voice (D61), and anything we cannot read */
    LORA_SEND_ALWAYS        = 1,   /* urgent broadcasts, SOS and all clear: both radios, always */
    LORA_SEND_IF_WIFI_DOWN  = 2,   /* everything else: only when ESP-NOW cannot carry it */
    LORA_SEND_SPARE_ROOM    = 3,   /* housekeeping: only when ESP-NOW cannot carry it AND the air is quiet */
} lora_policy_t;

/* Reads the envelope of an unsealed lg frame and says whether LoRa may carry it. */
lora_policy_t lora_policy_for_frame(const uint8_t *frame, size_t len);

/* Whether this frame is one of the alerts that must never wait behind anything. */
bool lora_is_alert_frame(const uint8_t *frame, size_t len);

/* ---- the bounded send queue ---- */

#define LORA_TXQ_SLOTS  5u

/*
 * A queued payload, with its own buffer from the caller for the same reason as a reassembly slot,
 * and with the progress of the transfer in it: a payload is transmitted straight out of its slot,
 * one part at a time, so nothing is ever copied to a staging buffer and an alert can take the
 * radio between two parts of a long transfer.
 */
typedef struct {
    bool     in_use;
    bool     alert;        /* rule 1 traffic: never dropped to make room, never made to wait */
    bool     sending;      /* parts of it are already on the air: never evicted, resumed in place */
    uint16_t dest;         /* LoRa address (docs/lora.md's address plan) */
    uint16_t len;
    uint8_t  id;           /* message number on the air */
    uint8_t  part;         /* the next part to transmit */
    uint8_t  of;
    uint32_t order;        /* arrival order, so a queue stays first in first out */
    uint32_t due_ms;       /* not before this: the random backoff and the longer retry wait */
    uint8_t *data;
    size_t   cap;
} lora_txq_slot_t;

typedef struct {
    lora_txq_slot_t slots[LORA_TXQ_SLOTS];
    uint32_t        next_order;
    uint32_t        dropped;    /* payloads the queue could not hold, never an alert already in it */
    uint16_t        depth;
    uint16_t        high;       /* high-water mark of depth */
} lora_txq_t;

void lora_txq_init(lora_txq_t *q);

/* Gives slot `i` its memory. A slot with no buffer is never used. */
void lora_txq_set_slot(lora_txq_t *q, size_t i, uint8_t *buffer, size_t cap);

/* The largest payload the queue can hold, for refusing one before it costs anything. */
size_t lora_txq_capacity(const lora_txq_t *q);

/* How many slots hold something now: housekeeping gives way once the queue is half full. */
size_t lora_txq_waiting(const lora_txq_t *q);

/*
 * Reserves the smallest free slot that can hold `cap` bytes, so the caller can build the payload
 * (seal it, say) straight into slot->data and then call lora_txq_commit with what it actually
 * wrote, or lora_txq_release to change its mind. Nothing is ever staged in a second buffer.
 * Returns NULL when the queue could not make room.
 */
lora_txq_slot_t *lora_txq_reserve(lora_txq_t *q, uint16_t dest, bool alert, uint8_t id, size_t cap,
                                  uint32_t due_ms);

/* Makes a reserved slot ready to transmit. False (and the slot is freed) if len does not fit. */
bool lora_txq_commit(lora_txq_t *q, lora_txq_slot_t *slot, size_t len);

/*
 * Adds one sealed payload, in the smallest free slot that holds it. When no slot is free the
 * oldest entry that is neither an alert nor already on the air makes room; when none can, the
 * newcomer is refused instead, so a queued alert is never lost and a transfer in progress is
 * never cut. `id` is the message number its parts will carry. Returns false when it was refused.
 */
bool lora_txq_push(lora_txq_t *q, uint16_t dest, bool alert, uint8_t id, const uint8_t *data, size_t len,
                   uint32_t due_ms);

/*
 * The next payload to transmit: the oldest alert that is due, otherwise the transfer already in
 * progress, otherwise the oldest entry that is due. Returns NULL when nothing is ready. The slot
 * stays in the queue, holding its progress, until lora_txq_release frees it.
 */
lora_txq_slot_t *lora_txq_peek(lora_txq_t *q, uint32_t now_ms);

/* Whether an alert is queued and due. The sender checks this between parts, so an alert waits at
 * most one part rather than a whole long transfer (docs/lora.md). */
bool lora_txq_alert_due(const lora_txq_t *q, uint32_t now_ms);

void lora_txq_release(lora_txq_t *q, lora_txq_slot_t *slot);

/* ---- the module's AT replies ---- */

typedef enum {
    LORA_AT_NONE = 0,   /* nothing we recognise */
    LORA_AT_OK,         /* +OK */
    LORA_AT_ERR,        /* +ERR=<n> */
    LORA_AT_RCV,        /* +RCV=<address>,<length>,<data>,<rssi>,<snr> */
    LORA_AT_VALUE,      /* +VER=..., +ADDRESS=..., +NETWORKID=..., +PARAMETER=... */
    LORA_AT_READY,      /* +READY, printed after a reset */
} lora_at_kind_t;

typedef struct {
    lora_at_kind_t kind;
    int            err;         /* LORA_AT_ERR */
    uint16_t       addr;        /* LORA_AT_RCV: who sent it */
    uint16_t       len;         /* LORA_AT_RCV: characters of data */
    const char    *data;        /* LORA_AT_RCV: into the line the caller passed in */
    int            rssi;
    int            snr;
    const char    *name;        /* LORA_AT_VALUE: "VER", "ADDRESS", ... */
    const char    *value;       /* LORA_AT_VALUE: what follows the '=' */
} lora_at_t;

/*
 * Parses one line the module printed, without its line ending. The line must stay alive while out
 * is used: data, name and value point into it. Returns false for LORA_AT_NONE.
 *
 * +RCV is parsed by its declared length rather than by counting commas, so a payload that
 * contained a comma would still be read correctly. Ours never does: it is base64.
 */
bool lora_at_parse(char *line, lora_at_t *out);

/* ---- airtime ---- */

/*
 * Time on the air of one LoRa transmission of `payload_chars` characters, in milliseconds,
 * rounded up, for the settings docs/lora.md names: SF9, BW 125 kHz, CR 4/5, preamble 12,
 * explicit header, CRC on. Semtech's formula (SX1276 datasheet 4.1.1.7), in integers.
 */
uint32_t lora_airtime_ms(size_t payload_chars);

#ifdef __cplusplus
}
#endif
