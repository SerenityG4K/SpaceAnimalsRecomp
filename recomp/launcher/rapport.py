#!/usr/bin/env python3
"""Rapport de bug : un seul fichier à joindre à une issue GitHub (bouton « RAPPORT DE BUG » du launcher).

  python rapport.py [--sortie <fichier.zip>]

Crée <racine>/SpaceAnimalsRecomp-bug-report.zip :
  report.txt   version, Windows, processeur, mémoire, GPU et pilote, disque, dossier d'installation, état de la
               construction, réglages, liste des journaux joints
  logs/…       journaux de la construction, des shaders, du jeu, de la mise à jour et du launcher

Rien du jeu n'y entre (ni la copie, ni le code généré, ni les shaders, ni la sauvegarde) : seulement des journaux texte.
Les chemins sont anonymisés : le dossier d'installation devient <install>, le profil Windows %USERPROFILE%, le nom
d'utilisateur <user> et le nom du PC <pc>. Le joueur peut ouvrir le zip et relire le texte avant de l'envoyer.
"""
import argparse
import ctypes
import datetime
import glob
import os
import platform
import re
import shutil
import sys
import zipfile

LANCEUR = os.path.dirname(os.path.abspath(__file__))
RECOMP = os.path.dirname(LANCEUR)
RACINE = os.path.dirname(RECOMP)
FULL = os.path.join(RECOMP, "build", "full")
NOM = "SpaceAnimalsRecomp-bug-report.zip"
TAILLE_MAX = 6 << 20          # par journal : au-delà, le début et la fin sont gardés

# (chemin, nom dans le zip) : du plus utile au moins utile
JOURNAUX = [
    (os.path.join(FULL, "recomp_run.log"), "logs/recomp_run.log"),                    # dernière partie
    (os.path.join(FULL, "recomp_run_precedent.log"), "logs/recomp_run_previous.log"),  # partie d'avant
    (os.path.join(FULL, "preparation.log"), "logs/preparation.log"),                  # construction
    (os.path.join(FULL, "precompile.log"), "logs/precompile.log"),                    # shaders pour ce GPU
    (os.path.join(FULL, "precompile_controle.log"), "logs/precompile_controle.log"),
    (os.path.join(FULL, "precompile_isolement.log"), "logs/precompile_isolement.log"),
    (os.path.join(FULL, "shaders_exclus.txt"), "logs/shaders_exclus.txt"),
    (os.path.join(FULL, "releve_shaders.log"), "logs/releve_shaders.log"),
    (os.path.join(FULL, "extraits.log"), "logs/extraits.log"),
    (os.path.join(RACINE, "mise_a_jour.log"), "logs/mise_a_jour.log"),
    (os.path.join(RACINE, "lanceur.log"), "logs/lanceur.log"),
    (os.path.join(FULL, "recomp.ini"), "settings/recomp.ini"),
    (os.path.join(RACINE, "lanceur.ini"), "settings/lanceur.ini"),
]


# ---------------------------------------------------------------- anonymisation
def anonymiseur():
    remplacements = []

    def ajouter(chemin, par):
        if not chemin or len(chemin) < 4:
            return
        for c in {chemin, chemin.replace("\\", "/"), chemin.replace("/", "\\")}:
            remplacements.append((re.compile(re.escape(c), re.IGNORECASE), par))

    ajouter(os.path.normpath(RACINE), "<install>")
    ajouter(os.environ.get("USERPROFILE", ""), "%USERPROFILE%")
    profil = re.compile(r"([A-Za-z]:[\\/]+Users[\\/]+)(?!<user>)([^\\/:*?\"<>|\r\n]+)", re.IGNORECASE)
    noms = []
    for var, par in (("USERNAME", "<user>"), ("COMPUTERNAME", "<pc>"), ("USERDOMAIN", "<pc>")):
        v = os.environ.get(var, "")
        if len(v) >= 3:
            noms.append((re.compile(r"(?<![A-Za-z0-9])" + re.escape(v) + r"(?![A-Za-z0-9])", re.IGNORECASE), par))

    def anonymiser(texte):
        for motif, par in remplacements:
            texte = motif.sub(lambda m, p=par: p, texte)
        texte = profil.sub(lambda m: m.group(1) + "<user>", texte)
        for motif, par in noms:
            texte = motif.sub(par, texte)
        return texte

    return anonymiser


# ---------------------------------------------------------------- système
def registre(cle, valeur, racine=None):
    try:
        import winreg
        with winreg.OpenKey(racine or winreg.HKEY_LOCAL_MACHINE, cle) as k:
            return winreg.QueryValueEx(k, valeur)[0]
    except OSError:
        return None


def windows():
    cle = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion"
    nom = registre(cle, "ProductName") or platform.platform()
    build = registre(cle, "CurrentBuild") or ""
    ubr = registre(cle, "UBR")
    if build.isdigit() and int(build) >= 22000:
        nom = nom.replace("Windows 10", "Windows 11")
    return f"{nom} {registre(cle, 'DisplayVersion') or ''} (build {build}{'.' + str(ubr) if ubr is not None else ''})"


class Memoire(ctypes.Structure):
    _fields_ = [("dwLength", ctypes.c_ulong), ("dwMemoryLoad", ctypes.c_ulong), ("ullTotalPhys", ctypes.c_ulonglong),
                ("ullAvailPhys", ctypes.c_ulonglong), ("ullTotalPageFile", ctypes.c_ulonglong),
                ("ullAvailPageFile", ctypes.c_ulonglong), ("ullTotalVirtual", ctypes.c_ulonglong),
                ("ullAvailVirtual", ctypes.c_ulonglong), ("ullAvailExtendedVirtual", ctypes.c_ulonglong)]


def memoire():
    m = Memoire()
    m.dwLength = ctypes.sizeof(Memoire)
    if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
        return "?"
    go = 1 << 30
    return (f"RAM {m.ullTotalPhys / go:.1f} GB ({m.ullAvailPhys / go:.1f} GB free); commit limit (RAM + page file) "
            f"{m.ullTotalPageFile / go:.1f} GB ({m.ullAvailPageFile / go:.1f} GB free)")


def processeur():
    nom = (registre(r"HARDWARE\DESCRIPTION\System\CentralProcessor\0", "ProcessorNameString") or platform.processor()).strip()
    avx2 = bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))   # PF_AVX2_INSTRUCTIONS_AVAILABLE
    return f"{nom}, {os.cpu_count()} threads, AVX2 {'yes' if avx2 else 'NO (required)'}"


def cartes_presentes():
    """Noms des cartes graphiques que Windows voit en ce moment (EnumDisplayDevices), en minuscules ; ensemble vide si
    l'appel échoue. Le registre garde aussi l'entrée du pilote d'une carte retirée du PC."""
    import ctypes
    import ctypes.wintypes as wt

    class DD(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("DeviceName", wt.WCHAR * 32), ("DeviceString", wt.WCHAR * 128),
                    ("StateFlags", wt.DWORD), ("DeviceID", wt.WCHAR * 128), ("DeviceKey", wt.WCHAR * 128)]
    noms = set()
    try:
        i = 0
        while i < 64:
            d = DD()
            d.cb = ctypes.sizeof(DD)
            if not ctypes.windll.user32.EnumDisplayDevicesW(None, i, ctypes.byref(d), 0):
                break
            noms.add(d.DeviceString.strip().lower())
            i += 1
    except Exception:  # noqa: BLE001 - le rapport ne doit jamais échouer pour ça
        return set()
    return noms


def gpus():
    """Cartes graphiques d'après le registre (classe Display) : nom, version du pilote, mémoire vidéo."""
    import winreg
    out = []
    presentes = cartes_presentes()
    base = r"SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}"
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, base) as k:
            i = 0
            while True:
                try:
                    sous = winreg.EnumKey(k, i)
                except OSError:
                    break
                i += 1
                if not sous.isdigit():
                    continue
                nom = registre(base + "\\" + sous, "DriverDesc")
                if not nom:
                    continue
                ver = registre(base + "\\" + sous, "DriverVersion") or "?"
                vram = registre(base + "\\" + sous, "HardwareInformation.qwMemorySize")
                ligne = f"{nom}, driver {ver}"
                chiffres = ver.replace(".", "")
                if "nvidia" in nom.lower() and len(chiffres) >= 5:   # 32.0.15.6201 → 562.01
                    ligne += f" (NVIDIA {chiffres[-5:-2]}.{chiffres[-2:]})"
                if isinstance(vram, int) and vram > 0:
                    ligne += f", {vram / (1 << 30):.1f} GB VRAM"
                if presentes and nom.strip().lower() not in presentes:
                    ligne += ", not present (leftover driver entry)"
                out.append(ligne)
    except OSError:
        pass
    return out or ["?"]


def lignes_journal(chemin, motifs, n=3):
    """Premières lignes d'un journal qui contiennent un des motifs (GPU vu par Vulkan, etc.)."""
    if not os.path.exists(chemin):
        return []
    out = []
    with open(chemin, encoding="utf-8", errors="replace") as f:
        for ligne in f:
            if any(m in ligne for m in motifs):
                out.append(ligne.rstrip())
                if len(out) >= n:
                    break
    return out


def dossier_installation():
    r = os.path.normpath(RACINE)
    notes = []
    if " " in r:
        notes.append("path has spaces")
    if any(ord(c) > 127 for c in r):
        notes.append("path has non-ASCII characters")
    if "program files" in r.lower():
        notes.append("under Program Files")
    if "onedrive" in r.lower():
        notes.append("inside OneDrive")
    try:
        u = shutil.disk_usage(r)
        notes.append(f"{u.free / (1 << 30):.1f} GB free on the drive")
    except OSError:
        pass
    return "<install> (" + ", ".join(notes) + ")"


def etat_construction():
    out = []
    exe = os.path.join(FULL, "recomp_main.exe")
    if os.path.exists(exe):
        st = os.stat(exe)
        out.append(f"game built: recomp_main.exe {st.st_size / 1e6:.0f} MB, "
                   f"{datetime.datetime.fromtimestamp(st.st_mtime):%Y-%m-%d %H:%M}")
    else:
        out.append("game built: no (recomp_main.exe missing)")
    marques = os.path.join(FULL, ".preparation")
    if os.path.isdir(marques):
        faites = []
        for f in sorted(os.listdir(marques)):
            p = os.path.join(marques, f)
            faites.append(f"{f} {datetime.datetime.fromtimestamp(os.path.getmtime(p)):%Y-%m-%d %H:%M}")
        out.append("steps done: " + (", ".join(faites) or "none"))
    spv = len(glob.glob(os.path.join(glob.escape(FULL), "shader_cache", "*.spv")))
    out.append(f"translated shader stages: {spv}")
    out.append(f"pipeline cache: {'yes' if os.path.exists(os.path.join(FULL, 'pipelines.cache')) else 'no'}")
    for m in ("main", "sdk"):
        d = os.path.join(FULL, m)
        if os.path.isdir(d):
            out.append(f"{m} objects: {len(glob.glob(os.path.join(glob.escape(d), '*.o')))}")
    return out


def outils():
    fourni = os.path.normcase(sys.executable).startswith(os.path.normcase(os.path.join(RACINE, "outils")))
    out = [f"Python {platform.python_version()} ({'bundled' if fourni else 'system'})"]
    for d in (os.path.join(RACINE, "outils"), os.path.join(RECOMP, "outils")):
        lis = os.path.join(d, "LISEZMOI.txt")
        if os.path.exists(lis):
            with open(lis, encoding="utf-8", errors="replace") as f:
                out += [l.strip() for l in f if re.match(r"^\S.* \d+\.\d", l) and not l.startswith(("Outils", "Licences"))][:6]
            break
    return out


def version():
    try:
        return open(os.path.join(RECOMP, "VERSION"), encoding="utf-8").read().strip()
    except OSError:
        return "?"


def ligne_systeme():
    """Résumé d'une ligne (en tête des journaux de construction) : version, Windows, processeur, mémoire, GPU."""
    try:
        g = "; ".join(x.split(", driver")[0] + (" (" + x.split("(NVIDIA ")[1] if "(NVIDIA " in x else "")
                      for x in gpus() if "Microsoft" not in x and "Remote" not in x and "not present" not in x)
        return f"SpaceAnimalsRecomp {version()}; {windows()}; {processeur()}; {memoire()}; GPU {g}"
    except Exception as e:  # noqa: BLE001 - un en-tête ne doit jamais faire échouer la construction
        return f"SpaceAnimalsRecomp {version()} (system: {type(e).__name__})"


def lire(chemin):
    taille = os.path.getsize(chemin)
    with open(chemin, "rb") as f:
        if taille <= TAILLE_MAX:
            data = f.read()
        else:   # début (démarrage, configuration) et fin (ce qui s'est passé en dernier)
            tete = f.read(TAILLE_MAX // 4)
            f.seek(taille - TAILLE_MAX * 3 // 4)
            data = tete + f"\n\n[... {taille - TAILLE_MAX} bytes cut by the bug report ...]\n\n".encode() + f.read()
    return data.decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sortie", default=os.path.join(RACINE, NOM))
    a = ap.parse_args()
    anon = anonymiseur()
    maintenant = datetime.datetime.now()
    joints = [(c, n) for c, n in JOURNAUX if os.path.exists(c)]
    run = os.path.join(FULL, "recomp_run.log")
    r = [f"SpaceAnimalsRecomp bug report, {maintenant:%Y-%m-%d %H:%M}", "",
         f"Version: {version()}",
         f"Windows: {windows()}",
         f"CPU: {processeur()}",
         f"Memory: {memoire()}"]
    r += [f"GPU: {x}" for x in gpus()]
    r += [f"Vulkan (last game): {x.split('] ', 2)[-1]}" for x in lignes_journal(run, ("[vk    ] GPU:", "[vk    ] display ", "[vk    ] pipelines:"))]
    r += [f"Install folder: {dossier_installation()}", ""]
    r += ["Build:"] + [f"  {x}" for x in etat_construction()]
    r += ["Tools:"] + [f"  {x}" for x in outils()]
    r += ["", "Attached files:"]
    for c, n in joints:
        st = os.stat(c)
        r.append(f"  {n}  ({st.st_size / 1024:.0f} KB, {datetime.datetime.fromtimestamp(st.st_mtime):%Y-%m-%d %H:%M})")
    absents = [n for c, n in JOURNAUX if not os.path.exists(c)]
    if absents:
        r.append("Not found: " + ", ".join(os.path.basename(n) for n in absents))
    texte = anon("\n".join(r) + "\n")
    tmp = a.sortie + ".tmp"
    with zipfile.ZipFile(tmp, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("report.txt", texte)
        for c, n in joints:
            try:
                z.writestr(n, anon(lire(c)))
            except OSError as e:
                z.writestr(n + ".error.txt", f"{type(e).__name__}: {e}")
    os.replace(tmp, a.sortie)
    print(texte)
    print(f"@rapport {a.sortie}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
