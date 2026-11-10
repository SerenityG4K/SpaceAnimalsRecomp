// Petits outils communs aux implémentations de services HLE.
#pragma once
#include "ipc.h"

namespace hos {

struct Event {   // événement noyau détenu par un service
    std::shared_ptr<KObject> obj;
    uint32_t handle = 0;
    explicit Event(bool sig = false, const char* tag = "") { handle = kernel_create_event(&obj, sig, tag); }
    uint32_t copy() { return kernel_new_handle(obj); }   // nouveau handle (copie) pour le client
};
void kernel_signal(KObject* ev, bool on);   // kernel.cpp
// mémoire partagée créée par un service : handle (copie pour le client) et accès au contenu
// (adresse de mapping si le client l'a mappée, sinon tampon interne)
uint32_t kernel_create_shared_memory(uint64_t size, std::shared_ptr<KObject>* out);
uint8_t* kernel_shmem_ptr(KObject* o);
std::shared_ptr<KObject> kernel_last_created_event();   // dernier svcCreateEvent du thread courant

// service défini par une lambda (Request&) → bool (false : bouchon générique)
template <class F>
struct Lambda : Service {
    F f;
    Lambda(const char* n, F fn) : f(fn) { name = n; }
    void handle(Request& r) override {
        if (!f(r)) stub(r);
    }
};
template <class F>
std::shared_ptr<Service> svc(const char* n, F f) {
    return std::make_shared<Lambda<F>>(n, f);
}

}  // namespace hos
