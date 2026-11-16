// Dear ImGui (third_party/imgui) sur les images de la fenêtre : voir menu_vk.h. Le menu (menu.cpp) dessine sur les listes
// de Dear ImGui ; ce fichier les rend avec le backend Vulkan de Dear ImGui, sur le périphérique du moteur de rendu
// (vk_render.cpp), dans le tampon de commandes de la présentation : aucune soumission ni attente de plus.
//
// D'après gpu/vk/vk_imgui.cpp de Longshot (MagicBeanDev, GPLv3 ; voir menu.h), adapté à ce moteur :
//  * les textures (l'atlas des lettres) sont créées et mises à jour dans le tampon de commandes de la présentation,
//    avant sa passe de rendu ; l'envoi du backend (une soumission et vkQueueWaitIdle) ne sert jamais, une lettre
//    nouvelle ne fait donc pas attendre le jeu ;
//  * ce qui est abandonné (tampons de transit, anciennes textures) est détruit quelques présentations plus tard, quand
//    le GPU a fini les soumissions qui s'en servaient ;
//  * images de fenêtre en sRGB (le format que ce moteur choisit) : les couleurs du menu sont données en sRGB, le
//    shader de fragments ci-dessous les passe en lumière linéaire, que l'image recode.
// Polices : Bahnschrift, celle du launcher (fournie avec Windows 10 et 11), sinon Segoe UI, sinon la police vectorielle
// de Dear ImGui. RECOMP_MENU_FONT=<fichier .ttf> impose une police.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include "menu.h"
#include "menu_ui.h"
#include "menu_vk.h"

#include "backends/imgui_impl_vulkan.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

// Vérification de Dear ImGui en échec (os/menu_imconfig.h, IM_ASSERT) : dans le journal, les premières seulement
extern "C" void hos_menu_assert(const char* expr, const char* fichier, int ligne) {
    static int n = 0;
    if (n++ < 12) hos::logf("menu", "Dear ImGui check failed: %s (%s:%d)", expr, fichier, ligne);
}

namespace hos {
namespace menu {
namespace vk {
namespace {

// ---------------------------------------------------------------- fonctions Vulkan (chargées à l'exécution)
#define FN_INSTANCE(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceMemoryProperties)
#define FN_APPAREIL(X)                                                                                              \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)        \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateBuffer) X(vkDestroyBuffer)          \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) X(vkCmdPipelineBarrier2)                \
    X(vkCmdCopyBufferToImage) X(vkCmdBeginRendering) X(vkCmdEndRendering)
#define DECL(n) PFN_##n n = nullptr;
FN_INSTANCE(DECL) FN_APPAREIL(DECL)
#undef DECL

// backends/vulkan/glsl_shader.frag de Dear ImGui, avec le passage du sRGB à la lumière linéaire. Compilé par
//   glslang -V -x -o menu_srgb.frag.u32 menu_srgb.frag      (glslang 16.6.0)
/*
#version 450 core
layout(location = 0) out vec4 fColor;
layout(set=0, binding=0) uniform texture2D _Texture;
layout(set=1, binding=0) uniform sampler _Sampler;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;
void main()
{
    vec4 c = In.Color * texture(sampler2D(_Texture, _Sampler), In.UV.st);
    vec3 bas = c.rgb / 12.92;
    vec3 haut = pow((c.rgb + 0.055) / 1.055, vec3(2.4));
    fColor = vec4(mix(bas, haut, step(vec3(0.04045), c.rgb)), c.a);
}
*/
const uint32_t kFragSrgb[] = {
    0x07230203,0x00010000,0x0008000b,0x0000004a,0x00000000,0x00020011,0x00000001,0x0006000b,
    0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
    0x0007000f,0x00000004,0x00000004,0x6e69616d,0x00000000,0x0000000d,0x00000038,0x00030010,
    0x00000004,0x00000007,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,0x6e69616d,
    0x00000000,0x00030005,0x00000009,0x00000063,0x00030005,0x0000000b,0x00000000,0x00050006,
    0x0000000b,0x00000000,0x6f6c6f43,0x00000072,0x00040006,0x0000000b,0x00000001,0x00005655,
    0x00030005,0x0000000d,0x00006e49,0x00050005,0x00000015,0x7865545f,0x65727574,0x00000000,
    0x00050005,0x00000019,0x6d61535f,0x72656c70,0x00000000,0x00030005,0x00000025,0x00736162,
    0x00040005,0x0000002b,0x74756168,0x00000000,0x00040005,0x00000038,0x6c6f4366,0x0000726f,
    0x00040047,0x0000000d,0x0000001e,0x00000000,0x00040047,0x00000015,0x00000021,0x00000000,
    0x00040047,0x00000015,0x00000022,0x00000000,0x00040047,0x00000019,0x00000021,0x00000000,
    0x00040047,0x00000019,0x00000022,0x00000001,0x00040047,0x00000038,0x0000001e,0x00000000,
    0x00020013,0x00000002,0x00030021,0x00000003,0x00000002,0x00030016,0x00000006,0x00000020,
    0x00040017,0x00000007,0x00000006,0x00000004,0x00040020,0x00000008,0x00000007,0x00000007,
    0x00040017,0x0000000a,0x00000006,0x00000002,0x0004001e,0x0000000b,0x00000007,0x0000000a,
    0x00040020,0x0000000c,0x00000001,0x0000000b,0x0004003b,0x0000000c,0x0000000d,0x00000001,
    0x00040015,0x0000000e,0x00000020,0x00000001,0x0004002b,0x0000000e,0x0000000f,0x00000000,
    0x00040020,0x00000010,0x00000001,0x00000007,0x00090019,0x00000013,0x00000006,0x00000001,
    0x00000000,0x00000000,0x00000000,0x00000001,0x00000000,0x00040020,0x00000014,0x00000000,
    0x00000013,0x0004003b,0x00000014,0x00000015,0x00000000,0x0002001a,0x00000017,0x00040020,
    0x00000018,0x00000000,0x00000017,0x0004003b,0x00000018,0x00000019,0x00000000,0x0003001b,
    0x0000001b,0x00000013,0x0004002b,0x0000000e,0x0000001d,0x00000001,0x00040020,0x0000001e,
    0x00000001,0x0000000a,0x00040017,0x00000023,0x00000006,0x00000003,0x00040020,0x00000024,
    0x00000007,0x00000023,0x0004002b,0x00000006,0x00000028,0x414eb852,0x0004002b,0x00000006,
    0x0000002e,0x3d6147ae,0x0004002b,0x00000006,0x00000031,0x3f870a3d,0x0004002b,0x00000006,
    0x00000034,0x4019999a,0x0006002c,0x00000023,0x00000035,0x00000034,0x00000034,0x00000034,
    0x00040020,0x00000037,0x00000003,0x00000007,0x0004003b,0x00000037,0x00000038,0x00000003,
    0x0004002b,0x00000006,0x0000003b,0x3d25aee6,0x0006002c,0x00000023,0x0000003c,0x0000003b,
    0x0000003b,0x0000003b,0x00040015,0x00000041,0x00000020,0x00000000,0x0004002b,0x00000041,
    0x00000042,0x00000003,0x00040020,0x00000043,0x00000007,0x00000006,0x00050036,0x00000002,
    0x00000004,0x00000000,0x00000003,0x000200f8,0x00000005,0x0004003b,0x00000008,0x00000009,
    0x00000007,0x0004003b,0x00000024,0x00000025,0x00000007,0x0004003b,0x00000024,0x0000002b,
    0x00000007,0x00050041,0x00000010,0x00000011,0x0000000d,0x0000000f,0x0004003d,0x00000007,
    0x00000012,0x00000011,0x0004003d,0x00000013,0x00000016,0x00000015,0x0004003d,0x00000017,
    0x0000001a,0x00000019,0x00050056,0x0000001b,0x0000001c,0x00000016,0x0000001a,0x00050041,
    0x0000001e,0x0000001f,0x0000000d,0x0000001d,0x0004003d,0x0000000a,0x00000020,0x0000001f,
    0x00050057,0x00000007,0x00000021,0x0000001c,0x00000020,0x00050085,0x00000007,0x00000022,
    0x00000012,0x00000021,0x0003003e,0x00000009,0x00000022,0x0004003d,0x00000007,0x00000026,
    0x00000009,0x0008004f,0x00000023,0x00000027,0x00000026,0x00000026,0x00000000,0x00000001,
    0x00000002,0x00060050,0x00000023,0x00000029,0x00000028,0x00000028,0x00000028,0x00050088,
    0x00000023,0x0000002a,0x00000027,0x00000029,0x0003003e,0x00000025,0x0000002a,0x0004003d,
    0x00000007,0x0000002c,0x00000009,0x0008004f,0x00000023,0x0000002d,0x0000002c,0x0000002c,
    0x00000000,0x00000001,0x00000002,0x00060050,0x00000023,0x0000002f,0x0000002e,0x0000002e,
    0x0000002e,0x00050081,0x00000023,0x00000030,0x0000002d,0x0000002f,0x00060050,0x00000023,
    0x00000032,0x00000031,0x00000031,0x00000031,0x00050088,0x00000023,0x00000033,0x00000030,
    0x00000032,0x0007000c,0x00000023,0x00000036,0x00000001,0x0000001a,0x00000033,0x00000035,
    0x0003003e,0x0000002b,0x00000036,0x0004003d,0x00000023,0x00000039,0x00000025,0x0004003d,
    0x00000023,0x0000003a,0x0000002b,0x0004003d,0x00000007,0x0000003d,0x00000009,0x0008004f,
    0x00000023,0x0000003e,0x0000003d,0x0000003d,0x00000000,0x00000001,0x00000002,0x0007000c,
    0x00000023,0x0000003f,0x00000001,0x00000030,0x0000003c,0x0000003e,0x0008000c,0x00000023,
    0x00000040,0x00000001,0x0000002e,0x00000039,0x0000003a,0x0000003f,0x00050041,0x00000043,
    0x00000044,0x00000009,0x00000042,0x0004003d,0x00000006,0x00000045,0x00000044,0x00050051,
    0x00000006,0x00000046,0x00000040,0x00000000,0x00050051,0x00000006,0x00000047,0x00000040,
    0x00000001,0x00050051,0x00000006,0x00000048,0x00000040,0x00000002,0x00070050,0x00000007,
    0x00000049,0x00000046,0x00000047,0x00000048,0x00000045,0x0003003e,0x00000038,0x00000049,
    0x000100fd,0x00010038,
};

// ---------------------------------------------------------------- état
Acces g;
bool g_acces = false;                    // init a réussi
bool g_essaye = false, g_pret = false;   // Dear ImGui a démarré
VkPhysicalDeviceMemoryProperties g_mem{};
VkFormat g_format = VK_FORMAT_UNDEFINED;            // format des images de la fenêtre
VkFormat g_format_pipeline = VK_FORMAT_UNDEFINED;   // format pour lequel le pipeline du backend est fait
bool g_srgb = false;                                // ce format est sRGB : shader kFragSrgb
std::vector<VkImage> g_images;
std::vector<VkImageView> g_vues;
ui::Polices g_polices;
int64_t g_presentations = 0;   // présentations enregistrées : quand un objet abandonné peut être détruit
std::vector<std::string> g_cartes;
std::string g_carte;

bool est_srgb(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}
PFN_vkVoidFunction charger(const char* nom, void*) {
    if (PFN_vkVoidFunction f = g.gdpa(g.dev, nom)) return f;
    if (PFN_vkVoidFunction f = g.gipa(g.inst, nom)) return f;
    logf("menu", "Dear ImGui: Vulkan function %s not found", nom);   // (le backend s'arrête à la première qui manque)
    return nullptr;
}
void resultat_vk(VkResult r) {
    static int n = 0;
    if (r < 0 && n++ < 8) logf("menu", "Dear ImGui: Vulkan error %d", (int)r);
}
uint32_t type_memoire(uint32_t bits, VkMemoryPropertyFlags voulu) {
    for (uint32_t i = 0; i < g_mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (g_mem.memoryTypes[i].propertyFlags & voulu) == voulu) return i;
    return UINT32_MAX;
}

// ---------------------------------------------------------------- textures et objets abandonnés
struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView vue = VK_NULL_HANDLE;
    VkDescriptorSet jeu = VK_NULL_HANDLE;
};
struct Mort {   // plus utilisé par rien d'enregistré après `a` : détruit quand les soumissions en vol alors sont finies
    int64_t a;
    Tex tex;
    VkBuffer tampon = VK_NULL_HANDLE;
    VkDeviceMemory tampon_mem = VK_NULL_HANDLE;
};
std::vector<Mort> g_morts;
void detruire(const Mort& m) {
    if (m.tex.jeu) ImGui_ImplVulkan_RemoveTexture(m.tex.jeu);
    if (m.tex.vue) vkDestroyImageView(g.dev, m.tex.vue, nullptr);
    if (m.tex.image) vkDestroyImage(g.dev, m.tex.image, nullptr);
    if (m.tex.mem) vkFreeMemory(g.dev, m.tex.mem, nullptr);
    if (m.tampon) vkDestroyBuffer(g.dev, m.tampon, nullptr);
    if (m.tampon_mem) vkFreeMemory(g.dev, m.tampon_mem, nullptr);
}
void faucher() {
    const int64_t plus_tard = (int64_t)g.en_vol + 2;
    for (size_t i = 0; i < g_morts.size();) {
        if (g_presentations - g_morts[i].a < plus_tard) {
            i++;
            continue;
        }
        detruire(g_morts[i]);
        g_morts[i] = g_morts.back();
        g_morts.pop_back();
    }
}

bool creer_tampon(uint64_t taille, VkBuffer* tampon, VkDeviceMemory* mem, void** adresse) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = taille;
    ci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(g.dev, &ci, nullptr, tampon) != VK_SUCCESS) return false;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, *tampon, &mr);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(g.dev, &ai, nullptr, mem) != VK_SUCCESS) {
        vkDestroyBuffer(g.dev, *tampon, nullptr);
        *tampon = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(g.dev, *tampon, *mem, 0) != VK_SUCCESS || vkMapMemory(g.dev, *mem, 0, VK_WHOLE_SIZE, 0, adresse) != VK_SUCCESS) {
        vkDestroyBuffer(g.dev, *tampon, nullptr);
        vkFreeMemory(g.dev, *mem, nullptr);
        *tampon = VK_NULL_HANDLE, *mem = VK_NULL_HANDLE;
        return false;
    }
    return true;
}
bool creer_texture(uint32_t w, uint32_t h, Tex* t) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    // les lettres sont blanches, leur couverture dans l'alpha : pareilles en sRGB et en lumière linéaire
    ci.format = VK_FORMAT_R8G8B8A8_UNORM;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool ok = vkCreateImage(g.dev, &ci, nullptr, &t->image) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(g.dev, t->image, &mr);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = mr.size;
        ai.memoryTypeIndex = type_memoire(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = type_memoire(mr.memoryTypeBits, 0);
        ok = ai.memoryTypeIndex != UINT32_MAX && vkAllocateMemory(g.dev, &ai, nullptr, &t->mem) == VK_SUCCESS &&
             vkBindImageMemory(g.dev, t->image, t->mem, 0) == VK_SUCCESS;
    }
    if (ok) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ci.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        ok = vkCreateImageView(g.dev, &vi, nullptr, &t->vue) == VK_SUCCESS;
    }
    if (ok) ok = (t->jeu = ImGui_ImplVulkan_AddTexture(t->vue, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)) != VK_NULL_HANDLE;
    if (!ok) {   // rien d'enregistré ne s'en sert encore
        Mort m{0, *t};
        detruire(m);
        *t = Tex{};
    }
    return ok;
}

void disposition(VkCommandBuffer cb, VkImage image, VkImageLayout de, VkImageLayout vers, VkPipelineStageFlags2 etage_avant,
                 VkAccessFlags2 acces_avant, VkPipelineStageFlags2 etage_apres, VkAccessFlags2 acces_apres) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = etage_avant, b.srcAccessMask = acces_avant;
    b.dstStageMask = etage_apres, b.dstAccessMask = acces_apres;
    b.oldLayout = de, b.newLayout = vers;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo d{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    d.imageMemoryBarrierCount = 1, d.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &d);
}

// pixels de tex dans le rectangle (x, y, w, h), en RGBA, vers dst
void pixels(ImTextureData* tex, int x, int y, int w, int h, unsigned char* dst) {
    for (int ligne = 0; ligne < h; ligne++) {
        const unsigned char* src = (const unsigned char*)tex->GetPixelsAt(x, y + ligne);
        if (tex->Format == ImTextureFormat_RGBA32) {
            memcpy(dst + (size_t)ligne * w * 4, src, (size_t)w * 4);
        } else {   // Alpha8 : blanc, la couverture dans l'alpha
            for (int i = 0; i < w; i++) {
                unsigned char* p = dst + ((size_t)ligne * w + i) * 4;
                p[0] = p[1] = p[2] = 255, p[3] = src[i];
            }
        }
    }
}

// Une texture que Dear ImGui veut créer, mettre à jour ou détruire : fait dans `cb`, avant la passe de rendu
void texture(VkCommandBuffer cb, ImTextureData* tex) {
    if (tex->Status == ImTextureStatus_WantDestroy) {
        if (Tex* t = (Tex*)tex->BackendUserData) {
            g_morts.push_back({g_presentations, *t});
            IM_DELETE(t);
        }
        tex->SetTexID(ImTextureID_Invalid);
        tex->BackendUserData = nullptr;
        tex->SetStatus(ImTextureStatus_Destroyed);
        return;
    }
    const bool creation = tex->Status == ImTextureStatus_WantCreate;
    if (!creation && !tex->BackendUserData) return;
    // à copier : tout pour une création, sinon les mises à jour en attente
    ImVector<ImTextureRect> rects;
    if (creation) rects.push_back({0, 0, (unsigned short)tex->Width, (unsigned short)tex->Height});
    else rects = tex->Updates;
    if (rects.empty()) rects.push_back(tex->UpdateRect);
    uint64_t taille = 0;
    for (const ImTextureRect& r : rects) taille += (uint64_t)r.w * r.h * 4;
    // le tampon de transit d'abord, puis l'image : un échec ne laisse rien, et la texture est redemandée
    VkBuffer tampon = VK_NULL_HANDLE;
    VkDeviceMemory tampon_mem = VK_NULL_HANDLE;
    void* adresse = nullptr;
    if (!taille || !creer_tampon(taille, &tampon, &tampon_mem, &adresse)) return;
    if (creation) {
        Tex faite;
        if (!creer_texture((uint32_t)tex->Width, (uint32_t)tex->Height, &faite)) {
            Mort m{0, Tex{}, tampon, tampon_mem};
            detruire(m);
            return;
        }
        Tex* t = IM_NEW(Tex)();
        *t = faite;
        tex->BackendUserData = t;
        tex->SetTexID((ImTextureID)t->jeu);
    }
    Tex* t = (Tex*)tex->BackendUserData;
    std::vector<VkBufferImageCopy> copies;
    uint64_t ou = 0;
    for (const ImTextureRect& r : rects) {
        pixels(tex, r.x, r.y, r.w, r.h, (unsigned char*)adresse + ou);
        VkBufferImageCopy c{};
        c.bufferOffset = ou;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = {r.x, r.y, 0};
        c.imageExtent = {r.w, r.h, 1};
        copies.push_back(c);
        ou += (uint64_t)r.w * r.h * 4;
    }
    disposition(cb, t->image, creation ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
    vkCmdCopyBufferToImage(cb, tampon, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, (uint32_t)copies.size(), copies.data());
    disposition(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    g_morts.push_back({g_presentations, Tex{}, tampon, tampon_mem});   // le tampon de transit : après cette image
    tex->SetStatus(ImTextureStatus_OK);
}

// ---------------------------------------------------------------- polices
ImFont* police_fichier(const std::string& chemin, const char* nom) {
    FILE* f = fopen(chemin.c_str(), "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    const long taille = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (taille < 12 || taille > (64 << 20)) {
        fclose(f);
        return nullptr;
    }
    void* octets = IM_ALLOC((size_t)taille);   // l'atlas en devient propriétaire
    const bool lu = fread(octets, 1, (size_t)taille, f) == (size_t)taille;
    fclose(f);
    if (!lu) {
        IM_FREE(octets);
        return nullptr;
    }
    ImFontConfig cfg;
    snprintf(cfg.Name, sizeof cfg.Name, "%s", nom);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(octets, (int)taille, 18.0f, &cfg);
}
void charger_polices() {
    ImFont* texte = nullptr;
    ImFont* gras = nullptr;
    const char* nom = "";
    if (const char* e = getenv("RECOMP_MENU_FONT")) {
        texte = police_fichier(e, "RECOMP_MENU_FONT");
        if (texte) nom = e;
    }
#ifdef _WIN32
    const char* win = getenv("WINDIR");
    const std::string dossier = std::string(win && *win ? win : "C:\\Windows") + "\\Fonts\\";
    if (!texte && (texte = police_fichier(dossier + "bahnschrift.ttf", "Bahnschrift"))) nom = "Bahnschrift";
    if (!texte && (texte = police_fichier(dossier + "segoeui.ttf", "Segoe UI"))) {
        nom = "Segoe UI";
        gras = police_fichier(dossier + "seguisb.ttf", "Segoe UI Semibold");
        if (!gras) gras = police_fichier(dossier + "segoeuib.ttf", "Segoe UI Bold");
    }
#endif
    if (!texte) {
        texte = ImGui::GetIO().Fonts->AddFontDefaultVector();
        nom = "Dear ImGui's own font";
    }
    if (!gras) gras = texte;
    g_polices = {texte, gras, gras};
    logf("menu", "menu font: %s", nom);
}

bool demarrer() {
    g_essaye = true;
    if (g_vues.empty() || !g_vues[0]) {
        logf("menu", "no view of the window's images: no in-game menu");
        return false;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.Fonts->TexDesiredFormat = ImTextureFormat_RGBA32;
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_3, charger)) {
        logf("menu", "Dear ImGui: the Vulkan functions did not load: no in-game menu");
        return false;
    }
    g_format_pipeline = g_format;
    g_srgb = est_srgb(g_format);
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = g.inst;
    ii.PhysicalDevice = g.pd;
    ii.Device = g.dev;
    ii.QueueFamily = g.famille;
    ii.Queue = g.file;              // pour les envois du backend, qui ne servent jamais (texture ci-dessus)
    ii.DescriptorPoolSize = 16;     // son propre pool : les jeux de descripteurs des textures
    ii.MinImageCount = 2;
    ii.ImageCount = g.en_vol + 2;   // ses tampons de sommets, à tour de rôle : jamais un que le GPU lit encore
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_format_pipeline;
    ii.CheckVkResultFn = resultat_vk;
    if (g_srgb) {
        ii.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        ii.CustomShaderFragCreateInfo.codeSize = sizeof kFragSrgb;
        ii.CustomShaderFragCreateInfo.pCode = kFragSrgb;
    }
    if (!ImGui_ImplVulkan_Init(&ii)) {
        logf("menu", "Dear ImGui: the Vulkan backend did not start: no in-game menu");
        return false;
    }
    charger_polices();
    logf("menu", "Dear ImGui %s on the window's images (format %d%s)", IMGUI_VERSION, (int)g_format_pipeline,
         g_srgb ? ", sRGB: colours made linear by the shader" : "");
    return true;
}

// Nouvelle image de Dear ImGui pour une fenêtre de w × h ; faux si rien ne peut se dessiner
bool debut(uint32_t w, uint32_t h) {
    if (!g_essaye) g_pret = demarrer();
    if (!g_pret || g_vues.empty()) return false;
    if (g_format != g_format_pipeline) {   // nouvelle swapchain d'un autre format
        if (est_srgb(g_format) != g_srgb) {   // le shader choisi au départ ne convient plus
            logf("menu", "the window's images changed between sRGB and linear formats (%d to %d): in-game menu off",
                 (int)g_format_pipeline, (int)g_format);
            g_pret = false;
            return false;
        }
        g_format_pipeline = g_format;
        ImGui_ImplVulkan_PipelineInfo pi{};
        pi.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        pi.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        pi.PipelineRenderingCreateInfo.pColorAttachmentFormats = &g_format_pipeline;
        ImGui_ImplVulkan_CreateMainPipeline(&pi);
        logf("menu", "the window's images are now format %d", (int)g_format_pipeline);
    }
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2((float)w, (float)h);
    static std::chrono::steady_clock::time_point avant;
    const auto t = std::chrono::steady_clock::now();
    const float dt = std::chrono::duration<float>(t - avant).count();
    io.DeltaTime = avant.time_since_epoch().count() && dt > 0 ? std::min(dt, 0.25f) : 1.0f / 60.0f;
    avant = t;
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    return true;
}

// Le dessin de l'image dans `cb`, par-dessus l'image de fenêtre (TRANSFER_DST_OPTIMAL avant et après) : les textures
// nouvelles d'abord, puis une passe de rendu
void enregistrer(VkCommandBuffer cb, VkImage image, VkImageView vue, uint32_t w, uint32_t h) {
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    g_presentations++;
    faucher();
    if (!image || !vue) return;
    bool textures_ok = true;   // une texture qui n'a pas pu être faite n'a pas de descripteur : rien ne doit l'échantillonner
    if (dd->Textures)
        for (ImTextureData* tex : *dd->Textures) {
            if (tex->Status == ImTextureStatus_OK || tex->Status == ImTextureStatus_Destroyed) continue;
            texture(cb, tex);
            if (tex->Status != ImTextureStatus_OK && tex->Status != ImTextureStatus_Destroyed) textures_ok = false;
        }
    dd->Textures = nullptr;   // fait ci-dessus : l'envoi du backend attendrait la file
    if (!textures_ok) {   // le dessin de cette image est laissé ; la texture est redemandée à la suivante
        static bool dit = false;
        if (!dit) dit = true, logf("menu", "a texture could not be made (out of video memory?): the menu waits for it");
        return;
    }
    disposition(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = vue;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {w, h}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    vkCmdBeginRendering(cb, &ri);
    ImGui_ImplVulkan_RenderDrawData(dd, cb);
    vkCmdEndRendering(cb);
    disposition(cb, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

}  // namespace

void init(const Acces& a) {
    g = a;
    bool ok = g.inst && g.pd && g.dev && g.gipa && g.gdpa;
#define CHARGE_I(n) if (ok && !(n = (PFN_##n)g.gipa(g.inst, #n))) ok = false;
#define CHARGE_A(n) if (ok && !(n = (PFN_##n)g.gdpa(g.dev, #n))) ok = false;
    FN_INSTANCE(CHARGE_I) FN_APPAREIL(CHARGE_A)
#undef CHARGE_I
#undef CHARGE_A
    if (!ok) {
        logf("menu", "Vulkan functions missing: no in-game menu");
        return;
    }
    vkGetPhysicalDeviceMemoryProperties(g.pd, &g_mem);
    // cartes graphiques, pour la ligne « Carte graphique » du menu (le jeu écrit le nom complet dans recomp.ini)
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    if (n) vkEnumeratePhysicalDevices(g.inst, &n, pds.data());
    pds.resize(n);
    g_cartes.clear();
    for (VkPhysicalDevice pd : pds) {
        VkPhysicalDeviceProperties pr{};
        vkGetPhysicalDeviceProperties(pd, &pr);
        if (pd == g.pd) g_carte = pr.deviceName;
        if (std::find(g_cartes.begin(), g_cartes.end(), pr.deviceName) == g_cartes.end()) g_cartes.push_back(pr.deviceName);
    }
    g_acces = true;
}

void images_fenetre(VkFormat format, const std::vector<VkImage>& images) {
    if (!g_acces) return;
    for (VkImageView v : g_vues)
        if (v) vkDestroyImageView(g.dev, v, nullptr);
    g_vues.clear();
    g_images = images;
    g_format = images.empty() ? g_format : format;
    for (VkImage image : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        if (vkCreateImageView(g.dev, &vi, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
        g_vues.push_back(v);
    }
}

void presenter(VkCommandBuffer cb, uint32_t index, uint32_t w, uint32_t h) {
    if (!g_acces || (g_essaye && !g_pret)) return;   // Dear ImGui n'a pas pu démarrer : pas de menu, le jeu garde ses entrées
    if (!menu::a_dessiner()) {   // rien à montrer : ce qui est abandonné est détruit quand même, le moment venu
        if (g_pret) {
            g_presentations++;
            faucher();
        }
        return;
    }
    if (!debut(w, h)) return;
    menu::dessiner((float)w, (float)h);
    const bool a = index < g_vues.size() && g_vues[index];
    enregistrer(cb, a ? g_images[index] : VK_NULL_HANDLE, a ? g_vues[index] : VK_NULL_HANDLE, w, h);
}

}  // namespace vk

const ui::Polices& ui::polices() { return vk::g_polices; }
std::vector<std::string> hote::cartes() { return vk::g_cartes; }
std::string hote::carte() { return vk::g_carte; }

}  // namespace menu
}  // namespace hos
