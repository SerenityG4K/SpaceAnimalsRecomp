// Textes du launcher, en français et en anglais.
#include "lanceur.h"

#include <cstring>

namespace lz {
namespace {

struct Texte {
    const char* cle;
    const wchar_t* fr;
    const wchar_t* en;
};

const Texte TEXTES[] = {
    {"sous_titre", L"REMAKE  [RECOMPILED]", L"REMAKE  [RECOMPILED]"},
    {"ligne_version", L"PORT NATIF PC  ·  VERSION %ls", L"NATIVE PC PORT  ·  VERSION %ls"},
    {"recompiler", L"RECOMPILER LE JEU", L"BUILD THE GAME"},
    {"jouer", L"JOUER", L"PLAY"},
    {"en_cours", L"RECOMPILATION", L"BUILDING"},
    {"shaders_en_cours", L"SHADERS", L"SHADERS"},
    {"maj_en_cours", L"MISE À JOUR", L"UPDATING"},
    {"maj_jeu", L"METTRE À JOUR", L"UPDATE GAME"},
    {"maj_dispo", L"MISE À JOUR %ls DISPONIBLE  ›", L"UPDATE %ls AVAILABLE  ›"},
    {"maj_echec", L"Mise à jour impossible : ", L"Update failed: "},
    {"affichage", L"AFFICHAGE", L"DISPLAY"},
    {"plein_maj", L"PLEIN ÉCRAN", L"FULL SCREEN"},
    {"fenetre_maj", L"FENÊTRÉ", L"WINDOWED"},
    {"jeu_obsolete", L"Mise à jour installée : le jeu doit être mis à jour.", L"Update installed: the game needs updating."},
    {"jeu_obsolete_aide", L"Seules les parties qui ont changé sont refaites.", L"Only the parts that changed are rebuilt."},
    {"en_jeu", L"EN JEU", L"IN GAME"},
    {"reglages", L"RÉGLAGES", L"SETTINGS"},
    {"controles", L"CONTRÔLES", L"CONTROLS"},
    {"construction", L"CONSTRUCTION", L"BUILD"},
    {"quitter", L"QUITTER", L"QUIT"},
    {"copie", L"COPIE", L"GAME COPY"},
    {"choisir", L"choisir  ›", L"choose  ›"},
    {"changer", L"changer  ›", L"change  ›"},
    {"copie_absente", L"Aucune pour l'instant : choisissez votre copie de Star Fox (.nup, mise à jour 2.0.0).",
     L"None yet: choose your Star Fox copy (.nup, update 2.0.0)."},
    {"copie_ok", L"Copie reconnue : Star Fox, mise à jour 2.0.0.", L"Copy recognized: Star Fox, update 2.0.0."},
    {"jeu_absent", L"Le jeu n'est pas encore construit.", L"The game is not built yet."},
    {"jeu_absent_aide", L"« Recompiler » le construit sur votre PC, à partir de votre copie.",
     L"“Build” builds it on your PC, from your own copy."},
    {"jeu_pret", L"Le jeu est prêt.", L"The game is ready."},
    {"jeu_pret_aide", L"Tout le jeu a été recompilé en C++ et construit sur ce PC.",
     L"The whole game was recompiled to C++ and built on this PC."},
    {"bandeau",
     L"STARFOX REMAKE [RECOMPILED]   ·   tout le jeu recompilé en C++, construit sur votre PC à partir de votre propre copie"
     L"   ·   rendu Vulkan jusqu'en 4K   ·   60 images par seconde   ·   shaders précompilés, sans saccades"
     L"   ·   manette et clavier   ·   aucune émulation   ·   vos fichiers ne quittent jamais votre PC   ·   ",
     L"STARFOX REMAKE [RECOMPILED]   ·   the whole game recompiled to C++, built on your PC from your own copy"
     L"   ·   Vulkan rendering up to 4K   ·   60 frames per second   ·   precompiled shaders, no stutter"
     L"   ·   gamepad and keyboard   ·   no emulation   ·   your files never leave your PC   ·   "},
    // réglages
    {"reg_titre", L"RÉGLAGES", L"SETTINGS"},
    {"reg_copie", L"Copie du jeu", L"Game copy"},
    {"parcourir", L"Parcourir…", L"Browse…"},
    {"aucune", L"aucune", L"none"},
    {"reg_res", L"Résolution", L"Resolution"},
    {"auto_ecran", L"Auto (écran)", L"Auto (screen)"},
    {"reg_res_aide", L"Auto : celle de votre écran", L"Auto: your screen's"},
    {"reg_hz", L"Images par s.", L"Frames per s."},
    {"reg_hz_aide", L"90 et 120 : pas encore stables", L"90 and 120: not yet stable"},
    {"reg_natif", L"Rendu 3D", L"3D rendering"},
    {"natif", L"Natif (net)", L"Native (sharp)"},
    {"console", L"Comme la console", L"Like the console"},
    {"reg_natif_aide", L"la console rend à 50 % puis agrandit", L"the console renders at 50% then upscales"},
    {"reg_aff", L"Affichage", L"Display"},
    {"sans_bordure", L"Plein écran", L"Full screen"},
    {"fenetre", L"Fenêtré", L"Windowed"},
    {"reg_aff_aide", L"Alt+Entrée bascule en jeu", L"Alt+Enter toggles in game"},
    {"reg_langue", L"Langue du jeu", L"Game language"},
    {"reg_langue_aide", L"Auto : celle de Windows", L"Auto: Windows language"},
    {"auto_langue", L"Auto (Windows)", L"Auto (Windows)"},
    {"reg_vol", L"Volume", L"Volume"},
    {"reg_fond", L"Fond animé", L"Background"},
    {"fond_extraits", L"Extraits du jeu", L"Game footage"},
    {"fond_sobre", L"Sobre", L"Plain"},
    {"reg_fond_aide", L"tirés de votre copie", L"taken from your copy"},
    {"reg_maj", L"Mises à jour", L"Updates"},
    {"reg_maj_aide", L"releases GitHub du projet", L"the project's GitHub releases"},
    {"maj_auto", L"Vérifier au démarrage", L"Check at startup"},
    {"maj_manuel", L"Jamais", L"Never"},
    {"experts", L"POUR EXPERTS", L"FOR EXPERTS"},
    {"reg_fils", L"Fils de compilation", L"Build threads"},
    {"auto_fils", L"Auto (%d)", L"Auto (%d)"},
    {"reconstruire", L"RECONSTRUIRE LE JEU", L"REBUILD THE GAME"},
    {"extraits_btn", L"RÉENREGISTRER LES EXTRAITS", L"RE-RECORD FOOTAGE"},
    // contrôles
    {"ctl_titre", L"CONTRÔLES", L"CONTROLS"},
    {"ctl_bouton", L"BOUTON SWITCH", L"SWITCH BUTTON"},
    {"ctl_clavier", L"CLAVIER", L"KEYBOARD"},
    {"ctl_manette", L"MANETTE XBOX", L"XBOX PAD"},
    {"stick_g", L"Stick gauche", L"Left stick"},
    {"stick_d", L"Stick droit", L"Right stick"},
    {"croix", L"Croix", L"D-pad"},
    {"ctl_note", L"Le clavier suit l'emplacement des touches (AZERTY comme QWERTY). La réassignation viendra plus tard.",
     L"Keys follow their physical position (AZERTY or QWERTY). Remapping will come later."},
    // construction
    {"cons_titre", L"CONSTRUCTION", L"BUILD"},
    {"e_verifier", L"Vérification de la copie", L"Checking the copy"},
    {"e_extraire", L"Extraction des exécutables", L"Extracting the executables"},
    {"e_images", L"Préparation des modules", L"Preparing the modules"},
    {"e_recompiler", L"Recompilation ARM64 → C++ → x86-64", L"Recompiling ARM64 → C++ → x86-64"},
    {"e_lier", L"Édition de liens", L"Linking"},
    {"e_shaders", L"Traduction des shaders", L"Translating the shaders"},
    {"e_pipelines", L"Compilation des shaders pour votre GPU", L"Compiling the shaders for your GPU"},
    {"e_extraits", L"Enregistrement des extraits", L"Recording the footage"},
    {"cons_ecoule", L"écoulé", L"elapsed"},
    {"cons_fini", L"Terminé : le jeu est prêt.", L"Done: the game is ready."},
    {"cons_echec", L"Échec : ", L"Failed: "},
    {"cons_annulee", L"Construction annulée.", L"Build cancelled."},
    {"cons_attente", L"Compter environ 35 min sur 16 fils, et 4 Go libres sur le disque. Le launcher reste utilisable.",
     L"Allow about 35 min on 16 threads and 4 GB of free disk space. The launcher stays usable."},
    {"cons_info_extraits", L"Le jeu s'ouvre quelques minutes, sans le son, pour enregistrer ses cinématiques.",
     L"The game opens for a few minutes, muted, to record its cutscenes."},
    {"cons_info_releve", L"Le jeu s'ouvre quelques secondes, sans le son, pour relever les shaders qu'il crée au démarrage.",
     L"The game opens for a few seconds, muted, to collect the shaders it creates when it starts."},
    {"deja_fait", L"déjà fait", L"already done"},
    {"annuler", L"ANNULER", L"CANCEL"},
    {"fermer", L"FERMER", L"CLOSE"},
    {"journal", L"JOURNAL", L"LOG"},
    // messages
    {"jeu_arrete", L"Le jeu s'est arrêté (code %d) : voir recomp_run.log.", L"The game stopped (code %d): see recomp_run.log."},
    {"pas_de_python", L"Python 3 introuvable : installez-le, ou placez-le dans outils\\python.",
     L"Python 3 not found: install it, or put it in outils\\python."},
    {"pas_de_copie", L"Choisissez d'abord votre copie du jeu.", L"Choose your game copy first."},
    {"filtre_nup", L"Copie de Star Fox (*.nup)", L"Star Fox copy (*.nup)"},
    {"dialogue_titre", L"Choisir votre copie de Star Fox", L"Choose your Star Fox copy"},
};

}  // namespace

const char* const LANGUES_JEU[] = {"auto", "fr", "en-US", "en-GB", "de", "es", "es-419", "it", "nl", "pt", "pt-BR", "ja", "ko",
                                   "zh-Hans", "zh-Hant"};
const int NB_LANGUES_JEU = (int)(sizeof LANGUES_JEU / sizeof LANGUES_JEU[0]);

std::wstring nom_langue_jeu(const std::string& code) {
    static const struct { const char* code; const wchar_t* nom; } N[] = {
        {"fr", L"Français"}, {"fr-CA", L"Français (Canada)"}, {"en-US", L"English (US)"}, {"en-GB", L"English (UK)"},
        {"de", L"Deutsch"}, {"es", L"Español (España)"}, {"es-419", L"Español (Latinoamérica)"}, {"it", L"Italiano"},
        {"nl", L"Nederlands"}, {"pt", L"Português"}, {"pt-BR", L"Português (Brasil)"}, {"ja", L"日本語"}, {"ko", L"한국어"},
        {"zh-Hans", L"简体中文"}, {"zh-Hant", L"繁體中文"}, {"ru", L"Русский"},
    };
    if (code == "auto") return T("auto_langue");
    for (auto& n : N)
        if (code == n.code) return n.nom;
    return std::wstring(code.begin(), code.end());
}

const wchar_t* T(const char* cle) {
    for (const Texte& t : TEXTES)
        if (!strcmp(t.cle, cle)) return g.langue == Langue::Fr ? t.fr : t.en;
    return L"?";
}

}  // namespace lz
