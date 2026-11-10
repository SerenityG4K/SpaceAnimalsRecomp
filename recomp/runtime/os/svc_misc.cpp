// Services HLE divers (contrôle parental, contenus additionnels…), au plus simple : l'objectif
// est de laisser le jeu démarrer, pas d'émuler les fonctions réseau ou de compte.
#include "svc_util.h"

#include <random>

namespace hos {
namespace {

std::shared_ptr<Service> make_pctl_service() {
    return svc("pctl:IParentalControlService", [](Request& r) {
        switch (r.cmd) {
        case 1: return true;                                  // Initialize
        case 1001: case 1017: case 1064: case 1403: return true;
        case 1013: return true;                               // ConfirmStereoVisionPermission
        case 1018: r.push<uint8_t>(1); return true;          // IsFreeCommunicationAvailable
        case 1031: r.push<uint8_t>(0); return true;          // IsRestrictionEnabled : non
        case 1032: r.push<uint32_t>(0); return true;         // GetSafetyLevel
        case 1061: r.push<uint8_t>(0); return true;
        case 1062: r.push<uint8_t>(0); return true;          // GetStereoVisionRestriction
        case 1065: r.push<uint8_t>(1); return true;          // IsStereoVisionPermitted
        }
        return false;
    });
}

std::shared_ptr<Service> make_pctl_root(const char* n) {
    return svc(n, [](Request& r) {
        if (r.cmd == 0 || r.cmd == 1) {   // CreateService / CreateServiceWithoutInitialize
            r.out_objects.push_back(make_pctl_service());
            return true;
        }
        return false;
    });
}

struct Registrar {
    Registrar() {
        for (const char* n : {"pctl", "pctl:s", "pctl:a", "pctl:r"})
            register_service(n, [n] { return make_pctl_root(n); });
        register_service("aoc:u", [] {
            return svc("aoc:u", [](Request& r) {
                switch (r.cmd) {
                case 2: r.push<int32_t>(0); return true;     // CountAddOnContent
                case 3: r.push<int32_t>(0); return true;     // ListAddOnContent
                case 5: r.push<uint64_t>(0x0400E88025B1B000ull); return true;   // GetAddOnContentBaseId
                case 7: case 11: case 12: case 50: return true;
                case 8: case 10: { static Event e(false, "svc_misc.cpp:46"); r.out_copy.push_back(e.copy()); return true; }
                case 100: case 101:   // Create(Permanent)EcPurchasedEventManager → IPurchaseEventManager
                    r.out_objects.push_back(svc("aoc:IPurchaseEventManager", [](Request& q) {
                        switch (q.cmd) {
                        case 0: case 1: return true;          // Set(Default)DeliveryTarget
                        case 2: {                             // GetPurchasedEventReadableHandle
                            static Event e(false, "aoc:achat");
                            q.out_copy.push_back(e.copy());
                            return true;
                        }
                        case 3: case 4: q.result = MakeResult(164, 400); return true;   // Pop… : rien
                        }
                        return false;
                    }));
                    return true;
                }
                return false;
            });
        });
        register_service("csrng", [] {
            return svc("csrng", [](Request& r) {
                if (r.cmd == 0) {   // GenerateRandomBytes → B
                    static std::mt19937_64 rng(std::random_device{}());
                    auto b = r.out_buf();
                    for (uint64_t i = 0; i < b.size; i++) ((uint8_t*)b.addr)[i] = (uint8_t)rng();
                    return true;
                }
                return false;
            });
        });
        for (const char* n : {"ssl", "ssl:s"})
            register_service(n, [n] {
                return svc(n, [](Request& r) {
                    if (r.cmd == 0) {   // CreateContext → ISslContext (bouchon)
                        r.out_objects.push_back(svc("ssl:ISslContext", [](Request&) { return false; }));
                        return true;
                    }
                    return r.cmd == 5;   // SetInterfaceVersion
                });
            });
    }
} g_reg;

}  // namespace
}  // namespace hos
