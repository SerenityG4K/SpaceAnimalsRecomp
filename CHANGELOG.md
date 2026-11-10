# Changelog

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
