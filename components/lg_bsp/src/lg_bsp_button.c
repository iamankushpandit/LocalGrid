/*
 * Physical buttons (D66): polled GPIOs with a debounce of DEBOUNCE_SAMPLES equal readings, turned
 * into press and hold events. Everything is fixed-size: one small state per profile row.
 */
#include "lg_bsp_button.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"

static const char *TAG = "BSP";

#define DEBOUNCE_SAMPLES 2      /* two equal readings in a row, at the UI's 20 ms tick: 40 ms */
#define EVENTS_MAX       4      /* waiting events; more than this in one poll cannot happen with 4 buttons */

typedef struct {
    int8_t   gpio;
    bool     active_low;
    bool     down;              /* debounced state */
    bool     raw;               /* last reading */
    uint8_t  same;              /* readings equal to raw in a row */
    bool     held;              /* the hold event went out for this press */
    uint32_t since_ms;          /* when the debounced press began */
} button_t;

static struct {
    uint8_t        n;
    uint32_t       hold_ms;
    button_t       b[LG_BOARD_BUTTONS_MAX];
    lg_btn_event_t q[EVENTS_MAX];
    uint8_t        q_head;
    uint8_t        q_len;
} s;

static void push(uint8_t id, lg_btn_event_kind_t kind)
{
    if (s.q_len >= EVENTS_MAX) {
        return;   /* the oldest are still waiting; a burst this long is contact noise */
    }
    s.q[(s.q_head + s.q_len) % EVENTS_MAX] = (lg_btn_event_t){ .id = id, .kind = kind };
    s.q_len++;
}

esp_err_t lg_bsp_button_start(const lg_board_t *board, uint32_t hold_ms)
{
    memset(&s, 0, sizeof(s));
    s.hold_ms = hold_ms;
    for (uint8_t i = 0; i < LG_BOARD_BUTTONS_MAX; i++) {
        s.b[i].gpio = LG_PIN_NONE;   /* never GPIO0 by default */
    }
    if (board == NULL || board->n_buttons == 0) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    for (uint8_t i = 0; i < board->n_buttons && i < LG_BOARD_BUTTONS_MAX; i++) {
        const lg_button_profile_t *p = &board->buttons[i];
        button_t *b = &s.b[i];
        if (p->gpio < 0) {
            continue;
        }
        gpio_config_t cfg = {
            .pin_bit_mask = 1ULL << p->gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = p->pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
        };
        if (gpio_config(&cfg) != ESP_OK) {
            ESP_LOGW(TAG, "[UI] Button %u on GPIO%d could not be configured", i, p->gpio);
            continue;
        }
        b->gpio = p->gpio;
        b->active_low = p->active_low;
        s.n = (uint8_t)(i + 1u);   /* ids are profile indices, so a skipped row keeps its place */
        ESP_LOGI(TAG, "[UI] Button %u on GPIO%d (%s)", i, p->gpio, p->active_low ? "active low" : "active high");
    }
    return s.n > 0 ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

uint8_t lg_bsp_button_count(void)
{
    return s.n;
}

bool lg_bsp_button_poll(uint32_t now_ms, lg_btn_event_t *out)
{
    for (uint8_t i = 0; i < s.n; i++) {
        button_t *b = &s.b[i];
        if (b->gpio < 0) {
            continue;   /* a profile row that could not be configured */
        }
        bool level = gpio_get_level((gpio_num_t)b->gpio) != 0;
        bool pressed = b->active_low ? !level : level;
        if (pressed != b->raw) {
            b->raw = pressed;
            b->same = 1;
        } else if (b->same < DEBOUNCE_SAMPLES) {
            b->same++;
        }
        if (b->same >= DEBOUNCE_SAMPLES && pressed != b->down) {
            b->down = pressed;
            if (pressed) {
                b->since_ms = now_ms;
                b->held = false;
                push(i, LG_BTN_DOWN);
            } else {
                push(i, b->held ? LG_BTN_UP : LG_BTN_SHORT);
            }
        }
        if (b->down && !b->held && now_ms - b->since_ms >= s.hold_ms) {
            b->held = true;
            push(i, LG_BTN_HOLD);
        }
    }
    if (s.q_len == 0) {
        return false;
    }
    *out = s.q[s.q_head];
    s.q_head = (uint8_t)((s.q_head + 1u) % EVENTS_MAX);
    s.q_len--;
    return true;
}

uint32_t lg_bsp_button_held_ms(uint8_t id, uint32_t now_ms)
{
    if (id >= s.n || !s.b[id].down) {
        return 0;
    }
    return now_ms - s.b[id].since_ms;
}
