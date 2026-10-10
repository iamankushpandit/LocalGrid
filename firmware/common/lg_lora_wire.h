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
 * LORA_SLICE_MAX bytes. Each slice gets a six-byte header in front of it:
 *
 *   0  u8  magic  LORA_MAGIC
 *   1  u8  sender: kind in the high nibble (0 an AP, 1 a handheld), index in the low nibble
 *   2  u8  message number, so parts of different payloads never mix
 *   3  u8  part number, 0-based
 *   4  u8  part count, 1..LORA_PARTS_MAX
 *   5  u8  who it is for: 0 everyone, otherwise that radio's place in the address plan below
 *          (1 + index for an AP, 100 + device number for a handheld)
 *
 * The sixth byte (2026-10-09) is there so that every part can be sent to the module's broadcast
 * address. The module hides from a radio whatever is addressed to another one, and has no carrier
 * sense, so an AP could not know that its two neighbours were in the middle of a frame and
 * transmitted over it: on the bench, with three APs in one room, that was where most parts were
 * still being lost once the radios themselves had stopped losing them. Now every radio hears
 * every part, takes the ones that are for it, and keeps off the air for the rest of the others.
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

/*
 * 'M'. It was 'L' while the header was five bytes and a part did not say who it was for (until
 * 2026-10-09). A board still on that format is refused at the first byte: without this the two
 * would hear each other and each read the other's parts one byte out of place, which looks
 * exactly like an unreliable radio.
 */
#define LORA_MAGIC          0x4Du
#define LORA_PART_HDR       6u
#define LORA_DEST_ALL       0u      /* header byte 5: for every radio that hears it */
/*
 * A part carries 85 bytes of payload: with the six-byte header 91 bytes, 124 characters of
 * base64, about two thirds of a second on the air at SF9 and 125 kHz. The parity part (below) is
 * one byte longer and the same 124 characters.
 *
 * What bounds it is time on the air, not characters (LG_LORA_AIR_CEILING_MS, at the end of this
 * file). The module carries 240 characters in one AT+SEND, and from 2026-10-09 18:00 to 20:00 a
 * part was 240: probes sent back to back had arrived 40 of 40 at that length. Sent the way real
 * traffic is sent, one packet after seconds of silence, 200 and 240 characters arrived 1 of 32
 * and 1 of 56 - broken, the module's own checksum failing - while 120, 140 and 160 arrived 28 of
 * 28, 31 of 32 and 55 of 56, at 5 dBm and at 22 dBm alike. A packet from rest is good for a little
 * under a second on the air. A faster radio setting can carry more in a part (240 characters are
 * 0.3 s at SF9 and 500 kHz); the assert at the end of this file is what says whether it may.
 */
#define LORA_SLICE_MAX      85u
#define LORA_PART_RAW_MAX   (LORA_PART_HDR + LORA_SLICE_MAX)             /* 91 */
#define LORA_PART_B64_MAX   (((LORA_PART_RAW_MAX + 2u) / 3u) * 4u)       /* 124 */
/*
 * The payload ceiling the owner settled (docs/lora.md, "Push-to-talk over LoRa") is 6 KB: a
 * 30-second Codec2 voice note at 1200 bit/s, with headroom. 73 parts of 85 bytes hold 6205, which
 * is that ceiling rounded up to whole parts, so the wire format never has to change for one.
 * Anything larger is refused with a reason, never truncated.
 */
#define LORA_PARTS_MAX      ((6144u + LORA_SLICE_MAX - 1u) / LORA_SLICE_MAX)   /* 73 */
#define LORA_PAYLOAD_MAX    (LORA_PARTS_MAX * LORA_SLICE_MAX)                  /* 6205 */

/*
 * Every frame the grid sends today is under 320 bytes. Slots of this size hold all of them, and
 * cost a few kilobytes of always-there memory; the 6 KB slots that a voice note would need are
 * taken once at start-up, and only on an AP that actually has a module (lora.c). A whole number
 * of parts, because reassembly takes a set only if all its parts would fit the slot at full size:
 * a queue slot that accepts more than a reassembly slot can take back sends frames nobody keeps.
 */
#define LORA_SMALL_PARTS    6u
#define LORA_SMALL_MAX      (LORA_SMALL_PARTS * LORA_SLICE_MAX)   /* 510 */

_Static_assert(LORA_PAYLOAD_MAX >= 6144u, "the owner's ceiling is 6 KB");
_Static_assert(LORA_PART_B64_MAX <= 240u, "a part fits one AT+SEND");
_Static_assert(LORA_SLICE_MAX <= 255u, "the parity part carries the last part's length in one byte");
_Static_assert(LORA_PARTS_MAX <= 255u, "the part number and count are one byte each");
_Static_assert(LORA_PAYLOAD_MAX <= 65535u, "a slot's length is sixteen bits");
_Static_assert(LORA_SMALL_MAX >= 288u + 12u + 16u, "a small slot holds the largest sealed frame");

/* ---- the radio, and the address plan (D71, D77) ---------------------------------------------
 *
 * One source of truth for both firmwares. An AP and a handheld that disagree on any of these are
 * on different channels and neither ever hears the other, which is a bug you find on a field trip
 * rather than at a desk -- so they live here, in the file both sides already compile, and not in
 * two headers that cannot include each other (D27).
 *
 * Region (owner, 2026-10-09): where the grid is used decides the band, and it is chosen by name,
 * here, before flashing every board in the grid. North America is the default - the grid's first
 * trip is there, and 915 MHz is also what the module itself comes up on, so one that restarts by
 * itself is at least on the right band. Europe is the other region known so far. The frequency is
 * all a region sets today; what each allows in power and airtime is the builder's to check.
 *
 * It is a build setting and not yet a setting of the grid. A band two devices disagree on splits
 * the grid without a word, so the place for it is the grid's shared settings, chosen once at setup
 * and adopted by every AP and handheld; until that exists, one name here is the whole of it.
 */
#define LG_LORA_REGION_US915 1           /* North America: 902-928 MHz */
#define LG_LORA_REGION_EU868 2           /* Europe: 863-870 MHz */
#ifndef LG_LORA_REGION
#define LG_LORA_REGION       LG_LORA_REGION_US915
#endif
#if LG_LORA_REGION == LG_LORA_REGION_US915
#define LG_LORA_BAND_HZ      915000000u
#elif LG_LORA_REGION == LG_LORA_REGION_EU868
#define LG_LORA_BAND_HZ      868500000u
#else
#error "LG_LORA_REGION names no region this firmware knows"
#endif
#define LG_LORA_UART_BAUD    115200u     /* the RYLR998 leaves the factory at this rate */
/* AT+PARAMETER: spreading factor 9, bandwidth 125 kHz (code 7), coding rate 4/5 (code 1),
 * preamble 12 -- the module's own default, and required while the network ID is left at 18. */
#define LG_LORA_SF           9
#define LG_LORA_BW_CODE      7
#define LG_LORA_CR_CODE      1
#define LG_LORA_PREAMBLE     12
/* The module's bandwidth codes: 7 is 125 kHz, 8 is 250, 9 is 500. */
#define LG_LORA_BW_HZ        (LG_LORA_BW_CODE == 7 ? 125000u : LG_LORA_BW_CODE == 8 ? 250000u : 500000u)
_Static_assert(LG_LORA_BW_CODE >= 7 && LG_LORA_BW_CODE <= 9, "the module knows bandwidth codes 7, 8 and 9");
_Static_assert(LG_LORA_SF >= 7 && LG_LORA_SF <= (LG_LORA_BW_CODE == 7 ? 9 : LG_LORA_BW_CODE == 8 ? 10 : 11),
               "the module allows SF7-9 at 125 kHz, SF7-10 at 250 and SF7-11 at 500");
#define LG_LORA_POWER_DBM    22          /* AT+CRFOP, 0..22 */
/*
 * AT+NETWORKID: 18, the module's factory value, and nothing else.
 *
 * Until 2026-10-09 every grid took one of 3..15 from its discriminator, so that a module from
 * another grid would be ignored by the radio itself. Measured that day on three APs side by side
 * (SF9, 125 kHz, 915 MHz, 5 dBm, RSSI -40, one AP sending 60 packets of 120 characters with nothing
 * else on the air, the two others counting): on 18 both heard 60 of 60, three runs of three; on
 * 14, 3 and 9 they heard 43 to 56 of 60, each missing different packets, with no error from the
 * module - the packet simply never came out of it. One packet in six, lost in silence, between
 * radios centimetres apart, is what most "LoRa lost a part" on this bench had underneath it: the
 * quiet a receiver seemed to need after a packet, the resends, and a length limit that looked
 * tighter than it is. The module's guide allows the preamble to be set only on 18 and pins it to
 * 12 on every other ID; whatever it does differently there, 18 is the setting its receiver was
 * built around.
 *
 * What is given up: the radio no longer tells one LocalGrid from another, or from a stranger's
 * module left on factory settings. Nothing depended on it. Every frame is sealed with the grid's
 * own key and a part that does not open is dropped (D71, D77); a stranger's packet costs the air
 * it occupies, as it always did, and now the moment it takes to refuse.
 */
#define LG_LORA_NETWORK_ID   18
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
_Static_assert(100u + LG_MAX_DEVICES <= 255u, "a part names who it is for in one byte");

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
    uint8_t        dest;        /* LORA_DEST_ALL, or the one radio it is for */
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

/* A part is built for everyone; this names the one radio it is for instead. `part` is what
 * lora_part_build or lora_parity_build wrote, `dest` a place in the address plan. */
static inline void lora_part_set_dest(uint8_t *part, uint16_t dest)
{
    part[5] = (uint8_t)dest;
}

/* Whether a part with this destination byte is for the radio at `address`. */
static inline bool lora_part_is_for(uint8_t dest, uint16_t address)
{
    return dest == LORA_DEST_ALL || dest == (uint8_t)address;
}

/* How many parts a payload of this length needs, or 0 if it is beyond the format's ceiling. */
size_t lora_part_count(size_t payload_len);

/*
 * The parity part (2026-10-09). A message-rank frame of two parts or more that nobody will
 * acknowledge is followed by one extra part carrying the XOR of every data slice zero-padded to
 * LORA_SLICE_MAX, so a receiver that lost exactly one part rebuilds it on the spot. A frame that
 * is acknowledged takes none (lora_txq_want_ack): it is sent again until it is answered, and a
 * parity sent every time cost more air than a resend sent when one is needed. Its header is an
 * ordinary part header with part == of. Its slice starts with one byte naming the final data
 * part's true length, because a rebuilt LAST part has no other way to know where the payload
 * ends, and the XOR of padded slices follows. At 92 raw bytes it is the same 124 characters as a
 * full data part.
 */
#define LORA_PARITY_SLICE   (LORA_SLICE_MAX + 1u)                 /* last_len byte + the XOR block */
#define LORA_PART_RAW_WIRE  (LORA_PART_HDR + LORA_PARITY_SLICE)   /* 92: the largest part on the air */
#define LORA_PART_B64_WIRE  (((LORA_PART_RAW_WIRE + 2u) / 3u) * 4u)   /* 124 characters */

_Static_assert(LORA_PART_B64_WIRE <= 240u, "the parity part, the largest there is, fits one AT+SEND");

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

/*
 * Times a frame that wants an acknowledgement goes on the air before it is given up. A message
 * given up between two APs is a message lost: nothing behind it sends it again, so more tries
 * look like the obvious cure. Five were tried on 2026-10-09 and taken out the same evening: on
 * the bench, with the Wi-Fi backbone cut, frames given up went from 11 to 9 in a run, and
 * messages the four-slot queue had no room for went from 1 to 16 - a frame waiting on its fourth
 * and fifth try was holding the slot the next message needed. More tries need a queue that lets
 * a frame past its third try give way to a newcomer, or more slots; until then, three.
 * A frame past LORA_TRIES_CLOSE tries, should the limit ever be raised above it, waits a random
 * few seconds more before each further one.
 */
#define LORA_TX_TRIES    3u
#define LORA_TRIES_CLOSE 3u
/*
 * An alert is not given up after three. On 2026-10-09 an all clear sent nine seconds after its SOS
 * went out three times into the SOS's own echo - every AP relaying and answering at once - was
 * heard by nobody, and was dropped: four handhelds went on sounding an alarm that had been stood
 * down, until someone sent it again by hand. An alert keeps trying, more slowly after the third,
 * for about two minutes; the driver sets the pace.
 */
#define LORA_ALERT_TRIES 12u
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
    uint32_t tag;          /* the driver's own mark; 0 when it has none. An AP marks an alert with its author */
    uint8_t  yield_to;     /* the driver's too: 1 + the AP whose transmission makes this frame needless, or 0 */
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
 * rounded up, for the radio setting above (LG_LORA_SF, LG_LORA_BW_CODE, LG_LORA_CR_CODE,
 * LG_LORA_PREAMBLE), explicit header, CRC on. Semtech's formula (SX1276 datasheet 4.1.1.7), in
 * integers.
 */
uint32_t lora_airtime_ms(size_t payload_chars);

/*
 * The same, as a constant expression, in microseconds: so that what a radio setting does to the
 * longest part can stop a build, not surprise a field trip.
 */
#define LG_LORA_SYM_US        ((uint32_t)(((uint64_t)1000000u << LG_LORA_SF) / LG_LORA_BW_HZ))
#define LG_LORA_AIRTIME_US(n) \
    (((4u * LG_LORA_PREAMBLE + 17u) * LG_LORA_SYM_US) / 4u + \
     (8u + ((8u * (n) + 44u - 1u) / (4u * LG_LORA_SF)) * (LG_LORA_CR_CODE + 4u)) * LG_LORA_SYM_US)
/*
 * How long one packet may be on the air. Measured 2026-10-09 on network ID 18, three APs side by
 * side, each packet sent after three seconds of silence, at SF9 and 125 kHz: 0.69 s (120
 * characters) 28 of 28, 0.80 s (140) 31 of 32, 0.88 s (160) 55 of 56, 1.07 s (200) 1 of 32,
 * 1.26 s (240) 1 of 56 - the rest arriving broken - and the same at 22 dBm as at 5. Back to back,
 * with no silence between them, the long ones arrive, which is how they passed the first test and
 * were shipped for two hours. 0.8 s leaves the measured edge alone.
 */
#define LG_LORA_AIR_CEILING_MS 800u
_Static_assert(LG_LORA_AIRTIME_US(LORA_PART_B64_WIRE) <= LG_LORA_AIR_CEILING_MS * 1000u,
               "the longest part is too long on the air at this radio setting: a smaller LORA_SLICE_MAX, "
               "or a faster setting");

/* ---- pacing, shared by both drivers ---------------------------------------------------------
 *
 * Each driver kept its own copy of these until 2026-10-09, and both copies were built on "a
 * receiver needs a second of quiet after a packet". It does not: that was the network-ID loss
 * (LG_LORA_NETWORK_ID). Measured on ID 18 with probes the firmware timed, SF9 at 125 kHz: the same
 * sender back to back with no quiet at all, 120 and 240 characters, heard 23-24 of 24; an answer
 * sent 20 ms after a packet, heard by a third radio 24 of 24 and by the radio that had just
 * finished transmitting 23 of 24. So the radio sets no pace of its own. What is left is the pace
 * the boards and the people need: a moment for whoever heard a packet to take its line from the
 * module and say so before the next one lands, and room for an answer before the next frame.
 */
#define LG_LORA_BACKOFF_MAX_MS    300u   /* the random wait before every transmission: no carrier sense */
#define LG_LORA_PART_GAP_MS       150u   /* a sender's pause after each of its own transmissions */
/* What a listener allows for each part still to come: the pause and the longest random start. */
#define LG_LORA_AIR_PART_GAP_MS   (LG_LORA_PART_GAP_MS + LG_LORA_BACKOFF_MAX_MS)
#define LG_LORA_ACK_CHARS         60u    /* an acknowledgement on the air: one sealed part of about 52 characters */
/* One acknowledgement, start to finish: its time on the air, and the module and the boards. */
#define LG_LORA_ACK_TURN_MS       (lora_airtime_ms(LG_LORA_ACK_CHARS) + 120u)
/* Nobody starts a frame on the heels of a packet it heard: that moment is the answer's. Housekeeping
 * keeps well clear; it is never worth the next thing a person sends. */
#define LG_LORA_ANSWER_QUIET_MS   LG_LORA_ACK_TURN_MS
#define LG_LORA_ROUTINE_QUIET_MS  1100u
/*
 * An alert waits for a frame in flight and its answers like a message does - but only this long,
 * and then it goes whatever the air is doing. Sent at once into a frame whose parts follow each
 * other 150 ms apart, it lands on a sender that is transmitting and cannot hear, and on receivers
 * that lose both: on the bench an SOS sent during a voice note reached the note's sender on its
 * fourth try, 13 s late (2026-10-09). A frame is over in two or three seconds, and the moment
 * after it is the one moment its sender is certain to be listening.
 */
#define LG_LORA_ALERT_PATIENCE_MS 4000u

/* ---- bring-up ---- */

/* What a witness prints beside the start of a probe it heard (`lora off` on a bench board), so
 * the measuring tool knows all of it arrived without the whole of it crossing the console. */
static inline unsigned lora_probe_sum(const char *text, size_t len)
{
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum = (sum * 31u + (unsigned char)text[i]) & 0xFFFFu;
    }
    return sum;
}
#define LORA_PROBE_TEXT_SHOWN 24   /* of a probe's characters a witness prints */

#ifdef __cplusplus
}
#endif
