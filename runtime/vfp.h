// Scalar VFP semantics for recompiled AArch32 code (included by cpu.h).
//
// Arithmetic maps onto the host's IEEE single / double operations; generated code is compiled with
// -ffp-contract=off so a VMLA (multiply, round, add, round) never turns into a fused multiply-add, and VFMA uses
// fma() explicitly. Conversions to integers follow ARM: saturating, NaN -> 0 (x86's cvtt* return 0x80000000).
#pragma once
#include <math.h>
#include <stdint.h>

static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }
static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }

// VCMP / VCMPE: FPSCR.NZCV (bits 31..28). equal 0110, less 1000, greater 0010, unordered 0011.
static inline void vfp_cmp_f(Cpu *c, float a, float b) {
    uint32_t f = a == b ? 0x6u : a < b ? 0x8u : a > b ? 0x2u : 0x3u;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (f << 28);
}
static inline void vfp_cmp_d(Cpu *c, double a, double b) {
    uint32_t f = a == b ? 0x6u : a < b ? 0x8u : a > b ? 0x2u : 0x3u;
    c->fpscr = (c->fpscr & 0x0FFFFFFFu) | (f << 28);
}
// VMRS APSR_nzcv, FPSCR
static inline void vfp_mrs_flags(Cpu *c) {
    uint32_t f = c->fpscr;
    c->nf = f >> 31; c->zf = (f >> 30) & 1; c->cf = (f >> 29) & 1; c->vf = (f >> 28) & 1;
}

// float / double -> 32-bit integer, round toward zero, saturating
static inline uint32_t vcvt_s32_f64(double d) {
    if (d != d) return 0;
    if (d >= 2147483647.0) return 0x7FFFFFFFu;
    if (d <= -2147483648.0) return 0x80000000u;
    return (uint32_t)(int32_t)d;
}
static inline uint32_t vcvt_u32_f64(double d) {
    if (d != d || d <= 0.0) return 0;
    if (d >= 4294967295.0) return 0xFFFFFFFFu;
    return (uint32_t)d;
}
static inline uint32_t vcvt_s32_f32(float f) { return vcvt_s32_f64((double)f); }
static inline uint32_t vcvt_u32_f32(float f) { return vcvt_u32_f64((double)f); }

// The FPSCR rounding mode (bits 23:22): 0 nearest, 1 +inf, 2 -inf, 3 zero.
static inline double vfp_round_mode(Cpu *c, double d) {
    switch ((c->fpscr >> 22) & 3) {
    case 0: return nearbyint(d);  // (host default mode: round to nearest even)
    case 1: return ceil(d);
    case 2: return floor(d);
    default: return trunc(d);
    }
}
// VCVTR (FPSCR rounding), VCVTA/N/P/M (explicit rounding)
static inline double round_ties_away(double d) { return round(d); }
static inline double round_ties_even(double d) { double r = nearbyint(d); return r; }

// fixed point (VCVT with #fbits): to fixed = round toward zero of x * 2^fbits, saturating to the size
static inline uint32_t vcvt_fx_s(double d, int fbits, int size) {
    d = ldexp(d, fbits);
    if (d != d) return 0;
    double hi = ldexp(1.0, size - 1) - 1, lo = -ldexp(1.0, size - 1);
    if (d >= hi) return size == 32 ? 0x7FFFFFFFu : (uint32_t)(int32_t)hi;
    if (d <= lo) return size == 32 ? 0x80000000u : (uint32_t)(int32_t)lo;
    return (uint32_t)(int32_t)(int64_t)d;
}
static inline uint32_t vcvt_fx_u(double d, int fbits, int size) {
    d = ldexp(d, fbits);
    if (d != d || d <= 0) return 0;
    double hi = ldexp(1.0, size) - 1;
    if (d >= hi) return (uint32_t)hi;
    return (uint32_t)(uint64_t)d;
}

// IEEE 754-2008 maxNum / minNum (VMAXNM / VMINNM): a quiet NaN loses to a number
static inline float vmaxnm_f(float a, float b) { return a != a ? b : b != b ? a : (a > b ? a : (b > a ? b : (signbit(a) ? b : a))); }
static inline float vminnm_f(float a, float b) { return a != a ? b : b != b ? a : (a < b ? a : (b < a ? b : (signbit(a) ? a : b))); }
static inline double vmaxnm_d(double a, double b) { return a != a ? b : b != b ? a : (a > b ? a : (b > a ? b : (signbit(a) ? b : a))); }
static inline double vminnm_d(double a, double b) { return a != a ? b : b != b ? a : (a < b ? a : (b < a ? b : (signbit(a) ? a : b))); }
// VMAX / VMIN (NEON, non-"NM"): NaN in -> default NaN out
static inline float vmax_f(float a, float b) { return (a != a || b != b) ? u2f(0x7FC00000u) : a > b ? a : (b > a ? b : (signbit(a) ? b : a)); }
static inline float vmin_f(float a, float b) { return (a != a || b != b) ? u2f(0x7FC00000u) : a < b ? a : (b < a ? b : (signbit(a) ? a : b)); }

// half precision <-> single (VCVTB / VCVTT)
static inline float h2f(uint16_t h) {
    uint32_t s = (uint32_t)(h >> 15) << 31, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    if (e == 0) {
        if (!m) return u2f(s);
        float f = ldexpf((float)m, -24);
        return s ? -f : f;
    }
    if (e == 31) return u2f(s | 0x7F800000u | (m << 13));
    return u2f(s | ((e + 112) << 23) | (m << 13));
}
static inline uint16_t f2h(float f) {
    uint32_t u = f2u(f), s = (u >> 16) & 0x8000;
    int e = (int)((u >> 23) & 0xFF) - 127 + 15;
    uint32_t m = u & 0x7FFFFF;
    if (((u >> 23) & 0xFF) == 0xFF) return (uint16_t)(s | 0x7C00 | (m ? 0x200 | (m >> 13) : 0));
    if (e >= 31) return (uint16_t)(s | 0x7C00);
    if (e <= 0) {
        if (e < -10) return (uint16_t)s;
        m |= 0x800000;
        uint32_t sh = (uint32_t)(14 - e), hm = m >> sh, rem = m & ((1u << sh) - 1), half = 1u << (sh - 1);
        if (rem > half || (rem == half && (hm & 1))) hm++;
        return (uint16_t)(s | hm);
    }
    uint32_t hm = m >> 13, rem = m & 0x1FFF;
    uint32_t r = s | ((uint32_t)e << 10) | hm;
    if (rem > 0x1000 || (rem == 0x1000 && (hm & 1))) r++;
    return (uint16_t)r;
}

// Advanced SIMD float arithmetic runs with the "standard FPSCR value": denormal inputs and results are zero.
static inline float nftz(float f) { return (f2u(f) & 0x7F800000u) == 0 ? u2f(f2u(f) & 0x80000000u) : f; }

// reciprocal / reciprocal square root estimates (NEON VRECPE / VRSQRTE, VFP has none): ARM's estimate tables
// give 8 significant bits; the exact quotient is close enough for games (they refine with VRECPS / VRSQRTS).
static inline float vrecpe_f(float a) { return 1.0f / a; }
static inline float vrsqrte_f(float a) { return 1.0f / sqrtf(a); }
static inline float vrecps_f(float a, float b) { return 2.0f - a * b; }
static inline float vrsqrts_f(float a, float b) { return (3.0f - a * b) * 0.5f; }
