// Réglages de l'exécutable, prévus pour être écrits par le futur launcher.
//
// Sources, par priorité croissante :
//   1. valeurs par défaut ;
//   2. recomp.ini à côté de l'exécutable (ou --config <fichier>) ; créé avec les valeurs par défaut
//      s'il n'existe pas ;
//   3. variables d'environnement RECOMP_REFRESH, RECOMP_RESOLUTION, RECOMP_NATIVE_RES, RECOMP_WINDOW,
//      RECOMP_LANGUAGE (développement) ;
//   4. options --refresh <Hz>, --resolution <auto|720|1080|1440|2160>, --native-res <0|1>, --window, --language.
// Les réglages sont lus une fois au lancement : un changement s'applique au lancement suivant.
#pragma once
#include <string>

namespace hos {

struct Settings {
    int refresh_rate = 60;           // Hz de l'écran virtuel : 60, 90 ou 120 (30 à 240 acceptés)
    int resolution = 0;              // 0 = auto (d'après l'écran de l'hôte), sinon 720 / 1080 / 1440 / 2160
    bool native_resolution = true;   // rendu interne à la résolution de sortie (sinon 50 % + agrandissement)
    bool borderless = true;          // plein écran sans bordure (image pixel pour pixel) ; sinon fenêtré (Alt+Entrée bascule)
    std::string language = "auto";  // langue du jeu (code Switch : fr, en-US, ja…), auto = celle de Windows
    int vibration = 100;             // intensité des vibrations de la manette, en % (0 : aucune)
    // Touches du clavier, une par action de la manette (recomp.ini, clés key_a, key_ls_up… écrites par la page
    // Contrôles du launcher) : scancodes, c'est-à-dire la position physique de la touche (0xE0xx : touche étendue, les
    // flèches par exemple) ; 0 = aucune touche. Valeurs par défaut : celles du jeu depuis la 1.0.0, par position
    // (W A S D sur un clavier QWERTY, Z Q S D sur un AZERTY). Même ordre que TOUCHES_CLES (launcher/lanceur.cpp).
    enum { K_A, K_B, K_X, K_Y, K_L, K_R, K_ZL, K_ZR, K_PLUS, K_MINUS, K_LSTICK, K_RSTICK, K_UP, K_DOWN, K_LEFT, K_RIGHT,
           K_LS_UP, K_LS_DOWN, K_LS_LEFT, K_LS_RIGHT, K_RS_UP, K_RS_DOWN, K_RS_LEFT, K_RS_RIGHT, K_NB };
    unsigned keys[K_NB] = {0x2E, 0x2D, 0x2F, 0x2C, 0x10, 0x12, 0x13, 0x14, 0x32, 0x31, 0x21, 0x23,
                           0xE048, 0xE050, 0xE04B, 0xE04D, 0x11, 0x1F, 0x1E, 0x20, 0x17, 0x25, 0x24, 0x26};
    // Économie de mémoire (depuis la 1.0.3) : mémoire du jeu engagée à la demande (memory.cpp) et pools du jeu importés
    // par fenêtres (vk_render.cpp). false : comportement des versions 1.0.0 à 1.0.2 (tout engagé à la réservation,
    // pools importés en entier, tous au premier calcul à mémoire globale) : recours si un joueur a un problème.
    bool memory_saver = true;
    // Depuis la 1.0.3 aussi, et sans effet si memory_saver est faux (settings_init les met alors à faux) :
    // données des textures rendues à Windows après leur envoi au GPU, gardées dans un fichier temporaire (memory.cpp,
    // mem_release) ;
    bool texture_release = true;
    // bibliothèques de shaders créées au premier dessin quand pipelines.cache est complet, au lieu de toutes au
    // démarrage (vk_render.cpp, ensure_init).
    bool shaders_on_demand = true;
    // pages de la mémoire du jeu qui ne contiennent que des zéros, rendues à Windows pendant la partie (memory.cpp,
    // vides_balayer) ; le jeu les retrouve à zéro s'il y revient.
    bool zero_page_release = true;
    // pages pleines de la mémoire du jeu que le jeu n'a pas touchées depuis 4 minutes, rangées dans le fichier
    // temporaire des textures (memory.cpp, vides_balayer) ; le jeu les retrouve telles quelles s'il y revient. Faux
    // aussi sans texture_release ou zero_page_release (settings_init).
    bool cold_page_release = true;
    std::string file;                // fichier de réglages utilisé
};

const Settings& settings();
void settings_init(int argc, char** argv);   // à appeler tôt dans main(), après l'ouverture du journal
// Code de langue rendu au jeu (nn::oe::GetDesiredLanguage) : réglage, ou langue de Windows si « auto »
std::string language_code();

}  // namespace hos
