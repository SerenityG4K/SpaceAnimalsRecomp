// Service HLE des sockets (bsd:u / bsd:s), numérotation Switch 1 (+ 36 = initialisation Switch 2).
// Hors ligne : les sockets existent mais ne se connectent jamais (réseau injoignable). Les
// eventfd sont réels (compteur), et poll() bloque jusqu'à l'expiration du délai ou l'écriture
// d'un eventfd surveillé : c'est ce dont a besoin une boucle d'événements de type libuv.
// errno de style FreeBSD (le module système bsd en dérive).
#include "svc_util.h"

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <deque>
#include <set>

namespace hos {
namespace {

enum : int32_t { E_NOENT = 2, E_BADF = 9, E_AGAIN = 35, E_INPROGRESS = 36, E_NOTSUP = 45,
                 E_NETUNREACH = 51, E_NOTCONN = 57 };

struct Fd {
    bool eventfd = false;
    uint64_t counter = 0;
    bool semaphore = false;
    bool failed_connect = false;
    // boucle locale TCP
    uint8_t addr[16] = {};          // sockaddr_in lié (len, famille, port BE, adresse)
    bool bound = false, listening = false, connected = false, peer_closed = false;
    int32_t peer = -1;
    std::deque<int32_t> accept_q;
    std::deque<uint8_t> rx;
};
std::mutex g_lk;
std::condition_variable g_cv;
std::map<int32_t, Fd> g_fds;
int32_t g_next = 3;

struct PollFd {
    int32_t fd;
    int16_t events, revents;
};

void ret(Request& r, int32_t v, int32_t err) {
    r.push<int32_t>(v);
    r.push<int32_t>(err);
}

uint16_t g_port = 40000;
uint16_t port_of(const uint8_t* a) { return (uint16_t)((a[2] << 8) | a[3]); }

int16_t ready(const Fd& f, int16_t events) {
    int16_t re = 0;
    if (f.listening) {
        if ((events & 1) && !f.accept_q.empty()) re |= 1;
    } else if (f.connected) {
        if ((events & 1) && (!f.rx.empty() || f.peer_closed)) re |= 1;
        if (events & 4) re |= 4;
        if (f.peer_closed) re |= 0x10;
    } else if (f.eventfd) {
        if ((events & 1) && f.counter) re |= 1;   // POLLIN
        if (events & 4) re |= 4;                  // POLLOUT
    } else if (f.failed_connect) {
        re |= 4 | 8 | 0x10;                       // POLLOUT | POLLERR | POLLHUP
    }
    return re;
}

void handle(Request& r) {
    static std::set<uint32_t> seen;
    {
        std::lock_guard<std::mutex> lk(g_lk);
        if (seen.insert(r.cmd).second) HLOG("bsd", "command %u (input %zu bytes)", r.cmd, r.in_size);
    }
    int32_t a0 = r.arg<int32_t>(0), a1 = r.arg<int32_t>(4), a2 = r.arg<int32_t>(8);
    switch (r.cmd) {
    case 0: case 36: ret(r, 0, 0); return;       // RegisterClient / initialisation
    case 1: return;                              // StartMonitoring
    case 2: case 3: {                            // Socket / SocketExempt(domaine, type, proto)
        std::lock_guard<std::mutex> lk(g_lk);
        int32_t fd = g_next++;
        g_fds[fd] = Fd{};
        ret(r, fd, 0);
        return;
    }
    case 31: {                                   // EventFd(initval u64, flags)
        std::lock_guard<std::mutex> lk(g_lk);
        int32_t fd = g_next++;
        Fd f;
        f.eventfd = true;
        f.counter = r.arg<uint64_t>(0);
        f.semaphore = (r.arg<int32_t>(8) & 1) != 0;
        g_fds[fd] = f;
        ret(r, fd, 0);
        return;
    }
    case 4: ret(r, -1, E_NOENT); return;         // Open
    case 6: {                                    // Poll(nfds, timeout ms) A : pollfd[] → B
        int32_t nfds = a0, timeout = a1;
        auto in = r.in_buf();
        auto out = r.out_buf();
        std::vector<PollFd> p(nfds > 0 ? nfds : 0);
        if (nfds > 0 && in.addr) memcpy(p.data(), (void*)in.addr, std::min<uint64_t>(in.size, p.size() * 8));
        std::unique_lock<std::mutex> lk(g_lk);
        auto count = [&] {
            int n = 0;
            for (auto& q : p) {
                auto it = g_fds.find(q.fd);
                q.revents = it == g_fds.end() ? (q.fd >= 0 ? 0x20 : 0) : ready(it->second, q.events);   // POLLNVAL
                if (q.revents) n++;
            }
            return n;
        };
        int n = count();
        if (!n && timeout != 0) {
            auto dl = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(timeout < 0 ? 1000 : std::min(timeout, 1000));
            while (!(n = count()) && g_cv.wait_until(lk, dl) != std::cv_status::timeout) {}
        }
        if (out.addr) memcpy((void*)out.addr, p.data(), std::min<uint64_t>(out.size, p.size() * 8));
        ret(r, n, 0);
        return;
    }
    case 5: {                                    // Select : rien de prêt après un court délai
        ret(r, 0, 0);
        return;
    }
    case 8: case 9: case 25: {                   // Recv / RecvFrom / Read(fd) → B
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end()) { ret(r, -1, E_BADF); return; }
        Fd& f = it->second;
        if (f.eventfd) {
            if (!f.counter) { ret(r, -1, E_AGAIN); return; }
            uint64_t v = f.semaphore ? 1 : f.counter;
            f.counter -= v;
            auto b = r.out_buf();
            if (b.addr && b.size >= 8) memcpy((void*)b.addr, &v, 8);
            ret(r, 8, 0);
            return;
        }
        if (f.connected) {
            if (f.rx.empty()) { ret(r, f.peer_closed ? 0 : -1, f.peer_closed ? 0 : E_AGAIN); return; }
            auto b = r.out_buf();
            size_t n = std::min<size_t>(f.rx.size(), (size_t)b.size);
            for (size_t i = 0; i < n; i++) ((uint8_t*)b.addr)[i] = f.rx[i];
            f.rx.erase(f.rx.begin(), f.rx.begin() + n);
            ret(r, (int32_t)n, 0);
            return;
        }
        ret(r, -1, E_NOTCONN);
        return;
    }
    case 10: case 11: case 24: {                 // Send / SendTo / Write(fd) + A
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end()) { ret(r, -1, E_BADF); return; }
        Fd& f = it->second;
        if (f.eventfd) {
            auto b = r.in_buf();
            uint64_t v = 0;
            if (b.addr && b.size >= 8) memcpy(&v, (void*)b.addr, 8);
            f.counter += v;
            g_cv.notify_all();
            ret(r, 8, 0);
            return;
        }
        if (f.connected) {
            auto pit = g_fds.find(f.peer);
            if (pit == g_fds.end()) { ret(r, -1, 32 /*EPIPE*/); return; }
            auto b = r.in_buf();
            const uint8_t* d = (const uint8_t*)b.addr;
            pit->second.rx.insert(pit->second.rx.end(), d, d + b.size);
            g_cv.notify_all();
            ret(r, (int32_t)b.size, 0);
            return;
        }
        ret(r, -1, r.cmd == 11 ? E_NETUNREACH : E_NOTCONN);
        return;
    }
    case 12: {                                   // Accept(fd) → B : adresse du pair
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end() || !it->second.listening) { ret(r, -1, E_BADF); return; }
        if (it->second.accept_q.empty()) { ret(r, -1, E_AGAIN); return; }
        int32_t fd = it->second.accept_q.front();
        it->second.accept_q.pop_front();
        auto b = r.out_buf();
        if (b.addr && b.size >= 16) {
            uint8_t a[16] = {16, 2, 0, 0, 127, 0, 0, 1};
            memcpy((void*)b.addr, a, 16);
        }
        ret(r, fd, 0);
        r.push<uint32_t>(16);
        return;
    }
    case 13: {                                   // Bind(fd) + A sockaddr
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end()) { ret(r, -1, E_BADF); return; }
        auto b = r.in_buf();
        memset(it->second.addr, 0, 16);
        if (b.addr) memcpy(it->second.addr, (void*)b.addr, std::min<uint64_t>(b.size, 16));
        it->second.addr[0] = 16;
        it->second.addr[1] = 2;
        if (!port_of(it->second.addr)) {        // port éphémère
            uint16_t p = g_port++;
            it->second.addr[2] = (uint8_t)(p >> 8);
            it->second.addr[3] = (uint8_t)p;
        }
        it->second.bound = true;
        HLOG("bsd", "bind fd %d port %u", a0, port_of(it->second.addr));
        ret(r, 0, 0);
        return;
    }
    case 18: {                                   // Listen
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end()) { ret(r, -1, E_BADF); return; }
        it->second.listening = true;
        ret(r, 0, 0);
        return;
    }
    case 14: {                                   // Connect(fd) + A sockaddr
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end()) { ret(r, -1, E_BADF); return; }
        uint8_t a[16] = {};
        auto b = r.in_buf();
        if (b.addr) memcpy(a, (void*)b.addr, std::min<uint64_t>(b.size, 16));
        bool local = (a[4] == 127) || (!a[4] && !a[5] && !a[6] && !a[7]);
        int32_t lst = -1;
        if (local)
            for (auto& [k, f] : g_fds)
                if (f.listening && port_of(f.addr) == port_of(a)) lst = k;
        HLOG("bsd", "connect fd %d to %u.%u.%u.%u:%u → %s", a0, a[4], a[5], a[6], a[7], port_of(a),
             lst >= 0 ? "loopback" : "unreachable");
        if (lst < 0) {                           // hors boucle locale : réseau injoignable
            it->second.failed_connect = true;
            g_cv.notify_all();
            ret(r, -1, E_NETUNREACH);
            return;
        }
        int32_t srv = g_next++;
        Fd& sf = g_fds[srv];
        sf.connected = true;
        sf.peer = a0;
        memcpy(sf.addr, g_fds[lst].addr, 16);
        Fd& cf = g_fds[a0];
        cf.connected = true;
        cf.peer = srv;
        if (!cf.bound) {
            uint16_t p = g_port++;
            uint8_t ca[16] = {16, 2, (uint8_t)(p >> 8), (uint8_t)p, 127, 0, 0, 1};
            memcpy(cf.addr, ca, 16);
            cf.bound = true;
        }
        g_fds[lst].accept_q.push_back(srv);
        g_cv.notify_all();
        ret(r, 0, 0);
        return;
    }
    case 15: ret(r, -1, E_NOTCONN); return;      // GetPeerName
    case 16: case 17: {                          // GetSockName / GetSockOpt → B (zéros ; SO_ERROR = 0)
        auto b = r.out_buf();
        if (b.addr) memset((void*)b.addr, 0, b.size);
        if (r.cmd == 16) {                       // GetSockName : adresse liée
            std::lock_guard<std::mutex> lk(g_lk);
            auto it = g_fds.find(a0);
            if (it != g_fds.end() && b.addr && b.size >= 16) memcpy((void*)b.addr, it->second.addr, 16);
        }
        if (r.cmd == 17 && a2 == 0x1007) {       // SO_ERROR : erreur de connexion différée
            std::lock_guard<std::mutex> lk(g_lk);
            auto it = g_fds.find(a0);
            int32_t e = it != g_fds.end() && it->second.failed_connect ? E_NETUNREACH : 0;
            if (b.addr && b.size >= 4) memcpy((void*)b.addr, &e, 4);
        }
        ret(r, 0, 0);
        r.push<uint32_t>((uint32_t)b.size);
        return;
    }
    case 19: case 21: case 22: case 23: ret(r, 0, 0); return;   // Ioctl, SetSockOpt, Shutdown…
    case 20: ret(r, 0, 0); return;               // Fcntl
    case 7: ret(r, -1, E_NOTSUP); return;        // Sysctl
    case 26: {                                   // Close
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end()) { ret(r, -1, E_BADF); return; }
        auto pit = g_fds.find(it->second.peer);
        if (it->second.connected && pit != g_fds.end()) pit->second.peer_closed = true;
        g_fds.erase(it);
        g_cv.notify_all();
        ret(r, 0, 0);
        return;
    }
    case 27: {                                   // DuplicateSocket
        std::lock_guard<std::mutex> lk(g_lk);
        auto it = g_fds.find(a0);
        if (it == g_fds.end()) { ret(r, -1, E_BADF); return; }
        int32_t fd = g_next++;
        g_fds[fd] = it->second;
        ret(r, fd, 0);
        return;
    }
    }
    ret(r, -1, E_NOTSUP);
}

struct Registrar {
    Registrar() {
        for (const char* n : {"bsd:u", "bsd:s", "bsd:a"})
            register_service(n, [n] { return svc(n, [](Request& r) { handle(r); return true; }); });
    }
} g_reg;

}  // namespace
}  // namespace hos
