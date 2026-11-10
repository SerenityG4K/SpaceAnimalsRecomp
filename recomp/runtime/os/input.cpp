// Entrées hôte pour la manette émulée (svc_hid.cpp) : clavier et première manette XInput (xinput1_4.dll,
// DLL système chargée dynamiquement), fusionnés, tous deux lus seulement si la fenêtre du jeu a le focus.
//
// Clavier, par position des touches (scancodes : identique en QWERTY et AZERTY) :
//   stick gauche   W A S D   (AZERTY : Z Q S D)        stick droit  I J K L
//   croix          flèches                              clics sticks F (gauche), H (droit)
//   A  C ou Entrée      B  X ou Retour arrière          X  V         Y  Z (AZERTY : W)
//   L  Q (AZERTY : A)   R  E      ZL  R      ZR  T      +  M (AZERTY : ,) ou Échap   -  N ou Tab
// Manette XInput, disposition par position (comme une manette Nintendo) : bouton du bas = B,
// droite = A, gauche = Y, haut = X ; LB/RB = L/R ; gâchettes = ZL/ZR ; Start = +, Back = -.
#include "gfx.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <xinput.h>

#include <algorithm>
#include <cstdlib>
#include <mutex>
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
XInputGetStateFn g_xget = nullptr;
std::once_flag g_once;

void init() {
    HMODULE m = LoadLibraryW(L"xinput1_4.dll");
    if (!m) m = LoadLibraryW(L"xinput9_1_0.dll");
    if (m) g_xget = (XInputGetStateFn)(void*)GetProcAddress(m, "XInputGetState");
    HLOG("input", "entrées : clavier%s, lus quand la fenêtre est active%s", g_xget ? " + manette XInput" : "",
         getenv("RECOMP_BACKGROUND_INPUT") ? " (manette aussi en arrière-plan : RECOMP_BACKGROUND_INPUT)" : "");
}

bool key_sc(UINT sc) {
    UINT vk = MapVirtualKeyW(sc, MAPVK_VSC_TO_VK);
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
        HLOG("input", "RECOMP_KEY_SCRIPT : %zu événements", v.size());
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

HostPad host_poll_input() {
    std::call_once(g_once, init);
    HostPad p;
    if (gfx_window_focused()) {
        struct { UINT sc; uint64_t b; } keys[] = {
            {0x2E, BTN_A}, {0x2D, BTN_B}, {0x2F, BTN_X}, {0x2C, BTN_Y},
            {0x10, BTN_L}, {0x12, BTN_R}, {0x13, BTN_ZL}, {0x14, BTN_ZR},
            {0x32, BTN_PLUS}, {0x31, BTN_MINUS}, {0x21, BTN_LSTICK}, {0x23, BTN_RSTICK},
        };
        for (auto& k : keys)
            if (key_sc(k.sc)) p.buttons |= k.b;
        if (key_vk(VK_RETURN)) p.buttons |= BTN_A;
        if (key_vk(VK_BACK)) p.buttons |= BTN_B;
        if (key_vk(VK_ESCAPE)) p.buttons |= BTN_PLUS;
        if (key_vk(VK_TAB)) p.buttons |= BTN_MINUS;
        if (key_vk(VK_LEFT)) p.buttons |= BTN_LEFT;
        if (key_vk(VK_UP)) p.buttons |= BTN_UP;
        if (key_vk(VK_RIGHT)) p.buttons |= BTN_RIGHT;
        if (key_vk(VK_DOWN)) p.buttons |= BTN_DOWN;
        p.lx = axis(key_sc(0x1E), key_sc(0x20));   // A / D
        p.ly = axis(key_sc(0x1F), key_sc(0x11));   // S / W
        p.rx = axis(key_sc(0x24), key_sc(0x26));   // J / L
        p.ry = axis(key_sc(0x25), key_sc(0x17));   // K / I
    }
    // Manette XInput lue seulement quand la fenêtre du jeu est active, comme le clavier : sinon une partie
    // jouée ailleurs avec la même manette pilotait aussi le jeu (le 2026-11-09, 17 appuis sur Start =
    // « + » pendant un test en arrière-plan avaient mis la cinématique en pause).
    // RECOMP_BACKGROUND_INPUT=1 : manette lue même sans le focus (ancien comportement).
    static const bool background = getenv("RECOMP_BACKGROUND_INPUT") != nullptr;
    XINPUT_STATE st{};
    if (g_xget && (background || gfx_window_focused()) && g_xget(0, &st) == ERROR_SUCCESS) {
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
        auto mix = [](int32_t a, int32_t b) { return a ? a : b; };   // le clavier prime
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
