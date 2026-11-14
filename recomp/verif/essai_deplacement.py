"""Essai de deplacer.py (emplacement du jeu, issue GitHub n° 14) sur de fausses installations, avec un TEMP d'essai :
rien de réel n'est touché.   python essai_deplacement.py <recomp/launcher de développement>
<dossier de travail>

Le déplacement vers un autre volume est essayé sur le même disque (DEPLACER_FORCER_COPIE=1 : même chemin de code, seul
le choix « même volume ou non » diffère). Le faux lanceur.exe n'est pas un programme : sa relance échoue, c'est le
journal du déplacement qui dit où elle a été tentée."""
import ctypes
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
COPIE = "Mon jeu [0400E88025B1A000][v131072].nup"


def fabriquer(nom, avec_copie=False, sources=False):
    base = os.path.join(travail, nom)
    shutil.rmtree(base, ignore_errors=True)
    racine = os.path.join(base, "ancien [x] y", "SpaceAnimalsRecomp-9.9.9")
    for d in ("recomp/launcher/extraits", "recomp/build/full/shader_cache", "recomp/out/main", "recomp/runtime/os",
              "extracted/patch/exefs", "work", "outils/python", "tools", "LICENCES", "recomp/build/vide"):
        os.makedirs(os.path.join(racine, d))
    fichiers = {"lanceur.exe": 2000, "lanceur.ini": 60, "LISEZMOI.txt": 100, "recomp/fullbuild.py": 100, "recomp/VERSION": 6,
                "recomp/launcher/maj.py": 100, "recomp/build/full/recomp_main.exe": 5000, "recomp/build/full/recomp.ini": 300,
                "recomp/build/full/shader_cache/a.spv": 100, "recomp/out/main/lot_000.cpp": 9000,
                "extracted/patch/exefs/main": 4000, "work/main.img": 20 << 20, "outils/python/python.exe": 100, "tools/nup.py": 50}
    for i, (f, n) in enumerate(fichiers.items()):
        p = os.path.join(racine, f)
        open(p, "wb").write(bytes([65 + i]) * n)
        os.utime(p, (1_700_000_000 + i * 1000, 1_700_000_000 + i * 1000))   # dates anciennes : gardées par la copie ?
    for f in ("deplacer.py", "desinstaller.py", "sauvegardes.py"):
        shutil.copy2(os.path.join(dev, f), os.path.join(racine, "recomp", "launcher", f))
    if avec_copie:
        open(os.path.join(racine, COPIE), "wb").write(b"n" * 1000)
    if sources:
        open(os.path.join(racine, "recomp", "launcher", "lanceur.cpp"), "w").write("// sources")
    env = dict(os.environ)
    for cle, d in (("TEMP", "temp"), ("TMP", "temp")):
        env[cle] = os.path.join(base, d)
        os.makedirs(env[cle], exist_ok=True)
    for cle in ("DEPLACER_FORCER_COPIE", "LANCEUR_ACTION", "LANCEUR_STATUT"):
        env.pop(cle, None)
    return base, racine, env


def lancer(script_dans, env, *args):
    r = subprocess.run([sys.executable, "-X", "utf8", os.path.join(script_dans, "recomp", "launcher", "deplacer.py"), *args],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", env=env, cwd=env["TEMP"])
    return r.returncode, r.stdout + r.stderr


def contenu(racine, sans=()):
    """{chemin relatif: (taille, date)} des fichiers, et l'ensemble des dossiers."""
    f, d = {}, set()
    for base, sous, noms in os.walk(racine):
        rel = os.path.relpath(base, racine)
        if rel != ".":
            d.add(rel)
        for n in noms:
            if n in sans:
                continue
            p = os.path.join(base, n)
            f[os.path.normpath(os.path.join(rel, n))] = (os.path.getsize(p), int(os.path.getmtime(p)))
    return f, d


def journal(env):
    try:
        return open(os.path.join(env["TEMP"], "SpaceAnimalsRecomp-move.log"), encoding="utf-8", errors="replace").read()
    except OSError:
        return ""


def attendre(condition, secondes=40):
    fin = time.time() + secondes
    while time.time() < fin:
        if condition():
            return True
        time.sleep(0.25)
    return condition()


def verifier(nom, conditions, detail=""):
    rates = [texte for texte, ok in conditions if not ok]
    for texte in rates:
        echecs.append(f"{nom} : {texte}")
    print(f"{nom} : {len(conditions) - len(rates)}/{len(conditions)} conditions")
    if rates and detail:
        print("   " + detail.strip()[-700:].replace("\n", "\n   "))


def existe(*p):
    return os.path.exists(os.path.join(*p))


# 1. contrôles rapides : destination et volume, rien n'est laissé sur le disque
base, racine, env = fabriquer("verifier")
vide, plein = os.path.join(base, "nouveau", "Jeux SF"), os.path.join(base, "plein")
os.makedirs(plein)
open(os.path.join(plein, "autre.txt"), "w").write("x")
c1, s1 = lancer(racine, env, "--verifier", "--racine", racine, "--vers", vide)
c2, s2 = lancer(racine, env, "--verifier", "--racine", racine, "--vers", plein)
verifier("vérifier", [
    ("dossier absent : pris tel quel", c1 == 0 and f"@destination {vide}" in s1),
    ("même volume", "@volume meme" in s1),
    ("rien créé", not existe(base, "nouveau")),
    ("dossier non vide : sous-dossier SpaceAnimalsRecomp", c2 == 0 and "@destination " + os.path.join(plein, "SpaceAnimalsRecomp") in s2),
    ("rien créé dans le dossier non vide", os.listdir(plein) == ["autre.txt"]),
], s1 + s2)

# 2. refus
os.makedirs(os.path.join(plein, "SpaceAnimalsRecomp"))
open(os.path.join(plein, "SpaceAnimalsRecomp", "deja.txt"), "w").write("x")
c1, s1 = lancer(racine, env, "--verifier", "--racine", racine, "--vers", plein)
c2, s2 = lancer(racine, env, "--verifier", "--racine", racine, "--vers", os.path.join(racine, "recomp"))
# Program Files : le vrai (Windows redonne cette variable à chaque processus), refusé avant tout essai d'écriture
programmes = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "SpaceAnimalsRecomp essai de refus")
c3, s3 = lancer(racine, env, "--verifier", "--racine", racine, "--vers", programmes)
c4, s4 = lancer(racine, env, "--verifier", "--racine", racine, "--vers", os.path.dirname(racine))
_, racine_s, env_s = fabriquer("sources", sources=True)
c5, s5 = lancer(racine_s, env_s, "--verifier", "--racine", racine_s, "--vers", os.path.join(base, "ailleurs"))
c6, s6 = lancer(racine, env, "--copier", "--racine", racine, "--vers", os.path.join(base, "l" * 120, "m" * 120))
c7, s7 = lancer(racine, env, "--verifier", "--racine", racine, "--vers", os.path.join(base, "ゲーム"))
verifier("refus", [
    ("dossier déjà occupé", c1 == 1 and "already exists" in s1),
    ("dans l'installation", c2 == 1 and "inside the current one" in s2),
    ("Program Files", c3 == 1 and "administrator rights" in s3 and not os.path.exists(programmes)),
    ("dossier parent : sous-dossier SpaceAnimalsRecomp accepté", c4 == 0 and "@destination" in s4),
    ("dossier de sources", c5 == 1 and "sources" in s5 and "does not move" in s5),
    ("chemin trop long", c6 == 1 and "too long" in s6 and not existe(base, "l" * 120)),
    ("caractères que le compilateur ne gère pas", c7 == 1 and "cannot handle" in s7 and not existe(base, "ゲーム")),
    ("installation intacte", existe(racine, "work", "main.img") and existe(racine, "lanceur.exe")),
], s1 + s2 + s3 + s4 + s5 + s6)

# 3. même volume : rien à copier, puis le dossier est renommé par PowerShell (la copie du jeu suit)
base, racine, env = fabriquer("meme_volume", avec_copie=True)
dest = os.path.join(base, "nouveau [z]", "Jeu d'essai")
avant = contenu(racine)
c1, s1 = lancer(racine, env, "--copier", "--racine", racine, "--vers", dest)
rien = not os.path.exists(dest) and existe(racine, "lanceur.exe")
c2, s2 = lancer(racine, env, "--finir", "--pid", "0", "--racine", racine, "--vers", dest)
attendre(lambda: existe(dest, "lanceur.exe") and "folder renamed" in journal(env))
verifier("même volume", [
    ("copie : code 0, rien à copier", c1 == 0 and "@ok dep_copier" in s1 and "nothing to copy" in s1 and rien),
    ("fin : code 0", c2 == 0),
    ("ancien dossier disparu", not os.path.exists(racine)),
    ("mêmes fichiers, tailles et dates, copie du jeu comprise", os.path.isdir(dest) and contenu(dest) == avant),
    ("journal : dossier renommé", "folder renamed" in journal(env)),
], s1 + s2 + journal(env))

# 4. autre volume (copie forcée), avec une copie du jeu dans le dossier : elle reste où elle est
base, racine, env = fabriquer("autre_volume", avec_copie=True)
env["DEPLACER_FORCER_COPIE"] = "1"
dest = os.path.join(base, "nouveau [z]", "Jeu d'essai")
avant = contenu(racine, sans=(COPIE,))
cv, sv = lancer(racine, env, "--verifier", "--racine", racine, "--vers", dest)
c1, s1 = lancer(racine, env, "--copier", "--racine", racine, "--vers", dest)
apres_copie = contenu(dest, sans=(".deplacement_pret",)) if os.path.isdir(dest) else None
intacte = contenu(racine, sans=(COPIE,)) == avant and existe(racine, COPIE)
# le launcher écrit encore en se fermant : ce fichier doit être recopié par --finir
open(os.path.join(racine, "lanceur.ini"), "wb").write(b"[lanceur]\r\ncopie=modifie apres la copie\r\n")
c2, s2 = lancer(dest, env, "--finir", "--pid", "0", "--racine", racine, "--vers", dest)   # lancé du nouveau dossier
ini = open(os.path.join(dest, "lanceur.ini"), "rb").read() if existe(dest, "lanceur.ini") else b""
verifier("autre volume, avec copie du jeu", [
    ("vérification : autre volume", cv == 0 and "@volume autre" in sv),
    ("copie : code 0", c1 == 0 and "@ok dep_copier" in s1),
    ("copie : mêmes fichiers, tailles, dates et dossiers, sans la copie du jeu", apres_copie == avant),
    ("copie : marque « prêt »", "@fini" in s1 and apres_copie is not None),
    ("copie : installation d'origine intacte", intacte),
    ("fin : code 0", c2 == 0),
    ("fin : fichier modifié recopié", b"modifie apres la copie" in ini),
    ("fin : marque retirée", not existe(dest, ".deplacement_pret")),
    ("fin : de l'ancien dossier il ne reste que la copie du jeu", os.path.isdir(racine) and os.listdir(racine) == [COPIE]),
    ("fin : copie du jeu intacte", existe(racine, COPIE) and os.path.getsize(os.path.join(racine, COPIE)) == 1000),
    ("journal : relance dans le nouveau dossier, copie restée", f"launcher restarted in {dest} (dep_fini_copie)" in journal(env)),
], s1 + s2 + journal(env))

# 5. autre volume, sans copie du jeu : l'ancien dossier disparaît
base, racine, env = fabriquer("autre_volume_sans_copie")
env["DEPLACER_FORCER_COPIE"] = "1"
dest = os.path.join(base, "plein")
os.makedirs(dest)
open(os.path.join(dest, "autre.txt"), "w").write("x")
dest = os.path.join(dest, "SpaceAnimalsRecomp")
avant = contenu(racine)
c1, s1 = lancer(racine, env, "--copier", "--racine", racine, "--vers", dest)
c2, s2 = lancer(dest, env, "--finir", "--pid", "0", "--racine", racine, "--vers", dest)
verifier("autre volume, sans copie du jeu", [
    ("codes 0", c1 == 0 and c2 == 0),
    ("ancien dossier supprimé", not os.path.exists(racine)),
    ("tout est dans le nouveau", os.path.isdir(dest) and contenu(dest) == avant),
    ("le fichier voisin est intact", existe(base, "plein", "autre.txt")),
    ("journal : relance dans le nouveau dossier", f"launcher restarted in {dest} (dep_fini)" in journal(env)),
], s1 + s2 + journal(env))

# 6. copie interrompue : seul un dossier marqué est retiré ; jamais le dossier d'où le script tourne
base, racine, env = fabriquer("abandon")
commence, autre = os.path.join(base, "commence"), os.path.join(base, "autre")
for d in (commence, autre):
    os.makedirs(os.path.join(d, "recomp"))
    open(os.path.join(d, "recomp", "x.bin"), "wb").write(b"x" * 100)
open(os.path.join(commence, ".deplacement_en_cours"), "w").write(racine)
open(os.path.join(racine, ".deplacement_pret"), "w").write("piège")
c1, s1 = lancer(racine, env, "--abandonner", "--vers", commence)
c2, s2 = lancer(racine, env, "--abandonner", "--vers", autre)
c3, s3 = lancer(racine, env, "--abandonner", "--vers", racine)
verifier("abandon", [
    ("dossier marqué retiré", c1 == 0 and not os.path.exists(commence)),
    ("dossier sans marque intact", c2 == 0 and existe(autre, "recomp", "x.bin")),
    ("dossier du script jamais retiré", c3 == 1 and existe(racine, "lanceur.exe") and existe(racine, "work", "main.img")),
], s1 + s2 + s3 + journal(env))
# une copie restée en plan est remplacée par la suivante
os.remove(os.path.join(racine, ".deplacement_pret"))
env["DEPLACER_FORCER_COPIE"] = "1"
dest = os.path.join(base, "repris")
os.makedirs(os.path.join(dest, "vieux"))
open(os.path.join(dest, "vieux", "reste.bin"), "wb").write(b"r" * 10)
open(os.path.join(dest, ".deplacement_en_cours"), "w").write(racine)
c1, s1 = lancer(racine, env, "--copier", "--racine", racine, "--vers", dest)
verifier("copie reprise", [("code 0", c1 == 0), ("reste de l'ancienne copie retiré", not existe(dest, "vieux")),
                           ("copie complète", existe(dest, "work", "main.img") and existe(dest, ".deplacement_pret"))], s1)

# 7. le jeu tourne (exécutable tenu) : rien n'est déplacé, le launcher est relancé à l'ancien endroit
base, racine, env = fabriquer("jeu_en_marche")
dest = os.path.join(base, "nouveau")
k32 = ctypes.windll.kernel32
k32.CreateFileW.restype = ctypes.c_void_p
h = k32.CreateFileW(os.path.join(racine, "recomp", "build", "full", "recomp_main.exe"), 0x80000000, 0, None, 3, 0, None)
c1, s1 = lancer(racine, env, "--finir", "--pid", "0", "--racine", racine, "--vers", dest)
k32.CloseHandle(ctypes.c_void_p(h))
verifier("jeu en marche", [
    ("code 1", c1 == 1),
    ("rien déplacé", existe(racine, "lanceur.exe") and not os.path.exists(dest)),
    ("journal : relance à l'ancien endroit", "is running: nothing moved" in journal(env)
     and f"launcher restarted in {racine} (dep_echec)" in journal(env)),
], s1 + journal(env))

print("ÉCHECS :" if echecs else "tout passe")
for e in echecs:
    print("  " + e)
sys.exit(1 if echecs else 0)
