# SpaceAnimalsRecomp — StarFox Remake [recompiled]

> [!IMPORTANT]
> **This project is made almost entirely with AI.** Nearly all of the code, tooling, reverse engineering and documentation
> here is written by an AI (Anthropic's Claude, through Claude Code), directed, tested and reviewed by a human,
> [SerenityG4K](https://github.com/SerenityG4K). If you don't trust code written by AI, you can simply ignore this project.
> As with anything on the internet, you run the code at your own discretion.

**SpaceAnimalsRecomp is a static recompilation of Star Fox (Nintendo Switch 2, update 2.0.0) into a native Windows
application: StarFox Remake [recompiled].** The game's ARM64 code is translated to C++ and compiled to x86-64 on your PC,
the Switch 2 graphics API (NVN) runs on Vulkan, and the game's shaders are translated to SPIR-V. There is no emulation:
the whole game runs as a native program. The goal is a full PC version of your own copy of the game: a launcher, PC display
options, keyboard and controller, and the game's text and voices in any of its languages.

> [!WARNING]
> **Experimental project: it runs, but it is not optimized yet.** SpaceAnimalsRecomp is at the "it runs" stage. The whole
> game can be finished from start to end: it has been played through completely several times, at 60 fps with sound,
> subtitles, saving and loading, and it is rather stable. But nothing has been optimized yet, and it has only been
> tested on a powerful PC: **AMD Ryzen 7 9700X and NVIDIA GeForce RTX 5070 Ti**. How it runs on other hardware is not
> known yet. Bugs can still happen: 80 of the game's 2,468 shader stages are not translated yet, and 90 and 120 fps are
> not stable yet. If you find a bug, please report it on GitHub with your hardware: every report helps.

> [!TIP]
> **Want to contribute?** Read [CONTRIBUTING.md](CONTRIBUTING.md). Open an issue before starting on something big, so we
> don't work on the same thing twice. The developer notes (in French) in
> [docs/DEV-NOTES-fr.md](docs/DEV-NOTES-fr.md) explain how every part works and how each bug was found.

> [!NOTE]
> **No game files are included.** SpaceAnimalsRecomp needs your own legally obtained copy of Star Fox for Nintendo Switch 2,
> updated to version 2.0.0, dumped from your own console as a `.nup` file. This project does not include, and will not
> provide, game files, keys, firmware, dumping or decryption tools, or instructions on how to obtain any of them.
> SpaceAnimalsRecomp never decrypts anything: it only reads the file you already have, and never modifies it. The
> recompilation of the game code happens locally, on your machine, from your file.

## Before you report a bug

- Use the latest release: the launcher tells you when there is a new one.
- Search the [existing issues](https://github.com/SerenityG4K/SpaceAnimalsRecomp/issues) first.
- Say which CPU and GPU you use (and your GPU driver version): only one configuration has been tested so far.
- Attach the logs, all in `recomp\build\full\` next to `lanceur.exe`:
  - `recomp_run.log` for a game bug (the game writes a new one at each start, so take it right after the bug;
    the launcher keeps the previous one as `recomp_run_precedent.log`);
  - `preparation.log` for a build problem;
  - `precompile.log` for a shader compilation problem.
- Crash or graphics bug? Tell us where in the game it happens, and attach your save if you can (close the game and
  zip `recomp\build\full\save\`).

## Getting started (1.0)

You need:

- Windows 10 or 11, 64-bit;
- a CPU with AVX2 (2013 or newer);
- a GPU with a Vulkan 1.3 driver (tested on NVIDIA; AMD and Intel are untested so far);
- about 6 GB of free disk space during the build;
- **memory: 32 GB of RAM.** The game needs about 13 GB of free memory (RAM and page file) to run, so 16 GB of RAM is
  not enough yet ([#5](https://github.com/SerenityG4K/SpaceAnimalsRecomp/issues/5)). Close other programs (another
  game, the browser…) before playing.

The only configuration tested so far is a powerful one: AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5070 Ti. The project is
experimental and not optimized yet, so a less powerful PC may not reach a steady 60 fps.

**Handheld PCs:** not tested yet on handhelds such as the Steam Deck, Lenovo Legion Go or ASUS ROG Ally. On the Steam
Deck (SteamOS), the Windows build would have to run through Proton. Feedback is welcome in the
[Issues](https://github.com/SerenityG4K/SpaceAnimalsRecomp/issues).

1. Download `SpaceAnimalsRecomp-1.0.2-windows.zip` from [Releases](https://github.com/SerenityG4K/SpaceAnimalsRecomp/releases).
   It bundles everything the build needs (Python, GCC, glslang): nothing else to install.
2. Extract it to a folder you can write to, **not** Program Files, whose path has only Latin characters, for example
   `C:\Games\StarFox`. Spaces are fine: the compiler is then copied once to `C:\ProgramData\StarFoxRecomp` (about 400 MB).
3. Run `lanceur.exe`. Choose your copy (**COPY › choose**: your `.nup` file), then click **BUILD THE GAME**.
   - The launcher checks that it is Star Fox 2.0.0 by the SHA-256 hashes in the file's index, without reading all 30 GB.
   - It extracts the executables, recompiles the whole game code (35 to 50 minutes, the launcher stays usable),
     links `recomp_main.exe` and translates the game's shaders.
   - It then compiles every shader for your GPU, like recent PC games do on their first launch (about a minute), so
     there is no shader compilation stutter in game. The game also opens for a few seconds, muted, to collect the
     shaders it creates when it starts: let it run.
4. Click **PLAY**. Under the button, choose **FULL SCREEN** or **WINDOWED** (Alt+Enter or F11 also switch in game).
   **SETTINGS** has the game language (15 choices), resolution (up to 4K), frame rate, native or console-like 3D
   rendering, volume and update checks.

Your copy is never modified (read-only). The game's own icon is taken from your copy at build time and shown on
`recomp_main.exe` and in the launcher's window; it is never distributed. The launcher's background clips, gameplay
recorded by SerenityG4K, come with the package.

### Updates

The launcher checks this repository's releases when it starts (you can turn it off in **SETTINGS**). When a new version
is out, click **UPDATE … AVAILABLE** at the top: the launcher downloads it, checks its SHA-256 hash, installs it and
restarts. **UPDATE GAME** then rebuilds only the parts whose code changed, often in a minute or two. Your copy, your
build, your saves and your settings stay.

### Shader precompilation

Every one of the game's 2,388 translated shaders is compiled for your GPU once, on the first launch, and again only after a GPU
driver update. SpaceAnimalsRecomp uses Vulkan graphics pipeline libraries (`VK_EXT_graphics_pipeline_library`): each
shader is compiled on its own, ahead of time, and a draw only links precompiled pieces, which takes a fraction of a
millisecond. The result is kept in `recomp\build\full\pipelines.cache`. Deleting it only means compiling again on the next
launch. On a GPU without pipeline libraries, shaders are compiled the first time they are drawn, then kept in the same
cache.

2,308 of them come from the shader archive of your copy. The other 80 (72 of them compute shaders) are created by the
game when it starts, from elsewhere in your copy: the build runs the game for a few seconds to collect them, then
translates and compiles them too.

## Where are my saves?

In `recomp\build\full\save\`, inside the folder where you extracted SpaceAnimalsRecomp. To back up your progress,
close the game and copy the whole `save` folder.

## What you need

- Your own Star Fox for Nintendo Switch 2 (cartridge or digital) with its **2.0.0 update** (v131072), dumped from your own
  console as a single `.nup` file (title ID `0400E88025B1A000`). The launcher tells you if the file isn't the right
  game or version.
- Windows 10 or 11 on x86-64 with AVX2, a GPU with Vulkan 1.3, and 32 GB of RAM.

## Status

Estimates of where each part stands. "Tested in game" is how much of the game has been played through on
SpaceAnimalsRecomp so far.

|   | Part | Progress | What it means |
|---|---|---|---|
| ✅ | Recompiler | 100% | The whole game (main, sdk and the four subsdk modules) recompiles from ARM64 to C++ and native x86-64 |
| ✅ | Launcher and game setup | 100% | Copy check by SHA-256, extraction, code build, link and shader translation, all from your own file, with progress and logs |
| 🔧 | Shader translation | 97% | 2,388 of the game's 2,468 shader stages translate to SPIR-V; 80 are left to translate |
| ✅ | Shader precompilation | 100% | Every shader compiled for your GPU on the first launch: no compilation stutter in game |
| ✅ | Text and voice languages | 100% | Any of the game's 15 languages (Automatic follows Windows) |
| ✅ | Sound | 100% | The game's audio, mixed by its own audio engine |
| ✅ | Display | 100% | Full screen (borderless) or windowed, from the launcher or in game (Alt+Enter, F11); resolution up to 4K |
| ✅ | Updates | 100% | One click in the launcher; only the parts that changed are rebuilt |
| 🔧 | Frame rate | 90% | A steady 60 fps; 90 and 120 fps are not stable yet (55 to 118 fps in Corneria at 120 for now) |
| 🔧 | Horizon OS services | 90% | Threads, files, controllers, applets, time and audio: everything the game has used so far |
| 🔧 | Graphics (NVN on Vulkan) | 90% | The whole game draws correctly on NVIDIA; AMD and Intel are untested |
| 🔧 | Controls | 70% | Keyboard and one XInput controller (Xbox layout), by button position; no mouse or remapping yet |
| 🔧 | Tested in game | 90% | The whole game has been finished several times, and it is rather stable: report what breaks |
| 🗺️ | NVIDIA DLSS | 0% | Planned: DLSS on PC, with free output resolution |
| 🗺️ | Frame generation | 0% | Planned: generated frames between the ones the game draws, for a higher frame rate on screen |
| 🗺️ | Mod support | 0% | Planned: loading mods, managed from the launcher |
| 🗺️ | Online play | 0% | Planned: the game's online modes |
| 🗺️ | Mouse controls | 0% | Planned: the game's mouse mode, with the PC mouse |
| 🗺️ | Key remapping and several controllers | 0% | Planned |
| 🗺️ | Linux | 0% | Planned |

✅ done · 🔧 in progress · 🗺️ roadmap

## Controls

Controller buttons follow the position on the pad (the Switch's A is the Xbox B, and so on). Keyboard keys follow their
physical position, so the layout is the same on AZERTY and QWERTY keyboards (names below are QWERTY).

| Switch | Keyboard | Xbox controller |
|---|---|---|
| A | C or Enter | B |
| B | X or Backspace | A |
| X | V | Y |
| Y | Z | X |
| L | Q | LB |
| R | E | RB |
| ZL | R | LT |
| ZR | T | RT |
| + | M or Esc | Start |
| − | N or Tab | Back |
| Left stick (click) | W A S D (F) | Left stick |
| Right stick (click) | I J K L (H) | Right stick |
| D-pad | Arrow keys | D-pad |

## Building from source

Windows only for now.

1. Clone the repository, then get the toolchain (Python 3.13 with zstandard, lz4 and capstone; GCC 16.2 from WinLibs;
   glslang 16.6), downloaded from their official sources with their SHA-256 hashes checked, into `recomp\outils`:
   `python recomp\launcher\outils.py`. MSYS2 UCRT64 with the same tools works too.
2. Build the launcher: `python recomp\launcher\construire.py` (it writes `lanceur.exe` at the root of the repository,
   next to `recomp`, as in the release package).
3. Run `lanceur.exe` and build the game from it, or from a shell:
   `python -u recomp\launcher\preparer.py --copie "<your copy>.nup"`.
4. To make release archives: `python recomp\launcher\paquet.py <output folder>`.

The developer notes (in French), with everything learned about the game, NVN and the Switch 2 along the way, and the
debugging switches (`RECOMP_*` environment variables), are in [docs/DEV-NOTES-fr.md](docs/DEV-NOTES-fr.md).

## Respect copyright

- Star Fox is © Nintendo. This is a fan project. It is not affiliated with, endorsed by or sponsored by Nintendo.
  Nintendo Switch and Star Fox are trademarks of Nintendo.
- This repository contains no game files, no game code (also not the C++ code the recompiler generates from it), no
  assets, no keys, and release builds won't either.
- The game code you generate and the build made from it contain Nintendo's code. They are for your own use only:
  don't share them, upload them or ship them.
- Issues, pull requests or discussions that contain game files, keys, generated code or links to pirated copies will be
  removed. Piracy is not supported in any way.
- Follow the copyright laws of your country. Dumping a game you own for personal use is legal in some countries and not
  in others; that is your responsibility.

## Credits

- **SerenityG4K**: creator and maintainer of SpaceAnimalsRecomp: direction, testing and review. Also the author of the
  Star Fox Command native port, whose launcher inspired this one.
- **Claude** (Anthropic), through Claude Code: code, tooling, reverse engineering and documentation, under SerenityG4K's
  direction.
- The static recompilation approach of [N64Recomp](https://github.com/N64Recomp/N64Recomp) and
  [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), which showed the way (no code from them is used).
- The Switch homebrew community's documentation ([Switchbrew](https://switchbrew.org)).
- Libraries and tools: [Capstone](https://www.capstone-engine.org) (BSD), [python-zstandard](https://github.com/indygreg/python-zstandard)
  (BSD), [python-lz4](https://github.com/python-lz4/python-lz4) (BSD), [glslang](https://github.com/KhronosGroup/glslang)
  (BSD-3-Clause and Apache 2.0), the Vulkan headers (Apache 2.0), [GCC](https://gcc.gnu.org) through
  [WinLibs](https://winlibs.com) (GPL 3 with the GCC Runtime Library Exception) and [Python](https://www.python.org) (PSF License).

## License

SpaceAnimalsRecomp's own code is free software under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)),
© 2026 SerenityG4K. You may use, study, share and modify it. Anything you distribute that is based on it must stay under the
same license, with its source code, and keep the copyright notices and credit to SpaceAnimalsRecomp and SerenityG4K.
Third-party components in `recomp/runtime/third_party/` keep their own licenses. The license covers this project's
code only: it grants nothing for Nintendo's game, which you must supply yourself.

The full Windows package (`-windows.zip`) also bundles, in its `outils` folder, the unmodified binaries of the build
tools, under their own licenses. GCC and glslang are reduced to the files the build uses.

- GCC 16.2.0, GNU Binutils 2.47 and MinGW-w64 14.0.0, from the
  [WinLibs build](https://github.com/brechtsanders/winlibs_mingw/releases/tag/16.2.0posix-23.1.3-14.0.0-ucrt-r3)
  (GPL 3, with the GCC Runtime Library Exception for the runtime libraries; MinGW-w64 under its own permissive licenses),
  with the libraries the compiler uses: GMP, MPFR, MPC (LGPL 3), isl (MIT), libiconv, libintl (LGPL 2.1), zstd (BSD)
  and zlib;
- glslang 16.6.0 (BSD-3-Clause, Apache 2.0 and other free licenses);
- Python 3.13.17 embeddable (PSF License), with zstandard 0.25.0, lz4 4.4.5 and capstone 5.0.7 (BSD).

[`recomp/licences/THIRD_PARTY_NOTICES.txt`](recomp/licences/THIRD_PARTY_NOTICES.txt) lists every component with its exact
version, its license and where to get its source code; the full license texts are next to it, in
[`recomp/licences/`](recomp/licences/). Both are also at the root of every release package (`THIRD_PARTY_NOTICES.txt`
and the `LICENCES` folder).
