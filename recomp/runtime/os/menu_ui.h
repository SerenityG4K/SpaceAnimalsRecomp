// Allure du menu en jeu (menu.cpp) : les couleurs et l'écriture du launcher (vert sur noir, Bahnschrift), pour que le
// menu aille avec lui. Dessin sur les listes de Dear ImGui, sans texture ni icône : les touches et les boutons des
// invites sont dessinés. Les tailles sont des points d'une fenêtre de 720 lignes, multipliés par l'unité (unite()).
// D'après gpu/vk/vk_ui.h de Longshot (MagicBeanDev, GPLv3), voir menu.h.
#pragma once

#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

#include "imgui.h"

namespace hos {
namespace menu {
namespace ui {

// polices chargées par menu_vk.cpp (Bahnschrift comme le launcher, sinon Segoe UI, sinon celle de Dear ImGui)
struct Polices {
    ImFont* texte = nullptr;   // lignes, aide, invites
    ImFont* gras = nullptr;    // valeur choisie, boutons
    ImFont* titre = nullptr;   // titre, onglets
};
const Polices& polices();

constexpr ImU32 rvb(unsigned rgb, int a = 255) {
    return IM_COL32((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255, a);
}
inline ImU32 alpha(ImU32 c, float k) {
    const int a = (int)((c >> IM_COL32_A_SHIFT & 0xFF) * k);
    return (c & ~IM_COL32_A_MASK) | ((ImU32)std::clamp(a, 0, 255) << IM_COL32_A_SHIFT);
}

// ---- couleurs : celles du launcher (launcher/dessin.cpp) ----
constexpr ImU32 kVert = rvb(0x3CFF7A), kVertFonce = rvb(0x0FA14A), kVertPale = rvb(0x9FE8B8), kTexte = rvb(0xD8F7E3),
                kBlanc = rvb(0xEFFFF4), kGris = rvb(0x6F9A80), kAmbre = rvb(0xF2C14E), kNoir = rvb(0x030604),
                kFond = rvb(0x050B08, 250),        // le panneau
                kFondLigne = rvb(0x3CFF7A, 24),    // la ligne choisie
                kTrait = rvb(0x0FA14A, 170),       // filets
                kVoile = rvb(0x000000, 165),       // le jeu derrière le menu
                kTouche = rvb(0xD8F7E3), kEncre = rvb(0x06140C), kEteint = rvb(0x5C7868);

/// L'unité : pixels par point, d'après la hauteur de la fenêtre et la taille du menu (1 petit, 2 moyen, 3 grand)
inline float unite(float h, int taille) {
    const float k = taille == 1 ? 0.85f : taille == 3 ? 1.2f : 1.0f;
    return std::max(0.5f, h / 720.0f * k);
}

struct Ctx {
    ImDrawList* d;
    Polices f;
    float s;      // pixels par point
    float w, h;   // la fenêtre
    bool pad;     // la dernière entrée venait d'une manette : ses boutons dans les invites
    int point_valider = 1, point_retour = 3;   // boutons de façade qui valident et reviennent : 0 haut, 1 droite, 2 gauche, 3 bas
};

inline ImVec2 taille_de(ImFont* font, float px, const char* t, float retour = 0) {
    return font->CalcTextSizeA(px, FLT_MAX, retour, t);
}
inline float texte(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t) {
    c.d->AddText(font, px, p, col, t);
    return font->CalcTextSizeA(px, FLT_MAX, 0, t).x;
}
/// Plus grande taille, par pas, à laquelle le texte tient dans `place` (px s'il y tient ; jamais sous min_px)
inline float taille_qui_tient(ImFont* font, float px, float min_px, float pas, float place, const char* t) {
    while (px > min_px && taille_de(font, px, t).x > place) px -= pas;
    return std::max(px, min_px);
}
/// Texte renvoyé à la ligne à `largeur` pixels, lignes à 1,3 em (le renvoi de Dear ImGui les met à 1 em : trop serré
/// à lire) ; rend sa hauteur. Les sauts de ligne du texte sont gardés. dessine faux : mesure seulement
inline float paragraphe(Ctx& c, ImFont* font, float px, ImVec2 p, ImU32 col, const char* t, float largeur, bool dessine = true) {
    const char* fin = t + strlen(t);
    float y = 0;
    while (t < fin) {
        if (*t == '\n') {
            y += px * 1.3f;
            t++;
            continue;
        }
        const char* fin_ligne = strchr(t, '\n');
        if (!fin_ligne) fin_ligne = fin;
        const char* coupe = font->CalcWordWrapPosition(px, t, fin_ligne, largeur);
        if (coupe == t) {   // un mot plus large que la ligne : une lettre au moins, entière en UTF-8
            coupe = t + 1;
            while (coupe < fin_ligne && ((unsigned char)*coupe & 0xC0) == 0x80) coupe++;
        }
        if (dessine) c.d->AddText(font, px, ImVec2(p.x, p.y + y), col, t, coupe);
        y += px * 1.3f;
        t = coupe;
        while (t < fin_ligne && *t == ' ') t++;
        if (t == fin_ligne && t < fin) t++;
    }
    return y > 0 ? y - px * 0.3f : 0;
}
/// Haut du texte pour des capitales centrées sur y : d'après la montée de la police, capitales de 0,7 em
inline float haut_centre(ImFont* font, float px, float y) { return y - font->GetFontBaked(px)->Ascent + 0.35f * px; }
inline float texte_milieu(Ctx& c, ImFont* font, float px, float x, float y, ImU32 col, const char* t) {
    return texte(c, font, px, ImVec2(x, haut_centre(font, px, y)), col, t);
}
inline void texte_milieu_centre(Ctx& c, ImFont* font, float px, float cx, float y, ImU32 col, const char* t) {
    texte_milieu(c, font, px, cx - taille_de(font, px, t).x * 0.5f, y, col, t);
}
inline void texte_milieu_droite(Ctx& c, ImFont* font, float px, float x, float y, ImU32 col, const char* t) {
    texte_milieu(c, font, px, x - taille_de(font, px, t).x, y, col, t);
}
inline void triangle(Ctx& c, ImVec2 m, float taille, int dir, ImU32 col) {   // dir : 0 gauche, 1 droite, 2 haut, 3 bas
    const float a = taille * 0.5f, b = taille * 0.42f;
    switch (dir) {
    case 0: c.d->AddTriangleFilled(m + ImVec2(b, -a), m + ImVec2(-b, 0), m + ImVec2(b, a), col); break;
    case 1: c.d->AddTriangleFilled(m + ImVec2(-b, -a), m + ImVec2(b, 0), m + ImVec2(-b, a), col); break;
    case 2: c.d->AddTriangleFilled(m + ImVec2(-a, b), m + ImVec2(0, -b), m + ImVec2(a, b), col); break;
    default: c.d->AddTriangleFilled(m + ImVec2(-a, -b), m + ImVec2(a, -b), m + ImVec2(0, b), col); break;
    }
}
/// Ombre douce sous un rectangle : couches qui s'élargissent en s'effaçant
inline void ombre(Ctx& c, ImVec2 a, ImVec2 b, float etendue, float force) {
    const int n = 12;
    for (int i = n; i >= 1; i--) {
        const float k = (float)i / n, e = etendue * k;
        c.d->AddRectFilled(a - ImVec2(e, e), b + ImVec2(e, e), IM_COL32(0, 0, 0, (int)(255 * force * (1 - k) * (1 - k) / 3.5f)), e);
    }
}

// ---- les invites : les touches, ou les boutons d'une manette repérés par leur position (toute disposition) ----
enum Signe { kHautBas, kGaucheDroite, kOnglets, kValider, kRetour, kTouche_ };
struct Invite {
    Signe g;
    const char* quoi;
    const char* touche = nullptr;   // kTouche_ : le nom d'une touche, montré au clavier comme à la manette
};
/// Un signe en p (son coin haut gauche ; la ligne fait px + 10 points de haut) ; rend sa largeur. dessine faux : mesure
inline float signe(Ctx& c, float px, ImVec2 p, const Invite& in, bool dessine) {
    const float s = c.s, hh = px + 10 * s, my = p.y + hh * 0.5f, kpx = px * 0.92f;
    auto touche = [&](float x, const char* nom) {   // le capuchon d'une touche, son nom dedans
        const float w = nom ? std::max(hh, taille_de(c.f.gras, kpx, nom).x + 14 * s) : hh;
        if (dessine) {
            c.d->AddRectFilled(ImVec2(x, p.y), ImVec2(x + w, p.y + hh), kTouche, 4 * s);
            if (nom) texte_milieu_centre(c, c.f.gras, kpx, x + w * 0.5f, my, kEncre, nom);
        }
        return w;
    };
    if (in.g == kTouche_) return touche(p.x, in.touche);
    if (!c.pad) {
        switch (in.g) {
        case kHautBas:
        case kGaucheDroite: {
            const bool v = in.g == kHautBas;
            const float w = touche(p.x, nullptr);
            touche(p.x + w + 3 * s, nullptr);
            if (dessine) {
                triangle(c, ImVec2(p.x + w * 0.5f, my), 8 * s, v ? 2 : 0, kEncre);
                triangle(c, ImVec2(p.x + w * 1.5f + 3 * s, my), 8 * s, v ? 3 : 1, kEncre);
            }
            return 2 * w + 3 * s;
        }
        case kOnglets: {
            const float w = touche(p.x, "Q");
            return w + 3 * s + touche(p.x + w + 3 * s, "E");
        }
        case kValider: return touche(p.x, in.touche ? in.touche : "Enter");
        default: return touche(p.x, in.touche ? in.touche : "Esc");
        }
    }
    const float r = hh * 0.5f;
    const ImVec2 m(p.x + r, my);
    if (in.g == kOnglets) {   // les boutons d'épaule
        float x = p.x;
        for (const char* l : {"L", "R"}) {
            const float w = hh * 1.25f;
            if (dessine) {
                c.d->AddRectFilled(ImVec2(x, p.y + 2 * s), ImVec2(x + w, p.y + hh - 2 * s), kTouche, (hh - 4 * s) * 0.5f);
                texte_milieu_centre(c, c.f.gras, px * 0.8f, x + w * 0.5f, my, kEncre, l);
            }
            x += w + 3 * s;
        }
        return x - p.x - 3 * s;
    }
    if (!dessine) return 2 * r;
    c.d->AddCircleFilled(m, r, kTouche);
    if (in.g == kHautBas || in.g == kGaucheDroite) {   // la croix : les branches utiles en sombre
        const float bras = r * 0.26f, lg = r * 0.68f;
        const bool v = in.g == kHautBas;
        c.d->AddRectFilled(m - ImVec2(bras, lg), m + ImVec2(bras, lg), v ? kEncre : kEteint, bras * 0.5f);
        c.d->AddRectFilled(m - ImVec2(lg, bras), m + ImVec2(lg, bras), v ? kEteint : kEncre, bras * 0.5f);
        c.d->AddRectFilled(m - ImVec2(bras, bras), m + ImVec2(bras, bras), kEncre);
    } else {   // les boutons de façade, quatre points : celui qui valide ou qui revient est plein
        const int plein = in.g == kValider ? c.point_valider : c.point_retour;
        const ImVec2 en[4] = {m + ImVec2(0, -r * 0.5f), m + ImVec2(r * 0.5f, 0), m + ImVec2(-r * 0.5f, 0), m + ImVec2(0, r * 0.5f)};
        for (int i = 0; i < 4; i++) {
            if (i == plein) c.d->AddCircleFilled(en[i], r * 0.27f, kEncre);
            else c.d->AddCircle(en[i], r * 0.22f, kEteint, 0, std::max(1.0f, s));
        }
    }
    return 2 * r;
}
/// Les invites, le mot avant son bouton ; rend leur largeur (dessine faux : mesure seulement)
inline float invites(Ctx& c, ImVec2 p, const Invite* liste, int n, float px, bool dessine) {
    const float s = c.s, hh = px + 10 * s, x0 = p.x;
    for (int i = 0; i < n; i++) {
        if (dessine) texte_milieu(c, c.f.texte, px, p.x, p.y + hh * 0.5f, kTexte, liste[i].quoi);
        p.x += taille_de(c.f.texte, px, liste[i].quoi).x + 7 * s;
        p.x += signe(c, px, p, liste[i], dessine) + 22 * s;
    }
    return n ? p.x - x0 - 22 * s : 0;
}

}  // namespace ui
}  // namespace menu
}  // namespace hos
