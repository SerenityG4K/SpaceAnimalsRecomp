#!/usr/bin/env python3
"""Copies de secours des sauvegardes du joueur, avant chaque construction et chaque mise à jour.

  python sauvegardes.py [--raison texte]      copie de secours maintenant, puis liste des copies gardées

Les sauvegardes du jeu sont dans %APPDATA%\\SpaceAnimalsRecomp\\save depuis la 1.0.3 (runtime/os/svc_fs.cpp) ; jusqu'à
la 1.0.2, dans recomp\\build\\full\\save, où elles restent tant que le jeu mis à jour n'a pas été lancé une fois.
Les deux dossiers sont copiés, s'ils contiennent des fichiers, dans
%APPDATA%\\SpaceAnimalsRecomp\\save_backups\\<date>_<heure>_<raison>[_ancien]. Une copie identique à la précédente n'est
pas refaite ; les 10 dernières sont gardées. Rien n'est jamais écrit ni supprimé dans les dossiers de sauvegarde
eux-mêmes : une construction ou une mise à jour ne peut donc pas les abîmer, et si un jour le jeu en abîmait une, la
copie d'avant est là.
"""
import datetime
import hashlib
import os
import shutil
import sys

LANCEUR = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(LANCEUR)
A_GARDER = 10
ANCIEN = os.path.join(RECOMP, "build", "full", "save")   # emplacement jusqu'à la 1.0.2 (remplacé dans les vérifications)


def racine_profil():
    """%APPDATA%\\SpaceAnimalsRecomp (None si APPDATA n'est pas défini)."""
    appdata = os.environ.get("APPDATA")
    return os.path.join(appdata, "SpaceAnimalsRecomp") if appdata else None


def dossiers():
    """[(dossier de sauvegarde, suffixe)] : le nouvel emplacement puis l'ancien, s'ils existent."""
    out = []
    profil = racine_profil()
    if profil:
        out.append((os.path.join(profil, "save"), ""))
    out.append((ANCIEN, "_ancien"))
    return [(d, s) for d, s in out if os.path.isdir(d)]


def fichiers(dossier):
    """Chemins relatifs des fichiers du dossier, triés (sans la note laissée à l'ancien emplacement)."""
    out = []
    for r, _, fs in os.walk(dossier):
        for f in fs:
            if not f.startswith("SAVE MOVED") and f != ".empreinte":   # ni la note, ni la marque d'une copie de secours
                out.append(os.path.relpath(os.path.join(r, f), dossier))
    return sorted(out)


def empreinte(dossier):
    """SHA-256 des noms et des contenus : identique si et seulement si les sauvegardes sont les mêmes."""
    h = hashlib.sha256()
    for rel in fichiers(dossier):
        h.update(rel.replace("\\", "/").encode("utf-8") + b"\0")
        with open(os.path.join(dossier, rel), "rb") as f:
            h.update(hashlib.sha256(f.read()).digest())
    return h.hexdigest()


def secours(raison="construction", dire=print):
    """Copie de secours des sauvegardes. Ne lève jamais d'exception : une copie impossible est signalée, sans arrêter
    la construction ni la mise à jour. Rend la liste des dossiers créés."""
    crees = []
    try:
        profil = racine_profil()
        if not profil:
            dire("saves: APPDATA missing, no backup")
            return crees
        base = os.path.join(profil, "save_backups")
        raison = "".join(c if c.isalnum() or c in "-_" else "-" for c in raison)[:24] or "copie"
        for dossier, suffixe in dossiers():
            noms = fichiers(dossier)
            if not noms:
                continue
            e = empreinte(dossier)
            os.makedirs(base, exist_ok=True)
            # dernière copie du même emplacement : identique → rien à refaire
            memes = [d for d in sorted(os.listdir(base), reverse=True)
                     if os.path.isdir(os.path.join(base, d)) and d.endswith("_ancien") == bool(suffixe)]
            deja = None
            if memes:
                m = os.path.join(base, memes[0], ".empreinte")
                if os.path.exists(m) and open(m, encoding="utf-8").read().strip() == e:
                    deja = memes[0]
            if deja:
                dire(f"saves: {len(noms)} file(s) in {dossier}, already copied ({deja})")
                continue
            nom = datetime.datetime.now().strftime("%Y-%m-%d_%H%M%S") + "_" + raison + suffixe
            dest = os.path.join(base, nom)
            shutil.copytree(dossier, dest)
            if empreinte(dest) != e:
                dire(f"saves: backup of {dossier} incomplete ({dest})")
                continue
            with open(os.path.join(dest, ".empreinte"), "w", encoding="utf-8") as f:
                f.write(e + "\n")
            crees.append(dest)
            dire(f"saves: {len(noms)} file(s) from {dossier} copied to {dest}")
        # les plus anciennes copies au-delà de A_GARDER (par emplacement) sont retirées
        if os.path.isdir(base):
            for suffixe in ("", "_ancien"):
                copies = sorted(d for d in os.listdir(base) if os.path.isdir(os.path.join(base, d)) and
                                (d.endswith("_ancien") if suffixe else not d.endswith("_ancien")))
                for d in copies[:-A_GARDER]:
                    shutil.rmtree(os.path.join(base, d), ignore_errors=True)
    except Exception as ex:  # noqa: BLE001 - jamais bloquant
        dire(f"saves: backup impossible ({type(ex).__name__}: {ex})")
    return crees


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    raison = sys.argv[sys.argv.index("--raison") + 1] if "--raison" in sys.argv else "manuel"
    secours(raison)
    profil = racine_profil()
    base = os.path.join(profil, "save_backups") if profil else None
    if base and os.path.isdir(base):
        for d in sorted(os.listdir(base)):
            print("  " + os.path.join(base, d))
    return 0


if __name__ == "__main__":
    sys.exit(main())
