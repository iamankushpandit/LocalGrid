#include "hh_mem.h"

#include <inttypes.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "MEM";

void hh_mem_mark(const char *stage)
{
    static int64_t s_previous = -1;
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    size_t free_bytes = heap_caps_get_free_size(caps);
    int64_t delta = s_previous < 0 ? 0 : (int64_t)free_bytes - s_previous;
    s_previous = (int64_t)free_bytes;
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total == 0) {
        ESP_LOGI(TAG, "[MEM] %s: free %u KB (%+" PRId64 "), lowest %u KB, largest block %u KB", stage,
                 (unsigned)(free_bytes / 1024u), delta / 1024,
                 (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024u),
                 (unsigned)(heap_caps_get_largest_free_block(caps) / 1024u));
        return;
    }
    /* Internal RAM stays the headline: DMA buffers, stacks, and small blocks can only live there,
     * so it is what runs out. PSRAM follows it, separately. */
    ESP_LOGI(TAG, "[MEM] %s: free %u KB (%+" PRId64 "), lowest %u KB, largest block %u KB; PSRAM free %u of %u KB, "
             "lowest %u KB", stage, (unsigned)(free_bytes / 1024u), delta / 1024,
             (unsigned)(heap_caps_get_minimum_free_size(caps) / 1024u),
             (unsigned)(heap_caps_get_largest_free_block(caps) / 1024u),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024u), (unsigned)(psram_total / 1024u),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024u));
}
