// Menu en jeu : voir menu.h. Ce fichier tient les lignes du menu, ses entrées (clavier, manette, souris) et son dessin
// sur les listes de Dear ImGui ; il ne dépend ni de Windows ni de Vulkan (services de l'hôte : menu.h, espace hote).
//
// Haut et Bas choisissent une ligne, Gauche et Droite changent sa valeur, Q et E (L et R à la manette) changent
// d'onglet, Entrée (le bouton donné à A) valide ou ouvre une page, Échap (le bouton donné à B) revient ou ferme.
// Souris : la ligne sous le curseur est choisie ; un clic sur un onglet le montre, sur la moitié gauche ou droite d'une
// valeur la change, sur la piste d'un curseur le règle ; la molette fait défiler ; un clic droit revient. Le dessin
// note la place de chaque élément (g_zones) et la souris est essayée contre ces places.
//
// Les réglages qui valent tout de suite sont écrits dans les réglages en service (settings_vivants) ; ceux que le jeu
// ne lit qu'au démarrage sont marqués « au prochain lancement » tant que leur valeur n'est pas celle du départ. Tout
// ce que le joueur a changé est écrit dans recomp.ini à la fermeture du menu (settings_enregistrer).
#include "menu.h"

#include "audio_out.h"
#include "menu_ui.h"
#include "settings.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace hos {
namespace menu {
namespace {

// touches virtuelles de Windows que le menu lit (winuser.h, VK_*)
enum : unsigned {
    TV_RETOUR = 0x08, TV_TAB = 0x09, TV_ENTREE = 0x0D, TV_ECHAP = 0x1B, TV_ESPACE = 0x20, TV_PAGE_HAUT = 0x21,
    TV_PAGE_BAS = 0x22, TV_GAUCHE = 0x25, TV_HAUT = 0x26, TV_DROITE = 0x27, TV_BAS = 0x28, TV_INSER = 0x2D,
    TV_SUPPR = 0x2E, TV_E = 0x45, TV_Q = 0x51, TV_F1 = 0x70,
};
constexpr unsigned SC_INSER = 0xE052;   // scancode de la touche Inser

// ---------------------------------------------------------------- textes (français, anglais)
struct Texte {
    const char* cle;
    const char* fr;
    const char* en;
};
const Texte TEXTES[] = {
    {"titre", "RÉGLAGES", "SETTINGS"},
    {"jeu_nom", "STARFOX REMAKE  [RECOMPILED]", "STARFOX REMAKE  [RECOMPILED]"},
    {"o_graphismes", "GRAPHISMES", "GRAPHICS"},
    {"o_controles", "CONTRÔLES", "CONTROLS"},
    {"o_jeu", "JEU", "GAME"},
    {"p_manette", "BOUTONS DE LA MANETTE", "CONTROLLER BUTTONS"},
    {"p_clavier", "TOUCHES DU CLAVIER", "KEYBOARD KEYS"},
    // graphismes
    {"aff", "Affichage", "Display"},
    {"aff_plein", "Plein écran", "Full screen"},
    {"aff_fenetre", "Fenêtré", "Windowed"},
    {"aff_aide", "Plein écran sans bordure, image pixel pour pixel, ou fenêtre. Alt+Entrée et F11 basculent aussi.",
     "Borderless full screen, pixel for pixel, or a window. Alt+Enter and F11 toggle it too."},
    {"res", "Résolution", "Resolution"},
    {"res_auto", "Auto (écran)", "Auto (screen)"},
    {"res_aide", "Taille de l'image que le jeu dessine. Auto : celle de votre écran.",
     "Size of the picture the game draws. Auto: your screen's."},
    {"hz", "Images par seconde", "Frames per second"},
    {"hz_aide", "Cadence du jeu. 90 et 120 ne sont pas encore stables.", "The game's frame rate. 90 and 120 are not stable yet."},
    {"echelle", "Rendu 3D", "3D rendering"},
    {"echelle_natif", "100 % (natif)", "100% (native)"},
    {"echelle_aide",
     "Échelle du rendu 3D. Sous 100 % (expérimental), la 3D est rendue plus petite puis agrandie : plus rapide, moins net. "
     "Sans effet quand le DLSS ou le FSR agrandit l'image.",
     "Scale of the 3D rendering. Below 100% (experimental), 3D is rendered smaller then enlarged: faster, less sharp. "
     "No effect when DLSS or FSR enlarges the picture."},
    {"dlss", "DLSS (NVIDIA)", "DLSS (NVIDIA)"},
    {"dlss_aide",
     "Remplace l'anticrénelage du jeu. Cartes NVIDIA RTX seulement, avec nvngx_dlss.dll à côté du jeu. De Qualité à "
     "Ultra perf. : mise à l'échelle expérimentale, la 3D est rendue plus petite puis agrandie.",
     "Replaces the game's anti-aliasing. NVIDIA RTX cards only, with nvngx_dlss.dll next to the game. Quality to "
     "Ultra Perf.: experimental upscaling, 3D is rendered smaller then enlarged."},
    {"non", "Désactivé", "Off"},
    {"oui", "Activé", "On"},
    {"dlss_dlaa", "DLAA (natif)", "DLAA (native)"},
    {"q_qualite", "Qualité (67 %)", "Quality (67%)"},
    {"dlss_equilibre", "Équilibré (58 %)", "Balanced (58%)"},
    {"q_perf", "Performances (50 %)", "Performance (50%)"},
    {"q_ultra", "Ultra perf. (33 %)", "Ultra Perf. (33%)"},
    {"dlss_modele", "Modèle DLSS", "DLSS model"},
    {"dlss_4", "DLSS 4 (modèle K)", "DLSS 4 (model K)"},
    {"dlss_45", "DLSS 4.5 (modèle M)", "DLSS 4.5 (model M)"},
    {"dlss_modele_aide", "Réseau du DLSS. 4.5 : plus fin, plus coûteux.", "The DLSS network. 4.5: sharper, costlier."},
    {"fsr", "FSR (AMD)", "FSR (AMD)"},
    {"fsr_natif", "Natif (100 %)", "Native (100%)"},
    {"fsr_equilibre", "Équilibré (59 %)", "Balanced (59%)"},
    {"fsr_aide",
     "Remplace l'anticrénelage du jeu, sur toute carte graphique. Sans effet quand le DLSS sert (carte NVIDIA RTX). De "
     "Qualité à Ultra perf. : mise à l'échelle expérimentale.",
     "Replaces the game's anti-aliasing, on any graphics card. No effect when DLSS is used (NVIDIA RTX card). Quality to "
     "Ultra Perf.: experimental upscaling."},
    {"fsr_version", "Version FSR", "FSR version"},
    {"auto", "Auto", "Auto"},
    {"fsr_version_aide", "Auto : FSR 4 sur les cartes AMD qui le proposent, sinon FSR 3.1.",
     "Auto: FSR 4 on the AMD cards that offer it, FSR 3.1 otherwise."},
    {"nettete", "Netteté du FSR", "FSR sharpening"},
    {"nettete_aide", "Netteté ajoutée par le FSR. Vaut tout de suite ; sans effet quand le FSR ne sert pas.",
     "Sharpening added by FSR. Applies at once; no effect when FSR is not in use."},
    {"aucune", "Aucune", "Off"},
    {"carte", "Carte graphique", "Graphics card"},
    {"carte_auto", "Automatique", "Automatic"},
    {"carte_aide",
     "Automatique : la carte dédiée qui a le plus de mémoire vidéo. Changer de carte refait la préparation des shaders "
     "au lancement suivant.",
     "Automatic: the dedicated card with the most video memory. Changing the card redoes the shader preparation at the "
     "next launch."},
    {"en_service", "En service : ", "In use: "},
    {"carte_service", "Carte : ", "Card: "},
    // contrôles
    {"vib", "Vibrations", "Rumble"},
    {"vib_aide", "Intensité des vibrations de la manette (Xbox, PlayStation 4 et 5, Pro de la Switch).",
     "Strength of the controller's rumble (Xbox, PlayStation 4 and 5, Switch Pro)."},
    {"manette", "Boutons de la manette", "Controller buttons"},
    {"manette_aide", "Quel bouton de la manette fait chaque bouton de la Switch. Vaut tout de suite.",
     "Which controller button presses each Switch button. Applies at once."},
    {"clavier", "Touches du clavier", "Keyboard keys"},
    {"clavier_aide", "Quelle touche fait chaque bouton de la Switch et chaque direction des sticks. Vaut tout de suite.",
     "Which key presses each Switch button and each stick direction. Applies at once."},
    {"defaut", "Par défaut", "Default"},
    {"perso_m", "Personnalisés", "Custom"},
    {"perso_f", "Personnalisées", "Custom"},
    {"remettre", "Remettre par défaut", "Reset to defaults"},
    {"remettre_aide", "Remet la disposition d'origine.", "Puts the original layout back."},
    {"ligne_manette_aide",
     "Entrée : appuyez ensuite sur le bouton de la manette voulu ; s'il était pris, les deux échangent. Gauche et "
     "Droite : les autres entrées.",
     "Enter: then press the controller button you want; if it was in use, the two swap. Left and Right: the other inputs."},
    {"ligne_clavier_aide", "Entrée : appuyez ensuite sur la touche voulue ; si elle était prise, les deux échangent.",
     "Enter: then press the key you want; if it was in use, the two swap."},
    {"attente_bouton", "Appuyez sur un bouton…", "Press a button…"},
    {"attente_touche", "Appuyez sur une touche…", "Press a key…"},
    {"attente_bouton_aide", "Appuyez sur le bouton de la manette voulu. Échap : annuler.",
     "Press the controller button you want. Esc: cancel."},
    {"attente_touche_aide", "Appuyez sur la touche voulue. Échap : annuler. Suppr : aucune touche.",
     "Press the key you want. Esc: cancel. Delete: no key."},
    {"aucune_touche", "Aucune", "None"},
    // jeu
    {"volume", "Volume", "Volume"},
    {"volume_aide", "Volume du jeu.", "The game's volume."},
    {"volume_partie", "Gardé pour cette partie seulement : le jeu n'a pas été lancé par le launcher.",
     "Kept for this session only: the game was not started by the launcher."},
    {"muet", "Muet", "Mute"},
    {"langue", "Langue du jeu", "Game language"},
    {"langue_auto", "Auto (Windows)", "Auto (Windows)"},
    {"langue_aide", "Textes et voix du jeu. Auto : la langue de Windows.", "The game's text and voices. Auto: the Windows language."},
    {"fps", "Compteur d'images", "FPS counter"},
    {"fps_aide", "Images par seconde, dans le coin de l'image.", "Frames per second, in the corner of the picture."},
    {"taille", "Taille du menu", "Menu size"},
    {"taille_1", "Petit", "Small"},
    {"taille_2", "Moyen", "Medium"},
    {"taille_3", "Grand", "Large"},
    {"taille_aide", "Taille de ce menu et du compteur d'images. Grand : pour un téléviseur, de loin.",
     "Size of this menu and of the FPS counter. Large: for a TV, from a distance."},
    {"quitter", "Quitter le jeu", "Quit the game"},
    {"quitter_aide", "Ferme le jeu. Ce que le jeu n'a pas sauvegardé est perdu.", "Closes the game. What the game has not saved is lost."},
    {"confirmer", "Confirmer ?", "Confirm?"},
    {"ouvrir", "Changer", "Change"},
    // communs
    {"relance", "au prochain lancement", "at the next launch"},
    {"relance_note", "S'applique au prochain lancement du jeu.", "Applies the next time the game starts."},
    {"tourne", "Le jeu continue pendant que ce menu est ouvert.", "The game keeps running while this menu is open."},
    {"i_choisir", "Choisir", "Select"},
    {"i_changer", "Changer", "Change"},
    {"i_onglet", "Onglet", "Tab"},
    {"i_valider", "Valider", "Confirm"},
    {"i_retour", "Retour", "Back"},
    {"i_fermer", "Fermer", "Close"},
    {"t_entree", "Entrée", "Enter"},
    {"t_echap", "Échap", "Esc"},
    {"rappel", "Réglages", "Settings"},
    // boutons de la Switch (ordre de Settings::keys)
    {"b_10", "Clic du stick gauche", "Left stick click"},
    {"b_11", "Clic du stick droit", "Right stick click"},
    {"b_12", "Croix : haut", "D-pad up"},
    {"b_13", "Croix : bas", "D-pad down"},
    {"b_14", "Croix : gauche", "D-pad left"},
    {"b_15", "Croix : droite", "D-pad right"},
    {"b_16", "Stick gauche : haut", "Left stick up"},
    {"b_17", "Stick gauche : bas", "Left stick down"},
    {"b_18", "Stick gauche : gauche", "Left stick left"},
    {"b_19", "Stick gauche : droite", "Left stick right"},
    {"b_20", "Stick droit : haut", "Right stick up"},
    {"b_21", "Stick droit : bas", "Right stick down"},
    {"b_22", "Stick droit : gauche", "Right stick left"},
    {"b_23", "Stick droit : droite", "Right stick right"},
    // entrées d'une manette, par position (ordre de Settings::P_*)
    {"e_0", "Aucune", "None"},
    {"e_1", "Bouton du bas", "Bottom button"},
    {"e_2", "Bouton de droite", "Right button"},
    {"e_3", "Bouton de gauche", "Left button"},
    {"e_4", "Bouton du haut", "Top button"},
    {"e_5", "Back / Share / −", "Back / Share / −"},
    {"e_6", "Bouton central", "Centre button"},
    {"e_7", "Start / Options / +", "Start / Options / +"},
    {"e_8", "Clic du stick gauche", "Left stick click"},
    {"e_9", "Clic du stick droit", "Right stick click"},
    {"e_10", "LB / L1 / L", "LB / L1 / L"},
    {"e_11", "RB / R1 / R", "RB / R1 / R"},
    {"e_12", "Croix : haut", "D-pad up"},
    {"e_13", "Croix : bas", "D-pad down"},
    {"e_14", "Croix : gauche", "D-pad left"},
    {"e_15", "Croix : droite", "D-pad right"},
    {"e_16", "Capture / Micro", "Capture / Mic"},
    {"e_17", "Palette droite 1", "Right paddle 1"},
    {"e_18", "Palette gauche 1", "Left paddle 1"},
    {"e_19", "Palette droite 2", "Right paddle 2"},
    {"e_20", "Palette gauche 2", "Left paddle 2"},
    {"e_21", "Pavé tactile", "Touchpad"},
    {"e_22", "LT / L2 / ZL", "LT / L2 / ZL"},
    {"e_23", "RT / R2 / ZR", "RT / R2 / ZR"},
    // langues du jeu qui ne s'écrivent pas en lettres latines (les autres : leur propre nom)
    {"l_ja", "Japonais", "Japanese"},
    {"l_ru", "Russe", "Russian"},
    {"l_ko", "Coréen", "Korean"},
    {"l_zh-Hans", "Chinois simplifié", "Simplified Chinese"},
    {"l_zh-Hant", "Chinois traditionnel", "Traditional Chinese"},
};
bool g_fr = false;
const char* T(const char* cle) {
    static const std::unordered_map<std::string, const Texte*> table = [] {
        std::unordered_map<std::string, const Texte*> m;
        for (const Texte& t : TEXTES) m[t.cle] = &t;
        return m;
    }();
    const auto it = table.find(cle);
    if (it == table.end()) return cle;
    return g_fr ? it->second->fr : it->second->en;
}
std::string pourcent(int v) {
    char b[24];
    snprintf(b, sizeof b, g_fr ? "%d %%" : "%d%%", v);
    return b;
}

// ---------------------------------------------------------------- valeurs du menu
// Une valeur entière par ligne, telle que le menu la montre ; `vers` les rend aux réglages (Settings).
struct Choix {
    int plein_ecran = 1;   // 0 fenêtré, 1 plein écran sans bordure
    int resolution = 0;    // 0 auto, puis kResolutions
    int hz = 0;            // kCadences
    int echelle = 0;       // kEchelles
    int dlss = 0;          // Settings::dlss
    int dlss_modele = 0;   // 0 DLSS 4, 1 DLSS 4.5
    int fsr = 0;           // Settings::fsr
    int fsr_version = 0;   // 0 auto, 1 FSR 3.1, 2 FSR 4
    int nettete = 0;       // 0 à 100
    int carte = 0;         // 0 automatique, puis les cartes de g_cartes
    int vibration = 100;   // 0 à 100
    int volume = 100;      // 0 à 100
    int langue = 0;        // 0 auto, puis kLanguages
    int fps = 0;           // compteur d'images
    int taille = 1;        // 0 petit, 1 moyen, 2 grand
};
constexpr int kResolutions[] = {0, 720, 1080, 1440, 2160};
constexpr int kCadences[] = {60, 90, 120};
constexpr int kEchelles[] = {100, 85, 75, 67};
Choix g_c, g_depart;
std::vector<std::string> g_cartes;

template <size_t N>
int plus_proche(const int (&t)[N], int v) {
    int m = 0;
    for (int i = 1; i < (int)N; i++)
        if (std::abs(t[i] - v) < std::abs(t[m] - v)) m = i;
    return m;
}
std::string minuscules(std::string s) {
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z') ch = (char)(ch - 'A' + 'a');
    return s;
}
Choix choix_de(const Settings& s) {
    Choix c;
    c.plein_ecran = s.borderless;
    c.resolution = plus_proche(kResolutions, s.resolution);
    c.hz = plus_proche(kCadences, s.refresh_rate);
    c.echelle = plus_proche(kEchelles, s.render_scale);
    c.dlss = std::clamp(s.dlss, 0, 5);
    c.dlss_modele = s.dlss_modele == 2;
    c.fsr = std::clamp(s.fsr, 0, 5);
    c.fsr_version = s.fsr_version == 3 ? 1 : s.fsr_version == 4 ? 2 : 0;
    c.nettete = std::clamp(s.fsr_sharpness, 0, 100);
    if (s.gpu != "auto")   // comme le jeu (vk_render.cpp, choisir_carte) : la première carte dont le nom le contient
        for (size_t i = 0; i < g_cartes.size() && !c.carte; i++)
            if (minuscules(g_cartes[i]).find(minuscules(s.gpu)) != std::string::npos) c.carte = (int)i + 1;
    c.vibration = std::clamp(s.vibration, 0, 100);
    for (int i = 0; i < kNbLanguages; i++)
        if (s.language == kLanguages[i]) c.langue = i + 1;
    c.fps = s.show_fps;
    c.taille = std::clamp(s.menu_size, 1, 3) - 1;
    return c;
}
void vers(const Choix& c, Settings& s) {
    s.borderless = c.plein_ecran != 0;
    s.resolution = kResolutions[c.resolution];
    s.refresh_rate = kCadences[c.hz];
    s.render_scale = kEchelles[c.echelle];
    s.dlss = c.dlss;
    s.dlss_modele = c.dlss_modele ? 2 : 1;
    s.fsr = c.fsr;
    s.fsr_version = c.fsr_version == 1 ? 3 : c.fsr_version == 2 ? 4 : 0;
    s.fsr_sharpness = c.nettete;
    s.gpu = c.carte >= 1 && c.carte <= (int)g_cartes.size() ? g_cartes[c.carte - 1] : "auto";
    s.vibration = c.vibration;
    s.language = c.langue >= 1 && c.langue <= kNbLanguages ? kLanguages[c.langue - 1] : "auto";
    s.show_fps = c.fps != 0;
    s.menu_size = c.taille + 1;
}

// ---- textes des valeurs ----
std::string tx_aff(int v) { return T(v ? "aff_plein" : "aff_fenetre"); }
std::string tx_res(int v) {
    if (!v) return T("res_auto");
    return std::to_string(kResolutions[v]) + (kResolutions[v] == 2160 ? "p (4K)" : "p");
}
std::string tx_hz(int v) { return std::to_string(kCadences[v]); }
std::string tx_echelle(int v) { return v ? pourcent(kEchelles[v]) : T("echelle_natif"); }
std::string tx_dlss(int v) {
    static const char* const t[] = {"non", "dlss_dlaa", "q_qualite", "dlss_equilibre", "q_perf", "q_ultra"};
    return T(t[std::clamp(v, 0, 5)]);
}
std::string tx_dlss_modele(int v) { return T(v ? "dlss_45" : "dlss_4"); }
std::string tx_fsr(int v) {
    static const char* const t[] = {"non", "fsr_natif", "q_qualite", "fsr_equilibre", "q_perf", "q_ultra"};
    return T(t[std::clamp(v, 0, 5)]);
}
std::string tx_fsr_version(int v) { return v == 1 ? "FSR 3.1" : v == 2 ? "FSR 4" : T("auto"); }
std::string tx_nettete(int v) { return v ? pourcent(v) : T("aucune"); }
std::string tx_carte(int v) { return v >= 1 && v <= (int)g_cartes.size() ? g_cartes[v - 1] : T("carte_auto"); }
std::string tx_vib(int v) { return v ? pourcent(v) : T("aucune"); }
std::string tx_volume(int v) { return v ? pourcent(v) : T("muet"); }
std::string tx_langue(int v) {
    static const char* const noms[] = {nullptr, "English (US)", "English (UK)", "Français", "Français (Canada)", "Deutsch",
                                       "Italiano", "Español", "Español (Latinoamérica)", "Nederlands", "Português",
                                       "Português (Brasil)", nullptr, nullptr, nullptr, nullptr};
    if (v < 1 || v > kNbLanguages) return T("langue_auto");
    if (noms[v - 1]) return noms[v - 1];
    return T((std::string("l_") + kLanguages[v - 1]).c_str());
}
std::string tx_oui_non(int v) { return T(v ? "oui" : "non"); }
std::string tx_taille(int v) { return T(v == 0 ? "taille_1" : v == 2 ? "taille_3" : "taille_2"); }

// ---- réglages qui valent tout de suite ----
void ap_aff(int v) { hote::plein_ecran(v != 0); }
void ap_nettete(int v) { settings_vivants().fsr_sharpness = v; }
void ap_vib(int v) { settings_vivants().vibration = v; }
void ap_volume(int v) { audio_out::set_volume(v); }
void ap_fps(int v) { settings_vivants().show_fps = v != 0; }
void ap_taille(int v) { settings_vivants().menu_size = v + 1; }

// ---------------------------------------------------------------- lignes
enum Vue { V_GRAPHISMES, V_CONTROLES, V_JEU, V_NB_ONGLETS, V_MANETTE = V_NB_ONGLETS, V_CLAVIER, V_NB };
enum Genre { G_CHOIX, G_CURSEUR, G_PAGE, G_ACTION };
enum Action { AC_AUCUNE, AC_QUITTER };
struct Ligne {
    int vue;
    Genre genre;
    const char* nom;                   // clé de texte (son aide : la même, suivie de _aide)
    int Choix::* v = nullptr;
    int lo = 0, hi = 0, pas = 1;
    std::string (*texte)(int) = nullptr;
    bool relance = false;              // lu une fois au démarrage du jeu : vaut au prochain lancement
    const char* cle = nullptr;         // clé de recomp.ini écrite quand la ligne change
    void (*applique)(int) = nullptr;   // réglage qui vaut tout de suite
    int ouvre = -1;                    // G_PAGE : la page ouverte
    Action action = AC_AUCUNE;         // G_ACTION
};
std::vector<Ligne> g_lignes;

void lignes_init() {
    auto choix = [](int vue, const char* nom, int Choix::* v, int hi, std::string (*tx)(int), bool relance, const char* cle,
                    void (*ap)(int) = nullptr) {
        Ligne l{vue, G_CHOIX, nom, v, 0, hi, 1, tx, relance, cle, ap};
        g_lignes.push_back(l);
    };
    auto curseur = [](int vue, const char* nom, int Choix::* v, int pas, std::string (*tx)(int), const char* cle, void (*ap)(int)) {
        Ligne l{vue, G_CURSEUR, nom, v, 0, 100, pas, tx, false, cle, ap};
        g_lignes.push_back(l);
    };
    choix(V_GRAPHISMES, "aff", &Choix::plein_ecran, 1, tx_aff, false, "window_mode", ap_aff);
    choix(V_GRAPHISMES, "res", &Choix::resolution, 4, tx_res, true, "resolution");
    choix(V_GRAPHISMES, "hz", &Choix::hz, 2, tx_hz, true, "refresh_rate");
    choix(V_GRAPHISMES, "echelle", &Choix::echelle, 3, tx_echelle, true, "render_scale");
    choix(V_GRAPHISMES, "dlss", &Choix::dlss, 5, tx_dlss, true, "dlss");
    choix(V_GRAPHISMES, "dlss_modele", &Choix::dlss_modele, 1, tx_dlss_modele, true, "dlss_model");
    choix(V_GRAPHISMES, "fsr", &Choix::fsr, 5, tx_fsr, true, "fsr");
    choix(V_GRAPHISMES, "fsr_version", &Choix::fsr_version, 2, tx_fsr_version, true, "fsr_version");
    curseur(V_GRAPHISMES, "nettete", &Choix::nettete, 10, tx_nettete, "fsr_sharpness", ap_nettete);
    choix(V_GRAPHISMES, "carte", &Choix::carte, (int)g_cartes.size(), tx_carte, true, "gpu");
    curseur(V_CONTROLES, "vib", &Choix::vibration, 10, tx_vib, "vibration", ap_vib);
    Ligne manette{V_CONTROLES, G_PAGE, "manette"};
    manette.ouvre = V_MANETTE;
    g_lignes.push_back(manette);
    Ligne clavier{V_CONTROLES, G_PAGE, "clavier"};
    clavier.ouvre = V_CLAVIER;
    g_lignes.push_back(clavier);
    curseur(V_JEU, "volume", &Choix::volume, 5, tx_volume, nullptr, ap_volume);
    choix(V_JEU, "langue", &Choix::langue, kNbLanguages, tx_langue, true, "language");
    choix(V_JEU, "fps", &Choix::fps, 1, tx_oui_non, false, "show_fps", ap_fps);
    choix(V_JEU, "taille", &Choix::taille, 2, tx_taille, false, "menu_size", ap_taille);
    Ligne quitter{V_JEU, G_ACTION, "quitter"};
    quitter.action = AC_QUITTER;
    g_lignes.push_back(quitter);
}
std::vector<int> lignes_de(int vue) {
    std::vector<int> v;
    for (int i = 0; i < (int)g_lignes.size(); i++)
        if (g_lignes[i].vue == vue) v.push_back(i);
    return v;
}
int nb_lignes(int vue) {
    if (vue == V_MANETTE) return Settings::PAD_NB + 1;   // les boutons, puis « Remettre par défaut »
    if (vue == V_CLAVIER) return Settings::K_NB + 1;
    return (int)lignes_de(vue).size();
}

// ---------------------------------------------------------------- état
struct Ev {
    enum { TOUCHE, BASCULE, MOLETTE, CLIC } type;
    unsigned tv = 0, sc = 0;
    bool repetee = false;
    int n = 0;
};
std::mutex g_ev_mx;
std::vector<Ev> g_evs;   // évènements de la fenêtre, en attente du fil qui présente
std::atomic<bool> g_ouvert{false};
std::atomic<bool> g_relache{false};   // menu juste fermé : le jeu ne reçoit rien tant qu'un bouton reste appuyé
std::atomic<uint32_t> g_pad{0};
std::atomic<int32_t> g_pad_lx{0}, g_pad_ly{0};

bool g_pret = false;
int g_vue = V_GRAPHISMES;
int g_sel[V_NB] = {}, g_defil[V_NB] = {};
std::atomic<int> g_attente{0};   // 0 non, 1 le prochain bouton de la manette, 2 la prochaine touche : pour la ligne choisie
bool g_confirme = false;   // « Quitter le jeu » attend son deuxième appui
bool g_pad_dernier = false;   // la dernière entrée venait d'une manette
uint32_t g_pad_avant = 0, g_pad_ignore = 0;
int g_dir = -1;            // direction tenue à la manette (0 haut, 1 bas, 2 gauche, 3 droite), -1 aucune
std::chrono::steady_clock::time_point g_dir_t, g_t0;
std::set<std::string> g_cles;   // clés de recomp.ini à écrire à la fermeture
bool g_volume_change = false;
bool g_volume_garde = true;     // le volume a pu être écrit pour le launcher
float g_souris_x = -1, g_souris_y = -1;
float g_h = 720;   // hauteur de la fenêtre au dernier dessin

// places des éléments au dernier dessin, pour la souris
enum GenreZone { Z_LIGNE, Z_ONGLET, Z_GAUCHE, Z_DROITE, Z_CURSEUR };
struct Zone {
    ImVec2 a, b;
    int genre, i;
};
std::vector<Zone> g_zones;

uint32_t bit(int entree) { return 1u << entree; }
int entree_de(int action, int defaut) {   // entrée de la manette donnée à un bouton de la Switch
    const int e = settings().pad[action];
    return e > Settings::P_AUCUNE && e < Settings::P_NB ? e : defaut;
}
int entree_valider() { return entree_de(Settings::K_A, Settings::P_EST); }
int entree_retour() {
    const int e = entree_de(Settings::K_B, Settings::P_SUD);
    return e != entree_valider() ? e : e == Settings::P_SUD ? Settings::P_EST : Settings::P_SUD;
}

bool defaut_manette() {
    const Settings d;
    return !memcmp(d.pad, settings().pad, sizeof d.pad);
}
bool defaut_clavier() {
    const Settings d;
    return !memcmp(d.keys, settings().keys, sizeof d.keys);
}

void enregistrer() {
    if (g_volume_change) {
        g_volume_change = false;
        g_volume_garde = settings_volume_enregistrer(g_c.volume);
    }
    if (g_cles.empty()) return;
    Settings s = settings();
    vers(g_c, s);
    std::vector<std::pair<std::string, std::string>> lignes;
    for (const auto& l : settings_lignes(s)) {
        const std::string groupe = l.first.compare(0, 4, "key_") == 0 ? "key_" : l.first.compare(0, 4, "pad_") == 0 ? "pad_" : l.first;
        if (g_cles.count(groupe)) lignes.push_back(l);
    }
    const bool ok = settings_enregistrer(lignes);
    logf("menu", "%zu setting(s) %s %s", lignes.size(), ok ? "written to" : "could NOT be written to", settings().file.c_str());
    if (ok) g_cles.clear();
}

void ouvrir() {
    g_c.plein_ecran = hote::plein_ecran();   // Alt+Entrée et F11 l'ont peut-être changé
    g_c.volume = audio_out::volume();
    g_confirme = false;
    g_attente = 0;
    g_pad_ignore = g_pad.load();   // les boutons qui l'ont ouvert ne comptent pas tant qu'ils restent appuyés
    g_pad_avant = 0;
    g_dir = -1;
    g_ouvert = true;
}
void fermer() {
    g_attente = 0;
    g_confirme = false;
    enregistrer();
    g_relache = true;
    g_ouvert = false;
}

void changer(const Ligne& l, int v) {
    if (!l.v || g_c.*l.v == v) return;
    g_c.*l.v = v;
    if (l.applique) l.applique(v);
    if (l.cle) g_cles.insert(l.cle);
    if (l.v == &Choix::volume) g_volume_change = true;
}

// ---- pages des contrôles : boutons de la manette, touches du clavier ----
void donner_bouton(int action, int entree) {   // une entrée déjà prise ailleurs : les deux échangent
    Settings& s = settings_vivants();
    const uint8_t avant = s.pad[action];
    if (entree != Settings::P_AUCUNE)
        for (int j = 0; j < Settings::PAD_NB; j++)
            if (j != action && s.pad[j] == entree) s.pad[j] = avant;
    s.pad[action] = (uint8_t)entree;
    g_cles.insert("pad_");
}
void donner_touche(int action, unsigned sc) {
    Settings& s = settings_vivants();
    const unsigned avant = s.keys[action];
    if (sc)
        for (int j = 0; j < Settings::K_NB; j++)
            if (j != action && s.keys[j] == sc) s.keys[j] = avant;
    s.keys[action] = sc;
    g_cles.insert("key_");
}
void remettre(int vue) {
    const Settings d;
    Settings& s = settings_vivants();
    if (vue == V_MANETTE) memcpy(s.pad, d.pad, sizeof d.pad), g_cles.insert("pad_");
    else memcpy(s.keys, d.keys, sizeof d.keys), g_cles.insert("key_");
}
std::string nom_bouton(int action) {
    static const char* const lettres[] = {"A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "−"};
    if (action < 10) return lettres[action];
    return T(("b_" + std::to_string(action)).c_str());
}
std::string nom_entree(int e) { return T(("e_" + std::to_string(e)).c_str()); }

// ---------------------------------------------------------------- actions
enum Act { A_HAUT, A_BAS, A_GAUCHE, A_DROITE, A_VALIDER, A_RETOUR, A_ONGLET_G, A_ONGLET_D };

void aller_onglet(int o) {
    g_vue = (o % V_NB_ONGLETS + V_NB_ONGLETS) % V_NB_ONGLETS;
    g_confirme = false;
    g_attente = 0;
}
void pas_de_valeur(const Ligne& l, int sens) {
    if (!l.v) return;
    int v = g_c.*l.v + sens * l.pas;
    if (l.genre == G_CURSEUR) v = std::clamp(v, l.lo, l.hi);   // un curseur s'arrête à ses bouts
    else if (v > l.hi) v = l.lo;                              // un choix fait le tour
    else if (v < l.lo) v = l.hi;
    changer(l, v);
}
void agir(Act a) {
    const int n = nb_lignes(g_vue);
    int& sel = g_sel[g_vue];
    if (a != A_VALIDER) g_confirme = false;
    switch (a) {
    case A_HAUT: sel = (sel + n - 1) % n; return;
    case A_BAS: sel = (sel + 1) % n; return;
    case A_ONGLET_G:
    case A_ONGLET_D: {
        const int o = g_vue < V_NB_ONGLETS ? g_vue : V_CONTROLES;
        aller_onglet(o + (a == A_ONGLET_D ? 1 : -1));
        return;
    }
    case A_RETOUR:
        if (g_vue >= V_NB_ONGLETS) g_vue = V_CONTROLES;   // d'une page à son onglet
        else fermer();
        return;
    default: break;
    }
    if (g_vue == V_MANETTE || g_vue == V_CLAVIER) {
        const bool manette = g_vue == V_MANETTE;
        const int actions = manette ? Settings::PAD_NB : Settings::K_NB;
        if (sel >= actions) {   // « Remettre par défaut »
            if (a == A_VALIDER) remettre(g_vue);
        } else if (a == A_VALIDER) {
            g_attente = manette ? 1 : 2;
            g_pad_ignore |= g_pad.load();   // le bouton qui valide n'est pas la réponse
        } else if (manette) {   // Gauche et Droite : l'entrée d'avant, la suivante
            const int e = (settings().pad[sel] + (a == A_DROITE ? 1 : Settings::P_NB - 1)) % Settings::P_NB;
            settings_vivants().pad[sel] = (uint8_t)e;
            g_cles.insert("pad_");
        }
        return;
    }
    const std::vector<int> lv = lignes_de(g_vue);
    const Ligne& l = g_lignes[lv[sel]];
    if (l.genre == G_PAGE) {
        if (a == A_VALIDER || a == A_DROITE) g_vue = l.ouvre;
    } else if (l.genre == G_ACTION) {
        if (a != A_VALIDER) return;
        if (!g_confirme) {
            g_confirme = true;
        } else if (l.action == AC_QUITTER) {
            enregistrer();
            logf("menu", "quit asked from the in-game menu");
            hote::quitter();
        }
    } else {
        pas_de_valeur(l, a == A_GAUCHE ? -1 : 1);
    }
}

// ---------------------------------------------------------------- entrées
void touche_menu(const Ev& e) {
    g_pad_dernier = false;
    if (g_attente) {   // la touche attendue (page du clavier), ou Échap pour annuler
        if (e.repetee) return;
        if (e.tv == TV_ECHAP || e.tv == TV_F1) {
            g_attente = 0;
        } else if (g_attente == 2) {
            const int action = g_sel[V_CLAVIER];
            if (e.tv == TV_SUPPR) donner_touche(action, 0);
            else if (e.sc && (e.sc <= 0xFF || (e.sc >= 0xE000 && e.sc <= 0xE0FF))) donner_touche(action, e.sc);
            else return;
            g_attente = 0;
        }
        return;
    }
    switch (e.tv) {
    case TV_HAUT: agir(A_HAUT); break;
    case TV_BAS: agir(A_BAS); break;
    case TV_GAUCHE: agir(A_GAUCHE); break;
    case TV_DROITE: agir(A_DROITE); break;
    case TV_ENTREE:
    case TV_ESPACE:
        if (!e.repetee) agir(A_VALIDER);
        break;
    case TV_ECHAP:
    case TV_RETOUR:
        if (!e.repetee) agir(A_RETOUR);
        break;
    case TV_Q:
    case TV_PAGE_HAUT:
        if (!e.repetee) agir(A_ONGLET_G);
        break;
    case TV_E:
    case TV_PAGE_BAS:
    case TV_TAB:
        if (!e.repetee) agir(A_ONGLET_D);
        break;
    default: break;
    }
}

const Zone* zone_sous(float x, float y, int genre_min) {
    for (auto it = g_zones.rbegin(); it != g_zones.rend(); ++it)   // les dernières dessinées sont au-dessus
        if (it->genre >= genre_min && x >= it->a.x && x < it->b.x && y >= it->a.y && y < it->b.y) return &*it;
    return nullptr;
}
void clic_menu(int bouton) {
    g_pad_dernier = false;
    if (g_attente) {   // un clic annule l'attente
        g_attente = 0;
        return;
    }
    if (bouton == 1) {
        agir(A_RETOUR);
        return;
    }
    float x, y;
    if (!hote::souris(&x, &y)) return;
    const Zone* z = zone_sous(x, y, Z_LIGNE);
    if (!z) return;
    if (z->genre == Z_ONGLET) {
        aller_onglet(z->i);
        return;
    }
    if (g_sel[g_vue] != z->i) g_confirme = false;
    g_sel[g_vue] = z->i;
    switch (z->genre) {
    case Z_GAUCHE: agir(A_GAUCHE); break;
    case Z_DROITE: agir(A_DROITE); break;
    case Z_CURSEUR: {
        const std::vector<int> lv = lignes_de(g_vue);
        if (z->i >= (int)lv.size()) break;
        const Ligne& l = g_lignes[lv[z->i]];
        const float jeu = 8 * ui::unite(g_h, settings().menu_size);   // la zone déborde la piste de 8 points de chaque côté
        const float k = std::clamp((x - z->a.x - jeu) / std::max(1.0f, z->b.x - z->a.x - 2 * jeu), 0.0f, 1.0f);
        changer(l, std::clamp((int)std::lround(k * (l.hi - l.lo) / l.pas) * l.pas + l.lo, l.lo, l.hi));
        break;
    }
    default: {   // la ligne elle-même : une page s'ouvre, une action se fait, une touche ou un bouton attend sa réponse
        bool valide = g_vue == V_MANETTE || g_vue == V_CLAVIER;
        if (!valide) {
            const std::vector<int> lv = lignes_de(g_vue);
            valide = z->i < (int)lv.size() && g_lignes[lv[z->i]].genre >= G_PAGE;
        }
        if (valide) agir(A_VALIDER);
        break;
    }
    }
}

void manette_menu() {
    using namespace std::chrono;
    const uint32_t brut = g_pad.load();
    g_pad_ignore &= brut;   // un bouton ignoré compte de nouveau une fois relâché
    const uint32_t e = brut & ~g_pad_ignore;
    const uint32_t nouveaux = e & ~g_pad_avant;
    g_pad_avant = e;
    if (nouveaux) g_pad_dernier = true;
    if (g_attente == 1) {   // le bouton attendu (page de la manette)
        for (int i = Settings::P_SUD; i < Settings::P_NB; i++)
            if (nouveaux & bit(i)) {
                donner_bouton(g_sel[V_MANETTE], i);
                g_attente = 0;
                g_pad_ignore |= brut;   // ce bouton n'agit pas sur le menu tant qu'il reste appuyé
                break;
            }
        return;
    }
    if (g_attente) return;
    // directions : la croix, ou le stick gauche au-delà de la mi-course ; tenues, elles se répètent
    constexpr int32_t T = 16384;
    const int32_t lx = g_pad_lx.load(), ly = g_pad_ly.load();
    int dir = -1;
    if (e & bit(Settings::P_HAUT) || ly > T) dir = 0;
    else if (e & bit(Settings::P_BAS) || ly < -T) dir = 1;
    else if (e & bit(Settings::P_GAUCHE) || lx < -T) dir = 2;
    else if (e & bit(Settings::P_DROITE) || lx > T) dir = 3;
    const auto t = steady_clock::now();
    if (dir != g_dir) {
        g_dir = dir;
        g_dir_t = t + milliseconds(380);
        if (dir >= 0) g_pad_dernier = true, agir((Act)dir);
    } else if (dir >= 0 && t >= g_dir_t) {
        g_dir_t = t + milliseconds(dir < 2 ? 90 : 70);
        agir((Act)dir);
    }
    if (nouveaux & bit(entree_valider())) agir(A_VALIDER);
    else if (nouveaux & bit(entree_retour())) agir(A_RETOUR);
    else if (nouveaux & bit(Settings::P_EPAULE_G)) agir(A_ONGLET_G);
    else if (nouveaux & bit(Settings::P_EPAULE_D)) agir(A_ONGLET_D);
}

// la manette ouvre et ferme le menu : bouton central, ou Back et Start ensemble
bool bascule_manette() {
    static uint32_t avant = 0;
    const uint32_t e = g_pad.load(), nouveaux = e & ~avant;
    avant = e;
    const uint32_t accord = bit(Settings::P_BACK) | bit(Settings::P_START);
    return (nouveaux & bit(Settings::P_GUIDE)) || ((e & accord) == accord && (nouveaux & accord));
}

void init() {
    g_pret = true;
    g_fr = language_code().compare(0, 2, "fr") == 0;
    g_cartes = hote::cartes();
    g_c = choix_de(settings());
    g_c.volume = audio_out::volume();
    g_depart = g_c;
    lignes_init();
    g_t0 = std::chrono::steady_clock::now();
    logf("menu", "in-game menu ready (F1, Insert, or the controller's centre button / Back + Start), %zu graphics card(s) listed",
         g_cartes.size());
}

// rappel de la touche du menu, les premières secondes de la partie (pas pendant le relevé des shaders, sans fenêtre)
constexpr double kRappelDuree = 9.0;
double rappel_age() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_t0).count(); }
bool rappel_visible() {
    static const bool jamais = getenv("RECOMP_WINDOW_HIDDEN") != nullptr || getenv("RECOMP_NO_MENU_HINT") != nullptr;
    return !jamais && rappel_age() < kRappelDuree;
}

// ---------------------------------------------------------------- dessin
struct Vu {   // une ligne telle qu'elle se montre
    std::string nom, valeur, aide;
    Genre genre = G_CHOIX;
    float part = 0;         // curseur : part remplie
    bool relance = false;   // changée, vaut au prochain lancement
    bool attend = false;    // attend une touche ou un bouton
    bool alerte = false;    // valeur en ambre (« Confirmer ? »)
};
Vu voir(int vue, int i) {
    Vu v;
    if (vue == V_MANETTE || vue == V_CLAVIER) {
        const bool manette = vue == V_MANETTE;
        const int actions = manette ? Settings::PAD_NB : Settings::K_NB;
        v.genre = G_ACTION;
        if (i >= actions) {
            v.nom = T("remettre");
            v.aide = T("remettre_aide");
            return v;
        }
        v.nom = nom_bouton(i);
        v.attend = g_attente && g_sel[vue] == i;
        if (v.attend) v.valeur = T(manette ? "attente_bouton" : "attente_touche");
        else if (manette) v.valeur = nom_entree(settings().pad[i]);
        else v.valeur = settings().keys[i] ? hote::nom_touche(settings().keys[i]) : std::string(T("aucune_touche"));
        v.aide = T(v.attend ? (manette ? "attente_bouton_aide" : "attente_touche_aide") : manette ? "ligne_manette_aide" : "ligne_clavier_aide");
        if (manette && !v.attend) v.genre = G_CHOIX;   // Gauche et Droite la changent aussi
        return v;
    }
    const std::vector<int> lv = lignes_de(vue);
    const Ligne& l = g_lignes[lv[i]];
    v.genre = l.genre;
    v.nom = T(l.nom);
    v.aide = T((std::string(l.nom) + "_aide").c_str());
    if (l.v) {
        const int val = g_c.*l.v;
        v.valeur = l.texte(val);
        v.part = l.hi > l.lo ? (float)(val - l.lo) / (float)(l.hi - l.lo) : 0;
        v.relance = l.relance && val != g_depart.*l.v;
    }
    if (l.genre == G_PAGE) v.valeur = l.ouvre == V_MANETTE ? T(defaut_manette() ? "defaut" : "perso_m") : T(defaut_clavier() ? "defaut" : "perso_f");
    if (l.genre == G_ACTION && g_confirme && g_sel[vue] == i) v.valeur = T("confirmer"), v.alerte = true;
    if (l.v == &Choix::volume && !g_volume_garde) v.aide += std::string(" ") + T("volume_partie");
    return v;
}

void dessiner_menu(ui::Ctx& c) {
    using namespace ui;
    const float s = c.s, w = c.w, h = c.h;
    g_zones.clear();
    c.d->AddRectFilled(ImVec2(0, 0), ImVec2(w, h), kVoile);
    const float marge = std::max(12 * s, h * 0.03f), bord = 26 * s;
    const float pw = std::min(w - 2 * marge, 880 * s);
    const float h_entete = 58 * s, h_onglets = 42 * s, h_ligne = 38 * s, h_aide = 100 * s, h_invites = 46 * s;
    const int n = nb_lignes(g_vue);
    // la liste garde la hauteur de la vue la plus longue des onglets (le panneau ne change pas de taille d'un onglet à
    // l'autre), moins si la fenêtre est petite ; une page plus longue défile
    int visibles = 0;
    for (int v = 0; v < V_NB_ONGLETS; v++) visibles = std::max(visibles, nb_lignes(v));
    visibles = std::clamp(std::min(visibles, (int)((h - 2 * marge - h_entete - h_onglets - h_aide - h_invites) / h_ligne)), 3, 12);
    const float ph = h_entete + h_onglets + visibles * h_ligne + h_aide + h_invites;
    const ImVec2 a(std::floor((w - pw) * 0.5f), std::floor(std::max(marge * 0.5f, (h - ph) * 0.5f))), b(a.x + pw, a.y + ph);
    const float trait = std::max(1.0f, s);
    ombre(c, a, b, 26 * s, 0.9f);
    c.d->AddRectFilled(a, b, kFond, 5 * s);
    c.d->AddRect(a, b, kTrait, 5 * s, 0, trait);

    // ---- en-tête
    float y = a.y;
    texte_milieu(c, c.f.titre, 30 * s, a.x + bord, y + h_entete * 0.5f, kVert, T("titre"));
    texte_milieu_droite(c, c.f.texte, 13 * s, b.x - bord, y + h_entete * 0.5f, kGris, T("jeu_nom"));
    y += h_entete;
    c.d->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), kTrait, trait);

    // ---- onglets (et le titre de la page ouverte, à droite)
    const int onglet = g_vue < V_NB_ONGLETS ? g_vue : V_CONTROLES;
    static const char* const noms_onglets[V_NB_ONGLETS] = {"o_graphismes", "o_controles", "o_jeu"};
    float x = a.x + bord;
    for (int i = 0; i < V_NB_ONGLETS; i++) {
        const char* nom = T(noms_onglets[i]);
        const float tw = taille_de(c.f.titre, 18 * s, nom).x;
        texte_milieu(c, c.f.titre, 18 * s, x, y + h_onglets * 0.5f, i == onglet ? kVert : kGris, nom);
        if (i == onglet) c.d->AddRectFilled(ImVec2(x, y + h_onglets - 3 * s), ImVec2(x + tw, y + h_onglets), kVert);
        g_zones.push_back({ImVec2(x - 12 * s, y), ImVec2(x + tw + 12 * s, y + h_onglets), Z_ONGLET, i});
        x += tw + 34 * s;
    }
    if (g_vue >= V_NB_ONGLETS)
        texte_milieu_droite(c, c.f.titre, 15 * s, b.x - bord, y + h_onglets * 0.5f, kVertPale, T(g_vue == V_MANETTE ? "p_manette" : "p_clavier"));
    y += h_onglets;
    c.d->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), kTrait, trait);

    // ---- lignes
    int& sel = g_sel[g_vue];
    int& defil = g_defil[g_vue];
    sel = std::clamp(sel, 0, n - 1);
    if (sel < defil) defil = sel;
    if (sel >= defil + visibles) defil = sel - visibles + 1;
    defil = std::clamp(defil, 0, std::max(0, n - visibles));
    Vu choisie;
    const float px = 18 * s;
    for (int k = 0; k < visibles && defil + k < n; k++) {
        const int i = defil + k;
        const Vu v = voir(g_vue, i);
        const bool choisi = i == sel;
        if (choisi) choisie = v;
        const float ya = y + k * h_ligne, yb = ya + h_ligne, ym = (ya + yb) * 0.5f;
        g_zones.push_back({ImVec2(a.x, ya), ImVec2(b.x, yb), Z_LIGNE, i});
        if (choisi) {
            c.d->AddRectFilled(ImVec2(a.x + trait, ya), ImVec2(b.x - trait, yb), kFondLigne);
            c.d->AddRectFilled(ImVec2(a.x + trait, ya), ImVec2(a.x + trait + 3 * s, yb), kVert);
        }
        texte_milieu(c, c.f.texte, px, a.x + bord, ym, choisi ? kBlanc : kTexte, v.nom.c_str());
        const float xr = b.x - bord;
        const ImU32 col = v.alerte || v.attend ? kAmbre : choisi ? kVert : kVertPale;
        if (v.genre == G_CURSEUR) {
            const float fl = 16 * s, nombre = 64 * s, piste = 190 * s, x1 = xr - fl - nombre, x0 = x1 - piste, ep = 3 * s;
            c.d->AddRectFilled(ImVec2(x0, ym - ep), ImVec2(x1, ym + ep), alpha(kVertFonce, 0.45f), ep);
            c.d->AddRectFilled(ImVec2(x0, ym - ep), ImVec2(x0 + piste * v.part, ym + ep), choisi ? kVert : kVertFonce, ep);
            c.d->AddCircleFilled(ImVec2(x0 + piste * v.part, ym), 7 * s, choisi ? kBlanc : kVertPale);
            texte_milieu_droite(c, c.f.gras, px, xr - fl, ym, col, v.valeur.c_str());   // aligné sur les valeurs des choix
            g_zones.push_back({ImVec2(x0 - 8 * s, ya), ImVec2(x1 + 8 * s, yb), Z_CURSEUR, i});
        } else {
            const bool fleches = choisi && v.genre == G_CHOIX;
            const float fl = 16 * s;   // place d'une flèche
            const float place = pw * 0.52f - 2 * fl;
            const float vpx = taille_qui_tient(c.f.gras, px, 11 * s, s, place, v.valeur.c_str());
            const float tw = taille_de(c.f.gras, vpx, v.valeur.c_str()).x;
            const float tx = xr - fl - tw;
            texte_milieu(c, c.f.gras, vpx, tx, ym, col, v.valeur.c_str());
            if (fleches) {
                triangle(c, ImVec2(xr - 5 * s, ym), 10 * s, 1, kVert);
                triangle(c, ImVec2(tx - fl + 5 * s, ym), 10 * s, 0, kVert);
            } else if (v.genre == G_PAGE) {
                triangle(c, ImVec2(xr - 5 * s, ym), 10 * s, 1, choisi ? kVert : kGris);
            }
            if (v.genre == G_CHOIX) {   // moitié gauche : la valeur d'avant ; moitié droite : la suivante
                const float xm = tx + tw * 0.5f;
                g_zones.push_back({ImVec2(tx - fl - 8 * s, ya), ImVec2(xm, yb), Z_GAUCHE, i});
                g_zones.push_back({ImVec2(xm, ya), ImVec2(b.x, yb), Z_DROITE, i});
            }
            if (v.relance) texte_milieu_droite(c, c.f.texte, 12 * s, tx - fl - 14 * s, ym, kAmbre, T("relance"));
        }
    }
    if (n > visibles) {   // ascenseur
        const float piste = visibles * h_ligne, hb = piste * visibles / n, yb0 = y + piste * defil / n;
        c.d->AddRectFilled(ImVec2(b.x - 5 * s, yb0 + 2 * s), ImVec2(b.x - 2 * s, yb0 + hb - 2 * s), alpha(kVert, 0.55f), 2 * s);
    }
    y += visibles * h_ligne;
    c.d->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), kTrait, trait);

    // ---- aide de la ligne choisie, puis ce qui est en service ou ce qui attend le prochain lancement
    {
        const float largeur = pw - 2 * bord;
        float apx = 15 * s;
        std::string note;
        ImU32 note_col = kGris;
        if (choisie.relance) {
            note = T("relance_note"), note_col = kAmbre;
        } else if (g_vue == V_GRAPHISMES) {
            const std::string t = hote::technique(), carte = hote::carte();
            if (!t.empty()) note = T("en_service") + t;
            if (!carte.empty()) note += (note.empty() ? "" : "   ·   ") + std::string(T("carte_service")) + carte;
        } else if (g_vue == V_JEU) {
            note = T("tourne");
        }
        const float dispo = h_aide - 20 * s;
        auto hauteur = [&](float p) {
            float t = paragraphe(c, c.f.texte, p, ImVec2(0, 0), 0, choisie.aide.c_str(), largeur, false);
            if (!note.empty()) t += p * 0.55f + paragraphe(c, c.f.texte, p * 0.9f, ImVec2(0, 0), 0, note.c_str(), largeur, false);
            return t;
        };
        while (apx > 10 * s && hauteur(apx) > dispo) apx -= s;
        float ya = y + 10 * s;
        ya += paragraphe(c, c.f.texte, apx, ImVec2(a.x + bord, ya), choisie.attend ? kAmbre : kTexte, choisie.aide.c_str(), largeur);
        if (!note.empty()) paragraphe(c, c.f.texte, apx * 0.9f, ImVec2(a.x + bord, ya + apx * 0.55f), note_col, note.c_str(), largeur);
    }
    y += h_aide;
    c.d->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), kTrait, trait);

    // ---- invites
    {
        const char* entree = T("t_entree");
        const char* echap = T("t_echap");
        const bool page = g_vue >= V_NB_ONGLETS;
        std::vector<Invite> v;
        if (g_attente) {
            v.push_back({kTouche_, T("i_retour"), echap});
        } else {
            v.push_back({kHautBas, T("i_choisir")});
            if (choisie.genre == G_CHOIX || choisie.genre == G_CURSEUR) v.push_back({kGaucheDroite, T("i_changer")});
            if (choisie.genre >= G_PAGE || page) v.push_back({kValider, T("i_valider"), entree});
            if (!page) v.push_back({kOnglets, T("i_onglet")});
            v.push_back({kRetour, T(page ? "i_retour" : "i_fermer"), echap});
        }
        float ipx = 15 * s;
        const float place = pw - 2 * bord;
        while (ipx > 9 * s && invites(c, ImVec2(0, 0), v.data(), (int)v.size(), ipx, false) > place) ipx -= s;
        const float iw = invites(c, ImVec2(0, 0), v.data(), (int)v.size(), ipx, false), ih = ipx + 10 * s;
        invites(c, ImVec2(b.x - bord - iw, y + (h_invites - ih) * 0.5f), v.data(), (int)v.size(), ipx, true);
    }

    // ---- souris : la ligne sous le curseur est choisie quand il bouge (jamais pendant une attente)
    float mx, my;
    if (hote::souris(&mx, &my) && (std::fabs(mx - g_souris_x) > 1 || std::fabs(my - g_souris_y) > 1)) {
        const bool premier = g_souris_x < 0;
        g_souris_x = mx, g_souris_y = my;
        if (!premier && !g_attente) {
            for (const Zone& z : g_zones)
                if (z.genre == Z_LIGNE && mx >= z.a.x && mx < z.b.x && my >= z.a.y && my < z.b.y && sel != z.i) {
                    sel = z.i;
                    g_confirme = false;
                    g_pad_dernier = false;
                }
        }
    }
}

void dessiner_compteur(ui::Ctx& c) {
    using namespace ui;
    const float s = c.s, px = 17 * s;
    char t[32];
    snprintf(t, sizeof t, "%.0f FPS", hote::images_par_seconde());
    const ImVec2 tt = taille_de(c.f.gras, px, t);
    const float m = 12 * s, pad_x = 9 * s, hh = px + 10 * s;
    const ImVec2 b(c.w - m, m + hh), a(b.x - tt.x - 2 * pad_x, m);
    c.d->AddRectFilled(a, b, rvb(0x030604, 190), 4 * s);
    texte_milieu(c, c.f.gras, px, a.x + pad_x, (a.y + b.y) * 0.5f, kVert, t);
}

void dessiner_rappel(ui::Ctx& c) {
    using namespace ui;
    const float s = c.s, px = 15 * s, hh = px + 10 * s, m = 14 * s, pad_x = 10 * s;
    const Invite in{kTouche_, T("rappel"), "F1"};
    Ctx c2 = c;
    c2.pad = false;
    const float iw = invites(c2, ImVec2(0, 0), &in, 1, px, false);
    const ImVec2 a(m, c.h - m - hh - 2 * 6 * s), b(m + iw + 2 * pad_x, c.h - m);
    c.d->AddRectFilled(a, b, rvb(0x030604, 200), 5 * s);
    invites(c2, ImVec2(a.x + pad_x, a.y + 6 * s), &in, 1, px, true);
}

}  // namespace

// ---------------------------------------------------------------- interface (menu.h)
bool touche(unsigned tv, unsigned scancode, bool repetee) {
    // F1 ouvre et ferme toujours ; Inser aussi, sauf si le joueur l'a donnée à une action du jeu
    bool bascule = tv == TV_F1;
    if (tv == TV_INSER) {
        bascule = true;
        for (int i = 0; i < Settings::K_NB; i++) bascule = bascule && settings().keys[i] != SC_INSER;
    }
    const bool ouvert = g_ouvert.load();
    if (bascule && !(ouvert && g_attente)) {
        if (!repetee) {
            std::lock_guard<std::mutex> lk(g_ev_mx);
            g_evs.push_back({Ev::BASCULE});
        }
        return true;
    }
    if (!ouvert) return false;
    std::lock_guard<std::mutex> lk(g_ev_mx);
    Ev e{Ev::TOUCHE};
    e.tv = tv, e.sc = scancode, e.repetee = repetee;
    g_evs.push_back(e);
    return true;
}
void molette(int crans) {
    if (!g_ouvert.load() || !crans) return;
    std::lock_guard<std::mutex> lk(g_ev_mx);
    Ev e{Ev::MOLETTE};
    e.n = crans;
    g_evs.push_back(e);
}
void clic(int bouton) {
    if (!g_ouvert.load()) return;
    std::lock_guard<std::mutex> lk(g_ev_mx);
    Ev e{Ev::CLIC};
    e.n = bouton;
    g_evs.push_back(e);
}

void manette(uint32_t entrees, int32_t lx, int32_t ly) {
    g_pad.store(entrees, std::memory_order_relaxed);
    g_pad_lx.store(lx, std::memory_order_relaxed);
    g_pad_ly.store(ly, std::memory_order_relaxed);
}
bool masquer(bool neutre) {
    if (g_ouvert.load(std::memory_order_relaxed)) return true;
    if (g_relache.load(std::memory_order_relaxed)) {
        if (!neutre) return true;
        g_relache = false;
    }
    return false;
}
bool ouvert() { return g_ouvert.load(std::memory_order_relaxed); }

bool a_dessiner() {
    if (!g_pret) init();
    std::vector<Ev> evs;
    {
        std::lock_guard<std::mutex> lk(g_ev_mx);
        evs.swap(g_evs);
    }
    // (pendant l'attente d'un bouton, le bouton central et Back + Start sont des réponses, pas la fermeture)
    bool bascule = bascule_manette() && !(g_ouvert && g_attente);
    if (bascule) g_pad_dernier = true;
    for (const Ev& e : evs) {
        if (e.type == Ev::BASCULE) {
            bascule = true;
            g_pad_dernier = false;
        } else if (!g_ouvert) {
            continue;
        } else if (e.type == Ev::TOUCHE) {
            touche_menu(e);
        } else if (e.type == Ev::CLIC) {
            clic_menu(e.n);
        } else if (e.type == Ev::MOLETTE && !g_attente) {   // la molette fait défiler les lignes
            const int n = nb_lignes(g_vue);
            g_sel[g_vue] = std::clamp(g_sel[g_vue] - e.n, 0, n - 1);
            g_confirme = false;
        }
    }
    if (bascule) {
        if (g_ouvert) fermer();
        else ouvrir();
    } else if (g_ouvert) {
        manette_menu();
    }
    return g_ouvert || settings().show_fps || rappel_visible();
}

void dessiner(float w, float h) {
    g_h = h;
    ui::Ctx c{ImGui::GetBackgroundDrawList(), ui::polices(), ui::unite(h, settings().menu_size), w, h, g_pad_dernier};
    // points des boutons de façade dans les invites : 0 haut, 1 droite, 2 gauche, 3 bas
    auto point = [](int e) { return e == Settings::P_NORD ? 0 : e == Settings::P_EST ? 1 : e == Settings::P_OUEST ? 2 : 3; };
    c.point_valider = point(entree_valider());
    c.point_retour = point(entree_retour());
    if (settings().show_fps) dessiner_compteur(c);
    if (g_ouvert) dessiner_menu(c);
    else if (rappel_visible()) dessiner_rappel(c);
}

}  // namespace menu
}  // namespace hos
