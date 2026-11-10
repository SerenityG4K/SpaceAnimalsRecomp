#!/usr/bin/env python3
"""Construit le jeu sur le PC du joueur, à partir de SA copie (.nup) : ce que lance « RECOMPILER LE JEU ».

  python -u preparer.py --copie <jeu.nup> [--fils N] [--etapes verifier,extraire,...] [--forcer] [--sans-extraits]

Étapes (chacune est sautée si elle est déjà faite pour cette copie ET pour la version actuelle de son code : après une
mise à jour, seules les étapes dont le code a changé sont refaites) :
  verifier    la copie est bien Star Fox 2.0.0 (empreintes SHA-256 de l'index du .nup, sans relire les 30 Go)
  extraire    exécutables du patch 2.0.0 → <racine>/extracted/patch_v131072/exefs (empreintes vérifiées)
  images      images mémoire à plat des modules → <racine>/work/*.bin
  recompiler  code ARM64 → C++ → objets x86-64 (fullbuild.py, ≈ 35 min sur 16 fils ; objets inchangés réutilisés)
  lier        exécutable recomp/build/full/recomp_main.exe (link.py)
  shaders     shaders du jeu extraits de la romfs puis traduits en SPIR-V (romfs_shaders.py, shaderc.py)
  pipelines   tous les shaders compilés pour ce GPU (recomp_main --precompile → pipelines.cache) : aucune saccade
  extraits    quelques extraits des cinématiques, enregistrés par le jeu recompilé, pour le fond du launcher

Le launcher lit la sortie : lignes « @etapes … », « @etape <id> », « @progres <0-1> », « @info <texte> »,
« @ok <id> », « @saute <id> », « @fini », « @erreur <texte> » ; les autres lignes vont au journal.

Tout ce que produit ce script dérive de la copie du joueur et reste sur son PC (jamais distribué).
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
EXTRAITS = os.path.join(LANCEUR, "extraits")

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
ETAPES = ["verifier", "extraire", "images", "recompiler", "lier", "shaders", "pipelines", "extraits"]
# Code dont dépend chaque étape (chemins relatifs à la racine, motifs glob) et étape dont elle reprend le résultat :
# une étape est refaite quand ce code change, ou quand l'étape dont elle dépend a été refaite.
DEPENDANCES = {
    "extraire": ([], ["tools/nup.py"]),
    "images": (["extraire"], ["tools/nso.py"]),
    "recompiler": (["images"], ["recomp/recomp.py", "recomp/build.py", "recomp/fullbuild.py", "recomp/hooks.txt",
                                "recomp/a64/*.py", "recomp/runtime/*.h"]),
    "lier": (["recompiler"], ["recomp/link.py", "recomp/runtime/*.cpp", "recomp/runtime/os/*.cpp", "recomp/runtime/os/*.h"]),
    "shaders": ([], ["recomp/shaders/*.py"]),
    "extraits": ([], []),
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
    dire("@info compilateur → " + dest)
    dire(f"copie du compilateur vers {dest} (chemin sans espaces, une seule fois)")
    try:
        shutil.rmtree(dest + ".part", ignore_errors=True)
        shutil.copytree(gcc, dest + ".part")
        if os.path.exists(dest):
            shutil.rmtree(dest)
        os.replace(dest + ".part", dest)
        open(os.path.join(dest, ".complet"), "w").close()
        return os.path.join(dest, "bin")
    except OSError as e:
        dire(f"copie impossible ({e}) : le compilateur fourni risque d'échouer dans {gcc}")
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
        raise Echec(f"{nom} a échoué (code {p.returncode}) : "
                    + (queue[-1] if queue else ""))


class Copie:
    def __init__(self, chemin):
        sys.path.insert(0, OUTILS)
        import nup  # noqa: E402
        self.chemin = chemin
        try:
            self.n = nup.Nup(chemin)
        except Exception as e:  # noqa: BLE001
            raise Echec(f"ce fichier n'est pas une copie .nup lisible ({e})")
        self.index = None
        for i, c in enumerate(self.n.contents):
            if c["type"] == 1 and (c["title_id"] & ~0xFFF) == TITRE and c.get("version") == VERSION:
                self.index = i
        if self.index is None:
            titres = ", ".join(f"{c['title_id']:016X} v{c.get('version')}" for c in self.n.contents)
            raise Echec(f"copie non reconnue : il faut Star Fox (0400E88025B1A000) avec la mise à jour 2.0.0 "
                        f"(v131072) ; trouvé : {titres or 'aucun contenu'}")
        self.fichiers = {p: fi for p, fi in self.n.walk(self.index)}

    def verifier(self):
        for m, attendu in EXEFS.items():
            fi = self.fichiers.get(f"exefs/{m}")
            if fi is None:
                raise Echec(f"exefs/{m} absent de la copie")
            h = fi["sha256"].hex() if isinstance(fi["sha256"], (bytes, bytearray)) else str(fi["sha256"])
            if h != attendu:
                raise Echec(f"exefs/{m} ne correspond pas à la version 2.0.0 (copie modifiée ou autre version)")
        dz = self.fichiers.get("romfs/data.zip")
        if dz is None or dz["compressed"]:
            raise Echec("romfs/data.zip absent de la copie, ou dans un format non prévu")

    def signature(self):
        return EXEFS["main"][:16] + ":" + os.path.basename(self.chemin)


def verifier_pc(construire):
    """Ce qui ferait échouer la construction ou le jeu chez un joueur, signalé clairement dès le début."""
    if os.name == "nt":
        import ctypes
        if not ctypes.windll.kernel32.IsProcessorFeaturePresent(40):   # PF_AVX2_INSTRUCTIONS_AVAILABLE
            raise Echec("ce processeur n'a pas les instructions AVX2 : le jeu recompilé ne peut pas y tourner")
        if not os.path.exists(os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32", "vulkan-1.dll")):
            dire("attention : Vulkan introuvable (vulkan-1.dll) : installez le pilote de votre carte graphique")
        try:   # GCC passe par les API ANSI : un chemin hors de la page de code de Windows le ferait échouer
            RACINE.encode("mbcs", errors="strict")
        except UnicodeEncodeError:
            raise Echec("le chemin du dossier contient des caractères que le compilateur ne gère pas : "
                        "déplacez le dossier du jeu (par exemple dans C:\\Jeux\\StarFox)")
    if construire:
        libre = shutil.disk_usage(RACINE).free
        if libre < 6e9:
            raise Echec(f"espace disque insuffisant : {libre / 1e9:.1f} Go libres, il en faut environ 6")


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
            for p in sorted(glob.glob(os.path.join(RACINE, *m.split("/")))):
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
            raise Echec(f"exefs/{m} : empreinte différente après extraction (copie abîmée ?)")
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
        m = re.match(r"(\w+): (\d+)/(\d+) lots compilés", l)
        if m and m.group(1) in fait:
            fait[m.group(1)] = int(m.group(2)) / max(1, int(m.group(3)))
        m2 = re.match(r"(\w+): (tour \d+|\d+ fonctions)", l)
        if m2 and m2.group(1) in fait and actuel["m"] != m2.group(1):
            actuel["m"] = m2.group(1)
            dire(f"@info {m2.group(1)}")
        m3 = re.match(r"(\w+): terminé", l)
        if m3 and m3.group(1) in fait:
            fait[m3.group(1)] = 1.0
        dire(f"@progres {sum(fait[k] * POIDS[k] for k in fait) / total:.3f}")

    ordre = ["sdk", "subsdk0", "subsdk1", "subsdk2", "subsdk3", "main"]
    lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "fullbuild.py"), *ordre, "-j", str(a.fils)], RECOMP,
           analyse)


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
        dire("icône du jeu absente des métadonnées de la copie : exécutable sans icône")
        return
    os.makedirs(FULL, exist_ok=True)
    chemin_jpg = os.path.join(FULL, "icone.jpg")
    with open(chemin_jpg, "wb") as f:
        f.write(jpg)
    try:
        jpeg_vers_ico(chemin_jpg, ico)
        dire(f"icône du jeu : {ico} ({os.path.getsize(ico)} octets)")
    except Exception as e:  # noqa: BLE001   (sans icône, le jeu fonctionne pareil)
        dire(f"icône non convertie : {e}")
    finally:
        os.remove(chemin_jpg)


def e_lier(copie, a):
    dire("@progres 0.1")
    extraire_icone(copie)
    dire("@progres 0.2")
    lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "link.py")], RECOMP)
    if not os.path.exists(os.path.join(FULL, "recomp_main.exe")):
        raise Echec("l'exécutable n'a pas été produit")


def compter(dossier, motif):
    try:
        return len(glob.glob(os.path.join(dossier, motif)))
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
    if compter(cache, "*.spv") < 2000:
        raise Echec("trop peu de shaders traduits")


def e_extraits(copie, a):
    exe = os.path.join(FULL, "recomp_main.exe")
    save = os.path.join(FULL, "essai_extraits")
    shutil.rmtree(save, ignore_errors=True)
    os.makedirs(save, exist_ok=True)
    os.makedirs(EXTRAITS, exist_ok=True)
    for f in glob.glob(os.path.join(EXTRAITS, "extrait_*.mp4*")):
        os.remove(f)
    env = outils_env()
    env.update({"RECOMP_SAVE_DIR": save, "RECOMP_AUTOPRESS": "1", "RECOMP_WINDOW": "windowed", "RECOMP_VOLUME": "0",
                "RECOMP_RECORD_DIR": EXTRAITS, "RECOMP_RECORD_EXIT": "1", "RECOMP_RECORD_CLIPS": "6"})
    dire("@info jeu")
    p = subprocess.Popen([exe, "--log", os.path.join(FULL, "extraits.log")], cwd=FULL, env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0, limite = time.time(), 420
    while p.poll() is None and time.time() - t0 < limite:
        n = compter(EXTRAITS, "extrait_*.mp4")
        dire(f"@progres {min(0.99, max(n / 6, (time.time() - t0) / limite)):.3f}")
        time.sleep(1.0)
    if p.poll() is None:
        p.kill()
    shutil.rmtree(save, ignore_errors=True)
    n = compter(EXTRAITS, "extrait_*.mp4")
    if n == 0:
        raise Echec("aucun extrait enregistré (voir build/full/extraits.log)")
    dire(f"@info {n}")
    # le jeu crée au démarrage des shaders absents de la romfs (vidés dans shaders/ pendant cette partie) :
    # on les traduit aussi, pour une installation identique à une installation faite en jouant
    lancer([sys.executable, "-X", "utf8", os.path.join(RECOMP, "shaders", "shaderc.py"), "--nouveaux", "-j", str(a.fils)],
           RECOMP)


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
            raise Echec("précompilation des shaders : " + l[8:])

    lancer([exe, "--precompile"], FULL, analyse)


FONCTIONS = {"extraire": e_extraire, "images": e_images, "recompiler": e_recompiler, "lier": e_lier,
             "shaders": e_shaders, "pipelines": e_pipelines, "extraits": e_extraits}


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--copie", required=True)
    ap.add_argument("--fils", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--etapes", default=",".join(ETAPES))
    ap.add_argument("--forcer", action="store_true")
    ap.add_argument("--sans-extraits", action="store_true")
    a = ap.parse_args()
    etapes = [e for e in a.etapes.split(",") if e in ETAPES]
    if a.sans_extraits and "extraits" in etapes:
        etapes.remove("extraits")
    dire("@etapes " + ",".join(etapes))
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
            if (not a.forcer and e not in TOUJOURS and deja_fait(e, sig[e])
                    and not (e == "extraits" and compter(EXTRAITS, "extrait_*.mp4") == 0)):
                dire(f"@saute {e}")
                continue
            t = time.time()
            FONCTIONS[e](copie, a)
            if e not in TOUJOURS:
                noter(e, sig[e])
            dire(f"@ok {e}")
            dire(f"{e} : {time.time() - t:.0f} s")
        if set(etapes) >= set(ETAPES) - {"extraits"}:   # construction complète : jeu à jour pour cette version
            noter("version", version_sources())
    except Echec as ex:
        dire(f"@erreur {ex}")
        return 1
    except Exception as ex:  # noqa: BLE001
        dire(f"@erreur {type(ex).__name__} : {ex}")
        return 1
    dire(f"@fini {time.time() - t0:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
