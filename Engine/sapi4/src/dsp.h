// Signal-processing leaves of msttssyn.dll (1999). Names are ours; addresses are the DLL's own.
#pragma once
#include <stdint.h>

// @0x636712d5 stdcall: all-pole (IIR) filter in place, direct form. a[1..order] are used (a[0] is not);
// mem[0..order] is the state: mem[1..order] the previous outputs. 26% of all guest instructions.
void dsp_allpole(const float *a, int order, float *mem, float *x, int n);
// @0x63671943 stdcall: FIR filter in place: x[i] = sum c[k] * x[i-k], k = 0..order-1; mem[0..order-1] holds
// the previous inputs.
void dsp_fir(float *x, int n, const float *c, float *mem, int order);
// @0x636719a0 stdcall: recursive filter in place: y = c[0]*x + sum c[k] * mem[k-1] (k = order-1..1); the
// output is shifted into mem[0].
void dsp_iir_c0(float *x, int n, const float *c, float *mem, int order);
// @0x63671e49 stdcall: sum of |x[i]|, returned as a double (ST0)
double dsp_sum_abs(const float *x, int n);
// @0x63671e79 stdcall: x[i] *= g
void dsp_scale(float *x, int n, float g);
// @0x63671e96 stdcall: dst[i] = src[n-1-i]
void dsp_reverse(const float *src, int n, float *dst);
// @0x63671ebc stdcall: float -> 16-bit PCM, round to nearest (no clipping: the low 16 bits of fistp)
void dsp_to_pcm16(int16_t *dst, const float *src, int n);
// @0x63671eee stdcall: float (16-bit scale) -> unsigned 8-bit PCM
void dsp_to_pcm8(uint8_t *dst, const float *src, int n);
// @0x63671f31 stdcall: max |x[i]| (x[0] when n <= 1), returned as a double (ST0)
double dsp_max_abs(const float *x, int n);

// @0x636886c6 stdcall: a second copy of dsp_scale (the linker kept both)
void dsp_scale_b(float *x, int n, float g);
// @0x6368864b stdcall: fold a pulse of len samples into a period of n: the first half (at most n) is
// copied to the start, the rest of dst cleared, and the second half added onto the end of dst.
void dsp_fold_pulse(float *dst, int n, const float *src, int len);
// @0x6368874c stdcall: place the two halves of src (n samples each) at dst[off..] and dst[len-off-n+1..]
void dsp_place_halves(float *dst, const float *src, int off, int n, int len);
// @0x636887a7 stdcall: the same, adding into dst
void dsp_add_halves(float *dst, const float *src, int off, int n, int len);
// @0x636886e3 cdecl: qsort comparator for floats (1 if a > b, -1 if a < b, else 0)
int dsp_cmp_float(const void *a, const void *b);
// @0x63688708 stdcall: sort x ascending (VC qsort); returns 1 if it was already sorted
int dsp_sort_floats(float *x, int n);
// @0x6367131b stdcall: fit one period of src (srcn samples) into dst (dstn, cleared first): dst[0] =
// src[0], then the first and the last min(srcn,dstn)-1 samples are added in, weighted by
// window[(int)(0.5 + k*winn/min)] (a cross-fade when the period shrinks).
void dsp_fit_period(const float *src, int srcn, float *dst, int dstn, const float *window, int winn);
// @0x636713b8 stdcall: one excitation period into dst: reversed (flag), copied (same length) or fitted
// (dsp_fit_period), then scaled by gain unless gain is 1.0 (or NaN).
void dsp_period(const float *src, int srcn, float *dst, int dstn, float gain, int reverse,
                const float *window, int winn);
