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
constexpr const wchar_t* VERSION = L"1.0.3";            // si recomp/VERSION est absent
constexpr const wchar_t* NOM = L"StarFox Remake [recompiled]";
constexpr const wchar_t* DEPOT = L"SerenityG4K/SpaceAnimalsRecomp";   // releases GitHub (mises à jour)

enum class Page { Aucune, Reglages, Controles, Construction };
enum class Langue { Fr, En };

// zones cliquables, déclarées par le dessin à chaque image
// Touches du clavier : une par action de la manette (page Contrôles). Même ordre que Settings::keys du jeu
// (runtime/os/settings.h) et que TOUCHES_CLES (lanceur.cpp).
constexpr int NB_TOUCHES = 24;
enum Touche { K_A, K_B, K_X, K_Y, K_L, K_R, K_ZL, K_ZR, K_PLUS, K_MINUS, K_LSTICK, K_RSTICK, K_UP, K_DOWN, K_LEFT, K_RIGHT,
              K_LS_UP, K_LS_DOWN, K_LS_LEFT, K_LS_RIGHT, K_RS_UP, K_RS_DOWN, K_RS_LEFT, K_RS_RIGHT };

enum Zone : int {
    Z_AUCUNE = 0, Z_PRINCIPAL, Z_MENU_REGLAGES, Z_MENU_CONTROLES, Z_MENU_CONSTRUCTION, Z_MENU_QUITTER, Z_COPIE,
    Z_FR, Z_EN, Z_REDUIRE, Z_FERMER, Z_PARCOURIR, Z_FERMER_PAGE,
    Z_RES_G, Z_RES_D, Z_HZ_G, Z_HZ_D, Z_NATIF_G, Z_NATIF_D, Z_AFF_G, Z_AFF_D, Z_VOL_G, Z_VOL_D, Z_FOND_G, Z_FOND_D,
    Z_FILS_G, Z_FILS_D, Z_RECONSTRUIRE, Z_ANNULER, Z_JOURNAL, Z_LANGUE_G, Z_LANGUE_D,
    Z_AFF_PLEIN, Z_AFF_FEN, Z_MAJ, Z_MAJ_AUTO_G, Z_MAJ_AUTO_D, Z_VIB_G, Z_VIB_D, Z_MENU_RAPPORT, Z_DESINSTALLER, Z_MAJ_JEU,
    Z_PRESET_G, Z_PRESET_D,                                   // page Contrôles : préréglage du clavier
    Z_TOUCHE0, Z_TOUCHE_FIN = Z_TOUCHE0 + NB_TOUCHES - 1,     // page Contrôles : une case par touche
};

// tâche en cours, lancée par le launcher (même protocole de lignes « @… »)
enum class Tache { Construction, Shaders, MiseAJour };

struct ReglagesJeu {   // recomp.ini du jeu
    int hz = 60;                // 60, 90, 120
    int resolution = 0;         // 0 auto, 720, 1080, 1440, 2160
    bool natif = true;          // rendu interne à la résolution de sortie
    bool sans_bordure = true;   // plein écran sans bordure, sinon fenêtré
    std::string langue = "auto";   // langue du jeu (code de la console), auto = celle de Windows
    int vibration = 100;        // vibrations de la manette, en % (0 : aucune)
    // touches du clavier par action (scancodes, 0xE0xx : touche étendue ; 0 : aucune) ; par défaut les positions du jeu
    unsigned touches[NB_TOUCHES] = {0x2E, 0x2D, 0x2F, 0x2C, 0x10, 0x12, 0x13, 0x14, 0x32, 0x31, 0x21, 0x23,
                                    0xE048, 0xE050, 0xE04B, 0xE04D, 0x11, 0x1F, 0x1E, 0x20, 0x17, 0x25, 0x24, 0x26};
    bool touches_ecrites = false;   // recomp.ini contient des clés key_… (ou le joueur vient de changer une touche)
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
    // chemins
    std::wstring dossier_exe, recomp, jeu, extraits, ini, copie;
    bool copie_ok = false;
    std::wstring copie_message;
    bool jeu_pret = false, jeu_lance = false;
    bool jeu_obsolete = false;   // des étapes sont à refaire après une mise à jour : reconstruction proposée, jamais imposée
    std::wstring version;        // version installée (recomp/VERSION)
    // mises à jour (releases GitHub) : seulement pour un paquet installé (lanceur.exe à côté de recomp/)
    bool paquet = false, maj_auto = true;
    std::mutex maj_lk;
    std::wstring maj;            // version disponible, plus récente (vide : à jour ou pas encore vérifié)
    bool quitter_sans_question = false;   // fermeture pour installer une mise à jour
    bool sans_jeu = false;                // vérification (LANCEUR_ACTION=shaders) : shaders compilés, jeu non lancé
    ReglagesJeu reglages;
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
void jlog(const wchar_t* fmt, ...);   // une ligne horodatée dans lanceur.log (en anglais)

}  // namespace lz
