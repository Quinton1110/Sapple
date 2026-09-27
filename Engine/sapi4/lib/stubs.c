// Functions of the original the portable build has no C for; none is reached by the engine's text
// path with the shipped voices (the older senone-tree format, the text mode of 0x636744c4, the
// sample-rate converters an audio destination would need if it refused the voice's own rate).
#include "unitsel.h"
#include "voice.h"
#include "fe_vm.h"
#include "fe_input.h"
#include "containers.h"
#include <stdio.h>
#include <stdlib.h>

int32_t text_from_other(char *text, GPTR(char) *out) { (void)text; (void)out; fprintf(stderr, "text mode not decompiled\n"); abort(); }
uint8_t vm_proc_guest(int inst, uint8_t n, int16_t *left, int16_t *len) {
    (void)left; (void)len;
    fprintf(stderr, "rule procedure %d/%d is not decompiled\n", inst, n);
    abort();
}
double lattice_cost_other(uint32_t fn, int32_t a, int32_t b) { (void)fn; (void)a; (void)b; abort(); }
int32_t tree_query(const SenoneTree *t, TreeQuery *q) { (void)t; (void)q; fprintf(stderr, "older senone trees not decompiled\n"); abort(); }
int32_t senone_tree_load_old(void *stg, const char *name, const void *ps, GPTR(SenoneTree) *out) {
    (void)stg; (void)name; (void)ps; (void)out;
    fprintf(stderr, "older senone trees not decompiled\n");
    abort();
}
uint32_t voc_resampler_new(int slot) { (void)slot; fprintf(stderr, "sample-rate converters not decompiled\n"); abort(); }
int32_t voc_resample(uint32_t obj, uint32_t method, float *in, GPTR(float) *out, int32_t n) {
    (void)obj; (void)method; (void)in; (void)out; (void)n;
    abort();
}
void voc_resample_flush(uint32_t obj, uint32_t method) { (void)obj; (void)method; abort(); }
