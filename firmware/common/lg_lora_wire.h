/*
 * lg_lora_wire.h - the parts of the LoRa backbone (D71) that need no radio.
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
 * 120 base64 characters is what one AT+SEND carries; they decode to exactly 90 bytes with no
 * padding, of which five are the header.
 *
 * The first sketch carried 180 (docs/lora.md). Measured on the bench on 2026-10-08 (RYLR998
 * firmware V1.2.4, SF9 BW125, 868.5 MHz, RSSI -43 dBm, SNR 9, quiet channel), delivery fell off a
 * cliff with payload length: 124 characters or less arrived 24 of 24, 132 arrived 11 of 12, 140
 * arrived 2 of 12, and 180 almost never - module behaviour, not the air, and exactly the range a
 * sealed text message landed in. 120 keeps one measured step of margin below the cliff.
 */
#define LORA_PART_B64_MAX   120u
#define LORA_PART_RAW_MAX   90u
#define LORA_SLICE_MAX      (LORA_PART_RAW_MAX - LORA_PART_HDR)   /* 85 */
#define LORA_PARTS_MAX      74u

/*
 * The payload ceiling the owner settled (docs/lora.md, "Push-to-talk over LoRa") is 6 KB: a
 * 30-second Codec2 voice note at 1200 bit/s, with headroom. 74 parts of 85 bytes hold 6290, which
 * is that ceiling rounded up to whole parts, so the wire format never has to change for one.
 * Anything larger is refused with a reason, never truncated.
 */
#define LORA_PAYLOAD_MAX    (LORA_PARTS_MAX * LORA_SLICE_MAX)   /* 6290 */

/*
 * Every frame the grid sends today is under 300 bytes. Slots of this size hold all of them, and
 * cost a few kilobytes of always-there memory; the 6 KB slots that a voice note would need are
 * taken once at start-up, and only on an AP that actually has a module (lora.c).
 */
#define LORA_SMALL_MAX      512u

_Static_assert(LORA_PAYLOAD_MAX >= 6144u, "the owner's ceiling is 6 KB");
_Static_assert(LORA_PART_HDR + LORA_SLICE_MAX == LORA_PART_RAW_MAX, "a part fills one AT+SEND");

/* ---- the radio, and the address plan (D71, D77) ---------------------------------------------
 *
 * One source of truth for both firmwares. An AP and a handheld that disagree on any of these are
 * on different channels and neither ever hears the other, which is a bug you find on a field trip
 * rather than at a desk -- so they live here, in the file both sides already compile, and not in
 * two headers that cannot include each other (D27).
 *
 * Band: 868.5 MHz is the European licence-free LoRa band. A 915 MHz region changes this one number
 * (and only this one) before flashing every board in the grid.
 */
#define LG_LORA_BAND_HZ      868500000u
#define LG_LORA_UART_BAUD    115200u     /* the RYLR998 leaves the factory at this rate */
/* AT+PARAMETER: spreading factor 9, bandwidth 125 kHz (code 7), coding rate 4/5 (code 1),
 * preamble 12 -- the module's own default, and required while the network ID is left at 18. */
#define LG_LORA_SF           9
#define LG_LORA_BW_CODE      7
#define LG_LORA_CR_CODE      1
#define LG_LORA_PREAMBLE     12
#define LG_LORA_POWER_DBM    22          /* AT+CRFOP, 0..22 */
/*
 * How strongly the LoRa radio transmits (D98). Full is what reaches furthest and is the default. The
 * lower two exist for one situation: devices close together - on one table, in one room - where a
 * module at full power swamps its neighbours' receivers and they lose frames they would otherwise
 * hear. The bench is that situation: D95's voice figures were only reached with the modules turned
 * down by hand. It is a setting on each device and is not shared across the grid.
 */
#define LG_LORA_POWER_FULL   22
#define LG_LORA_POWER_MEDIUM 14
#define LG_LORA_POWER_LOW    5
static inline int lg_lora_power_clamp(int dbm)
{
    return dbm <= (LG_LORA_POWER_LOW + LG_LORA_POWER_MEDIUM) / 2 ? LG_LORA_POWER_LOW
         : dbm <= (LG_LORA_POWER_MEDIUM + LG_LORA_POWER_FULL) / 2 ? LG_LORA_POWER_MEDIUM : LG_LORA_POWER_FULL;
}
static inline const char *lg_lora_power_name(int dbm)
{
    return dbm == LG_LORA_POWER_LOW ? "low" : dbm == LG_LORA_POWER_MEDIUM ? "medium" : "full";
}

/*
 * The address plan. Address 0 is the broadcast address and nothing else: the bench proved on
 * 2026-09-20 that a module cannot be both a broadcast target and a private one, so no AP sits
 * there. APs are 1 + index; a handheld is 100 + its device number (D71 settled this before
 * handhelds had modules, so D77 needed no change to it).
 */
#define LG_LORA_ADDR_BROADCAST   0u
#define LG_LORA_ADDR_AP(i)       ((uint16_t)((i) + 1u))
#define LG_LORA_ADDR_HANDHELD(d) ((uint16_t)(100u + (d)))

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

/*
 * The parity part (2026-10-09). A multi-part message-rank frame is followed by one extra part
 * carrying the XOR of every data slice zero-padded to LORA_SLICE_MAX, so a receiver that lost
 * exactly one part rebuilds it on the spot instead of waiting a resend round trip - on a channel
 * where that trip costs eight seconds, this is the difference between late and lost. Its header
 * is an ordinary part header with part == of, which firmware from before this change never
 * accepts: an AP's decode buffer stops it at 91 raw bytes with a loud garbled-part warning, and
 * the parser would refuse part == of regardless - so a mixed bench degrades gracefully (old
 * firmware just never heals), but its warning counts read as loss until it is reflashed, which
 * is worth knowing before reading a sweep on a half-flashed grid. Its slice starts with one
 * byte naming the final data part's true length, because a rebuilt LAST part has no other way to
 * know where the payload ends, and the XOR of padded slices follows. At 91 raw bytes it rides in
 * 124 base64 characters, which the 2026-10-08 length sweep measured clean (24/24; the module's
 * cliff starts near 130).
 */
#define LORA_PARITY_SLICE   (LORA_SLICE_MAX + 1u)                 /* last_len byte + the XOR block */
#define LORA_PART_RAW_WIRE  (LORA_PART_HDR + LORA_PARITY_SLICE)   /* 91: the largest part on the air */
#define LORA_PART_B64_WIRE  (((LORA_PART_RAW_WIRE + 2u) / 3u) * 4u)   /* 124 characters, measured clean */

_Static_assert(LORA_PART_B64_WIRE == 124u, "the parity part rides in the 124 characters the sweep cleared");

/* Writes the parity part for a whole payload into out. Returns its length, 0 on a bad argument
 * (a payload of one part takes no parity). */
size_t lora_parity_build(uint8_t peer, uint8_t id, const uint8_t *payload, size_t payload_len,
                         uint8_t *out, size_t cap);

/* Parses a parity part. Returns false for anything else, data parts included. */
bool lora_parity_parse(const uint8_t *in, size_t len, lora_part_t *out);

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
    bool     have_parity;   /* the set's parity part arrived; its slice is in `parity` */
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
    uint8_t  parity[LORA_PARITY_SLICE];
} lora_asm_slot_t;

typedef struct {
    lora_asm_slot_t slots[LORA_ASM_SLOTS];
    uint32_t        timeouts;      /* incomplete sets given up */
    uint32_t        dropped;       /* parts refused: malformed, duplicate, oversized, or no slot */
    uint32_t        healed;        /* parts rebuilt from a parity part instead of a resend */
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

/*
 * A frame of voice in the compact codec (D95). Between two APs, and from an AP to a handheld, it goes
 * once and is acknowledged and resent like text (lora_frame_wants_ap_ack, D104, D109). Only a
 * handheld's own notes still go twice: its frames are broadcast, so no one AP owes it an answer. The
 * second copy is sealed afresh, so the link does not see a replay; the core's newest-only rule for
 * voice drops whichever copy comes second.
 */
bool lora_frame_is_compact_voice(const uint8_t *frame, size_t len);

/* Whether this frame is one of the alerts that must never wait behind anything. */
bool lora_is_alert_frame(const uint8_t *frame, size_t len);

/*
 * What a frame is worth when the queue is full (D102). Three ranks:
 *   LORA_RANK_ALERT    an urgent broadcast, an SOS, an all clear: never dropped to make room
 *   LORA_RANK_MESSAGE  what a person said and the word that it arrived: text, its acknowledgement,
 *                      and a compact voice packet
 *   LORA_RANK_ROUTINE  everything the grid says to itself: presence, positions, trails, time, names,
 *                      settings. All of it is sent again within a minute anyway.
 */
#define LORA_RANK_ROUTINE 0u
#define LORA_RANK_MESSAGE 1u
#define LORA_RANK_ALERT   2u
uint8_t lora_frame_rank(const uint8_t *frame, size_t len);

/*
 * Whether the AP that receives this frame over LoRa says so, and the AP that sent it sends it again
 * until it does (D104): what a person wrote, the word that it arrived, and an alert. Not voice, which
 * is late by the time a second copy could go, and not routine traffic, which is repeated anyway. Both
 * ends ask this of the same frame, so they never disagree about whether an answer is owed.
 */
bool lora_frame_wants_ack(const uint8_t *frame, size_t len);

/*
 * The same question between two APs, where a compact voice packet is acknowledged too (D95, amended
 * 2026-10-07). Sent twice, each packet took about 3.8 s of air against one every 2.8 s from the talker,
 * the AP's four-slot queue overflowed, and a five-packet note arrived as one. Sent once and answered,
 * it costs a second copy's airtime only when the first is lost. A handheld's LoRa link is unchanged: it
 * still says a note twice and acknowledges none of it.
 */
bool lora_frame_wants_ap_ack(const uint8_t *frame, size_t len);

/*
 * The acknowledgement (D104): six bytes sealed with the backbone key like any frame between APs, so it
 * is authenticated and cannot be replayed, and far too short to be mistaken for an envelope. It names
 * the outer sequence number of the sealed frame it answers; who sent it is in its own outer header.
 */
#define LORA_ACK_LEN   6u
#define LORA_ACK_TAG0  0x4Cu   /* 'L' */
#define LORA_ACK_TAG1  0x41u   /* 'A' */
size_t lora_ack_build(uint32_t seq, uint8_t *out, size_t cap);
bool   lora_ack_parse(const uint8_t *in, size_t len, uint32_t *seq);

#define LORA_TX_TRIES  3u      /* times a frame that wants an acknowledgement goes on the air before it is given up */
/*
 * D109: in a slot's `await`, the one handheld the frame is addressed to, rather than an access point
 * by index. Its acknowledgement is matched on this bit, and a repeat keeps the handheld's address.
 */
#define LORA_AWAIT_HANDHELD 0x8000u

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
    bool     parity;       /* a parity part follows the data: message rank, two parts or more */
    uint8_t  rank;         /* LORA_RANK_*: what gives way to what when the queue is full */
    bool     sending;      /* parts of it are already on the air: never evicted, resumed in place */
    uint16_t dest;         /* LoRa address (docs/lora.md's address plan) */
    uint16_t len;
    uint8_t  id;           /* message number on the air */
    uint8_t  part;         /* the next part to transmit */
    uint8_t  of;
    uint32_t order;        /* arrival order, so a queue stays first in first out */
    uint32_t due_ms;       /* not before this: the random backoff and the longer retry wait */
    bool     waiting;      /* all of it has been on the air and an acknowledgement is awaited (D104) */
    bool     is_ack;       /* it is an acknowledgement: sent even while this AP is listening for its own */
    uint8_t  tries;        /* times all of it has been on the air */
    uint16_t await;        /* bit n set: AP n has not acknowledged it yet. 0: none is wanted */
    uint32_t seq;          /* the sealed frame's outer sequence number, which an acknowledgement names */
    uint8_t *data;
    size_t   cap;
} lora_txq_slot_t;

typedef struct {
    lora_txq_slot_t slots[LORA_TXQ_SLOTS];
    uint32_t        next_order;
    uint32_t        dropped;    /* payloads the queue could not hold, never an alert already in it */
    uint32_t        dropped_messages;   /* how many of those were a person's message or above, not routine */
    uint32_t        acked;      /* frames every awaited AP acknowledged (D104) */
    uint32_t        resent;     /* times a frame went out again for want of an acknowledgement */
    uint32_t        unacked;    /* frames given up: sent LORA_TX_TRIES times, or pushed out while waiting */
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

/*
 * The same, saying what the payload is worth (LORA_RANK_*). In a full queue the newcomer takes the
 * place of the oldest entry of the lowest rank there is, provided that rank is no higher than its
 * own: a message pushes out routine traffic before another message, and routine traffic never pushes
 * out a message - it is the one refused. Nothing pushes out an alert or a transfer already on the
 * air. lora_txq_reserve is this with LORA_RANK_ALERT or LORA_RANK_ROUTINE.
 */
lora_txq_slot_t *lora_txq_reserve_ranked(lora_txq_t *q, uint16_t dest, uint8_t rank, uint8_t id, size_t cap,
                                         uint32_t due_ms);

/*
 * Whether lora_txq_reserve_ranked would find a place for such a payload now, changing nothing and
 * counting nothing. For a sender that would rather wait a moment than be refused: the heartbeat, which
 * is routine traffic and so is refused by a queue full of messages, and which must not then stay silent
 * for a whole minute while its neighbours conclude the link is dead.
 */
bool lora_txq_would_take(const lora_txq_t *q, uint8_t rank, size_t cap);

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

/*
 * The same while this AP is listening for an acknowledgement (`listening`): only an alert or an
 * acknowledgement it owes somebody else is returned. The module cannot hear while it transmits, so
 * sending the next message at once would talk over the very answer being waited for.
 *
 * Either call also deals with frames whose wait has run out: one that has not been on the air
 * LORA_TX_TRIES times becomes due again from its first part - addressed to the one AP still to answer
 * when there is only one, so the others' radios discard it unread - and one that has is given up.
 */
lora_txq_slot_t *lora_txq_peek_for(lora_txq_t *q, uint32_t now_ms, bool listening);

/*
 * Says that a committed slot wants an acknowledgement from each AP whose bit is set in `await`, and
 * what sequence number those acknowledgements will name.
 */
void lora_txq_want_ack(lora_txq_slot_t *slot, uint32_t seq, uint16_t await);

/*
 * The last part of `slot` has gone out. A frame nobody is to acknowledge is finished and its slot is
 * freed; one that is awaited stays, not transmitted again before `resend_ms`. Returns whether it stays.
 */
bool lora_txq_sent(lora_txq_t *q, lora_txq_slot_t *slot, uint32_t resend_ms);

/* AP `from` acknowledged the frame sealed as `seq`. True if a frame here was waiting for exactly that. */
bool lora_txq_acked(lora_txq_t *q, uint32_t seq, uint8_t from);

/* Whether any frame is waiting for an acknowledgement now. */
bool lora_txq_awaiting(const lora_txq_t *q);

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
