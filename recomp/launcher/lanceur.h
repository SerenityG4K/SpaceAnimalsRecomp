// Launcher du portage natif : état partagé entre la fenêtre (lanceur.cpp), le dessin (dessin.cpp), la vidéo
// de fond (video.cpp) et les textes (textes.cpp).
#pragma once
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
// API de Windows 10 (DPI par écran…) : MSYS2 les expose par défaut, WinLibs (outils/ du paquet) vise Windows 7
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace lz {

constexpr float LARGEUR = 1000.f, HAUTEUR = 600.f;   // taille logique de la fenêtre (DIP)
// Agrandissement de la fenêtre (demande de l'utilisateur, 2026-11-11) : 1200 × 720 sur un écran à 100 %. Le dessin et
// les clics restent dans le repère logique de 1000 × 600 : c'est l'échelle d'affichage (g.dpi) qui est multipliée
// (lanceur.cpp, dpi_fenetre). Réduit de lui-même sur un écran trop petit.
constexpr float ECHELLE = 1.2f;
constexpr const wchar_t* VERSION = L"1.1.0";            // si recomp/VERSION est absent
constexpr const wchar_t* NOM = L"StarFox Remake [recompiled]";
constexpr const wchar_t* DEPOT = L"SerenityG4K/SpaceAnimalsRecomp";   // releases GitHub (mises à jour)

enum class Page { Aucune, Reglages, Controles, Construction, Graphismes };
enum class Langue { Fr, En };

// zones cliquables, déclarées par le dessin à chaque image
// Touches du clavier : une par action de la manette (page Contrôles). Même ordre que Settings::keys du jeu
// (runtime/os/settings.h) et que TOUCHES_CLES (lanceur.cpp).
constexpr int NB_TOUCHES = 24;
enum Touche { K_A, K_B, K_X, K_Y, K_L, K_R, K_ZL, K_ZR, K_PLUS, K_MINUS, K_LSTICK, K_RSTICK, K_UP, K_DOWN, K_LEFT, K_RIGHT,
              K_LS_UP, K_LS_DOWN, K_LS_LEFT, K_LS_RIGHT, K_RS_UP, K_RS_DOWN, K_RS_LEFT, K_RS_RIGHT };
// Manette (page Contrôles, onglet Manette) : les 16 boutons de la Switch (ordre K_A … K_RIGHT), chacun donné à une entrée
// de la manette, repérée par sa position comme SDL le fait (P_SUD : bouton du bas, A d'une manette Xbox, ✕ d'une
// PlayStation, B d'une Nintendo). Mêmes valeurs que Settings::P_* du jeu (runtime/os/settings.h) ; noms dans recomp.ini :
// ENTREES_PAD (lanceur.cpp), mêmes que PAD_ENTREES (runtime/os/settings.cpp).
constexpr int NB_PAD = 16;
enum EntreePad { P_AUCUNE, P_SUD, P_EST, P_OUEST, P_NORD, P_BACK, P_GUIDE, P_START, P_STICK_G, P_STICK_D, P_EPAULE_G,
                 P_EPAULE_D, P_HAUT, P_BAS, P_GAUCHE, P_DROITE, P_MISC1, P_PALETTE_D1, P_PALETTE_G1, P_PALETTE_D2,
                 P_PALETTE_G2, P_PAVE, P_GACHETTE_G, P_GACHETTE_D, P_NB };

enum Zone : int {
    Z_AUCUNE = 0, Z_PRINCIPAL, Z_MENU_REGLAGES, Z_MENU_CONTROLES, Z_MENU_CONSTRUCTION, Z_MENU_QUITTER, Z_COPIE,
    Z_FR, Z_EN, Z_REDUIRE, Z_FERMER, Z_PARCOURIR, Z_FERMER_PAGE,
    Z_RES_G, Z_RES_D, Z_HZ_G, Z_HZ_D, Z_NATIF_G, Z_NATIF_D, Z_AFF_G, Z_AFF_D, Z_VOL_G, Z_VOL_D, Z_FOND_G, Z_FOND_D,
    Z_FILS_G, Z_FILS_D, Z_RECONSTRUIRE, Z_ANNULER, Z_JOURNAL, Z_LANGUE_G, Z_LANGUE_D,
    Z_AFF_PLEIN, Z_AFF_FEN, Z_MAJ, Z_MAJ_AUTO_G, Z_MAJ_AUTO_D, Z_VIB_G, Z_VIB_D, Z_MENU_RAPPORT, Z_DESINSTALLER, Z_MAJ_JEU,
    Z_PRESET_G, Z_PRESET_D,                                   // page Contrôles : préréglage du clavier
    Z_MENU_GRAPHISMES,                                        // page Graphismes (2026-11-12)
    Z_DLSS_G, Z_DLSS_D, Z_DLSSM_G, Z_DLSSM_D,                 // page Graphismes : mode et modèle du DLSS
    Z_FSR_G, Z_FSR_D, Z_FSRV_G, Z_FSRV_D,                     // page Graphismes : mode et version du FSR d'AMD
    Z_GPU_G, Z_GPU_D,                                         // page Graphismes : carte graphique
    Z_DEPLACER, Z_RACCOURCI,                                  // page Réglages : emplacement du jeu, raccourci
    Z_ONGLET_CLAVIER, Z_ONGLET_MANETTE, Z_PAD_DEFAUT,         // page Contrôles : onglets, manette par défaut
    Z_PAD0, Z_PAD_FIN = Z_PAD0 + NB_PAD - 1,                  // bouton de la Switch (liste) : attend l'entrée voulue
    Z_PADG0, Z_PADG_FIN = Z_PADG0 + NB_PAD - 1,               // ‹ : entrée précédente
    Z_PADD0, Z_PADD_FIN = Z_PADD0 + NB_PAD - 1,               // › : entrée suivante
    Z_ENTREE0, Z_ENTREE_FIN = Z_ENTREE0 + P_NB - 1,           // entrée de la manette sur le dessin (clic)
    Z_TOUCHE0, Z_TOUCHE_FIN = Z_TOUCHE0 + NB_TOUCHES - 1,     // page Contrôles : une case par touche
};

// tâche en cours, lancée par le launcher (même protocole de lignes « @… »)
enum class Tache { Construction, Shaders, MiseAJour, Deplacement };

// modes du DLSS dans recomp.ini (clé dlss), dans l'ordre de ReglagesJeu::dlss
inline const char* const DLSS_MODES[6] = {"off", "dlaa", "quality", "balanced", "performance", "ultra_performance"};
// échelle du rendu 3D de chaque mode, en % (0 : celle de render_scale)
inline const int DLSS_ECHELLES[6] = {0, 100, 67, 58, 50, 33};
// modes du FSR d'AMD dans recomp.ini (clé fsr), dans l'ordre de ReglagesJeu::fsr, et échelle du rendu 3D de chacun
inline const char* const FSR_MODES[6] = {"off", "native", "quality", "balanced", "performance", "ultra_performance"};
inline const int FSR_ECHELLES[6] = {0, 100, 67, 59, 50, 33};

struct ReglagesJeu {   // recomp.ini du jeu
    int hz = 60;                // 60, 90, 120
    int resolution = 0;         // 0 auto, 720, 1080, 1440, 2160
    int echelle = 100;          // échelle du rendu 3D en % : 100 (natif), 85, 75, 67 (render_scale)
    // DLSS de NVIDIA (dlss) : 0 non, 1 DLAA, 2 qualité, 3 équilibré, 4 performances, 5 ultra performances (valeurs de
    // recomp.ini : DLSS_MODES)
    int dlss = 0;
    int dlss_modele = 1;        // 1 : DLSS 4 (modèle K), 2 : DLSS 4.5 (modèle M) (dlss_model)
    // FSR d'AMD (fsr), sur toute carte : 0 non, 1 natif, 2 qualité, 3 équilibré, 4 performances, 5 ultra performances
    // (FSR_MODES) ; sans effet quand le DLSS sert (carte NVIDIA RTX et DLSS activé). fsr_version : 0 auto (FSR 4 si la
    // carte le propose, sinon FSR 3.1), 3 ou 4 imposé.
    int fsr = 0;
    int fsr_version = 0;
    // carte graphique (gpu) : « auto », ou le nom d'une carte (Carte::nom ; à la main, un morceau du nom suffit). Le mode
    // DLSS est gardé quand la carte n'est pas une NVIDIA RTX : grisé ici, sans effet dans le jeu (dlss.cpp, carte_dlss).
    std::string gpu = "auto";
    bool sans_bordure = true;   // plein écran sans bordure, sinon fenêtré
    std::string langue = "auto";   // langue du jeu (code de la console), auto = celle de Windows
    int vibration = 100;        // vibrations de la manette, en % (0 : aucune)
    // touches du clavier par action (scancodes, 0xE0xx : touche étendue ; 0 : aucune) ; par défaut les positions du jeu
    unsigned touches[NB_TOUCHES] = {0x2E, 0x2D, 0x2F, 0x2C, 0x10, 0x12, 0x13, 0x14, 0x32, 0x31, 0x21, 0x23,
                                    0xE048, 0xE050, 0xE04B, 0xE04D, 0x11, 0x1F, 0x1E, 0x20, 0x17, 0x25, 0x24, 0x26};
    bool touches_ecrites = false;   // recomp.ini contient des clés key_… (ou le joueur vient de changer une touche)
    // entrée de la manette de chaque bouton de la Switch (pad_…) ; par défaut, la disposition par position du jeu
    uint8_t manette[NB_PAD] = {P_EST, P_SUD, P_NORD, P_OUEST, P_EPAULE_G, P_EPAULE_D, P_GACHETTE_G, P_GACHETTE_D,
                               P_START, P_BACK, P_STICK_G, P_STICK_D, P_HAUT, P_BAS, P_GAUCHE, P_DROITE};
    bool manette_ecrite = false;    // recomp.ini contient des clés pad_… (ou le joueur vient de changer un bouton)
};

// Manettes vues par le launcher (manette.cpp, SDL3 : onglet Manette de la page Contrôles)
struct EtatManettes {
    bool sdl = false;        // SDL3.dll chargée et prête
    int nb = 0;              // manettes branchées
    std::wstring nom;        // nom de la première
    int famille = 0;         // de la première : 0 inconnue, 1 Xbox, 2 PlayStation, 3 Nintendo
    int type = -1;           // de la première : SDL_GamepadType (5 PS4, 6 PS5, 7 Pro, 8 et 9 Joy-Con seul, 10 paire…)
    uint32_t appuyees = 0;   // entrées appuyées sur toutes les manettes (un bit par EntreePad)
    float axes[4] = {};      // sticks de la première manette, de −1 à 1 : gauche x, y (vers le bas), droit x, y
};

// Carte graphique vue par Vulkan (lanceur.cpp, cartes_lister), comme le jeu la voit
struct Carte {
    std::string nom;          // nom Vulkan : celui que le jeu cherche dans recomp.ini (gpu)
    uint32_t fabricant = 0;   // 0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel
    bool dediee = false;      // carte dédiée (pas intégrée au processeur)
    uint64_t memoire = 0;     // mémoire vidéo (plus grand tas local)
};

struct Construction {
    std::mutex lk;
    Tache tache = Tache::Construction;
    bool active = false, finie = false, echec = false, annulee = false;
    std::vector<std::string> etapes;   // identifiants (preparer.py)
    std::vector<int> statut;           // 0 à faire, 1 en cours, 2 faite, 3 déjà faite
    int courante = -1;
    double progres = 0;                // de l'étape courante
    std::wstring info, erreur;
    std::vector<std::wstring> journal;
    ULONGLONG debut = 0, fin = 0;
    HANDLE job = nullptr, processus = nullptr;
};

struct App {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    float dpi = 96.f;
    Langue langue = Langue::Fr;
    Page page = Page::Aucune;
    int capture = -1;   // page Contrôles : action dont on attend la nouvelle touche ; -1 : aucune
    bool onglet_manette = false;   // page Contrôles : onglet Manette (sinon Clavier)
    int capture_pad = -1;          // onglet Manette : bouton de la Switch qui attend un appui sur la manette ; -1 : aucun
    uint32_t pad_tenues = 0;       // entrées déjà appuyées quand l'attente a commencé (ne comptent qu'une fois relâchées)
    // chemins
    std::wstring dossier_exe, recomp, jeu, extraits, ini, copie;
    bool copie_ok = false;
    std::wstring copie_message;
    bool jeu_pret = false, jeu_lance = false;
    bool jeu_obsolete = false;   // des étapes sont à refaire après une mise à jour : reconstruction proposée, jamais imposée
    std::wstring version;        // version installée (recomp/VERSION)
    // mises à jour (releases GitHub) : seulement pour un paquet installé (lanceur.exe à côté de recomp/)
    bool paquet = false, maj_auto = true;
    bool sources = false;        // dossier de développement (sources du launcher à côté) : jamais déplacé
    // déplacement de l'installation (page Réglages, deplacer.py) : dossier d'arrivée, sur un autre volume ou non
    std::wstring dep_vers;
    bool dep_autre_volume = false;
    std::mutex maj_lk;
    std::wstring maj;            // version disponible, plus récente (vide : à jour ou pas encore vérifié)
    bool quitter_sans_question = false;   // fermeture pour installer une mise à jour
    bool sans_jeu = false;                // vérification (LANCEUR_ACTION=shaders) : shaders compilés, jeu non lancé
    ReglagesJeu reglages;
    // cartes graphiques : remplies une fois par un fil au démarrage (cartes_lister), lisibles quand cartes_pretes est vrai
    std::vector<Carte> cartes;
    std::atomic<bool> cartes_pretes{false};
    int volume = 100;            // RECOMP_VOLUME
    bool fond_video = true;
    int fils = 0;                // 0 = automatique
    int fils_max = 8;
    // interaction
    float sx = -1, sy = -1;      // souris (DIP)
    bool bouton_bas = false;
    int zone_survol = Z_AUCUNE, zone_appui = Z_AUCUNE;
    std::wstring statut_jeu;     // message après une partie (code de sortie…)
    bool rapport_en_cours = false;   // RAPPORT DE BUG : rapport.py tourne
    Construction cons;
    ULONGLONG t0 = 0;
};

extern App g;

// textes (textes.cpp) : clé → texte dans la langue courante
const wchar_t* T(const char* cle);
// langues du jeu proposées (codes de la console) et leur nom dans leur propre langue
extern const char* const LANGUES_JEU[];
extern const int NB_LANGUES_JEU;
std::wstring nom_langue_jeu(const std::string& code);

// vidéo de fond (video.cpp)
void video_demarrer(const std::wstring& dossier);
void video_arreter();
int video_nombre();
// dernière image décodée (BGRA, lignes de haut en bas) ; rend false si rien de nouveau depuis `numero`
bool video_image(uint64_t& numero, std::vector<uint8_t>& px, uint32_t& w, uint32_t& h, double& t, double& duree);

// dessin (dessin.cpp)
bool dessin_init(HWND hwnd);
void dessin_redim();
void dessin_image();          // une image complète ; met à jour g.zone_survol
void dessin_liberer();
int zone_sous(float x, float y);   // zone cliquable sous le point (DIP), d'après la dernière image

// lanceur.cpp
std::string utf8(const std::wstring& w);
std::wstring wide(const std::string& s);
int fils_effectifs();
int touches_prereglage_courant();   // 0 : WASD (QWERTY), 1 : ZQSD (AZERTY), -1 : touches personnalisées
// Carte que le jeu prendra avec le réglage gpu (même règle que vk_render.cpp, choisir_carte) : indice dans g.cartes, -1
// si la liste n'est pas prête ou vide ; *nommee : désignée par le réglage (sinon choix automatique)
int carte_du_jeu(bool* nommee = nullptr);
// Le DLSS est-il possible sur cette carte ? Une NVIDIA RTX (même règle que dlss.cpp, carte_dlss) ; oui tant qu'on ne
// sait pas (liste pas prête, aucune carte Vulkan : le jeu tranchera)
bool dlss_possible();
std::wstring nom_carte(const std::string& nom);   // nom affiché : sans (TM) ni (R), « GeForce RTX 5070 Ti »
// Raccourci du launcher sur le bureau (page Réglages) : 0 aucun, 1 présent, vers ce launcher, 2 vers un autre dossier
int raccourci_etat();

// manette.cpp
void manettes_lire();                 // une fois par image sur l'onglet Manette : charge SDL3 la première fois
void manettes_fermer();               // rend les manettes et SDL (onglet quitté, jeu lancé)
const EtatManettes& manettes();       // dernier état lu
std::wstring nom_entree(int entree, int famille);   // « B / ○ » (famille 0), « ○ » (PlayStation)…
void jlog(const wchar_t* fmt, ...);   // une ligne horodatée dans lanceur.log (en anglais)

}  // namespace lz
