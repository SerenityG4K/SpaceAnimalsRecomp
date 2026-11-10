// Réglages de l'exécutable (voir settings.h) : recomp.ini, variables RECOMP_*, ligne de commande.
#include "settings.h"

#include "hos.h"

#include <windows.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hos {
namespace {

Settings g_settings;
std::string g_file_label = "recomp.ini";
const char* g_src[3] = {"défaut", "défaut", "défaut"};   // fréquence, résolution, rendu interne

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

; Fenêtre : borderless (plein écran sans bordure, image pixel pour pixel) ou windowed (fenêtré).
; Alt+Entrée bascule pendant le jeu.
window_mode = borderless

[game]
; Langue du jeu : auto (celle de Windows), fr, en-US, en-GB, de, es, es-419, it, nl, pt, pt-BR, ja, ko,
; zh-Hans ou zh-Hant (codes de la console ; le jeu prend la plus proche de celles qu'il contient).
language = auto
)";

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
        else HLOG("cfg", "%s : refresh_rate = « %s » invalide (30 à 240 Hz) : ignoré", src, v.c_str());
    } else if (k == "resolution") {
        int r;
        if (parse_resolution(v, &r)) g_settings.resolution = r, g_src[1] = src;
        else HLOG("cfg", "%s : resolution = « %s » invalide (auto, 720, 1080, 1440, 2160) : ignoré", src, v.c_str());
    } else if (k == "native_resolution") {
        bool b;
        if (parse_bool(v, &b)) g_settings.native_resolution = b, g_src[2] = src;
        else HLOG("cfg", "%s : native_resolution = « %s » invalide (true / false) : ignoré", src, v.c_str());
    } else if (k == "window_mode") {
        std::string m = lower(v);
        if (m == "borderless" || m == "fullscreen" || m == "plein_ecran") g_settings.borderless = true;
        else if (m == "windowed" || m == "window" || m == "fenetre") g_settings.borderless = false;
        else HLOG("cfg", "%s : window_mode = « %s » invalide (borderless / windowed) : ignoré", src, v.c_str());
    } else if (k == "language") {
        const std::string l = lower(v);
        bool ok = l == "auto";
        if (ok) g_settings.language = "auto";
        for (const char* c : kLanguages)
            if (!ok && lower(c) == l) g_settings.language = c, ok = true;
        if (!ok) HLOG("cfg", "%s : language = « %s » invalide (auto, fr, en-US, ja…) : ignoré", src, v.c_str());
    } else {
        HLOG("cfg", "%s : clé inconnue « %s » ignorée", src, k.c_str());
    }
}

void load_file(const std::string& path, bool create) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        if (!create) {
            HLOG("cfg", "fichier de réglages introuvable : %s (valeurs par défaut)", path.c_str());
            return;
        }
        if (FILE* w = fopen(path.c_str(), "wb")) {
            fputs(kTemplate, w);
            fclose(w);
            HLOG("cfg", "%s créé avec les valeurs par défaut", path.c_str());
        }
        return;
    }
    char line[1024];
    int n = 0;
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
            HLOG("cfg", "%s, ligne %d : « clé = valeur » attendu, ignorée", g_file_label.c_str(), n);
            continue;
        }
        apply(s.substr(0, eq), s.substr(eq + 1), g_file_label.c_str());
    }
    fclose(f);
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
    if (const char* e = getenv("RECOMP_WINDOW")) apply("window_mode", e, "RECOMP_WINDOW");
    if (const char* e = getenv("RECOMP_LANGUAGE")) apply("language", e, "RECOMP_LANGUAGE");
    for (int i = 1; i + 1 < argc; i++) {
        std::string a = argv[i];
        if (a == "--refresh") apply("refresh_rate", argv[++i], "ligne de commande");
        else if (a == "--resolution") apply("resolution", argv[++i], "ligne de commande");
        else if (a == "--native-res") apply("native_resolution", argv[++i], "ligne de commande");
        else if (a == "--window") apply("window_mode", argv[++i], "ligne de commande");
        else if (a == "--language") apply("language", argv[++i], "ligne de commande");
    }
    const Settings& s = g_settings;
    char res[16];
    if (s.resolution) snprintf(res, sizeof res, "%d", s.resolution);
    else snprintf(res, sizeof res, "auto");
    HLOG("cfg", "réglages : %d Hz [%s], résolution %s [%s], rendu interne %s [%s], %s, langue %s (%s) (fichier %s)",
         s.refresh_rate, g_src[0], res, g_src[1], s.native_resolution ? "natif" : "50 %", g_src[2],
         s.borderless ? "plein écran sans bordure" : "fenêtré", s.language.c_str(), language_code().c_str(), s.file.c_str());
    if (s.refresh_rate != 60 && s.refresh_rate != 90 && s.refresh_rate != 120)
        HLOG("cfg", "%d Hz : valeur non testée (60, 90 et 120 Hz sont vérifiées)", s.refresh_rate);
}

}  // namespace hos
