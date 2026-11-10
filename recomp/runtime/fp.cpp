// Estimations ARM (FRECPE/FRSQRTE/FRECPX) et conversion double → demi-précision.
#include "cpu.h"

namespace {
// RecipEstimate / RecipSqrtEstimate du pseudo-code ARMv8.0 (estimations 8 bits)
int recip_estimate(int a) {   // 256 <= a < 512
    a = a * 2 + 1;
    int b = (1 << 19) / a;
    return (b + 1) / 2;
}
int rsqrt_estimate(int a) {   // 128 <= a < 512
    if (a < 256) a = a * 2 + 1;
    else { a = (a >> 1) << 1; a = (a + 1) * 2; }
    int b = 512;
    while ((int64_t)a * (b + 1) * (b + 1) < (int64_t(1) << 28)) b++;
    return (b + 1) / 2;
}
struct Tables {
    uint16_t rec[512], rsq[512];
    Tables() {
        for (int a = 256; a < 512; a++) rec[a] = (uint16_t)recip_estimate(a);
        for (int a = 128; a < 512; a++) rsq[a] = (uint16_t)rsqrt_estimate(a);
    }
};
const Tables& tables() { static Tables t; return t; }

template <class F> F nan_result(const CPUState* c, F a) { return fp_nan1(c, a); }

// Générique sur la largeur : E bits d'exposant, N bits de fraction
template <class U, int E, int N>
U recpe(const CPUState* c, U x, bool& handled) {
    handled = true;
    const U sign = x & ((U)1 << (E + N));
    const int bias = (1 << (E - 1)) - 1;
    const U expmask = ((U)1 << E) - 1;
    int exp = (int)((x >> N) & expmask);
    U frac = x & (((U)1 << N) - 1);
    if (exp == (int)expmask) {
        if (frac) { handled = false; return 0; }        // NaN : traité par l'appelant
        return sign;                                      // ±inf → ±0
    }
    if (exp == 0 && frac == 0) return sign | (expmask << N);  // ±0 → ±inf
    // très petit : débordement → ±inf (arrondi au plus proche)
    // |x| < 2^-(bias+1)  <=>  exp == 0 et frac < 2^(N-2)
    if (exp == 0 && !(frac >> (N - 2))) return sign | (expmask << N);
    const bool fz = (c->fpcr >> 24) & 1;
    // fraction étendue à 52 bits
    uint64_t f52 = (uint64_t)frac << (52 - N);
    if (exp == 0) {
        if (!((f52 >> 51) & 1)) { exp = -1; f52 = (f52 << 2) & ((1ull << 52) - 1); }
        else f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    int scaled = (int)(0x100 | (f52 >> 44));
    int result_exp = 2 * bias - 1 - exp;
    int est = tables().rec[scaled];
    uint64_t rf = (uint64_t)(est & 0xff) << 44;
    if (result_exp == 0) rf = (1ull << 51) | (rf >> 1);
    else if (result_exp == -1) { rf = (1ull << 50) | (rf >> 2); result_exp = 0; }
    if (fz && result_exp == 0) return sign;
    return sign | ((U)result_exp << N) | (U)(rf >> (52 - N));
}

template <class U, int E, int N>
U rsqrte(const CPUState* c, U x, bool& handled) {
    handled = true;
    const U sign = x & ((U)1 << (E + N));
    const int bias = (1 << (E - 1)) - 1;
    const U expmask = ((U)1 << E) - 1;
    int exp = (int)((x >> N) & expmask);
    U frac = x & (((U)1 << N) - 1);
    (void)c;
    if (exp == (int)expmask && frac) { handled = false; return 0; }
    if (exp == 0 && frac == 0) return sign | (expmask << N);       // ±0 → ±inf
    if (sign) { handled = false; return 1; }                         // négatif → NaN par défaut
    if (exp == (int)expmask) return 0;                               // +inf → +0
    uint64_t f52 = (uint64_t)frac << (52 - N);
    if (exp == 0) {
        while (!((f52 >> 51) & 1)) { f52 = (f52 << 1) & ((1ull << 52) - 1); exp--; }
        f52 = (f52 << 1) & ((1ull << 52) - 1);
    }
    int scaled = (exp & 1) == 0 ? (int)(0x100 | (f52 >> 44)) : (int)(0x80 | (f52 >> 45));
    int result_exp = (3 * bias - 1 - exp) / 2;
    if ((3 * bias - 1 - exp) < 0 && ((3 * bias - 1 - exp) & 1)) result_exp--;  // DIV = floor
    int est = tables().rsq[scaled];
    return ((U)result_exp << N) | ((U)(est & 0xff) << (N - 8));
}
}  // namespace

uint32_t fp_recpe32(const CPUState* c, uint32_t x) {
    bool ok;
    uint32_t r = recpe<uint32_t, 8, 23>(c, x, ok);
    return ok ? r : fbits(fp_nan1(c, f32_from(x)));
}
uint64_t fp_recpe64(const CPUState* c, uint64_t x) {
    bool ok;
    uint64_t r = recpe<uint64_t, 11, 52>(c, x, ok);
    return ok ? r : fbits(fp_nan1(c, f64_from(x)));
}
uint32_t fp_rsqrte32(const CPUState* c, uint32_t x) {
    bool ok;
    uint32_t r = rsqrte<uint32_t, 8, 23>(c, x, ok);
    if (ok) return r;
    return r == 1 ? FpT<float>::DNAN : fbits(fp_nan1(c, f32_from(x)));
}
uint64_t fp_rsqrte64(const CPUState* c, uint64_t x) {
    bool ok;
    uint64_t r = rsqrte<uint64_t, 11, 52>(c, x, ok);
    if (ok) return r;
    return r == 1 ? FpT<double>::DNAN : fbits(fp_nan1(c, f64_from(x)));
}
uint32_t fp_recpx32(const CPUState* c, uint32_t x) {
    if (f_isnan(f32_from(x))) return fbits(fp_nan1(c, f32_from(x)));
    uint32_t exp = (x >> 23) & 0xff;
    return (x & 0x80000000u) | ((exp ? (~exp & 0xff) : 0xfe) << 23);
}
uint64_t fp_recpx64(const CPUState* c, uint64_t x) {
    if (f_isnan(f64_from(x))) return fbits(fp_nan1(c, f64_from(x)));
    uint64_t exp = (x >> 52) & 0x7ff;
    return (x & (1ull << 63)) | ((exp ? (~exp & 0x7ff) : 0x7fe) << 52);
}

uint16_t f64_to_f16(const CPUState* c, double d) {
    uint64_t b = fbits(d);
    uint16_t sign = (uint16_t)((b >> 48) & 0x8000);
    int exp = (int)((b >> 52) & 0x7ff);
    uint64_t frac = b & ((1ull << 52) - 1);
    if (exp == 0x7ff) {
        if (frac) return fpcr_dn(c) ? 0x7e00 : (uint16_t)(sign | 0x7e00 | (frac >> 42));
        return sign | 0x7c00;
    }
    if (exp == 0) return sign;   // zéro ou sous-normal double : trop petit
    int e = exp - 1023 + 15;
    if (e >= 31) return sign | 0x7c00;
    uint64_t m = (1ull << 52) | frac;
    if (e <= 0) {
        int shift = 1051 - exp;
        if (shift > 60) return sign;
        uint64_t q = m >> shift, r = m & ((1ull << shift) - 1), half = 1ull << (shift - 1);
        if (r > half || (r == half && (q & 1))) q++;
        return (uint16_t)(sign | q);
    }
    uint64_t q = frac >> 42, r = frac & ((1ull << 42) - 1), half = 1ull << 41;
    if (r > half || (r == half && (q & 1))) {
        q++;
        if (q == 0x400) { q = 0; e++; if (e >= 31) return sign | 0x7c00; }
    }
    return (uint16_t)(sign | (e << 10) | q);
}
