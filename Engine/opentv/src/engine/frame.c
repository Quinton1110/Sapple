/*
 * Building one synthesizer frame.
 *
 * Synth_Step calls this once per frame.  It reads the 22 parameter tracks at
 * the current position, applies the per-voice percentage adjustments, turns
 * the pitch into the two half-period counts (with a jitter LFSR), looks the
 * formant frequencies and bandwidths up in the sample-rate dependent
 * coefficient tables, and leaves the 40 int16s of Engine.filt_coef ready for
 * Synth_Generate.
 */
#include "engine.h"

/* Per-voice parameter adjustments: 15 int32s per voice. */
/* @0x100b5068 */ extern const int32_t g_voice_adjust[];
/* Jitter, shimmer and gain tables. */
/* @0x100b5648 */ extern const int32_t g_tab_5648[];
/* @0x100b56c8 */ extern const int32_t g_tab_56c8[];
/* @0x100b5688 */ extern const int32_t g_tab_5688[];
/* @0x100b57c8 */ extern const int32_t g_tab_57c8[];
/* @0x100b5848 */ extern const int32_t g_tab_5848[];
/* @0x100b5a78 */ extern const int32_t g_tab_5a78[];
/* @0x100b5af8 */ extern const int32_t g_tab_5af8[];
/* @0x100b5418 */ extern const int32_t g_tab_5418[];
/* @0x10123a48 */ extern const int32_t g_tab_123a48[];
/* @0x101239bc */ extern const int32_t g_tab_1239bc[];
/* @0x10123b70 */ extern const int32_t g_tab_123b70[];
/* @0x10123d58 */ extern const int32_t g_tab_123d58[];
/* @0x10123818 */ extern const int32_t g_tab_123818[];
/* @0x101238ac */ extern const int32_t g_tab_1238ac[];
/* Flag bits per parameter index (shared with stage 0). */
/* @0x100f83a0 */ extern const uint32_t g_s0_flag_lo[16];

/* @0x10025150 */
int32_t TV_STDCALL Synth_MulQ15(int32_t a, int32_t b)
{
    return (b * a) / 0x7fff;
}

/* @0x10025280 */
int32_t TV_STDCALL Synth_MulShr11(int32_t a, int32_t b)
{
    return (b * a) >> 11;
}

/* Returns the product at Q12 and leaves it at Q13 in *hi. */
/* @0x10025290 */
int32_t TV_STDCALL Synth_MulShr12(int32_t a, int32_t b, int32_t *hi)
{
    int32_t v = b * a;

    *hi = v >> 13;
    return v >> 12;
}

/* @0x10004750 */
uint8_t TV_THISCALL Synth_Gate(Engine *self, int32_t op)
{
    if (op == 3) {
        if (self->synth_19ad != 0)
            return 0;
        if (self->synth_19ae != 0)
            return 0;
        self->synth_19ae = 1;
        return 1;
    }
    if (op == 4) {
        self->synth_19ad = 1;
        self->synth_19ae = 0;
        return 1;
    }
    if (op == 5)
        return self->synth_19ad;
    if (op == 6) {
        self->synth_19ad = 0;
        return 1;
    }
    return 0;
}

/* The pitch-jitter LFSR: one step, returning the low six bits. */
static int32_t jitter(Engine *self)
{
    int32_t v = self->e_206c;
    int32_t nv = ((((v * 2) ^ v) & 2) << 6) + (v >> 1);

    self->e_206c = nv;
    return nv & 0x3f;
}

int tv_ext_clarity = 0;

/* Widen a formant's bandwidth at high speech rates.
 *
 * The byte offset into tables 6 and 7 is the bandwidth in hertz -- they hold
 * exp(-4*pi*i/Fs) and its square for i = offset/4 -- so widening is a plain
 * scale of the offset.  A wider bandwidth is a shorter impulse response, so
 * the resonator settles inside the phoneme instead of ringing on into the
 * next one, which is what fast speech otherwise sounds like.
 *
 * From TGSpeechBox by Tamas Geczy (MIT), which does the same thing to its
 * cascade bandwidths above a speed threshold; see NOTICE.
 */
static int32_t bw_widen(const Engine *self, int32_t off)
{
    int32_t row = self->rate_index, ramp, scale;

    if (!tv_ext_clarity || row <= TV_BW_ROW_START)
        return off;
    ramp = (row - TV_BW_ROW_START) * 256 / (TV_BW_ROW_FULL - TV_BW_ROW_START);
    if (ramp > 256)
        ramp = 256;
    scale = 256 + ramp * (TV_BW_MAX_Q8 - 256) / 256;
    off = (off * scale) >> 8;
    /* Table 6 is 180 entries of four bytes; the last one is offset 716. */
    if (off > 716)
        off = 716;
    return off & ~3;
}

/* The coefficient tables are indexed by byte offset. */
#define SYN(i, off) (*(const int32_t *)((const uint8_t *)self->syn_tab[i] + (off)))

/* @0x10002a40 */
void TV_THISCALL Synth_Frame(Engine *self)
{
    int32_t p[22];
    const int32_t *adj;
    int32_t pos, i, k, v, sr2, jit, t10, t14 = 0, t1c, t18, t20, t24;
    int32_t c23, c25, c27, lo, hi, d1, d2, d3, e1, e2, e3;
    int32_t flag;
    int16_t w;

    if (self->s3_1fe0 + self->trk_04 >= self->trk_0c)
        return;
    if (!Synth_Gate(self, 3))
        return;

    /* The three amplitude tracks must not dip below the floor. */
    for (i = 0; i < 3; i++) {
        uint8_t *q = self->trk_buf[13 + i] + (self->trk_04 & 0xff);

        if ((int32_t)*q * 2 < 0x2d)
            *q = 0x16;
    }

    pos = self->trk_04 & 0xff;
    k = pos >> 3;
    v = pos & 7;
    if ((uint32_t)self->trk_14[k] & g_s0_flag_lo[v]) {
        self->trk_34++;
        self->trk_14[k] = (uint8_t)(self->trk_14[k] & (uint8_t)~(uint8_t)g_s0_flag_lo[v]);
    }
    flag = self->s3_1fbd[k] & (uint8_t)g_s0_flag_lo[v];
    self->s3_1fbd[k] = (uint8_t)(self->s3_1fbd[k] & (uint8_t)~(uint8_t)g_s0_flag_lo[v]);

    for (i = 0; i < 22; i++)
        p[i] = self->trk_buf[i][pos];

    /* ---- per-voice adjustments -------------------------------------- */
    t1c = (p[21] & 0xf0) >> 4;
    adj = &g_voice_adjust[t1c * 15];
    p[9] += (adj[0] * p[9]) / 100;
    p[13] += (adj[1] * p[13]) / 100;
    p[10] += (adj[2] * p[10]) / 100;
    if (p[10] > 0xff)
        p[10] = 0xff;
    p[14] += (adj[3] * p[14]) / 100;
    p[11] += (adj[4] * p[11]) / 100;
    p[15] += (adj[5] * p[15]) / 100;
    p[12] += (adj[6] * p[12]) / 100;
    v = p[11] + 0x14;
    if (v > p[12])
        p[12] = v;
    t24 = adj[8];
    v = (p[12] + 0x19) << 4;
    if (v > t24)
        t24 = v;
    p[18] = adj[13];
    p[19] = adj[14];
    p[2] += adj[12];
    if (p[2] > 0x6b)
        p[2] = 0x6b;

    /* ---- silence detection ------------------------------------------ */
    if (p[0] == 0 && p[1] == 0 && p[2] == 0) {
        if (self->e_2074 == 0)
            self->e_2074 = 1;
        self->e_2070++;
    } else {
        if (self->e_2074 == 1)
            self->e_2074 = 0;
        self->e_2070 = 0;
    }

    /* ---- the two half periods --------------------------------------- */
    if (p[0] == 0)
        p[17] = 0;
    p[17] <<= 1;
    if (p[17] == 0) {
        self->filt_coef[0] = 0;
        self->filt_coef[30] = 0;
        self->filt_coef[31] = 0;
        self->filt_coef[34] = 0;
        self->filt_coef[35] = 0;
    } else {
        if (p[17] == 0x45 || p[17] == 0x4a || p[17] == 0x4f ||
            p[17] == 0x54 || p[17] == 0x5a)
            p[17]++;

        jit = jitter(self);
        t10 = Synth_MulQ15(p[17], g_tab_5648[adj[10]]);
        v = Synth_MulQ15(t10, g_tab_56c8[jit]);
        self->filt_coef[30] = (int16_t)((int16_t)p[17] + v * 8);

        jit = jitter(self);
        v = Synth_MulQ15(t10, g_tab_56c8[jit]);
        self->filt_coef[31] = (int16_t)((int16_t)p[17] + v * 8);

        sr2 = (int32_t)self->sample_rate * 2;
        self->filt_coef[30] = (int16_t)((sr2 / (int32_t)self->filt_coef[30]) / 2);
        w = self->filt_coef[30];
        self->filt_coef[31] = (int16_t)((sr2 / (int32_t)self->filt_coef[31]) / 2);

        self->filt_coef[0] = (int16_t)p[18];
        v = Synth_MulQ15((int32_t)w, p[19] << 11);
        v = Synth_MulQ15(v, g_tab_57c8[p[18]]);
        if (v > 0x8b)
            v = 0x8b;
        self->filt_coef[1] = (int16_t)g_tab_5848[v];

        self->filt_coef[34] = (int16_t)g_tab_123a48[p[0]];
        v = Synth_MulQ15((int32_t)self->filt_coef[34], g_tab_5a78[p[18]]);
        self->filt_coef[34] = (int16_t)v;
        v = Synth_MulQ15((int32_t)self->filt_coef[34], g_tab_5af8[p[19]]);
        v >>= 3;
        self->filt_coef[34] = (int16_t)v;
        w = (int16_t)((int16_t)(self->filt_coef[30] >> 2) * (int16_t)v);
        self->filt_coef[34] = w;
        self->filt_coef[35] = w;

        v = Synth_MulQ15((int32_t)w, g_tab_5688[adj[11]]);
        v = Synth_MulQ15(v, g_tab_56c8[jit]);
        self->filt_coef[34] = (int16_t)(self->filt_coef[34] + (int16_t)(v << 3));

        jit = jitter(self);
        v = Synth_MulQ15((int32_t)self->filt_coef[35], g_tab_5688[adj[11]]);
        v = Synth_MulQ15(v, g_tab_56c8[jit]);
        self->filt_coef[35] = (int16_t)(self->filt_coef[35] + (int16_t)(v << 3));
    }

    if (self->mute == 1)
        self->filt_coef[0] = (int16_t)(self->filt_coef[0] | 0x80);
    if (self->e_2070 > 2) {
        self->filt_coef[0] = (int16_t)(self->filt_coef[0] | 0x80);
        self->e_2070 = 2;
    }

    /* ---- the nasal branch and the first formant ---------------------- */
    self->filt_coef[16] = (int16_t)g_tab_1239bc[p[2]];
    self->filt_coef[38] = (int16_t)*(const int32_t *)self->syn_tab[0];
    self->filt_coef[39] = (int16_t)*(const int32_t *)self->syn_tab[1];
    self->filt_coef[37] = (int16_t)*(const int32_t *)self->syn_tab[2];

    v = (int32_t)(int16_t)((int32_t)self->sample_rate / 2);
    if (v < t24) {
        self->filt_coef[21] = 0x2000;
        self->filt_coef[4] = 0;
        self->filt_coef[5] = 0;
    } else {
        int32_t o9 = bw_widen(self, adj[9] & ~3);

        t20 = 0;
        v = SYN(7, o9);
        lo = Synth_MulShr12(SYN(6, o9), SYN(8, ((t24 & ~6) >> 1)), &t14);
        self->filt_coef[4] = (int16_t)lo;
        self->filt_coef[5] = (int16_t)(v * 4);
        self->filt_coef[21] = (int16_t)((int16_t)((int16_t)v - (int16_t)t14) + 0x2000);
    }

    v = (int32_t)(int16_t)((int32_t)self->sample_rate / 2);
    if ((p[12] << 4) >= v)
        p[12] = (v - 0xa) >> 4;

    /* ---- F1: from adj[7] and p[12] ---------------------------------- */
    {
        int32_t o7 = bw_widen(self, adj[7] & ~3);

        c23 = SYN(7, o7);
        lo = Synth_MulShr12(SYN(6, o7), SYN(8, p[12] * 8), &t14);
        self->filt_coef[6] = (int16_t)lo;
        self->filt_coef[7] = (int16_t)(c23 * 4);
        c23 = c23 - t14 + 0x2000;
        self->filt_coef[23] = (int16_t)c23;
    }

    /* ---- F2: from p[15] and p[11] ----------------------------------- */
    {
        int32_t off = bw_widen(self, (p[15] & ~1) * 2);
        int32_t bwoff = flag ? (((p[11] * 10) & ~6) >> 1) : (p[11] * 8);

        c25 = SYN(7, off);
        lo = Synth_MulShr12(SYN(6, off), SYN(8, bwoff), &t14);
        self->filt_coef[8] = (int16_t)lo;
        self->filt_coef[9] = (int16_t)(c25 * 4);
        c25 = c25 - t14 + 0x2000;
        self->filt_coef[25] = (int16_t)c25;
    }

    /* ---- F3: from p[14] and p[10] ----------------------------------- */
    {
        int32_t off = bw_widen(self, (p[14] & ~1) * 2);
        int32_t bwoff = flag ? (((p[10] * 10) & ~6) >> 1) : (p[10] * 4 + 0xfc);

        c27 = SYN(7, off);
        lo = Synth_MulShr12(SYN(6, off), SYN(8, bwoff), &t14);
        self->filt_coef[10] = (int16_t)lo;
        self->filt_coef[11] = (int16_t)(c27 * 4);
        c27 = c27 - t14 + 0x2000;
        self->filt_coef[27] = (int16_t)c27;
    }

    /* ---- F4: from p[13] and p[9] ------------------------------------ */
    {
        int32_t off = bw_widen(self, (p[13] & ~1) * 2);
        int32_t bwoff = flag ? (((p[9] * 10) & ~6) >> 1) : ((p[9] & ~1) * 2);
        int32_t c36;

        c36 = SYN(7, off);
        lo = Synth_MulShr12(SYN(6, off), SYN(8, bwoff), &t14);
        self->filt_coef[14] = (int16_t)lo;
        self->filt_coef[15] = (int16_t)(c36 * 4);
        c36 = c36 - t14 + 0x2000;
        if (c36 > 0x7ff)
            c36 = 0x7ff;
        self->filt_coef[36] = (int16_t)((int16_t)c36 << 4);
    }

    self->filt_coef[17] = (int16_t)g_tab_5418[t1c];
    lo = Synth_MulShr12(self->syn_2038,
                        SYN(8, ((((p[16] & 0xfe) << 2) + 0xc0) & ~6) >> 1), &t14);
    self->filt_coef[32] = (int16_t)lo;
    self->filt_coef[29] =
        (int16_t)((int16_t)(*(const int32_t *)((const uint8_t *)self->syn_tab[9] +
                                               (p[16] & ~3))) * 2);

    /* ---- the amplitudes --------------------------------------------- */
    if (p[1] <= 0) {
        self->filt_coef[20] = 0;
        self->filt_coef[22] = 0;
        self->filt_coef[24] = 0;
        self->filt_coef[26] = 0;
        v = 0;
    } else {
        d1 = flag ? ((p[9] * 10) >> 5) : (p[9] >> 3);
        d2 = flag ? ((p[10] * 10) >> 5) : ((p[10] >> 2) + 0x10);
        e1 = g_tab_123b70[d1] * 2;
        t10 = e1 - g_tab_123b70[d2] - 0x2c;
        t18 = e1 + g_tab_123b70[d2] * 2 - 0xeb;
        d3 = flag ? ((p[11] * 10) >> 5) : (p[11] >> 1);
        e3 = (p[12] >> 1) - d3;
        e2 = d3 - d2;
        e1 = d2 - d1;
        if (e1 <= 2)
            e1 = 0;
        e2 -= 2;
        if (e2 <= 2)
            e2 = 0;
        e3 -= 5;
        if (e3 <= 2)
            e3 = 0;
        lo = g_tab_123d58[e2];
        hi = g_tab_123d58[e3];
        t20 = g_tab_123d58[e1];

        v = t10 + t20 * 2 + 0x1e + lo + p[1] + p[3];
        if (v < 0)
            v = 0;
        k = g_tab_123818[v];
        if (k > 0xa0)
            k = 0xa0;
        self->filt_coef[26] = (int16_t)Synth_MulShr11(k, c27);

        v = t18 + lo * 2 + 0x16 + hi + p[1] + p[4];
        if (v < 0)
            v = 0;
        if (v > 0xa0)
            v = 0xa0;
        self->filt_coef[24] = (int16_t)(-(int16_t)Synth_MulShr11(g_tab_123818[v], c25));

        v = t18 + hi * 2 + p[5] + p[1] + 0x11;
        if (v < 0)
            v = 0;
        if (v > 0xa0)
            v = 0xa0;
        self->filt_coef[22] = (int16_t)Synth_MulShr11(g_tab_123818[v], c23);
        if (self->sample_rate == 0x1f40)
            self->filt_coef[22] = (int16_t)(self->filt_coef[22] >> 1);

        v = t18 + p[1] + p[6] + 0x10;
        if (v < 0)
            v = 0;
        if (v > 0x9a)
            v = 0x9a;
        self->filt_coef[20] =
            (int16_t)(-(int16_t)Synth_MulShr11(g_tab_123818[v],
                                               (int32_t)self->filt_coef[21]));

        v = (int16_t)((int16_t)g_tab_1238ac[p[8] + p[1]] << 2);
    }
    self->filt_coef[28] = (int16_t)v;

    self->trk_04++;
    self->synth_19ad = 1;
    self->synth_19ae = 0;
}
