// HLE graphique, niveau API :
//  * nn::gs::sdk (démarrage du système graphique Switch 2 : gsv, nvsrv, syncpt…) : court-circuité ;
//  * nn::vi (affichage, couche, fenêtre native) : un affichage et une couche fictifs ;
//  * NVN : nvnBootstrapLoader / nvnDeviceGetProcAddress renvoient des fonctions natives. Les objets
//    NVN du jeu (device, queue, pools, textures, fenêtres…) sont suivis dans des tables côté hôte,
//    indexées par un identifiant écrit dans les 8 premiers octets de l'objet (le moteur copie des
//    objets initialisés). Fonctions non écrites : bouchons nommés qui journalisent leur premier
//    appel (arguments) et renvoient 0 (--trace-nvn : tous les appels).
//
// Le rendu réel (commandes, shaders) n'est pas encore traduit : la présentation affiche, dans une
// fenêtre hôte, la dernière couleur d'effacement de la texture présentée (preuve de la boucle).
#include "gfx.h"
#include "nvn.h"
#include "settings.h"
#include "vk_render.h"

#include <direct.h>

#include <array>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace hos {
namespace {
using namespace nvn;


// ================================================================ nn::gs
void gs_boot(CPUState* ctx) {
    HLOG("gfx", "nn::gs::sdk::BootGraphicsSystem(memory 0x%llx, 0x%llx): bypassed (NVN in HLE)",
         (unsigned long long)A(0), (unsigned long long)A(1));
}
void gs_nop(CPUState*) {}
// nn::camera (webcam USB, via gsdrv) : bibliothèque initialisée, mais aucun périphérique branché.
// Le moteur traite 0x7c4 (module 196, description 3) comme « No camera device detected. ».
// (0xdc4, « plateforme sans caméra », mène à un chemin non éprouvé qui déréférence un pointeur nul.)
constexpr uint32_t CAM_NO_DEVICE = 0x7c4;
void camera_ok(CPUState* ctx) { R(ctx, 0); }
void camera_no_device(CPUState* ctx) { R(ctx, CAM_NO_DEVICE); }
void camera_init(CPUState* ctx) {
    HLOG("gfx", "nn::camera::Initialize: no camera connected");
    R(ctx, 0);
}
void camera_conn_event(CPUState* ctx) {   // (SystemEventType*) : jamais signalé
    hle_make_system_event(ctx, A(0));
    R(ctx, 0);
}
void gs_true(CPUState* ctx) { R(ctx, 1); }

// ================================================================ nn::vi
alignas(16) uint8_t g_vi_display[0x100], g_vi_layer[0x100], g_native_window[0x100];

void vi_ok(CPUState* ctx) { R(ctx, 0); }
void vi_open_default_display(CPUState* ctx) {
    *P<uint64_t>(A(0)) = (uint64_t)g_vi_display;
    HLOG("gfx", "nn::vi::OpenDefaultDisplay");
    R(ctx, 0);
}
void vi_create_layer(CPUState* ctx) {
    *P<uint64_t>(A(0)) = (uint64_t)g_vi_layer;
    HLOG("gfx", "nn::vi::CreateLayer");
    R(ctx, 0);
}
void vi_get_native_window(CPUState* ctx) {
    *P<uint64_t>(A(0)) = (uint64_t)g_native_window;
    R(ctx, 0);
}
// Résolution d'affichage émulée : RECOMP_RESOLUTION = 720 | 1080 (défaut) | 1440 | 2160.
// Index = valeur de nn::vi::BufferMaxResolution (table du moteur : 720p, 1080p, 1440p, 2160p).
const uint32_t kDisplayW[4] = {1280, 1920, 2560, 3840}, kDisplayH[4] = {720, 1080, 1440, 2160};
int display_mode() {
    static int m = [] {
        int v = settings().resolution;   // recomp.ini : resolution (0 = auto)
        if (v <= 0) {   // auto : plus grand mode qui tient dans l'écran principal de l'hôte
            int sw = 0, sh = 0;
            gfx_screen_size(&sw, &sh);
            v = sw >= 3840 && sh >= 2160 ? 2160 : sw >= 2560 && sh >= 1440 ? 1440 : sw >= 1920 && sh >= 1080 ? 1080 : 720;
        }
        int mode = v >= 2160 ? 3 : v >= 1440 ? 2 : v >= 1080 ? 1 : 0;
        HLOG("gfx", "virtual display: %ux%u at %d Hz", kDisplayW[mode], kDisplayH[mode], settings().refresh_rate);
        return mode;
    }();
    return m;
}

void vi_list_displays(CPUState* ctx) {   // DisplayInfo (0x60 o) : nom[0x40], bool limite, i64 nb calques, i64 l, i64 h
    if (A(0) && (int32_t)A(1) > 0) {
        uint8_t* d = P<uint8_t>(A(0));
        memset(d, 0, 0x60);
        memcpy(d, "Default", 8);
        d[0x40] = 1;
        *(int64_t*)(d + 0x48) = 1;
        *(int64_t*)(d + 0x50) = kDisplayW[display_mode()];   // lu par le moteur : taille de l'écran
        *(int64_t*)(d + 0x58) = kDisplayH[display_mode()];
    }
    R(ctx, 1);
}
void vi_latest_frame(CPUState* ctx) {
    *P<uint64_t>(A(0)) = g_frame.load();
    R(ctx, 0);
}
void vi_zero_count(CPUState* ctx) { R(ctx, 0); }

// ---------------------------------------------------------------- écran virtuel (cadence)
// Fréquence annoncée au jeu : RECOMP_REFRESH (60 par défaut). Le régulateur de cadence du moteur
// cale ses présentations sur l'historique nn::vi::ListFrameHistory : sans lui il ajoute une
// période entière après le travail de l'image (≈ 20 ms → 50 img/s).
uint32_t refresh_hz() { return (uint32_t)settings().refresh_rate; }   // recomp.ini : refresh_rate
uint64_t vblank_period() { return rt_cntfrq() / refresh_hz(); }   // en ticks du compteur invité (31,25 MHz)
// FrameInfo (0x28 o) : u64 état (2 = affichée), u64 numéro d'image, u64 ?, Tick, Tick
struct FrameRec { uint64_t num = 0, queued = 0, shown = 0; };
std::mutex g_hist_lk;
FrameRec g_hist[16];
uint64_t g_hist_n = 0;
}  // namespace
uint64_t vi_vblank_period() { return vblank_period(); }
void vi_frame_displayed(uint64_t frame, uint64_t queued_tick, uint64_t shown_tick) {
    std::lock_guard<std::mutex> lk(g_hist_lk);
    g_hist[g_hist_n++ % 16] = {frame, queued_tick, shown_tick};
}
namespace {
void vi_frame_history(CPUState* ctx) {   // (FrameInfo* out, int count, Layer const*) → nombre
    // FrameInfo (0x28 o) : u64 état (2 = affichée), u64 numéro d'image, u64 0, Tick affichage, Tick fin.
    // Le régulateur du moteur compte les vblanks entre affichages successifs. RECOMP_FH_MODE=9 : vide.
    static const bool off = getenv("RECOMP_FH_MODE") && atoi(getenv("RECOMP_FH_MODE")) == 9;
    std::lock_guard<std::mutex> lk(g_hist_lk);
    uint64_t avail = off ? 0 : std::min<uint64_t>(g_hist_n, 16);
    int32_t n = (int32_t)A(1);
    if (!A(0) || n <= 0) {
        R(ctx, avail);
        return;
    }
    uint64_t k = std::min<uint64_t>((uint64_t)n, avail);
    uint64_t now = rt_cntvct(), per = vblank_period();
    uint64_t* o = P<uint64_t>(A(0));
    uint32_t written = 0;
    for (uint64_t i = 0; i < k; i++) {   // du plus ancien au plus récent
        const FrameRec& r = g_hist[(g_hist_n - k + i) % 16];
        if (r.shown > now) continue;   // pas encore affichée (vblank futur)
        uint64_t* e = o + 5 * written++;
        e[0] = 2, e[1] = r.num, e[2] = 0, e[3] = r.shown, e[4] = r.shown + per;
    }
    R(ctx, written);
}
void vi_120hz(CPUState* ctx) {   // (Display*, bool) : option 120 Hz du jeu
    HLOG("gfx", "nn::vi::SetDisplay120HzRefreshRateCapability(%d) (virtual display: %u Hz, RECOMP_REFRESH)",
         (int)(A(1) & 1), refresh_hz());
    R(ctx, 0);
}
void vi_swap_interval(CPUState* ctx) {   // (Layer*, int, int)
    static int n = 0;
    if (n++ < 20) HLOG("gfx", "nn::vi::SetLayerExtendedSwapInterval(%d, %d)", (int32_t)A(1), (int32_t)A(2));
    R(ctx, 0);
}
void vi_hdr_status(CPUState* ctx) {   // (bool*, HdrOutputFormat*, u16*, Display const*)
    if (A(0)) *P<uint8_t>(A(0)) = 0;
    if (A(1)) *P<uint32_t>(A(1)) = 0;
    if (A(2)) *P<uint16_t>(A(2)) = 0;
    R(ctx, 0);
}
void vi_paper_white(CPUState* ctx) {
    if (A(0)) *P<uint16_t>(A(0)) = 203;
    R(ctx, 0);
}
void vi_refresh_rate(CPUState* ctx) {   // nn::vi::RefreshRate : u32 en Hz (le moteur pré-remplit 60)
    if (A(0)) *P<uint32_t>(A(0)) = refresh_hz();
    R(ctx, 0);
}
void vi_max_resolution(CPUState* ctx) {   // nn::vi::BufferMaxResolution : énumération u32 (0..3)
    if (A(0)) *P<uint32_t>(A(0)) = (uint32_t)display_mode();
    R(ctx, 0);
}

// ================================================================ NVN : registre des fonctions
std::unordered_map<std::string, uint64_t> g_nvn_addr;   // nom → adresse native
std::mutex g_nvn_lock;

constexpr int NSTUB = 1024;
struct StubInfo {
    std::string name;
    std::atomic<uint64_t> calls{0};
};
StubInfo g_stub[NSTUB];
uint64_t g_stub_addr[NSTUB];
int g_nstub = 0;

void nvn_trace_impl(const char* name, CPUState* ctx, bool stub) {
    HLOG("nvn", "%s%s(0x%llx, 0x%llx, 0x%llx, 0x%llx) from %s [thread %u]", stub ? "[stub] " : "", name,
         (unsigned long long)A(0), (unsigned long long)A(1), (unsigned long long)A(2), (unsigned long long)A(3),
         symbolize(ctx->x[30]).c_str(), current_thread_id());
}

void stub_call(int i, CPUState* ctx) {
    uint64_t n = g_stub[i].calls++;
    if (g_trace_nvn) {
        nvn_trace_impl(g_stub[i].name.c_str(), ctx, true);
    } else if (n == 0 || g_verbose >= 2)
        HLOG("nvn", "[stub] %s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", g_stub[i].name.c_str(),
             (unsigned long long)A(0), (unsigned long long)A(1), (unsigned long long)A(2),
             (unsigned long long)A(3));
    R(ctx, 0);
}
template <int I>
void stub_fn(CPUState* ctx) { stub_call(I, ctx); }
template <int... I>
std::array<RecompFn, sizeof...(I)> make_stubs(std::integer_sequence<int, I...>) {
    return {{stub_fn<I>...}};
}

uint64_t nvn_resolve(const char* name) {
    if (!name || strncmp(name, "nvn", 3)) return 0;
    std::lock_guard<std::mutex> lk(g_nvn_lock);
    auto it = g_nvn_addr.find(name);
    if (it != g_nvn_addr.end()) return it->second;
    if (g_nstub >= NSTUB) {
        HLOG("nvn", "stub pool exhausted (%s)", name);
        return 0;
    }
    int i = g_nstub++;
    g_stub[i].name = name;
    rename_native(g_stub_addr[i], std::string("nvn:") + name + " (bouchon)");
    g_nvn_addr[name] = g_stub_addr[i];
    return g_stub_addr[i];
}

void nvn_bootstrap_loader(CPUState* ctx) {
    const char* n = S(A(0));
    uint64_t a = nvn_resolve(n);
    HLOG("nvn", "nvnBootstrapLoader(%s) → %s", n, a ? "native" : "missing");
    R(ctx, a);
}
void nvn_get_proc_address(CPUState* ctx) { R(ctx, nvn_resolve(S(A(1)))); }



// ---------------------------------------------------------------- device
NVN(nvnDeviceBuilderSetDefaults) { (void)ctx; }
static uint32_t g_dev_flags = 0;
NVN(nvnDeviceBuilderSetFlags) {
    g_dev_flags = (uint32_t)A(1);
    HLOG("nvn", "nvnDeviceBuilderSetFlags(0x%x)", g_dev_flags);
}
NVN(nvnDeviceBuilderGetFlags) { R(ctx, g_dev_flags); }
NVN(nvnDeviceInitialize) {
    HLOG("nvn", "nvnDeviceInitialize(0x%llx)", (unsigned long long)A(0));
    R(ctx, 1);
}
NVN(nvnDeviceFinalize) { (void)ctx; }
NVN(nvnDeviceSetDebugLabel) { (void)ctx; }
NVN(nvnDeviceGetProcAddress) { nvn_get_proc_address(ctx); }
NVN(nvnDeviceGetInteger) {   // (device, pname, int*)
    int32_t pname = (int32_t)A(1);
    static std::mutex m;
    static std::map<int32_t, int> seen;
    int32_t v = 0x100;   // défaut prudent (alignements, nombres de liaisons…)
    {
        std::lock_guard<std::mutex> lk(m);
        if (!seen.count(pname)) HLOG("nvn", "nvnDeviceGetInteger(%d) → %d (default value)", pname, v);
        seen[pname]++;
    }
    if (A(2)) *P<int32_t>(A(2)) = v;
}
NVN(nvnDeviceGetCurrentTimestampInNanoseconds) {
    R(ctx, (uint64_t)((unsigned __int128)rt_cntvct() * 1000000000ull / rt_cntfrq()));
}
NVN(nvnDeviceGetTimestampInNanoseconds) {   // (device, const NVNcounterData*)
    uint64_t v = A(1) ? *P<uint64_t>(A(1) + 8) : 0;
    static const long tl = getenv("RECOMP_TS_LOG") ? atol(getenv("RECOMP_TS_LOG")) : 0;
    if (tl && g_frame.load() >= (uint64_t)tl && g_frame.load() < (uint64_t)tl + 4)
        HLOG("ts", "frame %llu: counter %llx = %.3f ms (now %.3f ms)", (unsigned long long)g_frame.load(),
             (unsigned long long)A(1), v / 1e6, rt_cntvct() * 1e3 / rt_cntfrq());
    R(ctx, v);
}
NVN(nvnDeviceInstallDebugCallback) { (void)ctx; }
NVN(nvnDeviceSetWindowOriginMode) {   // 0 bas-gauche (convention GL), 1 haut-gauche
    g_origin_upper_left = A(1) == 1;
    HLOG("nvn", "window origin: %s", g_origin_upper_left ? "top-left" : "bottom-left");
}
NVN(nvnDeviceSetDepthMode) {   // 0 [-w, w] (GL), 1 [0, w]
    g_depth_zero_one = A(1) == 1;
    HLOG("nvn", "depth: %s", g_depth_zero_one ? "[0, 1]" : "[-1, 1]");
}

// ---------------------------------------------------------------- queue / sync
NVN(nvnQueueInitialize) {
    HLOG("nvn", "nvnQueueInitialize(0x%llx)", (unsigned long long)A(0));
    R(ctx, 1);
}
NVN(nvnQueueFinalize) { (void)ctx; }
NVN(nvnQueueFlush) { (void)ctx; }
NVN(nvnQueueFinish) { gpu_finish(); }
NVN(nvnQueueFenceSync) { gpu_fence(A(1)); }   // (queue, sync, condition, flags)
NVN(nvnQueueWaitSync) {                       // (queue, sync)
    gpu_wait_sync(A(1));
    R(ctx, 1);
}
NVN(nvnQueueGetError) { R(ctx, 0); }
NVN(nvnSyncInitialize) {   // (sync, device)
    sync_reset(A(0));
    R(ctx, 1);
}
NVN(nvnSyncFinalize) { (void)ctx; }
NVN(nvnSyncSetDebugLabel) { (void)ctx; }
NVN(nvnSyncWait) { R(ctx, (uint32_t)sync_wait(A(0), (int64_t)A(1))); }   // (sync, délai ns)

NVN(nvnQueuePresentTexture) {   // (queue, window, index)
    uint64_t f = ++g_frame;
    {   // diagnostic : RECOMP_FRAME_TRACE=N trace les attentes longues des images N à N+2
        static const long ft = getenv("RECOMP_FRAME_TRACE") ? atol(getenv("RECOMP_FRAME_TRACE")) : 0;
        if (ft && (f == (uint64_t)ft || f == (uint64_t)ft + 3)) {
            g_svc_timing = f == (uint64_t)ft;
        }
        if (ft && f >= (uint64_t)ft && f <= (uint64_t)ft + 3)
            HLOG("ftrace", "--- presenting frame %llu (thread %llu)", (unsigned long long)f,
                 (unsigned long long)current_thread_id());
    }
    Win& w = g_win[A(1)];
    int idx = (int32_t)A(2);
    uint64_t t = idx >= 0 && idx < (int)w.tex.size() ? w.tex[idx] : 0;
    float c[4] = {0, 0, 0, 1};
    if (t) memcpy(c, g_tex[t].clear, sizeof c);
    {
        static const long tl = getenv("RECOMP_TS_LOG") ? atol(getenv("RECOMP_TS_LOG")) : 0;
        if (tl && f >= (uint64_t)tl && f < (uint64_t)tl + 4)
            HLOG("ts", "presenting frame %llu at %.3f ms", (unsigned long long)f, rt_cntvct() * 1e3 / rt_cntfrq());
    }
    gpu_present(t ? obj_key(t) : 0, f, w.crop, c);   // exécutée par le thread GPU, dans l'ordre
    if (f == 1 || f == 2 || f == 10 || f % 600 == 0)
        HLOG("nvn", "frame %llu presented (texture %d/%zu, color %.2f %.2f %.2f)",
             (unsigned long long)f, idx, w.tex.size(), c[0], c[1], c[2]);
}

// ---------------------------------------------------------------- pools / buffers
NVN(nvnMemoryPoolBuilderSetDefaults) { g_poolb.create(A(0)); }
NVN(nvnMemoryPoolBuilderSetDevice) { (void)ctx; }
NVN(nvnMemoryPoolBuilderSetStorage) {   // (builder, void* memory, size_t size)
    auto& b = g_poolb[A(0)];
    b.storage = A(1);
    b.size = A(2);
}
NVN(nvnMemoryPoolBuilderSetFlags) { g_poolb[A(0)].flags = (uint32_t)A(1); }
NVN(nvnMemoryPoolBuilderGetMemory) { R(ctx, g_poolb[A(0)].storage); }
NVN(nvnMemoryPoolBuilderGetSize) { R(ctx, g_poolb[A(0)].size); }
NVN(nvnMemoryPoolBuilderGetFlags) { R(ctx, g_poolb[A(0)].flags); }
NVN(nvnMemoryPoolInitialize) {   // (pool, builder)
    PoolBuilder b = g_poolb[A(1)];
    g_pool.create(A(0), Pool{b.storage, b.size, b.flags});
    static const bool plog = getenv("RECOMP_POOL_LOG") != nullptr;
    if (plog)
        HLOG("nvn", "pool 0x%llx created: memory 0x%llx, 0x%llx bytes, flags 0x%x", (unsigned long long)A(0),
             (unsigned long long)b.storage, (unsigned long long)b.size, b.flags);
    R(ctx, 1);
}
NVN(nvnMemoryPoolFinalize) {
    static const bool plog = getenv("RECOMP_POOL_LOG") != nullptr;
    Pool p;
    const bool known = g_pool.find(A(0), &p);
    if (plog && known)
        HLOG("nvn", "pool 0x%llx destroyed: memory 0x%llx, 0x%llx bytes, flags 0x%x", (unsigned long long)A(0),
             (unsigned long long)p.storage, (unsigned long long)p.size, p.flags);
    g_pool.erase(A(0));
    // Imports Vulkan de cette mémoire retirés : un pool créé plus tard aux mêmes adresses (mémoire démappée puis
    // remappée au changement de niveau) doit être réimporté, sinon le GPU lit les anciennes pages.
    if (known && p.storage && p.size) gpu_pool_released(p.storage, p.size);
}
NVN(nvnMemoryPoolSetDebugLabel) { (void)ctx; }
NVN(nvnMemoryPoolMap) { R(ctx, g_pool[A(0)].storage); }
// (pool, décalage, taille) : le jeu annonce qu'il a écrit cette plage du pool. Sur la console le GPU lit la mémoire
// du jeu ; ici les textures sont copiées en mémoire vidéo : celles que la plage touche sont revérifiées (vk_render.cpp).
NVN(nvnMemoryPoolFlushMappedRange) {
    Pool p;
    if (g_pool.find(A(0), &p) && p.storage && A(2)) vkr::pool_flushed(p.storage + A(1), A(2));
}
NVN(nvnMemoryPoolInvalidateMappedRange) { (void)ctx; }
NVN(nvnMemoryPoolGetBufferAddress) { R(ctx, g_pool[A(0)].storage); }   // adresse GPU = adresse CPU
NVN(nvnMemoryPoolGetSize) { R(ctx, g_pool[A(0)].size); }
NVN(nvnMemoryPoolGetFlags) { R(ctx, g_pool[A(0)].flags); }

NVN(nvnBufferBuilderSetDefaults) { g_bufb.create(A(0)); }
NVN(nvnBufferBuilderSetDevice) { (void)ctx; }
NVN(nvnBufferBuilderSetStorage) {   // (builder, pool, offset, size)
    g_bufb[A(0)] = BufBuilder{A(1), A(2), A(3)};
}
NVN(nvnBufferBuilderGetMemoryPool) { R(ctx, g_bufb[A(0)].pool); }
NVN(nvnBufferBuilderGetMemoryOffset) { R(ctx, g_bufb[A(0)].offset); }
NVN(nvnBufferBuilderGetSize) { R(ctx, g_bufb[A(0)].size); }
NVN(nvnBufferInitialize) {   // (buffer, builder)
    BufBuilder b = g_bufb[A(1)];
    uint64_t addr = pool_ptr(b.pool, b.offset);
    g_buf.create(A(0), Buf{addr, b.size});
    if (!addr) {
        Pool pl;
        bool known = g_pool.find(b.pool, &pl);
        HLOG("nvn", "nvnBufferInitialize(0x%llx): pool 0x%llx %s (memory 0x%llx, flags 0x%x), "
             "offset 0x%llx, size 0x%llx → null address", (unsigned long long)A(0),
             (unsigned long long)b.pool, known ? "known" : "UNKNOWN", (unsigned long long)pl.storage, pl.flags,
             (unsigned long long)b.offset, (unsigned long long)b.size);
    }
    R(ctx, 1);
}
NVN(nvnBufferFinalize) { g_buf.erase(A(0)); }
NVN(nvnBufferSetDebugLabel) { (void)ctx; }
NVN(nvnBufferMap) { R(ctx, g_buf[A(0)].addr); }
NVN(nvnBufferGetAddress) { R(ctx, g_buf[A(0)].addr); }
NVN(nvnBufferGetSize) { R(ctx, g_buf[A(0)].size); }
NVN(nvnBufferFlushMappedRange) { (void)ctx; }
NVN(nvnBufferInvalidateMappedRange) { (void)ctx; }

// ---------------------------------------------------------------- textures
NVN(nvnTextureBuilderSetDefaults) { g_texb.create(A(0)); }
NVN(nvnTextureBuilderSetDevice) { (void)ctx; }
NVN(nvnTextureBuilderSetFlags) { g_texb[A(0)].flags = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetTarget) { g_texb[A(0)].target = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetWidth) { g_texb[A(0)].w = (int32_t)A(1); }
NVN(nvnTextureBuilderSetHeight) { g_texb[A(0)].h = (int32_t)A(1); }
NVN(nvnTextureBuilderSetDepth) { g_texb[A(0)].d = (int32_t)A(1); }
NVN(nvnTextureBuilderSetSize1D) { g_texb[A(0)].w = (int32_t)A(1); }
NVN(nvnTextureBuilderSetSize2D) {
    auto& b = g_texb[A(0)];
    b.w = (int32_t)A(1);
    b.h = (int32_t)A(2);
}
NVN(nvnTextureBuilderSetSize3D) {
    auto& b = g_texb[A(0)];
    b.w = (int32_t)A(1);
    b.h = (int32_t)A(2);
    b.d = (int32_t)A(3);
}
NVN(nvnTextureBuilderSetLevels) { g_texb[A(0)].levels = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetFormat) { g_texb[A(0)].format = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetSamples) { g_texb[A(0)].samples = (uint32_t)A(1); }
NVN(nvnTextureBuilderSetStorage) {   // (builder, pool, offset)
    auto& b = g_texb[A(0)];
    b.pool = A(1);
    b.offset = A(2);
}
NVN(nvnTextureBuilderSetStride) { g_texb[A(0)].stride = (int64_t)A(1); }
NVN(nvnTextureBuilderSetSwizzle) {   // (builder, r, g, b, a)
    auto& b = g_texb[A(0)];
    for (int i = 0; i < 4; i++) b.swizzle[i] = (uint8_t)A(1 + i);
}
NVN(nvnTextureBuilderSetPackagedTextureLayout) {   // (builder, const NVNpackagedTextureLayout*) : 8 octets
    auto& b = g_texb[A(0)];
    if (A(1)) {
        memcpy(b.layout, (const void*)A(1), 8);
        b.packaged = true;
    } else {
        memset(b.layout, 0, 8);
        b.packaged = false;
    }
}
NVN(nvnTextureGetFlags) { R(ctx, g_tex[A(0)].b.flags); }
NVN(nvnTextureGetStride) { R(ctx, (uint64_t)g_tex[A(0)].b.stride); }
NVN(nvnTextureGetZCullStorageSize) { R(ctx, 0); }
NVN(nvnTextureBuilderGetWidth) { R(ctx, (uint32_t)g_texb[A(0)].w); }
NVN(nvnTextureBuilderGetHeight) { R(ctx, (uint32_t)g_texb[A(0)].h); }
NVN(nvnTextureBuilderGetDepth) { R(ctx, (uint32_t)g_texb[A(0)].d); }
NVN(nvnTextureBuilderGetLevels) { R(ctx, g_texb[A(0)].levels); }
NVN(nvnTextureBuilderGetFormat) { R(ctx, g_texb[A(0)].format); }
NVN(nvnTextureBuilderGetTarget) { R(ctx, g_texb[A(0)].target); }
NVN(nvnTextureBuilderGetStorageSize) {
    // format inconnu : 8 octets par texel, ×2 si mipmaps (surestimation sans danger)
    TexBuilder b = g_texb[A(0)];
    uint64_t n = (uint64_t)std::max(b.w, 1) * std::max(b.h, 1) * std::max(b.d, 1) * 8;
    if (b.levels > 1) n *= 2;
    if (b.samples > 1) n *= b.samples;
    R(ctx, (n + 0xFFFF) & ~0xFFFFull);
}
NVN(nvnTextureBuilderGetStorageAlignment) { R(ctx, 0x10000); }
NVN(nvnTextureInitialize) {   // (texture, builder)
    TexBuilder b = g_texb[A(1)];
    Tex& t = g_tex.create(A(0));
    t.b = b;
    t.addr = pool_ptr(b.pool, b.offset);
    HDBG("nvn", "texture 0x%llx: %dx%dx%d, format %u, target %u", (unsigned long long)A(0), b.w, b.h,
         b.d, b.format, b.target);
    static const bool texlog = getenv("RECOMP_TEX_LOG") != nullptr;
    if (texlog)
        HLOG("tex", "init %llx @%llx %dx%dx%d lvl %u fmt %u target %u flags 0x%x stride %lld swz %u%u%u%u %s %02x%02x%02x%02x%02x%02x%02x%02x",
             (unsigned long long)obj_key(A(0)), (unsigned long long)t.addr, b.w, b.h, b.d, b.levels, b.format, b.target,
             b.flags, (long long)b.stride, b.swizzle[0], b.swizzle[1], b.swizzle[2], b.swizzle[3],
             b.packaged ? "packaged" : "-", b.layout[0], b.layout[1], b.layout[2], b.layout[3], b.layout[4],
             b.layout[5], b.layout[6], b.layout[7]);
    R(ctx, 1);
}
NVN(nvnTextureFinalize) {
    const uint64_t k = obj_key(A(0));
    const bool known = g_tex.find(A(0), nullptr);
    g_tex.erase(A(0));
    if (known && k) gpu_texture_released(k);   // l'image Vulkan perd une référence, dans l'ordre de la file GPU
}
NVN(nvnTextureSetDebugLabel) { (void)ctx; }
NVN(nvnTextureGetWidth) { R(ctx, (uint32_t)g_tex[A(0)].b.w); }
NVN(nvnTextureGetHeight) { R(ctx, (uint32_t)g_tex[A(0)].b.h); }
NVN(nvnTextureGetDepth) { R(ctx, (uint32_t)g_tex[A(0)].b.d); }
NVN(nvnTextureGetFormat) { R(ctx, g_tex[A(0)].b.format); }
NVN(nvnTextureGetTarget) { R(ctx, g_tex[A(0)].b.target); }
NVN(nvnTextureGetLevels) { R(ctx, g_tex[A(0)].b.levels); }
NVN(nvnTextureGetTextureAddress) { R(ctx, g_tex[A(0)].addr); }

// ---------------------------------------------------------------- fenêtres
NVN(nvnWindowBuilderSetDefaults) { g_winb.create(A(0)); }
NVN(nvnWindowBuilderSetDevice) { (void)ctx; }
NVN(nvnWindowBuilderSetNativeWindow) { g_winb[A(0)].native = A(1); }
NVN(nvnWindowBuilderSetTextures) {   // (builder, int n, NVNtexture* const* textures)
    auto& b = g_winb[A(0)];
    b.tex.clear();
    for (int i = 0; i < (int32_t)A(1); i++) b.tex.push_back(P<uint64_t>(A(2))[i]);
}
NVN(nvnWindowBuilderSetPresentInterval) { g_winb[A(0)].interval = (int32_t)A(1); }
NVN(nvnWindowBuilderGetNumTextures) { R(ctx, (uint32_t)g_winb[A(0)].tex.size()); }
NVN(nvnWindowInitialize) {   // (window, builder)
    WinBuilder b = g_winb[A(1)];
    Win& w = g_win.create(A(0));
    w.tex = b.tex;
    w.interval = b.interval;
    w.active = (int32_t)b.tex.size();
    if (!b.tex.empty()) {
        Tex& t = g_tex[b.tex[0]];
        gfx_open_window(t.b.w > 1 ? t.b.w : 1280, t.b.h > 1 ? t.b.h : 720);
        HLOG("nvn", "nvnWindowInitialize: %zu textures %dx%d (format %u)", b.tex.size(), t.b.w, t.b.h, t.b.format);
    }
    R(ctx, 1);
}
NVN(nvnWindowFinalize) { g_win.erase(A(0)); }
NVN(nvnWindowSetDebugLabel) { (void)ctx; }
NVN(nvnWindowAcquireTexture) {   // (window, sync, int* index) → NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS
    gpu_acquire_wait();             // au plus N présentations en file
    if (A(1)) sync_signal_now(A(1));
    Win& w = g_win[A(0)];
    int n = w.active > 0 ? w.active : 1;
    if (A(2)) *P<int32_t>(A(2)) = w.next;
    w.next = (w.next + 1) % n;
    R(ctx, 0);
}
NVN(nvnWindowGetNumTextures) { R(ctx, (uint32_t)g_win[A(0)].tex.size()); }
NVN(nvnWindowGetNumActiveTextures) { R(ctx, (uint32_t)g_win[A(0)].active); }
NVN(nvnWindowSetNumActiveTextures) { g_win[A(0)].active = (int32_t)A(1); }
NVN(nvnWindowGetPresentInterval) { R(ctx, (uint32_t)g_win[A(0)].interval); }
NVN(nvnWindowSetPresentInterval) { g_win[A(0)].interval = (int32_t)A(1); }
NVN(nvnWindowSetCrop) {   // (window, x, y, w, h) : zone de la texture présentée
    Win& w = g_win[A(0)];
    w.crop[0] = (int32_t)A(1), w.crop[1] = (int32_t)A(2), w.crop[2] = (int32_t)A(3), w.crop[3] = (int32_t)A(4);
    HLOG("nvn", "window: crop %d,%d %dx%d", w.crop[0], w.crop[1], w.crop[2], w.crop[3]);
}

}  // namespace

// ---------------------------------------------------------------- état NVN partagé (nvn.h)
std::map<std::string, RecompFn>& nvn_impls() {
    static std::map<std::string, RecompFn> m;
    return m;
}
void nvn_trace(const char* name, CPUState* ctx, bool stub) { nvn_trace_impl(name, ctx, stub); }
namespace nvn {
std::atomic<uint32_t> g_obj_id{1};
Table<PoolBuilder> g_poolb;
Table<Pool> g_pool;
Table<BufBuilder> g_bufb;
Table<Buf> g_buf;
Table<TexBuilder> g_texb;
Table<Tex> g_tex;
Table<WinBuilder> g_winb;
Table<Win> g_win;
std::atomic<uint64_t> g_frame{0};
// Profondeur [0, 1] par défaut : le jeu n'appelle pas nvnDeviceSetDepthMode et sa projection
// (Z inversé infini, z/w dans ]0, 1]) n'a de sens qu'avec cette convention ; [-1, 1] la ramenait à
// [0,5, 1]. Origine : réglée par le jeu (haut-gauche).
bool g_origin_upper_left = false, g_depth_zero_one = true;
uint64_t pool_ptr(uint64_t pool, uint64_t off) {
    Pool& p = g_pool[pool];
    return p.storage ? p.storage + off : 0;
}
}  // namespace nvn

// ================================================================ initialisation
void gfx_init() {
    static const auto stubs = make_stubs(std::make_integer_sequence<int, NSTUB>{});
    for (int i = 0; i < NSTUB; i++) g_stub_addr[i] = register_native("nvn:bouchon libre", stubs[i]);
    for (auto& [n, f] : nvn_impls()) g_nvn_addr[n] = register_native(("nvn:" + n).c_str(), f);
    HLOG("gfx", "NVN: %zu native functions, %d stubs in reserve", nvn_impls().size(), NSTUB);

    struct O { const char* sym; RecompFn fn; };
    const O ov[] = {
        {"_ZN2nn2gs3sdk18BootGraphicsSystemEPvm", gs_boot},
        {"_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm", gs_nop},
        {"_ZN2nn6camera10InitializeEv", camera_init},
        {"_ZN2nn6camera8FinalizeEv", camera_ok},
        {"_ZN2nn6camera8ActivateEv", camera_ok},
        {"_ZN2nn6camera10DeactivateEv", camera_ok},
        {"_ZN2nn6camera31GetDeviceConnectionChangedEventEPNS_2os15SystemEventTypeE", camera_conn_event},
        {"_ZN2nn6camera21QueryDeviceConnectionERKNS0_13DeviceRequestE", camera_no_device},
        {"_ZN2nn6camera10OpenCameraEPNS0_12DeviceHandleERKNS0_13DeviceRequestE", camera_no_device},   // gs court-circuité : toujours prêt
        {"_ZN2nn2gs14WaitNativeSyncERKNS0_16NativeSyncHandleE", gs_nop},
        {"_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", gs_true},
        {"_ZN2nn2gs19TimedWaitNativeSyncERKNS0_16NativeSyncHandleENS_8TimeSpanE", gs_true},
        {"nvnBootstrapLoader", nvn_bootstrap_loader},
        {"_ZN2nn2vi10InitializeEv", vi_ok},
        {"_ZN2nn2vi8FinalizeEv", vi_ok},
        {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open_default_display},
        {"_ZN2nn2vi12CloseDisplayEPNS0_7DisplayE", vi_ok},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_create_layer},
        {"_ZN2nn2vi12DestroyLayerEPNS0_5LayerE", vi_ok},
        {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_get_native_window},
        {"_ZN2nn2vi12ListDisplaysEPNS0_11DisplayInfoEi", vi_list_displays},
        {"_ZN2nn2vi16ListFrameHistoryEPNS0_9FrameInfoEiPKNS0_5LayerE", vi_frame_history},
        {"_ZN2nn2vi20GetLatestFrameNumberEPmPKNS0_5LayerE", vi_latest_frame},
        {"_ZN2nn2vi21QueryDisplayHdrStatusEPbPNS0_15HdrOutputFormatEPtPKNS0_7DisplayE", vi_hdr_status},
        {"_ZN2nn2vi25QueryDisplayHdrPaperWhiteEPtPKNS0_7DisplayE", vi_paper_white},
        {"_ZN2nn2vi28SetLayerExtendedSwapIntervalEPNS0_5LayerEii", vi_swap_interval},
        {"_ZN2nn2vi30ClearLayerExtendedSwapIntervalEPNS0_5LayerE", vi_ok},
        {"_ZN2nn2vi30QueryDisplayCurrentRefreshRateEPNS0_11RefreshRateEPKNS0_7DisplayE", vi_refresh_rate},
        {"_ZN2nn2vi31QueryDisplayBufferMaxResolutionEPNS0_19BufferMaxResolutionEPKNS0_7DisplayE", vi_max_resolution},
        {"_ZN2nn2vi36SetDisplay120HzRefreshRateCapabilityEPNS0_7DisplayEb", vi_120hz},
        {"_ZN2nn2vi52SetLayerNextHdrExtendedContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_ok},
        {"_ZN2nn2vi52SetLayerNextHdrStandardContentLuminanceRangeMetadataEPNS0_5LayerEtt", vi_ok},
    };
    int n = 0;
    for (auto& o : ov) n += override_export(o.sym, o.fn);
    HLOG("gfx", "%d gs/vi/nvn functions replaced", n);
}

}  // namespace hos
