// Espace d'adressage invité : réservation à adresse fixe (identité invité == hôte), carte des
// régions façon Horizon (état / permissions / attributs) pour svcQueryMemory & co.
#include <atomic>
#include "hos.h"
#include "settings.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace hos {
namespace {

struct Reg {
    uint64_t end;
    uint32_t state, perm, attr;
};
std::map<uint64_t, Reg> g_regs;  // début → région ; couvre exactement [AS_BASE, AS_END)
std::recursive_mutex g_mlock;

// coupe la région qui contient `at` pour qu'une région commence exactement en `at`
void split(uint64_t at) {
    if (at <= AS_BASE || at >= AS_END) return;
    auto it = g_regs.upper_bound(at);
    --it;
    if (it->first == at) return;
    Reg r = it->second;
    it->second.end = at;
    g_regs[at] = r;
}

void merge_around(uint64_t a, uint64_t b) {
    auto it = g_regs.lower_bound(a);
    if (it != g_regs.begin()) --it;
    while (it != g_regs.end() && it->first <= b) {
        auto nx = std::next(it);
        if (nx == g_regs.end()) break;
        const Reg &x = it->second, &y = nx->second;
        if (x.end == nx->first && x.state == y.state && x.perm == y.perm && x.attr == y.attr) {
            it->second.end = y.end;
            g_regs.erase(nx);
        } else {
            it = nx;
        }
    }
}

template <class F>
void for_range(uint64_t addr, uint64_t size, F f) {
    uint64_t end = addr + size;
    split(addr);
    split(end);
    for (auto it = g_regs.lower_bound(addr); it != g_regs.end() && it->first < end; ++it) f(it->second);
    merge_around(addr, end);
}

}  // namespace

void mem_init() {
    void* p = VirtualAlloc((void*)AS_BASE, AS_END - AS_BASE, MEM_RESERVE, PAGE_NOACCESS);
    if (p != (void*)AS_BASE) {
        DWORD err = GetLastError();
        // diagnostic : qui occupe déjà la zone ?
        for (uint64_t a = AS_BASE; a < AS_END;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
            if (mbi.State != MEM_FREE)
                HLOG("mem", "guest area already in use: 0x%llx (+0x%llx), type 0x%lx",
                     (unsigned long long)mbi.BaseAddress, (unsigned long long)mbi.RegionSize, mbi.Type);
            a = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
        }
        SetLastError(err);
    }
    if (p != (void*)AS_BASE)
        panic("cannot reserve the guest address space [0x%llx, 0x%llx) (error %lu)",
              (unsigned long long)AS_BASE, (unsigned long long)AS_END, GetLastError());
    g_regs.clear();
    g_regs[AS_BASE] = Reg{AS_END, MS_FREE, MP_NONE, 0};
}

bool mem_commit(uint64_t addr, uint64_t size) {
    if (!size) return true;
    void* p = VirtualAlloc((void*)addr, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        HLOG("mem", "commit failed 0x%llx+0x%llx (error %lu)", (unsigned long long)addr,
             (unsigned long long)size, GetLastError());
        return false;
    }
    return true;
}

// génération des mappages : invalide les caches de mem_valid à chaque désengagement
std::atomic<uint64_t> g_mem_gen{1};

// ---------------------------------------------------------------- engagement à la demande
// Le jeu réserve presque toute sa mémoire d'un coup (MapPhysicalMemory : 10,4 Go dans ses six premières secondes, dont
// un pool de 3 Go et deux de 1 Go) et n'en remplit qu'une partie. Engagée d'un bloc (jusqu'à la 1.0.2), elle comptait
// aussitôt contre la mémoire validable de Windows (RAM + fichier d'échange) : il fallait ≈ 13 Go libres pour lancer le
// jeu, et il s'arrêtait sur un PC de 16 Go ou sur un PC de 32 Go chargé. Une plage « à la demande » reste seulement
// réservée ; ses pages sont engagées par blocs de 2 Mo :
//   * au premier accès : l'exception d'accès est rattrapée par mem_fault (gestionnaire de host_main.cpp), qui engage
//     le bloc, et l'instruction reprend ;
//   * d'avance par mem_ensure, quand la plage part vers le noyau de Windows ou le pilote graphique (lecture de fichier
//     dans un tampon du jeu, import Vulkan) : eux échouent sur une page non engagée au lieu de lever une exception.
// memory_saver = false dans recomp.ini (ou RECOMP_MEMORY_SAVER=0, RECOMP_EAGER_COMMIT=1) : ancien comportement, tout est
// engagé à la réservation.
namespace {
// taille des blocs engagés au premier accès (puissance de 2, 64 Ko au moins) ; RECOMP_LAZY_BLOC_KO pour les essais
const uint64_t LAZY_BLOC = [] {
    uint64_t ko = getenv("RECOMP_LAZY_BLOC_KO") ? strtoull(getenv("RECOMP_LAZY_BLOC_KO"), nullptr, 10) : 2048;
    uint64_t b = 64ull << 10;
    while (b < (ko << 10) && b < (64ull << 20)) b <<= 1;
    return b;
}();
SRWLOCK g_lazy_lock = SRWLOCK_INIT;
std::map<uint64_t, uint64_t> g_lazy;   // début → fin des plages à la demande (disjointes)
std::atomic<uint64_t> g_lazy_blocs{0}, g_lazy_octets{0};

// Avec l'engagement à la demande, un PC à court de mémoire validable ne refuse plus le jeu au démarrage : il lui refuse
// une page en pleine partie (vu le 2026-11-11 : deux parties arrêtées net, dwm.exe tombé à la même seconde). Trois
// protections : une marge engagée d'avance (256 Mo, sans RAM tant qu'elle n'est pas touchée) rendue à Windows au
// premier refus ; le rendu prévenu, qui vide ses caches d'images et rend les fenêtres d'import inutilisées
// (mem_pression, vk_render.cpp) ; de nouveaux essais pendant 5 s, le temps que cette mémoire revienne ou que Windows
// agrandisse le fichier d'échange. Après quoi seulement le jeu s'arrête, avec un message clair (code 4).
constexpr size_t MARGE = 256u << 20;
std::atomic<void*> g_marge{nullptr};
std::atomic<int> g_pression{0};

bool engager(uint64_t p, uint64_t n) {
    if (VirtualAlloc((void*)p, n, MEM_COMMIT, PAGE_READWRITE)) return true;
    const DWORD err = GetLastError();
    if (err != ERROR_COMMITMENT_LIMIT && err != ERROR_NOT_ENOUGH_MEMORY && err != ERROR_OUTOFMEMORY &&
        err != ERROR_PAGEFILE_QUOTA && err != ERROR_WORKING_SET_QUOTA)
        return false;
    static std::atomic<int> dits{0};
    const bool dire = dits++ < 3;
    if (dire)
        HLOG("mem", "Windows refuses %llu KB of game memory (error %lu, %.1f GB available commit): releasing the reserve "
                    "and caches, retrying for up to 5 s", (unsigned long long)(n >> 10), err, memoire_validable_libre_go());
    if (void* m = g_marge.exchange(nullptr)) VirtualFree(m, 0, MEM_RELEASE);
    for (int essai = 0; essai < 50; essai++) {
        g_pression++;
        if (VirtualAlloc((void*)p, n, MEM_COMMIT, PAGE_READWRITE)) {
            if (dire)
                HLOG("mem", "game memory obtained after %d ms (%.1f GB available commit): the PC is very low on memory, "
                            "close other programs", essai * 100, memoire_validable_libre_go());
            return true;
        }
        Sleep(100);
    }
    return false;
}

// plage à la demande qui contient `a` : (début, fin), ou (0, 0). Verrou tenu par l'appelant.
std::pair<uint64_t, uint64_t> lazy_plage(uint64_t a) {
    auto it = g_lazy.upper_bound(a);
    if (it == g_lazy.begin()) return {0, 0};
    --it;
    return a < it->second ? std::make_pair(it->first, it->second) : std::make_pair<uint64_t, uint64_t>(0, 0);
}

void lazy_retirer(uint64_t addr, uint64_t size) {   // verrou exclusif tenu par l'appelant
    const uint64_t end = addr + size;
    auto it = g_lazy.upper_bound(addr);
    if (it != g_lazy.begin()) --it;
    while (it != g_lazy.end() && it->first < end) {
        const uint64_t s = it->first, e = it->second;
        if (e <= addr) {
            ++it;
            continue;
        }
        it = g_lazy.erase(it);
        if (s < addr) g_lazy[s] = addr;
        if (e > end) it = g_lazy.emplace(end, e).first, ++it;
    }
}

// ---------------------------------------------------------------- pages rendues (recomp.ini : texture_release)
// Sur la console, les données d'une texture servent directement au GPU. Ici elles sont copiées en mémoire vidéo et
// l'original reste dans la mémoire du jeu : 1,9 Go à 96 s de jeu (mesuré le 2026-11-11). mem_release écrit les pages
// entières de cette source dans un fichier temporaire (dossier temporaire de Windows, supprimé à la fermeture), puis
// les désengage : elles ne comptent plus dans la mémoire engagée ni dans la RAM du jeu. À la première lecture (par le
// jeu ou par le runtime), mem_fault relit le morceau du fichier et l'engage de nouveau avant de reprendre
// l'instruction : le jeu relit exactement ce qu'il avait laissé. mem_ensure (avant un appel système ou un
// import Vulkan) et mem_restore (avant une lecture en nombre) font de même pour une plage entière. Les pages sous un
// import ne sont jamais rendues : le pilote les verrouille (vk_render.cpp, tex_release).
// Lecture ou écriture du jeu dans des données rendues : le contenu revient du fichier avant que l'instruction
// reprenne, par morceaux de 64 Ko, page par page (chaque page engagée puis remplie aussitôt ; un autre fil qui touche
// une page encore désengagée faute et attend le verrou). Le jeu retrouve exactement ce qu'il avait laissé, qu'il
// réécrive tout ou une partie seulement.
// Du 2026-11-11 à 14 h à 17 h, une écriture dans les données d'une texture sans objet (Rendu::vivant faux) faisait
// oublier la plage entière sans rien relire, en supposant que le jeu réemployait cette mémoire. C'était faux : ce jeu
// détruit et recrée sans cesse ses objets texture sur des données restées en place (9 759 recréations en 18 minutes
// de jeu) ; une écriture partielle dans une telle plage laissait le reste à zéro, et la texture recréée était noire
// (grands rectangles noirs à la place de la fumée des explosions, vus par l'utilisateur). RECOMP_TEX_DROP=1 remet cet
// ancien comportement, pour comparer. Compteur 5 : données relues (par le jeu ou par le runtime) alors qu'une autre
// partie de la même plage sans objet avait été réécrite, c'est-à-dire ce que l'oubli aurait rendu à zéro.
// Avant le 2026-11-11 à 14 h, le morceau de 64 Ko était engagé d'un bloc puis copié : un autre fil pouvait écrire
// dedans entre les deux, et la copie écrasait ses nouvelles données par les anciennes.
// Limite connue : entre l'engagement d'une page et sa copie (moins d'une microseconde), un autre fil qui y accède
// sans fauter lit des zéros ou voit son écriture recouverte. La fermer demande des vues de fichier (espace
// d'adressage en « placeholders »).
// Verrous : g_rel_lock, puis g_lazy_lock ; jamais l'inverse.
struct Rendu {
    uint64_t fin, off, id;
    bool vivant;   // l'image qui a rendu ces pages a encore un objet texture du jeu
    bool entame = false;   // sans objet, une partie de ce rendu a été réécrite par le jeu (compteur 5)
    bool froid = false;    // pages pleines oubliées (vides_balayer, cold_page_release), pas des données de texture
};
SRWLOCK g_rel_lock = SRWLOCK_INIT;
std::map<uint64_t, Rendu> g_rel;                // début → plage rendue (pages entières, disjointes)
std::map<uint64_t, uint64_t> g_rel_libres;      // fichier : début → longueur des zones libres
uint64_t g_rel_taille = 0, g_rel_id = 0;        // taille du fichier ; numéro du dernier rendu
HANDLE g_rel_fichier = INVALID_HANDLE_VALUE;
constexpr uint64_t REL_TAMPON = 4ull << 20;   // 1 Mo jusqu'au 2026-11-13 : un arrêt des fils par tampon
constexpr uint64_t REL_AVANCE = 1ull << 20;   // ramené d'un coup par une lecture fautive (64 Ko avant)
uint8_t* g_rel_tampon = nullptr;                // lectures du fichier, sous le verrou
uint64_t g_rel_morceau = 64ull << 10;           // restauration à l'accès, par morceaux (RECOMP_TEX_MORCEAU_KO)
constexpr uint64_t REL_MAX = 6ull << 30;        // taille maximale du fichier
constexpr uint64_t REL_DISQUE_MIN = 4ull << 30; // place laissée libre sur le disque du dossier temporaire
uint64_t g_rel_max = REL_MAX;                   // taille maximale retenue au démarrage, d'après la place libre
wchar_t g_rel_dossier[MAX_PATH + 1] = L"";      // dossier du fichier (contrôle de la place libre)
uint64_t g_rel_ecrits = 0;                      // octets écrits depuis le dernier contrôle de la place libre
std::atomic<bool> g_rel_present{false}, g_rel_erreur{false}, g_rel_plein{false};
bool g_rel_abandon = false;                     // RECOMP_TEX_DROP : ancien comportement (plage oubliée), pour comparer
std::atomic<uint64_t> g_rel_octets{0}, g_rel_cumul{0}, g_rel_n[6], g_rel_o[6], g_rel_ns_ecrit{0}, g_rel_ns_lu{0};
// pages pleines oubliées rangées dans le même fichier (vides_balayer) : rangées maintenant, en tout, relues
std::atomic<uint64_t> g_froid_octets{0}, g_froid_cumul{0}, g_froid_relus{0};
std::atomic<bool> g_froid_actif{false};
uint32_t g_froid_tours = 8;   // balayages sans accès avant le rangement (RECOMP_COLD_SWEEPS, essais)

uint64_t maintenant_ns() {
    static const double par_tic = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e9 / (double)f.QuadPart;
    }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((double)t.QuadPart * par_tic);
}

bool rel_io(bool ecrire, uint64_t off, void* p, uint64_t n) {
    while (n) {
        OVERLAPPED ov{};
        ov.Offset = (DWORD)off, ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD fait = 0;
        const DWORD part = (DWORD)std::min<uint64_t>(n, 64ull << 20);
        const BOOL ok = ecrire ? WriteFile(g_rel_fichier, p, part, &fait, &ov) : ReadFile(g_rel_fichier, p, part, &fait, &ov);
        if (!ok || !fait) return false;
        off += fait, p = (uint8_t*)p + fait, n -= fait;
    }
    return true;
}

// zone du fichier de n octets : premier trou qui convient, sinon à la fin ; UINT64_MAX si le fichier est plein
uint64_t rel_place(uint64_t n) {
    for (auto it = g_rel_libres.begin(); it != g_rel_libres.end(); ++it)
        if (it->second >= n) {
            const uint64_t off = it->first, reste = it->second - n;
            g_rel_libres.erase(it);
            if (reste) g_rel_libres[off + n] = reste;
            return off;
        }
    if (g_rel_taille + n > g_rel_max) return UINT64_MAX;
    const uint64_t off = g_rel_taille;
    g_rel_taille += n;
    return off;
}

void rel_liberer(uint64_t off, uint64_t n) {   // zone du fichier rendue, fusionnée avec ses voisines
    auto nx = g_rel_libres.lower_bound(off);
    if (nx != g_rel_libres.begin()) {
        auto pv = std::prev(nx);
        if (pv->first + pv->second == off) {
            off = pv->first, n += pv->second;
            g_rel_libres.erase(pv);
        }
    }
    if (nx != g_rel_libres.end() && off + n == nx->first) {
        n += nx->second;
        g_rel_libres.erase(nx);
    }
    g_rel_libres[off] = n;
}

// retire [a, b) des plages rendues (verrou exclusif tenu) ; leurs zones du fichier redeviennent libres
void rel_retirer(uint64_t a, uint64_t b) {
    auto it = g_rel.upper_bound(a);
    if (it != g_rel.begin()) --it;
    while (it != g_rel.end() && it->first < b) {
        const uint64_t s = it->first, e = it->second.fin, off = it->second.off, id = it->second.id;
        const bool vivant = it->second.vivant, entame = it->second.entame, froid = it->second.froid;
        if (e <= a) {
            ++it;
            continue;
        }
        const uint64_t x0 = std::max(s, a), x1 = std::min(e, b);
        it = g_rel.erase(it);
        if (s < x0) g_rel[s] = Rendu{x0, off, id, vivant, entame, froid};
        if (x1 < e) it = g_rel.emplace(x1, Rendu{e, off + (x1 - s), id, vivant, entame, froid}).first, ++it;
        rel_liberer(off + (x0 - s), x1 - x0);
        (froid ? g_froid_octets : g_rel_octets) -= x1 - x0;
    }
}

// ---------------------------------------------------------------- restauration atomique (issue GitHub n° 17)
// Jusqu'à la 1.0.3, une page relue du fichier était engagée puis remplie : pendant le remplissage (moins d'une
// microseconde), un autre fil qui y accédait sans fauter lisait des zéros, ou voyait son écriture recouverte par la
// copie. Jugé improbable, puis vu chez un joueur : pointeur nul lu par le jeu pendant le combat contre Andross, après
// une heure de partie. Windows ne sait pas faire apparaître d'un coup une page privée déjà remplie : les autres fils du
// processus sont donc suspendus le temps d'engager et de copier le morceau, puis repris. Pendant l'arrêt, ce fil ne
// prend aucun verrou, n'écrit rien dans le journal et n'alloue rien sur le tas (un fil suspendu peut tenir ces
// verrous) : seulement des appels du noyau et memcpy. Un seul arrêt à la fois : rel_restaurer tient g_rel_lock en
// exclusif. RECOMP_NO_ATOMIC_RESTORE=1 (essais) : restauration d'avant.
using FilSuivant = LONG(__stdcall*)(HANDLE, HANDLE, ACCESS_MASK, ULONG, ULONG, PHANDLE);   // NtGetNextThread (ntdll)
FilSuivant g_fil_suivant = nullptr;
bool g_arret_actif = false;
std::atomic<uint64_t> g_arrets{0}, g_arrets_ns{0}, g_arret_max_ns{0}, g_arret_fils{0};

void arret_init() {
    if (getenv("RECOMP_NO_ATOMIC_RESTORE")) {
        HLOG("mem", "atomic page restore: off (RECOMP_NO_ATOMIC_RESTORE)");
        return;
    }
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll"))
        g_fil_suivant = reinterpret_cast<FilSuivant>(reinterpret_cast<void*>(GetProcAddress(nt, "NtGetNextThread")));
    g_arret_actif = g_fil_suivant != nullptr;
    if (!g_arret_actif) HLOG("mem", "atomic page restore unavailable (NtGetNextThread missing): pages restored as before");
}

// Tous les autres fils du processus suspendus, de la construction à la destruction. SuspendThread ne fait que demander
// l'arrêt : GetThreadContext ne rend la main qu'une fois le fil vraiment arrêté. Un fil créé pendant l'énumération par
// un fil pas encore suspendu est pris par la passe suivante.
struct MondeArrete {
    static constexpr int MAX = 1024;
    static inline HANDLE fils[MAX];
    static inline DWORD ids[MAX];
    int n = 0;
    uint64_t t0 = 0;
    MondeArrete() {
        if (!g_arret_actif) return;
        t0 = maintenant_ns();
        const DWORD moi = GetCurrentThreadId();
        const HANDLE proc = GetCurrentProcess();
        constexpr ACCESS_MASK acces = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION;
        for (int passe = 0; passe < 4; passe++) {
            int nouveaux = 0;
            HANDLE h = nullptr;
            bool garde = true;   // curseur gardé dans fils[] (ou nul) : pas à fermer ici
            for (;;) {
                HANDLE suiv = nullptr;
                const LONG etat = g_fil_suivant(proc, h, acces, 0, 0, &suiv);
                if (h && !garde) CloseHandle(h);
                if (etat < 0) break;
                h = suiv, garde = false;
                const DWORD id = GetThreadId(h);
                if (!id || id == moi || n >= MAX) continue;
                bool connu = false;
                for (int i = 0; i < n; i++) connu |= ids[i] == id;
                if (connu || SuspendThread(h) == (DWORD)-1) continue;   // déjà arrêté, ou fil en train de finir
                CONTEXT c;
                c.ContextFlags = CONTEXT_CONTROL;
                GetThreadContext(h, &c);
                fils[n] = h, ids[n] = id, n++, garde = true, nouveaux++;
            }
            if (!nouveaux) break;
        }
    }
    ~MondeArrete() {
        for (int i = 0; i < n; i++) {
            ResumeThread(fils[i]);
            CloseHandle(fils[i]);
        }
        if (!t0) return;
        const uint64_t d = maintenant_ns() - t0;
        g_arrets++;
        g_arrets_ns += d;
        if (d > g_arret_max_ns.load(std::memory_order_relaxed)) g_arret_max_ns.store(d, std::memory_order_relaxed);
        g_arret_fils.store((uint64_t)n, std::memory_order_relaxed);
    }
    MondeArrete(const MondeArrete&) = delete;
    MondeArrete& operator=(const MondeArrete&) = delete;
};

// Engage [p, p+n) et y copie src sans qu'un autre fil puisse voir la plage entre les deux (verrou exclusif tenu).
// Mémoire refusée par Windows : la place est d'abord obtenue ailleurs par engager (qui rend la réserve, attend et
// l'écrit dans le journal, les autres fils en marche), puis échangée contre la plage pendant l'arrêt.
bool engager_copier(uint64_t p, uint64_t n, const uint8_t* src) {
    {
        MondeArrete arret;
        if (VirtualAlloc((void*)p, n, MEM_COMMIT, PAGE_READWRITE)) {
            memcpy((void*)p, src, n);
            return true;
        }
    }
    void* ailleurs = VirtualAlloc(nullptr, n, MEM_RESERVE, PAGE_READWRITE);
    if (!ailleurs) return false;
    if (!engager((uint64_t)ailleurs, n)) {
        VirtualFree(ailleurs, 0, MEM_RELEASE);
        return false;
    }
    MondeArrete arret;
    VirtualFree(ailleurs, 0, MEM_RELEASE);
    if (!VirtualAlloc((void*)p, n, MEM_COMMIT, PAGE_READWRITE)) return false;
    memcpy((void*)p, src, n);
    return true;
}

// Copie src dans une page qui vient d'être engagée, donc à zéro, mot par mot et seulement là où elle est encore à
// zéro : ce qu'un autre fil y a écrit depuis l'engagement n'est jamais recouvert. Pour l'écriture fautive dans
// d'anciennes données de texture (cause 4) : le jeu réutilise cette mémoire, son ancien contenu ne vaut que comme fond
// d'une réécriture partielle, et un arrêt des fils à chacune de ces fautes (plus de cent par seconde chez un joueur)
// coûterait trop cher. Avant, memcpy pouvait recouvrir par ces vieilles données l'écriture d'un autre fil dans la même
// page.
void copier_sans_recouvrir(uint64_t p, const uint8_t* src, uint64_t n) {
    auto* d = (volatile LONG64*)p;
    for (uint64_t i = 0; i < n / 8; i++) {
        LONG64 v;
        memcpy(&v, src + i * 8, 8);
        if (v) InterlockedCompareExchange64(&d[i], v, 0);
    }
}

// Restaure la partie rendue de [a, b) (verrou exclusif tenu) : lue dans un tampon à part, par morceaux d'au plus
// 1 Mo, puis remise en place. Pages oubliées et données de texture relues (causes 0 à 3) : engagées et copiées les
// autres fils arrêtés (engager_copier) ; un fil qui touche une page encore désengagée faute et attend le verrou, aucun
// ne peut la voir engagée et pas encore remplie. Écriture fautive dans d'anciennes données de texture (cause 4) : page
// par page, sans arrêt (copier_sans_recouvrir). Si la lecture du fichier échoue, ou avec RECOMP_NO_ATOMIC_RESTORE :
// chemin d'avant (par_page, accès fautif du jeu : page par page ; sinon d'un bloc).
// Faux : Windows refuse la mémoire. cause : 0 lecture fautive, 1 écriture fautive dans une texture en service,
// 2 runtime, 3 avant un appel système, 4 écriture fautive dans une texture sans objet.
bool rel_restaurer(uint64_t a, uint64_t b, int cause, bool par_page = false) {
    for (;;) {
        auto it = g_rel.upper_bound(a);
        if (it != g_rel.begin() && std::prev(it)->second.fin > a) --it;
        if (it == g_rel.end() || it->first >= b) return true;
        const uint64_t p = std::max(it->first, a), n = std::min({it->second.fin, b, p + REL_TAMPON}) - p;
        const uint64_t off = it->second.off + (p - it->first);
        const uint64_t t0 = maintenant_ns();
        const bool lu = rel_io(false, off, g_rel_tampon, n);
        if (!lu && !g_rel_erreur.exchange(true))
            HLOG("mem", "%s not read back (error %lu): 0x%llx +0x%llx",
                 it->second.froid ? "forgotten game pages" : "released texture data", GetLastError(),
                 (unsigned long long)p, (unsigned long long)n);
        if (lu && g_arret_actif && (it->second.froid || cause != 4)) {
            if (!engager_copier(p, n, g_rel_tampon)) return false;
        } else if (par_page) {
            for (uint64_t q = 0; q < n; q += 0x1000) {
                if (!engager(p + q, 0x1000)) {
                    if (q) rel_retirer(p, p + q);
                    return false;
                }
                if (lu && g_arret_actif) copier_sans_recouvrir(p + q, g_rel_tampon + q, 0x1000);
                else if (lu) memcpy((void*)(p + q), g_rel_tampon + q, 0x1000);
            }
        } else {
            if (!engager(p, n)) return false;
            if (lu) memcpy((void*)p, g_rel_tampon, n);
        }
        g_rel_ns_lu += maintenant_ns() - t0;
        if (it->second.froid) {   // page oubliée relue : comptée à part des textures
            g_froid_relus += n;
        } else {
            g_rel_n[cause]++;
            g_rel_o[cause] += n;
            if (it->second.entame && cause != 4) {   // relu après une réécriture partielle : l'oubli l'aurait rendu à zéro
                g_rel_n[5]++;
                g_rel_o[5] += n;
            }
        }
        rel_retirer(p, p + n);
    }
}

// ---------------------------------------------------------------- pages vides rendues à Windows (zero_page_release)
// Mesuré le 2026-11-12 (recensement, RECOMP_MEM_CENSUS) : sur 2,9 à 3,9 Go de mémoire du jeu engagée à la demande,
// 1,3 Go ne contiennent que des zéros, du début à la fin d'une mission ; surtout des réserves de 80 à 100 Mo prises au
// démarrage (MapPhysicalMemory) et vides à 85-99 %. Rendues à Windows, elles se reliraient à zéro : le jeu ne verrait
// aucune différence. À 10 s, 25 s, puis toutes les 30 s, vides_balayer examine les morceaux de 64 Ko engagés des
// plages à la demande :
//   * candidats : en lecture-écriture, engagés depuis plus d'une période (g_age : un bloc tout juste engagé est souvent
//     en train d'être rempli), hors des fenêtres importées dans Vulkan (le GPU y lit et écrit sans fauter) ;
//   * lecture par ReadProcessMemory, qui ne déclenche pas l'engagement à la demande ; un morceau déjà trouvé plein n'est
//     relu qu'un balayage sur quatre, et seulement s'il est en RAM (rien n'est ramené du fichier d'échange) ;
//   * rendu par suites de 2 Mo au plus, sous le verrou de mem_fault (g_rel_lock) et la garde du noyau prise en
//     exclusif (sinon la suite attend le balayage suivant) : protection en lecture seule, nouvelle vérification (une
//     écriture faite avant la protection se voit ; une écriture après faute et attend le verrou), désengagement. Le fil
//     qui avait fauté trouve ensuite la page désengagée, mem_fault en engage le morceau, l'écriture reprend sur des
//     zéros : le contenu qu'elle avait avant.
// Au retour du jeu, mem_fault n'engage que le morceau de 64 Ko touché (g_vides) : le bloc de 2 Mo réengagerait toutes
// les pages rendues autour. ReadFile (svc_fs.cpp) et les imports Vulkan (vk_render.cpp) tiennent la garde du noyau en
// partage de l'engagement de leur plage à la fin de l'appel : Windows ou le pilote écrivent sans passer par une faute.
constexpr uint64_t VIDE_MORCEAU = 64ull << 10;
SRWLOCK g_noyau_lock = SRWLOCK_INIT;
std::mutex g_epingle_mx;
std::map<std::pair<uint64_t, uint64_t>, int> g_epingles;   // fenêtres importées : (début, fin) → nombre d'imports
std::map<uint64_t, uint64_t> g_vides;   // début → fin des plages rendues (pages entières, disjointes), sous g_rel_lock
std::atomic<bool> g_vides_present{false};
std::atomic<uint64_t> g_vides_octets{0}, g_vides_cumul{0}, g_vides_reengage{0};
// tour de balayage du dernier engagement de chaque bloc de 2 Mo de la zone du jeu
constexpr uint64_t AGE_BASE = 0x7000000000ull, AGE_FIN = 0x8000000000ull;
std::atomic<uint32_t> g_age[(AGE_FIN - AGE_BASE) >> 21];
std::atomic<uint32_t> g_tour{1};

void age_noter(uint64_t a, uint64_t b) {
    if (b <= AGE_BASE || a >= AGE_FIN || b <= a) return;
    a = std::max(a, AGE_BASE), b = std::min(b, AGE_FIN);
    const uint32_t t = g_tour.load(std::memory_order_relaxed);
    for (uint64_t i = (a - AGE_BASE) >> 21; i <= (b - 1 - AGE_BASE) >> 21; i++) g_age[i].store(t, std::memory_order_relaxed);
}

void vides_ajouter(uint64_t a, uint64_t b) {   // [a, b) rendue (verrou exclusif tenu) ; fusion avec ses voisines
    auto it = g_vides.upper_bound(a);
    if (it != g_vides.begin() && std::prev(it)->second >= a) {
        --it;
        a = it->first, b = std::max(b, it->second);
        it = g_vides.erase(it);
    }
    while (it != g_vides.end() && it->first <= b) {
        b = std::max(b, it->second);
        it = g_vides.erase(it);
    }
    g_vides[a] = b;
}

// retire [a, b) des plages rendues (verrou exclusif tenu) : pages engagées de nouveau, ou plus du tout au jeu ; rend
// le nombre d'octets retirés
uint64_t vides_retirer(uint64_t a, uint64_t b) {
    uint64_t n = 0;
    auto it = g_vides.upper_bound(a);
    if (it != g_vides.begin()) --it;
    while (it != g_vides.end() && it->first < b) {
        const uint64_t s = it->first, e = it->second;
        if (e <= a) {
            ++it;
            continue;
        }
        it = g_vides.erase(it);
        const uint64_t x0 = std::max(s, a), x1 = std::min(e, b);
        n += x1 - x0;
        if (s < x0) g_vides[s] = x0;
        if (x1 < e) it = g_vides.emplace(x1, e).first, ++it;
    }
    g_vides_octets -= std::min<uint64_t>(n, g_vides_octets.load());
    return n;
}
}  // namespace

void mem_release_init() {
    if (!settings().texture_release || g_rel_fichier != INVALID_HANDLE_VALUE) return;   // (faux aussi sans memory_saver)
    wchar_t chemin[MAX_PATH + 64];
    if (const wchar_t* e = _wgetenv(L"RECOMP_TEX_DIR")) {   // essais : autre dossier
        _snwprintf(g_rel_dossier, MAX_PATH, L"%ls\\", e);
        g_rel_dossier[MAX_PATH] = 0;
    } else if (!GetTempPathW(MAX_PATH, g_rel_dossier)) {
        HLOG("mem", "texture data released after upload: off, no temporary folder (error %lu)", GetLastError());
        return;
    }
    // restes d'une partie arrêtée par une coupure de courant (sinon Windows supprime le fichier dès que le jeu se
    // ferme, même tué) ; le fichier d'un autre jeu ouvert ne se laisse pas supprimer
    {
        wchar_t motif[MAX_PATH + 64];
        _snwprintf(motif, MAX_PATH + 63, L"%lsrecomp_textures_*.tmp", g_rel_dossier);
        motif[MAX_PATH + 63] = 0;
        WIN32_FIND_DATAW fd;
        const HANDLE h = FindFirstFileW(motif, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                wchar_t p[2 * MAX_PATH + 8];
                _snwprintf(p, 2 * MAX_PATH + 7, L"%ls%ls", g_rel_dossier, fd.cFileName);
                p[2 * MAX_PATH + 7] = 0;
                DeleteFileW(p);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    // jamais moins de 4 Go libres sur ce disque : le fichier prend ce qui reste au-delà, 6 Go au plus
    ULARGE_INTEGER libre{};
    if (!GetDiskFreeSpaceExW(g_rel_dossier, &libre, nullptr, nullptr) || libre.QuadPart < REL_DISQUE_MIN + (1ull << 30)) {
        HLOG("mem", "texture data released after upload: off, less than %.0f GB free in the temporary folder (%.1f GB)",
             (REL_DISQUE_MIN + (1ull << 30)) / 1073741824.0, libre.QuadPart / 1073741824.0);
        return;
    }
    g_rel_max = std::min<uint64_t>(REL_MAX, libre.QuadPart - REL_DISQUE_MIN);
    g_rel_tampon = (uint8_t*)VirtualAlloc(nullptr, REL_TAMPON, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    arret_init();
    if (!g_rel_tampon) return;
    _snwprintf(chemin, MAX_PATH + 63, L"%lsrecomp_textures_%lu.tmp", g_rel_dossier, (unsigned long)GetCurrentProcessId());
    chemin[MAX_PATH + 63] = 0;
    g_rel_fichier = CreateFileW(chemin, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (g_rel_fichier == INVALID_HANDLE_VALUE) {
        HLOG("mem", "texture data released after upload: off, temporary file not created (error %lu)", GetLastError());
        return;
    }
    g_rel_abandon = getenv("RECOMP_TEX_DROP") != nullptr;   // essais : ancien comportement
    if (g_rel_abandon) HLOG("mem", "texture data released after upload: RECOMP_TEX_DROP, data of unused textures dropped when rewritten");
    if (const char* m = getenv("RECOMP_TEX_MORCEAU_KO")) {   // essais
        const uint64_t ko = strtoull(m, nullptr, 10);
        g_rel_morceau = 4096;
        while (g_rel_morceau < (ko << 10) && g_rel_morceau < REL_TAMPON) g_rel_morceau <<= 1;
    }
    HLOG("mem", "texture data released after upload: on (temporary file recomp_textures_%lu.tmp in the Windows temp "
                "folder, up to %.1f GB, %.0f GB free there; restored by pieces of %llu KB)",
         (unsigned long)GetCurrentProcessId(), g_rel_max / 1073741824.0, libre.QuadPart / 1073741824.0,
         (unsigned long long)(g_rel_morceau >> 10));
}

bool mem_release_actif() {
    return g_rel_fichier != INVALID_HANDLE_VALUE && !g_rel_erreur.load(std::memory_order_relaxed) &&
           !g_rel_plein.load(std::memory_order_relaxed);
}

// Rend à Windows les pages entières de [addr, addr + size), que l'appelant vient de lire en entier. Tout ou rien :
// rend le nombre d'octets rendus (0 : rien n'a changé) et le numéro du rendu dans *id.
uint64_t mem_release(uint64_t addr, uint64_t size, uint64_t* id) {
    *id = 0;
    if (!mem_release_actif()) return 0;
    const uint64_t d = (addr + 0xfff) & ~0xfffull, f = (addr + size) & ~0xfffull;
    if (f <= d) return 0;
    AcquireSRWLockShared(&g_lazy_lock);
    const auto [ls, le] = lazy_plage(d);
    ReleaseSRWLockShared(&g_lazy_lock);
    if (!le || f > le) return 0;   // seulement dans une plage engagée à la demande : mem_fault saura la réengager
    AcquireSRWLockExclusive(&g_rel_lock);
    g_rel_present.store(true, std::memory_order_release);
    uint64_t fait = 0;
    const uint64_t t0 = maintenant_ns();
    // place libre du disque, relue tous les 256 Mo écrits : sous 2 Go, plus rien n'est rendu (ce qui l'est déjà reste
    // lisible)
    if (g_rel_ecrits >= (256ull << 20)) {
        g_rel_ecrits = 0;
        ULARGE_INTEGER libre{};
        if (GetDiskFreeSpaceExW(g_rel_dossier, &libre, nullptr, nullptr) && libre.QuadPart < (2ull << 30) &&
            !g_rel_plein.exchange(true))
            HLOG("mem", "texture data released after upload: stopped, %.1f GB free in the temporary folder",
                 libre.QuadPart / 1073741824.0);
    }
    const uint64_t off = g_rel_plein.load() ? UINT64_MAX : rel_place(f - d);
    if (off != UINT64_MAX && !rel_restaurer(d, f, 2)) rel_liberer(off, f - d);
    else if (off != UINT64_MAX) {
        DWORD avant = 0;
        // En lecture seule le temps de la copie : un fil qui y écrit à cet instant faute, attend le verrou dans
        // mem_fault, puis retrouve sa page restaurée ; son écriture n'est pas perdue.
        bool ok = VirtualProtect((void*)d, f - d, PAGE_READONLY, &avant) != 0;
        if (ok) {
            ok = rel_io(true, off, (void*)d, f - d);
            if (!ok && !g_rel_erreur.exchange(true))
                HLOG("mem", "texture data released after upload: stopped, temporary file not written (error %lu)",
                     GetLastError());
            if (ok) ok = VirtualFree((void*)d, f - d, MEM_DECOMMIT) != 0;
            if (!ok) VirtualProtect((void*)d, f - d, PAGE_READWRITE, &avant);
        }
        if (ok) {
            g_rel[d] = Rendu{f, off, ++g_rel_id, true};
            g_rel_octets += f - d;
            g_rel_cumul += f - d;
            g_rel_ecrits += f - d;
            *id = g_rel_id;
            fait = f - d;
        } else {
            rel_liberer(off, f - d);
        }
    }
    g_rel_ns_ecrit += maintenant_ns() - t0;
    ReleaseSRWLockExclusive(&g_rel_lock);
    return fait;
}

// Le rendu `id` de [addr, addr + size) n'a jamais été retouché : ni lu, ni écrit, ni restauré depuis.
bool mem_released_intact(uint64_t addr, uint64_t size, uint64_t id) {
    if (!id || !size) return false;
    AcquireSRWLockShared(&g_rel_lock);
    auto it = g_rel.find(addr);
    const bool ok = it != g_rel.end() && it->second.fin == addr + size && it->second.id == id;
    ReleaseSRWLockShared(&g_rel_lock);
    return ok;
}

// L'image qui a rendu [addr, addr + size) sous le numéro `id` a encore (ou n'a plus) d'objet texture du jeu. Sans
// objet, une écriture du jeu dans ces pages veut dire qu'il réemploie la mémoire (mem_fault).
void mem_release_vivant(uint64_t addr, uint64_t size, uint64_t id, bool vivant) {
    if (!id || !size || !g_rel_present.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_rel_lock);
    auto it = g_rel.upper_bound(addr);
    if (it != g_rel.begin()) --it;
    for (; it != g_rel.end() && it->first < addr + size; ++it)
        if (it->second.id == id) it->second.vivant = vivant;
    ReleaseSRWLockExclusive(&g_rel_lock);
}

void mem_restore(uint64_t addr, uint64_t size) {
    if (!size || !g_rel_present.load(std::memory_order_acquire)) return;
    AcquireSRWLockExclusive(&g_rel_lock);
    rel_restaurer(addr & ~0xfffull, (addr + size + 0xfff) & ~0xfffull, 2);
    ReleaseSRWLockExclusive(&g_rel_lock);
}

void mem_release_stats(RelStats* s) {
    s->rendus = g_rel_octets.load(), s->cumul = g_rel_cumul.load();
    for (int i = 0; i < 6; i++) s->n[i] = g_rel_n[i].load(), s->octets[i] = g_rel_o[i].load();
    s->ns_ecrit = g_rel_ns_ecrit.load(), s->ns_lu = g_rel_ns_lu.load();
    AcquireSRWLockShared(&g_rel_lock);
    s->fichier = g_rel_taille;
    ReleaseSRWLockShared(&g_rel_lock);
}

bool mem_commit_lazy(uint64_t addr, uint64_t size) {
    if (!settings().memory_saver) return mem_commit(addr, size);   // recomp.ini : comportement de la 1.0.2
    if (!size) return true;
    if (g_rel_present.load(std::memory_order_acquire) || g_vides_present.load(std::memory_order_acquire)) {
        // nouvelle plage : d'anciennes pages rendues (textures, pages vides) n'ont plus d'objet
        AcquireSRWLockExclusive(&g_rel_lock);
        rel_retirer(addr & ~0xfffull, (addr + size + 0xfff) & ~0xfffull);
        vides_retirer(addr & ~0xfffull, (addr + size + 0xfff) & ~0xfffull);
        ReleaseSRWLockExclusive(&g_rel_lock);
    }
    AcquireSRWLockExclusive(&g_lazy_lock);
    lazy_retirer(addr, size);
    g_lazy[addr] = addr + size;
    ReleaseSRWLockExclusive(&g_lazy_lock);
    return true;
}

// Accès fautif à `addr` : si l'adresse est dans une plage à la demande pas encore engagée, engage son bloc de 2 Mo
// et rend vrai (l'instruction fautive est alors reprise). Faux : ce n'est pas une page à engager (vrai plantage).
int mem_fault(uint64_t addr, bool ecriture) {
    AcquireSRWLockShared(&g_lazy_lock);
    const auto [s, e] = lazy_plage(addr);
    ReleaseSRWLockShared(&g_lazy_lock);
    if (!e) return 0;
    uint64_t b0 = std::max(s, addr & ~(LAZY_BLOC - 1)), b1 = std::min(e, (addr & ~(LAZY_BLOC - 1)) + LAZY_BLOC);
    int code = 1;
    // Pages rendues (mem_release, vides_balayer) : le verrou exclut un rendu ou une autre restauration jusqu'à la fin de
    // l'engagement.
    const bool rel = g_rel_present.load(std::memory_order_acquire) || g_vides_present.load(std::memory_order_acquire);
    struct Garde {
        bool pris;
        ~Garde() {
            if (pris) ReleaseSRWLockExclusive(&g_rel_lock);
        }
    } garde{rel};
    if (rel) {
        AcquireSRWLockExclusive(&g_rel_lock);
        const uint64_t page = addr & ~0xfffull;
        auto it = g_rel.upper_bound(page);
        if (it != g_rel.begin() && page < std::prev(it)->second.fin) {
            --it;
            const bool sans_objet = ecriture && !it->second.vivant;
            if (sans_objet && g_rel_abandon) {
                // RECOMP_TEX_DROP (essais) : ancien comportement, la plage entière est oubliée sans rien relire ; ses
                // pages reviennent vides au premier accès, par l'engagement ordinaire ci-dessous.
                const uint64_t s0 = it->first, e0 = it->second.fin;
                rel_retirer(s0, e0);
                g_rel_n[4]++;
                g_rel_o[4] += e0 - s0;
                code = 4;
                it = g_rel.upper_bound(page);
            } else {
                // Le contenu revient du fichier, page par page, que la texture ait encore un objet ou non : le jeu
                // recrée ses objets texture sur des données restées en place. Sans objet, les autres parties encore
                // rendues du même rendu sont marquées : les relire plus tard compte au compteur 5.
                if (sans_objet && !it->second.entame) {
                    const uint64_t id = it->second.id;
                    for (auto a = it;; --a) {
                        if (a->second.id != id) break;
                        a->second.entame = true;
                        if (a == g_rel.begin()) break;
                    }
                    for (auto a = std::next(it); a != g_rel.end() && a->second.id == id; ++a) a->second.entame = true;
                }
                // Pages oubliées : la suite rangée ensemble (2 Mo au plus) revient en entier, en un ou deux arrêts des
                // autres fils au lieu d'un par morceau de 64 Ko ; ses voisines sont en général touchées juste après.
                // Données de texture relues ou réécrites en service (causes 0 et 1) : REL_AVANCE d'un coup à partir du
                // morceau fautif, le jeu les lit à la suite ; sinon ces fautes arrivent par rafales de plus de mille
                // arrêts (à-coups des changements de scène allongés de ≈ 100 ms, mesuré le 2026-11-13).
                const bool suite = it->second.froid && g_arret_actif;
                const uint64_t avance = !sans_objet && g_arret_actif ? std::max(REL_AVANCE, g_rel_morceau) : g_rel_morceau;
                const uint64_t m0 = suite ? it->first : std::max(it->first, page & ~(g_rel_morceau - 1));
                const uint64_t m1 = suite ? it->second.fin
                                          : std::min(it->second.fin, (page & ~(g_rel_morceau - 1)) + avance);
                if (rel_restaurer(m0, m1, sans_objet ? 4 : ecriture ? 1 : 0, true)) return sans_objet ? 5 : ecriture ? 3 : 2;
                if (!g_manque_memoire.exchange(true))
                    HLOG("mem", "released game page 0x%llx not restored (error %lu): the PC is low on memory (%.1f GB "
                                "available commit; close other programs)", (unsigned long long)page, GetLastError(),
                         memoire_validable_libre_go());
                return 0;
            }
        }
        // bloc à engager : sans les pages rendues qu'il contient (engagées ici, elles se liraient à zéro)
        if (it != g_rel.begin()) b0 = std::max(b0, std::prev(it)->second.fin);
        if (it != g_rel.end()) b1 = std::min(b1, it->first);
        // pages vides rendues à Windows : seul le morceau de 64 Ko touché revient ; hors d'elles, le bloc s'arrête à
        // leurs bords (un bloc de 2 Mo les réengagerait toutes)
        auto v = g_vides.upper_bound(page);
        if (v != g_vides.begin() && page < std::prev(v)->second) {
            --v;
            b0 = std::max({b0, v->first, page & ~(VIDE_MORCEAU - 1)});
            b1 = std::min({b1, v->second, (page & ~(VIDE_MORCEAU - 1)) + VIDE_MORCEAU});
        } else {
            if (v != g_vides.begin()) b0 = std::max(b0, std::prev(v)->second);
            if (v != g_vides.end()) b1 = std::min(b1, v->first);
        }
    }
    // Le bloc peut être déjà engagé : un autre fil a fauté dessus au même instant et l'a engagé entre-temps (vu le
    // 2026-11-11 : plantage à 30 s quand ce cas était pris pour une erreur). Engager de nouveau ne change rien et
    // l'instruction reprend. VirtualQuery (≈ 25 µs dans ce grand espace réservé) n'est appelé que si la même page
    // faute trois fois de suite sur ce fil : une page déjà engagée qui faute encore est un vrai plantage.
    thread_local uint64_t derniere = 0;
    thread_local int repetitions = 0;
    const uint64_t page = addr & ~0xfffull;
    if (page != derniere) {
        derniere = page;
        repetitions = 0;
    } else if (++repetitions >= 3) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)addr, &mbi, sizeof mbi) || mbi.State == MEM_COMMIT) return 0;
    }
    if (!VirtualAlloc((void*)b0, b1 - b0, MEM_COMMIT, PAGE_READWRITE)) {
        // la mémoire validable manque : au moins la page demandée (marge rendue, caches vidés, essais pendant 5 s),
        // sinon le jeu s'arrête (message clair, code 4)
        const uint64_t p0 = addr & ~0xfffull;
        if (!engager(p0, 0x1000)) {
            if (!g_manque_memoire.exchange(true))
                HLOG("mem", "game page 0x%llx refused by Windows (error %lu): the PC is low on memory (%.1f GB "
                            "available commit; close other programs)", (unsigned long long)p0, GetLastError(),
                     memoire_validable_libre_go());
            return 0;
        }
        g_lazy_octets += 0x1000;
        age_noter(p0, p0 + 0x1000);
        if (rel) g_vides_reengage += vides_retirer(p0, p0 + 0x1000);
        return code;
    }
    g_lazy_blocs++;
    g_lazy_octets += b1 - b0;
    age_noter(b0, b1);
    if (rel) g_vides_reengage += vides_retirer(b0, b1);
    return code;
}

// Engage d'avance toute la partie à la demande de [addr, addr + size) : avant de passer la plage au noyau de Windows
// ou au pilote graphique. Faux si Windows refuse la mémoire.
bool mem_ensure(uint64_t addr, uint64_t size) {
    if (!size) return true;
    const uint64_t end = addr + size;
    bool ok = true;
    // pages rendues de la plage : restaurées d'abord (engagées ici, elles se liraient à zéro) ; le verrou exclut un
    // nouveau rendu jusqu'à la fin de l'engagement
    const bool rel = g_rel_present.load(std::memory_order_acquire) || g_vides_present.load(std::memory_order_acquire);
    if (rel) {
        AcquireSRWLockExclusive(&g_rel_lock);
        if (!rel_restaurer(addr & ~0xfffull, (end + 0xfff) & ~0xfffull, 3)) ok = false;
    }
    AcquireSRWLockShared(&g_lazy_lock);
    auto it = g_lazy.upper_bound(addr);
    if (it != g_lazy.begin()) --it;
    for (; it != g_lazy.end() && it->first < end; ++it) {
        const uint64_t s = std::max(it->first, addr & ~0xfffull), e = std::min(it->second, (end + 0xfff) & ~0xfffull);
        if (s >= e) continue;
        if (!engager(s, e - s)) {
            ok = false;
            continue;
        }
        age_noter(s, e);
        if (rel) g_vides_reengage += vides_retirer(s, e);   // pages vides rendues : engagées de nouveau (à zéro)
    }
    ReleaseSRWLockShared(&g_lazy_lock);
    if (rel) ReleaseSRWLockExclusive(&g_rel_lock);
    return ok;
}

void mem_marge_init() {
    if (!settings().memory_saver || g_marge.load()) return;
    g_marge = VirtualAlloc(nullptr, MARGE, MEM_RESERVE | MEM_COMMIT, PAGE_NOACCESS);
}

int mem_pression() { return g_pression.load(std::memory_order_relaxed); }

// Veille de la mémoire du PC. Le compilateur du pilote graphique prend 2 à 3 Go en deux secondes dans notre processus
// (README, « Pics de mémoire en jeu ») : si Windows lui refuse cette mémoire, il plante sans message et le jeu avec lui.
// Les protections d'engager() ne servent qu'au premier refus fait au jeu lui-même, et l'état de la mémoire n'était
// relu par le rendu qu'une fois par seconde. Toutes les 50 ms, ce fil relit la mémoire validable libre ; sous
// VEILLE_SEUIL, il rend la marge de 256 Mo et prévient le rendu (mem_pression), qui détruit à l'image suivante les
// images sans objet (jusqu'à 1,5 Go) et rend les fenêtres d'import inutilisées. Tant que la mémoire reste basse, le
// rendu est prévenu de nouveau quatre fois par seconde.
// RECOMP_VEILLE_GO=n : autre seuil, en Go (essais ; 0 : pas de veille).
void mem_veille_init() {
    if (!settings().memory_saver) return;
    static std::atomic<bool> lancee{false};
    if (lancee.exchange(true)) return;
    const double seuil = getenv("RECOMP_VEILLE_GO") ? atof(getenv("RECOMP_VEILLE_GO")) : 2.0;
    if (seuil <= 0) return;
    std::thread([seuil] {
        int episodes = 0, depuis = 0;
        bool bas = false;
        for (;;) {
            Sleep(50);
            const double libre = memoire_validable_libre_go();
            if (libre >= seuil) {
                bas = false;
                continue;
            }
            if (!bas) {   // début d'un épisode
                bas = true;
                depuis = 0;
                if (void* m = g_marge.exchange(nullptr)) VirtualFree(m, 0, MEM_RELEASE);
                if (episodes++ < 5)
                    HLOG("mem", "PC memory very low (%.1f GB available commit): reserve released, image caches being "
                                "emptied (close other programs)", libre);
            }
            if (depuis++ % 5 == 0) g_pression++;   // tout de suite, puis toutes les 250 ms
        }
    }).detach();
}

// (blocs engagés au premier accès, octets engagés ainsi, octets des plages à la demande)
// ---------------------------------------------------------------- recensement de la mémoire du jeu (diagnostic)
// RECOMP_MEM_CENSUS=s : toutes les s secondes, combien de la mémoire du jeu engagée à la demande pourrait être rendue à
// Windows sans rien comprendre du jeu (question de l'utilisateur, 2026-11-12 : une « réservation dynamique », qui rende
// aussi ce que le jeu n'utilise plus). Une page sur 16 des plages à la demande engagées est examinée :
//   * vide : elle ne contient que des zéros (engagée avec son bloc de 2 Mo sans avoir été écrite, ou effacée par le
//     jeu). Rendue à Windows, elle se relirait à zéro : rien ne changerait pour le jeu ;
//   * touchée : le jeu l'a lue ou écrite depuis le recensement précédent. Après chaque recensement, chaque page examinée
//     est retirée de l'ensemble de travail du processus (VirtualUnlock sur une page non verrouillée : son contenu reste,
//     en RAM sur la liste d'attente de Windows) ; elle y revient au premier accès (faute douce, de l'ordre de la
//     microseconde) ; QueryWorkingSetEx dit au recensement suivant si elle y est revenue. Une page lue seulement par le
//     GPU (fenêtre importée) n'y revient pas par ce chemin : elle est verrouillée, donc jamais retirée, et comptée touchée.
// Lecture par ReadProcessMemory : une page rendue ou désengagée entre-temps n'est ni restaurée ni engagée par la lecture,
// elle est sautée. Coût par période : la lecture de 1/16 de la mémoire du jeu, et autant de fautes douces ensuite.
// Le journal donne des estimations (fraction de l'échantillon × mémoire engagée exacte), au total et par plage.
namespace {
void recenser(unsigned periode) {
    constexpr uint64_t PAGE = 0x1000, PAS = 16 * PAGE;
    constexpr DWORD LISIBLE = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY |
                              PAGE_EXECUTE_WRITECOPY;
    struct Suivi { uint32_t premier, touchee, vue; };   // tours : première fois examinée, dernier accès vu, dernier passage
    std::unordered_map<uint64_t, Suivi> suivi;
    std::vector<uint64_t> tampon(PAGE / 8);
    const HANDLE moi = GetCurrentProcess();
    for (uint32_t tour = 1;; tour++) {
        Sleep(periode * 1000);
        const ULONGLONG t0 = GetTickCount64();
        std::vector<std::pair<uint64_t, uint64_t>> plages;
        AcquireSRWLockShared(&g_lazy_lock);
        for (auto& [s, e] : g_lazy) plages.push_back({s, e});
        ReleaseSRWLockShared(&g_lazy_lock);
        struct Plage { uint64_t s, e, engage, n, vides, touchees, froides; };
        std::vector<Plage> par_plage;
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> wi;
        std::vector<uint32_t> plage_de;
        uint64_t engage = 0;
        for (auto [s, e] : plages) {
            Plage pl{s, e, 0, 0, 0, 0, 0};
            for (uint64_t a = s; a < e;) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
                const uint64_t fin = std::min<uint64_t>(e, (uint64_t)mbi.BaseAddress + mbi.RegionSize);
                if (mbi.State == MEM_COMMIT && (mbi.Protect & LISIBLE) && !(mbi.Protect & PAGE_GUARD)) {
                    pl.engage += fin - a;
                    for (uint64_t p = (a + PAS - 1) / PAS * PAS; p < fin; p += PAS) {
                        PSAPI_WORKING_SET_EX_INFORMATION w{};
                        w.VirtualAddress = (void*)p;
                        wi.push_back(w);
                        plage_de.push_back((uint32_t)par_plage.size());
                    }
                }
                a = fin > a ? fin : a + PAGE;
            }
            engage += pl.engage;
            par_plage.push_back(pl);
        }
        if (!wi.empty() && !QueryWorkingSetEx(moi, wi.data(), (DWORD)(wi.size() * sizeof wi[0]))) {
            HLOG("mem", "census %u: QueryWorkingSetEx failed (error %lu)", tour, GetLastError());
            continue;
        }
        uint64_t n = 0, vides = 0, touchees = 0, f2 = 0, f5 = 0, f10 = 0, vides_f5 = 0, nouvelles = 0;
        for (size_t i = 0; i < wi.size(); i++) {
            const uint64_t p = (uint64_t)wi[i].VirtualAddress;
            SIZE_T lus = 0;
            if (!ReadProcessMemory(moi, (void*)p, tampon.data(), PAGE, &lus) || lus != PAGE) continue;
            bool vide = true;
            for (uint64_t m : tampon)
                if (m) {
                    vide = false;
                    break;
                }
            auto [it, neuve] = suivi.try_emplace(p, Suivi{tour, 0, tour});
            Suivi& s = it->second;
            // revenue dans l'ensemble de travail depuis qu'on l'en a retirée au tour précédent : le jeu y a accédé
            const bool touchee = !neuve && s.vue == tour - 1 && wi[i].VirtualAttributes.Valid;
            if (neuve || s.vue != tour - 1) s.premier = tour, s.touchee = 0, nouvelles++;
            if (touchee) s.touchee = tour;
            s.vue = tour;
            const uint32_t age = tour - (s.touchee ? s.touchee : s.premier);   // périodes sans accès vu
            n++;
            vides += vide;
            touchees += touchee;
            f2 += age >= 2;
            f5 += age >= 5;
            f10 += age >= 10;
            vides_f5 += vide && age >= 5;
            Plage& pl = par_plage[plage_de[i]];
            pl.n++;
            pl.vides += vide;
            pl.touchees += touchee;
            pl.froides += age >= 5;
            VirtualUnlock((void*)p, PAGE);   // retirée de l'ensemble de travail ; son prochain accès l'y ramènera
        }
        for (auto it = suivi.begin(); it != suivi.end();) it = it->second.vue != tour ? suivi.erase(it) : std::next(it);
        auto go = [&](uint64_t k) { return n ? engage * (double)k / n / 1073741824.0 : 0.0; };
        HLOG("mem", "census %u (every %u s): game memory committed on demand %.2f GB in %zu ranges; %llu pages examined "
                    "(1 in 16, %llu new): all zeros %.2f GB, touched since the previous census %.2f GB, untouched for 2+ "
                    "periods %.2f GB, 5+ %.2f GB, 10+ %.2f GB; all zeros and untouched 5+ periods %.2f GB [%llu ms]",
             tour, periode, engage / 1073741824.0, plages.size(), (unsigned long long)n, (unsigned long long)nouvelles,
             go(vides), tour > 1 ? go(touchees) : 0.0, go(f2), go(f5), go(f10), go(vides_f5),
             (unsigned long long)(GetTickCount64() - t0));
        std::sort(par_plage.begin(), par_plage.end(), [](const Plage& a, const Plage& b) { return a.engage > b.engage; });
        for (size_t k = 0; k < par_plage.size() && k < 8; k++) {
            const Plage& pl = par_plage[k];
            if (!pl.n) continue;
            HLOG("mem", "  range 0x%llx +%.0f MB: committed %.0f MB; all zeros %.0f %%, touched %.0f %%, untouched 5+ "
                        "periods %.0f %%",
                 (unsigned long long)pl.s, (pl.e - pl.s) / 1048576.0, pl.engage / 1048576.0, 100.0 * pl.vides / pl.n,
                 tour > 1 ? 100.0 * pl.touchees / pl.n : 0.0, 100.0 * pl.froides / pl.n);
        }
    }
}
}  // namespace

void mem_census_init() {
    const char* e = getenv("RECOMP_MEM_CENSUS");
    const int periode = e ? atoi(e) : 0;
    if (periode <= 0 || !settings().memory_saver) return;
    HLOG("mem", "memory census every %d s (RECOMP_MEM_CENSUS, diagnostic): 1 game page in 16 examined", periode);
    std::thread(recenser, (unsigned)periode).detach();
}

namespace {
bool est_vide(const uint64_t* p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i]) return false;
    return true;
}
std::vector<std::pair<uint64_t, uint64_t>> epingles_copie() {
    std::lock_guard<std::mutex> lk(g_epingle_mx);
    std::vector<std::pair<uint64_t, uint64_t>> v;
    for (auto& [k, n] : g_epingles) v.push_back(k);
    return v;
}
bool epingle(const std::vector<std::pair<uint64_t, uint64_t>>& v, uint64_t a, uint64_t b) {
    for (auto& [s, e] : v)
        if (a < e && s < b) return true;
    return false;
}

// ---------------------------------------------------------------- pages pleines oubliées (cold_page_release)
// Mesuré le 2026-11-12 (recensement) : en plus des pages vides, 0,3 à 0,4 Go de pages pleines de la mémoire du jeu ne
// sont pas touchées pendant 4 minutes et plus. Le balayage les range dans le fichier temporaire des textures (mêmes
// rendus que mem_release, dans g_rel, marqués froid) : elles ne comptent plus dans la mémoire engagée. Au premier
// accès, mem_fault relit le morceau de 64 Ko du fichier (quelques dizaines de microsecondes depuis le cache de
// Windows), comme pour une texture ; mem_ensure et mem_restore aussi, pour une plage entière.
// Savoir qu'un morceau n'est pas touché : le balayage le retire de l'ensemble de travail du processus (VirtualUnlock
// sur des pages non verrouillées : leur contenu reste en RAM, sur la liste des pages modifiées de Windows) ; au premier
// accès du jeu, du runtime ou du noyau, la page y revient (faute douce, de l'ordre de la microseconde), et
// QueryWorkingSetEx le dit aux balayages suivants. Un morceau pas revenu pendant g_froid_tours balayages (8, soit
// 4 minutes) est rangé. Seuls les morceaux trouvés pleins par le balayage des pages vides sont suivis, hors des
// fenêtres importées dans Vulkan. Le contrôle se fait avant les lectures du balayage, qui ramèneraient des pages dans
// l'ensemble de travail. Coût borné : au plus 128 Mo retirés par balayage, un appel par bloc de 2 Mo (64 au plus,
// 2 ms de pause tous les 8), à partir du 4e balayage (le premier niveau est chargé) ; un morceau trouvé touché attend
// 4, 8 puis 16 balayages avant d'être retiré de nouveau, un morceau relu du fichier 16. Rangement par suites de 2 Mo
// au plus, 1 ms de pause avant chacune : un fil du jeu qui attend le verrou de mem_fault passe.
// Jusqu'à la 1.0.3, limite connue, la même que pour les textures : pendant la restauration d'une page (moins d'une
// microseconde entre son engagement et sa copie), un autre fil qui y accédait sans fauter lisait des zéros ou voyait son
// écriture recouverte. Vu chez un joueur (issue GitHub n° 17) : la restauration est maintenant atomique (MondeArrete).
constexpr uint64_t FROID_RETRAIT_MAX = 128ull << 20;   // retirés de l'ensemble de travail par balayage
constexpr unsigned FROID_APPELS_MAX = 64;              // appels de VirtualUnlock par balayage (un par bloc de 2 Mo)
constexpr uint64_t FROID_SUITE_MAX = 2ull << 20;       // rangés par prise du verrou (écriture dans le fichier)
constexpr uint32_t FROID_DEBUT = 4;                    // premier balayage qui retire des pages

// Range [a, b) dans le fichier temporaire (verrous tenus : g_rel_lock et la garde du noyau en exclusif). 1 : rangé ;
// 0 : laissé (épinglé, plus engagé en lecture-écriture, protection refusée) ; -1 : fichier plein ou erreur d'écriture.
int froid_ranger(uint64_t a, uint64_t b) {
    if (epingle(epingles_copie(), a, b)) return 0;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Protect != PAGE_READWRITE ||
        (uint64_t)mbi.BaseAddress + mbi.RegionSize < b)
        return 0;
    const uint64_t off = rel_place(b - a);
    if (off == UINT64_MAX) return -1;
    DWORD avant = 0;
    // en lecture seule le temps de la copie : une écriture du jeu faute et attend le verrou dans mem_fault, puis relit
    // sa page du fichier ; une lecture voit le contenu, qui ne change plus
    if (!VirtualProtect((void*)a, b - a, PAGE_READONLY, &avant)) {
        rel_liberer(off, b - a);
        return 0;
    }
    if (!rel_io(true, off, (void*)a, b - a)) {
        if (!g_rel_erreur.exchange(true))
            HLOG("mem", "forgotten game pages: temporary file not written (error %lu): nothing more is put there, "
                        "texture data included", GetLastError());
        VirtualProtect((void*)a, b - a, PAGE_READWRITE, &avant);
        rel_liberer(off, b - a);
        return -1;
    }
    if (!VirtualFree((void*)a, b - a, MEM_DECOMMIT)) {
        VirtualProtect((void*)a, b - a, PAGE_READWRITE, &avant);
        rel_liberer(off, b - a);
        return 0;
    }
    Rendu r{b, off, ++g_rel_id, true};
    r.froid = true;
    g_rel[a] = r;
    g_rel_present.store(true, std::memory_order_release);
    g_rel_ecrits += b - a;
    g_froid_octets += b - a;
    g_froid_cumul += b - a;
    g_mem_gen++;
    return 1;
}

void vides_balayer(unsigned periode) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    constexpr uint64_t M = VIDE_MORCEAU, SUITE_MAX = 2ull << 20, PAGES = VIDE_MORCEAU / 0x1000;
    const HANDLE moi = GetCurrentProcess();
    std::vector<uint64_t> tampon(M / 8);
    std::unordered_set<uint64_t> pleins;   // morceaux trouvés pleins
    struct Froid {
        uint32_t retire = 0;     // balayage où le morceau a été retiré de l'ensemble de travail (0 : pas observé)
        uint32_t prochain = 0;   // premier balayage où il peut l'être de nouveau
        uint8_t chauds = 0;      // fois où il a été trouvé touché après un retrait
    };
    std::unordered_map<uint64_t, Froid> froids;   // morceaux pleins suivis (pages oubliées)
    std::unordered_set<uint64_t> deja_ranges;     // morceaux rangés : relus du fichier, ils attendront plus longtemps
    uint64_t curseur = 0;                         // reprise de la rotation des retraits
    for (uint32_t k = 1;; k++) {
        // Premiers balayages à 10 s et 25 s : un bloc n'est examiné qu'après un balayage entier, et le pic de mémoire
        // du démarrage (vers 45 s, chargement du premier niveau) tombait avant le premier rendu utile (60 s). Les pages
        // vides des réserves prises au démarrage (vers 3 s) sont ainsi rendues dès 25 s.
        Sleep((k == 1 ? std::min(periode, 10u) : k == 2 ? std::min(periode, 15u) : periode) * 1000);
        g_tour.store(k + 1, std::memory_order_relaxed);   // ce qui est engagé à partir d'ici attend la période suivante
        const ULONGLONG t0 = GetTickCount64();
        std::vector<std::pair<uint64_t, uint64_t>> plages;
        AcquireSRWLockShared(&g_lazy_lock);
        for (auto& [s, e] : g_lazy) plages.push_back({s, e});
        ReleaseSRWLockShared(&g_lazy_lock);
        const auto ep = epingles_copie();
        std::vector<uint64_t> cand;
        for (auto [s, e] : plages)
            for (uint64_t a = s; a < e;) {
                MEMORY_BASIC_INFORMATION mbi;
                if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
                const uint64_t fin = std::min<uint64_t>(e, (uint64_t)mbi.BaseAddress + mbi.RegionSize);
                if (mbi.State == MEM_COMMIT && mbi.Protect == PAGE_READWRITE)
                    for (uint64_t c = (a + M - 1) & ~(M - 1); c + M <= fin; c += M) {
                        if (c < AGE_BASE || c + M > AGE_FIN) continue;
                        if (g_age[(c - AGE_BASE) >> 21].load(std::memory_order_relaxed) + 1 > k) continue;   // trop récent
                        if (epingle(ep, c, c + M)) continue;
                        cand.push_back(c);
                    }
                a = fin > a ? fin : a + 0x1000;
            }
        if (pleins.size() > 2 * cand.size() + 4096) {   // morceaux disparus depuis (pages rendues, réserves libérées)
            std::unordered_set<uint64_t> garder;
            for (uint64_t c : cand)
                if (pleins.count(c)) garder.insert(c);
            pleins.swap(garder);
        }
        if (froids.size() > cand.size() + 4096) {   // de même pour les morceaux suivis
            std::unordered_map<uint64_t, Froid> garder;
            for (uint64_t c : cand) {
                auto f = froids.find(c);
                if (f != froids.end()) garder.insert(*f);
            }
            froids.swap(garder);
        }
        if (deja_ranges.size() > 200000) deja_ranges.clear();
        // pages oubliées, 1 : les morceaux retirés de l'ensemble de travail y sont-ils revenus ? Avant toute lecture
        // de ce balayage, qui les y ramènerait.
        const bool froid = g_froid_actif.load(std::memory_order_relaxed) && mem_release_actif();
        std::vector<uint64_t> a_ranger;
        size_t observes = 0;
        if (froid) {
            std::vector<uint64_t> obs;
            for (uint64_t c : cand) {
                auto f = froids.find(c);
                if (f != froids.end() && f->second.retire) obs.push_back(c);
            }
            observes = obs.size();
            std::vector<PSAPI_WORKING_SET_EX_INFORMATION> wq;
            for (size_t i = 0; i < obs.size();) {
                const size_t n = std::min<size_t>(256, obs.size() - i);   // 4 096 pages par appel
                wq.assign(n * PAGES, PSAPI_WORKING_SET_EX_INFORMATION{});
                for (size_t j = 0; j < n; j++)
                    for (uint64_t q = 0; q < PAGES; q++) wq[j * PAGES + q].VirtualAddress = (void*)(obs[i + j] + q * 0x1000);
                if (!QueryWorkingSetEx(moi, wq.data(), (DWORD)(wq.size() * sizeof wq[0]))) break;
                for (size_t j = 0; j < n; j++) {
                    bool touche = false;
                    for (uint64_t q = 0; q < PAGES && !touche; q++) touche = wq[j * PAGES + q].VirtualAttributes.Valid;
                    Froid& f = froids[obs[i + j]];
                    if (touche) {   // revenu : le jeu s'en sert ; prochain retrait dans 4, 8 puis 16 balayages
                        f.chauds = (uint8_t)std::min(f.chauds + 1, 3);
                        f.prochain = k + (2u << f.chauds);
                        f.retire = 0;
                    } else if (k - f.retire >= g_froid_tours) {
                        a_ranger.push_back(obs[i + j]);
                    }
                }
                i += n;
            }
        }
        // morceaux déjà trouvés pleins : relus un balayage sur quatre, et seulement s'ils sont en RAM
        std::vector<PSAPI_WORKING_SET_EX_INFORMATION> wi;
        for (uint64_t c : cand)
            if (pleins.count(c) && ((c / M) & 3) == (k & 3)) {
                PSAPI_WORKING_SET_EX_INFORMATION w{};
                w.VirtualAddress = (void*)c;
                wi.push_back(w);
            }
        std::unordered_set<uint64_t> a_relire;
        if (!wi.empty() && QueryWorkingSetEx(moi, wi.data(), (DWORD)(wi.size() * sizeof wi[0])))
            for (auto& w : wi)
                if (w.VirtualAttributes.Valid) a_relire.insert((uint64_t)w.VirtualAddress);
        std::vector<std::pair<uint64_t, uint64_t>> suites;
        uint64_t lus = 0;
        for (uint64_t c : cand) {
            if (pleins.count(c) && !a_relire.count(c)) continue;
            SIZE_T n = 0;
            if (!ReadProcessMemory(moi, (void*)c, tampon.data(), M, &n) || n != M) continue;
            lus++;
            if (!est_vide(tampon.data(), M / 8)) {
                if (pleins.insert(c).second && deja_ranges.erase(c)) {   // relu du fichier depuis son rangement
                    Froid& f = froids[c];
                    f.chauds = 3;
                    f.prochain = k + 16;
                    f.retire = 0;
                }
                continue;
            }
            pleins.erase(c);
            froids.erase(c);
            if (!suites.empty() && suites.back().second == c && suites.back().second - suites.back().first < SUITE_MAX)
                suites.back().second += M;
            else
                suites.push_back({c, c + M});
        }
        // rendu, suite par suite : sous le verrou de mem_fault, et seulement sans lecture du noyau ni import en cours.
        // Verrous tenus en priorité normale : un fil du jeu qui les attend ne reste pas derrière ce fil de basse
        // priorité.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        uint64_t rendus = 0, occupees = 0;
        unsigned prises = 0;
        for (auto [a, b] : suites) {
            // 1 ms de pause toutes les 8 suites : un fil du jeu qui attend le verrou passe (un verrou SRW relâché puis
            // repris aussitôt par le même fil ne laisse pas toujours passer ceux qui attendent)
            if (prises++ % 8 == 7) Sleep(1);
            AcquireSRWLockExclusive(&g_rel_lock);
            if (!TryAcquireSRWLockExclusive(&g_noyau_lock)) {
                ReleaseSRWLockExclusive(&g_rel_lock);
                occupees++;
                continue;
            }
            DWORD avant = 0;
            MEMORY_BASIC_INFORMATION mbi;
            // encore engagée en lecture-écriture d'un bout à l'autre : la protection remise plus bas est la bonne
            if (!epingle(epingles_copie(), a, b) && VirtualQuery((void*)a, &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                mbi.Protect == PAGE_READWRITE && (uint64_t)mbi.BaseAddress + mbi.RegionSize >= b &&
                VirtualProtect((void*)a, b - a, PAGE_READONLY, &avant)) {
                auto rendre = [&](uint64_t x0, uint64_t x1) {
                    if (VirtualFree((void*)x0, x1 - x0, MEM_DECOMMIT)) {
                        vides_ajouter(x0, x1);
                        g_vides_octets += x1 - x0;
                        g_vides_cumul += x1 - x0;
                        rendus += x1 - x0;
                    } else {
                        VirtualProtect((void*)x0, x1 - x0, PAGE_READWRITE, &avant);
                    }
                };
                uint64_t d0 = 0;
                for (uint64_t c = a; c < b; c += M) {
                    // en lecture seule : une écriture faite avant la protection se voit ici
                    if (est_vide((const uint64_t*)c, M / 8)) {
                        if (!d0) d0 = c;
                        continue;
                    }
                    if (d0) rendre(d0, c), d0 = 0;
                    VirtualProtect((void*)c, M, PAGE_READWRITE, &avant);
                    pleins.insert(c);
                }
                if (d0) rendre(d0, b);
                g_mem_gen++;
            }
            ReleaseSRWLockExclusive(&g_noyau_lock);
            ReleaseSRWLockExclusive(&g_rel_lock);
        }
        if (rendus || k % 10 == 1)
            HLOG("mem", "empty game pages given back to Windows: %.0f MB now (+%.0f MB this sweep; %.0f MB in total, "
                        "%.0f MB committed again when touched); %zu pieces of 64 KB eligible, %llu read, %llu runs "
                        "postponed (kernel or driver busy) [%llu ms]",
                 g_vides_octets.load() / 1048576.0, rendus / 1048576.0, g_vides_cumul.load() / 1048576.0,
                 g_vides_reengage.load() / 1048576.0, cand.size(), (unsigned long long)lus, (unsigned long long)occupees,
                 (unsigned long long)(GetTickCount64() - t0));
        // pages oubliées, 2 : rangées dans le fichier, suite par suite (au plus 256 Ko par prise du verrou)
        uint64_t ranges = 0, froid_occupees = 0;
        if (froid && !a_ranger.empty()) {
            std::vector<std::pair<uint64_t, uint64_t>> fs;
            for (uint64_t c : a_ranger)
                if (!fs.empty() && fs.back().second == c && fs.back().second - fs.back().first < FROID_SUITE_MAX)
                    fs.back().second += M;
                else
                    fs.push_back({c, c + M});
            for (auto [a, b] : fs) {
                Sleep(1);   // écriture dans le fichier : pause avant chaque suite (voir plus haut)
                AcquireSRWLockExclusive(&g_rel_lock);
                if (!TryAcquireSRWLockExclusive(&g_noyau_lock)) {
                    ReleaseSRWLockExclusive(&g_rel_lock);
                    froid_occupees++;
                    continue;   // toujours en observation : au balayage suivant
                }
                const int r = mem_release_actif() ? froid_ranger(a, b) : -1;
                ReleaseSRWLockExclusive(&g_noyau_lock);
                ReleaseSRWLockExclusive(&g_rel_lock);
                if (r < 0) break;   // fichier plein ou en erreur : les morceaux restent en observation
                for (uint64_t c = a; c < b; c += M) {
                    froids.erase(c);
                    if (r) pleins.erase(c), deja_ranges.insert(c);
                }
                if (r) ranges += b - a;
            }
        }
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        // pages oubliées, 3 : nouveaux retraits de l'ensemble de travail, en tournant sur les morceaux pleins. Un seul
        // VirtualUnlock par bloc de 2 Mo, du premier au dernier morceau retenu du bloc : chaque appel vide les TLB de
        // tous les processeurs qui exécutent le jeu (interruption), et un appel par morceau en faisait des milliers par
        // balayage (creux d'images vus le 2026-11-12). Les morceaux observés du bloc ont été contrôlés plus haut : les
        // retirer de nouveau ne fait rien perdre.
        uint64_t retires = 0;
        unsigned appels = 0;
        if (froid && k >= FROID_DEBUT && !cand.empty()) {
            const size_t i0 = std::lower_bound(cand.begin(), cand.end(), curseur) - cand.begin();
            uint64_t bloc = UINT64_MAX, s0 = 0, s1 = 0;
            auto vider = [&] {
                if (s0 < s1) {
                    VirtualUnlock((void*)s0, s1 - s0);   // échoue (pages non verrouillées), mais les retire
                    if (++appels % 8 == 0) Sleep(2);     // étalé : les fautes douces des pages encore utilisées
                }
                s0 = s1 = 0;
            };
            for (size_t n = 0; n < cand.size() && retires < FROID_RETRAIT_MAX && appels < FROID_APPELS_MAX; n++) {
                const uint64_t c = cand[(i0 + n) % cand.size()];
                if ((c & ~(LAZY_BLOC - 1)) != bloc) {
                    vider();
                    bloc = c & ~(LAZY_BLOC - 1);
                }
                if (!pleins.count(c)) continue;
                Froid& f = froids[c];
                if (f.retire || f.prochain > k) continue;
                f.retire = k;
                if (!s0) s0 = c;
                s1 = c + M;
                retires += M;
                curseur = c + M;
            }
            vider();
        }
        if (froid && (ranges || k % 10 == 1))
            HLOG("mem", "forgotten game pages put in the temporary file: %.0f MB now (+%.0f MB this sweep; %.0f MB in "
                        "total, %.0f MB read back when touched); %zu pieces of 64 KB watched, %.0f MB taken out of the "
                        "working set, %llu runs postponed [%llu ms]",
                 g_froid_octets.load() / 1048576.0, ranges / 1048576.0, g_froid_cumul.load() / 1048576.0,
                 g_froid_relus.load() / 1048576.0, observes, retires / 1048576.0, (unsigned long long)froid_occupees,
                 (unsigned long long)(GetTickCount64() - t0));
        static uint64_t arrets_dits = 0;   // restaurations atomiques (MondeArrete) : leur coût, quand il y en a eu
        if (const uint64_t na = g_arrets.load(); na != arrets_dits && (k % 10 == 1 || na > arrets_dits + 5000)) {
            arrets_dits = na;
            HLOG("mem", "pages restored with the other threads stopped: %llu times, %.1f ms in total, longest %.2f ms "
                        "(%llu threads)", (unsigned long long)na, g_arrets_ns.load() / 1e6, g_arret_max_ns.load() / 1e6,
                 (unsigned long long)g_arret_fils.load());
        }
    }
}
}  // namespace

void mem_noyau_debut() { AcquireSRWLockShared(&g_noyau_lock); }
void mem_noyau_fin() { ReleaseSRWLockShared(&g_noyau_lock); }
void mem_epingler(uint64_t addr, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_epingle_mx);
    g_epingles[{addr, addr + size}]++;
}
void mem_desepingler(uint64_t addr, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_epingle_mx);
    auto it = g_epingles.find({addr, addr + size});
    if (it != g_epingles.end() && --it->second <= 0) g_epingles.erase(it);
}

// RECOMP_ZERO_SWEEP_S=s : autre période de balayage (essais)
void mem_vides_init() {
    if (!settings().memory_saver || !settings().zero_page_release) return;
    const char* e = getenv("RECOMP_ZERO_SWEEP_S");
    const unsigned periode = e && atoi(e) > 0 ? (unsigned)atoi(e) : 30;
    g_vides_present.store(true, std::memory_order_release);   // dès maintenant, mem_fault et mem_ensure prennent le verrou
    HLOG("mem", "empty game pages given back to Windows: on (sweeps at %u s and %u s, then every %u s; pieces of 64 KB)",
         std::min(periode, 10u), std::min(periode, 10u) + std::min(periode, 15u), periode);
    // pages pleines oubliées : dans le fichier temporaire des textures, donc seulement s'il existe
    if (settings().cold_page_release) {
        if (const char* t = getenv("RECOMP_COLD_SWEEPS"))   // essais : autre nombre de balayages sans accès
            if (atoi(t) > 0) g_froid_tours = (uint32_t)atoi(t);
        if (mem_release_actif()) {
            g_froid_actif.store(true, std::memory_order_release);
            HLOG("mem", "forgotten game pages put in the temporary file: on (full pieces of 64 KB untouched for %u sweeps)",
                 g_froid_tours);
        } else {
            HLOG("mem", "forgotten game pages put in the temporary file: off (no temporary file)");
        }
    }
    std::thread(vides_balayer, periode).detach();
}

void mem_lazy_stats(uint64_t* blocs, uint64_t* octets, uint64_t* reserves) {
    *blocs = g_lazy_blocs.load();
    *octets = g_lazy_octets.load();
    uint64_t r = 0;
    AcquireSRWLockShared(&g_lazy_lock);
    for (auto& [s, e] : g_lazy) r += e - s;
    ReleaseSRWLockShared(&g_lazy_lock);
    *reserves = r;
}

void mem_decommit(uint64_t addr, uint64_t size) {
    if (size) {
        const bool rel = g_rel_present.load(std::memory_order_acquire) || g_vides_present.load(std::memory_order_acquire);
        if (rel) {   // pages rendues de la plage (textures, pages vides) : leur contenu n'a plus d'objet
            AcquireSRWLockExclusive(&g_rel_lock);
            rel_retirer(addr & ~0xfffull, (addr + size + 0xfff) & ~0xfffull);
            vides_retirer(addr & ~0xfffull, (addr + size + 0xfff) & ~0xfffull);
        }
        AcquireSRWLockExclusive(&g_lazy_lock);
        lazy_retirer(addr, size);
        ReleaseSRWLockExclusive(&g_lazy_lock);
        VirtualFree((void*)addr, size, MEM_DECOMMIT);
        if (rel) ReleaseSRWLockExclusive(&g_rel_lock);
    }
    g_mem_gen++;
}

void mem_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm, uint32_t attr) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for_range(addr, size, [&](Reg& r) { r.state = state; r.perm = perm; r.attr = attr; });
}

void mem_set_perm(uint64_t addr, uint64_t size, uint32_t perm) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for_range(addr, size, [&](Reg& r) { r.perm = perm; });
}

void mem_set_attr(uint64_t addr, uint64_t size, uint32_t mask, uint32_t attr) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for_range(addr, size, [&](Reg& r) { r.attr = (r.attr & ~mask) | (attr & mask); });
}

MemInfo mem_query(uint64_t addr) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    MemInfo mi{};
    if (addr < AS_BASE) {
        mi.addr = 0;
        mi.size = AS_BASE;
        mi.state = MS_INACCESSIBLE;
        return mi;
    }
    if (addr >= AS_END) {
        mi.addr = AS_END;
        mi.size = 0 - AS_END;
        mi.state = MS_INACCESSIBLE;
        return mi;
    }
    auto it = g_regs.upper_bound(addr);
    --it;
    mi.addr = it->first;
    mi.size = it->second.end - it->first;
    mi.state = it->second.state;
    mi.perm = it->second.perm;
    mi.attr = it->second.attr;
    return mi;
}

bool mem_check(uint64_t addr, uint64_t size, uint32_t state) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    if (addr < AS_BASE || addr + size > AS_END || addr + size < addr) return false;
    uint64_t a = addr, end = addr + size;
    while (a < end) {
        auto it = g_regs.upper_bound(a);
        --it;
        if (it->second.state != state) return false;
        a = it->second.end;
    }
    return true;
}

uint64_t mem_find_free(uint64_t base, uint64_t end, uint64_t size, uint64_t align) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for (auto it = g_regs.begin(); it != g_regs.end(); ++it) {
        if (it->second.state != MS_FREE) continue;
        uint64_t s = std::max(it->first, base), e = std::min(it->second.end, end);
        s = (s + align - 1) & ~(align - 1);
        if (s < e && e - s >= size) return s;
    }
    return 0;
}

// VirtualQuery coûte ≈ 25 µs ici (grand espace réservé) : cache par thread des dernières zones
// engagées trouvées, invalidé par g_mem_gen (l'engagement ne fait qu'ajouter des zones valides).
bool mem_valid(uint64_t addr, uint64_t size) {
    struct Zone { uint64_t gen = 0, s = 0, e = 0; };
    thread_local Zone cache[8];
    thread_local unsigned next = 0;
    uint64_t gen = g_mem_gen.load(std::memory_order_acquire), end = addr + size;
    for (auto& z : cache)
        if (z.gen == gen && addr >= z.s && end <= z.e) return true;
    uint64_t a = addr & ~0xfffull;
    uint64_t zs = 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) return false;
        const uint64_t fin_region = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_RESERVE) {
            // page d'une plage à la demande pas encore touchée : valide (elle se lira à zéro, engagée au premier accès)
            AcquireSRWLockShared(&g_lazy_lock);
            const auto [ls, le] = lazy_plage(a);
            ReleaseSRWLockShared(&g_lazy_lock);
            if (!le) return false;
            if (!zs) zs = std::max(ls, (uint64_t)mbi.BaseAddress);
            a = std::min(le, fin_region);
            continue;
        }
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (!zs) zs = (uint64_t)mbi.BaseAddress;
        a = fin_region;
    }
    cache[next++ % 8] = Zone{gen, zs, a};
    return true;
}

uint64_t mem_used() {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    uint64_t n = 0;
    for (auto& [s, r] : g_regs)
        if (r.state != MS_FREE && r.state != MS_INACCESSIBLE) n += r.end - s;
    return n;
}

}  // namespace hos
