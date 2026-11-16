// Écriture de recomp.ini par le menu en jeu (settings.h) : les lignes « clé = valeur » des réglages changés sont
// remplacées dans le fichier, le reste (commentaires, sections, ordre, fins de ligne) est gardé tel quel, comme le
// fait le launcher (lanceur.cpp, reglages_ecrire). Sans Windows ni journal : vérifié hors du jeu.
#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace hos {
namespace {

std::string rogner(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// la ligne (sans sa fin de ligne) donne-t-elle une valeur à cette clé ?
bool ligne_de(const std::string& ligne, const std::string& cle) {
    const std::string t = rogner(ligne);
    if (t.size() <= cle.size() || t.compare(0, cle.size(), cle) != 0) return false;
    const size_t i = t.find_first_not_of(" \t", cle.size());
    return i != std::string::npos && t[i] == '=';
}

}  // namespace

std::vector<std::pair<std::string, std::string>> settings_lignes(const Settings& s) {
    std::vector<std::pair<std::string, std::string>> v = {
        {"refresh_rate", std::to_string(s.refresh_rate)},
        {"resolution", s.resolution ? std::to_string(s.resolution) : "auto"},
        {"render_scale", std::to_string(s.render_scale)},
        {"dlss", kModesDlss[std::clamp(s.dlss, 0, 5)]},
        {"dlss_model", s.dlss_modele == 2 ? "dlss45" : "dlss4"},
        {"fsr", kModesFsr[std::clamp(s.fsr, 0, 5)]},
        {"fsr_version", s.fsr_version == 4 ? "fsr4" : s.fsr_version == 3 ? "fsr3" : "auto"},
        {"fsr_sharpness", std::to_string(s.fsr_sharpness)},
        {"gpu", s.gpu.empty() ? "auto" : s.gpu},
        {"window_mode", s.borderless ? "borderless" : "windowed"},
        {"language", s.language.empty() ? "auto" : s.language},
        {"vibration", std::to_string(s.vibration)},
        {"show_fps", s.show_fps ? "true" : "false"},
        {"menu_size", s.menu_size == 1 ? "small" : s.menu_size == 3 ? "large" : "medium"},
    };
    for (int i = 0; i < Settings::K_NB; i++) {
        char b[16];
        snprintf(b, sizeof b, "0x%X", s.keys[i]);
        v.push_back({std::string("key_") + kNomsActions[i], b});
    }
    for (int i = 0; i < Settings::PAD_NB; i++)
        v.push_back({std::string("pad_") + kNomsActions[i], PAD_ENTREES[std::min<int>(s.pad[i], Settings::P_NB - 1)]});
    return v;
}

std::string ini_remplacer(const std::string& contenu, const std::vector<std::pair<std::string, std::string>>& cles) {
    // lignes sans leur fin ; la fin de ligne du fichier est celle de sa première ligne (CRLF du launcher, LF du modèle)
    const std::string fin = contenu.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    std::vector<std::string> v;
    for (size_t i = 0; i < contenu.size();) {
        size_t j = contenu.find('\n', i);
        if (j == std::string::npos) j = contenu.size();
        std::string l = contenu.substr(i, j - i);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        v.push_back(l);
        i = j + 1;
    }
    while (!v.empty() && rogner(v.back()).empty()) v.pop_back();
    bool section_clavier = false, section_manette = false;
    for (const std::string& l : v) {
        section_clavier = section_clavier || rogner(l) == "[keyboard]";
        section_manette = section_manette || rogner(l) == "[controller]";
    }
    for (const auto& [k, val] : cles) {
        bool fait = false;
        for (std::string& l : v)
            if (ligne_de(l, k)) l = k + " = " + val, fait = true;
        if (fait) continue;
        // clé absente : à la fin ; les touches et les boutons sous leur section, expliquée comme le launcher le fait
        if (k.rfind("key_", 0) == 0 && !section_clavier) {
            section_clavier = true;
            v.insert(v.end(), {"", "[keyboard]",
                               "; Touches du clavier, une par bouton de la manette : réglées depuis le menu en jeu (F1) ou la page",
                               "; Contrôles du launcher. Valeur : position de la touche (scancode ; 0xE0.. : touche étendue ; 0 : aucune).",
                               "; Keyboard keys, one per controller button: set from the in-game menu (F1) or the launcher's Controls",
                               "; page. Value: position of the key (scancode; 0xE0..: extended key; 0: no key)."});
        }
        if (k.rfind("pad_", 0) == 0 && !section_manette) {
            section_manette = true;
            v.insert(v.end(), {"", "[controller]",
                               "; Boutons de la manette, un par bouton de la Switch : réglés depuis le menu en jeu (F1) ou la page",
                               "; Contrôles du launcher. Valeur : entrée repérée par sa position (south : bouton du bas, A d'une manette",
                               "; Xbox, ✕ d'une PlayStation, B d'une Nintendo ; east, west, north, leftshoulder, lefttrigger, start, back,",
                               "; dpad_up… ; none).",
                               "; Controller buttons, one per Switch button: set from the in-game menu (F1) or the launcher's Controls",
                               "; page. Value: input by position (south: bottom button, A on Xbox, ✕ on PlayStation, B on Nintendo; east,",
                               "; west, north, leftshoulder, lefttrigger, start, back, dpad_up…; none)."});
        }
        v.push_back(k + " = " + val);
    }
    std::string r;
    for (const std::string& l : v) r += l + fin;
    return r;
}

bool settings_enregistrer(const std::vector<std::pair<std::string, std::string>>& cles) {
    const std::string chemin = settings().file;
    if (chemin.empty() || cles.empty()) return false;
    std::string contenu;
    if (FILE* f = fopen(chemin.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) contenu.append(b, n);
        fclose(f);
    }
    const std::string nouveau = ini_remplacer(contenu, cles);
    if (nouveau == contenu) return true;
    // écrit à côté puis mis en place d'un seul coup : un arrêt du jeu au mauvais moment ne laisse pas un fichier tronqué
    const std::string part = chemin + ".part";
    FILE* w = fopen(part.c_str(), "wb");
    if (!w) return false;
    const bool ecrit = fwrite(nouveau.data(), 1, nouveau.size(), w) == nouveau.size();
    if (fclose(w) != 0 || !ecrit) {
        remove(part.c_str());
        return false;
    }
#ifdef _WIN32
    const bool ok = MoveFileExA(part.c_str(), chemin.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
    const bool ok = rename(part.c_str(), chemin.c_str()) == 0;
#endif
    if (!ok) remove(part.c_str());
    return ok;
}

}  // namespace hos
