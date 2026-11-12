// Service HLE de fichiers (fsp-srv) :
//  * romfs : image RomFS virtuelle (en-tête + tables construites en mémoire), dont les données
//    sont lues en place dans le conteneur .nup d'origine (ouvert en lecture seule, jamais copié) ;
//  * sauvegardes et carte SD : dossiers de l'hôte (à côté de l'exécutable).
#include "svc_util.h"
#include "settings.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace hos {
namespace {

constexpr uint32_t FS_PATH_NOT_FOUND = MakeResult(2, 1), FS_PATH_EXISTS = MakeResult(2, 2),
                   FS_TARGET_NOT_FOUND = MakeResult(2, 1002);

// ---------------------------------------------------------------- variables du moteur (PC)
// Réglages injectés dans veclipse_build.cvar. Rendu interne à la résolution de sortie (le moteur rend
// sinon à 50 % puis suréchantillonne, comme sur la console ; native_resolution = false pour cela).
std::vector<std::pair<std::string, std::string>> recomp_cvars() {
    std::vector<std::pair<std::string, std::string>> v;
    if (settings().native_resolution) {   // recomp.ini : native_resolution (false : comportement console)
        v.push_back({"render.resolution_scale_console", "1.0"});
        v.push_back({"render.resolution_scale", "1.0"});
        v.push_back({"render.dynres_enabled", "false"});
    }
    return v;
}

// ---------------------------------------------------------------- romfs virtuelle
struct Romfs {
    std::vector<uint8_t> meta;   // [0, data_off)
    uint64_t data_off = 0, total = 0;
    struct F {
        uint64_t off, size, nup_off;
        std::shared_ptr<std::vector<uint8_t>> host;   // contenu de remplacement (romfs_override/)
    };
    std::vector<F> files;
    HANDLE nup = INVALID_HANDLE_VALUE;

    static uint32_t hash(uint32_t parent, const std::string& name) {
        uint32_t h = parent ^ 123456789u;
        for (unsigned char c : name) {
            h = (h >> 5) | (h << 27);
            h ^= c;
        }
        return h;
    }

    void build() {
        nup = CreateFileA(g_nup_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (nup == INVALID_HANDLE_VALUE) HLOG("fs", "container not found: %s", g_nup_path.c_str());
        const uint32_t NONE = 0xFFFFFFFFu, NB = 3;
        std::vector<uint8_t> dirmeta, filemeta;
        auto put32 = [](std::vector<uint8_t>& v, uint32_t x) {
            v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 4);
        };
        auto put64 = [](std::vector<uint8_t>& v, uint64_t x) {
            v.insert(v.end(), (uint8_t*)&x, (uint8_t*)&x + 8);
        };
        std::vector<uint32_t> dirhash(NB, NONE), filehash(NB, NONE);
        // racine
        put32(dirmeta, 0);
        put32(dirmeta, NONE);
        put32(dirmeta, NONE);
        put32(dirmeta, g_romfs.empty() ? NONE : 0);
        put32(dirmeta, NONE);
        put32(dirmeta, 0);
        dirhash[hash(0, "") % NB] = 0;
        uint64_t data = 0;
        // Surcharges : <dossier de l'exe>/romfs_override/<fichier> remplace le fichier du jeu (le .nup
        // n'est jamais modifié). Sert aux variables du moteur (veclipse_build.cvar).
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string odir = exe;
        odir = odir.substr(0, odir.find_last_of("\/") + 1) + "romfs_override\\";
        for (size_t i = 0; i < g_romfs.size(); i++) {
            RomfsFile rf = g_romfs[i];
            std::shared_ptr<std::vector<uint8_t>> host;
            if (FILE* of = fopen((odir + rf.path).c_str(), "rb")) {
                host = std::make_shared<std::vector<uint8_t>>();
                uint8_t buf[65536];
                for (size_t k; (k = fread(buf, 1, sizeof buf, of)) > 0;) host->insert(host->end(), buf, buf + k);
                fclose(of);
                rf.size = host->size();
                HLOG("fs", "romfs: /%s replaced by %s (%zu bytes)", rf.path.c_str(), (odir + rf.path).c_str(), host->size());
            } else if (rf.path == "veclipse_build.cvar" && nup != INVALID_HANDLE_VALUE && rf.size < 65536 &&
                       !getenv("RECOMP_NO_CVAR_PATCH")) {
                // Cadence : sans « out.desired_frame_time », le régulateur du moteur vise ≈ 18 ms (55 img/s).
                // Une cible courte laisse l'écran virtuel (contre-pression à l'acquisition, nvn_gpu.cpp)
                // fixer le rythme au vblank, comme le vsync de la console.
                std::string txt(rf.size, ' ');
                OVERLAPPED ov{};
                ov.Offset = (DWORD)rf.nup_offset, ov.OffsetHigh = (DWORD)(rf.nup_offset >> 32);
                DWORD got = 0;
                if (ReadFile(nup, txt.data(), (DWORD)rf.size, &got, &ov) && got == rf.size) {
                    // entrées ajoutées : cadence, réglages PC (recomp_cvars), puis RECOMP_CVARS="nom=valeur;..."
                    std::vector<std::pair<std::string, std::string>> add;
                    if (txt.find("out.desired_frame_time") == std::string::npos) add.push_back({"out.desired_frame_time", "0.004"});
                    for (auto& kv : recomp_cvars()) add.push_back(kv);
                    if (const char* e = getenv("RECOMP_CVARS")) {
                        std::string all = e;
                        for (size_t p0 = 0; p0 < all.size();) {
                            size_t p1 = all.find(';', p0);
                            std::string item = all.substr(p0, p1 == std::string::npos ? std::string::npos : p1 - p0);
                            size_t eq = item.find('=');
                            if (eq != std::string::npos) add.push_back({item.substr(0, eq), item.substr(eq + 1)});
                            if (p1 == std::string::npos) break;
                            p0 = p1 + 1;
                        }
                    }
                    size_t close = txt.find_last_of('}');
                    if (close != std::string::npos && !add.empty()) {
                        size_t last = close ? txt.find_last_not_of(" \t\r\n", close - 1) : std::string::npos;
                        bool empty = last == std::string::npos || txt[last] == '{';
                        std::string ins;
                        for (size_t i = 0; i < add.size(); i++)
                            ins += "\t\"" + add[i].first + "\": " + add[i].second + (i + 1 < add.size() ? ",\r\n" : "\r\n");
                        txt.insert(close, ins);
                        if (!empty) txt.insert(last + 1, ",");
                        host = std::make_shared<std::vector<uint8_t>>(txt.begin(), txt.end());
                        rf.size = host->size();
                        for (auto& kv : add) HLOG("fs", "engine variable: %s = %s", kv.first.c_str(), kv.second.c_str());
                    }
                }
            }
            if (rf.path.find('/') != std::string::npos)
                HLOG("fs", "romfs: unsupported subfolders (%s)", rf.path.c_str());
            uint32_t off = (uint32_t)filemeta.size();
            uint32_t b = hash(0, rf.path) % NB;
            size_t nlen = rf.path.size(), padded = (nlen + 3) & ~size_t(3);
            uint32_t next = i + 1 < g_romfs.size() ? off + 0x20 + (uint32_t)padded : NONE;
            put32(filemeta, 0);       // parent
            put32(filemeta, next);    // frère
            put64(filemeta, data);
            put64(filemeta, rf.size);
            put32(filemeta, filehash[b]);
            put32(filemeta, (uint32_t)nlen);
            filehash[b] = off;
            filemeta.insert(filemeta.end(), rf.path.begin(), rf.path.end());
            filemeta.resize(filemeta.size() + padded - nlen, 0);
            files.push_back(F{data, rf.size, rf.nup_offset, host});
            HLOG("fs", "romfs: /%s (%.1f MB)", rf.path.c_str(), rf.size / 1048576.0);
            data = (data + rf.size + 15) & ~15ull;
        }
        uint64_t h[10];
        h[0] = 0x50;
        uint64_t p = 0x50;
        h[1] = p; h[2] = NB * 4; p += NB * 4;
        h[3] = p; h[4] = dirmeta.size(); p += dirmeta.size();
        h[5] = p; h[6] = NB * 4; p += NB * 4;
        h[7] = p; h[8] = filemeta.size(); p += filemeta.size();
        data_off = (p + 0x1FF) & ~0x1FFull;
        h[9] = data_off;
        meta.assign(data_off, 0);
        memcpy(meta.data(), h, 0x50);
        memcpy(meta.data() + h[1], dirhash.data(), NB * 4);
        memcpy(meta.data() + h[3], dirmeta.data(), dirmeta.size());
        memcpy(meta.data() + h[5], filehash.data(), NB * 4);
        memcpy(meta.data() + h[7], filemeta.data(), filemeta.size());
        total = data_off + data;
    }

    void read(uint64_t off, uint8_t* dst, uint64_t size) {
        while (size) {
            uint64_t n = size;
            if (off < data_off) {
                n = std::min(size, data_off - off);
                memcpy(dst, meta.data() + off, n);
            } else {
                uint64_t rel = off - data_off;
                const F* f = nullptr;
                for (auto& x : files)
                    if (rel >= x.off && rel < x.off + x.size) f = &x;
                if (!f) {
                    // bourrage entre fichiers / au-delà : zéros jusqu'au prochain fichier
                    uint64_t nxt = total;
                    for (auto& x : files)
                        if (x.off > rel) nxt = std::min(nxt, data_off + x.off);
                    n = std::min(size, nxt > off ? nxt - off : size);
                    memset(dst, 0, n);
                } else {
                    n = std::min(size, f->off + f->size - rel);
                    if (f->host) {
                        memcpy(dst, f->host->data() + (rel - f->off), n);
                        off += n, dst += n, size -= n;
                        continue;
                    }
                    uint64_t src = f->nup_off + (rel - f->off);
                    // ReadFile écrit directement dans la mémoire du jeu, sans faute que le runtime puisse rattraper :
                    // garde du noyau tenue de l'engagement de la plage à la fin de la lecture (aucune page vide n'est
                    // rendue à Windows pendant ce temps, memory.cpp)
                    MemNoyau garde;
                    mem_ensure((uint64_t)dst, n);
                    for (uint64_t done = 0; done < n;) {
                        DWORD chunk = (DWORD)std::min<uint64_t>(n - done, 1u << 30), got = 0;
                        OVERLAPPED ov{};
                        ov.Offset = (DWORD)(src + done);
                        ov.OffsetHigh = (DWORD)((src + done) >> 32);
                        if (!ReadFile(nup, dst + done, chunk, &got, &ov) || !got) {
                            memset(dst + done, 0, n - done);
                            break;
                        }
                        done += got;
                    }
                }
            }
            off += n;
            dst += n;
            size -= n;
        }
    }
};

Romfs& romfs() {
    static Romfs* r = [] {
        auto* x = new Romfs;
        x->build();
        return x;
    }();
    return *r;
}

std::shared_ptr<Service> make_romfs_storage() {
    return svc("fs:IStorage(romfs)", [](Request& r) {
        switch (r.cmd) {
        case 0: {   // Read(offset, size) → B
            uint64_t off = r.arg<uint64_t>(0), size = r.arg<uint64_t>(8);
            auto b = r.out_buf();
            romfs().read(off, (uint8_t*)b.addr, std::min(size, b.size));
            return true;
        }
        case 4: r.push<uint64_t>(romfs().total); return true;   // GetSize
        case 5: r.out.assign(0x40, 0); return true;             // OperateRange
        }
        return false;
    });
}

// ---------------------------------------------------------------- systèmes de fichiers hôtes
fs::path host_root(const char* sub) {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    fs::path p = fs::path(exe).parent_path() / sub;
    std::error_code ec;
    fs::create_directories(p, ec);
    return p;
}

// Dossier des sauvegardes du joueur.
// Jusqu'à la 1.0.2 : « save » à côté de l'exécutable (recomp\build\full\save), donc dans le dossier d'installation :
// perdu quand le joueur supprimait ce dossier pour réinstaller. Depuis la 1.0.3 : %APPDATA%\SpaceAnimalsRecomp\save,
// commun à toutes les installations. Au premier lancement, une sauvegarde trouvée à l'ancien endroit y est copiée ;
// l'ancienne reste en place. RECOMP_SAVE_DIR impose un autre dossier (essais : la sauvegarde du joueur reste intacte ;
// le 2026-11-09, un essai avait lancé une campagne dans celle du joueur).
size_t compter_fichiers(const fs::path& p) {
    std::error_code ec;
    size_t n = 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec)) n++;
    return n;
}

const fs::path& save_root() {
    static const fs::path racine = [] {
        std::error_code ec;
        char exe[MAX_PATH];
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        const fs::path ancien = fs::path(exe).parent_path() / "save";
        if (const char* alt = getenv("RECOMP_SAVE_DIR"); alt && *alt) {
            fs::path p = fs::absolute(fs::path(alt), ec);
            fs::create_directories(p, ec);
            HLOG("fs", "saves: %s (RECOMP_SAVE_DIR, %zu file(s))", p.u8string().c_str(), compter_fichiers(p));
            return p;
        }
        const wchar_t* appdata = _wgetenv(L"APPDATA");
        if (!appdata || !*appdata) {   // pas de profil : ancien emplacement
            fs::create_directories(ancien, ec);
            HLOG("fs", "saves: %s (APPDATA missing: next to the game, %zu file(s))", ancien.u8string().c_str(),
                 compter_fichiers(ancien));
            return ancien;
        }
        const fs::path nouveau = fs::path(appdata) / "SpaceAnimalsRecomp" / "save";
        const size_t n_ancien = fs::is_directory(ancien, ec) ? compter_fichiers(ancien) : 0;
        size_t n_nouveau = fs::is_directory(nouveau, ec) ? compter_fichiers(nouveau) : 0;
        fs::create_directories(nouveau, ec);
        if (ec) {   // dossier du profil inaccessible : on reste à l'ancien endroit plutôt que de perdre la sauvegarde
            HLOG("fs", "saves: %s inaccessible (%s): next to the game, %s", nouveau.u8string().c_str(),
                 ec.message().c_str(), ancien.u8string().c_str());
            fs::create_directories(ancien, ec);
            return ancien;
        }
        if (!n_nouveau && n_ancien) {   // première fois : la sauvegarde de l'ancien emplacement est reprise
            fs::copy(ancien, nouveau, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
            n_nouveau = compter_fichiers(nouveau);
            if (ec || n_nouveau < n_ancien) {
                HLOG("fs", "saves: cannot move %s over (%s): the game keeps the old folder",
                     ancien.u8string().c_str(), ec.message().c_str());
                return ancien;
            }
            HLOG("fs", "saves: %zu file(s) copied from %s (old location, left in place)", n_ancien,
                 ancien.u8string().c_str());
            if (FILE* f = _wfopen((ancien / L"SAVE MOVED - SAUVEGARDE DEPLACEE.txt").c_str(), L"wb")) {
                const std::string t = "Since version 1.0.3, saves are kept in:\r\n  " + nouveau.u8string() +
                                      "\r\nThis folder is the copy from before the move; the game no longer uses it.\r\n\r\n"
                                      "Depuis la version 1.0.3, les sauvegardes sont dans :\r\n  " + nouveau.u8string() +
                                      "\r\nCe dossier est la copie d'avant le deplacement ; le jeu ne s'en sert plus.\r\n";
                fwrite(t.data(), 1, t.size(), f);
                fclose(f);
            }
        } else if (n_nouveau && n_ancien) {
            HLOG("fs", "saves: old folder %s ignored (%zu file(s)): the game uses %s", ancien.u8string().c_str(),
                 n_ancien, nouveau.u8string().c_str());
        }
        HLOG("fs", "saves: %s (%zu file(s))", nouveau.u8string().c_str(), n_nouveau);
        return nouveau;
    }();
    return racine;
}

std::string guest_path(const Request& r) {
    char buf[0x302] = {};
    if (!r.x.empty() && r.x[0].addr) memcpy(buf, (void*)r.x[0].addr, std::min<uint64_t>(r.x[0].size, 0x301));
    else if (!r.a.empty() && r.a[0].addr) memcpy(buf, (void*)r.a[0].addr, std::min<uint64_t>(r.a[0].size, 0x301));
    return buf;
}

bool resolve(const fs::path& root, const std::string& gp, fs::path& out) {
    std::string s = gp;
    while (!s.empty() && s[0] == '/') s.erase(0, 1);
    fs::path rel = fs::u8path(s).lexically_normal();
    for (auto& part : rel)
        if (part == "..") return false;
    out = root / rel;
    return true;
}

std::shared_ptr<Service> make_file(fs::path p, uint32_t mode) {
    auto f = std::make_shared<std::fstream>(p, std::ios::binary | std::ios::in |
                                                   ((mode & 2) ? std::ios::out : std::ios::openmode{}));
    return svc("fs:IFile", [f, p](Request& r) {
        switch (r.cmd) {
        case 0: {   // Read(option, offset, size) → B, u64 lus
            uint64_t off = r.arg<uint64_t>(8), size = r.arg<uint64_t>(16);
            auto b = r.out_buf();
            f->clear();
            f->seekg((std::streamoff)off);
            f->read((char*)b.addr, (std::streamsize)std::min(size, b.size));
            r.push<uint64_t>((uint64_t)f->gcount());
            return true;
        }
        case 1: {   // Write(option, offset, size) + A
            uint64_t off = r.arg<uint64_t>(8), size = r.arg<uint64_t>(16);
            auto b = r.in_buf();
            f->clear();
            f->seekp((std::streamoff)off);
            f->write((const char*)b.addr, (std::streamsize)std::min(size, b.size));
            return true;
        }
        case 2: f->flush(); return true;
        case 3: {   // SetSize
            f->flush();
            std::error_code ec;
            fs::resize_file(p, r.arg<uint64_t>(0), ec);
            return true;
        }
        case 4: {   // GetSize
            std::error_code ec;
            r.push<uint64_t>((uint64_t)fs::file_size(p, ec));
            return true;
        }
        case 5: r.out.assign(0x40, 0); return true;
        }
        return false;
    });
}

struct DirEntry {
    char name[0x301];
    uint8_t attr, pad[2];
    int8_t type;
    uint8_t pad2[3];
    int64_t size;
};
static_assert(sizeof(DirEntry) == 0x310, "DirEntry");

std::shared_ptr<Service> make_dir(fs::path p, uint32_t filter) {
    auto entries = std::make_shared<std::vector<DirEntry>>();
    std::error_code ec;
    for (auto& e : fs::directory_iterator(p, ec)) {
        bool d = e.is_directory();
        if (d && !(filter & 1)) continue;
        if (!d && !(filter & 2)) continue;
        DirEntry de{};
        std::string n = e.path().filename().u8string();
        strncpy(de.name, n.c_str(), 0x300);
        de.type = d ? 0 : 1;
        de.size = d ? 0 : (int64_t)e.file_size(ec);
        entries->push_back(de);
    }
    auto pos = std::make_shared<size_t>(0);
    return svc("fs:IDirectory", [entries, pos](Request& r) {
        if (r.cmd == 0) {   // Read → nombre + B
            auto b = r.out_buf();
            size_t n = std::min<size_t>(entries->size() - *pos, b.size / sizeof(DirEntry));
            memcpy((void*)b.addr, entries->data() + *pos, n * sizeof(DirEntry));
            *pos += n;
            r.push<int64_t>((int64_t)n);
            return true;
        }
        if (r.cmd == 1) { r.push<int64_t>((int64_t)entries->size()); return true; }
        return false;
    });
}

std::shared_ptr<Service> make_host_fs(const char* n, fs::path root) {
    HLOG("fs", "%s → %s", n, root.u8string().c_str());
    const bool sauvegarde = !strcmp(n, "fs:save");
    return svc(n, [root, sauvegarde](Request& r) {
        fs::path p;
        std::error_code ec;
        std::string gp = guest_path(r);
        bool ok = resolve(root, gp, p);
        if (!ok && r.cmd <= 14) { r.result = FS_PATH_NOT_FOUND; return true; }
        switch (r.cmd) {
        case 0: {   // CreateFile(option, size, path)
            if (fs::exists(p, ec)) { r.result = FS_PATH_EXISTS; return true; }
            std::ofstream(p, std::ios::binary).close();
            fs::resize_file(p, r.arg<uint64_t>(8), ec);
            return true;
        }
        case 1: fs::remove(p, ec); return true;
        case 2: fs::create_directories(p, ec); return true;
        case 3: fs::remove(p, ec); return true;
        case 4: case 13: fs::remove_all(p, ec); if (r.cmd == 13) fs::create_directories(p, ec); return true;
        case 7: {   // GetEntryType
            if (!fs::exists(p, ec)) { r.result = FS_PATH_NOT_FOUND; return true; }
            r.push<uint32_t>(fs::is_directory(p, ec) ? 0 : 1);
            return true;
        }
        case 8: {   // OpenFile(mode, path)
            if (!fs::is_regular_file(p, ec)) { r.result = FS_PATH_NOT_FOUND; return true; }
            if (sauvegarde) {   // journal : le jeu a bien retrouvé (ou réécrit) sa sauvegarde
                static std::atomic<int> nlog{0};
                if (nlog++ < 12)
                    HLOG("fs", "save opened for %s: %s (%llu bytes)", (r.arg<uint32_t>(0) & 2) ? "writing" : "reading",
                         gp.c_str(), (unsigned long long)fs::file_size(p, ec));
            }
            r.out_objects.push_back(make_file(p, r.arg<uint32_t>(0)));
            return true;
        }
        case 9: {   // OpenDirectory(filter, path)
            if (!fs::is_directory(p, ec)) { r.result = FS_PATH_NOT_FOUND; return true; }
            r.out_objects.push_back(make_dir(p, r.arg<uint32_t>(0)));
            return true;
        }
        case 10: return true;   // Commit
        case 11: r.push<int64_t>(4ll << 30); return true;
        case 12: r.push<int64_t>(8ll << 30); return true;
        }
        HLOG("fs", "%s: command %u on '%s'", "fs:IFileSystem", r.cmd, gp.c_str());
        return false;
    });
}

std::shared_ptr<Service> make_fsp() {
    return svc("fsp-srv", [](Request& r) {
        switch (r.cmd) {
        case 1: return true;                                         // SetCurrentProcess
        case 18: r.out_objects.push_back(make_host_fs("fs:sdcard", host_root("sdcard"))); return true;
        case 22: return true;                                        // CreateSaveDataFileSystem
        case 51: case 52: case 53: {                                 // OpenSaveDataFileSystem…
            r.out_objects.push_back(make_host_fs("fs:save", save_root()));   // %APPDATA%\SpaceAnimalsRecomp\save
            return true;
        }
        case 61:                                                     // OpenSaveDataInfoReader…
            r.out_objects.push_back(svc("fs:ISaveDataInfoReader", [](Request& q) {
                if (q.cmd == 0) { q.push<int64_t>(0); return true; }
                return false;
            }));
            return true;
        case 200: case 203: case 205:                                // OpenDataStorageByCurrentProcess…
            HLOG("fs", "opening the romfs (command %u)", r.cmd);
            r.out_objects.push_back(make_romfs_storage());
            return true;
        case 202: r.result = FS_TARGET_NOT_FOUND; return true;      // OpenDataStorageByDataId
        case 1003: case 1004: case 1006: case 1016: case 1017: case 1018: return true;
        case 1005: r.push<uint32_t>(0); return true;                // GetGlobalAccessLogMode
        case 1011: r.push<uint32_t>(0); r.push<uint32_t>(1); return true;   // GetProgramIndexForAccessLog
        }
        return false;
    });
}

struct Registrar {
    Registrar() { register_service("fsp-srv", make_fsp); }
} g_reg;

}  // namespace
// Dossier des sauvegardes (reprise de l'ancien emplacement faite au besoin) : option --save-dir de l'exécutable.
std::string save_dir_path() { return save_root().u8string(); }

}  // namespace hos
