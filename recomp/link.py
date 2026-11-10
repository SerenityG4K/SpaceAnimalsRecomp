#!/usr/bin/env python3
"""Édition de liens de l'exécutable natif (Phase 2).

  python link.py [--modules main,sdk,...] [-O 2] [--out build/full/recomp_main.exe]

Objets des modules : build/full/<module>/objs.txt (produits par fullbuild.py) ; runtime et couche
hôte (runtime/*.cpp, runtime/os/*.cpp) compilés ici. Un module absent de la liste reste chargé
en mémoire (manifest) mais ses fonctions ne sont pas appelables : utile pour tester le
démarrage avant la fin du build de main.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import build  # noqa: E402

FULL = os.path.join(HERE, "build", "full")
ALL = ["main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "sdk"]


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    p = argparse.ArgumentParser()
    p.add_argument("--modules", default=",".join(ALL))
    p.add_argument("-O", default="2")
    p.add_argument("--out", default=os.path.join(FULL, "recomp_main.exe"))
    a = p.parse_args()
    t0 = time.time()
    objs = []
    for m in a.modules.split(","):
        lst = os.path.join(FULL, m, "objs.txt")
        if not os.path.exists(lst):
            print(f"module {m} non construit (pas de {lst}) : ignoré")
            continue
        objs += [l.strip() for l in open(lst) if l.strip()]
    rt = [os.path.join(build.RUNTIME, "runtime.cpp"), os.path.join(build.RUNTIME, "fp.cpp")]
    osdir = os.path.join(build.RUNTIME, "os")
    rt += sorted(os.path.join(osdir, f) for f in os.listdir(osdir) if f.endswith(".cpp"))
    rtobj = os.path.join(FULL, "rt")
    os.makedirs(rtobj, exist_ok=True)
    flags = build.BASE_FLAGS + ["-O" + a.O, "-g1", "-Wall", "-Wno-unused-function",
                                "-I" + os.path.join(build.RUNTIME, "third_party"),   # en-têtes Vulkan
                                # API de Windows 10 : MSYS2 les expose par défaut, WinLibs (outils/ du paquet) vise Windows 7
                                "-D_WIN32_WINNT=0x0A00", "-DWINVER=0x0A00",
                                # GCC suppose la pile alignée sur 32 octets pour l'AVX, Windows ne l'aligne que sur 16 :
                                # les écritures vectorielles alignées (vmovdqa) deviennent non alignées (même vitesse)
                                "-Wa,-muse-unaligned-vector-move"]
    errs = []
    for s in rt:
        o = os.path.join(rtobj, os.path.splitext(os.path.basename(s))[0] + ".o")
        _, err = build.compile_one("g++", s, o, flags)
        if err:
            errs.append((s, err))
        elif _ and os.path.exists(o):
            pass
        objs.append(o)
    if errs:
        for s, e in errs:
            print(f"--- {s}\n{e[:6000]}")
        sys.exit(1)
    # icône du jeu (build/full/icone.ico, tirée de la copie du joueur par launcher/preparer.py) : ressource 1
    # (windres lancé dans rt/ avec des chemins relatifs : rien ne dépend des espaces du dossier du jeu)
    if os.path.exists(os.path.join(FULL, "icone.ico")):
        with open(os.path.join(rtobj, "icone.rc"), "w", encoding="utf-8") as fp:
            fp.write('1 ICON "../icone.ico"\n')
        try:
            r = subprocess.run(["windres", "icone.rc", "-O", "coff", "-o", "icone_res.o"], cwd=rtobj, capture_output=True,
                               text=True)
            erreur = (r.stdout + r.stderr).strip() if r.returncode else None
        except OSError as e:   # windres absent : exécutable sans icône
            erreur = str(e)
        if erreur is None:
            objs.append(os.path.join(rtobj, "icone_res.o"))
        else:
            print("icône ignorée (windres) : " + erreur[:300])
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    rsp = os.path.join(FULL, "link.rsp")
    with open(rsp, "w", encoding="utf-8") as fp:
        for o in objs:
            fp.write('"' + o.replace("\\", "/") + '"\n')
    # pas d'ASLR « haute entropie » : sinon Windows peut placer tas/piles dans la zone invitée
    # fixe (0x70_0000_0000-0x80_0000_0000) que le code recompilé suppose libre
    cmd = ["g++", "-o", a.out, "@" + rsp, "-static", "-static-libgcc", "-static-libstdc++",
           "-latomic", "-lwinmm", "-lgdi32", "-luser32", "-lole32",
           "-lmfplat", "-lmfreadwrite", "-lmfuuid",   # extraits vidéo (record.cpp)
           "-Wl,--disable-high-entropy-va"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:8000])
        sys.exit(1)
    # le manifeste accompagne l'exécutable
    import shutil
    if os.path.dirname(os.path.abspath(a.out)) != FULL:
        shutil.copy(os.path.join(FULL, "manifest.txt"), os.path.dirname(os.path.abspath(a.out)))
    print(f"OK {a.out} ({os.path.getsize(a.out) / 1e6:.0f} Mo, {len(objs)} objets) [{time.time() - t0:.1f} s]")


if __name__ == "__main__":
    main()
