// Chargeur natif des modules Horizon (remplace rtld) : mapping des images à leurs bases fixes,
// relocations (RELA, JMPREL, RELR), résolution globale des symboles dans l'ordre des modules
// (main d'abord : ses exports — malloc, free… — remplacent ceux du SDK, comme sur console),
// symbolisation des adresses pour les journaux.
#include "hos.h"

#include <cxxabi.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace hos {

std::vector<Module> g_modules;
std::string g_nup_path;
std::vector<RomfsFile> g_romfs;

namespace {

constexpr uint32_t R_ABS64 = 257, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027,
                   R_TLSDESC = 1031;
constexpr int64_t DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
                  DT_RELASZ = 8, DT_STRSZ = 10, DT_SYMENT = 11, DT_INIT = 12, DT_FINI = 13,
                  DT_JMPREL = 23, DT_RELRSZ = 35, DT_RELR = 36;

struct Sym {
    uint32_t name;
    uint8_t info, other;
    uint16_t shndx;
    uint64_t value, size;
};
static_assert(sizeof(Sym) == 24, "Sym");

struct DynInfo {
    uint64_t symtab = 0, strtab = 0, syment = 24, strsz = 0, nsym = 0;
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, relr = 0, relrsz = 0;
};
std::vector<DynInfo> g_dyn;
std::unordered_map<std::string, uint64_t> g_global;            // 1re définition (ordre modules)
std::vector<std::vector<std::pair<uint64_t, const char*>>> g_bysym;  // par module, trié
std::vector<std::string> g_unresolved_names;

template <class T>
T rd(uint64_t a) {
    T v;
    memcpy(&v, (const void*)a, sizeof v);
    return v;
}
void wr64(uint64_t a, uint64_t v) { memcpy((void*)a, &v, 8); }

uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

// Import non résolu appelé (via PLT : x17 contient l'adresse cible)
void unresolved_stub(CPUState* ctx) {
    panic("appel d'un import non résolu %s (x17=0x%llx, lr=%s)", symbolize(ctx->x[17]).c_str(),
          (unsigned long long)ctx->x[17], symbolize(ctx->x[30]).c_str());
}

void tlsdesc_stub(CPUState* ctx) {
    panic("accès TLS par descripteur (TLSDESC) non géré : argument 0x%llx, appelant %s",
          (unsigned long long)ctx->x[0], symbolize(ctx->x[30]).c_str());
}

void relocate(size_t mi) {
    Module& m = g_modules[mi];
    const DynInfo& d = g_dyn[mi];
    uint64_t B = m.base;
    size_t n_rel = 0, n_sym = 0, n_weak = 0, n_unres = 0, n_tls = 0;
    auto sym_value = [&](uint32_t idx, bool& ok) -> uint64_t {
        ok = true;
        const Sym s = rd<Sym>(B + d.symtab + (uint64_t)idx * d.syment);
        const char* name = (const char*)(B + d.strtab + s.name);
        uint8_t bind = s.info >> 4;
        if (bind == 0 /*LOCAL*/ && s.shndx) return B + s.value;
        if (*name) {
            auto it = g_global.find(name);
            if (it != g_global.end()) return it->second;
        }
        if (s.shndx) return B + s.value;
        if (bind == 2 /*WEAK*/) {
            n_weak++;
            return 0;
        }
        n_unres++;
        g_unresolved_names.push_back(name);
        HLOG("loader", "%s : symbole non résolu %s", m.name.c_str(), name);
        uint64_t a = register_native((std::string("non résolu:") + name).c_str(), unresolved_stub);
        g_global[name] = a;
        ok = false;
        return a;
    };
    for (int t = 0; t < 2; t++) {
        uint64_t off = t ? d.jmprel : d.rela, size = t ? d.pltrelsz : d.relasz;
        if (!off) continue;
        for (uint64_t i = 0; i < size / 24; i++) {
            uint64_t r_off = rd<uint64_t>(B + off + i * 24);
            uint64_t r_info = rd<uint64_t>(B + off + i * 24 + 8);
            int64_t r_add = rd<int64_t>(B + off + i * 24 + 16);
            uint32_t type = (uint32_t)r_info, sym = (uint32_t)(r_info >> 32);
            bool ok;
            switch (type) {
            case R_RELATIVE: wr64(B + r_off, B + (uint64_t)r_add); n_rel++; break;
            case R_GLOB_DAT:
            case R_JUMP_SLOT:
            case R_ABS64: {
                uint64_t v = sym_value(sym, ok);
                wr64(B + r_off, v ? v + (uint64_t)r_add : 0);
                n_sym++;
                break;
            }
            case R_TLSDESC: {
                // descripteur TLS (2 mots : résolveur, argument). Non géré pour l'instant :
                // le résolveur natif s'arrête avec un message clair si ce code est appelé.
                static uint64_t res = register_native("tlsdesc:non_géré", tlsdesc_stub);
                wr64(B + r_off, res);
                wr64(B + r_off + 8, ((uint64_t)mi << 56) | (uint64_t)r_add);
                n_tls++;
                break;
            }
            default:
                panic("%s : relocation de type %u non gérée à +0x%llx", m.name.c_str(), type,
                      (unsigned long long)r_off);
            }
        }
    }
    // RELR (relatives compactées)
    if (d.relr) {
        uint64_t where = 0;
        for (uint64_t i = 0; i < d.relrsz / 8; i++) {
            uint64_t e = rd<uint64_t>(B + d.relr + i * 8);
            if (!(e & 1)) {
                wr64(B + e, rd<uint64_t>(B + e) + B);
                where = e + 8;
                n_rel++;
            } else {
                uint64_t bits = e >> 1;
                for (int j = 0; bits; j++, bits >>= 1)
                    if (bits & 1) {
                        wr64(B + where + j * 8, rd<uint64_t>(B + where + j * 8) + B);
                        n_rel++;
                    }
                where += 63 * 8;
            }
        }
    }
    HLOG("loader", "%-8s relogé : %zu relatives, %zu symboliques (%zu faibles nulles, %zu non résolues)%s",
         m.name.c_str(), n_rel, n_sym, n_weak, n_unres,
         n_tls ? (", " + std::to_string(n_tls) + " TLSDESC non gérés").c_str() : "");
}

}  // namespace

void load_modules(const std::string& manifest) {
    std::ifstream f(manifest);
    if (!f) panic("manifeste introuvable : %s", manifest.c_str());
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream is(line);
        Module m;
        std::string base, ts, ro, data, path;
        is >> m.name;
        if (m.name == "nup") {   // conteneur d'origine (lecture seule) pour la romfs
            std::getline(is, g_nup_path);
            g_nup_path.erase(0, g_nup_path.find_first_not_of(" 	"));
            continue;
        }
        if (m.name == "romfs") {   // fichier de la romfs stocké brut dans le .nup
            RomfsFile rf;
            std::string off, size;
            is >> off >> size;
            std::getline(is, rf.path);
            rf.path.erase(0, rf.path.find_first_not_of(" 	"));
            rf.nup_offset = strtoull(off.c_str(), nullptr, 0);
            rf.size = strtoull(size.c_str(), nullptr, 0);
            g_romfs.push_back(rf);
            continue;
        }
        is >> base >> ts >> ro >> data;
        std::getline(is, path);
        path.erase(0, path.find_first_not_of(" \t"));
        m.base = strtoull(base.c_str(), nullptr, 0);
        m.text_size = strtoull(ts.c_str(), nullptr, 0);
        m.ro_off = strtoull(ro.c_str(), nullptr, 0);
        m.data_off = strtoull(data.c_str(), nullptr, 0);
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) panic("image introuvable : %s", path.c_str());
        _fseeki64(fp, 0, SEEK_END);
        m.size = (uint64_t)_ftelli64(fp);
        _fseeki64(fp, 0, SEEK_SET);
        uint64_t msize = align_up(m.size, 0x1000);
        if (m.base < CODE_BASE || m.base + msize > CODE_END) panic("%s : base hors de la zone de code", m.name.c_str());
        if (!mem_commit(m.base, msize)) panic("%s : mapping impossible", m.name.c_str());
        if (fread((void*)m.base, 1, m.size, fp) != m.size) panic("%s : lecture incomplète", m.name.c_str());
        fclose(fp);
        // MOD0
        uint32_t mod0 = rd<uint32_t>(m.base + 4);
        if (memcmp((void*)(m.base + mod0), "MOD0", 4)) panic("%s : MOD0 absent", m.name.c_str());
        int32_t dyn = rd<int32_t>(m.base + mod0 + 4), bs = rd<int32_t>(m.base + mod0 + 8),
                be = rd<int32_t>(m.base + mod0 + 12);
        m.dynamic = mod0 + dyn;
        m.bss_off = mod0 + bs;
        m.bss_end = mod0 + be;
        if (m.bss_end > m.size) panic("%s : .bss hors image", m.name.c_str());
        memset((void*)(m.base + m.bss_off), 0, m.bss_end - m.bss_off);
        // carte mémoire : text RX, ro R, data+bss RW (états Code / CodeData)
        mem_set(m.base, align_up(m.text_size, 0x1000), MS_CODE, MP_RX);
        mem_set(m.base + m.ro_off, m.data_off - m.ro_off, MS_CODE, MP_R);
        mem_set(m.base + m.data_off, msize - m.data_off, MS_CODEDATA, MP_RW);
        // .dynamic
        DynInfo d;
        for (uint64_t p = m.base + m.dynamic;; p += 16) {
            int64_t tag = rd<int64_t>(p);
            uint64_t val = rd<uint64_t>(p + 8);
            if (tag == DT_NULL) break;
            switch (tag) {
            case DT_SYMTAB: d.symtab = val; break;
            case DT_STRTAB: d.strtab = val; break;
            case DT_SYMENT: d.syment = val; break;
            case DT_STRSZ: d.strsz = val; break;
            case DT_RELA: d.rela = val; break;
            case DT_RELASZ: d.relasz = val; break;
            case DT_JMPREL: d.jmprel = val; break;
            case DT_PLTRELSZ: d.pltrelsz = val; break;
            case DT_RELR: d.relr = val; break;
            case DT_RELRSZ: d.relrsz = val; break;
            case DT_INIT: m.init = m.base + val; break;
            case DT_FINI: m.fini = m.base + val; break;
            default: break;
            }
        }
        d.nsym = d.strtab > d.symtab ? (d.strtab - d.symtab) / d.syment : 0;
        for (uint64_t i = 0; i < d.nsym; i++) {
            Sym s = rd<Sym>(m.base + d.symtab + i * d.syment);
            const char* name = (const char*)(m.base + d.strtab + s.name);
            if (!*name) continue;
            if (!s.shndx) {
                m.imports.emplace_back(name);
                continue;
            }
            m.exports.emplace_back(name, m.base + s.value);
        }
        HLOG("loader", "%-8s 0x%llx  %6.1f Mo  %zu exports", m.name.c_str(), (unsigned long long)m.base,
             m.size / 1048576.0, m.exports.size());
        g_modules.push_back(std::move(m));
        g_dyn.push_back(d);
    }
    // portée globale : 1re définition dans l'ordre des modules (main, subsdk*, sdk)
    for (auto& m : g_modules)
        for (auto& [n, a] : m.exports) g_global.emplace(n, a);
    for (size_t i = 0; i < g_modules.size(); i++) relocate(i);
    // tables de symbolisation
    g_bysym.resize(g_modules.size());
    for (size_t i = 0; i < g_modules.size(); i++) {
        for (auto& [n, a] : g_modules[i].exports) g_bysym[i].emplace_back(a, n.c_str());
        std::sort(g_bysym[i].begin(), g_bysym[i].end());
    }
}

const Module* module_at(uint64_t addr) {
    for (auto& m : g_modules)
        if (addr >= m.base && addr < m.base + m.size) return &m;
    return nullptr;
}

uint64_t find_export(const std::string& name) {
    auto it = g_global.find(name);
    return it == g_global.end() ? 0 : it->second;
}

std::string demangle(const char* name) {
    int st = 0;
    char* d = abi::__cxa_demangle(name, nullptr, nullptr, &st);
    if (st || !d) return name;
    std::string s = d;
    free(d);
    return s;
}

std::string native_name(uint64_t addr);  // kernel.cpp

std::string symbolize(uint64_t addr) {
    char buf[64];
    if (addr >= NATIVE_BASE && addr < NATIVE_BASE + 0x1000000) return "natif!" + native_name(addr);
    for (size_t i = 0; i < g_modules.size(); i++) {
        const Module& m = g_modules[i];
        if (addr < m.base || addr >= m.base + m.size) continue;
        auto& v = g_bysym[i];
        auto it = std::upper_bound(v.begin(), v.end(), std::make_pair(addr, (const char*)~0ull));
        if (it != v.begin()) {
            --it;
            if (addr - it->first < 0x40000) {
                snprintf(buf, sizeof buf, "+0x%llx", (unsigned long long)(addr - it->first));
                return m.name + "!" + demangle(it->second) + (addr != it->first ? buf : "");
            }
        }
        snprintf(buf, sizeof buf, "+0x%llx", (unsigned long long)(addr - m.base));
        return m.name + buf;
    }
    snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)addr);
    return buf;
}

}  // namespace hos
