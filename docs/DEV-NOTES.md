# recomp/ — static ARM64 → C++ recompiler (Phases 1 to 4)

> English translation of [DEV-NOTES-fr.md](DEV-NOTES-fr.md), the notes written in French during development.
> Both files are updated together; if they differ, the French text is the reference.

Recompiler for the native Windows x86-64 port of the Switch 2 title described in
`../NOTES.md`: ARM64 → C++ core with its differential validation test bench against Unicorn
(Phase 1), then a native executable that loads all the recompiled modules and emulates the
Horizon kernel and the system services (Phase 2), then graphics startup with NVN in HLE, a host
window and a render loop that presents frames (Phase 3), and then execution of NVN rendering on
Vulkan with shader translation (Phase 4, in progress). See the dedicated sections below.

> **Private.** `out/`, `build/` and `bench/results/` are derived from the game (generated code,
> objects, executable, traces, memory states) and must never be distributed. Only the Python
> sources and the runtime (`a64/`, `runtime/`, `bench/*.py`, `*.py`) are generic.

## Directory layout

| Path | Role |
|---|---|
| `recomp.py` | CLI: `gen <module>` (C++ generation), `stats <module>` (decoder coverage) |
| `build.py` | parallel g++ compilation with a per-file cache (`.dll`, `.exe` or objects) |
| `a64/module.py` | loading of a `work/*.bin` image: MOD0, `.dynamic`, symbols, relocations (RELA, JMPREL, **RELR**), functions via `.eh_frame_hdr` |
| `a64/translate.py` | decoder + A64 translation: integer / branches / system / memory / atomics |
| `a64/simd.py` | scalar FP and AdvSIMD (NEON), structure loads/stores |
| `a64/jumptable.py` | recovery of LLVM jump tables (local `switch`) |
| `a64/codegen.py` | function discovery, emission of the bodies, dispatch table, stream mode |
| `runtime/cpu.h` | `CPUState`, guest memory, flags, LSE atomics, TRACE / CHECK_RET / MUSTTAIL macros |
| `runtime/simd.h`, `runtime/fp.cpp` | exact ARM FP semantics (NaN, min/max, FMA, conversions, FRECPE/FRSQRTE…) |
| `runtime/runtime.cpp` | dispatch of indirect jumps, svc, errors, sysregs; in test bench mode: trace and C API |
| `bench/guest.py` | guest memory at a fixed address in the Python process, shared state, Unicorn |
| `bench/harness.py` | test bench: `func` (whole functions, step-by-step trace) and `fuzz` (instruction by instruction) |
| `bench/run_all.py` | full validation suite → `bench/results/validation.log` |
| `fullbuild.py` | **Phase 2**: full build of a module (parallel discovery, balanced batches, generation + compilation, sources deleted after compilation) → `build/full/<module>/`, + `build/full/manifest.txt` |
| `link.py` | **Phase 2**: linking of `build/full/recomp_main.exe` (modules + runtime + host layer) |
| `runtime/os/hos.h` | "Horizon" host layer: guest memory layout, modules, kernel, IPC (never included by the generated code) |
| `runtime/os/memory.cpp` | reservation of the guest space at a fixed address, region map (state/perm/attributes) |
| `runtime/os/loader.cpp` | native loader (replaces rtld): mapping, RELA/JMPREL/RELR/TLSDESC relocations, global scope, symbolization |
| `runtime/os/kernel.cpp` | emulated kernel: handles, host threads, mutexes/condvars/address arbiter, events, memory, GetInfo |
| `runtime/os/ipc.cpp`, `ipc.h` | HIPC/CMIF IPC (domains, control) and TIPC, `sm:`, logged stub services |
| `runtime/os/svc_*.cpp` | HLE services: applets (`appletOE`), `lm`, time (`time:*`), files (`fsp-srv`, virtual romfs), parental controls, `aoc:u`, accounts/network, **input (`hid`)**, **sockets (`bsd`)** |
| `runtime/os/host_main.cpp` | `main()`: log, crash handler (guest call stack), rtld-style startup, diagnostic options |
| `runtime/os/hle_gfx.cpp` | **Phase 3**: `nn::gs` bypassed, fake `nn::vi`, **NVN in HLE** (native functions + named stubs), no camera |
| `runtime/os/present.cpp`, `gfx.h` | **Phase 3**: Win32 host window (GDI) paced at 60 Hz, frame counter |
| `runtime/os/input.cpp` | **Phase 3**: host keyboard + XInput controller for the emulated controller |
| `runtime/os/hle_os.cpp` | **Phase 3**: `nn::os` fibers and engine coroutines → Windows fibers; neutralization of namespaces (`nn::chat`) |
| `runtime/os/hle_audio.cpp` | **Phase 3**: `nn::audio` output streams (buffers, mixer, gain), hardware Opus decoder (silence) |
| `runtime/os/audio_out.cpp`, `audio_out.h` | Windows audio output (WASAPI): the sound card's clock drains the game's streams |
| `hooks.txt` | guest functions whose **direct** calls go through the dispatch (so that they can be replaced by native ones) |

## Usage

```bash
python recomp.py gen rtld                      # → out/rtld/*.cpp (whole module)
python recomp.py gen main --funcs 3000 --out out/main_lot1   # first batch of main
python recomp.py gen main --stream --no-comments               # whole main, bounded memory (~4 GB of C++)
python build.py out/rtld build/rtld.dll -O 2   # compiles (runtime included)
python bench/harness.py func rtld --trials 16  # whole-function differential
python bench/harness.py fuzz main --sample 20000 --fp-only    # FP/NEON fuzzing of main
python bench/run_all.py                        # everything (≈ 15 min)
```

## Execution model

* **One guest function = one C++ function** `void f_<address>(CPUState* ctx)`. The functions
  come from `.eh_frame_hdr` (248,842 for main), supplemented by the targets of `bl`/`b`, the
  exported symbols, `DT_INIT/FINI` and all the pointers relocated into `.text` (vtables…). An
  entry in the middle of a function yields a copy of the suffix (190 in the first 23,000
  functions of main); a function without an FDE (PLT stubs) is delimited by reachability.
* **Branches**: internal → `goto L_<addr>`; `b`/`b.cond` to another function → **guaranteed**
  tail call (`[[gnu::musttail]]`, gcc ≥ 15 / clang); `bl` → `x30 = return address` then a direct
  call; `ret` → `return`; `blr` → `rt_call` (binary search in the dispatch table); `br` → local
  `switch` if a jump table is recognized, otherwise `rt_tailcall`.
* **State**: `ctx->x[0..30]`, `ctx->sp`, separate flags `nf/zf/cf/vf` (0/1), `ctx->v[32]`
  (V128), `fpcr/fpsr`, `tpidr_el0/tpidrro_el0`, exclusive monitor. Layout frozen
  (`static_assert`) and reproduced by the test bench.
* **Memory**: identity guest address == host address (`RECOMP_MEM_OFFSET` option). The modules
  are loaded at fixed bases (`a64/module.py`, `DEFAULT_BASES`: 0x71xxxxxxxx, free in a Windows
  process), the emitted code contains absolute addresses. Unaligned accesses via `memcpy` (free
  on x86).
* **LSE → native atomics**: `cas*/casp*`,
  `ldadd/ldclr/ldeor/ldset/ldsmax/ldsmin/ldumax/ldumin/swp` (all A/L variants, 8–64 bits) →
  `__atomic_*` seq_cst (= x86 `lock`); `ldar/stlr/ldapr/ldapur` → atomic load/store;
  `ldxr/stxr/ldxp/stxp` emulated by CAS.
* **PAC → nop/ret**: `pacibsp/autibsp/xpaclri…` (HINT space) and the register forms
  `PAC*/AUT*/XPAC*` are nops; `retab/retaa` = `ret`, `braa/blraa…` = `br/blr`. (Unicorn has PAC
  disabled in SCTLR_EL1, hence the same semantics.)
* **FP**: ARM semantics reproduced (see `runtime/simd.h`): NaN (SNaN priority, 1st operand,
  positive default NaN, FPCR.DN), FMIN/FMAX/NM with signed zeros, saturating conversions, fused
  `fmadd/fmla`, `-ffp-contract=off` so that `fmul`+`fadd` are never fused, FRECPE/FRSQRTE using
  the tables from the ARM pseudocode, FPCR.RMode/FZ → MXCSR.
* **TLS**: `mrs tpidr_el0/tpidrro_el0` read the state; `dc zva` zeroes 64 bytes.

Example of emitted code (rtld):

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

## Differential validation test bench

The DLL of the recompiled code (compiled with `RECOMP_BENCH RECOMP_TRACE RECOMP_CHECK_RET`) is
loaded through ctypes into the Python process, which allocates the guest memory **at the same
addresses** (`VirtualAlloc` at a fixed address). Unicorn (`max` CPU: LSE, RCpc…) runs the
original ARM code on an identical copy.

* `func`: for each function and each trial, an identical random initial state on both sides
  (arguments pointing to a work area filled with pointers/values, stack, TLS, V registers with
  special FP values). Both sides record the complete state (X0–X30, SP, NZCV, V0–V31) **before
  each instruction**; the traces are compared step by step, then the final state and the memory
  (`.data/.bss`, stack, work area, TLS). The first divergence points to the faulty instruction.
  The imports point to thunks (which return 0), `svc` returns 0, on both sides. Verdicts: `PASS`
  (identical, including a fault/trap at the same step), `FAIL`, `NONCODE` (call through a random
  pointer into data: not comparable), `NOTBUILT` (target outside the compiled batch), `UNIMPL`.
* `fuzz`: each distinct instruction word is translated in isolation and executed on N random
  states (base register of the memory accesses pointed at the work area, sometimes unaligned);
  comparison of the registers + a memory window. `--mutate OLD=NEW` deliberately corrupts the
  translation to **check that the test bench detects errors** (done: every mutation tested —
  flags, shifts, access sizes, FP operations, lanes — produces failures).
* Known bug in the reference: Unicorn 2.1.4 (QEMU 5) compares 8/16/32-bit `ldsmax/ldsmin` as
  **unsigned**. These cases are checked by an independent Python oracle and classified `REFBUG`
  (our translation follows the ARM ARM).

## Results (2026-11-08)

See `bench/results/validation_phase1.log` for the last full run (the quick suite `--quick`,
rerun after Phase 2, gives the same results: `bench/results/validation.log`).

Figures from the last full run of `bench/run_all.py` (final code):

| Test | Result |
|---|---|
| Decoder on **all of main** | 49,811,607 instructions, **0 unhandled** |
| rtld, whole functions | 43 functions × 16 trials, 383,999 instructions compared step by step: **670 PASS, 0 FAIL** (18 NONCODE); coverage 41% (47.7% on another random draw) |
| subsdk3, whole functions | 151 functions × 16 trials, 38,139 instructions step by step: **2,416/2,416 PASS**; coverage 38% |
| rtld / subsdk3, fuzzing | 1,174 + 1,434 words × 16 trials = 41,728 executions: **0 failures** |
| main, FP/NEON fuzzing | 30,702 distinct words × 8 trials = 245,616 executions: **0 failures** |
| main, integer fuzzing | 40,000 words × 6 = 240,000 executions: **0 failures** |
| main, rare families (LSE, structure ld/st, tbl, estimates…) | 25,000 words × 8 = 200,000: 199,993 PASS + 7 REFBUG (Unicorn bug confirmed by oracle), **0 failures** |
| main, saturating (sqdmulh/sqrdmulh/sqshl) | 12 words × 32: 384/384 |
| main, whole functions | 600 functions (closures ≤ 60) × 8 trials, 624,047 instructions step by step: **4,670 PASS, 0 FAIL** (88 NONCODE, 42 NOTBUILT) |
| main, first compiled batches | batch 1: 3,000 functions / 167k instructions → -O2 in 19–23 s; batch 2: 20,000 functions / 1.29M instructions, 112 MB of C++ → -O2 in 41 s (16 threads), 23 MB of objects |

Bugs found and fixed thanks to the test bench during Phase 1: fall-through at the end of a
function onto a padding `udf` after a no-return call; quadratic splitting of the FDE-less PLT
stubs; LLVM jump tables whose base is hoisted out of the loop (rtld); test bench artifacts
(Unicorn data abort ≡ access violation; data imports pointing into the thunk area).

Extrapolation to the whole of main: ~4.3 GB of C++ (without comments), ~25 min of -O2
compilation on 16 threads, ~0.9 GB of objects.

## Phase 2 — native executable (2026-11-08)

### Build and run

```bash
python fullbuild.py sdk subsdk0 subsdk1 subsdk2 subsdk3 main -j 15   # ≈ 33 min (main ≈ 30 min)
python link.py                                                          # ≈ 10 s → build/full/recomp_main.exe
build/full/recomp_main.exe [-v 0|1|2] [--log file]                      # log: recomp_run.log
```

`fullbuild.py` discovers the functions in parallel (rounds until a fixed point is reached),
splits them into batches of about 60,000 instructions, generates each batch without `TRACE`
lines, compiles it at -O2 and then **deletes the source**: main = 251,466 functions, 51.3M
instructions, 3.1 GB of C++ never present on disk all at once, 0.85 GB of objects.
Content-based cache: rerunning after a change to the runtime recompiles nothing. `link.py`
produces an 890 MB executable (`--modules sdk,...` to link without main during a build).

### Architecture choice: recompiled SDK, emulated kernel

Instead of rewriting by hand the 998 functions that main imports from the SDK, **the SDK is
recompiled like everything else** (2.2M instructions) and what lies beneath it is emulated:

* **the Horizon kernel at the `svc` level**: the SDK contains only 57 `svc`s, each in a small
  wrapper (standard ABI, verified on the wrappers);
* **the system services at the IPC level** (HIPC/CMIF/TIPC), with stubs that log every unknown
  command: the log draws up the list of what remains to be done.

The libc, libc++, `nn::os`, client-side `nn::fs`, the allocator, TLS (the SDK writes
`tpidr_el0` itself)… therefore work as is. An API-level HLE replacement remains possible
function by function: a native function registered in the dispatch after the modules replaces
the recompiled code (calls via PLT/pointers).

### What the executable does

1. Reserves the guest space `[0x70_0000_0000, 0x80_0000_0000)` (identity guest == host), linked
   with `--disable-high-entropy-va` so that Windows places nothing there.
2. Loads the modules (`manifest.txt`), applies RELA/JMPREL/RELR (main: 94,052 relative +
   2,763 symbolic, all resolved), with rtld's global scope (main first: its `malloc` replaces
   the SDK's).
3. Reproduces rtld: `nn::init::Start(main thread handle, base of main, f2, f3, f4)` with three
   native callbacks; `f3` runs the `_init` functions in the order **sdk → subsdk3 → … → main**
   (rtld inserts each module at the head of the list); fills in the rtld variable that `nn::ro`
   uses.
4. Guest main thread in a host thread with a 256 MB stack (guest stack 1 MB, priority 44,
   according to `main.npdm`).

Emulated kernel (`runtime/os/kernel.cpp`): threads (one host thread per guest thread),
`ArbitrateLock/Unlock` mutexes, `WaitProcessWideKeyAtomic/SignalProcessWideKey` condvars,
`WaitForAddress/SignalToAddress` address arbiter (3 signal modes, like mesosphère), events,
multi-object `WaitSynchronization` with cancellation, heap (`SetHeapSize`), `MapMemory` (by
moving: the source becomes inaccessible, as on the console), alias-region physical memory,
shared memory, `QueryMemory` on a real region map, `GetInfo`.

Crashes: **guest** call stack reconstructed by unwinding the host stack and mapping each host
address to its recompiled function, with the SDK symbols demangled.

### Result: how far the game boots

The game gets through the whole SDK initialization and its own constructors, enters `nnMain`,
mounts its romfs and writes to its log:

```
[lm] vfs.nintendo.cpp(79): note: fs: MountRom succeeded.
[lm] ==[ CPU:Switch 2 (2), RAM:8192MB (2), OS:0 (new), GPU:Switch 2 (2), VRAM:0MB (2), Driver:recommended, Gen:2 ]==
[lm] vgpu.impl.c(229): note: game-0-gpu: Choosing default backend nvn (3)
```

It then starts ~8 worker threads, runs for 1.4 s, then stops (reproducibly, 6 runs out of 6) in
`nn::gs::sdk::BootGraphicsSystem` → `nn::gsv::BootWithRemoteMemory`: the startup of the
Switch 2 graphics system, which expects a handle from `ISelfController` (command 2000,
unknown). **This is the boundary of the graphics phase.**

Steps passed, in order: `nnosInitialize`, `nninitInitializeSdkModule` (applets, aoc, pctl),
`nntimeStartup`, `nninitStartup` of main (heap + VAMM), constructors of all the modules
(including the C++ exception avoided by fixing `clock_gettime`), `nnMain` → romfs mount,
sockets, nifm, `nn::ro`, accounts, then graphics.

### Facts discovered (Switch 2 kernel / services)

* `GetInfo`: new identifiers. **29/30** = 3rd region address/size pair (after heap and alias,
  for an `AddressSpaceAllocator`); **31** = read by `GetRemoteMemoryUsage` (0); **33** = page
  size (0x1000; `ProtectRelro` checks alignment against it, the VAMM maps its 1st management
  page with it); **32** unknown (0). **16** (system resource size) ≠ 0 enables the VAMM
  (`nn::os::AllocateAddressRegion`), which main cannot do without.
* Time shared memory: new block at 0xD0 (continuous adjustment point, 2 × 0x38 bytes, non-zero
  source identifier), without which `clock_gettime(CLOCK_MONOTONIC)` returns EAGAIN and libc++
  throws an exception during the constructors of subsdk2.
* `appletOE`: `OpenApplicationProxy` is command 10. `bsd`: 36 = initialization (version
  0x1000b), then Switch 1 numbering (corrected in Phase 3: 1 = `StartMonitoring`,
  2 = `Socket`); the engine requires that a socket can be created, even offline.
* subsdk2 (DLSS) uses a TLS descriptor (`R_AARCH64_TLSDESC`): wired to a native resolver that
  stops cleanly if it is called.

### HLE services written (`runtime/os/svc_*.cpp`)

| Service | Status |
|---|---|
| `sm:` | TIPC/CMIF: `GetServiceHandle` opens the HLE service or a logged stub |
| `appletOE` | `IApplicationProxy` and its sub-interfaces (common state + messages, controllers, application functions, preselected user, `IStorage`) |
| `lm` | SDK log and **game** log, decoded (text, function) |
| `time:*` | system/steady clocks, shared memory (including the 0xD0 block), time zone |
| `fsp-srv` | **virtual romfs** (RomFS header and tables built in memory, data read in place from the `.nup`, read-only); saves and SD card in host folders (`build/full/save`, `sdcard`) |
| `acc:*`, `nifm:*`, `pctl*`, `aoc:u` | single local user, network never connected |
| `bsd:*` | Switch 1 numbering (+ 36 = Switch 2 init), FreeBSD errno; real `eventfd`s, blocking `poll`, **TCP loopback** (bind/listen/connect/accept/send/recv: the engine's event loop uses it as a `socketpair`); any other destination is unreachable |
| `hid` | shared memory with a **Pro Controller connected as player 1** (see Phase 3), vibrations accepted |

## Phase 3 — graphics startup and render loop (2026-11-08)

### Running

```bash
python link.py                                   # after modifying the runtime (≈ 10-40 s)
build/full/recomp_main.exe                       # "recomp_main" window + recomp_run.log log
build/full/recomp_main.exe --dump 35             # + state of all the threads at 35 s
build/full/recomp_main.exe --trace-nvn           # every NVN call (native or stub) with the caller
build/full/recomp_main.exe --trace-api           # first main → SDK calls per thread
```

Modifying `hooks.txt` requires regenerating the affected module (`python fullbuild.py main -j 15`,
≈ 2.5 min incrementally thanks to the content-based cache).

### Result

The game gets past graphics startup, initializes NVN, opens the host window, loads
`bootstrap`, `global`, then the title screen (`title_screen_ftf`) and `prologue_cin`. Its
**render loop presents frames continuously** (≈ 42 fps, 90 s without a crash):

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

**The content of the frames is not rendered yet**: the NVN commands (draws, compute dispatches,
shaders) are accepted then ignored, and the window displays the last clear color of the
presented texture, with the frame counter. That is Phase 4 (translation of the rendering).

### Architecture

* **`nn::gs` / `nn::gsv`** (startup of the Switch 2 graphics system: remote memory, nvsrv,
  syncpoints): `BootGraphicsSystem` and `CheckGraphicsSystemInitialized` become no-ops, the
  `NativeSync` waits succeed immediately.
* **`nn::vi`**: a fake display, layer and native window (1080p, 60 Hz, SDR).
* **NVN in HLE**: the game obtains all of NVN through
  `nvnBootstrapLoader("nvnDeviceGetProcAddress")` then 728 names (`nvnLoadCProcs`). We return
  **native addresses**: 124 implemented functions (device, queue, sync, pools, buffers,
  textures, window, command buffers, presentation) and, for the others, named stubs drawn from a
  reserve of 1024, which log their first call and return 0. GPU address = CPU address; the syncs
  are always signaled.
* **NVN object identity**: the engine **copies** initialized NVN objects (opaque structures
  whose state lives in their memory on the console). At initialization, we therefore write into
  their first 8 bytes an identifier (`NVH1` | id) that follows the copies; the host state is
  indexed by this identifier.
* **Fibers and coroutines**: the `nn::os` fibers and the engine's assembly coroutines
  (`main+0xba33c30`, saving the callee-saved registers then swapping stacks) are implemented
  with Windows fibers, each with its own `CPUState`. `hooks.txt` routes the direct calls to
  these functions through the dispatch, where the native ones replace them.
* **Audio**: `nn::audio` streams consumed at 48 kHz by a clock (events signaled), hardware Opus
  decoder → silence (960 samples per packet). Since 2026-11-09, the sound is output through the
  sound card (see "Sound").
* **Input**: the hid shared memory presents a Pro Controller (FullKey style) connected as
  player 1, fed at 60 Hz by `input.cpp` (keyboard when the window has focus, first XInput
  controller):

  | Switch control | Keyboard (by position, QWERTY / AZERTY) | XInput controller |
  |---|---|---|
  | left / right stick | W A S D (Z Q S D) / I J K L | sticks |
  | D-pad | arrow keys | D-pad |
  | A / B / X / Y | C or Enter / X or Backspace / V / Z (W) | B / A / Y / X (by position) |
  | L / R / ZL / ZR | Q (A) / E / R / T | LB / RB / LT / RT |
  | + / − | M (,) or Esc / N or Tab | Start / Back |
  | stick clicks | F / H | clicks |

* **Camera** (`nn::camera`, USB webcam): library initialized, no device plugged in
  (0x7c4 → "No camera device detected."). `nn::chat` (GameChat) is neutralized.

### Facts discovered (Switch 2, SDK 21)

* hid shared memory: Npad at 0x9A00 + i × 0x5000 as on Switch 1 (Handheld = entry 8,
  Other = 9), LIFO of 17 entries protected by a sequence number (odd while being written),
  sample numbers that must be consecutive, device type at +0x4188. **New**: `NpadCondition`
  block at 0x3E200 (last active Npad, hold type, initialized flag), read directly by
  `GetNpadJoyHoldType` (otherwise `ShowControllerSupport` aborts).
* IPC domains: cloned sessions (`CloneCurrentObject`) **share** the domain's object table (the
  fs client opens a file on one session and reads it on another).
* `nn::camera::Initialize` → 0xdc4 ("platform without a camera") leads to an engine path that
  dereferences a null pointer: this case never happens on the console. The safe path is "no
  device".
* Engine event loop (libuv-style): its cross-thread wakeup goes through a pair of TCP loopback
  sockets; without it, the loop crashes (empty callback list, -ECANCELED).

### Diagnostics

* `--trace-nvn`: every NVN call with its first 4 arguments, the symbolized caller and the
  thread. This is what revealed the copy of an `NVNbuffer` between `nvnBufferInitialize` and
  `nvnBufferMap`.
* Faulty guest instruction: the crash gives `function (hôte +offset)`. `nm` on the objects in
  `build/full/<module>/` gives the address of `f_<address>`; `objdump -d` around
  address + offset shows the host code; the `CPUState` offsets (`0x8*n` = `x[n]`,
  `0xf8` = `sp`) make it possible to recover the guest sequence.

## Phase 4 — Vulkan rendering (2026-11-08, in progress)

### Running

```bash
build/full/recomp_main.exe                       # Vulkan rendering if available (otherwise GDI presentation)
python shaders/shaderc.py -j 12                  # translates the dumped shaders → build/full/shader_cache
python link.py                                   # after modifying the runtime
```

| Variable | Effect |
|---|---|
| `RECOMP_RESOLUTION` | overrides `resolution` from `recomp.ini` (`720`, `1080`, `1440`, `2160`, `auto`) |
| `RECOMP_REFRESH` | overrides `refresh_rate` from `recomp.ini` (Hz) |
| `RECOMP_NATIVE_RES` | overrides `native_resolution` from `recomp.ini` (`0`: rendering at 50% like the console) |
| `RECOMP_CVARS="name=value;…"` | engine variables appended to `veclipse_build.cvar` |
| `RECOMP_FRAMES_IN_FLIGHT` | frames waiting to be displayed before acquisition blocks (2 by default) |
| `RECOMP_NO_VSYNC_EMU=1` | no back-pressure at the virtual vblank (free-running frame rate) |
| `RECOMP_FH_MODE=9` | empty `ListFrameHistory` (the engine's regulator runs open-loop) |
| `RECOMP_PERF=1` | per-frame timings: presentation, GPU thread, operations, `draw` steps, sync waits |
| `RECOMP_FRAME_TRACE=N`, `RECOMP_FRAME_TRACE_MS=x` | system calls longer than x ms (1 by default) in frames N to N+2 |
| `RECOMP_TS_LOG=N` | GPU timestamps written and read back by the game (frames N to N+3) |
| `RECOMP_AUTOPRESS=1` | presses A for 100 ms every 2 s (title screen) |
| `RECOMP_RECORD_DIR=folder` | MP4 clips of the cutscenes for the launcher (`record.cpp`): detected by their black bars, cropped without them; `RECOMP_RECORD_CLIPS` (6), `RECOMP_RECORD_SECONDS` (9), `RECOMP_RECORD_MIN` (4), `RECOMP_RECORD_GAP` (12), `RECOMP_RECORD_AFTER` (frame), `RECOMP_RECORD_EXIT=1` (quit after the last one) |
| `RECOMP_DUMP_SHADERS=1` | dumps all of the game's shaders into `build/full/shaders/` (input of `shaderc.py`). Without this variable, only unknown shaders (neither translated nor already dumped) are written, with a "shader inconnu" line in the log |
| `RECOMP_NO_SHADER_DUMP=1` | writes no shader at all, not even an unknown one |
| `RECOMP_SAVE_DIR=folder` | game save stored in this folder instead of `build/full/save/`: to be used for all automated tests, so that the player's save is left untouched |
| `RECOMP_SNAPSHOT=N,M` | PNG of presented frame N, M (cropped area, half size) |
| `RECOMP_FRAME_STATS=N` | log of frame N: commands, programs, bindings, targets |
| `RECOMP_VK_DEBUG=N` | Vulkan log of frame N: handle → texture object → Vulkan image, addresses |
| `RECOMP_NO_VULKAN=1` | disables Vulkan |
| `RECOMP_PEEK=addr[:n],…` | with `--dump`: contents of guest memory |
| `RECOMP_TEX_LOG=1` | log of every `nvnTextureInitialize`: size, format, swizzle, packaged layout |
| `RECOMP_TEX_DUMP=N` | at frame N, raw memory of the packaged textures into `build/full/tex_dump/` (decoding: `shaders/texview.py`) |
| `RECOMP_NO_TEX_UPLOAD=1` | does not upload the game's textures (neutral gray, the earlier behavior) |
| `RECOMP_RT_SNAPSHOT=N` | at frame N, PNG of each render target (color, and depth as stretched grayscale) at the end of its pass (`build/full/rt_N_*.png`) |
| `RECOMP_FRAME_STATS=N+K` | log of frames N to N+K-1 |
| `RECOMP_DRAW_DUMP=N:k` | dumps the k-th indexed draw of frame N into `build/full/drawdump/` (constants, vertices, indices, state) and reads these buffers back through the GPU (`*.gpu.bin` / `*.cpu.bin`) |
| `RECOMP_GPU_DIAG=1` | NVIDIA markers (`VK_NV_device_diagnostic_checkpoints`) before/after each draw and compute dispatch, `VK_EXT_device_fault` report: on a hang, identifies the draw in progress |
| `RECOMP_GPU_SERIAL=1` | each draw completes before the next one (same batch): with `RECOMP_GPU_DIAG`, identifies exactly the faulty draw |
| `RECOMP_SYNC_FROM=N` | from frame N onward, each draw / compute dispatch is submitted and waited for on its own |
| `RECOMP_FLUSH_SUBMIT=1` | the GPU executes each of the game's submissions before it returns |
| `RECOMP_DRAW_WATCH=hash` | bindings (separate textures → image, targets, state) of this shader's draws, at every change |
| `RECOMP_DBG_SSBO=1` | with `RECOMP_DRAW_WATCH`: writable buffer in set 1, binding 31, for a hand-modified shader (register values read back after each frame) |
| `RECOMP_TEX_SLOT_WATCH=id`, `RECOMP_SAMP_WATCH=id` | rewrites of a slot of the texture / sampler set, NVN parameters of the sampler |
| `RECOMP_POOL_AUDIT=N` | every N frames, samples of each imported pool read back through the GPU and compared with the CPU; working set, Vulkan memory budget |
| `RECOMP_POOL_LOG=1`, `RECOMP_MEM_LOG=1` | creation / destruction of NVN pools; memory-mapping system calls |
| `RECOMP_NO_COMPARE`, `RECOMP_NO_BORDER`, `RECOMP_NO_3D_TEX`, `RECOMP_NO_ATT_BARRIER`, `RECOMP_NO_DEPTH_BIAS`, `RECOMP_NO_CULL` | disable one feature for a diagnosis (sampler comparison, border mode, 3D views, end-of-pass barriers, depth bias, face culling) |
| `RECOMP_VIEWPORT_VK=1` | old viewport convention (positive height): the scene is stored upside down |
| `RECOMP_FLIP_FRONT=1` | reverses the front-face winding |
| `RECOMP_DEPTH_EQUAL_AS=f` | replaces the EQUAL depth test with NVN function f (7 = GEQUAL, 8 = ALWAYS) |
| `RECOMP_NO_DEPTH_ONLY=1` | skips draws with no fragment shader (depth mask of the interface) |
| `RECOMP_NO_COPY_TEX=w` | skips `CopyBufferToTexture` calls to textures of width w (0: all of them) |
| `RECOMP_SKIP_COPY_TEX_1X1=1` | skips copies to 1×1 textures (old workaround for limitation 28, now resolved) |
| `RECOMP_FINISH_NO_FLUSH=1` | `nvnQueueFinish` without submitting the recorded Vulkan work (former cause of the blocky text) |
| `RECOMP_COPY_IN_PLACE=1` | `CopyBufferToTexture` reads the source at GPU execution time instead of a snapshot taken at recording time |
| `RECOMP_CPU_BUFFER_COPY=1` | `CopyBufferToBuffer` done by the CPU at recording time (old behavior) instead of an ordered GPU copy |
| `RECOMP_GPU_PROFILE=N` | at frame N, GPU duration of each draw and compute dispatch (timestamps), summed per program: the 30 most expensive go to the log |
| `RECOMP_SYNC_SUBMIT=1` | synchronous submissions: the GPU thread waits for each submission to finish (before the 8th pass) |
| `RECOMP_SNAPSHOT_EVERY=n`, `RECOMP_SNAPSHOT_DIR=d` | one snapshot every n frames; (existing) folder for the snapshots |
| `RECOMP_VOLUME=0..100` | output volume (100 by default; 0: muted, the sound card still paces the game's audio) |
| `RECOMP_NO_AUDIO=1` | no audio output: streams consumed by the virtual clock (the earlier behavior) |
| `RECOMP_AUDIO_WAV=f.wav` | records the game's mix (48 kHz, stereo, float, before the volume) |
| `RECOMP_AUDIO_LOG=1` | every 10 s: frames written, frames played, underruns and peak of each stream; Opus decoders and packets |
| `RECOMP_COPY_LOG=1` | bytes copied to small textures (≤ 64 bytes) |
| `RECOMP_NO_TEX_VIEW=1` | ignores the texture pool's views (swizzle, levels, layers) |
| `RECOMP_SAMP_LOG=1` | NVN parameters of every sampler created (filters, wrap, comparison, LOD) |
| `RECOMP_OLD_FILTERS=1` | old filter table (1-based, linear and nearest swapped): diagnostic |
| `RECOMP_DISPATCH_LOG=1` | each compute program executed (once) and the layout of its bank 0 |
| `RECOMP_NO_GLOBAL_MEM=1` | skips compute dispatches that use global memory (`LDG`/`STG`/atomics) |
| `RECOMP_NO_TEX_COPY=1` | skips `CopyTextureToTexture` (old behavior: stale shadow atlas) |
| `RECOMP_WINDOW` | overrides `window_mode` from `recomp.ini` (`borderless`, `windowed`) |
| `RECOMP_SKIP=pattern1,pattern2` | skips draws and compute dispatches whose program label contains a pattern, or one of whose shaders has this hash (e.g. `ssao`, `volumetric_fog`). Under Git Bash, a pattern that starts with `/` is converted to a Windows path and no longer matches anything |
| `RECOMP_KEY_SCRIPT="38:A,43:-A"` | scripted presses: button (A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LSTICK RSTICK) pressed, or released with `-`, at the given time in seconds. Holding A for 5 s skips a cutscene; "38:A,43:-A,52:A,57:-A" usually leads to the training in 70 s (depending on loading times) |
| `RECOMP_BACKGROUND_INPUT=1` | reads the XInput controller even when the window does not have focus (old behavior) |
| `RECOMP_INPUT_LOG=1` | log of every button change and of every change in the presence of the XInput controller |
| `RECOMP_CNTFRQ=hz` | frequency of the guest counter (31,250,000 by default, that of the Switch 2) |
| `RECOMP_ALIAS_PACKAGED=1` | old reuse, by address, of a packaged image without comparing its contents (stale textures) |
| `SHADERC_NO_CLIP=1` | (`shaderc.py`) translates without `gl_ClipDistance` |

Validation layer (MSYS2 package, already present):
`VK_LAYER_PATH='C:\msys64\ucrt64\bin' VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`; the only
remaining error: `Input-08733` (limitation 23). With bindless arrays, the game drops to
1-5 fps under validation.

### Result

Every frame of the game is executed on the GPU (RTX 5070 Ti, Vulkan 1.4), at ≈ 48-50 fps:
960×540 G-buffer, SSAO, FXAA, TAA, upsampling to 1920×1080, final grading through a
**64³ 3D LUT computed at startup by compute shaders** (`scene_lut_generation`,
`display_lut_generation`, `env_brdf`), then presentation of the cropped 1920×1080 area of the
3840×2160 window texture. The displayed image is a uniform color: **the scene does not contain
any geometry yet**, because the game stays on the title screen (loading of `prologue_cin` stuck
in the Wwise audio engine, limitation 14).

Milestones reached:

1. **1×1 resolution → 1920×1080**: the engine reads the screen size from `DisplayInfo` (+0x50 /
   +0x58, max layer width/height) returned by `nn::vi::ListDisplays`, which the HLE left at
   zero; `QueryDisplayBufferMaxResolution` returned 1920 instead of an enumeration (0..3).
2. **Vulkan images shared by address**: the engine often describes the same memory with
   several texture objects (render target, then pool texture); they share one image.
3. **Initialization outside the render pass**: transitions and fills were emitted inside a
   dynamic rendering pass (validation error); bindings are done before the pass.
4. **Compute shaders**: decoding of `SUST`/`SULD` with a handle in a constant bank (`0xb9b`…),
   translation to format-less `imageStore`/`imageLoad` (`GL_EXT_shader_image_load_formatted`),
   bindless storage images, `vkr::dispatch`, `BindImages`.
5. **Window crop** (`nvnWindowSetCrop`) applied to presentation and to snapshots.

### Architecture

* `shaders/`: `nvshader.py` (control section 0xE0 bytes + SPH v4 header 0x80 bytes), `sass.py`
  (SM 8.x decoder, 128 bits), `glsl.py` (SASS → GLSL: `switch (pc)` loop, `uint` registers,
  constant banks as std430 SSBOs), `shaderc.py` (batches → SPIR-V through the MSYS2
  glslangValidator, + `.meta` read by the runtime). **55/60 graphics stages and 9/170 compute
  stages** translated (the other compute shaders read global memory: `LDG`/`STG`).
* `runtime/os/nvn_cmd.cpp`: recording of command buffers (operation list),
  NVN state objects, programs, texture/sampler pools, handles.
  `nvn_exec.cpp`: execution at submission (states, counters, buffer → buffer copies,
  engine calls). `vk_render.cpp`: Vulkan engine.
* Engine: imported NVN pools (`VK_EXT_external_memory_host`, vertex, index and
  uniform buffers read in place), one Vulkan image per texture object (shared by address),
  dynamic rendering, cached pipelines (hash of the states), bindless descriptors indexed by
  the NVN pool identifier (set 2: textures 0-6, samplers 8, storage images
  10-15), constant banks as SSBOs (sets 0/1), driver constants generated in a ring,
  presentation by blit onto a FIFO swapchain.

### Facts discovered (NVN2 / Switch 2)

* `nn::vi::DisplayInfo` = 0x60 bytes: name[0x40], bool, i64 layer count, i64 width, i64 height.
  `BufferMaxResolution`: 0 720p, 1 1080p, 2 1440p, 3 2160p (engine table).
* Graphics: UBO of slot k → bank **k+3** (as in compute; k+2, adopted at first, read the
  matrices from the wrong buffer); bank 1 = program constants (`BRX` jump tables);
  bank 2 = handles (see "The cutscene is displayed"); bank 0x18 = driver constants
  (1/target size, size, viewport as integers).
* Compute: bank 0 = driver (image handles starting at 0x10, or 0x20 if a UBO is read),
  UBO of slot 0 → bank 3 (verified on 3 shaders).
* Images and textures share the pool's identifier space (`RegisterImage`). `SUST`/`SULD`:
  dimension 0 1D, 2 1D array, 3 2D, 4 2D array, 5 3D.
* The game fills 821 textures directly in memory (data already in GPU format) and uses
  only 11 `CopyBufferToTexture` calls.

### Beyond the title screen (2026-11-08, 4th pass)

**The "Wwise deadlock" was a lost wakeup in the emulated kernel.** The SDK's `nn::os` tests the
condition variable's word in user mode: `InternalConditionVariableImplByHorizon::Signal()` /
`Broadcast()` do not make the `SignalProcessWideKey` system call if it is 0. Our
`WaitProcessWideKeyAtomic` released the mutex **and then** wrote 1 to that word: a thread that took
the mutex between the two signaled into the void and the waiter slept forever. The Horizon kernel
writes the word before releasing the mutex (`KConditionVariable::Wait`); `kernel.cpp` now does the
same. Decisive clue: at the hang, the event awaited by `AK::AudioMgr` (`main+0xf0f3ca0`) had
its "signaled" flag set to 1 while the thread was still asleep on its condvar. The other forms of the
hang (`AK::BankMgr` waiting for a read by the `AK::IOThread` thread to finish) had the same
cause. Result: 12 launches out of 12 get past the title screen (prologue at ≈ 8.5 s, loading of the
`map_tutorial` tutorial at ≈ 31 s, then of the `corneria_briefing` opening cutscene), at 60 fps.
The rare hang at startup (limitation 16) has not appeared since.

**Packaged textures**: uploaded on first use (`upload_packaged`, `vk_render.cpp`). All of the
game's packaged formats are BCn (BC1, BC3, BC4, BC7) or uncompressed: no ASTC. The
layout was established from the data (raw dump, search for the permutation that gives
the smoothest image, then visual verification of 11 textures and their mips):

* GOBs of 64 bytes × 8 rows; the 16-byte unit (bytes `x[0..3]` of a row) is ordered by
  `y0` (bit 4), `y1` (bit 5), `x4` (bit 6), `y2` (bit 7), `x5` (bit 8) — **not** the Maxwell order
  (`y0, x4, y1, y2, x5`);
* blocks `bh` GOBs high, in columns one GOB wide; `bh` of a level =
  min(2^L, smallest power of 2 of GOBs covering the level, 16 at most), where L is the first
  byte of `NVNpackagedTextureLayout` (4 in general, 0 for non-power-of-2 sizes);
* mip levels one after another (width aligned to 64 bytes, height to 8·bh rows).

`nvnTextureBuilderSetPackagedTextureLayout`, `SetStride`, `SetSwizzle` (applied to views:
`RGB1`, `RRR1` for BC4) are implemented.

**Shader translator** (131/317 stages, up from 120): 8/16-bit `F2I` and f16 source; `BREAK`
(no effect per thread, like `BSSY`/`BSYNC`); shadow lookups `TEX.DC` / `TLD4.DC`
(`sampler*Shadow`, comparison samplers on the Vulkan side); `TXD` (`textureGrad`, operands:
coordinates then layer, interleaved derivatives); `HSETP2`; `BRX` (indirect jump: target = address
read from **bank 1**, jump table). A program's bank 1 = the constants placed after its
code (`code + ctl[0x40]` to `code + ctl[0x44]`): `RECOMP_DUMP_SHADERS` dumps it (`.cb1`) and the
translator embeds it as a constant array. Dumps are named by hash (`h<hash>_<i>`).
Forms `0xf60` / `0xf6c` (TEX / TXD, bit 91): provisionally decoded as `0xb60` / `0xb6c`.

**Image**: the cutscene runs (1024² shadow cascades, 8192² shadow atlas, main pass
2560×1440 R11G11B10F + RG16 + D32S8, SSAO and volumetric fog in compute), ≈ 680
draws per frame, ≈ 70 of them skipped, but the image stays empty: the main material
(`main.shader_graph[standard]`) is not translated yet (limitation 22).

### The cutscene is displayed (2026-11-09, 5th pass)

**Handles and banks (established from the shaders' reads and the game's bindings).**

* UBO of slot k → bank k+3 in graphics as in compute. This is the fix that made
  the first real geometry appear (the space scene of the title screen). It was verified by
  recomputing a vertex shader on the CPU (`vsemu.py`) with the buffers dumped by
  `RECOMP_DRAW_DUMP`.
* Bank 2 (0x660 bytes, built for each draw):

  | Offset | Contents | NVN binding |
  |---|---|---|
  | `0x20 + 8k` | 32 combined handles | `BindTextures` |
  | `0x120 + 8k` | 8 images | `BindImages` |
  | `0x168 + 8k` | 128 separate textures (0x160 before the 6th pass: wrong) | `BindSeparateTextures` |
  | `0x568 + 8k` | 32 separate samplers (0x560 before: wrong) | `BindSeparateSamplers` |

  Handle = texture identifier (20 bits) | sampler identifier << 20.
  `nvnDeviceGetSeparateSamplerHandle` therefore returns `id << 20`, and the shaders combine the two
  handles with an OR before a bindless `TEX` (`0x361`, handle as the last operand).
* Forms `0xf60` / `0xf6c` (bit 91): handle in a **pair of uniform registers**
  `URn` (texture | sampler << 20) / `URn+1` (sampler), n = bits 40-45. Fixed in
  the 6th pass (see below): it was not an index of a bound texture.
* `FMUL`: `pdiv` field = scaling by 2^(pdiv−4) (4 = ×1).
* Depth: infinite projection with reversed Z and a top-left origin. The game never calls
  `SetDepthMode`, hence the default [0, 1] range.

**The "black" cutscene was a lost GPU.** Around 38 s (frames 1,800 to 2,080 depending on the
timing), a command batch never completed. Windows reset the GPU after 2 s
(TDR) and the Vulkan device was lost, silently. All the following frames were black
and the readbacks zero: shadow maps "without depth", imported pools read as zero by the
GPU. The diagnostic chain:

1. GPU readback of the dumped draw's buffers: everything is zero;
2. periodic check of the pools: the break is dated, right after 2 s without a response;
3. return codes: `VK_ERROR_DEVICE_LOST`;
4. NVIDIA markers and serialized mode: the faulty draw is a 36-index cube of the material
   `main.frag.glsl` (vertex `ea1055854ca01c0b`, fragment `00f1ea3b370dd924`) in the main
   pass;
5. neutralization of its texture reads by bisection, then measurements inside the shader
   itself (`RECOMP_DBG_SSBO` buffer).

Fixes, all kept:

* **`CALL.REL.NOINC`** (bit 86 of `0x944`) is a jump, not a call. This is the case for the 95 dumped
  `CALL`s: this is how the compiler exits divergent loops (`@P CALL.NOINC exit ; BRA head`)
  and calls subroutines whose return address is in a register. Pushing them onto the stack
  overflowed `cstack[8]` in nested loops. **`RET.REL.NODEC Ra`** = indirect
  jump to `Ra` (address stored by `MOV` before the call).
* **Dummy descriptors**: the 12 bindless arrays (16,384 slots each) and the
  4,096 samplers first receive a black 1×1 image of the right type and a default
  sampler. A slot that was never written and then read made the GPU read address 0.
* **Non-uniform combinations**: `nonuniformEXT(sampler…(T[…], SMP[…]))`, because the result
  of `OpSampledImage` must itself be `NonUniform`. Identifiers are clamped to 16,383.
* **`robustBufferAccess2` / `robustImageAccess2`**: an out-of-range read from a bank returns
  0, like `LDC` on the hardware, and not the contents that follow in the imported pool.
* Image barrier (GENERAL → GENERAL) on each target at the end of a render pass.
* **Safeguard in the dispatcher of shaders with loops**: beyond one million block
  transitions, the invocation stops. With this addition alone, the faulty shader no longer hangs
  the GPU. The safeguard has never triggered, and no infinite loop was measured: any
  modification of the shader was enough to make the failure disappear, and the exact cause in the
  driver remains unknown (limitation 24).

Result: two complete runs (75 s) without losing the device, at 60 fps. The
opening cutscene is displayed: scenery, characters, lighting and shadows. The shadow
cascades of frame 2401 now contain real depths (up to 0.30, versus 0
before). Visible defects: "woven" patterns on many surfaces, a blue sphere,
overexposure (auto-exposure is computed by compute shaders that are not yet
translated), see limitation 25. Translator: 153 stages out of 317 (graphics 139/144).

### Rendering fixed (2026-11-09, 6th pass)

Starting point: the "hideous" cutscene (upside-down image, woven patterns, black room).
Each cause was isolated on the pass captures (`RECOMP_RT_SNAPSHOT=N+K`) and the frame log.
Ten fixes, all established from the game's data:

1. **Orientation (viewport)**. NVN follows `glClipControl`: with an upper-left origin, normalized Y +1
   goes to the top row of the image, as in D3D, whereas Vulkan puts −1 there. With a positive
   viewport height, the scene was stored upside down in memory. The full-screen passes that
   read their UVs from `gl_FragCoord` (distortion) kept it as is. Those that read them from a
   vertex attribute (motion blur, final scaling) flipped it. Fix: viewport `{x, y + h, w, −h}`
   with an upper-left origin. The holographic table is now in the foreground at the bottom, the
   Corneria hologram above it and the "Skip" prompt at the top right. Diagnostic:
   `RECOMP_VIEWPORT_VK=1` restores the old convention.
2. **Attribute interpolation (IPA), the cause of the "woven" patterns**. The hardware interpolates
   attribute/w in screen space. The shaders then multiply back by 1/`gl_FragCoord.w`, that is by w
   (verified on the main material and the motion blur). But the Vulkan "smooth" input is
   already perspective-corrected: the UVs of 3D objects ended up multiplied by their
   depth. The translator now reads the IMAP from the SPH header (2 bits per component, at
   0x18 + k):
   * perspective → `fin * gl_FragCoord.w`;
   * linear in screen space → `noperspective`;
   * constant → `flat`.

   Across the 317 dumped stages: 554 P components, 72 L, 5 C.
3. **Clears**. `ClearColor` and `ClearDepthStencil` went through `draw()`, which requires a
   graphics program. Right after a compute dispatch (volumetric fog, LUT, BRDF), they were
   skipped, and the targets kept the contents of the previous frames, which hid other
   defects. They no longer depend on the bound program.
4. **Depth-only draws** (`vui_depth_mask`, no fragment shader, color targets bound): they
   were skipped; they are now executed with a zero color write mask.
5. **The `0xf60` / `0xf6c` form of `TEX` / `TXD`**. Field 40-45 designates a pair of uniform
   registers, not a `BindTextures` slot. Just before it, the shader computes
   `UR4 = c[2][separate texture] | c[2][separate sampler]` then `UR5 = UR4 >> 20`. The
   proof: a shadow lookup, a 2D array and a 3D texture of the main material all used "4",
   that is 23 lookups on the same handle.
6. **Layout of bank 2**: separate textures at `0x168 + 8k`, samplers at `0x568 + 8k`. With
   these bases, every material lookup lands on a texture of the right type:
   * D16 8192² or D32F 8192×4096 shadow map for the comparisons;
   * 2D arrays, 3D volumes, 64×64 LTC tables;
   * fog volume (88×50, 3D) for the sky and the holographic surfaces;
   * bound samplers (ss10-12).

   With 0x160 / 0x560, the shadow comparison read a 32×32 3D texture and the sampler read
   an empty slot. This is the fix that lit the room again.
7. **Bank 0 of compute shaders**, read from the control section of each stage. The layout is
   as follows:
   * a 0x10-byte header;
   * 16 bytes per uniform buffer (count at byte 0xb1 of the control section);
   * 8 bytes per combined texture (byte 0xb3);
   * 8 bytes per image (byte 0xb6).

   The total size is word 0xb8, verified on the 10 translated compute shaders. Examples:
   * text SDF (2 buffers, no texture): image 0 at 0x30;
   * SSAO: t1 at 0x28, image 0 at 0x30;
   * fog integration: t0 at 0x20, image 1 at 0x30;
   * accumulation clear: images 5-7 at 0x48-0x58.

   The old heuristic ("images in offset order") wrote to images 0-2 instead of 5-7, and no
   texture handle was supplied to compute shaders. The translator now writes the
   `c0 nubo ntex nimg` and `texh bank:offset` lines into the metadata.
8. **`FSET` returns 1.0 / 0.0 (.BF)**, not a 0xffffffff mask. The 40 dumped `FSET`s have
   bit 72 at 0 (the bit taken for .BF until now) and are all used as floating-point operands. The
   mask, a NaN as a float, propagated through the whole fur lighting (`fur_vertex_color`): General
   Pepper's head was black. Found by instrumenting the shader: each invocation stores the
   first floating-point instruction that produces a NaN (`0x1a0 : fma(R15, R3, R0)`, R15 = `FSET`).
9. **`CopyBufferToTexture` and `ClearTexture` executed**. They were recorded and then ignored.
   * This is a Vulkan copy from the imported pool, in command order, with a region clamped
     to the level: a copy outside the image caused the GPU device to be lost.
   * The level, the layer and the key of the texture are frozen at recording time. The game reuses
     a single view object, which it modifies before each copy: when it was read at execution time,
     all the copies of the 512×512×128 BC7 array targeted its last level.
   * They fill the 64×64 LTC tables, the default textures and this array, which is streamed level
     by level.
   * Exception: the 1×1 textures (limitation 28).
10. **Texture pool views applied**. The view passed to `nvnTexturePoolRegisterTexture`
    (swizzle, level and layer range, depth / stencil mode) was ignored:
    every texture was sampled whole, with only its creation-time swizzle.

Also:
* invariant positions in vertex shaders (`invariant gl_Position`, `precise` registers,
  no FMUL + FADD fusion). The "standard" pass tests depth for equality
  against the pre-pass, which is drawn with a different vertex shader;
* `gl_FrontFacing` converted exactly (`uintBitsToFloat(cond ? ~0u : 0u)`), no more NaN
  constant.

Diagnostics added:
* every skipped draw or compute dispatch is logged once, with its cause and its program
  ("dessin ignoré (shader de calcul absent) : …");
* vertex buffers that cannot be found are logged;
* `RECOMP_DRAW_WATCH` prints the blend functions and the blend color;
* `RECOMP_DEPTH_EQUAL_AS`, `RECOMP_FLIP_FRONT`, `RECOMP_NO_DEPTH_ONLY`.

The magenta of the TAA target (format 42) is normal: the history is stored in YCoCg, and the game
clears it to (0.5, 0, 0.5).

Result: the cutscene is the right way up, textured, lit and shadowed, and the text is sharp. It
shows General Pepper (fur, cap, uniform, medals), the briefing room and the Corneria
hologram. Only one compute dispatch is still skipped per frame: `volumetric_fog_light`
(global memory). What remains: a gray haze in ellipses (limitation 25), black blocks in the
half-resolution depth-of-field blur pass (invisible in the final image), and a frame rate of 46 to
59 fps depending on the shot (limitation 27).

Diagnostic tool: `RECOMP_DRAW_WATCH=hash RECOMP_DBG_SSBO=1` and a hand-instrumented shader from the
cache. The ad hoc scripts add a line before the instruction at the given address and
recompile the `.spv`. The stored values are read from the `[dbg]` log, with a `.orig` copy
to put back in place afterwards.

### Settings: `recomp.ini` (interface with the future launcher)

`build/full/recomp.ini` is read at every launch and created with the default values if it is missing.
It is the intended point of contact between the launcher and the executable: the launcher writes this file
(or passes another one with `--config <file>`), then starts `recomp_main.exe`.

```ini
[display]
refresh_rate = 60          ; 60, 90 or 120 Hz (30 to 240 accepted, other values untested)
resolution = auto          ; auto (the host's primary display), 720, 1080, 1440, 2160 (or 4k)
native_resolution = true   ; false: rendered at 50% then upscaled, as on the console
window_mode = borderless   ; borderless (borderless full screen) or windowed

[game]
language = auto            ; auto (Windows language), fr, en-US, en-GB, de, es, es-419, it, nl, pt, pt-BR,
                           ; ja, ko, zh-Hans, zh-Hant: code returned to the game by nn::oe::GetDesiredLanguage
```

* The game language (added on 2026-11-09 at the user's request, also `RECOMP_LANGUAGE` and
  `--language`) is the one the game receives in place of the console's language. Verified: `en-US` puts the whole
  menu in English. According to the Nintendo store, the game includes Japanese, English, Spanish, French,
  Portuguese, German, Italian, Dutch, Korean, Simplified and Traditional Chinese; it picks the closest one.

* Increasing priority: default values < file < `RECOMP_REFRESH`,
  `RECOMP_RESOLUTION`, `RECOMP_NATIVE_RES` variables < `--refresh`, `--resolution`, `--native-res` options.
* Booleans: `true/false`, `1/0`, `yes/no`, `on/off`, `oui/non`. Sections and comments
  (`;`, `#`) are ignored; a UTF-8 BOM is accepted.
* An invalid value or an unknown key is reported in the log (`[cfg]`) and ignored; the
  log summarizes the settings in effect and where they come from:
  `réglages : 90 Hz [recomp.ini], résolution 1080 [recomp.ini], rendu interne 50 % [recomp.ini]`.
* A file passed with `--config` that does not exist is not created (default values).
* Code: `runtime/os/settings.h/.cpp`. The settings are read once: a change takes effect at the
  next launch.

### Performance, frame pacing and resolution (2026-11-08)

Result (title screen, RTX 5070 Ti, 2560×1440 host display at 165 Hz), average interval / worst case:

| Configuration | fps | ms per frame |
|---|---|---|
| default (1440p virtual display, native rendering, 60 Hz) | 60.0 | 16.67 / 17-19 |
| `RECOMP_RESOLUTION=1080` | 60.0 | 16.67 / 20 |
| `RECOMP_RESOLUTION=2160` (4K, 3840×2160 internal rendering) | 60.0 | 16.67 / 17.6 (60 s with no deviation) |
| `RECOMP_RESOLUTION=2160 RECOMP_REFRESH=90` | 90.0 | 11.11 / 11.9 |
| `RECOMP_RESOLUTION=2160 RECOMP_REFRESH=120` | 120.0 | 8.33 / 9.2 |

The real GPU costs only ≈ 0.4 ms per frame; the validation layer stays silent at 4K.

**120 Hz in-game (2026-11-09, 13th pass)**: on the title screen 120 fps holds, but not in a mission.
Test with `--refresh 120` at native 1440p, from the copy of the save to the start of Corneria
(`perf_120.log`, `RECOMP_PERF=1`): 100 to 121 fps in the menus and the briefing, 55 to 118 fps in
Corneria, 41 at worst. Average interval 9 to 14 ms (worst cases 17 to 52 ms). The graphics card is not the
only cause: 6.7 ms of real GPU work per frame (`RECOMP_GPU_PROFILE`, including 2.2 ms for the
fragment-less `shader_graph_default` depth pass), close to the 8.33 ms budget. The CPU side becomes the limit
before it does: the GPU thread (NVN → Vulkan translation) is busy 3.6 to 7.6 ms per frame, of which 2.8 to
6.7 ms in `nvn_execute` (mostly `DrawElements`). The user is staying at 60 Hz for now: to be
optimized later (draw translation, then DLSS for the GPU share).

What limited it to 50 fps, in the order in which it showed up:

1. **Synchronous translation**: each `nvnQueueSubmitCommands` translated the work to Vulkan on the
   game's render thread (≈ 3.5 ms). → `runtime/os/nvn_gpu.cpp`: a **GPU thread** that executes the
   submissions in order; **real `NVNsync`s** (pending as soon as a `FenceSync` is submitted,
   signaled when the GPU thread reaches it, after the Vulkan work has finished); real `nvnSyncWait`,
   `QueueFenceSync/WaitSync/Finish`.
2. **Hidden HLE costs**: `bind_handle` copied the entire texture pool (~800 descriptors) for
   every handle (1.8 ms/frame) → `Table::with_key` (access under lock without a copy);
   `mem_valid` called `VirtualQuery` (≈ 27 µs, 1.6 ms/frame for `ReportCounter` alone, and the
   game's render tasks paid for it too) → per-thread cache invalidated on every decommit.
   Outcome: GPU thread 4.1 → 0.8 ms/frame, the game's render tasks 3 → 0.7 ms.
3. **GPU clocks**: the `ReportCounter` counters carried ns since Windows boot,
   `nvnDeviceGetCurrentTimestampInNanoseconds` ns since launch → same base (19.2 MHz).
4. **Engine frame limiter**: without `out.desired_frame_time` (default 0), it targets ≈ 18.2 ms (open
   loop: period + unplanned work). The variable is injected as 0.004 into
   `veclipse_build.cvar` (virtual romfs, the `.nup` is not touched): the game produces frames as fast as it can
   and it is the **virtual display** that sets the pace, like the console's vsync:
   * the frame is displayed at the first free virtual vblank (the GPU does not wait: only presentation
     is aligned to it); `nn::vi::ListFrameHistory` reports these times (FrameInfo, 0x28 bytes: state 2,
     number, 0, display tick, end tick);
   * `nvnWindowAcquireTexture` waits if 2 frames are already waiting to be displayed (back-pressure
     from the 3 window textures);
   * the host swapchain is in MAILBOX mode (`RECOMP_HOST_FIFO=1` for FIFO).
5. **Resolution**: virtual display chosen from the host's display;
   `render.resolution_scale(_console) = 1.0` and `render.dynres_enabled = false` injected: internal rendering
   at the output resolution (the console renders at 50% then upscales; `RECOMP_NATIVE_RES=0` for that behavior).

Generic mechanism: `build/full/romfs_override/<file>` replaces a file of the romfs.
Rendering variables spotted in `main`: `render.dlss_allowed`, `render.dlss_preset`,
`render.dynres_*`, `render.present_resolution_x/y`, `draw.scene_resolution_scale_override`…

## Known limitations / blockers

1. **C++ exceptions**: 3,144 functions in main have an LSDA (personality imported from the SDK).
   If the game really throws exceptions, an unwinding that crosses the recompiled code will be
   needed (entry at the *landing pads* = mid-function entries, already supported,
   + discarding the host frames). To be handled with the HLE of `__cxa_throw`/`_Unwind_*` (Phase 2/3).
2. **Non-standard returns**: `ret` is translated to `return` (the caller is assumed to be the
   target). Verified in bench mode by the shadow stack (`RECOMP_CHECK_RET`); the SDK's
   `longjmp`/coroutines will have to be handled in the HLE.
3. **Data imports** (GLOB_DAT to SDK objects): for now they point into the
   thunk area; the HLE runtime will have to give them real storage.
4. **Unhandled instructions**: none in main. Still unimplemented (absent from main):
   FP16 arithmetic, a few rare saturating operations (`sqabs/sqneg`, immediate `sqshl`, `sqdmull`…),
   crypto, SVE; they are emitted as `rt_unimpl`, which stops and gives the address.
5. **Performance**: the CPU state lives in `ctx` (memory); every guest memory access may
   alias `ctx`, which prevents gcc from keeping the registers in registers. Next step:
   guest registers in local variables (written back/reloaded at calls), lazy flags.
6. **Coverage of whole-function tests** limited by the random states (many
   pointer faults); instruction-by-instruction fuzzing covers the semantics.
7. **FPSR** (cumulative exception flags) is not updated; FPCR.AHP not handled.
8. **svc in main**: main contains 4 real `svc`s (`svc 0x34` WaitForAddress, `svc 0x35`
   SignalToAddress). Done in Phase 2: the emulated kernel handles them like those of the SDK.

Open items after Phase 2:

9. **Rendering (next step, Phase 4)**: NVN is replaced at the API level (done in Phase 3), but
   the commands are not executed. What is needed: record the command buffers, handle the
   states, the textures (formats, "packaged"/block-linear layouts), the
   texture/sampler pools, and **translate the precompiled shaders** (the game's NVIDIA GPU binaries)
   to SPIR-V for a Vulkan backend. DLSS (subsdk2): stubs, the engine receives a
   1×1 resolution from `nvnDlssNetworkResize` (to be made consistent or disabled).
10. **C++ exceptions not handled** across the recompiled code. The unwinding (the SDK's libunwind)
    walks the *guest* stack correctly, but the installation of the final context (jump to the
    *landing pad*) will have to be intercepted: throw a host exception caught by the recompiled
    function that owns the landing pad (identified by its LSDA), which resumes at that label.
11. **Unhandled SDK instructions** (absent from main, emitted as `rt_unimpl`): mostly
    scalar SIMD (6,672 `?scalar simd` words, immediate `sqshl`, vector FP16…) and 4,752
    "undef" words (data in `.text`). They have not been reached so far.
12. **Simplified kernel**: one global lock, priorities and cores recorded but not applied,
    `MapMemory` by copy, no true shared-memory aliasing (a single mapped view).
13. **Services**: many stubs ("success + zeros"); `GetInfo 32` unknown;
    `ldr:ro`, `ectx:aw` and several new `am` commands are still just logged.

Open items after Phase 3:

14. **Resolved (2026-11-08)** — lost wakeup in `WaitProcessWideKeyAtomic`, see "Beyond the
    title screen". Original diagnosis, for the record:
    **loading of `prologue_cin` stuck in Wwise**:
    `AK::AudioMgr` (thread 282) holds the Wwise global lock (`main+0xf0f3b68`) and waits for a
    slot in the type 3 task queue (limit 1, counters at `main+0xf0f3c70`, limits at
    `+0xc90`); `AK::BankMgr` (thread 280) is loading a bank and waiting for a read from
    `AK::StreamMgr`, whose I/O thread is idle. The 3 Wwise workers sleep on their
    semaphore (count 0). With no bank loaded, the prologue does not load ("Async hint"
    absent) and pressing A does not start the tutorial. It is a **race**: already present before
    the rewrite of the kernel waits (18:49 run), got through only once (18:45).
    Leads: scheduling by core and priority (the emulated kernel runs everything in parallel),
    StreamMgr's timing heuristics, the game's low-level I/O hook.
15. **Frame pacing**: resolved — stable 60 fps (120 with `RECOMP_REFRESH=120`), see "Performance,
    frame pacing and resolution". With no geometry on the title screen, the real load of a gameplay
    scene remains to be measured.
16. **Rare hang at startup**: seen ≈ 3 times in about fifty launches on
    2026-11-08, always around 0.95 s right after thread 260 is created (before `vrender_create`),
    then 0 times in 12 attempts with `--dump`. Not reproduced under observation; relaunching is enough.
    Probably the same lost wakeup as limitation 14: not seen again since the fix.
17. **Applets** (`ShowControllerSupport`, software keyboard, errors): not emulated; having a
    controller connected avoids the first one.

Open items after Phase 4 (rendering):

18. **Textures**: packaged textures are deinterleaved and uploaded (2026-11-08);
    `CopyBufferToTexture` and `ClearTexture` are executed (2026-11-09). Remaining:
    * non-packaged textures written directly by the CPU (neutral gray);
    * tracking of rewrites (upload on first use only);
    * packaged 3D textures;
    * the row stride of copies (`SetCopyRowStride`, data assumed tightly packed);
    * `EvaluateDlss` (to be done as a blit); `CopyTextureToTexture` is done (7th pass), including
      depth ↔ color copies (8th pass);
    * `CopyBufferToBuffer`: resolved in the 8th pass (GPU copy in command order).
19. **Resolved (2026-11-09, 7th pass)** — global-memory compute shaders, translated with
    `GL_EXT_buffer_reference` and a pool table in bank 0 (see "7th pass").
20. **Untranslated stages**: 80 out of 435 since the 8th pass, none requested in 10 min of play. These are
    79 tensor-core compute shaders, which make up the console's DLSS network, replaced by our HLE. The
    last one is a global-memory fragment shader.
21. **Resolved (2026-11-09, 6th pass)**: texture and image handles of compute shaders
    (bank 0), see "Rendering fixed". The rule "images after the last texture read" assumes
    that the last declared texture is read; to be watched on other compute shaders.
22. **Resolved (2026-11-09)** — separate texture / sampler handles: layout of
    bank 2 established (see "The cutscene is displayed"); the main material is translated.
23. **Integer vertex attributes** (`R8G8B8A8_UINT`…) read as floating-point `vec4` by the translated
    shaders (validation error `Input-08733`): to be typed according to the format at pipeline time.
24. **Exact cause of the cutscene TDR unknown**: the dispatcher's safeguard avoids it
    (two complete runs), but no infinite loop was measured and any modification
    of shader `00f1ea3b370dd924` was enough to make the failure disappear. It is probably
    the driver's compilation of these very large shaders (≈ 200 variables live across
    the `switch (pc)` dispatcher). A structured control-flow output (loops and `if` instead
    of the dispatcher) would reduce the pressure on registers. If a hang reappears:
    `RECOMP_GPU_DIAG=1 RECOMP_GPU_SERIAL=1` identifies the draw.
25. **Visible defects of the cutscene**. Fixed in the 6th pass: "woven" patterns, flipped
    image, black room, blue sphere, black fur. Fixed in the 7th pass: stepped
    rings (inverted NVN filters) and gray haze (volumetric fog not translated). Fixed in the
    8th pass: black blocks in the depth-of-field blur (target views whose mip level was ignored).
28. **Resolved (2026-11-09, 8th pass)**: 1×1 textures and blocky text. `nvnQueueFinish` did not
    submit the recorded Vulkan work, and the game reused its staging memory too
    early (see "8th pass"). The neutral-gray workaround has been removed.
27. **Cutscene frame rate: resolved** (8th pass), stable 60 fps at 1440p. The banks have been
    moved to uniform buffers and submission has become asynchronous. What remains: stutters at
    scene changes (textures uploaded on first use, pipelines created synchronously).
    Leads:
    * background texture preparation;
    * persistent pipeline cache;
    * background pipeline creation;
    * cheaper shaders (structured output instead of the dispatcher, limitation 24).
26. The pool audit (`RECOMP_POOL_AUDIT`) shows that the GPU does read the game's memory in
    place as long as the device is alive: the `VK_EXT_external_memory_host` import is reliable,
    with no copy and no re-read.
29. **Resolved (9th pass)**: GPU crash at the start of the training mission (write to address 0). A depth
    image was used as a color target, because two texture objects with the same address shared one
    Vulkan image regardless of their format (see "9th pass"). The buffer initialized with a null pool had
    nothing to do with it.
30. **Holographic towers of the training mission** (`s_tutorial_procedural_building01`): resolved (13th pass).
    Only their edges were visible; their vertex shader (`7f3471f3fcda1e01`) performs a rotation using
    sine and cosine, and these were translated in radians whereas the GPU takes them in turns.

### Is clang/LLVM needed?

Not for now: gcc 16 (mingw) compiles the generated code without problems, handles
`[[gnu::musttail]]` (guaranteed tail calls, even at -O0) and 128-bit `__atomic` (`-mcx16`,
libatomic). clang would become interesting for: (a) the compile speed of the ~4 GB of C++
of main (to be measured), (b) `[[clang::preserve_none]]`/custom calling conventions to
pass guest registers in host registers, (c) the MSVC ABI if we link against MSVC libs
(Vulkan/SDL also work with mingw). Recommendation: install LLVM when we tackle the full
generation of main and compare the times.

### Rendering fixed (2026-11-09, 7th pass: review of the whole cutscene)

- **Sampler filters**: the NVN enumerations are 0-based, in GL order (min 0 NEAREST, 1 LINEAR,
  2 NEAREST_MIPMAP_NEAREST, 3 LINEAR_MIPMAP_NEAREST, 4 NEAREST_MIPMAP_LINEAR, 5 LINEAR_MIPMAP_LINEAR; mag 0 NEAREST,
  1 LINEAR; wrap 0 CLAMP, 1 REPEAT, 5 CLAMP_TO_BORDER, 6 MIRRORED_REPEAT, 7 CLAMP_TO_EDGE). Established on the
  game's 11 samplers (`RECOMP_SAMP_LOG=1`). The old table swapped linear and nearest: the 3D display LUT read with
  nearest filtering produced the elliptical rings in steps (6 steps → continuous gradient, verified on
  frame 3600). `RECOMP_OLD_FILTERS=1` restores the old table (diagnostic).
- **Robust presentation**: swapchain recreated when it is out of date (window minimized or resized); with no
  swapchain image, the frame's work is submitted anyway and the snapshots continue (they used to stop at 134 s).
- `RECOMP_DRAW_WATCH` prints the sampler of each texture (`éch. id min/mag`).
- **Missing shaders**: 50 shaders from parts 2 to 5 and from the training were missing (skin, hair, Venom terrain,
  effects; up to 887 draws skipped per frame after the cutscene). Dumped again over 10 min of gameplay
  (`RECOMP_DUMP_SHADERS=1`) then translated: **351 stages out of 435** (153 before), no draw is skipped anymore.
- **Compute global memory** (`LDG`, `STG`, `ATOMG`, `RED`) translated with `GL_EXT_buffer_reference`. Guest
  addresses are host pointers into the NVN pools; they come from the buffer descriptors of bank 0 or from
  game data (the fog light reads pointers stored in a uniform buffer). The runtime imports
  all NVN pools (`VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT`, `VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`) and writes
  a table at the end of bank 0 (0x600): number of pools, then guest base, Vulkan address and size. Every access
  goes through `gxl()`, which first tries the last pool found. Outside the pools: reads return 0 and writes are
  ignored, so there is never a GPU fault. 8/16-bit accesses within the 32-bit word (writes via
  `atomicAnd`/`atomicOr`), 32-bit atomics (INC, DEC and floating-point addition by compare-exchange).
  `RECOMP_NO_GLOBAL_MEM=1` skips these compute shaders.
- **Compute bank 0, full layout**, verified against the total size (byte 0xba) on 0098, b775, ff25 and
  the fog light: 0x10, inline data (byte 0xa9, in words, rounded to 16 bytes), 16 bytes per uniform
  buffer (0xb1), 16 bytes per storage buffer (0xb2: guest address, size), 8 bytes per texture (0xb3), per
  separate texture (0xb4), per separate sampler (0xb5), per image (0xb6). `c0` line of the `.meta` files: 7 numbers.
  The inline data comes from `nvnCommandBufferPushComputeConstants`, now copied at record time
  (never called in the cutscene: the stages concerned are not executed there).
- **Image atomics** `0x593` `SUATOM` (with return value) and `0x59f` `SURED`, handle in a constant bank:
  `imageAtomic*` on an `r32ui` declaration of the same array. Established on `fog_noise_height`, which accumulates
  density and 10:10:10 color in a 3D image, then saturates with OR.
- **Complete volumetric fog**, which removes the gray veil: `volumetric_fog_light` (global memory) and
  `fog_noise_height` (image atomics) now run. Without them, the integrated volume contained garbage and
  added a gray haze that grew with distance, hence a light gray space sky and washed-out objects.
- **Dynamic texture offsets**: `TLD4.AOFFI` via `textureGatherOffset` (6 signed bits at bits 0 and 8),
  `TEX`/`TLD.AOFFI` (4 signed bits per component) added to the coordinates; 8/16-bit shared memory and 32-bit
  `ATOMS` (signed min/max by compare-exchange).
- `RECOMP_DISPATCH_LOG=1`: each compute program executed (once), with the layout of its bank 0.
- **`CopyTextureToTexture` executed** (skipped until now): `vkCmdCopyImage` when the sizes are equal, otherwise
  `vkCmdBlitImage` (NVN flags 1 linear filter, 4 / 8 mirrors). The game uses it to copy each 1024×1024 shadow map
  into the 8192×4096 depth atlas: without the copy, the tiles kept stale shadows, hence intermittent dark blocks
  (holographic planet) and a briefing room with no cast shadows or light shafts.
  `RECOMP_NO_TEX_COPY=1` restores the old behavior (diagnostic).
- **Pixel-for-pixel presentation**: a bordered window the size of the screen only has a reduced client area
  (2560×1421 on a 1440p screen); the 2560×1440 image was downscaled into it with linear filtering, hence blurry and
  slightly squashed. New setting `window_mode` (`borderless` by default, or `windowed`), `RECOMP_WINDOW`,
  `--window`; Alt+Enter or F11 toggles in game. Nearest copy when the sizes match, aspect ratio
  preserved (black bars) otherwise; swapchain recreated when the window changes size.
- **Still untranslated (84 stages, never requested in 10 min of gameplay)**: 78 tensor-core compute shaders (`LDSM`
  `0x83b`, `HMMA` `0x23c`/`0x23a`/`0x237`/`0x240`), opcodes `0x8a5`, `0x3a1`, `0x3c4`, `0x2a5`, and one fragment
  shader with global memory (address in graphics bank 0, 0x5b0). See the 8th pass: 80 remaining.

### Fixes and frame rate (2026-11-09, 8th pass)

**Remaining defects fixed**

- **Depth blur (black blocks)**: render target views ignored their mip level and their layer.
  The passes that build the depth pyramid (SAO, blur) were all writing to level 0. The level and the
  layer of each view are now captured when `SetRenderTargets` is recorded, because the game reuses its
  view objects. `render_view()` creates the matching Vulkan view, and the draw takes the size of that level.
- **Depth ↔ color copies** (`CopyTextureToTexture`): routed through an intermediate buffer
  (`vkCmdCopyImageToBuffer` then `vkCmdCopyBufferToImage`), because Vulkan forbids `vkCmdCopyImage` between these
  aspects.
- **`CopyBufferToBuffer` in GPU order**: the copy is done on the GPU, between the imported pools.
  The source is snapshotted at record time, up to 4 MB. The CPU no longer performs the copy at
  submission time, ahead of the work that precedes it. `RECOMP_CPU_BUFFER_COPY=1` restores the old behavior.
- **Shaders: 355 stages out of 435.** New decodings:
  - `UIMAD.WIDE` (`0x2a5`, `0x8a5`), uniform `UFLO` / `UPOPC` / `UBREV` (`0x0bd`, `0x0bf`, `0x0be`);
  - `REDUX` (`0x3c4`, warp reduction into a uniform register: `subgroup*`), operation in bits 78-80;
  - `0x3a1`, read as `ATOMS.POPC.INC`.

  The remaining 80 are not used:
  - 79 tensor-core compute shaders (`LDSM`, `HMMA`). These are the layers of the console's DLSS network (API
    `nvnDlssNetwork*`). Our HLE replaces DLSS with rendering at native resolution, so they are never launched.
  - one fragment shader with global memory, unused.
- **1×1 textures and text rendered as blocks: cause found** (former limitation 28). `nvnQueueFinish` did not submit
  the Vulkan work already recorded. The game, believing the GPU was up to date, reused its transfer memory, and the
  copies of the default 1×1 textures read other data. The fix:
  - `nvnQueueFinish` now submits and waits;
  - the sources of `CopyBufferToTexture` are snapshotted at record time (4 MB at most).

  The workaround is removed: the real content is copied and the text is correct. `RECOMP_FINISH_NO_FLUSH=1`,
  `RECOMP_COPY_IN_PLACE=1` and `RECOMP_SKIP_COPY_TEX_1X1=1` reproduce the old behavior.

**Frame rate: from 42-53 fps to a stable 60 fps (cutscene, 1440p screen, RTX 5070 Ti)**

`RECOMP_GPU_PROFILE=N` measures, with timestamps, each draw and compute dispatch of frame N. Initially, frame 2700
cost **18.43 ms of GPU time**. The main material took 5.84 ms of it (29 draws), depth-only 2.99 ms
(392 draws), the fur (`fur_vertex_color`) 2.6 ms. Rendering was therefore limited by the cost of the shaders, then
by the wait for the GPU on the translation thread.

1. **Constant banks as uniform buffers** (`glsl.py`, `create_layouts`).
   - **Before**: each `c[bank][offset]` read by a shader was a load from a storage
     buffer. On the console, it is a direct operand of the instruction.
   - **Now**: each bank is a 64 KB `uniform` block in `scalar` layout (`uint d[16384]`).
     The NVIDIA driver reads these blocks through the constant cache, and a known offset becomes a direct
     operand again.
   - **Result**:
     - frame 2700: 18.43 → **9.7-10.6 ms** of GPU time;
     - main material: 5.84 → 1.19 ms;
     - depth-only: 2.99 → 1.48 ms;
     - pre-pass: 1.55 → 0.36 ms.
   - **Constraints**: the game's buffers are bound with 64 KB at most (`maxUniformBufferRange`). An address not
     aligned to `minUniformBufferOffsetAlignment` would be copied into the ring; this case has not been observed,
     NVN aligns to 256. Binding 31 remains a storage buffer for `RECOMP_DBG_SSBO`.
2. **Asynchronous submission** (`vk_render.cpp`, `Vk::Slot`).
   - **Before**: each `FenceSync` submitted the work then waited for the GPU to finish. Translation and
     GPU execution added up: 2 to 3 ms + 12.6 ms per frame, with missed frames (33-35 ms).
   - **The slots**: each recording takes one of 6 rotating slots, which holds a command
     buffer, its descriptor pools (chain extended if needed), its 16 MB constant ring and
     its intermediate buffers.
   - **Synchronization**: a slot is reused when a **timeline semaphore** shows that its
     previous submission has finished. A full barrier at the head of each buffer keeps the same execution order
     as before on the GPU.
   - **End of a submission**: a completion thread signals the game's `NVNsync` objects when the GPU has actually
     finished (`vkr::flush_async` + `vkr::when_done`).
   - **Presentation**: it no longer waits for the GPU. Acquisition uses one semaphore per slot and the end
     of the copy one semaphore per swapchain image.
   - **Cases that remain synchronous**: `nvnQueueFinish`; the diagnostics that read back from the GPU (captures,
     profiler, `RECOMP_DBG_SSBO`); `RECOMP_SYNC_FROM`, `RECOMP_GPU_DIAG`, `RECOMP_DRAW_WATCH`; and
     `RECOMP_SYNC_SUBMIT=1`, which restores the old behavior.
   - **Result**: the GPU thread goes from 14-15 ms to **2.3-2.9 ms per frame**. In the heavy scene,
     the average interval is 16.67 ms, with spikes at 18-27 ms instead of 33-35 ms. Rendering is
     unchanged on the snapshots checked (frames 2100, 2701, 3300, 3900 and 4500).
3. **Deswizzling of large textures on several cores** (strips of ≈ 1 MB, 8 threads at most). It
   is used at scene changes, where hundreds of MB of textures are uploaded on first use. One
   spike went from 205 to 146 ms in one test.

**What remains**

- **Stutters at scene changes** (60 to 800 ms, including the game's own loading). They come from the textures
  uploaded on first use (creation, deswizzling) and from the pipelines created synchronously. Leads:
  - prepare the textures as early as `nvnTexturePoolRegisterTexture`, in the background;
  - use a pipeline cache.
- **Fur**: about 1.6 ms for a single pre-pass draw, because of fill rate (stacked layers
  with alpha test). This is the cost of rendering at native resolution: the console renders at about 50% then
  upscales with DLSS.
  - With a 6 ms margin at 1440p, 60 fps holds.
  - At native 4K (`RECOMP_RESOLUTION=2160`, measured on the cutscene), it holds too. Frame 2700 costs
    **13.0 ms of GPU time**, including 2.91 ms for the main material. The average interval is 16.67 ms, with
    spikes at 17-20 ms (a few at 31 ms) outside scene changes.
  - To go beyond that (4K at 120 Hz, heavier gameplay scenes): real DLSS (after V1: `EvaluateDlss`
    and the NVIDIA SDK) or `native_resolution = 0`.
- **Structured control flow** (limitation 24): the shaders keep the `while (true) switch (pc)` dispatcher;
  257 of the 355 have no loop and could be structured. This is the next GPU gain, but it is no longer
  needed for 60 fps at 1440p.

### Sound (2026-11-09)

The game (Wwise) writes its final mix into a single `nn::audio::OpenOutputStream` stream.
- **Format**: `StreamFormat` = {sample format, channels, sample rate, reserved}, i.e. 5, 2, 48000, 0:
  32-bit float, stereo, 48 kHz. The old code read these fields in the wrong order.
- **Writes**: the game writes in blocks of 512 frames.
- **Confirmation**: Lichii's log describes the same stream (`OutputStream-0=>2ch48kF32`).

**How it works**
- `hle_audio.cpp` gives each stream a 100 ms buffer (stereo float). It converts the game's format
  (8, 16 or 32 bits, float; mono duplicated, 5.1 downmixed) and applies the gain from `SetOutputStreamGain`,
  read from `s0`.
- `audio_out.cpp` opens the default Windows output in shared-mode WASAPI, event-driven, with a 40 ms
  buffer. The format is 48 kHz stereo float, converted by Windows to that of the sound card
  (`AUTOCONVERTPCM`).
- The render thread calls the HLE mixer for as many frames as the card requests. **The sound card's
  clock therefore paces the consumption of the game's streams**, with no drift between two clocks. The stream's
  event is signaled as soon as there is room.
- With no device, or if it disappears, the previous virtual clock takes over (the game never waits).
  The output is reopened every second.

**Measurements over 70 s** (title screen then cutscene; card: 48 kHz, 8 channels):
- 512 frames per write, exact consumption of 48,000 frames/s, **0 underruns**, nothing clipped;
- peaks from 0.02 to 0.31 depending on the scene, of the same order as in Lichii (0.17 to 0.19);
- the abrupt jumps observed are attacks of the game's sounds, not buffer seams.

`RECOMP_AUDIO_WAV` records the mix, which makes it possible to check without speakers.

**Opus**: the game creates 16 "hardware" decoders, mono, 48 kHz, 20 ms frames. No packet reaches them in
70 s. This profile is that of online voice (GameChat, disabled): the cutscene does not depend on it. These
decoders therefore always return silence. The packets are logged (`RECOMP_AUDIO_LOG`) in case
another part of the game uses them; libopus would then be needed, and it is absent from the machine.

**Limitations**: latency of about 100 to 140 ms (stream buffer plus Windows buffer). A change of
default device during play is not followed, unless the old one disappears.

### Checking against Lichii (reference emulator)

The official copy of Lichii (Eden-based) runs this game. Its CLI, `eden-cli.exe`, serves as the reference.

**Launch**
```bash
EDEN_NX2_HIDE_WINDOW=1 EDEN_NX2_KEY_SCRIPT="10:4,10.15:-4,12:4,12.15:-4" \
  eden-cli.exe --filter "*:Info Service.LM:Trace" -c copie-de-sdl2-config.ini -g "<jeu>.nup"
```
- `KEY_SCRIPT`: seconds:SDL code, negative = release; the A key is the A button.
- Configuration: a copy of `%APPDATA%\eden\config\sdl2-config.ini` with `audio_muted=true`, so that
  the test run plays no sound.
- The `Service.LM` filter makes the game's log ("Log Text:") appear in
  `%APPDATA%\eden\log\eden_log.txt`. These are the same lines as our `[lm]` lines: this lets us compare
  progress (package loads) without any picture.
- With the window hidden, the frame captures (`EDEN_NX2_FRAME_CAPTURE`, `EDEN_NX2_FRAME_DUMP`) do not
  trigger: they require a visible window.

**Result of 2026-11-09**
- Same startup sequence as the recomp (romfs, `nvn`, `bootstrap`, `global`).
- Lichii is in `NORMAL` mode (handheld) with an HDR display; we are in `BOOST` (docked) and SDR.
- Lichii has a save: there the game goes to the menu (`mp_rewards`, `post_venom_ending_flow`). With no save,
  the recomp starts the prologue.
- For audio, Lichii's log gives a peak every 2 s: title screen level 0.015-0.020,
  versus 0.020-0.023 on our side.

### Comparison with Lichii and fixes (2026-11-09, 9th pass)

Lichii's captures (visible window, `EDEN_NX2_FRAME_CAPTURE`) serve as the rendering reference. Lichii
itself has display defects at the bottom of the screen, unrelated to rendering: we ignore them.
`compare_frames.py` (scratchpad) matches each capture with the most similar recomp snapshot
(thumbnail correlation) and puts them side by side.

**Fixes**

- **Guest clock at 31.25 MHz** (counter frequency of the Switch 2; 19.2 MHz is that of the Switch 1).
  With 19.2 MHz, the game simulated only 61% of real time: the cutscene lasted 1.63 times too long.
  It now reaches the training at 228 s, versus 232 s in Lichii. `RECOMP_CNTFRQ` changes the value.
- **Controller read only when the window has focus.** A game being played elsewhere with the same controller
  was also driving the game: 17 presses of Start had paused the cutscene during a test.
  `RECOMP_BACKGROUND_INPUT=1` restores the old behavior.
- **Stale packaged textures** (noise veil over the training scenery). A
  new packaged texture created at the address and size of an old one reused the Vulkan image of
  the old one, with its content. Now, a 64-bit hash of the uploaded content is compared with
  memory: identical content, the image is reused; different content, it is uploaded again into the same image.
  Over the full cutscene: 675 textures and 2.6 GB uploaded, instead of 3,047 textures and 9.7 GB without
  reuse.
- **Checkerboard on the ship (ambient occlusion)**. In the training, the Arwing's blue wing was dark
  and covered with squares of about 16 pixels; in Lichii it is a uniform blue. The checkerboard was already
  present at the output of the opaque pass (`RECOMP_RT_SNAPSHOT`). With `RECOMP_SKIP=ssao`, it disappeared:
  the culprit was the ambient occlusion (SAO: depth pyramid, computation at one third of the resolution,
  two blurs, upscale).
  - **Cause**: the `LDS` / `STS` shared-memory instructions of recent architectures address
    `[Ra.Xn + URb + imm]`. The scale n is 1, 4, 8 or 16 (bits 78-79) and multiplies the register. The
    translator ignored it. The two SAO blurs store a pair of floats per thread at `[R28.X8]`:
    the 256 threads were writing over one another and reading their neighbors' values.
  - **Fix**: scale decoded in `sass.py`. `glsl.py` (`saddr`) multiplies the register and adds the
    uniform register of the `0x988` form. 11 compute shaders change meaning, including the two SAO
    blurs; the other 9 are not launched during the cutscene.
- **GPU lost at the start of the training (former limitation 29)**. The game reuses its memory for
  resources of different formats. The portrait of the dialogue window (306×265 targets) is rendered into
  memory that had been used for other images. Yet two texture objects with the same address and the same size
  always shared the same Vulkan image, in the format of the first one. A depth image
  (D32F+S8) was thus used as an RG16 color target, and an RGBA16 texture as a depth buffer: invalid
  usage, write to address 0 and loss of the GPU about 8 s after the start of the playable training.
  - **Fix**: `by_addr` keeps all the images created at an address. A new texture object shares
    an image only if the shape and the **memory layout** are the same (same format, or linear / sRGB
    variants: `same_layout`). A packaged texture placed on the memory of a former render
    target also has its own image, with its uploaded content. Otherwise, a new image, reused afterwards for
    that address and that format.
  - **Result**: the training is playable (rings, "Nice work, Fox", next lesson) without losing the
    GPU. The same fault also served a BC1 texture in a BC7 image from an earlier scene.

**Checks**
- Shot from the user's capture (Arwing in profile above the grid, frame 13,530 with
  `RECOMP_AUTOPRESS=1`): the wing is a uniform blue, as in Lichii.
- Full cutscene with both fixes: lines up with Lichii's captures (Peppy, Pigma,
  cockpits, approach to Venom), without losing the GPU.
- Training playable for 140 s without losing the GPU (before: loss about 8 s after it started).

**What remains**
- Holographic towers of the training (limitation 30).
- The other 9 compute shaders whose shared addressing changed are not launched during the cutscene:
  their effect will be visible later in the game, with no verification so far.

### All the game's shaders, without playing (2026-11-09, 10th pass)

After a first real 10-minute play session, 50 shaders were missing (53 invisible effects: thrusters, trails,
boost, charged shots, shield, Venom tanks, holograms, cockpit screens…). The dumps made in-game only
covered the parts already reached. The shaders are therefore now extracted directly from the romfs.

**Where the shaders are**
- `romfs/data.zip` (15.6 GB in the patch content) is a **ZIP64** archive preceded by a `VZIP` header.
  - 114,154 entries, whose names are 64-bit hexadecimal hashes.
  - Declared method 0, but the data is **chained 64 KB LZ4 blocks**: u32 compressed size (bit
    31: stored block), then the block, which can reference the previous 64 KB of decompressed data.
- **2,828 shader packages** (signature `0x19866891`, 40 MB in total). A package contains:
  - a header: u32 signature, u32 code size, u32 control section size, u32 0, u64 hash;
  - then the code, with constant bank 1 between `ctl[0x40]` and `ctl[0x44]`;
  - then the NVN control section (`0x98761234`).
- The hash `sha1(code[:ctl[0x38]])` is the runtime's: 795 packages match a shader already dumped in-game.
- The 18.7 MB `VIPR` file contains 165 other control sections, in another format (without the code next to
  them); probably the video player's programs, never requested so far.

**Tool**: `python -I shaders/romfs_shaders.py` reads the `.nup` read-only: the `data.zip` index, the
first block of each entry (≈ 2.2 GB), then the packages. It writes `h<hash>_r.ctl/.bin/.cb1` into
`build/full/shaders/`. Then `python shaders/shaderc.py -j 12` (about 40 s).

**Result: 2,388 stages translated out of 2,468** (355 before). The remaining 80 are not used: the 79
tensor-core compute shaders of the console's DLSS network, and one fragment shader that uses global memory.

Instructions added for the new shaders:
- `0xf63`: `TLD4` (gather) with the handle in uniform registers, like `0xf60` / `0xf6c`;
- `0xabb`: `ULDC` with a uniform index (`ULDC.64 UR4, c[0x6][UR4+0x10]`, offset table of a blur);
- `PIXLD`: sample count and coverage mask (our targets have a single sample).

**Safety net**: a shader that is neither translated nor already dumped is now written automatically
to `build/full/shaders/` during play ("shader inconnu", unknown shader, in the log); it is then enough to run
`shaderc.py` again.

**Check**: title screen, main menu, new campaign, Great Fox and General Pepper's call (scenes
never seen before): 0 missing shaders, 0 skipped draws.

**Save data**: this test navigated the menu and started a campaign that was recorded in the
player's save. `RECOMP_SAVE_DIR=folder` now keeps the tests' save separate.

### Wrong 3D models after a level change (2026-11-09, 11th pass)

Reported by the user in Meteo (asteroid field).

- **Cause**: each NVN memory pool is made visible to the GPU through a Vulkan import of host memory
  (`VK_EXT_external_memory_host`), then kept forever in `g.pools`. But an import shows the GPU the
  physical pages present at the time of the import. On a level change, the game frees its pools, unmaps
  their memory (`UnmapPhysicalMemory`), then remaps the same addresses for the new level's pools.
  `find_pool` found the old import covering those addresses: the GPU read the new level's vertices, indices
  and constants from the old pages.
- **Evidence in the session log**: from 470 to 479 s (loading Meteo), 9 remapped ranges were
  still covered by imports made at 393 s, with no new import. The full run from the cutscene
  to the training reproduces it too (2 ranges at 228 s).
- **Fix**: `nvnMemoryPoolFinalize` goes through the GPU queue (`PoolReleased` item, after the commands
  already submitted) and calls `vkr::pools_released`. The imports covering the pool's memory are removed
  from `g.pools`; any new use of those addresses makes a new import. The removed Vulkan objects are destroyed
  in `begin_frame`, once the GPU has passed the submission that could still read them.
- `RECOMP_POOL_AUDIT` no longer reads unmapped memory on the CPU side: it counts those samples separately
  ("démappés", unmapped), the sign of a stale import.
- **Verified** on the user's next session (11 min, up to Sector X): 19 pool releases,
  no remapped range read through a stale import (`remap_check.py` script on the log), no more
  progressive corruption. Only remaining visual defect reported: the charged shot in Sector X.

### Textures never freed (2026-11-09, 12th pass)

After the import fix, the user still saw the "shaders glitching little by little": rating and lives
icons turned into solid squares, scrambled mission thumbnails, opaque HUD backgrounds, squares
next to the models with the charged shot or when passing through rings.

- **Cause**: Vulkan images were never destroyed. Over a 30-minute session: 6,170 images and
  15 GB of textures uploaded, for a 16 GB card. Past the budget, image creations failed
  silently (`vkAllocateMemory` failing: `get_image` returns `nullptr`). Draws then used a
  dummy descriptor: an effect or an icon with a transparent texture became a solid square.
- **Fix**: `pools_released` also destroys the images whose memory is in the released pool. They are
  removed from `g.images`, `g.alias` and `g.by_addr`, and the Vulkan objects are destroyed in `begin_frame` when
  the GPU has passed the recorded submission. The open render pass is closed, and the descriptor caches
  (`tex_written`, `img_written`) are cleared: a destroyed view handle can be reassigned.
- **Log**: the periodic line now gives the number of images freed and the video memory used
  relative to the driver's budget (`VK_EXT_memory_budget`).
- **Visible allocation failures**: a `vkAllocateMemory` refusal for an image is now logged
  ("mémoire vidéo refusée", video memory refused, with the memory used and the budget), instead of letting the
  texture go missing without a trace.
- **Program catalog**: `build/full/shaders/programmes.txt` gets one line per game program
  (effect name, then `stage:hash` for each shader), when it is created. Since the shaders extracted from
  the romfs have no name, this file links a reported effect (e.g. `vfx_charged_shot_…`) to its shaders.
- **Destroyed texture objects**: releasing per pool is not enough if the game sub-allocates its textures in
  pools that it keeps. Each image now counts its live texture objects (`refs`: the one that
  created it, plus those that share it). `nvnTextureFinalize` goes through the GPU queue (`TextureReleased`) and
  drops one reference. With no reference left, the image remains reusable by address (textures recreated over the
  same content, uploaded only once) in a cache capped at 1.5 GB. Beyond that, the oldest ones are
  destroyed, provided no draw has read them for 300 frames (5 s): the game sometimes copies its
  texture objects, and a copy can outlive the original. The periodic log line gives the size of
  this cache ("Mo sans objet", MB without an object).
- **Verified** on the opening cutscene then the training (14,880 frames, test save):
  video memory bounded at about 2.6 GB (the previous executable climbed to 3.9 GB at the same point, and
  without limit afterwards), 257 images destroyed, object-less cache under 1.5 GB, no allocation refusal and no
  device loss. The 17 screenshots paired with Lichii's are identical in content (HUD, portraits,
  controller, holographic city): no texture still in use was evicted.

### Sine and cosine in turns (2026-11-09, 13th pass)

The user saw "a small shader bug on all the circle-shaped holograms" (holographic spheres of
the planets on the map, rendered pinkish and without a gradient).

- **Cause**: since Volta (no more `RRO`), `MUFU.SIN` and `MUFU.COS` take the angle in **turns**: the
  compiler first multiplies the angle by 1/(2π) (`FMUL.RZ … 0.15915494`, `0x3e22f983`), and the hardware
  computes sin(2π·x) on the fractional part. `glsl.py` translated it as `sin(x)`: every angle was therefore
  divided by 2π. Of the 2,101 sines and cosines in the translated shaders, 2,096 directly follow the
  multiplication by 1/(2π) (and 5 by −1/(2π)): the convention is beyond doubt.
- **Affected effects**: 460 shaders, 71 named effects in `shaders/programmes.txt`. On the holographic
  sphere (`starmap_planet_holo_sphere`), the latitude gradient cos(π·|v − ½|) stayed near 1 instead
  of fading out at the poles. Also affected: all the `holo_advanced*` effects (transmissions included), the
  rings, selections and routes on the map, the impact sphere of the charged shot, the SSAO sample
  spiral, and the rotation in the vertex shader of the holographic buildings in the training.
- **Fix**: `sin(fract(x) * 2π)` and `cos(fract(x) * 2π)`. Full retranslation: only the 460
  files with a sine or cosine change, and only on those lines. The old cache is kept in
  `build/full/shader_cache_avant_sincos`.
- **Retranslating while the user is playing**: `shaderc.py --out build/full/shader_cache_new`, at low
  priority (IDLE class inherited by glslang), then swap the folders once the game is closed.
- **Verified** (`verif_carte.log`, `verif_sincos.log`, test saves): campaign briefing
  correct (holographic globe of Corneria with its halo, planets of the Lylat system). Cutscene and
  training compared with Lichii: 24 pairs kept instead of 17. The training shots
  (Lichii 5521 to 8521) now match at 0.87-0.98, and the **holographic towers** there are
  translucent volumes as in Lichii: that is where limit 30 came from. No device loss,
  no missing shader.

## Launcher (2026-11-09)

`lanceur.exe` (at the project root since 1.0.1, next to `recomp/`, as in the package; sources in
`recomp/launcher/`): a launch window in the spirit of the one from the native port of Star Fox
Command (SerenityG4K), in black and neon green. It builds the game on the player's PC, from **their own** game dump
(`.nup`), sets up `recomp.ini` and starts the game.

- **Technology**: dependency-free Win32, like the reference. Rendering goes through Direct2D 1.1 on a DXGI swap
  chain, text through DirectWrite (Bahnschrift font, shipped with Windows) and video through Media
  Foundation. No DLL to ship; the executable is 2.1 MB. Compilation: `python launcher/construire.py`
  (`--out other.exe` if `lanceur.exe` is open).
- **Screen**: glowing title, big "BUILD THE GAME" button, which becomes "PLAY" when the game is ready
  and shows the progress during the build. Settings, Controls, Build and Quit menu, game dump
  status, scrolling ticker, perspective grid, scanlines, FR / EN choice.
- **Background**: clips from the cutscenes, graded green (Direct2D color matrix), with a slow zoom and
  fades to black between two clips. Without footage, a holographic globe spins instead.
- **Footage**: recorded by the recompiled game itself (`RECOMP_RECORD_DIR`, `runtime/os/record.cpp`)
  during the opening cutscene: 6 MP4 clips of 9 s at 1280×560, without black bars or subtitles, in
  105 s. They come from the player's game dump and stay on their PC, in `launcher/extraits`, never
  distributed.
- **Settings** (the game's `recomp.ini`): game language (15 choices, names in their own language), resolution,
  fps, native rendering or console-like, fullscreen or windowed. The volume (`RECOMP_VOLUME`) is passed at
  launch. The background (footage or plain) and the number of compilation threads are also chosen here. Two
  buttons allow rebuilding the game or re-recording the footage. The launcher's own settings are in
  `lanceur.ini` (UTF-16).
- **Controls**: mapping of Switch buttons → keyboard (key names given by Windows, so AZERTY
  or QWERTY) → Xbox controller. Since 1.0.3 the keyboard keys can be changed on this page (section "Keyboard keys
  editable from the launcher" below).
- **Build**: `launcher/preparer.py`, run at low priority inside a job object (the whole process
  tree is stopped on "Cancel" or on closing). Steps:
  1. check the game dump (version 2.0.0, by the SHA-256 hashes in the `.nup` index, without re-reading 30 GB);
  2. extract the executables;
  3. prepare the module images;
  4. recompile (`fullbuild.py`);
  5. link, game icon included;
  6. translate the shaders (romfs then SPIR-V);
  7. record the footage.

  Each step already done for this game dump is skipped. The launcher reads the `@etape`, `@progres`,
  `@info`, `@ok`, `@saute` and `@erreur` lines, and writes the full log to `build/full/preparation.log`.
- **Game dump outside the project**: `fullbuild.py` reads `RECOMP_NUP` (passed by `preparer.py`) for the manifest;
  `shaderc.py` finds glslang through `RECOMP_GLSLANG`, then the `PATH`, then MSYS2.
- **Headless check**: `LANCEUR_CAPTURE=file.png` (`LANCEUR_CAPTURE_T=s`,
  `LANCEUR_CAPTURE_QUITTER=1`) writes the displayed image. `LANCEUR_PAGE=reglages|controles|construction` opens
  a page. `LANCEUR_ACTION=construire` starts a build with the driver script given by `LANCEUR_PREPARER`
  (never the game: a test that pressed "PLAY" had opened the real game on the player's save).
- **Game log**: before each session, the launcher renames `recomp_run.log` to `recomp_run_precedent.log`.
- **Game icon** (2026-11-10): `recomp_main.exe` carries the official icon, visible in Explorer, the
  taskbar, Task Manager and the window, whose title becomes "Star Fox - N img/s".
  - Source: the metadata of the base content of the game dump, a sequence of `[u32 kind][u32 size]` blocks.
    Block 3 is a 256×256 JPEG of 50 KB; block 1 is the NPDM, blocks 5 and 8 are the names.
  - `preparer.py` (step 5, `extraire_icone`) converts it to `build/full/icone.ico`: 7 sizes as PNG, from 256 down to
    16 px. Decoding and downscaling go through WIC, called via `ctypes`, and the PNG is written with `zlib`.
    No external program is involved: an encoded PowerShell command, started by the launcher, would look
    too much like malware to antivirus software.
  - `link.py` embeds it as resource no. 1 (windres run in `rt/` with relative paths) and
    `present.cpp` gives it to the window class. Without an icon, or without windres, the executable is built all the same.
  - The launcher shows it in its window and in the taskbar (`WM_SETICON`, at startup and after a
    build). The `lanceur.exe` file keeps its green icon: it is distributed, the game icon never
    is.
- **Fresh install verified** (2026-11-10): sources only (2.5 MB) + the game dump, in an empty folder. The seven
  steps pass without error in 51 min at the lowest priority: 3 s to check, extract and prepare, 49 min of
  recompilation, 56 s of linking and 98 s of shaders.
  - The game built this way plays the cutscene and the training (11,400 frames, no skipped draw, no
    missing shader).
  - At startup it dumps 159 shaders that are absent from the romfs (DLSS and related passes, never drawn in normal
    play). `shaderc.py --nouveaux` translates them after the footage step: the 2,388 SPIR-V files are then
    byte-for-byte identical to those of the install made while playing.
- **Package**: `launcher/paquet.py <folder> [--zip]` assembles lanceur.exe, `LISEZMOI.txt`, the sources and
  `tools/`. It refuses any file derived from the game (4.4 MB without tools).
- **Bundled tools** (`launcher/outils.py`, 2026-11-10, with the user's agreement): downloaded from the official
  sources into `recomp/outils/`, SHA-256 hashes verified (published by python.org, PyPI and GitHub):

  | Tool | Version | Role |
  |---|---|---|
  | Embeddable Python | 3.13.17 | driver script, recompiler, extraction (`._pth` set up for `site-packages`) |
  | zstandard, lz4 | 0.25.0, 4.4.5 | reading the `.nup`, the NSOs and `data.zip` |
  | capstone | 5.0.7 | the recompiler's disassembler (`a64/codegen.py`), version used for the validated code |
  | GCC (WinLibs, UCRT, POSIX) | 16.2.0 r3 | generated C++, runtime, launcher |
  | glslang | 16.6.0 | GLSL → SPIR-V (`glslang.exe` only since v14: `RECOMP_GLSLANG`) |

  `outils.py` installs only what is missing (markers in `outils/.installe/`). The archives are deleted after
  installation; `outils/LISEZMOI.txt` gives the sources, hashes and licenses.
- **GCC (WinLibs) pitfalls**:
  - it targets Windows 7 by default: `_WIN32_WINNT=0x0A00` is defined for the launcher (`lanceur.h`) and the runtime
    (`link.py`; the generated code is not touched, its cache stays valid);
  - **installed in a path with spaces, it fails at link time** ("cannot find C:/…/Projet"), even for
    a one-line program; neither the 8.3 short path nor an NTFS junction helps (GCC 16 resolves back to the
    real path). `preparer.py` (`gcc_sans_espaces`) then copies the compiler once (≈ 1 GB, 20 s) into
    `C:\ProgramData\StarFoxRecomp\gcc-<hash>`, without administrator rights;
  - GCC always adds `default-manifest.o`. The MSYS2 one is language-neutral and ld drops it in favor of a
    manifest from the program; the WinLibs one is in English (1033). The launcher therefore had two no. 1 manifests
    (".rsrc merge failure: multiple non-default manifests"). `construire.py` makes GCC find an empty
    `default-manifest.o` first, through `-B`; the game, which has no manifest of its own, keeps GCC's.
- **Package**: `launcher/paquet.py` adds `recomp/outils` to it (without the archives): 1.08 GB uncompressed.
  A package placed in a folder with spaces, built with its own tools only (`PATH` reduced to Windows,
  `RECOMP_SANS_MSYS2=1`), revealed that `capstone` was missing; it has been added since.
- **Package verified** (2026-11-10): same folder with spaces, same tools only, lowest priority.
  - The full rebuild passes in 3,091 s: 2,964 s of recompilation, 51 s of linking and 68 s of
    shaders (2,308 SPIR-V).
  - The game built this way runs for 4 min (11,554 frames at 60 fps) with no missing shader, no skipped draw,
    3.1 GB of video memory and correct frames.
  - The "lier" (link) step, redone with the icon, extracts and embeds it (14 s).
  - The package from this check (0.1) was replaced by release 1.0.0 (see below).
  - The SPIR-V files differ from those made with MSYS2, because glslang 16.6 is not 16.3: 16.6 folds more
    constants, for example `uintBitsToFloat(0x3f000000u)` → `0.5`. 400 of them, picked at random, pass
    `spirv-val` (Vulkan 1.3, scalar block layout like the runtime).
- **A/B of the 16.3 / 16.6 shaders** (2026-11-10): same executable, same cutscene (frames 1,201 to 10,800), four
  alternating runs. Mean interval 17.41 to 17.48 ms in both cases, same number of stutters, GPU time of frame 6600
  7.9 to 8.3 ms on both sides, no errors: the glslang 16.6 SPIR-V is as good as the 16.3 one.

## Version 1.0.0: SpaceAnimalsRecomp (2026-11-10)

Published under the name **SpaceAnimalsRecomp** (the launcher displays "StarFox Remake [recompiled]"):
<https://github.com/SerenityG4K/SpaceAnimalsRecomp>, GPL-3.0 license, release v1.0.0.

### Precompiled shaders (VK_EXT_graphics_pipeline_library)

- **Principle**: each shader is compiled on its own, once, into a pipeline library.
  - Vertex: "pre-rasterization" library, with the viewport, the [-1, 1] depth control and the polygon
    mode.
  - Fragment: dynamic depth and stencil, without multisample state, so that it links with any output.
  - Compute: full pipeline.
  - At draw time, all that remains is to create the vertex input and the output (formats, blending), without any
    shader, then to fast-link the four libraries: 0.01 to 0.65 ms per pipeline (`RECOMP_PIPE_TIMING=1`).
- **Dynamic states** (Vulkan 1.3, common to all graphics pipelines, set only when they change):
  face culling and winding, depth bias, test, write and compare, stencil test and operations.
  - They are no longer part of the pipeline key: 201 pipelines in the cutscene instead of 255.
  - Alpha-to-coverage, forgotten until then, goes into the key.
- **`recomp_main --precompile`**: no window, no game. It compiles the 2,388 shaders of the cache (1,137 vertex,
  1,131 fragment, 120 compute) in parallel and writes `pipelines.cache` (≈ 90 MB, `VkPipelineCache`).
  - Duration: 33.5 s cold on 16 threads (RTX 5070 Ti).
  - `pipelines.ok` keeps the marker: GPU, driver, cache UUID, shader list, size and date of the executable.
    When nothing has changed, the command stops in 0.3 s ("@saute").
  - It is run by the "pipelines" step of `preparer.py` and by the launcher before each session (progress
    "SHADERS xx %").
- **At game startup** (`warm_up`, before the first frame): all the libraries are created from the cache
  in 1.6 s. Without precompilation, it is the full compilation, with "compilation des shaders xx %" (compiling
  shaders) in the title.
  - The cache is rewritten on exit and at most once a minute if it has grown.
  - `RECOMP_NO_GPL=1` goes back to full pipelines, compiled at the first draw.
- **Validation** (Khronos layer from MSYS2, `VK_ADD_LAYER_PATH`): no errors on the libraries. In-game, a single
  kind of error, already there before: VUID-…-Input-08733, an R8G8B8A8_UINT attribute read as a floating-point `vec4`.
- **Measurement**: the "pipeline" counter of `RECOMP_PERF` covers the whole pipeline section of each draw (key,
  lookup, binding, dynamic states); its ≈ 5 s over the cutscene are a fixed cost of about 0.5 ms per frame,
  not compilations. The gain goes to the player whose driver cache is empty (first launch, new driver):
  each new material used to cost ≈ 200 ms of compilation at the moment it appeared.
- **GCC pitfall**: passing `VkGraphicsPipelineCreateInfo` by value crashed. GCC copied it with a `vmovdqa`
  (AVX write aligned on 32 bytes) into a stack that Windows only aligns on 16. `-Wa,-muse-unaligned-vector-move`
  (runtime, `link.py`) makes all these writes unaligned, at the same speed.

### Launcher 1.0

- Title "STARFOX / REMAKE [RECOMPILED]"; version read from `recomp/VERSION`.
- **FULLSCREEN / WINDOWED** choice under the main button, in addition to Settings.
- **PLAY** first starts the shader compilation (`Tache::Shaders`), then the game.
  - The tasks (build, shaders, update) go through `lancer_tache` and follow the same "@…" protocol.
  - `LANCEUR_ACTION=shaders` compiles the shaders without starting the game (check).
- **Updates** (`launcher/maj.py`, only for an installed package: `lanceur.exe` next to `recomp/`).
  - At startup, `--verifier` reads the latest release; a newer version is shown at the top
    ("UPDATE x AVAILABLE"). The check can be turned off in Settings.
  - `--installer` downloads `SpaceAnimalsRecomp-<v>-update.zip`, verifies its SHA-256 (GitHub's `digest` field, otherwise
    `SHA256SUMS.txt`), extracts it into `maj/` and installs, through `outils.py`, the tools the version requires.
  - `--finir` waits for the launcher to close, copies the files and restarts it.
- **Incremental rebuild**: each step of `preparer.py` has a signature = game dump + hash of its code + the steps
  it depends on.
  - After an update, the launcher sees `.preparation/version.ok` ≠ `recomp/VERSION` and offers
    "UPDATE"; only the steps whose code has changed are redone.
  - Unchanged objects are reused through the hash of `build.compile_one`.

### Publishing

- `launcher/depot.py <folder>` assembles the public repository. It takes the root files from `recomp/depot/`
  (English README, GPL-3.0 LICENSE, CHANGELOG, CONTRIBUTING, `.gitignore`), these notes become
  `docs/DEV-NOTES-fr.md`, and it refuses any file derived from the game and any personal trace.
- `launcher/paquet.py <folder>` produces `SpaceAnimalsRecomp-<v>-windows.zip` (91 MB since 1.0.1, slimmed
  tools included), `-update.zip` (1 MB) and `SHA256SUMS.txt`.
- New version:
  1. change `recomp/VERSION`;
  2. compile the launcher (`construire.py`);
  3. run `depot.py` in a clone of the repository, then commit and push;
  4. run `paquet.py`;
  5. `gh release create v<v> … *-windows.zip *-update.zip SHA256SUMS.txt`.

## Version 1.0.1: slimmed package and tool licenses (2026-11-10)

- **Slimming** (`launcher/outils.py`, `GARDER_GCC`): the list of useful GCC files comes from a trace of the real
  build commands, with the bundled GCC alone in the PATH: 26 files of the runtime and of the generated code compiled
  with `-v -MD`, `windres -v` (it calls `gcc -E`, hence `cc1`), the full link with `-v -Wl,--trace`, then
  the closure of the imported DLLs (PE import tables). 594 files are used (136 MB). In addition, the C/C++
  headers and the MinGW-w64, libstdc++ and libgcc libraries are kept in full: 4,093 files, 382 MB, versus 11,874 files and
  968 MB for the complete WinLibs. glslang: `bin/glslang.exe` only. `paquet.py` applies this filter when copying
  `recomp/outils` (which stays complete, with gdb, for development); `outils.py` slims what it installs, except
  with `--complet`.
- **Result**: a 91 MB package instead of 316 MB, 424 MB uncompressed instead of 1,076 MB.
- **Existing installs**: an update does not touch `outils/`; a 1.0.0 install keeps its complete GCC,
  which works. The copy in `C:\ProgramData\StarFoxRecomp\gcc-<hash>` (path with spaces) is named after
  the WinLibs archive: a complete copy already made by another install is reused as is.
- **Licenses**: `recomp/licences/` contains `THIRD_PARTY_NOTICES.txt` (each bundled component, its exact version, its
  license, the address of its source code) and the full texts. Versions read from the binaries: GMP 6.3.0,
  MPFR 4.2.2, MPC 1.4.1, isl 0.28, libiconv 1.19, gettext 1.0, zstd 1.5.7, zlib 1.3.2, binutils 2.47.20260726.
  `paquet.py` puts them at the root of the package (`THIRD_PARTY_NOTICES.txt`, `LICENCES/`), in both archives.
- **Launcher**: `construire.py` writes `lanceur.exe` at the project root, next to `recomp/`, as in the package.
  Its settings (`lanceur.ini`) sit next to it. In this layout, the launcher also checks for releases.

## Version 1.0.2: footage included, shader survey (2026-11-10)

- **Launcher background footage included**: at the user's request ("it's just my gameplay"), the 6 clips
  in `launcher/extraits/` (59 MB, video only) are in the package and in the update. `paquet.py` copies them (the only
  exception to the "nothing from the game in the package" rule); they stay out of the git repository.
- **"extraits" (footage) step**: skipped if clips are already there, unless explicitly requested (`--etapes …,extraits`,
  "RE-RECORD FOOTAGE" button in Settings). "REBUILD" (`--forcer`) no longer re-records them.
- **Shader survey** (`preparer.releve_shaders`, in the "pipelines" step, once per built executable): at startup
  the game creates 159 shaders that are absent from the `data.zip` packages (80 translatable, 72 of them compute, and the
  79 of the console's DLSS network) and writes them to `shaders/` ("shader inconnu"). The game is started windowed,
  without sound, with a separate save, up to its 600th frame; then `shaderc.py --nouveaux` and a second precompilation.
  Up to 1.0.1, recording the footage did this survey along the way (`e_extraits`): the 1.0.1 test without
  footage (`--sans-extraits`) therefore had 2,308 shaders, and the "known issue" note of 1.0.1 was wrong for
  a normal install.
- The launcher displays "The game opens for a few seconds…" during the survey (`cons_info_releve`).

## Fixes for GitHub issues #1 and #4 (2026-11-10, for 1.0.3)

- **#1, `--precompile` crash** (RTX 5070, driver 617.42: 0xC0000005 at around 83%, the driver's compiler crashes
  on one shader). `recomp_main --precompile` now does the work in a child process
  (`RECOMP_PRECOMPILE_ENFANT=1`, `precompile_travail`), which writes `+name` / `-name` around each shader in
  `precompile_en_cours.txt`. If the child crashes, the parent (`precompile_superviseur`) recompiles one by one the shaders
  that were in progress (`RECOMP_PRECOMPILE_SEULS`, `RECOMP_PRECOMPILE_FILS=1`, log `precompile_isolement.log`); the
  culprit is recorded in `shaders_exclus.txt` under the line `pilote <vendor:device> <version>`, then the
  precompilation resumes. For that driver only, an excluded shader is neither precompiled nor drawn (`get_shader` returns
  null: skipped draws instead of a crash). Crash not reproduced shader by shader: resume on a single thread.
  The parent writes `precompile_controle.log`. The launcher (PLAY) and `preparer.py` benefit from it without any change.
  Test: `RECOMP_PRECOMPILE_PLANTE=<name>` simulates the crash; shader isolated and excluded in under a second.
- **#4, install folder moved**: `manifest.txt` and `build/full/<module>/objs.txt` contain absolute
  paths (fullbuild.py), and the steps are skipped after a move (the signature only records the name of the
  game dump). The loader falls back to `<root>\work\` for a missing image, and to `RECOMP_NUP` (passed by the
  launcher, `lanceur.cpp`) or to a `.nup` at the root for the game dump; `link.py` falls back to the folder of
  `objs.txt` for a missing object.
  Test: manifest pointing to `C:\AncienDossierSFR` → images and game dump found, 90 s of play with no missing shader.

## Checks before each version (`verif/verifier.py`, 2026-11-10)

The user's request: "we need rigor, and to make sure nothing breaks with each build on our side but
potentially on other people's machines too". All the tests done by hand for 1.0.1 and 1.0.2 are now in
`verif/verifier.py` (see the documentation at the top of the file):

- `rapide` (≈ 5 min, game not rebuilt): consistent versions (VERSION, lanceur.h, lanceur.rc, resource and date of
  lanceur.exe, CHANGELOG, README); all the .py files; runtime compiled and linked (test executable outside build/full);
  launcher compiled without warnings; repository export; package (licenses, footage, slimmed tools, up-to-date sources,
  update = package without outils/); slimmed GCC (files of `verif/gcc_utilises.txt` present, imported DLLs
  present, Visual C++ runtime required by glslang reported); update from the latest published release with ITS OWN
  maj.py (only GitHub is simulated) and the steps that "UPDATE GAME" will redo; simulated driver crash (issue
  #1); moved install (issue #4).
- `complet` (≈ 50 min): in addition, a fresh install of the package in a folder WITH spaces (compiler copied into
  a test ProgramData) and a PATH without MSYS2, full build by preparer.py; shaders (2,388, GLSL and .meta
  identical to the reference tree); game for 150 s; game without `VK_EXT_graphics_pipeline_library` (`RECOMP_NO_GPL`, the
  case of GPUs that lack it); launcher (screenshot).
- The game tests have their own save and their own log, do not start if the game or the launcher is open, and
  stop if the player starts the game. Files derived from the game are deleted at the end (unless `--garder`).
- Report: `verif/rapports/<date>_<version>_<level>.md` (private, never exported). `verif/trace_gcc.py` regenerates
  `gcc_utilises.txt` after a change of GCC or of options.
- **Release procedure**: change `VERSION`, `lanceur.h` and `lanceur.rc`, the CHANGELOG (with "Fixed #n" for
  issues) and the package name in the README; `launcher/construire.py`; `verif/verifier.py complet` with no failure;
  `depot.py` in the clone, commit, push; `paquet.py`; `gh release create`; real test of the update from
  GitHub; message to the issues session (numbers fixed, version, rebuild or not).
- **First problem found by `complet`** (2026-11-10): the fresh build failed with a `MemoryError` during main.
  Memory sampled every 2 s: each fullbuild Python process holds ≈ 0.85 GB while g++ compiles its batch,
  cc1plus takes up to 1 GB; 15 jobs ≈ 28 GB, whereas this PC (31 GB of RAM, 8 GB of swap, programs open)
  had only 16 GB of free commit. `preparer.fils_selon_memoire` now caps the jobs at
  (min(available RAM, available commit) − 3 GB) / 2 GB, and writes it to preparation.log. fullbuild.py
  is not modified (otherwise everyone would recompile everything on update). Idea for later: read the images through
  memory mapping (shared between processes) rather than through a copy in each process.

## Faster build and low-end configurations: Clang (2026-11-11, for 1.0.3)

Requests from the user: reduce the build time (≈ 50 min for a player on a Ryzen 9 5950X, more than an hour on a
Ryzen 5 7600 with 32 GB, versus 10 to 25 min for the other Switch 2 recomps), then "faut vraiment optimiser la
recompilation pour les plus petites configs, et le temps de construction" (the recompilation really needs to be
optimised for the smallest configs, and so does the build time).

Comparison with the other projects (docs in their repositories): Hookshot and NX2navi (Zelda Ocarina of Time, 17.7 M
instructions, 295,000 functions) and DKB Recomp translate to C and compile with Clang at -O1, with -O2 reserved for hot
code (profile of a play session); their compiler takes 120 to 180 MB per 3 MB file. Star Fox: 51.3 M instructions
in main, three times as much code.

### Results (test copy: package with Clang tools, folder with spaces, without MSYS2; Ryzen 7 9700X)

| Configuration (restricted by a Job object: affinity and memory) | Compilations | Full build | Peak memory (all processes) |
|---|---|---|---|
| g++ -O2, the earlier 1.0.3 (16 threads, 16 GB free memory) | 7 then 3 | 66 min | ≈ 2 GB per task |
| clang -O1, 4 cores / 8 threads, 6 GB reported free | 7 | 25.7 min (recompiler step 1,534 s) | 2.41 GB (largest process 0.71 GB) |
| clang -O1, 6 cores / 12 threads, 8 GB reported free | 11 | ≈ 20 min (recompiler step ≈ 1,140 s) | not recorded |
| clang -O1, 8 cores / 16 threads | 15 | 16.7 min (recompiler step 997 s) | 4.10 GB |
| **clang -O2 (chosen setting)**, 8 cores / 16 threads | 15 | 21.6 min (recompiler step 1,292 s) | 4.11 GB (largest process 0.87 GB) |

The 9700X's cores are fast: a real low-end PC will be slower than these simulations, but in the same proportions.
Time per batch of main on 4 loaded cores (RECOMP_TEMPS): median 10.2 s, 90th percentile 11.7 s, maximum 68 s; the 20
slowest batches (almost all at the end of the module, 4 to 5 MB of C++) account for 7% of the time.

### What changed

- **Clang shipped and used by default**: same WinLibs release as GCC (16.2.0 r3), the variant with LLVM 23.1.3
  (`outils.py`, SHA-256 1f04e482…ba5f); GCC has not changed. `outils.GARDER_GCC` additionally keeps
  `bin/clang++.exe`, the closure of its DLLs (129 files, 206 MB: LLVM is in separate DLLs) and `lib/clang/23/include`;
  the package's gcc folder is ≈ 590 MB extracted, the full package 223 MB (≈ 150 MB before). Licences:
  `licences/LLVM-LICENSE.txt` (Apache 2.0 with LLVM exceptions), `z3-LICENSE.txt` (Z3 5.3.1, loaded by Clang). The
  project's `recomp/outils`: full WinLibs (`outils.py --complet`); `paquet.py` takes only the files that are needed.
- **Compiler choice** (`build.compilateur_genere`): clang++.exe if it sits next to the g++ that was found, otherwise
  g++; `RECOMP_CXX` forces it, `RECOMP_OPT` forces the level. -O2 by default for both (g++ flags unchanged: its already
  compiled objects remain valid). Measured game speed (CPU time per frame, frames 600 to 2400): g++ -O2 14.9 to
  15.6 ms, clang -O2 14.1 to 14.2 ms, clang -O1 16.6 to 16.9 ms: -O1 costs ≈ 17% of CPU in game for 5 min of build
  time saved once; the other recomps make up for it with a profile (-O2 on hot code). The runtime and the link stay
  with g++ (same ABI).
- **`clang_compat.h`** (included with `-include`, clang only): `[[clang::musttail]]` requires the callee to have the
  caller's signature; `rt_tailcall(ctx, cible)` becomes `rt_tailjump(ctx)` with the target in `ctx->pad`. The generated
  text is the same for both compilers.
- **Per-batch measurements** (6 batches of main, idle PC, 6 at a time): g++ -O2 17.6 s (≈ 1.3 GB, 2.5 GB for the
  largest); clang -O2 13.2 s; clang -O1 10.4 s, ≈ 180 MB (330 MB for the largest); clang -O0 2.8 s but objects three
  times larger. Profile of clang -O1 on one batch: parsing 21%, optimiser 42%, code generation 37%; no option gains
  more than 12% (`-mllvm -fast-isel`: −12%, objects +23%): -O1 kept as is.
- **Memory of the Python processes** (fullbuild.py, a64/): 993 MB → 167 MB per process for main.
  - module image mapped read-only (`mmap`, `Module.from_work`) instead of a 252 MB copy per process;
  - per-function containers created only at analysis time (`Function.oublier`, shared empty values): −140 MB;
  - cache of translated instructions flushed at 150,000 entries instead of 1.5 million (480 MB never returned to the
    system).
  The discovery of main goes from 43 s (5 processes, for lack of memory) to 17 s (11 processes).
- **Generation and compilation separated**: a few Python processes generate the batches (≈ 0.3 s each), the main
  process launches the compilers (`Compilations`); the module's last batch, the longest one, goes first.
  `preparer.fils_selon_memoire`: 0.4 GB per compilation with clang, 1.4 GB with g++, 2 GB kept in reserve;
  `fullbuild.processus_python`: 0.3 GB per Python process. `RECOMP_MEM_LIBRE_GO=n` simulates the free memory (tests).
- **Reproducible discovery**: every discovery task starts from the common list of functions (`_apply`), the split is
  fixed (64 tasks) and the results are taken in order. Before, the origin recorded in a function's comment (bl, b, ptr)
  depended on the order in which the results arrived: ≈ 6 batches recompiled for nothing on every rebuild. Now,
  regenerating all the code with no change recompiles no object (48 s with 15 compilations, 84 s with 6;
  `regeneration` check of `verif/verifier.py`).
- **Retry of a failed batch** (`Compilations.reprendre`): a batch whose compilation fails is recompiled on its own at
  the end; only a second failure stops the step. Seen during the 6-core simulation: clang crashed on batch 593 of main
  ("called-value-propagation" pass), which, rerun on its own, compiles in 8 s with 174 MB. Test:
  `RECOMP_ESSAI_ECHEC=<objet>`.
- **Install folder with square brackets** (issue #1, second case, 2026-11-11): `preparer.compter` passed the folder
  to `glob` without escaping it; in "Jeux [Switch 2]", 0 shaders counted: progress bar stuck at 35%, then "trop peu de
  shaders traduits" (too few shaders translated). `glob.escape` everywhere (counters, step signatures, extracts, bug
  report); the verifier now builds in "installation [neuve]" (spaces and square brackets).

### Validation

- Full `bench/run_all.py` with `RECOMP_CXX` = clang (-O1, runtime compiled by g++): results identical to g++
  (fuzz main FP/SIMD 245,624 PASS, integers 240,000, rare families 199,993 + 7 known REFBUG, saturating 384, func main
  4,670 PASS / 42 NOTBUILT / 88 NONCODE, rtld and subsdk3 all PASS).
- Generated code identical before and after the memory changes: no object recompiled apart from the batches whose
  origin comment already varied.
- `verifier.py rapide --seulement versions,python,paquet,outils`: package 223 MB, 737 used files present, imported
  DLLs all present.

### What remains

- **Update through the launcher**: `-update.zip` does not contain the tools, hence no Clang: these players stay on
  g++, regenerate the code (1 to 2 min) and recompile only a few batches. A fresh full package builds with Clang.
- **Lead for going further** (not done): compile at -O0 the code that the game never executes (profile of a play
  session, as Hookshot and NX2navi do), three times faster to compile; but -O0 objects are three times larger and the
  executable would exceed 2 GB: it would have to be split into several DLLs.

## Game memory: on-demand commit and windowed imports (2026-11-11, for 1.0.3)

Request from the user: "il va falloir optimiser très fortement la consommation de RAM et de VRAM" (RAM and VRAM usage
will have to be optimised very heavily). Issues #5 (black screen at the cutscene with 16 GB of RAM: imports refused,
-2) and #6 (AMD very slow, RAM full). On this 32 GB PC, test runs of the game failed as soon as the open programs left
less than ≈ 15 GB of available commit.

Measurements at frame 2400 (opening cutscene, 1440p windowed, `scratchpad/perf_jeu.py`):

| | In RAM | Committed | Game memory imported (locked) | Video |
|---|---|---|---|---|
| Before (1.0.2) | 12.5 GB | 14.9 GB | 9.3 GB (76 whole pools) | 2.1 GB |
| Now | 7.0 GB | 9.7 GB | 0.5 GB (17 windows) | 2.1 GB |

The game also reaches its 600th frame ≈ 5 s earlier (there are no longer 9 GB to lock in one go).

- **Where the memory came from**: the game reserves almost everything at once (MapPhysicalMemory: 10.4 GB in its
  first six seconds, including one 3 GB pool, two of 1 GB, two of 514 MB) and fills only part of it; everything was
  committed at reservation. Then, at the first compute dispatch using global memory, all the NVN pools were imported
  whole into Vulkan (`import_all_pools`): 9.3 GB locked in RAM.
- **On-demand commit** (`memory.cpp`: `mem_commit_lazy`, `mem_fault`, `mem_ensure`): MapPhysicalMemory and
  SetHeapSize now only record the range; its pages are committed in 2 MB blocks on first access (access violation
  caught by the handler in `host_main.cpp`, which retries the instruction), or ahead of time by `mem_ensure` when the
  range is handed to the Windows kernel or to the driver: IPC request buffers (`ipc.cpp`: ReadFile reads the romfs
  directly into the game's memory) and imported windows. `mem_valid` treats a page that has not been touched yet as
  valid. Two threads can fault on the same block at the same instant: the second one finds the block already committed
  and retries (first mistaken for an error: crash at 30 s). 64 KB blocks tried (`RECOMP_LAZY_BLOC_KO`): 0.6 GB less
  but 45 fps during loading; 2 MB kept. `RECOMP_EAGER_COMMIT=1`: old behaviour.
- **Windowed imports** (`vk_render.cpp`: `find_import`, `import_range`): a pool larger than 64 MB is imported in
  32 MB windows, on demand from draws, copies and compute dispatches; smaller ones are imported whole. Budget: 40% of
  the RAM (`RECOMP_IMPORT_MAX_MO`); beyond it, the windows unused for 2 s are released (`evict_import`), except those
  of compute dispatches. A refused import is retried only after one second (before: thousands of attempts per second,
  1 to 3 fps in issue #5). Vertex buffers are looked up with their size (one window must cover them).
- **Global memory of compute dispatches**: the ranges of a compute dispatch's buffers are imported before it, its
  uniform buffers and the start of its storage buffers are scanned (a 64-bit word that is a pool address causes its
  window to be imported; once per second and per buffer), and the shader records in a feedback buffer any address
  missing from its table (`gxl_miss` in `glsl.py`, `glob_retour`): its window is imported for the following compute
  dispatches. The table lists the windows of compute dispatches first. Change to `glsl.py`: the shaders are
  retranslated at the update.
- **Log**: a "frame N, memory: …" line every 600 frames (RAM, committed memory, game memory reserved and committed,
  imports, windows released, refusals). If Windows refuses a page, the game stops with code 4 (the launcher's "Mémoire
  insuffisante" (Not enough memory) message). Warning threshold at startup: 10 GB of available commit.
- **Tests** (test copy, Clang -O2): cutscene up to frame 2400 at 60 fps, 0 draws skipped, 0 shaders missing, correct
  screenshots (`RECOMP_SNAPSHOT_EVERY`); test with `RECOMP_IMPORT_MAX_MO=300` and `RECOMP_GLOB_RETOUR_SEUL=1`
  (neither pre-import nor scan): correct image, 3 windows released, no refusal.
- **Not tested yet**: beyond the opening cutscene (level changes: `pools_released_locked` with windows), the feedback
  buffer in real conditions (no address missed in these tests), an AMD card, a real 16 GB PC. The CPU time per frame
  could not be compared cleanly (PC short of available commit during the tests, and the intro's timeline shifted).
- **What memory remains** at frame 2400: ≈ 5.6 GB of game memory actually touched (including the texture data, also
  kept in video memory), ≈ 2 GB of video memory (counted by Windows in committed memory), ≈ 1.8 GB of baseline
  (driver, 2,388 precompiled shaders). Leads: return to Windows the pages of textures already sent to the GPU;
  video-memory copies of the buffers read by the GPU (what Hookshot and NX2navi do), probably necessary for AMD
  (issue #6).

## Updates without a rebuild: object keys, frozen batches, compiler kept (2026-11-11, for 1.0.3)

Request from the user: fix as early as possible everything that affects the build, so that the first players do not
have to rebuild their game at every update. Rule since 1.0.3: an update recompiles only the batches whose C++ text
really changes, and normally none.

**What invalidates an object.** An object's key (`build.cles_objet`) covers the compiler flags, the text of the batch
and the headers of the generated code (`cpu.h`, `simd.h`, plus `clang_compat.h` with Clang). So:

| Cause | Batches recompiled |
|---|---|
| one byte of `runtime/cpu.h`, `runtime/simd.h` or `runtime/clang_compat.h`, comments included | all |
| flags in `build.py`, `-O` level, change of compiler | all |
| text emitted by `a64/*.py` (translation, comments emitted into the C++) | those whose text changes |
| new line in `hooks.txt` | those that call the hooked function |
| a batch start moved in `lots.txt` | the two neighbouring batches |

Everything else (runtime, launcher, shaders, scripts) does not touch the objects. To replace a function imported by
the game, prefer `override_export` (runtime only) to a line in `hooks.txt`.

**Three defects found while preparing 1.0.3**, which would have made everything recompile on players' PCs:

1. *The install path was part of the key* (`-I…\recomp\runtime` in the flags): a moved or renamed folder recompiled
   everything. `cles_objet` now returns two keys: the current one, where this path is replaced by `<runtime>`, and the
   old one; `objet_a_jour` accepts both and rewrites the old one at the first build.
2. *The split into batches drifted.* Batches were cut as the functions went by (≈ 60,000 instructions each): one
   function discovered more or fewer shifted all the following batches of the module. `recomp/lots.txt` freezes the
   start address of the 902 batches (847 for main, 36 for sdk, 11, 5, 2 and 1 for the subsdks), with the split of
   versions 1.0.0 to 1.0.2; a batch is the set of functions whose address runs from its start to the start of the next
   one (`fullbuild.lots_figes`). `python fullbuild.py sdk subsdk0 subsdk1 subsdk2 subsdk3 main --figer-lots` writes it
   (discovery only, 33 s): never to be redone after a release. A frozen batch that has become empty is simply skipped.
3. *The update re-downloaded the compiler.* The GCC archive changed with Clang (new SHA-256 hash): the `maj.py` of
   1.0.2 would have downloaded it again (424 MB), `build.py` would have switched to Clang and changed all the flags.
   The `gcc` entry of `outils.py` now carries `acceptes` (the archive of versions 1.0.0 to 1.0.2): `outils.py` leaves
   it in place, `maj.outils_manquants` no longer asks for it. `outils.py --niveau` replaces it anyway;
   `preparer.compilateur_a_niveau` calls it when there are no objects to keep (fewer than 100 objects of main), so
   that a full build benefits from Clang. A failure (no network) leaves g++ in place.

On top of that comes the discovery of functions made reproducible (Clang section above): before, the origin comment
of a few functions changed from one build to the next.

**Verified.** By computation: an object compiled by 1.0.2 has a key that 1.0.3 accepts (same g++ flags, same
34,597 bytes of headers); the text generated by the 1.0.2 sources and by the current sources has the same 902
batches, identical except for the origin comment in 5 batches (this is the reproducibility defect of 1.0.2: a player
will therefore recompile about 5 batches, once). For real: the `maj.py` of 1.0.2 runs the new `outils.py`, which
keeps the compiler without downloading anything; on the test install, the regeneration with `lots.txt` and the new
keys keeps the 908 objects (0 recompiled, 91 s with 6 tasks).

**What the rebuild ("RECONSTRUIRE MAINTENANT" / REBUILD NOW, offered, never forced) does for a player on 1.0.2**: regeneration of the text and comparison
of the keys (1 to 5 min), about 5 batches recompiled by g++, runtime recompiled and link, 2,388 shaders retranslated
(`shaders/glsl.py` has changed). The "recompiler" (recompile) step is rerun as soon as one of its files changes
(`DEPENDANCES` in `preparer.py`, including `lots.txt`), but rerunning is not recompiling.

**Verifier.** The `maj` check now starts from the tools of the previous version (markers written by ITS OWN
`outils.py`, plus the markers of the current tree), lets `outils.py` run for real with `RECOMP_OUTILS_SANS_RESEAU=1`
(any download is an error) and compares `outils/` before and after. `regeneration` still checks "0 objects
recompiled". The test builds have their own `APPDATA`.

## Saves outside the build folder, backups (2026-11-11, for 1.0.3)

Request from the user: check that the saves are intact at every rebuild.

- **Location.** Up to 1.0.2: `recomp\build\full\save`, in the middle of the build files. Since 1.0.3:
  `%APPDATA%\SpaceAnimalsRecomp\save` (`svc_fs.cpp`, `save_root`). At the first launch, the files of the old folder are
  copied there (never moved, never overwritten if the new folder already contains some) and a note
  `SAVE MOVED - SAUVEGARDE DEPLACEE.txt` is left in the old one. If `%APPDATA%` is unusable, the old folder is still
  used. `RECOMP_SAVE_DIR` keeps priority (tests). `recomp_main --save-dir` prints the folder without launching the
  game; the opening of a save file is recorded in the log.
- **Backups** (`launcher/sauvegardes.py`): before every build (`preparer.py`) and every update (`maj.py`, starting
  with the version that follows 1.0.2), both locations are copied to
  `%APPDATA%\SpaceAnimalsRecomp\save_backups\<date>_<heure>_<raison>[_ancien]`, with their hash; a backup identical to
  the previous one is not made again, the last 10 are kept. Nothing is written in the save folders themselves, and a
  failed copy blocks nothing.
- **Verifier**: `sauvegardes` (carry-over to `%APPDATA%` identical, old folder intact, no overwriting,
  `RECOMP_SAVE_DIR` honoured, backups identical, without duplicates, bounded), always with test folders; `maj` places
  a test save at the old location and checks that it survives the update.

## PC memory exhausted: measurements, clean stop, warnings (2026-11-11, for 1.0.3)

Two of the user's play sessions stopped with code `0xC0000409`, with no line in the game's log. The Windows event log
(System, "Application Popup") gives the cause at the very same seconds: "dwm.exe : le processus est terminé, car il
est impossible d'allouer davantage de mémoire" (dwm.exe: the process was terminated because no more memory could be
allocated). It was the PC's commit (RAM + page file) that was full, not a rendering defect.

- **Measurements on this PC** (31 GB of RAM, fixed 8 GB page file, hence a 39.1 GB commit limit): 22 GB already
  committed without the game (340 processes, of which 15.6 GB private). The game commits 7.9 GB at frame 600, 12.06 GB
  after 3 min 30 (frame 10,800: 5.9 GB of game memory on first access, 3.4 GB of video memory, 1.5 GB of unreferenced
  images, the rest for the driver and the shaders). So the 10 GB figure announced until
  then was too low; the one to give players will be set at release, as a measured minimum (the next section shows
  spikes at 13 or 14 GB).
- **Who took the rest**: the first time, the build of the user's code graph (Graphify, 6 to 14 min, peak measured at
  7.8 GB: 98% of the graph is the compiler headers in `recomp/outils/gcc`); the second time, a comparison of generated
  text started 12 s earlier. Rule for tests: no job of more than 1 GB without a guard (refusal below 14 GB free, stop
  below 4 GB or if the game is launched), like `perf_jeu.py`.
- **Design flaw revealed**: with on-demand commit, a PC short of memory no longer refuses the game at startup, it
  refuses it a page in the middle of a play session. Four protections (`memory.cpp`, `engager`):
  1. at startup, the game records the available commit and warns below 13 GB; the launcher refuses the first click
     on JOUER (PLAY) below 13 GB (`memoire_faible`), a second click within 30 s launches anyway;
  2. a 256 MB margin is committed in advance (`mem_marge_init`; it takes no RAM as long as it is not touched) and
     returned to Windows at the first refusal;
  3. at the first refusal, the renderer is notified (`mem_pression`): at the next frame it destroys the unreferenced
     images and releases the import windows that are not used by the current frame; the faulting thread retries for
     5 s, the time for this memory to come back or for Windows to grow the page file;
  4. only after that does the game stop: one line in the log and code 4 (also for a refused runtime allocation,
     `std::set_new_handler`, and for `std::terminate` or `abort` when the available commit is exhausted); the launcher
     then shows "mémoire insuffisante" (not enough memory) instead of "le jeu s'est arrêté" (the game stopped).
- **`memory_saver` switch** (`recomp.ini`, `[memory]` section; `RECOMP_MEMORY_SAVER=0`, and the old
  `RECOMP_EAGER_COMMIT=1`): `false` brings back the behaviour of versions 1.0.0 to 1.0.2, that is, everything
  committed at reservation (`mem_commit_lazy`), pools imported whole, with no budget and no eviction, and all imported
  at the first compute dispatch using global memory (`import_all_pools`). It is the fallback for a player for whom the
  new memory management would cause a problem; the launcher keeps this line when it rewrites `recomp.ini`. The shaders
  are the same in both modes.
- **Leads for reducing further**: cap on unreferenced images (`UNREF_CAP`, 1.5 GB), texture pages returned to the
  system once they have been sent to the GPU, shaders created on demand rather than at startup.

## PlayStation and Switch Pro controllers, rumble (2026-11-11, for 1.0.3)

- **SDL3** (`input.cpp`): `SDL3.dll` is loaded dynamically (next to the executable, then `<root>\outils\sdl3`, then
  `<root>\recomp\outils\sdl3`) and recognises Xbox, PlayStation 3, 4 and 5, Switch Pro and Joy-Con controllers. Every
  connected controller drives player 1. Buttons are mapped by position (the bottom button of the controller is the
  console's B, the right one A, the left one Y, the top one X), as with XInput. Without `SDL3.dll`, or with
  `RECOMP_NO_SDL=1`, the first XInput controller is used, as before. The log records which library is used and each
  controller that is connected or disconnected.
- **Rumble**: the game calls `nn::hid::SendVibrationValue`, imported by main. It is replaced with `override_export`
  (runtime only, no object recompiled): byte 2 of the handle gives the side, the value carries two amplitudes and two
  frequencies (low band, high band). `host_set_vibration` keeps the last value of each side; `appliquer_vibrations`
  sends the low amplitude to the heavy motor and the high amplitude to the light motor (`SDL_RumbleGamepad`, resent
  every 150 ms with a 250 ms duration so that a rumble stops by itself if the game freezes; `XInputSetState` without
  SDL). Nothing rumbles while the window does not have the focus. The `hid` service answers commands 200
  (`GetVibrationDeviceInfo`) and 205 (`IsVibrationPermitted`).
- **Setting**: `vibration` in `recomp.ini` (0 to 100 %, section `[input]`), the "Vibrations" (Rumble) row of the
  launcher, `RECOMP_VIBRATION`. `RECOMP_INPUT_LOG=1` logs the first 20 values sent by the game.
- **HD Rumble**: the frequencies requested by the game are read but not rendered; SDL and XInput only drive two
  motors by their amplitude. Rendering them would need the haptic effects specific to each controller (DualSense,
  Joy-Con).
- **Not yet verified with a real PlayStation or Switch Pro controller.** The "CONTRÔLES" (CONTROLS) page of the
  launcher shows the Xbox and PlayStation mappings.

## Detailed logs, bug report, logs in English (2026-11-11, for 1.0.3)

- **Log header**: the first line of `recomp_run.log` (`journal_systeme`) and of `preparation.log`
  (`rapport.ligne_systeme`) gives the version, Windows, the processor (threads, AVX2), free RAM and available commit.
  The game then logs its settings and where each comes from, the graphics card, the display and its present modes,
  and every 600 frames one statistics line and one memory line.
- **`lanceur.log`** (root folder, 1 MB at most): launcher start, builds, games started with their settings, the
  game's exit code, reports created.
- **Bug report** (`launcher/rapport.py`, "RAPPORT DE BUG" (BUG REPORT) button): `SpaceAnimalsRecomp-bug-report.zip`
  in the root folder, with `report.txt` (system, graphics cards, state of the install, last errors), the logs and the
  settings. The install path becomes `<install>`; the user's folder, their name and the PC's name become `<user>`
  and `<pc>`. The player has a single file to attach to their issue.
- **Everything is in English** since 1.0.3: messages of the game, of the launcher (`lanceur.log`) and of the build
  scripts, and the errors the launcher shows during a build. The launcher's interface keeps its language (French or
  English). Code comments and development tools (`verif/`) remain in French.
- **Texts read back by scripts**, to change together: `frame N:`, `N skipped,`, `N missing shaders`,
  `unknown shader`, `fps` (`verifier.bilan_jeu`, `preparer.releve_shaders`); `batches compiled`, `round N`,
  `N functions`, `done` (`preparer.e_recompiler`); `shader survey: N created`; `copy found`, `image found`
  (`deplace` check); `[vk    ] GPU:`, `display `, `pipelines:` (`rapport.py`); `@info N shaders compiled in`
  (`precompile` check). The tokens `@etape`, `@ok`, `@saute`, `@progres`, `@info`, `@erreur`, `@fini` and the step
  names are a protocol between the scripts and the launcher: they do not change.
- **How the translation was done**: only strings changed. A check compares each file before and after: same number
  of lines, identical code outside strings, same `printf` formats in the same order, same escape sequences; for
  Python, the same syntax tree once strings are emptied. Neither `cpu.h`, nor `simd.h`, nor the text emitted by
  `a64/codegen.py` was touched (they would make the game recompile).

## Release procedure: additions for 1.0.3

To do on top of the procedure in the section "Checks before each release":

1. **"Does this release require recompiling?"** Compare with the last release: `runtime/cpu.h`, `runtime/simd.h`,
   `runtime/clang_compat.h`, `lots.txt`, `hooks.txt`, `a64/*.py`, the flags in `build.py`. The expected answer is no;
   otherwise write it at the top of the CHANGELOG and tell the issues session.
2. **Saves**: `verifier.py` passes `sauvegardes` and `maj` (test save intact after the update).
3. **English notes**: carry over to `recomp/DEV-NOTES.md` the sections of `recomp/README.md` that changed;
   `LISEZMOI.txt` and `README.txt` (`paquet.py`) change together.
4. **Logs**: any new message is written in English; if a text read back by a script changes, change the script.
5. **Memory**: a test of at least 10 min with `memory_saver = true`, then the start of the game with `false`.

## In-game memory spikes: the driver's compiler, and the tools to see it (2026-11-11, for 1.0.3)

Follow-up to the section "PC out of memory". Other programs did not explain everything: on this PC (15.6 GB of
available commit at launch), a test with nothing else in the background still stopped at about 2 minutes, the game
going from 11 to 13 or 14 GB committed in two seconds.

**What is established.** During these spikes, the process default heap goes from 0.6 to 2.6 GB in ≈ 2 s, in ≈ 20,000
blocks of 58 to 142 KB, then everything is freed. The allocating thread is a thread created by the NVIDIA driver
(`nvoglv64.dll`), 28 frames deep in `nvgpucomp64.dll`, its shader compiler: a few seconds after a pipeline has been
created by fast linking (0.06 ms on our side), the driver recompiles it in the background, and that recompilation
takes ≈ 2 GB for large shaders. The spike comes back at the same moments of the opening cinematic (around 115 s,
130 s, 160 s depending on the run), in 18 runs out of 19, including from one launch to the next. It already existed
in 1.0.2 and does not depend on `memory_saver`.

**What was ruled out, by test and not by reasoning** (each lead first seemed to explain the spike):

| Lead | Test | Result |
|---|---|---|
| saving `pipelines.cache` every minute | timings cross-checked, then save disabled | spike unchanged; the save costs ≈ 0.4 GB |
| pipeline libraries (fast linking) | `RECOMP_NO_GPL=1` | spike unchanged (compilation at the first draw) |
| our Vulkan allocations (render targets, textures, buffers) | `vkAllocateMemory` tracking | unchanged during the spike |
| our C++ allocations | `operator new` counted | 190 MB live during the spike |
| fiber and thread stacks | executable header, `CreateFiberEx` | 64 KB committed per fiber |
| large descriptor set rebound at every draw | bound once per recording | spike unchanged (removed) |
| texture-set writes one at a time | `RECOMP_TEX_WRITE_BATCH=1`: 7,109 writes in 148 calls | spike unchanged (not enabled) |
| optimisation of linked pipelines | `RECOMP_PIPE_NO_OPT=1` (`DISABLE_OPTIMIZATION`) | spike unchanged |

Lesson: I had announced the pipeline cache as the cause on a coincidence of timings. Since then, a cause is only
written here after a test that makes it disappear or a call stack that shows it.

**Tools added** (`RECOMP_MEM_MAP=1`, everything goes to the game log, every 600 frames and as soon as committed
memory has grown by 512 MB):

- committed-memory map (`mem_map_log`, `VirtualQuery`): private total by reservation size, largest reservations,
  reservations of 1 to 64 MB by size (the segments of a Windows heap are 16 MB);
- process heaps (`HeapSummary`): which one grows; the runtime's share (`operator new` counted in `host_main.cpp`,
  allocations by size class since the previous map);
- Vulkan allocations by kind (GPU memory, host-visible, imported) and by size;
- when the default heap has taken 512 MB since the previous map: busy blocks by size class with the start of their
  content (`tas_histogramme`), and call stacks of every thread that went through a heap function
  (`dump_thread_stacks`: the heap lock is held for 150 ms so that allocating threads stop on it).

**What was fixed along the way** (measured, independent of the driver's spike):

1. *Texture staging buffers*: one per uploaded texture, of its size, only freed 6 submissions later and unbounded
   within a recording. Now freed as soon as the GPU has finished (`free_done_staging`), 256 MB at most per
   recording (`borner_transit`).
2. *Cache of orphaned images* (`UNREF_CAP`, 1.5 GB): it is useful (1.3 GB of textures picked up again without a new
   upload in 100 s; at 256 MB, 14 % more uploads but 1 GB less video memory and committed memory). Its cap follows
   the PC's memory, read every second: 1.5 GB, 256 MB under 5 GB of available commit or when video memory exceeds
   85 % of its budget, emptied under 3 GB (import windows unused for 2 s are then released).
3. *Pipeline cache save*: every 10 minutes instead of every minute, and only with 8 GB available; still done when
   the game is closed.
4. Available commit is counted in binary GB everywhere.

`memory_saver = false` keeps the old behaviour for points 1 and 2.

**Measurements** (opening cinematic then training, A pressed every 2 s): outside spikes, 8.8 GB committed at 30 s
then 10.2 to 11.6 GB; a 5-minute test ran to the end (16,800 frames, 0 skipped draws, 60 fps median) with two dips
to 2.1 GB available, thanks to point 2. Breakdown at frame 6600: 5.6 GB of game memory, ≈ 2.3 GB of video memory (it
counts in committed memory), ≈ 1.5 GB of large runtime blocks, 0.26 GB of reserve, the rest for the driver.

**Video memory at level changes: no leak.** On the 5-minute test (cache capped at 256 MB most of the time):
2,150 MB at frame 2400, maximum 2,396 at frame 3600, 2,367 at frame 10800, then 1,820 MB after the training level
loaded (970 images freed in all), 2,029 at the end. The 3.3 GB seen before these fixes included 1.1 to 1.3 GB of
orphaned images.

**Textures kept twice.** On the console, a texture's data is used directly by the GPU; here it stays in game memory
after being uploaded to video memory. Measured (line "texture data kept in game memory" of the map): 1,337 MB for
377 textures at frame 5400, entirely in whole pages. Giving these pages back to Windows would therefore save
≈ 1.3 GB, but a released page comes back empty: we first need to know whether the game or the runtime reads them
again (the runtime re-hashes them when a texture object reuses the same address). Not in 1.0.3; to be done with
read monitoring (protected pages, access counting) on a long test.

**Does the driver keep its work from one launch to the next?** 100-second tests closed normally (`ESSAI_FERMER=1` in
`essai_long.py`: `WM_CLOSE`, exit code 0, `pipelines.cache` written on exit), heap sampled every 30 frames
(`RECOMP_MEM_MAP_FIN=120`). Fast linking, two launches in a row: same heap curve within 10 % (≈ 300 MB at 30 s,
490 at 70 s, 650 to 870 at 100 s) and the driver's compiler caught at work in both. Full pipelines
(`RECOMP_NO_GPL=1`), two launches in a row: same curve, `pipelines.cache` grown from 164 to 170 MB after the first,
and the compiler still caught once in the second. The large spike (after 113 s) could not be compared: it takes the
PC's available memory below the tests' guard.

**Not solved.** Preventing or moving the driver's recompilation. Leads: go through the large spike twice in a row
with a normal exit, on a PC that has the memory for it; try a full pipeline taken from the cache without
compilation (`FAIL_ON_PIPELINE_COMPILE_REQUIRED`) before fast linking; create, during the launcher's
precompilation, the pipelines already seen in previous games. This spike is also a lead for the stutter of issue
#7, the frame drops of issue #10 and the precompilation crash of issue #1. On this PC, tests can no longer go
through the spike safely while 24 GB are taken by other programs.

**Player-facing texts.** No memory requirement figure any more (launcher, `LISEZMOI.txt`, `README.txt`,
`recomp.ini`): it will be given at release as a measured minimum. The launcher's warning advises closing heavy
programs and letting Windows manage the page file; it was cut on screen after "Fermez d'a…" (one 400 px line) and
now fits on two lines (`texte_lignes`, checked by a capture with `LANCEUR_STATUT=memoire_faible`).

## GitHub issues #11 and #12: precompilation on AMD, black launcher window (2026-11-11, for 1.0.3)

**#12, black launcher window ("black box"), no hardware or version given.** One cause is established by test;
and the player confirmed it on 2026-11-11: with their screen set below 270 Hz, the 1.0.2 launcher displays ("This was
the problem. Lowered my monitor refresh and it fixed the problem.").

The launcher presents one frame every n screen refreshes, n = refresh rate / 60 rounded, to run at about 60 frames
per second. `IDXGISwapChain::Present` only accepts n = 1 to 4. On a screen of 270 Hz and more, n was 5 or more:
`Present` returned `DXGI_ERROR_INVALID_CALL` (0x887A0001) at every frame, the launcher drew into its buffer but
nothing reached the screen, and the loop spun freely (≈ 1,200 frames per second, one core busy). The computation is
the same in the published versions (1.0.0 to 1.0.2, read in the public repository). Test with a test launcher (code
from before the fix, simulated refresh rate, picture of the screen where the window is):

| Refresh rate | n | `Present` | Screen (pixel average / 255) |
|---|---|---|---|
| 165 Hz (this screen) | 3 | succeeded | 16.4 |
| 240 Hz simulated | 4 | succeeded | 16.2 |
| 300 Hz simulated | 5 | 0x887A0001 at every frame | 0.1 (black) |
| 360 Hz simulated | 6 | 0x887A0001 at every frame | 0.1 (black) |

Fix (`dessin.cpp`): n capped at 4 (90 frames per second at 360 Hz, 120 at 480 Hz; animations follow the clock, not
the frame count); if `Present` fails anyway, n goes back to 1 and the loop waits 15 ms. After the fix, 300, 360 and
500 Hz simulated: picture on screen (average 17), no failure.

Every display failure now leaves a line in `lanceur.log` (`jlog`, exported by `lanceur.h`), because this issue came
with no trace at all:

- at startup, `display: <adapter>, Direct3D 11.1, screen 165 Hz, one frame every 3 refresh(es)` (with `(software)`
  when no hardware Direct3D 11 device could be created);
- the failing step and its code when the display cannot be created (`display: … failed (0x…)`); the on-screen
  message is now bilingual (`affichage_indisponible`) and points to `lanceur.log`;
- `display: drawing failed (EndDraw 0x…)`, `display: Present failed (0x…)`, `display: ResizeBuffers failed`,
  `display: swap chain target not recreated` (this last case also leaves a black window).

Along the way: the title glow was drawn into its own bitmap in the middle of the frame in progress (nested
`BeginDraw`), which made the first frame fail (`EndDraw` 0x88990001, nothing visible). It is drawn before
`BeginDraw`.

Verification: `LANCEUR_HZ=<rate>` simulates the screen; the verifier's `launcher` check starts the test launcher at
360 Hz and rejects any display failure in its log (the `LANCEUR_CAPTURE` capture alone is not enough: it reads the
buffer, which is correct even when nothing reaches the screen). The launcher sources are not part of what the game
build reads (`DEPENDANCES` in `preparer.py`): this fix makes nothing rebuild.

For another black window, on a screen below 270 Hz, the 1.0.3 `lanceur.log` will give the cause.

**#11, crash during shader precompilation on AMD** (Radeon RX 6700 XT, Ryzen 5 7500F, 1.0.2). According to the
player's log as read by the issues session: recompilation, linking and shader translation succeeded, then
`pipelines` stops at `@progres 0.996`, with no `@erreur`. What the code says about that log: only the main thread
writes the progress, and `@progres 1.000` is written after all threads have finished and before `pipelines.cache`
is written. A log stopping below 1.000 therefore means the process died while the last shaders were being compiled
(about ten were left), not while writing the cache.

This is the case handled by the 1.0.3 supervisor (section "Fixes for GitHub issues #1 and #4"): the shaders in
progress at the time of the crash are recompiled one by one, the faulty one is excluded for this driver, otherwise
compilation resumes on a single thread, otherwise an error is reported with the crash code instead of a silent log.
Test of 2026-11-11 with the executable of the test installation (`scratchpad/lanceur_sync/essai_precompile.py`):
simulated crash on the last of the 2,388 shaders, so at the end of precompilation as for this player. Nine shaders
were in progress; the faulty one is isolated and excluded in under a second, precompilation finishes with 2,387
shaders, code 0; exclusion list then deleted, 2,388 shaders. **Limit: never tried on an AMD card, and the real crash
is not reproduced here; it is up to the player to confirm with 1.0.3.** Their first message (`@progres 0.951`,
third-party launcher) was very probably the same step, and not the recompilation as I first read it.

## Game memory: textures removed from RAM, shaders on demand, memory watch (2026-11-11, for 1.0.3)

Requests from the user on 2026-11-11: reduce memory use ("I have almost no VRAM use but a lot of RAM use"), then "we
want to avoid crashes as much as possible". Follow-up to the sections "Game memory" and "In-game memory spikes".

**Measurements** (opening cinematic, frame 5400, 96 s; `scratchpad/proto_tex/cmp_essais.py`):

| Run | Game RAM | Committed memory | Game space (0x7000000000) | Block 0x41dbff0000 |
|---|---|---|---|---|
| without the two savings | 7.2 to 8.0 GB | 10.3 to 10.9 GB | 6,185 MB | 921 MB |
| textures released | 5.8 GB | 9.1 GB | 4,322 MB | 922 MB |
| shaders on demand | 6.7 GB | 9.8 GB | 6,185 MB | 299 MB |
| both (default settings) | 4.8 to 5.7 GB | 8.5 to 9.3 GB | 4,322 MB | 272 to 301 MB |

A 36-minute game played by the user across several levels, with both: 4.0 to 5.4 GB of RAM, 7.7 to 9.5 GB committed
(11.4 at worst, a driver spike), 4.0 GB of available commit at the lowest on his PC, 0 skipped draws, 0 memory
refusals, 0.6 % of frames lost. His morning game without them: 12.06 GB committed and 8.0 GB of RAM in 3 min 30.

**1. Texture data given back to Windows** (`texture_release`, `memory.cpp`: `mem_release`; `vk_render.cpp`:
`tex_release`, `source_changed`). On the console a texture's data is used directly by the GPU; here it is copied to
video memory and the original stayed in game memory. After the upload, the whole pages of the source are written to a
temporary file (`%TEMP%\recomp_textures_<pid>.tmp`, `FILE_FLAG_DELETE_ON_CLOSE`: it disappears even if the game is
killed; leftovers of a power cut are deleted at the next start), then decommitted. After that:

- *read* (by the game: 863 times in 36 minutes, 54 MB; by the runtime before a re-upload or a hash): `mem_fault` reads
  the pages back from the file and commits them again before resuming the instruction, page by page;
- *write*, whether the texture still has a game object or not: same restore, then the write (`Rendu::vivant`,
  maintained by `ref_image` and `texture_released`, now only feeds the log counters);
- *before a system call or a Vulkan import* (`mem_ensure`): the range is restored; pages under an import are never
  released (the driver locks them);
- *new texture object on an existing image*: if the released pages were never touched (`mem_released_intact`, release
  number), the content is unchanged by construction; only the bytes outside whole pages are compared (`edge_hash`),
  nothing is read back (12,646 times in 36 minutes).

The file takes what the temp folder's disk has free beyond 4 GB, 6 GB at most; under 5 GB free at start, or under
2 GB during play, nothing more is released (`mem_release_actif`). Measured: 2.2 GB of file after 5 minutes, 3.8 GB
after 36 minutes; 0.9 s of writing in 5 minutes (≈ 0.3 ms per MB, on the render thread).

*Flaw fixed the same day.* The first version restored 64 KB of old content before any write (piece committed, then
copied). During his 36-minute game, the game rewrote 13.9 GB of released data, from four threads at once (225,072
restores): another thread could write into the piece between its commit and the copy, and the copy overwrote its new
data with the old. The user reported rare black artifacts in smoke and explosion effects; nothing establishes that
this was the cause. Since then: no copy at all into memory the game is rewriting (ranges dropped: 98 to 100 in 5
minutes, 580 MB, 44 to 66 ms of restoring instead of 268), and page-by-page restore for the rest. Frames lost over 5
minutes: 2.6 % after, 2.4 % before.

*Second flaw, fixed the same day (5 pm): the dropped ranges.* Between 2 pm and 5 pm, a write into the data of a
texture with no object dropped the whole range, on the assumption that the game was reusing that memory. During his
39-minute game (`partie_utilisateur_2`), the user saw large opaque black rectangles in place of the smoke of a big
explosion, and black debris (captures of the game window: `captures_partie.py`). What the log and the tests establish:

- a texture with no object is not free memory: the game destroys and recreates its texture objects on data left in
  place (17,671 recreations without reading in 39 minutes);
- the game moves texture data inside its memory: one function reads the old place (`main+0xbb74b10`), another writes
  the new one (`main+0xbb74e60`, which writes the last 8 bytes first). With the drop, the first write emptied the
  range and the rest of the copy read zeros;
- measured with the drop removed (`jeu_auto_1`, 7 minutes of the Corneria mission played unattended): 579 reads
  (36.2 MB) of data of which another part of the same object-less range had just been rewritten. These are exactly the
  bytes the drop turned into zeros (counter `read back after a partial rewrite`).

Fix: no drop any more. Every write first restores the content, page by page, in 64 KB pieces. Cost measured over
those 7 minutes: 61,090 restores on write (3.8 GB), 4.3 s in total, 60 fps median, 0 skipped draws. `RECOMP_TEX_DROP=1`
brings the drop back (comparison). **Not established**: that the black rectangles the user saw were these textures.
The same mission replayed with the drop (`jeu_auto_ancien`) shows no black rectangle on Corneria; the defect was seen
in a later level. To be confirmed by a game played by the user. His first game of the day (the version before 2 pm)
already showed rare black artifacts: the window between the commit of the 64 KB piece and its copy let another thread
read zeros there; restoring page by page reduced it to under a microsecond.

*Limits.* Between the commit of a page and its copy (under a microsecond), another thread that accesses it without
faulting reads zeros or sees its write overwritten; closing that needs file views (address space made of
placeholders). On a PC where
Windows manages the page file, this cold data already went to the page file: the real gain there is smaller than on a
PC with a fixed page file.

**2. Shaders on demand** (`shaders_on_demand`, `ensure_init`). At start the game created the libraries of all 2,388
shaders; it uses 127 of them in 96 s, 224 in 5 minutes, 560 pipelines in 36 minutes. When `pipelines.cache` is complete
for this GPU, this driver, these shaders and this executable (`pipelines_a_jour`: the signature in `pipelines.ok`,
written by `--precompile`), nothing is created at start: each library is created at its first draw, from the cache.
Gain: 0.75 GB at the first frame (3.64 → 2.89 GB committed), 0.62 GB at 96 s; it shrinks as shaders get used. Without
a complete cache (game started without the launcher, driver changed), everything is created at start as before. A
shader that takes more than 50 ms to prepare at first use is written to the log (`lent_au_premier_usage`).

**3. PC memory watch** (`mem_veille_init`). The driver's compiler takes 2 to 3 GB in two seconds inside our process;
if Windows refuses that memory, it crashes without a message and the game with it (the two stops of the morning). The
protections in `engager` only served at the first refusal made to the game itself, and the memory state was only read
by the renderer once a second. A thread now reads the available commit every 50 ms: under 2 GB, it releases the
256 MB reserve and warns the renderer, which destroys orphaned images at the next frame (up to 1.5 GB) and releases
unused import windows. `RECOMP_VEILLE_GO=n`: other threshold (tests). Not tried in real conditions: no run has gone
under 2 GB since.

**Settings** (`recomp.ini`, `[memory]` section): `texture_release` and `shaders_on_demand`, `true` by default, no
effect if `memory_saver = false` (everything then goes back to 1.0.2 behaviour). `RECOMP_TEX_RELEASE`,
`RECOMP_SHADERS_ON_DEMAND` for tests. A `recomp.ini` from a previous version gets the settings added since appended at
its end, with their explanation; its lines are not touched (`load_file`).

**Tests** (test copy, `scratchpad/longs/`): `int_sans` and `int3_defaut` (100 s, switches off then default: same
pictures, −1.7 GB committed); `int4_5min` (5 minutes, normal exit, 0 skipped draws); `partie_utilisateur_1` (36 minutes
played by the user, before the flaw was fixed); precompilation with a simulated crash unchanged;
`partie_utilisateur_2` (39 minutes, with the drop: black rectangles); `restaure_1` (200 s, drop removed); `jeu_auto_1`
and `jeu_auto_ancien` (mission played unattended, without then with the drop). Log: lines `texture data released: …
MB now`, `restored: … write faults on unused textures`, `read back after a partial rewrite`, `shader libraries created
at first use so far`.

*Playing a mission without a player* (found on 2026-11-11): `essai_long.py` with `ESSAI_SAUVEGARDE=<folder>` pointing
to a COPY of a save whose campaign is finished. The automatic A press goes through the results, starts a new
single-player campaign and fires during the Corneria mission, which is on rails: it reaches the boss and the mission
report in 6 minutes. It is the only automatic test that covers real gameplay (texture streaming, explosions); the
opening cinematic does not trigger the texture moves (counter at 0 in 200 s).

**Lead noted for a later version** (the user's request: a garbage collector): hook the engine into the game's
allocation functions to know which blocks are free in its reserves after a level is unloaded, and give their pages
back to Windows. Gain unknown: first measure the free share of the 5.6 to 7.3 GB of game memory.

## AMD cards: shader shape, protected precompilation, tests on the integrated card (2026-11-11, for 1.0.3)

Request from the user: "have you been able to improve support for AMD cards?". Until now nothing could be tried on
AMD. This PC nevertheless has an AMD card that Vulkan sees: the integrated card of the Ryzen 7 9700X ("AMD Radeon(TM)
Graphics", AMD driver 26.3.1). `RECOMP_GPU=<part of the name>` selects it instead of the discrete card (`init`,
`GPU available:` lines in the log); `scratchpad/integration/essai_amd.py` precompiles the shaders on it while putting
aside the usual card's caches.

**What the AMD card showed with the 1.0.2 shaders** (native 1080p): title screen at 30 frames per second with a large
gray area instead of the logo; 3D scenes at 0.1 frame per second, wrong picture (white areas, checkerboard). This is
the profile of issues #6 (RX 7800 XT: 9.5 to 12 then 1.5 fps) and #7 (RX 7900 XTX: 18 to 23 then 5.6 fps).

**Cause established by test: the shape of the translated shaders.** `glsl.py` wrote every shader as a dispatch loop,
`while (true) { switch (pc) { case N: … pc = M; continue; … } }`. NVIDIA's compiler recovers the real control flow
behind the switch; AMD's does not, and every pixel ran the loop and the switch. Shaders are now written as a chain of
guarded blocks (`en_chaine`): `if (pc == N) do { … pc = M; break; … } while (false);`, in address order. With no
backward jump and no computed jump (1,953 shaders out of 2,388), a single pass is enough: no loop and no switch left.
Otherwise (435 shaders) the chain is repeated, a backward jump taking effect at the next pass.
`RECOMP_SHADER_SWITCH=1` gives the old shape back.

| Measurement | Old shape | Chain |
|---|---|---|
| AMD integrated, 1080p, title screen | 30 fps | 35 fps |
| AMD integrated, 1080p, 3D scenes of the cinematic | 0.1 fps | 2.5 to 13 fps |
| AMD, compiling the 2,387 shaders (empty cache) | over 4 min | 53 s |
| NVIDIA, precompilation (empty cache) | 26 s | 27 s |
| NVIDIA, 100 s of play: frames lost out of ≈ 4,700 | 122 | 129 |
| NVIDIA, committed memory at 96 s | 9.26 GB | 9.05 GB |

On NVIDIA the snapshots show the same scenes, correctly rendered. The modified translator produces, byte for byte, the
2,388 GLSL files that were tested (first obtained by a text rewrite, `scratchpad/…/chaine.py`). Never tried on a
discrete AMD card: the gain is expected there but not measured.

**How to read these figures** (the user's reminder): the integrated card is very weak. Its 2.5 to 13 fps are its own
limit, not the fix's. What carries over to the players' cards is the ratio (25 to 130 times more frames in 3D scenes),
not the figure: their dedicated cards were at 1.5 and 5.6 fps with the old shaders, where this one was at 0.1. Nothing
is measured on a dedicated card; do not quote 2.5 to 13 fps to players.

**Image defects on AMD: two causes, found and fixed on the evening of 2026-11-11** (the user's request: "this has to be
fixed"). The defects noted in the afternoon (black bars of the cinematic shown gray, "white patches", gray area
instead of the title-screen logo, doubled subtitles) were a single one; the user reported a second one during the
search: "a mesh shows on more or less all textures and text".

1. *Black shown as mid gray.* The gray was exactly 188 in sRGB, that is 0.5 linear: the neutral value that
   `image_to_general` puts in a new image. The end-of-chain shader (`fxaa_luma`, fragment `00918579fe4c058f`) runs the
   color through a 64³ 3D table (`scene_lut_generation.comp.glsl`, compute `434a7989da7958c5`, run again at every
   grading change). Table read back on AMD (`RECOMP_TEX3D_DUMP`): 2,944 to 3,072 cells still at (0.5 0.5 0.5 1), at
   x 0..47, y 0..7, z 0..7, that is the first six work groups of the compute, the corner of the dark colors. Cause:
   no barrier after the neutral clear (`vkCmdClearColorImage`). So nothing ordered the clear and the compute that
   writes the image in the same submission; the AMD driver let the clear finish after the first writes and covered
   them. NVIDIA executes in order. Fix: `barrier_all()` after that clear (and after the one of the dummy images).
   After: 0 neutral cell, cell (0, 0, 0) at 0, black fades and bars, title screen without a gray area, no more
   "white patches".
2. *The mesh.* At full resolution (`RECOMP_SNAPSHOT_FULL`), the outline and the diagonal of each letter's rectangle
   stand out, by 2×2 pixel squares. The game's fragment shaders compute their screen derivatives by exchanging a value
   with the neighbouring pixel of the same square (`SHFL.BFLY` 1 or 2), then subtracting it according to the pixel's
   lane (`FSWZADD`): 295 shaders, 3,658 exchanges. The translator wrote this with Vulkan subgroups
   (`subgroupShuffleXor`, `gl_SubgroupInvocationID & 3`). On NVIDIA the subgroup of a fragment shader follows the
   squares, helper pixels included; on AMD a helper pixel (outside the triangle) is not an active lane, and the value
   exchanged at the edge of every triangle is undefined. Measured on NVIDIA with two probe shaders: neighbour "1" is
   at x ± 1, neighbour "2" at y ± 1, the lane is parity(x) | parity(y) << 1. New form (`glsl.py`, `QUAD_AIDES`):
   neighbour = v ± `dFdxFine(v)` or v ± `dFdyFine(v)` depending on the pixel's parity, lane from the parity.
   Equivalence probe on NVIDIA (title text): 0 pixel with a different value, 0 different lane. On AMD: no more mesh.
   `RECOMP_SHADER_QUAD_SUBGROUP=1` at translation time brings the old form back. (This removed the mesh on text.)
3. *The mesh left on 3D objects* (reported by the user after point 2: "on AMD the meshes are still there on
   textures"). 251 of these shaders also make the pixels vote (`VOTE.ALL` → `subgroupAll`) before skipping a block:
   on the console, if a single pixel of the group needs the block, all of them run it, helper pixels included, so
   that the level of detail of the textures read in the block stays right. On AMD a helper pixel does not vote and
   follows its own path: at the edge of every triangle, its neighbours' coordinates were not computed, and the edges
   stood out as dotted lines (pedestal of General Pepper's room, Arwing hull). I had first written that these votes
   had no consequence: that was wrong. Fix: `VK_KHR_shader_quad_control`, offered by both drivers. The 295 fragment
   shaders that vote or exchange are translated with `layout(full_quads) in;` (helper pixels take part in subgroup
   operations) and `layout(quad_derivatives) in;` (`glsl.py`, `QUAD_CONTROL`). The engine enables the extension when
   the driver offers it, except on NVIDIA where subgroups already follow the quads and the rendering is validated
   without it; when it is not enabled, it removes these marks from the SPIR-V at load time
   (`spirv_sans_quad_control`: capability 5087, extension, execution modes 5088 and 5089), which gives the previous
   module back. `RECOMP_QUAD_CONTROL=0` or `1` forces either; `RECOMP_SHADER_NO_QUAD_CONTROL=1` translates without
   the marks. After: pedestal and hull identical to NVIDIA on full-resolution captures (`amd_qc4`, `nv_plein`). A
   driver without this extension (old AMD driver, Intel not tried) keeps the defect of point 3.
4. *Compute: groups of 32 lanes.* Six compute shaders (among them `terrain.clutter_generate` and
   `terrain.clutter_cull`) vote, count and exchange between the 32 lanes of a console "warp", with 32-bit masks.
   NVIDIA has subgroups of 32; AMD of 64 by default, 32 to 64 allowed in compute. The engine requires 32
   (`VkPipelineShaderStageRequiredSubgroupSizeCreateInfo`, `Vk::sg32`, `RECOMP_NO_SG32=1` to compare) when the card
   takes another size by default. **Not verified by the picture**: these computes are mostly used in missions (grass
   and rocks of the terrain), which the integrated card does not reach in a reasonable time; only checked that
   precompilation and the cinematic pass with it.

*Hypotheses ruled out by test*, not to be taken up again: minimum, maximum and saturate letting a NaN through on AMD
(shaders retranslated with safe versions: picture unchanged, change removed); automatic texture level of detail
inside conditional code (level 0 forced: unchanged); `gl_FragCoord.w` of helper pixels (replaced by 1: unchanged);
D24S8 depth format missing on AMD (the game uses formats 52 and 54, present); the table's sampler (clamp to edge,
linear: correct).

*Method that worked*: `RECOMP_RT_SNAPSHOT=N` to find the pass where the defect is born, `RECOMP_VK_DEBUG=N` (which now
writes the shader hashes of every draw), then shaders modified by hand in the test game's cache with a marker color
(`essai_glsl*.py` scripts of the scratch folder; tests on the bits so that no compiler can remove them). New
diagnostics: `RECOMP_TEX3D_DUMP=N` (raw content of 3D textures), `RECOMP_SNAPSHOT_FULL=1` (snapshots not reduced: the
mesh was invisible on half-size snapshots), `RECOMP_3D_LOG=1` (who writes into 3D textures).

What remains true: nothing has been tried on a dedicated AMD card, and the giant shader below is excluded on AMD.

**The game's biggest shader** (compute `f35f4f42e2496a38`, 2.7 MB of SPIR-V, 711 blocks, 18,844 lines of GLSL): AMD's
compiler goes beyond 11.5 GB of memory in 14 to 36 s, whatever the shape (switch, 711 functions with global registers,
functions marked DontInline, chain). NVIDIA's compiles it in 9 s with 0.8 GB. On a loaded 32 GB PC, this is the
0xC0000005 crash of issue #11: its log stops at `@progres 0.951` then `0.996` because only the main thread writes the
progress. This shader is used neither in the opening cinematic nor in training (5 minutes with it excluded: never
requested); so it is not what causes the NVIDIA driver's 2 GB spike.

**Protected precompilation** (`precompile_enfant`, `precompile_superviseur`). The supervisor watches the compiler
process's memory ten times a second: beyond the cap (8 GB for a single shader, 16 GB for all threads together;
`RECOMP_PRECOMPILE_MAX_MO` for tests), or when the PC has less than 2 GB of available commit, it stops it before the
compiler crashes. The shaders in progress are then recompiled one by one under the single-shader cap; the one that
exceeds it is excluded for this driver (`shaders_exclus.txt`, "its compiler needs too much memory") and precompilation
resumes. If the PC is short of memory even without the compiler: clear error ("close other programs"), no shader
excluded. Test on the AMD driver, cap at 8 GB: stopped at 7.8 GB after 31 s, shader isolated and excluded, restart,
2,387 shaders ready, 64 s in all, 7.4 GB free at the lowest. On NVIDIA: simulated crash isolated as before, normal
precompilation unchanged.

**Seen along the way, to be checked**: with `native_resolution = false` ("Like the console"), the picture stayed gray
during a test on AMD: the compute shader `059b307b6448e8a1`, created by the game and never translated, was missing
(`draw skipped (missing compute shader)`). Not yet tried on NVIDIA with that setting.

## Forced rebuild after an update, uninstall (2026-11-11, for 1.0.3)

Requests from the user: "when we publish an update that requires a rebuild, it must be forced" and "add simpler
cleanup routines for uninstalling".

**Change of the evening of 2026-11-11: the rebuild is offered, no longer forced.** The user's last request: "I want you
to leave the player the choice to rebuild the game or not when we update elements that require it, with a mention:
some improvements require rebuilding the game". What holds now:

- the launcher still detects the steps to redo (point 1 below), but the main button stays PLAY and starts the current
  game; the home screen shows "Some improvements require rebuilding the game." and the link "REBUILD NOW ›"
  (`Z_MAJ_JEU`, only the changed steps);
- the question at the first opening of a new version (point 2) carries that mention; "no" lets the player play;
- the executable no longer refuses to start (point 3): `version_du_jeu_a_jour` only writes "some improvements require
  rebuilding the game" in the game's log, which a bug report will show. No more exit code 5, no more
  `RECOMP_NO_VERSION_CHECK` nor `RECOMP_SANS_DIALOGUE`.

Consequence to keep in mind when following issues: a player can run a game older than their launcher. The first
useful line of their `recomp_run.log` says so. A game built by 1.0.2 keeps its saves in `recomp\build\full\save` as
long as it is not rebuilt.

Test: `verif/essai_choix_maj.py`, 24 checks (decline: nothing starts, the rebuild stays offered); capture of the home
screen in that state checked by eye. What follows describes the first version (forced rebuild); points 1 and 2 remain
true for the detection and the question.

Two consequences of the choice, handled the same evening:

- *Launcher 1.0.3 with a game built by 1.0.2.* This case did not exist while the rebuild was forced. Checked by
  reading the 1.0.2 sources (update archive kept by the verifier), not by a run with a real 1.0.2 executable:
  `--precompile` already exists, unknown `recomp.ini` keys (`memory_saver`, `texture_release`…) are logged and
  ignored, `RECOMP_VOLUME` is the same, and the launcher starts the game even if precompiling fails. Moving the saves
  to `%APPDATA%` is done by the 1.0.3 executable at its first start (copy, the old folder stays): nothing moves the
  saves of a game left on 1.0.2.
- *Rebuild cancelled midway.* `link.py` wrote `recomp_main.exe` directly: the linker empties the file as soon as it
  opens it, so cancelling during the link left an empty or truncated executable, with a PLAY button that is now
  active. `link.py` now writes `recomp_main.part.exe` and puts it in place in one go (`os.replace`). Test on the test
  install (process tree killed the way the launcher's cancel does it): stop at the opening of the file (0 bytes), in
  the middle of the write (841 MB out of 855) and after the write before the move: the previous executable keeps the
  same SHA-256 each time. The partial file stays on disk until the next link, which deletes it. Cancelling during the
  shader step leaves a mix of old and new `.spv`, which the 1.0.3 executable accepts (it strips the `quad_control`
  marks itself when the driver does not take them).

**Forced rebuild (first version).** Three locks, from the widest to the narrowest:

1. *Step signatures*: `preparer.py --a-refaire --copie <copy>` writes `@a_refaire recompiler,lier,…` (the steps whose
   signature changed: code they depend on, game copy) and does nothing else (0.2 s). The launcher asks at every start
   (`verifier_reconstruction`, `WM_REFAIRE` message): if a step is left to redo, the game is "to update", the main
   button becomes UPDATE GAME and starts the build, never the game. Until then only the version number
   (`.preparation\version.ok` ≠ `recomp\VERSION`) made the game obsolete: sources changed without a new number let the
   old executable run.
2. *Question to the player*: the first time a new launcher version is opened (`version_vue` in `lanceur.ini`; update
   installed by `maj.py`, or package replaced by hand), if the game was built by another version, the launcher offers
   to update it right away (`maj_jeu_question`). "No": nothing starts, the button stays UPDATE GAME and the game does
   not start before that. Only the steps that changed are redone. (The first version of this point started the update
   on its own; the user asked the same day to leave the choice.)
3. *Executable*: `recomp_main.exe` refuses to start when `version.ok` and `VERSION` both exist and differ
   (`version_du_jeu_a_jour`, `host_main.cpp`: message, exit code 5). The launcher never starts such a game; this closes
   the double-click door. Exemptions: `RECOMP_CONSTRUCTION` (set by `preparer.py`, which starts the game during the
   build, before it has written the version), `RECOMP_NO_VERSION_CHECK=1` (tests), missing file (executable outside an
   installation). `RECOMP_SANS_DIALOGUE=1` removes the message box (tests).

**Package updates are never installed on their own** (the user's request: "do not make launcher updates mandatory,
put a prompt that leaves the choice"). When `maj.py --verifier` finds a newer version, the launcher asks
(`WM_MAJ_DISPO`, `maj_question`): "yes" installs it; "no" is remembered for that version (`maj_refusee` in
`lanceur.ini`) and the "UPDATE … AVAILABLE" banner stays there for later. Since the evening of 2026-11-11 nothing is forced any more:
neither installing an update, nor the game rebuild it asks for. The two questions never show on top of each other: if a question
is already on screen, the new-version one waits for its turn (`MINUTEUR_MAJ`). Test: `verif/essai_choix_maj.py`
(throwaway fake package, real launcher without test hooks, real dialog boxes that the script answers yes or no: 24
checks), wired into the verifier's `launcher` check.

Tests: in the test installation, untouched sources → "game ready", PLAY button; one line appended to
`runtime/os/settings.cpp` → `game to update before playing, steps to redo: lier`, UPDATE GAME button
(`scratchpad/integration/essai_lanceur_refaire.py`). Version mark changed to 1.0.2 → the game exits with code 5.

**Uninstall.** `recomp/launcher/desinstaller.py` removes everything the package wrote on the PC:

| Item | Location |
|---|---|
| built game, shaders, logs | `recomp\build` |
| generated C++ code | `recomp\out` |
| files extracted from the game copy | `extracted`, `work` |
| compiler copy | `%ProgramData%\StarFoxRecomp` |
| temporary file of a game interrupted by a power cut | `%TEMP%\recomp_textures_*.tmp` |
| the package | the installation folder |
| saves and their backups | `%APPDATA%\SpaceAnimalsRecomp`, only with `--sauvegardes` |

- `--lister` (nothing is deleted), `--nettoyer` (frees the space: built game, generated code, extracted files,
  compiler copy; the launcher, `recomp.ini` and the saves stay), `--desinstaller` (everything).
- Never deleted: the player's game copy (`.nup`, `.nsp`, `.xci`, `.nsz`), even when it sits in the installation
  folder; that folder then stays, emptied of the rest.
- Before any deletion: backup of the saves (`sauvegardes.secours`), and a save left in `recomp\build\full\save` (up to
  1.0.2) is moved to `%APPDATA%\SpaceAnimalsRecomp\save`.
- Refused in a source folder (`.git`, `recomp\launcher\lanceur.cpp`, `recomp\verif`, `recomp\depot`) and when the game
  or the launcher of that installation is running.
- The package's Python cannot delete itself: PowerShell finishes after it exits, and only touches folders of the
  installation (the script may also be run by the system Python).
- In the launcher: UNINSTALL button on the CONSTRUCTION page (installed package only), two questions (the second one
  about the saves, "no" by default), then the launcher closes. Without the launcher: `uninstall.bat` at the root of
  the package.
- Log: `%TEMP%\SpaceAnimalsRecomp-uninstall.log`.

Verification: `desinstaller` check (`verif/essai_desinstaller.py`, 7 scenarios on fake installations with test
`APPDATA`, `ProgramData` and `TEMP`); the `paquet` check requires `desinstaller.py` and `uninstall.bat`.

## The game's watchdog: counter stopped during a long compilation (issue #13, 2026-11-11, for 1.0.3)

**The report.** RX 6800 XT, package 1.0.2: black window at startup, then the game stops. In `recomp_run.log`: pipeline
cache loaded at 4.7 s, nothing more, then at 78.4 s "vwatchdog.c(98): error: game-0-watchdog: System vgame_sim appears
dead: last reported in 74282 ms ago", `svcBreak` from `nn::diag::detail::Abort` and PANIC.

**The cause.** The game's engine has its own watchdog: if its simulation gives no sign of life for about 74 s, it
stops the game. On the console nothing blocks for that long. Here, the first Vulkan initialization prepares the shaders
(`warm_up`) while the simulation waits; on NVIDIA that is one to two seconds from the cache, on AMD much more (15.2 s
on the 7900 XTX of issue #7 with a complete cache), and a real compilation takes minutes. Past 74 s the game kills
itself. This is not a driver crash.

**Reproduced** on NVIDIA with `RECOMP_STALL_TEST=90` (simulated 90 s stall at the same place) and
`RECOMP_NO_CLOCK_PAUSE=1`: stop at 77.4 s with the same message ("last reported in 74548 ms ago"), same stack.

**The fix** (`runtime/runtime.cpp`, `rt_stall_begin` / `rt_stall_end`; `vk_render.cpp`, struct `Blocage`). The watchdog
measures time with the guest counter (`rt_cntvct`). While the driver creates a pipeline or a shader library, and
during the whole startup preparation, this counter advances normally for 5 s, then stops; at the end of the stall it
resumes from where it stopped. For the game, a stall therefore lasts 5 s at most. An ordinary hitch (under 5 s) changes
nothing: in-game behaviour is the same as before. The counter is read without a lock (sequence counter), because the
game reads it very often.

With the fix and the same 90 s stall: first frame at 94 s, 2,400 frames at 60 fps, no stop.

**Not to be done again**: I first declared these two functions in `runtime/cpu.h`. That header is part of the key of
every object of the recompiled game (`build.py`, `GEN_HEADERS`): changing one line in it recompiles the 902 batches on
every player's PC (21 to 66 minutes). Restored identical (compared byte for byte with the one of the published 1.0.2);
the declarations are at the top of `vk_render.cpp`. `cpu.h` and `simd.h` are only modified when there is no other way.

**What this does not solve.** The player still waits during the preparation; the game just no longer stops. In 1.0.3
the launcher precompiles before starting and the game no longer prepares everything at startup when the cache is
complete (`shaders_on_demand`): the wait only comes back if precompilation did not finish. Not tried on an RX 6800 XT.
A shader prepared at first use in the middle of a game is covered the same way.

## Stale textures: images without an object that are still drawn (evening of 2026-11-11, for 1.0.3)

**The defect.** Seen by the user: in the end cinematic of Corneria, drawings from the Arwing's hull (white stripes,
orange lines, yellow bar) on Slippy's forehead. Seen again on NVIDIA in a game played without a player; absent from
the opening cinematic.

**What is measured.** On the console, the GPU reads a texture's data from the game's memory. Here the data is copied
to video memory at first use, then the image is kept. The game destroys its texture objects and reuses their memory
for other textures, but keeps drawing with descriptors that lead to the image of the destroyed object (`refs` counter
at 0): that image was never compared with memory again. Diagnostic `RECOMP_TEX_AUDIT=n` (every n frames, the source
memory of each texture in use is compared with what was uploaded): all the stale textures found in three games are
images without an object, none has a live object; 34 at once when going from the menu to the departure cinematic
(masks of 1,696 × 1,696 and similar).

**The fix** (`vk_render.cpp`, `flush_recheck`, called when a texture is bound, once per displayed frame): an image
without a live object that is still drawn is compared with memory, and uploaded again if it changed. Source released
to Windows (`texture_release`): compared only if its pages were touched, which costs one lookup. Otherwise (small
texture, `memory_saver` off): hash redone every 30 frames, spread by address. Measured on three automatic games: 5 to 7
re-uploads per game (mostly 2,048 × 2,048 textures), 106 to 157 periodic checks, 60 fps median, no skipped draw.
`RECOMP_NO_FLUSH_CHECK=1`: old behaviour. `RECOMP_FLUSH_LOG=1`: each re-upload is logged.

Added at the same time: `nvnMemoryPoolFlushMappedRange` is no longer ignored (`pool_flushed`, registry of flushed
ranges); a texture whose source is touched by a flush is compared again. This game hardly uses it (one call per game):
no measured effect, kept because it is the signal NVN provides.

**What is NOT verified.** Slippy's shot was not seen again with this fix: the two automatic games made with it ended in
a game over in the city, before the cinematic (the automatic player takes a random path), and a third was stopped by
the PC memory guard. What I have: one game with the diagnostic in "forced re-upload" mode (`RECOMP_TEX_AUDIT_FIX=1`),
where the face is clean (capture at frame 27,120), and an older game with nothing, where the defect is there. The link
between the defect and the images without an object is therefore likely, not demonstrated: I do not know which of the
re-uploaded textures is the forehead's. The user chose to consider that it works for now. To be checked by eye in the
next game; if the defect comes back, run again with `RECOMP_TEX_AUDIT=30 RECOMP_FLUSH_LOG=1` and read the "STALE" lines
around the shot.

**Ruled out.** Data rewritten under a live texture object (detail levels loaded afterwards): the audit found none.

## Launcher: larger window, nothing opens during the build any more (evening of 2026-11-11, for 1.0.3)

Requests from the user: "the launcher's video is published on GitHub, I do not want the first launch to be used to
record the cutscene, it can be confusing for an ordinary user" and "make the launcher window a bit larger".

- **Background footage.** A build (the first one like the following ones) never starts the game to record it any more,
  Then, at 23:00, the user settled it: "there is no re-recording, the launcher uses the video from the repo, nothing
  more". The `extraits` step of `preparer.py`, the "RE-RECORD FOOTAGE" button of Settings and their texts are removed:
  nothing creates, replaces or deletes these files any more. `--etapes …,extraits` and `--sans-extraits` (old
  launchers) are accepted and ignored. Without footage the launcher shows its background without video. The runtime's
  recorder (`RECOMP_RECORD_DIR`, `record.cpp`) stays, as a development tool.
- **Shader survey.** During the first build the game used to run for about fifteen seconds in a window to collect the
  shaders it creates at startup: that is what a player saw opening by itself. It now runs with no visible window
  (`RECOMP_WINDOW_HIDDEN=1`, `present.cpp`: the window exists for Vulkan but is never shown, not even in the taskbar).
  Test: 1,200 frames at 60 fps with no window; real `pipelines` step on the test install, survey done in 14 s.
  Launcher and README texts updated to match.
- **Window.** 1,200 × 720 at 100 % instead of 1,000 × 600 (`ECHELLE = 1.2` in `lanceur.h`). Drawing and clicks stay in
  the logical 1,000 × 600 frame: it is the display scale `g.dpi` that is multiplied (`dpi_fenetre`), so nothing has to
  be laid out again and text stays sharp. On a screen that is too small the scale is reduced so the window fits (down
  to 0.75). Captures of the home screen and of Settings checked by eye.
- Under "Animated background", "taken from your copy" becomes "provided with the launcher".

- **Text.** All font sizes go through `format()` in `dessin.cpp`, multiplied by `TAILLE_TEXTE = 0.9` (request: "make
  the text a bit smaller"). The layout does not move.
- **Launcher icon.** The window took the game's icon once the game was built (`icone_jeu`): removed, the launcher keeps
  its own everywhere (request: "it shows the game's icon instead of the launcher's").
- **Game name for Windows.** `link.py` adds version information to `recomp_main.exe`: description "StarFox Remake",
  product "StarFox Remake [recompiled]", version of the sources. Task Manager and the volume mixer show that name
  instead of "recomp_main"; the file keeps its name. Checked: description read by Windows on the file and on the
  running process.

**Incident of the same evening.** The old "RE-RECORD FOOTAGE" button deleted the footage before recording: game window
closed at 21:26, no footage left, `paquet.py` failing ("aucun extrait"). The six published files were put back,
identical (SHA-256). The button no longer exists.

Note: the `launcher` check failed once on a loaded PC (one update question did not appear in time, 19 checks out of
24), then passed 24 out of 24 on its own. Sensitive to load, not a launcher defect.

## Keyboard keys editable from the launcher (evening of 2026-11-11, for 1.0.3)

Request from the user: "make it possible to change the controls from the launcher, with 2 default mappings (one in
WASD (QWERTY) and one in ZQSD (AZERTY))".

**The game** (`runtime/os/settings.h`, `settings.cpp`, `input.cpp`). One key per controller action: 12 buttons, 4 D-pad
directions, 4 directions and the click of each stick, so 24 keys (`Settings::keys`). They are read from `recomp.ini`,
keys `key_a`, `key_b`… `key_rs_right`, section `[keyboard]`, as scancodes: the physical position of the key, `0xE0xx`
for an extended key (arrows, right Ctrl…), 0 for no key. Without these keys, the usual positions (same as 1.0.2).
`MapVirtualKeyW(…, MAPVK_VSC_TO_VK_EX)` then `GetAsyncKeyState`; the numeric keypad answers as digits and as
navigation keys. Enter, Backspace, Esc and Tab remain menu shortcuts (A, B, +, -) as long as the player does not give
them to an action. The game's log says how many keys differ from the default positions ("keyboard: N key(s) changed").

**The launcher** (`lanceur.cpp`, `dessin.cpp`, `textes.cpp`). The Controls page shows one box per key: a click, then
the wanted key (Esc or a click: cancel). A key already used by another action is swapped. At the top, a "Keyboard keys"
selector: two presets, "WASD (QWERTY)" and "ZQSD (AZERTY)", and "Custom" as soon as a key has been changed. The presets
are defined by the letter printed on the key and resolved on the keyboard layout set in Windows
(`MapVirtualKeyW(letter, MAPVK_VK_TO_VSC)`):
- WASD: left stick W A S D, Y on Z, L on Q, + on M;
- ZQSD: left stick Z Q S D, Y on W, L on A, + on the comma;
- the rest is shared (A: C, B: X, X: V, R: E, ZL: R, ZR: T, -: N, clicks F and H, right stick I J K L, D-pad: arrows).
On a QWERTY keyboard the first one, on an AZERTY keyboard the second one, give exactly the game's default positions:
that is the one the selector shows as long as the player has changed nothing. The 24 keys are only written to
`recomp.ini` at the first change. Changes apply the next time the game starts (settings are read at startup).

**Tests.** `essai_touches_launcher.py` (session work folder, not in the repository): test launcher on the test install,
clicks and keys sent to its window, `recomp.ini` read after each step: 16 checks out of 16 (change, swap, cancel with
Esc, extended key, both presets on this AZERTY PC). Game linked and started with 5 keys changed: "keyboard: 5 key(s)
changed", no "invalid" line. Captures of the page checked by eye (ZQSD preset shown by default on this PC). **Not
tried**: keys pressed during a game (the game window must be in the foreground); for the user to do.

Controllers cannot be remapped: buttons by position, as before.

## Game memory: what could be given back to Windows (census, 2026-11-12)

The user's question: the game takes too much memory; could we not have a "dynamic reservation", taking memory as it
is needed and giving it back when the game no longer needs it? The first half exists since 1.0.3 (on-demand commit, by
2 MB blocks). The second half only works when the game itself gives back a whole reservation (UnmapPhysicalMemory, at
area changes): inside its reservations the game manages its memory alone and tells nobody when it frees an object.

**Diagnostic added** (`memory.cpp`, `RECOMP_MEM_CENSUS=s`, inactive without the variable): every s seconds, one page
in 16 of the committed on-demand ranges is examined. Empty: it contains only zeros; given back to Windows, it would read
back as zero, nothing would change for the game. Touched: the game read or wrote it since the previous census (method:
the page is removed from the process working set with `VirtualUnlock` on a page that is not locked, its content stays
in RAM; it comes back on the first access; `QueryWorkingSetEx` tells). Reads go through `ReadProcessMemory`, which does
not trigger the on-demand commit. Estimates: sample fraction × exact committed memory, in total and per range. Cost:
0.14 to 0.55 s per census on a separate thread.

**Measurement** (Corneria mission played without a player, 1440p, 10 minutes, census every 45 s, Zen closed):

| | Value |
|---|---|
| Committed memory of the process | 7.7 to 9.7 GB (peak 9.69) |
| Game memory in the on-demand ranges | 2.9 to 3.9 GB (texture data, 1.3 to 2.9 GB, is already in the temporary file) |
| of which empty pages (only zeros) | **1.30 to 1.35 GB, stable from start to end** |
| touched by the game in 45 s | 0.14 to 0.74 GB |
| untouched for 5+ censuses (≈ 4 min) | 1.6 GB, of which 1.24 GB of empty pages |
| untouched for 10+ censuses (≈ 7.5 min) | 1.56 to 1.59 GB |

The empty pages are mostly in reservations of 80 to 112 MB committed whole and 62-99 % empty (0x79d8c00000,
0x79de000000, 0x7946a00000, 0x7854c00000, 0x7867200000…): reservations cleared by the game or committed up front for
the kernel or the driver (`mem_ensure`), never filled. The big 3 GB reservation (textures) has almost none (1 to 3 %).
The dip to 20.9 fps at 430 s is an area change (1.3 GB of textures uploaded in 10 s), not a census.

**What could be given back without understanding the game**:
- empty pages: ≈ 1.3 GB, with no difference for the game (it would read zeros). To do: a periodic sweep which, under
  the `mem_fault` lock, protects the page read-only, checks it again, then decommits it; to exclude: windows imported
  into Vulkan (the GPU reads and writes there) and pages being released as texture data; on a later access, commit only
  the touched piece (otherwise the 2 MB block would commit the given-back pages again);
- full pages untouched for several minutes: ≈ 0.4 GB more, to be put in the temporary file like texture data; an access
  by the game would bring them back (short wait). Riskier, smaller gain.
That is ≈ 1.3 to 1.7 GB out of a 9.7 GB peak. The other levers remain those of the "In-game memory spikes" section:
cache of images without an object (up to 1 GB), spikes of the driver's compiler (2 GB, unresolved).

**Caveat on the measurement**: a page read only by the GPU (imported window) is not seen as "touched"; it must never be
given back. A single game measured; the automatic player's path changes from one game to the next.

## Game memory: empty pages given back to Windows (2026-11-12, for 1.0.3)

Follow-up of the previous section, at the user's request: the pages of game memory that contain only zeros are given
back to Windows during the game. The game sees no difference: if it comes back to them, it finds them as zeros, the
content they had.

**How** (`memory.cpp`, `vides_balayer`, a low-priority thread): at 10 s, 25 s, then every 30 s, the committed 64 KB
pieces of the on-demand ranges are examined.
- Candidates: read-write, in a 2 MB block committed for more than one sweep (`g_age`: a block that was just committed
  is often being filled), outside the windows imported into Vulkan (`mem_epingler` at import, `mem_desepingler` when
  the pool is freed: the GPU reads and writes there without going through a fault).
- Read with `ReadProcessMemory`, which does not trigger the on-demand commit. A piece already found full is read again
  only one sweep in four, and only if it is in RAM (`QueryWorkingSetEx`: nothing is brought back from the page file).
- Given back in runs of at most 2 MB, under the `mem_fault` lock (`g_rel_lock`) and the kernel guard taken exclusively
  (otherwise the run waits for the next sweep): read-only protection, second check, then `VirtualFree(MEM_DECOMMIT)` of
  the pieces still empty; the others go back to read-write. A write by the game in the meantime faults and waits for
  the lock, then finds the page decommitted: `mem_fault` commits its piece, and the write resumes on zeros.
- When the game comes back, `mem_fault` commits only the 64 KB piece that was touched (`g_vides`): the 2 MB block would
  commit again all the given-back pages around it.
- Kernel guard (`MemNoyau` in `hos.h`): held shared from the commit of a range to the end of the call that writes there
  without going through the game's processor, hence without a fault: romfs reads (`ReadFile`, `svc_fs.cpp`) and import
  of a window into Vulkan (`vk_render.cpp`).

**Setting**: `zero_page_release = true` in `recomp.ini` (appended at the end of old files); `false` turns it off; no
effect if `memory_saver = false`. Variables: `RECOMP_ZERO_PAGE_RELEASE=0/1`, `RECOMP_ZERO_SWEEP_S=s` (another period,
tests). Log: `empty game pages given back to Windows: on (sweeps at 10 s and 25 s, then every 30 s; pieces of 64 KB)` at
startup, then one
`[mem]` line per sweep that gives something back (one in ten otherwise): given back now, by this sweep, total, committed
again when the game came back, pieces eligible and read, runs postponed, duration.

**Measurement** (Corneria mission without a player, 1440p, Zen closed, same settings as the census, sweeps every 30 s
from the start; the game was closed at 443 s through its window, exit code 0, not a crash):

| | Without (census) | With |
|---|---|---|
| Empty pages given back | — | 1,050 MB at the first useful sweep (90 s), then 994 to 1,029 MB |
| Committed again when the game came back | — | 117 MB in 7 minutes |
| Committed memory of the process, every 30 s (GB) | 7.8 7.5 8.4 8.9 9.0 8.6 8.0 8.2 8.7 8.9 9.0 9.1 9.0 9.5 | 7.2 7.5 6.5 6.6 7.6 7.9 7.4 7.1 8.2 7.5 7.7 6.5 6.0 7.4 |
| Average and peak over these 7 minutes | 8.6 and 9.5 GB (9.69 GB in 10 minutes) | 7.2 and 8.3 GB |
| Empty pages left (census) | 1.30 to 1.35 GB | 0.31 to 0.34 GB |
| Duration of a sweep | — | 15 to 281 ms, on its own thread |
| Runs postponed, refusals from Windows, skipped draws | — | 0, 0, 0 |

The safe gain is the one the sweep measures itself: a bit more than 1 GB given back permanently. The gap between the
readings (1.4 GB on average) includes some chance: the automatic player does not follow the same path from one game to
the next. Frames per second: median 60 in both games.

Second game, with the amiibo service of the next section (3 minutes, new game: title screen, prologue, tutorial):
1,038 MB given back at 60 s, then 1,054 to 1,062 MB; 19 MB committed again; sweeps of 16 to 421 ms; no run postponed,
no refusal from Windows, no skipped draw; 60 fps from start to end. The peak of the process (9.0 GB) came before the
first useful sweep: a block is examined only after a whole sweep, hence from 60 s on.

Hence the first sweeps moved to 10 s and 25 s. Same start of a new game, 3 minutes: 733 MB given back at 25 s already
(304 runs postponed: the tutorial was loading, romfs reads and Vulkan imports held the kernel guard), 1,021 MB at 55 s,
then
1,017 to 1,041 MB; peak of the process 8.6 GB instead of 9.0; reading at 45 s: 7.9 GB instead of 8.6. Sweeps of 78 to
281 ms; 78 MB committed again in 3 minutes; no refusal from Windows, no skipped draw. This game went further into the
tutorial (cinematic): dips to 37.7-50 fps with many textures uploaded and the image cache capped (4.4 to 5 GB of
available commit on the PC); they do not follow the sweeps, which are too short to weigh on a 10 s average.

**Not done**: full pages untouched for several minutes (≈ 0.4 GB more, to be put in the temporary file like textures),
the cap of the cache of images without an object, the spikes of the driver's compiler (section "In-game memory
spikes").

## Amiibo (nfp:user): an NFC reader without a figure (issue #9, 2026-11-12, for 1.0.3)

Issue #9 (three players): after the tutorial, on the first visit to the campaign menu, "Single Player" no longer
responds. At that moment the game opens the amiibo service `nfp:user` (Initialize, AttachAvailabilityChangeEvent,
ListDevices, then Finalize three seconds later). This service was the generic stub: command 0 of the root returned a
stub object, AttachAvailabilityChangeEvent no event handle although the game expects one, ListDevices no device,
GetState "not initialized".

**Now** (`svc_misc.cpp`, `make_nfp_user`): the service imitates a console with an NFC reader (player 1's controller)
and no figure on it, like Ryujinx; this is the path the game knew on the console.
- `CreateUserInterface` (0) returns an `IUser` object; Initialize (0) and Finalize (1); GetState (19): initialized or not.
- ListDevices (2): one device (handle 0, player 1's controller); before Initialize: WrongDeviceState.
- StartDetection (3) and StopDetection (4): the device searches for a figure (GetDeviceState, 20), which never comes.
- AttachActivateEvent (17), AttachDeactivateEvent (18), AttachAvailabilityChangeEvent (23): a real event handle, never
  signaled.
- Mount, application data, figure information (5, 7 to 16, 24): WrongDeviceState failure (module 115, code 73), as
  without a figure. GetNpadId (21): player 1. GetApplicationAreaSize (22): 0xD8.
- Any other command: generic stub, written to the log.

**Not verified**: our test games start from saves of a finished campaign, or from a new game that stays in the
tutorial (the automatic A press cannot bank the ship); none goes through this menu, and the game never opened
`nfp:user` in them. The link between this service and the stuck button is not proven. To be confirmed by an affected player (new game, tutorial,
then the campaign menu), or with a save taken just after the tutorial.

## Game memory: forgotten pages put in the temporary file (2026-11-12, for 1.0.3)

Follow-up of the empty pages, at the user's request: the full pages of game memory that the game has not touched for
4 minutes are put in the temporary file of the textures, and read back if it comes back to them. The census estimated
them at 0.3-0.4 GB.

**How** (`memory.cpp`, `vides_balayer`, same thread and same sweeps as the empty pages):
- Knowing that a 64 KB piece is no longer touched: the sweep takes it out of the process working set (`VirtualUnlock`
  on pages that are not locked; their content stays in RAM, on the Windows modified page list). On the first access by
  the game, the runtime or the kernel, the page comes back (soft fault, about a microsecond); `QueryWorkingSetEx` tells
  the next sweeps. The check is done at the start of the sweep, before its reads, which would bring pages back.
- Only the pieces found full by the empty-page sweep are watched, outside the windows imported into Vulkan. Pieces are
  taken out from the 4th sweep on, at most 128 MB per sweep, rotating over all the full pieces; a single `VirtualUnlock`
  per 2 MB block (64 at most, a 2 ms pause every 8). A piece found touched waits 4, 8 then 16 sweeps before being taken
  out again; a piece read back from the file, 16.
- A piece that did not come back for 8 sweeps (4 minutes) is put away: under the `mem_fault` lock and the kernel guard
  taken exclusively, read-only protection, write to the file, decommit; it enters `g_rel` like released texture data,
  marked `Rendu::froid` (counted apart from the textures). Runs of at most 2 MB, a 1 ms pause before each; same for the
  empty pages, a pause every 8 runs: a game thread waiting for the lock gets through (an SRW lock released then taken
  again at once by the same thread does not always let the waiters through).
- When the game comes back: `mem_fault` reads the 64 KB piece back from the file, as for a texture; `mem_ensure` and
  `mem_restore` do the same for a whole range.
- Locks are held at normal priority (the sweep thread is below normal): no priority inversion. The empty pages also
  check, under the lock, that the run is still committed read-write from end to end.
- Known limit, the same as for textures: while a page is being restored (less than a microsecond between its commit and
  its copy), another thread that accesses it without faulting reads zeros or sees its write overwritten; two threads
  would have to start touching the same page, forgotten for minutes, within the same microsecond.

**Setting**: `cold_page_release = true` in `recomp.ini` (appended at the end of old files); no effect if
`memory_saver`, `texture_release` or `zero_page_release` is `false`. Variables: `RECOMP_COLD_PAGE_RELEASE=0/1`,
`RECOMP_COLD_SWEEPS=n` (sweeps without access before a piece is put away, tests). Log:
`forgotten game pages put in the temporary file: on (...)` at startup, then one `[mem]` line per sweep that puts
something away (one in ten otherwise): put away now, by this sweep, in total, read back when the game came back, pieces
watched, taken out of the working set, runs postponed, duration. The file size in the textures line (`file … MB`)
counts them too.

**Measurements** (Corneria mission without a player, 1440p, same save copy as the previous games):
- Accelerated test (sweeps every 10 s, put away after 3 sweeps without access; first version): up to 931 MB put away;
  830 MB read back while Corneria loads, as it reuses the memory of the menu (read back from the file before being
  rewritten); no error, normal images.
- 10-minute game with the real settings (first version): first pieces put away at 5 min 28, 465 MB at 9 min 31 and
  still rising, 4 MB read back, no error. But frame-rate dips: the 10 s windows that contain a sweep fell to 56.7 fps on
  average (7 of 25 under 55) against 58.6 for the others. Cause: one `VirtualUnlock` per 64 KB piece (thousands per
  sweep) and runs of 256 KB put away (hundreds of protection changes and decommits); each of these calls interrupts
  every processor running the game to flush its TLB.
- Final version (one `VirtualUnlock` per 2 MB block, runs of 2 MB): 235 MB put away at 7 minutes, nothing read back, no
  error; windows with a sweep 57.9 fps (3 of 18 under 55), without 58.4 (3 of 24). Reference with the empty pages only:
  58.4 and 58.4; without any sweep (census): 58.0 and 58.8. This game was stopped at 7 min 09 by the test's guard:
  another program had taken memory, only 2.4 GB of available commit was left on the PC.
- Committed memory cannot be compared from one game to the next (the automatic player's path changes it by ±1 GB): the
  safe measure is the sweep's counter, each byte put away being decommitted.

Gain: 0.2 to 0.5 GB after 5 to 10 minutes of play, growing with the length of the game. Price: a few tens of
microseconds reading the file when the game comes back to a forgotten page.

## Launcher memory alert: 10 GB instead of 13 (2026-11-12, for 1.0.3)

The launcher refused the first click on JOUER (PLAY) below 13 GB of available commit (section "Game memory: textures
removed from RAM…"), a threshold computed when the game committed ≈ 9.7 GB. Since the empty pages and the forgotten
pages, the user's 19-minute game committed 6.4 GB on average and 7.4 GB at most; it was started with 9.6 GB free,
without any problem. At the user's request, the alert now only comes below 10 GB: `jouer()` in `lanceur.cpp`. The game
log (`host_main.cpp`, "available commit … TOO LOW") follows the same threshold. Thinner margin than before for the
spikes of the driver's compiler (up to 2 GB, section "In-game memory spikes"): if the game stops for lack of memory,
the launcher says so (code 4) as before.
