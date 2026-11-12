// Couche hôte « Horizon » de l'exécutable natif : espace d'adressage invité, chargeur de
// modules, noyau émulé (svc), IPC et journal. Le code recompilé n'inclut jamais ce fichier
// (il ne voit que cpu.h) : on peut le modifier sans recompiler les modules.
#pragma once
#include "cpu.h"

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// runtime.cpp (hors de cpu.h pour ne pas invalider les objets générés)
extern void (*rt_svc_handler)(CPUState* ctx, uint32_t imm, uint64_t pc);
extern void (*rt_fatal_hook)(const char* msg, uint64_t pc);
extern void (*rt_call_hook)(CPUState* ctx, uint64_t target);
void rt_dispatch_finalize();
const std::vector<std::pair<uint64_t, RecompFn>>& rt_dispatch_table();

namespace hos {

// ------------------------------------------------------------------------------ journal
extern int g_verbose;
extern bool g_trace_nvn;
extern std::atomic<bool> g_svc_timing;   // diagnostic : durée des svc (kernel.cpp)   // --trace-nvn : chaque appel NVN (natif ou bouchon), avec l'appelant   // 0 : erreurs et faits marquants, 1 : infos, 2 : chaque svc / IPC
void logf(const char* tag, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
[[noreturn]] void panic(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define HLOG(tag, ...) ::hos::logf(tag, __VA_ARGS__)
#define HINFO(tag, ...) do { if (::hos::g_verbose >= 1) ::hos::logf(tag, __VA_ARGS__); } while (0)
#define HDBG(tag, ...) do { if (::hos::g_verbose >= 2) ::hos::logf(tag, __VA_ARGS__); } while (0)

// ------------------------------------------------------------------------------ résultats
constexpr uint32_t MakeResult(uint32_t mod, uint32_t desc) { return mod | (desc << 9); }
namespace kr {  // résultats du noyau (module 1)
constexpr uint32_t OutOfSessions = MakeResult(1, 7);
constexpr uint32_t NotImplemented = MakeResult(1, 33);
constexpr uint32_t InvalidSize = MakeResult(1, 101);
constexpr uint32_t InvalidAddress = MakeResult(1, 102);
constexpr uint32_t OutOfResource = MakeResult(1, 103);
constexpr uint32_t OutOfMemory = MakeResult(1, 104);
constexpr uint32_t OutOfHandles = MakeResult(1, 105);
constexpr uint32_t InvalidCurrentMemory = MakeResult(1, 106);
constexpr uint32_t InvalidNewMemoryPermission = MakeResult(1, 108);
constexpr uint32_t InvalidMemoryRegion = MakeResult(1, 110);
constexpr uint32_t InvalidPriority = MakeResult(1, 112);
constexpr uint32_t InvalidCoreId = MakeResult(1, 113);
constexpr uint32_t InvalidHandle = MakeResult(1, 114);
constexpr uint32_t InvalidPointer = MakeResult(1, 115);
constexpr uint32_t InvalidCombination = MakeResult(1, 116);
constexpr uint32_t TimedOut = MakeResult(1, 117);
constexpr uint32_t Cancelled = MakeResult(1, 118);
constexpr uint32_t OutOfRange = MakeResult(1, 119);
constexpr uint32_t InvalidEnumValue = MakeResult(1, 120);
constexpr uint32_t NotFound = MakeResult(1, 121);
constexpr uint32_t Busy = MakeResult(1, 122);
constexpr uint32_t SessionClosed = MakeResult(1, 123);
constexpr uint32_t InvalidState = MakeResult(1, 125);
}  // namespace kr

// ------------------------------------------------------------------------------ mémoire
// Disposition de l'espace d'adressage invité (identité invité == hôte). Tout est réservé au
// démarrage du processus, puis engagé à la demande.
constexpr uint64_t AS_BASE = 0x7000000000ull, AS_END = 0x8000000000ull;
constexpr uint64_t CODE_BASE = 0x7100000000ull, CODE_END = 0x7180000000ull;
constexpr uint64_t HEAP_BASE = 0x7200000000ull, HEAP_SIZE = 0x400000000ull;    // 16 Go
constexpr uint64_t ALIAS_BASE = 0x7800000000ull, ALIAS_SIZE = 0x400000000ull;  // 16 Go
constexpr uint64_t EXTRA_BASE = 0x7C00000000ull, EXTRA_SIZE = 0x200000000ull;  // GetInfo 29/30
constexpr uint64_t STACK_BASE = 0x7E00000000ull, STACK_SIZE = 0x80000000ull;   // 2 Go
constexpr uint64_t KMISC_BASE = 0x7F00000000ull, KMISC_SIZE = 0x7F000000ull;   // TLR, partagée…
constexpr uint64_t NATIVE_BASE = 0x7F7F000000ull;  // adresses fictives des fonctions natives
constexpr uint64_t USER_EXC_CTX = 0x7F70000000ull;  // GetInfo 20 (dans KMISC, hors des TLR)
constexpr uint64_t RTLD_DATA = 0x7F71000000ull;     // variables que rtld partage avec nn::ro

enum : uint32_t {  // svc::MemoryState
    MS_FREE = 0x00, MS_IO = 0x01, MS_STATIC = 0x02, MS_CODE = 0x03, MS_CODEDATA = 0x04,
    MS_NORMAL = 0x05, MS_SHARED = 0x06, MS_ALIAS = 0x07, MS_ALIASCODE = 0x08,
    MS_ALIASCODEDATA = 0x09, MS_IPC = 0x0A, MS_STACK = 0x0B, MS_THREADLOCAL = 0x0C,
    MS_TRANSFERED = 0x0D, MS_SHAREDTRANSFERED = 0x0E, MS_SHAREDCODE = 0x0F,
    MS_INACCESSIBLE = 0x10,
};
enum : uint32_t { MP_NONE = 0, MP_R = 1, MP_W = 2, MP_RW = 3, MP_X = 4, MP_RX = 5 };
enum : uint32_t { MA_LOCKED = 1, MA_IPCLOCKED = 2, MA_DEVICESHARED = 4, MA_UNCACHED = 8 };

struct MemInfo {  // svc::MemoryInfo (0x28 octets)
    uint64_t addr, size;
    uint32_t state, attr, perm, ipc_ref, dev_ref, pad;
};
static_assert(sizeof(MemInfo) == 0x28, "MemInfo");

void mem_init();
bool mem_commit(uint64_t addr, uint64_t size);          // engage (RW, mis à zéro)
// Engagement à la demande (memory.cpp) : la plage reste réservée, ses pages sont engagées au premier accès.
bool mem_commit_lazy(uint64_t addr, uint64_t size);
// gestionnaire d'exceptions : 0 pas une page à engager, 1 bloc engagé ; données de texture rendues : 2 restaurées pour
// une lecture, 3 restaurées pour une écriture (texture encore utilisée), 4 oubliées (le jeu réemploie la mémoire)
int mem_fault(uint64_t addr, bool ecriture);
// Données de textures déjà envoyées au GPU, rendues à Windows (memory.cpp ; recomp.ini : texture_release)
void mem_release_init();
bool mem_release_actif();                                           // le fichier temporaire est ouvert et accepte des données
uint64_t mem_release(uint64_t addr, uint64_t size, uint64_t* id);   // pages entières de la plage ; octets rendus (tout ou rien)
bool mem_released_intact(uint64_t addr, uint64_t size, uint64_t id);   // ce rendu n'a jamais été retouché
void mem_restore(uint64_t addr, uint64_t size);                     // restaure la partie rendue (avant une lecture en nombre)
// l'image qui a fait le rendu `id` a encore, ou n'a plus, d'objet texture du jeu (voir mem_fault)
void mem_release_vivant(uint64_t addr, uint64_t size, uint64_t id, bool vivant);
struct RelStats {
    uint64_t rendus, cumul, fichier;   // octets rendus en ce moment, depuis le début ; taille du fichier temporaire
    // restaurations : 0 lecture fautive, 1 écriture fautive dans une texture encore utilisée, 2 runtime, 3 avant un
    // appel système, 4 écriture fautive dans une texture sans objet (RECOMP_TEX_DROP : plages oubliées) ; 5 parmi
    // 0, 2 et 3, données relues après une réécriture partielle de la même plage sans objet
    uint64_t n[6], octets[6];
    uint64_t ns_ecrit, ns_lu;
};
void mem_release_stats(RelStats* s);
bool mem_ensure(uint64_t addr, uint64_t size);          // engage d'avance (noyau de Windows, import Vulkan)
void mem_lazy_stats(uint64_t* blocs, uint64_t* octets, uint64_t* reserves);
void mem_marge_init();                                  // marge engagée d'avance, rendue au premier refus de Windows
int mem_pression();                                     // nombre de refus de mémoire : le rendu vide ses caches quand il change
void mem_veille_init();                                 // veille de la mémoire du PC : réagit avant le premier refus
void mem_census_init();                                 // RECOMP_MEM_CENSUS=s : recensement de la mémoire du jeu (diagnostic)
void mem_vides_init();                                  // recomp.ini, zero_page_release : pages vides rendues à Windows
// Garde du noyau : tant qu'elle est tenue, aucune page vide de la mémoire du jeu n'est rendue à Windows (memory.cpp).
// À tenir de l'engagement d'une plage (mem_ensure) jusqu'à la fin de l'appel qui y écrit sans passer par le processeur
// du jeu : ReadFile dans la mémoire du jeu (svc_fs.cpp), import d'une fenêtre dans Vulkan (vk_render.cpp).
void mem_noyau_debut();
void mem_noyau_fin();
struct MemNoyau {
    MemNoyau() { mem_noyau_debut(); }
    ~MemNoyau() { mem_noyau_fin(); }
    MemNoyau(const MemNoyau&) = delete;
    MemNoyau& operator=(const MemNoyau&) = delete;
};
// fenêtre importée dans Vulkan (le GPU y lit et écrit) : jamais rendue, de l'import jusqu'à la libération de la mémoire
void mem_epingler(uint64_t addr, uint64_t size);
void mem_desepingler(uint64_t addr, uint64_t size);
void dump_thread_stacks(const char* why);   // host_main.cpp : piles des fils passés par le tas (diagnostic)
void cpp_heap_stats(int64_t* vivants, uint64_t nombre[40], uint64_t octets[40]);   // host_main.cpp, RECOMP_MEM_MAP
void mem_decommit(uint64_t addr, uint64_t size);
void mem_set(uint64_t addr, uint64_t size, uint32_t state, uint32_t perm, uint32_t attr = 0);
void mem_set_perm(uint64_t addr, uint64_t size, uint32_t perm);
void mem_set_attr(uint64_t addr, uint64_t size, uint32_t mask, uint32_t attr);
MemInfo mem_query(uint64_t addr);
bool mem_check(uint64_t addr, uint64_t size, uint32_t state);   // toute la plage dans cet état
uint64_t mem_find_free(uint64_t base, uint64_t end, uint64_t size, uint64_t align);
bool mem_valid(uint64_t addr, uint64_t size);           // plage engagée et lisible
uint64_t mem_used();                                     // octets engagés (Code/Data/Heap…)

// ------------------------------------------------------------------------------ modules
struct Module {
    std::string name;
    uint64_t base = 0, size = 0;
    uint64_t text_size = 0, ro_off = 0, data_off = 0, bss_off = 0, bss_end = 0;
    uint64_t dynamic = 0, init = 0, fini = 0;
    std::vector<std::pair<std::string, uint64_t>> exports;   // nom → adresse absolue
    std::vector<std::string> imports;                         // symboles non définis
};
extern std::vector<Module> g_modules;   // ordre d'adresse : main, subsdk*, sdk
struct RomfsFile {   // fichier de la romfs, stocké brut dans le .nup (lu en place)
    std::string path;
    uint64_t nup_offset = 0, size = 0;
};
extern std::string g_nup_path;
// Windows a refusé de la mémoire au jeu (tas, mémoire physique) : le PC manque de mémoire libre (autres programmes
// ouverts). Le jeu s'arrête alors avec le code 4, que le launcher explique au joueur.
extern std::atomic<bool> g_manque_memoire;
double memoire_validable_libre_go();
extern std::vector<RomfsFile> g_romfs;
void load_modules(const std::string& manifest);
const Module* module_at(uint64_t addr);
// symbolisation : « sdk!nn::os::Foo+0x14 » (noms bruts mangled) ou « main+0x123 »
std::string symbolize(uint64_t addr);
uint64_t find_export(const std::string& name);
std::string demangle(const char* name);

// ------------------------------------------------------------------------------ noyau
struct KThread;
struct KObject {   // objet noyau désigné par un handle
    virtual ~KObject() = default;
    virtual const char* kind() const = 0;
    virtual bool is_signaled() const { return false; }
    std::vector<KThread*> sync_waiters;   // threads dans WaitSynchronization sur cet objet
    std::string tag;                      // créateur (diagnostic)
};
uint32_t kernel_new_handle(std::shared_ptr<KObject> o);
KObject* kernel_get(uint32_t h);
void kernel_close(uint32_t h);
uint32_t kernel_create_event(std::shared_ptr<KObject>* out, bool signaled, const char* tag = "");   // handle lisible
KThread* current_thread();
uint64_t current_thread_id();
void kernel_dump_threads();
std::vector<std::pair<uint64_t, void*>> kernel_thread_list();   // (id, handle hôte) des threads vivants   // diagnostic : état de tous les threads invités
std::string kernel_describe(uint32_t h);
CPUState* current_ctx();
void kernel_init();
// crée le thread principal de l'invité et l'exécute (bloquant jusqu'à ExitProcess / fin)
int run_main_thread(uint64_t entry, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3,
                    uint64_t arg4, uint64_t stack_size);
void svc_dispatch(CPUState* ctx, uint32_t imm, uint64_t pc);

// Fonctions natives appelables par l'invité (adresse fictive dans le dispatch)
uint64_t register_native(const char* name, RecompFn fn);
void rename_native(uint64_t addr, const std::string& name);
// remplace une fonction exportée (appels via PLT / pointeurs) par une fonction native
bool override_export(const char* sym, RecompFn fn);
void gfx_init();      // hle_gfx.cpp : remplacements graphiques (avant rt_dispatch_finalize)
std::string save_dir_path();   // svc_fs.cpp : dossier des sauvegardes du joueur (%APPDATA%\SpaceAnimalsRecomp\save)
void hle_os_init();   // hle_os.cpp : fibres nn::os
void hle_audio_init();
void hle_hid_init();     // svc_hid.cpp : vibrations (nn::hid::SendVibrationValue)
// crée un nn::os::SystemEventType invité (vrai événement noyau) et renvoie son objet noyau
std::shared_ptr<KObject> hle_make_system_event(CPUState* ctx, uint64_t system_event);   // hle_audio.cpp : flux audio nn::audio
// neutralise toutes les fonctions qu'importe main dans un espace de noms (préfixe mangled) :
// bouchons natifs nommés qui renvoient 0 et journalisent leur premier appel
int stub_namespace(const char* mangled_prefix, const char* why);
void kernel_set_running_ctx(CPUState* ctx);   // fibre en cours d'exécution sur ce thread
// appelle une fonction invitée depuis l'hôte (x0..x7 déjà placés dans ctx)
void call_guest(CPUState* ctx, uint64_t addr);

// ------------------------------------------------------------------------------ IPC
struct Session;
// ouvre une session client vers un service nommé (renvoie un handle, 0 si refus)
uint32_t ipc_connect_named_port(const std::string& name);
uint32_t ipc_send_sync_request(CPUState* ctx, uint32_t handle, uint64_t buf, uint64_t size);
// création d'un handle de session vers le service `name` (utilisé par sm:)
uint32_t ipc_open_service(const std::string& name);

}  // namespace hos
