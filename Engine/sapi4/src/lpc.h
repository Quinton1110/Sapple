#pragma once
// @0x63678470 stdcall: line spectral frequencies (normalised, 0..0.5) -> LPC polynomial out[0..order]
// (out[0] = 1). If lsf is not strictly increasing it is sorted IN PLACE first (dsp_sort_floats). The
// fourth argument is never read. 8.7% of all guest instructions.
void lpc_from_lsf(float *lsf, float *out, int order, int unused);
