/*
 * Formant synthesizer state: the 22 parameter tracks and the fixed filter
 * coefficients.
 *
 * Parameters (index: default raw value -> scaled by Synth_ScaleParam):
 *   0..8   amplitudes and source controls
 *   9..12  formant frequencies F1..F4 (400, 1396, 2400, 3296 Hz at default)
 *   13..17 bandwidths
 *   18..21 other source parameters
 */
#include "engine.h"

/* Initial raw value of each synthesis parameter track. */
/* @0x100f8438 */
extern const uint8_t g_param_init[22];

/* Sample-rate dependent synthesizer tables (contents not yet decoded). */
/* @0x100b54e0 */ extern const uint8_t g_syn8k_0[];
/* @0x100b5508 */ extern const uint8_t g_syn8k_1[];
/* @0x100b5530 */ extern const uint8_t g_syn8k_2[];
/* @0x100b55d0 */ extern const uint8_t g_syn8k_3[];
/* @0x100b55f8 */ extern const uint8_t g_syn8k_4[];
/* @0x100b5620 */ extern const uint8_t g_syn8k_5[];
/* @0x101219c8 */ extern const uint8_t g_syn8k_6[];
/* @0x10121f68 */ extern const uint8_t g_syn8k_7[];
/* @0x10122d28 */ extern const uint8_t g_syn8k_8[];
/* @0x100b5b88 */ extern const uint8_t g_syn8k_9[];
/* @0x100b5468 */ extern const uint8_t g_syn11k_0[];
/* @0x100b5490 */ extern const uint8_t g_syn11k_1[];
/* @0x100b54b8 */ extern const uint8_t g_syn11k_2[];
/* @0x100b5558 */ extern const uint8_t g_syn11k_3[];
/* @0x100b5580 */ extern const uint8_t g_syn11k_4[];
/* @0x100b55a8 */ extern const uint8_t g_syn11k_5[];
/* @0x101216f8 */ extern const uint8_t g_syn11k_6[];
/* @0x10121c98 */ extern const uint8_t g_syn11k_7[];
/* @0x10122238 */ extern const uint8_t g_syn11k_8[];
/* @0x100b5b40 */ extern const uint8_t g_syn11k_9[];

/* OpenTV's extra rate, which the original never had: computed rather than
 * lifted, by tools/gen_synth_hifi.py.  Only the three rate-dependent
 * resonator tables differ from the wideband set; see that generator.
 * No address annotation, because there is nothing to annotate. */
extern const int32_t g_synhifi_6[180];
extern const int32_t g_synhifi_7[180];
extern const int32_t g_synhifi_8[700];
extern const int32_t g_synhifi_2038;
extern const int32_t g_synhifi_c12;
extern const int32_t g_synhifi_c13;

/* Fixed Q15 filter coefficients, 8000 Hz and 11025 Hz output. */
static const int16_t filt_coef_8k[40] = {
    0, 0, -24759, 20770, 0, 0, 0, 25889, 0, 0,
    0, 0, 30914, 30245, 0, 0, 0, 0, 0, 8192,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 30245, 0, 0, 0, 11354, 0, 0,
};
static const int16_t filt_coef_11k[40] = {
    0, 0, -24759, 20770, -24550, 28898, -19553, 27618, 0, 0,
    0, 0, 31521, 30905, 0, 0, 0, 17368, 0, 8192,
    0, 27691, 0, 25148, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 30905, 0, 0, 0, 11354, 0, 0,
};

/* Every track restarts holding its initial value for 12 frames. */
/* @0x10003840 */
void TV_THISCALL Synth_ResetTracks(Engine *self)
{
    int i, j;
    for (i = 0; i < 22; i++) {
        self->trk_rd[i] = 10;
        self->trk_wr[i] = 12;
        self->trk_buf[i] = self->trk_data[i];
        for (j = 0; j < 12; j++)
            self->trk_buf[i][j] = g_param_init[i];
    }
    self->trk_04 = 0;
    self->trk_38 = 0;
    self->trk_10 = 12;
    self->trk_0c = 12;
    self->trk_34 = 0;
    for (i = 0; i < 32; i++) {
        self->trk_14[i] = 0;
        self->s3_1fbd[i] = 0;
    }
    self->trk_08 = self->trk_0c - 2;
    self->synth_busy = 0;
}

/* Select the filter coefficients and tables for the output sample rate. */
/* @0x100043c0 */
void TV_THISCALL Synth_InitFilters(Engine *self)
{
    const int16_t *coef;
    int i, hifi = 0;
    self->synth_19ad = 0;
    self->synth_19ae = 0;
    self->synth_hold = 0;
    if (self->sample_rate == 8000) {
        self->syn_tab[9] = g_syn8k_9;
        self->syn_tab[6] = g_syn8k_6;
        self->syn_tab[7] = g_syn8k_7;
        self->syn_tab[8] = g_syn8k_8;
        self->syn_tab[0] = g_syn8k_0;
        self->syn_tab[1] = g_syn8k_1;
        self->syn_tab[2] = g_syn8k_2;
        self->syn_tab[3] = g_syn8k_3;
        self->syn_tab[4] = g_syn8k_4;
        self->syn_tab[5] = g_syn8k_5;
        self->syn_2038 = -7870;
        self->syn_203c = 7281;
        self->syn_2040 = -6472;
        coef = filt_coef_8k;
    } else {
        self->syn_tab[9] = g_syn11k_9;
        self->syn_tab[6] = g_syn11k_6;
        self->syn_tab[7] = g_syn11k_7;
        self->syn_tab[8] = g_syn11k_8;
        self->syn_tab[0] = g_syn11k_0;
        self->syn_tab[1] = g_syn11k_1;
        self->syn_tab[2] = g_syn11k_2;
        self->syn_tab[3] = g_syn11k_3;
        self->syn_tab[4] = g_syn11k_4;
        self->syn_tab[5] = g_syn11k_5;
        self->syn_2038 = -7956;
        self->syn_203c = 7521;
        self->syn_2040 = -6905;
        coef = filt_coef_11k;
        /* OpenTV: the extra rate keeps the wideband tables above -- they are
         * not 11.025-specific, they are what the engine uses whenever it
         * is not at 8 kHz -- and swaps the three that really do depend on
         * the sample rate. */
        if (self->sample_rate == TV_SR_HIFI) {
            self->syn_tab[6] = (const uint8_t *)g_synhifi_6;
            self->syn_tab[7] = (const uint8_t *)g_synhifi_7;
            self->syn_tab[8] = (const uint8_t *)g_synhifi_8;
            self->syn_2038 = g_synhifi_2038;
            hifi = 1;
        }
    }
    for (i = 0; i < 40; i++)
        self->filt_coef[i] = coef[i];
    /* Three entries of that array are a fixed 242 Hz resonator rather
     * than a starting value, and frame.c never rewrites them, so at
     * the extra rate they have to be replaced too or the filter keeps 11 kHz
     * damping and the output rolls off far too steeply. */
    if (hifi) {
        self->filt_coef[12] = (int16_t)g_synhifi_c12;
        self->filt_coef[13] = (int16_t)g_synhifi_c13;
        self->filt_coef[33] = (int16_t)g_synhifi_c13;
    }
}

/* Convert a raw 8-bit parameter value to its working scale. */
/* @0x1002c920 */
int32_t TV_CDECL Synth_ScaleParam(int32_t index, uint8_t raw)
{
    int32_t v = raw;
    switch (index) {
    case 9:
        return v << 2;
    case 10:
        return v * 8 + 500;
    case 11:
    case 12:
        return v << 4;
    case 13:
    case 14:
    case 15:
        return v * 2;
    case 16:
        return v * 4 + 192;
    case 17:
        return v * 2;
    default:
        return v;
    }
}

/* Run the waveform generator for the pending parameter frame, if any, then
 * the per-step synthesizer bookkeeping.  Always returns 1. */
/* @0x100047e0 */
uint8_t TV_THISCALL Synth_Step(Engine *self)
{
    if (!self->synth_hold && !self->synth_busy && self->synth_19ad) {
        if (self->w_212e)
            Synth_Generate(self, (int16_t)self->sample_rate, self->filt_coef);
        self->synth_19ad = 0;
        if (self->trk_04 == self->trk_08)
            self->synth_busy = 1;
    }
    Synth_Frame(self);
    return 1;
}

/* Operations on the parameter-track frame positions:
 *   1: is there room for `n` more frames?     2: rebase all positions by
 *   0x800 once far enough in (keeps them small);  3: is the reader behind
 *   the writer?   4: advance the reader.  Other ops return 0. */
/* @0x100038d0 */
uint8_t TV_THISCALL Tracks_Op(Engine *self, int32_t op, int32_t n)
{
    int i;
    switch (op) {
    case 1:
        return self->s3_1fe4 - self->trk_04 + self->trk_10 + n < 0x100;
    case 2:
        if (self->trk_04 > 0x800) {
            for (i = 0; i < 22; i++) {
                self->trk_wr[i] -= 0x800;
                self->trk_rd[i] -= 0x800;
            }
            self->trk_04 -= 0x800;
            if (self->trk_08 != -1)
                self->trk_08 -= 0x800;
            self->trk_0c -= 0x800;
            self->trk_10 -= 0x800;
        }
        return 1;
    case 3:
        return self->trk_04 + self->s3_1fe0 < self->trk_0c;
    case 4:
        self->trk_04++;
        return 1;
    default:
        return 0;
    }
}
