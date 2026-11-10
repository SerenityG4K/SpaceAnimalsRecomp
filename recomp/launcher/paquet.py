#!/usr/bin/env python3
"""Assemble la release à distribuer : launcher, sources du recompilateur et outils, SANS rien qui vienne du jeu (ni copie,
ni code généré, ni objets, ni shaders, ni icône, ni journaux). Seule exception, voulue par SerenityG4K depuis la 1.0.2 :
les extraits vidéo du fond du launcher (son gameplay, recomp/launcher/extraits).

  python paquet.py <dossier de sortie> [--version 1.0.0] [--garder]

Produit dans le dossier de sortie :
  SpaceAnimalsRecomp-<version>-windows.zip   paquet complet (outils compris) : la première installation
  SpaceAnimalsRecomp-<version>-update.zip    mise à jour (sans les outils) : téléchargée par le launcher (maj.py)
  SHA256SUMS.txt                             empreintes des deux archives

Contenu :
  lanceur.exe                      le launcher (compilé par construire.py)
  LISEZMOI.txt                     mode d'emploi
  THIRD_PARTY_NOTICES.txt          logiciels tiers fournis : versions, licences, adresses du code source
  LICENCES/                        textes complets de ces licences (recomp/licences)
  recomp/*.py, hooks.txt, VERSION  recompilateur
  recomp/a64/*.py                  décodeur et traducteur ARM64 → C++
  recomp/runtime/**                runtime C++ (noyau, services, rendu Vulkan, en-têtes Vulkan tiers)
  recomp/shaders/*.py              extraction et traduction des shaders
  recomp/launcher/preparer.py      construction (bouton « RECOMPILER LE JEU »)
  recomp/launcher/maj.py           mises à jour depuis les releases GitHub
  recomp/launcher/outils.py        outils de construction depuis leurs sources officielles
  recomp/launcher/extraits/*.mp4   extraits vidéo du fond du launcher (gameplay de SerenityG4K, fournis à sa demande)
  tools/nup.py, tools/nso.py       lecture de la copie .nup et des exécutables NSO
  outils/                          Python, GCC, glslang (paquet complet seulement ; GCC et glslang réduits aux
                                   fichiers que la construction utilise, voir outils.GARDER_GCC)

Le joueur fournit sa propre copie (.nup) ; tout ce qui en dérive est produit sur son PC et y reste.
"""
import argparse
import hashlib
import os
import shutil
import sys
import zipfile

LANCEUR = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(LANCEUR)
PROJET = os.path.dirname(RECOMP)
NOM = "SpaceAnimalsRecomp"

LISEZMOI = """StarFox Remake [recompiled] — SpaceAnimalsRecomp {version}
=====================================================================

Portage natif PC de Star Fox (Nintendo Switch 2), par recompilation statique, créé par SerenityG4K.
https://github.com/SerenityG4K/SpaceAnimalsRecomp

Ce paquet ne contient AUCUN fichier du jeu. Il recompile le jeu sur votre PC, à partir de votre propre copie.

PROJET EXPÉRIMENTAL : on en est au stade « ça tourne », rien n'a encore été optimisé. Le jeu se termine en entier et
reste plutôt stable, mais il n'a été testé que sur un PC costaud (AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5070 Ti) : on ne
sait pas encore comment il tourne ailleurs. Signalez vos bugs sur GitHub en précisant votre processeur et votre carte
graphique.

Il vous faut :
  - Windows 10 ou 11, 64 bits, processeur avec AVX2, carte graphique Vulkan 1.3 (pilote à jour) ;
  - votre copie de Star Fox (Nintendo Switch 2) au format .nup, avec la mise à jour 2.0.0 (v131072) ;
  - environ 6 Go libres pendant la construction ;
  - un dossier dont le chemin n'a que des caractères latins (par exemple C:\\Jeux\\StarFox) : le compilateur
    ne gère pas les autres. Les espaces sont acceptés : dans ce cas, le compilateur est copié une fois dans
    C:\\ProgramData\\StarFoxRecomp (environ 400 Mo).

Étapes :
  1. Décompressez ce dossier où vous voulez (pas dans Program Files).
  2. Lancez lanceur.exe.
  3. Choisissez votre copie (COPIE › choisir), puis cliquez « RECOMPILER LE JEU ».
     Comptez 35 à 50 minutes : le launcher reste utilisable pendant ce temps.
  4. Vers la fin, tous les shaders du jeu sont compilés pour votre carte graphique (environ une minute) : plus
     aucune saccade de compilation en jeu. Le jeu s'ouvre aussi quelques secondes, sans le son, pour relever les
     shaders qu'il crée au démarrage : laissez-le faire.
  5. Cliquez « JOUER ». Sous le bouton : plein écran ou fenêtré. Réglages : langue du jeu, résolution, images
     par seconde, volume, mises à jour.

Mises à jour : le launcher vérifie au démarrage les releases du projet sur GitHub (désactivable dans Réglages). Un clic
sur « MISE À JOUR … DISPONIBLE » télécharge la nouvelle version, vérifie son empreinte SHA-256, l'installe et relance le
launcher ; « METTRE À JOUR LE JEU » refait ensuite seulement les étapes dont le code a changé.

Journaux : recomp\\build\\full\\preparation.log (construction), recomp\\build\\full\\recomp_run.log (partie),
recomp\\build\\full\\precompile.log (shaders), mise_a_jour.log (mises à jour).

Les extraits vidéo du fond du launcher (gameplay enregistré par SerenityG4K) sont fournis avec le paquet. L'icône du jeu
est tirée de votre copie et ne quitte jamais votre PC.
Votre copie n'est jamais modifiée (lecture seule).

Licences : SpaceAnimalsRecomp est sous licence GNU GPL v3. Le dossier outils contient des logiciels libres tiers,
inchangés (GCC, binutils et MinGW-w64 de WinLibs, glslang, Python) : leurs licences et les adresses de leur code
source sont dans THIRD_PARTY_NOTICES.txt, et les textes complets des licences dans le dossier LICENCES.

Star Fox est une marque de Nintendo. Projet de fan, sans lien avec Nintendo.
"""

EXCLUS_DOSSIERS = {"__pycache__", "build", "out", "extraits", "bench"}


def copier(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copier_arbre(src, dst, extensions=None):
    for racine, dossiers, fichiers in os.walk(src):
        dossiers[:] = [d for d in dossiers if d not in EXCLUS_DOSSIERS]
        for f in fichiers:
            if extensions and not f.endswith(extensions):
                continue
            p = os.path.join(racine, f)
            copier(p, os.path.join(dst, os.path.relpath(p, src)))


def empreinte(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def zipper(dossier, archive, sans=()):
    """Archive du dossier (son nom en tête des chemins), sans les sous-dossiers de premier niveau `sans`."""
    base = os.path.dirname(dossier)
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
        for rt, ds, fs in os.walk(dossier):
            if rt == dossier:
                ds[:] = [d for d in ds if d not in sans]
            for f in fs:
                p = os.path.join(rt, f)
                zf.write(p, os.path.relpath(p, base))


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser()
    ap.add_argument("sortie")
    ap.add_argument("--version", default=open(os.path.join(RECOMP, "VERSION"), encoding="utf-8").read().strip())
    ap.add_argument("--garder", action="store_true", help="garder le dossier assemblé à côté des archives")
    a = ap.parse_args()
    sortie = os.path.abspath(a.sortie)
    os.makedirs(sortie, exist_ok=True)
    out = os.path.join(sortie, f"{NOM}-{a.version}")
    if os.path.exists(out):
        sys.exit(f"{out} existe déjà")
    exe = os.path.join(PROJET, "lanceur.exe")   # à la racine du projet depuis la 1.0.1 (construire.py)
    if not os.path.exists(exe):
        sys.exit("lanceur.exe absent de la racine du projet : python construire.py")
    copier(exe, os.path.join(out, "lanceur.exe"))
    with open(os.path.join(out, "LISEZMOI.txt"), "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write(LISEZMOI.format(version=a.version))
    r = os.path.join(out, "recomp")
    for f in ("recomp.py", "build.py", "fullbuild.py", "link.py", "hooks.txt", "VERSION"):
        copier(os.path.join(RECOMP, f), os.path.join(r, f))
    copier_arbre(os.path.join(RECOMP, "a64"), os.path.join(r, "a64"), (".py",))
    copier_arbre(os.path.join(RECOMP, "runtime"), os.path.join(r, "runtime"), (".cpp", ".h", ".hpp", ".c", ".inl"))
    for f in os.listdir(os.path.join(RECOMP, "shaders")):
        if f.endswith(".py"):
            copier(os.path.join(RECOMP, "shaders", f), os.path.join(r, "shaders", f))
    for f in ("preparer.py", "maj.py", "outils.py"):
        copier(os.path.join(LANCEUR, f), os.path.join(r, "launcher", f))
    # extraits du fond du launcher : gameplay enregistré par SerenityG4K, distribué à sa demande (1.0.2)
    extraits = sorted(f for f in os.listdir(os.path.join(LANCEUR, "extraits")) if f.startswith("extrait_") and f.endswith(".mp4"))
    if not extraits:
        sys.exit("aucun extrait dans recomp/launcher/extraits")
    for f in extraits:
        copier(os.path.join(LANCEUR, "extraits", f), os.path.join(r, "launcher", "extraits", f))
    for f in ("nup.py", "nso.py"):
        copier(os.path.join(PROJET, "tools", f), os.path.join(out, "tools", f))
    # avis et textes des licences des outils fournis : à la racine, dans les deux archives
    licences = os.path.join(RECOMP, "licences")
    copier(os.path.join(licences, "THIRD_PARTY_NOTICES.txt"), os.path.join(out, "THIRD_PARTY_NOTICES.txt"))
    for f in sorted(os.listdir(licences)):
        if f.endswith(".txt") and f != "THIRD_PARTY_NOTICES.txt":
            copier(os.path.join(licences, f), os.path.join(out, "LICENCES", f))
    outils = os.path.join(RECOMP, "outils")
    if not os.path.isdir(outils):
        sys.exit("pas de dossier recomp/outils (python outils.py) : paquet complet impossible")
    sys.path.insert(0, LANCEUR)
    import outils as regles   # noqa: E402  (fichiers de GCC et de glslang utiles à la construction)
    retires = taille_retiree = 0
    for racine, dossiers, fichiers in os.walk(outils):   # sans les archives téléchargées ni les caches Python
        dossiers[:] = [d for d in dossiers if d not in ("_archives", "__pycache__")]
        for f in fichiers:
            p = os.path.join(racine, f)
            rel = os.path.relpath(p, outils).replace("\\", "/")
            outil, _, dans_outil = rel.partition("/")
            if dans_outil and not regles.garder(outil, dans_outil):
                retires += 1
                taille_retiree += os.path.getsize(p)
                continue
            copier(p, os.path.join(out, "outils", os.path.relpath(p, outils)))
    print(f"outils : {retires} fichiers inutiles à la construction écartés ({taille_retiree / 1e6:.0f} Mo)")
    # contrôle : rien qui vienne du jeu
    interdits = (".nup", ".bin", ".o", ".spv", ".mp4", ".log", ".save", ".ini", ".cache",
                 ".ico", ".jpg", ".jpeg", ".png")   # images : l'icône du jeu, les captures (lanceur.exe a la sienne)
    for racine, _, fichiers in os.walk(out):
        for f in fichiers:
            extrait = os.path.normpath(racine) == os.path.normpath(os.path.join(r, "launcher", "extraits")) and                 f.startswith("extrait_") and f.endswith(".mp4")
            if f.lower().endswith(interdits) and os.path.join(out, "outils") not in racine and not extrait:
                sys.exit(f"fichier interdit dans le paquet : {os.path.join(racine, f)}")
    taille = sum(os.path.getsize(os.path.join(rt, f)) for rt, _, fs in os.walk(out) for f in fs)
    print(f"paquet : {out} ({taille / 1e6:.1f} Mo)")
    complet = os.path.join(sortie, f"{NOM}-{a.version}-windows.zip")
    maj = os.path.join(sortie, f"{NOM}-{a.version}-update.zip")
    zipper(out, complet)
    zipper(out, maj, sans=("outils",))
    with open(os.path.join(sortie, "SHA256SUMS.txt"), "w", encoding="utf-8", newline="\n") as f:
        for z in (complet, maj):
            f.write(f"{empreinte(z)}  {os.path.basename(z)}\n")
            print(f"archive : {z} ({os.path.getsize(z) / 1e6:.1f} Mo)")
    if not a.garder:
        shutil.rmtree(out)


if __name__ == "__main__":
    main()
