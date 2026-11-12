// Entrées hôte pour la manette émulée (svc_hid.cpp) : clavier et manettes, fusionnés, lus seulement si la fenêtre du
// jeu a le focus. Manettes : SDL3 (SDL3.dll, chargée dynamiquement, fournie avec le paquet) reconnaît les manettes
// Xbox, PlayStation 4 et 5 (DualShock 4, DualSense), Pro de la Switch et bien d'autres, branchées ou en Bluetooth ;
// sans SDL3.dll, première manette XInput (xinput1_4.dll, Xbox seulement). Toutes les manettes branchées pilotent le
// joueur 1. Les vibrations du jeu (nn::hid::SendVibrationValue, svc_hid.cpp) passent par SDL_RumbleGamepad ou
// XInputSetState.
//
// Clavier : une touche par action (settings().keys, clés key_… de recomp.ini, changées depuis la page Contrôles du
// launcher), par position des touches (scancodes). Sans réglage, les positions de toujours, identiques en QWERTY et
// en AZERTY :
//   stick gauche   W A S D   (AZERTY : Z Q S D)        stick droit  I J K L
//   croix          flèches                              clics sticks F (gauche), H (droit)
//   A  C      B  X      X  V      Y  Z (AZERTY : W)
//   L  Q (AZERTY : A)   R  E      ZL  R      ZR  T      +  M (AZERTY : ,)      -  N
// En plus, pour les menus : Entrée = A, Retour arrière = B, Échap = +, Tab = -, tant que le joueur n'a pas donné
// cette touche à une autre action.
// Manettes, disposition par position (comme une manette Nintendo) : bouton du bas = B, droite = A, gauche = Y,
// haut = X ; LB/RB (L1/R1) = L/R ; gâchettes = ZL/ZR ; Start/Options/+ = +, Back/Share/Create/- = -.
// Sur une manette Pro de la Switch, chaque bouton fait donc ce qui est écrit dessus.
#include "gfx.h"
#include "settings.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <xinput.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <string>
#include <vector>

namespace hos {
namespace {

enum : uint64_t {
    BTN_A = 1ull << 0, BTN_B = 1ull << 1, BTN_X = 1ull << 2, BTN_Y = 1ull << 3,
    BTN_LSTICK = 1ull << 4, BTN_RSTICK = 1ull << 5, BTN_L = 1ull << 6, BTN_R = 1ull << 7,
    BTN_ZL = 1ull << 8, BTN_ZR = 1ull << 9, BTN_PLUS = 1ull << 10, BTN_MINUS = 1ull << 11,
    BTN_LEFT = 1ull << 12, BTN_UP = 1ull << 13, BTN_RIGHT = 1ull << 14, BTN_DOWN = 1ull << 15,
    // directions des sticks (16-19 gauche, 20-23 droit : gauche, haut, droite, bas)
    BTN_LS_LEFT = 1ull << 16, BTN_RS_LEFT = 1ull << 20,
};
constexpr int32_t STICK_MAX = 0x7FFF;

typedef DWORD(WINAPI* XInputGetStateFn)(DWORD, XINPUT_STATE*);
typedef DWORD(WINAPI* XInputSetStateFn)(DWORD, XINPUT_VIBRATION*);
XInputGetStateFn g_xget = nullptr;
XInputSetStateFn g_xset = nullptr;
std::once_flag g_once;

// ---------------------------------------------------------------- SDL3 (prototypes de l'ABI stable de SDL 3)
namespace sdl {
struct Gamepad;
typedef uint32_t JoystickID;
constexpr uint32_t INIT_GAMEPAD = 0x2000;
enum Button {
    SOUTH, EAST, WEST, NORTH, BACK, GUIDE, START, LEFT_STICK, RIGHT_STICK, LEFT_SHOULDER, RIGHT_SHOULDER,
    DPAD_UP, DPAD_DOWN, DPAD_LEFT, DPAD_RIGHT, MISC1, RIGHT_PADDLE1, LEFT_PADDLE1, RIGHT_PADDLE2, LEFT_PADDLE2, TOUCHPAD,
};
enum Axis { LEFTX, LEFTY, RIGHTX, RIGHTY, LEFT_TRIGGER, RIGHT_TRIGGER };
constexpr int TYPE_SWITCH_PRO = 7;
const char* const kTypes[] = {"unknown", "standard", "Xbox 360", "Xbox One/Series", "PS3", "PS4", "PS5", "Switch Pro",
                              "left Joy-Con", "right Joy-Con", "Joy-Con pair", "GameCube"};
bool (*Init)(uint32_t);
bool (*SetHint)(const char*, const char*);
const char* (*GetError)();
int (*GetVersion)();
void (*PumpEvents)();
void (*UpdateGamepads)();
JoystickID* (*GetGamepads)(int*);
Gamepad* (*OpenGamepad)(JoystickID);
void (*CloseGamepad)(Gamepad*);
bool (*GamepadConnected)(Gamepad*);
bool (*GetGamepadButton)(Gamepad*, int);
int16_t (*GetGamepadAxis)(Gamepad*, int);
bool (*RumbleGamepad)(Gamepad*, uint16_t, uint16_t, uint32_t);
const char* (*GetGamepadName)(Gamepad*);
int (*GetGamepadType)(Gamepad*);
void (*Free)(void*);
}  // namespace sdl

struct Pad {
    sdl::Gamepad* g = nullptr;
    sdl::JoystickID id = 0;
    std::string nom;
    int type = 0;
    uint16_t vib[2] = {0, 0};   // dernière vibration envoyée (bande basse, bande haute)
    uint64_t t_vib = 0;
};
bool g_sdl = false;
std::vector<Pad> g_pads;

std::string utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// SDL3.dll : à côté de l'exécutable, puis dans les outils du paquet (<racine>\outils\sdl3), puis dans ceux des sources
// (<racine>\recomp\outils\sdl3). L'exécutable est dans <racine>\recomp\build\full.
std::wstring chercher_sdl() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring racine = dir;
    for (int i = 0; i < 3 && racine.find_last_of(L"\\/") != std::wstring::npos; i++)
        racine = racine.substr(0, racine.find_last_of(L"\\/"));
    for (const std::wstring& c : {dir + L"\\SDL3.dll", racine + L"\\outils\\sdl3\\SDL3.dll",
                                  racine + L"\\recomp\\outils\\sdl3\\SDL3.dll"})
        if (GetFileAttributesW(c.c_str()) != INVALID_FILE_ATTRIBUTES) return c;
    return L"";
}

bool charger_sdl() {
    if (getenv("RECOMP_NO_SDL")) {
        HLOG("input", "SDL3 disabled (RECOMP_NO_SDL): XInput controller only");
        return false;
    }
    const std::wstring chemin = chercher_sdl();
    HMODULE m = chemin.empty() ? nullptr : LoadLibraryExW(chemin.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        HLOG("input", "SDL3.dll %s: PlayStation and Switch Pro controllers not recognized, XInput controller only",
             chemin.empty() ? "not found (neither next to the game nor in outils\\sdl3)" : "refused at load time");
        return false;
    }
    bool ok = true;
    auto f = [&](auto& ptr, const char* nom) {
        ptr = reinterpret_cast<std::remove_reference_t<decltype(ptr)>>((void*)GetProcAddress(m, nom));
        if (!ptr) {
            HLOG("input", "SDL3.dll: function %s missing", nom);
            ok = false;
        }
    };
    f(sdl::Init, "SDL_Init");
    f(sdl::SetHint, "SDL_SetHint");
    f(sdl::GetError, "SDL_GetError");
    f(sdl::GetVersion, "SDL_GetVersion");
    f(sdl::PumpEvents, "SDL_PumpEvents");
    f(sdl::UpdateGamepads, "SDL_UpdateGamepads");
    f(sdl::GetGamepads, "SDL_GetGamepads");
    f(sdl::OpenGamepad, "SDL_OpenGamepad");
    f(sdl::CloseGamepad, "SDL_CloseGamepad");
    f(sdl::GamepadConnected, "SDL_GamepadConnected");
    f(sdl::GetGamepadButton, "SDL_GetGamepadButton");
    f(sdl::GetGamepadAxis, "SDL_GetGamepadAxis");
    f(sdl::RumbleGamepad, "SDL_RumbleGamepad");
    f(sdl::GetGamepadName, "SDL_GetGamepadName");
    f(sdl::GetGamepadType, "SDL_GetGamepadType");
    f(sdl::Free, "SDL_free");
    if (!ok) return false;
    const int v = sdl::GetVersion();
    if (v / 1000000 != 3) {
        HLOG("input", "SDL3.dll: unexpected version %d, skipped", v);
        return false;
    }
    // Aucune fenêtre SDL : la manette est lue même sans « focus » SDL (le jeu filtre lui-même sur sa fenêtre), et la
    // détection des branchements tourne dans un fil de SDL (pas de boucle d'événements SDL dans le jeu).
    sdl::SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");
    sdl::SetHint("SDL_JOYSTICK_THREAD", "1");
    if (!sdl::Init(sdl::INIT_GAMEPAD)) {
        HLOG("input", "SDL_Init (controllers) failed: %s; XInput controller only", sdl::GetError());
        return false;
    }
    HLOG("input", "SDL %d.%d.%d loaded (%s): Xbox, PlayStation 4 and 5, Switch Pro controllers…", v / 1000000,
         v / 1000 % 1000, v % 1000, utf8(chemin).c_str());
    return true;
}

// Manettes branchées ou débranchées (toutes les 0,5 s) : chaque changement est noté dans le journal.
void suivre_pads() {
    int n = 0;
    sdl::JoystickID* ids = sdl::GetGamepads(&n);
    std::vector<sdl::JoystickID> vus;
    if (ids) {
        vus.assign(ids, ids + n);
        sdl::Free(ids);
    }
    for (size_t i = 0; i < g_pads.size();) {
        Pad& p = g_pads[i];
        if (std::find(vus.begin(), vus.end(), p.id) == vus.end() || !sdl::GamepadConnected(p.g)) {
            HLOG("input", "controller disconnected: %s", p.nom.c_str());
            sdl::CloseGamepad(p.g);
            g_pads.erase(g_pads.begin() + i);
        } else {
            i++;
        }
    }
    for (sdl::JoystickID id : vus) {
        if (std::any_of(g_pads.begin(), g_pads.end(), [&](const Pad& p) { return p.id == id; })) continue;
        Pad p;
        p.g = sdl::OpenGamepad(id);
        if (!p.g) {
            HLOG("input", "controller %u: cannot open (%s)", id, sdl::GetError());
            continue;
        }
        p.id = id;
        const char* nom = sdl::GetGamepadName(p.g);
        p.nom = nom ? nom : "?";
        p.type = sdl::GetGamepadType(p.g);
        const bool vib = sdl::RumbleGamepad(p.g, 0, 0, 0);
        HLOG("input", "controller connected: %s (type %s, rumble %s)", p.nom.c_str(),
             p.type >= 0 && p.type < (int)(sizeof sdl::kTypes / sizeof *sdl::kTypes) ? sdl::kTypes[p.type] : "?",
             vib ? "yes" : "no");
        g_pads.push_back(p);
    }
}

void init() {
    g_sdl = charger_sdl();
    if (!g_sdl) {
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
        if (m) {
            g_xget = (XInputGetStateFn)(void*)GetProcAddress(m, "XInputGetState");
            g_xset = (XInputSetStateFn)(void*)GetProcAddress(m, "XInputSetState");
        }
    } else {
        suivre_pads();
    }
    HLOG("input", "inputs: keyboard%s, read when the window is active%s; rumble %d %%",
         g_sdl ? " + controllers (SDL3)" : g_xget ? " + XInput controller" : "",
         getenv("RECOMP_BACKGROUND_INPUT") ? " (controller also in the background: RECOMP_BACKGROUND_INPUT)" : "",
         settings().vibration);
}

// ---------------------------------------------------------------- vibrations
// Dernière valeur envoyée par le jeu pour chaque côté de la manette (0 gauche, 1 droite) : amplitude et fréquence des
// bandes basse (≈ 160 Hz) et haute (≈ 320 Hz) de la vibration HD. Une manette à deux moteurs reçoit la bande basse sur
// son gros moteur (basse fréquence) et la bande haute sur le petit ; SDL fait de même avec les deux moteurs linéaires
// de la manette Pro. Les fréquences exactes de la vibration HD ne passent pas : SDL n'en donne pas l'accès.
std::mutex g_vib_lk;
float g_vib[2][4] = {};
uint64_t g_vib_n = 0;

uint16_t amplitude(float a, bool lineaire) {
    if (!(a > 0.0f)) return 0;
    a = std::min(a, 1.0f);
    // moteurs classiques : courbe relevée, sans quoi les faibles amplitudes de la vibration HD seraient imperceptibles
    const float v = lineaire ? a : 0.5f * (a + std::pow(a, 0.35f));
    return (uint16_t)std::min(65535.0f, v * 65535.0f * (float)settings().vibration / 100.0f);
}

void appliquer_vibrations(bool actif) {
    float v[2][4];
    {
        std::lock_guard<std::mutex> lk(g_vib_lk);
        memcpy(v, g_vib, sizeof v);
    }
    if (!actif || settings().vibration <= 0) memset(v, 0, sizeof v);   // fenêtre en arrière-plan : rien
    const float bas = std::max(v[0][0], v[1][0]), haut = std::max(v[0][2], v[1][2]);
    const uint64_t t = GetTickCount64();
    if (g_sdl) {
        for (Pad& p : g_pads) {
            const bool pro = p.type == sdl::TYPE_SWITCH_PRO;
            const uint16_t b = amplitude(bas, pro), h = amplitude(haut, pro);
            // renvoyée à chaque changement, et toutes les 150 ms tant qu'elle dure (durée 250 ms : elle s'arrête
            // seule si le jeu se fige)
            if (b != p.vib[0] || h != p.vib[1] || ((b || h) && t - p.t_vib >= 150)) {
                sdl::RumbleGamepad(p.g, b, h, 250);
                p.vib[0] = b, p.vib[1] = h, p.t_vib = t;
            }
        }
        return;
    }
    static uint16_t avant[2] = {0, 0};
    if (g_xset) {
        XINPUT_VIBRATION x{amplitude(bas, false), amplitude(haut, false)};
        if (x.wLeftMotorSpeed != avant[0] || x.wRightMotorSpeed != avant[1]) {
            g_xset(0, &x);
            avant[0] = x.wLeftMotorSpeed, avant[1] = x.wRightMotorSpeed;
        }
    }
}

bool key_sc(UINT sc) {   // sc : scancode, 0xE0xx pour une touche étendue (flèches…) ; 0 : aucune touche
    if (!sc) return false;
    // pavé numérique (scancodes non étendus 0x47 à 0x53) : selon le verrouillage numérique, Windows rapporte la touche en
    // chiffre (VK_NUMPAD8) ou en déplacement (VK_UP) ; les deux comptent
    static const int pave[13] = {VK_NUMPAD7, VK_NUMPAD8, VK_NUMPAD9, 0, VK_NUMPAD4, VK_NUMPAD5, VK_NUMPAD6, 0,
                                 VK_NUMPAD1, VK_NUMPAD2, VK_NUMPAD3, VK_NUMPAD0, VK_DECIMAL};
    if (sc >= 0x47 && sc <= 0x53 && pave[sc - 0x47] && (GetAsyncKeyState(pave[sc - 0x47]) & 0x8000)) return true;
    UINT vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
    return vk && (GetAsyncKeyState((int)vk) & 0x8000);
}
bool key_vk(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

int32_t axis(bool neg, bool pos) { return (pos ? STICK_MAX : 0) - (neg ? STICK_MAX : 0); }

int32_t dead(int32_t v, int32_t dz) {
    if (v > -dz && v < dz) return 0;
    return std::max(-STICK_MAX, std::min(STICK_MAX, v));
}

// bits de direction déduits d'un stick (seuil à mi-course)
uint64_t stick_dirs(int32_t x, int32_t y, uint64_t base) {
    constexpr int32_t T = STICK_MAX / 2;
    uint64_t b = 0;
    if (x < -T) b |= base << 0;
    if (y > T) b |= base << 1;
    if (x > T) b |= base << 2;
    if (y < -T) b |= base << 3;
    return b;
}

// RECOMP_KEY_SCRIPT="40:A,44:-A,50.5:PLUS,50.6:-PLUS" : boutons appuyés (nom) puis relâchés (-nom) aux
// instants donnés, en secondes depuis la première lecture des entrées (comme EDEN_NX2_KEY_SCRIPT de
// Lichii, avec des noms de boutons). Ex. : tenir A 4 s pendant une cinématique la passe.
uint64_t key_script_buttons(uint64_t ms) {
    struct Ev { uint64_t ms; uint64_t bit; bool down; };
    static const std::vector<Ev> evs = [] {
        std::vector<Ev> v;
        const char* s = getenv("RECOMP_KEY_SCRIPT");
        if (!s) return v;
        static const struct { const char* n; uint64_t b; } names[] = {
            {"A", BTN_A}, {"B", BTN_B}, {"X", BTN_X}, {"Y", BTN_Y}, {"L", BTN_L}, {"R", BTN_R}, {"ZL", BTN_ZL},
            {"ZR", BTN_ZR}, {"PLUS", BTN_PLUS}, {"MINUS", BTN_MINUS}, {"LEFT", BTN_LEFT}, {"UP", BTN_UP},
            {"RIGHT", BTN_RIGHT}, {"DOWN", BTN_DOWN}, {"LSTICK", BTN_LSTICK}, {"RSTICK", BTN_RSTICK}};
        std::string all(s), item;
        size_t pos = 0;
        while (pos <= all.size()) {
            size_t c = all.find(',', pos);
            item = all.substr(pos, c == std::string::npos ? std::string::npos : c - pos);
            pos = c == std::string::npos ? all.size() + 1 : c + 1;
            const size_t colon = item.find(':');
            if (colon == std::string::npos) continue;
            const double t = atof(item.substr(0, colon).c_str());
            std::string name = item.substr(colon + 1);
            const bool down = name.empty() || name[0] != '-';
            if (!down) name = name.substr(1);
            for (auto& n : names)
                if (name == n.n) v.push_back({(uint64_t)(t * 1000.0), n.b, down});
        }
        std::sort(v.begin(), v.end(), [](const Ev& a, const Ev& b) { return a.ms < b.ms; });
        HLOG("input", "RECOMP_KEY_SCRIPT: %zu events", v.size());
        return v;
    }();
    uint64_t held = 0;
    for (const Ev& e : evs) {
        if (e.ms > ms) break;
        held = e.down ? (held | e.bit) : (held & ~e.bit);
    }
    return held;
}

}  // namespace

void host_set_vibration(int side, float amp_low, float freq_low, float amp_high, float freq_high) {
    if (side < 0 || side > 1) return;
    std::lock_guard<std::mutex> lk(g_vib_lk);
    g_vib[side][0] = std::isfinite(amp_low) ? amp_low : 0.0f;
    g_vib[side][1] = freq_low;
    g_vib[side][2] = std::isfinite(amp_high) ? amp_high : 0.0f;
    g_vib[side][3] = freq_high;
    // la première valeur reçue est notée ; RECOMP_INPUT_LOG=1 : les 20 premières
    static const bool log = getenv("RECOMP_INPUT_LOG") != nullptr;
    if (g_vib_n++ < 20 && (log || g_vib_n == 1))
        HLOG("input", "rumble %s: low %.2f at %.0f Hz, high %.2f at %.0f Hz%s", side ? "right" : "left", amp_low,
             freq_low, amp_high, freq_high, g_vib_n == 1 && !log ? " (first value from the game)" : "");
}

HostPad host_poll_input() {
    std::call_once(g_once, init);
    HostPad p;
    if (gfx_window_focused()) {
        const unsigned* K = settings().keys;   // une touche par action (recomp.ini, page Contrôles du launcher)
        static const uint64_t bits[] = {BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_ZL, BTN_ZR, BTN_PLUS, BTN_MINUS,
                                        BTN_LSTICK, BTN_RSTICK, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT};
        for (int i = 0; i <= Settings::K_RIGHT; i++)
            if (key_sc(K[i])) p.buttons |= bits[i];
        // touches de confort des menus, sauf si le joueur a donné cette touche à une action
        auto libre = [&](unsigned sc) {
            for (int i = 0; i < Settings::K_NB; i++)
                if (K[i] == sc) return false;
            return true;
        };
        if (libre(0x1C) && libre(0xE01C) && key_vk(VK_RETURN)) p.buttons |= BTN_A;   // Entrée, et celle du pavé
        if (libre(0x0E) && key_vk(VK_BACK)) p.buttons |= BTN_B;
        if (libre(0x01) && key_vk(VK_ESCAPE)) p.buttons |= BTN_PLUS;
        if (libre(0x0F) && key_vk(VK_TAB)) p.buttons |= BTN_MINUS;
        p.lx = axis(key_sc(K[Settings::K_LS_LEFT]), key_sc(K[Settings::K_LS_RIGHT]));
        p.ly = axis(key_sc(K[Settings::K_LS_DOWN]), key_sc(K[Settings::K_LS_UP]));
        p.rx = axis(key_sc(K[Settings::K_RS_LEFT]), key_sc(K[Settings::K_RS_RIGHT]));
        p.ry = axis(key_sc(K[Settings::K_RS_DOWN]), key_sc(K[Settings::K_RS_UP]));
    }
    // Manette XInput lue seulement quand la fenêtre du jeu est active, comme le clavier : sinon une partie
    // jouée ailleurs avec la même manette pilotait aussi le jeu (le 2026-11-09, 17 appuis sur Start =
    // « + » pendant un test en arrière-plan avaient mis la cinématique en pause).
    // RECOMP_BACKGROUND_INPUT=1 : manette lue même sans le focus (ancien comportement).
    static const bool background = getenv("RECOMP_BACKGROUND_INPUT") != nullptr;
    const bool lire = background || gfx_window_focused();
    auto mix = [](int32_t a, int32_t b) { return a ? a : b; };   // le clavier prime
    if (g_sdl) {
        static int tour = 0;
        sdl::PumpEvents();
        if (++tour % 30 == 0) suivre_pads();
        sdl::UpdateGamepads();
        for (Pad& pd : g_pads) {
            if (!lire) break;
            p.xinput = true;
            static const struct { int s; uint64_t b; } map[] = {
                {sdl::SOUTH, BTN_B}, {sdl::EAST, BTN_A}, {sdl::WEST, BTN_Y}, {sdl::NORTH, BTN_X},
                {sdl::LEFT_SHOULDER, BTN_L}, {sdl::RIGHT_SHOULDER, BTN_R}, {sdl::START, BTN_PLUS},
                {sdl::BACK, BTN_MINUS}, {sdl::LEFT_STICK, BTN_LSTICK}, {sdl::RIGHT_STICK, BTN_RSTICK},
                {sdl::DPAD_LEFT, BTN_LEFT}, {sdl::DPAD_UP, BTN_UP}, {sdl::DPAD_RIGHT, BTN_RIGHT},
                {sdl::DPAD_DOWN, BTN_DOWN},
            };
            for (auto& m : map)
                if (sdl::GetGamepadButton(pd.g, m.s)) p.buttons |= m.b;
            if (sdl::GetGamepadAxis(pd.g, sdl::LEFT_TRIGGER) > 8000) p.buttons |= BTN_ZL;
            if (sdl::GetGamepadAxis(pd.g, sdl::RIGHT_TRIGGER) > 8000) p.buttons |= BTN_ZR;
            // SDL : y positif vers le bas ; Switch : vers le haut
            auto ax = [&](int a, bool inv, int32_t dz) {
                int32_t v = sdl::GetGamepadAxis(pd.g, a);
                if (inv) v = -v;
                return dead(std::max(-STICK_MAX, std::min(STICK_MAX, v)), dz);
            };
            p.lx = mix(p.lx, ax(sdl::LEFTX, false, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE));
            p.ly = mix(p.ly, ax(sdl::LEFTY, true, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE));
            p.rx = mix(p.rx, ax(sdl::RIGHTX, false, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
            p.ry = mix(p.ry, ax(sdl::RIGHTY, true, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
        }
    }
    appliquer_vibrations(lire);
    XINPUT_STATE st{};
    if (!g_sdl && g_xget && lire && g_xget(0, &st) == ERROR_SUCCESS) {
        p.xinput = true;
        const WORD w = st.Gamepad.wButtons;
        struct { WORD x; uint64_t b; } map[] = {
            {XINPUT_GAMEPAD_A, BTN_B}, {XINPUT_GAMEPAD_B, BTN_A}, {XINPUT_GAMEPAD_X, BTN_Y},
            {XINPUT_GAMEPAD_Y, BTN_X}, {XINPUT_GAMEPAD_LEFT_SHOULDER, BTN_L},
            {XINPUT_GAMEPAD_RIGHT_SHOULDER, BTN_R}, {XINPUT_GAMEPAD_START, BTN_PLUS},
            {XINPUT_GAMEPAD_BACK, BTN_MINUS}, {XINPUT_GAMEPAD_LEFT_THUMB, BTN_LSTICK},
            {XINPUT_GAMEPAD_RIGHT_THUMB, BTN_RSTICK}, {XINPUT_GAMEPAD_DPAD_LEFT, BTN_LEFT},
            {XINPUT_GAMEPAD_DPAD_UP, BTN_UP}, {XINPUT_GAMEPAD_DPAD_RIGHT, BTN_RIGHT},
            {XINPUT_GAMEPAD_DPAD_DOWN, BTN_DOWN},
        };
        for (auto& m : map)
            if (w & m.x) p.buttons |= m.b;
        if (st.Gamepad.bLeftTrigger > 30) p.buttons |= BTN_ZL;
        if (st.Gamepad.bRightTrigger > 30) p.buttons |= BTN_ZR;
        p.lx = mix(p.lx, dead(st.Gamepad.sThumbLX, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE));
        p.ly = mix(p.ly, dead(st.Gamepad.sThumbLY, XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE));
        p.rx = mix(p.rx, dead(st.Gamepad.sThumbRX, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
        p.ry = mix(p.ry, dead(st.Gamepad.sThumbRY, XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE));
    }
    // diagnostic : RECOMP_AUTOPRESS=1 appuie sur A 100 ms toutes les 2 s (tests sans écran) ;
    // RECOMP_AUTOPRESS_UNTIL=s : seulement pendant les s premières secondes (écran titre), le jeu se
    // déroule ensuite sans passer les cinématiques
    static const bool autopress = getenv("RECOMP_AUTOPRESS") != nullptr;
    static const long press_until = getenv("RECOMP_AUTOPRESS_UNTIL") ? atol(getenv("RECOMP_AUTOPRESS_UNTIL")) : -1;
    static const uint64_t t_first = GetTickCount64();
    bool pressing = autopress || (press_until >= 0 && GetTickCount64() - t_first < (uint64_t)press_until * 1000);
    if (pressing && (GetTickCount64() % 2000) < 100) p.buttons |= BTN_A;
    p.buttons |= key_script_buttons(GetTickCount64() - t_first);
    p.buttons |= stick_dirs(p.lx, p.ly, BTN_LS_LEFT) | stick_dirs(p.rx, p.ry, BTN_RS_LEFT);
    return p;
}

}  // namespace hos
