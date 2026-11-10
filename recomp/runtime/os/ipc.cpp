// IPC Horizon côté « serveur » émulé : décodage des messages HIPC (en-tête, handles,
// descripteurs X/A/B/W/C), protocoles CMIF (avec domaines et commandes de contrôle) et TIPC,
// gestionnaire de services `sm:` et services HLE. Un service inconnu répond « succès » avec une
// sortie nulle et journalise chaque commande (une fois) : c'est la carte de ce qu'il reste à
// implémenter.
#include "ipc.h"

#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace hos {

namespace {

constexpr uint32_t SFCI = 0x49434653, SFCO = 0x4F434653;
std::mutex g_ipc;
std::map<std::string, ServiceFactory>& factories() {
    static std::map<std::string, ServiceFactory> f;
    return f;
}
std::set<std::pair<std::string, uint32_t>> g_seen;

struct KSession : KObject {
    const char* kind() const override { return "Session"; }
    std::shared_ptr<Service> svc;
    bool domain = false;
    // Table du domaine, partagée entre la session et ses clones (CloneCurrentObject) :
    // un objet créé via une session est adressable depuis toutes les autres.
    struct Domain {
        std::mutex lk;
        std::map<uint32_t, std::shared_ptr<Service>> objects;
        uint32_t next_id = 1;
    };
    std::shared_ptr<Domain> dom = std::make_shared<Domain>();
    uint32_t add(std::shared_ptr<Service> s) {
        std::lock_guard<std::mutex> g(dom->lk);
        uint32_t id = dom->next_id++;
        dom->objects[id] = std::move(s);
        return id;
    }
    std::shared_ptr<Service> find(uint32_t id) {
        std::lock_guard<std::mutex> g(dom->lk);
        auto it = dom->objects.find(id);
        return it == dom->objects.end() ? nullptr : it->second;
    }
    void erase(uint32_t id) {
        std::lock_guard<std::mutex> g(dom->lk);
        dom->objects.erase(id);
    }
};

uint32_t rd32(uint64_t a) {
    uint32_t v;
    memcpy(&v, (void*)a, 4);
    return v;
}

uint32_t open_session(std::shared_ptr<Service> s) {
    auto ks = std::make_shared<KSession>();
    ks->svc = std::move(s);
    return kernel_new_handle(ks);
}

// ---------------------------------------------------------------- service générique
struct StubService : Service {
    bool root;
    StubService(std::string n, bool is_root) : root(is_root) { name = std::move(n); }
    void handle(Request& r) override {
        // Heuristique (services non implémentés) : à la racine, les commandes 0-2 à petite entrée
        // sont presque toujours des Create…Service qui renvoient une interface : on fournit un
        // objet bouchon (journalisé) pour que le client continue.
        if (root && r.cmd <= 2 && r.in_size <= 16 && r.out_buf().size == 0) {
            HLOG("ipc", "[deviné] %s : commande %u → objet bouchon", name.c_str(), r.cmd);
            r.out_objects.push_back(std::make_shared<StubService>(name + "#" + std::to_string(r.cmd), false));
            return;
        }
        stub(r);
    }
};

// ---------------------------------------------------------------- sm:
struct SmService : Service {
    SmService() { name = "sm:"; }
    void handle(Request& r) override {
        switch (r.cmd) {
        case 0:  // RegisterClient
            return;
        case 1: {  // GetServiceHandle(name u64)
            char nm[9] = {};
            memcpy(nm, r.in, std::min<size_t>(8, r.in_size));
            r.out_move.push_back(ipc_open_service(nm));
            return;
        }
        case 4:  // DetachClient
            return;
        default:
            stub(r);
        }
    }
};

// ---------------------------------------------------------------- décodage / encodage
struct Parsed {
    uint32_t type = 0, nx = 0, na = 0, nb = 0, nw = 0, ndata = 0, recv_mode = 0;
    bool special = false;
    uint64_t data = 0;      // adresse des mots de données
};

void parse(uint64_t buf, Parsed& p, Request& r) {
    uint32_t w0 = rd32(buf), w1 = rd32(buf + 4);
    p.type = w0 & 0xFFFF;
    p.nx = (w0 >> 16) & 0xF;
    p.na = (w0 >> 20) & 0xF;
    p.nb = (w0 >> 24) & 0xF;
    p.nw = (w0 >> 28) & 0xF;
    p.ndata = w1 & 0x3FF;
    p.recv_mode = (w1 >> 10) & 0xF;
    p.special = (w1 >> 31) & 1;
    uint64_t q = buf + 8;
    if (p.special) {
        uint32_t sh = rd32(q);
        q += 4;
        bool pid = sh & 1;
        uint32_t ncopy = (sh >> 1) & 0xF, nmove = (sh >> 5) & 0xF;
        if (pid) {
            memcpy(&r.pid, (void*)q, 8);
            q += 8;
        }
        for (uint32_t i = 0; i < ncopy; i++, q += 4) r.copy_h.push_back(rd32(q));
        for (uint32_t i = 0; i < nmove; i++, q += 4) r.move_h.push_back(rd32(q));
    }
    for (uint32_t i = 0; i < p.nx; i++, q += 8) {
        uint32_t a = rd32(q), lo = rd32(q + 4);
        Request::Buf b;
        b.addr = lo | ((uint64_t)((a >> 12) & 0xF) << 32) | ((uint64_t)((a >> 6) & 0x7) << 36);
        b.size = a >> 16;
        b.mode = (a & 0x3F) | ((a >> 3) & 0x1C0);   // index (compteur)
        r.x.push_back(b);
    }
    auto bufdesc = [&](std::vector<Request::Buf>& v, uint32_t n) {
        for (uint32_t i = 0; i < n; i++, q += 12) {
            uint32_t s = rd32(q), lo = rd32(q + 4), c = rd32(q + 8);
            Request::Buf b;
            b.addr = lo | ((uint64_t)((c >> 28) & 0xF) << 32) | ((uint64_t)((c >> 2) & 0x7) << 36);
            b.size = s | ((uint64_t)((c >> 24) & 0xF) << 32);
            b.mode = c & 3;
            v.push_back(b);
        }
    };
    bufdesc(r.a, p.na);
    bufdesc(r.b, p.nb);
    bufdesc(r.w, p.nw);
    p.data = q;
    // liste de réception (C) : après les données
    uint32_t nc = p.recv_mode >= 2 ? (p.recv_mode == 2 ? 1 : p.recv_mode - 2) : 0;
    uint64_t c = p.data + 4ull * p.ndata;
    for (uint32_t i = 0; i < nc; i++, c += 8) {
        uint32_t lo = rd32(c), hi = rd32(c + 4);
        Request::Buf b;
        b.addr = lo | ((uint64_t)(hi & 0xFFFF) << 32);
        b.size = hi >> 16;
        b.mode = 0;
        r.c.push_back(b);
    }
}

// écrit la réponse dans `buf` (même format HIPC)
void write_reply(uint64_t buf, uint64_t bufsize, const Request& r, bool tipc, bool domain,
                 const std::vector<uint32_t>& out_ids) {
    std::vector<uint32_t> w;
    w.push_back(0);   // en-tête complété plus bas
    w.push_back(0);
    bool special = !r.out_copy.empty() || !r.out_move.empty();
    if (special)
        w.push_back(((uint32_t)r.out_copy.size() << 1) | ((uint32_t)r.out_move.size() << 5));
    for (uint32_t h : r.out_copy) w.push_back(h);
    for (uint32_t h : r.out_move) w.push_back(h);
    size_t data_start = w.size();
    std::vector<uint8_t> raw;
    if (tipc) {
        raw.resize(4);
        memcpy(raw.data(), &r.result, 4);
        raw.insert(raw.end(), r.out.begin(), r.out.end());
    } else {
        // alignement sur 16 octets (par rapport au début du message)
        size_t pad = (16 - (data_start * 4) % 16) % 16;
        raw.assign(pad, 0);
        if (domain) {
            uint32_t dh[4] = {(uint32_t)out_ids.size(), 0, 0, 0};
            raw.insert(raw.end(), (uint8_t*)dh, (uint8_t*)dh + 16);
        }
        uint32_t oh[4] = {SFCO, 0, r.result, 0};
        raw.insert(raw.end(), (uint8_t*)oh, (uint8_t*)oh + 16);
        raw.insert(raw.end(), r.out.begin(), r.out.end());
        for (uint32_t id : out_ids) raw.insert(raw.end(), (uint8_t*)&id, (uint8_t*)&id + 4);
        raw.resize(raw.size() + 16 - pad, 0);   // marge comme les serveurs Horizon
    }
    raw.resize((raw.size() + 3) & ~size_t(3), 0);
    size_t nwords = raw.size() / 4;
    w[0] = 0;
    w[1] = (uint32_t)nwords | (special ? 0x80000000u : 0);
    size_t total = w.size() * 4 + raw.size();
    if (total > bufsize) panic("IPC : réponse trop grande (%zu > %llu)", total, (unsigned long long)bufsize);
    memcpy((void*)buf, w.data(), w.size() * 4);
    memcpy((void*)(buf + w.size() * 4), raw.data(), raw.size());
}

}  // namespace

// ---------------------------------------------------------------- API services
void Service::stub(Request& r) {
    bool first;
    {
        std::lock_guard<std::mutex> lk(g_ipc);
        first = g_seen.insert({name, r.cmd}).second;
    }
    if (first || g_verbose >= 2)
        HLOG("ipc", "[bouchon] %s : commande %u (entrée %zu o, A%zu B%zu X%zu C%zu W%zu, "
             "handles c%zu m%zu)", name.c_str(), r.cmd, r.in_size, r.a.size(), r.b.size(),
             r.x.size(), r.c.size(), r.w.size(), r.copy_h.size(), r.move_h.size());
    r.out.assign(0x40, 0);
}

void register_service(const std::string& name, ServiceFactory f) { factories()[name] = std::move(f); }

std::shared_ptr<Service> make_service(const std::string& name) {
    auto it = factories().find(name);
    if (it != factories().end()) return it->second();
    return std::make_shared<StubService>(name, true);
}

uint32_t ipc_open_service(const std::string& name) {
    bool known = factories().count(name) != 0;
    HLOG("ipc", "ouverture du service « %s »%s", name.c_str(), known ? "" : " (bouchon)");
    return open_session(make_service(name));
}

uint32_t ipc_new_session(std::shared_ptr<Service> s) { return open_session(std::move(s)); }

uint32_t ipc_connect_named_port(const std::string& name) {
    if (name == "sm:") return open_session(std::make_shared<SmService>());
    HLOG("ipc", "ConnectToNamedPort(« %s ») : port inconnu", name.c_str());
    return 0;
}

uint32_t ipc_send_sync_request(CPUState* ctx, uint32_t handle, uint64_t buf, uint64_t size) {
    (void)ctx;
    KSession* ks = dynamic_cast<KSession*>(kernel_get(handle));
    if (!ks) {
        HLOG("ipc", "SendSyncRequest sur un handle invalide 0x%x", handle);
        return kr::InvalidHandle;
    }
    Request r;
    Parsed p;
    parse(buf, p, r);
    r.ctx = ctx;
    std::shared_ptr<Service> target = ks->svc;
    bool tipc = p.type >= 16;
    if (p.type == 2 || p.type == 15) {   // Close (CMIF / TIPC)
        HDBG("ipc", "fermeture de session %s", ks->svc->name.c_str());
        return 0;
    }
    std::vector<uint32_t> out_ids;
    if (tipc) {
        r.cmd = p.type - 16;
        r.in = (const uint8_t*)p.data;
        r.in_size = 4ull * p.ndata;
    } else {
        uint64_t raw = (p.data + 15) & ~15ull;
        uint64_t hdr = raw;
        uint32_t dom_type = 0, dom_obj = 0, dom_size = 0, dom_nin = 0;
        if (ks->domain && (p.type == 4 || p.type == 6)) {
            uint8_t b[16];
            memcpy(b, (void*)raw, 16);
            dom_type = b[0];
            dom_nin = b[1];
            dom_size = b[2] | (b[3] << 8);
            memcpy(&dom_obj, b + 4, 4);
            hdr = raw + 16;
            if (dom_type == 2) {   // fermeture d'un objet du domaine
                ks->erase(dom_obj);
                Request rr;
                write_reply(buf, size, rr, false, true, {});
                return 0;
            }
            target = ks->find(dom_obj);
            if (!target) {
                HLOG("ipc", "%s : objet de domaine %u inconnu", ks->svc->name.c_str(), dom_obj);
                return kr::InvalidHandle;
            }
            for (uint32_t i = 0; i < dom_nin; i++) r.in_objects.push_back(rd32(hdr + dom_size + 4 * i));
        }
        if (rd32(hdr) != SFCI)
            HLOG("ipc", "%s : en-tête CMIF inattendu 0x%08x (type %u)", target->name.c_str(), rd32(hdr), p.type);
        r.cmd = rd32(hdr + 8);
        r.in = (const uint8_t*)(hdr + 16);
        size_t avail = 4ull * p.ndata - (hdr + 16 - p.data);
        r.in_size = ks->domain && dom_size >= 16 ? std::min<size_t>(dom_size - 16, avail) : avail;
        if (p.type == 5 || p.type == 7) {   // commandes de contrôle
            switch (r.cmd) {
            case 0: {   // ConvertCurrentObjectToDomain
                ks->domain = true;
                uint32_t id = ks->add(ks->svc);
                r.out.resize(4);
                memcpy(r.out.data(), &id, 4);
                write_reply(buf, size, r, false, false, {});
                return 0;
            }
            case 1: {   // CopyFromCurrentDomain(id) → handle
                uint32_t id = r.arg<uint32_t>(0);
                auto o = ks->find(id);
                if (!o) r.result = kr::InvalidHandle;
                else r.out_move.push_back(open_session(o));
                write_reply(buf, size, r, false, false, {});
                return 0;
            }
            case 2:     // CloneCurrentObject
            case 4: {   // CloneCurrentObjectEx
                auto ns = std::make_shared<KSession>();
                ns->svc = ks->svc;
                ns->domain = ks->domain;
                ns->dom = ks->dom;
                r.out_move.push_back(kernel_new_handle(ns));
                write_reply(buf, size, r, false, false, {});
                return 0;
            }
            case 3: {   // QueryPointerBufferSize
                uint16_t sz = ks->svc->pointer_buffer_size;
                r.out.resize(4, 0);
                memcpy(r.out.data(), &sz, 2);
                write_reply(buf, size, r, false, false, {});
                return 0;
            }
            default:
                HLOG("ipc", "commande de contrôle %u inconnue", r.cmd);
                r.result = kr::NotImplemented;
                write_reply(buf, size, r, false, false, {});
                return 0;
            }
        }
    }
    HDBG("ipc", "%s : commande %u (%s, %zu o)", target->name.c_str(), r.cmd, tipc ? "tipc" : "cmif", r.in_size);
    target->handle(r);
    // objets de sortie : ids de domaine ou nouvelles sessions
    for (auto& o : r.out_objects) {
        if (ks->domain) out_ids.push_back(ks->add(o));
        else r.out_move.push_back(open_session(o));
    }
    write_reply(buf, size, r, tipc, ks->domain && !tipc, out_ids);
    return 0;
}

}  // namespace hos
