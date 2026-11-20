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
#include <cstdint>
#include <string>

namespace hos {

struct Settings {
    int refresh_rate = 60;           // Hz de l'écran virtuel : 60, 90 ou 120 (30 à 240 acceptés)
    int resolution = 0;              // 0 = auto (d'après l'écran de l'hôte), sinon 720 / 1080 / 1440 / 2160
    bool native_resolution = true;   // rendu interne à la résolution de sortie (sinon 50 % + agrandissement)
    // Échelle du rendu 3D en % de la résolution de sortie (67 à 100) : le moteur rend plus petit, fait son
    // anticrénelage temporel à cette taille, puis agrandit l'image (upsample.frag, bilinéaire). native_resolution =
    // false (ancien « Comme la console », scène 3D noire jusqu'au 2026-11-12 : à 50 %, réseaux DLSS et NDSR de la
    // console, intraduisibles, désormais coupés dans recomp_cvars) vaut 67 si render_scale n'est pas donné.
    int render_scale = 100;
    // DLSS de NVIDIA à la place de l'anticrénelage temporel du jeu (dlss.cpp) : 0 non, 1 DLAA (rendu natif), 2 qualité
    // (67 %), 3 équilibré (58 %), 4 performances (50 %), 5 ultra performances (33 %). En super résolution (2 à 5),
    // l'échelle du rendu 3D suit le mode au lieu de render_scale. Cartes NVIDIA RTX seulement, avec nvngx_dlss.dll à
    // côté de l'exécutable ; sinon le jeu garde son anticrénelage (à render_scale).
    int dlss = 0;
    int dlss_modele = 1;   // 1 : DLSS 4 (modèle K), 2 : DLSS 4.5 (modèle M)
    // FSR d'AMD à la place de l'anticrénelage temporel du jeu (fsr.cpp), sur toute carte : 0 non, 1 natif, 2 qualité
    // (67 %), 3 équilibré (59 %), 4 performances (50 %), 5 ultra performances (33 %). Le DLSS passe devant s'il est
    // demandé aussi et que la carte est une NVIDIA RTX. fsr_version : 0 auto (la plus récente que la carte accepte :
    // FSR 4 sur les cartes AMD qui le proposent, sinon FSR 3.1), 3 ou 4 imposé.
    int fsr = 0;
    int fsr_version = 0;
    int fsr_sharpness = 0;   // netteté ajoutée par le FSR (passe RCAS), en % : 0 aucune (par défaut) à 100
    // Carte graphique (clé gpu, écrite par la page Graphismes du launcher ; RECOMP_GPU pour les essais) : « auto », ou
    // un texte contenu dans le nom de la carte voulue (le launcher écrit le nom complet). Choix dans vk_render.cpp
    // (choisir_carte) : la première carte dont le nom le contient ; sinon la carte dédiée qui a le plus de mémoire vidéo.
    // Le DLSS ne sert que sur une carte NVIDIA RTX (dlss.cpp, carte_dlss).
    std::string gpu = "auto";
    bool borderless = true;         // plein écran sans bordure (image pixel pour pixel) ; sinon fenêtré (Alt+Entrée bascule)
    std::string language = "auto";  // langue du jeu (code Switch : fr, en-US, ja…), auto = celle de Windows
    int vibration = 100;             // intensité des vibrations de la manette, en % (0 : aucune)
    // Touches du clavier, une par action de la manette (recomp.ini, clés key_a, key_ls_up… écrites par la page
    // Contrôles du launcher) : scancodes, c'est-à-dire la position physique de la touche (0xE0xx : touche étendue, les
    // flèches par exemple) ; 0 = aucune touche. Valeurs par défaut : celles du jeu depuis la 1.0.0, par position
    // (W A S D sur un clavier QWERTY, Z Q S D sur un AZERTY). Même ordre que TOUCHES_CLES (launcher/lanceur.cpp).
    // Puis SL et SR de chaque Joy-Con (K_LSL … K_RSR, aucune touche par défaut). Une touche peut demander des
    // modificateurs (MODIF_CTRL, MODIF_SHIFT, MODIF_ALT, bits 16 à 18 ; « shift+0x11 » dans recomp.ini) : Maj+W et W peuvent
    // faire deux actions, la combinaison la plus précise l'emporte (input.cpp, touches_actives).
    enum { K_A, K_B, K_X, K_Y, K_L, K_R, K_ZL, K_ZR, K_PLUS, K_MINUS, K_LSTICK, K_RSTICK, K_UP, K_DOWN, K_LEFT, K_RIGHT,
           K_LS_UP, K_LS_DOWN, K_LS_LEFT, K_LS_RIGHT, K_RS_UP, K_RS_DOWN, K_RS_LEFT, K_RS_RIGHT, K_LSL, K_LSR, K_RSL,
           K_RSR, K_NB };
    static constexpr unsigned MODIF_CTRL = 1u << 16, MODIF_SHIFT = 1u << 17, MODIF_ALT = 1u << 18,
                              MODIF_MASK = MODIF_CTRL | MODIF_SHIFT | MODIF_ALT;
    unsigned keys[K_NB] = {0x2E, 0x2D, 0x2F, 0x2C, 0x10, 0x12, 0x13, 0x14, 0x32, 0x31, 0x21, 0x23,
                           0xE048, 0xE050, 0xE04B, 0xE04D, 0x11, 0x1F, 0x1E, 0x20, 0x17, 0x25, 0x24, 0x26, 0, 0, 0, 0};
    // Boutons de la manette (recomp.ini, clés pad_a… écrites par la page Contrôles du launcher, onglet Manette) : pour
    // chaque bouton de la Switch, dans l'ordre K_A … K_RIGHT, l'entrée de la manette qui le fait (input.cpp), repérée
    // par sa position comme SDL le fait : P_SUD est le bouton du bas (A d'une manette Xbox, ✕ d'une PlayStation, B
    // d'une Nintendo). Valeurs de recomp.ini : PAD_ENTREES (settings.cpp), même ordre que ENTREES_PAD du launcher.
    // Par défaut, la disposition par position de toujours : le bouton de droite fait A, celui du bas B…
    enum { P_AUCUNE, P_SUD, P_EST, P_OUEST, P_NORD, P_BACK, P_GUIDE, P_START, P_STICK_G, P_STICK_D, P_EPAULE_G,
           P_EPAULE_D, P_HAUT, P_BAS, P_GAUCHE, P_DROITE, P_MISC1, P_PALETTE_D1, P_PALETTE_G1, P_PALETTE_D2,
           P_PALETTE_G2, P_PAVE, P_GACHETTE_G, P_GACHETTE_D, P_NB };
    static constexpr int PAD_NB = 16;   // boutons K_A … K_RIGHT (les sticks restent les sticks)
    uint8_t pad[PAD_NB] = {P_EST, P_SUD, P_NORD, P_OUEST, P_EPAULE_G, P_EPAULE_D, P_GACHETTE_G, P_GACHETTE_D,
                           P_START, P_BACK, P_STICK_G, P_STICK_D, P_HAUT, P_BAS, P_GAUCHE, P_DROITE};
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
    // Optimisations du 2026-11-12 (jeux de descripteurs repris d'un dessin à l'autre, état dynamique gardé, étages de
    // textures sautés, caches d'enregistrement et d'appels indirects, attente active entre fils du jeu) : false les
    // coupe toutes d'un coup (comme la 1.0.3), à essayer si un défaut d'image ou un plantage apparaît.
    bool fast_rendering = true;
    // Mode souris du jeu (hle_kbm.cpp) : la manette émulée devient une paire de Joy-Con 2 (svc_hid.cpp) et la souris
    // du PC devient le capteur souris du Joy-Con 2 droit, que le jeu suit dans son mode souris. Le clavier pilote
    // toujours la manette (keys) ; les boutons de la souris appuient sur des boutons de la manette (mouse_buttons).
    bool mouse_keyboard = false;
    // HDR (hle_gfx.cpp, vk_render.cpp) : le jeu sort en HDR10 quand l'écran le permet (HDR activé dans Windows) ;
    // luminosité maximale de l'écran et blanc papier (blanc de l'interface et des scènes SDR), en nits, donnés au jeu.
    bool hdr = false;
    int hdr_peak = 1000;          // 400 à 10000
    int hdr_paper_white = 203;    // 80 à 500
    // Format de l'image (aspect_ratio ; hle_gfx.cpp gfx_virtual_display) : largeur : hauteur de l'écran virtuel annoncé
    // au jeu, qui rend alors nativement à ce format (champ de vision plus large). 16:9 (celui de la console, par
    // défaut), 21:9, 32:9… ; 0:0 (auto) : celui de l'écran du PC. Plus étroit que 16:9 : 16:9.
    int aspect_x = 16, aspect_y = 9;
    int mouse_speed = 100;   // déplacements de la souris envoyés au jeu, en % (10 à 1000)
    // action de la manette (indice K_A … K_RSR : bouton, direction d'un stick, SL ou SR ; -1 : aucune) de chaque entrée
    // de la souris : boutons gauche, droit, milieu, arrière (X1), avant (X2), molette vers le haut et vers le bas (un
    // appui bref par cran). Par défaut R et ZR, les deux boutons sous les doigts du Joy-Con 2 tenu en souris.
    enum { M_GAUCHE, M_DROIT, M_MILIEU, M_ARRIERE, M_AVANT, M_MOLETTE_HAUT, M_MOLETTE_BAS, M_NB };
    int8_t mouse_buttons[M_NB] = {K_R, K_ZR, K_RSTICK, -1, -1, -1, -1};
    // touche qui rend le curseur de Windows (scancode et modificateurs, comme keys ; 0 : aucune) ; un clic dans l'image
    // le reprend. F9 par défaut.
    unsigned mouse_release = 0x43;
    std::string file;               // fichier de réglages utilisé
};

const Settings& settings();
void settings_init(int argc, char** argv);   // à appeler tôt dans main(), après l'ouverture du journal
// Code de langue rendu au jeu (nn::oe::GetDesiredLanguage) : réglage, ou langue de Windows si « auto »
std::string language_code();

}  // namespace hos
