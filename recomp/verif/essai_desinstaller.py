"""Essai de desinstaller.py sur de fausses installations, avec APPDATA, ProgramData et TEMP d'essai : rien de réel
n'est touché.   python essai_desinstaller.py <recomp/launcher de développement> <dossier de travail>"""
import os
import shutil
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
dev, travail = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
if "scratchpad" not in travail and "sar_verif" not in travail:
    sys.exit("refus : dossier de travail inattendu")
echecs = []


def fabriquer(nom, avec_copie, sources=False):
    base = os.path.join(travail, nom)
    shutil.rmtree(base, ignore_errors=True)
    racine = os.path.join(base, "install [x] y", "SpaceAnimalsRecomp-9.9.9")
    for d in ("recomp/launcher/extraits", "recomp/build/full/save", "recomp/build/full/shader_cache", "recomp/out/main",
              "recomp/runtime/os", "extracted/patch/exefs", "work", "outils/python", "tools", "LICENCES"):
        os.makedirs(os.path.join(racine, d))
    fichiers = {"lanceur.exe": 2000, "LISEZMOI.txt": 100, "recomp/fullbuild.py": 100, "recomp/VERSION": 6,
                "recomp/launcher/maj.py": 100, "recomp/build/full/recomp_main.exe": 5000,
                "recomp/build/full/save/save_game_data.save": 34057, "recomp/build/full/recomp.ini": 300,
                "recomp/build/full/shader_cache/a.spv": 100, "recomp/out/main/lot_000.cpp": 9000,
                "extracted/patch/exefs/main": 4000, "work/main.img": 4000, "outils/python/python.exe": 100, "tools/nup.py": 50}
    for f, n in fichiers.items():
        open(os.path.join(racine, f), "wb").write(b"x" * n)
    for f in ("desinstaller.py", "sauvegardes.py"):
        shutil.copy2(os.path.join(dev, f), os.path.join(racine, "recomp", "launcher", f))
    if avec_copie:
        open(os.path.join(racine, "Mon jeu [0400E88025B1A000][v131072].nup"), "wb").write(b"n" * 1000)
    if sources:
        open(os.path.join(racine, "recomp", "launcher", "lanceur.cpp"), "w").write("// sources")
    env = dict(os.environ)
    for cle, d in (("APPDATA", "appdata"), ("ProgramData", "programdata"), ("TEMP", "temp"), ("TMP", "temp")):
        env[cle] = os.path.join(base, d)
        os.makedirs(env[cle], exist_ok=True)
    os.makedirs(os.path.join(env["ProgramData"], "StarFoxRecomp", "gcc-abc", "bin"))
    open(os.path.join(env["ProgramData"], "StarFoxRecomp", "gcc-abc", "bin", "clang++.exe"), "wb").write(b"c" * 3000)
    open(os.path.join(env["TEMP"], "recomp_textures_123.tmp"), "wb").write(b"t" * 500)
    return base, racine, env


def lancer(racine, env, *args):
    r = subprocess.run([sys.executable, "-X", "utf8", os.path.join(racine, "recomp", "launcher", "desinstaller.py"), *args],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, cwd=env["TEMP"])
    return r.returncode, r.stdout + r.stderr


def verifier(nom, conditions):
    for texte, ok in conditions:
        if not ok:
            echecs.append(f"{nom} : {texte}")
    print(f"{nom} : {sum(1 for _, ok in conditions if ok)}/{len(conditions)} conditions")


def existe(*p):
    return os.path.exists(os.path.join(*p))


# 1. liste
base, racine, env = fabriquer("liste", True)
code, sortie = lancer(racine, env, "--lister")
verifier("lister", [("code 0", code == 0), ("jeu listé", "built game" in sortie), ("copie signalée", "never deleted" in sortie),
                    ("rien supprimé", existe(racine, "recomp", "build", "full", "recomp_main.exe"))])

# 2. nettoyage : la place revient, le paquet, les réglages, la copie et les sauvegardes restent
base, racine, env = fabriquer("nettoyer", True)
code, sortie = lancer(racine, env, "--nettoyer", "--oui")
sauve = os.path.join(env["APPDATA"], "SpaceAnimalsRecomp", "save", "save_game_data.save")
verifier("nettoyer", [
    ("code 0", code == 0), ("@fini", "@fini cleaned" in sortie),
    ("jeu construit supprimé", not existe(racine, "recomp", "build", "full", "recomp_main.exe")),
    ("code généré supprimé", not existe(racine, "recomp", "out")),
    ("extraits supprimés", not existe(racine, "extracted") and not existe(racine, "work")),
    ("compilateur supprimé", not existe(env["ProgramData"], "StarFoxRecomp")),
    ("fichier temporaire supprimé", not existe(env["TEMP"], "recomp_textures_123.tmp")),
    ("réglages gardés", existe(racine, "recomp", "build", "full", "recomp.ini")),
    ("paquet gardé", existe(racine, "lanceur.exe") and existe(racine, "recomp", "fullbuild.py") and existe(racine, "outils", "python", "python.exe")),
    ("copie du jeu gardée", existe(racine, "Mon jeu [0400E88025B1A000][v131072].nup")),
    ("sauvegarde reprise dans APPDATA", os.path.exists(sauve) and os.path.getsize(sauve) == 34057),
    ("copie de secours faite", existe(env["APPDATA"], "SpaceAnimalsRecomp", "save_backups")),
])

# 3. désinstallation avec une copie du jeu dans le dossier : tout part sauf elle ; les sauvegardes restent
base, racine, env = fabriquer("avec_copie", True)
code, sortie = lancer(racine, env, "--desinstaller", "--oui")
time.sleep(4)   # la fin est faite par PowerShell après la sortie du script
sauve = os.path.join(env["APPDATA"], "SpaceAnimalsRecomp", "save", "save_game_data.save")
reste = sorted(os.listdir(racine)) if os.path.isdir(racine) else None
verifier("désinstaller avec copie", [
    ("code 0", code == 0), ("@fini", "@fini uninstalled" in sortie),
    ("il ne reste que la copie du jeu", reste == ["Mon jeu [0400E88025B1A000][v131072].nup"]),
    ("compilateur supprimé", not existe(env["ProgramData"], "StarFoxRecomp")),
    ("sauvegardes gardées", os.path.exists(sauve) and os.path.getsize(sauve) == 34057),
])
if reste != ["Mon jeu [0400E88025B1A000][v131072].nup"]:
    print("   reste :", reste, "\n" + sortie[-600:])

# 4. désinstallation complète, sauvegardes comprises, sans copie du jeu dans le dossier : plus rien
base, racine, env = fabriquer("tout", False)
os.makedirs(os.path.join(env["APPDATA"], "SpaceAnimalsRecomp", "save"))
open(os.path.join(env["APPDATA"], "SpaceAnimalsRecomp", "save", "save_game_data.save"), "wb").write(b"s" * 100)
code, sortie = lancer(racine, env, "--desinstaller", "--oui", "--sauvegardes")
time.sleep(4)
verifier("désinstaller tout", [
    ("code 0", code == 0),
    ("dossier d'installation supprimé", not os.path.exists(racine)),
    ("sauvegardes supprimées", not existe(env["APPDATA"], "SpaceAnimalsRecomp")),
    ("compilateur supprimé", not existe(env["ProgramData"], "StarFoxRecomp")),
])
if os.path.exists(racine):
    print("   reste :", [os.path.join(d, f) for d, _, fs in os.walk(racine) for f in fs][:8], "\n" + sortie[-600:])

# 5. refus dans un dossier de sources, et dans un dossier quelconque
base, racine, env = fabriquer("sources", True, sources=True)
code, sortie = lancer(racine, env, "--desinstaller", "--oui", "--sauvegardes")
verifier("refus dans des sources", [("code 2", code == 2), ("@erreur", "@erreur" in sortie),
                                    ("rien supprimé", existe(racine, "recomp", "build", "full", "recomp_main.exe")
                                     and existe(env["ProgramData"], "StarFoxRecomp"))])
code, sortie = lancer(racine, env, "--nettoyer", "--oui", "--racine", env["TEMP"])
verifier("refus hors installation", [("code 2", code == 2), ("fichier temporaire intact", existe(env["TEMP"], "recomp_textures_123.tmp"))])

# 6. sans --oui et sans réponse : rien n'est supprimé
base, racine, env = fabriquer("sans_reponse", True)
r = subprocess.run([sys.executable, "-X", "utf8", os.path.join(racine, "recomp", "launcher", "desinstaller.py"), "--desinstaller"],
                   capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, cwd=env["TEMP"], input="")
verifier("sans réponse", [("code 1", r.returncode == 1), ("rien supprimé", existe(racine, "lanceur.exe") and existe(racine, "recomp", "out"))])

print("ÉCHECS :" if echecs else "tout passe")
for e in echecs:
    print("  " + e)
sys.exit(1 if echecs else 0)
