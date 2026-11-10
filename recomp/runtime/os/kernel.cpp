// Noyau Horizon émulé au niveau des svc : handles, threads (threads hôtes), synchronisation
// (mutex/condvars « process-wide », arbitre d'adresses, événements, WaitSynchronization),
// mémoire (tas, MapMemory, mémoire physique d'alias, mémoire partagée/transfert), GetInfo.
//
// Sémantique calquée sur le noyau (documentation switchbrew / comportement de mesosphère) ;
// un seul verrou global protège tout l'état du noyau.
#include "hos.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <thread>
#include <unordered_map>

namespace hos {

// ------------------------------------------------------------------------------ objets
struct KThread : KObject {
    const char* kind() const override { return "Thread"; }
    bool is_signaled() const override { return exited; }
    uint32_t handle = 0;
    uint64_t id = 0;
    uint64_t entry = 0, arg = 0, stack_top = 0;
    int32_t prio = 44, core = 0;
    uint64_t affinity = 1;
    uint64_t tlr = 0;
    CPUState* ctx = nullptr;
    HANDLE host = nullptr;
    bool started = false, exited = false;
    // attente : événement Win32 (réveil) + minuteur haute résolution (délais précis à ~0,5 ms,
    // indépendamment de la résolution du minuteur système ; std::condition_variable arrondissait
    // les délais à 10-15 ms, ce qui faisait tomber le jeu à ~42 images/s)
    HANDLE wake_ev = nullptr, hr_timer = nullptr;
    bool woken = false;
    uint32_t wait_result = 0;
    int32_t wait_index = -1;
    bool cancel_pending = false, in_sync = false;
    std::vector<KObject*> wait_objs;
    // mutex / condvar
    uint32_t tag = 0;          // valeur à écrire dans le mutex à l'acquisition
    uint64_t cv_mutex = 0;     // mutex associé (WaitProcessWideKeyAtomic)
    uint64_t cv_key = 0;
    int where = 0;             // 0 : rien, 1 : file mutex, 2 : file condvar, 3 : arbitre
    uint64_t where_key = 0;
    uint32_t last_svc = 0;     // diagnostic
    uint64_t last_args[3] = {};
    uint64_t svc_count = 0;
    uint32_t last_result = 0;
    uint64_t last_out1 = 0;
};

struct KEvent : KObject {
    const char* kind() const override { return "Event"; }
    bool is_signaled() const override { return signaled; }
    bool signaled = false;
};

struct KSharedMemory : KObject {
    const char* kind() const override { return "SharedMemory"; }
    uint64_t size = 0;
    std::vector<uint8_t> backing;
    uint64_t mapped = 0;
};

struct KTransferMemory : KObject {
    const char* kind() const override { return "TransferMemory"; }
    uint64_t addr = 0, size = 0;
    uint32_t perm = 0;
};

struct KResourceLimit : KObject {
    const char* kind() const override { return "ResourceLimit"; }
};

struct KProcess : KObject {
    const char* kind() const override { return "Process"; }
};

// ------------------------------------------------------------------------------ état
namespace {

constexpr uint32_t CUR_THREAD = 0xFFFF8000u, CUR_PROCESS = 0xFFFF8001u;
constexpr uint32_t HAS_WAITERS = 0x40000000u;
constexpr uint64_t HOST_STACK = 256ull << 20;   // pile hôte réservée par thread invité

enum : uint32_t { HF_READ = 1, HF_WRITE = 2 };
struct HEntry {
    std::shared_ptr<KObject> obj;
    uint32_t flags;
};

std::mutex g_k;
std::unordered_map<uint32_t, HEntry> g_handles;
uint32_t g_next_handle = 0x1001;
std::vector<std::shared_ptr<KThread>> g_threads;
std::unordered_map<uint64_t, std::deque<KThread*>> g_mutex_q, g_cv_q, g_arb_q;
std::shared_ptr<KProcess> g_process = std::make_shared<KProcess>();
std::shared_ptr<KResourceLimit> g_reslimit = std::make_shared<KResourceLimit>();
uint64_t g_heap_size = 0;
struct Mapping {
    uint64_t src, size;
};
std::map<uint64_t, Mapping> g_maps;      // MapMemory : dst → src
uint64_t g_next_tlr = KMISC_BASE;
std::atomic<uint64_t> g_next_tid{0x100};
thread_local KThread* t_cur = nullptr;
thread_local CPUState* t_running_ctx = nullptr;   // état de la fibre en cours (sinon t_cur->ctx)
thread_local std::shared_ptr<KObject> t_last_event;   // dernier svcCreateEvent de ce thread

// natives
struct Native {
    uint64_t addr;
    std::string name;
};
std::vector<Native> g_natives;
std::mutex g_nat_lock;

uint32_t new_handle(std::shared_ptr<KObject> o, uint32_t flags = 0) {
    uint32_t h;
    do {
        h = g_next_handle++;
        if (g_next_handle >= 0x3FFF0000u) g_next_handle = 0x1001;
    } while (g_handles.count(h));
    g_handles[h] = HEntry{std::move(o), flags};
    return h;
}

template <class T>
T* get(uint32_t h, uint32_t* flags = nullptr) {
    if (h == CUR_THREAD) return dynamic_cast<T*>(static_cast<KObject*>(t_cur));
    if (h == CUR_PROCESS) return dynamic_cast<T*>(static_cast<KObject*>(g_process.get()));
    auto it = g_handles.find(h);
    if (it == g_handles.end()) return nullptr;
    if (flags) *flags = it->second.flags;
    return dynamic_cast<T*>(it->second.obj.get());
}

KObject* get_any(uint32_t h) {
    if (h == CUR_THREAD) return t_cur;
    if (h == CUR_PROCESS) return g_process.get();
    auto it = g_handles.find(h);
    return it == g_handles.end() ? nullptr : it->second.obj.get();
}

uint32_t atomic_load32(uint64_t a) { return __atomic_load_n((uint32_t*)a, __ATOMIC_SEQ_CST); }
void atomic_store32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t*)a, v, __ATOMIC_SEQ_CST); }
bool cas32(uint64_t a, uint32_t& expect, uint32_t v) {
    return __atomic_compare_exchange_n((uint32_t*)a, &expect, v, false, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

// Attente du thread courant (verrou g_k tenu). timeout en ns (< 0 : infini).
// Renvoie true si réveillé, false si délai écoulé.
HANDLE hr_timer(KThread* t) {
    if (!t->hr_timer)
        t->hr_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    return t->hr_timer;
}

// Dort `ns` nanosecondes avec la précision du minuteur haute résolution.
void precise_sleep(KThread* t, int64_t ns) {
    HANDLE tm = t ? hr_timer(t) : nullptr;
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100 ? ns / 100 : 1);
    if (tm && SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(tm, INFINITE);
    else std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
}

bool block(std::unique_lock<std::mutex>& lk, KThread* t, int64_t timeout_ns) {
    if (t->woken) return true;
    if (!t->wake_ev) t->wake_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto dl = std::chrono::steady_clock::now() + std::chrono::nanoseconds(timeout_ns < 0 ? 0 : timeout_ns);
    while (!t->woken) {
        DWORD r;
        if (timeout_ns < 0) {
            lk.unlock();
            r = WaitForSingleObject(t->wake_ev, INFINITE);
        } else {
            int64_t rem = std::chrono::duration_cast<std::chrono::nanoseconds>(dl - std::chrono::steady_clock::now()).count();
            if (rem <= 0) return false;
            HANDLE tm = hr_timer(t);
            LARGE_INTEGER due;
            due.QuadPart = -(rem / 100 ? rem / 100 : 1);
            lk.unlock();
            if (tm && SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) {
                HANDLE hs[2] = {t->wake_ev, tm};
                r = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            } else {
                r = WaitForSingleObject(t->wake_ev, (DWORD)((rem + 999999) / 1000000));
            }
        }
        lk.lock();
        (void)r;   // réveil, délai ou réveil parasite : on reteste l'état sous le verrou
    }
    return true;
}

void wake(KThread* t, uint32_t result, int32_t index = -1) {
    t->woken = true;
    t->wait_result = result;
    t->wait_index = index;
    t->where = 0;
    if (t->wake_ev) SetEvent(t->wake_ev);
}

void prepare_wait(KThread* t) {
    t->woken = false;
    t->wait_result = 0;
    t->wait_index = -1;
}

void remove_from(std::unordered_map<uint64_t, std::deque<KThread*>>& m, uint64_t key, KThread* t) {
    auto it = m.find(key);
    if (it == m.end()) return;
    auto& q = it->second;
    q.erase(std::remove(q.begin(), q.end(), t), q.end());
    if (q.empty()) m.erase(it);
}

void notify_sync(KObject* o) {
    for (KThread* w : o->sync_waiters) {
        if (w->woken || !w->in_sync) continue;
        int idx = -1;
        for (size_t i = 0; i < w->wait_objs.size(); i++)
            if (w->wait_objs[i] == o) { idx = (int)i; break; }
        wake(w, 0, idx);
    }
}

// --- mutex (ArbitrateLock / ArbitrateUnlock)
void mutex_release(uint64_t addr) {
    auto it = g_mutex_q.find(addr);
    if (it == g_mutex_q.end() || it->second.empty()) {
        atomic_store32(addr, 0);
        if (it != g_mutex_q.end()) g_mutex_q.erase(it);
        return;
    }
    KThread* w = it->second.front();
    it->second.pop_front();
    bool more = !it->second.empty();
    atomic_store32(addr, w->tag | (more ? HAS_WAITERS : 0));
    if (!more) g_mutex_q.erase(it);
    wake(w, 0);
}

// le thread `w` (réveillé d'une condvar) tente de reprendre son mutex
void cv_reacquire(KThread* w) {
    uint64_t a = w->cv_mutex;
    uint32_t v = atomic_load32(a);
    for (;;) {
        if (v == 0) {
            if (cas32(a, v, w->tag)) { wake(w, 0); return; }
            continue;
        }
        if (cas32(a, v, v | HAS_WAITERS)) break;
    }
    w->where = 1;
    w->where_key = a;
    g_mutex_q[a].push_back(w);
}

// --- threads
unsigned __stdcall thread_proc(void* p) {
    KThread* t = (KThread*)p;
    t_cur = t;
    rt_set_fpcr(t->ctx, t->ctx->fpcr);
    HINFO("thread", "démarrage du thread %llu (entrée %s, arg 0x%llx, sp 0x%llx, prio %d)",
          (unsigned long long)t->id, symbolize(t->entry).c_str(), (unsigned long long)t->arg,
          (unsigned long long)t->stack_top, t->prio);
    call_guest(t->ctx, t->entry);
    HINFO("thread", "le thread %llu est revenu de sa fonction d'entrée", (unsigned long long)t->id);
    {
        std::lock_guard<std::mutex> lk(g_k);
        t->exited = true;
        notify_sync(t);
    }
    return 0;
}

std::shared_ptr<KThread> make_thread(uint64_t entry, uint64_t arg, uint64_t sp, int prio, int core) {
    auto t = std::make_shared<KThread>();
    t->id = g_next_tid++;
    t->entry = entry;
    t->arg = arg;
    t->stack_top = sp;
    t->prio = prio;
    t->core = core < 0 ? 0 : core;
    t->affinity = 1ull << t->core;
    // TLR : 0x200 octets par thread, pages de la zone noyau
    if ((g_next_tlr & 0xfff) == 0) {
        mem_commit(g_next_tlr, 0x1000);
        mem_set(g_next_tlr, 0x1000, MS_THREADLOCAL, MP_RW);
    }
    t->tlr = g_next_tlr;
    memset((void*)t->tlr, 0, 0x200);
    g_next_tlr += 0x200;
    t->ctx = (CPUState*)_aligned_malloc(sizeof(CPUState), 64);
    memset(t->ctx, 0, sizeof(CPUState));
    t->ctx->x[0] = arg;
    t->ctx->sp = sp;
    t->ctx->tpidrro_el0 = t->tlr;
    t->ctx->pc = entry;
    g_threads.push_back(t);
    return t;
}

void start_host_thread(KThread* t) {
    t->started = true;
    t->host = (HANDLE)_beginthreadex(nullptr, (unsigned)HOST_STACK, thread_proc, t,
                                     STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!t->host) panic("création du thread hôte impossible");
}

int64_t timeout_of(uint64_t v) { return (int64_t)v; }   // < 0 : infini

std::mt19937_64 g_rng(0x5eedULL);

}  // namespace

// ------------------------------------------------------------------------------ API
KThread* current_thread() { return t_cur; }
uint64_t current_thread_id() { return t_cur ? t_cur->id : 0; }
CPUState* current_ctx() { return t_running_ctx ? t_running_ctx : t_cur ? t_cur->ctx : nullptr; }
void kernel_set_running_ctx(CPUState* ctx) { t_running_ctx = ctx; }
std::shared_ptr<KObject> kernel_last_created_event() { return t_last_event; }

uint64_t register_native(const char* name, RecompFn fn) {
    std::lock_guard<std::mutex> lk(g_nat_lock);
    uint64_t a = NATIVE_BASE + g_natives.size() * 16;
    g_natives.push_back(Native{a, name});
    rt_register_functions(&a, &fn, 1);
    return a;
}

void rename_native(uint64_t addr, const std::string& name) {
    std::lock_guard<std::mutex> lk(g_nat_lock);
    uint64_t i = (addr - NATIVE_BASE) / 16;
    if (i < g_natives.size()) g_natives[i].name = name;
}

bool override_export(const char* sym, RecompFn fn) {
    uint64_t a = find_export(sym);
    if (!a) {
        HLOG("hle", "remplacement impossible : symbole %s introuvable", sym);
        return false;
    }
    rt_register_functions(&a, &fn, 1);   // inscrit après les modules : l'emporte au dispatch
    return true;
}

std::string native_name(uint64_t addr) {
    std::lock_guard<std::mutex> lk(g_nat_lock);
    uint64_t i = (addr - NATIVE_BASE) / 16;
    return i < g_natives.size() ? g_natives[i].name : "?";
}

void call_guest(CPUState* ctx, uint64_t addr) {
    RecompFn fn = rt_lookup(addr);
    if (!fn) panic("appel de 0x%llx (%s) : aucune fonction recompilée à cette adresse",
                   (unsigned long long)addr, symbolize(addr).c_str());
    uint64_t lr = ctx->x[30];
    ctx->x[30] = 0;   // `ret` = retour C++ : la valeur n'est pas utilisée
    fn(ctx);
    ctx->x[30] = lr;
}

void kernel_init() {
    rt_svc_handler = svc_dispatch;
    // contexte d'exception utilisateur (GetInfo 20) : le SDK y écrit, p. ex. la région du
    // « dying message » à +0x1c0
    mem_commit(USER_EXC_CTX, 0x1000);
    mem_set(USER_EXC_CTX, 0x1000, MS_THREADLOCAL, MP_RW);
}

int run_main_thread(uint64_t entry, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4,
                    uint64_t stack_size) {
    KThread* t;
    {
        std::lock_guard<std::mutex> lk(g_k);
        uint64_t top = STACK_BASE + STACK_SIZE - 0x10000;   // garde en haut de la zone
        uint64_t bottom = top - stack_size;
        mem_commit(bottom, stack_size);
        mem_set(bottom, stack_size, MS_STACK, MP_RW);
        auto th = make_thread(entry, a0, top, 44, 0);
        th->handle = new_handle(th);
        t = th.get();
        t->ctx->x[0] = th->handle;   // nn::init::Start(handle, …)
        t->ctx->x[1] = a1;
        t->ctx->x[2] = a2;
        t->ctx->x[3] = a3;
        t->ctx->x[4] = a4;
        (void)a0;
        HLOG("kernel", "thread principal : handle 0x%x, pile 0x%llx-0x%llx", th->handle,
             (unsigned long long)bottom, (unsigned long long)top);
        start_host_thread(t);
    }
    WaitForSingleObject(t->host, INFINITE);
    HLOG("kernel", "le thread principal s'est terminé");
    return 0;
}

// ------------------------------------------------------------------------------ svc
namespace {

// raccourcis registres
#define X(n) ctx->x[n]
#define W(n) ((uint32_t)ctx->x[n])
#define RET(r) (ctx->x[0] = (uint32_t)(r))

uint64_t info_value(uint32_t id, uint32_t handle, uint64_t sub, uint32_t& res) {
    res = 0;
    switch (id) {
    case 0: return 0xF;                         // CoreMask (cœurs 0-3)
    case 1: return 0xFFFFFFFFull << 28 & 0x0FFFFFFFF0000000ull;   // PriorityMask 28..59
    case 2: return ALIAS_BASE;
    case 3: return ALIAS_SIZE;
    case 4: return HEAP_BASE;
    case 5: return HEAP_SIZE;
    case 6: return 0x200000000ull;              // TotalMemorySize : 8 Go
    case 7: return mem_used();                  // UsedMemorySize
    case 8: return 0;                           // DebuggerAttached
    case 9: { std::lock_guard<std::mutex> lk(g_k); return new_handle(g_reslimit); }
    case 10: return 0;                          // IdleTickCount
    case 11: return g_rng();                    // RandomEntropy
    case 12: return AS_BASE;                    // AslrRegionAddress
    case 13: return AS_END - AS_BASE;           // AslrRegionSize
    case 14: return STACK_BASE;
    case 15: return STACK_SIZE;
    case 16: return 0x1000000;                  // SystemResourceSizeTotal (main.npdm) : VAMM actif
    case 17: return 0;                          // SystemResourceSizeUsed
    case 18: return 0x0400E88025B1A000ull;      // ProgramId
    case 19: return 0;                          // InitialProcessIdRange
    case 20: return USER_EXC_CTX;               // UserExceptionContextAddress (page dédiée)
    case 21: return 0x200000000ull;             // TotalNonSystemMemorySize
    case 22: return mem_used();                 // UsedNonSystemMemorySize
    case 23: return 1;                          // IsApplication
    case 24: return 200;                        // FreeThreadCount
    case 25: return 0;                          // ThreadTickCount
    case 26: return 1;                          // IsSvcPermitted
    case 27: return 0;                          // IoRegionHint
    case 28: return 0;                          // AliasRegionExtraSize
    // 29/30 : nouvelle paire adresse/taille (noyau Switch 2), lue par nnosInitialize juste
    // après tas et alias pour un AddressSpaceAllocator (granularité 64 Ko)
    case 29: return EXTRA_BASE;
    case 30: return EXTRA_SIZE;
    case 31: return 0;                          // lu par nn::os::GetRemoteMemoryUsage
    // 33 : taille de page (ProtectRelro vérifie l'alignement des bornes RELRO dessus ; le VAMM
    // mappe sa première page de gestion avec cette taille)
    case 33: return 0x1000;
    default:
        // identifiants récents inconnus : 0 + avertissement (une erreur ferait avorter le SDK)
        HLOG("svc", "GetInfo id %u (handle 0x%x, sub 0x%llx) inconnu → 0", id, handle,
             (unsigned long long)sub);
        return 0;
    }
}

}  // namespace

static void svc_dispatch_impl(CPUState* ctx, uint32_t imm, uint64_t pc);

std::atomic<bool> g_svc_timing{false};   // diagnostic (RECOMP_FRAME_TRACE) : attentes longues

void svc_dispatch(CPUState* ctx, uint32_t imm, uint64_t pc) {
    if (!g_svc_timing.load(std::memory_order_relaxed)) {
        svc_dispatch_impl(ctx, imm, pc);
    } else {
        uint64_t a0 = X(0), a1 = X(1), a2 = X(2), a3 = X(3), lr = ctx->x[30];
        std::string chain;   // appelants via la chaîne des cadres (x29)
        for (uint64_t fp = ctx->x[29], i = 0; i < 5 && fp && mem_valid(fp, 16); i++) {
            uint64_t ra = *(uint64_t*)(fp + 8);
            chain += " ← " + symbolize(ra);
            fp = *(uint64_t*)fp;
        }
        auto t0 = std::chrono::steady_clock::now();
        svc_dispatch_impl(ctx, imm, pc);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        static const double thr = getenv("RECOMP_FRAME_TRACE_MS") ? atof(getenv("RECOMP_FRAME_TRACE_MS")) : 1.0;
        if (ms >= thr)
            HLOG("ftrace", "thread %llu svc 0x%02x (%llx %llx %llx %llx) %.2f ms ← %s%s", (unsigned long long)current_thread_id(),
                 imm, (unsigned long long)a0, (unsigned long long)a1, (unsigned long long)a2, (unsigned long long)a3, ms,
                 symbolize(lr).c_str(), chain.c_str());
    }
    if (t_cur) t_cur->last_result = (uint32_t)ctx->x[0], t_cur->last_out1 = ctx->x[1];
}

static void svc_dispatch_impl(CPUState* ctx, uint32_t imm, uint64_t pc) {
    KThread* t = t_cur;
    if (t) {
        t->last_svc = imm;
        t->last_args[0] = X(0);
        t->last_args[1] = X(1);
        t->last_args[2] = X(2);
        t->svc_count++;
    }
    HDBG("svc", "svc 0x%02x x0=%llx x1=%llx x2=%llx x3=%llx (pc=%s)", imm, (unsigned long long)X(0),
         (unsigned long long)X(1), (unsigned long long)X(2), (unsigned long long)X(3),
         symbolize(pc).c_str());
    {   // RECOMP_MEM_LOG=1 : appels système qui touchent aux mappages (permissions, alias, mémoire partagée)
        static const bool memlog = getenv("RECOMP_MEM_LOG") != nullptr;
        if (memlog && ((imm >= 0x02 && imm <= 0x05) || (imm >= 0x13 && imm <= 0x15) || imm == 0x51 || imm == 0x52))
            HLOG("svc", "svc 0x%02x x0=%llx x1=%llx x2=%llx x3=%llx (pc=%s)", imm, (unsigned long long)X(0),
                 (unsigned long long)X(1), (unsigned long long)X(2), (unsigned long long)X(3), symbolize(pc).c_str());
    }
    switch (imm) {
    case 0x01: {  // SetHeapSize(size=x1) → x1 = adresse
        uint64_t size = X(1);
        if (size & 0x1FFFFF) { RET(kr::InvalidSize); return; }
        if (size > HEAP_SIZE) { RET(kr::OutOfMemory); return; }
        std::lock_guard<std::mutex> lk(g_k);
        if (size > g_heap_size) {
            if (!mem_commit(HEAP_BASE + g_heap_size, size - g_heap_size)) { RET(kr::OutOfMemory); return; }
            mem_set(HEAP_BASE + g_heap_size, size - g_heap_size, MS_NORMAL, MP_RW);
        } else if (size < g_heap_size) {
            mem_decommit(HEAP_BASE + size, g_heap_size - size);
            mem_set(HEAP_BASE + size, g_heap_size - size, MS_FREE, MP_NONE);
        }
        HINFO("svc", "SetHeapSize(0x%llx)", (unsigned long long)size);
        g_heap_size = size;
        X(1) = HEAP_BASE;
        RET(0);
        return;
    }
    case 0x02:  // SetMemoryPermission(addr, size, perm)
        mem_set_perm(X(0), X(1), W(2));
        RET(0);
        return;
    case 0x03:  // SetMemoryAttribute(addr, size, mask, attr)
        mem_set_attr(X(0), X(1), W(2), W(3));
        RET(0);
        return;
    case 0x04: {  // MapMemory(dst, src, size) : déplacement (src devient inaccessible)
        uint64_t dst = X(0), src = X(1), size = X(2);
        std::lock_guard<std::mutex> lk(g_k);
        if (!mem_check(dst, size, MS_FREE)) { RET(kr::InvalidCurrentMemory); return; }
        if (!mem_commit(dst, size)) { RET(kr::OutOfMemory); return; }
        memcpy((void*)dst, (void*)src, size);
        bool stack = dst >= STACK_BASE && dst < STACK_BASE + STACK_SIZE;
        mem_set(dst, size, stack ? MS_STACK : MS_ALIAS, MP_RW);
        mem_set_attr(src, size, MA_LOCKED, MA_LOCKED);
        mem_set_perm(src, size, MP_NONE);
        g_maps[dst] = Mapping{src, size};
        HDBG("svc", "MapMemory 0x%llx ← 0x%llx (0x%llx)", (unsigned long long)dst,
             (unsigned long long)src, (unsigned long long)size);
        RET(0);
        return;
    }
    case 0x05: {  // UnmapMemory(dst, src, size)
        uint64_t dst = X(0), src = X(1), size = X(2);
        std::lock_guard<std::mutex> lk(g_k);
        memcpy((void*)src, (void*)dst, size);
        mem_decommit(dst, size);
        mem_set(dst, size, MS_FREE, MP_NONE);
        mem_set_attr(src, size, MA_LOCKED, 0);
        mem_set_perm(src, size, MP_RW);
        g_maps.erase(dst);
        RET(0);
        return;
    }
    case 0x06: {  // QueryMemory(MemoryInfo* x0, addr x2) → w1 = PageInfo
        MemInfo mi = mem_query(X(2));
        memcpy((void*)X(0), &mi, sizeof mi);
        X(1) = 0;
        RET(0);
        return;
    }
    case 0x07:  // ExitProcess
        HLOG("kernel", "ExitProcess demandé par l'invité (%s)", symbolize(pc).c_str());
        fflush(nullptr);
        ExitProcess(0);
    case 0x08: {  // CreateThread(entry x1, arg x2, sp x3, prio w4, core w5) → w1 handle
        std::lock_guard<std::mutex> lk(g_k);
        int core = (int32_t)W(5);
        if (core == -2) core = t ? t->core : 0;
        auto th = make_thread(X(1), X(2), X(3), (int32_t)W(4), core);
        th->handle = new_handle(th);
        th->ctx->fpcr = t ? t->ctx->fpcr : 0;
        X(1) = th->handle;
        HINFO("svc", "CreateThread %s arg=0x%llx sp=0x%llx prio=%d → 0x%x",
              symbolize(th->entry).c_str(), (unsigned long long)th->arg,
              (unsigned long long)th->stack_top, th->prio, th->handle);
        RET(0);
        return;
    }
    case 0x09: {  // StartThread(handle)
        std::lock_guard<std::mutex> lk(g_k);
        KThread* th = get<KThread>(W(0));
        if (!th) { RET(kr::InvalidHandle); return; }
        if (th->started) { RET(kr::InvalidState); return; }
        start_host_thread(th);
        RET(0);
        return;
    }
    case 0x0A: {  // ExitThread
        HINFO("svc", "ExitThread (thread %llu)", (unsigned long long)(t ? t->id : 0));
        {
            std::lock_guard<std::mutex> lk(g_k);
            if (t) {
                t->exited = true;
                notify_sync(t);
            }
        }
        _endthreadex(0);
        return;
    }
    case 0x0B: {  // SleepThread(ns)
        int64_t ns = (int64_t)X(0);
        if (ns <= 0) SwitchToThread();
        else precise_sleep(t_cur, ns);
        return;
    }
    case 0x0C: {  // GetThreadPriority(handle w1) → w1
        std::lock_guard<std::mutex> lk(g_k);
        KThread* th = get<KThread>(W(1));
        if (!th) { RET(kr::InvalidHandle); return; }
        X(1) = (uint32_t)th->prio;
        RET(0);
        return;
    }
    case 0x0D: {  // SetThreadPriority(handle, prio)
        std::lock_guard<std::mutex> lk(g_k);
        KThread* th = get<KThread>(W(0));
        if (!th) { RET(kr::InvalidHandle); return; }
        th->prio = (int32_t)W(1);
        RET(0);
        return;
    }
    case 0x0E: {  // GetThreadCoreMask(handle w2) → w1 core, x2 mask
        std::lock_guard<std::mutex> lk(g_k);
        KThread* th = get<KThread>(W(2));
        if (!th) { RET(kr::InvalidHandle); return; }
        X(1) = (uint32_t)th->core;
        X(2) = th->affinity;
        RET(0);
        return;
    }
    case 0x0F: {  // SetThreadCoreMask(handle, core, mask)
        std::lock_guard<std::mutex> lk(g_k);
        KThread* th = get<KThread>(W(0));
        if (!th) { RET(kr::InvalidHandle); return; }
        int32_t core = (int32_t)W(1);
        if (core >= 0) th->core = core;
        if (X(2)) th->affinity = X(2);
        RET(0);
        return;
    }
    case 0x10:  // GetCurrentProcessorNumber
        X(0) = t ? (uint32_t)t->core : 0;
        return;
    case 0x11: {  // SignalEvent(writable)
        std::lock_guard<std::mutex> lk(g_k);
        uint32_t fl = 0;
        KEvent* e = get<KEvent>(W(0), &fl);
        if (!e) { RET(kr::InvalidHandle); return; }
        e->signaled = true;
        notify_sync(e);
        RET(0);
        return;
    }
    case 0x12:    // ClearEvent
    case 0x17: {  // ResetSignal
        std::lock_guard<std::mutex> lk(g_k);
        KEvent* e = get<KEvent>(W(0));
        if (!e) {
            KObject* o = get_any(W(0));
            if (o) { RET(0); return; }   // autre objet signalable : ignoré
            RET(kr::InvalidHandle);
            return;
        }
        if (imm == 0x17 && !e->signaled) { RET(kr::InvalidState); return; }
        e->signaled = false;
        RET(0);
        return;
    }
    case 0x13: {  // MapSharedMemory(handle, addr, size, perm)
        std::lock_guard<std::mutex> lk(g_k);
        KSharedMemory* sm = get<KSharedMemory>(W(0));
        if (!sm) { RET(kr::InvalidHandle); return; }
        uint64_t addr = X(1), size = X(2);
        if (!mem_commit(addr, size)) { RET(kr::InvalidCurrentMemory); return; }
        memcpy((void*)addr, sm->backing.data(), std::min<uint64_t>(size, sm->backing.size()));
        mem_set(addr, size, MS_SHARED, W(3) & 3);
        sm->mapped = addr;
        RET(0);
        return;
    }
    case 0x14: {  // UnmapSharedMemory(handle, addr, size)
        std::lock_guard<std::mutex> lk(g_k);
        KSharedMemory* sm = get<KSharedMemory>(W(0));
        if (!sm) { RET(kr::InvalidHandle); return; }
        uint64_t addr = X(1), size = X(2);
        memcpy(sm->backing.data(), (void*)addr, std::min<uint64_t>(size, sm->backing.size()));
        mem_decommit(addr, size);
        mem_set(addr, size, MS_FREE, MP_NONE);
        sm->mapped = 0;
        RET(0);
        return;
    }
    case 0x15: {  // CreateTransferMemory(addr x1, size x2, perm w3) → w1
        std::lock_guard<std::mutex> lk(g_k);
        auto tm = std::make_shared<KTransferMemory>();
        tm->addr = X(1);
        tm->size = X(2);
        tm->perm = W(3);
        X(1) = new_handle(tm);
        RET(0);
        return;
    }
    case 0x16: {  // CloseHandle
        std::lock_guard<std::mutex> lk(g_k);
        uint32_t h = W(0);
        if (h == CUR_THREAD || h == CUR_PROCESS) { RET(0); return; }
        if (!g_handles.erase(h)) { RET(kr::InvalidHandle); return; }
        RET(0);
        return;
    }
    case 0x18: {  // WaitSynchronization(handles x1, count w2, timeout x3) → w1 index
        uint64_t hp = X(1);
        int32_t n = (int32_t)W(2);
        int64_t to = timeout_of(X(3));
        if (n < 0 || n > 0x40) { RET(kr::OutOfRange); return; }
        std::unique_lock<std::mutex> lk(g_k);
        std::vector<KObject*> objs;
        for (int i = 0; i < n; i++) {
            uint32_t h;
            memcpy(&h, (void*)(hp + 4 * i), 4);
            KObject* o = get_any(h);
            if (!o) {
                static std::set<uint32_t> bad;
                if (bad.insert(h).second)
                    HLOG("svc", "WaitSynchronization : handle invalide 0x%x (thread %llu, appelant %s)", h,
                         (unsigned long long)(t ? t->id : 0), symbolize(X(30)).c_str());
                X(1) = (uint64_t)-1;
                RET(kr::InvalidHandle);
                return;
            }
            objs.push_back(o);
        }
        if (t->cancel_pending) {
            t->cancel_pending = false;
            X(1) = (uint32_t)-1;
            RET(kr::Cancelled);
            return;
        }
        for (int i = 0; i < n; i++)
            if (objs[i]->is_signaled()) { X(1) = i; RET(0); return; }
        if (to == 0) { X(1) = (uint32_t)-1; RET(kr::TimedOut); return; }
        prepare_wait(t);
        t->in_sync = true;
        t->wait_objs = objs;
        for (KObject* o : objs) o->sync_waiters.push_back(t);
        bool ok = block(lk, t, to);
        for (KObject* o : objs)
            o->sync_waiters.erase(std::remove(o->sync_waiters.begin(), o->sync_waiters.end(), t),
                                  o->sync_waiters.end());
        t->in_sync = false;
        t->wait_objs.clear();
        if (!ok) { X(1) = (uint32_t)-1; RET(kr::TimedOut); return; }
        X(1) = (uint32_t)t->wait_index;
        RET(t->wait_result);
        return;
    }
    case 0x19: {  // CancelSynchronization(thread)
        std::lock_guard<std::mutex> lk(g_k);
        KThread* th = get<KThread>(W(0));
        if (!th) { RET(kr::InvalidHandle); return; }
        if (th->in_sync && !th->woken) wake(th, kr::Cancelled, -1);
        else th->cancel_pending = true;
        RET(0);
        return;
    }
    case 0x1A: {  // ArbitrateLock(owner w0, addr x1, tag w2)
        uint32_t owner = W(0), tag = W(2);
        uint64_t addr = X(1);
        if (addr & 3) { RET(kr::InvalidAddress); return; }
        std::unique_lock<std::mutex> lk(g_k);
        if (atomic_load32(addr) != (owner | HAS_WAITERS)) { RET(0); return; }
        prepare_wait(t);
        t->tag = tag;
        t->where = 1;
        t->where_key = addr;
        g_mutex_q[addr].push_back(t);
        block(lk, t, -1);
        RET(t->wait_result);
        return;
    }
    case 0x1B: {  // ArbitrateUnlock(addr)
        uint64_t addr = X(0);
        if (addr & 3) { RET(kr::InvalidAddress); return; }
        std::lock_guard<std::mutex> lk(g_k);
        mutex_release(addr);
        RET(0);
        return;
    }
    case 0x1C: {  // WaitProcessWideKeyAtomic(mutex x0, key x1, tag w2, timeout x3)
        uint64_t maddr = X(0), key = X(1);
        uint32_t tag = W(2);
        int64_t to = timeout_of(X(3));
        std::unique_lock<std::mutex> lk(g_k);
        // Comme le noyau Horizon (KConditionVariable::Wait) : la condvar est marquée « avec
        // attendants » AVANT la libération du mutex. Signal()/Broadcast() du SDK lisent ce mot en
        // mode utilisateur et ne font pas l'appel système s'il vaut 0 ; écrit après la libération,
        // un thread qui prenait le mutex entre les deux signalait dans le vide et l'attendant
        // dormait pour toujours (interblocage Wwise : événement signalé, AudioMgr endormi).
        atomic_store32(key, 1);
        mutex_release(maddr);
        prepare_wait(t);
        t->tag = tag;
        t->cv_mutex = maddr;
        t->cv_key = key;
        t->where = 2;
        t->where_key = key;
        g_cv_q[key].push_back(t);
        bool ok = to == 0 ? false : block(lk, t, to);
        if (!ok && !t->woken) {
            if (t->where == 2) remove_from(g_cv_q, key, t);
            else if (t->where == 1) remove_from(g_mutex_q, t->where_key, t);
            t->where = 0;
            RET(kr::TimedOut);
            return;
        }
        RET(t->wait_result);
        return;
    }
    case 0x1D: {  // SignalProcessWideKey(key x0, count w1)
        uint64_t key = X(0);
        int32_t count = (int32_t)W(1);
        std::lock_guard<std::mutex> lk(g_k);
        auto it = g_cv_q.find(key);
        int n = 0;
        if (it != g_cv_q.end()) {
            auto& q = it->second;
            while (!q.empty() && (count <= 0 || n < count)) {
                KThread* w = q.front();
                q.pop_front();
                n++;
                w->where = 0;
                cv_reacquire(w);
            }
            if (q.empty()) {
                g_cv_q.erase(it);
                atomic_store32(key, 0);
            }
        }
        return;
    }
    case 0x1F: {  // ConnectToNamedPort(name x1) → w1
        char name[13] = {};
        memcpy(name, (void*)X(1), 12);
        uint32_t h = ipc_connect_named_port(name);
        if (!h) { RET(kr::NotFound); return; }
        X(1) = h;
        RET(0);
        return;
    }
    case 0x21:  // SendSyncRequest(session)
        RET(ipc_send_sync_request(ctx, W(0), t->tlr, 0x100));
        return;
    case 0x22:  // SendSyncRequestWithUserBuffer(buf, size, session)
        RET(ipc_send_sync_request(ctx, W(2), X(0), X(1)));
        return;
    case 0x25: {  // GetThreadId(handle w1) → x1
        std::lock_guard<std::mutex> lk(g_k);
        KThread* th = get<KThread>(W(1));
        if (!th) { RET(kr::InvalidHandle); return; }
        X(1) = th->id;
        RET(0);
        return;
    }
    case 0x26: {  // Break(reason, arg, size)
        uint32_t reason = W(0);
        HLOG("kernel", "svcBreak(raison 0x%x, arg 0x%llx, taille 0x%llx) depuis %s", reason,
             (unsigned long long)X(1), (unsigned long long)X(2), symbolize(pc).c_str());
        if (reason & 0x80000000u) { RET(0); return; }   // notification seulement
        panic("svcBreak : l'invité s'est arrêté (raison 0x%x)", reason);
    }
    case 0x27: {  // OutputDebugString(str, size)
        std::string s((const char*)X(0), (size_t)X(1));
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        HLOG("invité", "%s", s.c_str());
        RET(0);
        return;
    }
    case 0x29: {  // GetInfo(id w1, handle w2, sub x3) → x1
        uint32_t res;
        uint64_t v = info_value(W(1), W(2), X(3), res);
        HDBG("svc", "GetInfo(%u, 0x%x, %llu) = 0x%llx", W(1), W(2), (unsigned long long)X(3),
             (unsigned long long)v);
        X(1) = v;
        RET(res);
        return;
    }
    case 0x2C: {  // MapPhysicalMemory(addr, size)
        HINFO("mem", "MapPhysicalMemory 0x%llx +0x%llx (thread %llu)", (unsigned long long)X(0),
              (unsigned long long)X(1), (unsigned long long)(t ? t->id : 0));
        std::lock_guard<std::mutex> lk(g_k);
        if (!mem_commit(X(0), X(1))) { RET(kr::OutOfMemory); return; }
        mem_set(X(0), X(1), MS_NORMAL, MP_RW);
        RET(0);
        return;
    }
    case 0x2D: {  // UnmapPhysicalMemory(addr, size)
        HINFO("mem", "UnmapPhysicalMemory 0x%llx +0x%llx (thread %llu)", (unsigned long long)X(0),
              (unsigned long long)X(1), (unsigned long long)(t ? t->id : 0));
        std::lock_guard<std::mutex> lk(g_k);
        mem_decommit(X(0), X(1));
        mem_set(X(0), X(1), MS_FREE, MP_NONE);
        RET(0);
        return;
    }
    case 0x32:  // SetThreadActivity
        RET(0);
        return;
    case 0x34: {  // WaitForAddress(addr x0, type w1, value w2/x2, timeout x3)
        uint64_t addr = X(0);
        uint32_t type = W(1);
        int32_t value = (int32_t)W(2);
        int64_t to = timeout_of(X(3));
        std::unique_lock<std::mutex> lk(g_k);
        uint32_t uv = atomic_load32(addr);
        switch (type) {
        case 0:  // WaitIfLessThan
            if (!((int32_t)uv < value)) { RET(kr::InvalidState); return; }
            break;
        case 1:  // DecrementAndWaitIfLessThan
            for (;;) {
                if (!((int32_t)uv < value)) { RET(kr::InvalidState); return; }
                if (cas32(addr, uv, uv - 1)) break;
            }
            break;
        case 2:  // WaitIfEqual
            if ((int32_t)uv != value) { RET(kr::InvalidState); return; }
            break;
        case 3: {  // WaitIfEqual64
            uint64_t v64 = __atomic_load_n((uint64_t*)addr, __ATOMIC_SEQ_CST);
            if (v64 != X(2)) { RET(kr::InvalidState); return; }
            break;
        }
        default: RET(kr::InvalidEnumValue); return;
        }
        if (to == 0) { RET(kr::TimedOut); return; }
        prepare_wait(t);
        t->where = 3;
        t->where_key = addr;
        g_arb_q[addr].push_back(t);
        if (!block(lk, t, to)) {
            remove_from(g_arb_q, addr, t);
            t->where = 0;
            RET(kr::TimedOut);
            return;
        }
        RET(t->wait_result);
        return;
    }
    case 0x35: {  // SignalToAddress(addr x0, type w1, value w2, count w3)
        uint64_t addr = X(0);
        uint32_t type = W(1);
        int32_t value = (int32_t)W(2), count = (int32_t)W(3);
        std::lock_guard<std::mutex> lk(g_k);
        auto it = g_arb_q.find(addr);
        size_t nw = it == g_arb_q.end() ? 0 : it->second.size();
        if (type == 1 || type == 2) {
            int32_t nv;
            if (type == 1) nv = value + 1;
            else if (count <= 0) nv = nw ? value - 2 : value + 1;
            else if (nw) nv = ((int64_t)nw - 1 < count) ? value - 1 : value;   // tous réveillés ?
            else nv = value + 1;
            uint32_t expect = (uint32_t)value;
            if (!cas32(addr, expect, (uint32_t)nv)) { RET(kr::InvalidState); return; }
        } else if (type != 0) {
            RET(kr::InvalidEnumValue);
            return;
        }
        if (it != g_arb_q.end()) {
            auto& q = it->second;
            int n = 0;
            while (!q.empty() && (count <= 0 || n < count)) {
                KThread* w = q.front();
                q.pop_front();
                wake(w, 0);
                n++;
            }
            if (q.empty()) g_arb_q.erase(it);
        }
        RET(0);
        return;
    }
    case 0x36:  // SynchronizePreemptionState
        return;
    case 0x45: {  // CreateEvent → w1 writable, w2 readable
        std::lock_guard<std::mutex> lk(g_k);
        auto e = std::make_shared<KEvent>();
        e->tag = "CreateEvent←" + symbolize(X(30));
        t_last_event = e;
        X(1) = new_handle(e, HF_WRITE);
        X(2) = new_handle(e, HF_READ);
        RET(0);
        return;
    }
    case 0x50: {  // CreateSharedMemory(size x1, local w2, remote w3) → w1
        std::lock_guard<std::mutex> lk(g_k);
        auto sm = std::make_shared<KSharedMemory>();
        sm->size = X(1);
        sm->backing.assign(X(1), 0);
        X(1) = new_handle(sm);
        RET(0);
        return;
    }
    default:
        panic("svc 0x%x non implémenté (x0=0x%llx x1=0x%llx x2=0x%llx x3=0x%llx) depuis %s", imm,
              (unsigned long long)X(0), (unsigned long long)X(1), (unsigned long long)X(2),
              (unsigned long long)X(3), symbolize(pc).c_str());
    }
}

// Accès pour l'IPC : création d'événements/mémoire partagée côté services
uint32_t kernel_create_event(std::shared_ptr<KObject>* out, bool signaled, const char* tag) {
    std::lock_guard<std::mutex> lk(g_k);
    auto e = std::make_shared<KEvent>();
    e->signaled = signaled;
    e->tag = tag;
    if (out) *out = e;
    return new_handle(e, HF_READ);
}

std::string kernel_describe(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_k);
    KObject* o = get_any(h);
    return o ? std::string(o->kind()) + " " + o->tag : "?";
}

uint32_t kernel_new_handle(std::shared_ptr<KObject> o) {
    std::lock_guard<std::mutex> lk(g_k);
    return new_handle(std::move(o));
}

KObject* kernel_get(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_k);
    return get_any(h);
}

uint32_t kernel_create_shared_memory(uint64_t size, std::shared_ptr<KObject>* out) {
    std::lock_guard<std::mutex> lk(g_k);
    auto sm = std::make_shared<KSharedMemory>();
    sm->size = size;
    sm->backing.assign(size, 0);
    if (out) *out = sm;
    return new_handle(sm);
}

uint8_t* kernel_shmem_ptr(KObject* o) {
    auto* sm = dynamic_cast<KSharedMemory*>(o);
    if (!sm) return nullptr;
    return sm->mapped ? (uint8_t*)sm->mapped : sm->backing.data();
}

void (*g_dump_host_thread)(void* handle, uint64_t id) = nullptr;

std::vector<std::pair<uint64_t, void*>> kernel_thread_list() {
    std::vector<std::pair<uint64_t, void*>> v;
    std::lock_guard<std::mutex> lk(g_k);
    for (auto& t : g_threads)
        if (t->started && !t->exited && t->host) v.emplace_back(t->id, (void*)t->host);
    return v;
}

void kernel_dump_threads() {
    std::vector<std::pair<HANDLE, uint64_t>> running;
    {
    std::lock_guard<std::mutex> lk(g_k);
    HLOG("dump", "%zu threads invités :", g_threads.size());
    for (auto& t : g_threads) {
        const char* st = t->exited ? "terminé" : !t->started ? "non démarré"
                         : t->in_sync ? "WaitSynchronization"
                         : t->where == 1 ? "mutex (ArbitrateLock)"
                         : t->where == 2 ? "condvar (WaitProcessWideKey)"
                         : t->where == 3 ? "WaitForAddress" : "en cours / autre";
        std::string objs;
        for (KObject* o : t->wait_objs) objs += std::string(" ") + o->kind();
        HLOG("dump", "  thread %llu prio %d [%s] clé 0x%llx%s | entrée %s | lr %s | sp 0x%llx | dernier svc 0x%x (%llx %llx %llx) → 0x%x / 0x%llx, %llu svc",
             (unsigned long long)t->id, t->prio, st, (unsigned long long)t->where_key, objs.c_str(),
             symbolize(t->entry).c_str(), symbolize(t->ctx->x[30]).c_str(), (unsigned long long)t->ctx->sp,
             t->last_svc, (unsigned long long)t->last_args[0], (unsigned long long)t->last_args[1],
             (unsigned long long)t->last_args[2], t->last_result, (unsigned long long)t->last_out1,
             (unsigned long long)t->svc_count);
        if (t->started && !t->exited) {   // appelants (chaîne x29 invitée)
            std::string chain;
            for (uint64_t fp = t->ctx->x[29], i = 0; i < 10 && fp && mem_valid(fp, 16); i++) {
                char b[32];
                snprintf(b, sizeof b, " [fp %llx]", (unsigned long long)fp);
                chain += " ← " + symbolize(*(uint64_t*)(fp + 8)) + b;
                fp = *(uint64_t*)fp;
            }
            HLOG("dump", "      appelants :%s", chain.c_str());
        }
        if (t->started && !t->exited)
            HLOG("dump", "      x19-x24 : %llx %llx %llx %llx %llx %llx", (unsigned long long)t->ctx->x[19],
                 (unsigned long long)t->ctx->x[20], (unsigned long long)t->ctx->x[21], (unsigned long long)t->ctx->x[22],
                 (unsigned long long)t->ctx->x[23], (unsigned long long)t->ctx->x[24]);
        if (t->where == 2)
            HLOG("dump", "      condvar 0x%llx (valeur %u), mutex 0x%llx (valeur 0x%x), étiquette 0x%x",
                 (unsigned long long)t->cv_key, atomic_load32(t->cv_key), (unsigned long long)t->cv_mutex,
                 atomic_load32(t->cv_mutex), t->tag);
        if (t->where == 1 || t->where == 3)
            HLOG("dump", "      adresse 0x%llx (valeur 0x%x)", (unsigned long long)t->where_key,
                 atomic_load32(t->where_key));
        if ((t->where == 1 || t->where == 3) && mem_valid(t->where_key - 0x18, 0x30)) {   // objet englobant
            const uint32_t* w = (const uint32_t*)(t->where_key - 0x18);
            HLOG("dump", "      objet -0x18 : %08x %08x %08x %08x %08x %08x | %08x %08x %08x %08x %08x %08x", w[0], w[1],
                 w[2], w[3], w[4], w[5], w[6], w[7], w[8], w[9], w[10], w[11]);
        }
        if (t->started && !t->exited && !t->in_sync && !t->where) running.emplace_back(t->host, t->id);
        if (t->last_svc == 0x18 && t->last_args[1]) {   // WaitSynchronization : objets attendus
            for (int i = 0; i < (int)(uint32_t)t->last_args[2] && i < 8; i++) {
                uint32_t h;
                memcpy(&h, (void*)(t->last_args[1] + 4 * i), 4);
                KObject* o = get_any(h);
                HLOG("dump", "      handle 0x%x : %s %s%s", h, o ? o->kind() : "?", o ? o->tag.c_str() : "",
                     o && o->is_signaled() ? " (signalé)" : "");
            }
        }
    }
    }
    // RECOMP_PEEK=adresse[:octets],... : contenu de la mémoire invitée (diagnostic)
    if (const char* pk = getenv("RECOMP_PEEK")) {
        for (const char* q = pk; *q;) {
            uint64_t a = strtoull(q, (char**)&q, 0), n = 64;
            if (*q == ':') n = strtoull(q + 1, (char**)&q, 0);
            for (uint64_t o = 0; o < n && mem_valid(a + o, 16); o += 16) {
                const uint32_t* w = (const uint32_t*)(a + o);
                HLOG("dump", "  mem %llx : %08x %08x %08x %08x", (unsigned long long)(a + o), w[0], w[1], w[2], w[3]);
            }
            if (*q == ',') q++;
            else break;
        }
    }
    // piles d'appels des threads qui ne sont pas bloqués dans le noyau
    if (g_dump_host_thread)
        for (auto& [h, id] : running) g_dump_host_thread(h, id);
}

void kernel_signal(KObject* o, bool on) {
    std::lock_guard<std::mutex> lk(g_k);
    if (auto* e = dynamic_cast<KEvent*>(o)) {
        e->signaled = on;
        if (on) notify_sync(e);
    }
}

void kernel_close(uint32_t h) {
    std::lock_guard<std::mutex> lk(g_k);
    g_handles.erase(h);
}

}  // namespace hos
