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
 * whose I2S pins are not yet known, so it stays silent rather than pretending.
 */
#pragma once

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
 * LOW, MEDIUM and HIGH are exactly what the part can do. HIGH is full amplitude.
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

/* Shorthand for "volume is off", which is what a screen calls muted. */
bool lg_bsp_audio_muted(void);

/*
 * Plays one cue. Returns as soon as the cue is queued: the segments are played by the audio
 * task, so a screen or the network service never waits on a speaker.
 */
esp_err_t lg_bsp_audio_cue(lg_cue_t cue);

/*
 * One tone, for bring-up and for the serial console: the frequency in Hz (the cosine
 * generator's floor is about 130 Hz) and how long to hold it. Queued like a cue.
 */
esp_err_t lg_bsp_audio_tone(uint32_t hz, uint32_t ms);

/*
 * Lists the addresses answering on the board's I2C bus, for finding a codec whose pinout is
 * not documented. Returns how many were written to out, or a negative esp_err_t. Uses the bus
 * the touch controller already opened, so it never initialises that bus a second time.
 */
int lg_bsp_i2c_scan(uint8_t *out, size_t max);

#ifdef __cplusplus
}
#endif
