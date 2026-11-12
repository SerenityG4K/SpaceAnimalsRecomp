#!/usr/bin/env python3
"""Construit le jeu sur le PC du joueur, à partir de SA copie (.nup) : ce que lance « RECOMPILER LE JEU ».

  python -u preparer.py --copie <jeu.nup> [--fils N] [--etapes verifier,extraire,...] [--forcer]

Étapes (chacune est sautée si elle est déjà faite pour cette copie ET pour la version actuelle de son code : après une
mise à jour, seules les étapes dont le code a changé sont refaites) :
  verifier    la copie est bien Star Fox 2.0.0 (empreintes SHA-256 de l'index du .nup, sans relire les 30 Go)
  extraire    exécutables du patch 2.0.0 → <racine>/extracted/patch_v131072/exefs (empreintes vérifiées)
  images      images mémoire à plat des modules → <racine>/work/*.bin
  recompiler  code ARM64 → C++ → objets x86-64 (fullbuild.py, ≈ 35 min sur 16 fils ; objets inchangés réutilisés)
  lier        exécutable recomp/build/full/recomp_main.exe (link.py)
  shaders     shaders du jeu extraits de la romfs puis traduits en SPIR-V (romfs_shaders.py, shaderc.py)
  pipelines   tous les shaders compilés pour ce GPU (recomp_main --precompile → pipelines.cache) : aucune saccade ;
              une fois par exécutable construit, le jeu est aussi lancé quelques secondes pour relever les shaders qu'il
              crée au démarrage (absents des paquets de la romfs), qui sont alors traduits et compilés eux aussi

Les extraits vidéo du fond du launcher (recomp/launcher/extraits) sont fournis avec le paquet et publiés dans le dépôt.
Jusqu'à la 1.0.2, une étape « extraits » lançait le jeu pour les enregistrer. Elle n'existe plus depuis la 1.0.3
(demande de l'utilisateur, 2026-11-11 : « il n'y a pas de réenregistrement, le launcher utilise la vidéo du repo, rien
de plus ») : rien ici ne crée, ne remplace ni n'efface ces fichiers. « --etapes …,extraits » et « --sans-extraits »
(anciens launchers) sont acceptés et ignorés.

Le launcher lit la sortie : lignes « @etapes … », « @etape <id> », « @progres <0-1> », « @info <texte> »,
« @ok <id> », « @saute <id> », « @fini », « @erreur <texte> » ; les autres lignes vont au journal.

Tout ce que produit ce script dérive de la copie du joueur et reste sur son PC (jamais distribué). Seuls les extraits
fournis avec le paquet (gameplay enregistré par SerenityG4K) sont distribués, à sa demande.
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import zlib

LANCEUR = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(LANCEUR)
RACINE = os.path.dirname(RECOMP)                 # extracted/ et work/ (a64/module.py : PROJECT)
OUTILS = os.path.join(RACINE, "tools")           # nup.py, nso.py
FULL = os.path.join(RECOMP, "build", "full")
MARQUES = os.path.join(FULL, ".preparation")

TITRE = 0x0400E88025B1A000
VERSION = 131072                                 # patch 2.0.0
CONTENU = "patch_v131072"
MODULES = ["main", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "sdk"]
# empreintes SHA-256 (données décompressées) des exécutables du patch 2.0.0, telles que l'index du .nup les donne
EXEFS = {
    "main": "37466c29a0ce6261656b98670e6977fd7a6cfffe3ba4b411022313d694802716",
    "sdk": "f306d21b0507dce55bd5c8c9a9fa48ffbf11f3cdc246c9fe78079895241546c9",
    "subsdk0": "ab06fd16da9921d25a2196276ad523b510607625e257c03f7b5a4a7ca2b33c4c",
    "subsdk1": "409ed2912dda8737a3dd9753c2d34c16544dff7c863b36e2647b66f5eb337f49",
    "subsdk2": "517e7eb922bbe312740b9e4af4eab01aeeec6d5b230b5fa9ef28fcd897ed18d8",
    "subsdk3": "164f09d5abfc3a31df47e2665087aabbf704943286806d1a5b685de675a22cd8",
}
ETAPES = ["verifier", "extraire", "images", "recompiler", "lier", "shaders", "pipelines"]
# Code dont dépend chaque étape (chemins relatifs à la racine, motifs glob) et étape dont elle reprend le résultat :
# une étape est refaite quand ce code change, ou quand l'étape dont elle dépend a été refaite.
DEPENDANCES = {
    "extraire": ([], ["tools/nup.py"]),
    "images": (["extraire"], ["tools/nso.py"]),
    "recompiler": (["images"], ["recomp/recomp.py", "recomp/build.py", "recomp/fullbuild.py", "recomp/hooks.txt",
                                "recomp/lots.txt",
                                "recomp/a64/*.py", "recomp/runtime/*.h"]),
    "lier": (["recompiler"], ["recomp/link.py", "recomp/runtime/*.cpp", "recomp/runtime/os/*.cpp", "recomp/runtime/os/*.h"]),
    "shaders": ([], ["recomp/shaders/*.py"]),
}
# étape refaite à chaque construction : recomp_main --precompile ne recompile que si le GPU, le pilote ou les shaders
# ont changé (sa propre marque, pipelines.ok)
TOUJOURS = {"pipelines"}
# poids des modules dans la recompilation (millions d'instructions, ordre de grandeur)
POIDS = {"sdk": 2.2, "subsdk0": 0.1, "subsdk1": 0.6, "subsdk2": 0.2, "subsdk3": 0.01, "main": 51.3}


def dire(ligne):
    print(ligne, flush=True)


class Echec(Exception):
    pass


def empreinte_fichier(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for b in iter(lambda: f.read(1 << 22), b""):
            h.update(b)
    return h.hexdigest()


COPIE = []   # chemin de la copie, pour les sous-processus (fullbuild.py : RECOMP_NUP)
GCC = []     # dossier bin du compilateur fourni (déterminé une fois : copie éventuelle hors des espaces)


def gcc_sans_espaces(outils):
    """Dossier bin de GCC utilisable. GCC (WinLibs) échoue à l'édition de liens s'il est installé dans un dossier dont
    le chemin contient des espaces : il coupe ses propres chemins internes, et une jonction ne suffit pas (GCC 16
    remonte au chemin réel). Dans ce cas, le compilateur est copié une fois (≈ 400 Mo depuis la 1.0.1, ≈ 1 Go avec le
    GCC complet de la 1.0.0) dans C:\\ProgramData\\StarFoxRecomp\\gcc-<empreinte>, sans droits d'administrateur.
    L'empreinte est celle de l'archive WinLibs : une copie déjà faite par une autre installation (même complète, plus
    grosse) est réutilisée telle quelle, puisqu'elle contient tous les fichiers dont la construction a besoin."""
    gcc = os.path.abspath(os.path.join(outils, "gcc"))
    if " " not in gcc or os.name != "nt":
        return os.path.join(gcc, "bin")
    base = os.environ.get("ProgramData") or r"C:\ProgramData"
    if " " in base:
        base = os.environ.get("SystemDrive", "C:") + "\\"
    try:   # empreinte de l'archive de GCC installée (outils.py) : une copie par version du compilateur
        ident = open(os.path.join(outils, ".installe", "gcc"), "rb").read()
    except OSError:
        ident = str(os.path.getsize(os.path.join(gcc, "bin", "g++.exe"))).encode()
    dest = os.path.join(base, "StarFoxRecomp", "gcc-" + hashlib.sha1(ident).hexdigest()[:10])
    if os.path.exists(os.path.join(dest, ".complet")):
        return os.path.join(dest, "bin")
    dire("@info compiler → " + dest)
    dire(f"copying the compiler to {dest} (path without spaces, only once)")
    try:
        shutil.rmtree(dest + ".part", ignore_errors=True)
        shutil.copytree(gcc, dest + ".part")
        if os.path.exists(dest):
            shutil.rmtree(dest)
        os.replace(dest + ".part", dest)
        open(os.path.join(dest, ".complet"), "w").close()
        return os.path.join(dest, "bin")
    except OSError as e:
        dire(f"copy impossible ({e}): the bundled compiler may fail in {gcc}")
        return os.path.join(gcc, "bin")


def dossiers_outils():
    """outils/ du paquet (à côté de recomp/), ou recomp/outils (arbre de développement)."""
    return [d for d in (os.path.join(RACINE, "outils"), os.path.join(RECOMP, "outils")) if os.path.isdir(d)]


def outils_env():
    """PATH des sous-processus : compilateur et glslang fournis (outils/), sinon MSYS2, sinon le PATH.
    RECOMP_SANS_MSYS2=1 ignore MSYS2 (vérification du paquet comme sur un PC où rien n'est installé)."""
    env = dict(os.environ)
    chemins = []
    for base in dossiers_outils():
        if os.path.isdir(os.path.join(base, "gcc", "bin")):
            if not GCC:
                GCC.append(gcc_sans_espaces(base))
            chemins.append(GCC[0])
        if os.path.isdir(os.path.join(base, "glslang", "bin")):
            chemins.append(os.path.join(base, "glslang", "bin"))
        for nom in ("glslangValidator.exe", "glslang.exe"):   # glslang 14+ : glslang.exe seulement
            exe = os.path.join(base, "glslang", "bin", nom)
            if os.path.exists(exe) and "RECOMP_GLSLANG" not in env:
                env["RECOMP_GLSLANG"] = exe
    msys = r"C:\msys64\ucrt64\bin"
    if not os.environ.get("RECOMP_SANS_MSYS2") and os.path.isdir(msys):
        chemins.append(msys)
    env["PATH"] = os.pathsep.join(chemins + [env.get("PATH", "")])
    env["PYTHONUTF8"] = "1"
    if COPIE:
        env["RECOMP_NUP"] = COPIE[0]
    env["PYTHONIOENCODING"] = "utf-8"
    return env


def lancer(cmd, cwd, analyse=None, env=None):
    """Lance une commande, relaie sa sortie au journal ; `analyse(ligne)` peut émettre la progression."""
    p = subprocess.Popen(cmd, cwd=cwd, env=env or outils_env(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace", bufsize=1)
    queue = []
    for ligne in p.stdout:
        ligne = ligne.rstrip("\r\n")
        if not ligne:
            continue
        if not ligne.startswith("@progres "):   # (avancement d'un sous-processus : relayé par `analyse`, pas au journal)
            dire("  " + ligne[:300])
        queue.append(ligne)
        del queue[:-40]
        if analyse:
            analyse(ligne)
    if p.wait():
        nom = next((os.path.basename(c) for c in cmd if str(c).endswith((".py", ".exe"))), os.path.basename(cmd[0]))
        e = Echec(f"{nom} failed (code {p.returncode}): " + (queue[-1] if queue else ""))
        e.sortie = list(queue)   # dernières lignes (diagnostic : manque de mémoire…)
        raise e


class Copie:
    def __init__(self, chemin):
        sys.path.insert(0, OUTILS)
        import nup  # noqa: E402
        self.chemin = chemin
        try:
            self.n = nup.Nup(chemin)
        except Exception as e:  # noqa: BLE001
            raise Echec(f"this file is not a readable .nup game copy ({e})")
        self.index = None
        for i, c in enumerate(self.n.contents):
            if c["type"] == 1 and (c["title_id"] & ~0xFFF) == TITRE and c.get("version") == VERSION:
                self.index = i
        if self.index is None:
            titres = ", ".join(f"{c['title_id']:016X} v{c.get('version')}" for c in self.n.contents)
            raise Echec(f"game copy not recognized: Star Fox (0400E88025B1A000) with update 2.0.0 is required "
                        f"(v131072); found: {titres or 'no content'}")
        self.fichiers = {p: fi for p, fi in self.n.walk(self.index)}

    def verifier(self):
        for m, attendu in EXEFS.items():
            fi = self.fichiers.get(f"exefs/{m}")
            if fi is None:
                raise Echec(f"exefs/{m} missing from the game copy")
            h = fi["sha256"].hex() if isinstance(fi["sha256"], (bytes, bytearray)) else str(fi["sha256"])
            if h != attendu:
                raise Echec(f"exefs/{m} does not match version 2.0.0 (modified game copy or other version)")
        dz = self.fichiers.get("romfs/data.zip")
        if dz is None or dz["compressed"]:
            raise Echec("romfs/data.zip missing from the game copy, or in an unexpected format")

    def signature(self):
        return EXEFS["main"][:16] + ":" + os.path.basename(self.chemin)


def verifier_pc(construire):
    """Ce qui ferait échouer la construction ou le jeu chez un joueur, signalé clairement dès le début."""
    if os.name == "nt":
        import ctypes
        if not ctypes.windll.kernel32.IsProcessorFeaturePresent(40):   # PF_AVX2_INSTRUCTIONS_AVAILABLE
            raise Echec("this processor lacks the AVX2 instructions: the recompiled game cannot run on it")
        if not os.path.exists(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "vulkan-1.dll")):
            dire("warning: Vulkan not found (vulkan-1.dll): install your graphics card driver")
        try:   # GCC passe par les API ANSI : un chemin hors de la page de code de Windows le ferait échouer
            RACINE.encode("mbcs", errors="strict")
        except UnicodeEncodeError:
            raise Echec("the folder path contains characters the compiler cannot handle: "
                        "move the game folder (for example to C:\\Jeux\\StarFox)")
    if construire:
        libre = shutil.disk_usage(RACINE).free
        if libre < 6e9:
            raise Echec(f"not enough disk space: {libre / 1e9:.1f} GB free, about 6 are needed")


def marque(etape):
    return os.path.join(MARQUES, etape + ".ok")


def deja_fait(etape, sig):
    try:
        return open(marque(etape), encoding="utf-8").read().strip() == sig
    except OSError:
        return False


def noter(etape, sig):
    os.makedirs(MARQUES, exist_ok=True)
    with open(marque(etape), "w", encoding="utf-8") as f:
        f.write(sig + "\n")


def signatures(copie_sig):
    """Signature de chaque étape : la copie, l'empreinte du code dont elle dépend et la signature des étapes dont elle
    reprend le résultat. Elle change après une mise à jour qui touche ce code : l'étape est alors refaite."""
    sig = {}

    def code(motifs):
        h = hashlib.sha256()
        for m in motifs:
            for p in sorted(glob.glob(os.path.join(glob.escape(RACINE), *m.split("/")))):
                h.update(os.path.relpath(p, RACINE).replace("\\", "/").encode() + b"\0")
                with open(p, "rb") as f:
                    h.update(hashlib.sha256(f.read()).digest())
        return h.hexdigest()[:16]

    for e in ETAPES:   # (les étapes dont une étape dépend la précèdent dans ETAPES)
        avant, motifs = DEPENDANCES.get(e, ([], []))
        h = hashlib.sha256(code(motifs).encode())
        for d in avant:
            h.update(sig[d].encode())
        sig[e] = copie_sig + ":" + h.hexdigest()[:16]
    return sig


def version_sources():
    try:
        return open(os.path.join(RECOMP, "VERSION"), encoding="utf-8").read().strip()
    except OSError:
        return "?"


# ---------------------------------------------------------------------------------------------- étapes
def e_extraire(copie, a):
    dest = os.path.join(RACINE, "extracted", CONTENU, "exefs")
    a_faire = [m for m in EXEFS if not (os.path.exists(os.path.join(dest, m))
                                         and empreinte_fichier(os.path.join(dest, m)) == EXEFS[m])]
    for k, m in enumerate(a_faire):
        dire(f"@info exefs/{m}")
        ok = copie.n.extract(copie.fichiers[f"exefs/{m}"], os.path.join(dest, m), verify=True)
        if ok is False:
            raise Echec(f"exefs/{m}: SHA-256 hash differs after extraction (damaged game copy?)")
        dire(f"@progres {(k + 1) / len(a_faire):.3f}")


def e_images(copie, a):
    os.makedirs(os.path.join(RACINE, "work"), exist_ok=True)
    py = sys.executable
    for k, m in enumerate(MODULES):
        nso = os.path.join(RACINE, "extracted", CONTENU, "exefs", m)
        img = os.path.join(RACINE, "work", f"{m}.bin")
        if os.path.exists(img) and os.path.getmtime(img) >= os.path.getmtime(nso) and not a.forcer:
            continue
        dire(f"@info {m}")
        lancer([py, "-X", "utf8", os.path.join(OUTILS, "nso.py"), "unpack", nso, img], RACINE)
        dire(f"@progres {(k + 1) / len(MODULES):.3f}")


def e_recompiler(copie, a):
    total = sum(POIDS.values())
    fait = {m: 0.0 for m in POIDS}
    actuel = {"m": None}

    def analyse(l):
        m = re.match(r"(\w+): (\d+)/(\d+) batches compiled", l)
        if m and m.group(1) in fait:
            fait[m.group(1)] = int(m.group(2)) / max(1, int(m.group(3)))
        m2 = re.match(r"(\w+): (round \d+|\d+ functions)", l)
        if m2 and m2.group(1) in fait and actuel["m"] != m2.group(1):
            actuel["m"] = m2.group(1)
            dire(f"@info {m2.group(1)}")
        m3 = re.match(r"(\w+): done", l)
        if m3 and m3.group(1) in fait:
            fait[m3.group(1)] = 1.0
        dire(f"@progres {sum(fait[k] * POIDS[k] for k in fait) / total:.3f}")

    ordre = ["sdk", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "main"]
    compilateur_a_niveau()
    fils = fils_selon_memoire(a.fils)
    for essai in range(4):
        try:
            lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "fullbuild.py"), *ordre, "-j", str(fils)], RECOMP,
                   analyse)
            return
        except Echec as e:
            # certains lots demandent bien plus que la moyenne : si la mémoire a manqué (MemoryError dans Python, « out
            # of memory » dans cc1plus), on recommence avec deux fois moins de tâches. Les lots déjà compilés sont
            # gardés (empreinte de chaque objet) : seuls ceux qui ont échoué sont refaits.
            memoire = any("out of memory" in l or "MemoryError" in l for l in getattr(e, "sortie", []))
            if not memoire or fils == 1 or essai == 3:
                raise
            fils = max(1, fils // 2)
            dire(f"not enough memory during recompilation: retrying with {fils} parallel compilation(s) "
                 "(batches already compiled are kept)")
            dire("@info " + ("memory: retrying"))


# Mémoire d'une tâche de recompilation (relevé du 2026-11-10 pendant main) : le processus Python de fullbuild.py garde
# ≈ 0,85 Go pendant que g++ compile son lot, et cc1plus prend jusqu'à 1 Go. 15 tâches demandaient ≈ 28 Go : sur un PC
# où les programmes ouverts en prennent déjà beaucoup, la construction échouait (MemoryError) ou passait des minutes
# dans le fichier d'échange. Le nombre de tâches est donc borné par la mémoire libre au départ.
# Depuis la 1.0.3, fullbuild.py génère les lots dans un ou deux processus Python seulement (≈ 1,7 Go en tout, compris
# dans la réserve) et lance les compilateurs à part : une tâche ne coûte plus que son compilateur (≈ 1,3 Go en moyenne,
# jusqu'à 2,5 Go pour les plus gros lots ; un manque de mémoire relance l'étape avec deux fois moins de tâches).
# Avec clang (fourni depuis la 1.0.3, build.compilateur_genere), une compilation prend ≈ 180 Mo, 330 Mo pour le plus
# gros lot (relevé du 2026-11-11, -O1) : 0,4 Go par tâche laisse de la marge. Les processus Python de fullbuild.py ne
# prennent plus que ≈ 170 Mo chacun (≈ 1 Go jusqu'à la 1.0.2) : 2 Go de réserve pour eux et pour le système.
GO_PAR_TACHE = 1.4
GO_PAR_TACHE_CLANG = 0.4
GO_RESERVE = 2.0


def avec_clang():
    """Le code généré sera-t-il compilé par clang ? Même règle que build.compilateur_genere : RECOMP_CXX, sinon
    clang++.exe à côté du g++ que la construction trouve (outils fournis, MSYS2, PATH)."""
    choix = os.environ.get("RECOMP_CXX")
    if choix:
        return "clang" in os.path.basename(choix).lower()
    gxx = shutil.which("g++", path=outils_env()["PATH"])
    return bool(gxx) and os.path.exists(os.path.join(os.path.dirname(gxx), "clang++.exe"))


def compilateur_a_niveau():
    """Joueur venu de la 1.0.2 (g++ seul, archive gardée par la mise à jour) qui n'a pas d'objets à garder : la
    construction complète est 3 fois plus rapide et demande 3 fois moins de mémoire par tâche avec Clang, donc
    l'archive actuelle du compilateur est installée d'abord (outils.py --niveau, une seule fois). Tant qu'il a ses
    objets compilés par g++, rien n'est fait : ils restent valables. Un échec (pas de réseau…) laisse g++ en place."""
    if os.environ.get("RECOMP_CXX") or avec_clang():
        return
    objets = glob.glob(os.path.join(glob.escape(RECOMP), "build", "full", "main", "main_*.o"))
    if len(objets) >= 100:
        return
    for base in dossiers_outils():
        if not os.path.exists(os.path.join(base, ".installe", "gcc")):
            continue
        dire("@info Clang")
        dire("full build to do with g++ only: installing the current compiler (Clang, only once)")
        r = subprocess.run([sys.executable, "-X", "utf8", os.path.join(LANCEUR, "outils.py"), "--dest", base, "--niveau"],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        for ligne in (r.stdout + r.stderr).splitlines()[-8:]:
            dire("  " + ligne)
        if r.returncode:
            dire("installation impossible: the build continues with g++")
        GCC.clear()   # dossier du compilateur à redéterminer (copie éventuelle hors des espaces)
        return


def memoire_libre():
    """(Go de RAM disponibles, Go validables disponibles : RAM + fichier d'échange).
    RECOMP_MEM_LIBRE_GO=n : valeur imposée (essais : simuler un PC qui a peu de mémoire libre)."""
    if os.environ.get("RECOMP_MEM_LIBRE_GO"):
        v = float(os.environ["RECOMP_MEM_LIBRE_GO"])
        return v, v
    import ctypes

    class Etat(ctypes.Structure):
        _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                    ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                    ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                    ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]
    e = Etat()
    e.dwLength = ctypes.sizeof(Etat)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(e)):
        raise OSError("GlobalMemoryStatusEx")
    return e.ullAvailPhys / 1e9, e.ullAvailPageFile / 1e9


def fils_selon_memoire(demandes):
    try:
        ram, validable = memoire_libre()
    except (OSError, AttributeError):
        return demandes
    clang = avec_clang()
    n = max(1, min(demandes, int((min(ram, validable) - GO_RESERVE) / (GO_PAR_TACHE_CLANG if clang else GO_PAR_TACHE))))
    dire(f"free memory: {ram:.0f} GB of RAM, {validable:.0f} GB of commit, compiler {'clang' if clang else 'g++'}"
         f" → {n} parallel compilation(s)"
         + (f" instead of {demandes} (closing other programs speeds up the build)" if n < demandes else ""))
    return n


def png(largeur, hauteur, rgba):
    """Image RGBA 8 bits → PNG (sans filtre, zlib)."""
    ligne = largeur * 4
    brut = b"".join(b"\0" + rgba[y * ligne:(y + 1) * ligne] for y in range(hauteur))

    def bloc(genre, donnees):
        return struct.pack(">I", len(donnees)) + genre + donnees + struct.pack(">I", zlib.crc32(genre + donnees))
    return (b"\x89PNG\r\n\x1a\n" + bloc(b"IHDR", struct.pack(">IIBBBBB", largeur, hauteur, 8, 6, 0, 0, 0))
            + bloc(b"IDAT", zlib.compress(brut, 9)) + bloc(b"IEND", b""))


def jpeg_vers_ico(jpg, ico, tailles=(256, 128, 64, 48, 32, 24, 16)):
    """JPEG → ICO (une image PNG par taille). Décodage et réduction par le composant d'images de Windows (WIC), appelé
    directement : aucun programme extérieur."""
    import ctypes
    import uuid
    from ctypes import POINTER, byref, c_void_p, wintypes

    class GUID(ctypes.Structure):
        _fields_ = [("octets", ctypes.c_ubyte * 16)]

    def guid(texte):
        g = GUID()
        ctypes.memmove(byref(g), uuid.UUID(texte).bytes_le, 16)
        return g

    def methode(objet, rang, *types):   # méthode n° rang de la table virtuelle d'une interface COM
        table = ctypes.cast(objet, POINTER(POINTER(c_void_p))).contents
        f = ctypes.WINFUNCTYPE(ctypes.HRESULT, c_void_p, *types)(table[rang])
        return lambda *args: f(objet, *args)

    def liberer(objet):
        if objet:
            ctypes.WINFUNCTYPE(ctypes.c_ulong, c_void_p)(ctypes.cast(objet, POINTER(POINTER(c_void_p))).contents[2])(objet)

    ole32 = ctypes.windll.ole32
    init = ole32.CoInitializeEx(None, 0)
    fabrique, decodeur, image, conv = c_void_p(), c_void_p(), c_void_p(), c_void_p()
    images = []
    try:
        clsid, iid = guid("cacaf262-9370-4615-a13b-9f5539da4c0a"), guid("ec5ec8a9-c395-4314-9c77-54d7a935ff70")
        hr = ole32.CoCreateInstance(byref(clsid), None, 1, byref(iid), byref(fabrique))   # CLSCTX_INPROC_SERVER
        if hr != 0:
            raise OSError(f"WIC indisponible (0x{hr & 0xFFFFFFFF:08X})")
        # IWICImagingFactory::CreateDecoderFromFilename (GENERIC_READ), IWICBitmapDecoder::GetFrame
        methode(fabrique, 3, ctypes.c_wchar_p, c_void_p, wintypes.DWORD, ctypes.c_int, POINTER(c_void_p))(
            jpg, None, 0x80000000, 0, byref(decodeur))
        methode(decodeur, 13, wintypes.UINT, POINTER(c_void_p))(0, byref(image))
        # IWICImagingFactory::CreateFormatConverter → 32 bits BGRA
        methode(fabrique, 10, POINTER(c_void_p))(byref(conv))
        bgra = guid("6fddc324-4e03-4bfe-b185-3d77768dc90f")
        methode(conv, 8, c_void_p, POINTER(GUID), ctypes.c_int, c_void_p, ctypes.c_double, ctypes.c_int)(
            image, byref(bgra), 0, None, 0.0, 0)
        for t in tailles:
            pixels = None
            for mode in (4, 3):   # interpolation : cubique haute qualité (Windows 10), sinon Fant
                ech = c_void_p()
                try:
                    methode(fabrique, 11, POINTER(c_void_p))(byref(ech))   # CreateBitmapScaler
                    methode(ech, 8, c_void_p, wintypes.UINT, wintypes.UINT, ctypes.c_int)(conv, t, t, mode)
                    tampon = (ctypes.c_ubyte * (t * t * 4))()
                    methode(ech, 7, c_void_p, wintypes.UINT, wintypes.UINT, POINTER(ctypes.c_ubyte))(None, t * 4, t * t * 4, tampon)
                    pixels = bytes(tampon)
                    break
                except OSError:
                    if mode == 3:
                        raise
                finally:
                    liberer(ech)
            rgba = bytearray(pixels)
            rgba[0::4], rgba[2::4] = pixels[2::4], pixels[0::4]   # BGRA → RGBA
            images.append((t, png(t, t, bytes(rgba))))
    finally:
        for o in (conv, image, decodeur, fabrique):
            liberer(o)
        if init in (0, 1):
            ole32.CoUninitialize()
    debut = 6 + 16 * len(images)
    repertoire, donnees = b"", b""
    for t, p in images:
        c = 0 if t >= 256 else t
        repertoire += struct.pack("<BBBBHHII", c, c, 0, 0, 1, 32, len(p), debut + len(donnees))
        donnees += p
    with open(ico + ".part", "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(images)) + repertoire + donnees)
    os.replace(ico + ".part", ico)


def extraire_icone(copie):
    """Icône du jeu, tirée des métadonnées de la copie (bloc 3 : JPEG) → build/full/icone.ico, intégrée à
    recomp_main.exe par link.py. Elle dérive du jeu : elle reste sur le PC du joueur."""
    ico = os.path.join(FULL, "icone.ico")
    jpg = None
    for i in range(len(copie.n.contents)):
        try:
            m = copie.n.meta(i)
        except Exception:  # noqa: BLE001
            continue
        off = 0
        while off + 8 <= len(m):
            genre, taille = struct.unpack_from("<II", m, off)
            if genre == 3 and m[off + 8:off + 10] == b"\xff\xd8":
                jpg = m[off + 8:off + 8 + taille]
            off += 8 + taille
            if taille == 0:
                break
        if jpg:
            break
    if not jpg:
        dire("game icon missing from the game copy metadata: executable without icon")
        return
    os.makedirs(FULL, exist_ok=True)
    chemin_jpg = os.path.join(FULL, "icone.jpg")
    with open(chemin_jpg, "wb") as f:
        f.write(jpg)
    try:
        jpeg_vers_ico(chemin_jpg, ico)
        dire(f"game icon: {ico} ({os.path.getsize(ico)} bytes)")
    except Exception as e:  # noqa: BLE001   (sans icône, le jeu fonctionne pareil)
        dire(f"icon not converted: {e}")
    finally:
        os.remove(chemin_jpg)


def e_lier(copie, a):
    dire("@progres 0.1")
    extraire_icone(copie)
    dire("@progres 0.2")
    lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "link.py")], RECOMP)
    if not os.path.exists(os.path.join(FULL, "recomp_main.exe")):
        raise Echec("the executable was not produced")


def compter(dossier, motif):
    # glob.escape : un dossier d'installation qui contient [ ] * ou ? (« Jeux [Switch 2] ») n'est pas un motif. Sans
    # cela, rien n'était compté : la traduction des shaders restait à 35 % puis échouait (« trop peu de shaders
    # traduits », issue n° 1, 2026-11-11), et les signatures des étapes ne voyaient aucun fichier de code.
    try:
        return len(glob.glob(os.path.join(glob.escape(dossier), motif)))
    except OSError:
        return 0


def suivre(dossier, motif, attendu, debut, fin, arret):
    """Progression d'après le nombre de fichiers écrits (les scripts de shaders ne la donnent pas)."""
    while not arret.is_set():
        n = compter(dossier, motif)
        dire(f"@progres {debut + (fin - debut) * min(1.0, n / attendu):.3f}")
        arret.wait(1.0)


def e_shaders(copie, a):
    dump, cache = os.path.join(FULL, "shaders"), os.path.join(FULL, "shader_cache")
    arret = threading.Event()
    dire("@info romfs")
    t = threading.Thread(target=suivre, args=(dump, "h*_r.bin", 2828, 0.0, 0.35, arret), daemon=True)
    t.start()
    try:
        lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "shaders", "romfs_shaders.py"), "--nup", copie.chemin,
                "--out", dump], RECOMP)
    finally:
        arret.set()
        t.join()
    arret = threading.Event()
    dire("@info SPIR-V")
    t = threading.Thread(target=suivre, args=(cache, "*.spv", 2388, 0.35, 1.0, arret), daemon=True)
    t.start()
    try:
        lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "shaders", "shaderc.py"), "-j", str(a.fils)], RECOMP)
    finally:
        arret.set()
        t.join()
    n = compter(cache, "*.spv")
    dire(f"shaders: {compter(dump, 'h*_r.bin')} extracted from the game copy, {n} translated to SPIR-V ({cache})")
    if n < 2000:
        raise Echec(f"too few shaders translated ({n} out of ≈ 2 300: see the '[glslang]' lines of the log)")


def e_pipelines(copie, a):
    """Tous les shaders compilés pour ce GPU (comme les jeux récents au premier lancement) : recomp_main --precompile
    écrit pipelines.cache, que le jeu charge au démarrage. Rien à refaire si GPU, pilote et shaders sont inchangés."""
    exe = os.path.join(FULL, "recomp_main.exe")

    def analyse(l):
        if l.startswith("@progres ") or l.startswith("@info "):
            dire(l)
        elif l.startswith("@saute"):
            dire("@info " + l[7:])
        elif l.startswith("@erreur "):
            raise Echec("shader precompilation: " + l[8:])

    lancer([exe, "--precompile"], FULL, analyse)
    # une fois par exécutable : les shaders que le jeu crée au démarrage, puis leur compilation pour ce GPU
    cle = releve_cle()
    if cle and not deja_fait("releve", cle):
        avant = compter(os.path.join(FULL, "shader_cache"), "*.spv")
        if releve_shaders(a):
            noter("releve", cle)
        if compter(os.path.join(FULL, "shader_cache"), "*.spv") != avant:
            dire("@info " + "game shaders")
            lancer([exe, "--precompile"], FULL, analyse)


def releve_cle():
    """Identité de l'exécutable construit (taille, date) : le relevé est refait après chaque nouvelle édition de liens."""
    try:
        s = os.stat(os.path.join(FULL, "recomp_main.exe"))
    except OSError:
        return None
    return f"{s.st_size}:{int(s.st_mtime)}"


def releve_shaders(a):
    """Le jeu crée au démarrage des shaders qui ne sont dans aucun paquet de shaders de la romfs (80 traduisibles, et les
    79 du réseau DLSS de la console) : il les écrit dans shaders/ (« shader inconnu »). On le lance quelques secondes,
    sans fenêtre visible (RECOMP_WINDOW_HIDDEN, depuis la 1.0.3 : le joueur ne voit plus le jeu s'ouvrir tout seul),
    sans le son et avec une sauvegarde à part, jusqu'à sa 600e image, puis on traduit ces nouveaux shaders.
    Jusqu'à la 1.0.1, l'enregistrement des extraits faisait ce relevé au passage. Renvoie False si le jeu n'a pas
    atteint sa 600e image (le relevé sera retenté à la prochaine construction) ; ce n'est jamais bloquant."""
    exe = os.path.join(FULL, "recomp_main.exe")
    save = os.path.join(FULL, "essai_releve")
    journal = os.path.join(FULL, "releve_shaders.log")
    shutil.rmtree(save, ignore_errors=True)
    os.makedirs(save, exist_ok=True)
    try:
        os.remove(journal)
    except OSError:
        pass
    env = outils_env()
    env.update({"RECOMP_SAVE_DIR": save, "RECOMP_WINDOW": "windowed", "RECOMP_VOLUME": "0", "RECOMP_WINDOW_HIDDEN": "1"})
    dire("@info jeu")
    p = subprocess.Popen([exe, "--log", journal], cwd=FULL, env=env, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL)
    t0, limite, texte, fini = time.time(), 180, "", False
    try:
        while p.poll() is None and time.time() - t0 < limite:
            try:
                texte = open(journal, encoding="utf-8", errors="replace").read()
            except OSError:
                pass
            if " frame 600:" in texte:
                fini = True
                break
            dire(f"@progres {min(0.99, (time.time() - t0) / 30):.3f}")
            time.sleep(0.5)
    finally:
        if p.poll() is None:
            p.kill()
            try:
                p.wait(15)
            except subprocess.TimeoutExpired:
                pass
        shutil.rmtree(save, ignore_errors=True)
    n = texte.count("unknown shader")
    dire(f"shader survey: {n} created by the game at startup in {time.time() - t0:.0f} s"
         + ("" if fini else f" (survey incomplete: see {journal})"))
    lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "shaders", "shaderc.py"), "--nouveaux", "-j", str(a.fils)],
           RECOMP)
    return fini


FONCTIONS = {"extraire": e_extraire, "images": e_images, "recompiler": e_recompiler, "lier": e_lier,
             "shaders": e_shaders, "pipelines": e_pipelines}


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--copie", required=True)
    ap.add_argument("--fils", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--etapes", default=",".join(ETAPES))
    ap.add_argument("--forcer", action="store_true")
    ap.add_argument("--sans-extraits", action="store_true", help="sans effet (accepté pour les anciens launchers)")
    ap.add_argument("--a-refaire", action="store_true",
                    help="écrit « @a_refaire étape,étape » (les étapes que la construction referait) et ne fait rien d'autre")
    a = ap.parse_args()
    if a.a_refaire:
        # Question du launcher à son démarrage : le jeu construit est-il encore celui de ces sources et de cette copie ?
        # Une étape dont la signature a changé (mise à jour du code dont elle dépend, autre copie du jeu) est à refaire :
        # le launcher impose alors la reconstruction avant de laisser jouer. Rien n'est écrit ni construit ici.
        try:
            copie = Copie(a.copie)
            copie.verifier()
            sig = signatures(copie.signature())
            dire("@a_refaire " + ",".join(e for e in ETAPES if e not in TOUJOURS and e != "verifier"
                                          and not deja_fait(e, sig[e])))
            return 0
        except Exception as ex:  # noqa: BLE001
            dire(f"@erreur {type(ex).__name__}: {ex}")
            return 1
    # Le jeu refuse de démarrer quand il a été construit par une autre version que les sources installées
    # (host_main.cpp, version_du_jeu_a_jour) ; pendant une construction, c'est justement le cas tant que la version
    # n'est pas notée à la fin : le lancement du jeu fait ici (relevé des shaders) est exempté.
    os.environ["RECOMP_CONSTRUCTION"] = "1"
    etapes = [e for e in a.etapes.split(",") if e in ETAPES]
    dire("@etapes " + ",".join(etapes))
    try:   # copie de secours des sauvegardes du joueur avant de toucher à quoi que ce soit (sauvegardes.py)
        sys.path.insert(0, LANCEUR)
        import sauvegardes
        sauvegardes.secours("construction", dire)
    except Exception as ex:  # noqa: BLE001
        dire(f"saves: {type(ex).__name__}: {ex}")
    try:   # en tête du journal : ce qu'il faut pour comprendre un échec chez un joueur (rapport de bug)
        import rapport
        dire(rapport.ligne_systeme())
        dire(f"Python {sys.version.split()[0]}; {a.fils} thread(s) requested; steps {','.join(etapes)}"
             + ("; redo everything" if a.forcer else ""))
    except Exception as ex:  # noqa: BLE001
        dire(f"log header: {type(ex).__name__}: {ex}")
    t0 = time.time()
    try:
        dire("@etape verifier")
        copie = Copie(a.copie)
        COPIE.append(os.path.abspath(a.copie))
        copie.verifier()
        sig = signatures(copie.signature())
        verifier_pc(any(e in etapes and (a.forcer or not deja_fait(e, sig[e])) for e in ("recompiler", "lier")))
        dire("@ok verifier")
        for e in etapes:
            if e == "verifier":
                continue
            dire(f"@etape {e}")
            if not a.forcer and e not in TOUJOURS and deja_fait(e, sig[e]):
                dire(f"@saute {e}")
                continue
            t = time.time()
            FONCTIONS[e](copie, a)
            if e not in TOUJOURS:
                noter(e, sig[e])
            dire(f"@ok {e}")
            dire(f"{e}: {time.time() - t:.0f} s")
        if set(etapes) >= set(ETAPES):   # construction complète : jeu à jour pour cette version
            noter("version", version_sources())
    except Echec as ex:
        dire(f"@erreur {ex}")
        return 1
    except Exception as ex:  # noqa: BLE001
        dire(f"@erreur {type(ex).__name__}: {ex}")
        return 1
    dire(f"@fini {time.time() - t0:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
