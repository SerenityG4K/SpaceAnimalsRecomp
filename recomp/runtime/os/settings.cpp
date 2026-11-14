// Réglages de l'exécutable (voir settings.h) : recomp.ini, variables RECOMP_*, ligne de commande.
#include "settings.h"

#include "hos.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

namespace hos {
namespace {

Settings g_settings;
std::string g_file_label = "recomp.ini";
const char* g_src[3] = {"default", "default", "default"};   // fréquence, résolution, rendu interne
bool g_echelle_donnee = false;   // render_scale lu quelque part (sinon native_resolution = false vaut 67 %)

// Contenu du fichier créé au premier lancement (le launcher pourra le réécrire).
const char* const kTemplate = R"(; Réglages de recomp_main.exe, lus à chaque lancement.
; Ce fichier est prévu pour être écrit par le launcher ; il peut aussi être modifié à la main.
; Priorité : ce fichier < variables d'environnement RECOMP_* < options de la ligne de commande
; (--refresh, --resolution, --native-res, --config <fichier>).

[display]
; Fréquence d'images en Hz : 60, 90 ou 120 (30 à 240 acceptés, autres valeurs non testées).
; Le jeu se cale sur un écran virtuel à cette fréquence ; la simulation n'est pas accélérée.
refresh_rate = 60

; Résolution de sortie : auto (d'après l'écran principal), 720, 1080, 1440 ou 2160.
resolution = auto

; Rendu interne à la résolution de sortie (true),
; ou à 50 % puis agrandi comme sur la console (false).
native_resolution = true

; Échelle du rendu 3D, en % de la résolution de sortie : 100 (natif), 85, 75 ou 67. En dessous de 100 (mise à
; l'échelle expérimentale), le jeu rend plus petit puis agrandit l'image : plus rapide, un peu moins net (utile sur
; les petites cartes graphiques).
; 3D rendering scale, in % of the output resolution: 100 (native), 85, 75 or 67. Below 100 (experimental upscaling),
; the game renders smaller then enlarges the image: faster, slightly less sharp (useful on small graphics cards).
render_scale = 100

; Fenêtre : borderless (plein écran sans bordure, image pixel pour pixel) ou windowed (fenêtré).
; Alt+Entrée bascule pendant le jeu.
window_mode = borderless

[game]
; Langue du jeu : auto (celle de Windows), fr, en-US, en-GB, de, es, es-419, it, nl, pt, pt-BR, ja, ko,
; zh-Hans ou zh-Hant (codes de la console ; le jeu prend la plus proche de celles qu'il contient).
language = auto

[input]
; Vibrations de la manette, en % : 0 (aucune) à 100. Manettes Xbox, PlayStation 4 et 5, Pro de la Switch
; (SDL3), sinon Xbox seulement (XInput).
vibration = 100

)";

// Réglages apparus en 1.0.3 : à la suite du modèle, et ajoutés à la fin d'un recomp.ini d'une version précédente
// (load_file), pour que le joueur les trouve.
const char* const kSectionMemoire = R"([memory]
; Économie de mémoire : true (la mémoire du jeu est prise au fur et à mesure) ou false (comme jusqu'à la
; version 1.0.2 : tout est pris d'avance, plusieurs Go de plus). Mettre false si le jeu se comporte moins bien
; depuis la version 1.0.3 (images manquantes, arrêts).
; Memory saver: true (game memory is taken as needed) or false (as up to version 1.0.2: everything is taken
; up front, several GB more). Set it to false if the game misbehaves since version 1.0.3.
memory_saver = true
)";
const char* const kCleTextures = R"(
; Données des textures rendues à Windows une fois envoyées à la carte graphique : true ou false. Elles sont gardées
; dans un fichier temporaire (quelques Go dans le dossier temporaire de Windows, supprimé à la fermeture du jeu) et
; remises en place si le jeu les relit. Sans effet si memory_saver = false.
; Texture data given back to Windows once sent to the graphics card: true or false. It is kept in a temporary file
; (a few GB in the Windows temp folder, deleted when the game closes) and put back if the game reads it again.
; No effect if memory_saver = false.
texture_release = true
)";
const char* const kCleShaders = R"(
; Shaders préparés à leur premier usage (true), ou tous au démarrage du jeu comme jusqu'à la version 1.0.2 (false).
; Sans effet si memory_saver = false.
; Shaders prepared at first use (true), or all of them at game start as up to version 1.0.2 (false).
; No effect if memory_saver = false.
shaders_on_demand = true
)";
const char* const kCleVides = R"(
; Pages vides de la mémoire du jeu (que des zéros) rendues à Windows pendant la partie : true ou false. Le jeu les
; retrouve à zéro s'il y revient. Sans effet si memory_saver = false.
; Empty pages of game memory (only zeros) given back to Windows during the game: true or false. The game finds them
; as zeros if it comes back to them. No effect if memory_saver = false.
zero_page_release = true
)";
const char* const kCleFroides = R"(
; Pages pleines de la mémoire du jeu que le jeu n'a pas touchées depuis plusieurs minutes, rangées dans le fichier
; temporaire des textures : true ou false. Le jeu les retrouve telles quelles s'il y revient (courte attente). Sans
; effet si memory_saver, texture_release ou zero_page_release vaut false.
; Full pages of game memory the game has not touched for several minutes, put in the temporary file of the
; textures: true or false. The game finds them unchanged if it comes back to them (short wait). No effect if
; memory_saver, texture_release or zero_page_release is false.
cold_page_release = true
)";

const char* const kCleDlss = R"(
; DLSS de NVIDIA à la place de l'anticrénelage du jeu : off, dlaa (rendu natif), ou, en mise à l'échelle
; expérimentale, quality (rendu 3D à 67 %), balanced (58 %), performance (50 %) ou ultra_performance (33 %) : le DLSS
; agrandit alors l'image et render_scale ne sert pas. Cartes NVIDIA RTX seulement (voir gpu), avec nvngx_dlss.dll à
; côté de recomp_main.exe ; sinon le jeu garde son anticrénelage. dlss_model : dlss4 (modèle K) ou dlss45 (modèle M,
; plus fin, plus coûteux).
; NVIDIA DLSS instead of the game's anti-aliasing: off, dlaa (native rendering), or, as experimental upscaling,
; quality (3D rendering at 67%), balanced (58%), performance (50%) or ultra_performance (33%): DLSS then enlarges the
; image and render_scale is not used. NVIDIA RTX cards only (see gpu), with nvngx_dlss.dll next to recomp_main.exe;
; otherwise the game keeps its anti-aliasing. dlss_model: dlss4 (model K) or dlss45 (model M, sharper, costlier).
dlss = off
dlss_model = dlss4
)";

// Apparu après la 1.0.3 (optimisations du rendu du 2026-11-12)
const char* const kCleRendu = R"(
; Optimisations du rendu (dessins plus rapides à préparer, travail envoyé au GPU par morceaux) : true ou false. À
; mettre sur false seulement si un défaut d'image ou un plantage est apparu après une mise à jour : le jeu dessine
; alors comme la version 1.0.3, un peu moins vite dans les scènes chargées.
; Rendering optimizations (draws faster to prepare, work sent to the GPU in parts): true or false. Set it to false
; only if a picture defect or a crash appeared after an update: the game then draws like version 1.0.3, a little
; slower in heavy scenes.
fast_rendering = true
)";

// Apparu après la 1.0.3 (FSR d'AMD, 2026-11-12)
const char* const kCleFsr = R"(
; FSR d'AMD à la place de l'anticrénelage du jeu, sur toute carte graphique (AMD, NVIDIA, Intel) : off, native (rendu
; natif), ou, en mise à l'échelle expérimentale, quality (rendu 3D à 67 %), balanced (59 %), performance (50 %) ou
; ultra_performance (33 %) : le FSR agrandit alors l'image et render_scale ne sert pas. fsr_version : auto (la version
; la plus récente que la carte accepte : FSR 4 sur les cartes AMD qui le proposent, sinon FSR 3.1), fsr3 ou fsr4.
; Si le DLSS est demandé aussi et que la carte est une NVIDIA RTX, c'est le DLSS qui sert. Il faut les fichiers d'AMD
; (amd_fidelityfx_loader_vk.dll et amd_fidelityfx_upscaler_vk.dll) dans outils\fsr ; sinon le jeu garde son
; anticrénelage. fsr_sharpness : netteté ajoutée par le FSR, de 0 (aucune) à 100.
; AMD FSR instead of the game's anti-aliasing, on any graphics card (AMD, NVIDIA, Intel): off, native (native
; rendering), or, as experimental upscaling, quality (3D rendering at 67%), balanced (59%), performance (50%) or
; ultra_performance (33%): FSR then enlarges the image and render_scale is not used. fsr_version: auto (the newest
; version the card accepts: FSR 4 on the AMD cards that offer it, FSR 3.1 otherwise), fsr3 or fsr4. If DLSS is asked
; too and the card is an NVIDIA RTX, DLSS is used. AMD's files (amd_fidelityfx_loader_vk.dll and
; amd_fidelityfx_upscaler_vk.dll) are needed in outils\fsr; otherwise the game keeps its anti-aliasing.
; fsr_sharpness: sharpening added by FSR, from 0 (none) to 100.
fsr = off
fsr_version = auto
fsr_sharpness = 0
)";

// Apparu après la 1.0.3 (choix de la carte graphique dans le launcher)
const char* const kCleGpu = R"(
; Carte graphique : auto (la carte dédiée qui a le plus de mémoire vidéo), ou le nom de la carte voulue (choisie dans
; la page Graphismes du launcher ; un morceau du nom suffit, AMD par exemple). Le DLSS ne sert que sur une carte
; NVIDIA RTX. Changer de carte refait la préparation des shaders au lancement suivant.
; Graphics card: auto (the dedicated card with the most video memory), or the name of the wanted card (chosen in the
; launcher's Graphics page; part of the name is enough, AMD for example). DLSS only works on an NVIDIA RTX card.
; Changing the card redoes the shader preparation at the next launch.
gpu = auto
)";

// noms des actions dans les clés key_… et pad_… (ordre de Settings::keys ; les 16 premières sont les boutons)
const char* const kNomsActions[Settings::K_NB] = {
    "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "lstick", "rstick", "up", "down", "left", "right",
    "ls_up", "ls_down", "ls_left", "ls_right", "rs_up", "rs_down", "rs_left", "rs_right"};
// entrées des manettes (Settings::P_*), valeurs des clés pad_… ; même ordre que ENTREES_PAD (launcher/lanceur.cpp)
const char* const PAD_ENTREES[Settings::P_NB] = {
    "none", "south", "east", "west", "north", "back", "guide", "start", "leftstick", "rightstick", "leftshoulder",
    "rightshoulder", "dpad_up", "dpad_down", "dpad_left", "dpad_right", "misc1", "right_paddle1", "left_paddle1",
    "right_paddle2", "left_paddle2", "touchpad", "lefttrigger", "righttrigger"};

// codes de langue de la console (nn::settings::LanguageCode), écriture canonique
const char* const kLanguages[] = {"ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419", "nl",
                                  "pt", "pt-BR", "ru", "ko", "zh-Hans", "zh-Hant"};

std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool parse_int(const std::string& v, long* out) {   // tolère un suffixe (« 90 Hz », « 1080p »)
    std::string t = trim(v);
    char* end = nullptr;
    long n = strtol(t.c_str(), &end, 10);
    if (end == t.c_str()) return false;
    *out = n;
    return true;
}
bool parse_refresh(const std::string& v, int* out) {
    long n;
    if (!parse_int(v, &n) || n < 30 || n > 240) return false;
    *out = (int)n;
    return true;
}
bool parse_resolution(const std::string& v0, int* out) {
    std::string v = lower(trim(v0));
    if (v == "auto" || v == "0") {
        *out = 0;
        return true;
    }
    if (v == "4k" || v == "uhd") {
        *out = 2160;
        return true;
    }
    long n;
    if (!parse_int(v, &n) || (n != 720 && n != 1080 && n != 1440 && n != 2160)) return false;
    *out = (int)n;
    return true;
}
bool parse_bool(const std::string& v0, bool* out) {
    std::string v = lower(trim(v0));
    if (v == "1" || v == "true" || v == "yes" || v == "on" || v == "oui" || v == "vrai") {
        *out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off" || v == "non" || v == "faux") {
        *out = false;
        return true;
    }
    return false;
}

void apply(const std::string& key, const std::string& val, const char* src) {
    std::string k = lower(trim(key));
    std::string v = trim(val);
    if (k == "refresh_rate") {
        int r;
        if (parse_refresh(v, &r)) g_settings.refresh_rate = r, g_src[0] = src;
        else HLOG("cfg", "%s: refresh_rate = '%s' invalid (30 to 240 Hz): ignored", src, v.c_str());
    } else if (k == "resolution") {
        int r;
        if (parse_resolution(v, &r)) g_settings.resolution = r, g_src[1] = src;
        else HLOG("cfg", "%s: resolution = '%s' invalid (auto, 720, 1080, 1440, 2160): ignored", src, v.c_str());
    } else if (k == "native_resolution") {
        bool b;
        if (parse_bool(v, &b)) g_settings.native_resolution = b, g_src[2] = src;
        else HLOG("cfg", "%s: native_resolution = '%s' invalid (true / false): ignored", src, v.c_str());
    } else if (k == "render_scale") {
        const int e = atoi(v.c_str());
        if (e >= 50 && e <= 100) {
            g_settings.render_scale = std::max(e, 67), g_echelle_donnee = true, g_src[2] = src;
            if (e < 67) HLOG("cfg", "%s: render_scale = %d: 67 at least (below, the game takes the console's DLSS path)", src, e);
        } else {
            HLOG("cfg", "%s: render_scale = '%s' invalid (67 to 100): ignored", src, v.c_str());
        }
    } else if (k == "dlss") {
        static const char* const modes[] = {"off", "dlaa", "quality", "balanced", "performance", "ultra_performance"};
        const std::string m = lower(v);
        int i = 0;
        while (i < 6 && m != modes[i]) i++;
        if (i < 6) g_settings.dlss = i;
        else if (m == "0" || m == "false" || m == "non" || m == "no") g_settings.dlss = 0;
        else if (m == "on" || m == "true" || m == "native") g_settings.dlss = 1;
        else if (m == "ultra_perf" || m == "ultraperformance") g_settings.dlss = 5;
        else HLOG("cfg", "%s: dlss = '%s' invalid (off, dlaa, quality, balanced, performance, ultra_performance): ignored",
                  src, v.c_str());
    } else if (k == "dlss_model") {
        const std::string m = lower(v);
        if (m == "dlss4" || m == "4" || m == "k") g_settings.dlss_modele = 1;
        else if (m == "dlss45" || m == "dlss4.5" || m == "4.5" || m == "45" || m == "m") g_settings.dlss_modele = 2;
        else HLOG("cfg", "%s: dlss_model = '%s' invalid (dlss4, dlss45): ignored", src, v.c_str());
    } else if (k == "fsr") {
        static const char* const modes[] = {"off", "native", "quality", "balanced", "performance", "ultra_performance"};
        const std::string m = lower(v);
        int i = 0;
        while (i < 6 && m != modes[i]) i++;
        if (i < 6) g_settings.fsr = i;
        else if (m == "0" || m == "false" || m == "non" || m == "no") g_settings.fsr = 0;
        else if (m == "on" || m == "true" || m == "nativeaa" || m == "native_aa" || m == "aa") g_settings.fsr = 1;
        else if (m == "ultra_perf" || m == "ultraperformance") g_settings.fsr = 5;
        else HLOG("cfg", "%s: fsr = '%s' invalid (off, native, quality, balanced, performance, ultra_performance): ignored",
                  src, v.c_str());
    } else if (k == "fsr_version") {
        const std::string m = lower(v);
        if (m == "auto" || m.empty()) g_settings.fsr_version = 0;
        else if (m == "fsr3" || m == "3" || m == "fsr3.1" || m == "3.1") g_settings.fsr_version = 3;
        else if (m == "fsr4" || m == "4" || m == "fsr4.1" || m == "4.1") g_settings.fsr_version = 4;
        else HLOG("cfg", "%s: fsr_version = '%s' invalid (auto, fsr3, fsr4): ignored", src, v.c_str());
    } else if (k == "fsr_sharpness") {
        const int n = atoi(v.c_str());
        if (n >= 0 && n <= 100 && !v.empty() && isdigit((unsigned char)v[0])) g_settings.fsr_sharpness = n;
        else HLOG("cfg", "%s: fsr_sharpness = '%s' invalid (0 to 100): ignored", src, v.c_str());
    } else if (k == "gpu") {   // texte cherché dans le nom des cartes (vk_render.cpp, choisir_carte)
        g_settings.gpu = v.empty() || lower(v) == "auto" ? std::string("auto") : v;
    } else if (k == "window_mode") {
        std::string m = lower(v);
        if (m == "borderless" || m == "fullscreen" || m == "plein_ecran") g_settings.borderless = true;
        else if (m == "windowed" || m == "window" || m == "fenetre") g_settings.borderless = false;
        else HLOG("cfg", "%s: window_mode = '%s' invalid (borderless / windowed): ignored", src, v.c_str());
    } else if (k == "language") {
        const std::string l = lower(v);
        bool ok = l == "auto";
        if (ok) g_settings.language = "auto";
        for (const char* c : kLanguages)
            if (!ok && lower(c) == l) g_settings.language = c, ok = true;
        if (!ok) HLOG("cfg", "%s: language = '%s' invalid (auto, fr, en-US, ja...): ignored", src, v.c_str());
    } else if (k == "vibration") {
        long n;
        bool b;
        if (parse_int(v, &n) && n >= 0 && n <= 100) g_settings.vibration = (int)n;
        else if (parse_bool(v, &b)) g_settings.vibration = b ? 100 : 0;
        else HLOG("cfg", "%s: vibration = '%s' invalid (0 to 100): ignored", src, v.c_str());
    } else if (k == "memory_saver") {
        bool b;
        if (parse_bool(v, &b)) g_settings.memory_saver = b;
        else HLOG("cfg", "%s: memory_saver = '%s' invalid (true or false): ignored", src, v.c_str());
    } else if (k == "zero_page_release") {
        bool b;
        if (parse_bool(v, &b)) g_settings.zero_page_release = b;
        else HLOG("cfg", "%s: zero_page_release = '%s' invalid (true or false): ignored", src, v.c_str());
    } else if (k == "fast_rendering") {
        bool b;
        if (parse_bool(v, &b)) g_settings.fast_rendering = b;
        else HLOG("cfg", "%s: fast_rendering = '%s' invalid (true or false): ignored", src, v.c_str());
    } else if (k == "cold_page_release") {
        bool b;
        if (parse_bool(v, &b)) g_settings.cold_page_release = b;
        else HLOG("cfg", "%s: cold_page_release = '%s' invalid (true or false): ignored", src, v.c_str());
    } else if (k == "texture_release" || k == "shaders_on_demand") {
        bool b;
        if (parse_bool(v, &b)) (k == "texture_release" ? g_settings.texture_release : g_settings.shaders_on_demand) = b;
        else HLOG("cfg", "%s: %s = '%s' invalid (true or false): ignored", src, k.c_str(), v.c_str());
    } else if (k.rfind("pad_", 0) == 0) {   // bouton de manette d'une action (page Contrôles du launcher, onglet Manette)
        int i = 0, e = 0;
        while (i < Settings::PAD_NB && k.compare(4, std::string::npos, kNomsActions[i]) != 0) i++;
        const std::string m = lower(v);
        while (e < Settings::P_NB && m != PAD_ENTREES[e]) e++;
        if (i < Settings::PAD_NB && e < Settings::P_NB) g_settings.pad[i] = (uint8_t)e;
        else if (i >= Settings::PAD_NB) HLOG("cfg", "%s: unknown key '%s' ignored (pad_a ... pad_right)", src, k.c_str());
        else HLOG("cfg", "%s: %s = '%s' invalid (controller input such as east, south, lefttrigger; set it from the "
                         "launcher's Controls page): ignored", src, k.c_str(), v.c_str());
    } else if (k.rfind("key_", 0) == 0) {   // touche du clavier d'une action de la manette (page Contrôles du launcher)
        int i = 0;
        while (i < Settings::K_NB && k.compare(4, std::string::npos, kNomsActions[i]) != 0) i++;
        char* fin = nullptr;
        const unsigned long sc = strtoul(v.c_str(), &fin, 0);
        if (i < Settings::K_NB && fin != v.c_str() && (sc <= 0xFF || (sc >= 0xE000 && sc <= 0xE0FF))) g_settings.keys[i] = (unsigned)sc;
        else HLOG("cfg", "%s: %s = '%s' invalid (key scancode such as 0x11; set it from the launcher's Controls page): ignored",
                  src, k.c_str(), v.c_str());
    } else {
        HLOG("cfg", "%s: unknown key '%s' ignored", src, k.c_str());
    }
}

void load_file(const std::string& path, bool create) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (!create) {
            HLOG("cfg", "settings file not found: %s (default values)", path.c_str());
            return;
        }
        if (FILE* w = fopen(path.c_str(), "wb")) {
            fputs(kTemplate, w);
            fputs(kSectionMemoire, w);
            fputs(kCleTextures, w);
            fputs(kCleShaders, w);
            fputs(kCleVides, w);
            fputs(kCleFroides, w);
            fputs(kCleDlss, w);
            fputs(kCleFsr, w);
            fputs(kCleRendu, w);
            fputs(kCleGpu, w);
            fclose(w);
            HLOG("cfg", "%s created with default values", path.c_str());
        }
        return;
    }
    char line[1024];
    int n = 0;
    std::set<std::string> vues;   // clés présentes dans le fichier
    while (fgets(line, sizeof line, f)) {
        std::string s = line;
        if (n++ == 0 && s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF)
            s = s.substr(3);   // BOM UTF-8
        size_t c = s.find_first_of(";#");
        if (c != std::string::npos) s = s.substr(0, c);
        s = trim(s);
        if (s.empty() || s[0] == '[') continue;   // sections : simple rangement, ignorées
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            HLOG("cfg", "%s, line %d: expected 'key = value', ignored", g_file_label.c_str(), n);
            continue;
        }
        vues.insert(lower(trim(s.substr(0, eq))));
        apply(s.substr(0, eq), s.substr(eq + 1), g_file_label.c_str());
    }
    fclose(f);
    // recomp.ini d'une version précédente : les réglages apparus depuis sont ajoutés à la fin, avec leur explication ;
    // les lignes existantes ne sont pas touchées (pas pour un fichier donné par --config)
    if (create) {
        std::string ajout;
        if (!vues.count("memory_saver")) ajout += std::string("\n") + kSectionMemoire;
        if (!vues.count("texture_release")) ajout += kCleTextures;
        if (!vues.count("shaders_on_demand")) ajout += kCleShaders;
        if (!vues.count("zero_page_release")) ajout += kCleVides;
        if (!vues.count("cold_page_release")) ajout += kCleFroides;
        if (!vues.count("dlss")) ajout += kCleDlss;
        if (!vues.count("fsr")) ajout += kCleFsr;
        if (!vues.count("fast_rendering")) ajout += kCleRendu;
        if (!vues.count("gpu")) ajout += kCleGpu;
        if (!ajout.empty()) {
            if (FILE* w = fopen(path.c_str(), "ab")) {
                fputs(ajout.c_str(), w);
                fclose(w);
                HLOG("cfg", "%s: settings added since the previous version appended at the end", g_file_label.c_str());
            }
        }
    }
}

}  // namespace

const Settings& settings() { return g_settings; }

std::string language_code() {
    if (g_settings.language != "auto") return g_settings.language;
    // langue de l'interface de Windows → code de la console le plus proche
    const LANGID id = GetUserDefaultUILanguage();
    const int sub = SUBLANGID(id);
    switch (PRIMARYLANGID(id)) {
    case LANG_FRENCH: return sub == SUBLANG_FRENCH_CANADIAN ? "fr-CA" : "fr";
    case LANG_ENGLISH: return sub == SUBLANG_ENGLISH_UK || sub == SUBLANG_ENGLISH_AUS || sub == SUBLANG_ENGLISH_NZ ||
                                      sub == SUBLANG_ENGLISH_EIRE ? "en-GB" : "en-US";
    case LANG_GERMAN: return "de";
    case LANG_ITALIAN: return "it";
    case LANG_SPANISH: return sub == SUBLANG_SPANISH || sub == SUBLANG_SPANISH_MODERN ? "es" : "es-419";
    case LANG_DUTCH: return "nl";
    case LANG_PORTUGUESE: return sub == SUBLANG_PORTUGUESE_BRAZILIAN ? "pt-BR" : "pt";
    case LANG_RUSSIAN: return "ru";
    case LANG_JAPANESE: return "ja";
    case LANG_KOREAN: return "ko";
    case LANG_CHINESE: return sub == SUBLANG_CHINESE_SIMPLIFIED || sub == SUBLANG_CHINESE_SINGAPORE ? "zh-Hans" : "zh-Hant";
    default: return "en-US";
    }
}

void settings_init(int argc, char** argv) {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    dir = dir.substr(0, dir.find_last_of("\\/") + 1);
    std::string cfg;
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--config")) cfg = argv[i + 1];
    bool explicit_cfg = !cfg.empty();
    if (!explicit_cfg) cfg = dir + "recomp.ini";
    g_settings.file = cfg;
    g_file_label = cfg.substr(cfg.find_last_of("\\/") + 1);
    load_file(cfg, !explicit_cfg);
    if (const char* e = getenv("RECOMP_REFRESH")) apply("refresh_rate", e, "RECOMP_REFRESH");
    if (const char* e = getenv("RECOMP_RESOLUTION")) apply("resolution", e, "RECOMP_RESOLUTION");
    if (const char* e = getenv("RECOMP_NATIVE_RES")) apply("native_resolution", e, "RECOMP_NATIVE_RES");
    if (const char* e = getenv("RECOMP_RENDER_SCALE")) apply("render_scale", e, "RECOMP_RENDER_SCALE");
    if (const char* e = getenv("RECOMP_DLSS")) apply("dlss", e, "RECOMP_DLSS");
    if (const char* e = getenv("RECOMP_DLSS_MODEL")) apply("dlss_model", e, "RECOMP_DLSS_MODEL");
    if (const char* e = getenv("RECOMP_FSR")) apply("fsr", e, "RECOMP_FSR");
    if (const char* e = getenv("RECOMP_FSR_VERSION")) apply("fsr_version", e, "RECOMP_FSR_VERSION");
    if (const char* e = getenv("RECOMP_FSR_SHARPNESS")) apply("fsr_sharpness", e, "RECOMP_FSR_SHARPNESS");
    if (const char* e = getenv("RECOMP_GPU")) apply("gpu", e, "RECOMP_GPU");   // essais : la carte AMD intégrée…
    if (const char* e = getenv("RECOMP_WINDOW")) apply("window_mode", e, "RECOMP_WINDOW");
    if (const char* e = getenv("RECOMP_LANGUAGE")) apply("language", e, "RECOMP_LANGUAGE");
    if (const char* e = getenv("RECOMP_VIBRATION")) apply("vibration", e, "RECOMP_VIBRATION");
    if (const char* e = getenv("RECOMP_MEMORY_SAVER")) apply("memory_saver", e, "RECOMP_MEMORY_SAVER");
    if (getenv("RECOMP_EAGER_COMMIT")) apply("memory_saver", "false", "RECOMP_EAGER_COMMIT");   // ancien nom (essais)
    if (const char* e = getenv("RECOMP_TEX_RELEASE")) apply("texture_release", e, "RECOMP_TEX_RELEASE");
    if (const char* e = getenv("RECOMP_SHADERS_ON_DEMAND")) apply("shaders_on_demand", e, "RECOMP_SHADERS_ON_DEMAND");
    if (const char* e = getenv("RECOMP_ZERO_PAGE_RELEASE")) apply("zero_page_release", e, "RECOMP_ZERO_PAGE_RELEASE");
    if (const char* e = getenv("RECOMP_COLD_PAGE_RELEASE")) apply("cold_page_release", e, "RECOMP_COLD_PAGE_RELEASE");
    if (const char* e = getenv("RECOMP_FAST_RENDERING")) apply("fast_rendering", e, "RECOMP_FAST_RENDERING");
    for (int i = 1; i + 1 < argc; i++) {
        std::string a = argv[i];
        if (a == "--refresh") apply("refresh_rate", argv[++i], "command line");
        else if (a == "--resolution") apply("resolution", argv[++i], "command line");
        else if (a == "--native-res") apply("native_resolution", argv[++i], "command line");
        else if (a == "--window") apply("window_mode", argv[++i], "command line");
        else if (a == "--language") apply("language", argv[++i], "command line");
    }
    // ancien « Comme la console » (50 %, scène 3D noire) : 67 %, l'échelle la plus basse qui s'affiche
    if (!g_settings.native_resolution && !g_echelle_donnee) g_settings.render_scale = 67;
    const Settings& s = g_settings;
    char res[16];
    char echelle[24];
    if (s.render_scale >= 100) snprintf(echelle, sizeof echelle, "native");
    else snprintf(echelle, sizeof echelle, "%d %%", s.render_scale);
    if (s.resolution) snprintf(res, sizeof res, "%d", s.resolution);
    else snprintf(res, sizeof res, "auto");
    HLOG("cfg", "settings: %d Hz [%s], resolution %s [%s], internal rendering %s [%s], %s, language %s (%s), rumble %d %% (file %s)",
         s.refresh_rate, g_src[0], res, g_src[1], echelle, g_src[2],
         s.borderless ? "borderless fullscreen" : "windowed", s.language.c_str(), language_code().c_str(), s.vibration,
         s.file.c_str());
    if (s.dlss) {
        static const char* const modes[] = {"off", "DLAA", "Quality (67 %)", "Balanced (58 %)", "Performance (50 %)",
                                            "Ultra Performance (33 %)"};
        HLOG("cfg", "DLSS requested: %s, %s", modes[s.dlss], s.dlss_modele == 2 ? "DLSS 4.5 (model M)" : "DLSS 4 (model K)");
    }
    if (s.fsr) {
        static const char* const modes[] = {"off", "Native AA", "Quality (67 %)", "Balanced (59 %)", "Performance (50 %)",
                                            "Ultra Performance (33 %)"};
        HLOG("cfg", "AMD FSR requested: %s, version %s, sharpening %d %%", modes[s.fsr],
             s.fsr_version == 3 ? "FSR 3.1" : s.fsr_version == 4 ? "FSR 4" : "automatic", s.fsr_sharpness);
    }
    if (s.gpu != "auto") HLOG("cfg", "graphics card requested: '%s'", s.gpu.c_str());
    {   // touches du clavier changées par le joueur (page Contrôles du launcher)
        const Settings defaut;
        int n = 0;
        for (int i = 0; i < Settings::K_NB; i++) n += s.keys[i] != defaut.keys[i];
        if (n) HLOG("cfg", "keyboard: %d key(s) changed from the default layout (recomp.ini, key_...)", n);
        n = 0;
        for (int i = 0; i < Settings::PAD_NB; i++) n += s.pad[i] != defaut.pad[i];
        if (n) HLOG("cfg", "controller: %d button(s) changed from the default layout (recomp.ini, pad_...)", n);
    }
    // memory_saver = false : tout comme en 1.0.2, les deux autres économies comprises
    if (!g_settings.memory_saver)
        g_settings.texture_release = g_settings.shaders_on_demand = g_settings.zero_page_release = false;
    // pages oubliées : dans le fichier des textures, par le balayage des pages vides
    if (!g_settings.texture_release || !g_settings.zero_page_release) g_settings.cold_page_release = false;
    if (!g_settings.fast_rendering) {
        // Chaque optimisation lit sa variable d'essai à son premier usage, après ce chargement : les poser ici les
        // coupe toutes (vk_render.cpp, nvn_cmd.cpp, runtime.cpp).
        for (const char* v : {"RECOMP_NO_BANK_CACHE", "RECOMP_NO_DYN_CACHE", "RECOMP_NO_REC_CACHE", "RECOMP_NO_CALL_CACHE",
                              "RECOMP_NO_BIND_SKIP"})
            _putenv_s(v, "1");
        _putenv_s("RECOMP_PARTIAL_SUBMIT", "0");
        _putenv_s("RECOMP_SPIN_US", "0");
        HLOG("cfg", "fast_rendering = false: the rendering optimizations of 2026-11-12 are all off");
    }
    HLOG("cfg", "memory saver: %s", s.memory_saver ? "on (game memory committed on demand, pools imported by windows)"
                                                   : "off (as in 1.0.2: full commit, whole pools imported)");
    HLOG("cfg", "texture data released after upload: %s; shaders prepared at first use: %s; empty game pages given "
                "back: %s; forgotten game pages put in the temporary file: %s", s.texture_release ? "on" : "off",
         s.shaders_on_demand ? "on" : "off", s.zero_page_release ? "on" : "off", s.cold_page_release ? "on" : "off");
    if (s.refresh_rate != 60 && s.refresh_rate != 90 && s.refresh_rate != 120)
        HLOG("cfg", "%d Hz: untested value (60, 90 and 120 Hz are verified)", s.refresh_rate);
}

}  // namespace hos
