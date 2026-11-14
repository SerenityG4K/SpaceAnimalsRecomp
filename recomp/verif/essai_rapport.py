"""Essai de l'anonymisation du rapport de bug (launcher/rapport.py), sur une fausse installation.

  python essai_rapport.py <dossier launcher des sources> <dossier de travail>

Fausse installation (dossier à espaces et parenthèses) : rapport.py copié, lanceur.ini en UTF-16 comme celui du
launcher (clé « copie »), journaux qui contiennent le chemin de la copie (passé à preparer.py, retrouvé par le jeu, une
ancienne copie du manifeste), le profil Windows, un autre profil, le nom d'utilisateur et du PC, des chemins sur
d'autres lecteurs, et des chemins de Windows et de ProgramData qui doivent rester. Le rapport est fait avec un profil,
un nom d'utilisateur et un nom de PC fictifs ; aucun des noms privés ne doit rester dans le zip, et lanceur.ini doit y
être lisible (pas de caractères nuls). Imprime « tout passe » si c'est le cas.
"""
import os
import shutil
import subprocess
import sys
import zipfile

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
launcher, travail = os.path.abspath(sys.argv[1]), os.path.abspath(sys.argv[2])
shutil.rmtree(travail, ignore_errors=True)
racine = os.path.join(travail, "Mon Jeu (Perso)", "SpaceAnimalsRecomp")
full = os.path.join(racine, "recomp", "build", "full")
os.makedirs(os.path.join(racine, "recomp", "launcher"))
os.makedirs(full)
shutil.copy2(os.path.join(launcher, "rapport.py"), os.path.join(racine, "recomp", "launcher", "rapport.py"))
with open(os.path.join(racine, "recomp", "VERSION"), "w", encoding="utf-8") as f:
    f.write("9.9.9\n")

# chemins de profil écrits en morceaux : depot.py refuse tout chemin de profil Windows littéral dans le dépôt
USERS = "C:" + "\\" + "Us" + "ers"
PROFIL = USERS + r"\JeanTestNom"
COPIE = r"E:\Jeux Perso\Dumps Switch\Star_Fox_Mon_Dump_Secret.nup"
ANCIENNE = r"D:\Ancien Dossier Secret\vieux_dump_prive.nup"
SECRETS = ["JeanTestNom", "PC-SECRET-42", "Jeux Perso", "Dumps Switch", "Star_Fox_Mon_Dump_Secret", "Ancien Dossier",
           "vieux_dump_prive", "Outils Perso Secrets", "AutreSecret", "Projets Secrets", "Mon Jeu (Perso)",
           "Dossier Musique Prive", "copie_dans_le_jeu", "Dossier Deplace Secret"]
GARDES = [r"C:\Windows\System32\DriverStore\FileRepository\nvmdi.inf_amd64_x\_nvngx.dll",
          r"C:\ProgramData\StarFoxRecomp\gcc-1\bin\g++.exe", r"C:\Program Files (x86)\Steam\steam.exe",
          "<install>", "<copy>", "%USERPROFILE%", "Game copy: <copy> (drive E: (not the install's drive)",
          "langue=fr", "(2 file(s))", "F:\\<path>.dll"]

# lanceur.ini : UTF-16 avec BOM, comme WritePrivateProfileStringW
with open(os.path.join(racine, "lanceur.ini"), "wb") as f:
    # dossier et deplacement_vers : écrits par le launcher depuis la 1.1.0 (emplacement du jeu)
    f.write("\ufeff\r\n[lanceur]\r\nlangue=fr\r\ncopie={}\r\nvolume=100\r\ndossier={}\r\ndeplacement_vers={}\r\n".format(
        COPIE, racine, r"G:\Dossier Deplace Secret\SpaceAnimalsRecomp").encode("utf-16-le"))
with open(os.path.join(racine, "lanceur.log"), "w", encoding="utf-8") as f:
    f.write(f'2026-11-11 03:41:17  construction : "{racine}\\recomp\\outils\\python\\python.exe" -u -X utf8 '
            f'"{racine}\\recomp\\launcher\\preparer.py" --copie "{COPIE}" --fils 16\n'
            f'2026-11-11 03:41:18  copie choisie : {COPIE.replace(chr(92), "/")}\n'
            f"2026-11-11 03:41:19  profil : {PROFIL}\\AppData\\Local\\Temp\\x.tmp ; PC PC-SECRET-42, user JeanTestNom\n")
with open(os.path.join(full, "recomp_run.log"), "w", encoding="utf-8") as f:
    f.write(f"[    0.1] [loader] copy found: {COPIE} (the manifest says {ANCIENNE})\n"
            r"[    1.0] [dlss  ] NVIDIA NGX core: C:\Windows\System32\DriverStore\FileRepository\nvmdi.inf_amd64_x\_nvngx.dll;"
            r" nvngx_dlss.dll in F:\Outils Perso Secrets\dlss\nvngx_dlss.dll" "\n"
            f"[    2.0] [fs    ] saves: {PROFIL}\\AppData\\Roaming\\SpaceAnimalsRecomp\\save (2 file(s))\n"
            f"[    2.1] [fs    ] other profile: {USERS}\\AutreSecret\\Documents\\notes.txt\n"
            r"[    2.2] [vk    ] compiler: C:\ProgramData\StarFoxRecomp\gcc-1\bin\g++.exe, Steam: C:\Program Files (x86)\Steam\steam.exe" "\n"
            "[    2.3] [vk    ] cache: G:/Projets Secrets/truc/fichier.log, music: H:\\Dossier Musique Prive\n"
            f"[    2.4] [fs    ] romfs: {racine}\\copie_dans_le_jeu.nup ignored\n")
with open(os.path.join(full, "preparation.log"), "w", encoding="utf-8") as f:
    f.write(f"copie : {COPIE}\nfichier Star_Fox_Mon_Dump_Secret.nup vérifié\n")
with open(os.path.join(full, "recomp.ini"), "w", encoding="utf-8") as f:
    f.write("[recomp]\nresolution = auto\n")

env = dict(os.environ, USERPROFILE=PROFIL, USERNAME="JeanTestNom", COMPUTERNAME="PC-SECRET-42", USERDOMAIN="PC-SECRET-42")
env.pop("RECOMP_NUP", None)
sortie = os.path.join(travail, "rapport.zip")
r = subprocess.run([sys.executable, "-X", "utf8", os.path.join(racine, "recomp", "launcher", "rapport.py"), "--sortie",
                    sortie], cwd=racine, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
                   timeout=120)
echecs = []
if r.returncode or not os.path.exists(sortie):
    print(r.stdout[-3000:], r.stderr[-3000:])
    sys.exit("rapport.py a échoué")
textes = {}
with zipfile.ZipFile(sortie) as z:
    for n in z.namelist():
        textes[n] = z.read(n).decode("utf-8", errors="replace")
tout = "\n".join(textes.values())
for s in SECRETS:
    for n, t in textes.items():
        if s.lower() in t.lower():
            ligne = next(l for l in t.splitlines() if s.lower() in l.lower())
            echecs.append(f"« {s} » reste dans {n} : {ligne.strip()[:160]}")
for s in GARDES:
    if s not in tout:
        echecs.append(f"« {s} » manque (trop masqué)")
for n, t in textes.items():
    if "\x00" in t:
        echecs.append(f"caractères nuls dans {n} (fichier UTF-16 mal lu)")
for e in echecs:
    print("  ÉCHEC", e)
print(f"{len(textes)} fichiers dans le zip, {len(SECRETS)} noms privés cherchés, {len(GARDES)} morceaux à garder")
if echecs:
    print("\n--- extrait du zip ---")
    for n in ("logs/lanceur.log", "logs/recomp_run.log", "settings/lanceur.ini"):
        print(f"[{n}]\n{textes.get(n, '(absent)')}")
    sys.exit(1)
shutil.rmtree(travail, ignore_errors=True)
print("tout passe")
