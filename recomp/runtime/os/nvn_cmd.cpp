// HLE NVN, partie « commandes » : objets d'état (blend, couleur, profondeur…), programmes (shaders),
// échantillonneurs, pools de textures / d'échantillonneurs, enregistrement des tampons de commandes
// et soumission à la file.
//
// Objets d'état : leur contenu vit dans leur propre mémoire, dans une disposition qui nous appartient
// (seules nos fonctions NVN la lisent), dans les tailles NVN : blend 8 o, masque de canaux 4,
// couleur 4, profondeur/stencil 8, multi-échantillonnage 4, polygone 4, attribut de sommet 4, flux 8.
// Les commandes sont enregistrées côté hôte (listes d'opérations) ; EndRecording renvoie un handle,
// QueueSubmitCommands exécute les listes via nvn_execute (nvn_exec.h).
#include "nvn_exec.h"

#include <direct.h>
#include <io.h>

#include <cmath>
#include <deque>
#include <set>

namespace hos {
namespace nvn {

// ================================================================ SHA-1 (empreinte du code des shaders)
std::string sha1_16(const uint8_t* d, size_t n) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(d, d + n);
    uint64_t bitlen = (uint64_t)n * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((uint8_t)(bitlen >> (8 * i)));
    auto rol = [](uint32_t v, int s) { return (v << s) | (v >> (32 - s)); };
    for (size_t c = 0; c < m.size(); c += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (m[c + 4 * i] << 24) | (m[c + 4 * i + 1] << 16) | (m[c + 4 * i + 2] << 8) | m[c + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], cc = h[2], dd = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (b & cc) | (~b & dd), k = 0x5A827999;
            else if (i < 40) f = b ^ cc ^ dd, k = 0x6ED9EBA1;
            else if (i < 60) f = (b & cc) | (b & dd) | (cc & dd), k = 0x8F1BBCDC;
            else f = b ^ cc ^ dd, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = dd, dd = cc, cc = rol(b, 30), b = a, a = t;
        }
        h[0] += a, h[1] += b, h[2] += cc, h[3] += dd, h[4] += e;
    }
    char out[41];
    for (int i = 0; i < 5; i++) snprintf(out + 8 * i, 9, "%08x", h[i]);
    return std::string(out, 16);
}

namespace {

template <class T>
T& at(uint64_t a, size_t off) { return *(T*)(a + off); }

// ================================================================ objets d'état
// blend : [0] cible, [1] srcRGB, [2] dstRGB, [3] srcA, [4] dstA, [5] eqRGB, [6] eqA, [7] avancé
NVN(nvnBlendStateSetDefaults) {
    uint8_t d[8] = {0, NVN_BLEND_ONE, NVN_BLEND_ZERO, NVN_BLEND_ONE, NVN_BLEND_ZERO, NVN_EQ_ADD, NVN_EQ_ADD, 0};
    memcpy((void*)A(0), d, 8);
}
NVN(nvnBlendStateSetBlendTarget) { at<uint8_t>(A(0), 0) = (uint8_t)A(1); }
NVN(nvnBlendStateSetBlendFunc) {   // (state, srcRGB, dstRGB, srcA, dstA)
    at<uint8_t>(A(0), 1) = (uint8_t)A(1), at<uint8_t>(A(0), 2) = (uint8_t)A(2);
    at<uint8_t>(A(0), 3) = (uint8_t)A(3), at<uint8_t>(A(0), 4) = (uint8_t)A(4);
}
NVN(nvnBlendStateSetBlendEquation) { at<uint8_t>(A(0), 5) = (uint8_t)A(1), at<uint8_t>(A(0), 6) = (uint8_t)A(2); }
NVN(nvnBlendStateSetAdvancedMode) { at<uint8_t>(A(0), 7) = (uint8_t)A(1); }
NVN(nvnBlendStateGetBlendTarget) { R(ctx, at<uint8_t>(A(0), 0)); }

// masque de canaux : 4 bits par cible (r=1 g=2 b=4 a=8)
NVN(nvnChannelMaskStateSetDefaults) { at<uint32_t>(A(0), 0) = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask) {   // (state, index, r, g, b, a)
    uint32_t& m = at<uint32_t>(A(0), 0);
    int i = (int)A(1) & 7;
    uint32_t v = (A(2) ? 1 : 0) | (A(3) ? 2 : 0) | (A(4) ? 4 : 0) | (A(5) ? 8 : 0);
    m = (m & ~(0xfu << (4 * i))) | (v << (4 * i));
}

// couleur : [0] masque de mélange par cible, [1] opération logique, [2] test alpha
NVN(nvnColorStateSetDefaults) { at<uint32_t>(A(0), 0) = (NVN_LOGIC_COPY << 8) | (8u << 16); }
NVN(nvnColorStateSetBlendEnable) {   // (state, index, enable)
    uint8_t& m = at<uint8_t>(A(0), 0);
    m = (uint8_t)((m & ~(1u << (A(1) & 7))) | ((A(2) ? 1u : 0u) << (A(1) & 7)));
}
NVN(nvnColorStateSetLogicOp) { at<uint8_t>(A(0), 1) = (uint8_t)A(1); }
NVN(nvnColorStateSetAlphaTest) { at<uint8_t>(A(0), 2) = (uint8_t)A(1); }
NVN(nvnColorStateGetBlendEnable) { R(ctx, (at<uint8_t>(A(0), 0) >> (A(1) & 7)) & 1); }

// profondeur / stencil : [0] test, [1] écriture, [2] fonction, [3] test stencil,
// [4] fonctions (avant | arrière << 4), [5..7] opérations (fail, zfail, zpass) avant puis arrière
NVN(nvnDepthStencilStateSetDefaults) {
    uint8_t d[8] = {0, 1, NVN_FUNC_LESS, 0, (uint8_t)(NVN_FUNC_ALWAYS | (NVN_FUNC_ALWAYS << 4)), 0x11, 0x11, 0x11};
    memcpy((void*)A(0), d, 8);
}
NVN(nvnDepthStencilStateSetDepthTestEnable) { at<uint8_t>(A(0), 0) = A(1) ? 1 : 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable) { at<uint8_t>(A(0), 1) = A(1) ? 1 : 0; }
NVN(nvnDepthStencilStateSetDepthFunc) { at<uint8_t>(A(0), 2) = (uint8_t)A(1); }
NVN(nvnDepthStencilStateSetStencilTestEnable) { at<uint8_t>(A(0), 3) = A(1) ? 1 : 0; }
NVN(nvnDepthStencilStateGetStencilTestEnable) { R(ctx, at<uint8_t>(A(0), 3)); }
NVN(nvnDepthStencilStateGetDepthTestEnable) { R(ctx, at<uint8_t>(A(0), 0)); }
NVN(nvnDepthStencilStateGetDepthWriteEnable) { R(ctx, at<uint8_t>(A(0), 1)); }
NVN(nvnDepthStencilStateGetDepthFunc) { R(ctx, at<uint8_t>(A(0), 2)); }
NVN(nvnDepthStencilStateSetStencilFunc) {   // (state, face, func)
    uint8_t& f = at<uint8_t>(A(0), 4);
    if (A(1) & 1) f = (uint8_t)((f & 0xf0) | (A(2) & 0xf));
    if (A(1) & 2) f = (uint8_t)((f & 0x0f) | ((A(2) & 0xf) << 4));
}
NVN(nvnDepthStencilStateSetStencilOp) {   // (state, face, fail, depthFail, depthPass)
    uint32_t ops = (A(2) & 0xf) | ((A(3) & 0xf) << 4) | ((A(4) & 0xf) << 8);
    uint32_t cur = at<uint8_t>(A(0), 5) | (at<uint8_t>(A(0), 6) << 8) | (at<uint8_t>(A(0), 7) << 16);
    if (A(1) & 1) cur = (cur & ~0xfffu) | ops;
    if (A(1) & 2) cur = (cur & ~(0xfffu << 12)) | (ops << 12);
    at<uint8_t>(A(0), 5) = (uint8_t)cur, at<uint8_t>(A(0), 6) = (uint8_t)(cur >> 8), at<uint8_t>(A(0), 7) = (uint8_t)(cur >> 16);
}

// multi-échantillonnage : [0] activé, [1] alpha-to-coverage, [2] échantillons
NVN(nvnMultisampleStateSetDefaults) { at<uint32_t>(A(0), 0) = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable) { at<uint8_t>(A(0), 0) = A(1) ? 1 : 0; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable) { at<uint8_t>(A(0), 1) = A(1) ? 1 : 0; }
NVN(nvnMultisampleStateSetSamples) { at<uint8_t>(A(0), 2) = (uint8_t)A(1); }
NVN(nvnMultisampleStateGetSamples) { R(ctx, at<uint8_t>(A(0), 2)); }

// polygone : [0] faces éliminées, [1] sens avant, [2] mode, [3] décalages activés
NVN(nvnPolygonStateSetDefaults) { at<uint32_t>(A(0), 0) = 0 | (1u << 8) | (2u << 16); }
NVN(nvnPolygonStateSetCullFace) { at<uint8_t>(A(0), 0) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetFrontFace) { at<uint8_t>(A(0), 1) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonMode) { at<uint8_t>(A(0), 2) = (uint8_t)A(1); }
NVN(nvnPolygonStateSetPolygonOffsetEnables) { at<uint8_t>(A(0), 3) = (uint8_t)A(1); }

// attribut de sommet : bits 0-9 format, 10-14 flux, 15-31 décalage relatif
NVN(nvnVertexAttribStateSetDefaults) { at<uint32_t>(A(0), 0) = 0; }
NVN(nvnVertexAttribStateSetFormat) {   // (state, format, relativeOffset)
    uint32_t& v = at<uint32_t>(A(0), 0);
    v = (v & (0x1fu << 10)) | ((uint32_t)A(1) & 0x3ff) | (((uint32_t)A(2) & 0x1ffff) << 15);
}
NVN(nvnVertexAttribStateSetStreamIndex) {
    uint32_t& v = at<uint32_t>(A(0), 0);
    v = (v & ~(0x1fu << 10)) | (((uint32_t)A(1) & 0x1f) << 10);
}
// flux : u32 pas, u32 diviseur
NVN(nvnVertexStreamStateSetDefaults) { at<uint64_t>(A(0), 0) = 0; }
NVN(nvnVertexStreamStateSetStride) { at<uint32_t>(A(0), 0) = (uint32_t)A(1); }
NVN(nvnVertexStreamStateSetDivisor) { at<uint32_t>(A(0), 4) = (uint32_t)A(1); }

// ================================================================ programmes
Table<ProgramInfo> g_progb;   // constructeurs (ProgramBuilder)

void dump_range(const char* path, uint64_t addr, uint64_t size) {
    uint64_t n = 0;
    while (n < size && mem_valid(addr + n, std::min<uint64_t>(0x1000, size - n))) n += std::min<uint64_t>(0x1000, size - n);
    if (!n) return;
    if (FILE* f = fopen(path, "wb")) {
        fwrite((const void*)addr, 1, n, f);
        fclose(f);
    }
}

// Lit les étages (NVNshaderData = { adresse GPU du code, section de contrôle }). Un étage encore inconnu (ni
// traduit dans shader_cache/, ni déjà vidé dans shaders/) est écrit dans build/full/shaders/ pour la
// traduction hors ligne (shaders/shaderc.py) : une partie qui atteint un passage nouveau rapporte ses shaders.
// RECOMP_DUMP_SHADERS=1 : tous les étages ; RECOMP_NO_SHADER_DUMP=1 : aucun.
// (Depuis le 2026-11-09, shaders/ contient aussi tous les paquets de shaders de la romfs, extraits hors ligne.)
static bool file_exists(const char* p) { return _access(p, 0) == 0; }
static bool stage_known(const std::string& hash, uint64_t i) {
    char p[160];
    snprintf(p, sizeof p, "shader_cache/%s.meta", hash.c_str());
    if (file_exists(p)) return true;
    snprintf(p, sizeof p, "shaders/h%s_%llu.bin", hash.c_str(), (unsigned long long)i);
    if (file_exists(p)) return true;
    snprintf(p, sizeof p, "shaders/h%s_r.bin", hash.c_str());   // extrait de la romfs
    return file_exists(p);
}

ProgramInfo read_stages(uint64_t count, uint64_t stages) {
    ProgramInfo pi;
    static const bool dump_all = getenv("RECOMP_DUMP_SHADERS") != nullptr;
    static const bool no_dump = getenv("RECOMP_NO_SHADER_DUMP") != nullptr;
    static std::atomic<int> seq{0};
    static std::mutex dlk;
    static std::set<std::string> dumped;
    if (!stages || !mem_valid(stages, 16 * count)) return pi;
    for (uint64_t i = 0; i < count; i++) {
        uint64_t code = P<uint64_t>(stages)[2 * i], ctl = P<uint64_t>(stages)[2 * i + 1];
        if (!ctl || !mem_valid(ctl, 0xe0) || P<uint32_t>(ctl)[0] != 0x98761234) continue;
        ShaderStage st;
        st.code = code;
        st.ctl = ctl;
        st.size = P<uint32_t>(ctl)[0x38 / 4];
        if (!mem_valid(code, st.size)) continue;
        uint32_t sph0 = P<uint32_t>(code)[0];
        if ((sph0 & 0x1f) >= 1 && (sph0 & 0x1f) <= 2 && ((sph0 >> 5) & 0x1f) == 4) {
            // type SPH (1 sommets, 2 contrôle tess., 3 évaluation tess., 4 géométrie, 5 fragments)
            // → numérotation NVN (0 sommets, 1 fragments, 2 géométrie, 3 contrôle, 4 évaluation)
            static const int map[] = {-1, 0, 3, 4, 2, 1};
            uint32_t ty = (sph0 >> 10) & 0xf;
            st.stage = ty <= 5 ? map[ty] : -1;
        } else {
            st.stage = 5;   // calcul
        }
        st.hash = sha1_16(P<uint8_t>(code), st.size);
        if (st.stage >= 0) pi.stages.push_back(st);
        if (!no_dump) {
            std::lock_guard<std::mutex> lk(dlk);
            if (dumped.insert(st.hash).second && (dump_all || !stage_known(st.hash, i))) {
                if (!dump_all)
                    HLOG("nvn", "unknown shader %s saved to shaders/ (to translate: python shaders/shaderc.py)",
                         st.hash.c_str());
                _mkdir("shaders");
                seq++;
                // nommés par empreinte : un nouveau lancement n'écrase pas les vidages précédents
                char p[128];
                snprintf(p, sizeof p, "shaders/h%s_%llu.ctl", st.hash.c_str(), (unsigned long long)i);
                dump_range(p, ctl, 0x2000);
                snprintf(p, sizeof p, "shaders/h%s_%llu.bin", st.hash.c_str(), (unsigned long long)i);
                dump_range(p, code, st.size);
                // banque constante 1 du programme (tables de saut de BRX, constantes) : après le code,
                // de ctl+0x40 à ctl+0x44 (décalages dans le tampon du code)
                uint32_t c0 = P<uint32_t>(ctl)[0x40 / 4], c1 = P<uint32_t>(ctl)[0x44 / 4];
                if (c1 > c0 && c0 >= st.size && c1 - c0 <= 0x10000 && mem_valid(code + c0, c1 - c0)) {
                    snprintf(p, sizeof p, "shaders/h%s_%llu.cb1", st.hash.c_str(), (unsigned long long)i);
                    dump_range(p, code + c0, c1 - c0);
                }
            }
        }
    }
    return pi;
}

// Catalogue des programmes (build/full/shaders/programmes.txt) : une ligne par programme distinct, « nom de
// l'effet <tab> étage:empreinte … », écrite quand le nom et les étages sont connus. Sert à retrouver les shaders
// d'un effet signalé (ex. vfx_charged_shot_…), les shaders extraits de la romfs n'ayant pas de nom.
static void record_program(const ProgramInfo& p) {
    if (p.label.empty() || p.stages.empty()) return;
    std::string line = p.label + "\t";
    for (auto& st : p.stages) line += std::to_string(st.stage) + ":" + st.hash + " ";
    static std::mutex lk;
    static std::set<std::string> seen;
    std::lock_guard<std::mutex> g(lk);
    if (!seen.insert(line).second) return;
    _mkdir("shaders");
    if (FILE* f = fopen("shaders/programmes.txt", "a")) {
        fprintf(f, "%s\n", line.c_str());
        fclose(f);
    }
}

NVN(nvnProgramInitialize) {
    g_prog.create(A(0));
    R(ctx, 1);
}
NVN(nvnProgramFinalize) { g_prog.erase(A(0)); }
NVN(nvnProgramSetDebugLabel) {
    ProgramInfo& p = g_prog[A(0)];
    p.label = S(A(1));
    record_program(p);
}
NVN(nvnProgramSetShaders) {   // (program, count, const NVNshaderData*)
    ProgramInfo pi = read_stages(A(1), A(2));
    ProgramInfo& p = g_prog[A(0)];
    p.stages = pi.stages;
    p.version++;
    record_program(p);
    R(ctx, 1);
}
NVN(nvnProgramBuilderSetDefaults) { g_progb.create(A(0)); }
NVN(nvnProgramBuilderSetDevice) { (void)ctx; }
NVN(nvnProgramBuilderSetShaders) {   // (builder, count, const NVNshaderData*)
    g_progb[A(0)].stages = read_stages(A(1), A(2)).stages;
}
NVN(nvnProgramInitializeFromBuilder) {   // (program, builder)
    ProgramInfo b = g_progb[A(1)];
    ProgramInfo& p = g_prog.create(A(0), b);
    p.version++;
    R(ctx, 1);
}

// ================================================================ échantillonneurs
NVN(nvnSamplerBuilderSetDefaults) { g_samplerb.create(A(0)); }
NVN(nvnSamplerBuilderSetDevice) { (void)ctx; }
NVN(nvnSamplerBuilderSetMinMagFilter) { auto& s = g_samplerb[A(0)]; s.min = (uint32_t)A(1), s.mag = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetWrapMode) { auto& s = g_samplerb[A(0)]; s.wrap[0] = (uint32_t)A(1), s.wrap[1] = (uint32_t)A(2), s.wrap[2] = (uint32_t)A(3); }
NVN(nvnSamplerBuilderSetLodClamp) { auto& s = g_samplerb[A(0)]; s.min_lod = ctx->v[0].f32[0], s.max_lod = ctx->v[1].f32[0]; }
NVN(nvnSamplerBuilderSetLodBias) { g_samplerb[A(0)].lod_bias = ctx->v[0].f32[0]; }
NVN(nvnSamplerBuilderSetCompare) { auto& s = g_samplerb[A(0)]; s.compare_mode = (uint32_t)A(1), s.compare_func = (uint32_t)A(2); }
NVN(nvnSamplerBuilderSetBorderColor) { auto& s = g_samplerb[A(0)]; if (A(1)) memcpy(s.border, (void*)A(1), 16); }
NVN(nvnSamplerBuilderSetMaxAnisotropy) { g_samplerb[A(0)].max_aniso = ctx->v[0].f32[0]; }
NVN(nvnSamplerBuilderSetReductionFilter) { g_samplerb[A(0)].reduction = (uint32_t)A(1); }
NVN(nvnSamplerInitialize) {   // (sampler, builder)
    SamplerInfo b = g_samplerb[A(1)];
    g_sampler.create(A(0), b);
    R(ctx, 1);
}
NVN(nvnSamplerFinalize) { g_sampler.erase(A(0)); }
NVN(nvnSamplerSetDebugLabel) { (void)ctx; }

// ================================================================ vues de texture
// La taille de NVNtextureView diffère entre NVN1 et NVN2 : on n'écrit que l'identifiant (8 octets)
// dans l'objet, ses champs sont côté hôte (ViewInfo, nvn_exec.h).
NVN(nvnTextureViewSetDefaults) { g_view.create(A(0)); }
NVN(nvnTextureViewSetFormat) { auto& v = g_view[A(0)]; v.format = (uint32_t)A(1); v.flags |= 1; }
NVN(nvnTextureViewSetTarget) { auto& v = g_view[A(0)]; v.target = (uint32_t)A(1); v.flags |= 2; }
NVN(nvnTextureViewSetLevels) { auto& v = g_view[A(0)]; v.base_level = (uint32_t)A(1); v.num_levels = (uint32_t)A(2); v.flags |= 4; }
NVN(nvnTextureViewSetLayers) { auto& v = g_view[A(0)]; v.min_layer = (uint32_t)A(1); v.num_layers = (uint32_t)A(2); v.flags |= 8; }
NVN(nvnTextureViewSetSwizzle) {
    auto& v = g_view[A(0)];
    for (int i = 0; i < 4; i++) v.swizzle[i] = (uint8_t)A(1 + i);
    v.flags |= 16;
}
NVN(nvnTextureViewSetDepthStencilMode) { auto& v = g_view[A(0)]; v.ds_mode = (uint32_t)A(1); v.flags |= 32; }

// ================================================================ pools de textures / d'échantillonneurs
NVN(nvnTexturePoolInitialize) {   // (pool, memoryPool, offset, numDescriptors)
    auto& p = g_texpool.create(A(0));
    p.mempool = A(1), p.offset = A(2), p.count = (uint32_t)A(3);
    R(ctx, 1);
}
NVN(nvnTexturePoolFinalize) { g_texpool.erase(A(0)); }
NVN(nvnTexturePoolSetDebugLabel) { (void)ctx; }
NVN(nvnTexturePoolGetSize) { R(ctx, g_texpool[A(0)].count); }
NVN(nvnTexturePoolRegisterTexture) {   // (pool, id, texture, view)
    TexDesc d;
    d.tex = obj_key(A(2));
    d.tex_addr = A(2);
    ViewInfo v;
    if (A(3) && g_view.find(A(3), &v)) d.view = v;
    auto& p = g_texpool[A(0)];
    std::lock_guard<std::mutex> lk(g_desc_lk);
    p.desc[(uint32_t)A(1)] = d;
    p.gen++;
}
NVN(nvnTexturePoolRegisterImage) {   // (pool, id, texture, view)
    nvnTexturePoolRegisterTexture_impl(ctx);
}
NVN(nvnSamplerPoolInitialize) {   // (pool, memoryPool, offset, numDescriptors)
    auto& p = g_samppool.create(A(0));
    p.mempool = A(1), p.offset = A(2), p.count = (uint32_t)A(3);
    R(ctx, 1);
}
NVN(nvnSamplerPoolFinalize) { g_samppool.erase(A(0)); }
NVN(nvnSamplerPoolSetDebugLabel) { (void)ctx; }
NVN(nvnSamplerPoolRegisterSampler) {   // (pool, id, sampler)
    SamplerInfo s = g_sampler[A(2)];
    auto& p = g_samppool[A(0)];
    std::lock_guard<std::mutex> lk(g_desc_lk);
    p.desc[(uint32_t)A(1)] = s;
    p.gen++;
}
NVN(nvnSamplerPoolRegisterSamplerBuilder) {   // (pool, id, builder)
    SamplerInfo s = g_samplerb[A(2)];
    auto& p = g_samppool[A(0)];
    std::lock_guard<std::mutex> lk(g_desc_lk);
    p.desc[(uint32_t)A(1)] = s;
    p.gen++;
}
// poignées : id texture dans les bits 0-19, id échantillonneur dans les bits 20-31
NVN(nvnDeviceGetTextureHandle) { R(ctx, (A(1) & 0xFFFFF) | (A(2) << 20)); }
NVN(nvnDeviceGetSeparateTextureHandle) { R(ctx, A(1) & 0xFFFFF); }
NVN(nvnDeviceGetSeparateSamplerHandle) { R(ctx, A(1) << 20); }
NVN(nvnDeviceGetImageHandle) { R(ctx, A(1) & 0xFFFFF); }
NVN(nvnDeviceGetTexelFetchHandle) { R(ctx, A(1) & 0xFFFFF); }
NVN(nvnDeviceRegisterFastClearColor) { R(ctx, 1); }

// ================================================================ tampons de commandes
struct CmdBuf {
    std::shared_ptr<std::vector<Op>> ops;
    size_t dernier = 0;   // commandes de l'enregistrement précédent : capacité réservée au suivant
};
Table<CmdBuf> g_cmd;
std::mutex g_handles_lk;
std::unordered_map<uint64_t, std::shared_ptr<std::vector<Op>>> g_handles;
std::deque<uint64_t> g_handle_order;
std::atomic<uint64_t> g_next_handle{0x10000};

// Tampon en cours d'enregistrement (soir du 2026-11-12). Le jeu enregistre ≈ 36 000 commandes par image dans les scènes
// chargées, par séries de ≈ 1 000 dans le même tampon : chaque commande prenait le verrou de la table et y cherchait
// son tampon. Chaque fil retient maintenant le dernier tampon où il a écrit ; g_rec_gen change dès qu'un tampon
// commence, finit, apparaît ou disparaît, et la recherche est alors refaite. RECOMP_NO_REC_CACHE=1 : comme avant.
std::atomic<uint64_t> g_rec_gen{1};
std::vector<Op>* rec(CPUState* ctx) {
    static const bool sans = getenv("RECOMP_NO_REC_CACHE") != nullptr;
    struct Dernier {
        uint64_t obj = 0, gen = 0;
        std::vector<Op>* ops = nullptr;
    };
    thread_local Dernier d;
    const uint64_t obj = A(0), gen = g_rec_gen.load(std::memory_order_acquire);
    if (d.ops && d.obj == obj && d.gen == gen && !sans) return d.ops;
    auto& c = g_cmd[obj];
    if (!c.ops) c.ops = std::make_shared<std::vector<Op>>();
    d.obj = obj, d.gen = gen, d.ops = c.ops.get();
    return d.ops;
}
Op& push(CPUState* ctx, OpType t) {
    auto* v = rec(ctx);
    v->emplace_back();
    Op& o = v->back();
    o.t = t;
    for (int i = 0; i < 7; i++) o.a[i] = ctx->x[1 + i];
    for (int i = 0; i < 4; i++) o.f[i] = ctx->v[i].f32[0];
    return o;
}
void blob(Op& o, uint64_t addr, size_t n) {
    if (addr && n && mem_valid(addr, n)) o.blob.assign((uint8_t*)addr, (uint8_t*)addr + n);
}

NVN(nvnCommandBufferInitialize) {
    g_cmd.create(A(0));
    g_rec_gen.fetch_add(1, std::memory_order_release);
    R(ctx, 1);
}
NVN(nvnCommandBufferFinalize) {
    g_cmd.erase(A(0));
    g_rec_gen.fetch_add(1, std::memory_order_release);
}
NVN(nvnCommandBufferSetDebugLabel) { (void)ctx; }
NVN(nvnCommandBufferAddCommandMemory) { (void)ctx; }
NVN(nvnCommandBufferAddControlMemory) { (void)ctx; }
NVN(nvnCommandBufferSetMemoryCallback) { (void)ctx; }
NVN(nvnCommandBufferSetMemoryCallbackData) { (void)ctx; }
NVN(nvnCommandBufferGetCommandMemorySize) { R(ctx, 0x100000); }
NVN(nvnCommandBufferGetCommandMemoryUsed) { R(ctx, 0); }
NVN(nvnCommandBufferGetCommandMemoryFree) { R(ctx, 0x100000); }
NVN(nvnCommandBufferGetControlMemorySize) { R(ctx, 0x10000); }
NVN(nvnCommandBufferGetControlMemoryUsed) { R(ctx, 0); }
NVN(nvnCommandBufferGetControlMemoryFree) { R(ctx, 0x10000); }
NVN(nvnCommandBufferBeginRecording) {
    auto& c = g_cmd[A(0)];
    auto ops = std::make_shared<std::vector<Op>>();
    // capacité de l'enregistrement précédent de ce tampon, plus un huitième : le vecteur ne grandit plus par doublements
    ops->reserve(std::min<size_t>(c.dernier + c.dernier / 8 + 16, 65536));
    c.ops = std::move(ops);
    g_rec_gen.fetch_add(1, std::memory_order_release);
}
NVN(nvnCommandBufferEndRecording) {
    auto& c = g_cmd[A(0)];
    auto ops = c.ops ? c.ops : std::make_shared<std::vector<Op>>();
    c.dernier = ops->size();
    c.ops.reset();
    g_rec_gen.fetch_add(1, std::memory_order_release);
    uint64_t h = g_next_handle++;
    std::lock_guard<std::mutex> lk(g_handles_lk);
    g_handles[h] = ops;
    g_handle_order.push_back(h);
    // les handles sont rarement resoumis : on garde les 4096 derniers
    while (g_handle_order.size() > 4096) {
        g_handles.erase(g_handle_order.front());
        g_handle_order.pop_front();
    }
    R(ctx, h);
}

// états
NVN(nvnCommandBufferBindBlendState) { blob(push(ctx, OpType::BlendState), A(1), 8); }
NVN(nvnCommandBufferBindChannelMaskState) { blob(push(ctx, OpType::ChannelMaskState), A(1), 4); }
NVN(nvnCommandBufferBindColorState) { blob(push(ctx, OpType::ColorState), A(1), 4); }
NVN(nvnCommandBufferBindDepthStencilState) { blob(push(ctx, OpType::DepthStencilState), A(1), 8); }
NVN(nvnCommandBufferBindMultisampleState) { blob(push(ctx, OpType::MultisampleState), A(1), 4); }
NVN(nvnCommandBufferBindPolygonState) { blob(push(ctx, OpType::PolygonState), A(1), 4); }
NVN(nvnCommandBufferBindVertexAttribState) { blob(push(ctx, OpType::VertexAttribState), A(2), 4 * std::min<uint64_t>(A(1), 32)); }
NVN(nvnCommandBufferBindVertexStreamState) { blob(push(ctx, OpType::VertexStreamState), A(2), 8 * std::min<uint64_t>(A(1), 32)); }
NVN(nvnCommandBufferSetStencilValueMask) { push(ctx, OpType::StencilValueMask); }
NVN(nvnCommandBufferSetStencilMask) { push(ctx, OpType::StencilMask); }
NVN(nvnCommandBufferSetStencilRef) { push(ctx, OpType::StencilRef); }
NVN(nvnCommandBufferSetBlendColor) { blob(push(ctx, OpType::BlendColor), A(1), 16); }
NVN(nvnCommandBufferSetPointSize) { push(ctx, OpType::PointSize); }
NVN(nvnCommandBufferSetLineWidth) { push(ctx, OpType::LineWidth); }
NVN(nvnCommandBufferSetPolygonOffsetClamp) { push(ctx, OpType::PolygonOffset); }
NVN(nvnCommandBufferSetDepthClamp) { push(ctx, OpType::DepthClamp); }
NVN(nvnCommandBufferSetPrimitiveRestart) { push(ctx, OpType::PrimitiveRestart); }
NVN(nvnCommandBufferSetAlphaRef) { push(ctx, OpType::AlphaRef); }
// géométrie de la fenêtre de rendu
NVN(nvnCommandBufferSetViewport) { push(ctx, OpType::Viewport); }
NVN(nvnCommandBufferSetViewports) { blob(push(ctx, OpType::Viewports), A(3), 16 * std::min<uint64_t>(A(2), 16)); }
NVN(nvnCommandBufferSetScissor) { push(ctx, OpType::Scissor); }
NVN(nvnCommandBufferSetScissors) { blob(push(ctx, OpType::Scissors), A(3), 16 * std::min<uint64_t>(A(2), 16)); }
NVN(nvnCommandBufferSetDepthRange) { push(ctx, OpType::DepthRange); }
NVN(nvnCommandBufferSetDepthRanges) { blob(push(ctx, OpType::DepthRanges), A(3), 8 * std::min<uint64_t>(A(2), 16)); }
// cibles et effacements
static void view_level_layer(uint64_t view, uint32_t* out);
// (cmd, n, colors**, views**, depth*, depthView*) : 9 textures, 9 clés de vue, puis niveau et couche de chaque
// vue (9 × 2 × u32) lus à l'enregistrement. Les vues étaient ignorées : les passes de réduction en mipmaps
// (profondeur linéaire du SAO, lue par le flou de profondeur) écrivaient toutes dans le niveau 0.
NVN(nvnCommandBufferSetRenderTargets) {
    Op& o = push(ctx, OpType::RenderTargets);
    int n = (int)std::min<uint64_t>(A(1), 8);
    o.blob.assign(8 * 2 * 9 + 8 * 9, 0);
    uint64_t* b = (uint64_t*)o.blob.data();
    uint32_t* ll = (uint32_t*)(o.blob.data() + 8 * 2 * 9);
    for (int i = 0; i < n; i++) {
        b[i] = A(2) ? P<uint64_t>(A(2))[i] : 0;
        const uint64_t v = A(3) ? P<uint64_t>(A(3))[i] : 0;
        b[9 + i] = v ? obj_key(v) : 0;
        view_level_layer(v, ll + 2 * i);
    }
    b[8] = A(4);
    b[17] = A(5) ? obj_key(A(5)) : 0;
    view_level_layer(A(5), ll + 16);
}
NVN(nvnCommandBufferClearColor) { blob(push(ctx, OpType::ClearColor), A(2), 16); }
NVN(nvnCommandBufferClearColori) { blob(push(ctx, OpType::ClearColor), A(2), 16); }
NVN(nvnCommandBufferClearColorui) { blob(push(ctx, OpType::ClearColor), A(2), 16); }
NVN(nvnCommandBufferClearDepthStencil) { push(ctx, OpType::ClearDepthStencil); }
NVN(nvnCommandBufferDiscardColor) { (void)ctx; }
NVN(nvnCommandBufferDiscardDepthStencil) { (void)ctx; }
NVN(nvnCommandBufferSaveZCullData) { (void)ctx; }
NVN(nvnCommandBufferRestoreZCullData) { (void)ctx; }
NVN(nvnCommandBufferResetCounter) { (void)ctx; }
// programmes et ressources
NVN(nvnCommandBufferBindProgram) {   // (cmd, program, stageBits)
    Op& o = push(ctx, OpType::Program);
    o.a[0] = obj_key(A(1));
}
NVN(nvnCommandBufferBindUniformBuffer) { push(ctx, OpType::UniformBuffer); }       // (stage, index, addr, size)
NVN(nvnCommandBufferBindUniformBuffers) { blob(push(ctx, OpType::UniformBuffers), A(4), 16 * std::min<uint64_t>(A(3), 32)); }
NVN(nvnCommandBufferBindStorageBuffer) { push(ctx, OpType::StorageBuffer); }       // (stage, index, addr, size)
NVN(nvnCommandBufferBindStorageBuffers) { blob(push(ctx, OpType::StorageBuffers), A(4), 16 * std::min<uint64_t>(A(3), 32)); }
NVN(nvnCommandBufferBindTexture) { push(ctx, OpType::Texture); }                   // (stage, index, handle)
NVN(nvnCommandBufferBindTextures) { blob(push(ctx, OpType::Textures), A(4), 8 * std::min<uint64_t>(A(3), 64)); }
NVN(nvnCommandBufferBindSeparateTextures) { blob(push(ctx, OpType::SepTextures), A(4), 8 * std::min<uint64_t>(A(3), 64)); }
NVN(nvnCommandBufferBindSeparateSamplers) { blob(push(ctx, OpType::SepSamplers), A(4), 8 * std::min<uint64_t>(A(3), 64)); }
NVN(nvnCommandBufferBindImage) { push(ctx, OpType::Image); }                       // (stage, index, handle)
NVN(nvnCommandBufferBindImages) { blob(push(ctx, OpType::Images), A(4), 8 * std::min<uint64_t>(A(3), 64)); }
NVN(nvnCommandBufferSetTexturePool) { push(ctx, OpType::TexturePool).a[0] = obj_key(A(1)); }
NVN(nvnCommandBufferSetSamplerPool) { push(ctx, OpType::SamplerPool).a[0] = obj_key(A(1)); }
NVN(nvnCommandBufferSetShaderScratchMemory) { (void)ctx; }
NVN(nvnCommandBufferBindVertexBuffer) { push(ctx, OpType::VertexBuffer); }         // (index, addr, size)
NVN(nvnCommandBufferBindVertexBuffers) { blob(push(ctx, OpType::VertexBuffers), A(3), 16 * std::min<uint64_t>(A(2), 32)); }
// Données en ligne du calcul (banque 0 à partir de 0x10 ; taille en mots à l'octet 0xa9 du contrôle).
// Copiées à l'enregistrement : le jeu réutilise sa mémoire. Deux formes reconnues, (décalage, taille,
// données) ou (données, taille) ; les premiers appels sont journalisés pour la vérification.
NVN(nvnCommandBufferPushComputeConstants) {
    const uint64_t a1 = A(1), a2 = A(2), a3 = A(3);
    Op& o = push(ctx, OpType::PushConstants);
    static int nlog = 0;
    const char* form = "?";
    if (a3 && a2 && a2 <= 0x400 && a1 < 0x400 && mem_valid(a3, a2)) {
        o.a[6] = a1, form = "offset, size, data";
        blob(o, a3, a2);
    } else if (a1 && a2 && a2 <= 0x400 && mem_valid(a1, a2)) {
        o.a[6] = 0, form = "data, size";
        blob(o, a1, a2);
    }
    if (nlog++ < 6)
        HLOG("nvn", "PushComputeConstants(0x%llx, 0x%llx, 0x%llx): form '%s', %zu bytes", (unsigned long long)a1,
             (unsigned long long)a2, (unsigned long long)a3, form, o.blob.size());
}
// dessins et calculs
NVN(nvnCommandBufferDrawArrays) { push(ctx, OpType::DrawArrays); }                 // (prim, first, count)
NVN(nvnCommandBufferDrawElements) { push(ctx, OpType::DrawElements).a[4] = 0; }   // (prim, type, count, ib)
NVN(nvnCommandBufferDrawElementsBaseVertex) { push(ctx, OpType::DrawElements); }   // (prim, type, count, ib, baseVertex)
NVN(nvnCommandBufferDrawArraysInstanced) { push(ctx, OpType::DrawArraysInstanced); }   // (prim, first, count, baseInst, instCount)
NVN(nvnCommandBufferDrawElementsInstanced) { push(ctx, OpType::DrawElementsInstanced); } // (prim, type, count, ib, baseVtx, baseInst, instCount)
NVN(nvnCommandBufferDrawArraysIndirect) { push(ctx, OpType::DrawIndirect); }
NVN(nvnCommandBufferDrawElementsIndirect) { push(ctx, OpType::DrawIndirect); }
NVN(nvnCommandBufferMultiDrawArraysIndirectCount) { push(ctx, OpType::DrawIndirect); }
NVN(nvnCommandBufferMultiDrawElementsIndirectCount) { push(ctx, OpType::DrawIndirect); }
NVN(nvnCommandBufferDispatchCompute) { push(ctx, OpType::Dispatch); }
NVN(nvnCommandBufferDispatchComputeIndirect) { push(ctx, OpType::DispatchIndirect); }
// copies
// niveau et couche de base d'une vue, lus à l'enregistrement : le jeu réutilise un même objet vue
// qu'il modifie avant chaque copie (lu à l'exécution, toutes les copies visaient le dernier niveau)
static void view_level_layer(uint64_t view, uint32_t* out) {
    out[0] = out[1] = 0;
    ViewInfo v;
    if (view && g_view.find(view, &v)) {
        if (v.flags & 4) out[0] = v.base_level;
        if (v.flags & 8) out[1] = v.min_layer;
    }
}
NVN(nvnCommandBufferCopyBufferToTexture) {   // (src, dst, dstView, region, flags) : région (24 o), niveau, couche, clé
    Op& o = push(ctx, OpType::CopyBufferToTexture);
    o.blob.assign(40, 0);
    if (A(4) && mem_valid(A(4), 24)) memcpy(o.blob.data(), (void*)A(4), 24);
    view_level_layer(A(3), (uint32_t*)(o.blob.data() + 24));
    const uint64_t key = obj_key(A(2));   // l'objet texture peut être réinitialisé avant l'exécution
    memcpy(o.blob.data() + 32, &key, 8);
}
NVN(nvnCommandBufferCopyTextureToBuffer) { blob(push(ctx, OpType::CopyTextureToBuffer), A(3), 24); }   // (src, srcView, region, dst, flags)
// (src, srcView, srcRegion, dst, dstView, dstRegion, flags) : régions (2 × 24 o), niveau et couche des deux
// vues (2 × 8 o) et clés des deux textures (2 × 8 o), lus à l'enregistrement ; drapeaux en a[6]
NVN(nvnCommandBufferCopyTextureToTexture) {
    Op& o = push(ctx, OpType::CopyTextureToTexture);
    o.blob.assign(80, 0);
    if (A(3) && mem_valid(A(3), 24)) memcpy(o.blob.data(), (void*)A(3), 24);
    if (A(6) && mem_valid(A(6), 24)) memcpy(o.blob.data() + 24, (void*)A(6), 24);
    view_level_layer(A(2), (uint32_t*)(o.blob.data() + 48));
    view_level_layer(A(5), (uint32_t*)(o.blob.data() + 56));
    const uint64_t ks = A(1) ? obj_key(A(1)) : 0, kd = A(4) ? obj_key(A(4)) : 0;
    memcpy(o.blob.data() + 64, &ks, 8);
    memcpy(o.blob.data() + 72, &kd, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer) { push(ctx, OpType::CopyBufferToBuffer); }   // (src, dst, size, flags)
NVN(nvnCommandBufferClearBuffer) { push(ctx, OpType::ClearBuffer); }               // (addr, size, value)
NVN(nvnCommandBufferClearTexture) {   // (dst, dstView, region, color*, mask) : région (24 o), couleur (16 o), niveau, couche
    Op& o = push(ctx, OpType::ClearTexture);
    o.blob.assign(56, 0);
    if (A(3) && mem_valid(A(3), 24)) memcpy(o.blob.data(), (void*)A(3), 24);
    if (A(4) && mem_valid(A(4), 16)) memcpy(o.blob.data() + 24, (void*)A(4), 16);
    view_level_layer(A(2), (uint32_t*)(o.blob.data() + 40));
    const uint64_t key = obj_key(A(1));
    memcpy(o.blob.data() + 48, &key, 8);
}
NVN(nvnCommandBufferUpdateUniformBuffer) {   // (addr, size, offset, dataSize, data)
    Op& o = push(ctx, OpType::UpdateUniformBuffer);
    blob(o, A(5), std::min<uint64_t>(A(4), 0x10000));
}
// synchronisation
NVN(nvnCommandBufferBarrier) { push(ctx, OpType::Barrier); }
NVN(nvnCommandBufferFenceSync) { push(ctx, OpType::FenceSync); }
NVN(nvnCommandBufferWaitSync) { push(ctx, OpType::WaitSync); }
NVN(nvnCommandBufferReportCounter) { push(ctx, OpType::ReportCounter); }           // (type, addr)
NVN(nvnCommandBufferReportValue) { push(ctx, OpType::ReportValue); }
NVN(nvnCommandBufferSetRenderEnable) { push(ctx, OpType::RenderEnable); }
NVN(nvnCommandBufferInvalidateTexture) { (void)ctx; }
NVN(nvnTextureInvalidateTexels) { (void)ctx; }
// DLSS : résolution de rendu = résolution de sortie (pas de mise à l'échelle) ; l'évaluation est
// exécutée par le moteur de rendu comme une copie de l'image d'entrée vers celle de sortie.
struct DlssBuilder { uint32_t out_w = 1920, out_h = 1080; };
Table<DlssBuilder> g_dlssb;
NVN(nvnDlssNetworkBuilderSetDefaults) { g_dlssb.create(A(0)); }
NVN(nvnDlssNetworkBuilderSetOutputResolution) {   // (builder, w, h)
    auto& b = g_dlssb[A(0)];
    b.out_w = (uint32_t)A(1), b.out_h = (uint32_t)A(2);
}
NVN(nvnDlssNetworkBuilderGetOptimalSettings) {    // (builder, u32* w, u32* h, …)
    DlssBuilder b = g_dlssb[A(0)];
    // RECOMP_DLSS_SCALE=s (essais) : résolution de rendu = s × sortie
    static const double s = getenv("RECOMP_DLSS_SCALE") ? atof(getenv("RECOMP_DLSS_SCALE")) : 1.0;
    const uint32_t w = s > 0 && s < 1 ? (uint32_t)(b.out_w * s + 0.5) & ~1u : b.out_w;
    const uint32_t h = s > 0 && s < 1 ? (uint32_t)(b.out_h * s + 0.5) & ~1u : b.out_h;
    if (A(1)) *P<uint32_t>(A(1)) = w;
    if (A(2)) *P<uint32_t>(A(2)) = h;
    HLOG("nvn", "DLSS: rendering %ux%u for an output of %ux%u", w, h, b.out_w, b.out_h);
}
NVN(nvnDlssNetworkBuilderGetStorageSize) { R(ctx, 0x10000); }
Table<DlssBuilder> g_dlssn;   // réseau initialisé : sa résolution de sortie
NVN(nvnDlssNetworkInitialize) {   // (réseau, builder)
    g_dlssn.create(A(0), g_dlssb[A(1)]);
    R(ctx, 1);
}
NVN(nvnDlssNetworkResize) { R(ctx, 1); }

// Paramètres d'une évaluation (2026-11-12, relevés avec RECOMP_DLSS_TRACE) : le moteur n'évalue le DLSS que
// dans le réglage « Comme la console » (native_resolution = false, rendu à 50 %), comme un anticrénelage à la
// résolution interne : couleur RGBA16F, profondeur D32F_S8, vecteurs de mouvement RG16F, masque R8 et sortie
// RGBA16F, toutes à la résolution interne ; il agrandit ensuite l'image lui-même. Les textures sont gardées par
// clé d'objet, avec leur taille.
struct DlssParams {
    struct Tx {
        uint64_t key = 0;
        uint32_t w = 0, h = 0;
        int32_t base[2] = {0, 0};
    } color, depth, mv, out, mask;
    uint32_t in_w = 0, in_h = 0;
    float jitter[2] = {0, 0}, mv_scale[2] = {1, 1};
    bool reset = false;
    uint32_t index = 0;
};
Table<DlssParams> g_dlssp;
void dlss_tx(DlssParams::Tx& t, uint64_t obj) {
    t.key = obj ? obj_key(obj) : 0;
    Tex x;
    if (obj && g_tex.find(obj, &x)) t.w = (uint32_t)x.b.w, t.h = (uint32_t)x.b.h;
}
NVN(nvnDlssParametersSetDefaults) { g_dlssp.create(A(0)); }
NVN(nvnDlssParametersSetInputColor) { dlss_tx(g_dlssp[A(0)].color, A(1)); }
NVN(nvnDlssParametersSetDepth) { dlss_tx(g_dlssp[A(0)].depth, A(1)); }
NVN(nvnDlssParametersSetMotionVectors) { dlss_tx(g_dlssp[A(0)].mv, A(1)); }
NVN(nvnDlssParametersSetOutputColor) { dlss_tx(g_dlssp[A(0)].out, A(1)); }
NVN(nvnDlssParametersSetBiasCurrentColorMask) { dlss_tx(g_dlssp[A(0)].mask, A(1)); }
NVN(nvnDlssParametersSetInputColorSubrectangleBase) {
    auto& p = g_dlssp[A(0)];
    p.color.base[0] = (int32_t)A(1), p.color.base[1] = (int32_t)A(2);
}
NVN(nvnDlssParametersSetDepthSubrectangleBase) {
    auto& p = g_dlssp[A(0)];
    p.depth.base[0] = (int32_t)A(1), p.depth.base[1] = (int32_t)A(2);
}
NVN(nvnDlssParametersSetMotionVectorSubrectangleBase) {
    auto& p = g_dlssp[A(0)];
    p.mv.base[0] = (int32_t)A(1), p.mv.base[1] = (int32_t)A(2);
}
NVN(nvnDlssParametersSetOutputSubrectangleBase) {
    auto& p = g_dlssp[A(0)];
    p.out.base[0] = (int32_t)A(1), p.out.base[1] = (int32_t)A(2);
}
NVN(nvnDlssParametersSetBiasCurrentColorSubrectangleBase) {
    auto& p = g_dlssp[A(0)];
    p.mask.base[0] = (int32_t)A(1), p.mask.base[1] = (int32_t)A(2);
}
NVN(nvnDlssParametersSetInputSubrectangleSize) {
    auto& p = g_dlssp[A(0)];
    p.in_w = (uint32_t)A(1), p.in_h = (uint32_t)A(2);
}
NVN(nvnDlssParametersSetJitterOffset) {   // flottants (s0, s1)
    auto& p = g_dlssp[A(0)];
    p.jitter[0] = ctx->v[0].f32[0], p.jitter[1] = ctx->v[1].f32[0];
}
NVN(nvnDlssParametersSetMotionVectorScale) {   // flottants (s0, s1)
    auto& p = g_dlssp[A(0)];
    p.mv_scale[0] = ctx->v[0].f32[0], p.mv_scale[1] = ctx->v[1].f32[0];
}
NVN(nvnDlssParametersSetHistoryReset) { g_dlssp[A(0)].reset = A(1) != 0; }
NVN(nvnDlssParametersSetEvaluationIndex) { g_dlssp[A(0)].index = (uint32_t)A(1); }
// RECOMP_DLSS_TRACE=1 (essais) : arguments et mémoire pointée des appels DLSS du jeu, pour retrouver ce qu'il passe à
// l'évaluation (couleur, profondeur, vecteurs de mouvement, sortie, décalage de gigue…).
void dlss_trace(CPUState* ctx, const char* nom, uint32_t n) {
    HLOG("nvn", "DLSS trace %s #%u: x0..x7 = %llx %llx %llx %llx %llx %llx %llx %llx", nom, n,
         (unsigned long long)A(0), (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3),
         (unsigned long long)A(4), (unsigned long long)A(5), (unsigned long long)A(6), (unsigned long long)A(7));
    HLOG("nvn", "  s0..s7 = %g %g %g %g %g %g %g %g", ctx->v[0].f32[0], ctx->v[1].f32[0], ctx->v[2].f32[0],
         ctx->v[3].f32[0], ctx->v[4].f32[0], ctx->v[5].f32[0], ctx->v[6].f32[0], ctx->v[7].f32[0]);
    uint64_t pile[8] = {};
    if (mem_valid(ctx->sp, sizeof pile)) memcpy(pile, (const void*)ctx->sp, sizeof pile);
    HLOG("nvn", "  [sp+0..0x38] = %llx %llx %llx %llx %llx %llx %llx %llx", (unsigned long long)pile[0],
         (unsigned long long)pile[1], (unsigned long long)pile[2], (unsigned long long)pile[3],
         (unsigned long long)pile[4], (unsigned long long)pile[5], (unsigned long long)pile[6],
         (unsigned long long)pile[7]);
    auto vider = [](const char* nom, uint64_t p, int taille) {
        if (p < 0x7000000000ull || p >= 0x8000000000ull || !mem_valid(p, taille)) return;
        for (int l = 0; l < taille; l += 32) {
            char b[32 * 3 + 1];
            for (int j = 0; j < 32; j++) snprintf(b + 3 * j, 4, "%02x ", ((const uint8_t*)p)[l + j]);
            HLOG("nvn", "  %s+%03x: %s", nom, l, b);
        }
    };
    char etiquette[16];
    for (int k = 0; k < 8; k++) {
        Tex t;
        if (A(k) >= 0x7000000000ull && A(k) < 0x8000000000ull && g_tex.find(A(k), &t))
            HLOG("nvn", "  x%d = texture %dx%dx%d, format %u, target %u, levels %u, @%llx", k, t.b.w, t.b.h, t.b.d,
                 t.b.format, t.b.target, t.b.levels, (unsigned long long)t.addr);
    }
    for (int k = 0; k < 8; k++) {
        Tex t;
        if (pile[k] >= 0x7000000000ull && pile[k] < 0x8000000000ull && g_tex.find(pile[k], &t))
            HLOG("nvn", "  [sp+%x] = texture %dx%dx%d, format %u, target %u, levels %u, @%llx", k * 8, t.b.w, t.b.h,
                 t.b.d, t.b.format, t.b.target, t.b.levels, (unsigned long long)t.addr);
    }
    for (int k = 0; k < 8; k++) {
        snprintf(etiquette, sizeof etiquette, "x%d", k);
        vider(etiquette, A(k), 0x100);
    }
    for (int k = 0; k < 8; k++) {
        snprintf(etiquette, sizeof etiquette, "sp%d", k);
        vider(etiquette, pile[k], 0xc0);
    }
}
bool dlss_trace_on() {
    static const bool on = getenv("RECOMP_DLSS_TRACE") != nullptr;
    return on;
}
NVN(nvnDlssNetworkBuilderSetTextureIDs) {
    if (dlss_trace_on()) dlss_trace(ctx, "SetTextureIDs", 0);
}
// (tampon de commandes, réseau, paramètres) : jusqu'ici ignorée, la sortie restait vide (scène 3D noire avec « Comme
// la console »). Maintenant : la couleur d'entrée est recopiée dans la sortie (agrandie si leurs tailles diffèrent),
// en attendant un vrai DLSS (SDK de NVIDIA, avec l'accord de l'utilisateur).
NVN(nvnCommandBufferEvaluateDlss) {
    static std::atomic<uint32_t> n{0};
    if (dlss_trace_on()) {
        const uint32_t i = n++;
        if (i < 3 || i % 600 == 0) dlss_trace(ctx, "Evaluate", i);
    }
    DlssParams p;
    if (!A(2) || !g_dlssp.find(A(2), &p) || !p.color.key || !p.out.key) {
        push(ctx, OpType::EvaluateDlss);
        return;
    }
    const uint32_t iw = p.in_w ? p.in_w : p.color.w, ih = p.in_h ? p.in_h : p.color.h;
    DlssBuilder net;
    const bool net_ok = A(1) && g_dlssn.find(A(1), &net);
    uint32_t ow = p.out.w > (uint32_t)p.out.base[0] ? p.out.w - (uint32_t)p.out.base[0] : 0;
    uint32_t oh = p.out.h > (uint32_t)p.out.base[1] ? p.out.h - (uint32_t)p.out.base[1] : 0;
    if (net_ok && net.out_w && net.out_h) ow = std::min(ow, net.out_w), oh = std::min(oh, net.out_h);
    if (!iw || !ih || !ow || !oh) {
        push(ctx, OpType::EvaluateDlss);
        return;
    }
    Op& o = push(ctx, OpType::CopyTextureToTexture);
    o.blob.assign(80, 0);
    const int32_t rs[6] = {p.color.base[0], p.color.base[1], 0, (int32_t)iw, (int32_t)ih, 1};
    const int32_t rd[6] = {p.out.base[0], p.out.base[1], 0, (int32_t)ow, (int32_t)oh, 1};
    memcpy(o.blob.data(), rs, 24);
    memcpy(o.blob.data() + 24, rd, 24);
    memcpy(o.blob.data() + 64, &p.color.key, 8);
    memcpy(o.blob.data() + 72, &p.out.key, 8);
    o.a[6] = iw != ow || ih != oh ? 1 : 0;   // drapeaux de la copie : filtre linéaire si agrandie
    static std::atomic<bool> dit{false};
    if (!dit.exchange(true))
        HLOG("nvn", "DLSS evaluation: input %ux%u copied to output %ux%u (no DLSS yet)", iw, ih, ow, oh);
}

// ================================================================ file
NVN(nvnQueueSubmitCommands) {   // (queue, count, const NVNcommandHandle*)
    int n = (int)A(1);
    std::vector<std::shared_ptr<std::vector<Op>>> lists;
    {
        std::lock_guard<std::mutex> lk(g_handles_lk);
        for (int i = 0; i < n; i++) {
            auto it = g_handles.find(P<uint64_t>(A(2))[i]);
            if (it != g_handles.end()) lists.push_back(it->second);
        }
    }
    for (auto& l : lists) gpu_submit(l);   // exécution asynchrone (thread GPU, nvn_gpu.cpp)
}

}  // namespace
}  // namespace nvn
}  // namespace hos
