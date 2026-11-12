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

// nfp:user (amiibo). Un PC n'a pas de lecteur NFC. Jusqu'au 2026-11-12, le service était le bouchon générique : la
// commande 0 de la racine rendait un objet bouchon, puis AttachAvailabilityChangeEvent (23) ne rendait aucun handle
// d'événement alors que le jeu en attend un, ListDevices (2) aucun appareil, GetState (19) « non initialisé ». Issue
// n° 9 : au premier passage par le menu de la campagne après le tutoriel, le jeu ouvre ce service (Initialize, 23, 2,
// puis Finalize trois secondes plus tard), et « Single Player » ne répond plus (trois joueurs). Lien pas démontré :
// nos parties d'essai (sauvegardes de campagne finie) ne passent pas par là.
// Ce qui est imité maintenant, comme Ryujinx : une console avec un lecteur NFC (manette du joueur 1) et aucune
// figurine posée ; le chemin que le jeu a connu sur la console. Un appareil ; la détection attend une figurine qui ne
// vient jamais ; les événements ne sont jamais signalés ; les commandes qui demandent une figurine échouent comme sans
// figurine (WrongDeviceState, module 115).
std::shared_ptr<Service> make_nfp_user() {
    struct Etat {
        bool init = false;
        uint32_t appareil = 0;   // DeviceState : 0 Initialized, 1 SearchingForTag, 6 Finalized
    };
    auto e = std::make_shared<Etat>();
    return svc("nfp:IUser", [e](Request& r) {
        constexpr uint32_t NFP = 115, WRONG_DEVICE_STATE = 73;
        switch (r.cmd) {
        case 0:   // Initialize(aruid, …, A : données du MCU)
            e->init = true, e->appareil = 0;
            return true;
        case 1:   // Finalize
            e->init = false, e->appareil = 6;
            return true;
        case 2: {   // ListDevices → (u32 nombre, C : DeviceHandle[8 octets])
            if (!e->init) {
                r.result = MakeResult(NFP, WRONG_DEVICE_STATE);
                return true;
            }
            int32_t n = 0;
            if (!r.c.empty() && r.c[0].addr && r.c[0].size >= 8) {
                const uint64_t joueur1 = 0;   // manette du joueur 1
                memcpy((void*)r.c[0].addr, &joueur1, 8);
                n = 1;
            }
            r.push<int32_t>(n);
            return true;
        }
        case 3:   // StartDetection : en attente d'une figurine
            e->appareil = 1;
            return true;
        case 4:   // StopDetection
            e->appareil = 0;
            return true;
        case 6:   // Unmount
            return true;
        case 5: case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 24:
            // Mount, application area, informations de la figurine… : aucune figurine
            r.result = MakeResult(NFP, WRONG_DEVICE_STATE);
            return true;
        case 17: {   // AttachActivateEvent : figurine posée (jamais)
            static Event ev(false, "nfp: figurine posée");
            r.out_copy.push_back(ev.copy());
            return true;
        }
        case 18: {   // AttachDeactivateEvent : figurine retirée (jamais)
            static Event ev(false, "nfp: figurine retirée");
            r.out_copy.push_back(ev.copy());
            return true;
        }
        case 19:   // GetState : 0 NonInitialized, 1 Initialized
            r.push<uint32_t>(e->init ? 1 : 0);
            return true;
        case 20:   // GetDeviceState
            r.push<uint32_t>(e->appareil);
            return true;
        case 21:   // GetNpadId : joueur 1
            r.push<uint32_t>(0);
            return true;
        case 22:   // GetApplicationAreaSize
            r.push<uint32_t>(0xD8);
            return true;
        case 23: {   // AttachAvailabilityChangeEvent (jamais signalé : le lecteur reste disponible)
            static Event ev(false, "nfp: disponibilité");
            r.out_copy.push_back(ev.copy());
            return true;
        }
        }
        return false;   // bouchon générique, journalisé
    });
}

struct Registrar {
    Registrar() {
        register_service("nfp:user", [] {
            return svc("nfp:user", [](Request& r) {
                if (r.cmd == 0) {   // CreateUserInterface → IUser
                    r.out_objects.push_back(make_nfp_user());
                    return true;
                }
                return false;
            });
        });
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
