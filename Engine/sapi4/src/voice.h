// The synthesizer's view of a voice (the .vce Inventory stream, loaded) and the per-unit data it
// decodes. Offsets are the original's; fields not yet identified are padding.
#pragma once
#include "gptr.h"

typedef struct Codebook {
    int32_t count;          // 0x00 (inferred: number of entries - not read by the code decompiled so far)
    int32_t dim;            // 0x04 floats per entry
    GPTR(const float) data; // 0x08 count * dim floats
} Codebook;

typedef struct VocVoice {
    int32_t _00;
    int32_t noise_len;      // 0x04 length of the noise table (samples)
    int32_t n_lsf;          // 0x08 LSF codebooks
    int32_t n_exc;          // 0x0c excitation codebooks
    int32_t n_dexc;         // 0x10 delta-excitation codebooks
    GPTR(GPTR(const Codebook)) lsf;   // 0x14
    GPTR(GPTR(const Codebook)) exc;   // 0x18
    GPTR(GPTR(const Codebook)) dexc;  // 0x1c
    int32_t _20;
    GPTR(GPTR(const uint8_t)) units;  // 0x24 unit blobs
    int32_t order;          // 0x28 LPC order
    int32_t fft_n;          // 0x2c
    int32_t fft_log2;       // 0x30
    GPTR(const float) sine; // 0x34 sin table for the FFT
    GPTR(const float) window;  // 0x38 (fft_n) cross-fade window for dsp_fit_period
    GPTR(const int8_t) noise;  // 0x3c noise table
    int32_t _40;            // 0x40 set to 0
} VocVoice;

// one decoded unit (malloc'd, 0x20 bytes)
typedef struct VocUnit {
    int32_t nframes;        // 0x00
    int32_t total;          // 0x04 excitation samples (sum of |period|)
    int32_t order;          // 0x08
    GPTR(float) periods;    // 0x0c per frame: > 0 voiced period, <= 0 unvoiced length
    GPTR(float) lpc;        // 0x10 nframes * (order + 1)
    GPTR(float) exc;        // 0x14 total
    GPTR(float) gains;      // 0x18 nframes
    int32_t _1c;            // 0x1c set to 0
} VocUnit;

struct Effect;
// the synthesizer object (C++ in the original)
typedef struct VocSynthState {
    GPTR(const VocVoice) voice;  // 0x00
    float rate;             // 0x04 sample rate
    int32_t order;          // 0x08 LPC order
    int32_t fixed_rate;     // 0x0c 1: pitch marks at rate / pitch (see voc_pitch_marks)
    GPTR(VocUnit) unit;     // 0x10 the unit being synthesized
    GPTR(float) mem;        // 0x14 synthesis filter state [order + 1]
    GPTR(float) out;        // 0x18 output buffer
    int32_t cap;            // 0x1c its size in samples
    int32_t noise_pos;      // 0x20 read position in the voice's noise table
    uint32_t flags;         // 0x24 bit 0 whisper (every frame unvoiced), 1 FIR, 2 IIR, 3 effect
    GPTR(float) fmem;       // 0x28 FIR / IIR state
    GPTR(const float) fcoef;  // 0x2c FIR / IIR coefficients
    int32_t forder;         // 0x30
    GPTR(struct Effect) effect;  // 0x34
    GPTR(float) last_lpc;   // 0x38 the last frame's LPC coefficients [order + 1]
} VocSynthState;

// one synthesis request (a unit with its prosody)
typedef struct VocRequest {
    GPTR(char) name;        // 0x00 the unit's phone name (passed on to voc_pitch_marks, which ignores it)
    int32_t unit;           // 0x04 unit index; 0 = silence
    float dur;              // 0x08 duration scale (silence: seconds)
    int32_t npoints;        // 0x0c prosody contour points
    GPTR(float) times;      // 0x10 [npoints] sample positions
    GPTR(float) pitch;      // 0x14 [npoints] (scaled in place by the unit's length when not fixed_rate)
    GPTR(const float) gains;  // 0x18 [npoints]
    int32_t produced;       // 0x1c samples written to out
} VocRequest;

// @0x636719fa stdcall: decode unit `index`: periods, LSF frames -> LPC, gains, and the excitation -
// voiced periods from codebook spectra (delta-coded against the previous voiced frame) through the
// inverse FFT, unvoiced ones from the noise table. NULL if index < 0, fft_n > 512 or out of memory.
VocUnit *voc_unit_decode(int index, const VocVoice *v, VocSynthState *s);

// @0x63675bc4 stdcall: free a unit and its arrays
void voc_unit_free(VocUnit *u);
// @0x636710da stdcall: place the pitch marks of a unit. For each frame, periods[f] < 0 is an unvoiced
// stretch of -periods[f] samples; otherwise the pitch at the current position is interpolated from the
// contour (times[], values[], npoints) and the frame's period resampled to it (or, with fixed_rate, the
// period rate / pitch). Marks are emitted while they fall inside the frame's time span; per mark:
// len[j] (samples), frame[j] (source frame index), sign[j] (+1, or -1 for every second repeat of an
// unvoiced frame: played reversed). Returns the number of marks.
int voc_pitch_marks(const float *periods, int nframes, float *len, float time_scale, float rate,
                    const float *times, const float *values, int npoints, int fixed_rate, int unused,
                    int32_t *frame, float *sign);
// @0x63671429 stdcall: assemble the excitation: for each pitch mark j, source frame frames[j]'s
// excitation period (at exc + sum of the earlier |periods|) is fitted to lens[j] samples at out+pos
// (dsp_period: reversed when signs[j] is -1), scaled by the gain contour (times[], gains[]) at pos.
void voc_excitation(const float *periods, int nframes, const float *exc, const float *lens, int count,
                    const int32_t *frames, const float *signs, float *out, const float *times,
                    const float *gains, int npoints, const float *window, int winn);
// @0x636715e2 thiscall: synthesize one request into synth->out: decode the unit, place pitch marks,
// assemble the excitation, run the LPC synthesis filter per mark, then the mode's FIR / IIR / effect.
// Returns 1, or 0 if the unit cannot be decoded or the output does not fit.
int voc_synth_unit(VocSynthState *s, VocRequest *req);

// the synthesis thread's object; st is the synthesizer proper
struct Queue;
struct UnitOut;
typedef struct VocEngine {
    int32_t _00;
    int32_t pcm16;          // 0x04 1: 16-bit output (0: 8-bit)
    uint32_t rate;          // 0x08 sample rate
    uint32_t rate_out;      // 0x0c the sample rate asked for (a converter is set up for some pairs)
    uint32_t msg;           // 0x10 notification message
    int32_t chunks;         // 0x14 chunks sent since the queue was last empty
    uint32_t hwnd;          // 0x18
    uint8_t _1c[4];         // (an empty object; its constructor returns itself)
    int32_t effect;         // 0x20 index of the last unit's name in the phone table (-1: none)
    VocSynthState st;       // 0x24
    uint32_t thread;        // 0x60 the synthesis thread (SetThreadPriority)
    uint8_t _64[0x94 - 0x64];
    uint32_t resample[4];   // 0x94, 0x98, 0x9c, 0xa0 optional sample-rate converters (C++ objects)
    int32_t _a4, _a8, _ac;
    int32_t _b0;            // 0xb0 -1
    int32_t _b4, _b8;
    int32_t mode;           // 0xbc index into the engine's mode table
    int32_t style;          // 0xc0 < 0: the character (\Chr\) to re-apply after a reset
    uint32_t prio[7][3];    // 0xc4 thread priorities per level (low, high, ...)
    uint32_t level;         // 0x118 current row of prio
    uint8_t cs[24];         // 0x11c CRITICAL_SECTION guarding `_00` (the abort flag)
    uint32_t ev_stop;       // 0x134
    uint32_t ev_done;       // 0x138
    GPTR(struct Queue) in;  // 0x13c
    GPTR(struct Queue) out; // 0x140
} VocEngine;

// @0x63674d18 stdcall: index of a phone name (its alphabetic prefix, case-insensitive) in the table
// at 0x6371c0cc (54 entries); -1 if absent, 0 for an empty name
int32_t phone_table_index(const char *name);
// @0x63681cb2 thiscall: synthesize one unit from the unit stage into a new float buffer (grown until it
// fits), optionally resampled, clipped to the 16-bit range; *n = samples. Returns 0.
typedef struct FloatBuf { GPTR(float) data; uint32_t bytes; } FloatBuf;
int32_t voc_synth_request(VocEngine *v, const struct UnitOut *u, FloatBuf *out, int32_t *n);
// @0x63681b7e thiscall: hand n samples to the audio thread (queue record 6: PCM; preceded by record
// 0x1d with the effect index), unless stopped; notifies and raises the thread priority when the audio
// queue is full or five chunks went out
void voc_send_audio(VocEngine *v, const FloatBuf *pcm, int32_t n);
// a sample-rate converter object's convert method (not decompiled; never reached by the corpus)
int32_t voc_resample(uint32_t obj, uint32_t method, float *in, GPTR(float) *out, int32_t n);
// the engine's table of voice modes (per mode: the mode descriptions; +0x90c of each is its name)
typedef struct ModeTable {
    uint8_t mode_id[16];                // 0x00 the base voice's mode id (voices.c)
    int32_t refs;                       // 0x10 engines using it
    int32_t count;                      // 0x14 characters (CFG streams) of the voice
    GPTR(GPTR(const uint8_t)) modes;    // 0x18 [count + 1]: 1.. the characters' CFG streams (0 unused)
    int32_t loaded;                     // 0x1c the voice's Inventory has been loaded (shared by engines)
    GPTR(VocVoice) inv;                 // 0x20 it
} ModeTable;
// @0x636819e6 thiscall: apply character `style` of the engine's mode (0: back to plain): the mode's
// records switch on whisper (1), an FIR (2) or IIR (3) filter with their coefficients, a room/RoboSoft
// effect (4) and its FIR coefficients (6)
void voc_init(VocEngine *v, int32_t style);
// @0x6367ddfb stdcall: returns its argument
uint32_t identity32(uint32_t x);
// the resamplers' flush methods (not decompiled; unreached)
void voc_resample_flush(uint32_t obj, uint32_t method);
// @0x63681ede thiscall: the synthesis thread's loop; @0x636822a1 its thread procedure
int32_t voc_thread_loop(VocEngine *v);

// @0x63675c95 stdcall: a Gaussian deviate (Box-Muller over MSVCRT rand; pairs, the second cached in *b)
float voc_gauss(int32_t *have, float *a, float *b);
// @0x63675d45 stdcall: n bytes of Gaussian noise (sigma 50, rounded half away from zero)
int8_t *voc_noise_make(int32_t n);
// @0x636850c0 stdcall: sin(pi * i / n) over a full period of 2n entries, for the FFT
void voc_sine_table(int32_t n, float *out);
// @0x63685172 stdcall: raised-cosine fade (1 + cos(pi * i / n)) / 2, n entries
float *voc_fade_window(int32_t n);
// @0x63672010 stdcall: the Inventory header (noise length, LPC order, codebook counts, FFT size) and
// the tables derived from it
VocVoice *voc_header_load(void *stm);
// @0x63671f8d stdcall: one codebook (count, dim, count * dim floats)
Codebook *voc_codebook_load(void *stm);
// @0x636720f4 stdcall: the whole Inventory stream: header, the three codebook sets, the unit blobs
VocVoice *voc_inventory_load(void *stm);
// @0x63671063 thiscall: bind the synthesizer to a loaded voice (filter state, LPC memory); 1 = ok
int32_t voc_setup(VocSynthState *s, const VocVoice *v, int32_t unused);
// @0x63681688 thiscall: the synthesis thread's set-up: queues, output format, notification target;
// loads the voice's Inventory once per mode (cached in the mode table, under a lock), sets up an
// optional sample-rate converter (22050/11025 -> 8000, 8000 -> 11025, 22050 -> 16000) and the
// synthesizer, then applies the starting character. 1, or E_FAIL
int32_t voc_engine_init(VocEngine *v, uint8_t *qin, uint8_t *qout, int32_t mode, int32_t style, void *stg,
                        int32_t pcm16, uint32_t rate, uint32_t rate_out, int32_t no_load, uint32_t hwnd,
                        uint32_t msg);
// the sample-rate converters' constructors (C++; not decompiled, never reached by the corpus).
// by resample[] slot: 0 22050 -> 8000, 1 22050 -> 16000, 2 11025 -> 8000, 3 8000 -> 11025
uint32_t voc_resampler_new(int slot);
// @0x63671000 thiscall: the synthesizer object's constructor (no voice, no buffers)
VocSynthState *voc_state_ctor(VocSynthState *s);
// @0x63674cd8 thiscall: an empty object's constructor (returns itself)
void *empty_ctor(void *self);
// @0x63681401 thiscall: the synthesis thread object's constructor: no converters, a lock, the stop and
// done events, the current thread's handle, the table of thread priorities per level (level 3)
VocEngine *voc_engine_ctor(VocEngine *v);
