#pragma once
#include <stdint.h>
// @0x63675c05 stdcall: decode n values from signed bytes around base: d > 0 -> base + d, d <= 0 ->
// d - base; the escapes 0x7f (base + 127) and 0x80 (-128 - base) each add the following byte, and
// chain while the byte just added is the same escape. Returns the number of bytes consumed (EAX: the
// callers use it to advance through the stream).
int codec_bytes_to_floats(const int8_t *p, int n, float *out, int base);
