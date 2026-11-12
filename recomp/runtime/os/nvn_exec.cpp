// Exécution des listes de commandes NVN : suivi de l'état graphique, compteurs, et analyse d'une
// image (RECOMP_FRAME_STATS=N : chaque commande et chaque dessin de l'image N dans le journal).
// Le moteur de rendu Vulkan (vk_render.cpp) se branche sur ce suivi d'état.
#include "nvn_exec.h"
#include "vk_render.h"

#include <algorithm>
#include <chrono>
#include <direct.h>

namespace hos {
namespace nvn {

Table<ProgramInfo> g_prog;
Table<ViewInfo> g_view;
Table<SamplerInfo> g_samplerb, g_sampler;
Table<TexPool> g_texpool;
Table<SampPool> g_samppool;
std::mutex g_desc_lk;

namespace {

GfxState g_st;
std::mutex g_exec_lk;

const char* opname(OpType t) {
    static const char* n[] = {
        "BlendState", "ChannelMaskState", "ColorState", "DepthStencilState", "MultisampleState", "PolygonState",
        "VertexAttribState", "VertexStreamState", "StencilValueMask", "StencilMask", "StencilRef", "BlendColor",
        "PointSize", "LineWidth", "PolygonOffset", "DepthClamp", "PrimitiveRestart", "AlphaRef", "Viewport",
        "Viewports", "Scissor", "Scissors", "DepthRange", "DepthRanges", "RenderTargets", "ClearColor",
        "ClearDepthStencil", "Program", "UniformBuffer", "UniformBuffers", "StorageBuffer", "StorageBuffers",
        "Texture", "Textures", "Image", "Images", "TexturePool", "SamplerPool", "VertexBuffer", "VertexBuffers",
        "PushConstants", "DrawArrays", "DrawElements", "DrawArraysInstanced", "DrawElementsInstanced",
        "DrawIndirect", "Dispatch", "DispatchIndirect", "CopyBufferToTexture", "CopyTextureToBuffer",
        "CopyTextureToTexture", "CopyBufferToBuffer", "ClearBuffer", "ClearTexture", "UpdateUniformBuffer",
        "Barrier", "FenceSync", "WaitSync", "ReportCounter", "ReportValue", "RenderEnable", "EvaluateDlss",
        "SepTextures", "SepSamplers"};
    size_t i = (size_t)t;
    return i < sizeof n / sizeof *n ? n[i] : "?";
}

std::string texdesc(uint64_t texobj) {
    if (!texobj) return "-";
    Tex t;
    if (!g_tex.find_key(texobj, &t)) return "?";
    char b[96];
    snprintf(b, sizeof b, "%dx%d fmt %u", t.b.w, t.b.h, t.b.format);
    return b;
}

void log_draw(const GfxState& s, const Op& o, int idx) {
    ProgramInfo p;
    g_prog.find_key(s.program, &p);
    std::string st;
    for (auto& x : p.stages) st += std::to_string(x.stage) + ":" + x.hash + " ";
    HLOG("frame", "  #%d %s prim %llu n %llu | prog '%s' %s| rt0 %s (%d targets) | vp %d,%d %dx%d | ds %02x %02x %02x %02x %02x %02x %02x %02x poly %08x",
         idx, opname(o.t), (unsigned long long)o.a[0], (unsigned long long)(o.t == OpType::DrawArrays ? o.a[2] : o.a[2]),
         p.label.c_str(), st.c_str(), texdesc(s.rt[0]).c_str(), s.nrt, s.vp[0], s.vp[1], s.vp[2], s.vp[3],
         s.ds[0], s.ds[1], s.ds[2], s.ds[3], s.ds[4], s.ds[5], s.ds[6], s.ds[7], s.poly);
    for (int stg = 0; stg < 6; stg++) {
        std::string u, t;
        for (int k = 0; k < 32; k++)
            if (s.ubo[stg][k].addr) {
                char b[64];
                snprintf(b, sizeof b, " u%d=%llx+%llx", k, (unsigned long long)s.ubo[stg][k].addr,
                         (unsigned long long)s.ubo[stg][k].size);
                u += b;
            }
        for (int k = 0; k < 64; k++)
            if (s.tex[stg][k]) {
                char b[48];
                snprintf(b, sizeof b, " t%d=%llx", k, (unsigned long long)s.tex[stg][k]);
                t += b;
            }
        for (int k = 0; k < 64; k++)
            if (s.septex[stg][k]) {
                char b[48];
                snprintf(b, sizeof b, " st%d=%llx", k, (unsigned long long)s.septex[stg][k]);
                t += b;
            }
        for (int k = 0; k < 64; k++)
            if (s.sepsamp[stg][k]) {
                char b[48];
                snprintf(b, sizeof b, " ss%d=%llx", k, (unsigned long long)(s.sepsamp[stg][k] >> 20));
                t += b;
            }
        if (!u.empty() || !t.empty()) HLOG("frame", "      stage %d:%s%s", stg, u.c_str(), t.c_str());
    }
    std::string va;
    for (int k = 0; k < s.nattr; k++) {
        uint32_t a = s.attribs[k];
        char b[48];
        snprintf(b, sizeof b, " a%d(fmt %u, stream %u, +%u)", k, a & 0x3ff, (a >> 10) & 0x1f, a >> 15);
        va += b;
    }
    for (int k = 0; k < s.nstream; k++) {
        char b[48];
        snprintf(b, sizeof b, " s%d(stride %u, vb %llx)", k, s.streams[k][0], (unsigned long long)s.vb[k].addr);
        va += b;
    }
    if (!va.empty()) HLOG("frame", "      vertices:%s", va.c_str());
}

// Diagnostic (RECOMP_DRAW_DUMP=image:n) : écrit dans build/full/drawdump/ les tampons uniformes de
// l'étage des sommets, les 256 premiers sommets de chaque flux et les 256 premiers indices du n-ième
// dessin de l'image (pour recalculer le shader de sommets hors ligne).
void draw_dump(const GfxState& s, const Op& o, int nth) {
    _mkdir("drawdump");
    char p[160];
    snprintf(p, sizeof p, "drawdump/info_%d.txt", nth);
    FILE* f = fopen(p, "w");
    if (!f) return;
    ProgramInfo pi;
    g_prog.find_key(s.program, &pi);
    fprintf(f, "programme %s\n", pi.label.c_str());
    for (auto& x : pi.stages) fprintf(f, "étage %d %s\n", x.stage, x.hash.c_str());
    fprintf(f, "op %s a %llx %llx %llx %llx %llx %llx\n", opname(o.t), (unsigned long long)o.a[0], (unsigned long long)o.a[1],
            (unsigned long long)o.a[2], (unsigned long long)o.a[3], (unsigned long long)o.a[4], (unsigned long long)o.a[5]);
    fprintf(f, "viewport %d %d %d %d profondeur %g %g\n", s.vp[0], s.vp[1], s.vp[2], s.vp[3], s.dr[0], s.dr[1]);
    for (int k = 0; k < s.nattr; k++)
        fprintf(f, "attribut %d fmt %u flux %u décalage %u\n", k, s.attribs[k] & 0x3ff, (s.attribs[k] >> 10) & 0x1f, s.attribs[k] >> 15);
    for (int k = 0; k < s.nstream; k++) {
        fprintf(f, "flux %d pas %u div %u vb %llx taille %llx\n", k, s.streams[k][0], s.streams[k][1],
                (unsigned long long)s.vb[k].addr, (unsigned long long)s.vb[k].size);
        uint64_t n = std::min<uint64_t>(s.vb[k].size, (uint64_t)s.streams[k][0] * 256);
        if (s.vb[k].addr && n && mem_valid(s.vb[k].addr, n)) {
            snprintf(p, sizeof p, "drawdump/vb%d_%d.bin", k, nth);
            if (FILE* g = fopen(p, "wb")) fwrite((const void*)s.vb[k].addr, 1, n, g), fclose(g);
            snprintf(p, sizeof p, "drawdump/vb%d_%d", k, nth);
            vkr::gpu_peek(s.vb[k].addr, n, p);
        }
    }
    for (int stg = 0; stg < 2; stg++)
        for (int k = 0; k < 32; k++)
            if (s.ubo[stg][k].addr && mem_valid(s.ubo[stg][k].addr, std::min<uint64_t>(s.ubo[stg][k].size, 0x10000))) {
                fprintf(f, "ubo étage %d emplacement %d %llx taille %llx\n", stg, k, (unsigned long long)s.ubo[stg][k].addr,
                        (unsigned long long)s.ubo[stg][k].size);
                snprintf(p, sizeof p, "drawdump/ubo%d_%d_%d.bin", stg, k, nth);
                if (FILE* g = fopen(p, "wb"))
                    fwrite((const void*)s.ubo[stg][k].addr, 1, std::min<uint64_t>(s.ubo[stg][k].size, 0x10000), g), fclose(g);
                snprintf(p, sizeof p, "drawdump/ubo%d_%d_%d", stg, k, nth);
                vkr::gpu_peek(s.ubo[stg][k].addr, std::min<uint64_t>(s.ubo[stg][k].size, 0x10000), p);
            }
    if (o.t == OpType::DrawElements || o.t == OpType::DrawElementsInstanced) {
        uint64_t isz = (o.a[1] == 0 ? 1 : o.a[1] == 1 ? 2 : 4) * 256;
        if (o.a[3] && mem_valid(o.a[3], isz)) {
            snprintf(p, sizeof p, "drawdump/ib_%d.bin", nth);
            if (FILE* g = fopen(p, "wb")) fwrite((const void*)o.a[3], 1, isz, g), fclose(g);
            snprintf(p, sizeof p, "drawdump/ib_%d", nth);
            vkr::gpu_peek(o.a[3], isz, p);
        }
    }
    fclose(f);
}

// Horodatage « GPU » des compteurs : même base que nvnDeviceGetCurrentTimestampInNanoseconds
// (horloge du jeu, compteur invité à 31,25 MHz) ; le moteur compare les deux pour régler sa cadence.
uint64_t gpu_ns() { return (uint64_t)((unsigned __int128)rt_cntvct() * 1000000000ull / rt_cntfrq()); }

}  // namespace

std::atomic<uint64_t> g_exec_ns{0};
std::atomic<uint64_t> g_op_ns[96], g_op_n[96];   // RECOMP_PERF : temps par type d'opération
void perf_report_ops() {
    std::vector<std::pair<uint64_t, int>> v;
    for (int i = 0; i < 96; i++) v.push_back({g_op_ns[i].load(), i});
    std::sort(v.rbegin(), v.rend());
    std::string o;
    for (int k = 0; k < 6 && v[k].first; k++) {
        char b[80];
        snprintf(b, sizeof b, " %s %.3f (%.0f)", opname((OpType)v[k].second), v[k].first / 120e6, g_op_n[v[k].second].load() / 120.0);
        o += b;
    }
    for (int i = 0; i < 96; i++) g_op_ns[i] = 0, g_op_n[i] = 0;
    HLOG("perf", "operations (ms/frame, count):%s", o.c_str());
}
void nvn_execute(const std::vector<Op>& ops) {
    auto t_exec = std::chrono::steady_clock::now();
    struct Acc { std::chrono::steady_clock::time_point t; ~Acc() {
        g_exec_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t).count(); } } acc{t_exec};
    // RECOMP_FRAME_STATS=N (image N) ou N+K (images N à N+K-1)
    static const long stats_frame = getenv("RECOMP_FRAME_STATS") ? atol(getenv("RECOMP_FRAME_STATS")) : -1;
    static const long stats_count = [] {
        const char* e = getenv("RECOMP_FRAME_STATS");
        const char* p = e ? strchr(e, '+') : nullptr;
        return p ? std::max(1L, atol(p + 1)) : 1L;
    }();
    std::lock_guard<std::mutex> lk(g_exec_lk);
    GfxState& s = g_st;
    bool log = stats_frame >= 0 && g_frame.load() >= (uint64_t)stats_frame &&
               g_frame.load() < (uint64_t)(stats_frame + stats_count);
    if (log) HLOG("frame", "--- submitting %zu commands (frame %llu)", ops.size(), (unsigned long long)g_frame.load());
    // RECOMP_RT_SNAPSHOT=N : captures de toutes les soumissions commencées pendant l'image N (le
    // compteur avance à la présentation, sur un autre fil, en cours de soumission)
    static const char* rt_env = getenv("RECOMP_RT_SNAPSHOT");
    static const long rt_snap = rt_env ? atol(rt_env) : -1;
    static const long rt_count = rt_env && strchr(rt_env, '+') ? std::max(1L, atol(strchr(rt_env, '+') + 1)) : 1L;   // N+K
    const uint64_t snap_frame = g_frame.load();
    const bool snap_sub = rt_snap >= 0 && snap_frame >= (uint64_t)rt_snap && snap_frame < (uint64_t)(rt_snap + rt_count);
    int idx = 0;
    static const bool perf = getenv("RECOMP_PERF") != nullptr;
    for (const Op& o : ops) {
        idx++;
        auto t_op = perf ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        struct OpAcc { bool on; OpType t; std::chrono::steady_clock::time_point t0; ~OpAcc() {
            if (on) { g_op_ns[(int)t % 96] += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count(); g_op_n[(int)t % 96]++; } } } opacc{perf, o.t, t_op};
        const uint64_t* a = o.a;
        switch (o.t) {
        case OpType::RenderTargets: {
            if (snap_sub) {   // cibles de la passe qui se termine
                static int nsub = 0;
                static const void* last_ops = nullptr;
                if (last_ops != (const void*)&ops) last_ops = &ops, nsub++;
                for (int i = 0; i < s.nrt && i < 8; i++)
                    if (s.rt[i]) {
                        char lab[32];
                        snprintf(lab, sizeof lab, "s%02d_op%d_rt%d", nsub, idx, i);
                        vkr::capture(s.rt[i], snap_frame, lab);
                    }
                if (s.depth) {
                    char lab[32];
                    snprintf(lab, sizeof lab, "s%02d_op%d_prof", nsub, idx);
                    vkr::capture(s.depth, snap_frame, lab);
                }
            }
            const uint64_t* b = (const uint64_t*)o.blob.data();
            const uint32_t* ll = o.blob.size() >= 8 * 2 * 9 + 8 * 9 ? (const uint32_t*)(o.blob.data() + 8 * 2 * 9) : nullptr;
            s.nrt = 0;
            for (int i = 0; i < 8; i++) {
                s.rt[i] = obj_key(b[i]);
                s.rt_view[i] = b[9 + i];
                s.rt_level[i] = ll ? ll[2 * i] : 0;
                s.rt_layer[i] = ll ? ll[2 * i + 1] : 0;
                if (b[i]) s.nrt = i + 1;
            }
            s.depth = obj_key(b[8]);
            s.depth_view = b[17];
            s.depth_level = ll ? ll[16] : 0;
            s.depth_layer = ll ? ll[17] : 0;
            break;
        }
        case OpType::ClearColor: {
            int i = (int)a[0];
            if (i >= 0 && i < 8 && s.rt[i] && o.blob.size() >= 16) {
                std::lock_guard<std::mutex> tl(g_tex.m);
                auto it = g_tex.map.find(s.rt[i]);
                if (it != g_tex.map.end()) memcpy(it->second.clear, o.blob.data(), 16);
            }
            if (i >= 0 && i < 8 && o.blob.size() >= 16) {
                vkr::DrawCall d;
                d.kind = vkr::DrawCall::ClearColor;
                d.st = &s;
                d.index = i;
                memcpy(d.color, o.blob.data(), 16);
                vkr::draw(d);
            }
            break;
        }
        case OpType::Viewport: for (int k = 0; k < 4; k++) s.vp[k] = (int)a[k]; break;
        case OpType::Scissor: for (int k = 0; k < 4; k++) s.sc[k] = (int)a[k]; break;
        case OpType::DepthRange: s.dr[0] = o.f[0], s.dr[1] = o.f[1]; break;
        case OpType::BlendState:
            if (o.blob.size() >= 8) memcpy(s.blend[o.blob[0] & 7], o.blob.data(), 8);
            break;
        case OpType::ChannelMaskState: if (o.blob.size() >= 4) memcpy(&s.chmask, o.blob.data(), 4); break;
        case OpType::ColorState: if (o.blob.size() >= 4) memcpy(&s.color, o.blob.data(), 4); break;
        case OpType::DepthStencilState: if (o.blob.size() >= 8) memcpy(s.ds, o.blob.data(), 8); break;
        case OpType::MultisampleState: if (o.blob.size() >= 4) memcpy(s.ms, o.blob.data(), 4); break;
        case OpType::PolygonState: if (o.blob.size() >= 4) memcpy(&s.poly, o.blob.data(), 4); break;
        case OpType::VertexAttribState:
            s.nattr = (int)std::min<uint64_t>(a[0], 32);
            if (o.blob.size() >= 4u * s.nattr) memcpy(s.attribs, o.blob.data(), 4 * s.nattr);
            break;
        case OpType::VertexStreamState:
            s.nstream = (int)std::min<uint64_t>(a[0], 32);
            if (o.blob.size() >= 8u * s.nstream) memcpy(s.streams, o.blob.data(), 8 * s.nstream);
            break;
        case OpType::VertexBuffer:
            if (a[0] < 32) s.vb[a[0]] = {a[1], a[2]};
            break;
        case OpType::VertexBuffers:
            for (uint64_t k = 0; k < a[1] && a[0] + k < 32 && 16 * (k + 1) <= o.blob.size(); k++)
                s.vb[a[0] + k] = {((const uint64_t*)o.blob.data())[2 * k], ((const uint64_t*)o.blob.data())[2 * k + 1]};
            break;
        case OpType::Program: s.program = a[0]; break;
        case OpType::UniformBuffer:
            if (a[0] < 6 && a[1] < 32) s.ubo[a[0]][a[1]] = {a[2], a[3]};
            break;
        case OpType::UniformBuffers:
            for (uint64_t k = 0; k < a[2] && a[0] < 6 && a[1] + k < 32 && 16 * (k + 1) <= o.blob.size(); k++)
                s.ubo[a[0]][a[1] + k] = {((const uint64_t*)o.blob.data())[2 * k], ((const uint64_t*)o.blob.data())[2 * k + 1]};
            break;
        case OpType::StorageBuffer:
            if (a[0] < 6 && a[1] < 32) s.ssbo[a[0]][a[1]] = {a[2], a[3]};
            break;
        case OpType::PushConstants:
            if (a[6] < sizeof s.pushc && !o.blob.empty())
                memcpy(s.pushc + a[6], o.blob.data(), std::min<size_t>(o.blob.size(), sizeof s.pushc - a[6]));
            break;
        case OpType::StorageBuffers:
            for (uint64_t k = 0; k < a[2] && a[0] < 6 && a[1] + k < 32 && 16 * (k + 1) <= o.blob.size(); k++)
                s.ssbo[a[0]][a[1] + k] = {((const uint64_t*)o.blob.data())[2 * k], ((const uint64_t*)o.blob.data())[2 * k + 1]};
            break;
        case OpType::Texture:
            if (a[0] < 6 && a[1] < 64) s.tex[a[0]][a[1]] = a[2];
            break;
        case OpType::Textures:
            for (uint64_t k = 0; k < a[2] && a[0] < 6 && a[1] + k < 64 && 8 * (k + 1) <= o.blob.size(); k++)
                s.tex[a[0]][a[1] + k] = ((const uint64_t*)o.blob.data())[k];
            break;
        case OpType::Image:
            if (a[0] < 6 && a[1] < 64) s.img[a[0]][a[1]] = a[2];
            break;
        case OpType::TexturePool: s.texpool = a[0]; break;
        case OpType::SepTextures:   // (étage, premier, nombre) + poignées
            for (uint64_t k = 0; k < a[2] && a[0] < 6 && a[1] + k < 64 && 8 * (k + 1) <= o.blob.size(); k++)
                s.septex[a[0]][a[1] + k] = ((const uint64_t*)o.blob.data())[k];
            break;
        case OpType::SepSamplers:
            for (uint64_t k = 0; k < a[2] && a[0] < 6 && a[1] + k < 64 && 8 * (k + 1) <= o.blob.size(); k++)
                s.sepsamp[a[0]][a[1] + k] = ((const uint64_t*)o.blob.data())[k];
            break;
        case OpType::Images:   // (étage, premier, nombre) + poignées
            for (uint64_t k = 0; k < a[2] && a[0] < 6 && a[1] + k < 64 && 8 * (k + 1) <= o.blob.size(); k++)
                s.img[a[0]][a[1] + k] = ((const uint64_t*)o.blob.data())[k];
            break;
        case OpType::Dispatch: vkr::dispatch(s, (uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2]); break;
        case OpType::StencilRef: s.stencil_ref = (uint32_t)a[1]; break;
        case OpType::StencilValueMask: s.stencil_vmask = (uint32_t)a[1]; break;
        case OpType::StencilMask: s.stencil_wmask = (uint32_t)a[1]; break;
        case OpType::BlendColor: if (o.blob.size() >= 16) memcpy(s.blend_color, o.blob.data(), 16); break;
        case OpType::PolygonOffset: s.poly_offset[0] = o.f[0], s.poly_offset[1] = o.f[1], s.poly_offset[2] = o.f[2]; break;
        case OpType::Barrier: vkr::barrier(); break;
        case OpType::FenceSync: {   // (sync, condition, flags) : signalé quand le GPU a fini le travail précédent
            // soumission sans attente : le sync est signalé par le fil de complétion (vk_render.cpp)
            const uint64_t v = vkr::flush_async();
            if (a[0]) {
                const uint64_t sy = a[0];
                vkr::when_done(v, [sy] { sync_fence_executed(sy); });
            }
            break;
        }
        case OpType::WaitSync:    // (sync) : attente bornée (le signal peut venir du processeur)
            if (a[0]) sync_wait(a[0], 50000000);
            break;
        case OpType::DrawArrays: case OpType::DrawArraysInstanced: {
            vkr::DrawCall d;
            d.kind = vkr::DrawCall::Arrays;
            d.st = &s;
            d.prim = a[0], d.first = (uint32_t)a[1], d.count = (uint32_t)a[2];
            if (o.t == OpType::DrawArraysInstanced) d.base_instance = (uint32_t)a[3], d.instances = (uint32_t)a[4];
            vkr::draw(d);
            break;
        }
        case OpType::DrawElements: case OpType::DrawElementsInstanced: {
            {   // RECOMP_DRAW_DUMP=image:n : vidage du n-ième dessin indexé de l'image
                static const char* dd = getenv("RECOMP_DRAW_DUMP");
                static long dd_frame = dd ? atol(dd) : -1, dd_n = dd && strchr(dd, ':') ? atol(strchr(dd, ':') + 1) : 0;
                static long count = 0;
                static uint64_t cur_frame = ~0ull;
                if (dd_frame >= 0 && g_frame.load() == (uint64_t)dd_frame) {
                    if (cur_frame != g_frame.load()) cur_frame = g_frame.load(), count = 0;
                    if (count++ == dd_n) draw_dump(s, o, (int)dd_n);
                }
            }
            vkr::DrawCall d;
            d.kind = vkr::DrawCall::Elements;
            d.st = &s;
            d.prim = a[0], d.index_type = (uint32_t)a[1], d.count = (uint32_t)a[2], d.index_addr = a[3];
            d.base_vertex = (int32_t)a[4];
            if (o.t == OpType::DrawElementsInstanced) d.base_instance = (uint32_t)a[5], d.instances = (uint32_t)a[6];
            vkr::draw(d);
            break;
        }
        case OpType::ClearDepthStencil: {   // (profondeur s0, masque profondeur, stencil, masque stencil)
            vkr::DrawCall d;
            d.kind = vkr::DrawCall::ClearDepth;
            d.st = &s;
            d.depth = o.f[0], d.depth_mask = a[0] != 0, d.stencil = (int)a[1], d.stencil_mask = (uint32_t)a[2];
            vkr::draw(d);
            break;
        }
        case OpType::SamplerPool: s.samppool = a[0]; break;
        case OpType::ReportCounter:   // (type, adresse) : { u64 compteur, u64 horodatage }
        {
            static const long tl = getenv("RECOMP_TS_LOG") ? atol(getenv("RECOMP_TS_LOG")) : 0;
            if (tl && g_frame.load() >= (uint64_t)tl && g_frame.load() < (uint64_t)tl + 4)
                HLOG("ts", "  ReportCounter type %llu → %llx at %.3f ms", (unsigned long long)a[0],
                     (unsigned long long)a[1], gpu_ns() / 1e6);
        }
            if (a[1] && mem_valid(a[1], 16)) {
                ((uint64_t*)a[1])[0] = 0;
                ((uint64_t*)a[1])[1] = gpu_ns();
            }
            break;
        case OpType::ReportValue:     // (adresse, valeur 32 bits)
            if (a[0] && mem_valid(a[0], 4)) *(uint32_t*)a[0] = (uint32_t)a[1];
            break;
        case OpType::CopyBufferToTexture:   // (src, dst, dstView, region, flags) ; région dans le blob
        case OpType::ClearTexture: {         // (dst, dstView, region, color, mask) ; région + couleur dans le blob
            const bool copy = o.t == OpType::CopyBufferToTexture;
            if (o.blob.size() >= (copy ? 40u : 56u)) {
                int32_t r[6];
                memcpy(r, o.blob.data(), 24);
                uint32_t ll[2];   // niveau et couche de la vue, lus à l'enregistrement
                memcpy(ll, o.blob.data() + (copy ? 24 : 40), 8);
                uint64_t key;     // clé de la texture, lue à l'enregistrement
                memcpy(&key, o.blob.data() + (copy ? 32 : 48), 8);
                if (key && copy) {
                    vkr::copy_buffer_to_texture(a[0], key, ll[0], ll[1], r);
                } else if (key) {
                    float c[4];
                    memcpy(c, o.blob.data() + 24, 16);
                    vkr::clear_texture(key, ll[0], ll[1], r, c);
                }
            }
            break;
        }
        case OpType::CopyTextureToTexture:   // blob : régions, niveaux / couches, clés (nvn_cmd.cpp)
            if (o.blob.size() >= 80) {
                int32_t rs[6], rd[6];
                uint32_t ls[2], ld[2];
                uint64_t ks, kd;
                memcpy(rs, o.blob.data(), 24);
                memcpy(rd, o.blob.data() + 24, 24);
                memcpy(ls, o.blob.data() + 48, 8);
                memcpy(ld, o.blob.data() + 56, 8);
                memcpy(&ks, o.blob.data() + 64, 8);
                memcpy(&kd, o.blob.data() + 72, 8);
                if (ks && kd) vkr::copy_texture_to_texture(ks, ls[0], ls[1], rs, kd, ld[0], ld[1], rd, (uint32_t)a[6]);
            }
            break;
        case OpType::CopyBufferToBuffer:   // (src, dst, size) : les deux adresses sont dans la mémoire invitée
            // dans l'ordre du GPU (après les calculs qui écrivent la source) ; processeur à défaut
            if (a[0] && a[1] && a[2] && mem_valid(a[0], a[2]) && mem_valid(a[1], a[2])) {
                static const bool cpu_copy = getenv("RECOMP_CPU_BUFFER_COPY") != nullptr;   // ancien comportement
                if (cpu_copy || !vkr::copy_buffer(a[0], a[1], a[2])) memmove((void*)a[1], (const void*)a[0], a[2]);
            }
            break;
        case OpType::ClearBuffer:          // (addr, size, value u32)
            if (a[0] && a[1] && mem_valid(a[0], a[1]))
                for (uint64_t k = 0; k + 4 <= a[1]; k += 4) *(uint32_t*)(a[0] + k) = (uint32_t)a[2];
            break;
        case OpType::UpdateUniformBuffer: {  // (addr, size, offset, dataSize, data)
            static int nlog = 0;
            if (nlog++ < 8)
                HLOG("nvn", "UpdateUniformBuffer 0x%llx (size 0x%llx) + 0x%llx: %zu bytes", (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2], o.blob.size());
            if (a[0] && !o.blob.empty() && a[2] + o.blob.size() <= a[1] && mem_valid(a[0] + a[2], o.blob.size()))
                memcpy((void*)(a[0] + a[2]), o.blob.data(), o.blob.size());
            break;
        }
        default:
            break;
        }
        if (log) {
            switch (o.t) {
            case OpType::DrawArrays: case OpType::DrawElements: case OpType::DrawArraysInstanced:
            case OpType::DrawElementsInstanced: case OpType::DrawIndirect: case OpType::Dispatch:
                log_draw(s, o, idx);
                break;
            case OpType::RenderTargets:
                HLOG("frame", "  #%d targets: %s | %s | %s, depth %s", idx, texdesc(s.rt[0]).c_str(),
                     texdesc(s.rt[1]).c_str(), texdesc(s.rt[2]).c_str(), texdesc(s.depth).c_str());
                break;
            case OpType::SepTextures: case OpType::SepSamplers: {
                std::string v;
                for (size_t k = 0; k < o.blob.size() / 8 && k < 16; k++) {
                    char b[24];
                    snprintf(b, sizeof b, " %llx", (unsigned long long)((const uint64_t*)o.blob.data())[k]);
                    v += b;
                }
                HLOG("frame", "  #%d %s stage %llu, first %llu, count %llu:%s", idx, opname(o.t), (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2], v.c_str());
                break;
            }
            case OpType::Texture: case OpType::UniformBuffer: case OpType::Program: case OpType::BlendState:
            case OpType::VertexAttribState: case OpType::VertexStreamState: case OpType::VertexBuffers:
            case OpType::ColorState: case OpType::ChannelMaskState: case OpType::DepthStencilState:
            case OpType::PolygonState: case OpType::Viewport: case OpType::Scissor:
                break;   // résumés dans les dessins
            case OpType::ClearColor: {   // valeurs réelles (copiées dans le blob)
                float c[4] = {};
                if (o.blob.size() >= 16) memcpy(c, o.blob.data(), 16);
                HLOG("frame", "  #%d ClearColor target %llu mask %llx color %g %g %g %g", idx, (unsigned long long)a[0],
                     (unsigned long long)a[2], c[0], c[1], c[2], c[3]);
                break;
            }
            default:
                HLOG("frame", "  #%d %s (%llx %llx %llx %llx) f %g %g %g %g, %zu bytes", idx, opname(o.t), (unsigned long long)a[0],
                     (unsigned long long)a[1], (unsigned long long)a[2], (unsigned long long)a[3], o.f[0], o.f[1], o.f[2],
                     o.f[3], o.blob.size());
            }
        }
    }
    // RECOMP_FLUSH_SUBMIT=1 : le GPU exécute chaque soumission du jeu avant que celle-ci ne rende la main
    // (diagnostic des courses entre écritures du processeur et lectures différées du GPU)
    static const bool flush_submit = getenv("RECOMP_FLUSH_SUBMIT") != nullptr;
    if (flush_submit) vkr::flush();
}

}  // namespace nvn
}  // namespace hos
