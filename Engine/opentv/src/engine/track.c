/*
 * Parameter-track shaping.
 *
 * Stage 3 writes the 22 synthesis parameters into 256-byte circular tracks,
 * one byte per frame.  It rarely writes a flat run: a parameter that has to
 * move from one value to another does so along one of the twelve decay
 * curves in g_track_shape, which are tables of descending weights (255, 164,
 * 92, 41, 10, 0) ending in a zero.  The four helpers here are the ways those
 * curves get applied: a straight line, a curve laid down over a run, and the
 * two directions of blending a curve into what is already there.
 */
#include "engine.h"

/* The decay curves, indexed by how many frames the move is spread over. */
/* @0x100ef5f8 */
extern const tv_ref g_track_shape[];

/* Interpolate linearly across the run from at-back to at+fwd, taking the two
 * end values from the track itself. */
/* @0x10025210 */
void TV_STDCALL Track_Line(uint8_t *buf, int32_t at, int32_t back,
                           int32_t fwd)
{
    int32_t n = back + fwd;
    int32_t pos = at - back;
    int32_t lo = (int32_t)buf[pos & 0xff];
    int32_t delta = (int32_t)buf[(at + fwd) & 0xff] - lo;
    int32_t acc = 0;
    int32_t i;

    if (n <= 0)
        return;
    for (i = n; i != 0; i--) {
        buf[pos & 0xff] = (uint8_t)((acc / n) + lo);
        acc += delta;
        pos++;
    }
}

/* Lay a curve over the run: the track decays from "from" to "to" along the
 * shape, then holds "to" for whatever is left of the n frames. */
/* @0x10025170 */
void TV_THISCALL Track_Decay(Engine *self, uint8_t *buf, int32_t pos,
                             int32_t shape, int32_t n, uint8_t from,
                             uint8_t to)
{
    const uint8_t *s = TV_REF(uint8_t, g_track_shape[shape]);
    int32_t delta = (int32_t)from - (int32_t)to;
    int32_t w = (int32_t)*s++ << 7;
    int32_t left = n;
    (void)self;

    while (w != 0) {
        if (left == 0)
            return;
        left--;
        buf[pos & 0xff] = (uint8_t)(Synth_MulQ15(delta, w) + to);
        pos++;
        w = (int32_t)*s++ << 7;
    }
    while (left > 0) {
        buf[pos & 0xff] = to;
        pos++;
        left--;
    }
}

/* Blend the track backwards from pos toward the target along the shape. */
/* @0x100252b0 */
void TV_THISCALL Track_BlendBack(Engine *self, uint8_t *buf, int32_t pos,
                                 int32_t shape, int32_t n, uint8_t target)
{
    const uint8_t *s = TV_REF(uint8_t, g_track_shape[shape]) + 1;
    int32_t w = (int32_t)*s << 7;
    int32_t left = n;
    uint8_t *p;
    (void)self;

    while (w != 0) {
        if (left == 0)
            return;
        left--;
        pos--;
        p = buf + (pos & 0xff);
        s++;
        *p = (uint8_t)(*p + Synth_MulQ15((int32_t)target - (int32_t)*p, w));
        w = (int32_t)*s << 7;
    }
}

/* The same, forwards. */
/* @0x10025330 */
void TV_THISCALL Track_BlendFwd(Engine *self, uint8_t *buf, int32_t pos,
                                int32_t shape, int32_t n, uint8_t target)
{
    const uint8_t *s = TV_REF(uint8_t, g_track_shape[shape]);
    int32_t w = (int32_t)*s++ << 7;
    int32_t left = n;
    uint8_t *p;
    (void)self;

    while (w != 0) {
        if (left == 0)
            return;
        left--;
        p = buf + (pos & 0xff);
        pos++;
        *p = (uint8_t)(*p + Synth_MulQ15((int32_t)target - (int32_t)*p, w));
        w = (int32_t)*s++ << 7;
    }
}

/* Fill a run of the track with one value. */
/* @0x100054e0 */
void TV_STDCALL Track_Fill(uint8_t *buf, int32_t pos, int32_t n,
                           uint8_t value)
{
    int32_t i;

    for (i = n; i != 0; i--) {
        buf[pos & 0xff] = value;
        pos++;
    }
}

/* Ramp linearly from "from" to "to" over n frames, ending on "to". */
/* @0x1003b230 */
void TV_STDCALL Track_RampTo(uint8_t *buf, int32_t pos, int32_t n,
                             uint8_t from, uint8_t to)
{
    int32_t delta = (int32_t)to - (int32_t)from;
    int32_t acc = delta;
    int32_t i;

    if (n == 0)
        return;
    for (i = n; i != 0; i--) {
        buf[pos & 0xff] = (uint8_t)(from + (acc / n));
        acc += delta;
        pos++;
    }
}

/* Move a run of the track by a delta rather than toward a target: the delta
 * is applied in full at the start of the curve and fades out along it.  With
 * mode below 3 the track is not allowed to go negative. */
/* @0x10005510 */
void TV_THISCALL Track_Nudge(Engine *self, uint8_t *buf, int32_t mode,
                             int32_t pos, int32_t shape, int32_t n,
                             int32_t delta)
{
    const uint8_t *s = TV_REF(uint8_t, g_track_shape[shape]) + 1;
    int32_t p = pos - 1;
    int32_t w, v;
    (void)self;

    if ((int32_t)buf[p & 0xff] + delta < 0)
        delta = -(int32_t)buf[p & 0xff];
    w = (int32_t)*s << 7;
    while (w != 0) {
        if (n == 0)
            return;
        n--;
        v = Synth_MulQ15(delta, w) + (int32_t)buf[p & 0xff];
        if (v <= 0 && mode < 3)
            v = 0;
        buf[p & 0xff] = (uint8_t)v;
        p--;
        s++;
        w = (int32_t)*s << 7;
    }
}
