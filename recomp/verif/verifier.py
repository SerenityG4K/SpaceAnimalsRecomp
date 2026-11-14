#!/usr/bin/env python3
"""Vérifications avant chaque version de SpaceAnimalsRecomp : rien ne doit casser, ni chez nous ni chez les joueurs.

  python recomp/verif/verifier.py rapide  [--travail D] [--copie X.nup] [--garder]     ≈ 5 min, jeu non reconstruit
  python recomp/verif/verifier.py complet [--travail D] [--copie X.nup] [--garder]     + installation neuve (≈ 50 min)
  python recomp/verif/verifier.py rapide --seulement versions,paquet                   quelques vérifications

rapide
  versions    VERSION, lanceur.h, lanceur.rc, lanceur.exe (ressource, date), CHANGELOG, README : même version
  python      tous les .py du projet se compilent
  runtime     runtime C++ compilé et jeu lié (link.py), exécutable d'essai hors de build/full
  sauvegardes sauvegardes du joueur : reprise de l'ancien dossier vers %APPDATA% (recomp_main --save-dir), sans y toucher ;
              copies de secours (sauvegardes.py) identiques, sans doublon, bornées. Dossiers d'essai seulement.
  launcher    launcher compilé (construire.py --out), hors de la racine ; démarré sur un écran simulé de 360 Hz : image
              présentée, aucun échec d'affichage dans son journal (issue n° 12) ; mises à jour au choix du joueur :
              questions « nouvelle version » et « jeu à refaire », réponses oui et non, jamais deux à la fois
              (verif/essai_choix_maj.py, faux paquet jetable)
  desinstaller  désinstallation (desinstaller.py) sur de fausses installations, APPDATA, ProgramData et TEMP d'essai :
              nettoyage, désinstallation avec et sans sauvegardes, copie du jeu du joueur jamais supprimée, refus dans
              un dossier de sources (verif/essai_desinstaller.py)
  rapport     anonymisation du rapport de bug (rapport.py) sur une fausse installation : chemin de la copie, profils,
              noms d'utilisateur et de PC, autres lecteurs absents du zip ; lanceur.ini UTF-16 lisible
              (verif/essai_rapport.py)
  deplacement déplacement de l'installation (deplacer.py, issue n° 14) sur de fausses installations : même volume
              (renommage), autre volume (copie forcée, copie du jeu laissée en place), refus, copie interrompue, jeu
              en marche (verif/essai_deplacement.py)
  depot       export du dépôt public (depot.py : fichiers interdits, traces personnelles)
  paquet      paquet complet et mise à jour (paquet.py) : licences, extraits, outils allégés, sources à jour, rien du jeu
  outils      GCC allégé du paquet : fichiers utilisés (verif/gcc_utilises.txt) présents, DLL importées présentes
  maj         mise à jour depuis la dernière release publiée, avec SON maj.py (seul GitHub est simulé) : fichiers,
              outils intacts, étapes que « RECONSTRUIRE MAINTENANT » refera
  precompile  plantage simulé du pilote sur un shader : isolé, écarté, précompilation finie (issue n° 1)
  deplace     jeu lancé depuis une installation déplacée (manifeste périmé) : images et copie retrouvées (issue n° 4)
complet, en plus
  neuve       paquet installé dans un dossier AVEC des espaces et des crochets (copie du compilateur dans un ProgramData
              d'essai ; motifs de fichiers), PATH
              sans MSYS2 ni autre compilateur, construction complète par preparer.py (étapes du bouton du launcher)
  regeneration  la recompilation relancée sur l'installation neuve, sans rien changer : tout le code est régénéré et
              aucun objet n'est recompilé (découverte des fonctions reproductible, cache des objets), en moins de 5 min
  shaders     2 388 shaders ; GLSL et .meta identiques à ceux de l'arbre de référence (build/full)
  jeu         jeu de l'installation neuve, 150 s : aucun shader inconnu ni absent, aucun dessin ignoré, pas de plantage
  sans_gpl    le même sans VK_EXT_graphics_pipeline_library (RECOMP_NO_GPL : GPU qui ne l'ont pas), 60 s
  lanceur     launcher de l'installation neuve : il démarre, se capture et se ferme (JOUER n'est jamais déclenché)

Les essais du jeu ont leur propre sauvegarde et leur propre journal. Ils ne commencent pas si le jeu ou le launcher est
ouvert, et s'arrêtent si le joueur lance le jeu. Tout ce qui dérive du jeu reste dans le dossier de travail et en est
supprimé à la fin (sauf --garder). Le rapport va dans verif/rapports/ (privé : jamais exporté). Code de sortie 1 si une
vérification échoue.
"""
import argparse
import ctypes
import glob
import hashlib
import importlib.util
import io
import json
import os
import py_compile
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request
import zipfile

ICI = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(ICI)
PROJET = os.path.dirname(RECOMP)
LANCEUR = os.path.join(RECOMP, "launcher")
FULL = os.path.join(RECOMP, "build", "full")
DEPOT_GITHUB = "SerenityG4K/SpaceAnimalsRecomp"
NOM = "SpaceAnimalsRecomp"
RAPIDES = ["versions", "python", "runtime", "sauvegardes", "launcher", "desinstaller", "rapport", "deplacement", "depot",
           "paquet", "outils", "maj", "precompile", "deplace"]
COMPLETES = ["neuve", "regeneration", "shaders", "jeu", "sans_gpl", "lanceur"]


class Echec(Exception):
    pass


class Saute(Exception):
    pass


# ---------------------------------------------------------------------------------------------- outils communs
def dire(t):
    print(t, flush=True)


def version():
    return open(os.path.join(RECOMP, "VERSION"), encoding="utf-8").read().strip()


def empreinte(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


def lancer(cmd, cwd=None, env=None, timeout=None):
    """Commande ; rend (code, sortie) ; la sortie est aussi gardée pour le rapport en cas d'échec."""
    r = subprocess.run(cmd, cwd=cwd, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
                       timeout=timeout)
    return r.returncode, (r.stdout or "") + (r.stderr or "")


def fin(texte, n=15):
    return "\n".join(texte.strip().splitlines()[-n:])


def processus(nom):
    try:
        r = subprocess.run(["tasklist", "/FI", f"IMAGENAME eq {nom}", "/FO", "CSV", "/NH"], capture_output=True,
                           text=True, errors="replace")
    except OSError:
        return set()
    return {int(l.split('","')[1]) for l in r.stdout.splitlines() if l.startswith('"') and nom.lower() in l.lower()}


def memoire_validable_libre():
    """Go validables libres (RAM + fichier d'échange) : ce que Windows peut encore réserver."""
    class Etat(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    e = Etat()
    e.dwLength = ctypes.sizeof(Etat)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(e))
    return e.ullAvailPageFile / 1e9


def gros_processus(n=3):
    """Les n processus qui occupent le plus de mémoire (nom et Mo), pour expliquer un manque de mémoire."""
    try:
        r = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True, errors="replace")
    except OSError:
        return "?"
    procs = []
    for l in r.stdout.splitlines():
        c = l.strip('"').split('","')
        if len(c) >= 5:
            try:
                procs.append((int(re.sub(r"\D", "", c[4])) // 1024, c[0]))
            except ValueError:
                pass
    return ", ".join(f"{nom} {mo} Mo" for mo, nom in sorted(procs, reverse=True)[:n])


def vulkan():
    return os.path.exists(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "vulkan-1.dll"))


def charger(chemin, nom):
    """Module Python d'un autre arbre (installation d'essai), sans toucher sys.modules de façon durable."""
    spec = importlib.util.spec_from_file_location(nom, chemin)
    m = importlib.util.module_from_spec(spec)
    sys.path.insert(0, os.path.dirname(chemin))
    try:
        spec.loader.exec_module(m)
    finally:
        sys.path.pop(0)
    return m


def arbre(racine, sans=()):
    """{chemin relatif : SHA-256} des fichiers, sans les dossiers de premier niveau `sans` ni les caches Python."""
    out = {}
    for r, ds, fs in os.walk(racine):
        if r == racine:
            ds[:] = [d for d in ds if d not in sans]
        ds[:] = [d for d in ds if d != "__pycache__"]
        for f in fs:
            p = os.path.join(r, f)
            out[os.path.relpath(p, racine).replace("\\", "/")] = empreinte(p)
    return out


def imports_pe(p):
    """Noms des DLL importées par un exécutable ou une DLL PE32+."""
    b = open(p, "rb").read()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    nsec = struct.unpack_from("<H", b, pe + 6)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", b, opt)[0]
    rva_imp = struct.unpack_from("<I", b, opt + (112 if magic == 0x20B else 96) + 8)[0]
    so = opt + struct.unpack_from("<H", b, pe + 20)[0]
    secs = []
    for i in range(nsec):
        vs, va, _, raw = struct.unpack_from("<IIII", b, so + i * 40 + 8)
        secs.append((va, vs, raw))

    def off(rva):
        for va, vs, raw in secs:
            if va <= rva < va + max(vs, 1):
                return rva - va + raw
        return None
    noms = []
    o = off(rva_imp) if rva_imp else None
    while o is not None:
        champs = struct.unpack_from("<IIIII", b, o)
        if not any(champs):
            break
        n = off(champs[3])
        noms.append(b[n:b.index(b"\0", n)].decode("ascii", "replace"))
        o += 20
    return noms


def version_fichier(p):
    """FileVersion (chaîne) de la ressource de version d'un exécutable Windows."""
    v = ctypes.windll.version
    n = v.GetFileVersionInfoSizeW(p, None)
    if not n:
        return None
    buf = ctypes.create_string_buffer(n)
    if not v.GetFileVersionInfoW(p, 0, n, buf):
        return None
    ptr, lg = ctypes.c_void_p(), ctypes.c_uint()
    if not v.VerQueryValueW(buf, "\\VarFileInfo\\Translation", ctypes.byref(ptr), ctypes.byref(lg)) or lg.value < 4:
        return None
    langue, page = struct.unpack("<HH", ctypes.string_at(ptr.value, 4))
    cle = f"\\StringFileInfo\\{langue:04x}{page:04x}\\FileVersion"
    if not v.VerQueryValueW(buf, cle, ctypes.byref(ptr), ctypes.byref(lg)):
        return None
    return ctypes.wstring_at(ptr.value, lg.value).rstrip("\0")


def numeros(v):
    out = []
    for x in v.strip().lstrip("vV").split("-")[0].split("."):
        out.append(int(x) if x.isdigit() else 0)
    return tuple(out + [0] * (3 - len(out)))


# ---------------------------------------------------------------------------------------------- essais du jeu
def essai_jeu(full, dossier, duree, env_plus=None):
    """Lance recomp_main.exe de `full` pendant `duree` s (sauvegarde et journal dans `dossier`), fenêtré, sans le son,
    A toutes les 2 s. Rend (journal, raison de l'arrêt). Saute si le jeu ou le launcher est déjà ouvert ; s'arrête si
    un autre recomp_main.exe démarre (le joueur lance sa partie)."""
    if processus("recomp_main.exe") or processus("lanceur.exe"):
        raise Saute("le jeu ou le launcher est ouvert : essai du jeu remis à plus tard")
    libre = memoire_validable_libre()
    if libre < 12:   # le jeu engage ≈ 10 Go (mémoire à la demande depuis la 1.0.3) : en dessous, il risquerait de s'arrêter
        raise Saute(f"mémoire du PC insuffisante pour l'essai du jeu : {libre:.1f} Go validables libres, il en faut 12 "
                    f"(programmes les plus gourmands : {gros_processus()}) : à refaire après les avoir fermés")
    shutil.rmtree(dossier, ignore_errors=True)
    os.makedirs(os.path.join(dossier, "save"))
    journal = os.path.join(dossier, "run.log")
    env = dict(os.environ)
    env.update({"RECOMP_SAVE_DIR": os.path.join(dossier, "save"), "RECOMP_VOLUME": "0", "RECOMP_WINDOW": "windowed",
                "RECOMP_AUTOPRESS": "1"})
    env.update(env_plus or {})
    p = subprocess.Popen([os.path.join(full, "recomp_main.exe"), "--log", journal], cwd=full, env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0, raison = time.time(), "durée atteinte"
    try:
        while time.time() - t0 < duree:
            time.sleep(1)
            if p.poll() is not None:
                raison = f"arrêt du jeu (code {p.returncode:#x})"
                break
            if processus("recomp_main.exe") - {p.pid}:
                raison = "le joueur a lancé le jeu : essai arrêté"
                break
    finally:
        if p.poll() is None:
            p.kill()
            p.wait(15)
    try:
        texte = open(journal, encoding="utf-8", errors="replace").read()
    except OSError:
        texte = ""
    if raison.startswith("le joueur"):
        raise Saute(raison)
    return texte, raison


def bilan_jeu(texte):
    images = [int(x) for x in re.findall(r"\[vk +\] frame (\d+):", texte)]
    absents = [int(x) for x in re.findall(r"(\d+) missing shaders", texte)]
    ignores = [int(x) for x in re.findall(r" (\d+) skipped,", texte)]
    # économies de mémoire de la 1.0.3 : données des textures rendues à Windows (Mo au dernier relevé), bibliothèques de
    # shaders créées au premier usage ; une donnée rendue qui ne revient pas est un plantage
    rendus = [int(x) for x in re.findall(r"texture data released: (\d+) MB now", texte)]
    return {"images": max(images) if images else 0, "absents": max(absents) if absents else 0,
            "ignores": max(ignores) if ignores else 0, "inconnus": texte.count("unknown shader"),
            "plantage": bool(re.search(r"panic|crash|unhandled exception|fatal|not read back|not restored", texte, re.I)),
            "rendus": rendus[-1] if rendus else 0,
            "textures": "texture data released after upload: on" in texte,
            "a_la_demande": "shader libraries created at first use" in texte}


# ---------------------------------------------------------------------------------------------- vérifications
class Verif:
    def __init__(self, a):
        self.a = a
        self.t = os.path.abspath(a.travail)
        self.v = version()
        self.etat = {}   # résultats partagés (exécutable d'essai, paquet…)
        os.makedirs(self.t, exist_ok=True)

    # -- rapide
    def versions(self):
        v = self.v
        erreurs = []
        h = open(os.path.join(LANCEUR, "lanceur.h"), encoding="utf-8").read()
        if f'L"{v}"' not in h:
            erreurs.append("lanceur.h : VERSION de secours différente")
        rc = open(os.path.join(LANCEUR, "lanceur.rc"), encoding="utf-8", errors="replace").read()
        a, b, c = numeros(v)
        if f"FILEVERSION {a},{b},{c},0" not in rc or f'"FileVersion", "{v}"' not in rc:
            erreurs.append("lanceur.rc : version différente")
        ch = open(os.path.join(RECOMP, "depot", "CHANGELOG.md"), encoding="utf-8").read()
        m = re.search(r"^## (\S+)", ch, re.M)
        if not m or m.group(1) != v:
            erreurs.append(f"CHANGELOG : dernière entrée {m.group(1) if m else '?'}")
        rd = open(os.path.join(RECOMP, "depot", "README.md"), encoding="utf-8").read()
        if f"{NOM}-{v}-windows.zip" not in rd:
            erreurs.append("README : le paquet à télécharger n'est pas celui de cette version")
        exe = os.path.join(PROJET, "lanceur.exe")
        if not os.path.exists(exe):
            erreurs.append("lanceur.exe absent de la racine (construire.py)")
        else:
            fv = version_fichier(exe)
            if fv != v:
                erreurs.append(f"lanceur.exe : version {fv} (à recompiler : construire.py)")
            sources = [os.path.join(LANCEUR, f) for f in os.listdir(LANCEUR) if f.endswith((".cpp", ".h", ".rc"))]
            recente = max(sources, key=os.path.getmtime)
            if os.path.getmtime(recente) > os.path.getmtime(exe):
                erreurs.append(f"lanceur.exe plus ancien que {os.path.basename(recente)} (à recompiler : construire.py)")
        if erreurs:
            raise Echec("; ".join(erreurs))
        return f"version {v} partout"

    def python(self):
        fichiers = []
        for r, ds, fs in os.walk(RECOMP):
            ds[:] = [d for d in ds if d not in ("build", "out", "outils", "__pycache__", "bench")]
            fichiers += [os.path.join(r, f) for f in fs if f.endswith(".py")]
        fichiers += glob.glob(os.path.join(PROJET, "tools", "*.py"))
        erreurs = []
        cache = os.path.join(self.t, "pycache")
        for f in fichiers:
            try:
                py_compile.compile(f, cfile=os.path.join(cache, "x.pyc"), doraise=True)
            except py_compile.PyCompileError as e:
                erreurs.append(f"{os.path.relpath(f, PROJET)} : {str(e).strip().splitlines()[-1]}")
        shutil.rmtree(cache, ignore_errors=True)
        if erreurs:
            raise Echec("\n".join(erreurs))
        return f"{len(fichiers)} fichiers"

    def runtime(self):
        full = os.path.join(self.t, "installation_deplacee", "recomp", "build", "full")
        os.makedirs(full, exist_ok=True)
        exe = os.path.join(full, "recomp_main.exe")
        code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "link.py"), "--out", exe], cwd=RECOMP)
        if code or not os.path.exists(exe):
            raise Echec("link.py :\n" + fin(sortie, 30))
        avert = len(re.findall(r"warning:", sortie))
        self.etat["exe"] = exe
        return f"recomp_main.exe d'essai lié ({os.path.getsize(exe) / 1e6:.0f} Mo), {avert} avertissement(s)"

    def sauvegardes(self):
        exe = self.etat.get("exe")
        if not exe:
            raise Saute("pas d'exécutable d'essai (vérification « runtime » non faite)")
        base = os.path.join(self.t, "sauvegardes_essai")
        shutil.rmtree(base, ignore_errors=True)
        appdata = os.path.join(base, "appdata")
        os.makedirs(appdata)
        ancien = os.path.join(os.path.dirname(exe), "save")
        shutil.rmtree(ancien, ignore_errors=True)
        os.makedirs(os.path.join(ancien, "sous"))
        contenu = {"save_game_data.save": os.urandom(40000), os.path.join("sous", "b.bin"): os.urandom(123)}
        for n, c in contenu.items():
            open(os.path.join(ancien, n), "wb").write(c)
        env = dict(os.environ, APPDATA=appdata)
        env.pop("RECOMP_SAVE_DIR", None)
        erreurs = []

        def chemin(e):
            r = subprocess.run([exe, "--save-dir"], cwd=os.path.dirname(exe), env=e, capture_output=True, timeout=120)
            return r.returncode, r.stdout.decode("utf-8", "replace").strip().splitlines()[-1:] or [""]

        nouveau = os.path.join(appdata, "SpaceAnimalsRecomp", "save")
        for passe in (1, 2):   # la reprise, puis un second lancement qui ne doit rien changer
            code, sortie = chemin(env)
            if code or os.path.normcase(sortie[0]) != os.path.normcase(nouveau):
                erreurs.append(f"--save-dir (passe {passe}) : code {code}, « {sortie[0]} » au lieu de {nouveau}")
            for n, c in contenu.items():
                for dossier, quoi in ((nouveau, "reprise"), (ancien, "ancien dossier")):
                    p = os.path.join(dossier, n)
                    if not os.path.exists(p) or open(p, "rb").read() != c:
                        erreurs.append(f"{quoi} (passe {passe}) : {n} absent ou différent")
        if not glob.glob(os.path.join(glob.escape(ancien), "SAVE MOVED*")):
            erreurs.append("pas de note dans l'ancien dossier")
        # une sauvegarde existe déjà au nouvel endroit : l'ancienne ne l'écrase pas
        open(os.path.join(nouveau, "save_game_data.save"), "wb").write(b"plus recente")
        chemin(env)
        if open(os.path.join(nouveau, "save_game_data.save"), "rb").read() != b"plus recente":
            erreurs.append("sauvegarde du nouvel emplacement écrasée par l'ancienne")
        autre = os.path.join(base, "ailleurs")
        code, sortie = chemin(dict(env, RECOMP_SAVE_DIR=autre))
        if os.path.normcase(sortie[0]) != os.path.normcase(autre):
            erreurs.append(f"RECOMP_SAVE_DIR ignoré : « {sortie[0]} »")
        # copies de secours (sauvegardes.py), avec ces dossiers d'essai
        sv = charger(os.path.join(LANCEUR, "sauvegardes.py"), "verif_sauvegardes")
        sv.ANCIEN = ancien
        vrai = os.environ.get("APPDATA")
        os.environ["APPDATA"] = appdata
        try:
            notes = []
            avant = (sv.empreinte(nouveau), sv.empreinte(ancien))
            c1 = sv.secours("essai", notes.append)
            c2 = sv.secours("essai", notes.append)
            if len(c1) != 2 or c2:
                erreurs.append(f"copies de secours : {len(c1)} puis {len(c2)} (attendu 2 puis 0)")
            if c1 and sorted(sv.empreinte(c) for c in c1) != sorted(avant):
                erreurs.append("copie de secours différente de la sauvegarde")
            for i in range(sv.A_GARDER + 2):
                open(os.path.join(nouveau, "save_game_data.save"), "wb").write(os.urandom(64))
                time.sleep(1.05)
                sv.secours("essai", notes.append)
            copies = [x for x in os.listdir(os.path.join(appdata, "SpaceAnimalsRecomp", "save_backups"))
                      if not x.endswith("_ancien")]
            if len(copies) != sv.A_GARDER:
                erreurs.append(f"{len(copies)} copies gardées (attendu {sv.A_GARDER})")
            if (sv.empreinte(ancien),) != avant[1:]:
                erreurs.append("ancien dossier modifié par les copies de secours")
        finally:
            if vrai is None:
                os.environ.pop("APPDATA", None)
            else:
                os.environ["APPDATA"] = vrai
        shutil.rmtree(ancien, ignore_errors=True)
        shutil.rmtree(base, ignore_errors=True)
        if erreurs:
            raise Echec("\n".join(erreurs))
        return ("reprise de l'ancien dossier vers %APPDATA% identique, ancien intact ; copies de secours identiques, sans "
                f"doublon, {sv.A_GARDER} gardées")

    def launcher(self):
        out = os.path.join(self.t, "lanceur_essai.exe")
        code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(LANCEUR, "construire.py"), "--out", out],
                              cwd=LANCEUR)
        if code or not os.path.exists(out):
            raise Echec("construire.py :\n" + fin(sortie, 30))
        avert = len(re.findall(r"warning:", sortie))
        if avert:
            raise Echec(f"{avert} avertissement(s) :\n" + fin(sortie, 20))
        # Écran de 270 Hz et plus (issue n° 12) : Present n'accepte qu'une image tous les 1 à 4 balayages ; au-delà il
        # échouait à chaque image et la fenêtre restait noire. Le launcher d'essai démarre avec une fréquence simulée
        # (LANCEUR_HZ), se ferme seul après une capture, et son journal ne doit signaler aucun échec d'affichage.
        # Une seule instance du launcher à la fois : sans objet si celui de l'utilisateur est ouvert.
        if processus("lanceur.exe"):
            os.remove(out)
            return "compilé sans avertissement ; affichage non essayé (un launcher est ouvert)"
        journal, image = os.path.join(self.t, "lanceur.log"), os.path.join(self.t, "lanceur_360hz.png")
        for f in (journal, image):
            if os.path.exists(f):
                os.remove(f)
        env = {k: v for k, v in os.environ.items() if not k.startswith("LANCEUR_")}
        env.update({"LANCEUR_HZ": "360", "LANCEUR_CAPTURE": image, "LANCEUR_CAPTURE_T": "1.5",
                    "LANCEUR_CAPTURE_QUITTER": "1"})
        try:
            code, sortie = lancer([out], cwd=self.t, env=env, timeout=60)
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/F", "/IM", os.path.basename(out)], capture_output=True)
            raise Echec("launcher d'essai : pas fermé seul en 60 s")
        texte = open(journal, encoding="utf-8", errors="replace").read() if os.path.exists(journal) else ""
        erreurs = []
        if code:
            erreurs.append(f"launcher d'essai : code {code}")
        if "screen 360 Hz, one frame every 4 refresh" not in texte:
            erreurs.append("launcher d'essai : pas de ligne « display: … screen 360 Hz, one frame every 4 refresh(es) »")
        if " failed" in texte or "not recreated" in texte:
            erreurs.append("launcher d'essai : échec d'affichage dans son journal")
        if not os.path.exists(image) or os.path.getsize(image) < 50000:
            erreurs.append("launcher d'essai : pas d'image")
        if erreurs:
            raise Echec("\n".join(erreurs) + "\n" + fin(texte, 8))
        # Mises à jour au choix du joueur : dans un faux paquet jetable, le launcher ouvre ses vraies questions (nouvelle
        # version disponible, jeu à refaire) et l'essai y répond oui ou non (verif/essai_choix_maj.py).
        choix = "choix des mises à jour non essayé (pas de copie du jeu : --copie)"
        if self.a.copie and os.path.exists(self.a.copie):
            code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(ICI, "essai_choix_maj.py"), out, self.a.copie],
                                  timeout=600)
            if code:
                raise Echec("essai_choix_maj.py :\n" + fin(sortie, 45))
            choix = "mises à jour au choix du joueur : " + fin(sortie, 1)
        for f in (out, journal, image, os.path.join(self.t, "lanceur.ini")):
            if os.path.exists(f):
                os.remove(f)
        return "compilé sans avertissement ; écran simulé de 360 Hz : image présentée, aucun échec d'affichage ; " + choix

    def desinstaller(self):
        travail = os.path.join(self.t, "sar_verif_desinstaller")
        code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(ICI, "essai_desinstaller.py"), LANCEUR, travail],
                              timeout=300)
        shutil.rmtree(travail, ignore_errors=True)
        if code or "tout passe" not in sortie:
            raise Echec("essai_desinstaller.py :\n" + fin(sortie, 30))
        return f"{len(re.findall(r'conditions', sortie))} scénarios : nettoyage, désinstallation, copie du jeu gardée, refus"

    def deplacement(self):
        """Emplacement du jeu (issue n° 14) : deplacer.py sur de fausses installations."""
        travail = os.path.join(self.t, "sar_verif_deplacement")
        code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(ICI, "essai_deplacement.py"), LANCEUR, travail],
                              timeout=300)
        shutil.rmtree(travail, ignore_errors=True)
        if code or "tout passe" not in sortie:
            raise Echec("essai_deplacement.py :\n" + fin(sortie, 30))
        return f"{len(re.findall(r'conditions', sortie))} scénarios : même volume, autre volume, refus, abandon, jeu en marche"

    def rapport(self):
        """Anonymisation du rapport de bug (issue n° 5 : chemin de la copie resté dans lanceur.log)."""
        travail = os.path.join(self.t, "sar_verif_rapport")
        code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(ICI, "essai_rapport.py"), LANCEUR, travail],
                              timeout=180)
        shutil.rmtree(travail, ignore_errors=True)
        if code or "tout passe" not in sortie:
            raise Echec("essai_rapport.py :\n" + fin(sortie, 30))
        m = re.search(r"(\d+) noms privés cherchés", sortie)
        return f"fausse installation : {m.group(1) if m else '?'} noms privés absents du zip, lanceur.ini UTF-16 lisible"

    def depot(self):
        out = os.path.join(self.t, "depot")
        shutil.rmtree(out, ignore_errors=True)
        code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(LANCEUR, "depot.py"), out], cwd=LANCEUR)
        if code:
            raise Echec("depot.py :\n" + fin(sortie))
        n = sum(len(fs) for _, _, fs in os.walk(out))
        for nec in ("README.md", "LICENSE", "CHANGELOG.md", "recomp/VERSION", "recomp/licences/THIRD_PARTY_NOTICES.txt",
                    "recomp/verif/verifier.py"):
            if not os.path.exists(os.path.join(out, nec)):
                raise Echec(f"absent de l'export : {nec}")
        shutil.rmtree(out, ignore_errors=True)
        return f"{n} fichiers exportés, aucun interdit"

    def paquet(self):
        out = os.path.join(self.t, "paquet")
        shutil.rmtree(out, ignore_errors=True)
        code, sortie = lancer([sys.executable, "-X", "utf8", os.path.join(LANCEUR, "paquet.py"), out, "--garder"],
                              cwd=LANCEUR)
        if code:
            raise Echec("paquet.py :\n" + fin(sortie))
        v = self.v
        p = os.path.join(out, f"{NOM}-{v}")
        complet = os.path.join(out, f"{NOM}-{v}-windows.zip")
        maj = os.path.join(out, f"{NOM}-{v}-update.zip")
        erreurs = []

        def doit(rel, present=True):
            if os.path.exists(os.path.join(p, rel)) != present:
                erreurs.append(("absent : " if present else "présent : ") + rel)
        for rel in ("lanceur.exe", "LISEZMOI.txt", "README.txt", "THIRD_PARTY_NOTICES.txt", "recomp/VERSION",
                    "recomp/lots.txt", "recomp/hooks.txt", "recomp/launcher/sauvegardes.py", "recomp/launcher/rapport.py",
                    "recomp/launcher/preparer.py", "recomp/launcher/maj.py", "recomp/launcher/outils.py",
                    "recomp/launcher/desinstaller.py", "recomp/launcher/deplacer.py", "uninstall.bat",
                    "outils/sdl3/SDL3.dll", "outils/fsr/amd_fidelityfx_loader_vk.dll",
                    "outils/fsr/amd_fidelityfx_upscaler_vk.dll", "outils/dlss/nvngx_dlss.dll",
                    "LICENCES/AMD-FidelityFX-SDK-LICENSE.txt", "LICENCES/NVIDIA-DLSS-SDK-LICENSE.txt",
                    "recomp/runtime/third_party/ffx/LICENSE.md",
                    "outils/python/python.exe", "outils/gcc/bin/g++.exe", "outils/gcc/bin/clang++.exe",
                    "outils/glslang/bin/glslang.exe",
                    "outils/gcc/libexec/gcc/x86_64-w64-mingw32/16.2.0/cc1plus.exe"):
            doit(rel)
        for rel in ("outils/gcc/bin/gdb.exe", "outils/gcc/bin/cmake.exe", "outils/gcc/lib/python3.9",
                    "outils/gcc/libexec/gcc/x86_64-w64-mingw32/16.2.0/f951.exe", "outils/glslang/include"):
            doit(rel, False)
        if len(glob.glob(os.path.join(p, "LICENCES", "*.txt"))) < 13:
            erreurs.append("LICENCES : moins de 13 textes")
        if len(glob.glob(os.path.join(p, "recomp", "launcher", "extraits", "extrait_*.mp4"))) < 1:
            erreurs.append("aucun extrait du fond du launcher")
        for mode_emploi in ("LISEZMOI.txt", "README.txt"):   # français et anglais, à tenir à jour ensemble (paquet.py)
            if os.path.exists(os.path.join(p, mode_emploi)) and                     v not in open(os.path.join(p, mode_emploi), encoding="utf-8-sig").read():
                erreurs.append(f"{mode_emploi} : version absente")
        if empreinte(os.path.join(p, "lanceur.exe")) != empreinte(os.path.join(PROJET, "lanceur.exe")):
            erreurs.append("lanceur.exe différent de celui de la racine")
        for rel, h in arbre(p, sans=("outils", "LICENCES")).items():   # sources du paquet = sources du projet
            # (écrits par paquet.py, sans fichier source : modes d'emploi, notices, uninstall.bat)
            src = os.path.join(PROJET, rel) if not rel.startswith(("lanceur.exe", "LISEZMOI", "README.txt", "THIRD_PARTY",
                                                                    "uninstall.bat")) else None
            if src and (not os.path.exists(src) or empreinte(src) != h):
                erreurs.append(f"différent des sources : {rel}")
        noms_c = {n.split("/", 1)[1] for n in zipfile.ZipFile(complet).namelist() if "/" in n}
        noms_m = {n.split("/", 1)[1] for n in zipfile.ZipFile(maj).namelist() if "/" in n}
        if noms_m != {n for n in noms_c if not n.startswith("outils/")}:
            erreurs.append("la mise à jour ne contient pas exactement le paquet sans outils/")
        if erreurs:
            raise Echec("\n".join(erreurs[:30]))
        self.etat.update(paquet=p, complet=complet, maj=maj)
        return (f"complet {os.path.getsize(complet) / 1e6:.0f} Mo, mise à jour {os.path.getsize(maj) / 1e6:.0f} Mo, "
                f"{len(noms_c)} fichiers")

    def outils(self):
        p = self.etat.get("paquet")
        if not p:
            raise Saute("pas de paquet (vérification « paquet » non faite)")
        gcc = os.path.join(p, "outils", "gcc")
        utilises = [l.strip() for l in open(os.path.join(ICI, "gcc_utilises.txt"), encoding="utf-8")
                    if l.strip() and not l.startswith("#")]
        manque = [u for u in utilises if not os.path.exists(os.path.join(gcc, u))]
        presents = set()
        for r, _, fs in os.walk(gcc):
            for f in fs:
                presents.add(os.path.relpath(os.path.join(r, f), gcc).replace("\\", "/").lower())
        systeme = {f.lower() for f in os.listdir(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32"))}
        orphelins = []
        for rel in sorted(presents):
            if rel.endswith((".exe", ".dll")):
                d = os.path.dirname(rel)
                for nom in imports_pe(os.path.join(gcc, rel)):
                    n = nom.lower()
                    if (f"{d}/{n}".lstrip("/") not in presents and f"bin/{n}" not in presents and n not in systeme
                            and not n.startswith("api-ms-win-")):
                        orphelins.append(f"{rel} → {nom}")
        glslang = os.path.join(p, "outils", "glslang", "bin", "glslang.exe")
        vc = [n for n in imports_pe(glslang) if n.lower() not in systeme and not n.lower().startswith("api-ms-win-")]
        if manque or orphelins:
            raise Echec("\n".join([f"fichier utilisé absent : {m}" for m in manque[:20]] +
                                  [f"DLL introuvable : {o}" for o in orphelins[:20]]))
        info = f"{len(utilises)} fichiers utilisés présents, DLL importées toutes présentes"
        if vc:
            info += f" ; glslang.exe demande {', '.join(vc)} (runtime Visual C++, absent de ce PC ?)"
        return info

    def _derniere_release(self):
        """Dernière release publiée plus ancienne que la version vérifiée : (tag, asset de mise à jour)."""
        req = urllib.request.Request(f"https://api.github.com/repos/{DEPOT_GITHUB}/releases?per_page=20",
                                     headers={"User-Agent": "SpaceAnimalsRecomp-verif", "Accept": "application/vnd.github+json"})
        with urllib.request.urlopen(req, timeout=30) as r:
            rels = json.loads(r.read().decode("utf-8"))
        rels = [x for x in rels if not x.get("draft") and not x.get("prerelease") and numeros(x["tag_name"]) < numeros(self.v)]
        if not rels:
            raise Saute("aucune release publiée plus ancienne")
        rel = max(rels, key=lambda x: numeros(x["tag_name"]))
        asset = next((x for x in rel.get("assets", []) if x["name"].endswith("-update.zip")), None)
        if not asset:
            raise Saute(f"la release {rel['tag_name']} n'a pas d'archive de mise à jour")
        return rel["tag_name"], asset

    def maj(self):
        if not self.etat.get("maj"):
            raise Saute("pas d'archive de mise à jour (vérification « paquet » non faite)")
        tag, asset = self._derniere_release()
        cache = os.path.join(self.t, "cache")
        os.makedirs(cache, exist_ok=True)
        ancien_zip = os.path.join(cache, asset["name"])
        attendu = (asset.get("digest") or "")[7:]
        if not (os.path.exists(ancien_zip) and (not attendu or empreinte(ancien_zip) == attendu)):
            req = urllib.request.Request(asset["browser_download_url"], headers={"User-Agent": "SpaceAnimalsRecomp-verif"})
            with urllib.request.urlopen(req, timeout=120) as r, open(ancien_zip, "wb") as f:
                shutil.copyfileobj(r, f)
        if attendu and empreinte(ancien_zip) != attendu:
            raise Echec(f"archive {asset['name']} : empreinte différente de celle de GitHub")
        # installation de la version précédente : son paquet de mise à jour + les marques des outils actuels
        base = os.path.join(self.t, "maj")
        shutil.rmtree(base, ignore_errors=True)
        with zipfile.ZipFile(ancien_zip) as z:
            z.extractall(base)
        racine = os.path.join(base, os.listdir(base)[0])
        # outils du joueur : ceux de la version précédente (marques écrites par SON outils.py), pas les actuels. Si la
        # nouvelle version change une archive (compilateur avec Clang depuis la 1.0.3), la mise à jour doit garder
        # l'ancienne : la remplacer ferait retélécharger 424 Mo et recompiler tout le jeu.
        ns = {"__file__": "outils.py", "__name__": "verif_outils_avant"}
        exec(compile(open(os.path.join(racine, "recomp", "launcher", "outils.py"), encoding="utf-8").read(),
                     "outils.py", "exec"), ns)
        os.makedirs(os.path.join(racine, "outils", ".installe"))
        for o in ns["OUTILS"]:
            with open(os.path.join(racine, "outils", ".installe", o["nom"]), "w", encoding="utf-8") as f:
                f.write(o["sha256"] + "\n")
            os.makedirs(os.path.join(racine, "outils", *o["dest"].split("/")), exist_ok=True)
        appdata = os.path.join(self.t, "maj_appdata")   # copies de secours des sauvegardes : jamais dans le vrai profil
        shutil.rmtree(appdata, ignore_errors=True)
        os.makedirs(appdata)
        sauv = os.path.join(racine, "recomp", "build", "full", "save", "save_game_data.save")
        os.makedirs(os.path.dirname(sauv))
        sauv_contenu = os.urandom(40000)   # sauvegarde d'essai à l'emplacement de la version précédente
        open(sauv, "wb").write(sauv_contenu)
        outils_avant = arbre(os.path.join(racine, "outils"))
        # Outils que la nouvelle version ajoute (SDL3, DLL du FSR et du DLSS depuis la 1.1.0) : chez le joueur, la mise
        # à jour les télécharge depuis leurs sources. Ici rien n'est téléchargé : leurs archives sont posées d'avance
        # dans outils/_archives (outils.py vérifie leur SHA-256 et ne télécharge pas une archive déjà là), prises dans
        # le paquet (fichiers seuls) ou dans le cache des vérifications. RECOMP_OUTILS_SANS_RESEAU reste posé : tout
        # autre téléchargement fait échouer l'essai. Les adresses elles-mêmes ne sont donc pas essayées ici.
        ns_n = {"__file__": "outils.py", "__name__": "verif_outils_apres"}
        exec(compile(open(os.path.join(self.etat["paquet"], "recomp", "launcher", "outils.py"), encoding="utf-8").read(),
                     "outils.py", "exec"), ns_n)
        avant_par_nom = {o["nom"]: o["sha256"] for o in ns["OUTILS"]}
        ajoutes = [o for o in ns_n["OUTILS"]
                   if avant_par_nom.get(o["nom"]) not in (o["sha256"],) + tuple(o.get("acceptes", ()))]
        a_telecharger = 0
        for o in ajoutes:
            nom_archive = o["url"].rsplit("/", 1)[1]
            sources = [os.path.join(cache, nom_archive)]
            if o.get("fichier"):
                sources.insert(0, os.path.join(self.etat["paquet"], "outils", *o["dest"].split("/"), nom_archive))
            source = next((p for p in sources if os.path.exists(p) and empreinte(p) == o["sha256"]), None)
            if not source:
                raise Saute(f"archive de l'outil « {o['nom']} » absente de {cache} ({nom_archive}) : cette vérification "
                            "ne télécharge pas les outils")
            os.makedirs(os.path.join(racine, "outils", "_archives"), exist_ok=True)
            shutil.copyfile(source, os.path.join(racine, "outils", "_archives", nom_archive))
            a_telecharger += os.path.getsize(source)
        sig_avant = charger(os.path.join(racine, "recomp", "launcher", "preparer.py"), "verif_prep_avant").signatures("copie")
        mod = charger(os.path.join(racine, "recomp", "launcher", "maj.py"), "verif_maj_avant")
        if mod.version_locale(racine) != tag.lstrip("vV"):
            raise Echec(f"version de l'installation précédente : {mod.version_locale(racine)} (attendu {tag})")
        nouveau = self.etat["maj"]
        url = mod.TELECHARGEMENTS + f"v{self.v}/" + os.path.basename(nouveau)
        rel = {"tag_name": f"v{self.v}", "assets": [{"name": os.path.basename(nouveau), "browser_download_url": url,
                                                     "size": os.path.getsize(nouveau),
                                                     "digest": "sha256:" + empreinte(nouveau)}]}
        mod.derniere_release = lambda delai=15: rel

        class Reponse(io.BytesIO):
            headers = {}

            def __enter__(self):
                return self

            def __exit__(self, *e):
                self.close()

        def servir(req, timeout=None):
            u = req.full_url if hasattr(req, "full_url") else req
            if u != url:
                raise Echec(f"URL inattendue : {u}")
            r = Reponse(open(nouveau, "rb").read())
            r.headers = {"Content-Length": str(os.path.getsize(nouveau))}
            return r
        # urllib et subprocess sont les modules de ce processus : remplacés le temps de l'essai, puis rétablis
        vrai_urlopen, vrai_popen = urllib.request.urlopen, subprocess.Popen
        relance = []

        def faux_popen(*x, **k):   # le launcher n'est pas relancé ; les autres commandes (outils.py) tournent vraiment
            cmd = x[0] if x else k.get("args")
            if isinstance(cmd, (list, tuple)) and str(cmd[0]).lower().endswith("lanceur.exe"):
                relance.append(x)
                return None
            return vrai_popen(*x, **k)
        env_avant = {k: os.environ.get(k) for k in ("APPDATA", "RECOMP_OUTILS_SANS_RESEAU")}
        tampon = io.StringIO()
        sortie_std = sys.stdout
        try:
            mod.urllib.request.urlopen = servir
            mod.subprocess.Popen = faux_popen
            os.environ.update({"APPDATA": appdata, "RECOMP_OUTILS_SANS_RESEAU": "1"})
            sys.stdout = tampon
            mod.verifier(racine)
            mod.installer(racine)
            code = mod.finir(racine, 0)
        finally:
            sys.stdout = sortie_std
            urllib.request.urlopen, subprocess.Popen = vrai_urlopen, vrai_popen
            for k, v in env_avant.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        sortie = tampon.getvalue()
        erreurs = []
        if f"@maj {self.v}" not in sortie:
            erreurs.append("--verifier n'annonce pas la nouvelle version")
        if code:
            erreurs.append(f"--finir : code {code}")
        apres, attendu_arbre = arbre(racine, sans=("outils",)), arbre(self.etat["paquet"], sans=("outils",))
        faux = [k for k, h in attendu_arbre.items() if apres.get(k) != h]
        restes = [k for k in apres if k not in attendu_arbre and k != "mise_a_jour.log" and "/save/" not in k.replace("\\", "/")]
        if not os.path.exists(sauv) or open(sauv, "rb").read() != sauv_contenu:
            erreurs.append("sauvegarde du joueur absente ou modifiée après la mise à jour")
        if faux:
            erreurs.append("fichiers absents ou différents : " + ", ".join(faux[:10]))
        outils_apres = arbre(os.path.join(racine, "outils"))
        for o in (outils_avant, outils_apres):   # LISEZMOI.txt est réécrit par outils.py (liste des outils)
            o.pop("LISEZMOI.txt", None)
        # attendu : les outils d'avant intacts, plus ceux que la nouvelle version ajoute, tels que dans le paquet
        outils_attendus = dict(outils_avant)
        paquet_outils = arbre(os.path.join(self.etat["paquet"], "outils"))
        for o in ajoutes:
            outils_attendus[f".installe/{o['nom']}"] = paquet_outils.get(f".installe/{o['nom']}")
            for k, h in paquet_outils.items():
                if k.startswith(o["dest"] + "/"):
                    outils_attendus[k] = h
        if outils_apres != outils_attendus:
            erreurs.append("outils/ après la mise à jour, différences avec l'attendu : "
                           + ", ".join(sorted(k for k in set(outils_attendus) | set(outils_apres)
                                              if outils_attendus.get(k) != outils_apres.get(k))[:8]))
        if not relance:
            erreurs.append("le launcher n'aurait pas été relancé")
        if erreurs:
            raise Echec("\n".join(erreurs) + "\n" + fin(sortie))
        sig_apres = charger(os.path.join(racine, "recomp", "launcher", "preparer.py"), "verif_prep_apres").signatures("copie")
        refaites = [e for e in sig_apres if sig_avant.get(e) != sig_apres[e]]
        shutil.rmtree(base, ignore_errors=True)
        info = f"depuis {tag} : fichiers installés, outils d'avant intacts, sauvegarde intacte"
        if ajoutes:
            info += (f" ; outils ajoutés : {', '.join(o['nom'] for o in ajoutes)} ({a_telecharger / 1e6:.0f} Mo à "
                     "télécharger chez le joueur ; ici archives locales, adresses non essayées)")
        info += f" ; restes de {tag} non supprimés : {', '.join(restes)}" if restes else ""
        info += f" ; « RECONSTRUIRE MAINTENANT » refera : {', '.join(refaites) if refaites else 'rien'}"
        return info

    def _cache_shaders(self, full):
        dst = os.path.join(full, "shader_cache")
        os.makedirs(dst, exist_ok=True)
        src = os.path.join(FULL, "shader_cache")
        n = 0
        for f in os.listdir(src):
            if f.endswith((".spv", ".meta")):
                shutil.copy2(os.path.join(src, f), os.path.join(dst, f))
                n += f.endswith(".spv")
        return n

    def precompile(self):
        if not vulkan():
            raise Saute("pas de Vulkan sur ce PC")
        exe = self.etat.get("exe")
        if not exe:
            raise Saute("pas d'exécutable d'essai (vérification « runtime » non faite)")
        full = os.path.dirname(exe)
        n = self._cache_shaders(full)
        for f in ("shaders_exclus.txt", "pipelines.ok", "pipelines.cache"):
            if os.path.exists(os.path.join(full, f)):
                os.remove(os.path.join(full, f))
        cible = sorted(f[:-4] for f in os.listdir(os.path.join(full, "shader_cache")) if f.endswith(".spv"))[n // 2]
        env = dict(os.environ, RECOMP_PRECOMPILE_PLANTE=cible)
        code, sortie = lancer([exe, "--precompile"], cwd=full, env=env, timeout=900)
        erreurs = []
        exclus = open(os.path.join(full, "shaders_exclus.txt"), encoding="utf-8").read() if os.path.exists(
            os.path.join(full, "shaders_exclus.txt")) else ""
        if code:
            erreurs.append(f"code {code}")
        if cible not in exclus:
            erreurs.append("shader fautif non inscrit dans shaders_exclus.txt")
        if f"@info {n - 1} shaders compiled" not in sortie:
            erreurs.append(f"précompilation non finie avec {n - 1} shaders")
        # sans plantage : tout est compilé
        for f in ("shaders_exclus.txt", "pipelines.ok", "pipelines.cache"):
            if os.path.exists(os.path.join(full, f)):
                os.remove(os.path.join(full, f))
        code2, sortie2 = lancer([exe, "--precompile"], cwd=full, timeout=900)
        if code2 or f"@info {n} shaders compiled" not in sortie2:
            erreurs.append(f"précompilation normale : code {code2}")
        if erreurs:
            raise Echec("\n".join(erreurs) + "\n" + fin(sortie + sortie2, 20))
        t = re.search(r"@info \d+ shaders compiled in ([\d.]+) s", sortie2)
        return (f"plantage simulé sur {cible} : isolé et écarté, {n - 1} shaders compilés ; sans plantage : {n} shaders"
                + (f" en {t.group(1)} s" if t else ""))

    def deplace(self):
        if not vulkan():
            raise Saute("pas de Vulkan sur ce PC")
        exe = self.etat.get("exe")
        if not exe or not os.path.exists(os.path.join(os.path.dirname(exe), "pipelines.cache")):
            raise Saute("pas d'exécutable d'essai précompilé (vérifications « runtime » et « precompile »)")
        if not self.a.copie:
            raise Saute("copie du jeu (.nup) introuvable : --copie")
        full = os.path.dirname(exe)
        racine = os.path.dirname(os.path.dirname(os.path.dirname(full)))
        os.makedirs(os.path.join(racine, "work"), exist_ok=True)
        lignes = []
        for l in open(os.path.join(FULL, "manifest.txt"), encoding="utf-8"):
            p = l.split()
            if len(p) >= 6 and p[0] not in ("nup", "romfs") and not p[0].startswith("#"):
                image = " ".join(p[5:])
                shutil.copy2(image, os.path.join(racine, "work", os.path.basename(image)))
                l = " ".join(p[:5]) + " " + "C:\\AncienDossierSFR\\work\\" + os.path.basename(image) + "\n"
            elif p and p[0] == "nup":
                l = "nup C:\\AncienDossierSFR\\" + os.path.basename(" ".join(p[1:])) + "\n"
            lignes.append(l)
        open(os.path.join(full, "manifest.txt"), "w", encoding="utf-8").writelines(lignes)
        texte, raison = essai_jeu(full, os.path.join(self.t, "essai_deplace"), 60, {"RECOMP_NUP": self.a.copie})
        b = bilan_jeu(texte)
        erreurs = []
        if texte.count("image found") < 6:
            erreurs.append(f"images retrouvées : {texte.count('image found')} sur 6")
        if "copy found" not in texte:
            erreurs.append("copie non retrouvée")
        if b["images"] < 1800 or b["plantage"] or b["absents"]:
            erreurs.append(f"jeu : {b} ({raison})")
        if erreurs:
            raise Echec("\n".join(erreurs) + "\n" + fin(texte))
        return f"images et copie retrouvées ; {b['images']} images en 60 s, 0 shader absent"

    # -- complet
    def neuve(self):
        complet = self.etat.get("complet")
        if not complet:
            raise Saute("pas de paquet (vérification « paquet » non faite)")
        if not self.a.copie:
            raise Saute("copie du jeu (.nup) introuvable : --copie")
        # avec un espace (copie du compilateur dans ProgramData) et des crochets (motifs de fichiers : le comptage des
        # shaders traduits rendait 0 dans un tel dossier, issue n° 1 du 2026-11-11)
        base = os.path.join(self.t, "installation [neuve]")
        shutil.rmtree(base, ignore_errors=True)
        with zipfile.ZipFile(complet) as z:
            z.extractall(base)
        racine = os.path.join(base, f"{NOM}-{self.v}")
        pd = os.path.join(self.t, "programdata")
        shutil.rmtree(pd, ignore_errors=True)
        os.makedirs(pd)
        env = dict(os.environ)
        env["PATH"] = os.pathsep.join(x for x in env.get("PATH", "").split(os.pathsep)
                                      if x and not any(m in x.lower() for m in ("msys64", "mingw", "winlibs", "starfoxrecomp")))
        env.update({"RECOMP_SANS_MSYS2": "1", "ProgramData": pd, "APPDATA": os.path.join(self.t, "neuve_appdata")})
        os.makedirs(env["APPDATA"], exist_ok=True)
        for k in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "LIBRARY_PATH", "GCC_EXEC_PREFIX", "COMPILER_PATH",
                  "RECOMP_GLSLANG", "RECOMP_NUP"):
            env.pop(k, None)
        journal = os.path.join(self.t, "neuve_preparation.log")
        t0 = time.time()
        with open(journal, "w", encoding="utf-8") as j:
            r = subprocess.run([os.path.join(racine, "outils", "python", "python.exe"), "-u",
                                os.path.join(racine, "recomp", "launcher", "preparer.py"), "--copie", self.a.copie],
                               cwd=racine, env=env, stdout=j, stderr=subprocess.STDOUT)
        texte = open(journal, encoding="utf-8", errors="replace").read()
        if r.returncode or "@fini" not in texte:
            raise Echec(f"preparer.py : code {r.returncode}\n" + fin("\n".join(l for l in texte.splitlines()
                                                                          if not l.startswith("@progres")), 25))
        copies = glob.glob(os.path.join(pd, "StarFoxRecomp", "gcc-*"))
        erreurs = []
        if not copies:
            erreurs.append("compilateur non copié dans le ProgramData d'essai (chemin avec espaces)")
        # plus aucune étape n'enregistre les extraits (1.0.3) : la construction ne doit pas en parler, et les
        # extraits fournis avec le paquet doivent être encore là à la fin
        if "extraits" in "".join(l for l in texte.splitlines() if l.startswith(("@etapes", "@etape ", "@saute"))):
            erreurs.append("la construction parle encore d'une étape « extraits »")
        if len(glob.glob(os.path.join(glob.escape(racine), "recomp", "launcher", "extraits", "extrait_*.mp4"))) < 1:
            erreurs.append("extraits fournis absents après la construction")
        m = re.search(r"shader survey: (\d+) created", texte)
        if not m:
            erreurs.append("relevé des shaders absent")
        if erreurs:
            raise Echec("\n".join(erreurs))
        self.etat["neuve"] = racine
        self.etat["neuve_env"] = env
        taille = sum(os.path.getsize(os.path.join(r_, f)) for r_, _, fs in os.walk(copies[0]) for f in fs) / 1e6
        return (f"construite en {(time.time() - t0) / 60:.0f} min ; compilateur copié hors des espaces ({taille:.0f} Mo) ; "
                f"relevé : {m.group(1)} shaders créés par le jeu")

    def regeneration(self):
        racine = self.etat.get("neuve")
        if not racine:
            raise Saute("pas d'installation neuve (vérification « neuve » non faite)")
        full = os.path.join(racine, "recomp", "build", "full")

        def objets():
            return {p: os.path.getmtime(p) for m in ("main", "sdk", "subsdk0", "subsdk1", "subsdk2", "subsdk3")
                    for p in glob.glob(os.path.join(glob.escape(full), m, "*.o"))}
        avant = objets()
        journal = os.path.join(self.t, "regeneration.log")
        t0 = time.time()
        with open(journal, "w", encoding="utf-8") as j:
            r = subprocess.run([os.path.join(racine, "outils", "python", "python.exe"), "-u",
                                os.path.join(racine, "recomp", "launcher", "preparer.py"), "--copie", self.a.copie,
                                "--etapes", "verifier,recompiler", "--forcer"],
                               cwd=racine, env=self.etat["neuve_env"], stdout=j, stderr=subprocess.STDOUT)
        duree = time.time() - t0
        texte = open(journal, encoding="utf-8", errors="replace").read()
        if r.returncode or "@fini" not in texte:
            raise Echec(f"preparer.py : code {r.returncode}\n" + fin("\n".join(l for l in texte.splitlines()
                                                                          if not l.startswith("@progres")), 25))
        apres = objets()
        refaits = sorted(os.path.basename(p) for p in apres if apres[p] != avant.get(p))
        if refaits:
            raise Echec(f"{len(refaits)} objet(s) recompilé(s) sans changement du code (génération non reproductible ?) : "
                        + ", ".join(refaits[:12]))
        if duree > 300:
            raise Echec(f"régénération sans changement en {duree:.0f} s (plus de 5 min)")
        return f"{len(apres)} objets gardés, aucun recompilé ; code régénéré en {duree:.0f} s"

    def shaders(self):
        racine = self.etat.get("neuve")
        if not racine:
            raise Saute("pas d'installation neuve")
        a = os.path.join(racine, "recomp", "build", "full", "shader_cache")
        b = os.path.join(FULL, "shader_cache")

        def noms(d, ext):
            return {f for f in os.listdir(d) if f.endswith(ext)}
        erreurs, info = [], []
        for ext in (".spv", ".glsl", ".meta"):
            na, nb = noms(a, ext), noms(b, ext)
            if na != nb:
                erreurs.append(f"{ext} : {len(na)} dans l'installation neuve, {len(nb)} dans l'arbre de référence")
            communs = na & nb
            pareils = sum(1 for f in communs if empreinte(os.path.join(a, f)) == empreinte(os.path.join(b, f)))
            if ext != ".spv" and pareils != len(communs):
                erreurs.append(f"{ext} : {len(communs) - pareils} différents de l'arbre de référence")
            info.append(f"{ext} {pareils}/{len(communs)} identiques")
        if erreurs:
            raise Echec("\n".join(erreurs))
        return f"{len(noms(a, '.spv'))} shaders ; " + ", ".join(info) + " (SPIR-V : selon la version de glslang)"

    def jeu(self):
        racine = self.etat.get("neuve")
        if not racine:
            raise Saute("pas d'installation neuve")
        full = os.path.join(racine, "recomp", "build", "full")
        texte, raison = essai_jeu(full, os.path.join(self.t, "essai_jeu"), 150)
        b = bilan_jeu(texte)
        if b["images"] < 7000 or b["absents"] or b["ignores"] or b["inconnus"] or b["plantage"]:
            raise Echec(f"{b} ({raison})\n" + fin(texte))
        if not b["textures"] or not b["a_la_demande"] or not b["rendus"]:
            raise Echec(f"économies de mémoire inactives sur une installation neuve : {b} ({raison})\n" + fin(texte))
        return (f"{b['images']} images en 150 s ; 0 shader inconnu ou absent ; 0 dessin ignoré ; {b['rendus']} Mo de "
                "textures rendus à Windows, shaders préparés à la demande")

    def sans_gpl(self):
        racine = self.etat.get("neuve")
        if not racine:
            raise Saute("pas d'installation neuve")
        full = os.path.join(racine, "recomp", "build", "full")
        texte, raison = essai_jeu(full, os.path.join(self.t, "essai_sans_gpl"), 60, {"RECOMP_NO_GPL": "1"})
        b = bilan_jeu(texte)
        if b["images"] < 1800 or b["absents"] or b["plantage"]:
            raise Echec(f"{b} ({raison})\n" + fin(texte))
        return f"{b['images']} images en 60 s sans pipeline libraries ; 0 shader absent"

    def lanceur(self):
        racine = self.etat.get("neuve")
        if not racine:
            raise Saute("pas d'installation neuve")
        if processus("lanceur.exe"):
            raise Saute("un launcher est ouvert")
        png = os.path.join(self.t, "lanceur.png")
        if os.path.exists(png):
            os.remove(png)
        env = dict(os.environ, LANCEUR_CAPTURE=png, LANCEUR_CAPTURE_T="6", LANCEUR_CAPTURE_QUITTER="1")
        p = subprocess.Popen([os.path.join(racine, "lanceur.exe")], cwd=racine, env=env)
        try:
            code = p.wait(40)
        except subprocess.TimeoutExpired:
            p.kill()
            raise Echec("le launcher ne s'est pas fermé après sa capture")
        if code or not os.path.exists(png) or os.path.getsize(png) < 50000:
            raise Echec(f"code {code}, capture {'absente' if not os.path.exists(png) else 'vide'}")
        return f"démarré, capturé ({png}), fermé"


# ---------------------------------------------------------------------------------------------- programme
def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("niveau", choices=("rapide", "complet"))
    ap.add_argument("--travail", default=os.path.join(tempfile.gettempdir(), "sar_verif"))
    ap.add_argument("--copie", default=None, help="copie du jeu (.nup) ; par défaut un .nup à la racine du projet")
    ap.add_argument("--seulement", default="", help="liste de vérifications (noms ci-dessus)")
    ap.add_argument("--garder", action="store_true", help="garder le dossier de travail (dont des fichiers dérivés du jeu)")
    a = ap.parse_args()
    if not a.copie:
        nups = glob.glob(os.path.join(PROJET, "*.nup"))
        a.copie = nups[0] if nups else None
    noms = RAPIDES + (COMPLETES if a.niveau == "complet" else [])
    if a.seulement:
        voulu = a.seulement.split(",")
        noms = [n for n in RAPIDES + COMPLETES if n in voulu]
    v = Verif(a)
    lignes, echecs = [], 0
    t_debut = time.time()
    dire(f"vérification {a.niveau} de la version {v.v} (travail : {v.t})")
    for n in noms:
        t0 = time.time()
        try:
            detail = getattr(v, n)()
            statut = "ok"
        except Saute as e:
            statut, detail = "sauté", str(e)
        except Echec as e:
            statut, detail = "ÉCHEC", str(e)
            echecs += 1
        except Exception as e:  # noqa: BLE001
            statut, detail = "ÉCHEC", f"{type(e).__name__} : {e}"
            echecs += 1
        d = time.time() - t0
        dire(f"{statut:6s} {n:11s} {d:6.0f} s  {detail.splitlines()[0] if detail else ''}")
        lignes.append((n, statut, d, detail))
    if not a.garder:   # fichiers dérivés du jeu : installations et exécutables d'essai, journaux du jeu
        for d in ("installation_deplacee", "installation [neuve]", "programdata", "essai_deplace", "essai_jeu",
                  "essai_sans_gpl", "paquet", "maj", "depot"):
            shutil.rmtree(os.path.join(v.t, d), ignore_errors=True)
    rapports = os.path.join(ICI, "rapports")
    os.makedirs(rapports, exist_ok=True)
    nom = time.strftime("%Y-%m-%d_%H%M") + f"_{v.v}_{a.niveau}.md"
    with open(os.path.join(rapports, nom), "w", encoding="utf-8") as f:
        f.write(f"# Vérification {a.niveau} — {NOM} {v.v}\n\n{time.strftime('%Y-%m-%d %H:%M')}, "
                f"{(time.time() - t_debut) / 60:.0f} min, {echecs} échec(s)\n\n| Vérification | Résultat | Durée | Détail |\n"
                "|---|---|---|---|\n")
        for n, s, d, det in lignes:
            f.write(f"| {n} | {s} | {d:.0f} s | {(det.splitlines()[0] if det else '').replace('|', '/')} |\n")
        for n, s, d, det in lignes:
            if s == "ÉCHEC":
                f.write(f"\n## {n}\n\n```\n{det}\n```\n")
    dire(f"{echecs} échec(s) en {(time.time() - t_debut) / 60:.0f} min ; rapport : {os.path.join(rapports, nom)}")
    return 1 if echecs else 0


if __name__ == "__main__":
    sys.exit(main())
