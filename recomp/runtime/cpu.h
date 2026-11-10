// Runtime du code recompilé ARM64 → C++ : état CPU invité et primitives utilisées par le code
// généré. Tout est `static inline` pour que le compilateur fonde les helpers dans le code émis.
//
// Modèle mémoire : adresse invitée == adresse hôte (+ RECOMP_MEM_OFFSET, 0 par défaut). Le runtime
// réserve les modules à leurs adresses fixes (cf. a64/module.py, DEFAULT_BASES).
//
// Options de compilation :
//   RECOMP_TRACE      trace instruction par instruction + journal des écritures (banc de test)
//   RECOMP_CHECK_RET  pile d'ombre : vérifie que chaque `ret` revient bien à l'appelant
//   RECOMP_STRICT_NAN propagation des NaN conforme ARM (défaut : activé)
#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef RECOMP_MEM_OFFSET
#define RECOMP_MEM_OFFSET 0
#endif
#ifndef RECOMP_STRICT_NAN
#define RECOMP_STRICT_NAN 1
#endif

#if defined(_WIN32)
#define RECOMP_EXPORT extern "C" __declspec(dllexport)
#else
#define RECOMP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

struct alignas(16) V128 {
    union {
        uint8_t u8[16];
        int8_t s8[16];
        uint16_t u16[8];
        int16_t s16[8];
        uint32_t u32[4];
        int32_t s32[4];
        uint64_t u64[2];
        int64_t s64[2];
        float f32[4];
        double f64[2];
    };
};
static_assert(sizeof(V128) == 16, "V128");

// Disposition figée : le banc Python (bench/harness.py) la reproduit avec ctypes.
struct alignas(16) CPUState {
    uint64_t x[31];        // X0..X30
    uint64_t sp;
    uint64_t pc;           // dernier pc connu (diagnostic)
    uint32_t nf, zf, cf, vf; // drapeaux NZCV (0/1)
    uint32_t fpcr, fpsr;
    uint64_t tpidr_el0, tpidrro_el0;
    uint64_t excl_addr, excl_val0, excl_val1;
    uint32_t excl_size, excl_valid;
    uint64_t user;         // libre pour le runtime (contexte de thread)
    uint64_t pad;
    V128 v[32];
};
static_assert(offsetof(CPUState, sp) == 248, "layout");
static_assert(offsetof(CPUState, nf) == 264, "layout");
static_assert(offsetof(CPUState, v) == 352, "layout");
static_assert(sizeof(CPUState) == 352 + 512, "layout");

typedef void (*RecompFn)(CPUState*);

// Appels terminaux garantis (b/br vers une autre fonction) : évite toute croissance de la pile
// hôte sur les chaînes d'appels terminaux de l'invité.
#if defined(__clang__)
#define MUSTTAIL [[clang::musttail]]
#elif defined(__GNUC__) && __GNUC__ >= 15
#define MUSTTAIL [[gnu::musttail]]
#else
#define MUSTTAIL
#endif

// ------------------------------------------------------------------------------------------
// Services du runtime (runtime.cpp)
// ------------------------------------------------------------------------------------------
void rt_call(CPUState* ctx, uint64_t target);          // appel indirect (blr)
void rt_tailcall(CPUState* ctx, uint64_t target);      // saut indirect hors fonction (br)
void rt_svc(CPUState* ctx, uint32_t imm, uint64_t pc);
[[noreturn]] void rt_trap(CPUState* ctx, uint64_t pc, uint32_t word);
[[noreturn]] void rt_undefined(CPUState* ctx, uint64_t pc, uint32_t word);
void rt_unimpl(CPUState* ctx, uint64_t pc, uint32_t word);
uint64_t rt_mrs(CPUState* ctx, uint32_t enc, uint64_t pc);
void rt_msr(CPUState* ctx, uint32_t enc, uint64_t val, uint64_t pc);
void rt_set_fpcr(CPUState* ctx, uint32_t v);
uint64_t rt_cntvct();
uint64_t rt_cntfrq();
RecompFn rt_lookup(uint64_t addr);
void rt_register_functions(const uint64_t* addrs, const RecompFn* fns, size_t n);

#ifdef RECOMP_TRACE
void rt_trace(CPUState* ctx, uint64_t pc);
void rt_trace_write(uint64_t addr, const void* data, uint32_t size);
#define TRACE(pc) rt_trace(ctx, pc)
#define TRACE_WRITE(a, p, n) rt_trace_write(a, p, n)
#else
#define TRACE(pc) ((void)0)
#define TRACE_WRITE(a, p, n) ((void)0)
#endif

#ifdef RECOMP_CHECK_RET
void rt_push_ret(uint64_t ret);
void rt_check_ret(CPUState* ctx, uint64_t target, uint64_t pc);
#define PUSH_RET(r) rt_push_ret(r)
#define CHECK_RET(t, pc) rt_check_ret(ctx, t, pc)
#else
#define PUSH_RET(r) ((void)0)
#define CHECK_RET(t, pc) ((void)0)
#endif

// ------------------------------------------------------------------------------------------
// Mémoire invitée
// ------------------------------------------------------------------------------------------
#define GPTR(a) ((void*)(uintptr_t)((uint64_t)(a) + (uint64_t)RECOMP_MEM_OFFSET))

static inline uint8_t rd8(uint64_t a) { return *(const uint8_t*)GPTR(a); }
static inline uint16_t rd16(uint64_t a) { uint16_t v; memcpy(&v, GPTR(a), 2); return v; }
static inline uint32_t rd32(uint64_t a) { uint32_t v; memcpy(&v, GPTR(a), 4); return v; }
static inline uint64_t rd64(uint64_t a) { uint64_t v; memcpy(&v, GPTR(a), 8); return v; }
static inline V128 rd128(uint64_t a) { V128 v; memcpy(&v, GPTR(a), 16); return v; }
static inline void wr8(uint64_t a, uint8_t v) { TRACE_WRITE(a, &v, 1); *(uint8_t*)GPTR(a) = v; }
static inline void wr16(uint64_t a, uint16_t v) { TRACE_WRITE(a, &v, 2); memcpy(GPTR(a), &v, 2); }
static inline void wr32(uint64_t a, uint32_t v) { TRACE_WRITE(a, &v, 4); memcpy(GPTR(a), &v, 4); }
static inline void wr64(uint64_t a, uint64_t v) { TRACE_WRITE(a, &v, 8); memcpy(GPTR(a), &v, 8); }
static inline void wr128(uint64_t a, V128 v) { TRACE_WRITE(a, &v, 16); memcpy(GPTR(a), &v, 16); }

static inline void rt_dc_zva(uint64_t a) {
    a &= ~UINT64_C(63);
#ifdef RECOMP_TRACE
    static const uint8_t z[64] = {};
    TRACE_WRITE(a, z, 64);
#endif
    memset(GPTR(a), 0, 64);
}

// ------------------------------------------------------------------------------------------
// Drapeaux et arithmétique entière
// ------------------------------------------------------------------------------------------
static inline uint64_t adds64(CPUState* ctx, uint64_t a, uint64_t b) {
    uint64_t r = a + b;
    ctx->nf = (uint32_t)(r >> 63); ctx->zf = r == 0; ctx->cf = r < a;
    ctx->vf = (uint32_t)(((~(a ^ b)) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adds32(CPUState* ctx, uint32_t a, uint32_t b) {
    uint32_t r = a + b;
    ctx->nf = r >> 31; ctx->zf = r == 0; ctx->cf = r < a;
    ctx->vf = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static inline uint64_t subs64(CPUState* ctx, uint64_t a, uint64_t b) {
    uint64_t r = a - b;
    ctx->nf = (uint32_t)(r >> 63); ctx->zf = r == 0; ctx->cf = a >= b;
    ctx->vf = (uint32_t)(((a ^ b) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t subs32(CPUState* ctx, uint32_t a, uint32_t b) {
    uint32_t r = a - b;
    ctx->nf = r >> 31; ctx->zf = r == 0; ctx->cf = a >= b;
    ctx->vf = ((a ^ b) & (a ^ r)) >> 31;
    return r;
}
static inline uint64_t adcs64(CPUState* ctx, uint64_t a, uint64_t b, uint32_t carry) {
    unsigned __int128 t = (unsigned __int128)a + b + carry;
    uint64_t r = (uint64_t)t;
    ctx->nf = (uint32_t)(r >> 63); ctx->zf = r == 0; ctx->cf = (uint32_t)(t >> 64);
    ctx->vf = (uint32_t)(((~(a ^ b)) & (a ^ r)) >> 63);
    return r;
}
static inline uint32_t adcs32(CPUState* ctx, uint32_t a, uint32_t b, uint32_t carry) {
    uint64_t t = (uint64_t)a + b + carry;
    uint32_t r = (uint32_t)t;
    ctx->nf = r >> 31; ctx->zf = r == 0; ctx->cf = (uint32_t)(t >> 32);
    ctx->vf = ((~(a ^ b)) & (a ^ r)) >> 31;
    return r;
}
static inline uint64_t logic64(CPUState* ctx, uint64_t r) {
    ctx->nf = (uint32_t)(r >> 63); ctx->zf = r == 0; ctx->cf = 0; ctx->vf = 0;
    return r;
}
static inline uint32_t logic32(CPUState* ctx, uint32_t r) {
    ctx->nf = r >> 31; ctx->zf = r == 0; ctx->cf = 0; ctx->vf = 0;
    return r;
}
static inline uint64_t rt_get_nzcv(CPUState* ctx) {
    return ((uint64_t)ctx->nf << 31) | ((uint64_t)ctx->zf << 30) | ((uint64_t)ctx->cf << 29) |
           ((uint64_t)ctx->vf << 28);
}
static inline void rt_set_nzcv(CPUState* ctx, uint64_t v) {
    ctx->nf = (v >> 31) & 1; ctx->zf = (v >> 30) & 1; ctx->cf = (v >> 29) & 1; ctx->vf = (v >> 28) & 1;
}

static inline uint64_t ror64(uint64_t a, unsigned s) { s &= 63; return s ? (a >> s) | (a << (64 - s)) : a; }
static inline uint32_t ror32(uint32_t a, unsigned s) { s &= 31; return s ? (a >> s) | (a << (32 - s)) : a; }
static inline uint64_t clz64(uint64_t a) { return a ? (uint64_t)__builtin_clzll(a) : 64; }
static inline uint32_t clz32(uint32_t a) { return a ? (uint32_t)__builtin_clz(a) : 32; }
static inline uint64_t ctz64(uint64_t a) { return a ? (uint64_t)__builtin_ctzll(a) : 64; }
static inline uint32_t ctz32(uint32_t a) { return a ? (uint32_t)__builtin_ctz(a) : 32; }
static inline uint64_t cls64(uint64_t a) {
    uint64_t y = (a >> 1) ^ (a & 0x7fffffffffffffffull);
    return y ? clz64(y) - 1 : 63;
}
static inline uint32_t cls32(uint32_t a) {
    uint32_t y = (a >> 1) ^ (a & 0x7fffffffu);
    return y ? clz32(y) - 1 : 31;
}
static inline uint64_t rbit64(uint64_t a) {
    a = ((a >> 1) & 0x5555555555555555ull) | ((a & 0x5555555555555555ull) << 1);
    a = ((a >> 2) & 0x3333333333333333ull) | ((a & 0x3333333333333333ull) << 2);
    a = ((a >> 4) & 0x0f0f0f0f0f0f0f0full) | ((a & 0x0f0f0f0f0f0f0f0full) << 4);
    return __builtin_bswap64(a);
}
static inline uint32_t rbit32(uint32_t a) { return (uint32_t)(rbit64(a) >> 32); }
static inline uint64_t rev16_64(uint64_t a) {
    return ((a >> 8) & 0x00ff00ff00ff00ffull) | ((a & 0x00ff00ff00ff00ffull) << 8);
}
static inline uint32_t rev16_32(uint32_t a) { return ((a >> 8) & 0x00ff00ffu) | ((a & 0x00ff00ffu) << 8); }
static inline uint64_t rev32_64(uint64_t a) {
    return ((uint64_t)__builtin_bswap32((uint32_t)(a >> 32)) << 32) | __builtin_bswap32((uint32_t)a);
}
static inline uint64_t udiv64(uint64_t a, uint64_t b) { return b ? a / b : 0; }
static inline uint32_t udiv32(uint32_t a, uint32_t b) { return b ? a / b : 0; }
static inline uint64_t sdiv64(uint64_t a, uint64_t b) {
    if (!b) return 0;
    if ((int64_t)a == INT64_MIN && (int64_t)b == -1) return a;
    return (uint64_t)((int64_t)a / (int64_t)b);
}
static inline uint32_t sdiv32(uint32_t a, uint32_t b) {
    if (!b) return 0;
    if ((int32_t)a == INT32_MIN && (int32_t)b == -1) return a;
    return (uint32_t)((int32_t)a / (int32_t)b);
}
static inline uint64_t smulh64(uint64_t a, uint64_t b) {
    return (uint64_t)(((__int128)(int64_t)a * (__int128)(int64_t)b) >> 64);
}
static inline uint64_t umulh64(uint64_t a, uint64_t b) {
    return (uint64_t)(((unsigned __int128)a * b) >> 64);
}

uint32_t crc32_update(uint32_t crc, uint64_t data, int bytes, bool castagnoli);
static inline uint32_t crc32_8(uint32_t c, uint32_t d) { return crc32_update(c, d, 1, false); }
static inline uint32_t crc32_16(uint32_t c, uint32_t d) { return crc32_update(c, d, 2, false); }
static inline uint32_t crc32_32(uint32_t c, uint32_t d) { return crc32_update(c, d, 4, false); }
static inline uint32_t crc32_64(uint32_t c, uint64_t d) { return crc32_update(c, d, 8, false); }
static inline uint32_t crc32c_8(uint32_t c, uint32_t d) { return crc32_update(c, d, 1, true); }
static inline uint32_t crc32c_16(uint32_t c, uint32_t d) { return crc32_update(c, d, 2, true); }
static inline uint32_t crc32c_32(uint32_t c, uint32_t d) { return crc32_update(c, d, 4, true); }
static inline uint32_t crc32c_64(uint32_t c, uint64_t d) { return crc32_update(c, d, 8, true); }

// ------------------------------------------------------------------------------------------
// Atomiques LSE / acquire-release → atomiques natives x86 (toutes en seq_cst : même coût sur x86)
// ------------------------------------------------------------------------------------------
#define AT_P(T, a) ((T*)GPTR(a))
#ifdef RECOMP_TRACE
#define AT_LOG(a, T) do { T _v = *AT_P(T, a); TRACE_WRITE(a, &_v, sizeof(T)); } while (0)
#else
#define AT_LOG(a, T) ((void)0)
#endif

#define DEF_ATOMICS(BITS, T, ST)                                                                   \
    static inline T at_load##BITS(uint64_t a) { return __atomic_load_n(AT_P(T, a), __ATOMIC_SEQ_CST); } \
    static inline void at_store##BITS(uint64_t a, T v) {                                         \
        TRACE_WRITE(a, &v, sizeof(T)); __atomic_store_n(AT_P(T, a), v, __ATOMIC_SEQ_CST); }      \
    static inline T at_cas##BITS(uint64_t a, T expected, T desired) {                            \
        T e = expected;                                                                          \
        if (__atomic_compare_exchange_n(AT_P(T, a), &e, desired, false, __ATOMIC_SEQ_CST,        \
                                        __ATOMIC_SEQ_CST)) AT_LOG(a, T);                         \
        return e; }                                                                              \
    static inline T at_swp##BITS(uint64_t a, T v) {                                              \
        T o = __atomic_exchange_n(AT_P(T, a), v, __ATOMIC_SEQ_CST); AT_LOG(a, T); return o; }    \
    static inline T at_add##BITS(uint64_t a, T v) {                                              \
        T o = __atomic_fetch_add(AT_P(T, a), v, __ATOMIC_SEQ_CST); AT_LOG(a, T); return o; }     \
    static inline T at_clr##BITS(uint64_t a, T v) {                                              \
        T o = __atomic_fetch_and(AT_P(T, a), (T)~v, __ATOMIC_SEQ_CST); AT_LOG(a, T); return o; } \
    static inline T at_eor##BITS(uint64_t a, T v) {                                              \
        T o = __atomic_fetch_xor(AT_P(T, a), v, __ATOMIC_SEQ_CST); AT_LOG(a, T); return o; }     \
    static inline T at_set##BITS(uint64_t a, T v) {                                              \
        T o = __atomic_fetch_or(AT_P(T, a), v, __ATOMIC_SEQ_CST); AT_LOG(a, T); return o; }      \
    static inline T at_smax##BITS(uint64_t a, T v) {                                             \
        T o = __atomic_load_n(AT_P(T, a), __ATOMIC_RELAXED);                                     \
        while (!__atomic_compare_exchange_n(AT_P(T, a), &o, (ST)o > (ST)v ? o : v, true,         \
                                            __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {}              \
        AT_LOG(a, T); return o; }                                                                \
    static inline T at_smin##BITS(uint64_t a, T v) {                                             \
        T o = __atomic_load_n(AT_P(T, a), __ATOMIC_RELAXED);                                     \
        while (!__atomic_compare_exchange_n(AT_P(T, a), &o, (ST)o < (ST)v ? o : v, true,         \
                                            __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {}              \
        AT_LOG(a, T); return o; }                                                                \
    static inline T at_umax##BITS(uint64_t a, T v) {                                             \
        T o = __atomic_load_n(AT_P(T, a), __ATOMIC_RELAXED);                                     \
        while (!__atomic_compare_exchange_n(AT_P(T, a), &o, o > v ? o : v, true,                 \
                                            __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {}              \
        AT_LOG(a, T); return o; }                                                                \
    static inline T at_umin##BITS(uint64_t a, T v) {                                             \
        T o = __atomic_load_n(AT_P(T, a), __ATOMIC_RELAXED);                                     \
        while (!__atomic_compare_exchange_n(AT_P(T, a), &o, o < v ? o : v, true,                 \
                                            __ATOMIC_SEQ_CST, __ATOMIC_RELAXED)) {}              \
        AT_LOG(a, T); return o; }                                                                \
    static inline uint64_t rt_ldxr##BITS(CPUState* ctx, uint64_t a) {                            \
        T v = __atomic_load_n(AT_P(T, a), __ATOMIC_SEQ_CST);                                     \
        ctx->excl_addr = a; ctx->excl_val0 = v; ctx->excl_size = sizeof(T); ctx->excl_valid = 1; \
        return (uint64_t)v; }                                                                    \
    static inline uint32_t rt_stxr##BITS(CPUState* ctx, uint64_t a, T v) {                       \
        if (!ctx->excl_valid || ctx->excl_addr != a || ctx->excl_size != sizeof(T)) {            \
            ctx->excl_valid = 0; return 1; }                                                     \
        ctx->excl_valid = 0;                                                                     \
        T e = (T)ctx->excl_val0;                                                                 \
        if (!__atomic_compare_exchange_n(AT_P(T, a), &e, v, false, __ATOMIC_SEQ_CST,             \
                                         __ATOMIC_SEQ_CST)) return 1;                            \
        AT_LOG(a, T); return 0; }

DEF_ATOMICS(8, uint8_t, int8_t)
DEF_ATOMICS(16, uint16_t, int16_t)
DEF_ATOMICS(32, uint32_t, int32_t)
DEF_ATOMICS(64, uint64_t, int64_t)

static inline void at_casp32(uint64_t a, uint64_t* s0, uint64_t* s1, uint64_t n0, uint64_t n1) {
    uint64_t e = (uint32_t)*s0 | ((uint64_t)(uint32_t)*s1 << 32);
    uint64_t d = (uint32_t)n0 | ((uint64_t)(uint32_t)n1 << 32);
    if (__atomic_compare_exchange_n(AT_P(uint64_t, a), &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        AT_LOG(a, uint64_t);
    *s0 = (uint32_t)e; *s1 = (uint32_t)(e >> 32);
}
static inline void at_casp64(uint64_t a, uint64_t* s0, uint64_t* s1, uint64_t n0, uint64_t n1) {
    unsigned __int128 e = (unsigned __int128)*s0 | ((unsigned __int128)*s1 << 64);
    unsigned __int128 d = (unsigned __int128)n0 | ((unsigned __int128)n1 << 64);
    if (__atomic_compare_exchange_n(AT_P(unsigned __int128, a), &e, d, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST))
        AT_LOG(a, unsigned __int128);
    *s0 = (uint64_t)e; *s1 = (uint64_t)(e >> 64);
}
static inline void rt_ldxp32(CPUState* ctx, uint64_t a, int rt, int rt2) {
    uint64_t v = __atomic_load_n(AT_P(uint64_t, a), __ATOMIC_SEQ_CST);
    ctx->excl_addr = a; ctx->excl_val0 = v; ctx->excl_size = 8; ctx->excl_valid = 1;
    if (rt != 31) ctx->x[rt] = (uint32_t)v;
    if (rt2 != 31) ctx->x[rt2] = (uint32_t)(v >> 32);
}
static inline uint32_t rt_stxp32(CPUState* ctx, uint64_t a, uint64_t v0, uint64_t v1) {
    if (!ctx->excl_valid || ctx->excl_addr != a || ctx->excl_size != 8) { ctx->excl_valid = 0; return 1; }
    ctx->excl_valid = 0;
    uint64_t e = ctx->excl_val0, d = (uint32_t)v0 | ((uint64_t)(uint32_t)v1 << 32);
    if (!__atomic_compare_exchange_n(AT_P(uint64_t, a), &e, d, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 1;
    AT_LOG(a, uint64_t);
    return 0;
}
static inline void rt_ldxp64(CPUState* ctx, uint64_t a, int rt, int rt2) {
    unsigned __int128 v = __atomic_load_n(AT_P(unsigned __int128, a), __ATOMIC_SEQ_CST);
    ctx->excl_addr = a; ctx->excl_val0 = (uint64_t)v; ctx->excl_val1 = (uint64_t)(v >> 64);
    ctx->excl_size = 16; ctx->excl_valid = 1;
    if (rt != 31) ctx->x[rt] = (uint64_t)v;
    if (rt2 != 31) ctx->x[rt2] = (uint64_t)(v >> 64);
}
static inline uint32_t rt_stxp64(CPUState* ctx, uint64_t a, uint64_t v0, uint64_t v1) {
    if (!ctx->excl_valid || ctx->excl_addr != a || ctx->excl_size != 16) { ctx->excl_valid = 0; return 1; }
    ctx->excl_valid = 0;
    unsigned __int128 e = (unsigned __int128)ctx->excl_val0 | ((unsigned __int128)ctx->excl_val1 << 64);
    unsigned __int128 d = (unsigned __int128)v0 | ((unsigned __int128)v1 << 64);
    if (!__atomic_compare_exchange_n(AT_P(unsigned __int128, a), &e, d, false, __ATOMIC_SEQ_CST,
                                     __ATOMIC_SEQ_CST))
        return 1;
    AT_LOG(a, unsigned __int128);
    return 0;
}

#include "simd.h"
