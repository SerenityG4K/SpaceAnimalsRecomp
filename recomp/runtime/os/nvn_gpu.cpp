// File d'exécution « GPU » : les soumissions NVN (listes de commandes, présentations, signaux de
// syncs) sont exécutées dans l'ordre par un thread hôte dédié, comme le ferait le GPU de la console.
// Le thread du jeu qui soumet n'attend plus la traduction Vulkan (≈ 3 ms par image) : le régulateur
// de cadence du moteur, qui ne prévoit pas ce temps, visait sinon ≈ 20 ms par image.
//
// Syncs NVN : un objet est « en attente » entre sa soumission dans un FenceSync et la fin, sur le GPU, du
// travail Vulkan soumis avant ce FenceSync. Le thread GPU soumet sans attendre (vkr::flush_async) ; le fil
// de complétion de vk_render.cpp signale le sync quand le sémaphore chronologique atteint la soumission.
#include <windows.h>

#include "nvn_exec.h"
#include "gfx.h"
#include "vk_render.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

namespace hos {
namespace nvn {
extern std::atomic<uint64_t> g_exec_ns;
void perf_report_ops();
namespace {

struct SyncState {
    uint64_t want = 0, done = 0;   // FenceSync soumis / exécutés
};
std::mutex g_sync_lk;
std::condition_variable g_sync_cv;
std::unordered_map<uint64_t, SyncState> g_syncs;   // adresse de l'objet NVNsync → état

struct Item {
    enum Kind { Cmds, Present, Fence, Wait, Flush, PoolReleased, TextureReleased } k = Cmds;
    std::shared_ptr<std::vector<Op>> ops;
    uint64_t a = 0, b = 0, q = 0;   // q : tick de mise en file (présentation)
    int32_t crop[4] = {0, 0, 0, 0};
    float clear[4] = {0, 0, 0, 1};
};
std::mutex g_q_lk;
std::condition_variable g_q_cv, g_idle_cv;
std::deque<Item> g_q;
bool g_busy = false;
int g_presents = 0;   // présentations en file, pas encore exécutées
std::deque<uint64_t> g_disp;   // vblanks d'affichage prévus (images présentées, pas encore à l'écran)
uint64_t g_last_vb = 0;
std::once_flag g_started;
// RECOMP_PERF : temps cumulés (µs) depuis le dernier rapport
std::atomic<uint64_t> g_t_sync{0}, g_t_acq{0}, g_t_busy{0}, g_t_flush{0}, g_n_sync{0}, g_n_flush{0};
inline uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

int max_in_flight() {
    static const int n = [] {
        const char* e = getenv("RECOMP_FRAMES_IN_FLIGHT");
        int v = e ? atoi(e) : 2;
        return v >= 1 && v <= 4 ? v : 2;
    }();
    return n;
}

// Attente précise jusqu'à un tick (minuteur haute résolution, puis attente active < 0,3 ms).
void wait_until_tick(uint64_t tick) {
    static thread_local HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const uint64_t f = rt_cntfrq();
    for (;;) {
        uint64_t now = rt_cntvct();
        if (now >= tick) return;
        int64_t ns = (int64_t)((tick - now) * 1000000000ull / f);
        if (ns > 300000 && tm) {
            LARGE_INTEGER due;
            due.QuadPart = -((ns - 250000) / 100);
            if (SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(tm, INFINITE);
        } else {
            YieldProcessor();
        }
    }
}

void signal_done(uint64_t sync, bool force) {
    std::lock_guard<std::mutex> lk(g_sync_lk);
    auto& s = g_syncs[sync];
    if (force) s.done = s.want;
    else if (s.done < s.want) s.done++;
    g_sync_cv.notify_all();
}

void run_item(Item& it) {
    switch (it.k) {
    case Item::Cmds:
        nvn_execute(*it.ops);
        break;
    case Item::Fence: {   // nvnQueueFenceSync : signalé quand tout le travail précédent est terminé (GPU compris)
        uint64_t t0 = now_us();
        const uint64_t v = vkr::flush_async();
        g_t_flush += now_us() - t0, g_n_flush++;
        const uint64_t sy = it.a;
        vkr::when_done(v, [sy] { signal_done(sy, false); });
        break;
    }
    case Item::Wait:    // nvnQueueWaitSync
        sync_wait(it.a, 100000000);
        break;
    case Item::Flush:   // nvnQueueFinish : travail Vulkan enregistré soumis et terminé
        vkr::flush();
        break;
    case Item::PoolReleased:   // nvnMemoryPoolFinalize : imports Vulkan de cette mémoire retirés (vk_render.cpp)
        vkr::pools_released(it.a, it.b);
        break;
    case Item::TextureReleased:   // nvnTextureFinalize : l'image perd une référence (vk_render.cpp)
        vkr::texture_released(it.a);
        break;
    case Item::Present: {
        // Écran virtuel (RECOMP_REFRESH, 60 Hz par défaut) : comme une file FIFO de la console, l'image
        // s'affiche au premier vblank libre ; l'historique (nn::vi::ListFrameHistory) rapporte cet instant.
        // Le GPU n'attend pas le vblank : seul l'affichage y est calé. La contre-pression vient de
        // l'acquisition d'une texture de fenêtre (gpu_acquire_wait), comme sur la console.
        uint64_t per = vi_vblank_period(), now = rt_cntvct();
        uint64_t vb = (now / per + 1) * per;
        {
            std::lock_guard<std::mutex> lk(g_q_lk);
            if (vb <= g_last_vb) vb = g_last_vb + per;
            g_last_vb = vb;
            g_disp.push_back(vb);
        }
        if (!vkr::present(it.a, it.b, it.crop)) gfx_present(it.clear, it.b);
        else gfx_count_frame(it.b);
        vi_frame_displayed(it.b, it.q, vb);
        break;
    }
    }
}

void gpu_thread() {
    for (;;) {
        Item it;
        {
            std::unique_lock<std::mutex> lk(g_q_lk);
            g_q_cv.wait(lk, [] { return !g_q.empty(); });
            it = std::move(g_q.front());
            g_q.pop_front();
            g_busy = true;
        }
        uint64_t t0 = now_us();
        run_item(it);
        g_t_busy += now_us() - t0;
        if (it.k == Item::Present && getenv("RECOMP_PERF") && it.b % 120 == 0) {
            HLOG("perf", "nvn_execute : %.2f ms/image", g_exec_ns.exchange(0) / 120e6);
            perf_report_ops();
            HLOG("perf", "thread GPU : occupé %.2f ms/image (dont vidages %.2f, %llu), syncs attendus %.2f ms/image (%llu), acquisition %.2f ms/image",
                 g_t_busy.exchange(0) / 120e3, g_t_flush.exchange(0) / 120e3, (unsigned long long)g_n_flush.exchange(0),
                 g_t_sync.exchange(0) / 120e3, (unsigned long long)g_n_sync.exchange(0), g_t_acq.exchange(0) / 120e3);
        }
        {
            std::lock_guard<std::mutex> lk(g_q_lk);
            if (it.k == Item::Present) g_presents--;
            g_busy = false;
            g_idle_cv.notify_all();
        }
    }
}

void push(Item&& it) {
    std::call_once(g_started, [] {
        std::thread(gpu_thread).detach();
        HLOG("nvn", "thread GPU démarré (%d images en vol au plus)", max_in_flight());
    });
    std::lock_guard<std::mutex> lk(g_q_lk);
    if (it.k == Item::Present) g_presents++;
    g_q.push_back(std::move(it));
    g_q_cv.notify_one();
}

}  // namespace

// ---------------------------------------------------------------- syncs
void sync_reset(uint64_t sync) {
    std::lock_guard<std::mutex> lk(g_sync_lk);
    g_syncs[sync] = SyncState{};
}
void sync_signal_now(uint64_t sync) { signal_done(sync, true); }
void sync_fence_executed(uint64_t sync) { signal_done(sync, false); }
int sync_wait(uint64_t sync, int64_t timeout_ns) {   // NVNsyncWaitResult
    std::unique_lock<std::mutex> lk(g_sync_lk);
    auto ready = [&] {
        auto it = g_syncs.find(sync);
        return it == g_syncs.end() || it->second.done >= it->second.want;
    };
    if (ready()) return 0;   // ALREADY_SIGNALED
    if (timeout_ns == 0) return 2;
    uint64_t t0 = now_us();
    struct Acc { uint64_t t0; ~Acc() { g_t_sync += now_us() - t0, g_n_sync++; } } acc{t0};
    if (timeout_ns < 0 || (uint64_t)timeout_ns > 3600ull * 1000000000ull) {
        g_sync_cv.wait(lk, ready);
        return 1;   // CONDITION_SATISFIED
    }
    return g_sync_cv.wait_for(lk, std::chrono::nanoseconds(timeout_ns), ready) ? 1 : 2;
}

// ---------------------------------------------------------------- file GPU
void gpu_submit(std::shared_ptr<std::vector<Op>> ops) {
    {   // les FenceSync de la liste deviennent « en attente » dès la soumission
        std::lock_guard<std::mutex> lk(g_sync_lk);
        for (const Op& o : *ops)
            if (o.t == OpType::FenceSync && o.a[0]) g_syncs[o.a[0]].want++;
    }
    Item it;
    it.k = Item::Cmds;
    it.ops = std::move(ops);
    push(std::move(it));
}
void gpu_fence(uint64_t sync) {
    {
        std::lock_guard<std::mutex> lk(g_sync_lk);
        g_syncs[sync].want++;
    }
    Item it;
    it.k = Item::Fence;
    it.a = sync;
    push(std::move(it));
}
void gpu_wait_sync(uint64_t sync) {
    Item it;
    it.k = Item::Wait;
    it.a = sync;
    push(std::move(it));
}
void gpu_present(uint64_t texkey, uint64_t frame, const int32_t* crop, const float* clear) {
    Item it;
    it.k = Item::Present;
    it.a = texkey, it.b = frame, it.q = rt_cntvct();
    if (crop) memcpy(it.crop, crop, sizeof it.crop);
    if (clear) memcpy(it.clear, clear, sizeof it.clear);
    push(std::move(it));
}
// nvnQueueFinish : la file traduite ne suffit pas, le travail Vulkan enregistré attendait le vidage suivant
// (FenceSync ou présentation). Le jeu, le croyant fini, réutilisait sa mémoire de transfert : la copie des
// textures 1×1 par défaut lisait d'autres données (texte en pavés). Un élément Flush soumet et attend.
void gpu_finish() {
    static const bool old = getenv("RECOMP_FINISH_NO_FLUSH") != nullptr;   // diagnostic : ancien comportement
    if (!old) {
        Item it;
        it.k = Item::Flush;
        push(std::move(it));
    }
    std::unique_lock<std::mutex> lk(g_q_lk);
    g_idle_cv.wait(lk, [] { return g_q.empty() && !g_busy; });
}
// Pool libéré par le jeu : ses imports Vulkan sont retirés dans l'ordre de la file, après les commandes déjà
// soumises qui pouvaient encore le lire (vkr::pools_released).
void gpu_pool_released(uint64_t addr, uint64_t size) {
    Item it;
    it.k = Item::PoolReleased;
    it.a = addr, it.b = size;
    push(std::move(it));
}
void gpu_texture_released(uint64_t texkey) {
    Item it;
    it.k = Item::TextureReleased;
    it.a = texkey;
    push(std::move(it));
}
// Contre-pression de la fenêtre : au plus N présentations en file (sinon nvnWindowAcquireTexture attend).
// Une texture de fenêtre se libère quand l'image suivante s'affiche : au plus N images en attente
// d'affichage (en file GPU ou planifiées sur un vblank futur), sinon on attend le prochain vblank.
void gpu_acquire_wait() {
    static const bool vsync = !getenv("RECOMP_NO_VSYNC_EMU");
    uint64_t t0 = now_us();
    struct Acc { uint64_t t0; ~Acc() { g_t_acq += now_us() - t0; } } acc{t0};
    for (;;) {
        uint64_t wake = 0;
        {
            std::unique_lock<std::mutex> lk(g_q_lk);
            g_idle_cv.wait(lk, [] { return g_presents < max_in_flight(); });
            if (!vsync) return;
            uint64_t now = rt_cntvct();
            while (!g_disp.empty() && g_disp.front() <= now) g_disp.pop_front();
            if ((int)g_disp.size() + g_presents < max_in_flight()) return;
            wake = g_disp.front();
        }
        wait_until_tick(wake);
    }
}

}  // namespace nvn
}  // namespace hos
