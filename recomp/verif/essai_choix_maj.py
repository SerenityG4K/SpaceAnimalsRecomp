#!/usr/bin/env python3
"""Les mises à jour laissent-elles le choix au joueur ? Essai du launcher dans un dossier jetable.

  python essai_choix_maj.py <lanceur.exe à essayer> <copie .nup> [--garder]

Le dossier jetable est un faux paquet : faux maj.py (annonce la version 9.9.9, n'installe rien), faux preparer.py (ne
construit rien), faux jeu « construit par la version 1.0.2 » alors que le paquet est en 1.0.3. Le launcher, lui, est le
vrai, sans crochet d'essai : ses boîtes de dialogue s'ouvrent pour de bon et le script y répond.

  A  nouvelle version du launcher, jeu à refaire, réponse « non » : rien ne démarre, la question n'est pas reposée,
     la reconstruction reste proposée (elle n'est jamais imposée : le jeu actuel reste jouable)
  B  même situation, réponse « oui » : la mise à jour du jeu démarre
  C  mise à jour disponible, réponse « non » : rien n'est installé, refus retenu, la question n'est pas reposée
  D  mise à jour disponible, réponse « oui » : l'installation démarre
  E  les deux à la fois : une seule question à l'écran, l'autre attend son tour

La copie du jeu n'est lue que par le launcher (en-tête et index). Refus si un launcher ou le jeu est ouvert.
"""
import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
import time
from ctypes import wintypes

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

FAUX_MAJ = '''import os
import sys

racine = sys.argv[sys.argv.index("--racine") + 1] if "--racine" in sys.argv else os.getcwd()
if "--verifier" in sys.argv:
    print("@maj 9.9.9")
elif "--installer" in sys.argv:
    with open(os.path.join(racine, "essai_installer.txt"), "a") as f:
        f.write("installer\\n")
    print("essai : rien n'est installe")
    sys.exit(1)
'''
FAUX_PREPARER = '''import os
import sys

racine = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
if "--a-refaire" in sys.argv:
    print("@a_refaire lier")
else:
    with open(os.path.join(racine, "essai_construction.txt"), "a") as f:
        f.write("construction\\n")
'''

u = ctypes.WinDLL("user32", use_last_error=True)
ENUM = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
u.EnumWindows.argtypes = [ENUM, wintypes.LPARAM]
u.EnumChildWindows.argtypes = [wintypes.HWND, ENUM, wintypes.LPARAM]
u.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
u.GetClassNameW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
u.IsWindowVisible.argtypes = [wintypes.HWND]
u.GetDlgItem.argtypes = [wintypes.HWND, ctypes.c_int]
u.GetDlgItem.restype = wintypes.HWND
u.PostMessageW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM]
WM_CLOSE, WM_COMMAND, IDYES, IDNO = 0x0010, 0x0111, 6, 7


def classe(h):
    b = ctypes.create_unicode_buffer(128)
    u.GetClassNameW(h, b, 128)
    return b.value


def fenetres(pid):
    """Fenêtres visibles de premier niveau du processus : (poignée, classe)."""
    vues = []

    def une(h, _):
        p = wintypes.DWORD()
        u.GetWindowThreadProcessId(h, ctypes.byref(p))
        if p.value == pid and u.IsWindowVisible(h):
            vues.append((h, classe(h)))
        return True

    u.EnumWindows(ENUM(une), 0)
    return vues


def dialogues(pid):
    return [h for h, c in fenetres(pid) if c == "#32770"]


def texte(dialogue):
    morceaux = []

    def un(h, _):
        if classe(h) == "Static":
            b = ctypes.create_unicode_buffer(2048)
            u.GetWindowTextW(h, b, 2048)
            if b.value.strip():
                morceaux.append(b.value)
        return True

    u.EnumChildWindows(dialogue, ENUM(un), 0)
    return "\n".join(morceaux)


def attendre_dialogue(pid, delai):
    fin = time.time() + delai
    while time.time() < fin:
        d = dialogues(pid)
        if d:
            time.sleep(0.3)   # le texte est posé juste après la création
            return d[0]
        time.sleep(0.2)
    return None


def repondre(dialogue, oui):
    ident = IDYES if oui else IDNO
    u.PostMessageW(dialogue, WM_COMMAND, ident, u.GetDlgItem(dialogue, ident) or 0)
    fin = time.time() + 5
    while time.time() < fin and u.IsWindowVisible(dialogue):
        time.sleep(0.1)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if len(args) < 2:
        sys.exit(__doc__)
    exe, copie = os.path.abspath(args[0]), os.path.abspath(args[1])
    taches = subprocess.run(["tasklist"], capture_output=True, text=True, errors="replace").stdout.lower()
    if "lanceur" in taches or "recomp_main.exe" in taches:
        sys.exit("refus : un launcher ou le jeu est ouvert")
    travail = tempfile.mkdtemp(prefix="sar_choix_maj_")
    racine = os.path.join(travail, "paquet")
    jeu = os.path.join(racine, "recomp", "build", "full")
    os.makedirs(os.path.join(racine, "recomp", "launcher"))
    os.makedirs(os.path.join(jeu, "shader_cache"))
    os.makedirs(os.path.join(jeu, ".preparation"))
    # le launcher essayé, sous un autre nom : ce script ne voit et ne ferme jamais que le sien
    lanceur = os.path.join(racine, "lanceur_essai.exe")
    shutil.copy2(exe, lanceur)
    for chemin, contenu in ((("recomp", "fullbuild.py"), "# faux paquet\n"), (("recomp", "VERSION"), "1.0.3\n"),
                            (("recomp", "launcher", "maj.py"), FAUX_MAJ), (("recomp", "launcher", "preparer.py"), FAUX_PREPARER),
                            (("recomp", "build", "full", "recomp_main.exe"), ""), (("recomp", "build", "full", "manifest.txt"), ""),
                            (("recomp", "build", "full", ".preparation", "version.ok"), "1.0.2\n")):
        with open(os.path.join(racine, *chemin), "w", encoding="utf-8", newline="\n") as f:
            f.write(contenu)
    for i in range(1001):   # le launcher compte plus de 1 000 shaders pour dire le jeu construit
        open(os.path.join(jeu, "shader_cache", f"{i:04d}.spv"), "wb").close()
    ini, journal = os.path.join(racine, "lanceur.ini"), os.path.join(racine, "lanceur.log")
    m_cons, m_inst = os.path.join(racine, "essai_construction.txt"), os.path.join(racine, "essai_installer.txt")
    env = {k: v for k, v in os.environ.items() if not k.startswith("LANCEUR_")}
    for nom in ("APPDATA", "LOCALAPPDATA", "ProgramData"):   # jamais le vrai profil
        env[nom] = os.path.join(travail, nom)
        os.makedirs(env[nom])
    bilan = []

    def note(nom, ok, detail=""):
        bilan.append(ok)
        print(f"  {'ok ' if ok else 'NON'}  {nom}{' : ' + detail if detail else ''}")

    def ecrire_ini(**cles):
        with open(ini, "w", encoding="utf-16") as f:   # UTF-16 avec BOM, comme le launcher
            f.write("[lanceur]\n" + f"copie={copie}\n" + "".join(f"{k}={v}\n" for k, v in cles.items()))

    def lire(chemin, codage="utf-8"):
        return open(chemin, encoding=codage, errors="replace").read() if os.path.exists(chemin) else ""

    def ouvrir(titre):
        print(f"--- {titre}")
        for f in (journal, m_cons, m_inst):
            if os.path.exists(f):
                os.remove(f)
        return subprocess.Popen([lanceur], cwd=racine, env=env)

    def fermer(p):
        for h, c in fenetres(p.pid):
            if c != "#32770":
                u.PostMessageW(h, WM_CLOSE, 0, 0)
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()   # le launcher de cet essai, jamais un autre
            note("fermeture du launcher", False, "il a fallu l'arrêter")

    def question(p, delai, avec):
        """Attend une question ; `avec` : texte attendu dedans (None : la question sur le jeu, sans numéro de version)."""
        d = attendre_dialogue(p.pid, delai)
        t = texte(d) if d else ""
        if d:
            print("      « " + t.replace("\n", " ⏎ ")[:230] + " »")
        bonne = bool(d) and (("9.9.9" in t) if avec else ("9.9.9" not in t))
        return d, bonne

    try:
        # A : nouvelle version du launcher, jeu à refaire, « non »
        ecrire_ini(maj_auto=0)
        p = ouvrir("A  jeu à refaire après une nouvelle version, réponse « non »")
        d, bonne = question(p, 25, False)
        note("la question sur la mise à jour du jeu s'affiche", bonne)
        if d:
            repondre(d, False)
        time.sleep(3)
        note("rien ne démarre", not os.path.exists(m_cons) and "build:" not in lire(journal))
        note("refus dans le journal", "game update postponed by the player" in lire(journal))
        note("version notée comme vue", "version_vue=1.0.3" in lire(ini, "utf-16"))
        note("pas d'autre question", not dialogues(p.pid))
        fermer(p)
        p = ouvrir("A' réouverture")
        note("la question n'est pas reposée", attendre_dialogue(p.pid, 7) is None)
        note("la reconstruction reste proposée", "rebuild offered" in lire(journal))
        note("rien ne démarre", not os.path.exists(m_cons))
        fermer(p)

        # B : même situation, « oui »
        ecrire_ini(maj_auto=0)
        p = ouvrir("B  jeu à refaire après une nouvelle version, réponse « oui »")
        d, bonne = question(p, 25, False)
        note("la question sur la mise à jour du jeu s'affiche", bonne)
        if d:
            repondre(d, True)
        fin = time.time() + 20
        while time.time() < fin and not os.path.exists(m_cons):
            time.sleep(0.3)
        note("la mise à jour du jeu démarre", os.path.exists(m_cons))
        note("accord dans le journal", "game update accepted by the player" in lire(journal))
        time.sleep(2)
        fermer(p)

        # C : mise à jour disponible, « non »
        ecrire_ini(maj_auto=1, version_vue="1.0.3")
        p = ouvrir("C  mise à jour 9.9.9 disponible, réponse « non »")
        d, bonne = question(p, 30, True)
        note("la question sur la nouvelle version s'affiche", bonne)
        if d:
            repondre(d, False)
        time.sleep(3)
        note("rien n'est installé", not os.path.exists(m_inst) and "update: installing" not in lire(journal))
        note("refus retenu pour cette version", "maj_refusee=9.9.9" in lire(ini, "utf-16"))
        note("refus dans le journal", "update 9.9.9 declined" in lire(journal))
        fermer(p)
        p = ouvrir("C' réouverture")
        note("la question n'est pas reposée", attendre_dialogue(p.pid, 9) is None)
        note("la version reste proposée (bandeau)", "update available: 9.9.9" in lire(journal))
        note("rien n'est installé", not os.path.exists(m_inst))
        fermer(p)

        # D : mise à jour disponible, « oui »
        ecrire_ini(maj_auto=1, version_vue="1.0.3")
        p = ouvrir("D  mise à jour 9.9.9 disponible, réponse « oui »")
        d, bonne = question(p, 30, True)
        note("la question sur la nouvelle version s'affiche", bonne)
        if d:
            repondre(d, True)
        fin = time.time() + 20
        while time.time() < fin and not os.path.exists(m_inst):
            time.sleep(0.3)
        note("l'installation démarre", os.path.exists(m_inst) and "update: installing 9.9.9" in lire(journal))
        time.sleep(2)
        fermer(p)

        # E : les deux à la fois
        ecrire_ini(maj_auto=1)
        p = ouvrir("E  jeu à refaire et mise à jour disponible en même temps")
        d, bonne = question(p, 25, False)
        note("d'abord la question sur le jeu", bonne)
        fin = time.time() + 20
        while time.time() < fin and "update available" not in lire(journal):
            time.sleep(0.3)
        time.sleep(4)   # la seconde question aurait eu le temps de s'ouvrir par-dessus
        note("une seule question à l'écran", len(dialogues(p.pid)) == 1, f"{len(dialogues(p.pid))} à l'écran")
        if d:
            repondre(d, False)
        d2, bonne = question(p, 10, True)
        note("puis la question sur la nouvelle version", bonne)
        if d2:
            repondre(d2, False)
        time.sleep(2)
        note("rien ne démarre, rien n'est installé", not os.path.exists(m_cons) and not os.path.exists(m_inst))
        fermer(p)
    finally:
        if "--garder" in sys.argv:
            print("dossier gardé :", travail)
        else:
            time.sleep(1)
            shutil.rmtree(travail, ignore_errors=True)
    print(f"{sum(bilan)} contrôles sur {len(bilan)} passés")
    sys.exit(0 if all(bilan) else 1)


if __name__ == "__main__":
    main()
