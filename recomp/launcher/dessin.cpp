// Dessin du launcher : Direct2D 1.1 sur une chaîne d'échange DXGI, textes DirectWrite (Bahnschrift).
// Direction artistique : noir et vert néon, extraits des cinématiques en fond (étalonnés en vert), grille en
// perspective, globe holographique quand il n'y a pas encore d'extraits, bandeau défilant.
#include "lanceur.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dwrite_1.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace lz {
namespace {

// effets Direct2D (d2d1effects.h) : GUID repris ici pour ne pas dépendre de libdxguid
const GUID CLSID_MATRICE = {0x921f03d6, 0x641c, 0x47df, {0x85, 0x2d, 0xb4, 0xbb, 0x61, 0x53, 0xae, 0x11}};
const GUID CLSID_FLOU = {0x1feb6d69, 0x2fe6, 0x4ac9, {0x8c, 0x58, 0x1d, 0x7f, 0x93, 0xe7, 0xa6, 0xa5}};

template <class T> void rel(T*& p) { if (p) p->Release(), p = nullptr; }

ID3D11Device* g_d3d = nullptr;
ID3D11DeviceContext* g_d3dctx = nullptr;
IDXGISwapChain1* g_chaine = nullptr;
ID2D1Factory1* g_fab = nullptr;
ID2D1Device* g_dev = nullptr;
ID2D1DeviceContext* g_dc = nullptr;
ID2D1Bitmap1* g_cible = nullptr;
IDWriteFactory1* g_dw = nullptr;
ID2D1SolidColorBrush* g_br = nullptr;
ID2D1Effect* g_fx_couleur = nullptr;
ID2D1Effect* g_fx_flou = nullptr;
ID2D1Bitmap* g_bm_video = nullptr;
uint32_t g_bvw = 0, g_bvh = 0;
uint64_t g_vnum = 0;
double g_vt = 0, g_vd = 0;
bool g_video_ok = false;
std::vector<uint8_t> g_vpx;
ID2D1Bitmap1* g_bm_titre = nullptr;   // titre rendu une fois pour sa lueur
float g_titre_dpi = 0;
ID2D1BitmapBrush* g_trame = nullptr;
UINT g_sync = 1;
HWND g_hwnd = nullptr;

struct ZoneR { D2D1_RECT_F r; int id; };
std::vector<ZoneR> g_zones, g_zones_prec;

enum Fmt { F_TITRE, F_SOUS, F_VERSION, F_BOUTON, F_MENU, F_PETIT, F_CORPS, F_BANDEAU, F_PANNEAU, F_VALEUR, F_MONO,
           F_ETAPE, F_GRAS, F_NB };
IDWriteTextFormat* g_fmt[F_NB] = {};
IDWriteInlineObject* g_ellipse[F_NB] = {};
std::unordered_map<std::wstring, IDWriteTextLayout*> g_cache;

// ---- couleurs ----
D2D1_COLOR_F C(uint32_t rgb, float a = 1.f) {
    return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a);
}
constexpr uint32_t VERT = 0x3CFF7A, VERT_FONCE = 0x0FA14A, VERT_PALE = 0x9FE8B8, TEXTE = 0xD8F7E3, BLANC = 0xEFFFF4,
                   GRIS = 0x6F9A80, AMBRE = 0xF2C14E, NOIR = 0x030604;

ID2D1SolidColorBrush* pinceau(uint32_t rgb, float a = 1.f) {
    g_br->SetColor(C(rgb, a));
    return g_br;
}

// ---- textes ----
// Toutes les écritures sont réduites de 10 % (demande de l'utilisateur après l'agrandissement de la fenêtre,
// 2026-11-11) : la mise en page ne change pas, le texte respire un peu plus.
constexpr float TAILLE_TEXTE = 0.9f;
bool format(Fmt f, const wchar_t* famille, float taille, DWRITE_FONT_WEIGHT poids, DWRITE_FONT_STRETCH etirement) {
    taille *= TAILLE_TEXTE;
    HRESULT hr = g_dw->CreateTextFormat(famille, nullptr, poids, DWRITE_FONT_STYLE_NORMAL, etirement, taille, L"fr-fr", &g_fmt[f]);
    if (FAILED(hr)) hr = g_dw->CreateTextFormat(L"Segoe UI", nullptr, poids, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                 taille, L"fr-fr", &g_fmt[f]);
    if (FAILED(hr)) return false;
    g_fmt[f]->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    g_dw->CreateEllipsisTrimmingSign(g_fmt[f], &g_ellipse[f]);
    DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    g_fmt[f]->SetTrimming(&t, g_ellipse[f]);
    return true;
}

void vider_cache() {
    for (auto& [k, v] : g_cache) v->Release();
    g_cache.clear();
}

// mise en page mise en cache : texte, format, largeur max., interlettrage, retour à la ligne
IDWriteTextLayout* mise(const std::wstring& s, Fmt f, float largeur = 2000.f, float espace = 0.f, bool lignes = false) {
    wchar_t cle[64];
    swprintf(cle, 64, L"%d|%.1f|%.1f|%d|", (int)f, largeur, espace, (int)lignes);
    std::wstring k = cle + s;
    auto it = g_cache.find(k);
    if (it != g_cache.end()) return it->second;
    if (g_cache.size() > 600) vider_cache();
    IDWriteTextLayout* l = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), g_fmt[f], largeur, 1000.f, &l))) return nullptr;
    if (lignes) l->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    if (espace != 0.f) {
        IDWriteTextLayout1* l1 = nullptr;
        if (SUCCEEDED(l->QueryInterface(__uuidof(IDWriteTextLayout1), (void**)&l1))) {
            l1->SetCharacterSpacing(0.f, espace, 0.f, DWRITE_TEXT_RANGE{0, (UINT32)s.size()});
            l1->Release();
        }
    }
    g_cache[k] = l;
    return l;
}

float largeur_texte(IDWriteTextLayout* l) {
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}
float hauteur_texte(IDWriteTextLayout* l) {
    DWRITE_TEXT_METRICS m{};
    if (l) l->GetMetrics(&m);
    return m.height;
}

// alignement : 0 gauche, 1 centre, 2 droite (par rapport à x)
float texte(const std::wstring& s, Fmt f, float x, float y, uint32_t rgb, float a = 1.f, float espace = 0.f, int align = 0,
            float largeur = 2000.f) {
    IDWriteTextLayout* l = mise(s, f, largeur, espace);
    if (!l) return 0;
    const float w = std::min(largeur, largeur_texte(l));
    if (align == 1) x -= w / 2;
    if (align == 2) x -= w;
    g_dc->DrawTextLayout(D2D1::Point2F(x, y), l, pinceau(rgb, a), D2D1_DRAW_TEXT_OPTIONS_NONE);
    return w;
}

float texte_lignes(const std::wstring& s, Fmt f, float x, float y, float largeur, uint32_t rgb, float a = 1.f) {
    IDWriteTextLayout* l = mise(s, f, largeur, 0.f, true);
    if (!l) return 0;
    g_dc->DrawTextLayout(D2D1::Point2F(x, y), l, pinceau(rgb, a), D2D1_DRAW_TEXT_OPTIONS_NONE);
    return hauteur_texte(l);
}

void zone(D2D1_RECT_F r, int id) { g_zones.push_back({r, id}); }
bool survol(int id) { return g.zone_survol == id; }
bool appui(int id) { return g.zone_appui == id && g.bouton_bas && g.zone_survol == id; }

void ligne(float x0, float y0, float x1, float y1, uint32_t rgb, float a, float ep = 1.f) {
    g_dc->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), pinceau(rgb, a), ep);
}
void rect(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
    g_dc->FillRectangle(D2D1::RectF(x0, y0, x1, y1), pinceau(rgb, a));
}
void cadre(float x0, float y0, float x1, float y1, uint32_t rgb, float a, float ep = 1.f) {
    g_dc->DrawRectangle(D2D1::RectF(x0 + ep / 2, y0 + ep / 2, x1 - ep / 2, y1 - ep / 2), pinceau(rgb, a), ep);
}

ID2D1LinearGradientBrush* degrade(float x0, float y0, float x1, float y1, std::initializer_list<D2D1_GRADIENT_STOP> arrets) {
    ID2D1GradientStopCollection* col = nullptr;
    std::vector<D2D1_GRADIENT_STOP> v(arrets);
    g_dc->CreateGradientStopCollection(v.data(), (UINT32)v.size(), &col);
    ID2D1LinearGradientBrush* b = nullptr;
    g_dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1)), col, &b);
    rel(col);
    return b;
}

// coins en équerre (cadres des panneaux, comme un viseur)
void equerres(float x0, float y0, float x1, float y1, float l, uint32_t rgb, float a, float ep = 2.f) {
    auto* b = pinceau(rgb, a);
    const float e = ep / 2;
    g_dc->DrawLine(D2D1::Point2F(x0 + e, y0 + e), D2D1::Point2F(x0 + l, y0 + e), b, ep);
    g_dc->DrawLine(D2D1::Point2F(x0 + e, y0 + e), D2D1::Point2F(x0 + e, y0 + l), b, ep);
    g_dc->DrawLine(D2D1::Point2F(x1 - e, y0 + e), D2D1::Point2F(x1 - l, y0 + e), b, ep);
    g_dc->DrawLine(D2D1::Point2F(x1 - e, y0 + e), D2D1::Point2F(x1 - e, y0 + l), b, ep);
    g_dc->DrawLine(D2D1::Point2F(x0 + e, y1 - e), D2D1::Point2F(x0 + l, y1 - e), b, ep);
    g_dc->DrawLine(D2D1::Point2F(x0 + e, y1 - e), D2D1::Point2F(x0 + e, y1 - l), b, ep);
    g_dc->DrawLine(D2D1::Point2F(x1 - e, y1 - e), D2D1::Point2F(x1 - l, y1 - e), b, ep);
    g_dc->DrawLine(D2D1::Point2F(x1 - e, y1 - e), D2D1::Point2F(x1 - e, y1 - l), b, ep);
}

ID2D1PathGeometry* parallelogramme(float x, float y, float w, float h, float biais) {
    ID2D1PathGeometry* g = nullptr;
    g_fab->CreatePathGeometry(&g);
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    s->BeginFigure(D2D1::Point2F(x + biais, y), D2D1_FIGURE_BEGIN_FILLED);
    D2D1_POINT_2F p[3] = {D2D1::Point2F(x + w, y), D2D1::Point2F(x + w - biais, y + h), D2D1::Point2F(x, y + h)};
    s->AddLines(p, 3);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    rel(s);
    return g;
}

void triangle(float x, float y, float t, uint32_t rgb, float a) {
    ID2D1PathGeometry* g = nullptr;
    g_fab->CreatePathGeometry(&g);
    ID2D1GeometrySink* s = nullptr;
    g->Open(&s);
    s->BeginFigure(D2D1::Point2F(x, y - t * 0.6f), D2D1_FIGURE_BEGIN_FILLED);
    D2D1_POINT_2F p[2] = {D2D1::Point2F(x + t, y), D2D1::Point2F(x, y + t * 0.6f)};
    s->AddLines(p, 2);
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    rel(s);
    g_dc->FillGeometry(g, pinceau(rgb, a));
    rel(g);
}

// ---- appareil ----
bool cible_creer() {
    IDXGISurface* surf = nullptr;
    if (FAILED(g_chaine->GetBuffer(0, IID_PPV_ARGS(&surf)))) return false;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), g.dpi, g.dpi);
    HRESULT hr = g_dc->CreateBitmapFromDxgiSurface(surf, &bp, &g_cible);
    rel(surf);
    if (FAILED(hr)) return false;
    g_dc->SetTarget(g_cible);
    g_dc->SetDpi(g.dpi, g.dpi);
    return true;
}

void trame_creer() {
    rel(g_trame);
    // lignes de balayage : une ligne sombre toutes les 3 lignes de pixels de l'écran
    const uint32_t px[3] = {0x55000000u, 0, 0};
    ID2D1Bitmap* bm = nullptr;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                       g.dpi, g.dpi);
    if (FAILED(g_dc->CreateBitmap(D2D1::SizeU(1, 3), px, 4, &bp, &bm))) return;
    D2D1_BITMAP_BRUSH_PROPERTIES bbp = D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                    D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    g_dc->CreateBitmapBrush(bm, &bbp, nullptr, &g_trame);
    rel(bm);
}

void matrice_couleur() {
    // étalonnage « hologramme » : mélange de l'image et d'une bichromie noir → vert, légèrement assombri
    const float lum[3] = {0.2126f, 0.7152f, 0.0722f};
    const float teinte[3] = {0.30f, 1.00f, 0.58f};
    const float m = 0.42f, k = 0.80f;
    D2D1_MATRIX_5X4_F mat{};
    float* c = &mat._11;   // ligne i = canal d'entrée (r, g, b, a, 1), colonne j = canal de sortie
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) c[i * 4 + j] = k * ((1 - m) * (i == j) + m * teinte[j] * lum[i]);
    c[3 * 4 + 3] = 1.f;
    g_fx_couleur->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, D2D1_PROPERTY_TYPE_MATRIX_5X4, (const BYTE*)&mat, sizeof mat);
}

void titre_lueur() {
    if (g_bm_titre && g_titre_dpi == g.dpi) return;
    rel(g_bm_titre);
    g_titre_dpi = g.dpi;
    const float W = 560, H = 120;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), g.dpi, g.dpi);
    if (FAILED(g_dc->CreateBitmap(D2D1::SizeU((UINT32)(W * g.dpi / 96), (UINT32)(H * g.dpi / 96)), nullptr, 0, &bp, &g_bm_titre)))
        return;
    ID2D1Image* avant = nullptr;
    g_dc->GetTarget(&avant);
    g_dc->SetTarget(g_bm_titre);
    g_dc->BeginDraw();
    g_dc->SetTransform(D2D1::IdentityMatrix());
    g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    texte(L"STARFOX", F_TITRE, 20, 14, VERT, 1.f, 3.f);
    g_dc->EndDraw();
    g_dc->SetTarget(avant);
    rel(avant);
    g_fx_flou->SetInput(0, g_bm_titre, TRUE);
}

// ---- éléments ----
struct Particule { float x, y, v, t, ph; };
std::vector<Particule> g_part;

void particules(float t) {
    if (g_part.empty()) {
        uint32_t s = 12345;
        auto r = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.f; };
        for (int i = 0; i < 70; i++) g_part.push_back({r() * LARGEUR, r() * HAUTEUR, 4 + r() * 14, 0.8f + r() * 1.6f, r() * 6.28f});
    }
    for (auto& p : g_part) {
        const float y = fmodf(p.y - t * p.v + 10 * HAUTEUR, HAUTEUR);
        const float x = p.x + sinf(t * 0.3f + p.ph) * 6;
        const float a = 0.15f + 0.35f * (0.5f + 0.5f * sinf(t * 1.7f + p.ph));
        rect(x, y, x + p.t, y + p.t, VERT, a);
    }
}

void grille(float t) {
    const float horizon = 452, bas = HAUTEUR, vx = 500, vy = 300;
    ID2D1LinearGradientBrush* b = degrade(0, horizon, 0, bas, {{0.f, C(VERT, 0.f)}, {0.35f, C(VERT, 0.10f)}, {1.f, C(VERT, 0.30f)}});
    if (!b) return;
    for (int k = -14; k <= 14; k++) {
        const float xb = vx + k * 95.f;
        const float xh = vx + (xb - vx) * (horizon - vy) / (bas - vy);
        g_dc->DrawLine(D2D1::Point2F(xh, horizon), D2D1::Point2F(xb, bas), b, 1.f);
    }
    const int N = 9;
    const float ph = fmodf(t * 0.22f, 1.f);
    for (int i = 0; i < N; i++) {
        const float z = (i + ph) / N;
        const float y = horizon + (bas - horizon) * powf(z, 2.1f);
        g_dc->DrawLine(D2D1::Point2F(0, y), D2D1::Point2F(LARGEUR, y), b, 1.f);
    }
    rel(b);
}

// globe holographique (fond sans extraits) : méridiens qui tournent, parallèles, anneau, balayage
void globe(float t) {
    const float cx = 720, cy = 262, r = 150;
    ID2D1GradientStopCollection* col = nullptr;
    D2D1_GRADIENT_STOP st[3] = {{0.f, C(VERT, 0.22f)}, {0.55f, C(VERT_FONCE, 0.08f)}, {1.f, C(VERT, 0.f)}};
    g_dc->CreateGradientStopCollection(st, 3, &col);
    ID2D1RadialGradientBrush* rb = nullptr;
    g_dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(cx, cy), D2D1::Point2F(0, 0), r * 2.1f, r * 2.1f),
                                    col, &rb);
    if (rb) g_dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 2.1f, r * 2.1f), rb);
    rel(rb), rel(col);
    g_dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), pinceau(0x02140A, 0.85f));
    for (int k = 0; k < 12; k++) {   // méridiens
        const float phi = t * 0.25f + k * 3.14159f / 12;
        const float rx = r * fabsf(sinf(phi));
        const bool devant = cosf(phi) > 0;
        g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rx, r), pinceau(VERT, devant ? 0.42f : 0.12f), devant ? 1.2f : 1.f);
    }
    for (int k = -3; k <= 3; k++) {   // parallèles
        const float lat = k * 0.38f;
        const float y = cy - r * sinf(lat), rx = r * cosf(lat);
        g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y), rx, rx * 0.16f), pinceau(VERT, 0.28f), 1.f);
    }
    // balayage horizontal qui descend sur la sphère
    const float yb = cy - r + fmodf(t * 60.f, 2 * r);
    const float dx = sqrtf(std::max(0.f, r * r - (yb - cy) * (yb - cy)));
    ligne(cx - dx, yb, cx + dx, yb, VERT, 0.75f, 2.f);
    g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), pinceau(VERT, 0.85f), 1.6f);
    // anneau incliné
    D2D1_MATRIX_3X2_F avant;
    g_dc->GetTransform(&avant);
    g_dc->SetTransform(D2D1::Matrix3x2F::Rotation(-14.f, D2D1::Point2F(cx, cy)) * avant);
    g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 1.75f, r * 0.34f), pinceau(VERT, 0.55f), 1.4f);
    g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r * 1.62f, r * 0.30f), pinceau(VERT, 0.22f), 1.f);
    g_dc->SetTransform(avant);
}

void fond_video(float t) {
    std::vector<uint8_t> px;
    uint32_t w = 0, h = 0;
    double vt = 0, vd = 0;
    if (video_image(g_vnum, px, w, h, vt, vd)) {
        if (px.empty()) {
            g_video_ok = false;
        } else {
            if (!g_bm_video || w != g_bvw || h != g_bvh) {
                rel(g_bm_video);
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(
                    D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.f, 96.f);
                if (SUCCEEDED(g_dc->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, &bp, &g_bm_video))) g_bvw = w, g_bvh = h;
                g_fx_couleur->SetInput(0, g_bm_video, TRUE);
            } else {
                g_bm_video->CopyFromMemory(nullptr, px.data(), w * 4);
            }
            g_vt = vt, g_vd = vd;
            g_video_ok = g_bm_video != nullptr;
        }
    }
    if (!g_video_ok) return;
    // plein cadre, léger zoom lent (effet « Ken Burns »), fondus au noir entre deux extraits
    const float avance = g_vd > 0 ? (float)std::clamp(g_vt / g_vd, 0.0, 1.0) : 0.f;
    const float ech = std::max(LARGEUR / g_bvw, HAUTEUR / g_bvh) * (1.03f + 0.06f * avance);
    const float dx = (LARGEUR - g_bvw * ech) / 2 + 180 * ech * 0.1f, dy = (HAUTEUR - g_bvh * ech) / 2;
    D2D1_MATRIX_3X2_F avant;
    g_dc->GetTransform(&avant);
    g_dc->SetTransform(D2D1::Matrix3x2F::Scale(ech, ech) * D2D1::Matrix3x2F::Translation(dx, dy) * avant);
    ID2D1Image* img = nullptr;
    g_fx_couleur->GetOutput(&img);
    g_dc->DrawImage(img, nullptr, nullptr, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    rel(img);
    g_dc->SetTransform(avant);
    float fondu = 1.f;
    if (g_vd > 0) fondu = std::min({1.f, (float)g_vt / 0.7f, (float)(g_vd - g_vt) / 0.7f});
    if (fondu < 1.f) rect(0, 0, LARGEUR, HAUTEUR, NOIR, 1.f - std::max(0.f, fondu));
    (void)t;
}

void voiles() {
    // gauche sombre (lisibilité du menu), bas sombre (grille et bandeau), haut léger, vignette
    ID2D1LinearGradientBrush* b = degrade(0, 0, 720, 0, {{0.f, C(NOIR, 0.94f)}, {0.42f, C(NOIR, 0.70f)}, {1.f, C(NOIR, 0.f)}});
    if (b) g_dc->FillRectangle(D2D1::RectF(0, 0, 720, HAUTEUR), b), rel(b);
    b = degrade(0, 380, 0, HAUTEUR, {{0.f, C(NOIR, 0.f)}, {1.f, C(NOIR, 0.92f)}});
    if (b) g_dc->FillRectangle(D2D1::RectF(0, 380, LARGEUR, HAUTEUR), b), rel(b);
    b = degrade(0, 0, 0, 120, {{0.f, C(NOIR, 0.65f)}, {1.f, C(NOIR, 0.f)}});
    if (b) g_dc->FillRectangle(D2D1::RectF(0, 0, LARGEUR, 120), b), rel(b);
    ID2D1GradientStopCollection* col = nullptr;
    D2D1_GRADIENT_STOP st[2] = {{0.55f, C(NOIR, 0.f)}, {1.f, C(NOIR, 0.55f)}};
    g_dc->CreateGradientStopCollection(st, 2, &col);
    ID2D1RadialGradientBrush* rb = nullptr;
    g_dc->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(D2D1::Point2F(560, 300), D2D1::Point2F(0, 0), 700, 520), col, &rb);
    if (rb) g_dc->FillRectangle(D2D1::RectF(0, 0, LARGEUR, HAUTEUR), rb);
    rel(rb), rel(col);
    if (g_trame) g_dc->FillRectangle(D2D1::RectF(0, 0, LARGEUR, HAUTEUR), g_trame);
}

void titre(float t) {
    titre_lueur();
    if (g_bm_titre) {   // lueur qui respire
        const float a = 0.55f + 0.25f * sinf(t * 1.3f);
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                           D2D1::IdentityMatrix(), a, nullptr, D2D1_LAYER_OPTIONS1_NONE);
        g_dc->PushLayer(&lp, nullptr);
        ID2D1Image* img = nullptr;
        g_fx_flou->GetOutput(&img);
        D2D1_POINT_2F o = D2D1::Point2F(24, 20);
        g_dc->DrawImage(img, &o, nullptr, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
        g_dc->DrawImage(img, &o, nullptr, D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
        rel(img);
        g_dc->PopLayer();
    }
    // titre net : dégradé vertical blanc-vert
    IDWriteTextLayout* l = mise(L"STARFOX", F_TITRE, 2000.f, 3.f);
    ID2D1LinearGradientBrush* b = degrade(0, 44, 0, 104, {{0.f, C(0xF2FFF6)}, {0.55f, C(0x8CFFB5)}, {1.f, C(VERT)}});
    if (l && b) g_dc->DrawTextLayout(D2D1::Point2F(44, 34), l, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
    rel(b);
    texte(T("sous_titre"), F_SOUS, 48, 108, VERT, 1.f, 6.f);
    wchar_t v[96];
    swprintf(v, 96, T("ligne_version"), g.version.c_str());
    texte(v, F_VERSION, 49, 140, GRIS, 1.f, 2.f);
}

double progres_total(Construction& c) {
    static const struct { const char* id; double poids; } P[] = {
        {"verifier", 1}, {"extraire", 2}, {"images", 2}, {"recompiler", 78}, {"lier", 3}, {"shaders", 9}, {"pipelines", 3},
        {"dep_verifier", 1}, {"dep_copier", 30}};   // déplacement de l'installation (deplacer.py)
    auto poids = [&](const std::string& id) {
        for (auto& p : P)
            if (id == p.id) return p.poids;
        return 1.0;
    };
    double tot = 0, fait = 0;
    for (size_t i = 0; i < c.etapes.size(); i++) {
        const double w = poids(c.etapes[i]);
        tot += w;
        if (c.statut[i] >= 2) fait += w;
        else if (c.statut[i] == 1) fait += w * std::clamp(c.progres, 0.0, 1.0);
    }
    return tot > 0 ? fait / tot : 0;
}

void bouton_principal(float t) {
    const float x = 40, y = 194, w = 332, h = 52, biais = 17;
    zone(D2D1::RectF(x, y, x + w, y + h), Z_PRINCIPAL);
    ID2D1PathGeometry* geo = parallelogramme(x, y, w, h, biais);
    bool active, finie;
    double p = 0;
    Tache tache;
    {
        std::lock_guard<std::mutex> l(g.cons.lk);
        active = g.cons.active, finie = g.cons.finie, tache = g.cons.tache;
        if (active) p = tache == Tache::Construction ? progres_total(g.cons) : std::clamp(g.cons.progres, 0.0, 1.0);
    }
    (void)finie;
    const bool sv = survol(Z_PRINCIPAL), ap = appui(Z_PRINCIPAL);
    if (active) {
        g_dc->FillGeometry(geo, pinceau(0x07160D, 0.92f));
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), geo);
        g_dc->PushLayer(&lp, nullptr);
        ID2D1LinearGradientBrush* b = degrade(x, 0, x + w, 0, {{0.f, C(0x1DB954)}, {1.f, C(VERT)}});
        if (b) g_dc->FillRectangle(D2D1::RectF(x, y, x + (float)(w * p), y + h), b), rel(b);
        // reflet qui parcourt la barre
        const float xr = x + fmodf(t * 160.f, w + 120) - 60;
        b = degrade(xr - 50, 0, xr + 50, 0, {{0.f, C(0xFFFFFF, 0.f)}, {0.5f, C(0xFFFFFF, 0.16f)}, {1.f, C(0xFFFFFF, 0.f)}});
        if (b) g_dc->FillRectangle(D2D1::RectF(xr - 50, y, xr + 50, y + h), b), rel(b);
        g_dc->PopLayer();
        g_dc->DrawGeometry(geo, pinceau(VERT, sv ? 0.95f : 0.7f), 1.4f);
        wchar_t s[96];
        swprintf(s, 96, L"%ls   %d %%", T(tache == Tache::Construction ? "en_cours" : tache == Tache::Shaders ? "shaders_en_cours" :
                   tache == Tache::Deplacement ? "dep_en_cours" : "maj_en_cours"),
                 (int)(p * 100));
        texte(s, F_BOUTON, x + w / 2 + 4, y + 13, BLANC, 1.f, 2.5f, 1);
    } else {
        const bool en_jeu = g.jeu_lance;
        if (sv && !en_jeu) {   // lueur au survol
            g_dc->DrawGeometry(geo, pinceau(VERT, 0.10f), 14.f);
            g_dc->DrawGeometry(geo, pinceau(VERT, 0.22f), 7.f);
        }
        uint32_t c0 = ap ? 0x18B055 : sv ? 0x6BFFA2 : 0x34F078, c1 = ap ? 0x0B7A36 : sv ? 0x22CC63 : 0x13A64E;
        if (en_jeu) c0 = 0x2A5A3B, c1 = 0x1A3A26;
        ID2D1LinearGradientBrush* b = degrade(x, y, x + w, y + h, {{0.f, C(c0)}, {1.f, C(c1)}});
        if (b) g_dc->FillGeometry(geo, b), rel(b);
        // fines rayures diagonales en surimpression
        D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(D2D1::InfiniteRect(), geo);
        g_dc->PushLayer(&lp, nullptr);
        for (float k = -h; k < w; k += 9) ligne(x + k, y + h, x + k + h, y, 0xFFFFFF, 0.05f, 2.f);
        g_dc->PopLayer();
        const std::wstring s = en_jeu ? T("en_jeu") : g.jeu_pret ? T("jouer") : T("recompiler");   // jeu à refaire : jouable quand même
        IDWriteTextLayout* l = mise(s, F_BOUTON, 2000.f, 3.f);
        const float tw = largeur_texte(l);
        const float tx = x + 28 + (w - 56 - tw - 26) / 2;
        triangle(tx, y + h / 2, 13, 0x04140A, 0.9f);
        g_dc->DrawTextLayout(D2D1::Point2F(tx + 26, y + 12), l, pinceau(0x04140A, 0.92f), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
    rel(geo);
}

void menu() {
    struct { int id; const char* cle; float y; Page p; } items[] = {
        {Z_MENU_GRAPHISMES, "graphismes", 262, Page::Graphismes},
        {Z_MENU_REGLAGES, "reglages", 294, Page::Reglages},
        {Z_MENU_CONTROLES, "controles", 326, Page::Controles},
        {Z_MENU_CONSTRUCTION, "construction", 358, Page::Construction},
        {Z_MENU_RAPPORT, "rapport", 390, Page::Aucune},
        {Z_MENU_QUITTER, "quitter", 422, Page::Aucune},
    };
    for (auto& it : items) {
        const bool actif = it.id != Z_MENU_QUITTER && it.id != Z_MENU_RAPPORT && g.page == it.p;
        const bool sv = survol(it.id);
        IDWriteTextLayout* l = mise(T(it.cle), F_MENU, 2000.f, 3.5f);
        const float w = largeur_texte(l);
        zone(D2D1::RectF(44, it.y - 4, 60 + w + 20, it.y + 26), it.id);
        if (actif || sv) {
            rect(44, it.y + 2, 47, it.y + 21, VERT, actif ? 1.f : 0.8f);
            for (float x = 66 + w + 12; x < 330; x += 9) rect(x, it.y + 11, x + 2, it.y + 13, VERT, 0.45f);
        }
        g_dc->DrawTextLayout(D2D1::Point2F(60, it.y), l, pinceau(actif || sv ? VERT : TEXTE, 1.f), D2D1_DRAW_TEXT_OPTIONS_NONE);
    }
}

std::wstring nom_fichier(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? p : p.substr(k + 1);
}

void bloc_copie() {
    const float y = 452;
    texte(T("copie"), F_PETIT, 44, y + 2, GRIS, 1.f, 2.5f);
    float x = 124;
    if (!g.copie.empty()) x += texte(nom_fichier(g.copie), F_CORPS, x, y, TEXTE, 1.f, 0.f, 0, 240) + 12;
    const std::wstring lien = g.copie.empty() ? T("choisir") : T("changer");
    const float lw = texte(lien, F_CORPS, x, y, survol(Z_COPIE) ? BLANC : VERT, 1.f, 0.5f);
    zone(D2D1::RectF(x - 4, y - 2, x + lw + 4, y + 20), Z_COPIE);
    if (g.copie_ok) texte(T("copie_ok"), F_CORPS, 44, y + 24, VERT_PALE, 1.f, 0.f, 0, 400);
    else texte(g.copie_message.empty() ? T("copie_absente") : g.copie_message, F_CORPS, 44, y + 24, AMBRE, 1.f, 0.f, 0, 400);
    bool shaders;
    std::wstring info;
    {
        std::lock_guard<std::mutex> l(g.cons.lk);
        shaders = g.cons.active && g.cons.tache != Tache::Construction;
        if (shaders)
            info = g.cons.tache == Tache::Shaders ? T("e_pipelines") : g.cons.tache == Tache::Deplacement ? T("dep_en_cours") : T("maj_en_cours");
        if (shaders && !g.cons.info.empty()) info += L"  ·  " + g.cons.info;
    }
    texte(!g.jeu_pret ? T("jeu_absent") : g.jeu_obsolete ? T("jeu_obsolete") : T("jeu_pret"), F_CORPS, 44, y + 46, BLANC, 1.f);
    if (shaders) texte(info, F_CORPS, 44, y + 66, VERT_PALE, 1.f, 0.f, 0, 400);
    // message d'état : sur deux lignes au besoin (l'avertissement de mémoire était coupé à « Fermez d'a… » sur une ligne
    // de 400 px : le joueur ne voyait ni le conseil ni « recliquez pour lancer quand même »)
    else if (!g.statut_jeu.empty()) texte_lignes(g.statut_jeu, F_CORPS, 44, y + 66, 640, AMBRE);
    else if (g.jeu_pret && g.jeu_obsolete) {
        // Reconstruction proposée, jamais imposée : le bouton principal lance le jeu actuel ; ce lien refait les étapes
        // qui ont changé.
        const bool libre = !g.cons.active && !g.jeu_lance;
        const bool sv = libre && survol(Z_MAJ_JEU);
        const float lw = texte(T("jeu_obsolete_aide"), F_CORPS, 44, y + 66, sv ? BLANC : libre ? VERT : GRIS, 1.f, 0.5f);
        if (libre) zone(D2D1::RectF(40, y + 62, 44 + lw + 6, y + 86), Z_MAJ_JEU);
        if (sv) ligne(44, y + 85, 44 + lw, y + 85, BLANC, 0.8f, 1.f);
        texte(T("jeu_obsolete_detail"), F_CORPS, 44 + lw + 14, y + 66, GRIS, 1.f, 0.f, 0, 330);
    } else texte(!g.jeu_pret ? T("jeu_absent_aide") : T("jeu_pret_aide"), F_CORPS, 44, y + 66, GRIS, 1.f, 0.f, 0, 400);
}

void bandeau(float t) {
    const float y0 = 566, y1 = 594;
    rect(0, y0, LARGEUR, y1, NOIR, 0.55f);
    ligne(0, y0, LARGEUR, y0, VERT, 0.35f);
    IDWriteTextLayout* l = mise(T("bandeau"), F_BANDEAU, 20000.f, 1.2f);
    const float w = largeur_texte(l);
    if (!l || w < 10) return;
    g_dc->PushAxisAlignedClip(D2D1::RectF(0, y0, LARGEUR, y1), D2D1_ANTIALIAS_MODE_ALIASED);
    float x = -fmodf(t * 42.f, w);
    for (; x < LARGEUR; x += w) g_dc->DrawTextLayout(D2D1::Point2F(x, y0 + 4), l, pinceau(VERT_PALE, 0.9f), D2D1_DRAW_TEXT_OPTIONS_NONE);
    g_dc->PopAxisAlignedClip();
}

void barre_haut() {
    // mise à jour disponible (releases GitHub)
    std::wstring maj;
    {
        std::lock_guard<std::mutex> l(g.maj_lk);
        maj = g.maj;
    }
    if (!maj.empty() && !g.cons.active) {
        wchar_t s[96];
        swprintf(s, 96, T("maj_dispo"), maj.c_str());
        const bool sv = survol(Z_MAJ);
        const float w = texte(s, F_PETIT, 820, 14, sv ? BLANC : AMBRE, 1.f, 1.5f, 2);
        zone(D2D1::RectF(820 - w - 6, 8, 826, 32), Z_MAJ);
        if (sv) ligne(820 - w, 31, 820, 31, BLANC, 0.8f, 1.f);
    }
    // langues
    const float y = 14;
    const bool fr = g.langue == Langue::Fr;
    float x = 846;
    float w = texte(L"FR", F_PETIT, x, y, fr ? VERT : survol(Z_FR) ? BLANC : GRIS, 1.f, 1.5f);
    if (fr) ligne(x, y + 17, x + w - 1.5f, y + 17, VERT, 1.f, 1.f);
    zone(D2D1::RectF(x - 6, y - 4, x + w + 4, y + 20), Z_FR);
    texte(L"·", F_PETIT, x + w + 6, y, GRIS);
    x += w + 18;
    w = texte(L"EN", F_PETIT, x, y, !fr ? VERT : survol(Z_EN) ? BLANC : GRIS, 1.f, 1.5f);
    if (!fr) ligne(x, y + 17, x + w - 1.5f, y + 17, VERT, 1.f, 1.f);
    zone(D2D1::RectF(x - 4, y - 4, x + w + 6, y + 20), Z_EN);
    // réduire, fermer
    const float bx = 922, by = 6, bw = 32, bh = 28;
    if (survol(Z_REDUIRE)) rect(bx, by, bx + bw, by + bh, VERT, 0.16f);
    ligne(bx + 11, by + 15, bx + 21, by + 15, survol(Z_REDUIRE) ? BLANC : TEXTE, 1.f, 1.2f);
    zone(D2D1::RectF(bx, by, bx + bw, by + bh), Z_REDUIRE);
    const float fx = bx + bw + 2;
    if (survol(Z_FERMER)) rect(fx, by, fx + bw, by + bh, 0xE0404A, 0.85f);
    ligne(fx + 11, by + 9, fx + 21, by + 19, survol(Z_FERMER) ? BLANC : TEXTE, 1.f, 1.3f);
    ligne(fx + 21, by + 9, fx + 11, by + 19, survol(Z_FERMER) ? BLANC : TEXTE, 1.f, 1.3f);
    zone(D2D1::RectF(fx, by, fx + bw, by + bh), Z_FERMER);
}

// ---- panneaux ----
const float PX0 = 432, PY0 = 54, PX1 = 970, PY1 = 546;

void panneau(const wchar_t* titre_p) {
    rect(PX0, PY0, PX1, PY1, 0x010402, 0.86f);
    cadre(PX0, PY0, PX1, PY1, VERT, 0.16f);
    equerres(PX0 - 1, PY0 - 1, PX1 + 1, PY1 + 1, 18, VERT, 0.9f);
    texte(titre_p, F_PANNEAU, PX0 + 24, PY0 + 18, VERT, 1.f, 4.f);
    ligne(PX0 + 24, PY0 + 44, PX1 - 24, PY0 + 44, VERT, 0.18f);
    // fermer le panneau
    const float x = PX1 - 40, y = PY0 + 14;
    zone(D2D1::RectF(x, y, x + 26, y + 26), Z_FERMER_PAGE);
    const uint32_t c = survol(Z_FERMER_PAGE) ? BLANC : GRIS;
    ligne(x + 8, y + 8, x + 18, y + 18, c, 1.f, 1.2f);
    ligne(x + 18, y + 8, x + 8, y + 18, c, 1.f, 1.2f);
}

// sélecteur ‹ valeur ›
void selecteur(float y, const wchar_t* valeur, int zg, int zd, bool actif = true) {
    const float x0 = 652, x1 = 940, h = 28;
    const bool sv = actif && (survol(zg) || survol(zd));
    rect(x0, y, x1, y + h, NOIR, 0.65f);
    cadre(x0, y, x1, y + h, VERT, !actif ? 0.14f : sv ? 0.75f : 0.32f);
    if (actif) {   // inactif : valeur fixée par un autre réglage (rendu 3D d'un mode DLSS), grisée, sans flèches
        texte(L"‹", F_VALEUR, x0 + 14, y + 2, survol(zg) ? BLANC : VERT, 1.f, 0.f, 1);
        texte(L"›", F_VALEUR, x1 - 14, y + 2, survol(zd) ? BLANC : VERT, 1.f, 0.f, 1);
        zone(D2D1::RectF(x0, y, (x0 + x1) / 2, y + h), zg);
        zone(D2D1::RectF((x0 + x1) / 2, y, x1, y + h), zd);
    }
    texte(valeur, F_VALEUR, (x0 + x1) / 2, y + 4, actif ? BLANC : GRIS, 1.f, 0.5f, 1, x1 - x0 - 70);
}

// libellé d'un réglage et sa ligne d'aide (ambre : mise en garde, une mise à l'échelle expérimentale par exemple)
void libelle_aide(float y, const char* cle, const std::wstring& aide, uint32_t couleur = GRIS) {
    texte(T(cle), F_CORPS, PX0 + 24, y + 1, TEXTE, 1.f, 0.3f);
    if (!aide.empty()) texte(aide, F_PETIT, PX0 + 24, y + 19, couleur, 1.f, 0.f, 0, 190);
}
void libelle(float y, const char* cle, const char* aide, uint32_t couleur = GRIS) {
    libelle_aide(y, cle, aide ? std::wstring(T(aide)) : std::wstring(), couleur);
}

void bouton_contour(float x0, float y0, float x1, float y1, const wchar_t* s, int id, bool actif = true) {
    const bool sv = actif && survol(id);
    rect(x0, y0, x1, y1, sv ? VERT : NOIR, sv ? 0.16f : 0.5f);
    cadre(x0, y0, x1, y1, VERT, actif ? (sv ? 0.95f : 0.55f) : 0.18f);
    texte(s, F_PETIT, (x0 + x1) / 2, y0 + (y1 - y0 - 15) / 2, actif ? (sv ? BLANC : VERT) : GRIS, 1.f, 2.f, 1);
    if (actif) zone(D2D1::RectF(x0, y0, x1, y1), id);
}

// dossier affiché dans une case étroite : « D:\…\SpaceAnimalsRecomp » quand le chemin est long
std::wstring chemin_court(const std::wstring& p) {
    if (p.size() <= 26) return p;
    const size_t k = p.find_last_of(L'\\');
    if (k == std::wstring::npos || k < 3) return p;
    return p.substr(0, 3) + L"…" + p.substr(k);
}

void page_reglages() {
    panneau(T("reg_titre"));
    float y = PY0 + 60;
    const float pas = 33;
    // copie
    libelle(y, "reg_copie", nullptr);
    rect(652, y, 846, y + 28, NOIR, 0.65f);
    cadre(652, y, 846, y + 28, VERT, 0.32f);
    texte(g.copie.empty() ? std::wstring(T("aucune")) : nom_fichier(g.copie), F_PETIT, 662, y + 6, g.copie.empty() ? GRIS : BLANC, 1.f,
          0.f, 0, 176);
    bouton_contour(852, y, 940, y + 28, T("parcourir"), Z_PARCOURIR);
    y += pas;
    wchar_t v[64];
    libelle(y, "reg_langue", "reg_langue_aide");
    selecteur(y, nom_langue_jeu(g.reglages.langue).c_str(), Z_LANGUE_G, Z_LANGUE_D);
    y += pas;
    // résolution, images par seconde, rendu 3D, DLSS et affichage : page Graphismes
    libelle(y, "reg_vol", nullptr);
    swprintf(v, 64, L"%d %%", g.volume);
    selecteur(y, v, Z_VOL_G, Z_VOL_D);
    y += pas;
    libelle(y, "reg_vib", "reg_vib_aide");
    if (g.reglages.vibration == 0) wcscpy(v, T("vib_aucune"));
    else swprintf(v, 64, L"%d %%", g.reglages.vibration);
    selecteur(y, v, Z_VIB_G, Z_VIB_D);
    y += pas;
    libelle(y, "reg_fond", "reg_fond_aide");
    selecteur(y, g.fond_video ? T("fond_extraits") : T("fond_sobre"), Z_FOND_G, Z_FOND_D);
    y += pas;
    libelle(y, "reg_maj", "reg_maj_aide");
    selecteur(y, g.maj_auto ? T("maj_auto") : T("maj_manuel"), Z_MAJ_AUTO_G, Z_MAJ_AUTO_D);
    y += pas;
    // Emplacement du jeu et raccourci sur le bureau (issue GitHub n° 14). Le dossier d'installation est déplacé par
    // deplacer.py (lanceur.cpp, deplacer) ; un dossier de développement ne l'est jamais.
    const bool libre = !g.cons.active && !g.jeu_lance;
    const bool deplacable = g.paquet && !g.sources;
    libelle(y, "reg_dossier", deplacable ? "reg_dossier_aide" : "reg_dossier_sources");
    rect(652, y, 846, y + 28, NOIR, 0.65f);
    cadre(652, y, 846, y + 28, VERT, 0.32f);
    texte(chemin_court(g.dossier_exe), F_PETIT, 662, y + 6, BLANC, 1.f, 0.f, 0, 176);
    bouton_contour(852, y, 940, y + 28, T("deplacer"), Z_DEPLACER, libre && deplacable);
    y += pas;
    const int rac = raccourci_etat();
    libelle(y, "reg_raccourci", rac == 1 ? "rac_present" : rac == 2 ? "rac_ailleurs" : "rac_absent");
    bouton_contour(652, y, 940, y + 28, T(rac == 1 ? "rac_retirer" : "rac_creer"), Z_RACCOURCI);
    y += pas + 8;
    texte(T("experts"), F_PETIT, PX0 + 24, y, VERT, 0.85f, 3.f);
    ligne(PX0 + 140, y + 8, PX1 - 24, y + 8, VERT, 0.15f);
    y += 20;
    libelle(y, "reg_fils", nullptr);
    if (g.fils == 0) swprintf(v, 64, T("auto_fils"), fils_effectifs());
    else swprintf(v, 64, L"%d", g.fils);
    selecteur(y, v, Z_FILS_G, Z_FILS_D);
    bouton_contour(PX0 + 24, PY1 - 46, PX0 + 262, PY1 - 18, T("reconstruire"), Z_RECONSTRUIRE, libre && g.copie_ok);
}

// Page Graphismes (2026-11-12, demande de l'utilisateur) : tout ce qui touche à l'image (recomp.ini du jeu). Le soir,
// autre demande : choisir la carte graphique (une carte AMD par exemple ; le DLSS n'est alors plus proposé), et dire
// que toutes les mises à l'échelle sont expérimentales (DLSS Qualité à Ultra perf., rendu 3D sous 100 %).
void page_graphismes() {
    panneau(T("gra_titre"));
    float y = PY0 + 58;
    const float pas = 33;   // neuf réglages depuis le FSR (2026-11-12)
    wchar_t v[64];
    {   // carte graphique : Automatique (la carte que le jeu prendra est nommée), puis chaque carte vue par Vulkan
        const bool pretes = g.cartes_pretes.load(std::memory_order_acquire);
        bool nommee = false;
        const int c = carte_du_jeu(&nommee);
        std::wstring val, aide;
        if (!pretes) val = T("gpu_auto"), aide = T("gpu_recherche");
        else if (c < 0) val = T("gpu_auto"), aide = T("gpu_aucune");
        else if (nommee) val = nom_carte(g.cartes[c].nom), aide = T("gpu_aide");
        else val = std::wstring(T("gpu_auto_carte")) + nom_carte(g.cartes[c].nom), aide = T("gpu_aide");
        libelle_aide(y, "reg_gpu", aide);
        selecteur(y, val.c_str(), Z_GPU_G, Z_GPU_D, c >= 0);
        y += pas;
    }
    libelle(y, "reg_res", "reg_res_aide");
    if (g.reglages.resolution == 0) wcscpy(v, T("auto_ecran"));
    else if (g.reglages.resolution == 2160) wcscpy(v, L"2160p (4K)");
    else swprintf(v, 64, L"%dp", g.reglages.resolution);
    selecteur(y, v, Z_RES_G, Z_RES_D);
    y += pas;
    libelle(y, "reg_hz", "reg_hz_aide");
    swprintf(v, 64, L"%d", g.reglages.hz);
    selecteur(y, v, Z_HZ_G, Z_HZ_D);
    y += pas;
    libelle(y, "reg_aff", "reg_aff_aide");
    selecteur(y, g.reglages.sans_bordure ? T("sans_bordure") : T("fenetre"), Z_AFF_G, Z_AFF_D);
    y += pas;
    // DLSS : mode (aide changée si nvngx_dlss.dll manque à côté du jeu, dans son dossier dlss et dans outils\dlss, où le
    // paquet le range), puis modèle. Sur une
    // carte qui n'est pas une NVIDIA RTX : « Indisponible », grisé ; le mode choisi reste dans recomp.ini (retrouvé en
    // revenant à la carte NVIDIA), sans effet dans le jeu (dlss.cpp, carte_dlss).
    static const char* const modes[6] = {"dlss_non", "dlss_dlaa", "dlss_qualite", "dlss_equilibre", "dlss_perf", "dlss_ultra"};
    const bool possible = dlss_possible();
    const int mode = possible ? std::clamp(g.reglages.dlss, 0, 5) : 0;   // mode qui servira
    static bool dll = false;   // relu une fois par seconde (quatre dossiers, comme dlss.cpp)
    static ULONGLONG dll_t = 0;
    if (GetTickCount64() - dll_t > 1000) {
        dll_t = GetTickCount64();
        dll = false;
        for (const std::wstring& dossier : {g.jeu, g.jeu + L"\\dlss", g.dossier_exe + L"\\outils\\dlss", g.recomp + L"\\outils\\dlss"})
            dll = dll || GetFileAttributesW((dossier + L"\\nvngx_dlss.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (!possible) {
        libelle(y, "reg_dlss", "reg_dlss_rtx");
        selecteur(y, T("dlss_indispo"), Z_DLSS_G, Z_DLSS_D, false);
    } else if (mode && !dll) {
        libelle(y, "reg_dlss", "reg_dlss_manque");
        selecteur(y, T(modes[mode]), Z_DLSS_G, Z_DLSS_D);
    } else {   // super résolution (Qualité à Ultra perf.) : mise à l'échelle expérimentale
        libelle(y, "reg_dlss", mode >= 2 ? "reg_dlss_exp" : "reg_dlss_aide", mode >= 2 ? AMBRE : GRIS);
        selecteur(y, T(modes[mode]), Z_DLSS_G, Z_DLSS_D);
    }
    y += pas;
    libelle(y, "reg_dlss_modele", "reg_dlss_modele_aide");
    selecteur(y, g.reglages.dlss_modele == 2 ? T("dlss_45") : T("dlss_4"), Z_DLSSM_G, Z_DLSSM_D, mode != 0);
    y += pas;
    // FSR d'AMD (2026-11-12) : sur toute carte ; sans effet quand le DLSS sert (le jeu fait le même choix, fsr.cpp), ou
    // quand les deux bibliothèques d'AMD manquent (à côté du jeu, dans son dossier fsr ou dans outils\fsr). Puis la
    // version : automatique (FSR 4 si la carte le propose), FSR 4 ou FSR 3.1.
    static const char* const modes_fsr[6] = {"dlss_non", "fsr_natif", "dlss_qualite", "fsr_equilibre", "dlss_perf", "dlss_ultra"};
    const int mode_fsr = std::clamp(g.reglages.fsr, 0, 5);
    static bool fsr_dll = false;   // relu une fois par seconde (huit fichiers cherchés)
    static ULONGLONG fsr_vu = 0;
    if (const ULONGLONG t = GetTickCount64(); !fsr_vu || t - fsr_vu > 1000) {
        fsr_vu = t;
        fsr_dll = false;
        for (const std::wstring& dossier : {g.jeu, g.jeu + L"\\fsr", g.dossier_exe + L"\\outils\\fsr", g.recomp + L"\\outils\\fsr"})
            fsr_dll = fsr_dll || (GetFileAttributesW((dossier + L"\\amd_fidelityfx_loader_vk.dll").c_str()) != INVALID_FILE_ATTRIBUTES &&
                                  GetFileAttributesW((dossier + L"\\amd_fidelityfx_upscaler_vk.dll").c_str()) != INVALID_FILE_ATTRIBUTES);
    }
    const bool fsr_sert = mode_fsr && mode == 0 && fsr_dll;   // comme le jeu : sans les fichiers d'AMD, render_scale
    if (mode_fsr && mode) libelle(y, "reg_fsr", "reg_fsr_dlss", AMBRE);
    else if (mode_fsr && !fsr_dll) libelle(y, "reg_fsr", "reg_fsr_manque", AMBRE);
    else libelle(y, "reg_fsr", mode_fsr >= 2 ? "reg_dlss_exp" : "reg_fsr_aide", mode_fsr >= 2 ? AMBRE : GRIS);
    selecteur(y, T(modes_fsr[mode_fsr]), Z_FSR_G, Z_FSR_D);
    y += pas;
    libelle(y, "reg_fsr_version", "reg_fsr_version_aide");
    selecteur(y, g.reglages.fsr_version == 4 ? L"FSR 4" : g.reglages.fsr_version == 3 ? L"FSR 3.1" : T("gpu_auto"), Z_FSRV_G,
              Z_FSRV_D, fsr_sert);
    y += pas;
    // rendu 3D : fixé par le mode du DLSS ou du FSR quand il y en a un (100 % en DLAA ou en natif) ; sous 100 %, mise à
    // l'échelle expérimentale
    const int e_dlss = mode ? DLSS_ECHELLES[mode] : fsr_sert ? FSR_ECHELLES[mode_fsr] : 0;
    if (e_dlss >= 100 || (!e_dlss && g.reglages.echelle >= 100)) libelle(y, "reg_natif", "reg_natif_aide");
    else libelle(y, "reg_natif", !e_dlss ? "reg_natif_exp" : mode ? "reg_natif_dlss" : "reg_natif_fsr", AMBRE);
    wchar_t echelle[48];
    if (e_dlss) swprintf(echelle, 48, T(mode ? "rendu_dlss" : "rendu_fsr"), e_dlss);
    else if (g.reglages.echelle >= 100) swprintf(echelle, 48, L"%ls", T("natif"));
    else swprintf(echelle, 48, L"%d %%", g.reglages.echelle);
    selecteur(y, echelle, Z_NATIF_G, Z_NATIF_D, e_dlss == 0);
    y += pas + 6;
    ligne(PX0 + 24, y, PX1 - 24, y, VERT, 0.15f);
    texte_lignes(T("gra_note"), F_PETIT, PX0 + 24, y + 12, PX1 - PX0 - 48, GRIS);
}

// Nom d'une touche d'après son scancode (0xE0xx : touche étendue), sur le clavier réglé dans Windows
std::wstring nom_touche(UINT sc) {
    switch (sc) {
    case 0: return L"—";
    case 0xE048: return L"↑";
    case 0xE050: return L"↓";
    case 0xE04B: return L"←";
    case 0xE04D: return L"→";
    }
    wchar_t b[64] = {};
    if (GetKeyNameTextW((LONG)(((sc & 0xFF) << 16) | ((sc & 0xE000) ? (1 << 24) : 0)), b, 64) <= 0) return L"?";
    return b;
}

// ---- page Contrôles : onglets CLAVIER · MANETTE, à droite du titre (soulignés comme FR · EN)
void onglets_controles() {
    float x = PX0 + 200;
    const float y = PY0 + 20;
    for (int k = 0; k < 2; k++) {
        const int id = k ? Z_ONGLET_MANETTE : Z_ONGLET_CLAVIER;
        const bool ouvert = (k == 1) == g.onglet_manette;
        const float w = texte(T(k ? "ong_manette" : "ong_clavier"), F_PETIT, x, y, ouvert ? VERT : survol(id) ? BLANC : GRIS, 1.f, 2.f);
        if (ouvert) ligne(x, y + 17, x + w - 2, y + 17, VERT, 1.f, 1.f);
        zone(D2D1::RectF(x - 6, y - 5, x + w + 6, y + 21), id);
        x += w + 26;
    }
}

// ---- onglet Manette : la manette du joueur, dessinée à plat (2026-11-12, 2e version). Demande de l'utilisateur, avec
// quatre modèles d'« overlay » (manettes pleines et ombrées, à plat) : « je veux que tu dessines la manette avec ce
// type d'overlay », puis, à la question posée : la manette branchée, la Pro de la Switch quand aucune ne l'est. Dessins
// faits ici à la main (chemins de Bézier dans un repère de 300 × 190, pas d'image, pas de logo de marque) : Pro de la
// Switch, Xbox, PlayStation (DualShock 4 sombre, DualSense clair), Joy-Con en paire, Joy-Con seul à l'horizontale.
// Chaque bouton est une entrée physique (EntreePad) : il s'allume quand on l'appuie, comme un overlay d'entrées, et les
// sticks bougent avec la manette. Un bouton donné à une action de la Switch se clique : il attend alors l'appui qui le
// remplace (même chose qu'un clic dans la liste) ; au survol, une étiquette dit l'action qu'il fait.
// Vérification sans manette : LANCEUR_MANETTE=<SDL_GamepadType> force un dessin, LANCEUR_APPUIS=<masque hexadécimal
// des entrées> les montre appuyées.
const float MX = 545, MY = 118, MS = 1.04f;   // origine et échelle du repère 300 × 190
float mx(float x) { return MX + x * MS; }
float my(float y) { return MY + y * MS; }

void rond(float cx, float cy, float r, uint32_t rgb, float a, bool plein, float ep = 1.f) {
    const D2D1_ELLIPSE e = D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r);
    if (plein) g_dc->FillEllipse(e, pinceau(rgb, a));
    else g_dc->DrawEllipse(e, pinceau(rgb, a), ep);
}
void arrondi(float x0, float y0, float x1, float y1, float r, uint32_t rgb, float a, bool plein, float ep = 1.f) {
    const D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x0, y0, x1, y1), r, r);
    if (plein) g_dc->FillRoundedRectangle(rr, pinceau(rgb, a));
    else g_dc->DrawRoundedRectangle(rr, pinceau(rgb, a), ep);
}

// Chemins : sous-ensemble du « d » de SVG (M, L, C, Q, Z, coordonnées absolues du repère), géométries faites une fois
std::unordered_map<const char*, ID2D1PathGeometry*> g_chemins;
ID2D1PathGeometry* chemin(const char* d) {
    auto it = g_chemins.find(d);
    if (it != g_chemins.end()) return it->second;
    ID2D1PathGeometry* p = nullptr;
    ID2D1GeometrySink* s = nullptr;
    if (FAILED(g_fab->CreatePathGeometry(&p)) || FAILED(p->Open(&s))) {
        rel(p);
        g_chemins[d] = nullptr;
        return nullptr;
    }
    s->SetFillMode(D2D1_FILL_MODE_WINDING);
    const char* c = d;
    char cmd = 0;
    bool ouvert = false;
    auto nombre = [&](float& v) {
        while (*c == ' ' || *c == ',') c++;
        char* fin = nullptr;
        v = strtof(c, &fin);
        if (fin == c) return false;
        c = fin;
        return true;
    };
    auto pt = [](float x, float y) { return D2D1::Point2F(mx(x), my(y)); };
    for (;;) {
        while (*c == ' ' || *c == ',') c++;
        if (!*c) break;
        if ((*c >= 'A' && *c <= 'Z')) cmd = *c++;
        float v[6];
        if (cmd == 'M' && nombre(v[0]) && nombre(v[1])) {
            if (ouvert) s->EndFigure(D2D1_FIGURE_END_OPEN);
            s->BeginFigure(pt(v[0], v[1]), D2D1_FIGURE_BEGIN_FILLED);
            ouvert = true;
            cmd = 'L';
        } else if (cmd == 'L' && nombre(v[0]) && nombre(v[1])) {
            s->AddLine(pt(v[0], v[1]));
        } else if (cmd == 'C' && nombre(v[0]) && nombre(v[1]) && nombre(v[2]) && nombre(v[3]) && nombre(v[4]) && nombre(v[5])) {
            s->AddBezier(D2D1::BezierSegment(pt(v[0], v[1]), pt(v[2], v[3]), pt(v[4], v[5])));
        } else if (cmd == 'Q' && nombre(v[0]) && nombre(v[1]) && nombre(v[2]) && nombre(v[3])) {
            s->AddQuadraticBezier(D2D1::QuadraticBezierSegment(pt(v[0], v[1]), pt(v[2], v[3])));
        } else if (cmd == 'Z') {
            if (ouvert) s->EndFigure(D2D1_FIGURE_END_CLOSED);
            ouvert = false;
            cmd = 0;
        } else {
            break;   // chemin mal écrit : on s'arrête là
        }
    }
    if (ouvert) s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close();
    rel(s);
    g_chemins[d] = p;
    return p;
}
void remplir(const char* d, uint32_t c, float a = 1.f) {
    if (ID2D1PathGeometry* p = chemin(d)) g_dc->FillGeometry(p, pinceau(c, a));
}
void contour(const char* d, uint32_t c, float a, float ep) {
    if (ID2D1PathGeometry* p = chemin(d)) g_dc->DrawGeometry(p, pinceau(c, a), ep);
}

// ---- état d'une entrée de la manette : action de la Switch qu'elle fait, appuyée, survolée, en attente d'un appui
const wchar_t* const NOMS_SWITCH[NB_PAD] = {L"A", L"B", L"X", L"Y", L"L", L"R", L"ZL", L"ZR", L"+", L"−",
                                            L"L ●", L"R ●", L"↑", L"↓", L"←", L"→"};
uint32_t appuis() {
    static const wchar_t* essai = _wgetenv(L"LANCEUR_APPUIS");   // vérification : entrées montrées appuyées
    return manettes().appuyees | (essai ? (uint32_t)wcstoul(essai, nullptr, 16) : 0u);
}
struct EtatBouton {
    int entree = P_AUCUNE, action = -1;
    bool appui = false, cliquable = false, survol = false, attente = false;
};
EtatBouton etat_entree(int e) {
    EtatBouton s;
    s.entree = e;
    for (int i = 0; i < NB_PAD && s.action < 0; i++)
        if (g.reglages.manette[i] == e) s.action = i;
    s.appui = (appuis() >> e) & 1;
    // cliquable : une entrée qui fait un bouton de la Switch (clic : ce bouton attend l'entrée voulue) et, pendant une
    // attente, toutes (clic : l'entrée est donnée au bouton qui attend ; lanceur.cpp, pad_entree_cliquee)
    s.cliquable = s.action >= 0 || g.capture_pad >= 0;
    s.survol = s.cliquable && ::lz::survol(Z_ENTREE0 + e);
    s.attente = s.action >= 0 && g.capture_pad == s.action;
    return s;
}
float pulsation() { return 0.5f + 0.5f * std::sin((float)(GetTickCount64() % 100000) * 0.006f); }

// étiquette du bouton survolé : l'action de la Switch qu'il fait (dessinée en dernier, par-dessus le reste)
struct {
    bool la = false;
    float x = 0, y = 0;
    int action = -1;
} g_etiquette;
void zone_bouton(const EtatBouton& s, float x0, float y0, float x1, float y1) {
    if (!s.cliquable) return;
    zone(D2D1::RectF(mx(x0 - 2), my(y0 - 2), mx(x1 + 2), my(y1 + 2)), Z_ENTREE0 + s.entree);
    if (s.survol && s.action >= 0)
        g_etiquette.la = true, g_etiquette.x = mx(x1), g_etiquette.y = my(y0), g_etiquette.action = s.action;
}
void etiquette() {
    if (!g_etiquette.la) return;
    g_etiquette.la = false;
    const float x = g_etiquette.x + 2, y = g_etiquette.y - 16;
    rect(x, y, x + 34, y + 16, NOIR, 0.88f);
    cadre(x, y, x + 34, y + 16, VERT, 0.9f);
    texte(NOMS_SWITCH[g_etiquette.action], F_PETIT, x + 17, y + 1, VERT, 1.f, 0.f, 1);
}

// Signe d'un bouton (lettre, flèche…) : sombre sur un bouton allumé
auto signe_texte(float x, float y, const wchar_t* s, uint32_t c, Fmt f = F_PETIT) {
    return [=](bool appui) { texte(s, f, mx(x), my(y) - (f == F_GRAS ? 8.5f : 7.f), appui ? NOIR : c, 1.f, 0.f, 1); };
}
auto sans_signe() {
    return [](bool) {};
}

// Boutons : fond, puis l'état (appuyé : plein et halo verts ; en attente : anneau ambre pulsé ; survolé : contour
// blanc), puis le signe ; une entrée sans action s'allume plus pâle
template <class F> void b_rond(int e, float x, float y, float r, uint32_t fond, F signe) {
    const EtatBouton s = etat_entree(e);
    rond(mx(x), my(y), r * MS, fond, 1.f, true);
    if (s.appui) {
        rond(mx(x), my(y), (r + 4) * MS, VERT, 0.22f, true);
        rond(mx(x), my(y), r * MS, s.action >= 0 ? VERT : VERT_PALE, 0.9f, true);
    }
    if (s.attente) rond(mx(x), my(y), (r + 2.5f) * MS, AMBRE, 0.35f + 0.65f * pulsation(), false, 2.f);
    else if (s.survol) rond(mx(x), my(y), (r + 1.2f) * MS, BLANC, 1.f, false, 1.5f);
    signe(s.appui);
    zone_bouton(s, x - r, y - r, x + r, y + r);
}
template <class F> void b_capsule(int e, float x0, float y0, float x1, float y1, float r, uint32_t fond, F signe) {
    const EtatBouton s = etat_entree(e);
    arrondi(mx(x0), my(y0), mx(x1), my(y1), r * MS, fond, 1.f, true);
    if (s.appui) {
        arrondi(mx(x0 - 3), my(y0 - 3), mx(x1 + 3), my(y1 + 3), (r + 3) * MS, VERT, 0.22f, true);
        arrondi(mx(x0), my(y0), mx(x1), my(y1), r * MS, s.action >= 0 ? VERT : VERT_PALE, 0.9f, true);
    }
    if (s.attente) arrondi(mx(x0 - 2), my(y0 - 2), mx(x1 + 2), my(y1 + 2), (r + 2) * MS, AMBRE, 0.35f + 0.65f * pulsation(), false, 2.f);
    else if (s.survol) arrondi(mx(x0 - 1), my(y0 - 1), mx(x1 + 1), my(y1 + 1), (r + 1) * MS, BLANC, 1.f, false, 1.5f);
    signe(s.appui);
    zone_bouton(s, x0, y0, x1, y1);
}
template <class F> void b_chemin(int e, const char* d, float x0, float y0, float x1, float y1, uint32_t fond, F signe) {
    const EtatBouton s = etat_entree(e);
    remplir(d, fond);
    if (s.appui) remplir(d, s.action >= 0 ? VERT : VERT_PALE, 0.9f);
    if (s.attente) contour(d, AMBRE, 0.35f + 0.65f * pulsation(), 2.f);
    else if (s.survol) contour(d, BLANC, 1.f, 1.5f);
    signe(s.appui);
    zone_bouton(s, x0, y0, x1, y1);
}

// Stick : cuvette, puis chapeau (son clic est un bouton) déplacé comme le stick de la manette
void stick(int e, int cote, float x, float y, float rc, float rh, uint32_t cuvette, uint32_t chapeau, uint32_t centre) {
    rond(mx(x), my(y), rc * MS, cuvette, 1.f, true);
    const float* a = manettes().axes + cote * 2;
    const float jeu = rc - rh - 1.5f, cx = x + a[0] * jeu, cy = y + a[1] * jeu;
    b_rond(e, cx, cy, rh, chapeau, [=](bool appui) {
        if (!appui) rond(mx(cx), my(cy), (rh - 4) * MS, centre, 1.f, true);
        rond(mx(cx), my(cy), (rh - 0.8f) * MS, 0x000000, 0.25f, false, 1.f);
    });
}
// Croix d'un seul tenant (Pro, Xbox) : quatre branches, chacune son entrée, et le centre
void croix_pad(float cx, float cy, float l, float w, uint32_t fond, uint32_t signe) {
    const float h = w / 2;
    b_capsule(P_HAUT, cx - h, cy - l - h, cx + h, cy - h, 2, fond, signe_texte(cx, cy - l / 2 - h, L"▲", signe));
    b_capsule(P_BAS, cx - h, cy + h, cx + h, cy + l + h, 2, fond, signe_texte(cx, cy + l / 2 + h, L"▼", signe));
    b_capsule(P_GAUCHE, cx - l - h, cy - h, cx - h, cy + h, 2, fond, signe_texte(cx - l / 2 - h, cy, L"◀", signe));
    b_capsule(P_DROITE, cx + h, cy - h, cx + l + h, cy + h, 2, fond, signe_texte(cx + l / 2 + h, cy, L"▶", signe));
    rect(mx(cx - h), my(cy - h), mx(cx + h), my(cy + h), fond, 1.f);
}

// ---- Pro de la Switch (et manette GameCube, rare) : anthracite, ZL et ZR en languettes au-dessus du corps
const char* const PRO_CORPS = "M 34 56 C 44 40 100 36 150 36 C 200 36 256 40 266 56 C 284 74 294 118 294 150 "
                              "C 294 172 276 182 258 178 C 238 174 226 152 212 140 C 196 130 104 130 88 140 "
                              "C 74 152 62 174 42 178 C 24 182 6 172 6 150 C 6 118 16 74 34 56 Z";
const char* const PRO_FACE = "M 62 58 C 92 46 208 46 238 58 C 252 76 252 108 240 124 C 212 118 88 118 60 124 "
                             "C 48 108 48 76 62 58 Z";
const char* const PRO_L = "M 28 46 C 40 30 84 24 114 24 L 114 34 C 86 34 46 40 34 54 Z";
const char* const PRO_R = "M 272 46 C 260 30 216 24 186 24 L 186 34 C 214 34 254 40 266 54 Z";
void manette_pro() {
    const uint32_t corps = 0x2B2B2F, face = 0x333338, bouton = 0x404046, signe = 0xD8D8DC, cuvette = 0x18181B,
                   chapeau = 0x4A4A51, centre = 0x3B3B41;
    b_capsule(P_GACHETTE_G, 86, 8, 122, 24, 6, bouton, signe_texte(104, 16, L"ZL", signe));
    b_capsule(P_GACHETTE_D, 178, 8, 214, 24, 6, bouton, signe_texte(196, 16, L"ZR", signe));
    b_chemin(P_EPAULE_G, PRO_L, 28, 24, 114, 54, bouton, sans_signe());   // L et R : bandes du haut, sans nom
    b_chemin(P_EPAULE_D, PRO_R, 186, 24, 272, 54, bouton, sans_signe());
    remplir(PRO_CORPS, corps);
    remplir(PRO_FACE, face);
    contour(PRO_CORPS, 0x000000, 0.55f, 1.f);
    b_capsule(P_BACK, 112, 52, 126, 58, 3, bouton, sans_signe());   // −
    b_capsule(P_START, 180, 52, 194, 58, 3, bouton, [=](bool appui) {   // + : barre verticale
        arrondi(mx(184.5f), my(48), mx(189.5f), my(62), 2.f * MS, appui ? VERT : bouton, 1.f, true);
    });
    b_capsule(P_MISC1, 125, 74, 137, 86, 2, bouton, [=](bool appui) {   // capture
        rond(mx(131), my(80), 3.f * MS, appui ? NOIR : cuvette, 1.f, true);
    });
    b_rond(P_GUIDE, 169, 80, 6.5f, bouton, [=](bool appui) {   // accueil
        rond(mx(169), my(80), 3.6f * MS, appui ? NOIR : signe, 0.85f, false, 1.f);
    });
    stick(P_STICK_G, 0, 78, 80, 22, 16, cuvette, chapeau, centre);
    rond(mx(110), my(116), 21 * MS, cuvette, 1.f, true);
    croix_pad(110, 116, 12, 12, bouton, signe);
    stick(P_STICK_D, 1, 190, 116, 22, 16, cuvette, chapeau, centre);
    b_rond(P_NORD, 224, 60, 10.5f, bouton, signe_texte(224, 60, L"X", signe, F_GRAS));
    b_rond(P_EST, 246, 80, 10.5f, bouton, signe_texte(246, 80, L"A", signe, F_GRAS));
    b_rond(P_SUD, 224, 100, 10.5f, bouton, signe_texte(224, 100, L"B", signe, F_GRAS));
    b_rond(P_OUEST, 202, 80, 10.5f, bouton, signe_texte(202, 80, L"Y", signe, F_GRAS));
}

// ---- Xbox (et manettes inconnues ou génériques) : noire, lettres de couleur, bouton central sans logo
const char* const XB_CORPS = "M 52 60 C 66 48 96 42 118 42 L 182 42 C 204 42 234 48 248 60 C 272 70 286 92 292 126 "
                             "C 298 156 296 178 278 184 C 262 190 246 176 232 160 C 220 148 206 144 196 144 "
                             "L 104 144 C 94 144 80 148 68 160 C 54 176 38 190 22 184 C 4 178 2 156 8 126 "
                             "C 14 92 28 70 52 60 Z";
const char* const XB_HAUT = "M 108 42 L 192 42 L 180 66 C 168 68 132 68 120 66 Z";
const char* const XB_LB = "M 36 56 C 48 40 80 32 108 32 L 108 40 C 82 40 54 46 44 60 Z";
const char* const XB_RB = "M 264 56 C 252 40 220 32 192 32 L 192 40 C 218 40 246 46 256 60 Z";
const char* const XB_LT = "M 58 34 C 58 18 66 8 80 6 C 88 6 94 12 94 20 L 94 34 Z";
const char* const XB_RT = "M 242 34 C 242 18 234 8 220 6 C 212 6 206 12 206 20 L 206 34 Z";
void manette_xbox() {
    const uint32_t corps = 0x161618, haut = 0x202024, bouton = 0x27272C, signe = 0xCFCFD4, cuvette = 0x07070A,
                   chapeau = 0x323238, centre = 0x41414A, face = 0x1E1E23;
    b_chemin(P_GACHETTE_G, XB_LT, 58, 6, 94, 34, bouton, signe_texte(76, 21, L"LT", signe));
    b_chemin(P_GACHETTE_D, XB_RT, 206, 6, 242, 34, bouton, signe_texte(224, 21, L"RT", signe));
    b_chemin(P_EPAULE_G, XB_LB, 36, 32, 108, 60, bouton, sans_signe());   // LB et RB : bandes du haut, sans nom
    b_chemin(P_EPAULE_D, XB_RB, 192, 32, 264, 60, bouton, sans_signe());
    remplir(XB_CORPS, corps);
    remplir(XB_HAUT, haut);
    contour(XB_CORPS, 0x000000, 0.6f, 1.f);
    b_rond(P_GUIDE, 150, 54, 10, 0xE4E4E8, [=](bool appui) {   // bouton central (sans logo)
        rond(mx(150), my(54), 6.5f * MS, appui ? NOIR : 0xB8B8BF, 0.9f, false, 1.3f);
    });
    b_rond(P_BACK, 128, 84, 6.5f, bouton, [=](bool appui) {   // View : deux carrés
        const uint32_t c = appui ? NOIR : signe;
        cadre(mx(124.6f), my(81.2f), mx(129.4f), my(85.6f), c, 1.f);
        cadre(mx(126.6f), my(83.2f), mx(131.4f), my(87.6f), c, 1.f);
    });
    b_rond(P_START, 172, 84, 6.5f, bouton, [=](bool appui) {   // Menu : trois traits
        for (int k = -1; k <= 1; k++)
            ligne(mx(168.6f), my(84 + k * 2.2f), mx(175.4f), my(84 + k * 2.2f), appui ? NOIR : signe, 1.f, 1.f);
    });
    b_capsule(P_MISC1, 145, 95, 155, 100, 2.5f, bouton, sans_signe());   // Share (Xbox Series)
    stick(P_STICK_G, 0, 74, 84, 22, 15, cuvette, chapeau, centre);
    rond(mx(112), my(124), 22 * MS, cuvette, 1.f, true);
    croix_pad(112, 124, 13, 13, 0x2D2D33, signe);
    stick(P_STICK_D, 1, 190, 124, 22, 15, cuvette, chapeau, centre);
    b_rond(P_NORD, 228, 66, 11, face, signe_texte(228, 66, L"Y", 0xE5CC2A, F_GRAS));
    b_rond(P_EST, 250, 86, 11, face, signe_texte(250, 86, L"B", 0xD9413F, F_GRAS));
    b_rond(P_SUD, 228, 106, 11, face, signe_texte(228, 106, L"A", 0x5DBB4F, F_GRAS));
    b_rond(P_OUEST, 206, 86, 11, face, signe_texte(206, 86, L"X", 0x3C83D9, F_GRAS));
}

// ---- PlayStation : DualShock 4 sombre, DualSense clair ; pavé tactile, symboles de couleur, croix en quatre boutons
const char* const PS_CORPS = "M 44 50 C 58 40 90 36 112 36 L 188 36 C 210 36 242 40 256 50 C 276 62 286 86 288 112 "
                             "C 290 140 290 164 282 178 C 274 190 252 190 244 178 C 234 162 226 140 214 128 "
                             "C 204 120 96 120 86 128 C 74 140 66 162 56 178 C 48 190 26 190 18 178 "
                             "C 10 164 10 140 12 112 C 14 86 24 62 44 50 Z";
const char* const PS_CENTRE = "M 98 80 L 202 80 C 212 96 214 112 210 124 C 194 118 106 118 90 124 C 86 112 88 96 98 80 Z";
const char* const PS_PAVE = "M 106 38 L 194 38 L 190 78 C 170 81 130 81 110 78 Z";
const char* const PS_L1 = "M 38 48 C 48 32 74 24 100 24 L 100 36 C 78 36 56 42 46 54 Z";
const char* const PS_R1 = "M 262 48 C 252 32 226 24 200 24 L 200 36 C 222 36 244 42 254 54 Z";
const char* const PS_L2 = "M 52 32 C 52 18 60 8 72 6 C 82 6 88 12 88 20 L 88 32 Z";
const char* const PS_R2 = "M 248 32 C 248 18 240 8 228 6 C 218 6 212 12 212 20 L 212 32 Z";
void manette_ps(bool ps5) {
    const uint32_t corps = ps5 ? 0xE9E9EC : 0x232429, centre_c = ps5 ? 0x1F2025 : 0x1B1C20,
                   pave = ps5 ? 0xF4F4F6 : 0x17181C, bouton = ps5 ? 0x26272C : 0x2F3036, signe = 0xD5D5DA,
                   cuvette = ps5 ? 0x15161A : 0x0E0E10, chapeau = 0x35363C, centre = 0x2A2B30,
                   creux = ps5 ? 0xD4D4D9 : 0x1B1C20, tranche = ps5 ? 0xDBDBDF : 0x2F3036, st = ps5 ? 0x3A3B40 : signe;
    b_chemin(P_GACHETTE_G, PS_L2, 52, 6, 88, 32, tranche, signe_texte(70, 20, L"L2", st));
    b_chemin(P_GACHETTE_D, PS_R2, 212, 6, 248, 32, tranche, signe_texte(230, 20, L"R2", st));
    b_chemin(P_EPAULE_G, PS_L1, 38, 24, 100, 54, tranche, sans_signe());   // L1 et R1 : bandes du haut, sans nom
    b_chemin(P_EPAULE_D, PS_R1, 200, 24, 262, 54, tranche, sans_signe());
    remplir(PS_CORPS, corps);
    remplir(PS_CENTRE, centre_c);
    contour(PS_CORPS, ps5 ? 0x8E8E96 : 0x000000, 0.6f, 1.f);
    b_chemin(P_PAVE, PS_PAVE, 106, 38, 194, 80, pave, sans_signe());   // pavé tactile (son clic)
    if (ps5) contour(PS_PAVE, 0xBDBDC4, 1.f, 1.f);
    b_capsule(P_BACK, 92, 42, 99, 58, 3, bouton, sans_signe());    // Share / Create
    b_capsule(P_START, 201, 42, 208, 58, 3, bouton, sans_signe());  // Options
    // croix : quatre boutons séparés
    rond(mx(60), my(84), 24 * MS, creux, 1.f, true);
    b_capsule(P_HAUT, 54, 65, 66, 77, 2.5f, bouton, signe_texte(60, 71, L"▲", signe));
    b_capsule(P_BAS, 54, 91, 66, 103, 2.5f, bouton, signe_texte(60, 97, L"▼", signe));
    b_capsule(P_GAUCHE, 41, 78, 53, 90, 2.5f, bouton, signe_texte(47, 84, L"◀", signe));
    b_capsule(P_DROITE, 67, 78, 79, 90, 2.5f, bouton, signe_texte(73, 84, L"▶", signe));
    // △ ○ ✕ □, dessinés (pas des caractères) et de leurs couleurs
    rond(mx(240), my(84), 26 * MS, creux, 1.f, true);
    b_rond(P_NORD, 240, 66, 10, bouton, [=](bool appui) {
        const uint32_t c = appui ? NOIR : 0x3FC9A3;
        const float s = 5.2f;
        ligne(mx(240), my(66 - s), mx(240 + s), my(66 + s * 0.6f), c, 1.f, 1.6f);
        ligne(mx(240 + s), my(66 + s * 0.6f), mx(240 - s), my(66 + s * 0.6f), c, 1.f, 1.6f);
        ligne(mx(240 - s), my(66 + s * 0.6f), mx(240), my(66 - s), c, 1.f, 1.6f);
    });
    b_rond(P_EST, 258, 84, 10, bouton, [=](bool appui) { rond(mx(258), my(84), 5.f * MS, appui ? NOIR : 0xE5545F, 1.f, false, 1.6f); });
    b_rond(P_SUD, 240, 102, 10, bouton, [=](bool appui) {
        const uint32_t c = appui ? NOIR : 0x7DA6E8;
        ligne(mx(236), my(98), mx(244), my(106), c, 1.f, 1.6f);
        ligne(mx(236), my(106), mx(244), my(98), c, 1.f, 1.6f);
    });
    b_rond(P_OUEST, 222, 84, 10, bouton, [=](bool appui) {
        cadre(mx(217.6f), my(79.6f), mx(226.4f), my(88.4f), appui ? NOIR : 0xE59BCB, 1.f, 1.6f);
    });
    stick(P_STICK_G, 0, 106, 116, 18, 13, cuvette, chapeau, centre);
    stick(P_STICK_D, 1, 194, 116, 18, 13, cuvette, chapeau, centre);
    b_rond(P_GUIDE, 150, 102, 6.5f, bouton, sans_signe());   // bouton PS (sans logo)
    if (ps5) b_capsule(P_MISC1, 144, 112, 156, 117, 2.5f, bouton, sans_signe());   // micro
}

// ---- Joy-Con : gris ; en paire (verticaux, côte à côte) ou seul (à l'horizontale, le rail en haut)
const uint32_t JC_CORPS = 0x8B8E94, JC_BOUTON = 0x2D2E32, JC_SIGNE = 0xE2E2E5, JC_CUVETTE = 0x1F2023, JC_CHAPEAU = 0x36373C,
               JC_CENTRE = 0x2A2B2F, JC_RAIL = 0x5D6066, JC_TRANCHE = 0x3A3B40;
const char* const JC_G = "M 146 28 L 120 28 C 98 28 82 44 82 66 L 82 150 C 82 172 98 188 120 188 L 146 188 Z";
const char* const JC_D = "M 154 28 L 180 28 C 202 28 218 44 218 66 L 218 150 C 218 172 202 188 180 188 L 154 188 Z";
void joycons_paire() {
    b_capsule(P_GACHETTE_G, 98, 6, 132, 16, 4, JC_TRANCHE, signe_texte(115, 11, L"ZL", JC_SIGNE));
    b_capsule(P_GACHETTE_D, 168, 6, 202, 16, 4, JC_TRANCHE, signe_texte(185, 11, L"ZR", JC_SIGNE));
    b_capsule(P_EPAULE_G, 92, 18, 140, 27, 4, JC_TRANCHE, signe_texte(116, 22.5f, L"L", JC_SIGNE));
    b_capsule(P_EPAULE_D, 160, 18, 208, 27, 4, JC_TRANCHE, signe_texte(184, 22.5f, L"R", JC_SIGNE));
    remplir(JC_G, JC_CORPS);
    remplir(JC_D, JC_CORPS);
    rect(mx(143), my(32), mx(146), my(184), JC_RAIL, 1.f);
    rect(mx(154), my(32), mx(157), my(184), JC_RAIL, 1.f);
    contour(JC_G, 0x000000, 0.4f, 1.f);
    contour(JC_D, 0x000000, 0.4f, 1.f);
    // Joy-Con gauche : −, stick, quatre flèches, capture
    b_capsule(P_BACK, 126, 38, 138, 43, 2, JC_BOUTON, sans_signe());
    stick(P_STICK_G, 0, 114, 72, 17, 12, JC_CUVETTE, JC_CHAPEAU, JC_CENTRE);
    b_rond(P_HAUT, 114, 104, 8, JC_BOUTON, signe_texte(114, 104, L"▲", JC_SIGNE));
    b_rond(P_BAS, 114, 132, 8, JC_BOUTON, signe_texte(114, 132, L"▼", JC_SIGNE));
    b_rond(P_GAUCHE, 100, 118, 8, JC_BOUTON, signe_texte(100, 118, L"◀", JC_SIGNE));
    b_rond(P_DROITE, 128, 118, 8, JC_BOUTON, signe_texte(128, 118, L"▶", JC_SIGNE));
    b_capsule(P_MISC1, 122, 156, 132, 166, 2, JC_BOUTON, sans_signe());
    // Joy-Con droit : +, X A B Y, stick, accueil
    b_capsule(P_START, 160, 38.5f, 172, 43.5f, 2, JC_BOUTON, [=](bool appui) {
        arrondi(mx(163.5f), my(35), mx(168.5f), my(47), 1.5f * MS, appui ? VERT : JC_BOUTON, 1.f, true);
    });
    b_rond(P_NORD, 186, 58, 8, JC_BOUTON, signe_texte(186, 58, L"X", JC_SIGNE));
    b_rond(P_EST, 200, 72, 8, JC_BOUTON, signe_texte(200, 72, L"A", JC_SIGNE));
    b_rond(P_SUD, 186, 86, 8, JC_BOUTON, signe_texte(186, 86, L"B", JC_SIGNE));
    b_rond(P_OUEST, 172, 72, 8, JC_BOUTON, signe_texte(172, 72, L"Y", JC_SIGNE));
    stick(P_STICK_D, 1, 186, 120, 17, 12, JC_CUVETTE, JC_CHAPEAU, JC_CENTRE);
    b_rond(P_GUIDE, 176, 158, 6, JC_BOUTON, [=](bool appui) {
        rond(mx(176), my(158), 3.3f * MS, appui ? NOIR : JC_SIGNE, 0.85f, false, 1.f);
    });
}
const char* const JC_SEUL = "M 20 76 L 280 76 L 280 116 C 280 142 262 158 236 158 L 64 158 C 38 158 20 142 20 116 Z";
void joycon_seul(bool droit) {
    rect(mx(24), my(70), mx(276), my(76), JC_RAIL, 1.f);   // rail, en haut
    b_capsule(P_EPAULE_G, 70, 61, 112, 70, 3, JC_TRANCHE, signe_texte(91, 65.5f, L"SL", JC_SIGNE));
    b_capsule(P_EPAULE_D, 188, 61, 230, 70, 3, JC_TRANCHE, signe_texte(209, 65.5f, L"SR", JC_SIGNE));
    remplir(JC_SEUL, JC_CORPS);
    contour(JC_SEUL, 0x000000, 0.4f, 1.f);
    if (!droit) {   // Joy-Con gauche : − au bout, stick, losange de flèches, capture
        b_capsule(P_START, 30, 104, 36, 124, 2, JC_BOUTON, sans_signe());
        stick(P_STICK_G, 0, 96, 116, 18, 13, JC_CUVETTE, JC_CHAPEAU, JC_CENTRE);
        b_capsule(P_MISC1, 252, 110, 264, 122, 2, JC_BOUTON, sans_signe());
    } else {        // Joy-Con droit : accueil, stick, losange de lettres, + au bout
        b_rond(P_GUIDE, 46, 116, 6, JC_BOUTON, [=](bool appui) {
            rond(mx(46), my(116), 3.3f * MS, appui ? NOIR : JC_SIGNE, 0.85f, false, 1.f);
        });
        stick(P_STICK_G, 0, 104, 116, 18, 13, JC_CUVETTE, JC_CHAPEAU, JC_CENTRE);
        b_capsule(P_START, 252, 113, 266, 119, 2, JC_BOUTON, [=](bool appui) {
            arrondi(mx(256.5f), my(109), mx(261.5f), my(123), 1.5f * MS, appui ? VERT : JC_BOUTON, 1.f, true);
        });
    }
    // losange : à l'horizontale, chaque bouton fait ce que sa place indique (lettres dehors, comme sur la console)
    const float cx = 196, cy = 116, d = 16;
    const struct { int e; float x, y; const wchar_t* lettre; float lx, ly; } b[4] = {
        {P_NORD, cx, cy - d, L"X", cx, cy - d - 13}, {P_EST, cx + d, cy, L"A", cx + d + 14, cy},
        {P_SUD, cx, cy + d, L"B", cx, cy + d + 13}, {P_OUEST, cx - d, cy, L"Y", cx - d - 14, cy}};
    for (const auto& k : b) {
        b_rond(k.e, k.x, k.y, 7.5f, JC_BOUTON, [=](bool appui) { rond(mx(k.x), my(k.y), 1.8f * MS, appui ? NOIR : JC_SIGNE, 1.f, true); });
        texte(k.lettre, F_PETIT, mx(k.lx), my(k.ly) - 7, 0x2B2C30, 1.f, 0.f, 1);
    }
}

// Dessin de la manette branchée (la première), la Pro de la Switch sans manette
void dessin_manette() {
    static const wchar_t* force = _wgetenv(L"LANCEUR_MANETTE");   // vérification : SDL_GamepadType à dessiner
    const EtatManettes& e = manettes();
    const int t = force ? _wtoi(force) : e.sdl && e.nb > 0 ? e.type : 7;
    switch (t) {
    case 0: case 1: case 2: case 3: manette_xbox(); break;
    case 4: case 5: manette_ps(false); break;
    case 6: manette_ps(true); break;
    case 8: joycon_seul(false); break;
    case 9: joycon_seul(true); break;
    case 10: joycons_paire(); break;
    default: manette_pro(); break;
    }
    etiquette();
}

// Nom d'un bouton de la Switch dans la liste et les messages : A, B … ZR, +, −, Stick L ●, Croix ↑ …
std::wstring nom_bouton_switch(int i) {
    static const wchar_t* const noms[10] = {L"A", L"B", L"X", L"Y", L"L", L"R", L"ZL", L"ZR", L"+", L"−"};
    static const wchar_t* const croix[4] = {L" ↑", L" ↓", L" ←", L" →"};
    if (i < 10) return noms[i];
    if (i == K_LSTICK) return T("pad_clic_g");
    if (i == K_RSTICK) return T("pad_clic_d");
    return std::wstring(T("croix")) + croix[i - K_UP];
}

void page_manette() {
    const EtatManettes& e = manettes();
    if (e.sdl && e.nb > 0) {   // manette branchée : son nom (la première, et combien d'autres)
        wchar_t b[256];
        swprintf(b, 256, T("pad_branchee"), e.nom.c_str());
        std::wstring s = b;
        if (e.nb > 1) {
            swprintf(b, 256, T("pad_autres"), e.nb - 1);
            s += b;
        }
        texte(s, F_PETIT, PX0 + 24, PY0 + 56, VERT_PALE, 1.f, 0.f, 0, PX1 - PX0 - 48);
    }
    dessin_manette();
    // liste : bouton de la Switch, puis l'entrée de la manette du joueur qui le fait (noms de sa famille de manettes)
    const float y0 = PY0 + 268, pas = 22;
    for (int i = 0; i < NB_PAD; i++) {
        const float cx = i < 8 ? PX0 + 24 : PX0 + 274, y = y0 + (i % 8) * pas;
        const bool attente = g.capture_pad == i;
        const bool appui = (appuis() >> g.reglages.manette[i]) & 1;
        if (i % 2 == 0) rect(cx - 6, y - 2, cx + 238, y + 20, VERT, 0.035f);
        texte(nom_bouton_switch(i), F_GRAS, cx, y + 1, attente ? AMBRE : appui ? VERT : BLANC, 1.f, 0.f, 0, 76);
        zone(D2D1::RectF(cx - 6, y - 2, cx + 78, y + 20), Z_PAD0 + i);   // le nom aussi : attendre l'entrée voulue
        const float b0 = cx + 80, b1 = cx + 234;
        rect(b0, y, b1, y + 19, appui ? VERT : NOIR, appui ? 0.3f : 0.6f);
        cadre(b0, y, b1, y + 19, attente ? AMBRE : VERT, attente ? 1.f : survol(Z_PAD0 + i) ? 0.85f : 0.3f);
        texte(L"‹", F_VALEUR, b0 + 9, y - 1, survol(Z_PADG0 + i) ? BLANC : VERT, 1.f, 0.f, 1);
        texte(L"›", F_VALEUR, b1 - 9, y - 1, survol(Z_PADD0 + i) ? BLANC : VERT, 1.f, 0.f, 1);
        zone(D2D1::RectF(b0, y, b0 + 18, y + 19), Z_PADG0 + i);
        zone(D2D1::RectF(b1 - 18, y, b1, y + 19), Z_PADD0 + i);
        zone(D2D1::RectF(b0 + 18, y, b1 - 18, y + 19), Z_PAD0 + i);
        texte(attente ? L"…" : nom_entree(g.reglages.manette[i], e.famille), F_PETIT, (b0 + b1) / 2, y + 3,
              attente ? AMBRE : appui ? BLANC : VERT_PALE, 1.f, 0.f, 1, b1 - b0 - 40);
    }
    // par défaut, et ce qu'il faut savoir ; pendant une attente : le bouton qui attend, et comment lui donner une entrée
    // (appui sur la manette ou clic sur le dessin ; clic seulement sans manette)
    bouton_contour(PX0 + 24, PY1 - 40, PX0 + 150, PY1 - 16, T("pad_defaut"), Z_PAD_DEFAUT);
    const bool attente = g.capture_pad >= 0, branchee = e.sdl && e.nb > 0;
    const char* msg = attente ? (branchee ? "pad_attente" : "pad_attente_clic")
                      : !e.sdl ? "pad_sans_sdl" : e.nb == 0 ? "pad_aucune" : "pad_note";
    wchar_t b[512];
    swprintf(b, 512, T(msg), attente ? nom_bouton_switch(g.capture_pad).c_str() : L"");
    texte_lignes(b, F_PETIT, PX0 + 164, PY1 - 44, PX1 - PX0 - 188, attente ? AMBRE : GRIS);
}

// Case d'une touche : un clic, puis la touche voulue (lanceur.cpp : Z_TOUCHE0 + action, g.capture, WM_KEYDOWN).
void case_touche(float x0, float x1, float y, int action, const wchar_t* prefixe) {
    const int id = Z_TOUCHE0 + action;
    const bool attente = g.capture == action, sv = survol(id);
    rect(x0, y, x1, y + 20, attente ? VERT : NOIR, attente ? 0.25f : 0.55f);
    cadre(x0, y, x1, y + 20, VERT, attente ? 1.f : sv ? 0.85f : 0.3f);
    std::wstring s = attente ? L"…" : nom_touche(g.reglages.touches[action]);
    if (prefixe && !attente) s = std::wstring(prefixe) + L"  " + s;
    texte(s, F_PETIT, (x0 + x1) / 2, y + 3, attente || sv ? BLANC : VERT_PALE, 1.f, 0.f, 1, x1 - x0 - 6);
    zone(D2D1::RectF(x0, y, x1, y + 20), id);
}

void page_controles() {
    panneau(T("ctl_titre"));
    onglets_controles();
    if (g.onglet_manette) {
        page_manette();
        return;
    }
    float y = PY0 + 54;
    // préréglage du clavier : WASD (QWERTY) ou ZQSD (AZERTY) ; « personnalisées » dès qu'une touche a été changée
    libelle(y, "ctl_disposition", nullptr);
    const int p = touches_prereglage_courant();
    selecteur(y, p == 0 ? L"WASD  (QWERTY)" : p == 1 ? L"ZQSD  (AZERTY)" : T("ctl_perso"), Z_PRESET_G, Z_PRESET_D);
    y += 38;
    const float c0 = PX0 + 24, c1 = PX0 + 122, c1f = PX0 + 372, c2 = PX0 + 386;
    texte(T("ctl_bouton"), F_PETIT, c0, y, VERT, 0.9f, 2.f);
    texte(T("ctl_clavier"), F_PETIT, c1, y, VERT, 0.9f, 2.f);
    texte(T("ctl_manette"), F_PETIT, c2, y, VERT, 0.9f, 2.f);
    y += 20;
    const std::wstring ou = g.langue == Langue::Fr ? L"ou  " : L"or  ";
    struct Ligne { std::wstring bouton; std::vector<int> actions; std::wstring manette; unsigned confort; };
    const std::vector<Ligne> L = {
        {L"A", {K_A}, L"B  /  ○", 0x1C},
        {L"B", {K_B}, L"A  /  ✕", 0x0E},
        {L"X", {K_X}, L"Y  /  △", 0},
        {L"Y", {K_Y}, L"X  /  □", 0},
        {L"L", {K_L}, L"LB  /  L1", 0},
        {L"R", {K_R}, L"RB  /  R1", 0},
        {L"ZL", {K_ZL}, L"LT  /  L2", 0},
        {L"ZR", {K_ZR}, L"RT  /  R2", 0},
        {L"+", {K_PLUS}, L"Start  /  Options", 0x01},
        {L"−", {K_MINUS}, L"Back  /  Share", 0x0F},
        {T("stick_g"), {K_LS_UP, K_LS_LEFT, K_LS_DOWN, K_LS_RIGHT, K_LSTICK}, T("stick_g"), 0},
        {T("stick_d"), {K_RS_UP, K_RS_LEFT, K_RS_DOWN, K_RS_RIGHT, K_RSTICK}, T("stick_d"), 0},
        {T("croix"), {K_UP, K_LEFT, K_DOWN, K_RIGHT}, T("croix"), 0},
    };
    static const wchar_t* const sens[5] = {L"▲", L"◀", L"▼", L"▶", L"●"};
    const float pas = 23, esp = 4, lc = (c1f - c1 - 4 * esp) / 5;
    for (size_t i = 0; i < L.size(); i++) {
        if (i % 2 == 0) rect(c0 - 8, y - 2, PX1 - 24, y + 21, VERT, 0.035f);
        texte(L[i].bouton, F_GRAS, c0, y + 1, BLANC, 1.f, 0.f, 0, c1 - c0 - 6);
        if (L[i].actions.size() == 1) {
            case_touche(c1, c1 + 2 * lc + esp, y, L[i].actions[0], nullptr);
            // touche de confort des menus (Entrée, Retour arrière, Échap, Tab), tant qu'elle n'est donnée à aucune action
            bool prise = false;
            for (unsigned t : g.reglages.touches) prise = prise || t == L[i].confort;
            if (L[i].confort && !prise)
                texte(ou + nom_touche(L[i].confort), F_PETIT, c1 + 2 * lc + esp + 10, y + 3, GRIS, 1.f, 0.f, 0, c1f - (c1 + 2 * lc + esp + 10));
        } else {
            for (size_t k = 0; k < L[i].actions.size(); k++)
                case_touche(c1 + k * (lc + esp), c1 + k * (lc + esp) + lc, y, L[i].actions[k], sens[k]);
        }
        // bouton de la manette : réglé dans l'onglet Manette (A … − : une entrée ; sticks et croix : le texte fixe)
        const std::wstring m = L[i].actions.size() == 1 ? nom_entree(g.reglages.manette[L[i].actions[0]], 0) : L[i].manette;
        texte(m, F_CORPS, c2, y + 1, TEXTE, 1.f, 0.f, 0, PX1 - 24 - c2);
        y += pas;
    }
    texte_lignes(T(g.capture >= 0 ? "ctl_attente" : "ctl_note"), F_PETIT, c0, PY1 - 52, PX1 - PX0 - 48, g.capture >= 0 ? VERT : GRIS);
}

std::wstring duree_txt(ULONGLONG ms) {
    const unsigned s = (unsigned)(ms / 1000);
    wchar_t b[32];
    if (s >= 3600) swprintf(b, 32, L"%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else swprintf(b, 32, L"%u:%02u", s / 60, s % 60);
    return b;
}

void page_construction(float t) {
    panneau(T("cons_titre"));
    std::lock_guard<std::mutex> l(g.cons.lk);
    Construction& c = g.cons;
    float y = PY0 + 58;
    if (c.etapes.empty()) {
        texte_lignes(T("cons_attente"), F_CORPS, PX0 + 24, y, PX1 - PX0 - 48, GRIS);
    }
    for (size_t i = 0; i < c.etapes.size(); i++) {
        const int s = c.statut[i];
        const std::string cle = "e_" + c.etapes[i];
        const float cx = PX0 + 32, cy = y + 10;
        if (s == 2 || s == 3) {   // coche
            ligne(cx - 6, cy, cx - 2, cy + 5, VERT, 1.f, 2.f);
            ligne(cx - 2, cy + 5, cx + 7, cy - 5, VERT, 1.f, 2.f);
        } else if (s == 1) {   // arc qui tourne
            const float a0 = t * 5.f;
            for (int k = 0; k < 8; k++) {
                const float a = a0 + k * 0.785f;
                rect(cx + 7 * cosf(a) - 1.5f, cy + 7 * sinf(a) - 1.5f, cx + 7 * cosf(a) + 1.5f, cy + 7 * sinf(a) + 1.5f, VERT,
                     0.15f + 0.1f * k);
            }
        } else {
            g_dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), 5, 5), pinceau(GRIS, 0.8f), 1.f);
        }
        const uint32_t col = s == 1 ? VERT : s >= 2 ? VERT_PALE : GRIS;
        texte(T(cle.c_str()), s == 1 ? F_GRAS : F_ETAPE, PX0 + 52, y, col, 1.f);
        if (s == 1) {
            wchar_t p[16];
            swprintf(p, 16, L"%d %%", (int)(std::clamp(c.progres, 0.0, 1.0) * 100));
            texte(p, F_ETAPE, PX1 - 30, y, VERT, 1.f, 0.f, 2);
            if (!c.info.empty()) texte(c.info, F_PETIT, PX1 - 90, y + 2, GRIS, 1.f, 0.f, 2, 220);
        } else if (s == 3) {
            texte(T("deja_fait"), F_PETIT, PX1 - 30, y + 2, GRIS, 1.f, 0.f, 2);
        }
        y += 28;
    }
    // barre globale
    const float by = PY0 + 268;
    if (!c.etapes.empty()) {
        const double p = c.finie && !c.echec && !c.annulee ? 1.0 : progres_total(c);
        rect(PX0 + 24, by, PX1 - 24, by + 6, VERT, 0.12f);
        ID2D1LinearGradientBrush* b = degrade(PX0 + 24, 0, PX1 - 24, 0, {{0.f, C(VERT_FONCE)}, {1.f, C(VERT)}});
        if (b) g_dc->FillRectangle(D2D1::RectF(PX0 + 24, by, PX0 + 24 + (float)((PX1 - PX0 - 48) * p), by + 6), b), rel(b);
        const ULONGLONG fin = c.fin ? c.fin : GetTickCount64();
        texte(duree_txt(c.debut ? fin - c.debut : 0) + L" " + T("cons_ecoule"), F_PETIT, PX0 + 24, by + 12, GRIS, 1.f, 1.f);
        wchar_t s[16];
        swprintf(s, 16, L"%d %%", (int)(p * 100));
        texte(s, F_PETIT, PX1 - 24, by + 12, VERT, 1.f, 1.f, 2);
    }
    // résultat
    float ry = by + 36;
    if (c.finie) {
        if (c.annulee) texte(T("cons_annulee"), F_CORPS, PX0 + 24, ry, AMBRE);
        else if (c.echec) texte_lignes(std::wstring(T("cons_echec")) + c.erreur, F_CORPS, PX0 + 24, ry, PX1 - PX0 - 48, AMBRE);
        else texte(T(c.tache == Tache::Deplacement ? "dep_cons_fini" : "cons_fini"), F_GRAS, PX0 + 24, ry, VERT);
    } else if (c.active && c.courante >= 0 && c.courante < (int)c.etapes.size() && c.etapes[c.courante] == "pipelines" &&
               c.info == L"jeu") {   // relevé des shaders créés par le jeu (preparer.py, releve_shaders)
        texte_lignes(T("cons_info_releve"), F_CORPS, PX0 + 24, ry, PX1 - PX0 - 48, VERT_PALE);
    }
    // journal : dernières lignes
    const float jy0 = PY0 + 352, jy1 = PY1 - 56;
    rect(PX0 + 24, jy0, PX1 - 24, jy1, NOIR, 0.7f);
    cadre(PX0 + 24, jy0, PX1 - 24, jy1, VERT, 0.14f);
    const int n = (int)c.journal.size(), nmax = (int)((jy1 - jy0 - 10) / 15);
    float ly = jy0 + 6;
    for (int i = std::max(0, n - nmax); i < n; i++, ly += 15)
        texte(c.journal[i], F_MONO, PX0 + 32, ly, 0x7FBF95, 0.9f, 0.f, 0, PX1 - PX0 - 64);
    // boutons
    if (c.active) bouton_contour(PX1 - 160, PY1 - 44, PX1 - 24, PY1 - 16, T("annuler"), Z_ANNULER);
    else bouton_contour(PX1 - 160, PY1 - 44, PX1 - 24, PY1 - 16, T("fermer"), Z_FERMER_PAGE);
    if (!c.etapes.empty()) bouton_contour(PX0 + 24, PY1 - 44, PX0 + 160, PY1 - 16, T("journal"), Z_JOURNAL);
    // paquet installé, rien en cours : tout retirer du PC (desinstaller.py, après deux questions)
    if (g.paquet && !c.active && !g.jeu_lance)
        bouton_contour(PX0 + 172, PY1 - 44, PX1 - 172, PY1 - 16, T("desinstaller"), Z_DESINSTALLER);
}

bool sauver_png(const wchar_t* chemin);

}  // namespace

// ================================================================ interface
bool dessin_init(HWND hwnd) {
    g_hwnd = hwnd;
    // Tout échec d'affichage laisse une ligne dans lanceur.log : l'issue GitHub n° 12 (fenêtre noire) est arrivée sans
    // aucune trace.
    auto echec = [](const wchar_t* etape, HRESULT hr) {
        jlog(L"display: %ls failed (0x%08lx)", etape, (unsigned long)hr);
        return false;
    };
    const D3D_FEATURE_LEVEL niveaux[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL niveau = D3D_FEATURE_LEVEL_10_0;
    bool logiciel = false;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, niveaux, 4,
                                   D3D11_SDK_VERSION, &g_d3d, &niveau, &g_d3dctx);
    if (FAILED(hr)) {
        jlog(L"display: no hardware Direct3D 11 device (0x%08lx), software rendering", (unsigned long)hr);
        logiciel = true;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, niveaux, 4, D3D11_SDK_VERSION,
                               &g_d3d, &niveau, &g_d3dctx);
    }
    if (FAILED(hr)) return echec(L"D3D11CreateDevice", hr);
    IDXGIDevice* dxdev = nullptr;
    if (FAILED(hr = g_d3d->QueryInterface(IID_PPV_ARGS(&dxdev)))) return echec(L"IDXGIDevice", hr);
    D2D1_FACTORY_OPTIONS opt{};
    if (FAILED(hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opt, (void**)&g_fab)))
        return echec(L"D2D1CreateFactory", hr);
    if (FAILED(hr = g_fab->CreateDevice(dxdev, &g_dev))) return echec(L"ID2D1Factory1::CreateDevice", hr);
    if (FAILED(hr = g_dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc))) return echec(L"CreateDeviceContext", hr);
    IDXGIAdapter* ad = nullptr;
    if (FAILED(hr = dxdev->GetAdapter(&ad))) return echec(L"IDXGIDevice::GetAdapter", hr);
    DXGI_ADAPTER_DESC carte{};
    ad->GetDesc(&carte);
    IDXGIFactory2* f2 = nullptr;
    if (FAILED(hr = ad->GetParent(IID_PPV_ARGS(&f2)))) return echec(L"IDXGIFactory2", hr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    hr = f2->CreateSwapChainForHwnd(g_d3d, hwnd, &sd, nullptr, nullptr, &g_chaine);
    if (SUCCEEDED(hr)) f2->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    rel(f2), rel(ad), rel(dxdev);
    if (FAILED(hr)) return echec(L"CreateSwapChainForHwnd", hr);
    if (!cible_creer()) return echec(L"swap chain target", E_FAIL);
    // Une image sur n balayages de l'écran : environ 60 images par seconde quel que soit l'écran. Present n'accepte
    // que n = 1 à 4 : au-delà (écran de 270 Hz et plus), il échouait à chaque image (DXGI_ERROR_INVALID_CALL) et la
    // fenêtre restait noire (issue GitHub n° 12, reproduite avec LANCEUR_HZ=300 et 360). D'où la borne : 90 images
    // par seconde à 360 Hz, 120 à 480 Hz. LANCEUR_HZ : fréquence d'écran simulée (vérification).
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    DWORD hz = 0;
    if (const wchar_t* e = _wgetenv(L"LANCEUR_HZ")) hz = (DWORD)_wtoi(e);
    else if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) hz = dm.dmDisplayFrequency;
    g_sync = hz > 1 ? std::clamp<UINT>((UINT)lround(hz / 60.0), 1, 4) : 1;
    jlog(L"display: %ls%ls, Direct3D %u.%u, screen %lu Hz, one frame every %u refresh(es)", carte.Description,
         logiciel ? L" (software)" : L"", (unsigned)niveau >> 12, ((unsigned)niveau >> 8) & 15, (unsigned long)hz, g_sync);
    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory1), (IUnknown**)&g_dw)))
        return echec(L"DWriteCreateFactory", hr);
    const wchar_t* B = L"Bahnschrift";
    format(F_TITRE, B, 66, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STRETCH_NORMAL);
    format(F_SOUS, B, 22, DWRITE_FONT_WEIGHT_LIGHT, DWRITE_FONT_STRETCH_NORMAL);
    format(F_VERSION, B, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL);
    format(F_BOUTON, B, 23, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_NORMAL);
    format(F_MENU, B, 19, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_NORMAL);
    format(F_PETIT, B, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL);
    format(F_CORPS, B, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL);
    format(F_BANDEAU, B, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_NORMAL);
    format(F_PANNEAU, B, 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_NORMAL);
    format(F_VALEUR, B, 14, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL);
    format(F_MONO, L"Cascadia Mono", 10.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL);
    format(F_ETAPE, B, 13.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL);
    format(F_GRAS, B, 13.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STRETCH_NORMAL);
    g_dc->CreateSolidColorBrush(C(VERT), &g_br);
    if (FAILED(hr = g_dc->CreateEffect(CLSID_MATRICE, &g_fx_couleur))) return echec(L"CreateEffect (color matrix)", hr);
    if (FAILED(hr = g_dc->CreateEffect(CLSID_FLOU, &g_fx_flou))) return echec(L"CreateEffect (blur)", hr);
    matrice_couleur();
    const float ecart = 9.f;
    g_fx_flou->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, D2D1_PROPERTY_TYPE_FLOAT, (const BYTE*)&ecart, sizeof ecart);
    trame_creer();
    return true;
}

void dessin_redim() {
    if (!g_chaine) return;
    g_dc->SetTarget(nullptr);
    rel(g_cible);
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    const HRESULT hr = g_chaine->ResizeBuffers(0, (UINT)std::max<LONG>(1, rc.right), (UINT)std::max<LONG>(1, rc.bottom),
                                               DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) jlog(L"display: ResizeBuffers failed (0x%08lx)", (unsigned long)hr);
    if (!cible_creer()) jlog(L"display: swap chain target not recreated, nothing will be drawn");
    trame_creer();
    vider_cache();
}

int zone_sous(float x, float y) {
    for (auto it = g_zones_prec.rbegin(); it != g_zones_prec.rend(); ++it)
        if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return it->id;
    return Z_AUCUNE;
}

void dessin_image() {
    if (!g_dc || !g_cible) return;
    const float t = (GetTickCount64() - g.t0) / 1000.f;
    g_zones.clear();
    titre_lueur();   // avant BeginDraw : il dessine dans sa propre image (imbriqué, il faisait échouer la première image)
    g_dc->BeginDraw();
    g_dc->SetTransform(D2D1::IdentityMatrix());
    g_dc->Clear(C(NOIR));
    if (g.fond_video) fond_video(t);
    else g_video_ok = false;
    if (!g_video_ok) globe(t);
    voiles();
    grille(t);
    particules(t);
    titre(t);
    bouton_principal(t);
    menu();   // plein écran / fenêtré : page Graphismes (plus de bascule sur l'accueil depuis le 2026-11-12)
    bloc_copie();
    if (g.page == Page::Reglages) page_reglages();
    else if (g.page == Page::Controles) page_controles();
    else if (g.page == Page::Construction) page_construction(t);
    else if (g.page == Page::Graphismes) page_graphismes();
    bandeau(t);
    barre_haut();
    cadre(0, 0, LARGEUR, HAUTEUR, VERT, 0.45f);
    HRESULT hr = g_dc->EndDraw();
    g_zones_prec.swap(g_zones);
    if (hr == D2DERR_RECREATE_TARGET) {
        dessin_redim();
        return;
    }
    static HRESULT dessin_avant = S_OK;   // un échec de dessin ou de présentation : une ligne dans lanceur.log
    if (FAILED(hr) && hr != dessin_avant) jlog(L"display: drawing failed (EndDraw 0x%08lx)", (unsigned long)hr);
    dessin_avant = hr;
    static const wchar_t* capture = _wgetenv(L"LANCEUR_CAPTURE");   // vérification : image PNG après n secondes
    static const float capture_t = _wgetenv(L"LANCEUR_CAPTURE_T") ? (float)_wtof(_wgetenv(L"LANCEUR_CAPTURE_T")) : 3.f;
    static bool capture_faite = false;
    if (capture && !capture_faite && t >= capture_t) {
        capture_faite = true;
        sauver_png(capture);
        if (_wgetenv(L"LANCEUR_CAPTURE_QUITTER")) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
    }
    const HRESULT hp = g_chaine->Present(g_sync, 0);
    static HRESULT present_avant = S_OK;
    if (FAILED(hp)) {   // rien n'est arrivé à l'écran
        if (hp != present_avant) jlog(L"display: Present failed (0x%08lx, every %u refreshes)", (unsigned long)hp, g_sync);
        g_sync = 1;     // l'intervalle que tout écran accepte
        Sleep(15);      // Present n'a pas attendu l'écran : pas de boucle à vide
    }
    present_avant = hp;
}

void dessin_liberer() {
    vider_cache();
    for (int i = 0; i < F_NB; i++) rel(g_fmt[i]), rel(g_ellipse[i]);
    rel(g_trame), rel(g_bm_titre), rel(g_bm_video), rel(g_fx_flou), rel(g_fx_couleur), rel(g_br);
    for (auto& c : g_chemins) rel(c.second);   // dessins des manettes (onglet Manette)
    g_chemins.clear();
    rel(g_dw), rel(g_cible), rel(g_dc), rel(g_dev), rel(g_fab), rel(g_chaine), rel(g_d3dctx), rel(g_d3d);
}

namespace {
// copie de l'image affichée en PNG (WIC) : vérification du rendu sans écran (LANCEUR_CAPTURE)
bool sauver_png(const wchar_t* chemin) {
    ID3D11Texture2D* tampon = nullptr;
    if (FAILED(g_chaine->GetBuffer(0, IID_PPV_ARGS(&tampon)))) return false;
    D3D11_TEXTURE2D_DESC d;
    tampon->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MiscFlags = 0;
    ID3D11Texture2D* copie = nullptr;
    bool ok = false;
    if (SUCCEEDED(g_d3d->CreateTexture2D(&d, nullptr, &copie))) {
        g_d3dctx->CopyResource(copie, tampon);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_d3dctx->Map(copie, 0, D3D11_MAP_READ, 0, &m))) {
            IWICImagingFactory* wic = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
                IWICStream* st = nullptr;
                IWICBitmapEncoder* enc = nullptr;
                IWICBitmapFrameEncode* fr = nullptr;
                if (SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(chemin, GENERIC_WRITE)) &&
                    SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                    SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
                    SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->SetSize(d.Width, d.Height))) {
                    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
                    fr->SetPixelFormat(&pf);
                    std::vector<uint8_t> px((size_t)d.Width * d.Height * 4);
                    for (UINT y = 0; y < d.Height; y++) {
                        memcpy(px.data() + (size_t)y * d.Width * 4, (const uint8_t*)m.pData + (size_t)y * m.RowPitch, d.Width * 4);
                        for (UINT x = 0; x < d.Width; x++) px[((size_t)y * d.Width + x) * 4 + 3] = 255;
                    }
                    ok = SUCCEEDED(fr->WritePixels(d.Height, d.Width * 4, (UINT)px.size(), px.data())) && SUCCEEDED(fr->Commit()) &&
                         SUCCEEDED(enc->Commit());
                }
                rel(fr), rel(enc), rel(st), rel(wic);
            }
            g_d3dctx->Unmap(copie, 0);
        }
        rel(copie);
    }
    rel(tampon);
    return ok;
}
}  // namespace

}  // namespace lz
