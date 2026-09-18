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
 *
 * Push-to-talk voice rides on the same two backends (see the header for the rules): 8 kHz mono
 * PCM in and out. On the codec board the I2S port runs full duplex at 16 kHz, so the microphone
 * is decimated to 8 kHz and voice is interpolated up to 16 kHz; on the DAC board voice goes out
 * through the DAC's DMA mode, which has to take the DAC channel from the cosine generator and
 * give it back. One mutex serialises everything that touches the output: the audio task holds
 * it for a whole cue, the voice and microphone calls for one short chunk at a time.
 */
#include "lg_bsp_audio.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lg_bsp_i2c.h"
#include "nvs.h"
#include "soc/soc_caps.h"

#if SOC_DAC_SUPPORTED
#define LG_AUDIO_DAC_BACKEND 1
#include "driver/dac_continuous.h"
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
#define NVS_KEY_TALK   "talkboost"

/* A request is either a cue from the table or one bare tone from the console. */
typedef struct {
    uint8_t  cue;      /* LG_CUE_COUNT means "the tone below instead"; REQ_WAKE means nothing */
    uint16_t hz;
    uint16_t ms;
    bool     always;   /* an emergency: played whatever the volume says */
} req_t;

/* Not a sound: wakes the audio task so it looks at the voice and microphone state again. */
#define REQ_WAKE 0xFF

#define QUEUE_LEN  4
#define TASK_STACK 3072  /* the codec path calls sinf and writes to I2S from here */
#define TASK_PRIO  2     /* below the drawing task, which runs at 3 */

/* While a cue is held back for a voice stream, the audio task looks again this often. */
#define DEFER_POLL_MS 50

/* Voice is written in chunks of this many 8 kHz samples (16 ms), taking the lock per chunk, so
 * an urgent cue waits at most one chunk to cut in. */
#define VOICE_CHUNK 128

/* The longest the public calls wait for the lock: a cue in progress, never an urgent one. */
#define LOCK_WAIT_MS 3000

/*
 * The microphone's gain, in the codec's own steps. Speech at about 30 cm from a small electret
 * is some 60 to 65 dB SPL, well under a millivolt at the capsule, so it needs every bit of the
 * analogue PGA (+30 dB, the maximum) before the ADC; after it, the digital scale adds +18 dB and
 * the ADC volume +4.5 dB (0xC8, 0xBF being 0 dB). About 52 dB in all, which puts ordinary speech
 * near -25 dBFS RMS and leaves some 20 dB for peaks and raised voices before clipping. The same
 * three values are Espressif's example defaults for this codec, and 0xC8 is also what recorded
 * speech on this exact board in Braino's bring-up (the register resets to minimum, and a codec
 * left there records silence). The digital scale is the knob: `mic gain` changes it at run time.
 */
#define MIC_PGA_REG      0x1A   /* register 0x14: analogue mic input, PGA at +30 dB */
#define MIC_ADC_VOLUME   0xC8   /* register 0x17: +4.5 dB */
#define MIC_SCALE_DEFAULT 3     /* register 0x16 low bits: 6 dB a step, 3 is +18 dB */
#define MIC_SCALE_MAX    7

static struct {
    const lg_board_t *board;
    QueueHandle_t     queue;
    TaskHandle_t      task;
    SemaphoreHandle_t lock;         /* everything that touches the output, see the file header */
    StaticSemaphore_t lock_buf;
    bool              available;
    bool              amp_on;       /* what the amplifier line says now */
    bool              out_on;       /* a cue played recently: its tail keeps the output up */
    bool              cue_active;   /* a cue is being played this moment */
    TickType_t        last_cue;
    volatile bool     voice_open;   /* read without the lock as a hint, changed only under it */
    volatile bool     mic_open;
    uint8_t           volume;       /* lg_volume_t; OFF is the mute */
    uint8_t           mic_scale;
    uint8_t          *voice_buf;    /* one chunk, converted for the backend; only while open */
#if LG_AUDIO_DAC_BACKEND
    int               channel;
    dac_continuous_handle_t dac_voice;
    uint8_t           dac_up;       /* output samples per 8 kHz sample: 1, or 3 without the APLL */
    int16_t           dac_prev;
    int32_t           dac_lp;      /* the voice low-pass's state, at the DAC's rate */
    int64_t           dac_until_us; /* when the audio already handed to the DMA will have played */
#endif
#if LG_AUDIO_CODEC_BACKEND
    i2c_master_dev_handle_t codec;
    i2s_chan_handle_t       tx;
    i2s_chan_handle_t       rx;     /* the microphone, the other half of the same port */
    bool                    codec_up;
    bool                    codec_tried;
    bool                    tx_enabled;
    bool                    rx_enabled;
    uint32_t                phase;   /* kept across segments, so a change of note does not click */
    int32_t                 level;   /* the amplitude the last sample was written at */
    int16_t                 up_prev; /* the last voice sample, for interpolating to 16 kHz */
    int16_t                 dec_hist[7];   /* the decimator's delay line, newest last */
    uint8_t                 dec_odd;
    int16_t                *mic_buf;       /* one I2S read; only while the microphone is open */
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
 *
 * Lengthened from 1.1 s to 2.3 s (owner, 2026-09-17): the alert repeats every 4 s while an
 * emergency is up, and a second of sound in every four was easy to miss from another tent or
 * to mistake for a notification. Five pairs and a held last note carry across a camp.
 */
static const seg_t CUE_URGENT[] = {
    { 853, 190, ATTEN_FULL }, { 960, 190, ATTEN_FULL },
    { 853, 190, ATTEN_FULL }, { 960, 190, ATTEN_FULL },
    { 853, 190, ATTEN_FULL }, { 960, 190, ATTEN_FULL },
    { 853, 190, ATTEN_FULL }, { 960, 190, ATTEN_FULL },
    { 853, 190, ATTEN_FULL }, { 960, 400, ATTEN_FULL },
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

/*
 * Voice on the DAC: the channel in DMA mode, 8 bits a sample. It cannot share the channel with
 * the cosine generator, so it exists only while a voice stream is open, and a cue that has to
 * play (an urgent one) takes the channel back by closing it. Called with the lock held.
 *
 * 8 kHz needs the audio PLL: from the default clock the DAC's DMA cannot go below 19.6 kHz on
 * this chip. If the APLL is refused, it runs at 24 kHz and each sample is interpolated to three.
 * Four 800-byte buffers of 16-bit words (the driver widens each byte) hold 200 ms at 8 kHz, which
 * is the jitter a frame arriving late can absorb.
 */
#define DAC_VOICE_DESC   4
#define DAC_VOICE_BUF    1200    /* 4 x 1200 samples at 24 kHz: 200 ms */
#define DAC_VOICE_HZ     8000
#define DAC_VOICE_UP     3       /* 8 kHz straight to the DAC was a staircase that sounded like a
                                  * radio; three interpolated samples each is much smoother */
#define DAC_VOICE_DRAIN_MS 300
#define DAC_LP_A         154     /* 0.6 in 1/256: 1 - e^(-2 pi 3500 / 24000) */   /* the longest voice_stop waits for buffered audio to play */

static esp_err_t dac_voice_open(void)
{
    dac_continuous_config_t cfg = {
        .chan_mask = (dac_channel_mask_t)BIT(s.channel),
        .desc_num  = DAC_VOICE_DESC,
        .buf_size  = DAC_VOICE_BUF,
        .freq_hz   = DAC_VOICE_HZ * DAC_VOICE_UP,
        .offset    = 0,
        .clk_src   = DAC_DIGI_CLK_SRC_DEFAULT,
        .chan_mode = DAC_CHANNEL_MODE_SIMUL,
    };
    s.dac_up = DAC_VOICE_UP;
    esp_err_t err = dac_continuous_new_channels(&cfg, &s.dac_voice);
    if (err != ESP_OK) {
        s.dac_voice = NULL;
        ESP_LOGW(TAG, "[AUDIO] DAC would not stream voice: %s", esp_err_to_name(err));
        return err;
    }
    err = dac_continuous_enable(s.dac_voice);
    if (err != ESP_OK) {
        dac_continuous_del_channels(s.dac_voice);
        s.dac_voice = NULL;
        return err;
    }
    s.dac_prev = 0;
    s.dac_lp = 0;
    s.dac_until_us = esp_timer_get_time();
    ESP_LOGI(TAG, "[AUDIO] Voice out on DAC channel %d at %u Hz%s", s.channel, (unsigned)cfg.freq_hz,
             s.dac_up > 1 ? " (no APLL, interpolated from 8 kHz)" : "");
    return ESP_OK;
}

/* Gives the channel back to the cosine generator. With drain, first lets what is in the DMA
 * play out, so the end of the last word is not cut off. */
static void dac_voice_close(bool drain)
{
    if (s.dac_voice == NULL) {
        return;
    }
    if (drain) {
        int64_t left_ms = (s.dac_until_us - esp_timer_get_time()) / 1000;
        if (left_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(left_ms > DAC_VOICE_DRAIN_MS ? DAC_VOICE_DRAIN_MS : (uint32_t)left_ms) + 1);
        }
    }
    dac_continuous_disable(s.dac_voice);
    dac_continuous_del_channels(s.dac_voice);
    s.dac_voice = NULL;
}

/* One chunk of 8 kHz voice to the DAC, already scaled for the volume. Lock held. */
static esp_err_t dac_voice_chunk(const int16_t *pcm, size_t n, int shift, uint32_t timeout_ms)
{
    if (s.dac_voice == NULL || s.voice_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t out = 0;
    for (size_t i = 0; i < n; i++) {
        int32_t x = shift < 0 ? 0 : (int32_t)pcm[i] >> shift;
        int32_t prev = s.dac_prev;
        for (uint8_t k = 1; k <= s.dac_up; k++) {
            int32_t v = prev + (x - prev) * k / s.dac_up;
            /* One-pole low-pass at about 3.5 kHz (at 24 kHz, a = 0.6): speech stops there, and
             * what the interpolation leaves above it is the fizz an unfiltered DAC plays. */
            s.dac_lp += ((v - s.dac_lp) * DAC_LP_A) / 256;
            int32_t o = (s.dac_lp >> 8) + 128;   /* signed 16-bit to the DAC's 0..255 */
            s.voice_buf[out++] = (uint8_t)(o < 0 ? 0 : o > 255 ? 255 : o);
        }
        s.dac_prev = (int16_t)x;
    }
    size_t loaded = 0;
    int wait = timeout_ms > (uint32_t)INT_MAX ? -1 : (int)timeout_ms;
    esp_err_t err = dac_continuous_write(s.dac_voice, s.voice_buf, out, &loaded, wait);
    /* Track when the queued audio will have played, for a clean stop. */
    int64_t now = esp_timer_get_time();
    if (s.dac_until_us < now) {
        s.dac_until_us = now;
    }
    s.dac_until_us += (int64_t)(loaded / s.dac_up) * 1000000 / DAC_VOICE_HZ;
    return err;
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
/*
 * 120 ms queued ahead of the speaker at most, in six 20 ms buffers, for each direction: 7.5 KB
 * each of internal RAM on the S3. Cues need only a little; voice needs the slack, because a
 * 100 ms frame written into a ring that still holds the last one would otherwise block the
 * network service for most of a frame, and a frame that arrives late plays out of the slack
 * instead of as a gap.
 */
#define CODEC_DMA_DESC    6
#define CODEC_DMA_FRAMES  320
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

    /* Full duplex when the board wires the codec's ADC back: one port, one clock, TX and RX.
     * The receive side shares the transmit side's BCLK and WS, so the transmit channel has to
     * be running whenever the microphone is. */
    const bool mic = a->i2s_din != LG_PIN_NONE;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = CODEC_DMA_DESC;
    chan_cfg.dma_frame_num = CODEC_DMA_FRAMES;
    chan_cfg.auto_clear = true;   /* an empty buffer plays zeros, which on a codec is silence */
    err = i2s_new_channel(&chan_cfg, &s.tx, mic ? &s.rx : NULL);
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
            .din  = mic ? (gpio_num_t)a->i2s_din : I2S_GPIO_UNUSED,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_384;
    err = i2s_channel_init_std_mode(s.tx, &std_cfg);
    if (err == ESP_OK && s.rx != NULL) {
        err = i2s_channel_init_std_mode(s.rx, &std_cfg);   /* a duplex pair takes one config */
    }
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
    ESP_LOGI(TAG, "[AUDIO] ES8311 at 0x%02X up: %d Hz, MCLK %d Hz on GPIO%d, ceiling %u%% (register 0x%02X), "
             "microphone %s", a->codec_addr, CODEC_RATE_HZ, CODEC_RATE_HZ * 384, (int)a->i2s_mclk,
             (unsigned)a->max_volume, volume_register(a->max_volume), s.rx != NULL ? "on DIN" : "not wired");
    return ESP_OK;
}

/*
 * The codec is opened once, at the first sound or the first voice or microphone call, whichever
 * comes first, and never retried after a failure (a half-claimed I2S port cannot be claimed
 * again). Lock held. A bus not yet open is "not yet", not a failure.
 */
static esp_err_t codec_ensure(void)
{
    if (s.codec_up) {
        return ESP_OK;
    }
    if (s.codec_tried) {
        return ESP_ERR_INVALID_STATE;
    }
    if (lg_bsp_touch_i2c_bus() == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s.codec_tried = true;
    esp_err_t err = codec_open();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[AUDIO] Codec not started (%s); sounds are refused from now on", esp_err_to_name(err));
        s.available = false;
    }
    return err;
}

/*
 * The microphone's three gain registers (see MIC_* above). The digital scale shares its
 * register with other bits, which are kept. Lock held, codec up.
 */
static esp_err_t codec_mic_gain(void)
{
    esp_err_t err = es_write(ES_SYS_14, MIC_PGA_REG);
    err |= es_write(ES_ADC_17, MIC_ADC_VOLUME);
    err |= es_update(ES_ADC_16, 0xF8, (uint8_t)(s.mic_scale & 0x07));
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

/*
 * 16 kHz to 8 kHz: the classic 7-tap half-band low-pass, (-1, 0, 9, 16, 9, 0, -1) / 32, kept
 * at every second input. Unity at DC, -6 dB at 4 kHz where the new Nyquist falls, and deep
 * attenuation towards 8 kHz, which is what would otherwise fold back into the speech band.
 * Integer only, five multiplies an output sample. Returns true when x completed an output.
 */
static bool decimate(int16_t x, int16_t *out)
{
    int16_t *h = s.dec_hist;
    memmove(h, h + 1, 6 * sizeof(int16_t));
    h[6] = x;
    s.dec_odd ^= 1;
    if (s.dec_odd) {
        return false;
    }
    int32_t y = -(int32_t)h[0] + 9 * (int32_t)h[2] + 16 * (int32_t)h[3] + 9 * (int32_t)h[4] - (int32_t)h[6];
    y = (y + 16) >> 5;
    *out = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
    return true;
}

/*
 * One chunk of 8 kHz voice to the codec: linearly interpolated to 16 kHz, scaled for the volume
 * (shift < 0 is silence), both slots carrying the same sample. Lock held.
 */
static esp_err_t codec_voice_chunk(const int16_t *pcm, size_t n, int shift, uint32_t timeout_ms)
{
    if (!s.codec_up || s.voice_buf == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    int16_t *out = (int16_t *)s.voice_buf;
    int32_t prev = s.up_prev;
    for (size_t i = 0; i < n; i++) {
        int32_t x = shift < 0 ? 0 : (int32_t)pcm[i] >> shift;
        int16_t mid = (int16_t)((prev + x) / 2);
        out[4 * i] = mid;
        out[4 * i + 1] = mid;
        out[4 * i + 2] = (int16_t)x;
        out[4 * i + 3] = (int16_t)x;
        prev = x;
    }
    s.up_prev = (int16_t)prev;
    size_t written = 0;
    return i2s_channel_write(s.tx, out, n * 4 * sizeof(int16_t), &written, timeout_ms);
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

#define LOCK_FOREVER UINT32_MAX

static bool lock_take(uint32_t ms)
{
    TickType_t ticks = ms == LOCK_FOREVER ? portMAX_DELAY : pdMS_TO_TICKS(ms);
    return s.lock != NULL && xSemaphoreTake(s.lock, ticks) == pdTRUE;
}

static void lock_give(void)
{
    xSemaphoreGive(s.lock);
}

/*
 * Sets the clock and the amplifier from what is going on, rather than from whoever asked last.
 * Lock held.
 *
 *   the codec's clock  runs for a cue and its tail, an open voice stream, or the microphone
 *                      (the receive side has no clock of its own);
 *   the amplifier      is on for a cue being played, a cue's tail unless the microphone is
 *                      listening, and a voice stream unless the volume is off. While the
 *                      microphone is open only an urgent cue can be playing, so that is the
 *                      one sound that switches the speaker on over a recording.
 *
 * Clock before amplifier going up and amplifier before clock going down, as before.
 */
static void output_apply(void)
{
    bool amp_want = s.cue_active || (s.out_on && !s.mic_open) ||
                    (s.voice_open && s.volume != LG_VOLUME_OFF);
#if LG_AUDIO_CODEC_BACKEND
    if (is_codec()) {
        bool clock = s.cue_active || s.out_on || s.voice_open || s.mic_open;
        if (clock) {
            codec_power(true);
            if (amp_want != s.amp_on) {
                amp(amp_want);
            }
        } else {
            if (s.amp_on) {
                amp(false);
            }
            codec_power(false);
        }
        return;
    }
#endif
    if (amp_want != s.amp_on) {
        amp(amp_want);
    }
}

/* Ends an open voice stream early, for an urgent cue or the microphone. Lock held. */
static void voice_preempt(const char *why)
{
    if (!s.voice_open) {
        return;
    }
    s.voice_open = false;
#if LG_AUDIO_DAC_BACKEND
    if (!is_codec()) {
        dac_voice_close(false);
    }
#endif
    ESP_LOGI(TAG, "[AUDIO] Voice stream cut for %s", why);
}

/* One request, start to finish, with the output held for it. */
static void play_request(const req_t *r)
{
    if (!lock_take(LOCK_FOREVER)) {
        return;
    }
    if (r->always) {
        voice_preempt("an urgent cue");
    }
    s.cue_active = true;
    s.out_on = true;
    output_apply();
    if (r->cue < LG_CUE_COUNT) {
        const cue_t *c = &CUES[r->cue];
        for (uint8_t i = 0; i < c->count; i++) {
            play_segment(c->segs[i].hz, c->segs[i].ms, atten_for(c->segs[i].atten, r->always));
        }
    } else {
        play_segment(r->hz, r->ms, atten_for(ATTEN_QUIET, false));
    }
    end_cue();
    s.cue_active = false;
    s.last_cue = xTaskGetTickCount();
    output_apply();
    lock_give();
}

/*
 * The audio task. Ordinary cues that arrive while a voice stream or the microphone is open are
 * held (up to QUEUE_LEN of them, oldest first) and played once both have closed; an urgent cue
 * plays at once and ends the voice stream. See the header.
 */
static void audio_task(void *arg)
{
    (void)arg;
#if LG_AUDIO_CODEC_BACKEND
    if (is_codec() && lock_take(LOCK_FOREVER)) {
        (void)codec_ensure();
        output_apply();
        lock_give();
    }
#endif
    const TickType_t tail = pdMS_TO_TICKS(is_codec() ? CODEC_AMP_TAIL_MS : DAC_AMP_TAIL_MS);
    req_t held[QUEUE_LEN];
    uint8_t held_n = 0;
    req_t r;
    for (;;) {
        TickType_t wait = held_n > 0 ? pdMS_TO_TICKS(DEFER_POLL_MS) : s.out_on ? tail : portMAX_DELAY;
        if (xQueueReceive(s.queue, &r, wait) == pdTRUE && r.cue != REQ_WAKE) {
            if (!s.available) {
                continue;
            }
            if (!r.always && (s.voice_open || s.mic_open)) {
                if (held_n < QUEUE_LEN) {
                    held[held_n++] = r;
                } else {
                    ESP_LOGW(TAG, "[AUDIO] Cue dropped: %u already waiting for voice to end", (unsigned)held_n);
                }
                continue;
            }
            play_request(&r);
            continue;
        }
        /* A wake-up or a timeout: see whether held cues may play and whether the tail is over. */
        if (held_n > 0 && !s.voice_open && !s.mic_open) {
            for (uint8_t i = 0; i < held_n; i++) {
                play_request(&held[i]);
            }
            held_n = 0;
        }
        if (s.out_on && xTaskGetTickCount() - s.last_cue >= tail && lock_take(LOCK_FOREVER)) {
            s.out_on = false;   /* the tail passed with nothing more to play */
            output_apply();
            lock_give();
        }
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
    if (xTaskCreate(audio_task, "lg_audio", TASK_STACK, NULL, TASK_PRIO, &s.task) != pdPASS) {
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
#if LG_AUDIO_BACKEND
    if (s.lock == NULL) {
        s.lock = xSemaphoreCreateMutexStatic(&s.lock_buf);
    }
    s.mic_scale = MIC_SCALE_DEFAULT;
#endif
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

/* ------------------------------------------------------------------ push-to-talk voice */

bool lg_bsp_audio_can_record(void)
{
#if LG_AUDIO_CODEC_BACKEND
    return s.available && is_codec() && s.board->audio.i2s_din != LG_PIN_NONE;
#else
    return false;
#endif
}

#if LG_AUDIO_BACKEND

/* What is left of a timeout that started at start. */
static uint32_t ms_left(TickType_t start, uint32_t timeout_ms)
{
    uint32_t spent = (uint32_t)(xTaskGetTickCount() - start) * portTICK_PERIOD_MS;
    return spent >= timeout_ms ? 0 : timeout_ms - spent;
}

/* Tells the audio task that voice or the microphone changed, so held cues and the tail are
 * looked at now rather than at the next cue. */
static void wake_task(void)
{
    if (ensure_task() == ESP_OK) {
        req_t r = { .cue = REQ_WAKE, .hz = 0, .ms = 0, .always = false };
        (void)xQueueSend(s.queue, &r, 0);   /* a full queue will wake it anyway */
    }
}

/*
 * The volume as a right shift of the samples; -1 is silence. Talk plays one 6 dB step louder than
 * the cues (LOW is -6 dB, MEDIUM and HIGH are full), because a chime at LOW is still heard across a
 * room and a voice at LOW was not: the owner found PTT "too faint" at LOW.
 */
static int voice_shift(void)
{
    if (s.volume == LG_VOLUME_OFF) {
        return -1;
    }
    uint8_t level = s.volume > LG_VOLUME_HIGH ? (uint8_t)LG_VOLUME_HIGH : s.volume;
    int shift = (int)(LG_VOLUME_HIGH - level) - 1;
    return shift < 0 ? 0 : shift;
}

#endif /* LG_AUDIO_BACKEND */

#if LG_AUDIO_CODEC_BACKEND
#define MIC_READ_FRAMES   256   /* 16 ms of 16 kHz stereo a read: 1 KB, only while open */
#define MIC_SETTLE_FRAMES 512   /* 32 ms thrown away at start: the ADC settles and the amp clicks */
#endif

esp_err_t lg_bsp_audio_mic_start(void)
{
#if LG_AUDIO_CODEC_BACKEND
    if (!lg_bsp_audio_can_record()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s.mic_open) {
        return ESP_OK;
    }
    if (!lock_take(LOCK_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = codec_ensure();
    if (err == ESP_OK && s.rx == NULL) {
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK && s.mic_buf == NULL) {
        s.mic_buf = malloc(MIC_READ_FRAMES * 2 * sizeof(int16_t));
        err = s.mic_buf == NULL ? ESP_ERR_NO_MEM : ESP_OK;
    }
    if (err == ESP_OK) {
        err = codec_mic_gain();
    }
    if (err == ESP_OK) {
        voice_preempt("the microphone");   /* half duplex: talking ends listening */
        s.mic_open = true;
        output_apply();                     /* clock on, speaker off so it cannot feed back */
        err = i2s_channel_enable(s.rx);
        s.rx_enabled = err == ESP_OK;
        if (err != ESP_OK) {
            s.mic_open = false;
            output_apply();
        }
    }
    memset(s.dec_hist, 0, sizeof(s.dec_hist));
    s.dec_odd = 0;
    lock_give();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[AUDIO] Microphone not started: %s", esp_err_to_name(err));
        return err;
    }
    size_t junk = 0;
    for (uint32_t got = 0; got < MIC_SETTLE_FRAMES; got += MIC_READ_FRAMES) {
        if (i2s_channel_read(s.rx, s.mic_buf, MIC_READ_FRAMES * 2 * sizeof(int16_t), &junk, 100) != ESP_OK) {
            break;
        }
    }
    ESP_LOGI(TAG, "[AUDIO] Microphone on: 8 kHz, PGA +30 dB, digital +%u dB", (unsigned)s.mic_scale * 6u);
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

int lg_bsp_audio_mic_read(int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
#if LG_AUDIO_CODEC_BACKEND
    if (pcm == NULL) {
        return -ESP_ERR_INVALID_ARG;
    }
    if (!s.mic_open || !s.rx_enabled || s.mic_buf == NULL) {
        return -ESP_ERR_INVALID_STATE;
    }
    TickType_t start = xTaskGetTickCount();
    size_t got = 0;
    while (got < samples) {
        size_t want = (samples - got) * 2;   /* two 16 kHz frames make one 8 kHz sample */
        if (want > MIC_READ_FRAMES) {
            want = MIC_READ_FRAMES;
        }
        size_t bytes = 0;
        esp_err_t err = i2s_channel_read(s.rx, s.mic_buf, want * 2 * sizeof(int16_t), &bytes,
                                         ms_left(start, timeout_ms));
        size_t frames = bytes / (2 * sizeof(int16_t));
        for (size_t i = 0; i < frames; i++) {
            int16_t y;
            /* The codec's mono ADC arrives in the left slot. */
            if (decimate(s.mic_buf[2 * i], &y) && got < samples) {
                pcm[got++] = y;
            }
        }
        if (err == ESP_ERR_TIMEOUT) {
            break;
        }
        if (err != ESP_OK) {
            return got > 0 ? (int)got : -err;
        }
    }
    return (int)got;
#else
    (void)pcm;
    (void)samples;
    (void)timeout_ms;
    return -ESP_ERR_NOT_SUPPORTED;
#endif
}

void lg_bsp_audio_mic_stop(void)
{
#if LG_AUDIO_CODEC_BACKEND
    if (!s.mic_open || !lock_take(LOCK_FOREVER)) {
        return;
    }
    if (s.rx_enabled) {
        i2s_channel_disable(s.rx);
        s.rx_enabled = false;
    }
    s.mic_open = false;
    output_apply();
    free(s.mic_buf);
    s.mic_buf = NULL;
    lock_give();
    ESP_LOGI(TAG, "[AUDIO] Microphone off");
    wake_task();   /* cues held for the recording may play now */
#endif
}

esp_err_t lg_bsp_audio_mic_gain_set(uint8_t step)
{
#if LG_AUDIO_CODEC_BACKEND
    if (step > MIC_SCALE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!lg_bsp_audio_can_record()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s.mic_scale = step;
    esp_err_t err = ESP_OK;
    if (lock_take(LOCK_WAIT_MS)) {
        if (s.codec_up) {
            err = codec_mic_gain();
        }
        lock_give();
    }
    return err;
#else
    (void)step;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

uint8_t lg_bsp_audio_mic_gain(void)
{
#if LG_AUDIO_CODEC_BACKEND
    return s.mic_scale;
#else
    return 0;
#endif
}

esp_err_t lg_bsp_audio_voice_start(void)
{
#if LG_AUDIO_BACKEND
    if (!s.available) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!lock_take(LOCK_WAIT_MS)) {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t err = ESP_OK;
    if (s.mic_open) {
        err = ESP_ERR_INVALID_STATE;   /* this handheld is talking; see the header */
    } else if (!s.voice_open) {
        size_t buf_bytes = 0;
#if LG_AUDIO_CODEC_BACKEND
        if (is_codec()) {
            err = codec_ensure();
            s.up_prev = 0;
            buf_bytes = VOICE_CHUNK * 4 * sizeof(int16_t);   /* two stereo frames a sample */
        }
#endif
#if LG_AUDIO_DAC_BACKEND
        if (!is_codec()) {
            buf_bytes = VOICE_CHUNK * 3;   /* up to three 8-bit samples a sample */
        }
#endif
        if (err == ESP_OK && buf_bytes == 0) {
            err = ESP_ERR_NOT_SUPPORTED;
        }
        if (err == ESP_OK && s.voice_buf == NULL) {
            s.voice_buf = malloc(buf_bytes);
            err = s.voice_buf == NULL ? ESP_ERR_NO_MEM : ESP_OK;
        }
#if LG_AUDIO_DAC_BACKEND
        if (err == ESP_OK && !is_codec()) {
            err = dac_voice_open();
        }
#endif
        if (err == ESP_OK) {
            s.voice_open = true;
            output_apply();
            ESP_LOGI(TAG, "[AUDIO] Voice stream open, volume %s", lg_bsp_audio_volume_name(s.volume));
        }
    }
    lock_give();
    return err;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

#define TALK_KNEE 20000   /* about -4 dBFS: above this, peaks are rounded off */

static uint8_t s_talk = 0xFF;   /* 0xFF: not read from NVS yet */

static uint8_t talk_default(void)
{
    return is_codec() ? (uint8_t)LG_TALK_LOUD : (uint8_t)LG_TALK_LOUDER;
}

uint8_t lg_bsp_audio_talk_boost(void)
{
    if (s_talk == 0xFF) {
        s_talk = talk_default();
        nvs_handle_t h;
        uint8_t stored = 0;
        if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
            if (nvs_get_u8(h, NVS_KEY_TALK, &stored) == ESP_OK && stored < LG_TALK_STEPS) {
                s_talk = stored;
            }
            nvs_close(h);
        }
    }
    return s_talk;
}

esp_err_t lg_bsp_audio_set_talk_boost(uint8_t level)
{
    s_talk = level < LG_TALK_STEPS ? level : (uint8_t)LG_TALK_LOUDER;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, NVS_KEY_TALK, s_talk);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "[AUDIO] Talk loudness +%d dB", s_talk * 6);
    return err;
}

/* The boost, then a soft knee: above TALK_KNEE the curve bends toward full scale and never
 * reaches past it, so a loud word is rounded rather than squared off. */
static void talk_gain(const int16_t *in, int16_t *out, size_t n)
{
    int32_t g = 1 << lg_bsp_audio_talk_boost();
    const int32_t room = 32767 - TALK_KNEE;
    for (size_t i = 0; i < n; i++) {
        int32_t v = (int32_t)in[i] * g;
        int32_t a = v < 0 ? -v : v;
        if (a > TALK_KNEE) {
            int32_t over = a - TALK_KNEE;
            a = TALK_KNEE + (int32_t)((int64_t)over * room / (over + room));
            v = v < 0 ? -a : a;
        }
        out[i] = (int16_t)v;
    }
}

esp_err_t lg_bsp_audio_voice_write(const int16_t *pcm, size_t samples, uint32_t timeout_ms)
{
#if LG_AUDIO_BACKEND
    if (pcm == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s.voice_open) {
        return ESP_ERR_INVALID_STATE;   /* never opened, stopped, or cut by an urgent cue */
    }
    TickType_t start = xTaskGetTickCount();
    size_t done = 0;
    while (done < samples) {
        size_t n = samples - done > VOICE_CHUNK ? VOICE_CHUNK : samples - done;
        if (!lock_take(ms_left(start, timeout_ms))) {
            return ESP_ERR_TIMEOUT;
        }
        if (!s.voice_open) {
            lock_give();
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = ESP_ERR_NOT_SUPPORTED;
        int16_t boosted[VOICE_CHUNK];
        talk_gain(pcm + done, boosted, n);
#if LG_AUDIO_CODEC_BACKEND
        if (is_codec()) {
            err = codec_voice_chunk(boosted, n, voice_shift(), ms_left(start, timeout_ms));
        }
#endif
#if LG_AUDIO_DAC_BACKEND
        if (!is_codec()) {
            err = dac_voice_chunk(boosted, n, voice_shift(), ms_left(start, timeout_ms));
        }
#endif
        lock_give();
        if (err != ESP_OK) {
            return err;
        }
        done += n;
    }
    return ESP_OK;
#else
    (void)pcm;
    (void)samples;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

void lg_bsp_audio_voice_stop(void)
{
#if LG_AUDIO_BACKEND
    if (s.lock == NULL || !lock_take(LOCK_FOREVER)) {
        return;
    }
    bool was_open = s.voice_open;
#if LG_AUDIO_DAC_BACKEND
    if (!is_codec()) {
        dac_voice_close(true);   /* lets the last words play before the cosine gets the channel */
    }
#endif
    s.voice_open = false;
    free(s.voice_buf);
    s.voice_buf = NULL;
    if (was_open) {
        /* Keep the output up for a cue's tail: on the codec the last 120 ms are still in the DMA. */
        s.out_on = true;
        s.last_cue = xTaskGetTickCount();
    }
    output_apply();
    lock_give();
    if (was_open) {
        ESP_LOGI(TAG, "[AUDIO] Voice stream closed");
        wake_task();   /* held cues play now, and the tail is timed from here */
    }
#endif
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
