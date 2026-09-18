/*
 * IMA ADPCM, from the published IMA/DVI description. See hh_adpcm.h.
 */
#include "hh_adpcm.h"

/* How far the step index moves after each code: a small code shrinks the step, a large one
 * grows it. Indexed by the three magnitude bits; the sign bit does not matter. */
static const int8_t INDEX_ADJUST[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

/* The quantiser step sizes, roughly 10% apart, from 7 to 32767. */
static const int16_t STEPS[89] = {
    7,     8,     9,     10,    11,    12,    13,    14,    16,    17,
    19,    21,    23,    25,    28,    31,    34,    37,    41,    45,
    50,    55,    60,    66,    73,    80,    88,    97,    107,   118,
    130,   143,   157,   173,   190,   209,   230,   253,   279,   307,
    337,   371,   408,   449,   494,   544,   598,   658,   724,   796,
    876,   963,   1060,  1166,  1282,  1411,  1552,  1707,  1878,  2066,
    2272,  2499,  2749,  3024,  3327,  3660,  4026,  4428,  4871,  5358,
    5894,  6484,  7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

#define STEP_INDEX_MAX 88

/*
 * Applies one 4-bit code to the state: the reconstruction both ends compute. Returns the new
 * predictor. The difference is step/8 plus step, step/2 and step/4 for each magnitude bit set,
 * which is the integer form of (code + 0.5) * step / 4 the algorithm specifies.
 */
static int16_t apply(hh_adpcm_state_t *st, uint8_t code)
{
    int32_t step = STEPS[st->step_index];
    int32_t diff = step >> 3;
    if (code & 4) {
        diff += step;
    }
    if (code & 2) {
        diff += step >> 1;
    }
    if (code & 1) {
        diff += step >> 2;
    }
    int32_t p = st->predictor + ((code & 8) ? -diff : diff);
    if (p > 32767) {
        p = 32767;
    } else if (p < -32768) {
        p = -32768;
    }
    st->predictor = (int16_t)p;

    int32_t idx = (int32_t)st->step_index + INDEX_ADJUST[code & 7];
    st->step_index = (uint8_t)(idx < 0 ? 0 : idx > STEP_INDEX_MAX ? STEP_INDEX_MAX : idx);
    return st->predictor;
}

/* The code for one sample: the sign, then the difference measured in quarters of the step. */
static uint8_t quantise(hh_adpcm_state_t *st, int16_t sample)
{
    if (st->step_index > STEP_INDEX_MAX) {
        st->step_index = STEP_INDEX_MAX;   /* a corrupt state must not index past the table */
    }
    int32_t step = STEPS[st->step_index];
    int32_t diff = (int32_t)sample - st->predictor;
    uint8_t code = 0;
    if (diff < 0) {
        code = 8;
        diff = -diff;
    }
    if (diff >= step) {
        code |= 4;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 2;
        diff -= step;
    }
    step >>= 1;
    if (diff >= step) {
        code |= 1;
    }
    (void)apply(st, code);   /* the encoder tracks exactly what the decoder will rebuild */
    return code;
}

void hh_adpcm_encode(hh_adpcm_state_t *st, const int16_t *pcm, size_t n, uint8_t *out)
{
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t lo = quantise(st, pcm[i]);
        uint8_t hi = quantise(st, pcm[i + 1]);
        out[i / 2] = (uint8_t)(lo | (hi << 4));
    }
}

void hh_adpcm_decode(hh_adpcm_state_t *st, const uint8_t *in, size_t n, int16_t *pcm)
{
    if (st->step_index > STEP_INDEX_MAX) {
        st->step_index = STEP_INDEX_MAX;
    }
    for (size_t i = 0; i + 1 < n; i += 2) {
        uint8_t b = in[i / 2];
        pcm[i] = apply(st, (uint8_t)(b & 0x0F));
        pcm[i + 1] = apply(st, (uint8_t)(b >> 4));
    }
}
