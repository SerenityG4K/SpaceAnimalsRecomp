// Runtime minimal du code recompilé : dispatch des sauts indirects, services (svc, sysregs),
// erreurs, et — en mode banc (RECOMP_BENCH) — trace et API C pour le banc différentiel Python.
#include "cpu.h"

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#endif

namespace {

struct Dispatch {
    std::vector<std::pair<uint64_t, RecompFn>> tab;
    bool sorted = true;
};
Dispatch& dispatch() {
    static Dispatch d;
    return d;
}

#ifdef RECOMP_BENCH
// Arrêt contrôlé de l'exécution (le banc récupère le code et le message).
struct RecompStop {
    int code;
    uint64_t pc;
    std::string msg;
};
enum { ST_OK = 0, ST_TRAP = 1, ST_UNIMPL = 2, ST_LIMIT = 3, ST_BADTARGET = 4, ST_BADRET = 5,
       ST_UNDEF = 6 };
std::string g_last_error;
#endif

}  // namespace

// Crochets de la couche hôte (runtime/os) ; nuls dans le banc et les builds autonomes.
void (*rt_svc_handler)(CPUState* ctx, uint32_t imm, uint64_t pc) = nullptr;
void (*rt_fatal_hook)(const char* msg, uint64_t pc) = nullptr;
void (*rt_call_hook)(CPUState* ctx, uint64_t target) = nullptr;   // traçage des appels indirects

namespace {
[[noreturn]] void fatal(int code, uint64_t pc, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
[[noreturn]] void fatal(int code, uint64_t pc, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef RECOMP_BENCH
    throw RecompStop{code, pc, buf};
#else
    (void)code;
    if (rt_fatal_hook) rt_fatal_hook(buf, pc);   // ne revient pas normalement
    fprintf(stderr, "[recomp] fatal : %s\n", buf);
    fflush(stderr);
    abort();
#endif
}
}  // namespace

void rt_register_functions(const uint64_t* addrs, const RecompFn* fns, size_t n) {
    auto& d = dispatch();
    d.tab.reserve(d.tab.size() + n);
    for (size_t i = 0; i < n; i++) d.tab.emplace_back(addrs[i], fns[i]);
    d.sorted = false;
}

// Tri définitif de la table (à appeler avant de lancer des threads invités). En cas de
// doublon, la dernière inscription l'emporte : les fonctions natives (HLE), inscrites après
// les tables des modules, remplacent le code recompilé.
void rt_dispatch_finalize() {
    auto& d = dispatch();
    std::stable_sort(d.tab.begin(), d.tab.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::pair<uint64_t, RecompFn>> out;
    out.reserve(d.tab.size());
    for (size_t i = 0; i < d.tab.size(); i++) {
        if (i + 1 < d.tab.size() && d.tab[i + 1].first == d.tab[i].first) continue;
        out.push_back(d.tab[i]);
    }
    d.tab.swap(out);
    d.sorted = true;
}

const std::vector<std::pair<uint64_t, RecompFn>>& rt_dispatch_table() { return dispatch().tab; }

RecompFn rt_lookup(uint64_t addr) {
    auto& d = dispatch();
    if (!d.sorted) {
        std::sort(d.tab.begin(), d.tab.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
        d.sorted = true;
    }
    auto it = std::lower_bound(d.tab.begin(), d.tab.end(), addr,
                               [](const auto& e, uint64_t a) { return e.first < a; });
    if (it != d.tab.end() && it->first == addr) return it->second;
    return nullptr;
}

void rt_call(CPUState* ctx, uint64_t target) {
#ifndef RECOMP_BENCH
    if (rt_call_hook) rt_call_hook(ctx, target);
#endif
    RecompFn fn = rt_lookup(target);
    if (!fn) fatal(4, target, "appel indirect vers 0x%llx inconnu (lr=0x%llx)",
                   (unsigned long long)target, (unsigned long long)ctx->x[30]);
    fn(ctx);
}

void rt_tailcall(CPUState* ctx, uint64_t target) {
#ifndef RECOMP_BENCH
    if (rt_call_hook) rt_call_hook(ctx, target);
#endif
    RecompFn fn = rt_lookup(target);
    if (!fn) fatal(4, target, "saut indirect vers 0x%llx inconnu (lr=0x%llx)",
                   (unsigned long long)target, (unsigned long long)ctx->x[30]);
    MUSTTAIL return fn(ctx);
}

void rt_svc(CPUState* ctx, uint32_t imm, uint64_t pc) {
#ifdef RECOMP_BENCH
    (void)imm;
    (void)pc;
    ctx->x[0] = 0;  // comportement figé identique au hook Unicorn du banc
    ctx->x[1] = 0;
#else
    if (rt_svc_handler) return rt_svc_handler(ctx, imm, pc);
    fatal(1, pc, "svc 0x%x non géré (pc=0x%llx)", imm, (unsigned long long)pc);
#endif
}

void rt_trap(CPUState* ctx, uint64_t pc, uint32_t word) {
    (void)ctx;
    fatal(1, pc, "trap (brk/udf) 0x%08x à 0x%llx", word, (unsigned long long)pc);
}

void rt_undefined(CPUState* ctx, uint64_t pc, uint32_t word) {
    (void)ctx;
    fatal(6, pc, "instruction non allouée 0x%08x à 0x%llx", word, (unsigned long long)pc);
}

void rt_unimpl(CPUState* ctx, uint64_t pc, uint32_t word) {
    (void)ctx;
    fatal(2, pc, "instruction non implémentée 0x%08x à 0x%llx", word, (unsigned long long)pc);
}

uint64_t rt_mrs(CPUState* ctx, uint32_t enc, uint64_t pc) {
    (void)ctx;
    switch (enc) {   // (op0<<14)|(op1<<11)|(CRn<<7)|(CRm<<3)|op2
    case 0xD801: return 0x8444C004;   // CTR_EL0 : lignes I/D de 64 octets (valeur Cortex-A7x)
    }
    fatal(2, pc, "mrs registre système 0x%x non géré à 0x%llx", enc, (unsigned long long)pc);
}

void rt_msr(CPUState* ctx, uint32_t enc, uint64_t val, uint64_t pc) {
    (void)ctx;
    (void)val;
    fatal(2, pc, "msr registre système 0x%x non géré à 0x%llx", enc, (unsigned long long)pc);
}

void rt_set_fpcr(CPUState* ctx, uint32_t v) {
    ctx->fpcr = v;
#if defined(__x86_64__) || defined(_M_X64)
    // RMode (FPCR[23:22]) → MXCSR.RC ; FZ (FPCR[24]) → FTZ + DAZ
    static const unsigned rc[4] = {0u << 13, 2u << 13, 1u << 13, 3u << 13};
    unsigned m = _mm_getcsr() & ~((3u << 13) | (1u << 15) | (1u << 6));
    m |= rc[(v >> 22) & 3];
    if (v & (1u << 24)) m |= (1u << 15) | (1u << 6);
    _mm_setcsr(m);
#endif
}

// Compteur système de la Switch 2 : 31,25 MHz (19,2 MHz sur la Switch 1). Le SDK convertit les ticks en
// temps avec 31,25 MHz : avec un compteur à 19,2 MHz, le jeu ne voyait passer que 61 % du temps réel
// (cinématique d'ouverture 1,63 fois plus lente que dans Lichii, qui annonce « Guest counter frequency:
// 31250000 Hz »). RECOMP_CNTFRQ=f force une autre fréquence (diagnostic : 19200000 = ancien comportement).
static uint64_t cntfrq_init() {
    const char* e = getenv("RECOMP_CNTFRQ");
    const unsigned long long f = e ? strtoull(e, nullptr, 10) : 0;
    return f >= 1000000 && f <= 1000000000 ? (uint64_t)f : 31250000ull;
}
uint64_t rt_cntfrq() {
    static const uint64_t f = cntfrq_init();
    return f;
}

uint64_t rt_cntvct() {
#ifdef RECOMP_BENCH
    return 0x123456789ull;
#else
    using namespace std::chrono;
    static const auto t0 = steady_clock::now();
    auto ns = duration_cast<nanoseconds>(steady_clock::now() - t0).count();
    return (uint64_t)((unsigned __int128)ns * rt_cntfrq() / 1000000000ull);
#endif
}

uint32_t crc32_update(uint32_t crc, uint64_t data, int bytes, bool castagnoli) {
    const uint32_t poly = castagnoli ? 0x82F63B78u : 0xEDB88320u;
    for (int i = 0; i < bytes; i++) {
        crc ^= (uint8_t)(data >> (8 * i));
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (poly & (0u - (crc & 1)));
    }
    return crc;
}

// ------------------------------------------------------------------------------------------
// Banc de validation : trace, pile d'ombre, API C exportée
// ------------------------------------------------------------------------------------------
#ifdef RECOMP_BENCH
namespace {
struct TraceEntry {
    uint64_t pc;
    uint64_t x[31];
    uint64_t sp;
    uint32_t nzcv;
    uint32_t pad;
    V128 v[32];
};
static_assert(sizeof(TraceEntry) == 784, "TraceEntry");
struct WriteEntry {
    uint64_t step;
    uint64_t addr;
    uint32_t size;
    uint32_t pad;
    uint8_t data[64];
};
std::vector<TraceEntry> g_trace;
std::vector<WriteEntry> g_writes;
uint64_t g_steps = 0, g_max_steps = 1000000;
bool g_trace_on = false, g_trace_v = true;
std::vector<uint64_t> g_shadow;

void stub_return_zero(CPUState* ctx) {
    ctx->x[0] = 0;
#ifdef RECOMP_CHECK_RET
    rt_check_ret(ctx, ctx->x[30], 0);
#endif
}
}  // namespace

#ifdef RECOMP_TRACE
void rt_trace(CPUState* ctx, uint64_t pc) {
    ctx->pc = pc;
    if (++g_steps > g_max_steps) fatal(3, pc, "limite de pas atteinte à 0x%llx", (unsigned long long)pc);
    if (!g_trace_on) return;
    g_trace.emplace_back();
    TraceEntry& e = g_trace.back();
    e.pc = pc;
    memcpy(e.x, ctx->x, sizeof e.x);
    e.sp = ctx->sp;
    e.nzcv = (uint32_t)rt_get_nzcv(ctx);
    e.pad = 0;
    if (g_trace_v) memcpy(e.v, ctx->v, sizeof e.v);
    else memset(e.v, 0, sizeof e.v);
}

void rt_trace_write(uint64_t addr, const void* data, uint32_t size) {
    if (!g_trace_on) return;
    const uint8_t* p = (const uint8_t*)data;
    while (size) {
        uint32_t n = size > 64 ? 64 : size;
        WriteEntry w{};
        w.step = g_steps ? g_steps - 1 : 0;
        w.addr = addr;
        w.size = n;
        memcpy(w.data, p, n);
        g_writes.push_back(w);
        addr += n; p += n; size -= n;
    }
}
#endif

#ifdef RECOMP_CHECK_RET
void rt_push_ret(uint64_t ret) { g_shadow.push_back(ret); }
void rt_check_ret(CPUState* ctx, uint64_t target, uint64_t pc) {
    (void)ctx;
    if (g_shadow.empty()) fatal(5, pc, "ret sans appel à 0x%llx", (unsigned long long)pc);
    uint64_t expect = g_shadow.back();
    g_shadow.pop_back();
    if (expect != target)
        fatal(5, pc, "ret vers 0x%llx au lieu de 0x%llx (pc=0x%llx)", (unsigned long long)target,
              (unsigned long long)expect, (unsigned long long)pc);
}
#endif

RECOMP_EXPORT int recomp_state_size() { return (int)sizeof(CPUState); }
RECOMP_EXPORT int recomp_trace_entry_size() { return (int)sizeof(TraceEntry); }
RECOMP_EXPORT int recomp_write_entry_size() { return (int)sizeof(WriteEntry); }

RECOMP_EXPORT void recomp_trace_config(int on, int with_v, uint64_t max_steps) {
    g_trace_on = on != 0;
    g_trace_v = with_v != 0;
    g_max_steps = max_steps;
}
RECOMP_EXPORT uint64_t recomp_trace_count() { return g_trace.size(); }
RECOMP_EXPORT const void* recomp_trace_data() { return g_trace.data(); }
RECOMP_EXPORT uint64_t recomp_writes_count() { return g_writes.size(); }
RECOMP_EXPORT const void* recomp_writes_data() { return g_writes.data(); }
RECOMP_EXPORT uint64_t recomp_steps() { return g_steps; }
RECOMP_EXPORT const char* recomp_last_error() { return g_last_error.c_str(); }
RECOMP_EXPORT int recomp_has_function(uint64_t addr) { return rt_lookup(addr) != nullptr; }
RECOMP_EXPORT uint64_t recomp_function_count() { return dispatch().tab.size(); }

RECOMP_EXPORT void recomp_register_stub(uint64_t addr) {
    static RecompFn f = stub_return_zero;
    rt_register_functions(&addr, &f, 1);
}

RECOMP_EXPORT int recomp_call(CPUState* ctx, uint64_t addr) {
    g_trace.clear();
    g_writes.clear();
    g_shadow.clear();
    g_steps = 0;
    g_last_error.clear();
    RecompFn fn = rt_lookup(addr);
    if (!fn) {
        g_last_error = "fonction inconnue";
        return ST_BADTARGET;
    }
#ifdef RECOMP_CHECK_RET
    g_shadow.push_back(ctx->x[30]);
#endif
    try {
        fn(ctx);
    } catch (const RecompStop& s) {
        g_last_error = s.msg;
        ctx->pc = s.pc;
        return s.code;
    }
    return ST_OK;
}
#endif  // RECOMP_BENCH
