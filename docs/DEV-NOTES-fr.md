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
  ou QWERTY) → manette Xbox. La réassignation n'existe pas encore.
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
