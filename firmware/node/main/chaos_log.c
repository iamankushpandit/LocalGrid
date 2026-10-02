/* chaos_log.c - the chaos journal in flash (D83). See chaos_log.h. */
#include "chaos_log.h"

#include <inttypes.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "node_app.h"

static const char *TAG = "CHAOS";

/*
 * The header, in the first CHAOS_LOG_REC_LEN bytes so records stay aligned to their own size:
 *   0 u32 magic | 4 u32 count | 8 u8 ap | 9..15 reserved 0
 *
 * `count` is rewritten as records are added. Flash cannot be rewritten without erasing, so the
 * header's count is written once per record into a fresh slot rather than updated in place: instead
 * of that complexity, the count is recovered on read by scanning for the first unwritten record,
 * which is what erased flash (all 0xFF) makes easy and reliable across a restart.
 */
#define HDR_LEN  CHAOS_LOG_REC_LEN

static struct {
    const esp_partition_t *part;
    bool     ready;
    uint32_t count;     /* records written */
    bool     full;
    uint16_t ap;
} L;

static const esp_partition_t *journal(void)
{
    if (L.part == NULL) {
        L.part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)0x40,
                                          CHAOS_LOG_PARTITION);
    }
    return L.part;
}

static uint16_t heap_kb(void)
{
    size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    size_t kb = free_b / 1024u;
    return kb > 0xFFFFu ? 0xFFFFu : (uint16_t)kb;
}

/*
 * How many records are already there: the first slot that is still erased. Recovered by scanning
 * rather than trusted from a counter, because a restart is one of the faults and a count held in RAM
 * would be lost by it - and flash cannot be rewritten in place to keep one on disk.
 */
static uint32_t scan_count(const esp_partition_t *p)
{
    uint8_t rec[CHAOS_LOG_REC_LEN];
    for (uint32_t i = 0; i < CHAOS_LOG_MAX; i++) {
        if (esp_partition_read(p, HDR_LEN + i * CHAOS_LOG_REC_LEN, rec, sizeof(rec)) != ESP_OK) {
            return i;
        }
        bool erased = true;
        for (size_t b = 0; b < sizeof(rec); b++) {
            if (rec[b] != 0xFFu) {
                erased = false;
                break;
            }
        }
        if (erased) {
            return i;
        }
    }
    return CHAOS_LOG_MAX;
}

static bool open_existing(void)
{
    const esp_partition_t *p = journal();
    if (p == NULL) {
        return false;
    }
    uint8_t hdr[HDR_LEN];
    if (esp_partition_read(p, 0, hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    uint32_t magic = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) |
                     ((uint32_t)hdr[3] << 24);
    if (magic != CHAOS_LOG_MAGIC) {
        return false;   /* never begun, or erased: no journal to read */
    }
    L.ap = hdr[8];
    L.count = scan_count(p);
    L.full = L.count >= CHAOS_LOG_MAX;
    L.ready = true;
    return true;
}

bool chaos_log_begin(uint16_t ap_index)
{
    const esp_partition_t *p = journal();
    if (p == NULL) {
        ESP_LOGW(TAG, "[CHAOS] No journal partition; this AP is on the older layout and keeps no record");
        return false;
    }
    if (esp_partition_erase_range(p, 0, p->size) != ESP_OK) {
        ESP_LOGE(TAG, "[CHAOS] The journal could not be erased; the run would keep no record");
        return false;
    }
    uint8_t hdr[HDR_LEN] = { 0 };
    hdr[0] = (uint8_t)(CHAOS_LOG_MAGIC & 0xFFu);
    hdr[1] = (uint8_t)((CHAOS_LOG_MAGIC >> 8) & 0xFFu);
    hdr[2] = (uint8_t)((CHAOS_LOG_MAGIC >> 16) & 0xFFu);
    hdr[3] = (uint8_t)((CHAOS_LOG_MAGIC >> 24) & 0xFFu);
    hdr[8] = (uint8_t)ap_index;
    if (esp_partition_write(p, 0, hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGE(TAG, "[CHAOS] The journal header could not be written");
        return false;
    }
    L.ap = (uint8_t)ap_index;
    L.count = 0;
    L.full = false;
    L.ready = true;
    ESP_LOGW(TAG, "[CHAOS] Journal ready: room for %u records", (unsigned)CHAOS_LOG_MAX);
    return true;
}

void chaos_log_add(uint8_t kind, uint16_t detail)
{
    if (!L.ready && !open_existing()) {
        return;
    }
    if (L.full) {
        return;
    }
    const esp_partition_t *p = journal();
    if (p == NULL) {
        return;
    }
    if (L.count >= CHAOS_LOG_MAX) {
        L.full = true;
        return;
    }
    /*
     * The last slot records that the journal filled, rather than being spent on another fault: a
     * reader must be able to tell "the run ended" from "the evidence ran out".
     */
    bool last = (L.count + 1u) >= CHAOS_LOG_MAX;
    if (last) {
        kind = CHAOS_EV_FULL;
        detail = 0;
    }

    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    uint32_t gt = app_grid_time();
    uint16_t hk = heap_kb();
    uint8_t rec[CHAOS_LOG_REC_LEN];
    rec[0] = (uint8_t)(gt & 0xFFu);
    rec[1] = (uint8_t)((gt >> 8) & 0xFFu);
    rec[2] = (uint8_t)((gt >> 16) & 0xFFu);
    rec[3] = (uint8_t)((gt >> 24) & 0xFFu);
    rec[4] = (uint8_t)(up & 0xFFu);
    rec[5] = (uint8_t)((up >> 8) & 0xFFu);
    rec[6] = (uint8_t)((up >> 16) & 0xFFu);
    rec[7] = (uint8_t)((up >> 24) & 0xFFu);
    rec[8] = (uint8_t)(g_app.boot & 0xFFu);
    rec[9] = (uint8_t)((g_app.boot >> 8) & 0xFFu);
    rec[10] = L.ap;
    rec[11] = kind;
    rec[12] = (uint8_t)(detail & 0xFFu);
    rec[13] = (uint8_t)((detail >> 8) & 0xFFu);
    rec[14] = (uint8_t)(hk & 0xFFu);
    rec[15] = (uint8_t)((hk >> 8) & 0xFFu);

    if (esp_partition_write(p, HDR_LEN + L.count * CHAOS_LOG_REC_LEN, rec, sizeof(rec)) == ESP_OK) {
        L.count++;
        if (last) {
            L.full = true;
            ESP_LOGW(TAG, "[CHAOS] Journal full after %" PRIu32 " records; no more will be kept", L.count);
        }
    }
}

uint32_t chaos_log_count(void)
{
    if (!L.ready) {
        (void)open_existing();
    }
    return L.count;
}

bool chaos_log_full(void)
{
    if (!L.ready) {
        (void)open_existing();
    }
    return L.full;
}

size_t chaos_log_read(uint32_t from, size_t max, uint8_t *out, size_t cap)
{
    if (out == NULL || max == 0) {
        return 0;
    }
    if (!L.ready && !open_existing()) {
        return 0;
    }
    const esp_partition_t *p = journal();
    if (p == NULL || from >= L.count) {
        return 0;
    }
    size_t n = L.count - from;
    if (n > max) {
        n = max;
    }
    if (n * CHAOS_LOG_REC_LEN > cap) {
        n = cap / CHAOS_LOG_REC_LEN;
    }
    if (n == 0) {
        return 0;
    }
    if (esp_partition_read(p, HDR_LEN + from * CHAOS_LOG_REC_LEN, out, n * CHAOS_LOG_REC_LEN) != ESP_OK) {
        return 0;
    }
    return n;
}
