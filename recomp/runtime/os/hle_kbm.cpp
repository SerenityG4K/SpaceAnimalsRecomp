// Mode souris du jeu (réglage mouse_keyboard, recomp.ini). Sur la Switch 2, le jeu se joue à la souris avec un
// Joy-Con 2 posé sur la table : son capteur optique (« Jcms ») donne des déplacements que le jeu lit par
// nn::hid::GetJcmsStates. Le jeu ne se sert pas de la souris USB pour jouer (couche vwm, main+0xba67000…) :
//   - chaque manette connectée (sauf le mode portable) a deux poignées Jcms, Joy-Con gauche (type 0) et droit (1) ;
//     le jeu lit jusqu'à 16 états de chacune à chaque image et additionne les déplacements des états plus récents que
//     le dernier vu (main+0xba67e8c) ;
//   - une manette « a une souris » si le bit 0 des attributs de l'état le plus récent d'une de ses poignées est mis
//     (main+0xba69b04) ; le bit 1 dit que le Joy-Con est posé (« pad_left/right_mouse_contact ») ;
//   - le type de manette vient du style : bit 12 = « joycon-dual-ms » (paire de Joy-Con 2, avec souris), 13 et 14 = un
//     Joy-Con 2 seul (main+0xba69fa0) ; la manette Pro (« fullkey ») n'en a pas. svc_hid.cpp présente donc une paire
//     de Joy-Con 2 quand le réglage est mis (paire ordinaire, bit 2, quand le jeu n'accepte pas le bit 12 : en solo) ;
//   - sur la console, le jeu passe de lui-même en visée à la souris quand le Joy-Con droit est posé sur la table et
//     bouge (« Hold your Joy-Con 2 controller like a mouse ») : contact Jcms, mouvement, et capteur de mouvement qui
//     dit le Joy-Con couché sur la tranche (hid_six_axis_state). Les capteurs ne sont demandés que pour les styles
//     que rend GetSupportedNpadStyleSet (svc_hid.cpp, commande 101 : jusque-là 0, donc aucun capteur).
// Ici, les fonctions Jcms du SDK sont remplacées : la souris du PC devient le capteur du Joy-Con 2 droit, posé tant que
// la fenêtre du jeu a le focus ; le gauche est présent, levé, immobile. Les boutons de la souris appuient sur des boutons
// de la manette (input.cpp, réglages mouse_left…) ; le clavier pilote la manette comme d'habitude.
//
// JcmsState (0x20 octets, lu dans le jeu et dans nn::hid::detail::GetJcmsStates : LIFO de 17 états de 0x28 octets
// par poignée, comme ceux des Npad) : { s64 (horodatage ?), s64 n° d'échantillon (+8), s32 dx (+0x10), s32 dy
// (+0x14), u32 attributs (+0x18 : bit 0 capteur présent, bit 1 posé), u32 réservé }. Le plus récent d'abord.
// JcmsHandle : u32 { u8 Npad, u8 type } (nn::hid::utility::MakeJcmsHandle). JcmsType : u8.
//
// Souris : déplacements bruts (Raw Input, sans l'accélération de Windows : la visée suit la main). Le curseur de
// Windows est caché et retenu dans l'image du jeu : en plein écran dès que la fenêtre est active, en fenêtré après un
// clic dans l'image (la barre de titre reste à portée pour déplacer ou redimensionner la fenêtre). La touche
// mouse_release (F9 par défaut), Alt+Tab, la touche Windows ou la fermeture du jeu le rendent ; un clic dans l'image le
// reprend (ce clic n'appuie sur rien dans le jeu). Capture écrite pour Windows : ailleurs, le mode reste coupé
// (settings.cpp).
#include "gfx.h"
#include "settings.h"
#include "svc_util.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace hos {
namespace {

std::atomic<int64_t> g_dx{0}, g_dy{0};   // Raw Input accumulé depuis le dernier état Jcms
std::atomic<bool> g_captured{false};

#ifdef _WIN32
// ---------------------------------------------------------------- capture du curseur
std::atomic<bool> g_rendu{false};      // rendu par mouse_release : pas de reprise sans un clic dans l'image
std::atomic<uint32_t> g_masque{0};     // boutons de la souris tenus à la reprise : ignorés jusqu'à leur relâchement

bool down_vk(int vk) { return GetAsyncKeyState(vk) & 0x8000; }

// boutons de la souris tenus (bits Settings::M_GAUCHE … M_AVANT)
uint32_t boutons_tenus() {
    uint32_t b = 0;
    if (down_vk(VK_LBUTTON)) b |= 1u << Settings::M_GAUCHE;
    if (down_vk(VK_RBUTTON)) b |= 1u << Settings::M_DROIT;
    if (down_vk(VK_MBUTTON)) b |= 1u << Settings::M_MILIEU;
    if (down_vk(VK_XBUTTON1)) b |= 1u << Settings::M_ARRIERE;
    if (down_vk(VK_XBUTTON2)) b |= 1u << Settings::M_AVANT;
    return b;
}

// fenêtre à cadre (fenêtré) : sa barre de titre doit rester à portée du curseur
bool fenetre_a_cadre(HWND h) { return (GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) == WS_CAPTION; }

void clip(HWND h) {
    RECT r;
    GetClientRect(h, &r);
    POINT a{r.left, r.top}, b{r.right, r.bottom};
    ClientToScreen(h, &a);
    ClientToScreen(h, &b);
    RECT s{a.x, a.y, b.x, b.y};
    ClipCursor(&s);
}
void capture(HWND h, bool on, const char* pourquoi) {
    if (on == g_captured.load()) return;
    g_captured = on;
    if (on) {
        clip(h);
        SetCursor(nullptr);
        g_rendu = false;
        g_masque = boutons_tenus();   // le clic qui reprend la souris n'appuie sur rien dans le jeu
    } else {
        ClipCursor(nullptr);
    }
    g_dx = 0, g_dy = 0;   // rien de ce qui s'est passé hors capture
    HLOG("input", "mouse %s (%s)", on ? "captured by the game window" : "released", pourquoi);
}
#endif

// ---------------------------------------------------------------- capteur souris des Joy-Con 2 (Jcms)
struct JcmsState {
    int64_t tick;
    int64_t sampling;
    int32_t dx, dy;
    uint32_t attributes;
    uint32_t reserved;
};
static_assert(sizeof(JcmsState) == 0x20, "nn::hid::JcmsState");

constexpr int JCMS_N = 16;   // états gardés par Joy-Con (le jeu en demande 16)
struct Capteur {
    JcmsState s[JCMS_N] = {};   // s[0] : le plus récent
    int n = 0;
    std::chrono::steady_clock::time_point dernier{};
};
std::mutex g_jlk;
Capteur g_capteurs[2];          // Joy-Con 2 gauche, droit du joueur 1
double g_reste_x = 0, g_reste_y = 0;   // fractions de déplacement (mouse_speed) pas encore envoyées

// Joy-Con 2 droit posé sur la table comme une souris : tant que la fenêtre du jeu a le focus ; essais :
// RECOMP_JCMS_CONTACT=0 ou 1 l'impose
bool pose_souris() {
    static const char* const env_contact = getenv("RECOMP_JCMS_CONTACT");
    return env_contact ? atoi(env_contact) != 0 : gfx_window_focused();
}

// nouvel état d'un Joy-Con (g_jlk tenu)
void produire(int cote) {
    Capteur& c = g_capteurs[cote];
    memmove(&c.s[1], &c.s[0], (JCMS_N - 1) * sizeof(JcmsState));
    JcmsState& e = c.s[0];
    const int64_t n = c.s[1].sampling + 1;
    e = JcmsState{};
    e.tick = n;
    e.sampling = n;
    e.attributes = 1u << 0;   // capteur présent
    c.n = std::min(c.n + 1, JCMS_N);
    if (cote != 1) return;    // le gauche reste levé et immobile
    const bool focus = gfx_window_focused();
    if (pose_souris()) e.attributes |= 1u << 1;   // posé : mode souris du jeu
    const double dx = (double)g_dx.exchange(0), dy = (double)g_dy.exchange(0);
    if (!g_captured.load() || !focus) return;
    const double k = settings().mouse_speed / 100.0;
    g_reste_x += dx * k, g_reste_y += dy * k;
    const double ix = std::trunc(g_reste_x), iy = std::trunc(g_reste_y);
    g_reste_x -= ix, g_reste_y -= iy;
    e.dx = (int32_t)std::clamp(ix, -1e9, 1e9);
    e.dy = (int32_t)std::clamp(iy, -1e9, 1e9);
}

// nn::hid::InitializeJcms(void* buffer, size_t) / FinalizeJcms() / StartJcms(const JcmsHandle&) / StopJcms(…) : rien
// à faire (le SDK passerait le tampon au service hid, qui y écrirait les LIFO des capteurs)
void hid_jcms_rien(CPUState* ctx) { ctx->x[0] = 0; }

// nn::hid::GetJcmsHandle(const NpadIdType&, const JcmsType&) → JcmsHandle
void hid_jcms_handle(CPUState* ctx) {
    const uint32_t npad = ctx->x[0] ? *(const uint32_t*)ctx->x[0] : 0;
    const uint32_t type = ctx->x[1] ? *(const uint8_t*)ctx->x[1] : 0;
    ctx->x[0] = (npad & 0xff) | (type & 0xff) << 8;
}

// nn::hid::GetJcmsStates(JcmsState* out, int count, const JcmsHandle&) → nombre d'états (le plus récent d'abord)
void hid_jcms_states(CPUState* ctx) {
    static std::atomic<bool> vu{false};
    if (!vu.exchange(true)) HLOG("input", "the game reads the Joy-Con 2 mouse sensors (nn::hid::GetJcmsStates)");
    JcmsState* out = (JcmsState*)ctx->x[0];
    const int count = (int32_t)ctx->x[1];
    const uint32_t h = ctx->x[2] ? *(const uint32_t*)ctx->x[2] : 0xffffffffu;
    const uint32_t npad = h & 0xff, cote = (h >> 8) & 0xff;
    if (!out || count < 1) {
        ctx->x[0] = 0;
        return;
    }
    if (npad != 0 || cote > 1) {   // seul le joueur 1 a des Joy-Con 2 ; le jeu lit out[0] même sans état rendu
        memset(out, 0, sizeof *out);
        ctx->x[0] = 0;
        return;
    }
    std::lock_guard<std::mutex> lk(g_jlk);
    Capteur& c = g_capteurs[cote];
    // un état par image au plus (le jeu lit chaque poignée une ou deux fois par image) : les déplacements arrivent
    // une fois, dans un état plus récent que ceux déjà vus
    const auto t = std::chrono::steady_clock::now();
    if (c.n == 0 || t - c.dernier >= std::chrono::milliseconds(4)) {
        produire((int)cote);
        c.dernier = t;
    }
    const int n = std::min(count, c.n);
    memcpy(out, c.s, n * sizeof(JcmsState));
    ctx->x[0] = (uint64_t)n;
}

// ---------------------------------------------------------------- capteurs de mouvement des Joy-Con (six axes)
// player_input.vscript (main+0x881ccfa0 de l'image de référence) passe en mode souris quand un Joy-Con est posé
// (contact Jcms), que la souris bouge, et que son accélération x dépasse un seuil : Joy-Con gauche x > seuil, droit
// x < -seuil (posé sur la tranche, la gravité le long de son axe x) ; il en ressort quand x revient au-dessus de -0,5
// (droit). Indices des capteurs : 0 = Joy-Con gauche, 1 = droit (poignées de GetSixAxisSensorHandles, style JoyDual).
// SixAxisSensorState (0x60 octets, nn::hid::detail::GetSixAxisSensorStates) : { s64 intervalle (ns), s64 n°
// d'échantillon, f32 accélération[3] (G), vitesse angulaire[3], angle[3], orientation[3][3], u32 attributs (bit 0
// connecté), u32 réservé }. SixAxisSensorHandle : { u8 style + 3, u8 Npad, u8 Joy-Con (0 gauche, 1 droit), u8 - }.
struct SixAxisState {
    int64_t delta_time;
    int64_t sampling;
    float acc[3], gyro[3], angle[3], dir[9];
    uint32_t attributes, reserved;
};
static_assert(sizeof(SixAxisState) == 0x60, "nn::hid::SixAxisSensorState");

// nn::hid::GetSixAxisSensorState(SixAxisSensorState*, const SixAxisSensorHandle&) : au repos ; Joy-Con droit du joueur
// 1 couché sur la tranche tant qu'il est « posé » (pose_souris), sinon à plat comme le gauche
void hid_six_axis_state(CPUState* ctx) {
    SixAxisState* s = (SixAxisState*)ctx->x[0];
    const uint8_t* h = (const uint8_t*)ctx->x[1];
    if (!s) return;
    static std::atomic<int64_t> n{0};
    *s = SixAxisState{};
    s->delta_time = 16666667;
    s->sampling = ++n;
    s->dir[0] = s->dir[4] = s->dir[8] = 1.0f;
    s->attributes = 1u << 0;   // connecté
    if (h && h[1] == 0 && h[2] == 1 && pose_souris()) s->acc[0] = -1.0f;   // droit, sur la tranche
    else s->acc[2] = -1.0f;                                                // à plat
    static std::atomic<bool> vu{false};
    if (!vu.exchange(true))
        HLOG("input", "the game reads the Joy-Con motion sensors (nn::hid::GetSixAxisSensorState, handle %02x %02x %02x)",
             h ? h[0] : 0, h ? h[1] : 0, h ? h[2] : 0);
}

}  // namespace

// ---------------------------------------------------------------- molette
// Chaque cran de la molette (WM_INPUT) devient un appui bref sur l'action choisie (mouse_wheel_up / _down) : 80 ms
// appuyé, 40 ms relâché avant le cran suivant, assez pour que le jeu (60 lectures par seconde) voie chaque appui.
namespace {
struct Molette {
    int crans = 0;   // appuis en attente
    bool appui = false;
    std::chrono::steady_clock::time_point t{};
};
std::mutex g_molette_lk;
Molette g_molette[2];   // 0 vers le haut, 1 vers le bas

void molette_cran(int sens, int n) {
    std::lock_guard<std::mutex> lk(g_molette_lk);
    g_molette[sens].crans = std::min(g_molette[sens].crans + n, 4);
}
bool molette_appui(int sens) {
    using namespace std::chrono;
    std::lock_guard<std::mutex> lk(g_molette_lk);
    Molette& m = g_molette[sens];
    const auto t = steady_clock::now();
    if (m.appui && t - m.t >= milliseconds(80)) m.appui = false, m.t = t;
    else if (!m.appui && m.crans > 0 && t - m.t >= milliseconds(40)) m.appui = true, m.crans--, m.t = t;
    return m.appui;
}
}  // namespace

#ifdef _WIN32
// ---------------------------------------------------------------- boutons de la souris (input.cpp)
uint32_t kbm_mouse_buttons() {
    if (!settings().mouse_keyboard || !g_captured.load() || !gfx_window_focused()) return 0;
    uint32_t b = boutons_tenus();
    if (uint32_t m = g_masque.load()) {   // tenus depuis la reprise : ignorés tant qu'ils ne sont pas relâchés
        m &= b;
        g_masque = m;
        b &= ~m;
    }
    if (molette_appui(0)) b |= 1u << Settings::M_MOLETTE_HAUT;
    if (molette_appui(1)) b |= 1u << Settings::M_MOLETTE_BAS;
    return b;
}

void kbm_release() {
    if (g_captured.exchange(false)) ClipCursor(nullptr);
}

// ---------------------------------------------------------------- fenêtre (present.cpp)
void kbm_window_created(void* hwnd) {
    if (!settings().mouse_keyboard) return;
    RAWINPUTDEVICE d{};
    d.usUsagePage = 0x01;   // bureau générique
    d.usUsage = 0x02;       // souris
    d.dwFlags = 0;          // seulement quand la fenêtre est au premier plan
    d.hwndTarget = (HWND)hwnd;
    if (!RegisterRawInputDevices(&d, 1, sizeof d))
        HLOG("input", "mouse: RegisterRawInputDevices failed (%lu): no mouse for the game", GetLastError());
}

// Messages de la fenêtre qui concernent la souris du jeu ; vrai si le message est traité (*res : sa réponse)
bool kbm_wndproc(void* hwnd, unsigned msg, uintptr_t wp, intptr_t lp, intptr_t* res) {
    if (!settings().mouse_keyboard) return false;
    HWND h = (HWND)hwnd;
    switch (msg) {
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == (UINT)-1) break;
        if (ri.header.dwType != RIM_TYPEMOUSE) break;
        // plein écran : reprise dès que la fenêtre est active ; fenêtré ou souris rendue : au clic dans l'image
        if (!g_captured.load() && !g_rendu.load() && !fenetre_a_cadre(h) && GetForegroundWindow() == h && !IsIconic(h))
            capture(h, true, "borderless window active");
        if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {   // (une tablette ou un bureau à distance : absolu, ignoré)
            g_dx += ri.data.mouse.lLastX;
            g_dy += ri.data.mouse.lLastY;
        }
        // molette : crans de 120 ; une molette à haute résolution en envoie des fractions, additionnées jusqu'au cran
        // (remises à zéro quand le sens change)
        static int reste = 0;
        if ((ri.data.mouse.usButtonFlags & RI_MOUSE_WHEEL) && g_captured.load()) {
            const int d = (short)ri.data.mouse.usButtonData;
            if (d && (reste > 0) != (d > 0)) reste = 0;
            reste += d;
            const int n = reste / WHEEL_DELTA;
            if (n) {
                molette_cran(n > 0 ? 0 : 1, std::abs(n));
                reste -= n * WHEEL_DELTA;
            }
        }
        break;   // DefWindowProc fait le ménage du message
    }
    case WM_LBUTTONDOWN:   // clic dans l'image (pas sur le cadre) : la souris est reprise
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
        if (!g_captured.load()) capture(h, true, "click in the game");
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {   // mouse_release : la souris est rendue (un clic dans l'image la reprend)
        const unsigned t = settings().mouse_release, sc = (unsigned)((lp >> 16) & 0xFF) | ((lp & (1 << 24)) ? 0xE000u : 0u);
        if (g_captured.load() && t && sc == (t & 0xFFFF) && !(lp & (1 << 30)) &&
            ((t & Settings::MODIF_MASK) & ~host_modifiers_held()) == 0) {
            capture(h, false, "release key");
            g_rendu = true;
        }
        break;
    }
    case WM_SETCURSOR:
        if (g_captured.load() && LOWORD(lp) == HTCLIENT) {
            SetCursor(nullptr);
            *res = TRUE;
            return true;
        }
        break;
    case WM_KILLFOCUS:
    case WM_CANCELMODE:
        capture(h, false, "focus lost");
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) capture(h, false, "window inactive");
        break;
    case WM_SIZE:
    case WM_MOVE:
        if (g_captured.load()) clip(h);
        break;
    case WM_STYLECHANGED:   // plein écran → fenêtré (F11, Alt+Entrée) : la barre de titre redevient accessible
        if (g_captured.load() && fenetre_a_cadre(h)) capture(h, false, "window mode");
        break;
    case WM_DESTROY:
        capture(h, false, "window closed");
        break;
    }
    return false;
}
#else
// ailleurs que sous Windows : pas de capture de la souris (le mode est coupé, settings.cpp)
uint32_t kbm_mouse_buttons() { return 0; }
void kbm_release() {}
void kbm_window_created(void*) {}
bool kbm_wndproc(void*, unsigned, uintptr_t, intptr_t, intptr_t*) { return false; }
#endif

void hle_kbm_init() {
    if (!settings().mouse_keyboard) return;
    int n = 0;
    n += override_export("_ZN2nn3hid14InitializeJcmsEPvm", hid_jcms_rien);
    n += override_export("_ZN2nn3hid12FinalizeJcmsEv", hid_jcms_rien);
    n += override_export("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", hid_jcms_handle);
    n += override_export("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", hid_jcms_rien);
    n += override_export("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", hid_jcms_rien);
    n += override_export("_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", hid_jcms_states);
    HLOG("hle", "mouse mode (mouse_keyboard): %d of 6 nn::hid Jcms functions → PC mouse as the right Joy-Con 2 mouse", n);
    const bool six = override_export(
        "_ZN2nn3hid21GetSixAxisSensorStateEPNS0_18SixAxisSensorStateERKNS0_19SixAxisSensorHandleE", hid_six_axis_state);
    HLOG("hle", "mouse mode: Joy-Con motion sensors %s", six ? "emulated (right Joy-Con on its side while focused)"
                                                             : "NOT emulated");
}

}  // namespace hos
