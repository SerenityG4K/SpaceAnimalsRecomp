// Espace d'adressage invité : réservation à adresse fixe (identité invité == hôte), carte des
// régions façon Horizon (état / permissions / attributs) pour svcQueryMemory & co.
#include <atomic>
#include "hos.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <map>
#include <mutex>

namespace hos {
namespace {

struct Reg {
    uint64_t end;
    uint32_t state, perm, attr;
};
std::map<uint64_t, Reg> g_regs;  // début → région ; couvre exactement [AS_BASE, AS_END)
std::recursive_mutex g_mlock;

// coupe la région qui contient `at` pour qu'une région commence exactement en `at`
void split(uint64_t at) {
    if (at <= AS_BASE || at >= AS_END) return;
    auto it = g_regs.upper_bound(at);
    --it;
    if (it->first == at) return;
    Reg r = it->second;
    it->second.end = at;
    g_regs[at] = r;
}

void merge_around(uint64_t a, uint64_t b) {
    auto it = g_regs.lower_bound(a);
    if (it != g_regs.begin()) --it;
    while (it != g_regs.end() && it->first <= b) {
        auto nx = std::next(it);
        if (nx == g_regs.end()) break;
        const Reg &x = it->second, &y = nx->second;
        if (x.end == nx->first && x.state == y.state && x.perm == y.perm && x.attr == y.attr) {
            it->second.end = y.end;
            g_regs.erase(nx);
        } else {
            it = nx;
        }
    }
}

template <class F>
void for_range(uint64_t addr, uint64_t size, F f) {
    uint64_t end = addr + size;
    split(addr);
    split(end);
    for (auto it = g_regs.lower_bound(addr); it != g_regs.end() && it->first < end; ++it) f(it->second);
    merge_around(addr, end);
}

}  // namespace

void mem_init() {
    void* p = VirtualAlloc((void*)AS_BASE, AS_END - AS_BASE, MEM_RESERVE, PAGE_NOACCESS);
    if (p != (void*)AS_BASE) {
        DWORD err = GetLastError();
        // diagnostic : qui occupe déjà la zone ?
        for (uint64_t a = AS_BASE; a < AS_END;) {
            MEMORY_BASIC_INFORMATION mbi;
            if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
            if (mbi.State != MEM_FREE)
                HLOG("mem", "zone invitée déjà occupée : 0x%llx (+0x%llx), type 0x%lx",
                     (unsigned long long)mbi.BaseAddress, (unsigned long long)mbi.RegionSize, mbi.Type);
            a = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
        }
        SetLastError(err);
    }
    if (p != (void*)AS_BASE)
        panic("impossible de réserver l'espace invité [0x%llx, 0x%llx) (erreur %lu)",
              (unsigned long long)AS_BASE, (unsigned long long)AS_END, GetLastError());
    g_regs.clear();
    g_regs[AS_BASE] = Reg{AS_END, MS_FREE, MP_NONE, 0};
}

bool mem_commit(uint64_t addr, uint64_t size) {
    if (!size) return true;
    void* p = VirtualAlloc((void*)addr, size, MEM_COMMIT, PAGE_READWRITE);
    if (!p) {
        HLOG("mem", "échec d'engagement 0x%llx+0x%llx (erreur %lu)", (unsigned long long)addr,
             (unsigned long long)size, GetLastError());
        return false;
    }
    return true;
}

// génération des mappages : invalide les caches de mem_valid à chaque désengagement
std::atomic<uint64_t> g_mem_gen{1};

void mem_decommit(uint64_t addr, uint64_t size) {
    if (size) VirtualFree((void*)addr, size, MEM_DECOMMIT);
    g_mem_gen++;
}

void mem_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm, uint32_t attr) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for_range(addr, size, [&](Reg& r) { r.state = state; r.perm = perm; r.attr = attr; });
}

void mem_set_perm(uint64_t addr, uint64_t size, uint32_t perm) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for_range(addr, size, [&](Reg& r) { r.perm = perm; });
}

void mem_set_attr(uint64_t addr, uint64_t size, uint32_t mask, uint32_t attr) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for_range(addr, size, [&](Reg& r) { r.attr = (r.attr & ~mask) | (attr & mask); });
}

MemInfo mem_query(uint64_t addr) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    MemInfo mi{};
    if (addr < AS_BASE) {
        mi.addr = 0;
        mi.size = AS_BASE;
        mi.state = MS_INACCESSIBLE;
        return mi;
    }
    if (addr >= AS_END) {
        mi.addr = AS_END;
        mi.size = 0 - AS_END;
        mi.state = MS_INACCESSIBLE;
        return mi;
    }
    auto it = g_regs.upper_bound(addr);
    --it;
    mi.addr = it->first;
    mi.size = it->second.end - it->first;
    mi.state = it->second.state;
    mi.perm = it->second.perm;
    mi.attr = it->second.attr;
    return mi;
}

bool mem_check(uint64_t addr, uint64_t size, uint32_t state) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    if (addr < AS_BASE || addr + size > AS_END || addr + size < addr) return false;
    uint64_t a = addr, end = addr + size;
    while (a < end) {
        auto it = g_regs.upper_bound(a);
        --it;
        if (it->second.state != state) return false;
        a = it->second.end;
    }
    return true;
}

uint64_t mem_find_free(uint64_t base, uint64_t end, uint64_t size, uint64_t align) {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    for (auto it = g_regs.begin(); it != g_regs.end(); ++it) {
        if (it->second.state != MS_FREE) continue;
        uint64_t s = std::max(it->first, base), e = std::min(it->second.end, end);
        s = (s + align - 1) & ~(align - 1);
        if (s < e && e - s >= size) return s;
    }
    return 0;
}

// VirtualQuery coûte ≈ 25 µs ici (grand espace réservé) : cache par thread des dernières zones
// engagées trouvées, invalidé par g_mem_gen (l'engagement ne fait qu'ajouter des zones valides).
bool mem_valid(uint64_t addr, uint64_t size) {
    struct Zone { uint64_t gen = 0, s = 0, e = 0; };
    thread_local Zone cache[8];
    thread_local unsigned next = 0;
    uint64_t gen = g_mem_gen.load(std::memory_order_acquire), end = addr + size;
    for (auto& z : cache)
        if (z.gen == gen && addr >= z.s && end <= z.e) return true;
    uint64_t a = addr & ~0xfffull;
    uint64_t zs = 0;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        if (!zs) zs = (uint64_t)mbi.BaseAddress;
        a = (uint64_t)mbi.BaseAddress + mbi.RegionSize;
    }
    cache[next++ % 8] = Zone{gen, zs, a};
    return true;
}

uint64_t mem_used() {
    std::lock_guard<std::recursive_mutex> lk(g_mlock);
    uint64_t n = 0;
    for (auto& [s, r] : g_regs)
        if (r.state != MS_FREE && r.state != MS_INACCESSIBLE) n += r.end - s;
    return n;
}

}  // namespace hos
