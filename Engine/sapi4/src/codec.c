// Voice data decoders of msttssyn.dll.
#include "codec.h"

int codec_bytes_to_floats(const int8_t *p, int n, float *out, int base) {
    int i = 0;
    for (; n > 0; n--, out++, i++) {
        int8_t d = p[i];
        if (d == 0x7f) {
            *out = (float)((double)base - -127.0);
            while (p[i] == 0x7f) { i++; *out = (float)((double)p[i] + *out); }
        } else if (d == -128) {
            *out = (float)(-128.0 - (double)base);
            while (p[i] == -128) { i++; *out = (float)((double)p[i] + *out); }
        } else if (d > 0) {
            *out = (float)((double)base + d);
        } else {
            *out = (float)((double)d - base);
        }
    }
    return i;
}
