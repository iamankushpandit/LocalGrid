/*
 * hh_voice.h - push-to-talk (D61): the microphone, the speaker, and the frames between them.
 *
 * Live and half duplex, like a walkie-talkie. Holding the talk bar captures 100 ms frames at
 * 8 kHz, packs each as IMA ADPCM (400 bytes), and hands it to the network service, which sends it
 * once to the AP: nothing is stored or retried, so a lost frame is a gap of a tenth of a second.
 * Frames arriving for this handheld are held for a moment so a late one still plays in order,
 * then decoded to the speaker.
 *
 * One talker at a time. While this handheld is talking it plays nothing, and while it is playing
 * someone it will not start talking; a second talker in the same group is not heard until the
 * first lets go. Only boards with a microphone can talk (the FNK0104B); every board with a
 * speaker listens.
 *
 * Layering (D27): this module owns the audio hardware through lg_bsp and reaches the network
 * only through hh_service.h. Screens call it for the talk bar and read its state.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hh_service.h"

typedef struct {
    uint32_t version;          /* changes whenever anything below changes */
    bool     can_talk;         /* this board has a microphone */
    bool     can_hear;         /* this board has a speaker */
    bool     talking;          /* the microphone is open and frames are going out */
    uint8_t  talk_scope;       /* the conversation being talked to */
    uint32_t talk_target;
    uint32_t heard;            /* the device being played now, 0 when nobody */
    uint8_t  heard_scope;      /* its conversation: LG_SCOPE_DIRECT (heard_target = heard) or a group */
    uint32_t heard_target;
    char     problem[HH_PROBLEM_MAX];   /* why the last talk stopped early or never started; empty if fine */
} hh_voice_state_t;

/* Starts the capture and playback tasks. Call once, after hh_service_start. */
esp_err_t hh_voice_start(void);

/*
 * Talks with a generated tone instead of the microphone, for `ms` milliseconds (capped at one
 * minute, as a held talk bar is). Everything after the capture is the ordinary talk path: the
 * same AGC, ADPCM encoding, frame numbering and END frame, sent through the same service call,
 * so what this exercises is push-to-talk itself and not a parallel imitation of it.
 *
 * It exists because only the FNK0104B has a microphone (see can_talk), which left voice
 * undemonstrable on the four boards that can only listen, and untestable without a person
 * willing to talk into a board. A board with no microphone may talk this way; a board with no
 * audio hardware at all still cannot.
 *
 * The capture task is created on the first call on a board that has no microphone, so a handheld
 * that never runs a demo pays nothing for this.
 */
esp_err_t hh_voice_tone_start(uint8_t scope, uint32_t target, uint16_t ms);

/*
 * Starts talking to a 1:1 conversation (LG_SCOPE_DIRECT, target = device) or a group
 * (LG_SCOPE_GROUP, target = group id). ESP_ERR_NOT_SUPPORTED on a board with no microphone,
 * ESP_ERR_INVALID_STATE while someone else is being played or a talk is already running,
 * ESP_ERR_INVALID_ARG for any other scope.
 */
esp_err_t hh_voice_ptt_start(uint8_t scope, uint32_t target);

/* Stops talking: the last frame goes out marked as the end. Safe to call when not talking. */
void hh_voice_ptt_stop(void);

/* Copies the current state; safe from any task. */
void hh_voice_state(hh_voice_state_t *out);
