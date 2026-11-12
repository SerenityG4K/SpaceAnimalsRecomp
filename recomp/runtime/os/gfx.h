// HLE graphique : interface entre la couche NVN (hle_gfx.cpp) et la présentation hôte (present.cpp).
#pragma once
#include "hos.h"

#include <cstring>

namespace hos {

void gfx_open_window(int width, int height);           // crée la fenêtre hôte (une fois)
void gfx_present(const float rgba[4], uint64_t frame);  // présente une image (cadencée à 60 Hz)
bool gfx_window_focused();                              // la fenêtre hôte a le focus clavier
void* gfx_window_handle();                             // HWND (moteur Vulkan)
void gfx_status(const char* text);                     // état affiché dans le titre de la fenêtre (avant la 1re image)
void gfx_count_frame(uint64_t frame);
// écran virtuel (hle_gfx.cpp) : période de vblank en ticks du compteur invité (31,25 MHz), image affichée à un vblank
uint64_t vi_vblank_period();
void gfx_screen_size(int* w, int* h);   // résolution de l'écran principal de l'hôte
void vi_frame_displayed(uint64_t frame, uint64_t queued_tick, uint64_t shown_tick);                  // compteur img/s quand Vulkan présente

// Entrées hôte (input.cpp) : clavier (si la fenêtre a le focus) + première manette XInput,
// dans la convention Switch (bits nn::hid::NpadButton, sticks ±32767, haut = +y).
struct HostPad {
    uint64_t buttons = 0;
    int32_t lx = 0, ly = 0, rx = 0, ry = 0;
    bool xinput = false;   // une manette XInput est branchée
};
HostPad host_poll_input();
// Vibration envoyée par le jeu (svc_hid.cpp, nn::hid::SendVibrationValue) pour un côté de la manette (0 gauche,
// 1 droite) : amplitudes 0..1 et fréquences (Hz) des bandes basse et haute ; appliquée par input.cpp à 60 Hz.
void host_set_vibration(int side, float amp_low, float freq_low, float amp_high, float freq_high);

}  // namespace hos
