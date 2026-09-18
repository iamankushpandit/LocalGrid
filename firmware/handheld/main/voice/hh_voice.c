/*
 * Push-to-talk (D61): see hh_voice.h.
 *
 * Two tasks, because both ends of the pipe block on hardware at the audio rate:
 *   capture   while talking, reads 100 ms from the microphone, packs it, and queues it for the
 *             service. Only created on a board that can record.
 *   playback  takes frames the service received, holds the first ones PREBUFFER_MS so a late
 *             frame still plays in order, and writes the decoded audio to the speaker, whose
 *             DMA paces it.
 * The service task only ever copies into the playback queue and out of its own voice queue, so a
 * slow speaker or microphone never holds up the network.
 *
 * Every frame carries the ADPCM state it was encoded from, so each decodes on its own: a lost
 * frame is a tenth of a second of silence, never a burst of noise afterwards.
 */
#include "hh_voice.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hh_adpcm.h"
#include "lg_body.h"
#include "lg_bsp_audio.h"
#include "lg_envelope.h"

static const char *TAG = "VOICE";

#define FRAME_SAMPLES   LG_VOICE_SAMPLES          /* 100 ms at 8 kHz */
#define FRAME_DATA      (FRAME_SAMPLES / 2u)      /* 4 bits a sample */
#define MIC_WAIT_MS     250
#define MAX_TALK_MS     60000    /* a talk bar held down in a pocket stops by itself */
#define PREBUFFER_MS    400      /* the first frames wait this long, so jitter does not break the audio */
#define GAP_END_MS      1500     /* no frame for this long ends what we were playing (the END was lost) */
#define SPEAKER_WAIT_MS 400
#define RX_QUEUE_LEN    12       /* 1.2 s of audio: Wi-Fi delivers in bursts, and a burst must fit */
#define TASK_STACK      3072
#define TASK_PRIORITY   4        /* below the network task (5), above drawing (3) and cues (2) */

typedef struct {
    uint32_t author;
    uint32_t boot;
    uint8_t  scope;
    uint32_t target;
    uint16_t len;
    uint8_t  frame[HH_VOICE_FRAME_MAX];
} rx_frame_t;

static struct {
    portMUX_TYPE     mux;
    hh_voice_state_t st;          /* guarded by mux */
    volatile bool    talk_req;    /* the talk bar is held */
    TaskHandle_t     capture;
    QueueHandle_t    rx;
    uint16_t         talk_id;
    uint32_t         rx_dropped;
} v = { .mux = portMUX_INITIALIZER_UNLOCKED };

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ---- state, for the screens ---- */

static void set_talking(bool on, uint8_t scope, uint32_t target)
{
    taskENTER_CRITICAL(&v.mux);
    v.st.talking = on;
    if (on) {
        v.st.talk_scope = scope;
        v.st.talk_target = target;
    }
    v.st.version++;
    taskEXIT_CRITICAL(&v.mux);
}

static void set_heard(uint32_t device, uint8_t scope, uint32_t target)
{
    taskENTER_CRITICAL(&v.mux);
    v.st.heard = device;
    v.st.heard_scope = scope;
    v.st.heard_target = target;
    v.st.version++;
    taskEXIT_CRITICAL(&v.mux);
}

static void set_problem(const char *text)
{
    taskENTER_CRITICAL(&v.mux);
    snprintf(v.st.problem, sizeof(v.st.problem), "%s", text);
    v.st.version++;
    taskEXIT_CRITICAL(&v.mux);
}

static const char *refusal_text(int reason)
{
    switch (reason) {
    case LG_ACK_REJ_OFFLINE:        return "They are not online";
    case LG_ACK_REJ_NOT_MEMBER:     return "You are not in this group";
    case LG_ACK_REJ_TIME:           return "Grid time is not set, so talk cannot go out";
    case LG_ACK_REJ_UNKNOWN_TARGET: return "The AP does not know them";
    case LG_ERR_TIME:               return "Grid time is not set, so talk cannot go out";
    case LG_ERR_SHORT:              return "Not connected to an AP";
    case LG_ERR_ARG:                return "No key for them yet; send a message first";
    case LG_ERR_FULL:               return "The link to the AP is too slow";
    default:                        return reason > 0 ? "The AP refused talk" : "Talk could not be sent";
    }
}

/* ---- from the service task: copy and return ---- */

static void on_frame(uint32_t author, uint32_t boot, uint8_t scope, uint32_t target, const uint8_t *frame, size_t len)
{
    if (v.rx == NULL || len > HH_VOICE_FRAME_MAX || v.talk_req) {
        return;   /* half duplex: nothing plays while this handheld talks */
    }
    static rx_frame_t item;   /* the service task is the only caller */
    item.author = author;
    item.boot = boot;
    item.scope = scope;
    item.target = target;
    item.len = (uint16_t)len;
    memcpy(item.frame, frame, len);
    if (xQueueSend(v.rx, &item, 0) != pdTRUE) {
        v.rx_dropped++;
    }
}

static void on_refused(int reason)
{
    if (!v.talk_req) {
        return;
    }
    v.talk_req = false;   /* the capture task sends the END frame and closes the microphone */
    set_problem(refusal_text(reason));
    ESP_LOGW(TAG, "[MSG] Talk stopped: %s", refusal_text(reason));
}

static const hh_voice_io_t s_io = { .on_frame = on_frame, .on_refused = on_refused };

/* ---- capture ---- */

/*
 * Automatic gain on the talker's side, so every listener gets speech at one loudness whoever holds
 * the handheld and however far from the mouth. Each frame's RMS is steered toward AGC_TARGET: the
 * gain drops at once when a frame would clip or run loud, and rises slowly (AGC_RISE a frame) when
 * speech is quiet, never above AGC_MAX. Frames below AGC_GATE are room noise and leave the gain
 * alone, so pauses are not pumped up into hiss. Gain is in 1/256 steps.
 */
#define AGC_TARGET  6500     /* about -14 dBFS RMS */
#define AGC_PEAK    30000    /* no sample above this after the gain */
#define AGC_GATE    300      /* about -40 dBFS RMS: quieter than this is not speech */
#define AGC_MAX     (4 * 256)   /* +12 dB: more lifted the board's own noise into the pauses */
#define AGC_MIN     256
#define AGC_RISE    12       /* about 0.4 dB a frame, 4 dB a second */

/*
 * Noise gate. The microphone runs at +48 dB fixed gain, and between words that is the board's own
 * noise: a recording of a talk (owner, 2026-09-17) had a buzz around 1.4-1.5 kHz in 60 ms bursts,
 * only 14 dB under the speech, that started and stopped with the talk -- most likely the radio's
 * transmit bursts coupling into the mic, lifted further by the gain above. A frame quieter than
 * GATE_OPEN (before gain) closes the gate after GATE_HOLD frames, and a closed gate turns the frame
 * down GATE_DEPTH; the level moves in a straight line across each frame, so nothing clicks.
 */
#define GATE_OPEN   300      /* about -40 dBFS RMS before gain: speech opens the gate at once */
#define GATE_HOLD   2        /* frames (200 ms) below it before closing, so word endings are kept */
#define GATE_DEPTH  32       /* closed: 32/256, -18 dB */

static int32_t s_agc = 2 * 256;
static int32_t s_gate = 256;     /* the gate's level at the end of the last frame, in 1/256 */
static uint8_t s_quiet;          /* frames in a row below GATE_OPEN */

static void agc(int16_t *pcm, size_t n)
{
    int64_t sum = 0;
    int32_t peak = 1;
    for (size_t i = 0; i < n; i++) {
        int32_t a = pcm[i] < 0 ? -pcm[i] : pcm[i];
        peak = a > peak ? a : peak;
        sum += (int64_t)pcm[i] * pcm[i];
    }
    int32_t rms = 0;
    for (int64_t mean = sum / (int64_t)n, r = 1 << 15; r > 0; r >>= 1) {
        if ((int64_t)(rms + r) * (rms + r) <= mean) {
            rms += (int32_t)r;   /* integer square root, 16 steps */
        }
    }
    if (rms >= AGC_GATE) {
        int32_t want = (int32_t)((int64_t)AGC_TARGET * 256 / rms);
        if (want < s_agc) {
            s_agc = want;                         /* loud: come down at once */
        } else if (s_agc + AGC_RISE <= want) {
            s_agc += AGC_RISE;                    /* quiet: go up slowly */
        }
    }
    int32_t limit = (int32_t)((int64_t)AGC_PEAK * 256 / peak);
    int32_t g = s_agc < limit ? s_agc : limit;   /* never clip this frame, whatever the average says */
    g = g < AGC_MIN ? AGC_MIN : g > AGC_MAX ? AGC_MAX : g;
    s_quiet = rms >= GATE_OPEN ? 0 : (uint8_t)(s_quiet < 255 ? s_quiet + 1 : 255);
    int32_t gate_to = s_quiet > GATE_HOLD ? GATE_DEPTH : 256;
    int32_t gate_from = s_gate;
    for (size_t i = 0; i < n; i++) {
        int32_t gate = gate_from + (int32_t)((int64_t)(gate_to - gate_from) * (int32_t)i / (int32_t)n);
        int32_t v = (int32_t)(((int64_t)pcm[i] * g * gate) / (256 * 256));
        pcm[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    s_gate = gate_to;
}

static size_t pack(const hh_adpcm_state_t *from, uint16_t talk, uint16_t frame, uint8_t flags, uint8_t *out)
{
    lg_voice_hdr_t h = {
        .codec = LG_VOICE_CODEC_IMA_8K,
        .flags = flags,
        .talk = talk,
        .frame = frame,
        .predictor = from->predictor,
        .step_index = from->step_index,
    };
    return lg_voice_hdr_enc(&h, out);
}

static void capture_task(void *arg)
{
    (void)arg;
    static int16_t pcm[FRAME_SAMPLES];
    static uint8_t out[HH_VOICE_FRAME_MAX];
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!v.talk_req) {
            set_talking(false, 0, 0);   /* pressed and released before this task woke */
            continue;
        }
        hh_voice_state_t st;
        hh_voice_state(&st);
        uint8_t scope = st.talk_scope;
        uint32_t target = st.talk_target;
        if (lg_bsp_audio_mic_start() != ESP_OK) {
            v.talk_req = false;
            set_talking(false, 0, 0);
            set_problem("The microphone did not start");
            continue;
        }
        uint16_t talk = ++v.talk_id;
        uint16_t frame = 0;
        hh_adpcm_state_t enc = { 0 };
        s_gate = 256;   /* a talk starts open: the first word is never faded in */
        s_quiet = 0;
        uint32_t started = now_ms();
        uint32_t dropped = 0;
        ESP_LOGI(TAG, "[MSG] Talking to %s %" PRIu32, scope == LG_SCOPE_GROUP ? "group" : "device", target);
        while (v.talk_req) {
            size_t got = 0;
            while (got < FRAME_SAMPLES && v.talk_req) {
                int n = lg_bsp_audio_mic_read(pcm + got, FRAME_SAMPLES - got, MIC_WAIT_MS);
                if (n < 0) {
                    v.talk_req = false;
                    set_problem("The microphone stopped");
                    break;
                }
                got += (size_t)n;
            }
            if (got < FRAME_SAMPLES) {
                break;   /* released mid-frame: that part is dropped, the END below closes the talk */
            }
            agc(pcm, FRAME_SAMPLES);
            size_t hl = pack(&enc, talk, frame, 0, out);
            hh_adpcm_encode(&enc, pcm, FRAME_SAMPLES, out + hl);
            if (hh_service_voice_send(scope, target, out, hl + FRAME_DATA) != ESP_OK) {
                dropped++;
            }
            frame++;
            if (now_ms() - started >= MAX_TALK_MS) {
                v.talk_req = false;
                set_problem("Talk stops after a minute; press again to go on");
            }
        }
        size_t hl = pack(&enc, talk, frame, LG_VOICE_END, out);
        (void)hh_service_voice_send(scope, target, out, hl);
        lg_bsp_audio_mic_stop();
        set_talking(false, 0, 0);
        ESP_LOGI(TAG, "[MSG] Talk ended: %u frame(s), %" PRIu32 " dropped before the AP, %" PRIu32 " ms", frame,
                 dropped, now_ms() - started);
    }
}

/* ---- playback ---- */

/*
 * The listener's gate. Between words a frame carries only the talker's gated-down noise, and the
 * listener's talk boost lifts it straight back up; on the Hosyond's 8-bit DAC, beside its own radio,
 * that was the "disturbance" the owner still heard once the dropouts were gone. A frame below
 * PLAY_GATE fades to true silence across the frame and the next speech fades back in, so the
 * pauses are quiet and nothing clicks.
 */
#define PLAY_GATE 500   /* RMS, about -36 dBFS: the talker's AGC puts speech near -14 dBFS */

static int32_t s_play_gate = 256;

static void play(const rx_frame_t *f, const lg_voice_hdr_t *h)
{
    static int16_t pcm[FRAME_SAMPLES];
    size_t data = f->len - LG_VOICE_HDR_LEN;
    if (data == 0) {
        return;
    }
    size_t n = data * 2u > FRAME_SAMPLES ? FRAME_SAMPLES : data * 2u;
    hh_adpcm_state_t dec = { .predictor = h->predictor, .step_index = h->step_index };
    hh_adpcm_decode(&dec, f->frame + LG_VOICE_HDR_LEN, n, pcm);
    int64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        sum += (int64_t)pcm[i] * pcm[i];
    }
    static uint8_t quiet;   /* frames in a row below the gate: two are kept, so word endings stay */
    bool loud = sum / (int64_t)n >= (int64_t)PLAY_GATE * PLAY_GATE;
    quiet = loud ? 0 : (uint8_t)(quiet < 255 ? quiet + 1 : 255);
    int32_t to = quiet > 2 ? 0 : 256;
    int32_t from = s_play_gate;
    if (from != 256 || to != 256) {
        for (size_t i = 0; i < n; i++) {
            int32_t g = from + (int32_t)((int64_t)(to - from) * (int32_t)i / (int32_t)n);
            pcm[i] = (int16_t)((pcm[i] * g) / 256);
        }
    }
    s_play_gate = to;
    (void)lg_bsp_audio_voice_write(pcm, n, SPEAKER_WAIT_MS);
}

static void playback_task(void *arg)
{
    (void)arg;
    static rx_frame_t f;
    bool playing = false;
    uint32_t author = 0;
    uint32_t boot = 0;
    uint32_t heard_ms = 0;
    for (;;) {
        TickType_t wait = playing ? pdMS_TO_TICKS(50) : portMAX_DELAY;   /* 50 ms: the silence below fills it */
        if (xQueueReceive(v.rx, &f, wait) != pdTRUE) {
            if (playing && now_ms() - heard_ms < GAP_END_MS) {
                /* A late frame. Silence keeps the output fed: an empty DMA ring replays its old
                 * buffers, which on the Hosyond was a buzz in bursts and words chopped up. */
                static int16_t quiet[FRAME_SAMPLES];
                (void)lg_bsp_audio_voice_write(quiet, FRAME_SAMPLES / 2u, SPEAKER_WAIT_MS);
            }
            if (playing && now_ms() - heard_ms >= GAP_END_MS) {
                lg_bsp_audio_voice_stop();
                playing = false;
                set_heard(0, 0, 0);
                ESP_LOGI(TAG, "[MSG] Talk from %" PRIu32 " ended without its last frame", author);
            }
            continue;
        }
        lg_voice_hdr_t h;
        if (!lg_voice_hdr_dec(f.frame, f.len, &h) || v.talk_req) {
            continue;
        }
        if (playing && (f.author != author || f.boot != boot)) {
            continue;   /* one talker at a time: the second is heard once the first lets go */
        }
        heard_ms = now_ms();
        if (!playing) {
            if (h.flags & LG_VOICE_END) {
                continue;   /* the tail of a talk we never started playing */
            }
            author = f.author;
            boot = f.boot;
            s_play_gate = 0;   /* the first word fades in from the silence before it */
            set_heard(author, f.scope, f.target);
            ESP_LOGI(TAG, "[MSG] Hearing device %" PRIu32 " (%s %" PRIu32 ")", author,
                     f.scope == LG_SCOPE_GROUP ? "group" : "1:1", f.target);
            vTaskDelay(pdMS_TO_TICKS(PREBUFFER_MS));   /* the frames behind this one queue up meanwhile */
            if (lg_bsp_audio_voice_start() != ESP_OK) {
                ESP_LOGW(TAG, "[MSG] The speaker would not open for talk");
            } else {
                static int16_t lead[FRAME_SAMPLES / 2u];
                (void)lg_bsp_audio_voice_write(lead, FRAME_SAMPLES / 2u, SPEAKER_WAIT_MS);   /* 50 ms of
                     silence first, so the output starts from clean buffers */
            }
            playing = true;
        }
        play(&f, &h);
        if (h.flags & LG_VOICE_END) {
            lg_bsp_audio_voice_stop();
            playing = false;
            set_heard(0, 0, 0);
            ESP_LOGI(TAG, "[MSG] Talk from %" PRIu32 " ended", author);
        }
    }
}

/* ---- public ---- */

esp_err_t hh_voice_start(void)
{
    taskENTER_CRITICAL(&v.mux);
    v.st.can_talk = lg_bsp_audio_can_record();
    v.st.can_hear = lg_bsp_audio_available();
    v.st.version++;
    taskEXIT_CRITICAL(&v.mux);
    if (v.st.can_hear) {
        v.rx = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_frame_t));
        if (v.rx == NULL || xTaskCreate(playback_task, "hh_play", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (v.st.can_talk &&
        xTaskCreate(capture_task, "hh_talk", TASK_STACK, NULL, TASK_PRIORITY, &v.capture) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    hh_service_set_voice_io(&s_io);
    ESP_LOGI(TAG, "[MSG] Push-to-talk: %s", v.st.can_talk ? "talk and listen" : v.st.can_hear ? "listen only" : "off");
    return ESP_OK;
}

esp_err_t hh_voice_ptt_start(uint8_t scope, uint32_t target)
{
    if (scope != LG_SCOPE_DIRECT && scope != LG_SCOPE_GROUP) {
        return ESP_ERR_INVALID_ARG;
    }
    if (v.capture == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    taskENTER_CRITICAL(&v.mux);
    bool busy = v.st.talking || v.st.heard != 0;
    if (!busy) {
        v.st.talking = true;
        v.st.talk_scope = scope;
        v.st.talk_target = target;
        v.st.problem[0] = '\0';
        v.st.version++;
    }
    taskEXIT_CRITICAL(&v.mux);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    v.talk_req = true;
    xTaskNotifyGive(v.capture);
    return ESP_OK;
}

void hh_voice_ptt_stop(void)
{
    v.talk_req = false;
}

void hh_voice_state(hh_voice_state_t *out)
{
    taskENTER_CRITICAL(&v.mux);
    *out = v.st;
    taskEXIT_CRITICAL(&v.mux);
}
