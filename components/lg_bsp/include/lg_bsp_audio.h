/*
 * lg_bsp_audio.h - the sounds a handheld makes, and the driver underneath them.
 *
 * Every sound is SYNTHESISED from a few tone segments at play time. There are no samples in
 * this firmware and there should not be: a second of 16-bit mono is 32 KB, while the whole
 * vocabulary below costs a few dozen bytes of const data, on boards with about 21% of the app
 * partition left. (The same reasoning Braino reached; the approach is shared, not the code.)
 *
 * A named vocabulary rather than beep(frequency, ms) so the handheld has one voice: "sent"
 * sounds the same wherever it is played, and a screen cannot invent its own note. Adding a cue
 * means the device has something new to say, not that an existing cue is nearly right.
 *
 * Pins live in the board profile (D9). On a board with no way to make a sound every call here
 * returns ESP_ERR_NOT_SUPPORTED and does nothing, so callers need no board knowledge: the
 * Hosyond has a DAC on a free pin, while the FNK0104B's speaker is behind an ES8311 codec
 * driven over I2C and I2S, set up at the first sound once the touch driver has opened the bus.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_board.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LG_CUE_SENT = 0,   /* a message left this handheld */
    LG_CUE_RECEIVED,   /* a message arrived */
    LG_CUE_ANNOUNCE,   /* a broadcast everyone should hear: the food is ready */
    LG_CUE_URGENT,     /* an urgent broadcast: the two-tone alert, and it ignores the volume */
    LG_CUE_COUNT,
} lg_cue_t;

/*
 * Claims whatever the board can make a sound with. Safe to call on a board that cannot:
 * it reports ESP_ERR_NOT_SUPPORTED and every later call is a quiet no-op.
 */
esp_err_t lg_bsp_audio_start(const lg_board_t *board);

/* True when this board can actually make a sound, so a screen can hide a volume control. */
bool lg_bsp_audio_available(void);

/*
 * Volume, in the only steps the hardware has.
 *
 * The cosine generator attenuates in four fixed steps (0, -6, -12, -18 dB) and the DAC path
 * has no gain register at all, so a slider pretending to be continuous would be a lie: OFF,
 * LOW, MEDIUM and HIGH are exactly what the part can do. HIGH is full amplitude. The codec
 * path uses the same steps so a cue sounds alike on every board, under a fixed ceiling from
 * the board profile's max_volume.
 *
 * OFF is the mute, and it silences ordinary cues and the console's own tone alike. An URGENT
 * broadcast ignores both the mute and the volume and always plays at full: D6 lets urgent
 * through when nothing else gets out, and a handheld that stays silent for one would be worse
 * than useless. Kept in NVS beside the touch calibration, so it survives a reboot and a
 * reflash. A board that cannot make a sound reports itself off.
 */
typedef enum {
    LG_VOLUME_OFF = 0,
    LG_VOLUME_LOW,
    LG_VOLUME_MEDIUM,
    LG_VOLUME_HIGH,
    LG_VOLUME_STEPS,
} lg_volume_t;

uint8_t lg_bsp_audio_volume(void);
esp_err_t lg_bsp_audio_set_volume(uint8_t level);
const char *lg_bsp_audio_volume_name(uint8_t level);

/*
 * Talk loudness (D61): extra gain on push-to-talk playback only, on top of the volume, because
 * speech from a small microphone arrives far quieter than a synthesised chime. NORMAL adds nothing,
 * LOUD +6 dB, LOUDER +12 dB; peaks above about -4 dBFS are rounded off rather than clipped, so a
 * shout stays speech. Kept in NVS. The default is LOUD on the codec board and LOUDER on the DAC
 * board, whose 8-bit output is the quieter of the two.
 */
typedef enum {
    LG_TALK_NORMAL = 0,
    LG_TALK_LOUD,
    LG_TALK_LOUDER,
    LG_TALK_STEPS,
} lg_talk_boost_t;

uint8_t lg_bsp_audio_talk_boost(void);
esp_err_t lg_bsp_audio_set_talk_boost(uint8_t level);

/* Shorthand for "volume is off", which is what a screen calls muted. */
bool lg_bsp_audio_muted(void);

/*
 * Plays one cue. Returns as soon as the cue is queued: the segments are played by the audio
 * task, so a screen or the network service never waits on a speaker.
 */
esp_err_t lg_bsp_audio_cue(lg_cue_t cue);

/*
 * One tone, for bring-up and for the serial console: the frequency in Hz (the cosine
 * generator's floor is about 130 Hz; the codec path's ceiling is below 8000 Hz, half its
 * sample rate) and how long to hold it. Queued like a cue.
 */
esp_err_t lg_bsp_audio_tone(uint32_t hz, uint32_t ms);

/*
 * Push-to-talk voice: 8 kHz mono 16-bit PCM in from the microphone and out to the speaker.
 *
 * Who can do what. Only a codec board with its ADC wired back (the FNK0104B: ES8311, i2s_din
 * set) can record. Every board with a speaker can play: the codec board through the same I2S
 * port, run full duplex at the cues' 16 kHz (the microphone decimated to 8 kHz through a
 * half-band filter, voice interpolated up to 16 kHz); the DAC board (Hosyond) through the DAC's
 * DMA mode at 8 kHz, 8 bits, which borrows the channel from the cosine generator for as long
 * as a stream is open. The Hosyond is listen-only.
 *
 * Threads. mic_* belong to one capture task and voice_* to one service task; cues stay with
 * the audio task. Nothing here is called from a callback. The calls take the audio mutex for
 * one 16 ms chunk at a time, so they block only on the speaker or microphone themselves (and
 * at most one chunk on each other).
 *
 * Half duplex. Opening the microphone ends an open voice stream (voice_write then returns
 * ESP_ERR_INVALID_STATE), and voice_start is refused with ESP_ERR_INVALID_STATE while the
 * microphone is open. While recording the codec's clock runs but its amplifier is off, so the
 * speaker cannot feed back into the microphone.
 *
 * Cues and voice. While a voice stream or the microphone is open, ordinary cues are held (up
 * to four, played in order once both have closed; more are dropped with a log line) so a
 * notification never talks over someone. LG_CUE_URGENT does not wait: it ends an open voice
 * stream at once (voice_write returns ESP_ERR_INVALID_STATE until voice_start is called again)
 * and plays; over an open microphone it plays too, and the recording will hear it.
 *
 * Volume. Voice follows the same four steps as the cues (HIGH as sent, then -6 dB a step, under
 * the codec board's ceiling). OFF is silence, but the stream is still accepted and paced, so a
 * caller needs no special case for a muted handheld.
 *
 * Buffering. The codec path queues up to 120 ms ahead of the speaker and the DAC path 200 ms,
 * so writing one 100 ms frame into a stream that is keeping up returns without waiting out the
 * frame, and a late frame plays from the slack.
 */

/* True on a board that can record: a codec with a microphone input (i2s_din set). */
bool lg_bsp_audio_can_record(void);

/* Opens the microphone path. ESP_ERR_NOT_SUPPORTED if this board cannot record. About 32 ms of
 * settling audio is read and thrown away before it returns. */
esp_err_t lg_bsp_audio_mic_start(void);

/*
 * Reads up to samples of 8 kHz mono, blocking up to timeout_ms. Returns how many samples were
 * read (fewer only on timeout), or a negated esp_err_t (-ESP_ERR_INVALID_STATE when the
 * microphone is not open): esp_err_to_name(-n) names it.
 */
int lg_bsp_audio_mic_read(int16_t *pcm, size_t samples, uint32_t timeout_ms);

void lg_bsp_audio_mic_stop(void);

/*
 * The microphone's digital gain in 6 dB steps, 0 to 7 (0 to +42 dB), on top of the fixed +30 dB
 * analogue PGA and +4.5 dB ADC volume. The default is 3 (+18 dB): ordinary speech at about
 * 30 cm near -25 dBFS RMS. Not kept across a reboot; it is for tuning with `mic level`.
 */
esp_err_t lg_bsp_audio_mic_gain_set(uint8_t step);
uint8_t lg_bsp_audio_mic_gain(void);

/* Opens an 8 kHz mono PCM stream to the speaker, on codec and DAC boards alike. */
esp_err_t lg_bsp_audio_voice_start(void);

/*
 * Plays samples of 8 kHz mono, blocking until they are queued to the speaker or timeout_ms
 * passes (ESP_ERR_TIMEOUT). ESP_ERR_INVALID_STATE when no stream is open, including after an
 * urgent cue or the microphone ended it.
 */
esp_err_t lg_bsp_audio_voice_write(const int16_t *pcm, size_t samples, uint32_t timeout_ms);

/* Closes the stream. What is already queued still plays (on the DAC board this call waits up
 * to 300 ms for it); held cues play after. Safe to call when no stream is open. */
void lg_bsp_audio_voice_stop(void);

/*
 * Lists the addresses answering on the board's I2C bus, for finding a codec whose pinout is
 * not documented. Returns how many were written to out, or a negative esp_err_t. Uses the bus
 * the touch controller already opened, so it never initialises that bus a second time.
 */
int lg_bsp_i2c_scan(uint8_t *out, size_t max);

#ifdef __cplusplus
}
#endif
