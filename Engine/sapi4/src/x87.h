// What the original's x87 code does, in portable C. The engine runs with the Win32 default control
// word: 53-bit precision, round to nearest. So every x87 intermediate is an IEEE double, a store to a
// float rounds once more, and "float op float" in the original is (float)((double)a op (double)b) here.
// Build with -ffp-contract=off (no fused multiply-add) or the rounding differs.
#pragma once
#include <math.h>
#include <stdint.h>

// fistp m32 (round to nearest even; out of range or NaN = the "integer indefinite" 0x80000000)
static inline int32_t x87_fistp32(double v) {
    double r = nearbyint(v);
    if (!(r >= -2147483648.0 && r < 2147483648.0)) return INT32_MIN;
    return (int32_t)r;
}
// MSVCRT _ftol: truncate to a 64-bit integer (indefinite = INT64_MIN); callers use EAX, the low half
static inline int32_t x87_ftol(double v) {
    int64_t r;
    if (isnan(v) || v >= 9223372036854775808.0 || v < -9223372036854775808.0) r = INT64_MIN;
    else r = (int64_t)v;
    return (int32_t)(uint32_t)(uint64_t)r;
}
// |x| as the original computes it: fld x; fcomp 0.0; jae skip; fchs. NaN is negated (unordered = below).
static inline double x87_abs_cmp0(float x) {
    double v = x;
    return (v >= 0.0) ? v : -v;
}
// Conditional jumps after fcom a, b (ST0 = a), NaN-exact: an unordered compare sets C0 and C3, so it
// reads as "below" and as "equal" at the same time.
static inline int x87_je(double a, double b) { return !(a < b) && !(a > b); }   // equal or unordered
static inline int x87_jb(double a, double b) { return !(a >= b); }              // below or unordered
static inline int x87_jbe(double a, double b) { return !(a > b); }              // below, equal or unordered
static inline int x87_ja(double a, double b) { return a > b; }
static inline int x87_jae(double a, double b) { return a >= b; }
