// Audio HLE, niveau API (nn::audio, API « flux » de la Switch 2) : sortie stéréo, entrée micro absente.
// Chaque flux de sortie du jeu a un tampon de 100 ms (stéréo flottant, 48 kHz) que le mixeur vide au
// rythme de la carte son (audio_out.cpp, WASAPI) ; sans périphérique, une horloge virtuelle le vide à
// 48 kHz (comportement d'avant, muet). L'événement de notification d'un flux est créé par le SDK
// lui-même (nn::os::CreateSystemEvent appelé depuis le natif) puis signalé quand il y a de la place.
#include "svc_util.h"
#include "audio_out.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace hos {
namespace {

// nn::audio::StreamFormat (16 o) : format d'échantillon, canaux, fréquence, réservé — vu : 5, 2, 48000, 0.
// Formats d'échantillon (numérotation nn::audio::SampleFormat) : 1 PcmInt8, 2 PcmInt16, 4 PcmInt32, 5 PcmFloat.
struct Stream {
    bool started = false;
    uint32_t rate = 48000, channels = 2, fmt = 5;
    uint32_t capacity = 4800;        // trames (100 ms à 48 kHz)
    std::vector<float> ring;         // capacity trames stéréo flottantes
    uint32_t rd = 0, fill = 0;       // position de lecture, trames en attente
    double vdrain = 0;               // horloge virtuelle : trames dues (fraction comprise)
    float gain = 1.0f;               // SetOutputStreamGain
    std::chrono::steady_clock::time_point last = std::chrono::steady_clock::now();
    std::shared_ptr<KObject> event;  // notification (peut être nul)
    uint64_t written = 0, played = 0, underruns = 0;
};
std::mutex g_lk;
std::map<uint64_t, Stream> g_streams;   // OutputStreamType* → flux
uint64_t g_sym_create_event = 0;
const bool g_log = getenv("RECOMP_AUDIO_LOG") != nullptr;
std::atomic<uint32_t> g_peak_bits{0};   // crête du mixage depuis le dernier bilan (RECOMP_AUDIO_LOG)

Stream& get_stream(uint64_t key) {
    Stream& s = g_streams[key];
    if (s.ring.size() != 2ull * s.capacity) s.ring.assign(2ull * s.capacity, 0.0f);
    return s;
}

// Sans périphérique audio : les trames sont « jouées » au rythme de 48 kHz (horloge du processeur).
void drain_virtual(Stream& s) {
    auto now = std::chrono::steady_clock::now();
    double dt = std::chrono::duration<double>(now - s.last).count();
    s.last = now;
    if (!s.started || audio_out::active()) return;
    s.vdrain += dt * 48000.0;
    const uint32_t k = (uint32_t)std::min<double>(s.fill, std::floor(s.vdrain));
    s.vdrain -= k;
    s.rd = (s.rd + k) % s.capacity;
    s.fill -= k;
    s.played += k;
    if (!s.fill) s.vdrain = 0;   // pas de dette accumulée pendant que le flux est vide
}

// Mixeur appelé par le fil de rendu audio (audio_out.cpp), au rythme de la carte son.
void mix(float* out, uint32_t n) {
    memset(out, 0, (size_t)n * 8);
    std::vector<std::shared_ptr<KObject>> sig;
    {
        std::lock_guard<std::mutex> lk(g_lk);
        for (auto& [k, s] : g_streams) {
            if (!s.started || s.ring.empty()) continue;
            const uint32_t m = std::min(n, s.fill);
            for (uint32_t i = 0; i < m; i++) {
                const uint32_t j = (s.rd + i) % s.capacity;
                out[2 * i] += s.ring[2 * j] * s.gain;
                out[2 * i + 1] += s.ring[2 * j + 1] * s.gain;
            }
            s.rd = (s.rd + m) % s.capacity;
            s.fill -= m;
            s.played += m;
            if (m < n && s.written) s.underruns++;   // le jeu n'a pas écrit assez tôt (craquement)
            s.last = std::chrono::steady_clock::now();
            if (s.event && s.fill < s.capacity) sig.push_back(s.event);
        }
    }
    float peak = 0.0f;
    for (uint32_t i = 0; i < 2 * n; i++) {
        out[i] = std::clamp(out[i], -1.0f, 1.0f);
        peak = std::max(peak, std::fabs(out[i]));
    }
    if (g_log) {
        uint32_t pb;
        memcpy(&pb, &peak, 4);
        uint32_t cur = g_peak_bits.load();
        while (pb > cur && !g_peak_bits.compare_exchange_weak(cur, pb)) {}   // flottants positifs : ordre des bits
    }
    for (auto& e : sig) kernel_signal(e.get(), true);   // place libre : le jeu écrit la suite
}

void clock_thread() {
    auto next_log = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::vector<std::shared_ptr<KObject>> sig;
        {
            std::lock_guard<std::mutex> lk(g_lk);
            for (auto& [k, s] : g_streams) {
                drain_virtual(s);
                if (s.started && s.event && s.fill < s.capacity) sig.push_back(s.event);
            }
            if (g_log && std::chrono::steady_clock::now() >= next_log) {
                next_log += std::chrono::seconds(10);
                uint32_t pb = g_peak_bits.exchange(0);
                float peak;
                memcpy(&peak, &pb, 4);
                for (auto& [k, s] : g_streams)
                    HLOG("audio", "stream 0x%llx: %s, %llu frames written, %llu played, %llu underruns, %u pending; mix peak %.3f (%s)",
                         (unsigned long long)k, s.started ? "started" : "stopped", (unsigned long long)s.written,
                         (unsigned long long)s.played, (unsigned long long)s.underruns, s.fill, peak,
                         audio_out::active() ? "audio device" : "virtual clock");
            }
        }
        for (auto& e : sig) kernel_signal(e.get(), true);
    }
}

std::shared_ptr<KObject> make_guest_event(CPUState* ctx, uint64_t system_event) {
    if (!system_event || !g_sym_create_event) return nullptr;
    // nn::os::CreateSystemEvent(SystemEventType*, EventClearMode_AutoClear, interProcess) : le
    // SDK crée un vrai événement noyau, dont on récupère l'objet pour le signaler nous-mêmes
    CPUState saved = *ctx;
    ctx->x[0] = system_event;
    ctx->x[1] = 1;
    ctx->x[2] = 1;
    call_guest(ctx, g_sym_create_event);
    uint32_t r = (uint32_t)ctx->x[0];
    std::shared_ptr<KObject> e = kernel_last_created_event();
    *ctx = saved;
    if (r) HLOG("audio", "CreateSystemEvent failed (0x%x)", r);
    return e;
}

void open_stream(CPUState* ctx, uint64_t s, uint64_t ev, uint64_t fmt) {
    Stream st;
    if (fmt) {
        uint32_t f[4];
        memcpy(f, (void*)fmt, 16);
        if (f[0] >= 1 && f[0] <= 6) st.fmt = f[0];
        if (f[1] >= 1 && f[1] <= 8) st.channels = f[1];
        if (f[2] >= 8000 && f[2] <= 192000) st.rate = f[2];
        HLOG("audio", "OpenOutputStream: format %u %u %u %u → %s samples, %u channels, %u Hz", f[0], f[1], f[2], f[3],
             st.fmt == 5 ? "float" : st.fmt == 2 ? "16-bit" : st.fmt == 4 ? "32-bit" : st.fmt == 1 ? "8-bit" : "unknown",
             st.channels, st.rate);
        if (st.rate != 48000) HLOG("audio", "rate %u Hz not resampled: playing at 48 kHz (pitch off)", st.rate);
    }
    st.event = make_guest_event(ctx, ev);
    std::lock_guard<std::mutex> lk(g_lk);
    g_streams[s] = st;
    get_stream(s);   // tampon alloué
}

// Une trame du format du jeu → stéréo flottant (mono dupliqué, 5.1 replié : FL FR FC LFE BL BR)
inline float sample(const Stream& s, const uint8_t* p, uint64_t idx) {
    switch (s.fmt) {
    case 1: return ((const int8_t*)p)[idx] / 128.0f;
    case 2: return ((const int16_t*)p)[idx] / 32768.0f;
    case 4: return (float)(((const int32_t*)p)[idx] / 2147483648.0);
    default: return ((const float*)p)[idx];
    }
}
void read_frame(const Stream& s, const uint8_t* p, uint64_t i, float* l, float* r) {
    const uint64_t b = i * s.channels;
    if (s.channels == 1) {
        *l = *r = sample(s, p, b);
    } else if (s.channels >= 6) {
        const float c = 0.7071f * sample(s, p, b + 2);
        *l = sample(s, p, b) + c + 0.7071f * sample(s, p, b + 4);
        *r = sample(s, p, b + 1) + c + 0.7071f * sample(s, p, b + 5);
    } else {
        *l = sample(s, p, b);
        *r = sample(s, p, b + 1);
    }
}

// OpenOutputStream(type*, SystemEvent*, buf, size, const name&, const fmt&, int, int, flags)
void a_open_ev(CPUState* ctx) {
    open_stream(ctx, ctx->x[0], ctx->x[1], ctx->x[5]);
    ctx->x[0] = 0;
}
// OpenOutputStream(type*, buf, size, const name&, const fmt&, int, flags)
void a_open(CPUState* ctx) {
    open_stream(ctx, ctx->x[0], 0, ctx->x[4]);
    ctx->x[0] = 0;
}
void a_start(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    auto& s = get_stream(ctx->x[0]);
    drain_virtual(s);
    s.started = true;
    ctx->x[0] = 0;
}
void a_start_many(CPUState* ctx) {   // (type* const*, n)
    std::lock_guard<std::mutex> lk(g_lk);
    for (int i = 0; i < (int32_t)ctx->x[1]; i++) {
        auto& s = get_stream(((uint64_t*)ctx->x[0])[i]);
        drain_virtual(s);
        s.started = true;
    }
    ctx->x[0] = 0;
}
void a_stop(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    get_stream(ctx->x[0]).started = false;
    ctx->x[0] = 0;
}
void a_close(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    g_streams.erase(ctx->x[0]);
    ctx->x[0] = 0;
}
void a_purge(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    auto& s = get_stream(ctx->x[0]);
    s.fill = s.rd = 0;
    s.vdrain = 0;
    ctx->x[0] = 0;
}
void a_write(CPUState* ctx) {   // (type*, data, frames) → trames acceptées
    std::lock_guard<std::mutex> lk(g_lk);
    auto& s = get_stream(ctx->x[0]);
    drain_virtual(s);
    const int32_t n = (int32_t)ctx->x[2];
    const int32_t acc = std::max(0, std::min<int32_t>(n, (int32_t)(s.capacity - s.fill)));
    const uint8_t* src = (const uint8_t*)ctx->x[1];
    for (int32_t i = 0; i < acc; i++) {
        float l = 0.0f, r = 0.0f;
        if (src) read_frame(s, src, (uint64_t)i, &l, &r);
        const uint32_t j = (s.rd + s.fill) % s.capacity;
        s.ring[2 * j] = l;
        s.ring[2 * j + 1] = r;
        s.fill++;
    }
    s.written += acc;
    static int nlog = 0;
    if (g_log && nlog < 6) {
        nlog++;
        HLOG("audio", "WriteOutputStream: %d frames requested, %d accepted, %u pending", n, acc, s.fill);
    }
    ctx->x[0] = (uint32_t)acc;
}
void a_free(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    auto& s = get_stream(ctx->x[0]);
    drain_virtual(s);
    ctx->x[0] = s.capacity - s.fill;
}
void a_filled(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    auto& s = get_stream(ctx->x[0]);
    drain_virtual(s);
    ctx->x[0] = s.fill;
}
void a_state(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    ctx->x[0] = get_stream(ctx->x[0]).started ? 1 : 0;
}
void a_gain(CPUState* ctx) {   // SetOutputStreamGain(type*, float gain, TimeSpan rampe) : gain dans s0, rampe ignorée
    std::lock_guard<std::mutex> lk(g_lk);
    const float g = ctx->v[0].f32[0];
    get_stream(ctx->x[0]).gain = std::isfinite(g) ? std::clamp(g, 0.0f, 4.0f) : 1.0f;
    ctx->x[0] = 0;
}
void a_zero(CPUState* ctx) { ctx->x[0] = 0; }
void a_channels(CPUState* ctx) { ctx->x[0] = 2; }
void a_device_name(CPUState* ctx) {   // (AudioDeviceName*)
    if (ctx->x[0]) memcpy((void*)ctx->x[0], "MainAudioOut", 13);
    ctx->x[0] = 0;
}
void a_notification(CPUState* ctx) {   // AcquireAudioDeviceNotificationForOutput(SystemEvent*)
    make_guest_event(ctx, ctx->x[0]);   // jamais signalé : le périphérique ne change pas
    ctx->x[0] = 0;
}
void a_no_input(CPUState* ctx) { ctx->x[0] = MakeResult(153, 1); }   // pas de micro

// ---------------------------------------------------------------- décodeur Opus « matériel »
// Pas de libopus ici : chaque paquet accepté rend 20 ms de silence (960 trames à 48 kHz).
struct OpusDec {
    uint64_t manager = 0;
    int32_t rate = 48000, channels = 2;
    int pending = 0, completed = 0;
};
struct OpusMgr { std::shared_ptr<KObject> event; };
std::map<uint64_t, OpusDec> g_dec;
std::map<uint64_t, OpusMgr> g_mgr;

void o_wbsize(CPUState* ctx) { ctx->x[0] = 0x20000; }
void o_init_mgr(CPUState* ctx) {   // (mgr*, SystemEventType*, int, int, TimeSpan, int, int, buf, size)
    uint64_t mgr = ctx->x[0], ev = ctx->x[1];
    auto e = make_guest_event(ctx, ev);
    std::lock_guard<std::mutex> lk(g_lk);
    g_mgr[mgr].event = e;
    ctx->x[0] = 0;
}
void o_fini_mgr(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    g_mgr.erase(ctx->x[0]);
    ctx->x[0] = 0;
}
void o_init_dec(CPUState* ctx) {   // (dec*, rate, channels, TimeSpan, mgr*)
    std::lock_guard<std::mutex> lk(g_lk);
    OpusDec d;
    d.rate = (int32_t)ctx->x[1];
    d.channels = (int32_t)ctx->x[2];
    d.manager = ctx->x[4];
    g_dec[ctx->x[0]] = d;
    static int nlog = 0;
    if (g_log && nlog++ < 16)
        HLOG("audio", "Opus decoder 0x%llx: %d Hz, %d channels, duration %lld, handler 0x%llx", (unsigned long long)ctx->x[0],
             d.rate, d.channels, (long long)ctx->x[3], (unsigned long long)d.manager);
    ctx->x[0] = 0;
}
void o_fini_dec(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    g_dec.erase(ctx->x[0]);
    ctx->x[0] = 0;
}
void o_reset_dec(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    auto& d = g_dec[ctx->x[0]];
    d.pending = d.completed = 0;
    ctx->x[0] = 0;
}
void o_enqueue(CPUState* ctx) {   // (dec*, size_t* consumed, data, size)
    std::lock_guard<std::mutex> lk(g_lk);
    g_dec[ctx->x[0]].pending++;
    static uint64_t npk = 0;
    if (g_log && npk++ < 24 && ctx->x[2]) {   // forme des données : en-tête éventuel, taille du paquet
        const uint8_t* p = (const uint8_t*)ctx->x[2];
        char hex[3 * 24 + 1] = {};
        for (int i = 0; i < 24 && (uint64_t)i < ctx->x[3]; i++) snprintf(hex + 3 * i, 4, "%02x ", p[i]);
        HLOG("audio", "Opus packet #%llu for 0x%llx: %llu bytes: %s", (unsigned long long)npk, (unsigned long long)ctx->x[0],
             (unsigned long long)ctx->x[3], hex);
    }
    if (ctx->x[1]) *(uint64_t*)ctx->x[1] = ctx->x[3];
    ctx->x[0] = 0;
}
void o_execute(CPUState* ctx) {   // (mgr*) : tout ce qui attend est « décodé »
    std::shared_ptr<KObject> e;
    {
        std::lock_guard<std::mutex> lk(g_lk);
        for (auto& [k, d] : g_dec)
            if (d.manager == ctx->x[0]) {
                d.completed += d.pending;
                d.pending = 0;
            }
        e = g_mgr[ctx->x[0]].event;
    }
    if (e) kernel_signal(e.get(), true);
    ctx->x[0] = 0;
}
void o_free_io(CPUState* ctx) { ctx->x[0] = 8; }
void o_waiting(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    ctx->x[0] = (uint32_t)g_dec[ctx->x[0]].pending;
}
void o_completed(CPUState* ctx) {
    std::lock_guard<std::mutex> lk(g_lk);
    ctx->x[0] = (uint32_t)g_dec[ctx->x[0]].completed;
}
void o_dequeue(CPUState* ctx) {   // (dec*, int* trames, int16* pcm, size)
    std::lock_guard<std::mutex> lk(g_lk);
    auto& d = g_dec[ctx->x[0]];
    int32_t frames = 0;
    if (d.completed > 0) {
        d.completed--;
        int ch = d.channels > 0 ? d.channels : 2;
        frames = std::min<int64_t>(d.rate / 50, (int64_t)(ctx->x[3] / (2 * ch)));
        if (ctx->x[2]) memset((void*)ctx->x[2], 0, (size_t)frames * 2 * ch);
    }
    if (ctx->x[1]) *(int32_t*)ctx->x[1] = frames;
    ctx->x[0] = 0;
}

}  // namespace

// Pour les autres HLE : crée un événement système invité (nn::os::SystemEventType) via le SDK.
std::shared_ptr<KObject> hle_make_system_event(CPUState* ctx, uint64_t system_event) {
    return make_guest_event(ctx, system_event);
}

void hle_audio_init() {
    g_sym_create_event = find_export("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
    struct O { const char* sym; RecompFn fn; };
    const O ov[] = {
        {"_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", a_open_ev},
        {"_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE", a_open},
        {"_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", a_start},
        {"_ZN2nn5audio18StartOutputStreamsEPKPNS0_16OutputStreamTypeEi", a_start_many},
        {"_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", a_stop},
        {"_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", a_close},
        {"_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", a_purge},
        {"_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", a_write},
        {"_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", a_free},
        {"_ZN2nn5audio31GetOutputStreamFilledFrameCountEPKNS0_16OutputStreamTypeE", a_filled},
        {"_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", a_state},
        {"_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", a_zero},
        {"_ZN2nn5audio19SetOutputStreamGainEPNS0_16OutputStreamTypeEfNS_8TimeSpanE", a_gain},
        {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", a_channels},
        {"_ZN2nn5audio24GetActiveAudioDeviceNameEPNS0_15AudioDeviceNameE", a_device_name},
        {"_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", a_device_name},
        {"_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", a_device_name},
        {"_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", a_zero},
        {"_ZN2nn5audio31GetVirtualInputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", a_no_input},
        {"_ZN2nn5audio39AcquireAudioDeviceNotificationForOutputEPNS_2os11SystemEventE", a_notification},
        {"_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", a_zero},
        {"_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", a_no_input},
        {"_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", a_zero},
        {"_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", a_no_input},
        {"_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", a_zero},
        {"_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", a_zero},
        {"_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", a_zero},
        {"_ZN2nn5codec43GetHardwareOpusDecoderManagerWorkBufferSizeEiiNS_8TimeSpanEii", o_wbsize},
        {"_ZN2nn5codec36InitializeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeEPNS_2os15SystemEventTypeEiiNS_8TimeSpanEiiPvm", o_init_mgr},
        {"_ZN2nn5codec34FinalizeHardwareOpusDecoderManagerEPNS0_30HardwareOpusDecoderManagerTypeE", o_fini_mgr},
        {"_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiNS_8TimeSpanEPNS0_30HardwareOpusDecoderManagerTypeE", o_init_dec},
        {"_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", o_fini_dec},
        {"_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", o_reset_dec},
        {"_ZN2nn5codec36EnqueueOpusDataToHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPmPKvm", o_enqueue},
        {"_ZN2nn5codec34ExecuteHardwareOpusDecoderDecodingEPNS0_30HardwareOpusDecoderManagerTypeE", o_execute},
        {"_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", o_free_io},
        {"_ZN2nn5codec47GetHardwareOpusDecoderWaitingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", o_waiting},
        {"_ZN2nn5codec49GetHardwareOpusDecoderExecutingDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", a_zero},
        {"_ZN2nn5codec49GetHardwareOpusDecoderCompletedDecodeRequestCountEPKNS0_23HardwareOpusDecoderTypeE", o_completed},
        {"_ZN2nn5codec48DequeuePcmInterleavedDataFromHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEPiPsm", o_dequeue},
    };
    int n = 0;
    for (auto& o : ov) n += override_export(o.sym, o.fn);
    std::thread(clock_thread).detach();
    audio_out::start(mix);
    HLOG("hle", "audio: %d nn::audio / nn::codec functions replaced (Windows output, Opus → silence)", n);
}

}  // namespace hos
