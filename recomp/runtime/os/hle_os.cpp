// Fibres nn::os → fibres Windows.
//
// Le code recompilé utilise la pile hôte pour ses appels : changer de pile invitée (ce que fait
// nn::os::SwitchToFiber) ne suffit pas, il faut aussi changer de pile hôte. Chaque fibre
// invitée a donc sa fibre Windows (pile hôte propre) et son propre état CPU ; changer de fibre =
// SwitchToFiber hôte, en recopiant les registres propres au thread (TLS, FPCR/FPSR). Une fibre
// peut ainsi être reprise sur un autre thread que celui où elle s'est suspendue.
//
// Remplace InitializeFiber / FinalizeFiber / SwitchToFiber / GetCurrentFiber ; les appels
// internes au SDK (nn::async) passent par le dispatch grâce à recomp/hooks.txt.
#include "hos.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <utility>
#include <mutex>
#include <unordered_map>

namespace hos {
namespace {

struct HFiber {
    void* host = nullptr;      // fibre Windows
    CPUState* ctx = nullptr;   // état CPU invité de la fibre
    uint64_t guest = 0;        // nn::os::FiberType* (0 : contexte d'origine d'un thread)
    uint64_t func = 0, arg = 0, stack = 0, stack_size = 0;
    bool finished = false;
};

std::mutex g_lk;
std::unordered_map<uint64_t, HFiber*> g_fibers;   // FiberType* → fibre
thread_local HFiber* t_thread_fiber = nullptr;    // contexte d'origine du thread hôte
thread_local HFiber* t_current = nullptr;
constexpr size_t HOST_FIBER_STACK = 64ull << 20;

HFiber* lookup(uint64_t guest) {
    std::lock_guard<std::mutex> lk(g_lk);
    auto it = g_fibers.find(guest);
    return it == g_fibers.end() ? nullptr : it->second;
}

void ensure_thread_fiber(CPUState* ctx) {
    if (t_thread_fiber) return;
    auto* f = new HFiber;
    f->host = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiberEx(nullptr, FIBER_FLAG_FLOAT_SWITCH);
    if (!f->host) panic("ConvertThreadToFiber impossible (erreur %lu)", GetLastError());
    f->ctx = ctx;
    t_thread_fiber = t_current = f;
}

void switch_to(CPUState* cur_ctx, HFiber* target) {
    // registres propres au thread, partagés par toutes ses fibres
    target->ctx->tpidr_el0 = cur_ctx->tpidr_el0;
    target->ctx->tpidrro_el0 = cur_ctx->tpidrro_el0;
    target->ctx->fpcr = cur_ctx->fpcr;
    target->ctx->fpsr = cur_ctx->fpsr;
    t_current = target;
    kernel_set_running_ctx(target->ctx);
    ::SwitchToFiber(target->host);
    // repris ici : celui qui nous a relancés a mis à jour t_current
}

void WINAPI fiber_entry(void* p) {
    HFiber* f = (HFiber*)p;
    CPUState* c = f->ctx;
    c->x[0] = f->arg;
    c->sp = (f->stack + f->stack_size) & ~15ull;
    c->x[29] = 0;
    call_guest(c, f->func);
    // la fonction de fibre renvoie la fibre suivante (nullptr : contexte d'origine du thread)
    uint64_t next = c->x[0];
    f->finished = true;
    HFiber* n = next ? lookup(next) : t_thread_fiber;
    if (!n) panic("fibre 0x%llx terminée vers une fibre inconnue 0x%llx", (unsigned long long)f->guest,
                  (unsigned long long)next);
    switch_to(c, n);
    panic("fibre terminée reprise");
}

// nn::os::InitializeFiber(FiberType*, FiberFunction, void* arg, void* stack, size_t size, int flags)
void init_fiber(CPUState* ctx) {
    auto* f = new HFiber;
    f->guest = ctx->x[0];
    f->func = ctx->x[1];
    f->arg = ctx->x[2];
    f->stack = ctx->x[3];
    f->stack_size = ctx->x[4];
    f->ctx = (CPUState*)_aligned_malloc(sizeof(CPUState), 64);
    memset(f->ctx, 0, sizeof(CPUState));
    f->host = CreateFiberEx(64 << 10, HOST_FIBER_STACK, FIBER_FLAG_FLOAT_SWITCH, fiber_entry, f);
    if (!f->host) panic("CreateFiberEx impossible (erreur %lu)", GetLastError());
    HFiber* old = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fibers.find(f->guest);
        if (it != g_fibers.end()) old = it->second;
        g_fibers[f->guest] = f;
    }
    if (old && old != t_current) {
        DeleteFiber(old->host);
        _aligned_free(old->ctx);
        delete old;
    }
    HDBG("fiber", "InitializeFiber 0x%llx : %s, pile 0x%llx+0x%llx", (unsigned long long)f->guest,
         symbolize(f->func).c_str(), (unsigned long long)f->stack, (unsigned long long)f->stack_size);
}

void finalize_fiber(CPUState* ctx) {
    uint64_t g = ctx->x[0];
    HFiber* f = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fibers.find(g);
        if (it == g_fibers.end()) return;
        f = it->second;
        g_fibers.erase(it);
    }
    if (f == t_current) {   // ne devrait pas arriver : on ne détruit pas la fibre qui tourne
        HLOG("fiber", "FinalizeFiber sur la fibre courante 0x%llx : ignoré", (unsigned long long)g);
        return;
    }
    DeleteFiber(f->host);
    _aligned_free(f->ctx);
    delete f;
}

void switch_fiber(CPUState* ctx) {
    ensure_thread_fiber(ctx);
    HFiber* t = lookup(ctx->x[0]);
    if (!t) panic("SwitchToFiber vers une fibre inconnue 0x%llx (appelant %s)",
                  (unsigned long long)ctx->x[0], symbolize(ctx->x[30]).c_str());
    if (t == t_current) return;
    if (t->finished) panic("SwitchToFiber vers une fibre terminée 0x%llx", (unsigned long long)ctx->x[0]);
    switch_to(ctx, t);
}

void current_fiber(CPUState* ctx) {
    ctx->x[0] = (t_current && t_current != t_thread_fiber) ? t_current->guest : 0;
}

// ---------------------------------------------------------------- coroutines du moteur
// main+0xba33c30 : jump(x0 = &case_sauvegarde, x1 = &case_cible). Version d'origine : empile
// x29/x30, d15..d8, x28..x19 (0xa0 octets), range sp dans *x0, charge sp depuis *x1, dépile et
// « revient » (ret) dans l'autre contexte avec x0 = 0. Ici : chaque contexte a sa fibre Windows
// et son état CPU ; la « poignée » est la valeur de sp rangée dans la case.
std::unordered_map<uint64_t, HFiber*> g_eng;   // poignée (sp suspendu) → contexte suspendu

void write_frame(CPUState* c, uint64_t h) {
    uint64_t* q = (uint64_t*)h;
    q[0] = c->x[29];
    q[1] = c->x[30];
    for (int i = 0; i < 8; i++) q[2 + i] = c->v[15 - i].u64[0];        // d15..d8
    for (int i = 0; i < 10; i++) q[10 + i] = c->x[28 - i];             // x28..x19
}

void read_frame(CPUState* c, uint64_t h) {
    const uint64_t* q = (const uint64_t*)h;
    c->x[29] = q[0];
    c->x[30] = q[1];
    for (int i = 0; i < 8; i++) c->v[15 - i] = V128{}, c->v[15 - i].u64[0] = q[2 + i];
    for (int i = 0; i < 10; i++) c->x[28 - i] = q[10 + i];
    c->sp = h + 0xa0;
    c->x[0] = 0;
}

void WINAPI eng_entry(void* p) {
    HFiber* f = (HFiber*)p;
    CPUState* c = f->ctx;
    // nouveau contexte préparé par le moteur : « retour » à x30 (trampoline : x0 = x19, br x20)
    call_guest(c, c->x[30]);
    panic("coroutine du moteur terminée (entrée %s)", symbolize(f->func).c_str());
}

void eng_jump(CPUState* ctx) {
    ensure_thread_fiber(ctx);
    uint64_t save_slot = ctx->x[0], target_slot = ctx->x[1];
    HFiber* cur = t_current;
    uint64_t h = (ctx->sp - 0xa0) & ~15ull;
    write_frame(ctx, h);                       // même empreinte mémoire que l'original
    uint64_t th = *(uint64_t*)target_slot;
    HFiber* t = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_lk);
        g_eng[h] = cur;
        *(uint64_t*)save_slot = h;
        auto it = g_eng.find(th);
        if (it != g_eng.end()) {
            t = it->second;
            g_eng.erase(it);
        }
    }
    if (!t) {   // contexte neuf : registres lus dans le bloc préparé sur sa pile
        t = new HFiber;
        t->ctx = (CPUState*)_aligned_malloc(sizeof(CPUState), 64);
        memset(t->ctx, 0, sizeof(CPUState));
        read_frame(t->ctx, th);
        t->func = t->ctx->x[30];
        t->host = CreateFiberEx(64 << 10, HOST_FIBER_STACK, FIBER_FLAG_FLOAT_SWITCH, eng_entry, t);
        if (!t->host) panic("CreateFiberEx impossible (erreur %lu)", GetLastError());
        HDBG("fiber", "nouvelle coroutine : pile 0x%llx, reprise %s", (unsigned long long)th,
             symbolize(t->func).c_str());
    }
    if (t == cur) {   // retour sur soi-même
        ctx->x[0] = 0;
        return;
    }
    switch_to(ctx, t);
    ctx->x[0] = 0;   // repris : jump renvoie 0
}

// ---------------------------------------------------------------- bouchons d'API nommés
constexpr int NAPI = 512;
struct ApiStub {
    std::string name;
    std::atomic<uint64_t> calls{0};
};
ApiStub g_api[NAPI];
int g_napi = 0;
void api_stub_call(int i, CPUState* ctx) {
    if (g_api[i].calls++ == 0)
        HLOG("hle", "[neutralisé] %s (appelant %s)", g_api[i].name.c_str(), symbolize(ctx->x[30]).c_str());
    ctx->x[0] = 0;
}
template <int I>
void api_stub_fn(CPUState* ctx) { api_stub_call(I, ctx); }
template <int... I>
std::array<RecompFn, sizeof...(I)> make_api_stubs(std::integer_sequence<int, I...>) {
    return {{api_stub_fn<I>...}};
}

}  // namespace

int stub_namespace(const char* prefix, const char* why) {
    static const auto stubs = make_api_stubs(std::make_integer_sequence<int, NAPI>{});
    int n = 0;
    for (auto& m : g_modules) {
        if (m.name != "main") continue;
        for (auto& imp : m.imports) {
            if (imp.compare(0, strlen(prefix), prefix)) continue;
            if (g_napi >= NAPI) panic("réserve de bouchons d'API épuisée");
            int i = g_napi++;
            g_api[i].name = demangle(imp.c_str());
            if (override_export(imp.c_str(), stubs[i])) n++;
        }
    }
    HLOG("hle", "%s neutralisé (%d fonctions) : %s", prefix, n, why);
    return n;
}

void hle_os_init() {
    int n = 0;
    n += override_export("_ZN2nn2os15InitializeFiberEPNS0_9FiberTypeEPFS2_PvES3_S3_mi", init_fiber);
    n += override_export("_ZN2nn2os13FinalizeFiberEPNS0_9FiberTypeE", finalize_fiber);
    n += override_export("_ZN2nn2os13SwitchToFiberEPNS0_9FiberTypeE", switch_fiber);
    n += override_export("_ZN2nn2os15GetCurrentFiberEv", current_fiber);
    for (auto& m : g_modules)
        if (m.name == "main") {
            uint64_t a = m.base + 0xba33c30;
            RecompFn fn = eng_jump;
            rt_register_functions(&a, &fn, 1);
            n++;
        }
    HLOG("hle", "fibres nn::os et coroutines du moteur → fibres Windows (%d fonctions remplacées)", n);
    stub_namespace("_ZN2nn4chat", "GameChat repose sur le système graphique gsv, court-circuité");
}

}  // namespace hos
