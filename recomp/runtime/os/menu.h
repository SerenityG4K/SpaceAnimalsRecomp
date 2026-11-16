// Menu en jeu (menu.cpp) : les réglages pendant la partie, dessinés par-dessus l'image du jeu à la dernière étape de
// la présentation. F1 (ou Inser) l'ouvre et le ferme ; à la manette, le bouton central (Guide, PS, Home), ou Back et
// Start ensemble (Share et Options, − et +). Tant qu'il est ouvert, le jeu ne reçoit ni clavier ni manette ; il
// continue de tourner (mettre le jeu en pause d'abord).
//
// D'après le menu des recompilations Switch 2 de MagicBeanDev (Longshot, GPLv3 :
// https://git.magicbean.dev/MagicBeanDev/Longshot, gpu/vk/vk_menu.cpp, vk_imgui.cpp, vk_ui.h) : même principe (Dear
// ImGui pour le dessin seulement, ses propres touches, manette et souris, lignes à valeur, lignes « au prochain
// lancement »), réécrit pour ce jeu, ses réglages (recomp.ini) et l'allure de son launcher.
//
// Trois fichiers :
//   menu.cpp       les lignes, les entrées, le dessin (listes de dessin de Dear ImGui) ; ni Windows ni Vulkan ;
//   menu_ui.h      l'allure (couleurs du launcher, textes, touches dessinées) ;
//   menu_vk.cpp    Dear ImGui sur les images de la fenêtre (Vulkan), polices, textures.
// Le reste de l'hôte lui parle par les fonctions ci-dessous, et lui rend les services de l'espace hote.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace hos {
// journal (hos.h) : redéclaré ici, menu.cpp et menu_vk.cpp n'incluent pas hos.h (qui tire cpu.h)
void logf(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

namespace menu {

// ---- fil de la fenêtre (present.cpp) ----
// Touche enfoncée (WM_KEYDOWN, WM_SYSKEYDOWN) : touche virtuelle de Windows, scancode (0xE0xx : touche étendue),
// répétition automatique. Vrai : le menu l'a prise (fermé, seulement celles qui l'ouvrent ; ouvert, tout le clavier).
bool touche(unsigned tv, unsigned scancode, bool repetee);
void molette(int crans);   // molette de la souris, en crans (positif : vers le haut) ; sans effet menu fermé
void clic(int bouton);     // bouton de la souris enfoncé dans la fenêtre : 0 gauche, 1 droit ; sans effet menu fermé

// ---- lecture des entrées (input.cpp, fil du jeu) ----
// État brut des manettes à chaque lecture : entrées appuyées (un bit par Settings::P_*, par position, sans les
// réglages du joueur) et stick gauche (±32767, haut = +y).
void manette(uint32_t entrees, int32_t lx, int32_t ly);
// Le jeu ne doit recevoir aucune entrée : menu ouvert, ou juste fermé et un bouton de la fermeture encore appuyé
// (`neutre` : aucun bouton du jeu appuyé à cette lecture, ce qui lève l'attente).
bool masquer(bool neutre);

// ---- présentation (menu_vk.cpp, fil qui présente) ----
// Une fois par image présentée : entrées traitées ; vrai s'il y a quelque chose à dessiner (menu ouvert, compteur
// d'images, rappel de la touche au démarrage). Rien ne coûte tant que c'est faux.
bool a_dessiner();
void dessiner(float w, float h);   // entre vk::debut et vk::enregistrer : le menu, le compteur, le rappel
bool ouvert();

// ---- services de l'hôte ----
namespace hote {
std::string nom_touche(unsigned scancode);   // present.cpp : nom de la touche sur le clavier du joueur (« A », « Espace »)
bool plein_ecran();                          // present.cpp : plein écran sans bordure (sinon fenêtré)
void plein_ecran(bool oui);                  // present.cpp : bascule, comme Alt+Entrée
void quitter();                              // present.cpp : ferme le jeu comme la croix de la fenêtre
bool souris(float* x, float* y);             // present.cpp : curseur dans la fenêtre du jeu, en pixels ; faux hors d'elle
double images_par_seconde();                 // present.cpp : cadence mesurée (une valeur par seconde)
std::vector<std::string> cartes();           // menu_vk.cpp : cartes graphiques vues par Vulkan
std::string carte();                         // menu_vk.cpp : celle qui sert
std::string technique();                     // vk_render.cpp : DLSS ou FSR en service (« DLSS 4, modèle K… »), vide sinon
}  // namespace hote

}  // namespace menu
}  // namespace hos
