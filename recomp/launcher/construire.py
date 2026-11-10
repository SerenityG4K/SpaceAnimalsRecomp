#!/usr/bin/env python3
"""Compile le launcher : lanceur.exe à la racine du projet, à côté de recomp/, comme dans le paquet (Win32, Direct2D,
DirectWrite, Media Foundation ; aucune DLL à fournir à côté).

  python construire.py [--out lanceur_new.exe]   # g++ et windres de MSYS2 (ucrt64) ou du PATH
"""
import os
import shutil
import subprocess
import sys

ICI = os.path.dirname(os.path.abspath(__file__))
SOURCES = ["lanceur.cpp", "dessin.cpp", "video.cpp", "textes.cpp"]
LIBS = ["-ld3d11", "-ldxgi", "-ld2d1", "-ldwrite", "-lwindowscodecs", "-lmfplat", "-lmfreadwrite", "-lmfuuid", "-lole32",
        "-loleaut32", "-luuid", "-lshell32", "-lshlwapi", "-ldwmapi", "-lwinmm"]


def outil(nom):
    sys.path.insert(0, ICI)
    from preparer import gcc_sans_espaces   # copie hors des chemins à espaces si besoin (voir preparer.py)
    for o in (os.path.join(ICI, "..", "..", "outils"), os.path.join(ICI, "..", "outils")):
        p = os.path.join(o, "gcc", "bin", nom + ".exe")
        if os.path.exists(p):
            return os.path.join(gcc_sans_espaces(os.path.abspath(o)), nom + ".exe")
    p = os.path.join(r"C:\msys64\ucrt64\bin", nom + ".exe")
    if os.path.exists(p):
        return p
    return shutil.which(nom) or nom


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    # à la racine du projet, comme dans le paquet (recomp/ à côté) ; autre nom avec --out si lanceur.exe tourne.
    # Chemin relatif au dossier du launcher : sans espaces, quel que soit le dossier du projet.
    sortie = sys.argv[sys.argv.index("--out") + 1] if "--out" in sys.argv else os.path.join("..", "..", "lanceur.exe")
    os.makedirs(os.path.join(ICI, "build"), exist_ok=True)
    if not os.path.exists(os.path.join(ICI, "icone.ico")):
        subprocess.run([sys.executable, "-I", os.path.join(ICI, "icone.py")], check=True)
    env = dict(os.environ)
    gxx = outil("g++")
    env["PATH"] = os.path.dirname(gxx) + os.pathsep + env.get("PATH", "")
    res = os.path.join(ICI, "build", "lanceur_res.o")
    r = subprocess.run([outil("windres"), "lanceur.rc", "-O", "coff", "-o", res], cwd=ICI, env=env, capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    # GCC lie d'office default-manifest.o (manifeste minimal). Celui de MSYS2 est en langue neutre et ld l'écarte devant
    # le nôtre ; celui de WinLibs (outils/) est en anglais (1033) : ld garde alors les deux manifestes n° 1 (« .rsrc
    # merge failure: multiple non-default manifests »). Un default-manifest.o vide, trouvé d'abord grâce à -B, l'évite.
    # Chemin relatif : sans espaces, quel que soit le dossier du launcher.
    sans_manifeste = os.path.join("build", "sans_manifeste")
    os.makedirs(os.path.join(ICI, sans_manifeste), exist_ok=True)
    vide = os.path.join(sans_manifeste, "vide.c")
    open(os.path.join(ICI, vide), "w").close()
    r = subprocess.run([gxx, "-c", "-x", "c", vide, "-o", os.path.join(sans_manifeste, "default-manifest.o")], cwd=ICI, env=env,
                       capture_output=True, text=True)
    if r.returncode:
        print(r.stdout + r.stderr)
        return 1
    cmd = [gxx, "-std=c++20", "-O2", "-municode", "-mwindows", "-Wall", "-Wno-unknown-pragmas", "-static", "-static-libgcc",
           "-static-libstdc++", "-B" + sans_manifeste + "/", *SOURCES, res, "-o", sortie, *LIBS]
    r = subprocess.run(cmd, cwd=ICI, env=env, capture_output=True, text=True)
    messages = (r.stdout + r.stderr).strip()
    if messages:
        print(messages[:12000])
    if r.returncode:
        return 1
    fini = os.path.abspath(os.path.join(ICI, sortie))
    print(f"OK {fini} ({os.path.getsize(fini) / 1e6:.1f} Mo)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
