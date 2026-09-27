#pragma once
// @0x63684b60 stdcall: inverse real FFT, in place (Sorensen's split-radix algorithm, decimation in
// frequency, then length-two butterflies, bit reversal and division by n). Input packing: x[0..n/2] the
// real parts, x[n-k] the imaginary part of bin k. sine[k] = sin(2*pi*k / (2n)) for k < 2n... (the table
// is read at sine[s*j] and sine[s*j + n/2] with s the stage's step). 36% of all guest instructions.
void fft_inverse_real(float *x, int n, int m, const float *sine);
