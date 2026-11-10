// HLE NVN : définitions partagées entre l'enregistrement des commandes (nvn_cmd.cpp) et leur
// exécution (nvn_exec.cpp → moteur de rendu Vulkan).
#pragma once
#include "nvn.h"

#include <memory>

namespace hos {
namespace nvn {

// énumérations NVN utilisées par défaut
enum : uint8_t {
    NVN_BLEND_ZERO = 1, NVN_BLEND_ONE = 2,
    NVN_EQ_ADD = 1,
    NVN_FUNC_NEVER = 1, NVN_FUNC_LESS = 2, NVN_FUNC_ALWAYS = 8,
    NVN_LOGIC_COPY = 3,
};

enum class OpType : uint16_t {
    BlendState, ChannelMaskState, ColorState, DepthStencilState, MultisampleState, PolygonState,
    VertexAttribState, VertexStreamState, StencilValueMask, StencilMask, StencilRef, BlendColor, PointSize,
    LineWidth, PolygonOffset, DepthClamp, PrimitiveRestart, AlphaRef,
    Viewport, Viewports, Scissor, Scissors, DepthRange, DepthRanges,
    RenderTargets, ClearColor, ClearDepthStencil,
    Program, UniformBuffer, UniformBuffers, StorageBuffer, StorageBuffers, Texture, Textures, Image, Images,
    TexturePool, SamplerPool, VertexBuffer, VertexBuffers, PushConstants,
    DrawArrays, DrawElements, DrawArraysInstanced, DrawElementsInstanced, DrawIndirect, Dispatch, DispatchIndirect,
    CopyBufferToTexture, CopyTextureToBuffer, CopyTextureToTexture, CopyBufferToBuffer, ClearBuffer, ClearTexture,
    UpdateUniformBuffer, Barrier, FenceSync, WaitSync, ReportCounter, ReportValue, RenderEnable, EvaluateDlss,
    SepTextures, SepSamplers,   // nvnCommandBufferBindSeparateTextures / BindSeparateSamplers
};

// Une commande enregistrée : arguments x1..x7, s0..s3, et les données pointées copiées.
struct Op {
    OpType t;
    uint64_t a[7];
    float f[4];
    std::vector<uint8_t> blob;
};

struct ShaderStage {
    int stage = -1;          // numérotation NVN : 0 sommets, 1 fragments, 2 géométrie, 3 contrôle tess., 4 évaluation tess., 5 calcul
    uint64_t code = 0, ctl = 0;
    uint32_t size = 0;
    std::string hash;        // SHA-1 du code (16 car.) = nom du SPIR-V dans build/full/shader_cache
};
struct ProgramInfo {
    std::vector<ShaderStage> stages;
    std::string label;
    uint32_t version = 0;
};

struct SamplerInfo {
    uint32_t min = 0, mag = 0, wrap[3] = {0, 0, 0}, compare_mode = 0, compare_func = 0, reduction = 0;
    float min_lod = 0, max_lod = 1000, lod_bias = 0, max_aniso = 1, border[4] = {0, 0, 0, 0};
};
struct ViewInfo {
    uint32_t flags = 0;      // 1 format, 2 cible, 4 niveaux, 8 couches, 16 swizzle, 32 mode prof./stencil
    uint32_t format = 0, target = 0, base_level = 0, num_levels = 0, min_layer = 0, num_layers = 0, ds_mode = 0;
    uint8_t swizzle[4] = {0, 0, 0, 0};
};
struct TexDesc {
    uint64_t tex = 0, tex_addr = 0;   // clé de l'objet texture, adresse de l'objet
    ViewInfo view;
};
struct TexPool {
    uint64_t mempool = 0, offset = 0;
    uint32_t count = 0;
    uint64_t gen = 0;
    std::unordered_map<uint32_t, TexDesc> desc;
};
struct SampPool {
    uint64_t mempool = 0, offset = 0;
    uint32_t count = 0;
    uint64_t gen = 0;
    std::unordered_map<uint32_t, SamplerInfo> desc;
};

// état graphique courant (suivi par nvn_exec.cpp, lu par le moteur de rendu)
struct Range { uint64_t addr = 0, size = 0; };

struct GfxState {
    uint64_t rt[8] = {}, rt_view[8] = {}, depth = 0, depth_view = 0;
    uint32_t rt_level[8] = {}, rt_layer[8] = {}, depth_level = 0, depth_layer = 0;   // vues des cibles (enregistrement)
    int nrt = 0;
    int vp[4] = {0, 0, 0, 0}, sc[4] = {0, 0, 0, 0};
    float dr[2] = {0, 1};
    uint8_t blend[8][8] = {};
    uint32_t chmask = 0xffffffffu, color = 0, poly = 0;
    uint8_t ds[8] = {}, ms[4] = {};
    uint32_t attribs[32] = {};
    int nattr = 0;
    uint32_t streams[32][2] = {};
    int nstream = 0;
    Range vb[32];
    uint64_t program = 0;
    Range ubo[6][32], ssbo[6][32];
    uint64_t tex[6][64] = {}, img[6][64] = {};
    uint64_t septex[6][64] = {}, sepsamp[6][64] = {};   // poignées séparées (texture : id ; échantillonneur : id << 20)
    uint64_t texpool = 0, samppool = 0;
    uint32_t stencil_ref = 0, stencil_vmask = 0xff, stencil_wmask = 0xff;
    float blend_color[4] = {0, 0, 0, 0};
    float poly_offset[3] = {0, 0, 0};   // facteur, unités, limite
    // nvnCommandBufferPushComputeConstants : données en ligne du calcul, copiées en banque 0 à partir de 0x10
    uint8_t pushc[1024] = {};
};

extern Table<ProgramInfo> g_prog;
extern Table<ViewInfo> g_view;
extern Table<SamplerInfo> g_samplerb, g_sampler;
extern Table<TexPool> g_texpool;
extern Table<SampPool> g_samppool;
extern std::mutex g_desc_lk;

std::string sha1_16(const uint8_t* d, size_t n);

// Exécute une liste de commandes (soumission à la file).
void nvn_execute(const std::vector<Op>& ops);
// Présentation d'une texture de fenêtre (nvnQueuePresentTexture).
void nvn_present(uint64_t texture_obj, uint64_t frame);

// file d'exécution « GPU » (nvn_gpu.cpp) : soumissions exécutées dans l'ordre par un thread dédié
void gpu_submit(std::shared_ptr<std::vector<Op>> ops);
void gpu_present(uint64_t texkey, uint64_t frame, const int32_t* crop, const float* clear);
void gpu_fence(uint64_t sync);
void gpu_wait_sync(uint64_t sync);
void gpu_finish();
void gpu_pool_released(uint64_t addr, uint64_t size);   // nvnMemoryPoolFinalize, après le travail déjà soumis
void gpu_texture_released(uint64_t texkey);              // nvnTextureFinalize, de même
void gpu_acquire_wait();
void sync_reset(uint64_t sync);
void sync_signal_now(uint64_t sync);
void sync_fence_executed(uint64_t sync);
int sync_wait(uint64_t sync, int64_t timeout_ns);   // NVNsyncWaitResult (0 déjà, 1 satisfait, 2 délai)

}  // namespace nvn
}  // namespace hos
