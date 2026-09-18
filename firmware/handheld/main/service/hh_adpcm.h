/*
 * hh_adpcm.h - IMA ADPCM for push-to-talk voice: 16-bit PCM to 4 bits a sample and back.
 *
 * The IMA/DVI algorithm (IMA Recommended Practices for Enhancing Digital Audio Compatibility,
 * 1992), written here from the published description: an 89-entry step table, a 16-entry index
 * adjustment, and a predictor that encoder and decoder update identically. 8 kHz mono becomes
 * 32 kbit/s, so a 100 ms frame of 800 samples is 400 bytes.
 *
 * Portable C with no ESP-IDF, no heap and no tables outside this file. The state is the whole
 * codec: an encoder and the decoder that hears it stay in step only if both start from the same
 * state, so a sender puts its state at the head of every frame (or resets it per frame) and a
 * receiver that lost a frame resynchronises at the next one instead of drifting.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t predictor;    /* the last reconstructed sample */
    uint8_t step_index;   /* 0..88 into the step table */
} hh_adpcm_state_t;

/*
 * Encodes n samples (n even) into n/2 bytes. Each byte carries two samples, the earlier one in
 * the low nibble. The state is advanced, so consecutive calls continue one stream.
 */
void hh_adpcm_encode(hh_adpcm_state_t *st, const int16_t *pcm, size_t n, uint8_t *out);

/* Decodes n samples from n/2 bytes (n even), low nibble first. Advances the state. */
void hh_adpcm_decode(hh_adpcm_state_t *st, const uint8_t *in, size_t n, int16_t *pcm);

#ifdef __cplusplus
}
#endif
