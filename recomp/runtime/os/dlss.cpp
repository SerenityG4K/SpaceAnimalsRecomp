// DLSS de NVIDIA (2026-11-12) : DLAA et super résolution par NGX, la couche du pilote NVIDIA qui charge le DLSS
// (nvngx_dlss.dll) et l'exécute dans nos tampons de commandes Vulkan.
//
//  * cœur NGX : _nvngx.dll du dossier du pilote (registre : NGXCore), chargé dynamiquement ; ses fonctions
//    NVSDK_NGX_VULKAN_* sont appelées directement (pas de bibliothèque statique : le SDK n'en fournit que pour MSVC) ;
//  * nvngx_dlss.dll : cherché par NGX dans les dossiers donnés ici (RECOMP_DLSS_DIR, dossier de l'exécutable, son
//    sous-dossier dlss, puis les outils du paquet, <racine>\outils\dlss, et des sources, <racine>\recomp\outils\dlss),
//    avant son propre cache ; fourni dans le paquet depuis la 1.1.0 (launcher/outils.py), sous la licence de NVIDIA ;
//  * modèle : préréglage NGX K (DLSS 4, transformeur) ou M (DLSS 4.5, transformeur de 2e génération), recomp.ini dlss ;
//    RECOMP_DLSS_PRESET=<lettre> (essais) en impose un autre ;
//  * branchement : vk_render.cpp (dlss_taa) remplace la passe taa_resolve du moteur par une évaluation du DLSS.
// Le DLSS ne sert que sur une carte NVIDIA RTX (GeForce 20 et suivantes) ; partout ailleurs, ou si quelque chose
// manque, le jeu garde son propre anticrénelage temporel et le journal dit pourquoi (« dlss »).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "hos.h"   // avant <windows.h>, comme settings.cpp (GPTR)
#include "settings.h"

#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "dlss.h"
#include "fsr.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace hos {
namespace vkr {
bool carte_prevue(std::string* nom, uint32_t* fabricant);   // vk_render.cpp (vk_render.h tire les en-têtes NVN)
}
namespace dlss {
namespace {

// ---------------------------------------------------------------- interface C de NGX
// Disposition des structures et valeurs de l'interface NGX (SDK DLSS 310.9), redéclarées ici : le recomp n'inclut
// aucun fichier du SDK.
typedef unsigned Res;
bool reussi(Res r) { return (r & 0xFFF00000u) != 0xBAD00000u; }
constexpr int VERSION_API = 0x15;
constexpr int FONCTION_SUPER_RESOLUTION = 1;
constexpr int QUALITE_PERF = 0, QUALITE_EQUILIBRE = 1, QUALITE_QUALITE = 2, QUALITE_ULTRA_PERF = 3, QUALITE_DLAA = 5;
constexpr int DRAPEAU_MV_BASSE_RES = 1 << 1, DRAPEAU_PROFONDEUR_INVERSEE = 1 << 3;
struct ListeChemins {
    const wchar_t* const* chemins;
    unsigned n;
};
typedef void (*RappelJournal)(const char* message, int niveau, int fonction);
struct InfoJournal {
    RappelJournal rappel;
    int niveau;   // 0 coupé, 1 activé, 2 détaillé
    bool seul;    // rien d'autre que le rappel
};
struct InfoCommune {
    ListeChemins chemins;
    void* interne;
    InfoJournal journal;
};
struct Projet {
    const char* id;
    int moteur;   // 0 : moteur maison
    const char* version;
};
struct Identite {
    int type;   // 1 : identifiant de projet
    union {
        Projet projet;
        unsigned long long app;
    } v;
};
struct Decouverte {
    int version_api;
    int fonction;
    Identite ident;
    const wchar_t* donnees;
    const InfoCommune* info;
};
struct RessourceVk {
    union {
        struct {
            VkImageView vue;
            VkImage image;
            VkImageSubresourceRange plage;
            VkFormat format;
            unsigned w, h;
        } img;
        struct {
            VkBuffer tampon;
            unsigned taille;
        } tampon;
    } r;
    int type;   // 0 : vue d'image
    bool lecture_ecriture;
};
static_assert(sizeof(Identite) == 32 && sizeof(Decouverte) == 56 && sizeof(InfoCommune) == 40 &&
                  sizeof(RessourceVk) == 56,
              "disposition des structures NGX");
// Table de paramètres de NGX : objet C++ du pilote, appelé par sa table virtuelle (convention x64 de Windows, `this`
// en premier). Emplacements : Set(void*) 0, Set(int) 3, Set(unsigned) 4, Set(float) 6, Get(void**) 8, Get(int*) 11,
// Get(unsigned*) 12.
struct Parametres;
void** vt(Parametres* p) { return *(void***)p; }
void p_ptr(Parametres* p, const char* n, void* v) { ((void (*)(Parametres*, const char*, void*))vt(p)[0])(p, n, v); }
void p_i(Parametres* p, const char* n, int v) { ((void (*)(Parametres*, const char*, int))vt(p)[3])(p, n, v); }
void p_ui(Parametres* p, const char* n, unsigned v) { ((void (*)(Parametres*, const char*, unsigned))vt(p)[4])(p, n, v); }
void p_f(Parametres* p, const char* n, float v) { ((void (*)(Parametres*, const char*, float))vt(p)[6])(p, n, v); }
Res g_i(Parametres* p, const char* n, int* v) { return ((Res(*)(Parametres*, const char*, int*))vt(p)[11])(p, n, v); }
Res g_ui(Parametres* p, const char* n, unsigned* v) {
    return ((Res(*)(Parametres*, const char*, unsigned*))vt(p)[12])(p, n, v);
}

typedef Res (*F_Init)(const char*, int, const char*, const wchar_t*, VkInstance, VkPhysicalDevice, VkDevice,
                      PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int, const InfoCommune*);
typedef Res (*F_Params)(Parametres**);
typedef Res (*F_Creer)(VkDevice, VkCommandBuffer, int, Parametres*, void**);
typedef Res (*F_Evaluer)(VkCommandBuffer, void*, Parametres*, void*);
typedef Res (*F_Liberer)(void*);
typedef Res (*F_ExtInst)(const Decouverte*, uint32_t*, VkExtensionProperties**);
typedef Res (*F_ExtPer)(VkInstance, VkPhysicalDevice, const Decouverte*, uint32_t*, VkExtensionProperties**);

struct Etat {
    bool tente = false;      // cœur NGX cherché
    HMODULE coeur = nullptr;
    F_Init init = nullptr;
    F_Params capacites = nullptr, allouer = nullptr;
    F_Creer creer = nullptr;
    F_Evaluer evaluer = nullptr;
    F_Liberer liberer = nullptr;
    F_ExtInst ext_inst = nullptr;
    F_ExtPer ext_per = nullptr;
    bool extensions_ok = false;   // extensions de périphérique demandées par NGX toutes ajoutées
    bool pret = false;
    VkDevice dev = VK_NULL_HANDLE;
    Parametres* params = nullptr;   // table des évaluations (allouée une fois)
    void* fonction = nullptr;       // fonction DLSS créée
    // fonctions remplacées, à libérer quand le GPU a fini la soumission indiquée (voir menage) : jamais pendant que
    // des commandes déjà enregistrées s'en servent encore (Area 6, 2026-11-12 : deux vues de tailles différentes,
    // fonction recréée deux fois par image et libérée sous une évaluation pas encore exécutée → GPU perdu)
    std::vector<std::pair<uint64_t, void*>> a_liberer;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0, cree_rw = 0, cree_rh = 0;
    int qualite = -1;
    bool echec_creation = false;    // création refusée pour ces tailles (nouvel essai si elles changent)
    std::wstring dossier_exe, donnees;
    std::wstring chemins_s[5];
    const wchar_t* chemins[5] = {};
    unsigned nchemins = 0;
    InfoCommune info{};
    Decouverte decouverte{};
    char desc[128] = "off";
};
Etat g;
const char* const PROJET = "4959de5f-0d41-49b2-ac9d-49c3c65aa74f";   // identifiant de projet de SpaceAnimalsRecomp

void journal_ngx(const char* message, int niveau, int fonction) {
    std::string s = message ? message : "";
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    HLOG("dlss", "NGX [%d/%d] %s", niveau, fonction, s.c_str());
}

std::wstring registre(const wchar_t* cle, const wchar_t* valeur) {
    wchar_t b[1024];
    DWORD t = sizeof b;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, cle, valeur, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, b, &t) !=
        ERROR_SUCCESS)
        return L"";
    return b;
}
bool existe(const std::wstring& f) {
    const DWORD a = GetFileAttributesW(f.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// Cœur NGX du pilote et dossiers de nvngx_dlss.dll. Faux (et journal) si le DLSS ne peut pas servir.
bool charger_coeur() {
    if (g.tente) return g.coeur != nullptr;
    g.tente = true;
    std::wstring d = registre(L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath");
    if (d.empty()) d = registre(L"SYSTEM\\CurrentControlSet\\Services\\nvlddmkm\\Parameters\\NGXCore", L"NGXPath");
    if (d.empty()) d = registre(L"SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", L"FullPath");
    if (d.empty()) {
        HLOG("dlss", "NVIDIA NGX not installed (no NVIDIA driver with DLSS support): DLSS off, the game's anti-aliasing stays");
        return false;
    }
    // nvngx_dlss.dll : RECOMP_DLSS_DIR, dossier de l'exécutable, sous-dossier dlss, puis outils du paquet et des sources
    // (l'exécutable est dans <racine>\recomp\build\full, ou <racine>\build\full dans les sources)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g.dossier_exe = exe;
    g.dossier_exe = g.dossier_exe.substr(0, g.dossier_exe.find_last_of(L"\\/"));
    std::wstring racine = g.dossier_exe;
    for (int i = 0; i < 3 && racine.find_last_of(L"\\/") != std::wstring::npos; i++)
        racine = racine.substr(0, racine.find_last_of(L"\\/"));
    std::wstring candidats[5];
    int nc = 0;
    if (const char* e = getenv("RECOMP_DLSS_DIR")) {
        wchar_t w[MAX_PATH] = {};
        MultiByteToWideChar(CP_UTF8, 0, e, -1, w, MAX_PATH);
        candidats[nc++] = w;
    }
    candidats[nc++] = g.dossier_exe;
    candidats[nc++] = g.dossier_exe + L"\\dlss";
    candidats[nc++] = racine + L"\\outils\\dlss";
    candidats[nc++] = racine + L"\\recomp\\outils\\dlss";
    for (int i = 0; i < nc; i++)
        if (existe(candidats[i] + L"\\nvngx_dlss.dll")) g.chemins_s[g.nchemins++] = candidats[i];
    if (!g.nchemins) {
        HLOG("dlss", "nvngx_dlss.dll not found (next to recomp_main.exe, in its dlss folder, in outils\\dlss, or "
                     "RECOMP_DLSS_DIR): DLSS off, the game's anti-aliasing stays");
        return false;
    }
    for (unsigned i = 0; i < g.nchemins; i++) g.chemins[i] = g.chemins_s[i].c_str();
    const std::wstring dll = d + L"\\_nvngx.dll";
    g.coeur = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g.coeur) {
        HLOG("dlss", "NVIDIA NGX core not loaded (%s, error %lu): DLSS off", utf8(dll).c_str(), GetLastError());
        return false;
    }
#define NGXF(champ, T, nom) g.champ = (T)(void*)GetProcAddress(g.coeur, "NVSDK_NGX_VULKAN_" nom)
    NGXF(init, F_Init, "Init_ProjectID_Ext");
    NGXF(capacites, F_Params, "GetCapabilityParameters");
    NGXF(allouer, F_Params, "AllocateParameters");
    NGXF(creer, F_Creer, "CreateFeature1");
    NGXF(evaluer, F_Evaluer, "EvaluateFeature");
    NGXF(liberer, F_Liberer, "ReleaseFeature");
    NGXF(ext_inst, F_ExtInst, "GetFeatureInstanceExtensionRequirements");
    NGXF(ext_per, F_ExtPer, "GetFeatureDeviceExtensionRequirements");
#undef NGXF
    if (!g.init || !g.capacites || !g.allouer || !g.creer || !g.evaluer || !g.liberer || !g.ext_inst || !g.ext_per) {
        HLOG("dlss", "NVIDIA NGX core too old (missing Vulkan functions): DLSS off; update the NVIDIA driver");
        FreeLibrary(g.coeur);
        g.coeur = nullptr;
        return false;
    }
    g.donnees = g.dossier_exe + L"\\ngx";
    CreateDirectoryW(g.donnees.c_str(), nullptr);
    static const bool journal = getenv("RECOMP_DLSS_LOG") != nullptr;   // journal détaillé de NGX
    g.info.chemins = {g.chemins, g.nchemins};
    g.info.journal = {journal ? journal_ngx : nullptr, journal ? 1 : 0, false};
    g.decouverte.version_api = VERSION_API;
    g.decouverte.fonction = FONCTION_SUPER_RESOLUTION;
    g.decouverte.ident.type = 1;
    g.decouverte.ident.v.projet = {PROJET, 0, "1.0"};
    g.decouverte.donnees = g.donnees.c_str();
    g.decouverte.info = &g.info;
    HLOG("dlss", "NVIDIA NGX core: %s; nvngx_dlss.dll in %s", utf8(dll).c_str(), utf8(g.chemins_s[0]).c_str());
    return true;
}

const char* nom_modele(char m) {
    switch (m) {
    case 'K': return "DLSS 4 (transformer, model K)";
    case 'M': return "DLSS 4.5 (2nd-gen transformer, model M)";
    case 'L': return "DLSS 4.5 (2nd-gen transformer, model L)";
    case 'J': return "DLSS 4 (transformer, model J)";
    default: return "DLSS";
    }
}

// La carte que le jeu prendra (vk_render.cpp, carte_prevue) peut-elle faire le DLSS ? Une carte NVIDIA RTX : même règle
// que la page Graphismes du launcher, qui grise le DLSS sinon. Le launcher garde le mode DLSS de recomp.ini quand le
// joueur choisit une carte AMD ou Intel (réglage gpu), pour le retrouver s'il revient à sa carte NVIDIA : le mode est
// alors sans effet, et la 3D garde render_scale. Sans réponse de Vulkan : oui, l'initialisation tranchera
// (extensions_peripherique, puis NGX).
bool carte_dlss() {
    static const bool ok = [] {
        std::string nom;
        uint32_t fabricant = 0;
        if (!vkr::carte_prevue(&nom, &fabricant)) return true;
        std::string m = nom;
        for (char& c : m) c = (char)tolower((unsigned char)c);
        if (fabricant == 0x10de && m.find("rtx") != std::string::npos) return true;
        HLOG("dlss", "DLSS needs an NVIDIA RTX graphics card (the game will use %s): DLSS off, the game keeps its "
                     "anti-aliasing and its 3D rendering scale", nom.c_str());
        return false;
    }();
    return ok;
}

}  // namespace

char modele_demande() {
    static const char m = [] {
        if (!settings().dlss || !carte_dlss()) return (char)0;
        if (const char* e = getenv("RECOMP_DLSS_PRESET")) {   // essais : J à O
            const char c = (char)toupper((unsigned char)e[0]);
            if (c >= 'J' && c <= 'O') return c;
        }
        return settings().dlss_modele == 2 ? 'M' : 'K';
    }();
    return m;
}

bool dlss_prevu() {
    static const bool p = settings().dlss >= 1 && settings().dlss <= 5 && carte_dlss() && charger_coeur();
    return p;
}

const char* technique() { return !g.pret && fsr::pret() ? "FSR" : "DLSS"; }

double echelle_dlss() {
    static const double e = [] {
        static const double echelles[] = {0, 0, 2.0 / 3.0, 0.58, 0.5, 1.0 / 3.0};
        const int mode = settings().dlss;
        // une carte NVIDIA RTX (carte_dlss), le cœur NGX et nvngx_dlss.dll doivent être là : sinon le jeu garde
        // render_scale avec son anticrénelage (une carte RTX dont le pilote refuse le DLSS ne se voit qu'à
        // l'initialisation de NGX : rendu à cette échelle avec l'anticrénelage du jeu, que le journal signale)
        if (dlss_prevu()) return mode >= 2 && mode <= 5 ? echelles[mode] : 0.0;   // DLAA : rendu natif
        return fsr::echelle();   // FSR d'AMD s'il est demandé (fsr.cpp), sinon 0 : render_scale
    }();
    return e;
}

void extensions_instance(std::vector<const char*>& ext, const std::vector<VkExtensionProperties>& dispo) {
    if (!modele_demande() || !charger_coeur()) return;
    uint32_t n = 0;
    VkExtensionProperties* e = nullptr;
    const Res r = g.ext_inst(&g.decouverte, &n, &e);
    if (!reussi(r) || !e) return;   // aucune (0xBAD00012 : rien à demander sur ce pilote)
    for (uint32_t i = 0; i < n; i++) {
        bool offerte = false, deja = false;
        for (auto& x : dispo) offerte |= !strcmp(x.extensionName, e[i].extensionName);
        for (const char* x : ext) deja |= !strcmp(x, e[i].extensionName);
        if (offerte && !deja) ext.push_back(e[i].extensionName);
        if (!offerte) HLOG("dlss", "instance extension %s requested by NGX not available", e[i].extensionName);
    }
}

void extensions_peripherique(VkInstance inst, VkPhysicalDevice pd, const VkPhysicalDeviceProperties& pr,
                             const std::vector<VkExtensionProperties>& dispo, std::vector<const char*>& ext) {
    if (!modele_demande()) return;
    if (pr.vendorID != 0x10de) {
        HLOG("dlss", "DLSS needs an NVIDIA RTX graphics card (this one: %s): DLSS off, the game's anti-aliasing stays",
             pr.deviceName);
        return;
    }
    if (!charger_coeur()) return;
    uint32_t n = 0;
    VkExtensionProperties* e = nullptr;
    const Res r = g.ext_per(inst, pd, &g.decouverte, &n, &e);
    if (!reussi(r)) {
        HLOG("dlss", "NGX device extension query failed (0x%08x): DLSS off", r);
        return;
    }
    std::vector<const char*> ajout;
    for (uint32_t i = 0; i < n && e; i++) {
        bool offerte = false, deja = false;
        for (auto& x : dispo) offerte |= !strcmp(x.extensionName, e[i].extensionName);
        for (const char* x : ext) deja |= !strcmp(x, e[i].extensionName);
        if (!offerte) {
            HLOG("dlss", "device extension %s requested by NGX not available: DLSS off", e[i].extensionName);
            return;
        }
        if (!deja) ajout.push_back(e[i].extensionName);
    }
    ext.insert(ext.end(), ajout.begin(), ajout.end());
    g.extensions_ok = true;
}

bool init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
          PFN_vkGetDeviceProcAddr gdpa) {
    if (!modele_demande() || !g.extensions_ok) return false;
    const Res r = g.init(PROJET, 0, "1.0", g.donnees.c_str(), inst, pd, dev, gipa, gdpa, VERSION_API, &g.info);
    if (!reussi(r)) {
        HLOG("dlss", "NGX initialization failed (0x%08x): DLSS off", r);
        return false;
    }
    Parametres* cap = nullptr;
    if (!reussi(g.capacites(&cap)) || !cap) {
        HLOG("dlss", "NGX capabilities not available: DLSS off");
        return false;
    }
    int dispo = 0, maj = 0, resultat = 0;
    unsigned vmaj = 0, vmin = 0;
    g_i(cap, "SuperSampling.Available", &dispo);
    g_i(cap, "SuperSampling.NeedsUpdatedDriver", &maj);
    g_ui(cap, "SuperSampling.MinDriverVersionMajor", &vmaj);
    g_ui(cap, "SuperSampling.MinDriverVersionMinor", &vmin);
    g_i(cap, "SuperSampling.FeatureInitResult", &resultat);
    // table des capacités gardée (petite) : sa destruction pendant l'arrêt de NGX a fait planter un essai isolé
    if (!dispo) {
        if (maj) HLOG("dlss", "DLSS needs a newer NVIDIA driver (%u.%u at least): DLSS off", vmaj, vmin);
        else HLOG("dlss", "DLSS not available on this graphics card (result 0x%08x): DLSS off", (unsigned)resultat);
        return false;
    }
    if (!reussi(g.allouer(&g.params)) || !g.params) {
        HLOG("dlss", "NGX parameters not allocated: DLSS off");
        return false;
    }
    g.dev = dev;
    g.pret = true;
    snprintf(g.desc, sizeof g.desc, "%s", nom_modele(modele_demande()));
    HLOG("dlss", "%s ready: it replaces the game's temporal anti-aliasing (taa_resolve)", g.desc);
    return true;
}

bool pret() { return g.pret || fsr::pret(); }

void couper(const char* raison) {
    if (!g.pret) return fsr::couper(raison);
    g.pret = false;
    HLOG("dlss", "DLSS stopped for this session (%s): the game's anti-aliasing is back", raison);
}

const char* description() { return !g.pret && fsr::pret() ? fsr::description() : g.desc; }

bool fonction_prete(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    if (!g.pret && fsr::pret()) return fsr::fonction_prete(rw, rh, ow, oh);
    return g.fonction && g.cree_rw == rw && g.cree_rh == rh && g.ow == ow && g.oh == oh;
}

void menage(uint64_t fini) {
    fsr::menage(fini);
    for (size_t i = 0; i < g.a_liberer.size();) {
        if (g.a_liberer[i].first > fini) {
            i++;
            continue;
        }
        g.liberer(g.a_liberer[i].second);
        g.a_liberer.erase(g.a_liberer.begin() + (std::ptrdiff_t)i);
    }
}

bool evaluer(VkCommandBuffer cb, const Evaluation& e, uint64_t serie) {
    if (!g.pret) return fsr::evaluer(cb, e, serie);   // FSR d'AMD, s'il sert (sinon faux)
    const uint32_t ow = e.sortie.w, oh = e.sortie.h;
    if (e.rw < 32 || e.rh < 32 || ow < 32 || oh < 32) return false;   // taille minimale de NGX
    // qualité d'après le rapport des tailles : DLAA si elles sont égales
    const float rapport = (float)e.rw / (float)ow;
    const int qualite = e.rw == ow && e.rh == oh ? QUALITE_DLAA
                        : rapport >= 0.6f        ? QUALITE_QUALITE
                        : rapport >= 0.55f       ? QUALITE_EQUILIBRE
                        : rapport >= 0.45f       ? QUALITE_PERF
                                                 : QUALITE_ULTRA_PERF;
    // fonction DLSS : créée pour la taille rendue et la taille de sortie ; recréée si elles changent
    if (!g.fonction || g.cree_rw != e.rw || g.cree_rh != e.rh || g.ow != ow || g.oh != oh || g.qualite != qualite) {
        const bool memes = g.cree_rw == e.rw && g.cree_rh == e.rh && g.ow == ow && g.oh == oh && g.qualite == qualite;
        if (g.echec_creation && memes) return false;
        if (g.fonction) {   // peut encore servir dans des commandes pas encore exécutées : libérée plus tard
            g.a_liberer.emplace_back(serie, g.fonction);
            g.fonction = nullptr;
        }
        g.cree_rw = e.rw, g.cree_rh = e.rh, g.ow = ow, g.oh = oh, g.qualite = qualite;
        Parametres* p = g.params;
        p_ui(p, "CreationNodeMask", 1);
        p_ui(p, "VisibilityNodeMask", 1);
        p_ui(p, "Width", e.rw);
        p_ui(p, "Height", e.rh);
        p_ui(p, "OutWidth", ow);
        p_ui(p, "OutHeight", oh);
        p_i(p, "PerfQualityValue", qualite);
        p_i(p, "DLSS.Feature.Create.Flags", DRAPEAU_MV_BASSE_RES | DRAPEAU_PROFONDEUR_INVERSEE);
        p_i(p, "DLSS.Enable.Output.Subrects", 0);
        const unsigned preset = 10u + (unsigned)(modele_demande() - 'J');   // J = 10, K = 11, L = 12, M = 13
        for (const char* m : {"DLAA", "Quality", "Balanced", "Performance", "UltraPerformance", "UltraQuality"}) {
            const std::string k = std::string("DLSS.Hint.Render.Preset.") + m;
            p_ui(p, k.c_str(), preset);
        }
        p_i(p, "FreeMemOnReleaseFeature", 1);
        const Res r = g.creer(g.dev, cb, FONCTION_SUPER_RESOLUTION, p, &g.fonction);
        static const char* const noms_q[] = {"Performance", "Balanced", "Quality", "Ultra Performance", "Ultra Quality",
                                             "DLAA"};
        if (!reussi(r) || !g.fonction) {
            g.fonction = nullptr;
            g.echec_creation = true;
            HLOG("dlss", "DLSS creation refused for %ux%u -> %ux%u, %s (0x%08x): the game's anti-aliasing stays", e.rw,
                 e.rh, ow, oh, noms_q[qualite], r);
            return false;
        }
        g.echec_creation = false;
        snprintf(g.desc, sizeof g.desc, "%s, %s %ux%u -> %ux%u", nom_modele(modele_demande()), noms_q[qualite], e.rw,
                 e.rh, ow, oh);
        HLOG("dlss", "created: %s", g.desc);
    }
    // ressources : structures gardées le temps de l'appel (NGX les lit pendant l'évaluation)
    auto ressource = [](const Image& im, bool rw) {
        RessourceVk r{};
        r.r.img.vue = im.vue;
        r.r.img.image = im.image;
        r.r.img.plage = {im.aspect, 0, 1, 0, 1};
        r.r.img.format = im.format;
        r.r.img.w = im.w, r.r.img.h = im.h;
        r.type = 0;
        r.lecture_ecriture = rw;
        return r;
    };
    RessourceVk couleur = ressource(e.couleur, false), profondeur = ressource(e.profondeur, false),
                vecteurs = ressource(e.vecteurs, false), sortie = ressource(e.sortie, true);
    Parametres* p = g.params;
    p_ptr(p, "Color", &couleur);
    p_ptr(p, "Output", &sortie);
    p_ptr(p, "Depth", &profondeur);
    p_ptr(p, "MotionVectors", &vecteurs);
    p_ptr(p, "TransparencyMask", nullptr);
    p_ptr(p, "ExposureTexture", nullptr);
    p_f(p, "Jitter.Offset.X", e.gigue[0]);
    p_f(p, "Jitter.Offset.Y", e.gigue[1]);
    p_f(p, "Sharpness", 0.0f);
    p_i(p, "Reset", e.reset ? 1 : 0);
    p_f(p, "MV.Scale.X", e.echelle_mv[0]);
    p_f(p, "MV.Scale.Y", e.echelle_mv[1]);
    p_ui(p, "DLSS.Render.Subrect.Dimensions.Width", e.rw);
    p_ui(p, "DLSS.Render.Subrect.Dimensions.Height", e.rh);
    p_ui(p, "DLSS.Input.Color.Subrect.Base.X", 0);
    p_ui(p, "DLSS.Input.Color.Subrect.Base.Y", 0);
    p_ui(p, "DLSS.Input.Depth.Subrect.Base.X", 0);
    p_ui(p, "DLSS.Input.Depth.Subrect.Base.Y", 0);
    p_ui(p, "DLSS.Input.MV.Subrect.Base.X", 0);
    p_ui(p, "DLSS.Input.MV.Subrect.Base.Y", 0);
    p_ui(p, "DLSS.Output.Subrect.Base.X", 0);
    p_ui(p, "DLSS.Output.Subrect.Base.Y", 0);
    p_f(p, "DLSS.Pre.Exposure", 1.0f);
    p_f(p, "DLSS.Exposure.Scale", 1.0f);
    const Res r = g.evaluer(cb, g.fonction, p, nullptr);
    if (!reussi(r)) {
        static int nlog = 0;
        if (nlog++ < 8) HLOG("dlss", "DLSS evaluation failed (0x%08x)", r);
        return false;
    }
    return true;
}

}  // namespace dlss
}  // namespace hos
