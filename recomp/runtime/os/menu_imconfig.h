// Réglages de Dear ImGui (third_party/imgui) pour le menu en jeu : link.py donne ce fichier à tout le runtime par
// -DIMGUI_USER_CONFIG=<os/menu_imconfig.h>, pour que les fichiers de Dear ImGui et ceux du menu (menu.cpp, menu_vk.cpp)
// voient les mêmes.
#pragma once

#define IMGUI_IMPL_VULKAN_NO_PROTOTYPES         // vulkan-1.dll est chargée à l'exécution (vk_render.cpp) : pas de prototypes
#define IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS   // pas d'ouverture de liens
#define IMGUI_DISABLE_WIN32_FUNCTIONS           // ni presse-papiers ni saisie de Windows : le menu a ses propres entrées
#define IMGUI_DISABLE_DEMO_WINDOWS
#define IMGUI_DISABLE_DEBUG_TOOLS

// Une vérification de Dear ImGui qui échoue est notée dans le journal du jeu (menu_vk.cpp) au lieu d'arrêter la partie.
extern "C" void hos_menu_assert(const char* expr, const char* fichier, int ligne);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : hos_menu_assert(#_EXPR, __FILE__, __LINE__))
