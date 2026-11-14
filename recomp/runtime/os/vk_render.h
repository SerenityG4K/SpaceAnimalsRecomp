// Interface du moteur de rendu Vulkan (vk_render.cpp) pour l'exécuteur de commandes NVN.
#pragma once
#include "nvn_exec.h"

#include <functional>
#include <string>

namespace hos {
namespace vkr {

struct DrawCall {
    enum Kind { Arrays, Elements, ClearColor, ClearDepth } kind = Arrays;
    const nvn::GfxState* st = nullptr;
    uint64_t prim = 4;
    uint32_t first = 0, count = 0, instances = 1, base_instance = 0;
    int32_t base_vertex = 0;
    uint32_t index_type = 1;
    uint64_t index_addr = 0;
    // effacements
    int index = 0;
    float color[4] = {0, 0, 0, 0};
    float depth = 1.0f;
    int stencil = 0;
    bool depth_mask = true;
    uint32_t stencil_mask = 0;
};

bool available();
void ensure_init();
// Carte graphique que le jeu prendra (même choix que l'initialisation de Vulkan : recomp.ini gpu, sinon la carte dédiée
// qui a le plus de mémoire vidéo), connue avant celle-ci : son nom et l'identifiant de son fabricant (0x10DE : NVIDIA).
// Faux si Vulkan ne répond pas. Crée une instance Vulkan temporaire : pour le DLSS (dlss.cpp), pas à chaque image.
bool carte_prevue(std::string* nom, uint32_t* fabricant);
// recomp_main --precompile : compile tous les shaders du cache pour ce GPU (pipelines.cache), sans fenêtre ni jeu ;
// avancement sur la sortie standard (@progres, @info, @saute, @erreur). Rend le code de sortie du processus.
int precompile();
void save_caches();   // écrit pipelines.cache (fermeture du jeu)
// Le jeu a libéré la mémoire [addr, addr+size) (nvnMemoryPoolFinalize) : les imports Vulkan qui la couvrent
// sont retirés, et réimportés au prochain usage de cette plage (la mémoire peut être remappée ailleurs)
void pools_released(uint64_t addr, uint64_t size);
// nvnMemoryPoolFlushMappedRange : le jeu a écrit [addr, addr+size). Appelable de n'importe quel fil. Les textures
// packagées dont la source touche cette plage sont comparées à leur dernier envoi au prochain usage, et renvoyées
// au GPU si elles ont changé.
void pool_flushed(uint64_t addr, uint64_t size);
// nvnTextureFinalize : l'image de cet objet texture perd une référence (détruite plus tard si plus personne ne
// l'utilise, au-delà d'un cache de réemploi plafonné)
void texture_released(uint64_t texkey);
void draw(const DrawCall& d);
void barrier();
void flush();   // soumet le travail enregistré et attend que le GPU ait tout fini (nvnQueueFinish)
// Soumet le travail enregistré sans attendre le GPU (FenceSync) ; rend la valeur chronologique qui couvre
// tout le travail soumis jusque-là (0 : rien n'a jamais été soumis)
uint64_t flush_async();
// fn appelée par le fil de complétion quand le GPU a atteint `serial` (tout de suite si c'est déjà fait)
void when_done(uint64_t serial, std::function<void()> fn);
void dispatch(const nvn::GfxState& s, uint32_t gx, uint32_t gy, uint32_t gz);
bool present(uint64_t texkey, uint64_t frame, const int32_t* crop = nullptr);   // crop : x, y, l, h (l = 0 : tout)   // false : Vulkan indisponible (présentation GDI)
// Diagnostic : copie la texture (couleur) dans un PNG rt_<image>_<n>_<étiquette>.png (moitié de
// taille, converti en RGBA8 par blit), écrit après la prochaine attente du GPU.
void capture(uint64_t texkey, uint64_t frame, const char* label);
void gpu_peek(uint64_t addr, uint64_t size, const char* path);   // diagnostic : mémoire vue par le GPU
// nvnCommandBufferCopyBufferToTexture : données serrées à l'adresse src (mémoire invitée) vers la
// région {x, y, z, l, h, p} du niveau `level` (z / p = couches pour les tableaux)
void copy_buffer_to_texture(uint64_t src, uint64_t texkey, uint32_t level, uint32_t layer, const int32_t* region);
// nvnCommandBufferClearTexture : région {x, y, z, l, h, p} du niveau `level` à la couleur donnée
void clear_texture(uint64_t texkey, uint32_t level, uint32_t layer, const int32_t* region, const float* color);
// nvnCommandBufferCopyBufferToBuffer sur le GPU (pools importés) ; false : à faire par le processeur
bool copy_buffer(uint64_t src, uint64_t dst, uint64_t size);
// nvnCommandBufferCopyTextureToTexture : copie (même taille) ou blit (mise à l'échelle, drapeau 1 = filtre linéaire)
void copy_texture_to_texture(uint64_t srckey, uint32_t slevel, uint32_t slayer, const int32_t* sregion, uint64_t dstkey,
                             uint32_t dlevel, uint32_t dlayer, const int32_t* dregion, uint32_t flags);

}  // namespace vkr
}  // namespace hos
