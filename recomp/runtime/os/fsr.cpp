// FSR d'AMD (2026-11-12) : anticrénelage natif et super résolution par l'API FSR du SDK FidelityFX 2.4 (FSR 4.1,
// réseau de neurones, sur les cartes AMD qui le proposent ; FSR 3.1 sur toutes les autres, NVIDIA et Intel compris).
//
//  * bibliothèques d'AMD, signées, chargées dynamiquement : amd_fidelityfx_loader_vk.dll (petit chargeur) et
//    amd_fidelityfx_upscaler_vk.dll (FSR 4, 3.1 et 2), cherchées dans RECOMP_FSR_DIR, à côté de l'exécutable, dans son
//    sous-dossier fsr, puis dans les outils du paquet (<racine>\outils\fsr) et des sources (<racine>\recomp\outils\fsr) ;
//  * en-têtes de l'API (licence MIT) : runtime/third_party/ffx, tels que le SDK les fournit ;
//  * version : sans rien forcer, l'API prend la plus récente que la carte accepte, y compris une version plus récente
//    apportée par le pilote d'AMD ; recomp.ini fsr_version = fsr3 ou fsr4 en impose une (RECOMP_FSR_VERSION) ;
//  * branchement : celui du DLSS (vk_render.cpp, dlss_taa et dlss_agrandir), par les fonctions de dlss.h : mêmes
//    images (couleur RVB compressée, profondeur matérielle inversée, vecteurs de mouvement en pixels, gigue).
// Si quelque chose manque ou échoue, le jeu garde son propre anticrénelage et le journal dit pourquoi (« fsr »).
// Netteté : recomp.ini fsr_sharpness, de 0 (aucune, par défaut) à 100 (passe RCAS du FSR).
// Essais : RECOMP_FSR_LINEAR=1 (couleur déclarée linéaire), RECOMP_FSR_DEBUG=1 (vue de contrôle du FSR dans l'image,
// messages détaillés).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "hos.h"   // avant <windows.h>, comme settings.cpp (GPTR)
#include "settings.h"

#include <windows.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "fsr.h"

#include "ffx/upscalers/include/ffx_upscale.h"
#include "ffx/api/include/vk/ffx_api_vk.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace hos {
namespace dlss {
bool dlss_prevu();   // dlss.cpp : le DLSS de NVIDIA servira sans doute (demandé, carte NVIDIA RTX, cœur NGX trouvé)
}
namespace fsr {
namespace {

struct Etat {
    bool cherche = false, tente = false;
    std::wstring chemin;   // amd_fidelityfx_loader_vk.dll
    HMODULE dll = nullptr;
    PfnFfxCreateContext creer = nullptr;
    PfnFfxDestroyContext detruire = nullptr;
    PfnFfxQuery interroger = nullptr;
    PfnFfxDispatch executer = nullptr;
    bool pret = false;
    VkDeviceContext dc{};
    ffxContext ctx = nullptr;
    // contextes remplacés, détruits quand le GPU a fini la soumission indiquée (comme les fonctions du DLSS)
    std::vector<std::pair<uint64_t, ffxContext>> a_liberer;
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    bool echec_creation = false;
    uint64_t version_id = 0;   // version imposée (fsr_version), 0 : choix de l'API
    std::string version_nom;
    std::string versions;      // versions offertes pour cette carte (journal)
    LARGE_INTEGER dernier{};   // horloge de la dernière évaluation (durée de l'image)
    bool camera_dite = false;
    int sans_camera = 0;
    char desc[160] = "off";
};
Etat g;

std::string utf8(const std::wstring& w) {
    if (w.empty()) return "";
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool existe(const std::wstring& f) { return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Chargeur d'AMD, avec amd_fidelityfx_upscaler_vk.dll dans le même dossier (le chargeur le prend là, ou à côté de
// l'exécutable). L'exécutable est dans <racine>\recomp\build\full.
const std::wstring& chercher() {
    if (g.cherche) return g.chemin;
    g.cherche = true;
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring racine = dir;
    for (int i = 0; i < 3 && racine.find_last_of(L"\\/") != std::wstring::npos; i++)
        racine = racine.substr(0, racine.find_last_of(L"\\/"));
    std::vector<std::wstring> dossiers;
    wchar_t env[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"RECOMP_FSR_DIR", env, MAX_PATH) > 0) dossiers.push_back(env);
    dossiers.push_back(dir);
    dossiers.push_back(dir + L"\\fsr");
    dossiers.push_back(racine + L"\\outils\\fsr");
    dossiers.push_back(racine + L"\\recomp\\outils\\fsr");
    for (const std::wstring& d : dossiers)
        if (existe(d + L"\\amd_fidelityfx_loader_vk.dll") &&
            (existe(d + L"\\amd_fidelityfx_upscaler_vk.dll") || existe(dir + L"\\amd_fidelityfx_upscaler_vk.dll"))) {
            g.chemin = d + L"\\amd_fidelityfx_loader_vk.dll";
            break;
        }
    return g.chemin;
}

bool charger() {
    if (g.tente) return g.dll != nullptr;
    g.tente = true;
    const std::wstring& chemin = chercher();
    if (chemin.empty()) {
        HLOG("fsr", "AMD FSR files not found (amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll, next to "
                    "the game or in outils\\fsr): FSR off, the game keeps its anti-aliasing");
        return false;
    }
    HMODULE m = LoadLibraryExW(chemin.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        HLOG("fsr", "%s refused at load time (error %lu): FSR off", utf8(chemin).c_str(), GetLastError());
        return false;
    }
    g.creer = (PfnFfxCreateContext)(void*)GetProcAddress(m, "ffxCreateContext");
    g.detruire = (PfnFfxDestroyContext)(void*)GetProcAddress(m, "ffxDestroyContext");
    g.interroger = (PfnFfxQuery)(void*)GetProcAddress(m, "ffxQuery");
    g.executer = (PfnFfxDispatch)(void*)GetProcAddress(m, "ffxDispatch");
    if (!g.creer || !g.detruire || !g.interroger || !g.executer) {
        HLOG("fsr", "%s: FSR API functions missing: FSR off", utf8(chemin).c_str());
        return false;
    }
    g.dll = m;
    return true;
}

void message_fsr(uint32_t type, const wchar_t* message) {
    static int n = 0;
    if (n++ >= 24) return;
    HLOG("fsr", "FSR %s: %s", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", utf8(message ? message : L"").c_str());
}

// Numéro majeur d'un nom de version (« 4.1.2 », « FSR 3.1.6 », « 4.1.2 * » pour une version du pilote)
int majeur(const char* nom) {
    while (nom && *nom && (*nom < '0' || *nom > '9')) nom++;
    return nom && *nom ? atoi(nom) : 0;
}

// Versions offertes pour cette carte ; fsr_version = fsr3 ou fsr4 : la première de ce numéro est imposée.
void choisir_version() {
    uint64_t n = 0;
    ffxQueryDescGetVersions q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = &g.dc;
    q.outputCount = &n;
    ffxReturnCode_t r = g.interroger(nullptr, &q.header);
    if (r != FFX_API_RETURN_OK || !n) {
        HLOG("fsr", "version list not available (code %u): the FSR API will choose", r);
        return;
    }
    std::vector<uint64_t> ids(n);
    std::vector<const char*> noms(n, nullptr);
    q.versionIds = ids.data();
    q.versionNames = noms.data();
    r = g.interroger(nullptr, &q.header);
    if (r != FFX_API_RETURN_OK) return;
    const int voulu = settings().fsr_version;   // 0 auto, 3, 4
    for (uint64_t i = 0; i < n; i++) {
        const char* nom = noms[i] ? noms[i] : "?";
        g.versions += (g.versions.empty() ? "" : ", ") + std::string(nom);
        if (voulu && !g.version_id && majeur(nom) == voulu) g.version_id = ids[i], g.version_nom = nom;
    }
    HLOG("fsr", "FSR versions offered for this graphics card: %s", g.versions.c_str());
    if (voulu && !g.version_id)
        HLOG("fsr", "FSR %d asked (fsr_version) but not offered for this graphics card: automatic choice", voulu);
    else if (voulu)
        HLOG("fsr", "FSR %s imposed by fsr_version", g.version_nom.c_str());
}

FfxApiResource ressource(const dlss::Image& im, uint32_t etat, bool ecrite) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im.format;
    ci.extent = {im.w, im.h, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | (ecrite ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    return ffxApiGetResourceVK((void*)im.image, ffxApiGetImageResourceDescriptionVK(im.image, ci, 0), etat);
}

// Contexte du FSR pour ces tailles. `infini` : plan lointain à l'infini (profondeur inversée dans tous les cas).
bool creer_contexte(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh, bool infini, bool non_lineaire) {
    static const bool debug = getenv("RECOMP_FSR_DEBUG") != nullptr;
    ffxCreateBackendVKDesc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    be.deviceContext = g.dc;
    ffxCreateContextDescUpscale cu{};
    cu.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cu.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | (infini ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0) |
               (non_lineaire ? FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE : 0) |
               (debug ? FFX_UPSCALE_ENABLE_DEBUG_CHECKING | FFX_UPSCALE_ENABLE_DEBUG_VISUALIZATION : 0);
    cu.maxRenderSize = {rw, rh};
    cu.maxUpscaleSize = {ow, oh};
    cu.fpMessage = message_fsr;
    ffxCreateContextDescUpscaleVersion ver{};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    ov.versionId = g.version_id;
    // essais : version imposée puis choix de l'API, avec puis sans la version de l'en-tête (fournisseur plus ancien)
    ffxReturnCode_t r = FFX_API_RETURN_ERROR;
    ffxContext ctx = nullptr;
    for (int essai = 0; essai < 4 && !ctx; essai++) {
        const bool impose = !(essai & 1) && g.version_id, avec_ver = essai < 2;
        if ((essai & 1) == 0 && !g.version_id) continue;   // rien à imposer : essais 1 et 3 seulement
        cu.header.pNext = &be.header;
        be.header.pNext = avec_ver ? &ver.header : (impose ? &ov.header : nullptr);
        ver.header.pNext = impose ? &ov.header : nullptr;
        ov.header.pNext = nullptr;
        ctx = nullptr;
        r = g.creer(&ctx, &cu.header, nullptr);
        if (r != FFX_API_RETURN_OK) {
            ctx = nullptr;
            HLOG("fsr", "FSR context refused (code %u%s%s)", r, impose ? ", imposed version" : ", automatic version",
                 avec_ver ? "" : ", without the header version");
        }
    }
    if (!ctx) {
        HLOG("fsr", "FSR creation refused for %ux%u -> %ux%u (code %u): the game's anti-aliasing stays", rw, rh, ow, oh, r);
        return false;
    }
    g.ctx = ctx;
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    const char* nom = "?";
    if (g.interroger(&g.ctx, &pv.header) == FFX_API_RETURN_OK && pv.versionName) nom = pv.versionName;
    FfxApiEffectMemoryUsage mem{};
    ffxQueryDescUpscaleGetGPUMemoryUsage mu{};
    mu.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GPU_MEMORY_USAGE;
    mu.gpuMemoryUsageUpscaler = &mem;
    g.interroger(&g.ctx, &mu.header);
    static const char* const modes[] = {"", "Native AA", "Quality", "Balanced", "Performance", "Ultra Performance"};
    const float rapport = (float)rw / (float)ow;
    const int mode = rw == ow && rh == oh ? 1 : rapport >= 0.63f ? 2 : rapport >= 0.55f ? 3 : rapport >= 0.42f ? 4 : 5;
    snprintf(g.desc, sizeof g.desc, "AMD FSR %s, %s %ux%u -> %ux%u", nom, modes[mode], rw, rh, ow, oh);
    HLOG("fsr", "created: %s (%.0f MB of video memory, far plane %s)", g.desc, (double)mem.totalUsageInBytes / 1048576.0,
         infini ? "at infinity" : "finite");
    return true;
}

}  // namespace

int mode_demande() {
    static const int m = [] {
        const int mode = settings().fsr;
        if (mode < 1 || mode > 5 || dlss::dlss_prevu()) return 0;
        if (chercher().empty()) {
            charger();   // le dit dans le journal
            return 0;
        }
        return mode;
    }();
    return m;
}

double echelle() {
    static const double echelles[] = {0, 0, 1.0 / 1.5, 1.0 / 1.7, 0.5, 1.0 / 3.0};
    return echelles[mode_demande()];
}

bool prevu() { return mode_demande() != 0; }

bool init(VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
    if (!mode_demande() || !charger()) return false;
    g.dc.vkDevice = dev;
    g.dc.vkPhysicalDevice = pd;
    g.dc.vkDeviceProcAddr = gdpa;
    choisir_version();
    g.pret = true;
    snprintf(g.desc, sizeof g.desc, "AMD FSR");
    HLOG("fsr", "AMD FSR ready (%s): it replaces the game's temporal anti-aliasing (taa_resolve)", utf8(g.chemin).c_str());
    return true;
}

bool pret() { return g.pret; }

void couper(const char* raison) {
    if (!g.pret) return;
    g.pret = false;
    HLOG("fsr", "FSR stopped for this session (%s): the game's anti-aliasing is back", raison);
}

const char* description() { return g.desc; }

bool fonction_prete(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh) {
    return g.ctx && g.rw == rw && g.rh == rh && g.ow == ow && g.oh == oh;
}

void menage(uint64_t fini) {
    for (size_t i = 0; i < g.a_liberer.size();) {
        if (g.a_liberer[i].first > fini) {
            i++;
            continue;
        }
        g.detruire(&g.a_liberer[i].second, nullptr);
        g.a_liberer.erase(g.a_liberer.begin() + (std::ptrdiff_t)i);
    }
}

bool evaluer(VkCommandBuffer cb, const dlss::Evaluation& e, uint64_t serie) {
    if (!g.pret) return false;
    static const bool lineaire = getenv("RECOMP_FSR_LINEAR") != nullptr;
    static const bool debug = getenv("RECOMP_FSR_DEBUG") != nullptr;
    static const float nettete = (float)settings().fsr_sharpness / 100.0f;   // recomp.ini fsr_sharpness, RECOMP_FSR_SHARPNESS
    const uint32_t ow = e.sortie.w, oh = e.sortie.h;
    if (e.rw < 32 || e.rh < 32 || ow < 32 || oh < 32) return false;
    // plans de la caméra : ceux de la matrice de la vue (vk_render.cpp, dlss_plans_camera), sinon des valeurs neutres
    const bool camera = e.proche > 0 && std::isfinite(e.proche) && e.fov > 0.01f && e.fov < 3.1f;
    const float proche = camera ? e.proche : 0.1f;
    const bool infini = !camera || !(e.loin > proche) || !std::isfinite(e.loin);
    const float loin = infini ? 1e7f : e.loin;
    const bool non_lineaire = e.compression == 2 && !lineaire;   // racine carrée : couleur perceptuelle
    if (!g.ctx || g.rw != e.rw || g.rh != e.rh || g.ow != ow || g.oh != oh) {
        const bool memes = g.rw == e.rw && g.rh == e.rh && g.ow == ow && g.oh == oh;
        if (g.echec_creation && memes) return false;
        if (g.ctx) {   // peut encore servir dans des commandes pas encore exécutées : détruit plus tard
            g.a_liberer.emplace_back(serie, g.ctx);
            g.ctx = nullptr;
        }
        g.rw = e.rw, g.rh = e.rh, g.ow = ow, g.oh = oh;
        g.echec_creation = !creer_contexte(e.rw, e.rh, ow, oh, infini, non_lineaire);
        if (g.echec_creation) return false;
        g.dernier.QuadPart = 0;
    }
    if (camera && !g.camera_dite) {
        g.camera_dite = true;
        HLOG("fsr", "camera: near plane %.4g, far plane %.4g%s, vertical field of view %.1f degrees", proche, loin,
             infini ? " (infinite)" : "", e.fov * 57.29578f);
    } else if (!camera && !g.camera_dite && ++g.sans_camera == 600) {
        HLOG("fsr", "camera planes not found in the view matrix after 600 frames (got near %.4g, far %.4g, field of view "
                    "%.4g): neutral values (near 0.1, far at infinity, 60 degrees)", e.proche, e.loin, e.fov);
    }
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    float dt = 16.7f;
    if (g.dernier.QuadPart) dt = (float)((double)(t.QuadPart - g.dernier.QuadPart) * 1000.0 / (double)f.QuadPart);
    g.dernier = t;
    dt = dt < 1.0f ? 1.0f : dt > 200.0f ? 200.0f : dt;

    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = (void*)cb;
    d.color = ressource(e.couleur, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    d.depth = ressource(e.profondeur, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    d.motionVectors = ressource(e.vecteurs, FFX_API_RESOURCE_STATE_COMPUTE_READ, false);
    d.output = ressource(e.sortie, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, true);
    d.jitterOffset = {e.gigue[0], e.gigue[1]};
    d.motionVectorScale = {e.echelle_mv[0], e.echelle_mv[1]};
    d.renderSize = {e.rw, e.rh};
    d.upscaleSize = {ow, oh};
    d.enableSharpening = nettete > 0;
    d.sharpness = nettete > 1 ? 1.0f : nettete;
    d.frameTimeDelta = dt;
    d.preExposure = 1.0f;
    d.reset = e.reset;
    d.cameraNear = proche;
    d.cameraFar = loin;
    d.cameraFovAngleVertical = camera ? e.fov : 1.0472f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = (non_lineaire ? FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB : 0) | (debug ? FFX_UPSCALE_FLAG_DRAW_DEBUG_VIEW : 0);
    const ffxReturnCode_t r = g.executer(&g.ctx, &d.header);
    if (r != FFX_API_RETURN_OK) {
        static int nlog = 0;
        if (nlog++ < 8) HLOG("fsr", "FSR evaluation failed (code %u)", r);
        return false;
    }
    return true;
}

}  // namespace fsr
}  // namespace hos
