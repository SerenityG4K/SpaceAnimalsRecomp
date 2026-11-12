// Services HLE : gestionnaire d'applets (appletOE et sous-interfaces) et journal du SDK (lm).
// Identifiants de commandes d'après la documentation publique (switchbrew) ; les commandes non
// listées passent par le bouchon générique (journalisées).
#include "settings.h"
#include "svc_util.h"

#include <deque>
#include <mutex>

namespace hos {
extern const uint8_t k_user_uid[16];   // svc_account.cpp
namespace {

constexpr uint32_t AM_NO_DATA = MakeResult(128, 2), AM_NO_MESSAGE = MakeResult(128, 3);

// ---------------------------------------------------------------- IStorage
struct Storage : Service {
    std::shared_ptr<std::vector<uint8_t>> data;
    explicit Storage(size_t n) : data(std::make_shared<std::vector<uint8_t>>(n)) { name = "am:IStorage"; }
    void handle(Request& r) override {
        if (r.cmd == 0) {   // Open → IStorageAccessor
            auto d = data;
            r.out_objects.push_back(svc("am:IStorageAccessor", [d](Request& q) {
                switch (q.cmd) {
                case 0: q.push<int64_t>((int64_t)d->size()); return true;   // GetSize
                case 10: {   // Write(offset, buffer A)
                    int64_t off = q.arg<int64_t>(0);
                    auto b = q.in_buf();
                    if (off >= 0 && (uint64_t)off + b.size <= d->size())
                        memcpy(d->data() + off, (void*)b.addr, b.size);
                    return true;
                }
                case 11: {   // Read(offset) → buffer B
                    int64_t off = q.arg<int64_t>(0);
                    auto b = q.out_buf();
                    if (off >= 0 && (uint64_t)off < d->size())
                        memcpy((void*)b.addr, d->data() + off,
                               std::min<uint64_t>(b.size, d->size() - off));
                    return true;
                }
                }
                return false;
            }));
            return;
        }
        stub(r);
    }
};

// ---------------------------------------------------------------- ICommonStateGetter
struct CommonStateGetter : Service {
    Event ev{true, "am:ICommonStateGetter(messages)"};
    std::deque<uint32_t> msgs{15};   // FocusStateChanged au démarrage
    std::mutex lk;
    CommonStateGetter() { name = "am:ICommonStateGetter"; }
    void handle(Request& r) override {
        switch (r.cmd) {
        case 0: r.out_copy.push_back(ev.copy()); return;   // GetEventHandle
        case 1: {                                            // ReceiveMessage
            std::lock_guard<std::mutex> g(lk);
            if (msgs.empty()) {
                kernel_signal(ev.obj.get(), false);   // plus rien : l'événement retombe
                r.result = AM_NO_MESSAGE;
                return;
            }
            r.push<uint32_t>(msgs.front());
            msgs.pop_front();
            if (msgs.empty()) kernel_signal(ev.obj.get(), false);
            return;
        }
        case 5: r.push<uint8_t>(1); return;          // GetOperationMode : console (TV)
        case 6: r.push<uint32_t>(1); return;         // GetPerformanceMode : boost (TV)
        case 7: r.push<uint8_t>(0); return;          // GetCradleStatus
        case 8: r.push<uint8_t>(0); return;          // GetBootMode
        case 9: r.push<uint8_t>(1); return;          // GetCurrentFocusState : InFocus
        case 60: r.push<int32_t>(1920); r.push<int32_t>(1080); return;   // GetDefaultDisplayResolution
        case 61: { static Event e(false, "svc_am.cpp:76"); r.out_copy.push_back(e.copy()); return; }
        case 66: case 67: return;                    // Set/CancelCpuBoostMode
        case 91: r.push<uint32_t>(0x00020003); return;   // GetCurrentPerformanceConfiguration
        case 300: r.push<uint32_t>(1); return;       // GetSettingsPlatformRegion : Global
        }
        stub(r);
    }
};

std::shared_ptr<Service> make_self_controller() {
    return svc("am:ISelfController", [](Request& r) {
        switch (r.cmd) {
        case 0: case 1: case 2: case 3: case 4: case 10: case 11: case 12: case 13: case 14:
        case 15: case 16: case 17: case 18: case 19: case 20: case 50: case 51: case 60:
        case 61: case 62: case 64: case 65: case 68: case 72: case 80: case 100: case 110:
        case 130:
            return true;                              // réglages acceptés sans effet
        case 9: { static Event e(false, "svc_am.cpp:93"); r.out_copy.push_back(e.copy()); return true; }
        case 40: r.push<uint64_t>(1); return true;   // CreateManagedDisplayLayer → id de couche
        case 41: r.push<uint8_t>(0); return true;    // IsSystemBufferSharingEnabled
        case 63: r.push<uint32_t>(0); return true;   // GetIdleTimeDetectionExtension
        case 67: r.push<uint8_t>(0); return true;    // IsIlluminanceAvailable
        case 69: r.push<uint8_t>(0); return true;    // IsAutoSleepDisabled
        case 90: r.push<uint64_t>(0); return true;   // GetAccumulatedSuspendedTickValue
        case 91: { static Event e(false, "svc_am.cpp:100"); r.out_copy.push_back(e.copy()); return true; }
        }
        return false;
    });
}

std::shared_ptr<Service> make_window_controller() {
    return svc("am:IWindowController", [](Request& r) {
        switch (r.cmd) {
        case 1: r.push<uint64_t>(0x1000); return true;   // GetAppletResourceUserId
        case 2: r.push<uint64_t>(0); return true;
        case 10: case 11: case 12: case 20: case 21: return true;
        }
        return false;
    });
}

std::shared_ptr<Service> make_audio_controller() {
    return svc("am:IAudioController", [](Request& r) {
        switch (r.cmd) {
        case 0: case 3: case 4: return true;
        case 1: case 2: r.push<float>(1.0f); return true;
        }
        return false;
    });
}

std::shared_ptr<Service> make_library_applet_creator() {
    return svc("am:ILibraryAppletCreator", [](Request& r) {
        if (r.cmd == 10) {   // CreateStorage(size)
            r.out_objects.push_back(std::make_shared<Storage>((size_t)r.arg<int64_t>(0)));
            return true;
        }
        return false;
    });
}

std::shared_ptr<Service> make_application_functions() {
    return svc("am:IApplicationFunctions", [](Request& r) {
        switch (r.cmd) {
        case 1: {   // PopLaunchParameter(kind) : 2 = utilisateur présélectionné (une fois)
            static bool given = false;
            if (r.arg<uint32_t>(0) == 2 && !given) {
                given = true;
                auto st = std::make_shared<Storage>(0x88);
                uint32_t hdr[2] = {0xC79497CAu, 1};
                memcpy(st->data->data(), hdr, 8);
                memcpy(st->data->data() + 8, k_user_uid, 16);
                r.out_objects.push_back(st);
                return true;
            }
            r.result = AM_NO_DATA;
            return true;
        }
        case 20: r.push<uint64_t>(0); return true;             // EnsureSaveData
        case 21: {                                              // GetDesiredLanguage : recomp.ini [game] language
            char code[8] = {};
            const std::string l = language_code();
            memcpy(code, l.data(), std::min<size_t>(l.size(), sizeof code));
            r.push(code);
            return true;
        }
        case 22: case 30: case 31: case 32: case 33: case 60: case 67: case 90: case 100:
        case 101: case 102: case 124: case 131: case 170:
            return true;
        case 23: { char v[16] = "2.0.0"; r.push(v); return true; }   // GetDisplayVersion
        case 25: r.push<uint64_t>(0); return true;             // ExtendSaveData
        case 26: r.push<int64_t>(0x10000000); r.push<int64_t>(0x10000000); return true;
        case 28: r.push<int64_t>(0x10000000); r.push<int64_t>(0x10000000); return true;
        case 40: r.push<uint8_t>(0); return true;              // NotifyRunning
        case 50: { uint8_t id[16] = {0x52, 0x45, 0x43, 0x4f, 0x4d, 0x50}; r.push(id); return true; }
        case 65: r.push<uint8_t>(0); return true;              // IsGamePlayRecordingSupported
        case 66: return true;                                  // InitializeGamePlayRecording
        case 123: r.push<int32_t>(-1); return true;            // GetPreviousProgramIndex
        case 130: case 140: case 150: case 160: { static Event e(false, "svc_am.cpp:172"); r.out_copy.push_back(e.copy()); return true; }
        case 141: case 151: r.result = AM_NO_DATA; return true;
        // 320 / 321 (Switch 2) : événements lus par nn::oe::GetOperationMode / GetPerformanceMode
        // (changement de mode console/portable et de performances)
        // 210 (Switch 2) : événement attaché par l'initialisation de nn::oe::GetOperationMode
        case 210: { static Event e(false, "am:210 (GetOperationMode)"); r.out_copy.push_back(e.copy()); return true; }
        case 320: { static Event e(false, "am:320 (mode de fonctionnement)"); r.out_copy.push_back(e.copy()); return true; }
        case 321: { static Event e(false, "am:321 (mode de performances)"); r.out_copy.push_back(e.copy()); return true; }
        }
        return false;
    });
}

std::shared_ptr<Service> make_application_proxy() {
    auto csg = std::make_shared<CommonStateGetter>();
    return svc("am:IApplicationProxy", [csg](Request& r) {
        switch (r.cmd) {
        case 0: r.out_objects.push_back(csg); return true;
        case 1: r.out_objects.push_back(make_self_controller()); return true;
        case 2: r.out_objects.push_back(make_window_controller()); return true;
        case 3: r.out_objects.push_back(make_audio_controller()); return true;
        case 4: r.out_objects.push_back(svc("am:IDisplayController", [](Request&) { return false; })); return true;
        case 10: r.out_objects.push_back(svc("am:IProcessWindingController", [](Request&) { return false; })); return true;
        case 11: r.out_objects.push_back(make_library_applet_creator()); return true;
        case 20: r.out_objects.push_back(make_application_functions()); return true;
        case 1000: r.out_objects.push_back(svc("am:IDebugFunctions", [](Request&) { return false; })); return true;
        }
        // nouvelles sous-interfaces (Switch 2) : objet générique, dont
        // GraphicsScalingParametersFunctions (commande 10 → {échelle 10/20/30, 0, 0}, lue par
        // nn::oe::GetCurrentGraphicsScale ; 10 = échelle 0)
        HLOG("ipc", "am:IApplicationProxy: unknown command %u → generic object", r.cmd);
        r.out_objects.push_back(svc("am:IApplicationProxy?", [](Request& q) {
            if (q.cmd == 10) {
                static const uint32_t scale = getenv("RECOMP_GFX_SCALE") ? (uint32_t)atoi(getenv("RECOMP_GFX_SCALE")) : 10;
                q.push<uint32_t>(scale == 20 || scale == 30 ? scale : 10);
                q.push<uint32_t>(0);
                q.push<uint32_t>(0);
                return true;
            }
            return false;
        }));
        return true;
    });
}

// ---------------------------------------------------------------- lm (journal du SDK)
void lm_log(const uint8_t* p, size_t n) {
    if (n < 0x18) return;
    uint8_t severity = p[0x12];
    uint32_t payload;
    memcpy(&payload, p + 0x14, 4);
    std::string text, file, func, module;
    int line = -1;
    size_t i = 0x18, end = std::min(n, (size_t)0x18 + payload);
    while (i < end) {
        uint8_t key = p[i++];
        uint64_t len = 0;
        int sh = 0;
        while (i < end) {
            uint8_t b = p[i++];
            len |= (uint64_t)(b & 0x7f) << sh;
            sh += 7;
            if (!(b & 0x80)) break;
        }
        if (i + len > end) break;
        std::string v((const char*)p + i, (size_t)len);
        switch (key) {
        case 2: text += v; break;   // TextLog
        case 3: if (len >= 4) memcpy(&line, p + i, 4); break;
        case 4: file = v; break;
        case 5: func = v; break;
        case 6: module = v; break;
        }
        i += len;
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\0')) text.pop_back();
    if (!text.empty() || !func.empty())
        HLOG("lm", "[%u] %s%s%s%s", severity, text.c_str(), func.empty() ? "" : "  (", func.c_str(),
             func.empty() ? "" : ")");
}

std::shared_ptr<Service> make_lm() {
    return svc("lm", [](Request& r) {
        if (r.cmd == 0) {   // OpenLogger(pid) → ILogger
            r.out_objects.push_back(svc("lm:ILogger", [](Request& q) {
                if (q.cmd == 0) {   // Log(buffer)
                    auto b = q.in_buf();
                    if (b.addr && b.size) lm_log((const uint8_t*)b.addr, (size_t)b.size);
                    return true;
                }
                if (q.cmd == 1) return true;   // SetDestination
                return false;
            }));
            return true;
        }
        return false;
    });
}

struct Registrar {
    Registrar() {
        register_service("appletOE", [] {
            return svc("appletOE", [](Request& r) {
                if (r.cmd == 0 || r.cmd == 10) {   // OpenApplicationProxy (10 : variante récente)
                    r.out_objects.push_back(make_application_proxy());
                    return true;
                }
                return false;
            });
        });
        register_service("lm", make_lm);
    }
} g_reg;

}  // namespace
}  // namespace hos

