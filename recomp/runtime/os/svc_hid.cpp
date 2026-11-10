// Service HLE des entrées (hid) : ressource d'applet et sa mémoire partagée, que le SDK lit
// directement. On y présente une manette Pro (style FullKey) connectée en joueur 1, alimentée à
// 60 Hz par le clavier et une manette XInput de l'hôte (input.cpp).
//
// Disposition de la mémoire partagée (lue dans NpadSharedMemoryAccessor::Attach du SDK 21, identique
// à celle de la Switch 1 pour les Npad) :
//   0x9A00 + i × 0x5000 : état interne du Npad i (No1..No8 = 0..7, Handheld = 8, Other = 9)
//   0x3E200            : NpadCondition (nouveau) { u32 dernier Npad actif, u8 valide (+4),
//                        u32 type de maintien (+8), u8 initialisé (+0xc) }
// État interne Npad : +0 style, +4 mode d'assignation, +8 couleurs FullKey, +0x14 couleurs Joy-Con,
// +0x28 LIFO FullKey (puis Handheld, JoyDual… de 0x350 octets), +0x4188 type de périphérique,
// +0x4190 propriétés système, +0x419c batteries.
// LIFO : { u64 horodatage, u64 capacité (17), u64 queue, u64 nombre, 17 × { u64 séquence (impaire
// pendant l'écriture), état 0x28 } } ; état = { u64 n° d'échantillon, u64 boutons, sticks
// gauche/droit (s32 x, y), u32 attributs (bit 0 connecté), u32 réservé }. Le SDK exige des numéros
// d'échantillon consécutifs.
#include "gfx.h"
#include "svc_util.h"

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
#include <thread>

namespace hos {
namespace {

constexpr uint64_t HID_SHMEM_SIZE = 0x40000;
constexpr uint64_t NPAD_BASE = 0x9A00, NPAD_STRIDE = 0x5000, NPAD_COND = 0x3E200;
constexpr uint64_t LIFO_FULLKEY = 0x28, LIFO_N = 17, LIFO_ENTRY = 0x30;
constexpr uint32_t STYLE_FULLKEY = 1u << 0, DEVICE_FULLKEY = 1u << 0;

std::shared_ptr<KObject>& hid_shmem() {
    static std::shared_ptr<KObject> s;
    if (!s) kernel_create_shared_memory(HID_SHMEM_SIZE, &s);
    return s;
}

std::atomic<uint32_t> g_hold_type{1};   // NpadJoyHoldType : 0 vertical, 1 horizontal

template <class T>
void st(uint8_t* p, T v) { __atomic_store_n((T*)p, v, __ATOMIC_RELEASE); }
template <class T>
T ld(const uint8_t* p) { return __atomic_load_n((const T*)p, __ATOMIC_ACQUIRE); }

void write_static(uint8_t* m) {
    uint8_t* n = m + NPAD_BASE;   // joueur 1
    st<uint32_t>(n + 0x00, STYLE_FULLKEY);
    st<uint32_t>(n + 0x04, 0);            // assignation : double
    st<uint32_t>(n + 0x08, 0);            // couleurs FullKey : valides
    st<uint32_t>(n + 0x0c, 0x2d2d2d);     // corps
    st<uint32_t>(n + 0x10, 0xe6e6e6);     // boutons
    st<uint64_t>(n + LIFO_FULLKEY + 8, LIFO_N);
    st<uint32_t>(n + 0x4188, DEVICE_FULLKEY);
    st<uint64_t>(n + 0x4190, (1u << 13) | (1u << 14) | (1u << 15));   // utilise +, -, croix
    for (int i = 0; i < 3; i++) st<uint32_t>(n + 0x419c + 4 * i, 4);   // batteries pleines
    uint8_t* c = m + NPAD_COND;
    st<uint32_t>(c + 0x0, 0);             // dernier actif : No1
    st<uint8_t>(c + 0x4, 1);
    st<uint32_t>(c + 0x8, g_hold_type.load());
    st<uint8_t>(c + 0xc, 1);
}

void lifo_push(uint8_t* L, uint64_t sample, const HostPad& p) {
    uint64_t tail = ld<uint64_t>(L + 0x10), count = ld<uint64_t>(L + 0x18);
    uint64_t nt = (tail + 1) % LIFO_N;
    uint8_t* e = L + 0x20 + nt * LIFO_ENTRY;
    uint64_t seq = ld<uint64_t>(e);
    seq = (seq | 1) + 1;                  // pair suivant
    st<uint64_t>(e, seq - 1);             // impair : écriture en cours
    uint8_t* s = e + 8;
    memcpy(s + 0x00, &sample, 8);
    memcpy(s + 0x08, &p.buttons, 8);
    memcpy(s + 0x10, &p.lx, 4);
    memcpy(s + 0x14, &p.ly, 4);
    memcpy(s + 0x18, &p.rx, 4);
    memcpy(s + 0x1c, &p.ry, 4);
    uint32_t attr = 1, rsv = 0;           // connecté
    memcpy(s + 0x20, &attr, 4);
    memcpy(s + 0x24, &rsv, 4);
    st<uint64_t>(e, seq);
    st<uint64_t>(L + 0x00, sample);
    st<uint64_t>(L + 0x10, nt);
    st<uint64_t>(L + 0x18, std::min<uint64_t>(count + 1, LIFO_N - 1));
}

void input_thread() {
    uint64_t sample = 0, last_buttons = ~0ull;
    int last_xinput = -1;
    // RECOMP_INPUT_LOG=1 : chaque changement de boutons et chaque (dé)connexion de la manette XInput
    static const bool input_log = getenv("RECOMP_INPUT_LOG") != nullptr;
    auto next = std::chrono::steady_clock::now();
    for (;;) {
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
        uint8_t* m = kernel_shmem_ptr(hid_shmem().get());
        if (!m) continue;
        HostPad p = host_poll_input();
        st<uint32_t>(m + NPAD_COND + 0x8, g_hold_type.load());
        lifo_push(m + NPAD_BASE + LIFO_FULLKEY, ++sample, p);
        if (p.buttons != last_buttons && (g_verbose >= 2 || input_log))
            HLOG("hid", "boutons 0x%llx, sticks (%d,%d) (%d,%d)", (unsigned long long)p.buttons, p.lx, p.ly,
                 p.rx, p.ry);
        if (input_log && (int)p.xinput != last_xinput)
            HLOG("hid", "manette XInput hôte %s", p.xinput ? "présente" : "absente");
        last_buttons = p.buttons;
        last_xinput = p.xinput;
    }
}

void start_input() {
    static std::once_flag once;
    std::call_once(once, [] {
        uint8_t* m = kernel_shmem_ptr(hid_shmem().get());
        write_static(m);
        lifo_push(m + NPAD_BASE + LIFO_FULLKEY, 0, HostPad{});   // un premier état « connecté »
        std::thread(input_thread).detach();
        HLOG("hid", "manette Pro émulée connectée (joueur 1)");
    });
}

std::shared_ptr<Service> make_applet_resource() {
    return svc("hid:IAppletResource", [](Request& r) {
        if (r.cmd == 0) {   // GetSharedMemoryHandle
            start_input();
            r.out_copy.push_back(kernel_new_handle(hid_shmem()));
            return true;
        }
        return false;
    });
}

// commandes connues qui renvoient un handle d'événement (copie)
const std::set<uint32_t> k_event_cmds = {40, 106, 211, 309, 525, 1001};

std::shared_ptr<Service> make_hid(const char* n) {
    return svc(n, [](Request& r) {
        if (r.cmd == 0) {   // CreateAppletResource(aruid)
            r.out_objects.push_back(make_applet_resource());
            return true;
        }
        if (k_event_cmds.count(r.cmd)) {
            static Event e(false, "svc_hid.cpp:40");
            r.out_copy.push_back(e.copy());
            return true;
        }
        if (r.cmd == 203) {   // CreateActiveVibrationDeviceList → IActiveVibrationDeviceList
            r.out_objects.push_back(svc("hid:IActiveVibrationDeviceList", [](Request&) { return true; }));
            return true;
        }
        if (r.cmd == 120 && r.in_size >= 16) {   // SetNpadJoyHoldType(aruid, type)
            g_hold_type = (uint32_t)r.arg<uint64_t>(8);
            HLOG("hid", "SetNpadJoyHoldType : arguments 0x%llx 0x%llx → type %u",
                 (unsigned long long)r.arg<uint64_t>(0), (unsigned long long)r.arg<uint64_t>(8), g_hold_type.load());
        }
        // activations, réglages (style de manette, vibrations…) : acceptés, sortie nulle.
        // Journalisées une fois pour savoir lesquelles il faudra vraiment implémenter.
        r.out.assign(0x40, 0);
        static std::mutex m;
        static std::set<uint32_t> seen;
        std::lock_guard<std::mutex> lk(m);
        if (seen.insert(r.cmd).second)
            HLOG("hid", "commande %u acceptée (entrée %zu o, A%zu B%zu X%zu C%zu)", r.cmd, r.in_size,
                 r.a.size(), r.b.size(), r.x.size(), r.c.size());
        return true;
    });
}

struct Registrar {
    Registrar() {
        for (const char* n : {"hid", "hid:sys", "hid:dbg"})
            register_service(n, [n] { return make_hid(n); });
    }
} g_reg;

}  // namespace
}  // namespace hos
