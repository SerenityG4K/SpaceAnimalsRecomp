// Moteur de rendu Vulkan de la HLE NVN.
//
//  * vulkan-1.dll est chargé dynamiquement (aucune bibliothèque d'import) ;
//  * mémoire : les pools NVN du jeu sont importés tels quels (VK_EXT_external_memory_host) ; une
//    adresse GPU NVN (= adresse CPU invitée) devient (VkBuffer du pool, décalage) ;
//  * textures : une VkImage par objet texture (cibles de rendu et textures échantillonnées) ; le
//    contenu des textures jamais écrites par le GPU n'est pas encore désentrelacé (gris neutre) ;
//  * shaders : SPIR-V produit hors ligne par shaders/shaderc.py (build/full/shader_cache/<hash>) ;
//  * listes de commandes rejouées dans un tampon de commandes par image, en rendu dynamique, avec un
//    cache de pipelines ; présentation : copie de la texture de fenêtre vers la swapchain.
// Activé par défaut ; RECOMP_NO_VULKAN=1 revient à la présentation GDI (couleur d'effacement).
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <direct.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include "gfx.h"
#include "nvn_exec.h"
#include "vk_render.h"
#include "record.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <thread>

namespace hos {
namespace vkr {
void flush_locked(const char* where);   // soumet et attend le GPU (défini plus bas)
namespace {

// ================================================================ fonctions Vulkan
#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE(X)                                                                                       \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures2) X(vkCreateDevice)                \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties) X(vkCreateWin32SurfaceKHR)               \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR)                    \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceSurfacePresentModesKHR)                          \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceMemoryProperties2)
#define VK_DEVICE(X)                                                                                        \
    X(vkGetDeviceQueue) X(vkCreateSwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR)            \
    X(vkQueuePresentKHR) X(vkDestroySwapchainKHR) X(vkCreateCommandPool) X(vkAllocateCommandBuffers)           \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences)         \
    X(vkResetFences) X(vkCreateSemaphore) X(vkResetCommandPool) X(vkCreateImage) X(vkGetImageMemoryRequirements) \
    X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer)                            \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkGetMemoryHostPointerPropertiesEXT) \
    X(vkCreateSampler) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateDescriptorPool)       \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateShaderModule)      \
    X(vkCreateGraphicsPipelines) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdPipelineBarrier2)          \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer)          \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdClearAttachments)          \
    X(vkCmdBlitImage) X(vkCmdCopyBufferToImage) X(vkCmdCopyImage) X(vkCmdClearColorImage)                     \
    X(vkCmdClearDepthStencilImage) X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask)                  \
    X(vkCmdSetStencilWriteMask) X(vkCmdSetBlendConstants) X(vkDeviceWaitIdle) X(vkCmdSetDepthBias)             \
    X(vkDestroyImageView) X(vkDestroyImage) X(vkFreeMemory) X(vkDestroySemaphore) X(vkDestroyFence) \
    X(vkDestroyBuffer) X(vkUnmapMemory) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBuffer) X(vkCreateComputePipelines) X(vkCmdDispatch)            \
    X(vkCmdSetCheckpointNV) X(vkGetQueueCheckpointDataNV) X(vkGetDeviceFaultInfoEXT) X(vkInvalidateMappedMemoryRanges) \
    X(vkGetBufferDeviceAddress) X(vkCreateQueryPool) X(vkResetQueryPool) X(vkCmdWriteTimestamp2) X(vkGetQueryPoolResults) \
    X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue)                                                       \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable)            \
    X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetDepthBiasEnable)     \
    X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipeline)

#define DECL(n) PFN_##n n = nullptr;
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
VK_GLOBAL(DECL) VK_INSTANCE(DECL) VK_DEVICE(DECL)
#undef DECL

// ================================================================ état
struct Image {
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;          // vue complète (cible de rendu, copie)
    std::map<uint64_t, VkImageView> views;      // vues d'échantillonnage par type et vue NVN du descripteur
    std::map<uint32_t, VkImageView> sviews;     // vues de stockage par dimension (niveau 0)
    bool storage = false;                       // VK_IMAGE_USAGE_STORAGE_BIT
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t w = 1, h = 1, layers = 1, levels = 1, nvnfmt = 0, target = 1;
    bool initialized = false;
    // contenu écrit par le processeur (texture « packagée » du jeu) : désentrelacé au premier usage
    uint64_t addr = 0;
    uint32_t bpu = 4;                  // octets par unité : texel, ou bloc 4×4 compressé
    uint8_t layout0 = 4;               // log2 de la hauteur de bloc maximale (GOB) de la disposition
    uint8_t swz[4] = {2, 3, 4, 5};     // nvnTextureBuilderSetSwizzle (vues d'échantillonnage)
    bool packaged = false;
    // contenu envoyé (textures packagées) : empreinte et taille de la mémoire source au dernier envoi
    uint64_t content_hash = 0, src_bytes = 0;
    // objets texture vivants du jeu qui l'utilisent (celui qui l'a créée + ceux qui la partagent) ; à 0, l'image
    // reste réemployable (by_addr) dans la limite du cache des images sans objet (texture_released)
    uint32_t refs = 0;
    uint64_t bytes = 0;      // mémoire vidéo de l'image
    uint64_t last_use = 0;   // dernière image (g.cur_frame) où un dessin l'a demandée
};
struct PoolBuf {
    uint64_t base = 0, size = 0;
    uint64_t dev = 0;   // adresse GPU Vulkan de base (vkGetBufferDeviceAddress)
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
};
struct ShaderMod {
    bool ok = false;
    std::string hash;   // nom dans le cache (diagnostic des blocages GPU)
    VkShaderModule mod = VK_NULL_HANDLE;
    std::vector<int> banks, vin, fout;
    int model = -1;     // modèle d'exécution SPIR-V : 0 sommets, 4 fragments, 5 calcul
    std::vector<std::string> tex;
    struct ImgHandle { int bank; uint32_t off; std::string dim; };
    std::vector<ImgHandle> img;                 // poignées d'images lues par le shader
    std::vector<std::pair<int, uint32_t>> texh; // poignées de textures lues en banque constante (banque, déplacement)
    // calcul (section de contrôle) : nombres de tampons uniformes, textures, images, mots de données en
    // ligne (octet 0xa9) et tampons de stockage (octet 0xb2) ; les deux derniers absents des anciens .meta
    // + textures et échantillonneurs séparés (octets 0xb4, 0xb5), entre les textures et les images
    int c0[7] = {-1, -1, -1, 0, 0, 0, 0};
    uint32_t local[3] = {1, 1, 1};
    bool glob = false;                          // accès à la mémoire globale (ligne « glob » des métadonnées)
};

struct Vk {
    bool ok = false, failed = false;
    HMODULE lib = nullptr;
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props{};
    VkPhysicalDeviceMemoryProperties memprops{};
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue q = VK_NULL_HANDLE;
    uint32_t qf = 0;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkFormat scfmt = VK_FORMAT_B8G8R8A8_UNORM;
    VkExtent2D scext{};
    std::vector<VkImage> scimgs;
    HWND hwnd = nullptr;
    bool sc_stale = false;   // swapchain périmée (fenêtre réduite ou redimensionnée) : recréée à la présentation
    int last_present_w = 0, last_present_h = 0;   // journal de la mise à l'échelle de présentation
    // RECOMP_GPU_PROFILE=N : horodatages GPU de chaque dessin / calcul de l'image N, sommés par programme
    VkQueryPool qpool = VK_NULL_HANDLE;
    uint32_t qn = 0;
    bool prof = false;
    float ts_period = 1.0f;
    std::vector<std::string> qlabel;
    VkCommandBuffer cb = VK_NULL_HANDLE;   // tampon de commandes de l'emplacement en cours (voir Slot)
    bool recording = false;
    uint64_t host_align = 4096;
    uint64_t ubo_align = 256, ubo_max = 65536;   // minUniformBufferOffsetAlignment, maxUniformBufferRange (≤ 64 Ko)
    bool depth_clip_control = false;
    bool mem_budget = false;
    // diagnostic des blocages GPU : shaders du lot en cours, shaders déjà menés à terme
    std::vector<std::string> batch;
    std::set<std::string> done_shaders;
    bool lost = false, hang_logged = false;
    uint64_t cur_frame = 0;
    std::string last_label;   // programme du dernier dessin / calcul enregistré
    bool checkpoints = false, device_fault = false;   // RECOMP_GPU_DIAG
    std::vector<std::string> marks;                   // marqueur k (1…) du lot en cours → description
    // ressources
    std::unordered_map<uint64_t, Image> images;     // clé objet texture → image
    // Plusieurs objets texture NVN peuvent décrire la même mémoire (cible de rendu, puis texture
    // enregistrée dans le pool) : ils partagent alors la même image Vulkan.
    std::unordered_map<uint64_t, uint64_t> alias;   // clé objet texture → clé de l'image partagée
    // adresse mémoire → clés des images créées à cette adresse (une par forme et disposition : le jeu réutilise
    // sa mémoire pour des ressources de formats différents, qui ne peuvent pas partager une image Vulkan)
    std::unordered_map<uint64_t, std::vector<uint64_t>> by_addr;
    std::vector<PoolBuf> pools;                     // trié par base
    std::unordered_map<std::string, ShaderMod> shaders;
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    // Pipelines sans saccade. Avec VK_EXT_graphics_pipeline_library (liaison rapide), chaque shader est compilé seul,
    // une fois, en bibliothèque : sommets (+ fenêtre, rastérisation) et fragments (+ profondeur et stencil, dynamiques).
    // Toutes sont créées avant la première image (warm_up), depuis pipelines.cache, rempli au premier lancement
    // (recomp_main --precompile, lancé par le launcher). Au dessin, il ne reste qu'à assembler : entrées de sommets
    // et sortie (formats, mélange), sans shader, puis liaison rapide des quatre bibliothèques.
    VkPipelineCache pcache = VK_NULL_HANDLE;
    bool gpl = false;
    std::unordered_map<uint64_t, VkPipeline> lib_vs, lib_fs, lib_vi, lib_fo, linked;
    uint64_t pc_dirty = 0;                    // pipelines créés depuis la dernière sauvegarde du cache
    std::chrono::steady_clock::time_point pc_saved = std::chrono::steady_clock::now();
    uint64_t libs_on_demand = 0;              // bibliothèques de shaders compilées pendant le jeu (non précompilées)
    // état dynamique déjà réglé dans le tampon de commandes en cours (remis à zéro à chaque enregistrement)
    struct Dyn { int cull = -1, front = -1, bias = -1, dt = -1, dw = -1, dop = -1, st = -1; uint32_t sf = ~0u, sb = ~0u; } dyn;
    VkDescriptorSetLayout dsl_banks = VK_NULL_HANDLE, dsl_tex = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE, tpool = VK_NULL_HANDLE;
    VkDescriptorSet texset = VK_NULL_HANDLE;
    // tampon hôte pour les constantes générées (banques pilote et poignées) : anneau de l'emplacement en cours
    VkBuffer ring = VK_NULL_HANDLE;
    uint8_t* ringptr = nullptr;
    uint64_t ringsize = 16ull << 20, ringoff = 0;
    VkBuffer zerobuf = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkSampler> samplers;   // empreinte → échantillonneur
    std::unordered_map<uint32_t, uint64_t> tex_written, samp_written;   // id → clé écrite dans le jeu de textures
    std::unordered_map<uint32_t, uint64_t> img_written;                 // id*8 + dim → vue de stockage écrite
    std::unordered_map<uint64_t, VkPipeline> cpipelines;                // module de calcul → pipeline
    uint64_t frame_dispatches = 0;
    VkImage gray = VK_NULL_HANDLE;
    bool dummies = false;   // jeu de textures entièrement rempli de descripteurs factices valides
    std::vector<std::pair<VkImage, VkImageAspectFlags>> att;   // cibles de la passe de rendu en cours
    VkBuffer dbgbuf = VK_NULL_HANDLE;   // RECOMP_DBG_SSBO
    VkDeviceMemory dbgmem = VK_NULL_HANDLE;
    uint32_t* dbgptr = nullptr;
    // rendu en cours
    bool in_render = false;
    uint64_t cur_rt[18] = {};   // cibles de la passe ouverte et niveau / couche de leurs vues
    uint64_t frame_draws = 0, frame_skipped = 0;
    bool dbg = false;   // RECOMP_VK_DEBUG=N : journal détaillé de l'image N
    std::set<std::string> missing;
    // tampons intermédiaires des envois de textures : libérés après la prochaine attente du GPU
    struct Staging { VkBuffer b; VkDeviceMemory m; };
    std::vector<Staging> staging;
    uint64_t tex_uploads = 0, tex_upload_bytes = 0, tex_reuploads = 0;   // envois, dont renvois (contenu changé)
    // Soumissions asynchrones. Chaque enregistrement (jusqu'au vidage suivant : FenceSync, présentation)
    // prend l'emplacement suivant : tampon de commandes, pools de descripteurs, anneau de constantes,
    // tampons intermédiaires. Un emplacement est réutilisé quand le GPU a fini sa soumission précédente
    // (sémaphore chronologique) : le thread GPU traduit l'image suivante pendant que la carte exécute la
    // précédente, au lieu d'attendre la fin de chaque soumission. RECOMP_SYNC_SUBMIT=1 : ancien comportement.
    struct Slot {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> dpools;   // chaîne : un pool de plus quand les précédents sont pleins
        size_t dcur = 0;
        VkBuffer ring = VK_NULL_HANDLE;
        VkDeviceMemory ringmem = VK_NULL_HANDLE;
        uint8_t* ringptr = nullptr;
        std::vector<Staging> staging;           // libérés à la réutilisation de l'emplacement
        std::vector<std::string> batch;         // shaders de la soumission (diagnostic des blocages)
        VkSemaphore sem_acq = VK_NULL_HANDLE;   // acquisition de l'image d'écran (présentation)
        uint64_t serial = 0;                    // valeur chronologique de sa dernière soumission (0 : jamais)
    };
    static constexpr int NSLOT = 6;
    Slot slots[NSLOT];
    int slot = 0;
    VkSemaphore timeline = VK_NULL_HANDLE;
    uint64_t submitted = 0;            // dernière valeur chronologique soumise
    // imports de pools libérés par le jeu : détruits quand le GPU a passé la soumission notée (begin_frame)
    std::vector<std::pair<uint64_t, PoolBuf>> dead_pools;
    std::vector<std::pair<uint64_t, Image>> dead_images;   // images de ces pools, détruites de même
    uint64_t images_freed = 0;                             // journal : images détruites
    // images dont aucun objet texture du jeu n'est vivant (refs = 0), dans l'ordre où elles le sont devenues :
    // gardées pour un réemploi par adresse, détruites au-delà de UNREF_CAP octets
    std::deque<uint64_t> unref_q;
    uint64_t unref_bytes = 0;
    bool async = true;
    std::vector<VkSemaphore> sem_img;  // fin de la copie vers l'image d'écran k (attendue par la présentation)
    uint64_t ring_peak = 0;            // RECOMP_PERF : plus forte occupation de l'anneau par une soumission
};
Vk g;
std::mutex g_vk_lk;

constexpr uint32_t TEX_MAX = 16384, SAMP_MAX = 4096;

#define VKCHK(x)                                                                    \
    do {                                                                            \
        VkResult r_ = (x);                                                          \
        if (r_ != VK_SUCCESS) {                                                     \
            HLOG("vk", "%s → %d (%s:%d)", #x, (int)r_, __FILE__, __LINE__);          \
            return false;                                                           \
        }                                                                           \
    } while (0)

// ================================================================ formats NVN → Vulkan
struct FmtInfo { VkFormat vk; uint32_t bpp; };
FmtInfo nvn_format(uint32_t f) {
    switch (f) {
    case 1: return {VK_FORMAT_R8_UNORM, 1};
    case 2: return {VK_FORMAT_R8_SNORM, 1};
    case 3: return {VK_FORMAT_R8_UINT, 1};
    case 4: return {VK_FORMAT_R8_SINT, 1};
    case 5: return {VK_FORMAT_R16_SFLOAT, 2};
    case 6: return {VK_FORMAT_R16_UNORM, 2};
    case 7: return {VK_FORMAT_R16_SNORM, 2};
    case 8: return {VK_FORMAT_R16_UINT, 2};
    case 9: return {VK_FORMAT_R16_SINT, 2};
    case 10: return {VK_FORMAT_R32_SFLOAT, 4};
    case 11: return {VK_FORMAT_R32_UINT, 4};
    case 12: return {VK_FORMAT_R32_SINT, 4};
    case 13: return {VK_FORMAT_R8G8_UNORM, 2};
    case 14: return {VK_FORMAT_R8G8_SNORM, 2};
    case 15: return {VK_FORMAT_R8G8_UINT, 2};
    case 16: return {VK_FORMAT_R8G8_SINT, 2};
    case 17: return {VK_FORMAT_R16G16_SFLOAT, 4};
    case 18: return {VK_FORMAT_R16G16_UNORM, 4};
    case 19: return {VK_FORMAT_R16G16_SNORM, 4};
    case 20: return {VK_FORMAT_R16G16_UINT, 4};
    case 21: return {VK_FORMAT_R16G16_SINT, 4};
    case 22: return {VK_FORMAT_R32G32_SFLOAT, 8};
    case 23: return {VK_FORMAT_R32G32_UINT, 8};
    case 24: return {VK_FORMAT_R32G32_SINT, 8};
    case 25: return {VK_FORMAT_R8G8B8_UNORM, 3};
    case 29: return {VK_FORMAT_R16G16B16_SFLOAT, 6};
    case 34: return {VK_FORMAT_R32G32B32_SFLOAT, 12};
    case 35: return {VK_FORMAT_R32G32B32_UINT, 12};
    case 36: return {VK_FORMAT_R32G32B32_SINT, 12};
    case 37: return {VK_FORMAT_R8G8B8A8_UNORM, 4};
    case 38: return {VK_FORMAT_R8G8B8A8_SNORM, 4};
    case 39: return {VK_FORMAT_R8G8B8A8_UINT, 4};
    case 40: return {VK_FORMAT_R8G8B8A8_SINT, 4};
    case 41: return {VK_FORMAT_R16G16B16A16_SFLOAT, 8};
    case 42: return {VK_FORMAT_R16G16B16A16_UNORM, 8};
    case 43: return {VK_FORMAT_R16G16B16A16_SNORM, 8};
    case 44: return {VK_FORMAT_R16G16B16A16_UINT, 8};
    case 45: return {VK_FORMAT_R16G16B16A16_SINT, 8};
    case 46: return {VK_FORMAT_R32G32B32A32_SFLOAT, 16};
    case 47: return {VK_FORMAT_R32G32B32A32_UINT, 16};
    case 48: return {VK_FORMAT_R32G32B32A32_SINT, 16};
    case 49: return {VK_FORMAT_S8_UINT, 1};
    case 50: return {VK_FORMAT_D16_UNORM, 2};
    case 51: return {VK_FORMAT_D24_UNORM_S8_UINT, 4};
    case 52: return {VK_FORMAT_D32_SFLOAT, 4};
    case 53: return {VK_FORMAT_D24_UNORM_S8_UINT, 4};
    case 54: return {VK_FORMAT_D32_SFLOAT_S8_UINT, 8};
    case 55: return {VK_FORMAT_R8G8B8A8_SRGB, 4};
    case 56: return {VK_FORMAT_R8G8B8A8_SRGB, 4};
    case 57: return {VK_FORMAT_R4G4B4A4_UNORM_PACK16, 2};
    case 59: return {VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2};
    case 60: return {VK_FORMAT_R5G6B5_UNORM_PACK16, 2};
    case 61: return {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4};
    case 62: return {VK_FORMAT_A2B10G10R10_UINT_PACK32, 4};
    case 63: return {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4};
    case 64: return {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 4};
    case 65: return {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 8};
    case 66: return {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8};
    case 67: return {VK_FORMAT_BC2_UNORM_BLOCK, 16};
    case 68: return {VK_FORMAT_BC3_UNORM_BLOCK, 16};
    case 69: return {VK_FORMAT_BC1_RGB_SRGB_BLOCK, 8};
    case 70: return {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 8};
    case 71: return {VK_FORMAT_BC2_SRGB_BLOCK, 16};
    case 72: return {VK_FORMAT_BC3_SRGB_BLOCK, 16};
    case 73: return {VK_FORMAT_BC4_UNORM_BLOCK, 8};
    case 74: return {VK_FORMAT_BC4_SNORM_BLOCK, 8};
    case 75: return {VK_FORMAT_BC5_UNORM_BLOCK, 16};
    case 76: return {VK_FORMAT_BC5_SNORM_BLOCK, 16};
    case 77: return {VK_FORMAT_BC7_UNORM_BLOCK, 16};
    case 78: return {VK_FORMAT_BC7_SRGB_BLOCK, 16};
    case 79: return {VK_FORMAT_BC6H_SFLOAT_BLOCK, 16};
    case 80: return {VK_FORMAT_BC6H_UFLOAT_BLOCK, 16};
    default: return {VK_FORMAT_UNDEFINED, 4};
    }
}
bool is_compressed(VkFormat f) { return f >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && f <= VK_FORMAT_BC7_SRGB_BLOCK; }
bool is_depth(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT ||
           f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT;
}
bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_S8_UINT;
}
// Même disposition en mémoire : même format Vulkan, ou variantes linéaire / sRGB d'un même format. Deux objets
// texture du jeu sur la même mémoire peuvent alors partager une image Vulkan (lue avec le format de l'image).
bool same_layout(VkFormat a, VkFormat b) {
    auto lin = [](VkFormat f) {
        switch (f) {
        case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
        case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case VK_FORMAT_BC2_SRGB_BLOCK: return VK_FORMAT_BC2_UNORM_BLOCK;
        case VK_FORMAT_BC3_SRGB_BLOCK: return VK_FORMAT_BC3_UNORM_BLOCK;
        case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_BC7_UNORM_BLOCK;
        default: return f;
        }
    };
    return lin(a) == lin(b);
}
VkFormat vertex_format(uint32_t f) {
    VkFormat v = nvn_format(f).vk;
    return v;
}

// ================================================================ initialisation
bool load_instance_fns() {
#define LOADI(n) n = (PFN_##n)vkGetInstanceProcAddr(g.inst, #n);
    VK_INSTANCE(LOADI)
#undef LOADI
    return vkCreateDevice != nullptr;
}
bool load_device_fns() {
#define LOADD(n) n = (PFN_##n)vkGetDeviceProcAddr(g.dev, #n);
    VK_DEVICE(LOADD)
#undef LOADD
    return vkCmdDraw != nullptr;
}

uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < g.memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g.memprops.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

bool make_buffer(uint64_t size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer* b,
                 VkDeviceMemory* m) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VKCHK(vkCreateBuffer(g.dev, &bi, nullptr, b));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, *b, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, props);
    // tampon de relecture (GPU → processeur) : mémoire hôte mise en cache si possible ; relire une
    // mémoire non cachée coûtait ≈ 0,3 s par image capturée (captures et instantanés)
    if (usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT && (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
        uint32_t t = mem_type(mr.memoryTypeBits, props | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (t != UINT32_MAX) ai.memoryTypeIndex = t;
    }
    VKCHK(vkAllocateMemory(g.dev, &ai, nullptr, m));
    VKCHK(vkBindBufferMemory(g.dev, *b, *m, 0));
    return true;
}

bool create_swapchain(HWND hwnd, VkSwapchainKHR old = VK_NULL_HANDLE) {
    VkSurfaceCapabilitiesKHR caps;
    VKCHK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.pd, g.surf, &caps));
    g.hwnd = hwnd;
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0) {   // fenêtre réduite : rien à créer
        g.sc_stale = true;
        return false;
    }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.pd, g.surf, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g.pd, g.surf, &n, fmts.data());
    VkSurfaceFormatKHR sf = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB) sf = f;
    RECT rc;
    GetClientRect(hwnd, &rc);
    VkExtent2D ext = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent
                                                             : VkExtent2D{(uint32_t)rc.right, (uint32_t)rc.bottom};
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g.surf;
    ci.minImageCount = std::max(caps.minImageCount, 2u);
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    {   // MAILBOX : la cadence vient de l'écran virtuel du jeu, pas du vsync de l'écran hôte
        uint32_t npm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.pd, g.surf, &npm, nullptr);
        std::vector<VkPresentModeKHR> pms(npm);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.pd, g.surf, &npm, pms.data());
        for (auto pm : pms)
            if (pm == VK_PRESENT_MODE_MAILBOX_KHR && !getenv("RECOMP_HOST_FIFO")) ci.presentMode = pm;
    }
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    VKCHK(vkCreateSwapchainKHR(g.dev, &ci, nullptr, &g.sc));
    vkGetSwapchainImagesKHR(g.dev, g.sc, &n, nullptr);
    g.scimgs.resize(n);
    vkGetSwapchainImagesKHR(g.dev, g.sc, &n, g.scimgs.data());
    g.scfmt = sf.format;
    g.scext = ext;
    {
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm);
        uint32_t npm = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.pd, g.surf, &npm, nullptr);
        std::vector<VkPresentModeKHR> pms(npm);
        vkGetPhysicalDeviceSurfacePresentModesKHR(g.pd, g.surf, &npm, pms.data());
        std::string m;
        for (auto pm : pms) m += " " + std::to_string((int)pm);
        HLOG("vk", "écran %ux%u à %u Hz ; modes de présentation :%s", (unsigned)dm.dmPelsWidth, (unsigned)dm.dmPelsHeight,
             (unsigned)dm.dmDisplayFrequency, m.c_str());
    }
    HLOG("vk", "swapchain %ux%u, %u images, format %d", ext.width, ext.height, n, (int)sf.format);
    return true;
}

bool create_layouts() {
    // jeux 0 / 1 : banques de constantes 0..30 (sommets / fragments) en tampons uniformes, lus par le cache
    // de constantes (glsl.py) ; liaison 31 : tampon de stockage de débogage (RECOMP_DBG_SSBO)
    std::vector<VkDescriptorSetLayoutBinding> b(32);
    for (uint32_t i = 0; i < 32; i++)
        b[i] = {i, i == 31 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    std::vector<VkDescriptorBindingFlags> bf(32, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT);
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 32;
    bfi.pBindingFlags = bf.data();
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.pNext = &bfi;
    li.bindingCount = 32;
    li.pBindings = b.data();
    VKCHK(vkCreateDescriptorSetLayout(g.dev, &li, nullptr, &g.dsl_banks));
    // jeu 2 : tableaux de textures (0..6 : 1D, 2D, 3D, Cube, 1DArray, 2DArray, CubeArray) et
    // d'échantillonneurs (8), indexés par les identifiants des pools NVN
    std::vector<VkDescriptorSetLayoutBinding> t;
    std::vector<VkDescriptorBindingFlags> tf;
    for (uint32_t i = 0; i < 7; i++) {
        t.push_back({i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEX_MAX, VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        tf.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
    }
    t.push_back({8, VK_DESCRIPTOR_TYPE_SAMPLER, SAMP_MAX, VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    tf.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
    // images de stockage : 10 + dimension NAK (10 1D, 12 1DArray, 13 2D, 14 2DArray, 15 3D)
    for (uint32_t i : {10u, 12u, 13u, 14u, 15u}) {
        t.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, TEX_MAX, VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        tf.push_back(VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT);
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo tfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    tfi.bindingCount = (uint32_t)tf.size();
    tfi.pBindingFlags = tf.data();
    VkDescriptorSetLayoutCreateInfo ti{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ti.pNext = &tfi;
    ti.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    ti.bindingCount = (uint32_t)t.size();
    ti.pBindings = t.data();
    VKCHK(vkCreateDescriptorSetLayout(g.dev, &ti, nullptr, &g.dsl_tex));
    VkDescriptorSetLayout sets[3] = {g.dsl_banks, g.dsl_banks, g.dsl_tex};
    VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pi.setLayoutCount = 3;
    pi.pSetLayouts = sets;
    VKCHK(vkCreatePipelineLayout(g.dev, &pi, nullptr, &g.layout));
    // pools de descripteurs des banques : par emplacement de soumission (new_bank_pool), jeu de textures : unique
    VkDescriptorPoolSize tps[3] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 7 * TEX_MAX}, {VK_DESCRIPTOR_TYPE_SAMPLER, SAMP_MAX},
                                   {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5 * TEX_MAX}};
    VkDescriptorPoolCreateInfo tci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    tci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    tci.maxSets = 1;
    tci.poolSizeCount = 3;
    tci.pPoolSizes = tps;
    VKCHK(vkCreateDescriptorPool(g.dev, &tci, nullptr, &g.tpool));
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = g.tpool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &g.dsl_tex;
    VKCHK(vkAllocateDescriptorSets(g.dev, &ai, &g.texset));
    return true;
}

bool init(HWND hwnd) {
    g.lib = LoadLibraryW(L"vulkan-1.dll");
    if (!g.lib) {
        HLOG("vk", "vulkan-1.dll introuvable");
        return false;
    }
    vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)(void*)GetProcAddress(g.lib, "vkGetInstanceProcAddr");
#define LOADG(n) n = (PFN_##n)vkGetInstanceProcAddr(nullptr, #n);
    VK_GLOBAL(LOADG)
#undef LOADG
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "recomp_main";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* iext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = iext;
    VKCHK(vkCreateInstance(&ici, nullptr, &g.inst));
    if (!load_instance_fns()) return false;
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g.inst, &n, pds.data());
    for (auto p : pds) {   // GPU dédié de préférence
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (!g.pd || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) g.pd = p, g.props = pr;
    }
    if (!g.pd) return false;
    vkGetPhysicalDeviceMemoryProperties(g.pd, &g.memprops);
    HLOG("vk", "GPU : %s (Vulkan %u.%u)", g.props.deviceName, VK_API_VERSION_MAJOR(g.props.apiVersion),
         VK_API_VERSION_MINOR(g.props.apiVersion));
    if (hwnd) {   // sans fenêtre : précompilation des pipelines (recomp_main --precompile)
        VkWin32SurfaceCreateInfoKHR si{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        si.hinstance = GetModuleHandleW(nullptr);
        si.hwnd = hwnd;
        VKCHK(vkCreateWin32SurfaceKHR(g.inst, &si, nullptr, &g.surf));
    }
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &n, qfs.data());
    g.qf = UINT32_MAX;
    for (uint32_t i = 0; i < n; i++) {
        VkBool32 pres = hwnd ? VK_FALSE : VK_TRUE;
        if (hwnd) vkGetPhysicalDeviceSurfaceSupportKHR(g.pd, i, g.surf, &pres);
        if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && pres) {
            g.qf = i;
            break;
        }
    }
    if (g.qf == UINT32_MAX) return false;
    // extensions
    vkEnumerateDeviceExtensionProperties(g.pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(g.pd, nullptr, &n, exts.data());
    auto has = [&](const char* e) {
        for (auto& x : exts)
            if (!strcmp(x.extensionName, e)) return true;
        return false;
    };
    std::vector<const char*> dext = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    if (!has(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
        HLOG("vk", "VK_EXT_external_memory_host absent : rendu Vulkan désactivé");
        return false;
    }
    dext.push_back(VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME);
    bool robust2 = has(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    if (robust2) dext.push_back(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);
    g.depth_clip_control = has(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    if (g.depth_clip_control) dext.push_back(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    g.mem_budget = has(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);   // budget par tas (journal de RECOMP_POOL_AUDIT)
    if (g.mem_budget) dext.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    // bibliothèques de pipelines à liaison rapide : chaque shader compilé seul, à l'avance (voir Vk::gpl)
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT gplf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    if (has(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) && has(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 qf2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        qf2.pNext = &gplf;
        vkGetPhysicalDeviceFeatures2(g.pd, &qf2);
        VkPhysicalDeviceProperties2 qp2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        qp2.pNext = &gplp;
        vkGetPhysicalDeviceProperties2(g.pd, &qp2);
    }
    g.gpl = gplf.graphicsPipelineLibrary && gplp.graphicsPipelineLibraryFastLinking && !getenv("RECOMP_NO_GPL");
    if (g.gpl) {
        dext.push_back(VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
        dext.push_back(VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME);
    }
    HLOG("vk", "pipelines : %s", g.gpl ? "bibliothèques de shaders précompilées, liaison rapide (VK_EXT_graphics_pipeline_library)"
                                       : "complets, compilés au premier dessin (pas de bibliothèques à liaison rapide)");
    // fonctionnalités
    VkPhysicalDeviceDepthClipControlFeaturesEXT dcc{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT};
    dcc.depthClipControl = VK_TRUE;
    VkPhysicalDeviceRobustness2FeaturesEXT rb2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    rb2.nullDescriptor = VK_TRUE;
    // Lectures hors plage bornées à la plage du descripteur et renvoyant 0, comme LDC sur une banque
    // constante du matériel. Sans cela (robustesse v1), une lecture au-delà d'un petit tampon uniforme
    // renvoyait le contenu suivant du pool importé : borne de boucle aberrante (cinématique, boucle à
    // 0x4a00 du matériau principal), GPU bloqué 2 s, réinitialisation (TDR) et périphérique perdu.
    rb2.robustBufferAccess2 = VK_TRUE;
    rb2.robustImageAccess2 = VK_TRUE;
    rb2.pNext = g.depth_clip_control ? &dcc : nullptr;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = VK_TRUE;   // capacité émise par glslang pour certains shaders de fragments
    f13.maintenance4 = VK_TRUE;
    f13.pNext = robust2 ? (void*)&rb2 : (g.depth_clip_control ? (void*)&dcc : nullptr);
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.descriptorIndexing = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    f12.shaderStorageBufferArrayNonUniformIndexing = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
    f12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
    f12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    f12.shaderInt8 = VK_TRUE;
    f12.uniformAndStorageBuffer8BitAccess = VK_TRUE;
    f12.scalarBlockLayout = VK_TRUE;   // banques de constantes : tableau serré de mots dans un tampon uniforme
    // mémoire globale des calculs (LDG/STG/ATOMG) : pointeurs de tampon vers les pools importés
    f12.bufferDeviceAddress = VK_TRUE;
    f12.shaderBufferInt64Atomics = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;   // profileur GPU (RECOMP_GPU_PROFILE)
    f12.timelineSemaphore = VK_TRUE;   // soumissions asynchrones (Vk::Slot)
    f12.pNext = &f13;
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    f11.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f11;
    f2.features.shaderInt64 = VK_TRUE;
    f2.features.shaderImageGatherExtended = VK_TRUE;   // textureGatherOffset à décalage dynamique (TLD4.AOFFI)
    f2.features.robustBufferAccess = VK_TRUE;
    f2.features.independentBlend = VK_TRUE;
    f2.features.depthClamp = VK_TRUE;
    f2.features.samplerAnisotropy = VK_TRUE;
    f2.features.fillModeNonSolid = VK_TRUE;
    f2.features.fragmentStoresAndAtomics = VK_TRUE;
    f2.features.shaderClipDistance = VK_TRUE;
    f2.features.imageCubeArray = VK_TRUE;
    f2.features.multiViewport = VK_TRUE;
    f2.features.shaderStorageImageReadWithoutFormat = VK_TRUE;    // GL_EXT_shader_image_load_formatted
    f2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    // RECOMP_GPU_DIAG=1 : marqueurs NVIDIA (dernier dessin atteint par le GPU) et rapport de faute à la
    // perte du périphérique
    static const bool gpu_diag = getenv("RECOMP_GPU_DIAG") != nullptr;
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    if (gpu_diag && has(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME)) {
        dext.push_back(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
        g.checkpoints = true;
    }
    if (gpu_diag && has(VK_EXT_DEVICE_FAULT_EXTENSION_NAME)) {
        dext.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        fault.deviceFault = VK_TRUE;
        fault.pNext = f2.pNext;
        f2.pNext = &fault;
        g.device_fault = true;
    }
    if (g.gpl) {
        gplf.graphicsPipelineLibrary = VK_TRUE;
        gplf.pNext = f2.pNext;
        f2.pNext = &gplf;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.qf;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = (uint32_t)dext.size();
    dci.ppEnabledExtensionNames = dext.data();
    VKCHK(vkCreateDevice(g.pd, &dci, nullptr, &g.dev));
    if (!load_device_fns()) return false;
    vkGetDeviceQueue(g.dev, g.qf, 0, &g.q);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &hp;
    vkGetPhysicalDeviceProperties2(g.pd, &p2);
    g.host_align = hp.minImportedHostPointerAlignment;
    g.ubo_align = std::max<uint64_t>(1, p2.properties.limits.minUniformBufferOffsetAlignment);
    g.ubo_max = std::min<uint64_t>(65536, p2.properties.limits.maxUniformBufferRange);
    HLOG("vk", "tampons uniformes : alignement %llu, taille max %u, %u par étage",
         (unsigned long long)g.ubo_align, p2.properties.limits.maxUniformBufferRange,
         p2.properties.limits.maxPerStageDescriptorUniformBuffers);
    if (hwnd && !create_swapchain(hwnd) && !g.sc_stale) return false;
    if (!create_layouts()) return false;
    // emplacements de soumission (voir Vk::Slot) et sémaphore chronologique des soumissions
    VkSemaphoreTypeCreateInfo sti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
    sti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    VkSemaphoreCreateInfo tci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    tci.pNext = &sti;
    VKCHK(vkCreateSemaphore(g.dev, &tci, nullptr, &g.timeline));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = g.qf;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (auto& s : g.slots) {
        VKCHK(vkCreateCommandPool(g.dev, &cpi, nullptr, &s.pool));
        VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        cai.commandPool = s.pool;
        cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cai.commandBufferCount = 1;
        VKCHK(vkAllocateCommandBuffers(g.dev, &cai, &s.cb));
        VKCHK(vkCreateSemaphore(g.dev, &sci, nullptr, &s.sem_acq));
        if (!make_buffer(g.ringsize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &s.ring, &s.ringmem))
            return false;
        VKCHK(vkMapMemory(g.dev, s.ringmem, 0, VK_WHOLE_SIZE, 0, (void**)&s.ringptr));
    }
    g.slot = Vk::NSLOT - 1;   // le premier enregistrement prend l'emplacement 0
    g.cb = g.slots[0].cb, g.ring = g.slots[0].ring, g.ringptr = g.slots[0].ringptr;
    // diagnostics qui relisent l'état du GPU juste après chaque soumission : soumissions synchrones
    g.async = !getenv("RECOMP_SYNC_SUBMIT") && !getenv("RECOMP_SYNC_FROM") && !getenv("RECOMP_GPU_DIAG") &&
              !getenv("RECOMP_DRAW_WATCH");
    HLOG("vk", "soumissions %s (%d emplacements, anneau de constantes de %llu Mo chacun)",
         g.async ? "asynchrones" : "synchrones (attente du GPU à chaque vidage)", Vk::NSLOT,
         (unsigned long long)(g.ringsize >> 20));
    HLOG("vk", "rendu Vulkan prêt (import mémoire hôte aligné sur 0x%llx, depth_clip_control %d)",
         (unsigned long long)g.host_align, g.depth_clip_control);
    return true;
}

// ================================================================ mémoire invitée → tampons
bool find_pool(uint64_t addr, uint64_t size, VkBuffer* buf, uint64_t* off) {
    for (auto& p : g.pools)
        if (addr >= p.base && addr + size <= p.base + p.size) {
            *buf = p.buf;
            *off = addr - p.base;
            return true;
        }
    // pool NVN contenant l'adresse
    nvn::Pool found;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(nvn::g_pool.m);
        for (auto& [k, p] : nvn::g_pool.map)
            if (p.storage && addr >= p.storage && addr + size <= p.storage + p.size) {
                found = p;
                ok = true;
                break;
            }
    }
    if (!ok) return false;
    uint64_t base = found.storage & ~(g.host_align - 1);
    uint64_t end = (found.storage + found.size + g.host_align - 1) & ~(g.host_align - 1);
    PoolBuf pb;
    pb.base = base;
    pb.size = end - base;
    VkMemoryHostPointerPropertiesEXT hpp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vkGetMemoryHostPointerPropertiesEXT(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            (void*)base, &hpp) != VK_SUCCESS)
        return false;
    VkExternalMemoryBufferCreateInfo ebi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &ebi;
    bi.size = pb.size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(g.dev, &bi, nullptr, &pb.buf) != VK_SUCCESS) return false;
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = (void*)base;
    VkMemoryAllocateFlagsInfo maf{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};   // adresse GPU du pool (LDG/STG)
    maf.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    imp.pNext = &maf;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = pb.size;
    ai.memoryTypeIndex = mem_type(hpp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkResult r = vkAllocateMemory(g.dev, &ai, nullptr, &pb.mem);
    if (r != VK_SUCCESS) {
        HLOG("vk", "import du pool 0x%llx (+0x%llx) refusé (%d)", (unsigned long long)base, (unsigned long long)pb.size, (int)r);
        return false;
    }
    vkBindBufferMemory(g.dev, pb.buf, pb.mem, 0);
    {
        VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        dai.buffer = pb.buf;
        pb.dev = vkGetBufferDeviceAddress(g.dev, &dai);
    }
    HLOG("vk", "pool importé : 0x%llx +0x%llx (pool NVN 0x%llx +0x%llx, drapeaux 0x%x ; type mémoire %u, propriétés 0x%x)",
         (unsigned long long)base, (unsigned long long)pb.size, (unsigned long long)found.storage,
         (unsigned long long)found.size, found.flags, ai.memoryTypeIndex,
         ai.memoryTypeIndex < g.memprops.memoryTypeCount ? (unsigned)g.memprops.memoryTypes[ai.memoryTypeIndex].propertyFlags : 0u);
    g.pools.push_back(pb);
    *buf = pb.buf;
    *off = addr - pb.base;
    return true;
}

// Le jeu libère un pool (nvnMemoryPoolFinalize) : les imports Vulkan qui couvrent sa mémoire sont retirés.
// Un import montre au GPU les pages physiques présentes au moment de l'import. Au changement de niveau, le jeu
// démappe sa mémoire (UnmapPhysicalMemory) puis la remappe aux mêmes adresses pour de nouveaux pools : le GPU
// lisait les nouveaux modèles à travers l'ancien import, donc dans les anciennes pages (modèles 3D faux dans
// Météo, 2026-11-09). Les objets Vulkan sont détruits quand le GPU a fini le travail déjà enregistré.
void end_render();
// Détruit des images (après la soumission qui peut encore les lire) et les retire de toutes les tables.
// Rend la mémoire vidéo libérée.
uint64_t drop_images(const std::vector<uint64_t>& dk) {
    if (dk.empty()) return 0;
    end_render();   // la passe ouverte peut viser une de ces images
    const uint64_t serial = g.submitted + (g.recording ? 1 : 0);
    std::set<uint64_t> dead(dk.begin(), dk.end());
    uint64_t bytes = 0;
    for (uint64_t k : dk) {
        auto it = g.images.find(k);
        if (it == g.images.end()) continue;
        bytes += it->second.bytes;
        if (it->second.refs == 0) g.unref_bytes -= std::min(g.unref_bytes, it->second.bytes);
        g.dead_images.emplace_back(serial, std::move(it->second));
        g.images.erase(it);
        g.images_freed++;
    }
    for (auto it = g.alias.begin(); it != g.alias.end();)
        it = dead.count(it->second) ? g.alias.erase(it) : std::next(it);
    for (auto it = g.by_addr.begin(); it != g.by_addr.end();) {
        auto& v = it->second;
        v.erase(std::remove_if(v.begin(), v.end(), [&](uint64_t k) { return dead.count(k) != 0; }), v.end());
        it = v.empty() ? g.by_addr.erase(it) : std::next(it);
    }
    // descripteurs du jeu de textures : réécrits au prochain usage (une vue détruite peut voir sa poignée
    // réattribuée à une nouvelle vue)
    g.tex_written.clear();
    g.img_written.clear();
    return bytes;
}

// Images sans objet texture vivant au-delà du plafond : les plus anciennes sont détruites. Le plafond garde le
// réemploi par adresse (textures recréées sur le même contenu : envoyées une fois) sans laisser grossir la
// mémoire vidéo.
// Une image lue par un dessin depuis moins de 5 s n'est pas détruite : le jeu copie parfois ses objets texture,
// et une copie peut rester en usage après la destruction de l'original.
constexpr uint64_t UNREF_CAP = 1536ull << 20;
void evict_unreferenced(uint64_t cap = UNREF_CAP) {
    if (g.unref_bytes <= cap) return;
    std::vector<uint64_t> dk;
    uint64_t over = g.unref_bytes - cap;
    size_t turns = g.unref_q.size();   // un tour de file au plus
    while (over && turns-- && !g.unref_q.empty()) {
        const uint64_t k = g.unref_q.front();
        g.unref_q.pop_front();
        auto it = g.images.find(k);
        if (it == g.images.end() || it->second.refs != 0) continue;   // déjà détruite, ou réemployée entre-temps
        if (g.cur_frame - it->second.last_use < 300) {   // encore lue récemment : plus tard
            g.unref_q.push_back(k);
            continue;
        }
        dk.push_back(k);
        over -= std::min(over, it->second.bytes);
    }
    drop_images(dk);
}
void ref_image(Image& o) {
    if (o.refs++ == 0) g.unref_bytes -= std::min(g.unref_bytes, o.bytes);
}

void pools_released_locked(uint64_t addr, uint64_t size) {
    if (!size) return;
    // Images dont la mémoire est dans le pool : la ressource n'existe plus. Elles n'étaient jamais détruites :
    // au fil des niveaux, 6 170 images et 15 Go de textures sur une carte de 16 Go (2026-11-09) ; les créations
    // finissaient par échouer sans bruit et des textures manquaient (icônes en carrés pleins, vignettes
    // brouillées, « shaders qui buguent petit à petit »).
    std::vector<uint64_t> dk;
    for (auto& [k, im] : g.images)
        if (im.addr && im.addr >= addr && im.addr < addr + size) dk.push_back(k);
    if (!dk.empty()) {
        const uint64_t bytes = drop_images(dk);
        static int nl = 0;
        if (nl++ < 24)
            HLOG("vk", "pool libéré 0x%llx +0x%llx : %zu image(s) libérée(s) (%.0f Mo)", (unsigned long long)addr,
                 (unsigned long long)size, dk.size(), bytes / 1048576.0);
    }
    size_t n = 0;
    for (size_t i = 0; i < g.pools.size();) {
        const PoolBuf& p = g.pools[i];
        if (p.base < addr + size && addr < p.base + p.size) {
            g.dead_pools.emplace_back(g.submitted + (g.recording ? 1 : 0), p);
            g.pools.erase(g.pools.begin() + i);
            n++;
        } else {
            i++;
        }
    }
    static int nlog = 0;
    if (n && nlog++ < 24)
        HLOG("vk", "pool libéré 0x%llx +0x%llx : %zu import(s) retiré(s)", (unsigned long long)addr,
             (unsigned long long)size, n);
}

// Place de n octets dans l'anneau des constantes (alignée sur 256, assez pour les tampons uniformes)
uint64_t ring_take(uint64_t n) {
    uint64_t off = (g.ringoff + 255) & ~255ull;
    if (off + n > g.ringsize) {   // débordement : les constantes du début de la soumission seraient écrasées
        static int nlog = 0;
        if (nlog++ < 4) HLOG("vk", "anneau de constantes plein (%llu Mo) : reprise au début", (unsigned long long)(g.ringsize >> 20));
        off = 0;
    }
    g.ringoff = off + n;
    return off;
}

// Banque de constantes liée au tampon uniforme du jeu (adresse invitée, taille) : tampon uniforme Vulkan
// de 64 Ko au plus, décalage multiple de minUniformBufferOffsetAlignment. Adresse mal alignée (NVN aligne
// ses tampons uniformes sur 256 octets, cas non observé) : instantané dans l'anneau.
bool bank_from_guest(uint64_t addr, uint64_t size, VkDescriptorBufferInfo* bi) {
    size = std::min<uint64_t>(std::max<uint64_t>(size, 16), g.ubo_max);
    VkBuffer b;
    uint64_t o;
    if (!find_pool(addr, size, &b, &o)) return false;
    if (o % g.ubo_align == 0) {
        *bi = {b, o, size};
        return true;
    }
    static int nmis = 0;
    if (nmis++ < 5) HLOG("vk", "tampon uniforme mal aligné (0x%llx) : copié dans l'anneau", (unsigned long long)addr);
    uint64_t off = ring_take(size);
    memcpy(g.ringptr + off, (const void*)addr, size);
    *bi = {g.ring, off, size};
    return true;
}

// Importe tous les pools NVN ayant une mémoire (à chaque changement de leur nombre) : la mémoire globale
// des calculs peut viser n'importe quel pool, par des pointeurs rangés dans les données du jeu.
void import_all_pools() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    {
        std::lock_guard<std::mutex> lk(nvn::g_pool.m);
        for (auto& [k, p] : nvn::g_pool.map)
            if (p.storage && p.size) v.push_back({p.storage, p.size});
    }
    static size_t last = 0;
    if (v.size() == last) return;
    last = v.size();
    for (auto& [a, sz] : v) {
        VkBuffer b;
        uint64_t o;
        find_pool(a, 4, &b, &o);
    }
}

// Adresse GPU Vulkan d'une plage invitée (0 : hors des pools importables) : mémoire globale des calculs.
uint64_t guest_devaddr(uint64_t addr, uint64_t size) {
    VkBuffer b;
    uint64_t o;
    if (!find_pool(addr, size ? size : 4, &b, &o)) return 0;
    for (auto& p : g.pools)
        if (p.buf == b) return p.dev ? p.dev + o : 0;
    return 0;
}

// ================================================================ blocages GPU
// Un shader traduit qui boucle sans fin bloque le GPU : Windows le réinitialise au bout de ≈ 2 s (TDR)
// et le périphérique Vulkan est perdu (toutes les images suivantes sont noires, les relectures nulles).
// On le signale avec les shaders du lot fautif (* = jamais menés à terme auparavant : suspects).
void note_shader(const ShaderMod* m) {
    if (m && std::find(g.batch.begin(), g.batch.end(), m->hash) == g.batch.end()) g.batch.push_back(m->hash);
}
uint64_t gpu_done_serial();
std::string batch_desc() {   // shaders des soumissions pas encore terminées et de l'enregistrement en cours
    std::vector<std::string> all;
    const uint64_t done = gpu_done_serial();
    for (auto& sl : g.slots)
        if (sl.serial > done) all.insert(all.end(), sl.batch.begin(), sl.batch.end());
    all.insert(all.end(), g.batch.begin(), g.batch.end());
    std::sort(all.begin(), all.end());
    all.erase(std::unique(all.begin(), all.end()), all.end());
    std::string s;
    int n = 0;
    for (auto& h : all)
        if (!g.done_shaders.count(h) && n++ < 32) s += " " + h + "*";
    for (auto& h : all)
        if (g.done_shaders.count(h) && n++ < 48) s += " " + h;
    if (n > 48) s += " …";
    return s;
}
// RECOMP_GPU_DIAG : marqueur NVIDIA k (1…) dans le lot en cours
void mark(const std::string& desc) {
    if (!g.checkpoints || !vkCmdSetCheckpointNV) return;
    g.marks.push_back(desc);
    vkCmdSetCheckpointNV(g.cb, (const void*)(uintptr_t)g.marks.size());
}
void report_lost() {
    if (g.checkpoints && vkGetQueueCheckpointDataNV) {
        uint32_t n = 0;
        vkGetQueueCheckpointDataNV(g.q, &n, nullptr);
        std::vector<VkCheckpointDataNV> cd(n, VkCheckpointDataNV{VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV});
        vkGetQueueCheckpointDataNV(g.q, &n, cd.data());
        HLOG("vk", "marqueurs : %u relevés sur %zu posés dans le lot", n, g.marks.size());
        for (auto& c : cd) {
            size_t k = (size_t)(uintptr_t)c.pCheckpointMarker;
            HLOG("vk", "  dernier marqueur à l'étage 0x%x : n° %zu « %s »", (unsigned)c.stage, k,
                 k && k <= g.marks.size() ? g.marks[k - 1].c_str() : "?");
        }
    }
    if (g.device_fault && vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT fc{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        VkResult r = vkGetDeviceFaultInfoEXT(g.dev, &fc, nullptr);
        HLOG("vk", "rapport de faute : %d (%u adresses, %u fournisseur)", (int)r, fc.addressInfoCount, fc.vendorInfoCount);
        std::vector<VkDeviceFaultAddressInfoEXT> ai(fc.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vi(fc.vendorInfoCount);
        fc.vendorBinarySize = 0;
        VkDeviceFaultInfoEXT fi{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
        fi.pAddressInfos = ai.empty() ? nullptr : ai.data();
        fi.pVendorInfos = vi.empty() ? nullptr : vi.data();
        r = vkGetDeviceFaultInfoEXT(g.dev, &fc, &fi);
        HLOG("vk", "faute GPU (%d) : %s", (int)r, fi.description);
        for (auto& a : ai)
            HLOG("vk", "  adresse (type %d) : 0x%llx, précision 0x%llx", (int)a.addressType, (unsigned long long)a.reportedAddress,
                 (unsigned long long)a.addressPrecision);
        for (auto& v : vi)
            HLOG("vk", "  fournisseur : %s (code 0x%llx, données 0x%llx)", v.description, (unsigned long long)v.vendorFaultCode,
                 (unsigned long long)v.vendorFaultData);
    }
}
void check_vk(VkResult r, const char* where) {
    if (r == VK_ERROR_DEVICE_LOST && !g.lost) {
        g.lost = true;
        HLOG("vk", "PÉRIPHÉRIQUE PERDU (%s, image %llu) ; shaders du lot (* = jamais menés à terme avant) :%s", where,
             (unsigned long long)g.cur_frame, batch_desc().c_str());
        report_lost();
    }
}
// valeur du sémaphore chronologique atteinte par le GPU (toutes les soumissions jusqu'à elle sont finies)
uint64_t gpu_done_serial() {
    uint64_t v = 0;
    if (g.timeline && vkGetSemaphoreCounterValue(g.dev, g.timeline, &v) != VK_SUCCESS) return ~0ull;   // périphérique perdu
    return v;
}
std::atomic<uint64_t> g_wait_us{0};   // RECOMP_PERF : attentes du GPU par le thread de traduction
// Attend que le GPU ait fini la soumission `val` (et toutes les précédentes : un signal de sémaphore couvre
// tout le travail soumis avant lui sur la file).
void wait_serial(uint64_t val, const char* where) {
    if (!val || g.lost) return;
    auto t0 = std::chrono::steady_clock::now();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g.timeline;
    wi.pValues = &val;
    VkResult r;
    bool hung = false;
    while ((r = vkWaitSemaphores(g.dev, &wi, 1000000000ull)) == VK_TIMEOUT)
        if (!g.hang_logged) {
            g.hang_logged = hung = true;
            HLOG("vk", "GPU sans réponse depuis 1 s (%s, image %llu, dernier programme « %s ») ; shaders du lot (* = jamais menés à terme avant) :%s",
                 where, (unsigned long long)g.cur_frame, g.last_label.c_str(), batch_desc().c_str());
            report_lost();   // marqueurs atteints pendant le blocage
        }
    if (hung) {
        HLOG("vk", "fin de l'attente bloquée : résultat %d", (int)r);
        report_lost();
    }
    check_vk(r, where);
    if (r == VK_SUCCESS && !hung) {
        for (auto& sl : g.slots)
            if (sl.serial && sl.serial <= val) {
                for (auto& h : sl.batch) g.done_shaders.insert(h);
                sl.batch.clear();
            }
        if (val >= g.submitted) g.marks.clear();   // plus rien en vol : numéros de marqueurs libres
    }
    g_wait_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

// ================================================================ descripteurs factices
// Les tableaux du jeu 2 sont « partiellement liés » : un emplacement jamais écrit puis lu par un
// shader (identifiant hors pool, texture 2D lue en 3D par une traduction fautive…) fait lire au GPU
// l'adresse 0 : faute de page, réinitialisation du GPU (TDR) et périphérique perdu, constaté à la
// cinématique. Tous les emplacements reçoivent donc d'abord une image 1×1 noire du bon type et un
// échantillonneur par défaut ; les vraies liaisons les remplacent ensuite.
void barrier_all();
VkSampler get_sampler(const nvn::SamplerInfo& s);
VkImage dummy_image(VkImageType type, uint32_t layers, bool cube, VkDeviceMemory* mem) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    ci.imageType = type;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {1, 1, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VkImage img = VK_NULL_HANDLE;
    if (vkCreateImage(g.dev, &ci, nullptr, &img) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.dev, img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.dev, &ai, nullptr, mem) != VK_SUCCESS) return VK_NULL_HANDLE;
    vkBindImageMemory(g.dev, img, *mem, 0);
    // UNDEFINED → GENERAL puis noir transparent
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.image = img;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(g.cb, &di);
    VkClearColorValue c{};
    vkCmdClearColorImage(g.cb, img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &ib.subresourceRange);
    return img;
}
VkImageView dummy_view(VkImage img, VkImageViewType vt, uint32_t layers) {
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img;
    vi.viewType = vt;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(g.dev, &vi, nullptr, &v);
    return v;
}
void fill_dummy_descriptors() {   // dans un tampon de commandes en cours d'enregistrement
    g.dummies = true;
    VkDeviceMemory m1, m2, m3, mc;
    VkImage i1 = dummy_image(VK_IMAGE_TYPE_1D, 1, false, &m1), i2 = dummy_image(VK_IMAGE_TYPE_2D, 1, false, &m2),
            i3 = dummy_image(VK_IMAGE_TYPE_3D, 1, false, &m3), ic = dummy_image(VK_IMAGE_TYPE_2D, 6, true, &mc);
    if (!i1 || !i2 || !i3 || !ic) {
        HLOG("vk", "images factices impossibles : jeu de textures non prérempli");
        return;
    }
    barrier_all();
    // échantillonnées (0 1D, 1 2D, 2 3D, 3 Cube, 4 1DArray, 5 2DArray, 6 CubeArray)
    VkImageView sv[7] = {dummy_view(i1, VK_IMAGE_VIEW_TYPE_1D, 1),       dummy_view(i2, VK_IMAGE_VIEW_TYPE_2D, 1),
                         dummy_view(i3, VK_IMAGE_VIEW_TYPE_3D, 1),       dummy_view(ic, VK_IMAGE_VIEW_TYPE_CUBE, 6),
                         dummy_view(i1, VK_IMAGE_VIEW_TYPE_1D_ARRAY, 1), dummy_view(i2, VK_IMAGE_VIEW_TYPE_2D_ARRAY, 1),
                         dummy_view(ic, VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, 6)};
    std::vector<VkDescriptorImageInfo> ii(TEX_MAX);
    for (uint32_t k = 0; k < 7; k++) {
        if (!sv[k]) continue;
        for (auto& x : ii) x = {VK_NULL_HANDLE, sv[k], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws.dstSet = g.texset;
        ws.dstBinding = k;
        ws.descriptorCount = TEX_MAX;
        ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ws.pImageInfo = ii.data();
        vkUpdateDescriptorSets(g.dev, 1, &ws, 0, nullptr);
    }
    // stockage (10 1D, 12 1DArray, 13 2D, 14 2DArray, 15 3D)
    struct { uint32_t b; VkImageView v; } st[] = {{10, sv[0]}, {12, sv[4]}, {13, sv[1]}, {14, sv[5]}, {15, sv[2]}};
    for (auto& s : st) {
        if (!s.v) continue;
        for (auto& x : ii) x = {VK_NULL_HANDLE, s.v, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws.dstSet = g.texset;
        ws.dstBinding = s.b;
        ws.descriptorCount = TEX_MAX;
        ws.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ws.pImageInfo = ii.data();
        vkUpdateDescriptorSets(g.dev, 1, &ws, 0, nullptr);
    }
    // échantillonneurs (8)
    VkSampler smp = get_sampler(nvn::SamplerInfo{});
    std::vector<VkDescriptorImageInfo> si(SAMP_MAX, VkDescriptorImageInfo{smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
    VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    ws.dstSet = g.texset;
    ws.dstBinding = 8;
    ws.descriptorCount = SAMP_MAX;
    ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    ws.pImageInfo = si.data();
    vkUpdateDescriptorSets(g.dev, 1, &ws, 0, nullptr);
    HLOG("vk", "jeu de textures prérempli : %u emplacements × 12 tableaux + %u échantillonneurs factices", TEX_MAX, SAMP_MAX);
}

// ================================================================ commandes
// Nouvel enregistrement : emplacement suivant, réutilisé quand le GPU a fini sa soumission précédente.
void begin_frame() {
    if (g.recording) return;
    g.slot = (g.slot + 1) % Vk::NSLOT;
    Vk::Slot& s = g.slots[g.slot];
    if (s.serial) wait_serial(s.serial, "réutilisation d'un emplacement");
    if (!g.dead_images.empty()) {   // images des pools libérés (pools_released) que le GPU n'utilise plus
        const uint64_t done = gpu_done_serial();
        for (size_t i = 0; i < g.dead_images.size();) {
            if (g.dead_images[i].first > done) { i++; continue; }
            Image& im = g.dead_images[i].second;
            for (auto& [k, v] : im.views)
                if (v && v != im.view) vkDestroyImageView(g.dev, v, nullptr);
            for (auto& [k, v] : im.sviews)
                if (v) vkDestroyImageView(g.dev, v, nullptr);
            if (im.view) vkDestroyImageView(g.dev, im.view, nullptr);
            if (im.img) vkDestroyImage(g.dev, im.img, nullptr);
            if (im.mem) vkFreeMemory(g.dev, im.mem, nullptr);
            g.dead_images.erase(g.dead_images.begin() + i);
        }
    }
    if (!g.dead_pools.empty()) {   // imports de pools libérés (pools_released) que le GPU n'utilise plus
        const uint64_t done = gpu_done_serial();
        for (size_t i = 0; i < g.dead_pools.size();) {
            if (g.dead_pools[i].first <= done) {
                vkDestroyBuffer(g.dev, g.dead_pools[i].second.buf, nullptr);
                vkFreeMemory(g.dev, g.dead_pools[i].second.mem, nullptr);
                g.dead_pools.erase(g.dead_pools.begin() + i);
            } else {
                i++;
            }
        }
    }
    for (auto& x : s.staging) {
        vkDestroyBuffer(g.dev, x.b, nullptr);
        vkFreeMemory(g.dev, x.m, nullptr);
    }
    s.staging.clear();
    vkResetCommandPool(g.dev, s.pool, 0);
    for (VkDescriptorPool p : s.dpools) vkResetDescriptorPool(g.dev, p, 0);
    s.dcur = 0;
    g.cb = s.cb, g.ring = s.ring, g.ringptr = s.ringptr, g.ringoff = 0;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(g.cb, &bi);
    g.dyn = Vk::Dyn{};    // état dynamique indéfini au début d'un tampon de commandes
    g.recording = true;   // compteurs de dessins : remis à zéro à la présentation (plusieurs vidages par image)
    // Les soumissions s'exécutaient une à une (attente du GPU entre elles) : une barrière complète en tête
    // garde cet ordre sur le GPU (sa première portée couvre tout le travail soumis avant sur la file).
    barrier_all();
    if (!g.dummies) fill_dummy_descriptors();
}

// Pools de descripteurs des banques (jeux 0 / 1) : chaîne par emplacement, allongée à l'épuisement.
VkDescriptorPool new_bank_pool() {
    constexpr uint32_t SETS = 4096;
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 31 * SETS}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = SETS;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = ps;
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(g.dev, &pci, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
    return p;
}
bool alloc_bank_sets(uint32_t n, VkDescriptorSet* out) {   // n ≤ 2
    Vk::Slot& s = g.slots[g.slot];
    VkDescriptorSetLayout lays[2] = {g.dsl_banks, g.dsl_banks};
    for (;;) {
        if (s.dcur >= s.dpools.size()) {
            VkDescriptorPool p = new_bank_pool();
            if (!p) return false;
            s.dpools.push_back(p);
            static int nlog = 0;
            if (s.dpools.size() > 1 && nlog++ < 8)
                HLOG("vk", "emplacement %d : pool de descripteurs n° %zu (soumission chargée)", g.slot, s.dpools.size());
        }
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = s.dpools[s.dcur];
        dai.descriptorSetCount = n;
        dai.pSetLayouts = lays;
        VkResult r = vkAllocateDescriptorSets(g.dev, &dai, out);
        if (r == VK_SUCCESS) return true;
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) return false;
        s.dcur++;
    }
}

void barrier_all() {
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(g.cb, &di);
}

void end_render() {
    if (g.in_render) {
        vkCmdEndRendering(g.cb);
        g.in_render = false;
        // barrière d'image sur chaque cible de la passe (GENERAL → GENERAL) : une simple barrière mémoire
        // ne laisse pas au pilote l'occasion de décompresser une profondeur / couleur rendue avant de la
        // lire comme texture dans la même soumission (atlas d'ombres de la cinématique : faute GPU)
        static const bool no_att = getenv("RECOMP_NO_ATT_BARRIER") != nullptr;
        if (!no_att && !g.att.empty()) {
            std::vector<VkImageMemoryBarrier2> ibs;
            for (auto& [img, aspect] : g.att) {
                VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
                ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                ib.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_2_MEMORY_WRITE_BIT;
                ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
                ib.oldLayout = ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                ib.image = img;
                ib.subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
                ibs.push_back(ib);
            }
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.imageMemoryBarrierCount = (uint32_t)ibs.size();
            di.pImageMemoryBarriers = ibs.data();
            vkCmdPipelineBarrier2(g.cb, &di);
        }
        g.att.clear();
        barrier_all();
    }
}

// ---------------------------------------------------------------- textures du jeu (block-linear)
// Disposition établie sur les données du jeu (textures « packagées » vidées par RECOMP_TEX_DUMP) :
//  * GOB de 64 o × 8 lignes ; l'unité de 16 o (octets x[0..3] d'une ligne) est rangée dans le GOB
//    selon y0 (bit 4), y1 (bit 5), x4 (bit 6), y2 (bit 7), x5 (bit 8) — pas l'ordre Maxwell
//    (y0, x4, y1, y2, x5) ;
//  * blocs de bh GOB de haut, en colonnes d'un GOB de large, de gauche à droite puis de haut en bas ;
//  * hauteur de bloc d'un niveau = min(2^L, plus petite puissance de 2 de GOB couvrant le niveau,
//    16 au plus), L = premier octet de NVNpackagedTextureLayout (0 à 4) ;
//  * niveaux de mip à la suite (largeur alignée sur 64 o, hauteur sur 8·bh lignes).
// Pour un format compressé, une « ligne » est une rangée de blocs 4×4.
uint32_t std_block_height(uint32_t rows) {
    uint32_t bh = 16;
    while (bh > 1 && rows <= (bh / 2) * 8) bh >>= 1;
    return bh;
}
uint64_t bl_level_size(uint32_t wbytes, uint32_t rows, uint32_t bh) {
    uint64_t wg = (wbytes + 63) / 64;
    uint64_t ra = (rows + 8ull * bh - 1) / (8ull * bh) * (8ull * bh);
    return wg * 64 * ra;
}
// lignes [y0, y1) d'un niveau (y1 = 0 : jusqu'à la fin) ; les bandes de lignes sont indépendantes
void deswizzle_level(const uint8_t* src, uint8_t* dst, uint32_t wbytes, uint32_t rows, uint32_t bh, uint32_t y0 = 0,
                     uint32_t y1 = 0) {
    const uint64_t wg = (wbytes + 63) / 64, col = 512ull * bh;
    if (!y1 || y1 > rows) y1 = rows;
    for (uint32_t y = y0; y < y1; y++) {
        const uint64_t rb = (uint64_t)(y / (8 * bh)) * wg * col + (uint64_t)((y % (8 * bh)) / 8) * 512;
        const uint32_t yo = ((y & 1) << 4) | (((y >> 1) & 1) << 5) | (((y >> 2) & 1) << 7);
        uint8_t* d = dst + (uint64_t)y * wbytes;
        for (uint32_t x = 0; x < wbytes; x += 16) {
            const uint64_t o = rb + (uint64_t)(x >> 6) * col + yo + (((x >> 4) & 1) << 6) + (((x >> 5) & 1) << 8);
            memcpy(d + x, src + o, std::min<uint32_t>(16, wbytes - x));
        }
    }
}

// Captures de cibles de rendu (RECOMP_RT_SNAPSHOT) : relues après l'attente du GPU.
struct Readback {
    VkBuffer b;
    VkDeviceMemory m;
    VkImage tmp;
    VkDeviceMemory tmpm;
    uint32_t w, h;
    std::string path;
    int depth = 0;   // 0 : RGBA8 ; 4 : profondeur 32 bits (flottant ou 24+8), 2 : 16 bits → niveaux de gris
    bool dfloat = false;
    uint64_t raw_addr = 0, raw_size = 0;   // gpu_peek : octets bruts vus par le GPU
    std::vector<std::array<uint64_t, 3>> audit;   // contrôle d'un pool importé : (adresse, taille, décalage dans b)
    uint64_t audit_frame = 0;
};
std::vector<Readback> g_readbacks;
void write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h);

// ---- profileur GPU (RECOMP_GPU_PROFILE=N) : deux horodatages par dessin / calcul de l'image N
constexpr uint32_t QMAX = 16384;
int prof_begin() {
    if (!g.prof || !g.qpool || g.qn + 2 > QMAX) return -1;
    const int q = (int)g.qn;
    g.qn += 2;
    vkCmdWriteTimestamp2(g.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qpool, (uint32_t)q);
    return q;
}
void prof_end(int q, const std::string& label) {
    if (q < 0) return;
    vkCmdWriteTimestamp2(g.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g.qpool, (uint32_t)q + 1);
    g.qlabel.push_back(label);
}
// à la présentation, après l'attente du GPU : bilan de l'image profilée, puis armement de la suivante
void prof_frame(uint64_t frame) {
    static const long pf = getenv("RECOMP_GPU_PROFILE") ? atol(getenv("RECOMP_GPU_PROFILE")) : -1;
    if (pf < 0) return;
    if (g.prof) {
        g.prof = false;
        std::vector<uint64_t> ts(g.qn, 0);
        if (g.qn && vkGetQueryPoolResults(g.dev, g.qpool, 0, g.qn, ts.size() * 8, ts.data(), 8, VK_QUERY_RESULT_64_BIT) ==
                        VK_SUCCESS) {
            std::map<std::string, std::pair<double, int>> acc;
            double total = 0;
            for (size_t k = 0; k < g.qlabel.size() && 2 * k + 1 < ts.size(); k++) {
                const double ms = ts[2 * k + 1] > ts[2 * k] ? (ts[2 * k + 1] - ts[2 * k]) * g.ts_period * 1e-6 : 0.0;
                auto& a = acc[g.qlabel[k]];
                a.first += ms, a.second++;
                total += ms;
            }
            std::vector<std::pair<double, std::string>> v;
            for (auto& [l, a] : acc) v.push_back({a.first, l + " ×" + std::to_string(a.second)});
            std::sort(v.rbegin(), v.rend());
            HLOG("gpuprof", "image %llu : %zu dessins / calculs mesurés, %.2f ms GPU au total", (unsigned long long)frame,
                 g.qlabel.size(), total);
            for (size_t k = 0; k < v.size() && k < 30; k++)
                HLOG("gpuprof", "  %6.2f ms  %4.1f %%  %s", v[k].first, total > 0 ? 100.0 * v[k].first / total : 0.0,
                     v[k].second.c_str());
        }
    }
    if ((long)frame + 1 == pf) {
        if (!g.qpool) {
            VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qi.queryCount = QMAX;
            vkCreateQueryPool(g.dev, &qi, nullptr, &g.qpool);
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(g.pd, &pp);
            g.ts_period = pp.limits.timestampPeriod;
        }
        vkResetQueryPool(g.dev, g.qpool, 0, QMAX);
        g.qn = 0;
        g.qlabel.clear();
        g.prof = true;
    }
}

void free_staging() {   // après une attente de la barrière du GPU
    for (auto& s : g.staging) {
        vkDestroyBuffer(g.dev, s.b, nullptr);
        vkFreeMemory(g.dev, s.m, nullptr);
    }
    g.staging.clear();
    for (auto& r : g_readbacks) {
        uint8_t* p = nullptr;
        if (vkMapMemory(g.dev, r.m, 0, VK_WHOLE_SIZE, 0, (void**)&p) == VK_SUCCESS) {
            if (!r.audit.empty()) {   // RECOMP_POOL_AUDIT : échantillons du pool vus par le GPU / le processeur
                size_t same = 0, gpu_zero = 0, other = 0, unmapped = 0;
                uint64_t first_bad = 0;
                for (auto& [a, n, o] : r.audit) {
                    if (!mem_valid(a, n)) {   // mémoire démappée par le jeu : import périmé (pools_released)
                        unmapped++;
                        if (!first_bad) first_bad = a;
                        continue;
                    }
                    const uint8_t* cpu = (const uint8_t*)a;
                    if (!memcmp(p + o, cpu, n)) { same++; continue; }
                    bool z = true;
                    for (uint64_t i = 0; i < n && z; i++) z = p[o + i] == 0;
                    (z ? gpu_zero : other)++;
                    if (!first_bad) first_bad = a;
                }
                if (gpu_zero || other || unmapped)
                    HLOG("vk", "contrôle image %llu, import 0x%llx : %zu échantillons, %zu identiques, %zu NULS côté GPU, %zu autres écarts, %zu démappés (premier 0x%llx)",
                         (unsigned long long)r.audit_frame, (unsigned long long)r.raw_addr, r.audit.size(), same, gpu_zero, other,
                         unmapped, (unsigned long long)first_bad);
                else
                    HLOG("vk", "contrôle image %llu, import 0x%llx : %zu échantillons identiques", (unsigned long long)r.audit_frame,
                         (unsigned long long)r.raw_addr, r.audit.size());
            } else if (r.raw_size) {   // gpu_peek : vu par le GPU contre vu par le processeur au même instant
                const uint8_t* cpu = (const uint8_t*)r.raw_addr;
                size_t diff = SIZE_MAX, ndiff = 0;
                for (size_t i = 0; i < r.raw_size; i++)
                    if (p[i] != cpu[i]) ndiff++, diff = std::min(diff, i);
                if (FILE* f = fopen((r.path + ".gpu.bin").c_str(), "wb")) fwrite(p, 1, r.raw_size, f), fclose(f);
                if (FILE* f = fopen((r.path + ".cpu.bin").c_str(), "wb")) fwrite(cpu, 1, r.raw_size, f), fclose(f);
                if (ndiff)
                    HLOG("vk", "relecture GPU %s (0x%llx +0x%llx) : %zu octets DIFFÉRENTS du processeur (premier à +0x%zx)",
                         r.path.c_str(), (unsigned long long)r.raw_addr, (unsigned long long)r.raw_size, ndiff, diff);
                else
                    HLOG("vk", "relecture GPU %s (0x%llx +0x%llx) : identique au processeur", r.path.c_str(),
                         (unsigned long long)r.raw_addr, (unsigned long long)r.raw_size);
            } else if (!r.depth) {
                write_png(r.path.c_str(), p, r.w, r.h);
            } else {   // profondeur : niveaux de gris étirés entre min et max (hors 0 et 1 exacts)
                std::vector<float> d((size_t)r.w * r.h);
                for (size_t i = 0; i < d.size(); i++) {
                    if (r.depth == 2) d[i] = ((const uint16_t*)p)[i] / 65535.0f;
                    else if (r.dfloat) d[i] = ((const float*)p)[i];
                    else d[i] = (((const uint32_t*)p)[i] & 0xffffff) / 16777215.0f;
                }
                float lo = 1e30f, hi = -1e30f;
                for (float v : d)
                    if (v > 0.0f && v < 1.0f) lo = std::min(lo, v), hi = std::max(hi, v);
                std::vector<uint8_t> rgba(d.size() * 4);
                for (size_t i = 0; i < d.size(); i++) {
                    float v = d[i];
                    uint8_t c = v <= 0.0f ? 0 : v >= 1.0f ? 255 : hi > lo ? (uint8_t)(16 + 223 * (v - lo) / (hi - lo)) : 128;
                    rgba[4 * i] = rgba[4 * i + 1] = rgba[4 * i + 2] = c;
                    rgba[4 * i + 3] = 255;
                }
                write_png(r.path.c_str(), rgba.data(), r.w, r.h);
                HLOG("vk", "capture profondeur %s : min %.6f max %.6f (hors 0 et 1)", r.path.c_str(), lo, hi);
            }
            vkUnmapMemory(g.dev, r.m);
        }
        vkDestroyBuffer(g.dev, r.b, nullptr);
        vkFreeMemory(g.dev, r.m, nullptr);
        if (r.tmp) vkDestroyImage(g.dev, r.tmp, nullptr);
        if (r.tmpm) vkFreeMemory(g.dev, r.tmpm, nullptr);
    }
    if (!g_readbacks.empty()) HLOG("vk", "captures écrites : %zu", g_readbacks.size());
    g_readbacks.clear();
}

// Empreinte 64 bits d'une plage de mémoire invitée (contenu d'une texture packagée) : par mots de 8 octets,
// multiplication-décalage ; ≈ 10 Go/s, calculée à l'envoi et quand une nouvelle texture packagée reprend
// l'adresse d'une image existante.
uint64_t hash_mem(const uint8_t* p, uint64_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n, a, b, c, d;
    uint64_t i = 0;
    for (; i + 32 <= n; i += 32) {
        memcpy(&a, p + i, 8), memcpy(&b, p + i + 8, 8), memcpy(&c, p + i + 16, 8), memcpy(&d, p + i + 24, 8);
        h = (h ^ (a * 0x87C37B91114253D5ull)) * 0x4CF5AD432745937Full;
        h = (h ^ (b * 0x52DCE729DA3ED6A5ull)) * 0x87C37B91114253D5ull;
        h = (h ^ (c * 0x4CF5AD432745937Full)) * 0x52DCE729DA3ED6A5ull;
        h = (h ^ (d * 0x9E3779B97F4A7C15ull)) * 0x4CF5AD432745937Full;
        h ^= h >> 29;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 31);
}

// Copie le contenu d'une texture packagée (mémoire invitée, block-linear) dans son image Vulkan.
bool upload_packaged(Image& im) {
    const bool bc = is_compressed(im.fmt);
    const uint32_t unit = bc ? 4 : 1;
    struct Lv { uint32_t wbytes, rows, bh, pw, ph; uint64_t soff, doff; };
    std::vector<Lv> lv;
    uint64_t soff = 0, doff = 0;
    uint32_t bh0 = 1;
    const uint32_t bhmax = 1u << std::min<uint32_t>(im.layout0, 4);
    for (uint32_t l = 0; l < im.levels; l++) {
        uint32_t pw = std::max(1u, im.w >> l), ph = std::max(1u, im.h >> l);
        uint32_t wbytes = (pw + unit - 1) / unit * im.bpu, rows = (ph + unit - 1) / unit;
        uint32_t bh = std::min(bhmax, std_block_height(rows));
        if (l == 0) bh0 = bh;
        lv.push_back({wbytes, rows, bh, pw, ph, soff, doff});
        soff += bl_level_size(wbytes, rows, bh);
        doff = (doff + (uint64_t)wbytes * rows + 15) & ~15ull;
    }
    const uint64_t blk = 512ull * bh0, layer_stride = (soff + blk - 1) / blk * blk;
    const uint64_t src_total = layer_stride * (im.layers - 1) + soff;
    if (!mem_valid(im.addr, src_total)) {
        static int nw = 0;
        if (nw++ < 8) HLOG("vk", "texture %llx (%ux%u fmt %u) : mémoire illisible (0x%llx o)", (unsigned long long)im.addr,
                           im.w, im.h, im.nvnfmt, (unsigned long long)src_total);
        return false;
    }
    VkBuffer b;
    VkDeviceMemory m;
    if (!make_buffer(doff * im.layers, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &b, &m))
        return false;
    uint8_t* p = nullptr;
    vkMapMemory(g.dev, m, 0, VK_WHOLE_SIZE, 0, (void**)&p);
    std::vector<VkBufferImageCopy> regions;
    // Désentrelacement par bandes de lignes (≈ 1 Mo), réparties sur plusieurs cœurs pour les grosses textures :
    // les changements de scène en envoient des centaines de Mo d'un coup (pic de ≈ 200 ms sur un seul cœur).
    struct Job { const uint8_t* src; uint8_t* dst; uint32_t wbytes, rows, bh, y0, y1; };
    std::vector<Job> jobs;
    for (uint32_t layer = 0; layer < im.layers; layer++)
        for (uint32_t l = 0; l < (uint32_t)lv.size(); l++) {
            const Lv& v = lv[l];
            const uint64_t dst = layer * doff + v.doff;
            const uint8_t* s = (const uint8_t*)(im.addr + layer * layer_stride + v.soff);
            const uint32_t band = std::max<uint32_t>(8 * v.bh, (uint32_t)((1u << 20) / std::max<uint32_t>(1, v.wbytes)));
            for (uint32_t y = 0; y < v.rows; y += band) jobs.push_back({s, p + dst, v.wbytes, v.rows, v.bh, y, std::min(v.rows, y + band)});
            VkBufferImageCopy r{};
            r.bufferOffset = dst;
            r.bufferRowLength = v.wbytes / im.bpu * unit;
            r.bufferImageHeight = v.rows * unit;
            r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, layer, 1};
            r.imageExtent = {v.pw, v.ph, 1};
            regions.push_back(r);
        }
    auto run = [](const Job& j) { deswizzle_level(j.src, j.dst, j.wbytes, j.rows, j.bh, j.y0, j.y1); };
    static const unsigned NT = std::max(1u, std::min(8u, std::thread::hardware_concurrency() / 2));
    if (NT == 1 || jobs.size() < 4 || doff * im.layers < (4u << 20)) {
        for (auto& j : jobs) run(j);
    } else {
        std::atomic<size_t> next{0};
        auto worker = [&] {
            for (size_t k; (k = next++) < jobs.size();) run(jobs[k]);
        };
        std::vector<std::thread> th;
        for (unsigned t = 1; t < NT && t < jobs.size(); t++) th.emplace_back(worker);
        worker();
        for (auto& t : th) t.join();
    }
    vkUnmapMemory(g.dev, m);
    vkCmdCopyBufferToImage(g.cb, b, im.img, VK_IMAGE_LAYOUT_GENERAL, (uint32_t)regions.size(), regions.data());
    barrier_all();
    g.staging.push_back({b, m});
    g.tex_uploads++;
    g.tex_upload_bytes += src_total;
    im.src_bytes = src_total;
    im.content_hash = hash_mem((const uint8_t*)im.addr, src_total);
    return true;
}

void image_to_general(Image& im) {
    if (im.initialized) return;
    end_render();   // transitions et effacements interdits dans une passe de rendu dynamique
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.image = im.img;
    ib.subresourceRange = {im.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(g.cb, &di);
    im.initialized = true;
    static const bool no_upload = getenv("RECOMP_NO_TEX_UPLOAD") != nullptr;
    if (im.packaged && im.addr && !no_upload && (im.target == 1 || im.target == 4 || im.target == 8 || im.target == 9) &&
        upload_packaged(im))
        return;
    if (!is_depth(im.fmt) && !is_compressed(im.fmt)) {   // contenu initial neutre (cibles, textures non packagées)
        VkClearColorValue c{{0.5f, 0.5f, 0.5f, 1.0f}};
        VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(g.cb, im.img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
    }
}

Image* get_image(uint64_t texkey);
// Mémoire vidéo utilisée et budget du pilote (VK_EXT_memory_budget), pour le journal périodique
std::string vram_text() {
    if (!g.mem_budget || !vkGetPhysicalDeviceMemoryProperties2) return "";
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    mp.pNext = &bp;
    vkGetPhysicalDeviceMemoryProperties2(g.pd, &mp);
    uint64_t used = 0, budget = 0;
    for (uint32_t h = 0; h < mp.memoryProperties.memoryHeapCount; h++)
        if (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            used += bp.heapUsage[h], budget += bp.heapBudget[h];
    char b[96];
    snprintf(b, sizeof b, ", mémoire vidéo %.0f / %.0f Mo", used / 1048576.0, budget / 1048576.0);
    return b;
}
// Diagnostic : objet texture → image Vulkan partagée, adresse mémoire
void dbg_tex(const char* what, int idx, uint64_t texkey) {
    if (!g.dbg) return;
    nvn::Tex t;
    bool ok = texkey && nvn::g_tex.find_key(texkey, &t);
    auto al = g.alias.find(texkey);
    uint64_t ck = al != g.alias.end() ? al->second : texkey;
    auto it = g.images.find(ck);
    HLOG("vkdbg", "  %s %d : objet %llx -> image %llx%s @%llx %dx%d fmt %u", what, idx, (unsigned long long)texkey,
         (unsigned long long)ck, it == g.images.end() ? " (absente)" : "", ok ? (unsigned long long)t.addr : 0ull,
         ok ? t.b.w : 0, ok ? t.b.h : 0, ok ? t.b.format : 0);
}

Image* get_image(uint64_t texkey) {
    if (!texkey) return nullptr;
    auto it = g.images.find(texkey);
    if (it != g.images.end()) {
        it->second.last_use = g.cur_frame;
        return &it->second;
    }
    auto al = g.alias.find(texkey);
    if (al != g.alias.end()) {
        auto ai = g.images.find(al->second);
        if (ai != g.images.end()) {
            ai->second.last_use = g.cur_frame;
            return &ai->second;
        }
        g.alias.erase(al);   // image détruite entre-temps
    }
    nvn::Tex t;
    if (!nvn::g_tex.find_key(texkey, &t)) return nullptr;
    auto bai = t.addr ? g.by_addr.find(t.addr) : g.by_addr.end();
    if (bai != g.by_addr.end()) {   // même mémoire qu'une image existante ?
        static const bool alias_packaged = getenv("RECOMP_ALIAS_PACKAGED") != nullptr;
        const VkFormat nf = nvn_format(t.b.format).vk;
        const auto& cands = bai->second;
        for (size_t ci = cands.size(); ci-- > 0;) {   // la plus récente d'abord
            const uint64_t okey = cands[ci];
            auto oit = g.images.find(okey);
            if (oit == g.images.end()) continue;
            Image& o = oit->second;
            const bool same_shape =
                o.w == (uint32_t)std::max(1, t.b.w) && o.h == (uint32_t)std::max(1, t.b.h) && o.target == t.b.target;
            if (!same_shape) continue;
            // Deux textures « packagées » (contenu écrit par le processeur) à la même adresse sont deux contenus
            // successifs : le jeu a déchargé un paquet et chargé le suivant dans la même mémoire. Les partager
            // sans contrôle montrait l'ancien contenu (fond de l'entraînement remplacé par une texture de nuages).
            // RECOMP_ALIAS_PACKAGED=1 : ancien comportement (diagnostic).
            if (o.packaged && t.b.packaged && !alias_packaged) {
                // Même format et même disposition : la même image sert, et son contenu est renvoyé seulement si
                // la mémoire a changé depuis l'envoi (empreinte). Sinon, autre image (candidate suivante ou nouvelle).
                const bool arr = t.b.target == 3 || t.b.target == 4 || t.b.target == 6 || t.b.target == 8 || t.b.target == 9;
                const uint32_t levels = std::max(1u, std::min<uint32_t>(t.b.levels, 16));
                const uint32_t layers = t.b.target == 8 ? 6u : arr ? (uint32_t)std::max(1, t.b.d) : 1u;
                if (o.nvnfmt != t.b.format || o.levels != levels || o.layers != layers || o.layout0 != t.b.layout[0]) continue;
                g.alias[texkey] = okey;
                ref_image(o);
                o.last_use = g.cur_frame;
                if (o.initialized && o.src_bytes && mem_valid(t.addr, o.src_bytes) &&
                    hash_mem((const uint8_t*)t.addr, o.src_bytes) != o.content_hash) {
                    memcpy(o.swz, t.b.swizzle, 4);
                    o.initialized = false;   // renvoyé au prochain usage (image_to_general)
                    g.tex_reuploads++;
                }
                return &o;
            }
            // Une texture packagée sur la mémoire d'une cible de rendu : nouveau contenu écrit par le processeur,
            // à envoyer (l'image de la cible garderait l'ancien).
            if (t.b.packaged && !o.packaged && !alias_packaged) continue;
            // Contenu écrit par le GPU (cible de rendu relue comme texture par un autre objet) : partage, si la
            // disposition en mémoire est la même. Profondeur ↔ couleur, autre taille de texel, autre compression :
            // la mémoire sert à une autre ressource. Partager servait l'ancienne image dans son ancien format (une
            // image de profondeur comme cible couleur : GPU perdu au début de l'entraînement, portrait 306×265 de
            // la fenêtre de dialogue ; une texture BC1 lue dans une image BC7 d'une scène précédente).
            if (!same_layout(o.fmt, nf)) {
                static int nw = 0;
                if (nw++ < 16)
                    HLOG("vk", "adresse %llx : format %u puis %u (%ux%u), dispositions différentes : image distincte",
                         (unsigned long long)t.addr, o.nvnfmt, t.b.format, o.w, o.h);
                continue;
            }
            if (o.nvnfmt != t.b.format) {
                static int nw = 0;
                if (nw++ < 8) HLOG("vk", "alias %llx : format %u réinterprété en %u (%ux%u)", (unsigned long long)t.addr,
                                   o.nvnfmt, t.b.format, o.w, o.h);
            }
            g.alias[texkey] = okey;
            ref_image(o);
            o.last_use = g.cur_frame;
            return &o;
        }
    }
    FmtInfo fi = nvn_format(t.b.format);
    if (fi.vk == VK_FORMAT_UNDEFINED) {
        static std::set<uint32_t> warned;
        if (warned.insert(t.b.format).second) HLOG("vk", "format NVN %u non pris en charge", t.b.format);
        return nullptr;
    }
    Image im;
    im.fmt = fi.vk;
    im.nvnfmt = t.b.format;
    im.target = t.b.target;
    im.w = std::max(1, t.b.w), im.h = std::max(1, t.b.h);
    bool arr = t.b.target == 3 || t.b.target == 4 || t.b.target == 6 || t.b.target == 8 || t.b.target == 9;
    bool is3d = t.b.target == 2;
    im.layers = arr ? std::max(1, t.b.d) : 1;
    if (t.b.target == 8) im.layers = 6;
    im.levels = std::max(1u, std::min<uint32_t>(t.b.levels, 16));
    im.addr = t.addr;
    im.packaged = t.b.packaged;
    im.layout0 = t.b.layout[0];
    memcpy(im.swz, t.b.swizzle, 4);
    im.bpu = std::max<uint32_t>(fi.bpp, 1);
    im.aspect = is_depth(fi.vk) ? (VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(fi.vk) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                : VK_IMAGE_ASPECT_COLOR_BIT;
    if (fi.vk == VK_FORMAT_S8_UINT) im.aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = is3d ? VK_IMAGE_TYPE_3D : (t.b.target == 0 || t.b.target == 3 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D);
    ci.format = fi.vk;
    ci.extent = {im.w, ci.imageType == VK_IMAGE_TYPE_1D ? 1u : im.h, is3d ? (uint32_t)std::max(1, t.b.d) : 1u};
    ci.mipLevels = im.levels;
    ci.arrayLayers = im.layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               (is_depth(fi.vk) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                : is_compressed(fi.vk) ? 0 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (!is_depth(fi.vk) && !is_compressed(fi.vk)) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(g.pd, fi.vk, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT, im.storage = true;
    }
    if (t.b.target == 8 || t.b.target == 9) ci.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (vkCreateImage(g.dev, &ci, nullptr, &im.img) != VK_SUCCESS) {
        HLOG("vk", "création d'image %ux%u fmt %u impossible", im.w, im.h, t.b.format);
        return nullptr;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.dev, im.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (VkResult ar = vkAllocateMemory(g.dev, &ai, nullptr, &im.mem); ar != VK_SUCCESS) {
        // échec autrefois silencieux : la texture manquait (descripteur factice, icônes en carrés pleins)
        static int nf = 0;
        if (nf++ < 16)
            HLOG("vk", "mémoire vidéo refusée (%d) pour une image %ux%u fmt %u (%.1f Mo)%s", (int)ar, im.w, im.h,
                 t.b.format, mr.size / 1048576.0, vram_text().c_str());
        vkDestroyImage(g.dev, im.img, nullptr);
        return nullptr;
    }
    vkBindImageMemory(g.dev, im.img, im.mem, 0);
    im.bytes = mr.size;
    im.refs = 1;   // l'objet texture qui la crée
    im.last_use = g.cur_frame;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = is3d ? VK_IMAGE_VIEW_TYPE_3D : (ci.imageType == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    vi.format = fi.vk;
    vi.subresourceRange = {is_depth(fi.vk) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : im.aspect, 0, 1, 0, im.layers};
    if (fi.vk == VK_FORMAT_S8_UINT) vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!is3d && im.layers == 1) vi.viewType = ci.imageType == VK_IMAGE_TYPE_1D ? VK_IMAGE_VIEW_TYPE_1D : VK_IMAGE_VIEW_TYPE_2D;
    vkCreateImageView(g.dev, &vi, nullptr, &im.view);
    auto& r = g.images[texkey] = im;
    if (t.addr) g.by_addr[t.addr].push_back(texkey);
    return &r;
}

// Vue de rendu d'un niveau de mipmap et d'une couche (vue NVN passée à SetRenderTargets). Niveau 0 et couche
// 0 : la vue complète, comme avant. Les volumes 3D (tranche) ne sont pas gérés : vue complète.
VkImageView render_view(Image& im, uint32_t level, uint32_t layer) {
    if (level >= im.levels) level = 0;
    if (layer >= im.layers) layer = 0;
    if ((level == 0 && layer == 0) || im.target == 2) return im.view;
    const uint64_t key = (1ull << 62) | ((uint64_t)level << 16) | layer;   // hors de l'espace des vues d'échantillonnage
    auto it = im.views.find(key);
    if (it != im.views.end()) return it->second;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = im.fmt;
    vi.subresourceRange = {im.aspect, level, 1, layer, 1};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(g.dev, &vi, nullptr, &v) != VK_SUCCESS) v = im.view;
    static int nlog = 0;
    if (nlog++ < 12)
        HLOG("vk", "vue de rendu : %ux%u fmt %u niveau %u couche %u (%ux%u)", im.w, im.h, im.nvnfmt, level, layer,
             std::max(1u, im.w >> level), std::max(1u, im.h >> level));
    return im.views[key] = v;
}

// vue d'échantillonnage d'un type donné (0 1D, 1 2D, 2 3D, 3 Cube, 4 1DArray, 5 2DArray, 6 CubeArray),
// selon la vue NVN enregistrée avec la texture dans le pool (nvnTexturePoolRegisterTexture) : permutation,
// plage de niveaux et de couches, mode profondeur / stencil. Elle était ignorée : la texture par défaut
// « noire » du jeu (blanche permutée en 0, 0, 0, 1) se lisait blanche, et le texte devenait des pavés.
VkImageView sample_view(Image& im, uint32_t type, const nvn::ViewInfo* nv = nullptr) {
    static const bool no_view = getenv("RECOMP_NO_TEX_VIEW") != nullptr;   // diagnostic : vues ignorées
    if (no_view) nv = nullptr;
    uint64_t vkey = type;
    if (nv && (nv->flags & (4 | 8 | 16 | 32))) {
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](uint64_t x) { h = (h ^ x) * 1099511628211ull; };
        mix(nv->flags & (4 | 8 | 16 | 32));
        if (nv->flags & 4) mix(nv->base_level), mix(nv->num_levels);
        if (nv->flags & 8) mix(nv->min_layer), mix(nv->num_layers);
        if (nv->flags & 16) mix(nv->swizzle[0] | nv->swizzle[1] << 8 | nv->swizzle[2] << 16 | (uint32_t)nv->swizzle[3] << 24);
        if (nv->flags & 32) mix(nv->ds_mode);
        vkey |= h << 3;
    }
    auto it = im.views.find(vkey);
    if (it != im.views.end()) return it->second;
    static const VkImageViewType vt[] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_3D,
                                         VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                         VK_IMAGE_VIEW_TYPE_CUBE_ARRAY};
    bool is3d = im.target == 2;
    if ((type == 2) != is3d) return im.views[vkey] = VK_NULL_HANDLE;
    if ((type == 3 || type == 6) && im.layers < 6) return im.views[vkey] = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = vt[type];
    vi.format = im.fmt;
    uint32_t base_level = 0, levels = im.levels, base_layer = 0;
    uint32_t layers = (type == 1 || type == 0) ? 1 : (type == 3 ? 6 : im.layers);
    if (nv && (nv->flags & 4) && nv->base_level < im.levels) {
        base_level = nv->base_level;
        levels = std::max(1u, std::min(nv->num_levels ? nv->num_levels : im.levels, im.levels - base_level));
    }
    if (nv && (nv->flags & 8) && nv->min_layer < im.layers && !is3d) {
        base_layer = nv->min_layer;
        const uint32_t avail = im.layers - base_layer;
        const uint32_t want = nv->num_layers ? nv->num_layers : avail;
        layers = (type == 1 || type == 0) ? 1 : (type == 3 ? std::min(6u, avail) : std::max(1u, std::min(want, avail)));
        if ((type == 3 && layers < 6) || (type == 6 && layers % 6)) return im.views[vkey] = VK_NULL_HANDLE;
    }
    VkImageAspectFlags aspect = is_depth(im.fmt) ? (VkImageAspectFlags)VK_IMAGE_ASPECT_DEPTH_BIT : im.aspect;
    if (im.fmt == VK_FORMAT_S8_UINT || (nv && (nv->flags & 32) && nv->ds_mode == 1 && has_stencil(im.fmt)))
        aspect = VK_IMAGE_ASPECT_STENCIL_BIT;   // NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL
    vi.subresourceRange = {aspect, base_level, levels, base_layer, layers};
    if (!is_depth(im.fmt)) {   // permutation : 0 zéro, 1 un, 2 R, 3 G, 4 B, 5 A (celle de la vue prime)
        auto cs = [](uint8_t s) { return s <= 5 ? (VkComponentSwizzle)(s + 1) : VK_COMPONENT_SWIZZLE_IDENTITY; };
        const uint8_t* sw = nv && (nv->flags & 16) ? nv->swizzle : im.swz;
        vi.components = {cs(sw[0]), cs(sw[1]), cs(sw[2]), cs(sw[3])};
    }
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(g.dev, &vi, nullptr, &v);
    return im.views[vkey] = v;
}

// ================================================================ échantillonneurs
// Énumérations NVN (ordre GL, base 0), établies sur les 11 échantillonneurs du jeu (RECOMP_SAMP_LOG) :
// min 0 NEAREST, 1 LINEAR, 2 NEAREST_MIPMAP_NEAREST, 3 LINEAR_MIPMAP_NEAREST, 4 NEAREST_MIPMAP_LINEAR,
// 5 LINEAR_MIPMAP_LINEAR ; mag 0 NEAREST, 1 LINEAR. Le jeu n'emploie que (5,1) trilinéaire, (3,1) bilinéaire
// (LUT, post-traitement, ombres) et (2,0) au plus proche. L'ancienne table (base 1) inversait tout : les LUT
// 3D lues au plus proche donnaient les anneaux en paliers, les textures linéaires étaient pixelisées.
VkFilter filt(uint32_t f) { return (f == 1 || f == 3 || f == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; }
// NVN : 0 CLAMP, 1 REPEAT, 2 MIRROR_CLAMP, 3 MIRROR_CLAMP_TO_EDGE, 4 MIRROR_CLAMP_TO_BORDER, 5 CLAMP_TO_BORDER,
// 6 MIRRORED_REPEAT, 7 CLAMP_TO_EDGE (le jeu emploie 0, 1, 5 et surtout 7)
VkSamplerAddressMode wrap(uint32_t w) {
    switch (w) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;   // CLAMP (GL ancien) : approché
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return getenv("RECOMP_NO_BORDER") ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}
VkSampler get_sampler(const nvn::SamplerInfo& s) {
    uint64_t h = std::hash<std::string>()(std::string((const char*)&s, sizeof s));
    auto it = g.samplers.find(h);
    if (it != g.samplers.end()) return it->second;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    static const bool old_filt = getenv("RECOMP_OLD_FILTERS") != nullptr;   // diagnostic : ancienne table inversée
    ci.magFilter = (s.mag == 1) != old_filt ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;   // NVN : 0 NEAREST, 1 LINEAR
    ci.minFilter = old_filt ? (filt(s.min) == VK_FILTER_LINEAR ? VK_FILTER_NEAREST : VK_FILTER_LINEAR) : filt(s.min);
    ci.mipmapMode = (s.min == 5 || s.min == 4) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap(s.wrap[0]);
    ci.addressModeV = wrap(s.wrap[1]);
    ci.addressModeW = wrap(s.wrap[2]);
    ci.mipLodBias = s.lod_bias;
    ci.maxAnisotropy = std::max(1.0f, std::min(16.0f, s.max_aniso));
    ci.anisotropyEnable = s.max_aniso > 1.0f;
    ci.minLod = s.min_lod;
    ci.maxLod = s.max_lod;
    if (s.min <= 1 && !old_filt) ci.minLod = 0, ci.maxLod = 0.25f;   // filtre sans mipmap : niveau de base seul (GL)
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    static const bool no_cmp = getenv("RECOMP_NO_COMPARE") != nullptr;   // diagnostic
    if (s.compare_mode && !no_cmp) {   // nvnSamplerBuilderSetCompare : lectures d'ombre (TEX.DC / sampler*Shadow)
        static const VkCompareOp ops[] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                          VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                          VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
        ci.compareEnable = VK_TRUE;
        ci.compareOp = (s.compare_func >= 1 && s.compare_func <= 8) ? ops[s.compare_func - 1] : VK_COMPARE_OP_LESS_OR_EQUAL;
    }
    static const bool slog = getenv("RECOMP_SAMP_LOG") != nullptr;   // paramètres NVN de chaque échantillonneur créé
    if (slog && g.samplers.size() < 300)
        HLOG("samp", "min %u mag %u enveloppe %u %u %u comparaison %u/%u réduction %u lod %g..%g biais %g aniso %g", s.min, s.mag,
             s.wrap[0], s.wrap[1], s.wrap[2], s.compare_mode, s.compare_func, s.reduction, s.min_lod, s.max_lod, s.lod_bias,
             s.max_aniso);
    VkSampler smp = VK_NULL_HANDLE;
    VkResult r = vkCreateSampler(g.dev, &ci, nullptr, &smp);
    if (r != VK_SUCCESS) {
        static int nf = 0;
        if (nf++ < 10) HLOG("vk", "création d'échantillonneur impossible (%d) après %zu échantillonneurs", (int)r, g.samplers.size());
    }
    if (g.samplers.size() % 500 == 499) HLOG("vk", "%zu échantillonneurs Vulkan créés", g.samplers.size() + 1);
    return g.samplers[h] = smp;
}

// Écrit dans le jeu de textures les descripteurs des poignées liées (texture et échantillonneur).
void bind_tex_id(uint32_t tid, uint64_t texpool, const std::vector<std::string>& types, uint64_t handle);
void bind_samp_id(uint32_t sid, uint64_t samppool);
void bind_handle(uint64_t handle, uint64_t texpool, uint64_t samppool, const std::vector<std::string>& types) {
    bind_tex_id(handle & 0xfffff, texpool, types, handle);
    bind_samp_id((handle >> 20) & 0xfff, samppool);
}
void bind_tex_id(uint32_t tid, uint64_t texpool, const std::vector<std::string>& types, uint64_t handle) {
    nvn::TexDesc td;
    bool have_t = false;
    {
        std::lock_guard<std::mutex> lk(nvn::g_desc_lk);
        nvn::g_texpool.with_key(texpool, [&](const nvn::TexPool& p) {   // sans copier le pool entier
            auto it = p.desc.find(tid);
            if (it != p.desc.end()) td = it->second, have_t = true;
        });
    }
    if (g.dbg) HLOG("vkdbg", "  poignée %llx : texture %u %s", (unsigned long long)handle, tid, have_t ? "" : "(absente du pool)");
    if (tid >= TEX_MAX) {
        static int nw = 0;
        if (nw++ < 20)
            HLOG("vk", "identifiant de texture %u ≥ %u (poignée %llx, %s du pool)", tid, TEX_MAX, (unsigned long long)handle,
                 have_t ? "présent" : "absent");
    }
    if (have_t && tid < TEX_MAX) {
        Image* im = get_image(td.tex);
        dbg_tex("tex", (int)tid, td.tex);
        if (im) {
            image_to_general(*im);
            static const char* names[] = {"1d", "2d", "3d", "cube", "1darray", "2darray", "cubearray"};
            for (auto& ty : types)
                for (uint32_t k = 0; k < 7; k++)
                    if (ty == names[k]) {
                        static const bool no3d = getenv("RECOMP_NO_3D_TEX") != nullptr;   // diagnostic
                        if (no3d && k == 2) continue;
                        VkImageView v = sample_view(*im, k, &td.view);
                        if (!v) continue;
                        uint64_t key = (uint64_t)v;
                        auto& w = g.tex_written[tid * 8 + k];
                        if (w == key) continue;
                        {   // RECOMP_TEX_SLOT_WATCH=id : réécritures d'un emplacement du jeu de textures
                            static const long ws_id = getenv("RECOMP_TEX_SLOT_WATCH") ? atol(getenv("RECOMP_TEX_SLOT_WATCH")) : -1;
                            static int nws = 0;
                            if ((long)tid == ws_id && nws++ < 40)
                                HLOG("watch", "emplacement %u type %s : image %ux%u×%u fmt %u cible %u niveaux %u (objet %llx, pool %llx, image %s, vue %llx → %llx) ; vue NVN drapeaux %u permutation %u %u %u %u, permutation de la texture %u %u %u %u",
                                     tid, names[k], im->w, im->h, im->layers, im->nvnfmt, im->target, im->levels,
                                     (unsigned long long)td.tex, (unsigned long long)texpool, im->initialized ? "initialisée" : "neuve",
                                     (unsigned long long)w, (unsigned long long)key, td.view.flags, td.view.swizzle[0],
                                     td.view.swizzle[1], td.view.swizzle[2], td.view.swizzle[3], im->swz[0], im->swz[1], im->swz[2],
                                     im->swz[3]);
                        }
                        w = key;
                        VkDescriptorImageInfo ii{VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
                        VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        ws.dstSet = g.texset;
                        ws.dstBinding = k;
                        ws.dstArrayElement = tid;
                        ws.descriptorCount = 1;
                        ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        ws.pImageInfo = &ii;
                        vkUpdateDescriptorSets(g.dev, 1, &ws, 0, nullptr);
                    }
        }
    }
}
void bind_samp_id(uint32_t sid, uint64_t samppool) {
    nvn::SamplerInfo si;
    bool have_s = false;
    {
        std::lock_guard<std::mutex> lk(nvn::g_desc_lk);
        nvn::g_samppool.with_key(samppool, [&](const nvn::SampPool& p) {
            auto it = p.desc.find(sid);
            if (it != p.desc.end()) si = it->second, have_s = true;
        });
    }
    if (sid < SAMP_MAX) {
        {   // RECOMP_SAMP_WATCH=id : paramètres NVN d'un échantillonneur
            static const long sw = getenv("RECOMP_SAMP_WATCH") ? atol(getenv("RECOMP_SAMP_WATCH")) : -1;
            static int nsw = 0;
            if ((long)sid == sw && have_s && nsw++ < 4)
                HLOG("watch", "échantillonneur %u : min %u mag %u enveloppe %u %u %u comparaison %u/%u réduction %u lod %g..%g biais %g aniso %g bord %g %g %g %g",
                     sid, si.min, si.mag, si.wrap[0], si.wrap[1], si.wrap[2], si.compare_mode, si.compare_func, si.reduction,
                     si.min_lod, si.max_lod, si.lod_bias, si.max_aniso, si.border[0], si.border[1], si.border[2], si.border[3]);
        }
        VkSampler smp = get_sampler(have_s ? si : nvn::SamplerInfo{});
        auto& w = g.samp_written[sid];
        if (w != (uint64_t)smp) {
            {
                static const long sw = getenv("RECOMP_SAMP_WATCH") ? atol(getenv("RECOMP_SAMP_WATCH")) : -1;
                static int nrw = 0;
                if ((long)sid == sw && nrw++ < 40)
                    HLOG("watch", "emplacement d'échantillonneur %u réécrit (image %llu, pool %llx, %s, comparaison %u, enveloppe %u) : %llx → %llx",
                         sid, (unsigned long long)g.cur_frame + 1, (unsigned long long)samppool, have_s ? "présent" : "ABSENT → défaut",
                         si.compare_mode, si.wrap[0], (unsigned long long)w, (unsigned long long)(uint64_t)smp);
            }
            w = (uint64_t)smp;
            VkDescriptorImageInfo ii{smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
            VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            ws.dstSet = g.texset;
            ws.dstBinding = 8;
            ws.dstArrayElement = sid;
            ws.descriptorCount = 1;
            ws.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            ws.pImageInfo = &ii;
            vkUpdateDescriptorSets(g.dev, 1, &ws, 0, nullptr);
        }
    }
}

// ================================================================ shaders
std::string exe_dir() {   // dossier de l'exécutable, avec la barre finale
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string d = exe;
    return d.substr(0, d.find_last_of("\\/") + 1);
}
std::string cache_dir() { return exe_dir() + "shader_cache\\"; }

// modèle d'exécution du premier OpEntryPoint (0 sommets, 4 fragments, 5 calcul), -1 si illisible
int spirv_model(const std::vector<char>& code) {
    const uint32_t* w = (const uint32_t*)code.data();
    const size_t n = code.size() / 4;
    if (n < 5 || w[0] != 0x07230203) return -1;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len) break;
        if (op == 15 && i + 1 < n) return (int)w[i + 1];
        i += len;
    }
    return -1;
}

ShaderMod* get_shader(const std::string& hash) {
    auto it = g.shaders.find(hash);
    if (it != g.shaders.end()) return it->second.ok ? &it->second : nullptr;
    ShaderMod& sm = g.shaders[hash];
    sm.hash = hash;
    std::string base = cache_dir() + hash;
    std::ifstream f(base + ".spv", std::ios::binary);
    std::ifstream mf(base + ".meta");
    if (!f || !mf) {
        if (g.missing.insert(hash).second && g.missing.size() <= 50)
            HLOG("vk", "shader %s absent du cache (lancer shaders/shaderc.py)", hash.c_str());
        return nullptr;
    }
    std::vector<char> code((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    std::string line;
    while (std::getline(mf, line)) {
        std::istringstream ss(line);
        std::string key;
        ss >> key;
        if (key == "banks") for (int v; ss >> v;) sm.banks.push_back(v);
        else if (key == "vin") for (int v; ss >> v;) sm.vin.push_back(v);
        else if (key == "fout") for (int v; ss >> v;) sm.fout.push_back(v);
        else if (key == "tex") for (std::string v; ss >> v;) sm.tex.push_back(v);
        else if (key == "local") ss >> sm.local[0] >> sm.local[1] >> sm.local[2];
        else if (key == "glob") sm.glob = true;
        else if (key == "c0") {
            ss >> sm.c0[0] >> sm.c0[1] >> sm.c0[2];
            for (int k = 3; k < 7; k++)
                if (!(ss >> sm.c0[k])) sm.c0[k] = 0;
        }
        else if (key == "texh")
            for (std::string v; ss >> v;) {   // banque:déplacement
                size_t a = v.find(':');
                if (a != std::string::npos)
                    sm.texh.push_back({atoi(v.substr(0, a).c_str()), (uint32_t)strtoul(v.substr(a + 1).c_str(), nullptr, 0)});
            }
        else if (key == "img")
            for (std::string v; ss >> v;) {   // banque:déplacement:dim
                size_t a = v.find(':'), b = v.find(':', a + 1);
                if (a == std::string::npos || b == std::string::npos) continue;
                sm.img.push_back({atoi(v.substr(0, a).c_str()), (uint32_t)strtoul(v.substr(a + 1, b - a - 1).c_str(), nullptr, 0),
                                  v.substr(b + 1)});
            }
    }
    sm.model = spirv_model(code);
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = code.size();
    ci.pCode = (const uint32_t*)code.data();
    if (vkCreateShaderModule(g.dev, &ci, nullptr, &sm.mod) != VK_SUCCESS) return nullptr;
    sm.ok = true;
    return &sm;
}

// ================================================================ pipelines
VkBlendFactor bfac(uint8_t f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 0x61: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x62: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x63: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x64: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp bop(uint8_t e) {
    switch (e) {
    case 2: return VK_BLEND_OP_SUBTRACT;
    case 3: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 4: return VK_BLEND_OP_MIN;
    case 5: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
VkCompareOp cmpop(uint8_t f) {
    switch (f) {
    case 1: return VK_COMPARE_OP_NEVER;
    case 2: return VK_COMPARE_OP_LESS;
    case 3: return VK_COMPARE_OP_EQUAL;
    case 4: return VK_COMPARE_OP_LESS_OR_EQUAL;
    case 5: return VK_COMPARE_OP_GREATER;
    case 6: return VK_COMPARE_OP_NOT_EQUAL;
    case 7: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
VkStencilOp stop(uint8_t o) {
    switch (o) {
    case 2: return VK_STENCIL_OP_ZERO;
    case 3: return VK_STENCIL_OP_REPLACE;
    case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 6: return VK_STENCIL_OP_INVERT;
    case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
VkPrimitiveTopology topo(uint64_t p) {
    switch (p) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 14: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}


// ================================================================ instantané PNG (diagnostic)
uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0) {
    static uint32_t t[256];
    if (!t[1])
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t v = i;
            for (int k = 0; k < 8; k++) v = v & 1 ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            t[i] = v;
        }
    c = ~c;
    for (size_t i = 0; i < n; i++) c = t[(c ^ d[i]) & 0xff] ^ (c >> 8);
    return ~c;
}
void write_png(const char* path, const uint8_t* rgba, uint32_t w, uint32_t h) {
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(w * 3 + 1) * h);
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; x++) raw.insert(raw.end(), rgba + 4 * ((size_t)y * w + x), rgba + 4 * ((size_t)y * w + x) + 3);
    }
    std::vector<uint8_t> z = {0x78, 0x01};   // zlib, blocs non compressés
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) a = (a + v) % 65521, b = (b + a) % 65521;
    for (size_t i = 0; i < raw.size(); i += 65535) {
        uint16_t n = (uint16_t)std::min<size_t>(65535, raw.size() - i);
        z.push_back(i + n >= raw.size() ? 1 : 0);
        z.push_back(n & 0xff), z.push_back(n >> 8), z.push_back(~n & 0xff), z.push_back((~n >> 8) & 0xff);
        z.insert(z.end(), raw.begin() + i, raw.begin() + i + n);
    }
    uint32_t ad = (b << 16) | a;
    for (int k = 3; k >= 0; k--) z.push_back((uint8_t)(ad >> (8 * k)));
    FILE* f = fopen(path, "wb");
    if (!f) return;
    auto chunk = [&](const char* ty, const std::vector<uint8_t>& d) {
        uint8_t len[4] = {(uint8_t)(d.size() >> 24), (uint8_t)(d.size() >> 16), (uint8_t)(d.size() >> 8), (uint8_t)d.size()};
        fwrite(len, 1, 4, f);
        std::vector<uint8_t> td(ty, ty + 4);
        td.insert(td.end(), d.begin(), d.end());
        fwrite(td.data(), 1, td.size(), f);
        uint32_t c = crc32(td.data(), td.size());
        uint8_t cb[4] = {(uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c};
        fwrite(cb, 1, 4, f);
    };
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    std::vector<uint8_t> ihdr = {(uint8_t)(w >> 24), (uint8_t)(w >> 16), (uint8_t)(w >> 8), (uint8_t)w,
                                 (uint8_t)(h >> 24), (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h, 8, 2, 0, 0, 0};
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    fclose(f);
}

// Diagnostic (RECOMP_TEX_DUMP=N, image N) : mémoire brute des textures « packagées », une par
// combinaison dimensions / format / octet de disposition, dans build/full/tex_dump/ (données du jeu :
// dossier privé). Décodage hors ligne avec la même disposition : shaders/texview.py.
void tex_dump() {
    CreateDirectoryA("tex_dump", nullptr);
    std::vector<std::pair<uint64_t, nvn::Tex>> list;
    {
        std::lock_guard<std::mutex> lk(nvn::g_tex.m);
        for (auto& [k, t] : nvn::g_tex.map)
            if (t.b.packaged && t.addr) list.push_back({k, t});
    }
    std::set<std::tuple<int, int, uint32_t, uint32_t, uint8_t>> seen;
    int n = 0;
    for (auto& [k, t] : list) {
        if (!seen.insert({t.b.w, t.b.h, t.b.format, t.b.levels, t.b.layout[0]}).second) continue;
        FmtInfo fi = nvn_format(t.b.format);
        bool bc = is_compressed(fi.vk);
        uint64_t bw = bc ? (uint64_t)(t.b.w + 3) / 4 : (uint64_t)t.b.w, bh = bc ? (uint64_t)(t.b.h + 3) / 4 : (uint64_t)t.b.h;
        uint64_t sz = bw * bh * std::max<uint32_t>(fi.bpp, 1) * 2 + 0x40000;
        while (sz > 0x1000 && !mem_valid(t.addr, sz)) sz /= 2;
        if (!mem_valid(t.addr, sz)) continue;
        char path[160];
        snprintf(path, sizeof path, "tex_dump/%016llx_%dx%d_f%u_n%u_L%02x%02x%02x%02x%02x%02x%02x%02x.bin", (unsigned long long)k,
                 t.b.w, t.b.h, t.b.format, t.b.levels, t.b.layout[0], t.b.layout[1], t.b.layout[2], t.b.layout[3],
                 t.b.layout[4], t.b.layout[5], t.b.layout[6], t.b.layout[7]);
        if (FILE* f = fopen(path, "wb")) {
            fwrite((const void*)t.addr, 1, sz, f);
            fclose(f);
            n++;
        }
    }
    HLOG("vk", "textures vidées : %d (sur %zu packagées) dans tex_dump/", n, list.size());
}

// Copie la texture présentée (RGBA8) dans un PNG réduit de moitié : build/full/snapshot_<n>.png
// Réduction faite par le GPU (blit linéaire) dans une image persistante, relue dans un tampon
// mémoire « mise en cache » réutilisé : ≈ 1 ms par instantané. L'ancienne version allouait et
// relisait l'image pleine taille en mémoire non cachée (≈ 0,4 s : 2,5 img/s avec SNAPSHOT_EVERY=4).
struct SnapRes {
    uint32_t w = 0, h = 0;
    VkFormat fmt = VK_FORMAT_UNDEFINED;
    VkImage img = VK_NULL_HANDLE;
    VkDeviceMemory imem = VK_NULL_HANDLE;
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory bmem = VK_NULL_HANDLE;
    uint8_t* ptr = nullptr;
    bool coherent = true;
} g_snap;
bool snap_resources(uint32_t w, uint32_t h, VkFormat fmt) {
    if (g_snap.img && g_snap.w == w && g_snap.h == h && g_snap.fmt == fmt) return true;
    if (g_snap.img) {   // taille changée : on recrée (après l'attente de l'image précédente)
        vkDestroyImage(g.dev, g_snap.img, nullptr);
        vkFreeMemory(g.dev, g_snap.imem, nullptr);
        vkUnmapMemory(g.dev, g_snap.bmem);
        vkDestroyBuffer(g.dev, g_snap.buf, nullptr);
        vkFreeMemory(g.dev, g_snap.bmem, nullptr);
        g_snap = SnapRes{};
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(g.dev, &ci, nullptr, &g_snap.img) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.dev, g_snap.img, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.dev, &ai, nullptr, &g_snap.imem) != VK_SUCCESS) return false;
    vkBindImageMemory(g.dev, g_snap.img, g_snap.imem, 0);
    // tampon de relecture : mémoire hôte mise en cache (lecture processeur rapide)
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (uint64_t)w * h * 4;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(g.dev, &bi, nullptr, &g_snap.buf) != VK_SUCCESS) return false;
    vkGetBufferMemoryRequirements(g.dev, g_snap.buf, &mr);
    uint32_t t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    g_snap.coherent = t != UINT32_MAX;
    if (t == UINT32_MAX) t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (t == UINT32_MAX) t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
                         g_snap.coherent = true;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = t;
    if (vkAllocateMemory(g.dev, &ai, nullptr, &g_snap.bmem) != VK_SUCCESS) return false;
    vkBindBufferMemory(g.dev, g_snap.buf, g_snap.bmem, 0);
    vkMapMemory(g.dev, g_snap.bmem, 0, VK_WHOLE_SIZE, 0, (void**)&g_snap.ptr);
    g_snap.w = w, g_snap.h = h, g_snap.fmt = fmt;
    return true;
}
// Relit l'image présentée réduite de moitié (blit GPU) dans g_snap.ptr (RGBA, lignes de haut en bas).
bool readback_half(Image& im, const int32_t* crop, uint32_t& w2, uint32_t& h2) {
    if (im.fmt != VK_FORMAT_R8G8B8A8_SRGB && im.fmt != VK_FORMAT_R8G8B8A8_UNORM) return false;
    int32_t cx = 0, cy = 0;
    uint32_t cw = im.w, ch = im.h;
    if (crop && crop[2] > 0 && crop[3] > 0)
        cx = crop[0], cy = crop[1], cw = std::min<uint32_t>(crop[2], im.w - cx), ch = std::min<uint32_t>(crop[3], im.h - cy);
    w2 = std::max(1u, cw / 2), h2 = std::max(1u, ch / 2);
    if (!snap_resources(w2, h2, im.fmt)) return false;
    begin_frame();   // emplacement suivant (après le travail de l'image : barrière de tête)
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.image = g_snap.img;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(g.cb, &di);
    VkImageBlit bl{};
    bl.srcSubresource = bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[0] = {cx, cy, 0};
    bl.srcOffsets[1] = {cx + (int32_t)cw, cy + (int32_t)ch, 1};
    bl.dstOffsets[1] = {(int32_t)w2, (int32_t)h2, 1};
    vkCmdBlitImage(g.cb, im.img, VK_IMAGE_LAYOUT_GENERAL, g_snap.img, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_LINEAR);
    barrier_all();
    VkBufferImageCopy r{};
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.imageExtent = {w2, h2, 1};
    vkCmdCopyImageToBuffer(g.cb, g_snap.img, VK_IMAGE_LAYOUT_GENERAL, g_snap.buf, 1, &r);
    barrier_all();
    flush_locked("capture");   // soumet et attend
    if (!g_snap.coherent) {
        VkMappedMemoryRange mrg{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        mrg.memory = g_snap.bmem;
        mrg.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(g.dev, 1, &mrg);
    }
    return true;
}
void snapshot(Image& im, uint64_t frame, const int32_t* crop) {
    uint32_t w2, h2;
    if (!readback_half(im, crop, w2, h2)) return;
    char path[160];
    static const char* dir = getenv("RECOMP_SNAPSHOT_DIR");   // dossier des instantanés (existant)
    if (dir) snprintf(path, sizeof path, "%s/snapshot_%06llu.png", dir, (unsigned long long)frame);
    else snprintf(path, sizeof path, "snapshot_%llu.png", (unsigned long long)frame);
    write_png(path, g_snap.ptr, w2, h2);
    static const bool quiet = getenv("RECOMP_SNAPSHOT_EVERY") != nullptr;
    if (!quiet) HLOG("vk", "instantané : %s (%ux%u)", path, w2, h2);
}

}  // namespace

// ================================================================ interface (vk_render.h)
bool available() { return g.ok; }

void pools_released(uint64_t addr, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    if (g.ok) pools_released_locked(addr, size);
}

// nvnTextureFinalize : l'objet texture du jeu n'existe plus. Son image perd une référence ; sans référence, elle
// reste réemployable par adresse et entre dans le cache plafonné des images sans objet (evict_unreferenced).
void texture_released(uint64_t texkey) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    if (!g.ok) return;
    uint64_t ik = texkey;
    auto al = g.alias.find(texkey);
    if (al != g.alias.end()) {
        ik = al->second;
        g.alias.erase(al);
    }
    auto it = g.images.find(ik);
    if (it == g.images.end()) return;
    Image& im = it->second;
    if (im.refs && --im.refs == 0) {
        g.unref_bytes += im.bytes;
        g.unref_q.push_back(ik);
        evict_unreferenced();
    }
}

// ================================================================ pipelines précompilés (voir Vk::gpl)
// États dynamiques de tous les pipelines graphiques : les bibliothèques de shaders ne dépendent ainsi ni du
// rejet des faces, ni du sens des faces, ni de la profondeur ou du stencil.
const VkDynamicState DYN_STATES[] = {
    VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
    VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE};
VkPipelineDynamicStateCreateInfo dyn_info() {
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof DYN_STATES / sizeof *DYN_STATES;
    dy.pDynamicStates = DYN_STATES;
    return dy;
}

// ---- cache de pipelines sur disque (build/full/pipelines.cache)
std::string pcache_path() { return exe_dir() + "pipelines.cache"; }
void pcache_open() {
    std::vector<char> data;
    {
        std::ifstream f(pcache_path(), std::ios::binary);
        if (f) data.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();   // données d'un autre GPU ou pilote : ignorées par le pilote (en-tête vérifié)
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(g.dev, &ci, nullptr, &g.pcache) != VK_SUCCESS) {
        ci.initialDataSize = 0, ci.pInitialData = nullptr;
        if (vkCreatePipelineCache(g.dev, &ci, nullptr, &g.pcache) != VK_SUCCESS) g.pcache = VK_NULL_HANDLE;
    }
    HLOG("vk", "cache de pipelines : %s (%.1f Mo)", data.empty() ? "vide" : "chargé", data.size() / 1048576.0);
}
// écrit le cache (fichier temporaire, puis remplacement : jamais de cache à moitié écrit). Sans verrou : le cache de
// pipelines est synchronisé par le pilote.
std::mutex g_pc_file_lk;
bool pcache_write() {
    if (!g.pcache) return false;
    std::lock_guard<std::mutex> lk(g_pc_file_lk);
    size_t n = 0;
    if (vkGetPipelineCacheData(g.dev, g.pcache, &n, nullptr) != VK_SUCCESS || !n) return false;
    std::vector<char> d(n);
    if (vkGetPipelineCacheData(g.dev, g.pcache, &n, d.data()) != VK_SUCCESS) return false;
    const std::string p = pcache_path(), part = p + ".part";
    {
        std::ofstream o(part, std::ios::binary | std::ios::trunc);
        o.write(d.data(), (std::streamsize)n);
        if (!o) return false;
    }
    return MoveFileExA(part.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
// en jeu : sauvegarde sur un autre thread, au plus une par minute, si des pipelines ont été créés depuis la dernière
std::atomic<bool> g_pc_saving{false};
void pcache_save_later() {
    using namespace std::chrono;
    if (!g.pc_dirty || steady_clock::now() - g.pc_saved < seconds(60)) return;
    bool libre = false;
    if (!g_pc_saving.compare_exchange_strong(libre, true)) return;
    g.pc_dirty = 0;
    g.pc_saved = steady_clock::now();
    std::thread([] {
        pcache_write();
        g_pc_saving = false;
    }).detach();
}

// ---- bibliothèques
// (par référence : GCC copiait la structure passée par valeur avec une écriture AVX alignée sur 32 octets, alors que
// Windows n'aligne la pile que sur 16 → plantage ; voir aussi -muse-unaligned-vector-move dans link.py)
VkPipeline make_library(VkGraphicsPipelineCreateInfo& gp, VkGraphicsPipelineLibraryFlagsEXT part) {
    VkGraphicsPipelineLibraryCreateInfoEXT li{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT};
    li.flags = part;
    li.pNext = gp.pNext;
    gp.pNext = &li;
    gp.flags |= VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    gp.layout = g.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g.dev, g.pcache, 1, &gp, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    gp.pNext = li.pNext;
    return p;
}
// avant rastérisation : shader de sommets, fenêtre (contrôle de la profondeur [-1, 1]), mode des polygones
VkPipeline create_vs_library(VkShaderModule m, VkPolygonMode mode, bool neg_one) {
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = VK_SHADER_STAGE_VERTEX_BIT;
    st.module = m;
    st.pName = "main";
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
    dcc.negativeOneToOne = neg_one;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    vps.pNext = g.depth_clip_control ? &dcc : nullptr;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = mode;
    rs.lineWidth = 1.0f;
    VkPipelineRenderingCreateInfo pri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};   // viewMask 0
    VkPipelineDynamicStateCreateInfo dy = dyn_info();
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &pri;
    gp.stageCount = 1;
    gp.pStages = &st;
    gp.pViewportState = &vps;
    gp.pRasterizationState = &rs;
    gp.pDynamicState = &dy;
    return make_library(gp, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT);
}
// fragments : shader de fragments ; profondeur et stencil dynamiques. Sans état multi-échantillon, la bibliothèque
// se lie à toute sortie (alpha → couverture compris).
VkPipeline create_fs_library(VkShaderModule m) {
    VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    st.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st.module = m;
    st.pName = "main";
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.maxDepthBounds = 1.0f;
    VkPipelineRenderingCreateInfo pri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    VkPipelineDynamicStateCreateInfo dy = dyn_info();
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &pri;
    gp.stageCount = 1;
    gp.pStages = &st;
    gp.pDepthStencilState = &dss;
    gp.pDynamicState = &dy;
    return make_library(gp, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT);
}
VkPipeline create_compute_pipeline(VkShaderModule m) {
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = m;
    ci.stage.pName = "main";
    ci.layout = g.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateComputePipelines(g.dev, g.pcache, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}
uint64_t vs_library_key(VkShaderModule m, VkPolygonMode mode, bool neg_one) {
    const uint64_t k[3] = {(uint64_t)m, (uint64_t)mode, (uint64_t)neg_one};
    return fnv(k, sizeof k);
}
// bibliothèques de shaders : précompilées (warm_up) ; sinon compilées ici, une fois (variante rare)
VkPipeline vs_library(ShaderMod* vs, VkPolygonMode mode) {
    const bool neg_one = !nvn::g_depth_zero_one;
    const uint64_t key = vs_library_key(vs->mod, mode, neg_one);
    auto it = g.lib_vs.find(key);
    if (it != g.lib_vs.end()) return it->second;
    VkPipeline p = create_vs_library(vs->mod, mode, neg_one);
    g.lib_vs[key] = p;
    if (g.libs_on_demand++ < 20) HLOG("vk", "shader de sommets %s compilé en jeu (mode %d)", vs->hash.c_str(), (int)mode);
    return p;
}
VkPipeline fs_library(ShaderMod* fs) {
    auto it = g.lib_fs.find((uint64_t)fs->mod);
    if (it != g.lib_fs.end()) return it->second;
    VkPipeline p = create_fs_library(fs->mod);
    g.lib_fs[(uint64_t)fs->mod] = p;
    if (g.libs_on_demand++ < 20) HLOG("vk", "shader de fragments %s compilé en jeu", fs->hash.c_str());
    return p;
}
// Pipeline d'un dessin par bibliothèques : entrées de sommets et sortie (sans shader, rapides à créer, gardées par
// contenu), puis liaison rapide des quatre bibliothèques (gardée par quadruplet).
VkPipeline gpl_pipeline(ShaderMod* vs, ShaderMod* fs, const VkPipelineVertexInputStateCreateInfo& vi, VkPrimitiveTopology tp,
                        VkPolygonMode pm, const VkPipelineColorBlendStateCreateInfo& cbs, const VkPipelineRenderingCreateInfo& pri,
                        bool a2c) {
    // RECOMP_PIPE_TIMING=1 : durée de chaque partie (entrées, sortie, liaison) des 200 premiers pipelines
    static const bool timing = getenv("RECOMP_PIPE_TIMING") != nullptr;
    static int n_timing = 0;
    using hr = std::chrono::high_resolution_clock;
    double t_vi = 0, t_fo = 0, t_link = 0;
    auto t = hr::now();
    auto lap = [&](double& acc) {
        const auto n = hr::now();
        acc += std::chrono::duration<double, std::milli>(n - t).count();
        t = n;
    };
    struct Rapport {
        bool on; double& vi; double& fo; double& link; const ShaderMod* vs; const ShaderMod* fs;
        ~Rapport() {
            if (on && n_timing++ < 200)
                HLOG("pipe", "pipeline %s/%s : entrées %.2f ms, sortie %.2f ms, liaison %.2f ms", vs->hash.c_str(), fs->hash.c_str(), vi, fo, link);
        }
    } rapport{timing, t_vi, t_fo, t_link, vs, fs};
    VkPipeline libs[4] = {};
    {   // entrées de sommets
        uint64_t h = fnv(vi.pVertexAttributeDescriptions, vi.vertexAttributeDescriptionCount * sizeof(VkVertexInputAttributeDescription));
        h = fnv(vi.pVertexBindingDescriptions, vi.vertexBindingDescriptionCount * sizeof(VkVertexInputBindingDescription), h);
        h = fnv(&tp, sizeof tp, h);
        auto it = g.lib_vi.find(h);
        if (it != g.lib_vi.end()) {
            libs[0] = it->second;
        } else {
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = tp;
            VkPipelineDynamicStateCreateInfo dy = dyn_info();
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gp.pVertexInputState = &vi;
            gp.pInputAssemblyState = &ia;
            gp.pDynamicState = &dy;
            libs[0] = g.lib_vi[h] = make_library(gp, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT);
        }
    }
    lap(t_vi);
    libs[1] = vs_library(vs, pm);
    libs[2] = fs_library(fs);
    t = hr::now();
    {   // sortie : formats des cibles, mélange, masques d'écriture, alpha → couverture
        uint64_t h = fnv(cbs.pAttachments, cbs.attachmentCount * sizeof(VkPipelineColorBlendAttachmentState));
        h = fnv(pri.pColorAttachmentFormats, pri.colorAttachmentCount * sizeof(VkFormat), h);
        const uint32_t x[4] = {pri.colorAttachmentCount, (uint32_t)pri.depthAttachmentFormat, (uint32_t)pri.stencilAttachmentFormat,
                               (uint32_t)a2c};
        h = fnv(x, sizeof x, h);
        auto it = g.lib_fo.find(h);
        if (it != g.lib_fo.end()) {
            libs[3] = it->second;
        } else {
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            ms.alphaToCoverageEnable = a2c;
            VkPipelineRenderingCreateInfo r = pri;
            r.pNext = nullptr;
            VkPipelineDynamicStateCreateInfo dy = dyn_info();
            VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            gp.pNext = &r;
            gp.pColorBlendState = &cbs;
            gp.pMultisampleState = &ms;
            gp.pDynamicState = &dy;
            libs[3] = g.lib_fo[h] = make_library(gp, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT);
        }
    }
    lap(t_fo);
    for (VkPipeline l : libs)
        if (!l) return VK_NULL_HANDLE;
    const uint64_t lk = fnv(libs, sizeof libs);
    auto it = g.linked.find(lk);
    if (it != g.linked.end()) return it->second;
    VkPipelineLibraryCreateInfoKHR li{VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR};
    li.libraryCount = 4;
    li.pLibraries = libs;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.pNext = &li;
    gp.layout = g.layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(g.dev, g.pcache, 1, &gp, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    lap(t_link);
    return g.linked[lk] = p;
}

// Crée, en parallèle, les bibliothèques de tous les shaders du cache et les pipelines de calcul. Depuis pipelines.cache,
// quelques dizaines de millisecondes ; sinon (premier lancement sans le launcher), la compilation complète.
// avance(k, n) : progression, appelée sur ce thread. Rend le nombre de shaders prêts.
size_t warm_up(const std::function<void(size_t, size_t)>& avance) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<ShaderMod*> mods;   // chargés ici : get_shader n'est pas réentrant
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((cache_dir() + "*.spv").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            const std::string n = fd.cFileName;
            if (ShaderMod* sm = get_shader(n.substr(0, n.size() - 4))) mods.push_back(sm);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    const bool neg_one = !nvn::g_depth_zero_one;
    struct Job { ShaderMod* sm; uint64_t key; VkPipeline out; };
    std::vector<Job> jobs;
    for (ShaderMod* sm : mods) {
        if (sm->model == 0 && g.gpl) {
            const uint64_t k = vs_library_key(sm->mod, VK_POLYGON_MODE_FILL, neg_one);
            if (!g.lib_vs.count(k)) jobs.push_back({sm, k, VK_NULL_HANDLE});
        } else if (sm->model == 4 && g.gpl) {
            if (!g.lib_fs.count((uint64_t)sm->mod)) jobs.push_back({sm, (uint64_t)sm->mod, VK_NULL_HANDLE});
        } else if (sm->model == 5) {
            if (!g.cpipelines.count((uint64_t)sm->mod)) jobs.push_back({sm, (uint64_t)sm->mod, VK_NULL_HANDLE});
        }
    }
    std::atomic<size_t> next{0}, done{0};
    auto travail = [&](bool rapporte) {
        for (size_t i; (i = next++) < jobs.size();) {
            Job& j = jobs[i];
            if (j.sm->model == 0) j.out = create_vs_library(j.sm->mod, VK_POLYGON_MODE_FILL, neg_one);
            else if (j.sm->model == 4) j.out = create_fs_library(j.sm->mod);
            else j.out = create_compute_pipeline(j.sm->mod);
            const size_t d = ++done;
            if (rapporte) avance(d, jobs.size());
        }
    };
    const unsigned hc = std::thread::hardware_concurrency();
    const unsigned nth = std::max(1u, std::min(16u, hc > 2 ? hc - 1 : 1u));
    std::vector<std::thread> th;
    for (unsigned t = 1; t < nth; t++) th.emplace_back(travail, false);
    travail(true);
    for (auto& t : th) t.join();
    avance(jobs.size(), jobs.size());
    size_t ko = 0, nvs = 0, nfs = 0, ncs = 0;
    for (Job& j : jobs) {
        ko += !j.out;
        if (j.sm->model == 0) g.lib_vs[j.key] = j.out, nvs++;
        else if (j.sm->model == 4) g.lib_fs[j.key] = j.out, nfs++;
        else g.cpipelines[j.key] = j.out, ncs++;
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    HLOG("vk", "pipelines : %zu shaders du cache, %zu préparés (%zu sommets, %zu fragments, %zu calculs, %zu échecs) en %.2f s",
         mods.size(), jobs.size(), nvs, nfs, ncs, ko, s);
    return jobs.size() - ko;
}

void ensure_init() {
    if (g.ok || g.failed) return;
    if (getenv("RECOMP_NO_VULKAN")) {
        g.failed = true;
        return;
    }
    HWND h = (HWND)gfx_window_handle();
    if (!h) return;
    g.ok = init(h);
    if (!g.ok) {
        g.failed = true;
        HLOG("vk", "initialisation Vulkan impossible : présentation GDI");
        return;
    }
    // tous les shaders prêts avant la première image (rapide si le launcher a fait la précompilation)
    pcache_open();
    const auto t0 = std::chrono::steady_clock::now();
    int dernier = -1;
    warm_up([&](size_t k, size_t n) {
        const int pct = n ? (int)(k * 100 / n) : 100;
        if (pct / 5 == dernier / 5 || std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) return;
        dernier = pct;
        char b[96];
        snprintf(b, sizeof b, "compilation des shaders %d %%", pct);
        gfx_status(b);
    });
    if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(1)) pcache_write();   // c'était une compilation
}

// pipelines.cache écrit à la fermeture du jeu (fenêtre fermée)
void save_caches() {
    if (g.ok) pcache_write();
}

// recomp_main --precompile : compile tous les shaders du cache (premier lancement, nouveau pilote, nouveaux shaders),
// sans fenêtre ni jeu, en écrivant l'avancement sur la sortie standard pour le launcher (« @progres x »).
int precompile() {
    auto dire = [](const char* f, auto... a) {
        printf(f, a...);
        fflush(stdout);
    };
    if (!init(nullptr)) {
        dire("@erreur Vulkan 1.3 indisponible\n");
        return 1;
    }
    // signature : GPU, pilote, mode des pipelines et liste des shaders (nombre, taille, date la plus récente)
    uint64_t taille = 0, recent = 0, nombre = 0;
    WIN32_FIND_DATAA fd;
    HANDLE fh = FindFirstFileA((cache_dir() + "*.spv").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            nombre++;
            taille += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            recent = std::max(recent, ((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime);
        } while (FindNextFileA(fh, &fd));
        FindClose(fh);
    }
    // + l'exécutable (taille, date) : le pilote NVIDIA garde aussi son propre cache par programme ; après un nouvel
    // exécutable (mise à jour), le réchauffer ici (avec la progression dans le launcher) plutôt qu'au démarrage du jeu
    uint64_t exe_taille = 0, exe_date = 0;
    {
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        WIN32_FILE_ATTRIBUTE_DATA ad;
        if (GetFileAttributesExA(exe, GetFileExInfoStandard, &ad)) {
            exe_taille = ((uint64_t)ad.nFileSizeHigh << 32) | ad.nFileSizeLow;
            exe_date = ((uint64_t)ad.ftLastWriteTime.dwHighDateTime << 32) | ad.ftLastWriteTime.dwLowDateTime;
        }
    }
    char sig[320];
    std::string uuid;
    for (uint8_t b : g.props.pipelineCacheUUID) {
        char x[3];
        snprintf(x, sizeof x, "%02x", b);
        uuid += x;
    }
    snprintf(sig, sizeof sig, "v1 %04x:%04x pilote %u %s %s %llu %llu %llu exe %llu %llu", g.props.vendorID, g.props.deviceID,
             g.props.driverVersion, uuid.c_str(), g.gpl ? "gpl" : "complet", (unsigned long long)nombre, (unsigned long long)taille,
             (unsigned long long)recent, (unsigned long long)exe_taille, (unsigned long long)exe_date);
    const std::string marque = exe_dir() + "pipelines.ok";
    {
        std::ifstream f(marque);
        std::string avant;
        std::getline(f, avant);
        if (avant == sig && GetFileAttributesA(pcache_path().c_str()) != INVALID_FILE_ATTRIBUTES) {
            dire("@saute shaders déjà compilés pour ce GPU et ce pilote\n");
            return 0;
        }
    }
    dire("@info %s : %llu shaders\n", g.props.deviceName, (unsigned long long)nombre);
    pcache_open();
    int dernier = -1;
    const auto t0 = std::chrono::steady_clock::now();
    const size_t n = warm_up([&](size_t k, size_t total) {
        const int pm = total ? (int)(k * 1000 / total) : 1000;
        if (pm == dernier) return;
        dernier = pm;
        dire("@progres %.3f\n", pm / 1000.0);
    });
    if (!pcache_write()) {
        dire("@erreur cache de pipelines non écrit\n");
        return 1;
    }
    std::ofstream(marque) << sig << "\n";
    dire("@info %zu shaders compilés en %.1f s\n", n, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}

// RECOMP_PERF : temps CPU par étape de draw (µs cumulés)
std::atomic<uint64_t> g_dt[6], g_dn{0};
struct DrawTimer {
    std::chrono::steady_clock::time_point t = std::chrono::steady_clock::now();
    int k = 0;
    void mark() {
        auto n = std::chrono::steady_clock::now();
        g_dt[k++] += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(n - t).count();
        t = n;
    }
};
// Soumet le tampon de commandes de l'emplacement en cours (enregistrement terminé) en signalant la valeur
// chronologique suivante, et au besoin un sémaphore binaire (image d'écran) ; wait_bin : acquisition de
// l'image d'écran. Les tampons intermédiaires de l'enregistrement restent à l'emplacement jusqu'à sa
// réutilisation ; les relectures de diagnostic (captures) imposent d'attendre le GPU tout de suite.
uint64_t submit_cb(const char* where, VkSemaphore wait_bin = VK_NULL_HANDLE, VkSemaphore signal_bin = VK_NULL_HANDLE) {
    Vk::Slot& s = g.slots[g.slot];
    const uint64_t val = ++g.submitted;
    VkSemaphore sig[2] = {g.timeline, signal_bin};
    const uint64_t sigv[2] = {val, 0}, waitv[1] = {0};
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    ts.waitSemaphoreValueCount = wait_bin ? 1 : 0;
    ts.pWaitSemaphoreValues = waitv;
    ts.signalSemaphoreValueCount = signal_bin ? 2 : 1;
    ts.pSignalSemaphoreValues = sigv;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ts;
    si.waitSemaphoreCount = wait_bin ? 1 : 0;
    si.pWaitSemaphores = &wait_bin;
    si.pWaitDstStageMask = &ws;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cb;
    si.signalSemaphoreCount = signal_bin ? 2 : 1;
    si.pSignalSemaphores = sig;
    check_vk(vkQueueSubmit(g.q, 1, &si, VK_NULL_HANDLE), where);
    s.serial = val;
    s.staging.insert(s.staging.end(), g.staging.begin(), g.staging.end());
    g.staging.clear();
    s.batch = std::move(g.batch);
    g.batch.clear();
    g.ring_peak = std::max(g.ring_peak, g.ringoff);
    if (!g_readbacks.empty()) {
        wait_serial(val, where);
        free_staging();   // écrit les relectures
    }
    return val;
}
// soumet le travail enregistré sans attendre le GPU (verrou g_vk_lk déjà pris) ; 0 si rien à soumettre
uint64_t submit_locked(const char* where) {
    if (!g.ok || !g.recording) return 0;
    end_render();
    vkEndCommandBuffer(g.cb);
    g.recording = false;
    return submit_cb(where);
}
// soumet le travail enregistré et attend que le GPU ait tout fini (verrou g_vk_lk déjà pris)
void flush_locked(const char* where) {
    if (!g.ok) return;
    submit_locked(where);
    wait_serial(g.submitted, where);
}
// RECOMP_SYNC_FROM=N : à partir de l'image N, chaque dessin et chaque calcul est soumis et attendu seul
// (diagnostic : le lot qui bloque le GPU ne contient alors que le shader fautif)
bool sync_each() {
    static const long from = getenv("RECOMP_SYNC_FROM") ? atol(getenv("RECOMP_SYNC_FROM")) : -1;
    return from >= 0 && g.cur_frame + 1 >= (uint64_t)from;
}

// dessin ou calcul ignoré : compté par image ; chaque couple (cause, programme) est journalisé une fois
void skip(const char* why, const std::string& label) {
    g.frame_skipped++;
    static std::set<std::string> seen;
    if (seen.size() < 512 && seen.insert(std::string(why) + "|" + label).second)
        HLOG("vk", "dessin ignoré (%s) : « %s »", why, label.c_str());
}

// RECOMP_SKIP=motif1,motif2 (diagnostic) : ignore les dessins et calculs dont l'étiquette du programme
// contient un des motifs, ou dont un shader a cette empreinte (ex. « volumetric_fog », « 24be96fb87506555 »).
bool user_skip(const std::string& label, const ShaderMod* a, const ShaderMod* b = nullptr) {
    static const std::vector<std::string> pats = [] {
        std::vector<std::string> v;
        if (const char* e = getenv("RECOMP_SKIP")) {
            std::string s(e), cur;
            for (char c : s + ",") {
                if (c == ',') {
                    if (!cur.empty()) v.push_back(cur);
                    cur.clear();
                } else {
                    cur += c;
                }
            }
        }
        return v;
    }();
    for (auto& p : pats)
        if (label.find(p) != std::string::npos || (a && a->hash == p) || (b && b->hash == p)) return true;
    return false;
}

void draw(const DrawCall& d) {
    DrawTimer dtm;
    g_dn++;
    std::lock_guard<std::mutex> lk(g_vk_lk);
    ensure_init();
    if (!g.ok) return;
    begin_frame();
    const auto& s = *d.st;
    // les effacements ne dépendent pas du programme lié (souvent un programme de calcul à ce moment :
    // ils étaient alors ignorés, et les cibles gardaient le contenu de l'image précédente)
    const bool clear = d.kind == DrawCall::ClearColor || d.kind == DrawCall::ClearDepth;
    nvn::ProgramInfo prog;
    ShaderMod* vs = nullptr;
    ShaderMod* fs = nullptr;
    if (!clear) {
        if (!nvn::g_prog.find_key(s.program, &prog)) return;
        for (auto& st : prog.stages) {
            if (st.stage == 0) vs = get_shader(st.hash);
            else if (st.stage == 1) fs = get_shader(st.hash);
            else { skip(st.stage == 5 ? "programme de calcul lié" : "étage géométrie/tessellation", prog.label); return; }
            if ((st.stage == 0 && !vs) || (st.stage == 1 && !fs)) {
                skip(st.stage == 0 ? "shader de sommets absent" : "shader de fragments absent", prog.label);
                return;
            }
        }
        if (!vs) {
            skip("pas de shader de sommets", prog.label);
            return;
        }
        if (user_skip(prog.label, vs, fs)) {
            skip("RECOMP_SKIP", prog.label);
            return;
        }
    }
    // cibles
    Image* rts[8] = {};
    VkFormat rtf[8] = {};
    int nrt = 0;
    for (int i = 0; i < s.nrt; i++) {
        rts[i] = get_image(s.rt[i]);
        if (rts[i] && is_compressed(rts[i]->fmt)) rts[i] = nullptr;
        if (rts[i]) image_to_general(*rts[i]), rtf[i] = rts[i]->fmt;
        nrt = i + 1;
    }
    Image* ds = get_image(s.depth);
    if (ds) image_to_general(*ds);
    if (g.dbg) {
        HLOG("vkdbg", "dessin %d « %s »", (int)d.kind, prog.label.c_str());
        for (int i = 0; i < s.nrt; i++) dbg_tex("rt", i, s.rt[i]);
        if (s.depth) dbg_tex("ds", 0, s.depth);
    }
    // zone de rendu : taille du niveau de mipmap visé par la vue de la première cible
    const uint32_t lv0 = rts[0] ? s.rt_level[0] : s.depth_level;
    uint32_t W = rts[0] ? rts[0]->w : (ds ? ds->w : 1), H = rts[0] ? rts[0]->h : (ds ? ds->h : 1);
    W = std::max(1u, W >> lv0), H = std::max(1u, H >> lv0);
    {   // RECOMP_DRAW_WATCH=hash : liaisons des dessins de ce shader (textures séparées → image, cible en cours ?)
        static const char* watch = getenv("RECOMP_DRAW_WATCH");
        static int nlog = 0;
        if (watch && nlog < 400 && ((vs && vs->hash == watch) || (fs && fs->hash == watch))) {
            nlog++;
            auto desc = [&](uint64_t h) -> std::string {
                uint32_t tid = h & 0xfffff;
                nvn::TexDesc td;
                bool have = false;
                {
                    std::lock_guard<std::mutex> lk(nvn::g_desc_lk);
                    nvn::g_texpool.with_key(s.texpool, [&](const nvn::TexPool& p) {
                        auto it = p.desc.find(tid);
                        if (it != p.desc.end()) td = it->second, have = true;
                    });
                }
                if (!have) return "absente";
                Image* im = get_image(td.tex);
                if (!im) return "sans image";
                char b[200];
                bool cur = im == ds;
                for (int i = 0; i < nrt; i++) cur |= im == rts[i];
                // échantillonneur de la poignée combinée (bits 20-31) : filtres NVN min/mag (base 0 : mag 0 NEAREST, 1 LINEAR)
                const uint32_t sid = (uint32_t)(h >> 20) & 0xfff;
                nvn::SamplerInfo si;
                bool have_s = false;
                {
                    std::lock_guard<std::mutex> lk(nvn::g_desc_lk);
                    nvn::g_samppool.with_key(s.samppool, [&](const nvn::SampPool& p) {
                        auto it = p.desc.find(sid);
                        if (it != p.desc.end()) si = it->second, have_s = true;
                    });
                }
                snprintf(b, sizeof b, "%ux%u×%u fmt %u cible %u clé %llx éch. %u%s%u/%u%s", im->w, im->h, im->layers, im->nvnfmt,
                         im->target, (unsigned long long)td.tex, sid, have_s ? " " : " absent ", si.min, si.mag,
                         cur ? " = CIBLE EN COURS" : "");
                return b;
            };
            std::string l;
            for (int k = 0; k < 64; k++)
                if (uint64_t h = s.septex[1][k]) l += " st" + std::to_string(k) + "=" + std::to_string(h & 0xfffff) + "(" + desc(h) + ")";
            for (int k = 0; k < 32; k++)
                if (uint64_t h = s.tex[1][k]) l += " t" + std::to_string(k) + "=" + std::to_string(h & 0xfffff) + "(" + desc(h) + ")";
            std::string rt;
            for (int i = 0; i < nrt; i++)
                rt += rts[i] ? " rt" + std::to_string(i) + "=" + std::to_string(rts[i]->w) + "x" + std::to_string(rts[i]->h) +
                                   "/fmt" + std::to_string(rts[i]->nvnfmt)
                             : " rt" + std::to_string(i) + "=-";
            char st[320];
            const uint8_t* bl = s.blend[0];
            snprintf(st, sizeof st, " | mélange %08x (fonctions %u %u %u %u éq. %u %u, constante %g %g %g %g) masque %08x ds %02x%02x%02x%02x%02x%02x%02x%02x poly %08x",
                     s.color, bl[1], bl[2], bl[3], bl[4], bl[5], bl[6], s.blend_color[0], s.blend_color[1], s.blend_color[2],
                     s.blend_color[3], s.chmask, s.ds[0], s.ds[1], s.ds[2], s.ds[3], s.ds[4], s.ds[5], s.ds[6], s.ds[7], s.poly);
            static std::string last;   // n'imprime que les changements de liaisons ou d'état
            std::string all = rt + st + l;
            if (all != last) {
                last = all;
                HLOG("watch", "image %llu dessin %llu « %s » prof %s%s%s :%s", (unsigned long long)g.cur_frame + 1,
                     (unsigned long long)g.frame_draws, prog.label.c_str(),
                     ds ? (std::to_string(ds->w) + "x" + std::to_string(ds->h) + " fmt " + std::to_string(ds->nvnfmt)).c_str() : "-",
                     rt.c_str(), st, l.c_str());
            } else {
                nlog--;
            }
        }
    }
    dtm.mark();   // 0 : programme, shaders, cibles
    // textures liées : images créées et initialisées avant d'ouvrir la passe de rendu
    if (d.kind != DrawCall::ClearColor && d.kind != DrawCall::ClearDepth)
        for (int si = 0; si < 2; si++) {
            ShaderMod* sm = si == 0 ? vs : fs;
            if (!sm || std::find(sm->banks.begin(), sm->banks.end(), 2) == sm->banks.end()) continue;
            for (int k = 0; k < 64; k++)
                if (uint64_t h = s.tex[si][k]) bind_handle(h, s.texpool, s.samppool, sm->tex);
            // poignées séparées (BindSeparateTextures / Samplers) : combinées par le shader (TEX bindless)
            for (int k = 0; k < 64; k++) {
                if (uint64_t h = s.septex[si][k]) bind_tex_id(h & 0xfffff, s.texpool, sm->tex, h);
                if (uint64_t h = s.sepsamp[si][k]) bind_samp_id((h >> 20) & 0xfff, s.samppool);
            }
        }
    dtm.mark();   // 1 : liaison des textures
    // rendu dynamique (recommencé quand les cibles changent)
    uint64_t rtkey[18];   // cibles, puis niveau | couche << 16 de chaque vue
    for (int i = 0; i < 8; i++) {
        rtkey[i] = i < nrt ? s.rt[i] : 0;
        rtkey[9 + i] = i < nrt ? s.rt_level[i] | (uint64_t)s.rt_layer[i] << 16 : 0;
    }
    rtkey[8] = s.depth;
    rtkey[17] = s.depth_level | (uint64_t)s.depth_layer << 16;
    if (!g.in_render || memcmp(rtkey, g.cur_rt, sizeof rtkey)) {
        end_render();
        VkRenderingAttachmentInfo ca[8];
        for (int i = 0; i < nrt; i++) {
            ca[i] = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            ca[i].imageView = rts[i] ? render_view(*rts[i], s.rt_level[i], s.rt_layer[i]) : VK_NULL_HANDLE;
            ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = ds ? render_view(*ds, s.depth_level, s.depth_layer) : VK_NULL_HANDLE;
        da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {W, H}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = nrt;
        ri.pColorAttachments = ca;
        ri.pDepthAttachment = ds ? &da : nullptr;
        ri.pStencilAttachment = ds && has_stencil(ds->fmt) ? &da : nullptr;
        vkCmdBeginRendering(g.cb, &ri);
        g.in_render = true;
        g.att.clear();
        for (int i = 0; i < nrt; i++)
            if (rts[i]) g.att.push_back({rts[i]->img, VK_IMAGE_ASPECT_COLOR_BIT});
        if (ds) g.att.push_back({ds->img, ds->aspect});
        memcpy(g.cur_rt, rtkey, sizeof rtkey);
    }
    if (d.kind == DrawCall::ClearColor || d.kind == DrawCall::ClearDepth) {
        VkClearAttachment ca{};
        VkClearRect cr{{{0, 0}, {W, H}}, 0, 1};
        if (d.kind == DrawCall::ClearColor) {
            if (d.index >= nrt || !rts[d.index]) return;
            ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ca.colorAttachment = d.index;
            memcpy(ca.clearValue.color.float32, d.color, 16);
        } else {
            if (!ds) return;
            ca.aspectMask = (d.depth_mask ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                            (has_stencil(ds->fmt) && d.stencil_mask ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if (!ca.aspectMask) return;
            ca.clearValue.depthStencil = {d.depth, (uint32_t)d.stencil};
        }
        vkCmdClearAttachments(g.cb, 1, &ca, 1, &cr);
        return;
    }
    // programme sans shader de fragments (masque de profondeur de l'interface vui_depth_mask) : la
    // profondeur seule est écrite, les cibles couleur liées restent intactes (masque d'écriture nul)
    static const bool no_depth_only = getenv("RECOMP_NO_DEPTH_ONLY") != nullptr;   // diagnostic
    if (!fs && nrt && no_depth_only) {
        skip("pas de shader de fragments avec cibles", prog.label);
        return;
    }
    dtm.mark();   // 2 : passe de rendu, effacements
    // ---- pipeline
    struct Key {
        uint64_t vs, fs;
        VkFormat rtf[8], dsf;
        uint8_t blend[8][8];
        uint32_t chmask, color, poly;
        uint8_t dsst[8];
        uint32_t attribs[16];
        uint32_t streams[16][2];
        uint32_t topo;
        uint32_t origin;
    } key;
    memset(&key, 0, sizeof key);
    key.vs = (uint64_t)vs->mod;
    key.fs = fs ? (uint64_t)fs->mod : 0;
    memcpy(key.rtf, rtf, sizeof rtf);
    key.dsf = ds ? ds->fmt : VK_FORMAT_UNDEFINED;
    memcpy(key.blend, s.blend, sizeof key.blend);
    // rejet et sens des faces, biais, profondeur et stencil : états dynamiques (réglés après la liaison), hors clé
    key.chmask = s.chmask, key.color = s.color, key.poly = (s.poly >> 16) & 0xff;
    key.dsst[0] = s.ms[1];   // alpha → couverture (état de sortie)
    for (int i = 0; i < 16 && i < s.nattr; i++) key.attribs[i] = s.attribs[i];
    for (int i = 0; i < 16 && i < s.nstream; i++) key.streams[i][0] = s.streams[i][0], key.streams[i][1] = s.streams[i][1];
    key.topo = (uint32_t)d.prim;
    key.origin = nvn::g_depth_zero_one;   // convention de profondeur : bibliothèque de sommets
    uint64_t kh = fnv(&key, sizeof key);
    VkPipeline pipe = VK_NULL_HANDLE;
    auto pit = g.pipelines.find(kh);
    if (pit != g.pipelines.end()) {
        pipe = pit->second;
    } else {
        VkPipelineShaderStageCreateInfo stg[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                  {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stg[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stg[0].module = vs->mod;
        stg[0].pName = "main";
        stg[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stg[1].module = fs ? fs->mod : VK_NULL_HANDLE;
        stg[1].pName = "main";
        // entrées de sommets : uniquement les emplacements lus par le shader
        std::vector<VkVertexInputAttributeDescription> va;
        std::vector<VkVertexInputBindingDescription> vb;
        std::set<uint32_t> used_streams;
        for (int loc : vs->vin) {
            uint32_t a = loc < s.nattr ? s.attribs[loc] : 0;
            VkFormat f = vertex_format(a & 0x3ff);
            if (f == VK_FORMAT_UNDEFINED) f = VK_FORMAT_R32G32B32A32_SFLOAT;
            uint32_t stream = (a >> 10) & 0x1f;
            va.push_back({(uint32_t)loc, stream, f, a >> 15});
            used_streams.insert(stream);
        }
        for (uint32_t st : used_streams) {
            uint32_t stride = st < (uint32_t)s.nstream ? s.streams[st][0] : 0;
            uint32_t div = st < (uint32_t)s.nstream ? s.streams[st][1] : 0;
            vb.push_back({st, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
        }
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        vi.vertexBindingDescriptionCount = (uint32_t)vb.size();
        vi.pVertexBindingDescriptions = vb.data();
        vi.vertexAttributeDescriptionCount = (uint32_t)va.size();
        vi.pVertexAttributeDescriptions = va.data();
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = topo(d.prim);
        VkPipelineViewportDepthClipControlCreateInfoEXT dcc{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT};
        dcc.negativeOneToOne = !nvn::g_depth_zero_one;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = 1;
        vps.scissorCount = 1;
        vps.pNext = g.depth_clip_control ? &dcc : nullptr;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        uint8_t cull = s.poly & 0xff, front = (s.poly >> 8) & 0xff, mode = (s.poly >> 16) & 0xff;
        rs.cullMode = cull == 1 ? VK_CULL_MODE_FRONT_BIT : cull == 2 ? VK_CULL_MODE_BACK_BIT : cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE;
        // le retournement de Y (origine bas-gauche) inverse le sens des faces
        bool ccw = front == 1;
        if (!nvn::g_origin_upper_left) ccw = !ccw;
        static const bool flip_front = getenv("RECOMP_FLIP_FRONT") != nullptr;   // diagnostic
        if (flip_front) ccw = !ccw;
        rs.frontFace = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        rs.polygonMode = mode == 0 ? VK_POLYGON_MODE_POINT : mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1.0f;
        rs.depthBiasEnable = ((s.poly >> 24) & 0xff) != 0;
        static const bool no_bias = getenv("RECOMP_NO_DEPTH_BIAS") != nullptr, no_cull = getenv("RECOMP_NO_CULL") != nullptr;
        if (no_bias) rs.depthBiasEnable = VK_FALSE;   // diagnostic
        if (no_cull) rs.cullMode = VK_CULL_MODE_NONE;  // diagnostic
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        ms.alphaToCoverageEnable = s.ms[1];
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = ds ? s.ds[0] : VK_FALSE;
        dss.depthWriteEnable = ds ? s.ds[1] : VK_FALSE;
        dss.depthCompareOp = cmpop(s.ds[2]);
        static const char* eq_as = getenv("RECOMP_DEPTH_EQUAL_AS");   // diagnostic : 7 = GEQUAL, 8 = ALWAYS
        if (eq_as && s.ds[2] == 3) dss.depthCompareOp = cmpop((uint8_t)atoi(eq_as));
        dss.stencilTestEnable = ds && has_stencil(ds->fmt) ? s.ds[3] : VK_FALSE;
        uint32_t ops = s.ds[5] | (s.ds[6] << 8) | (s.ds[7] << 16);
        dss.front = {stop(ops & 0xf), stop((ops >> 8) & 0xf), stop((ops >> 4) & 0xf), cmpop(s.ds[4] & 0xf), 0xff, 0xff, 0};
        dss.back = {stop((ops >> 12) & 0xf), stop((ops >> 20) & 0xf), stop((ops >> 16) & 0xf), cmpop(s.ds[4] >> 4), 0xff, 0xff, 0};
        VkPipelineColorBlendAttachmentState cba[8];
        for (int i = 0; i < nrt; i++) {
            cba[i] = {};
            const uint8_t* b = s.blend[i];
            cba[i].blendEnable = ((s.color >> i) & 1) != 0;
            cba[i].srcColorBlendFactor = bfac(b[1]);
            cba[i].dstColorBlendFactor = bfac(b[2]);
            cba[i].srcAlphaBlendFactor = bfac(b[3]);
            cba[i].dstAlphaBlendFactor = bfac(b[4]);
            cba[i].colorBlendOp = bop(b[5]);
            cba[i].alphaBlendOp = bop(b[6]);
            uint32_t m = (s.chmask >> (4 * i)) & 0xf;
            cba[i].colorWriteMask = fs ? m : 0;
        }
        VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cbs.attachmentCount = nrt;
        cbs.pAttachments = cba;
        VkPipelineDynamicStateCreateInfo dy = dyn_info();
        VkPipelineRenderingCreateInfo pri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        pri.colorAttachmentCount = nrt;
        pri.pColorAttachmentFormats = rtf;
        pri.depthAttachmentFormat = ds ? ds->fmt : VK_FORMAT_UNDEFINED;
        pri.stencilAttachmentFormat = ds && has_stencil(ds->fmt) ? ds->fmt : VK_FORMAT_UNDEFINED;
        VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.pNext = &pri;
        gp.stageCount = fs ? 2 : 1;
        gp.pStages = stg;
        gp.pVertexInputState = &vi;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vps;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pDepthStencilState = &dss;
        gp.pColorBlendState = &cbs;
        gp.pDynamicState = &dy;
        gp.layout = g.layout;
        // bibliothèques précompilées + liaison rapide ; sinon (pas de shader de fragments, GPU sans bibliothèques)
        // pipeline complet, compilé ici une fois (puis gardé dans pipelines.cache)
        const auto t_pipe = std::chrono::steady_clock::now();
        if (g.gpl && fs) pipe = gpl_pipeline(vs, fs, vi, ia.topology, rs.polygonMode, cbs, pri, ms.alphaToCoverageEnable);
        else if (vkCreateGraphicsPipelines(g.dev, g.pcache, 1, &gp, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        static const bool pipe_timing = getenv("RECOMP_PIPE_TIMING") != nullptr;
        if (pipe_timing)
            HLOG("pipe", "image %llu : pipeline %s (« %s ») en %.2f ms", (unsigned long long)g.cur_frame + 1,
                 g.gpl && fs ? "par bibliothèques" : "complet", prog.label.c_str(),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_pipe).count());
        if (!pipe) HLOG("vk", "création de pipeline impossible (prog « %s »)", prog.label.c_str());
        g.pipelines[kh] = pipe;
        g.pc_dirty++;
    }
    if (!pipe) {
        skip("pipeline non créé", prog.label);
        return;
    }
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    {   // ---- états dynamiques du pipeline, réglés quand ils changent dans ce tampon de commandes
        static const bool no_bias = getenv("RECOMP_NO_DEPTH_BIAS") != nullptr, no_cull = getenv("RECOMP_NO_CULL") != nullptr;
        static const bool flip_front = getenv("RECOMP_FLIP_FRONT") != nullptr;   // diagnostic
        static const char* eq_as = getenv("RECOMP_DEPTH_EQUAL_AS");               // diagnostic : 7 = GEQUAL, 8 = ALWAYS
        const uint8_t cull = s.poly & 0xff, front = (s.poly >> 8) & 0xff;
        const int cm = no_cull ? VK_CULL_MODE_NONE
                       : cull == 1 ? VK_CULL_MODE_FRONT_BIT
                       : cull == 2 ? VK_CULL_MODE_BACK_BIT
                       : cull == 3 ? VK_CULL_MODE_FRONT_AND_BACK
                                   : VK_CULL_MODE_NONE;
        bool ccw = front == 1;
        if (!nvn::g_origin_upper_left) ccw = !ccw;   // le retournement de Y (origine bas-gauche) inverse le sens des faces
        if (flip_front) ccw = !ccw;
        const int ff = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        const int bias = ((s.poly >> 24) & 0xff) != 0 && !no_bias;
        const int dt = ds ? s.ds[0] != 0 : 0, dw = ds ? s.ds[1] != 0 : 0;
        const int dop = cmpop(eq_as && s.ds[2] == 3 ? (uint8_t)atoi(eq_as) : s.ds[2]);
        const int st = ds && has_stencil(ds->fmt) ? s.ds[3] != 0 : 0;
        const uint32_t ops = s.ds[5] | (s.ds[6] << 8) | (s.ds[7] << 16);
        const uint32_t sf = (ops & 0xfff) | (uint32_t)(s.ds[4] & 0xf) << 12, sb = ((ops >> 12) & 0xfff) | (uint32_t)(s.ds[4] >> 4) << 12;
        Vk::Dyn& y = g.dyn;
        if (y.cull != cm) vkCmdSetCullMode(g.cb, (VkCullModeFlags)cm), y.cull = cm;
        if (y.front != ff) vkCmdSetFrontFace(g.cb, (VkFrontFace)ff), y.front = ff;
        if (y.bias != bias) vkCmdSetDepthBiasEnable(g.cb, bias), y.bias = bias;
        if (y.dt != dt) vkCmdSetDepthTestEnable(g.cb, dt), y.dt = dt;
        if (y.dw != dw) vkCmdSetDepthWriteEnable(g.cb, dw), y.dw = dw;
        if (y.dop != dop) vkCmdSetDepthCompareOp(g.cb, (VkCompareOp)dop), y.dop = dop;
        if (y.st != st) vkCmdSetStencilTestEnable(g.cb, st), y.st = st;
        if (y.sf != sf)
            vkCmdSetStencilOp(g.cb, VK_STENCIL_FACE_FRONT_BIT, stop(ops & 0xf), stop((ops >> 8) & 0xf), stop((ops >> 4) & 0xf),
                              cmpop(s.ds[4] & 0xf)),
                y.sf = sf;
        if (y.sb != sb)
            vkCmdSetStencilOp(g.cb, VK_STENCIL_FACE_BACK_BIT, stop((ops >> 12) & 0xf), stop((ops >> 20) & 0xf), stop((ops >> 16) & 0xf),
                              cmpop(s.ds[4] >> 4)),
                y.sb = sb;
    }
    note_shader(vs);
    note_shader(fs);
    dtm.mark();   // 3 : pipeline
    // ---- descripteurs : banques de constantes (sommets → jeu 0, fragments → jeu 1)
    VkDescriptorSet sets[2] = {};
    if (!alloc_bank_sets(2, sets)) {
        skip("descripteurs épuisés", prog.label);
        return;
    }
    std::vector<VkDescriptorBufferInfo> infos;
    std::vector<VkWriteDescriptorSet> writes;
    infos.reserve(64);
    uint64_t watch_ring = ~0ull;   // RECOMP_DRAW_WATCH : banque 2 (fragments) du dessin surveillé
    auto ring_alloc = [&](uint64_t n) -> uint64_t { return ring_take(n); };
    for (int si = 0; si < 2; si++) {
        ShaderMod* sm = si == 0 ? vs : fs;
        if (!sm) continue;
        int nvst = si == 0 ? 0 : 1;   // étage NVN
        for (int bank : sm->banks) {
            VkDescriptorBufferInfo bi{g.ring, 0, 256};
            if (bank == 2) {
                // banque 2 (établie sur les lectures des shaders et les liaisons du jeu) :
                //   0x020 + 8k : 32 poignées combinées (BindTextures)   0x120 + 8k : images (BindImages)
                //   0x168 + 8k : 128 textures séparées                  0x568 + 8k : 32 échantillonneurs séparés
                // (0x168 / 0x568 et non 0x160 / 0x560 : avec ces bases, chaque lecture du matériau
                // principal tombe sur une texture du bon type — carte d'ombre D16 pour la comparaison,
                // tableaux 2D, volumes 3D — et sur un échantillonneur lié, ss10-12)
                constexpr uint32_t SZ = 0x680;
                uint64_t off = ring_alloc(SZ);
                uint8_t* p = g.ringptr + off;
                memset(p, 0, SZ);
                for (int k = 0; k < 32; k++) memcpy(p + 0x20 + 8 * k, &s.tex[nvst][k], 8);
                for (int k = 0; k < 8; k++) memcpy(p + 0x120 + 8 * k, &s.img[nvst][k], 8);
                for (int k = 0; k < 64; k++) memcpy(p + 0x168 + 8 * k, &s.septex[nvst][k], 8);
                for (int k = 0; k < 32; k++) memcpy(p + 0x568 + 8 * k, &s.sepsamp[nvst][k], 8);
                bi = {g.ring, off, SZ};
                static const char* watch = getenv("RECOMP_DRAW_WATCH");
                if (watch && nvst == 1 && fs && fs->hash == watch) watch_ring = off;   // relu après le dessin
            } else if (bank == 0x18) {   // constantes pilote : taille de cible, viewport
                uint64_t off = ring_alloc(0x100);
                float* f = (float*)(g.ringptr + off);
                int32_t* i = (int32_t*)f;
                memset(f, 0, 0x100);
                f[0] = 1.0f / W, f[1] = 1.0f / H, f[2] = (float)W, f[3] = (float)H;
                i[0x50 / 4] = s.vp[0], i[0x54 / 4] = s.vp[1];
                i[0x58 / 4] = std::max(0, s.vp[2] - 1), i[0x5c / 4] = std::max(0, s.vp[3] - 1);
                bi = {g.ring, off, 0x100};
            } else if (bank >= 3 && bank - 3 < 32 && s.ubo[nvst][bank - 3].addr) {
                // tampon uniforme de l'emplacement k → banque k + 3 (comme en calcul) ; établi en
                // recalculant un shader de sommets sur les données du jeu (k + 2 lisait les matrices
                // dans le mauvais tampon : positions nulles, aucune géométrie)
                auto& u = s.ubo[nvst][bank - 3];
                bank_from_guest(u.addr, u.size, &bi);
            }
            infos.push_back(bi);
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[si];
            w.dstBinding = bank;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w.pBufferInfo = &infos.back();
            writes.push_back(w);
        }
    }
    {   // RECOMP_DBG_SSBO (avec RECOMP_DRAW_WATCH) : tampon de débogage inscriptible en jeu 1, liaison 31,
        // pour un shader modifié à la main qui y range des valeurs de registres (lues après chaque image)
        static const char* watch = getenv("RECOMP_DRAW_WATCH");
        static const bool dbg_ssbo = getenv("RECOMP_DBG_SSBO") != nullptr;
        if (dbg_ssbo && watch && fs && fs->hash == watch) {
            if (!g.dbgbuf) {
                make_buffer(65536, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                            &g.dbgbuf, &g.dbgmem);
                vkMapMemory(g.dev, g.dbgmem, 0, VK_WHOLE_SIZE, 0, (void**)&g.dbgptr);
                memset(g.dbgptr, 0, 65536);
            }
            infos.push_back({g.dbgbuf, 0, 65536});
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = sets[1];
            w.dstBinding = 31;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &infos.back();
            writes.push_back(w);
        }
    }
    if (!writes.empty()) vkUpdateDescriptorSets(g.dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    VkDescriptorSet all[3] = {sets[0], sets[1], g.texset};
    vkCmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 3, all, 0, nullptr);
    dtm.mark();   // 4 : descripteurs
    // ---- tampons de sommets
    for (int loc : vs->vin) {
        uint32_t a = loc < s.nattr ? s.attribs[loc] : 0;
        uint32_t st = (a >> 10) & 0x1f;
        VkBuffer b = g.ring;
        uint64_t o = 0;
        if (st < 32 && s.vb[st].addr && !find_pool(s.vb[st].addr, 1, &b, &o)) {
            static std::set<std::string> seen;
            if (seen.size() < 64 && seen.insert(prog.label + "|" + std::to_string(st)).second)
                HLOG("vk", "tampon de sommets introuvable : flux %u à 0x%llx (« %s »)", st,
                     (unsigned long long)s.vb[st].addr, prog.label.c_str());
        }
        vkCmdBindVertexBuffers(g.cb, st, 1, &b, &o);
    }
    // ---- état dynamique
    VkViewport vp;
    float vx = (float)s.vp[0], vy = (float)s.vp[1], vw = (float)s.vp[2], vh = (float)s.vp[3];
    if (vw <= 0 || vh <= 0) vw = (float)W, vh = (float)H, vx = vy = 0;
    // NVN suit glClipControl : en origine haut-gauche, Y normalisé +1 va à la ligne vy (haut), comme en
    // D3D, alors que Vulkan y met -1 → hauteur négative. Établi sur la cinématique : avec une hauteur
    // positive, la scène était rangée à l'envers en mémoire et chaque passe plein écran dont les UV
    // viennent d'un attribut de sommet (flou de mouvement, mise à l'échelle finale) la retournait.
    static const bool vp_vk = getenv("RECOMP_VIEWPORT_VK") != nullptr;   // diagnostic : ancienne convention
    if (nvn::g_origin_upper_left) {
        vp = vp_vk ? VkViewport{vx, vy, vw, vh, s.dr[0], s.dr[1]} : VkViewport{vx, vy + vh, vw, -vh, s.dr[0], s.dr[1]};
    } else {   // origine bas-gauche : y retourné
        vp = {vx, (float)H - vy, vw, -vh, s.dr[0], s.dr[1]};
    }
    vkCmdSetViewport(g.cb, 0, 1, &vp);
    VkRect2D sc;
    if (s.sc[2] > 0 && s.sc[3] > 0) {
        int sy = nvn::g_origin_upper_left ? s.sc[1] : (int)H - (s.sc[1] + s.sc[3]);
        sc = {{std::max(0, s.sc[0]), std::max(0, sy)}, {(uint32_t)s.sc[2], (uint32_t)s.sc[3]}};
    } else {
        sc = {{0, 0}, {W, H}};
    }
    vkCmdSetScissor(g.cb, 0, 1, &sc);
    vkCmdSetStencilReference(g.cb, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil_ref);
    vkCmdSetStencilCompareMask(g.cb, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil_vmask);
    vkCmdSetStencilWriteMask(g.cb, VK_STENCIL_FACE_FRONT_AND_BACK, s.stencil_wmask);
    vkCmdSetBlendConstants(g.cb, s.blend_color);
    vkCmdSetDepthBias(g.cb, s.poly_offset[1], s.poly_offset[2], s.poly_offset[0]);
    // ---- dessin
    std::string mk;
    if (g.checkpoints) {
        char b[256];
        snprintf(b, sizeof b, "image %llu dessin %llu (%s n %u ×%u) « %s » vs %s fs %s", (unsigned long long)g.cur_frame + 1,
                 (unsigned long long)g.frame_draws, d.kind == DrawCall::Arrays ? "tableau" : "indexé", d.count, d.instances,
                 prog.label.c_str(), vs->hash.c_str(), fs ? fs->hash.c_str() : "-");
        mk = b;
        mark("avant " + mk);
    }
    const int pq = prof_begin();
    if (d.kind == DrawCall::Arrays) {
        vkCmdDraw(g.cb, d.count, d.instances, d.first, d.base_instance);
    } else {
        VkBuffer b;
        uint64_t o;
        uint32_t isz = d.index_type == 0 ? 1 : d.index_type == 1 ? 2 : 4;
        if (!find_pool(d.index_addr, (uint64_t)d.count * isz, &b, &o) || d.index_type == 0) {
            skip(d.index_type == 0 ? "index 8 bits" : "tampon d’index introuvable", prog.label);
            prof_end(pq, "(dessin ignoré)");
            return;
        }
        vkCmdBindIndexBuffer(g.cb, b, o, d.index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(g.cb, d.count, d.instances, 0, d.base_vertex, d.base_instance);
    }
    if (pq >= 0) prof_end(pq, prog.label + (fs ? " [fs " + fs->hash + "]" : " [sans fs]"));
    if (g.checkpoints) mark("après " + mk);
    if (watch_ring != ~0ull) {   // banque 2 vue par le GPU à l'exécution, contre son contenu à l'enregistrement
        static int nw = 0;
        if (getenv("RECOMP_DRAW_WATCH_PEEK") && nw++ < 40) {
            constexpr uint32_t SZ = 0x660;
            char path[96];
            snprintf(path, sizeof path, "drawdump/bank2_%llu_%llu", (unsigned long long)g.cur_frame + 1, (unsigned long long)g.frame_draws);
            _mkdir("drawdump");
            if (FILE* f = fopen((std::string(path) + ".rec.bin").c_str(), "wb")) fwrite(g.ringptr + watch_ring, 1, SZ, f), fclose(f);
            end_render();
            Readback r{};
            r.raw_addr = (uint64_t)(g.ringptr + watch_ring), r.raw_size = SZ;
            if (make_buffer(SZ, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r.b, &r.m)) {
                VkBufferCopy c{watch_ring, 0, SZ};
                vkCmdCopyBuffer(g.cb, g.ring, r.b, 1, &c);
                barrier_all();
                r.path = path;
                g_readbacks.push_back(r);
            }
        }
    }
    // RECOMP_GPU_SERIAL=1 : chaque dessin se termine avant le suivant (même lot : la cadence processeur /
    // GPU ne change pas) ; les marqueurs désignent alors exactement le dessin fautif
    static const bool serial = getenv("RECOMP_GPU_SERIAL") != nullptr;
    if (serial) end_render();
    g.frame_draws++;
    g.last_label = prog.label;
    if (sync_each()) flush_locked("dessin isolé");
    dtm.mark();   // 5 : sommets, état dynamique, dessin
}

// ================================================================ calcul
// dimension NAK des images (0 1D, 2 1DArray, 3 2D, 4 2DArray, 5 3D) depuis le nom du traducteur
int img_dim_code(const std::string& d) {
    if (d == "1d") return 0;
    if (d == "1darray") return 2;
    if (d == "2d") return 3;
    if (d == "2darray") return 4;
    if (d == "3d") return 5;
    return -1;
}
VkImageView storage_view(Image& im, int dim) {
    auto it = im.sviews.find(dim);
    if (it != im.sviews.end()) return it->second;
    bool is3d = im.target == 2;
    static const VkImageViewType vt[] = {VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D, VK_IMAGE_VIEW_TYPE_1D_ARRAY,
                                         VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D};
    if (!im.storage || dim < 0 || dim > 5 || (dim == 5) != is3d) return im.sviews[dim] = VK_NULL_HANDLE;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.img;
    vi.viewType = vt[dim];
    vi.format = im.fmt;
    uint32_t layers = (dim == 2 || dim == 4) ? im.layers : 1;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    VkImageView v = VK_NULL_HANDLE;
    vkCreateImageView(g.dev, &vi, nullptr, &v);
    return im.sviews[dim] = v;
}
// Écrit le descripteur d'image de stockage de la poignée (id de texture dans les bits 0-19).
void bind_image(uint64_t handle, uint64_t texpool, int dim) {
    uint32_t tid = handle & 0xfffff;
    nvn::TexDesc td;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(nvn::g_desc_lk);
        nvn::g_texpool.with_key(texpool, [&](const nvn::TexPool& p) {
            auto it = p.desc.find(tid);
            if (it != p.desc.end()) td = it->second, have = true;
        });
    }
    if (!have || tid >= TEX_MAX) return;
    Image* im = get_image(td.tex);
    if (g.dbg) dbg_tex("img", (int)tid, td.tex);
    if (!im) return;
    image_to_general(*im);
    VkImageView v = storage_view(*im, dim);
    if (!v) {
        static int nw = 0;
        if (nw++ < 8) HLOG("vk", "image %u (fmt NVN %u, cible %u) non liable en stockage dim %d", tid, im->nvnfmt, im->target, dim);
        return;
    }
    auto& w = g.img_written[tid * 8 + dim];
    if (w == (uint64_t)v) return;
    w = (uint64_t)v;
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet ws{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    ws.dstSet = g.texset;
    ws.dstBinding = 10 + dim;
    ws.dstArrayElement = tid;
    ws.descriptorCount = 1;
    ws.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ws.pImageInfo = &ii;
    vkUpdateDescriptorSets(g.dev, 1, &ws, 0, nullptr);
}

void dispatch(const nvn::GfxState& s, uint32_t gx, uint32_t gy, uint32_t gz) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    ensure_init();
    if (!g.ok) return;
    begin_frame();
    nvn::ProgramInfo prog;
    if (!nvn::g_prog.find_key(s.program, &prog)) return;
    ShaderMod* cs = nullptr;
    for (auto& st : prog.stages)
        if (st.stage == 5) cs = get_shader(st.hash);
    if (!cs) {
        skip("shader de calcul absent", prog.label);
        return;
    }
    static const bool no_glob = getenv("RECOMP_NO_GLOBAL_MEM") != nullptr;   // diagnostic
    if (cs->glob && no_glob) {
        skip("calcul à mémoire globale désactivé", prog.label);
        return;
    }
    if (user_skip(prog.label, cs)) {
        skip("RECOMP_SKIP", prog.label);
        return;
    }
    {   // RECOMP_DISPATCH_LOG=1 : chaque programme de calcul exécuté (une fois), disposition de sa banque 0
        static const bool dlog = getenv("RECOMP_DISPATCH_LOG") != nullptr;
        static std::set<std::string> seen;
        if (dlog && seen.insert(cs->hash).second)
            HLOG("vk", "calcul %s « %s » %ux%ux%u : UBO %d, tampons de stockage %d, mots en ligne %d%s", cs->hash.c_str(),
                 prog.label.c_str(), gx, gy, gz, cs->c0[0], cs->c0[4], cs->c0[3], cs->glob ? ", mémoire globale" : "");
    }
    end_render();
    if (g.dbg) HLOG("vkdbg", "dispatch %ux%ux%u « %s »", gx, gy, gz, prog.label.c_str());
    // Banque 0 du calcul : en-tête 0x10, puis 16 o par tampon uniforme, 8 o par texture combinée k,
    // 8 o par image k. Les nombres viennent de la section de contrôle (ligne « c0 » des métadonnées) ;
    // règle vérifiée sur 10 étages (taille totale = mot 0xb8). Ex. : SDF du texte (2 tampons, 0 texture)
    // image 0 en 0x30 ; intégration du brouillard t0 en 0x20, image 1 en 0x30 ; effacement de
    // l'accumulation images 5-7 en 0x48-0x58. À défaut : un tampon, textures jusqu'à la dernière lue.
    // Disposition complète (établie sur 0098, b775, ff25 et la lumière du brouillard avec la taille totale de
    // l'octet 0xba) : 0x10 + données en ligne (octet 0xa9, en mots, arrondies à 16 o ;
    // nvnCommandBufferPushComputeConstants) + 16 o par tampon uniforme + 16 o par tampon de stockage (adresse
    // invitée, taille) + 8 o par texture + 8 o par texture séparée + 8 o par échantillonneur séparé + 8 o par image.
    uint32_t ntex = 0, tex_base = 0x20;
    const uint32_t inl = ((uint32_t)std::max(0, cs->c0[3]) * 4 + 15) & ~15u;
    const uint32_t nssbo = (uint32_t)std::max(0, cs->c0[4]);
    const uint32_t nsept = (uint32_t)std::max(0, cs->c0[5]), nseps = (uint32_t)std::max(0, cs->c0[6]);
    const uint32_t ssbo_base = 0x10 + inl + 0x10 * (uint32_t)std::max(0, cs->c0[0]);
    if (cs->c0[0] >= 0) {
        tex_base = ssbo_base + 0x10 * nssbo;
        ntex = (uint32_t)cs->c0[1];
    } else {
        for (auto& th : cs->texh)
            if (th.first == 0 && th.second >= 0x20) ntex = std::max(ntex, (th.second - 0x20) / 8 + 1);
    }
    const uint32_t sept_base = tex_base + 8 * ntex, seps_base = sept_base + 8 * nsept;
    const uint32_t img_base = seps_base + 8 * nseps;
    auto img_slot = [&](uint32_t off) -> int {
        int k = off >= img_base ? (int)((off - img_base) / 8) : -1;
        return k >= 0 && k < 64 ? k : -1;
    };
    for (auto& ih : cs->img) {
        int slot = ih.bank == 0 ? img_slot(ih.off) : -1;
        uint64_t h = slot >= 0 ? s.img[5][slot] : 0;
        if (h) {
            bind_image(h, s.texpool, img_dim_code(ih.dim));
            bind_handle(h, s.texpool, s.samppool, cs->tex);   // TXQ sur la même poignée
        }
    }
    for (uint32_t k = 0; k < ntex && k < 32; k++)
        if (uint64_t h = s.tex[5][k]) bind_handle(h, s.texpool, s.samppool, cs->tex);
    for (uint32_t k = 0; k < nsept && k < 64; k++)   // textures et échantillonneurs séparés (combinés par le shader)
        if (uint64_t h = s.septex[5][k]) bind_tex_id((uint32_t)(h & 0xfffff), s.texpool, cs->tex, h);
    for (uint32_t k = 0; k < nseps && k < 64; k++)
        if (uint64_t h = s.sepsamp[5][k]) bind_samp_id((uint32_t)((h >> 20) & 0xfff), s.samppool);
    // pipeline
    VkPipeline pipe = VK_NULL_HANDLE;
    auto pit = g.cpipelines.find((uint64_t)cs->mod);
    if (pit != g.cpipelines.end()) {
        pipe = pit->second;
    } else {   // précompilé d'habitude (warm_up) : shader absent du cache au démarrage
        pipe = create_compute_pipeline(cs->mod);
        if (!pipe) HLOG("vk", "création de pipeline de calcul impossible (« %s »)", prog.label.c_str());
        g.cpipelines[(uint64_t)cs->mod] = pipe;
        g.pc_dirty++;
    }
    if (!pipe) {
        skip("pipeline de calcul non créé", prog.label);
        return;
    }
    // banques : 0 = pilote (poignées d'images), 3 + k = tampon uniforme k (hypothèse : lue en c[3])
    VkDescriptorSet set;
    if (!alloc_bank_sets(1, &set)) {
        skip("descripteurs de calcul épuisés", prog.label);
        return;
    }
    std::vector<VkDescriptorBufferInfo> infos(cs->banks.size());
    std::vector<VkWriteDescriptorSet> writes;
    for (size_t k = 0; k < cs->banks.size(); k++) {
        int bank = cs->banks[k];
        VkDescriptorBufferInfo bi{g.ring, 0, 256};
        if (bank == 0) {
            // 0x000-0x5ff : banque 0 de NVN ; 0x600-0xfff : table de traduction de la mémoire globale
            // (glsl.py, GTAB) : nombre de pools, puis en 0x610 + 32 k : base invitée, adresse Vulkan, taille
            constexpr uint32_t B0 = 0x1000, GTAB = 0x600;
            const uint64_t off = ring_take(B0);
            uint8_t* p = g.ringptr + off;
            memset(p, 0, B0);
            uint32_t ng[3] = {gx, gy, gz};
            memcpy(p, ng, 12);   // nombre de groupes (emplacement supposé)
            if (inl) memcpy(p + 0x10, s.pushc, std::min<uint32_t>(inl, sizeof s.pushc));
            for (uint32_t k = 0; k < (uint32_t)std::max(0, cs->c0[0]) && k < 32; k++) {
                const uint64_t d[2] = {s.ubo[5][k].addr, s.ubo[5][k].size};
                if (0x10 + inl + 16 * k + 16 <= GTAB) memcpy(p + 0x10 + inl + 16 * k, d, 16);
            }
            for (uint32_t k = 0; k < nssbo && k < 32; k++) {
                const nvn::Range& r = s.ssbo[5][k];
                const uint64_t d[2] = {r.addr, r.size};
                if (ssbo_base + 16 * k + 16 <= GTAB) memcpy(p + ssbo_base + 16 * k, d, 16);
                if (cs->glob && r.addr && r.size && !guest_devaddr(r.addr, r.size)) {   // importe le pool au besoin
                    static int nmiss = 0;
                    if (nmiss++ < 10)
                        HLOG("vk", "tampon de stockage %u du calcul « %s » hors des pools (0x%llx +0x%llx) : accès ignorés", k,
                             prog.label.c_str(), (unsigned long long)r.addr, (unsigned long long)r.size);
                }
            }
            if (cs->glob) {   // tous les pools NVN (pointeurs rangés dans les données du jeu compris)
                import_all_pools();
                uint32_t n = 0;
                for (auto& pl : g.pools) {
                    if (!pl.dev) continue;
                    if (0x610 + 32 * n + 32 > B0) break;
                    const uint64_t t[4] = {pl.base, pl.dev, pl.size, 0};
                    memcpy(p + 0x610 + 32 * n, t, 32);
                    n++;
                }
                memcpy(p + GTAB, &n, 4);
            }
            for (uint32_t t = 0; t < ntex && t < 32; t++) if (tex_base + 8 * t + 8 <= GTAB) memcpy(p + tex_base + 8 * t, &s.tex[5][t], 8);
            for (uint32_t t = 0; t < nsept && t < 64; t++) if (sept_base + 8 * t + 8 <= GTAB) memcpy(p + sept_base + 8 * t, &s.septex[5][t], 8);
            for (uint32_t t = 0; t < nseps && t < 64; t++) if (seps_base + 8 * t + 8 <= GTAB) memcpy(p + seps_base + 8 * t, &s.sepsamp[5][t], 8);
            for (auto& ih : cs->img) {
                int slot = ih.bank == 0 ? img_slot(ih.off) : -1;
                if (slot >= 0 && ih.off + 8 <= GTAB) memcpy(p + ih.off, &s.img[5][slot], 8);
            }
            bi = {g.ring, off, B0};
        } else if (bank >= 3 && bank - 3 < 32 && s.ubo[5][bank - 3].addr) {
            auto& u = s.ubo[5][bank - 3];
            bank_from_guest(u.addr, u.size, &bi);
        }
        infos[k] = bi;
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = bank;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &infos[k];
        writes.push_back(w);
    }
    if (!writes.empty()) vkUpdateDescriptorSets(g.dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
    vkCmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    note_shader(cs);
    vkCmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0, 1, &set, 0, nullptr);
    vkCmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 2, 1, &g.texset, 0, nullptr);
    std::string mk;
    if (g.checkpoints) {
        char b[256];
        snprintf(b, sizeof b, "image %llu calcul %llu (%ux%ux%u) « %s » cs %s", (unsigned long long)g.cur_frame + 1,
                 (unsigned long long)g.frame_dispatches, gx, gy, gz, prog.label.c_str(), cs->hash.c_str());
        mk = b;
        mark("avant " + mk);
    }
    const int pq = prof_begin();
    vkCmdDispatch(g.cb, gx, gy, gz);
    if (pq >= 0) prof_end(pq, prog.label + " [calcul " + cs->hash + "]");
    if (g.checkpoints) mark("après " + mk);
    barrier_all();
    g.frame_dispatches++;
    g.last_label = prog.label + " (calcul)";
    if (sync_each()) flush_locked("calcul isolé");
}

std::atomic<uint64_t> g_flush_us{0}, g_flush_n{0};   // vidages (FenceSync) : temps cumulé (soumission et attentes)
uint64_t flush_async() {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    if (!g.ok) return 0;
    auto t0 = std::chrono::steady_clock::now();
    submit_locked("vidage");
    if (!g.async) wait_serial(g.submitted, "vidage");
    g_flush_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    g_flush_n++;
    return g.submitted;
}
void flush() {
    const uint64_t v = flush_async();
    std::lock_guard<std::mutex> lk(g_vk_lk);
    if (g.ok) wait_serial(v, "vidage");
}

// Fil de complétion : exécute les rappels (signal des syncs NVN) quand le GPU atteint leur valeur.
namespace {
std::mutex g_cq_lk;
std::condition_variable g_cq_cv;
std::deque<std::pair<uint64_t, std::function<void()>>> g_cq;   // valeurs croissantes
void completion_thread() {
    for (;;) {
        uint64_t want;
        {
            std::unique_lock<std::mutex> lk(g_cq_lk);
            g_cq_cv.wait(lk, [] { return !g_cq.empty(); });
            want = g_cq.front().first;
        }
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &g.timeline;
        wi.pValues = &want;
        VkResult r = vkWaitSemaphores(g.dev, &wi, 50000000ull);   // 50 ms, puis nouvel essai
        uint64_t done = gpu_done_serial();
        if ((r != VK_SUCCESS && r != VK_TIMEOUT) || g.lost) done = ~0ull;   // périphérique perdu : tout libérer
        std::vector<std::function<void()>> run;
        {
            std::lock_guard<std::mutex> lk(g_cq_lk);
            while (!g_cq.empty() && g_cq.front().first <= done) {
                run.push_back(std::move(g_cq.front().second));
                g_cq.pop_front();
            }
        }
        for (auto& f : run) f();
    }
}
}  // namespace
void when_done(uint64_t serial, std::function<void()> fn) {
    if (!serial || !g.ok || g.lost || gpu_done_serial() >= serial) {
        fn();
        return;
    }
    static std::once_flag once;
    std::call_once(once, [] { std::thread(completion_thread).detach(); });
    {
        std::lock_guard<std::mutex> lk(g_cq_lk);
        g_cq.emplace_back(serial, std::move(fn));
    }
    g_cq_cv.notify_one();
}

void barrier() {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    if (!g.ok || !g.recording) return;
    end_render();
}


// RECOMP_POOL_AUDIT=N : toutes les N images, 256 octets par Mo de chaque pool importé sont relus par le
// GPU et comparés au processeur (date le moment où un import ne voit plus la mémoire du jeu)
void pool_audit(uint64_t frame) {
    constexpr uint64_t S = 256, STEP = 0x100000;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    pmc.cb = sizeof pmc;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc))
        HLOG("vk", "contrôle image %llu : ensemble de travail %.0f Mo, mémoire privée %.0f Mo, défauts de page %lu",
             (unsigned long long)frame, pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0, pmc.PageFaultCount);
    if (g.mem_budget && vkGetPhysicalDeviceMemoryProperties2) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        mp.pNext = &bp;
        vkGetPhysicalDeviceMemoryProperties2(g.pd, &mp);
        for (uint32_t h = 0; h < mp.memoryProperties.memoryHeapCount; h++)
            HLOG("vk", "contrôle image %llu : tas %u (%s, %.0f Mo) : utilisé %.0f Mo / budget %.0f Mo", (unsigned long long)frame, h,
                 (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "vidéo" : "système",
                 mp.memoryProperties.memoryHeaps[h].size / 1048576.0, bp.heapUsage[h] / 1048576.0, bp.heapBudget[h] / 1048576.0);
    }
    for (auto& pb : g.pools) {
        Readback r{};
        std::vector<VkBufferCopy> cps;
        for (uint64_t o = 0; o + S <= pb.size; o += STEP) {
            r.audit.push_back({pb.base + o, S, (uint64_t)cps.size() * S});
            cps.push_back({o, (uint64_t)cps.size() * S, S});
        }
        if (cps.empty() || !make_buffer(cps.size() * S, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r.b, &r.m))
            continue;
        barrier_all();
        vkCmdCopyBuffer(g.cb, pb.buf, r.b, (uint32_t)cps.size(), cps.data());
        barrier_all();
        r.raw_addr = pb.base;
        r.audit_frame = frame;
        r.path = "contrôle";
        g_readbacks.push_back(std::move(r));
    }
}

// Diagnostic : copie par le GPU d'une plage de mémoire invitée, lue à travers le pool importé ; à la fin
// du travail GPU, comparée au contenu que voit le processeur au même instant (import périmé ?)
// Région NVN {x, y, z, l, h, p} → sous-ressource et étendue Vulkan (z / p = couches des tableaux)
static bool copy_region(const Image& im, uint32_t level, uint32_t layer, const int32_t* r, VkImageSubresourceLayers* sub,
                        VkOffset3D* off, VkExtent3D* ext) {
    const bool arr = im.layers > 1 || im.target == 4 || im.target == 8 || im.target == 9;
    if (r[3] <= 0 || r[4] <= 0 || level >= im.levels) return false;
    const int32_t z = std::max(0, r[2]), d = std::max(1, r[5]);
    *sub = {im.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT ? im.aspect & ~VK_IMAGE_ASPECT_STENCIL_BIT : im.aspect, level,
            arr ? layer + (uint32_t)z : layer, arr ? (uint32_t)d : 1u};
    if (sub->baseArrayLayer + sub->layerCount > im.layers) return false;
    // région bornée au niveau (une copie hors de l'image fait perdre le périphérique)
    const uint32_t lw = std::max(1u, im.w >> level), lh = std::max(1u, im.h >> level);
    const uint32_t x = (uint32_t)std::max(0, r[0]), y = (uint32_t)std::max(0, r[1]);
    if (x >= lw || y >= lh || (!arr && z > 0)) return false;   // volumes 3D : non gérés ici
    *off = {(int32_t)x, (int32_t)y, 0};
    *ext = {std::min((uint32_t)r[3], lw - x), std::min((uint32_t)r[4], lh - y), 1u};
    // formats compressés : décalage et étendue en blocs entiers (sauf bord du niveau)
    if (is_compressed(im.fmt) && ((x | y) & 3)) return false;
    return true;
}

void copy_buffer_to_texture(uint64_t src, uint64_t texkey, uint32_t level, uint32_t layer, const int32_t* region) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    ensure_init();
    if (!g.ok) return;
    begin_frame();
    Image* im = get_image(texkey);
    if (!im) return;
    static const char* no_copy = getenv("RECOMP_NO_COPY_TEX");   // diagnostic : 0 = toutes, sinon largeur cible
    if (no_copy && (atoi(no_copy) == 0 || (uint32_t)atoi(no_copy) == im->w)) return;
    // Ancien contournement (README, limite 28, résolue) : RECOMP_SKIP_COPY_TEX_1X1=1 laisse les textures 1×1
    // par défaut en gris neutre. Le texte en pavés venait de la source lue trop tard (voir plus bas).
    static const bool skip_1x1 = getenv("RECOMP_SKIP_COPY_TEX_1X1") != nullptr;
    if (im->w == 1 && im->h == 1 && skip_1x1) return;
    VkBufferImageCopy c{};
    if (!copy_region(*im, level, layer, region, &c.imageSubresource, &c.imageOffset, &c.imageExtent)) return;
    // données serrées : lignes de blocs 4×4 pour les formats compressés
    const uint32_t unit = is_compressed(im->fmt) ? 4 : 1;
    const uint64_t row = (uint64_t)((c.imageExtent.width + unit - 1) / unit) * im->bpu;
    const uint64_t size = row * ((c.imageExtent.height + unit - 1) / unit) * c.imageExtent.depth * c.imageSubresource.layerCount;
    VkBuffer b;
    uint64_t o;
    if (!find_pool(src, size, &b, &o)) {
        static int n = 0;
        if (n++ < 20) HLOG("vk", "CopyBufferToTexture : source 0x%llx +0x%llx hors des pools", (unsigned long long)src, (unsigned long long)size);
        return;
    }
    {   // une ligne par type de texture cible (taille, couches, format)
        static std::set<std::array<uint32_t, 4>> seen;
        if (seen.size() < 64 && seen.insert({im->w, im->h, im->layers, im->nvnfmt}).second)
            HLOG("vk", "CopyBufferToTexture 0x%llx → %ux%u×%u fmt %u niveau %u couche %u : %d,%d %ux%u ×%u (%llu o)",
                 (unsigned long long)src, im->w, im->h, im->layers, im->nvnfmt, level, c.imageSubresource.baseArrayLayer,
                 c.imageOffset.x, c.imageOffset.y, c.imageExtent.width, c.imageExtent.height, c.imageSubresource.layerCount,
                 (unsigned long long)size);
    }
    if (getenv("RECOMP_COPY_LOG") && size <= 64) {   // diagnostic : octets copiés (petites textures)
        std::string hex;
        for (uint64_t k = 0; k < size; k++) {
            char b[4];
            snprintf(b, sizeof b, "%02x", ((const uint8_t*)src)[k]);
            hex += b;
        }
        HLOG("vk", "  copie vers %ux%u fmt %u (clé %llx) : %s", im->w, im->h, im->nvnfmt, (unsigned long long)texkey, hex.c_str());
    }
    image_to_general(*im);   // contenu initial d'abord, la copie ne doit pas être écrasée ensuite
    end_render();
    // Source figée au moment de la soumission (petites copies) : le GPU n'exécute la copie qu'au vidage
    // suivant, et le jeu réécrit entre-temps son tampon de transfert. Les textures 1×1 par défaut lisaient
    // ainsi (16, …, 92) au lieu du blanc copié : texte en pavés (README, limite 28, résolue).
    static const bool no_snap = getenv("RECOMP_COPY_IN_PLACE") != nullptr;   // diagnostic : ancien comportement
    if (size <= (4u << 20) && !no_snap) {
        VkBuffer sb;
        VkDeviceMemory sm;
        if (make_buffer(std::max<uint64_t>(size, 4), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &sb, &sm)) {
            void* p = nullptr;
            vkMapMemory(g.dev, sm, 0, VK_WHOLE_SIZE, 0, &p);
            memcpy(p, (const void*)src, size);
            vkUnmapMemory(g.dev, sm);
            c.bufferOffset = 0;
            barrier_all();
            vkCmdCopyBufferToImage(g.cb, sb, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            barrier_all();
            g.staging.push_back({sb, sm});
            return;
        }
    }
    // vkCmdCopyBufferToImage : décalage multiple de 4 et de la taille d'un texel ou d'un bloc
    if (o % 4 || o % im->bpu) return;
    c.bufferOffset = o;
    barrier_all();
    vkCmdCopyBufferToImage(g.cb, b, im->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier_all();
}

void clear_texture(uint64_t texkey, uint32_t level, uint32_t layer, const int32_t* region, const float* color) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    ensure_init();
    if (!g.ok) return;
    begin_frame();
    Image* im = get_image(texkey);
    if (!im || is_compressed(im->fmt)) return;
    VkImageSubresourceLayers sub;
    VkOffset3D off;
    VkExtent3D ext;
    if (!copy_region(*im, level, layer, region, &sub, &off, &ext)) return;
    {
        static int n = 0;
        if (n++ < 40)
            HLOG("vk", "ClearTexture %ux%u×%u fmt %u niveau %u : %d,%d %ux%u couleur %g %g %g %g", im->w, im->h, im->layers,
                 im->nvnfmt, sub.mipLevel, off.x, off.y, ext.width, ext.height, color[0], color[1], color[2], color[3]);
    }
    image_to_general(*im);
    end_render();
    barrier_all();
    VkImageSubresourceRange rr{sub.aspectMask, sub.mipLevel, 1, sub.baseArrayLayer, sub.layerCount};
    // région entière seulement (vkCmdClearColorImage n'a pas de rectangle) ; sinon, approximation par
    // la sous-ressource entière, journalisée
    const bool whole = off.x == 0 && off.y == 0 && ext.width >= std::max(1u, im->w >> level) && ext.height >= std::max(1u, im->h >> level);
    if (!whole) {
        static int n = 0;
        if (n++ < 10) HLOG("vk", "ClearTexture partiel %ux%u à %d,%d sur %ux%u : niveau entier effacé", ext.width, ext.height, off.x, off.y, im->w, im->h);
    }
    if (is_depth(im->fmt)) {
        VkClearDepthStencilValue v{color[0], 0};
        vkCmdClearDepthStencilImage(g.cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
    } else {
        VkClearColorValue v;
        memcpy(v.float32, color, 16);
        vkCmdClearColorImage(g.cb, im->img, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &rr);
    }
    barrier_all();
}

// nvnCommandBufferCopyBufferToBuffer dans l'ordre du GPU (vkCmdCopyBuffer entre pools importés). L'ancienne
// copie par le processeur, au moment de la soumission, passait avant les calculs et dessins qui la précèdent
// dans la file : faux dès qu'un calcul écrit la source. Faux si hors des pools ou plages chevauchantes.
bool copy_buffer(uint64_t src, uint64_t dst, uint64_t size) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    ensure_init();
    if (!g.ok || !size) return false;
    if (src < dst + size && dst < src + size) return false;   // chevauchement : copie processeur (memmove)
    VkBuffer sb, db;
    uint64_t so, dof;
    if (!find_pool(src, size, &sb, &so) || !find_pool(dst, size, &db, &dof)) return false;
    begin_frame();
    end_render();
    barrier_all();
    // source figée à la soumission (jusqu'à 4 Mo) : le jeu réécrit son tampon de transfert avant que le
    // GPU n'exécute la copie (même défaut que les textures 1×1) ; l'écriture de la cible reste ordonnée
    VkBuffer stb = VK_NULL_HANDLE;
    VkDeviceMemory stm = VK_NULL_HANDLE;
    if (size <= (4u << 20) && make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &stb, &stm)) {
        void* p = nullptr;
        vkMapMemory(g.dev, stm, 0, VK_WHOLE_SIZE, 0, &p);
        memcpy(p, (const void*)src, size);
        vkUnmapMemory(g.dev, stm);
        sb = stb, so = 0;
        g.staging.push_back({stb, stm});
    }
    VkBufferCopy c{so, dof, size};
    vkCmdCopyBuffer(g.cb, sb, db, 1, &c);
    barrier_all();
    static int nlog = 0;
    if (nlog++ < 6) HLOG("vk", "CopyBufferToBuffer 0x%llx → 0x%llx (%llu o) : sur le GPU", (unsigned long long)src,
                         (unsigned long long)dst, (unsigned long long)size);
    return true;
}

// nvnCommandBufferCopyTextureToTexture, ignoré jusqu'ici (README, limite 18) : le jeu en fait plusieurs par
// image pendant la cinématique ; une copie manquée laisse un contenu périmé dans la texture cible.
// Même taille et formats de même taille de texel : vkCmdCopyImage ; sinon vkCmdBlitImage (drapeaux NVN :
// 1 filtre linéaire, 4 / 8 miroir X / Y). Profondeur ↔ couleur : non pris en charge par Vulkan, journalisé.
void copy_texture_to_texture(uint64_t skey, uint32_t slevel, uint32_t slayer, const int32_t* sr, uint64_t dkey,
                             uint32_t dlevel, uint32_t dlayer, const int32_t* dr, uint32_t flags) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    ensure_init();
    if (!g.ok) return;
    begin_frame();
    static const bool no_c2c = getenv("RECOMP_NO_TEX_COPY") != nullptr;   // diagnostic : ancien comportement
    if (no_c2c) return;
    Image* si = get_image(skey);
    Image* di = get_image(dkey);
    if (!si || !di || si == di) return;
    VkImageSubresourceLayers ss, ds;
    VkOffset3D so, dof;
    VkExtent3D se, de;
    if (!copy_region(*si, slevel, slayer, sr, &ss, &so, &se) || !copy_region(*di, dlevel, dlayer, dr, &ds, &dof, &de)) return;
    const bool sdepth = is_depth(si->fmt), ddepth = is_depth(di->fmt);
    const bool same = se.width == de.width && se.height == de.height && !(flags & 0xc);
    const bool copyable = same && sdepth == ddepth && (si->fmt == di->fmt || (!sdepth && si->bpu == di->bpu &&
                                                                            is_compressed(si->fmt) == is_compressed(di->fmt)));
    const bool blittable = !copyable && sdepth == ddepth && !is_compressed(si->fmt) && !is_compressed(di->fmt) &&
                           (!sdepth || si->fmt == di->fmt);
    // profondeur ↔ couleur (Vulkan interdit vkCmdCopyImage et le blit entre les deux) : par un tampon
    // intermédiaire, octets de texel égaux (aspect profondeur : 2 o en D16, 4 o sinon)
    auto texel = [](const Image* im) -> uint32_t {
        if (!is_depth(im->fmt)) return im->bpu;
        return im->fmt == VK_FORMAT_D16_UNORM ? 2u : im->fmt == VK_FORMAT_S8_UINT ? 1u : 4u;
    };
    if (same && sdepth != ddepth && !is_compressed(si->fmt) && !is_compressed(di->fmt) && texel(si) == texel(di)) {
        {
            static std::set<std::array<uint32_t, 4>> seen;
            if (seen.size() < 16 && seen.insert({si->nvnfmt, di->nvnfmt, se.width, se.height}).second)
                HLOG("vk", "CopyTextureToTexture %ux%u fmt %u → %ux%u fmt %u : profondeur ↔ couleur par tampon", si->w,
                     si->h, si->nvnfmt, di->w, di->h, di->nvnfmt);
        }
        const uint32_t layers = std::min(ss.layerCount, ds.layerCount);
        const uint64_t size = (uint64_t)se.width * se.height * texel(si) * layers;
        VkBuffer b;
        VkDeviceMemory m;
        if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &b, &m))
            return;
        image_to_general(*si);
        image_to_general(*di);
        end_render();
        barrier_all();
        ss.layerCount = ds.layerCount = layers;
        if (sdepth) ss.aspectMask = si->fmt == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        if (ddepth) ds.aspectMask = di->fmt == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        VkBufferImageCopy r1{0, 0, 0, ss, so, {se.width, se.height, 1}};
        vkCmdCopyImageToBuffer(g.cb, si->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r1);
        barrier_all();
        VkBufferImageCopy r2{0, 0, 0, ds, dof, {se.width, se.height, 1}};
        vkCmdCopyBufferToImage(g.cb, b, di->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r2);
        barrier_all();
        g.staging.push_back({b, m});
        return;
    }
    {
        static std::set<std::array<uint32_t, 8>> seen;
        if (seen.size() < 32 && seen.insert({si->w, si->h, si->nvnfmt, di->w, di->h, di->nvnfmt, flags, copyable}).second)
            HLOG("vk", "CopyTextureToTexture %ux%u fmt %u (%d,%d %ux%u) → %ux%u fmt %u (%d,%d %ux%u), drapeaux %u : %s", si->w,
                 si->h, si->nvnfmt, so.x, so.y, se.width, se.height, di->w, di->h, di->nvnfmt, dof.x, dof.y, de.width, de.height,
                 flags, copyable ? "copie" : blittable ? "blit" : "NON PRISE EN CHARGE");
    }
    if (!copyable && !blittable) return;
    image_to_general(*si);
    image_to_general(*di);
    end_render();
    barrier_all();
    ss.layerCount = ds.layerCount = std::min(ss.layerCount, ds.layerCount);
    if (copyable) {
        VkImageCopy c{ss, so, ds, dof, {std::min(se.width, de.width), std::min(se.height, de.height), 1}};
        if (sdepth) c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = si->aspect & di->aspect;
        vkCmdCopyImage(g.cb, si->img, VK_IMAGE_LAYOUT_GENERAL, di->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        VkImageBlit b{};
        b.srcSubresource = ss;
        b.dstSubresource = ds;
        b.srcOffsets[0] = so;
        b.srcOffsets[1] = {so.x + (int32_t)se.width, so.y + (int32_t)se.height, 1};
        b.dstOffsets[0] = dof;
        b.dstOffsets[1] = {dof.x + (int32_t)de.width, dof.y + (int32_t)de.height, 1};
        if (flags & 4) std::swap(b.dstOffsets[0].x, b.dstOffsets[1].x);   // miroir X
        if (flags & 8) std::swap(b.dstOffsets[0].y, b.dstOffsets[1].y);   // miroir Y
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(g.pd, si->fmt, &fp);
        const bool lin = (flags & 1) && !sdepth && (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        vkCmdBlitImage(g.cb, si->img, VK_IMAGE_LAYOUT_GENERAL, di->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       lin ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    barrier_all();
}

void gpu_peek(uint64_t addr, uint64_t size, const char* path) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    if (!g.ok || !size) return;
    VkBuffer src;
    uint64_t off;
    if (!find_pool(addr, size, &src, &off)) {
        HLOG("vk", "relecture GPU %s : 0x%llx +0x%llx hors des pools", path, (unsigned long long)addr, (unsigned long long)size);
        return;
    }
    for (auto& p : g.pools)
        if (p.buf == src) HLOG("vk", "relecture GPU %s : servie par l'import 0x%llx +0x%llx", path, (unsigned long long)p.base, (unsigned long long)p.size);
    {
        std::lock_guard<std::mutex> pl(nvn::g_pool.m);
        for (auto& [k, p] : nvn::g_pool.map)
            if (p.storage && addr >= p.storage && addr < p.storage + p.size)
                HLOG("vk", "relecture GPU %s : pool NVN actuel 0x%llx +0x%llx drapeaux 0x%x", path,
                     (unsigned long long)p.storage, (unsigned long long)p.size, p.flags);
    }
    begin_frame();
    end_render();
    Readback r{};
    r.raw_addr = addr, r.raw_size = size;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r.b, &r.m))
        return;
    barrier_all();
    VkBufferCopy c{off, 0, size};
    vkCmdCopyBuffer(g.cb, src, r.b, 1, &c);
    barrier_all();
    r.path = path;
    g_readbacks.push_back(r);
}

void capture(uint64_t texkey, uint64_t frame, const char* label) {
    std::lock_guard<std::mutex> lk(g_vk_lk);
    if (!g.ok) return;
    Image* im = get_image(texkey);
    if (!im || !im->initialized || is_compressed(im->fmt)) return;
    if (is_depth(im->fmt)) {   // profondeur : copie directe du plan de profondeur (pleine taille)
        if (im->fmt == VK_FORMAT_S8_UINT) return;
        if ((uint64_t)im->w * im->h > 4096ull * 4096) return;   // atlas géants : PNG de 200 Mo inutiles
        begin_frame();
        end_render();
        Readback r{};
        r.w = im->w, r.h = im->h;
        r.depth = im->fmt == VK_FORMAT_D16_UNORM ? 2 : 4;
        r.dfloat = im->fmt == VK_FORMAT_D32_SFLOAT || im->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
        if (!make_buffer((uint64_t)r.w * r.h * r.depth, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r.b, &r.m))
            return;
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
        c.imageExtent = {r.w, r.h, 1};
        vkCmdCopyImageToBuffer(g.cb, im->img, VK_IMAGE_LAYOUT_GENERAL, r.b, 1, &c);
        barrier_all();
        char path[128];
        snprintf(path, sizeof path, "rt_%llu_%03zu_%s_f%u_%ux%u.png", (unsigned long long)frame, g_readbacks.size(), label,
                 im->nvnfmt, im->w, im->h);
        r.path = path;
        g_readbacks.push_back(r);
        return;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(g.pd, im->fmt, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) return;
    begin_frame();
    end_render();
    uint32_t w = std::max(1u, im->w / 2), h = std::max(1u, im->h / 2);
    Readback r{};
    r.w = w, r.h = h;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateImage(g.dev, &ci, nullptr, &r.tmp) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g.dev, r.tmp, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(g.dev, &ai, nullptr, &r.tmpm) != VK_SUCCESS) return;
    vkBindImageMemory(g.dev, r.tmp, r.tmpm, 0);
    if (!make_buffer((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r.b, &r.m))
        return;
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.image = r.tmp;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    vkCmdPipelineBarrier2(g.cb, &di);
    VkImageBlit bl{};
    bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.srcOffsets[1] = {(int32_t)im->w, (int32_t)im->h, 1};
    bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bl.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
    bool integer = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT ? false : true;
    vkCmdBlitImage(g.cb, im->img, VK_IMAGE_LAYOUT_GENERAL, r.tmp, VK_IMAGE_LAYOUT_GENERAL, 1, &bl,
                   integer ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    barrier_all();
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(g.cb, r.tmp, VK_IMAGE_LAYOUT_GENERAL, r.b, 1, &c);
    barrier_all();
    char path[128];
    snprintf(path, sizeof path, "rt_%llu_%03zu_%s_f%u_%ux%u.png", (unsigned long long)frame, g_readbacks.size(), label,
             im->nvnfmt, im->w, im->h);
    r.path = path;
    g_readbacks.push_back(r);
}

// RECOMP_PERF=1 : temps par étape de la présentation (moyennes et pires cas toutes les 120 images)
struct PerfAcc {
    double sum[6] = {}, mx[6] = {};
    int n = 0;
    std::chrono::steady_clock::time_point last{};
};
PerfAcc g_perf;
inline double ms_since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

bool present(uint64_t texkey, uint64_t frame, const int32_t* crop) {
    static const bool perf = getenv("RECOMP_PERF") != nullptr;
    auto t_in = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(g_vk_lk);
    double t_lock = ms_since(t_in);
    ensure_init();
    if (!g.ok) return false;
    begin_frame();
    end_render();
    Image* src = get_image(texkey);
    if (src) image_to_general(*src);
    dbg_tex("présentée", 0, texkey);
    uint32_t idx = 0;
    auto t_acq = std::chrono::steady_clock::now();
    // Swapchain périmée (fenêtre réduite, redimensionnée) : recréée dès que la fenêtre a une taille.
    // Sans image d'écran, le travail de l'image est quand même soumis (et l'instantané pris).
    if (!g.sc_stale && g.sc && g.hwnd) {   // fenêtre redimensionnée (Alt+Entrée, bascule de mode) : nouvelle taille
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.pd, g.surf, &caps) == VK_SUCCESS &&
            caps.currentExtent.width != UINT32_MAX &&
            (caps.currentExtent.width != g.scext.width || caps.currentExtent.height != g.scext.height))
            g.sc_stale = true;
    }
    if (g.sc_stale && g.hwnd) {
        VkSurfaceCapabilitiesKHR caps;
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g.pd, g.surf, &caps) == VK_SUCCESS && caps.currentExtent.width &&
            caps.currentExtent.height) {
            vkDeviceWaitIdle(g.dev);
            for (VkSemaphore sm : g.sem_img) vkDestroySemaphore(g.dev, sm, nullptr);   // recréés avec les images
            g.sem_img.clear();
            VkSwapchainKHR old = g.sc;
            g.sc = VK_NULL_HANDLE;
            g.sc_stale = false;
            if (!create_swapchain(g.hwnd, old)) g.sc = VK_NULL_HANDLE, g.sc_stale = true;
            if (old) vkDestroySwapchainKHR(g.dev, old, nullptr);
            HLOG("vk", "swapchain recréée (%ux%u)", g.scext.width, g.scext.height);
        }
    }
    bool have = false;
    Vk::Slot& sl = g.slots[g.slot];   // emplacement de l'enregistrement en cours (begin_frame ci-dessus)
    if (g.sc && !g.sc_stale) {
        VkResult ar = vkAcquireNextImageKHR(g.dev, g.sc, UINT64_MAX, sl.sem_acq, VK_NULL_HANDLE, &idx);
        have = ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR;
        if (ar == VK_SUBOPTIMAL_KHR || ar == VK_ERROR_OUT_OF_DATE_KHR) g.sc_stale = true;
        static int nwarn = 0;
        if (!have && nwarn++ < 5) HLOG("vk", "acquisition de l'image d'écran impossible (%d) : image non affichée", (int)ar);
    }
    double d_acq = ms_since(t_acq);
    VkImage dst = have ? g.scimgs[idx] : VK_NULL_HANDLE;
    VkImageMemoryBarrier2 ib{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    ib.srcStageMask = ib.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    ib.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.image = dst;
    ib.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &ib;
    if (have) vkCmdPipelineBarrier2(g.cb, &di);
    if (!have) {
    } else if (src) {
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // origine bas-gauche : l'image est rangée retournée en mémoire
        int cx = 0, cy = 0, cw = (int)src->w, ch = (int)src->h;
        if (crop && crop[2] > 0 && crop[3] > 0)   // zone présentée (nvnWindowSetCrop)
            cx = crop[0], cy = crop[1], cw = std::min(crop[2], (int)src->w - cx), ch = std::min(crop[3], (int)src->h - cy);
        if (nvn::g_origin_upper_left) bl.srcOffsets[0] = {cx, cy, 0}, bl.srcOffsets[1] = {cx + cw, cy + ch, 1};
        else bl.srcOffsets[0] = {cx, cy + ch, 0}, bl.srcOffsets[1] = {cx + cw, cy, 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        // Format conservé (bandes noires si la fenêtre n'a pas les proportions de l'image) ; copie au plus
        // proche quand les tailles coïncident (pixel pour pixel), linéaire sinon.
        const int W = (int)g.scext.width, H = (int)g.scext.height;
        int dw = W, dh = H;
        if (cw > 0 && ch > 0) {
            if ((int64_t)W * ch > (int64_t)H * cw) dw = (int)((int64_t)H * cw / ch);
            else dh = (int)((int64_t)W * ch / cw);
        }
        const int dx = (W - dw) / 2, dy = (H - dh) / 2;
        if (dw != W || dh != H) {
            VkClearColorValue black{{0.0f, 0.0f, 0.0f, 1.0f}};
            vkCmdClearColorImage(g.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &ib.subresourceRange);
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            mb.srcAccessMask = mb.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            VkDependencyInfo mdi{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            mdi.memoryBarrierCount = 1;
            mdi.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(g.cb, &mdi);
        }
        bl.dstOffsets[0] = {dx, dy, 0};
        bl.dstOffsets[1] = {dx + dw, dy + dh, 1};
        const bool exact = dw == cw && dh == ch;
        vkCmdBlitImage(g.cb, src->img, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       exact ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        static int nlog = 0;
        if (nlog < 4 && (W != g.last_present_w || H != g.last_present_h)) {
            nlog++;
            g.last_present_w = W, g.last_present_h = H;
            HLOG("vk", "présentation : image %dx%d → fenêtre %dx%d (zone %dx%d en %d,%d, %s)", cw, ch, W, H, dw, dh, dx, dy,
                 exact ? "pixel pour pixel" : "mise à l'échelle linéaire");
        }
    } else {
        VkClearColorValue c{{0.1f, 0.0f, 0.1f, 1.0f}};
        vkCmdClearColorImage(g.cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &ib.subresourceRange);
    }
    ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    ib.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    ib.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = 0;
    if (have) vkCmdPipelineBarrier2(g.cb, &di);
    static const long audit_every = getenv("RECOMP_POOL_AUDIT") ? atol(getenv("RECOMP_POOL_AUDIT")) : 0;
    if (audit_every > 0 && frame % (uint64_t)audit_every == 0) pool_audit(frame);
    vkEndCommandBuffer(g.cb);
    g.recording = false;
    // fin de copie vers l'image d'écran : un sémaphore par image (réutilisé quand l'image revient)
    VkSemaphore done_sem = VK_NULL_HANDLE;
    if (have) {
        while (g.sem_img.size() <= idx) {
            VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkSemaphore sm = VK_NULL_HANDLE;
            vkCreateSemaphore(g.dev, &sci, nullptr, &sm);
            g.sem_img.push_back(sm);
        }
        done_sem = g.sem_img[idx];
    }
    auto t_sub = std::chrono::steady_clock::now();
    g.cur_frame = frame;
    const uint64_t serial = submit_cb("présentation", have ? sl.sem_acq : VK_NULL_HANDLE, done_sem);
    double d_sub = ms_since(t_sub);
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &done_sem;
    pi.swapchainCount = 1;
    pi.pSwapchains = &g.sc;
    pi.pImageIndices = &idx;
    auto t_pr = std::chrono::steady_clock::now();
    if (have) {
        VkResult pr = vkQueuePresentKHR(g.q, &pi);
        if (pr == VK_SUBOPTIMAL_KHR || pr == VK_ERROR_OUT_OF_DATE_KHR) g.sc_stale = true;
    }
    double d_pr = ms_since(t_pr);
    auto t_f = std::chrono::steady_clock::now();
    // soumissions asynchrones : pas d'attente, sauf pour relire l'état du GPU (profileur, tampon de débogage)
    if (!g.async || g.prof || g.dbgptr) wait_serial(serial, "présentation");
    prof_frame(frame);   // RECOMP_GPU_PROFILE : bilan de l'image profilée, armement de la suivante
    if (g.dbgptr && g.dbgptr[1]) {   // RECOMP_DBG_SSBO : enregistrements (4 mots) rangés par le shader surveillé
        static int nd = 0;
        uint32_t n = std::min<uint32_t>(g.dbgptr[1], 4000);
        std::map<std::array<uint32_t, 4>, uint32_t> uniq;
        for (uint32_t k = 0; k < n; k++)
            uniq[{g.dbgptr[4 + 4 * k], g.dbgptr[5 + 4 * k], g.dbgptr[6 + 4 * k], g.dbgptr[7 + 4 * k]}]++;
        static std::string lastv;
        std::string key;
        for (auto& [v, cnt] : uniq) key += std::to_string(v[0]) + "," + std::to_string(v[1]) + ";";
        if (key != lastv && nd++ < 200) {
            lastv = key;
            std::string l;
            int c = 0;
            for (auto& [v, cnt] : uniq) {
                if (c++ >= 12) break;
                char b[96];
                snprintf(b, sizeof b, " [%08x %08x %08x %08x]×%u", v[0], v[1], v[2], v[3], cnt);
                l += b;
            }
            HLOG("dbg", "image %llu : %u passages, %u enregistrés, %zu valeurs distinctes :%s", (unsigned long long)frame, g.dbgptr[0],
                 g.dbgptr[1], uniq.size(), l.c_str());
        }
        memset(g.dbgptr, 0, 16 + 16 * 4000);
    }
    double d_f = ms_since(t_f);
    if (perf) {
        auto now = std::chrono::steady_clock::now();
        double iv = g_perf.last.time_since_epoch().count() ? std::chrono::duration<double, std::milli>(now - g_perf.last).count() : 0;
        g_perf.last = now;
        double v[6] = {iv, t_lock, d_acq, d_sub, d_pr, d_f};
        for (int i = 0; i < 6; i++) g_perf.sum[i] += v[i], g_perf.mx[i] = std::max(g_perf.mx[i], v[i]);
        if (++g_perf.n == 120) {
            const char* nm[6] = {"intervalle", "verrou", "acquisition", "soumission", "présentation", "attente GPU"};
            std::string o;
            for (int i = 0; i < 6; i++) {
                char b[96];
                snprintf(b, sizeof b, " %s %.2f/%.2f", nm[i], g_perf.sum[i] / 120, g_perf.mx[i]);
                o += b;
            }
            HLOG("perf", "ms moy/max :%s | %llu dessins | vidages %.2f ms/image (%llu) | attentes du GPU %.2f ms/image, anneau %.1f Mo au plus",
                 o.c_str(), (unsigned long long)g.frame_draws, g_flush_us.exchange(0) / 120e3, (unsigned long long)g_flush_n.exchange(0),
                 g_wait_us.exchange(0) / 120e3, g.ring_peak / 1048576.0);
            g.ring_peak = 0;
            HLOG("perf", "draw : %.1f appels/image, ms/image : prog %.3f textures %.3f passe %.3f pipeline %.3f descripteurs %.3f fin %.3f",
                 g_dn.exchange(0) / 120.0, g_dt[0].exchange(0) / 120e6, g_dt[1].exchange(0) / 120e6, g_dt[2].exchange(0) / 120e6,
                 g_dt[3].exchange(0) / 120e6, g_dt[4].exchange(0) / 120e6, g_dt[5].exchange(0) / 120e6);
            g_perf = PerfAcc();
            g_perf.last = now;
        }
    }
    static const long dbg_frame = getenv("RECOMP_VK_DEBUG") ? atol(getenv("RECOMP_VK_DEBUG")) : -1;
    g.dbg = (long)frame + 1 == dbg_frame;
    static const long texdump_frame = getenv("RECOMP_TEX_DUMP") ? atol(getenv("RECOMP_TEX_DUMP")) : -1;
    if ((long)frame == texdump_frame) tex_dump();
    // RECOMP_SNAPSHOT_EVERY=n : un instantané toutes les n images (aperçu vidéo, voir RECOMP_SNAPSHOT_DIR)
    static const long snap_every = getenv("RECOMP_SNAPSHOT_EVERY") ? atol(getenv("RECOMP_SNAPSHOT_EVERY")) : 0;
    if (snap_every > 0 && src && frame % (uint64_t)snap_every == 0) snapshot(*src, frame, crop);
    static const char* snap_env = getenv("RECOMP_SNAPSHOT");
    if (snap_env && src) {
        for (const char* q = snap_env; *q;) {
            uint64_t n = strtoull(q, (char**)&q, 10);
            if (n == frame) snapshot(*src, frame, crop);
            if (*q == ',') q++;
            else break;
        }
    }
    // RECOMP_RECORD_DIR : extraits vidéo des cinématiques pour le launcher (record.cpp)
    if (src && rec::wanted(frame)) {
        uint32_t w2, h2;
        if (readback_half(*src, crop, w2, h2)) rec::frame(g_snap.ptr, w2, h2, frame);
    }
    if (frame % 60 == 0) evict_unreferenced();   // images sans objet devenues anciennes depuis leur libération
    if (frame % 600 == 0) pcache_save_later();   // pipelines créés depuis la dernière sauvegarde (au plus 1 par minute)
    if (frame == 1 || frame % 600 == 0)
        HLOG("vk", "image %llu : %llu dessins, %llu ignorés, %zu pipelines, %zu shaders absents, %llu textures envoyées (%.0f Mo, dont %llu renvois), %zu images (%llu libérées, %.0f Mo sans objet)%s",
             (unsigned long long)frame, (unsigned long long)g.frame_draws, (unsigned long long)g.frame_skipped,
             g.pipelines.size(), g.missing.size(), (unsigned long long)g.tex_uploads, g.tex_upload_bytes / 1048576.0,
             (unsigned long long)g.tex_reuploads, g.images.size(), (unsigned long long)g.images_freed,
             g.unref_bytes / 1048576.0, vram_text().c_str());
    g.frame_draws = g.frame_skipped = 0;
    return true;
}

}  // namespace vkr
}  // namespace hos
