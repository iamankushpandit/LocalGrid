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
 * Two backends sit under one vocabulary, and the board profile picks between them (D9):
 *
 *   DAC     the classic ESP32's cosine wave generator into a small amplifier (Hosyond). It
 *           needs no sample buffer, no DMA and no timer of ours.
 *   ES8311  a codec on I2C for control and I2S for samples (FNK0104B, an ESP32-S3 with no
 *           DAC). Sines are generated here in small blocks and the codec's DAC plays them.
 *
 * LG_AUDIO_BACKEND is the seam: on a chip that can drive neither, everything that exists only
 * to make a noise compiles away and the public calls report ESP_ERR_NOT_SUPPORTED.
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
#define LG_AUDIO_DAC_BACKEND 1
#include "driver/dac_cosine.h"
#else
#define LG_AUDIO_DAC_BACKEND 0
#endif

#if SOC_I2S_SUPPORTED
#define LG_AUDIO_CODEC_BACKEND 1
#include <math.h>
#include "driver/i2s_std.h"
#else
#define LG_AUDIO_CODEC_BACKEND 0
#endif

#define LG_AUDIO_BACKEND (LG_AUDIO_DAC_BACKEND || LG_AUDIO_CODEC_BACKEND)

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
#define TASK_STACK 3072  /* the codec path calls sinf and writes to I2S from here */
#define TASK_PRIO  2     /* below the drawing task, which runs at 3 */

static struct {
    const lg_board_t *board;
    QueueHandle_t     queue;
    bool              available;
    bool              amp_on;
    uint8_t           volume;   /* lg_volume_t; OFF is the mute */
#if LG_AUDIO_DAC_BACKEND
    int               channel;
#endif
#if LG_AUDIO_CODEC_BACKEND
    i2c_master_dev_handle_t codec;
    i2s_chan_handle_t       tx;
    bool                    codec_up;
    bool                    tx_enabled;
    uint32_t                phase;   /* kept across segments, so a change of note does not click */
    int32_t                 level;   /* the amplitude the last sample was written at */
#endif
} s;

#if LG_AUDIO_BACKEND

/*
 * One segment of a cue: a frequency, how long to hold it, and how loud. A frequency of 0 is a
 * rest, which is what separates two blips.
 *
 * Loudness is per segment, not per cue, because a bell is a strike that fades: one level for
 * a whole cue can only make a beep. There are four steps (full, -6, -12, -18 dB): the DAC's
 * generator offers exactly those, and the codec path keeps the same steps so a cue sounds the
 * same shape on either board.
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
 * amplitude tail is gone. Full is the ceiling: the DAC path has four attenuation steps and no
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

/*
 * The amplifier. Without this the speaker stays silent however right everything else is, and
 * the failure is quiet rather than noisy: on a board of this family the same line was declared
 * as an LED, so switching the LED off switched the speaker off with it.
 */
static void amp(bool on)
{
    s.amp_on = on;
    if (s.board == NULL || s.board->audio.amp_enable == LG_PIN_NONE) {
        return;
    }
    bool high = s.board->audio.amp_active_low ? !on : on;
    gpio_set_level((gpio_num_t)s.board->audio.amp_enable, high ? 1 : 0);
}

/*
 * The volume setting as an attenuation offset: HIGH leaves a segment as written, and each step
 * down adds 6 dB of attenuation, clamped at the quietest step. An emergency is played exactly
 * as written, whatever the setting.
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

#if LG_AUDIO_DAC_BACKEND

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
static void dac_segment(uint16_t hz, uint16_t ms, uint8_t atten)
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

#endif /* LG_AUDIO_DAC_BACKEND */

#if LG_AUDIO_CODEC_BACKEND

/*
 * The ES8311 path.
 *
 * Clocking: the ESP32 is the I2S master and drives MCLK at 384 times the sample rate, which at
 * 16 kHz is 6.144 MHz, and the codec divides that down itself (pre-divider 3, multiplier 2,
 * DAC oversampling 0x20, LRCK divider 0x00FF, BCLK divider 4). Those are the codec's own
 * coefficient-table values for exactly that pair; any other MCLK needs a different row.
 *
 * The ESP32-S3 has no audio PLL, so MCLK comes from the 160 MHz PLL through the I2S fractional
 * divider. Gume asked for the APLL on this board under the Arduino core's older driver; on
 * ESP-IDF v6.1 there is nothing to ask for, and Espressif's own ES8311 example runs this exact
 * rate and multiple on the S3.
 *
 * 16 kHz is plenty: the highest note in the vocabulary is 2093 Hz.
 */
#define CODEC_RATE_HZ     16000
#define CODEC_I2C_HZ      100000
#define CODEC_I2C_MS      50
#define CODEC_BLOCK       128        /* frames per write: 8 ms, and 512 bytes of stereo */
#define CODEC_DMA_DESC    4
#define CODEC_DMA_FRAMES  256        /* 64 ms queued ahead of the speaker at most */
#define CODEC_RAMP        48         /* 3 ms from one loudness to the next, so steps do not click */
#define CODEC_PEAK        24000      /* about -3 dBFS, headroom for the codec's output stage */
#define CODEC_WRITE_MS    500

/*
 * Held on for this long after the last cue, and switched on and off as a pair with the I2S
 * clock. Switching an amplifier is itself a pop, so a burst of cues keeps it up throughout;
 * an idle handheld still drops it, so a battery is not feeding a class-D stage for nothing.
 */
#define CODEC_AMP_TAIL_MS 1500

/* ES8311 registers this driver touches, by the datasheet's numbers. */
#define ES_RESET    0x00
#define ES_CLK1     0x01   /* clock on/off and source */
#define ES_CLK2     0x02   /* pre-divider and multiplier */
#define ES_CLK3     0x03   /* ADC fs mode and oversampling */
#define ES_CLK4     0x04   /* DAC oversampling */
#define ES_CLK5     0x05   /* ADC and DAC dividers */
#define ES_CLK6     0x06   /* BCLK divider and inversion */
#define ES_CLK7     0x07   /* LRCK divider, high bits */
#define ES_CLK8     0x08   /* LRCK divider, low byte */
#define ES_SDP_IN   0x09   /* serial data into the DAC: format, width, mute */
#define ES_SDP_OUT  0x0A   /* serial data out of the ADC */
#define ES_SYS_0B   0x0B
#define ES_SYS_0C   0x0C
#define ES_SYS_0D   0x0D   /* analogue power */
#define ES_SYS_0E   0x0E   /* PGA and ADC modulator power */
#define ES_SYS_10   0x10
#define ES_SYS_11   0x11
#define ES_SYS_12   0x12   /* DAC power */
#define ES_SYS_13   0x13   /* output drive */
#define ES_SYS_14   0x14   /* input selection */
#define ES_ADC_15   0x15
#define ES_ADC_16   0x16
#define ES_ADC_17   0x17   /* ADC volume */
#define ES_ADC_1B   0x1B
#define ES_ADC_1C   0x1C
#define ES_DAC_31   0x31   /* DAC mute */
#define ES_DAC_32   0x32   /* DAC volume: 0.5 dB a step, 0xBF is 0 dB, 0x00 is silence */
#define ES_DAC_37   0x37   /* DAC ramp and equaliser bypass */
#define ES_GPIO_44  0x44
#define ES_GP_45    0x45

#define ES_VOL_0DB  0xBF

static esp_err_t es_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s.codec, buf, sizeof(buf), CODEC_I2C_MS);
}

static esp_err_t es_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s.codec, &reg, 1, val, 1, CODEC_I2C_MS);
}

/* Read, keep the bits in keep, OR in set, write back. */
static esp_err_t es_update(uint8_t reg, uint8_t keep, uint8_t set)
{
    uint8_t v = 0;
    esp_err_t err = es_read(reg, &v);
    if (err != ESP_OK) {
        return err;
    }
    return es_write(reg, (uint8_t)((v & keep) | set));
}

/*
 * The board's volume ceiling as the codec's volume register.
 *
 * The register is logarithmic, half a decibel per step, so a percentage must go through
 * decibels first. Laid straight onto the byte, 60% lands near -19.5 dB and 100% at +32 dB:
 * too quiet in the middle and clipped at the top, which is how Gume first got it wrong.
 * The ceiling is a fraction of amplitude, never above unity.
 */
static uint8_t volume_register(uint8_t percent)
{
    if (percent == 0) {
        return 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    float db = 20.0f * log10f((float)percent / 100.0f);
    long reg = lroundf((float)ES_VOL_0DB + db * 2.0f);
    return (uint8_t)(reg < 1 ? 1 : reg > ES_VOL_0DB ? ES_VOL_0DB : reg);
}

/*
 * Brings the codec up for playback only: the ESP32 is master, MCLK comes in on its pin, 16-bit
 * Philips I2S at 16 kHz, the DAC and output drive powered, the ADC and microphone left alone.
 * The sequence follows the ES8311 datasheet and Espressif's codec driver for the same part.
 */
static esp_err_t codec_registers(void)
{
    esp_err_t err = ESP_OK;
    /* The first write after power-up is sometimes lost, so the noise-immunity setting goes
     * twice before anything that matters. */
    es_write(ES_GPIO_44, 0x08);
    err |= es_write(ES_GPIO_44, 0x08);

    err |= es_write(ES_CLK1, 0x30);
    err |= es_write(ES_CLK2, 0x00);
    err |= es_write(ES_CLK3, 0x10);
    err |= es_write(ES_ADC_16, 0x24);
    err |= es_write(ES_CLK4, 0x10);
    err |= es_write(ES_CLK5, 0x00);
    err |= es_write(ES_SYS_0B, 0x00);
    err |= es_write(ES_SYS_0C, 0x00);
    err |= es_write(ES_SYS_10, 0x1F);
    err |= es_write(ES_SYS_11, 0x7F);
    err |= es_write(ES_RESET, 0x80);           /* out of reset, codec as I2S slave */
    err |= es_write(ES_CLK1, 0x3F);            /* every clock on, internal clock from MCLK */
    err |= es_update(ES_CLK6, 0xDF, 0x00);     /* BCLK not inverted */
    err |= es_write(ES_SYS_13, 0x10);
    err |= es_write(ES_ADC_1B, 0x0A);
    err |= es_write(ES_ADC_1C, 0x6A);
    err |= es_write(ES_GPIO_44, 0x08);         /* no DAC loopback into the ADC */

    /* Data format: 16-bit words, standard I2S, both directions. */
    err |= es_update(ES_SDP_IN, 0xE0, 0x0C);
    err |= es_update(ES_SDP_OUT, 0xE0, 0x0C);

    /* Clock dividers for 6.144 MHz MCLK at 16 kHz (see the note above). */
    err |= es_update(ES_CLK2, 0x07, (uint8_t)(((3 - 1) << 5) | (1 << 3)));
    err |= es_write(ES_CLK5, 0x00);
    err |= es_update(ES_CLK3, 0x80, 0x10);
    err |= es_update(ES_CLK4, 0x80, 0x20);
    err |= es_update(ES_CLK7, 0xC0, 0x00);
    err |= es_write(ES_CLK8, 0xFF);
    err |= es_update(ES_CLK6, 0xE0, (uint8_t)(4 - 1));

    /* Start: unmute the serial inputs, power the analogue side and the DAC. */
    err |= es_write(ES_RESET, 0x80);
    err |= es_write(ES_CLK1, 0x3F);
    err |= es_update(ES_SDP_IN, 0xBF, 0x00);
    err |= es_update(ES_SDP_OUT, 0xBF, 0x00);
    err |= es_write(ES_ADC_17, 0xBF);
    err |= es_write(ES_SYS_0E, 0x02);
    err |= es_write(ES_SYS_12, 0x00);
    err |= es_write(ES_SYS_14, 0x1A);
    err |= es_write(ES_SYS_0D, 0x01);
    err |= es_write(ES_ADC_15, 0x40);
    err |= es_write(ES_DAC_37, 0x08);
    err |= es_write(ES_GP_45, 0x00);

    /* The volume register resets to silence, so a codec that is otherwise right stays mute
     * until this is written. */
    err |= es_update(ES_DAC_31, 0x9F, 0x00);
    err |= es_write(ES_DAC_32, volume_register(s.board->audio.max_volume));
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

/*
 * Claims the codec and the I2S channel. Runs on the audio task at the first sound: the codec
 * shares the touch controller's I2C bus, which the display start opens after audio starts.
 */
static esp_err_t codec_open(void)
{
    const lg_audio_profile_t *a = &s.board->audio;
    i2c_master_bus_handle_t bus = lg_bsp_touch_i2c_bus();
    if (bus == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (i2c_master_probe(bus, a->codec_addr, CODEC_I2C_MS) != ESP_OK) {
        ESP_LOGW(TAG, "[AUDIO] No ES8311 answering at 0x%02X; this board stays silent", a->codec_addr);
        return ESP_ERR_NOT_FOUND;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = a->codec_addr,
        .scl_speed_hz = CODEC_I2C_HZ,
    };
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &s.codec);
    if (err != ESP_OK) {
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CODEC_DMA_DESC;
    chan_cfg.dma_frame_num = CODEC_DMA_FRAMES;
    chan_cfg.auto_clear = true;   /* an empty buffer plays zeros, which on a codec is silence */
    err = i2s_new_channel(&chan_cfg, &s.tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[AUDIO] No I2S channel for the codec: %s", esp_err_to_name(err));
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(CODEC_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)a->i2s_mclk,
            .bclk = (gpio_num_t)a->i2s_bclk,
            .ws   = (gpio_num_t)a->i2s_ws,
            .dout = (gpio_num_t)a->i2s_dout,
            .din  = I2S_GPIO_UNUSED,   /* the microphone is not used */
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    err = i2s_channel_init_std_mode(s.tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[AUDIO] I2S would not take the codec's clock or pins: %s", esp_err_to_name(err));
        return err;
    }
    /* The codec divides MCLK, so MCLK must be running while its dividers are set. */
    err = i2s_channel_enable(s.tx);
    if (err != ESP_OK) {
        return err;
    }
    s.tx_enabled = true;
    err = codec_registers();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[AUDIO] ES8311 at 0x%02X did not take its settings", a->codec_addr);
        return err;
    }
    s.codec_up = true;
    ESP_LOGI(TAG, "[AUDIO] ES8311 at 0x%02X up: %d Hz, MCLK %d Hz on GPIO%d, ceiling %u%% (register 0x%02X)",
             a->codec_addr, CODEC_RATE_HZ, CODEC_RATE_HZ * 384, (int)a->i2s_mclk, (unsigned)a->max_volume,
             volume_register(a->max_volume));
    return ESP_OK;
}

/*
 * Writes frames of one note (or of silence when hz is 0), easing the loudness from wherever
 * the last sample left it. Blocks until the samples are in the DMA, which is what paces a cue
 * to real time.
 */
static void codec_render(uint16_t hz, uint32_t frames, int32_t target)
{
    static int16_t block[CODEC_BLOCK * 2];
    const uint32_t step = (uint32_t)(((uint64_t)hz << 32) / CODEC_RATE_HZ);
    int32_t from = s.level;
    uint32_t done = 0;
    while (done < frames && s.codec_up) {
        uint32_t n = frames - done > CODEC_BLOCK ? CODEC_BLOCK : frames - done;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t k = done + i;
            int32_t lvl = k < CODEC_RAMP ? from + (target - from) * (int32_t)k / CODEC_RAMP : target;
            int16_t v = 0;
            if (lvl != 0) {
                float x = (float)s.phase * (float)(2.0 * M_PI / 4294967296.0);
                v = (int16_t)((float)lvl * sinf(x));
            }
            s.phase += step;
            block[2 * i] = v;       /* left */
            block[2 * i + 1] = v;   /* right: the codec's DAC is mono and takes either */
        }
        size_t written = 0;
        if (i2s_channel_write(s.tx, block, n * 2 * sizeof(int16_t), &written, CODEC_WRITE_MS) != ESP_OK) {
            break;
        }
        done += n;
    }
    s.level = target;
}

static void codec_segment(uint16_t hz, uint16_t ms, uint8_t atten)
{
    uint32_t frames = (uint32_t)ms * CODEC_RATE_HZ / 1000u;
    codec_render(hz, frames, hz == 0 ? 0 : (CODEC_PEAK >> atten));
}

/* The clock and the amplifier go up and down together; the codec keeps its settings. */
static void codec_power(bool on)
{
    if (!s.codec_up) {
        return;
    }
    if (on && !s.tx_enabled) {
        s.tx_enabled = i2s_channel_enable(s.tx) == ESP_OK;
    }
    if (!on && s.tx_enabled) {
        i2s_channel_disable(s.tx);
        s.tx_enabled = false;
    }
}

#endif /* LG_AUDIO_CODEC_BACKEND */

static bool is_codec(void)
{
    return s.board != NULL && s.board->audio.kind == LG_AUDIO_ES8311_I2S;
}

static void play_segment(uint16_t hz, uint16_t ms, uint8_t atten)
{
#if LG_AUDIO_CODEC_BACKEND
    if (is_codec()) {
        codec_segment(hz, ms, atten);
        return;
    }
#endif
#if LG_AUDIO_DAC_BACKEND
    dac_segment(hz, ms, atten);
#else
    (void)hz;
    (void)ms;
    (void)atten;
#endif
}

/* A cue ends by easing to silence, so its last note stops rather than being cut. */
static void end_cue(void)
{
#if LG_AUDIO_CODEC_BACKEND
    if (is_codec() && s.codec_up) {
        codec_render(0, CODEC_RAMP, 0);
    }
#endif
}

/*
 * Output on before a cue, and off only once nothing else has been asked for within the tail.
 * Dropping the amplifier as the last segment ends clips the end off every sound; on the codec
 * path the tail is longer, since samples are still in the DMA and switching is itself a pop.
 */
#define DAC_AMP_TAIL_MS 60

static void power(bool on)
{
#if LG_AUDIO_CODEC_BACKEND
    if (is_codec()) {
        if (on) {
            codec_power(true);
            amp(true);
        } else {
            amp(false);
            codec_power(false);
        }
        return;
    }
#endif
    amp(on);
}

static void audio_task(void *arg)
{
    (void)arg;
#if LG_AUDIO_CODEC_BACKEND
    if (is_codec()) {
        esp_err_t err = codec_open();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "[AUDIO] Codec not started (%s); sounds are refused from now on", esp_err_to_name(err));
            s.available = false;
        }
    }
#endif
    const uint32_t tail_ms = is_codec() ? CODEC_AMP_TAIL_MS : DAC_AMP_TAIL_MS;
    req_t r;
    for (;;) {
        TickType_t wait = s.amp_on ? pdMS_TO_TICKS(tail_ms) : portMAX_DELAY;
        if (xQueueReceive(s.queue, &r, wait) != pdTRUE) {
            power(false);   /* the tail passed with nothing more to play */
            continue;
        }
        if (!s.available) {
            continue;
        }
        if (!s.amp_on) {
            power(true);
        }
        if (r.cue < LG_CUE_COUNT) {
            const cue_t *c = &CUES[r.cue];
            for (uint8_t i = 0; i < c->count; i++) {
                play_segment(c->segs[i].hz, c->segs[i].ms, atten_for(c->segs[i].atten, r.always));
            }
        } else {
            play_segment(r.hz, r.ms, atten_for(ATTEN_QUIET, false));
        }
        end_cue();
    }
}

/*
 * The queue and the task are claimed at the first sound, not at start.
 *
 * Claiming them at boot cost the tighter board about 20 KB of heap. Audio starts as soon as
 * the board profile is known, well before Wi-Fi, so that the console can be asked for a cue
 * immediately; but a task and its stack standing there while lwIP and the display take their buffers
 * changes what fits where, and the free heap at Online fell from 115 KB to 94 KB. Waiting
 * costs nothing: the first cue creates both in a few hundred microseconds.
 *
 * A codec board also waits for its I2C bus, which the touch driver opens during the display
 * start; a sound asked for before that is refused rather than creating a task with no codec.
 */
static esp_err_t ensure_task(void)
{
    if (s.queue != NULL) {
        return ESP_OK;
    }
    if (is_codec() && lg_bsp_touch_i2c_bus() == NULL) {
        return ESP_ERR_INVALID_STATE;
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

/* The amplifier line as an output, starting off. */
static void amp_setup(const lg_board_t *board)
{
    if (board->audio.amp_enable == LG_PIN_NONE) {
        return;
    }
    gpio_config_t amp_cfg = {
        .pin_bit_mask = 1ULL << (unsigned)board->audio.amp_enable,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&amp_cfg);
    amp(false);
}

/* A missing key is normal on a board that has never been muted; a bus that will not open
 * is not, and it used to pass unnoticed, so it says so now. */
static void load_volume(void)
{
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
}

#endif /* LG_AUDIO_BACKEND */

esp_err_t lg_bsp_audio_start(const lg_board_t *board)
{
    s.board = board;
    if (board == NULL || board->audio.kind == LG_AUDIO_NONE) {
        ESP_LOGI(TAG, "[AUDIO] This board has no speaker");
        return ESP_ERR_NOT_SUPPORTED;
    }
    const lg_audio_profile_t *a = &board->audio;
    if (a->kind == LG_AUDIO_DAC) {
#if LG_AUDIO_DAC_BACKEND
        s.channel = channel_of(a->speaker);
        if (s.channel < 0) {
            ESP_LOGW(TAG, "[AUDIO] Speaker pin %d is not a DAC pin", (int)a->speaker);
            return ESP_ERR_INVALID_ARG;
        }
        amp_setup(board);
        load_volume();
        s.available = true;   /* the queue and task wait for the first sound; see ensure_task */
        ESP_LOGI(TAG, "[AUDIO] Speaker on GPIO%d, DAC channel %d, amplifier on GPIO%d active %s",
                 (int)a->speaker, s.channel, (int)a->amp_enable, a->amp_active_low ? "low" : "high");
        return ESP_OK;
#else
        ESP_LOGI(TAG, "[AUDIO] A DAC board, but this chip has none");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    if (a->kind == LG_AUDIO_ES8311_I2S) {
#if LG_AUDIO_CODEC_BACKEND
        if (a->codec_addr == 0 || a->i2s_mclk == LG_PIN_NONE || a->i2s_bclk == LG_PIN_NONE ||
            a->i2s_ws == LG_PIN_NONE || a->i2s_dout == LG_PIN_NONE) {
            ESP_LOGW(TAG, "[AUDIO] Codec board profile is missing its address or I2S pins");
            return ESP_ERR_INVALID_ARG;
        }
        amp_setup(board);
        load_volume();
        s.available = true;   /* the codec is set up by the audio task at the first sound */
        ESP_LOGI(TAG, "[AUDIO] ES8311 at 0x%02X, I2S MCLK %d BCLK %d WS %d DOUT %d, amplifier on GPIO%d active %s; "
                 "set up at the first sound", a->codec_addr, (int)a->i2s_mclk, (int)a->i2s_bclk, (int)a->i2s_ws,
                 (int)a->i2s_dout, (int)a->amp_enable, a->amp_active_low ? "low" : "high");
        return ESP_OK;
#else
        ESP_LOGI(TAG, "[AUDIO] A codec board, but this chip has no I2S");
        return ESP_ERR_NOT_SUPPORTED;
#endif
    }
    return ESP_ERR_NOT_SUPPORTED;
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
    esp_err_t err = ensure_task();
    if (err != ESP_OK) {
        return err;
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
#if LG_AUDIO_CODEC_BACKEND
    if (is_codec() && hz >= CODEC_RATE_HZ / 2) {
        return ESP_ERR_INVALID_ARG;   /* above half the sample rate it would alias to another note */
    }
#endif
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
