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
    {"graphismes", L"GRAPHISMES", L"GRAPHICS"},
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
    {"natif", L"100 % (natif)", L"100% (native)"},
    // rendu 3D (page Graphismes) : toutes les mises à l'échelle sont dites expérimentales (demande de l'utilisateur)
    {"reg_natif_aide", L"sous 100 % : expérimental, plus rapide", L"below 100%: experimental, faster"},
    {"reg_natif_exp", L"expérimental : plus rapide, moins net", L"experimental: faster, less sharp"},
    {"reg_natif_dlss", L"expérimental : agrandi par le DLSS", L"experimental: enlarged by DLSS"},
    // page Graphismes (2026-11-12)
    {"gra_titre", L"GRAPHISMES", L"GRAPHICS"},
    {"reg_gpu", L"Carte graphique", L"Graphics card"},
    {"gpu_auto", L"Automatique", L"Automatic"},
    {"gpu_auto_carte", L"Auto : ", L"Auto: "},
    {"gpu_aide", L"changer de carte refait les shaders", L"changing it rebuilds the shaders"},
    {"gpu_recherche", L"recherche des cartes…", L"looking for graphics cards…"},
    {"gpu_aucune", L"aucune carte Vulkan trouvée", L"no Vulkan card found"},
    {"reg_dlss", L"DLSS (NVIDIA)", L"DLSS (NVIDIA)"},
    {"reg_dlss_aide", L"cartes RTX, remplace l'anticrénelage", L"RTX cards, replaces anti-aliasing"},
    {"reg_dlss_exp", L"mise à l'échelle expérimentale", L"experimental upscaling"},
    {"reg_dlss_rtx", L"cartes NVIDIA RTX seulement", L"NVIDIA RTX cards only"},
    {"dlss_indispo", L"Indisponible", L"Unavailable"},
    {"reg_dlss_manque", L"nvngx_dlss.dll manque à côté du jeu", L"nvngx_dlss.dll missing next to the game"},
    {"dlss_non", L"Désactivé", L"Off"},
    {"dlss_dlaa", L"DLAA (natif)", L"DLAA (native)"},
    {"dlss_qualite", L"Qualité (67 %)", L"Quality (67%)"},
    {"dlss_equilibre", L"Équilibré (58 %)", L"Balanced (58%)"},
    {"dlss_perf", L"Performances (50 %)", L"Performance (50%)"},
    {"dlss_ultra", L"Ultra perf. (33 %)", L"Ultra Perf. (33%)"},
    {"reg_dlss_modele", L"Modèle DLSS", L"DLSS model"},
    {"reg_dlss_modele_aide", L"4.5 : plus fin, plus coûteux", L"4.5: sharper, costlier"},
    {"dlss_4", L"DLSS 4 (modèle K)", L"DLSS 4 (model K)"},
    {"dlss_45", L"DLSS 4.5 (modèle M)", L"DLSS 4.5 (model M)"},
    {"rendu_dlss", L"%d %% (mode DLSS)", L"%d%% (DLSS mode)"},
    // FSR d'AMD (2026-11-12)
    {"reg_fsr", L"FSR (AMD)", L"FSR (AMD)"},
    {"reg_fsr_aide", L"toute carte, remplace l'anticrénelage", L"any card, replaces anti-aliasing"},
    {"reg_fsr_dlss", L"sans effet : le DLSS passe devant", L"no effect: DLSS takes priority"},
    {"reg_fsr_manque", L"fichiers d'AMD absents (outils\\fsr)", L"AMD's files missing (outils\\fsr)"},
    {"fsr_natif", L"Natif (100 %)", L"Native (100%)"},
    {"fsr_equilibre", L"Équilibré (59 %)", L"Balanced (59%)"},
    {"reg_fsr_version", L"Version FSR", L"FSR version"},
    {"reg_fsr_version_aide", L"auto : FSR 4 si la carte le propose", L"auto: FSR 4 if the card offers it"},
    {"rendu_fsr", L"%d %% (mode FSR)", L"%d%% (FSR mode)"},
    {"reg_natif_fsr", L"expérimental : agrandi par le FSR", L"experimental: enlarged by FSR"},
    {"gra_note", L"Le DLSS et le FSR remplacent l'anticrénelage du jeu. Le DLSS n'est proposé qu'avec une carte NVIDIA RTX "
                 L"(et nvngx_dlss.dll à côté du jeu) ; le FSR d'AMD marche sur toute carte : FSR 4 sur les cartes AMD qui "
                 L"le proposent, FSR 3.1 sur les autres. Les mises à l'échelle sont expérimentales (Qualité à Ultra perf., "
                 L"Rendu 3D sous 100 %) : la 3D est rendue plus petite puis agrandie. Changer de carte graphique refait "
                 L"la préparation des shaders. Valable au prochain lancement du jeu.",
     L"DLSS and FSR replace the game's anti-aliasing. DLSS is only offered with an NVIDIA RTX card (and nvngx_dlss.dll "
     L"next to the game); AMD FSR works on any card: FSR 4 on the AMD cards that offer it, FSR 3.1 on the others. "
     L"Upscaling options are experimental (Quality to Ultra Perf., 3D rendering below 100%): 3D is rendered smaller "
     L"then enlarged. Changing the graphics card redoes the shader preparation. Applies at the next game launch."},
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
    {"reg_dossier", L"Emplacement du jeu", L"Game location"},
    {"reg_dossier_aide", L"tout le dossier d'installation", L"the whole install folder"},
    {"reg_dossier_sources", L"dossier de développement : fixe", L"development folder: fixed"},
    {"deplacer", L"Déplacer…", L"Move…"},
    {"reg_raccourci", L"Raccourci sur le bureau", L"Desktop shortcut"},
    {"rac_absent", L"aucun pour l'instant", L"none yet"},
    {"rac_present", L"présent, vers ce launcher", L"present, to this launcher"},
    {"rac_ailleurs", L"présent, vers un autre dossier", L"present, to another folder"},
    {"rac_creer", L"CRÉER LE RACCOURCI", L"CREATE THE SHORTCUT"},
    {"rac_retirer", L"RETIRER LE RACCOURCI", L"REMOVE THE SHORTCUT"},
    {"rac_echec", L"Raccourci impossible à créer : voir lanceur.log.", L"Shortcut not created: see lanceur.log."},
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
                 L"prochain lancement du jeu. Boutons de la manette : onglet MANETTE.",
     L"Click a key to change it; a key already in use is swapped. Changes apply the next time the game starts. "
     L"Controller buttons: CONTROLLER tab."},
    // onglet Manette (2026-11-12) : manettes Xbox, PlayStation, Switch Pro et Joy-Con (SDL3)
    {"ong_clavier", L"CLAVIER", L"KEYBOARD"},
    {"ong_manette", L"MANETTE", L"CONTROLLER"},
    {"pad_branchee", L"Manette : %ls", L"Controller: %ls"},
    {"pad_autres", L"  (+ %d autre(s))", L"  (+ %d more)"},
    {"pad_aucune", L"Aucune manette branchée : cliquez sur un bouton (dessin ou liste), puis sur celui du dessin qui doit "
                   L"le faire ; s'il était pris, ils échangent. ‹ › : autres entrées. Valable au prochain lancement.",
     L"No controller connected: click a button (drawing or list), then the one on the drawing that should do it; if it "
     L"was in use, they swap. ‹ ›: other inputs. Applies at the next game launch."},
    {"pad_sans_sdl", L"SDL3 manque (outils\\sdl3) : manettes Xbox seulement. Cliquez sur un bouton (dessin ou liste), "
                     L"puis sur celui du dessin qui doit le faire. ‹ › : autres entrées.",
     L"SDL3 missing (outils\\sdl3): Xbox controllers only. Click a button (drawing or list), then the one on the "
     L"drawing that should do it. ‹ ›: other inputs."},
    {"pad_note", L"Cliquez sur un bouton (dessin ou liste), puis appuyez sur celui de la manette qui doit le faire, ou "
                 L"cliquez-le sur le dessin ; s'il était pris, ils échangent. ‹ › : autres entrées. Valable au prochain "
                 L"lancement.",
     L"Click a button (drawing or list), then press the controller button that should do it, or click it on the "
     L"drawing; if it was in use, they swap. ‹ ›: other inputs. Applies at the next game launch."},
    // pendant une attente (%ls : le bouton de la Switch qui attend) : manette branchée, ou pas (clic sur le dessin)
    {"pad_attente", L"%ls : appuyez sur le bouton de la manette qui doit le faire, ou cliquez-le sur le dessin.  Échap "
                    L"ou un clic ailleurs : annuler.",
     L"%ls: press the controller button that should do it, or click it on the drawing.  Esc or a click elsewhere: "
     L"cancel."},
    {"pad_attente_clic", L"%ls : cliquez sur le bouton du dessin qui doit le faire.  Échap ou un clic ailleurs : annuler.",
     L"%ls: click the button on the drawing that should do it.  Esc or a click elsewhere: cancel."},
    {"pad_defaut", L"PAR DÉFAUT", L"DEFAULTS"},
    {"pad_clic_g", L"Stick L ●", L"L stick ●"},
    {"pad_clic_d", L"Stick R ●", L"R stick ●"},
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
    // emplacement du jeu (issue GitHub n° 14). Pas de « % » dans dep_fini, dep_fini_copie ni dep_echec : affichés par
    // LANCEUR_STATUT, qui les passe à swprintf.
    {"dep_dialogue", L"Choisir le dossier où déplacer le jeu", L"Choose the folder to move the game to"},
    {"dep_question_meme",
     L"Déplacer SpaceAnimalsRecomp dans ce dossier ?\n\n%ls\n\nLe launcher se ferme, le dossier est déplacé, puis le launcher se rouvre. Le jeu n'est pas reconstruit.",
     L"Move SpaceAnimalsRecomp to this folder?\n\n%ls\n\nThe launcher closes, the folder is moved, then the launcher opens again. The game is not rebuilt."},
    {"dep_question_autre",
     L"Déplacer SpaceAnimalsRecomp dans ce dossier ?\n\n%ls\n\nC'est un autre disque : les fichiers sont d'abord copiés (quelques minutes). Ensuite le launcher se ferme, l'ancien dossier est supprimé, puis le launcher se rouvre. Le jeu n'est pas reconstruit.\n\nVotre copie du jeu n'est ni déplacée ni supprimée.",
     L"Move SpaceAnimalsRecomp to this folder?\n\n%ls\n\nIt is another drive: the files are copied first (a few minutes). Then the launcher closes, the old folder is deleted, and the launcher opens again. The game is not rebuilt.\n\nYour copy of the game is neither moved nor deleted."},
    {"dep_refus", L"Déplacement impossible : ", L"The game cannot be moved: "},
    {"dep_en_cours", L"DÉPLACEMENT", L"MOVING"},
    {"e_dep_verifier", L"Taille et place libre", L"Size and free space"},
    {"e_dep_copier", L"Copie des fichiers", L"Copying the files"},
    {"dep_cons_fini", L"Prêt : le launcher se ferme, puis se rouvre dans le nouveau dossier.",
     L"Ready: the launcher closes, then opens again in the new folder."},
    {"dep_fini", L"Déplacement terminé : le jeu est dans son nouveau dossier.", L"Move done: the game is in its new folder."},
    {"dep_fini_copie", L"Déplacement terminé. Votre copie du jeu est restée dans l'ancien dossier.",
     L"Move done. Your copy of the game stayed in the old folder."},
    {"dep_echec", L"Déplacement impossible : un fichier du dossier est sans doute ouvert dans un autre programme. Rien n'a changé.",
     L"The game was not moved: a file of the folder is probably open in another program. Nothing changed."},
    {"e_maj", L"Téléchargement et vérification", L"Download and check"},
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
