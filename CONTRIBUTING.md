# Contributing

Thank you for helping SpaceAnimalsRecomp. A few rules keep the project healthy and legal.

- **Never** attach or link game files, keys, firmware, dumps, the C++ code the recompiler generates, shader binaries,
  captures of copyrighted assets beyond a small screenshot of a bug, or anything pirated. Such issues and pull requests
  are removed.
- **Bugs**: use the latest release, search the existing issues, and follow "Before you report a bug" in the README (logs,
  where in the game, save if possible).
- **Pull requests**: open an issue first for anything big, so work isn't done twice. Keep one topic per pull request,
  describe how you tested it (which part of the game, which GPU), and keep the existing style: the code comments and the
  developer notes are in French, the user-facing docs in English.
- **Check before you submit**: `python recomp/verif/verifier.py rapide` (about 5 minutes, needs a built game). It
  compiles the runtime and the launcher, checks the versions, the package and its licenses, the update from the last
  release, a simulated GPU driver crash and a moved install. Paste its summary in your pull request. Every release is
  also checked with `verifier.py complet`: a new install built from scratch in a folder with spaces, without MSYS2,
  then the game, the game without Vulkan pipeline libraries, and the launcher.
- The launcher's French and English texts are in `recomp/launcher/textes.cpp`.
- Developer notes, debugging switches and what was learned about the game: `docs/DEV-NOTES-fr.md`.

By contributing, you agree that your contribution is licensed under the project's license (GPL-3.0) and that
SerenityG4K, the maintainer, may include it in SpaceAnimalsRecomp.
