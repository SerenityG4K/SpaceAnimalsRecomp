// Présentation hôte provisoire : fenêtre Win32 (GDI) qui affiche la couleur d'effacement de la
// dernière image présentée par le jeu et le compteur d'images. Sert à vérifier que la boucle de
// rendu tourne ; sera remplacée par le moteur de rendu (Vulkan) quand les commandes NVN seront
// traduites. Fermer la fenêtre termine le processus.
#include "gfx.h"
#include "settings.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace hos {
namespace vkr {
void save_caches();   // vk_render.cpp : écrit pipelines.cache
}
namespace {

std::once_flag g_once;
HWND g_hwnd = nullptr;
std::atomic<bool> g_vulkan{false};
std::mutex g_lk;
float g_color[4] = {0, 0, 0, 1};
uint64_t g_frame = 0;
int g_w = 1280, g_h = 720;
std::chrono::steady_clock::time_point g_last;
std::chrono::steady_clock::time_point g_fps_t0;
uint64_t g_fps_f0 = 0;
double g_fps = 0;

// Mise à l'échelle de Windows de l'écran principal (1 = 100 %, 1,5 = 150 %). Le jeu est compatible DPI depuis le
// 2026-11-12 (host_main.cpp, dpi_compatible) : ses tailles sont en pixels réels. RECOMP_DPI=<ppp> (essais : 144 pour
// 150 %) simule une mise à l'échelle pour le choix de resolution = auto et la taille de la fenêtre.
double echelle_ecran() {
    static const double k = [] {
        UINT ppp = GetDpiForSystem();
        if (const char* e = getenv("RECOMP_DPI")) ppp = (UINT)atoi(e);
        return ppp >= 48 ? ppp / 96.0 : 1.0;
    }();
    return k;
}

// Plein écran sans bordure (recomp.ini : window_mode) : fenêtre sans cadre couvrant l'écran où elle se
// trouve, pour présenter l'image pixel pour pixel. Une fenêtre à cadre de la taille de l'écran n'a
// qu'une zone utile plus petite (2560×1421 sur un écran 1440p) : l'image y était réduite, donc floue.
bool g_borderless = false;
WINDOWPLACEMENT g_wp_windowed{sizeof(WINDOWPLACEMENT)};
void set_borderless(HWND h, bool on) {
    if (on == g_borderless) return;
    if (on) {
        GetWindowPlacement(h, &g_wp_windowed);
        MONITORINFO mi{sizeof mi};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi);
        SetWindowLongW(h, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
    } else {
        SetWindowLongW(h, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
        SetWindowPlacement(h, &g_wp_windowed);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    g_borderless = on;
    HLOG("gfx", "window: %s", on ? "borderless fullscreen" : "windowed");
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        if (g_vulkan) {   // la swapchain Vulkan dessine la fenêtre
            ValidateRect(h, nullptr);
            return 0;
        }
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        float c[4];
        uint64_t f;
        double fps;
        {
            std::lock_guard<std::mutex> lk(g_lk);
            memcpy(c, g_color, sizeof c);
            f = g_frame;
            fps = g_fps;
        }
        auto to8 = [](float v) { return (int)(std::min(1.0f, std::max(0.0f, v)) * 255.0f + 0.5f); };
        RECT rc;
        GetClientRect(h, &rc);
        HBRUSH br = CreateSolidBrush(RGB(to8(c[0]), to8(c[1]), to8(c[2])));
        FillRect(dc, &rc, br);
        DeleteObject(br);
        char txt[160];
        snprintf(txt, sizeof txt, "frame %llu  -  %.1f fps  -  NVN rendering not translated (clear color)",
                 (unsigned long long)f, fps);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        TextOutA(dc, 12, 12, txt, (int)strlen(txt));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SYSKEYDOWN:   // Alt+Entrée : plein écran sans bordure ↔ fenêtré
        if (wp == VK_RETURN && (lp & (1 << 29))) {
            set_borderless(h, !g_borderless);
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_F11) {
            set_borderless(h, !g_borderless);
            return 0;
        }
        break;
    case WM_DPICHANGED: {   // fenêtre passée sur un écran à une autre mise à l'échelle (compatible DPI par écran)
        const RECT* r = (const RECT*)lp;
        if (g_borderless) {   // plein écran : couvrir le nouvel écran, pixel pour pixel
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(MonitorFromRect(r, MONITOR_DEFAULTTONEAREST), &mi);
            SetWindowPos(h, nullptr, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOZORDER | SWP_NOACTIVATE);
        } else {   // fenêtré : taille apparente gardée (rectangle proposé par Windows)
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        }
        HLOG("gfx", "window moved to a screen with %u %% display scaling", (unsigned)(LOWORD(wp) * 100 / 96));
        return 0;
    }
    case WM_CLOSE:
        HLOG("gfx", "window closed: process exiting");
        vkr::save_caches();   // pipelines créés pendant la partie
        fflush(nullptr);
        ExitProcess(0);
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void window_thread() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    // icône du jeu, tirée de la copie du joueur et intégrée à l'exécutable à l'édition de liens (link.py)
    wc.hIcon = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    wc.lpszClassName = L"recomp_main";
    RegisterClassExW(&wc);
    {   // fenêtré : taille apparente d'avant la compatibilité DPI (2/3 de la sortie, en points), passée en pixels réels.
        // À 100 %, rien ne change : trop grande pour l'écran, Windows la réduit à sa création, comme avant (2560×1421 sur
        // un écran 1440p). Sur un écran mis à l'échelle, Windows ne réduisait que sa hauteur (3840×1421 sur un écran
        // 2560×1440 à 150 %, plus large que l'écran) : elle tient maintenant dans la zone de travail, cadre compris,
        // proportions gardées.
        const double e = echelle_ecran();
        if (e != 1.0) {
            g_w = (int)lround(g_w * e);
            g_h = (int)lround(g_h * e);
            RECT zt{}, cadre{0, 0, 0, 0};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &zt, 0);
            AdjustWindowRectExForDpi(&cadre, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
            const int mw = (zt.right - zt.left) - (cadre.right - cadre.left), mh = (zt.bottom - zt.top) - (cadre.bottom - cadre.top);
            if (mw > 0 && mh > 0 && (g_w > mw || g_h > mh)) {
                const double r = std::min((double)mw / g_w, (double)mh / g_h);
                g_w = (int)lround(g_w * r);
                g_h = (int)lround(g_h * r);
            }
        }
        HLOG("gfx", "host window %dx%d (Windows display scaling %.0f %%)", g_w, g_h, e * 100);
    }
    RECT r{0, 0, g_w, g_h};
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForSystem());
    g_hwnd = CreateWindowW(L"recomp_main", L"StarFox Remake [recompiled]", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                           nullptr, inst, nullptr);
    // RECOMP_WINDOW_HIDDEN=1 : la fenêtre existe (Vulkan y présente ses images) mais n'est jamais montrée, ni dans la
    // barre des tâches. Sert au relevé des shaders de la construction (launcher/preparer.py) : voir le jeu s'ouvrir
    // tout seul pendant la première construction déroutait (demande de l'utilisateur, 2026-11-11).
    static const bool cachee = getenv("RECOMP_WINDOW_HIDDEN") != nullptr;
    if (cachee) {
        HLOG("gfx", "window hidden (RECOMP_WINDOW_HIDDEN)");
    } else {
        ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
        if (settings().borderless) set_borderless(g_hwnd, true);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace

void gfx_open_window(int width, int height) {
    std::call_once(g_once, [&] {
        // fenêtre à l'échelle 2/3 pour une sortie 1080p
        g_w = width >= 1920 ? width * 2 / 3 : width;
        g_h = width >= 1920 ? height * 2 / 3 : height;
        g_last = g_fps_t0 = std::chrono::steady_clock::now();
        std::thread(window_thread).detach();   // taille en pixels réels : window_thread
    });
}

void* gfx_window_handle() { return g_hwnd; }

void gfx_status(const char* text) {
    char title[160];
    snprintf(title, sizeof title, "StarFox Remake [recompiled]  -  %s", text);
    if (g_hwnd) SetWindowTextA(g_hwnd, title);
}

void gfx_screen_size(int* w, int* h) {
    // en points (pixels réels ÷ mise à l'échelle de Windows) : resolution = auto garde le choix d'avant la compatibilité
    // DPI, par exemple 1080p pour un écran 4K à 200 %, plutôt qu'un rendu plus lourd après une mise à jour
    const double k = echelle_ecran();
    *w = (int)lround(GetSystemMetrics(SM_CXSCREEN) / k);
    *h = (int)lround(GetSystemMetrics(SM_CYSCREEN) / k);
}

void gfx_count_frame(uint64_t frame) {
    using namespace std::chrono;
    g_vulkan = true;
    auto now = steady_clock::now();
    std::lock_guard<std::mutex> lk(g_lk);
    g_frame = frame;
    double dt = duration<double>(now - g_fps_t0).count();
    if (dt >= 1.0) {
        g_fps = (frame - g_fps_f0) / dt;
        g_fps_t0 = now;
        g_fps_f0 = frame;
        static int n = 0;
        if (++n % 10 == 0) HLOG("gfx", "%.1f fps (frame %llu, Vulkan)", g_fps, (unsigned long long)frame);
        char title[96];
        snprintf(title, sizeof title, "StarFox Remake [recompiled]  -  %.0f fps", g_fps);
        if (g_hwnd) SetWindowTextA(g_hwnd, title);
    }
}

bool gfx_window_focused() {
    HWND f = GetForegroundWindow();
    return g_hwnd && f == g_hwnd;
}

void gfx_present(const float rgba[4], uint64_t frame) {
    using namespace std::chrono;
    auto now = steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(g_lk);
        memcpy(g_color, rgba, sizeof g_color);
        g_frame = frame;
        double dt = duration<double>(now - g_fps_t0).count();
        if (dt >= 1.0) {
            g_fps = (frame - g_fps_f0) / dt;
            g_fps_t0 = now;
            g_fps_f0 = frame;
            static int n = 0;
            if (++n % 10 == 0) HLOG("gfx", "%.1f fps (frame %llu)", g_fps, (unsigned long long)frame);
        }
    }
    if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE);
    // synchro verticale simulée : au plus 60 images par seconde, attente au minuteur haute
    // résolution (précis à ~0,5 ms quelle que soit la résolution du minuteur système)
    static const bool novsync = getenv("RECOMP_NOVSYNC") != nullptr;   // diagnostic
    auto next = g_last + microseconds(16667);
    if (now < next && !novsync) {
        thread_local HANDLE tm = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                        TIMER_ALL_ACCESS);
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)(duration_cast<nanoseconds>(next - now).count() / 100);
        if (tm && SetWaitableTimer(tm, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(tm, INFINITE);
        else std::this_thread::sleep_until(next);
    }
    // pas de rattrapage en rafale après un retard : la cadence repart de maintenant
    g_last = std::max(next, now);
}

}  // namespace hos
