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
- Click **BUG REPORT** in the launcher and attach the `SpaceAnimalsRecomp-bug-report.zip` it makes: it has your
  hardware and all the logs, without your Windows user name and without anything from the game.
- No launcher window (or no button)? Attach the logs by hand, all in `recomp\build\full\` next to `lanceur.exe`:
  - `recomp_run.log` for a game bug (the game writes a new one at each start, so take it right after the bug;
    the launcher keeps the previous one as `recomp_run_precedent.log`);
  - `preparation.log` for a build problem;
  - `precompile.log` for a shader compilation problem;
  - `lanceur.log`, next to `lanceur.exe`, for a launcher problem.
- Crash or graphics bug? Tell us where in the game it happens, and attach your save if you can (close the game and
  zip `%APPDATA%\SpaceAnimalsRecomp\save`).

## Getting started (1.0)

You need:

- Windows 10 or 11, 64-bit;
- a CPU with AVX2 (2013 or newer);
- a GPU with a Vulkan 1.3 driver (tested on NVIDIA; AMD support improved in 1.0.3; Intel is untested);
- about 6 GB of free disk space during the build;
- **memory: 32 GB of RAM recommended.** Since 1.0.3 the game uses much less memory; with 16 GB, close other programs
  (another game, the browser…) before playing. The launcher warns you when less than 10 GB is free. 16 GB players:
  please tell us how it goes in [#5](https://github.com/SerenityG4K/SpaceAnimalsRecomp/issues/5).

The only configuration tested so far is a powerful one: AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5070 Ti. The project is
experimental and not optimized yet, so a less powerful PC may not reach a steady 60 fps.

**About performance**: on Switch 2, the game needs DLSS to run smoothly: it draws at a lower resolution and DLSS
upscales the picture. By default SpaceAnimalsRecomp draws everything at native resolution: the picture is sharper, but
it needs a lot more power from your graphics card. Since 1.1.0, **GRAPHICS** in the launcher has a lower 3D rendering
scale, AMD FSR (all cards) and NVIDIA DLSS (RTX cards), all experimental. They help when your graphics card is what
holds the game back.

**Handheld PCs:** not tested yet on handhelds such as the Steam Deck, Lenovo Legion Go or ASUS ROG Ally. On the Steam
Deck (SteamOS), the Windows build would have to run through Proton. Feedback is welcome in the
[Issues](https://github.com/SerenityG4K/SpaceAnimalsRecomp/issues).

1. Download `SpaceAnimalsRecomp-1.1.0-windows.zip` from [Releases](https://github.com/SerenityG4K/SpaceAnimalsRecomp/releases).
   It bundles everything the build and the game need (Python, Clang, GCC, glslang, SDL3, and the FSR and DLSS
   libraries): nothing else to install.
2. Extract it to a folder you can write to, **not** Program Files, whose path has only Latin characters, for example
   `C:\Games\StarFox`. Spaces are fine: the compiler is then copied once to `C:\ProgramData\StarFoxRecomp` (about 400 MB).
3. Run `lanceur.exe`. Choose your copy (**COPY › choose**: your `.nup` file), then click **BUILD THE GAME**.
   - The launcher checks that it is Star Fox 2.0.0 by the SHA-256 hashes in the file's index, without reading all 30 GB.
   - It extracts the executables, recompiles the whole game code (about 20 to 30 minutes, the launcher stays usable),
     links `recomp_main.exe` and translates the game's shaders.
   - It then compiles every shader for your GPU, like recent PC games do on their first launch (about a minute), so
     there is no shader compilation stutter in game. Nothing opens by itself during the build.
4. Click **PLAY**. Under the button, choose **FULL SCREEN** or **WINDOWED** (Alt+Enter or F11 also switch in game).
   **SETTINGS** has the game language (15 choices), resolution (up to 4K), frame rate, volume, update checks, the
   game's location (**Move…**) and a desktop shortcut. **GRAPHICS** has the graphics card, the 3D rendering scale and
   upscaling (FSR, or DLSS on NVIDIA RTX).

Your copy is never modified (read-only). The game's own icon is taken from your copy at build time and shown on
`recomp_main.exe` (named "StarFox Remake" in the Task Manager); it is never distributed. The launcher's background clips, gameplay
recorded by SerenityG4K, come with the package.

### Updates

The launcher checks this repository's releases when it starts (you can turn it off in **SETTINGS**). When a new version
is out, it asks you first; if you say yes, it downloads it, checks its SHA-256 hash, installs it and restarts. It then
asks if you want to rebuild the game: some improvements require rebuilding the game. It only redoes the parts whose
code changed. You can say no and keep playing; **REBUILD NOW ›** on the launcher's home page does it later. Your copy,
your saves and your settings stay. Since 1.1.0, the update also downloads the FSR and DLSS libraries from AMD's and
NVIDIA's official repositories, with their SHA-256 hashes checked.

### Shader precompilation

Every one of the game's 2,388 translated shaders is compiled for your GPU once, on the first launch, and again only after a GPU
driver update. SpaceAnimalsRecomp uses Vulkan graphics pipeline libraries (`VK_EXT_graphics_pipeline_library`): each
shader is compiled on its own, ahead of time, and a draw only links precompiled pieces, which takes a fraction of a
millisecond. The result is kept in `recomp\build\full\pipelines.cache`. Deleting it only means compiling again on the next
launch. On a GPU without pipeline libraries, shaders are compiled the first time they are drawn, then kept in the same
cache.

2,308 of them come from the shader archive of your copy. The other 80 (72 of them compute shaders) are created by the
game when it starts, from elsewhere in your copy: the build runs the game for a few seconds in the background, without a window, to collect them, then
translates and compiles them too.

## Where are my saves?

In `%APPDATA%\SpaceAnimalsRecomp\save` (since 1.0.3; saves from an older version are copied there the first time).
They stay if you reinstall or move SpaceAnimalsRecomp. A backup is made before every build and update; to make your
own, close the game and copy the whole `save` folder.

## Uninstalling

Click **UNINSTALL** on the launcher's build page, or run `uninstall.bat`. It removes everything SpaceAnimalsRecomp put on
your PC (the install folder, the build, the compiler copy in `C:\ProgramData\StarFoxRecomp`, temporary files and
logs). It never touches your copy of the game, and keeps your saves unless you ask it to remove them.

## What you need

- Your own Star Fox for Nintendo Switch 2 (cartridge or digital) with its **2.0.0 update** (v131072), dumped from your own
  console as a single `.nup` file (title ID `0400E88025B1A000`). The launcher tells you if the file isn't the right
  game or version.
- Windows 10 or 11 on x86-64 with AVX2, a GPU with Vulkan 1.3, and 32 GB of RAM (recommended).

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
| ✅ | Updates | 100% | The launcher asks before installing; only the parts that changed are rebuilt |
| 🔧 | Frame rate | 90% | A steady 60 fps; 90 and 120 fps are not stable yet (55 to 118 fps in Corneria at 120 for now) |
| 🔧 | Horizon OS services | 90% | Threads, files, controllers, applets, time and audio: everything the game has used so far |
| 🔧 | Graphics (NVN on Vulkan) | 90% | The whole game draws correctly on NVIDIA; AMD support improved in 1.0.3; Intel is untested |
| 🔧 | Controls | 85% | Keyboard and controllers (Xbox, PlayStation, Switch Pro, Joy-Con), all remappable in the launcher; no mouse yet |
| 🔧 | Tested in game | 90% | The whole game has been finished several times, and it is rather stable: report what breaks |
| 🔧 | NVIDIA DLSS | 70% | Experimental since 1.1.0, RTX cards only: DLAA or upscaling, DLSS 4 or 4.5 model |
| 🔧 | AMD FSR | 70% | Experimental since 1.1.0, all cards: FSR 3.1, or FSR 4 on some Radeon cards (FSR 4 untested so far) |
| 🗺️ | Frame generation | 0% | Planned: generated frames between the ones the game draws, for a higher frame rate on screen |
| 🗺️ | Mod support | 0% | Planned: loading mods, managed from the launcher |
| 🗺️ | Online play | 0% | Planned: the game's online modes |
| 🗺️ | Mouse controls | 0% | Planned: the game's mouse mode, with the PC mouse |
| 🗺️ | amiibo | 0% | Planned: the game's amiibo features |
| 🗺️ | Several controllers at once | 0% | Planned |
| 🗺️ | Ultrawide (21:9, 32:9) | 0% | Possibly feasible, not started (asked for in #2) |
| 🗺️ | 5.1 surround sound | 0% | Planned, as an experimental option: the game already mixes in 5.1 (asked for in #28) |
| 🗺️ | Linux | 0% | Planned |

✅ done · 🔧 in progress · 🗺️ roadmap

## Controls

Xbox, PlayStation (DualShock 4, DualSense), Switch Pro and Joy-Con controllers work, USB or Bluetooth (since 1.1.0;
Switch 2 controllers are not supported yet). Controller buttons follow the position on the pad (the Switch's A is the
Xbox B, and so on). The keys and buttons below are the default ones. You can change them on the launcher's
**CONTROLS** page:

- **KEYBOARD**: pick **WASD (QWERTY)** or **ZQSD (AZERTY)**, or click a key's box and press the key you want;
- **CONTROLLER**: click a button, then press the new one or click it on the drawing of your controller.

Changes apply the next time you start the game.

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
  [WinLibs](https://winlibs.com) (GPL 3 with the GCC Runtime Library Exception), [Python](https://www.python.org) (PSF License),
  [SDL](https://www.libsdl.org) (zlib), the [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)
  (MIT) and [NVIDIA DLSS](https://github.com/NVIDIA/DLSS) (NVIDIA's license). This software uses NVIDIA DLSS.

## License

SpaceAnimalsRecomp's own code is free software under the **GNU General Public License v3.0** (see [LICENSE](LICENSE)),
© 2026 SerenityG4K. You may use, study, share and modify it. Anything you distribute that is based on it must stay under the
same license, with its source code, and keep the copyright notices and credit to SpaceAnimalsRecomp and SerenityG4K.
Third-party components in `recomp/runtime/third_party/` keep their own licenses. The license covers this project's
code only: it grants nothing for Nintendo's game, which you must supply yourself.

**Additional permission (GPL-3.0 section 7) for NVIDIA DLSS.** If you modify SpaceAnimalsRecomp, or any covered work, by
linking or combining it with NVIDIA DLSS (`nvngx_dlss.dll`, from the NVIDIA DLSS SDK), containing parts covered by the
terms of NVIDIA's DLSS SDK license, the licensor of SpaceAnimalsRecomp grants you additional permission to convey the
resulting work. NVIDIA DLSS is a separate work: the GPL does not apply to it, and it stays under NVIDIA's license. The
AMD FSR libraries are under the MIT license, which is compatible with the GPL.

The full Windows package (`-windows.zip`) also bundles, in its `outils` folder, the unmodified binaries of the build
tools and of the libraries the game loads, under their own licenses. GCC and glslang are reduced to the files the build
uses.

- GCC 16.2.0, GNU Binutils 2.47 and MinGW-w64 14.0.0, from the
  [WinLibs build](https://github.com/brechtsanders/winlibs_mingw/releases/tag/16.2.0posix-23.1.3-14.0.0-ucrt-r3)
  (GPL 3, with the GCC Runtime Library Exception for the runtime libraries; MinGW-w64 under its own permissive licenses),
  with the libraries the compiler uses: GMP, MPFR, MPC (LGPL 3), isl (MIT), libiconv, libintl (LGPL 2.1), zstd (BSD)
  and zlib;
- glslang 16.6.0 (BSD-3-Clause, Apache 2.0 and other free licenses);
- Python 3.13.17 embeddable (PSF License), with zstandard 0.25.0, lz4 4.4.5 and capstone 5.0.7 (BSD);
- SDL 3.4.20 (zlib), for controllers;
- AMD FSR libraries from the FidelityFX SDK 2.4.0 (MIT);
- NVIDIA DLSS (`nvngx_dlss.dll`, DLSS SDK 310.9.16). It is **not** free software and **not** under the GPL: it stays
  under NVIDIA's license (`NVIDIA-DLSS-SDK-LICENSE.txt`), may only be used with this application, and may not be
  modified or redistributed on its own. This software uses NVIDIA DLSS. NVIDIA, GeForce RTX and DLSS are trademarks
  of NVIDIA Corporation.

[`recomp/licences/THIRD_PARTY_NOTICES.txt`](recomp/licences/THIRD_PARTY_NOTICES.txt) lists every component with its exact
version, its license and where to get its source code; the full license texts are next to it, in
[`recomp/licences/`](recomp/licences/). Both are also at the root of every release package (`THIRD_PARTY_NOTICES.txt`
and the `LICENCES` folder).
