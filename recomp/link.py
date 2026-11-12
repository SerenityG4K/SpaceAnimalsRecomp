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
            print(f"module {m} not built (no {lst}): ignored")
            continue
        # chemins absolus (fullbuild.py) : après un déplacement du dossier d'installation (issue GitHub n° 4), les
        # objets sont repris à côté de leur liste
        for l in open(lst):
            o = l.strip()
            if not o:
                continue
            if not os.path.exists(o):
                ici = os.path.join(FULL, m, os.path.basename(o.replace("\\", "/")))
                if os.path.exists(ici):
                    o = ici
            objs.append(o)
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
    # Ressources de l'exécutable (windres lancé dans rt/ avec des chemins relatifs : rien ne dépend des espaces du
    # dossier du jeu) :
    #  - icône du jeu (build/full/icone.ico, tirée de la copie du joueur par launcher/preparer.py) : ressource 1 ;
    #  - informations de version : Windows (gestionnaire des tâches, mélangeur de volume, propriétés du fichier) montre
    #    la description du fichier au lieu de « recomp_main » (demande de l'utilisateur, 2026-11-11). Constantes écrites
    #    en chiffres : aucun en-tête de Windows n'est nécessaire.
    try:
        version = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "VERSION"), encoding="utf-8").read().strip()
    except OSError:
        version = "0.0.0"
    chiffres = ([int(x) for x in version.split(".") if x.isdigit()] + [0, 0, 0, 0])[:4]
    rc = ""
    if os.path.exists(os.path.join(FULL, "icone.ico")):
        rc += '1 ICON "../icone.ico"\n'
    rc += ("1 VERSIONINFO\nFILEVERSION {v}\nPRODUCTVERSION {v}\nFILEOS 0x40004\nFILETYPE 0x1\nBEGIN\n"
           '    BLOCK "StringFileInfo"\n    BEGIN\n        BLOCK "040904B0"\n        BEGIN\n'
           '            VALUE "FileDescription", "StarFox Remake"\n'
           '            VALUE "ProductName", "StarFox Remake [recompiled]"\n'
           '            VALUE "FileVersion", "{t}"\n            VALUE "ProductVersion", "{t}"\n'
           '            VALUE "InternalName", "recomp_main"\n            VALUE "OriginalFilename", "recomp_main.exe"\n'
           '        END\n    END\n'
           '    BLOCK "VarFileInfo"\n    BEGIN\n        VALUE "Translation", 0x0409, 1200\n    END\nEND\n'
           ).format(v=",".join(str(c) for c in chiffres), t=version)
    if rc:
        with open(os.path.join(rtobj, "icone.rc"), "w", encoding="utf-8") as fp:
            fp.write(rc)
        try:
            r = subprocess.run(["windres", "icone.rc", "-O", "coff", "-o", "icone_res.o"], cwd=rtobj, capture_output=True,
                               text=True)
            erreur = (r.stdout + r.stderr).strip() if r.returncode else None
        except OSError as e:   # windres absent : exécutable sans icône ni description
            erreur = str(e)
        if erreur is None:
            objs.append(os.path.join(rtobj, "icone_res.o"))
        else:
            print("icon and file description ignored (windres): " + erreur[:300])
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    rsp = os.path.join(FULL, "link.rsp")
    with open(rsp, "w", encoding="utf-8") as fp:
        for o in objs:
            fp.write('"' + o.replace("\\", "/") + '"\n')
    # pas d'ASLR « haute entropie » : sinon Windows peut placer tas/piles dans la zone invitée
    # fixe (0x70_0000_0000-0x80_0000_0000) que le code recompilé suppose libre
    # L'exécutable est écrit à côté, puis mis en place d'un seul coup : une reconstruction annulée ou en échec pendant
    # l'édition de liens laisse le jeu précédent intact. Depuis la 1.0.3 la reconstruction après une mise à jour est un
    # choix du joueur et le bouton JOUER reste actif : il ne doit jamais tomber sur un exécutable à moitié écrit.
    part = (a.out[:-4] + ".part.exe") if a.out.lower().endswith(".exe") else a.out + ".part"
    if os.path.exists(part):   # reste d'une édition de liens interrompue
        os.remove(part)
    cmd = ["g++", "-o", part, "@" + rsp, "-static", "-static-libgcc", "-static-libstdc++",
           "-latomic", "-lwinmm", "-lgdi32", "-luser32", "-lole32",
           "-lmfplat", "-lmfreadwrite", "-lmfuuid",   # extraits vidéo (record.cpp)
           "-Wl,--disable-high-entropy-va"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[:8000])
        if os.path.exists(part):
            os.remove(part)
        sys.exit(1)
    os.replace(part, a.out)
    # le manifeste accompagne l'exécutable
    import shutil
    if os.path.dirname(os.path.abspath(a.out)) != FULL:
        shutil.copy(os.path.join(FULL, "manifest.txt"), os.path.dirname(os.path.abspath(a.out)))
    print(f"OK {a.out} ({os.path.getsize(a.out) / 1e6:.0f} MB, {len(objs)} objects) [{time.time() - t0:.1f} s]")


if __name__ == "__main__":
    main()
