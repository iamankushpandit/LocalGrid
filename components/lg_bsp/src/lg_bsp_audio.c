/*
 * The sounds a handheld makes.
 *
 * Synthesis, not samples: each cue is a short script of (frequency, milliseconds) segments.
 * A whole vocabulary costs a few dozen bytes of const data, where one second of 16-bit mono
 * would cost 32 KB, on boards with about a fifth of the app partition left.
 *
 * Cues are played by a small task, so a screen or the network service never waits on a
 * speaker: a cue is queued and forgotten, and a full queue drops it rather than blocking.
 *
 * LG_AUDIO_BACKEND is the seam. It is on where this build can actually drive a speaker, and
 * everything that exists only to make a noise sits behind it: on a chip with no backend the
 * file compiles down to the public calls, each reporting ESP_ERR_NOT_SUPPORTED, with no
 * unused code left over. Today the only backend is the classic ESP32's cosine wave generator,
 * which needs no sample buffer, no DMA and no timer of ours. The FNK0104B's ES8311 is next:
 * its wiring is recorded in the board profile, and it is the backend that is missing, not the
 * pins. Which board can do what is the profile's business, never the chip's (D9).
 */
#include "lg_bsp_audio.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lg_bsp_i2c.h"
#include "nvs.h"
#include "soc/soc_caps.h"

#if SOC_DAC_SUPPORTED
#define LG_AUDIO_BACKEND 1
#include "driver/dac_cosine.h"
#else
#define LG_AUDIO_BACKEND 0   /* the ES8311 backend will turn this on for the ESP32-S3 */
#endif

static const char *TAG = "BSP";

/* The same namespace the touch calibration uses; one key, one byte. */
#define NVS_NAMESPACE "lgui"
#define NVS_KEY_MUTE   "mute"   /* the older on/off flag, read once and carried forward */
#define NVS_KEY_VOLUME "vol"

/* A request is either a cue from the table or one bare tone from the console. */
typedef struct {
    uint8_t  cue;      /* LG_CUE_COUNT means "the tone below instead" */
    uint16_t hz;
    uint16_t ms;
    bool     always;   /* an emergency: played whatever the volume says */
} req_t;

#define QUEUE_LEN  4
#define TASK_STACK 2560
#define TASK_PRIO  2     /* below the drawing task, which runs at 3 */

static struct {
    const lg_board_t *board;
    QueueHandle_t     queue;
    bool              available;
    uint8_t           volume;   /* lg_volume_t; OFF is the mute */
#if LG_AUDIO_BACKEND
    int               channel;
#endif
} s;

#if LG_AUDIO_BACKEND

/*
 * One segment of a cue: a frequency, how long to hold it, and how loud. A frequency of 0 is a
 * rest, which is what separates two blips.
 *
 * Loudness is per segment, not per cue, because a bell is a strike that fades: one level for
 * a whole cue can only make a beep. The generator offers four (full, -6, -12, -18 dB), which
 * is coarse but enough to shape an attack and a decay.
 */
#define ATTEN_FULL  0   /* DAC_COSINE_ATTEN_DEFAULT */
#define ATTEN_QUIET 1   /* DAC_COSINE_ATTEN_DB_6:  half amplitude */
#define ATTEN_SOFT  2   /* DAC_COSINE_ATTEN_DB_12: a quarter */
#define ATTEN_FAINT 3   /* DAC_COSINE_ATTEN_DB_18: an eighth */

typedef struct {
    uint16_t hz;
    uint16_t ms;
    uint8_t  atten;
} seg_t;

/*
 * The vocabulary. Three rules, the same ones any console sound has to obey:
 * a cue heard hundreds of times a day is short and quiet; cues that mean opposite things
 * differ in DIRECTION rather than only in pitch, so a rising pair means "gone" while a falling
 * one means "arrived"; and urgent is the only one allowed to be loud and repeated.
 *
 * A message arriving rings a bell: a brief bright strike, the note it settles on, its fall,
 * and a fainter tail. Four segments and two dozen bytes, where a recorded chime would be tens
 * of kilobytes. It is not a real bell's inharmonic spectrum -- one sine at a time cannot be --
 * but the shape of a strike and a decay is what makes it read as a bell rather than a beep.
 */
static const seg_t CUE_SENT[] = {
    { 880, 55, ATTEN_QUIET }, { 1319, 70, ATTEN_SOFT },
};
/*
 * Louder than it was, and the decay is what paid for it. The strike and the note it rings on
 * are both at full amplitude now, the fall is one step down, and the near-inaudible eighth-
 * amplitude tail is gone. Full is the ceiling: this path has four attenuation steps and no
 * gain register, so there is nothing above this in software.
 */
static const seg_t CUE_RECEIVED[] = {
    { 2093, 45, ATTEN_FULL },    /* the strike */
    { 1568, 85, ATTEN_FULL },    /* the note it rings on */
    { 1047, 130, ATTEN_QUIET },  /* falling away */
};
/*
 * An announcement: three rising notes, unmistakably not the arrival bell, because it means
 * something for everybody rather than something for you.
 */
static const seg_t CUE_ANNOUNCE[] = {
    { 784, 110, ATTEN_FULL }, { 988, 110, ATTEN_FULL }, { 1319, 200, ATTEN_FULL },
};

/*
 * The emergency: two tones alternating, which is what a phone's alert does and what ears are
 * already trained by. Three repeats of one note read as a doorbell; this reads as a warning.
 * Always at full amplitude -- an urgent broadcast ignores the volume setting entirely (D40).
 */
static const seg_t CUE_URGENT[] = {
    { 853, 170, ATTEN_FULL }, { 960, 170, ATTEN_FULL },
    { 853, 170, ATTEN_FULL }, { 960, 170, ATTEN_FULL },
    { 853, 170, ATTEN_FULL }, { 960, 230, ATTEN_FULL },
};

typedef struct {
    const seg_t *segs;
    uint8_t      count;
} cue_t;

static const cue_t CUES[LG_CUE_COUNT] = {
    [LG_CUE_SENT]     = { CUE_SENT,     sizeof(CUE_SENT) / sizeof(seg_t)     },
    [LG_CUE_RECEIVED] = { CUE_RECEIVED, sizeof(CUE_RECEIVED) / sizeof(seg_t) },
    [LG_CUE_ANNOUNCE] = { CUE_ANNOUNCE, sizeof(CUE_ANNOUNCE) / sizeof(seg_t) },
    [LG_CUE_URGENT]   = { CUE_URGENT,   sizeof(CUE_URGENT) / sizeof(seg_t)   },
};

/* GPIO25 is DAC channel 0 and GPIO26 is channel 1; anything else is not a DAC pin. */
static int channel_of(int8_t pin)
{
    if (pin == 25) {
        return DAC_CHAN_0;
    }
    return pin == 26 ? DAC_CHAN_1 : -1;
}

/*
 * One segment. The generator is created, started, held, then torn down: only the first
 * channel may set the frequency, so a new frequency means a new channel rather than a
 * reconfigure, and force_set_freq lets each segment claim it.
 */
static void play_segment(uint16_t hz, uint16_t ms, uint8_t atten)
{
    if (hz == 0 || s.channel < 0) {
        vTaskDelay(pdMS_TO_TICKS(ms));   /* a rest */
        return;
    }
    dac_cosine_config_t cfg = {
        .chan_id = (dac_channel_t)s.channel,
        .freq_hz = hz,
        .clk_src = DAC_COSINE_CLK_SRC_DEFAULT,
        .atten   = (dac_cosine_atten_t)atten,
        .phase   = DAC_COSINE_PHASE_0,
        .offset  = 0,
        .flags   = { .force_set_freq = true },
    };
    dac_cosine_handle_t h = NULL;
    if (dac_cosine_new_channel(&cfg, &h) != ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }
    dac_cosine_start(h);
    vTaskDelay(pdMS_TO_TICKS(ms));
    dac_cosine_stop(h);
    dac_cosine_del_channel(h);
}

/*
 * The amplifier. Without this the speaker stays silent however right everything else is, and
 * the failure is quiet rather than noisy: on a board of this family the same line was declared
 * as an LED, so switching the LED off switched the speaker off with it. It goes on before a
 * cue and drops only once nothing else is waiting, after a short tail, because dropping it as
 * the last segment ends clips the end off every sound.
 */
#define AMP_TAIL_MS 60

static void amp(bool on)
{
    if (s.board == NULL || s.board->audio.amp_enable == LG_PIN_NONE) {
        return;
    }
    bool high = s.board->audio.amp_active_low ? !on : on;
    gpio_set_level((gpio_num_t)s.board->audio.amp_enable, high ? 1 : 0);
}

/*
 * The volume setting as an attenuation offset: HIGH leaves a segment as written, and each step
 * down adds 6 dB of attenuation, clamped at the quietest the part offers. An emergency is
 * played exactly as written, whatever the setting.
 */
static uint8_t atten_for(uint8_t seg_atten, bool always)
{
    if (always) {
        return seg_atten;
    }
    uint8_t level = s.volume > LG_VOLUME_HIGH ? LG_VOLUME_HIGH : s.volume;
    uint32_t a = (uint32_t)seg_atten + (uint32_t)(LG_VOLUME_HIGH - level);
    return (uint8_t)(a > ATTEN_FAINT ? ATTEN_FAINT : a);
}

static void audio_task(void *arg)
{
    (void)arg;
    req_t r;
    for (;;) {
        if (xQueueReceive(s.queue, &r, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        amp(true);
        if (r.cue < LG_CUE_COUNT) {
            const cue_t *c = &CUES[r.cue];
            for (uint8_t i = 0; i < c->count; i++) {
                play_segment(c->segs[i].hz, c->segs[i].ms, atten_for(c->segs[i].atten, r.always));
            }
        } else {
            play_segment(r.hz, r.ms, atten_for(ATTEN_QUIET, false));
        }
        if (uxQueueMessagesWaiting(s.queue) == 0) {
            vTaskDelay(pdMS_TO_TICKS(AMP_TAIL_MS));   /* let the sound finish before muting */
            amp(false);
        }
    }
}

/*
 * The queue and the task are claimed at the first sound, not at start.
 *
 * Claiming them at boot cost the tighter board about 20 KB of heap. Audio starts as soon as
 * the board profile is known, well before Wi-Fi, so that the console can be asked for a cue
 * immediately; but a task and its stack standing there while lwIP and LVGL take their buffers
 * changes what fits where, and the free heap at Online fell from 115 KB to 94 KB. Waiting
 * costs nothing: the first cue creates both in a few hundred microseconds.
 */
static esp_err_t ensure_task(void)
{
    if (s.queue != NULL) {
        return ESP_OK;
    }
    s.queue = xQueueCreate(QUEUE_LEN, sizeof(req_t));
    if (s.queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "lg_audio", TASK_STACK, NULL, TASK_PRIO, NULL) != pdPASS) {
        vQueueDelete(s.queue);
        s.queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif /* LG_AUDIO_BACKEND */

esp_err_t lg_bsp_audio_start(const lg_board_t *board)
{
    s.board = board;
    if (board == NULL || board->audio.kind == LG_AUDIO_NONE) {
        ESP_LOGI(TAG, "[AUDIO] This board has no speaker");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (board->audio.kind != LG_AUDIO_DAC) {
        /* The pins are in the profile; what is missing is the driver for that codec. */
        ESP_LOGI(TAG, "[AUDIO] Codec at 0x%02X not driven yet, so this board stays silent",
                 board->audio.codec_addr);
        return ESP_ERR_NOT_SUPPORTED;
    }
#if LG_AUDIO_BACKEND
    if (board->audio.amp_enable != LG_PIN_NONE) {
        gpio_config_t amp_cfg = {
            .pin_bit_mask = 1ULL << (unsigned)board->audio.amp_enable,
            .mode = GPIO_MODE_OUTPUT,
        };
        gpio_config(&amp_cfg);
        gpio_set_level((gpio_num_t)board->audio.amp_enable, board->audio.amp_active_low ? 1 : 0);
    }
    s.channel = channel_of(board->audio.speaker);
    if (s.channel < 0) {
        ESP_LOGW(TAG, "[AUDIO] Speaker pin %d is not a DAC pin", (int)board->audio.speaker);
        return ESP_ERR_INVALID_ARG;
    }
    s.available = true;   /* the queue and task wait for the first sound; see ensure_task */
    /* A missing key is normal on a board that has never been muted; a bus that will not open
     * is not, and it used to pass unnoticed, so it says so now. */
    s.volume = LG_VOLUME_HIGH;   /* a handheld nobody has quietened is loud */
    nvs_handle_t nvs;
    esp_err_t nvs_err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (nvs_err == ESP_OK) {
        uint8_t stored = 0;
        if (nvs_get_u8(nvs, NVS_KEY_VOLUME, &stored) == ESP_OK) {
            s.volume = stored > LG_VOLUME_HIGH ? (uint8_t)LG_VOLUME_HIGH : stored;
        } else if (nvs_get_u8(nvs, NVS_KEY_MUTE, &stored) == ESP_OK && stored != 0) {
            s.volume = LG_VOLUME_OFF;   /* carried over from the older on/off flag */
        }
        nvs_close(nvs);
    } else if (nvs_err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "[AUDIO] Volume setting unreadable (%s); staying loud until it is set again",
                 esp_err_to_name(nvs_err));
    }
    ESP_LOGI(TAG, "[AUDIO] Speaker on GPIO%d, DAC channel %d, amplifier on GPIO%d active %s",
             (int)board->audio.speaker, s.channel, (int)board->audio.amp_enable,
             board->audio.amp_active_low ? "low" : "high");
    return ESP_OK;
#else
    ESP_LOGI(TAG, "[AUDIO] A DAC board, but this chip has none");
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

bool lg_bsp_audio_available(void)
{
    return s.available;
}

bool lg_bsp_audio_muted(void)
{
    return !s.available || s.volume == LG_VOLUME_OFF;
}

uint8_t lg_bsp_audio_volume(void)
{
    return s.available ? s.volume : (uint8_t)LG_VOLUME_OFF;
}

const char *lg_bsp_audio_volume_name(uint8_t level)
{
    switch (level) {
    case LG_VOLUME_OFF:    return "off";
    case LG_VOLUME_LOW:    return "low";
    case LG_VOLUME_MEDIUM: return "medium";
    default:               return "high";
    }
}

esp_err_t lg_bsp_audio_set_volume(uint8_t level)
{
    s.volume = level > LG_VOLUME_HIGH ? (uint8_t)LG_VOLUME_HIGH : level;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;   /* the choice still holds until the next boot */
    }
    err = nvs_set_u8(h, NVS_KEY_VOLUME, s.volume);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "[AUDIO] Volume %s%s", lg_bsp_audio_volume_name(s.volume),
             s.volume == LG_VOLUME_OFF ? " (urgent broadcasts still sound)" : "");
    return err;
}

static esp_err_t enqueue(const req_t *r)
{
    if (!s.available) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s.volume == LG_VOLUME_OFF && !r->always) {
        return ESP_OK;   /* asked for, deliberately not played */
    }
#if LG_AUDIO_BACKEND
    if (ensure_task() != ESP_OK) {
        return ESP_ERR_NO_MEM;
    }
#endif
    if (s.queue == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    /* A cue is never worth waiting for: a full queue means sounds are already playing. */
    return xQueueSend(s.queue, r, 0) == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t lg_bsp_audio_cue(lg_cue_t cue)
{
    if (cue >= LG_CUE_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    /* The emergency exception: urgent is the one cue a quietened handheld still plays. */
    req_t r = { .cue = (uint8_t)cue, .hz = 0, .ms = 0, .always = cue == LG_CUE_URGENT };
    return enqueue(&r);
}

esp_err_t lg_bsp_audio_tone(uint32_t hz, uint32_t ms)
{
    if (hz > 20000u || ms > 5000u) {
        return ESP_ERR_INVALID_ARG;
    }
    req_t r = { .cue = LG_CUE_COUNT, .hz = (uint16_t)hz, .ms = (uint16_t)ms, .always = false };
    return enqueue(&r);
}

int lg_bsp_i2c_scan(uint8_t *out, size_t max)
{
    i2c_master_bus_handle_t bus = lg_bsp_touch_i2c_bus();
    if (bus == NULL) {
        return 0;   /* this board has no I2C bus open, so nothing can answer */
    }
    size_t found = 0;
    for (uint8_t addr = 0x08; addr <= 0x77 && found < max; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK) {
            if (out != NULL) {
                out[found] = addr;
            }
            found++;
        }
    }
    return (int)found;
}
