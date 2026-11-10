#!/usr/bin/env python3
"""Assemble le dépôt public SpaceAnimalsRecomp (github.com/SerenityG4K/SpaceAnimalsRecomp) dans un dossier : les sources,
les notes de développement et les fichiers de la racine (recomp/depot/). Rien du jeu, rien de personnel.

  python depot.py <dossier>      dossier vide, ou clone du dépôt (ses fichiers suivis sont remplacés, .git gardé)

Ensuite, dans ce dossier : git add -A, git commit, git push ; puis la release (paquet.py, gh release create).
"""
import os
import re
import shutil
import sys

LANCEUR = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(LANCEUR)
PROJET = os.path.dirname(RECOMP)
DEPOT = os.path.join(RECOMP, "depot")

SOURCES = {   # dossier (relatif à recomp/) → extensions
    "a64": (".py",),
    "runtime": (".cpp", ".h", ".hpp", ".c", ".inl"),
    "shaders": (".py",),
    "bench": (".py",),
    "licences": (".txt",),   # avis et licences des outils fournis dans le paquet (THIRD_PARTY_NOTICES.txt)
}
LANCEUR_FICHIERS = (".cpp", ".h", ".rc", ".manifest", ".py")
INTERDITS = (".nup", ".bin", ".o", ".spv", ".mp4", ".log", ".save", ".ini", ".cache", ".png", ".jpg", ".jpeg", ".exe", ".dll",
             ".zip", ".wav")
# (motifs écrits en morceaux : ce fichier, exporté lui aussi, ne doit pas se signaler lui-même)
PERSONNEL = re.compile("C:" + r"[\\/]+" + "Us" + "ers[\\\\/]|" + "mag" + "di|" + "@gm" + "ail|" + "Desk" + r"top[\\/]+Projet", re.I)


def copier(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):   # export précédent : remplacé (l'historique git reste)
        if f != ".git":
            p = os.path.join(out, f)
            shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
    for f in ("README.md", "LICENSE", "CHANGELOG.md", "CONTRIBUTING.md"):
        copier(os.path.join(DEPOT, f), os.path.join(out, f))
    copier(os.path.join(DEPOT, "gitignore.txt"), os.path.join(out, ".gitignore"))
    copier(os.path.join(RECOMP, "README.md"), os.path.join(out, "docs", "DEV-NOTES-fr.md"))
    r = os.path.join(out, "recomp")
    for f in ("recomp.py", "build.py", "fullbuild.py", "link.py", "hooks.txt", "VERSION"):
        copier(os.path.join(RECOMP, f), os.path.join(r, f))
    for d, ext in SOURCES.items():
        for racine, dossiers, fichiers in os.walk(os.path.join(RECOMP, d)):
            dossiers[:] = [x for x in dossiers if x not in ("__pycache__", "results")]
            for f in fichiers:
                if f.endswith(ext):
                    p = os.path.join(racine, f)
                    copier(p, os.path.join(r, os.path.relpath(p, RECOMP)))
    for f in os.listdir(LANCEUR):
        if f.endswith(LANCEUR_FICHIERS):
            copier(os.path.join(LANCEUR, f), os.path.join(r, "launcher", f))
    copier(os.path.join(LANCEUR, "icone.ico"), os.path.join(r, "launcher", "icone.ico"))   # icône du launcher (icone.py)
    for f in ("nup.py", "nso.py"):
        copier(os.path.join(PROJET, "tools", f), os.path.join(out, "tools", f))
    # contrôles : aucun fichier du jeu ni produit, aucune trace personnelle
    n = 0
    for racine, dossiers, fichiers in os.walk(out):
        dossiers[:] = [x for x in dossiers if x != ".git"]
        for f in fichiers:
            p = os.path.join(racine, f)
            n += 1
            if f.lower().endswith(INTERDITS):
                sys.exit(f"fichier interdit dans le dépôt : {p}")
            if f.endswith((".py", ".cpp", ".h", ".md", ".txt", ".rc", ".manifest", ".inl", ".c", ".hpp")) or f.startswith("."):
                texte = open(p, encoding="utf-8", errors="replace").read()
                m = PERSONNEL.search(texte)
                if m:
                    sys.exit(f"trace personnelle « {m.group(0)} » dans {p}")
    taille = sum(os.path.getsize(os.path.join(rt, f)) for rt, ds, fs in os.walk(out) if ".git" not in rt for f in fs)
    print(f"dépôt : {out} ({n} fichiers, {taille / 1e6:.1f} Mo)")


if __name__ == "__main__":
    main()
