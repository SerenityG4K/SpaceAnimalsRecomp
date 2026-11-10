// Point d'entrée de l'exécutable natif : journal, gestionnaire de plantage (pile d'appels
// invitée reconstituée depuis la pile hôte), chargement des modules et démarrage comme le fait
// rtld : nn::init::Start(handle du thread principal, adresse de main, rappels init/fini).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   // Windows 10+ : SetProcessInformation (minuteur)
#endif
#include "hos.h"
#include "settings.h"
#include "vk_render.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <string>
#include <thread>

namespace hos {

int g_verbose = 1;
bool g_trace_nvn = false;
extern void (*g_dump_host_thread)(void* handle, uint64_t id);

namespace {
std::mutex g_log_lock;
FILE* g_logfile = nullptr;
const auto g_t0 = std::chrono::steady_clock::now();

void vlog(const char* tag, const char* fmt, va_list ap) {
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    std::lock_guard<std::mutex> lk(g_log_lock);
    char head[64];
    snprintf(head, sizeof head, "[%9.3f] [%-6s] ", t, tag);
    fputs(head, stderr);
    fputs(msg, stderr);
    fputc('\n', stderr);
    if (g_logfile) {
        fputs(head, g_logfile);
        fputs(msg, g_logfile);
        fputc('\n', g_logfile);
        fflush(g_logfile);
    }
}

// table « adresse de fonction hôte → adresse invitée » pour reconstituer les piles d'appels
std::vector<std::pair<uint64_t, uint64_t>> g_host2guest;
uint64_t g_gen_end = 0;   // fin du code généré (le runtime est lié après les modules)

void build_host_map() {
    for (auto& [ga, fn] : rt_dispatch_table())
        if (ga < NATIVE_BASE) g_host2guest.emplace_back((uint64_t)fn, ga);
    std::sort(g_host2guest.begin(), g_host2guest.end());
    // borne : plus petite adresse connue du runtime (runtime.cpp est le 1er objet du runtime)
    uint64_t rt[] = {(uint64_t)&rt_register_functions, (uint64_t)&rt_lookup, (uint64_t)&rt_call,
                     (uint64_t)&rt_dispatch_finalize};
    g_gen_end = *std::min_element(std::begin(rt), std::end(rt)) - 0x800;
}

std::string host_frame(uint64_t rip) {
    char buf[96];
    auto it = std::upper_bound(g_host2guest.begin(), g_host2guest.end(), std::make_pair(rip, ~0ull));
    if (it != g_host2guest.begin() && rip < g_gen_end) {
        --it;
        if (rip - it->first < 0x200000) {
            snprintf(buf, sizeof buf, " (hôte +0x%llx)", (unsigned long long)(rip - it->first));
            return symbolize(it->second) + buf;
        }
    }
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)rip, &m);
    snprintf(buf, sizeof buf, "hôte 0x%llx (image+0x%llx)", (unsigned long long)rip,
             (unsigned long long)(rip - (uint64_t)m));
    return buf;
}

void print_backtrace(const CONTEXT* c0) {
    CONTEXT c;
    if (c0) c = *c0;
    else RtlCaptureContext(&c);
    HLOG("crash", "pile d'appels (fonctions invitées recompilées) :");
    for (int i = 0; i < 48 && c.Rip; i++) {
        uint64_t rip = c.Rip;
        HLOG("crash", "  #%-2d %s", i, host_frame(rip).c_str());
        DWORD64 imagebase = 0;
        PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(rip, &imagebase, nullptr);
        if (!rf) {
            c.Rip = *(DWORD64*)c.Rsp;
            c.Rsp += 8;
        } else {
            void* hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imagebase, rip, rf, &c, &hd, &est, nullptr);
        }
    }
}

void print_guest_state() {
    CPUState* ctx = current_ctx();
    if (!ctx) return;
    HLOG("crash", "registres invités (thread courant) :");
    for (int i = 0; i < 31; i += 4) {
        char line[256];
        int n = 0;
        for (int j = i; j < i + 4 && j < 31; j++)
            n += snprintf(line + n, sizeof line - n, " x%-2d=%016llx", j, (unsigned long long)ctx->x[j]);
        HLOG("crash", "%s", line);
    }
    HLOG("crash", " sp=%016llx lr=%s tpidr=%llx tpidrro=%llx", (unsigned long long)ctx->sp,
         symbolize(ctx->x[30]).c_str(), (unsigned long long)ctx->tpidr_el0,
         (unsigned long long)ctx->tpidrro_el0);
    // mémoire autour du cadre (pile invitée)
    for (uint64_t base : {ctx->x[29], ctx->sp}) {
        uint64_t a = (base & ~7ull) - 0x20;
        if (!mem_valid(a, 0x60)) continue;
        for (int i = 0; i < 0x60; i += 0x20) {
            uint64_t* q = (uint64_t*)(a + i);
            HLOG("crash", "  [0x%llx] %016llx %016llx %016llx %016llx", (unsigned long long)(a + i),
                 (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2],
                 (unsigned long long)q[3]);
        }
    }
    if (uint64_t g = find_export("__stack_chk_guard"))
        HLOG("crash", "  __stack_chk_guard = %016llx", (unsigned long long)*(uint64_t*)g);
}

std::atomic<int> g_crashing{0};

LONG WINAPI veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case 0xC0000409:   // STATUS_STACK_BUFFER_OVERRUN
        break;
    default:
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_crashing++) {
        Sleep(INFINITE);
    }
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        const char* kind = er->ExceptionInformation[0] == 0 ? "lecture"
                           : er->ExceptionInformation[0] == 1 ? "écriture" : "exécution";
        uint64_t a = er->ExceptionInformation[1];
        MemInfo mi = mem_query(a);
        HLOG("crash", "violation d'accès (%s) à 0x%llx [état invité 0x%x, perm %u] depuis %s (thread %llu)", kind,
             (unsigned long long)a, mi.state, mi.perm, host_frame((uint64_t)er->ExceptionAddress).c_str(),
             (unsigned long long)current_thread_id());
    } else {
        HLOG("crash", "exception 0x%08lx depuis %s", code, host_frame((uint64_t)er->ExceptionAddress).c_str());
    }
    print_guest_state();
    print_backtrace(ep->ContextRecord);
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

void dump_host_thread(void* h, uint64_t id) {
    HANDLE th = (HANDLE)h;
    if (SuspendThread(th) == (DWORD)-1) return;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &c)) {
        HLOG("dump", "pile du thread %llu :", (unsigned long long)id);
        print_backtrace(&c);
    }
    ResumeThread(th);
}


// --profile T : profil par échantillonnage (1 ms) de tous les threads invités pendant 10 s à
// partir de T secondes. Chaque échantillon est rangé dans la fonction invitée recompilée qui
// s'exécute, ou dans « runtime » / « système » (DLL Windows : attentes, synchronisation).
void run_profile(double start, double dur) {
    std::this_thread::sleep_for(std::chrono::duration<double>(start));
    HLOG("prof", "profil : %.0f s d'échantillonnage", dur);
    std::map<uint64_t, std::map<uint64_t, uint64_t>> per;   // thread → clé → n
    std::map<uint64_t, uint64_t> rt_rips;                    // RIP du runtime → n
    HMODULE self = GetModuleHandleW(nullptr);
    const uint64_t K_SYS = 1, K_RT = 2;
    auto t_end = std::chrono::steady_clock::now() + std::chrono::duration<double>(dur);
    uint64_t total = 0;
    while (std::chrono::steady_clock::now() < t_end) {
        for (auto& [id, h] : kernel_thread_list()) {
            HANDLE th = (HANDLE)h;
            if (SuspendThread(th) == (DWORD)-1) continue;
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL;
            bool ok = GetThreadContext(th, &c);
            ResumeThread(th);
            if (!ok) continue;
            uint64_t rip = c.Rip, key;
            auto it = std::upper_bound(g_host2guest.begin(), g_host2guest.end(), std::make_pair(rip, ~0ull));
            if (it != g_host2guest.begin() && rip < g_gen_end && rip - std::prev(it)->first < 0x200000) {
                key = std::prev(it)->second;
            } else {
                HMODULE m = nullptr;
                GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   (LPCSTR)rip, &m);
                key = m == self ? K_RT : K_SYS;
                if (key == K_RT) rt_rips[rip - (uint64_t)self]++;
            }
            per[id][key]++;
            total++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto name = [&](uint64_t k) -> std::string {
        if (k == K_SYS) return "[système : attentes, DLL Windows]";
        if (k == K_RT) return "[runtime : dispatch, noyau émulé, HLE]";
        return symbolize(k);
    };
    std::map<uint64_t, uint64_t> glob;
    for (auto& [id, m] : per) {
        uint64_t n = 0, busy = 0;
        for (auto& [k, v] : m) { n += v; if (k != K_SYS) busy += v; glob[k] += v; }
        if (!n) continue;
        HLOG("prof", "thread %llu : %llu échantillons, occupé %.0f %%", (unsigned long long)id,
             (unsigned long long)n, 100.0 * busy / n);
        std::vector<std::pair<uint64_t, uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 6 && busy * 10 > n; i++)
            HLOG("prof", "    %5.1f %%  %s", 100.0 * v[i].second / n, name(v[i].first).c_str());
    }
    std::vector<std::pair<uint64_t, uint64_t>> v(glob.begin(), glob.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    HLOG("prof", "global (%llu échantillons) :", (unsigned long long)total);
    for (size_t i = 0; i < v.size() && i < 30; i++)
        HLOG("prof", "  %5.1f %%  %s", 100.0 * v[i].second / total, name(v[i].first).c_str());
    std::vector<std::pair<uint64_t, uint64_t>> r(rt_rips.begin(), rt_rips.end());
    std::sort(r.begin(), r.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < r.size() && i < 25; i++)
        HLOG("prof", "  runtime image+0x%llx : %llu", (unsigned long long)r[i].first, (unsigned long long)r[i].second);
}

// --trace-api : appels de main vers le SDK (via PLT), par thread, premières occurrences
uint64_t g_main_lo = 0, g_main_hi = 0, g_sdk_lo = 0, g_sdk_hi = 0;
void api_trace(CPUState* ctx, uint64_t target) {
    if (target < g_sdk_lo || target >= g_sdk_hi) return;
    uint64_t lr = ctx->x[30];
    if (lr < g_main_lo || lr >= g_main_hi) return;
    static std::mutex m;
    static std::map<std::pair<uint64_t, uint64_t>, uint64_t> seen;
    uint64_t tid = current_thread_id(), n;
    {
        std::lock_guard<std::mutex> lk(m);
        n = seen[{tid, target}]++;
    }
    if (n < 3 || (n & (n - 1)) == 0)
        HLOG("api", "t%llu %s (n°%llu) ← %s", (unsigned long long)tid, symbolize(target).c_str(),
             (unsigned long long)n + 1, symbolize(lr).c_str());
}

void fatal_hook(const char* msg, uint64_t pc) {
    panic("%s (pc invité %s)", msg, symbolize(pc).c_str());
}

// ---- rappels passés par rtld à nn::init::Start
void native_nop(CPUState*) {}

void native_init_modules(CPUState* ctx) {
    // rtld insère chaque module en tête de liste : init dans l'ordre sdk → … → main
    for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
        if (!it->init) continue;
        HLOG("init", "%s : _init (%s)", it->name.c_str(), symbolize(it->init).c_str());
        call_guest(ctx, it->init);
    }
    HLOG("init", "constructeurs des modules terminés");
}

void native_fini_modules(CPUState* ctx) {
    for (auto& m : g_modules) {
        if (!m.fini) continue;
        HLOG("init", "%s : _fini", m.name.c_str());
        call_guest(ctx, m.fini);
    }
}

}  // namespace

void logf(const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(tag, fmt, ap);
    va_end(ap);
}

void panic(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("PANIQ", fmt, ap);
    va_end(ap);
    if (!g_crashing++) {
        print_guest_state();
        print_backtrace(nullptr);
    }
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), 2);
    for (;;) {}
}

}  // namespace hos

using namespace hos;

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::string manifest, logpath = "recomp_run.log";
    double dump_after = 0;
    bool trace_api = false;
    double profile_at = 0;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if ((a == "-v" || a == "--verbose") && i + 1 < argc) g_verbose = atoi(argv[++i]);
        else if (a == "--manifest" && i + 1 < argc) manifest = argv[++i];
        else if (a == "--log" && i + 1 < argc) logpath = argv[++i];
        else if (a == "--dump" && i + 1 < argc) dump_after = atof(argv[++i]);
        else if (a == "--trace-api") trace_api = true;
        else if (a == "--trace-nvn") g_trace_nvn = true;
        else if (a == "--profile" && i + 1 < argc) profile_at = atof(argv[++i]);
    }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--precompile")) {   // shaders compilés pour ce GPU, sans le jeu (launcher : premier lancement)
            char exe[MAX_PATH];
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string d = exe;
            if (logpath == "recomp_run.log") logpath = d.substr(0, d.find_last_of("\\/") + 1) + "precompile.log";
            g_logfile = fopen(logpath.c_str(), "w");
            const int code = vkr::precompile();
            fflush(nullptr);
            return code;
        }
    if (manifest.empty()) {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string d = exe;
        d = d.substr(0, d.find_last_of("\\/"));
        manifest = d + "\\manifest.txt";
    }
    g_logfile = fopen(logpath.c_str(), "w");
    settings_init(argc, argv);   // recomp.ini, RECOMP_*, --refresh / --resolution / --native-res / --config
    // Minuteur à 1 ms. Windows 11 ignore timeBeginPeriod pour un processus dont la fenêtre est
    // masquée ou en arrière-plan (les attentes courtes durent alors 15,6 ms, et le jeu tombe à
    // ~42 images/s) ; on demande explicitement que la résolution soit respectée et que le
    // processus ne soit pas bridé (EcoQoS).
    {
        PROCESS_POWER_THROTTLING_STATE st{};
        st.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        st.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION | PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
        st.StateMask = 0;
        SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &st, sizeof st);
    }
    timeBeginPeriod(1);
    mem_init();   // en premier : réserve l'espace invité avant toute autre allocation
    AddVectoredExceptionHandler(1, veh);
    rt_fatal_hook = fatal_hook;
    g_dump_host_thread = dump_host_thread;
    kernel_init();
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; i++) Sleep(1);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10;
        HLOG("main", "Sleep(1) dure %.2f ms (résolution du minuteur)", ms);
    }
    HLOG("main", "chargement des modules (%s)", manifest.c_str());
    load_modules(manifest);
    // données propres à rtld : nn::ro y range sa fonction de recherche globale de symboles
    // (sdk!nn::ro::detail::g_pLookupGlobalManualFunctionPointer pointe vers une case de rtld)
    mem_commit(RTLD_DATA, 0x1000);
    mem_set(RTLD_DATA, 0x1000, MS_CODEDATA, MP_RW);
    if (uint64_t p = find_export("_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE")) {
        uint64_t slot = RTLD_DATA;
        memcpy((void*)p, &slot, 8);
    }
    gfx_init();
    hle_os_init();
    hle_audio_init();
    uint64_t f2 = register_native("rtld:marquer_initialisé", native_nop);
    uint64_t f3 = register_native("rtld:init_modules", native_init_modules);
    uint64_t f4 = register_native("rtld:fini_modules", native_fini_modules);
    rt_dispatch_finalize();
    build_host_map();
    for (auto& m : g_modules) {
        if (m.name == "main") { g_main_lo = m.base; g_main_hi = m.base + m.size; }
        if (m.name == "sdk") { g_sdk_lo = m.base; g_sdk_hi = m.base + m.size; }
    }
    if (trace_api) rt_call_hook = api_trace;
    HLOG("main", "%zu fonctions dans la table de dispatch", rt_dispatch_table().size());
    uint64_t start = find_export("_ZN2nn4init5StartEmmPFvvES2_S2_");
    if (!start || !rt_lookup(start)) panic("nn::init::Start introuvable");
    const Module* mainm = nullptr;
    for (auto& m : g_modules)
        if (m.name == "main") mainm = &m;
    HLOG("main", "démarrage : nn::init::Start @ %s", symbolize(start).c_str());
    // pile du thread principal : 1 Mo (main.npdm), priorité 44
    if (dump_after > 0)
        std::thread([dump_after] {
            std::this_thread::sleep_for(std::chrono::duration<double>(dump_after));
            kernel_dump_threads();
        }).detach();
    if (profile_at > 0) std::thread([profile_at] { run_profile(profile_at, 10); }).detach();
    run_main_thread(start, 0, mainm ? mainm->base : 0, f2, f3, f4, 0x100000);
    HLOG("main", "fin");
    fflush(nullptr);
    return 0;
}
