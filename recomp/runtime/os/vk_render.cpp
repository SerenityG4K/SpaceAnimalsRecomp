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
#include "settings.h"

// runtime.cpp : pause du compteur du jeu pendant un long blocage du pilote. Pas déclarées dans cpu.h : cet en-tête
// entre dans l'empreinte de chaque objet du jeu recompilé (build.py, GEN_HEADERS) ; y toucher ferait recompiler tout
// le jeu chez les joueurs.
void rt_stall_begin();
void rt_stall_end();

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
    // source rendue à Windows après l'envoi (tex_release) : pages [rel_d, rel_f), numéro du rendu, empreinte des
    // octets de la source hors de ces pages ; rel_n : nombre de rendus de cette image
    uint64_t rel_d = 0, rel_f = 0, rel_id = 0, edge_hash = 0;
    uint8_t rel_n = 0;
    // objets texture vivants du jeu qui l'utilisent (celui qui l'a créée + ceux qui la partagent) ; à 0, l'image
    // reste réemployable (by_addr) dans la limite du cache des images sans objet (texture_released)
    uint32_t refs = 0;
    uint64_t bytes = 0;      // mémoire vidéo de l'image
    uint64_t last_use = 0;   // dernière image (g.cur_frame) où un dessin l'a demandée
    uint64_t up_frame = 0;   // image (g.cur_frame) du dernier envoi du contenu (diagnostic RECOMP_TEX_AUDIT)
    // plages vidées par le jeu (pool_flushed) : numéro du dernier vidage connu quand le contenu a été lu pour la
    // dernière fois (envoi ou contrôle), et image affichée du dernier contrôle (un par image affichée)
    uint64_t chk_serial = 0, chk_frame = 0;
};
struct PoolBuf {
    uint64_t base = 0, size = 0;
    uint64_t dev = 0;   // adresse GPU Vulkan de base (vkGetBufferDeviceAddress)
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    uint64_t last = 0;   // dernière image (g.cur_frame) où un dessin, une copie ou un calcul l'a demandé
    bool calc = false;   // lu par un calcul à mémoire globale : gardé tant que son pool vit, en tête de sa table
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
    // Shaders de calcul : sous-groupes de 32 voies imposés (VkPipelineShaderStageRequiredSubgroupSizeCreateInfo) quand
    // la carte en prend d'autres par défaut. Six shaders de calcul du jeu (dont terrain.clutter_generate et
    // terrain.clutter_cull) votent, comptent et échangent entre les voies d'un « warp » de 32 de la console, avec des
    // masques de 32 bits ; NVIDIA a des sous-groupes de 32, AMD de 64 par défaut (32 à 64 permis en calcul).
    bool sg32 = false;
    // VK_KHR_shader_quad_control actif : les shaders de fragments marqués full_quads / quad_derivatives (glsl.py,
    // QUAD_CONTROL) sont chargés tels quels ; sinon ces marques sont retirées du SPIR-V (spirv_sans_quad_control).
    bool quad_control = false;
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
    std::vector<PoolBuf> pools;                     // imports de la mémoire du jeu (pools entiers ou fenêtres)
    uint64_t pools_gen = 1;                         // change à chaque import ajouté ou retiré (cache de find_pool)
    size_t pools_mru[4] = {SIZE_MAX, SIZE_MAX, SIZE_MAX, SIZE_MAX};
    uint64_t pools_mru_gen = 0;
    unsigned pools_mru_n = 0;
    uint64_t imported = 0;                          // octets de mémoire du jeu importés (verrouillés en RAM)
    uint64_t imports_total = 0, imports_evinces = 0, imports_refuses = 0;
    std::unordered_map<uint64_t, uint64_t> import_refus;   // base refusée → image du refus (pas de nouvel essai aussitôt)
    // retour des calculs à mémoire globale : adresses absentes de leur table (glob_retour)
    VkBuffer fb_buf = VK_NULL_HANDLE;
    VkDeviceMemory fb_mem = VK_NULL_HANDLE;
    uint32_t* fb_ptr = nullptr;
    uint64_t fb_dev = 0;
    uint64_t glob_retours = 0;
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
    uint64_t libs_on_demand = 0;              // bibliothèques de shaders créées pendant le jeu, au premier dessin
    bool libs_sur_demande = false;            // recomp.ini, shaders_on_demand : c'est le cas normal (voir ensure_init)
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
    // écritures du jeu de textures en attente : faites en un seul appel à la soumission (voir tex_write)
    struct TexWrite { uint32_t binding, element; VkDescriptorType type; VkDescriptorImageInfo ii; };
    std::vector<TexWrite> tex_pending;
    uint64_t tex_writes = 0, tex_write_calls = 0;   // journal : emplacements écrits, appels à vkUpdateDescriptorSets
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
    uint64_t staging_bytes = 0;   // octets des tampons de transit de l'enregistrement en cours (voir borner_transit)
    // données des textures rendues (tex_release) : octets gardés parce qu'un import les verrouille, réemplois d'une
    // image sans relire sa source, nombre de rendus
    uint64_t tex_rel_import = 0, tex_rel_intactes = 0, tex_rel_rendus = 0;
    uint64_t tex_uploads = 0, tex_upload_bytes = 0, tex_reuploads = 0;   // envois, dont renvois (contenu changé)
    // contrôles du contenu à l'usage (flush_recheck), dont renvois ; par cause : 1 plage vidée par le jeu, 2 image sans
    // objet dont les données rendues ont été retouchées, 3 image sans objet, contrôle périodique
    uint64_t flush_checks[4] = {}, flush_reuploads[4] = {};
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
    uint64_t unref_reuses = 0, unref_reuse_bytes = 0;      // journal : images sans objet reprises par un nouvel objet
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

// ================================================================ suivi des allocations (diagnostic)
// RECOMP_MEM_MAP=1 : vkAllocateMemory et vkFreeMemory passent par ces deux fonctions, qui tiennent le compte des octets
// vivants par genre de mémoire (0 : dédiée au GPU, 1 : visible de l'hôte, 2 : mémoire du jeu importée). La carte de
// la mémoire engagée (mem_map_log) les écrit : on voit ainsi si un saut de mémoire vient de nos allocations Vulkan
// (cibles de rendu, textures, tampons) ou du pilote lui-même.
struct VkSuivi {
    std::mutex m;
    std::map<VkDeviceMemory, std::pair<uint64_t, int>> vivants;
    uint64_t octets[3] = {}, nombre[3] = {};
};
VkSuivi g_vk_suivi;
PFN_vkAllocateMemory vrai_vkAllocateMemory = nullptr;
PFN_vkFreeMemory vrai_vkFreeMemory = nullptr;
VKAPI_ATTR VkResult VKAPI_CALL suivi_vkAllocateMemory(VkDevice d, const VkMemoryAllocateInfo* ai,
                                                      const VkAllocationCallbacks* a, VkDeviceMemory* m) {
    const VkResult r = vrai_vkAllocateMemory(d, ai, a, m);
    if (r != VK_SUCCESS) return r;
    int genre = 0;
    for (const VkBaseInStructure* q = (const VkBaseInStructure*)ai->pNext; q; q = q->pNext)
        if (q->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT) genre = 2;
    if (!genre && ai->memoryTypeIndex < g.memprops.memoryTypeCount &&
        (g.memprops.memoryTypes[ai->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        genre = 1;
    std::lock_guard<std::mutex> lk(g_vk_suivi.m);
    g_vk_suivi.vivants[*m] = {ai->allocationSize, genre};
    g_vk_suivi.octets[genre] += ai->allocationSize;
    g_vk_suivi.nombre[genre]++;
    return r;
}
VKAPI_ATTR void VKAPI_CALL suivi_vkFreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks* a) {
    if (m) {
        std::lock_guard<std::mutex> lk(g_vk_suivi.m);
        auto it = g_vk_suivi.vivants.find(m);
        if (it != g_vk_suivi.vivants.end()) {
            g_vk_suivi.octets[it->second.second] -= it->second.first;
            g_vk_suivi.nombre[it->second.second]--;
            g_vk_suivi.vivants.erase(it);
        }
    }
    vrai_vkFreeMemory(d, m, a);
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
    if (getenv("RECOMP_MEM_MAP") && vkAllocateMemory && vkFreeMemory) {   // suivi des allocations (diagnostic)
        vrai_vkAllocateMemory = vkAllocateMemory, vrai_vkFreeMemory = vkFreeMemory;
        vkAllocateMemory = suivi_vkAllocateMemory, vkFreeMemory = suivi_vkFreeMemory;
    }
    return vkCmdDraw != nullptr;
}

void imports_init();   // tampon de retour des calculs et journal du budget des imports (défini plus bas)
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
        HLOG("vk", "display %ux%u at %u Hz; present modes:%s", (unsigned)dm.dmPelsWidth, (unsigned)dm.dmPelsHeight,
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
        HLOG("vk", "vulkan-1.dll not found");
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
    // GPU dédié de préférence. RECOMP_GPU=<partie du nom> (essais) : une autre carte, par exemple la carte AMD ou
    // Intel intégrée d'un PC qui a aussi une carte dédiée ; sans correspondance, le choix habituel.
    const char* voulu = getenv("RECOMP_GPU");
    auto minuscules = [](std::string s) {
        for (char& c : s) c = (char)tolower((unsigned char)c);
        return s;
    };
    bool choisi = false;
    for (auto p : pds) {
        VkPhysicalDeviceProperties pr;
        vkGetPhysicalDeviceProperties(p, &pr);
        if (pds.size() > 1)
            HLOG("vk", "GPU available: %s%s", pr.deviceName,
                 pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? " (discrete)"
                 : pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? " (integrated)" : "");
        if (choisi) continue;
        if (voulu && *voulu && minuscules(pr.deviceName).find(minuscules(voulu)) != std::string::npos) {
            g.pd = p, g.props = pr;
            choisi = true;
        } else if (!g.pd || pr.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            g.pd = p, g.props = pr;
        }
    }
    if (voulu && *voulu && !choisi) HLOG("vk", "RECOMP_GPU=%s: no GPU with that name, usual choice", voulu);
    if (!g.pd) return false;
    vkGetPhysicalDeviceMemoryProperties(g.pd, &g.memprops);
    HLOG("vk", "GPU: %s (Vulkan %u.%u)", g.props.deviceName, VK_API_VERSION_MAJOR(g.props.apiVersion),
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
        HLOG("vk", "VK_EXT_external_memory_host missing: Vulkan rendering disabled");
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
    HLOG("vk", "pipelines: %s", g.gpl ? "precompiled shader libraries, fast linking (VK_EXT_graphics_pipeline_library)"
                                       : "full pipelines, compiled at the first draw (no fast-linking libraries)");
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
    {
        VkPhysicalDeviceVulkan13Properties p13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES};
        VkPhysicalDeviceVulkan11Properties p11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES};
        p11.pNext = &p13;
        VkPhysicalDeviceProperties2 qp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        qp.pNext = &p11;
        vkGetPhysicalDeviceProperties2(g.pd, &qp);
        g.sg32 = p11.subgroupSize != 32 && p13.minSubgroupSize <= 32 && p13.maxSubgroupSize >= 32 &&
                 (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) && !getenv("RECOMP_NO_SG32");
        if (g.sg32) {
            f13.subgroupSizeControl = VK_TRUE;
            HLOG("vk", "compute shaders: subgroups of 32 lanes required, like the console's warps (this GPU's default: %u)",
                 p11.subgroupSize);
        } else if (p11.subgroupSize != 32) {
            HLOG("vk", "compute shaders: subgroups of %u lanes, 32 cannot be required on this GPU", p11.subgroupSize);
        }
    }
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
    // Carrés de pixels complets : pixels auxiliaires actifs dans les votes et dérivées définies par carré (voir
    // glsl.py, QUAD_CONTROL). Pas sur NVIDIA, où les sous-groupes d'un shader de fragments suivent déjà les carrés et
    // où le rendu est validé sans. RECOMP_QUAD_CONTROL=0 : jamais ; =1 : dès que le pilote l'offre, NVIDIA compris.
    VkPhysicalDeviceShaderQuadControlFeaturesKHR qcf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_QUAD_CONTROL_FEATURES_KHR};
    VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR mrf{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_MAXIMAL_RECONVERGENCE_FEATURES_KHR};
    if (has(VK_KHR_SHADER_QUAD_CONTROL_EXTENSION_NAME) && has(VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME)) {
        VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        q.pNext = &qcf;
        qcf.pNext = &mrf;
        vkGetPhysicalDeviceFeatures2(g.pd, &q);
        const char* e = getenv("RECOMP_QUAD_CONTROL");
        g.quad_control = qcf.shaderQuadControl && mrf.shaderMaximalReconvergence &&
                         (e ? atoi(e) != 0 : g.props.vendorID != 0x10de);
        qcf.pNext = nullptr;
    }
    if (g.quad_control) {
        dext.push_back(VK_KHR_SHADER_QUAD_CONTROL_EXTENSION_NAME);
        dext.push_back(VK_KHR_SHADER_MAXIMAL_RECONVERGENCE_EXTENSION_NAME);
        qcf.shaderQuadControl = VK_TRUE;
        mrf.shaderMaximalReconvergence = VK_TRUE;
        mrf.pNext = f2.pNext;
        qcf.pNext = &mrf;
        f2.pNext = &qcf;
        HLOG("vk", "fragment shaders: full pixel quads (VK_KHR_shader_quad_control), helper pixels take part in votes");
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
    HLOG("vk", "uniform buffers: alignment %llu, max size %u, %u per stage",
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
    imports_init();
    g.cb = g.slots[0].cb, g.ring = g.slots[0].ring, g.ringptr = g.slots[0].ringptr;
    // diagnostics qui relisent l'état du GPU juste après chaque soumission : soumissions synchrones
    g.async = !getenv("RECOMP_SYNC_SUBMIT") && !getenv("RECOMP_SYNC_FROM") && !getenv("RECOMP_GPU_DIAG") &&
              !getenv("RECOMP_DRAW_WATCH");
    HLOG("vk", "submissions %s (%d slots, constants ring of %llu MB each)",
         g.async ? "asynchronous" : "synchronous (wait for the GPU at each flush)", Vk::NSLOT,
         (unsigned long long)(g.ringsize >> 20));
    HLOG("vk", "Vulkan rendering ready (host memory import aligned on 0x%llx, depth_clip_control %d)",
         (unsigned long long)g.host_align, g.depth_clip_control);
    return true;
}

// ================================================================ mémoire invitée → tampons
// ---- imports de la mémoire du jeu, par fenêtres
// Le GPU lit les tampons du jeu (sommets, index, constantes, stockage) directement dans sa mémoire, importée par
// VK_EXT_external_memory_host. Jusqu'à la 1.0.2, chaque pool NVN était importé en entier, et tous l'étaient au premier
// calcul à mémoire globale : 9,3 Go verrouillés en RAM (un pool de 3 Go, deux de 1 Go…), que le jeu s'en serve ou non,
// et autant de mémoire engagée. Sur un PC de 16 Go, le pilote refusait la suite (-2) : écran noir à la cinématique
// (issue n° 5). Un grand pool est maintenant importé par fenêtres de 32 Mo, à la demande : seule la mémoire que le GPU
// lit vraiment est verrouillée. Quand le budget est atteint, les fenêtres qui ne servent plus depuis 2 s sont rendues.
constexpr uint64_t IMPORT_CELL = 32ull << 20;        // fenêtre d'un grand pool
constexpr uint64_t IMPORT_WHOLE_MAX = 64ull << 20;   // pools importés en entier jusqu'à cette taille

// Budget des imports : RECOMP_IMPORT_MAX_MO (essais), sinon 40 % de la RAM du PC.
uint64_t import_budget() {
    static const uint64_t budget = [] {
        if (!settings().memory_saver) return (uint64_t)UINT64_MAX;   // recomp.ini : comme en 1.0.2, rien n'est rendu
        if (const char* e = getenv("RECOMP_IMPORT_MAX_MO")) return (uint64_t)strtoull(e, nullptr, 10) << 20;
        MEMORYSTATUSEX ms{};
        ms.dwLength = sizeof ms;
        GlobalMemoryStatusEx(&ms);
        return (uint64_t)(ms.ullTotalPhys * 0.4);
    }();
    return budget;
}

// Retire un import : ses objets Vulkan sont détruits quand le GPU a fini le travail déjà enregistré (begin_frame).
void retire_import(size_t i) {
    const PoolBuf p = g.pools[i];
    g.imported -= std::min(g.imported, p.size);
    g.dead_pools.emplace_back(g.submitted + (g.recording ? 1 : 0), p);
    g.pools.erase(g.pools.begin() + i);
    g.pools_gen++;
}

// Rend la fenêtre la plus anciennement utilisée, sauf celles des calculs et celles lues depuis moins de `age` images.
bool evict_import(uint64_t age) {
    size_t best = SIZE_MAX;
    for (size_t i = 0; i < g.pools.size(); i++) {
        const PoolBuf& p = g.pools[i];
        if (p.calc || g.cur_frame - p.last < age) continue;
        if (best == SIZE_MAX || p.last < g.pools[best].last) best = i;
    }
    if (best == SIZE_MAX) return false;
    retire_import(best);
    g.imports_evinces++;
    return true;
}

// Importe [base, base + size) (aligné sur host_align, dans un seul pool NVN). nullptr : refusé (noté, pas de nouvel
// essai avant une seconde).
PoolBuf* import_range(uint64_t base, uint64_t size, const nvn::Pool& found) {
    auto rf = g.import_refus.find(base);
    if (rf != g.import_refus.end()) {
        if (g.cur_frame - rf->second < 60) return nullptr;
        g.import_refus.erase(rf);
    }
    while (g.imported + size > import_budget() && evict_import(120)) {}
    PoolBuf pb;
    pb.base = base;
    pb.size = size;
    pb.last = g.cur_frame;
    auto refus = [&](const char* quoi, int r) -> PoolBuf* {
        g.import_refus[base] = g.cur_frame;
        if (g.imports_refuses++ < 12 || g.imports_refuses % 1000 == 0)
            HLOG("vk", "import 0x%llx (+0x%llx) refused: %s (%d); %.0f MB imported in %zu windows, %llu refusals",
                 (unsigned long long)base, (unsigned long long)size, quoi, r, g.imported / 1048576.0, g.pools.size(),
                 (unsigned long long)g.imports_refuses);
        while (evict_import(2)) {}   // rend ce qui ne sert pas à l'image en cours : la place revient quand le GPU a fini
        return nullptr;
    };
    // pages du jeu engagées à la demande (memory.cpp) : le pilote ne verrouille que des pages engagées. Garde du noyau
    // de l'engagement à l'épinglage de la fenêtre : aucune page vide n'en est rendue à Windows entre-temps.
    MemNoyau garde;
    if (!mem_ensure(base, size)) return refus("not enough PC memory", 0);
    VkMemoryHostPointerPropertiesEXT hpp{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkResult r = vkGetMemoryHostPointerPropertiesEXT(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                     (void*)base, &hpp);
    if (r != VK_SUCCESS) return refus("host pointer", (int)r);
    VkExternalMemoryBufferCreateInfo ebi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ebi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.pNext = &ebi;
    bi.size = pb.size;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
               VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    r = vkCreateBuffer(g.dev, &bi, nullptr, &pb.buf);
    if (r != VK_SUCCESS) return refus("vkCreateBuffer", (int)r);
    VkImportMemoryHostPointerInfoEXT imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
    imp.pHostPointer = (void*)base;
    VkMemoryAllocateFlagsInfo maf{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};   // adresse GPU de la fenêtre (LDG/STG)
    maf.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    imp.pNext = &maf;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &imp;
    ai.allocationSize = pb.size;
    ai.memoryTypeIndex = mem_type(hpp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    r = vkAllocateMemory(g.dev, &ai, nullptr, &pb.mem);
    if (r != VK_SUCCESS) {
        vkDestroyBuffer(g.dev, pb.buf, nullptr);
        return refus("vkAllocateMemory", (int)r);
    }
    vkBindBufferMemory(g.dev, pb.buf, pb.mem, 0);
    {
        VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        dai.buffer = pb.buf;
        pb.dev = vkGetBufferDeviceAddress(g.dev, &dai);
    }
    g.imported += pb.size;
    if (g.imports_total++ < 40 || g.imports_total % 100 == 0)
        HLOG("vk", "import #%llu: 0x%llx +0x%llx (NVN pool 0x%llx +0x%llx, flags 0x%x; memory type %u, properties "
                   "0x%x); %.0f MB imported in %zu windows",
             (unsigned long long)g.imports_total, (unsigned long long)base, (unsigned long long)pb.size,
             (unsigned long long)found.storage, (unsigned long long)found.size, found.flags, ai.memoryTypeIndex,
             ai.memoryTypeIndex < g.memprops.memoryTypeCount ? (unsigned)g.memprops.memoryTypes[ai.memoryTypeIndex].propertyFlags : 0u,
             g.imported / 1048576.0, g.pools.size() + 1);
    mem_epingler(base, pb.size);   // le GPU y lit et écrit : jamais rendue tant que la mémoire importée existe
    g.pools.push_back(pb);
    g.pools_gen++;
    return &g.pools.back();
}

// Import qui couvre [addr, addr + size) parmi ceux qui existent (nullptr : aucun). Les derniers trouvés d'abord : les
// dessins d'une image reprennent les mêmes tampons.
PoolBuf* import_existant(uint64_t addr, uint64_t size) {
    if (g.pools_mru_gen != g.pools_gen) {
        g.pools_mru_gen = g.pools_gen;
        for (size_t& m : g.pools_mru) m = SIZE_MAX;
    }
    for (size_t m : g.pools_mru)
        if (m < g.pools.size() && addr >= g.pools[m].base && addr + size <= g.pools[m].base + g.pools[m].size)
            return &g.pools[m];
    for (size_t i = 0; i < g.pools.size(); i++)
        if (addr >= g.pools[i].base && addr + size <= g.pools[i].base + g.pools[i].size) {
            g.pools_mru[g.pools_mru_n++ % 4] = i;
            return &g.pools[i];
        }
    return nullptr;
}

PoolBuf* find_import(uint64_t addr, uint64_t size) {
    if (!size) size = 1;
    if (PoolBuf* p = import_existant(addr, size)) {
        p->last = g.cur_frame;
        return p;
    }
    // pool NVN qui contient l'adresse ; la plage est bornée à sa fin
    nvn::Pool found;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(nvn::g_pool.m);
        for (auto& [k, p] : nvn::g_pool.map)
            if (p.storage && addr >= p.storage && addr < p.storage + p.size) {
                found = p;
                ok = true;
                break;
            }
    }
    if (!ok) return nullptr;
    if (addr + size > found.storage + found.size) {
        size = found.storage + found.size - addr;
        if (PoolBuf* p = import_existant(addr, size)) {
            p->last = g.cur_frame;
            return p;
        }
    }
    const uint64_t pbase = found.storage & ~(g.host_align - 1);
    const uint64_t pend = (found.storage + found.size + g.host_align - 1) & ~(g.host_align - 1);
    uint64_t base = pbase, end = pend;
    if (settings().memory_saver && pend - pbase > IMPORT_WHOLE_MAX) {   // grand pool : les fenêtres de 32 Mo touchées
        base = std::max(pbase, addr & ~(IMPORT_CELL - 1));
        end = std::min(pend, (addr + size + IMPORT_CELL - 1) & ~(IMPORT_CELL - 1));
    }
    return import_range(base, end - base, found);
}

bool find_pool(uint64_t addr, uint64_t size, VkBuffer* buf, uint64_t* off) {
    PoolBuf* p = find_import(addr, size);
    if (!p) return false;
    *buf = p->buf;
    *off = addr - p->base;
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
// RECOMP_UNREF_CAP_MO : plafond en Mo (essais : mesurer ce que ce cache évite vraiment de renvoyer au GPU).
const uint64_t UNREF_CAP = [] {
    const char* e = getenv("RECOMP_UNREF_CAP_MO");
    return (e ? strtoull(e, nullptr, 10) : 1536ull) << 20;
}();
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
    if (o.refs++ == 0) {
        // de nouveau un objet texture : une écriture du jeu dans ses données rendues les restaure (memory.cpp)
        if (o.rel_id) mem_release_vivant(o.rel_d, o.rel_f - o.rel_d, o.rel_id, true);
        g.unref_bytes -= std::min(g.unref_bytes, o.bytes);
        g.unref_reuses++;
        g.unref_reuse_bytes += o.bytes;
    }
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
            HLOG("vk", "pool freed 0x%llx +0x%llx: %zu image(s) freed (%.0f MB)", (unsigned long long)addr,
                 (unsigned long long)size, dk.size(), bytes / 1048576.0);
    }
    size_t n = 0;
    for (size_t i = 0; i < g.pools.size();) {
        const PoolBuf& p = g.pools[i];
        if (p.base < addr + size && addr < p.base + p.size) {
            retire_import(i);
            n++;
        } else {
            i++;
        }
    }
    g.import_refus.clear();
    static int nlog = 0;
    if (n && nlog++ < 24)
        HLOG("vk", "pool freed 0x%llx +0x%llx: %zu import(s) removed", (unsigned long long)addr,
             (unsigned long long)size, n);
}

// Place de n octets dans l'anneau des constantes (alignée sur 256, assez pour les tampons uniformes)
uint64_t ring_take(uint64_t n) {
    uint64_t off = (g.ringoff + 255) & ~255ull;
    if (off + n > g.ringsize) {   // débordement : les constantes du début de la soumission seraient écrasées
        static int nlog = 0;
        if (nlog++ < 4) HLOG("vk", "constants ring full (%llu MB): wrapping around to the start", (unsigned long long)(g.ringsize >> 20));
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
    if (nmis++ < 5) HLOG("vk", "misaligned uniform buffer (0x%llx): copied into the ring", (unsigned long long)addr);
    uint64_t off = ring_take(size);
    memcpy(g.ringptr + off, (const void*)addr, size);
    *bi = {g.ring, off, size};
    return true;
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

// ================================================================ mémoire globale des calculs (LDG/STG/ATOMG)
// Un calcul lit et écrit la mémoire du jeu par adresse : celles de ses tampons, données dans la banque 0, et des
// pointeurs rangés dans les données du jeu (lumière du brouillard). Sa table (GTAB, glsl.py) liste des fenêtres
// importées ; une adresse hors de la table se lit à zéro. Jusqu'à la 1.0.2, tous les pools étaient importés en entier
// pour que tout pointeur tombe juste (9,3 Go). Maintenant :
//   * les plages des tampons du calcul sont importées avant lui (glob_fenetre) ;
//   * ses tampons uniformes et le début de ses tampons de stockage sont balayés : tout mot de 64 bits qui est une
//     adresse d'un pool NVN fait importer sa fenêtre (glob_balayer ; cas connu : pointeurs dans un tampon uniforme) ;
//   * une adresse que le shader ne trouve pas dans sa table est notée dans un tampon de retour (gxl_miss, glsl.py) :
//     sa fenêtre est importée pour les calculs suivants (glob_retour).
// Ces fenêtres sont gardées tant que leur pool vit et passent en tête de la table (78 entrées).

// Plages [début, fin) des pools NVN qui ont une mémoire, triées.
std::vector<std::pair<uint64_t, uint64_t>> nvn_pool_plages() {
    std::vector<std::pair<uint64_t, uint64_t>> v;
    {
        std::lock_guard<std::mutex> lk(nvn::g_pool.m);
        for (auto& [k, p] : nvn::g_pool.map)
            if (p.storage && p.size) v.push_back({p.storage, p.storage + p.size});
    }
    std::sort(v.begin(), v.end());
    return v;
}

// Fenêtre importée qui couvre la plage, marquée « calcul » (nullptr : hors des pools, ou import refusé).
PoolBuf* glob_fenetre(uint64_t addr, uint64_t size) {
    if (!addr) return nullptr;
    PoolBuf* p = find_import(addr, size ? size : 4);
    if (p) p->calc = true;
    return p;
}

// Balaye les `max` premiers octets d'un tampon du calcul : chaque mot de 64 bits qui tombe dans un pool NVN est pris
// pour un pointeur, et sa fenêtre est importée.
void glob_balayer(const nvn::Range& r, uint64_t max, const std::vector<std::pair<uint64_t, uint64_t>>& plages) {
    if (!r.addr || r.size < 8 || plages.empty()) return;
    // un même tampon n'est balayé qu'une fois par seconde : le balayage sert surtout au premier calcul qui le lit (une
    // adresse apparue depuis est rattrapée par le tampon de retour). Balayé à chaque calcul, il coûtait des images.
    static std::unordered_map<uint64_t, uint64_t> vus;   // tampon → image du dernier balayage
    if (vus.size() > 8192) vus.clear();
    uint64_t& vu = vus[r.addr ^ (r.size << 40)];
    if (vu && g.cur_frame - vu < 60) return;
    vu = g.cur_frame + 1;
    const uint64_t debut = (r.addr + 7) & ~7ull;
    uint64_t n = std::min(r.size - (debut - r.addr), max) & ~7ull;
    if (!n || !mem_valid(debut, n)) return;
    const uint64_t lo = plages.front().first, hi = plages.back().second;
    for (uint64_t o = 0; o < n; o += 8) {
        uint64_t v;
        memcpy(&v, (const void*)(debut + o), 8);
        if (v < lo || v >= hi) continue;
        auto it = std::upper_bound(plages.begin(), plages.end(), std::make_pair(v, UINT64_MAX));
        if (it == plages.begin()) continue;
        --it;
        if (v < it->second) glob_fenetre(v & ~3ull, 8);
    }
}

// Adresses que les calculs n'ont pas trouvées dans leur table (tampon de retour écrit par gxl_miss) : leur fenêtre
// est importée, ou marquée « calcul » si elle existait sans tenir dans la table.
void glob_retour() {
    if (!g.fb_ptr) return;
    const uint32_t n = std::min<uint32_t>(g.fb_ptr[0], 255);
    if (!n) return;
    uint64_t a[255];
    memcpy(a, g.fb_ptr + 2, 8ull * n);
    g.fb_ptr[0] = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] < 0x10000) continue;
        const bool connu = import_existant(a[i] & ~3ull, 4) != nullptr;
        PoolBuf* p = glob_fenetre(a[i] & ~3ull, 4);
        if (g.glob_retours++ < 24)
            HLOG("vk", "compute: address 0x%llx missing from its table → %s", (unsigned long long)a[i],
                 !p ? "outside the game pools (reads as zero)" : connu ? "window already imported, moved to the head of the table"
                                                                      : "window imported for the following computes");
    }
}

// Tampon de retour : { u32 nombre, u32 -, u64 adresses[255] }, lu par le processeur, écrit par les shaders de calcul
// à travers son adresse GPU (donnée dans la table GTAB).
void glob_creer_retour() {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = 4096;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    if (vkCreateBuffer(g.dev, &bi, nullptr, &g.fb_buf) != VK_SUCCESS) return;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, g.fb_buf, &mr);
    VkMemoryAllocateFlagsInfo maf{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    maf.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = &maf;
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    void* ptr = nullptr;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.dev, &ai, nullptr, &g.fb_mem) != VK_SUCCESS ||
        vkBindBufferMemory(g.dev, g.fb_buf, g.fb_mem, 0) != VK_SUCCESS ||
        vkMapMemory(g.dev, g.fb_mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS) {
        HLOG("vk", "compute readback buffer unavailable: addresses missing from the tables will not be recovered");
        return;
    }
    memset(ptr, 0, 4096);
    g.fb_ptr = (uint32_t*)ptr;
    VkBufferDeviceAddressInfo dai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    dai.buffer = g.fb_buf;
    g.fb_dev = vkGetBufferDeviceAddress(g.dev, &dai);
}

// memory_saver = false (recomp.ini) : comme jusqu'à la 1.0.2, tous les pools NVN sont importés en entier au premier
// calcul à mémoire globale, puis à chaque nouveau pool.
void import_all_pools() {
    const auto v = nvn_pool_plages();
    static size_t last = 0;
    if (v.size() == last) return;
    last = v.size();
    for (auto& [a, e] : v) find_import(a, 4);
}

void imports_init() {
    glob_creer_retour();
    if (!settings().memory_saver) {
        HLOG("vk", "game memory imports: memory saver off (recomp.ini), whole pools as in 1.0.2, no budget");
        return;
    }
    HLOG("vk", "game memory imports: windows of %llu MB on demand (whole pools up to %llu MB), "
               "budget %.1f GB", (unsigned long long)(IMPORT_CELL >> 20), (unsigned long long)(IMPORT_WHOLE_MAX >> 20),
         import_budget() / 1073741824.0);
}

// Mémoire du processus et de la mémoire du jeu, pour le journal périodique.
// Diagnostic d'un pic du tas par défaut (RECOMP_MEM_MAP=1) : blocs occupés par classe de taille, et le début du contenu
// de quelques blocs des classes les plus lourdes (texte, code SPIR-V, flottants… : de quoi reconnaître leur
// propriétaire, notre code ou le pilote). Le tas est verrouillé pendant le parcours : le jeu se fige une à deux
// secondes, d'où le déclenchement seulement quand le tas a grossi de 512 Mo depuis la carte précédente.
void tas_histogramme() {
    const HANDLE h = GetProcessHeap();
    if (!HeapLock(h)) return;
    Sleep(150);                      // les fils qui allouent viennent buter sur le verrou du tas
    dump_thread_stacks("heap spike");   // leurs piles disent qui demande la mémoire
    uint64_t n[40] = {}, o[40] = {};
    const uint8_t* ex[40][3] = {};
    size_t ext[40][3] = {};
    PROCESS_HEAP_ENTRY e{};
    while (HeapWalk(h, &e)) {
        if (!(e.wFlags & PROCESS_HEAP_ENTRY_BUSY)) continue;
        const size_t t = e.cbData;
        const int c = std::min(39, t ? 63 - __builtin_clzll(t) : 0);
        ex[c][n[c] % 3] = (const uint8_t*)e.lpData;   // les trois derniers vus de chaque classe
        ext[c][n[c] % 3] = t;
        n[c]++;
        o[c] += t;
    }
    std::vector<std::pair<uint64_t, int>> cl;
    for (int c = 0; c < 40; c++)
        if (o[c]) cl.push_back({o[c], c});
    std::sort(cl.rbegin(), cl.rend());
    for (size_t i = 0; i < cl.size() && i < 6; i++) {
        const int c = cl[i].second;
        HLOG("mem", "  default heap, blocks of 2^%d bytes and more: %llu blocks, %.0f MB", c, (unsigned long long)n[c],
             o[c] / 1048576.0);
        for (int k = 0; k < 3 && i < 3; k++) {
            if (!ex[c][k]) continue;
            char hex[3 * 40 + 1] = "", asc[41] = "";
            const size_t m = std::min<size_t>(40, ext[c][k]);
            for (size_t j = 0; j < m; j++) {
                snprintf(hex + 3 * j, 4, "%02x ", ex[c][k][j]);
                asc[j] = ex[c][k][j] >= 32 && ex[c][k][j] < 127 ? (char)ex[c][k][j] : '.';
            }
            asc[m] = 0;
            HLOG("mem", "    %p +%zu: %s|%s|", (const void*)ex[c][k], ext[c][k], hex, asc);
        }
    }
    HeapUnlock(h);
}

// Mémoire engagée du tas par défaut du processus (HeapSummary), en octets ; 0 si indisponible.
uint64_t tas_defaut_engage() {
    struct Resume { DWORD cb; SIZE_T alloue, engage, reserve, max; };
    static const auto resume = (BOOL(WINAPI*)(HANDLE, DWORD, Resume*))(void*)GetProcAddress(
        GetModuleHandleA("kernel32.dll"), "HeapSummary");
    Resume r{};
    r.cb = sizeof r;
    return resume && resume(GetProcessHeap(), 0, &r) ? r.engage : 0;
}

// RECOMP_MEM_MAP=1 : carte de la mémoire engagée du processus, écrite toutes les 600 images. Sert à savoir ce qui
// compte dans la mémoire validable de Windows en dehors de la mémoire du jeu : tas du runtime, pilote graphique, copies
// des ressources. Le parcours de l'espace d'adressage prend quelques dizaines de millisecondes : diagnostic seulement.
void mem_map_log(uint64_t frame) {
    struct Bloc { uint64_t base = 0, octets = 0; DWORD type = 0; };
    std::map<uint64_t, Bloc> blocs;   // par réservation (AllocationBase)
    uint64_t prive = 0, image = 0, mappe = 0, regions = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uint64_t a = 0x10000; a < 0x7ffffffe0000ull && VirtualQuery((void*)a, &mbi, sizeof mbi);
         a = (uint64_t)mbi.BaseAddress + mbi.RegionSize) {
        if (mbi.State != MEM_COMMIT) continue;
        regions++;
        (mbi.Type == MEM_PRIVATE ? prive : mbi.Type == MEM_IMAGE ? image : mappe) += mbi.RegionSize;
        if (mbi.Type == MEM_IMAGE) continue;
        Bloc& bl = blocs[(uint64_t)mbi.AllocationBase];
        bl.base = (uint64_t)mbi.AllocationBase;
        bl.octets += mbi.RegionSize;
        bl.type = mbi.Type;
    }
    std::vector<Bloc> v;
    uint64_t petits = 0, moyens = 0, gros = 0;   // réservations de moins de 1 Mo, de 1 à 64 Mo, de plus de 64 Mo
    for (auto& [k, bl] : blocs) {
        v.push_back(bl);
        if (bl.type != MEM_PRIVATE) continue;
        (bl.octets < (1u << 20) ? petits : bl.octets < (64u << 20) ? moyens : gros) += bl.octets;
    }
    std::sort(v.begin(), v.end(), [](const Bloc& x, const Bloc& y) { return x.octets > y.octets; });
    HLOG("mem", "frame %llu, committed memory map: private %.2f GB (reservations under 1 MB: %.2f GB, 1 to 64 MB: %.2f GB, "
                "over 64 MB: %.2f GB), mapped %.2f GB, images %.2f GB; %llu regions, %zu reservations",
         (unsigned long long)frame, prive / 1073741824.0, petits / 1073741824.0, moyens / 1073741824.0,
         gros / 1073741824.0, mappe / 1073741824.0, image / 1073741824.0, (unsigned long long)regions, blocs.size());
    for (size_t i = 0; i < v.size() && i < 8; i++)
        HLOG("mem", "  0x%012llx: %8.1f MB %s", (unsigned long long)v[i].base, v[i].octets / 1048576.0,
             v[i].type == MEM_PRIVATE ? "private" : "mapped");
    // réservations privées de 1 à 64 Mo, par taille arrondie au Mo : les 8 tailles qui pèsent le plus
    std::map<uint64_t, std::pair<uint64_t, uint64_t>> tailles;   // Mo → (nombre, octets)
    for (auto& bl : v)
        if (bl.type == MEM_PRIVATE && bl.octets >= (1u << 20) && bl.octets < (64u << 20)) {
            auto& t = tailles[(bl.octets + (1u << 19)) >> 20];
            t.first++, t.second += bl.octets;
        }
    std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> tv(tailles.begin(), tailles.end());
    std::sort(tv.begin(), tv.end(), [](auto& x, auto& y) { return x.second.second > y.second.second; });
    std::string txt;
    for (size_t i = 0; i < tv.size() && i < 8; i++) {
        char b[64];
        snprintf(b, sizeof b, "%s%llu MB x %llu", i ? ", " : "", (unsigned long long)tv[i].first, (unsigned long long)tv[i].second.first);
        txt += b;
    }
    HLOG("mem", "  private reservations of 1 to 64 MB by size: %s", txt.c_str());
    {   // tas du processus : le tas par défaut sert à malloc et à new (runtime) ; les autres sont créés par des DLL
        // (pilote graphique, Windows). HeapSummary lit les segments sans parcourir les blocs.
        struct Resume { DWORD cb; SIZE_T alloue, engage, reserve, max; };
        static const auto resume = (BOOL(WINAPI*)(HANDLE, DWORD, Resume*))(void*)GetProcAddress(
            GetModuleHandleA("kernel32.dll"), "HeapSummary");
        HANDLE tas[256];
        const DWORD nt = GetProcessHeaps(256, tas);
        std::vector<std::pair<SIZE_T, std::string>> lignes;
        SIZE_T total = 0, defaut = 0;
        for (DWORD i = 0; resume && i < nt && i < 256; i++) {
            Resume r{};
            r.cb = sizeof r;
            if (!resume(tas[i], 0, &r)) continue;
            total += r.engage;
            if (tas[i] == GetProcessHeap()) defaut = r.engage;
            char t2[112];
            snprintf(t2, sizeof t2, "%s%.0f MB committed (%.0f allocated)", tas[i] == GetProcessHeap() ? "default heap " : "",
                     r.engage / 1048576.0, r.alloue / 1048576.0);
            lignes.push_back({r.engage, t2});
        }
        std::sort(lignes.begin(), lignes.end(), [](auto& x, auto& y) { return x.first > y.first; });
        std::string ht;
        for (size_t i = 0; i < lignes.size() && i < 5; i++) ht += (i ? "; " : "") + lignes[i].second;
        HLOG("mem", "  heaps: %lu, %.0f MB committed in all; largest: %s", nt, total / 1048576.0,
             resume ? ht.c_str() : "HeapSummary unavailable");
        static SIZE_T defaut_vu = 0;
        if (defaut_vu && defaut > defaut_vu + (512ull << 20)) tas_histogramme();
        defaut_vu = defaut;
    }
    {   // part du runtime dans ce tas (operator new, host_main.cpp) et tailles allouées depuis la carte précédente
        static uint64_t n0[40], o0[40];
        uint64_t n1[40], o1[40];
        int64_t vivants = 0;
        cpp_heap_stats(&vivants, n1, o1);
        std::vector<std::pair<uint64_t, int>> cl;
        for (int c = 0; c < 40; c++)
            if (o1[c] > o0[c]) cl.push_back({o1[c] - o0[c], c});
        std::sort(cl.rbegin(), cl.rend());
        std::string ct;
        for (size_t i = 0; i < cl.size() && i < 6; i++) {
            char t3[96];
            const int c = cl[i].second;
            snprintf(t3, sizeof t3, "%s%llu x %s%llu %s = %.0f MB", i ? ", " : "", (unsigned long long)(n1[c] - n0[c]),
                     c >= 20 ? ">=" : ">=", (unsigned long long)(c >= 20 ? 1ull << (c - 20) : c >= 10 ? 1ull << (c - 10) : 1ull << c),
                     c >= 20 ? "MB" : c >= 10 ? "KB" : "B", cl[i].first / 1048576.0);
            ct += t3;
        }
        HLOG("mem", "  runtime C++ heap: %.0f MB live; allocated since the previous map: %s", vivants / 1048576.0, ct.c_str());
        memcpy(n0, n1, sizeof n0);
        memcpy(o0, o1, sizeof o0);
    }
    if (vrai_vkAllocateMemory) {
        std::lock_guard<std::mutex> lk(g_vk_suivi.m);
        std::map<uint64_t, std::pair<uint64_t, uint64_t>> gpu;   // tailles des allocations dédiées au GPU
        for (auto& [h, e] : g_vk_suivi.vivants)
            if (e.second == 0) {
                auto& t = gpu[(e.first + (1u << 19)) >> 20];
                t.first++, t.second += e.first;
            }
        std::vector<std::pair<uint64_t, std::pair<uint64_t, uint64_t>>> gv(gpu.begin(), gpu.end());
        std::sort(gv.begin(), gv.end(), [](auto& x, auto& y) { return x.second.second > y.second.second; });
        std::string gt;
        for (size_t i = 0; i < gv.size() && i < 8; i++) {
            char b[64];
            snprintf(b, sizeof b, "%s%llu MB x %llu", i ? ", " : "", (unsigned long long)gv[i].first, (unsigned long long)gv[i].second.first);
            gt += b;
        }
        HLOG("mem", "  Vulkan allocations: GPU %.0f MB in %llu, host-visible %.0f MB in %llu, imported %.0f MB in %llu; GPU by "
                    "size: %s", g_vk_suivi.octets[0] / 1048576.0, (unsigned long long)g_vk_suivi.nombre[0],
             g_vk_suivi.octets[1] / 1048576.0, (unsigned long long)g_vk_suivi.nombre[1], g_vk_suivi.octets[2] / 1048576.0,
             (unsigned long long)g_vk_suivi.nombre[2], gt.c_str());
        HLOG("mem", "  images: %zu alive (%.0f MB orphaned), %zu awaiting destruction, %zu staging buffers pending; "
                    "texture-set writes: %llu in %llu calls",
             g.images.size(), g.unref_bytes / 1048576.0, g.dead_images.size(), g.staging.size(),
             (unsigned long long)g.tex_writes, (unsigned long long)g.tex_write_calls);
        // Textures envoyées au GPU dont les données restent aussi dans la mémoire du jeu (sur la console, c'est la même
        // mémoire) : ce que rendre ces pages à Windows ferait gagner au plus. « pages entières » : les pages de 4 Ko
        // entièrement couvertes par une texture (une page partagée avec d'autres données ne peut pas être rendue).
        uint64_t src = 0, pages = 0, nsrc = 0;
        std::vector<std::pair<uint64_t, uint64_t>> plages;
        for (auto& [k, im] : g.images)
            if (im.initialized && im.packaged && im.src_bytes && im.addr) {
                src += im.src_bytes;
                nsrc++;
                plages.push_back({im.addr, im.addr + im.src_bytes});
            }
        std::sort(plages.begin(), plages.end());
        uint64_t fin_vue = 0;   // (les images qui partagent une adresse ne sont comptées qu'une fois)
        for (auto& [a0, a1] : plages) {
            const uint64_t d = std::max((a0 + 0xfff) & ~0xfffull, fin_vue), f = a1 & ~0xfffull;
            if (f > d) pages += f - d, fin_vue = f;
        }
        RelStats rs{};
        mem_release_stats(&rs);
        HLOG("mem", "  texture data kept in game memory after upload: %.0f MB for %llu textures (%.0f MB in whole pages); "
                    "%.0f MB of game memory released to Windows after upload",
             src / 1048576.0, (unsigned long long)nsrc, pages / 1048576.0, rs.rendus / 1048576.0);
    }
}

std::string ram_text() {
    PROCESS_MEMORY_COUNTERS pmc{};
    pmc.cb = sizeof pmc;
    K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
    uint64_t blocs = 0, octets = 0, reserves = 0;
    mem_lazy_stats(&blocs, &octets, &reserves);
    size_t calc = 0;
    for (auto& p : g.pools) calc += p.calc;
    char b[400];
    snprintf(b, sizeof b,
             "%.2f GB in RAM, %.2f GB committed; game memory: %.2f GB reserved on demand, %.2f GB committed at first "
             "access; imports: %.0f MB in %zu windows (%zu for computes), %llu released, %llu refusals",
             pmc.WorkingSetSize / 1073741824.0, pmc.PagefileUsage / 1073741824.0, reserves / 1073741824.0,
             octets / 1073741824.0, g.imported / 1048576.0, g.pools.size(), calc, (unsigned long long)g.imports_evinces,
             (unsigned long long)g.imports_refuses);
    return b;
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
        HLOG("vk", "markers: %u read out of %zu placed in the batch", n, g.marks.size());
        for (auto& c : cd) {
            size_t k = (size_t)(uintptr_t)c.pCheckpointMarker;
            HLOG("vk", "  last marker at stage 0x%x: #%zu '%s'", (unsigned)c.stage, k,
                 k && k <= g.marks.size() ? g.marks[k - 1].c_str() : "?");
        }
    }
    if (g.device_fault && vkGetDeviceFaultInfoEXT) {
        VkDeviceFaultCountsEXT fc{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
        VkResult r = vkGetDeviceFaultInfoEXT(g.dev, &fc, nullptr);
        HLOG("vk", "fault report: %d (%u addresses, %u vendor)", (int)r, fc.addressInfoCount, fc.vendorInfoCount);
        std::vector<VkDeviceFaultAddressInfoEXT> ai(fc.addressInfoCount);
        std::vector<VkDeviceFaultVendorInfoEXT> vi(fc.vendorInfoCount);
        fc.vendorBinarySize = 0;
        VkDeviceFaultInfoEXT fi{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
        fi.pAddressInfos = ai.empty() ? nullptr : ai.data();
        fi.pVendorInfos = vi.empty() ? nullptr : vi.data();
        r = vkGetDeviceFaultInfoEXT(g.dev, &fc, &fi);
        HLOG("vk", "GPU fault (%d): %s", (int)r, fi.description);
        for (auto& a : ai)
            HLOG("vk", "  address (type %d): 0x%llx, precision 0x%llx", (int)a.addressType, (unsigned long long)a.reportedAddress,
                 (unsigned long long)a.addressPrecision);
        for (auto& v : vi)
            HLOG("vk", "  vendor: %s (code 0x%llx, data 0x%llx)", v.description, (unsigned long long)v.vendorFaultCode,
                 (unsigned long long)v.vendorFaultData);
    }
}
void check_vk(VkResult r, const char* where) {
    if (r == VK_ERROR_DEVICE_LOST && !g.lost) {
        g.lost = true;
        HLOG("vk", "DEVICE LOST (%s, frame %llu); shaders in the batch (* = never completed before):%s", where,
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
            HLOG("vk", "GPU not responding for 1 s (%s, frame %llu, last program '%s'); shaders in the batch (* = never completed before):%s",
                 where, (unsigned long long)g.cur_frame, g.last_label.c_str(), batch_desc().c_str());
            report_lost();   // marqueurs atteints pendant le blocage
        }
    if (hung) {
        HLOG("vk", "stuck wait ended: result %d", (int)r);
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
    barrier_all();   // effacement ordonné avant tout usage (voir image_to_general)
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
        HLOG("vk", "dummy images impossible: texture set not prefilled");
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
    HLOG("vk", "texture set prefilled: %u slots × 12 arrays + %u dummy samplers", TEX_MAX, SAMP_MAX);
}

// ================================================================ commandes
// Nouvel enregistrement : emplacement suivant, réutilisé quand le GPU a fini sa soumission précédente.
// Tampons de transit des textures : un par texture envoyée, de la taille de la texture, en mémoire du PC. Ils
// n'étaient rendus qu'à la réutilisation de leur emplacement (6 soumissions plus tard), et rien ne bornait ceux d'un
// même enregistrement : un changement de scène envoie 1 à 2 Go de textures en quelques images, et la mémoire engagée
// du jeu faisait un saut d'autant (mesuré le 2026-11-11 : 11,8 → 13,4 Go en 20 s, 1,6 Go de blocs de 1 à 64 Mo ; sur
// un PC à court de mémoire, c'est là que le jeu s'arrêtait). Ils sont maintenant rendus dès que le GPU a fini la
// soumission qui les lit, et borner_transit soumet et attend dès que l'enregistrement en cours en porte plus de
// 256 Mo.
void free_done_staging() {
    const uint64_t done = gpu_done_serial();
    for (auto& s : g.slots) {
        if (s.staging.empty() || !s.serial || s.serial > done) continue;
        for (auto& x : s.staging) {
            vkDestroyBuffer(g.dev, x.b, nullptr);
            vkFreeMemory(g.dev, x.m, nullptr);
        }
        s.staging.clear();
    }
}

// Écriture d'un emplacement du jeu de textures (jeu 2 : 16 384 images par type, 4 096 échantillonneurs, mis à jour
// après liaison) : un appel à vkUpdateDescriptorSets par emplacement, dès qu'une vue change, comme depuis la 1.0.0.
// RECOMP_TEX_WRITE_BATCH=1 (essai) : les écritures sont mises de côté et faites en un seul appel juste avant la
// soumission. Essayé le 2026-11-11 en cherchant la cause des pics de mémoire : 7 109 écritures en 148 appels sur
// 2 minutes, sans effet sur le pic (qui vient du compilateur du pilote), donc pas activé.
void flush_tex_writes() {
    if (g.tex_pending.empty()) return;
    std::vector<VkWriteDescriptorSet> ws(g.tex_pending.size());
    for (size_t i = 0; i < ws.size(); i++) {
        const Vk::TexWrite& t = g.tex_pending[i];
        ws[i] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        ws[i].dstSet = g.texset;
        ws[i].dstBinding = t.binding;
        ws[i].dstArrayElement = t.element;
        ws[i].descriptorCount = 1;
        ws[i].descriptorType = t.type;
        ws[i].pImageInfo = &t.ii;
    }
    vkUpdateDescriptorSets(g.dev, (uint32_t)ws.size(), ws.data(), 0, nullptr);
    g.tex_write_calls++;
    g.tex_pending.clear();
}
void tex_write(uint32_t binding, uint32_t element, VkDescriptorType type, const VkDescriptorImageInfo& ii) {
    static const bool groupe = getenv("RECOMP_TEX_WRITE_BATCH") != nullptr;
    g.tex_writes++;
    g.tex_pending.push_back({binding, element, type, ii});
    if (!groupe) flush_tex_writes();
}

void begin_frame() {
    if (g.recording) return;
    free_done_staging();
    g.slot = (g.slot + 1) % Vk::NSLOT;
    Vk::Slot& s = g.slots[g.slot];
    if (s.serial) wait_serial(s.serial, "reuse of a slot");
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
                mem_desepingler(g.dead_pools[i].second.base, g.dead_pools[i].second.size);
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
                HLOG("vk", "slot %d: descriptor pool #%zu (heavy submission)", g.slot, s.dpools.size());
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
    uint64_t dump3d = 0;                   // RECOMP_TEX3D_DUMP : octets bruts d'une texture 3D, écrits tels quels
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
            HLOG("gpuprof", "frame %llu: %zu draws / computes measured, %.2f ms GPU in total", (unsigned long long)frame,
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
    g.staging_bytes = 0;
    for (auto& r : g_readbacks) {
        uint8_t* p = nullptr;
        if (vkMapMemory(g.dev, r.m, 0, VK_WHOLE_SIZE, 0, (void**)&p) == VK_SUCCESS) {
            if (r.dump3d) {   // RECOMP_TEX3D_DUMP : fichier brut
                if (FILE* f = fopen(r.path.c_str(), "wb")) {
                    fwrite(p, 1, r.dump3d, f);
                    fclose(f);
                    HLOG("vk", "3D texture dumped: %s (%llu bytes)", r.path.c_str(), (unsigned long long)r.dump3d);
                }
            } else if (!r.audit.empty()) {   // RECOMP_POOL_AUDIT : échantillons du pool vus par le GPU / le processeur
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
                    HLOG("vk", "check frame %llu, import 0x%llx: %zu samples, %zu identical, %zu ZERO on the GPU side, %zu other mismatches, %zu unmapped (first 0x%llx)",
                         (unsigned long long)r.audit_frame, (unsigned long long)r.raw_addr, r.audit.size(), same, gpu_zero, other,
                         unmapped, (unsigned long long)first_bad);
                else
                    HLOG("vk", "check frame %llu, import 0x%llx: %zu identical samples", (unsigned long long)r.audit_frame,
                         (unsigned long long)r.raw_addr, r.audit.size());
            } else if (r.raw_size) {   // gpu_peek : vu par le GPU contre vu par le processeur au même instant
                const uint8_t* cpu = (const uint8_t*)r.raw_addr;
                size_t diff = SIZE_MAX, ndiff = 0;
                for (size_t i = 0; i < r.raw_size; i++)
                    if (p[i] != cpu[i]) ndiff++, diff = std::min(diff, i);
                if (FILE* f = fopen((r.path + ".gpu.bin").c_str(), "wb")) fwrite(p, 1, r.raw_size, f), fclose(f);
                if (FILE* f = fopen((r.path + ".cpu.bin").c_str(), "wb")) fwrite(cpu, 1, r.raw_size, f), fclose(f);
                if (ndiff)
                    HLOG("vk", "GPU readback %s (0x%llx +0x%llx): %zu bytes DIFFER from the CPU (first at +0x%zx)",
                         r.path.c_str(), (unsigned long long)r.raw_addr, (unsigned long long)r.raw_size, ndiff, diff);
                else
                    HLOG("vk", "GPU readback %s (0x%llx +0x%llx): identical to the CPU", r.path.c_str(),
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
                HLOG("vk", "depth capture %s: min %.6f max %.6f (excluding 0 and 1)", r.path.c_str(), lo, hi);
            }
            vkUnmapMemory(g.dev, r.m);
        }
        vkDestroyBuffer(g.dev, r.b, nullptr);
        vkFreeMemory(g.dev, r.m, nullptr);
        if (r.tmp) vkDestroyImage(g.dev, r.tmp, nullptr);
        if (r.tmpm) vkFreeMemory(g.dev, r.tmpm, nullptr);
    }
    if (!g_readbacks.empty()) HLOG("vk", "captures written: %zu", g_readbacks.size());
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

// ---- données des textures rendues à Windows après l'envoi (recomp.ini : texture_release ; voir memory.cpp) ----
// empreinte des octets de la source hors des pages entières [d, f)
uint64_t edges_hash(uint64_t addr, uint64_t n, uint64_t d, uint64_t f) {
    return hash_mem((const uint8_t*)addr, d - addr) ^ (hash_mem((const uint8_t*)f, addr + n - f) * 0x9E3779B97F4A7C15ull);
}

// Rend les pages entières de la source de l'image, qui vient d'être lue en entier (envoi, ou empreinte inchangée).
void tex_release(Image& im) {
    im.rel_d = im.rel_f = im.rel_id = 0;
    if (!mem_release_actif() || !im.src_bytes || !im.addr || im.rel_n >= 3) return;
    const uint64_t d = (im.addr + 0xfff) & ~0xfffull, f = (im.addr + im.src_bytes) & ~0xfffull;
    if (f < d + (64ull << 10)) return;   // petite texture : rien à gagner
    // pages verrouillées par un import (le GPU les lit directement) : on n'y touche pas
    for (auto& p : g.pools)
        if (d < p.base + p.size && p.base < f) {
            g.tex_rel_import += f - d;
            return;
        }
    for (auto& dp : g.dead_pools)
        if (d < dp.second.base + dp.second.size && dp.second.base < f) {
            g.tex_rel_import += f - d;
            return;
        }
    uint64_t id = 0;
    if (mem_release(d, f - d, &id) != f - d) return;
    im.rel_d = d, im.rel_f = f, im.rel_id = id, im.rel_n++;
    im.edge_hash = edges_hash(im.addr, im.src_bytes, d, f);
    g.tex_rel_rendus++;
    if (!im.refs) mem_release_vivant(d, f - d, id, false);
}

// ---- plages vidées par le jeu (nvnMemoryPoolFlushMappedRange) ----
// Sur la console, le GPU lit les données d'une texture dans la mémoire du jeu : quand le jeu les réécrit sous un objet
// texture qu'il garde (données déplacées par son gestionnaire de textures, niveaux de détail chargés après coup), le
// nouveau contenu s'affiche. Ici le contenu est copié en mémoire vidéo au premier usage, et rien ne signalait ces
// réécritures : l'ancienne copie restait (dessins de la coque de l'Arwing sur le front de Slippy dans la cinématique
// de fin de Corneria, 2026-11-11). Le jeu annonce pourtant chaque écriture par nvnMemoryPoolFlushMappedRange, que le
// runtime ignorait. Registre : plages disjointes, chacune avec le numéro du dernier vidage qui la couvre ; une image
// retient le numéro courant quand son contenu est lu. Un vidage plus récent sur sa source : son contenu est comparé
// à l'envoi (source_changed), et renvoyé s'il a changé. RECOMP_NO_FLUSH_CHECK=1 : ancien comportement.
std::mutex g_flush_mx;
std::map<uint64_t, std::pair<uint64_t, uint64_t>> g_flush;   // début → (fin, numéro)
std::atomic<uint64_t> g_flush_serial{0}, g_flush_bytes{0};
void flush_note(uint64_t a, uint64_t b) {
    std::lock_guard<std::mutex> lk(g_flush_mx);
    const uint64_t s = ++g_flush_serial;
    g_flush_bytes += b - a;
    auto it = g_flush.upper_bound(a);
    if (it != g_flush.begin() && std::prev(it)->second.first > a) --it;
    while (it != g_flush.end() && it->first < b) {
        const uint64_t s0 = it->first, e0 = it->second.first, n0 = it->second.second;
        it = g_flush.erase(it);
        if (s0 < a) g_flush[s0] = {a, n0};
        if (e0 > b) it = g_flush.emplace(b, std::make_pair(e0, n0)).first, ++it;
    }
    g_flush[a] = {b, s};
}
// numéro du dernier vidage qui touche [a, b) ; 0 : aucun
uint64_t flush_last(uint64_t a, uint64_t b) {
    std::lock_guard<std::mutex> lk(g_flush_mx);
    uint64_t n = 0;
    auto it = g_flush.upper_bound(a);
    if (it != g_flush.begin() && std::prev(it)->second.first > a) --it;
    for (; it != g_flush.end() && it->first < b; ++it) n = std::max(n, it->second.second);
    return n;
}

// La mémoire source d'une image a-t-elle changé depuis son envoi ? Pages rendues jamais retouchées : inchangées par
// construction ; seuls les octets hors de ces pages sont comparés, sans rien relire du fichier.
bool source_changed(Image& o, uint64_t addr) {
    if (o.rel_id && addr == o.addr && mem_released_intact(o.rel_d, o.rel_f - o.rel_d, o.rel_id)) {
        g.tex_rel_intactes++;
        return edges_hash(addr, o.src_bytes, o.rel_d, o.rel_f) != o.edge_hash;
    }
    mem_restore(addr, o.src_bytes);   // pages rendues : restaurées d'un bloc, plutôt qu'une faute par morceau
    const bool change = hash_mem((const uint8_t*)addr, o.src_bytes) != o.content_hash;
    if (!change && addr == o.addr) tex_release(o);     // relue sans avoir changé : rendue de nouveau
    return change;
}

// Contrôle fait une fois par image affichée, à la première liaison d'une texture : son contenu en mémoire vidéo
// est-il encore celui de la mémoire du jeu ? Deux cas demandent de comparer (source_changed), et de renvoyer l'image
// si elle a changé :
//  1. le jeu a vidé une plage de sa source depuis la dernière lecture (pool_flushed) ;
//  2. l'image n'a plus d'objet texture vivant (refs à 0) et un descripteur du jeu la désigne encore. Le jeu détruit et
//     recrée ses objets texture sur des données en place, et garde le même descripteur quand l'adresse et le format
//     n'ont pas changé : sur la console un descripteur ne contient que cette adresse, il montre ce que la mémoire
//     contient maintenant. Ici il menait à l'image de l'objet détruit, jamais recomparée : après le chargement d'une
//     autre scène dans la même mémoire, l'ancienne texture restait (34 masques de 1696×1696 réécrits à l'image 7 230
//     d'une partie ; dessins de la coque de l'Arwing sur le front de Slippy dans la cinématique de fin de Corneria).
//     Source rendue à Windows (tex_release) : comparée seulement si ses pages ont été retouchées, ce qui ne coûte
//     qu'une recherche. Sinon (petite texture, memory_saver coupé) : empreinte refaite toutes les 30 images, à une
//     image qui dépend de l'adresse pour étaler le coût.
// RECOMP_NO_FLUSH_CHECK=1 : ancien comportement (aucun contrôle). RECOMP_FLUSH_LOG=1 : chaque renvoi est noté.
void flush_recheck(Image& im) {
    if (im.chk_frame == g.cur_frame) return;
    im.chk_frame = g.cur_frame;
    if (!im.packaged || !im.initialized || !im.src_bytes || !im.addr) return;
    static const bool off = getenv("RECOMP_NO_FLUSH_CHECK") != nullptr;
    if (off) return;
    int cause = flush_last(im.addr, im.addr + im.src_bytes) > im.chk_serial ? 1 : 0;
    if (!cause && im.refs == 0) {
        if (im.rel_id) cause = mem_released_intact(im.rel_d, im.rel_f - im.rel_d, im.rel_id) ? 0 : 2;
        else if ((g.cur_frame + (im.addr >> 16)) % 30 == 0) cause = 3;
    }
    if (!cause) return;
    const uint64_t now = g_flush_serial.load();   // avant la lecture : un vidage pendant la lecture redemande un contrôle
    g.flush_checks[cause]++;
    if (mem_valid(im.addr, im.src_bytes) && source_changed(im, im.addr)) {
        im.initialized = false;   // renvoyé à cet usage (image_to_general)
        g.tex_reuploads++;
        g.flush_reuploads[cause]++;
        static const bool flog = getenv("RECOMP_FLUSH_LOG") != nullptr;
        if (flog)
            HLOG("vk", "frame %llu: texture %llx %ux%u fmt %u (%llu KB, %u live objects) rewritten by the game after its "
                       "upload at frame %llu: sent again (%s)", (unsigned long long)g.cur_frame, (unsigned long long)im.addr,
                 im.w, im.h, im.nvnfmt, (unsigned long long)(im.src_bytes >> 10), im.refs, (unsigned long long)im.up_frame,
                 cause == 1 ? "range flushed by the game" : cause == 2 ? "no live object, released data touched"
                                                                       : "no live object, periodic check");
    }
    im.chk_serial = now;
}

// RECOMP_TEX_AUDIT=n (diagnostic) : toutes les n images, la mémoire source de chaque texture packagée demandée par un
// dessin depuis le contrôle précédent est comparée à ce qui a été envoyé au GPU. Différente : le jeu a réécrit ces
// données après l'envoi, et l'image du GPU est périmée (sur la console, le GPU lit la mémoire du jeu directement).
// RECOMP_TEX_AUDIT_FROM=f : à partir de l'image f. RECOMP_TEX_AUDIT_FIX=1 : l'image périmée est renvoyée au GPU.
void tex_audit(uint64_t frame, uint64_t every) {
    static const bool fix = getenv("RECOMP_TEX_AUDIT_FIX") != nullptr;
    static std::map<uint64_t, uint64_t> dits;   // image → empreinte déjà signalée
    static uint64_t tours = 0, total = 0;
    uint64_t vues = 0, intactes = 0, octets = 0, perimees = 0;
    for (auto& [key, im] : g.images) {
        if (!im.packaged || !im.initialized || !im.src_bytes || !im.addr || im.last_use + every <= frame) continue;
        if (!mem_valid(im.addr, im.src_bytes)) continue;
        vues++;
        if (im.rel_id && mem_released_intact(im.rel_d, im.rel_f - im.rel_d, im.rel_id)) intactes++;
        else octets += im.src_bytes;
        if (!source_changed(im, im.addr)) continue;
        perimees++;
        const uint64_t h = hash_mem((const uint8_t*)im.addr, im.src_bytes);
        if (dits[key] != h) {
            dits[key] = h;
            total++;
            HLOG("vk", "tex audit frame %llu: STALE image %llx addr %llx %ux%u fmt %u levels %u layers %u target %u, %llu KB, "
                       "uploaded at frame %llu, refs %u, flush %llu after read %llu%s", (unsigned long long)frame,
                 (unsigned long long)key, (unsigned long long)im.addr, im.w, im.h, im.nvnfmt, im.levels, im.layers, im.target,
                 (unsigned long long)(im.src_bytes >> 10), (unsigned long long)im.up_frame, im.refs,
                 (unsigned long long)flush_last(im.addr, im.addr + im.src_bytes), (unsigned long long)im.chk_serial,
                 fix ? ", re-uploaded" : "");
        }
        if (fix) {
            im.initialized = false;   // renvoyé au prochain usage (image_to_general)
            g.tex_reuploads++;
        }
    }
    if (perimees || tours++ % 20 == 0)
        HLOG("vk", "tex audit frame %llu: %llu textures in use, %llu released and untouched, %llu KB hashed, %llu stale (%llu "
                   "reported so far); flushes: %llu (%llu MB); checks/re-uploads: flushed %llu/%llu, no object touched "
                   "%llu/%llu, no object periodic %llu/%llu",
             (unsigned long long)frame, (unsigned long long)vues, (unsigned long long)intactes,
             (unsigned long long)(octets >> 10), (unsigned long long)perimees, (unsigned long long)total,
             (unsigned long long)g_flush_serial.load(), (unsigned long long)(g_flush_bytes.load() >> 20),
             (unsigned long long)g.flush_checks[1], (unsigned long long)g.flush_reuploads[1],
             (unsigned long long)g.flush_checks[2], (unsigned long long)g.flush_reuploads[2],
             (unsigned long long)g.flush_checks[3], (unsigned long long)g.flush_reuploads[3]);
}

void tex_release_log(uint64_t frame) {
    RelStats s{};
    mem_release_stats(&s);
    if (!s.cumul) return;
    HLOG("vk", "frame %llu, texture data released: %.0f MB now (%.0f MB in total, %llu releases, file %.0f MB, %.0f MB kept "
               "under an import); restored: %llu write faults on unused textures (%.0f MB), %llu read "
               "faults (%.1f MB), %llu write faults on textures in use (%.1f MB), %llu by the runtime (%.0f MB), %llu before "
               "a system call (%.0f MB); read back after a partial rewrite: %llu (%.1f MB); %llu reuses without reading; "
               "%.0f ms writing, %.0f ms restoring",
         (unsigned long long)frame, s.rendus / 1048576.0, s.cumul / 1048576.0, (unsigned long long)g.tex_rel_rendus,
         s.fichier / 1048576.0, g.tex_rel_import / 1048576.0, (unsigned long long)s.n[4], s.octets[4] / 1048576.0,
         (unsigned long long)s.n[0], s.octets[0] / 1048576.0, (unsigned long long)s.n[1], s.octets[1] / 1048576.0,
         (unsigned long long)s.n[2], s.octets[2] / 1048576.0, (unsigned long long)s.n[3], s.octets[3] / 1048576.0,
         (unsigned long long)s.n[5], s.octets[5] / 1048576.0, (unsigned long long)g.tex_rel_intactes, s.ns_ecrit / 1e6,
         s.ns_lu / 1e6);
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
        if (nw++ < 8) HLOG("vk", "texture %llx (%ux%u fmt %u): unreadable memory (0x%llx bytes)", (unsigned long long)im.addr,
                           im.w, im.h, im.nvnfmt, (unsigned long long)src_total);
        return false;
    }
    mem_restore(im.addr, src_total);   // source rendue après un envoi précédent : restaurée d'un bloc
    im.chk_serial = g_flush_serial.load();   // avant la lecture : un vidage du jeu pendant l'envoi redemande un contrôle
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
    g.staging_bytes += doff * im.layers;
    g.tex_uploads++;
    g.tex_upload_bytes += src_total;
    im.src_bytes = src_total;
    im.content_hash = hash_mem((const uint8_t*)im.addr, src_total);
    im.up_frame = g.cur_frame;
    tex_release(im);
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
        // Barrière après l'effacement : sans elle, rien n'ordonne cet effacement et le premier usage de l'image dans la
        // même soumission. Le pilote AMD laissait l'effacement finir après le début d'un calcul qui écrivait l'image :
        // les premières cases écrites revenaient au gris neutre (table de couleurs 3D du jeu : 3 000 cases du coin des
        // couleurs sombres, d'où le noir rendu gris sur AMD, issues n° 6 et 7). NVIDIA exécute dans l'ordre.
        barrier_all();
    }
}

Image* get_image(uint64_t texkey);
// Mémoire vidéo utilisée et budget du pilote (VK_EXT_memory_budget), pour le journal périodique
// Mémoire vidéo dédiée utilisée et budget accordé par Windows (VK_EXT_memory_budget). Faux si le pilote ne le dit pas.
bool vram_usage(uint64_t* used, uint64_t* budget) {
    *used = *budget = 0;
    if (!g.mem_budget || !vkGetPhysicalDeviceMemoryProperties2) return false;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    mp.pNext = &bp;
    vkGetPhysicalDeviceMemoryProperties2(g.pd, &mp);
    for (uint32_t h = 0; h < mp.memoryProperties.memoryHeapCount; h++)
        if (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            *used += bp.heapUsage[h], *budget += bp.heapBudget[h];
    return *budget != 0;
}

std::string vram_text() {
    uint64_t used = 0, budget = 0;
    if (!vram_usage(&used, &budget)) return "";
    char b[96];
    snprintf(b, sizeof b, ", video memory %.0f / %.0f MB", used / 1048576.0, budget / 1048576.0);
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
    HLOG("vkdbg", "  %s %d: object %llx -> image %llx%s @%llx %dx%d fmt %u", what, idx, (unsigned long long)texkey,
         (unsigned long long)ck, it == g.images.end() ? " (missing)" : "", ok ? (unsigned long long)t.addr : 0ull,
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
                if (o.initialized && o.src_bytes && mem_valid(t.addr, o.src_bytes) && source_changed(o, t.addr)) {
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
                    HLOG("vk", "address %llx: format %u then %u (%ux%u), different layouts: separate image",
                         (unsigned long long)t.addr, o.nvnfmt, t.b.format, o.w, o.h);
                continue;
            }
            if (o.nvnfmt != t.b.format) {
                static int nw = 0;
                if (nw++ < 8) HLOG("vk", "alias %llx: format %u reinterpreted as %u (%ux%u)", (unsigned long long)t.addr,
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
        if (warned.insert(t.b.format).second) HLOG("vk", "NVN format %u not supported", t.b.format);
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
        HLOG("vk", "image creation %ux%u fmt %u failed", im.w, im.h, t.b.format);
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
            HLOG("vk", "video memory refused (%d) for an image %ux%u fmt %u (%.1f MB)%s", (int)ar, im.w, im.h,
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
        HLOG("vk", "render view: %ux%u fmt %u level %u layer %u (%ux%u)", im.w, im.h, im.nvnfmt, level, layer,
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
        HLOG("samp", "min %u mag %u wrap %u %u %u compare %u/%u reduction %u lod %g..%g bias %g aniso %g", s.min, s.mag,
             s.wrap[0], s.wrap[1], s.wrap[2], s.compare_mode, s.compare_func, s.reduction, s.min_lod, s.max_lod, s.lod_bias,
             s.max_aniso);
    VkSampler smp = VK_NULL_HANDLE;
    VkResult r = vkCreateSampler(g.dev, &ci, nullptr, &smp);
    if (r != VK_SUCCESS) {
        static int nf = 0;
        if (nf++ < 10) HLOG("vk", "sampler creation failed (%d) after %zu samplers", (int)r, g.samplers.size());
    }
    if (g.samplers.size() % 500 == 499) HLOG("vk", "%zu Vulkan samplers created", g.samplers.size() + 1);
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
    if (g.dbg) HLOG("vkdbg", "  handle %llx: texture %u %s", (unsigned long long)handle, tid, have_t ? "" : "(missing from the pool)");
    if (tid >= TEX_MAX) {
        static int nw = 0;
        if (nw++ < 20)
            HLOG("vk", "texture id %u ≥ %u (handle %llx, %s from the pool)", tid, TEX_MAX, (unsigned long long)handle,
                 have_t ? "present" : "missing");
    }
    if (have_t && tid < TEX_MAX) {
        Image* im = get_image(td.tex);
        dbg_tex("tex", (int)tid, td.tex);
        if (im) {
            flush_recheck(*im);   // données réécrites par le jeu depuis l'envoi : renvoyées
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
                                HLOG("watch", "slot %u type %s: image %ux%u×%u fmt %u target %u levels %u (object %llx, pool %llx, image %s, view %llx → %llx); NVN view flags %u swizzle %u %u %u %u, texture swizzle %u %u %u %u",
                                     tid, names[k], im->w, im->h, im->layers, im->nvnfmt, im->target, im->levels,
                                     (unsigned long long)td.tex, (unsigned long long)texpool, im->initialized ? "initialized" : "new",
                                     (unsigned long long)w, (unsigned long long)key, td.view.flags, td.view.swizzle[0],
                                     td.view.swizzle[1], td.view.swizzle[2], td.view.swizzle[3], im->swz[0], im->swz[1], im->swz[2],
                                     im->swz[3]);
                        }
                        w = key;
                        tex_write(k, tid, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL});
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
                HLOG("watch", "sampler %u: min %u mag %u wrap %u %u %u compare %u/%u reduction %u lod %g..%g bias %g aniso %g border %g %g %g %g",
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
                    HLOG("watch", "sampler slot %u rewritten (frame %llu, pool %llx, %s, compare %u, wrap %u): %llx → %llx",
                         sid, (unsigned long long)g.cur_frame + 1, (unsigned long long)samppool, have_s ? "present" : "MISSING → default",
                         si.compare_mode, si.wrap[0], (unsigned long long)w, (unsigned long long)(uint64_t)smp);
            }
            w = (uint64_t)smp;
            tex_write(8, sid, VK_DESCRIPTOR_TYPE_SAMPLER, {smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED});
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

}  // namespace
bool shader_exclu(const std::string& hash);   // shaders_exclus.txt (plus bas, avec warm_up, hors de l'espace anonyme)
// Retire d'un module SPIR-V ce qui tient à SPV_KHR_quad_control : la capacité QuadControlKHR (5087), la déclaration de
// l'extension et les modes d'exécution QuadDerivativesKHR (5088) et RequireFullQuadsKHR (5089). Le module restant est
// celui que glslang aurait produit sans ces marques : pour les cartes où l'extension n'est pas activée.
void spirv_sans_quad_control(std::vector<char>& code) {
    if (code.size() < 20 || code.size() % 4) return;
    const uint32_t* w = (const uint32_t*)code.data();
    const size_t n = code.size() / 4;
    std::vector<uint32_t> out(w, w + 5);
    bool change = false;
    for (size_t i = 5; i < n;) {
        const uint32_t op = w[i] & 0xffff, wc = w[i] >> 16;
        if (!wc || i + wc > n) return;   // module mal formé : laissé tel quel
        bool retire = false;
        if (op == 17 && wc == 2 && w[i + 1] == 5087) retire = true;   // OpCapability QuadControlKHR
        else if (op == 10 && wc >= 2 && !strncmp((const char*)&w[i + 1], "SPV_KHR_quad_control", (wc - 1) * 4)) retire = true;
        else if (op == 16 && wc >= 3 && (w[i + 2] == 5088 || w[i + 2] == 5089)) retire = true;   // OpExecutionMode
        if (retire) change = true;
        else out.insert(out.end(), w + i, w + i + wc);
        i += wc;
    }
    if (!change) return;
    code.resize(out.size() * 4);
    memcpy(code.data(), out.data(), code.size());
}
namespace {

ShaderMod* get_shader(const std::string& hash) {
    auto it = g.shaders.find(hash);
    if (it != g.shaders.end()) return it->second.ok ? &it->second : nullptr;
    ShaderMod& sm = g.shaders[hash];
    sm.hash = hash;
    if (shader_exclu(hash)) {   // fait planter le compilateur de ce pilote (shaders_exclus.txt) : dessins ignorés
        HLOG("vk", "shader %s excluded for this driver: its draws are skipped", hash.c_str());
        return nullptr;
    }
    std::string base = cache_dir() + hash;
    std::ifstream f(base + ".spv", std::ios::binary);
    std::ifstream mf(base + ".meta");
    if (!f || !mf) {
        if (g.missing.insert(hash).second && g.missing.size() <= 50)
            HLOG("vk", "shader %s missing from the cache (run shaders/shaderc.py)", hash.c_str());
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
    if (!g.quad_control) spirv_sans_quad_control(code);
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
    HLOG("vk", "textures dumped: %d (of %zu packaged) into tex_dump/", n, list.size());
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
    static const bool plein = getenv("RECOMP_SNAPSHOT_FULL") != nullptr;   // diagnostic : instantanés non réduits
    w2 = std::max(1u, plein ? cw : cw / 2), h2 = std::max(1u, plein ? ch : ch / 2);
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
    if (!quiet) HLOG("vk", "snapshot: %s (%ux%u)", path, w2, h2);
}

}  // namespace

// ================================================================ interface (vk_render.h)
bool available() { return g.ok; }

void pool_flushed(uint64_t addr, uint64_t size) {
    if (size) flush_note(addr, addr + size);
}

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
        // plus d'objet texture : si le jeu écrit dans ses données rendues, il réemploie la mémoire (memory.cpp)
        if (im.rel_id) mem_release_vivant(im.rel_d, im.rel_f - im.rel_d, im.rel_id, false);
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
    HLOG("vk", "pipeline cache: %s (%.1f MB)", data.empty() ? "empty" : "loaded", data.size() / 1048576.0);
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
// En jeu : sauvegarde sur un autre thread si des pipelines ont été créés depuis la dernière. Jusqu'au 2026-11-11, une
// par minute : chaque sauvegarde relit les 157 Mo du cache (vkGetPipelineCacheData), les copie et les écrit sur le
// disque, soit ≈ 0,4 Go de mémoire pendant un instant et une écriture inutile en pleine partie. Le cache est de toute
// façon écrit à la fermeture du jeu (save_caches) et après une compilation au démarrage : en jeu, il n'est plus écrit
// que toutes les 10 minutes, et seulement s'il reste 8 Go validables libres. (Ce n'est pas la cause des pics de 2 Go
// observés le même jour : voir la section « Pics de mémoire en jeu » de recomp/README.md.)
std::atomic<bool> g_pc_saving{false};
void pcache_save_later() {
    using namespace std::chrono;
    if (!g.pc_dirty || steady_clock::now() - g.pc_saved < minutes(10)) return;
    if (memoire_validable_libre_go() < 8.0) return;   // retenté 600 images plus tard
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
// Le pilote compile (création d'un pipeline ou d'une bibliothèque de shaders) : au-delà de 5 s, le compteur du jeu
// s'arrête jusqu'à la fin, pour que le chien de garde de son moteur n'arrête pas le jeu (runtime.cpp, rt_stall_begin).
struct Blocage {
    Blocage() { rt_stall_begin(); }
    ~Blocage() { rt_stall_end(); }
    Blocage(const Blocage&) = delete;
    Blocage& operator=(const Blocage&) = delete;
};

VkPipeline make_library(VkGraphicsPipelineCreateInfo& gp, VkGraphicsPipelineLibraryFlagsEXT part) {
    Blocage bl;
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
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO};
    rs.requiredSubgroupSize = 32;
    if (g.sg32) ci.stage.pNext = &rs;   // warps de 32 voies, comme sur la console (voir Vk::sg32)
    ci.layout = g.layout;
    VkPipeline p = VK_NULL_HANDLE;
    Blocage bl;
    if (vkCreateComputePipelines(g.dev, g.pcache, 1, &ci, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    return p;
}
uint64_t vs_library_key(VkShaderModule m, VkPolygonMode mode, bool neg_one) {
    const uint64_t k[3] = {(uint64_t)m, (uint64_t)mode, (uint64_t)neg_one};
    return fnv(k, sizeof k);
}
// bibliothèques de shaders : précompilées (warm_up) ; sinon compilées ici, une fois (variante rare)
// Shader préparé au premier usage (shaders_on_demand, ou shader absent du cache) : au-delà de 50 ms, c'est une saccade
// et souvent une grosse compilation du pilote ; son nom est noté (les 40 premiers).
void lent_au_premier_usage(const char* etage, const std::string& hash, std::chrono::steady_clock::time_point debut) {
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - debut).count();
    static int dits = 0;
    if (ms > 50 && dits++ < 40)
        HLOG("vk", "%s shader %s prepared at first use in %.0f ms (frame %llu)", etage, hash.c_str(), ms,
             (unsigned long long)g.cur_frame);
}

VkPipeline vs_library(ShaderMod* vs, VkPolygonMode mode) {
    const bool neg_one = !nvn::g_depth_zero_one;
    const uint64_t key = vs_library_key(vs->mod, mode, neg_one);
    auto it = g.lib_vs.find(key);
    if (it != g.lib_vs.end()) return it->second;
    const auto t_creation = std::chrono::steady_clock::now();
    VkPipeline p = create_vs_library(vs->mod, mode, neg_one);
    g.lib_vs[key] = p;
    lent_au_premier_usage("vertex", vs->hash, t_creation);
    if (g.libs_on_demand++ < 20 && !g.libs_sur_demande)
        HLOG("vk", "vertex shader %s compiled in game (mode %d)", vs->hash.c_str(), (int)mode);
    return p;
}
VkPipeline fs_library(ShaderMod* fs) {
    auto it = g.lib_fs.find((uint64_t)fs->mod);
    if (it != g.lib_fs.end()) return it->second;
    const auto t_creation = std::chrono::steady_clock::now();
    VkPipeline p = create_fs_library(fs->mod);
    g.lib_fs[(uint64_t)fs->mod] = p;
    lent_au_premier_usage("fragment", fs->hash, t_creation);
    if (g.libs_on_demand++ < 20 && !g.libs_sur_demande) HLOG("vk", "fragment shader %s compiled in game", fs->hash.c_str());
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
                HLOG("pipe", "pipeline %s/%s: inputs %.2f ms, output %.2f ms, linking %.2f ms", vs->hash.c_str(), fs->hash.c_str(), vi, fo, link);
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
    // RECOMP_PIPE_NO_OPT=1 (essai) : demande au pilote de ne pas optimiser le pipeline lié. Après une liaison rapide,
    // un fil du pilote NVIDIA recompile le pipeline en arrière-plan (nvgpucomp64.dll) et prend ≈ 2 Go pendant quelques
    // secondes pour les gros shaders (mesuré le 2026-11-11 : c'est le pic de mémoire des changements de scène).
    static const bool sans_opt = getenv("RECOMP_PIPE_NO_OPT") != nullptr;
    if (sans_opt) gp.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    VkPipeline p = VK_NULL_HANDLE;
    {
        Blocage bl;
        if (vkCreateGraphicsPipelines(g.dev, g.pcache, 1, &gp, nullptr, &p) != VK_SUCCESS) p = VK_NULL_HANDLE;
    }
    lap(t_link);
    return g.linked[lk] = p;
}

// ---- shaders qui font planter le compilateur du pilote (issue GitHub n° 1) ----
// Chez certains joueurs (RTX 5070, pilote 617.42), le compilateur du pilote plante (0xC0000005) sur un shader pendant
// --precompile. precompile() isole alors le shader fautif et l'inscrit dans build/full/shaders_exclus.txt, sous le nom du
// pilote : pour ce pilote seulement, il n'est ni précompilé ni dessiné (dessins ignorés au lieu d'un plantage). Après une
// mise à jour du pilote, la liste ne vaut plus et le shader est réessayé.
std::string exclus_path() { return exe_dir() + "shaders_exclus.txt"; }
std::string suivi_path() { return exe_dir() + "precompile_en_cours.txt"; }
std::string pilote_id() {
    char b[64];
    snprintf(b, sizeof b, "pilote %04x:%04x %u", g.props.vendorID, g.props.deviceID, g.props.driverVersion);
    return b;
}
bool shader_exclu(const std::string& hash) {
    static const std::set<std::string> exclus = [] {   // lu au premier shader (Vulkan initialisé : pilote connu)
        std::set<std::string> s;
        std::ifstream f(exclus_path());
        const std::string moi = pilote_id();
        bool ce_pilote = false;
        for (std::string l; std::getline(f, l);) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            if (l.empty() || l[0] == '#') continue;
            if (l.rfind("pilote ", 0) == 0) ce_pilote = l == moi;
            else if (ce_pilote) s.insert(l);
        }
        if (!s.empty()) HLOG("vk", "%zu shader(s) excluded for this driver (shaders_exclus.txt)", s.size());
        return s;
    }();
    return exclus.count(hash) != 0;
}
// --precompile (processus enfant) : « +nom » avant de compiler un shader, « -nom » après. Après un plantage, les shaders
// ouverts sont ceux qui étaient en cours (un par fil de compilation).
FILE* g_suivi = nullptr;
std::mutex g_suivi_lk;
void suivre(char c, const std::string& hash) {
    if (!g_suivi) return;
    std::lock_guard<std::mutex> lk(g_suivi_lk);
    fprintf(g_suivi, "%c%s\n", c, hash.c_str());
    fflush(g_suivi);
}

// Crée, en parallèle, les bibliothèques de tous les shaders du cache et les pipelines de calcul. Depuis pipelines.cache,
// quelques dizaines de millisecondes ; sinon (premier lancement sans le launcher), la compilation complète.
// avance(k, n) : progression, appelée sur ce thread. Rend le nombre de shaders prêts.
// RECOMP_PRECOMPILE_SEULS=a,b,… : ces shaders seulement (isolement d'un plantage) ; RECOMP_PRECOMPILE_FILS=n : n fils ;
// RECOMP_PRECOMPILE_PLANTE=nom : plantage simulé sur ce shader (essai de l'isolement).
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
    std::set<std::string> seuls;
    if (const char* e = getenv("RECOMP_PRECOMPILE_SEULS")) {
        std::stringstream ss(e);
        for (std::string h; std::getline(ss, h, ',');)
            if (!h.empty()) seuls.insert(h);
    }
    const char* plante = getenv("RECOMP_PRECOMPILE_PLANTE");
    struct Job { ShaderMod* sm; uint64_t key; VkPipeline out; };
    std::vector<Job> jobs;
    for (ShaderMod* sm : mods) {
        if (!seuls.empty() && !seuls.count(sm->hash)) continue;
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
            suivre('+', j.sm->hash);
            if (plante && j.sm->hash == plante) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
            if (j.sm->model == 0) j.out = create_vs_library(j.sm->mod, VK_POLYGON_MODE_FILL, neg_one);
            else if (j.sm->model == 4) j.out = create_fs_library(j.sm->mod);
            else j.out = create_compute_pipeline(j.sm->mod);
            suivre('-', j.sm->hash);
            const size_t d = ++done;
            if (rapporte) avance(d, jobs.size());
        }
    };
    const unsigned hc = std::thread::hardware_concurrency();
    unsigned nth = std::max(1u, std::min(16u, hc > 2 ? hc - 1 : 1u));
    if (const char* e = getenv("RECOMP_PRECOMPILE_FILS")) nth = std::max(1, std::min(16, atoi(e)));
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
    HLOG("vk", "pipelines: %zu shaders from the cache, %zu prepared (%zu vertex, %zu fragment, %zu compute, %zu failures) in %.2f s",
         mods.size(), jobs.size(), nvs, nfs, ncs, ko, s);
    return jobs.size() - ko;
}

// Signature de ce que --precompile a compilé : GPU, pilote, mode des pipelines, liste des shaders (nombre, taille, date
// la plus récente) et exécutable (taille, date : le pilote NVIDIA garde aussi son propre cache par programme ; après
// un nouvel exécutable, le réchauffer pendant la précompilation, avec la progression dans le launcher, plutôt qu'au
// démarrage du jeu). pipelines.ok garde la signature de la dernière précompilation complète.
std::string pipelines_signature(uint64_t* nombre_out = nullptr) {
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
    if (nombre_out) *nombre_out = nombre;
    return sig;
}
std::string pipelines_marque() { return exe_dir() + "pipelines.ok"; }
// pipelines.cache contient déjà tous les shaders, pour ce GPU, ce pilote, ces shaders et cet exécutable
bool pipelines_a_jour(const std::string& sig) {
    std::ifstream f(pipelines_marque());
    std::string avant;
    std::getline(f, avant);
    return avant == sig && GetFileAttributesA(pcache_path().c_str()) != INVALID_FILE_ATTRIBUTES;
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
        HLOG("vk", "Vulkan initialization failed: GDI presentation");
        return;
    }
    pcache_open();
    if (const char* e = getenv("RECOMP_STALL_TEST")) {   // essai : blocage simulé de n secondes, comme une longue compilation
        Blocage bl;
        HLOG("vk", "simulated stall of %d s (RECOMP_STALL_TEST)", atoi(e));
        Sleep((DWORD)atoi(e) * 1000);
        HLOG("vk", "simulated stall over");
    }
    // Shaders à la demande (recomp.ini, shaders_on_demand) : quand pipelines.cache contient déjà tout (précompilation du
    // launcher faite pour ce GPU, ce pilote, ces shaders et cet exécutable), les bibliothèques ne sont plus toutes
    // créées au démarrage mais au premier dessin, depuis le cache. Créées d'avance, elles prenaient ≈ 0,6 Go de
    // mémoire dans le pilote (0,75 Go à la première image) pour 150 à 250 shaders utilisés sur 2 388 en 5 minutes de
    // jeu, sans image perdue de plus à la demande (mesuré le 2026-11-11).
    if (settings().shaders_on_demand && pipelines_a_jour(pipelines_signature())) {
        HLOG("vk", "pipelines: shader libraries created at first use (pipelines.cache is complete for this GPU, driver "
                   "and game build)");
        g.libs_sur_demande = true;
        return;
    }
    // sinon : tous les shaders prêts avant la première image (compilation si pipelines.cache n'est pas à jour).
    // Un seul blocage pour le compteur du jeu : sur une carte AMD cette préparation peut dépasser le délai du chien de
    // garde du moteur (issue n° 13).
    Blocage bl;
    const auto t0 = std::chrono::steady_clock::now();
    int dernier = -1;
    warm_up([&](size_t k, size_t n) {
        const int pct = n ? (int)(k * 100 / n) : 100;
        if (pct / 5 == dernier / 5 || std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500)) return;
        dernier = pct;
        char b[96];
        snprintf(b, sizeof b, "compiling shaders %d %%", pct);
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
// Le travail se fait dans un processus enfant (precompile_travail) : si le compilateur du pilote y plante, le parent
// (precompile_superviseur) isole le shader fautif, l'écarte pour ce pilote et recommence.
int precompile_travail() {
    auto dire = [](const char* f, auto... a) {
        printf(f, a...);
        fflush(stdout);
    };
    if (!init(nullptr)) {
        dire("@erreur Vulkan 1.3 unavailable\n");
        return 1;
    }
    dire("@%s\n", pilote_id().c_str());   // pour le parent : shaders_exclus.txt est rangé par pilote
    if (getenv("RECOMP_PRECOMPILE_SEULS")) {   // isolement : ces shaders un par un, sans rien écrire d'autre
        g_suivi = fopen(suivi_path().c_str(), "w");
        const size_t n = warm_up([](size_t, size_t) {});
        if (g_suivi) fclose(g_suivi), g_suivi = nullptr;
        dire("@info isolation: %zu shaders compiled without a crash\n", n);
        return 0;
    }
    uint64_t nombre = 0;
    const std::string sig = pipelines_signature(&nombre);
    const std::string marque = pipelines_marque();
    if (pipelines_a_jour(sig)) {
        dire("@saute shaders already compiled for this GPU and driver\n");
        return 0;
    }
    dire("@info %s: %llu shaders\n", g.props.deviceName, (unsigned long long)nombre);
    pcache_open();
    int dernier = -1;
    const auto t0 = std::chrono::steady_clock::now();
    g_suivi = fopen(suivi_path().c_str(), "w");
    const size_t n = warm_up([&](size_t k, size_t total) {
        const int pm = total ? (int)(k * 1000 / total) : 1000;
        if (pm == dernier) return;
        dernier = pm;
        dire("@progres %.3f\n", pm / 1000.0);
    });
    if (g_suivi) fclose(g_suivi), g_suivi = nullptr;
    DeleteFileA(suivi_path().c_str());   // pas de plantage : rien en cours
    if (!pcache_write()) {
        dire("@erreur pipeline cache not written\n");
        return 1;
    }
    std::ofstream(marque) << sig << "\n";
    dire("@info %zu shaders compiled in %.1f s\n", n, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}

// Lance « recomp_main --precompile » en processus enfant (RECOMP_PRECOMPILE_ENFANT=1, plus `envs`), relaie sa sortie
// si `relayer`, retient sa ligne « @pilote … ». Rend son code de sortie (0 fini, 1 erreur annoncée, autre : plantage).
// Sa mémoire est surveillée dix fois par seconde : au-delà de `plafond` octets engagés, ou quand il reste moins de
// 2 Go validables libres sur le PC, il est arrêté (*manque : 1 plafond dépassé, 2 mémoire du PC presque épuisée) avant
// que le compilateur du pilote ne plante faute de mémoire, ou ne fasse tomber d'autres programmes.
DWORD precompile_enfant(const std::vector<std::pair<std::string, std::string>>& envs, bool relayer, const std::string& journal,
                        std::string& pilote, uint64_t plafond, int* manque) {
    *manque = 0;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    SetEnvironmentVariableA("RECOMP_PRECOMPILE_ENFANT", "1");
    for (auto& [k, v] : envs) SetEnvironmentVariableA(k.c_str(), v.c_str());
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE lit = nullptr, ecrit = nullptr;
    CreatePipe(&lit, &ecrit, &sa, 0);
    SetHandleInformation(lit, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = ecrit;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = std::string("\"") + exe + "\" --log \"" + journal + "\" --precompile";
    const BOOL ok = CreateProcessA(exe, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableA("RECOMP_PRECOMPILE_ENFANT", nullptr);
    for (auto& [k, v] : envs) SetEnvironmentVariableA(k.c_str(), nullptr);
    CloseHandle(ecrit);
    if (!ok) {
        CloseHandle(lit);
        printf("@erreur recomp_main --precompile not started (%lu)\n", GetLastError());
        fflush(stdout);
        return 1;
    }
    std::atomic<bool> fini{false};
    std::atomic<int> arret{0};
    std::thread veille([&] {
        while (!fini.load()) {
            PROCESS_MEMORY_COUNTERS pmc{};
            pmc.cb = sizeof pmc;
            const bool lu = K32GetProcessMemoryInfo(pi.hProcess, &pmc, sizeof pmc) != 0;
            const double libre = memoire_validable_libre_go();
            if ((lu && pmc.PagefileUsage > plafond) || libre < 2.0) {
                arret = lu && pmc.PagefileUsage > plafond ? 1 : 2;
                HLOG("vk", "--precompile: compiler process stopped, %.1f GB committed, %.1f GB available commit on the PC",
                     pmc.PagefileUsage / 1073741824.0, libre);
                TerminateProcess(pi.hProcess, 0xC000012D);   // STATUS_COMMITMENT_LIMIT
                return;
            }
            Sleep(100);
        }
    });
    std::string ligne;
    auto traiter = [&](const std::string& l) {
        if (l.rfind("@pilote ", 0) == 0) pilote = l.substr(1);
        else if (relayer) printf("%s\n", l.c_str()), fflush(stdout);
    };
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(lit, buf, sizeof buf, &n, nullptr) && n) {
        for (DWORD i = 0; i < n; i++) {
            if (buf[i] == '\n') traiter(ligne), ligne.clear();
            else if (buf[i] != '\r') ligne += buf[i];
        }
    }
    if (!ligne.empty()) traiter(ligne);
    CloseHandle(lit);
    WaitForSingleObject(pi.hProcess, INFINITE);
    fini = true;
    veille.join();
    *manque = arret.load();
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

// Shaders commencés et pas finis au moment d'un plantage (precompile_en_cours.txt)
std::vector<std::string> precompile_en_cours() {
    std::vector<std::string> ouverts;
    std::ifstream f(suivi_path());
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.size() < 2) continue;
        const std::string h = l.substr(1);
        if (l[0] == '+') ouverts.push_back(h);
        else if (l[0] == '-') ouverts.erase(std::remove(ouverts.begin(), ouverts.end(), h), ouverts.end());
    }
    return ouverts;
}

// Ajoute `hash` aux shaders écartés de ce pilote (une seule section : celle du pilote actuel)
void ecarter_shader(const std::string& pilote, const std::string& hash) {
    std::vector<std::string> lignes;
    {
        std::ifstream f(exclus_path());
        for (std::string l; std::getline(f, l);) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lignes.push_back(l);
        }
    }
    if (std::find(lignes.begin(), lignes.end(), pilote) == lignes.end())
        lignes = {"# shaders qui font planter le compilateur du pilote ci-dessous, ou lui font épuiser la mémoire : ni "
                  "précompilés ni dessinés",
                  pilote};
    if (std::find(lignes.begin(), lignes.end(), hash) == lignes.end()) lignes.push_back(hash);
    std::ofstream o(exclus_path(), std::ios::trunc);
    for (auto& l : lignes) o << l << "\n";
}

// Plafonds de mémoire engagée du processus de compilation (RECOMP_PRECOMPILE_MAX_MO pour les essais : les deux).
// Un shader seul : 8 Go (mesures du plus gros shader : 0,8 Go sur NVIDIA, plus de 11,5 Go sur AMD). Tous les fils
// ensemble : 16 Go, et jamais plus que ce que le PC a de libre moins 2 Go (surveillé à part).
uint64_t plafond_precompile(bool un_seul) {
    if (const char* e = getenv("RECOMP_PRECOMPILE_MAX_MO")) return (uint64_t)strtoull(e, nullptr, 10) << 20;
    return (un_seul ? 8ull : 16ull) << 30;
}

int precompile_superviseur() {
    auto dire = [](const char* f, auto... a) {
        printf(f, a...);
        fflush(stdout);
    };
    const std::string journal = exe_dir() + "precompile.log", journal_isole = exe_dir() + "precompile_isolement.log";
    std::vector<std::pair<std::string, std::string>> envs;   // compilation (sur un seul fil après un plantage non isolé)
    for (int tour = 0; tour < 12; tour++) {
        std::string pilote;
        int manque = 0;
        const DWORD code = precompile_enfant(envs, true, journal, pilote, plafond_precompile(!envs.empty()), &manque);
        if (!manque && (code == 0 || code == 1)) return (int)code;
        // plantage du pilote, ou mémoire épuisée par son compilateur : les shaders qui étaient en cours, recompilés un
        // par un, désignent le fautif
        const std::vector<std::string> suspects = precompile_en_cours();
        std::string liste;
        for (auto& s : suspects) liste += (liste.empty() ? "" : ",") + s;
        if (manque)
            dire("shader compilation stopped: the driver's compiler was %s; in progress: %s\n",
                 manque == 1 ? "taking too much memory" : "running the PC out of memory", liste.empty() ? "none" : liste.c_str());
        else
            dire("crash during shader compilation (code 0x%08lx); in progress: %s\n", code, liste.empty() ? "none" : liste.c_str());
        HLOG("vk", "--precompile: %s 0x%08lx, in progress: %s", manque ? "stopped for memory," : "crash", code, liste.c_str());
        if (manque && memoire_validable_libre_go() < 4.0) {   // le PC manque de mémoire même sans le compilateur
            dire("@erreur not enough free memory to compile the shaders (%.1f GB available): close other programs and "
                 "try again\n", memoire_validable_libre_go());
            return 2;
        }
        if (suspects.empty() || pilote.empty()) {
            dire("@erreur shader compilation %s (code 0x%08lx): see precompile.log\n", manque ? "ran out of memory" : "crashed", code);
            return 2;
        }
        dire("@info isolating the faulty shader\n");
        std::string pilote2;
        int manque2 = 0;
        const DWORD c2 = precompile_enfant({{"RECOMP_PRECOMPILE_SEULS", liste}, {"RECOMP_PRECOMPILE_FILS", "1"}}, false,
                                           journal_isole, pilote2, plafond_precompile(true), &manque2);
        if (manque2 == 2) {
            dire("@erreur not enough free memory to compile the shaders (%.1f GB available): close other programs and "
                 "try again\n", memoire_validable_libre_go());
            return 2;
        }
        const std::vector<std::string> fautif = !manque2 && (c2 == 0 || c2 == 1) ? std::vector<std::string>{} : precompile_en_cours();
        if (fautif.size() == 1) {
            ecarter_shader(pilote, fautif[0]);
            dire("shader %s excluded for this driver (%s): %s; its draws will be skipped\n", fautif[0].c_str(), pilote.c_str(),
                 manque2 ? "its compiler needs too much memory" : "it crashes its compiler");
            HLOG("vk", "--precompile: shader %s excluded for this %s (%s)", fautif[0].c_str(), pilote.c_str(),
                 manque2 ? "too much memory" : "crash");
            continue;
        }
        if (envs.empty()) {   // pas reproduit shader par shader : la compilation en parallèle elle-même ?
            envs = {{"RECOMP_PRECOMPILE_FILS", "1"}};
            dire("%s not reproduced shader by shader: compilation resumed on a single thread\n", manque ? "memory use" : "crash");
            continue;
        }
        dire("@erreur shader compilation %s (code 0x%08lx) with no shader identified: see precompile.log\n",
             manque ? "runs out of memory" : "crashes", code);
        return 2;
    }
    dire("@erreur too many shaders crash the driver: see precompile_controle.log\n");
    return 2;
}

int precompile() {
    if (getenv("RECOMP_PRECOMPILE_ENFANT")) return precompile_travail();
    return precompile_superviseur();
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
    flush_tex_writes();   // RECOMP_TEX_WRITE_BATCH : écritures du jeu de textures de cet enregistrement (voir tex_write)
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
    g.staging_bytes = 0;
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
// Entre deux dessins : au-delà de 256 Mo de tampons de transit dans l'enregistrement en cours, il est soumis, le GPU
// attendu et les tampons rendus (voir free_done_staging). memory_saver = false : comme en 1.0.2, pas de borne.
void borner_transit() {
    constexpr uint64_t TRANSIT_MAX = 256ull << 20;
    if (g.staging_bytes <= TRANSIT_MAX || !settings().memory_saver) return;
    flush_locked("texture uploads");
    free_done_staging();
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
        HLOG("vk", "draw skipped (%s): '%s'", why, label.c_str());
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
            else { skip(st.stage == 5 ? "compute program bound" : "geometry/tessellation stage", prog.label); return; }
            if ((st.stage == 0 && !vs) || (st.stage == 1 && !fs)) {
                skip(st.stage == 0 ? "missing vertex shader" : "missing fragment shader", prog.label);
                return;
            }
        }
        if (!vs) {
            skip("no vertex shader", prog.label);
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
    for (int i = 0; i < nrt; i++)
        if (rts[i] && rts[i]->target == 2) {   // RECOMP_3D_LOG : volume 3D en cible de rendu, qui, quelle tranche
            static const bool log3d = getenv("RECOMP_3D_LOG") != nullptr;
            static int n3 = 0;
            if (log3d && n3++ < 24)
                HLOG("vk", "3D render target %ux%u fmt %u, level %u layer %u, kind %d, frame %llu: '%s' (fs %s)", rts[i]->w,
                     rts[i]->h, rts[i]->nvnfmt, s.rt_level[i], s.rt_layer[i], (int)d.kind,
                     (unsigned long long)g.cur_frame + 1, prog.label.c_str(), fs ? fs->hash.c_str() : "-");
        }
    if (g.dbg) {
        HLOG("vkdbg", "draw %d '%s' vs %s fs %s", (int)d.kind, prog.label.c_str(), vs ? vs->hash.c_str() : "-",
             fs ? fs->hash.c_str() : "-");
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
                if (!im) return "no image";
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
                snprintf(b, sizeof b, "%ux%u×%u fmt %u target %u key %llx smp %u%s%u/%u%s", im->w, im->h, im->layers, im->nvnfmt,
                         im->target, (unsigned long long)td.tex, sid, have_s ? " " : " missing ", si.min, si.mag,
                         cur ? " = CURRENT TARGET" : "");
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
            snprintf(st, sizeof st, " | blend %08x (funcs %u %u %u %u eq. %u %u, constant %g %g %g %g) mask %08x ds %02x%02x%02x%02x%02x%02x%02x%02x poly %08x",
                     s.color, bl[1], bl[2], bl[3], bl[4], bl[5], bl[6], s.blend_color[0], s.blend_color[1], s.blend_color[2],
                     s.blend_color[3], s.chmask, s.ds[0], s.ds[1], s.ds[2], s.ds[3], s.ds[4], s.ds[5], s.ds[6], s.ds[7], s.poly);
            static std::string last;   // n'imprime que les changements de liaisons ou d'état
            std::string all = rt + st + l;
            if (all != last) {
                last = all;
                HLOG("watch", "frame %llu draw %llu '%s' depth %s%s%s:%s", (unsigned long long)g.cur_frame + 1,
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
        skip("no fragment shader with targets", prog.label);
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
        {
            Blocage bl;
            if (g.gpl && fs) pipe = gpl_pipeline(vs, fs, vi, ia.topology, rs.polygonMode, cbs, pri, ms.alphaToCoverageEnable);
            else if (vkCreateGraphicsPipelines(g.dev, g.pcache, 1, &gp, nullptr, &pipe) != VK_SUCCESS) pipe = VK_NULL_HANDLE;
        }
        static const bool pipe_timing = getenv("RECOMP_PIPE_TIMING") != nullptr;
        if (pipe_timing)
            HLOG("pipe", "frame %llu: pipeline %s ('%s') in %.2f ms", (unsigned long long)g.cur_frame + 1,
                 g.gpl && fs ? "from libraries" : "full", prog.label.c_str(),
                 std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_pipe).count());
        if (!pipe) HLOG("vk", "pipeline creation failed (prog '%s')", prog.label.c_str());
        g.pipelines[kh] = pipe;
        g.pc_dirty++;
    }
    if (!pipe) {
        skip("pipeline not created", prog.label);
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
        skip("descriptors exhausted", prog.label);
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
        if (st < 32 && s.vb[st].addr && !find_pool(s.vb[st].addr, std::max<uint64_t>(s.vb[st].size, 1), &b, &o)) {
            static std::set<std::string> seen;
            if (seen.size() < 64 && seen.insert(prog.label + "|" + std::to_string(st)).second)
                HLOG("vk", "vertex buffer not found: stream %u at 0x%llx ('%s')", st,
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
        snprintf(b, sizeof b, "frame %llu draw %llu (%s n %u ×%u) '%s' vs %s fs %s", (unsigned long long)g.cur_frame + 1,
                 (unsigned long long)g.frame_draws, d.kind == DrawCall::Arrays ? "array" : "indexed", d.count, d.instances,
                 prog.label.c_str(), vs->hash.c_str(), fs ? fs->hash.c_str() : "-");
        mk = b;
        mark("before " + mk);
    }
    const int pq = prof_begin();
    if (d.kind == DrawCall::Arrays) {
        vkCmdDraw(g.cb, d.count, d.instances, d.first, d.base_instance);
    } else {
        VkBuffer b;
        uint64_t o;
        uint32_t isz = d.index_type == 0 ? 1 : d.index_type == 1 ? 2 : 4;
        if (!find_pool(d.index_addr, (uint64_t)d.count * isz, &b, &o) || d.index_type == 0) {
            skip(d.index_type == 0 ? "8-bit index" : "index buffer not found", prog.label);
            prof_end(pq, "(draw skipped)");
            return;
        }
        vkCmdBindIndexBuffer(g.cb, b, o, d.index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(g.cb, d.count, d.instances, 0, d.base_vertex, d.base_instance);
    }
    if (pq >= 0) prof_end(pq, prog.label + (fs ? " [fs " + fs->hash + "]" : " [no fs]"));
    if (g.checkpoints) mark("after " + mk);
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
    if (sync_each()) flush_locked("isolated draw");
    else borner_transit();
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
    if (im->target == 2) {   // RECOMP_3D_LOG : volume 3D écrit par un calcul
        static const bool log3d = getenv("RECOMP_3D_LOG") != nullptr;
        static int n3 = 0;
        if (log3d && n3++ < 24)
            HLOG("vk", "3D storage image %ux%u fmt %u (texture %u) bound, frame %llu, last program '%s'", im->w, im->h,
                 im->nvnfmt, tid, (unsigned long long)g.cur_frame + 1, g.last_label.c_str());
    }
    image_to_general(*im);
    VkImageView v = storage_view(*im, dim);
    if (!v) {
        static int nw = 0;
        if (nw++ < 8) HLOG("vk", "image %u (NVN fmt %u, target %u) not bindable as storage dim %d", tid, im->nvnfmt, im->target, dim);
        return;
    }
    auto& w = g.img_written[tid * 8 + dim];
    if (w == (uint64_t)v) return;
    w = (uint64_t)v;
    tex_write(10 + dim, tid, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, {VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_GENERAL});
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
        skip("missing compute shader", prog.label);
        return;
    }
    static const bool no_glob = getenv("RECOMP_NO_GLOBAL_MEM") != nullptr;   // diagnostic
    if (cs->glob && no_glob) {
        skip("compute with global memory disabled", prog.label);
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
            HLOG("vk", "compute %s '%s' %ux%ux%u: UBO %d, storage buffers %d, inline words %d%s", cs->hash.c_str(),
                 prog.label.c_str(), gx, gy, gz, cs->c0[0], cs->c0[4], cs->c0[3], cs->glob ? ", global memory" : "");
    }
    end_render();
    if (g.dbg) HLOG("vkdbg", "dispatch %ux%ux%u '%s'", gx, gy, gz, prog.label.c_str());
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
        const auto t_creation = std::chrono::steady_clock::now();
        pipe = create_compute_pipeline(cs->mod);
        lent_au_premier_usage("compute", cs->hash, t_creation);
        if (!pipe) HLOG("vk", "compute pipeline creation failed ('%s')", prog.label.c_str());
        g.cpipelines[(uint64_t)cs->mod] = pipe;
        g.pc_dirty++;
    }
    if (!pipe) {
        skip("compute pipeline not created", prog.label);
        return;
    }
    // banques : 0 = pilote (poignées d'images), 3 + k = tampon uniforme k (hypothèse : lue en c[3])
    VkDescriptorSet set;
    if (!alloc_bank_sets(1, &set)) {
        skip("compute descriptors exhausted", prog.label);
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
                if (cs->glob && r.addr && r.size && !getenv("RECOMP_GLOB_RETOUR_SEUL") && !guest_devaddr(r.addr, r.size)) {   // importe sa fenêtre au besoin
                    static int nmiss = 0;
                    if (nmiss++ < 10)
                        HLOG("vk", "storage buffer %u of compute '%s' outside the pools (0x%llx +0x%llx): accesses skipped", k,
                             prog.label.c_str(), (unsigned long long)r.addr, (unsigned long long)r.size);
                }
            }
            if (cs->glob) {   // table des fenêtres importées que le calcul peut lire par adresse (voir glob_fenetre)
                glob_retour();
                // RECOMP_GLOB_RETOUR_SEUL=1 (essai) : ni pré-import ni balayage, le tampon de retour fait tout le travail
                static const bool retour_seul = getenv("RECOMP_GLOB_RETOUR_SEUL") != nullptr;
                const bool tout = !settings().memory_saver;   // comme en 1.0.2 : tous les pools, en entier
                if (tout) import_all_pools();
                const auto plages = retour_seul || tout ? std::vector<std::pair<uint64_t, uint64_t>>() : nvn_pool_plages();
                for (uint32_t k = 0; !retour_seul && k < (uint32_t)std::max(0, cs->c0[0]) && k < 32; k++) {
                    glob_fenetre(s.ubo[5][k].addr, s.ubo[5][k].size);
                    if (!tout) glob_balayer(s.ubo[5][k], 0x10000, plages);
                }
                for (uint32_t k = 0; !retour_seul && k < nssbo && k < 32; k++) {
                    glob_fenetre(s.ssbo[5][k].addr, s.ssbo[5][k].size);
                    if (!tout) glob_balayer(s.ssbo[5][k], 0x40000, plages);
                }
                std::vector<const PoolBuf*> ordre;   // fenêtres des calculs d'abord, puis les plus récentes
                for (auto& pl : g.pools)
                    if (pl.dev) ordre.push_back(&pl);
                std::stable_sort(ordre.begin(), ordre.end(), [](const PoolBuf* a, const PoolBuf* b) {
                    return a->calc != b->calc ? a->calc : a->last > b->last;
                });
                uint32_t n = 0;
                for (const PoolBuf* pl : ordre) {
                    if (0x610 + 32 * n + 32 > B0) break;
                    const uint64_t t[4] = {pl->base, pl->dev, pl->size, 0};
                    memcpy(p + 0x610 + 32 * n, t, 32);
                    n++;
                }
                memcpy(p + GTAB, &n, 4);
                memcpy(p + GTAB + 8, &g.fb_dev, 8);   // tampon de retour des adresses absentes (gxl_miss, glsl.py)
                static bool plein = false;
                if (!plein && n < ordre.size() && ordre[n]->calc) {
                    plein = true;
                    HLOG("vk", "global memory table full: %zu compute windows for %u entries", ordre.size(), n);
                }
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
        snprintf(b, sizeof b, "frame %llu compute %llu (%ux%ux%u) '%s' cs %s", (unsigned long long)g.cur_frame + 1,
                 (unsigned long long)g.frame_dispatches, gx, gy, gz, prog.label.c_str(), cs->hash.c_str());
        mk = b;
        mark("before " + mk);
    }
    const int pq = prof_begin();
    vkCmdDispatch(g.cb, gx, gy, gz);
    if (pq >= 0) prof_end(pq, prog.label + " [calcul " + cs->hash + "]");
    if (g.checkpoints) mark("after " + mk);
    barrier_all();
    g.frame_dispatches++;
    g.last_label = prog.label + " (calcul)";
    if (sync_each()) flush_locked("isolated compute");
    else borner_transit();
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
        HLOG("vk", "check frame %llu: working set %.0f MB, private memory %.0f MB, page faults %lu",
             (unsigned long long)frame, pmc.WorkingSetSize / 1048576.0, pmc.PrivateUsage / 1048576.0, pmc.PageFaultCount);
    if (g.mem_budget && vkGetPhysicalDeviceMemoryProperties2) {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT bp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
        VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
        mp.pNext = &bp;
        vkGetPhysicalDeviceMemoryProperties2(g.pd, &mp);
        for (uint32_t h = 0; h < mp.memoryProperties.memoryHeapCount; h++)
            HLOG("vk", "check frame %llu: heap %u (%s, %.0f MB): used %.0f MB / budget %.0f MB", (unsigned long long)frame, h,
                 (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "video" : "system",
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
    if (im->target == 2) {   // RECOMP_3D_LOG : volume 3D rempli par une copie, région demandée
        static const bool log3d = getenv("RECOMP_3D_LOG") != nullptr;
        static int n3 = 0;
        if (log3d && n3++ < 24)
            HLOG("vk", "CopyBufferToTexture to a 3D texture %ux%u fmt %u level %u: region %d,%d,%d %dx%dx%d, frame %llu", im->w,
                 im->h, im->nvnfmt, level, region[0], region[1], region[2], region[3], region[4], region[5],
                 (unsigned long long)g.cur_frame + 1);
    }
    if (!copy_region(*im, level, layer, region, &c.imageSubresource, &c.imageOffset, &c.imageExtent)) return;
    // données serrées : lignes de blocs 4×4 pour les formats compressés
    const uint32_t unit = is_compressed(im->fmt) ? 4 : 1;
    const uint64_t row = (uint64_t)((c.imageExtent.width + unit - 1) / unit) * im->bpu;
    const uint64_t size = row * ((c.imageExtent.height + unit - 1) / unit) * c.imageExtent.depth * c.imageSubresource.layerCount;
    VkBuffer b;
    uint64_t o;
    if (!find_pool(src, size, &b, &o)) {
        static int n = 0;
        if (n++ < 20) HLOG("vk", "CopyBufferToTexture: source 0x%llx +0x%llx outside the pools", (unsigned long long)src, (unsigned long long)size);
        return;
    }
    {   // une ligne par type de texture cible (taille, couches, format)
        static std::set<std::array<uint32_t, 4>> seen;
        if (seen.size() < 64 && seen.insert({im->w, im->h, im->layers, im->nvnfmt}).second)
            HLOG("vk", "CopyBufferToTexture 0x%llx → %ux%u×%u fmt %u level %u layer %u: %d,%d %ux%u ×%u (%llu bytes)",
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
        HLOG("vk", "  copy to %ux%u fmt %u (key %llx): %s", im->w, im->h, im->nvnfmt, (unsigned long long)texkey, hex.c_str());
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
            g.staging_bytes += size;
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
            HLOG("vk", "ClearTexture %ux%u×%u fmt %u level %u: %d,%d %ux%u color %g %g %g %g", im->w, im->h, im->layers,
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
        if (n++ < 10) HLOG("vk", "ClearTexture partial %ux%u at %d,%d on %ux%u: whole level cleared", ext.width, ext.height, off.x, off.y, im->w, im->h);
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
        g.staging_bytes += size;
    }
    VkBufferCopy c{so, dof, size};
    vkCmdCopyBuffer(g.cb, sb, db, 1, &c);
    barrier_all();
    static int nlog = 0;
    if (nlog++ < 6) HLOG("vk", "CopyBufferToBuffer 0x%llx → 0x%llx (%llu bytes): on the GPU", (unsigned long long)src,
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
                HLOG("vk", "CopyTextureToTexture %ux%u fmt %u → %ux%u fmt %u: depth ↔ color via buffer", si->w,
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
            HLOG("vk", "CopyTextureToTexture %ux%u fmt %u (%d,%d %ux%u) → %ux%u fmt %u (%d,%d %ux%u), flags %u: %s", si->w,
                 si->h, si->nvnfmt, so.x, so.y, se.width, se.height, di->w, di->h, di->nvnfmt, dof.x, dof.y, de.width, de.height,
                 flags, copyable ? "copy" : blittable ? "blit" : "NOT SUPPORTED");
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
        HLOG("vk", "GPU readback %s: 0x%llx +0x%llx outside the pools", path, (unsigned long long)addr, (unsigned long long)size);
        return;
    }
    for (auto& p : g.pools)
        if (p.buf == src) HLOG("vk", "GPU readback %s: served by import 0x%llx +0x%llx", path, (unsigned long long)p.base, (unsigned long long)p.size);
    {
        std::lock_guard<std::mutex> pl(nvn::g_pool.m);
        for (auto& [k, p] : nvn::g_pool.map)
            if (p.storage && addr >= p.storage && addr < p.storage + p.size)
                HLOG("vk", "GPU readback %s: current NVN pool 0x%llx +0x%llx flags 0x%x", path,
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

// RECOMP_TEX3D_DUMP=N (diagnostic) : niveau 0 de chaque texture 3D non compressée, relu après l'attente du GPU et
// écrit brut dans tex3d_<image>_<clé>_<l>x<h>x<p>_f<format NVN>.bin (dossier courant). Sert à comparer le contenu
// d'une table de couleurs entre deux cartes graphiques.
void tex3d_dump(uint64_t frame) {
    begin_frame();
    end_render();
    for (auto& [key, im] : g.images) {
        if (im.target != 2 || !im.initialized || is_compressed(im.fmt) || is_depth(im.fmt)) continue;
        nvn::Tex t;
        if (!nvn::g_tex.find_key(key, &t)) continue;
        const uint32_t d = (uint32_t)std::max(1, t.b.d);
        const uint64_t size = (uint64_t)im.w * im.h * d * im.bpu;
        if (!size || size > (64ull << 20)) continue;
        Readback r{};
        if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &r.b, &r.m))
            continue;
        barrier_all();
        VkBufferImageCopy c{};
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {im.w, im.h, d};
        vkCmdCopyImageToBuffer(g.cb, im.img, VK_IMAGE_LAYOUT_GENERAL, r.b, 1, &c);
        barrier_all();
        char path[160];
        snprintf(path, sizeof path, "tex3d_%llu_%016llx_%ux%ux%u_f%u.bin", (unsigned long long)frame, (unsigned long long)key,
                 im.w, im.h, d, im.nvnfmt);
        r.path = path;
        r.dump3d = size;
        g_readbacks.push_back(r);
    }
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
    dbg_tex("presented", 0, texkey);
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
            HLOG("vk", "swapchain recreated (%ux%u)", g.scext.width, g.scext.height);
        }
    }
    bool have = false;
    Vk::Slot& sl = g.slots[g.slot];   // emplacement de l'enregistrement en cours (begin_frame ci-dessus)
    if (g.sc && !g.sc_stale) {
        VkResult ar = vkAcquireNextImageKHR(g.dev, g.sc, UINT64_MAX, sl.sem_acq, VK_NULL_HANDLE, &idx);
        have = ar == VK_SUCCESS || ar == VK_SUBOPTIMAL_KHR;
        if (ar == VK_SUBOPTIMAL_KHR || ar == VK_ERROR_OUT_OF_DATE_KHR) g.sc_stale = true;
        static int nwarn = 0;
        if (!have && nwarn++ < 5) HLOG("vk", "display image acquisition failed (%d): frame not shown", (int)ar);
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
            HLOG("vk", "presentation: frame %dx%d → window %dx%d (area %dx%d at %d,%d, %s)", cw, ch, W, H, dw, dh, dx, dy,
                 exact ? "pixel for pixel" : "linear scaling");
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
    const uint64_t serial = submit_cb("presentation", have ? sl.sem_acq : VK_NULL_HANDLE, done_sem);
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
    if (!g.async || g.prof || g.dbgptr) wait_serial(serial, "presentation");
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
            HLOG("dbg", "frame %llu: %u passes, %u recorded, %zu distinct values:%s", (unsigned long long)frame, g.dbgptr[0],
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
            const char* nm[6] = {"interval", "lock", "acquire", "submission", "presentation", "GPU wait"};
            std::string o;
            for (int i = 0; i < 6; i++) {
                char b[96];
                snprintf(b, sizeof b, " %s %.2f/%.2f", nm[i], g_perf.sum[i] / 120, g_perf.mx[i]);
                o += b;
            }
            HLOG("perf", "ms avg/max:%s | %llu draws | flushes %.2f ms/frame (%llu) | GPU waits %.2f ms/frame, ring %.1f MB at most",
                 o.c_str(), (unsigned long long)g.frame_draws, g_flush_us.exchange(0) / 120e3, (unsigned long long)g_flush_n.exchange(0),
                 g_wait_us.exchange(0) / 120e3, g.ring_peak / 1048576.0);
            g.ring_peak = 0;
            HLOG("perf", "draw: %.1f calls/frame, ms/frame: prog %.3f textures %.3f pass %.3f pipeline %.3f descriptors %.3f end %.3f",
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
    static const long taudit_every = getenv("RECOMP_TEX_AUDIT") ? atol(getenv("RECOMP_TEX_AUDIT")) : 0;
    static const long audit_from = getenv("RECOMP_TEX_AUDIT_FROM") ? atol(getenv("RECOMP_TEX_AUDIT_FROM")) : 0;
    if (taudit_every > 0 && (long)frame >= audit_from && frame % (uint64_t)taudit_every == 0) tex_audit(frame, (uint64_t)taudit_every);
    static const long tex3d_frame = getenv("RECOMP_TEX3D_DUMP") ? atol(getenv("RECOMP_TEX3D_DUMP")) : -1;
    if ((long)frame == tex3d_frame) tex3d_dump(frame);
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
    // Images sans objet : gardées pour être reprises sans nouvel envoi (mesuré le 2026-11-11 sur la cinématique
    // d'ouverture : 1,3 Go de textures reprises en 100 s avec un plafond de 1,5 Go ; à 256 Mo, 14 % d'envois en plus
    // mais 1 Go de mémoire vidéo et de mémoire engagée en moins). Le plafond suit donc la mémoire du PC, relue une
    // fois par seconde : 1,5 Go tant qu'il y a de la marge ; 256 Mo sous 5 Go validables libres ou quand la mémoire
    // vidéo dépasse 85 % de son budget ; rien sous 3 Go, et les fenêtres d'import inutilisées depuis 2 s sont rendues.
    // memory_saver = false : plafond fixe, comme en 1.0.2.
    if (frame % 60 == 0) {
        int niveau = 0;
        if (settings().memory_saver) {
            const double libre = memoire_validable_libre_go();
            uint64_t used = 0, budget = 0;
            const bool video_pleine = vram_usage(&used, &budget) && used > budget / 100 * 85;
            niveau = libre < 3.0 ? 2 : libre < 5.0 || video_pleine ? 1 : 0;
            static int niveau_vu = 0;
            if (niveau != niveau_vu) {
                HLOG("vk", "orphaned-image cache: %s (%.1f GB available commit, video memory %.0f / %.0f MB)",
                     niveau == 2 ? "emptied, unused import windows released" : niveau == 1 ? "capped at 256 MB" : "full size",
                     libre, used / 1048576.0, budget / 1048576.0);
                niveau_vu = niveau;
            }
        }
        evict_unreferenced(niveau == 2 ? 0 : niveau == 1 ? std::min<uint64_t>(UNREF_CAP, 256ull << 20) : UNREF_CAP);
        if (niveau == 2)
            while (evict_import(120)) {}
    }
    // Windows a refusé de la mémoire au jeu (memory.cpp, mem_pression) : les images sans objet et les fenêtres d'import
    // qui ne servent pas à l'image en cours sont rendues tout de suite, pendant que le fil fautif réessaie.
    static int pression_vue = 0;
    if (const int pr = mem_pression(); pr != pression_vue) {
        const bool premier = !pression_vue;
        pression_vue = pr;
        const uint64_t avant = g.unref_bytes, imp = g.imported;
        evict_unreferenced(0);
        while (evict_import(2)) {}
        if (premier)
            HLOG("vk", "memory pressure: %.0f MB of orphaned images and %.0f MB of import windows released",
                 (avant - g.unref_bytes) / 1048576.0, (imp - g.imported) / 1048576.0);
    }
    if (frame % 600 == 0) pcache_save_later();   // pipelines créés depuis la dernière sauvegarde (toutes les 10 min au plus)
    if (frame == 1 || frame % 600 == 0)
        HLOG("vk", "frame %llu: %llu draws, %llu skipped, %zu pipelines, %zu missing shaders, %llu textures uploaded (%.0f MB, of which %llu re-uploads), %zu images (%llu freed, %.0f MB orphaned, %llu orphans reused = %.0f MB)%s",
             (unsigned long long)frame, (unsigned long long)g.frame_draws, (unsigned long long)g.frame_skipped,
             g.pipelines.size(), g.missing.size(), (unsigned long long)g.tex_uploads, g.tex_upload_bytes / 1048576.0,
             (unsigned long long)g.tex_reuploads, g.images.size(), (unsigned long long)g.images_freed,
             g.unref_bytes / 1048576.0, (unsigned long long)g.unref_reuses, g.unref_reuse_bytes / 1048576.0,
             vram_text().c_str());
    if (frame == 1 || frame % 600 == 0) HLOG("vk", "frame %llu, memory: %s", (unsigned long long)frame, ram_text().c_str());
    if (frame % 1800 == 0) tex_release_log(frame);
    if (g.libs_sur_demande && frame % 1800 == 0)
        HLOG("vk", "frame %llu, shader libraries created at first use so far: %llu", (unsigned long long)frame,
             (unsigned long long)g.libs_on_demand);
    // RECOMP_MEM_MAP=1 : carte de la mémoire engagée toutes les 600 images, et dès qu'elle a grossi de 512 Mo depuis la
    // dernière carte (les chargements de niveau prennent 2 Go en quelques secondes : c'est là qu'il faut regarder)
    static const bool carte = getenv("RECOMP_MEM_MAP") != nullptr;
    // RECOMP_MEM_MAP_FIN=n : en plus, le tas par défaut est relevé toutes les 30 images ; s'il a pris n Mo depuis le
    // relevé précédent, les piles d'appels et le contenu du tas sont écrits (petites compilations du pilote : pour
    // comparer deux lancements et savoir si son cache sur disque évite de recompiler).
    static const long fin_mo = getenv("RECOMP_MEM_MAP_FIN") ? atol(getenv("RECOMP_MEM_MAP_FIN")) : 0;
    if (carte && fin_mo > 0 && frame % 30 == 0) {
        static uint64_t avant = 0;
        const uint64_t d = tas_defaut_engage();
        HLOG("mem", "frame %llu, default heap: %.0f MB committed", (unsigned long long)frame, d / 1048576.0);
        if (avant && d > avant + ((uint64_t)fin_mo << 20)) tas_histogramme();
        avant = d;
    }
    if (carte && frame % 20 == 0) {
        static uint64_t vu = 0;
        PROCESS_MEMORY_COUNTERS pmc{};
        pmc.cb = sizeof pmc;
        K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc);
        if (frame % 600 == 0 || pmc.PagefileUsage > vu + (512ull << 20)) {
            vu = pmc.PagefileUsage;
            if (frame % 600) HLOG("vk", "frame %llu, memory: %s", (unsigned long long)frame, ram_text().c_str());
            if (frame % 600)
                HLOG("vk", "frame %llu: %llu textures uploaded (%.0f MB), %zu images (%llu freed, %.0f MB orphaned)%s",
                     (unsigned long long)frame, (unsigned long long)g.tex_uploads, g.tex_upload_bytes / 1048576.0,
                     g.images.size(), (unsigned long long)g.images_freed, g.unref_bytes / 1048576.0, vram_text().c_str());
            mem_map_log(frame);
        }
    }
    g.frame_draws = g.frame_skipped = 0;
    return true;
}

}  // namespace vkr
}  // namespace hos
