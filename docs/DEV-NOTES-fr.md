# recomp/ — recompilateur statique ARM64 → C++ (Phases 1 à 4)

Recompilateur pour le portage natif Windows x86-64 du titre Switch 2 décrit dans
`../NOTES.md` : cœur ARM64 → C++ avec son banc de validation différentielle contre Unicorn
(Phase 1), puis exécutable natif qui charge tous les modules recompilés et émule le noyau
Horizon et les services système (Phase 2), enfin démarrage graphique avec NVN en HLE, fenêtre
hôte et boucle de rendu qui présente des images (Phase 3), puis exécution du rendu NVN sur
Vulkan avec traduction des shaders (Phase 4, en cours). Voir les sections dédiées plus bas.

> **Privé.** `out/`, `build/` et `bench/results/` sont dérivés du jeu (code généré, objets,
> exécutable, traces, états mémoire) et ne doivent jamais être diffusés. Seuls les sources
> Python et le runtime (`a64/`, `runtime/`, `bench/*.py`, `*.py`) sont génériques.

## Arborescence

| Chemin | Rôle |
|---|---|
| `recomp.py` | CLI : `gen <module>` (génération C++), `stats <module>` (couverture du décodeur) |
| `build.py` | compilation parallèle g++ avec cache par fichier (`.dll`, `.exe` ou objets) |
| `a64/module.py` | chargement d'une image `work/*.bin` : MOD0, `.dynamic`, symboles, relocations (RELA, JMPREL, **RELR**), fonctions via `.eh_frame_hdr` |
| `a64/translate.py` | décodeur + traduction A64 entier / branchements / système / mémoire / atomiques |
| `a64/simd.py` | FP scalaire et AdvSIMD (NEON), chargements/rangements de structures |
| `a64/jumptable.py` | récupération des tables de sauts LLVM (`switch` local) |
| `a64/codegen.py` | découverte des fonctions, émission des corps, table de dispatch, mode flux |
| `runtime/cpu.h` | `CPUState`, mémoire invitée, drapeaux, atomiques LSE, macros TRACE / CHECK_RET / MUSTTAIL |
| `runtime/simd.h`, `runtime/fp.cpp` | sémantique FP ARM exacte (NaN, min/max, FMA, conversions, FRECPE/FRSQRTE…) |
| `runtime/runtime.cpp` | dispatch des sauts indirects, svc, erreurs, sysregs ; en mode banc : trace et API C |
| `bench/guest.py` | mémoire invitée à adresse fixe dans le processus Python, état partagé, Unicorn |
| `bench/harness.py` | banc : `func` (fonctions entières, trace pas à pas) et `fuzz` (instruction par instruction) |
| `bench/run_all.py` | suite de validation complète → `bench/results/validation.log` |
| `fullbuild.py` | **Phase 2** : build complet d'un module (découverte parallèle, lots équilibrés, génération + compilation, sources supprimées après compilation) → `build/full/<module>/`, + `build/full/manifest.txt` |
| `link.py` | **Phase 2** : édition de liens de `build/full/recomp_main.exe` (modules + runtime + couche hôte) |
| `runtime/os/hos.h` | couche hôte « Horizon » : disposition mémoire invitée, modules, noyau, IPC (jamais inclus par le code généré) |
| `runtime/os/memory.cpp` | réservation de l'espace invité à adresse fixe, carte des régions (état/perm/attributs) |
| `runtime/os/loader.cpp` | chargeur natif (remplace rtld) : mapping, relocations RELA/JMPREL/RELR/TLSDESC, portée globale, symbolisation |
| `runtime/os/kernel.cpp` | noyau émulé : handles, threads hôtes, mutex/condvars/arbitre d'adresses, événements, mémoire, GetInfo |
| `runtime/os/ipc.cpp`, `ipc.h` | IPC HIPC/CMIF (domaines, contrôle) et TIPC, `sm:`, services bouchons journalisés |
| `runtime/os/svc_*.cpp` | services HLE : applets (`appletOE`), `lm`, heure (`time:*`), fichiers (`fsp-srv`, romfs virtuelle), contrôle parental, `aoc:u`, comptes/réseau, **entrées (`hid`)**, **sockets (`bsd`)** |
| `runtime/os/host_main.cpp` | `main()` : journal, gestionnaire de plantage (pile d'appels invitée), démarrage façon rtld, options de diagnostic |
| `runtime/os/hle_gfx.cpp` | **Phase 3** : `nn::gs` court-circuité, `nn::vi` fictif, **NVN en HLE** (fonctions natives + bouchons nommés), caméra absente |
| `runtime/os/present.cpp`, `gfx.h` | **Phase 3** : fenêtre hôte Win32 (GDI) cadencée à 60 Hz, compteur d'images |
| `runtime/os/input.cpp` | **Phase 3** : clavier + manette XInput de l'hôte pour la manette émulée |
| `runtime/os/hle_os.cpp` | **Phase 3** : fibres `nn::os` et coroutines du moteur → fibres Windows ; neutralisation d'espaces de noms (`nn::chat`) |
| `runtime/os/hle_audio.cpp` | **Phase 3** : flux de sortie `nn::audio` (tampons, mixeur, gain), décodeur Opus matériel (silence) |
| `runtime/os/audio_out.cpp`, `audio_out.h` | sortie audio Windows (WASAPI) : l'horloge de la carte son vide les flux du jeu |
| `hooks.txt` | fonctions invitées dont les appels **directs** passent par le dispatch (pour être remplacées par des natives) |

## Utilisation

```bash
python recomp.py gen rtld                      # → out/rtld/*.cpp (module complet)
python recomp.py gen main --funcs 3000 --out out/main_lot1   # premier lot de main
python recomp.py gen main --stream --no-comments               # main complet, mémoire bornée (~4 Go de C++)
python build.py out/rtld build/rtld.dll -O 2   # compile (runtime inclus)
python bench/harness.py func rtld --trials 16  # différentiel fonctions entières
python bench/harness.py fuzz main --sample 20000 --fp-only    # fuzzing FP/NEON de main
python bench/run_all.py                        # tout (≈ 15 min)
```

## Modèle d'exécution

* **Une fonction invitée = une fonction C++** `void f_<adresse>(CPUState* ctx)`. Les fonctions
  viennent de `.eh_frame_hdr` (248 842 pour main), complétées par les cibles de `bl`/`b`, les
  symboles exportés, `DT_INIT/FINI` et tous les pointeurs relogés vers `.text` (vtables…). Une
  entrée au milieu d'une fonction donne une copie du suffixe (190 dans les 23 000 premières
  fonctions de main) ; une fonction sans FDE (stubs PLT) est délimitée par atteignabilité.
* **Branchements** : internes → `goto L_<adr>` ; `b`/`b.cond` vers une autre fonction → appel
  terminal **garanti** (`[[gnu::musttail]]`, gcc ≥ 15 / clang) ; `bl` → `x30 = retour` puis appel
  direct ; `ret` → `return` ; `blr` → `rt_call` (recherche dichotomique dans la table de
  dispatch) ; `br` → `switch` local si une table de sauts est reconnue, sinon `rt_tailcall`.
* **État** : `ctx->x[0..30]`, `ctx->sp`, drapeaux séparés `nf/zf/cf/vf` (0/1), `ctx->v[32]`
  (V128), `fpcr/fpsr`, `tpidr_el0/tpidrro_el0`, moniteur exclusif. Disposition figée
  (`static_assert`) et reproduite par le banc.
* **Mémoire** : identité adresse invitée == adresse hôte (option `RECOMP_MEM_OFFSET`). Les
  modules sont chargés à des bases fixes (`a64/module.py`, `DEFAULT_BASES` : 0x71xxxxxxxx, libre
  dans un processus Windows), le code émis contient des adresses absolues. Accès non alignés
  via `memcpy` (gratuit sur x86).
* **LSE → atomiques natives** : `cas*/casp*`, `ldadd/ldclr/ldeor/ldset/ldsmax/ldsmin/ldumax/
  ldumin/swp` (toutes variantes A/L, 8–64 bits) → `__atomic_*` seq_cst (= `lock` x86) ;
  `ldar/stlr/ldapr/ldapur` → load/store atomiques ; `ldxr/stxr/ldxp/stxp` émulés par CAS.
* **PAC → nop/ret** : `pacibsp/autibsp/xpaclri…` (espace HINT) et `PAC*/AUT*/XPAC*` registre sont
  des nop ; `retab/retaa` = `ret`, `braa/blraa…` = `br/blr`. (Unicorn a PAC désactivé dans
  SCTLR_EL1, donc même sémantique.)
* **FP** : sémantique ARM reproduite (voir `runtime/simd.h`) : NaN (priorité SNaN, 1er opérande,
  NaN par défaut positif, FPCR.DN), FMIN/FMAX/NM avec zéros signés, conversions saturantes,
  `fmadd/fmla` fusionnés, `-ffp-contract=off` pour ne jamais fusionner `fmul`+`fadd`,
  FRECPE/FRSQRTE par les tables du pseudo-code ARM, FPCR.RMode/FZ → MXCSR.
* **TLS** : `mrs tpidr_el0/tpidrro_el0` lisent l'état ; `dc zva` met à zéro 64 octets.

Exemple de code émis (rtld) :

```cpp
void f_7100000000(CPUState* ctx) {
L_710000000c:
    TRACE(0x710000000cull); /* cmp x0, #0 */
    { (void)(subs64(ctx, ctx->x[0], 0x0ull)); }
    TRACE(0x7100000010ull); /* b.ne #0x7100000090 */
    if (!ctx->zf) goto L_7100000090;
    ...
    ctx->x[30] = 0x7100000034ull; PUSH_RET(0x7100000034ull); f_71000021e0(ctx);
```

## Banc de validation différentielle

La DLL du code recompilé (compilée avec `RECOMP_BENCH RECOMP_TRACE RECOMP_CHECK_RET`) est
chargée par ctypes dans le processus Python, qui alloue la mémoire invitée **aux mêmes adresses**
(`VirtualAlloc` à adresse fixe). Unicorn (CPU `max` : LSE, RCpc…) exécute le code ARM d'origine
sur une copie identique.

* `func` : pour chaque fonction et chaque essai, état initial aléatoire identique des deux côtés
  (arguments pointant vers une zone de travail remplie de pointeurs/valeurs, pile, TLS, V
  registres avec valeurs FP spéciales). Les deux côtés enregistrent l'état complet
  (X0–X30, SP, NZCV, V0–V31) **avant chaque instruction** ; les traces sont comparées pas à pas,
  puis l'état final et la mémoire (`.data/.bss`, pile, zone de travail, TLS). Le premier écart
  désigne l'instruction fautive. Les imports pointent vers des thunks (renvoient 0), `svc`
  renvoie 0, des deux côtés. Verdicts : `PASS` (identique, y compris faute/trap au même pas),
  `FAIL`, `NONCODE` (appel via un pointeur aléatoire vers des données : non comparable),
  `NOTBUILT` (cible hors du lot compilé), `UNIMPL`.
* `fuzz` : chaque mot d'instruction distinct est traduit isolément et exécuté sur N états
  aléatoires (registre de base des accès mémoire pointé vers la zone de travail, parfois non
  aligné) ; comparaison registres + fenêtre mémoire. `--mutate OLD=NEW` corrompt volontairement
  la traduction pour **vérifier que le banc détecte les erreurs** (fait : chaque mutation
  testée — flags, décalages, tailles d'accès, opérations FP, lanes — produit des échecs).
* Bogue connu de la référence : Unicorn 2.1.4 (QEMU 5) compare `ldsmax/ldsmin` 8/16/32 bits en
  **non signé**. Ces cas sont vérifiés par un oracle Python indépendant et classés `REFBUG`
  (notre traduction suit l'ARM ARM).

## Résultats (2026-11-08)

Voir `bench/results/validation_phase1.log` pour la dernière exécution complète (la suite rapide
`--quick`, relancée après la Phase 2, donne les mêmes résultats : `bench/results/validation.log`).

Chiffres de la dernière exécution complète de `bench/run_all.py` (code final) :

| Test | Résultat |
|---|---|
| Décodeur sur **tout main** | 49 811 607 instructions, **0 non gérée** |
| rtld, fonctions entières | 43 fonctions × 16 essais, 383 999 instructions comparées pas à pas : **670 PASS, 0 FAIL** (18 NONCODE) ; couverture 41 % (47,7 % sur un autre tirage) |
| subsdk3, fonctions entières | 151 fonctions × 16 essais, 38 139 instructions pas à pas : **2 416/2 416 PASS** ; couverture 38 % |
| rtld / subsdk3, fuzzing | 1 174 + 1 434 mots × 16 essais = 41 728 exécutions : **0 échec** |
| main, fuzzing FP/NEON | 30 702 mots distincts × 8 essais = 245 616 exécutions : **0 échec** |
| main, fuzzing entiers | 40 000 mots × 6 = 240 000 exécutions : **0 échec** |
| main, familles rares (LSE, ld/st structures, tbl, estimations…) | 25 000 mots × 8 = 200 000 : 199 993 PASS + 7 REFBUG (bogue Unicorn confirmé par oracle), **0 échec** |
| main, saturants (sqdmulh/sqrdmulh/sqshl) | 12 mots × 32 : 384/384 |
| main, fonctions entières | 600 fonctions (fermetures ≤ 60) × 8 essais, 624 047 instructions pas à pas : **4 670 PASS, 0 FAIL** (88 NONCODE, 42 NOTBUILT) |
| main, premiers lots compilés | lot 1 : 3 000 fonctions / 167 k instructions → -O2 en 19–23 s ; lot 2 : 20 000 fonctions / 1,29 M instructions, 112 Mo de C++ → -O2 en 41 s (16 threads), 23 Mo d'objets |

Bogues trouvés et corrigés grâce au banc pendant la Phase 1 : chute en fin de fonction sur un
`udf` de bourrage après un appel sans retour ; découpage quadratique des stubs PLT sans FDE ;
tables de sauts LLVM à base remontée hors de la boucle (rtld) ; artefacts du banc (data abort
Unicorn ≡ violation d'accès ; imports de données vers la zone des thunks).

Extrapolation main complet : ~4,3 Go de C++ (sans commentaires), ~25 min de compilation -O2 sur
16 threads, ~0,9 Go d'objets.

## Phase 2 — exécutable natif (2026-11-08)

### Construire et lancer

```bash
python fullbuild.py sdk subsdk0 subsdk1 subsdk2 subsdk3 main -j 15   # ≈ 33 min (main ≈ 30 min)
python link.py                                                          # ≈ 10 s → build/full/recomp_main.exe
build/full/recomp_main.exe [-v 0|1|2] [--log fichier]                   # journal : recomp_run.log
```

`fullbuild.py` découvre les fonctions en parallèle (tours jusqu'au point fixe), découpe en lots
d'environ 60 000 instructions, génère chaque lot sans lignes `TRACE`, le compile en -O2 puis
**supprime la source** : main = 251 466 fonctions, 51,3 M instructions, 3,1 Go de C++ jamais
présents ensemble sur le disque, 0,85 Go d'objets. Cache par contenu : relancer après une
modification du runtime ne recompile rien. `link.py` produit un exécutable de 890 Mo
(`--modules sdk,...` pour lier sans main pendant un build).

### Choix d'architecture : SDK recompilé, noyau émulé

Au lieu de réécrire à la main les 998 fonctions que main importe du SDK, **le SDK est recompilé
comme le reste** (2,2 M instructions) et on émule ce qu'il y a en dessous :

* **le noyau Horizon au niveau des `svc`** : le SDK ne contient que 57 `svc`, chacun dans un
  petit wrapper (ABI standard, vérifiée sur les wrappers) ;
* **les services système au niveau IPC** (HIPC/CMIF/TIPC), avec des bouchons qui journalisent
  chaque commande inconnue : le journal dresse la liste de ce qu'il reste à faire.

La libc, la libc++, `nn::os`, `nn::fs` côté client, l'allocateur, le TLS (le SDK écrit lui-même
`tpidr_el0`)… fonctionnent donc tels quels. Un remplacement HLE au niveau API reste possible
fonction par fonction : une fonction native inscrite dans le dispatch après les modules
remplace le code recompilé (appels via PLT/pointeurs).

### Ce que fait l'exécutable

1. Réserve l'espace invité `[0x70_0000_0000, 0x80_0000_0000)` (identité invité == hôte), lié avec
   `--disable-high-entropy-va` pour que Windows n'y place rien.
2. Charge les modules (`manifest.txt`), applique RELA/JMPREL/RELR (main : 94 052 relatives +
   2 763 symboliques, tout résolu), avec la portée globale de rtld (main d'abord : son `malloc`
   remplace celui du SDK).
3. Reproduit rtld : `nn::init::Start(handle du thread principal, base de main, f2, f3, f4)` avec
   trois rappels natifs ; `f3` exécute les `_init` dans l'ordre **sdk → subsdk3 → … → main** (rtld
   insère chaque module en tête de liste) ; renseigne la variable de rtld que `nn::ro` utilise.
4. Thread principal invité dans un thread hôte de 256 Mo de pile (pile invitée 1 Mo, priorité 44,
   d'après `main.npdm`).

Noyau émulé (`runtime/os/kernel.cpp`) : threads (un thread hôte par thread invité), mutex
`ArbitrateLock/Unlock`, condvars `WaitProcessWideKeyAtomic/SignalProcessWideKey`, arbitre
d'adresses `WaitForAddress/SignalToAddress` (3 modes de signal, comme mesosphère), événements,
`WaitSynchronization` multi-objets avec annulation, tas (`SetHeapSize`), `MapMemory` (par
déplacement : la source devient inaccessible, comme sur console), mémoire physique d'alias,
mémoire partagée, `QueryMemory` sur une vraie carte des régions, `GetInfo`.

Plantages : pile d'appels **invitée** reconstituée en déroulant la pile hôte et en associant
chaque adresse hôte à sa fonction recompilée, avec les symboles du SDK démanglés.

### Résultat : jusqu'où le jeu démarre

Le jeu passe toute l'initialisation du SDK et ses propres constructeurs, entre dans `nnMain`,
monte sa romfs et écrit dans son journal :

```
[lm] vfs.nintendo.cpp(79): note: fs: MountRom succeeded.
[lm] ==[ CPU:Switch 2 (2), RAM:8192MB (2), OS:0 (new), GPU:Switch 2 (2), VRAM:0MB (2), Driver:recommended, Gen:2 ]==
[lm] vgpu.impl.c(229): note: game-0-gpu: Choosing default backend nvn (3)
```

Il lance ensuite ~8 threads de travail, tourne 1,4 s, puis s'arrête (de façon reproductible,
6 lancements sur 6) dans `nn::gs::sdk::BootGraphicsSystem` → `nn::gsv::BootWithRemoteMemory` :
le démarrage du système graphique de la Switch 2, qui attend un handle de `ISelfController`
(commande 2000, inconnue). **C'est la frontière de la phase graphismes.**

Étapes franchies dans l'ordre : `nnosInitialize`, `nninitInitializeSdkModule` (applets, aoc,
pctl), `nntimeStartup`, `nninitStartup` de main (tas + VAMM), constructeurs de tous les
modules (dont l'exception C++ évitée en réparant `clock_gettime`), `nnMain` → montage romfs,
sockets, nifm, `nn::ro`, comptes, puis graphismes.

### Faits découverts (noyau / services Switch 2)

* `GetInfo` : nouveaux identifiants. **29/30** = 3ᵉ paire adresse/taille de région (après tas et
  alias, pour un `AddressSpaceAllocator`) ; **31** = lu par `GetRemoteMemoryUsage` (0) ; **33** =
  taille de page (0x1000 ; `ProtectRelro` vérifie l'alignement dessus, le VAMM mappe sa 1ʳᵉ page
  de gestion avec) ; **32** inconnu (0). **16** (taille de ressources système) ≠ 0 active le
  VAMM (`nn::os::AllocateAddressRegion`), indispensable à main.
* Mémoire partagée de l'heure : nouveau bloc à 0xD0 (point d'ajustement continu, 2 × 0x38 o,
  identifiant de source non nul), sans lequel `clock_gettime(CLOCK_MONOTONIC)` renvoie EAGAIN et
  libc++ lève une exception pendant les constructeurs de subsdk2.
* `appletOE` : `OpenApplicationProxy` en commande 10. `bsd` : 36 = initialisation (version
  0x1000b), puis numérotation Switch 1 (corrigé en Phase 3 : 1 = `StartMonitoring`,
  2 = `Socket`) ; le moteur exige qu'une socket se crée, même hors ligne.
* subsdk2 (DLSS) utilise un descripteur TLS (`R_AARCH64_TLSDESC`) : relié à un résolveur natif
  qui s'arrête proprement s'il est appelé.

### Services HLE écrits (`runtime/os/svc_*.cpp`)

| Service | État |
|---|---|
| `sm:` | TIPC/CMIF : `GetServiceHandle` ouvre le service HLE ou un bouchon journalisé |
| `appletOE` | `IApplicationProxy` et ses sous-interfaces (état commun + messages, contrôleurs, fonctions d'application, utilisateur présélectionné, `IStorage`) |
| `lm` | journal du SDK et **du jeu** décodé (texte, fonction) |
| `time:*` | horloges système/stationnaire, mémoire partagée (dont le bloc 0xD0), fuseau horaire |
| `fsp-srv` | **romfs virtuelle** (en-tête et tables RomFS construits en mémoire, données lues en place dans le `.nup`, en lecture seule) ; sauvegardes et carte SD dans des dossiers hôtes (`build/full/save`, `sdcard`) |
| `acc:*`, `nifm:*`, `pctl*`, `aoc:u` | utilisateur local unique, réseau jamais connecté |
| `bsd:*` | numérotation Switch 1 (+ 36 = init Switch 2), errno FreeBSD ; `eventfd` réels, `poll` bloquant, **boucle locale TCP** (bind/listen/connect/accept/send/recv : la boucle d'événements du moteur s'en sert comme d'un `socketpair`) ; toute autre destination est injoignable |
| `hid` | mémoire partagée avec une **manette Pro connectée en joueur 1** (voir Phase 3), vibrations acceptées |

## Phase 3 — démarrage graphique et boucle de rendu (2026-11-08)

### Lancer

```bash
python link.py                                   # après modification du runtime (≈ 10-40 s)
build/full/recomp_main.exe                       # fenêtre « recomp_main » + journal recomp_run.log
build/full/recomp_main.exe --dump 35             # + état de tous les threads à 35 s
build/full/recomp_main.exe --trace-nvn           # chaque appel NVN (natif ou bouchon) avec l'appelant
build/full/recomp_main.exe --trace-api           # premiers appels main → SDK par thread
```

Modifier `hooks.txt` impose de régénérer le module concerné (`python fullbuild.py main -j 15`,
≈ 2,5 min en incrémental grâce au cache par contenu).

### Résultat

Le jeu dépasse le démarrage graphique, initialise NVN, ouvre la fenêtre hôte, charge
`bootstrap`, `global`, puis l'écran titre (`title_screen_ftf`) et `prologue_cin`. Sa **boucle de
rendu présente des images en continu** (≈ 42 images/s, 90 s sans plantage) :

```
[gfx] nn::gs::sdk::BootGraphicsSystem(mémoire 0x0, 0x0) : court-circuité (NVN en HLE)
[nvn] nvnWindowInitialize : 3 textures 3840x2160 (format 56)
[gfx] fenêtre hôte 2560x1440
[lm ] game-0-render: Calling vrender_create
[nvn] image 1 présentée (texture 0/3, couleur 0.00 0.00 0.00)
[lm ] game-0-package: Loading …/environments/title_screen/title_screen_ftf.level.package_list
[hid] manette Pro émulée connectée (joueur 1)
[gfx] 42.4 img/s (image 853)
```

**Le contenu des images n'est pas encore rendu** : les commandes NVN (draws, dispatchs compute,
shaders) sont acceptées puis ignorées, et la fenêtre affiche la dernière couleur d'effacement
de la texture présentée, avec le compteur d'images. C'est la Phase 4 (traduction du rendu).

### Architecture

* **`nn::gs` / `nn::gsv`** (démarrage du système graphique Switch 2 : mémoire distante, nvsrv,
  syncpoints) : `BootGraphicsSystem` et `CheckGraphicsSystemInitialized` deviennent des no-op,
  les attentes de `NativeSync` réussissent tout de suite.
* **`nn::vi`** : un affichage, une couche et une fenêtre native fictifs (1080p, 60 Hz, SDR).
* **NVN en HLE** : le jeu obtient tout NVN par `nvnBootstrapLoader("nvnDeviceGetProcAddress")`
  puis 728 noms (`nvnLoadCProcs`). On renvoie des **adresses natives** : 124 fonctions
  implémentées (device, queue, sync, pools, buffers, textures, fenêtre, tampons de commandes,
  présentation) et, pour les autres, des bouchons nommés tirés d'une réserve de 1024, qui
  journalisent leur premier appel et renvoient 0. Adresse GPU = adresse CPU ; les syncs sont
  toujours signalés.
* **Identité des objets NVN** : le moteur **copie** des objets NVN initialisés (structures
  opaques dont l'état vit dans leur mémoire sur la console). À l'initialisation, on écrit donc
  dans leurs 8 premiers octets un identifiant (`NVH1` | id) qui suit les copies ; l'état hôte est
  indexé par cet identifiant.
* **Fibres et coroutines** : les fibres `nn::os` et les coroutines en assembleur du moteur
  (`main+0xba33c30`, sauvegarde des registres callee-saved puis échange de piles) sont rendues
  par des fibres Windows, chacune avec son `CPUState`. `hooks.txt` fait passer les appels directs
  à ces fonctions par le dispatch, où les natives les remplacent.
* **Audio** : flux `nn::audio` consommés à 48 kHz par une horloge (événements signalés), décodeur
  Opus matériel → silence (960 échantillons par paquet). Depuis le 2026-11-09, le son sort sur la
  carte son (voir « Son »).
* **Entrées** : la mémoire partagée hid présente une manette Pro (style FullKey) connectée en
  joueur 1, alimentée à 60 Hz par `input.cpp` (clavier quand la fenêtre a le focus, première
  manette XInput) :

  | Commande Switch | Clavier (par position, QWERTY / AZERTY) | Manette XInput |
  |---|---|---|
  | stick gauche / droit | W A S D (Z Q S D) / I J K L | sticks |
  | croix | flèches | croix |
  | A / B / X / Y | C ou Entrée / X ou Retour arrière / V / Z (W) | B / A / Y / X (par position) |
  | L / R / ZL / ZR | Q (A) / E / R / T | LB / RB / LT / RT |
  | + / − | M (,) ou Échap / N ou Tab | Start / Back |
  | clics des sticks | F / H | clics |

* **Caméra** (`nn::camera`, webcam USB) : bibliothèque initialisée, aucun périphérique branché
  (0x7c4 → « No camera device detected. »). `nn::chat` (GameChat) est neutralisé.

### Faits découverts (Switch 2, SDK 21)

* Mémoire partagée hid : Npad à 0x9A00 + i × 0x5000 comme sur Switch 1 (Handheld = entrée 8,
  Other = 9), LIFO de 17 entrées protégées par un numéro de séquence (impair pendant
  l'écriture), numéros d'échantillon obligatoirement consécutifs, type de périphérique à
  +0x4188. **Nouveau** : bloc `NpadCondition` à 0x3E200 (dernier Npad actif, type de maintien,
  drapeau initialisé), lu directement par `GetNpadJoyHoldType` (sinon `ShowControllerSupport`
  avorte).
* Domaines IPC : les sessions clonées (`CloneCurrentObject`) **partagent** la table d'objets du
  domaine (le client fs ouvre un fichier sur une session et le lit sur une autre).
* `nn::camera::Initialize` → 0xdc4 (« plateforme sans caméra ») mène à un chemin du moteur qui
  déréférence un pointeur nul : ce cas n'arrive jamais sur la console. Le chemin sûr est « aucun
  périphérique ».
* Boucle d'événements du moteur (type libuv) : son réveil inter-threads passe par une paire de
  sockets TCP en boucle locale ; sans elle, la boucle plante (liste de rappels vide, -ECANCELED).

### Diagnostic

* `--trace-nvn` : chaque appel NVN avec ses 4 premiers arguments, l'appelant symbolisé et le
  thread. C'est ce qui a montré la copie d'un `NVNbuffer` entre `nvnBufferInitialize` et
  `nvnBufferMap`.
* Instruction invitée fautive : le plantage donne `fonction (hôte +décalage)`. `nm` sur les objets
  de `build/full/<module>/` donne l'adresse de `f_<adresse>` ; `objdump -d` autour de
  adresse + décalage montre le code hôte ; les décalages de `CPUState` (`0x8*n` = `x[n]`,
  `0xf8` = `sp`) permettent de retrouver la séquence invitée.

## Phase 4 — rendu Vulkan (2026-11-08, en cours)

### Lancer

```bash
build/full/recomp_main.exe                       # rendu Vulkan si disponible (sinon présentation GDI)
python shaders/shaderc.py -j 12                  # traduit les shaders vidés → build/full/shader_cache
python link.py                                   # après modification du runtime
```

| Variable | Effet |
|---|---|
| `RECOMP_RESOLUTION` | remplace `resolution` de `recomp.ini` (`720`, `1080`, `1440`, `2160`, `auto`) |
| `RECOMP_REFRESH` | remplace `refresh_rate` de `recomp.ini` (Hz) |
| `RECOMP_NATIVE_RES` | remplace `native_resolution` de `recomp.ini` (`0` : rendu à 50 % comme la console) |
| `RECOMP_CVARS="nom=valeur;…"` | variables du moteur ajoutées à `veclipse_build.cvar` |
| `RECOMP_FRAMES_IN_FLIGHT` | images en attente d'affichage avant blocage de l'acquisition (2 par défaut) |
| `RECOMP_NO_VSYNC_EMU=1` | pas de contre-pression au vblank virtuel (cadence libre) |
| `RECOMP_FH_MODE=9` | `ListFrameHistory` vide (régulateur du moteur en boucle ouverte) |
| `RECOMP_PERF=1` | temps par image : présentation, thread GPU, opérations, étapes de `draw`, attentes de syncs |
| `RECOMP_FRAME_TRACE=N`, `RECOMP_FRAME_TRACE_MS=x` | appels système de plus de x ms (1 par défaut) des images N à N+2 |
| `RECOMP_TS_LOG=N` | horodatages GPU écrits et relus par le jeu (images N à N+3) |
| `RECOMP_AUTOPRESS=1` | appuie sur A 100 ms toutes les 2 s (écran titre) |
| `RECOMP_RECORD_DIR=dossier` | extraits MP4 des cinématiques pour le launcher (`record.cpp`) : repérées à leurs bandes noires, recadrées sans elles ; `RECOMP_RECORD_CLIPS` (6), `RECOMP_RECORD_SECONDS` (9), `RECOMP_RECORD_MIN` (4), `RECOMP_RECORD_GAP` (12), `RECOMP_RECORD_AFTER` (image), `RECOMP_RECORD_EXIT=1` (quitter après le dernier) |
| `RECOMP_DUMP_SHADERS=1` | vide tous les shaders du jeu dans `build/full/shaders/` (entrée de `shaderc.py`). Sans cette variable, seuls les shaders inconnus (ni traduits, ni déjà vidés) sont écrits, avec une ligne « shader inconnu » au journal |
| `RECOMP_NO_SHADER_DUMP=1` | n'écrit aucun shader, même inconnu |
| `RECOMP_SAVE_DIR=dossier` | sauvegarde du jeu dans ce dossier au lieu de `build/full/save/` : à utiliser pour tous les essais automatisés, afin de laisser intacte celle du joueur |
| `RECOMP_SNAPSHOT=N,M` | PNG de l'image présentée N, M (zone recadrée, moitié de taille) |
| `RECOMP_FRAME_STATS=N` | journal de l'image N : commandes, programmes, liaisons, cibles |
| `RECOMP_VK_DEBUG=N` | journal Vulkan de l'image N : poignée → objet texture → image Vulkan, adresses |
| `RECOMP_NO_VULKAN=1` | désactive Vulkan |
| `RECOMP_PEEK=adr[:n],…` | avec `--dump` : contenu de la mémoire invitée |
| `RECOMP_TEX_LOG=1` | journal de chaque `nvnTextureInitialize` : taille, format, swizzle, disposition packagée |
| `RECOMP_TEX_DUMP=N` | à l'image N, mémoire brute des textures packagées dans `build/full/tex_dump/` (décodage : `shaders/texview.py`) |
| `RECOMP_NO_TEX_UPLOAD=1` | n'envoie pas les textures du jeu (gris neutre, comportement d'avant) |
| `RECOMP_RT_SNAPSHOT=N` | à l'image N, PNG de chaque cible de rendu (couleur, et profondeur en niveaux de gris étirés) à la fin de sa passe (`build/full/rt_N_*.png`) |
| `RECOMP_FRAME_STATS=N+K` | journal des images N à N+K-1 |
| `RECOMP_DRAW_DUMP=N:k` | vide le k-ième dessin indexé de l'image N dans `build/full/drawdump/` (constantes, sommets, index, état) et relit ces tampons par le GPU (`*.gpu.bin` / `*.cpu.bin`) |
| `RECOMP_GPU_DIAG=1` | marqueurs NVIDIA (`VK_NV_device_diagnostic_checkpoints`) avant/après chaque dessin et calcul, rapport `VK_EXT_device_fault` : à un blocage, désigne le dessin en cours |
| `RECOMP_GPU_SERIAL=1` | chaque dessin se termine avant le suivant (même lot) : avec `RECOMP_GPU_DIAG`, désigne exactement le dessin fautif |
| `RECOMP_SYNC_FROM=N` | à partir de l'image N, chaque dessin / calcul est soumis et attendu seul |
| `RECOMP_FLUSH_SUBMIT=1` | le GPU exécute chaque soumission du jeu avant qu'elle ne rende la main |
| `RECOMP_DRAW_WATCH=hash` | liaisons (textures séparées → image, cibles, état) des dessins de ce shader, à chaque changement |
| `RECOMP_DBG_SSBO=1` | avec `RECOMP_DRAW_WATCH` : tampon inscriptible en jeu 1, liaison 31, pour un shader modifié à la main (valeurs de registres relues après chaque image) |
| `RECOMP_TEX_SLOT_WATCH=id`, `RECOMP_SAMP_WATCH=id` | réécritures d'un emplacement du jeu de textures / d'échantillonneurs, paramètres NVN de l'échantillonneur |
| `RECOMP_POOL_AUDIT=N` | toutes les N images, échantillons de chaque pool importé relus par le GPU et comparés au processeur ; ensemble de travail, budget mémoire Vulkan |
| `RECOMP_POOL_LOG=1`, `RECOMP_MEM_LOG=1` | création / destruction des pools NVN ; appels système de mappage mémoire |
| `RECOMP_NO_COMPARE`, `RECOMP_NO_BORDER`, `RECOMP_NO_3D_TEX`, `RECOMP_NO_ATT_BARRIER`, `RECOMP_NO_DEPTH_BIAS`, `RECOMP_NO_CULL` | désactivent une fonction pour un diagnostic (comparaison des échantillonneurs, mode bord, vues 3D, barrières de fin de passe, biais de profondeur, élimination des faces) |
| `RECOMP_VIEWPORT_VK=1` | ancienne convention de viewport (hauteur positive) : la scène est rangée à l'envers |
| `RECOMP_FLIP_FRONT=1` | inverse le sens des faces avant |
| `RECOMP_DEPTH_EQUAL_AS=f` | remplace le test de profondeur EQUAL par la fonction NVN f (7 = GEQUAL, 8 = ALWAYS) |
| `RECOMP_NO_DEPTH_ONLY=1` | ignore les dessins sans shader de fragments (masque de profondeur de l'interface) |
| `RECOMP_NO_COPY_TEX=w` | ignore les `CopyBufferToTexture` vers les textures de largeur w (0 : toutes) |
| `RECOMP_SKIP_COPY_TEX_1X1=1` | ignore les copies vers les textures 1×1 (ancien contournement de la limite 28, résolue) |
| `RECOMP_FINISH_NO_FLUSH=1` | `nvnQueueFinish` sans soumission du travail Vulkan enregistré (ancienne cause du texte en pavés) |
| `RECOMP_COPY_IN_PLACE=1` | `CopyBufferToTexture` lit la source au moment de l'exécution GPU au lieu d'un instantané pris à l'enregistrement |
| `RECOMP_CPU_BUFFER_COPY=1` | `CopyBufferToBuffer` par le processeur à l'enregistrement (ancien comportement) au lieu d'une copie GPU ordonnée |
| `RECOMP_GPU_PROFILE=N` | à l'image N, durée GPU de chaque dessin et calcul (horodatages), sommée par programme : 30 plus coûteux au journal |
| `RECOMP_SYNC_SUBMIT=1` | soumissions synchrones : le thread GPU attend la fin de chaque soumission (avant la 8e passe) |
| `RECOMP_SNAPSHOT_EVERY=n`, `RECOMP_SNAPSHOT_DIR=d` | un instantané toutes les n images ; dossier (existant) des instantanés |
| `RECOMP_VOLUME=0..100` | volume de sortie (100 par défaut ; 0 : muet, la carte son rythme quand même l'audio du jeu) |
| `RECOMP_NO_AUDIO=1` | pas de sortie audio : flux consommés par l'horloge virtuelle (comportement d'avant) |
| `RECOMP_AUDIO_WAV=f.wav` | enregistre le mixage du jeu (48 kHz, stéréo, flottant, avant le volume) |
| `RECOMP_AUDIO_LOG=1` | toutes les 10 s : trames écrites, jouées, manques et crête de chaque flux ; décodeurs et paquets Opus |
| `RECOMP_COPY_LOG=1` | octets copiés vers les petites textures (≤ 64 o) |
| `RECOMP_NO_TEX_VIEW=1` | ignore les vues du pool de textures (permutation, niveaux, couches) |
| `RECOMP_SAMP_LOG=1` | paramètres NVN de chaque échantillonneur créé (filtres, enveloppe, comparaison, LOD) |
| `RECOMP_OLD_FILTERS=1` | ancienne table des filtres (base 1, linéaire et plus proche inversés) : diagnostic |
| `RECOMP_DISPATCH_LOG=1` | chaque programme de calcul exécuté (une fois) et la disposition de sa banque 0 |
| `RECOMP_NO_GLOBAL_MEM=1` | ignore les calculs à mémoire globale (`LDG`/`STG`/atomiques) |
| `RECOMP_NO_TEX_COPY=1` | ignore `CopyTextureToTexture` (ancien comportement : atlas d'ombres périmé) |
| `RECOMP_WINDOW` | remplace `window_mode` de `recomp.ini` (`borderless`, `windowed`) |
| `RECOMP_SKIP=motif1,motif2` | ignore les dessins et calculs dont l'étiquette du programme contient un motif, ou dont un shader a cette empreinte (ex. `ssao`, `volumetric_fog`). Sous Git Bash, un motif qui commence par `/` est converti en chemin Windows et ne correspond plus à rien |
| `RECOMP_KEY_SCRIPT="38:A,43:-A"` | appuis scriptés : bouton (A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK) appuyé, ou relâché avec `-`, à l'instant donné en secondes. Tenir A 5 s passe une cinématique ; « 38:A,43:-A,52:A,57:-A » mène en général à l'entraînement en 70 s (selon les temps de chargement) |
| `RECOMP_BACKGROUND_INPUT=1` | lit la manette XInput même quand la fenêtre n'a pas le focus (ancien comportement) |
| `RECOMP_INPUT_LOG=1` | journal de chaque changement de bouton et de présence de la manette XInput |
| `RECOMP_CNTFRQ=hz` | fréquence du compteur invité (31 250 000 par défaut, celle de la Switch 2) |
| `RECOMP_ALIAS_PACKAGED=1` | ancienne réutilisation, par adresse, d'une image packagée sans comparer son contenu (textures périmées) |
| `SHADERC_NO_CLIP=1` | (`shaderc.py`) traduit sans `gl_ClipDistance` |

Couche de validation (paquet MSYS2, déjà présent) :
`VK_LAYER_PATH='C:\msys64\ucrt64\bin' VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` ; seule
erreur restante : `Input-08733` (limite 23). Avec les tableaux sans liaison, le jeu tombe à
1-5 img/s sous validation.

### Résultat

Chaque image du jeu est exécutée sur le GPU (RTX 5070 Ti, Vulkan 1.4), à ≈ 48-50 images/s :
G-buffer 960×540, SSAO, FXAA, TAA, suréchantillonnage en 1920×1080, étalonnage final par une
**LUT 3D 64³ calculée au démarrage par des shaders de calcul** (`scene_lut_generation`,
`display_lut_generation`, `env_brdf`), puis présentation de la zone 1920×1080 recadrée de la
texture fenêtre 3840×2160. L'image affichée est une couleur uniforme : **la scène ne contient
encore aucune géométrie**, car le jeu reste à l'écran titre (chargement de `prologue_cin` bloqué
dans le moteur audio Wwise, limite 14).

Étapes franchies :

1. **Résolution 1×1 → 1920×1080** : le moteur lit la taille de l'écran dans `DisplayInfo` (+0x50 /
   +0x58, largeur/hauteur max de couche) rendu par `nn::vi::ListDisplays`, que le HLE laissait à
   zéro ; `QueryDisplayBufferMaxResolution` renvoyait 1920 au lieu d'une énumération (0..3).
2. **Images Vulkan partagées par adresse** : le moteur décrit souvent la même mémoire avec
   plusieurs objets texture (cible de rendu, puis texture du pool) ; ils partagent une image.
3. **Initialisation hors passe de rendu** : transitions et remplissages étaient émis dans une
   passe de rendu dynamique (erreur de validation) ; liaisons faites avant la passe.
4. **Shaders de calcul** : décodage de `SUST`/`SULD` à poignée en banque constante (`0xb9b`…),
   traduction en `imageStore`/`imageLoad` sans format (`GL_EXT_shader_image_load_formatted`),
   images de stockage sans liaison, `vkr::dispatch`, `BindImages`.
5. **Recadrage de fenêtre** (`nvnWindowSetCrop`) appliqué à la présentation et aux instantanés.

### Architecture

* `shaders/` : `nvshader.py` (section de contrôle 0xE0 o + en-tête SPH v4 0x80 o), `sass.py`
  (décodeur SM 8.x, 128 bits), `glsl.py` (SASS → GLSL : boucle `switch (pc)`, registres `uint`,
  banques constantes en SSBO std430), `shaderc.py` (lots → SPIR-V par glslangValidator
  MSYS2, + `.meta` lu par le runtime). **55/60 étages graphiques et 9/170 de calcul** traduits
  (les autres calculs lisent la mémoire globale : `LDG`/`STG`).
* `runtime/os/nvn_cmd.cpp` : enregistrement des tampons de commandes (liste d'opérations),
  objets d'état NVN, programmes, pools de textures/échantillonneurs, poignées.
  `nvn_exec.cpp` : exécution à la soumission (états, compteurs, copies tampon → tampon,
  appels du moteur). `vk_render.cpp` : moteur Vulkan.
* Moteur : pools NVN importés (`VK_EXT_external_memory_host`, tampons de sommets, d'index et
  uniformes lus en place), une image Vulkan par objet texture (partagée par adresse), rendu
  dynamique, pipelines en cache (empreinte des états), descripteurs sans liaison indexés par
  l'identifiant du pool NVN (jeu 2 : textures 0-6, échantillonneurs 8, images de stockage
  10-15), banques constantes en SSBO (jeux 0/1), constantes pilote générées dans un anneau,
  présentation par blit sur une swapchain FIFO.

### Faits découverts (NVN2 / Switch 2)

* `nn::vi::DisplayInfo` = 0x60 o : nom[0x40], bool, i64 nb de couches, i64 largeur, i64 hauteur.
  `BufferMaxResolution` : 0 720p, 1 1080p, 2 1440p, 3 2160p (table du moteur).
* Graphique : UBO du slot k → banque **k+3** (comme en calcul ; k+2, retenu d'abord, lisait les
  matrices dans le mauvais tampon) ; banque 1 = constantes du programme (tables de saut de `BRX`) ;
  banque 2 = poignées (voir « La cinématique s'affiche ») ; banque 0x18 = constantes pilote
  (1/taille de cible, taille, viewport en entiers).
* Calcul : banque 0 = pilote (poignées d'images à partir de 0x10, ou 0x20 si un UBO est lu),
  UBO du slot 0 → banque 3 (vérifié sur 3 shaders).
* Images et textures partagent l'espace d'identifiants du pool (`RegisterImage`). `SUST`/`SULD`
  : dimension 0 1D, 2 1D tableau, 3 2D, 4 2D tableau, 5 3D.
* Le jeu remplit 821 textures directement en mémoire (données déjà au format GPU) et n'utilise
  que 11 `CopyBufferToTexture`.

### Au-delà de l'écran titre (2026-11-08, 4e passe)

**L'« interblocage Wwise » était un réveil perdu du noyau émulé.** `nn::os` du SDK teste le mot de
la variable de condition en mode utilisateur : `InternalConditionVariableImplByHorizon::Signal()` /
`Broadcast()` ne font pas l'appel système `SignalProcessWideKey` s'il vaut 0. Notre
`WaitProcessWideKeyAtomic` libérait le mutex **puis** écrivait 1 dans ce mot : un thread qui prenait
le mutex entre les deux signalait dans le vide et l'attendant dormait pour toujours. Le noyau Horizon
écrit le mot avant de libérer le mutex (`KConditionVariable::Wait`) ; `kernel.cpp` fait désormais de
même. Indice décisif : au blocage, l'événement attendu par `AK::AudioMgr` (`main+0xf0f3ca0`) avait
son drapeau « signalé » à 1 alors que le thread dormait encore sur sa condvar. Les autres formes du
blocage (`AK::BankMgr` attendant la fin d'une lecture du thread `AK::IOThread`) avaient la même
cause. Résultat : 12 lancements sur 12 passent l'écran titre (prologue à ≈ 8,5 s, chargement du
tutoriel `map_tutorial` à ≈ 31 s, puis de la cinématique d'ouverture `corneria_briefing`), à 60 img/s.
Le blocage rare au démarrage (limite 16) n'est plus apparu depuis.

**Textures packagées** : envoyées au premier usage (`upload_packaged`, `vk_render.cpp`). Tous les
formats packagés du jeu sont des BCn (BC1, BC3, BC4, BC7) ou non compressés : pas d'ASTC. La
disposition a été établie sur les données (vidage brut, recherche de la permutation qui donne
l'image la plus lisse, puis vérification visuelle de 11 textures et de leurs mips) :

* GOB de 64 o × 8 lignes ; l'unité de 16 o (octets `x[0..3]` d'une ligne) est rangée selon
  `y0` (bit 4), `y1` (bit 5), `x4` (bit 6), `y2` (bit 7), `x5` (bit 8) — **pas** l'ordre Maxwell
  (`y0, x4, y1, y2, x5`) ;
* blocs de `bh` GOB de haut, en colonnes d'un GOB de large ; `bh` d'un niveau =
  min(2^L, plus petite puissance de 2 de GOB couvrant le niveau, 16 au plus), où L est le premier
  octet de `NVNpackagedTextureLayout` (4 en général, 0 pour les tailles non puissances de 2) ;
* niveaux de mip à la suite (largeur alignée sur 64 o, hauteur sur 8·bh lignes).

`nvnTextureBuilderSetPackagedTextureLayout`, `SetStride`, `SetSwizzle` (appliqué aux vues :
`RGB1`, `RRR1` pour le BC4) sont implémentés.

**Traducteur de shaders** (131/317 étages, contre 120) : `F2I` 8/16 bits et source f16 ; `BREAK`
(sans effet par thread, comme `BSSY`/`BSYNC`) ; lectures d'ombre `TEX.DC` / `TLD4.DC`
(`sampler*Shadow`, échantillonneurs à comparaison côté Vulkan) ; `TXD` (`textureGrad`, opérandes :
coordonnées puis couche, dérivées entrelacées) ; `HSETP2` ; `BRX` (saut indirect : cible = adresse
lue dans la **banque 1**, table de saut). La banque 1 d'un programme = constantes placées après son
code (`code + ctl[0x40]` à `code + ctl[0x44]`) : `RECOMP_DUMP_SHADERS` la vide (`.cb1`) et le
traducteur l'intègre comme tableau constant. Les vidages sont nommés par empreinte (`h<hash>_<i>`).
Formes `0xf60` / `0xf6c` (TEX / TXD, bit 91) : décodées provisoirement comme `0xb60` / `0xb6c`.

**Image** : la cinématique s'exécute (cascades d'ombres 1024², atlas d'ombres 8192², passe
principale 2560×1440 R11G11B10F + RG16 + D32S8, SSAO et brouillard volumétrique en calcul), ≈ 680
dessins par image dont ≈ 70 ignorés, mais l'image reste vide : le matériau principal
(`main.shader_graph[standard]`) n'est pas encore traduit (limite 22).

### La cinématique s'affiche (2026-11-09, 5e passe)

**Poignées et banques (établi sur les lectures des shaders et les liaisons du jeu).**

* UBO du slot k → banque k+3 en graphique comme en calcul. C'est cette correction qui a fait
  apparaître la première vraie géométrie (scène spatiale de l'écran titre). Elle a été vérifiée en
  recalculant un shader de sommets sur processeur (`vsemu.py`) avec les tampons vidés par
  `RECOMP_DRAW_DUMP`.
* Banque 2 (0x660 o, construite à chaque dessin) :

  | Déplacement | Contenu | Liaison NVN |
  |---|---|---|
  | `0x20 + 8k` | 32 poignées combinées | `BindTextures` |
  | `0x120 + 8k` | 8 images | `BindImages` |
  | `0x168 + 8k` | 128 textures séparées (0x160 avant la 6e passe : faux) | `BindSeparateTextures` |
  | `0x568 + 8k` | 32 échantillonneurs séparés (0x560 avant : faux) | `BindSeparateSamplers` |

  Poignée = identifiant de texture (20 bits) | identifiant d'échantillonneur << 20.
  `nvnDeviceGetSeparateSamplerHandle` rend donc `id << 20`, et les shaders combinent les deux
  poignées par un OU avant un `TEX` bindless (`0x361`, poignée en dernier opérande).
* Formes `0xf60` / `0xf6c` (bit 91) : poignée dans une **paire de registres uniformes**
  `URn` (texture | échantillonneur << 20) / `URn+1` (échantillonneur), n = bits 40-45. Corrigé à
  la 6e passe (voir plus bas) : ce n'était pas un indice de texture liée.
* `FMUL` : champ `pdiv` = mise à l'échelle par 2^(pdiv−4) (4 = ×1).
* Profondeur : projection infinie à Z inversé et origine haut-gauche. Le jeu n'appelle jamais
  `SetDepthMode`, d'où la plage [0, 1] par défaut.

**La cinématique « noire » était un GPU perdu.** Vers 38 s (images 1 800 à 2 080 selon le
minutage), un lot de commandes ne se terminait pas. Windows réinitialisait le GPU au bout de 2 s
(TDR) et le périphérique Vulkan était perdu, en silence. Toutes les images suivantes étaient noires
et les relectures nulles : cartes d'ombres « sans profondeur », pools importés lus à zéro par le
GPU. La chaîne de diagnostic :

1. relecture GPU des tampons du dessin vidé : tout est nul ;
2. contrôle périodique des pools : la rupture est datée, juste après 2 s sans réponse ;
3. codes de retour : `VK_ERROR_DEVICE_LOST` ;
4. marqueurs NVIDIA et mode sérialisé : le dessin fautif est un cube de 36 indices du matériau
   `main.frag.glsl` (sommets `ea1055854ca01c0b`, fragments `00f1ea3b370dd924`) dans la passe
   principale ;
5. neutralisation par dichotomie de ses lectures de texture, puis mesures dans le shader
   lui-même (tampon `RECOMP_DBG_SSBO`).

Corrections, toutes conservées :

* **`CALL.REL.NOINC`** (bit 86 de `0x944`) est un saut, pas un appel. C'est le cas des 95 `CALL`
  vidés : le compilateur sort ainsi des boucles divergentes (`@P CALL.NOINC sortie ; BRA tête`)
  et appelle des sous-programmes dont l'adresse de retour est dans un registre. Les empiler
  faisait déborder `cstack[8]` dans les boucles imbriquées. **`RET.REL.NODEC Ra`** = saut
  indirect vers `Ra` (adresse rangée par `MOV` avant l'appel).
* **Descripteurs factices** : les 12 tableaux sans liaison (16 384 emplacements chacun) et les
  4 096 échantillonneurs reçoivent d'abord une image 1×1 noire du bon type et un échantillonneur
  par défaut. Un emplacement jamais écrit puis lu faisait lire l'adresse 0 au GPU.
* **Combinaisons non uniformes** : `nonuniformEXT(sampler…(T[…], SMP[…]))`, car le résultat
  d'`OpSampledImage` doit lui-même être `NonUniform`. Les identifiants sont bornés à 16 383.
* **`robustBufferAccess2` / `robustImageAccess2`** : une lecture hors plage d'une banque renvoie
  0, comme `LDC` sur le matériel, et non le contenu suivant du pool importé.
* Barrière d'image (GENERAL → GENERAL) sur chaque cible à la fin d'une passe de rendu.
* **Garde-fou dans l'aiguilleur des shaders à boucles** : au-delà d'un million de transitions de
  bloc, l'invocation s'arrête. Avec ce seul ajout, le shader fautif ne bloque plus le GPU. Le
  garde-fou ne s'est jamais déclenché, et aucune boucle infinie n'a été mesurée : toute
  modification du shader suffisait à faire disparaître la panne, et la cause exacte chez le
  pilote reste inconnue (limite 24).

Résultat : deux exécutions complètes (75 s) sans perte du périphérique, à 60 img/s. La
cinématique d'ouverture s'affiche : décors, personnages, éclairage et ombres. Les cascades
d'ombres de l'image 2401 contiennent maintenant de vraies profondeurs (jusqu'à 0,30, contre 0
auparavant). Défauts visibles : motifs « tissés » sur beaucoup de surfaces, sphère bleue,
surexposition (l'exposition automatique est calculée par des shaders de calcul encore non
traduits), voir limite 25. Traducteur : 153 étages sur 317 (graphiques 139/144).

### Rendu corrigé (2026-11-09, 6e passe)

Point de départ : la cinématique « ignoble » (image à l'envers, motifs tissés, pièce noire).
Chaque cause a été isolée sur les captures de passes (`RECOMP_RT_SNAPSHOT=N+K`) et le journal
d'image. Dix corrections, toutes établies sur les données du jeu :

1. **Orientation (viewport)**. NVN suit `glClipControl` : en origine haut-gauche, Y normalisé +1
   va à la ligne haute de l'image, comme en D3D, alors que Vulkan y met −1. Avec une hauteur de
   viewport positive, la scène était rangée à l'envers en mémoire. Les passes plein écran qui
   lisent leurs UV dans `gl_FragCoord` (distorsion) la gardaient telle quelle. Celles qui les
   lisent dans un attribut de sommet (flou de mouvement, mise à l'échelle finale) la
   retournaient. Correction : viewport `{x, y + h, w, −h}` en origine haut-gauche. La table
   holographique est maintenant au premier plan en bas, l'hologramme de Corneria au-dessus et
   l'invite « Passer » en haut à droite. Diagnostic : `RECOMP_VIEWPORT_VK=1` remet l'ancienne
   convention.
2. **Interpolation des attributs (IPA), cause des motifs « tissés »**. Le matériel interpole
   attribut/w à l'écran. Les shaders remultiplient ensuite par 1/`gl_FragCoord.w`, donc par w
   (vérifié sur le matériau principal et le flou de mouvement). Or l'entrée Vulkan « smooth » est
   déjà corrigée de la perspective : les UV des objets 3D se retrouvaient multipliées par leur
   profondeur. Le traducteur lit maintenant l'IMAP de l'en-tête SPH (2 bits par composante, en
   0x18 + k) :
   * perspective → `fin * gl_FragCoord.w` ;
   * linéaire à l'écran → `noperspective` ;
   * constante → `flat`.

   Sur les 317 étages vidés : 554 composantes P, 72 L, 5 C.
3. **Effacements**. `ClearColor` et `ClearDepthStencil` passaient par `draw()`, qui exige un
   programme graphique. Juste après un calcul (brouillard volumétrique, LUT, BRDF), ils étaient
   ignorés, et les cibles gardaient le contenu des images précédentes, ce qui masquait d'autres
   défauts. Ils ne dépendent plus du programme lié.
4. **Dessins de profondeur seule** (`vui_depth_mask`, sans shader de fragments, cibles couleur
   liées) : ils étaient ignorés, ils sont maintenant exécutés avec un masque d'écriture couleur
   nul.
5. **Forme `0xf60` / `0xf6c` de `TEX` / `TXD`**. Le champ 40-45 désigne une paire de registres
   uniformes, pas un emplacement de `BindTextures`. Le shader calcule juste avant
   `UR4 = c[2][texture séparée] | c[2][échantillonneur séparé]` puis `UR5 = UR4 >> 20`. La
   preuve : une lecture d'ombre, un tableau 2D et une texture 3D du matériau principal
   utilisaient tous « 4 », soit 23 lectures sur la même poignée.
6. **Disposition de la banque 2** : textures séparées en `0x168 + 8k`, échantillonneurs en
   `0x568 + 8k`. Avec ces bases, chaque lecture des matériaux tombe sur une texture du bon type :
   * carte d'ombre D16 8192² ou D32F 8192×4096 pour les comparaisons ;
   * tableaux 2D, volumes 3D, tables LTC 64×64 ;
   * volume du brouillard (88×50, 3D) pour le ciel et les surfaces holographiques ;
   * échantillonneurs liés (ss10-12).

   Avec 0x160 / 0x560, la comparaison d'ombre lisait une texture 3D 32×32 et l'échantillonneur
   un emplacement vide. C'est la correction qui a rallumé la pièce.
7. **Banque 0 des calculs**, lue dans la section de contrôle de chaque étage. La disposition est
   la suivante :
   * en-tête de 0x10 octets ;
   * 16 octets par tampon uniforme (nombre à l'octet 0xb1 du contrôle) ;
   * 8 octets par texture combinée (octet 0xb3) ;
   * 8 octets par image (octet 0xb6).

   La taille totale est le mot 0xb8, vérifié sur les 10 calculs traduits. Exemples :
   * SDF du texte (2 tampons, aucune texture) : image 0 en 0x30 ;
   * SSAO : t1 en 0x28, image 0 en 0x30 ;
   * intégration du brouillard : t0 en 0x20, image 1 en 0x30 ;
   * effacement de l'accumulation : images 5-7 en 0x48-0x58.

   L'ancienne heuristique (« images dans l'ordre des déplacements ») écrivait dans les images
   0-2 au lieu de 5-7, et aucune poignée de texture n'était fournie aux calculs. Le traducteur
   écrit maintenant les lignes `c0 nubo ntex nimg` et `texh banque:déplacement` dans les
   métadonnées.
8. **`FSET` rend 1.0 / 0.0 (.BF)**, et non un masque 0xffffffff. Les 40 `FSET` vidés ont le
   bit 72 à 0 (pris jusqu'ici pour .BF) et servent tous d'opérandes flottants. Le masque, un NaN en
   flottant, se propageait dans tout l'éclairage de la fourrure (`fur_vertex_color`) : la tête du
   Général Pepper était noire. Trouvé en instrumentant le shader : chaque invocation range la
   première instruction flottante qui produit un NaN (`0x1a0 : fma(R15, R3, R0)`, R15 = `FSET`).
9. **`CopyBufferToTexture` et `ClearTexture` exécutés**. Ils étaient enregistrés puis ignorés.
   * Il s'agit d'une copie Vulkan depuis le pool importé, dans l'ordre des commandes, avec une
     région bornée au niveau : une copie hors de l'image faisait perdre le GPU.
   * Le niveau, la couche et la clé de la texture sont figés à l'enregistrement. Le jeu réutilise
     un même objet vue, qu'il modifie avant chaque copie : lu à l'exécution, toutes les copies du
     tableau BC7 512×512×128 visaient son dernier niveau.
   * Ils remplissent les tables LTC 64×64, les textures par défaut et ce tableau, diffusé niveau
     par niveau.
   * Exception : les textures 1×1 (limite 28).
10. **Vues du pool de textures appliquées**. La vue passée à `nvnTexturePoolRegisterTexture`
    (permutation, plage de niveaux et de couches, mode profondeur / stencil) était ignorée :
    chaque texture était échantillonnée entière, avec la seule permutation de création.

Aussi :
* positions invariantes dans les shaders de sommets (`invariant gl_Position`, registres
  `precise`, aucune fusion FMUL + FADD). La passe « standard » teste la profondeur en égalité
  contre la pré-passe, dessinée avec un autre shader de sommets ;
* `gl_FrontFacing` converti exactement (`uintBitsToFloat(cond ? ~0u : 0u)`), plus de constante
  NaN.

Diagnostics ajoutés :
* chaque dessin ou calcul ignoré est journalisé une fois, avec sa cause et son programme
  (« dessin ignoré (shader de calcul absent) : … ») ;
* les tampons de sommets introuvables sont journalisés ;
* `RECOMP_DRAW_WATCH` affiche les fonctions et la couleur de mélange ;
* `RECOMP_DEPTH_EQUAL_AS`, `RECOMP_FLIP_FRONT`, `RECOMP_NO_DEPTH_ONLY`.

Le magenta de la cible TAA (format 42) est normal : l'historique est rangé en YCoCg, et le jeu
l'efface à (0,5 ; 0 ; 0,5).

Résultat : la cinématique est à l'endroit, texturée, éclairée et ombrée, et le texte est net. On
y voit le Général Pepper (fourrure, casquette, uniforme, médailles), la salle de briefing et
l'hologramme de Corneria. Un seul calcul reste ignoré par image : `volumetric_fog_light`
(mémoire globale). Restent un voile gris en ellipses (limite 25), des blocs noirs dans la passe
de flou de profondeur à demi-résolution (invisibles dans l'image finale), et une cadence de 46 à
59 img/s selon les plans (limite 27).

Outil de diagnostic : `RECOMP_DRAW_WATCH=hash RECOMP_DBG_SSBO=1` et un shader du cache instrumenté
à la main. Les scripts ad hoc ajoutent une ligne avant l'instruction d'adresse donnée et
recompilent le `.spv`. Les valeurs rangées sont lues dans le journal `[dbg]`, avec une copie
`.orig` à remettre en place ensuite.

### Réglages : `recomp.ini` (interface avec le futur launcher)

`build/full/recomp.ini` est lu à chaque lancement et créé avec les valeurs par défaut s'il manque.
C'est le point de contact prévu entre le launcher et l'exécutable : le launcher écrit ce fichier
(ou en passe un autre avec `--config <fichier>`), puis lance `recomp_main.exe`.

```ini
[display]
refresh_rate = 60          ; 60, 90 ou 120 Hz (30 à 240 acceptés, autres valeurs non testées)
resolution = auto          ; auto (écran principal de l'hôte), 720, 1080, 1440, 2160 (ou 4k)
native_resolution = true   ; false : rendu à 50 % puis agrandi, comme sur la console
window_mode = borderless   ; borderless (plein écran sans bordure) ou windowed (fenêtré)

[game]
language = auto            ; auto (langue de Windows), fr, en-US, en-GB, de, es, es-419, it, nl, pt, pt-BR,
                           ; ja, ko, zh-Hans, zh-Hant : code rendu au jeu par nn::oe::GetDesiredLanguage
```

* La langue du jeu (ajoutée le 2026-11-09 à la demande de l'utilisateur, aussi `RECOMP_LANGUAGE` et
  `--language`) est celle que le jeu reçoit à la place de la langue de la console. Vérifié : `en-US` met tout le
  menu en anglais. Le jeu contient, selon la boutique Nintendo, japonais, anglais, espagnol, français, portugais,
  allemand, italien, néerlandais, coréen, chinois simplifié et traditionnel ; il prend la plus proche.

* Priorité croissante : valeurs par défaut < fichier < variables `RECOMP_REFRESH`,
  `RECOMP_RESOLUTION`, `RECOMP_NATIVE_RES` < options `--refresh`, `--resolution`, `--native-res`.
* Booléens : `true/false`, `1/0`, `yes/no`, `on/off`, `oui/non`. Les sections et les commentaires
  (`;`, `#`) sont ignorés ; un BOM UTF-8 est accepté.
* Une valeur invalide ou une clé inconnue est signalée dans le journal (`[cfg]`) et ignorée ; le
  journal récapitule les réglages retenus et leur origine :
  `réglages : 90 Hz [recomp.ini], résolution 1080 [recomp.ini], rendu interne 50 % [recomp.ini]`.
* Un fichier passé par `--config` qui n'existe pas n'est pas créé (valeurs par défaut).
* Code : `runtime/os/settings.h/.cpp`. Les réglages sont lus une fois : un changement s'applique au
  lancement suivant.

### Performances, cadence et résolution (2026-11-08)

Résultat (écran titre, RTX 5070 Ti, écran hôte 2560×1440 à 165 Hz), intervalle moyen / pire cas :

| Configuration | img/s | ms par image |
|---|---|---|
| défaut (écran virtuel 1440p, rendu natif, 60 Hz) | 60,0 | 16,67 / 17-19 |
| `RECOMP_RESOLUTION=1080` | 60,0 | 16,67 / 20 |
| `RECOMP_RESOLUTION=2160` (4K, rendu interne 3840×2160) | 60,0 | 16,67 / 17,6 (60 s sans écart) |
| `RECOMP_RESOLUTION=2160 RECOMP_REFRESH=90` | 90,0 | 11,11 / 11,9 |
| `RECOMP_RESOLUTION=2160 RECOMP_REFRESH=120` | 120,0 | 8,33 / 9,2 |

Le GPU réel ne coûte que ≈ 0,4 ms par image ; la couche de validation reste muette en 4K.

**120 Hz en jeu (2026-11-09, 13e passe)** : sur l'écran titre le 120 img/s tient, mais pas en mission.
Essai `--refresh 120` en 1440p natif, de la copie de sauvegarde au début de Corneria
(`perf_120.log`, `RECOMP_PERF=1`) : 100 à 121 img/s dans les menus et le briefing, 55 à 118 img/s dans
Corneria, 41 au pire. Intervalle moyen 9 à 14 ms (pires cas 17 à 52 ms). La carte n'est pas en cause
seule : 6,7 ms de travail GPU réel par image (`RECOMP_GPU_PROFILE`, dont 2,2 ms pour la passe de
profondeur `shader_graph_default` sans fragments), proche du budget de 8,33 ms. Le côté processeur limite
avant elle : le thread GPU (traduction NVN → Vulkan) est occupé 3,6 à 7,6 ms par image, dont 2,8 à
6,7 ms dans `nvn_execute` (surtout `DrawElements`). L'utilisateur garde 60 Hz pour l'instant : à
optimiser plus tard (traduction des dessins, puis DLSS pour la part GPU).

Ce qui limitait à 50 img/s, dans l'ordre où c'est apparu :

1. **Traduction synchrone** : chaque `nvnQueueSubmitCommands` traduisait le travail en Vulkan sur le
   thread de rendu du jeu (≈ 3,5 ms). → `runtime/os/nvn_gpu.cpp` : **thread GPU** qui exécute les
   soumissions dans l'ordre ; **vrais `NVNsync`** (en attente dès la soumission d'un `FenceSync`,
   signalés quand le thread GPU l'atteint, après la fin du travail Vulkan) ; `nvnSyncWait`,
   `QueueFenceSync/WaitSync/Finish` réels.
2. **Coûts cachés du HLE** : `bind_handle` copiait le pool de textures entier (~800 descripteurs) à
   chaque poignée (1,8 ms/image) → `Table::with_key` (accès sous verrou sans copie) ;
   `mem_valid` appelait `VirtualQuery` (≈ 27 µs, 1,6 ms/image rien que pour `ReportCounter`, et les
   tâches de rendu du jeu le payaient aussi) → cache par thread invalidé à chaque désengagement.
   Bilan : thread GPU 4,1 → 0,8 ms/image, tâches de rendu du jeu 3 → 0,7 ms.
3. **Horloges GPU** : les compteurs `ReportCounter` portaient des ns depuis le démarrage de Windows,
   `nvnDeviceGetCurrentTimestampInNanoseconds` des ns depuis le lancement → même base (19,2 MHz).
4. **Régulateur du moteur** : sans `out.desired_frame_time` (défaut 0), il vise ≈ 18,2 ms (boucle
   ouverte : période + travail non prévu). La variable est injectée à 0,004 dans
   `veclipse_build.cvar` (romfs virtuelle, le `.nup` n'est pas touché) : le jeu produit au plus vite
   et c'est l'**écran virtuel** qui fixe le rythme, comme le vsync de la console :
   * l'image s'affiche au premier vblank virtuel libre (le GPU n'attend pas : seul l'affichage y
     est calé) ; `nn::vi::ListFrameHistory` rapporte ces instants (FrameInfo 0x28 o : état 2,
     numéro, 0, tick d'affichage, tick de fin) ;
   * `nvnWindowAcquireTexture` attend si 2 images sont déjà en attente d'affichage (contre-pression
     des 3 textures de fenêtre) ;
   * la swapchain hôte est en MAILBOX (`RECOMP_HOST_FIFO=1` pour FIFO).
5. **Résolution** : écran virtuel choisi d'après l'écran de l'hôte ; `render.resolution_scale(_console)
   = 1.0` et `render.dynres_enabled = false` injectés : rendu interne à la résolution de sortie (la
   console rend à 50 % puis suréchantillonne ; `RECOMP_NATIVE_RES=0` pour ce comportement).

Mécanisme générique : `build/full/romfs_override/<fichier>` remplace un fichier de la romfs.
Variables de rendu repérées dans `main` : `render.dlss_allowed`, `render.dlss_preset`,
`render.dynres_*`, `render.present_resolution_x/y`, `draw.scene_resolution_scale_override`…

## Limites connues / points de blocage

1. **Exceptions C++** : 3 144 fonctions de main ont une LSDA (personnalité importée du SDK).
   Si le jeu lève réellement des exceptions, il faudra un déroulement qui traverse le code
   recompilé (entrée sur les *landing pads* = entrées en milieu de fonction, déjà supportées,
   + abandon des cadres hôtes). À traiter avec le HLE de `__cxa_throw`/`_Unwind_*` (Phase 2/3).
2. **Retours non standards** : `ret` est traduit en `return` (l'appelant est supposé être la
   cible). Vérifié en mode banc par la pile d'ombre (`RECOMP_CHECK_RET`) ; `longjmp`/coroutines
   du SDK seront à traiter dans le HLE.
3. **Imports de données** (GLOB_DAT vers des objets du SDK) : pointent pour l'instant vers la
   zone des thunks ; le runtime HLE devra leur fournir un stockage réel.
4. **Instructions non gérées** : aucune dans main. Restent non implémentés (absents de main) :
   FP16 arithmétique, quelques saturants rares (`sqabs/sqneg`, `sqshl` immédiat, `sqdmull`…),
   crypto, SVE ; ils sont émis en `rt_unimpl` qui s'arrête en donnant l'adresse.
5. **Performance** : l'état CPU vit dans `ctx` (mémoire) ; chaque accès mémoire invité peut
   aliaser `ctx`, ce qui empêche gcc de garder les registres en registres. Étape suivante :
   registres invités en variables locales (écrites/relues aux appels), drapeaux paresseux.
6. **Couverture des tests de fonctions entières** limitée par les états aléatoires (beaucoup de
   fautes sur pointeurs) ; le fuzzing instruction par instruction couvre la sémantique.
7. **FPSR** (drapeaux d'exception cumulés) n'est pas mis à jour ; FPCR.AHP non géré.
8. **svc dans main** : main contient 4 `svc` réels (`svc 0x34` WaitForAddress, `svc 0x35`
   SignalToAddress). Fait en Phase 2 : le noyau émulé les gère comme ceux du SDK.

Points ouverts après la Phase 2 :

9. **Rendu (prochaine étape, Phase 4)** : NVN est remplacé au niveau API (fait en Phase 3), mais
   les commandes ne sont pas exécutées. Il faut : enregistrer les tampons de commandes, gérer les
   états, les textures (formats, mises en page « packaged »/bloc linéaire), les pools de
   textures/échantillonneurs, et **traduire les shaders précompilés** (binaires GPU NVIDIA du jeu)
   vers SPIR-V pour un moteur Vulkan. DLSS (subsdk2) : bouchons, le moteur reçoit une
   résolution 1×1 de `nvnDlssNetworkResize` (à rendre cohérent ou à désactiver).
10. **Exceptions C++ non gérées** à travers le code recompilé. Le déroulement (libunwind du SDK)
    parcourt correctement la pile *invitée*, mais l'installation du contexte final (saut vers le
    *landing pad*) devra être interceptée : lever une exception hôte attrapée par la fonction
    recompilée propriétaire du landing pad (repérée par sa LSDA), qui reprend à ce label.
11. **Instructions non gérées du SDK** (absentes de main, émises en `rt_unimpl`) : surtout des
    SIMD scalaires (6 672 mots `?scalar simd`, `sqshl` immédiat, FP16 vectoriel…) et 4 752 mots
    « undef » (données dans `.text`). Elles n'ont pas été atteintes jusqu'ici.
12. **Noyau simplifié** : un verrou global, priorités et cœurs mémorisés mais pas appliqués,
    `MapMemory` par copie, pas de vrai alias de mémoire partagée (une seule vue mappée).
13. **Services** : beaucoup de bouchons (« succès + zéros ») ; `GetInfo 32` inconnu ;
    `ldr:ro`, `ectx:aw`, plusieurs commandes nouvelles d'`am` restent journalisées.

Points ouverts après la Phase 3 :

14. **Résolu (2026-11-08)** — réveil perdu de `WaitProcessWideKeyAtomic`, voir « Au-delà de
    l'écran titre ». Diagnostic d'origine, pour mémoire :
    **chargement de `prologue_cin` bloqué dans Wwise** :
    `AK::AudioMgr` (thread 282) tient le verrou global Wwise (`main+0xf0f3b68`) et attend une
    place dans la file de tâches de type 3 (limite 1, compteurs à `main+0xf0f3c70`, limites à
    `+0xc90`) ; `AK::BankMgr` (thread 280) charge une banque et attend une lecture
    d'`AK::StreamMgr`, dont le thread d'E/S est inactif. Les 3 ouvriers Wwise dorment sur leur
    sémaphore (compteur 0). Sans banque chargée, le prologue ne se charge pas (« Async hint »
    absent) et l'appui sur A ne lance pas le tutoriel. C'est une **course** : déjà présente avant
    la réécriture des attentes du noyau (exécution de 18 h 49), franchie une seule fois (18 h 45).
    Pistes : ordonnancement par cœur et priorité (le noyau émulé exécute tout en parallèle),
    heuristiques temporelles de StreamMgr, crochet d'E/S bas niveau du jeu.
15. **Cadence** : résolu — 60 img/s stables (120 avec `RECOMP_REFRESH=120`), voir « Performances,
    cadence et résolution ». Sans géométrie à l'écran titre, la charge réelle d'une scène de jeu
    reste à mesurer.
16. **Blocage rare au démarrage** : vu ≈ 3 fois sur une cinquantaine de lancements le
    2026-11-08, toujours vers 0,95 s juste après la création du thread 260 (avant `vrender_create`),
    puis 0 fois sur 12 essais avec `--dump`. Non reproduit sous observation ; relancer suffit.
    Probablement le même réveil perdu que la limite 14 : plus vu depuis la correction.
17. **Applets** (`ShowControllerSupport`, clavier logiciel, erreurs) : non émulés ; la manette
    connectée évite le premier.

Points ouverts après la Phase 4 (rendu) :

18. **Textures** : les textures packagées sont désentrelacées et envoyées (2026-11-08) ;
    `CopyBufferToTexture` et `ClearTexture` sont exécutés (2026-11-09). Restent :
    * les textures non packagées écrites directement par le processeur (gris neutre) ;
    * le suivi des réécritures (envoi au premier usage seulement) ;
    * les textures 3D packagées ;
    * le pas de ligne des copies (`SetCopyRowStride`, données supposées serrées) ;
    * `EvaluateDlss` (à faire comme un blit) ; `CopyTextureToTexture` est fait (7e passe), copies
      profondeur ↔ couleur comprises (8e passe) ;
    * `CopyBufferToBuffer` : résolu à la 8e passe (copie GPU dans l'ordre des commandes).
19. **Résolu (2026-11-09, 7e passe)** — shaders de calcul à mémoire globale, traduits avec
    `GL_EXT_buffer_reference` et une table des pools en banque 0 (voir « 7e passe »).
20. **Étages non traduits** : 80 sur 435 depuis la 8e passe, aucun demandé en 10 min de jeu. Ce sont
    79 calculs à cœurs tensoriels, qui forment le réseau DLSS de la console, remplacé par notre HLE. Le
    dernier est un shader de fragments à mémoire globale.
21. **Résolu (2026-11-09, 6e passe)** : poignées de textures et d'images des shaders de calcul
    (banque 0), voir « Rendu corrigé ». La règle « images après la dernière texture lue » suppose
    que la dernière texture déclarée est lue ; à surveiller sur d'autres calculs.
22. **Résolu (2026-11-09)** — poignées séparées texture / échantillonneur : disposition de la
    banque 2 établie (voir « La cinématique s'affiche ») ; le matériau principal est traduit.
23. **Attributs de sommets entiers** (`R8G8B8A8_UINT`…) lus comme `vec4` flottants par les shaders
    traduits (erreur de validation `Input-08733`) : à typer selon le format au moment du pipeline.
24. **Cause exacte du TDR de la cinématique inconnue** : le garde-fou de l'aiguilleur l'évite
    (deux exécutions complètes), mais aucune boucle infinie n'a été mesurée et toute modification
    du shader `00f1ea3b370dd924` suffisait à faire disparaître la panne. Il s'agit probablement de
    la compilation de ces très gros shaders par le pilote (≈ 200 variables vivantes à travers
    l'aiguilleur `switch (pc)`). Une sortie structurée du flot de contrôle (boucles et `if` au
    lieu de l'aiguilleur) réduirait la pression sur les registres. Si un blocage réapparaît :
    `RECOMP_GPU_DIAG=1 RECOMP_GPU_SERIAL=1` désigne le dessin.
25. **Défauts visibles de la cinématique**. Corrigés à la 6e passe : motifs « tissés », image
    retournée, pièce noire, sphère bleue, fourrure noire. Corrigés à la 7e passe : anneaux en
    paliers (filtres NVN inversés) et voile gris (brouillard volumétrique non traduit). Corrigés à la
    8e passe : blocs noirs du flou de profondeur (vues de cible à niveau de mip ignoré).
28. **Résolu (2026-11-09, 8e passe)** : textures 1×1 et texte en pavés. `nvnQueueFinish` ne
    soumettait pas le travail Vulkan enregistré, et le jeu réutilisait sa mémoire de transfert trop
    tôt (voir « 8e passe »). Le contournement en gris neutre est retiré.
27. **Cadence de la cinématique : résolue** (8e passe), 60 img/s stables à 1440p. Les banques sont
    passées en tampons uniformes et la soumission est devenue asynchrone. Restent des à-coups aux
    changements de scène (textures envoyées au premier usage, pipelines créés de façon synchrone).
    Pistes :
    * préparation des textures en arrière-plan ;
    * cache de pipelines persistant ;
    * création des pipelines en arrière-plan ;
    * shaders moins coûteux (sortie structurée au lieu de l'aiguilleur, limite 24).
26. Le contrôle des pools (`RECOMP_POOL_AUDIT`) montre que le GPU lit bien la mémoire du jeu en
    place tant que le périphérique vit : l'import `VK_EXT_external_memory_host` est fiable,
    sans copie ni relecture.
29. **Résolu (9e passe)** : plantage GPU au début de l'entraînement (écriture à l'adresse 0). Une image de
    profondeur servait de cible couleur, parce que deux objets texture de même adresse partageaient une
    image Vulkan quel que soit leur format (voir « 9e passe »). Le tampon initialisé avec un pool nul n'y
    était pour rien.
30. **Tours holographiques de l'entraînement** (`s_tutorial_procedural_building01`) : résolu (13e passe).
    On ne voyait que leurs arêtes ; leur shader de sommets (`7f3471f3fcda1e01`) fait une rotation par
    sinus et cosinus, et ceux-ci étaient traduits en radians alors que le GPU les prend en tours.

### clang/LLVM nécessaire ?

Pas pour l'instant : gcc 16 (mingw) compile le code généré sans problème, gère
`[[gnu::musttail]]` (appels terminaux garantis, même en -O0) et `__atomic` 128 bits (`-mcx16`,
libatomic). clang deviendrait intéressant pour : (a) la vitesse de compilation des ~4 Go de C++
de main (à mesurer), (b) `[[clang::preserve_none]]`/conventions d'appel personnalisées pour
passer des registres invités en registres hôtes, (c) l'ABI MSVC si on lie avec des libs MSVC
(Vulkan/SDL fonctionnent aussi avec mingw). Recommandation : installer LLVM quand on attaque la
génération complète de main et comparer les temps.

### Rendu corrigé (2026-11-09, 7e passe : revue de toute la cinématique)

- **Filtres des échantillonneurs** : les énumérations NVN sont en base 0, ordre GL (min 0 NEAREST, 1 LINEAR,
  2 NEAREST_MIPMAP_NEAREST, 3 LINEAR_MIPMAP_NEAREST, 4 NEAREST_MIPMAP_LINEAR, 5 LINEAR_MIPMAP_LINEAR ; mag 0 NEAREST,
  1 LINEAR ; enveloppe 0 CLAMP, 1 REPEAT, 5 CLAMP_TO_BORDER, 6 MIRRORED_REPEAT, 7 CLAMP_TO_EDGE). Établi sur les
  11 échantillonneurs du jeu (`RECOMP_SAMP_LOG=1`). L’ancienne table inversait linéaire et plus proche : la LUT
  d’affichage 3D lue au plus proche donnait les anneaux elliptiques en paliers (6 marches → dégradé continu, vérifié
  sur l’image 3600). `RECOMP_OLD_FILTERS=1` rétablit l’ancienne table (diagnostic).
- **Présentation robuste** : swapchain recréée quand elle est périmée (fenêtre réduite ou redimensionnée) ; sans
  image d’écran, le travail de l’image est soumis quand même et les instantanés continuent (ils s’arrêtaient à 134 s).
- `RECOMP_DRAW_WATCH` affiche l’échantillonneur de chaque texture (`éch. id min/mag`).
- **Shaders absents** : 50 shaders des parties 2 à 5 et de l’entraînement manquaient (peau, cheveux, terrain de Venom,
  effets ; jusqu’à 887 dessins ignorés par image après la cinématique). Revidés sur 10 min de jeu
  (`RECOMP_DUMP_SHADERS=1`) puis traduits : **351 étages sur 435** (153 avant), plus aucun dessin ignoré.
- **Mémoire globale des calculs** (`LDG`, `STG`, `ATOMG`, `RED`) traduite avec `GL_EXT_buffer_reference`. Les adresses
  invitées sont des pointeurs hôtes des pools NVN ; elles viennent des descripteurs de tampons de la banque 0 ou
  des données du jeu (la lumière du brouillard lit des pointeurs rangés dans un tampon uniforme). Le runtime importe
  tous les pools NVN (`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`) et écrit
  en fin de banque 0 (0x600) une table : nombre de pools, puis base invitée, adresse Vulkan et taille. Chaque accès
  passe par `gxl()`, qui essaie d’abord le dernier pool trouvé. Hors des pools : lecture 0 et écriture ignorée, donc
  jamais de faute GPU. Accès 8/16 bits dans le mot de 32 bits (écritures par `atomicAnd`/`atomicOr`), atomiques
  32 bits (INC, DEC et addition flottante par comparaison-échange). `RECOMP_NO_GLOBAL_MEM=1` ignore ces calculs.
- **Banque 0 des calculs, disposition complète**, vérifiée par la taille totale (octet 0xba) sur 0098, b775, ff25 et
  la lumière du brouillard : 0x10, données en ligne (octet 0xa9, en mots, arrondies à 16 o), 16 o par tampon
  uniforme (0xb1), 16 o par tampon de stockage (0xb2 : adresse invitée, taille), 8 o par texture (0xb3), par
  texture séparée (0xb4), par échantillonneur séparé (0xb5), par image (0xb6). Ligne `c0` des `.meta` : 7 nombres.
  Les données en ligne viennent de `nvnCommandBufferPushComputeConstants`, désormais copiées à l’enregistrement
  (jamais appelé dans la cinématique : les étages concernés n’y sont pas exécutés).
- **Atomiques d’image** `0x593` `SUATOM` (avec retour) et `0x59f` `SURED`, poignée en banque constante :
  `imageAtomic*` sur une déclaration `r32ui` du même tableau. Établi sur `fog_noise_height`, qui accumule densité
  et couleur 10:10:10 dans une image 3D, puis sature par OR.
- **Brouillard volumétrique complet**, ce qui supprime le voile gris : `volumetric_fog_light` (mémoire globale) et
  `fog_noise_height` (atomiques d’image) s’exécutent. Sans eux, le volume intégré contenait n’importe quoi et
  ajoutait une brume grise croissante avec la distance, d’où un ciel spatial gris clair et des objets délavés.
- **Décalages de texture dynamiques** : `TLD4.AOFFI` par `textureGatherOffset` (6 bits signés aux bits 0 et 8),
  `TEX`/`TLD.AOFFI` (4 bits signés par composante) ajoutés aux coordonnées ; mémoire partagée 8/16 bits et `ATOMS`
  32 bits (min/max signés par comparaison-échange).
- `RECOMP_DISPATCH_LOG=1` : chaque programme de calcul exécuté (une fois), avec la disposition de sa banque 0.
- **`CopyTextureToTexture` exécuté** (ignoré jusqu'ici) : `vkCmdCopyImage` à taille égale, sinon `vkCmdBlitImage`
  (drapeaux NVN 1 filtre linéaire, 4 / 8 miroirs). Le jeu y recopie chaque carte d'ombre 1024×1024 dans l'atlas
  de profondeur 8192×4096 : sans la copie, les tuiles gardaient des ombres périmées, d'où des blocs sombres
  intermittents (planète holographique) et une salle de briefing sans ombres portées ni rais de lumière.
  `RECOMP_NO_TEX_COPY=1` rétablit l'ancien comportement (diagnostic).
- **Présentation pixel pour pixel** : une fenêtre à cadre de la taille de l'écran n'a qu'une zone utile réduite
  (2560×1421 sur un écran 1440p) ; l'image 2560×1440 y était réduite par filtrage linéaire, donc floue et
  légèrement écrasée. Nouveau réglage `window_mode` (`borderless` par défaut, ou `windowed`), `RECOMP_WINDOW`,
  `--window` ; Alt+Entrée ou F11 bascule en jeu. Copie au plus proche quand les tailles coïncident, format
  conservé (bandes noires) sinon ; swapchain recréée quand la fenêtre change de taille.
- **Restent non traduits (84 étages, jamais demandés en 10 min de jeu)** : 78 calculs à cœurs tensoriels (`LDSM`
  `0x83b`, `HMMA` `0x23c`/`0x23a`/`0x237`/`0x240`), opcodes `0x8a5`, `0x3a1`, `0x3c4`, `0x2a5`, et un shader de
  fragments à mémoire globale (adresse en banque 0 graphique, 0x5b0). Voir la 8e passe : 80 restants.

### Corrections et cadence (2026-11-09, 8e passe)

**Défauts restants corrigés**

- **Flou de profondeur (blocs noirs)** : les vues de cible de rendu ignoraient leur niveau de mip et leur couche.
  Les passes qui construisent la pyramide de profondeur (SAO, flou) écrivaient toutes le niveau 0. Le niveau et la
  couche de chaque vue sont maintenant relevés à l'enregistrement de `SetRenderTargets`, car le jeu réutilise ses
  objets vue. `render_view()` crée la vue Vulkan correspondante, et le dessin prend la taille de ce niveau.
- **Copies profondeur ↔ couleur** (`CopyTextureToTexture`) : passage par un tampon intermédiaire
  (`vkCmdCopyImageToBuffer` puis `vkCmdCopyBufferToImage`), car Vulkan interdit `vkCmdCopyImage` entre ces
  aspects.
- **`CopyBufferToBuffer` dans l'ordre du GPU** : la copie est faite sur le GPU, entre les pools importés.
  La source est photographiée à l'enregistrement, jusqu'à 4 Mo. Le processeur ne fait plus la copie au moment de
  la soumission, avant le travail qui la précède. `RECOMP_CPU_BUFFER_COPY=1` rétablit l'ancien comportement.
- **Shaders : 355 étages sur 435.** Nouveaux décodages :
  - `UIMAD.WIDE` (`0x2a5`, `0x8a5`), `UFLO` / `UPOPC` / `UBREV` uniformes (`0x0bd`, `0x0bf`, `0x0be`) ;
  - `REDUX` (`0x3c4`, réduction de warp vers un registre uniforme : `subgroup*`), opération aux bits 78-80 ;
  - `0x3a1`, lu comme `ATOMS.POPC.INC`.

  Les 80 restants ne servent pas :
  - 79 calculs à cœurs tensoriels (`LDSM`, `HMMA`). Ce sont les couches du réseau DLSS de la console (API
    `nvnDlssNetwork*`). Notre HLE remplace DLSS par un rendu à la résolution native, donc ils ne sont jamais lancés.
  - un shader de fragments à mémoire globale, inutilisé.
- **Textures 1×1 et texte en pavés : cause trouvée** (ancienne limite 28). `nvnQueueFinish` ne soumettait pas le
  travail Vulkan déjà enregistré. Le jeu, croyant le GPU à jour, réutilisait sa mémoire de transfert, et les
  copies des textures 1×1 par défaut lisaient d'autres données. Le correctif :
  - `nvnQueueFinish` soumet maintenant et attend ;
  - les sources de `CopyBufferToTexture` sont photographiées à l'enregistrement (4 Mo au plus).

  Le contournement est retiré : le vrai contenu est copié et le texte est correct. `RECOMP_FINISH_NO_FLUSH=1`,
  `RECOMP_COPY_IN_PLACE=1` et `RECOMP_SKIP_COPY_TEX_1X1=1` reproduisent l'ancien comportement.

**Cadence : de 42-53 img/s à 60 img/s stables (cinématique, écran 1440p, RTX 5070 Ti)**

`RECOMP_GPU_PROFILE=N` mesure, par horodatages, chaque dessin et calcul de l'image N. Au départ, l'image 2700
coûtait **18,43 ms de GPU**. Le matériau principal en prenait 5,84 ms (29 dessins), la profondeur seule 2,99 ms
(392 dessins), la fourrure (`fur_vertex_color`) 2,6 ms. Le rendu était donc limité par le coût des shaders, puis
par l'attente du GPU sur le thread de traduction.

1. **Banques de constantes en tampons uniformes** (`glsl.py`, `create_layouts`).
   - **Avant** : chaque `c[banque][déplacement]` lu par un shader était un chargement depuis un tampon de
     stockage. Sur la console, c'est un opérande direct de l'instruction.
   - **Maintenant** : chaque banque est un bloc `uniform` de 64 Ko en disposition `scalar` (`uint d[16384]`).
     Le pilote NVIDIA lit ces blocs par le cache de constantes, et un déplacement connu redevient un opérande
     direct.
   - **Résultat** :
     - image 2700 : 18,43 → **9,7-10,6 ms** de GPU ;
     - matériau principal : 5,84 → 1,19 ms ;
     - profondeur seule : 2,99 → 1,48 ms ;
     - pré-passe : 1,55 → 0,36 ms.
   - **Contraintes** : les tampons du jeu sont liés à 64 Ko au plus (`maxUniformBufferRange`). Une adresse non
     alignée sur `minUniformBufferOffsetAlignment` serait recopiée dans l'anneau ; ce cas n'a pas été observé,
     NVN aligne sur 256. La liaison 31 reste un tampon de stockage pour `RECOMP_DBG_SSBO`.
2. **Soumission asynchrone** (`vk_render.cpp`, `Vk::Slot`).
   - **Avant** : chaque `FenceSync` soumettait le travail puis attendait la fin du GPU. La traduction et
     l'exécution GPU s'additionnaient : 2 à 3 ms + 12,6 ms par image, avec des images manquées (33-35 ms).
   - **Les emplacements** : chaque enregistrement prend l'un de 6 emplacements tournants, qui contient un tampon
     de commandes, ses pools de descripteurs (chaîne allongée si besoin), son anneau de constantes de 16 Mo et
     ses tampons intermédiaires.
   - **Synchronisation** : un emplacement est réutilisé quand un **sémaphore chronologique** montre que sa
     soumission précédente est finie. Une barrière complète en tête de chaque tampon garde l'ordre d'exécution
     d'avant sur le GPU.
   - **Fin d'une soumission** : un fil de complétion signale les `NVNsync` du jeu quand le GPU a réellement fini
     (`vkr::flush_async` + `vkr::when_done`).
   - **Présentation** : elle n'attend plus le GPU. L'acquisition utilise un sémaphore par emplacement et la fin
     de copie un sémaphore par image d'écran.
   - **Cas restés synchrones** : `nvnQueueFinish` ; les diagnostics qui relisent le GPU (captures, profileur,
     `RECOMP_DBG_SSBO`) ; `RECOMP_SYNC_FROM`, `RECOMP_GPU_DIAG`, `RECOMP_DRAW_WATCH` ; et
     `RECOMP_SYNC_SUBMIT=1`, qui rétablit l'ancien comportement.
   - **Résultat** : le thread GPU passe de 14-15 ms à **2,3-2,9 ms par image**. Dans la scène lourde,
     l'intervalle moyen est de 16,67 ms, avec des pointes à 18-27 ms au lieu de 33-35 ms. Le rendu est
     inchangé sur les instantanés vérifiés (images 2100, 2701, 3300, 3900 et 4500).
3. **Désentrelacement des grosses textures sur plusieurs cœurs** (bandes de ≈ 1 Mo, 8 fils au plus). Il
   sert aux changements de scène, où des centaines de Mo de textures sont envoyées au premier usage. Une
   pointe est passée de 205 à 146 ms sur un essai.

**Ce qui reste**

- **À-coups aux changements de scène** (60 à 800 ms, chargement du jeu compris). Ils viennent des textures
  envoyées au premier usage (création, désentrelacement) et des pipelines créés de façon synchrone. Pistes :
  - préparer les textures dès `nvnTexturePoolRegisterTexture`, en arrière-plan ;
  - utiliser un cache de pipelines.
- **Fourrure** : environ 1,6 ms pour un seul dessin de pré-passe, à cause du remplissage (couches superposées
  avec test alpha). C'est le coût du rendu à la résolution native : la console rend à environ 50 % puis
  suréchantillonne avec DLSS.
  - Avec une marge de 6 ms à 1440p, le 60 img/s tient.
  - En 4K natif (`RECOMP_RESOLUTION=2160`, mesuré sur la cinématique), il tient aussi. L'image 2700 coûte
    **13,0 ms de GPU**, dont 2,91 ms pour le matériau principal. L'intervalle moyen est de 16,67 ms, avec des
    pointes à 17-20 ms (quelques-unes à 31 ms) hors changements de scène.
  - Pour aller au-delà (4K à 120 Hz, scènes de jeu plus lourdes) : le vrai DLSS (après la V1 : `EvaluateDlss`
    et SDK NVIDIA) ou `native_resolution = 0`.
- **Flot de contrôle structuré** (limite 24) : les shaders gardent l'aiguilleur `while (true) switch (pc)` ;
  257 des 355 sont sans boucle et pourraient être structurés. C'est le prochain gain GPU, mais il n'est plus
  nécessaire au 60 img/s à 1440p.

### Son (2026-11-09)

Le jeu (Wwise) écrit son mixage final dans un seul flux `nn::audio::OpenOutputStream`.
- **Format** : `StreamFormat` = {format d'échantillon, canaux, fréquence, réservé}, soit 5, 2, 48000, 0 :
  flottant 32 bits, stéréo, 48 kHz. L'ancien code lisait ces champs dans le désordre.
- **Écriture** : le jeu écrit par blocs de 512 trames.
- **Confirmation** : le journal de Lichii décrit le même flux (`OutputStream-0=>2ch48kF32`).

**Fonctionnement**
- `hle_audio.cpp` donne à chaque flux un tampon de 100 ms (stéréo flottant). Il convertit le format du jeu
  (8, 16 ou 32 bits, flottant ; mono dupliqué, 5.1 replié) et applique le gain de `SetOutputStreamGain`,
  lu dans `s0`.
- `audio_out.cpp` ouvre la sortie Windows par défaut en WASAPI partagé, piloté par événement, avec un tampon
  de 40 ms. Le format est 48 kHz stéréo flottant, converti par Windows vers celui de la carte
  (`AUTOCONVERTPCM`).
- Le fil de rendu appelle le mixeur du HLE autant de fois que la carte demande de trames. **L'horloge de la
  carte son rythme donc la consommation des flux du jeu**, sans dérive entre deux horloges. L'événement du
  flux est signalé dès qu'il y a de la place.
- Sans périphérique, ou s'il disparaît, l'horloge virtuelle d'avant prend le relais (le jeu n'attend jamais).
  La sortie est rouverte toutes les secondes.

**Mesures sur 70 s** (écran titre puis cinématique ; carte : 48 kHz, 8 canaux) :
- 512 trames par écriture, consommation exacte de 48 000 trames/s, **0 manque**, rien de saturé ;
- crêtes de 0,02 à 0,31 selon les scènes, du même ordre que dans Lichii (0,17 à 0,19) ;
- les sauts brusques relevés sont des attaques de sons du jeu, pas des raccords de tampons.

`RECOMP_AUDIO_WAV` enregistre le mixage, ce qui permet de vérifier sans haut-parleurs.

**Opus** : le jeu crée 16 décodeurs « matériels », mono, 48 kHz, trames de 20 ms. Aucun paquet n'y arrive en
70 s. Ce profil est celui de la voix en ligne (GameChat, neutralisé) : la cinématique n'en dépend pas. Ces
décodeurs rendent donc toujours du silence. Les paquets sont journalisés (`RECOMP_AUDIO_LOG`) au cas où un
autre passage du jeu s'en servirait ; il faudrait alors libopus, absent de la machine.

**Limites** : latence d'environ 100 à 140 ms (tampon du flux plus tampon de Windows). Un changement de
périphérique par défaut en cours de partie n'est pas suivi, sauf si l'ancien disparaît.

### Vérifier avec Lichii (émulateur de référence)

La copie officielle de Lichii (base Eden) lance ce jeu. Sa CLI, `eden-cli.exe`, sert de référence.

**Lancement**
```bash
EDEN_NX2_HIDE_WINDOW=1 EDEN_NX2_KEY_SCRIPT="10:4,10.15:-4,12:4,12.15:-4" \
  eden-cli.exe --filter "*:Info Service.LM:Trace" -c copie-de-sdl2-config.ini -g "<jeu>.nup"
```
- `KEY_SCRIPT` : secondes:code SDL, négatif = relâcher ; la touche A est le bouton A.
- Configuration : une copie de `%APPDATA%\eden\config\sdl2-config.ini` avec `audio_muted=true`, pour que
  l'essai ne joue pas de son.
- Le filtre `Service.LM` fait apparaître le journal du jeu (« Log Text: ») dans
  `%APPDATA%\eden\log\eden_log.txt`. Ce sont les mêmes lignes que nos `[lm]` : on compare ainsi la
  progression (chargements de paquets) sans image.
- Avec la fenêtre cachée, les captures d'image (`EDEN_NX2_FRAME_CAPTURE`, `EDEN_NX2_FRAME_DUMP`) ne se
  déclenchent pas : elles demandent une fenêtre affichée.

**Résultat du 2026-11-09**
- Même séquence de démarrage que la recomp (romfs, `nvn`, `bootstrap`, `global`).
- Lichii est en mode `NORMAL` (portable) avec écran HDR ; nous sommes en `BOOST` (salon) et SDR.
- Lichii a une sauvegarde : le jeu y va au menu (`mp_rewards`, `post_venom_ending_flow`). Sans sauvegarde,
  la recomp lance le prologue.
- Pour l'audio, le journal de Lichii donne une crête toutes les 2 s : niveau de l'écran titre 0,015-0,020,
  contre 0,020-0,023 chez nous.

### Comparaison avec Lichii et corrections (2026-11-09, 9e passe)

Les captures de Lichii (fenêtre affichée, `EDEN_NX2_FRAME_CAPTURE`) servent de référence de rendu. Lichii a
lui-même des défauts d'affichage en bas de l'écran, sans rapport avec le rendu : on les ignore.
`compare_frames.py` (scratchpad) associe chaque capture à l'instantané de la recomp le plus ressemblant
(corrélation de vignettes) et les met côte à côte.

**Corrections**

- **Horloge invitée à 31,25 MHz** (fréquence du compteur de la Switch 2 ; 19,2 MHz est celle de la Switch 1).
  Avec 19,2 MHz, le jeu ne simulait que 61 % du temps réel : la cinématique durait 1,63 fois trop longtemps.
  Elle arrive maintenant à l'entraînement à 228 s, contre 232 s dans Lichii. `RECOMP_CNTFRQ` change la valeur.
- **Manette lue seulement quand la fenêtre a le focus.** Une partie jouée ailleurs avec la même manette
  pilotait aussi le jeu : 17 appuis sur Start avaient mis la cinématique en pause pendant un test.
  `RECOMP_BACKGROUND_INPUT=1` rétablit l'ancien comportement.
- **Textures packagées périmées** (voile de bruit sur le décor de l'entraînement). Une
  nouvelle texture packagée créée à l'adresse et à la taille d'une ancienne réutilisait l'image Vulkan de
  l'ancienne, avec son contenu. Maintenant, une empreinte de 64 bits du contenu envoyé est comparée à la
  mémoire : contenu identique, l'image est réutilisée ; contenu différent, il est renvoyé dans la même image.
  Sur la cinématique complète : 675 textures et 2,6 Go envoyés, au lieu de 3 047 textures et 9,7 Go sans
  réutilisation.
- **Damier sur le vaisseau (occlusion ambiante)**. Dans l'entraînement, l'aile bleue de l'Arwing était sombre
  et couverte de cases d'environ 16 pixels ; chez Lichii elle est d'un bleu uniforme. Le damier était déjà
  présent à la sortie de la passe opaque (`RECOMP_RT_SNAPSHOT`). Avec `RECOMP_SKIP=ssao`, il disparaissait :
  le coupable était l'occlusion ambiante (SAO : pyramide de profondeurs, calcul au tiers de la résolution,
  deux flous, agrandissement).
  - **Cause** : les instructions de mémoire partagée `LDS` / `STS` des architectures récentes adressent
    `[Ra.Xn + URb + imm]`. L'échelle n vaut 1, 4, 8 ou 16 (bits 78-79) et multiplie le registre. Le
    traducteur l'ignorait. Les deux flous de la SAO rangent une paire de flottants par fil en `[R28.X8]` :
    les 256 fils écrivaient les uns sur les autres et lisaient les valeurs de leurs voisins.
  - **Correction** : échelle décodée dans `sass.py`. `glsl.py` (`saddr`) multiplie le registre et ajoute le
    registre uniforme de la forme `0x988`. 11 shaders de calcul changent de sens, dont les deux flous de la
    SAO ; les 9 autres ne sont pas lancés pendant la cinématique.
- **GPU perdu au début de l'entraînement (ancienne limite 29)**. Le jeu réutilise sa mémoire pour des
  ressources de formats différents. Le portrait de la fenêtre de dialogue (cibles de 306×265) est rendu dans
  une mémoire qui avait servi à d'autres images. Or deux objets texture de même adresse et de même taille
  partageaient toujours la même image Vulkan, dans le format de la première. Une image de profondeur
  (D32F+S8) servait ainsi de cible couleur RG16, et une texture RGBA16 de tampon de profondeur : usage
  invalide, écriture à l'adresse 0 et perte du GPU environ 8 s après le début de l'entraînement jouable.
  - **Correction** : `by_addr` garde toutes les images créées à une adresse. Un nouvel objet texture partage
    une image seulement si la forme et la **disposition en mémoire** sont les mêmes (même format, ou variantes
    linéaire / sRGB : `same_layout`). Une texture packagée posée sur la mémoire d'une ancienne cible de
    rendu a aussi sa propre image, avec son contenu envoyé. Sinon, nouvelle image, réutilisée ensuite pour
    cette adresse et ce format.
  - **Résultat** : l'entraînement se joue (anneaux, « Beau travail, Fox », leçon suivante) sans perte du
    GPU. La même faute servait aussi une texture BC1 dans une image BC7 d'une scène précédente.

**Vérifications**
- Plan de la capture de l'utilisateur (Arwing de profil au-dessus de la grille, image 13 530 avec
  `RECOMP_AUTOPRESS=1`) : l'aile est d'un bleu uniforme, comme chez Lichii.
- Cinématique complète avec les deux corrections : superposable aux captures de Lichii (Peppy, Pigma,
  cockpits, approche de Venom), sans perte du GPU.
- Entraînement jouable pendant 140 s sans perte du GPU (avant : perte environ 8 s après son début).

**Ce qui reste**
- Tours holographiques de l'entraînement (limite 30).
- Les 9 autres shaders de calcul dont l'adressage partagé a changé ne sont pas lancés pendant la cinématique :
  leur effet sera visible plus loin dans le jeu, sans vérification à ce jour.

### Tous les shaders du jeu, sans jouer (2026-11-09, 10e passe)

Après une première vraie partie de 10 minutes, 50 shaders manquaient (53 effets invisibles : réacteurs, traînées,
boost, tirs chargés, bouclier, chars de Venom, hologrammes, écrans du cockpit…). Les vidages faits en jeu ne
couvraient que les passages déjà atteints. Les shaders sont donc maintenant extraits directement de la romfs.

**Où sont les shaders**
- `romfs/data.zip` (15,6 Go dans le contenu patch) est une archive **ZIP64** précédée d'un en-tête `VZIP`.
  - 114 154 entrées, dont les noms sont des empreintes hexadécimales de 64 bits.
  - Méthode déclarée 0, mais les données sont des **blocs LZ4 chaînés de 64 Ko** : u32 taille compressée (bit
    31 : bloc stocké), puis le bloc, qui peut référencer les 64 Ko décompressés précédents.
- **2 828 paquets de shaders** (signature `0x19866891`, 40 Mo au total). Un paquet contient :
  - un en-tête : u32 signature, u32 taille du code, u32 taille de la section de contrôle, u32 0, u64 empreinte ;
  - puis le code, avec la banque constante 1 entre `ctl[0x40]` et `ctl[0x44]` ;
  - puis la section de contrôle NVN (`0x98761234`).
- L'empreinte `sha1(code[:ctl[0x38]])` est celle du runtime : 795 paquets retrouvent un shader déjà vidé en jeu.
- Le fichier `VIPR` de 18,7 Mo contient 165 autres sections de contrôle, dans un autre format (sans le code à
  côté) ; probablement les programmes du lecteur vidéo, jamais demandés jusqu'ici.

**Outil** : `python -I shaders/romfs_shaders.py` lit le `.nup` en lecture seule : l'index de `data.zip`, le
premier bloc de chaque entrée (≈ 2,2 Go), puis les paquets. Il écrit `h<empreinte>_r.ctl/.bin/.cb1` dans
`build/full/shaders/`. Ensuite, `python shaders/shaderc.py -j 12` (environ 40 s).

**Résultat : 2 388 étages traduits sur 2 468** (355 avant). Les 80 restants ne servent pas : les 79 calculs à
cœurs tensoriels du réseau DLSS de la console, et un shader de fragments à mémoire globale.

Instructions ajoutées pour les nouveaux shaders :
- `0xf63` : `TLD4` (lecture groupée) à poignée en registres uniformes, comme `0xf60` / `0xf6c` ;
- `0xabb` : `ULDC` à indice uniforme (`ULDC.64 UR4, c[0x6][UR4+0x10]`, table de décalages d'un flou) ;
- `PIXLD` : nombre d'échantillons et masque de couverture (nos cibles ont un seul échantillon).

**Filet de sécurité** : un shader qui ne serait ni traduit ni déjà vidé est maintenant écrit automatiquement
dans `build/full/shaders/` pendant la partie (« shader inconnu » au journal) ; il suffit alors de relancer
`shaderc.py`.

**Vérification** : écran titre, menu principal, nouvelle campagne, Great Fox et appel du général Pepper (scènes
jamais vues avant) : 0 shader absent, 0 dessin ignoré.

**Sauvegarde** : cet essai a navigué dans le menu et lancé une campagne qui a été enregistrée dans la
sauvegarde du joueur. `RECOMP_SAVE_DIR=dossier` sépare désormais la sauvegarde des essais.

### Modèles 3D faux après un changement de niveau (2026-11-09, 11e passe)

Signalé par l'utilisateur dans Météo (champ d'astéroïdes).

- **Cause** : chaque pool mémoire NVN est rendu visible au GPU par un import Vulkan de la mémoire hôte
  (`VK_EXT_external_memory_host`), gardé ensuite pour toujours dans `g.pools`. Or un import montre au GPU les
  pages physiques présentes au moment de l'import. Au changement de niveau, le jeu libère ses pools, démappe
  leur mémoire (`UnmapPhysicalMemory`), puis remappe les mêmes adresses pour les pools du nouveau niveau.
  `find_pool` retrouvait l'ancien import couvrant ces adresses : le GPU lisait les sommets, les index et les
  constantes du nouveau niveau dans les anciennes pages.
- **Preuve dans le journal de la partie** : de 470 à 479 s (chargement de Météo), 9 plages remappées étaient
  encore couvertes par des imports faits à 393 s, sans nouvel import. Le parcours complet de la cinématique
  jusqu'à l'entraînement le reproduit aussi (2 plages à 228 s).
- **Correction** : `nvnMemoryPoolFinalize` passe par la file GPU (élément `PoolReleased`, après les commandes
  déjà soumises) et appelle `vkr::pools_released`. Les imports qui couvrent la mémoire du pool sont retirés
  de `g.pools` ; un nouvel usage de ces adresses fait un nouvel import. Les objets Vulkan retirés sont détruits
  dans `begin_frame`, une fois que le GPU a passé la soumission qui pouvait encore les lire.
- `RECOMP_POOL_AUDIT` ne lit plus la mémoire démappée côté processeur : il compte ces échantillons à part
  (« démappés »), signe d'un import périmé.
- **Vérifié** sur la partie suivante de l'utilisateur (11 min, jusqu'au Secteur X) : 19 libérations de pools,
  aucune plage remappée lue à travers un import périmé (script `remap_check.py` sur le journal), plus de
  corruption progressive. Seul défaut visuel restant signalé : le tir chargé dans le Secteur X.

### Textures jamais libérées (2026-11-09, 12e passe)

Après le correctif des imports, l'utilisateur voyait encore les « shaders bugger petit à petit » : icônes de
notation et des vies devenues des carrés pleins, vignettes de missions brouillées, fonds du HUD opaques, carrés
à côté des modèles avec le tir chargé ou le passage des anneaux.

- **Cause** : les images Vulkan n'étaient jamais détruites. Sur une partie de 30 minutes : 6 170 images et
  15 Go de textures envoyées, pour une carte de 16 Go. Passé le budget, les créations d'images échouaient
  sans message (`vkAllocateMemory` en échec : `get_image` rend `nullptr`). Les dessins prenaient alors un
  descripteur factice : un effet ou une icône à texture transparente devenait un carré plein.
- **Correction** : `pools_released` détruit aussi les images dont la mémoire est dans le pool libéré. Elles sont
  retirées de `g.images`, `g.alias` et `g.by_addr`, et les objets Vulkan sont détruits dans `begin_frame` quand
  le GPU a passé la soumission notée. La passe de rendu ouverte est fermée, et les caches de descripteurs
  (`tex_written`, `img_written`) sont vidés : une poignée de vue détruite peut être réattribuée.
- **Journal** : la ligne périodique donne maintenant le nombre d'images libérées et la mémoire vidéo utilisée
  par rapport au budget du pilote (`VK_EXT_memory_budget`).
- **Échecs d'allocation visibles** : un refus de `vkAllocateMemory` pour une image est maintenant journalisé
  (« mémoire vidéo refusée », avec la mémoire utilisée et le budget), au lieu de laisser la texture manquer
  sans trace.
- **Catalogue des programmes** : `build/full/shaders/programmes.txt` reçoit une ligne par programme du jeu
  (nom de l'effet, puis `étage:empreinte` de chaque shader), au moment de sa création. Les shaders extraits de
  la romfs n'ayant pas de nom, ce fichier relie un effet signalé (ex. `vfx_charged_shot_…`) à ses shaders.
- **Objets texture détruits** : la libération par pool ne suffit pas si le jeu sous-alloue ses textures dans
  des pools qu'il garde. Chaque image compte maintenant ses objets texture vivants (`refs` : celui qui l'a
  créée, plus ceux qui la partagent). `nvnTextureFinalize` passe par la file GPU (`TextureReleased`) et retire
  une référence. Sans référence, l'image reste réemployable par adresse (textures recréées sur le même
  contenu, envoyées une seule fois) dans un cache plafonné à 1,5 Go. Au-delà, les plus anciennes sont
  détruites, à condition qu'aucun dessin ne les ait lues depuis 300 images (5 s) : le jeu copie parfois ses
  objets texture, et une copie peut survivre à l'original. La ligne périodique du journal donne la taille de
  ce cache (« Mo sans objet »).
- **Vérifié** sur la cinématique d'ouverture puis l'entraînement (14 880 images, sauvegarde d'essai) :
  mémoire vidéo bornée à environ 2,6 Go (l'exécutable précédent montait à 3,9 Go au même point, et sans
  limite ensuite), 257 images détruites, cache sans objet sous 1,5 Go, aucun refus d'allocation ni perte du
  périphérique. Les 17 captures appariées à celles de Lichii sont identiques au contenu près (HUD, portraits,
  manette, ville holographique) : aucune texture encore utilisée n'a été évincée.

### Sinus et cosinus en tours (2026-11-09, 13e passe)

L'utilisateur voyait « un petit bug de shader sur tous les hologrammes en forme de cercle » (sphères
holographiques des planètes sur la carte, au rendu rosé et sans dégradé).

- **Cause** : depuis Volta (plus de `RRO`), `MUFU.SIN` et `MUFU.COS` prennent l'angle en **tours** : le
  compilateur multiplie d'abord l'angle par 1/(2π) (`FMUL.RZ … 0.15915494`, `0x3e22f983`), et le matériel
  calcule sin(2π·x) sur la partie fractionnaire. `glsl.py` traduisait `sin(x)` : chaque angle était donc
  divisé par 2π. Sur les 2 101 sinus et cosinus des shaders traduits, 2 096 suivent directement la
  multiplication par 1/(2π) (et 5 par −1/(2π)) : la convention ne fait aucun doute.
- **Effets touchés** : 460 shaders, 71 effets nommés dans `shaders/programmes.txt`. Sur la sphère
  holographique (`starmap_planet_holo_sphere`), le dégradé en latitude cos(π·|v − ½|) restait vers 1 au lieu
  de s'éteindre aux pôles. Sont aussi touchés tous les `holo_advanced*` (transmissions comprises), les
  anneaux, sélections et trajets de la carte, la sphère d'impact du tir chargé, la spirale d'échantillons
  du SSAO, et la rotation du shader de sommets des bâtiments holographiques de l'entraînement.
- **Correction** : `sin(fract(x) * 2π)` et `cos(fract(x) * 2π)`. Retraduction complète : seuls les 460
  fichiers à sinus ou cosinus changent, et seulement sur ces lignes. L'ancien cache est gardé dans
  `build/full/shader_cache_avant_sincos`.
- **Retraduire pendant que l'utilisateur joue** : `shaderc.py --out build/full/shader_cache_new`, en priorité
  basse (classe IDLE héritée par glslang), puis échanger les dossiers une fois le jeu fermé.
- **Vérifié** (`verif_carte.log`, `verif_sincos.log`, sauvegardes d'essai) : briefing de la campagne
  correct (globe holographique de Corneria avec son halo, planètes du système Lylat). Cinématique et
  entraînement comparés à Lichii : 24 paires retenues au lieu de 17. Les plans de l'entraînement
  (Lichii 5521 à 8521) s'apparient maintenant à 0,87-0,98, et les **tours holographiques** y sont des
  volumes translucides comme dans Lichii : la limite 30 venait de là. Aucune perte du périphérique,
  aucun shader absent.

## Launcher (2026-11-09)

`lanceur.exe` (à la racine du projet depuis la 1.0.1, à côté de `recomp/`, comme dans le paquet ; sources dans
`recomp/launcher/`) : fenêtre de lancement dans l'esprit de celle du portage natif de Star Fox
Command (SerenityG4K), en noir et vert néon. Il construit le jeu sur le PC du joueur, à partir de **sa** copie
(`.nup`), règle `recomp.ini` et lance le jeu.

- **Technique** : Win32 sans dépendance, comme la référence. Le rendu passe par Direct2D 1.1 sur une chaîne
  d'échange DXGI, les textes par DirectWrite (police Bahnschrift, fournie avec Windows) et la vidéo par Media
  Foundation. Pas de DLL à fournir ; l'exécutable fait 2,1 Mo. Compilation : `python launcher/construire.py`
  (`--out autre.exe` si `lanceur.exe` est ouvert).
- **Écran** : titre lumineux, gros bouton « RECOMPILER LE JEU », qui devient « JOUER » quand le jeu est prêt
  et affiche la progression pendant la construction. Menu Réglages, Contrôles, Construction et Quitter, état
  de la copie, bandeau défilant, grille en perspective, lignes de balayage, choix FR / EN.
- **Fond** : extraits des cinématiques, étalonnés en vert (matrice de couleur Direct2D), avec un zoom lent et
  des fondus au noir entre deux extraits. Sans extraits, un globe holographique tourne à la place.
- **Extraits** : enregistrés par le jeu recompilé lui-même (`RECOMP_RECORD_DIR`, `runtime/os/record.cpp`)
  pendant la cinématique d'ouverture : 6 extraits MP4 de 9 s en 1280×560, sans bandes ni sous-titres, en
  105 s. Ils viennent de la copie du joueur et restent sur son PC, dans `launcher/extraits`, jamais
  distribués.
- **Réglages** (`recomp.ini` du jeu) : langue du jeu (15 choix, noms dans leur langue), résolution, images
  par seconde, rendu natif ou comme la console, plein écran ou fenêtré. Le volume (`RECOMP_VOLUME`) est passé au lancement. On choisit aussi le fond
  (extraits ou sobre) et le nombre de fils de compilation. Deux boutons permettent de reconstruire le jeu ou
  de réenregistrer les extraits. Les réglages propres au launcher sont dans `lanceur.ini` (UTF-16).
- **Contrôles** : correspondance boutons Switch → clavier (noms des touches donnés par Windows, donc AZERTY
  ou QWERTY) → manette Xbox. Depuis la 1.0.3, les touches du clavier se changent sur cette page (section « Touches du
  clavier modifiables depuis le launcher » plus bas).
- **Construction** : `launcher/preparer.py`, lancé en priorité basse dans un objet tâche (arrêt de toute
  l'arborescence sur « Annuler » ou à la fermeture). Étapes :
  1. vérifier la copie (version 2.0.0, par les empreintes SHA-256 de l'index du `.nup`, sans relire 30 Go) ;
  2. extraire les exécutables ;
  3. préparer les images des modules ;
  4. recompiler (`fullbuild.py`) ;
  5. faire l'édition de liens, icône du jeu comprise ;
  6. traduire les shaders (romfs puis SPIR-V) ;
  7. enregistrer les extraits.

  Chaque étape déjà faite pour cette copie est sautée. Le launcher lit les lignes `@etape`, `@progres`,
  `@info`, `@ok`, `@saute` et `@erreur`, et écrit le journal complet dans `build/full/preparation.log`.
- **Copie hors du projet** : `fullbuild.py` lit `RECOMP_NUP` (passé par `preparer.py`) pour le manifeste ;
  `shaderc.py` trouve glslang par `RECOMP_GLSLANG`, puis le `PATH`, puis MSYS2.
- **Vérification sans écran** : `LANCEUR_CAPTURE=fichier.png` (`LANCEUR_CAPTURE_T=s`,
  `LANCEUR_CAPTURE_QUITTER=1`) écrit l'image affichée. `LANCEUR_PAGE=reglages|controles|construction` ouvre
  une page. `LANCEUR_ACTION=construire` lance une construction avec le pilote de `LANCEUR_PREPARER`
  (jamais le jeu : un essai qui appuyait sur « JOUER » avait ouvert le vrai jeu sur la sauvegarde du joueur).
- **Journal du jeu** : avant chaque partie, le launcher renomme `recomp_run.log` en `recomp_run_precedent.log`.
- **Icône du jeu** (2026-11-10) : `recomp_main.exe` porte l'icône officielle, visible dans l'Explorateur, la
  barre des tâches, le Gestionnaire des tâches et la fenêtre, dont le titre devient « Star Fox - N img/s ».
  - Source : les métadonnées du contenu de base de la copie, une suite de blocs `[u32 genre][u32 taille]`.
    Le bloc 3 est un JPEG 256×256 de 50 Ko ; le 1 est le NPDM, le 5 et le 8 sont les noms.
  - `preparer.py` (étape 5, `extraire_icone`) le convertit en `build/full/icone.ico` : 7 tailles en PNG, de 256 à
    16 px. Le décodage et la réduction passent par WIC, appelé par `ctypes`, et le PNG est écrit avec `zlib`.
    Il n'y a aucun programme extérieur : une commande PowerShell encodée, lancée par le launcher, ressemblerait
    trop à un logiciel malveillant pour les antivirus.
  - `link.py` l'intègre comme ressource n° 1 (windres lancé dans `rt/` avec des chemins relatifs) et
    `present.cpp` la donne à la classe de fenêtre. Sans icône, ou sans windres, l'exécutable est fait pareil.
  - Le launcher l'affiche dans sa fenêtre et la barre des tâches (`WM_SETICON`, au démarrage et après une
    construction). Le fichier `lanceur.exe` garde son icône verte : il est distribué, l'icône du jeu ne l'est
    jamais.
- **Installation neuve vérifiée** (2026-11-10) : sources seules (2,5 Mo) + la copie, dans un dossier vide. Les sept
  étapes passent sans erreur en 51 min en priorité minimale : 3 s pour vérifier, extraire et préparer, 49 min de
  recompilation, 56 s d'édition de liens et 98 s de shaders.
  - Le jeu ainsi construit joue la cinématique et l'entraînement (11 400 images, aucun dessin ignoré, aucun
    shader absent).
  - Il vide au démarrage 159 shaders absents de la romfs (DLSS et passes associées, jamais dessinés en jeu
    normal). `shaderc.py --nouveaux` les traduit après l'étape des extraits : les 2 388 SPIR-V sont alors
    identiques, octet pour octet, à ceux de l'installation faite en jouant.
- **Paquet** : `launcher/paquet.py <dossier> [--zip]` assemble lanceur.exe, `LISEZMOI.txt`, les sources et
  `tools/`. Il refuse tout fichier dérivé du jeu (4,4 Mo sans outils).
- **Outils fournis** (`launcher/outils.py`, 2026-11-10, avec l'accord de l'utilisateur) : téléchargés depuis les sources
  officielles dans `recomp/outils/`, empreintes SHA-256 vérifiées (publiées par python.org, PyPI et GitHub) :

  | Outil | Version | Rôle |
  |---|---|---|
  | Python embarquable | 3.13.17 | pilote, recompilateur, extraction (`._pth` réglé pour `site-packages`) |
  | zstandard, lz4 | 0.25.0, 4.4.5 | lecture du `.nup`, des NSO et de `data.zip` |
  | capstone | 5.0.7 | désassembleur du recompilateur (`a64/codegen.py`), version du code validé |
  | GCC (WinLibs, UCRT, POSIX) | 16.2.0 r3 | C++ généré, runtime, launcher |
  | glslang | 16.6.0 | GLSL → SPIR-V (`glslang.exe` seulement depuis la v14 : `RECOMP_GLSLANG`) |

  `outils.py` n'installe que ce qui manque (marques `outils/.installe/`). Les archives sont supprimées après
  installation ; `outils/LISEZMOI.txt` donne sources, empreintes et licences.
- **Pièges de GCC (WinLibs)** :
  - il vise Windows 7 par défaut : `_WIN32_WINNT=0x0A00` est défini pour le launcher (`lanceur.h`) et le runtime
    (`link.py` ; le code généré n'est pas touché, son cache reste valable) ;
  - **installé dans un chemin à espaces, il échoue à l'édition de liens** (« cannot find C:/…/Projet »), même pour
    un programme d'une ligne ; ni le chemin court 8.3 ni une jonction NTFS n'y font rien (GCC 16 remonte au chemin
    réel). `preparer.py` (`gcc_sans_espaces`) copie alors le compilateur une fois (≈ 1 Go, 20 s) dans
    `C:\ProgramData\StarFoxRecomp\gcc-<empreinte>`, sans droits d'administrateur ;
  - GCC ajoute d'office `default-manifest.o`. Celui de MSYS2 est en langue neutre et ld l'écarte devant un
    manifeste du programme ; celui de WinLibs est en anglais (1033). Le launcher avait donc deux manifestes n° 1
    (« .rsrc merge failure: multiple non-default manifests »). `construire.py` fait trouver d'abord, par `-B`,
    un `default-manifest.o` vide ; le jeu, qui n'a pas de manifeste à lui, garde celui de GCC.
- **Paquet** : `launcher/paquet.py` y ajoute `recomp/outils` (sans les archives) : 1,08 Go décompressé.
  Un paquet placé dans un dossier à espaces, construit avec ses seuls outils (`PATH` réduit à Windows,
  `RECOMP_SANS_MSYS2=1`), a révélé l'absence de `capstone`, ajouté depuis.
- **Paquet vérifié** (2026-11-10) : même dossier à espaces, mêmes outils seuls, priorité minimale.
  - La reconstruction complète passe en 3 091 s : 2 964 s de recompilation, 51 s d'édition de liens et 68 s de
    shaders (2 308 SPIR-V).
  - Le jeu ainsi construit tourne 4 min (11 554 images à 60 img/s) sans shader absent, sans dessin ignoré, avec
    une mémoire vidéo de 3,1 Go et des images correctes.
  - L'étape « lier », refaite avec l'icône, l'extrait et l'intègre (14 s).
  - Le paquet de cette vérification (0.1) a été remplacé par la release 1.0.0 (voir plus bas).
  - Les SPIR-V diffèrent de ceux faits avec MSYS2, parce que glslang 16.6 n'est pas 16.3 : 16.6 plie davantage les
    constantes, par exemple `uintBitsToFloat(0x3f000000u)` → `0.5`. 400 d'entre eux, tirés au hasard, passent
    `spirv-val` (Vulkan 1.3, blocs scalaires comme le runtime).
- **A/B des shaders 16.3 / 16.6** (2026-11-10) : même exécutable, même cinématique (images 1 201 à 10 800), quatre parties
  en alternance. Intervalle moyen 17,41 à 17,48 ms dans les deux cas, même nombre d'à-coups, temps GPU de l'image 6600
  7,9 à 8,3 ms des deux côtés, aucune erreur : les SPIR-V de glslang 16.6 valent ceux de 16.3.

## Version 1.0.0 : SpaceAnimalsRecomp (2026-11-10)

Publiée sous le nom **SpaceAnimalsRecomp** (le launcher affiche « StarFox Remake [recompiled] ») :
<https://github.com/SerenityG4K/SpaceAnimalsRecomp>, licence GPL-3.0, release v1.0.0.

### Shaders précompilés (VK_EXT_graphics_pipeline_library)

- **Principe** : chaque shader est compilé seul, une fois, en bibliothèque de pipeline.
  - Sommets : bibliothèque « avant rastérisation », avec la fenêtre, le contrôle de profondeur [-1, 1] et le mode des
    polygones.
  - Fragments : profondeur et stencil dynamiques, sans état multi-échantillon, pour qu'elle se lie à toute sortie.
  - Calculs : pipeline complet.
  - Au dessin, il ne reste qu'à créer les entrées de sommets et la sortie (formats, mélange), sans shader, puis à lier
    rapidement les quatre bibliothèques : 0,01 à 0,65 ms par pipeline (`RECOMP_PIPE_TIMING=1`).
- **États dynamiques** (Vulkan 1.3, communs à tous les pipelines graphiques, réglés seulement quand ils changent) :
  rejet et sens des faces, biais, test, écriture et comparaison de profondeur, test et opérations de stencil.
  - Ils ne font plus partie de la clé des pipelines : 201 pipelines dans la cinématique au lieu de 255.
  - Alpha → couverture, oublié jusque-là, entre dans la clé.
- **`recomp_main --precompile`** : pas de fenêtre, pas de jeu. Il compile les 2 388 shaders du cache (1 137 sommets,
  1 131 fragments, 120 calculs) en parallèle et écrit `pipelines.cache` (≈ 90 Mo, `VkPipelineCache`).
  - Durée : 33,5 s à froid sur 16 fils (RTX 5070 Ti).
  - `pipelines.ok` garde la marque : GPU, pilote, UUID du cache, liste des shaders, taille et date de l'exécutable.
    Quand rien n'a changé, la commande s'arrête en 0,3 s (« @saute »).
  - Elle est lancée par l'étape « pipelines » de `preparer.py` et par le launcher avant chaque partie (progression
    « SHADERS xx % »).
- **Au démarrage du jeu** (`warm_up`, avant la première image) : toutes les bibliothèques sont créées depuis le cache
  en 1,6 s. Sans précompilation, c'est la compilation complète, avec « compilation des shaders xx % » dans le titre.
  - Le cache est réécrit à la fermeture et au plus une fois par minute s'il a grandi.
  - `RECOMP_NO_GPL=1` revient aux pipelines complets, compilés au premier dessin.
- **Validation** (couche Khronos de MSYS2, `VK_ADD_LAYER_PATH`) : aucune erreur sur les bibliothèques. En jeu, une seule
  sorte d'erreur, déjà là avant : VUID-…-Input-08733, un attribut R8G8B8A8_UINT lu en `vec4` flottant.
- **Mesure** : le compteur « pipeline » de `RECOMP_PERF` couvre toute la section pipeline de chaque dessin (clé,
  recherche, liaison, états dynamiques) ; ses ≈ 5 s sur la cinématique sont un coût fixe d'environ 0,5 ms par image,
  pas des compilations. Le gain va au joueur dont le cache du pilote est vide (premier lancement, nouveau pilote) :
  chaque nouveau matériau coûtait ≈ 200 ms de compilation au moment de son apparition.
- **Piège de GCC** : passer `VkGraphicsPipelineCreateInfo` par valeur plantait. GCC le copiait avec un `vmovdqa`
  (écriture AVX alignée sur 32 octets) dans une pile que Windows n'aligne que sur 16. `-Wa,-muse-unaligned-vector-move`
  (runtime, `link.py`) rend toutes ces écritures non alignées, à la même vitesse.

### Launcher 1.0

- Titre « STARFOX / REMAKE [RECOMPILED] » ; version lue dans `recomp/VERSION`.
- Choix **PLEIN ÉCRAN / FENÊTRÉ** sous le bouton principal, en plus de Réglages.
- **JOUER** lance d'abord la compilation des shaders (`Tache::Shaders`), puis le jeu.
  - Les tâches (construction, shaders, mise à jour) passent par `lancer_tache` et suivent le même protocole « @… ».
  - `LANCEUR_ACTION=shaders` compile les shaders sans lancer le jeu (vérification).
- **Mises à jour** (`launcher/maj.py`, seulement pour un paquet installé : `lanceur.exe` à côté de `recomp/`).
  - Au démarrage, `--verifier` lit la dernière release ; une version plus récente s'affiche en haut
    (« MISE À JOUR x DISPONIBLE »). La vérification se désactive dans Réglages.
  - `--installer` télécharge `SpaceAnimalsRecomp-<v>-update.zip`, vérifie son SHA-256 (champ `digest` de GitHub, sinon
    `SHA256SUMS.txt`), l'extrait dans `maj/` et installe par `outils.py` les outils que la version demande.
  - `--finir` attend la fermeture du launcher, copie les fichiers et le relance.
- **Reconstruction incrémentale** : chaque étape de `preparer.py` a une signature = copie + empreinte de son code + étapes
  dont elle dépend.
  - Après une mise à jour, le launcher voit `.preparation/version.ok` ≠ `recomp/VERSION` et propose
    « METTRE À JOUR » ; seules les étapes dont le code a changé sont refaites.
  - Les objets inchangés sont réutilisés par l'empreinte de `build.compile_one`.

### Publication

- `launcher/depot.py <dossier>` assemble le dépôt public. Il prend les fichiers de la racine dans `recomp/depot/`
  (README anglais, LICENSE GPL-3.0, CHANGELOG, CONTRIBUTING, `.gitignore`), ces notes deviennent
  `docs/DEV-NOTES-fr.md`, et il refuse tout fichier dérivé du jeu et toute trace personnelle.
- `launcher/paquet.py <dossier>` produit `SpaceAnimalsRecomp-<v>-windows.zip` (91 Mo depuis la 1.0.1, outils
  allégés compris), `-update.zip` (1 Mo) et `SHA256SUMS.txt`.
- Nouvelle version :
  1. changer `recomp/VERSION` ;
  2. compiler le launcher (`construire.py`) ;
  3. lancer `depot.py` dans un clone du dépôt, puis commit et push ;
  4. lancer `paquet.py` ;
  5. `gh release create v<v> … *-windows.zip *-update.zip SHA256SUMS.txt`.

## Version 1.0.1 : paquet allégé et licences des outils (2026-11-10)

- **Allègement** (`launcher/outils.py`, `GARDER_GCC`) : la liste des fichiers de GCC utiles vient d'une trace des vraies
  commandes de la construction, avec le GCC fourni seul dans le PATH : 26 fichiers du runtime et du code généré compilés
  avec `-v -MD`, `windres -v` (il appelle `gcc -E`, donc `cc1`), l'édition de liens complète avec `-v -Wl,--trace`, puis
  la fermeture des DLL importées (tables d'import PE). 594 fichiers servent (136 Mo). On garde en plus, en entier, les
  en-têtes C/C++ et les bibliothèques MinGW-w64, libstdc++ et libgcc : 4 093 fichiers, 382 Mo, contre 11 874 fichiers et
  968 Mo pour WinLibs complet. glslang : `bin/glslang.exe` seul. `paquet.py` applique ce filtre en copiant
  `recomp/outils` (qui reste complet, avec gdb, pour le développement) ; `outils.py` allège ce qu'il installe, sauf
  avec `--complet`.
- **Résultat** : paquet de 91 Mo au lieu de 316 Mo, 424 Mo décompressé au lieu de 1 076 Mo.
- **Installations existantes** : une mise à jour ne touche pas `outils/` ; une installation 1.0.0 garde son GCC complet,
  qui marche. La copie dans `C:\ProgramData\StarFoxRecomp\gcc-<empreinte>` (chemin avec espaces) est nommée d'après
  l'archive WinLibs : une copie complète déjà faite par une autre installation est réutilisée telle quelle.
- **Licences** : `recomp/licences/` contient `THIRD_PARTY_NOTICES.txt` (chaque composant fourni, sa version exacte, sa
  licence, l'adresse de son code source) et les textes complets. Versions relevées dans les binaires : GMP 6.3.0,
  MPFR 4.2.2, MPC 1.4.1, isl 0.28, libiconv 1.19, gettext 1.0, zstd 1.5.7, zlib 1.3.2, binutils 2.47.20260726.
  `paquet.py` les met à la racine du paquet (`THIRD_PARTY_NOTICES.txt`, `LICENCES/`), dans les deux archives.
- **Launcher** : `construire.py` écrit `lanceur.exe` à la racine du projet, à côté de `recomp/`, comme dans le paquet.
  Ses réglages (`lanceur.ini`) sont à côté de lui. Dans cette disposition, le launcher vérifie aussi les releases.

## Version 1.0.2 : extraits fournis, relevé des shaders (2026-11-10)

- **Extraits du fond du launcher fournis** : à la demande de l'utilisateur (« c'est juste mon gameplay »), les 6 extraits
  de `launcher/extraits/` (59 Mo, vidéo seule) sont dans le paquet et dans la mise à jour. `paquet.py` les copie (seule
  exception à la règle « rien du jeu dans le paquet ») ; ils restent hors du dépôt git.
- **Étape « extraits »** : sautée si des extraits sont déjà là, sauf demande explicite (`--etapes …,extraits`, bouton
  « RÉENREGISTRER LES EXTRAITS » des Réglages). « RECONSTRUIRE » (`--forcer`) ne les réenregistre plus.
- **Relevé des shaders** (`preparer.releve_shaders`, dans l'étape « pipelines », une fois par exécutable construit) : le
  jeu crée au démarrage 159 shaders absents des paquets de `data.zip` (80 traduisibles, dont 72 calculs, et les 79 du
  réseau DLSS de la console) et les écrit dans `shaders/` (« shader inconnu »). Le jeu est lancé fenêtré, sans le son,
  avec une sauvegarde à part, jusqu'à sa 600e image ; puis `shaderc.py --nouveaux` et une seconde précompilation.
  Jusqu'à la 1.0.1, l'enregistrement des extraits faisait ce relevé au passage (`e_extraits`) : l'essai 1.0.1 sans
  extraits (`--sans-extraits`) avait donc 2 308 shaders, et la note « problème connu » de la 1.0.1 était fausse pour
  une installation normale.
- Le launcher affiche « Le jeu s'ouvre quelques secondes… » pendant le relevé (`cons_info_releve`).

## Correctifs des issues GitHub n° 1 et n° 4 (2026-11-10, pour la 1.0.3)

- **N° 1, plantage de `--precompile`** (RTX 5070, pilote 617.42 : 0xC0000005 vers 83 %, le compilateur du pilote plante
  sur un shader). `recomp_main --precompile` fait maintenant le travail dans un processus enfant
  (`RECOMP_PRECOMPILE_ENFANT=1`, `precompile_travail`), qui écrit `+nom` / `-nom` autour de chaque shader dans
  `precompile_en_cours.txt`. Si l'enfant plante, le parent (`precompile_superviseur`) recompile un par un les shaders
  qui étaient en cours (`RECOMP_PRECOMPILE_SEULS`, `RECOMP_PRECOMPILE_FILS=1`, journal `precompile_isolement.log`) ; le
  fautif est inscrit dans `shaders_exclus.txt` sous la ligne `pilote <vendeur:appareil> <version>`, puis la
  précompilation reprend. Pour ce pilote seulement, un shader écarté n'est ni précompilé ni dessiné (`get_shader` rend
  nul : dessins ignorés au lieu d'un plantage). Plantage non reproduit shader par shader : reprise sur un seul fil.
  Le parent écrit `precompile_controle.log`. Le launcher (JOUER) et `preparer.py` en profitent sans changement.
  Essai : `RECOMP_PRECOMPILE_PLANTE=<nom>` simule le plantage ; shader isolé et écarté en moins d'une seconde.
- **N° 4, dossier d'installation déplacé** : `manifest.txt` et `build/full/<module>/objs.txt` contiennent des chemins
  absolus (fullbuild.py), et les étapes sont sautées après un déplacement (la signature ne retient que le nom de la
  copie). Le chargeur reprend une image absente dans `<racine>\work\` et la copie dans `RECOMP_NUP` (passée par le
  launcher, `lanceur.cpp`) ou dans un `.nup` à la racine ; `link.py` reprend un objet absent à côté de `objs.txt`.
  Essai : manifeste pointant vers `C:\AncienDossierSFR` → images et copie retrouvées, 90 s de jeu sans shader absent.

## Vérifications avant chaque version (`verif/verifier.py`, 2026-11-10)

Demande de l'utilisateur : « il faut de la rigueur et s'assurer que rien ne casse à chaque build chez nous mais
potentiellement chez les autres aussi ». Tous les essais faits à la main pour les 1.0.1 et 1.0.2 sont maintenant dans
`verif/verifier.py` (voir sa documentation en tête) :

- `rapide` (≈ 5 min, jeu non reconstruit) : versions cohérentes (VERSION, lanceur.h, lanceur.rc, ressource et date de
  lanceur.exe, CHANGELOG, README) ; tous les .py ; runtime compilé et lié (exécutable d'essai hors de build/full) ;
  launcher compilé sans avertissement ; export du dépôt ; paquet (licences, extraits, outils allégés, sources à jour,
  mise à jour = paquet sans outils/) ; GCC allégé (fichiers de `verif/gcc_utilises.txt` présents, DLL importées
  présentes, runtime Visual C++ demandé par glslang signalé) ; mise à jour depuis la dernière release publiée avec SON
  maj.py (seul GitHub est simulé) et étapes que « RECONSTRUIRE MAINTENANT » refera ; plantage simulé du pilote (issue
  n° 1) ; installation déplacée (issue n° 4).
- `complet` (≈ 50 min) : en plus, installation neuve du paquet dans un dossier AVEC des espaces (copie du compilateur dans
  un ProgramData d'essai) et PATH sans MSYS2, construction complète par preparer.py ; shaders (2 388, GLSL et .meta
  identiques à l'arbre de référence) ; jeu 150 s ; jeu sans `VK_EXT_graphics_pipeline_library` (`RECOMP_NO_GPL`, cas
  des GPU qui ne l'ont pas) ; launcher (capture).
- Les essais du jeu ont leur propre sauvegarde et leur journal, ne commencent pas si le jeu ou le launcher est ouvert et
  s'arrêtent si le joueur lance le jeu. Les fichiers dérivés du jeu sont supprimés à la fin (sauf `--garder`).
- Rapport : `verif/rapports/<date>_<version>_<niveau>.md` (privé, jamais exporté). `verif/trace_gcc.py` régénère
  `gcc_utilises.txt` après un changement de GCC ou d'options.
- **Procédure de version** : changer `VERSION`, `lanceur.h` et `lanceur.rc`, le CHANGELOG (avec « Fixed #n » pour les
  issues) et le nom du paquet dans le README ; `launcher/construire.py` ; `verif/verifier.py complet` sans échec ;
  `depot.py` dans le clone, commit, push ; `paquet.py` ; `gh release create` ; essai réel de la mise à jour depuis
  GitHub ; message à la session des issues (numéros corrigés, version, reconstruction ou non).
- **Premier problème trouvé par `complet`** (2026-11-10) : la construction neuve échouait en `MemoryError` pendant main.
  Relevé de mémoire toutes les 2 s : chaque processus Python de fullbuild garde ≈ 0,85 Go pendant que g++ compile son lot,
  cc1plus prend jusqu'à 1 Go ; 15 tâches ≈ 28 Go, alors que ce PC (31 Go de RAM, 8 Go d'échange, programmes ouverts)
  n'avait que 16 Go validables libres. `preparer.fils_selon_memoire` borne maintenant les tâches à
  (min(RAM disponible, mémoire validable disponible) − 3 Go) / 2 Go, et l'écrit dans preparation.log. fullbuild.py
  n'est pas modifié (sinon tout le monde recompilerait tout à la mise à jour). Piste pour plus tard : lire les images par
  projection en mémoire (partagée entre les processus) plutôt que par une copie dans chaque processus.

## Construction plus rapide et petites configurations : Clang (2026-11-11, pour la 1.0.3)

Demandes de l'utilisateur : alléger le temps de construction (≈ 50 min chez un joueur sur Ryzen 9 5950X, plus d'une
heure sur Ryzen 5 7600 avec 32 Go, contre 10 à 25 min pour les autres recomps de la Switch 2), puis « faut vraiment
optimiser la recompilation pour les plus petites configs, et le temps de construction ».

Comparaison avec les autres projets (docs de leurs dépôts) : Hookshot et NX2navi (Zelda Ocarina of Time, 17,7 M
instructions, 295 000 fonctions) et DKB Recomp traduisent en C et compilent avec Clang en -O1, -O2 réservé au code
chaud (profil d'une partie) ; leur compilateur prend 120 à 180 Mo par fichier de 3 Mo. Star Fox : 51,3 M instructions
dans main, trois fois plus de code.

### Résultats (copie d'essai : paquet avec outils Clang, dossier avec espaces, sans MSYS2 ; Ryzen 7 9700X)

| Configuration (bridée par un objet Job : affinité et mémoire) | Compilations | Construction complète | Mémoire de pointe (tous processus) |
|---|---|---|---|
| g++ -O2, 1.0.3 d'avant (16 fils, mémoire libre 16 Go) | 7 puis 3 | 66 min | ≈ 2 Go par tâche |
| clang -O1, 4 cœurs / 8 fils, 6 Go libres annoncés | 7 | 25,7 min (recompiler 1 534 s) | 2,41 Go (plus gros processus 0,71 Go) |
| clang -O1, 6 cœurs / 12 fils, 8 Go libres annoncés | 11 | ≈ 20 min (recompiler ≈ 1 140 s) | non relevée |
| clang -O1, 8 cœurs / 16 fils | 15 | 16,7 min (recompiler 997 s) | 4,10 Go |
| **clang -O2 (réglage retenu)**, 8 cœurs / 16 fils | 15 | 21,6 min (recompiler 1 292 s) | 4,11 Go (plus gros processus 0,87 Go) |

Les cœurs du 9700X sont rapides : un vrai petit PC sera plus lent que ces simulations, mais dans les mêmes proportions.
Temps par lot de main sur 4 cœurs chargés (RECOMP_TEMPS) : médiane 10,2 s, 90e centile 11,7 s, maximum 68 s ; les 20
lots les plus lents (presque tous à la fin du module, 4 à 5 Mo de C++) font 7 % du temps.

### Ce qui a changé

- **Clang fourni et pris par défaut** : même release WinLibs que GCC (16.2.0 r3), variante avec LLVM 23.1.3
  (`outils.py`, SHA-256 1f04e482…ba5f) ; GCC n'a pas changé. `outils.GARDER_GCC` garde en plus `bin/clang++.exe`, la
  fermeture de ses DLL (129 fichiers, 206 Mo : LLVM est en DLL séparées) et `lib/clang/23/include` ; dossier gcc du
  paquet ≈ 590 Mo extrait, paquet complet 223 Mo (≈ 150 Mo avant). Licences : `licences/LLVM-LICENSE.txt` (Apache 2.0
  avec exceptions LLVM), `z3-LICENSE.txt` (Z3 5.3.1, chargé par Clang). `recomp/outils` du projet : WinLibs complet
  (`outils.py --complet`), `paquet.py` n'en prend que les fichiers utiles.
- **Choix du compilateur** (`build.compilateur_genere`) : clang++.exe s'il est à côté du g++ trouvé, sinon g++ ;
  `RECOMP_CXX` l'impose, `RECOMP_OPT` le niveau. -O2 par défaut pour les deux (drapeaux de g++ inchangés : ses objets
  déjà compilés restent valables). Vitesse du jeu mesurée (temps processeur par image, images 600 à 2400) : g++ -O2
  14,9 à 15,6 ms, clang -O2 14,1 à 14,2 ms, clang -O1 16,6 à 16,9 ms : -O1 coûte ≈ 17 % de processeur en jeu pour
  5 min de construction gagnées une fois ; les autres recomps compensent par un profil (-O2 sur le code chaud). Le runtime et l'édition de liens restent à g++ (même ABI).
- **`clang_compat.h`** (inclus par `-include`, clang seulement) : `[[clang::musttail]]` exige que l'appelé ait la
  signature de l'appelant ; `rt_tailcall(ctx, cible)` devient `rt_tailjump(ctx)` avec la cible dans `ctx->pad`. Le texte
  généré est le même pour les deux compilateurs.
- **Mesures par lot** (6 lots de main, PC libre, 6 à la fois) : g++ -O2 17,6 s (≈ 1,3 Go, 2,5 Go pour le plus gros) ;
  clang -O2 13,2 s ; clang -O1 10,4 s, ≈ 180 Mo (330 Mo pour le plus gros) ; clang -O0 2,8 s mais objets trois fois plus
  gros. Profil de clang -O1 sur un lot : analyse 21 %, optimiseur 42 %, génération de code 37 % ; aucune option ne
  donne plus de 12 % (`-mllvm -fast-isel` : −12 %, objets +23 %) : -O1 gardé tel quel.
- **Mémoire des processus Python** (fullbuild.py, a64/) : 993 Mo → 167 Mo par processus pour main.
  - image du module projetée en lecture seule (`mmap`, `Module.from_work`) au lieu d'une copie de 252 Mo par processus ;
  - conteneurs par fonction créés à l'analyse seulement (`Function.oublier`, valeurs vides partagées) : −140 Mo ;
  - cache des instructions traduites vidé à 150 000 entrées au lieu de 1,5 million (480 Mo jamais rendus au système).
  La découverte de main passe de 43 s (5 processus, faute de mémoire) à 17 s (11 processus).
- **Génération et compilation séparées** : quelques processus Python génèrent les lots (≈ 0,3 s chacun), le processus
  principal lance les compilateurs (`Compilations`) ; le dernier lot du module, le plus long, part en premier.
  `preparer.fils_selon_memoire` : 0,4 Go par compilation avec clang, 1,4 Go avec g++, 2 Go de réserve ;
  `fullbuild.processus_python` : 0,3 Go par processus Python. `RECOMP_MEM_LIBRE_GO=n` simule la mémoire libre (essais).
- **Découverte reproductible** : chaque tâche de découverte part de la liste commune des fonctions (`_apply`), le
  découpage est fixe (64 tâches) et les résultats sont pris dans l'ordre. Avant, l'origine notée en commentaire d'une
  fonction (bl, b, ptr) dépendait de l'ordre d'arrivée des résultats : ≈ 6 lots recompilés pour rien à chaque
  reconstruction. Maintenant, régénérer tout le code sans changement ne recompile aucun objet (48 s avec 15
  compilations, 84 s avec 6 ; vérification `regeneration` de `verif/verifier.py`).
- **Reprise d'un lot en échec** (`Compilations.reprendre`) : un lot dont la compilation échoue est recompilé seul à la
  fin ; seul un second échec arrête l'étape. Vu pendant la simulation 6 cœurs : clang planté sur le lot 593 de main
  (passe « called-value-propagation »), qui relancé seul compile en 8 s avec 174 Mo. Essai : `RECOMP_ESSAI_ECHEC=<objet>`.
- **Dossier d'installation avec crochets** (issue n° 1, second cas, 2026-11-11) : `preparer.compter` passait le dossier
  à `glob` sans l'échapper ; dans « Jeux [Switch 2] », 0 shader compté : barre bloquée à 35 %, puis « trop peu de
  shaders traduits ». `glob.escape` partout (compteurs, signatures des étapes, extraits, rapport de bug) ; le
  vérificateur construit maintenant dans « installation [neuve] » (espaces et crochets).

### Validation

- `bench/run_all.py` complet avec `RECOMP_CXX` = clang (-O1, runtime compilé par g++) : résultats identiques à g++
  (fuzz main FP/SIMD 245 624 PASS, entiers 240 000, familles rares 199 993 + 7 REFBUG connus, saturants 384, func main
  4 670 PASS / 42 NOTBUILT / 88 NONCODE, rtld et subsdk3 tout PASS).
- Code généré identique avant et après les changements de mémoire : aucun objet recompilé hors des lots dont le
  commentaire d'origine variait déjà.
- `verifier.py rapide --seulement versions,python,paquet,outils` : paquet 223 Mo, 737 fichiers utilisés présents, DLL
  importées toutes présentes.

### Ce qui reste

- **Mise à jour par le launcher** : `-update.zip` ne contient pas les outils, donc pas Clang : ces joueurs restent sur
  g++, régénèrent le code (1 à 2 min) et ne recompilent que quelques lots. Un paquet complet neuf construit avec Clang.
- **Piste pour aller plus loin** (pas faite) : compiler en -O0 le code que le jeu n'exécute jamais (profil d'une
  partie, comme Hookshot et NX2navi), trois fois plus rapide à compiler ; mais les objets -O0 sont trois fois plus
  gros et l'exécutable dépasserait 2 Go : il faudrait le découper en plusieurs DLL.

## Mémoire du jeu : engagement à la demande et imports par fenêtres (2026-11-11, pour la 1.0.3)

Demande de l'utilisateur : « il va falloir optimiser très fortement la consommation de RAM et de VRAM ». Issues n° 5
(écran noir à la cinématique avec 16 Go de RAM : imports refusés, -2) et n° 6 (AMD très lent, RAM saturée). Sur ce PC
de 32 Go, les essais du jeu échouaient dès que les programmes ouverts laissaient moins de ≈ 15 Go validables.

Mesures à l'image 2400 (cinématique d'ouverture, 1440p fenêtré, `scratchpad/perf_jeu.py`) :

| | En RAM | Engagée | Mémoire du jeu importée (verrouillée) | Vidéo |
|---|---|---|---|---|
| Avant (1.0.2) | 12,5 Go | 14,9 Go | 9,3 Go (76 pools entiers) | 2,1 Go |
| Maintenant | 7,0 Go | 9,7 Go | 0,5 Go (17 fenêtres) | 2,1 Go |

Le jeu atteint aussi sa 600e image ≈ 5 s plus tôt (plus de 9 Go à verrouiller d'un coup).

- **D'où venait la mémoire** : le jeu réserve presque tout d'un coup (MapPhysicalMemory : 10,4 Go dans ses six
  premières secondes, dont un pool de 3 Go, deux de 1 Go, deux de 514 Mo) et n'en remplit qu'une partie ; tout était
  engagé à la réservation. Puis, au premier calcul à mémoire globale, tous les pools NVN étaient importés en entier
  dans Vulkan (`import_all_pools`) : 9,3 Go verrouillés en RAM.
- **Engagement à la demande** (`memory.cpp` : `mem_commit_lazy`, `mem_fault`, `mem_ensure`) : MapPhysicalMemory et
  SetHeapSize ne font plus que noter la plage ; ses pages sont engagées par blocs de 2 Mo au premier accès (exception
  d'accès rattrapée par le gestionnaire de `host_main.cpp`, qui reprend l'instruction), ou d'avance par `mem_ensure`
  quand la plage part vers le noyau de Windows ou le pilote : tampons des requêtes IPC (`ipc.cpp` : ReadFile lit la
  romfs directement dans la mémoire du jeu) et fenêtres importées. `mem_valid` tient une page pas encore touchée pour
  valide. Deux fils peuvent fauter sur le même bloc au même instant : le second trouve le bloc déjà engagé et reprend
  (pris d'abord pour une erreur : plantage à 30 s). Blocs de 64 Ko essayés (`RECOMP_LAZY_BLOC_KO`) : 0,6 Go de moins
  mais 45 img/s pendant les chargements ; 2 Mo gardés. `RECOMP_EAGER_COMMIT=1` : ancien comportement.
- **Imports par fenêtres** (`vk_render.cpp` : `find_import`, `import_range`) : un pool de plus de 64 Mo est importé par
  fenêtres de 32 Mo, à la demande des dessins, des copies et des calculs ; les plus petits en entier. Budget : 40 % de
  la RAM (`RECOMP_IMPORT_MAX_MO`) ; au-delà, les fenêtres inutilisées depuis 2 s sont rendues (`evict_import`), sauf
  celles des calculs. Un import refusé n'est retenté qu'après une seconde (avant : des milliers d'essais par seconde,
  1 à 3 img/s dans l'issue n° 5). Les tampons de sommets sont cherchés avec leur taille (une fenêtre doit les couvrir).
- **Mémoire globale des calculs** : les plages des tampons du calcul sont importées avant lui, ses tampons uniformes
  et le début de ses tampons de stockage sont balayés (un mot de 64 bits qui est une adresse de pool fait importer sa
  fenêtre ; une fois par seconde et par tampon), et le shader note dans un tampon de retour toute adresse absente de
  sa table (`gxl_miss` dans `glsl.py`, `glob_retour`) : sa fenêtre est importée pour les calculs suivants. La table
  liste les fenêtres des calculs d'abord. Changement de `glsl.py` : les shaders sont retraduits à la mise à jour.
- **Journal** : ligne « image N, mémoire : … » toutes les 600 images (RAM, mémoire engagée, mémoire du jeu réservée et
  engagée, imports, fenêtres rendues, refus). Si Windows refuse une page, le jeu s'arrête avec le code 4 (message
  « Mémoire insuffisante » du launcher). Seuil d'avertissement au démarrage : 10 Go validables libres.
- **Essais** (copie d'essai, Clang -O2) : cinématique jusqu'à l'image 2400 à 60 img/s, 0 dessin ignoré, 0 shader absent,
  captures d'écran correctes (`RECOMP_SNAPSHOT_EVERY`) ; essai avec `RECOMP_IMPORT_MAX_MO=300` et
  `RECOMP_GLOB_RETOUR_SEUL=1` (ni pré-import ni balayage) : image correcte, 3 fenêtres rendues, aucun refus.
- **Pas encore essayé** : au-delà de la cinématique d'ouverture (changements de niveau : `pools_released_locked` avec
  des fenêtres), le tampon de retour en conditions réelles (aucune adresse manquée dans ces essais), une carte AMD, un
  vrai PC de 16 Go. Le temps processeur par image n'a pas pu être comparé proprement (PC à court de mémoire validable
  pendant les essais, et déroulement de l'introduction décalé).
- **Ce qui reste comme mémoire** à l'image 2400 : ≈ 5,6 Go de mémoire du jeu réellement touchée (dont les données
  des textures, gardées aussi en mémoire vidéo), ≈ 2 Go de mémoire vidéo (comptée par Windows dans la mémoire engagée),
  ≈ 1,8 Go de base (pilote, 2 388 shaders précompilés). Pistes : rendre à Windows les pages des textures déjà envoyées
  au GPU ; copies en mémoire vidéo des tampons lus par le GPU (ce que font Hookshot et NX2navi), sans doute nécessaires
  pour AMD (issue n° 6).

## Mises à jour sans reconstruction : empreintes, lots figés, compilateur gardé (2026-11-11, pour la 1.0.3)

Demande de l'utilisateur : corriger au plus tôt tout ce qui touche à la construction, pour que les premiers joueurs
n'aient pas à reconstruire leur jeu à chaque mise à jour. Règle depuis la 1.0.3 : une mise à jour ne recompile que les
lots dont le texte C++ change vraiment, et normalement aucun.

**Ce qui invalide un objet.** L'empreinte d'un objet (`build.cles_objet`) porte sur les drapeaux du compilateur, le
texte du lot et les en-têtes du code généré (`cpu.h`, `simd.h`, plus `clang_compat.h` avec Clang). Donc :

| Cause | Lots recompilés |
|---|---|
| un octet de `runtime/cpu.h`, `runtime/simd.h` ou `runtime/clang_compat.h`, commentaire compris | tous |
| drapeaux de `build.py`, niveau `-O`, changement de compilateur | tous |
| texte émis par `a64/*.py` (traduction, commentaires émis dans le C++) | ceux dont le texte change |
| nouvelle ligne de `hooks.txt` | ceux qui appellent la fonction accrochée |
| déplacement d'un début de lot dans `lots.txt` | les deux lots voisins |

Tout le reste (runtime, launcher, shaders, scripts) ne touche pas aux objets. Pour remplacer une fonction importée par
le jeu, préférer `override_export` (runtime seul) à une ligne de `hooks.txt`.

**Trois défauts trouvés en préparant la 1.0.3**, qui auraient fait tout recompiler chez les joueurs :

1. *Le chemin d'installation entrait dans l'empreinte* (`-I…\recomp\runtime` dans les drapeaux) : un dossier déplacé ou
   renommé recompilait tout. `cles_objet` rend maintenant deux empreintes : l'actuelle, où ce chemin est remplacé par
   `<runtime>`, et l'ancienne ; `objet_a_jour` accepte les deux et réécrit l'ancienne à la première construction.
2. *Le découpage en lots glissait.* Les lots étaient découpés au fil des fonctions (≈ 60 000 instructions chacun) : une
   fonction découverte en plus ou en moins décalait tous les lots suivants du module. `recomp/lots.txt` fige l'adresse
   de début des 902 lots (847 de main, 36 de sdk, 11, 5, 2 et 1 pour les subsdk), avec le découpage des versions 1.0.0
   à 1.0.2 ; un lot est l'ensemble des fonctions dont l'adresse va de son début à celui du suivant
   (`fullbuild.lots_figes`). `python fullbuild.py sdk subsdk0 subsdk1 subsdk2 subsdk3 main --figer-lots` l'écrit
   (découverte seule, 33 s) : à ne jamais refaire après une publication. Un lot figé devenu vide est simplement sauté.
3. *La mise à jour retéléchargeait le compilateur.* L'archive de GCC a changé avec Clang (nouvelle empreinte SHA-256) :
   le `maj.py` de la 1.0.2 l'aurait retéléchargée (424 Mo), `build.py` serait passé à Clang et aurait changé tous les
   drapeaux. L'entrée `gcc` de `outils.py` porte maintenant `acceptes` (l'archive des versions 1.0.0 à 1.0.2) :
   `outils.py` la laisse en place, `maj.outils_manquants` ne la réclame plus. `outils.py --niveau` la remplace quand
   même ; `preparer.compilateur_a_niveau` l'appelle quand il n'y a pas d'objets à garder (moins de 100 objets de main),
   pour qu'une construction complète profite de Clang. Un échec (pas de réseau) laisse g++ en place.

S'y ajoute la découverte des fonctions rendue reproductible (section Clang ci-dessus) : avant, le commentaire d'origine
de quelques fonctions changeait d'une construction à l'autre.

**Vérifié.** Par le calcul : un objet compilé par la 1.0.2 a une empreinte que la 1.0.3 accepte (mêmes drapeaux g++,
mêmes 34 597 octets d'en-têtes) ; le texte généré par les sources de la 1.0.2 et par les sources actuelles a les mêmes
902 lots, identiques sauf le commentaire d'origine dans 5 lots (c'est le défaut de reproductibilité de la 1.0.2 : un
joueur recompilera donc environ 5 lots, une fois). En vrai : le `maj.py` de la 1.0.2 lance le nouvel `outils.py`, qui
garde le compilateur sans rien télécharger ; sur l'installation d'essai, la régénération avec `lots.txt` et les
nouvelles empreintes garde les 908 objets (0 recompilé, 91 s avec 6 tâches).

**Ce que la reconstruction (« RECONSTRUIRE MAINTENANT », proposée, jamais imposée) fait chez un joueur en 1.0.2** : régénération du texte et comparaison des empreintes
(1 à 5 min), environ 5 lots recompilés par g++, runtime recompilé et édition de liens, 2 388 shaders retraduits
(`shaders/glsl.py` a changé). L'étape « recompiler » est relancée dès qu'un de ses fichiers change (`DEPENDANCES` dans
`preparer.py`, dont `lots.txt`), mais relancer n'est pas recompiler.

**Vérificateur.** Le contrôle `maj` part maintenant des outils de la version précédente (marques écrites par SON
`outils.py`, plus les marques de l'arbre actuel), laisse `outils.py` tourner pour de bon avec
`RECOMP_OUTILS_SANS_RESEAU=1` (tout téléchargement est une erreur) et compare `outils/` avant et après.
`regeneration` vérifie toujours « 0 objet recompilé ». Les constructions d'essai ont leur propre `APPDATA`.

## Sauvegardes hors du dossier de construction, copies de secours (2026-11-11, pour la 1.0.3)

Demande de l'utilisateur : vérifier que les sauvegardes sont intactes à chaque reconstruction.

- **Emplacement.** Jusqu'à la 1.0.2 : `recomp\build\full\save`, au milieu des fichiers de construction. Depuis la
  1.0.3 : `%APPDATA%\SpaceAnimalsRecomp\save` (`svc_fs.cpp`, `save_root`). Au premier lancement, les fichiers de
  l'ancien dossier y sont copiés (jamais déplacés, jamais écrasés si le nouveau dossier en contient déjà) et une note
  `SAVE MOVED - SAUVEGARDE DEPLACEE.txt` est laissée dans l'ancien. Si `%APPDATA%` est inutilisable, l'ancien dossier
  sert encore. `RECOMP_SAVE_DIR` garde la priorité (essais). `recomp_main --save-dir` affiche le dossier sans lancer
  le jeu ; l'ouverture d'un fichier de sauvegarde est notée dans le journal.
- **Copies de secours** (`launcher/sauvegardes.py`) : avant chaque construction (`preparer.py`) et chaque mise à jour
  (`maj.py`, à partir de la version qui suit la 1.0.2), les deux emplacements sont copiés dans
  `%APPDATA%\SpaceAnimalsRecomp\save_backups\<date>_<heure>_<raison>[_ancien]`, avec leur empreinte ; une copie
  identique à la précédente n'est pas refaite, les 10 dernières sont gardées. Rien n'est écrit dans les dossiers de
  sauvegarde eux-mêmes, et un échec de copie ne bloque rien.
- **Vérificateur** : `sauvegardes` (reprise vers `%APPDATA%` identique, ancien dossier intact, pas d'écrasement,
  `RECOMP_SAVE_DIR` respecté, copies de secours identiques, sans doublon, bornées), toujours avec des dossiers
  d'essai ; `maj` pose une sauvegarde d'essai à l'ancien emplacement et vérifie qu'elle survit à la mise à jour.

## Mémoire du PC épuisée : mesures, arrêt net, avertissements (2026-11-11, pour la 1.0.3)

Deux parties de l'utilisateur se sont arrêtées avec le code `0xC0000409`, sans ligne dans le journal du jeu. Le journal
de Windows (Système, « Application Popup ») donne la cause aux mêmes secondes : « dwm.exe : le processus est terminé,
car il est impossible d'allouer davantage de mémoire ». C'est la mémoire validable du PC (RAM + fichier d'échange) qui
était pleine, pas un défaut du rendu.

- **Mesures sur ce PC** (31 Go de RAM, fichier d'échange fixe de 8 Go, donc 39,1 Go validables) : 22 Go déjà engagés
  sans le jeu (340 processus, dont 15,6 Go privés). Le jeu engage 7,9 Go à l'image 600, 12,06 Go après 3 min 30
  (image 10 800 : 5,9 Go de mémoire du jeu au premier accès, 3,4 Go de mémoire vidéo, 1,5 Go d'images sans objet, le
  reste pour le pilote et les shaders). Le chiffre de 10 Go annoncé jusque-là était
  donc trop bas ; celui à donner aux joueurs sera fixé à la sortie, comme un minimum mesuré (la section suivante
  montre des pics à 13 ou 14 Go).
- **Qui a pris le reste** : la première fois, la construction du graphe de code de l'utilisateur (Graphify, 6 à
  14 min, pic mesuré à 7,8 Go : 98 % du graphe sont les en-têtes du compilateur dans `recomp/outils/gcc`) ; la seconde,
  une comparaison de texte généré lancée 12 s avant. Règle pour les essais : aucun travail de plus de 1 Go sans
  garde-fou (refus sous 14 Go libres, arrêt sous 4 Go ou si le jeu est lancé), comme `perf_jeu.py`.
- **Défaut de conception révélé** : avec l'engagement à la demande, un PC à court de mémoire ne refuse plus le jeu au
  démarrage, il lui refuse une page en pleine partie. Quatre protections (`memory.cpp`, `engager`) :
  1. au démarrage, le jeu note la mémoire validable libre et prévient sous 13 Go ; le launcher refuse le premier clic
     sur JOUER sous 13 Go (`memoire_faible`), un second clic dans les 30 s lance quand même ;
  2. une marge de 256 Mo est engagée d'avance (`mem_marge_init` ; elle ne prend pas de RAM tant qu'elle n'est pas
     touchée) et rendue à Windows au premier refus ;
  3. au premier refus, le rendu est prévenu (`mem_pression`) : à l'image suivante il détruit les images sans objet et
     rend les fenêtres d'import qui ne servent pas à l'image en cours ; le fil fautif réessaie pendant 5 s, le temps
     que cette mémoire revienne ou que Windows agrandisse le fichier d'échange ;
  4. après quoi seulement le jeu s'arrête : une ligne dans le journal et le code 4 (aussi pour une allocation du
     runtime refusée, `std::set_new_handler`, et pour `std::terminate` ou `abort` quand la mémoire validable est
     épuisée) ; le launcher affiche alors « mémoire insuffisante » au lieu de « le jeu s'est arrêté ».
- **Interrupteur `memory_saver`** (`recomp.ini`, section `[memory]` ; `RECOMP_MEMORY_SAVER=0`, et l'ancien
  `RECOMP_EAGER_COMMIT=1`) : `false` ramène le comportement des versions 1.0.0 à 1.0.2, c'est-à-dire tout engagé à la
  réservation (`mem_commit_lazy`), pools importés en entier, sans budget ni éviction, et tous importés au premier
  calcul à mémoire globale (`import_all_pools`). C'est le recours d'un joueur chez qui la nouvelle gestion poserait un
  problème ; le launcher garde cette ligne quand il réécrit `recomp.ini`. Les shaders sont les mêmes dans les deux
  modes.
- **Pistes pour réduire encore** : plafond des images sans objet (`UNREF_CAP`, 1,5 Go), pages des textures rendues au
  système après leur envoi au GPU, shaders créés à la demande plutôt qu'au démarrage.

## Manettes PlayStation et Switch Pro, vibrations (2026-11-11, pour la 1.0.3)

- **SDL3** (`input.cpp`) : `SDL3.dll` est chargée dynamiquement (à côté de l'exécutable, puis `<racine>\outils\sdl3`,
  puis `<racine>\recomp\outils\sdl3`) et reconnaît les manettes Xbox, PlayStation 3, 4 et 5, Pro de la Switch et
  Joy-Con. Toutes les manettes branchées pilotent le joueur 1. Les boutons sont repris par position (celui du bas de la
  manette est le B de la console, celui de droite le A, celui de gauche le Y, celui du haut le X), comme avec XInput.
  Sans `SDL3.dll`, ou avec `RECOMP_NO_SDL=1`, la première manette XInput sert, comme avant. Le journal note la
  bibliothèque utilisée et chaque manette branchée ou débranchée.
- **Vibrations** : le jeu appelle `nn::hid::SendVibrationValue`, importée par main. Elle est remplacée par
  `override_export` (runtime seul, aucun objet recompilé) : l'octet 2 de la poignée donne le côté, la valeur porte
  deux amplitudes et deux fréquences (bande basse, bande haute). `host_set_vibration` garde la dernière valeur de
  chaque côté ; `appliquer_vibrations` envoie l'amplitude basse au moteur lourd et l'amplitude haute au moteur léger
  (`SDL_RumbleGamepad`, renvoyé toutes les 150 ms avec une durée de 250 ms pour qu'une vibration s'arrête seule si le
  jeu se fige ; `XInputSetState` sans SDL). Rien ne vibre quand la fenêtre n'a pas le focus. Le service `hid` répond
  aux commandes 200 (`GetVibrationDeviceInfo`) et 205 (`IsVibrationPermitted`).
- **Réglage** : `vibration` dans `recomp.ini` (0 à 100 %, section `[input]`), ligne « Vibrations » du launcher,
  `RECOMP_VIBRATION`. `RECOMP_INPUT_LOG=1` note les 20 premières valeurs envoyées par le jeu.
- **HD Rumble** : les fréquences demandées par le jeu sont lues mais pas rendues ; SDL et XInput ne pilotent que deux
  moteurs par leur amplitude. Les rendre demanderait les effets haptiques propres à chaque manette (DualSense, Joy-Con).
- **Pas encore vérifié avec une vraie manette PlayStation ou Switch Pro.** La page « CONTRÔLES » du launcher montre
  les correspondances Xbox et PlayStation.

## Journaux détaillés, rapport de bug, journaux en anglais (2026-11-11, pour la 1.0.3)

- **En-tête des journaux** : la première ligne de `recomp_run.log` (`journal_systeme`) et de `preparation.log`
  (`rapport.ligne_systeme`) donne la version, Windows, le processeur (fils, AVX2), la RAM et la mémoire validable
  libres. Le jeu note ensuite ses réglages et leur origine, la carte graphique, l'écran et ses modes de présentation,
  et toutes les 600 images une ligne de statistiques et une ligne de mémoire.
- **`lanceur.log`** (racine, 1 Mo au plus) : démarrage du launcher, constructions, parties lancées avec leurs réglages,
  code de sortie du jeu, rapports créés.
- **Rapport de bug** (`launcher/rapport.py`, bouton « RAPPORT DE BUG ») : `SpaceAnimalsRecomp-bug-report.zip` à la
  racine, avec `report.txt` (système, cartes graphiques, état de l'installation, dernières erreurs), les journaux et
  les réglages. Le chemin d'installation devient `<install>`, le dossier de l'utilisateur, son nom et celui du PC
  `<user>` et `<pc>`. Le joueur n'a qu'un fichier à joindre à son issue.
- **Tout est en anglais** depuis la 1.0.3 : messages du jeu, du launcher (`lanceur.log`) et des scripts de
  construction, erreurs affichées par le launcher pendant une construction. L'interface du launcher garde sa langue
  (français ou anglais). Les commentaires du code et les outils de développement (`verif/`) restent en français.
- **Textes relus par des scripts**, à changer ensemble : `frame N:`, `N skipped,`, `N missing shaders`,
  `unknown shader`, `fps` (`verifier.bilan_jeu`, `preparer.releve_shaders`) ; `batches compiled`, `round N`,
  `N functions`, `done` (`preparer.e_recompiler`) ; `shader survey: N created` ; `copy found`, `image found`
  (vérification `deplace`) ; `[vk    ] GPU:`, `display `, `pipelines:` (`rapport.py`) ;
  `@info N shaders compiled in` (vérification `precompile`). Les jetons `@etape`, `@ok`, `@saute`, `@progres`,
  `@info`, `@erreur`, `@fini` et les noms d'étapes sont un protocole entre scripts et launcher : ils ne changent pas.
- **Méthode de traduction** : seules les chaînes ont changé. Un contrôle compare chaque fichier avant et après : même
  nombre de lignes, code identique hors chaînes, mêmes formats `printf` dans le même ordre, mêmes séquences
  d'échappement ; pour le Python, même arbre syntaxique une fois les chaînes vidées. Ni `cpu.h`, ni `simd.h`, ni le
  texte émis par `a64/codegen.py` n'ont été touchés (ils feraient recompiler le jeu).

## Procédure de version : ajouts de la 1.0.3

À faire en plus de la procédure de la section « Vérifications avant chaque version » :

1. **« Cette version demande-t-elle de recompiler ? »** Comparer avec la dernière release : `runtime/cpu.h`,
   `runtime/simd.h`, `runtime/clang_compat.h`, `lots.txt`, `hooks.txt`, `a64/*.py`, les drapeaux de `build.py`. La
   réponse attendue est non ; sinon l'écrire en tête du CHANGELOG et le dire à la session des issues.
2. **Sauvegardes** : `verifier.py` passe `sauvegardes` et `maj` (sauvegarde d'essai intacte après la mise à jour).
3. **Notes en anglais** : reporter dans `recomp/DEV-NOTES.md` les sections de `recomp/README.md` modifiées ;
   `LISEZMOI.txt` et `README.txt` (`paquet.py`) changent ensemble.
4. **Journaux** : tout nouveau message s'écrit en anglais ; si un texte relu par un script change, changer le script.
5. **Mémoire** : un essai de 10 min au moins avec `memory_saver = true`, puis le début du jeu avec `false`.

## Pics de mémoire en jeu : le compilateur du pilote, et les outils pour le voir (2026-11-11, pour la 1.0.3)

Suite de la section « Mémoire du PC épuisée ». Les autres programmes n'expliquaient pas tout : sur ce PC (15,6 Go
validables libres au lancement), un essai sans rien d'autre en fond s'arrêtait quand même vers 2 minutes, le jeu
passant de 11 à 13 ou 14 Go engagés en deux secondes.

**Ce qui est établi.** Pendant ces pics, le tas par défaut du processus passe de 0,6 à 2,6 Go en ≈ 2 s, en ≈ 20 000
blocs de 58 à 142 Ko, puis tout est rendu. Le fil qui alloue est un fil créé par le pilote NVIDIA (`nvoglv64.dll`),
28 cadres dans `nvgpucomp64.dll`, son compilateur de shaders : quelques secondes après qu'un pipeline a été créé par
liaison rapide (0,06 ms de notre côté), le pilote le recompile en arrière-plan, et cette recompilation prend ≈ 2 Go
pour les gros shaders. Le pic revient aux mêmes moments de la cinématique d'ouverture (vers 115 s, 130 s, 160 s selon
les essais), dans 18 essais sur 19, y compris d'un lancement au suivant. Il existait déjà en 1.0.2 et ne dépend pas de
`memory_saver`.

**Ce qui a été écarté, par essai et non par raisonnement** (chaque piste a d'abord semblé expliquer le pic) :

| Piste | Essai | Résultat |
|---|---|---|
| sauvegarde de `pipelines.cache` chaque minute | horaires recoupés, puis sauvegarde coupée | pic inchangé ; la sauvegarde coûte ≈ 0,4 Go |
| bibliothèques de pipelines (liaison rapide) | `RECOMP_NO_GPL=1` | pic inchangé (compilation au premier dessin) |
| nos allocations Vulkan (cibles de rendu, textures, tampons) | suivi de `vkAllocateMemory` | inchangées pendant le pic |
| nos allocations C++ | `operator new` compté | 190 Mo vivants pendant le pic |
| piles des fibres et des fils | en-tête de l'exécutable, `CreateFiberEx` | 64 Ko engagés par fibre |
| grand jeu de descripteurs relié à chaque dessin | lié une fois par enregistrement | pic inchangé (retiré) |
| écritures du jeu de textures une à une | `RECOMP_TEX_WRITE_BATCH=1` : 7 109 écritures en 148 appels | pic inchangé (non activé) |
| optimisation des pipelines liés | `RECOMP_PIPE_NO_OPT=1` (`DISABLE_OPTIMIZATION`) | pic inchangé |

Leçon : j'avais annoncé le cache de pipelines comme la cause sur une coïncidence d'horaires. Depuis, une cause n'est
écrite ici qu'après un essai qui la fait disparaître ou une pile d'appels qui la montre.

**Outils ajoutés** (`RECOMP_MEM_MAP=1`, tout dans le journal du jeu, toutes les 600 images et dès que la mémoire
engagée a grossi de 512 Mo) :

- carte de la mémoire engagée (`mem_map_log`, `VirtualQuery`) : total privé par taille de réservation, plus grosses
  réservations, réservations de 1 à 64 Mo par taille (les segments d'un tas Windows font 16 Mo) ;
- tas du processus (`HeapSummary`) : lequel grossit ; part du runtime (`operator new` compté dans `host_main.cpp`,
  allocations par classe de taille depuis la carte précédente) ;
- allocations Vulkan par genre (mémoire du GPU, visible de l'hôte, importée) et par taille ;
- quand le tas par défaut a pris 512 Mo depuis la carte précédente : blocs occupés par classe de taille avec le
  début de leur contenu (`tas_histogramme`), et piles d'appels de tous les fils passés par une fonction du tas
  (`dump_thread_stacks` : le verrou du tas est tenu 150 ms pour que les fils qui allouent s'y arrêtent).

**Ce qui a été corrigé en chemin** (mesuré, indépendant du pic du pilote) :

1. *Tampons de transit des textures* : un par texture envoyée, de sa taille, rendus seulement 6 soumissions plus tard
   et sans borne par enregistrement. Rendus maintenant dès que le GPU a fini (`free_done_staging`), 256 Mo au plus
   par enregistrement (`borner_transit`).
2. *Cache des images sans objet* (`UNREF_CAP`, 1,5 Go) : il sert (1,3 Go de textures reprises sans nouvel envoi en
   100 s ; à 256 Mo, 14 % d'envois en plus mais 1 Go de mémoire vidéo et de mémoire engagée en moins). Son plafond
   suit la mémoire du PC, relue chaque seconde : 1,5 Go, 256 Mo sous 5 Go validables libres ou quand la mémoire vidéo
   dépasse 85 % de son budget, vidé sous 3 Go (les fenêtres d'import inutilisées depuis 2 s sont alors rendues).
3. *Sauvegarde du cache de pipelines* : toutes les 10 minutes au lieu de chaque minute, et seulement avec 8 Go
   libres ; toujours à la fermeture du jeu.
4. La mémoire validable libre est comptée en Go binaires partout.

`memory_saver = false` garde les anciens comportements des points 1 et 2.

**Mesures** (cinématique d'ouverture puis entraînement, A appuyé toutes les 2 s) : hors pics, 8,8 Go engagés à 30 s
puis 10,2 à 11,6 Go ; un essai de 5 minutes est allé au bout (16 800 images, 0 dessin ignoré, 60 img/s en médiane)
avec deux creux à 2,1 Go libres, grâce au point 2. Composition à l'image 6600 : 5,6 Go de mémoire du jeu, ≈ 2,3 Go de
mémoire vidéo (elle compte dans la mémoire engagée), ≈ 1,5 Go de gros blocs du runtime, 0,26 Go de marge, le reste au
pilote.

**Mémoire vidéo aux changements de niveau : pas de fuite.** Sur l'essai de 5 minutes (plafond du cache à 256 Mo la
plupart du temps) : 2 150 Mo à l'image 2400, maximum 2 396 à l'image 3600, 2 367 à l'image 10800, puis 1 820 Mo après
le chargement de l'entraînement (970 images libérées en tout), 2 029 à la fin. Les 3,3 Go relevés avant ces
corrections comprenaient 1,1 à 1,3 Go d'images sans objet.

**Textures gardées en double.** Sur la console, les données d'une texture servent directement au GPU ; ici elles
restent dans la mémoire du jeu après leur envoi en mémoire vidéo. Mesuré (ligne « texture data kept in game memory »
de la carte) : 1 337 Mo pour 377 textures à l'image 5400, entièrement en pages entières. Rendre ces pages à Windows
ferait donc gagner ≈ 1,3 Go, mais une page rendue revient vide : il faut d'abord savoir si le jeu ou le runtime les
relit (le runtime en refait l'empreinte quand un objet texture reprend la même adresse). Pas dans la 1.0.3 ; à
faire avec une surveillance des relectures (pages protégées, comptage des accès) sur un essai long.

**Le pilote garde-t-il son travail d'un lancement à l'autre ?** Essais de 100 s fermés normalement (`ESSAI_FERMER=1`
dans `essai_long.py` : `WM_CLOSE`, code de sortie 0, `pipelines.cache` écrit à la fermeture), tas relevé toutes les
30 images (`RECOMP_MEM_MAP_FIN=120`). Liaison rapide, deux lancements de suite : même courbe du tas à 10 % près
(≈ 300 Mo à 30 s, 490 à 70 s, 650 à 870 à 100 s) et le compilateur du pilote surpris au travail dans les deux.
Pipelines complets (`RECOMP_NO_GPL=1`), deux lancements de suite : même courbe, `pipelines.cache` passé de 164 à
170 Mo après le premier, et le compilateur encore surpris une fois au second. Le gros pic (après 113 s) n'a pas pu
être comparé : il fait passer la mémoire libre du PC sous le garde-fou des essais.

**Pas résolu.** Empêcher ou déplacer la recompilation du pilote. Pistes : traverser le gros pic deux fois de suite
avec fermeture normale, sur un PC qui a la mémoire pour ; essayer un pipeline complet repris du cache sans
compilation (`FAIL_ON_PIPELINE_COMPILE_REQUIRED`) avant la liaison rapide ; créer pendant la précompilation du
launcher les pipelines déjà vus aux parties précédentes. Ce pic est aussi une piste pour les saccades de l'issue
n° 7, les chutes d'images de l'issue n° 10 et le plantage de la précompilation de l'issue n° 1. Sur ce PC, les
essais ne peuvent plus traverser le pic sans risque tant que 24 Go sont pris par d'autres programmes.

**Textes destinés aux joueurs.** Plus aucun chiffre d'exigence mémoire (launcher, `LISEZMOI.txt`, `README.txt`,
`recomp.ini`) : il sera donné à la sortie comme un minimum mesuré. L'avertissement du launcher conseille de fermer
les programmes lourds et de laisser Windows gérer le fichier d'échange ; il était coupé à l'écran après « Fermez
d'a… » (une ligne de 400 px) et tient maintenant sur deux lignes (`texte_lignes`, vérifié par capture avec
`LANCEUR_STATUT=memoire_faible`).

## Issues GitHub n° 11 et n° 12 : précompilation sur AMD, fenêtre du launcher noire (2026-11-11, pour la 1.0.3)

**N° 12, fenêtre du launcher noire (« black box »), sans matériel ni version donnés.** Une cause est établie par
essai, et confirmée par le joueur le 2026-11-11 : son écran passé sous 270 Hz, le launcher de la 1.0.2 s'affiche
(« This was the problem. Lowered my monitor refresh and it fixed the problem. »).

Le launcher présente une image tous les n balayages de l'écran, n = fréquence / 60 arrondi, pour tourner à environ
60 images par seconde. `IDXGISwapChain::Present` n'accepte que n = 1 à 4. Sur un écran de 270 Hz et plus, n valait 5
ou plus : `Present` rendait `DXGI_ERROR_INVALID_CALL` (0x887A0001) à chaque image, le launcher dessinait dans son
tampon mais rien n'arrivait à l'écran, et la boucle tournait à vide (≈ 1 200 images par seconde, un cœur occupé).
Le calcul est le même dans les versions publiées (1.0.0 à 1.0.2, relu dans le dépôt public). Essai avec un launcher
d'essai (code d'avant le correctif, fréquence simulée, photo de l'écran à l'endroit de la fenêtre) :

| Fréquence | n | `Present` | Écran (moyenne des pixels / 255) |
|---|---|---|---|
| 165 Hz (cet écran) | 3 | réussi | 16,4 |
| 240 Hz simulés | 4 | réussi | 16,2 |
| 300 Hz simulés | 5 | 0x887A0001 à chaque image | 0,1 (noir) |
| 360 Hz simulés | 6 | 0x887A0001 à chaque image | 0,1 (noir) |

Correctif (`dessin.cpp`) : n borné à 4 (90 images par seconde à 360 Hz, 120 à 480 Hz ; les animations suivent
l'horloge, pas le nombre d'images) ; si `Present` échoue quand même, n repasse à 1 et la boucle attend 15 ms. Après
correctif, 300, 360 et 500 Hz simulés : image à l'écran (moyenne 17), aucun échec.

Tout échec d'affichage laisse maintenant une ligne dans `lanceur.log` (`jlog`, exporté par `lanceur.h`), parce que
cette issue est arrivée sans aucune trace :

- au démarrage, `display: <carte>, Direct3D 11.1, screen 165 Hz, one frame every 3 refresh(es)` (avec `(software)` si
  aucun appareil Direct3D 11 matériel n'a pu être créé) ;
- l'étape en échec et son code si l'affichage ne peut pas être créé (`display: … failed (0x…)`) ; le message à l'écran
  est maintenant bilingue (`affichage_indisponible`) et renvoie à `lanceur.log` ;
- `display: drawing failed (EndDraw 0x…)`, `display: Present failed (0x…)`, `display: ResizeBuffers failed`,
  `display: swap chain target not recreated` (ce dernier cas laisse aussi une fenêtre noire).

Au passage : la lueur du titre se dessinait dans sa propre image au milieu de l'image en cours (`BeginDraw` imbriqué),
ce qui faisait échouer la première image (`EndDraw` 0x88990001, sans effet visible). Elle est dessinée avant
`BeginDraw`.

Vérification : `LANCEUR_HZ=<fréquence>` simule l'écran ; le contrôle `launcher` du vérificateur démarre le launcher
d'essai à 360 Hz et refuse tout échec d'affichage dans son journal (la capture `LANCEUR_CAPTURE` seule ne suffit pas :
elle lit le tampon, qui est correct même quand rien n'arrive à l'écran). Les sources du launcher ne font pas partie
de ce que la construction du jeu relit (`DEPENDANCES` de `preparer.py`) : ce correctif ne fait rien reconstruire.

Pour une autre fenêtre noire, sur un écran de moins de 270 Hz, le `lanceur.log` de la 1.0.3 donnera la cause.

**N° 11, plantage pendant la précompilation des shaders sur AMD** (Radeon RX 6700 XT, Ryzen 5 7500F, 1.0.2). D'après
le journal du joueur relu par la session des issues : recompilation, édition de liens et traduction des shaders
réussies, puis `pipelines` s'arrête à `@progres 0.996`, sans `@erreur`. Ce que le code dit de ce journal : seul le fil
principal écrit la progression, et `@progres 1.000` est écrit après la fin de tous les fils et avant l'écriture de
`pipelines.cache`. Un journal arrêté sous 1.000 veut donc dire que le processus est mort pendant la compilation des
derniers shaders (une dizaine restaient), pas pendant l'écriture du cache.

C'est le cas que traite le superviseur de la 1.0.3 (section « Correctifs des issues GitHub n° 1 et n° 4 ») : les
shaders en cours au moment du plantage sont recompilés un par un, le fautif est écarté pour ce pilote, sinon la
compilation reprend sur un seul fil, sinon une erreur est annoncée avec le code du plantage au lieu d'un journal muet.
Essai du 2026-11-11 avec l'exécutable de l'installation d'essai (`scratchpad/lanceur_sync/essai_precompile.py`) :
plantage simulé sur le dernier des 2 388 shaders, donc en fin de précompilation comme chez ce joueur. Neuf shaders
étaient en cours ; le fautif est isolé et écarté en moins d'une seconde, la précompilation finit avec 2 387 shaders,
code 0 ; liste des exclus effacée ensuite, 2 388 shaders. **Limite : jamais essayé sur une carte
AMD, et le plantage réel n'est pas reproduit ici ; c'est au joueur de confirmer avec la 1.0.3.** Son premier
message (`@progres 0.951`, launcher tiers) était très probablement la même étape, et non la recompilation comme je
l'avais d'abord lu.

## Mémoire du jeu : textures retirées de la RAM, shaders à la demande, veille (2026-11-11, pour la 1.0.3)

Demandes de l'utilisateur le 2026-11-11 : réduire la consommation de mémoire (« j'ai presque aucune utilisation de
VRAM mais beaucoup d'utilisation de RAM »), puis « on veut éviter les plantages le plus possible ». Suite des sections
« Mémoire du jeu » et « Pics de mémoire en jeu ».

**Mesures** (cinématique d'ouverture, image 5400, 96 s ; `scratchpad/proto_tex/cmp_essais.py`) :

| Essai | RAM du jeu | Mémoire engagée | Espace du jeu (0x7000000000) | Bloc 0x41dbff0000 |
|---|---|---|---|---|
| sans les deux économies | 7,2 à 8,0 Go | 10,3 à 10,9 Go | 6 185 Mo | 921 Mo |
| textures rendues | 5,8 Go | 9,1 Go | 4 322 Mo | 922 Mo |
| shaders à la demande | 6,7 Go | 9,8 Go | 6 185 Mo | 299 Mo |
| les deux (réglages par défaut) | 4,8 à 5,7 Go | 8,5 à 9,3 Go | 4 322 Mo | 272 à 301 Mo |

Partie de 36 minutes de l'utilisateur sur plusieurs niveaux, avec les deux : 4,0 à 5,4 Go de RAM, 7,7 à 9,5 Go engagés
(11,4 au pire, un pic du pilote), 4,0 Go validables libres au plus bas sur son PC, 0 dessin ignoré, 0 refus de
mémoire, 0,6 % d'images perdues. Sa partie du matin sans elles : 12,06 Go engagés et 8,0 Go de RAM en 3 min 30.

**1. Données des textures rendues à Windows** (`texture_release`, `memory.cpp` : `mem_release` ; `vk_render.cpp` :
`tex_release`, `source_changed`). Sur la console les données d'une texture servent directement au GPU ; ici elles
sont copiées en mémoire vidéo et l'original restait dans la mémoire du jeu. Après l'envoi, les pages entières de la
source sont écrites dans un fichier temporaire (`%TEMP%\recomp_textures_<pid>.tmp`, `FILE_FLAG_DELETE_ON_CLOSE` : il
disparaît même si le jeu est tué ; les restes d'une coupure de courant sont supprimés au démarrage suivant), puis
désengagées. Ensuite :

- *lecture* (par le jeu : 863 fois en 36 minutes, 54 Mo ; par le runtime avant un renvoi au GPU ou une empreinte) :
  `mem_fault` relit les pages du fichier et les réengage avant de reprendre l'instruction, page par page ;
- *écriture*, que la texture ait encore un objet du jeu ou non : même restauration, puis l'écriture
  (`Rendu::vivant`, tenu par `ref_image` et `texture_released`, ne sert plus qu'aux compteurs du journal) ;
- *avant un appel système ou un import Vulkan* (`mem_ensure`) : restauration de la plage ; les pages sous un import
  ne sont jamais rendues (le pilote les verrouille) ;
- *nouvel objet texture sur une image existante* : si les pages rendues n'ont jamais été retouchées
  (`mem_released_intact`, numéro de rendu), le contenu est inchangé par construction ; seuls les octets hors des pages
  entières sont comparés (`edge_hash`), sans rien relire (12 646 fois en 36 minutes).

Le fichier prend ce que le disque du dossier temporaire a de libre au-delà de 4 Go, 6 Go au plus ; sous 5 Go libres au
démarrage, ou sous 2 Go en cours de partie, plus rien n'est rendu (`mem_release_actif`). Mesuré : 2,2 Go de fichier
après 5 minutes, 3,8 Go après 36 minutes ; 0,9 s d'écriture en 5 minutes (≈ 0,3 ms par Mo, sur le fil du rendu).

*Faille corrigée le jour même.* La première version restaurait 64 Ko d'ancien contenu avant toute écriture (morceau
engagé, puis copié). Pendant sa partie de 36 minutes, le jeu a réécrit 13,9 Go de données rendues, par quatre fils à
la fois (225 072 restaurations) : un autre fil pouvait écrire dans le morceau entre son engagement et la copie, et la
copie recouvrait ses nouvelles données par les anciennes. L'utilisateur a signalé de rares artéfacts noirs dans des
effets de fumée et d'explosion ; rien n'établit que c'en était la cause. Depuis : plus aucune copie dans une mémoire
que le jeu réécrit (plages oubliées : 98 à 100 en 5 minutes, 580 Mo, 44 à 66 ms de restauration au lieu de 268), et
restauration page par page pour le reste. Images perdues sur 5 minutes : 2,6 % après, 2,4 % avant.

*Seconde faille, corrigée le même jour (17 h) : les plages oubliées.* Entre 14 h et 17 h, une écriture dans les données
d'une texture sans objet faisait oublier la plage entière, en supposant que le jeu réemployait cette mémoire. Pendant
sa partie de 39 minutes (`partie_utilisateur_2`), l'utilisateur a vu de grands rectangles noirs opaques à la place de
la fumée d'une grosse explosion, et des débris noirs (captures de la fenêtre du jeu : `captures_partie.py`). Ce que le
journal et les essais établissent :

- une texture sans objet n'est pas une mémoire libre : le jeu détruit et recrée ses objets texture sur des données
  restées en place (17 671 recréations sans relecture en 39 minutes) ;
- le jeu déplace des données de textures dans sa mémoire : une fonction lit l'ancien emplacement (`main+0xbb74b10`),
  une autre écrit le nouveau (`main+0xbb74e60`, qui écrit d'abord les 8 derniers octets). Avec l'oubli, la première
  écriture vidait la plage et la suite de la copie lisait des zéros ;
- mesure, oubli retiré (`jeu_auto_1`, 7 minutes de la mission Corneria jouée seule) : 579 relectures (36,2 Mo) de
  données dont une autre partie de la même plage sans objet venait d'être réécrite. Ce sont exactement les octets que
  l'oubli rendait à zéro (compteur `read back after a partial rewrite`).

Correction : plus aucun oubli. Toute écriture restaure d'abord le contenu, page par page, par morceaux de 64 Ko. Coût
mesuré sur ces 7 minutes : 61 090 restaurations à l'écriture (3,8 Go), 4,3 s cumulées, 60 img/s en médiane, 0 dessin
ignoré. `RECOMP_TEX_DROP=1` remet l'oubli (comparaison). **Pas établi** : que les rectangles noirs vus par
l'utilisateur étaient ces textures. La même mission rejouée avec l'oubli (`jeu_auto_ancien`) ne montre aucun rectangle
noir sur Corneria ; le défaut a été vu dans un niveau plus tardif. À confirmer par une partie de l'utilisateur. Sa
première partie du jour (version d'avant 14 h) montrait déjà de rares artéfacts noirs : la fenêtre entre l'engagement
du morceau de 64 Ko et sa copie y laissait lire des zéros à un autre fil ; restaurer page par page l'a réduite à
moins d'une microseconde.

*Limites.* Entre l'engagement d'une page et sa copie (moins d'une microseconde), un autre fil qui y accède sans
fauter lit des zéros ou voit son écriture recouverte ; la fermer demande des vues de fichier (espace d'adressage en
« placeholders »). Sur un PC dont Windows gère le fichier d'échange, ces données froides partaient déjà dans le fichier d'échange :
le gain réel y est plus faible que sur un PC au fichier d'échange fixe.

**2. Shaders à la demande** (`shaders_on_demand`, `ensure_init`). Le jeu créait au démarrage les bibliothèques des
2 388 shaders ; il en utilise 127 en 96 s, 224 en 5 minutes, 560 pipelines en 36 minutes. Quand `pipelines.cache` est
complet pour ce GPU, ce pilote, ces shaders et cet exécutable (`pipelines_a_jour` : la signature de `pipelines.ok`,
écrite par `--precompile`), rien n'est créé au démarrage : chaque bibliothèque l'est au premier dessin, depuis le
cache. Gain : 0,75 Go à la première image (3,64 → 2,89 Go engagés), 0,62 Go à 96 s ; il diminue à mesure que les
shaders servent. Sans cache complet (jeu lancé sans le launcher, pilote changé), tout est créé au démarrage comme
avant. Un shader qui met plus de 50 ms à être préparé au premier usage est noté dans le journal
(`lent_au_premier_usage`).

**3. Veille de la mémoire du PC** (`mem_veille_init`). Le compilateur du pilote prend 2 à 3 Go en deux secondes dans
notre processus ; si Windows lui refuse cette mémoire, il plante sans message et le jeu avec lui (les deux arrêts du
matin). Les protections d'`engager` ne servaient qu'au premier refus fait au jeu lui-même, et l'état de la mémoire
n'était relu par le rendu qu'une fois par seconde. Un fil relit maintenant la mémoire validable libre toutes les
50 ms : sous 2 Go, il rend la marge de 256 Mo et prévient le rendu, qui détruit à l'image suivante les images sans
objet (jusqu'à 1,5 Go) et rend les fenêtres d'import inutilisées. `RECOMP_VEILLE_GO=n` : autre seuil (essais). Non
essayé en conditions réelles : aucun essai n'est descendu sous 2 Go depuis.

**Réglages** (`recomp.ini`, section `[memory]`) : `texture_release` et `shaders_on_demand`, `true` par défaut, sans
effet si `memory_saver = false` (tout revient alors comme en 1.0.2). `RECOMP_TEX_RELEASE`, `RECOMP_SHADERS_ON_DEMAND`
pour les essais. Un `recomp.ini` d'une version précédente reçoit à la fin les réglages apparus depuis, avec leur
explication ; ses lignes ne sont pas touchées (`load_file`).

**Essais** (copie d'essai, `scratchpad/longs/`) : `int_sans` et `int3_defaut` (100 s, interrupteurs coupés puis par
défaut : mêmes images, −1,7 Go engagés) ; `int4_5min` (5 minutes, fermeture normale, 0 dessin ignoré) ;
`partie_utilisateur_1` (36 minutes jouées par l'utilisateur, avant la correction de la faille) ; précompilation avec
plantage simulé inchangée ; `partie_utilisateur_2` (39 minutes, avec l'oubli : rectangles noirs) ; `restaure_1`
(200 s, oubli retiré) ; `jeu_auto_1` et `jeu_auto_ancien` (mission jouée seule, sans puis avec l'oubli). Journal :
lignes `texture data released: … MB now`, `restored: … write faults on unused textures`, `read back after a partial
rewrite`, `shader libraries created at first use so far`.

*Faire jouer une mission sans joueur* (trouvé le 2026-11-11) : `essai_long.py` avec `ESSAI_SAUVEGARDE=<dossier>`
pointant une COPIE d'une sauvegarde dont la campagne est terminée. L'appui automatique sur A passe le bilan, lance
une nouvelle campagne à un joueur et tire pendant la mission Corneria, qui est sur rails : elle va jusqu'au boss et au
rapport de mission en 6 minutes. C'est le seul essai automatique qui couvre le jeu réel (flux de textures,
explosions) ; la cinématique d'ouverture ne déclenche pas le déplacement de textures (compteur à 0 en 200 s).

**Piste notée pour une version suivante** (demande de l'utilisateur : un ramasse-miettes) : brancher le moteur sur
les fonctions d'allocation du jeu pour connaître les blocs libres de ses réserves après un déchargement de niveau, et
rendre leurs pages à Windows. Gain inconnu : mesurer d'abord la part libre des 5,6 à 7,3 Go de mémoire du jeu.

## Cartes AMD : forme des shaders, précompilation protégée, essais sur la carte intégrée (2026-11-11, pour la 1.0.3)

Demande de l'utilisateur : « est-ce que tu as pu améliorer le support des cartes AMD ? ». Jusqu'ici rien n'avait pu
être essayé sur AMD. Ce PC a pourtant une carte AMD que Vulkan voit : la carte intégrée du Ryzen 7 9700X
(« AMD Radeon(TM) Graphics », pilote AMD 26.3.1). `RECOMP_GPU=<partie du nom>` la choisit à la place de la carte
dédiée (`init`, lignes `GPU available:` du journal) ; `scratchpad/integration/essai_amd.py` y précompile les shaders
en mettant de côté les caches de la carte habituelle.

**Ce que la carte AMD montrait avec les shaders de la 1.0.2** (1080p natif) : écran titre à 30 images par seconde avec
une grande zone grise à la place du logo ; scènes 3D à 0,1 image par seconde, image fausse (zones blanches, damier).
C'est le profil des issues n° 6 (RX 7800 XT : 9,5 à 12 puis 1,5 img/s) et n° 7 (RX 7900 XTX : 18 à 23 puis 5,6 img/s).

**Cause établie par essai : la forme des shaders traduits.** `glsl.py` écrivait chaque shader comme une boucle de
répartition, `while (true) { switch (pc) { case N: … pc = M; continue; … } }`. Le compilateur de NVIDIA retrouve le vrai
flot de contrôle derrière l'aiguillage ; celui d'AMD non, et chaque pixel exécutait la boucle et l'aiguillage. Les
shaders sont maintenant écrits en chaîne de blocs gardés (`en_chaine`) :
`if (pc == N) do { … pc = M; break; … } while (false);`, dans l'ordre des adresses. Sans saut arrière ni saut calculé
(1 953 shaders sur 2 388), une seule passe suffit : plus de boucle ni d'aiguillage. Sinon (435 shaders) la chaîne est
répétée, un saut arrière prenant effet à la passe suivante. `RECOMP_SHADER_SWITCH=1` redonne l'ancienne forme.

| Mesure | Ancienne forme | Chaîne |
|---|---|---|
| AMD intégrée, 1080p, écran titre | 30 img/s | 35 img/s |
| AMD intégrée, 1080p, scènes 3D de la cinématique | 0,1 img/s | 2,5 à 13 img/s |
| AMD, compilation des 2 387 shaders (cache vide) | plus de 4 min | 53 s |
| NVIDIA, précompilation (cache vide) | 26 s | 27 s |
| NVIDIA, 100 s de jeu : images perdues sur ≈ 4 700 | 122 | 129 |
| NVIDIA, mémoire engagée à 96 s | 9,26 Go | 9,05 Go |

Sur NVIDIA les instantanés montrent les mêmes scènes, correctement rendues. Le traducteur modifié produit, octet pour
octet, les 2 388 fichiers GLSL essayés (d'abord obtenus par une réécriture de texte, `scratchpad/…/chaine.py`).
Jamais essayé sur une carte AMD dédiée : le gain y est attendu mais pas mesuré.

**Lire ces chiffres** (rappel de l'utilisateur) : la carte intégrée est très peu puissante. Ses 2,5 à 13 img/s sont
sa limite à elle, pas celle du correctif. Ce qui se transpose aux cartes des joueurs est le rapport (25 à 130 fois
plus d'images dans les scènes 3D), pas le chiffre : leurs cartes dédiées étaient à 1,5 et 5,6 img/s avec les anciens
shaders, là où celle-ci était à 0,1. Rien n'est mesuré sur une carte dédiée ; ne pas citer 2,5 à 13 img/s aux joueurs.

**Défauts d'image sur AMD : deux causes, trouvées et corrigées le 2026-11-11 au soir** (demande de l'utilisateur : « il
faut corriger ça »). Les défauts notés l'après-midi (bandes noires de la cinématique en gris, « taches blanches »,
zone grise à la place du logo de l'écran titre, sous-titres dédoublés) n'en faisaient qu'un ; l'utilisateur en a
signalé un second pendant la recherche : « un maillage apparaît sur plus ou moins toutes les textures et les
écritures ».

1. *Le noir rendu en gris moyen.* Le gris valait exactement 188 en sRGB, soit 0,5 en linéaire : la valeur neutre que
   `image_to_general` met dans une image neuve. Le shader de fin de chaîne (`fxaa_luma`, fragment
   `00918579fe4c058f`) passe la couleur par une table 3D de 64³ (`scene_lut_generation.comp.glsl`, calcul
   `434a7989da7958c5`, relancé à chaque changement d'étalonnage). Table relue sur AMD (`RECOMP_TEX3D_DUMP`) : 2 944 à
   3 072 cases encore à (0,5 0,5 0,5 1), en x 0..47, y 0..7, z 0..7, c'est-à-dire les six premiers groupes de travail
   du calcul, le coin des couleurs sombres. Cause : après l'effacement neutre (`vkCmdClearColorImage`), aucune
   barrière. Rien n'ordonnait donc l'effacement et le calcul qui écrit l'image dans la même soumission ; le pilote AMD
   laissait l'effacement finir après les premières écritures et les recouvrait. NVIDIA exécute dans l'ordre.
   Correction : `barrier_all()` après cet effacement (et après celui des images factices). Après : 0 case neutre, case
   (0, 0, 0) à 0, fondus et bandes noirs, écran titre sans zone grise, plus de « taches blanches ».
2. *Le maillage.* En pleine résolution (`RECOMP_SNAPSHOT_FULL`), le contour et la diagonale du rectangle de chaque
   lettre ressortent, par carrés de 2×2 pixels. Les shaders de fragments du jeu calculent leurs dérivées d'écran en
   échangeant une valeur avec le pixel voisin du même carré (`SHFL.BFLY` 1 ou 2), puis en la soustrayant selon la voie
   du pixel (`FSWZADD`) : 295 shaders, 3 658 échanges. Le traducteur l'écrivait avec les sous-groupes de Vulkan
   (`subgroupShuffleXor`, `gl_SubgroupInvocationID & 3`). Sur NVIDIA le sous-groupe d'un shader de fragments suit les
   carrés, pixels auxiliaires compris ; sur AMD un pixel auxiliaire (hors du triangle) n'est pas une voie active, et
   la valeur échangée au bord de chaque triangle est indéfinie. Mesuré sur NVIDIA par deux shaders-sondes : le voisin
   « 1 » est en x ± 1, le voisin « 2 » en y ± 1, la voie vaut parité(x) | parité(y) << 1. Nouvelle écriture
   (`glsl.py`, `QUAD_AIDES`) : voisin = v ± `dFdxFine(v)` ou v ± `dFdyFine(v)` selon la parité du pixel, voie par la
   parité. Sonde d'équivalence sur NVIDIA (texte du titre) : 0 pixel de valeur différente, 0 voie différente. Sur AMD :
   plus de maillage sur le texte. `RECOMP_SHADER_QUAD_SUBGROUP=1` à la traduction remet l'ancienne écriture.
3. *Le maillage restant sur les objets 3D* (signalé par l'utilisateur après le point 2 : « sur AMD les maillages sont
   toujours présents sur les textures »). 251 de ces shaders font aussi voter les pixels (`VOTE.ALL` → `subgroupAll`)
   avant de sauter un bloc : sur la console, si un seul pixel du groupe a besoin du bloc, tous l'exécutent, pixels
   auxiliaires compris, pour que le niveau de détail des textures lues dans le bloc reste juste. Sur AMD un pixel
   auxiliaire ne vote pas et suit son propre chemin : au bord de chaque triangle, les coordonnées de ses voisins
   n'étaient pas calculées, et les arêtes ressortaient en pointillés (socle de la salle du général Pepper, coque de
   l'Arwing). J'avais d'abord écrit que ces votes étaient sans conséquence : c'était faux. Correction :
   `VK_KHR_shader_quad_control`, offert par les deux pilotes. Les 295 shaders de fragments qui votent ou échangent sont
   traduits avec `layout(full_quads) in;` (les pixels auxiliaires participent aux opérations de sous-groupe) et
   `layout(quad_derivatives) in;` (`glsl.py`, `QUAD_CONTROL`). Le moteur active l'extension quand le pilote l'offre,
   sauf sur NVIDIA où les sous-groupes suivent déjà les carrés et où le rendu est validé sans ; quand elle n'est pas
   active, il retire ces marques du SPIR-V au chargement (`spirv_sans_quad_control` : capacité 5087, extension, modes
   d'exécution 5088 et 5089), ce qui redonne le module d'avant. `RECOMP_QUAD_CONTROL=0` ou `1` force l'un ou l'autre ;
   `RECOMP_SHADER_NO_QUAD_CONTROL=1` traduit sans ces marques. Après : socle et coque identiques à NVIDIA sur les
   captures en pleine résolution (`amd_qc4`, `nv_plein`). Un pilote sans cette extension (ancien pilote AMD, Intel non
   essayé) garde le défaut du point 3.
4. *Calculs : groupes de 32 voies.* Six shaders de calcul (dont `terrain.clutter_generate` et `terrain.clutter_cull`)
   votent, comptent et échangent entre les 32 voies d'un « warp » de la console, avec des masques de 32 bits. NVIDIA a
   des sous-groupes de 32 ; AMD de 64 par défaut, 32 à 64 permis en calcul. Le moteur impose 32
   (`VkPipelineShaderStageRequiredSubgroupSizeCreateInfo`, `Vk::sg32`, `RECOMP_NO_SG32=1` pour comparer) quand la carte
   en prend d'autres par défaut. **Pas vérifié par l'image** : ces calculs servent surtout en mission (herbe et
   cailloux du terrain), que la carte intégrée n'atteint pas en un temps raisonnable ; vérifié seulement que la
   précompilation et la cinématique passent avec.

*Hypothèses écartées par essai*, à ne pas reprendre : minimum, maximum et saturation qui laissent passer un NaN sur
AMD (shaders retraduits avec des versions sûres : image inchangée, modification retirée) ; niveau de détail
automatique des textures dans du code conditionnel (niveau 0 imposé : inchangé) ; `gl_FragCoord.w` des pixels
auxiliaires (remplacé par 1 : inchangé) ; format de profondeur D24S8 absent d'AMD (le jeu emploie les formats 52 et
54, présents) ; échantillonneur de la table (bord fixe, linéaire : correct).

*Méthode qui a marché* : `RECOMP_RT_SNAPSHOT=N` pour trouver la passe où le défaut naît, `RECOMP_VK_DEBUG=N` (qui écrit
maintenant l'empreinte des shaders de chaque dessin), puis des shaders modifiés à la main dans le cache du jeu d'essai
avec une couleur témoin (scripts `essai_glsl*.py` du dossier de travail ; tests sur les bits pour qu'aucun
compilateur ne les supprime). Nouveaux diagnostics : `RECOMP_TEX3D_DUMP=N` (contenu brut des textures 3D),
`RECOMP_SNAPSHOT_FULL=1` (instantanés non réduits : le maillage était invisible sur les instantanés à demi-taille),
`RECOMP_3D_LOG=1` (qui écrit dans les textures 3D).

Ce qui reste vrai : rien n'est essayé sur une carte AMD dédiée, et le shader géant ci-dessous est écarté sur AMD.

**Le plus gros shader du jeu** (calcul `f35f4f42e2496a38`, 2,7 Mo de SPIR-V, 711 blocs, 18 844 lignes de GLSL) : le
compilateur d'AMD dépasse 11,5 Go de mémoire en 14 à 36 s, quelle que soit l'écriture (aiguillage, 711 fonctions avec
registres globaux, fonctions marquées DontInline, chaîne). Celui de NVIDIA le compile en 9 s avec 0,8 Go. Sur un PC de
32 Go chargé, c'est le plantage 0xC0000005 de l'issue n° 11 : son journal s'arrête à `@progres 0.951` puis `0.996`
parce que seul le fil principal écrit la progression. Ce shader ne sert ni dans la cinématique d'ouverture ni dans
l'entraînement (5 minutes avec ce shader écarté : jamais demandé) ; ce n'est donc pas lui qui cause le pic de 2 Go du
pilote NVIDIA.

**Précompilation protégée** (`precompile_enfant`, `precompile_superviseur`). Le superviseur surveille dix fois par
seconde la mémoire du processus de compilation : au-delà du plafond (8 Go pour un shader seul, 16 Go tous fils
confondus ; `RECOMP_PRECOMPILE_MAX_MO` pour les essais), ou quand le PC a moins de 2 Go validables libres, il l'arrête
avant que le compilateur ne plante. Les shaders en cours sont alors recompilés un par un sous le plafond d'un shader
seul ; celui qui le dépasse est écarté pour ce pilote (`shaders_exclus.txt`, « its compiler needs too much memory ») et
la précompilation reprend. Si le PC manque de mémoire même sans le compilateur : erreur claire (« close other
programs »), aucun shader écarté. Essai sur le pilote AMD, plafond à 8 Go : arrêt à 7,8 Go après 31 s, shader isolé et
écarté, reprise, 2 387 shaders prêts, 64 s en tout, 7,4 Go libres au plus bas. Sur NVIDIA : plantage simulé isolé
comme avant, précompilation normale inchangée.

**Vu au passage, à vérifier** : avec `native_resolution = false` (« Comme la console »), l'image est restée grise
pendant un essai sur AMD : le shader de calcul `059b307b6448e8a1`, créé par le jeu et jamais traduit, manquait
(`draw skipped (missing compute shader)`). Pas encore essayé sur NVIDIA avec ce réglage.

## Reconstruction imposée après une mise à jour, désinstallation (2026-11-11, pour la 1.0.3)

Demandes de l'utilisateur : « lorsque l'on publie une mise à jour qui nécessite la reconstruction, elle doit être
forcée » et « rajoute des routines de nettoyage plus simples pour la désinstallation ».

**Changement du 2026-11-11 au soir : la reconstruction est proposée, plus imposée.** Dernière demande de
l'utilisateur : « je veux que tu laisses le choix au joueur de reconstruire le jeu ou pas lorsque l'on met à jour des
éléments qui la nécessitent, avec une mention : certaines améliorations nécessitent la reconstruction du jeu ». Ce
qui vaut maintenant :

- le launcher détecte toujours les étapes à refaire (point 1 ci-dessous), mais le bouton principal reste JOUER et
  lance le jeu actuel ; l'accueil affiche « Certaines améliorations nécessitent la reconstruction du jeu. » et le
  lien « RECONSTRUIRE MAINTENANT › » (`Z_MAJ_JEU`, seules les étapes changées) ;
- la question à la première ouverture d'une nouvelle version (point 2) porte cette mention ; « non » laisse jouer ;
- l'exécutable ne refuse plus de démarrer (point 3) : `version_du_jeu_a_jour` écrit seulement dans le journal du jeu
  « some improvements require rebuilding the game », ce qu'un rapport de bug montrera. Plus de code de sortie 5, plus
  de `RECOMP_NO_VERSION_CHECK` ni de `RECOMP_SANS_DIALOGUE`.

Conséquence à garder en tête pour le suivi des issues : un joueur peut tourner avec un jeu plus ancien que son
launcher. La première ligne utile de son `recomp_run.log` le dit. Un jeu construit par la 1.0.2 garde ses sauvegardes
dans `recomp\build\full\save` tant qu'il n'est pas reconstruit.

Essai : `verif/essai_choix_maj.py`, 24 contrôles (refus : rien ne démarre, la reconstruction reste proposée) ; capture
de l'accueil dans cet état vérifiée à l'œil. Ce qui suit décrit la première version (reconstruction imposée) ; les
points 1 et 2 restent vrais pour la détection et la question.

Deux conséquences du choix, traitées le même soir :

- *Launcher 1.0.3 avec un jeu construit par la 1.0.2.* Ce cas n'existait pas tant que la reconstruction était
  imposée. Vérifié par lecture des sources de la 1.0.2 (archive de mise à jour gardée par le vérificateur), pas par
  un essai avec un vrai exécutable 1.0.2 : `--precompile` existe déjà, les clés inconnues de `recomp.ini`
  (`memory_saver`, `texture_release`…) sont notées dans le journal et ignorées, `RECOMP_VOLUME` est le même, et le
  launcher lance le jeu même si la précompilation échoue. La reprise des sauvegardes vers `%APPDATA%` est faite par
  l'exécutable 1.0.3 à son premier lancement (copie, l'ancien dossier reste) : rien ne déplace les sauvegardes d'un
  jeu resté en 1.0.2.
- *Reconstruction annulée en cours.* `link.py` écrivait directement `recomp_main.exe` : l'éditeur de liens vide le
  fichier dès qu'il l'ouvre, donc une annulation pendant l'édition de liens laissait un exécutable vide ou tronqué,
  avec un bouton JOUER désormais actif. `link.py` écrit maintenant `recomp_main.part.exe` puis le met en place d'un
  coup (`os.replace`). Essai sur l'installation d'essai (arbre de processus tué comme le fait l'annulation du
  launcher) : arrêt à l'ouverture du fichier (0 octet), en pleine écriture (841 Mo sur 855) et après l'écriture avant
  la mise en place : l'exécutable précédent garde chaque fois la même empreinte SHA-256. Le fichier partiel reste
  sur le disque jusqu'à l'édition de liens suivante, qui le supprime. Une annulation pendant l'étape des shaders
  laisse un mélange d'anciens et de nouveaux `.spv`, que l'exécutable 1.0.3 accepte (il retire lui-même les marques
  de `quad_control` quand le pilote ne les prend pas).

**Reconstruction imposée (première version).** Trois verrous, du plus large au plus étroit :

1. *Signatures des étapes* : `preparer.py --a-refaire --copie <copie>` écrit `@a_refaire recompiler,lier,…` (les étapes
   dont la signature a changé : code dont elles dépendent, copie du jeu) et ne fait rien d'autre (0,2 s). Le launcher
   le demande à chaque démarrage (`verifier_reconstruction`, message `WM_REFAIRE`) : s'il reste une étape à refaire, le
   jeu est « à mettre à jour », le bouton principal devient METTRE À JOUR et lance la construction, jamais le jeu.
   Jusque-là seul le numéro de version (`.preparation\version.ok` ≠ `recomp\VERSION`) rendait le jeu obsolète : des
   sources changées sans changement de numéro laissaient jouer l'ancien exécutable.
2. *Question au joueur* : à la première ouverture d'une nouvelle version du launcher (`version_vue` dans
   `lanceur.ini` ; mise à jour installée par `maj.py`, ou paquet remplacé à la main), si le jeu a été construit par une
   autre version, le launcher propose de le mettre à jour tout de suite (`maj_jeu_question`). « Non » : rien ne
   démarre, le bouton reste METTRE À JOUR et le jeu ne se lance pas avant. Seules les étapes changées sont refaites.
   (La première version de ce point démarrait la mise à jour d'office ; l'utilisateur a demandé le même jour de
   laisser le choix.)
3. *Exécutable* : `recomp_main.exe` refuse de démarrer quand `version.ok` et `VERSION` existent et diffèrent
   (`version_du_jeu_a_jour`, `host_main.cpp` : message, code de sortie 5). Le launcher ne lance jamais un tel jeu ; ceci
   ferme la porte du double-clic. Exemptions : `RECOMP_CONSTRUCTION` (mis par `preparer.py`, qui lance le jeu pendant
   la construction, avant d'avoir noté la version), `RECOMP_NO_VERSION_CHECK=1` (essais), fichier absent (exécutable
   hors d'une installation). `RECOMP_SANS_DIALOGUE=1` retire la boîte de message (essais).

**Les mises à jour du paquet ne sont jamais installées d'office** (demande de l'utilisateur : « ne rends pas les
mises à jour du launcher obligatoires, mets un prompt qui laisse le choix »). Quand `maj.py --verifier` trouve une
version plus récente, le launcher pose la question (`WM_MAJ_DISPO`, `maj_question`) : « oui » l'installe ; « non » est
retenu pour cette version (`maj_refusee` dans `lanceur.ini`) et le bandeau « MISE À JOUR … DISPONIBLE » reste là pour
plus tard. Depuis le soir du 2026-11-11, plus rien n'est imposé : ni l'installation d'une mise à jour, ni la
reconstruction du jeu qu'elle demande. Les deux questions ne s'affichent jamais l'une sur l'autre : si une question est déjà à l'écran, celle de
la nouvelle version attend son tour (`MINUTEUR_MAJ`). Essai : `verif/essai_choix_maj.py` (faux paquet jetable, vrai
launcher sans crochet d'essai, vraies boîtes de dialogue auxquelles le script répond oui ou non : 24 contrôles),
branché dans le contrôle `launcher` du vérificateur.

Essais : dans l'installation d'essai, sources intactes → « game ready », bouton JOUER ; une ligne ajoutée à
`runtime/os/settings.cpp` → `game to update before playing, steps to redo: lier`, bouton METTRE À JOUR
(`scratchpad/integration/essai_lanceur_refaire.py`). Marque de version changée en 1.0.2 → le jeu sort avec le code 5.

**Désinstallation.** `recomp/launcher/desinstaller.py` retire tout ce que le paquet a écrit sur le PC :

| Élément | Emplacement |
|---|---|
| jeu construit, shaders, journaux | `recomp\build` |
| code C++ généré | `recomp\out` |
| fichiers extraits de la copie du jeu | `extracted`, `work` |
| copie du compilateur | `%ProgramData%\StarFoxRecomp` |
| fichier temporaire d'une partie interrompue | `%TEMP%\recomp_textures_*.tmp` |
| le paquet | le dossier d'installation |
| sauvegardes et copies de secours | `%APPDATA%\SpaceAnimalsRecomp`, seulement avec `--sauvegardes` |

- `--lister` (rien n'est supprimé), `--nettoyer` (la place revient : jeu construit, code généré, extraits, copie du
  compilateur ; le launcher, `recomp.ini` et les sauvegardes restent), `--desinstaller` (tout).
- Jamais supprimée : la copie du jeu du joueur (`.nup`, `.nsp`, `.xci`, `.nsz`), même posée dans le dossier
  d'installation ; ce dossier reste alors, vidé du reste.
- Avant toute suppression : copie de secours des sauvegardes (`sauvegardes.secours`), et reprise dans
  `%APPDATA%\SpaceAnimalsRecomp\save` d'une sauvegarde restée dans `recomp\build\full\save` (jusqu'à la 1.0.2).
- Refus dans un dossier de sources (`.git`, `recomp\launcher\lanceur.cpp`, `recomp\verif`, `recomp\depot`) et si le
  jeu ou le launcher de cette installation tourne.
- Le Python du paquet ne peut pas se supprimer lui-même : PowerShell finit après sa sortie, et ne touche qu'à des
  dossiers de l'installation (le script peut aussi être lancé par le Python du système).
- Dans le launcher : bouton DÉSINSTALLER de la page CONSTRUCTION (paquet installé seulement), deux questions (la
  seconde pour les sauvegardes, « non » par défaut), puis le launcher se ferme. Sans le launcher : `uninstall.bat`
  à la racine du paquet.
- Journal : `%TEMP%\SpaceAnimalsRecomp-uninstall.log`.

Vérification : contrôle `desinstaller` (`verif/essai_desinstaller.py`, 7 scénarios sur de fausses installations avec
`APPDATA`, `ProgramData` et `TEMP` d'essai) ; le contrôle `paquet` exige `desinstaller.py` et `uninstall.bat`.

## Chien de garde du jeu : compteur arrêté pendant une longue compilation (issue n° 13, 2026-11-11, pour la 1.0.3)

**Le rapport.** RX 6800 XT, paquet 1.0.2 : fenêtre noire au démarrage, puis arrêt. Dans `recomp_run.log` : cache de
pipelines chargé à 4,7 s, plus rien, puis à 78,4 s « vwatchdog.c(98): error: game-0-watchdog: System vgame_sim appears
dead: last reported in 74282 ms ago », `svcBreak` depuis `nn::diag::detail::Abort` et PANIC.

**La cause.** Le moteur du jeu a son propre chien de garde : si sa simulation ne donne plus signe de vie pendant
environ 74 s, il arrête le jeu. Sur la console rien ne bloque aussi longtemps. Ici, la première initialisation de
Vulkan prépare les shaders (`warm_up`) pendant que la simulation attend ; sur NVIDIA c'est une à deux secondes depuis
le cache, sur AMD beaucoup plus (15,2 s sur la 7900 XTX de l'issue n° 7 avec un cache complet), et une vraie
compilation dure des minutes. Au-delà de 74 s, le jeu se tue lui-même. Ce n'est pas un plantage du pilote.

**Reproduit** sur NVIDIA avec `RECOMP_STALL_TEST=90` (blocage simulé de 90 s au même endroit) et
`RECOMP_NO_CLOCK_PAUSE=1` : arrêt à 77,4 s avec le même message (« last reported in 74548 ms ago »), même pile.

**Le correctif** (`runtime/runtime.cpp`, `rt_stall_begin` / `rt_stall_end` ; `vk_render.cpp`, structure `Blocage`). Le
chien de garde mesure le temps avec le compteur invité (`rt_cntvct`). Pendant que le pilote crée un pipeline ou une
bibliothèque de shaders, et pendant toute la préparation du démarrage, ce compteur avance normalement 5 s, puis
s'arrête ; à la fin du blocage il reprend d'où il s'était arrêté. Pour le jeu, un blocage dure donc 5 s au plus. Une
saccade ordinaire (moins de 5 s) ne change rien : le comportement en jeu est le même qu'avant. Lecture du compteur sans
verrou (compteur de séquence), parce que le jeu le lit très souvent.

Avec le correctif et le même blocage de 90 s : première image à 94 s, 2 400 images à 60 img/s, pas d'arrêt.

**À ne pas refaire** : j'avais d'abord déclaré ces deux fonctions dans `runtime/cpu.h`. Cet en-tête entre dans
l'empreinte de chaque objet du jeu recompilé (`build.py`, `GEN_HEADERS`) : y changer une ligne fait recompiler les 902
lots chez chaque joueur (21 à 66 minutes). Remis à l'identique (comparé octet par octet à celui de la 1.0.2 publiée) ;
les déclarations sont en tête de `vk_render.cpp`. `cpu.h` et `simd.h` ne se modifient que si c'est indispensable.

**Ce que ça ne règle pas.** Le joueur attend toujours pendant la préparation, il ne voit plus le jeu s'arrêter. En
1.0.3 le launcher précompile avant de lancer et le jeu ne prépare plus tout au démarrage quand le cache est complet
(`shaders_on_demand`) : l'attente ne revient que si la précompilation n'a pas abouti. Pas essayé sur une RX 6800 XT.
Un shader préparé au premier usage en pleine partie est couvert de la même façon.

## Textures périmées : images sans objet encore dessinées (2026-11-11 au soir, pour la 1.0.3)

**Le défaut.** Vu par l'utilisateur : dans la cinématique de fin de Corneria, des dessins de la coque de l'Arwing
(bandes blanches, traits orange, barre jaune) sur le front de Slippy. Revu sur NVIDIA dans une partie jouée sans
joueur ; absent de la cinématique d'ouverture.

**Ce qui est mesuré.** Sur la console, le GPU lit les données d'une texture dans la mémoire du jeu. Ici elles sont
copiées en mémoire vidéo au premier usage, puis l'image est gardée. Le jeu détruit ses objets texture et réemploie
leur mémoire pour d'autres textures, mais continue de dessiner avec des descripteurs qui mènent à l'image de l'objet
détruit (compteur `refs` à 0) : cette image n'était jamais recomparée à la mémoire. Diagnostic `RECOMP_TEX_AUDIT=n`
(toutes les n images, la mémoire source de chaque texture en service est comparée à ce qui a été envoyé) : toutes les
textures périmées trouvées en trois parties sont des images sans objet, aucune n'a d'objet vivant ; 34 d'un coup au
passage du menu à la cinématique de départ (masques de 1 696 × 1 696 et voisins).

**Le correctif** (`vk_render.cpp`, `flush_recheck`, appelé à la liaison d'une texture, une fois par image affichée) :
une image sans objet vivant qui est encore dessinée est comparée à la mémoire, et renvoyée au GPU si elle a changé.
Source rendue à Windows (`texture_release`) : comparée seulement si ses pages ont été retouchées, ce qui ne coûte qu'une
recherche. Sinon (petite texture, `memory_saver` coupé) : empreinte refaite toutes les 30 images, étalée selon
l'adresse. Mesuré sur trois parties automatiques : 5 à 7 renvois par partie (textures de 2 048 × 2 048 surtout), 106 à
157 contrôles périodiques, 60 img/s en médiane, aucun dessin ignoré.
`RECOMP_NO_FLUSH_CHECK=1` : ancien comportement. `RECOMP_FLUSH_LOG=1` : chaque renvoi est noté dans le journal.

Ajouté en même temps : `nvnMemoryPoolFlushMappedRange` n'est plus ignoré (`pool_flushed`, registre des plages vidées),
une texture dont la source est touchée par un vidage est recomparée. Ce jeu ne s'en sert presque pas (un appel par
partie) : sans effet mesuré, gardé parce que c'est le signal prévu par NVN.

**Ce qui n'est PAS vérifié.** Le plan de Slippy n'a pas été revu avec ce correctif : les deux parties automatiques
faites avec lui se sont terminées par un game over dans la ville, avant la cinématique (le joueur automatique prend un
chemin au hasard), et une troisième a été arrêtée par le garde-fou de mémoire du PC. Ce que j'ai : une partie avec le
diagnostic en mode « renvoi forcé » (`RECOMP_TEX_AUDIT_FIX=1`), où le visage est propre (capture à l'image 27 120), et
une partie plus ancienne sans rien, où le défaut est là. Le lien entre le défaut et les images sans objet est donc
probable, pas démontré : je ne sais pas laquelle des textures renvoyées est celle du front. L'utilisateur a choisi de
considérer que ça marche pour l'instant. À revoir à l'œil à la prochaine partie ; si le défaut revient, relancer avec
`RECOMP_TEX_AUDIT=30 RECOMP_FLUSH_LOG=1` et lire les lignes « STALE » autour du plan.

**Piste écartée.** Données réécrites sous un objet texture vivant (niveaux de détail chargés après coup) : l'audit n'en
a trouvé aucune.

## Launcher : fenêtre agrandie, plus rien ne s'ouvre pendant la construction (2026-11-11 au soir, pour la 1.0.3)

Demandes de l'utilisateur : « la vidéo du launcher est publiée dans le GitHub, je ne veux pas que le premier lancement
serve à enregistrer la cinématique, ça peut être perturbant pour un utilisateur lambda » et « agrandis un peu la
fenêtre du launcher ».

- **Extraits du fond.** Une construction (la première comme les suivantes) ne lance plus jamais le jeu pour les
  enregistrer. Puis, à 23 h, l'utilisateur a tranché : « il n'y a pas de réenregistrement, le launcher utilise la
  vidéo du repo, rien de plus ». L'étape `extraits` de `preparer.py`, le bouton « RÉENREGISTRER LES EXTRAITS » des
  Réglages et leurs textes sont retirés : plus rien ne crée, ne remplace ni n'efface ces fichiers. `--etapes …,extraits`
  et `--sans-extraits` (anciens launchers) sont acceptés et ignorés. Sans extraits, le launcher montre son fond sans
  vidéo. L'enregistreur du runtime (`RECOMP_RECORD_DIR`, `record.cpp`) reste, comme outil de développement.
- **Relevé des shaders.** Pendant la première construction, le jeu tournait une quinzaine de secondes dans une fenêtre
  pour relever les shaders qu'il crée au démarrage : c'est ce qu'un joueur voyait s'ouvrir tout seul. Il tourne
  maintenant sans fenêtre visible (`RECOMP_WINDOW_HIDDEN=1`, `present.cpp` : la fenêtre existe pour Vulkan mais n'est
  jamais montrée, ni dans la barre des tâches). Essai : 1 200 images à 60 img/s sans fenêtre ; vraie étape `pipelines`
  sur l'installation d'essai, relevé fait en 14 s. Textes du launcher et du LISEZMOI en accord.
- **Fenêtre.** 1 200 × 720 à 100 % au lieu de 1 000 × 600 (`ECHELLE = 1,2` dans `lanceur.h`). Le dessin et les clics
  restent dans le repère logique de 1 000 × 600 : c'est l'échelle d'affichage `g.dpi` qui est multipliée
  (`dpi_fenetre`), donc rien n'est à remettre en page et le texte reste net. Sur un écran trop petit, l'échelle est
  réduite pour que la fenêtre tienne (jusqu'à 0,75). Captures de l'accueil et des Réglages vérifiées à l'œil.
- Sous « Fond animé », « tirés de votre copie » devient « fournis avec le launcher ».

- **Écritures.** Toutes les tailles de police passent par `format()` dans `dessin.cpp`, multipliées par
  `TAILLE_TEXTE = 0,9` (demande : « réduis un peu la taille des écritures »). La mise en page ne bouge pas.
- **Icône du launcher.** La fenêtre prenait l'icône du jeu une fois le jeu construit (`icone_jeu`) : retiré, le
  launcher garde la sienne partout (demande : « elle affiche l'icône du jeu au lieu de l'icône du launcher »).
- **Nom du jeu pour Windows.** `link.py` ajoute des informations de version à `recomp_main.exe` : description
  « StarFox Remake », produit « StarFox Remake [recompiled] », version des sources. Le gestionnaire des tâches et le
  mélangeur de volume affichent ce nom au lieu de « recomp_main » ; le fichier garde son nom. Vérifié : description lue
  par Windows sur le fichier et sur le processus en marche.

**Incident du même soir.** L'ancien bouton « RÉENREGISTRER LES EXTRAITS » effaçait les extraits avant d'enregistrer :
fenêtre du jeu fermée à 21 h 26, plus aucun extrait, `paquet.py` en échec (« aucun extrait »). Les six fichiers publiés
ont été remis à l'identique (SHA-256). Le bouton n'existe plus.

À savoir : le contrôle `launcher` a échoué une fois sur un PC chargé (une question de mise à jour pas apparue à temps,
19 contrôles sur 24), puis est repassé à 24 sur 24 seul. Sensible à la charge, pas un défaut du launcher.

## Touches du clavier modifiables depuis le launcher (2026-11-11 au soir, pour la 1.0.3)

Demande de l'utilisateur : « fais en sorte qu'on puisse changer les contrôles depuis le launcher, avec 2 mappages par
défaut (un en WASD (QWERTY) et un en ZQSD (AZERTY)) ».

**Le jeu** (`runtime/os/settings.h`, `settings.cpp`, `input.cpp`). Une touche par action de la manette : 12 boutons, 4
directions de la croix, 4 directions et le clic de chaque stick, soit 24 touches (`Settings::keys`). Elles sont lues
dans `recomp.ini`, clés `key_a`, `key_b`… `key_rs_right`, section `[keyboard]`, en scancodes : la position physique de
la touche, `0xE0xx` pour une touche étendue (flèches, Ctrl droit…), 0 pour aucune touche. Sans ces clés, les positions
de toujours (identiques à la 1.0.2). `MapVirtualKeyW(…, MAPVK_VSC_TO_VK_EX)` puis `GetAsyncKeyState` ; le pavé
numérique répond en chiffres comme en déplacements. Entrée, Retour arrière, Échap et Tab restent des raccourcis de
menu (A, B, +, -) tant que le joueur ne les donne pas à une action. Le journal du jeu dit combien de touches diffèrent
des positions par défaut (« keyboard: N key(s) changed »).

**Le launcher** (`lanceur.cpp`, `dessin.cpp`, `textes.cpp`). La page Contrôles montre une case par touche : un clic,
puis la touche voulue (Échap ou un clic : annuler). Une touche déjà prise par une autre action est échangée. En haut,
un sélecteur « Touches du clavier » : deux préréglages, « WASD (QWERTY) » et « ZQSD (AZERTY) », et « Personnalisées »
dès qu'une touche a été changée. Les préréglages sont définis par la lettre gravée sur la touche et résolus sur le
clavier réglé dans Windows (`MapVirtualKeyW(lettre, MAPVK_VK_TO_VSC)`) :
- WASD : stick gauche W A S D, Y sur Z, L sur Q, + sur M ;
- ZQSD : stick gauche Z Q S D, Y sur W, L sur A, + sur la virgule ;
- le reste est commun (A : C, B : X, X : V, R : E, ZL : R, ZR : T, - : N, clics F et H, stick droit I J K L, croix :
  flèches).
Sur un clavier QWERTY le premier, sur un AZERTY le second, donnent exactement les positions par défaut du jeu : c'est
celui qu'affiche le sélecteur tant que le joueur n'a rien changé. Les 24 clés ne sont écrites dans `recomp.ini` qu'au
premier changement. Les changements valent au prochain lancement du jeu (réglages lus au démarrage).

**Essais.** `essai_touches_launcher.py` (dossier de travail de la session, pas dans le dépôt) : launcher d'essai sur
l'installation d'essai, clics et touches envoyés à sa fenêtre, `recomp.ini` relu après chaque étape : 16 contrôles sur
16 (changement, échange, annulation par Échap, touche étendue, les deux préréglages sur ce PC en AZERTY). Jeu relié et
démarré avec 5 touches changées : « keyboard: 5 key(s) changed », aucune ligne « invalid ». Captures de la page vérifiées
à l'œil (préréglage ZQSD affiché par défaut sur ce PC). **Pas essayé** : les touches pressées pendant une partie
(il faut la fenêtre du jeu au premier plan) ; à faire par l'utilisateur.

Les manettes ne sont pas réaffectables : boutons par position, comme avant.

## Mémoire du jeu : ce qu'on pourrait rendre à Windows (recensement, 2026-11-12)

Question de l'utilisateur : le jeu prend trop de mémoire ; ne peut-on pas faire une « réservation dynamique », qui
prenne la mémoire au fur et à mesure et la rende quand le jeu n'en a plus besoin ? La première moitié existe depuis la
1.0.3 (engagement à la demande, par blocs de 2 Mo). La seconde ne marche que quand le jeu rend lui-même une réserve
entière (UnmapPhysicalMemory, aux changements de zone) : à l'intérieur de ses réserves, le jeu gère sa mémoire seul et
ne prévient personne quand il libère un objet.

**Diagnostic ajouté** (`memory.cpp`, `RECOMP_MEM_CENSUS=s`, inactif sans la variable) : toutes les s secondes, une page
sur 16 des plages à la demande engagées est examinée. Vide : elle ne contient que des zéros ; rendue à Windows, elle se
relirait à zéro, rien ne changerait pour le jeu. Touchée : le jeu l'a lue ou écrite depuis le recensement précédent
(méthode : la page est retirée de l'ensemble de travail du processus par `VirtualUnlock` sur une page non verrouillée,
son contenu reste en RAM ; elle y revient au premier accès ; `QueryWorkingSetEx` le dit). Lecture par
`ReadProcessMemory`, qui ne déclenche pas l'engagement à la demande. Estimations : fraction de l'échantillon × mémoire
engagée exacte, au total et par plage. Coût : 0,14 à 0,55 s par recensement sur un fil à part.

**Mesure** (mission de Corneria jouée sans joueur, 1440p, 10 minutes, recensement toutes les 45 s, Zen fermé) :

| | Valeur |
|---|---|
| Mémoire engagée du processus | 7,7 à 9,7 Go (pic 9,69) |
| Mémoire du jeu dans les plages à la demande | 2,9 à 3,9 Go (les données des textures, 1,3 à 2,9 Go, sont déjà dans le fichier temporaire) |
| dont pages vides (que des zéros) | **1,30 à 1,35 Go, stable du début à la fin** |
| touchée par le jeu en 45 s | 0,14 à 0,74 Go |
| pas touchée depuis 5 recensements et plus (≈ 4 min) | 1,6 Go, dont 1,24 Go de pages vides |
| pas touchée depuis 10 recensements et plus (≈ 7,5 min) | 1,56 à 1,59 Go |

Les pages vides sont surtout dans des réserves de 80 à 112 Mo engagées en entier et vides à 62-99 % (0x79d8c00000,
0x79de000000, 0x7946a00000, 0x7854c00000, 0x7867200000…) : réserves effacées par le jeu ou engagées d'avance pour le
noyau ou le pilote (`mem_ensure`), jamais remplies. La grande réserve de 3 Go (textures) n'en a presque pas (1 à 3 %).
Le creux à 20,9 img/s à 430 s est un changement de zone (1,3 Go de textures envoyées en 10 s), pas un recensement.

**Ce qu'on pourrait rendre sans rien comprendre du jeu** :
- les pages vides : ≈ 1,3 Go, sans aucune différence pour le jeu (il les relirait à zéro). À faire : un balayage
  périodique qui, sous le verrou de `mem_fault`, protège la page en lecture seule, la revérifie, puis la désengage ;
  à exclure : les fenêtres importées dans Vulkan (le GPU y lit et écrit) et les pages en cours de rendu de textures ;
  au retour d'un accès, n'engager que le morceau touché (sinon le bloc de 2 Mo réengagerait les pages rendues) ;
- les pages pleines pas touchées depuis plusieurs minutes : ≈ 0,4 Go de plus, à ranger dans le fichier temporaire
  comme les textures ; un accès du jeu les ferait revenir (petite attente). Plus risqué, gain plus faible.
Soit ≈ 1,3 à 1,7 Go sur un pic de 9,7 Go. Les autres leviers restent ceux de la section « Pics de mémoire en jeu » :
cache des images sans objet (jusqu'à 1 Go), pics du compilateur du pilote (2 Go, non résolu).

**Réserve sur la mesure** : une page lue seulement par le GPU (fenêtre importée) n'est pas vue « touchée » ; elle ne
devra jamais être rendue. Une seule partie mesurée ; le chemin du joueur automatique change d'une partie à l'autre.

## Mémoire du jeu : pages vides rendues à Windows (2026-11-12, pour la 1.0.3)

Suite de la section précédente, à la demande de l'utilisateur : les pages de la mémoire du jeu qui ne contiennent que
des zéros sont rendues à Windows pendant la partie. Le jeu ne voit aucune différence : s'il y revient, il les retrouve à
zéro, le contenu qu'elles avaient.

**Comment** (`memory.cpp`, `vides_balayer`, un fil de priorité basse) : à 10 s, 25 s, puis toutes les 30 s, les morceaux
de 64 Ko engagés des plages à la demande sont examinés.
- Candidats : en lecture-écriture, dans un bloc de 2 Mo engagé depuis plus d'un balayage (`g_age` : un bloc tout juste
  engagé est souvent en train d'être rempli), hors des fenêtres importées dans Vulkan (`mem_epingler` à l'import,
  `mem_desepingler` à la libération du pool : le GPU y lit et écrit sans passer par une faute).
- Lecture par `ReadProcessMemory`, qui ne déclenche pas l'engagement à la demande. Un morceau déjà trouvé plein n'est
  relu qu'un balayage sur quatre, et seulement s'il est en RAM (`QueryWorkingSetEx` : rien n'est ramené du fichier
  d'échange).
- Rendu par suites de 2 Mo au plus, sous le verrou de `mem_fault` (`g_rel_lock`) et la garde du noyau prise en exclusif
  (sinon la suite attend le balayage suivant) : protection en lecture seule, nouvelle vérification, puis
  `VirtualFree(MEM_DECOMMIT)` des morceaux toujours vides ; les autres repassent en lecture-écriture. Une écriture du
  jeu pendant ce temps faute et attend le verrou, puis trouve la page désengagée : `mem_fault` en engage le morceau,
  l'écriture reprend sur des zéros.
- Au retour du jeu, `mem_fault` n'engage que le morceau de 64 Ko touché (`g_vides`) : le bloc de 2 Mo réengagerait
  toutes les pages rendues autour.
- Garde du noyau (`MemNoyau` dans `hos.h`) : tenue en partage de l'engagement d'une plage jusqu'à la fin de l'appel qui
  y écrit sans passer par le processeur du jeu, donc sans faute : lecture de la romfs (`ReadFile`, `svc_fs.cpp`) et
  import d'une fenêtre dans Vulkan (`vk_render.cpp`).

**Réglage** : `zero_page_release = true` dans `recomp.ini` (ajouté à la fin des anciens fichiers) ; `false` le coupe ;
sans effet si `memory_saver = false`. Variables : `RECOMP_ZERO_PAGE_RELEASE=0/1`, `RECOMP_ZERO_SWEEP_S=s` (autre
période, essais). Journal : `empty game pages given back to Windows: on (sweeps at 10 s and 25 s, then every 30 s;
pieces of 64 KB)` au démarrage, puis une ligne `[mem]` par balayage qui rend quelque chose (une sur dix sinon) : rendu maintenant, rendu par
ce balayage, total, réengagé au retour du jeu, morceaux examinés et lus, suites reportées, durée.

**Mesure** (mission de Corneria sans joueur, 1440p, Zen fermé, mêmes réglages que le recensement, balayages toutes les
30 s dès le début ; la partie a été fermée à 443 s par sa fenêtre, code de sortie 0, pas un plantage) :

| | Sans (recensement) | Avec |
|---|---|---|
| Pages vides rendues | — | 1 050 Mo au premier balayage utile (90 s), puis 994 à 1 029 Mo |
| Réengagées au retour du jeu | — | 117 Mo en 7 minutes |
| Mémoire engagée du processus, relevés toutes les 30 s (Go) | 7,8 7,5 8,4 8,9 9,0 8,6 8,0 8,2 8,7 8,9 9,0 9,1 9,0 9,5 | 7,2 7,5 6,5 6,6 7,6 7,9 7,4 7,1 8,2 7,5 7,7 6,5 6,0 7,4 |
| Moyenne et pic sur ces 7 minutes | 8,6 et 9,5 Go (9,69 Go en 10 minutes) | 7,2 et 8,3 Go |
| Pages vides restantes (recensement) | 1,30 à 1,35 Go | 0,31 à 0,34 Go |
| Durée d'un balayage | — | 15 à 281 ms, sur son fil |
| Suites reportées, refus de Windows, dessins ignorés | — | 0, 0, 0 |

Le gain sûr est celui que le balayage mesure lui-même : un peu plus de 1 Go rendu en permanence. L'écart des relevés
(1,4 Go en moyenne) en contient une part de hasard : le joueur automatique ne suit pas le même chemin d'une partie à
l'autre. Images par seconde : médiane 60 dans les deux parties.

Seconde partie, avec le service des amiibo de la section suivante (3 minutes, nouvelle partie : écran titre, prologue,
tutoriel) : 1 038 Mo rendus à 60 s, puis 1 054 à 1 062 Mo ; 19 Mo réengagés ; balayages de 16 à 421 ms ; aucune suite
reportée, aucun refus de Windows, aucun dessin ignoré ; 60 img/s du début à la fin. Le pic du processus (9,0 Go) tombait
avant le premier balayage utile : un bloc n'est examiné qu'après un balayage entier, donc à partir de 60 s.

D'où les premiers balayages avancés à 10 s et 25 s. Même début de partie neuve, 3 minutes : 733 Mo rendus dès 25 s
(304 suites reportées : chargement du tutoriel, les lectures de la romfs et les imports Vulkan tenaient la garde du
noyau), 1 021 Mo à 55 s, puis 1 017
à 1 041 Mo ; pic du processus 8,6 Go au lieu de 9,0 ; relevé à 45 s : 7,9 Go au lieu de 8,6. Balayages de 78 à 281 ms ;
78 Mo réengagés en 3 minutes ; aucun refus de Windows, aucun dessin ignoré. Cette partie est allée plus loin dans le
tutoriel (cinématique) : creux à 37,7-50 img/s avec beaucoup de textures envoyées et le cache des images plafonné (4,4 à
5 Go de mémoire validable libre sur le PC) ; ils ne suivent pas les balayages, trop courts pour peser sur une moyenne de
10 s.

**Pas fait** : les pages pleines pas touchées depuis plusieurs minutes (≈ 0,4 Go de plus, à ranger dans le fichier
temporaire comme les textures), le plafond du cache des images sans objet, les pics du compilateur du pilote (section
« Pics de mémoire en jeu »).

## Amiibo (nfp:user) : un lecteur NFC sans figurine (issue n° 9, 2026-11-12, pour la 1.0.3)

Issue n° 9 (trois joueurs) : après le tutoriel, au premier passage par le menu de la campagne, « Single Player » ne
répond plus. À ce moment, le jeu ouvre le service des amiibo `nfp:user` (Initialize, AttachAvailabilityChangeEvent,
ListDevices, puis Finalize trois secondes plus tard). Ce service était le bouchon générique : la commande 0 de la racine
rendait un objet bouchon, AttachAvailabilityChangeEvent aucun handle d'événement alors que le jeu en attend un,
ListDevices aucun appareil, GetState « non initialisé ».

**Maintenant** (`svc_misc.cpp`, `make_nfp_user`) : le service imite une console avec un lecteur NFC (la manette du
joueur 1) et aucune figurine posée, comme Ryujinx ; c'est le chemin que le jeu a connu sur la console.
- `CreateUserInterface` (0) rend un objet `IUser` ; Initialize (0) et Finalize (1) ; GetState (19) : initialisé ou non.
- ListDevices (2) : un appareil (handle 0, manette du joueur 1) ; avant Initialize : WrongDeviceState.
- StartDetection (3) et StopDetection (4) : l'appareil cherche une figurine (GetDeviceState, 20), qui ne vient jamais.
- AttachActivateEvent (17), AttachDeactivateEvent (18), AttachAvailabilityChangeEvent (23) : un vrai handle d'événement,
  jamais signalé.
- Mount, données de l'application, informations de la figurine (5, 7 à 16, 24) : échec WrongDeviceState (module 115,
  code 73), comme sans figurine. GetNpadId (21) : joueur 1. GetApplicationAreaSize (22) : 0xD8.
- Toute autre commande : bouchon générique, notée dans le journal.

**Pas vérifié** : nos parties d'essai partent de sauvegardes de campagne finie, ou d'une partie neuve qui reste dans le
tutoriel (l'appui automatique sur A ne sait pas incliner l'appareil) ; aucune ne passe par ce menu, et le jeu n'y a
jamais ouvert `nfp:user`. Le lien entre ce service et le bouton bloqué n'est pas démontré. À confirmer par un joueur touché (nouvelle partie,
tutoriel, puis menu de la campagne), ou avec une sauvegarde prise juste après le tutoriel.

## Mémoire du jeu : pages oubliées rangées dans le fichier temporaire (2026-11-12, pour la 1.0.3)

Suite des pages vides, à la demande de l'utilisateur : les pages pleines de la mémoire du jeu que le jeu ne touche plus
depuis 4 minutes sont rangées dans le fichier temporaire des textures, et relues s'il y revient. Le recensement les
estimait à 0,3-0,4 Go.

**Comment** (`memory.cpp`, `vides_balayer`, même fil et mêmes balayages que les pages vides) :
- Savoir qu'un morceau de 64 Ko n'est plus touché : le balayage le retire de l'ensemble de travail du processus
  (`VirtualUnlock` sur des pages non verrouillées ; leur contenu reste en RAM, sur la liste des pages modifiées de
  Windows). Au premier accès du jeu, du runtime ou du noyau, la page y revient (faute douce, de l'ordre de la
  microseconde) ; `QueryWorkingSetEx` le dit aux balayages suivants. Le contrôle se fait au début du balayage, avant ses
  lectures, qui ramèneraient des pages.
- Seuls les morceaux trouvés pleins par le balayage des pages vides sont suivis, hors des fenêtres importées dans Vulkan.
  Retraits à partir du 4e balayage, 128 Mo au plus par balayage, en tournant sur tous les morceaux pleins ; un seul
  `VirtualUnlock` par bloc de 2 Mo (64 au plus, 2 ms de pause tous les 8). Un morceau trouvé touché attend 4, 8 puis 16
  balayages avant d'être retiré de nouveau ; un morceau relu du fichier, 16.
- Un morceau pas revenu pendant 8 balayages (4 minutes) est rangé : sous le verrou de `mem_fault` et la garde du noyau
  en exclusif, protection en lecture seule, écriture dans le fichier, désengagement ; il entre dans `g_rel` comme une
  texture rendue, marqué `Rendu::froid` (compté à part des textures). Suites de 2 Mo au plus, 1 ms de pause avant
  chacune ; même chose pour les pages vides, une pause toutes les 8 suites : un fil du jeu qui attend le verrou passe
  (un verrou SRW relâché puis repris aussitôt par le même fil ne laisse pas toujours passer ceux qui attendent).
- Au retour du jeu : `mem_fault` relit le morceau de 64 Ko du fichier, comme pour une texture ; `mem_ensure` et
  `mem_restore` de même pour une plage entière.
- Verrous tenus en priorité normale (le fil du balayage est en dessous de la normale) : pas d'inversion de priorité.
  Les pages vides vérifient aussi, sous le verrou, que la suite est encore engagée en lecture-écriture d'un bout à
  l'autre.
- Limite connue, la même que pour les textures : pendant la restauration d'une page (moins d'une microseconde entre son
  engagement et sa copie), un autre fil qui y accède sans fauter lit des zéros ou voit son écriture recouverte ; il
  faudrait que deux fils se mettent à toucher la même page oubliée depuis des minutes dans la même microseconde.

**Réglage** : `cold_page_release = true` dans `recomp.ini` (ajouté à la fin des anciens fichiers) ; sans effet si
`memory_saver`, `texture_release` ou `zero_page_release` vaut `false`. Variables : `RECOMP_COLD_PAGE_RELEASE=0/1`,
`RECOMP_COLD_SWEEPS=n` (balayages sans accès avant le rangement, essais). Journal :
`forgotten game pages put in the temporary file: on (...)` au démarrage, puis une ligne `[mem]` par balayage qui range
quelque chose (une sur dix sinon) : rangé maintenant, par ce balayage, en tout, relu au retour du jeu, morceaux
observés, retirés de l'ensemble de travail, suites reportées, durée. La taille du fichier de la ligne des textures
(`file … MB`) les compte aussi.

**Mesures** (mission de Corneria sans joueur, 1440p, même copie de sauvegarde que les parties précédentes) :
- Essai accéléré (balayages toutes les 10 s, rangement après 3 balayages sans accès ; première version) : jusqu'à
  931 Mo rangés ; 830 Mo relus pendant le chargement de Corneria, qui réemploie la mémoire du menu (relue du fichier
  avant d'être réécrite) ; aucune erreur, images normales.
- Partie de 10 minutes avec les vrais réglages (première version) : premiers rangements à 5 min 28, 465 Mo rangés à
  9 min 31 et ça montait encore, 4 Mo relus, aucune erreur. Mais des creux d'images : les fenêtres de 10 s qui
  contiennent un balayage tombaient à 56,7 img/s en moyenne (7 sur 25 sous 55) contre 58,6 pour les autres. Cause : un
  `VirtualUnlock` par morceau de 64 Ko (des milliers par balayage) et un rangement par suites de 256 Ko (des centaines de
  changements de protection et de désengagements) ; chacun de ces appels interrompt tous les processeurs qui exécutent
  le jeu pour vider leurs TLB.
- Version finale (un `VirtualUnlock` par bloc de 2 Mo, suites de 2 Mo) : 235 Mo rangés à 7 minutes, rien de relu,
  aucune erreur ; fenêtres avec un balayage 57,9 img/s (3 sur 18 sous 55), sans 58,4 (3 sur 24). Référence avec les
  pages vides seules : 58,4 et 58,4 ; sans aucun balayage (recensement) : 58,0 et 58,8. Cette partie a été arrêtée à
  7 min 09 par le garde-fou de l'essai : un autre programme avait pris de la mémoire, il ne restait que 2,4 Go de
  mémoire validable libre sur le PC.
- La mémoire engagée ne se compare pas d'une partie à l'autre (le chemin du joueur automatique fait varier de ±1 Go) :
  la mesure sûre est le compteur du balayage, chaque octet rangé étant désengagé.

Gain : 0,2 à 0,5 Go après 5 à 10 minutes de jeu, et il grandit avec la durée de la partie. Prix : quelques dizaines
de microsecondes de lecture du fichier quand le jeu revient sur une page oubliée.

## Alerte de mémoire du launcher : 10 Go au lieu de 13 (2026-11-12, pour la 1.0.3)

Le launcher refusait le premier clic sur JOUER sous 13 Go de mémoire validable libre (section « Mémoire du jeu :
textures retirées de la RAM… »), un seuil calculé quand le jeu en engageait ≈ 9,7 Go. Depuis les pages vides et les
pages oubliées, la partie de 19 minutes de l'utilisateur a engagé 6,4 Go en moyenne et 7,4 Go au plus ; elle avait été
lancée avec 9,6 Go libres, sans aucun problème. À la demande de l'utilisateur, l'alerte ne vient plus que sous 10 Go :
`jouer()` dans `lanceur.cpp`. Le journal du jeu (`host_main.cpp`, « available commit … TOO LOW ») suit le même seuil.
Marge plus mince qu'avant pour les pics du compilateur du pilote (jusqu'à 2 Go, section « Pics de mémoire en jeu ») :
si le jeu s'arrête faute de mémoire, le launcher le dit (code 4) comme avant.
