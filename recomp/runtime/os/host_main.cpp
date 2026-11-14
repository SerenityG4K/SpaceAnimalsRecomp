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
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <exception>
#include <new>
#include <malloc.h>
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

// Chemin du profil Windows (le dossier personnel, qui porte le nom d'utilisateur) remplacé par %USERPROFILE% dans
// chaque ligne du journal : un
// recomp_run.log joint à la main à une issue publique ne donne plus le nom d'utilisateur (issue GitHub n° 19 ; le
// rapport de bug du launcher l'anonymisait déjà, rapport.py). Sans allocation : vlog sert aussi pendant un plantage.
// Appelé sous g_log_lock. RECOMP_LOG_PATHS=1 : chemins laissés tels quels.
void masquer_profil(char* msg, size_t taille) {
    static char profil[520];
    static size_t n = (size_t)-1;
    if (n == (size_t)-1) {
        const wchar_t* w = getenv("RECOMP_LOG_PATHS") ? nullptr : _wgetenv(L"USERPROFILE");
        const int k = w ? WideCharToMultiByte(CP_UTF8, 0, w, -1, profil, (int)sizeof profil, nullptr, nullptr) : 0;
        n = k > 5 ? (size_t)k - 1 : 0;   // au moins « C:\xy »
    }
    if (!n) return;
    static const char remplacement[] = "%USERPROFILE%";
    const size_t r = sizeof remplacement - 1;
    for (char* p = msg; *p;) {
        if ((p[0] | 0x20) == (profil[0] | 0x20) && !_strnicmp(p, profil, n)) {
            const size_t reste = strlen(p + n);
            if (r > n && (size_t)(p - msg) + r + reste + 1 > taille) return;   // ne tiendrait pas : ligne laissée
            memmove(p + r, p + n, reste + 1);
            memcpy(p, remplacement, r);
            p += r;
        } else {
            p++;
        }
    }
}

void vlog(const char* tag, const char* fmt, va_list ap) {
    char msg[4096];
    vsnprintf(msg, sizeof msg, fmt, ap);
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count();
    std::lock_guard<std::mutex> lk(g_log_lock);
    masquer_profil(msg, sizeof msg);
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
            snprintf(buf, sizeof buf, " (host +0x%llx)", (unsigned long long)(rip - it->first));
            return symbolize(it->second) + buf;
        }
    }
    HMODULE m = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)rip, &m);
    snprintf(buf, sizeof buf, "host 0x%llx (image+0x%llx)", (unsigned long long)rip,
             (unsigned long long)(rip - (uint64_t)m));
    return buf;
}

void print_backtrace(const CONTEXT* c0) {
    CONTEXT c;
    if (c0) c = *c0;
    else RtlCaptureContext(&c);
    HLOG("crash", "call stack (recompiled guest functions):");
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
    HLOG("crash", "guest registers (current thread):");
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
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    // premier accès à une page de la mémoire du jeu engagée à la demande (memory.cpp) : on l'engage et on reprend
    if (code == EXCEPTION_ACCESS_VIOLATION && er->ExceptionInformation[0] <= 1) {
        const int r = mem_fault(er->ExceptionInformation[1], er->ExceptionInformation[0] == 1);
        if (r >= 2) {   // données de texture rendues puis redemandées : qui y revient ? (les 20 premières fois)
            static std::atomic<int> dits{0};
            if (dits++ < 20)
                HLOG("mem", "released texture data at 0x%llx %s from %s (thread %llu)",
                     (unsigned long long)er->ExceptionInformation[1],
                     r == 4 ? "rewritten by the game (range dropped)" : r == 5 ? "written (unused texture, restored)"
                     : r == 3 ? "written (texture in use, restored)" : "read (restored)",
                     host_frame((uint64_t)er->ExceptionAddress).c_str(), (unsigned long long)current_thread_id());
        }
        if (r) return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_ACCESS_VIOLATION && g_manque_memoire.load() && !g_crashing++) {
        // Windows a refusé la page que le jeu demandait (mem_fault) : arrêt net, avec le code que le launcher traduit
        // par « Mémoire insuffisante »
        HLOG("PANIC", "the game is stopping: the PC is out of memory (%.1f GB available commit)", memoire_validable_libre_go());
        fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 4);
    }
    if (g_crashing++) {
        Sleep(INFINITE);
    }
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        const char* kind = er->ExceptionInformation[0] == 0 ? "read"
                           : er->ExceptionInformation[0] == 1 ? "write" : "execute";
        uint64_t a = er->ExceptionInformation[1];
        MemInfo mi = mem_query(a);
        HLOG("crash", "access violation (%s) at 0x%llx [guest state 0x%x, perm %u] from %s (thread %llu)", kind,
             (unsigned long long)a, mi.state, mi.perm, host_frame((uint64_t)er->ExceptionAddress).c_str(),
             (unsigned long long)current_thread_id());
    } else {
        HLOG("crash", "exception 0x%08lx from %s", code, host_frame((uint64_t)er->ExceptionAddress).c_str());
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
        HLOG("dump", "stack of thread %llu:", (unsigned long long)id);
        print_backtrace(&c);
    }
    ResumeThread(th);
}


// --profile T : profil par échantillonnage (1 ms) de tous les threads invités pendant 10 s à
// partir de T secondes. Chaque échantillon est rangé dans la fonction invitée recompilée qui
// s'exécute, ou dans « runtime » / « système » (DLL Windows : attentes, synchronisation).
void run_profile(double start, double dur) {
    std::this_thread::sleep_for(std::chrono::duration<double>(start));
    HLOG("prof", "profile: %.0f s of sampling", dur);
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
        if (k == K_SYS) return "[system: waits, Windows DLLs]";
        if (k == K_RT) return "[runtime: dispatch, emulated kernel, HLE]";
        return symbolize(k);
    };
    std::map<uint64_t, uint64_t> glob;
    for (auto& [id, m] : per) {
        uint64_t n = 0, busy = 0;
        for (auto& [k, v] : m) { n += v; if (k != K_SYS) busy += v; glob[k] += v; }
        if (!n) continue;
        HLOG("prof", "thread %llu: %llu samples, busy %.0f %%", (unsigned long long)id,
             (unsigned long long)n, 100.0 * busy / n);
        std::vector<std::pair<uint64_t, uint64_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < v.size() && i < 6 && busy * 10 > n; i++)
            HLOG("prof", "    %5.1f %%  %s", 100.0 * v[i].second / n, name(v[i].first).c_str());
    }
    std::vector<std::pair<uint64_t, uint64_t>> v(glob.begin(), glob.end());
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
    HLOG("prof", "global (%llu samples):", (unsigned long long)total);
    for (size_t i = 0; i < v.size() && i < 30; i++)
        HLOG("prof", "  %5.1f %%  %s", 100.0 * v[i].second / total, name(v[i].first).c_str());
    std::vector<std::pair<uint64_t, uint64_t>> r(rt_rips.begin(), rt_rips.end());
    std::sort(r.begin(), r.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < r.size() && i < 25; i++)
        HLOG("prof", "  runtime image+0x%llx: %llu", (unsigned long long)r[i].first, (unsigned long long)r[i].second);
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
        HLOG("api", "t%llu %s (no. %llu) ← %s", (unsigned long long)tid, symbolize(target).c_str(),
             (unsigned long long)n + 1, symbolize(lr).c_str());
}

void fatal_hook(const char* msg, uint64_t pc) {
    panic("%s (guest pc %s)", msg, symbolize(pc).c_str());
}

// ---- rappels passés par rtld à nn::init::Start
void native_nop(CPUState*) {}

void native_init_modules(CPUState* ctx) {
    // rtld insère chaque module en tête de liste : init dans l'ordre sdk → … → main
    for (auto it = g_modules.rbegin(); it != g_modules.rend(); ++it) {
        if (!it->init) continue;
        HLOG("init", "%s: _init (%s)", it->name.c_str(), symbolize(it->init).c_str());
        call_guest(ctx, it->init);
    }
    HLOG("init", "module constructors finished");
}

void native_fini_modules(CPUState* ctx) {
    for (auto& m : g_modules) {
        if (!m.fini) continue;
        HLOG("init", "%s: _fini", m.name.c_str());
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
    vlog("PANIC", fmt, ap);
    va_end(ap);
    if (!g_crashing++) {
        print_guest_state();
        print_backtrace(nullptr);
    }
    // Windows avait refusé de la mémoire au jeu : c'est la cause (le jeu s'arrête de lui-même faute de mémoire).
    // Code 4 : le launcher demande au joueur de fermer d'autres programmes.
    if (g_manque_memoire)
        HLOG("PANIC", "cause: the PC is low on memory (%.1f GB available commit, about 8 are needed): close other "
                      "programs (other games, browser...) then restart", memoire_validable_libre_go());
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), g_manque_memoire ? 4 : 2);
    for (;;) {}
}

}  // namespace hos

using namespace hos;

// ---------------------------------------------------------------- suivi du tas C++ (diagnostic RECOMP_MEM_MAP=1)
// Le tas par défaut du processus sert à notre code (new, malloc) et au pilote graphique. Pour savoir lequel des deux
// grossit pendant un pic, operator new et operator delete tiennent le compte des octets vivants alloués par le runtime
// et des allocations par classe de taille (puissance de 2). Sans RECOMP_MEM_MAP, seul un test de booléen s'ajoute.
namespace {
std::atomic<int64_t> g_cpp_octets{0};
std::atomic<uint64_t> g_cpp_n[40], g_cpp_o[40];
bool cpp_suivi() {
    static const bool actif = getenv("RECOMP_MEM_MAP") != nullptr;
    return actif;
}
}  // namespace
void* operator new(std::size_t n) {
    for (;;) {
        if (void* q = malloc(n ? n : 1)) {
            if (cpp_suivi()) {
                const size_t t = _msize(q);
                g_cpp_octets += (int64_t)t;
                const int c = std::min(39, t ? 63 - __builtin_clzll(t) : 0);
                g_cpp_n[c]++;
                g_cpp_o[c] += t;
            }
            return q;
        }
        std::new_handler h = std::get_new_handler();
        if (!h) throw std::bad_alloc();
        h();
    }
}
void operator delete(void* q) noexcept {
    if (q && cpp_suivi()) g_cpp_octets -= (int64_t)_msize(q);
    free(q);
}
void operator delete(void* q, std::size_t) noexcept { operator delete(q); }
void* operator new[](std::size_t n) { return operator new(n); }
void operator delete[](void* q) noexcept { operator delete(q); }
void operator delete[](void* q, std::size_t) noexcept { operator delete(q); }
namespace hos {
// ---------------------------------------------------------------- piles d'appels de tous les fils (diagnostic)
// Qui alloue pendant un pic du tas ? L'appelant tient le verrou du tas (HeapLock) : les fils qui allouent s'y
// arrêtent, et leur pile montre le module qui a demandé la mémoire. Chaque fil est suspendu le temps de lire son
// contexte et de remonter sa pile (RtlVirtualUnwind), puis repris ; les noms sont cherchés après coup (export le plus
// proche du module : exact pour ntdll, approximatif ailleurs). Seuls les fils passés par une fonction du tas sont
// écrits, 12 au plus.
namespace {
std::string nom_adresse(uint64_t pc) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)pc, &m) || !m)
        return "?";
    char chemin[MAX_PATH] = "";
    GetModuleFileNameA(m, chemin, sizeof chemin);
    const char* nom = strrchr(chemin, '\\');
    nom = nom ? nom + 1 : chemin;
    const uint8_t* base = (const uint8_t*)m;
    const auto* dos = (const IMAGE_DOS_HEADER*)base;
    const auto* nt = (const IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    const char* meilleur = nullptr;
    uint32_t ecart = 0xffffffff;
    const uint32_t rva = (uint32_t)(pc - (uint64_t)base);
    if (dir.VirtualAddress && _stricmp(nom, "recomp_main.exe")) {   // (l'exécutable du jeu : décalage seul, pour addr2line)
        const auto* ex = (const IMAGE_EXPORT_DIRECTORY*)(base + dir.VirtualAddress);
        const uint32_t* noms = (const uint32_t*)(base + ex->AddressOfNames);
        const uint16_t* ords = (const uint16_t*)(base + ex->AddressOfNameOrdinals);
        const uint32_t* fn = (const uint32_t*)(base + ex->AddressOfFunctions);
        for (uint32_t i = 0; i < ex->NumberOfNames; i++) {
            const uint32_t f = fn[ords[i]];
            if (f <= rva && rva - f < ecart) ecart = rva - f, meilleur = (const char*)(base + noms[i]);
        }
    }
    char b[256];
    if (meilleur && ecart < 0x4000) snprintf(b, sizeof b, "%s!%s+0x%x", nom, meilleur, ecart);
    else snprintf(b, sizeof b, "%s+0x%x", nom, rva);
    return b;
}
}  // namespace

void dump_thread_stacks(const char* why) {
    struct Pile { DWORD tid; int n; uint64_t pc[72]; };
    static Pile piles[512];
    int np = 0;
    const DWORD moi = GetCurrentThreadId(), pid = GetCurrentProcessId();
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok && np < 512; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == moi) continue;
        const HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
        if (!th) continue;
        if (SuspendThread(th) != (DWORD)-1) {
            CONTEXT c{};
            c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            Pile& p = piles[np];
            p.tid = te.th32ThreadID;
            p.n = 0;
            if (GetThreadContext(th, &c)) {
                while (p.n < 72 && c.Rip) {
                    p.pc[p.n++] = c.Rip;
                    DWORD64 base = 0;
                    const PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                    if (!rf) {   // fonction feuille : l'adresse de retour est au sommet de la pile
                        if (IsBadReadPtr((const void*)c.Rsp, 8)) break;
                        c.Rip = *(const DWORD64*)c.Rsp;
                        c.Rsp += 8;
                    } else {
                        void* hd = nullptr;
                        DWORD64 ef = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, nullptr);
                    }
                }
                np++;
            }
            ResumeThread(th);
        }
        CloseHandle(th);
    }
    CloseHandle(snap);
    int ecrits = 0, dans_tas = 0;
    for (int i = 0; i < np; i++) {
        std::string ligne;
        bool tas = false;
        std::string avant;   // les cadres consécutifs d'un même module sont comptés, pas répétés
        int memes = 0;
        for (int k = 0; k < piles[i].n; k++) {
            const std::string nm = nom_adresse(piles[i].pc[k]);
            if (nm.find("Heap") != std::string::npos || nm.find("alloc") != std::string::npos) tas = true;
            const std::string mod = nm.substr(0, nm.find_first_of("!+"));
            const bool nomme = nm.find('!') != std::string::npos || mod == "recomp_main.exe";
            if (!nomme && mod == avant && k + 1 < piles[i].n) {
                memes++;
                continue;
            }
            if (memes) ligne += " (+" + std::to_string(memes) + ")";
            memes = 0;
            avant = mod;
            ligne += (k ? " < " : "") + nm;
        }
        if (!tas) continue;
        dans_tas++;
        if (ecrits++ < 12) HLOG("mem", "  thread %lu: %s", piles[i].tid, ligne.c_str());
    }
    HLOG("mem", "  stacks (%s): %d threads, %d inside a heap function", why, np, dans_tas);
}

// (octets vivants alloués par new ; pour chaque classe de taille 2^c : nombre et octets alloués depuis le lancement)
void cpp_heap_stats(int64_t* vivants, uint64_t nombre[40], uint64_t octets[40]) {
    *vivants = g_cpp_octets.load();
    for (int c = 0; c < 40; c++) nombre[c] = g_cpp_n[c].load(), octets[c] = g_cpp_o[c].load();
}
}  // namespace hos

// Première ligne du journal : version (recomp/VERSION, deux dossiers au-dessus de l'exécutable), Windows, processeur,
// mémoire. Le GPU et son pilote suivent à l'initialisation de Vulkan.
std::string lire_registre(HKEY racine, const char* cle, const char* valeur) {
    char b[256] = {};
    DWORD n = sizeof b - 1;
    if (RegGetValueA(racine, cle, valeur, RRF_RT_REG_SZ, nullptr, b, &n) != ERROR_SUCCESS) return "?";
    std::string r = b;   // le nom du processeur est suivi d'espaces sur certains PC
    while (!r.empty() && r.back() == ' ') r.pop_back();
    return r;
}
// Écran à mise à l'échelle Windows (125 %, 150 %…) : le jeu se déclare compatible DPI, écran par écran. Jusqu'à la
// 1.0.3, il ne l'était pas et Windows lui donnait des tailles en points : sur un écran 2560×1440 à 150 %, une fenêtre
// « plein écran » de 1707×960 où l'image 1080p était réduite, puis étirée par Windows à 2560×1440, donc floue (journal
// de l'issue GitHub n° 10, 2026-11-12). Les tailles sont maintenant en pixels réels ; resolution = auto et la taille de
// la fenêtre gardent le choix d'avant (present.cpp : gfx_screen_size, window_thread). À appeler avant toute fenêtre.
const char* g_dpi_mode = "not DPI aware";
void dpi_compatible() {
    if (SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) g_dpi_mode = "per-monitor DPI aware";
    else if (SetProcessDPIAware()) g_dpi_mode = "DPI aware for the main screen";   // Windows 10 avant 1703
}

void journal_systeme() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string d = exe;
    for (int i = 0; i < 3 && d.find_last_of("\\/") != std::string::npos; i++) d = d.substr(0, d.find_last_of("\\/"));
    std::string version = "?";
    if (FILE* f = fopen((d + "\\VERSION").c_str(), "rb")) {
        char b[32] = {};
        if (fgets(b, sizeof b, f)) version = b;
        fclose(f);
        while (!version.empty() && (version.back() == '\n' || version.back() == '\r' || version.back() == ' ')) version.pop_back();
    }
    const char* nt = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    std::string os = lire_registre(HKEY_LOCAL_MACHINE, nt, "ProductName");
    const std::string build = lire_registre(HKEY_LOCAL_MACHINE, nt, "CurrentBuild");
    if (atoi(build.c_str()) >= 22000 && os.find("Windows 10") != std::string::npos) os.replace(os.find("Windows 10"), 10, "Windows 11");
    MEMORYSTATUSEX ms{sizeof ms};
    GlobalMemoryStatusEx(&ms);
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    HLOG("main", "SpaceAnimalsRecomp %s; %s %s (build %s); %s, %lu threads, AVX2 %s; RAM %.1f GB (%.1f free)", version.c_str(),
         os.c_str(), lire_registre(HKEY_LOCAL_MACHINE, nt, "DisplayVersion").c_str(), build.c_str(),
         lire_registre(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString").c_str(),
         si.dwNumberOfProcessors, IsProcessorFeaturePresent(PF_AVX2_INSTRUCTIONS_AVAILABLE) ? "yes" : "NO",
         ms.ullTotalPhys / 1073741824.0, ms.ullAvailPhys / 1073741824.0);
}

// Arrêts sans exception Windows : allocation refusée (operator new), std::terminate, abort(). Jusqu'à la 1.0.2, le jeu
// se fermait alors d'un coup (code 0xC0000409) sans un mot dans le journal : vu le 2026-11-11 chez l'utilisateur, 18 s
// après un lancement avec 8,6 Go de mémoire validable libre. Le journal dit maintenant pourquoi, et le code de sortie 4
// fait afficher « Mémoire insuffisante » par le launcher quand la mémoire du PC est épuisée.
[[noreturn]] void arret_net(const char* quoi) {
    const double libre = memoire_validable_libre_go();
    const bool memoire = g_manque_memoire.load() || libre < 1.0;
    HLOG("PANIC", "%s; %.1f GB of available commit%s", quoi, libre,
         memoire ? ": the PC is out of memory (close other programs, other games, browser...)" : "");
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), memoire ? 4 : 3);
    for (;;) Sleep(INFINITE);
}
void plus_de_memoire() {
    g_manque_memoire = true;
    arret_net("memory allocation refused by Windows");
}
void arret_imprevu() { arret_net("unexpected program exit (abort or std::terminate)"); }

// Jeu construit par une autre version que celle installée. preparer.py note dans .preparation\version.ok, à côté de
// l'exécutable, la version des sources qui a construit le jeu en entier ; recomp\VERSION (deux dossiers plus haut) est
// celle des sources installées. Différentes : une mise à jour a été installée et le joueur n'a pas (encore) reconstruit
// le jeu. C'est son choix (demande de l'utilisateur du 2026-11-11 au soir) : le jeu démarre, et son journal le dit,
// pour qu'un rapport de bug montre qu'il tourne sans les améliorations de la version installée. Du 2026-11-11 à 15 h au
// soir, le jeu refusait de démarrer (code 5).
// Sans objet si l'un des deux fichiers manque (exécutable hors d'une installation, construction à la main) et pendant
// une construction (RECOMP_CONSTRUCTION).
bool version_du_jeu_a_jour() {
    if (getenv("RECOMP_CONSTRUCTION")) return true;
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dossier = exe;
    dossier = dossier.substr(0, dossier.find_last_of("\\/") + 1);
    auto ligne = [](const std::string& chemin, std::string* v) {
        FILE* f = fopen(chemin.c_str(), "rb");
        if (!f) return false;
        char b[64] = {};
        if (fgets(b, sizeof b, f)) *v = b;
        fclose(f);
        while (!v->empty() && (v->back() == '\n' || v->back() == '\r' || v->back() == ' ')) v->pop_back();
        return !v->empty();
    };
    std::string construit, installe;
    if (!ligne(dossier + ".preparation\\version.ok", &construit) || !ligne(dossier + "..\\..\\VERSION", &installe)) return true;
    if (construit == installe) return true;
    HLOG("main", "the game was built by version %s and version %s is installed: some improvements require rebuilding the "
                 "game (launcher, REBUILD NOW)", construit.c_str(), installe.c_str());
    return false;
}

int main(int argc, char** argv) {
    dpi_compatible();   // avant toute fenêtre (et avant SDL)
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
        if (!strcmp(argv[i], "--save-dir")) {   // dossier des sauvegardes, sans lancer le jeu (launcher, vérifications)
            printf("%s\n", save_dir_path().c_str());
            fflush(nullptr);
            return 0;
        }
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--precompile")) {   // shaders compilés pour ce GPU, sans le jeu (launcher : premier lancement)
            char exe[MAX_PATH];
            GetModuleFileNameA(nullptr, exe, MAX_PATH);
            std::string d = exe;
            // le travail se fait dans un processus enfant (precompile.log) ; le parent, qui le surveille et isole un
            // shader qui ferait planter le pilote, écrit precompile_controle.log
            if (logpath == "recomp_run.log")
                logpath = d.substr(0, d.find_last_of("\\/") + 1) +
                          (getenv("RECOMP_PRECOMPILE_ENFANT") ? "precompile.log" : "precompile_controle.log");
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
    journal_systeme();           // version, Windows, processeur, mémoire : en tête de chaque journal (rapport de bug)
    HLOG("main", "Windows display scaling of the main screen: %u %% (%s)", GetDpiForSystem() * 100 / 96, g_dpi_mode);
    settings_init(argc, argv);   // recomp.ini, RECOMP_*, --refresh / --resolution / --native-res / --config
    version_du_jeu_a_jour();   // note seulement dans le journal un jeu construit par une autre version
    mem_marge_init();            // marge de mémoire engagée d'avance (memory.cpp), rendue si Windows en refuse au jeu
    mem_release_init();          // recomp.ini, texture_release : données des textures rendues après l'envoi au GPU
    mem_veille_init();           // veille de la mémoire du PC : caches vidés avant le premier refus de Windows
    mem_census_init();           // RECOMP_MEM_CENSUS=s seulement (diagnostic)
    mem_vides_init();            // recomp.ini, zero_page_release : pages vides de la mémoire du jeu rendues à Windows
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
    std::set_new_handler(plus_de_memoire);   // allocation refusée dans le runtime : message clair, code 4
    std::set_terminate(arret_imprevu);
    signal(SIGABRT, [](int) { arret_imprevu(); });
    rt_fatal_hook = fatal_hook;
    g_dump_host_thread = dump_host_thread;
    kernel_init();
    {
        auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 10; i++) Sleep(1);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 10;
        HLOG("main", "Sleep(1) takes %.2f ms (timer resolution)", ms);
    }
    {   // Mémoire validable qu'il faut au jeu (relevés du 2026-11-11, cinématique d'ouverture) : ≈ 9,7 Go depuis que sa
        // mémoire est engagée à la demande (memory.cpp) et importée par fenêtres (vk_render.cpp) ; ≈ 15 Go avant.
        // Depuis le 2026-11-12 (pages vides rendues, pages oubliées rangées) : 7,4 Go au plus pendant 19 minutes de
        // partie de l'utilisateur ; seuil de 10 Go comme l'alerte du launcher (lanceur.cpp, jouer).
        // Prévenir dans le journal s'il en manque (erreur 1455, ERROR_COMMITMENT_LIMIT).
        const double libre = memoire_validable_libre_go();
        HLOG("main", "available commit: %.1f GB%s", libre,
             libre < 10.0 ? ": TOO LOW, the game may stop (close heavy programs, let Windows manage the page file size)" : "");
    }
    HLOG("main", "loading modules (%s)", manifest.c_str());
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
    hle_hid_init();
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
    HLOG("main", "%zu functions in the dispatch table", rt_dispatch_table().size());
    uint64_t start = find_export("_ZN2nn4init5StartEmmPFvvES2_S2_");
    if (!start || !rt_lookup(start)) panic("nn::init::Start not found");
    const Module* mainm = nullptr;
    for (auto& m : g_modules)
        if (m.name == "main") mainm = &m;
    HLOG("main", "starting: nn::init::Start @ %s", symbolize(start).c_str());
    // pile du thread principal : 1 Mo (main.npdm), priorité 44
    if (dump_after > 0)
        std::thread([dump_after] {
            std::this_thread::sleep_for(std::chrono::duration<double>(dump_after));
            kernel_dump_threads();
        }).detach();
    if (profile_at > 0) std::thread([profile_at] { run_profile(profile_at, 10); }).detach();
    run_main_thread(start, 0, mainm ? mainm->base : 0, f2, f3, f4, 0x100000);
    HLOG("main", "end");
    fflush(nullptr);
    return 0;
}
