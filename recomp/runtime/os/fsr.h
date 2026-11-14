// FSR d'AMD (FSR 4 et FSR 3.1, anticrénelage natif et super résolution) par l'API FSR du SDK FidelityFX 2.4 : voir
// fsr.cpp. Mêmes entrées et même branchement que le DLSS (dlss.h, dlss::Evaluation) ; les fonctions de dlss.h
// passent la main à celles-ci quand le DLSS ne sert pas.
// À inclure après <vulkan/vulkan.h> (VK_NO_PROTOTYPES).
#pragma once
#include "dlss.h"

namespace hos {
namespace fsr {

// Mode demandé dans recomp.ini (fsr) et utilisable : 0 aucun (pas demandé, DLSS prévu sur cette carte, ou fichiers
// d'AMD absents), 1 natif (anticrénelage seul), 2 qualité, 3 équilibré, 4 performances, 5 ultra performances.
int mode_demande();
// Échelle du rendu 3D imposée par le mode (0,6667 qualité … 0,3333 ultra performances), ou 0 : pas de super
// résolution par le FSR. Lue au démarrage par les variables du moteur, avant Vulkan (dlss::echelle_dlss).
double echelle();
// Avant vkCreateDevice : le FSR sera-t-il essayé sur cette partie ? (capacités de shaders à activer, vk_render.cpp)
bool prevu();
// Après vkCreateDevice : charge les bibliothèques d'AMD et choisit la version. Faux si le FSR n'est pas utilisable
// (le jeu garde son anticrénelage).
bool init(VkPhysicalDevice pd, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa);
bool pret();
void couper(const char* raison);
// Comme dlss::evaluer, dlss::menage, dlss::fonction_prete et dlss::description.
bool evaluer(VkCommandBuffer cb, const dlss::Evaluation& e, uint64_t serie);
void menage(uint64_t fini);
bool fonction_prete(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh);
const char* description();

}  // namespace fsr
}  // namespace hos
