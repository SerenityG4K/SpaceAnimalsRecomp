#!/usr/bin/env python3
"""Nettoyage et désinstallation de SpaceAnimalsRecomp : tout ce que le paquet a écrit sur le PC, en une commande.

  python desinstaller.py --lister                      ce qui existe sur ce PC, avec les tailles ; rien n'est supprimé
  python desinstaller.py --nettoyer [--oui]            le jeu construit, le code généré, les images extraites et la
                                                       copie du compilateur : la place revient ; le launcher, vos
                                                       réglages et vos sauvegardes restent, le jeu se reconstruit
  python desinstaller.py --desinstaller [--sauvegardes] [--oui] [--pid P]
                                                       tout le paquet ; les sauvegardes seulement avec --sauvegardes

Options : --racine R (dossier d'installation, par défaut celui de ce script), --pid P (attendre la fin de ce processus,
le launcher), --oui (sans question).

Jamais supprimés : votre copie du jeu (.nup, .nsp, .xci, .nsz, où qu'elle soit), et vos sauvegardes sans --sauvegardes.
Avant toute suppression, les sauvegardes sont copiées dans %APPDATA%\\SpaceAnimalsRecomp\\save_backups, et une
sauvegarde restée dans l'ancien dossier (recomp\\build\\full\\save, jusqu'à la 1.0.2) est reprise dans
%APPDATA%\\SpaceAnimalsRecomp\\save. Refus dans un dossier de sources (dépôt cloné, dossier de développement) : ce
script ne supprime que des installations faites depuis le paquet.

Ce que le paquet écrit sur un PC :
  <installation>\\recomp\\build          jeu construit, shaders, journaux, réglages (recomp.ini)
  <installation>\\recomp\\out            code C++ généré depuis votre copie du jeu
  <installation>\\extracted, \\work      fichiers extraits de votre copie du jeu
  %ProgramData%\\StarFoxRecomp           copie du compilateur (installation dans un dossier dont le chemin a des espaces)
  %TEMP%\\recomp_textures_*.tmp          fichier temporaire d'une partie interrompue par une coupure de courant
  %APPDATA%\\SpaceAnimalsRecomp          sauvegardes et copies de secours
  <installation>                         le paquet : launcher, outils, sources
Journal : %TEMP%\\SpaceAnimalsRecomp-uninstall.log (en anglais).
"""
import argparse
import ctypes
import glob
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time

LANCEUR = os.path.dirname(os.path.abspath(__file__))
COPIES_DU_JEU = (".nup", ".nsp", ".xci", ".nsz")
JOURNAL = os.path.join(tempfile.gettempdir(), "SpaceAnimalsRecomp-uninstall.log")


def dire(texte):
    print(texte, flush=True)
    try:
        with open(JOURNAL, "a", encoding="utf-8") as f:
            f.write(time.strftime("%Y-%m-%d %H:%M:%S  ") + texte + "\n")
    except OSError:
        pass


def long(p):
    """Chemin étendu : au-delà de 260 caractères, noms qui finissent par un point ou une espace."""
    p = os.path.abspath(p)
    return p if p.startswith("\\\\") else "\\\\?\\" + p


def taille(p):
    if os.path.isfile(p):
        return os.path.getsize(p)
    n = 0
    for d, _, fichiers in os.walk(long(p)):
        for f in fichiers:
            try:
                n += os.path.getsize(os.path.join(d, f))
            except OSError:
                pass
    return n


def go(n):
    return f"{n / 2**30:.1f} GB" if n >= 2**30 else f"{n / 2**20:.0f} MB"


def copies_du_jeu(racine):
    """Copies du jeu du joueur sous l'installation : jamais supprimées."""
    out = []
    for d, sous, fichiers in os.walk(racine):
        sous[:] = [s for s in sous if s.lower() not in ("outils", "out", "build", "shader_cache")]   # rien à y chercher
        out += [os.path.join(d, f) for f in fichiers if f.lower().endswith(COPIES_DU_JEU)]
    return out


def elements(racine):
    """(clé, description en anglais, chemins) de tout ce que le paquet a écrit ; les chemins absents sont retirés."""
    r = os.path.join(racine, "recomp")
    programdata = os.environ.get("ProgramData") or r"C:\ProgramData"
    appdata = os.environ.get("APPDATA")
    liste = [
        ("jeu", "built game, shaders, logs", [os.path.join(r, "build")]),
        ("code", "C++ code generated from your game copy", [os.path.join(r, "out")]),
        ("extraits", "files extracted from your game copy", [os.path.join(racine, "extracted"), os.path.join(racine, "work")]),
        ("compilateur", "compiler copy", [os.path.join(programdata, "StarFoxRecomp")]),
        ("temporaires", "temporary files", glob.glob(os.path.join(glob.escape(tempfile.gettempdir()), "recomp_textures_*.tmp"))),
        ("paquet", "the package: launcher, tools, sources", [racine]),
        ("sauvegardes", "your saves and their backups", [os.path.join(appdata, "SpaceAnimalsRecomp")] if appdata else []),
    ]
    return [(k, d, [p for p in ch if os.path.exists(p)]) for k, d, ch in liste]


def installation(racine):
    """None si `racine` est une installation faite depuis le paquet, sinon la raison du refus (en anglais)."""
    if not (os.path.exists(os.path.join(racine, "lanceur.exe")) and os.path.exists(os.path.join(racine, "recomp", "fullbuild.py"))
            and os.path.exists(os.path.join(racine, "recomp", "launcher", "maj.py"))):
        return "this folder is not a SpaceAnimalsRecomp installation"
    for marque in (".git", os.path.join("recomp", "launcher", "lanceur.cpp"), os.path.join("recomp", "verif"),
                   os.path.join("recomp", "depot")):
        if os.path.exists(os.path.join(racine, marque)):
            return f"this folder holds the project's sources ({marque}): delete it yourself if that is what you want"
    return None


def en_marche(racine):
    """Le jeu ou le launcher de cette installation tourne-t-il ? (un exécutable en cours ne s'ouvre pas en écriture)"""
    for exe in (os.path.join(racine, "recomp", "build", "full", "recomp_main.exe"), os.path.join(racine, "lanceur.exe")):
        if os.path.exists(exe):
            try:
                open(exe, "ab").close()
            except OSError:
                return os.path.basename(exe)
    return None


def attendre(pid, secondes=60):
    if not pid:
        return
    k32 = ctypes.windll.kernel32
    k32.OpenProcess.restype = ctypes.c_void_p
    h = k32.OpenProcess(0x00100000, False, int(pid))   # SYNCHRONIZE
    if h:
        k32.WaitForSingleObject(ctypes.c_void_p(h), int(secondes * 1000))
        k32.CloseHandle(ctypes.c_void_p(h))


def supprimer(chemin, garder=()):
    """Supprime un fichier ou un dossier, sauf les fichiers de `garder` (et les dossiers qui les contiennent). Rend la
    liste de ce qui n'a pas pu être supprimé (fichiers en cours d'utilisation)."""
    garder = {os.path.normcase(os.path.abspath(g)) for g in garder}
    restes = []

    def forcer(fonction, p, _):
        try:
            os.chmod(p, stat.S_IWRITE)
            fonction(p)
        except OSError:
            restes.append(p.replace("\\\\?\\", ""))

    if os.path.isfile(chemin) or os.path.islink(chemin):
        if os.path.normcase(os.path.abspath(chemin)) not in garder:
            try:
                os.chmod(chemin, stat.S_IWRITE)
                os.remove(chemin)
            except OSError:
                restes.append(chemin)
        return restes
    if not any(g.startswith(os.path.normcase(os.path.abspath(chemin)) + os.sep) for g in garder):
        shutil.rmtree(long(chemin), onerror=forcer)
        return restes
    for nom in os.listdir(chemin):   # ce dossier contient une copie du jeu : il reste, vidé du reste
        restes += supprimer(os.path.join(chemin, nom), garder)
    return restes


def sauver_les_sauvegardes(racine):
    """Copie de secours, puis reprise d'une sauvegarde restée dans l'ancien dossier (jusqu'à la 1.0.2)."""
    try:
        sys.path.insert(0, LANCEUR)
        import sauvegardes
        sauvegardes.secours("desinstallation", dire)
    except Exception as ex:  # noqa: BLE001
        dire(f"saves: backup not made ({type(ex).__name__}: {ex})")
    ancien = os.path.join(racine, "recomp", "build", "full", "save")
    appdata = os.environ.get("APPDATA")
    if appdata and os.path.isdir(ancien) and os.listdir(ancien):
        nouveau = os.path.join(appdata, "SpaceAnimalsRecomp", "save")
        if not os.path.exists(nouveau):
            shutil.copytree(ancien, nouveau)
            dire(f"saves: moved from the old folder to {nouveau}")
            return True
        return True
    return not (os.path.isdir(ancien) and os.listdir(ancien))


def question(texte):
    try:
        return input(texte + " [o/y = oui/yes, autre = non/no] ").strip().lower() in ("o", "oui", "y", "yes")
    except EOFError:
        return False


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--lister", action="store_true")
    g.add_argument("--nettoyer", action="store_true")
    g.add_argument("--desinstaller", action="store_true")
    ap.add_argument("--sauvegardes", action="store_true")
    ap.add_argument("--oui", action="store_true")
    ap.add_argument("--pid", type=int, default=0)
    ap.add_argument("--racine", default=os.path.dirname(os.path.dirname(LANCEUR)))
    a = ap.parse_args()
    racine = os.path.abspath(a.racine)
    el = elements(racine)
    copies = copies_du_jeu(racine)

    if a.lister:
        dire(f"SpaceAnimalsRecomp on this PC (installation: {racine})")
        for cle, desc, chemins in el:
            for p in chemins:
                dire(f"  {desc}: {p} ({go(taille(p))})")
        for c in copies:
            dire(f"  your game copy, never deleted: {c}")
        return 0

    refus = installation(racine)
    if refus:
        dire("@erreur " + refus)
        return 2
    attendre(a.pid)
    qui = en_marche(racine)
    if qui:
        dire(f"@erreur {qui} is running: close it first")
        return 2
    voulu = ("jeu", "code", "extraits", "compilateur", "temporaires") if a.nettoyer else \
        ("jeu", "code", "extraits", "compilateur", "temporaires", "paquet") + (("sauvegardes",) if a.sauvegardes else ())
    cibles = [(cle, desc, chemins) for cle, desc, chemins in el if cle in voulu and chemins]
    dire(("Cleaning" if a.nettoyer else "Uninstalling") + f" SpaceAnimalsRecomp ({racine})")
    for cle, desc, chemins in cibles:
        for p in chemins:
            dire(f"  to delete: {desc}: {p}" + ("" if cle == "paquet" else f" ({go(taille(p))})"))
    for c in copies:
        dire(f"  kept, your game copy: {c}")
    if not a.sauvegardes:
        dire("  kept, your saves: %APPDATA%\\SpaceAnimalsRecomp")
    if not a.oui:
        if not question("Supprimer ces fichiers ? / Delete these files?"):
            dire("nothing deleted")
            return 1
        if a.desinstaller and not a.sauvegardes and any(cle == "sauvegardes" and ch for cle, _, ch in el):
            if question("Supprimer aussi vos sauvegardes ? / Delete your saves too?"):
                cibles += [(cle, desc, ch) for cle, desc, ch in el if cle == "sauvegardes"]
                a.sauvegardes = True

    if not a.sauvegardes and not sauver_les_sauvegardes(racine):
        dire("@erreur your saves are still in recomp\\build\\full\\save and could not be copied to %APPDATA%: nothing deleted")
        return 2
    reglages = os.path.join(racine, "recomp", "build", "full", "recomp.ini")
    ini = open(reglages, "rb").read() if a.nettoyer and os.path.exists(reglages) else None
    restes = []
    moi = os.path.normcase(os.path.abspath(sys.executable))
    for cle, desc, chemins in cibles:
        for p in chemins:
            r = supprimer(p, copies)
            restes += r
            dire(f"  deleted: {p}" + (f" (except {len(r)} file(s) in use)" if r else ""))
    if ini is not None:   # nettoyage : les réglages du jeu restent
        os.makedirs(os.path.dirname(reglages), exist_ok=True)
        open(reglages, "wb").write(ini)
    if a.desinstaller:
        # Ce qui reste est ce Python (il ne peut pas se supprimer lui-même) : PowerShell finit après notre sortie, puis
        # retire le dossier d'installation s'il est vide (il ne l'est pas s'il contient votre copie du jeu).
        q = lambda s: "'" + s.replace("'", "''") + "'"   # noqa: E731
        # (jamais rien hors de l'installation : ce Python peut être celui du système, pas celui du paquet)
        sous = os.path.normcase(racine) + os.sep
        dossiers = [os.path.join(racine, d) for d in ("outils", "recomp", "tools", "LICENCES")] if not copies else \
            [os.path.dirname(moi)] if moi.startswith(sous) else []
        dossiers = [d for d in dossiers if os.path.normcase(os.path.abspath(d)).startswith(sous)]
        cmd = (f"Wait-Process -Id {os.getpid()} -ErrorAction SilentlyContinue; Start-Sleep -Milliseconds 500; "
               + "; ".join(f"Remove-Item -LiteralPath {q(d)} -Recurse -Force -ErrorAction SilentlyContinue"
                           for d in dossiers if os.path.exists(d))
               + f"; if (-not (Get-ChildItem -LiteralPath {q(racine)} -Force -ErrorAction SilentlyContinue)) "
               + f"{{ Remove-Item -LiteralPath {q(racine)} -Force -ErrorAction SilentlyContinue }}")
        subprocess.Popen(["powershell", "-NoProfile", "-NonInteractive", "-WindowStyle", "Hidden", "-Command", cmd],
                         cwd=tempfile.gettempdir(), creationflags=0x00000008 | 0x00000200 | 0x08000000,
                         stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        autres = [r for r in restes if not os.path.normcase(r).startswith(os.path.normcase(os.path.dirname(moi)))]
        if autres:
            dire(f"  {len(autres)} file(s) in use could not be deleted, for example {autres[0]}")
        dire("@fini uninstalled" + (f"; your game copy is still in {os.path.dirname(copies[0])}" if copies else ""))
    else:
        dire("@fini cleaned: the launcher will rebuild the game")
    return 0


if __name__ == "__main__":
    sys.exit(main())
