// Services HLE (IPC) : interface commune aux implémentations de services.
#pragma once
#include "hos.h"

#include <algorithm>
#include <cstring>
#include <functional>

namespace hos {

struct Service;

struct Request {
    CPUState* ctx = nullptr;
    uint32_t cmd = 0;
    uint64_t pid = 0;
    std::vector<uint32_t> copy_h, move_h;
    struct Buf {
        uint64_t addr = 0, size = 0;
        uint32_t mode = 0;
    };
    std::vector<Buf> x, a, b, w, c;   // X : envoi « pointeur », A : envoi, B : réception, C : réception pointeur
    const uint8_t* in = nullptr;      // arguments (après l'en-tête CMIF)
    size_t in_size = 0;
    std::vector<uint32_t> in_objects;
    // sortie
    uint32_t result = 0;
    std::vector<uint8_t> out;
    std::vector<uint32_t> out_copy, out_move;
    std::vector<std::shared_ptr<Service>> out_objects;

    template <class T>
    T arg(size_t off) const {
        T v{};
        if (off + sizeof(T) <= in_size) memcpy(&v, in + off, sizeof v);
        return v;
    }
    template <class T>
    void push(const T& v) {
        size_t o = out.size();
        out.resize(o + sizeof(T));
        memcpy(out.data() + o, &v, sizeof v);
    }
    // tampon de sortie de type B ou C (le premier disponible)
    Buf out_buf(size_t i = 0) const {
        if (i < b.size() && b[i].addr) return b[i];
        if (i < c.size()) return c[i];
        return i < b.size() ? b[i] : Buf{};
    }
    Buf in_buf(size_t i = 0) const {
        if (i < a.size() && a[i].addr) return a[i];
        if (i < x.size()) return x[i];
        return i < a.size() ? a[i] : Buf{};
    }
};

struct Service {
    std::string name;
    uint16_t pointer_buffer_size = 0x8000;
    virtual ~Service() = default;
    virtual void handle(Request& r) { stub(r); }
    void stub(Request& r);   // journalise la commande et répond succès + zéros
};

using ServiceFactory = std::function<std::shared_ptr<Service>()>;
void register_service(const std::string& name, ServiceFactory f);
std::shared_ptr<Service> make_service(const std::string& name);
uint32_t ipc_new_session(std::shared_ptr<Service> s);

}  // namespace hos
