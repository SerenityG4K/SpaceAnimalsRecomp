# Changelog

## 1.0.1 — lighter package, third-party licenses

- **Licenses of the bundled tools**: the package now has `THIRD_PARTY_NOTICES.txt`, which lists every bundled component
  with its exact version, its license and where to get its source code, and a `LICENCES` folder with the full license
  texts (GPL 3, LGPL 3 and 2.1, GCC Runtime Library Exception, MinGW-w64, isl, zlib, zstd, glslang). Both are also in
  the source repository, in `recomp/licences/`.
- **Lighter package**: GCC and glslang are reduced to the files the build uses (list made by tracing the real build
  commands). The download is 91 MB instead of 316 MB, and 424 MB once extracted instead of 1.1 GB. When the install
  path has spaces, the compiler copy in `C:\ProgramData\StarFoxRecomp` is about 400 MB instead of 1 GB. Removed:
  Fortran, Objective-C, LTO, gdb and its Python, CMake, Doxygen, Cppcheck, ctags, premake, NASM, Yasm, ccache, gcov and
  the documentation.
- **Launcher**: in the source tree, `lanceur.exe` is now built at the root, next to `recomp`, as in the package.
- **README**: the project is experimental. It is at the "it runs" stage, not optimized yet, and has only been tested on
  an AMD Ryzen 7 9700X with an NVIDIA GeForce RTX 5070 Ti.
- **Updating from 1.0.0** with the launcher: no code of the game changed, so nothing is rebuilt and **UPDATE GAME** takes
  a few seconds. An install updated from 1.0.0 keeps its full compiler, which still works. To get the lighter one,
  extract the 1.0.1 package to a new folder.
- **Known issue** (also in 1.0.0): on a new install, the build extracts 2,308 of the game's shaders from your copy;
  80 more (72 compute shaders) that the game loads at start are not extracted yet. A scene that uses one would miss
  that effect. A fix is planned for the next version.

## 1.0.0 — first public release

StarFox Remake [recompiled]: Star Fox (Nintendo Switch 2, update 2.0.0) as a native Windows program, built on your PC
from your own copy.

- **Recompiler**: the whole game code (main, sdk, subsdk0 to subsdk3) translated from ARM64 to C++, then compiled to x86-64.
- **Runtime**: Horizon OS kernel and services emulated at the system-call and IPC level; the NVN graphics API runs on
  Vulkan 1.3; the game's shaders translated to SPIR-V (2,388 stages).
- **Shader precompilation**: every shader compiled for your GPU on the first launch with Vulkan graphics pipeline
  libraries, plus a pipeline cache on disk: no shader compilation stutter in game.
- **Launcher** (`lanceur.exe`): builds the game from your `.nup` copy (SHA-256 check, extraction, recompilation, link,
  shaders), with progress and logs; full screen or windowed; game language (15), resolution up to 4K, frame rate,
  3D rendering, volume; background clips recorded from your copy's cutscenes; French and English.
- **Updates**: the launcher checks this repository's releases, downloads and verifies an update (SHA-256), installs it,
  and rebuilds only the parts whose code changed.
- **Game icon** taken from your copy at build time (never distributed).
- The whole game can be finished: it has been played through completely several times at 60 fps with sound, and it is
  rather stable.
- Known limits: 80 of the 2,468 shader stages are left to translate; 90 and 120 fps are not stable yet.
- Planned: NVIDIA DLSS, frame generation, mod support, online play, mouse controls, key remapping and several controllers, Linux.
