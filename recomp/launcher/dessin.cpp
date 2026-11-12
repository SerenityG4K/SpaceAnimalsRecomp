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
        {"verifier", 1}, {"extraire", 2}, {"images", 2}, {"recompiler", 78}, {"lier", 3}, {"shaders", 9}, {"pipelines", 3}};
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
        swprintf(s, 96, L"%ls   %d %%", T(tache == Tache::Construction ? "en_cours" : tache == Tache::Shaders ? "shaders_en_cours" : "maj_en_cours"),
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

// plein écran ou fenêtré (recomp.ini du jeu), sous le bouton principal
void bascule_affichage() {
    const float y = 258, h = 22;
    texte(T("affichage"), F_PETIT, 44, y + 5, GRIS, 1.f, 2.5f);
    float x = 134;
    for (int k = 0; k < 2; k++) {
        const bool plein = k == 0, actif = g.reglages.sans_bordure == plein;
        const int z = plein ? Z_AFF_PLEIN : Z_AFF_FEN;
        IDWriteTextLayout* l = mise(T(plein ? "plein_maj" : "fenetre_maj"), F_PETIT, 2000.f, 1.5f);
        const float w = largeur_texte(l) + 22;
        const bool sv = survol(z);
        if (actif) rect(x, y, x + w, y + h, VERT, sv ? 1.f : 0.88f);
        else rect(x, y, x + w, y + h, NOIR, 0.55f), cadre(x, y, x + w, y + h, sv ? VERT : GRIS, sv ? 0.9f : 0.45f);
        if (l)
            g_dc->DrawTextLayout(D2D1::Point2F(x + 11, y + 4), l, pinceau(actif ? 0x04140A : sv ? BLANC : TEXTE, 1.f),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE);
        zone(D2D1::RectF(x, y, x + w, y + h), z);
        x += w + 6;
    }
}

void menu() {
    struct { int id; const char* cle; float y; Page p; } items[] = {
        {Z_MENU_REGLAGES, "reglages", 284, Page::Reglages},
        {Z_MENU_CONTROLES, "controles", 316, Page::Controles},
        {Z_MENU_CONSTRUCTION, "construction", 348, Page::Construction},
        {Z_MENU_RAPPORT, "rapport", 380, Page::Aucune},
        {Z_MENU_QUITTER, "quitter", 418, Page::Aucune},
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
        if (shaders) info = g.cons.tache == Tache::Shaders ? T("e_pipelines") : T("maj_en_cours");
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
void selecteur(float y, const wchar_t* valeur, int zg, int zd) {
    const float x0 = 652, x1 = 940, h = 28;
    const bool sv = survol(zg) || survol(zd);
    rect(x0, y, x1, y + h, NOIR, 0.65f);
    cadre(x0, y, x1, y + h, VERT, sv ? 0.75f : 0.32f);
    texte(L"‹", F_VALEUR, x0 + 14, y + 2, survol(zg) ? BLANC : VERT, 1.f, 0.f, 1);
    texte(L"›", F_VALEUR, x1 - 14, y + 2, survol(zd) ? BLANC : VERT, 1.f, 0.f, 1);
    texte(valeur, F_VALEUR, (x0 + x1) / 2, y + 4, BLANC, 1.f, 0.5f, 1, x1 - x0 - 70);
    zone(D2D1::RectF(x0, y, (x0 + x1) / 2, y + h), zg);
    zone(D2D1::RectF((x0 + x1) / 2, y, x1, y + h), zd);
}

void libelle(float y, const char* cle, const char* aide) {
    texte(T(cle), F_CORPS, PX0 + 24, y + 1, TEXTE, 1.f, 0.3f);
    if (aide) texte(T(aide), F_PETIT, PX0 + 24, y + 19, GRIS, 1.f, 0.f, 0, 190);
}

void bouton_contour(float x0, float y0, float x1, float y1, const wchar_t* s, int id, bool actif = true) {
    const bool sv = actif && survol(id);
    rect(x0, y0, x1, y1, sv ? VERT : NOIR, sv ? 0.16f : 0.5f);
    cadre(x0, y0, x1, y1, VERT, actif ? (sv ? 0.95f : 0.55f) : 0.18f);
    texte(s, F_PETIT, (x0 + x1) / 2, y0 + (y1 - y0 - 15) / 2, actif ? (sv ? BLANC : VERT) : GRIS, 1.f, 2.f, 1);
    if (actif) zone(D2D1::RectF(x0, y0, x1, y1), id);
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
    // résolution
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
    libelle(y, "reg_natif", "reg_natif_aide");
    selecteur(y, g.reglages.natif ? T("natif") : T("console"), Z_NATIF_G, Z_NATIF_D);
    y += pas;
    libelle(y, "reg_aff", "reg_aff_aide");
    selecteur(y, g.reglages.sans_bordure ? T("sans_bordure") : T("fenetre"), Z_AFF_G, Z_AFF_D);
    y += pas;
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
    texte(T("experts"), F_PETIT, PX0 + 24, y, VERT, 0.85f, 3.f);
    ligne(PX0 + 140, y + 8, PX1 - 24, y + 8, VERT, 0.15f);
    y += 20;
    libelle(y, "reg_fils", nullptr);
    if (g.fils == 0) swprintf(v, 64, T("auto_fils"), fils_effectifs());
    else swprintf(v, 64, L"%d", g.fils);
    selecteur(y, v, Z_FILS_G, Z_FILS_D);
    const bool libre = !g.cons.active && !g.jeu_lance;
    bouton_contour(PX0 + 24, PY1 - 46, PX0 + 262, PY1 - 18, T("reconstruire"), Z_RECONSTRUIRE, libre && g.copie_ok);
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
        texte(L[i].manette, F_CORPS, c2, y + 1, TEXTE, 1.f, 0.f, 0, PX1 - 24 - c2);
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
        else texte(T("cons_fini"), F_GRAS, PX0 + 24, ry, VERT);
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
    bascule_affichage();
    menu();
    bloc_copie();
    if (g.page == Page::Reglages) page_reglages();
    else if (g.page == Page::Controles) page_controles();
    else if (g.page == Page::Construction) page_construction(t);
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
