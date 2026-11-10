// Services HLE : comptes (un utilisateur local unique, présélectionné), réseau (nifm : jamais
// connecté) et sockets (bsd : refus). Démarrage hors ligne uniquement.
#include "svc_util.h"

#include <atomic>
#include <mutex>
#include <set>

namespace hos {

// utilisateur local unique (partagé avec am : paramètre de lancement « PreselectedUser »)
extern const uint8_t k_user_uid[16] = {0x01, 0x52, 0x45, 0x43, 0x4f, 0x4d, 0x50, 0x55,
                                0x53, 0x45, 0x52, 0x00, 0x00, 0x00, 0x00, 0x01};

namespace {

constexpr uint32_t NIFM_NOT_CONNECTED = MakeResult(110, 300);   // « pas de connexion »

struct ProfileBase {
    uint8_t uid[16];
    uint64_t last_edit;
    char nickname[0x20];
};
static_assert(sizeof(ProfileBase) == 0x38, "ProfileBase");

std::shared_ptr<Service> make_profile() {
    return svc("acc:IProfile", [](Request& r) {
        ProfileBase pb{};
        memcpy(pb.uid, k_user_uid, 16);
        pb.last_edit = 1700000000;
        strcpy(pb.nickname, "Joueur");
        switch (r.cmd) {
        case 0: {   // Get → UserData (B) + ProfileBase
            auto b = r.out_buf();
            if (b.addr) memset((void*)b.addr, 0, b.size);
            r.push(pb);
            return true;
        }
        case 1: r.push(pb); return true;            // GetBase
        case 10: r.push<uint32_t>(0); return true;  // GetImageSize : pas d'avatar
        case 11: r.push<uint32_t>(0); return true;  // LoadImage
        }
        return false;
    });
}

std::shared_ptr<Service> make_acc(const char* n) {
    return svc(n, [](Request& r) {
        auto write_uid_list = [&] {
            auto b = r.out_buf();
            if (b.addr) {
                memset((void*)b.addr, 0, b.size);
                if (b.size >= 16) memcpy((void*)b.addr, k_user_uid, 16);
            }
        };
        switch (r.cmd) {
        case 0: r.push<int32_t>(1); return true;                 // GetUserCount
        case 1: r.push<uint8_t>(1); return true;                 // GetUserExistence
        case 2: case 3: case 60: case 131: write_uid_list(); return true;   // List…Users
        case 4: r.out.assign(k_user_uid, k_user_uid + 16); return true;     // GetLastOpenedUser
        case 5: r.out_objects.push_back(make_profile()); return true;       // GetProfile
        case 50: r.push<uint8_t>(0); return true;                // IsUserRegistrationRequestPermitted
        case 51: r.out.assign(k_user_uid, k_user_uid + 16); return true;    // TrySelectUserWithoutInteraction
        case 100: case 140: case 160: return true;               // InitializeApplicationInfo…
        case 101:                                                 // GetBaasAccountManagerForApplication
            r.out_objects.push_back(svc("acc:IManagerForApplication", [](Request& q) {
                switch (q.cmd) {
                case 0: q.result = MakeResult(124, 280); return true;   // CheckAvailability : hors ligne
                case 1: q.push<uint64_t>(0x0123456789abcdefull); return true;   // GetAccountId
                }
                return false;
            }));
            return true;
        case 110: case 111: return true;                         // thumbnails de sauvegarde
        case 150: r.push<uint8_t>(0); return true;               // IsUserAccountSwitchLocked
        }
        return false;
    });
}

std::shared_ptr<Service> make_nifm_request() {
    auto ev1 = std::make_shared<Event>(), ev2 = std::make_shared<Event>();
    return svc("nifm:IRequest", [ev1, ev2](Request& r) {
        switch (r.cmd) {
        case 0: r.push<int32_t>(1); return true;                 // GetRequestState : Free (non accepté)
        case 1: r.result = NIFM_NOT_CONNECTED; return true;      // GetResult
        case 2: r.out_copy.push_back(ev1->copy()); r.out_copy.push_back(ev2->copy()); return true;
        case 3: case 4: case 6: case 8: case 11: case 12: case 13: case 14: case 15: case 16:
        case 17: case 18: case 19: case 20: case 21: case 22: case 23:
            return true;
        }
        return false;
    });
}

std::shared_ptr<Service> make_nifm_general() {
    return svc("nifm:IGeneralService", [](Request& r) {
        switch (r.cmd) {
        case 1: r.out.assign(16, 0); r.out[0] = 1; return true;  // GetClientId
        case 4: r.out_objects.push_back(make_nifm_request()); return true;   // CreateRequest
        case 5: case 12: case 15: case 18: r.result = NIFM_NOT_CONNECTED; return true;
        case 21: case 22: r.push<uint8_t>(0); return true;      // IsAny…RequestAccepted
        }
        return false;
    });
}

struct Registrar {
    Registrar() {
        for (const char* n : {"acc:u0", "acc:u1", "acc:su", "acc:aa"})
            register_service(n, [n] { return make_acc(n); });
        for (const char* n : {"nifm:u", "nifm:a", "nifm:s"})
            register_service(n, [n] {
                return svc(n, [](Request& r) {
                    if (r.cmd == 4 || r.cmd == 5) {   // CreateGeneralService(Old)
                        r.out_objects.push_back(make_nifm_general());
                        return true;
                    }
                    return false;
                });
            });
    }
} g_reg;

}  // namespace
}  // namespace hos
