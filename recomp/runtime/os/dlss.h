// DLSS de NVIDIA (DLAA et super résolution) par le cœur NGX du pilote : voir dlss.cpp. Les fonctions pret, couper,
// evaluer, menage, fonction_prete, description et echelle_dlss passent la main au FSR d'AMD (fsr.cpp) quand c'est lui
// qui sert (recomp.ini fsr, et pas de DLSS sur cette carte).
// À inclure après <vulkan/vulkan.h> (VK_NO_PROTOTYPES).
#pragma once
#include <cstdint>
#include <vector>

namespace hos {
namespace dlss {

// Modèle demandé dans recomp.ini (dlss, dlss_model) : 0 aucun (ou la carte que le jeu prendra n'est pas une NVIDIA
// RTX : réglage gpu), sinon la lettre du préréglage NGX ('K' : DLSS 4, 'M' : DLSS 4.5)
char modele_demande();
// Échelle du rendu 3D imposée par le mode DLSS (0,6667 qualité … 0,3333 ultra performances), ou 0 : pas de super
// résolution demandée, ou le DLSS ne pourra sans doute pas servir (carte qui n'est pas une NVIDIA RTX, pas de NGX, pas
// de nvngx_dlss.dll). Lue au démarrage par les variables du moteur (svc_fs.cpp), avant que Vulkan et NGX ne soient
// prêts.
double echelle_dlss();
// Avant vkCreateInstance : ajoute à `ext` les extensions d'instance que NGX demande et que le chargeur offre (`dispo`).
void extensions_instance(std::vector<const char*>& ext, const std::vector<VkExtensionProperties>& dispo);
// Avant vkCreateDevice (carte choisie) : ajoute à `ext` les extensions de périphérique demandées par NGX, toutes ou
// aucune (le DLSS reste alors coupé).
void extensions_peripherique(VkInstance inst, VkPhysicalDevice pd, const VkPhysicalDeviceProperties& pr,
                             const std::vector<VkExtensionProperties>& dispo, std::vector<const char*>& ext);
// Après vkCreateDevice : prépare NGX. Faux si le DLSS n'est pas utilisable (le jeu garde son anticrénelage).
bool init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
          PFN_vkGetDeviceProcAddr gdpa);
bool pret();
void couper(const char* raison);   // DLSS abandonné pour la partie (échecs répétés)

struct Image {   // vue passée à NGX
    VkImage image = VK_NULL_HANDLE;
    VkImageView vue = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;   // aspect de la vue (profondeur seule pour D32F_S8)
    uint32_t w = 0, h = 0;
};
struct Evaluation {
    // Entrées dans la disposition VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, sortie dans VK_IMAGE_LAYOUT_GENERAL
    // (avec VK_IMAGE_USAGE_STORAGE_BIT), comme NGX le demande.
    Image couleur, profondeur, vecteurs, sortie;
    uint32_t rw = 0, rh = 0;     // taille rendue (coin haut gauche des entrées)
    float gigue[2] = {0, 0};     // décalage de la projection en pixels rendus (x vers la droite, y vers le bas)
    float echelle_mv[2] = {1, 1};
    bool reset = false;          // historique du DLSS à oublier (changement de scène)
    // Pour le FSR d'AMD (fsr.cpp) : plans proche et lointain et angle de vue vertical (radians) de la caméra, tirés de
    // la matrice de la vue (0 : inconnus ; lointain infini possible) ; compression de la couleur d'entrée
    // (0 linéaire, 1 x / (1 + x), 2 racine carrée)
    float proche = 0, loin = 0, fov = 0;
    int compression = 0;
};
// Enregistre l'évaluation dans `cb`, après avoir créé la fonction DLSS pour ces tailles si besoin. Une fonction
// d'autres tailles qui existait est mise de côté : `serie` est le numéro de la soumission qui contiendra `cb` (elle
// peut encore servir dans des commandes enregistrées mais pas encore exécutées) ; `menage` la libère quand le GPU a
// fini cette soumission. Faux en cas d'échec (rien d'enregistré d'utile : le jeu garde alors son anticrénelage pour
// cette image).
bool evaluer(VkCommandBuffer cb, const Evaluation& e, uint64_t serie);
// Libère les fonctions mises de côté dont la soumission est finie (`fini` : dernière soumission terminée par le GPU).
void menage(uint64_t fini);
// Vrai si la fonction DLSS de ces tailles existe déjà (sinon `evaluer` la crée : quelques centaines de ms).
bool fonction_prete(uint32_t rw, uint32_t rh, uint32_t ow, uint32_t oh);
// Description pour le journal (« DLSS 4, modèle K, DLAA 2560x1440 »)
const char* description();
// Le DLSS de NVIDIA servira sans doute sur cette partie (demandé, carte NVIDIA RTX, cœur NGX trouvé) : le FSR d'AMD
// (fsr.cpp), demandé en même temps, lui laisse alors la place.
bool dlss_prevu();
// « DLSS » ou « FSR » : la technique en service (journal)
const char* technique();

}  // namespace dlss
}  // namespace hos
