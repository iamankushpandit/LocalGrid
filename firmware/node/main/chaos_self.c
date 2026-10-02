/* chaos_self.c - an AP that attacks itself, for chaos testing with no wires (D82). See chaos_self.h. */
#include "chaos_self.h"
#include "chaos_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "backbone.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lora.h"
#include "node_app.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "CHAOS";

#define NVS_NAMESPACE  "lgchaos"
#define NVS_KEY_COUNT  "faults"
#define NVS_KEY_UNTIL  "until"    /* the dead-man deadline, grid-time seconds */

/*
 * The seed. Compiled in rather than random, so a run is reproducible: the laptop can print the same
 * schedule from the same three numbers and check what the grid actually did against it. Change it to
 * get a different night.
 */
#ifndef CHAOS_SEED
#define CHAOS_SEED 0x4C47u   /* 'LG' */
#endif

typedef enum {
    FAULT_BACKBONE_OFF = 0,
    FAULT_LORA_OFF     = 1,
    FAULT_RESTART      = 2,
    FAULT_KINDS        = 3,
} fault_kind_t;

static struct {
    bool     armed;          /* this build can inject, and the budget is not spent */
    bool     stopped;        /* the budget ran out, or the console said stop */
    uint16_t index;
    uint32_t boot;
    uint32_t state;          /* the schedule's own generator, seeded once */
    uint32_t count;          /* faults injected, mirrored from NVS */
    uint32_t next_ms;        /* when the next fault is due */
    uint32_t until;          /* D83: grid time the window closes; 0 closed */
    uint32_t lora_after_ms;  /* when to write the radio's counters again, after an outage; 0 none */
    uint32_t lora_logged_ms; /* when they were last written, for the periodic snapshot */
} C;

/*
 * A small deterministic generator. Not for anything that needs to be unguessable: a chaos schedule
 * has to be *reproducible*, which is the opposite requirement, and using the hardware RNG here would
 * make a night impossible to replay.
 */
static uint32_t next_rand(void)
{
    C.state = C.state * 1664525u + 1013904223u;
    return C.state >> 8;
}

static uint32_t rand_between(uint32_t lo, uint32_t hi)
{
    return hi <= lo ? lo : lo + next_rand() % (hi - lo + 1u);
}

static uint32_t load_count(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    uint32_t n = 0;
    (void)nvs_get_u32(h, NVS_KEY_COUNT, &n);
    nvs_close(h);
    return n;
}

/*
 * The budget lives in NVS, not in RAM, and this is the whole reason: one of the faults is a restart,
 * and a counter in RAM would be wiped by it, so an AP that restarts itself would restart its own
 * chaos run and never stop. Written before the fault is injected, for the same reason.
 */
static void save_count(uint32_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "[CHAOS] Could not record the fault count; stopping rather than risk a loop");
        C.stopped = true;
        return;
    }
    if (nvs_set_u32(h, NVS_KEY_COUNT, n) == ESP_OK) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

static void schedule_next(uint32_t now_ms);

static uint32_t load_u32(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return 0;
    }
    uint32_t v = 0;
    (void)nvs_get_u32(h, key, &v);
    nvs_close(h);
    return v;
}

static void save_u32(const char *key, uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (nvs_set_u32(h, key, v) == ESP_OK) {
        (void)nvs_commit(h);
    }
    nvs_close(h);
}

bool chaos_self_arm(uint32_t minutes)
{
    if (!chaos_self_available()) {
        return false;
    }
    uint32_t now = app_grid_time();
    if (now == 0) {
        /* No clock, no enforceable deadline: an AP armed like this could never promise to stop. */
        ESP_LOGW(TAG, "[CHAOS] Refusing to arm: the grid has no time, so the window cannot be enforced");
        return false;
    }
    uint32_t mins = minutes == 0 ? 1u : minutes > CHAOS_WINDOW_MAX_MIN ? CHAOS_WINDOW_MAX_MIN : minutes;
    C.until = now + mins * 60u;
    save_u32(NVS_KEY_UNTIL, C.until);
    bool first = C.count == 0;
    if (first) {
        (void)chaos_log_begin(C.index);
    }
    chaos_log_add(first ? CHAOS_EV_ARMED : CHAOS_EV_RENEWED, (uint16_t)mins);
    if (!C.stopped && C.count < CHAOS_MAX_FAULTS) {
        C.armed = true;
        schedule_next(app_now_ms());
    }
    ESP_LOGW(TAG, "[CHAOS] Window open for %" PRIu32 " min (until grid time %" PRIu32 "); "
                  "it closes by itself if nobody renews it", mins, C.until);
    return true;
}

void chaos_self_disarm(void)
{
    C.until = 0;
    save_u32(NVS_KEY_UNTIL, 0);
    C.armed = false;
    chaos_log_add(CHAOS_EV_DISARMED, 0);
    ESP_LOGW(TAG, "[CHAOS] Window closed; no more faults until it is armed again");
}

bool chaos_self_armed_now(void)
{
    uint32_t now = app_grid_time();
    return C.until != 0 && now != 0 && now < C.until;
}

uint32_t chaos_self_window_left_s(void)
{
    uint32_t now = app_grid_time();
    return chaos_self_armed_now() ? C.until - now : 0u;
}

static void schedule_next(uint32_t now_ms)
{
    C.next_ms = now_ms + rand_between(CHAOS_GAP_MIN_S, CHAOS_GAP_MAX_S) * 1000u;
}

/* u32 counter into the journal's u16 detail, saying so rather than wrapping silently. */
static uint16_t sat16(uint32_t v)
{
    return v > 0xFFFFu ? 0xFFFFu : (uint16_t)v;
}

/*
 * Three records saying what the radio has carried. Written around every fault and periodically while
 * armed; the reader differences consecutive snapshots. Costs no airtime and nothing but flash.
 */
static void log_lora(uint32_t now_ms)
{
    C.lora_logged_ms = now_ms != 0 ? now_ms : 1u;
    if (!lora_fitted()) {
        return;   /* an AP with no module has nothing to say, and a zero would read as "carried nothing" */
    }
    lora_traffic_t tr;
    lora_traffic(&tr);
    chaos_log_add(CHAOS_EV_LORA_FIRST, sat16(tr.frames_first));
    chaos_log_add(CHAOS_EV_LORA_OUT, sat16(tr.frames_out));
    chaos_log_add(CHAOS_EV_LORA_IN, sat16(tr.frames_in));
}

bool chaos_self_available(void)
{
#if NODE_CHAOS_ENABLED
    return true;
#else
    return false;
#endif
}

uint32_t chaos_self_count(void)
{
    return C.count;
}

void chaos_self_start(uint16_t ap_index, uint32_t boot)
{
    if (!chaos_self_available()) {
        return;   /* an ordinary build: this module is inert and costs a few bytes */
    }
    memset(&C, 0, sizeof(C));
    C.index = ap_index;
    C.boot = boot;
    /*
     * Seeded from the build's seed, this AP's index and this boot, so the three APs never take
     * themselves out at the same moment and the schedule can be replayed from the report.
     */
    C.lora_after_ms = 0;
    C.lora_logged_ms = 0;
    C.state = (uint32_t)CHAOS_SEED * 2654435761u + (uint32_t)ap_index * 40503u + boot;
    C.count = load_count();
    C.until = load_u32(NVS_KEY_UNTIL);   /* D83: the window survives the restart a fault causes */
    if (C.count >= CHAOS_MAX_FAULTS) {
        C.stopped = true;
        ESP_LOGW(TAG, "[CHAOS] Budget already spent (%" PRIu32 " faults); this AP will not attack itself "
                      "again until `chaos clear`", C.count);
        return;
    }
    /*
     * D83: armed only while the window is open. A chaos build that nobody has armed does nothing at
     * all, which is what makes it safe to leave on an AP between runs.
     */
    C.armed = chaos_self_armed_now();
    schedule_next(app_now_ms());
    /*
     * D83: the journal. Begun only on the first boot of a run (count 0), never on the restart that a
     * fault causes -- erasing it there would throw away the evidence of everything before the restart,
     * which is exactly what a reader needs most.
     */
    if (C.count == 0) {
        (void)chaos_log_begin(ap_index);
    }
    chaos_log_add(CHAOS_EV_BOOT, (uint16_t)esp_reset_reason());
    ESP_LOGW(TAG, "[CHAOS] SELF-CHAOS BUILD: this access point will break itself on purpose. "
                  "%" PRIu32 " of %u faults used, seed 0x%04X, AP %u, boot %" PRIu32,
             C.count, (unsigned)CHAOS_MAX_FAULTS, (unsigned)CHAOS_SEED, (unsigned)ap_index, boot);
}

void chaos_self_poll(uint32_t now_ms)
{
    if (C.stopped) {
        return;
    }
    /*
     * D83: has the window closed? Checked before anything else, so an expiry stops the run even if a
     * fault was already due. Nobody renewing is the normal way a run ends.
     */
    if (C.until != 0 && !chaos_self_armed_now()) {
        C.until = 0;
        save_u32(NVS_KEY_UNTIL, 0);
        C.armed = false;
        chaos_log_add(CHAOS_EV_EXPIRED, 0);
        ESP_LOGW(TAG, "[CHAOS] Window expired with nobody renewing it; this AP stops attacking itself");
        return;
    }
    if (!C.armed) {
        return;
    }
    /* The other half of a fault's evidence: what the radio had carried once the outage was over. */
    if (C.lora_after_ms != 0 && (int32_t)(now_ms - C.lora_after_ms) >= 0) {
        C.lora_after_ms = 0;
        log_lora(now_ms);
    }
    if (C.lora_logged_ms == 0 || now_ms - C.lora_logged_ms >= CHAOS_LORA_EVERY_S * 1000u) {
        log_lora(now_ms);
    }
    if ((int32_t)(now_ms - C.next_ms) < 0) {
        return;
    }
    if (C.count >= CHAOS_MAX_FAULTS) {
        C.stopped = true;
        chaos_log_add(CHAOS_EV_BUDGET_SPENT, (uint16_t)C.count);
        ESP_LOGW(TAG, "[CHAOS] Budget spent after %" PRIu32 " faults; no more, and the grid is left alone",
                 C.count);
        return;
    }

    fault_kind_t kind = (fault_kind_t)(next_rand() % FAULT_KINDS);
    uint32_t secs = rand_between(CHAOS_OUTAGE_MIN_S, CHAOS_OUTAGE_MAX_S);

    /*
     * The radio's counters as they stand *before* the fault, and an appointment to write them again two
     * seconds after the outage should have lifted. The pair is what makes "LoRa carried it" a number: a
     * backbone-off window with a rising FIRST is LoRa doing the grid's work, and a flat one is the grid
     * having quietly stopped.
     */
    log_lora(now_ms);
    if (kind != FAULT_RESTART) {
        C.lora_after_ms = now_ms + secs * 1000u + 2000u;
    }

    /* Counted before it happens: a restart never returns from here, and a fault that was injected
     * but not counted would let the budget drift upwards forever. */
    C.count++;
    save_count(C.count);
    if (C.stopped) {
        return;   /* save_count could not write, and said so */
    }

    switch (kind) {
    case FAULT_BACKBONE_OFF:
        ESP_LOGW(TAG, "[CHAOS] Fault %" PRIu32 "/%u: ESP-NOW backbone off for %" PRIu32 " s",
                 C.count, (unsigned)CHAOS_MAX_FAULTS, secs);
        chaos_log_add(CHAOS_EV_BACKBONE_OFF, (uint16_t)secs);
        lgbb_disable(secs);
        break;
    case FAULT_LORA_OFF:
        ESP_LOGW(TAG, "[CHAOS] Fault %" PRIu32 "/%u: LoRa off for %" PRIu32 " s",
                 C.count, (unsigned)CHAOS_MAX_FAULTS, secs);
        chaos_log_add(CHAOS_EV_LORA_OFF, (uint16_t)secs);
        lora_disable(secs);
        break;
    case FAULT_RESTART:
    default:
        /*
         * The one fault that used to need the reset line. The log line goes out first and the delay
         * lets it reach the serial port and the BLE beacon before the AP disappears, so a watcher can
         * tell a deliberate restart from a crash or a brownout.
         */
        ESP_LOGW(TAG, "[CHAOS] Fault %" PRIu32 "/%u: restarting on purpose",
                 C.count, (unsigned)CHAOS_MAX_FAULTS);
        chaos_log_add(CHAOS_EV_RESTART, 0);   /* written before the restart, or it is never written */
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
        return;
    }
    schedule_next(now_ms);
}

void chaos_self_print(void)
{
    if (!chaos_self_available()) {
        printf("Chaos: this is an ordinary build; it never attacks itself (D82).\n");
        return;
    }
    printf("Chaos (D82): SELF-CHAOS BUILD, seed 0x%04X, AP %u, boot %" PRIu32 "\n",
           (unsigned)CHAOS_SEED, (unsigned)C.index, C.boot);
    printf("  faults:   %" PRIu32 " of %u used%s\n", C.count, (unsigned)CHAOS_MAX_FAULTS,
           C.stopped ? " (stopped)" : "");
    if (C.armed && !C.stopped) {
        uint32_t now = app_now_ms();
        printf("  next:     in %" PRIu32 " s\n",
               (int32_t)(C.next_ms - now) > 0 ? (C.next_ms - now) / 1000u : 0u);
    }
    if (chaos_self_armed_now()) {
        printf("  window:   open, %" PRIu32 " s left (it closes by itself if nobody renews it)\n",
               chaos_self_window_left_s());
    } else {
        printf("  window:   closed; this AP is not attacking itself\n");
    }
    printf("  journal:  %" PRIu32 " records%s\n", chaos_log_count(), chaos_log_full() ? " (FULL)" : "");
    printf("  faults it uses: backbone off, LoRa off, restart. It cannot cut its own power.\n");
}

void chaos_self_stop(void)
{
    C.stopped = true;
    save_count(CHAOS_MAX_FAULTS);   /* survives a restart: stop means stop */
    C.count = CHAOS_MAX_FAULTS;
    chaos_log_add(CHAOS_EV_DISARMED, 0);
    ESP_LOGW(TAG, "[CHAOS] Stopped on request; this AP will not attack itself again");
}

void chaos_self_clear(void)
{
    save_count(0);
    C.count = 0;
    C.stopped = false;
    C.armed = chaos_self_available();
    if (C.armed) {
        schedule_next(app_now_ms());
    }
    ESP_LOGW(TAG, "[CHAOS] Budget cleared; a fresh run starts now");
}
