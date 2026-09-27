/* ClassicVoices: the streaming resampler every engine that is not natively 22050 Hz goes through (the SAPI 4
 * telephone modes at 8 kHz, L&H TruVoice at 11025 Hz, Microsoft Anna at 16 kHz; since 2026-09-27 also the neural voices'
 * 24 kHz, down). The audio unit has one
 * 22050 Hz format for every voice.
 *
 * Ratio L/M in lowest terms (441/160 for 8 kHz, 2/1 for 11025 Hz, 441/320 for 16 kHz), so every output
 * sample falls on one of L fixed phases between input samples: a polyphase windowed-sinc (16 taps, Kaiser
 * beta 7, cutoff 0.47 of the input rate) with one normalised tap set per phase. Streaming: input is consumed
 * as it arrives, 8 samples of look-ahead; cv_rs_run(r, 1) flushes the end. */
#ifndef CV_RESAMPLE_H
#define CV_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int L, M;             /* output/input rate ratio in lowest terms */
    float *taps;          /* L x 16 */
    float *in;            /* input window, in[0] is input sample number `base` */
    size_t nin, cap;
    uint64_t base;        /* absolute index of in[0] */
    uint64_t total_in;    /* input samples received */
    uint64_t next_out;    /* absolute index of the next output sample */
    int16_t *out;         /* what the last cv_rs_run produced */
    size_t ocap;
} cv_resampler;

#define CV_RS_OUT_RATE 22050

/* in_rate: any rate below 22050, or above it up to 4x (the neural voices' 24000 Hz: 147/160, anti-aliased at 0.47 of
 * the output rate). Returns 0, or -1 (unsupported rate / out of memory). */
int cv_rs_init(cv_resampler *r, int in_rate);
void cv_rs_free(cv_resampler *r);
/* start a new stream (keeps the taps and buffers) */
void cv_rs_reset(cv_resampler *r);
/* append input; returns 0, or -1 when out of memory */
int cv_rs_feed(cv_resampler *r, const int16_t *pcm, size_t n);
/* produce every output sample whose taps are available (flush: all of them, the end of the stream) into
 * r->out; returns the count */
size_t cv_rs_run(cv_resampler *r, int flush);

#ifdef __cplusplus
}
#endif
#endif
