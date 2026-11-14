// Manettes dans le launcher (page Contrôles, onglet Manette, 2026-11-12) : SDL3.dll, la même que le jeu (outils\sdl3,
// launcher/outils.py), chargée à l'exécution quand l'onglet s'ouvre ; SDL est arrêté quand l'onglet se ferme ou que le
// jeu démarre, pour que les manettes soient au jeu seul. Sert à voir la manette réagir sur le dessin de l'onglet, et à
// donner un bouton à une action en l'appuyant. Les entrées sont repérées par leur position, comme dans le jeu
// (runtime/os/input.cpp : mêmes indices, EntreePad = Settings::P_*).
#include "lanceur.h"

#include <type_traits>

namespace lz {
namespace {

// ---------------------------------------------------------------- SDL3 (prototypes de l'ABI stable de SDL 3)
namespace sdl {
struct Gamepad;
typedef uint32_t JoystickID;
constexpr uint32_t INIT_GAMEPAD = 0x2000;
constexpr int LEFT_TRIGGER = 4, RIGHT_TRIGGER = 5;   // SDL_GamepadAxis
constexpr int BOUTONS = 21;                         // SDL_GAMEPAD_BUTTON_SOUTH … TOUCHPAD
bool (*Init)(uint32_t);
void (*Quit)();
bool (*SetHint)(const char*, const char*);
int (*GetVersion)();
void (*PumpEvents)();
void (*UpdateGamepads)();
JoystickID* (*GetGamepads)(int*);
Gamepad* (*OpenGamepad)(JoystickID);
void (*CloseGamepad)(Gamepad*);
bool (*GetGamepadButton)(Gamepad*, int);
int16_t (*GetGamepadAxis)(Gamepad*, int);
const char* (*GetGamepadName)(Gamepad*);
int (*GetGamepadType)(Gamepad*);
void (*Free)(void*);
}  // namespace sdl

struct Pad {
    sdl::Gamepad* g = nullptr;
    sdl::JoystickID id = 0;
};
EtatManettes g_etat;
HMODULE g_lib = nullptr;
bool g_tente = false;   // chargement de SDL3.dll essayé (une fois par lancement du launcher)
bool g_init = false;    // SDL_Init fait (onglet ouvert)
bool g_refus = false;   // SDL_Init a échoué : pas de nouvel essai avant la fermeture de l'onglet
std::vector<Pad> g_pads;
unsigned g_tour = 0;

// SDL3.dll : à côté du jeu, puis dans les outils du paquet (<racine>\outils\sdl3), puis dans ceux des sources
// (recomp\outils\sdl3), comme le jeu (input.cpp, chercher_sdl)
bool charger() {
    if (g_tente) return g_lib != nullptr;
    g_tente = true;
    for (const std::wstring& c : {g.jeu + L"\\SDL3.dll", g.dossier_exe + L"\\outils\\sdl3\\SDL3.dll",
                                  g.recomp + L"\\outils\\sdl3\\SDL3.dll"}) {
        if (GetFileAttributesW(c.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        g_lib = LoadLibraryExW(c.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (g_lib) {
            jlog(L"controllers: SDL3 loaded from %ls", c.c_str());
            break;
        }
    }
    if (!g_lib) {
        jlog(L"controllers: SDL3.dll not found (outils\\sdl3): buttons changed with the arrows only");
        return false;
    }
    bool ok = true;
    auto f = [&](auto& p, const char* n) {
        p = reinterpret_cast<std::remove_reference_t<decltype(p)>>((void*)GetProcAddress(g_lib, n));
        ok = ok && p;
    };
    f(sdl::Init, "SDL_Init");
    f(sdl::Quit, "SDL_Quit");
    f(sdl::SetHint, "SDL_SetHint");
    f(sdl::GetVersion, "SDL_GetVersion");
    f(sdl::PumpEvents, "SDL_PumpEvents");
    f(sdl::UpdateGamepads, "SDL_UpdateGamepads");
    f(sdl::GetGamepads, "SDL_GetGamepads");
    f(sdl::OpenGamepad, "SDL_OpenGamepad");
    f(sdl::CloseGamepad, "SDL_CloseGamepad");
    f(sdl::GetGamepadButton, "SDL_GetGamepadButton");
    f(sdl::GetGamepadAxis, "SDL_GetGamepadAxis");
    f(sdl::GetGamepadName, "SDL_GetGamepadName");
    f(sdl::GetGamepadType, "SDL_GetGamepadType");
    f(sdl::Free, "SDL_free");
    if (!ok || sdl::GetVersion() / 1000000 != 3) {
        jlog(L"controllers: SDL3.dll not usable (missing functions or not version 3)");
        FreeLibrary(g_lib);
        g_lib = nullptr;
        return false;
    }
    return true;
}

// SDL_GamepadType → famille de la manette (noms des boutons)
int famille(int type) {
    if (type == 2 || type == 3) return 1;   // Xbox 360, Xbox One / Series
    if (type >= 4 && type <= 6) return 2;   // PS3, PS4, PS5
    if (type >= 7 && type <= 11) return 3;  // Switch Pro, Joy-Con (gauche, droit, paire), GameCube
    return 0;
}

// Manettes branchées ou débranchées : ouvertes et fermées ici ; la première donne le nom et la famille affichés
void suivre() {
    int n = 0;
    sdl::JoystickID* ids = sdl::GetGamepads(&n);
    std::vector<sdl::JoystickID> vus;
    if (ids) {
        vus.assign(ids, ids + n);
        sdl::Free(ids);
    }
    for (size_t i = 0; i < g_pads.size();) {
        bool la = false;
        for (sdl::JoystickID id : vus) la |= id == g_pads[i].id;
        if (la) {
            i++;
            continue;
        }
        sdl::CloseGamepad(g_pads[i].g);
        g_pads.erase(g_pads.begin() + (std::ptrdiff_t)i);
    }
    for (sdl::JoystickID id : vus) {
        bool deja = false;
        for (const Pad& p : g_pads) deja |= p.id == id;
        if (deja) continue;
        Pad p;
        p.g = sdl::OpenGamepad(id);
        if (!p.g) continue;
        p.id = id;
        g_pads.push_back(p);
        const char* nom = sdl::GetGamepadName(p.g);
        jlog(L"controllers: %ls connected (type %d)", wide(nom ? nom : "?").c_str(), sdl::GetGamepadType(p.g));
    }
    g_etat.nb = (int)g_pads.size();
    g_etat.nom.clear();
    g_etat.famille = 0;
    g_etat.type = -1;
    if (!g_pads.empty()) {
        const char* nom = sdl::GetGamepadName(g_pads[0].g);
        g_etat.nom = wide(nom ? nom : "?");
        g_etat.type = sdl::GetGamepadType(g_pads[0].g);
        g_etat.famille = famille(g_etat.type);
    }
}

}  // namespace

const EtatManettes& manettes() { return g_etat; }

void manettes_lire() {
    if (!g_init) {
        if (g_refus || !charger()) {
            g_etat = EtatManettes{};
            return;
        }
        // comme le jeu (input.cpp) : pas de fenêtre SDL, détection des branchements dans un fil de SDL ; Joy-Con réunis
        // par paire, un Joy-Con seul à l'horizontale
        sdl::SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
        sdl::SetHint("SDL_JOYSTICK_THREAD", "1");
        sdl::SetHint("SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS", "1");
        sdl::SetHint("SDL_JOYSTICK_HIDAPI_VERTICAL_JOY_CONS", "0");
        if (!sdl::Init(sdl::INIT_GAMEPAD)) {
            jlog(L"controllers: SDL_Init failed");
            g_refus = true;
            g_etat = EtatManettes{};
            return;
        }
        g_init = true;
        g_tour = 0;
    }
    sdl::PumpEvents();
    if (g_tour++ % 15 == 0) suivre();   // branchements : quatre fois par seconde
    sdl::UpdateGamepads();
    uint32_t m = 0;
    for (const Pad& p : g_pads) {
        for (int b = 0; b < sdl::BOUTONS; b++)
            if (sdl::GetGamepadButton(p.g, b)) m |= 1u << (P_SUD + b);
        if (sdl::GetGamepadAxis(p.g, sdl::LEFT_TRIGGER) > 8000) m |= 1u << P_GACHETTE_G;   // quart de course, comme le jeu
        if (sdl::GetGamepadAxis(p.g, sdl::RIGHT_TRIGGER) > 8000) m |= 1u << P_GACHETTE_D;
    }
    g_etat.sdl = true;
    g_etat.appuyees = m;
    // sticks de la première manette (le dessin les fait bouger), petite zone morte
    for (int a = 0; a < 4; a++) {
        const float v = g_pads.empty() ? 0.f : sdl::GetGamepadAxis(g_pads[0].g, a) / 32767.f;
        g_etat.axes[a] = v > -0.08f && v < 0.08f ? 0.f : (v < -1.f ? -1.f : v > 1.f ? 1.f : v);
    }
}

void manettes_fermer() {
    g_refus = false;
    if (!g_init) return;
    for (const Pad& p : g_pads) sdl::CloseGamepad(p.g);
    g_pads.clear();
    sdl::Quit();   // SDL3.dll reste chargée : l'onglet peut se rouvrir
    g_init = false;
    g_etat = EtatManettes{};
}

std::wstring nom_entree(int e, int f) {
    const bool fr = g.langue == Langue::Fr;
    // familles : 0 inconnue (Xbox / PlayStation), 1 Xbox, 2 PlayStation, 3 Nintendo
    auto choix = [f](const wchar_t* x, const wchar_t* ps, const wchar_t* n, const wchar_t* deux) {
        return std::wstring(f == 1 ? x : f == 2 ? ps : f == 3 ? n : deux);
    };
    switch (e) {
    case P_AUCUNE: return fr ? L"Aucun" : L"None";
    case P_SUD: return choix(L"A", L"✕", L"B", L"A / ✕");
    case P_EST: return choix(L"B", L"○", L"A", L"B / ○");
    case P_OUEST: return choix(L"X", L"□", L"Y", L"X / □");
    case P_NORD: return choix(L"Y", L"△", L"X", L"Y / △");
    case P_BACK: return choix(L"Back", L"Share", L"−", L"Back / Share");
    case P_GUIDE: return choix(L"Xbox", L"PS", L"Home", L"Xbox / PS");
    case P_START: return choix(L"Start", L"Options", L"+", L"Start / Options");
    case P_STICK_G: return choix(L"LS", L"L3", fr ? L"Stick L" : L"L stick", L"LS / L3");
    case P_STICK_D: return choix(L"RS", L"R3", fr ? L"Stick R" : L"R stick", L"RS / R3");
    case P_EPAULE_G: return choix(L"LB", L"L1", L"L", L"LB / L1");
    case P_EPAULE_D: return choix(L"RB", L"R1", L"R", L"RB / R1");
    case P_HAUT: return fr ? L"Croix ↑" : L"D-pad ↑";
    case P_BAS: return fr ? L"Croix ↓" : L"D-pad ↓";
    case P_GAUCHE: return fr ? L"Croix ←" : L"D-pad ←";
    case P_DROITE: return fr ? L"Croix →" : L"D-pad →";
    case P_MISC1: return choix(L"Share", fr ? L"Micro" : L"Mic", L"Capture", L"Share / Capture");
    case P_PALETTE_D1: return f == 1 ? L"P1" : fr ? L"Palette D1" : L"Paddle R1";
    case P_PALETTE_G1: return f == 1 ? L"P3" : fr ? L"Palette G1" : L"Paddle L1";
    case P_PALETTE_D2: return f == 1 ? L"P2" : fr ? L"Palette D2" : L"Paddle R2";
    case P_PALETTE_G2: return f == 1 ? L"P4" : fr ? L"Palette G2" : L"Paddle L2";
    case P_PAVE: return fr ? L"Pavé tactile" : L"Touchpad";
    case P_GACHETTE_G: return choix(L"LT", L"L2", L"ZL", L"LT / L2");
    case P_GACHETTE_D: return choix(L"RT", L"R2", L"ZR", L"RT / R2");
    default: return L"?";
    }
}

}  // namespace lz
