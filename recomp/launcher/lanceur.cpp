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

namespace {

constexpr UINT WM_JEU_FINI = WM_APP + 1, WM_CONS_FINIE = WM_APP + 2;
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

// icône du jeu (build/full/icone.ico, tirée de la copie du joueur par preparer.py) : fenêtre et barre des tâches.
// Le fichier lanceur.exe garde la sienne : il est distribué, il ne contient rien du jeu.
void icone_jeu() {
    const std::wstring ico = g.jeu + L"\\icone.ico";
    if (!g.hwnd || !existe(ico)) return;
    const UINT dpi = GetDpiForWindow(g.hwnd);
    const struct { WPARAM genre; int cx, cy; } tailles[] = {
        {ICON_BIG, GetSystemMetricsForDpi(SM_CXICON, dpi), GetSystemMetricsForDpi(SM_CYICON, dpi)},
        {ICON_SMALL, GetSystemMetricsForDpi(SM_CXSMICON, dpi), GetSystemMetricsForDpi(SM_CYSMICON, dpi)}};
    for (const auto& t : tailles)
        if (HICON i = (HICON)LoadImageW(nullptr, ico.c_str(), IMAGE_ICON, t.cx, t.cy, LR_LOADFROMFILE))
            if (HICON avant = (HICON)SendMessageW(g.hwnd, WM_SETICON, t.genre, (LPARAM)i)) DestroyIcon(avant);
}

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
    }
}

void reglages_ecrire() {
    std::vector<std::string> v = lignes_fichier(recomp_ini());
    if (v.empty())
        v = {"; Réglages de recomp_main.exe, écrits par le launcher (ils peuvent aussi être modifiés à la main).", "",
             "[display]"};
    const std::pair<std::string, std::string> cles[] = {
        {"refresh_rate", std::to_string(g.reglages.hz)},
        {"resolution", g.reglages.resolution ? std::to_string(g.reglages.resolution) : "auto"},
        {"native_resolution", g.reglages.natif ? "true" : "false"},
        {"window_mode", g.reglages.sans_bordure ? "borderless" : "windowed"},
        {"language", g.reglages.langue},
    };
    for (auto& [k, val] : cles) {
        bool fait = false;
        for (std::string& l : v) {
            const std::string t = trim(l);
            if (t.rfind(k, 0) == 0 && t.size() > k.size() && (t[k.size()] == ' ' || t[k.size()] == '=')) {
                l = k + " = " + val;
                fait = true;
            }
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

void lancer_construction(bool forcer, bool extraits_seuls) {
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
    if (extraits_seuls) cmd += L" --etapes verifier,extraits";
    SHCreateDirectoryExW(nullptr, g.jeu.c_str(), nullptr);
    // compilation en priorité basse : le PC reste utilisable
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
    }).detach();
}

void lancer_mise_a_jour() {
    if (g.cons.active || g.jeu_lance) return;
    const std::wstring cmd = commande_python(g.recomp + L"\\launcher\\maj.py");
    if (cmd.empty()) return;
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
    const std::wstring exe = g.jeu + L"\\recomp_main.exe";
    if (!lancer_tache(Tache::Shaders, L"\"" + exe + L"\" --precompile", g.jeu, L"", NORMAL_PRIORITY_CLASS)) lancer_jeu();
}

void lancer_jeu() {
    const std::wstring exe = g.jeu + L"\\recomp_main.exe";
    std::wstring cmd = L"\"" + exe + L"\"";
    // le jeu réécrit recomp_run.log : le journal de la partie précédente est gardé à côté
    const std::wstring journal = g.jeu + L"\\recomp_run.log";
    if (existe(journal)) MoveFileExW(journal.c_str(), (g.jeu + L"\\recomp_run_precedent.log").c_str(), MOVEFILE_REPLACE_EXISTING);
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
        return;
    }
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

void basculer(Page p) { g.page = g.page == p ? Page::Aucune : p; }

void action(int z) {
    switch (z) {
    case Z_PRINCIPAL:
        if (g.jeu_lance) return;
        if (g.cons.active) {
            if (g.cons.tache == Tache::Construction) basculer(Page::Construction);
            return;
        }
        if (g.jeu_pret && !g.jeu_obsolete) return jouer();
        if (!g.copie_ok) choisir_copie();
        if (g.copie_ok) lancer_construction(false, false);   // après une mise à jour : seules les étapes changées
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
    case Z_RECONSTRUIRE: lancer_construction(true, false); return;
    case Z_EXTRAITS: video_arreter(); lancer_construction(true, true); return;
    case Z_ANNULER: annuler_construction(); return;
    case Z_JOURNAL: ShellExecuteW(g.hwnd, L"open", (g.jeu + L"\\preparation.log").c_str(), nullptr, nullptr, SW_SHOWNORMAL); return;
    }
}

// ---------------------------------------------------------------- fenêtre
float dip(int px) { return px * 96.f / g.dpi; }

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
        if (z != Z_AUCUNE && z == a) action(z);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_RETURN) action(Z_PRINCIPAL);
        else if (w == VK_ESCAPE) g.page = Page::Aucune;
        return 0;
    case WM_DPICHANGED: {
        g.dpi = (float)HIWORD(w);
        const RECT* r = (const RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        dessin_redim();
        return 0;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) dessin_redim();
        return 0;
    case WM_JEU_FINI:
        g.jeu_lance = false;
        if ((int)w != 0) {
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
        icone_jeu();
        reglages_lire();
        fond_relancer();
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
    g.dpi = (float)GetDpiForSystem();
    const int w = (int)lround(LARGEUR * g.dpi / 96), hh = (int)lround(HAUTEUR * g.dpi / 96);
    RECT zt;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &zt, 0);
    g.hwnd = CreateWindowExW(WS_EX_APPWINDOW, CLASSE, NOM, WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU,
                             zt.left + (zt.right - zt.left - w) / 2, zt.top + (zt.bottom - zt.top - hh) / 2, w, hh, nullptr, nullptr,
                             inst, nullptr);
    if (!g.hwnd) return 1;
    const float d = (float)GetDpiForWindow(g.hwnd);
    if (d != g.dpi) {
        g.dpi = d;
        SetWindowPos(g.hwnd, nullptr, 0, 0, (int)lround(LARGEUR * d / 96), (int)lround(HAUTEUR * d / 96), SWP_NOMOVE | SWP_NOZORDER);
    }
    icone_jeu();
    verifier_mises_a_jour();
    const DWORD coins = 1;   // DWMWCP_DONOTROUND : angles droits, comme le cadre dessiné
    DwmSetWindowAttribute(g.hwnd, 33, &coins, sizeof coins);
    const MARGINS ombre{0, 0, 0, 1};   // ombre de fenêtre (DWM) malgré WS_POPUP
    DwmExtendFrameIntoClientArea(g.hwnd, &ombre);
    if (!dessin_init(g.hwnd)) {
        MessageBoxW(nullptr, L"Direct2D / Direct3D 11 indisponible.", NOM, MB_ICONERROR);
        return 1;
    }
    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);
    fond_relancer();
    // vérification de l'interface : lance une construction (jamais le jeu), avec un autre pilote (LANCEUR_PREPARER)
    if (const wchar_t* a = _wgetenv(L"LANCEUR_ACTION")) {
        if (!wcscmp(a, L"construire") && _wgetenv(L"LANCEUR_PREPARER")) {
            lancer_construction(false, false);
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
