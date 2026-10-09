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
#include <stdlib.h>
#include <string.h>

#include "codec2.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
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
#define TASK_STACK      3584     /* room for the voice request staged on the sender's stack (~220 B) */
#define TASK_PRIORITY   4        /* below the network task (5), above drawing (3) and cues (2) */

typedef struct {
    uint32_t author;
    uint32_t boot;
    uint8_t  scope;
    uint32_t target;
    uint16_t len;
    uint8_t  frame[HH_VOICE_FRAME_MAX];
} rx_frame_t;

/*
 * The tasks' working buffers are allocated with the tasks, not kept as statics, so a board that
 * can neither talk nor listen (the alert unit, D66) carries none of them: about 8 KB of RAM.
 */
typedef struct {
    int16_t pcm[FRAME_SAMPLES];
    uint8_t out[HH_VOICE_FRAME_MAX];
} capture_buf_t;

typedef struct {
    rx_frame_t f;                            /* the frame being played */
    rx_frame_t item;                         /* the service task's copy on its way into the queue */
    int16_t    pcm[FRAME_SAMPLES];
    int16_t    silence[FRAME_SAMPLES / 2u];  /* 50 ms of zeros: the lead-in, and the fill for a late frame */
} playback_buf_t;

static playback_buf_t *s_play;

static struct {
    portMUX_TYPE     mux;
    hh_voice_state_t st;          /* guarded by mux */
    volatile bool    talk_req;    /* the talk bar is held */
    TaskHandle_t     capture;
    QueueHandle_t    rx;
    uint16_t         talk_id;
    uint32_t         rx_dropped;
    volatile bool    talk_synth;    /* the frames come from synth() rather than the microphone */
    volatile bool    talk_clip;     /* the frames come from a clip given over the cable (D112) */
    volatile uint32_t talk_limit_ms;/* the talk stops itself after this long; 0 = until released */
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
    if (v.rx == NULL || s_play == NULL || len > HH_VOICE_FRAME_MAX) {
        return;
    }
    if (v.talk_req && (len == 0 || frame[0] != LG_VOICE_CODEC_C2_1200)) {
        return;   /* half duplex for LIVE sound only: a note packet is bytes to store, not sound to
                     play, and its radio copy was already acknowledged - dropped here it was gone */
    }
    rx_frame_t *item = &s_play->item;   /* the service task is the only caller */
    item->author = author;
    item->boot = boot;
    item->scope = scope;
    item->target = target;
    item->len = (uint16_t)len;
    memcpy(item->frame, frame, len);
    if (xQueueSend(v.rx, item, 0) != pdTRUE) {
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

/*
 * A generated voice, for testing and for the grid demonstrating itself.
 *
 * Only two of the handhelds on the bench have a microphone, and a test that needs one can never cover
 * the other four, nor say the same thing twice. This fills a frame with a **voiced buzz in syllables**
 * instead: a fundamental around 110 Hz with its second and third harmonics, switched on and off about
 * three and a half times a second with short attack and release, and the pitch stepping between four
 * values so successive syllables differ. It is unmistakably artificial and unmistakably speech-shaped.
 *
 * Speech-shaped is the point, not decoration. The frames go out as IMA ADPCM, which is a waveform
 * coder, so anything survives it - but the AGC, the noise gate and the listener's gate are all tuned to
 * speech, and a flat tone would sail through gates that real talk has to open. A buzz in syllables
 * exercises them the way a voice does.
 *
 * It is a pure function of a sample counter, so the same seconds produce the same samples on every
 * board and on every run: what arrives can be compared with what was sent.
 */
static const int8_t SINE[32] = {
      0,  24,  48,  70,  90, 106, 118, 125, 127, 125, 118, 106,  90,  70,  48,  24,
      0, -24, -48, -70, -90,-106,-118,-125,-127,-125,-118,-106, -90, -70, -48, -24,
};

#define SYNTH_HZ        3                /* syllables a second, near enough */
#define SYNTH_SYL       (8000u / 4u)     /* samples in one syllable slot: 250 ms */
#define SYNTH_ON        1600u            /* of which this many carry sound: 200 ms */
#define SYNTH_EDGE      160u             /* 20 ms of attack and of release, so nothing clicks */
#define SYNTH_PEAK      7000             /* well inside the ADPCM range, with headroom for the sum */

static uint32_t s_synth_n;               /* samples generated since this talk started */

static inline int32_t synth_sine(uint32_t phase)
{
    return SINE[(phase >> 16) & 31u];
}

static void synth(int16_t *pcm, size_t n)
{
    static const uint16_t F0[4] = { 104, 117, 98, 126 };   /* Hz: the pitch steps between syllables */
    for (size_t i = 0; i < n; i++, s_synth_n++) {
        uint32_t pos = s_synth_n % SYNTH_SYL;
        uint32_t syl = s_synth_n / SYNTH_SYL;
        int32_t env = 0;
        if (pos < SYNTH_ON) {
            env = 256;
            if (pos < SYNTH_EDGE) {
                env = (int32_t)(pos * 256u / SYNTH_EDGE);
            } else if (pos >= SYNTH_ON - SYNTH_EDGE) {
                env = (int32_t)((SYNTH_ON - pos) * 256u / SYNTH_EDGE);
            }
        }
        if (env == 0) {
            pcm[i] = 0;
            continue;
        }
        /*
         * The phase is computed from the sample index rather than accumulated, so a dropped frame
         * never shifts what follows it: sample k always has the same value.
         */
        uint32_t f0 = F0[syl & 3u];
        uint32_t step = (f0 * 32u * 65536u) / 8000u;   /* table entries per sample, 16.16 */
        uint32_t ph = (uint32_t)(s_synth_n % (8000u * 4u)) * step;
        int32_t s = synth_sine(ph) * 3 + synth_sine(ph * 2u) * 2 + synth_sine(ph * 3u);
        int32_t vq = (s * SYNTH_PEAK) / (127 * 6);     /* the three weights sum to 6 */
        pcm[i] = (int16_t)((vq * env) / 256);
    }
}

/* ---- the compact copy of a talk: Codec2, for LoRa (D72, D78, D95) ----
 *
 * Every talk is also encoded with Codec2 at 1200 bit/s and sent beside the live frames, under the same
 * talk id, in packets of LG_VOICE_C2_FRAMES codec frames (1.12 s, 168 bytes). The live frames need
 * Wi-Fi; these are small enough for LoRa, so a listener who can only be reached that way - or a talker
 * who has only that - still gets the talk, late and in one piece. A listener who heard the live frames
 * ignores these. Only the first C2_PACKETS_MAX packets of a talk are sent, about ten seconds, which is
 * what D78 allows on a channel that carries a second of speech in a little more than a second.
 *
 * Nothing is kept: the packets are collected until the last one arrives, played once, and freed.
 *
 * Codec2 wants a deep stack and some heap, so each direction gets a task that exists only while there
 * is work for it: one for the length of a talk on the talker, one from the first packet of a note to
 * the end of its playback on a listener. A handheld that is doing neither carries none of it.
 */
#define C2_SAMPLES        320u      /* 40 ms at 8 kHz: one Codec2 1200 frame in */
#define C2_PACKETS_MAX    9u        /* 9 x 1.12 s: the ten seconds of D78 */
#define C2_ENC_STACK      20480
#define C2_DEC_STACK      24576     /* 14 KB overflowed on the first note played (2026-10-04); about 18 KB is used */
#define C2_PRIORITY       3         /* below capture and playback (4): live talk is never held up by this */
#define C2_PCM_BUFFER     (FRAME_SAMPLES * 5u * sizeof(int16_t))   /* half a second of samples waiting */
/*
 * Between packets. A packet is three 120-character parts since 2026-10-08, and the radios leave
 * 1.5 s of quiet air after each transmission, so one packet is about 8 s of airtime with its
 * acknowledgement; packets offered faster than that piled up in an AP's four-slot queue and the
 * older ones were evicted - a talk arrived 1 packet in 3. Late and in one piece is the deal (D95).
 */
#define C2_SPACING_MS     9000
/*
 * A talker with no Wi-Fi session pays more channel per packet: its packet is broadcast with no
 * acknowledgement, so it rides as one copy plus its parity part (2026-10-09) - four paced parts,
 * about 9 s of channel. Spaced for what is actually queued, or its own four-slot queue fills and
 * evicts the very redundancy it is paying for, which is how a talk once arrived 1 packet in 3.
 */
#define C2_SPACING_LORA_MS 12000
#define NOTE_STRAGGLE_MS  3000      /* after the last packet has come, how long to wait for a missing one */
#define NOTE_GIVE_UP_MS   16000     /* no packet for this long: play what there is. A second over the widest
                                       spacing above, so a note still flowing is never cut into fragments */
#define NOTE_BUSY_WAIT_MS 70000     /* a live talk may legally hold the speaker MAX_TALK_MS (60 s): a finished
                                       note outwaits it rather than being freed unheard */
/*
 * The owner's replay (2026-10-08): a partial note stays open, and when the rest lands it is played
 * once more, whole. Packets cross about nine seconds apart, and a lost one is resent behind the
 * others, so the wait is paced by the air, not by a clock: the note keeps waiting while packets are
 * still arriving, and gives up only after this much quiet. A first wait of 25 s expired with the
 * rest still on the way - a note played one packet of three and never got its replay.
 */
#define NOTE_REPLAY_QUIET_MS 45000  /* no packet for this long: what is missing is not coming */
#define NOTE_REPLAY_CAP_MS  180000  /* however it goes, a note lets the handheld go inside three minutes */

static struct {
    TaskHandle_t          task;      /* NULL when no talk is being encoded */
    StreamBufferHandle_t  pcm;
    volatile bool         ending;
    volatile bool         abandon;   /* a newer talk wants the encoder: stop sending stale speech */
    uint16_t              talk;
    uint8_t               scope;
    uint32_t              target;
} c2tx;

typedef struct {
    uint32_t author;
    uint32_t boot;
    uint8_t  scope;
    uint32_t target;
    uint16_t talk;
    volatile int16_t  last;          /* index of the packet that carried END, -1 until it has come */
    volatile uint32_t heard_ms;      /* when the newest packet came */
    volatile bool     closed;        /* playback has begun: later packets are too late */
    volatile bool     yield;         /* a newer talker is waiting for the slot: wind the waits up */
    bool     have[C2_PACKETS_MAX];
    uint8_t  len[C2_PACKETS_MAX];
    uint8_t  data[C2_PACKETS_MAX][LG_VOICE_C2_DATA_MAX];
} c2_note_t;

static c2_note_t *volatile c2rx;     /* the note being collected or played; NULL when none */

/* A newer talker's first packet, caught while the one note slot was still held: its radio copy
 * was acknowledged, so this is the only copy there will ever be. It seeds the next note. */
static struct {
    bool       valid;
    rx_frame_t f;
} c2_hold;

static void c2_send(uint16_t index, uint8_t flags, uint8_t *pkt, size_t data)
{
    lg_voice_hdr_t h = { .codec = LG_VOICE_CODEC_C2_1200, .flags = flags, .talk = c2tx.talk, .frame = index };
    (void)lg_voice_hdr_enc(&h, pkt);
    for (int tries = 0; tries < 25; tries++) {   /* the service's queue is four deep and drains every pass */
        if (hh_service_voice_send(c2tx.scope, c2tx.target, pkt, LG_VOICE_HDR_LEN + data) != ESP_ERR_NO_MEM) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    /* Said out loud, because the sender's "Note sent" would otherwise count it as sent. */
    ESP_LOGW(TAG, "[MSG] Note packet %u found no room in half a second and was not sent", index);
}

/*
 * Encodes the talk as it is spoken and sends the packets C2_SPACING_MS apart.
 *
 * Spaced, because the frame is two LoRa parts of about 0.65 s each and every radio on the way has a
 * queue of four: sent as fast as they were spoken (one every 1.12 s) the first test on the bench lost
 * two packets of five to a full queue, on a channel that was otherwise idle. At this spacing nothing
 * waits behind anything, on the talker's radio or on an AP relaying to a listener's. The talk therefore
 * finishes arriving after it has finished being said - which over LoRa it always did.
 */
typedef struct {
    uint8_t len;
    uint8_t body[LG_VOICE_C2_PAYLOAD_MAX];   /* header room first, then the codec frames */
} c2_packet_t;

static void c2_encode_task(void *arg)
{
    (void)arg;
    struct CODEC2 *c = codec2_create(CODEC2_MODE_1200);
    int16_t *in = malloc(C2_SAMPLES * sizeof(int16_t));
    c2_packet_t *pk = calloc(C2_PACKETS_MAX, sizeof(c2_packet_t));
    size_t have = 0, bytes = 0;
    uint16_t made = 0, sent = 0;   /* packets filled, packets sent */
    uint32_t crc = 0, worst_ms = 0, next_ms = 0;
    bool sent_end = false;
    bool encoding = c != NULL && in != NULL && pk != NULL;
    bool ok = encoding;
    while (ok && (encoding || sent < made) && !c2tx.abandon) {
        if (encoding) {
            size_t got = xStreamBufferReceive(c2tx.pcm, (uint8_t *)in + have * sizeof(int16_t),
                                              (C2_SAMPLES - have) * sizeof(int16_t), pdMS_TO_TICKS(60));
            have += got / sizeof(int16_t);
            bool drained = c2tx.ending && xStreamBufferIsEmpty(c2tx.pcm);
            if ((have == C2_SAMPLES || (drained && have > 0)) && made <= C2_PACKETS_MAX) {
                uint16_t at = made > 0 && pk[made - 1u].len < LG_VOICE_C2_DATA_MAX ? (uint16_t)(made - 1u) : made;
                if (at < C2_PACKETS_MAX) {
                    memset(in + have, 0, (C2_SAMPLES - have) * sizeof(int16_t));   /* the last, partial 40 ms */
                    uint8_t *to = pk[at].body + LG_VOICE_HDR_LEN + pk[at].len;
                    uint32_t t0 = now_ms();
                    codec2_encode(c, to, in);
                    uint32_t took = now_ms() - t0;
                    worst_ms = took > worst_ms ? took : worst_ms;
                    crc = esp_rom_crc32_le(crc, to, LG_VOICE_C2_FRAME_LEN);
                    pk[at].len = (uint8_t)(pk[at].len + LG_VOICE_C2_FRAME_LEN);
                    bytes += LG_VOICE_C2_FRAME_LEN;
                    made = (uint16_t)(at + 1u);
                }
                have = 0;   /* past the ten seconds the samples are simply let go: that part is live only */
            }
            if (drained) {
                encoding = false;
                vStreamBufferDelete(c2tx.pcm);   /* the capture task set `ending` and feeds no more */
                c2tx.pcm = NULL;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        /* A packet goes once it is full, or once the talk is over, and never sooner than the spacing. */
        bool whole = sent < made && (pk[sent].len == LG_VOICE_C2_DATA_MAX || !encoding);
        if (whole && (int32_t)(now_ms() - next_ms) >= 0) {
            bool last = (!encoding && sent + 1u == made) || sent + 1u == C2_PACKETS_MAX;
            c2_send(sent, last ? LG_VOICE_END : 0, pk[sent].body, pk[sent].len);
            sent_end = last;
            sent++;
            /* With no Wi-Fi session each packet goes twice, so the spacing covers both copies. */
            next_ms = now_ms() + (hh_service_wifi_session() ? C2_SPACING_MS : C2_SPACING_LORA_MS);
        }
    }
    if (ok && sent > 0 && !sent_end && sent < C2_PACKETS_MAX && !c2tx.abandon) {
        /* Released exactly on a packet boundary, after that packet had gone: an empty last one says so. */
        while ((int32_t)(now_ms() - next_ms) < 0 && !c2tx.abandon) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        if (!c2tx.abandon) {
            memset(pk[sent].body, 0, sizeof(pk[sent].body));
            c2_send(sent, LG_VOICE_END, pk[sent].body, 0);
            sent++;
        }
    }
    if (ok && c2tx.pcm != NULL) {
        /* Abandoned before the stream drained; the talk has ended, so nothing feeds it again. */
        vStreamBufferDelete(c2tx.pcm);
        c2tx.pcm = NULL;
    }
    /* The sender's half of the measurement, as "Talk ended" is for the live frames. */
    if (ok) {
        ESP_LOGI(TAG, "[MSG] Note sent: talk %u, %u packet(s), %u bytes, crc %08" PRIx32
                 ", slowest encode %" PRIu32 " ms, stack left %u%s",
                 c2tx.talk, sent, (unsigned)bytes, crc, worst_ms, (unsigned)uxTaskGetStackHighWaterMark(NULL),
                 c2tx.abandon ? ", let go for a newer talk" : "");
    } else {
        ESP_LOGW(TAG, "[MSG] No memory for the compact copy of this talk; it went out live only");
        while (!c2tx.ending) {
            vTaskDelay(pdMS_TO_TICKS(20));   /* the capture task still holds the buffer */
            (void)xStreamBufferReset(c2tx.pcm);
        }
        vStreamBufferDelete(c2tx.pcm);
        c2tx.pcm = NULL;
    }
    if (c != NULL) {
        codec2_destroy(c);
    }
    free(in);
    free(pk);
    c2tx.task = NULL;
    vTaskDelete(NULL);
}

/* Capture task: a talk is starting. The last talk's copy may still be pacing its sends out -
 * up to ~80 s of spaced packets - and quietly skipping this talk meant a retried talk never
 * had a compact copy at all. The new talk outranks stale speech: the old copy is told to let
 * go, and this one takes the encoder when it does. */
static void c2_talk_begin(uint16_t talk, uint8_t scope, uint32_t target)
{
    if (c2tx.task != NULL) {
        c2tx.abandon = true;
        for (int i = 0; i < 40 && c2tx.task != NULL; i++) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (c2tx.task != NULL) {
            return;   /* it would not let go; this talk goes out live only, as before */
        }
    }
    c2tx.abandon = false;
    c2tx.pcm = xStreamBufferCreate(C2_PCM_BUFFER, C2_SAMPLES * sizeof(int16_t));
    if (c2tx.pcm == NULL) {
        return;
    }
    c2tx.talk = talk;
    c2tx.scope = scope;
    c2tx.target = target;
    c2tx.ending = false;
    if (xTaskCreate(c2_encode_task, "hh_c2enc", C2_ENC_STACK, NULL, C2_PRIORITY, &c2tx.task) != pdPASS) {
        vStreamBufferDelete(c2tx.pcm);
        c2tx.pcm = NULL;
        c2tx.task = NULL;
    }
}

/* Capture task: 100 ms of the talk, after the gain. Never waits: what does not fit is left out. */
static void c2_talk_feed(uint16_t talk, const int16_t *pcm, size_t samples)
{
    if (c2tx.task != NULL && c2tx.pcm != NULL && c2tx.talk == talk && !c2tx.ending) {
        (void)xStreamBufferSend(c2tx.pcm, pcm, samples * sizeof(int16_t), 0);
    }
}

static void c2_talk_end(uint16_t talk)
{
    if (c2tx.task != NULL && c2tx.talk == talk) {
        c2tx.ending = true;
    }
}

static void c2_play_task(void *arg)
{
    c2_note_t *n = arg;
    /* Wait for the note to be whole, or for it to be clear that it will not be. */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        uint32_t quiet = now_ms() - n->heard_ms;
        if (n->last >= 0) {
            bool whole = true;
            for (int i = 0; i <= n->last; i++) {
                whole = whole && n->have[i];
            }
            if (whole || quiet >= NOTE_STRAGGLE_MS) {
                break;
            }
        } else if (quiet >= NOTE_GIVE_UP_MS) {
            break;
        }
    }
    bool replayed = false;
    uint16_t played_any = 0;   /* which packets some pass has actually played (C2_PACKETS_MAX <= 16) */
    for (;;) {
        n->closed = true;
        int top = n->last >= 0 ? n->last : (int)C2_PACKETS_MAX - 1;
        while (top >= 0 && !n->have[top]) {
            top--;
        }
        unsigned packets = 0, missing = 0, bytes = 0;
        uint32_t crc = 0;
        for (int i = 0; i <= top; i++) {
            if (n->have[i]) {
                packets++;
                bytes += n->len[i];
                crc = esp_rom_crc32_le(crc, n->data[i], n->len[i]);
            } else {
                missing++;
            }
        }
        /* Half duplex, and one talker at a time: a note waits for a talk in either direction to finish. */
        bool free_to_play = false;
        for (uint32_t waited = 0; waited < NOTE_BUSY_WAIT_MS && bytes > 0; waited += 100) {
            taskENTER_CRITICAL(&v.mux);
            free_to_play = !v.st.talking && v.st.heard == 0 && !v.talk_req;
            if (free_to_play) {
                v.st.heard = n->author;
                v.st.heard_scope = n->scope;
                v.st.heard_target = n->target;
                v.st.version++;
            }
            taskEXIT_CRITICAL(&v.mux);
            if (free_to_play) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        struct CODEC2 *c = free_to_play ? codec2_create(CODEC2_MODE_1200) : NULL;
        int16_t *pcm = free_to_play ? malloc(C2_SAMPLES * sizeof(int16_t)) : NULL;
        uint32_t worst_ms = 0;
        if (c != NULL && pcm != NULL) {
            ESP_LOGI(TAG, "[MSG] Hearing device %" PRIu32 " (%s %" PRIu32 "), a note%s", n->author,
                     n->scope == LG_SCOPE_GROUP ? "group" : n->scope == LG_SCOPE_BROADCAST ? "everyone" : "1:1",
                     n->target, replayed ? ", whole this time" : "");
            lg_bsp_audio_cue(LG_CUE_TALK);
            vTaskDelay(pdMS_TO_TICKS(PREBUFFER_MS));
            if (lg_bsp_audio_voice_start() == ESP_OK) {
                for (int i = 0; i <= top && !v.talk_req; i++) {
                    for (unsigned off = 0; n->have[i] && off + LG_VOICE_C2_FRAME_LEN <= n->len[i];
                         off += LG_VOICE_C2_FRAME_LEN) {
                        uint32_t t0 = now_ms();
                        codec2_decode(c, pcm, n->data[i] + off);
                        uint32_t took = now_ms() - t0;
                        worst_ms = took > worst_ms ? took : worst_ms;
                        (void)lg_bsp_audio_voice_write(pcm, C2_SAMPLES, SPEAKER_WAIT_MS);
                    }
                    if (n->have[i]) {
                        played_any |= (uint16_t)(1u << i);
                    }
                }
                lg_bsp_audio_voice_stop();
            } else {
                ESP_LOGW(TAG, "[MSG] The speaker would not open for a note");
            }
        } else if (bytes > 0) {
            ESP_LOGW(TAG, "[MSG] A note from device %" PRIu32 " was not played: %s", n->author,
                     free_to_play ? "no memory for the decoder" : "this handheld stayed busy");
        }
        if (c != NULL) {
            codec2_destroy(c);
        }
        free(pcm);
        if (free_to_play) {
            set_heard(0, 0, 0);
        }
        /* The listener's half of the measurement: the same crc as the sender's "Note sent" means every byte. */
        ESP_LOGI(TAG, "[MSG] Heard note %u from %" PRIu32 ": %u packet(s), %u missing, %u bytes, crc %08" PRIx32
                 ", slowest decode %" PRIu32 " ms, stack left %u%s%s",
                 n->talk, n->author, packets, missing, bytes, crc, worst_ms,
                 (unsigned)uxTaskGetStackHighWaterMark(NULL), n->last >= 0 ? "" : ", no last packet",
                 replayed ? ", replayed whole after the rest arrived" : "");
        /*
         * The owner's replay (2026-10-08): what came played as it came, but packets missing when the
         * note went quiet can still arrive - the AP sends an unacknowledged one again, and they are
         * accepted into empty slots even while a pass plays. The decision is therefore made from the
         * LIVE state, never from the counts the pass started with: a packet stored mid-play that the
         * pass walked past must still be heard, and a note that became whole mid-play replays now.
         */
        int live_top = n->last >= 0 ? n->last : (int)C2_PACKETS_MAX - 1;
        while (live_top >= 0 && !n->have[live_top]) {
            live_top--;
        }
        unsigned live_missing = 0, unplayed = 0;
        for (int i = 0; i <= live_top; i++) {
            if (!n->have[i]) {
                live_missing++;
            } else if ((played_any & (uint16_t)(1u << i)) == 0u) {
                unplayed++;
            }
        }
        if (!replayed && !n->yield && live_top >= 0 &&
            (unplayed > 0 || live_missing > 0 || n->last < 0)) {
            if (n->last >= 0 && live_missing == 0) {
                replayed = true;   /* whole already - the rest landed during the pass */
                continue;
            }
            n->closed = false;
            bool whole = false;
            for (uint32_t waited = 0; waited < NOTE_REPLAY_CAP_MS && !whole; waited += 200) {
                vTaskDelay(pdMS_TO_TICKS(200));
                if (n->yield || now_ms() - n->heard_ms >= NOTE_REPLAY_QUIET_MS) {
                    break;   /* quiet too long - what is missing is not coming - or a newer talker waits */
                }
                if (n->last >= 0) {
                    whole = true;
                    for (int i = 0; i <= n->last; i++) {
                        whole = whole && n->have[i];
                    }
                }
            }
            n->closed = true;
            if (whole) {
                replayed = true;
                continue;
            }
        }
        break;
    }
    c2rx = NULL;
    free(n);
    vTaskDelete(NULL);
}

/* Stores one packet into the note; the caller has checked identity and the frame index. */
static void c2_store(c2_note_t *n, const rx_frame_t *f, const lg_voice_hdr_t *h)
{
    size_t data = f->len - LG_VOICE_HDR_LEN;
    memcpy(n->data[h->frame], f->frame + LG_VOICE_HDR_LEN, data);
    n->len[h->frame] = (uint8_t)data;
    n->have[h->frame] = data > 0;
    n->heard_ms = now_ms();
    if (h->flags & LG_VOICE_END) {
        n->last = (int16_t)h->frame;
    }
}

/* Playback task: a compact packet of a talk whose live frames this handheld did not hear. */
static void c2_on_packet(const rx_frame_t *f, const lg_voice_hdr_t *h)
{
    if (h->frame >= C2_PACKETS_MAX) {
        return;
    }
    c2_note_t *n = c2rx;
    if (n == NULL) {
        n = calloc(1, sizeof(*n));
        if (n == NULL) {
            return;
        }
        n->author = f->author;
        n->boot = f->boot;
        n->scope = f->scope;
        n->target = f->target;
        n->talk = h->talk;
        n->last = -1;
        n->heard_ms = now_ms();
        c2rx = n;
        if (xTaskCreate(c2_play_task, "hh_c2dec", C2_DEC_STACK, n, C2_PRIORITY, NULL) != pdPASS) {
            c2rx = NULL;
            free(n);
            ESP_LOGW(TAG, "[MSG] No memory to take a note from device %" PRIu32, f->author);
            return;
        }
        if (c2_hold.valid) {
            /* The packet held from when the slot was still taken seeds the note it belongs to. */
            lg_voice_hdr_t held;
            if (lg_voice_hdr_dec(c2_hold.f.frame, c2_hold.f.len, &held) && held.frame < C2_PACKETS_MAX &&
                c2_hold.f.author == f->author && c2_hold.f.boot == f->boot && held.talk == h->talk) {
                c2_store(n, &c2_hold.f, &held);
            }
            c2_hold.valid = false;   /* matched or stale, it is spent either way */
        }
    }
    if (n->author != f->author || n->boot != f->boot || n->talk != h->talk) {
        /*
         * One note at a time, and this packet is another talk's. Its radio copy was already
         * acknowledged, so nothing anywhere will send it again: dropping it silently cost whole
         * notes. If it is newer than the note holding the slot, keep this one copy, ask the open
         * note to wind its waits up, and seed the next note from it once the slot is free.
         */
        bool newer = n->author != f->author || f->boot > n->boot ||
                     (f->boot == n->boot && (int16_t)(h->talk - n->talk) > 0);
        if (newer) {
            c2_hold.f = *f;
            c2_hold.valid = true;
            n->yield = true;
        }
        return;
    }
    if (n->closed && n->have[h->frame]) {
        return;   /* mid-playback a duplicate is too late, but a new packet still fills its own slot:
                     dropping it here lost it for good, and the note could never become whole */
    }
    c2_store(n, f, h);
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

/*
 * D112: a talk from a clip given over the cable. The demo (tools/demo.py) makes a few seconds of speech
 * on its laptop, sends it as IMA ADPCM at 8 kHz, the codec talk already uses, and this handheld talks it
 * out through the ordinary push-to-talk path: paced like a microphone, gained like one, and heard by
 * every listener as speech. It exists so a talk can be demonstrated and tested with real words and
 * nobody's voice. The clip lives in PSRAM where there is some, and is freed with `clip free`.
 */
#define CLIP_MAX_BYTES  (64u * 1024u)    /* 16 s of speech: longer than any demonstration needs */

static uint8_t          *s_clip;
static size_t            s_clip_cap, s_clip_len, s_clip_pos;
static hh_adpcm_state_t  s_clip_dec;

esp_err_t hh_voice_clip_begin(size_t bytes)
{
    if (v.st.talking || v.talk_req) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bytes == 0u || bytes > CLIP_MAX_BYTES) {
        return ESP_ERR_INVALID_SIZE;
    }
    hh_voice_clip_free();
    s_clip = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_clip == NULL) {
        s_clip = malloc(bytes);
    }
    if (s_clip == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_clip_cap = bytes;
    s_clip_len = 0;
    return ESP_OK;
}

esp_err_t hh_voice_clip_add(const uint8_t *data, size_t n, size_t *at)
{
    if (s_clip == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_clip_len + n > s_clip_cap) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_clip + s_clip_len, data, n);
    s_clip_len += n;
    if (at != NULL) {
        *at = s_clip_len;
    }
    return ESP_OK;
}

void hh_voice_clip_free(void)
{
    if (v.talk_clip) {
        return;   /* the talk reading it frees nothing under it; `clip free` after it ends */
    }
    free(s_clip);
    s_clip = NULL;
    s_clip_cap = s_clip_len = s_clip_pos = 0;
}

/* The next frame of the clip, decoded; false once the clip has run out (the rest of the frame is silence). */
static bool clip_frame(int16_t *pcm)
{
    size_t want = FRAME_SAMPLES / 2u;
    size_t have = s_clip_len > s_clip_pos ? s_clip_len - s_clip_pos : 0u;
    size_t take = have < want ? have : want;
    memset(pcm, 0, FRAME_SAMPLES * sizeof(int16_t));
    if (take > 0u) {
        hh_adpcm_decode(&s_clip_dec, s_clip + s_clip_pos, take * 2u, pcm);
        s_clip_pos += take;
    }
    return take == want && s_clip_pos < s_clip_len;
}

static void capture_task(void *arg)
{
    capture_buf_t *buf = arg;
    int16_t *pcm = buf->pcm;
    uint8_t *out = buf->out;
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
        bool clip = v.talk_clip;
        bool synthetic = v.talk_synth || clip;   /* neither needs the microphone */
        uint32_t limit_ms = v.talk_limit_ms;
        if (clip) {
            s_clip_pos = 0;
            s_clip_dec = (hh_adpcm_state_t){ 0 };
        }
        if (!synthetic && lg_bsp_audio_mic_start() != ESP_OK) {
            v.talk_req = false;
            set_talking(false, 0, 0);
            set_problem("The microphone did not start");
            continue;
        }
        s_synth_n = 0;
        uint16_t talk = ++v.talk_id;
        uint16_t frame = 0;
        hh_adpcm_state_t enc = { 0 };
        s_gate = 256;   /* a talk starts open: the first word is never faded in */
        s_quiet = 0;
        uint32_t started = now_ms();
        uint32_t dropped = 0;
        c2_talk_begin(talk, scope, target);   /* the compact copy, for whoever only LoRa reaches (D95) */
        ESP_LOGI(TAG, "[MSG] Talking to %s %" PRIu32 "%s",
                 scope == LG_SCOPE_BROADCAST ? "everyone" : scope == LG_SCOPE_GROUP ? "group" : "device",
                 target, clip ? " (a clip given over the cable)" : synthetic ? " (generated voice)" : "");
        while (v.talk_req) {
            if (synthetic) {
                /*
                 * Paced against the clock rather than by a delay per frame, so a slow frame does not
                 * push the ones behind it: a generated talk sends at the same rate a microphone would,
                 * which is the rate the AP and the listener's jitter buffer are built for. Sending as
                 * fast as the CPU can encode would be a flood test, and would prove nothing about voice.
                 */
                uint32_t due = started + (uint32_t)(frame + 1u) * 100u;
                uint32_t now = now_ms();
                if (due > now) {
                    vTaskDelay(pdMS_TO_TICKS(due - now));
                }
                if (clip) {
                    if (!clip_frame(pcm)) {
                        v.talk_req = false;   /* the clip is spoken: this frame is its last */
                    }
                    agc(pcm, FRAME_SAMPLES);   /* the same loudness a microphone talk gets */
                } else {
                    synth(pcm, FRAME_SAMPLES);
                }
            } else {
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
            }
            c2_talk_feed(talk, pcm, FRAME_SAMPLES);
            size_t hl = pack(&enc, talk, frame, 0, out);
            hh_adpcm_encode(&enc, pcm, FRAME_SAMPLES, out + hl);
            if (hh_service_voice_send(scope, target, out, hl + FRAME_DATA) != ESP_OK) {
                dropped++;
            }
            frame++;
            /*
             * With no Wi-Fi session only the compact copy is going out, and that is ten seconds (D78):
             * a talk held longer would be speech nobody receives.
             */
            if (!hh_service_wifi_session() && now_ms() - started >= C2_PACKETS_MAX * LG_VOICE_C2_FRAMES * 40u) {
                v.talk_req = false;
                set_problem("Over LoRa a talk is ten seconds; press again to go on");
            }
            if (limit_ms != 0 && now_ms() - started >= limit_ms) {
                v.talk_req = false;   /* a talk of an asked-for length: it stops itself */
            }
            if (now_ms() - started >= MAX_TALK_MS) {
                v.talk_req = false;
                set_problem("Talk stops after a minute; press again to go on");
            }
        }
        c2_talk_end(talk);
        size_t hl = pack(&enc, talk, frame, LG_VOICE_END, out);
        (void)hh_service_voice_send(scope, target, out, hl);
        if (!synthetic) {
            lg_bsp_audio_mic_stop();
        }
        v.talk_synth = false;
        v.talk_clip = false;
        v.talk_limit_ms = 0;
        set_talking(false, 0, 0);
        /*
         * The sender's half of the measurement. `talk` identifies the stream, so a listener's count can
         * be matched to this one and the difference is the loss between a microphone and an ear.
         */
        ESP_LOGI(TAG, "[MSG] Talk ended: talk %u, %u frame(s), %" PRIu32 " dropped before the AP, %" PRIu32 " ms",
                 talk, frame, dropped, now_ms() - started);
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
    int16_t *pcm = s_play->pcm;
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
    rx_frame_t *f = &s_play->f;
    bool playing = false;
    uint32_t author = 0;
    uint32_t boot = 0;
    uint32_t heard_ms = 0;
    /*
     * The listener's half of the measurement. The sender reports how many frames it sent under a talk
     * id; this reports how many of them were played under the same id, so the pair is a loss figure for
     * a real voice over a real grid - and for a group or an announcement, one figure per listener.
     * `gaps` counts the frames that never arrived, found from the frame numbers rather than the count,
     * so a stream missing its middle is told apart from one cut short.
     */
    uint16_t played = 0;
    uint16_t gaps = 0;
    uint16_t talk_id = 0;
    uint16_t last_frame = 0;
    bool live_seen = false;   /* author, boot and talk_id name a talk whose live frames were played */
    for (;;) {
        TickType_t wait = playing ? pdMS_TO_TICKS(50) : portMAX_DELAY;   /* 50 ms: the silence below fills it */
        if (xQueueReceive(v.rx, f, wait) != pdTRUE) {
            if (playing && now_ms() - heard_ms < GAP_END_MS) {
                /* A late frame. Silence keeps the output fed: an empty DMA ring replays its old
                 * buffers, which on the Hosyond was a buzz in bursts and words chopped up. */
                (void)lg_bsp_audio_voice_write(s_play->silence, FRAME_SAMPLES / 2u, SPEAKER_WAIT_MS);
            }
            if (playing && now_ms() - heard_ms >= GAP_END_MS) {
                lg_bsp_audio_voice_stop();
                playing = false;
                set_heard(0, 0, 0);
                ESP_LOGI(TAG, "[MSG] Heard talk %u from %" PRIu32 ": %u frame(s), %u missing, no last frame",
                         talk_id, author, played, gaps);
            }
            continue;
        }
        lg_voice_hdr_t h;
        if (!lg_voice_hdr_dec(f->frame, f->len, &h)) {
            continue;
        }
        if (v.talk_req && h.codec != LG_VOICE_CODEC_C2_1200) {
            continue;   /* half duplex drops live sound while this handheld talks, never a note packet */
        }
        if (h.codec == LG_VOICE_CODEC_C2_1200) {
            /*
             * The compact copy of a talk (D95). Whoever heard the live frames has heard the talk, and
             * `talk_id` still names it after its END, which is when the last compact packet arrives.
             * Anyone else was reached only this way, and gets it as a note.
             */
            if (!(live_seen && f->author == author && f->boot == boot && h.talk == talk_id)) {
                c2_on_packet(f, &h);
            }
            continue;
        }
        if (playing && (f->author != author || f->boot != boot)) {
            continue;   /* one talker at a time: the second is heard once the first lets go */
        }
        heard_ms = now_ms();
        if (!playing) {
            if (h.flags & LG_VOICE_END) {
                continue;   /* the tail of a talk we never started playing */
            }
            author = f->author;
            boot = f->boot;
            talk_id = h.talk;
            live_seen = true;
            played = 0;
            gaps = 0;
            last_frame = h.frame;
            s_play_gate = 0;   /* the first word fades in from the silence before it */
            set_heard(author, f->scope, f->target);
            ESP_LOGI(TAG, "[MSG] Hearing device %" PRIu32 " (%s %" PRIu32 ")", author,
                     f->scope == LG_SCOPE_GROUP ? "group"
                     : f->scope == LG_SCOPE_BROADCAST ? "everyone" : "1:1", f->target);
            /*
             * A beep before the voice, the way a push-to-talk radio does it (owner, 2026-10-02).
             * Without it speech simply begins out of a silent handheld, and the first word is spent on
             * the listener working out that the device is talking to them rather than on what was said.
             * It goes out before the speaker is opened for the stream, and PREBUFFER_MS is the gap the
             * frames behind this one are already queueing in - so the alert costs no extra delay.
             */
            lg_bsp_audio_cue(LG_CUE_TALK);
            vTaskDelay(pdMS_TO_TICKS(PREBUFFER_MS));   /* the frames behind this one queue up meanwhile */
            if (lg_bsp_audio_voice_start() != ESP_OK) {
                ESP_LOGW(TAG, "[MSG] The speaker would not open for talk");
            } else {
                (void)lg_bsp_audio_voice_write(s_play->silence, FRAME_SAMPLES / 2u, SPEAKER_WAIT_MS);   /* 50 ms of
                     silence first, so the output starts from clean buffers */
            }
            playing = true;
        }
        if (h.frame > last_frame + 1u) {
            gaps = (uint16_t)(gaps + (h.frame - last_frame - 1u));   /* the ones between did not arrive */
        }
        last_frame = h.frame;
        play(f, &h);
        if (!(h.flags & LG_VOICE_END)) {
            played++;   /* the END frame carries no audio: counting it would beat the sender by one */
        }
        if (h.flags & LG_VOICE_END) {
            lg_bsp_audio_voice_stop();
            playing = false;
            set_heard(0, 0, 0);
            ESP_LOGI(TAG, "[MSG] Heard talk %u from %" PRIu32 ": %u frame(s), %u missing",
                     talk_id, author, played, gaps);
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
        s_play = calloc(1, sizeof(*s_play));
        if (s_play == NULL) {
            return ESP_ERR_NO_MEM;
        }
        v.rx = xQueueCreate(RX_QUEUE_LEN, sizeof(rx_frame_t));
        if (v.rx == NULL || xTaskCreate(playback_task, "hh_play", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
            return ESP_ERR_NO_MEM;
        }
    }
    if (v.st.can_talk) {
        capture_buf_t *buf = calloc(1, sizeof(*buf));   /* the capture task's for good: it never ends */
        if (buf == NULL ||
            xTaskCreate(capture_task, "hh_talk", TASK_STACK, buf, TASK_PRIORITY, &v.capture) != pdPASS) {
            free(buf);
            return ESP_ERR_NO_MEM;
        }
    }
    hh_service_set_voice_io(&s_io);
    ESP_LOGI(TAG, "[MSG] Push-to-talk: %s", v.st.can_talk ? "talk and listen" : v.st.can_hear ? "listen only" : "off");
    return ESP_OK;
}

esp_err_t hh_voice_ptt_start(uint8_t scope, uint32_t target)
{
    if (scope != LG_SCOPE_DIRECT && scope != LG_SCOPE_GROUP && scope != LG_SCOPE_BROADCAST) {
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

/*
 * A talk of a known length from a generated voice.
 *
 * The capture task is created here when the board never had a microphone to create it for. That costs
 * about 5 KB and only on a board asked to do this - the four handhelds with a speaker and no microphone
 * can then originate a talk, which they can never do in use, and which is the only way the group and
 * announcement paths get exercised from more than one sender.
 */
esp_err_t hh_voice_demo_start(uint8_t scope, uint32_t target, uint8_t seconds)
{
    if (seconds == 0u) {
        return ESP_ERR_INVALID_ARG;
    }
    if (v.capture == NULL) {
        capture_buf_t *buf = calloc(1, sizeof(*buf));
        if (buf == NULL ||
            xTaskCreate(capture_task, "hh_talk", TASK_STACK, buf, TASK_PRIORITY, &v.capture) != pdPASS) {
            free(buf);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "[MSG] Capture started for a generated talk on a board with no microphone");
    }
    v.talk_synth = true;
    v.talk_limit_ms = (uint32_t)seconds * 1000u;
    esp_err_t err = hh_voice_ptt_start(scope, target);
    if (err != ESP_OK) {
        v.talk_synth = false;
        v.talk_limit_ms = 0;
    }
    return err;
}

esp_err_t hh_voice_clip_talk(uint8_t scope, uint32_t target)
{
    if (s_clip == NULL || s_clip_len < FRAME_SAMPLES / 2u) {
        return ESP_ERR_INVALID_STATE;
    }
    if (v.capture == NULL) {
        capture_buf_t *buf = calloc(1, sizeof(*buf));
        if (buf == NULL ||
            xTaskCreate(capture_task, "hh_talk", TASK_STACK, buf, TASK_PRIORITY, &v.capture) != pdPASS) {
            free(buf);
            return ESP_ERR_NO_MEM;
        }
    }
    v.talk_clip = true;
    v.talk_limit_ms = 0;   /* it stops when the clip does */
    esp_err_t err = hh_voice_ptt_start(scope, target);
    if (err != ESP_OK) {
        v.talk_clip = false;
    }
    return err;
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
