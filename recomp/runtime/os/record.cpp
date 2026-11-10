// Extraits vidéo des cinématiques, pour le fond du launcher (recomp/launcher).
//
// RECOMP_RECORD_DIR=<dossier> : l'image présentée est relue une fois sur deux (30 img/s, réduite de moitié) et
// analysée. Une cinématique se reconnaît à ses bandes noires (haut et bas) : chaque passage en cinématique
// donne des extraits MP4 (H.264, Media Foundation de Windows), recadrés sans les bandes, donc sans les
// sous-titres ni l'invite « Passer ». Un extrait commence sur un changement de plan quand c'est possible et
// s'arrête à la fin de la cinématique ou à sa durée maximale ; un extrait trop court ou trop sombre est jeté.
//
// Réglages : RECOMP_RECORD_CLIPS (nombre d'extraits, 6), RECOMP_RECORD_SECONDS (durée max., 9 s),
// RECOMP_RECORD_MIN (durée min., 4 s), RECOMP_RECORD_GAP (écart min. entre deux débuts d'extraits, 12 s),
// RECOMP_RECORD_AFTER (image à partir de laquelle chercher, 0), RECOMP_RECORD_EXIT=1 (quitter le jeu une
// fois le dernier extrait écrit).
//
// Les extraits sont tirés de la copie du joueur et restent sur son PC (jamais distribués).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include "hos.h"
#include "record.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hos {
namespace rec {
namespace {

struct Config {
    std::string dir;
    int clips = 6;
    int max_frames = 9 * 30, min_frames = 4 * 30, gap_frames = 12 * 30, skip_frames = 30;
    uint64_t after = 0;
    bool exit_after = false;
};

const Config& cfg() {
    static const Config c = [] {
        Config c;
        if (const char* d = getenv("RECOMP_RECORD_DIR")) c.dir = d;
        auto num = [](const char* n, double def) { const char* v = getenv(n); return v ? atof(v) : def; };
        c.clips = (int)num("RECOMP_RECORD_CLIPS", 6);
        c.max_frames = (int)(num("RECOMP_RECORD_SECONDS", 9) * 30);
        c.min_frames = (int)(num("RECOMP_RECORD_MIN", 4) * 30);
        c.gap_frames = (int)(num("RECOMP_RECORD_GAP", 12) * 30);
        c.after = (uint64_t)num("RECOMP_RECORD_AFTER", 0);
        c.exit_after = getenv("RECOMP_RECORD_EXIT") != nullptr;
        if (!c.dir.empty()) {
            CreateDirectoryA(c.dir.c_str(), nullptr);
            HLOG("rec", "extraits de cinématiques dans %s (%d au plus, %d à %d s)", c.dir.c_str(), c.clips,
                 c.min_frames / 30, c.max_frames / 30);
        }
        return c;
    }();
    return c;
}

// ---- écriture MP4 sur un fil à part (Media Foundation) ----
struct Msg {
    enum Kind { Start, Frame, End } k;
    std::string path;
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> px;   // BGRA
    bool keep = false;
    bool last = false;
};

std::mutex g_lk;
std::condition_variable g_cv;
std::deque<Msg> g_q;
std::atomic<int> g_pending{0};   // images en attente d'écriture (limite la mémoire)

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

template <class T> void release(T*& p) { if (p) p->Release(), p = nullptr; }

void writer_thread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    if (!mf) HLOG("rec", "Media Foundation indisponible : pas d'extraits");
    IMFSinkWriter* sw = nullptr;
    DWORD si = 0;
    LONGLONG t = 0;
    uint32_t w = 0, h = 0;
    std::string path, tmp;
    for (;;) {
        Msg m;
        {
            std::unique_lock<std::mutex> lk(g_lk);
            g_cv.wait(lk, [] { return !g_q.empty(); });
            m = std::move(g_q.front());
            g_q.pop_front();
        }
        if (m.k == Msg::Start && mf) {
            path = m.path, tmp = m.path + ".part", w = m.w, h = m.h, t = 0;
            IMFAttributes* at = nullptr;
            MFCreateAttributes(&at, 3);
            at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
            at->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
            at->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
            HRESULT hr = MFCreateSinkWriterFromURL(widen(tmp).c_str(), nullptr, at, &sw);
            release(at);
            IMFMediaType *out = nullptr, *in = nullptr;
            if (SUCCEEDED(hr)) {
                MFCreateMediaType(&out);
                out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
                out->SetUINT32(MF_MT_AVG_BITRATE, 9000000);
                out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
                MFSetAttributeRatio(out, MF_MT_FRAME_RATE, 30, 1);
                MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                hr = sw->AddStream(out, &si);
            }
            if (SUCCEEDED(hr)) {
                MFCreateMediaType(&in);
                in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
                in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
                in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
                in->SetUINT32(MF_MT_DEFAULT_STRIDE, w * 4);   // lignes de haut en bas
                MFSetAttributeSize(in, MF_MT_FRAME_SIZE, w, h);
                MFSetAttributeRatio(in, MF_MT_FRAME_RATE, 30, 1);
                MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
                hr = sw->SetInputMediaType(si, in, nullptr);
            }
            if (SUCCEEDED(hr)) hr = sw->BeginWriting();
            release(out), release(in);
            if (FAILED(hr)) {
                HLOG("rec", "écrivain MP4 refusé (0x%08lx) pour %s", (unsigned long)hr, tmp.c_str());
                release(sw);
            }
        } else if (m.k == Msg::Frame) {
            if (sw && m.px.size() == (size_t)w * h * 4) {
                IMFMediaBuffer* b = nullptr;
                IMFSample* s = nullptr;
                if (SUCCEEDED(MFCreateMemoryBuffer(w * h * 4, &b))) {
                    BYTE* p = nullptr;
                    b->Lock(&p, nullptr, nullptr);
                    memcpy(p, m.px.data(), m.px.size());
                    b->Unlock();
                    b->SetCurrentLength(w * h * 4);
                    MFCreateSample(&s);
                    s->AddBuffer(b);
                    s->SetSampleTime(t);
                    s->SetSampleDuration(333333);
                    t += 333333;
                    sw->WriteSample(si, s);
                }
                release(s), release(b);
            }
            g_pending--;
        } else if (m.k == Msg::End) {
            bool ok = false;
            if (sw) {
                ok = SUCCEEDED(sw->Finalize());
                release(sw);
            }
            if (ok && m.keep) {
                DeleteFileW(widen(path).c_str());
                ok = MoveFileW(widen(tmp).c_str(), widen(path).c_str());
                HLOG("rec", "extrait écrit : %s (%ux%u, %.1f s)", path.c_str(), w, h, t / 1e7);
            } else {
                DeleteFileW(widen(tmp).c_str());
            }
            if (m.last && cfg().exit_after) {
                HLOG("rec", "derniers extraits écrits : fin du jeu (RECOMP_RECORD_EXIT)");
                fflush(nullptr);
                TerminateProcess(GetCurrentProcess(), 0);
            }
        }
    }
}

void post(Msg&& m) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(writer_thread).detach(); });
    {
        std::lock_guard<std::mutex> lk(g_lk);
        g_q.push_back(std::move(m));
    }
    g_cv.notify_one();
}

// ---- analyse des images ----
inline int luma(const uint8_t* p) { return (p[0] * 54 + p[1] * 183 + p[2] * 19) >> 8; }

// nombre de lignes noires en haut de l'image ; le quart droit est ignoré : l'invite « Passer (maintenir) »
// s'y affiche dans la bande pendant les cinématiques
uint32_t top_bar(const uint8_t* px, uint32_t w, uint32_t h) {
    uint32_t y = 0;
    for (; y < h / 3; y++) {
        const uint8_t* row = px + (size_t)y * w * 4;
        int dark = 0, n = 0;
        for (uint32_t x = 0; x < w * 3 / 4; x += 8, n++) dark += luma(row + 4 * x) < 14;
        if (dark * 100 < n * 92) break;
    }
    return y;
}

struct State {
    bool rec = false;
    int clips = 0, len = 0, run = 0, miss = 0, since = 1 << 30, waited = 0;
    uint32_t last_bar = 0;   // dernière bande valide (recadrage)
    uint32_t y0 = 0, cw = 0, ch = 0;
    double lsum = 0;
    float thumb[32 * 18] = {};
    bool have_thumb = false;
    bool done = false;
} g_st;

// vignette 32×18 en niveaux de gris de la zone utile ; rend l'écart moyen avec la précédente (0-255)
float thumb_diff(const uint8_t* px, uint32_t w, uint32_t y0, uint32_t ch, double* mean) {
    float t[32 * 18];
    double s = 0;
    for (int j = 0; j < 18; j++)
        for (int i = 0; i < 32; i++) {
            const uint32_t x = (uint32_t)((i + 0.5) * w / 32), y = y0 + (uint32_t)((j + 0.5) * ch / 18);
            t[j * 32 + i] = (float)luma(px + ((size_t)y * w + x) * 4);
            s += t[j * 32 + i];
        }
    float d = 0;
    if (g_st.have_thumb)
        for (int k = 0; k < 32 * 18; k++) d += fabsf(t[k] - g_st.thumb[k]);
    memcpy(g_st.thumb, t, sizeof t);
    const bool first = !g_st.have_thumb;
    g_st.have_thumb = true;
    *mean = s / (32 * 18);
    return first ? 0.f : d / (32 * 18);
}

void end_clip(bool keep) {
    g_st.rec = false;
    const bool good = keep && g_st.len >= cfg().min_frames && g_st.lsum / std::max(1, g_st.len) >= 22.0;
    if (good) g_st.clips++;
    Msg m{Msg::End};
    m.keep = good;
    m.last = g_st.clips >= cfg().clips;
    if (m.last) g_st.done = true;
    post(std::move(m));
    if (!good)
        HLOG("rec", "extrait abandonné (%d images, luminance %.0f)", g_st.len, g_st.lsum / std::max(1, g_st.len));
}

}  // namespace

bool wanted(uint64_t frame) {
    const Config& c = cfg();
    return !c.dir.empty() && !g_st.done && frame >= c.after && (frame & 1) == 0;
}

void frame(const uint8_t* px, uint32_t w, uint32_t h, uint64_t) {
    const Config& c = cfg();
    if (c.dir.empty() || g_st.done || w < 64 || h < 64) return;
    const uint32_t bar = top_bar(px, w, h);
    const bool cine = bar >= h * 6 / 100 && bar <= h * 22 / 100;
    static uint64_t n_calls = 0;
    if (n_calls++ % 300 == 0)
        HLOG("rec", "image %llu : %ux%u, bande %u, cinématique %d, extraits %d", (unsigned long long)n_calls, w, h, bar,
             (int)cine, g_st.clips);
    g_st.since++;
    // une image ratée (fondu au noir, plan très sombre) ne coupe pas une cinématique : 0,5 s de tolérance
    g_st.miss = cine ? 0 : g_st.miss + 1;
    if (!cine && (g_st.run == 0 || g_st.miss > 15)) {
        if (g_st.rec) end_clip(true);
        g_st.run = 0, g_st.have_thumb = false;
        return;
    }
    g_st.run++;
    if (cine) g_st.last_bar = bar;
    const uint32_t b = g_st.last_bar;
    // zone utile : sans les bandes (marge de 2 lignes), hauteur multiple de 16
    uint32_t y0 = g_st.rec ? g_st.y0 : b + 2;
    uint32_t ch = g_st.rec ? g_st.ch : ((h - 2 * (b + 2)) & ~15u);
    if (!g_st.rec) y0 += ((h - 2 * (b + 2)) - ch) / 2;
    double mean = 0;
    const float diff = thumb_diff(px, w, y0, ch, &mean);
    const bool cut = diff > 28.f;
    if (!g_st.rec) {
        if (!cine || g_st.clips >= c.clips || g_st.run < c.skip_frames || g_st.since < c.gap_frames) return;
        // départ sur un changement de plan, ou après 3 s d'attente
        if (!cut && ++g_st.waited < 90) return;
        g_st.rec = true, g_st.len = 0, g_st.lsum = 0, g_st.waited = 0, g_st.since = 0;
        g_st.y0 = y0, g_st.cw = w & ~15u, g_st.ch = ch;
        char path[512];
        snprintf(path, sizeof path, "%s/extrait_%02d.mp4", c.dir.c_str(), g_st.clips + 1);
        Msg m{Msg::Start};
        m.path = path, m.w = g_st.cw, m.h = g_st.ch;
        post(std::move(m));
        HLOG("rec", "extrait %d commencé (bande %u, zone %ux%u, %s)", g_st.clips + 1, bar, g_st.cw, g_st.ch,
             cut ? "changement de plan" : "attente");
    } else if (cut && g_st.len >= c.max_frames * 2 / 3) {   // fin propre sur un changement de plan
        end_clip(true);
        return;
    }
    // copie de la zone utile en BGRA (attente si l'écriture prend du retard)
    while (g_pending.load() > 60) Sleep(1);
    Msg m{Msg::Frame};
    m.px.resize((size_t)g_st.cw * g_st.ch * 4);
    const uint32_t x0 = (w - g_st.cw) / 2;
    for (uint32_t y = 0; y < g_st.ch; y++) {
        const uint8_t* s = px + ((size_t)(g_st.y0 + y) * w + x0) * 4;
        uint8_t* d = m.px.data() + (size_t)y * g_st.cw * 4;
        for (uint32_t x = 0; x < g_st.cw; x++, s += 4, d += 4) d[0] = s[2], d[1] = s[1], d[2] = s[0], d[3] = 255;
    }
    g_pending++;
    post(std::move(m));
    g_st.len++;
    g_st.lsum += mean;
    if (g_st.len >= c.max_frames) end_clip(true);
}

}  // namespace rec
}  // namespace hos
