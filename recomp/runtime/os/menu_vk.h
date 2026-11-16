// Dear ImGui sur les images de la fenêtre (menu_vk.cpp) : le menu en jeu (menu.h), son compteur d'images et son rappel
// sont dessinés à la taille de la fenêtre, après la copie de l'image du jeu, dans le tampon de commandes de la
// présentation. Rien n'est enregistré tant que rien ne se montre.
// À inclure après <vulkan/vulkan.h> (VK_NO_PROTOTYPES).
#pragma once
#include <cstdint>
#include <vector>

namespace hos {
namespace menu {
namespace vk {

struct Acces {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    uint32_t famille = 0;   // famille de la file
    VkQueue file = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    // soumissions que le GPU peut encore exécuter quand la suivante s'enregistre (emplacements de vk_render.cpp) : ce
    // que le menu abandonne (tampons, textures) n'est détruit qu'après autant de présentations, et deux de plus
    uint32_t en_vol = 1;
};
// Après vkCreateDevice, avant la première swapchain. Ne crée rien : Dear ImGui démarre au premier dessin.
void init(const Acces& a);
// La swapchain vient d'être créée (ses images, leur format) ou va être détruite (liste vide) : vues des images. À
// appeler quand le GPU ne s'en sert plus (vkDeviceWaitIdle), sous le verrou du moteur de rendu.
void images_fenetre(VkFormat format, const std::vector<VkImage>& images);
// À chaque présentation, l'image `index` de la fenêtre (w × h) dans la disposition TRANSFER_DST_OPTIMAL avant et après :
// traite les entrées du menu, et enregistre dans `cb` ce qui est à dessiner par-dessus (rien : aucune commande).
void presenter(VkCommandBuffer cb, uint32_t index, uint32_t w, uint32_t h);

}  // namespace vk
}  // namespace menu
}  // namespace hos
