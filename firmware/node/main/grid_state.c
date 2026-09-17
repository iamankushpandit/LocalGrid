#include "grid_state.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "backbone.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lg_node.h"
#include "node_app.h"

static const char *TAG = "GRID";

/*
 * GRID_STATE body, little-endian, exactly GS_LEN bytes:
 *     0  u8    layout (GS_LAYOUT)
 *     1  u32   settings seq
 *     5  u16   settings author
 *     7  u8    configured (0 or 1)
 *     8  33 B  grid name, NUL-terminated
 *    41  48 B  time zone, NUL-terminated
 *    89  16 B  PBKDF2 salt
 *   105  u32   PBKDF2 iterations
 *   109  32 B  PBKDF2 hash
 *   141  u32   time generation
 *   145  u16   time author
 * The whole frame travels sealed under the backbone key, like every backbone frame.
 */
#define GS_LAYOUT          1u
#define GS_LEN             147u
#define GS_ITERATIONS_MIN  1000u
#define GS_ITERATIONS_MAX  200000u

_Static_assert(GS_LEN <= LG_GRID_STATE_MAX, "grid state body exceeds the core's limit");
_Static_assert(SETTINGS_GRID_NAME_MAX + 1 == 33 && SETTINGS_TZ_MAX + 1 == 48, "grid state layout needs updating");

static struct {
    SemaphoreHandle_t lock;
    uint16_t          self;
    node_settings_t   settings;
    uint32_t          time_gen;       /* RAM only, like grid time itself (D6) */
    uint16_t          time_author;
    bool              heard_other;    /* a grid state from another AP arrived since boot */
} s;

static bool newer(uint32_t seq_a, uint16_t author_a, uint32_t seq_b, uint16_t author_b)
{
    return seq_a > seq_b || (seq_a == seq_b && author_a > author_b);
}

void grid_state_init(uint16_t self)
{
    s.lock = xSemaphoreCreateMutex();
    s.self = self;
    esp_err_t err = settings_load(&s.settings);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[GRID] Settings unreadable (%s); waiting for another AP or setup", esp_err_to_name(err));
        memset(&s.settings, 0, sizeof(s.settings));
    }
    ESP_LOGI(TAG, "[GRID] Settings version %" PRIu32 " from AP %u (%s)", s.settings.seq, s.settings.author,
             s.settings.configured ? "set up" : "not set up");
}

void grid_state_settings(node_settings_t *out)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    *out = s.settings;
    xSemaphoreGive(s.lock);
}

esp_err_t grid_state_commit(const node_settings_t *in)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
    node_settings_t next = *in;
    next.seq = s.settings.seq + 1u;
    next.author = s.self;
    esp_err_t err = settings_save(&next);
    if (err == ESP_OK) {
        s.settings = next;
    }
    xSemaphoreGive(s.lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "[GRID] Settings version %" PRIu32 " made on this AP", next.seq);
        node_cmd_t cmd = { .type = NODE_CMD_GRID_ANNOUNCE };
        (void)xQueueSend(g_app.cmd_queue, &cmd, pdMS_TO_TICKS(500));
    }
    return err;
}

bool grid_state_setup_allowed(void)
{
    return s.heard_other || lgbb_link_count() == 0;
}

void grid_state_time_set_here(void)
{
    s.time_gen++;
    s.time_author = s.self;
}

static size_t encode(uint8_t out[GS_LEN])
{
    memset(out, 0, GS_LEN);
    xSemaphoreTake(s.lock, portMAX_DELAY);
    const node_settings_t *c = &s.settings;
    out[0] = GS_LAYOUT;
    lg_wr32(out + 1, c->seq);
    lg_wr16(out + 5, c->author);
    out[7] = c->configured ? 1u : 0u;
    memcpy(out + 8, c->grid_name, SETTINGS_GRID_NAME_MAX + 1);
    memcpy(out + 41, c->timezone, SETTINGS_TZ_MAX + 1);
    memcpy(out + 89, c->salt, SETTINGS_SALT_LEN);
    lg_wr32(out + 105, c->iterations);
    memcpy(out + 109, c->hash, SETTINGS_HASH_LEN);
    xSemaphoreGive(s.lock);
    lg_wr32(out + 141, s.time_gen);
    lg_wr16(out + 145, s.time_author);
    return GS_LEN;
}

void grid_state_announce(void)
{
    uint8_t body[GS_LEN];
    size_t len = encode(body);
    (void)lg_node_announce_grid_state(&g_app.core, body, len);
}

void grid_state_on_frame(uint16_t origin_node, const uint8_t *body, size_t len)
{
    if (len != GS_LEN || body[0] != GS_LAYOUT) {
        ESP_LOGW(TAG, "[GRID] Grid state from AP %u ignored: length %u or layout %u not understood", origin_node,
                 (unsigned)len, len > 0 ? body[0] : 0u);
        return;
    }
    s.heard_other = true;

    node_settings_t in;
    memset(&in, 0, sizeof(in));
    in.seq = lg_rd32(body + 1);
    in.author = lg_rd16(body + 5);
    in.configured = body[7] != 0;
    memcpy(in.grid_name, body + 8, SETTINGS_GRID_NAME_MAX + 1);
    memcpy(in.timezone, body + 41, SETTINGS_TZ_MAX + 1);
    memcpy(in.salt, body + 89, SETTINGS_SALT_LEN);
    in.iterations = lg_rd32(body + 105);
    memcpy(in.hash, body + 109, SETTINGS_HASH_LEN);
    uint32_t time_gen = lg_rd32(body + 141);
    uint16_t time_author = lg_rd16(body + 145);

    bool well_formed = body[7] <= 1u && in.grid_name[SETTINGS_GRID_NAME_MAX] == '\0' &&
                       in.timezone[SETTINGS_TZ_MAX] == '\0' &&
                       (!in.configured || (in.iterations >= GS_ITERATIONS_MIN && in.iterations <= GS_ITERATIONS_MAX));

    xSemaphoreTake(s.lock, portMAX_DELAY);
    bool take = well_formed && newer(in.seq, in.author, s.settings.seq, s.settings.author);
    esp_err_t err = ESP_OK;
    if (take) {
        err = settings_save(&in);
        s.settings = in;   /* held in RAM even if flash refused it, so this AP still agrees with the grid */
    }
    xSemaphoreGive(s.lock);
    if (!well_formed) {
        ESP_LOGW(TAG, "[GRID] Settings from AP %u ignored: malformed", origin_node);
    } else if (take) {
        ESP_LOGI(TAG, "[GRID] Adopted settings version %" PRIu32 " (made on AP %u) via AP %u%s", in.seq, in.author,
                 origin_node, err == ESP_OK ? "" : "; not saved to flash");
    }

    if (newer(time_gen, time_author, s.time_gen, s.time_author)) {
        s.time_gen = time_gen;
        s.time_author = time_author;
        if (g_app.time_quality == LG_TIME_AUTHORITATIVE && time_author != s.self) {
            /* Somebody set time more recently elsewhere: stop defending ours and take theirs. */
            g_app.time_quality = LG_TIME_CARRIED;
            ESP_LOGI("TIME", "[TIME] Time was set more recently on AP %u; following it", time_author);
        }
    }
}

void grid_state_print(void)
{
    node_settings_t c;
    grid_state_settings(&c);
    printf("Settings version %" PRIu32 " (made on AP %u), %s; time generation %" PRIu32 " (set on AP %u)\n", c.seq,
           c.author, c.configured ? "set up" : "not set up", s.time_gen, s.time_author);
}
