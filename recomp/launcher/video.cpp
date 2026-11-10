// Fond vidéo du launcher : les extraits MP4 enregistrés par le jeu recompilé (launcher/extraits), lus en
// boucle dans un ordre aléatoire par Media Foundation, sur un fil à part. Le dessin prend la dernière image.
#include "lanceur.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>

#include <algorithm>
#include <chrono>
#include <random>
#include <thread>

namespace lz {
namespace {

std::mutex g_lk;
std::vector<uint8_t> g_px;
uint32_t g_w = 0, g_h = 0;
uint64_t g_numero = 0;
double g_t = 0, g_duree = 0;
std::atomic<bool> g_marche{false};
std::atomic<int> g_nombre{0};
std::thread g_fil;

template <class T> void liberer(T*& p) { if (p) p->Release(), p = nullptr; }

void lire(const std::wstring& chemin, std::vector<uint8_t>& px) {
    IMFAttributes* at = nullptr;
    MFCreateAttributes(&at, 1);
    at->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);   // conversion en RGB32
    IMFSourceReader* rd = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(chemin.c_str(), at, &rd);
    liberer(at);
    if (FAILED(hr)) return;
    IMFMediaType* t = nullptr;
    MFCreateMediaType(&t);
    t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
    liberer(t);
    IMFMediaType* cur = nullptr;
    UINT32 w = 0, h = 0;
    if (SUCCEEDED(hr)) hr = rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    if (SUCCEEDED(hr)) MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
    LONG pas = (LONG)w * 4;
    if (cur) {
        UINT32 s = 0;
        if (SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s))) pas = (LONG)(INT32)s;
    }
    liberer(cur);
    if (FAILED(hr) || !w || !h) {
        liberer(rd);
        return;
    }
    double duree = 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(rd->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v))) {
        duree = v.uhVal.QuadPart / 1e7;
        PropVariantClear(&v);
    }
    px.resize((size_t)w * h * 4);
    const auto t0 = std::chrono::steady_clock::now();
    while (g_marche) {
        DWORD idx = 0, drapeaux = 0;
        LONGLONG ts = 0;
        IMFSample* s = nullptr;
        if (FAILED(rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &idx, &drapeaux, &ts, &s))) break;
        if (drapeaux & MF_SOURCE_READERF_ENDOFSTREAM) {
            liberer(s);
            break;
        }
        if (!s) continue;
        const double du = ts / 1e7;
        while (g_marche) {   // attente de l'instant de l'image
            const double maintenant = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (maintenant >= du) break;
            Sleep((DWORD)std::clamp((du - maintenant) * 1000.0, 1.0, 20.0));
        }
        IMFMediaBuffer* b = nullptr;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
            IMF2DBuffer* b2 = nullptr;
            BYTE* p = nullptr;
            LONG pitch = 0;
            if (SUCCEEDED(b->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&p, &pitch))) {
                for (UINT32 y = 0; y < h; y++) memcpy(px.data() + (size_t)y * w * 4, p + (ptrdiff_t)y * pitch, (size_t)w * 4);
                b2->Unlock2D();
            } else {
                DWORD n = 0;
                if (SUCCEEDED(b->Lock(&p, nullptr, &n))) {
                    for (UINT32 y = 0; y < h; y++) {
                        const BYTE* ligne = pas >= 0 ? p + (size_t)y * pas : p + (size_t)(h - 1 - y) * (size_t)(-pas);
                        memcpy(px.data() + (size_t)y * w * 4, ligne, (size_t)w * 4);
                    }
                    b->Unlock();
                }
            }
            liberer(b2);
            liberer(b);
            // RGB32 de Media Foundation : 4e octet à 0 ; les effets Direct2D le liraient comme transparent
            for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
            std::lock_guard<std::mutex> l(g_lk);
            g_px = px;
            g_w = w, g_h = h, g_t = du, g_duree = duree;
            g_numero++;
        }
        liberer(s);
    }
    liberer(rd);
}

void boucle(std::wstring dossier) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    std::vector<std::wstring> extraits;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dossier + L"\\extrait_*.mp4").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do extraits.push_back(dossier + L"\\" + fd.cFileName);
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_nombre = (int)extraits.size();
    std::mt19937 alea((unsigned)GetTickCount64());
    std::shuffle(extraits.begin(), extraits.end(), alea);
    std::vector<uint8_t> px;
    for (size_t k = 0; g_marche && !extraits.empty(); k++) lire(extraits[k % extraits.size()], px);
    MFShutdown();
    CoUninitialize();
}

}  // namespace

void video_demarrer(const std::wstring& dossier) {
    if (g_marche) return;
    if (g_fil.joinable()) g_fil.join();
    g_marche = true;
    g_fil = std::thread(boucle, dossier);
}

void video_arreter() {
    g_marche = false;
    if (g_fil.joinable()) g_fil.join();
    std::lock_guard<std::mutex> l(g_lk);
    g_px.clear();
    g_numero++;
}

int video_nombre() { return g_nombre; }

bool video_image(uint64_t& numero, std::vector<uint8_t>& px, uint32_t& w, uint32_t& h, double& t, double& duree) {
    std::lock_guard<std::mutex> l(g_lk);
    if (numero == g_numero) return false;
    numero = g_numero;
    if (g_px.empty()) {
        px.clear();
        return true;
    }
    px = g_px;
    w = g_w, h = g_h, t = g_t, duree = g_duree;
    return true;
}

}  // namespace lz
