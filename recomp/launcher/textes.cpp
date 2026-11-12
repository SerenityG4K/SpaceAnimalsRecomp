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
    {"maj_question", L"La version %ls de SpaceAnimalsRecomp est disponible.\n\nL'installer maintenant ?\n\nNon : rien ne change, et le bandeau « MISE À JOUR » du launcher reste là pour plus tard.",
     L"Version %ls of SpaceAnimalsRecomp is available.\n\nInstall it now?\n\nNo: nothing changes, and the launcher's UPDATE banner stays there for later."},
    {"maj_jeu_question", L"Certaines améliorations nécessitent la reconstruction du jeu.\n\nSeules les parties qui ont changé sont refaites. La faire maintenant ?\n\nNon : vous continuez à jouer avec le jeu actuel, et vous pourrez la lancer plus tard depuis l'accueil du launcher.",
     L"Some improvements require rebuilding the game.\n\nOnly the parts that changed are rebuilt. Do it now?\n\nNo: you keep playing with the current game, and you can start it later from the launcher's home screen."},
    {"affichage", L"AFFICHAGE", L"DISPLAY"},
    {"plein_maj", L"PLEIN ÉCRAN", L"FULL SCREEN"},
    {"fenetre_maj", L"FENÊTRÉ", L"WINDOWED"},
    {"jeu_obsolete", L"Certaines améliorations nécessitent la reconstruction du jeu.", L"Some improvements require rebuilding the game."},
    {"jeu_obsolete_aide", L"RECONSTRUIRE MAINTENANT  ›", L"REBUILD NOW  ›"},
    {"jeu_obsolete_detail", L"seules les parties qui ont changé sont refaites", L"only the parts that changed are rebuilt"},
    {"en_jeu", L"EN JEU", L"IN GAME"},
    {"reglages", L"RÉGLAGES", L"SETTINGS"},
    {"controles", L"CONTRÔLES", L"CONTROLS"},
    {"construction", L"CONSTRUCTION", L"BUILD"},
    {"quitter", L"QUITTER", L"QUIT"},
    {"rapport", L"RAPPORT DE BUG", L"BUG REPORT"},
    {"rapport_en_cours", L"Rapport de bug en préparation…", L"Preparing the bug report…"},
    {"rapport_ok", L"Rapport créé : joignez SpaceAnimalsRecomp-bug-report.zip à une issue GitHub (le dossier vient de s'ouvrir).",
     L"Report ready: attach SpaceAnimalsRecomp-bug-report.zip to a GitHub issue (its folder just opened)."},
    {"rapport_echec", L"Rapport de bug impossible : voir lanceur.log.", L"Bug report failed: see lanceur.log."},
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
    {"reg_vib", L"Vibrations", L"Rumble"},
    {"reg_vib_aide", L"manettes Xbox, PS4, PS5, Pro", L"Xbox, PS4, PS5, Pro controllers"},
    {"vib_aucune", L"Aucune", L"Off"},
    {"reg_fond", L"Fond animé", L"Background"},
    {"fond_extraits", L"Extraits du jeu", L"Game footage"},
    {"fond_sobre", L"Sobre", L"Plain"},
    {"reg_fond_aide", L"fournis avec le launcher", L"provided with the launcher"},
    {"reg_maj", L"Mises à jour", L"Updates"},
    {"reg_maj_aide", L"releases GitHub du projet", L"the project's GitHub releases"},
    {"maj_auto", L"Vérifier au démarrage", L"Check at startup"},
    {"maj_manuel", L"Jamais", L"Never"},
    {"experts", L"POUR EXPERTS", L"FOR EXPERTS"},
    {"reg_fils", L"Fils de compilation", L"Build threads"},
    {"auto_fils", L"Auto (%d)", L"Auto (%d)"},
    {"reconstruire", L"RECONSTRUIRE LE JEU", L"REBUILD THE GAME"},
    // contrôles
    {"ctl_titre", L"CONTRÔLES", L"CONTROLS"},
    {"ctl_bouton", L"SWITCH", L"SWITCH"},
    {"ctl_clavier", L"CLAVIER", L"KEYBOARD"},
    {"ctl_manette", L"MANETTE", L"CONTROLLER"},
    {"stick_g", L"Stick gauche", L"Left stick"},
    {"stick_d", L"Stick droit", L"Right stick"},
    {"croix", L"Croix", L"D-pad"},
    {"ctl_note", L"Cliquez sur une touche pour la changer ; une touche déjà prise est échangée. Les changements valent au "
                 L"prochain lancement du jeu. Manettes Xbox, PS4, PS5 et Pro de la Switch : boutons par position, non modifiables.",
     L"Click a key to change it; a key already in use is swapped. Changes apply the next time the game starts. Xbox, PS4, "
     L"PS5 and Switch Pro controllers: buttons by position, not editable."},
    {"ctl_attente", L"Appuyez sur la touche voulue.  Échap ou un clic : annuler.",
     L"Press the key you want.  Esc or a click: cancel."},
    {"ctl_disposition", L"Touches du clavier", L"Keyboard keys"},
    {"ctl_perso", L"Personnalisées", L"Custom"},
    // construction
    {"cons_titre", L"CONSTRUCTION", L"BUILD"},
    {"e_verifier", L"Vérification de la copie", L"Checking the copy"},
    {"e_extraire", L"Extraction des exécutables", L"Extracting the executables"},
    {"e_images", L"Préparation des modules", L"Preparing the modules"},
    {"e_recompiler", L"Recompilation ARM64 → C++ → x86-64", L"Recompiling ARM64 → C++ → x86-64"},
    {"e_lier", L"Édition de liens", L"Linking"},
    {"e_shaders", L"Traduction des shaders", L"Translating the shaders"},
    {"e_pipelines", L"Compilation des shaders pour votre GPU", L"Compiling the shaders for your GPU"},
    {"cons_ecoule", L"écoulé", L"elapsed"},
    {"cons_fini", L"Terminé : le jeu est prêt.", L"Done: the game is ready."},
    {"cons_echec", L"Échec : ", L"Failed: "},
    {"cons_annulee", L"Construction annulée.", L"Build cancelled."},
    {"cons_attente", L"Compter environ 35 min sur 16 fils, et 4 Go libres sur le disque. Le launcher reste utilisable.",
     L"Allow about 35 min on 16 threads and 4 GB of free disk space. The launcher stays usable."},
    {"cons_info_releve", L"Le jeu tourne quelques secondes en arrière-plan, sans fenêtre ni son, pour relever les shaders qu'il crée au démarrage.",
     L"The game runs for a few seconds in the background, with no window and no sound, to collect the shaders it creates when it starts."},
    {"deja_fait", L"déjà fait", L"already done"},
    {"annuler", L"ANNULER", L"CANCEL"},
    {"fermer", L"FERMER", L"CLOSE"},
    {"journal", L"JOURNAL", L"LOG"},
    // messages
    {"jeu_arrete", L"Le jeu s'est arrêté (code %d). « RAPPORT DE BUG » crée le fichier à joindre à une issue.",
     L"The game stopped (code %d). “BUG REPORT” creates the file to attach to an issue."},
    // (pas de chiffre d'exigence : il sera donné à la sortie d'après les mesures ; le conseil du fichier d'échange vient
    // des arrêts du 2026-11-11 sur un PC de 32 Go au fichier d'échange fixe de 8 Go)
    {"memoire_faible", L"Mémoire libre : %.1f Go, c'est peu pour le jeu. Fermez les programmes lourds et laissez Windows gérer le fichier d'échange, ou recliquez pour lancer quand même.",
     L"Free memory: %.1f GB, that is low for the game. Close heavy programs and let Windows manage the page file, or click again to start anyway."},
    {"jeu_memoire", L"Mémoire insuffisante : fermez les programmes lourds (jeux, navigateur…), laissez Windows gérer le fichier d'échange, puis relancez.",
     L"Not enough memory: close heavy programs (games, browser…), let Windows manage the page file, then try again."},
    {"affichage_indisponible", L"Affichage impossible : Direct3D 11 ou Direct2D indisponible (détail dans lanceur.log). Mettez à jour le pilote de la carte graphique.",
     L"Cannot display: Direct3D 11 or Direct2D unavailable (details in lanceur.log). Update the graphics driver."},
    {"desinstaller", L"DÉSINSTALLER", L"UNINSTALL"},
    {"desinst_question", L"Désinstaller SpaceAnimalsRecomp ?\n\nSeront supprimés : le jeu construit, le launcher et ses outils (tout ce dossier) et la copie du compilateur.\n\nVotre copie du jeu n'est jamais supprimée.",
     L"Uninstall SpaceAnimalsRecomp?\n\nThis deletes the built game, the launcher and its tools (this whole folder) and the compiler copy.\n\nYour game copy is never deleted."},
    {"desinst_sauvegardes", L"Supprimer aussi vos sauvegardes ?\n\nNon : elles restent dans %APPDATA%\\SpaceAnimalsRecomp et seront reprises si vous réinstallez.",
     L"Delete your saves too?\n\nNo: they stay in %APPDATA%\\SpaceAnimalsRecomp and will be picked up if you reinstall."},
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
