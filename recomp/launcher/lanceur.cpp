// Launcher du portage natif de Star Fox (Switch 2) : fenêtre sans bordure, choix de la copie du joueur,
// construction du jeu sur son PC (preparer.py), réglages (recomp.ini), lancement du jeu.
//
// Les chemins se déduisent de l'emplacement de lanceur.exe : le dossier « recomp » (celui qui contient
// fullbuild.py) est cherché à côté, au-dessus, ou deux niveaux au-dessus. Le jeu construit est dans
// recomp/build/full, les extraits vidéo dans recomp/launcher/extraits, les réglages du launcher dans lanceur.ini.
#include "lanceur.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <timeapi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

// en-têtes Vulkan du moteur (runtime/third_party, construire.py) ; vulkan-1.dll est chargé à l'exécution
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

namespace lz {

App g;

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

// lanceur.log, à côté de lanceur.exe : ce que fait le launcher (démarrage, affichage, copie, construction, mise à
// jour, lancement et code de sortie du jeu, rapport de bug), horodaté. Au-delà de 1 Mo, il devient
// lanceur_precedent.log. Joint au rapport de bug (rapport.py). Appelé aussi par dessin.cpp (échecs d'affichage).
static std::mutex g_jl_lk;
void jlog(const wchar_t* fmt, ...) {
    wchar_t b[2048];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(b, 2047, fmt, a);
    va_end(a);
    b[2047] = 0;
    std::lock_guard<std::mutex> l(g_jl_lk);
    if (g.dossier_exe.empty()) return;
    const std::wstring p = g.dossier_exe + L"\\lanceur.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > (1u << 20))
        MoveFileExW(p.c_str(), (g.dossier_exe + L"\\lanceur_precedent.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    FILE* f = _wfopen(p.c_str(), L"ab");
    if (!f) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    // Chemin du profil Windows (le dossier personnel, qui porte le nom d'utilisateur) remplacé par %USERPROFILE% :
    // un lanceur.log joint à la main à une
    // issue publique ne donne plus le nom d'utilisateur (le jeu fait de même, host_main.cpp ; le rapport de bug
    // l'anonymisait déjà). LANCEUR_LOG_PATHS=1 : chemins laissés tels quels.
    std::wstring ligne = b;
    static const std::wstring profil = [] {
        const wchar_t* e = _wgetenv(L"LANCEUR_LOG_PATHS") ? nullptr : _wgetenv(L"USERPROFILE");
        return e && wcslen(e) > 5 ? std::wstring(e) : std::wstring();
    }();
    if (!profil.empty())
        for (size_t i = 0; i + profil.size() <= ligne.size();) {
            if (!_wcsnicmp(ligne.c_str() + i, profil.c_str(), profil.size())) {
                ligne.replace(i, profil.size(), L"%USERPROFILE%");
                i += 13;
            } else {
                i++;
            }
        }
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            utf8(ligne).c_str());
    fclose(f);
}

namespace {

constexpr UINT WM_JEU_FINI = WM_APP + 1, WM_CONS_FINIE = WM_APP + 2, WM_RAPPORT_FINI = WM_APP + 3, WM_REFAIRE = WM_APP + 4,
               WM_MAJ_DISPO = WM_APP + 5;
constexpr UINT_PTR MINUTEUR_MAJ = 1;

// Une seule instance par classe de fenêtre. Version d'essai (construire.py --classe) : une autre classe, pour tourner à
// côté du launcher du joueur sans le toucher.
#ifndef LANCEUR_CLASSE
#define LANCEUR_CLASSE L"LanceurStarFoxRecompile"
#endif
const wchar_t* CLASSE = LANCEUR_CLASSE;

bool existe(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring parent(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(0, k);
}
std::wstring absolu(const std::wstring& p) {
    wchar_t b[2048];
    DWORD n = GetFullPathNameW(p.c_str(), 2048, b, nullptr);
    return n && n < 2048 ? std::wstring(b) : p;
}
int compter(const std::wstring& motif) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(motif.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do n++;
    while (FindNextFileW(h, &fd));
    FindClose(h);
    return n;
}

// ---------------------------------------------------------------- chemins et état
void chemins() {
    wchar_t b[2048];
    GetModuleFileNameW(nullptr, b, 2048);
    g.dossier_exe = parent(b);
    for (const wchar_t* rel : {L"", L"\\recomp", L"\\..", L"\\..\\.."}) {
        const std::wstring c = absolu(g.dossier_exe + rel);
        if (existe(c + L"\\fullbuild.py")) {
            g.recomp = c;
            break;
        }
    }
    if (g.recomp.empty()) g.recomp = g.dossier_exe;
    // paquet installé (lanceur.exe à la racine, recomp/ à côté) : il se met à jour depuis les releases GitHub
    g.paquet = existe(g.dossier_exe + L"\\recomp\\fullbuild.py") && existe(g.dossier_exe + L"\\recomp\\launcher\\maj.py");
    g.sources = existe(g.recomp + L"\\launcher\\lanceur.cpp");   // dossier de développement : jamais déplacé
    g.version = VERSION;
    if (FILE* f = _wfopen((g.recomp + L"\\VERSION").c_str(), L"rb")) {
        char b[64] = {};
        if (fgets(b, sizeof b, f)) {
            std::string v = b;
            while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
            if (!v.empty()) g.version = wide(v);
        }
        fclose(f);
    }
    g.jeu = g.recomp + L"\\build\\full";
    g.extraits = g.recomp + L"\\launcher\\extraits";
    g.ini = g.dossier_exe + L"\\lanceur.ini";
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g.fils_max = std::max<int>(1, (int)si.dwNumberOfProcessors);
}

void detecter_jeu() {
    g.jeu_pret = existe(g.jeu + L"\\recomp_main.exe") && existe(g.jeu + L"\\manifest.txt") &&
                 compter(g.jeu + L"\\shader_cache\\*.spv") > 1000;
    // version des sources qui a construit le jeu (notée par preparer.py) : autre que l'actuelle après une mise à jour
    std::string v;
    if (FILE* f = _wfopen((g.jeu + L"\\.preparation\\version.ok").c_str(), L"rb")) {
        char b[64] = {};
        if (fgets(b, sizeof b, f)) v = b;
        fclose(f);
    }
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
    g.jeu_obsolete = g.jeu_pret && wide(v) != g.version;
}

// Icône : celle du launcher (ressource 1 de lanceur.exe, launcher/icone.ico), dans la fenêtre, la barre des tâches et
// le gestionnaire des tâches. Jusqu'au 2026-11-11, la fenêtre prenait l'icône du jeu (build/full/icone.ico) une fois
// le jeu construit : le launcher se confondait avec le jeu (demande de l'utilisateur). L'icône du jeu reste sur
// l'exécutable du jeu.

// vérification rapide de la copie : en-tête et index du conteneur .nup (la vérification complète, par
// empreintes, est faite par preparer.py)
bool verifier_copie(const std::wstring& chemin, std::wstring& msg) {
    msg.clear();
    HANDLE f = CreateFileW(chemin.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        msg = g.langue == Langue::Fr ? L"Copie introuvable : " + chemin : L"Copy not found: " + chemin;
        return false;
    }
    auto lire = [&](uint64_t off, void* d, DWORD n) {
        LARGE_INTEGER li;
        li.QuadPart = (LONGLONG)off;
        DWORD lu = 0;
        return SetFilePointerEx(f, li, nullptr, FILE_BEGIN) && ReadFile(f, d, n, &lu, nullptr) && lu == n;
    };
    bool ok = false;
    uint8_t h[0x80];
    if (lire(0, h, sizeof h) && !memcmp(h, "NUP\x01", 4)) {
        uint32_t toc_off, n_contenus;
        uint64_t toc_taille;
        memcpy(&toc_off, h + 0x08, 4);
        memcpy(&toc_taille, h + 0x10, 8);
        memcpy(&n_contenus, h + 0x48, 4);
        const uint32_t n = std::min<uint32_t>(n_contenus, 16);
        std::vector<uint8_t> toc(0x30 + n * 0x40);
        if (toc_taille >= toc.size() && lire(toc_off, toc.data(), (DWORD)toc.size()) && !memcmp(toc.data(), "TOC\x01", 4)) {
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t* c = toc.data() + 0x30 + i * 0x40;
                uint64_t titre;
                uint32_t version, type;
                memcpy(&titre, c, 8);
                memcpy(&version, c + 0x14, 4);
                memcpy(&type, c + 0x18, 4);
                if (type == 1 && (titre & ~0xFFFull) == 0x0400E88025B1A000ull && version == 131072) ok = true;
            }
        }
    }
    CloseHandle(f);
    if (!ok)
        msg = g.langue == Langue::Fr ? L"Copie non reconnue : il faut Star Fox avec la mise à jour 2.0.0 (v131072)."
                                     : L"Copy not recognized: Star Fox with update 2.0.0 (v131072) is required.";
    return ok;
}

// ---------------------------------------------------------------- lanceur.ini (UTF-16 pour les chemins accentués)
void ini_ecrire(const wchar_t* cle, const std::wstring& v) { WritePrivateProfileStringW(L"lanceur", cle, v.c_str(), g.ini.c_str()); }
std::wstring ini_lire(const wchar_t* cle, const wchar_t* def) {
    wchar_t b[2048];
    GetPrivateProfileStringW(L"lanceur", cle, def, b, 2048, g.ini.c_str());
    return b;
}

void config_lire() {
    if (!existe(g.ini)) {   // fichier Unicode : WritePrivateProfileStringW garde alors les accents
        HANDLE f = CreateFileW(g.ini.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            const wchar_t bom = 0xFEFF;
            DWORD n;
            WriteFile(f, &bom, 2, &n, nullptr);
            CloseHandle(f);
        }
    }
    const std::wstring l = ini_lire(L"langue", L"");
    if (l == L"fr") g.langue = Langue::Fr;
    else if (l == L"en") g.langue = Langue::En;
    else g.langue = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_FRENCH ? Langue::Fr : Langue::En;
    g.copie = ini_lire(L"copie", L"");
    // Dossier d'installation déplacé depuis le dernier démarrage (« Emplacement du jeu » de la page Réglages, ou à la
    // main) : une copie du jeu rangée dans l'ancien dossier a suivi, son chemin est refait
    const std::wstring ancien = ini_lire(L"dossier", L"");
    if (!ancien.empty() && _wcsicmp(ancien.c_str(), g.dossier_exe.c_str()) != 0) {
        jlog(L"install folder changed since the last start");
        if (g.copie.size() > ancien.size() + 1 && !_wcsnicmp(g.copie.c_str(), ancien.c_str(), ancien.size()) &&
            g.copie[ancien.size()] == L'\\' && !existe(g.copie) && existe(g.dossier_exe + g.copie.substr(ancien.size()))) {
            g.copie = g.dossier_exe + g.copie.substr(ancien.size());
            ini_ecrire(L"copie", g.copie);
        }
    }
    if (ancien != g.dossier_exe) ini_ecrire(L"dossier", g.dossier_exe);
    g.volume = std::clamp(_wtoi(ini_lire(L"volume", L"100").c_str()), 0, 100);
    g.fond_video = ini_lire(L"fond", L"extraits") != L"sobre";
    g.fils = std::clamp(_wtoi(ini_lire(L"fils", L"0").c_str()), 0, g.fils_max);
    g.maj_auto = ini_lire(L"maj_auto", L"1") != L"0";
}

void config_ecrire() {
    ini_ecrire(L"langue", g.langue == Langue::Fr ? L"fr" : L"en");
    ini_ecrire(L"copie", g.copie);
    ini_ecrire(L"volume", std::to_wstring(g.volume));
    ini_ecrire(L"fond", g.fond_video ? L"extraits" : L"sobre");
    ini_ecrire(L"fils", std::to_wstring(g.fils));
    ini_ecrire(L"maj_auto", g.maj_auto ? L"1" : L"0");
}

void copie_trouver() {
    if (!g.copie.empty() && existe(g.copie)) {
        g.copie_ok = verifier_copie(g.copie, g.copie_message);
        return;
    }
    // sinon : une copie posée dans le dossier du projet, ou à côté du launcher
    for (const std::wstring& d : {parent(g.recomp), g.dossier_exe}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*.nup").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        FindClose(h);
        g.copie = d + L"\\" + fd.cFileName;
        g.copie_ok = verifier_copie(g.copie, g.copie_message);
        if (g.copie_ok) config_ecrire();
        return;
    }
    g.copie.clear();
    g.copie_ok = false;
}

// ---------------------------------------------------------------- cartes graphiques
// Choix de la carte du jeu dans la page Graphismes (2026-11-12, demande de l'utilisateur : « certains vont lancer avec
// une carte AMD […] si ce n'est pas un NVIDIA alors on ne peut pas choisir le DLSS »). Les cartes sont listées par
// Vulkan, comme le jeu les voit (mêmes noms), et le choix automatique est le sien (vk_render.cpp, choisir_carte) : les
// garder pareils.
std::string minuscules(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// Dans un fil au démarrage : créer une instance Vulkan charge les pilotes des cartes (jusqu'à quelques centaines de ms).
// vulkan-1.dll reste chargé (un pilote peut garder un fil à lui) ; rien ne sert d'autre que la liste.
void cartes_lister() {
    std::vector<Carte> l;
    int ecartees = 0;   // cartes pas proposées (voir plus bas)
    const ULONGLONG t0 = GetTickCount64();
    if (HMODULE lib = LoadLibraryW(L"vulkan-1.dll")) {
        const auto gipa = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(lib, "vkGetInstanceProcAddr");
        const auto creer = gipa ? (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance") : nullptr;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "lanceur";
        app.apiVersion = VK_API_VERSION_1_3;   // comme le jeu
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (creer && creer(&ici, nullptr, &inst) == VK_SUCCESS) {
            const auto detruire = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
            const auto enumerer = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
            const auto proprietes = (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
            const auto memoire = (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
            if (enumerer && proprietes && memoire) {
                uint32_t n = 0;
                enumerer(inst, &n, nullptr);
                std::vector<VkPhysicalDevice> pds(n);
                if (n) enumerer(inst, &n, pds.data());
                for (uint32_t i = 0; i < n; i++) {
                    VkPhysicalDeviceProperties pr;
                    proprietes(pds[i], &pr);
                    // Pas proposées, comme le jeu ne les prend pas de lui-même (vk_render.cpp, carte_utilisable) : une
                    // carte sans Vulkan 1.3, et la couche « Vulkan sur Direct3D 12 » de Microsoft, qui double chaque
                    // carte sous le nom « Microsoft Direct3D12 (…) » (issue GitHub n° 18)
                    if (pr.apiVersion < VK_API_VERSION_1_3 || !strncmp(pr.deviceName, "Microsoft Direct3D12", 20)) {
                        ecartees++;
                        continue;
                    }
                    VkPhysicalDeviceMemoryProperties mp;
                    memoire(pds[i], &mp);
                    Carte c;
                    c.nom = pr.deviceName;
                    c.fabricant = pr.vendorID;
                    c.dediee = pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
                    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
                        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                            c.memoire = std::max<uint64_t>(c.memoire, mp.memoryHeaps[h].size);
                    // deux cartes du même nom : une seule ligne (le jeu prendrait la première, qui a ce nom)
                    bool deja = false;
                    for (const Carte& x : l) deja |= x.nom == c.nom;
                    if (!deja) l.push_back(c);
                }
            }
            if (detruire) detruire(inst, nullptr);
        }
    }
    std::wstring s;
    for (const Carte& c : l) {
        wchar_t b[320];
        swprintf(b, 320, L"%ls%ls (%ls, %.1f GB)", s.empty() ? L"" : L"; ", wide(c.nom).c_str(),
                 c.dediee ? L"discrete" : L"integrated or other", c.memoire / 1073741824.0);
        s += b;
    }
    jlog(L"graphics cards (Vulkan, %llu ms): %ls", (unsigned long long)(GetTickCount64() - t0),
         l.empty() ? L"none" : s.c_str());
    if (ecartees) jlog(L"graphics cards set aside (Vulkan below 1.3, or the Direct3D 12 layer): %d", ecartees);
    g.cartes = std::move(l);
    g.cartes_pretes.store(true, std::memory_order_release);
}

}  // namespace

// déclarées dans lanceur.h (page Graphismes, dessin.cpp)
int carte_du_jeu(bool* nommee) {
    if (nommee) *nommee = false;
    if (!g.cartes_pretes.load(std::memory_order_acquire)) return -1;
    const std::string voulu = minuscules(g.reglages.gpu);
    if (voulu != "auto" && !voulu.empty())
        for (size_t i = 0; i < g.cartes.size(); i++)
            if (minuscules(g.cartes[i].nom).find(voulu) != std::string::npos) {
                if (nommee) *nommee = true;
                return (int)i;
            }
    // automatique (ou carte absente : changée depuis) : la carte dédiée qui a le plus de mémoire, à défaut la carte qui en
    // a le plus ; à égalité, la première
    int choix = -1;
    for (size_t i = 0; i < g.cartes.size(); i++) {
        const Carte& c = g.cartes[i];
        if (choix < 0 || (c.dediee != g.cartes[choix].dediee ? c.dediee : c.memoire > g.cartes[choix].memoire))
            choix = (int)i;
    }
    return choix;
}

bool dlss_possible() {
    const int i = carte_du_jeu();
    if (i < 0) return true;
    return g.cartes[i].fabricant == 0x10DE && minuscules(g.cartes[i].nom).find("rtx") != std::string::npos;
}

std::wstring nom_carte(const std::string& nom) {
    std::string s = nom;
    for (const char* x : {"(TM)", "(tm)", "(R)", "(r)"})
        for (size_t p; (p = s.find(x)) != std::string::npos;) s.erase(p, strlen(x));
    if (s.rfind("NVIDIA GeForce ", 0) == 0) s.erase(0, 7);   // « GeForce » suffit à dire NVIDIA
    for (size_t p; (p = s.find("  ")) != std::string::npos;) s.erase(p, 1);
    return wide(s);
}

namespace {

// ---------------------------------------------------------------- recomp.ini du jeu
std::wstring recomp_ini() { return g.jeu + L"\\recomp.ini"; }

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> lignes_fichier(const std::wstring& p) {
    std::vector<std::string> v;
    FILE* f = _wfopen(p.c_str(), L"rb");
    if (!f) return v;
    std::string tout;
    char b[4096];
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) tout.append(b, n);
    fclose(f);
    size_t i = 0;
    while (i <= tout.size()) {
        size_t j = tout.find('\n', i);
        if (j == std::string::npos) j = tout.size();
        std::string l = tout.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        v.push_back(l);
        i = j + 1;
    }
    while (!v.empty() && v.back().empty()) v.pop_back();
    return v;
}

// ---------------------------------------------------------------- touches du clavier (page Contrôles)
// Une touche par action de la manette, par scancode : la position physique de la touche, que le jeu lit telle quelle
// (runtime/os/input.cpp). Écrites dans recomp.ini (« key_a = 0x2E »…) dès que le joueur en change une.
const char* const TOUCHES_CLES[NB_TOUCHES] = {
    "key_a", "key_b", "key_x", "key_y", "key_l", "key_r", "key_zl", "key_zr", "key_plus", "key_minus", "key_lstick", "key_rstick",
    "key_up", "key_down", "key_left", "key_right", "key_ls_up", "key_ls_down", "key_ls_left", "key_ls_right",
    "key_rs_up", "key_rs_down", "key_rs_left", "key_rs_right"};
// ---------------------------------------------------------------- boutons de la manette (page Contrôles, onglet Manette)
// Une entrée de la manette par bouton de la Switch (« pad_a = east »…), écrites dans recomp.ini dès que le joueur en
// change une. Noms des entrées : mêmes que PAD_ENTREES du jeu (runtime/os/settings.cpp), ordre de EntreePad.
const char* const PAD_CLES[NB_PAD] = {"pad_a", "pad_b", "pad_x", "pad_y", "pad_l", "pad_r", "pad_zl", "pad_zr", "pad_plus",
                                      "pad_minus", "pad_lstick", "pad_rstick", "pad_up", "pad_down", "pad_left", "pad_right"};
const char* const ENTREES_PAD[P_NB] = {
    "none", "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "right_paddle1", "left_paddle1",
    "right_paddle2", "left_paddle2", "touchpad", "lefttrigger", "righttrigger"};

// Deux préréglages, définis par la lettre gravée sur la touche et résolus sur le clavier réglé dans Windows :
//   WASD (QWERTY) : stick gauche W A S D, Y sur Z, L sur Q, + sur M ;
//   ZQSD (AZERTY) : stick gauche Z Q S D, Y sur W, L sur A, + sur la virgule.
// Le reste est commun (A : C, B : X, X : V, R : E, ZL : R, ZR : T, - : N, clics F et H, stick droit I J K L, croix :
// flèches). Sur un clavier QWERTY le premier, sur un clavier AZERTY le second donnent exactement les positions par
// défaut du jeu : c'est le préréglage affiché tant que le joueur n'a rien changé.
struct Lettre { UINT vk; unsigned fixe; };
const Lettre PRESETS[2][NB_TOUCHES] = {
    {{'C', 0}, {'X', 0}, {'V', 0}, {'Z', 0}, {'Q', 0}, {'E', 0}, {'R', 0}, {'T', 0}, {'M', 0}, {'N', 0}, {'F', 0}, {'H', 0},
     {0, 0xE048}, {0, 0xE050}, {0, 0xE04B}, {0, 0xE04D}, {'W', 0}, {'S', 0}, {'A', 0}, {'D', 0}, {'I', 0}, {'K', 0}, {'J', 0}, {'L', 0}},
    {{'C', 0}, {'X', 0}, {'V', 0}, {'W', 0}, {'A', 0}, {'E', 0}, {'R', 0}, {'T', 0}, {VK_OEM_COMMA, 0}, {'N', 0}, {'F', 0}, {'H', 0},
     {0, 0xE048}, {0, 0xE050}, {0, 0xE04B}, {0, 0xE04D}, {'Z', 0}, {'S', 0}, {'Q', 0}, {'D', 0}, {'I', 0}, {'K', 0}, {'J', 0}, {'L', 0}}};
void touches_prereglage(int p, unsigned* t) {
    const ReglagesJeu defaut;
    for (int i = 0; i < NB_TOUCHES; i++) {
        const Lettre& l = PRESETS[p][i];
        const unsigned sc = l.fixe ? l.fixe : MapVirtualKeyW(l.vk, MAPVK_VK_TO_VSC);
        t[i] = sc ? sc : defaut.touches[i];   // lettre absente de ce clavier : position par défaut
    }
}
int prereglage_courant() {
    for (int p = 0; p < 2; p++) {
        unsigned t[NB_TOUCHES];
        touches_prereglage(p, t);
        if (!memcmp(t, g.reglages.touches, sizeof t)) return p;
    }
    return -1;
}
void reglages_ecrire();
// Donne la touche `sc` à l'action ; si une autre action l'avait, elle reçoit l'ancienne touche de celle-ci (échange).
void touche_affecter(int action, unsigned sc) {
    unsigned* t = g.reglages.touches;
    for (int i = 0; i < NB_TOUCHES; i++)
        if (i != action && t[i] == sc) t[i] = t[action];
    t[action] = sc;
    g.reglages.touches_ecrites = true;
    reglages_ecrire();
    jlog(L"controls: %hs = 0x%X", TOUCHES_CLES[action], sc);
}
void touches_choisir_prereglage(int p) {
    touches_prereglage(p, g.reglages.touches);
    g.reglages.touches_ecrites = true;
    reglages_ecrire();
    jlog(L"controls: preset %ls", p ? L"ZQSD (AZERTY)" : L"WASD (QWERTY)");
}
// Donne l'entrée de la manette au bouton de la Switch `action` ; si un autre bouton l'avait, il reçoit l'ancienne entrée
// de celui-ci (échange, comme pour les touches). « Aucun » n'est jamais échangé.
void pad_affecter(int action, int entree) {
    uint8_t* m = g.reglages.manette;
    if (entree != P_AUCUNE)
        for (int i = 0; i < NB_PAD; i++)
            if (i != action && m[i] == entree) m[i] = m[action];
    m[action] = (uint8_t)entree;
    g.reglages.manette_ecrite = true;
    reglages_ecrire();
    jlog(L"controls: %hs = %hs", PAD_CLES[action], ENTREES_PAD[entree]);
}
void pad_par_defaut() {
    const ReglagesJeu defaut;
    memcpy(g.reglages.manette, defaut.manette, sizeof defaut.manette);
    g.reglages.manette_ecrite = true;
    reglages_ecrire();
    jlog(L"controls: controller buttons back to the default layout");
}
// Onglet Manette en attente d'un appui : la première entrée appuyée qui ne l'était pas au début de l'attente est donnée
// au bouton choisi (une entrée déjà tenue ne compte qu'après avoir été relâchée)
void pad_suivre_capture() {
    if (g.capture_pad < 0) return;
    const uint32_t a = manettes().appuyees, nouvelles = a & ~g.pad_tenues;
    g.pad_tenues &= a;
    if (!nouvelles) return;
    int e = 0;
    while (!(nouvelles >> e & 1)) e++;
    const int action = g.capture_pad;
    g.capture_pad = -1;
    pad_affecter(action, e);
}
// Onglet Manette, clic sur une entrée du dessin. Pendant une attente, elle est donnée au bouton qui attend, comme un
// appui sur la manette : on assigne ainsi en cliquant, sans manette branchée. Sinon, le bouton de la Switch qu'elle fait
// se met à attendre (une entrée qui ne fait rien n'est cliquable que pendant une attente : dessin.cpp, zone_bouton).
void pad_entree_cliquee(int e, int attente) {
    if (attente >= 0) {
        if (g.reglages.manette[attente] != e) pad_affecter(attente, e);   // la sienne : l'attente est seulement annulée
        return;
    }
    for (int i = 0; i < NB_PAD; i++)
        if (g.reglages.manette[i] == e) {
            g.capture_pad = i;
            g.pad_tenues = manettes().appuyees;
            return;
        }
}

void reglages_lire() {
    bool echelle_lue = false;
    for (const std::string& l0 : lignes_fichier(recomp_ini())) {
        const std::string l = trim(l0);
        if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (k == "refresh_rate") g.reglages.hz = std::clamp(atoi(v.c_str()), 30, 240);
        else if (k == "resolution") g.reglages.resolution = v == "auto" ? 0 : atoi(v.c_str());
        else if (k == "native_resolution") {   // ancien « Comme la console » : 67 % (si render_scale manque)
            if ((v == "false" || v == "0" || v == "non" || v == "off") && !echelle_lue) g.reglages.echelle = 67;
        } else if (k == "render_scale") {
            g.reglages.echelle = std::clamp(atoi(v.c_str()), 67, 100);
            echelle_lue = true;
        }
        else if (k == "dlss") {
            g.reglages.dlss = 0;
            for (int i = 1; i < 6; i++)
                if (v == DLSS_MODES[i]) g.reglages.dlss = i;
        } else if (k == "dlss_model") g.reglages.dlss_modele = v == "dlss45" ? 2 : 1;
        else if (k == "fsr") {
            g.reglages.fsr = 0;
            for (int i = 1; i < 6; i++)
                if (v == FSR_MODES[i]) g.reglages.fsr = i;
        } else if (k == "fsr_version") g.reglages.fsr_version = v == "fsr4" ? 4 : v == "fsr3" ? 3 : 0;
        else if (k == "gpu") g.reglages.gpu = v.empty() || minuscules(v) == "auto" ? std::string("auto") : v;
        else if (k == "window_mode") g.reglages.sans_bordure = !(v == "windowed" || v == "window" || v == "fenetre");
        else if (k == "language") g.reglages.langue = v.empty() ? "auto" : v;
        else if (k == "vibration") g.reglages.vibration = std::clamp(atoi(v.c_str()), 0, 100);
        else if (k.rfind("pad_", 0) == 0) {
            for (int i = 0; i < NB_PAD; i++)
                if (k == PAD_CLES[i])
                    for (int e = 0; e < P_NB; e++)
                        if (v == ENTREES_PAD[e]) {
                            g.reglages.manette[i] = (uint8_t)e;
                            g.reglages.manette_ecrite = true;
                        }
        } else if (k.rfind("key_", 0) == 0) {
            for (int i = 0; i < NB_TOUCHES; i++)
                if (k == TOUCHES_CLES[i]) {
                    const unsigned long sc = strtoul(v.c_str(), nullptr, 0);
                    if (sc <= 0xFF || (sc >= 0xE000 && sc <= 0xE0FF)) g.reglages.touches[i] = (unsigned)sc;
                    g.reglages.touches_ecrites = true;
                }
        }
    }
}

void reglages_ecrire() {
    std::vector<std::string> v = lignes_fichier(recomp_ini());
    if (v.empty())
        v = {"; Réglages de recomp_main.exe, écrits par le launcher (ils peuvent aussi être modifiés à la main).", "",
             "[display]"};
    std::vector<std::pair<std::string, std::string>> cles = {
        {"refresh_rate", std::to_string(g.reglages.hz)},
        {"resolution", g.reglages.resolution ? std::to_string(g.reglages.resolution) : "auto"},
        {"native_resolution", "true"},   // jeux construits avant render_scale : rendu natif
        {"render_scale", std::to_string(g.reglages.echelle)},
        {"dlss", DLSS_MODES[std::clamp(g.reglages.dlss, 0, 5)]},
        {"dlss_model", g.reglages.dlss_modele == 2 ? "dlss45" : "dlss4"},
        {"fsr", FSR_MODES[std::clamp(g.reglages.fsr, 0, 5)]},
        {"fsr_version", g.reglages.fsr_version == 4 ? "fsr4" : g.reglages.fsr_version == 3 ? "fsr3" : "auto"},
        {"gpu", g.reglages.gpu},
        {"window_mode", g.reglages.sans_bordure ? "borderless" : "windowed"},
        {"language", g.reglages.langue},
        {"vibration", std::to_string(g.reglages.vibration)},
    };
    if (g.reglages.touches_ecrites)   // touches du clavier : seulement une fois que le joueur en a changé une
        for (int i = 0; i < NB_TOUCHES; i++) {
            char b[16];
            snprintf(b, sizeof b, "0x%X", g.reglages.touches[i]);
            cles.push_back({TOUCHES_CLES[i], b});
        }
    if (g.reglages.manette_ecrite)   // boutons de la manette : seulement une fois que le joueur en a changé un
        for (int i = 0; i < NB_PAD; i++) cles.push_back({PAD_CLES[i], ENTREES_PAD[std::min<int>(g.reglages.manette[i], P_NB - 1)]});
    bool section_clavier = false, section_manette = false;
    for (const std::string& l : v) {
        section_clavier = section_clavier || trim(l) == "[keyboard]";
        section_manette = section_manette || trim(l) == "[controller]";
    }
    for (auto& [k, val] : cles) {
        bool fait = false;
        for (std::string& l : v) {
            const std::string t = trim(l);
            if (t.rfind(k, 0) == 0 && t.size() > k.size() && (t[k.size()] == ' ' || t[k.size()] == '=')) {
                l = k + " = " + val;
                fait = true;
            }
        }
        if (!fait && k.rfind("key_", 0) == 0 && !section_clavier) {
            section_clavier = true;
            v.push_back("");
            v.push_back("[keyboard]");
            v.push_back("; Touches du clavier, une par bouton de la manette : réglées depuis la page Contrôles du launcher.");
            v.push_back("; Valeur : position de la touche (scancode ; 0xE0.. : touche étendue ; 0 : aucune touche).");
            v.push_back("; Keyboard keys, one per controller button: set from the launcher's Controls page.");
            v.push_back("; Value: position of the key (scancode; 0xE0..: extended key; 0: no key).");
        }
        if (!fait && k.rfind("pad_", 0) == 0 && !section_manette) {
            section_manette = true;
            v.push_back("");
            v.push_back("[controller]");
            v.push_back("; Boutons de la manette, un par bouton de la Switch : réglés depuis la page Contrôles du launcher (onglet");
            v.push_back("; Manette). Valeur : entrée repérée par sa position (south : bouton du bas, A d'une manette Xbox, ✕ d'une");
            v.push_back("; PlayStation, B d'une Nintendo ; east, west, north, leftshoulder, lefttrigger, start, back, dpad_up… ; none).");
            v.push_back("; Controller buttons, one per Switch button: set from the launcher's Controls page (Controller tab).");
            v.push_back("; Value: input by position (south: bottom button, A on Xbox, ✕ on PlayStation, B on Nintendo; east, west,");
            v.push_back("; north, leftshoulder, lefttrigger, start, back, dpad_up…; none).");
        }
        if (!fait && k == "fsr") {   // apparu après la 1.0.3 : expliqué (comme le jeu le fait, settings.cpp kCleFsr)
            const std::vector<std::string> bloc = {
                "",
                "; FSR d'AMD à la place de l'anticrénelage du jeu, sur toute carte graphique : off, native, quality (rendu 3D",
                "; à 67 %), balanced (59 %), performance (50 %) ou ultra_performance (33 %). fsr_version : auto (FSR 4 sur les",
                "; cartes AMD qui le proposent, sinon FSR 3.1), fsr3 ou fsr4. Le DLSS passe devant sur une carte NVIDIA RTX.",
                "; AMD FSR instead of the game's anti-aliasing, on any graphics card: off, native, quality (3D rendering at",
                "; 67%), balanced (59%), performance (50%) or ultra_performance (33%). fsr_version: auto (FSR 4 on the AMD",
                "; cards that offer it, FSR 3.1 otherwise), fsr3 or fsr4. DLSS takes priority on an NVIDIA RTX card.",
                k + " = " + val};
            auto pos = std::find_if(v.begin(), v.end(), [](const std::string& l) { return trim(l) == "[keyboard]"; });
            if (pos != v.end()) {   // avant la ligne vide qui précède la section
                while (pos != v.begin() && trim(*(pos - 1)).empty()) --pos;
                v.insert(pos, bloc.begin(), bloc.end());
            } else {
                v.insert(v.end(), bloc.begin(), bloc.end());
            }
            continue;
        }
        if (!fait && k == "fsr_version") {   // juste après fsr, qui vient d'être écrit
            auto pos = std::find_if(v.begin(), v.end(), [](const std::string& l) { return trim(l).rfind("fsr =", 0) == 0; });
            v.insert(pos != v.end() ? pos + 1 : v.end(), k + " = " + val);
            continue;
        }
        if (!fait && k == "gpu") {   // apparu après la 1.0.3 : expliqué (comme le jeu le fait, settings.cpp kCleGpu),
            // avant la section des touches s'il y en a une
            const std::vector<std::string> bloc = {
                "",
                "; Carte graphique : auto (la carte dédiée qui a le plus de mémoire vidéo) ou le nom de la carte, choisie dans",
                "; la page Graphismes du launcher. Le DLSS ne sert que sur une carte NVIDIA RTX.",
                "; Graphics card: auto (the dedicated card with the most video memory) or the card's name, chosen in the",
                "; launcher's Graphics page. DLSS only works on an NVIDIA RTX card.",
                k + " = " + val};
            auto pos = std::find_if(v.begin(), v.end(), [](const std::string& l) { return trim(l) == "[keyboard]"; });
            if (pos != v.end()) {   // avant la ligne vide qui précède la section
                while (pos != v.begin() && trim(*(pos - 1)).empty()) --pos;
                v.insert(pos, bloc.begin(), bloc.end());
            } else {
                v.insert(v.end(), bloc.begin(), bloc.end());
            }
            continue;
        }
        if (!fait) v.push_back(k + " = " + val);
    }
    CreateDirectoryW(g.jeu.c_str(), nullptr);
    FILE* f = _wfopen(recomp_ini().c_str(), L"wb");
    if (!f) return;
    for (const std::string& l : v) fprintf(f, "%s\r\n", l.c_str());
    fclose(f);
}

// ---------------------------------------------------------------- choix de la copie
void choisir_copie() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC filtre[] = {{T("filtre_nup"), L"*.nup"}};
    d->SetFileTypes(1, filtre);
    d->SetTitle(T("dialogue_titre"));
    if (!g.copie.empty()) {
        IShellItem* dossier = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(parent(g.copie).c_str(), nullptr, IID_PPV_ARGS(&dossier)))) {
            d->SetFolder(dossier);
            dossier->Release();
        }
    }
    if (SUCCEEDED(d->Show(g.hwnd))) {
        IShellItem* r = nullptr;
        if (SUCCEEDED(d->GetResult(&r))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                g.copie = p;
                CoTaskMemFree(p);
                g.copie_ok = verifier_copie(g.copie, g.copie_message);
                config_ecrire();
            }
            r->Release();
        }
    }
    d->Release();
}

// ---------------------------------------------------------------- construction (preparer.py)
std::wstring trouver_python(bool& lanceur_py) {
    lanceur_py = false;
    for (const std::wstring& base : {parent(g.recomp) + L"\\outils", g.recomp + L"\\outils"}) {   // paquet, puis développement
        const std::wstring fourni = base + L"\\python\\python.exe";
        if (existe(fourni)) return fourni;
    }
    wchar_t b[2048];
    // Python du PATH, sauf l'alias du Microsoft Store (il ouvre le Store au lieu de lancer Python)
    if (SearchPathW(nullptr, L"python.exe", nullptr, 2048, b, nullptr)) {
        std::wstring p = b;
        if (p.find(L"\\WindowsApps\\") == std::wstring::npos) return p;
    }
    if (SearchPathW(nullptr, L"py.exe", nullptr, 2048, b, nullptr)) {
        lanceur_py = true;
        return b;
    }
    return {};
}

FILE* g_journal = nullptr;

void traiter_ligne(const std::string& s) {
    if (g_journal) fprintf(g_journal, "%s\n", s.c_str()), fflush(g_journal);
    std::lock_guard<std::mutex> l(g.cons.lk);
    Construction& c = g.cons;
    auto indice = [&](const std::string& id) {
        for (size_t i = 0; i < c.etapes.size(); i++)
            if (c.etapes[i] == id) return (int)i;
        return -1;
    };
    if (s.rfind("@etapes ", 0) == 0) {
        c.etapes.clear();
        std::string r = s.substr(8);
        size_t i = 0;
        while (i <= r.size()) {
            size_t j = r.find(',', i);
            if (j == std::string::npos) j = r.size();
            if (j > i) c.etapes.push_back(trim(r.substr(i, j - i)));
            i = j + 1;
        }
        c.statut.assign(c.etapes.size(), 0);
    } else if (s.rfind("@etape ", 0) == 0) {
        const int i = indice(trim(s.substr(7)));
        if (i >= 0) c.courante = i, c.statut[i] = 1, c.progres = 0, c.info.clear();
    } else if (s.rfind("@progres ", 0) == 0) {
        c.progres = atof(s.c_str() + 9);
    } else if (s.rfind("@info ", 0) == 0) {
        c.info = wide(s.substr(6));
    } else if (s.rfind("@ok ", 0) == 0 || s.rfind("@saute ", 0) == 0) {
        const bool saute = s[1] == 's';
        const int i = indice(trim(s.substr(saute ? 7 : 4)));
        if (i >= 0) c.statut[i] = saute ? 3 : 2;
    } else if (s.rfind("@erreur ", 0) == 0) {
        c.echec = true;
        c.erreur = wide(s.substr(8));
    } else if (s.rfind("@fini", 0) == 0) {
    } else {
        c.journal.push_back(wide(s));
        if (c.journal.size() > 500) c.journal.erase(c.journal.begin(), c.journal.begin() + 100);
    }
}

void lecteur(HANDLE lecture, HANDLE proc, HANDLE job) {
    std::string tampon;
    char b[8192];
    DWORD n = 0;
    while (ReadFile(lecture, b, sizeof b, &n, nullptr) && n) {
        tampon.append(b, n);
        size_t k;
        while ((k = tampon.find('\n')) != std::string::npos) {
            std::string l = tampon.substr(0, k);
            if (!l.empty() && l.back() == '\r') l.pop_back();
            tampon.erase(0, k + 1);
            if (!l.empty()) traiter_ligne(l);
        }
    }
    if (!tampon.empty()) traiter_ligne(tampon);
    WaitForSingleObject(proc, 10000);
    DWORD code = 1;
    GetExitCodeProcess(proc, &code);
    CloseHandle(lecture);
    {
        std::lock_guard<std::mutex> l(g.cons.lk);
        Construction& c = g.cons;
        c.active = false;
        c.finie = true;
        c.fin = GetTickCount64();
        if (!c.annulee && code != 0 && !c.echec) {
            c.echec = true;
            c.erreur = L"code " + std::to_wstring(code);
        }
        c.processus = c.job = nullptr;
    }
    if (g_journal) fclose(g_journal), g_journal = nullptr;
    CloseHandle(proc);
    CloseHandle(job);
    PostMessageW(g.hwnd, WM_CONS_FINIE, code, 0);
}

// Lance une tâche (construction, compilation des shaders, mise à jour) dont la sortie suit le protocole « @… ». Son
// arbre de processus meurt avec le launcher ou sur « Annuler ». false : rien n'a été lancé (erreur notée).
bool lancer_tache(Tache t, std::wstring cmd, const std::wstring& dossier, const std::wstring& journal, DWORD priorite) {
    {
        std::lock_guard<std::mutex> l(g.cons.lk);
        if (g.cons.active) return false;
        Construction& c = g.cons;
        c.tache = t;
        c.finie = c.echec = c.annulee = false;
        c.etapes.clear(), c.statut.clear(), c.journal.clear();
        c.courante = -1, c.progres = 0, c.info.clear(), c.erreur.clear();
        c.debut = GetTickCount64(), c.fin = 0;
    }
    auto echec = [](const std::wstring& m) {
        std::lock_guard<std::mutex> l(g.cons.lk);
        g.cons.finie = g.cons.echec = true;
        g.cons.erreur = m;
        g.cons.fin = GetTickCount64();
        return false;
    };
    if (!journal.empty()) g_journal = _wfopen(journal.c_str(), L"wb");
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE lecture = nullptr, ecriture = nullptr;
    if (!CreatePipe(&lecture, &ecriture, &sa, 0)) return echec(L"CreatePipe");
    SetHandleInformation(lecture, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = ecriture;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dossier.c_str(), &si,
                        &pi)) {
        CloseHandle(lecture), CloseHandle(ecriture);
        if (g_journal) fclose(g_journal), g_journal = nullptr;
        return echec(cmd.substr(0, 200) + L" : erreur " + std::to_wstring(GetLastError()));
    }
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    AssignProcessToJobObject(job, pi.hProcess);
    SetPriorityClass(pi.hProcess, priorite);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(ecriture);
    {
        std::lock_guard<std::mutex> l(g.cons.lk);
        g.cons.active = true;
        g.cons.job = job, g.cons.processus = pi.hProcess;
    }
    std::thread(lecteur, lecture, pi.hProcess, job).detach();
    return true;
}

std::wstring commande_python(const std::wstring& script) {
    bool py = false;
    const std::wstring python = trouver_python(py);
    if (python.empty()) return {};
    return L"\"" + python + L"\"" + (py ? L" -3" : L"") + L" -u -X utf8 \"" + script + L"\"";
}

void lancer_construction(bool forcer) {
    if (g.cons.active) return;
    g.page = Page::Construction;
    auto echec = [](const std::wstring& m) {
        std::lock_guard<std::mutex> l(g.cons.lk);
        g.cons.tache = Tache::Construction;
        g.cons.finie = g.cons.echec = true;
        g.cons.erreur = m;
        g.cons.etapes.clear(), g.cons.statut.clear();
        g.cons.fin = GetTickCount64();
    };
    if (!g.copie_ok) return echec(T("pas_de_copie"));
    // LANCEUR_PREPARER : autre script de construction (vérification de l'interface)
    const wchar_t* autre = _wgetenv(L"LANCEUR_PREPARER");
    std::wstring cmd = commande_python(autre ? std::wstring(autre) : g.recomp + L"\\launcher\\preparer.py");
    if (cmd.empty()) return echec(T("pas_de_python"));
    cmd += L" --copie \"" + g.copie + L"\" --fils " + std::to_wstring(fils_effectifs());
    if (forcer) cmd += L" --forcer";
    SHCreateDirectoryExW(nullptr, g.jeu.c_str(), nullptr);
    // compilation en priorité basse : le PC reste utilisable
    jlog(L"build: %ls", cmd.c_str());
    lancer_tache(Tache::Construction, cmd, g.recomp, g.jeu + L"\\preparation.log", BELOW_NORMAL_PRIORITY_CLASS);
}

// ---------------------------------------------------------------- mises à jour (releases GitHub, recomp/launcher/maj.py)
// Au démarrage : maj.py --verifier interroge la dernière release ; une version plus récente s'affiche en haut.
void verifier_mises_a_jour() {
    if (!g.paquet || !g.maj_auto) return;
    const std::wstring cmd0 = commande_python(g.recomp + L"\\launcher\\maj.py");
    if (cmd0.empty()) return;
    std::thread([cmd = cmd0 + L" --verifier --racine \"" + g.dossier_exe + L"\""]() mutable {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE lecture = nullptr, ecriture = nullptr;
        if (!CreatePipe(&lecture, &ecriture, &sa, 0)) return;
        SetHandleInformation(lecture, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = ecriture;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g.dossier_exe.c_str(), &si, &pi)) {
            CloseHandle(lecture), CloseHandle(ecriture);
            return;
        }
        CloseHandle(ecriture);
        CloseHandle(pi.hThread);
        std::string sortie;
        char b[4096];
        DWORD n = 0;
        while (ReadFile(lecture, b, sizeof b, &n, nullptr) && n) sortie.append(b, n);
        CloseHandle(lecture);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        const size_t k = sortie.find("@maj ");
        if (k == std::string::npos) return;
        std::string v = sortie.substr(k + 5);
        v = v.substr(0, v.find_first_of(" \r\n"));
        std::lock_guard<std::mutex> l(g.maj_lk);
        g.maj = wide(v);
        jlog(L"update available: %ls", g.maj.c_str());
        PostMessageW(g.hwnd, WM_MAJ_DISPO, 0, 0);   // la question est posée par le fil de la fenêtre
    }).detach();
}

// Le jeu construit est-il encore celui de ces sources et de cette copie ? preparer.py --a-refaire compare les
// signatures des étapes (≈ 0,3 s, rien n'est construit). S'il reste des étapes à refaire, le jeu est « à mettre à
// jour » (WM_REFAIRE) : le bouton principal lance alors la reconstruction, jamais le jeu. Une mise à jour qui exige une
// reconstruction l'impose donc toujours, même sans changement de numéro de version.
void verifier_reconstruction() {
    if (!g.jeu_pret || !g.copie_ok || g.copie.empty()) return;
    const std::wstring cmd0 = commande_python(g.recomp + L"\\launcher\\preparer.py");
    if (cmd0.empty()) return;
    std::thread([cmd = cmd0 + L" --copie \"" + g.copie + L"\" --a-refaire"]() mutable {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE lecture = nullptr, ecriture = nullptr;
        if (!CreatePipe(&lecture, &ecriture, &sa, 0)) return;
        SetHandleInformation(lecture, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = si.hStdError = ecriture;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g.dossier_exe.c_str(), &si, &pi)) {
            CloseHandle(lecture), CloseHandle(ecriture);
            return;
        }
        CloseHandle(ecriture);
        CloseHandle(pi.hThread);
        std::string sortie;
        char b[4096];
        DWORD n = 0;
        while (ReadFile(lecture, b, sizeof b, &n, nullptr) && n) sortie.append(b, n);
        CloseHandle(lecture);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        const size_t k = sortie.find("@a_refaire");
        if (k == std::string::npos) return;
        std::string v = sortie.substr(k + 10);
        v = v.substr(0, v.find_first_of("\r\n"));
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        if (!v.empty()) PostMessageW(g.hwnd, WM_REFAIRE, 0, (LPARAM) new std::wstring(wide(v)));
    }).detach();
}

// DÉSINSTALLER (page Construction, paquet installé seulement) : deux questions, puis desinstaller.py attend la
// fermeture du launcher et retire tout ce que le paquet a écrit sur le PC. La copie du jeu du joueur n'est jamais
// supprimée ; ses sauvegardes seulement s'il le demande. Lancé depuis le dossier temporaire, pour ne pas tenir le
// dossier d'installation.
void desinstaller() {
    if (!g.paquet || g.cons.active || g.jeu_lance) return;
    if (MessageBoxW(g.hwnd, T("desinst_question"), NOM, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
    const bool sauvegardes =
        MessageBoxW(g.hwnd, T("desinst_sauvegardes"), NOM, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
    std::wstring cmd = commande_python(g.recomp + L"\\launcher\\desinstaller.py");
    if (cmd.empty()) {
        g.statut_jeu = T("pas_de_python");
        return;
    }
    cmd += L" --desinstaller --oui --pid " + std::to_wstring(GetCurrentProcessId()) + L" --racine \"" + g.dossier_exe + L"\"" +
           (sauvegardes ? L" --sauvegardes" : L"");
    jlog(L"uninstall requested (saves %ls)", sauvegardes ? L"deleted too" : L"kept");
    wchar_t temp[MAX_PATH + 1] = L"";
    GetTempPathW(MAX_PATH, temp);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const DWORD drapeaux = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, drapeaux | CREATE_BREAKAWAY_FROM_JOB, nullptr, temp, &si, &pi) &&
        !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, drapeaux, nullptr, temp, &si, &pi)) {
        jlog(L"uninstall: desinstaller.py not started (error %lu)", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g.quitter_sans_question = true;
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

void lancer_mise_a_jour() {
    if (g.cons.active || g.jeu_lance) return;
    const std::wstring cmd = commande_python(g.recomp + L"\\launcher\\maj.py");
    if (cmd.empty()) return;
    jlog(L"update: installing %ls", g.maj.c_str());
    lancer_tache(Tache::MiseAJour, cmd + L" --installer --racine \"" + g.dossier_exe + L"\"", g.dossier_exe,
                 g.dossier_exe + L"\\mise_a_jour.log", NORMAL_PRIORITY_CLASS);
}

// mise à jour téléchargée et vérifiée : maj.py --finir attend la fermeture du launcher, installe les fichiers puis le
// relance (lanceur.exe ne peut pas être remplacé tant qu'il tourne)
void finir_mise_a_jour() {
    std::wstring cmd = commande_python(g.recomp + L"\\launcher\\maj.py");
    if (cmd.empty()) return;
    cmd += L" --finir --pid " + std::to_wstring(GetCurrentProcessId()) + L" --racine \"" + g.dossier_exe + L"\"";
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const DWORD drapeaux = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, drapeaux | CREATE_BREAKAWAY_FROM_JOB, nullptr,
                        g.dossier_exe.c_str(), &si, &pi) &&
        !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, drapeaux, nullptr, g.dossier_exe.c_str(), &si, &pi)) {
        std::lock_guard<std::mutex> l(g.cons.lk);
        g.cons.echec = true;
        g.cons.erreur = L"maj.py --finir : erreur " + std::to_wstring(GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    g.quitter_sans_question = true;
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- emplacement du jeu (issue GitHub n° 14, deplacer.py)
// argument entre guillemets : des barres obliques inverses finales (« D:\ ») sont doublées, sinon la dernière
// échapperait le guillemet fermant
std::wstring entre_guillemets(const std::wstring& p) {
    size_t n = 0;
    while (n < p.size() && p[p.size() - 1 - n] == L'\\') n++;
    return L"\"" + p + std::wstring(n, L'\\') + L"\"";
}

// lance une commande sans fenêtre et rend sa sortie (vide si elle n'a pas démarré)
std::string sortie_commande(std::wstring cmd, DWORD delai_ms) {
    std::string sortie;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE lecture = nullptr, ecriture = nullptr;
    if (!CreatePipe(&lecture, &ecriture, &sa, 0)) return sortie;
    SetHandleInformation(lecture, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = ecriture;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g.dossier_exe.c_str(), &si, &pi)) {
        CloseHandle(lecture), CloseHandle(ecriture);
        return sortie;
    }
    CloseHandle(ecriture);
    CloseHandle(pi.hThread);
    char b[4096];
    DWORD n = 0;
    while (ReadFile(lecture, b, sizeof b, &n, nullptr) && n) sortie.append(b, n);
    CloseHandle(lecture);
    WaitForSingleObject(pi.hProcess, delai_ms);
    CloseHandle(pi.hProcess);
    return sortie;
}

// reste de la ligne qui suit `marque` dans une sortie au protocole « @… » (vide : absente)
std::string apres_marque(const std::string& sortie, const char* marque) {
    const size_t k = sortie.find(marque);
    if (k == std::string::npos) return {};
    const std::string v = sortie.substr(k + strlen(marque));
    return v.substr(0, v.find_first_of("\r\n"));
}

// processus détaché, depuis le dossier temporaire (il ne tient pas le dossier d'installation) : il survit au launcher
bool lancer_detache(std::wstring cmd) {
    wchar_t temp[MAX_PATH + 1] = L"";
    GetTempPathW(MAX_PATH, temp);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const DWORD drapeaux = CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, drapeaux | CREATE_BREAKAWAY_FROM_JOB, nullptr, temp, &si, &pi) &&
        !CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, drapeaux, nullptr, temp, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// copie annulée, en échec ou restée en plan : le dossier commencé est retiré (deplacer.py --abandonner ne supprime
// qu'un dossier qui porte sa marque, jamais celui d'où il tourne)
void abandonner_deplacement() {
    if (g.dep_vers.empty()) return;
    const std::wstring cmd = commande_python(g.recomp + L"\\launcher\\deplacer.py");
    if (!cmd.empty()) lancer_detache(cmd + L" --abandonner --vers " + entre_guillemets(g.dep_vers));
    ini_ecrire(L"deplacement_vers", L"");
    g.dep_vers.clear();
}

// au démarrage : un déplacement resté en plan (launcher fermé pendant la copie) ; après un déplacement réussi, la clé
// désigne ce dossier-ci : elle est seulement effacée
void deplacement_au_demarrage() {
    const std::wstring vers = ini_lire(L"deplacement_vers", L"");
    if (vers.empty()) return;
    if (_wcsicmp(vers.c_str(), g.dossier_exe.c_str()) != 0) {
        jlog(L"move: unfinished copy left by the last run, removed");
        g.dep_vers = vers;
        abandonner_deplacement();
    } else {
        ini_ecrire(L"deplacement_vers", L"");
    }
}

// Emplacement du jeu (page Réglages) : le joueur choisit un dossier ; deplacer.py --verifier dit où l'installation ira
// et si c'est un autre volume ; une question ; puis deplacer.py --copier, tâche suivie par la page Construction (rien à
// copier sur le même volume). Ensuite finir_deplacement ferme le launcher. Jamais dans un dossier de développement.
// LANCEUR_DEPLACER_VERS (vérification) : dossier choisi, sans dialogue ni question.
void deplacer() {
    if (!g.paquet || g.sources || g.cons.active || g.jeu_lance) return;
    std::wstring choix;
    const wchar_t* essai = _wgetenv(L"LANCEUR_DEPLACER_VERS");
    if (essai) {
        choix = essai;
    } else {
        IFileOpenDialog* d = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
        DWORD options = 0;
        d->GetOptions(&options);
        d->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        d->SetTitle(T("dep_dialogue"));
        if (SUCCEEDED(d->Show(g.hwnd))) {
            IShellItem* r = nullptr;
            if (SUCCEEDED(d->GetResult(&r))) {
                PWSTR p = nullptr;
                if (SUCCEEDED(r->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) choix = p;
                CoTaskMemFree(p);
                r->Release();
            }
        }
        d->Release();
    }
    if (choix.empty() || g.cons.active || g.jeu_lance) return;
    const std::wstring script = commande_python(g.recomp + L"\\launcher\\deplacer.py");
    if (script.empty()) {
        g.statut_jeu = T("pas_de_python");
        return;
    }
    const std::wstring base = script + L" --racine " + entre_guillemets(g.dossier_exe);
    const std::string sortie = sortie_commande(base + L" --verifier --vers " + entre_guillemets(choix), 20000);
    const std::wstring dest = wide(apres_marque(sortie, "@destination "));
    if (dest.empty()) {
        const std::wstring e = wide(apres_marque(sortie, "@erreur "));
        jlog(L"move refused: %ls", e.empty() ? L"deplacer.py gave no answer" : e.c_str());
        const std::wstring m = std::wstring(T("dep_refus")) + (e.empty() ? L"deplacer.py" : e);
        if (essai) g.statut_jeu = m;
        else MessageBoxW(g.hwnd, m.c_str(), NOM, MB_OK | MB_ICONWARNING);
        return;
    }
    const bool autre = apres_marque(sortie, "@volume ") != "meme";
    if (!essai) {
        wchar_t q[1600];
        swprintf(q, 1600, T(autre ? "dep_question_autre" : "dep_question_meme"), dest.c_str());
        if (MessageBoxW(g.hwnd, q, NOM, MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
    }
    g.dep_vers = dest, g.dep_autre_volume = autre;
    ini_ecrire(L"deplacement_vers", dest);   // launcher fermé pendant la copie : retirée au prochain démarrage
    jlog(L"move requested (%ls)", autre ? L"other drive: copy, then the old folder is deleted" : L"same drive: the folder is renamed");
    g.page = Page::Construction;
    lancer_tache(Tache::Deplacement, base + L" --copier --vers " + entre_guillemets(dest), g.dossier_exe,
                 g.dossier_exe + L"\\deplacement.log", NORMAL_PRIORITY_CLASS);
}

// Copie faite (ou rien à copier) : deplacer.py --finir attend la fermeture du launcher, renomme le dossier ou supprime
// l'ancien, puis relance le launcher. Autre volume : c'est le Python du nouveau dossier qui le lance, pour que l'ancien
// dossier puisse être supprimé en entier.
void finir_deplacement() {
    const std::wstring python = g.dep_vers + L"\\outils\\python\\python.exe", script = g.dep_vers + L"\\recomp\\launcher\\deplacer.py";
    std::wstring cmd;
    if (g.dep_autre_volume && existe(python) && existe(script)) cmd = L"\"" + python + L"\" -u -X utf8 \"" + script + L"\"";
    else cmd = commande_python(g.recomp + L"\\launcher\\deplacer.py");
    if (!cmd.empty())
        cmd += L" --finir --pid " + std::to_wstring(GetCurrentProcessId()) + L" --racine " + entre_guillemets(g.dossier_exe) +
               L" --vers " + entre_guillemets(g.dep_vers);
    if (cmd.empty() || !lancer_detache(cmd)) {
        jlog(L"move: deplacer.py --finir not started (error %lu)", GetLastError());
        abandonner_deplacement();
        g.statut_jeu = T("dep_echec");
        return;
    }
    jlog(L"move: the launcher closes, the folder is moved next");
    g.quitter_sans_question = true;
    PostMessageW(g.hwnd, WM_CLOSE, 0, 0);
}

void annuler_construction() {
    std::lock_guard<std::mutex> l(g.cons.lk);
    if (!g.cons.active || !g.cons.job) return;
    g.cons.annulee = true;
    TerminateJobObject(g.cons.job, 1);
}

// ---------------------------------------------------------------- jeu
void lancer_jeu();
// JOUER : d'abord les shaders compilés pour ce GPU (au premier lancement, après une mise à jour ou un nouveau pilote ;
// sinon fini en une fraction de seconde), puis le jeu
void jouer() {
    manettes_fermer();   // les manettes sont au jeu (onglet Manette ouvert : SDL arrêté dans le launcher)
    // Le jeu engage environ 12 Go de mémoire validable (RAM + fichier d'échange ; 12,06 Go mesurés après 3 min 30 de
    // partie le 2026-11-11). S'il en reste moins de 13, il s'arrêterait
    // au bout de quelques secondes (vu le 2026-11-11 : 8,6 Go libres juste après une construction) : on le dit avant
    // de lancer. Un second clic dans les 30 s lance quand même.
    // Depuis le 2026-11-12, le jeu rend ses pages vides et range ses pages oubliées (memory.cpp) : 6,4 Go engagés en
    // moyenne et 7,4 Go au plus pendant 19 minutes de partie de l'utilisateur, lancée avec 9,6 Go libres sans aucun
    // problème. Seuil de l'alerte demandé par l'utilisateur : 10 Go.
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    GlobalMemoryStatusEx(&ms);
    const double libre = ms.ullAvailPageFile / 1073741824.0;
    static ULONGLONG averti = 0;
    if (libre < 10.0 && (!averti || GetTickCount64() - averti > 30000)) {
        averti = GetTickCount64();
        wchar_t b[400];
        swprintf(b, 400, T("memoire_faible"), libre);
        g.statut_jeu = b;
        jlog(L"game: low available commit (%.1f GB), start to be confirmed", libre);
        return;
    }
    averti = 0;
    const std::wstring exe = g.jeu + L"\\recomp_main.exe";
    if (!lancer_tache(Tache::Shaders, L"\"" + exe + L"\" --precompile", g.jeu, L"", NORMAL_PRIORITY_CLASS)) lancer_jeu();
}

void lancer_jeu() {
    const std::wstring exe = g.jeu + L"\\recomp_main.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    // le jeu réécrit recomp_run.log : le journal de la partie précédente est gardé à côté
    const std::wstring journal = g.jeu + L"\\recomp_run.log";
    if (existe(journal)) MoveFileExW(journal.c_str(), (g.jeu + L"\\recomp_run_precedent.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    // la copie choisie : le jeu la reprend si le manifeste pointe vers un ancien emplacement (dossier déplacé, issue n° 4)
    if (!g.copie.empty() && existe(g.copie)) SetEnvironmentVariableW(L"RECOMP_NUP", g.copie.c_str());
    // environnement : celui du launcher, plus le volume choisi
    std::vector<wchar_t> env;
    if (g.volume != 100) {
        wchar_t* e = GetEnvironmentStringsW();
        for (wchar_t* p = e; *p; p += wcslen(p) + 1) {
            if (!_wcsnicmp(p, L"RECOMP_VOLUME=", 14)) continue;
            env.insert(env.end(), p, p + wcslen(p) + 1);
        }
        FreeEnvironmentStringsW(e);
        const std::wstring v = L"RECOMP_VOLUME=" + std::to_wstring(g.volume);
        env.insert(env.end(), v.c_str(), v.c_str() + v.size() + 1);
        env.push_back(0);
    }
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.empty() ? nullptr : env.data(),
                        g.jeu.c_str(), &si, &pi)) {
        g.statut_jeu = L"recomp_main.exe : erreur " + std::to_wstring(GetLastError());
        jlog(L"game: cannot start (error %lu)", GetLastError());
        return;
    }
    jlog(L"game started (pid %lu, volume %d %%, %dp, %d Hz, %ls, rumble %d %%)", pi.dwProcessId, g.volume,
            g.reglages.resolution, g.reglages.hz, g.reglages.sans_bordure ? L"fullscreen" : L"windowed", g.reglages.vibration);
    CloseHandle(pi.hThread);
    g.jeu_lance = true;
    g.statut_jeu.clear();
    video_arreter();
    ShowWindow(g.hwnd, SW_HIDE);
    std::thread([h = pi.hProcess] {
        WaitForSingleObject(h, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(h, &code);
        CloseHandle(h);
        PostMessageW(g.hwnd, WM_JEU_FINI, code, 0);
    }).detach();
}

void fond_relancer() {
    video_arreter();
    if (g.fond_video && compter(g.extraits + L"\\extrait_*.mp4") > 0) video_demarrer(g.extraits);
}

// ---------------------------------------------------------------- raccourci sur le bureau (issue GitHub n° 14)
// Un fichier « StarFox Remake [recompiled].lnk » sur le bureau du joueur, vers ce lanceur.exe (avec son icône). Créé ou
// retiré depuis la page Réglages. Au démarrage, un raccourci dont la cible n'existe plus (dossier déplacé) est refait
// vers ce launcher. LANCEUR_BUREAU (vérification) : un autre dossier que le bureau.
std::wstring chemin_exe() {
    wchar_t b[2048];
    GetModuleFileNameW(nullptr, b, 2048);
    return b;
}

std::wstring raccourci_chemin() {
    std::wstring bureau;
    wchar_t b[MAX_PATH + 1] = L"";
    if (const wchar_t* e = _wgetenv(L"LANCEUR_BUREAU")) bureau = e;
    else if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, b))) bureau = b;
    return bureau.empty() ? bureau : bureau + L"\\" + NOM + L".lnk";
}

// cible d'un raccourci (vide : illisible)
std::wstring raccourci_cible(const std::wstring& lnk) {
    std::wstring cible;
    IShellLinkW* l = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)))) return cible;
    IPersistFile* f = nullptr;
    if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f)))) {
        wchar_t b[2048] = L"";
        if (SUCCEEDED(f->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(l->GetPath(b, 2048, nullptr, SLGP_RAWPATH))) cible = b;
        f->Release();
    }
    l->Release();
    return cible;
}

bool raccourci_creer() {
    const std::wstring lnk = raccourci_chemin(), exe = chemin_exe();
    if (lnk.empty()) return false;
    IShellLinkW* l = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&l)))) return false;
    l->SetPath(exe.c_str());
    l->SetWorkingDirectory(parent(exe).c_str());
    l->SetDescription(NOM);
    l->SetIconLocation(exe.c_str(), 0);
    bool ok = false;
    IPersistFile* f = nullptr;
    if (SUCCEEDED(l->QueryInterface(IID_PPV_ARGS(&f)))) {
        ok = SUCCEEDED(f->Save(lnk.c_str(), TRUE));
        f->Release();
    }
    l->Release();
    return ok;
}

int g_raccourci = -1;   // dernier état lu pour la page Réglages (-1 : à relire)
ULONGLONG g_raccourci_t = 0;

int raccourci_lire() {   // 0 aucun, 1 vers ce launcher, 2 vers un autre dossier
    const std::wstring lnk = raccourci_chemin();
    if (lnk.empty() || !existe(lnk)) return 0;
    return _wcsicmp(raccourci_cible(lnk).c_str(), chemin_exe().c_str()) == 0 ? 1 : 2;
}

void raccourci_basculer() {   // présent, vers ce launcher : retiré ; sinon créé, ou refait vers ce launcher
    if (raccourci_lire() == 1) {
        jlog(L"desktop shortcut %ls", DeleteFileW(raccourci_chemin().c_str()) ? L"removed" : L"not removed");
    } else {
        const bool ok = raccourci_creer();
        jlog(L"desktop shortcut %ls", ok ? L"created" : L"not created");
        if (!ok) g.statut_jeu = T("rac_echec");
    }
    g_raccourci = -1;
}

void raccourci_suivre() {
    const std::wstring lnk = raccourci_chemin();
    if (lnk.empty() || !existe(lnk)) return;
    const std::wstring cible = raccourci_cible(lnk);
    if (cible.empty() || existe(cible)) return;   // illisible, ou vers un launcher qui existe (celui-ci ou un autre)
    jlog(L"desktop shortcut %ls", raccourci_creer() ? L"updated (the folder moved)" : L"not updated");
}

// ---------------------------------------------------------------- actions
template <class T> void cycler(T& v, std::initializer_list<T> valeurs, int sens) {
    std::vector<T> l(valeurs);
    auto it = std::find(l.begin(), l.end(), v);
    int i = it == l.end() ? 0 : (int)(it - l.begin());
    i = (i + sens + (int)l.size()) % (int)l.size();
    v = l[i];
}

// RAPPORT DE BUG : rapport.py rassemble version, matériel, réglages et journaux (chemins anonymisés) dans
// <racine>\SpaceAnimalsRecomp-bug-report.zip ; l'Explorateur s'ouvre ensuite sur ce fichier.
void rapport_de_bug() {
    if (g.rapport_en_cours) return;
    std::wstring cmd = commande_python(g.recomp + L"\\launcher\\rapport.py");
    if (cmd.empty()) {
        g.statut_jeu = T("pas_de_python");
        return;
    }
    g.rapport_en_cours = true;
    g.statut_jeu = T("rapport_en_cours");
    jlog(L"bug report requested");
    std::thread([cmd]() mutable {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        HANDLE lecture = nullptr, ecriture = nullptr;
        std::string sortie;
        if (CreatePipe(&lecture, &ecriture, &sa, 0)) {
            SetHandleInformation(lecture, HANDLE_FLAG_INHERIT, 0);
            STARTUPINFOW si{};
            si.cb = sizeof si;
            si.dwFlags = STARTF_USESTDHANDLES;
            si.hStdOutput = si.hStdError = ecriture;
            PROCESS_INFORMATION pi{};
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, g.dossier_exe.c_str(), &si, &pi)) {
                CloseHandle(ecriture);
                CloseHandle(pi.hThread);
                char b[4096];
                DWORD n = 0;
                while (ReadFile(lecture, b, sizeof b, &n, nullptr) && n) sortie.append(b, n);
                WaitForSingleObject(pi.hProcess, 60000);
                CloseHandle(pi.hProcess);
            } else {
                CloseHandle(ecriture);
            }
            CloseHandle(lecture);
        }
        auto* chemin = new std::wstring;
        const size_t k = sortie.find("@rapport ");
        if (k != std::string::npos) {
            std::string v = sortie.substr(k + 9);
            *chemin = wide(v.substr(0, v.find_first_of("\r\n")));
        }
        PostMessageW(g.hwnd, WM_RAPPORT_FINI, 0, (LPARAM)chemin);
    }).detach();
}

void basculer(Page p) { g.page = g.page == p ? Page::Aucune : p; }

void action(int z) {
    if (z >= Z_TOUCHE0 && z <= Z_TOUCHE_FIN) {   // page Contrôles : cette case attend la prochaine touche pressée
        g.capture = z - Z_TOUCHE0;
        return;
    }
    if (z >= Z_PAD0 && z <= Z_PAD_FIN) {   // onglet Manette : ce bouton attend un appui sur la manette, ou un clic sur
        g.capture_pad = z - Z_PAD0;        // une entrée du dessin (avec ou sans manette branchée)
        g.pad_tenues = manettes().appuyees;
        return;
    }
    if ((z >= Z_PADG0 && z <= Z_PADG_FIN) || (z >= Z_PADD0 && z <= Z_PADD_FIN)) {
        // ‹ › : entrée libre précédente ou suivante (celles des autres boutons sont sautées, « Aucun » reste toujours
        // possible) : pas d'échange en cascade en faisant défiler ; pour échanger deux boutons, appuyer sur la manette
        const bool suiv = z >= Z_PADD0;
        const int i = z - (suiv ? Z_PADD0 : Z_PADG0);
        int e = g.reglages.manette[i];
        for (int n = 0; n < P_NB; n++) {
            e = (e + (suiv ? 1 : P_NB - 1)) % P_NB;
            bool prise = false;
            for (int k = 0; k < NB_PAD; k++) prise |= k != i && e != P_AUCUNE && g.reglages.manette[k] == e;
            if (!prise) break;
        }
        pad_affecter(i, e);
        return;
    }
    switch (z) {
    case Z_ONGLET_CLAVIER: g.onglet_manette = false; return;
    case Z_ONGLET_MANETTE: g.onglet_manette = true; return;
    case Z_PAD_DEFAUT: pad_par_defaut(); return;
    case Z_PRESET_G:
    case Z_PRESET_D: {   // deux préréglages : l'un puis l'autre ; depuis des touches personnalisées, ‹ WASD, › ZQSD
        const int p = prereglage_courant();
        touches_choisir_prereglage(p < 0 ? (z == Z_PRESET_D ? 1 : 0) : 1 - p);
        return;
    }
    case Z_PRINCIPAL:
        if (g.jeu_lance) return;
        if (g.cons.active) {
            if (g.cons.tache == Tache::Construction || g.cons.tache == Tache::Deplacement) basculer(Page::Construction);
            return;
        }
        if (g.jeu_pret) return jouer();   // même s'il reste des étapes à refaire : la reconstruction est un choix
        if (!g.copie_ok) choisir_copie();
        if (g.copie_ok) lancer_construction(false);
        return;
    case Z_MAJ_JEU:   // lien « RECONSTRUIRE MAINTENANT » de l'accueil : seules les étapes changées
        if (g.cons.active || g.jeu_lance) return;
        if (!g.copie_ok) choisir_copie();
        if (g.copie_ok) {
            jlog(L"game rebuild started by the player");
            lancer_construction(false);
        }
        return;
    case Z_AFF_PLEIN:
    case Z_AFF_FEN: g.reglages.sans_bordure = z == Z_AFF_PLEIN; reglages_ecrire(); return;
    case Z_MAJ: lancer_mise_a_jour(); return;
    case Z_MAJ_AUTO_G:
    case Z_MAJ_AUTO_D:
        g.maj_auto = !g.maj_auto;
        config_ecrire();
        verifier_mises_a_jour();
        return;
    case Z_MENU_REGLAGES: return basculer(Page::Reglages);
    case Z_MENU_CONTROLES: return basculer(Page::Controles);
    case Z_MENU_CONSTRUCTION: return basculer(Page::Construction);
    case Z_MENU_QUITTER:
    case Z_FERMER: PostMessageW(g.hwnd, WM_CLOSE, 0, 0); return;
    case Z_REDUIRE: ShowWindow(g.hwnd, SW_MINIMIZE); return;
    case Z_COPIE:
    case Z_PARCOURIR: choisir_copie(); return;
    case Z_FR: g.langue = Langue::Fr; config_ecrire(); return;
    case Z_EN: g.langue = Langue::En; config_ecrire(); return;
    case Z_FERMER_PAGE: g.page = Page::Aucune; return;
    case Z_RES_G:
    case Z_RES_D: cycler(g.reglages.resolution, {0, 720, 1080, 1440, 2160}, z == Z_RES_D ? 1 : -1); reglages_ecrire(); return;
    case Z_HZ_G:
    case Z_HZ_D: cycler(g.reglages.hz, {60, 90, 120}, z == Z_HZ_D ? 1 : -1); reglages_ecrire(); return;
    case Z_NATIF_G:
    case Z_NATIF_D: cycler(g.reglages.echelle, {100, 85, 75, 67}, z == Z_NATIF_D ? 1 : -1); reglages_ecrire(); return;
    case Z_DLSS_G:
    case Z_DLSS_D: cycler(g.reglages.dlss, {0, 1, 2, 3, 4, 5}, z == Z_DLSS_D ? 1 : -1); reglages_ecrire(); return;
    case Z_DLSSM_G:
    case Z_DLSSM_D: g.reglages.dlss_modele = g.reglages.dlss_modele == 2 ? 1 : 2; reglages_ecrire(); return;
    case Z_FSR_G:
    case Z_FSR_D: cycler(g.reglages.fsr, {0, 1, 2, 3, 4, 5}, z == Z_FSR_D ? 1 : -1); reglages_ecrire(); return;
    case Z_FSRV_G:
    case Z_FSRV_D: cycler(g.reglages.fsr_version, {0, 4, 3}, z == Z_FSRV_D ? 1 : -1); reglages_ecrire(); return;
    case Z_GPU_G:
    case Z_GPU_D: {   // Automatique, puis chaque carte (nom complet dans recomp.ini)
        if (!g.cartes_pretes.load(std::memory_order_acquire) || g.cartes.empty()) return;
        bool nommee = false;
        const int c = carte_du_jeu(&nommee);
        const int n = (int)g.cartes.size(), i = nommee ? c : -1;   // -1 : Automatique
        const int j = z == Z_GPU_D ? (i + 2) % (n + 1) - 1 : (i + n + 1) % (n + 1) - 1;
        g.reglages.gpu = j < 0 ? std::string("auto") : g.cartes[j].nom;
        reglages_ecrire();
        jlog(L"graphics card: %ls (DLSS %ls)", wide(g.reglages.gpu).c_str(), dlss_possible() ? L"possible" : L"not possible");
        return;
    }
    case Z_MENU_GRAPHISMES: return basculer(Page::Graphismes);
    case Z_LANGUE_G:
    case Z_LANGUE_D: {
        int i = 0;
        for (int k = 0; k < NB_LANGUES_JEU; k++)
            if (g.reglages.langue == LANGUES_JEU[k]) i = k;
        i = (i + (z == Z_LANGUE_D ? 1 : -1) + NB_LANGUES_JEU) % NB_LANGUES_JEU;
        g.reglages.langue = LANGUES_JEU[i];
        reglages_ecrire();
        return;
    }
    case Z_AFF_G:
    case Z_AFF_D: g.reglages.sans_bordure = !g.reglages.sans_bordure; reglages_ecrire(); return;
    case Z_VOL_G:
    case Z_VOL_D: g.volume = std::clamp(g.volume + (z == Z_VOL_D ? 10 : -10), 0, 100); config_ecrire(); return;
    case Z_FOND_G:
    case Z_FOND_D: g.fond_video = !g.fond_video; config_ecrire(); fond_relancer(); return;
    case Z_FILS_G:
    case Z_FILS_D: g.fils = (g.fils + (z == Z_FILS_D ? 1 : -1) + g.fils_max + 1) % (g.fils_max + 1); config_ecrire(); return;
    case Z_RECONSTRUIRE: lancer_construction(true); return;
    case Z_ANNULER: annuler_construction(); return;
    case Z_JOURNAL: ShellExecuteW(g.hwnd, L"open", (g.jeu + L"\\preparation.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); return;
    case Z_VIB_G:
    case Z_VIB_D: cycler(g.reglages.vibration, {0, 25, 50, 75, 100}, z == Z_VIB_D ? 1 : -1); reglages_ecrire(); return;
    case Z_MENU_RAPPORT: rapport_de_bug(); return;
    case Z_DESINSTALLER: desinstaller(); return;
    case Z_DEPLACER: deplacer(); return;
    case Z_RACCOURCI: raccourci_basculer(); return;
    }
}

// ---------------------------------------------------------------- fenêtre
float dip(int px) { return px * 96.f / g.dpi; }

// Échelle d'affichage de la fenêtre : celle de l'écran, multipliée par ECHELLE. Tout passe par g.dpi (taille de la
// fenêtre, dessin Direct2D, position de la souris) : la fenêtre grandit sans toucher à la mise en page. Si la zone de
// travail de l'écran est trop petite pour la fenêtre agrandie, l'échelle est réduite pour qu'elle tienne (jusqu'à
// 0,75 : avant, la fenêtre de 1000 × 600 dépassait d'un écran de 1366 × 768 réglé à 125 %).
float dpi_fenetre(float dpi_ecran, const RECT& zone) {
    const float lw = LARGEUR * dpi_ecran / 96, lh = HAUTEUR * dpi_ecran / 96;
    float e = std::min({ECHELLE, (zone.right - zone.left) / lw, (zone.bottom - zone.top) / lh});
    return dpi_ecran * std::max(e, 0.75f);
}
RECT zone_de_travail(HWND h) {
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    if (h && GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return mi.rcWork;
    RECT zt{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &zt, 0);
    return zt;
}

LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static bool suivi = false;
    switch (m) {
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        const float x = dip(p.x), y = dip(p.y);
        if (y < 46 && zone_sous(x, y) == Z_AUCUNE) return HTCAPTION;   // déplacement par le haut de la fenêtre
        return HTCLIENT;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, g.zone_survol != Z_AUCUNE ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE:
        g.sx = dip(GET_X_LPARAM(l)), g.sy = dip(GET_Y_LPARAM(l));
        g.zone_survol = zone_sous(g.sx, g.sy);
        if (!suivi) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            suivi = true;
        }
        return 0;
    case WM_MOUSELEAVE:
        suivi = false;
        g.sx = g.sy = -1;
        g.zone_survol = Z_AUCUNE;
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(h);
        g.bouton_bas = true;
        g.zone_appui = zone_sous(dip(GET_X_LPARAM(l)), dip(GET_Y_LPARAM(l)));
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        g.bouton_bas = false;
        const int z = zone_sous(dip(GET_X_LPARAM(l)), dip(GET_Y_LPARAM(l)));
        const int a = g.zone_appui;
        g.zone_appui = Z_AUCUNE;
        const int attente = g.capture;   // un clic met fin à l'attente d'une touche (page Contrôles)
        g.capture = -1;
        const int attente_pad = g.capture_pad;   // ou d'un appui sur la manette (onglet Manette)
        g.capture_pad = -1;
        if (z != Z_AUCUNE && z == a) {
            if (z >= Z_ENTREE0 && z <= Z_ENTREE_FIN)   // entrée du dessin : donnée au bouton qui attendait, ou attente
                pad_entree_cliquee(z - Z_ENTREE0, attente_pad);
            else if (!(attente >= 0 && z == Z_TOUCHE0 + attente) && !(attente_pad >= 0 && z == Z_PAD0 + attente_pad))
                action(z);
        }
        return 0;
    }
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
        if (g.capture_pad >= 0) {   // onglet Manette : Échap annule l'attente d'un appui sur la manette
            if (w == VK_ESCAPE) g.capture_pad = -1;
            return 0;
        }
        if (g.capture >= 0) {   // page Contrôles : la touche pressée est donnée à l'action choisie (Échap : annuler)
            if (w == VK_ESCAPE) {
                g.capture = -1;
            } else if (!(l & (1 << 30))) {   // pas une répétition
                const unsigned sc = (unsigned)((l >> 16) & 0xFF) | ((l & (1 << 24)) ? 0xE000u : 0u);
                if (sc & 0xFF) {
                    touche_affecter(g.capture, sc);
                    g.capture = -1;
                }
            }
            return 0;
        }
        if (m == WM_SYSKEYDOWN) break;
        if (w == VK_RETURN) action(Z_PRINCIPAL);
        else if (w == VK_ESCAPE) g.page = Page::Aucune;
        return 0;
    case WM_DPICHANGED: {
        g.dpi = dpi_fenetre((float)HIWORD(w), zone_de_travail(h));
        const RECT* r = (const RECT*)l;   // position proposée par Windows ; la taille suit notre échelle
        SetWindowPos(h, nullptr, r->left, r->top, (int)lround(LARGEUR * g.dpi / 96), (int)lround(HAUTEUR * g.dpi / 96),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        dessin_redim();
        return 0;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) dessin_redim();
        return 0;
    case WM_JEU_FINI:
        g.jeu_lance = false;
        jlog(L"game exited: code %d%ls", (int)w, (int)w == 4 ? L" (out of memory)" : (int)w ? L" (see recomp_run.log)" : L"");
        if ((int)w == 4) {   // le jeu manquait de mémoire (Windows a refusé ses réservations) : autres programmes ouverts
            g.statut_jeu = T("jeu_memoire");
        } else if ((int)w != 0) {
            wchar_t b[256];
            swprintf(b, 256, T("jeu_arrete"), (int)w);
            g.statut_jeu = b;
        }
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        fond_relancer();
        return 0;
    case WM_CONS_FINIE: {
        Tache t;
        bool ok;
        {
            std::lock_guard<std::mutex> lk(g.cons.lk);
            t = g.cons.tache;
            ok = !g.cons.echec && !g.cons.annulee;
        }
        jlog(L"%ls: %ls%ls",
             t == Tache::Shaders ? L"shaders" : t == Tache::MiseAJour ? L"update" : t == Tache::Deplacement ? L"move" : L"build",
                ok ? L"finished" : g.cons.annulee ? L"canceled" : L"failed: ", ok || g.cons.annulee ? L"" : g.cons.erreur.c_str());
        if (t == Tache::Shaders) {   // même en cas d'échec : le jeu compile alors ses shaders au besoin
            if (!g.cons.annulee && !g.sans_jeu) lancer_jeu();
            return 0;
        }
        if (t == Tache::Deplacement) {
            if (ok) {
                finir_deplacement();
            } else {   // rien n'a été déplacé : le dossier commencé est retiré
                abandonner_deplacement();
                std::lock_guard<std::mutex> lk(g.cons.lk);
                if (!g.cons.annulee) g.statut_jeu = std::wstring(T("dep_refus")) + (g.cons.erreur.empty() ? L"?" : g.cons.erreur);
            }
            return 0;
        }
        if (t == Tache::MiseAJour) {
            if (ok) {
                finir_mise_a_jour();
            } else {
                std::lock_guard<std::mutex> lk(g.cons.lk);
                g.statut_jeu = std::wstring(T("maj_echec")) + (g.cons.erreur.empty() ? L"?" : g.cons.erreur);
            }
            return 0;
        }
        detecter_jeu();
        reglages_lire();
        fond_relancer();
        return 0;
    }
    case WM_MAJ_DISPO: {
        // Une nouvelle version est disponible : jamais installée d'office (demande de l'utilisateur, 2026-11-11). Le
        // joueur choisit ; « non » est retenu pour cette version (maj_refusee dans lanceur.ini) et le bandeau
        // « MISE À JOUR … DISPONIBLE » reste là pour plus tard.
        std::wstring maj;
        {
            std::lock_guard<std::mutex> lk(g.maj_lk);
            maj = g.maj;
        }
        const bool essai = _wgetenv(L"LANCEUR_ACTION") || _wgetenv(L"LANCEUR_CAPTURE") || _wgetenv(L"LANCEUR_HZ");
        if (maj.empty() || essai || g.cons.active || g.jeu_lance || ini_lire(L"maj_refusee", L"") == maj) return 0;
        if (!IsWindowEnabled(h)) {   // une autre question est à l'écran : celle-ci attend son tour, jamais deux à la fois
            SetTimer(h, MINUTEUR_MAJ, 1500, nullptr);
            return 0;
        }
        wchar_t q[640];
        swprintf(q, 640, T("maj_question"), maj.c_str());
        if (MessageBoxW(h, q, NOM, MB_YESNO | MB_ICONQUESTION) == IDYES) {
            lancer_mise_a_jour();
        } else {
            ini_ecrire(L"maj_refusee", maj);
            jlog(L"update %ls declined for now by the player", maj.c_str());
        }
        return 0;
    }
    case WM_TIMER:
        if (w == MINUTEUR_MAJ) {   // la question de mise à jour, remise à plus tard parce qu'une autre était à l'écran
            KillTimer(h, MINUTEUR_MAJ);
            PostMessageW(h, WM_MAJ_DISPO, 0, 0);
            return 0;
        }
        break;
    case WM_REFAIRE: {   // preparer.py --a-refaire : des étapes de la construction sont à refaire
        std::wstring* v = (std::wstring*)l;
        if (g.jeu_pret && !g.cons.active && !g.jeu_lance) {
            if (!g.jeu_obsolete) jlog(L"game rebuild offered (not required), steps to redo: %ls", v->c_str());
            g.jeu_obsolete = true;
        }
        delete v;
        return 0;
    }
    case WM_RAPPORT_FINI: {
        std::wstring* chemin = (std::wstring*)l;
        if (chemin && !chemin->empty()) {
            jlog(L"bug report created");
            g.statut_jeu = std::wstring(T("rapport_ok"));
            // l'Explorateur s'ouvre sur le fichier, sélectionné : il n'y a plus qu'à le glisser dans l'issue
            ShellExecuteW(h, nullptr, L"explorer.exe", (L"/select,\"" + *chemin + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            g.statut_jeu = std::wstring(T("rapport_echec"));
            jlog(L"bug report: failed");
        }
        g.rapport_en_cours = false;
        delete chemin;
        return 0;
    }
    case WM_CLOSE:
        if (g.cons.active && !_wgetenv(L"LANCEUR_CAPTURE_QUITTER") && !g.quitter_sans_question) {
            const wchar_t* q = g.langue == Langue::Fr ? L"Une tâche est en cours (construction, shaders, mise à jour ou déplacement). L'arrêter et quitter ?"
                                                      : L"A task is running (build, shaders, update or move). Stop it and quit?";
            if (MessageBoxW(h, q, NOM, MB_YESNO | MB_ICONWARNING) != IDYES) return 0;
        }
        annuler_construction();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int fils_effectifs() { return g.fils ? g.fils : std::max(1, g.fils_max - 1); }
int touches_prereglage_courant() { return prereglage_courant(); }   // pour dessin.cpp (page Contrôles)
int raccourci_etat() {   // pour dessin.cpp (page Réglages) : relu au plus une fois par seconde
    const ULONGLONG t = GetTickCount64();
    if (g_raccourci < 0 || t - g_raccourci_t > 1000) g_raccourci = raccourci_lire(), g_raccourci_t = t;
    return g_raccourci;
}

}  // namespace lz

using namespace lz;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (HWND autre = FindWindowW(CLASSE, nullptr)) {   // une seule instance : on ramène la première
        ShowWindow(autre, SW_RESTORE);
        SetForegroundWindow(autre);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    timeBeginPeriod(1);
    g.inst = inst;
    g.t0 = GetTickCount64();
    chemins();
    config_lire();
    copie_trouver();
    detecter_jeu();
    reglages_lire();
    jlog(L"--- launcher %ls started (%ls): game copy %ls, game %ls", g.version.c_str(), g.paquet ? L"package" : L"sources",
            g.copie.empty() ? L"missing" : g.copie_ok ? L"recognized" : L"not recognized",
            g.jeu_pret ? (g.jeu_obsolete ? L"ready, rebuild offered" : L"ready") : L"not built");
    deplacement_au_demarrage();
    // raccourci du bureau suivi quand le dossier a été déplacé ; en vérification, seulement avec un bureau d'essai
    if (_wgetenv(L"LANCEUR_BUREAU") || !(_wgetenv(L"LANCEUR_ACTION") || _wgetenv(L"LANCEUR_CAPTURE") || _wgetenv(L"LANCEUR_HZ")))
        raccourci_suivre();
    std::thread(cartes_lister).detach();   // cartes graphiques (page Graphismes)
    if (const wchar_t* k = _wgetenv(L"LANCEUR_STATUT")) {   // vérification : afficher un message d'état (capture)
        char cle[64] = "";
        WideCharToMultiByte(CP_UTF8, 0, k, -1, cle, sizeof cle, nullptr, nullptr);
        wchar_t b[400];
        swprintf(b, 400, T(cle), 9.9);
        g.statut_jeu = b;
    }
    if (const wchar_t* p = _wgetenv(L"LANCEUR_PAGE")) {   // vérification : ouvrir une page au démarrage
        if (!wcscmp(p, L"reglages")) g.page = Page::Reglages;
        else if (!wcscmp(p, L"controles")) g.page = Page::Controles;
        else if (!wcscmp(p, L"manette")) g.page = Page::Controles, g.onglet_manette = true;
        else if (!wcscmp(p, L"construction")) g.page = Page::Construction;
        else if (!wcscmp(p, L"graphismes")) g.page = Page::Graphismes;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = CLASSE;
    RegisterClassExW(&wc);
    RECT zt;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &zt, 0);
    g.dpi = dpi_fenetre((float)GetDpiForSystem(), zt);
    const int w = (int)lround(LARGEUR * g.dpi / 96), hh = (int)lround(HAUTEUR * g.dpi / 96);
    g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, CLASSE, NOM, WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                             zt.left + (zt.right - zt.left - w) / 2, zt.top + (zt.bottom - zt.top - hh) / 2, w, hh, nullptr, nullptr,
                             inst, nullptr);
    if (!g.hwnd) return 1;
    const RECT zf = zone_de_travail(g.hwnd);
    const float d = dpi_fenetre((float)GetDpiForWindow(g.hwnd), zf);
    if (d != g.dpi) {   // écran de la fenêtre à une autre échelle que l'écran principal : taille refaite, recentrée
        g.dpi = d;
        const int w2 = (int)lround(LARGEUR * d / 96), h2 = (int)lround(HAUTEUR * d / 96);
        SetWindowPos(g.hwnd, nullptr, zf.left + (zf.right - zf.left - w2) / 2, zf.top + (zf.bottom - zf.top - h2) / 2, w2, h2,
                     SWP_NOZORDER);
    }
    verifier_mises_a_jour();
    const DWORD coins = 1;   // DWMWCP_DONOTROUND : angles droits, comme le cadre dessiné
    DwmSetWindowAttribute(g.hwnd, 33, &coins, sizeof coins);
    const MARGINS ombre{0, 0, 0, 1};   // ombre de fenêtre (DWM) malgré WS_POPUP
    DwmExtendFrameIntoClientArea(g.hwnd, &ombre);
    if (!dessin_init(g.hwnd)) {
        MessageBoxW(nullptr, T("affichage_indisponible"), NOM, MB_ICONERROR);   // l'étape en échec est dans lanceur.log
        return 1;
    }
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    fond_relancer();
    // Reconstruction proposée après une mise à jour, jamais imposée (demande de l'utilisateur du 2026-11-11 au soir :
    // « laisse le choix au joueur de reconstruire le jeu ou pas […] avec une mention : certaines améliorations
    // nécessitent la reconstruction du jeu »). À la première ouverture d'une nouvelle version du launcher, si le jeu a
    // été construit par une autre version, le launcher pose la question ; rien ne démarre sans un « oui ». Tant qu'il
    // reste des étapes à refaire (detecter_jeu, verifier_reconstruction), l'accueil garde la mention et le lien
    // « RECONSTRUIRE MAINTENANT », et le bouton principal lance le jeu actuel.
    {
        const bool essai = _wgetenv(L"LANCEUR_ACTION") || _wgetenv(L"LANCEUR_CAPTURE") || _wgetenv(L"LANCEUR_HZ");
        const bool nouvelle_version = g.paquet && ini_lire(L"version_vue", L"") != g.version;
        if (nouvelle_version && !essai) ini_ecrire(L"version_vue", g.version);
        if (nouvelle_version && !essai && g.jeu_pret && g.jeu_obsolete && g.copie_ok) {
            // le joueur choisit : reconstruire maintenant, ou garder le jeu actuel et le faire plus tard, ou jamais
            if (MessageBoxW(g.hwnd, T("maj_jeu_question"), NOM, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                jlog(L"new launcher version: game update accepted by the player");
                lancer_construction(false);
            } else {
                jlog(L"new launcher version: game update postponed by the player");
            }
        } else {
            verifier_reconstruction();
        }
    }
    // vérification de l'interface : lance une construction (jamais le jeu), avec un autre pilote (LANCEUR_PREPARER)
    if (const wchar_t* a = _wgetenv(L"LANCEUR_ACTION")) {
        if (!wcscmp(a, L"construire") && _wgetenv(L"LANCEUR_PREPARER")) {
            lancer_construction(false);
        } else if (!wcscmp(a, L"shaders")) {   // compilation des shaders seule, sans le jeu
            g.sans_jeu = true;
            jouer();
        } else if (!wcscmp(a, L"deplacer")) {   // déplacement vers LANCEUR_DEPLACER_VERS, sans dialogue ni question
            deplacer();
        } else if (!wcscmp(a, L"raccourci")) {   // raccourci créé ou retiré (LANCEUR_BUREAU)
            raccourci_basculer();
        }
    }

    MSG msg;
    bool fin = false;
    while (!fin) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) fin = true;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (fin) break;
        if (IsIconic(g.hwnd) || !IsWindowVisible(g.hwnd)) {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
            continue;
        }
        if (g.sx >= 0) g.zone_survol = zone_sous(g.sx, g.sy);
        // onglet Manette ouvert, sans partie ni construction en cours : manettes lues (dessin, attente d'un appui) ;
        // sinon SDL est arrêté et les manettes sont rendues (au jeu)
        if (g.page == Page::Controles && g.onglet_manette && !g.jeu_lance && !g.cons.active) {
            manettes_lire();
            pad_suivre_capture();
        } else {
            manettes_fermer();
            g.capture_pad = -1;
        }
        dessin_image();   // Present attend le balayage de l'écran : ≈ 60 images par seconde
    }
    video_arreter();
    annuler_construction();
    dessin_liberer();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
