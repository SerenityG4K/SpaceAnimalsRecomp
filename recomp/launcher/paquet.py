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
  LISEZMOI.txt, README.txt         mode d'emploi, en français et en anglais (à tenir à jour ensemble)
  THIRD_PARTY_NOTICES.txt          logiciels tiers fournis : versions, licences, adresses du code source
  LICENCES/                        textes complets de ces licences (recomp/licences)
  recomp/*.py, hooks.txt, lots.txt, VERSION  recompilateur
  recomp/a64/*.py                  décodeur et traducteur ARM64 → C++
  recomp/runtime/**                runtime C++ (noyau, services, rendu Vulkan, en-têtes Vulkan tiers)
  recomp/shaders/*.py              extraction et traduction des shaders
  recomp/launcher/preparer.py      construction (bouton « RECOMPILER LE JEU »)
  recomp/launcher/maj.py           mises à jour depuis les releases GitHub
  recomp/launcher/outils.py        outils de construction depuis leurs sources officielles
  recomp/launcher/rapport.py       rapport de bug (bouton « RAPPORT DE BUG ») et en-tête des journaux
  recomp/launcher/sauvegardes.py   copies de secours des sauvegardes avant construction et mise à jour
  recomp/launcher/desinstaller.py  nettoyage et désinstallation (bouton « DÉSINSTALLER » du launcher)
  recomp/launcher/deplacer.py      déplacement de l'installation (page Réglages, « Emplacement du jeu »)
  uninstall.bat                 la même désinstallation sans le launcher (double-clic)
  recomp/launcher/extraits/*.mp4   extraits vidéo du fond du launcher (gameplay de SerenityG4K, fournis à sa demande)
  tools/nup.py, tools/nso.py       lecture de la copie .nup et des exécutables NSO
  outils/                          Python, GCC, glslang, SDL3, et les bibliothèques de mise à l'échelle : FSR d'AMD
                                   (outils/fsr) et DLSS de NVIDIA (outils/dlss), ni l'une ni l'autre sous GPL (paquet
                                   complet seulement ; GCC et glslang réduits aux fichiers que la construction
                                   utilise, voir outils.GARDER_GCC)

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

# Désinstallation sans le launcher : même script que le bouton « DÉSINSTALLER ». Depuis le dossier temporaire, pour
# ne pas tenir le dossier d'installation ; tout sur une ligne après Python, car ce fichier est supprimé en route.
DESINSTALLER_BAT = """@echo off
rem SpaceAnimalsRecomp : desinstallation / uninstall (voir LISEZMOI.txt, see README.txt)
setlocal
set "RACINE=%~dp0"
cd /d "%TEMP%"
set "PY=%RACINE%outils\\python\\python.exe"
if not exist "%PY%" set "PY=py"
"%PY%" -X utf8 "%RACINE%recomp\\launcher\\desinstaller.py" --desinstaller --racine "%RACINE%." & echo. & pause & exit /b
"""

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
  - 32 Go de RAM conseillés ; avant de jouer, fermez les programmes lourds et laissez Windows gérer la taille du
    fichier d'échange (c'est son réglage par défaut) ;
  - votre copie de Star Fox (Nintendo Switch 2) au format .nup, avec la mise à jour 2.0.0 (v131072) ;
  - environ 6 Go libres pendant la construction ;
  - un dossier dont le chemin n'a que des caractères latins (par exemple C:\\Jeux\\StarFox) : le compilateur
    ne gère pas les autres. Les espaces sont acceptés : dans ce cas, le compilateur est copié une fois dans
    C:\\ProgramData\\StarFoxRecomp (environ 600 Mo).

Étapes :
  1. Décompressez ce dossier où vous voulez (pas dans Program Files). Vous pourrez le déplacer plus tard depuis le
     launcher (Réglages › Emplacement du jeu), et y créer un raccourci sur le bureau.
  2. Lancez lanceur.exe.
  3. Choisissez votre copie (COPIE › choisir), puis cliquez « RECOMPILER LE JEU ».
     Comptez 20 à 45 minutes selon votre processeur : le launcher reste utilisable pendant ce temps.
  4. Vers la fin, tous les shaders du jeu sont compilés pour votre carte graphique (environ une minute) : plus
     aucune saccade de compilation en jeu. Le jeu tourne aussi quelques secondes en arrière-plan, sans fenêtre ni
     son, pour relever les shaders qu'il crée au démarrage. Rien ne s'ouvre tout seul pendant la construction.
  5. Cliquez « JOUER ». Sous le bouton : plein écran ou fenêtré. Graphismes : carte graphique, résolution, images
     par seconde, rendu 3D, et deux mises à l'échelle expérimentales, le DLSS de NVIDIA (cartes RTX seulement) et le
     FSR d'AMD. Réglages : langue du jeu, volume, vibrations, mises à jour, emplacement du jeu, raccourci sur le
     bureau. Contrôles : les touches du clavier se changent (clic sur une touche, puis la touche voulue), avec deux
     préréglages, WASD (clavier QWERTY) et ZQSD (clavier AZERTY) ; les boutons de la manette aussi (Xbox,
     PlayStation, Switch Pro, Joy-Con).

Mises à jour : le launcher vérifie au démarrage les releases du projet sur GitHub (désactivable dans Réglages) et vous
demande avant d'en installer une. Un clic sur « MISE À JOUR … DISPONIBLE » télécharge la nouvelle version, vérifie son
empreinte SHA-256, l'installe et relance le launcher. Votre jeu compilé est gardé. Certaines améliorations nécessitent
la reconstruction du jeu : le launcher la propose alors (« RECONSTRUIRE MAINTENANT », seules les étapes dont le code a
changé sont refaites). Si vous refusez, vous continuez à jouer avec le jeu actuel.

Sauvegardes : %APPDATA%\\SpaceAnimalsRecomp\\save. Une copie de secours est faite avant chaque construction et chaque
mise à jour, dans %APPDATA%\\SpaceAnimalsRecomp\\save_backups (les 10 dernières sont gardées).

Journaux (en anglais) : recomp\\build\\full\\preparation.log (construction), recomp\\build\\full\\recomp_run.log
(partie), recomp\\build\\full\\precompile.log (shaders), lanceur.log (launcher), mise_a_jour.log (mises à jour).
« RAPPORT DE BUG » dans le launcher les réunit dans SpaceAnimalsRecomp-bug-report.zip, sans votre nom d'utilisateur
ni vos noms de dossiers : joignez ce fichier à votre issue GitHub.

Mémoire : depuis la version 1.0.3, le jeu prend sa mémoire au fur et à mesure, retire de la RAM les données des
textures déjà envoyées à la carte graphique (gardées dans un fichier temporaire de quelques Go, dans le dossier
temporaire de Windows, supprimé à la fermeture du jeu) et prépare ses shaders à leur premier usage. Si le jeu se
comporte moins bien depuis (images manquantes, arrêts), coupez ces économies dans recomp\\build\\full\\recomp.ini :
texture_release = false, shaders_on_demand = false, zero_page_release = false (pages vides de la mémoire du jeu
rendues à Windows), cold_page_release = false (pages oubliées rangées dans le fichier temporaire), ou
memory_saver = false pour tout remettre comme en version 1.0.2 (le jeu prend alors plus de mémoire).

Mises à jour : le launcher vous demande avant d'en installer une, vous pouvez refuser. Une fois une mise à jour
installée, il peut indiquer que certaines améliorations nécessitent la reconstruction du jeu : il la propose (seules
les parties qui ont changé sont refaites). Si vous refusez, vous continuez à jouer avec le jeu actuel et vous pouvez
la lancer plus tard depuis l'accueil du launcher.

Désinstaller : bouton « DÉSINSTALLER » de la page CONSTRUCTION du launcher, ou double-clic sur uninstall.bat.
Tout ce que le paquet a écrit sur le PC est supprimé : ce dossier, le jeu construit et la copie du compilateur
(C:\\ProgramData\\StarFoxRecomp). Votre copie du jeu n'est jamais supprimée, et vos sauvegardes
(%APPDATA%\\SpaceAnimalsRecomp) seulement si vous le demandez.

Les extraits vidéo du fond du launcher (gameplay enregistré par SerenityG4K) sont fournis avec le paquet. L'icône du jeu
est tirée de votre copie et ne quitte jamais votre PC.
Votre copie n'est jamais modifiée (lecture seule).

Licences : SpaceAnimalsRecomp est sous licence GNU GPL v3. Le dossier outils contient des logiciels libres tiers,
inchangés (GCC, Clang/LLVM, binutils et MinGW-w64 de WinLibs, glslang, Python, SDL3) : leurs licences et les adresses de leur code
source sont dans THIRD_PARTY_NOTICES.txt, et les textes complets des licences dans le dossier LICENCES.
Il contient aussi deux bibliothèques de mise à l'échelle, inchangées : le FSR d'AMD (outils\\fsr, licence MIT) et le DLSS
de NVIDIA (outils\\dlss\\nvngx_dlss.dll). Le DLSS n'est pas un logiciel libre : il est fourni sous la licence de NVIDIA
(LICENCES\\NVIDIA-DLSS-SDK-LICENSE.txt), pas sous la GPL. Ce logiciel utilise NVIDIA DLSS ; NVIDIA, GeForce RTX et DLSS
sont des marques de NVIDIA Corporation.

Star Fox est une marque de Nintendo. Projet de fan, sans lien avec Nintendo.
"""


README_EN = """StarFox Remake [recompiled] — SpaceAnimalsRecomp {version}
=====================================================================

Native PC port of Star Fox (Nintendo Switch 2) by static recompilation, created by SerenityG4K.
https://github.com/SerenityG4K/SpaceAnimalsRecomp

This package contains NO game files. It recompiles the game on your PC, from your own copy.

EXPERIMENTAL PROJECT: we are at the "it runs" stage. The game can be finished and is fairly stable, but it has only
been tested on a strong PC (AMD Ryzen 7 9700X, NVIDIA GeForce RTX 5070 Ti): we do not know yet how it runs elsewhere.
Report bugs on GitHub with your processor and graphics card.

You need:
  - Windows 10 or 11, 64-bit, a processor with AVX2, a Vulkan 1.3 graphics card (up-to-date driver);
  - 32 GB of RAM recommended; before playing, close heavy programs and let Windows manage the page file size
    (its default setting);
  - your copy of Star Fox (Nintendo Switch 2) in .nup format, with update 2.0.0 (v131072);
  - about 6 GB free during the build;
  - a folder whose path only has Latin characters (for example C:\\Games\\StarFox): the compiler does not handle
    others. Spaces are accepted: in that case the compiler is copied once to C:\\ProgramData\\StarFoxRecomp
    (about 600 MB).

Steps:
  1. Unzip this folder wherever you like (not in Program Files). You can move it later from the launcher (Settings ›
     Game location), and create a desktop shortcut there.
  2. Run lanceur.exe (the launcher; EN at the top right switches it to English).
  3. Choose your copy (COPY › choose), then click "RECOMPILE THE GAME".
     Allow 20 to 45 minutes depending on your processor: the launcher stays usable meanwhile.
  4. Near the end, all the game's shaders are compiled for your graphics card (about a minute): no more
     compilation stutter in game. The game also runs for a few seconds in the background, with no window and no
     sound, to collect the shaders it creates at startup. Nothing opens by itself during the build.
  5. Click "PLAY". Under the button: fullscreen or windowed. Graphics: graphics card, resolution, frames per
     second, 3D rendering, and two experimental upscalers, NVIDIA DLSS (RTX cards only) and AMD FSR. Settings: game
     language, volume, rumble, updates, game location, desktop shortcut. Controls: the keyboard keys can be changed
     (click a key, then press the one you want), with two presets, WASD (QWERTY keyboard) and ZQSD (AZERTY
     keyboard); so can the controller buttons (Xbox, PlayStation, Switch Pro, Joy-Con).

Updates: at startup the launcher checks the project's releases on GitHub (can be turned off in Settings) and asks
before installing one. A click on "UPDATE … AVAILABLE" downloads the new version, checks its SHA-256 hash, installs it
and restarts the launcher. Your compiled game is kept. Some improvements require rebuilding the game: the launcher
then offers it ("REBUILD NOW", only the steps whose code changed are redone). If you decline, you keep playing with
the current game.

Saves: %APPDATA%\\SpaceAnimalsRecomp\\save. A backup is made before each build and each update, in
%APPDATA%\\SpaceAnimalsRecomp\\save_backups (the last 10 are kept).

Logs (in English): recomp\\build\\full\\preparation.log (build), recomp\\build\\full\\recomp_run.log (game),
recomp\\build\\full\\precompile.log (shaders), lanceur.log (launcher), mise_a_jour.log (updates).
"BUG REPORT" in the launcher gathers them in SpaceAnimalsRecomp-bug-report.zip, with your user name and folder
names removed: attach that file to your GitHub issue.

Memory: since version 1.0.3 the game takes its memory as needed, removes from RAM the texture data already sent
to the graphics card (kept in a temporary file of a few GB in the Windows temp folder, deleted when the game closes)
and prepares its shaders at first use. If the game misbehaves since then (missing images, stops), turn these savings
off in recomp\\build\\full\\recomp.ini: texture_release = false, shaders_on_demand = false, zero_page_release = false
(empty pages of game memory given back to Windows), cold_page_release = false (forgotten pages put in the
temporary file), or memory_saver = false to put everything back as in version 1.0.2 (the game then takes more
memory).

Updates: the launcher asks before installing one, and you can decline. Once an update is installed, it offers to
rebuild the parts of the game that changed, because some improvements require rebuilding the game. If you decline, you
keep playing with the current game and you can start the rebuild later from the launcher's home screen.

Uninstall: UNINSTALL button on the launcher's CONSTRUCTION page, or double-click uninstall.bat. Everything the
package wrote on the PC is deleted: this folder, the built game and the compiler copy (C:\\ProgramData\\StarFoxRecomp).
Your game copy is never deleted, and your saves (%APPDATA%\\SpaceAnimalsRecomp) only if you ask.

The video clips behind the launcher (gameplay recorded by SerenityG4K) come with the package. The game icon is taken
from your copy and never leaves your PC.
Your copy is never modified (read-only).

Licences: SpaceAnimalsRecomp is under the GNU GPL v3. The outils folder contains third-party free software, unchanged
(GCC, Clang/LLVM, binutils and MinGW-w64 from WinLibs, glslang, Python, SDL3): their licences and the addresses of their
source code are in THIRD_PARTY_NOTICES.txt, and the full licence texts in the LICENCES folder.
It also contains two upscaling libraries, unchanged: AMD FSR (outils\\fsr, MIT licence) and NVIDIA DLSS
(outils\\dlss\\nvngx_dlss.dll). DLSS is not free software: it comes under NVIDIA's licence
(LICENCES\\NVIDIA-DLSS-SDK-LICENSE.txt), not under the GPL. This software uses NVIDIA DLSS; NVIDIA, GeForce RTX and DLSS
are trademarks of NVIDIA Corporation.

Star Fox is a trademark of Nintendo. Fan project, not affiliated with Nintendo.
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
    with open(os.path.join(out, "uninstall.bat"), "w", encoding="ascii", newline="\r\n") as f:
        f.write(DESINSTALLER_BAT)
    with open(os.path.join(out, "LISEZMOI.txt"), "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write(LISEZMOI.format(version=a.version))
    with open(os.path.join(out, "README.txt"), "w", encoding="utf-8-sig", newline="\r\n") as f:
        f.write(README_EN.format(version=a.version))
    r = os.path.join(out, "recomp")
    for f in ("recomp.py", "build.py", "fullbuild.py", "link.py", "hooks.txt", "lots.txt", "VERSION"):
        copier(os.path.join(RECOMP, f), os.path.join(r, f))
    copier_arbre(os.path.join(RECOMP, "a64"), os.path.join(r, "a64"), (".py",))
    copier_arbre(os.path.join(RECOMP, "runtime"), os.path.join(r, "runtime"), (".cpp", ".h", ".hpp", ".c", ".inl", ".comp",
                                                                            ".py", ".md", ".txt"))
    for f in os.listdir(os.path.join(RECOMP, "shaders")):
        if f.endswith(".py"):
            copier(os.path.join(RECOMP, "shaders", f), os.path.join(r, "shaders", f))
    for f in ("preparer.py", "maj.py", "outils.py", "rapport.py", "sauvegardes.py", "desinstaller.py", "deplacer.py"):
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
    # Outils faits de fichiers seuls (DLL du FSR, du DLSS) : le paquet ne porte que ce que outils.py installe, pour
    # qu'une installation mise à jour (qui les télécharge) ait le même dossier outils qu'une installation neuve. Leurs
    # licences sont dans LICENCES/, à la racine.
    seuls = {}
    for o in regles.OUTILS:
        if o.get("fichier"):
            seuls.setdefault(o["dest"], set()).add(o["url"].rsplit("/", 1)[1])
    for racine, dossiers, fichiers in os.walk(outils):   # sans les archives téléchargées ni les caches Python
        dossiers[:] = [d for d in dossiers if d not in ("_archives", "__pycache__")]
        for f in fichiers:
            p = os.path.join(racine, f)
            rel = os.path.relpath(p, outils).replace("\\", "/")
            outil, _, dans_outil = rel.partition("/")
            if outil in seuls and dans_outil not in seuls[outil]:
                continue   # note ou licence posée à la main à côté des DLL : pas dans le paquet
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
