#!/usr/bin/env python3
"""Mises à jour depuis les releases GitHub du projet. Lancé par le launcher, avec le Python fourni.

  python maj.py --verifier [--racine R]          dernière release plus récente ? « @maj <version> » ou « @ajour <version> »
  python maj.py --installer [--racine R]         télécharge la mise à jour, vérifie son empreinte, prépare ses fichiers
  python maj.py --finir --pid P [--racine R]     après la fermeture du launcher (processus P) : installe, puis le relance

La mise à jour (SpaceAnimalsRecomp-<version>-update.zip) contient le launcher, les sources et tools/, sans les outils
(Python, GCC, glslang) : si la nouvelle version en demande d'autres, son outils.py les télécharge depuis leurs sources
officielles. Rien de ce que le joueur a produit n'est touché : copie du jeu, jeu construit (recomp/build), extraits,
sauvegardes et réglages. Le launcher refait ensuite seulement les étapes de construction dont le code a changé.
"""
import argparse
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

DEPOT = "SerenityG4K/SpaceAnimalsRecomp"
API = f"https://api.github.com/repos/{DEPOT}/releases/latest"
TELECHARGEMENTS = f"https://github.com/{DEPOT}/releases/download/"
AGENT = "SpaceAnimalsRecomp-launcher"
SUFFIXE = "-update.zip"
RACINE_DEFAUT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def dire(ligne):
    print(ligne, flush=True)


class Echec(Exception):
    pass


def version_locale(racine):
    try:
        return open(os.path.join(racine, "recomp", "VERSION"), encoding="utf-8").read().strip()
    except OSError:
        return "0"


def numeros(v):
    """« v1.2.3 » → (1, 2, 3) ; ce qui n'est pas un nombre compte pour 0."""
    out = []
    for x in v.strip().lstrip("vV").split("-")[0].split("."):
        out.append(int(x) if x.isdigit() else 0)
    return tuple(out + [0] * (3 - len(out)))


def lire_json(url, delai):
    req = urllib.request.Request(url, headers={"User-Agent": AGENT, "Accept": "application/vnd.github+json"})
    with urllib.request.urlopen(req, timeout=delai) as r:
        return json.loads(r.read().decode("utf-8"))


def derniere_release(delai=15):
    rel = lire_json(API, delai)
    if not isinstance(rel, dict) or "tag_name" not in rel:
        raise Echec("réponse inattendue de GitHub")
    return rel


def asset_maj(rel):
    for a in rel.get("assets", []):
        nom = a.get("name", "")
        url = a.get("browser_download_url", "")
        if nom.endswith(SUFFIXE) and url.startswith(TELECHARGEMENTS):
            return a
    return None


def empreinte_attendue(rel, a):
    """SHA-256 de l'archive : calculée par GitHub (champ digest), sinon donnée par SHA256SUMS.txt de la release."""
    d = a.get("digest") or ""
    if d.startswith("sha256:") and len(d) == 71:
        return d[7:].lower()
    for b in rel.get("assets", []):
        if b.get("name") == "SHA256SUMS.txt" and b.get("browser_download_url", "").startswith(TELECHARGEMENTS):
            req = urllib.request.Request(b["browser_download_url"], headers={"User-Agent": AGENT})
            with urllib.request.urlopen(req, timeout=30) as r:
                for ligne in r.read().decode("utf-8", "replace").splitlines():
                    parts = ligne.split()
                    if len(parts) == 2 and parts[1].lstrip("*") == a["name"]:
                        return parts[0].lower()
    raise Echec("empreinte SHA-256 de la mise à jour introuvable : téléchargement refusé")


def telecharger(url, chemin, debut, fin):
    req = urllib.request.Request(url, headers={"User-Agent": AGENT})
    h = hashlib.sha256()
    with urllib.request.urlopen(req, timeout=60) as r, open(chemin + ".part", "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        lu, dernier = 0, -1
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            f.write(b)
            h.update(b)
            lu += len(b)
            pm = lu * 1000 // total if total else 0
            if pm != dernier:
                dernier = pm
                dire(f"@progres {debut + (fin - debut) * pm / 1000:.3f}")
    os.replace(chemin + ".part", chemin)
    return h.hexdigest()


def extraire(archive, dest):
    """Extraction protégée (aucun chemin absolu ni « .. »), sans le dossier de tête de l'archive."""
    dest = os.path.abspath(dest)
    with zipfile.ZipFile(archive) as z:
        for m in z.infolist():
            nom = m.filename.replace("\\", "/")
            nom = nom.split("/", 1)[1] if "/" in nom else ""
            if not nom or nom.endswith("/"):
                continue
            cible = os.path.abspath(os.path.join(dest, nom))
            if not cible.startswith(dest + os.sep) or os.path.isabs(nom) or ".." in nom.split("/"):
                raise Echec(f"chemin refusé dans la mise à jour : {m.filename}")
            os.makedirs(os.path.dirname(cible), exist_ok=True)
            with z.open(m) as s, open(cible, "wb") as d:
                shutil.copyfileobj(s, d, 1 << 20)


def outils_manquants(racine, nouvelle):
    """Outils que demande la nouvelle version et qui ne sont pas installés (marques outils/.installe/<nom>)."""
    script = os.path.join(nouvelle, "recomp", "launcher", "outils.py")
    if not os.path.exists(script):
        return script, []
    ns = {"__file__": script, "__name__": "outils_maj"}
    exec(compile(open(script, encoding="utf-8").read(), script, "exec"), ns)   # noqa: S102 (code de la release vérifiée)
    manque = []
    for o in ns["OUTILS"]:
        marque = os.path.join(racine, "outils", ".installe", o["nom"])
        try:
            ok = open(marque, encoding="utf-8").read().strip() == o["sha256"]
        except OSError:
            ok = False
        if not ok:
            manque.append(o["nom"])
    return script, manque


# ---------------------------------------------------------------------------------------------- commandes
def verifier(racine):
    locale = version_locale(racine)
    rel = derniere_release(delai=8)
    tag = rel["tag_name"]
    if numeros(tag) > numeros(locale) and asset_maj(rel):
        dire(f"@maj {tag.lstrip('vV')}")
    else:
        dire(f"@ajour {tag.lstrip('vV')}")


def installer(racine):
    locale = version_locale(racine)
    dire("@info GitHub")
    rel = derniere_release()
    tag = rel["tag_name"].lstrip("vV")
    if numeros(tag) <= numeros(locale):
        raise Echec(f"déjà à jour ({locale})")
    a = asset_maj(rel)
    if not a:
        raise Echec(f"la release {tag} n'a pas d'archive de mise à jour")
    attendu = empreinte_attendue(rel, a)
    dossier = os.path.join(racine, "maj")
    shutil.rmtree(dossier, ignore_errors=True)
    os.makedirs(dossier)
    archive = os.path.join(dossier, a["name"])
    dire(f"@info {a['name']} ({a.get('size', 0) / 1e6:.1f} Mo)")
    h = telecharger(a["browser_download_url"], archive, 0.0, 0.8)
    if h != attendu:
        os.remove(archive)
        raise Echec(f"empreinte SHA-256 différente ({h}, attendu {attendu}) : archive supprimée")
    dire("@info empreinte SHA-256 vérifiée")
    nouvelle = os.path.join(dossier, "nouvelle")
    extraire(archive, nouvelle)
    os.remove(archive)
    if not os.path.exists(os.path.join(nouvelle, "lanceur.exe")) or not os.path.exists(os.path.join(nouvelle, "recomp", "VERSION")):
        raise Echec("archive de mise à jour incomplète")
    dire("@progres 0.9")
    script, manque = outils_manquants(racine, nouvelle)
    if "python" in manque:   # le Python en cours d'utilisation ne peut pas être remplacé ici
        raise Echec("cette version demande un nouveau Python : téléchargez le paquet complet de la release")
    if manque:
        dire("@info outils : " + ", ".join(manque))
        r = subprocess.run([sys.executable, "-X", "utf8", script, "--dest", os.path.join(racine, "outils")],
                           capture_output=True, text=True, encoding="utf-8", errors="replace")
        if r.returncode:
            raise Echec("outils de la nouvelle version : " + (r.stdout + r.stderr).strip()[-300:])
    with open(os.path.join(dossier, "pret.txt"), "w", encoding="utf-8") as f:
        f.write(tag + "\n")
    dire("@progres 1.000")
    dire(f"@info {tag} prête")


def message(texte):
    try:
        ctypes.windll.user32.MessageBoxW(None, texte, "StarFox Remake [recompiled]", 0x30)
    except Exception:  # noqa: BLE001
        pass


def finir(racine, pid):
    journal = open(os.path.join(racine, "mise_a_jour.log"), "a", encoding="utf-8")

    def note(t):
        journal.write(time.strftime("%Y-%m-%d %H:%M:%S ") + t + "\n")
        journal.flush()

    nouvelle = os.path.join(racine, "maj", "nouvelle")
    if not os.path.exists(os.path.join(racine, "maj", "pret.txt")):
        note("rien à installer")
    else:
        # attendre la fermeture du launcher (lanceur.exe ne peut être remplacé qu'ensuite)
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(0x00100000, False, pid)   # SYNCHRONIZE
        if h:
            k32.WaitForSingleObject(h, 60000)
            k32.CloseHandle(h)
        erreurs = []
        for base, _, fichiers in os.walk(nouvelle):
            for f in fichiers:
                src = os.path.join(base, f)
                dst = os.path.join(racine, os.path.relpath(src, nouvelle))
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                for essai in range(20):   # un antivirus peut encore tenir lanceur.exe quelques instants
                    try:
                        shutil.copy2(src, dst)
                        break
                    except OSError as e:
                        if essai == 19:
                            erreurs.append(f"{dst} : {e}")
                        time.sleep(0.5)
        if erreurs:
            note("échec : " + " ; ".join(erreurs[:5]))
            message("La mise à jour n'a pas pu remplacer tous les fichiers (voir mise_a_jour.log).\n"
                    "The update could not replace every file (see mise_a_jour.log).")
            return 1
        note("installée : " + version_locale(racine))
        shutil.rmtree(os.path.join(racine, "maj"), ignore_errors=True)
    lanceur = os.path.join(racine, "lanceur.exe")
    subprocess.Popen([lanceur], cwd=racine, creationflags=0x00000008 | 0x00000200)   # DETACHED_PROCESS, NEW_PROCESS_GROUP
    return 0


def main():
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", line_buffering=True)
    ap = argparse.ArgumentParser()
    ap.add_argument("--racine", default=RACINE_DEFAUT)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--verifier", action="store_true")
    g.add_argument("--installer", action="store_true")
    g.add_argument("--finir", action="store_true")
    ap.add_argument("--pid", type=int, default=0)
    a = ap.parse_args()
    racine = os.path.abspath(a.racine)
    try:
        if a.verifier:
            verifier(racine)
        elif a.installer:
            dire("@etapes maj")
            dire("@etape maj")
            installer(racine)
            dire("@ok maj")
            dire("@fini")
        else:
            return finir(racine, a.pid)
    except Echec as e:
        dire(f"@erreur {e}")
        return 1
    except Exception as e:  # noqa: BLE001  (réseau, disque…)
        dire(f"@erreur {type(e).__name__} : {e}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
