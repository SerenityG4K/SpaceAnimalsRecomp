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
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <thread>

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
    fprintf(f, "%04u-%02u-%02u %02u:%02u:%02u  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
            utf8(b).c_str());
    fclose(f);
}

namespace {

constexpr UINT WM_JEU_FINI = WM_APP + 1, WM_CONS_FINIE = WM_APP + 2, WM_RAPPORT_FINI = WM_APP + 3, WM_REFAIRE = WM_APP + 4,
               WM_MAJ_DISPO = WM_APP + 5;
constexpr UINT_PTR MINUTEUR_MAJ = 1;

const wchar_t* CLASSE = L"LanceurStarFoxRecompile";

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

void reglages_lire() {
    for (const std::string& l0 : lignes_fichier(recomp_ini())) {
        const std::string l = trim(l0);
        if (l.empty() || l[0] == ';' || l[0] == '#' || l[0] == '[') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(l.substr(0, eq)), v = trim(l.substr(eq + 1));
        if (k == "refresh_rate") g.reglages.hz = std::clamp(atoi(v.c_str()), 30, 240);
        else if (k == "resolution") g.reglages.resolution = v == "auto" ? 0 : atoi(v.c_str());
        else if (k == "native_resolution") g.reglages.natif = !(v == "false" || v == "0" || v == "non" || v == "off");
        else if (k == "window_mode") g.reglages.sans_bordure = !(v == "windowed" || v == "window" || v == "fenetre");
        else if (k == "language") g.reglages.langue = v.empty() ? "auto" : v;
        else if (k == "vibration") g.reglages.vibration = std::clamp(atoi(v.c_str()), 0, 100);
        else if (k.rfind("key_", 0) == 0) {
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
        {"native_resolution", g.reglages.natif ? "true" : "false"},
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
    bool section_clavier = false;
    for (const std::string& l : v) section_clavier = section_clavier || trim(l) == "[keyboard]";
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
    switch (z) {
    case Z_PRESET_G:
    case Z_PRESET_D: {   // deux préréglages : l'un puis l'autre ; depuis des touches personnalisées, ‹ WASD, › ZQSD
        const int p = prereglage_courant();
        touches_choisir_prereglage(p < 0 ? (z == Z_PRESET_D ? 1 : 0) : 1 - p);
        return;
    }
    case Z_PRINCIPAL:
        if (g.jeu_lance) return;
        if (g.cons.active) {
            if (g.cons.tache == Tache::Construction) basculer(Page::Construction);
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
    case Z_NATIF_D: g.reglages.natif = !g.reglages.natif; reglages_ecrire(); return;
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
        if (z != Z_AUCUNE && z == a && !(attente >= 0 && z == Z_TOUCHE0 + attente)) action(z);
        return 0;
    }
    case WM_SYSKEYDOWN:
    case WM_KEYDOWN:
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
        jlog(L"%ls: %ls%ls", t == Tache::Shaders ? L"shaders" : t == Tache::MiseAJour ? L"update" : L"build",
                ok ? L"finished" : g.cons.annulee ? L"canceled" : L"failed: ", ok || g.cons.annulee ? L"" : g.cons.erreur.c_str());
        if (t == Tache::Shaders) {   // même en cas d'échec : le jeu compile alors ses shaders au besoin
            if (!g.cons.annulee && !g.sans_jeu) lancer_jeu();
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
            const wchar_t* q = g.langue == Langue::Fr ? L"Une tâche est en cours (construction, shaders ou mise à jour). L'arrêter et quitter ?"
                                                      : L"A task is running (build, shaders or update). Stop it and quit?";
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
        else if (!wcscmp(p, L"construction")) g.page = Page::Construction;
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
        dessin_image();   // Present attend le balayage de l'écran : ≈ 60 images par seconde
    }
    video_arreter();
    annuler_construction();
    dessin_liberer();
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}
