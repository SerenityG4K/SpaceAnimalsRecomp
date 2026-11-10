#!/usr/bin/env python3
"""Télécharge les outils de construction fournis avec le paquet, depuis leurs sources officielles, et vérifie leur
empreinte SHA-256 (publiée par python.org, PyPI et GitHub) : rien d'autre à installer pour le joueur.

  python outils.py [--dest <dossier>] [--garder-archives] [--complet]

GCC et glslang sont réduits aux fichiers que la construction utilise (voir GARDER_GCC), sauf avec --complet.

Résultat (par défaut recomp/outils/) :
  python/     Python 3.13 embarquable, avec zstandard et lz4 (lecture de la copie, extraction des shaders)
  gcc/        GCC 16.2 (WinLibs, UCRT, threads POSIX) : compile le C++ généré, le runtime et le launcher
  glslang/    glslang 16.6 : traduit les shaders GLSL en SPIR-V
  LISEZMOI.txt   versions, sources et licences
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request
import zipfile

LANCEUR = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(LANCEUR)

OUTILS = [
    {"nom": "python", "version": "3.13.17",
     "url": "https://www.python.org/ftp/python/3.13.17/python-3.13.17-embed-amd64.zip",
     "sha256": "9e35319bffedab8b36ad44d8f377cb4f1733bcec257a0b6d04d94f01486ddb38", "dest": "python", "racine": False,
     "licence": "Python Software Foundation License"},
    {"nom": "zstandard", "version": "0.25.0",
     "url": "https://files.pythonhosted.org/packages/d9/82/b9c06c870f3bd8767c201f1edbdf9e8dc34be5b0fbc5682c4f80fe948475/"
            "zstandard-0.25.0-cp313-cp313-win_amd64.whl",
     "sha256": "1f830a0dac88719af0ae43b8b2d6aef487d437036468ef3c2ea59c51f9d55fd5", "dest": "python/Lib/site-packages",
     "racine": False, "licence": "BSD"},
    {"nom": "lz4", "version": "4.4.5",
     "url": "https://files.pythonhosted.org/packages/18/e0/f06028aea741bbecb2a7e9648f4643235279a770c7ffaf70bd4860c73661/"
            "lz4-4.4.5-cp313-cp313-win_amd64.whl",
     "sha256": "da68497f78953017deb20edff0dba95641cc86e7423dfadf7c0264e1ac60dc22", "dest": "python/Lib/site-packages",
     "racine": False, "licence": "BSD"},
    # désassembleur utilisé par le recompilateur (a64/codegen.py) ; version exacte du code validé
    {"nom": "capstone", "version": "5.0.7",
     "url": "https://files.pythonhosted.org/packages/70/39/2138d890a8e827636b9de9924fbc8527fe83e38b6d26605b30ac55e30ebe/"
            "capstone-5.0.7-py3-none-win_amd64.whl",
     "sha256": "4ab8bcb7da8f221ff45926ca168ca33e76f7237d06fbf3c10780002faa2670e1", "dest": "python/Lib/site-packages",
     "racine": False, "licence": "BSD-3-Clause"},
    {"nom": "gcc", "version": "16.2.0 (WinLibs, MinGW-w64 14.0.0 UCRT, r3)",
     "url": "https://github.com/brechtsanders/winlibs_mingw/releases/download/16.2.0posix-23.1.3-14.0.0-ucrt-r3/"
            "winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64ucrt-14.0.0-r3.zip",
     "sha256": "3c69305081e4a647144339a936244fcf7d3d9e558319e1ad7be2cd2886d4b6a1", "dest": "gcc", "racine": True,
     "licence": "GPL v3 avec exception de bibliothèque d'exécution GCC ; MinGW-w64 : licences libres (ZPL, MIT…)"},
    {"nom": "glslang", "version": "16.6.0",
     "url": "https://github.com/KhronosGroup/glslang/releases/download/16.6.0/glslang-16.6.0-windows-x86_64-release.zip",
     "sha256": "82bf434e69b9bb4829de7e2b4bc2c5e7a7861e53d66cf75e5cc70f5f694a8d9b", "dest": "glslang", "racine": False,
     "licence": "BSD-3-Clause, Apache-2.0 et autres licences libres (voir le dépôt KhronosGroup/glslang)"},
]


# ------------------------------------------------------------------------------------------ allègement
# Fichiers de GCC et de glslang que la construction du jeu utilise vraiment. Liste établie par trace (2026-11-10) :
# vraies commandes de build.py, fullbuild.py et link.py avec le GCC fourni seul dans le PATH, en relevant les
# programmes appelés (-v), les en-têtes lus (-MD), les entrées de l'éditeur de liens (-Wl,--trace), la commande de
# windres (qui appelle « gcc -E », donc cc1), puis les DLL importées par ces programmes. Les en-têtes et les
# bibliothèques C/C++ (MinGW-w64, libstdc++, libgcc) sont gardés en entier. Tout le reste de WinLibs (Fortran,
# Objective-C, LTO, gdb et son Python, CMake, Doxygen, Cppcheck, ctags, premake, nasm, yasm, ccache, gcov,
# documentation…) ne sert pas et n'est pas distribué. `--complet` garde tout (développement : gdb…).
_LIBEXEC = "libexec/gcc/x86_64-w64-mingw32/16.2.0/"
_LIBGCC = "lib/gcc/x86_64-w64-mingw32/16.2.0/"
GARDER_GCC = {
    "fichiers": {
        "version_info.txt", "mingwvars.bat",
        # pilotes (windres appelle gcc -E) et DLL qu'ils importent
        "bin/g++.exe", "bin/gcc.exe", "bin/windres.exe",
        "bin/libiconv-2.dll", "bin/libintl-8.dll", "bin/libwinpthread-1.dll", "bin/libzstd.dll", "bin/zlib1.dll",
        # compilateurs, éditeur de liens de GCC, greffon LTO (passé à collect2 par défaut) et leurs DLL
        _LIBEXEC + "cc1.exe", _LIBEXEC + "cc1plus.exe", _LIBEXEC + "collect2.exe", _LIBEXEC + "lto-wrapper.exe",
        _LIBEXEC + "liblto_plugin.dll", _LIBEXEC + "libgcc_s_seh-1.dll", _LIBEXEC + "libgmp-10.dll",
        _LIBEXEC + "libiconv-2.dll", _LIBEXEC + "libisl-23.dll", _LIBEXEC + "libmpc-3.dll", _LIBEXEC + "libmpfr-6.dll",
        _LIBEXEC + "libwinpthread-1.dll", _LIBEXEC + "libzstd.dll", _LIBEXEC + "zlib1.dll",
        # assembleur et éditeur de liens (binutils) et leurs DLL
        "x86_64-w64-mingw32/bin/as.exe", "x86_64-w64-mingw32/bin/ld.exe",
        "x86_64-w64-mingw32/bin/libiconv-2.dll", "x86_64-w64-mingw32/bin/libintl-8.dll",
        "x86_64-w64-mingw32/bin/libwinpthread-1.dll", "x86_64-w64-mingw32/bin/libzstd.dll",
        "x86_64-w64-mingw32/bin/zlib1.dll",
        # objets de démarrage et bibliothèques de GCC
        _LIBGCC + "crtbegin.o", _LIBGCC + "crtend.o", _LIBGCC + "crtfastmath.o", _LIBGCC + "libgcc.a",
        _LIBGCC + "libgcc_eh.a", _LIBGCC + "libgcov.a",
        # bibliothèques C++ et d'exécution de GCC (lib/)
        "lib/libstdc++.a", "lib/libstdc++.dll.a", "lib/libstdc++exp.a", "lib/libstdc++fs.a", "lib/libsupc++.a",
        "lib/libstdc++.modules.json", "lib/libatomic.a", "lib/libatomic.dll.a", "lib/libatomic_asneeded.a",
        "lib/libgcc_s.a", "lib/libssp.a", "lib/libssp.dll.a", "lib/libssp_nonshared.a", "lib/libquadmath.a",
        "lib/libquadmath.dll.a", "lib/libgomp.a", "lib/libgomp.dll.a", "lib/libgomp.spec", "lib/libmangle.a",
    },
    # dossiers gardés en entier : en-têtes C/C++, CRT et bibliothèques d'import de MinGW-w64
    "dossiers": ("include/c++/", "x86_64-w64-mingw32/include/", "x86_64-w64-mingw32/lib/", _LIBGCC + "include/",
                 _LIBGCC + "include-fixed/"),
}
GARDER_GLSLANG = {"fichiers": {"bin/glslang.exe"}, "dossiers": ()}


def garder(outil, rel):
    """Le fichier `rel` (relatif au dossier de l'outil, séparateurs « / ») sert-il à la construction ?"""
    regles = {"gcc": GARDER_GCC, "glslang": GARDER_GLSLANG}.get(outil)
    if regles is None:   # python et ses modules : tout est utile
        return True
    rel = rel.replace("\\", "/")
    return rel in regles["fichiers"] or rel.startswith(regles["dossiers"])


def alleger(dossier, outil):
    """Supprime de l'outil installé dans `dossier` ce que la construction n'utilise pas (voir GARDER_GCC)."""
    n = taille = 0
    for racine, _, fichiers in os.walk(dossier, topdown=False):
        for f in fichiers:
            p = os.path.join(racine, f)
            if not garder(outil, os.path.relpath(p, dossier)):
                taille += os.path.getsize(p)
                os.remove(p)
                n += 1
        if racine != dossier and not os.listdir(racine):
            os.rmdir(racine)
    return n, taille


def telecharger(url, chemin):
    req = urllib.request.Request(url, headers={"User-Agent": "starfox-recomp-outils/1.0"})
    with urllib.request.urlopen(req, timeout=120) as r, open(chemin + ".part", "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        lu, dernier = 0, -1
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            lu += len(b)
            pct = lu * 100 // total if total else 0
            if pct // 10 != dernier:
                dernier = pct // 10
                print(f"    {lu / 1e6:6.1f} Mo" + (f" / {total / 1e6:.1f} Mo" if total else ""), flush=True)
    os.replace(chemin + ".part", chemin)


def empreinte(chemin):
    h = hashlib.sha256()
    with open(chemin, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def extraire(archive, dest, sans_racine):
    """Extraction protégée (aucun chemin absolu ni « .. ») ; `sans_racine` retire le dossier de tête de l'archive."""
    dest = os.path.abspath(dest)
    with zipfile.ZipFile(archive) as z:
        for m in z.infolist():
            nom = m.filename.replace("\\", "/")
            if sans_racine:
                nom = nom.split("/", 1)[1] if "/" in nom else ""
            if not nom:
                continue
            cible = os.path.abspath(os.path.join(dest, nom))
            if not cible.startswith(dest + os.sep) or os.path.isabs(nom) or ".." in nom.split("/"):
                raise RuntimeError(f"chemin refusé dans {os.path.basename(archive)} : {m.filename}")
            if nom.endswith("/"):
                os.makedirs(cible, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(cible), exist_ok=True)
            with z.open(m) as s, open(cible, "wb") as d:
                shutil.copyfileobj(s, d, 1 << 20)


def regler_python(dossier):
    """Python embarquable : active site-packages (fichier ._pth)."""
    for f in os.listdir(dossier):
        if f.endswith("._pth"):
            p = os.path.join(dossier, f)
            lignes = [l.rstrip("\r\n") for l in open(p, encoding="utf-8")]
            lignes = [("import site" if l.strip() == "#import site" else l) for l in lignes]
            if "Lib\\site-packages" not in lignes:
                lignes.insert(len(lignes) - 1 if lignes and lignes[-1].startswith("import site") else len(lignes),
                              "Lib\\site-packages")
            if "import site" not in lignes:
                lignes.append("import site")
            open(p, "w", encoding="utf-8", newline="\r\n").write("\n".join(lignes) + "\n")
    os.makedirs(os.path.join(dossier, "Lib", "site-packages"), exist_ok=True)


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default=os.path.join(RECOMP, "outils"))
    ap.add_argument("--garder-archives", action="store_true")
    ap.add_argument("--complet", action="store_true", help="garder tout GCC et glslang (gdb… : développement)")
    a = ap.parse_args()
    dest = os.path.abspath(a.dest)
    archives = os.path.join(dest, "_archives")
    os.makedirs(archives, exist_ok=True)
    marques = os.path.join(dest, ".installe")
    os.makedirs(marques, exist_ok=True)
    for i, o in enumerate(OUTILS):
        fichier = os.path.join(archives, o["url"].rsplit("/", 1)[1])
        print(f"{o['nom']} {o['version']}", flush=True)
        marque = os.path.join(marques, o["nom"])
        deja = os.path.exists(marque) and open(marque, encoding="utf-8").read().strip() == o["sha256"]
        # Python réinstallé : ses modules aussi (ils sont dans son dossier)
        if deja and not any(p["dest"] == "python" and not os.path.exists(os.path.join(marques, p["nom"])) for p in OUTILS):
            print("  déjà installé", flush=True)
            continue
        if not (os.path.exists(fichier) and empreinte(fichier) == o["sha256"]):
            print(f"  téléchargement : {o['url']}", flush=True)
            telecharger(o["url"], fichier)
        h = empreinte(fichier)
        if h != o["sha256"]:
            os.remove(fichier)
            sys.exit(f"  EMPREINTE DIFFÉRENTE pour {o['nom']} : {h} (attendu {o['sha256']}) : archive supprimée")
        print(f"  SHA-256 vérifiée : {h}", flush=True)
        cible = os.path.join(dest, o["dest"])
        if o["nom"] in ("python", "gcc", "glslang") and os.path.isdir(cible):
            shutil.rmtree(cible)
        os.makedirs(cible, exist_ok=True)
        extraire(fichier, cible, o["racine"])
        if o["nom"] in ("gcc", "glslang") and not a.complet:
            n, taille = alleger(cible, o["nom"])
            print(f"  allégé : {n} fichiers inutiles à la construction retirés ({taille / 1e6:.0f} Mo)", flush=True)
        if o["nom"] == "python":
            regler_python(cible)
            for p in OUTILS:   # les modules seront réinstallés dans ce nouveau Python
                if p["dest"].startswith("python/") and os.path.exists(os.path.join(marques, p["nom"])):
                    os.remove(os.path.join(marques, p["nom"]))
        with open(marque, "w", encoding="utf-8") as f:
            f.write(o["sha256"] + "\n")
        print(f"  installé dans {cible}", flush=True)
    with open(os.path.join(dest, "LISEZMOI.txt"), "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write("Outils de construction fournis avec le launcher (téléchargés depuis leurs sources officielles,\n"
                "empreintes SHA-256 vérifiées par recomp/launcher/outils.py). GCC et glslang sont réduits aux fichiers\n"
                "que la construction utilise ; les fichiers gardés sont inchangés.\n"
                "Licences et liens vers le code source de chaque outil : THIRD_PARTY_NOTICES.txt et le dossier\n"
                "LICENCES, à la racine du paquet.\n\n")
        for o in OUTILS:
            f.write(f"{o['nom']} {o['version']}\n  source : {o['url']}\n  SHA-256 : {o['sha256']}\n  licence : {o['licence']}\n\n")
    if not a.garder_archives:
        shutil.rmtree(archives, ignore_errors=True)
    print("outils prêts", flush=True)


if __name__ == "__main__":
    main()
