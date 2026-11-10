// Infrastructure commune de la HLE NVN (hle_gfx.cpp, nvn_cmd.cpp, vk_*.cpp) : registre des fonctions
// natives (macro NVN), identité des objets NVN (identifiant inscrit dans l'objet), tables d'état.
#pragma once
#include "hos.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hos {

std::map<std::string, RecompFn>& nvn_impls();
void nvn_trace(const char* name, CPUState* ctx, bool stub);

namespace nvn {

#define A(n) (ctx->x[n])
inline void R(CPUState* ctx, uint64_t v) { ctx->x[0] = v; }
template <class T>
inline T* P(uint64_t a) { return (T*)a; }
inline const char* S(uint64_t a) { return a ? (const char*)a : "(null)"; }

struct Reg {
    Reg(const char* n, RecompFn f) { nvn_impls()[n] = f; }
};
// Déclare une fonction NVN native (enregistrée sous son nom pour nvnDeviceGetProcAddress).
#define NVN(name) \
    void name##_impl(CPUState* ctx); \
    void name(CPUState* ctx) { \
        if (::hos::g_trace_nvn) ::hos::nvn_trace(#name, ctx, false); \
        name##_impl(ctx); \
    } \
    ::hos::nvn::Reg reg_##name(#name, name); \
    void name##_impl(CPUState* ctx)

// ================================================================ NVN : objets
// Identité des objets NVN : le jeu peut copier un objet initialisé (c'est une structure opaque dont
// tout l'état vit dans sa mémoire sur la vraie machine). À l'initialisation (SetDefaults /
// Initialize), on écrit donc dans ses 8 premiers octets un identifiant (magique | id) qui suit les
// copies ; les tables hôte sont indexées par cet identifiant (repli : l'adresse de l'objet).
constexpr uint32_t OBJ_MAGIC = 0x4E564831;   // « NVH1 »
extern std::atomic<uint32_t> g_obj_id;
inline uint64_t obj_key(uint64_t a) {
    if (!a) return 0;
    uint64_t v = *(volatile uint64_t*)a;
    return (v >> 32) == OBJ_MAGIC ? (1ull << 63) | (v & 0xffffffffu) : a;
}
inline uint64_t obj_new(uint64_t a) {
    uint32_t id = g_obj_id++;
    *(volatile uint64_t*)a = ((uint64_t)OBJ_MAGIC << 32) | id;
    return (1ull << 63) | id;
}

template <class T>
struct Table {
    std::mutex m;
    std::unordered_map<uint64_t, T> map;
    T& operator[](uint64_t obj) {
        uint64_t k = obj_key(obj);
        std::lock_guard<std::mutex> lk(m);
        return map[k];
    }
    T& create(uint64_t obj, T v = T{}) {   // nouvel objet (SetDefaults / Initialize)
        uint64_t k = obj_new(obj);
        std::lock_guard<std::mutex> lk(m);
        return map[k] = std::move(v);
    }
    bool find(uint64_t obj, T* out) {
        uint64_t k = obj_key(obj);
        std::lock_guard<std::mutex> lk(m);
        auto it = map.find(k);
        if (it == map.end()) return false;
        if (out) *out = it->second;
        return true;
    }
    bool find_key(uint64_t k, T* out) {   // clé déjà calculée (obj_key)
        std::lock_guard<std::mutex> lk(m);
        auto it = map.find(k);
        if (it == map.end()) return false;
        if (out) *out = it->second;
        return true;
    }
    template <class F>
    bool with_key(uint64_t k, F&& f) {   // f(const T&) sous verrou, sans copier l'objet
        std::lock_guard<std::mutex> lk(m);
        auto it = map.find(k);
        if (it == map.end()) return false;
        f(it->second);
        return true;
    }
    void erase(uint64_t obj) {
        uint64_t k = obj_key(obj);
        std::lock_guard<std::mutex> lk(m);
        map.erase(k);
    }
};

struct PoolBuilder { uint64_t storage = 0, size = 0; uint32_t flags = 0; };
struct Pool { uint64_t storage = 0, size = 0; uint32_t flags = 0; };
struct BufBuilder { uint64_t pool = 0, offset = 0, size = 0; };
struct Buf { uint64_t addr = 0, size = 0; };
struct TexBuilder {
    uint32_t flags = 0, target = 0, format = 0, levels = 1, samples = 0;
    int32_t w = 1, h = 1, d = 1;
    uint64_t pool = 0, offset = 0;
    int64_t stride = 0;                  // nvnTextureBuilderSetStride (textures linéaires)
    uint8_t swizzle[4] = {2, 3, 4, 5};   // nvnTextureBuilderSetSwizzle : 0 zéro, 1 un, 2 R, 3 G, 4 B, 5 A
    uint8_t layout[8] = {};              // nvnTextureBuilderSetPackagedTextureLayout (texture « packagée »)
    bool packaged = false;
};
struct Tex { TexBuilder b; uint64_t addr = 0; float clear[4] = {0, 0, 0, 1}; };
struct WinBuilder { uint64_t native = 0; std::vector<uint64_t> tex; int32_t interval = 1; };
struct Win {
    std::vector<uint64_t> tex;
    int32_t interval = 1, next = 0, active = 0;
    int32_t crop[4] = {0, 0, 0, 0};   // nvnWindowSetCrop (x, y, l, h) ; l = 0 : texture entière
};

extern Table<PoolBuilder> g_poolb;
extern Table<Pool> g_pool;
extern Table<BufBuilder> g_bufb;
extern Table<Buf> g_buf;
extern Table<TexBuilder> g_texb;
extern Table<Tex> g_tex;
extern Table<WinBuilder> g_winb;
extern Table<Win> g_win;
extern std::atomic<uint64_t> g_frame;
extern bool g_origin_upper_left, g_depth_zero_one;   // nvnDeviceSetWindowOriginMode / SetDepthMode
uint64_t pool_ptr(uint64_t pool, uint64_t off);

}  // namespace nvn
}  // namespace hos
