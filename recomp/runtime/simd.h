// Helpers FP / AdvSIMD pour le code généré (inclus par cpu.h).
//
// Sémantique ARM exacte là où x86 diffère :
//  * propagation des NaN (FPProcessNaNs : SNaN prioritaires, puis 1er opérande ; NaN par défaut
//    0x7FC00000 positif au lieu du « real indefinite » négatif de x86 ; FPCR.DN) ;
//  * FMIN/FMAX/FMINNM/FMAXNM (zéros signés, NaN silencieux ignorés par *NM) ;
//  * conversions flottant → entier saturantes (NaN → 0) ;
//  * FMADD/FMLA fusionnés (fma), FMUL+FADD séparés jamais contractés (-ffp-contract=off) ;
//  * FRECPE/FRSQRTE : estimations 8 bits du pseudo-code ARM (tables).
#pragma once
#include <limits>
#include <type_traits>

static inline V128 v128_zext8(uint8_t x) { V128 r{}; r.u8[0] = x; return r; }
static inline V128 v128_zext16(uint16_t x) { V128 r{}; r.u16[0] = x; return r; }
static inline V128 v128_zext32(uint32_t x) { V128 r{}; r.u32[0] = x; return r; }
static inline V128 v128_zext64(uint64_t x) { V128 r{}; r.u64[0] = x; return r; }
static inline V128 v128_f32(float x) { V128 r{}; r.f32[0] = x; return r; }
static inline V128 v128_f64(double x) { V128 r{}; r.f64[0] = x; return r; }
static inline V128 v128_u64x2(uint64_t lo, uint64_t hi) { V128 r; r.u64[0] = lo; r.u64[1] = hi; return r; }

template <class F> struct FpT;
template <> struct FpT<float> {
    typedef uint32_t U;
    static constexpr U SIGN = 0x80000000u, EXP = 0x7f800000u, FRAC = 0x007fffffu, QBIT = 0x00400000u,
                       DNAN = 0x7fc00000u;
};
template <> struct FpT<double> {
    typedef uint64_t U;
    static constexpr U SIGN = 0x8000000000000000ull, EXP = 0x7ff0000000000000ull,
                       FRAC = 0x000fffffffffffffull, QBIT = 0x0008000000000000ull,
                       DNAN = 0x7ff8000000000000ull;
};
template <class F> static inline typename FpT<F>::U fbits(F f) { typename FpT<F>::U u; memcpy(&u, &f, sizeof u); return u; }
template <class F> static inline F ffrom(typename FpT<F>::U u) { F f; memcpy(&f, &u, sizeof f); return f; }
static inline float f32_from(uint32_t u) { return ffrom<float>(u); }
static inline double f64_from(uint64_t u) { return ffrom<double>(u); }
template <class F> static inline bool f_isnan(F a) { return a != a; }
template <class F> static inline bool f_issnan(F a) {
    auto u = fbits(a);
    return (u & FpT<F>::EXP) == FpT<F>::EXP && (u & FpT<F>::FRAC) && !(u & FpT<F>::QBIT);
}
template <class F> static inline bool f_isinf(F a) { return (fbits(a) & ~FpT<F>::SIGN) == FpT<F>::EXP; }
template <class F> static inline bool f_iszero(F a) { return (fbits(a) & ~FpT<F>::SIGN) == 0; }
template <class F> static inline bool f_sign(F a) { return (fbits(a) & FpT<F>::SIGN) != 0; }
template <class F> static inline F f_quiet(F a) { return ffrom<F>(fbits(a) | FpT<F>::QBIT); }
template <class F> static inline F f_dnan() { return ffrom<F>(FpT<F>::DNAN); }
template <class F> static inline F f_neg(F a) { return ffrom<F>(fbits(a) ^ FpT<F>::SIGN); }
template <class F> static inline F f_abs(F a) { return ffrom<F>(fbits(a) & ~FpT<F>::SIGN); }
static inline bool fpcr_dn(const CPUState* c) { return (c->fpcr >> 25) & 1; }

template <class F> static inline F fp_nan1(const CPUState* c, F a) {
    if (fpcr_dn(c) || !f_isnan(a)) return f_dnan<F>();
    return f_quiet(a);
}
template <class F> static inline F fp_nan2(const CPUState* c, F a, F b) {
    if (fpcr_dn(c)) return f_dnan<F>();
    if (f_issnan(a)) return f_quiet(a);
    if (f_issnan(b)) return f_quiet(b);
    if (f_isnan(a)) return a;
    if (f_isnan(b)) return b;
    return f_dnan<F>();
}

#define FP_BIN(name, expr)                                                     \
    template <class F> static inline F name(const CPUState* c, F a, F b) {     \
        F r = (expr);                                                          \
        if (__builtin_expect(r != r, 0)) r = fp_nan2(c, a, b);                 \
        return r;                                                              \
    }
FP_BIN(fp_add, a + b)
FP_BIN(fp_sub, a - b)
FP_BIN(fp_mul, a * b)
FP_BIN(fp_div, a / b)
#undef FP_BIN

template <class F> static inline F fp_nmul(const CPUState* c, F a, F b) { return f_neg(fp_mul(c, a, b)); }
template <class F> static inline F fp_abd(const CPUState* c, F a, F b) { return f_abs(fp_sub(c, a, b)); }

template <class F> static inline F fp_sqrt(const CPUState* c, F a) {
    F r = std::sqrt(a);
    if (__builtin_expect(r != r, 0)) r = fp_nan1(c, a);
    return r;
}

// FMULX : comme FMUL mais 0 × ∞ = ±2
template <class F> static inline F fp_mulx(const CPUState* c, F a, F b) {
    if ((f_isinf(a) && f_iszero(b)) || (f_iszero(a) && f_isinf(b))) {
        if (f_isnan(a) || f_isnan(b)) return fp_nan2(c, a, b);
        return (f_sign(a) != f_sign(b)) ? (F)-2.0 : (F)2.0;
    }
    return fp_mul(c, a, b);
}

template <class F> static inline F fp_max(const CPUState* c, F a, F b) {
    if (f_isnan(a) || f_isnan(b)) return fp_nan2(c, a, b);
    if (f_iszero(a) && f_iszero(b)) return f_sign(a) ? b : a;
    return a > b ? a : b;
}
template <class F> static inline F fp_min(const CPUState* c, F a, F b) {
    if (f_isnan(a) || f_isnan(b)) return fp_nan2(c, a, b);
    if (f_iszero(a) && f_iszero(b)) return f_sign(a) ? a : b;
    return a < b ? a : b;
}
template <class F> static inline F fp_maxnm(const CPUState* c, F a, F b) {
    bool qa = f_isnan(a) && !f_issnan(a), qb = f_isnan(b) && !f_issnan(b);
    if (qa && !f_isnan(b)) return b;
    if (qb && !f_isnan(a)) return a;
    return fp_max(c, a, b);
}
template <class F> static inline F fp_minnm(const CPUState* c, F a, F b) {
    bool qa = f_isnan(a) && !f_issnan(a), qb = f_isnan(b) && !f_issnan(b);
    if (qa && !f_isnan(b)) return b;
    if (qb && !f_isnan(a)) return a;
    return fp_min(c, a, b);
}

// addend + x*y, arrondi unique (FPMulAdd)
template <class F> static inline F fp_muladd(const CPUState* c, F ad, F x, F y) {
    F r = std::fma(x, y, ad);
    if (__builtin_expect(r != r, 0)) {
        if (fpcr_dn(c)) return f_dnan<F>();
        bool infzero = (f_isinf(x) && f_iszero(y)) || (f_iszero(x) && f_isinf(y));
        if (f_issnan(ad)) return f_quiet(ad);
        if (f_issnan(x)) return f_quiet(x);
        if (f_issnan(y)) return f_quiet(y);
        if (f_isnan(ad) && infzero) return f_dnan<F>();
        if (f_isnan(ad)) return ad;
        if (f_isnan(x)) return x;
        if (f_isnan(y)) return y;
        return f_dnan<F>();
    }
    return r;
}

// FRECPS : 2 - a*b ; FRSQRTS : (3 - a*b) / 2
template <class F> static inline F fp_recps(const CPUState* c, F a, F b) {
    a = f_neg(a);
    if (f_isnan(a) || f_isnan(b)) return fp_nan2(c, a, b);
    if ((f_isinf(a) && f_iszero(b)) || (f_iszero(a) && f_isinf(b))) return (F)2.0;
    if (f_isinf(a) || f_isinf(b)) return (f_sign(a) != f_sign(b)) ? -std::numeric_limits<F>::infinity()
                                                                  : std::numeric_limits<F>::infinity();
    return std::fma(a, b, (F)2.0);
}
template <class F> static inline F fp_rsqrts(const CPUState* c, F a, F b) {
    a = f_neg(a);
    if (f_isnan(a) || f_isnan(b)) return fp_nan2(c, a, b);
    if ((f_isinf(a) && f_iszero(b)) || (f_iszero(a) && f_isinf(b))) return (F)1.5;
    if (f_isinf(a) || f_isinf(b)) return (f_sign(a) != f_sign(b)) ? -std::numeric_limits<F>::infinity()
                                                                  : std::numeric_limits<F>::infinity();
    // (3 + a*b) / 2 avec un seul arrondi : fma(a, b, 3) puis division exacte par 2
    return std::fma(a, b, (F)3.0) * (F)0.5;
}

uint32_t fp_recpe32(const CPUState* c, uint32_t x);
uint64_t fp_recpe64(const CPUState* c, uint64_t x);
uint32_t fp_rsqrte32(const CPUState* c, uint32_t x);
uint64_t fp_rsqrte64(const CPUState* c, uint64_t x);
uint32_t fp_recpx32(const CPUState* c, uint32_t x);
uint64_t fp_recpx64(const CPUState* c, uint64_t x);
static inline float fp_recpe(const CPUState* c, float a) { return f32_from(fp_recpe32(c, fbits(a))); }
static inline double fp_recpe(const CPUState* c, double a) { return f64_from(fp_recpe64(c, fbits(a))); }
static inline float fp_rsqrte(const CPUState* c, float a) { return f32_from(fp_rsqrte32(c, fbits(a))); }
static inline double fp_rsqrte(const CPUState* c, double a) { return f64_from(fp_rsqrte64(c, fbits(a))); }
static inline float fp_recpx(const CPUState* c, float a) { return f32_from(fp_recpx32(c, fbits(a))); }
static inline double fp_recpx(const CPUState* c, double a) { return f64_from(fp_recpx64(c, fbits(a))); }

// Comparaison → NZCV
template <class F> static inline void fp_cmp(CPUState* c, F a, F b) {
    if (f_isnan(a) || f_isnan(b)) { c->nf = 0; c->zf = 0; c->cf = 1; c->vf = 1; }
    else if (a == b) { c->nf = 0; c->zf = 1; c->cf = 1; c->vf = 0; }
    else if (a < b) { c->nf = 1; c->zf = 0; c->cf = 0; c->vf = 0; }
    else { c->nf = 0; c->zf = 0; c->cf = 1; c->vf = 0; }
}

// Arrondis : 0 = N (pair), 1 = P (+inf), 2 = M (-inf), 3 = Z, 4 = A (loin de zéro), 5 = courant
static inline float f_round(float a, int m) {
    switch (m) {
        case 0: return __builtin_roundevenf(a);
        case 1: return __builtin_ceilf(a);
        case 2: return __builtin_floorf(a);
        case 3: return __builtin_truncf(a);
        case 4: return __builtin_roundf(a);
        default: return __builtin_nearbyintf(a);
    }
}
static inline double f_round(double a, int m) {
    switch (m) {
        case 0: return __builtin_roundeven(a);
        case 1: return __builtin_ceil(a);
        case 2: return __builtin_floor(a);
        case 3: return __builtin_trunc(a);
        case 4: return __builtin_round(a);
        default: return __builtin_nearbyint(a);
    }
}
template <class F> static inline F fp_rint(const CPUState* c, F a, int m) {
    if (f_isnan(a)) return fp_nan1(c, a);
    return f_round(a, m);
}
static inline int fpcr_rmode(const CPUState* c) {
    static const int map[4] = {0, 1, 2, 3};
    return map[(c->fpcr >> 22) & 3];
}

template <class I, class F> static inline I fcvt_int(F a, int m) {
    if (a != a) return 0;
    F r = f_round(a, m);
    if (r >= (F)std::numeric_limits<I>::max()) return std::numeric_limits<I>::max();
    if (r <= (F)std::numeric_limits<I>::min()) return std::numeric_limits<I>::min();
    return (I)r;
}
// conversion vers virgule fixe : a * 2^fbits puis conversion (multiplication exacte)
template <class I, class F> static inline I fcvt_fixed(F a, int fbits_, int m) {
    return fcvt_int<I, F>(a * (F)std::ldexp((F)1, fbits_), m);
}

// Demi-précision (IEEE, FPCR.AHP non géré)
static inline float f16_to_f32(uint16_t h) {
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, f = h & 0x3ff;
    if (e == 0x1f) return f32_from(s | 0x7f800000u | (f << 13) | (f ? 0x00400000u : 0));
    if (e == 0) {
        if (!f) return f32_from(s);
        float v = (float)f * (1.0f / 16777216.0f);
        return s ? -v : v;
    }
    return f32_from(s | ((e + 112) << 23) | (f << 13));
}
static inline double f16_to_f64(uint16_t h) { return (double)f16_to_f32(h); }
uint16_t f64_to_f16(const CPUState* c, double d);
static inline uint16_t f32_to_f16(const CPUState* c, float f) { return f64_to_f16(c, (double)f); }

template <class F> static inline F fp_cvt_nan(const CPUState* c, F r) { return fpcr_dn(c) ? f_dnan<F>() : r; }
static inline double fcvt_s2d(const CPUState* c, float a) {
    double r = (double)a;
    return (r != r) ? fp_cvt_nan(c, f_quiet(r)) : r;
}
static inline float fcvt_d2s(const CPUState* c, double a) {
    float r = (float)a;
    return (r != r) ? fp_cvt_nan(c, f_quiet(r)) : r;
}

// Entiers saturants
template <class T> static inline T sat_add_s(T a, T b) { T r; return __builtin_add_overflow(a, b, &r) ? (b > 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min()) : r; }
template <class T> static inline T sat_sub_s(T a, T b) { T r; return __builtin_sub_overflow(a, b, &r) ? (b < 0 ? std::numeric_limits<T>::max() : std::numeric_limits<T>::min()) : r; }
template <class T> static inline T sat_add_u(T a, T b) { T r = (T)(a + b); return r < a ? std::numeric_limits<T>::max() : r; }
template <class T> static inline T sat_sub_u(T a, T b) { return a > b ? (T)(a - b) : (T)0; }
template <class D, class S> static inline D sat_narrow_s(S v) {
    if (v > (S)std::numeric_limits<D>::max()) return std::numeric_limits<D>::max();
    if (v < (S)std::numeric_limits<D>::min()) return std::numeric_limits<D>::min();
    return (D)v;
}
template <class D, class S> static inline D sat_narrow_u(S v) {
    if (v > (S)std::numeric_limits<D>::max()) return std::numeric_limits<D>::max();
    return (D)v;
}
template <class D, class S> static inline D sat_narrow_su(S v) {   // signé → non signé (SQXTUN)
    if (v < 0) return 0;
    if ((typename std::make_unsigned<S>::type)v > std::numeric_limits<D>::max()) return std::numeric_limits<D>::max();
    return (D)v;
}

// Décalages vectoriels par registre (SSHL/USHL : quantité signée sur l'octet bas)
template <class T> static inline T vshl_u(T a, int8_t s) {
    const int bits = sizeof(T) * 8;
    if (s >= bits || s <= -bits) return 0;
    return s >= 0 ? (T)(a << s) : (T)(a >> -s);
}
template <class T> static inline T vshl_s(T a, int8_t s) {
    const int bits = sizeof(T) * 8;
    if (s >= bits) return 0;
    if (s <= -bits) return a < 0 ? (T)-1 : 0;
    return s >= 0 ? (T)((typename std::make_unsigned<T>::type)a << s) : (T)(a >> -s);
}
template <class T> static inline T vrshl_u(T a, int8_t s) {
    const int bits = sizeof(T) * 8;
    if (s >= bits || s < -bits) return 0;
    if (s >= 0) return (T)(a << s);
    unsigned __int128 v = (unsigned __int128)a + ((unsigned __int128)1 << (-s - 1));
    return (T)(v >> -s);
}
template <class T> static inline T vrshl_s(T a, int8_t s) {
    const int bits = sizeof(T) * 8;
    if (s >= bits || s < -bits) return 0;
    if (s >= 0) return (T)((typename std::make_unsigned<T>::type)a << s);
    __int128 v = (__int128)a + ((__int128)1 << (-s - 1));
    return (T)(v >> -s);
}

// Décalages saturants par registre (SQSHL/UQSHL) : quantité signée, droite sans arrondi
template <class T> static inline T vqshl_s(T a, int8_t s) {
    const int bits = sizeof(T) * 8;
    if (s >= 0) {
        if (a == 0) return 0;
        if (s >= bits) return a < 0 ? std::numeric_limits<T>::min() : std::numeric_limits<T>::max();
        __int128 r = (__int128)a * ((__int128)1 << s);
        if (r > (__int128)std::numeric_limits<T>::max()) return std::numeric_limits<T>::max();
        if (r < (__int128)std::numeric_limits<T>::min()) return std::numeric_limits<T>::min();
        return (T)r;
    }
    int n = -s;
    if (n >= bits) return a < 0 ? (T)-1 : (T)0;
    return (T)(a >> n);
}
template <class T> static inline T vqshl_u(T a, int8_t s) {
    const int bits = sizeof(T) * 8;
    if (s >= 0) {
        if (a == 0) return 0;
        if (s >= bits) return std::numeric_limits<T>::max();
        unsigned __int128 r = (unsigned __int128)a << s;
        return r > (unsigned __int128)std::numeric_limits<T>::max() ? std::numeric_limits<T>::max() : (T)r;
    }
    int n = -s;
    if (n >= bits) return 0;
    return (T)(a >> n);
}
