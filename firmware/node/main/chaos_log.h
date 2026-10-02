/*
 * chaos_log.h - the chaos journal: what an AP did to itself, kept in flash (D83).
 *
 * Why it exists. A chaos run over USB was watched through the serial logs, and that is where every
 * bug the ten-hour night run found actually showed up (docs/chaos-engineering.md: "Coverage is
 * bounded by what the firmware logs"). Once the APs run on battery there is no serial port, and the
 * BLE status beacon is 27 packed bytes - enough to see that the grid recovered, nowhere near enough
 * to see *why* it did not. Without a journal a wireless run is blind in a way the USB one never was.
 *
 * So each AP writes what it did into its own flash, and the laptop reads it back afterwards through
 * the admin page. **The read side is in every AP build, not only chaos builds**, because a run ends
 * by putting an ordinary image back over the air (D81) - and that is exactly when you want to read
 * the journal. Only a chaos build ever writes.
 *
 * Devices hold bytes, browsers make text (D49): records are packed binary, 16 bytes each, and the
 * page that asks for them formats them. Nothing here keeps a text buffer.
 *
 * Bounded, and never wraps. The partition holds CHAOS_LOG_MAX records; a run bounded to
 * CHAOS_MAX_FAULTS faults writes a few hundred at most, so filling it means something is wrong and
 * the journal says so by refusing further records rather than by eating the oldest evidence.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CHAOS_LOG_PARTITION  "chaoslog"
#define CHAOS_LOG_MAGIC      0x43484A31u   /* "CHJ1" */
#define CHAOS_LOG_REC_LEN    16u
#define CHAOS_LOG_MAX        4095u         /* the first record's worth of space holds the header */

/*
 * How often a LoRa snapshot is written while armed, beyond the pair around every fault. Three records
 * each, so a ten-hour armed night costs 3 * 10 * 3600 / 300 = 360 records of the 4095 available, and the
 * 60-fault budget adds at most another 360. Comfortable, and deliberately so: a journal that fills is a
 * journal that stops recording the thing you armed it for.
 */
#define CHAOS_LORA_EVERY_S   300u

/* What happened. Kept small and stable: the laptop decodes these numbers. */
typedef enum {
    CHAOS_EV_BOOT          = 0,   /* an AP started; detail is esp_reset_reason() */
    CHAOS_EV_ARMED         = 1,   /* chaos armed, detail is the dead-man window in minutes */
    CHAOS_EV_RENEWED       = 2,   /* the window was extended */
    CHAOS_EV_DISARMED      = 3,   /* stopped on request */
    CHAOS_EV_EXPIRED       = 4,   /* nobody renewed in time, so chaos stopped by itself */
    CHAOS_EV_BACKBONE_OFF  = 5,   /* detail is seconds */
    CHAOS_EV_LORA_OFF      = 6,   /* detail is seconds */
    CHAOS_EV_RESTART       = 7,   /* about to restart on purpose */
    CHAOS_EV_BUDGET_SPENT  = 8,   /* the fault budget ran out; no more faults ever */
    CHAOS_EV_FULL          = 9,   /* the journal filled: written once, as the last record */
    /*
     * What the LoRa radio has carried, written as three records so the 16-byte layout does not change.
     * The journal used to say which fault happened and never what the second radio did, so a night on
     * battery could show that the grid survived an ESP-NOW outage and could not show that **LoRa carried
     * it** - the difference between resilience as a fact and as a statement.
     *
     * A snapshot is written just before each fault and again just after the outage it caused should have
     * ended, so the delta across the window is the evidence; and once every CHAOS_LORA_EVERY_S while
     * armed, so a quiet stretch still has a baseline to difference against.
     *
     * FIRST is the one to read. It is the firmware's own "N of them arrived here first": frames that
     * came over LoRa before Wi-Fi had delivered them. Out and in give it context - a large out with a
     * flat first means the radio was busy carrying what Wi-Fi had already managed.
     */
    CHAOS_EV_LORA_FIRST    = 10,  /* detail: frames that LoRa delivered before Wi-Fi did (saturates) */
    CHAOS_EV_LORA_OUT      = 11,  /* detail: frames this AP has put on the air (saturates) */
    CHAOS_EV_LORA_IN       = 12,  /* detail: frames it has taken off the air (saturates) */
} chaos_ev_t;

/*
 * One record, little-endian, exactly CHAOS_LOG_REC_LEN bytes:
 *
 *   0  u32  grid_time   Unix seconds, or 0 when the grid had no time
 *   4  u32  uptime_s    since this boot
 *   8  u16  boot        this AP's boot counter, low 16 bits
 *   10 u8   ap          AP index
 *   11 u8   kind        chaos_ev_t
 *   12 u16  detail      seconds, minutes or a reset reason, per kind
 *   14 u16  heap_kb     free internal heap, KB (saturates)
 */
typedef struct {
    uint32_t grid_time;
    uint32_t uptime_s;
    uint16_t boot;
    uint8_t  ap;
    uint8_t  kind;
    uint16_t detail;
    uint16_t heap_kb;
} chaos_rec_t;

/*
 * Prepares the journal for a fresh run: erases the partition and writes the header. Only a chaos
 * build calls this, and only when a run is armed, so an ordinary AP never touches the flash.
 * Returns false when there is no journal partition (an AP on the older layout).
 */
bool chaos_log_begin(uint16_t ap_index);

/* Appends one record. Silently does nothing without a journal, or once it is full. */
void chaos_log_add(uint8_t kind, uint16_t detail);

/* How many records the journal holds, and whether it filled up. Any build. */
uint32_t chaos_log_count(void);
bool     chaos_log_full(void);

/*
 * Reads `max` records from `from`, packed as on flash, into out (max * CHAOS_LOG_REC_LEN bytes).
 * Returns how many records were copied. Available in every build: this is how a run is read back
 * after an ordinary image has been put back over the air.
 */
size_t chaos_log_read(uint32_t from, size_t max, uint8_t *out, size_t cap);
