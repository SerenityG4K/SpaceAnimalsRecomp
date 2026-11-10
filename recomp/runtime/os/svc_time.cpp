// Service HLE de l'heure (time:u / time:a / time:s) : horloges système et stationnaire, mémoire
// partagée lue directement par le SDK, fuseau horaire (UTC + décalage local de l'hôte).
#include "svc_util.h"

#include <chrono>
#include <ctime>

namespace hos {
namespace {

const uint8_t k_steady_id[16] = {0x52, 0x45, 0x43, 0x4f, 0x4d, 0x50, 0x2d, 0x53,
                                 0x54, 0x45, 0x41, 0x44, 0x59, 0x00, 0x00, 0x01};

struct SteadyClockTimePoint {
    int64_t time_point;   // secondes
    uint8_t source_id[16];
};
struct SystemClockContext {
    int64_t offset;
    SteadyClockTimePoint timestamp;
};
static_assert(sizeof(SystemClockContext) == 0x20, "SystemClockContext");

int64_t steady_seconds() {
    // même base que le compteur invité (rt_cntvct : temps écoulé depuis le démarrage)
    return (int64_t)(rt_cntvct() / rt_cntfrq());
}
int64_t unix_now() {
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

SystemClockContext clock_context() {
    SystemClockContext c{};
    int64_t s = steady_seconds();
    c.offset = unix_now() - s;
    c.timestamp.time_point = s;
    memcpy(c.timestamp.source_id, k_steady_id, 16);
    return c;
}

std::shared_ptr<Service> make_system_clock(const char* n) {
    return svc(n, [](Request& r) {
        switch (r.cmd) {
        case 0: r.push<int64_t>(unix_now()); return true;        // GetCurrentTime
        case 1: case 3: return true;                              // Set… : ignorés
        case 2: r.push(clock_context()); return true;            // GetSystemClockContext
        case 4: { static Event e(false, "svc_time.cpp:48"); r.out_copy.push_back(e.copy()); return true; }
        }
        return false;
    });
}

std::shared_ptr<Service> make_steady_clock() {
    return svc("time:ISteadyClock", [](Request& r) {
        switch (r.cmd) {
        case 0: {   // GetCurrentTimePoint
            SteadyClockTimePoint tp{steady_seconds(), {}};
            memcpy(tp.source_id, k_steady_id, 16);
            r.push(tp);
            return true;
        }
        case 2: case 200: r.push<int64_t>(0); return true;     // GetTestOffset / GetInternalOffset
        case 3: case 201: return true;
        case 100: r.push<int64_t>(unix_now()); return true;    // GetRtcValue
        case 101: r.push<uint8_t>(0); return true;             // IsRtcResetDetected
        case 102: r.push<uint32_t>(0); return true;            // GetSetupResultValue
        }
        return false;
    });
}

struct CalendarTime {
    int16_t year;
    int8_t month, day, hour, minute, second, pad;
};
struct CalendarAdditionalInfo {
    uint32_t day_of_week, day_of_year;
    char tz_name[8];
    uint32_t is_dst;
    int32_t utc_offset;
};
static_assert(sizeof(CalendarTime) == 8 && sizeof(CalendarAdditionalInfo) == 0x18, "calendar");

void to_calendar(int64_t t, CalendarTime& ct, CalendarAdditionalInfo& ai) {
    time_t tt = (time_t)t;
    struct tm lt;
    localtime_s(&lt, &tt);
    struct tm ut;
    gmtime_s(&ut, &tt);
    ct.year = (int16_t)(lt.tm_year + 1900);
    ct.month = (int8_t)(lt.tm_mon + 1);
    ct.day = (int8_t)lt.tm_mday;
    ct.hour = (int8_t)lt.tm_hour;
    ct.minute = (int8_t)lt.tm_min;
    ct.second = (int8_t)lt.tm_sec;
    ct.pad = 0;
    ai = {};
    ai.day_of_week = (uint32_t)lt.tm_wday;
    ai.day_of_year = (uint32_t)lt.tm_yday;
    memcpy(ai.tz_name, "LOCAL", 5);
    ai.is_dst = lt.tm_isdst > 0;
    ai.utc_offset = (int32_t)(_mkgmtime(&lt) - tt);
}

std::shared_ptr<Service> make_timezone() {
    return svc("time:ITimeZoneService", [](Request& r) {
        switch (r.cmd) {
        case 0: { char loc[0x24] = "Europe/Paris"; r.push(loc); return true; }   // GetDeviceLocationName
        case 1: return true;
        case 2: r.push<uint32_t>(1); return true;            // GetTotalLocationNameCount
        case 4: return true;                                  // LoadTimeZoneRule : tampon laissé tel quel
        case 5: { uint8_t v[0x10] = {'2', '0', '2', '4', 'a'}; r.push(v); return true; }
        case 100:                                             // ToCalendarTime(posix, règle)
        case 101: {                                           // ToCalendarTimeWithMyRule(posix)
            CalendarTime ct;
            CalendarAdditionalInfo ai;
            to_calendar(r.arg<int64_t>(0), ct, ai);
            r.push(ct);
            r.push(ai);
            return true;
        }
        case 201:                                             // ToPosixTime(calendar, règle) → C
        case 202: {
            CalendarTime ct = r.arg<CalendarTime>(0);
            struct tm lt{};
            lt.tm_year = ct.year - 1900;
            lt.tm_mon = ct.month - 1;
            lt.tm_mday = ct.day;
            lt.tm_hour = ct.hour;
            lt.tm_min = ct.minute;
            lt.tm_sec = ct.second;
            lt.tm_isdst = -1;
            int64_t t = (int64_t)mktime(&lt);
            auto b = r.out_buf();
            if (b.addr && b.size >= 8) memcpy((void*)b.addr, &t, 8);
            r.push<uint32_t>(1);
            return true;
        }
        }
        return false;
    });
}

struct TimeService : Service {
    std::shared_ptr<KObject> shm;
    explicit TimeService(const char* n) {
        name = n;
        kernel_create_shared_memory(0x1000, &shm);
        uint8_t* p = kernel_shmem_ptr(shm.get());
        // 0x00 : contexte de l'horloge stationnaire (compteur + 2 copies)
        for (int i = 0; i < 2; i++) {
            uint64_t off = 0;
            memcpy(p + 8 + i * 0x18, &off, 8);
            memcpy(p + 8 + i * 0x18 + 8, k_steady_id, 16);
        }
        // 0x38 / 0x80 : contextes des horloges locale et réseau
        SystemClockContext c = clock_context();
        for (uint64_t base : {0x38ull, 0x80ull})
            for (int i = 0; i < 2; i++) memcpy(p + base + 8 + i * 0x20, &c, 0x20);
        // 0xC8 : correction automatique de l'horloge utilisateur (désactivée)
        // 0xD0 : point d'ajustement continu de l'horloge stationnaire (compteur + 2 × 0x38 o).
        // Le SDK calcule t = ns(tick) + clamp(((ns - base) * mul >> shift) + add) ; tout à zéro
        // donne t = ns(tick). L'identifiant de source (+0x28) doit être non nul.
        for (int i = 0; i < 2; i++) memcpy(p + 0xD8 + i * 0x38 + 0x28, k_steady_id, 16);
    }
    void handle(Request& r) override {
        switch (r.cmd) {
        case 0: case 4: r.out_objects.push_back(make_system_clock("time:ISystemClock")); return;
        case 1: case 5: r.out_objects.push_back(make_system_clock("time:ISystemClock(réseau)")); return;
        case 2: r.out_objects.push_back(make_steady_clock()); return;
        case 3: r.out_objects.push_back(make_timezone()); return;
        case 20: r.out_copy.push_back(kernel_new_handle(shm)); return;   // GetSharedMemoryNativeHandle
        case 30: case 31: { static Event e(false, "svc_time.cpp:174"); r.out_copy.push_back(e.copy()); return; }
        case 50: return;
        case 51: r.push<int64_t>(unix_now()); return;
        case 100: r.push<uint8_t>(0); return;     // IsStandardUserSystemClockAutomaticCorrectionEnabled
        case 101: return;
        case 102: r.push<int32_t>(2024); return;
        case 200: r.push<uint8_t>(1); return;     // IsStandardNetworkSystemClockAccuracySufficient
        case 300: {                                // CalculateMonotonicSystemClockBaseTimePoint
            SystemClockContext c = r.arg<SystemClockContext>(0);
            r.push<int64_t>(c.offset + steady_seconds() - steady_seconds());
            return;
        }
        }
        stub(r);
    }
};

struct Registrar {
    Registrar() {
        for (const char* n : {"time:u", "time:a", "time:s", "time:r", "time:sm", "time:su", "time:al"})
            register_service(n, [n] { return std::make_shared<TimeService>(n); });
    }
} g_reg;

}  // namespace
}  // namespace hos
