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
    std::string file;                // fichier de réglages utilisé
};

const Settings& settings();
void settings_init(int argc, char** argv);   // à appeler tôt dans main(), après l'ouverture du journal
// Code de langue rendu au jeu (nn::oe::GetDesiredLanguage) : réglage, ou langue de Windows si « auto »
std::string language_code();

}  // namespace hos
